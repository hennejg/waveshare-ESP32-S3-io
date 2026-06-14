#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialise the WS2812 LED on GPIO38. Starts off. */
esp_err_t led_init(void);

/* Set LED colour directly. */
esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Wire into MQTT callbacks in main.c. */
void led_on_mqtt_connected(void);
void led_on_mqtt_message(const char *topic, size_t tlen,
                          const char *data,  size_t dlen);
