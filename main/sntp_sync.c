#include "sntp_sync.h"
#include "app_config.h"
#include "rtc.h"

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>

#define TAG "sntp"

static bool s_started = false;

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
}

void sntp_sync_apply(void)
{
    const app_config_t *cfg = app_config_get();

    if (s_started) {          /* tear down so a server/enable change applies cleanly */
        esp_netif_sntp_deinit();
        s_started = false;
    }
    if (!cfg->sntp.enable) {
        ESP_LOGI(TAG, "SNTP disabled by config");
        return;
    }

    const char *server = cfg->sntp.server[0] ? cfg->sntp.server : "pool.ntp.org";
    esp_sntp_config_t scfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    scfg.start   = true;
    scfg.sync_cb = on_time_synced;

    esp_err_t ret = esp_netif_sntp_init(&scfg);
    if (ret == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "SNTP started (server=%s)", server);
    } else {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
    }
}
