#include <stdio.h>
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
#include "aux_io.h"
#include "eth.h"
#include "led.h"
#include "can_server.h"
#include "mb_server.h"
#include "mb_tcp_master.h"
#include "web_server.h"
#ifdef CONFIG_APP_MATTER_ENABLE
#include "matter.h"
#endif
#include "scripting.h"
#include "sntp_sync.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

/* output(0..7) is DO1-DO8, output(8..10) a header pad set to output -- the
   rule engine sees one row of outputs, as it sees one row of inputs. */
static esp_err_t rules_out_set(uint8_t ch, bool on)
{
    return ch < APP_CFG_DO_COUNT ? dout_set(ch, on) : aux_io_out_set((uint8_t)(ch - APP_CFG_DO_COUNT), on);
}
static bool rules_out_get(uint8_t ch)
{
    bool on = false;
    if (ch < APP_CFG_DO_COUNT) return dout_get(ch);
    aux_io_out_get((uint8_t)(ch - APP_CFG_DO_COUNT), &on);
    return on;
}
static esp_err_t rules_out_toggle(uint8_t ch)
{
    return ch < APP_CFG_DO_COUNT ? dout_toggle(ch) : aux_io_out_toggle((uint8_t)(ch - APP_CFG_DO_COUNT));
}

#if CONFIG_APP_AUX_DI_ENABLE
/* The rule engine numbers the pads 1..3, like AUX1..AUX3; aux_io 0..2. */
static esp_err_t rules_pwm_set(uint8_t pad, uint32_t freq_hz, uint16_t duty_pm)
{
    return (pad >= 1 && pad <= 3) ? aux_io_pwm_set((uint8_t)(pad - 1), freq_hz, duty_pm)
                                  : ESP_ERR_INVALID_ARG;
}
static bool rules_pwm_get(uint8_t pad, uint32_t *freq_hz, uint16_t *duty_pm)
{
    return pad >= 1 && pad <= 3 && aux_io_pwm_info((uint8_t)(pad - 1), freq_hz, duty_pm, NULL);
}
static esp_err_t rules_analog_get(uint8_t pad, int *mv)
{
    return (pad >= 1 && pad <= 3) ? aux_io_analog_mv((uint8_t)(pad - 1), mv) : ESP_ERR_INVALID_ARG;
}
#endif

/* The poll table hands its values to the rules itself (on_modbus_value())
   and publishes them: subscribed at the broker as well, each would reach a
   rule twice. */
static bool rules_mqtt_local(const char *topic)
{
    return strncmp(topic, "modbus/", 7) == 0;
}

/* The rule engine's subscriptions as its own owner: a filter a module
   also holds ("output/set") stays when the rules stop reading it. */
static int rules_mqtt_subscribe(const char *topic, int qos)
{
    return app_mqtt_subscribe_as(APP_MQTT_OWNER_RULES, topic, qos);
}
static int rules_mqtt_unsubscribe(const char *topic)
{
    return app_mqtt_unsubscribe_as(APP_MQTT_OWNER_RULES, topic);
}

