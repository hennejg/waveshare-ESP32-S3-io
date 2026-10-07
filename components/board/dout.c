#include "dout.h"
#include "app_config.h"
#ifdef CONFIG_APP_MQTT_ENABLE
#include <stdbool.h>
int  app_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain);
int  app_mqtt_subscribe(const char *topic, int qos);
bool app_mqtt_is_connected(void);
#include "cJSON.h"
#endif

#include <string.h>
#include <ctype.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"

#define TAG          "dout"
#define NUM_DO       8
#define TCA9554_ADDR 0x20

/* TCA9554 register map */
#define REG_OUTPUT   0x01
#define REG_CONFIG   0x03

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool                    s_state[NUM_DO];  /* logical state */

/* Guards s_state[] together with the TCA9554 write it feeds. Setting one
   output is a read-modify-write of the whole port byte, and it is driven from
   the CAN task, the Modbus event task, the rule engine, the HTTP server and
   the MQTT task. Without this, two concurrent changes let the loser write its
   stale byte last: the reported state and the relay disagree until some
   unrelated write happens to repair it.
   Recursive, because dout_publish_all() is reached both directly and from the
   bulk MQTT path, which already holds the lock. */
static SemaphoreHandle_t       s_lock;

static inline void dout_lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static inline void dout_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

/* ------------------------------------------------------------------ helpers */

#ifdef CONFIG_APP_MQTT_ENABLE
/* Accepted on-payloads: true 1 high on   (case-insensitive)
   Accepted off-payloads: false 0 low off */
static bool parse_payload(const char *data, size_t len, bool *out)
{
    char buf[8];
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, data, len);
    buf[len] = '\0';
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)buf[i]);

    if (!strcmp(buf, "true") || !strcmp(buf, "1") ||
        !strcmp(buf, "high") || !strcmp(buf, "on"))  { *out = true;  return true; }
    if (!strcmp(buf, "false") || !strcmp(buf, "0") ||
        !strcmp(buf, "low")   || !strcmp(buf, "off")) { *out = false; return true; }
    return false;
}

static bool parse_toggle(const char *data, size_t len)
{
    char buf[8];
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, data, len);
    buf[len] = '\0';
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    return !strcmp(buf, "toggle");
}

/* Apply a single cJSON item (bool / number / string) to output n.
   Strings use the same vocabulary as individual output commands. */
/* Collect what one array element asks for into the masks, rather than editing
   the state here: the whole array is then applied as one operation. */
static void apply_json_item(cJSON *item, uint8_t n,
                            uint8_t *set, uint8_t *clear, uint8_t *toggle)
{
    const uint8_t bit = (uint8_t)(1u << n);

    if (cJSON_IsBool(item)) {
        if (cJSON_IsTrue(item)) *set |= bit; else *clear |= bit;
    } else if (cJSON_IsNumber(item)) {
        if (item->valuedouble != 0.0) *set |= bit; else *clear |= bit;
    } else if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        size_t slen   = strlen(s);
        bool state;
        if (parse_toggle(s, slen))               *toggle |= bit;
        else if (parse_payload(s, slen, &state)) { if (state) *set |= bit; else *clear |= bit; }
    }
}
#endif /* CONFIG_APP_MQTT_ENABLE */

/* i2c_master_transmit wrapper: on ESP_ERR_INVALID_STATE (bus stuck) clocks
 * 9 SCL pulses to release SDA and retries once.  Handles both a mid-reset
 * stuck bus and the TCA9554 power-on edge case on a never-flashed device. */
static esp_err_t i2c_transmit_safe(const uint8_t *buf, size_t len)
{
    esp_err_t ret = i2c_master_transmit(s_dev, buf, len, 10);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C bus stuck — recovering");
        i2c_master_bus_reset(s_bus);
        ret = i2c_master_transmit(s_dev, buf, len, 10);
    }
    return ret;
}

