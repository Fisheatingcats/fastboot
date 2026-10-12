/**
 * @file fastboot_w25q64.c
 * @brief W25Q64 SPI2 驱动及 OTA 暂存写入状态机。
 * @note 接收 writer 支持协作式异步擦写；原始 Flash 接口为同步访问。
 */
#include "fastboot_w25q64.h"
#include "fastboot_config.h"
#include "fastboot_memory_map.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <string.h>

#define W25Q_CMD_WRITE_ENABLE 0x06u /**< @brief 写使能命令。 */
#define W25Q_CMD_READ_DATA    0x03u /**< @brief 普通读取命令。 */
#define W25Q_CMD_READ_ID      0x9Fu /**< @brief JEDEC 标识读取命令。 */
#define W25Q_CMD_READ_STATUS1 0x05u /**< @brief 状态寄存器 1 读取命令。 */
#define W25Q_CMD_PAGE_PROGRAM 0x02u /**< @brief 页编程命令。 */
#define W25Q_CMD_SECTOR_ERASE 0x20u /**< @brief 4 KiB 扇区擦除命令。 */
#define W25Q_CMD_BLOCK32_ERASE 0x52u /**< @brief 32 KiB 块擦除命令。 */
#define W25Q_CMD_BLOCK64_ERASE 0xD8u /**< @brief 64 KiB 块擦除命令。 */

#define W25Q_STATUS_BUSY      0x01u /**< @brief 状态寄存器忙标志。 */
#define W25Q64_JEDEC_MF_ID    0xEFu /**< @brief Winbond 厂商标识。 */
#define W25Q64_JEDEC_CAP_ID   0x17u /**< @brief 64 Mbit 容量标识。 */
#define W25Q_SPI_TIMEOUT_MS   100u /**< @brief SPI 事务和编程忙等待上限，单位毫秒。 */
#define W25Q_ERASE_TIMEOUT_MS 2000u /**< @brief 擦除忙等待上限，单位毫秒。 */
#define W25Q_PAGE_SIZE        256u /**< @brief 页编程边界，单位字节。 */
#define W25Q_SECTOR_SIZE      4096u /**< @brief 最小擦除边界，单位字节。 */
#define W25Q_BLOCK32_SIZE     0x8000u /**< @brief 32 KiB 擦除块大小。 */
#define W25Q_BLOCK64_SIZE     0x10000u /**< @brief 64 KiB 擦除块大小。 */

/** @brief SPI2 外设句柄。 */
static SPI_HandleTypeDef s_hspi2;
/** @brief 驱动初始化成功标志。 */
static bool s_initialized;
/** @brief 缓存的三个 JEDEC 标识字节。 */
static uint8_t s_jedec_id[3];
/** @brief 异步写入路径的性能统计。 */
static w25q_perf_t s_perf;

/** @brief 一次暂存接收会话的区域与擦除前缀。 */
typedef struct {
    uint32_t base;         /**< @brief 暂存区在芯片内的起始偏移。 */
    uint32_t capacity;     /**< @brief 暂存区容量。 */
    uint32_t active_size;  /**< @brief 当前接收文件的声明长度。 */
    uint32_t erased_until; /**< @brief 已擦除前缀的结束偏移，可截断到 active_size。 */
} w25q_sink_ctx_t;

/** @brief OTA 写入器的协作调度状态。 */
typedef enum {
    W25Q_ASYNC_IDLE = 0, /**< @brief 无活动请求。 */
    W25Q_ASYNC_WAIT_READY, /**< @brief 等待芯片空闲。 */
    W25Q_ASYNC_ERASE_WRITE_ENABLE, /**< @brief 发送擦除前的写使能。 */
    W25Q_ASYNC_ERASE_SECTOR, /**< @brief 发出选定大小的擦除命令。 */
    W25Q_ASYNC_WAIT_ERASE_DONE, /**< @brief 等待芯片内部擦除完成。 */
    W25Q_ASYNC_WRITE_ENABLE, /**< @brief 发送页编程前的写使能。 */
    W25Q_ASYNC_PROGRAM_PAGE, /**< @brief 发出页编程命令与数据。 */
    W25Q_ASYNC_WAIT_PROGRAM_DONE, /**< @brief 等待芯片内部页编程完成。 */
} w25q_async_state_t;

/**
 * @brief 唯一异步写入请求的状态与进度。
 * @note data 指向队列槽位内存，完成或失败之前不能回收该槽位。
 */
