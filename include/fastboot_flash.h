/**
 * @file fastboot_flash.h
 * @brief Flash 区域抽象接口
 *
 * 定义了 Flash 操作函数表和 Flash 区域描述结构体，
 * 并提供一组内联辅助函数用于安全地执行 Flash 读/写/擦除操作。
 * 各平台通过实现 fastboot_flash_ops_t 函数表来适配不同的 Flash 硬件。
 */

#ifndef FASTBOOT_FLASH_H
#define FASTBOOT_FLASH_H

#include "fastboot_status.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Flash 操作函数表
 *
 * 提供 read/write/erase 三种 Flash 操作的函数指针。
 * 各平台（内部 Flash、W25Q64 等）通过实现此接口接入 FastBoot。
 */
typedef struct fastboot_flash_ops {
    /** @brief 从 Flash 读取数据
     *  @param[in]  ctx    平台上下文指针
     *  @param[in]  offset 绝对地址偏移
     *  @param[out] buf    读取缓冲区
     *  @param[in]  len    读取字节数
     *  @return FB_OK 成功，其他为错误码 */
    fboot_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buf,
                           size_t len);
    /** @brief 向 Flash 写入数据
     *  @param[in] ctx    平台上下文指针
     *  @param[in] offset 绝对地址偏移
     *  @param[in] buf    写入数据缓冲区
     *  @param[in] len    写入字节数
     *  @return FB_OK 成功，其他为错误码 */
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *buf,
                            size_t len);
    /** @brief 擦除 Flash 指定区域
     *  @param[in] ctx    平台上下文指针
     *  @param[in] offset 绝对地址偏移
     *  @param[in] len    擦除字节数（应为扇区大小的整数倍）
     *  @return FB_OK 成功，其他为错误码 */
    fboot_status_t (*erase)(void *ctx, uint32_t offset, uint32_t len);
} fastboot_flash_ops_t;

/**
 * @brief Flash 区域描述
 *
 * 描述一个连续的 Flash 区域，包含起始偏移、大小、操作函数表和平台上下文。
 * 内联辅助函数会将相对偏移自动转换为绝对地址。
 */
typedef struct {
    uint32_t offset;                  /**< @brief 区域在 Flash 中的绝对起始偏移 */
    uint32_t size;                    /**< @brief 区域大小（字节） */
    const fastboot_flash_ops_t *ops;  /**< @brief Flash 操作函数表指针 */
    void *ctx;                        /**< @brief 平台上下文指针，透传给 ops 回调 */
} fastboot_flash_area_t;

/**
 * @brief 校验 Flash 区域操作的参数合法性
 *
 * 检查 area 指针、ops 指针是否有效，以及 offset+len 是否在区域内。
 *
 * @param[in] area   Flash 区域描述指针
 * @param[in] offset 区域内相对偏移
 * @param[in] len    操作长度（字节）
 * @retval FB_OK        参数合法
 * @retval FB_ERR_PARAM area 或 ops 为空
 * @retval FB_ERR_RANGE offset+len 超出区域范围或地址溢出
 */
static inline fboot_status_t fastboot_flash_area_check(
    const fastboot_flash_area_t *area, uint32_t offset, size_t len)
{
    if (!area || !area->ops) {
        return FB_ERR_PARAM;
    }
    if (offset > area->size || len > (size_t)(area->size - offset)) {
        return FB_ERR_RANGE;
    }
    if (area->offset + offset < area->offset) {
        return FB_ERR_RANGE;
    }
    return FB_OK;
}

/**
 * @brief 从 Flash 区域读取数据
 *
 * 先校验参数合法性，再将相对偏移转换为绝对地址后调用 ops->read。
 *
 * @param[in]  area   Flash 区域描述指针
 * @param[in]  offset 区域内相对偏移
 * @param[out] buf    读取缓冲区
 * @param[in]  len    读取字节数
 * @return FB_OK 成功，其他为错误码
 */
static inline fboot_status_t fastboot_flash_area_read(
    const fastboot_flash_area_t *area, uint32_t offset, uint8_t *buf,
    size_t len)
{
    fboot_status_t rc = fastboot_flash_area_check(area, offset, len);

    if (rc != FB_OK) {
        return rc;
    }
    if (!area->ops->read) {
        return FB_ERR_PARAM;
    }
    return area->ops->read(area->ctx, area->offset + offset, buf, len);
}

/**
 * @brief 向 Flash 区域写入数据
 *
 * 先校验参数合法性，再将相对偏移转换为绝对地址后调用 ops->write。
 *
 * @param[in] area   Flash 区域描述指针
 * @param[in] offset 区域内相对偏移
 * @param[in] buf    写入数据缓冲区
 * @param[in] len    写入字节数
 * @return FB_OK 成功，其他为错误码
 */
static inline fboot_status_t fastboot_flash_area_write(
    const fastboot_flash_area_t *area, uint32_t offset, const uint8_t *buf,
    size_t len)
{
    fboot_status_t rc = fastboot_flash_area_check(area, offset, len);

    if (rc != FB_OK) {
        return rc;
    }
    if (!area->ops->write) {
        return FB_ERR_PARAM;
    }
    return area->ops->write(area->ctx, area->offset + offset, buf, len);
}

/**
 * @brief 擦除 Flash 区域
 *
 * 先校验参数合法性，len 为 0 时直接返回成功，
 * 再将相对偏移转换为绝对地址后调用 ops->erase。
 *
 * @param[in] area   Flash 区域描述指针
 * @param[in] offset 区域内相对偏移
 * @param[in] len    擦除字节数
 * @return FB_OK 成功，其他为错误码
 *
 * @note len 为 0 时不会调用底层擦除函数，直接返回 FB_OK。
 */
static inline fboot_status_t fastboot_flash_area_erase(
    const fastboot_flash_area_t *area, uint32_t offset, uint32_t len)
{
    fboot_status_t rc = fastboot_flash_area_check(area, offset, len);

    if (rc != FB_OK) {
        return rc;
    }
    if (len == 0u) {
        return FB_OK;
    }
    if (!area->ops->erase) {
        return FB_ERR_PARAM;
    }
    return area->ops->erase(area->ctx, area->offset + offset, len);
}

#endif /* FASTBOOT_FLASH_H */
