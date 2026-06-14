/**
 * @file fastboot_transport.h
 * @brief 字节传输接口
 *
 * 定义了 FastBoot 使用的字节级传输抽象，用于 YMODEM 协议的底层通信。
 * 各平台通过实现 read/write_byte 回调来适配 UART、USB CDC 等传输介质。
 */

#ifndef FASTBOOT_TRANSPORT_H
#define FASTBOOT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 字节传输函数表
 *
 * 提供阻塞式字节读取和单字节写入接口。
 * read 支持超时机制，write_byte 为同步阻塞写入。
 */
typedef struct {
    /** @brief 从传输介质读取数据
     *  @param[in]  ctx        平台上下文指针
     *  @param[out] buf        读取缓冲区
     *  @param[in]  len        期望读取字节数
     *  @param[in]  timeout_ms 超时时间（毫秒），0 表示不等待
     *  @return 实际读取的字节数，超时返回 0 */
    size_t (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
    /** @brief 向传输介质写入单个字节（阻塞）
     *  @param[in] ctx  平台上下文指针
     *  @param[in] byte 要发送的字节 */
    void (*write_byte)(void *ctx, uint8_t byte);
    /** @brief 用户上下文指针，透传给回调函数 */
    void *ctx;
} fastboot_transport_t;

#endif /* FASTBOOT_TRANSPORT_H */
