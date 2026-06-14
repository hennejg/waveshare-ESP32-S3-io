#pragma once
#include <stddef.h>
#include "esp_err.h"

/* Initialise the piezo buzzer on GPIO46 (LEDC). */
esp_err_t buzzer_init(void);

/* Wire into MQTT callbacks in main.c. */
void buzzer_on_mqtt_connected(void);
void buzzer_on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen);
