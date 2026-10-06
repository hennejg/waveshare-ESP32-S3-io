#include "mb_tcp_master.h"

#include <errno.h>
#include <fcntl.h>
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
static bool          s_running;
static uint16_t      s_tid;

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

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

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
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l);
        if (soerr) {
            snprintf(err, err_len, "refused: errno %d", soerr);
            close(fd);
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags);          /* back to blocking, with timeouts */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval slice = { .tv_sec = 0, .tv_usec = IO_SLICE_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &slice, sizeof(slice));
    return fd;
}

static conn_t *conn_get(const char *host, uint16_t port, char *err, size_t err_len)
{
    int64_t now = now_ms();

    for (int i = 0; i < CONN_CACHE; i++) {
        if (s_conns[i].fd >= 0 && s_conns[i].port == port &&
            strcmp(s_conns[i].host, host) == 0) {
            s_conns[i].used_ms = now;
            return &s_conns[i];
        }
    }

    /* Take a free slot, or the one unused the longest. */
    conn_t *pick = NULL;
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd < 0) { pick = &s_conns[i]; break; }
    if (!pick) {
        pick = &s_conns[0];
        for (int i = 1; i < CONN_CACHE; i++)
            if (s_conns[i].used_ms < pick->used_ms) pick = &s_conns[i];
        conn_close(pick);
    }

    int fd = dial(host, port, err, err_len);
    if (fd < 0) return NULL;

    strlcpy(pick->host, host, sizeof(pick->host));
    pick->port    = port;
    pick->fd      = fd;
    pick->used_ms = now;
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

static bool io_all(int fd, uint8_t *buf, size_t len, bool sending)
{
    int64_t deadline = now_ms() + RESPONSE_MS;
    size_t  done = 0;

    while (done < len) {
        if (now_ms() > deadline) return false;
        int n = sending ? send(fd, buf + done, len - done, 0)
                        : recv(fd, buf + done, len - done, 0);
        if (n > 0) { done += (size_t)n; continue; }
        if (n == 0) return false;                        /* the peer closed */
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return false;
    }
    return true;
}

/* One read, start to finish. Returns true with *value set, or false with the
   reason in err. */
static bool read_value(const mbm_poll_t *e, double *value, char *err, size_t err_len)
{
    uint16_t regs = mb_value_regs(e->type);
    if (regs == 0) { snprintf(err, err_len, "unknown value type %u", e->type); return false; }

    conn_t *c = conn_get(e->host, e->port, err, err_len);
    if (!c) return false;

    uint8_t  req[MB_MBAP_LEN + 5];
    uint16_t tid = ++s_tid;
    uint16_t n = mb_build_read_request(req, tid, e->unit_id, e->fc, e->reg, regs);

    if (!io_all(c->fd, req, n, true)) {
        snprintf(err, err_len, "send failed");
        conn_close(c);
        return false;
    }

    /* The header says how long the rest is, so it is read in two goes rather
       than guessed at. */
    uint8_t rsp[MB_MBAP_LEN + 2 + 8];
    if (!io_all(c->fd, rsp, 6, false)) {
        snprintf(err, err_len, "no answer in %d ms", RESPONSE_MS);
        conn_close(c);
        return false;
    }
    uint16_t rest = mb_be16(&rsp[4]);
    if (rest < 2 || rest > sizeof(rsp) - 6) {
        snprintf(err, err_len, "bad length field %u", rest);
        conn_close(c);                       /* the stream is out of step now */
        return false;
    }
    if (!io_all(c->fd, &rsp[6], rest, false)) {
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
    *value = raw * (double)e->scale;
    return true;
}

/* ------------------------------------------------------------- publishing */

static void publish(const mbm_poll_t *e, double value)
{
    char topic[8 + APP_CFG_MBM_NAME_LEN + 1];
    snprintf(topic, sizeof(topic), "modbus/%s", e->name);

    char payload[32];
    int len = snprintf(payload, sizeof(payload), "%.6g", value);
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

static void poll_entry(const mbm_poll_t *e, entry_state_t *st)
{
    double value = 0;
    char   err[sizeof(st->last_error)];

    if (read_value(e, &value, err, sizeof(err))) {
        st->value      = value;
        st->value_ms   = now_ms();
        st->valid      = true;
        st->reads++;
        st->backoff_ms = 0;
        st->last_error[0] = '\0';
        publish(e, value);
        ESP_LOGD(TAG, "%s = %.6g", e->name, value);
    } else {
        st->errors++;
        if (strcmp(st->last_error, err) != 0) {
            /* Only the first of a repeating failure is logged: a device that
               is switched off would otherwise fill the log at its poll rate. */
            ESP_LOGW(TAG, "%s: %s", e->name, err);
            strlcpy(st->last_error, err, sizeof(st->last_error));
        }
        st->backoff_ms = st->backoff_ms ? (st->backoff_ms * 2) : BACKOFF_FIRST_MS;
        if (st->backoff_ms > BACKOFF_MAX_MS) st->backoff_ms = BACKOFF_MAX_MS;
    }
}

static void master_task(void *arg)
{
    (void)arg;
    const app_config_t *cfg = app_config_get();

    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) s_state[i].due_ms = now_ms();

    for (;;) {
        int64_t now  = now_ms();
        int64_t next = now + 1000;

        for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
            const mbm_poll_t *e = &cfg->mbm[i];
            entry_state_t    *st = &s_state[i];
            if (!e->enable || !e->name[0] || !e->host[0]) continue;

            if (now >= st->due_ms) {
                poll_entry(e, st);
                now = now_ms();
                uint32_t wait = st->backoff_ms ? st->backoff_ms : e->interval_ms;
                if (wait < MBM_INTERVAL_MIN_MS) wait = MBM_INTERVAL_MIN_MS;
                st->due_ms = now + wait;
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

esp_err_t mb_tcp_master_start(void)
{
    if (s_running) return ESP_OK;

    const app_config_t *cfg = app_config_get();
    int enabled = 0;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++)
        if (cfg->mbm[i].enable && cfg->mbm[i].name[0] && cfg->mbm[i].host[0]) enabled++;
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
    const app_config_t *cfg = app_config_get();
    uint8_t n = 0;

    for (int i = 0; i < APP_CFG_MBM_COUNT && n < count; i++, n++) {
        const entry_state_t *st = &s_state[i];
        out[n] = (mbm_status_t){
            .enabled = cfg->mbm[i].enable != 0,
            .valid   = st->valid,
            .value   = st->value,
            .age_ms  = st->value_ms ? (now_ms() - st->value_ms) : -1,
            .reads   = st->reads,
            .errors  = st->errors,
        };
        strlcpy(out[n].last_error, st->last_error, sizeof(out[n].last_error));
    }
    return n;
}
