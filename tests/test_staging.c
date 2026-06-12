#include "fastboot_config.h"
#include "fastboot_staging.h"
#include "fastboot_ota.h"
#include <assert.h>
#include <string.h>

#define FAKE_FLASH_SIZE  (FASTBOOT_APP_FLASH_SIZE)

static uint8_t s_fake_ext[0x10000u];
static uint8_t s_fake_flash[FAKE_FLASH_SIZE];
static uint32_t s_ext_clear_count;

static fboot_status_t ext_read(void *ctx, uint32_t offset, uint8_t *data,
                               size_t len)
{
    (void)ctx;
    if (offset + len > sizeof(s_fake_ext)) {
        return FB_ERR_RANGE;
    }
    memcpy(data, &s_fake_ext[offset], len);
    return FB_OK;
}

static fboot_status_t ext_clear(void *ctx)
{
    (void)ctx;
    memset(s_fake_ext, 0xFFu, 4096u);
    ++s_ext_clear_count;
    return FB_OK;
}

static const fastboot_staging_store_t s_store = {
    NULL,
    ext_read,
    ext_clear,
    NULL,
};

static fboot_status_t iflash_erase(void *ctx)
{
    (void)ctx;
    memset(s_fake_flash, 0xFFu, sizeof(s_fake_flash));
    return FB_OK;
}

static fboot_status_t iflash_write(void *ctx, uint32_t addr,
                                   const uint8_t *data, size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FASTBOOT_APP_FLASH_BASE ||
        addr + len > FASTBOOT_APP_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FASTBOOT_APP_FLASH_BASE;
    memcpy(&s_fake_flash[offset], data, len);
    return FB_OK;
}

static fboot_status_t iflash_verify(void *ctx, uint32_t addr,
                                    const uint8_t *data, size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FASTBOOT_APP_FLASH_BASE ||
        addr + len > FASTBOOT_APP_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FASTBOOT_APP_FLASH_BASE;
    return memcmp(&s_fake_flash[offset], data, len) == 0 ? FB_OK : FB_ERR_VERIFY;
}

static fboot_status_t iflash_read(void *ctx, uint32_t addr, uint8_t *data,
                                  size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FASTBOOT_APP_FLASH_BASE ||
        addr + len > FASTBOOT_APP_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FASTBOOT_APP_FLASH_BASE;
    memcpy(data, &s_fake_flash[offset], len);
    return FB_OK;
}

static const fastboot_iflash_ops_t s_iflash = {
    iflash_erase,
    iflash_write,
    iflash_verify,
    iflash_read,
    NULL,
};

static void build_valid_vector(uint8_t *vec)
{
    uint32_t sp = FASTBOOT_SRAM_BASE + 0x1000u;
    uint32_t reset = FASTBOOT_APP_FLASH_BASE | 1u;

    memcpy(vec, &sp, 4u);
    memcpy(vec + 4u, &reset, 4u);
}

static uint32_t build_fwot(uint8_t *buf, const uint8_t *image,
                           uint32_t image_size)
{
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)buf;

    memset(buf, 0, sizeof(fastboot_ota_header_t));
    hdr->magic = FASTBOOT_OTA_MAGIC;
    hdr->header_size = sizeof(fastboot_ota_header_t);
    hdr->header_version = FASTBOOT_OTA_HEADER_VER;
    hdr->image_offset = sizeof(fastboot_ota_header_t);
    hdr->image_size = image_size;
    hdr->load_addr = FASTBOOT_APP_FLASH_BASE;
    hdr->image_crc32 = fastboot_crc32(image, image_size, 0u);
    hdr->header_crc32 = 0u;
    hdr->header_crc32 = fastboot_crc32(buf, sizeof(fastboot_ota_header_t), 0u);
    memcpy(&buf[sizeof(fastboot_ota_header_t)], image, image_size);
    return sizeof(fastboot_ota_header_t) + image_size;
}

static void test_no_update(void)
{
    fboot_status_t rc;

    memset(s_fake_ext, 0, sizeof(s_fake_ext));
    rc = fastboot_staging_install_if_pending(&s_store, &s_iflash, NULL);
    assert(rc == FB_NO_UPDATE);
}

static void test_valid_install(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    uint32_t pkg_len;
    fboot_status_t rc;

    build_valid_vector(image);
    pkg_len = build_fwot(pkg, image, 8u);

    memset(s_fake_ext, 0, sizeof(s_fake_ext));
    memcpy(s_fake_ext, pkg, pkg_len);
    memset(s_fake_flash, 0, sizeof(s_fake_flash));

    s_ext_clear_count = 0u;
    rc = fastboot_staging_install_if_pending(&s_store, &s_iflash, NULL);
    assert(rc == FB_OK);
    assert(s_ext_clear_count == 1u);
    assert(memcmp(s_fake_flash, image, 8u) == 0);
}

#define BASE_OFFSET 0x1000u

static uint8_t s_fake_ext_offset[0x20000u];

static fboot_status_t ext_read_offset(void *ctx, uint32_t offset,
                                      uint8_t *data, size_t len)
{
    (void)ctx;
    uint32_t abs_offset = BASE_OFFSET + offset;
    if (abs_offset + len > sizeof(s_fake_ext_offset)) {
        return FB_ERR_RANGE;
    }
    memcpy(data, &s_fake_ext_offset[abs_offset], len);
    return FB_OK;
}

static fboot_status_t ext_clear_offset(void *ctx)
{
    (void)ctx;
    memset(&s_fake_ext_offset[BASE_OFFSET], 0xFFu, 4096u);
    return FB_OK;
}

static const fastboot_staging_store_t s_store_offset = {
    NULL,
    ext_read_offset,
    ext_clear_offset,
    NULL,
};

static void test_nonzero_backend_base(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    uint32_t pkg_len;
    fboot_status_t rc;

    build_valid_vector(image);
    pkg_len = build_fwot(pkg, image, 8u);

    memset(s_fake_ext_offset, 0, sizeof(s_fake_ext_offset));
    memcpy(&s_fake_ext_offset[BASE_OFFSET], pkg, pkg_len);
    memset(s_fake_flash, 0, sizeof(s_fake_flash));

    rc = fastboot_staging_install_if_pending(&s_store_offset, &s_iflash, NULL);
    assert(rc == FB_OK);
    assert(memcmp(s_fake_flash, image, 8u) == 0);
}

static void test_null_params(void)
{
    fboot_status_t rc;

    rc = fastboot_staging_install_if_pending(NULL, &s_iflash, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_staging_install_if_pending(&s_store, NULL, NULL);
    assert(rc == FB_ERR_PARAM);
}

int main(void)
{
    test_no_update();
    test_valid_install();
    test_nonzero_backend_base();
    test_null_params();
    return 0;
}