typedef struct {
    w25q_async_state_t state; /**< @brief 当前状态。 */
    uint32_t offset; /**< @brief 当前请求的芯片内起始偏移。 */
    const uint8_t *data; /**< @brief 借用的输入缓冲区，不复制内容。 */
    size_t len; /**< @brief 当前请求总字节数。 */
    size_t pos; /**< @brief 已发出页编程的数据字节数。 */
    uint32_t wait_start; /**< @brief 当前忙等待的开始毫秒时基。 */
    uint32_t erase_addr; /**< @brief 下一次擦除的芯片内起始偏移。 */
    uint32_t erase_size; /**< @brief 下一次擦除命令覆盖的块大小。 */
} w25q_async_writer_t;

/** @brief 唯一暂存接收会话上下文。 */
static w25q_sink_ctx_t s_ota_sink_ctx = {
    FASTBOOT_EXTFLASH_OTA_OFFSET,
    FASTBOOT_CFG_STAGING_CAPACITY,
    0u,
    0u,
};
/** @brief 唯一异步写入请求对象。 */
static w25q_async_writer_t s_ota_writer;

/**
 * @brief 拉低软件片选，开始一次 SPI 命令事务。
 */
static void cs_low(void)
{
    HAL_GPIO_WritePin(PB12_FLASH_SPI_NSS_M_GPIO_Port,
                      PB12_FLASH_SPI_NSS_M_Pin, GPIO_PIN_RESET);
}

/**
 * @brief 拉高软件片选，结束一次 SPI 命令事务。
 */
static void cs_high(void)
{
    HAL_GPIO_WritePin(PB12_FLASH_SPI_NSS_M_GPIO_Port,
                      PB12_FLASH_SPI_NSS_M_Pin, GPIO_PIN_SET);
}

/**
 * @brief 通过 SPI2 阻塞发送命令或数据。
 * @param[in] data 输入缓冲区。
 * @param[in] len 发送字节数，调用方保证不超过 UINT16_MAX。
 * @return FB_OK 成功；FB_ERR_IO HAL 发送失败或超时。
 */
static fboot_status_t spi_tx(const uint8_t *data, size_t len)
{
    if (HAL_SPI_Transmit(&s_hspi2, (uint8_t *)data, (uint16_t)len,
                         W25Q_SPI_TIMEOUT_MS) != HAL_OK) {
        return FB_ERR_IO;
    }
    return FB_OK;
}

/**
 * @brief 通过 SPI2 阻塞接收数据。
 * @param[out] data 输出缓冲区。
 * @param[in] len 接收字节数，调用方保证不超过 UINT16_MAX。
 * @return FB_OK 成功；FB_ERR_IO HAL 接收失败或超时。
 */
static fboot_status_t spi_rx(uint8_t *data, size_t len)
{
    if (HAL_SPI_Receive(&s_hspi2, data, (uint16_t)len,
                        W25Q_SPI_TIMEOUT_MS) != HAL_OK) {
        return FB_ERR_IO;
    }
    return FB_OK;
}

/**
 * @brief 读取状态寄存器 1。
 * @param[out] status 有效的状态字节指针。
 * @return FB_OK 成功；FB_ERR_PARAM 指针为空；其他为 SPI 错误。
 */
static fboot_status_t read_status(uint8_t *status)
{
    uint8_t cmd = W25Q_CMD_READ_STATUS1;
    fboot_status_t rc;

    if (!status) {
        return FB_ERR_PARAM;
    }
    cs_low();
    rc = spi_tx(&cmd, 1u);
    if (rc == FB_OK) {
        rc = spi_rx(status, 1u);
    }
    cs_high();
    return rc;
}

/**
 * @brief 阻塞轮询 BUSY 位直到空闲或超时。
 * @param[in] timeout_ms 等待上限，单位毫秒。
 * @return FB_OK 已空闲；FB_ERR_IO 超时；其他为读取错误。
 * @note 同步路径使用此函数，循环期间不会返回主协议处理。
 */
static fboot_status_t wait_not_busy(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t status = 0u;

    do {
        fboot_status_t rc = read_status(&status);
        if (rc != FB_OK) {
            return rc;
        }
        if ((status & W25Q_STATUS_BUSY) == 0u) {
            return FB_OK;
        }
    } while ((HAL_GetTick() - start) < timeout_ms);

    return FB_ERR_IO;
}

/**
 * @brief 检查一次 BUSY 位并判断等待是否超时。
 * @param[in] timeout_ms 等待上限，单位毫秒。
 * @param[in] wait_start 本次等待开始时的 HAL 毫秒时基。
 * @param[out] ready 状态为空闲时置 true，仍忙时置 false；错误时不保证更新。
 * @return FB_OK 已空闲；FB_BUSY 仍忙；负值为超时或读取错误。
 * @note 异步路径调用一次即返回，不在芯片忙期间自旋等待。
 */
