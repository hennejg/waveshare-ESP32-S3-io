#pragma once
// Local-time / timezone helper.
//
// Applies the configured POSIX TZ string (app_config.tz) to the C library so that
// localtime_r / strftime — and, because QuickJS derives its local Date methods from
// newlib localtime(), the rule engine's cron schedules — all evaluate in local time
// with DST handled. An empty TZ config means UTC.

// Apply app_config's TZ via setenv("TZ", ...) + tzset(). Call at boot after the config
// is loaded, and again whenever the timezone setting changes. Safe to call repeatedly.
void app_time_apply_tz(void);
