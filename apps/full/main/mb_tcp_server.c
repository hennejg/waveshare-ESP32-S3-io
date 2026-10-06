#include "mb_tcp_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

#include "mbcontroller.h"      /* the MB_FUNC_* function codes */
#include "mb_request.h"
#include "app_config.h"
#include "mb_server.h"
#include "mb_gateway.h"

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
 * and does nothing that can block for long; the work goes to a pool of
 * workers, one request at a time. A worker occupied with the segment holds up
 * nothing else, and an idle connection occupies no worker at all -- which is
 * why the pool is per request and not per connection: Modbus clients keep
 * their connections open for hours.
 *
 * The segment itself still carries one transaction at a time; that is
 * physics, and mb_gateway_handle() serialises for it. What the pool buys is
 * that everything which does NOT need the segment goes straight through. */

#define MB_TCP_WORKERS        4
/* The board has a fixed number of lwIP sockets and the web server wants its
   share, so this is a budget, not a capacity. Of the 24 the build allows, the
   HTTP server takes about ten and SNTP one; eight connections plus the
   listener still leave room for MQTT and for sockets lingering in TIME_WAIT.
   A client arriving when all eight are taken has its connection closed at
   once, which it sees as a reset -- the only honest answer, since there is no
   request yet to reply to. */
#define MB_TCP_MAX_CONN       8
#define MB_TCP_JOB_DEPTH      MB_TCP_WORKERS

#define MBAP_LEN              7           /* tid(2) pid(2) len(2) uid(1) */
#define MBAP_HDR              6           /* the part before uid         */
#define FRAME_MAX             (MBAP_LEN + MB_PDU_MAX)

/* How long the rest of a frame may take once its first bytes have arrived. On
   a local segment a 260-byte frame is one or two packets; a client that
   cannot finish one in this time is broken, and waiting longer would hold up
   every other connection, since this runs on the polling task. */
#define FRAME_WAIT_MS         200


enum { SLOT_FREE = 0, SLOT_IDLE, SLOT_BUSY, SLOT_DEAD };

typedef struct {
    int              fd;
    volatile uint8_t state;
} conn_t;

typedef struct {
    int      slot;
    uint16_t len;
    uint8_t  frame[FRAME_MAX];
} job_t;

static conn_t        s_conn[MB_TCP_MAX_CONN];
static QueueHandle_t s_jobs;
static int           s_listen_fd = -1;

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

/* ------------------------------------------------------------------ parsing */

/* Limits from the specification. A frame that breaks one of them is a client
   error, not a device error, and gets the matching exception rather than
   being passed on to something that would have to guess. */
#define MAX_READ_BITS         2000
#define MAX_READ_REGS         125
#define MAX_WRITE_BITS        1968
#define MAX_WRITE_REGS        123

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Takes a PDU apart. Returns 0 and fills req, or the exception to answer
   with. Every length is checked against the function code, so a handler
   never sees a count that does not match the bytes behind it. */