static const scripting_io_t s_scripting_io = {
    .di_get         = di_get,
    .di_count       = APP_CFG_DI_COUNT,
#if CONFIG_APP_AUX_DI_ENABLE
    .aux_pwm_set    = rules_pwm_set,
    .aux_pwm_get    = rules_pwm_get,
    .aux_analog_get = rules_analog_get,
#endif
    .dout_set       = rules_out_set,
    .dout_get       = rules_out_get,
    .dout_toggle    = rules_out_toggle,
    .do_count       = APP_CFG_DO_COUNT + APP_CFG_DI_AUX_COUNT,
    .mqtt_subscribe = rules_mqtt_subscribe,
    .mqtt_unsubscribe = rules_mqtt_unsubscribe,
    .mqtt_local     = rules_mqtt_local,
    .mqtt_publish   = app_mqtt_publish,
    .led_set        = led_set_rgb,      /* effective in IO mode (no-op in status mode) */
    .buzzer_set     = buzzer_set_tone,
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
 * message ("up"/"down"), so rules can gate on mqttConnected()/mqttDown(). Local
 * only -- never published to or read from the broker -- and kept as state by the
 * engine, so a full event queue cannot lose it. */
static void scripting_feed_mqtt_state(const char *state)
{
    scripting_on_mqtt_state(!strcmp(state, "up"));
}

static void on_mqtt_connected(void)
{
    led_status_set_mqtt(true);
    di_on_mqtt_connected();
    dout_on_mqtt_connected();
    aux_io_on_mqtt_connected();
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
    aux_io_on_mqtt_message(topic, tlen, data, dlen);
    led_on_mqtt_message(topic, tlen, data, dlen);
    buzzer_on_mqtt_message(topic, tlen, data, dlen);
    scripting_on_mqtt_message(topic, tlen, data, dlen);
}

static void on_mqtt_publish(void) { led_status_flash_tx(); }

/* A value the poll table has read: on MQTT as modbus/<name>, and straight to
   the rule engine under the same topic rather than round the broker -- a
   rule must work on a board with no broker configured, and the broker does
   not echo our own publishes back to us anyway. */
static void on_modbus_value(uint8_t index, const mbm_poll_t *e, double value)
{
    (void)index;
    char topic[8 + APP_CFG_MBM_NAME_LEN + 1];
    snprintf(topic, sizeof(topic), "modbus/%s", e->name);

    char payload[32];
    /* Six significant digits turn a 32-bit counter into 4.29497e+09, which
       reads back as 4294970000 -- the status API and the published value then
       disagree about the same reading. Ten covers u32 and s32 exactly without
       printing noise from below a float32's precision. */
    int len = snprintf(payload, sizeof(payload), "%.10g", value);
    if (len < 0) return;
    if (len >= (int)sizeof(payload)) len = (int)sizeof(payload) - 1;

    app_mqtt_publish(topic, payload, len, 0, false);
    scripting_on_mqtt_message(topic, strlen(topic), payload, (size_t)len);
}

/* ------------------------------------------------- network callbacks */

/* on_network_ready() runs once per interface, and the two arrive on
   different tasks: WiFi from the wifi-bootstrap timer, Ethernet from the
   event loop. Every start function below checks "already running" and then
   starts, which only holds if no second caller can get between the two --
   with both links coming up together at boot, two Modbus TCP pollers ran on
   one set of slots. Serialised here, once, for all of them. */
static SemaphoreHandle_t s_net_ready_mux;

static void on_network_ready(const char *iface)
{
    if (s_net_ready_mux) xSemaphoreTake(s_net_ready_mux, portMAX_DELAY);
    ESP_LOGI(TAG, "%s connected — starting services", iface);
    led_status_set_network(true);
    esp_err_t ws_ret = web_server_start();
    if (ws_ret != ESP_OK)
        ESP_LOGW(TAG, "Web server start failed: %s", esp_err_to_name(ws_ret));
    /* Modbus TCP opens a listening socket, so it cannot be started from
       app_main() with the rest of Modbus -- lwIP is not up at that point. */
    esp_err_t mbn_ret = mb_server_net_start();
    if (mbn_ret != ESP_OK)
        ESP_LOGE(TAG, "Modbus TCP start failed: %s", esp_err_to_name(mbn_ret));
    esp_err_t mbm_ret = mb_tcp_master_start();
    if (mbm_ret != ESP_OK)
        ESP_LOGE(TAG, "Modbus TCP master start failed: %s", esp_err_to_name(mbm_ret));
    app_mqtt_set_connected_callback(on_mqtt_connected);
    app_mqtt_set_disconnected_callback(on_mqtt_disconnected);
    app_mqtt_set_msg_callback(on_mqtt_message);
    app_mqtt_set_publish_callback(on_mqtt_publish);
    esp_err_t mqtt_ret = app_mqtt_start();
    if (mqtt_ret != ESP_OK)
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(mqtt_ret));
    sntp_sync_apply();   /* start NTP sync now that the network is up */
    if (s_net_ready_mux) xSemaphoreGive(s_net_ready_mux);
}

static bool s_eth_connected  = false;
static bool s_wifi_connected = false;

static bool is_eth_connected(void) { return s_eth_connected; }

static void on_wifi_ready(void)
{
    s_wifi_connected = true;
    on_network_ready("WiFi");
}

static void on_eth_ready(void)
{
    s_eth_connected = true;
    on_network_ready("Ethernet");
}

