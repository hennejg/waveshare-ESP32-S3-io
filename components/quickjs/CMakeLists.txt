cmake_minimum_required(VERSION 3.16)

# QuickJS source lives in extern/quickjs (git submodule: bellard/quickjs).
# Set up once:
#   git submodule add https://github.com/bellard/quickjs.git components/quickjs/extern/quickjs
#   git submodule update --init components/quickjs/extern/quickjs

set(QJS "${CMAKE_CURRENT_LIST_DIR}/extern/quickjs")

if(NOT EXISTS "${QJS}/quickjs.c")
    message(FATAL_ERROR
        "QuickJS sources not found at ${QJS}.\n"
        "Run: git submodule add https://github.com/bellard/quickjs.git "
        "components/quickjs/extern/quickjs")
endif()

set(QJS_SRCS
    "${QJS}/quickjs.c"
    "${QJS}/libregexp.c"
    "${QJS}/libunicode.c"
    "${QJS}/cutils.c"
)

# dtoa.c (float↔string conversion) introduced in 2025+ builds; compile if present.
if(EXISTS "${QJS}/dtoa.c")
    list(APPEND QJS_SRCS "${QJS}/dtoa.c")
endif()

# libbf.c provides BigInt/BigFloat support; required when CONFIG_BIGNUM is set.
set(QJS_DEFS _GNU_SOURCE)
if(EXISTS "${QJS}/libbf.c")
    list(APPEND QJS_SRCS "${QJS}/libbf.c")
    list(APPEND QJS_DEFS CONFIG_BIGNUM)
endif()

# Version string — QuickJS references CONFIG_VERSION at compile time.
# Read it from version.h if present, otherwise use a placeholder.
if(EXISTS "${QJS}/version.h")
    # The Makefile generates version.h; if checked in, use it.
    list(APPEND QJS_DEFS "CONFIG_VERSION=\"source\"")
else()
    list(APPEND QJS_DEFS "CONFIG_VERSION=\"embedded\"")
endif()

idf_component_register(
    SRCS        ${QJS_SRCS}
    INCLUDE_DIRS "${QJS}"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ${QJS_DEFS})

# Force-include the ESP32 portability shims into every QuickJS source file.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${CMAKE_CURRENT_LIST_DIR}/compat/esp32_compat.h"
)

# Suppress upstream warnings so our build output stays clean.
# -Wno-format: uint32_t is 'unsigned long' on Xtensa, causing format mismatches.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-incompatible-pointer-types
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -Wno-type-limits
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-format-truncation
    -Wno-implicit-function-declaration
)
