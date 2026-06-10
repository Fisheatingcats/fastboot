#include "fastboot_staging.h"
#include "fastboot_ota.h"
#include "fastboot_w25q64.h"
#include "fastboot_memory_map.h"

static int read_ota_package(uint32_t offset, uint8_t *data, size_t len,
                            void *ctx)
{
    (void)ctx;
    return fastboot_w25q64_read(FASTBOOT_EXTFLASH_OTA_OFFSET + offset,
                                      data, len) == FB_OK
               ? 0
               : -1;
}

fboot_status_t fastboot_staging_install_if_pending(void)
{
    uint32_t magic = 0u;
    fboot_status_t rc = fastboot_w25q64_init();

    if (rc != FB_OK) {
        return rc;
    }
    rc = fastboot_w25q64_read(FASTBOOT_EXTFLASH_OTA_OFFSET,
                                    (uint8_t *)&magic, sizeof(magic));
    if (rc != FB_OK) {
        return rc;
    }
    if (magic != FASTBOOT_OTA_MAGIC) {
        return FB_NO_UPDATE;
    }

    rc = fastboot_ota_install(read_ota_package, NULL);
    if (rc == FB_OK) {
        (void)fastboot_w25q64_erase_sector(FASTBOOT_EXTFLASH_OTA_OFFSET);
    }
    return rc;
}
