#pragma once
#include "esp_err.h"

/* Callback fired when the Ethernet interface obtains an IP address. */
typedef void (*eth_connected_cb_t)(void);

/* Initialise the W5500 SPI Ethernet interface.
   Call after esp_event_loop_create_default() (i.e. after wifi_config_init). */
esp_err_t eth_init(eth_connected_cb_t on_connected);
