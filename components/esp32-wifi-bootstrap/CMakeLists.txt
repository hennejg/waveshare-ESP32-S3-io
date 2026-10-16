idf_component_register(
    SRCS "src/wifi_config.c" "src/form_urlencoded.c" "src/wifi_config_util.c"
    INCLUDE_DIRS "include" "content"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer http_parser
)

# A lost WiFi link is retried by the network monitor, never answered with a
# reboot: the board keeps its outputs, Modbus, CAN and rules running, and may
# well be reachable over Ethernet meanwhile. component.mk set this for the old
# make build; the CMake build never did, so every outage restarted the device.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_CONFIG_NO_RESTART)
