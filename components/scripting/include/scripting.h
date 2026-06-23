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

typedef struct {
    scripting_di_get_fn_t         di_get;
    scripting_dout_set_fn_t       dout_set;
    scripting_dout_get_fn_t       dout_get;
    scripting_mqtt_subscribe_fn_t mqtt_subscribe;
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
