#pragma once
// Shared I2C master bus (SDA=GPIO42, SCL=GPIO41) used by the on-board peripherals
// that sit on it — the TCA9554 output expander (dout) and the PCF85063 RTC (rtc).
// init is idempotent, so callers don't need to coordinate order.

#include "esp_err.h"
#include "driver/i2c_master.h"

// Create the shared bus if it doesn't exist yet. Safe to call from multiple modules.
esp_err_t i2c_bus_init(void);

// The shared bus handle, or NULL if i2c_bus_init() hasn't succeeded yet.
i2c_master_bus_handle_t i2c_bus_handle(void);

// Held around every transfer on the bus, and around a bus reset with the
// transfer it repeats. The driver serialises transfers itself, but not
// i2c_master_bus_reset(): a reset by one device's code cut into the other
// device's transfer under way. Recursive.
void i2c_bus_lock(void);
void i2c_bus_unlock(void);
