#include "mb_tcp_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "mb_pdu.h"
#include "mb_request.h"
#include "app_config.h"
#include "mb_server.h"
#include "mb_gateway.h"
#include "net_budget.h"

#define TAG "mb_tcp"

/* Why this is not the component's own TCP slave.
 *
 * That one serves every connection from a single task and answers each
 * request before reading the next. A request this board answers itself takes
 * about 50 ms; one forwarded to a silent device takes 650. With one task the
 * second kind delays the first -- measured, a local read that took 52 ms
 * alone took 594 ms alongside a request to an address nobody answers.
 *
 * So connections and requests are separated. One task polls every connection
 * and never blocks on receiving at all -- it takes what has arrived and comes
 * back -- and the work goes to a pool of workers, one request at a time. It
 * never waits on a send either: what is left of an answer stays with its
 * connection and goes out when the socket can take it, and a client that has
 * not taken its answer within SEND_WAIT_MS is dropped. A worker occupied with the segment holds up
 * nothing else, and an idle connection occupies no worker at all -- which is
 * why the pool is per request and not per connection: Modbus clients keep
 * their connections open for hours.
 *
 * The segment itself still carries one transaction at a time; that is
 * physics, and mb_gateway_handle() serialises for it. What the pool buys is
 * that everything which does NOT need the segment goes straight through. */

/* A budget, not a capacity -- net_budget.h says what the board can spare. A
   client arriving when all slots are taken has its connection closed at once,
   which it sees as a reset: the only honest answer, since there is no request
   yet to reply to. */
#define MB_TCP_MAX_CONN       NET_SOCK_MB_CONN
#define MB_TCP_JOB_DEPTH      MB_TCP_WORKERS

#define MBAP_HDR              (MB_MBAP_LEN - 1)   /* the part before uid */
#define FRAME_MAX             (MB_MBAP_LEN + MB_PDU_MAX)

/* How long a whole frame may take once its first bytes have arrived. On a
   local segment a 260-byte frame is one or two packets; a client that cannot
   finish one in this time is broken, and waiting longer would hold up every
   other connection, since this runs on the polling task.

   The socket's own timeout is much shorter so that no single recv() can
   overrun the deadline by more than a little: the deadline is checked between
   calls, so without this the last recv() could add a further whole
   FRAME_WAIT_MS, and with every connection doing it the poll loop would stall
   for a multiple of it. */
#define FRAME_WAIT_MS         200

/* How often the poll loop looks at the clock while some connection holds a
   partly received frame. Only reached in that case; an idle server waits on
   the network and nothing else. */
#define PARTIAL_POLL_MS        50

/* A peer that is switched off or dropped by a NAT never closes its end, and
   select() never reports its connection readable again, so without this its
   slot would be held until the board reboots. Modbus clients do hold a
   connection open for hours while idle, which is why the dead ones are found
   by keepalive rather than by an idle timeout. */
#define KEEPALIVE_IDLE_S      60
#define KEEPALIVE_INTVL_S     10
#define KEEPALIVE_COUNT        3

/* How long an answer may take to leave. A response the peer never reads
   fills the socket buffer; every send() here is non-blocking, so nothing
   waits on such a peer, but its slot and its buffer would be held for ever.
   Measured on the bench: a client that sends 8000 requests and reads none
   gets 200 responses and the rest only when it finally reads.

   A response is at most 260 bytes, so half a second is already a hundred
   times what a peer that is reading at all needs; one that cannot take it in
   that time is not going to. The connection is then ended -- there is no way
   to resync a half-written response anyway. */
#define SEND_WAIT_MS         500


enum { SLOT_FREE = 0, SLOT_IDLE, SLOT_BUSY, SLOT_DEAD };

