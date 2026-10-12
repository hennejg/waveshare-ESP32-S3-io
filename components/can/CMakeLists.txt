# CAN bus for both firmwares: Basic mode (11-bit frames) and NMEA 2000.
idf_component_register(
    SRCS "can_server.c"
    INCLUDE_DIRS "."
    REQUIRES board esp_driver_twai esp_driver_gpio
)
