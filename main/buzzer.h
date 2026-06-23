#pragma once
#include <stddef.h>
#include "esp_err.h"

/* Initialise the piezo buzzer on GPIO46 (LEDC). */
esp_err_t buzzer_init(void);

/* One-shot beep — frequency clamped to 100-10 000 Hz, duration to 1-5 000 ms. */
void buzzer_beep_once(uint32_t freq_hz, uint32_t dur_ms);

/* Continuous tone that plays until changed or stopped — frequency clamped to
   100-10 000 Hz; freq_hz == 0 silences. Used by the rule engine (buzzer().set / .off()). */
void buzzer_set_tone(uint32_t freq_hz);

/* Wire into MQTT callbacks in main.c. */
void buzzer_on_mqtt_connected(void);
void buzzer_on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen);
