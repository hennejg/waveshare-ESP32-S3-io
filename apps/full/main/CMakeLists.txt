set(SRCS
    "main.c" "app_mqtt.c" "app_time.c" "web_server.c"
    "can_server.c" "mb_server.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp32-wifi-bootstrap nvs_flash spiffs esp_http_server
             esp_netif esp_driver_twai esp_wifi esp_coex board scripting lwip
)

set(_WWW ${CMAKE_SOURCE_DIR}/../../www)
foreach(_f index.html qrcode.min.js
           codemirror.min.js codemirror.min.css codemirror-javascript.min.js)
    add_custom_command(
        OUTPUT  ${_WWW}/${_f}.gz
        COMMAND gzip -k -f ${_WWW}/${_f}
        DEPENDS ${_WWW}/${_f}
        COMMENT "Compressing www/${_f}"
    )
endforeach()

add_custom_target(www_compress DEPENDS
    ${_WWW}/index.html.gz
    ${_WWW}/qrcode.min.js.gz
    ${_WWW}/codemirror.min.js.gz
    ${_WWW}/codemirror.min.css.gz
    ${_WWW}/codemirror-javascript.min.js.gz
)

spiffs_create_partition_image(storage ${CMAKE_SOURCE_DIR}/../../www FLASH_IN_PROJECT DEPENDS www_compress)
