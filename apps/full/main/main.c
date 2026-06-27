#include <string.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_spiffs.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#ifdef CONFIG_BT_ENABLED
#include <esp_bt.h>
#endif
#include <wifi_config.h>
#include "app_config.h"
#include "app_time.h"
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
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif
#include "scripting.h"
#include "app_rtc.h"
#include "sntp_sync.h"
#include <sys/time.h>

#define TAG          "main"

// ── Demo rule script ─────────────────────────────────────────────────────────
// Publish "ON" to <prefix>/rules/trigger (or /rules/trigger for absolute) to fire it.
// The rule also requires digital input 0 to be inactive (not conducting).
const char DEMO_SCRIPT[] =
    "var trig = mqtt('rules/trigger').is(function(m) { return m === 'ON'; });\n"
    "rule('demo')\n"
    "  .when(trig, input(0).is(false))\n"
    "  .then(function() {\n"
    "    print('Demo rule fired! payload=' + trig.value);\n"
    "    output(0).set(true);\n"
    "  });\n";

static const scripting_io_t s_scripting_io = {
    .di_get        = di_get,
    .dout_set      = dout_set,
    .dout_get      = dout_get,
    .mqtt_subscribe = app_mqtt_subscribe,
    .led_set       = led_set_rgb,      /* effective in IO mode (no-op in status mode) */
    .buzzer_set    = buzzer_set_tone,
};
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

/* Feed the rule engine the broker connection state as an internal '$sys/mqtt'
 * message ("up"/"down"), so rules can gate on mqttConnected()/mqttDown(). Injected
 * locally via the normal message path — never published to or read from the broker. */
static void scripting_feed_mqtt_state(const char *state)
{
    scripting_on_mqtt_message("$sys/mqtt", 9, state, strlen(state));
}

static void on_mqtt_connected(void)
{
    led_status_set_mqtt(true);
    di_on_mqtt_connected();
    dout_on_mqtt_connected();
    led_on_mqtt_connected();
    buzzer_on_mqtt_connected();
    scripting_on_mqtt_connected();
    scripting_feed_mqtt_state("up");
}

static void on_mqtt_disconnected(void)
{
    led_status_set_mqtt(false);
    scripting_feed_mqtt_state("down");
}

static void on_mqtt_message(const char *topic, size_t tlen,
                             const char *data,  size_t dlen)
{
    led_status_flash_rx();
    di_on_mqtt_message(topic, tlen, data, dlen);
    dout_on_mqtt_message(topic, tlen, data, dlen);
    led_on_mqtt_message(topic, tlen, data, dlen);
    buzzer_on_mqtt_message(topic, tlen, data, dlen);
    scripting_on_mqtt_message(topic, tlen, data, dlen);
}

static void on_mqtt_publish(void) { led_status_flash_tx(); }

/* ------------------------------------------------- network callbacks */

static void on_network_ready(const char *iface)
{
    ESP_LOGI(TAG, "%s connected — starting services", iface);
    led_status_set_network(true);
    esp_err_t ws_ret = web_server_start();
    if (ws_ret != ESP_OK)
        ESP_LOGW(TAG, "Web server start failed: %s", esp_err_to_name(ws_ret));
    app_mqtt_set_connected_callback(on_mqtt_connected);
    app_mqtt_set_disconnected_callback(on_mqtt_disconnected);
    app_mqtt_set_msg_callback(on_mqtt_message);
    app_mqtt_set_publish_callback(on_mqtt_publish);
    esp_err_t mqtt_ret = app_mqtt_start();
    if (mqtt_ret != ESP_OK)
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(mqtt_ret));
    sntp_sync_apply();   /* start NTP sync now that the network is up */
}

static bool s_eth_connected = false;

static bool is_eth_connected(void) { return s_eth_connected; }

static void on_wifi_ready(void) { on_network_ready("WiFi"); }

static void on_eth_ready(void)
{
    s_eth_connected = true;
    on_network_ready("Ethernet");
}

