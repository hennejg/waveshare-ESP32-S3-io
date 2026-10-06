#include "mb_tcp_master.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "app_mqtt.h"
#include "mb_pdu.h"
#include "scripting.h"

#define TAG "mb_master"

/* app_config.h cannot include mb_request.h -- it belongs to the board and
   must not depend on the application -- so the value types are written out
   twice. If they ever drift apart the build says so here. */
_Static_assert(MBM_VAL_U16 == MB_VAL_U16 && MBM_VAL_S16 == MB_VAL_S16 &&
               MBM_VAL_U32 == MB_VAL_U32 && MBM_VAL_S32 == MB_VAL_S32 &&
               MBM_VAL_F32 == MB_VAL_F32,
               "MBM_VAL_* and MB_VAL_* have drifted apart");

/* How long a device gets to accept a connection and to answer. Both are
   deadlines over the whole operation, not over one system call -- the socket
   timeout is a short slice underneath. */
#define CONNECT_MS      3000
#define RESPONSE_MS     2000
#define IO_SLICE_MS       50

/* After a failure an entry is not tried again immediately: a device that is
   switched off would otherwise be dialled at its poll interval for ever,
   which on a short interval is a connection attempt several times a second.
   The wait doubles up to a ceiling and is cleared by the first success. */
#define BACKOFF_FIRST_MS  2000
#define BACKOFF_MAX_MS   60000

/* Connections are kept open between polls -- a meter polled every second
   should not see a new connection every second. Three covers the usual case
   of one or two devices without holding sockets the web server needs. */
#define CONN_CACHE       3
#define CONN_IDLE_MS    60000

typedef struct {
    char     host[APP_CFG_MBM_HOST_LEN + 1];
    uint16_t port;
    int      fd;
    int64_t  used_ms;
} conn_t;

typedef struct {
    int64_t  due_ms;
    int64_t  value_ms;     /* when the last good value arrived, 0 = never */
    double   value;
    bool     valid;
    uint32_t reads;
    uint32_t errors;
    uint32_t backoff_ms;
    char     last_error[48];
} entry_state_t;

static conn_t        s_conns[CONN_CACHE];
static entry_state_t s_state[APP_CFG_MBM_COUNT];
static bool              s_running;
static uint16_t          s_tid;
/* s_state is written by the polling task and read by the HTTP task. The
   64-bit fields are two stores on this core, so without this a status request
   could show half of one value and half of the previous one. */
static SemaphoreHandle_t s_state_mux;

#define ST_LOCK()   do { if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY); } while (0)
#define ST_UNLOCK() do { if (s_state_mux) xSemaphoreGive(s_state_mux); } while (0)

/* Set by a configuration change, acted on by the polling task: the cached
   sockets belong to that task and must not be closed from under it. */
/* The table this task works from, and nobody else touches.
 *
 * Reading the shared configuration directly cannot be made safe by ordering
 * or by a generation counter, because the writer replaces the whole struct
 * with an unsynchronised memcpy and only afterwards says so. A poll that read
 * it mid-copy could take the new name with the old host and publish one
 * device's reading under another's name, and no later signal could take that
 * back.
 *
 * So the configuration is handed over instead of shared: the HTTP task leaves
 * a complete copy in s_staged, and this task picks it up between polls, where
 * nothing of its own is in flight. Everything that follows -- which entry is
 * polled, what its answer is committed to, what name it is published under --
 * then belongs to one task from start to finish. */
static mbm_poll_t    s_active[APP_CFG_MBM_COUNT];   /* this task only       */
static mbm_poll_t    s_staged[APP_CFG_MBM_COUNT];   /* under s_state_mux    */
static volatile bool s_reload_pending;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------ connections */

static void conn_close(conn_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->host[0] = '\0';
}

/* Non-blocking connect with a deadline. A blocking one would sit on lwIP's
   own retransmission schedule -- tens of seconds -- for a device that is
   simply not there, and the whole polling task with it. */
