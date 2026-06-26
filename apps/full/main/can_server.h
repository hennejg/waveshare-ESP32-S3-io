#pragma once
#include "esp_err.h"

/* Initialise and start the CAN bus server if enabled in app_config.
   Call after di_init(), dout_init(), led_init(), buzzer_init(). */
esp_err_t can_server_init(void);
