#include "di.h"
#include "app_config.h"
#include "aux_io.h"
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

#define TAG          "di"
#define NUM_DI       APP_CFG_DI_COUNT
#define DEBOUNCE_MS  10

static const gpio_num_t s_gpios[NUM_DI] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6,  GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
#if CONFIG_APP_AUX_DI_ENABLE
    /* Auxiliary inputs — direct 3.3 V GPIOs, no optocoupler. */
    AUX_PAD_GPIOS,
#endif
};

static bool         s_state[NUM_DI];
static TaskHandle_t s_task;
/* Which auxiliary pins are inputs, bit i = channel 8+i.  Written under s_mux
   by di_aux_input(), read by the input task under the same lock.

   Nothing is published under s_mux: publishing waits on the MQTT client's
   lock, and the MQTT task calls into the drivers while holding it. Changes
   are collected under the lock and reported after it. s_report: auxiliary
   pins that have just become inputs and whose level is still to be reported;
   s_gone: ones that have stopped being inputs, which the rule engine is told
   read false. */
static uint8_t           s_aux;
static uint8_t           s_report;
static uint8_t           s_gone;
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

/* Who hears of a change, and in which order.

   The rule engine hears of it under s_mux, in the same step that read the
   pin. Told after the lock, a task could read HIGH, block in the MQTT publish
   before it, and meanwhile di_publish_all() read LOW and delivered it: the
   engine got LOW, then the stale HIGH, kept HIGH with the pin LOW -- and a
   heldFor() on it switched an output. Under the lock the engine gets the
   levels in the order they were read. scripting_on_input_change() only
   numbers and queues the event (its lock is never held while taking s_mux),
   it does not wait on the network.

   MQTT and Matter cannot be told under the lock (see s_mux). Their messages
   may still go out of order, so each sender looks again after sending: if the
   level has moved on from the one it sent, it sends the current one. The last
   message out is then always followed by a look that found it current. A pin
   that keeps changing is sent by di_task anyway, so the look-again is
   bounded. */
static void deliver_rules(uint8_t n, bool state)
{
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_input_change(n, state);
#else
    (void)n; (void)state;
#endif
}

static void notify_ext(uint8_t n, bool state)
{
    for (int tries = 0; tries < 4; tries++) {
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
        xSemaphoreTake(s_mux, portMAX_DELAY);
        bool active = di_is_active(n), now = s_state[n];
        xSemaphoreGive(s_mux);
        if (!active || now == state) return;
        state = now;
    }
}

void di_publish_all(void)
{
    /* Refreshed from the pins under the lock, so that a pad changing hands
       cannot be read as an input after it stopped being one; the rule engine
       told under it, MQTT and Matter after it. */
    bool active[NUM_DI], val[NUM_DI];
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (uint8_t i = 0; i < NUM_DI; i++) {
        active[i] = di_is_active(i);
        if (active[i]) s_state[i] = read_pin(i);
        val[i] = s_state[i];
        if (active[i]) deliver_rules(i, val[i]);
    }
    xSemaphoreGive(s_mux);
    for (uint8_t i = 0; i < NUM_DI; i++)
        if (active[i]) notify_ext(i, val[i]);
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

/* One pass over the pins after an edge. */
static void di_scan(void)
{
    bool    send[NUM_DI] = {0}, val[NUM_DI];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t report = s_report, gone = s_gone;
    s_report = s_gone = 0;
    for (uint8_t i = 0; i < NUM_DI; i++) {
        uint8_t a   = (uint8_t)(i - APP_CFG_DI_ISOLATED_COUNT);
        bool    aux = i >= APP_CFG_DI_ISOLATED_COUNT;
        val[i] = s_state[i];
        /* A pad that is no longer an input reads false; the rule engine
           holds the last level it was given and has to be told. */
        if (aux && (gone >> a) & 1u) deliver_rules(i, false);
        if (!di_is_active(i)) continue;
        bool cur = read_pin(i);
        if (cur != s_state[i]) { s_state[i] = val[i] = cur; send[i] = true; }
        if (aux && (report >> a) & 1u) send[i] = true;
        if (send[i]) deliver_rules(i, val[i]);
    }
    xSemaphoreGive(s_mux);

    for (uint8_t i = 0; i < NUM_DI; i++)
        if (send[i]) notify_ext(i, val[i]);
}

static void di_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        di_scan();
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

/* The input path of an auxiliary pin, nothing else: aux_io.c decides what
   the pad is for and has already taken it off UART0 when on is true. */
void di_aux_input(uint8_t a, bool on)
{
#if CONFIG_APP_AUX_DI_ENABLE
    if (a >= APP_CFG_DI_AUX_COUNT || !s_mux) return;
    uint8_t    n = (uint8_t)(APP_CFG_DI_ISOLATED_COUNT + a);
    gpio_num_t g = s_gpios[n];

    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool was = (s_aux >> a) & 1u;
    if (on && !was) {
        input_config(g);
        s_aux |= (uint8_t)(1u << a);
        s_state[n] = read_pin(n);
        if (s_isr_ready) {
            gpio_isr_handler_add(g, gpio_isr, NULL);
            gpio_set_intr_type(g, GPIO_INTR_ANYEDGE);
        }
        s_report |= (uint8_t)(1u << a);       /* reported by di_task */
        s_gone   &= (uint8_t)~(1u << a);
    } else if (!on && was) {
        /* Off the input path first, so nothing reads the pin while it
           changes hands. */
        s_aux &= (uint8_t)~(1u << a);
        if (s_isr_ready) {
            gpio_set_intr_type(g, GPIO_INTR_DISABLE);
            gpio_isr_handler_remove(g);
        }
        s_state[n] = false;
        s_gone   |= (uint8_t)(1u << a);
        s_report &= (uint8_t)~(1u << a);
    }
    xSemaphoreGive(s_mux);
    if (on != was && s_task) xTaskNotifyGive(s_task);
#else
    (void)a; (void)on;
#endif
}

/* ------------------------------------------------------------------ public */

esp_err_t di_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;

    /* Configure GPIOs with interrupts initially disabled to avoid a race
       between gpio_config and gpio_isr_handler_add. Only the isolated inputs
       here: aux_io_init() hands over the auxiliary pins set to input. */
    for (uint8_t i = 0; i < APP_CFG_DI_ISOLATED_COUNT; i++) {
        input_config(s_gpios[i]);
        s_state[i] = read_pin(i);
    }
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