static int dial(const char *host, uint16_t port, char *err, size_t err_len)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai) {
        snprintf(err, err_len, "name not resolved");
        return -1;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(err, err_len, "no socket: errno %d", errno);
        freeaddrinfo(ai);
        return -1;
    }

    /* The whole pacing of io_all() rests on these: a blocking socket with a
       short timeout. If the flags could not be read, restoring them later
       would leave the socket non-blocking and io_all() would spin hot for its
       whole deadline instead of waiting. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        snprintf(err, err_len, "cannot set socket mode: errno %d", errno);
        close(fd);
        freeaddrinfo(ai);
        return -1;
    }

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);

    if (rc != 0) {
        if (errno != EINPROGRESS) {
            snprintf(err, err_len, "connect: errno %d", errno);
            close(fd);
            return -1;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(fd, &wr);
        struct timeval tv = { .tv_sec = CONNECT_MS / 1000,
                              .tv_usec = (CONNECT_MS % 1000) * 1000 };
        if (select(fd + 1, NULL, &wr, NULL, &tv) <= 0) {
            snprintf(err, err_len, "no answer in %d ms", CONNECT_MS);
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t l = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l) != 0) soerr = errno;
        if (soerr) {
            snprintf(err, err_len, "refused: errno %d", soerr);
            close(fd);
            return -1;
        }
    }

    int one = 1;
    struct timeval slice = { .tv_sec = 0, .tv_usec = IO_SLICE_MS * 1000 };
    if (fcntl(fd, F_SETFL, flags) < 0 ||          /* back to blocking */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &slice, sizeof(slice)) != 0) {
        snprintf(err, err_len, "cannot set socket timeouts: errno %d", errno);
        close(fd);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  /* a hint, not a must */
    return fd;
}

static conn_t *conn_get(const char *host, uint16_t port, bool *fresh,
                        char *err, size_t err_len)
{
    int64_t now = now_ms();
    *fresh = false;

    for (int i = 0; i < CONN_CACHE; i++) {
        if (s_conns[i].fd >= 0 && s_conns[i].port == port &&
            strcmp(s_conns[i].host, host) == 0) {
            s_conns[i].used_ms = now;
            return &s_conns[i];   /* kept from an earlier poll; may be stale */
        }
    }

    /* Take a free slot, or the one unused the longest. The new connection is
       dialled before the old one is dropped: giving up a working connection
       for one that then fails to open would cost two entries instead of one. */
    conn_t *pick = NULL;
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd < 0) { pick = &s_conns[i]; break; }
    if (!pick) {
        pick = &s_conns[0];
        for (int i = 1; i < CONN_CACHE; i++)
            if (s_conns[i].used_ms < pick->used_ms) pick = &s_conns[i];
    }

    int fd = dial(host, port, err, err_len);
    if (fd < 0) return NULL;
    if (pick->fd >= 0) conn_close(pick);

    strlcpy(pick->host, host, sizeof(pick->host));
    pick->port    = port;
    pick->fd      = fd;
    pick->used_ms = now;
    *fresh = true;
    ESP_LOGI(TAG, "connected to %s:%u", host, port);
    return pick;
}

static void conn_reap_idle(void)
{
    int64_t now = now_ms();
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd >= 0 && now - s_conns[i].used_ms > CONN_IDLE_MS) {
            ESP_LOGD(TAG, "closing idle connection to %s", s_conns[i].host);
            conn_close(&s_conns[i]);
        }
}

/* ------------------------------------------------------------------- i/o */

/* *closed distinguishes "the peer hung up" from "it said nothing in time".
   Both end the connection, but during commissioning they mean quite
   different things and the reason is shown to the user. */
