#pragma once
#include "esp_err.h"

/* Initialise and start the Modbus RTU slave if enabled in app_config.
   Call after di_init(), dout_init(), led_init(), buzzer_init().
   Configuration changes take effect on the next reboot. */
esp_err_t mb_server_init(void);
