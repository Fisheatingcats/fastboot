#ifndef FASTBOOT_IFLASH_H
#define FASTBOOT_IFLASH_H

#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

fboot_status_t fastboot_iflash_erase_app(void);
fboot_status_t fastboot_iflash_write(uint32_t addr, const uint8_t *data,
                                            size_t len);
fboot_status_t fastboot_iflash_verify(uint32_t addr,
                                             const uint8_t *data, size_t len);

#endif /* FASTBOOT_IFLASH_H */
