#ifndef FASTBOOT_H
#define FASTBOOT_H

/**
 * @file fastboot.h
 * @brief FastBoot – lightweight OTA bootloader library.
 *
 * This is the single public header for the FastBoot library.
 * Include this in your application to access all bootloader functionality.
 *
 * Architecture:
 *   - src/core/    : Hardware-independent protocol and logic
 *   - src/queue/   : Async write queue for streaming to flash
 *   - port/        : Platform-specific implementations
 *
 * To port FastBoot to a new platform:
 *   1. Create a fastboot_config.h with memory layout constants
 *   2. Implement the fastboot_port_* functions for your hardware
 *   3. Implement fastboot_uart, fastboot_iflash, fastboot_w25q64
 */

#include "fastboot_config.h"
#include "fastboot_io.h"
#include "fastboot_port.h"
#include "fastboot_sink.h"
#include "fastboot_status.h"

/* Core modules (hardware independent) */
#include "fastboot_ota.h"
#include "fastboot_queue.h"
#include "fastboot_staging.h"
#include "fastboot_ymodem.h"

/* Port-specific modules (must be provided by port/) */
/* Include these in your application as needed:
 *   fastboot_uart.h
 *   fastboot_iflash.h
 *   fastboot_w25q64.h
 */

/**
 * @brief Version information.
 */
#define FASTBOOT_VERSION_MAJOR  1
#define FASTBOOT_VERSION_MINOR  0
#define FASTBOOT_VERSION_PATCH  0

/**
 * @brief Application vector table validity check.
 *
 * @param app_base  Base address of the application firmware.
 * @return true if the vector table looks valid (SP in SRAM, reset handler
 *         in app flash, Thumb bit set).
 */
static inline bool fastboot_app_vector_is_valid(uint32_t app_base)
{
    uint32_t initial_sp    = *(const uint32_t *)app_base;
    uint32_t reset_handler = *(const uint32_t *)(app_base + 4u);

    if (initial_sp < FASTBOOT_SRAM_BASE || initial_sp > FASTBOOT_SRAM_END) {
        return false;
    }
    if (reset_handler < FASTBOOT_APP_FLASH_BASE ||
        reset_handler >= FASTBOOT_APP_FLASH_END) {
        return false;
    }
    if ((reset_handler & 0x1u) == 0u) {
        return false;
    }
    return true;
}

#endif /* FASTBOOT_H */
