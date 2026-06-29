#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Hardware I/O callbacks injected from main.c.
// Avoids a circular dependency between the scripting component and main.
typedef bool      (*scripting_di_get_fn_t)(uint8_t ch);
typedef esp_err_t (*scripting_dout_set_fn_t)(uint8_t ch, bool state);
typedef bool      (*scripting_dout_get_fn_t)(uint8_t ch);
typedef int       (*scripting_mqtt_subscribe_fn_t)(const char *topic, int qos);
typedef int       (*scripting_mqtt_publish_fn_t)(const char *topic, const char *payload, int len, int qos, bool retain);
typedef esp_err_t (*scripting_led_set_fn_t)(uint8_t r, uint8_t g, uint8_t b);
typedef void      (*scripting_buzzer_set_fn_t)(uint32_t freq_hz);   /* 0 = off */

typedef struct {
    scripting_di_get_fn_t         di_get;
    scripting_dout_set_fn_t       dout_set;
    scripting_dout_get_fn_t       dout_get;
    scripting_mqtt_subscribe_fn_t mqtt_subscribe;
    scripting_mqtt_publish_fn_t   mqtt_publish;
    scripting_led_set_fn_t        led_set;      /* drive the RGB LED (IO mode only) */
    scripting_buzzer_set_fn_t     buzzer_set;   /* continuous tone; 0 Hz = off */
} scripting_io_t;

// Initialize the scripting engine and start the rule evaluation task.
// user_script must remain valid for the application lifetime (string literal or static buffer).
// io must remain valid for the application lifetime.
esp_err_t scripting_init(const char *user_script, const scripting_io_t *io);

// Called from the MQTT connected callback — subscribes to the rules topic.
void scripting_on_mqtt_connected(void);

// Feed an incoming MQTT message into the rule engine.
// Safe to call from any task; delivers via an internal queue.
void scripting_on_mqtt_message(const char *topic, size_t topic_len,
                                const char *data,  size_t data_len);

// Notify the rule engine that a digital input changed state.
// Safe to call from any task.
void scripting_on_input_change(uint8_t channel, bool state);

// Replace the running script with new_script and restart rule evaluation.
// Clears all existing rules, re-evaluates the DSL, then evaluates new_script.
// Safe to call from any task. new_script is copied internally.
void scripting_reload(const char *new_script);

// Mark the wall-clock as valid (real time available). Cron triggers stay suppressed
// until this is true, so they don't fire boot-relative. Call BEFORE scripting_init()
// when the RTC seeded real time at boot. Safe to call from any task.
void scripting_set_time_valid(void);

// Notify the engine that the clock was (re)synced from SNTP. Marks time valid and
// re-arms all cron triggers against the corrected wall-clock — the fix for the timer
// step that occurs when SNTP jumps the clock from boot-relative to real time.
// Safe to call from any task (e.g. the SNTP sync callback).
void scripting_on_time_sync(void);

// Notify the engine that an upstream command arrived on a fieldbus — feeds the
// corresponding command-health source (modbus()/can() in the DSL). Call on each inbound
// MODBUS coil/holding-register write, and each CAN/NMEA2000 control write, respectively.
// Safe to call from any task (e.g. the Modbus/CAN RX tasks).
void scripting_on_modbus_activity(void);
void scripting_on_can_activity(void);
