/**
 * @file fastboot_iflash.c
 * @brief STM32F4 内部 Flash 驱动
 *
 * 提供内部 flash 的擦除、编程和读取操作，以及 flash_area_t 实例。
 * 编程时自动处理对齐优化：前导字节 → 4字批量(16字节/次) → 剩余字 → 尾部字节。
 */

#include "fastboot_iflash.h"
#include "fastboot_memory_map.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <string.h>

/**
 * @brief 将地址映射到 STM32F4 flash 扇区编号
 *
 * STM32F411 flash 扇区布局：
 * - 扇区 0-3：各 16 KiB（0x0800_0000 - 0x0800_FFFF）
 * - 扇区 4：64 KiB（0x0801_0000 - 0x0801_FFFF）
 * - 扇区 5-7：各 128 KiB（0x0802_0000 - 0x0807_FFFF）
 *
 * @param addr  flash 地址
 * @return      FLASH_SECTOR_0 ~ FLASH_SECTOR_7
 */
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

/**
 * @brief 检查地址范围是否完全在 App 区域内
 *
 * App 区域由 FASTBOOT_APP_FLASH_BASE 和 FASTBOOT_APP_FLASH_END 定义。
 * 同时检查溢出：end = addr + len - 1 必须 >= addr。
 *
 * @param addr  起始地址
 * @param len   长度（字节）
 * @return      true 范围在 App 区内；false 越界或溢出
 */
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

/**
 * @brief 擦除内部 flash 指定地址范围所在的扇区
 *
 * 自动计算起始和结束扇区，执行批量扇区擦除。
 * 使用电压范围 3（2.7V - 3.6V）。
 *
 * @param addr  起始地址（必须在 App 区内）
 * @param len   擦除长度（字节），0 表示无操作
 * @return      FB_OK 成功；FB_ERR_RANGE 地址越界；FB_ERR_FLASH HAL 错误
 */
fboot_status_t fastboot_iflash_erase_range(uint32_t addr, uint32_t len)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    uint32_t first;
    uint32_t last;

    if (len == 0u) {
        return FB_OK;
    }
    if (!range_inside_app(addr, len)) {
        return FB_ERR_RANGE;
    }

    first = sector_for_addr(addr);
    last = sector_for_addr(addr + len - 1u);

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

/**
 * @brief 向内部 flash 写入数据（自动对齐优化）
 *
 * 写入策略（优化 STM32F4 flash 编程效率）：
 * 1. 前导未对齐字节：逐字节编程，直到地址 4 字节对齐
 * 2. 对齐中间段：每次编程 4 个 word（16 字节），减少 HAL 调用开销
 * 3. 剩余对齐 word：逐 word 编程
 * 4. 尾部未对齐字节：逐字节编程
 *
 * @param addr  目标地址（必须在 App 区内）
 * @param data  源数据指针
 * @param len   数据长度（字节）
 * @return      FB_OK 成功；FB_ERR_PARAM data 为 NULL；FB_ERR_RANGE 地址越界；FB_ERR_FLASH HAL 错误
 */
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

    /* 前导未对齐字节：逐字节编程直到 4 字节对齐 */
    while (pos < len && (addr + pos) % 4u != 0u) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + pos,
                              data[pos]) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return FB_ERR_FLASH;
        }
        ++pos;
    }

    /* 对齐中间段：每次编程 4 个 word（16 字节），提高吞吐 */
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

    /* 剩余对齐 word：逐 word 编程 */
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

    /* 尾部未对齐字节：逐字节编程 */
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

/**
 * @brief 从内部 flash 读取数据
 *
 * 直接通过 memcpy 从 flash 地址读取，无需特殊操作。
 *
 * @param addr  源地址（必须在 App 区内）
 * @param data  目标缓冲区
 * @param len   读取长度（字节）
 * @return      FB_OK 成功；FB_ERR_PARAM data 为 NULL；FB_ERR_RANGE 地址越界
 */
fboot_status_t fastboot_iflash_read(uint32_t addr, uint8_t *data, size_t len)
{
    if (!data && len > 0u) {
        return FB_ERR_PARAM;
    }
    if (!range_inside_app(addr, len)) {
        return FB_ERR_RANGE;
    }
    memcpy(data, (const void *)(uintptr_t)addr, len);
    return FB_OK;
}

/**
 * @brief flash_area_t 擦除回调
 *
 * @param ctx   未使用（内部 flash 无需上下文）
 * @param addr  起始地址
 * @param len   擦除长度
 * @return      fastboot_iflash_erase_range() 的返回值
 */
static fboot_status_t iflash_area_erase(void *ctx, uint32_t addr,
                                        uint32_t len)
{
    (void)ctx;
    return fastboot_iflash_erase_range(addr, len);
}

/**
 * @brief flash_area_t 写入回调
 *
 * @param ctx   未使用
 * @param addr  目标地址
 * @param data  源数据
 * @param len   数据长度
 * @return      fastboot_iflash_write() 的返回值
 */
static fboot_status_t iflash_area_write(void *ctx, uint32_t addr,
                                        const uint8_t *data, size_t len)
{
    (void)ctx;
    return fastboot_iflash_write(addr, data, len);
}

/**
 * @brief flash_area_t 读取回调
 *
 * @param ctx   未使用
 * @param addr  源地址
 * @param data  目标缓冲区
 * @param len   读取长度
 * @return      fastboot_iflash_read() 的返回值
 */
static fboot_status_t iflash_area_read(void *ctx, uint32_t addr,
                                       uint8_t *data, size_t len)
{
    (void)ctx;
    return fastboot_iflash_read(addr, data, len);
}

/**
 * @brief 获取内部 flash primary 区域的 flash_area_t 实例
 *
 * 返回静态分配的 flash_area_t，包含 read/write/erase 回调，
 * base = FASTBOOT_APP_FLASH_BASE，size = FASTBOOT_APP_FLASH_SIZE。
 *
 * @return  指向静态 flash_area_t 的常量指针
 */
const fastboot_flash_area_t *fastboot_iflash_primary_area(void)
{
    static const fastboot_flash_ops_t ops = {
        iflash_area_read,
        iflash_area_write,
        iflash_area_erase,
    };
    static const fastboot_flash_area_t area = {
        FASTBOOT_APP_FLASH_BASE,
        FASTBOOT_APP_FLASH_SIZE,
        &ops,
        NULL,
    };

    return &area;
}
