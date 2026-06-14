#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_spiffs.h>
#include <wifi_config.h>
#include "app_config.h"
#include "app_mqtt.h"
#include "button.h"
#include "buzzer.h"
#include "di.h"
#include "dout.h"
#include "led.h"
#include "web_server.h"

static const char *TAG = "main";

static void on_mqtt_connected(void)
{
    di_on_mqtt_connected();
    dout_on_mqtt_connected();
    led_on_mqtt_connected();
    buzzer_on_mqtt_connected();
}

static void on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen)
{
    di_on_mqtt_message(topic, tlen, data, dlen);
    dout_on_mqtt_message(topic, tlen, data, dlen);
    led_on_mqtt_message(topic, tlen, data, dlen);
    buzzer_on_mqtt_message(topic, tlen, data, dlen);
}

static void on_wifi_ready(void)
{
    ESP_LOGI(TAG, "WiFi connected");
    ESP_ERROR_CHECK(web_server_start());
    app_mqtt_set_connected_callback(on_mqtt_connected);
    app_mqtt_set_msg_callback(on_mqtt_message);
    ESP_ERROR_CHECK(app_mqtt_start());
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
    ESP_ERROR_CHECK(di_init());
    ESP_ERROR_CHECK(dout_init());
    ESP_ERROR_CHECK(led_init());
    ESP_ERROR_CHECK(buzzer_init());

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

    /* wifi_config_init creates the STA netif synchronously; DHCP only starts
       once WiFi associates, so setting the hostname here reaches the first
       DHCP DISCOVER. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_set_hostname(sta, app_config_get()->device_name);
    }
}
