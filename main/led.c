#include "led.h"
#include "app_config.h"
#include "app_mqtt.h"

#include <string.h>

#include "cJSON.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG         "led"
#define LED_GPIO    GPIO_NUM_38
#define SEQ_MAX     16
#define DUR_MAX_MS  30000u

typedef struct {
    uint8_t  r, g, b;
    uint8_t  _pad;
    uint32_t duration_ms;
} led_step_t;

typedef struct {
    led_step_t steps[SEQ_MAX];
    int        count;
} led_seq_t;

static led_strip_handle_t s_strip;
static uint8_t            s_r, s_g, s_b;
static QueueHandle_t      s_queue;
static bool               s_status_mode;

/* ------------------------------------------------------------------ helpers */

static uint8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

static bool parse_hex(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!s || s[0] != '#' || strlen(s) < 7) return false;
    *r = (hex_digit(s[1]) << 4) | hex_digit(s[2]);
    *g = (hex_digit(s[3]) << 4) | hex_digit(s[4]);
    *b = (hex_digit(s[5]) << 4) | hex_digit(s[6]);
    return true;
}

/* Parse a sequence step: {"color":"#RRGGBB","duration":N} — both fields required. */
static bool parse_step(cJSON *obj, led_step_t *s)
{
    if (!cJSON_IsObject(obj)) return false;
    cJSON *col = cJSON_GetObjectItem(obj, "color");
    cJSON *dur = cJSON_GetObjectItem(obj, "duration");
    if (!cJSON_IsString(col) || !cJSON_IsNumber(dur)) return false;
    uint32_t d = (uint32_t)dur->valuedouble;
    if (d == 0 || d > DUR_MAX_MS) return false;
    if (!parse_hex(col->valuestring, &s->r, &s->g, &s->b)) return false;
    s->duration_ms = d;
    return true;
}

static void hw_apply(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r; s_g = g; s_b = b;
    /* WS2812 wire order is GRB — swap red and green at the call site. */
    led_strip_set_pixel(s_strip, 0, g, r, b);
    led_strip_refresh(s_strip);
}

static void publish_color(void)
{
    char p[8];
    snprintf(p, sizeof(p), "#%02x%02x%02x", s_r, s_g, s_b);
    app_mqtt_publish("led", p, -1, 0, false);
}

/* -------------------------------------------------------------------- task */

static void led_task(void *arg)
{
    led_seq_t seq;
    for (;;) {
        xQueueReceive(s_queue, &seq, portMAX_DELAY);
        for (int i = 0; i < seq.count; i++) {
            hw_apply(seq.steps[i].r, seq.steps[i].g, seq.steps[i].b);
            vTaskDelay(pdMS_TO_TICKS(seq.steps[i].duration_ms));
            /* Preempt remaining steps if a newer sequence has arrived. */
            if (uxQueueMessagesWaiting(s_queue)) break;
        }
        if (app_mqtt_is_connected()) publish_color();
    }
}

/* ------------------------------------------------------------------ public */

/* ============================================================ status mode === */

/* Colours (50% brightness for persistent states, 100% for flashes) */
#define STA_BOOT_R  77  /* 30% yellow  */
#define STA_BOOT_G  77
#define STA_BOOT_B  0
#define STA_NET_R   0   /* 30% green   */
#define STA_NET_G   77
#define STA_NET_B   0
#define STA_MQTT_R  77  /* 30% purple  */
#define STA_MQTT_G  0
#define STA_MQTT_B  77
#define STA_RX_R    255 /* 100% red    */
#define STA_RX_G    0
#define STA_RX_B    0
#define STA_TX_R    0   /* 100% blue   */
#define STA_TX_G    0
#define STA_TX_B    255
#define FLASH_MS       100
#define AP_BLINK_US    400000ULL   /* 400 ms half-period → slow blue pulse */

typedef struct { uint8_t r, g, b; } flash_t;

static uint8_t            s_bg_r, s_bg_g, s_bg_b;
static uint8_t            s_net_level;
static int                s_net_count;
static QueueHandle_t      s_flash_q;
static bool               s_ap_mode      = false;
static bool               s_ap_blink_on  = false;
static esp_timer_handle_t s_ap_blink_t   = NULL;
static portMUX_TYPE       s_mux          = portMUX_INITIALIZER_UNLOCKED;

static void set_background(uint8_t r, uint8_t g, uint8_t b)
{
    s_bg_r = r; s_bg_g = g; s_bg_b = b;
    hw_apply(r, g, b);
}

static void refresh_status(void)
{
    switch (s_net_level) {
    case 0: set_background(STA_BOOT_R, STA_BOOT_G, STA_BOOT_B); break;
    case 1: set_background(STA_NET_R,  STA_NET_G,  STA_NET_B);  break;
    case 2: set_background(STA_MQTT_R, STA_MQTT_G, STA_MQTT_B); break;
    }
}

static void flash_task(void *arg)
{
    flash_t f;
    for (;;) {
        xQueueReceive(s_flash_q, &f, portMAX_DELAY);
        hw_apply(f.r, f.g, f.b);
        vTaskDelay(pdMS_TO_TICKS(FLASH_MS));
        hw_apply(s_bg_r, s_bg_g, s_bg_b);
    }
}

static void ap_blink_cb(void *arg)
{
    s_ap_blink_on = !s_ap_blink_on;
    hw_apply(0, 0, s_ap_blink_on ? 77 : 0);   /* 30% blue pulse */
}

