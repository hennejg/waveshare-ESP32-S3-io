#include "aux_io.h"
#include "app_config.h"
#include "di.h"
#include "dout.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_log.h>

#ifdef CONFIG_APP_MQTT_ENABLE
#include "cJSON.h"
int  app_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain);
int  app_mqtt_subscribe(const char *topic, int qos);
bool app_mqtt_is_connected(void);
#endif
#ifdef CONFIG_APP_SCRIPTING_ENABLE
/* Declared rather than included: components/board does not depend on the
   scripting component. pad is 1..3; mv INT_MIN = the pad is no longer an
   analog input. False if the engine could not take it (not running yet,
   queue full) -- the caller tries again with its next sample. */
bool scripting_on_analog(uint8_t pad, int mv);
#endif

#if CONFIG_APP_AUX_DI_ENABLE
#include <driver/uart.h>
#include <driver/ledc.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_rom_gpio.h>
#include <esp_private/esp_gpio_reserve.h>
#include <soc/gpio_sig_map.h>
#endif

#define TAG "aux"

_Static_assert(sizeof(((app_config_t *)0)->aux_mode) == AUX_PAD_COUNT, "aux_mode[] is not one per pad");
_Static_assert(sizeof(((app_config_t *)0)->aux_pwm) / sizeof(((app_config_t *)0)->aux_pwm[0]) == AUX_PAD_COUNT,
               "aux_pwm[] is not one per pad");

static const char *const s_names[AUX_MODE_COUNT] = { "standard", "input", "pwm", "analog", "output" };

const char *aux_mode_name(aux_mode_t m)
{
    return (m < AUX_MODE_COUNT) ? s_names[m] : "standard";
}

bool aux_mode_parse(const char *s, aux_mode_t *out)
{
    for (int m = 0; m < AUX_MODE_COUNT; m++)
        if (s && strcmp(s, s_names[m]) == 0) { *out = (aux_mode_t)m; return true; }
    return false;
}

bool aux_mode_allowed(uint8_t a, aux_mode_t m)
{
    if (a >= AUX_PAD_COUNT || m >= AUX_MODE_COUNT) return false;
    /* Only GPIO1 has an ADC channel (ADC1 channel 0); GPIO43/44 have none. */
    if (m == AUX_MODE_ANALOG) return a == 0;
    return true;
}

bool aux_pwm_duty_from_pct(double pct, uint16_t *duty_pm)
{
    if (!(pct >= 0.0 && pct <= 100.0)) return false;          /* also NaN */
    *duty_pm = (uint16_t)lround(pct * 10.0);
    return true;
}

bool aux_pwm_freq_ok(double hz)
{
    return hz >= AUX_PWM_FREQ_MIN && hz <= AUX_PWM_FREQ_MAX && hz == floor(hz);
}

#if !CONFIG_APP_AUX_DI_ENABLE