static uint8_t parse_pdu(const uint8_t *pdu, uint16_t len, mb_request_t *req)
{
    memset(req, 0, sizeof(*req));
    if (len < 1) return MB_EXC_ILLEGAL_FUNC;   /* req->fc stays 0 for the reply */
    req->fc = pdu[0];

    switch (req->fc) {
    case MB_FUNC_READ_COILS:
    case MB_FUNC_READ_DISCRETE_INPUTS:
    case MB_FUNC_READ_HOLDING_REGISTER:
    case MB_FUNC_READ_INPUT_REGISTER: {
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        req->addr  = be16(&pdu[1]);
        req->count = be16(&pdu[3]);
        uint16_t max = (req->fc == MB_FUNC_READ_COILS ||
                        req->fc == MB_FUNC_READ_DISCRETE_INPUTS)
                       ? MAX_READ_BITS : MAX_READ_REGS;
        if (req->count < 1 || req->count > max) return MB_EXC_ILLEGAL_VALUE;
        return MB_EXC_NONE;
    }

    case MB_FUNC_WRITE_SINGLE_COIL:
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        /* Only all-ones and all-zeros mean anything here. */
        if (!((pdu[3] == 0xFF || pdu[3] == 0x00) && pdu[4] == 0x00))
            return MB_EXC_ILLEGAL_VALUE;
        req->addr = be16(&pdu[1]);
        req->count = 1;
        req->data = &pdu[3];
        req->data_len = 2;
        return MB_EXC_NONE;

    case MB_FUNC_WRITE_REGISTER:
        if (len != 5) return MB_EXC_ILLEGAL_VALUE;
        req->addr = be16(&pdu[1]);
        req->count = 1;
        req->data = &pdu[3];
        req->data_len = 2;
        return MB_EXC_NONE;

    case MB_FUNC_WRITE_MULTIPLE_COILS:
    case MB_FUNC_WRITE_MULTIPLE_REGISTERS: {
        if (len < 7) return MB_EXC_ILLEGAL_VALUE;
        req->addr  = be16(&pdu[1]);
        req->count = be16(&pdu[3]);
        uint8_t  byte_cnt = pdu[5];
        bool     bits     = (req->fc == MB_FUNC_WRITE_MULTIPLE_COILS);
        uint16_t max      = bits ? MAX_WRITE_BITS : MAX_WRITE_REGS;
        uint16_t want     = bits ? (uint16_t)((req->count + 7u) / 8u)
                                 : (uint16_t)(req->count * 2u);
        if (req->count < 1 || req->count > max) return MB_EXC_ILLEGAL_VALUE;
        if (byte_cnt != want || len != 6u + byte_cnt) return MB_EXC_ILLEGAL_VALUE;
        req->data = &pdu[6];
        req->data_len = byte_cnt;
        return MB_EXC_NONE;
    }

    default:
        return MB_EXC_ILLEGAL_FUNC;
    }
}

/* Builds the response PDU. For a read the handler supplied the data bytes;
   for a write the answer is the request's own first five bytes, which is what
   the specification asks for in every one of the four write cases. */
static uint16_t build_response(const mb_request_t *req, const uint8_t *pdu,
                               const uint8_t *data, uint16_t data_len,
                               uint8_t exc, uint8_t *out)
{
    if (exc != MB_EXC_NONE) {
        out[0] = (uint8_t)(req->fc | 0x80u);
        out[1] = exc;
        return 2;
    }

    switch (req->fc) {
    case MB_FUNC_READ_COILS:
    case MB_FUNC_READ_DISCRETE_INPUTS:
    case MB_FUNC_READ_HOLDING_REGISTER:
    case MB_FUNC_READ_INPUT_REGISTER:
        out[0] = req->fc;
        out[1] = (uint8_t)data_len;
        memcpy(&out[2], data, data_len);
        return (uint16_t)(2 + data_len);

    default:                      /* FC05, FC06, FC15, FC16 all echo five bytes */
        memcpy(out, pdu, 5);
        return 5;
    }
}

/* ------------------------------------------------------------------ workers */

/* Which device a request is for. 0 and 255 count as this board because a TCP
   client with nothing to address sends one of them, and on TCP neither means
   broadcast. */
static bool is_local(uint8_t uid)
{
    return (uid == s_local_uid) || (uid == 0) || (uid == 255);
}

static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* Answers one frame. Called on the polling task for a request this board can
   answer itself, and on a worker for one that has to go to the segment. */
static uint16_t process(const uint8_t *frame, uint16_t len, uint8_t *out)
{
    const uint8_t *pdu     = &frame[MBAP_LEN];
    uint16_t       pdu_len = (uint16_t)(len - MBAP_LEN);
    uint8_t        uid     = frame[6];

    mb_request_t req;
    uint8_t  data[MB_DATA_MAX];
    uint16_t data_len = 0;

    uint8_t exc = parse_pdu(pdu, pdu_len, &req);
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

    uint16_t out_pdu = build_response(&req, pdu, data, data_len, exc, &out[MBAP_LEN]);

    memcpy(out, frame, 4);                      /* transaction and protocol id */
    out[4] = (uint8_t)((out_pdu + 1) >> 8);     /* length counts the unit id   */
    out[5] = (uint8_t)((out_pdu + 1) & 0xFF);
    out[6] = uid;

    s_stats.requests++;
    if (exc != MB_EXC_NONE) s_stats.exceptions++;
    return (uint16_t)(MBAP_LEN + out_pdu);
}

/* The deepest call here is a forwarded request: two buffers of a quarter
   kilobyte each, and the whole esp-modbus request chain below them. Rather
   than guess the headroom and find out from a corrupted stack, say so while
   there is still some left. */
