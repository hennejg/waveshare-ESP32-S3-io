#pragma once

typedef enum {
        WIFI_CONFIG_CONNECTED = 1,
        WIFI_CONFIG_DISCONNECTED = 2,
} wifi_config_event_t;

void wifi_config_init(const char *ssid_prefix, const char *password, void (*on_wifi_ready)());
void wifi_config_init2(const char *ssid_prefix, const char *password, void (*on_event)(wifi_config_event_t));

void wifi_config_reset();
void wifi_config_get(char **ssid, char **password);
void wifi_config_set(const char *ssid, const char *password);

void wifi_config_set_custom_html(char *html);

/* Register a one-shot callback invoked when the user taps "Use Ethernet only"
   in the provisioning portal. Use it to persist the preference before reboot. */
void wifi_config_set_eth_only_callback(void (*cb)(void));

/** Optional: register a predicate called at page-render time to decide whether
 *  to show the "ETH only — skip WiFi" option on the provisioning page.  When
 *  omitted the option is hidden.  Typically returns true when the Ethernet
 *  interface has an IP address. */
void wifi_config_set_eth_available_fn(bool (*fn)(void));

esp_err_t safe_set_auto_connect(bool enable);

/** Disable SoftAP mode entirely.  Must be called BEFORE wifi_config_init().
 *  Use when an out-of-band commissioning mechanism (e.g. Matter CHIPoBLE)
 *  handles WiFi credential delivery, so the captive-portal AP is unneeded and
 *  would consume DMA that BLE and the WiFi driver both need. */
void wifi_config_disable_ap(void);
