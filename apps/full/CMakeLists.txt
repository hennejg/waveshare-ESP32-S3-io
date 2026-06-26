cmake_minimum_required(VERSION 3.16)

# Shared components: board HAL + quickjs/scripting (repo root/components/).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(waveshare_esp32s3_full)

idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