static fboot_status_t check_not_busy(uint32_t timeout_ms, uint32_t wait_start,
                                      bool *ready)
{
    uint8_t status = 0u;
    fboot_status_t rc;

    if (!ready) {
        return FB_ERR_PARAM;
    }
    rc = read_status(&status);
    if (rc != FB_OK) {
        return rc;
    }
    if ((status & W25Q_STATUS_BUSY) == 0u) {
        *ready = true;
        return FB_OK;
    }
    if ((HAL_GetTick() - wait_start) >= timeout_ms) {
        return FB_ERR_IO;
    }
    *ready = false;
    return FB_BUSY;
}

/**
 * @brief 发出写使能命令。
 * @return FB_OK 命令发送成功；其他为 SPI 错误。
 * @note 发送成功不代表后续擦写已经完成。
 */
static fboot_status_t write_enable(void)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    fboot_status_t rc;

    cs_low();
    rc = spi_tx(&cmd, 1u);
    cs_high();
    return rc;
}

/**
 * @brief 初始化 SPI2、片选 GPIO 并检查 JEDEC 厂商和容量标识。
 * @return FB_OK 成功或已初始化；FB_ERR_IO HAL/SPI 失败；FB_ERR_ID 标识不匹配。
 */
fboot_status_t fastboot_w25q64_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t cmd = W25Q_CMD_READ_ID;
    uint8_t id[3] = {0};
    fboot_status_t rc;

    if (s_initialized) {
        return FB_OK;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = PB12_FLASH_SPI_NSS_M_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(PB12_FLASH_SPI_NSS_M_GPIO_Port, &gpio);
    cs_high();

    s_hspi2.Instance = SPI2;
    s_hspi2.Init.Mode = SPI_MODE_MASTER;
    s_hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    s_hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    s_hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_hspi2.Init.NSS = SPI_NSS_SOFT;
    s_hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    s_hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    s_hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    s_hspi2.Init.CRCPolynomial = 10u;
    if (HAL_SPI_Init(&s_hspi2) != HAL_OK) {
        return FB_ERR_IO;
    }

    cs_low();
    rc = spi_tx(&cmd, 1u);
    if (rc == FB_OK) {
        rc = spi_rx(id, sizeof(id));
    }
    cs_high();
    if (rc != FB_OK) {
        return rc;
    }
    s_jedec_id[0] = id[0];
    s_jedec_id[1] = id[1];
    s_jedec_id[2] = id[2];
    if (id[0] != W25Q64_JEDEC_MF_ID || id[2] != W25Q64_JEDEC_CAP_ID) {
        return FB_ERR_ID;
    }

    s_initialized = true;
    return FB_OK;
}

/**
 * @brief 读取缓存的 24 位 JEDEC 标识。
 * @return 厂商字节、类型字节和容量字节拼接的值。
 * @note 本函数不访问 SPI，也不触发初始化；应在 init 成功后调用。
 */
uint32_t fastboot_w25q64_jedec_id(void)
{
    return ((uint32_t)s_jedec_id[0] << 16) |
           ((uint32_t)s_jedec_id[1] << 8) |
           (uint32_t)s_jedec_id[2];
}

/**
 * @brief 阻塞读取外部 Flash。
 * @param[in] offset 芯片内绝对字节偏移。
 * @param[out] data 输出缓冲区，len 大于 0 时必须有效。
 * @param[in] len 字节数，单次不能超过 UINT16_MAX。
 * @return FB_OK 成功；FB_ERR_PARAM/FB_ERR_RANGE 参数或范围错误；其他为底层错误。
 */
fboot_status_t fastboot_w25q64_read(uint32_t offset, uint8_t *data,
                                           size_t len)
{
    uint8_t cmd[4];
    fboot_status_t rc;

    if (!data && len > 0u) {
        return FB_ERR_PARAM;
    }
    if (offset > FASTBOOT_EXTFLASH_SIZE ||
        len > (FASTBOOT_EXTFLASH_SIZE - offset) ||
        len > UINT16_MAX) {
        return FB_ERR_RANGE;
    }
    if (!s_initialized) {
        rc = fastboot_w25q64_init();
        if (rc != FB_OK) {
            return rc;
        }
    }

    cmd[0] = W25Q_CMD_READ_DATA;
    cmd[1] = (uint8_t)(offset >> 16);
    cmd[2] = (uint8_t)(offset >> 8);
    cmd[3] = (uint8_t)offset;

    cs_low();
    rc = spi_tx(cmd, sizeof(cmd));
    if (rc == FB_OK) {
        rc = spi_rx(data, len);
    }
    cs_high();
    return rc;
}

