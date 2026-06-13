#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <wifi_config.h>
#include "app_config.h"
#include "button.h"
#include "web_server.h"

static const char *TAG = "main";

static void on_wifi_ready(void)
{
    ESP_LOGI(TAG, "WiFi connected — starting web server");
    ESP_ERROR_CHECK(web_server_start());
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(app_config_init());
    button_init();

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path              = "/www",
        .partition_label        = "storage",
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs));

    /* First boot: SoftAP "Waveshare-Setup" → captive portal → save credentials.
       Subsequent boots: reconnects and calls on_wifi_ready. */
    wifi_config_init("Waveshare-Setup", NULL, on_wifi_ready);
}
