#include "buzzer.h"
#include "app_mqtt.h"

#include <string.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG          "buzzer"
#define GPIO_BUZZER   GPIO_NUM_46
#define LEDC_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHAN     LEDC_CHANNEL_0
#define LEDC_RES      LEDC_TIMER_10_BIT
#define DUTY_ON       512u   /* 50% duty — square wave */

#define FREQ_MIN      100u
#define FREQ_MAX      10000u
#define DUR_MIN_MS    1u
#define DUR_MAX_MS    5000u

typedef struct { uint32_t freq_hz; uint32_t dur_ms; } beep_t;

static QueueHandle_t s_queue;

/* ------------------------------------------------------------------ helpers */

static bool json_get_u32(const char *json, const char *key, uint32_t *out)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return false;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
    *out = v;
    return true;
}

/* ------------------------------------------------------------------- task */

static void buzzer_task(void *arg)
{
    beep_t cmd;
    for (;;) {
        xQueueReceive(s_queue, &cmd, portMAX_DELAY);
        ledc_set_freq(LEDC_MODE, LEDC_TIMER, cmd.freq_hz);
        ledc_set_duty(LEDC_MODE, LEDC_CHAN, DUTY_ON);
        ledc_update_duty(LEDC_MODE, LEDC_CHAN);
        vTaskDelay(pdMS_TO_TICKS(cmd.dur_ms));
        ledc_set_duty(LEDC_MODE, LEDC_CHAN, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHAN);
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer config");

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHAN,
        .timer_sel  = LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = GPIO_BUZZER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "channel config");

    s_queue = xQueueCreate(1, sizeof(beep_t));
    xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initialized buzzer on GPIO%d", GPIO_BUZZER);
    return ESP_OK;
}

void buzzer_on_mqtt_connected(void)
{
    app_mqtt_subscribe("buzzer/beep", 0);
}

void buzzer_on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen)
{
    static const char SUFFIX[] = "buzzer/beep";
    const size_t sl = sizeof(SUFFIX) - 1;
    if (tlen < sl || memcmp(topic + tlen - sl, SUFFIX, sl) != 0) return;

    char buf[64];
    if (dlen >= sizeof(buf)) return;
    memcpy(buf, data, dlen);
    buf[dlen] = '\0';

    uint32_t freq_hz = 1000, dur_ms = 200;
    json_get_u32(buf, "freq", &freq_hz);
    json_get_u32(buf, "duration", &dur_ms);

    if (freq_hz < FREQ_MIN) freq_hz = FREQ_MIN;
    if (freq_hz > FREQ_MAX) freq_hz = FREQ_MAX;
    if (dur_ms  < DUR_MIN_MS) dur_ms = DUR_MIN_MS;
    if (dur_ms  > DUR_MAX_MS) dur_ms = DUR_MAX_MS;

    beep_t cmd = { freq_hz, dur_ms };
    xQueueOverwrite(s_queue, &cmd);
    ESP_LOGD(TAG, "Beep %"PRIu32" Hz for %"PRIu32" ms", freq_hz, dur_ms);
}
