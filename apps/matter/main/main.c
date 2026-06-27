#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <wifi_config.h>
#include "app_config.h"
#include "auth.h"
#include "button.h"
#include "di.h"
#include "dout.h"
#include "eth.h"
#include "led.h"
#include "matter.h"
#include "web_server.h"

#define TAG "main"

static bool s_eth_connected = false;
static bool is_eth_connected(void) { return s_eth_connected; }

static void on_network_ready(void)
{
    led_status_set_network(true);
    web_server_start();
}

static void on_eth_ready(void)
{
    s_eth_connected = true;
    on_network_ready();
}

static void on_ip_lost(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) s_eth_connected = false;
    led_status_set_network(false);
    web_server_stop();
}

static void on_matter_wifi_got_ip(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    if (!s_eth_connected)
        on_network_ready();
}

static void on_wifi_ap(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    led_status_set_ap_mode(true);
    web_server_start();
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
    ESP_ERROR_CHECK(auth_init());

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path              = "/www",
        .partition_label        = "storage",
        .max_files              = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t spiffs_ret = esp_vfs_spiffs_register(&spiffs);
    if (spiffs_ret != ESP_OK)
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) — web UI unavailable",
                 esp_err_to_name(spiffs_ret));

    button_init();
    ESP_ERROR_CHECK(di_init());
    ESP_ERROR_CHECK(dout_init());
    ESP_ERROR_CHECK(led_init());

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_lost, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_lost, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_matter_wifi_got_ip, NULL);

    esp_err_t eth_ret = eth_init(on_eth_ready);
    if (eth_ret != ESP_OK)
        ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));
    else {
        for (int i = 0; i < 10 && !eth_link_is_up(); i++)
            vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "ETH link %s before matter_init", eth_link_is_up() ? "UP" : "down");
    }

    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    ESP_LOGI(TAG, "heap before matter_init: free=%u DMA_free=%u DMA_largest=%u",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    esp_err_t matter_ret = matter_init();
    if (matter_ret != ESP_OK)
        ESP_LOGW(TAG, "Matter init failed: %s", esp_err_to_name(matter_ret));

    ESP_LOGI(TAG, "heap after matter_init: free=%u internal_free=%u internal_largest=%u",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_ap, NULL);
    wifi_config_set_eth_available_fn(is_eth_connected);

    /* Always init WiFi — creates a SoftAP in uncommissioned state so the web UI
     * (QR code / status) is reachable before Matter commissioning completes.
     * In commissioned state: suppress the AP if credentials are already stored
     * so the device connects STA-only without advertising a provisioning AP. */
    {
        char *ssid = NULL;
        wifi_config_get(&ssid, NULL);
        if (matter_is_commissioned() && ssid && ssid[0])
            wifi_config_disable_ap();
        free(ssid);
        wifi_config_init("Waveshare Matter (192.168.4.1)", NULL, NULL);
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_hostname(sta, app_config_get()->device_name);
}