static bool io_all(int fd, uint8_t *buf, size_t len, bool sending, bool *closed)
{
    int64_t deadline = now_ms() + RESPONSE_MS;
    size_t  done = 0;
    *closed = false;

    while (done < len) {
        if (now_ms() > deadline) return false;
        int n = sending ? send(fd, buf + done, len - done, 0)
                        : recv(fd, buf + done, len - done, 0);
        if (n > 0) { done += (size_t)n; continue; }
        if (n == 0) { *closed = true; return false; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        *closed = true;
        return false;
    }
    return true;
}

/* One read, start to finish. *fresh tells the caller whether the connection
   it used had just been opened. Returns true with *value set, or false with
   the reason in err. */
static bool read_once(const mbm_poll_t *e, uint16_t regs, double *value,
                      bool *fresh, char *err, size_t err_len)
{
    conn_t *c = conn_get(e->host, e->port, fresh, err, err_len);
    if (!c) return false;

    uint8_t  req[MB_MBAP_LEN + 5];
    uint16_t tid = ++s_tid;
    uint16_t n = mb_build_read_request(req, tid, e->unit_id, e->fc, e->reg, regs);

    bool closed = false;
    if (!io_all(c->fd, req, n, true, &closed)) {
        snprintf(err, err_len, closed ? "connection lost while sending"
                                      : "could not send in %d ms", RESPONSE_MS);
        conn_close(c);
        return false;
    }

    /* The header says how long the rest is, so it is read in two goes rather
       than guessed at. */
    uint8_t rsp[MB_MBAP_LEN + 2 + 8];
    if (!io_all(c->fd, rsp, 6, false, &closed)) {
        snprintf(err, err_len, closed ? "connection closed by the device"
                                      : "no answer in %d ms", RESPONSE_MS);
        conn_close(c);
        return false;
    }
    uint16_t rest = mb_be16(&rsp[4]);
    if (rest < 2 || rest > sizeof(rsp) - 6) {
        snprintf(err, err_len, "bad length field %u", rest);
        conn_close(c);                       /* the stream is out of step now */
        return false;
    }
    if (!io_all(c->fd, &rsp[6], rest, false, &closed)) {
        snprintf(err, err_len, "answer cut short");
        conn_close(c);
        return false;
    }

    const uint8_t *data = NULL;
    uint8_t exc = 0;
    mb_rsp_t r = mb_parse_read_response(rsp, (uint16_t)(6 + rest), tid, e->unit_id,
                                        e->fc, regs, &data, &exc);
    if (r == MB_RSP_EXCEPTION) {
        snprintf(err, err_len, "device says exception 0x%02X", exc);
        return false;                        /* the connection is still good */
    }
    if (r != MB_RSP_OK) {
        snprintf(err, err_len, "answer does not match the request");
        conn_close(c);
        return false;
    }

    double raw;
    if (!mb_value_decode(data, e->type, e->word_swap != 0, &raw)) {
        snprintf(err, err_len, "cannot decode type %u", e->type);
        return false;
    }
    double scaled = raw * (double)e->scale;
    /* A meter that reports "no reading" as 0xFFFFFFFF decodes to NaN, and
       0x7F800000 to infinity. Publishing either gives the rule engine a value
       every comparison is false against, and MQTT the literal text "nan". */
    if (!isfinite(scaled)) {
        snprintf(err, err_len, "device returned a value that is not a number");
        return false;
    }
    *value = scaled;
    return true;
}

/* A connection kept from an earlier poll may have been closed at the other
   end without us hearing: the send succeeds into a half-closed socket and the
   read comes back empty. Devices that drop idle connections after 10 to 30
   seconds are common, and this cache holds them for 60, so without a second
   attempt on a fresh connection such a device would fail every other poll for
   ever. A connection that was already fresh is not retried -- that failure is
   real. */
static bool read_value(const mbm_poll_t *e, double *value, char *err, size_t err_len)
{
    uint16_t regs = mb_value_regs(e->type);
    if (regs == 0) { snprintf(err, err_len, "unknown value type %u", e->type); return false; }

    bool fresh = false;
    if (read_once(e, regs, value, &fresh, err, err_len)) return true;
    if (fresh) return false;
    return read_once(e, regs, value, &fresh, err, err_len);
}

/* ------------------------------------------------------------- publishing */

static void publish(const mbm_poll_t *e, double value)
{
    char topic[8 + APP_CFG_MBM_NAME_LEN + 1];
    snprintf(topic, sizeof(topic), "modbus/%s", e->name);

    char payload[32];
    /* Six significant digits turn a 32-bit counter into 4.29497e+09, which
       reads back as 4294970000 -- the status API and the published value then
       disagree about the same reading. Ten covers u32 and s32 exactly without
       printing noise from below a float32's precision. */
    int len = snprintf(payload, sizeof(payload), "%.10g", value);
    if (len < 0) return;
    if (len >= (int)sizeof(payload)) len = (int)sizeof(payload) - 1;

    app_mqtt_publish(topic, payload, len, 0, false);

    /* The same topic goes straight to the rule engine rather than round the
       broker: a rule must work on a board with no broker configured, and the
       broker does not echo our own publishes back to us anyway. The string is
       the relative topic, which is what a rule writes. */
    scripting_on_mqtt_message(topic, strlen(topic), payload, (size_t)len);
}

/* ------------------------------------------------------------------ task */

/* e is a copy, not a pointer into the live configuration: the HTTP task
   overwrites that table in place, and a read in progress would otherwise
   finish against an entry that describes a different device. */
/* e points into s_active, which only this task writes and only between
   polls, so it cannot change under this function. The status commit still
   takes the lock -- the HTTP task reads the status -- but there is nothing
   left to compare against: a configuration change cannot be applied while
   this is running. */
static void poll_entry(const mbm_poll_t *e, entry_state_t *st)
{
    double value = 0;
    char   err[sizeof(st->last_error)];
    bool   ok = read_value(e, &value, err, sizeof(err));

    ST_LOCK();
    if (ok) {
        st->value      = value;
        st->value_ms   = now_ms();
        st->valid      = true;
        st->reads++;
        st->backoff_ms = 0;
        st->last_error[0] = '\0';
        ST_UNLOCK();
        publish(e, value);
        ESP_LOGD(TAG, "%s = %.10g", e->name, value);
    } else {
        st->errors++;
        st->backoff_ms = st->backoff_ms ? (st->backoff_ms * 2) : BACKOFF_FIRST_MS;
        if (st->backoff_ms > BACKOFF_MAX_MS) st->backoff_ms = BACKOFF_MAX_MS;
        bool first = strcmp(st->last_error, err) != 0;
        if (first) strlcpy(st->last_error, err, sizeof(st->last_error));
        ST_UNLOCK();
        if (first) {
            /* Only the first of a repeating failure is logged: a device that
               is switched off would otherwise fill the log at its poll rate. */
            ESP_LOGW(TAG, "%s: %s", e->name, err);
        }
    }
}

/* Picks up a configuration the HTTP task has left, at a point where this task
   holds nothing of its own: between polls, never during one. */
static void apply_staged_config(void)
{
    ST_LOCK();
    if (!s_reload_pending) { ST_UNLOCK(); return; }
    s_reload_pending = false;
    memcpy(s_active, s_staged, sizeof(s_active));

    /* Everything known about the old entries described something that is no
       longer there: a value from a register that has been repointed, a
       backoff earned by a host that has been corrected, counters belonging to
       another device. */
    memset(s_state, 0, sizeof(s_state));
    int64_t now = now_ms();
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) s_state[i].due_ms = now;
    ST_UNLOCK();

    /* A cached socket may lead to a device nobody asks about any more. */
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd >= 0) conn_close(&s_conns[i]);
}

