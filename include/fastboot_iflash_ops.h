#ifndef FASTBOOT_IFLASH_OPS_H
#define FASTBOOT_IFLASH_OPS_H

#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    fboot_status_t (*erase_app)(void *ctx);
    fboot_status_t (*write)(void *ctx, uint32_t addr, const uint8_t *data,
                            size_t len);
    fboot_status_t (*verify)(void *ctx, uint32_t addr, const uint8_t *data,
                             size_t len);
    fboot_status_t (*read)(void *ctx, uint32_t addr, uint8_t *data,
                           size_t len);
    void *ctx;
} fastboot_iflash_ops_t;

#endif /* FASTBOOT_IFLASH_OPS_H */
