idf_component_register(
    SRCS "main.c" "app_config.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-wifi-bootstrap nvs_flash spiffs esp_http_server
)

spiffs_create_partition_image(storage ${CMAKE_SOURCE_DIR}/www FLASH_IN_PROJECT)
