# Modbus for both firmwares: RTU slave on RS-485 (esp-modbus), the board's own
# Modbus TCP server, and the TCP-to-RS-485 gateway. The Modbus TCP master
# (polling other devices) stays in apps/full: it reports to MQTT and the rule
# engine, which only that firmware has.
set(SRCS "mb_server.c" "mb_tcp_server.c" "mb_ident.c" "mb_pdu.c")
if(CONFIG_MB_RS485_MASTER)
    list(APPEND SRCS "mb_gateway.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES board esp_driver_uart esp_driver_gpio esp_netif esp_timer lwip vfs
             esp_app_format
)
