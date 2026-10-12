#include "can_server.h"
#include "app_config.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "buzzer.h"

#include <string.h>
#include <inttypes.h>

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/idf_additions.h"

#define TAG          "can"
#define CAN_TX_GPIO  GPIO_NUM_2
#define CAN_RX_GPIO  GPIO_NUM_3
#define TX_POLL_MS   50
#define TX_FAIL_LOG_MS 10000   /* do not repeat a transmit failure more often */
/* Do not repeat the bus-off warning more often than this. A node alone on the
   bus cycles through bus-off indefinitely; the log must stay readable. */
#define BUS_OFF_LOG_MS 10000
#define HB_PERIOD_MS 1000        /* Basic mode heartbeat */
/* PGN 126993 goes out every 60 s, the NMEA 2000 output period (vendor PGN
   references give 60000 ms); every 1 s was 60 times that. */
#define N2K_HB_PERIOD_MS 60000u
#define RX_QUEUE_DEPTH 32

#define CAN_MODE_BASIC  1
#define CAN_MODE_N2K    2

/* ---------------------------------------------------------------- shared */

static twai_node_handle_t s_node = NULL;
static uint8_t            s_mode;
static QueueHandle_t      s_rx_q;
static can_server_hooks_t s_hooks;   /* what the firmware lets CAN tell it */

/* A control frame was acted on: the rule engine's command-health feed in
   the full firmware. */
static void command_seen(void)
{
    if (s_hooks.on_command) s_hooks.on_command();
}

typedef struct { uint32_t id; uint8_t ide; uint8_t dlc; uint8_t data[8]; } rx_msg_t;

static IRAM_ATTR bool rx_done_cb(twai_node_handle_t node,
                                  const twai_rx_done_event_data_t *edata,
                                  void *ctx)
{
    uint8_t buf[8] = {0};
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) return false;
    /* A remote-transmission-request frame carries no data. The HAL parses its
       DLC but skips the data copy, so buf would keep whatever the ISR stack
       held and basic_handle_rx() would drive the relay bank from it. */
    if (f.header.rtr) return false;
    rx_msg_t m = { .id = f.header.id, .ide = f.header.ide,
                   .dlc = f.header.dlc > 8 ? 8 : f.header.dlc };
    memcpy(m.data, buf, m.dlc);
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rx_q, &m, &woken);
    return woken;
}

/* ================================================================ BASIC MODE */

static uint16_t s_base;

/* The output state last sent, in either mode. The senders set it, so a
   change the worker has not seen yet -- an output switched over Modbus,
   MQTT, a rule or the web page -- is told apart from one a CAN command
   already answered. s_do_sent: whether it holds anything yet. A start value
   of 0xFF instead was a state like any other -- with all eight outputs on
   when CAN started (the Matter firmware starts it after commissioning) the
   first report never went out. */
static uint8_t s_last_do;
static bool    s_do_sent;

/* The driver does not copy a frame: it keeps a pointer to the twai_frame_t and
   to its buffer in p_curr_tx and in tx_mount_queue until the transmission
   finishes. Frames therefore live here rather than on the sender's stack, and
   a slot is reused only once the driver has reported that frame done.

   The table is one slot deeper than the driver's TX queue, because the driver
   can hold tx_queue_depth queued frames plus the one in flight. A free-running
   ring would not be enough even at that size: on a bus that makes no progress
   the index wraps while frames are still pending, and overwriting a pending
   frame changes what later goes on the wire.

   If every slot is in flight the frame is dropped and reported. That is the
   safe direction: losing a status frame on a stalled bus is recoverable,
   corrupting one that is already queued is not.

   Known limitation: frames still sitting in the driver's queue when the node
   goes bus-off are not dequeued and produce no completion callback, so their
   slots stay taken until the node recovers. Transmission then reports
   ESP_ERR_NO_MEM rather than going quiet. */
#define TX_QUEUE_DEPTH 16
#define TX_SLOTS       (TX_QUEUE_DEPTH + 1)

static struct {
    twai_frame_t  f;
    uint8_t       buf[8];
    volatile bool busy;
} s_tx[TX_SLOTS];