esp_err_t  aux_io_init(void)  { return ESP_OK; }
esp_err_t  aux_io_apply(void) { return ESP_OK; }
aux_mode_t aux_io_mode(uint8_t a) { (void)a; return AUX_MODE_STANDARD; }
bool aux_io_pwm_info(uint8_t a, uint32_t *f, uint16_t *d, uint8_t *b) { (void)a; (void)f; (void)d; (void)b; return false; }
uint32_t aux_io_pwm_actual_hz(uint8_t a) { (void)a; return 0; }
void aux_io_status(uint8_t a, aux_status_t *st) { (void)a; memset(st, 0, sizeof(*st)); st->mode = AUX_MODE_STANDARD; }
esp_err_t aux_io_pwm_set(uint8_t a, uint32_t f, uint16_t d) { (void)a; (void)f; (void)d; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t aux_io_analog_mv(uint8_t a, int *mv) { (void)a; (void)mv; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t aux_io_out_set(uint8_t a, bool on) { (void)a; (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t aux_io_out_toggle(uint8_t a) { (void)a; return ESP_ERR_NOT_SUPPORTED; }
bool aux_io_out_get(uint8_t a, bool *on) { (void)a; (void)on; return false; }
esp_err_t aux_io_out_set_many(uint8_t m, uint8_t on) { (void)m; (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t aux_io_pwm_write(const aux_pwm_write_t w[AUX_PAD_COUNT]) { (void)w; return ESP_ERR_NOT_SUPPORTED; }
void aux_io_on_mqtt_connected(void) {}
void aux_io_on_mqtt_message(const char *t, size_t tl, const char *d, size_t dl) { (void)t; (void)tl; (void)d; (void)dl; }

#else

static const gpio_num_t s_gpio[AUX_PAD_COUNT] = { AUX_PAD_GPIOS };

/* LEDC: the buzzer has timer 0 and channel 0. Each pad gets a timer of its
   own, so the three frequencies are independent. The clock is fixed to APB:
   the low-speed timers share one clock source, and LEDC_AUTO_CLK could pick
   another for a low frequency and collide with the buzzer's timer. */
#define PWM_MODE        LEDC_LOW_SPEED_MODE
#define PWM_TIMER(a)    ((ledc_timer_t)(LEDC_TIMER_1 + (a)))
#define PWM_CHAN(a)     ((ledc_channel_t)(LEDC_CHANNEL_1 + (a)))
#define PWM_SRC_HZ      80000000u
#define PWM_BITS_MAX    14u

/* Everything below is under s_mux, and nothing under s_mux publishes, takes
   another driver's lock that does, or waits on the network: the MQTT task
   calls in here while it holds its client's API lock. */
static SemaphoreHandle_t s_mux;
static aux_mode_t        s_mode[AUX_PAD_COUNT];
static uint32_t          s_freq[AUX_PAD_COUNT];   /* as given, Hz              */
static uint16_t          s_duty[AUX_PAD_COUNT];   /* as given, 0.1 %           */
static uint8_t           s_bits[AUX_PAD_COUNT];

/* The configuration's PWM values as last applied. A save that does not touch
   them must not overwrite a duty cycle set at run time. */
static uint32_t          s_cfg_freq[AUX_PAD_COUNT];
static uint16_t          s_cfg_duty[AUX_PAD_COUNT];

/* Output mode: the logical state, and the invert flag it was driven with --
   a save that changes the flag re-drives the pin. */
static bool              s_out[AUX_PAD_COUNT];
static bool              s_out_inv[AUX_PAD_COUNT];

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int                       s_an_mv;         /* last sample, mV          */
static bool                      s_an_valid;      /* s_an_mv is current       */

/* Publishing is left to the pad task, so that nothing that changes a pad --
   a Modbus request, a rule, an HTTP save -- waits on the MQTT client. */
static TaskHandle_t      s_task;
static volatile uint8_t  s_dirty;          /* bit a: publish pad a's state */

static void mark_dirty(uint8_t a)
{
    __atomic_fetch_or(&s_dirty, (uint8_t)(1u << a), __ATOMIC_RELAXED);
    if (s_task) xTaskNotifyGive(s_task);
}

/* ------------------------------------------------------------ pad function */

/* Off UART0. The next mode's gpio_config()/ledc_channel_config() selects the
   plain GPIO function, which takes U0TXD off GPIO43; U0RXD is fed a constant
   high instead of GPIO44, so the UART sees an idle line rather than whatever
   is on the pad. The console reserved both pins at boot. */
static void detach_uart(gpio_num_t g)
{
    if (g == GPIO_NUM_43 || g == GPIO_NUM_44) esp_gpio_revoke(BIT64(g));
    if (g == GPIO_NUM_44)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);
}

/* Back to the standard function. The reset first: it clears an output enable
   a PWM channel left behind, which the IO MUX path of uart_set_pin() would
   not, and gives back a GPIO reservation. GPIO1 is then left floating with
   its input off -- no pull-up to switch on a load that was driven by PWM;
   GPIO44/43 go back to UART0 through the IO MUX, which needs no UART driver. */
static void restore_standard(gpio_num_t g)
{
    gpio_reset_pin(g);
    if (g == GPIO_NUM_43)
        uart_set_pin(UART_NUM_0, GPIO_NUM_43, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    else if (g == GPIO_NUM_44)
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, GPIO_NUM_44,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    else
        gpio_set_pull_mode(g, GPIO_FLOATING);
}

/* ------------------------------------------------------------------- PWM */

static uint32_t duty_ticks(uint16_t duty_pm, uint8_t bits)
{
    return (uint32_t)(((uint64_t)duty_pm * (1u << bits) + AUX_PWM_DUTY_MAX / 2) / AUX_PWM_DUTY_MAX);
}

/* The duty cycle onto a configured channel. 100 % is the channel stopped
   with its idle level high, never a duty of 2^bits: at the S3's maximum
   resolution of 14 bits that overflows the hardware duty counter (ESP-IDF
   LEDC documentation), which is every frequency up to 4.88 kHz here.
   ledc_update_duty() starts a stopped channel again. */
static void pwm_put_duty(uint8_t a, uint16_t duty, uint8_t bits)
{
    if (duty >= AUX_PWM_DUTY_MAX) {
        ledc_stop(PWM_MODE, PWM_CHAN(a), 1);
    } else {
        ledc_set_duty(PWM_MODE, PWM_CHAN(a), duty_ticks(duty, bits));
        ledc_update_duty(PWM_MODE, PWM_CHAN(a));
    }
}

static esp_err_t pwm_set(uint8_t a, uint32_t freq, uint16_t duty, bool fresh)
{
    uint32_t bits = ledc_find_suitable_duty_resolution(PWM_SRC_HZ, freq);
    if (bits > PWM_BITS_MAX) bits = PWM_BITS_MAX;
    if (bits < 1) return ESP_ERR_INVALID_ARG;

    if (fresh || freq != s_freq[a]) {
        /* A new resolution takes effect with the timer reset, a new duty
           only at the next period: in between the old tick count would run
           against the new counter. Held low for that moment instead. */
        if (!fresh && bits != s_bits[a]) ledc_stop(PWM_MODE, PWM_CHAN(a), 0);
        ledc_timer_config_t t = {
            .speed_mode      = PWM_MODE,
            .timer_num       = PWM_TIMER(a),
            .duty_resolution = (ledc_timer_bit_t)bits,
            .freq_hz         = freq,
            .clk_cfg         = LEDC_USE_APB_CLK,
        };
        esp_err_t r = ledc_timer_config(&t);
        if (r != ESP_OK) return r;
    }
    if (fresh) {
        ledc_channel_config_t c = {
            .gpio_num   = s_gpio[a],
            .speed_mode = PWM_MODE,
            .channel    = PWM_CHAN(a),
            .timer_sel  = PWM_TIMER(a),
            .duty       = 0,
            .hpoint     = 0,
        };
        esp_err_t r = ledc_channel_config(&c);
        if (r != ESP_OK) return r;
    }
    pwm_put_duty(a, duty, (uint8_t)bits);
    s_freq[a] = freq; s_duty[a] = duty; s_bits[a] = (uint8_t)bits;
    return ESP_OK;
}

/* Output low, and the pin given back: ledc_channel_config() reserved it, and
   the next ledc_channel_config() on it would otherwise warn of a conflict. */
static void pwm_stop(uint8_t a)
{
    ledc_stop(PWM_MODE, PWM_CHAN(a), 0);
    esp_gpio_revoke(BIT64(s_gpio[a]));
}

/* ---------------------------------------------------------------- output */

static void out_drive(uint8_t a)
{
    gpio_set_level(s_gpio[a], s_out[a] ^ s_out_inv[a]);
}

/* The level is set before the pad becomes an output, so it does not show
   whatever the output register last held for a moment. */
static esp_err_t out_start(uint8_t a, bool invert)
{
    s_out[a] = false;
    s_out_inv[a] = invert;
    out_drive(a);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_gpio[a],
        .mode         = GPIO_MODE_INPUT_OUTPUT,   /* the level reads back */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t aux_io_out_set(uint8_t a, bool on)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t r = ESP_ERR_INVALID_STATE;
    if (s_mode[a] == AUX_MODE_OUTPUT) {
        r = ESP_OK;
        if (s_out[a] != on) { s_out[a] = on; out_drive(a); mark_dirty(a); }
    }
    xSemaphoreGive(s_mux);
    return r;
}

esp_err_t aux_io_out_toggle(uint8_t a)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* read and flip as one */
    esp_err_t r = ESP_ERR_INVALID_STATE;
    if (s_mode[a] == AUX_MODE_OUTPUT) {
        s_out[a] = !s_out[a]; out_drive(a); mark_dirty(a); r = ESP_OK;
    }
    xSemaphoreGive(s_mux);
    return r;
}

esp_err_t aux_io_out_set_many(uint8_t mask, uint8_t on)
{
    if (!s_mux || (mask >> AUX_PAD_COUNT)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* checked and set as one */
    esp_err_t r = ESP_OK;
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++)
        if (((mask >> a) & 1u) && s_mode[a] != AUX_MODE_OUTPUT) r = ESP_ERR_INVALID_STATE;
    if (r == ESP_OK)
        for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
            bool v = (on >> a) & 1u;
            if (((mask >> a) & 1u) && s_out[a] != v) { s_out[a] = v; out_drive(a); mark_dirty(a); }
        }
    xSemaphoreGive(s_mux);
    return r;
}

bool aux_io_out_get(uint8_t a, bool *on)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* mode and state together */
    bool out = s_mode[a] == AUX_MODE_OUTPUT;
    if (out && on) *on = s_out[a];
    xSemaphoreGive(s_mux);
    return out;
}

/* ---------------------------------------------------------------- analog */

static esp_err_t analog_start(void)
{
    if (!s_adc) {
        adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
        esp_err_t r = adc_oneshot_new_unit(&u, &s_adc);
        if (r != ESP_OK) return r;
    }
    /* Also puts the pad in analog mode: input off, no pulls. */
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    esp_err_t r = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c);
    if (r != ESP_OK) return r;

    if (!s_cali) {
        adc_cali_curve_fitting_config_t k = {
            .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0,
            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&k, &s_cali) != ESP_OK) {
            s_cali = NULL;
            ESP_LOGW(TAG, "no ADC calibration in eFuse -- readings are uncalibrated");
        }
    }
    s_an_valid = false;
    return ESP_OK;
}

/* One averaged reading; under s_mux. A single conversion moves by tens of
   millivolts. */
static bool analog_sample(int *mv)
{
    int sum = 0, raw = 0, n = 0;
    for (int i = 0; i < 16; i++)
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) == ESP_OK) { sum += raw; n++; }
    if (!n) return false;
    raw = (sum + n / 2) / n;
    if (s_cali) return adc_cali_raw_to_voltage(s_cali, raw, mv) == ESP_OK;
    *mv = (int)((int64_t)raw * 3100 / 4095);
    return true;
}