typedef struct {
    int              fd;
    volatile uint8_t state;
    /* Receiving is a state machine, not a loop: the polling task reads what
       has arrived and comes back, so a client that sends one byte and stops
       delays nobody but itself. Each frame gets one deadline covering the
       whole of it, header and body together. */
    uint16_t         got;          /* bytes of the current frame held     */
    uint16_t         want;         /* bytes expected in total; 0 = idle   */
    int64_t          deadline_us;
    uint8_t          frame[FRAME_MAX];

    /* Sending is a state machine too, for the same reason. A client whose
       receive window has closed would otherwise hold the polling task for
       the whole send deadline while every other connection waits. What is
       left of a response sits here and goes out when the socket says it can
       take it. A worker does not use this -- it has a task of its own and
       may block on its own connection. */
    uint8_t          out[MB_MBAP_LEN + MB_PDU_MAX];
    uint16_t         out_len;      /* 0 = nothing pending                 */
    uint16_t         out_sent;
    int64_t          out_deadline_us;
} conn_t;

/* The slot, not the frame. A worker owns its connection outright for the
   length of the job -- pump_slot() marks the slot SLOT_BUSY before queueing
   and every poller path skips a slot that is not SLOT_IDLE -- so the request
   can stay where it was received and the answer be written where it will be
   sent from. Copying it through the queue would cost a quarter kilobyte
   three times over for a buffer that cannot move. */
typedef struct {
    int      slot;
    uint16_t len;
} job_t;

static conn_t        s_conn[MB_TCP_MAX_CONN];
static QueueHandle_t s_jobs;
static int           s_listen_fd = -1;
/* Separate from s_listen_fd on purpose. The poller reads s_listen_fd as its
   first act, so the socket has to be published before the task exists; this
   flag is what "already started" means, and it is set last. */
static bool          s_started;

/* A worker finishing cannot be seen by select(), so it writes here and the
   poller wakes. Polling for it instead cost every request on a busy
   connection up to the poll interval -- measured on the bench, the median
   went from 3 ms with the connection idle between requests to 20 ms with it
   not. */
static int           s_wake_fd = -1;
static uint8_t       s_local_uid = MB_TCP_UID_DEFAULT;
/* Written by every worker without a lock. A count can be lost when two
   workers finish in the same instant; these are for telling a busy segment
   from a broken one, not for billing. */
static mb_tcp_stats_t s_stats;

void mb_tcp_server_get_stats(mb_tcp_stats_t *out) { *out = s_stats; }

/* ------------------------------------------------------------------ workers */

/* Says that the answer already written into the connection's out buffer is
   ready to go, and starts its clock. Whatever the socket will not take now
   goes when select() says it is writable again.

   Never overwrites an answer still going out: the poll loop reads from a
   connection only once its previous answer has left, so a second request
   cannot be taken in before the first is finished with. */
static void arm_out(int i, uint16_t len)
{
    conn_t *c = &s_conn[i];
    c->out_len  = len;
    c->out_sent = 0;
    c->out_deadline_us = esp_timer_get_time() + (int64_t)SEND_WAIT_MS * 1000;
}

/* Which device a request is for. 0 and 255 count as this board because a TCP
   client with nothing to address sends one of them, and on TCP neither means
   broadcast. */
static bool is_local(uint8_t uid)
{
    return (uid == s_local_uid) || (uid == 0) || (uid == 255);
}

/* Answers one frame. Called on the polling task for a request this board can
   answer itself, and on a worker for one that has to go to the segment. */
static uint16_t process(const uint8_t *frame, uint16_t len, uint8_t *out)
{
    const uint8_t *pdu     = &frame[MB_MBAP_LEN];
    uint16_t       pdu_len = (uint16_t)(len - MB_MBAP_LEN);
    uint8_t        uid     = frame[6];

    mb_request_t req;
    uint8_t  data[MB_DATA_MAX];
    uint16_t data_len = 0;

    uint8_t exc = mb_parse_pdu(pdu, pdu_len, &req);
    if (exc == MB_EXC_NONE) {
        if (is_local(uid)) {
            exc = mb_server_handle(&req, data, &data_len);
        } else if (mb_gateway_is_running()) {
            s_stats.forwarded++;
            exc = mb_gateway_handle(uid, &req, data, &data_len);
        } else {
            exc = MB_EXC_GW_PATH;   /* no segment to forward to */
        }
    }

    uint16_t out_pdu = mb_build_response(&req, pdu, data, data_len, exc, &out[MB_MBAP_LEN]);

    memcpy(out, frame, 4);                      /* transaction and protocol id */
    out[4] = (uint8_t)((out_pdu + 1) >> 8);     /* length counts the unit id   */
    out[5] = (uint8_t)((out_pdu + 1) & 0xFF);
    out[6] = uid;

    s_stats.requests++;
    if (exc != MB_EXC_NONE) s_stats.exceptions++;
    return (uint16_t)(MB_MBAP_LEN + out_pdu);
}

