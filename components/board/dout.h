#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialise 8 digital outputs via TCA9554 on I2C (SDA GPIO42, SCL GPIO41).
   All outputs start in the off state. */
esp_err_t dout_init(void);

/* Returns the current logical state of output n (0-7). */
bool      dout_get(uint8_t n);

/* Current logical state of all eight outputs, bit 0 = DO1. */
uint8_t   dout_get_all(void);

/* Change several outputs as one operation: read, compute and write happen
   under the same lock and reach the chip in a single transfer, so the
   addressed channels switch together and no other task can slip a change in
   between. Applied in the order clear, set, toggle; overlapping masks are
   therefore resolved toggle-last.

   Bit n of each mask refers to output n+1. Returns the result of the transfer;
   on failure nothing changes. Prefer this over a loop of dout_set() — that
   writes the whole port once per bit, so the outputs step through the
   intermediate patterns and a transfer failing halfway leaves some channels
   switched and others not. */
esp_err_t dout_modify(uint8_t set, uint8_t clear, uint8_t toggle);

/* Set output n to state, write to hardware, publish confirmation.
   Shorthand for dout_modify() on a single channel. */
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