static void on_ip_lost(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) s_eth_connected = false;
    led_status_set_network(false);
    web_server_stop();   /* free port 80 before wifi-bootstrap may restart the AP HTTP server */
}

static void on_wifi_ap(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    led_status_set_ap_mode(true);
    /* web_server (INADDR_ANY:80) and wifi_config's captive-portal server
     * (192.168.4.1:80) coexist: both sockets have SO_REUSEADDR, which lwIP
     * allows (CONFIG_LWIP_SO_REUSE=y).  lwIP routes 192.168.4.1 connections
     * to the specific binding and ETH-IP connections to the wildcard. */
}

/* Called by wifi-bootstrap when user taps "Use Ethernet only". */
static void on_eth_only_requested(void)
{
    set_eth_only(true);
    ESP_LOGI(TAG, "ETH-only mode saved — rebooting");
}

/* Days-from-civil (UTC) → epoch seconds — dependency-free (avoids relying on timegm). */
static time_t utc_tm_to_epoch(const struct tm *t)
{
    int      y   = t->tm_year + 1900;
    int      m   = t->tm_mon + 1;
    int      d   = t->tm_mday;
    y -= (m <= 2);
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long     days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

/* ------------------------------------------------------------ app_main */

#ifdef CONFIG_APP_MATTER_ENABLE
/* Called when Matter's ConnectivityManager connects WiFi STA post-commissioning.
 * Start application services on WiFi only if Ethernet hasn't already done so. */
static void on_matter_wifi_got_ip(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    if (!s_eth_connected)
        on_wifi_ready();
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(app_config_init());
    app_time_apply_tz();   /* local time for cron + UI display; applied before scripting starts */
    ESP_ERROR_CHECK(auth_init());
    button_init();
    ESP_ERROR_CHECK(di_init());
    ESP_ERROR_CHECK(dout_init());

    /* RTC: seed the system clock from the battery-backed PCF85063 if its stored time
     * is valid (i.e. set by a previous SNTP sync). SNTP will refine/correct it once the
     * network is up; with no valid RTC time we simply wait for SNTP. Non-fatal — a
     * missing/failed RTC must not stop the device from booting. */
    if (rtc_dev_init() == ESP_OK) {
        struct tm rt;
        bool valid = false;
        if (rtc_dev_read(&rt, &valid) == ESP_OK && valid) {
            struct timeval tv = { .tv_sec = utc_tm_to_epoch(&rt), .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System clock seeded from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
                     rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
                     rt.tm_hour, rt.tm_min, rt.tm_sec);
            /* Real time available before the engine starts → cron may arm at load. */
            scripting_set_time_valid();
        } else {
            ESP_LOGW(TAG, "RTC time not valid yet — waiting for SNTP");
        }
    } else {
        ESP_LOGW(TAG, "RTC not available");
    }

    /* Load user script from NVS; fall back to built-in demo if none stored. */
    const char *startup_script = DEMO_SCRIPT;
    static char s_nvs_script_buf[4096];
    {
        nvs_handle_t h;
        size_t len = sizeof(s_nvs_script_buf);
        if (nvs_open("scripting", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "script", s_nvs_script_buf, &len) == ESP_OK)
                startup_script = s_nvs_script_buf;
            nvs_close(h);
        }
    }
    ESP_ERROR_CHECK(scripting_init(startup_script, &s_scripting_io));
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

#ifdef CONFIG_APP_MATTER_ENABLE
    /* Init order: Ethernet → Matter (BLE).
     *
     * Ethernet (W5500 SPI) allocates from PSRAM (emac struct, rx_buffer) and
     * does not consume internal DMA heap.  Initialising it first lets us detect
     * the physical link before Matter configures its NetworkCommissioning
     * cluster: if Ethernet link is up at that point, Matter uses Ethernet
     * NetworkCommissioning (commissioner skips WiFi credential provisioning and
     * reaches the device via mDNS on Ethernet).  If the link is down, Matter
     * falls back to WiFi NetworkCommissioning.
     *
     * BLE still gets a contiguous DMA block because Ethernet init does not
     * allocate from the DMA heap — the static dma_buf is in .dram1.bss and
     * rx_buffer uses MALLOC_CAP_DEFAULT (→ PSRAM via SPIRAM_MALLOC_ALWAYSINTERNAL).
     *
     * Pre-register the STA netif BEFORE matter_init so that CHIP's
     * InitWiFiStack finds it and skips creating a duplicate.  The AP (SoftAP)
     * is intentionally NOT pre-created: CHIPoBLE handles commissioning, so the
     * captive-portal AP is unnecessary and its beacon buffer (752 B DMA) would
     * crash after BLE has consumed the main free block. */
    if (!eth_only) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
    }