static twai_frame_t *tx_claim(const uint8_t *data, uint8_t dlc, uint8_t fill)
{
    for (int i = 0; i < TX_SLOTS; i++) {
        if (s_tx[i].busy) continue;
        s_tx[i].busy = true;
        memset(s_tx[i].buf, fill, sizeof(s_tx[i].buf));
        if (dlc && data) memcpy(s_tx[i].buf, data, dlc);
        s_tx[i].f.buffer     = s_tx[i].buf;
        s_tx[i].f.buffer_len = dlc;
        return &s_tx[i].f;
    }
    return NULL;
}

static void tx_release(const twai_frame_t *f)
{
    for (int i = 0; i < TX_SLOTS; i++)
        if (&s_tx[i].f == f) { s_tx[i].busy = false; return; }
}

/* Fires for every frame the hardware took, successful or not. */
static IRAM_ATTR bool tx_done_cb(twai_node_handle_t node,
                                 const twai_tx_done_event_data_t *edata, void *ctx)
{
    (void)node; (void)ctx;
    tx_release(edata->done_tx_frame);
    return false;
}

/* A rejected transmit used to vanish: basic_send() discarded the result and
   n2k_send_frame() logged at DEBUG, which is below the configured level. That
   is how an every-frame failure stayed invisible. Rate-limited so a
   disconnected bus cannot flood the log. */
static void tx_failed(uint32_t id, esp_err_t err)
{
    static TickType_t last;
    static uint32_t   suppressed;
    TickType_t now = xTaskGetTickCount();

    if (last == 0 || (now - last) >= pdMS_TO_TICKS(TX_FAIL_LOG_MS)) {
        if (suppressed)
            ESP_LOGW(TAG, "%" PRIu32 " further transmits failed", suppressed);
        ESP_LOGW(TAG, "TX %03" PRIx32 " failed: %s", id, esp_err_to_name(err));
        last = now; suppressed = 0;
    } else {
        suppressed++;
    }
}

/* Whether the driver took the frame. A state report that was not taken is
   sent again on the next tick -- see can_tick(). */
static bool basic_send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;
    twai_frame_t *f = tx_claim(data, dlc, 0x00);
    if (!f) { tx_failed(id, ESP_ERR_NO_MEM); return false; }
    /* buffer_len must describe the payload, not the buffer: the driver rejects
       the frame unless header.dlc == twaifd_len2dlc(buffer_len). */
    f->header = (twai_frame_header_t){ .id = id, .dlc = dlc, .ide = 0, .rtr = 0 };
    f->buffer_len = dlc;
    esp_err_t r = twai_node_transmit(s_node, f, 5);
    if (r != ESP_OK) { tx_release(f); tx_failed(id, r); return false; }
    return true;
}

static void basic_tx_heartbeat(void)
{
    uint8_t s = (s_hooks.online && s_hooks.online()) ? 0x03 : 0x01;
    basic_send(s_base + 0, &s, 1);
}

static bool basic_tx_di(uint8_t bits) { return basic_send(s_base + 1, &bits, 1); }

/* s_last_do only once the frame is taken: set before, a failed send counted
   as sent and, without a periodic interval, the new state never went out. */
static void basic_tx_do(void)
{
    uint8_t f[2] = {0, dout_get_all()};
    if (basic_send(s_base + 2, f, 2)) { s_last_do = f[1]; s_do_sent = true; }
}

/* DO opcodes: 0=WRITE, 1=SET, 2=CLEAR, 3=TOGGLE. False for any other:
   nothing done, and not a command -- see basic_handle_rx(). */
static bool basic_apply_do(uint8_t op, uint8_t mask)
{
    /* One atomic change rather than a read followed by eight single-bit
       writes: dout_set() rewrites the whole port each time, so the outputs
       used to step through the intermediate patterns, the state could move
       between the read and the write, and a transfer failing halfway left
       some channels switched and others not. */
    esp_err_t ret;
    switch (op) {
    case 0: ret = dout_modify(mask, (uint8_t)~mask, 0u); break;   /* WRITE  */
    case 1: ret = dout_modify(mask, 0u, 0u);             break;   /* SET    */
    case 2: ret = dout_modify(0u, mask, 0u);             break;   /* CLEAR  */
    case 3: ret = dout_modify(0u, 0u, mask);             break;   /* TOGGLE */
    default: return false;
    }
    if (ret != ESP_OK) ESP_LOGW(TAG, "DO command failed: %s", esp_err_to_name(ret));
    basic_tx_do();
    return true;
}

