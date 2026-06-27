set(SRCS
    "main.c" "matter.cpp" "web_server.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp32-wifi-bootstrap nvs_flash spiffs esp_http_server
             esp_netif esp_wifi esp_coex board lwip mbedtls esp_app_format
    PRIV_REQUIRES esp_matter
)

set(_WWW  ${CMAKE_SOURCE_DIR}/../../www)
set(_MWWW ${CMAKE_CURRENT_SOURCE_DIR}/../www)

add_custom_command(
    OUTPUT  ${_MWWW}/index.html.gz
    COMMAND gzip -k -f ${_MWWW}/index.html
    DEPENDS ${_MWWW}/index.html
    COMMENT "Compressing matter/www/index.html"
)

add_custom_command(
    OUTPUT  ${_WWW}/qrcode.min.js.gz
    COMMAND gzip -k -f ${_WWW}/qrcode.min.js
    DEPENDS ${_WWW}/qrcode.min.js
    COMMENT "Compressing www/qrcode.min.js (for matter)"
)

add_custom_command(
    OUTPUT  ${_MWWW}/qrcode.min.js ${_MWWW}/qrcode.min.js.gz ${_MWWW}/favicon.svg
    COMMAND ${CMAKE_COMMAND} -E copy ${_WWW}/qrcode.min.js    ${_MWWW}/qrcode.min.js
    COMMAND ${CMAKE_COMMAND} -E copy ${_WWW}/qrcode.min.js.gz ${_MWWW}/qrcode.min.js.gz
    COMMAND ${CMAKE_COMMAND} -E copy ${_WWW}/favicon.svg      ${_MWWW}/favicon.svg
    DEPENDS ${_WWW}/qrcode.min.js.gz ${_WWW}/favicon.svg
    COMMENT "Copying shared web assets to matter/www/"
)

add_custom_target(matter_www_prepare DEPENDS
    ${_MWWW}/index.html.gz
    ${_MWWW}/qrcode.min.js.gz
    ${_MWWW}/favicon.svg
)

spiffs_create_partition_image(storage ${_MWWW} FLASH_IN_PROJECT DEPENDS matter_www_prepare)
