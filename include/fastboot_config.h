/**
 * @file fastboot_config.h
 * @brief FastBoot 板级配置校验
 *
 * 包含板级配置头文件并对其所有必需宏进行编译期断言校验。
 * 板级 BSP 必须提供 fastboot_board_config.h 并定义全部 FASTBOOT_CFG_* 宏，
 * 否则编译将报错。
 */

#ifndef FASTBOOT_CONFIG_H
#define FASTBOOT_CONFIG_H

#include "fastboot_board_config.h"

#ifndef FASTBOOT_BOARD_CONFIG_INCLUDED
#error "Board must provide fastboot_board_config.h"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_YMODEM 以启用/禁用 YMODEM 接收功能 */
#ifndef FASTBOOT_CFG_ENABLE_YMODEM
#error "FASTBOOT_CFG_ENABLE_YMODEM is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_FWOT 以启用/禁用 FWOT 包解析功能 */
#ifndef FASTBOOT_CFG_ENABLE_FWOT
#error "FASTBOOT_CFG_ENABLE_FWOT is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_STAGING 以启用/禁用 staging 区安装功能 */
#ifndef FASTBOOT_CFG_ENABLE_STAGING
#error "FASTBOOT_CFG_ENABLE_STAGING is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_ASYNC_SINK 以启用/禁用异步写入器 */
#ifndef FASTBOOT_CFG_ENABLE_ASYNC_SINK
#error "FASTBOOT_CFG_ENABLE_ASYNC_SINK is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_READBACK_VERIFY 以启用/禁用回读校验 */
#ifndef FASTBOOT_CFG_ENABLE_READBACK_VERIFY
#error "FASTBOOT_CFG_ENABLE_READBACK_VERIFY is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_LOG 以启用/禁用日志输出 */
#ifndef FASTBOOT_CFG_ENABLE_LOG
#error "FASTBOOT_CFG_ENABLE_LOG is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_ENABLE_WATCHDOG 以启用/禁用看门狗喂狗 */
#ifndef FASTBOOT_CFG_ENABLE_WATCHDOG
#error "FASTBOOT_CFG_ENABLE_WATCHDOG is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_QUEUE_DEPTH 指定数据包队列深度 */
#ifndef FASTBOOT_CFG_QUEUE_DEPTH
#error "FASTBOOT_CFG_QUEUE_DEPTH is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_YMODEM_PACKET_SIZE 指定 YMODEM 包大小 */
#ifndef FASTBOOT_CFG_YMODEM_PACKET_SIZE
#error "FASTBOOT_CFG_YMODEM_PACKET_SIZE is required"
#endif

/** @brief 必须定义 FASTBOOT_CFG_STAGING_CAPACITY 指定 staging 区容量 */
#ifndef FASTBOOT_CFG_STAGING_CAPACITY
#error "FASTBOOT_CFG_STAGING_CAPACITY is required"
#endif

#if FASTBOOT_CFG_QUEUE_DEPTH < 2u
#error "FASTBOOT_CFG_QUEUE_DEPTH must be at least 2"
#endif

#if FASTBOOT_CFG_YMODEM_PACKET_SIZE < 128u
#error "FASTBOOT_CFG_YMODEM_PACKET_SIZE must be at least 128"
#endif

#endif /* FASTBOOT_CONFIG_H */