/* command_seen() only for a frame that was a command and was carried out:
   it feeds the rules' can(ms) watchdog, and a stream of frames with an
   unknown opcode kept a fallback from ever starting although no command
   was being executed. */
static void basic_handle_rx(const rx_msg_t *m)
{
    if (m->id == (uint16_t)(s_base + 2) && m->dlc == 2) {
        if (basic_apply_do(m->data[0], m->data[1])) command_seen();
    } else if (m->id == (uint16_t)(s_base + 3) && m->dlc == 3) {
        led_set_rgb(m->data[0], m->data[1], m->data[2]);
        command_seen();
    } else if (m->id == (uint16_t)(s_base + 4) && (m->dlc == 2 || m->dlc == 3)) {
        uint16_t f = (uint16_t)(m->data[0] | (m->data[1] << 8));
        uint32_t d = (m->dlc == 3) ? (uint32_t)m->data[2] * 10u : 200u;
        if (f > 0) { buzzer_beep_once(f, d); command_seen(); }
    } else if (m->id == (uint16_t)(s_base + 5) && m->dlc == 0) {
        uint8_t di = 0;
        for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
        basic_tx_heartbeat(); basic_tx_di(di); basic_tx_do();
    }
}

/* ================================================================ NMEA2000 */

/* ---- PGN constants ---- */
#define N2K_PGN_ADDRESS_CLAIM  60928UL  /* 0x0EE00 — PDU1 */
#define N2K_PGN_ISO_REQUEST    59904UL  /* 0x0EA00 — PDU1 */
#define N2K_PGN_HEARTBEAT     126993UL  /* 0x1F011 — PDU2, single-frame */
#define N2K_PGN_SW_STATUS     127501UL  /* 0x1F20D — PDU2, single-frame */
#define N2K_PGN_SW_CONTROL    127502UL  /* 0x1F20E — PDU2, single-frame */
#define N2K_PGN_PROPRIETARY   126720UL  /* 0x1EF00 — PDU1 broadcast, fast-packet */

#define N2K_MFR_CODE  0x7FFU   /* development/unregistered */
#define N2K_ADDR_NULL 0xFE
#define N2K_ADDR_GLOBAL 0xFF

#define N2K_DI_INSTANCE  0
#define N2K_DO_INSTANCE  1

/* ---- Address-claiming state ---- */
typedef enum { AC_UNINIT, AC_CLAIMING, AC_ACTIVE, AC_FAILED } ac_state_t;

static ac_state_t s_ac_state;
static uint8_t    s_addr;         /* current claimed address */
static uint8_t    s_name[8];      /* our 64-bit NAME */
static TickType_t s_claim_tick;
static uint8_t    s_fp_seq;       /* fast-packet sequence counter */
static uint8_t    s_hb_seq;       /* heartbeat sequence */

/* ---- CAN ID helpers ---- */

/* Encode a NMEA2000 29-bit CAN ID.
   PDU1 (PF < 0xF0): dst goes into PS field.
   PDU2 (PF >= 0xF0): PS from PGN goes into ID; dst ignored. */
static uint32_t n2k_make_id(uint32_t pgn, uint8_t pri, uint8_t src, uint8_t dst)
{
    uint8_t dp  = (pgn >> 16) & 0x01;
    uint8_t pf  = (pgn >>  8) & 0xFF;
    uint8_t ps  =  pgn        & 0xFF;
    return ((uint32_t)(pri & 7) << 26)
         | ((uint32_t)dp << 24)
         | ((uint32_t)pf << 16)
         | ((uint32_t)(pf < 0xF0 ? dst : ps) << 8)
         | src;
}

/* Decode a 29-bit CAN ID into PGN and source address. */
static uint32_t n2k_decode_id(uint32_t can_id, uint8_t *src, uint8_t *dst)
{
    *src = can_id & 0xFF;
    uint8_t pf  = (can_id >> 16) & 0xFF;
    uint8_t ps  = (can_id >>  8) & 0xFF;
    uint8_t dp  = (can_id >> 24) & 0x01;
    *dst = (pf < 0xF0) ? ps : N2K_ADDR_GLOBAL;
    return ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | (pf >= 0xF0 ? ps : 0);
}