esp_err_t aux_io_analog_mv(uint8_t a, int *mv)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t r = ESP_ERR_INVALID_STATE;
    if (s_mode[a] == AUX_MODE_ANALOG && s_an_valid) { *mv = s_an_mv; r = ESP_OK; }
    xSemaphoreGive(s_mux);
    return r;
}

/* ------------------------------------------------------------- switching */

static void leave(uint8_t a)
{
    switch (s_mode[a]) {
    case AUX_MODE_INPUT:  di_aux_input(a, false); break;
    case AUX_MODE_PWM:    pwm_stop(a);            break;
    case AUX_MODE_ANALOG: s_an_valid = false;     break;
    case AUX_MODE_OUTPUT: s_out[a] = false; out_drive(a); break;   /* off first */
    default:                                      break;
    }
}

/* Puts pad a into mode m, freq/duty for PWM. On failure the pad is left in
   its standard function, never half in one mode. */
static esp_err_t enter(uint8_t a, aux_mode_t m, uint32_t freq, uint16_t duty, bool invert)
{
    gpio_num_t g = s_gpio[a];
    esp_err_t  r = ESP_OK;

    leave(a);
    if (m == AUX_MODE_STANDARD) {
        restore_standard(g);
        s_mode[a] = m;
        ESP_LOGI(TAG, "AUX%u (GPIO%d): standard function", a + 1, g);
        return ESP_OK;
    }
    if (s_mode[a] == AUX_MODE_STANDARD) detach_uart(g);

    switch (m) {
    case AUX_MODE_INPUT:  di_aux_input(a, true);           break;
    case AUX_MODE_PWM:    r = pwm_set(a, freq, duty, true); break;
    case AUX_MODE_ANALOG: r = analog_start();               break;
    case AUX_MODE_OUTPUT: r = out_start(a, invert);         break;
    default:              r = ESP_ERR_INVALID_ARG;          break;
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "AUX%u (GPIO%d): %s failed (%s), back to standard function",
                 a + 1, g, aux_mode_name(m), esp_err_to_name(r));
        if (m == AUX_MODE_PWM) pwm_stop(a);
        restore_standard(g);
        s_mode[a] = AUX_MODE_STANDARD;
        return r;
    }
    s_mode[a] = m;
    if (m == AUX_MODE_PWM)
        ESP_LOGW(TAG, "AUX%u (GPIO%d): NON-ISOLATED PWM output, %lu Hz, %u.%u %%, %u bit",
                 a + 1, g, (unsigned long)freq, duty / 10, duty % 10, s_bits[a]);
    else
        ESP_LOGW(TAG, "AUX%u (GPIO%d): NON-ISOLATED %s", a + 1, g, aux_mode_name(m));
    return ESP_OK;
}

