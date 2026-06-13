#include <nvs_flash.h>
#include <esp_log.h>
#include <wifi_config.h>

static const char *TAG = "main";

static void on_wifi_ready(void)
{
    ESP_LOGI(TAG, "WiFi connected — starting application");
    /* Add application logic here */
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* First boot: starts SoftAP "Waveshare-Setup" for browser-based WiFi config.
       Subsequent boots: reconnects to saved credentials and calls on_wifi_ready. */
    wifi_config_init("Waveshare-Setup", NULL, on_wifi_ready);
}