/* ---- Frame send ---- */

static bool n2k_send_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;
    twai_frame_t *f = tx_claim(data, dlc, 0xFF);   /* see basic_send() */
    if (!f) { tx_failed(can_id, ESP_ERR_NO_MEM); return false; }
    f->header = (twai_frame_header_t){ .id = can_id, .dlc = dlc, .ide = 1, .rtr = 0 };
    f->buffer_len = dlc;
    esp_err_t r = twai_node_transmit(s_node, f, 5);
    if (r != ESP_OK) { tx_release(f); tx_failed(can_id, r); return false; }
    return true;
}

/* Fast-packet sender: handles messages > 8 bytes. */
static void n2k_send_fp(uint32_t pgn, uint8_t pri, uint8_t dst,
                         const uint8_t *payload, uint8_t len)
{
    uint32_t can_id = n2k_make_id(pgn, pri, s_addr, dst);
    uint8_t  seq    = (s_fp_seq++ & 0x07) << 5;
    uint8_t  offset = 0, frame = 0;
    while (offset < len) {
        uint8_t buf[8];
        memset(buf, 0xFF, 8);
        buf[0] = seq | (frame & 0x1F);
        int hdr = 1;
        if (frame == 0) { buf[1] = len; hdr = 2; }
        uint8_t cap = 8 - hdr;
        uint8_t copy = (len - offset) < cap ? (len - offset) : cap;
        memcpy(buf + hdr, payload + offset, copy);
        n2k_send_frame(can_id, buf, 8);
        offset += copy;
        frame++;
    }
}

/* ---- NAME field ---- */

static void n2k_build_name(uint8_t name[8])
{
    /* The low 21 bits of the base MAC as identity number. The OUI and the
       top bits are what a batch shares; the low bits are what tells chips
       apart -- Espressif hands each chip a block of four addresses, so
       neighbours differ in exactly those. An earlier derivation dropped the
       three lowest bits and gave such neighbours the same NAME. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t identity = (((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5]) & 0x1FFFFF;

    uint16_t mfr = N2K_MFR_CODE & 0x7FF;
    /* Byte 0-1: identity[15:0] */
    name[0] = identity & 0xFF;
    name[1] = (identity >> 8) & 0xFF;
    /* Byte 2: identity[20:16] | mfr[2:0]<<5 */
    name[2] = ((identity >> 16) & 0x1F) | ((mfr & 0x07) << 5);
    /* Byte 3: mfr[10:3] */
    name[3] = (mfr >> 3) & 0xFF;
    /* Byte 4: ECU instance(3) | function instance(5) */
    name[4] = 0x00;
    /* Byte 5: function code = 0x80 (I/O Gateway) */
    name[5] = 0x80;
    /* Byte 6: reserved(1) | device class 0x19(7 bits) */
    name[6] = (0x19 & 0x7F) << 1;
    /* Byte 7: system instance(4) | industry group 4=Marine(3) | arb_addr_capable(1) */
    name[7] = (0 << 0) | (4 << 4) | (1 << 7);
}

/* ---- PGN senders ---- */

static void n2k_send_address_claim(uint8_t addr)
{
    uint32_t id = n2k_make_id(N2K_PGN_ADDRESS_CLAIM, 6, addr, N2K_ADDR_GLOBAL);
    n2k_send_frame(id, s_name, 8);
}

/* PGN 126993 is a single 8-byte frame (canboat: PACKET_SINGLE; the NMEA 2000
   heartbeat corrigendum): transmit interval in ms, sequence counter, then
   controller/equipment state bits and reserved. It went out as a 9-byte
   fast-packet with the interval in 0.01 s, so receivers read the fast-packet
   header as the interval -- and even unpacked, 100 meant 100 ms, not 1 s. */
static void n2k_tx_heartbeat(void)
{
    uint8_t payload[8];
    uint16_t interval_ms = N2K_HB_PERIOD_MS;
    payload[0] = interval_ms & 0xFF;
    payload[1] = (interval_ms >> 8) & 0xFF;
    payload[2] = s_hb_seq;
    s_hb_seq = s_hb_seq >= 252 ? 0 : s_hb_seq + 1;   /* 253..255: reserved, error, n/a */
    memset(payload + 3, 0xFF, 5);     /* controller/equipment state n/a, reserved */
    n2k_send_frame(n2k_make_id(N2K_PGN_HEARTBEAT, 7, s_addr, N2K_ADDR_GLOBAL), payload, sizeof(payload));
}

