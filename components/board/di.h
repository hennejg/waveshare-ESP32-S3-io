#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialise the digital inputs: the 8 isolated ones on GPIO4-11. The 3
   auxiliary ones on GPIO1/44/43 are switched by aux_io.c (APP_AUX_DI_ENABLE).
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

/* False for an auxiliary channel left in its standard function; such a
   channel reads false and is not published.  Always true for 0-7. */
bool di_is_active(uint8_t n);

/* Puts auxiliary pin a (0..2) on or off the input path: pin configuration,
   interrupt, state, publishing. For aux_io.c, which owns the pad's function;
   a channel that becomes an input is read and published at once. */
void di_aux_input(uint8_t a, bool on);

/* Publish the current state of all inputs immediately. */
void di_publish_all(void);

/* Wire these into the MQTT callbacks in main.c. */
void di_on_mqtt_connected(void);
void di_on_mqtt_message(const char *topic, size_t topic_len,
                         const char *data,  size_t data_len);