/* Build the physical output byte and write it to the TCA9554. */
static esp_err_t write_outputs(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    const app_config_t *cfg = app_config_get();
    uint8_t byte = 0;
    for (uint8_t i = 0; i < NUM_DO; i++) {
        bool physical = s_state[i] ^ cfg->dout[i].invert;
        if (physical) byte |= (1u << i);
    }
    uint8_t buf[2] = {REG_OUTPUT, byte};
    return i2c_transmit_safe(buf, sizeof(buf));
}

#ifdef CONFIG_APP_MQTT_ENABLE
static void publish_one(uint8_t n)
{
    const char *name = app_config_get()->dout[n].name;
    char topic[32];
    if (name[0]) snprintf(topic, sizeof(topic), "output/%.20s",   name);
    else         snprintf(topic, sizeof(topic), "output/%u",    n + 1);
    app_mqtt_publish(topic, s_state[n] ? "true" : "false", -1, 0, false);
}
#endif

/* ------------------------------------------------------------------ public */

esp_err_t dout_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "output mutex");
    }
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "I2C bus init");
    s_bus = i2c_bus_handle();   /* shared with the PCF85063 RTC */

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA9554_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                        TAG, "TCA9554 device add");

    /* Configure all 8 pins as outputs, then drive them all off.
     * i2c_transmit_safe recovers from a stuck bus on either write. */
    uint8_t cfg_cmd[2] = {REG_CONFIG, 0x00};
    ESP_RETURN_ON_ERROR(i2c_transmit_safe(cfg_cmd, sizeof(cfg_cmd)), TAG, "TCA9554 config");
    ESP_RETURN_ON_ERROR(write_outputs(), TAG, "initial write");

    ESP_LOGI(TAG, "Initialized %d outputs via TCA9554 (I2C addr 0x%02X)", NUM_DO, TCA9554_ADDR);
    return ESP_OK;
}

bool dout_get(uint8_t n)
{
    return (n < NUM_DO) ? s_state[n] : false;
}

uint8_t dout_get_all(void)
{
    uint8_t v = 0;
    dout_lock();
    for (uint8_t i = 0; i < NUM_DO; i++) if (s_state[i]) v |= (uint8_t)(1u << i);
    dout_unlock();
    return v;
}

esp_err_t dout_modify(uint8_t set, uint8_t clear, uint8_t toggle)
{
    uint8_t before = 0, after = 0;

    dout_lock();
    for (uint8_t i = 0; i < NUM_DO; i++) if (s_state[i]) before |= (uint8_t)(1u << i);

    after = (uint8_t)(((before & (uint8_t)~clear) | set) ^ toggle);
    for (uint8_t i = 0; i < NUM_DO; i++) s_state[i] = (after >> i) & 1u;

    esp_err_t ret = write_outputs();
    /* The port byte never reached the chip, so nothing moved. Keeping the
       requested values would make dout_get() report a state the hardware is not
       in -- and a repeat of the same command would then be skipped by callers
       that compare against dout_get() first. */
    if (ret != ESP_OK) {
        for (uint8_t i = 0; i < NUM_DO; i++) s_state[i] = (before >> i) & 1u;
        after = before;
        ESP_LOGW(TAG, "outputs: write failed (%s), nothing changed",
                 esp_err_to_name(ret));
    }
    dout_unlock();

    /* Only publish channels that actually moved — avoids an echo loop from our
       own confirmations coming back off the broker. */
#ifdef CONFIG_APP_MQTT_ENABLE
    uint8_t changed = (uint8_t)(before ^ after);
    if (changed && app_mqtt_is_connected())
        for (uint8_t i = 0; i < NUM_DO; i++)
            if (changed & (1u << i)) publish_one(i);
#endif
    return ret;
}

esp_err_t dout_set(uint8_t n, bool state)
{
    if (n >= NUM_DO) return ESP_ERR_INVALID_ARG;
    uint8_t bit = (uint8_t)(1u << n);
    return dout_modify(state ? bit : 0u, state ? 0u : bit, 0u);
}

esp_err_t dout_publish_all(void)
{
    dout_lock();
    esp_err_t ret = write_outputs();   /* re-apply — picks up invert changes */
    dout_unlock();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "outputs: re-apply failed (%s)", esp_err_to_name(ret));
