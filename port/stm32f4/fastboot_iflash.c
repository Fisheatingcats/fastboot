#include "fastboot_iflash.h"
#include "fastboot_memory_map.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <string.h>

static uint32_t sector_for_addr(uint32_t addr)
{
    if (addr < 0x08004000u) {
        return FLASH_SECTOR_0;
    }
    if (addr < 0x08008000u) {
        return FLASH_SECTOR_1;
    }
    if (addr < 0x0800C000u) {
        return FLASH_SECTOR_2;
    }
    if (addr < 0x08010000u) {
        return FLASH_SECTOR_3;
    }
    if (addr < 0x08020000u) {
        return FLASH_SECTOR_4;
    }
    if (addr < 0x08040000u) {
        return FLASH_SECTOR_5;
    }
    if (addr < 0x08060000u) {
        return FLASH_SECTOR_6;
    }
    return FLASH_SECTOR_7;
}

static bool range_inside_app(uint32_t addr, size_t len)
{
    uint32_t end;

    if (len == 0u) {
        return true;
    }
    end = addr + (uint32_t)len - 1u;
    return addr >= FASTBOOT_APP_FLASH_BASE &&
           end >= addr &&
           end < FASTBOOT_APP_FLASH_END;
}

fboot_status_t fastboot_iflash_erase_app(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    uint32_t first = sector_for_addr(FASTBOOT_APP_FLASH_BASE);
    uint32_t last = sector_for_addr(FASTBOOT_APP_FLASH_END - 1u);

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = first;
    erase.NbSectors = (last - first) + 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return FB_ERR_FLASH;
    }
    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        (void)HAL_FLASH_Lock();
        return FB_ERR_FLASH;
    }
    (void)HAL_FLASH_Lock();
    return FB_OK;
}

fboot_status_t fastboot_iflash_write(uint32_t addr, const uint8_t *data,
                                            size_t len)
{
    if (!data && len > 0u) {
        return FB_ERR_PARAM;
    }
    if (!range_inside_app(addr, len)) {
        return FB_ERR_RANGE;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return FB_ERR_FLASH;
    }

    size_t pos = 0u;

    /* Leading unaligned bytes - program one byte at a time until aligned. */
    while (pos < len && (addr + pos) % 4u != 0u) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + pos,
                              data[pos]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return FB_ERR_FLASH;
        }
        ++pos;
    }

    /* Aligned middle - program 4 words at a time (16 bytes per call). */
    while (pos + 16u <= len) {
        uint32_t words[4];
        memcpy(words, &data[pos], 16u);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + pos,
                              words[0]) != HAL_OK ||
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + pos + 4u,
                              words[1]) != HAL_OK ||
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + pos + 8u,
                              words[2]) != HAL_OK ||
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + pos + 12u,
                              words[3]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return FB_ERR_FLASH;
        }
        pos += 16u;
    }

    /* Remaining aligned words. */
    while (pos + 4u <= len) {
        uint32_t word;
        memcpy(&word, &data[pos], 4u);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + pos,
                              word) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return FB_ERR_FLASH;
        }
        pos += 4u;
    }

    /* Trailing unaligned bytes. */
    while (pos < len) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + pos,
                              data[pos]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return FB_ERR_FLASH;
        }
        ++pos;
    }

    (void)HAL_FLASH_Lock();
    return FB_OK;
}

fboot_status_t fastboot_iflash_verify(uint32_t addr,
                                             const uint8_t *data, size_t len)
{
    if (!data && len > 0u) {
        return FB_ERR_PARAM;
    }
    if (!range_inside_app(addr, len)) {
        return FB_ERR_RANGE;
    }
    if (memcmp((const void *)(uintptr_t)addr, data, len) != 0) {
        return FB_ERR_VERIFY;
    }
    return FB_OK;
}