/* The deepest call here is a forwarded request: the esp-modbus request chain
   and its buffers. Rather than guess the headroom and find out from a
   corrupted stack, say so while there is still some left.

   Not on every request: uxTaskGetStackHighWaterMark() walks the unused stack
   byte by byte looking for the fill pattern, which against the millisecond a
   local request takes is not free. The watermark only ever falls, so once a
   second reports the same thing as once a request. INT64_MAX in *next_us
   means it has been reported and there is nothing more to say. */
static void check_stack(const char *who, int64_t *next_us)
{
    int64_t now = esp_timer_get_time();
    if (now < *next_us) return;

    UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
    if (left < 512) {
        *next_us = INT64_MAX;
        ESP_LOGW(TAG, "%s stack down to %u bytes — raise it", who, (unsigned)left);
    } else {
        *next_us = now + 1000000;
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    int64_t stack_next_us = 0;

    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;

        /* The slot is this task's for the length of the job, so the answer is
           built where it will be sent from and sending is left to the poller
           -- which already has a non-blocking send path for its own answers,
           and is the only task that may wait on a socket without holding a
           connection hostage. */
        conn_t *c = &s_conn[job.slot];
        arm_out(job.slot, process(c->frame, job.len, c->out));

        c->state = SLOT_IDLE;                 /* after the answer is armed */

        uint64_t one = 1;                     /* the connection can be polled again */
        (void)write(s_wake_fd, &one, sizeof(one));

        check_stack("worker", &stack_next_us);
    }
}

/* ------------------------------------------------------------------ poller */

/* Returns false if the connection is finished -- either the peer is gone or
   it has not taken the response within SEND_WAIT_MS. Half a response cannot
   be taken back, so there is nothing else to do with such a connection. */
