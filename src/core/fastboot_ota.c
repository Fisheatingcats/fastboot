#include "fastboot_ota.h"
#include "fastboot_uart.h"
#include "fastboot_watchdog.h"
#include "fastboot_memory_map.h"
#include <stdbool.h>
#include <string.h>

#define OTA_CHUNK_SIZE 1024u
#define OTA_PROGRESS_STEP 0x10000u

static uint8_t s_chunk[OTA_CHUNK_SIZE];

uint32_t fastboot_crc32(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t crc = seed ^ 0xFFFFFFFFu;

    for (size_t i = 0u; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool app_vector_bytes_are_valid(const uint8_t *data, size_t len)
{
    uint32_t initial_sp;
    uint32_t reset_handler;

    if (!data || len < 8u) {
        return false;
    }
    memcpy(&initial_sp, data, sizeof(initial_sp));
    memcpy(&reset_handler, data + 4u, sizeof(reset_handler));

    return initial_sp >= FASTBOOT_SRAM_BASE &&
           initial_sp <= FASTBOOT_SRAM_END &&
           reset_handler >= FASTBOOT_APP_FLASH_BASE &&
           reset_handler < FASTBOOT_APP_FLASH_END &&
           (reset_handler & 1u) != 0u;
}

static uint32_t header_crc32(const fastboot_ota_header_t *header)
{
    fastboot_ota_header_t tmp = *header;

    tmp.header_crc32 = 0u;
    return fastboot_crc32((const uint8_t *)&tmp, sizeof(tmp), 0u);
}

static fboot_status_t validate_header(const fastboot_ota_header_t *header)
{
    if (!header ||
        header->magic != FASTBOOT_OTA_MAGIC ||
        header->header_size != sizeof(*header) ||
        header->header_version != FASTBOOT_OTA_HEADER_VER ||
        header->image_offset < sizeof(*header) ||
        header->image_size < 8u ||
        header->image_size > FASTBOOT_APP_FLASH_SIZE ||
        header->load_addr != FASTBOOT_APP_FLASH_BASE) {
        return FB_ERR_FORMAT;
    }
    if (header_crc32(header) != header->header_crc32) {
        return FB_ERR_CRC;
    }
    return FB_OK;
}

fboot_status_t fastboot_ota_install(fastboot_ota_read_fn read_fn,
                                           void *ctx)
{
    fastboot_ota_header_t header;
    uint32_t image_crc = 0u;
    uint32_t remaining;
    uint32_t image_pos = 0u;
    uint32_t next_progress = OTA_PROGRESS_STEP;
    fboot_status_t rc;

    if (!read_fn) {
        return FB_ERR_PARAM;
    }
    if (read_fn(0u, (uint8_t *)&header, sizeof(header), ctx) != 0) {
        return FB_ERR_IO;
    }
    rc = validate_header(&header);
    if (rc != FB_OK) {
        return rc;
    }
    if (read_fn(header.image_offset, s_chunk, 8u, ctx) != 0 ||
        !app_vector_bytes_are_valid(s_chunk, 8u)) {
        return FB_ERR_FORMAT;
    }

    fastboot_uart_dec32("install image bytes", header.image_size);
    fastboot_uart_puts("[FB] erase app flash");
    rc = fastboot_iflash_erase_app();
    if (rc != FB_OK) {
        return rc;
    }
    fastboot_uart_puts("[FB] write app flash");

    remaining = header.image_size;
    while (remaining > 0u) {
        size_t chunk = remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining;
        uint32_t src = header.image_offset + image_pos;
        uint32_t dst = FASTBOOT_APP_FLASH_BASE + image_pos;

        if (read_fn(src, s_chunk, chunk, ctx) != 0) {
            return FB_ERR_IO;
        }

        image_crc = fastboot_crc32(s_chunk, chunk, image_crc);
        rc = fastboot_iflash_write(dst, s_chunk, chunk);
        if (rc != FB_OK) {
            return rc;
        }
        rc = fastboot_iflash_verify(dst, s_chunk, chunk);
        if (rc != FB_OK) {
            return rc;
        }

        image_pos += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
        boot_feed_watchdog();
        if (image_pos >= next_progress || remaining == 0u) {
            fastboot_uart_dec32("install written", image_pos);
            while (next_progress <= image_pos) {
                next_progress += OTA_PROGRESS_STEP;
            }
        }
    }

    if (image_crc != header.image_crc32) {
        return FB_ERR_CRC;
    }
    fastboot_uart_puts("[FB] stream crc ok");

    /* Second pass: read back from Flash and verify CRC independently.
     * This catches Flash write hardware faults that per-chunk memcmp might miss. */
    {
        uint32_t readback_crc = 0u;
        uint32_t rb_pos = 0u;
        uint32_t rb_remaining = header.image_size;

        fastboot_uart_puts("[FB] readback verify");
        while (rb_remaining > 0u) {
            size_t rb_chunk = rb_remaining > OTA_CHUNK_SIZE
                                  ? OTA_CHUNK_SIZE
                                  : (size_t)rb_remaining;
            const uint8_t *flash_ptr =
                (const uint8_t *)(uintptr_t)(FASTBOOT_APP_FLASH_BASE + rb_pos);
            readback_crc = fastboot_crc32(flash_ptr, rb_chunk, readback_crc);
            rb_pos += (uint32_t)rb_chunk;
            rb_remaining -= (uint32_t)rb_chunk;
        }
        if (readback_crc != header.image_crc32) {
            fastboot_uart_puts("[FB] readback CRC MISMATCH");
            return FB_ERR_VERIFY;
        }
    }

    fastboot_uart_puts("[FB] install verified ok");
    return FB_OK;
}
