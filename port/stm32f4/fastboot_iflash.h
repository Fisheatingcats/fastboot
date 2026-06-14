#ifndef FASTBOOT_IFLASH_H
#define FASTBOOT_IFLASH_H

#include "fastboot_flash.h"
#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

fboot_status_t fastboot_iflash_erase_range(uint32_t addr, uint32_t len);
fboot_status_t fastboot_iflash_write(uint32_t addr, const uint8_t *data,
                                     size_t len);
fboot_status_t fastboot_iflash_read(uint32_t addr, uint8_t *data, size_t len);
const fastboot_flash_area_t *fastboot_iflash_primary_area(void);

#endif /* FASTBOOT_IFLASH_H */
