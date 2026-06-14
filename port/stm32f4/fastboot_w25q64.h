#ifndef FASTBOOT_W25Q64_H
#define FASTBOOT_W25Q64_H

#include "fastboot_flash.h"
#include "fastboot_status.h"
#include "fastboot_writer.h"
#include <stddef.h>
#include <stdint.h>

fboot_status_t fastboot_w25q64_init(void);
uint32_t fastboot_w25q64_jedec_id(void);
fboot_status_t fastboot_w25q64_read(uint32_t offset, uint8_t *data,
                                    size_t len);
fboot_status_t fastboot_w25q64_write(uint32_t offset, const uint8_t *data,
                                     size_t len);
fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset);
fboot_status_t fastboot_w25q64_erase(uint32_t offset, uint32_t size);
uint32_t fastboot_w25q64_best_erase_size(uint32_t offset,
                                         uint32_t remaining);
fboot_status_t fastboot_w25q64_erase_range(uint32_t offset, size_t len);
const fastboot_writer_t *fastboot_w25q64_staging_writer(void);
const fastboot_flash_area_t *fastboot_w25q64_staging_area(void);

typedef struct {
    uint32_t erase_count;
    uint32_t erase_us;
    uint32_t program_count;
    uint32_t program_us;
    uint32_t busy_poll_count;
} w25q_perf_t;

void fastboot_w25q64_perf_reset(void);
const w25q_perf_t *fastboot_w25q64_perf(void);

#endif /* FASTBOOT_W25Q64_H */
