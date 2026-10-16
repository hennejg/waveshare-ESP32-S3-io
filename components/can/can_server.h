#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* What the firmware hands the CAN server. Every field may be left zero. */
typedef struct {
    /* Status bit 1 of the Basic-mode heartbeat (0x03 instead of 0x01): the
       firmware's uplink -- the MQTT broker in full, a Matter fabric in matter. */
    bool (*online)(void);
    /* A control frame (outputs, LED, buzzer) was acted on. */
    void (*on_command)(void);
    /* Run the worker task on a PSRAM stack. */
    bool psram_stack;
} can_server_hooks_t;

/* Initialise and start the CAN bus server if enabled in app_config.
   Call after di_init(), dout_init(), led_init(), buzzer_init(). */
esp_err_t can_server_init(const can_server_hooks_t *hooks);

/* Whether can_server_init() has the controller running. */
bool can_server_running(void);
