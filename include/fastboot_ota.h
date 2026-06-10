#ifndef FASTBOOT_OTA_H
#define FASTBOOT_OTA_H

#include "fastboot_iflash.h"
#include <stddef.h>
#include <stdint.h>

#define FASTBOOT_OTA_MAGIC        0x544F5746u /* FWOT */
#define FASTBOOT_OTA_HEADER_VER   1u

typedef int (*fastboot_ota_read_fn)(uint32_t offset, uint8_t *data,
                                          size_t len, void *ctx);

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t header_size;
    uint16_t header_version;
    uint32_t flags;
    uint32_t image_offset;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t load_addr;
    uint32_t image_version;
    uint32_t reserved[7];
    uint32_t header_crc32;
} fastboot_ota_header_t;
#pragma pack(pop)

fboot_status_t fastboot_ota_install(fastboot_ota_read_fn read_fn,
                                           void *ctx);
uint32_t fastboot_crc32(const uint8_t *data, size_t len,
                              uint32_t seed);

#endif /* FASTBOOT_OTA_H */
