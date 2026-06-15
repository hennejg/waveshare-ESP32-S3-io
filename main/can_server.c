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

#define TAG          "can"
#define CAN_TX_GPIO  GPIO_NUM_2
#define CAN_RX_GPIO  GPIO_NUM_3
#define TX_POLL_MS   50
#define HB_PERIOD_MS 1000
#define RX_QUEUE_DEPTH 16

static twai_node_handle_t s_node = NULL;
static uint16_t           s_base;
static QueueHandle_t      s_rx_q;

/* ---------------------------------------------------------------- frame RX */

typedef struct { uint16_t id; uint8_t dlc; uint8_t data[8]; } rx_msg_t;

static IRAM_ATTR bool rx_done_cb(twai_node_handle_t node,
                                  const twai_rx_done_event_data_t *edata,
                                  void *ctx)
{
    uint8_t buf[8];
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) return false;

    rx_msg_t msg = { .id = (uint16_t)f.header.id, .dlc = (uint8_t)f.header.dlc };
    if (msg.dlc > 8) msg.dlc = 8;
    memcpy(msg.data, buf, msg.dlc);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rx_q, &msg, &woken);
    return woken;
}

/* ---------------------------------------------------------------- frame TX */

static void can_send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    uint8_t buf[8] = {0};
    if (dlc && data) memcpy(buf, data, dlc);
    twai_frame_t f = {
        .header = { .id = id, .dlc = dlc, .ide = 0, .rtr = 0 },
        .buffer = buf, .buffer_len = sizeof(buf),
    };
    esp_err_t r = twai_node_transmit(s_node, &f, 5);
    if (r != ESP_OK) ESP_LOGD(TAG, "TX 0x%03x: %s", id, esp_err_to_name(r));
}

static void tx_heartbeat(void)
{
    uint8_t s = app_mqtt_is_connected() ? 0x03 : 0x01;
    can_send(s_base + 0, &s, 1);
}

static void tx_di(uint8_t bits) { can_send(s_base + 1, &bits, 1); }

static void tx_do(void)
{
    uint8_t frame[2] = {0, 0};
    for (int i = 0; i < 8; i++) if (dout_get(i)) frame[1] |= (uint8_t)(1u << i);
    can_send(s_base + 2, frame, 2);
}

/* ---------------------------------------------------------------- DO opcodes
   0 WRITE  state  = mask
   1 SET    state |= mask
   2 CLEAR  state &= ~mask
   3 TOGGLE state ^= mask                                                    */

static void apply_do_cmd(uint8_t opcode, uint8_t mask)
{
    uint8_t cur = 0;
    for (int i = 0; i < 8; i++) if (dout_get(i)) cur |= (uint8_t)(1u << i);

    uint8_t next;
    switch (opcode) {
    case 0: next = mask;         break;
    case 1: next = cur |  mask;  break;
    case 2: next = cur & ~mask;  break;
    case 3: next = cur ^  mask;  break;
    default: ESP_LOGW(TAG, "Unknown DO opcode %u", opcode); return;
    }

    for (int i = 0; i < 8; i++) {
        bool bit = (next >> i) & 1;
        if (bit != dout_get(i)) dout_set(i, bit);
    }
    tx_do();
}

static void handle_rx(const rx_msg_t *m)
{
    if (m->id == (uint16_t)(s_base + 2) && m->dlc == 2) {
        apply_do_cmd(m->data[0], m->data[1]);

    } else if (m->id == (uint16_t)(s_base + 3) && m->dlc == 3) {
        led_set_rgb(m->data[0], m->data[1], m->data[2]);

    } else if (m->id == (uint16_t)(s_base + 4) && m->dlc >= 2) {
        uint16_t freq = (uint16_t)(m->data[0] | (m->data[1] << 8));
        uint32_t dur  = (m->dlc >= 3) ? (uint32_t)m->data[2] * 10u : 200u;
        if (freq > 0) buzzer_beep_once(freq, dur);

    } else if (m->id == (uint16_t)(s_base + 5) && m->dlc == 0) {
        uint8_t di = 0;
        for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);
        tx_heartbeat();
        tx_di(di);
        tx_do();
    }
}

/* ---------------------------------------------------------------- tasks */

static void can_rx_task(void *arg)
{
    rx_msg_t m;
    for (;;)
        if (xQueueReceive(s_rx_q, &m, portMAX_DELAY)) handle_rx(&m);
}

static void can_tx_task(void *arg)
{
    uint8_t    last_di  = 0xFF;
    TickType_t last_hb  = 0;
    TickType_t last_per = 0;
    uint16_t   interval = app_config_get()->can.tx_interval_ms;

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        if ((now - last_hb) >= pdMS_TO_TICKS(HB_PERIOD_MS)) {
            tx_heartbeat(); last_hb = now;
        }

        uint8_t di = 0;
        for (int i = 0; i < 8; i++) if (di_get(i)) di |= (uint8_t)(1u << i);

        if (di != last_di) { tx_di(di); last_di = di; }

        if (interval > 0 && (now - last_per) >= pdMS_TO_TICKS(interval)) {
            tx_di(di); last_per = now;
        }

        vTaskDelay(pdMS_TO_TICKS(TX_POLL_MS));
    }
}

/* ---------------------------------------------------------------- public */

esp_err_t can_server_init(void)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg->can.enable) { ESP_LOGI(TAG, "CAN disabled"); return ESP_OK; }

    twai_onchip_node_config_t node_cfg = {
        .io_cfg.tx              = CAN_TX_GPIO,
        .io_cfg.rx              = CAN_RX_GPIO,
        .io_cfg.quanta_clk_out  = GPIO_NUM_NC,
        .io_cfg.bus_off_indicator = GPIO_NUM_NC,
        .bit_timing.bitrate     = cfg->can.bitrate,
        .tx_queue_depth         = 8,
    };
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_cfg, &s_node), TAG, "new node");

    s_rx_q = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_msg_t));

    twai_event_callbacks_t cbs = { .on_rx_done = rx_done_cb };
    ESP_RETURN_ON_ERROR(
        twai_node_register_event_callbacks(s_node, &cbs, NULL),
        TAG, "register cbs");

    ESP_RETURN_ON_ERROR(twai_node_enable(s_node), TAG, "enable");

    s_base = cfg->can.base_id;

    xTaskCreate(can_rx_task, "can_rx", 3072, NULL, 5, NULL);
    xTaskCreate(can_tx_task, "can_tx", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "Started — base_id=0x%03x bitrate=%"PRIu32" tx_interval=%ums",
             s_base, cfg->can.bitrate, cfg->can.tx_interval_ms);
    return ESP_OK;
}
