#include "fastboot_staging.h"
#include "fastboot_config.h"
#include "fastboot_ota.h"

static int staging_read_fn(uint32_t offset, uint8_t *data, size_t len,
                           void *ctx)
{
    const fastboot_staging_store_t *store =
        (const fastboot_staging_store_t *)ctx;

    return store->read(store->ctx, offset, data, len) == FB_OK ? 0 : -1;
}

fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_staging_store_t *store,
    const fastboot_iflash_ops_t *iflash,
    const fboot_log_t *log)
{
    uint32_t magic = 0u;
    fboot_status_t rc;

    if (!store || !store->read || !iflash) {
        return FB_ERR_PARAM;
    }

    rc = store->read(store->ctx, 0u, (uint8_t *)&magic, sizeof(magic));
    if (rc != FB_OK) {
        return rc;
    }
    if (magic != FASTBOOT_OTA_MAGIC) {
        return FB_NO_UPDATE;
    }

    rc = fastboot_ota_install(staging_read_fn, (void *)store, iflash, log);
    if (rc == FB_OK && store->clear) {
        (void)store->clear(store->ctx);
    }
    return rc;
}
