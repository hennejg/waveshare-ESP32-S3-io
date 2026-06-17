#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_spiffs.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <wifi_config.h>
#include "app_config.h"
#include "app_mqtt.h"
#include "auth.h"
#include "button.h"
#include "buzzer.h"
#include "di.h"
#include "dout.h"
#include "eth.h"
#include "led.h"
#include "can_server.h"
#include "mb_server.h"
#include "web_server.h"
#include "matter.h"

#define TAG          "main"
#define NVS_ETH_NS   "app_config"
#define NVS_ETH_KEY  "eth_only"

/* ---------------------------------------------------------------- helpers */

static bool get_eth_only(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_ETH_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_ETH_KEY, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void set_eth_only(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_ETH_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (on) nvs_set_u8(h, NVS_ETH_KEY, 1);
        else    nvs_erase_key(h, NVS_ETH_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* --------------------------------------------------------- MQTT callbacks */

static void on_mqtt_connected(void)
{
    led_status_set_mqtt(true);
    di_on_mqtt_connected();
    dout_on_mqtt_connected();
    led_on_mqtt_connected();
    buzzer_on_mqtt_connected();
}

static void on_mqtt_disconnected(void) { led_status_set_mqtt(false); }

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

/* ------------------------------------------------- network callbacks */

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

static void on_ip_lost(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    led_status_set_network(false);
}

static void on_wifi_ap(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    led_status_set_ap_mode(true);
}

/* Called by wifi-bootstrap when user taps "Use Ethernet only". */
static void on_eth_only_requested(void)
{
    set_eth_only(true);
    ESP_LOGI(TAG, "ETH-only mode saved — rebooting");
}

/* ------------------------------------------------------------ app_main */

/* Custom HTML injected at the bottom of the WiFi provisioning page. */
static const char s_portal_extra[] =
    "<div style='margin-top:1.5rem;border-top:1px solid #ccc;padding-top:1rem;"
    "font-family:sans-serif;font-size:.9rem;color:#555;text-align:center'>"
    "<p>Have an Ethernet cable? You can skip WiFi entirely.</p>"
    "<a href='/eth-only' style='display:inline-block;padding:.4rem 1rem;"
    "background:#1a73e8;color:#fff;border-radius:4px;text-decoration:none'>"
    "Use Ethernet only &rarr;</a>"
    "<p style='margin-top:.8rem;color:#999;font-size:.8rem'>"
    "To return to WiFi setup: hold the BOOT button for &ge;&nbsp;5&nbsp;s.</p>"
    "</div>";

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
    esp_err_t spiffs_ret = esp_vfs_spiffs_register(&spiffs);
    if (spiffs_ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s) — web UI unavailable; "
                 "reflash storage partition to restore it",
                 esp_err_to_name(spiffs_ret));
    }

    bool eth_only = get_eth_only();

    if (!eth_only) {
        /* Normal: WiFi provisioning + STA. */
        wifi_config_set_custom_html((char *)s_portal_extra);
        wifi_config_set_eth_only_callback(on_eth_only_requested);
        wifi_config_init("Waveshare-Setup", NULL, on_wifi_ready);

        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_set_hostname(sta, app_config_get()->device_name);
    } else {
        ESP_LOGI(TAG, "ETH-only mode — skipping WiFi");
    }

    /* Ethernet always. Event loop is ready after wifi_config_init (or immediately). */
    if (eth_only) esp_netif_init(), esp_event_loop_create_default();
    esp_err_t eth_ret = eth_init(on_eth_ready);
    if (eth_ret != ESP_OK)
        ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));

    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_LOST_IP, on_ip_lost,  NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_ETH_LOST_IP, on_ip_lost,  NULL);
    if (!eth_only)
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_ap, NULL);

    /* Matter — initialise after network interfaces so the event loop is ready */
    esp_err_t matter_ret = matter_init();
    if (matter_ret != ESP_OK)
        ESP_LOGW(TAG, "Matter init failed: %s", esp_err_to_name(matter_ret));
}
