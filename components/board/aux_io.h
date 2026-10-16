#pragma once
/* The three header pads IO1, RXD and TXD (GPIO1, GPIO44, GPIO43), each in one
 * of several modes chosen in the configuration (APP_AUX_DI_ENABLE):
 *
 *   standard  GPIO1 unused and high-impedance, GPIO44/43 UART0 RX/TX
 *   input     digital input AUX1..AUX3, DI channels 8..10
 *   pwm       PWM output, frequency and duty cycle per pad (LEDC)
 *   analog    analog input, millivolts (ADC1 channel 0) -- GPIO1 only
 *   output    digital output, on/off; starts off, not stored
 *
 * All three pads are bare 3.3 V GPIOs: no isolation, no protection. What a
 * pad does outside the firmware's control matters for a load on it:
 *   - TXD is UART0 TX. From every reset until aux_io_init() it carries the
 *     boot log (idle high), and indefinitely in download mode.
 *   - In input mode a pad has its pull-up on; RXD in its standard function
 *     too (UART RX idles high). Switching a PWM pad to another mode is
 *     therefore not a way to switch its load off -- duty 0 is.
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
    AUX_MODE_OUTPUT   = 4,      /* appended: the values are stored in NVS */
    AUX_MODE_COUNT
} aux_mode_t;

#define AUX_PAD_COUNT          3
/* The pads' GPIOs in pad order -- the one list; di.c builds its channel
   table from it. */
#define AUX_PAD_GPIOS          1, 44, 43

#define AUX_PWM_FREQ_MIN       5u          /* Hz; below this the 80 MHz clock cannot be divided down */
#define AUX_PWM_FREQ_MAX       1000000u    /* Hz; leaves 6 bits of duty resolution */
#define AUX_PWM_FREQ_DEFAULT   1000u
#define AUX_PWM_DUTY_MAX       1000u       /* duty is stored in tenths of a percent */
#define AUX_PWM_DUTY_KEEP      0xFFFFu

/* Whether pad a can be put in mode m. */
bool        aux_mode_allowed(uint8_t a, aux_mode_t m);
const char *aux_mode_name(aux_mode_t m);
/* Parses a mode name; false if it is not one. */
bool        aux_mode_parse(const char *s, aux_mode_t *out);

/* The input checks every interface shares (web, MQTT, Modbus): a duty cycle
   in percent, 0..100, to tenths of a percent; a frequency, a whole number of
   Hz in range. Checked on the double, before anything is cast. */
bool        aux_pwm_duty_from_pct(double pct, uint16_t *duty_pm);
bool        aux_pwm_freq_ok(double hz);

/* Applies the stored configuration to the pads. Call once after di_init();
   afterwards aux_io_apply() after every configuration change. Returns the
   first error; a pad that cannot take its mode is left in its standard
   function. */
esp_err_t   aux_io_init(void);
esp_err_t   aux_io_apply(void);

/* The mode pad a is in now. */
aux_mode_t  aux_io_mode(uint8_t a);

/* A PWM output's settings: the frequency and duty cycle it was given (not
   the quantised values the timer produces -- what Modbus reads back is what
   was written), and the duty resolution in bits. Any pointer may be NULL.
   False if pad a is not a PWM output. */
bool        aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint16_t *duty_pm, uint8_t *bits);
/* The frequency the timer really produces, Hz; 0 if not a PWM output. */
uint32_t    aux_io_pwm_actual_hz(uint8_t a);

/* Everything a status report shows of pad a, read at one moment: its mode
   and the values that belong to it -- an output's state, a PWM output's
   settings and real frequency, the analog reading. Read field by field
   through the getters above, a report could pair a mode with values from
   before or after a mode change. */
typedef struct {
    aux_mode_t mode;
    bool       out;                      /* AUX_MODE_OUTPUT                 */
    uint32_t   freq_hz, actual_hz;       /* AUX_MODE_PWM                    */
    uint16_t   duty_pm;
    uint8_t    bits;
    bool       mv_valid;                 /* AUX_MODE_ANALOG, with a reading */
    int        mv;
} aux_status_t;
void        aux_io_status(uint8_t a, aux_status_t *st);

