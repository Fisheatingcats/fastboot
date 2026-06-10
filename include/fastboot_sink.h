#ifndef FASTBOOT_SINK_H
#define FASTBOOT_SINK_H

#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    fboot_status_t (*write)(void *ctx, uint32_t offset,
                             const uint8_t *data, size_t len);
    fboot_status_t (*write_start)(void *ctx, uint32_t offset,
                                   const uint8_t *data, size_t len);
    fboot_status_t (*poll)(void *ctx);
    bool (*busy)(void *ctx);
    void *ctx;
} fboot_sink_t;

#endif /* FASTBOOT_SINK_H */
