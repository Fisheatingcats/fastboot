/**
 * @file fastboot_runtime.h
 * @brief 运行时服务接口
 *
 * 提供系统滴答计时和看门狗喂狗的硬件抽象。
 * 当 FASTBOOT_CFG_ENABLE_WATCHDOG 为 0 时，喂狗调用编译为空操作。
 */

#ifndef FASTBOOT_RUNTIME_H
#define FASTBOOT_RUNTIME_H

#include "fastboot_config.h"
#include <stdint.h>

/**
 * @brief 运行时服务函数表
 *
 * 提供毫秒级滴答读取和看门狗喂狗回调，用于超时检测和系统保活。
 */
typedef struct {
    /** @brief 获取系统毫秒滴答值
     *  @param[in] ctx 平台上下文指针
     *  @return 当前毫秒计数值 */
    uint32_t (*tick_ms)(void *ctx);
    /** @brief 喂看门狗
     *  @param[in] ctx 平台上下文指针 */
    void (*feed_watchdog)(void *ctx);
    /** @brief 用户上下文指针，透传给回调函数 */
    void *ctx;
} fastboot_runtime_t;

/**
 * @brief 获取系统毫秒滴答值
 *
 * 安全包装函数，runtime 或 tick_ms 为 NULL 时返回 0。
 *
 * @param[in] runtime 运行时服务实例指针
 * @return 当前毫秒计数值，无效参数时返回 0
 */
static inline uint32_t fastboot_runtime_tick_ms(
    const fastboot_runtime_t *runtime)
{
    return (runtime && runtime->tick_ms) ? runtime->tick_ms(runtime->ctx) : 0u;
}

/**
 * @brief 喂看门狗
 *
 * 安全包装函数，runtime 或 feed_watchdog 为 NULL 时无操作。
 * 当 FASTBOOT_CFG_ENABLE_WATCHDOG 为 0 时，此函数编译为空操作。
 *
 * @param[in] runtime 运行时服务实例指针
 */
static inline void fastboot_runtime_feed_watchdog(
    const fastboot_runtime_t *runtime)
{
#if FASTBOOT_CFG_ENABLE_WATCHDOG
    if (runtime && runtime->feed_watchdog) {
        runtime->feed_watchdog(runtime->ctx);
    }
#else
    (void)runtime;
#endif
}

#endif /* FASTBOOT_RUNTIME_H */
