/**
 * @file fastboot_writer.h
 * @brief 数据写入器接口
 *
 * 定义了 FastBoot 使用的数据写入器抽象，支持同步和异步两种写入模式。
 * 同步模式通过 write 回调完成；异步模式通过 write_start/poll/busy 三步协作。
 */

#ifndef FASTBOOT_WRITER_H
#define FASTBOOT_WRITER_H

#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 数据写入器函数表
 *
 * 提供固件数据写入 Flash 的抽象接口。
 * 支持同步写入（write）和异步写入（write_start + poll + busy）两种模式。
 */
typedef struct {
    /** @brief 初始化写入器，准备接收指定大小的数据
     *  @param[in] ctx  平台上下文指针
     *  @param[in] size 待写入数据总字节数
     *  @return FB_OK 成功，其他为错误码 */
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    /** @brief 同步写入数据（阻塞直到写入完成）
     *  @param[in] ctx    平台上下文指针
     *  @param[in] offset 写入起始偏移
     *  @param[in] data   写入数据缓冲区
     *  @param[in] len    写入字节数
     *  @return FB_OK 成功，其他为错误码 */
    fboot_status_t (*write)(void *ctx, uint32_t offset,
                            const uint8_t *data, size_t len);
    /** @brief 异步启动写入（非阻塞，立即返回）
     *  @param[in] ctx    平台上下文指针
     *  @param[in] offset 写入起始偏移
     *  @param[in] data   写入数据缓冲区（调用方需在 busy 返回 false 前保持有效）
     *  @param[in] len    写入字节数
     *  @return FB_OK 启动成功，其他为错误码 */
    fboot_status_t (*write_start)(void *ctx, uint32_t offset,
                                  const uint8_t *data, size_t len);
    /** @brief 轮询异步写入状态，驱动写入进度
     *  @param[in] ctx 平台上下文指针
     *  @return FB_OK 写入完成，FB_BUSY 写入进行中，其他为错误码 */
    fboot_status_t (*poll)(void *ctx);
    /** @brief 查询异步写入是否仍在进行
     *  @param[in] ctx 平台上下文指针
     *  @return true 正在写入，false 写入已完成或未启动 */
    bool (*busy)(void *ctx);
    /** @brief 用户上下文指针，透传给回调函数 */
    void *ctx;
} fastboot_writer_t;

#endif /* FASTBOOT_WRITER_H */
