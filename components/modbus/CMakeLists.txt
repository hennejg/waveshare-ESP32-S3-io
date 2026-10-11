# Modbus for both firmwares: RTU slave on RS-485 (esp-modbus), the board's own
# Modbus TCP server, the RS-485 master with the TCP-to-RS-485 gateway, and the
# poll table (mb_tcp_master.c), which hands what it reads to the application.
set(SRCS "mb_server.c" "mb_tcp_server.c" "mb_ident.c" "mb_pdu.c" "mb_status_json.c")
if(CONFIG_MB_RS485_MASTER)
    list(APPEND SRCS "mb_gateway.c")
endif()
if(CONFIG_MB_RS485_MASTER OR CONFIG_MB_TCP_MASTER)
    list(APPEND SRCS "mb_tcp_master.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES board esp_driver_uart esp_driver_gpio esp_netif esp_timer json lwip vfs
             esp_app_format
)
