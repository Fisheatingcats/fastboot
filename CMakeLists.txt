cmake_minimum_required(VERSION 3.22)

project(fastboot C)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# FastBoot library
# Usage:
#   set(FASTBOOT_PORT "stm32f4" CACHE STRING "" FORCE)
#   set(FASTBOOT_CONFIG_DIR ".../config" CACHE PATH "" FORCE)
#   set(FASTBOOT_PLATFORM_INCLUDES ".../hal;.../cmsis" CACHE STRING "" FORCE)
#   add_subdirectory(FastBoot)
#   target_link_libraries(your_target PRIVATE fastboot)

set(FASTBOOT_CORE_SOURCES
    src/core/fastboot_ymodem.c
)

option(FASTBOOT_ENABLE_OTA
    "Build FWOT install and staging helpers. Requires a platform port."
    ON)
option(FASTBOOT_BUILD_TESTS
    "Build host-side FastBoot unit tests."
    OFF)

set(FASTBOOT_QUEUE_SOURCES
    src/queue/fastboot_queue.c
)

if(NOT DEFINED FASTBOOT_PORT)
    set(FASTBOOT_PORT "none" CACHE STRING "FastBoot port: stm32f4, none")
endif()
set_property(CACHE FASTBOOT_PORT PROPERTY STRINGS none stm32f4)

if(FASTBOOT_PORT STREQUAL "none" AND FASTBOOT_ENABLE_OTA)
    message(STATUS "FASTBOOT_PORT=none: disabling FASTBOOT_ENABLE_OTA")
    set(FASTBOOT_ENABLE_OTA OFF CACHE BOOL
        "Build FWOT install and staging helpers. Requires a platform port."
        FORCE)
endif()

if(FASTBOOT_ENABLE_OTA OR FASTBOOT_BUILD_TESTS)
    list(APPEND FASTBOOT_CORE_SOURCES
        src/core/fastboot_ota.c
        src/core/fastboot_staging.c
    )
endif()

set(FASTBOOT_PORT_SOURCES "")
if(FASTBOOT_PORT STREQUAL "stm32f4")
    list(APPEND FASTBOOT_PORT_SOURCES
        port/stm32f4/fastboot_stm32_runtime.c
        port/stm32f4/fastboot_uart.c
        port/stm32f4/fastboot_iflash.c
        port/stm32f4/fastboot_w25q64.c
        port/stm32f4/hal_msp_boot.c
    )
elseif(NOT FASTBOOT_PORT STREQUAL "none")
    message(FATAL_ERROR "Unsupported FASTBOOT_PORT: ${FASTBOOT_PORT}")
endif()

add_library(fastboot STATIC
    ${FASTBOOT_CORE_SOURCES}
    ${FASTBOOT_QUEUE_SOURCES}
    ${FASTBOOT_PORT_SOURCES}
)

target_compile_features(fastboot PUBLIC c_std_99)

target_include_directories(fastboot PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# A host-only configuration lets the protocol/queue code build and test
# without pulling in STM32 HAL or board headers. Embedded applications should
# provide FASTBOOT_CONFIG_DIR instead.
if(FASTBOOT_PORT STREQUAL "none" AND NOT DEFINED FASTBOOT_CONFIG_DIR)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fastboot_board_config.h" "\
#ifndef FASTBOOT_BOARD_CONFIG_H\n\
#define FASTBOOT_BOARD_CONFIG_H\n\
#define FASTBOOT_BOARD_CONFIG_INCLUDED 1\n\
#define FASTBOOT_CFG_ENABLE_YMODEM           1\n\
#define FASTBOOT_CFG_ENABLE_FWOT             1\n\
#define FASTBOOT_CFG_ENABLE_STAGING          1\n\
#define FASTBOOT_CFG_ENABLE_ASYNC_SINK       1\n\
#define FASTBOOT_CFG_ENABLE_READBACK_VERIFY  1\n\
#define FASTBOOT_CFG_ENABLE_LOG              0\n\
#define FASTBOOT_CFG_ENABLE_WATCHDOG         1\n\
#define FASTBOOT_CFG_QUEUE_DEPTH             8\n\
#define FASTBOOT_CFG_YMODEM_PACKET_SIZE      1024\n\
#define FASTBOOT_CFG_STAGING_CAPACITY        0x400000u\n\
#define FASTBOOT_EXTFLASH_OTA_SIZE           0x400000u\n\
#define FASTBOOT_EXTFLASH_OTA_OFFSET         0x000000u\n\
#define FASTBOOT_APP_FLASH_BASE              0x08008000u\n\
#define FASTBOOT_APP_FLASH_SIZE              0x00078000u\n\
#define FASTBOOT_APP_FLASH_END               0x08080000u\n\
#define FASTBOOT_SRAM_BASE                   0x20000000u\n\
#define FASTBOOT_SRAM_END                    0x20020000u\n\
#endif\n\
")
    target_include_directories(fastboot PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

if(FASTBOOT_PORT STREQUAL "stm32f4")
    target_include_directories(fastboot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f4
    )
    target_compile_definitions(fastboot PRIVATE
        USE_HAL_DRIVER
        STM32F411xE
    )
endif()

if(DEFINED FASTBOOT_CONFIG_DIR)
    target_include_directories(fastboot PUBLIC ${FASTBOOT_CONFIG_DIR})
endif()

if(DEFINED FASTBOOT_PLATFORM_INCLUDES)
    target_include_directories(fastboot PRIVATE ${FASTBOOT_PLATFORM_INCLUDES})
endif()

if(FASTBOOT_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(fastboot_test_queue tests/test_queue.c)
        target_link_libraries(fastboot_test_queue PRIVATE fastboot)
        add_test(NAME fastboot_queue COMMAND fastboot_test_queue)

        add_executable(fastboot_test_ymodem tests/test_ymodem.c)
        target_link_libraries(fastboot_test_ymodem PRIVATE fastboot)
        add_test(NAME fastboot_ymodem COMMAND fastboot_test_ymodem)

        add_executable(fastboot_test_crc tests/test_crc.c)
        target_link_libraries(fastboot_test_crc PRIVATE fastboot)
        add_test(NAME fastboot_crc COMMAND fastboot_test_crc)

        add_executable(fastboot_test_ota tests/test_ota.c)
        target_link_libraries(fastboot_test_ota PRIVATE fastboot)
        add_test(NAME fastboot_ota COMMAND fastboot_test_ota)

        add_executable(fastboot_test_staging tests/test_staging.c)
        target_link_libraries(fastboot_test_staging PRIVATE fastboot)
        add_test(NAME fastboot_staging COMMAND fastboot_test_staging)
    endif()
endif()
