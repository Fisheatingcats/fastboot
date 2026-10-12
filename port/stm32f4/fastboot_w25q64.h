/**
 * @file fastboot_w25q64.h
 * @brief W25Q64 原始访问、OTA 暂存写入器与性能统计接口。
 * @note 原始接口使用芯片内偏移；writer 使用暂存区内偏移。
 * @note 驱动使用静态状态，不支持并发访问或多个 Flash 实例。
 */
#ifndef FASTBOOT_W25Q64_H
#define FASTBOOT_W25Q64_H

#include "fastboot_flash.h"
#include "fastboot_status.h"
#include "fastboot_writer.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 初始化 SPI2、片选 GPIO 并检查 JEDEC 厂商和容量标识。
 * @return FB_OK 成功或已初始化；FB_ERR_IO HAL/SPI 失败；FB_ERR_ID 标识不匹配。
 */
fboot_status_t fastboot_w25q64_init(void);
/**
 * @brief 读取缓存的 24 位 JEDEC 标识。
 * @return 厂商字节、类型字节和容量字节拼接的值。
 * @note 本函数不访问 SPI，也不触发初始化；应在 init 成功后调用。
 */
uint32_t fastboot_w25q64_jedec_id(void);
/**
 * @brief 阻塞读取外部 Flash。
 * @param[in] offset 芯片内绝对字节偏移。
 * @param[out] data 输出缓冲区，len 大于 0 时必须有效。
 * @param[in] len 字节数，单次不能超过 UINT16_MAX。
 * @return FB_OK 成功；FB_ERR_PARAM/FB_ERR_RANGE 参数或范围错误；其他为底层错误。
 */
fboot_status_t fastboot_w25q64_read(uint32_t offset, uint8_t *data,
                                    size_t len);
/**
 * @brief 按 256 字节页边界分块编程外部 Flash，并等待完成。
 * @param[in] offset 芯片内绝对字节偏移。
 * @param[in] data 输入缓冲区，len 大于 0 时必须有效。
 * @param[in] len 写入字节数。
 * @return FB_OK 成功；其他为参数、范围或底层错误。
 * @note 不自动擦除目标区域；调用方必须保证目标可编程。
 */
fboot_status_t fastboot_w25q64_write(uint32_t offset, const uint8_t *data,
                                     size_t len);
/**
 * @brief 阻塞擦除一个 4 KiB 扇区。
 * @param[in] offset 芯片内偏移，必须按 4 KiB 对齐。
 * @return FB_OK 成功；其他为范围或底层错误。
 */
fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset);
/**
 * @brief 使用指定大小的擦除命令并等待完成。
 * @param[in] offset 芯片内起始偏移，至少按 4 KiB 对齐。
 * @param[in] size 擦除块大小，仅支持 4、32、64 KiB。
 * @return FB_OK 成功；FB_ERR_PARAM 大小不支持；其他为范围或底层错误。
 * @note 使用 32/64 KiB 命令时，调用方还应保证对应块对齐和完整块范围有效。
 */
fboot_status_t fastboot_w25q64_erase(uint32_t offset, uint32_t size);
/**
 * @brief 根据对齐和剩余长度选择优先使用的擦除块大小。
 * @param[in] offset 芯片内起始偏移，调用方应保证至少按 4 KiB 对齐。
 * @param[in] remaining 尚需覆盖的字节数。
 * @return 64、32 或 4 KiB；尾部不足 4 KiB 时仍返回 4 KiB。
 * @note 仅计算大小，不访问 Flash，也不校验整个分区边界。
 */
uint32_t fastboot_w25q64_best_erase_size(uint32_t offset,
                                         uint32_t remaining);
/**
 * @brief 擦除与指定字节范围相交的所有 4 KiB 扇区。
 * @param[in] offset 芯片内起始偏移。
 * @param[in] len 请求覆盖的字节数；0 表示不擦除。
 * @return FB_OK 成功；其他为范围或底层错误。
 * @warning 实际擦除范围向扇区边界扩展，分区边界必须按扇区对齐。
 */
fboot_status_t fastboot_w25q64_erase_range(uint32_t offset, size_t len);
/**
 * @brief 获取支持按需擦除的 OTA 暂存写入器。
 * @return 静态 writer 对象的只读指针，调用方不得释放。
 * @note begin 只登记长度；异步模式须持续调用 poll，并保持输入缓冲区有效直至完成。
 */
const fastboot_writer_t *fastboot_w25q64_staging_writer(void);
/**
 * @brief 获取外部 Flash OTA 暂存区域。
 * @return 静态区域对象的只读指针，调用方不得释放。
 * @note 区域包装接口使用区域内相对偏移，回调转换到芯片内偏移。
 */
const fastboot_flash_area_t *fastboot_w25q64_staging_area(void);

/**
 * @brief OTA 异步写入路径的累积性能统计。
 * @note 时间取自毫秒时基并乘以 1000，字段单位为微秒但精度为毫秒。
 * @note 原始同步擦写接口不会更新这些计数。
 */
typedef struct {
    uint32_t erase_count;     /**< @brief 已发出的擦除命令次数。 */
    uint32_t erase_us;        /**< @brief 完成的擦除等待累计时间，单位微秒。 */
    uint32_t program_count;   /**< @brief 已发出的页编程命令次数。 */
    uint32_t program_us;      /**< @brief 完成的编程等待累计时间，单位微秒。 */
    uint32_t busy_poll_count; /**< @brief 擦除/编程等待返回忙状态的次数。 */
} w25q_perf_t;

/**
 * @brief 清零 OTA 异步写入性能计数。
 * @note 应在一次传输开始前调用；不会重置正在执行的写入状态。
 */
void fastboot_w25q64_perf_reset(void);
/**
 * @brief 获取当前性能计数对象。
 * @return 静态统计对象的只读指针；数据会随异步写入继续变化。
 */
const w25q_perf_t *fastboot_w25q64_perf(void);

#endif /* FASTBOOT_W25Q64_H */
