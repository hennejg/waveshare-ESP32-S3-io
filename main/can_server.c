#include "can_server.h"
#include "app_config.h"
#include "app_mqtt.h"
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

#define TAG          "can"
#define CAN_TX_GPIO  GPIO_NUM_2
#define CAN_RX_GPIO  GPIO_NUM_3
#define TX_POLL_MS   50
#define HB_PERIOD_MS 1000
#define RX_QUEUE_DEPTH 32

#define CAN_MODE_BASIC  1
#define CAN_MODE_N2K    2

/* ---------------------------------------------------------------- shared */

static twai_node_handle_t s_node = NULL;
static uint8_t            s_mode;
static QueueHandle_t      s_rx_q;

typedef struct { uint32_t id; uint8_t ide; uint8_t dlc; uint8_t data[8]; } rx_msg_t;

static IRAM_ATTR bool rx_done_cb(twai_node_handle_t node,
                                  const twai_rx_done_event_data_t *edata,
                                  void *ctx)
{
    uint8_t buf[8];
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) return false;
    rx_msg_t m = { .id = f.header.id, .ide = f.header.ide,
                   .dlc = f.header.dlc > 8 ? 8 : f.header.dlc };
    memcpy(m.data, buf, m.dlc);
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rx_q, &m, &woken);
    return woken;
}

/* ================================================================ BASIC MODE */

static uint16_t s_base;

static void basic_send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    uint8_t buf[8] = {0};
    if (dlc && data) memcpy(buf, data, dlc);
    twai_frame_t f = {
        .header = { .id = id, .dlc = dlc, .ide = 0, .rtr = 0 },
        .buffer = buf, .buffer_len = sizeof(buf),
    };
    twai_node_transmit(s_node, &f, 5);
}

static void basic_tx_heartbeat(void)
{
    uint8_t s = app_mqtt_is_connected() ? 0x03 : 0x01;
    basic_send(s_base + 0, &s, 1);
}

static void basic_tx_di(uint8_t bits) { basic_send(s_base + 1, &bits, 1); }

static void basic_tx_do(void)
{
    uint8_t f[2] = {0, 0};
    for (int i = 0; i < 8; i++) if (dout_get(i)) f[1] |= (uint8_t)(1u << i);
    basic_send(s_base + 2, f, 2);
}

/* DO opcodes: 0=WRITE, 1=SET, 2=CLEAR, 3=TOGGLE */
static void basic_apply_do(uint8_t op, uint8_t mask)
{
    uint8_t cur = 0;
    for (int i = 0; i < 8; i++) if (dout_get(i)) cur |= (uint8_t)(1u << i);
    uint8_t next;
    switch (op) {
    case 0: next = mask;         break;
    case 1: next = cur |  mask;  break;
    case 2: next = cur & ~mask;  break;
    case 3: next = cur ^  mask;  break;
    default: return;
    }
    for (int i = 0; i < 8; i++) { bool b = (next >> i) & 1; if (b != dout_get(i)) dout_set(i, b); }
    basic_tx_do();
}

