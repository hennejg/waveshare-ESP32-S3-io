#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialise the 8 digital inputs on GPIO4-11.
   Safe to call before WiFi/MQTT are up — monitoring starts immediately,
   publishing begins once the MQTT broker connects. */
esp_err_t di_init(void);

/* Returns the current logical state of input n (0-7).
   true = input active (optocoupler conducting, GPIO low). */
bool di_get(uint8_t n);

/* Publish the current state of all 8 inputs immediately. */
void di_publish_all(void);

/* Wire these into the MQTT callbacks in main.c. */
void di_on_mqtt_connected(void);
void di_on_mqtt_message(const char *topic, size_t topic_len,
                         const char *data,  size_t data_len);