static bool flush_out(int i)
{
    conn_t *c = &s_conn[i];

    while (c->out_sent < c->out_len) {
        int n = send(c->fd, c->out + c->out_sent,
                     c->out_len - c->out_sent, MSG_DONTWAIT);
        if (n > 0) { c->out_sent = (uint16_t)(c->out_sent + n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return esp_timer_get_time() <= c->out_deadline_us;
        return false;
    }
    c->out_len = 0;                       /* the whole answer is away */
    return true;
}

static void close_slot(int i)
{
    if (s_conn[i].fd >= 0) close(s_conn[i].fd);
    s_conn[i].fd    = -1;
    s_conn[i].state = SLOT_FREE;
    s_conn[i].want    = 0;
    s_conn[i].got     = 0;
    s_conn[i].out_len = 0;
}

/* True if the connection held a frame it did not finish in time, in which
   case it has been closed. A frame's deadline covers the whole of it, so this
   has to be asked both on the clock and again before a frame that has just
   been completed is served. */
static bool frame_expired(int i, int64_t now)
{
    if (!s_conn[i].got || now <= s_conn[i].deadline_us) return false;

    s_stats.malformed++;
    ESP_LOGW(TAG, "slot %d: frame unfinished after %d ms — closing", i, FRAME_WAIT_MS);
    close_slot(i);
    return true;
}

/* Answers without going near a worker. Used when the pool is saturated: the
   client is told to retry rather than left waiting for a slot. */
static uint16_t build_busy(const uint8_t *frame, uint8_t *out)
{
    memcpy(out, frame, 4);
    out[4] = 0; out[5] = 3;
    out[6] = frame[6];
    out[7] = (uint8_t)(frame[MB_MBAP_LEN] | 0x80u);
    out[8] = MB_EXC_DEVICE_BUSY;
    return MB_MBAP_LEN + 2;
}

static void accept_one(void)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;

    /* A slot a worker gave up on in this same iteration is free in all but
       name; reclaim it before turning anyone away. */
    for (int i = 0; i < MB_TCP_MAX_CONN; i++)
        if (s_conn[i].state == SLOT_DEAD) close_slot(i);

    int slot = -1;
    for (int i = 0; i < MB_TCP_MAX_CONN; i++)
        if (s_conn[i].state == SLOT_FREE) { slot = i; break; }

    if (slot < 0) {
        s_stats.refused++;
        ESP_LOGW(TAG, "no free slot, dropped a connection (%d in use)", MB_TCP_MAX_CONN);
        close(fd);
        return;
    }

    /* Modbus frames are small and answered one at a time, so Nagle would only
       add delay waiting for a second frame that is not coming. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int ka = 1, idle = KEEPALIVE_IDLE_S, intvl = KEEPALIVE_INTVL_S, cnt = KEEPALIVE_COUNT;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &ka,    sizeof(ka));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    s_conn[slot].fd    = fd;
    s_conn[slot].state = SLOT_IDLE;
    s_stats.accepted++;
    ESP_LOGD(TAG, "connection on slot %d", slot);
}

/* Takes whatever has arrived on one connection and, when that completes a
   frame, hands it on. Never waits: a single non-blocking read per visit, so
   eight clients dribbling one byte each cost eight reads, not eight timeouts
   in series.

   The deadline covers a whole frame rather than each read, so a client cannot
   extend it by arriving in pieces -- which the previous version allowed, a
   frame finishing at 360 ms against a stated limit of 200. */
static void pump_slot(int i)
{
    conn_t *c = &s_conn[i];

    /* The periodic sweep runs once per pass of the poll loop, and a slot
       served earlier in the same pass can have used up the time since. A
       frame whose deadline has gone must not be served just because its last
       bytes happened to arrive. */
    if (frame_expired(i, esp_timer_get_time())) return;

    if (c->want == 0) c->want = MBAP_HDR;     /* nothing of a frame yet */

    /* Read until the socket is empty. Every call is non-blocking, so this
       loop ends on the first EAGAIN -- it exists so that a frame already
       waiting whole is taken in one visit rather than one piece per pass of
       the poll loop. */
    for (;;) {
        int n = recv(c->fd, c->frame + c->got, c->want - c->got, MSG_DONTWAIT);
        if (n == 0) { close_slot(i); return; }        /* the peer closed */
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) { close_slot(i); return; }
            return;                                   /* nothing more for now */
        }

        /* The clock starts with the first byte of a frame, not when the
           connection was last looked at: a client may sit idle for hours
           between requests, and only the one it has begun is on a deadline. */
        if (c->got == 0)
            c->deadline_us = esp_timer_get_time() + (int64_t)FRAME_WAIT_MS * 1000;
        c->got = (uint16_t)(c->got + n);

        /* With the header complete, the frame's real length is known. */
        if (c->want == MBAP_HDR && c->got == MBAP_HDR) {
            uint16_t pid = mb_be16(&c->frame[2]);
            uint16_t hlen = mb_be16(&c->frame[4]);    /* unit id plus PDU */
            if (pid != 0 || hlen < 2 || hlen > (uint16_t)(1 + MB_PDU_MAX)) {
                s_stats.malformed++;
                ESP_LOGW(TAG, "slot %d: protocol id %u, length %u — closing",
                         i, pid, hlen);
                close_slot(i);
                return;
            }
            c->want = (uint16_t)(MBAP_HDR + hlen);
        }

        if (c->got >= c->want) break;         /* a whole frame is in hand */
    }

    uint16_t len = c->want;
    c->want = 0;                              /* ready for the next frame */
    c->got  = 0;

    /* A request this board answers itself reads cached state and queues any
       write -- about a millisecond, with nothing in it that can block for
       long. Doing it here rather than through the pool means the segment can
       never delay it, and no number of clients waiting on a dead address can
       take the last worker away from it. */
    /* Also everything else when there is no pool: with no gateway running
       there is nothing a foreign unit id could be forwarded to, so process()
       answers "gateway path unavailable" without touching anything that can
       block, and a worker would add a task switch for nothing. */
    if (is_local(c->frame[6]) || !s_jobs) {
        arm_out(i, process(c->frame, len, c->out));
        if (!flush_out(i)) close_slot(i);
        return;
    }

    /* Marked busy before the job is queued, which is what makes the slot's
       buffers the worker's alone: every poller path below skips a slot that
       is not SLOT_IDLE. */
    job_t job = { .slot = i, .len = len };
    c->state = SLOT_BUSY;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        /* Every worker is on the segment already. Rather than queue without
           bound, say so: "slave device busy" is the one answer a client knows
           to retry. */
        s_stats.overloaded++;
        c->state = SLOT_IDLE;
        arm_out(i, build_busy(c->frame, c->out));
        if (!flush_out(i)) close_slot(i);
    }
}

