/**
 * @file fastboot_stm32_runtime.h
 * @brief STM32 HAL 毫秒时基与看门狗服务适配接口。
 */
#ifndef FASTBOOT_STM32_RUNTIME_H
#define FASTBOOT_STM32_RUNTIME_H

#include "fastboot_runtime.h"

/**
 * @brief 获取 STM32 运行时服务对象。
 * @return 静态服务对象的只读指针，调用方不得释放。
 * @note tick_ms 使用 HAL_GetTick；当前 feed_watchdog 为空实现，需由产品适配。
 */
const fastboot_runtime_t *fastboot_stm32_runtime(void);

#endif /* FASTBOOT_STM32_RUNTIME_H */
