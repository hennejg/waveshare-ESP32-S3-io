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
#include <arpa/inet.h>
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
#include "mb_gateway.h"
#include "net_budget.h"
#include "scripting.h"

#define TAG "mb_master"

/* Why this is not the component's own TCP master.
 *
 * mbc_master_create_tcp() is given its whole node list at once, in
 * tcp_opts.ip_addr_table, and its parameter descriptors once more through
 * mbc_master_set_descriptor(). The public API has no way to change either
 * afterwards -- only create, start, stop, delete. These eight entries are
 * rows in a web form: correcting the address of one of them would mean
 * tearing down the master and rebuilding it, taking the other seven off the
 * air for as long as that takes, on every save.
 *
 * Reading registers over TCP is also the one part of Modbus that is nearly
 * all framing, and the framing is already here: mb_pdu.c builds the request
 * and checks the answer, with no dependency on the component or on the
 * board, which is why it can be and is tested on a development machine. What
 * is left below that is a socket and a clock. */

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
   should not see a new connection every second. The share is what the board
   can spare, not what the table could use: eight entries on eight distinct
   hosts will redial, which costs a connect per poll and nothing else. */
#define CONN_CACHE       NET_SOCK_MB_MASTER
#define CONN_IDLE_MS    60000

typedef struct {
    char     host[APP_CFG_MBM_HOST_LEN + 1];
    uint16_t port;
    int      fd;
    int64_t  used_ms;
} conn_t;

/* Names are resolved per host, not per entry: eight registers of one meter
   are eight entries and one name, and with DNS down each of them paying its
   own resolver timeout on the one polling task is the stall this exists to
   prevent.

   lwIP's resolver blocks the calling task for its whole retry sequence when
   nobody answers -- several seconds. So a name is looked up at most once per
   DNS_RETRY_MS while it fails, and once per DNS_TTL_MS while it works: often
   enough that a name moved to another address is followed within minutes,
   rarely enough that the lookup costs nothing. A failed lookup keeps the
   last good address, so a DNS outage does not take down devices whose
   address has not changed. */
#define DNS_TTL_MS     600000
#define DNS_RETRY_MS    10000

typedef struct {
    char     host[APP_CFG_MBM_HOST_LEN + 1];
    uint32_t addr_be;      /* last good answer, 0 = never resolved            */
    int64_t  next_ms;      /* when to ask DNS again                           */
    bool     last_ok;      /* the most recent lookup answered                 */
} dns_entry_t;

/* One slot per entry is enough: a table names at most APP_CFG_MBM_COUNT
   hosts, and the cache is emptied whenever the table changes. */
static dns_entry_t s_dns[APP_CFG_MBM_COUNT];

typedef struct {
    int64_t  due_ms;
    int64_t  value_ms;     /* when the last good value arrived, 0 = never */
    double   value;
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
/* Serialises start() and reload(), which are reached from different tasks:
   the network event task on an interface coming up, the HTTP task on a save.
   Both used to check s_running unlocked, so a save landing as a second
   interface came up could start two polling tasks on one connection cache. */
static SemaphoreHandle_t s_start_mux;

#define ST_LOCK()   do { if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY); } while (0)
#define ST_UNLOCK() do { if (s_state_mux) xSemaphoreGive(s_state_mux); } while (0)

/* The table this task works from, and nobody else touches.
 *
 * A poll has to be committed against the configuration it was started from:
 * which entry is polled, what its answer is committed to, what name it is
 * published under all belong to one generation from start to finish, and the
 * counters, due times and cached sockets have to change over together at one
 * defined point between polls. Reading the shared configuration directly
 * offers no such point, whatever it is guarded with. So the configuration is
 * handed over instead: the HTTP task leaves a complete copy in s_staged, and
 * this task picks it up between polls, where nothing of its own is in flight.
 * That it also sidesteps the unsynchronised memcpy in app_config_update() is
 * a property of that function, not of this file. */
static mbm_poll_t    s_active[APP_CFG_MBM_COUNT];   /* this task only       */
static mbm_poll_t    s_staged[APP_CFG_MBM_COUNT];   /* under s_state_mux    */
/* Set by a configuration change, acted on by the polling task: the cached
   sockets belong to that task and must not be closed from under it. */
static volatile bool s_reload_pending;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* Enabled and complete: a name, and a host unless it is on RS-485. */
static bool entry_active(const mbm_poll_t *e)
{
    return e->enable && e->name[0] && (e->transport == MBM_TRANSPORT_RTU || e->host[0]);
}

/* ------------------------------------------------------------ connections */

static void conn_close(conn_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->host[0] = '\0';
}

static dns_entry_t *dns_slot(const char *host)
{
    dns_entry_t *free_e = NULL;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        dns_entry_t *d = &s_dns[i];
        if (d->host[0] && strcmp(d->host, host) == 0) return d;
        if (!d->host[0] && !free_e) free_e = d;
    }
    /* Cannot run out (see s_dns); should it ever, the first slot is reused
       rather than the lookup refused. */
    dns_entry_t *d = free_e ? free_e : &s_dns[0];
    memset(d, 0, sizeof(*d));
    strlcpy(d->host, host, sizeof(d->host));
    return d;
}

