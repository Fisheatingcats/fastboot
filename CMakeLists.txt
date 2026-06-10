cmake_minimum_required(VERSION 3.22)

# FastBoot library
# Usage:
#   add_subdirectory(FastBoot)
#   target_link_libraries(your_target PRIVATE fastboot)

project(fastboot C)

# ── Library sources ──────────────────────────────────────────────────────────

set(FASTBOOT_CORE_SOURCES
    src/core/fastboot_ymodem.c
    src/core/fastboot_ota.c
    src/core/fastboot_staging.c
)

set(FASTBOOT_QUEUE_SOURCES
    src/queue/fastboot_queue.c
)

# Port sources are NOT added by default.
# The user must provide port sources or use a preset:
if(NOT DEFINED FASTBOOT_PORT)
    set(FASTBOOT_PORT "none" CACHE STRING "FastBoot port: stm32f4, none")
endif()

set(FASTBOOT_PORT_SOURCES "")
if(FASTBOOT_PORT STREQUAL "stm32f4")
    list(APPEND FASTBOOT_PORT_SOURCES
        port/stm32f4/fastboot_port.c
        port/stm32f4/fastboot_uart.c
        port/stm32f4/fastboot_iflash.c
        port/stm32f4/fastboot_w25q64.c
        port/stm32f4/hal_msp_boot.c
    )
endif()

# ── Library target ───────────────────────────────────────────────────────────

add_library(fastboot STATIC
    ${FASTBOOT_CORE_SOURCES}
    ${FASTBOOT_QUEUE_SOURCES}
    ${FASTBOOT_PORT_SOURCES}
)

target_include_directories(fastboot PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(FASTBOOT_PORT STREQUAL "stm32f4")
    target_include_directories(fastboot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f4
    )
    target_compile_definitions(fastboot PRIVATE
        USE_HAL_DRIVER
        STM32F411xE
    )
endif()

# User must provide fastboot_config.h with memory layout constants.
# Add the directory containing it to the include path:
if(DEFINED FASTBOOT_CONFIG_DIR)
    target_include_directories(fastboot PUBLIC ${FASTBOOT_CONFIG_DIR})
endif()