/* Encode 8 switch states (bitmask) into PGN 127501/127502 data. */
static void encode_switch_bank(uint8_t data[8], uint8_t instance, uint8_t bitmask)
{
    data[0] = instance;
    data[1] = data[2] = 0;
    for (int sw = 0; sw < 8; sw++) {
        uint8_t state = (bitmask >> sw) & 1;   /* 0=off, 1=on */
        int byte_pos  = 1 + sw / 4;
        int bit_pos   = (sw % 4) * 2;
        data[byte_pos] |= (state & 0x3) << bit_pos;
    }
    memset(data + 3, 0xFF, 5);    /* switches 9-28: not available */
}

static bool n2k_tx_di_bank(void)
{
    uint8_t bitmask = 0;
    for (int i = 0; i < 8; i++) if (di_get(i)) bitmask |= (uint8_t)(1u << i);
    uint8_t data[8];
    encode_switch_bank(data, N2K_DI_INSTANCE, bitmask);
    return n2k_send_frame(n2k_make_id(N2K_PGN_SW_STATUS, 3, s_addr, N2K_ADDR_GLOBAL), data, 8);
}

static void n2k_tx_do_bank(void)
{
    uint8_t bitmask = dout_get_all();
    uint8_t data[8];
    encode_switch_bank(data, N2K_DO_INSTANCE, bitmask);
    if (n2k_send_frame(n2k_make_id(N2K_PGN_SW_STATUS, 3, s_addr, N2K_ADDR_GLOBAL), data, 8))
        { s_last_do = bitmask; s_do_sent = true; }   /* see basic_tx_do() */
}

/* Proprietary fast-packet for LED or buzzer. */
static void n2k_tx_proprietary(uint8_t sub_fn, const uint8_t *args, uint8_t args_len)
{
    /* Payload: [mfr_lo][mfr_hi][sub_fn][args...] */
    uint8_t payload[8];
    payload[0] = N2K_MFR_CODE & 0xFF;
    payload[1] = (N2K_MFR_CODE >> 8) & 0xFF;
    payload[2] = sub_fn;
    if (args_len > 5) args_len = 5;
    memcpy(payload + 3, args, args_len);
    uint8_t total = 3 + args_len;
    n2k_send_fp(N2K_PGN_PROPRIETARY, 6, N2K_ADDR_GLOBAL, payload, total);
}

/* ---- RX handler ---- */

