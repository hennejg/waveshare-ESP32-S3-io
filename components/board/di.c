#include "di.h"
#include "app_config.h"
#ifdef CONFIG_APP_MQTT_ENABLE
#include <stdbool.h>
int  app_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain);
int  app_mqtt_subscribe(const char *topic, int qos);
bool app_mqtt_is_connected(void);
#endif
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif
#ifdef CONFIG_APP_SCRIPTING_ENABLE
/* Declared rather than included: components/board does not depend on the
   scripting component, and apps/matter excludes it from the build. */
void scripting_on_input_change(uint8_t channel, bool state);
#endif

#include <string.h>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#if CONFIG_APP_AUX_DI_ENABLE
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_private/esp_gpio_reserve.h>
#include <soc/gpio_sig_map.h>
#endif

#define TAG          "di"
#define NUM_DI       APP_CFG_DI_COUNT
#define DEBOUNCE_MS  10

static const gpio_num_t s_gpios[NUM_DI] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6,  GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
#if CONFIG_APP_AUX_DI_ENABLE
    /* Auxiliary inputs — direct 3.3 V GPIOs, no optocoupler. */
    GPIO_NUM_1, GPIO_NUM_44, GPIO_NUM_43,
#endif
};

static bool         s_state[NUM_DI];
static TaskHandle_t s_task;
/* Which auxiliary pins are inputs, bit i = channel 8+i.  Written under s_mux
   by di_set_aux() (HTTP task), read by the input task under the same lock. */
static uint8_t           s_aux;
static SemaphoreHandle_t s_mux;
static bool              s_isr_ready;

/* ------------------------------------------------------------------ helpers */

static inline bool read_pin(uint8_t n)
{
    /* Default sense: GPIO high → true.  Flip with di[n].invert = true. */
    bool level = (bool)gpio_get_level(s_gpios[n]);
    return level ^ app_config_get()->di[n].invert;
}

#ifdef CONFIG_APP_MQTT_ENABLE
static void publish_one(uint8_t n, bool state)
{
    const char *name = app_config_get()->di[n].name;
    char topic[32];
    if (name[0]) snprintf(topic, sizeof(topic), "input/%.20s",   name);
    else         snprintf(topic, sizeof(topic), "input/%u",    n + 1);
    app_mqtt_publish(topic, state ? "true" : "false", -1, 0, false);
}
#endif

static void notify_one(uint8_t n, bool state)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    if (app_mqtt_is_connected())
        publish_one(n, state);
#endif
#ifdef CONFIG_APP_MATTER_ENABLE
    /* The Matter app exposes exactly 8 contact sensors; the auxiliary inputs
       have no endpoint and are reported over MQTT and the web UI only. */
    if (di_is_isolated(n))
        matter_di_update(n, state);
#endif
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_input_change(n, state);
#endif
}

void di_publish_all(void)
{
    for (uint8_t i = 0; i < NUM_DI; i++) {
        if (!di_is_active(i)) continue;
        s_state[i] = read_pin(i);   /* refresh from hardware before publishing */
        notify_one(i, s_state[i]);
    }
}

bool di_is_active(uint8_t n)
{
    if (n < APP_CFG_DI_ISOLATED_COUNT) return true;
    if (n >= NUM_DI) return false;
    return (s_aux >> (n - APP_CFG_DI_ISOLATED_COUNT)) & 1u;
}

bool di_get(uint8_t n)
{
    if (n >= NUM_DI || !di_is_active(n)) return false;
    return s_state[n];
}

bool di_is_isolated(uint8_t n)
{
    return n < APP_CFG_DI_ISOLATED_COUNT;
}

/* ----------------------------------------------------------------- ISR / task */

static void IRAM_ATTR gpio_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void di_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        xSemaphoreTake(s_mux, portMAX_DELAY);
        for (uint8_t i = 0; i < NUM_DI; i++) {
            if (!di_is_active(i)) continue;
            bool cur = read_pin(i);
            if (cur != s_state[i]) {
                s_state[i] = cur;
                notify_one(i, cur);
            }
        }
        xSemaphoreGive(s_mux);
    }
}

/* ------------------------------------------------------------ auxiliary pins */

static void input_config(gpio_num_t g)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << g,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

#if CONFIG_APP_AUX_DI_ENABLE
/* Takes a pin from its standard function. gpio_config() selects the plain
   GPIO function on the pad, which takes U0TXD off GPIO43; U0RXD is fed a
   constant high instead of GPIO44, so the UART sees an idle line rather than
   whatever is wired to the input. The console reserved both pins at boot. */
static void aux_take(gpio_num_t g)
{
    if (g == GPIO_NUM_43 || g == GPIO_NUM_44) esp_gpio_revoke(BIT64(g));
    if (g == GPIO_NUM_44)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);
    input_config(g);
}

/* Gives a pin back. GPIO1 has no function in this firmware and is left an
   input with its pull-up, as gpio_reset_pin() leaves every pin; GPIO44/43 go
   back to UART0 through the IO MUX, which needs no UART driver installed. */
