#pragma once
#include "esp_err.h"

/* Initialise the Modbus server if enabled in app_config. Call after di_init(),
   dout_init(), led_init(), buzzer_init(). Configuration changes take effect on
   the next reboot. */
esp_err_t mb_server_init(void);

/* Start the parts that need a working IP stack. Call once the network is up;
   calling it again is harmless. Without a TCP server configured it does
   nothing, which is why mb_server_init() alone is enough for plain RTU. */
esp_err_t mb_server_net_start(void);
