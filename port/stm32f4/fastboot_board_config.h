/**
 * @file fastboot_board_config.h
 * @brief STM32F411CEU6 板级配置与内存布局
 *
 * 定义 FastBoot 库所需的全部编译时配置宏及内存地址映射。
 * 移植到新平台时，修改此文件即可。
 */

#ifndef FASTBOOT_BOARD_CONFIG_H
#define FASTBOOT_BOARD_CONFIG_H

#include <stdint.h>

#define FASTBOOT_BOARD_CONFIG_INCLUDED 1

/* ── 内存布局 ────────────────────────────────────────────────────────────── */

/** @brief 内部 Flash 起始地址 */
#define FASTBOOT_FLASH_BASE           0x08000000u

/** @brief Bootloader 区域大小 (32 KiB) */
#define FASTBOOT_FLASH_SIZE           0x00008000u

/** @brief App 主区起始地址 (Sector 2) */
#define FASTBOOT_APP_FLASH_BASE       0x08008000u

/** @brief App 主区大小 (480 KiB) */
#define FASTBOOT_APP_FLASH_SIZE       0x00078000u

/** @brief App 主区结束地址 */
#define FASTBOOT_APP_FLASH_END        (FASTBOOT_APP_FLASH_BASE + FASTBOOT_APP_FLASH_SIZE)

/** @brief SRAM 起始地址 */
#define FASTBOOT_SRAM_BASE            0x20000000u

/** @brief SRAM 大小 (128 KiB) */
#define FASTBOOT_SRAM_SIZE            0x00020000u

/** @brief SRAM 结束地址 */
#define FASTBOOT_SRAM_END             (FASTBOOT_SRAM_BASE + FASTBOOT_SRAM_SIZE)

/** @brief 外部 Flash 总大小 (8 MiB) */
#define FASTBOOT_EXTFLASH_SIZE        0x800000u

/** @brief OTA 暂存区偏移 */
#define FASTBOOT_EXTFLASH_OTA_OFFSET  0x000000u

/** @brief OTA 暂存区大小 (4 MiB) */
#define FASTBOOT_EXTFLASH_OTA_SIZE    0x400000u

/* ── 功能模块开关 ────────────────────────────────────────────────────────── */

/** @brief 启用 YMODEM-1K 固件接收模块 */
#define FASTBOOT_CFG_ENABLE_YMODEM          1

/** @brief 启用 FWOT 包解析和安装模块 */
#define FASTBOOT_CFG_ENABLE_FWOT            1

/** @brief 启用 Staging 区检测和自动安装模块 */
#define FASTBOOT_CFG_ENABLE_STAGING         1

/** @brief 启用异步写入支持（write_start/poll/busy 回调） */
#define FASTBOOT_CFG_ENABLE_ASYNC_SINK      1

/** @brief 启用 OTA 安装后的 readback CRC 回读验证 */
#define FASTBOOT_CFG_ENABLE_READBACK_VERIFY 1

/** @brief 启用日志输出（fboot_log_t 接口） */
#define FASTBOOT_CFG_ENABLE_LOG             1

/** @brief 启用看门狗喂狗（通过 runtime 接口） */
#define FASTBOOT_CFG_ENABLE_WATCHDOG        1

/* ── 队列和协议参数 ──────────────────────────────────────────────────────── */

/** @brief 数据包队列深度（槽位数量），建议 >= 8 */
#define FASTBOOT_CFG_QUEUE_DEPTH            16u

/** @brief YMODEM 数据包大小（字节），支持 128 或 1024 */
#define FASTBOOT_CFG_YMODEM_PACKET_SIZE     1024u

/** @brief Staging 区容量（字节），默认 4 MiB */
#define FASTBOOT_CFG_STAGING_CAPACITY       FASTBOOT_EXTFLASH_OTA_SIZE

#endif /* FASTBOOT_BOARD_CONFIG_H */
