#ifndef FASTBOOT_CONFIG_H
#define FASTBOOT_CONFIG_H

#include <stdint.h>

/**
 * @file fastboot_config.h
 * @brief Platform-specific configuration for FastBoot.
 *
 * The port must define these constants before including fastboot headers.
 * Provide a fastboot_config.h in your project that sets these values.
 */

#ifndef FASTBOOT_EXTFLASH_OTA_SIZE
#error "Define FASTBOOT_EXTFLASH_OTA_SIZE in your fastboot_config.h"
#endif

#ifndef FASTBOOT_APP_FLASH_BASE
#error "Define FASTBOOT_APP_FLASH_BASE in your fastboot_config.h"
#endif

#ifndef FASTBOOT_APP_FLASH_END
#error "Define FASTBOOT_APP_FLASH_END in your fastboot_config.h"
#endif

#ifndef FASTBOOT_SRAM_BASE
#error "Define FASTBOOT_SRAM_BASE in your fastboot_config.h"
#endif

#ifndef FASTBOOT_SRAM_END
#error "Define FASTBOOT_SRAM_END in your fastboot_config.h"
#endif

#ifndef FASTBOOT_EXTFLASH_OTA_OFFSET
#define FASTBOOT_EXTFLASH_OTA_OFFSET  0x000000u
#endif

#endif /* FASTBOOT_CONFIG_H */
