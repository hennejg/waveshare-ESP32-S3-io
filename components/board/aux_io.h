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

/* PWM as it actually runs: frequency in Hz and duty resolution in bits.
   False if pad a is not a PWM output. */
bool        aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint8_t *bits);

/* Reads the analog input of pad a in millivolts. ESP_ERR_INVALID_STATE if
   the pad is not an analog input. */
esp_err_t   aux_io_analog_mv(uint8_t a, int *mv);
