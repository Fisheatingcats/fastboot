#include "fastboot_ota.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_crc32_empty(void)
{
    uint32_t crc = fastboot_crc32(NULL, 0u, 0u);

    assert(crc == 0x00000000u);
}

static void test_crc32_known(void)
{
    const char *data = "123456789";
    uint32_t crc = fastboot_crc32((const uint8_t *)data, 9u, 0u);

    assert(crc == 0xCBF43926u);
}

static void test_crc32_seeded(void)
{
    const uint8_t data[] = {0x01u, 0x02u, 0x03u};
    uint32_t crc1 = fastboot_crc32(data, sizeof(data), 0u);
    uint32_t crc2 = fastboot_crc32(data + 1u, 2u, fastboot_crc32(data, 1u, 0u));

    assert(crc1 == crc2);
}

static void test_crc32_single_byte(void)
{
    uint32_t crc = fastboot_crc32((const uint8_t *)"A", 1u, 0u);

    assert(crc != 0u);
}

int main(void)
{
    test_crc32_empty();
    test_crc32_known();
    test_crc32_seeded();
    test_crc32_single_byte();
    return 0;
}
