#ifndef FASTBOOT_STAGING_STORE_H
#define FASTBOOT_STAGING_STORE_H

#include "fastboot_sink.h"
#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const fboot_sink_t *sink;
    fboot_status_t (*read)(void *ctx, uint32_t offset, uint8_t *data,
                           size_t len);
    fboot_status_t (*clear)(void *ctx);
    void *ctx;
} fastboot_staging_store_t;

static inline const fboot_sink_t *
fastboot_staging_store_sink(const fastboot_staging_store_t *store)
{
    return store ? store->sink : NULL;
}

#endif /* FASTBOOT_STAGING_STORE_H */
