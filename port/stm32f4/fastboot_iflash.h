/**
 * @file fastboot_iflash.h
 * @brief STM32F411 内部 Flash 操作与 App 区域适配接口。
 */
#ifndef FASTBOOT_IFLASH_H
#define FASTBOOT_IFLASH_H

#include "fastboot_flash.h"
#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 擦除与指定地址范围相交的内部 Flash 扇区。
 * @param[in] addr MCU 绝对地址，必须位于 App 区域。
 * @param[in] len 请求擦除的字节数，0 表示不擦除。
 * @return FB_OK 成功；FB_ERR_RANGE 越界；FB_ERR_FLASH HAL 擦除失败。
 * @note 实际擦除范围由芯片扇区边界决定，可能大于请求范围。
 */
fboot_status_t fastboot_iflash_erase_range(uint32_t addr, uint32_t len);
/**
 * @brief 按地址对齐情况使用 word 或 byte 编程已擦除的内部 Flash。
 * @param[in] addr MCU 绝对地址，必须位于 App 区域。
 * @param[in] data 输入数据，len 大于 0 时必须有效。
 * @param[in] len 写入字节数。
 * @return FB_OK 成功；FB_ERR_PARAM 参数无效；FB_ERR_RANGE 越界；FB_ERR_FLASH 编程失败。
 * @note 调用方负责先擦除目标扇区。
 */
fboot_status_t fastboot_iflash_write(uint32_t addr, const uint8_t *data,
                                     size_t len);
/**
 * @brief 从 App 区域的内存映射 Flash 读取数据。
 * @param[in] addr MCU 绝对地址。
 * @param[out] data 输出缓冲区，len 大于 0 时必须有效。
 * @param[in] len 读取字节数。
 * @return FB_OK 成功；FB_ERR_PARAM 参数无效；FB_ERR_RANGE 越界。
 */
fboot_status_t fastboot_iflash_read(uint32_t addr, uint8_t *data, size_t len);
/**
 * @brief 获取配置的内部 App Flash 区域。
 * @return 静态区域对象的只读指针，调用方不得释放。
 * @note 区域包装接口接收相对偏移，底层回调接收 MCU 绝对地址。
 */
const fastboot_flash_area_t *fastboot_iflash_primary_area(void);

#endif /* FASTBOOT_IFLASH_H */
