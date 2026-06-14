/**
 * @file fastboot_stm32_runtime.c
 * @brief STM32 运行时接口实现
 *
 * 提供 fastboot_runtime_t 实例，封装 HAL_GetTick() 作为系统滴答源，
 * 以及空操作的看门狗喂狗回调（当前未启用硬件看门狗）。
 */

#include "fastboot_stm32_runtime.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/**
 * @brief 获取系统滴答毫秒数
 *
 * 封装 HAL_GetTick()，供 fastboot 超时计算使用。
 *
 * @param ctx  未使用
 * @return     系统启动以来的毫秒数
 */
static uint32_t stm32_tick_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

/**
 * @brief 看门狗喂狗回调（空操作）
 *
 * 当前未启用硬件看门狗，此函数为空实现。
 * 若需要启用，可在此处添加 IWDG 刷新操作。
 *
 * @param ctx  未使用
 */
static void stm32_feed_watchdog(void *ctx)
{
    (void)ctx;
}

/**
 * @brief 获取 STM32 运行时接口的 runtime_t 实例
 *
 * 返回静态分配的 runtime_t，包含 tick_ms 和 feed_watchdog 回调。
 *
 * @return  指向静态 runtime_t 的常量指针
 */
const fastboot_runtime_t *fastboot_stm32_runtime(void)
{
    static const fastboot_runtime_t runtime = {
        stm32_tick_ms,
        stm32_feed_watchdog,
        NULL,
    };

    return &runtime;
}
