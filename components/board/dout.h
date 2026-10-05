#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialise 8 digital outputs via TCA9554 on I2C (SDA GPIO42, SCL GPIO41).
   All outputs start in the off state. */
esp_err_t dout_init(void);

/* Returns the current logical state of output n (0-7). */
bool      dout_get(uint8_t n);

/* Set output n to state, write to hardware, publish confirmation. */
esp_err_t dout_set(uint8_t n, bool state);

/* Re-apply current logical states to hardware (e.g. after invert config
   change) and publish all states. */
/* Re-apply the logical state to the chip and publish it. Returns the result of
   the transfer; on failure the previous state is restored and published, so
   what is reported is always what the hardware actually holds. */
esp_err_t dout_publish_all(void);

/* Wire these into the MQTT callbacks in main.c. */
void dout_on_mqtt_connected(void);
void dout_on_mqtt_message(const char *topic, size_t topic_len,
                           const char *data,  size_t data_len);
