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
typedef esp_err_t (*scripting_dout_toggle_fn_t)(uint8_t ch);   /* atomic flip */
/* The IO1/RXD/TXD pads, numbered 1..3. Optional: NULL where the board has none.
   duty in tenths of a percent; freq 0 / duty 0xFFFF keep the current value. */
typedef esp_err_t (*scripting_aux_pwm_set_fn_t)(uint8_t pad, uint32_t freq_hz, uint16_t duty_pm);
typedef bool      (*scripting_aux_pwm_get_fn_t)(uint8_t pad, uint32_t *freq_hz, uint16_t *duty_pm);
typedef esp_err_t (*scripting_aux_analog_get_fn_t)(uint8_t pad, int *mv);
typedef int       (*scripting_mqtt_subscribe_fn_t)(const char *topic, int qos);
typedef int       (*scripting_mqtt_publish_fn_t)(const char *topic, const char *payload, int len, int qos, bool retain);
typedef esp_err_t (*scripting_led_set_fn_t)(uint8_t r, uint8_t g, uint8_t b);
typedef void      (*scripting_buzzer_set_fn_t)(uint32_t freq_hz);   /* 0 = off */

typedef struct {
    scripting_di_get_fn_t         di_get;
    scripting_dout_set_fn_t       dout_set;
    scripting_dout_get_fn_t       dout_get;
    scripting_dout_toggle_fn_t    dout_toggle;
    uint8_t                       di_count;     /* inputs input() accepts; 0 = 8 */
    uint8_t                       do_count;     /* outputs output() accepts; 0 = 8 */
    scripting_aux_pwm_set_fn_t    aux_pwm_set;
    scripting_aux_pwm_get_fn_t    aux_pwm_get;
    scripting_aux_analog_get_fn_t aux_analog_get;
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

// The broker connection went up or down: rules see it as the internal
// '$sys/mqtt' message "up"/"down" (mqttConnected()/mqttDown()). Kept as
// state, never dropped when the event queue is full; safe from any task.
void scripting_on_mqtt_state(bool up);

// Notify the rule engine that a digital input changed state.
// Safe to call from any task.
void scripting_on_input_change(uint8_t channel, bool state);

// A new analog reading of pad 1..3 in millivolts (only when it moved by the
// driver's step); INT_MIN when the pad has stopped being an analog input.
// Safe to call from any task. False if the engine could not take it (not
// started yet, queue full): the caller offers it again with its next sample.
bool scripting_on_analog(uint8_t pad, int mv);

// Replace the running script with new_script and restart rule evaluation.
// The script is evaluated into a fresh context and only takes over if that
// works; otherwise the rules already running carry on.
// Safe to call from any task. new_script is copied internally.
// Returns the ticket of the queued reload, or 0 if it could not be queued.
uint32_t scripting_reload(const char *new_script);

// The same, and once the engine has accepted new_script, it calls store(to_store)
// on its own task -- also when the caller has stopped waiting for the verdict.
// Stored by the caller after the verdict, a script accepted after the caller's
// timeout was running but never stored, and the next boot brought back the old
// one. to_store is what the next boot should load ("" for the built-in demo);
// it is copied. store's result is in the status.
typedef esp_err_t (*scripting_store_fn_t)(const char *to_store);
uint32_t scripting_reload_and_store(const char *new_script, const char *to_store,
                                    scripting_store_fn_t store);

// Outcome of the most recent reload the engine has finished acting on,
// stamped with the ticket scripting_reload() handed out for it. A caller
// waits until `ticket` reaches its own; verdicts come in ticket order, so a
// higher one means its own verdict has already been overwritten by a later
// reload's and must not be taken for it. Waiting for "any change" instead
// let one request take another's verdict -- and store a refused script.
typedef struct {
    uint32_t ticket;
    bool     ok;
    char     message[96];      // empty when ok
    bool     store_tried;      // accepted, and the reload asked to be stored
    esp_err_t store_err;       // store()'s result when store_tried
} scripting_reload_status_t;

void scripting_reload_status(scripting_reload_status_t *out);

// The timezone changed: cron timers already armed carry the old offset and
// are re-armed. Does not declare the clock valid. Safe to call from any task.
void scripting_on_tz_change(void);

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
