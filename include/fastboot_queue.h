/**
 * @file fastboot_queue.h
 * @brief 数据包队列接口
 *
 * 实现了生产者-消费者模式的环形队列，用于解耦 YMODEM 数据接收与 Flash 写入。
 * 生产者通过 alloc/commit 提交数据包，消费者通过 drain_one/drain_all 消费并写入 Flash。
 */

#ifndef FASTBOOT_QUEUE_H
#define FASTBOOT_QUEUE_H

#include "fastboot_config.h"
#include "fastboot_runtime.h"
#include "fastboot_status.h"
#include "fastboot_writer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 队列深度（槽位数量），取自板级配置 */
#define FASTBOOT_QUEUE_DEPTH        FASTBOOT_CFG_QUEUE_DEPTH
/** @brief 每个数据包的最大有效载荷大小 */
#define FASTBOOT_QUEUE_PACKET_SIZE  FASTBOOT_CFG_YMODEM_PACKET_SIZE
/** @brief YMODEM 包头大小（起始字节 + 序号 + 反序号） */
#define FASTBOOT_QUEUE_HEADER_SIZE  3u
/** @brief 单个槽位缓冲区总大小 = 包头 + 有效载荷 + CRC16 */
#define FASTBOOT_QUEUE_BUF_SIZE \
    (FASTBOOT_QUEUE_PACKET_SIZE + FASTBOOT_QUEUE_HEADER_SIZE + 2u)

/**
 * @brief 队列槽位
 *
 * 存储一个完整的 YMODEM 数据包及其写入状态。
 */
typedef struct {
    uint8_t  packet[FASTBOOT_QUEUE_BUF_SIZE]; /**< @brief 数据包缓冲区 */
    uint32_t offset;     /**< @brief 写入目标 Flash 偏移 */
    uint32_t len;        /**< @brief 有效数据长度 */
    bool     writing;    /**< @brief 异步写入进行中标记 */
} fboot_queue_slot_t;

/**
 * @brief 环形数据包队列
 *
 * 使用 head/tail/count 管理的固定深度环形队列。
 */
typedef struct {
    fboot_queue_slot_t slots[FASTBOOT_QUEUE_DEPTH]; /**< @brief 槽位数组 */
    uint32_t head;    /**< @brief 队头索引（下一个消费位置） */
    uint32_t tail;    /**< @brief 队尾索引（下一个分配位置） */
    uint32_t count;   /**< @brief 当前队列中的数据包数量 */
} fboot_queue_t;

/**
 * @brief 重置队列为空状态
 *
 * @param[out] q 队列指针
 */
void fastboot_queue_reset(fboot_queue_t *q);

/**
 * @brief 查询队列是否已满
 *
 * @param[in] q 队列指针
 * @return true 队列已满，false 未满
 */
bool fastboot_queue_full(const fboot_queue_t *q);

/**
 * @brief 查询队列是否为空
 *
 * @param[in] q 队列指针
 * @return true 队列为空，false 非空
 */
bool fastboot_queue_empty(const fboot_queue_t *q);

/**
 * @brief 获取队列中当前数据包数量
 *
 * @param[in] q 队列指针
 * @return 当前数据包数量
 */
uint32_t fastboot_queue_count(const fboot_queue_t *q);

/**
 * @brief 分配一个空闲槽位
 *
 * 从队尾分配一个槽位供生产者填充数据。队列满时返回 NULL。
 * 调用方填充数据后必须调用 fastboot_queue_commit() 提交。
 *
 * @param[in,out] q 队列指针
 * @return 可用槽位指针，队列满时返回 NULL
 *
 * @note 分配后必须调用 commit 提交，否则槽位不会被消费。
 */
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q);

/**
 * @brief 提交已填充的槽位
 *
 * 将最近一次 alloc 获取的槽位提交到队列，使其可被消费。
 *
 * @param[in,out] q 队列指针
 *
 * @warning 调用前必须确保已通过 alloc 获取了有效槽位。
 */
void fastboot_queue_commit(fboot_queue_t *q);

/**
 * @brief 消费并写入队头的一个数据包
 *
 * 从队头取出一个数据包，通过 writer 写入 Flash。
 * 支持同步和异步写入模式：异步模式下需多次调用以完成写入。
 *
 * @param[in,out] q       队列指针
 * @param[in]     writer  写入器实例
 * @param[in]     runtime 运行时服务实例（用于喂狗和超时）
 * @param[out]    written 本次写入的字节数（可为 NULL）
 * @retval FB_OK    写入完成并已释放槽位
 * @retval FB_BUSY  异步写入进行中，需继续调用
 * @retval FB_ERR_* 写入失败
 */
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
                                        const fastboot_writer_t *writer,
                                        const fastboot_runtime_t *runtime,
                                        uint32_t *written);

/**
 * @brief 消费并写入队列中所有数据包
 *
 * 循环调用 drain_one 直到队列为空或遇到错误。
 *
 * @param[in,out] q       队列指针
 * @param[in]     writer  写入器实例
 * @param[in]     runtime 运行时服务实例（用于喂狗和超时）
 * @param[out]    written 累计写入的字节数（可为 NULL）
 * @retval FB_OK    全部写入完成
 * @retval FB_ERR_* 写入过程中出错
 */
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q,
                                        const fastboot_writer_t *writer,
                                        const fastboot_runtime_t *runtime,
                                        uint32_t *written);

#endif /* FASTBOOT_QUEUE_H */