esp_err_t aux_io_apply(void)
{
    if (!s_mux) return ESP_ERR_INVALID_STATE;
    const app_config_t *cfg = app_config_get();
    esp_err_t first = ESP_OK;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
        aux_mode_t m = (aux_mode_t)cfg->aux_mode[a];
        if (!aux_mode_allowed(a, m)) m = AUX_MODE_STANDARD;
        uint32_t f = cfg->aux_pwm[a].freq_hz;
        uint16_t d = cfg->aux_pwm[a].duty_pm;
        bool     inv = cfg->aux_out[a].invert;
        bool cfg_changed = f != s_cfg_freq[a] || d != s_cfg_duty[a];

        esp_err_t r = ESP_OK;
        if (m == s_mode[a]) {
            /* Same mode: only new configured PWM values are applied. A save of
               something else leaves a run-time duty cycle where it is. */
            if (m == AUX_MODE_PWM && cfg_changed) {
                r = pwm_set(a, f, d, false);
                if (r != ESP_OK)
                    ESP_LOGE(TAG, "AUX%u: PWM %lu Hz failed (%s)", a + 1,
                             (unsigned long)f, esp_err_to_name(r));
                mark_dirty(a);
            }
            if (m == AUX_MODE_OUTPUT && inv != s_out_inv[a]) {
                s_out_inv[a] = inv;                    /* same state, new level */
                out_drive(a);
            }
        } else {
            r = enter(a, m, f, d, inv);
            mark_dirty(a);
        }
        /* Remembered only once applied, so that saving the same values again
           tries again. */
        if (r == ESP_OK) { s_cfg_freq[a] = f; s_cfg_duty[a] = d; }
        else if (first == ESP_OK) first = r;
    }
    xSemaphoreGive(s_mux);
    return first;
}