static void on_ip_lost(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) s_eth_connected  = false;
    if (id == IP_EVENT_STA_LOST_IP) s_wifi_connected = false;

    /* The web server listens on INADDR_ANY and is shared by both interfaces.
       Stopping it whenever either one loses its address took the Ethernet UI
       down when WiFi dropped -- and nothing brought it back, because the
       Ethernet link never produced a new Got-IP event. Only stop once no
       usable interface is left. */
    if (s_eth_connected || s_wifi_connected) return;

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
    /* Before anything that can bring a network interface up. */
    s_net_ready_mux = xSemaphoreCreateMutex();
    if (!s_net_ready_mux) ESP_LOGE(TAG, "network start mutex: out of memory");
    sntp_sync_init();
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
    /* After di_init(): a pad set to input joins the input path. Not fatal --
       a pad that cannot take its mode is left in its standard function. */
    esp_err_t aux_ret = aux_io_init();
    if (aux_ret != ESP_OK)
        ESP_LOGE(TAG, "IO1/RXD/TXD pads: %s", esp_err_to_name(aux_ret));
    ESP_ERROR_CHECK(dout_init());

    /* RTC: seed the system clock from the battery-backed PCF85063 if its stored time
     * is valid (i.e. set by a previous SNTP sync). SNTP will refine/correct it once the
     * network is up. Real time available before the engine starts → cron may arm at load. */
    if (app_time_seed_from_rtc()) scripting_set_time_valid();

    /* Load user script from NVS; fall back to built-in demo if none stored. */
    const char *startup_script = DEMO_SCRIPT;
    {
        char *stored = NULL;   /* kept for good: scripting_init() wants it for life */
        esp_err_t err = rules_nvs_read(&stored);
        if (err == ESP_OK)
            startup_script = stored;
        else if (err != ESP_ERR_NVS_NOT_FOUND)
            /* Not quietly: the demo runs in place of the user's rules. */
            ESP_LOGE(TAG, "stored rules unreadable (%s) - running the demo", esp_err_to_name(err));
    }
    /* The script task runs at a higher priority than app_main and evaluates
       the stored script the moment it exists, so everything a top-level
       statement can reach has to be there first: buzzer().off() outside a
       rule hit the buzzer's queue before buzzer_init() had created it, and
       the assert in xQueueOverwrite() rebooted the device -- at every boot,
       because the script is stored. */
    ESP_ERROR_CHECK(led_init());
    ESP_ERROR_CHECK(buzzer_init());
    ESP_ERROR_CHECK(scripting_init(startup_script, &s_scripting_io));
    /* Not fatal: both take a stored baudrate/bitrate, and eth_init() runs much
       further down. Aborting here would leave the unit with no network and no
       web UI to correct the value with — an endless boot loop recoverable only
       over the serial port. Log and carry on instead. */
    esp_err_t mb_ret = mb_server_init();
    if (mb_ret != ESP_OK)
        ESP_LOGE(TAG, "Modbus server init failed: %s — continuing without Modbus",
                 esp_err_to_name(mb_ret));
    esp_err_t mbm_init_ret = mb_tcp_master_init(on_modbus_value, false);
    if (mbm_init_ret != ESP_OK)
        ESP_LOGE(TAG, "Modbus TCP master init failed: %s — continuing without it",
                 esp_err_to_name(mbm_init_ret));
    /* Heartbeat status: the broker; control frames feed the rule engine's
       CAN command-health check. */
    const can_server_hooks_t can_hooks = {
        .online     = app_mqtt_is_connected,
        .on_command = scripting_on_can_activity,
    };
    esp_err_t can_ret = can_server_init(&can_hooks);
    if (can_ret != ESP_OK)
        ESP_LOGE(TAG, "CAN server init failed: %s — continuing without CAN",
                 esp_err_to_name(can_ret));

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
    /* The IP stack and Ethernet in either case: in ETH-only mode this path
       used to skip them along with WiFi and Matter, and the board came up
       with no network at all. */
    esp_netif_init();
    esp_event_loop_create_default();
    if (!eth_only)
        esp_netif_create_default_wifi_sta();

#ifdef CONFIG_BT_ENABLED
    /* Release classic-BT memory (only relevant when BT stack is compiled in). */
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif

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

    if (!eth_only) {
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

    /* RS-485 rows of the poll table need no network: a board with neither an
       Ethernet link nor WiFi still reads the devices on its own segment, and
       rules get their values. Started here, once the IP stack exists -- the
       poller's network rows only fail until a link comes up, and
       on_network_ready()'s start is then a no-op. */
    if (mb_tcp_master_has_rtu()) {
        /* Under the same lock as on_network_ready(): eth_init() above has
           started the link, and a quick DHCP can bring Got-IP -- and its
           start of the same poller -- in between. */
        if (s_net_ready_mux) xSemaphoreTake(s_net_ready_mux, portMAX_DELAY);
        esp_err_t mbm_ret = mb_tcp_master_start();
        if (s_net_ready_mux) xSemaphoreGive(s_net_ready_mux);
        if (mbm_ret != ESP_OK)
            ESP_LOGE(TAG, "Modbus master start failed: %s", esp_err_to_name(mbm_ret));
    }

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
            /* Commissioned: no captive portal, unless the BOOT long press
             * asked for it. Suppressing it only when wifi_cfg held an SSID
             * left it open on a board commissioned over Ethernet or Matter
             * WiFi (whose credentials are not in wifi_cfg) -- an open AP
             * taking an SSID from anyone in range. It also costs: the
             * monitor's AP start runs a blocking all-channel scan every
             * 10 s that starves STA traffic. */
            if (!wifi_config_reset_requested())
                wifi_config_disable_ap();
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
