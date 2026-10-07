#include "aux_io.h"
#include "app_config.h"
#include "di.h"

#include <string.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>

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

static const char *const s_names[AUX_MODE_COUNT] = { "standard", "input", "pwm", "analog" };

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

#if !CONFIG_APP_AUX_DI_ENABLE

esp_err_t  aux_io_init(void)  { return ESP_OK; }
esp_err_t  aux_io_apply(void) { return ESP_OK; }
aux_mode_t aux_io_mode(uint8_t a) { (void)a; return AUX_MODE_STANDARD; }
bool aux_io_pwm_info(uint8_t a, uint32_t *f, uint8_t *b) { (void)a; (void)f; (void)b; return false; }
esp_err_t aux_io_analog_mv(uint8_t a, int *mv) { (void)a; (void)mv; return ESP_ERR_NOT_SUPPORTED; }

#else

static const gpio_num_t s_gpio[AUX_PAD_COUNT] = { GPIO_NUM_1, GPIO_NUM_44, GPIO_NUM_43 };

/* LEDC: the buzzer has timer 0 and channel 0. Each pad gets a timer of its
   own, so the three frequencies are independent. The clock is fixed to APB:
   the low-speed timers share one clock source, and LEDC_AUTO_CLK could pick
   another for a low frequency and collide with the buzzer's timer. */
#define PWM_MODE        LEDC_LOW_SPEED_MODE
#define PWM_TIMER(a)    ((ledc_timer_t)(LEDC_TIMER_1 + (a)))
#define PWM_CHAN(a)     ((ledc_channel_t)(LEDC_CHANNEL_1 + (a)))
#define PWM_SRC_HZ      80000000u
#define PWM_BITS_MAX    14u

static SemaphoreHandle_t s_mux;
static aux_mode_t        s_mode[AUX_PAD_COUNT];
static uint32_t          s_freq[AUX_PAD_COUNT];
static uint16_t          s_duty[AUX_PAD_COUNT];
static uint8_t           s_bits[AUX_PAD_COUNT];

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

/* ------------------------------------------------------------ pad function */

/* Off UART0. gpio_config() in the next mode selects the plain GPIO function,
   which takes U0TXD off GPIO43; U0RXD is fed a constant high instead of
   GPIO44, so the UART sees an idle line rather than whatever is on the pad.
   The console reserved both pins at boot. */
static void detach_uart(gpio_num_t g)
{
    if (g == GPIO_NUM_43 || g == GPIO_NUM_44) esp_gpio_revoke(BIT64(g));
    if (g == GPIO_NUM_44)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);
}

/* Back to the standard function. The reset first: it clears an output enable
   a PWM channel left behind, which the IO MUX path of uart_set_pin() would
   not. GPIO1 stays as gpio_reset_pin() leaves it, an input with pull-up;
   GPIO44/43 go back to UART0, which needs no UART driver installed. */
static void restore_standard(gpio_num_t g)
{
    gpio_reset_pin(g);
    if (g == GPIO_NUM_43)
        uart_set_pin(UART_NUM_0, GPIO_NUM_43, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    else if (g == GPIO_NUM_44) {
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, GPIO_NUM_44,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        esp_rom_gpio_connect_in_signal(GPIO_NUM_44, U0RXD_IN_IDX, false);
    }
}

/* ------------------------------------------------------------------- PWM */

static uint32_t duty_ticks(uint16_t duty_pm, uint8_t bits)
{
    uint32_t full = 1u << bits;
    /* 100 % has to be the full count, which LEDC takes as "always high". */
    return (uint32_t)(((uint64_t)duty_pm * full + AUX_PWM_DUTY_MAX / 2) / AUX_PWM_DUTY_MAX);
}

static esp_err_t pwm_set(uint8_t a, uint32_t freq, uint16_t duty, bool fresh)
{
    uint32_t bits = ledc_find_suitable_duty_resolution(PWM_SRC_HZ, freq);
    if (bits > PWM_BITS_MAX) bits = PWM_BITS_MAX;
    if (bits < 1) return ESP_ERR_INVALID_ARG;

    if (fresh || freq != s_freq[a] || bits != s_bits[a]) {
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
            .duty       = duty_ticks(duty, (uint8_t)bits),
            .hpoint     = 0,
        };
        esp_err_t r = ledc_channel_config(&c);
        if (r != ESP_OK) return r;
    } else {
        ledc_set_duty(PWM_MODE, PWM_CHAN(a), duty_ticks(duty, (uint8_t)bits));
        ledc_update_duty(PWM_MODE, PWM_CHAN(a));
    }
    s_freq[a] = freq; s_duty[a] = duty; s_bits[a] = (uint8_t)bits;
    return ESP_OK;
}

