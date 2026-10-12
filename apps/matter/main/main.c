#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <wifi_config.h>
#include "app_config.h"
#include "app_time.h"
#include "auth.h"
#include "aux_io.h"
#include "button.h"
#include "buzzer.h"
#include "can_server.h"
#include "di.h"
#include "dout.h"
#include "eth.h"
#include "led.h"
#include "matter.h"
#include "matter_mbm.h"
#include "mb_server.h"
#include "mb_tcp_master.h"
#include "sntp_sync.h"
#include "web_server.h"

#define TAG "main"

static bool s_eth_connected  = false;
static bool s_wifi_connected = false;

/* Modbus (RTU slave or RS-485 master with the poll table, Modbus TCP server)
   and CAN run only on a commissioned device. Neither has authentication: a
   device nobody has taken into a fabric yet does not answer on RS-485, CAN
   or port 502.
   (This was a RAM limit first -- about 10 KB free during commissioning --
   until the Matter stack's static data moved to PSRAM; now 74 KB are left
   at the lowest.) mb_server_init() runs once; mb_server_net_start() whenever
   an interface comes up (it is idempotent). */
/* The two paths -- the start below and an interface coming up -- run on
   different tasks; mb_server_net_start() must not run twice at once. */
static bool              s_mb_inited;
static SemaphoreHandle_t s_mb_mux;

static void modbus_net_start_locked(void)
{
    if (!s_mb_inited || !(s_eth_connected || s_wifi_connected)) return;
    esp_err_t r = mb_server_net_start();
    if (r != ESP_OK) ESP_LOGE(TAG, "Modbus TCP start failed: %s", esp_err_to_name(r));
}

static void fieldbus_start(void)
{
    if (!s_mb_mux || !matter_is_commissioned()) return;
    xSemaphoreTake(s_mb_mux, portMAX_DELAY);
    if (!s_mb_inited) {
        /* CAN once, like Modbus. Its worker only switches outputs and the
           LED and sends frames, so its stack can be in PSRAM. Heartbeat
           status stays 0x01: there is no broker in this firmware. */
        const can_server_hooks_t can_hooks = { .psram_stack = true };
        esp_err_t c = can_server_init(&can_hooks);
        if (c != ESP_OK)
            ESP_LOGE(TAG, "CAN init failed: %s — continuing without CAN", esp_err_to_name(c));

        s_mb_inited = true;
        esp_err_t r = mb_server_init();
        if (r != ESP_OK)
            ESP_LOGE(TAG, "Modbus init failed: %s — continuing without Modbus", esp_err_to_name(r));
        /* RS-485 rows need no network; the task starts only with one enabled. */
        if (r == ESP_OK && mb_tcp_master_has_rtu()) {
            r = mb_tcp_master_start();
            if (r != ESP_OK) ESP_LOGE(TAG, "Modbus poll table: %s", esp_err_to_name(r));
        }
        modbus_net_start_locked();
    }
    xSemaphoreGive(s_mb_mux);
}

/* The poll table was changed in the web UI. Applied at once, but only where
   Modbus already runs: before commissioning the save waits for the start. */
void app_modbus_table_changed(void)
{
    if (!s_mb_mux) return;
    xSemaphoreTake(s_mb_mux, portMAX_DELAY);
    if (s_mb_inited) mb_tcp_master_reload();
    xSemaphoreGive(s_mb_mux);
}

/* A value the poll table read: to its Matter sensor endpoint, if it has one. */
static void on_modbus_value(uint8_t index, const mbm_poll_t *e, double value)
{
    matter_mbm_update(index, e->matter, value);
}

/* After a fresh commissioning (matter.cpp waits a few seconds for BLE and
   the setup AP to hand their memory back). On the CHIP task, from its timer:
   a task of our own for this needs a 4 KB internal stack, and with one Modbus
   did not start after a commissioning -- internal RAM was at about 4 KB then. */
void matter_on_commissioned(void)
{
    fieldbus_start();
}

static void on_network_ready(void)
{
    led_status_set_network(true);
    web_server_start();
    sntp_sync_apply();   /* idempotent; per the configuration */
    if (s_mb_mux) {
        xSemaphoreTake(s_mb_mux, portMAX_DELAY);
        modbus_net_start_locked();
        xSemaphoreGive(s_mb_mux);
    }
}

static void on_eth_ready(void)
{
    s_eth_connected = true;
    on_network_ready();
}

static void on_ip_lost(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) s_eth_connected  = false;
    if (id == IP_EVENT_STA_LOST_IP) s_wifi_connected = false;

    /* The web server listens on INADDR_ANY and is shared by both interfaces.
       Stopping it whenever either one lost its address took the Ethernet UI
       down when WiFi dropped -- and nothing brought it back: the Ethernet
       link never produces a new Got-IP event, and the WiFi one was ignored
       while Ethernet counted as connected. Only stop once no usable
       interface is left. Same rule as in apps/full. */
    if (s_eth_connected || s_wifi_connected) return;

    led_status_set_network(false);
    web_server_stop();
}

static void on_matter_wifi_got_ip(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    s_wifi_connected = true;
    on_network_ready();          /* web_server_start() is idempotent */
}

static void on_wifi_ap(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    led_status_set_ap_mode(true);
    web_server_start();
}

void app_main(void)
{
    sntp_sync_init();   /* before anything can bring a network interface up */
    s_mb_mux = xSemaphoreCreateMutex();
    /* The sink only hands the value to the CHIP task, never touches the
       flash: the polling task can run on a PSRAM stack. */
    if (mb_tcp_master_init(on_modbus_value, true) != ESP_OK)
        ESP_LOGE(TAG, "Modbus poll table init failed");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(app_config_init());
    app_time_apply_tz();
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
    /* After di_init(), before matter_init(): a pad set to input joins the
       input path, and matter_init() creates the pads' endpoints by their
       mode. Not fatal -- a pad that cannot take its mode stays standard. */
    esp_err_t aux_ret = aux_io_init();
    if (aux_ret != ESP_OK)
        ESP_LOGE(TAG, "IO1/RXD/TXD pads: %s", esp_err_to_name(aux_ret));
    ESP_ERROR_CHECK(dout_init());
    ESP_ERROR_CHECK(led_init());
    ESP_ERROR_CHECK(buzzer_init());
    app_time_seed_from_rtc();   /* SNTP corrects it once the network is up */

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

    if (matter_ret == ESP_OK) fieldbus_start();   /* already commissioned: now */

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_ap, NULL);
    /* No wifi_config_set_eth_available_fn(): its "ETH only" button needs an
       eth-only mode this firmware does not have, and only rebooted back
       into the same AP. */

    /* Always init WiFi — creates a SoftAP in uncommissioned state so the web UI
     * (QR code / status) is reachable before Matter commissioning completes.
     * Once commissioned, never the open AP: a device paired over Ethernet, or
     * over Matter WiFi (whose credentials are not in wifi_cfg), counted as
     * unconfigured, and the AP -- open, taking an SSID from anyone in range --
     * came up for good or on every WiFi outage. matter.cpp also turns it off
     * when commissioning completes. The exception is the BOOT long press:
     * someone at the device asked for the setup AP. */
    if (matter_is_commissioned() && !wifi_config_reset_requested())
        wifi_config_disable_ap();
    wifi_config_init("Waveshare Matter (192.168.4.1)", NULL, NULL);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_hostname(sta, app_config_get()->device_name);
}