/* The address to dial for host. A literal address never reaches the
   resolver. Needs no socket, so it runs before anything is given up for the
   connection it is for. */
static bool resolve(const char *host, uint32_t *addr_be, char *err, size_t err_len)
{
    struct in_addr lit;
    if (inet_aton(host, &lit)) {
        /* inet_aton also takes "0" and "0.0.0.0". Dialled, that is a connect
           timeout reported as "no answer" -- a configuration error dressed
           up as a dead device. */
        if (lit.s_addr == 0) {
            snprintf(err, err_len, "0.0.0.0 is not a device address");
            return false;
        }
        *addr_be = lit.s_addr;
        return true;
    }

    int64_t now = now_ms();
    dns_entry_t *d = dns_slot(host);

    if (now >= d->next_ms) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *ai = NULL;
        uint32_t got = 0;
        if (getaddrinfo(host, NULL, &hints, &ai) == 0 && ai) {
            got = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(ai);
        }
        now = now_ms();                          /* the lookup may have taken a while */
        d->last_ok = got != 0;
        if (got) { d->addr_be = got; d->next_ms = now + DNS_TTL_MS; }
        else     {                   d->next_ms = now + DNS_RETRY_MS; }
    }

    if (!d->addr_be) {
        snprintf(err, err_len, "name not resolved");
        return false;
    }
    *addr_be = d->addr_be;
    return true;
}

/* A connect to the address failed: it may have moved, so the next attempt
   asks DNS again -- once, for every entry naming this host.

   Only if the last lookup answered. If it did not, DNS is down and the
   address in use is the one kept from before; asking again at once would
   pay the resolver's whole timeout on every failed connect, entry after
   entry, which is what DNS_RETRY_MS is there to prevent. */
static void dns_recheck(const char *host)
{
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++)
        if (s_dns[i].host[0] && strcmp(s_dns[i].host, host) == 0 && s_dns[i].last_ok)
            s_dns[i].next_ms = 0;
}

static void dns_flush(void)
{
    memset(s_dns, 0, sizeof(s_dns));
}

/* Non-blocking connect with a deadline. A blocking one would sit on lwIP's
   own retransmission schedule -- tens of seconds -- for a device that is
   simply not there, and the whole polling task with it. */
static int dial(uint32_t addr_be, uint16_t port, char *err, size_t err_len)
{
    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = addr_be,
    };

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        snprintf(err, err_len, "no socket: errno %d", errno);
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
        return -1;
    }

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));

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
    *fresh = false;

    /* First, and before any connection is touched: a name that does not
       resolve must not cost some other entry its connection. */
    uint32_t addr_be = 0;
    if (!resolve(host, &addr_be, err, err_len)) return NULL;
    int64_t now = now_ms();

    /* An open connection is kept even if the name now resolves elsewhere.
       A TCP connection cannot change peers: if another device had taken the
       old address, it would answer this one with a reset, and the stale
       connection retry in read_value() would dial the new address. Closing
       on every differing answer instead tore down working connections to
       names with several addresses, whose order the DNS server rotates. */
    for (int i = 0; i < CONN_CACHE; i++) {
        conn_t *c = &s_conns[i];
        if (c->fd >= 0 && c->port == port && strcmp(c->host, host) == 0) {
            c->used_ms = now;
            return c;             /* kept from an earlier poll; may be stale */
        }
    }

    /* Take a free slot, or the one unused the longest. The old connection is
       closed before the new one is dialled, not after: for that moment the
       two would hold a socket each, and net_budget.h hands this task exactly
       CONN_CACHE of them. Losing a working connection to a dial that then
       fails costs that entry a reconnect on its next poll; running past the
       budget costs some other subsystem its socket, and that one does not
       get it back. */
    conn_t *pick = NULL;
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd < 0) { pick = &s_conns[i]; break; }
    if (!pick) {
        pick = &s_conns[0];
        for (int i = 1; i < CONN_CACHE; i++)
            if (s_conns[i].used_ms < pick->used_ms) pick = &s_conns[i];
    }

    if (pick->fd >= 0) conn_close(pick);
    int fd = dial(addr_be, port, err, err_len);
    if (fd < 0) { dns_recheck(host); return NULL; }

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