static void aux_release(gpio_num_t g)
{
    if (g == GPIO_NUM_43)
        uart_set_pin(UART_NUM_0, GPIO_NUM_43, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    else if (g == GPIO_NUM_44)
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, GPIO_NUM_44,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    else
        gpio_reset_pin(g);
}
#endif

void di_set_aux(uint8_t mask)
{
#if CONFIG_APP_AUX_DI_ENABLE
    mask &= (uint8_t)((1u << APP_CFG_DI_AUX_COUNT) - 1u);
    if (!s_mux) { s_aux = mask; return; }        /* di_init() applies it */

    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t changed = (uint8_t)(s_aux ^ mask);
    for (uint8_t a = 0; a < APP_CFG_DI_AUX_COUNT; a++) {
        if (!(changed & (1u << a))) continue;
        uint8_t    n = (uint8_t)(APP_CFG_DI_ISOLATED_COUNT + a);
        gpio_num_t g = s_gpios[n];
        if (mask & (1u << a)) {
            aux_take(g);
            s_aux |= (uint8_t)(1u << a);
            s_state[n] = read_pin(n);
            if (s_isr_ready) {
                gpio_isr_handler_add(g, gpio_isr, NULL);
                gpio_set_intr_type(g, GPIO_INTR_ANYEDGE);
            }
            ESP_LOGW(TAG, "AUX%u (GPIO%d) is now a NON-ISOLATED input", a + 1, g);
            notify_one(n, s_state[n]);
        } else {
            /* Off the input path first, so nothing reads the pin while it
               changes hands. */
            s_aux &= (uint8_t)~(1u << a);
            if (s_isr_ready) {
                gpio_set_intr_type(g, GPIO_INTR_DISABLE);
                gpio_isr_handler_remove(g);
            }
            aux_release(g);
            s_state[n] = false;
            ESP_LOGI(TAG, "AUX%u (GPIO%d) back to its standard function", a + 1, g);
        }
    }
    xSemaphoreGive(s_mux);
#else
    (void)mask;
#endif
}

/* ------------------------------------------------------------------ public */

esp_err_t di_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;

    /* Configure GPIOs with interrupts initially disabled to avoid a race
       between gpio_config and gpio_isr_handler_add. Only the isolated inputs
       here: the auxiliary pins are taken below, and only those selected. */
    for (uint8_t i = 0; i < APP_CFG_DI_ISOLATED_COUNT; i++) {
        input_config(s_gpios[i]);
        s_state[i] = read_pin(i);
    }
#if CONFIG_APP_AUX_DI_ENABLE
    uint8_t want = (uint8_t)(app_config_get()->aux_in & ((1u << APP_CFG_DI_AUX_COUNT) - 1u));
    for (uint8_t a = 0; a < APP_CFG_DI_AUX_COUNT; a++) {
        if (!(want & (1u << a))) continue;
        uint8_t n = (uint8_t)(APP_CFG_DI_ISOLATED_COUNT + a);
        aux_take(s_gpios[n]);
        s_state[n] = read_pin(n);
    }
    s_aux = want;
#endif

    xTaskCreate(di_task, "di", 4096, NULL, 5, &s_task);

    /* Install ISR service (ignore ESP_ERR_INVALID_STATE if already done). */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    /* Register handlers then enable edge interrupts. */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (uint8_t i = 0; i < NUM_DI; i++) {
        if (!di_is_active(i)) continue;
        gpio_isr_handler_add(s_gpios[i], gpio_isr, NULL);
        gpio_set_intr_type(s_gpios[i], GPIO_INTR_ANYEDGE);
    }
    s_isr_ready = true;
    xSemaphoreGive(s_mux);

    ESP_LOGI(TAG, "Initialized %d inputs (GPIO%d..GPIO%d)",
             APP_CFG_DI_ISOLATED_COUNT, s_gpios[0],
             s_gpios[APP_CFG_DI_ISOLATED_COUNT - 1]);
#if CONFIG_APP_AUX_DI_ENABLE
    for (uint8_t a = 0; a < APP_CFG_DI_AUX_COUNT; a++)
        if (s_aux & (1u << a))
            ESP_LOGW(TAG, "AUX%u (GPIO%d) is a NON-ISOLATED input", a + 1,
                     s_gpios[APP_CFG_DI_ISOLATED_COUNT + a]);
#endif
    return ESP_OK;
}

void di_on_mqtt_connected(void)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    app_mqtt_subscribe("input/read", 0);
    di_publish_all();
#endif
}

void di_on_mqtt_message(const char *topic, size_t tlen,
                         const char *data,  size_t dlen)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    /* The topic arrives relative, without the prefix, and is compared whole. */
    static const char TOPIC[] = "input/read";
    const size_t slen = sizeof(TOPIC) - 1;
    if (tlen == slen && memcmp(topic, TOPIC, slen) == 0) {
        di_publish_all();
    }
#else
    (void)topic; (void)tlen; (void)data; (void)dlen;
#endif
}
