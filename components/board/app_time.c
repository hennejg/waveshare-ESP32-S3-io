#include "app_time.h"
#include "app_config.h"
#include "app_rtc.h"
#ifdef CONFIG_APP_SCRIPTING_ENABLE
void scripting_on_tz_change(void);
#endif

#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <esp_log.h>

#define TAG "app_time"

void app_time_apply_tz(void)
{
    const app_config_t *cfg = app_config_get();
    /* "UTC0" is the POSIX spelling of UTC; an empty config means "no local offset". */
    const char *tz = cfg->tz[0] ? cfg->tz : "UTC0";
    setenv("TZ", tz, 1);
    tzset();
#ifdef CONFIG_APP_SCRIPTING_ENABLE
    scripting_on_tz_change();   /* cron timers already armed carry the old offset */
#endif
    ESP_LOGI(TAG, "timezone applied: TZ=%s", tz);
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

bool app_time_seed_from_rtc(void)
{
    /* The battery-backed PCF85063 keeps the time across power loss once an
       SNTP sync (or a manual set) has written it. Non-fatal: a missing RTC
       or one without a valid time leaves the clock to SNTP. */
    if (rtc_dev_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC not available");
        return false;
    }
    struct tm rt;
    bool valid = false;
    if (rtc_dev_read(&rt, &valid) != ESP_OK || !valid) {
        ESP_LOGW(TAG, "RTC time not valid yet — waiting for SNTP");
        return false;
    }
    struct timeval tv = { .tv_sec = utc_tm_to_epoch(&rt), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System clock seeded from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
             rt.tm_hour, rt.tm_min, rt.tm_sec);
    return true;
}