/* The registers of an answer -- big endian, as on the wire -- into the scaled
   value. Shared by both transports. */
static bool decode_scaled(const mbm_poll_t *e, const uint8_t *data, double *value,
                          char *err, size_t err_len)
{
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

/* An entry on the RS-485 segment: one transaction through the bus master,
   shared with forwarded Modbus TCP requests. Nothing to retry on a fresh
   connection here -- a device that did not answer gets the entry's backoff. */
static bool read_rtu(const mbm_poll_t *e, uint16_t regs, double *value,
                     char *err, size_t err_len)
{
    if (!mb_gateway_is_running()) {
        snprintf(err, err_len, "RS-485 is not set to master");
        return false;
    }
    uint16_t r[2] = {0};
    uint8_t exc = mb_gateway_read_regs(e->unit_id, e->fc, e->reg, regs, r);
    if (exc == MB_EXC_GW_TARGET) { snprintf(err, err_len, "no answer on RS-485"); return false; }
    if (exc != MB_EXC_NONE)      { snprintf(err, err_len, "device says exception 0x%02X", exc); return false; }
    uint8_t wire[4];
    for (uint16_t i = 0; i < regs; i++) mb_put16(&wire[i * 2], r[i]);
    return decode_scaled(e, wire, value, err, err_len);
}

/* What happened to one attempt, and whether trying again could change it. */
typedef enum { RD_OK, RD_FAILED, RD_STALE_CONN } rd_result_t;

/* One read, start to finish. Returns RD_STALE_CONN only for a failure that a
   fresh connection might cure: the connection was one kept from an earlier
   poll and the exchange on it went wrong. A connection that could not be
   opened at all, or a device that answered, are not that. */
static rd_result_t read_once(const mbm_poll_t *e, uint16_t regs, double *value,
                             char *err, size_t err_len)
{
    bool fresh = false;
    conn_t *c = conn_get(e->host, e->port, &fresh, err, err_len);
    if (!c) return RD_FAILED;        /* no connection; dialling again is the same */

    uint8_t  req[MB_MBAP_LEN + 5];
    uint16_t tid = ++s_tid;
    uint16_t n = mb_build_read_request(req, tid, e->unit_id, e->fc, e->reg, regs);
    /* An I/O failure on a connection kept from an earlier poll may be the
       connection's fault, and is worth one retry on a fresh one; on a fresh
       connection it is the device's. Decided once, used at every exit. */
    const rd_result_t on_io_fail = fresh ? RD_FAILED : RD_STALE_CONN;

    bool closed = false;
    if (!io_all(c->fd, req, n, true, &closed)) {
        snprintf(err, err_len, closed ? "connection lost while sending"
                                      : "could not send in %d ms", RESPONSE_MS);
        conn_close(c);
        return on_io_fail;
    }

    /* The header says how long the rest is, so it is read in two goes rather
       than guessed at. */
    uint8_t rsp[MB_MBAP_LEN + 2 + 8];
    if (!io_all(c->fd, rsp, 6, false, &closed)) {
        snprintf(err, err_len, closed ? "connection closed by the device"
                                      : "no answer in %d ms", RESPONSE_MS);
        conn_close(c);
        return on_io_fail;
    }
    uint16_t rest = mb_be16(&rsp[4]);
    if (rest < 2 || rest > sizeof(rsp) - 6) {
        snprintf(err, err_len, "bad length field %u", rest);
        conn_close(c);                       /* the stream is out of step now */
        return RD_FAILED;
    }
    if (!io_all(c->fd, &rsp[6], rest, false, &closed)) {
        snprintf(err, err_len, "answer cut short");
        conn_close(c);
        return on_io_fail;
    }

    const uint8_t *data = NULL;
    uint8_t exc = 0;
    mb_rsp_t r = mb_parse_read_response(rsp, (uint16_t)(6 + rest), tid, e->unit_id,
                                        e->fc, regs, &data, &exc);
    if (r == MB_RSP_EXCEPTION) {
        /* The device answered. Asking again would only ask the same thing. */
        snprintf(err, err_len, "device says exception 0x%02X", exc);
        return RD_FAILED;
    }
    if (r != MB_RSP_OK) {
        snprintf(err, err_len, "answer does not match the request");
        conn_close(c);
        return RD_FAILED;
    }

    return decode_scaled(e, data, value, err, err_len) ? RD_OK : RD_FAILED;
}

/* A connection kept from an earlier poll may have been closed at the other
   end without us hearing: the send succeeds into a half-closed socket and the
   read comes back empty. Devices that drop idle connections after 10 to 30
   seconds are common, and this cache holds them for 60, so without a second
   attempt on a fresh connection such a device would fail every other poll for
   ever.

   Only that case is retried. A host that cannot be reached at all would
   otherwise cost two full connect timeouts per poll on the one polling task
   -- with a tableful of dead entries, all made due at once by a save, a
   single pass would block for most of a minute and every healthy entry would
   miss its turn. And a device that answered with an exception has said what
   it has to say. */
static bool read_value(const mbm_poll_t *e, double *value, char *err, size_t err_len)
{
    uint16_t regs = mb_value_regs(e->type);
    if (regs == 0) { snprintf(err, err_len, "unknown value type %u", e->type); return false; }

    if (e->transport == MBM_TRANSPORT_RTU) return read_rtu(e, regs, value, err, err_len);

    rd_result_t r = read_once(e, regs, value, err, err_len);
    if (r == RD_OK)     return true;
    if (r == RD_FAILED) return false;
    return read_once(e, regs, value, err, err_len) == RD_OK;   /* once more, fresh */
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
    /* Spread out rather than all due at the same instant. The one polling task
       works them in order, and an entry whose host is switched off costs a
       full connect timeout before the next one is even tried; a tableful of
       those, paid back to back, is most of a minute in which every healthy
       entry misses its interval. */
    int64_t now = now_ms();
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) s_state[i].due_ms = now + i * 200;
    ST_UNLOCK();

    /* A cached socket may lead to a device nobody asks about any more, and a
       cached name to an address nobody has looked at since the save. */
    for (int i = 0; i < CONN_CACHE; i++)
        if (s_conns[i].fd >= 0) conn_close(&s_conns[i]);
    dns_flush();
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
            if (!entry_active(e)) continue;

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

esp_err_t mb_tcp_master_init(void)
{
    if (!s_state_mux) s_state_mux = xSemaphoreCreateMutex();
    if (!s_start_mux) s_start_mux = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_mux && s_start_mux, ESP_ERR_NO_MEM, TAG, "mutexes");
    return ESP_OK;
}

/* Under s_start_mux. */
static esp_err_t start_locked(void)
{
    if (s_running) return ESP_OK;

    stage_config();           /* the task reads it on its first pass */

    int enabled = 0;
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++)
        if (entry_active(&s_staged[i])) enabled++;
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

esp_err_t mb_tcp_master_start(void)
{
    ESP_RETURN_ON_FALSE(s_start_mux, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_start_mux, portMAX_DELAY);
    esp_err_t ret = start_locked();
    xSemaphoreGive(s_start_mux);
    return ret;
}

esp_err_t mb_tcp_master_reload(void)
{
    ESP_RETURN_ON_FALSE(s_start_mux, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_start_mux, portMAX_DELAY);
    /* With no task yet -- the first entry anyone has enabled -- starting is
       the reload: start_locked() stages the table itself. */
    esp_err_t ret = ESP_OK;
    if (s_running) stage_config();
    else           ret = start_locked();
    xSemaphoreGive(s_start_mux);
    return ret;
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
            .valid   = st->value_ms != 0,
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
