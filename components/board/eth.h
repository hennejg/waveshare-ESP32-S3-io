#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Callback fired when the Ethernet interface obtains an IP address. */
typedef void (*eth_connected_cb_t)(void);

/* Initialise the W5500 SPI Ethernet interface.
   Call after esp_event_loop_create_default(). */
esp_err_t eth_init(eth_connected_cb_t on_connected);

/* True from the moment ETHERNET_EVENT_CONNECTED fires (physical link up).
   Available immediately after eth_init() — does not require an IP address. */
bool eth_link_is_up(void);