static void pwm_stop(uint8_t a)
{
    ledc_stop(PWM_MODE, PWM_CHAN(a), 0);
    s_freq[a] = 0; s_bits[a] = 0;
}

/* ---------------------------------------------------------------- analog */

static esp_err_t analog_start(void)
{
    if (!s_adc) {
        adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
        esp_err_t r = adc_oneshot_new_unit(&u, &s_adc);
        if (r != ESP_OK) return r;
    }
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    esp_err_t r = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c);
    if (r != ESP_OK) return r;
    /* Floating: the pull-up would add its own current to the reading. */
    gpio_set_pull_mode(GPIO_NUM_1, GPIO_FLOATING);

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
    return ESP_OK;
}

esp_err_t aux_io_analog_mv(uint8_t a, int *mv)
{
    if (a >= AUX_PAD_COUNT || !s_mux) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t r = ESP_ERR_INVALID_STATE;
    if (s_mode[a] == AUX_MODE_ANALOG && s_adc) {
        /* Averaged: one conversion moves by tens of millivolts. */
        int sum = 0, raw = 0, n = 0;
        for (int i = 0; i < 16; i++)
            if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) == ESP_OK) { sum += raw; n++; }
        if (n) {
            raw = (sum + n / 2) / n;
            if (s_cali) r = adc_cali_raw_to_voltage(s_cali, raw, mv);
            else { *mv = (int)((int64_t)raw * 3100 / 4095); r = ESP_OK; }
        } else r = ESP_FAIL;
    }
    xSemaphoreGive(s_mux);
    return r;
}

/* ------------------------------------------------------------- switching */

static void leave(uint8_t a)
{
    switch (s_mode[a]) {
    case AUX_MODE_INPUT:  di_aux_input(a, false); break;
    case AUX_MODE_PWM:    pwm_stop(a);            break;
    default:                                      break;
    }
}

/* Puts pad a into mode m, freq/duty for PWM. On failure the pad is left in
   its standard function, never half in one mode. */
static esp_err_t enter(uint8_t a, aux_mode_t m, uint32_t freq, uint16_t duty)
{
    gpio_num_t g = s_gpio[a];
    esp_err_t  r = ESP_OK;

    if (m == s_mode[a]) {
        if (m == AUX_MODE_PWM && (freq != s_freq[a] || duty != s_duty[a]))
            r = pwm_set(a, freq, duty, false);
        return r;
    }

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
    default:              r = ESP_ERR_INVALID_ARG;          break;
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "AUX%u (GPIO%d): %s failed (%s), back to standard function",
                 a + 1, g, aux_mode_name(m), esp_err_to_name(r));
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
        esp_err_t r = enter(a, m, cfg->aux_pwm[a].freq_hz, cfg->aux_pwm[a].duty_pm);
        if (r != ESP_OK && first == ESP_OK) first = r;
    }
    xSemaphoreGive(s_mux);
    return first;
}

esp_err_t aux_io_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;
    return aux_io_apply();
}

aux_mode_t aux_io_mode(uint8_t a)
{
    return (a < AUX_PAD_COUNT) ? s_mode[a] : AUX_MODE_STANDARD;
}

bool aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint8_t *bits)
{
    if (a >= AUX_PAD_COUNT || s_mode[a] != AUX_MODE_PWM) return false;
    *freq_hz = ledc_get_freq(PWM_MODE, PWM_TIMER(a));
    *bits    = s_bits[a];
    return true;
}

#endif /* CONFIG_APP_AUX_DI_ENABLE */
