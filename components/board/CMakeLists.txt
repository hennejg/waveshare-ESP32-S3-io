set(SRCS
    "di.c" "dout.c" "eth.c" "led.c" "button.c" "buzzer.c"
    "app_config.c" "auth.c" "rtc.c" "i2c_bus.c" "aux_io.c" "web_common.c" "app_time.c"
)

if(CONFIG_APP_SNTP_ENABLE)
    list(APPEND SRCS "sntp_sync.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_uart esp_adc
             esp_eth esp_driver_spi esp_netif esp32-wifi-bootstrap lwip
             esp_timer json mbedtls esp_http_server
)
