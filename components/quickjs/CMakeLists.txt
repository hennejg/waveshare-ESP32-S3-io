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

# The submodule is pinned to pristine upstream; ESP-IDF/newlib adaptations live as
# patches here and are applied (idempotently) at configure time. Currently:
#   0001-getTimezoneOffset-portable.patch — use the portable gmtime/localtime path
#   where struct tm has no tm_gmtoff (newlib), instead of the glibc tm_gmtoff branch.
file(GLOB QJS_PATCHES "${CMAKE_CURRENT_LIST_DIR}/patches/*.patch")
list(SORT QJS_PATCHES)
foreach(patch ${QJS_PATCHES})
    # Skip if already applied: a reverse-apply check succeeds only when it's in place.
    execute_process(
        COMMAND git apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${QJS}"
        RESULT_VARIABLE _qjs_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _qjs_applied EQUAL 0)
        execute_process(
            COMMAND git apply "${patch}"
            WORKING_DIRECTORY "${QJS}"
            RESULT_VARIABLE _qjs_rc)
        if(NOT _qjs_rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply QuickJS patch: ${patch}")
        endif()
        message(STATUS "Applied QuickJS patch: ${patch}")
    endif()
endforeach()

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
# -Wno-error: this is vendored upstream code — don't fail our build on its warnings,
#   which vary by toolchain (e.g. -Wmaybe-uninitialized false positives under the newer
#   GCC in CI). Our own components are still built with -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
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
