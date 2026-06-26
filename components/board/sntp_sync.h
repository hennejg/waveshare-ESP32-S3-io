#pragma once
// SNTP time synchronisation.
//
// When enabled (default), syncs the system clock from an NTP server (default
// pool.ntp.org) — NTP takes precedence over the RTC. Each successful sync is mirrored
// to the battery-backed RTC so the time survives power loss / offline periods.
// Server and enable/disable come from app_config.

// Start SNTP per the current config, or reconfigure if already running (call when the
// network comes up, and again after the SNTP settings change). Safe to call repeatedly.
void sntp_sync_apply(void);
