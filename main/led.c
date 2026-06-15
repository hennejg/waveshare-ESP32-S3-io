#include "led.h"
#include "app_mqtt.h"

#include <string.h>

#include "cJSON.h"
#include "led_strip.h"
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
    led_strip_set_pixel(s_strip, 0, r, g, b);
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

esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    hw_apply(r, g, b);
    return ESP_OK;
}

esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = LED_GPIO,
        .max_leds               = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,  /* WS2812 is GRB */
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

    s_queue = xQueueCreate(1, sizeof(led_seq_t));
    xTaskCreate(led_task, "led", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initialized WS2812 on GPIO%d", LED_GPIO);
    return ESP_OK;
}

void led_on_mqtt_connected(void)
{
    app_mqtt_subscribe("led/set", 0);
    publish_color();
}

void led_on_mqtt_message(const char *topic, size_t tlen,
                          const char *data,  size_t dlen)
{
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