/* Changes a running PWM output at run time -- Modbus, MQTT, rules. Not stored:
   the configuration keeps the values a reboot starts with, and a controller
   writing a duty cycle every second must not wear out the flash. freq_hz 0
   keeps the frequency, duty_pm AUX_PWM_DUTY_KEEP keeps the duty cycle.
   ESP_ERR_INVALID_STATE if the pad is not a PWM output, ESP_ERR_INVALID_ARG
   for a value out of range. */
esp_err_t   aux_io_pwm_set(uint8_t a, uint32_t freq_hz, uint16_t duty_pm);

/* A pad in output mode: its logical state (the pad's invert flag decides the
   level), set at run time and not stored -- it starts off after a reboot and
   whenever the pad enters output mode. ESP_ERR_INVALID_STATE if the pad is
   not an output. aux_io_out_get() is false then. */
esp_err_t   aux_io_out_set(uint8_t a, bool on);
esp_err_t   aux_io_out_toggle(uint8_t a);
bool        aux_io_out_get(uint8_t a, bool *on);

/* Writes to several pads at once (Modbus FC 15 and 16), all or nothing:
   every pad's mode and value is checked and the whole write applied under
   the lock aux_io_apply() takes, so a configuration saved in between cannot
   leave the first pads changed and the later ones refused. Pad by pad
   through aux_io_out_set()/aux_io_pwm_set() it could.

   aux_io_out_set_many(): the pads in mask (bit a = pad a) to the states in
   on. ESP_ERR_INVALID_STATE, and nothing set, if one of them is not an
   output.

   aux_io_pwm_write(): per pad the duty cycle and either half of the
   frequency as Modbus writes them -- an unwritten half is the frequency's
   as it was set; a pad with none of the three is left alone.
   ESP_ERR_INVALID_STATE if a pad written is not a PWM output,
   ESP_ERR_INVALID_ARG for a value out of range, and nothing changed. Should
   the LEDC driver refuse a pad, the pads already changed are put back and
   its error returned. */
typedef struct {
    bool     duty_set, hi_set, lo_set;
    uint16_t duty;                 /* 0.1 %                                  */
    uint16_t hi, lo;               /* the frequency's upper and lower 16 bits */
} aux_pwm_write_t;
esp_err_t   aux_io_out_set_many(uint8_t mask, uint8_t on);
esp_err_t   aux_io_pwm_write(const aux_pwm_write_t w[AUX_PAD_COUNT]);

/* The analog input of pad a in millivolts, as last sampled (every 200 ms,
   averaged over 16 conversions). ESP_ERR_INVALID_STATE if the pad is not an
   analog input or has no reading yet. */
esp_err_t   aux_io_analog_mv(uint8_t a, int *mv);

/* Change of the analog reading that counts as a new value -- for MQTT and
   the rule engine; below it the reading is noise. */
#define AUX_ANALOG_STEP_MV     20

/* MQTT, wired into main.c next to the other drivers:
     aux/<1..3>/mode       standard | input | pwm | analog  (published, retained)
     aux/<1..3>/pwm        {"duty":25.0,"freq":1000}         (published, retained;
                                                              emptied when not PWM)
     aux/<1..3>/pwm/set    25.0  or  {"duty":25.0,"freq":1000} (subscribed)
     aux/1/analog          millivolts                         (published)
     aux/read              publish all of the above           (subscribed)
   and for a pad in output mode, like DO1-DO8, under the pad's output name or
   its number 9..11:
     output/<name|9..11>      true | false                      (published)
     output/<name|9..11>/set  true/on/1, false/off/0, toggle    (subscribed)
     output/read              publish                           (subscribed) */
void        aux_io_on_mqtt_connected(void);
void        aux_io_on_mqtt_message(const char *topic, size_t topic_len,
                                   const char *data,  size_t data_len);
