set(SRCS
    "di.c" "dout.c" "eth.c" "led.c" "button.c" "buzzer.c"
    "app_config.c" "auth.c" "rtc.c" "i2c_bus.c"
)

if(CONFIG_APP_SNTP_ENABLE)
    list(APPEND SRCS "sntp_sync.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_ledc
             esp_eth esp_driver_spi esp_netif esp32-wifi-bootstrap lwip
             esp_timer json mbedtls
)