esp_err_t aux_io_pwm_set(uint8_t a, uint32_t freq_hz, uint16_t duty_pm)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return ESP_ERR_INVALID_ARG;
    if (freq_hz && (freq_hz < AUX_PWM_FREQ_MIN || freq_hz > AUX_PWM_FREQ_MAX))
        return ESP_ERR_INVALID_ARG;
    if (duty_pm != AUX_PWM_DUTY_KEEP && duty_pm > AUX_PWM_DUTY_MAX)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t r = ESP_ERR_INVALID_STATE;
    if (s_mode[a] == AUX_MODE_PWM) {
        uint32_t f = freq_hz ? freq_hz : s_freq[a];
        uint16_t d = (duty_pm == AUX_PWM_DUTY_KEEP) ? s_duty[a] : duty_pm;
        if (f == s_freq[a] && d == s_duty[a]) {
            r = ESP_OK;                      /* nothing moves, nothing to tell */
        } else {
            r = pwm_set(a, f, d, false);
            if (r == ESP_OK) mark_dirty(a);
        }
    }
    xSemaphoreGive(s_mux);
    return r;
}

esp_err_t aux_io_pwm_write(const aux_pwm_write_t w[AUX_PAD_COUNT])
{
    if (!s_mux) return ESP_ERR_INVALID_ARG;
    uint32_t f[AUX_PAD_COUNT], old_f[AUX_PAD_COUNT];
    uint16_t d[AUX_PAD_COUNT], old_d[AUX_PAD_COUNT];
    uint8_t  todo = 0, done = 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);           /* checked and set as one */
    esp_err_t r = ESP_OK;
    for (uint8_t a = 0; a < AUX_PAD_COUNT && r == ESP_OK; a++) {
        if (!w[a].duty_set && !w[a].hi_set && !w[a].lo_set) continue;
        if (s_mode[a] != AUX_MODE_PWM) { r = ESP_ERR_INVALID_STATE; break; }
        uint32_t hi = w[a].hi_set ? w[a].hi : (uint16_t)(s_freq[a] >> 16);
        uint32_t lo = w[a].lo_set ? w[a].lo : (uint16_t)s_freq[a];
        f[a] = (hi << 16) | lo;
        d[a] = w[a].duty_set ? w[a].duty : s_duty[a];
        if (f[a] < AUX_PWM_FREQ_MIN || f[a] > AUX_PWM_FREQ_MAX || d[a] > AUX_PWM_DUTY_MAX)
            r = ESP_ERR_INVALID_ARG;
        else if (f[a] != s_freq[a] || d[a] != s_duty[a])
            todo |= (uint8_t)(1u << a);
    }
    for (uint8_t a = 0; a < AUX_PAD_COUNT && r == ESP_OK; a++) {
        if (!((todo >> a) & 1u)) continue;
        old_f[a] = s_freq[a]; old_d[a] = s_duty[a];
        r = pwm_set(a, f[a], d[a], false);
        if (r == ESP_OK) done |= (uint8_t)(1u << a);
    }
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
        if (!((done >> a) & 1u)) continue;
        if (r != ESP_OK) pwm_set(a, old_f[a], old_d[a], false);   /* back as it was */
        else             mark_dirty(a);
    }
    xSemaphoreGive(s_mux);
    return r;
}

