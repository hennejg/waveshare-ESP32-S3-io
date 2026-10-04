#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* GPIO43/44 are UART0 TX/RX.  CONFIG_ESP_CONSOLE_UART is set for both UART
   console choices — "Default: UART0" and "Custom", whose TX/RX default to
   exactly GPIO43/44 on the ESP32-S3. */
#if CONFIG_APP_AUX_DI_ENABLE && CONFIG_ESP_CONSOLE_UART
#error "APP_AUX_DI_ENABLE uses GPIO43/44, which a UART console drives as TX/RX. Select USB Serial/JTAG Controller as the console channel (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) or turn APP_AUX_DI_ENABLE off."
#endif

/* Initialise the digital inputs: the 8 isolated ones on GPIO4-11, plus the
   3 auxiliary ones on GPIO1/43/44 when APP_AUX_DI_ENABLE is set.
   Safe to call before WiFi/MQTT are up — monitoring starts immediately,
   publishing begins once the MQTT broker connects. */
esp_err_t di_init(void);

/* Returns the current logical state of input n (0 .. APP_CFG_DI_COUNT-1).

   The default sense is the raw pin level: GPIO high -> true, flipped per
   channel by di[n].invert.  On the isolated inputs the optocoupler pulls the
   pin low when the input is energised, so those usually want invert = true;
   the auxiliary inputs have no optocoupler and read the pin as it is. */
bool di_get(uint8_t n);

/* True for the optocoupler-isolated inputs (0-7), false for the auxiliary
   ones, which sit directly on a 3.3 V GPIO with no optocoupler and no series
   protection.  The web UI uses this to warn about them. */
bool di_is_isolated(uint8_t n);

/* Publish the current state of all inputs immediately. */
void di_publish_all(void);

/* Wire these into the MQTT callbacks in main.c. */
void di_on_mqtt_connected(void);
void di_on_mqtt_message(const char *topic, size_t topic_len,
                         const char *data,  size_t data_len);
