#include "sntp_sync.h"
#include "app_config.h"
#include "app_rtc.h"
#ifdef CONFIG_APP_SCRIPTING_ENABLE
void scripting_on_time_sync(void);
#endif

#include "esp_netif_sntp.h"
#include "esp_log.h"
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

void sntp_sync_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/* Called after each successful SNTP sync. The SNTP client has already stepped the
 * system clock (so NTP takes precedence over the RTC seed); mirror it to the
 * battery-backed RTC so the time persists across power loss / offline periods. */
static void on_time_synced(struct timeval *tv)
{
    time_t now = tv ? tv->tv_sec : time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (rtc_dev_set(&utc) == ESP_OK)
        ESP_LOGI(TAG, "SNTP synced; RTC updated");
    else
        ESP_LOGW(TAG, "SNTP synced; RTC update failed");

    /* Clock is now real: let the rule engine arm/re-arm cron from the corrected time. */
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_time_sync();
#endif
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
