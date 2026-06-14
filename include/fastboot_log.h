/**
 * @file fastboot_log.h
 * @brief FastBoot 日志接口
 *
 * 提供轻量级的日志输出接口，支持字符串打印和十进制数值打印。
 * 当 FASTBOOT_CFG_ENABLE_LOG 为 0 时，所有日志调用将被编译为空操作。
 */

#ifndef FASTBOOT_LOG_H
#define FASTBOOT_LOG_H

#include "fastboot_config.h"
#include <stdint.h>

/**
 * @brief 日志输出函数表
 *
 * 通过函数指针实现日志输出的硬件解耦。
 * 使用者需提供 puts 和 dec32 两个回调以及可选的上下文指针。
 */
typedef struct {
    /** @brief 输出字符串回调 */
    void (*puts)(void *ctx, const char *s);
    /** @brief 输出标签+十进制数值回调（如 "size: 1024"） */
    void (*dec32)(void *ctx, const char *label, uint32_t value);
    /** @brief 用户上下文指针，透传给回调函数 */
    void *ctx;
} fboot_log_t;

/**
 * @brief 输出字符串日志
 *
 * 当日志功能禁用时，此函数编译为空操作。
 *
 * @param[in] log 日志实例指针，可为 NULL
 * @param[in] s   要输出的以 '\0' 结尾的字符串
 */
static inline void fboot_log_puts(const fboot_log_t *log, const char *s)
{
#if FASTBOOT_CFG_ENABLE_LOG
    if (log && log->puts) {
        log->puts(log->ctx, s);
    }
#else
    (void)log;
    (void)s;
#endif
}

/**
 * @brief 输出十进制数值日志
 *
 * 以 "label: value" 格式输出一个 uint32_t 数值。
 * 当日志功能禁用时，此函数编译为空操作。
 *
 * @param[in] log   日志实例指针，可为 NULL
 * @param[in] label 数值标签字符串
 * @param[in] value 要输出的数值
 */
static inline void fboot_log_dec32(const fboot_log_t *log, const char *label,
                                   uint32_t value)
{
#if FASTBOOT_CFG_ENABLE_LOG
    if (log && log->dec32) {
        log->dec32(log->ctx, label, value);
    }
#else
    (void)log;
    (void)label;
    (void)value;
#endif
}

#endif /* FASTBOOT_LOG_H */
