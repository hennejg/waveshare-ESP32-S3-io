#include "led.h"
#include "app_mqtt.h"

#include <string.h>

#include "cJSON.h"
#include "led_strip.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG      "led"
#define LED_GPIO  GPIO_NUM_38

static led_strip_handle_t s_strip;
static uint8_t s_r, s_g, s_b;

/* ------------------------------------------------------------------ helpers */

static uint8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

/* Accepts "#RRGGBB" or "[R,G,B]" (values 0-255, spaces allowed). */
static bool parse_color(const char *data, size_t dlen,
                         uint8_t *r, uint8_t *g, uint8_t *b)
{
    char buf[32];
    if (dlen == 0 || dlen >= sizeof(buf)) return false;
    memcpy(buf, data, dlen);
    buf[dlen] = '\0';

    if (buf[0] == '#' && dlen >= 7) {
        *r = (hex_digit(buf[1]) << 4) | hex_digit(buf[2]);
        *g = (hex_digit(buf[3]) << 4) | hex_digit(buf[4]);
        *b = (hex_digit(buf[5]) << 4) | hex_digit(buf[6]);
        return true;
    }

    if (buf[0] == '[') {
        cJSON *arr = cJSON_Parse(buf);
        bool ok = cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3;
        if (ok) {
            *r = (uint8_t)cJSON_GetArrayItem(arr, 0)->valuedouble;
            *g = (uint8_t)cJSON_GetArrayItem(arr, 1)->valuedouble;
            *b = (uint8_t)cJSON_GetArrayItem(arr, 2)->valuedouble;
        }
        cJSON_Delete(arr);
        if (ok) return true;
    }

    return false;
}

static void publish_color(void)
{
    char payload[8];
    snprintf(payload, sizeof(payload), "#%02x%02x%02x", s_r, s_g, s_b);
    app_mqtt_publish("led", payload, -1, 0, false);
}

/* ------------------------------------------------------------------ public */

esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r; s_g = g; s_b = b;
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, r, g, b), TAG, "set pixel");
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_strip), TAG, "refresh");
    return ESP_OK;
}

esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz      = 10 * 1000 * 1000,
        .flags.with_dma     = false,
    };
    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
        TAG, "new RMT device");

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

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

    uint8_t r, g, b;
    if (parse_color(data, dlen, &r, &g, &b)) {
        if (led_set_rgb(r, g, b) == ESP_OK) {
            publish_color();
        }
    } else {
        ESP_LOGW(TAG, "Unrecognised colour payload");
    }
}
