set(SRCS
    "main.c" "matter.cpp" "web_server.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp32-wifi-bootstrap nvs_flash esp_http_server
             esp_netif esp_wifi esp_coex board lwip
    PRIV_REQUIRES esp_matter
)