/* The getters take s_mux like everything that changes a pad: read without
   it, a status query running beside aux_io_apply() could see a pad between
   two modes -- an output's state after leave() cleared it, a PWM timer
   being reconfigured. */
aux_mode_t aux_io_mode(uint8_t a)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return AUX_MODE_STANDARD;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    aux_mode_t m = s_mode[a];
    xSemaphoreGive(s_mux);
    return m;
}

void aux_io_status(uint8_t a, aux_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->mode = AUX_MODE_STANDARD;
    if (a >= AUX_PAD_COUNT || !s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* one pad, one moment */
    st->mode = s_mode[a];
    switch (st->mode) {
    case AUX_MODE_OUTPUT:
        st->out = s_out[a];
        break;
    case AUX_MODE_PWM:
        st->freq_hz   = s_freq[a];
        st->duty_pm   = s_duty[a];
        st->bits      = s_bits[a];
        st->actual_hz = ledc_get_freq(PWM_MODE, PWM_TIMER(a));
        break;
    case AUX_MODE_ANALOG:
        st->mv_valid = s_an_valid;
        st->mv       = s_an_mv;
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mux);
}

bool aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint16_t *duty_pm, uint8_t *bits)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* the three values together */
    bool on = s_mode[a] == AUX_MODE_PWM;
    if (on) {
        if (freq_hz) *freq_hz = s_freq[a];
        if (duty_pm) *duty_pm = s_duty[a];
        if (bits)    *bits    = s_bits[a];
    }
    xSemaphoreGive(s_mux);
    return on;
}

uint32_t aux_io_pwm_actual_hz(uint8_t a)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);           /* not while the timer is reconfigured */
    uint32_t hz = s_mode[a] == AUX_MODE_PWM ? ledc_get_freq(PWM_MODE, PWM_TIMER(a)) : 0;
    xSemaphoreGive(s_mux);
    return hz;
}

/* ------------------------------------------------------------- pad task */

#define SAMPLE_MS 200

/* What the rule engine and MQTT were last told about the analog input. Kept
   apart: a reading the engine could not take is offered again, while MQTT
   republishes on reconnect anyway. Pad task only. */
static bool s_an_rules_has;
static int  s_an_rules_mv;
static bool s_an_mqtt_has;
static int  s_an_mqtt_mv;

#ifdef CONFIG_APP_MQTT_ENABLE
/* "output/<name>" or "output/<9..11>" plus suffix: numbered after DO1-DO8,
   as the pads' inputs are numbered after DI1-DI8. */
static void out_topic(uint8_t a, char *buf, size_t len, const char *suffix)
{
    const char *name = app_config_get()->aux_out[a].name;
    if (name[0]) snprintf(buf, len, "output/%.20s%s", name, suffix);
    else         snprintf(buf, len, "output/%u%s", (unsigned)(APP_CFG_DO_COUNT + 1 + a), suffix);
}