static void n2k_handle_rx(const rx_msg_t *m)
{
    uint8_t src, dst;
    uint32_t pgn = n2k_decode_id(m->id, &src, &dst);

    /* Address claims come first, before the "not ours" filter below. A claim
       that conflicts with us is by definition sent from our own address, so
       the filter used to discard exactly the frames the conflict branch was
       looking for and that branch could never run: two devices on the default
       address both kept it and neither backed off. Claims are also handled
       while we are still claiming, not only once active -- that is the window
       in which a collision is most likely. */
    if (pgn == N2K_PGN_ADDRESS_CLAIM && m->dlc == 8) {
        if (src != s_addr) return;                 /* somebody else's address */
        if (s_ac_state != AC_ACTIVE && s_ac_state != AC_CLAIMING) return;

        uint64_t their_name, our_name;
        memcpy(&their_name, m->data, 8);
        memcpy(&our_name,   s_name,  8);

        /* ISO 11783-5: the lower NAME wins the address. An equal NAME has no
           winner, and two devices that both re-assert never settle; moving is
           the one way out of that. */
        if (our_name >= their_name) {
            s_addr++;
            if (s_addr > 251) {
                /* ISO 11783-5: a node that cannot claim says so once, as an
                   address claim from the null address, and from then on
                   sends nothing else. It used to stay on 252 and answer
                   requests from there. */
                s_addr     = N2K_ADDR_NULL;
                s_ac_state = AC_FAILED;
                ESP_LOGW(TAG, "N2k: no free address left, giving up");
                n2k_send_address_claim(N2K_ADDR_NULL);
                return;
            }
            s_ac_state   = AC_CLAIMING;
            s_claim_tick = xTaskGetTickCount();
            ESP_LOGW(TAG, "N2k: address conflict, moving to %u", s_addr);
            n2k_send_address_claim(s_addr);
        } else {
            /* Our NAME is lower, so the address stays ours. Re-assert it so the
               other device knows to move. */
            n2k_send_address_claim(s_addr);
        }
        return;
    }

    /* Skip our own frames. */
    if (src == s_addr) return;

    /* Only accept frames addressed to us or global. */
    if (dst != N2K_ADDR_GLOBAL && dst != s_addr) return;

    if (pgn == N2K_PGN_ISO_REQUEST && m->dlc == 3) {
        uint32_t requested = (uint32_t)m->data[0]
                           | ((uint32_t)m->data[1] << 8)
                           | ((uint32_t)m->data[2] << 16);
        /* A claim is answered in any state: from 254 once claiming failed
           ("cannot claim"). Data only with an address of our own -- not
           while the claim is still open, and not after it failed. */
        if (requested == N2K_PGN_ADDRESS_CLAIM)
            n2k_send_address_claim(s_addr);
        else if (requested == N2K_PGN_SW_STATUS && s_ac_state == AC_ACTIVE) {
            n2k_tx_di_bank();
            n2k_tx_do_bank();
        }
        return;
    }

    if (s_ac_state != AC_ACTIVE) return;

    if (pgn == N2K_PGN_SW_CONTROL && m->dlc == 8) {
        /* Only handle bank N2K_DO_INSTANCE */
        if (m->data[0] != N2K_DO_INSTANCE) return;
        /* Collect the whole bank first: a switch-control message addresses up
           to eight channels and they have to change together, not one port
           write at a time. 0x02/0x03 mean "no change" and stay out of both
           masks. */
        uint8_t set = 0, clear = 0;
        for (int sw = 0; sw < 8; sw++) {
            int byte_pos = 1 + sw / 4;
            int bit_pos  = (sw % 4) * 2;
            uint8_t state = (m->data[byte_pos] >> bit_pos) & 0x3;
            if (state > 0x01) continue;
            if (state == 0x01) set |= (uint8_t)(1u << sw);
            else               clear |= (uint8_t)(1u << sw);
        }
        if (set || clear) {
            esp_err_t ret = dout_modify(set, clear, 0u);
            if (ret != ESP_OK)
                ESP_LOGW(TAG, "N2k switch control failed: %s", esp_err_to_name(ret));
        }
        n2k_tx_do_bank();
        command_seen();
        return;
    }

    if (pgn == N2K_PGN_PROPRIETARY && m->dlc == 8) {
        /* Only single-frame fast-packet messages are supported. The length
         * counts the manufacturer code (2), the sub-function (1) and the
         * arguments: LED 3 -> 6, buzzer 2 or 3 -> 5 or 6. Both fit in frame
         * 0 (6 payload bytes). Exactly those lengths: a first frame that
         * announces more (100) is part of a multi-frame message, and its LED
         * bytes were carried out although the message was never whole. */
        uint8_t fp_frame = m->data[0] & 0x1F;
        if (fp_frame != 0) return;   /* only first frame */
        /* data[1]=total len, data[2]=mfr_lo, data[3]=mfr_hi, data[4]=sub_fn ... */
        uint16_t mfr = (uint16_t)m->data[2] | ((uint16_t)m->data[3] << 8);
        /* manufacturer code (11 bits), reserved (2), industry group (3):
           a standard stack sends the reserved bits set and industry 4. */
        if ((mfr & 0x7FF) != N2K_MFR_CODE) return;
        uint8_t sub_fn = m->data[4];
        if (sub_fn == 0x01 && m->data[1] == 6) {          /* LED */
            led_set_rgb(m->data[5], m->data[6], m->data[7]);
            command_seen();
        } else if (sub_fn == 0x02 && (m->data[1] == 5 || m->data[1] == 6)) {   /* Buzzer */
            /* The duration is the third argument, length 6. It was read
               only from 7 on, so every documented command beeped 200 ms,
               and one without the duration (length 5) was dropped. */
            uint16_t freq = (uint16_t)m->data[5] | ((uint16_t)m->data[6] << 8);
            uint32_t dur  = (m->data[1] == 6) ? (uint32_t)m->data[7] * 10u : 200u;
            if (freq > 0) { buzzer_beep_once(freq, dur); command_seen(); }
        }
    }
}

