#include "fastboot_config.h"
#include "fastboot_ota.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define FAKE_FLASH_SIZE  (FASTBOOT_APP_FLASH_SIZE)

static uint8_t s_fake_flash[FAKE_FLASH_SIZE];
static uint32_t s_erase_count;
static uint32_t s_write_count;

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

static fboot_status_t primary_read(void *ctx, uint32_t offset,
                                   uint8_t *buf, size_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memcpy(buf, &s_fake_flash[offset], len);
    return FB_OK;
}

static fboot_status_t primary_write(void *ctx, uint32_t offset,
                                    const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memcpy(&s_fake_flash[offset], buf, len);
    ++s_write_count;
    return FB_OK;
}

static fboot_status_t primary_erase(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memset(&s_fake_flash[offset], 0xFFu, len);
    ++s_erase_count;
    return FB_OK;
}

static const fastboot_flash_ops_t s_primary_ops = {
    primary_read,
    primary_write,
    primary_erase,
};

static fastboot_flash_area_t s_primary = {
    0u,
    FAKE_FLASH_SIZE,
    &s_primary_ops,
    NULL,
};

static bool s_corrupt_readback;

static fboot_status_t primary_read_corruptible(void *ctx, uint32_t offset,
                                               uint8_t *buf, size_t len)
{
    (void)ctx;
    if (offset + len > FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    memcpy(buf, &s_fake_flash[offset], len);
    if (s_corrupt_readback && len > 0u) {
        buf[0] ^= 0xFFu;
    }
    return FB_OK;
}

static const fastboot_flash_ops_t s_primary_ops_corrupt = {
    primary_read_corruptible,
    primary_write,
    primary_erase,
};

typedef struct {
    const uint8_t *data;
    size_t len;
} fake_source_t;

static fake_source_t s_staging_src;

static fboot_status_t staging_read(void *ctx, uint32_t offset,
                                   uint8_t *buf, size_t len)
{
    fake_source_t *src = (fake_source_t *)ctx;

    if (offset + len > src->len) {
        return FB_ERR_RANGE;
    }
    memcpy(buf, &src->data[offset], len);
    return FB_OK;
}

static const fastboot_flash_ops_t s_staging_ops = {
    staging_read,
    NULL,
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

static const fastboot_image_policy_t s_policy = {
    FASTBOOT_APP_FLASH_BASE,
    FASTBOOT_APP_FLASH_SIZE,
    vector_is_valid,
    NULL,
};

static uint32_t build_fwot(uint8_t *buf, const uint8_t *image, uint32_t image_size)
{
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)buf;
    uint32_t image_crc;

    memset(buf, 0, sizeof(fastboot_ota_header_t));
    hdr->magic = FASTBOOT_OTA_MAGIC;
    hdr->header_size = sizeof(fastboot_ota_header_t);
    hdr->header_version = FASTBOOT_OTA_HEADER_VER;
    hdr->image_offset = sizeof(fastboot_ota_header_t);
    hdr->image_size = image_size;
    hdr->load_addr = FASTBOOT_APP_FLASH_BASE;

    image_crc = fastboot_crc32(image, image_size, 0u);
    hdr->image_crc32 = image_crc;

    hdr->header_crc32 = 0u;
    hdr->header_crc32 = fastboot_crc32(buf, sizeof(fastboot_ota_header_t), 0u);

    memcpy(&buf[sizeof(fastboot_ota_header_t)], image, image_size);
    return sizeof(fastboot_ota_header_t) + image_size;
}

static void build_valid_vector(uint8_t *vec)
{
    uint32_t sp = FASTBOOT_SRAM_BASE + 0x1000u;
    uint32_t reset = FASTBOOT_APP_FLASH_BASE | 1u;

    memcpy(vec, &sp, 4u);
    memcpy(vec + 4u, &reset, 4u);
}

static void test_install_null_params(void)
{
    fboot_status_t rc;
    fastboot_flash_area_t staging = {0u, 0u, &s_staging_ops, &s_staging_src};

    rc = fastboot_ota_install(NULL, &s_primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_ota_install(&staging, NULL, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_ota_install(&staging, &s_primary, NULL, &s_runtime, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_ota_install(&staging, &s_primary, &s_policy, NULL, NULL);
    assert(rc == FB_ERR_PARAM);
}

static void test_install_invalid_magic(void)
{
    uint8_t pkg[128];
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)pkg;
    fastboot_flash_area_t staging;
    fboot_status_t rc;

    memset(pkg, 0, sizeof(pkg));
    hdr->magic = 0xDEADBEEFu;
    hdr->header_size = sizeof(fastboot_ota_header_t);
    hdr->header_version = FASTBOOT_OTA_HEADER_VER;
    hdr->image_offset = sizeof(fastboot_ota_header_t);
    hdr->image_size = 8u;
    hdr->load_addr = FASTBOOT_APP_FLASH_BASE;
    hdr->header_crc32 = 0u;
    hdr->header_crc32 = fastboot_crc32(pkg, sizeof(fastboot_ota_header_t), 0u);

    s_staging_src.data = pkg;
    s_staging_src.len = sizeof(pkg);
    staging.offset = 0u;
    staging.size = sizeof(pkg);
    staging.ops = &s_staging_ops;
    staging.ctx = &s_staging_src;

    rc = fastboot_ota_install(&staging, &s_primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_FORMAT);
}

static void test_install_bad_crc(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)pkg;
    fastboot_flash_area_t staging;
    fboot_status_t rc;

    build_valid_vector(image);
    memset(pkg, 0, sizeof(pkg));
    hdr->magic = FASTBOOT_OTA_MAGIC;
    hdr->header_size = sizeof(fastboot_ota_header_t);
    hdr->header_version = FASTBOOT_OTA_HEADER_VER;
    hdr->image_offset = sizeof(fastboot_ota_header_t);
    hdr->image_size = 8u;
    hdr->load_addr = FASTBOOT_APP_FLASH_BASE;
    hdr->image_crc32 = 0xBADu;
    hdr->header_crc32 = 0u;
    hdr->header_crc32 = fastboot_crc32(pkg, sizeof(fastboot_ota_header_t), 0u);
    memcpy(&pkg[sizeof(fastboot_ota_header_t)], image, 8u);

    s_staging_src.data = pkg;
    s_staging_src.len = sizeof(pkg);
    staging.offset = 0u;
    staging.size = sizeof(pkg);
    staging.ops = &s_staging_ops;
    staging.ctx = &s_staging_src;

    rc = fastboot_ota_install(&staging, &s_primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_CRC);
}

static void test_install_valid_small(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fastboot_flash_area_t staging;
    fboot_status_t rc;

    build_valid_vector(image);
    s_staging_src.data = pkg;
    s_staging_src.len = build_fwot(pkg, image, 8u);
    staging.offset = 0u;
    staging.size = s_staging_src.len;
    staging.ops = &s_staging_ops;
    staging.ctx = &s_staging_src;

    s_erase_count = 0u;
    s_write_count = 0u;
    rc = fastboot_ota_install(&staging, &s_primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_OK);
    assert(s_erase_count == 1u);
    assert(s_write_count >= 1u);
    assert(memcmp(s_fake_flash, image, 8u) == 0);
}

static void test_install_invalid_vector(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fastboot_flash_area_t staging;
    fboot_status_t rc;

    memset(image, 0, sizeof(image));
    image[0] = 0x00u;
    image[1] = 0x80u;
    image[2] = 0x00u;
    image[3] = 0x20u;
    image[4] = 0x00u;
    image[5] = 0x00u;
    image[6] = 0x00u;
    image[7] = 0x00u;
    s_staging_src.data = pkg;
    s_staging_src.len = build_fwot(pkg, image, 8u);
    staging.offset = 0u;
    staging.size = s_staging_src.len;
    staging.ops = &s_staging_ops;
    staging.ctx = &s_staging_src;

    rc = fastboot_ota_install(&staging, &s_primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_FORMAT);
}

static void test_install_readback_corruption(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fastboot_flash_area_t staging;
    fastboot_flash_area_t primary;
    fboot_status_t rc;

    build_valid_vector(image);
    s_staging_src.data = pkg;
    s_staging_src.len = build_fwot(pkg, image, 8u);
    staging.offset = 0u;
    staging.size = s_staging_src.len;
    staging.ops = &s_staging_ops;
    staging.ctx = &s_staging_src;

    primary.offset = 0u;
    primary.size = FAKE_FLASH_SIZE;
    primary.ops = &s_primary_ops_corrupt;
    primary.ctx = NULL;

    s_corrupt_readback = true;
    rc = fastboot_ota_install(&staging, &primary, &s_policy, &s_runtime, NULL);
    assert(rc == FB_ERR_VERIFY);
    s_corrupt_readback = false;
}

int main(void)
{
    memset(s_fake_flash, 0, sizeof(s_fake_flash));
    test_install_null_params();
    test_install_invalid_magic();
    test_install_bad_crc();
    test_install_valid_small();
    test_install_invalid_vector();
    test_install_readback_corruption();
    return 0;
}
