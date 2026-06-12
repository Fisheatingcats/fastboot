#include "fastboot_config.h"
#include "fastboot_ota.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define FAKE_FLASH_SIZE  (FASTBOOT_APP_FLASH_SIZE)
#define FAKE_FLASH_BASE  FASTBOOT_APP_FLASH_BASE

static uint8_t s_fake_flash[FAKE_FLASH_SIZE];
static uint32_t s_erase_count;
static uint32_t s_write_count;

static fboot_status_t fake_erase_app(void *ctx)
{
    (void)ctx;
    memset(s_fake_flash, 0xFFu, sizeof(s_fake_flash));
    ++s_erase_count;
    return FB_OK;
}

static fboot_status_t fake_write(void *ctx, uint32_t addr,
                                 const uint8_t *data, size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FAKE_FLASH_BASE || addr + len > FAKE_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FAKE_FLASH_BASE;
    memcpy(&s_fake_flash[offset], data, len);
    ++s_write_count;
    return FB_OK;
}

static fboot_status_t fake_verify(void *ctx, uint32_t addr,
                                  const uint8_t *data, size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FAKE_FLASH_BASE || addr + len > FAKE_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FAKE_FLASH_BASE;
    return memcmp(&s_fake_flash[offset], data, len) == 0 ? FB_OK : FB_ERR_VERIFY;
}

static fboot_status_t fake_iflash_read(void *ctx, uint32_t addr, uint8_t *data,
                                       size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FAKE_FLASH_BASE || addr + len > FAKE_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FAKE_FLASH_BASE;
    memcpy(data, &s_fake_flash[offset], len);
    return FB_OK;
}

static const fastboot_iflash_ops_t s_iflash_ops = {
    fake_erase_app,
    fake_write,
    fake_verify,
    fake_iflash_read,
    NULL,
};

typedef struct {
    const uint8_t *data;
    size_t len;
} fake_source_t;

static int fake_read(uint32_t offset, uint8_t *data, size_t len, void *ctx)
{
    fake_source_t *src = (fake_source_t *)ctx;

    if (offset + len > src->len) {
        return -1;
    }
    memcpy(data, &src->data[offset], len);
    return 0;
}

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

    rc = fastboot_ota_install(NULL, NULL, &s_iflash_ops, NULL);
    assert(rc == FB_ERR_PARAM);

    rc = fastboot_ota_install(fake_read, NULL, NULL, NULL);
    assert(rc == FB_ERR_PARAM);
}

static void test_install_invalid_magic(void)
{
    uint8_t pkg[128];
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)pkg;
    fake_source_t src = {pkg, sizeof(pkg)};
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

    rc = fastboot_ota_install(fake_read, &src, &s_iflash_ops, NULL);
    assert(rc == FB_ERR_FORMAT);
}

static void test_install_bad_crc(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fastboot_ota_header_t *hdr = (fastboot_ota_header_t *)pkg;
    fake_source_t src = {pkg, sizeof(pkg)};
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

    rc = fastboot_ota_install(fake_read, &src, &s_iflash_ops, NULL);
    assert(rc == FB_ERR_CRC);
}

static void test_install_valid_small(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fake_source_t src;
    fboot_status_t rc;

    build_valid_vector(image);
    src.data = pkg;
    src.len = build_fwot(pkg, image, 8u);

    s_erase_count = 0u;
    s_write_count = 0u;
    rc = fastboot_ota_install(fake_read, &src, &s_iflash_ops, NULL);
    assert(rc == FB_OK);
    assert(s_erase_count == 1u);
    assert(s_write_count >= 1u);
    assert(memcmp(s_fake_flash, image, 8u) == 0);
}

static void test_install_invalid_vector(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fake_source_t src;
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
    src.data = pkg;
    src.len = build_fwot(pkg, image, 8u);

    rc = fastboot_ota_install(fake_read, &src, &s_iflash_ops, NULL);
    assert(rc == FB_ERR_FORMAT);
}

static bool s_corrupt_readback;

static fboot_status_t fake_iflash_read_corruptible(void *ctx, uint32_t addr,
                                                   uint8_t *data, size_t len)
{
    uint32_t offset;

    (void)ctx;
    if (addr < FAKE_FLASH_BASE || addr + len > FAKE_FLASH_BASE + FAKE_FLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    offset = addr - FAKE_FLASH_BASE;
    memcpy(data, &s_fake_flash[offset], len);
    if (s_corrupt_readback && len > 0u) {
        data[0] ^= 0xFFu;
    }
    return FB_OK;
}

static void test_install_readback_corruption(void)
{
    uint8_t image[8];
    uint8_t pkg[256];
    fake_source_t src;
    fastboot_iflash_ops_t corrupt_ops;
    fboot_status_t rc;

    build_valid_vector(image);
    src.data = pkg;
    src.len = build_fwot(pkg, image, 8u);

    corrupt_ops = s_iflash_ops;
    corrupt_ops.read = fake_iflash_read_corruptible;

    s_corrupt_readback = true;
    rc = fastboot_ota_install(fake_read, &src, &corrupt_ops, NULL);
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
