#include "sntp_sync.h"
#include "app_config.h"
#include "app_rtc.h"
#ifdef CONFIG_APP_SCRIPTING_ENABLE
void scripting_on_time_sync(void);
#endif

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define TAG "sntp"

static bool s_started = false;
/* sntp_sync_apply() tears SNTP down and starts it again, and is called from
   the network-up path and from a configuration save on the web server's task:
   unguarded, the two could init twice or deinit under a running init. */
static SemaphoreHandle_t s_lock;

/* Mirrors the system clock, which SNTP has just stepped (so NTP takes
 * precedence over the RTC seed), to the battery-backed RTC so the time
 * persists across power loss / offline periods. Runs on the esp_timer task:
 * the SNTP callback itself runs on lwIP's task, and an I2C transfer there --
 * up to 50 ms, longer while the output expander holds the bus for a
 * recovery -- stalled every socket on the board meanwhile. */
static esp_timer_handle_t s_rtc_timer;

static void write_rtc(void *arg)
{
    (void)arg;
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (rtc_dev_set(&utc) == ESP_OK)
        ESP_LOGI(TAG, "SNTP synced; RTC updated");
    else
        ESP_LOGW(TAG, "SNTP synced; RTC update failed");
}

/* Called after each successful SNTP sync, on lwIP's task: nothing here may
   block. */
static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    /* INVALID_STATE: a write is already on its way, and it reads the clock
       when it runs. */
    esp_err_t r = s_rtc_timer ? esp_timer_start_once(s_rtc_timer, 0) : ESP_ERR_INVALID_STATE;
    if (r != ESP_OK && !(s_rtc_timer && r == ESP_ERR_INVALID_STATE))
        ESP_LOGW(TAG, "SNTP synced; RTC update not scheduled");

    /* Clock is now real: let the rule engine arm/re-arm cron from the corrected time. */
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_time_sync();
#endif
}

void sntp_sync_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_rtc_timer) {
        const esp_timer_create_args_t a = { .callback = write_rtc, .name = "sntp_rtc" };
        if (esp_timer_create(&a, &s_rtc_timer) != ESP_OK) s_rtc_timer = NULL;
    }
}

/* The server SNTP runs with while s_started. Every configuration save calls
   sntp_sync_apply(), renaming an input included; a restart then drops a
   sync that is under way and sends a fresh request to the server for
   nothing. */
static char s_server[APP_CFG_SNTP_SERVER_LEN];

static void apply_locked(void)
{
    const app_config_t *cfg = app_config_get();
    const char *server = cfg->sntp.server[0] ? cfg->sntp.server : "pool.ntp.org";

    if (s_started && cfg->sntp.enable && !strcmp(server, s_server))
        return;               /* running as configured */
    if (s_started) {          /* tear down so a server/enable change applies cleanly */
        esp_netif_sntp_deinit();
        s_started = false;
    }
    if (!cfg->sntp.enable) {
        ESP_LOGI(TAG, "SNTP disabled by config");
        return;
    }

    esp_sntp_config_t scfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    scfg.start   = true;
    scfg.sync_cb = on_time_synced;

    esp_err_t ret = esp_netif_sntp_init(&scfg);
    if (ret == ESP_OK) {
        s_started = true;
        strlcpy(s_server, server, sizeof(s_server));
        ESP_LOGI(TAG, "SNTP started (server=%s)", server);
    } else {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
    }
}

void sntp_sync_apply(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_locked();
    if (s_lock) xSemaphoreGive(s_lock);
}
