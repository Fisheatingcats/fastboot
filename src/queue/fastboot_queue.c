/**
 * @file fastboot_queue.c
 * @brief 数据包队列实现
 *
 * 提供环形缓冲区队列，用于解耦 YMODEM 接收与 flash 写入。
 * 支持槽位分配（alloc）→ 数据填充 → 提交（commit）的工作模式，
 * 以及同步/异步写入的排水操作。
 */

#include "fastboot_queue.h"

/** @brief 包头长度（类型 + 序号 + 反码），用于定位载荷起始位置 */
#define PACKET_HEADER  3u

/**
 * @brief 重置队列到初始状态
 *
 * 将 head、tail、count 全部清零。通常在 YMODEM 会话开始时调用。
 *
 * @param q  队列指针
 */
void fastboot_queue_reset(fboot_queue_t *q)
{
    q->head  = 0u;
    q->tail  = 0u;
    q->count = 0u;
}

/**
 * @brief 检查队列是否已满
 *
 * @param q  队列指针
 * @return   true 队列满；false 有空闲槽位
 */
bool fastboot_queue_full(const fboot_queue_t *q)
{
    return q->count >= FASTBOOT_QUEUE_DEPTH;
}

/**
 * @brief 检查队列是否为空
 *
 * @param q  队列指针
 * @return   true 队列空；false 有待排水槽位
 */
bool fastboot_queue_empty(const fboot_queue_t *q)
{
    return q->count == 0u;
}

/**
 * @brief 获取队列中待排水的槽位数量
 *
 * @param q  队列指针
 * @return   当前排队的槽位数
 */
uint32_t fastboot_queue_count(const fboot_queue_t *q)
{
    return q->count;
}

/**
 * @brief 分配队列尾部槽位（预留）
 *
 * 若队列未满，返回 tail 位置的槽位指针供调用者填充数据。
 * 调用者填充完毕后需调用 fastboot_queue_commit() 提交。
 *
 * @note  若队列已满返回 NULL，调用者不应解引用。
 *
 * @param q  队列指针
 * @return   可写入的槽位指针；队列满时返回 NULL
 */
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q)
{
    if (fastboot_queue_full(q)) {
        return NULL;
    }
    q->slots[q->tail].writing = false;
    return &q->slots[q->tail];
}

/**
 * @brief 提交已分配的槽位
 *
 * 将 tail 指针前移并增加 count，使槽位对 drain 操作可见。
 * 必须在 fastboot_queue_alloc() 之后、填充数据之后调用。
 *
 * @param q  队列指针
 */
void fastboot_queue_commit(fboot_queue_t *q)
{
    q->tail = (q->tail + 1u) % FASTBOOT_QUEUE_DEPTH;
    ++q->count;
}

/**
 * @brief 排水一个队列槽位
 *
 * 从 head 位置取出槽位并写入 flash。支持三种写入模式：
 * 1. 异步写入进行中（slot->writing）：调用 writer->poll() 检查完成状态
 * 2. 异步写入就绪（write_start + poll + busy 均可用）：启动异步写入
 * 3. 同步写入：直接调用 writer->write()
 *
 * @note  异步模式下，若 write_start 返回 FB_BUSY 或 busy() 为 true，
 *        标记 slot->writing 并返回 FB_BUSY，下次调用时进入 poll 路径。
 *
 * @param q        队列指针
 * @param writer   写入接口
 * @param runtime  运行时接口（喂看门狗）
 * @param written  [in/out] 已写字节数累加器（可为 NULL）
 * @return         FB_OK 槽位已排水；FB_BUSY 异步写入进行中；
 *                 FB_NO_UPDATE 队列空；其他为错误码
 */
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
                                        const fastboot_writer_t *writer,
                                        const fastboot_runtime_t *runtime,
                                        uint32_t *written)
{
    fboot_queue_slot_t *slot;
    fboot_status_t rc;

    if (!writer) {
        return FB_ERR_PARAM;
    }
    if (fastboot_queue_empty(q)) {
        return FB_NO_UPDATE;
    }

    slot = &q->slots[q->head];

    if (slot->writing) {
        /* 异步写入进行中，轮询完成状态 */
        rc = writer->poll ? writer->poll(writer->ctx) : FB_OK;
#if FASTBOOT_CFG_ENABLE_ASYNC_SINK
    } else if (writer->write_start && writer->poll && writer->busy) {
        /* 异步写入就绪，启动 write_start */
        rc = writer->write_start(writer->ctx, slot->offset,
                                 &slot->packet[PACKET_HEADER], slot->len);
        if (rc == FB_BUSY || writer->busy(writer->ctx)) {
            slot->writing = true;
            return FB_BUSY;
        }
#endif
    } else {
        /* 同步写入 */
        rc = writer->write(writer->ctx, slot->offset,
                           &slot->packet[PACKET_HEADER], slot->len);
    }

    if (rc == FB_BUSY) {
        return FB_BUSY;
    }
    if (rc != FB_OK) {
        return rc;
    }
    /* 异步写入完成但 busy() 仍为 true，等待下一次 poll */
    if (slot->writing && writer->busy && writer->busy(writer->ctx)) {
        return FB_BUSY;
    }

    /* 更新已写字节数（取较大值，防止回绕） */
    if (written && *written < slot->offset + slot->len) {
        *written = slot->offset + slot->len;
    }

    /* 前移 head 指针，释放槽位 */
    q->head = (q->head + 1u) % FASTBOOT_QUEUE_DEPTH;
    --q->count;
    fastboot_runtime_feed_watchdog(runtime);
    return FB_OK;
}

/**
 * @brief 排水队列中所有槽位
 *
 * 循环调用 fastboot_queue_drain_one() 直到队列为空。
 * FB_BUSY 时继续重试（异步写入尚未完成）；其他错误立即返回。
 *
 * @param q        队列指针
 * @param writer   写入接口
 * @param runtime  运行时接口
 * @param written  [in/out] 已写字节数累加器（可为 NULL）
 * @return         FB_OK 全部排水；其他为错误码
 */
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q,
                                        const fastboot_writer_t *writer,
                                        const fastboot_runtime_t *runtime,
                                        uint32_t *written)
{
    while (!fastboot_queue_empty(q)) {
        fboot_status_t rc =
            fastboot_queue_drain_one(q, writer, runtime, written);
        if (rc == FB_BUSY) {
            fastboot_runtime_feed_watchdog(runtime);
            continue;
        }
        if (rc != FB_OK && rc != FB_NO_UPDATE) {
            return rc;
        }
    }
    return FB_OK;
}
