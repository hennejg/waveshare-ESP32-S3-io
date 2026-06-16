#pragma once

/* Starts a background task that monitors GPIO0.
   Holding the button for BUTTON_HOLD_MS triggers a WiFi config reset + reboot. */
void button_init(void);

/* Register a one-shot callback that fires on the next short press (< 5 s).
   Pass NULL to cancel a pending registration. */
void button_on_short_press(void (*cb)(void));
