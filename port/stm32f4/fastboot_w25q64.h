#ifndef FASTBOOT_W25Q64_H
#define FASTBOOT_W25Q64_H

#include "fastboot_sink.h"
#include "fastboot_staging.h"
#include "fastboot_staging_store.h"
#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

fboot_status_t fastboot_w25q64_init(void);
uint32_t fastboot_w25q64_jedec_id(void);
fboot_status_t fastboot_w25q64_read(uint32_t offset, uint8_t *data,
                                           size_t len);
fboot_status_t fastboot_w25q64_write(uint32_t offset,
                                            const uint8_t *data, size_t len);
fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset);
fboot_status_t fastboot_w25q64_erase_range(uint32_t offset,
                                                  size_t len);
const fboot_sink_t *fastboot_w25q64_ota_sink(void);
const fastboot_staging_source_t *fastboot_w25q64_staging_source(void);
const fastboot_staging_store_t *fastboot_w25q64_staging_store(void);

#endif /* FASTBOOT_W25Q64_H */