/**
 * @brief 按 256 字节页边界分块编程外部 Flash，并等待完成。
 * @param[in] offset 芯片内绝对字节偏移。
 * @param[in] data 输入缓冲区，len 大于 0 时必须有效。
 * @param[in] len 写入字节数。
 * @return FB_OK 成功；其他为参数、范围或底层错误。
 * @note 不自动擦除目标区域；调用方必须保证目标可编程。
 */
fboot_status_t fastboot_w25q64_write(uint32_t offset,
                                            const uint8_t *data, size_t len)
{
    fboot_status_t rc;

    if (!data && len > 0u) {
        return FB_ERR_PARAM;
    }
    if (offset > FASTBOOT_EXTFLASH_SIZE ||
        len > (FASTBOOT_EXTFLASH_SIZE - offset)) {
        return FB_ERR_RANGE;
    }
    if (!s_initialized) {
        rc = fastboot_w25q64_init();
        if (rc != FB_OK) {
            return rc;
        }
    }

    while (len > 0u) {
        uint8_t cmd[4];
        size_t page_room = W25Q_PAGE_SIZE - (offset % W25Q_PAGE_SIZE);
        size_t chunk = len < page_room ? len : page_room;

        if (chunk > UINT16_MAX) {
            chunk = UINT16_MAX;
        }

        rc = wait_not_busy(W25Q_SPI_TIMEOUT_MS);
        if (rc != FB_OK) {
            return rc;
        }
        rc = write_enable();
        if (rc != FB_OK) {
            return rc;
        }

        cmd[0] = W25Q_CMD_PAGE_PROGRAM;
        cmd[1] = (uint8_t)(offset >> 16);
        cmd[2] = (uint8_t)(offset >> 8);
        cmd[3] = (uint8_t)offset;

        cs_low();
        rc = spi_tx(cmd, sizeof(cmd));
        if (rc == FB_OK) {
            rc = spi_tx(data, chunk);
        }
        cs_high();
        if (rc != FB_OK) {
            return rc;
        }

        offset += (uint32_t)chunk;
        data += chunk;
        len -= chunk;
    }
    return wait_not_busy(W25Q_SPI_TIMEOUT_MS);
}

/**
 * @brief 发送带 24 位地址的擦除命令并等待完成。
 * @param[in] offset 芯片内字节偏移。
 * @param[in] cmd 4/32/64 KiB 擦除命令。
 * @return FB_OK 完成；其他为初始化、SPI 或忙等待错误。
 * @note 调用方负责地址、擦除大小和分区边界的合法性。
 */
static fboot_status_t erase_cmd(uint32_t offset, uint8_t cmd)
{
    uint8_t buf[4];
    fboot_status_t rc;

    if (!s_initialized) {
        rc = fastboot_w25q64_init();
        if (rc != FB_OK) {
            return rc;
        }
    }
    rc = wait_not_busy(W25Q_SPI_TIMEOUT_MS);
    if (rc != FB_OK) {
        return rc;
    }
    rc = write_enable();
    if (rc != FB_OK) {
        return rc;
    }

    buf[0] = cmd;
    buf[1] = (uint8_t)(offset >> 16);
    buf[2] = (uint8_t)(offset >> 8);
    buf[3] = (uint8_t)offset;

    cs_low();
    rc = spi_tx(buf, sizeof(buf));
    cs_high();
    if (rc != FB_OK) {
        return rc;
    }
    return wait_not_busy(W25Q_ERASE_TIMEOUT_MS);
}

/**
 * @brief 根据对齐和剩余长度选择优先使用的擦除块大小。
 * @param[in] offset 芯片内起始偏移，调用方应保证至少按 4 KiB 对齐。
 * @param[in] remaining 尚需覆盖的字节数。
 * @return 64、32 或 4 KiB；尾部不足 4 KiB 时仍返回 4 KiB。
 * @note 仅计算大小，不访问 Flash，也不校验整个分区边界。
 */
