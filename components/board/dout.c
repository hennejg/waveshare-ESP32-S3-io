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
static void apply_json_item(cJSON *item, uint8_t n)
{
    if (cJSON_IsBool(item)) {
        s_state[n] = cJSON_IsTrue(item);
    } else if (cJSON_IsNumber(item)) {
        s_state[n] = (item->valuedouble != 0.0);
    } else if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        size_t slen   = strlen(s);
        bool state;
        if (parse_toggle(s, slen))          s_state[n] = !s_state[n];
        else if (parse_payload(s, slen, &state)) s_state[n] = state;
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

esp_err_t dout_set(uint8_t n, bool state)
{
    if (n >= NUM_DO) return ESP_ERR_INVALID_ARG;
    bool changed = (s_state[n] != state);
    s_state[n] = state;
    esp_err_t ret = write_outputs();
    /* Only publish when the state changed — avoids an infinite echo loop
       caused by receiving our own confirmations back from the broker. */
#ifdef CONFIG_APP_MQTT_ENABLE
    if (ret == ESP_OK && changed && app_mqtt_is_connected()) publish_one(n);
#endif
    return ret;
}

void dout_publish_all(void)
{
    write_outputs();    /* re-apply to hardware — picks up invert changes */
#ifdef CONFIG_APP_MQTT_ENABLE
    for (uint8_t i = 0; i < NUM_DO; i++) publish_one(i);
#endif
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
    /* Match suffix "output/set" — bulk set all outputs at once.
       Single value: apply same state/toggle to every output.
       JSON array:   apply each element to the corresponding output (1-indexed). */
    static const char BULK_SUFFIX[] = "output/set";
    const size_t bs = sizeof(BULK_SUFFIX) - 1;
    if (tlen >= bs && memcmp(topic + tlen - bs, BULK_SUFFIX, bs) == 0) {
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
            for (int i = 0; i < n; i++)
                apply_json_item(cJSON_GetArrayItem(arr, i), (uint8_t)i);
            cJSON_Delete(arr);
        } else {
            bool state;
            if (parse_toggle(data, dlen)) {
                for (uint8_t i = 0; i < NUM_DO; i++) s_state[i] = !s_state[i];
            } else if (parse_payload(data, dlen, &state)) {
                for (uint8_t i = 0; i < NUM_DO; i++) s_state[i] = state;
            } else {
                ESP_LOGW(TAG, "output/set: unrecognised payload");
                return;
            }
        }
        dout_publish_all();
        return;
    }

    /* Match suffix "output/read" */
    static const char READ_SUFFIX[] = "output/read";
    const size_t rs = sizeof(READ_SUFFIX) - 1;
    if (tlen >= rs && memcmp(topic + tlen - rs, READ_SUFFIX, rs) == 0) {
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
            if (tlen >= clen && memcmp(topic + tlen - clen, cmd, clen) == 0) {
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