static void master_task(void *arg)
{
    (void)arg;

    for (;;) {
        apply_staged_config();
        int64_t now  = now_ms();
        int64_t next = now + 1000;

        for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
            const mbm_poll_t *e  = &s_active[i];
            entry_state_t    *st = &s_state[i];
            if (!e->enable || !e->name[0] || !e->host[0]) continue;

            if (now >= st->due_ms) {
                poll_entry(e, st);
                now = now_ms();

                ST_LOCK();
                /* The backoff widens the gap after a failure; it must never
                   narrow it. An entry polled once a day that failed once
                   would otherwise be retried every minute from then on --
                   more traffic to a dead device than the working one ever
                   caused. */
                uint32_t wait = e->interval_ms;
                if (st->backoff_ms > wait)      wait = st->backoff_ms;
                if (wait < MBM_INTERVAL_MIN_MS) wait = MBM_INTERVAL_MIN_MS;
                st->due_ms = now + wait;
                ST_UNLOCK();
            }
            if (st->due_ms < next) next = st->due_ms;
        }

        conn_reap_idle();

        int64_t sleep_ms = next - now_ms();
        if (sleep_ms < 20)   sleep_ms = 20;     /* never spin */
        if (sleep_ms > 1000) sleep_ms = 1000;   /* notice a reload promptly */
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

/* ---------------------------------------------------------------- public */

/* Leaves a complete copy of the table for the polling task to pick up. Called
   on the HTTP task, directly after app_config_update() and therefore on the
   only task that writes the configuration -- so what is copied here is whole,
   which is exactly what the polling task cannot guarantee for itself. */
static void stage_config(void)
{
    const app_config_t *cfg = app_config_get();
    ST_LOCK();
    memcpy(s_staged, cfg->mbm, sizeof(s_staged));
    s_reload_pending = true;
    ST_UNLOCK();
}

esp_err_t mb_tcp_master_reload(void)
{
    stage_config();
    /* And if this is the first entry anyone has enabled, there is no task
       yet. Without this the entry would sit there, reported as enabled,
       until the next reboot. */
    return mb_tcp_master_start();
}

esp_err_t mb_tcp_master_start(void)
{
    if (s_running) return ESP_OK;

    if (!s_state_mux) {
        s_state_mux = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_state_mux, ESP_ERR_NO_MEM, TAG, "state mutex");
    }
    stage_config();           /* the task reads it on its first pass */

    int enabled = 0;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++)
        if (s_staged[i].enable && s_staged[i].name[0] && s_staged[i].host[0]) enabled++;
    if (!enabled) return ESP_OK;              /* nothing to do, no task */

    for (int i = 0; i < CONN_CACHE; i++) s_conns[i].fd = -1;

    ESP_RETURN_ON_FALSE(
        xTaskCreate(master_task, "mb_master", 5120, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "master task");

    s_running = true;
    ESP_LOGI(TAG, "reading %d value%s from other Modbus TCP devices",
             enabled, enabled == 1 ? "" : "s");
    return ESP_OK;
}

uint8_t mb_tcp_master_get_status(mbm_status_t *out, uint8_t count)
{
    uint8_t n = 0;

    ST_LOCK();
    for (int i = 0; i < APP_CFG_MBM_COUNT && n < count; i++, n++) {
        const entry_state_t *st = &s_state[i];
        /* From the task's own table, not the stored one: right after a save
           they differ for a moment, and reporting the new name beside the old
           entry's last value would be the one thing this is meant to tell
           apart. */
        out[n] = (mbm_status_t){
            .enabled = s_active[i].enable != 0,
            .valid   = st->valid,
            .value   = st->value,
            .age_ms  = st->value_ms ? (now_ms() - st->value_ms) : -1,
            .reads   = st->reads,
            .errors  = st->errors,
        };
        strlcpy(out[n].name,       s_active[i].name, sizeof(out[n].name));
        strlcpy(out[n].last_error, st->last_error,   sizeof(out[n].last_error));
    }
    ST_UNLOCK();
    return n;
}
