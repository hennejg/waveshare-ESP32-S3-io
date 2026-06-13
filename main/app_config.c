#include "app_config.h"
#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>

#define NVS_NS "app_config"

static app_config_t s_cfg = {
    .device_name = "Waveshare-ESP32",
};

esp_err_t app_config_init(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* use compiled-in defaults */
    }
    if (ret != ESP_OK) return ret;

    size_t len = sizeof(s_cfg.device_name);
    nvs_get_str(h, "device_name", s_cfg.device_name, &len);

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

    nvs_set_str(h, "device_name", s_cfg.device_name);

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