/* Closes any connection that began a frame and did not finish it in time.
   Separate from the poll loop so it can be tested on its own -- a client that
   sends one byte and stops is never reported readable again, so this is the
   only thing that ends such a connection before keepalive would. */
static void sweep_deadlines(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MB_TCP_MAX_CONN; i++) {
        if (s_conn[i].state != SLOT_IDLE) continue;

        if (s_conn[i].out_len && now > s_conn[i].out_deadline_us) {
            ESP_LOGW(TAG, "slot %d: answer not taken within %d ms — closing",
                     i, SEND_WAIT_MS);
            close_slot(i);
            continue;
        }
        frame_expired(i, now);
    }
}

static void poller_task(void *arg)
{
    (void)arg;
    int64_t stack_next_us = 0;

    for (;;) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(s_listen_fd, &rd);
        FD_SET(s_wake_fd, &rd);
        int  maxfd   = (s_listen_fd > s_wake_fd) ? s_listen_fd : s_wake_fd;
        bool partial = false;

        for (int i = 0; i < MB_TCP_MAX_CONN; i++) {
            if (s_conn[i].state == SLOT_DEAD) {
                close_slot(i);                       /* a worker could not answer */
                continue;
            }
            if (s_conn[i].state != SLOT_IDLE) continue;

            if (s_conn[i].fd > maxfd) maxfd = s_conn[i].fd;

            if (s_conn[i].out_len) {
                /* An answer is still going out. Nothing new is read from this
                   connection until it has: Modbus is one request at a time,
                   and anything the client sends meanwhile keeps in the
                   socket. */
                FD_SET(s_conn[i].fd, &wr);
                partial = true;
            } else {
                FD_SET(s_conn[i].fd, &rd);
                if (s_conn[i].got) partial = true;
            }
        }

        /* Normally there is no timeout: the only things that can change are a
           packet, a new connection, or a worker finishing, and all three are
           in the set. A connection holding half a frame is the exception --
           it will never be reported readable again if the client has simply
           stopped, so its deadline has to be looked at on a clock. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = PARTIAL_POLL_MS * 1000 };
        int ready = select(maxfd + 1, &rd, &wr, NULL, partial ? &tv : NULL);
        if (ready < 0) {
            /* With no timeout select() cannot return 0, so this is a real
               failure. Spinning on it at this priority would starve the
               workers in silence. */
            /* Once, not ten times a second for as long as it lasts. */
            static bool said;
            if (!said) { said = true; ESP_LOGE(TAG, "select: errno %d", errno); }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* Sweep first: a half-finished frame whose client fell silent is
           closed on time whether or not anything else happened. */
        if (partial) sweep_deadlines();
        if (ready == 0) continue;

        if (FD_ISSET(s_wake_fd, &rd)) {
            uint64_t v;
            (void)read(s_wake_fd, &v, sizeof(v));    /* clears the counter */
        }
        if (FD_ISSET(s_listen_fd, &rd)) accept_one();

        /* Outgoing first: a connection waiting to be written to is holding a
           buffer and a deadline, and finishing it frees both. */
        for (int i = 0; i < MB_TCP_MAX_CONN; i++)
            if (s_conn[i].state == SLOT_IDLE && s_conn[i].out_len &&
                FD_ISSET(s_conn[i].fd, &wr) && !flush_out(i))
                close_slot(i);

        for (int i = 0; i < MB_TCP_MAX_CONN; i++)
            if (s_conn[i].state == SLOT_IDLE && !s_conn[i].out_len &&
                FD_ISSET(s_conn[i].fd, &rd))
                pump_slot(i);

        check_stack("poller", &stack_next_us);
    }
}

/* ------------------------------------------------------------------ public */

