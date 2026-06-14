#include "app_config.h"
#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>

#define NVS_NS "app_config"

/* NVS key names must be <= 15 chars. */
#define K_DEVICE_NAME   "device_name"
#define K_MQTT_URL      "mqtt_url"
#define K_MQTT_USER     "mqtt_user"
#define K_MQTT_PASS     "mqtt_password"
#define K_MQTT_TOPIC    "mqtt_topic"

static app_config_t s_cfg = {
    .device_name      = "Waveshare-ESP32",
    .mqtt_url         = "",
    .mqtt_user        = "",
    .mqtt_password    = "",
    .mqtt_topic_prefix = "",
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

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
