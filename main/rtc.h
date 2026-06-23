#pragma once
// PCF85063 battery-backed RTC (I2C 0x51) on the shared bus. Times are UTC.
//
// Used to keep wall-clock time across reboots/power loss: seed the system clock
// from the RTC at boot (if valid), and write SNTP-synced time back to the RTC.

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

// Probe and initialise the PCF85063 on the shared I2C bus. Does NOT modify the
// stored time (so it survives reboots). Returns an error if the chip isn't found.
esp_err_t rtc_dev_init(void);

// Read the RTC into *out (broken-down UTC time). *valid is set false when the chip's
// oscillator-stop flag is set — i.e. the time was lost (power loss / never set) and
// must not be trusted. Caller should fall back to SNTP in that case.
esp_err_t rtc_dev_read(struct tm *out, bool *valid);

// Write *t (broken-down UTC time) to the RTC. Clears the oscillator-stop flag, marking
// the stored time valid. tm_year must map to RTC years 1970-2069.
esp_err_t rtc_dev_set(const struct tm *t);