static void basic_handle_rx(const rx_msg_t *m)
{
    if (m->id == (uint16_t)(s_base + 2) && m->dlc == 2)
        basic_apply_do(m->data[0], m->data[1]);
    else if (m->id == (uint16_t)(s_base + 3) && m->dlc == 3)
        led_set_rgb(m->data[0], m->data[1], m->data[2]);
    else if (m->id == (uint16_t)(s_base + 4) && m->dlc >= 2) {
        uint16_t f = (uint16_t)(m->data[0] | (m->data[1] << 8));
        uint32_t d = (m->dlc >= 3) ? (uint32_t)m->data[2] * 10u : 200u;
        if (f > 0) buzzer_beep_once(f, d);
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
#define N2K_PGN_HEARTBEAT     126993UL  /* 0x1F011 — PDU2, fast-packet */
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

static void n2k_send_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    uint8_t buf[8];
    memset(buf, 0xFF, 8);
    if (dlc && data) memcpy(buf, data, dlc < 8 ? dlc : 8);
    twai_frame_t f = {
        .header = { .id = can_id, .dlc = dlc < 8 ? dlc : 8, .ide = 1, .rtr = 0 },
        .buffer = buf, .buffer_len = sizeof(buf),
    };
    esp_err_t r = twai_node_transmit(s_node, &f, 5);
    if (r != ESP_OK) ESP_LOGD(TAG, "N2k TX %08"PRIx32": %s", can_id, esp_err_to_name(r));
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
    /* Use last 21 bits of base MAC as identity number. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t identity = ((uint32_t)mac[3] << 13) | ((uint32_t)mac[4] << 5) | (mac[5] >> 3);
    identity &= 0x1FFFFF;

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

static void n2k_tx_heartbeat(void)
{
    uint8_t payload[9];
    uint16_t rate_cs = 100;           /* 1.00 s in 0.01 s units */
    payload[0] = rate_cs & 0xFF;
    payload[1] = (rate_cs >> 8) & 0xFF;
    payload[2] = s_hb_seq++;
    memset(payload + 3, 0xFF, 6);     /* reserved / not available */
    n2k_send_fp(N2K_PGN_HEARTBEAT, 7, N2K_ADDR_GLOBAL, payload, sizeof(payload));
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

static void n2k_tx_di_bank(void)
{
    uint8_t bitmask = 0;
    for (int i = 0; i < 8; i++) if (di_get(i)) bitmask |= (uint8_t)(1u << i);
    uint8_t data[8];
    encode_switch_bank(data, N2K_DI_INSTANCE, bitmask);
    n2k_send_frame(n2k_make_id(N2K_PGN_SW_STATUS, 3, s_addr, N2K_ADDR_GLOBAL), data, 8);
}

static void n2k_tx_do_bank(void)
{
    uint8_t bitmask = 0;
    for (int i = 0; i < 8; i++) if (dout_get(i)) bitmask |= (uint8_t)(1u << i);
    uint8_t data[8];
    encode_switch_bank(data, N2K_DO_INSTANCE, bitmask);
    n2k_send_frame(n2k_make_id(N2K_PGN_SW_STATUS, 3, s_addr, N2K_ADDR_GLOBAL), data, 8);
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

    /* Skip our own frames. */
    if (src == s_addr) return;

    /* Only accept frames addressed to us or global. */
    if (dst != N2K_ADDR_GLOBAL && dst != s_addr) return;

    if (pgn == N2K_PGN_ADDRESS_CLAIM && m->dlc == 8) {
        /* Conflict check: other device claiming same address */
        if (src == s_addr && s_ac_state == AC_ACTIVE) {
            uint64_t their_name, our_name;
            memcpy(&their_name, m->data, 8);
            memcpy(&our_name,   s_name,  8);
            if (our_name > their_name) {
                /* We lose: try next address */
                s_addr++;
                if (s_addr > 251) { s_ac_state = AC_FAILED; return; }
                s_ac_state = AC_CLAIMING;
                s_claim_tick = xTaskGetTickCount();
                n2k_send_address_claim(s_addr);
            }
            /* If our_name < their_name: they should re-address; we stay. */
        }
        return;
    }

    if (pgn == N2K_PGN_ISO_REQUEST && m->dlc == 3) {
        uint32_t requested = (uint32_t)m->data[0]
                           | ((uint32_t)m->data[1] << 8)
                           | ((uint32_t)m->data[2] << 16);
        if (requested == N2K_PGN_ADDRESS_CLAIM)
            n2k_send_address_claim(s_addr);
        else if (requested == N2K_PGN_SW_STATUS) {
            n2k_tx_di_bank();
            n2k_tx_do_bank();
        }
        return;
    }

    if (s_ac_state != AC_ACTIVE) return;

    if (pgn == N2K_PGN_SW_CONTROL && m->dlc == 8) {
        /* Only handle bank N2K_DO_INSTANCE */
        if (m->data[0] != N2K_DO_INSTANCE) return;
        for (int sw = 0; sw < 8; sw++) {
            int byte_pos = 1 + sw / 4;
            int bit_pos  = (sw % 4) * 2;
            uint8_t state = (m->data[byte_pos] >> bit_pos) & 0x3;
            if (state > 0x01) continue;   /* 0x02/0x03 = no-change */
            bool want = (state == 0x01);
            if (want != dout_get(sw)) dout_set(sw, want);
        }
        n2k_tx_do_bank();
        return;
    }

    if (pgn == N2K_PGN_PROPRIETARY && m->dlc == 8) {
        /* Only single-frame fast-packet messages are supported.
         * LED (3 bytes) and buzzer (4 bytes) both fit in frame 0
         * (6 payload bytes available). Multi-frame messages are
         * silently discarded. */
        uint8_t fp_frame = m->data[0] & 0x1F;
        if (fp_frame != 0) return;   /* only first frame */
        /* data[1]=total len, data[2]=mfr_lo, data[3]=mfr_hi, data[4]=sub_fn ... */
        uint16_t mfr = (uint16_t)m->data[2] | ((uint16_t)m->data[3] << 8);
        if (mfr != N2K_MFR_CODE) return;
        uint8_t sub_fn = m->data[4];
        if (sub_fn == 0x01 && m->data[1] >= 6)           /* LED */
            led_set_rgb(m->data[5], m->data[6], m->data[7]);
        else if (sub_fn == 0x02 && m->data[1] >= 6) {    /* Buzzer */
            uint16_t freq = (uint16_t)m->data[5] | ((uint16_t)m->data[6] << 8);
            uint32_t dur  = (m->data[1] >= 7) ? (uint32_t)m->data[7] * 10u : 200u;
            if (freq > 0) buzzer_beep_once(freq, dur);
        }
    }
}

/* ================================================================ worker task */

static void can_worker_task(void *arg)
{
    uint8_t    last_di  = 0xFF;
    TickType_t last_hb  = 0;
    TickType_t last_per = 0;
    uint16_t   interval = app_config_get()->can.tx_interval_ms;

    for (;;) {
        rx_msg_t m;
        if (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(TX_POLL_MS)) == pdTRUE) {
            if (s_mode == CAN_MODE_BASIC && !m.ide) basic_handle_rx(&m);
            else if (s_mode == CAN_MODE_N2K &&  m.ide) n2k_handle_rx(&m);
        }

        TickType_t now = xTaskGetTickCount();

        /* ---- Basic mode ---- */
        if (s_mode == CAN_MODE_BASIC) {
            if ((now - last_hb) >= pdMS_TO_TICKS(HB_PERIOD_MS)) {
                basic_tx_heartbeat(); last_hb = now;
            }
            uint8_t di = 0;
            for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
            if (di != last_di) { basic_tx_di(di); last_di = di; }
            if (interval > 0 && (now - last_per) >= pdMS_TO_TICKS(interval)) {
                basic_tx_di(di); last_per = now;
            }
        }

        /* ---- NMEA2000 mode ---- */
        if (s_mode == CAN_MODE_N2K) {
            /* Address claiming */
            if (s_ac_state == AC_CLAIMING &&
                (now - s_claim_tick) >= pdMS_TO_TICKS(250)) {
                s_ac_state = AC_ACTIVE;
                ESP_LOGI(TAG, "N2k address %u claimed", s_addr);
                n2k_tx_heartbeat(); n2k_tx_di_bank(); n2k_tx_do_bank();
                last_hb = last_per = now;
            }

            if (s_ac_state == AC_ACTIVE) {
                if ((now - last_hb) >= pdMS_TO_TICKS(HB_PERIOD_MS)) {
                    n2k_tx_heartbeat(); last_hb = now;
                }
                uint8_t di = 0;
                for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
                if (di != last_di) { n2k_tx_di_bank(); last_di = di; }
                if (interval > 0 && (now - last_per) >= pdMS_TO_TICKS(interval)) {
                    n2k_tx_di_bank(); last_per = now;
                }
            }
        }
    }
}

/* ================================================================ public */

esp_err_t can_server_init(void)
{
    const app_config_t *cfg = app_config_get();
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
        .tx_queue_depth         = 16,
    };
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_cfg, &s_node), TAG, "new node");

    s_rx_q = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_msg_t));
    twai_event_callbacks_t cbs = { .on_rx_done = rx_done_cb };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_node, &cbs, NULL),
                        TAG, "register cbs");
    ESP_RETURN_ON_ERROR(twai_node_enable(s_node), TAG, "enable");

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

    xTaskCreate(can_worker_task, "can", 4096, NULL, 5, NULL);
    return ESP_OK;
}