/* ================================================================ worker task */

/* The node stays in bus-off until recovery is started explicitly: after a
   shorted or unterminated bus, or a bitrate mismatch, twai_node_transmit()
   fails forever and RX is dead too. Nothing else watches for that — neither
   sender checks the return value — so CAN would be gone until the next reboot
   with nothing in the log. Polled here, where the worker already wakes every
   TX_POLL_MS; reading the status is a register access. */
static void can_check_bus_off(void)
{
    static bool       recovering;
    static TickType_t last_log;
    static uint32_t   suppressed;

    twai_node_status_t st;
    if (!s_node || twai_node_get_info(s_node, &st, NULL) != ESP_OK) return;

    if (st.state != TWAI_ERROR_BUS_OFF) {
        recovering = false;
        return;
    }
    if (recovering) return;   /* recovery already under way */

    TickType_t now = xTaskGetTickCount();
    if (last_log == 0 || (now - last_log) >= pdMS_TO_TICKS(BUS_OFF_LOG_MS)) {
        if (suppressed)
            ESP_LOGW(TAG, "bus-off repeated %" PRIu32 " more times", suppressed);
        ESP_LOGW(TAG, "bus-off (TEC=%u) — starting recovery", st.tx_error_count);
        last_log = now; suppressed = 0;
    } else {
        suppressed++;
    }
    if (twai_node_recover(s_node) == ESP_OK) recovering = true;
}

/* The periodic part of the worker: heartbeat, address-claim timeout, and
   inputs and outputs sent on change and every tx_interval_ms. Outputs used
   to go out only after a CAN command or a request: one switched over
   Modbus, MQTT, a rule or the web page stayed at its old state on the bus. */
static void can_tick(TickType_t now)
{
    /* di_sent as s_do_sent: 0xFF is eight inputs high, not "nothing sent",
       and with no periodic interval that state was never reported. */
    static uint8_t    last_di;
    static bool       di_sent;
    static TickType_t last_hb  = 0;
    static TickType_t last_per = 0;
    /* At least one tick: at 100 Hz, 1..9 ms is 0 ticks, and every pass of
       the loop -- one per received frame -- sent the inputs again. */
    const uint16_t   interval = app_config_get()->can.tx_interval_ms;
    const TickType_t period   = interval == 0 ? 0
                              : (pdMS_TO_TICKS(interval) ? pdMS_TO_TICKS(interval) : 1);

    uint8_t di = 0;
    for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
    bool due = period && (now - last_per) >= period;

    /* ---- Basic mode ---- */
    if (s_mode == CAN_MODE_BASIC) {
        if ((now - last_hb) >= pdMS_TO_TICKS(HB_PERIOD_MS)) {
            basic_tx_heartbeat(); last_hb = now;
        }
        /* A report the driver did not take stays due: last_di and
           s_last_do move only with a frame that went out, so the next tick
           tries again (every TX_POLL_MS; tx_failed() keeps the log quiet). */
        if ((!di_sent || di != last_di || due) && basic_tx_di(di)) { last_di = di; di_sent = true; }
        if (!s_do_sent || dout_get_all() != s_last_do || due) basic_tx_do();
        if (due) last_per = now;
    }

    /* ---- NMEA2000 mode ---- */
    if (s_mode == CAN_MODE_N2K) {
        /* Address claiming */
        if (s_ac_state == AC_CLAIMING &&
            (now - s_claim_tick) >= pdMS_TO_TICKS(250)) {
            s_ac_state = AC_ACTIVE;
            ESP_LOGI(TAG, "N2k address %u claimed", s_addr);
            n2k_tx_heartbeat();
            bool sent = n2k_tx_di_bank();
            n2k_tx_do_bank();
            if (sent) { last_di = di; di_sent = true; }
            last_hb = last_per = now;
            due = false;
        }

        if (s_ac_state == AC_ACTIVE) {
            if ((now - last_hb) >= pdMS_TO_TICKS(N2K_HB_PERIOD_MS)) {
                n2k_tx_heartbeat(); last_hb = now;
            }
            if ((!di_sent || di != last_di || due) && n2k_tx_di_bank()) { last_di = di; di_sent = true; }
            if (!s_do_sent || dout_get_all() != s_last_do || due) n2k_tx_do_bank();
            if (due) last_per = now;
        }
    }
}