/* Undoes a partial start, in the right order: the tasks go first, because a
   worker is blocked on the job queue and deleting the queue under it would
   take the board down. Everything here tolerates not having been created, so
   one path can clean up after any of the failures below. */
static void start_unwind(int fd, TaskHandle_t *tasks, int n_tasks)
{
    for (int i = 0; i < n_tasks; i++)
        if (tasks[i]) vTaskDelete(tasks[i]);
    s_listen_fd = -1;
    if (fd >= 0)          close(fd);
    if (s_wake_fd >= 0) { close(s_wake_fd); s_wake_fd = -1; }
    if (s_jobs)         { vQueueDelete(s_jobs); s_jobs = NULL; }
}

esp_err_t mb_tcp_server_start(uint16_t port, uint8_t local_uid)
{
    if (s_started) return ESP_OK;                    /* a second interface came up */

    s_local_uid = local_uid;

    for (int i = 0; i < MB_TCP_MAX_CONN; i++) { s_conn[i].fd = -1; s_conn[i].state = SLOT_FREE; }

    /* The pool exists to keep a slow RS-485 segment off the polling task.
       With the segment configured as a slave there is no segment to be slow:
       nothing is ever forwarded, and four task stacks would stand idle for
       the life of the board. The role is fixed at boot -- changing it needs a
       reboot -- so asking once here is enough. */
    if (mb_gateway_is_running()) {
        s_jobs = xQueueCreate(MB_TCP_JOB_DEPTH, sizeof(job_t));
        ESP_RETURN_ON_FALSE(s_jobs, ESP_ERR_NO_MEM, TAG, "job queue");
    }

    /* select() cannot see a worker finish, so one eventfd stands in for all
       of them. Registering twice is not an error anyone else's fault. */
    esp_vfs_eventfd_config_t efd_cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t efd_err = esp_vfs_eventfd_register(&efd_cfg);
    if (efd_err != ESP_OK && efd_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "eventfd vfs: %s", esp_err_to_name(efd_err));
        start_unwind(-1, NULL, 0);
        return efd_err;
    }
    s_wake_fd = eventfd(0, 0);
    if (s_wake_fd < 0) {
        ESP_LOGE(TAG, "eventfd: errno %d", errno);
        start_unwind(-1, NULL, 0);
        return ESP_FAIL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        start_unwind(-1, NULL, 0);
        return ESP_FAIL;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind port %u: errno %d", port, errno);
        start_unwind(fd, NULL, 0);
        return ESP_FAIL;
    }
    if (listen(fd, MB_TCP_MAX_CONN) < 0) {
        ESP_LOGE(TAG, "listen: errno %d", errno);
        start_unwind(fd, NULL, 0);
        return ESP_FAIL;
    }

    /* The one socket on this task with no timeout. accept() is only reached
       when select() says there is something, but a connection aborted in
       between would otherwise block the task that owns every connection and
       every worker completion, with nothing to recover it. */
    int lflags = fcntl(fd, F_GETFL, 0);
    if (lflags < 0 || fcntl(fd, F_SETFL, lflags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "listen socket mode: errno %d", errno);
        start_unwind(fd, NULL, 0);
        return ESP_FAIL;
    }

    /* Published before the tasks exist: the poller's first statement reads it
       into its fd set, and on a dual-core board it can get there before this
       function's next line. */
    s_listen_fd = fd;

    int n_workers = s_jobs ? MB_TCP_WORKERS : 0;
    TaskHandle_t tasks[MB_TCP_WORKERS + 1] = {0};
    for (int i = 0; i < n_workers; i++) {
        char name[16];
        snprintf(name, sizeof(name), "mb_tcp_w%d", i);
        if (xTaskCreate(worker_task, name, 5120, NULL, 5, &tasks[i]) != pdPASS) {
            ESP_LOGE(TAG, "worker task %d", i);
            start_unwind(fd, tasks, n_workers + 1);
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(poller_task, "mb_tcp_poll", 5120, NULL, 6,
                    &tasks[n_workers]) != pdPASS) {
        ESP_LOGE(TAG, "poller task");
        start_unwind(fd, tasks, n_workers + 1);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    ESP_LOGI(TAG, "listening on port %u — %d connections, %d workers",
             port, MB_TCP_MAX_CONN, n_workers);
    return ESP_OK;
}
