#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialise the WS2812 LED on GPIO38. Starts off. */
esp_err_t led_init(void);

/* Set LED colour directly (bypasses sequence queue — avoid during active sequences). */
esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Force-set LED colour, bypassing IO/Status mode guard.
   For use by system functions (e.g. authentication blink). */
void led_force_set(uint8_t r, uint8_t g, uint8_t b);

/* Status mode API — only active when led_mode == LED_MODE_STATUS.
   Call from network / MQTT event handlers in main.c. */
void led_status_set_network(bool up);   /* true once WiFi or ETH gets an IP */
void led_status_set_mqtt(bool up);      /* true on broker connect */
void led_status_flash_rx(void);         /* 100 ms red flash on MQTT RX */
void led_status_flash_tx(void);         /* 100 ms blue flash on MQTT TX */

/* Wire into MQTT callbacks in main.c.
 *
 * Accepted payloads on <prefix>/led/set:
 *   "#RRGGBB"                               — instant set, no timing
 *   {"color":"#RRGGBB","duration":ms}        — single timed step
 *   [{"color":"#RRGGBB","duration":ms}, ...] — sequence, up to 16 steps
 *
 * State published on <prefix>/led as "#rrggbb". */
void led_on_mqtt_connected(void);
void led_on_mqtt_message(const char *topic, size_t tlen,
                          const char *data,  size_t dlen);
