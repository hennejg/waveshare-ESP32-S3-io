#include "aux_io.h"
#include "app_config.h"
#include "di.h"

#include <limits.h>
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
   scripting component. pad is 1..3. */
void scripting_on_analog(uint8_t pad, int mv);
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
bool aux_io_pwm_info(uint8_t a, uint32_t *f, uint16_t *d, uint8_t *b) { (void)a; (void)f; (void)d; (void)b; return false; }
esp_err_t aux_io_pwm_set(uint8_t a, uint32_t f, uint16_t d) { (void)a; (void)f; (void)d; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t aux_io_analog_mv(uint8_t a, int *mv) { (void)a; (void)mv; return ESP_ERR_NOT_SUPPORTED; }
void aux_io_on_mqtt_connected(void) {}
void aux_io_on_mqtt_message(const char *t, size_t tl, const char *d, size_t dl) { (void)t; (void)tl; (void)d; (void)dl; }

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

/* The configuration's PWM values as last applied. A save that does not touch
   them must not overwrite a duty cycle set at run time. */
static uint32_t          s_cfg_freq[AUX_PAD_COUNT];
static uint16_t          s_cfg_duty[AUX_PAD_COUNT];

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

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
        uint32_t f = cfg->aux_pwm[a].freq_hz;
        uint16_t d = cfg->aux_pwm[a].duty_pm;
        bool cfg_changed = f != s_cfg_freq[a] || d != s_cfg_duty[a];
        s_cfg_freq[a] = f;
        s_cfg_duty[a] = d;

        esp_err_t r = ESP_OK;
        if (m == s_mode[a]) {
            /* Same mode: only new configured PWM values are applied. A save of
               something else leaves a run-time duty cycle where it is. */
            if (m == AUX_MODE_PWM && cfg_changed) {
                r = pwm_set(a, f, d, false);
                mark_dirty(a);
            }
        } else {
            r = enter(a, m, f, d);
            mark_dirty(a);
        }
        if (r != ESP_OK && first == ESP_OK) first = r;
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
        r = (f == s_freq[a] && d == s_duty[a]) ? ESP_OK : pwm_set(a, f, d, false);
        if (r == ESP_OK) mark_dirty(a);
    }
    xSemaphoreGive(s_mux);
    return r;
}

/* ------------------------------------------------------------- pad task */

#define SAMPLE_MS 200

static int s_an_reported = INT_MIN;   /* last analog value passed on, mV */

#ifdef CONFIG_APP_MQTT_ENABLE
static void publish_pad(uint8_t a)
{
    if (!app_mqtt_is_connected()) return;
    char topic[24], payload[48];
    snprintf(topic, sizeof(topic), "aux/%u/mode", a + 1);
    app_mqtt_publish(topic, aux_mode_name(s_mode[a]), -1, 0, true);
    uint32_t f; uint16_t d; uint8_t b;
    if (aux_io_pwm_info(a, &f, &d, &b)) {
        snprintf(topic, sizeof(topic), "aux/%u/pwm", a + 1);
        snprintf(payload, sizeof(payload), "{\"duty\":%u.%u,\"freq\":%lu}",
                 d / 10, d % 10, (unsigned long)f);
        app_mqtt_publish(topic, payload, -1, 0, true);
    }
}
#endif

static void report_analog(int mv)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    if (app_mqtt_is_connected()) {
        char payload[16];
        snprintf(payload, sizeof(payload), "%d", mv);
        app_mqtt_publish("aux/1/analog", payload, -1, 0, false);
    }
#endif
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_analog(1, mv);
#endif
    (void)mv;
}