static void publish_analog(int mv)
{
    char payload[16];
    snprintf(payload, sizeof(payload), "%d", mv);
    app_mqtt_publish("aux/1/analog", payload, -1, 0, false);
    s_an_mqtt_has = true;
    s_an_mqtt_mv  = mv;
}

static void publish_pad(uint8_t a)
{
    if (!app_mqtt_is_connected()) return;
    char topic[40], payload[48];
    aux_status_t st;                                /* one snapshot, published unlocked */
    aux_io_status(a, &st);
    snprintf(topic, sizeof(topic), "aux/%u/mode", a + 1);
    app_mqtt_publish(topic, aux_mode_name(st.mode), -1, 0, true);
    snprintf(topic, sizeof(topic), "aux/%u/pwm", a + 1);
    if (st.mode == AUX_MODE_PWM) {
        snprintf(payload, sizeof(payload), "{\"duty\":%u.%u,\"freq\":%lu}",
                 st.duty_pm / 10, st.duty_pm % 10, (unsigned long)st.freq_hz);
        app_mqtt_publish(topic, payload, -1, 0, true);
    } else {
        /* Not a PWM output (any more): an empty retained payload removes the
           old state, so a new subscriber is not told it still runs. */
        app_mqtt_publish(topic, "", 0, 0, true);
    }
    if (a == 0 && st.mode == AUX_MODE_ANALOG && st.mv_valid) publish_analog(st.mv);
    if (st.mode == AUX_MODE_OUTPUT) {
        out_topic(a, topic, sizeof(topic), "");
        app_mqtt_publish(topic, st.out ? "true" : "false", -1, 0, false);
    }
}
#endif

static void aux_task(void *arg)
{
    (void)arg;
    TickType_t next_sample = xTaskGetTickCount();
    for (;;) {
        /* Sampled on its own clock, and only while IO1 is analog: woken early
           to publish, sampling again on that wake fed a rule that sets a PWM
           output from the reading a new reading, which set the PWM again. */
        TickType_t wait = portMAX_DELAY;
        if (aux_io_mode(0) == AUX_MODE_ANALOG || s_an_rules_has) {
            TickType_t now = xTaskGetTickCount();
            wait = ((int32_t)(next_sample - now) > 0) ? (TickType_t)(next_sample - now) : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        uint8_t dirty = __atomic_exchange_n(&s_dirty, 0, __ATOMIC_RELAXED);
#ifdef CONFIG_APP_MQTT_ENABLE
        for (uint8_t a = 0; a < AUX_PAD_COUNT; a++)
            if (dirty & (1u << a)) publish_pad(a);
#else
        (void)dirty;
#endif

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_sample) < 0) continue;      /* not time yet */
        next_sample = now + pdMS_TO_TICKS(SAMPLE_MS);

        int  mv = 0;
        bool have = false;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_mode[0] == AUX_MODE_ANALOG && s_adc) {
            have = analog_sample(&mv);
            s_an_mv = mv;
            s_an_valid = have;
        }
        xSemaphoreGive(s_mux);

        /* Outside the lock from here on: both of these may wait. */
#ifdef CONFIG_APP_SCRIPTING_ENABLE
        if (have) {
            if ((!s_an_rules_has || abs(mv - s_an_rules_mv) >= AUX_ANALOG_STEP_MV) &&
                scripting_on_analog(1, mv)) {
                s_an_rules_has = true;
                s_an_rules_mv  = mv;
            }
        } else if (s_an_rules_has && aux_io_mode(0) != AUX_MODE_ANALOG) {
            /* IO1 has left analog mode: the engine forgets its last reading. */
            if (scripting_on_analog(1, INT_MIN)) s_an_rules_has = false;
        }
#endif
#ifdef CONFIG_APP_MQTT_ENABLE
        if (have && app_mqtt_is_connected() &&
            (!s_an_mqtt_has || abs(mv - s_an_mqtt_mv) >= AUX_ANALOG_STEP_MV))
            publish_analog(mv);
        if (!have) s_an_mqtt_has = false;
#endif
    }
}

