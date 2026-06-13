#include "button.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <wifi_config.h>

#define TAG          "button"
#define BUTTON_GPIO  GPIO_NUM_0
#define POLL_MS      50
#define HOLD_MS      5000

static void button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    uint32_t held_ms  = 0;
    bool     notified = false; /* avoid spamming the log */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (gpio_get_level(BUTTON_GPIO) == 0) {
            held_ms += POLL_MS;

            if (!notified && held_ms >= 1000) {
                ESP_LOGI(TAG, "Button held — release within %us to cancel WiFi reset",
                         (HOLD_MS - held_ms) / 1000 + 1);
                notified = true;
            }

            if (held_ms >= HOLD_MS) {
                ESP_LOGW(TAG, "WiFi reset triggered — clearing credentials and rebooting");
                wifi_config_reset(); /* clears NVS config, calls esp_restart() */
                /* never returns */
            }
        } else {
            if (held_ms > 0 && held_ms < HOLD_MS) {
                ESP_LOGI(TAG, "Button released after %"PRIu32" ms — no reset", held_ms);
            }
            held_ms  = 0;
            notified = false;
        }
    }
}

void button_init(void)
{
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);
}
