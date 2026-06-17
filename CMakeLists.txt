cmake_minimum_required(VERSION 3.16)

# esp-matter paths — must be set before project() / IDF cmake inclusion
get_filename_component(ESP_MATTER_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/esp-matter REALPATH)
set(ENV{ESP_MATTER_PATH} ${ESP_MATTER_PATH})

set(MATTER_SDK_PATH
    ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# Component search paths: chip stack + esp_matter component only.
# We deliberately exclude device_hal (DevKit LED/button HAL) because we
# have our own hardware abstraction for the Waveshare board.
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Merge Matter sdkconfig defaults (handles c2 quirks, sets CHIP_HAVE_CONFIG_H)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)

project(waveshare_esp32s3)

idf_build_set_property(CXX_COMPILE_OPTIONS
    "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS
    "-Wno-format-nonliteral;-Wno-format-security" APPEND)