void led_status_set_ap_mode(bool up)
{
    if (!s_status_mode) return;

    taskENTER_CRITICAL(&s_mux);
    s_ap_mode = up;
    taskEXIT_CRITICAL(&s_mux);

    if (up) {
        if (!s_ap_blink_t) {
            esp_timer_create_args_t a = { .callback = ap_blink_cb, .name = "ap_blink" };
            esp_timer_create(&a, &s_ap_blink_t);
        }
        esp_timer_start_periodic(s_ap_blink_t, AP_BLINK_US);
        ESP_LOGI(TAG, "AP mode — LED blue blink");
    } else {
        if (s_ap_blink_t) { esp_timer_stop(s_ap_blink_t); }
        refresh_status();
    }
}

void led_status_set_network(bool up)
{
    if (!s_status_mode) return;

    bool do_ap_down = false;
    bool do_refresh = false;

    if (up) {
        led_status_set_ap_mode(false);   /* AP done once we have an IP */
        taskENTER_CRITICAL(&s_mux);
        s_net_count++;
        if (s_net_level < 1) { s_net_level = 1; do_refresh = true; }
        taskEXIT_CRITICAL(&s_mux);
    } else {
        taskENTER_CRITICAL(&s_mux);
        if (--s_net_count <= 0) {
            s_net_count = 0;
            if (s_net_level > 0) { s_net_level = 0; do_refresh = true; }
        }
        taskEXIT_CRITICAL(&s_mux);
    }

    (void)do_ap_down;
    if (do_refresh) refresh_status();
}

void led_status_set_mqtt(bool up)
{
    if (!s_status_mode) return;

    taskENTER_CRITICAL(&s_mux);
    if (up) {
        s_net_level = 2;
    } else {
        s_net_level = (s_net_count > 0) ? 1 : 0;
    }
    taskEXIT_CRITICAL(&s_mux);

    refresh_status();
}

static void do_flash(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_status_mode || !s_flash_q) return;
    flash_t f = {r, g, b};
    xQueueOverwrite(s_flash_q, &f);
}

void led_status_flash_rx(void) { do_flash(STA_RX_R, STA_RX_G, STA_RX_B); }
void led_status_flash_tx(void) { do_flash(STA_TX_R, STA_TX_G, STA_TX_B); }

/* ============================================================== IO mode ==== */

esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_status_mode) return ESP_OK;   /* ignore in status mode */
    hw_apply(r, g, b);
    return ESP_OK;
}

void led_force_set(uint8_t r, uint8_t g, uint8_t b)
{
    hw_apply(r, g, b);   /* always applies, regardless of mode */
}

esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
        TAG, "new RMT device");

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    s_status_mode = (app_config_get()->led_mode == LED_MODE_STATUS);

    if (s_status_mode) {
        s_flash_q = xQueueCreate(1, sizeof(flash_t));
        xTaskCreate(flash_task, "led_flash", 3072, NULL, 6, NULL);
        refresh_status();   /* boot = 50% yellow */
        ESP_LOGI(TAG, "WS2812 on GPIO%d — Status mode", LED_GPIO);
    } else {
        s_queue = xQueueCreate(1, sizeof(led_seq_t));
        xTaskCreate(led_task, "led", 3072, NULL, 5, NULL);
        ESP_LOGI(TAG, "WS2812 on GPIO%d — IO mode", LED_GPIO);
    }
    return ESP_OK;
}

void led_on_mqtt_connected(void)
{
    if (s_status_mode) return;   /* status mode doesn't subscribe to led/set */
    app_mqtt_subscribe("led/set", 0);
    publish_color();
}

void led_on_mqtt_message(const char *topic, size_t tlen,
                          const char *data,  size_t dlen)
{
    if (s_status_mode) return;   /* status mode ignores MQTT LED commands */
    static const char SUFFIX[] = "led/set";
    const size_t sl = sizeof(SUFFIX) - 1;
    if (tlen < sl || memcmp(topic + tlen - sl, SUFFIX, sl) != 0) return;

    char buf[1024];
    if (dlen == 0 || dlen >= sizeof(buf)) return;
    memcpy(buf, data, dlen);
    buf[dlen] = '\0';

    /* "#RRGGBB" — instant set, no queue */
    if (buf[0] == '#') {
        uint8_t r, g, b;
        if (parse_hex(buf, &r, &g, &b)) {
            hw_apply(r, g, b);
            if (app_mqtt_is_connected()) publish_color();
        } else {
            ESP_LOGW(TAG, "Invalid colour: %s", buf);
        }
        return;
    }

    /* {"color":"#RRGGBB","duration":N} or [{...},{...}] — goes through task */
    cJSON *root = cJSON_Parse(buf);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return; }

    led_seq_t seq = { .count = 0 };

    if (cJSON_IsObject(root)) {
        led_step_t s = {0};
        if (parse_step(root, &s)) seq.steps[seq.count++] = s;
    } else if (cJSON_IsArray(root)) {
        int n = cJSON_GetArraySize(root);
        if (n > SEQ_MAX) n = SEQ_MAX;
        for (int i = 0; i < n; i++) {
            led_step_t s = {0};
            if (parse_step(cJSON_GetArrayItem(root, i), &s))
                seq.steps[seq.count++] = s;
        }
    }

    cJSON_Delete(root);

    if (seq.count > 0) {
        xQueueOverwrite(s_queue, &seq);
    } else {
        ESP_LOGW(TAG, "No valid steps in LED command");
    }
}
