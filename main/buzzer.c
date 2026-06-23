#include "buzzer.h"
#include "app_mqtt.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG        "buzzer"
#define GPIO_BUZZER GPIO_NUM_46
#define LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER  LEDC_TIMER_0
#define LEDC_CHAN   LEDC_CHANNEL_0
#define LEDC_RES    LEDC_TIMER_10_BIT
#define DUTY_ON     512u   /* 50% duty — square wave */
#define SEQ_MAX     16

#define FREQ_MIN    100u
#define FREQ_MAX    10000u
#define DUR_MIN_MS  1u
#define DUR_MAX_MS  5000u

typedef struct {
    uint32_t freq_hz;   /* 0 = silence */
    uint32_t dur_ms;
} bstep_t;

typedef struct {
    bstep_t steps[SEQ_MAX];
    int     count;
} bseq_t;

static QueueHandle_t s_queue;

/* ------------------------------------------------------------------ helpers */

/* Parse {"freq": N, "duration": N} — duration required, freq optional (0=silence). */
static bool parse_bstep(cJSON *obj, bstep_t *s)
{
    if (!cJSON_IsObject(obj)) return false;
    cJSON *dur = cJSON_GetObjectItem(obj, "duration");
    if (!cJSON_IsNumber(dur)) return false;

    uint32_t d = (uint32_t)dur->valuedouble;
    if (d < DUR_MIN_MS) d = DUR_MIN_MS;
    if (d > DUR_MAX_MS) d = DUR_MAX_MS;

    cJSON *freq = cJSON_GetObjectItem(obj, "freq");
    uint32_t f = cJSON_IsNumber(freq) ? (uint32_t)freq->valuedouble : 0;
    if (f > 0) {
        if (f < FREQ_MIN) f = FREQ_MIN;
        if (f > FREQ_MAX) f = FREQ_MAX;
    }

    s->freq_hz = f;
    s->dur_ms  = d;
    return true;
}

/* -------------------------------------------------------------------- task */

static void buzzer_task(void *arg)
{
    bseq_t seq;
    for (;;) {
        xQueueReceive(s_queue, &seq, portMAX_DELAY);
        bool sustain = false;
        for (int i = 0; i < seq.count; i++) {
            if (seq.steps[i].freq_hz > 0) {
                ledc_set_freq(LEDC_MODE, LEDC_TIMER, seq.steps[i].freq_hz);
                ledc_set_duty(LEDC_MODE, LEDC_CHAN, DUTY_ON);
            } else {
                ledc_set_duty(LEDC_MODE, LEDC_CHAN, 0);  /* silence */
            }
            ledc_update_duty(LEDC_MODE, LEDC_CHAN);
            /* dur_ms == 0 means "hold this tone until the next command" (continuous tone
               from buzzer_set_tone — MQTT sequences always clamp duration to >= 1 ms). */
            if (seq.steps[i].dur_ms == 0) { sustain = true; break; }
            vTaskDelay(pdMS_TO_TICKS(seq.steps[i].dur_ms));
            /* Preempt remaining steps if a newer sequence has arrived. */
            if (uxQueueMessagesWaiting(s_queue)) break;
        }
        /* Silence after a sequence ends or is preempted — but not for a held tone. */
        if (!sustain) {
            ledc_set_duty(LEDC_MODE, LEDC_CHAN, 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHAN);
        }
    }
}

/* ------------------------------------------------------------------ public */

void buzzer_beep_once(uint32_t freq_hz, uint32_t dur_ms)
{
    if (freq_hz < FREQ_MIN) freq_hz = FREQ_MIN;
    if (freq_hz > FREQ_MAX) freq_hz = FREQ_MAX;
    if (dur_ms  < DUR_MIN_MS) dur_ms = DUR_MIN_MS;
    if (dur_ms  > DUR_MAX_MS) dur_ms = DUR_MAX_MS;
    bseq_t seq = { .count = 1 };
    seq.steps[0].freq_hz = freq_hz;
    seq.steps[0].dur_ms  = dur_ms;
    xQueueOverwrite(s_queue, &seq);
}

void buzzer_set_tone(uint32_t freq_hz)
{
    if (freq_hz > 0) {
        if (freq_hz < FREQ_MIN) freq_hz = FREQ_MIN;
        if (freq_hz > FREQ_MAX) freq_hz = FREQ_MAX;
    }
    /* dur_ms = 0 → the task holds the tone (or silence, if freq 0) until the next command. */
    bseq_t seq = { .count = 1 };
    seq.steps[0].freq_hz = freq_hz;
    seq.steps[0].dur_ms  = 0;
    xQueueOverwrite(s_queue, &seq);
}

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

    s_queue = xQueueCreate(1, sizeof(bseq_t));
    xTaskCreate(buzzer_task, "buzzer", 3072, NULL, 5, NULL);

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

    char buf[1024];
    if (dlen == 0 || dlen >= sizeof(buf)) return;
    memcpy(buf, data, dlen);
    buf[dlen] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return; }

    bseq_t seq = { .count = 0 };

    if (cJSON_IsObject(root)) {
        bstep_t s = {0};
        if (parse_bstep(root, &s)) seq.steps[seq.count++] = s;
    } else if (cJSON_IsArray(root)) {
        int n = cJSON_GetArraySize(root);
        if (n > SEQ_MAX) n = SEQ_MAX;
        for (int i = 0; i < n; i++) {
            bstep_t s = {0};
            if (parse_bstep(cJSON_GetArrayItem(root, i), &s))
                seq.steps[seq.count++] = s;
        }
    }

    cJSON_Delete(root);

    if (seq.count > 0) {
        xQueueOverwrite(s_queue, &seq);
    } else {
        ESP_LOGW(TAG, "No valid steps in buzzer command");
    }
}
