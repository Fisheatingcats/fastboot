#ifndef FASTBOOT_STAGING_H
#define FASTBOOT_STAGING_H

#include "fastboot_iflash_ops.h"
#include "fastboot_log.h"
#include "fastboot_staging_store.h"
#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_staging_store_t *store,
    const fastboot_iflash_ops_t *iflash,
    const fboot_log_t *log);

/* Legacy staging source type — prefer fastboot_staging_store_t for new code. */
typedef struct {
    fboot_status_t (*read)(void *ctx, uint32_t offset, uint8_t *data,
                           size_t len);
    fboot_status_t (*erase_sector)(void *ctx, uint32_t offset);
    void *ctx;
} fastboot_staging_source_t;

#endif /* FASTBOOT_STAGING_H */
