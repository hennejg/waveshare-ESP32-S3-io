#pragma once

/* Starts a background task that monitors GPIO0.
   Holding the button for BUTTON_HOLD_MS triggers a WiFi config reset + reboot. */
void button_init(void);