#ifdef CONFIG_APP_MQTT_ENABLE
    for (uint8_t i = 0; i < NUM_DO; i++) publish_one(i);
#endif
    return ret;
}

void dout_on_mqtt_connected(void)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    const app_config_t *cfg = app_config_get();
    for (uint8_t i = 0; i < NUM_DO; i++) {
        char topic[36];
        const char *name = cfg->dout[i].name;
        if (name[0]) snprintf(topic, sizeof(topic), "output/%.20s/set",   name);
        else         snprintf(topic, sizeof(topic), "output/%u/set",    i + 1);
        app_mqtt_subscribe(topic, 0);
    }
    app_mqtt_subscribe("output/read", 0);
    app_mqtt_subscribe("output/set", 0);
    dout_publish_all();
#endif
}

void dout_on_mqtt_message(const char *topic, size_t tlen,
                           const char *data,  size_t dlen)
{
#ifdef CONFIG_APP_MQTT_ENABLE
    /* The topic arrives relative, without the prefix (app_mqtt strips it),
       and is compared whole: a suffix match used to let "rules/output/set"
       -- delivered through the rule engine's "rules/#" subscription -- pass
       for the bulk command and switch every output.

       "output/set" — bulk set all outputs at once.
       Single value: apply same state/toggle to every output.
       JSON array:   apply each element to the corresponding output (1-indexed). */
    static const char BULK_TOPIC[] = "output/set";
    const size_t bs = sizeof(BULK_TOPIC) - 1;
    if (tlen == bs && memcmp(topic, BULK_TOPIC, bs) == 0) {
        char buf[256];
        if (dlen == 0 || dlen >= sizeof(buf)) return;
        memcpy(buf, data, dlen);
        buf[dlen] = '\0';

        if (buf[0] == '[') {
            cJSON *arr = cJSON_Parse(buf);
            if (!cJSON_IsArray(arr)) {
                ESP_LOGW(TAG, "output/set: invalid JSON array");
                cJSON_Delete(arr);
                return;
            }
            int n = cJSON_GetArraySize(arr);
            if (n > NUM_DO) n = NUM_DO;
            uint8_t set = 0, clear = 0, toggle = 0;
            for (int i = 0; i < n; i++)
                apply_json_item(cJSON_GetArrayItem(arr, i), (uint8_t)i,
                                &set, &clear, &toggle);
            cJSON_Delete(arr);
            dout_modify(set, clear, toggle);
        } else {
            bool state;
            if (parse_toggle(data, dlen)) {
                dout_modify(0u, 0u, 0xFFu);
            } else if (parse_payload(data, dlen, &state)) {
                dout_modify(state ? 0xFFu : 0u, state ? 0u : 0xFFu, 0u);
            } else {
                ESP_LOGW(TAG, "output/set: unrecognised payload");
                return;
            }
        }
        return;   /* dout_modify() publishes the channels that moved */
    }

    static const char READ_TOPIC[] = "output/read";
    const size_t rs = sizeof(READ_TOPIC) - 1;
    if (tlen == rs && memcmp(topic, READ_TOPIC, rs) == 0) {
        dout_publish_all();
        return;
    }

    /* Match individual output command topics (name or number). */
    {
        const app_config_t *cfg = app_config_get();
        for (uint8_t i = 0; i < NUM_DO; i++) {
            char cmd[36];
            const char *name = cfg->dout[i].name;
            if (name[0]) snprintf(cmd, sizeof(cmd), "output/%.20s/set",   name);
            else         snprintf(cmd, sizeof(cmd), "output/%u/set",    i + 1);
            size_t clen = strlen(cmd);
            if (tlen == clen && memcmp(topic, cmd, clen) == 0) {
                bool state;
                if (parse_toggle(data, dlen))              dout_set(i, !dout_get(i));
                else if (parse_payload(data, dlen, &state)) dout_set(i, state);
                else ESP_LOGW(TAG, "Unrecognised payload for %s", cmd);
                return;
            }
        }
    }
#else
    (void)topic; (void)tlen; (void)data; (void)dlen;
#endif
}