uint32_t fastboot_w25q64_best_erase_size(uint32_t offset, uint32_t remaining)
{
    if (remaining >= W25Q_BLOCK64_SIZE &&
        (offset % W25Q_BLOCK64_SIZE) == 0u) {
        return W25Q_BLOCK64_SIZE;
    }
    if (remaining >= W25Q_BLOCK32_SIZE &&
        (offset % W25Q_BLOCK32_SIZE) == 0u) {
        return W25Q_BLOCK32_SIZE;
    }
    return W25Q_SECTOR_SIZE;
}

/**
 * @brief 使用指定大小的擦除命令并等待完成。
 * @param[in] offset 芯片内起始偏移，至少按 4 KiB 对齐。
 * @param[in] size 擦除块大小，仅支持 4、32、64 KiB。
 * @return FB_OK 成功；FB_ERR_PARAM 大小不支持；其他为范围或底层错误。
 * @note 使用 32/64 KiB 命令时，调用方还应保证对应块对齐和完整块范围有效。
 */
fboot_status_t fastboot_w25q64_erase(uint32_t offset, uint32_t size)
{
    uint8_t cmd;

    if ((offset % W25Q_SECTOR_SIZE) != 0u || offset >= FASTBOOT_EXTFLASH_SIZE) {
        return FB_ERR_RANGE;
    }
    switch (size) {
    case W25Q_BLOCK64_SIZE:
        cmd = W25Q_CMD_BLOCK64_ERASE;
        break;
    case W25Q_BLOCK32_SIZE:
        cmd = W25Q_CMD_BLOCK32_ERASE;
        break;
    case W25Q_SECTOR_SIZE:
        cmd = W25Q_CMD_SECTOR_ERASE;
        break;
    default:
        return FB_ERR_PARAM;
    }
    return erase_cmd(offset, cmd);
}

/**
 * @brief 阻塞擦除一个 4 KiB 扇区。
 * @param[in] offset 芯片内偏移，必须按 4 KiB 对齐。
 * @return FB_OK 成功；其他为范围或底层错误。
 */
fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset)
{
    return fastboot_w25q64_erase(offset, W25Q_SECTOR_SIZE);
}

/**
 * @brief 擦除与指定字节范围相交的所有 4 KiB 扇区。
 * @param[in] offset 芯片内起始偏移。
 * @param[in] len 请求覆盖的字节数；0 表示不擦除。
 * @return FB_OK 成功；其他为范围或底层错误。
 * @warning 实际擦除范围向扇区边界扩展，分区边界必须按扇区对齐。
 */
fboot_status_t fastboot_w25q64_erase_range(uint32_t offset, size_t len)
{
    uint32_t start;
    uint32_t end;

    if (len == 0u) {
        return FB_OK;
    }
    if (offset > FASTBOOT_EXTFLASH_SIZE ||
        len > (FASTBOOT_EXTFLASH_SIZE - offset)) {
        return FB_ERR_RANGE;
    }

    start = offset - (offset % W25Q_SECTOR_SIZE);
    end = offset + (uint32_t)len;
    end = (end + W25Q_SECTOR_SIZE - 1u) & ~(W25Q_SECTOR_SIZE - 1u);

    for (uint32_t addr = start; addr < end; addr += W25Q_SECTOR_SIZE) {
        fboot_status_t rc = fastboot_w25q64_erase_sector(addr);
        if (rc != FB_OK) {
            return rc;
        }
    }
    return FB_OK;
}

/**
 * @brief 按需扩展已擦除的暂存区前缀。
 * @param[in] ctx 暂存写入上下文。
 * @param[in] end_offset 必须已擦除的区域内结束偏移，不含该字节。
 * @return FB_OK 完成；FB_ERR_RANGE 上下文或范围无效；其他为擦除错误。
 * @note 同步路径优先选择对齐的 64/32 KiB 块，尾部使用 4 KiB。
 */
static fboot_status_t ota_sink_erase_until(void *ctx, uint32_t end_offset)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;

    if (!sink || end_offset > sink->active_size) {
        return FB_ERR_RANGE;
    }
    while (sink->erased_until < end_offset) {
        fboot_status_t rc;
        uint32_t abs_offset = sink->base + sink->erased_until;
        uint32_t remaining = sink->active_size - sink->erased_until;
        uint32_t erase_sz = fastboot_w25q64_best_erase_size(abs_offset,
                                                            remaining);

        rc = fastboot_w25q64_erase(abs_offset, erase_sz);
        if (rc != FB_OK) {
            return rc;
        }
        sink->erased_until += erase_sz;
        if (sink->erased_until > sink->active_size) {
            sink->erased_until = sink->active_size;
        }
    }
    return FB_OK;
}