esp_err_t aux_io_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;
    if (xTaskCreate(aux_task, "aux", 3584, NULL, 4, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return aux_io_apply();
}

/* ------------------------------------------------------------------ MQTT */

void aux_io_on_mqtt_connected(void)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    app_mqtt_subscribe("aux/+/pwm/set", 0);
    app_mqtt_subscribe("aux/read", 0);
    /* Output command topics for every pad, whatever its mode: a pad switched
       to output later is then reachable without a reconnect. Called again
       after a save, for changed names. */
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
        char topic[40];
        out_topic(a, topic, sizeof(topic), "/set");
        app_mqtt_subscribe(topic, 0);
    }
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) mark_dirty(a);
#endif
}

void aux_io_on_mqtt_message(const char *topic, size_t tlen, const char *data, size_t dlen)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    static const char READ[] = "aux/read";
    if (tlen == sizeof(READ) - 1 && memcmp(topic, READ, tlen) == 0) {
        for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) mark_dirty(a);
        return;
    }
    static const char OREAD[] = "output/read";
    if (tlen == sizeof(OREAD) - 1 && memcmp(topic, OREAD, tlen) == 0) {
        for (uint8_t a = 0; a < AUX_PAD_COUNT; a++)
            if (aux_io_mode(a) == AUX_MODE_OUTPUT) mark_dirty(a);
        return;
    }
    for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
        char cmd[40];
        out_topic(a, cmd, sizeof(cmd), "/set");
        size_t clen = strlen(cmd);
        if (tlen != clen || memcmp(topic, cmd, clen) != 0) continue;
        bool st;
        esp_err_t r;
        if (dout_parse_toggle(data, dlen))            r = aux_io_out_toggle(a);
        else if (dout_parse_state(data, dlen, &st))   r = aux_io_out_set(a, st);
        else { ESP_LOGW(TAG, "%s: unrecognised payload", cmd); return; }
        if (r == ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "%s: pad is not an output", cmd);
        return;
    }

    /* aux/<n>/pwm/set, compared whole. */
    static const char TAIL[] = "/pwm/set";
    const size_t tail = sizeof(TAIL) - 1;
    if (tlen != 4 + 1 + tail || memcmp(topic, "aux/", 4) != 0 ||
        memcmp(topic + 5, TAIL, tail) != 0 || topic[4] < '1' || topic[4] > '3')
        return;
    uint8_t a = (uint8_t)(topic[4] - '1');

    char buf[96];
    if (dlen >= sizeof(buf)) { ESP_LOGW(TAG, "aux/%u/pwm/set: payload too long", a + 1); return; }
    memcpy(buf, data, dlen);
    buf[dlen] = '\0';

    /* A bare number is a duty cycle; an object may carry duty, freq or both.
       Whatever is present has to be a number in range -- a field that is
       there but wrong is refused, never read as "keep". */
    bool    has_duty = false, has_freq = false, bad = false;
    double  duty = 0, freq = 0;
    cJSON  *j = cJSON_Parse(buf);
    if (cJSON_IsNumber(j)) {
        has_duty = true; duty = j->valuedouble;
    } else if (cJSON_IsObject(j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(j, "duty"))) { has_duty = true; bad |= !cJSON_IsNumber(v); duty = v->valuedouble; }
        if ((v = cJSON_GetObjectItem(j, "freq"))) { has_freq = true; bad |= !cJSON_IsNumber(v); freq = v->valuedouble; }
        bad |= !has_duty && !has_freq;
    } else {
        bad = true;
    }
    cJSON_Delete(j);

    uint16_t d = AUX_PWM_DUTY_KEEP;
    if (!bad && has_duty && !aux_pwm_duty_from_pct(duty, &d)) bad = true;
    if (!bad && has_freq && !aux_pwm_freq_ok(freq)) bad = true;
    if (bad) {
        ESP_LOGW(TAG, "aux/%u/pwm/set: expected a duty cycle 0..100 or "
                      "{\"duty\":0..100,\"freq\":%u..%u}", a + 1, AUX_PWM_FREQ_MIN, AUX_PWM_FREQ_MAX);
        return;
    }
    esp_err_t r = aux_io_pwm_set(a, has_freq ? (uint32_t)freq : 0, d);
    if (r != ESP_OK)
        ESP_LOGW(TAG, "aux/%u/pwm/set: %s", a + 1,
                 r == ESP_ERR_INVALID_STATE ? "pad is not a PWM output" : esp_err_to_name(r));
#else
    (void)topic; (void)tlen; (void)data; (void)dlen;
#endif
}

#endif /* CONFIG_APP_AUX_DI_ENABLE */
