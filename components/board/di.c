#include "di.h"
#include "app_config.h"
#ifdef CONFIG_APP_MQTT_ENABLE
#include <stdbool.h>
int  app_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain);
int  app_mqtt_subscribe(const char *topic, int qos);
bool app_mqtt_is_connected(void);
#endif
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif

#include <string.h>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#define TAG          "di"
#define NUM_DI       8
#define DEBOUNCE_MS  10

static const gpio_num_t s_gpios[NUM_DI] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6,  GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
};

static bool         s_state[NUM_DI];
static TaskHandle_t s_task;

/* ------------------------------------------------------------------ helpers */

static inline bool read_pin(uint8_t n)
{
    /* Default sense: GPIO high → true.  Flip with di[n].invert = true. */
    bool level = (bool)gpio_get_level(s_gpios[n]);
    return level ^ app_config_get()->di[n].invert;
}

#ifdef CONFIG_APP_MQTT_ENABLE
static void publish_one(uint8_t n, bool state)
{
    const char *name = app_config_get()->di[n].name;
    char topic[32];
    if (name[0]) snprintf(topic, sizeof(topic), "input/%.20s",   name);
    else         snprintf(topic, sizeof(topic), "input/%u",    n + 1);
    app_mqtt_publish(topic, state ? "true" : "false", -1, 0, false);
}
#endif

static void notify_one(uint8_t n, bool state)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    if (app_mqtt_is_connected())
        publish_one(n, state);
#endif
#ifdef CONFIG_APP_MATTER_ENABLE
    matter_di_update(n, state);
#endif
}

void di_publish_all(void)
{
    for (uint8_t i = 0; i < NUM_DI; i++) {
        s_state[i] = read_pin(i);   /* refresh from hardware before publishing */
        notify_one(i, s_state[i]);
    }
}

bool di_get(uint8_t n)
{
    if (n >= NUM_DI) return false;
    return s_state[n];
}

/* ----------------------------------------------------------------- ISR / task */

static void IRAM_ATTR gpio_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void di_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        for (uint8_t i = 0; i < NUM_DI; i++) {
            bool cur = read_pin(i);
            if (cur != s_state[i]) {
                s_state[i] = cur;
                notify_one(i, cur);
            }
        }
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t di_init(void)
{
    /* Configure GPIOs with interrupts initially disabled to avoid a race
       between gpio_config and gpio_isr_handler_add. */
    for (uint8_t i = 0; i < NUM_DI; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_gpios[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        s_state[i] = read_pin(i);
    }

    xTaskCreate(di_task, "di", 4096, NULL, 5, &s_task);

    /* Install ISR service (ignore ESP_ERR_INVALID_STATE if already done). */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    /* Register handlers then enable edge interrupts. */
    for (uint8_t i = 0; i < NUM_DI; i++) {
        gpio_isr_handler_add(s_gpios[i], gpio_isr, NULL);
        gpio_set_intr_type(s_gpios[i], GPIO_INTR_ANYEDGE);
    }

    ESP_LOGI(TAG, "Initialized %d inputs (GPIO%d..GPIO%d)",
             NUM_DI, s_gpios[0], s_gpios[NUM_DI - 1]);
    return ESP_OK;
}

void di_on_mqtt_connected(void)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    app_mqtt_subscribe("input/read", 0);
    di_publish_all();
#endif
}

void di_on_mqtt_message(const char *topic, size_t tlen,
                         const char *data,  size_t dlen)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    /* Match any topic ending in "input/read" regardless of prefix. */
    static const char SUFFIX[] = "input/read";
    const size_t slen = sizeof(SUFFIX) - 1;
    if (tlen >= slen && memcmp(topic + tlen - slen, SUFFIX, slen) == 0) {
        di_publish_all();
    }
#else
    (void)topic; (void)tlen; (void)data; (void)dlen;
#endif
}