/**
 * @brief 登记接收文件长度并重置写入状态。
 * @param[in] ctx 暂存写入上下文。
 * @param[in] size 文件总长度，包含 FWOT 包头。
 * @return FB_OK 就绪；FB_ERR_RANGE 上下文、长度或容量无效。
 * @note 不立即擦除 Flash，也不初始化 SPI；只能在前一写入已结束后开启新会话。
 */
static fboot_status_t ota_sink_begin(void *ctx, uint32_t size)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;

    if (!sink || size == 0u || size > sink->capacity) {
        return FB_ERR_RANGE;
    }
    s_ota_writer.state = W25Q_ASYNC_IDLE;
    sink->active_size = size;
    sink->erased_until = 0u;
    return FB_OK;
}

/**
 * @brief 同步擦除必要区域并写入接收数据。
 * @param[in] ctx 暂存写入上下文。
 * @param[in] offset 暂存区内偏移。
 * @param[in] data 输入缓冲区。
 * @param[in] len 输入字节数。
 * @return FB_OK 写入完成；其他为参数、范围或 Flash 错误。
 */
static fboot_status_t ota_sink_write(void *ctx, uint32_t offset,
                                      const uint8_t *data, size_t len)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;
    fboot_status_t rc;

    if (!sink || (!data && len > 0u)) {
        return FB_ERR_PARAM;
    }
    if (offset > sink->active_size || len > (sink->active_size - offset)) {
        return FB_ERR_RANGE;
    }
    rc = ota_sink_erase_until(ctx, offset + (uint32_t)len);
    if (rc != FB_OK) {
        return rc;
    }
    return fastboot_w25q64_write(sink->base + offset, data, len);
}

/**
 * @brief 查询唯一 OTA 异步写入器是否处于活动状态。
 * @param[in] ctx 接口上下文，当前实现未使用。
 * @return true 尚未结束；false 当前空闲。
 */
static bool ota_sink_busy(void *ctx)
{
    (void)ctx;
    return s_ota_writer.state != W25Q_ASYNC_IDLE;
}

/**
 * @brief 推进按需擦除和分页编程的协作状态机。
 * @param[in] ctx 暂存写入上下文。
 * @return FB_OK 当前请求完成或空闲；FB_BUSY 需继续推进；负值为失败。
 * @note SPI 命令传输仍为短时阻塞；芯片内部擦写等待通过返回 FB_BUSY 让出 CPU。
 */
