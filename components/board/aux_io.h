#pragma once
/* The three header pads IO1, RXD and TXD (GPIO1, GPIO44, GPIO43), each in one
 * of several modes chosen in the configuration (APP_AUX_DI_ENABLE):
 *
 *   standard  GPIO1 unused, GPIO44/43 UART0 RX/TX -- where the console is
 *   input     digital input AUX1..AUX3, DI channels 8..10
 *   pwm       PWM output, frequency and duty cycle per pad (LEDC)
 *   analog    analog input, millivolts (ADC1 channel 0) -- GPIO1 only
 *
 * All three pads are bare 3.3 V GPIOs: no isolation, no protection.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    AUX_MODE_STANDARD = 0,
    AUX_MODE_INPUT    = 1,
    AUX_MODE_PWM      = 2,
    AUX_MODE_ANALOG   = 3,
    AUX_MODE_COUNT
} aux_mode_t;

#define AUX_PAD_COUNT          3
#define AUX_PWM_FREQ_MIN       5u          /* Hz; below this the 80 MHz clock cannot be divided down */
#define AUX_PWM_FREQ_MAX       1000000u    /* Hz; leaves 6 bits of duty resolution */
#define AUX_PWM_FREQ_DEFAULT   1000u
#define AUX_PWM_DUTY_MAX       1000u       /* duty is stored in tenths of a percent */

/* Whether pad a can be put in mode m. */
bool        aux_mode_allowed(uint8_t a, aux_mode_t m);
const char *aux_mode_name(aux_mode_t m);
/* Parses a mode name; false if it is not one. */
bool        aux_mode_parse(const char *s, aux_mode_t *out);

/* Applies the stored configuration to the pads. Call once after di_init();
   afterwards aux_io_apply() after every configuration change. */
esp_err_t   aux_io_init(void);
esp_err_t   aux_io_apply(void);

/* The mode pad a is in now. */
aux_mode_t  aux_io_mode(uint8_t a);

/* PWM as it actually runs: frequency in Hz as the timer produces it, duty in
   tenths of a percent, duty resolution in bits. False if pad a is not a PWM
   output. */
bool        aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint16_t *duty_pm, uint8_t *bits);

/* Changes a running PWM output at run time -- Modbus, MQTT, rules. Not stored:
   the configuration keeps the values a reboot starts with, and a controller
   writing a duty cycle every second must not wear out the flash. freq_hz 0
   keeps the frequency, duty_pm AUX_PWM_DUTY_KEEP keeps the duty cycle.
   ESP_ERR_INVALID_STATE if the pad is not a PWM output, ESP_ERR_INVALID_ARG
   for a value out of range. */
#define AUX_PWM_DUTY_KEEP      0xFFFFu
esp_err_t   aux_io_pwm_set(uint8_t a, uint32_t freq_hz, uint16_t duty_pm);

/* Change of the analog reading that counts as a new value -- for MQTT and
   the rule engine; below it the reading is noise. */
#define AUX_ANALOG_STEP_MV     20

/* MQTT, wired into main.c next to the other drivers:
     aux/<1..3>/mode       standard | input | pwm | analog  (published)
     aux/<1..3>/pwm        {"duty":25.0,"freq":1000}         (published)
     aux/<1..3>/pwm/set    25.0  or  {"duty":25.0,"freq":1000} (subscribed)
     aux/1/analog          millivolts                         (published)
     aux/read              publish all of the above           (subscribed) */
void        aux_io_on_mqtt_connected(void);
void        aux_io_on_mqtt_message(const char *topic, size_t topic_len,
                                   const char *data,  size_t data_len);

/* Reads the analog input of pad a in millivolts. ESP_ERR_INVALID_STATE if
   the pad is not an analog input. */
esp_err_t   aux_io_analog_mv(uint8_t a, int *mv);