#ifdef CONFIG_BT_ENABLED
    /* Release classic-BT memory (only relevant when BT stack is compiled in). */
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif

    if (!eth_only) {
        /* Ethernet init first — lets matter_init() detect the link state. */
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_lost, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_lost, NULL);
        esp_err_t eth_ret = eth_init(on_eth_ready);
        if (eth_ret != ESP_OK)
            ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));
        else {
            /* Wait up to 500 ms for physical link detection (W5500 auto-neg). */
            for (int i = 0; i < 10 && !eth_link_is_up(); i++)
                vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "ETH link %s before matter_init",
                     eth_link_is_up() ? "UP" : "down");
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
    }
#endif

#ifndef CONFIG_APP_MATTER_ENABLE
    /* Non-Matter path: Ethernet init here (Matter path already did it above). */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_LOST_IP, on_ip_lost,  NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_ETH_LOST_IP, on_ip_lost,  NULL);
    esp_err_t eth_ret = eth_init(on_eth_ready);
    if (eth_ret != ESP_OK)
        ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));
#endif

    if (!eth_only) {
#ifdef CONFIG_APP_MATTER_ENABLE
        /* Do NOT call wifi_config_init when BLE is active (uncommissioned state):
         * the wifi_config monitor task competes for the same scarce DMA heap.
         *
         * Once commissioned, BLE has been deinitialized and it is safe to use
         * wifi_config for all WiFi management.  wifi_config connects directly if
         * sysparam holds credentials, or opens the captive portal if not.
         * Matter stores its WiFi credentials in chip-kvs (separate from sysparam);
         * after a button-triggered reset or a captive-portal re-provision, chip-kvs
         * may be stale or empty, so wifi_config must always drive the connection. */
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   on_matter_wifi_got_ip, NULL);
        if (matter_is_commissioned()) {
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_ap, NULL);
            wifi_config_set_eth_only_callback(on_eth_only_requested);
            wifi_config_set_eth_available_fn(is_eth_connected);
            /* If credentials are already stored, suppress the captive portal entirely.
             * Without this, wifi_config's monitor fires on any brief disconnect and
             * calls wifi_config_softap_start() → esp_wifi_scan_start(NULL, true) — a
             * blocking all-channel scan that holds the radio for several seconds every
             * 10 s, starving STA traffic of ACKs and causing send() EAGAIN on the httpd. */
            char *ssid = NULL;
            wifi_config_get(&ssid, NULL);
            if (ssid && ssid[0])
                wifi_config_disable_ap();
            free(ssid);
            wifi_config_init("Waveshare (192.168.4.1)", NULL, NULL);
        }
#else
        /* Non-Matter: captive-portal WiFi provisioning via wifi-bootstrap. */
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_ap, NULL);
        wifi_config_set_eth_only_callback(on_eth_only_requested);
        wifi_config_set_eth_available_fn(is_eth_connected);
        wifi_config_init("Waveshare (192.168.4.1)", NULL, on_wifi_ready);
#endif

        /* STA netif may already exist (created by CHIP's InitWiFiStack above). */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_set_hostname(sta, app_config_get()->device_name);
    } else {
        ESP_LOGI(TAG, "ETH-only mode — skipping WiFi");
    }
}
