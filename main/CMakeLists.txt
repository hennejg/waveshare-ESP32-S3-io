set(SRCS
    "main.c" "app_config.c" "app_mqtt.c" "auth.c" "button.c" "buzzer.c"
    "can_server.c" "di.c" "dout.c" "eth.c" "led.c" "mb_server.c" "web_server.c"
)

if(CONFIG_APP_MATTER_ENABLE)
    list(APPEND SRCS "matter.cpp")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp32-wifi-bootstrap nvs_flash spiffs esp_http_server esp_driver_gpio
             esp_netif esp_driver_i2c esp_driver_ledc esp_eth esp_driver_spi
             esp_driver_twai esp_wifi esp_coex
)

if(CONFIG_APP_MATTER_ENABLE)
    idf_component_optional_requires(PRIVATE esp_matter)
endif()

spiffs_create_partition_image(storage ${CMAKE_SOURCE_DIR}/www FLASH_IN_PROJECT)