static void aux_task(void *arg)
{
    (void)arg;
    TickType_t next_sample = xTaskGetTickCount();
    for (;;) {
        /* Woken early to publish, the analog input is still sampled on its own
           clock: a rule that sets a PWM output from the reading wakes this
           task, and sampling again on that wake fed the rule a new reading,
           which set the PWM again -- 30 readings a second instead of 5. */
        TickType_t now  = xTaskGetTickCount();
        TickType_t wait = (TickType_t)(next_sample - now);
        if ((int32_t)wait < 0) wait = 0;
        ulTaskNotifyTake(pdTRUE, wait);

        uint8_t dirty = __atomic_exchange_n(&s_dirty, 0, __ATOMIC_RELAXED);
        for (uint8_t a = 0; a < AUX_PAD_COUNT; a++) {
            if (!(dirty & (1u << a))) continue;
#ifdef CONFIG_APP_MQTT_ENABLE
            publish_pad(a);
#endif
            /* A pad that has just become an analog input reports at once. */
            if (a == 0) s_an_reported = INT_MIN;
        }

        now = xTaskGetTickCount();
        if ((int32_t)(now - next_sample) < 0) continue;      /* not time to sample yet */
        next_sample = now + pdMS_TO_TICKS(SAMPLE_MS);

        int mv;
        if (aux_io_analog_mv(0, &mv) == ESP_OK) {
            if (s_an_reported == INT_MIN || abs(mv - s_an_reported) >= AUX_ANALOG_STEP_MV) {
                s_an_reported = mv;
                report_analog(mv);
            }
        } else {
            s_an_reported = INT_MIN;
        }
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

    double duty = -1, freq = 0;
    cJSON *j = cJSON_Parse(buf);
    if (cJSON_IsNumber(j)) {
        duty = j->valuedouble;
    } else if (cJSON_IsObject(j)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(j, "duty")) && cJSON_IsNumber(v)) duty = v->valuedouble;
        else duty = -2;                                         /* keep */
        if ((v = cJSON_GetObjectItem(j, "freq")) && cJSON_IsNumber(v)) freq = v->valuedouble;
    } else {
        cJSON_Delete(j);
        ESP_LOGW(TAG, "aux/%u/pwm/set: expected a number or {\"duty\":..,\"freq\":..}", a + 1);
        return;
    }
    cJSON_Delete(j);

    uint16_t d;
    if (duty == -2) d = AUX_PWM_DUTY_KEEP;
    else if (duty >= 0.0 && duty <= 100.0) d = (uint16_t)(duty * 10.0 + 0.5);
    else { ESP_LOGW(TAG, "aux/%u/pwm/set: duty must be 0..100", a + 1); return; }
    if (freq != 0 && (freq != (double)(uint32_t)freq || freq < AUX_PWM_FREQ_MIN || freq > AUX_PWM_FREQ_MAX)) {
        ESP_LOGW(TAG, "aux/%u/pwm/set: freq must be a whole number of Hz from %u to %u",
                 a + 1, AUX_PWM_FREQ_MIN, AUX_PWM_FREQ_MAX);
        return;
    }
    esp_err_t r = aux_io_pwm_set(a, (uint32_t)freq, d);
    if (r != ESP_OK)
        ESP_LOGW(TAG, "aux/%u/pwm/set: %s", a + 1,
                 r == ESP_ERR_INVALID_STATE ? "pad is not a PWM output" : esp_err_to_name(r));
#else
    (void)topic; (void)tlen; (void)data; (void)dlen;
#endif
}

aux_mode_t aux_io_mode(uint8_t a)
{
    return (a < AUX_PAD_COUNT) ? s_mode[a] : AUX_MODE_STANDARD;
}

bool aux_io_pwm_info(uint8_t a, uint32_t *freq_hz, uint16_t *duty_pm, uint8_t *bits)
{
    if (a >= AUX_PAD_COUNT || s_mode[a] != AUX_MODE_PWM) return false;
    *freq_hz = ledc_get_freq(PWM_MODE, PWM_TIMER(a));
    *duty_pm = s_duty[a];
    *bits    = s_bits[a];
    return true;
}

#endif /* CONFIG_APP_AUX_DI_ENABLE */