static void can_worker_task(void *arg)
{
    for (;;) {
        rx_msg_t m;
        if (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(TX_POLL_MS)) == pdTRUE) {
            if (s_mode == CAN_MODE_BASIC && !m.ide) basic_handle_rx(&m);
            else if (s_mode == CAN_MODE_N2K &&  m.ide) n2k_handle_rx(&m);
        }

        can_check_bus_off();
        can_tick(xTaskGetTickCount());
    }
}

/* ================================================================ public */

bool can_server_running(void)
{
    return s_node != NULL;
}

esp_err_t can_server_init(const can_server_hooks_t *hooks)
{
    const app_config_t *cfg = app_config_get();
    if (hooks) s_hooks = *hooks;
    s_mode = cfg->can.mode;
    if (s_mode == 0) { ESP_LOGI(TAG, "CAN disabled"); return ESP_OK; }

    uint32_t bitrate = (s_mode == CAN_MODE_N2K) ? 250000 : cfg->can.bitrate;

    twai_timing_basic_config_t timing = { .bitrate = bitrate };
    twai_onchip_node_config_t node_cfg = {
        .io_cfg.tx              = CAN_TX_GPIO,
        .io_cfg.rx              = CAN_RX_GPIO,
        .io_cfg.quanta_clk_out  = GPIO_NUM_NC,
        .io_cfg.bus_off_indicator = GPIO_NUM_NC,
        .bit_timing             = timing,
        .tx_queue_depth         = TX_QUEUE_DEPTH,
        /* -1 = retransmit on arbitration loss or bus error, which is the
           ordinary CAN behaviour. Left at its 0 default the HAL arms
           single-shot transmission (twai_hal_v1.c: .ss = retry_cnt != -1), so
           a frame lost to a higher-priority node was simply dropped — and no
           sender here looks at the return value to notice. */
        .fail_retry_cnt         = -1,
    };
    /* Before the node: the receive callback posts here from the ISR, and a
       NULL queue would have been found by the first frame on the bus. */
    if (!s_rx_q) s_rx_q = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_msg_t));
    ESP_RETURN_ON_FALSE(s_rx_q, ESP_ERR_NO_MEM, TAG, "rx queue");
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_cfg, &s_node), TAG, "new node");

    /* From here on a failure deletes the node again: left behind, it held
       the controller and its pins, and an enabled node with no worker task
       to empty the queue answered nothing. */
    esp_err_t err;
    const char *step;
    twai_event_callbacks_t cbs = { .on_rx_done = rx_done_cb, .on_tx_done = tx_done_cb };
    if ((err = twai_node_register_event_callbacks(s_node, &cbs, NULL)) != ESP_OK) {
        step = "register cbs";
        goto fail;
    }
    if ((err = twai_node_enable(s_node)) != ESP_OK) {
        step = "enable";
        goto fail;
    }

    if (s_mode == CAN_MODE_BASIC) {
        s_base = cfg->can.base_id;
        ESP_LOGI(TAG, "Basic mode — base_id=0x%03x bitrate=%"PRIu32, s_base, bitrate);
    } else {
        s_addr = cfg->can.n2k_addr;
        n2k_build_name(s_name);
        s_ac_state = AC_CLAIMING;
        s_claim_tick = xTaskGetTickCount();
        n2k_send_address_claim(s_addr);
        ESP_LOGI(TAG, "N2k mode — preferred addr=0x%02x mfr=0x%03x", s_addr, N2K_MFR_CODE);
    }

    /* The worker touches neither the flash nor an ISR path, so its stack can
       live in PSRAM where the firmware is short of internal RAM. */
    BaseType_t ok = s_hooks.psram_stack
        ? xTaskCreateWithCaps(can_worker_task, "can", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : xTaskCreate(can_worker_task, "can", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        err = ESP_ERR_NO_MEM; step = "worker task";
        twai_node_disable(s_node);
        goto fail;
    }
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "%s: %s", step, esp_err_to_name(err));
    twai_node_delete(s_node);
    s_node = NULL;
    return err;
}