static void check_stack(const char *who)
{
    static bool warned;
    UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
    if (left < 512 && !warned) {
        warned = true;
        ESP_LOGW(TAG, "%s stack down to %u bytes — raise it", who, (unsigned)left);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    job_t *job = malloc(sizeof(job_t));
    configASSERT(job);

    for (;;) {
        if (xQueueReceive(s_jobs, job, portMAX_DELAY) != pdTRUE) continue;

        uint8_t  out[MBAP_LEN + MB_PDU_MAX];
        uint16_t out_len = process(job->frame, job->len, out);

        int slot = job->slot;
        bool ok = send_all(s_conn[slot].fd, out, out_len);
        s_conn[slot].state = ok ? SLOT_IDLE : SLOT_DEAD;

        uint64_t one = 1;                     /* the connection can be polled again */
        (void)write(s_wake_fd, &one, sizeof(one));

        check_stack("worker");
    }
}

/* ------------------------------------------------------------------ poller */

/* Reads exactly len bytes, giving up after FRAME_WAIT_MS in total. Returns
   false on close, error or timeout -- in every one of those the connection is
   finished, because half a frame cannot be recovered from. */
static bool recv_exact(int fd, uint8_t *buf, size_t len)
{
    /* The socket timeout bounds each call, but not the sequence: a peer that
       sends one byte just inside it, over and over, would hold this task for
       as long as it liked. The deadline covers the whole frame. */
    int64_t deadline = esp_timer_get_time() + (int64_t)FRAME_WAIT_MS * 1000;
    size_t  got = 0;

    while (got < len) {
        if (esp_timer_get_time() > deadline) return false;
        int n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static void close_slot(int i)
{
    if (s_conn[i].fd >= 0) close(s_conn[i].fd);
    s_conn[i].fd    = -1;
    s_conn[i].state = SLOT_FREE;
}

/* Answers without going near a worker. Used when the pool is saturated: the
   client is told to retry rather than left waiting for a slot. */
static void answer_busy(int fd, const uint8_t *frame)
{
    uint8_t out[MBAP_LEN + 2];
    memcpy(out, frame, 4);
    out[4] = 0; out[5] = 3;
    out[6] = frame[6];
    out[7] = (uint8_t)(frame[MBAP_LEN] | 0x80u);
    out[8] = MB_EXC_DEVICE_BUSY;
    (void)send_all(fd, out, sizeof(out));
}

static void accept_one(void)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;

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

    struct timeval tv = { .tv_sec = FRAME_WAIT_MS / 1000,
                          .tv_usec = (FRAME_WAIT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_conn[slot].fd    = fd;
    s_conn[slot].state = SLOT_IDLE;
    s_stats.accepted++;
    ESP_LOGD(TAG, "connection on slot %d", slot);
}

/* Reads one whole frame from a readable connection and hands it to the pool.
 *
 * The header says how long the rest is, so once anything has arrived the
 * frame is read to its end here rather than being accumulated across polls.
 * On a local segment the remainder is already in the socket or one packet
 * behind it; a client that cannot manage that within FRAME_WAIT_MS has its
 * connection closed, which is also what keeps a stalled peer from holding up
 * the poll loop for longer than that. */
static void serve_slot(int i, job_t *job)
{
    int fd = s_conn[i].fd;

    int n = recv(fd, job->frame, MBAP_HDR, 0);
    if (n <= 0) { close_slot(i); return; }          /* closed or timed out */
    if (n < MBAP_HDR && !recv_exact(fd, job->frame + n, MBAP_HDR - n)) {
        close_slot(i); return;
    }

    uint16_t pid = be16(&job->frame[2]);
    uint16_t len = be16(&job->frame[4]);            /* unit id plus PDU */

    if (pid != 0 || len < 2 || len > (uint16_t)(1 + MB_PDU_MAX)) {
        s_stats.malformed++;
        ESP_LOGW(TAG, "slot %d: protocol id %u, length %u — closing", i, pid, len);
        close_slot(i);
        return;
    }

    if (!recv_exact(fd, &job->frame[MBAP_HDR], len)) { close_slot(i); return; }
    job->len  = (uint16_t)(MBAP_HDR + len);
    job->slot = i;

    /* A request this board answers itself reads cached state and queues any
       write -- about a millisecond, with nothing in it that can block for
       long. Doing it here rather than through the pool means the segment can
       never delay it, and no number of clients waiting on a dead address can
       take the last worker away from it. */
    if (is_local(job->frame[6])) {
        uint8_t  out[MBAP_LEN + MB_PDU_MAX];
        uint16_t out_len = process(job->frame, job->len, out);
        if (!send_all(fd, out, out_len)) close_slot(i);
        return;
    }

    s_conn[i].state = SLOT_BUSY;
    if (xQueueSend(s_jobs, job, 0) != pdTRUE) {
        /* Every worker is on the segment already. Rather than queue without
           bound, say so: "slave device busy" is the one answer a client knows
           to retry. */
        s_stats.overloaded++;
        answer_busy(fd, job->frame);
        s_conn[i].state = SLOT_IDLE;
    }
}

static void poller_task(void *arg)
{
    (void)arg;
    job_t *job = malloc(sizeof(job_t));
    configASSERT(job);

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_listen_fd, &rd);
        FD_SET(s_wake_fd, &rd);
        int maxfd = (s_listen_fd > s_wake_fd) ? s_listen_fd : s_wake_fd;

        for (int i = 0; i < MB_TCP_MAX_CONN; i++) {
            if (s_conn[i].state == SLOT_IDLE) {
                FD_SET(s_conn[i].fd, &rd);
                if (s_conn[i].fd > maxfd) maxfd = s_conn[i].fd;
            } else if (s_conn[i].state == SLOT_DEAD) {
                close_slot(i);                       /* a worker could not answer */
            }
        }

        /* No timeout: the only things that can change are a packet, a new
           connection, or a worker finishing -- and all three are in the set. */
        int ready = select(maxfd + 1, &rd, NULL, NULL, NULL);
        if (ready <= 0) continue;

        if (FD_ISSET(s_wake_fd, &rd)) {
            uint64_t v;
            (void)read(s_wake_fd, &v, sizeof(v));    /* clears the counter */
        }
        if (FD_ISSET(s_listen_fd, &rd)) accept_one();

        for (int i = 0; i < MB_TCP_MAX_CONN; i++)
            if (s_conn[i].state == SLOT_IDLE && FD_ISSET(s_conn[i].fd, &rd))
                serve_slot(i, job);

        check_stack("poller");
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t mb_tcp_server_start(uint16_t port, uint8_t local_uid)
{
    if (s_listen_fd >= 0) return ESP_OK;             /* a second interface came up */

    s_local_uid = local_uid;

    for (int i = 0; i < MB_TCP_MAX_CONN; i++) { s_conn[i].fd = -1; s_conn[i].state = SLOT_FREE; }

    s_jobs = xQueueCreate(MB_TCP_JOB_DEPTH, sizeof(job_t));
    ESP_RETURN_ON_FALSE(s_jobs, ESP_ERR_NO_MEM, TAG, "job queue");

    /* select() cannot see a worker finish, so one eventfd stands in for all
       of them. Registering twice is not an error anyone else's fault. */
    esp_vfs_eventfd_config_t efd_cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t efd_err = esp_vfs_eventfd_register(&efd_cfg);
    if (efd_err != ESP_OK && efd_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "eventfd vfs: %s", esp_err_to_name(efd_err));
        return efd_err;
    }
    s_wake_fd = eventfd(0, 0);
    ESP_RETURN_ON_FALSE(s_wake_fd >= 0, ESP_FAIL, TAG, "eventfd: errno %d", errno);

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "socket: errno %d", errno);

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind port %u: errno %d", port, errno);
        close(fd);
        return ESP_FAIL;
    }
    if (listen(fd, MB_TCP_MAX_CONN) < 0) {
        ESP_LOGE(TAG, "listen: errno %d", errno);
        close(fd);
        return ESP_FAIL;
    }
    s_listen_fd = fd;

    for (int i = 0; i < MB_TCP_WORKERS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "mb_tcp_w%d", i);
        ESP_RETURN_ON_FALSE(
            xTaskCreate(worker_task, name, 5120, NULL, 5, NULL) == pdPASS,
            ESP_ERR_NO_MEM, TAG, "worker task");
    }
    ESP_RETURN_ON_FALSE(
        xTaskCreate(poller_task, "mb_tcp_poll", 5120, NULL, 6, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "poller task");

    ESP_LOGI(TAG, "listening on port %u — %d connections, %d workers",
             port, MB_TCP_MAX_CONN, MB_TCP_WORKERS);
    return ESP_OK;
}
