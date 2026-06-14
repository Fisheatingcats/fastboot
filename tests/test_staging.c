#include "fastboot_config.h"
#include "fastboot_staging.h"
#include "fastboot_ota.h"
#include <assert.h>
#include <string.h>

#define FAKE_FLASH_SIZE  (FASTBOOT_APP_FLASH_SIZE)

static uint8_t s_fake_ext[0x10000u];
static uint8_t s_fake_flash[FAKE_FLASH_SIZE];
static uint32_t s_ext_clear_count;

static uint32_t runtime_tick_ms(void *ctx)
{
    (void)ctx;
    return 0u;
}

static const fastboot_runtime_t s_runtime = {
    runtime_tick_ms,
    NULL,
    NULL,
};

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

static fboot_status_t ext_write(void *ctx, uint32_t offset,
                                const uint8_t *data, size_t len)
{
    (void)ctx;
    if (offset + len > sizeof(s_fake_ext)) {
        return FB_ERR_RANGE;
    }
    memcpy(&s_fake_ext[offset], data, len);
    return FB_OK;
}

static fboot_status_t ext_erase(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    if (offset + len > sizeof(s_fake_ext)) {
        return FB_ERR_RANGE;
    }
    memset(&s_fake_ext[offset], 0xFFu, len);
    ++s_ext_clear_count;
    return FB_OK;
}

static const fastboot_flash_ops_t s_ext_ops = {
    ext_read,
    ext_write,
    ext_erase,
};

static fastboot_flash_area_t s_staging = {
    0u,
    sizeof(s_fake_ext),
    &s_ext_ops,
    NULL,
};

static fboot_status_t iflash_read(void *ctx, uint32_t offset, uint8_t *data,
                                  size_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memcpy(data, &s_fake_flash[offset], len);
    return FB_OK;
}

static fboot_status_t iflash_write(void *ctx, uint32_t offset,
                                   const uint8_t *data, size_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memcpy(&s_fake_flash[offset], data, len);
    return FB_OK;
}

static fboot_status_t iflash_erase(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memset(&s_fake_flash[offset], 0xFFu, len);
    return FB_OK;
}

static const fastboot_flash_ops_t s_iflash_ops = {
    iflash_read,
    iflash_write,
    iflash_erase,
};

static fastboot_flash_area_t s_primary = {
    0u,
    FAKE_FLASH_SIZE,
    &s_iflash_ops,
    NULL,
};

static bool vector_is_valid(const uint8_t *vector, size_t len,
                            uint32_t load_addr, uint32_t image_size, void *ctx)
{
    uint32_t sp;
    uint32_t reset;

    (void)load_addr;
    (void)image_size;
    (void)ctx;
    if (len < 8u) {
        return false;
    }
    memcpy(&sp, vector, 4u);
    memcpy(&reset, vector + 4u, 4u);
    if (sp < FASTBOOT_SRAM_BASE || sp > FASTBOOT_SRAM_END) {
        return false;
    }
    if (reset < FASTBOOT_APP_FLASH_BASE ||
        reset > FASTBOOT_APP_FLASH_END) {
        return false;
    }
    return true;
}

static fastboot_image_policy_t s_policy = {
    FASTBOOT_APP_FLASH_BASE,
    FASTBOOT_APP_FLASH_SIZE,
    vector_is_valid,
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
    rc = fastboot_staging_install_if_pending(&s_staging, &s_primary,
                                             &s_policy, &s_runtime, NULL);
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
    rc = fastboot_staging_install_if_pending(&s_staging, &s_primary,
                                             &s_policy, &s_runtime, NULL);
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

static fboot_status_t ext_write_offset(void *ctx, uint32_t offset,
                                       const uint8_t *data, size_t len)
{
    (void)ctx;
    uint32_t abs_offset = BASE_OFFSET + offset;
    if (abs_offset + len > sizeof(s_fake_ext_offset)) {
        return FB_ERR_RANGE;
    }
    memcpy(&s_fake_ext_offset[abs_offset], data, len);
    return FB_OK;
}

static fboot_status_t ext_erase_offset(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    uint32_t abs_offset = BASE_OFFSET + offset;
    if (abs_offset + len > sizeof(s_fake_ext_offset)) {
        return FB_ERR_RANGE;
    }
    memset(&s_fake_ext_offset[abs_offset], 0xFFu, len);
    return FB_OK;
}

static const fastboot_flash_ops_t s_ext_ops_offset = {
    ext_read_offset,
    ext_write_offset,
    ext_erase_offset,
};

static fastboot_flash_area_t s_staging_offset = {
    0u,
    sizeof(s_fake_ext_offset) - BASE_OFFSET,
    &s_ext_ops_offset,
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

    rc = fastboot_staging_install_if_pending(&s_staging_offset, &s_primary,
                                             &s_policy, &s_runtime, NULL);
    assert(rc == FB_OK);
    assert(memcmp(s_fake_flash, image, 8u) == 0);
}

static void test_null_params(void)
{
    fboot_status_t rc;

    rc = fastboot_staging_install_if_pending(NULL, &s_primary,
                                             &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_staging_install_if_pending(&s_staging, NULL,
                                             &s_policy, &s_runtime, NULL);
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