static fboot_status_t ota_sink_poll(void *ctx)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;

    if (!sink) {
        return FB_ERR_PARAM;
    }
    if (s_ota_writer.state == W25Q_ASYNC_IDLE) {
        return FB_OK;
    }
    if (!s_initialized) {
        fboot_status_t rc = fastboot_w25q64_init();
        if (rc != FB_OK) {
            s_ota_writer.state = W25Q_ASYNC_IDLE;
            return rc;
        }
    }

    while (s_ota_writer.state != W25Q_ASYNC_IDLE) {
        fboot_status_t rc;
        bool ready = false;

        switch (s_ota_writer.state) {
        case W25Q_ASYNC_WAIT_READY:
            rc = check_not_busy(W25Q_SPI_TIMEOUT_MS, s_ota_writer.wait_start,
                                &ready);
            if (rc == FB_BUSY) {
                return FB_BUSY;
            }
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            if ((s_ota_writer.offset - sink->base) +
                    (uint32_t)s_ota_writer.len >
                sink->erased_until) {
                uint32_t abs = sink->base + sink->erased_until;
                uint32_t rem = sink->active_size - sink->erased_until;
                s_ota_writer.erase_addr = abs;
                s_ota_writer.erase_size =
                    fastboot_w25q64_best_erase_size(abs, rem);
                s_ota_writer.state = W25Q_ASYNC_ERASE_WRITE_ENABLE;
            } else {
                s_ota_writer.state = W25Q_ASYNC_WRITE_ENABLE;
            }
            break;

        case W25Q_ASYNC_ERASE_WRITE_ENABLE:
            rc = write_enable();
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            s_ota_writer.state = W25Q_ASYNC_ERASE_SECTOR;
            break;

        case W25Q_ASYNC_ERASE_SECTOR: {
            uint8_t erase_cmd;

            switch (s_ota_writer.erase_size) {
            case W25Q_BLOCK64_SIZE:
                erase_cmd = W25Q_CMD_BLOCK64_ERASE;
                break;
            case W25Q_BLOCK32_SIZE:
                erase_cmd = W25Q_CMD_BLOCK32_ERASE;
                break;
            default:
                erase_cmd = W25Q_CMD_SECTOR_ERASE;
                break;
            }

            uint8_t cmd[4];
            cmd[0] = erase_cmd;
            cmd[1] = (uint8_t)(s_ota_writer.erase_addr >> 16);
            cmd[2] = (uint8_t)(s_ota_writer.erase_addr >> 8);
            cmd[3] = (uint8_t)s_ota_writer.erase_addr;

            cs_low();
            rc = spi_tx(cmd, sizeof(cmd));
            cs_high();
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            ++s_perf.erase_count;
            s_ota_writer.wait_start = HAL_GetTick();
            s_ota_writer.state = W25Q_ASYNC_WAIT_ERASE_DONE;
            return FB_BUSY;
        }

        case W25Q_ASYNC_WAIT_ERASE_DONE:
            rc = check_not_busy(W25Q_ERASE_TIMEOUT_MS, s_ota_writer.wait_start,
                                &ready);
            if (rc == FB_BUSY) {
                ++s_perf.busy_poll_count;
                return FB_BUSY;
            }
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            s_perf.erase_us += (HAL_GetTick() - s_ota_writer.wait_start) * 1000u;
            sink->erased_until += s_ota_writer.erase_size;
            if (sink->erased_until > sink->active_size) {
                sink->erased_until = sink->active_size;
            }
            /* If current write still needs more erases, continue erasing.
             * Otherwise, go to program. */
            if ((s_ota_writer.offset - sink->base) +
                    (uint32_t)s_ota_writer.len >
                sink->erased_until) {
                uint32_t abs = sink->base + sink->erased_until;
                uint32_t rem = sink->active_size - sink->erased_until;
                s_ota_writer.erase_addr = abs;
                s_ota_writer.erase_size =
                    fastboot_w25q64_best_erase_size(abs, rem);
                s_ota_writer.state = W25Q_ASYNC_ERASE_WRITE_ENABLE;
            } else {
                s_ota_writer.state = W25Q_ASYNC_WRITE_ENABLE;
            }
            break;

        case W25Q_ASYNC_WRITE_ENABLE:
            rc = write_enable();
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            s_ota_writer.state = W25Q_ASYNC_PROGRAM_PAGE;
            break;

        case W25Q_ASYNC_PROGRAM_PAGE: {
            uint32_t addr = s_ota_writer.offset + (uint32_t)s_ota_writer.pos;
            size_t page_room = W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE);
            size_t remaining = s_ota_writer.len - s_ota_writer.pos;
            size_t chunk = remaining < page_room ? remaining : page_room;
            uint8_t cmd[4];

            if (chunk > UINT16_MAX) {
                chunk = UINT16_MAX;
            }
            cmd[0] = W25Q_CMD_PAGE_PROGRAM;
            cmd[1] = (uint8_t)(addr >> 16);
            cmd[2] = (uint8_t)(addr >> 8);
            cmd[3] = (uint8_t)addr;

            cs_low();
            rc = spi_tx(cmd, sizeof(cmd));
            if (rc == FB_OK) {
                rc = spi_tx(&s_ota_writer.data[s_ota_writer.pos], chunk);
            }
            cs_high();
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            s_ota_writer.pos += chunk;
            ++s_perf.program_count;
            s_ota_writer.wait_start = HAL_GetTick();
            s_ota_writer.state = W25Q_ASYNC_WAIT_PROGRAM_DONE;
            return FB_BUSY;
        }

        case W25Q_ASYNC_WAIT_PROGRAM_DONE:
            rc = check_not_busy(W25Q_SPI_TIMEOUT_MS, s_ota_writer.wait_start,
                                &ready);
            if (rc == FB_BUSY) {
                ++s_perf.busy_poll_count;
                return FB_BUSY;
            }
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            s_perf.program_us +=
                (HAL_GetTick() - s_ota_writer.wait_start) * 1000u;
            if (s_ota_writer.pos >= s_ota_writer.len) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return FB_OK;
            }
            s_ota_writer.state = W25Q_ASYNC_WRITE_ENABLE;
            break;

        case W25Q_ASYNC_IDLE:
        default:
            return FB_OK;
        }
    }

    return FB_OK;
}

/**
 * @brief 保存输入缓冲区并启动一次异步写入请求。
 * @param[in] ctx 暂存写入上下文。
 * @param[in] offset 暂存区内偏移。
 * @param[in] data 输入缓冲区，操作结束前必须保持有效且不修改。
 * @param[in] len 输入字节数。
 * @return FB_OK 已完成；FB_BUSY 原请求仍忙或新请求待推进；负值为错误。
 * @note 不复制输入数据，保存指针后立即调用一次 poll；队列需保证请求串行。
 */
