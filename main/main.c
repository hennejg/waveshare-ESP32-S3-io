#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_spiffs.h>
#include <esp_event.h>
#include <wifi_config.h>
#include "app_config.h"
#include "app_mqtt.h"
#include "button.h"
#include "buzzer.h"
#include "di.h"
#include "dout.h"
#include "eth.h"
#include "led.h"
#include "can_server.h"
#include "mb_server.h"
#include "web_server.h"

static const char *TAG = "main";

/* --------------------------------------------------------- MQTT callbacks -- */

static void on_mqtt_connected(void)
{
    led_status_set_mqtt(true);
    di_on_mqtt_connected();
    dout_on_mqtt_connected();
    led_on_mqtt_connected();
    buzzer_on_mqtt_connected();
}

static void on_mqtt_disconnected(void)
{
    led_status_set_mqtt(false);
}

static void on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen)
{
    led_status_flash_rx();
    di_on_mqtt_message(topic, tlen, data, dlen);
    dout_on_mqtt_message(topic, tlen, data, dlen);
    led_on_mqtt_message(topic, tlen, data, dlen);
    buzzer_on_mqtt_message(topic, tlen, data, dlen);
}

static void on_mqtt_publish(void) { led_status_flash_tx(); }

/* --------------------------------------------------- network callbacks -- */

/* Called when either WiFi or Ethernet obtains an IP.
   Both web_server_start() and app_mqtt_start() are idempotent. */
static void on_network_ready(const char *iface)
{
    ESP_LOGI(TAG, "%s connected — starting services", iface);
    led_status_set_network(true);
    ESP_ERROR_CHECK(web_server_start());
    app_mqtt_set_connected_callback(on_mqtt_connected);
    app_mqtt_set_disconnected_callback(on_mqtt_disconnected);
    app_mqtt_set_msg_callback(on_mqtt_message);
    app_mqtt_set_publish_callback(on_mqtt_publish);
    ESP_ERROR_CHECK(app_mqtt_start());
}

static void on_wifi_ready(void) { on_network_ready("WiFi"); }
static void on_eth_ready(void)  { on_network_ready("Ethernet"); }

/* Track IP loss so status LED can fall back to yellow/green. */
static void on_ip_lost(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    led_status_set_network(false);
}

/* ------------------------------------------------------------ app_main -- */

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
    ESP_ERROR_CHECK(mb_server_init());
    ESP_ERROR_CHECK(can_server_init());

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path              = "/www",
        .partition_label        = "storage",
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs));

    /* WiFi: captive portal on first boot, reconnects on subsequent boots. */
    wifi_config_init("Waveshare-Setup", NULL, on_wifi_ready);

    /* Set WiFi STA hostname before DHCP starts. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_hostname(sta, app_config_get()->device_name);

    /* Ethernet: W5500 over SPI. Event loop is ready after wifi_config_init. */
    esp_err_t eth_ret = eth_init(on_eth_ready);
    if (eth_ret != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet init failed: %s (continuing without ETH)",
                 esp_err_to_name(eth_ret));
    }

    /* Watch for IP loss so status LED falls back to green or yellow. */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,  on_ip_lost, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,  on_ip_lost, NULL);
}
