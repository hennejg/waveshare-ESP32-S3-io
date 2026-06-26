#include "app_config.h"
#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>

_Static_assert(sizeof(app_config_t) < 3000,
               "app_config_t too large for NVS single-key storage");

#define NVS_NS "app_config"

/* NVS key names must be <= 15 chars. */
#define K_DEVICE_NAME   "device_name"
#define K_MQTT_URL      "mqtt_url"
#define K_MQTT_USER     "mqtt_user"
#define K_MQTT_PASS     "mqtt_password"
#define K_MQTT_TOPIC    "mqtt_topic"
#define K_DI_CFG        "di_cfg"
#define K_DOUT_CFG      "dout_cfg"
#define K_MODBUS_CFG    "mb_cfg"
#define K_LED_MODE      "led_mode"
#define K_CAN_CFG       "can_cfg"
#define K_SNTP_CFG      "sntp_cfg"
#define K_TZ            "tz"

static app_config_t s_cfg = {
    .device_name       = "Waveshare-ESP32",
    .mqtt_url          = "",
    .mqtt_user         = "",
    .mqtt_password     = "",
    .mqtt_topic_prefix = "",
    /* di/dout names and invert default to zero */
    .led_mode = LED_MODE_STATUS,   /* status feedback on by default */
    .modbus = { .enable = 0, .address = 1, .baudrate = 9600 },
    .can    = { .mode = 0, .n2k_addr = 0x50, .base_id = 0x100,
                .bitrate = 250000, .tx_interval_ms = 1000 },
    .sntp   = { .enable = 1, .server = "pool.ntp.org" },
    .tz     = "",   /* empty = UTC */
};

#define NVS_GET_STR(h, key, dst) \
    do { size_t _l = sizeof(dst); nvs_get_str((h), (key), (dst), &_l); } while (0)

esp_err_t app_config_init(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    NVS_GET_STR(h, K_DEVICE_NAME, s_cfg.device_name);
    NVS_GET_STR(h, K_MQTT_URL,    s_cfg.mqtt_url);
    NVS_GET_STR(h, K_MQTT_USER,   s_cfg.mqtt_user);
    NVS_GET_STR(h, K_MQTT_PASS,   s_cfg.mqtt_password);
    NVS_GET_STR(h, K_MQTT_TOPIC,  s_cfg.mqtt_topic_prefix);
    NVS_GET_STR(h, K_TZ,          s_cfg.tz);

    /* DI config blob — silently keep defaults if not found or size changed
       (the latter happens when di_config_t grows beyond its reserved bytes). */
    size_t sz = sizeof(s_cfg.di);
    nvs_get_blob(h, K_DI_CFG, s_cfg.di, &sz);
    sz = sizeof(s_cfg.dout);
    nvs_get_blob(h, K_DOUT_CFG, s_cfg.dout, &sz);
    sz = sizeof(s_cfg.modbus);
    nvs_get_blob(h, K_MODBUS_CFG, &s_cfg.modbus, &sz);
    uint8_t led_mode_val = 0;
    nvs_get_u8(h, K_LED_MODE, &led_mode_val);
    s_cfg.led_mode = led_mode_val;
    sz = sizeof(s_cfg.can);
    nvs_get_blob(h, K_CAN_CFG, &s_cfg.can, &sz);
    sz = sizeof(s_cfg.sntp);
    nvs_get_blob(h, K_SNTP_CFG, &s_cfg.sntp, &sz);

    nvs_close(h);
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_update(const app_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_str(h, K_DEVICE_NAME, s_cfg.device_name);
    nvs_set_str(h, K_MQTT_URL,    s_cfg.mqtt_url);
    nvs_set_str(h, K_MQTT_USER,   s_cfg.mqtt_user);
    nvs_set_str(h, K_MQTT_PASS,   s_cfg.mqtt_password);
    nvs_set_str(h, K_MQTT_TOPIC,  s_cfg.mqtt_topic_prefix);
    nvs_set_str(h, K_TZ,          s_cfg.tz);
    nvs_set_blob(h, K_DI_CFG,    s_cfg.di,   sizeof(s_cfg.di));
    nvs_set_blob(h, K_DOUT_CFG,  s_cfg.dout, sizeof(s_cfg.dout));
    nvs_set_blob(h, K_MODBUS_CFG, &s_cfg.modbus, sizeof(s_cfg.modbus));
    nvs_set_u8(h, K_LED_MODE, s_cfg.led_mode);
    nvs_set_blob(h, K_CAN_CFG, &s_cfg.can, sizeof(s_cfg.can));
    nvs_set_blob(h, K_SNTP_CFG, &s_cfg.sntp, sizeof(s_cfg.sntp));

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