static fboot_status_t ota_sink_write_start(void *ctx, uint32_t offset,
                                            const uint8_t *data, size_t len)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;

    if (!sink || (!data && len > 0u)) {
        return FB_ERR_PARAM;
    }
    if (ota_sink_busy(ctx)) {
        return FB_BUSY;
    }
    if (offset > sink->active_size || len > (sink->active_size - offset)) {
        return FB_ERR_RANGE;
    }
    if (len == 0u) {
        return FB_OK;
    }

    s_ota_writer.offset = sink->base + offset;
    s_ota_writer.data = data;
    s_ota_writer.len = len;
    s_ota_writer.pos = 0u;
    s_ota_writer.wait_start = HAL_GetTick();
    s_ota_writer.state = W25Q_ASYNC_WAIT_READY;
    return ota_sink_poll(ctx);
}

/**
 * @brief 获取支持按需擦除的 OTA 暂存写入器。
 * @return 静态 writer 对象的只读指针，调用方不得释放。
 * @note begin 只登记长度；异步模式须持续调用 poll，并保持输入缓冲区有效直至完成。
 */
const fastboot_writer_t *fastboot_w25q64_staging_writer(void)
{
    static const fastboot_writer_t writer = {
        ota_sink_begin,
        ota_sink_write,
        ota_sink_write_start,
        ota_sink_poll,
        ota_sink_busy,
        &s_ota_sink_ctx,
    };

    return &writer;
}

/**
 * @brief 适配 Flash 区域读取回调。
 * @param[in] ctx 接口上下文，未使用。
 * @param[in] offset 包装层转换后的芯片内偏移。
 * @param[out] data 输出缓冲区。
 * @param[in] len 读取字节数。
 * @return fastboot_w25q64_read 的返回状态。
 */
static fboot_status_t w25q_area_read(void *ctx, uint32_t offset, uint8_t *data,
                                     size_t len)
{
    (void)ctx;
    return fastboot_w25q64_read(offset, data, len);
}

/**
 * @brief 适配 Flash 区域同步写入回调。
 * @param[in] ctx 接口上下文，未使用。
 * @param[in] offset 包装层转换后的芯片内偏移。
 * @param[in] data 输入缓冲区。
 * @param[in] len 写入字节数。
 * @return fastboot_w25q64_write 的返回状态。
 * @note 不自动擦除，与 OTA writer 的按需擦除行为不同。
 */
static fboot_status_t w25q_area_write(void *ctx, uint32_t offset,
                                      const uint8_t *data, size_t len)
{
    (void)ctx;
    return fastboot_w25q64_write(offset, data, len);
}

/**
 * @brief 适配 Flash 区域扇区擦除回调。
 * @param[in] ctx 接口上下文，未使用。
 * @param[in] offset 包装层转换后的芯片内偏移。
 * @param[in] len 请求覆盖的字节数。
 * @return fastboot_w25q64_erase_range 的返回状态。
 * @note 底层向 4 KiB 边界扩展，擦除 1 字节也会擦除其所在扇区。
 */
static fboot_status_t w25q_area_erase(void *ctx, uint32_t offset,
                                      uint32_t len)
{
    (void)ctx;
    return fastboot_w25q64_erase_range(offset, len);
}

/**
 * @brief 获取外部 Flash OTA 暂存区域。
 * @return 静态区域对象的只读指针，调用方不得释放。
 * @note 区域包装接口使用区域内相对偏移，回调转换到芯片内偏移。
 */
const fastboot_flash_area_t *fastboot_w25q64_staging_area(void)
{
    static const fastboot_flash_ops_t ops = {
        w25q_area_read,
        w25q_area_write,
        w25q_area_erase,
    };
    static const fastboot_flash_area_t area = {
        FASTBOOT_EXTFLASH_OTA_OFFSET,
        FASTBOOT_CFG_STAGING_CAPACITY,
        &ops,
        NULL,
    };

    return &area;
}

/**
 * @brief 清零 OTA 异步写入性能计数。
 * @note 应在一次传输开始前调用；不会重置正在执行的写入状态。
 */
void fastboot_w25q64_perf_reset(void)
{
    memset(&s_perf, 0, sizeof(s_perf));
}

/**
 * @brief 获取当前性能计数对象。
 * @return 静态统计对象的只读指针；数据会随异步写入继续变化。
 */
const w25q_perf_t *fastboot_w25q64_perf(void)
{
    return &s_perf;
}
