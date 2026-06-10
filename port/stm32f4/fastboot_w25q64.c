#include "fastboot_w25q64.h"
#include "fastboot_memory_map.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>

#define W25Q_CMD_WRITE_ENABLE 0x06u
#define W25Q_CMD_READ_DATA    0x03u
#define W25Q_CMD_READ_ID      0x9Fu
#define W25Q_CMD_READ_STATUS1 0x05u
#define W25Q_CMD_PAGE_PROGRAM 0x02u
#define W25Q_CMD_SECTOR_ERASE 0x20u

#define W25Q_STATUS_BUSY      0x01u
#define W25Q64_JEDEC_MF_ID    0xEFu
#define W25Q64_JEDEC_CAP_ID   0x17u
#define W25Q_SPI_TIMEOUT_MS   100u
#define W25Q_ERASE_TIMEOUT_MS 2000u
#define W25Q_PAGE_SIZE        256u
#define W25Q_SECTOR_SIZE      4096u

static SPI_HandleTypeDef s_hspi2;
static bool s_initialized;
static uint8_t s_jedec_id[3];

typedef struct {
    uint32_t base;
    uint32_t capacity;
    uint32_t active_size;
    uint32_t erased_until;
} w25q_sink_ctx_t;

typedef enum {
    W25Q_ASYNC_IDLE = 0,
    W25Q_ASYNC_WAIT_READY,
    W25Q_ASYNC_ERASE_WRITE_ENABLE,
    W25Q_ASYNC_ERASE_SECTOR,
    W25Q_ASYNC_WAIT_ERASE_DONE,
    W25Q_ASYNC_WRITE_ENABLE,
    W25Q_ASYNC_PROGRAM_PAGE,
    W25Q_ASYNC_WAIT_PROGRAM_DONE,
} w25q_async_state_t;

typedef struct {
    w25q_async_state_t state;
    uint32_t offset;
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t wait_start;
    uint32_t erase_addr;
} w25q_async_writer_t;

static w25q_sink_ctx_t s_ota_sink_ctx = {
    FASTBOOT_EXTFLASH_OTA_OFFSET,
    FASTBOOT_EXTFLASH_OTA_SIZE,
    0u,
    0u,
};
static w25q_async_writer_t s_ota_writer;

static void cs_low(void)
{
    HAL_GPIO_WritePin(PB12_FLASH_SPI_NSS_M_GPIO_Port,
                      PB12_FLASH_SPI_NSS_M_Pin, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(PB12_FLASH_SPI_NSS_M_GPIO_Port,
                      PB12_FLASH_SPI_NSS_M_Pin, GPIO_PIN_SET);
}

static fboot_status_t spi_tx(const uint8_t *data, size_t len)
{
    if (HAL_SPI_Transmit(&s_hspi2, (uint8_t *)data, (uint16_t)len,
                         W25Q_SPI_TIMEOUT_MS) != HAL_OK) {
        return FB_ERR_IO;
    }
    return FB_OK;
}

static fboot_status_t spi_rx(uint8_t *data, size_t len)
{
    if (HAL_SPI_Receive(&s_hspi2, data, (uint16_t)len,
                        W25Q_SPI_TIMEOUT_MS) != HAL_OK) {
        return FB_ERR_IO;
    }
    return FB_OK;
}

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

static fboot_status_t write_enable(void)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    fboot_status_t rc;

    cs_low();
    rc = spi_tx(&cmd, 1u);
    cs_high();
    return rc;
}

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
    s_hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
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

uint32_t fastboot_w25q64_jedec_id(void)
{
    return ((uint32_t)s_jedec_id[0] << 16) |
           ((uint32_t)s_jedec_id[1] << 8) |
           (uint32_t)s_jedec_id[2];
}

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

fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset)
{
    uint8_t cmd[4];
    fboot_status_t rc;

    if ((offset % 4096u) != 0u || offset >= FASTBOOT_EXTFLASH_SIZE) {
        return FB_ERR_RANGE;
    }
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

    cmd[0] = W25Q_CMD_SECTOR_ERASE;
    cmd[1] = (uint8_t)(offset >> 16);
    cmd[2] = (uint8_t)(offset >> 8);
    cmd[3] = (uint8_t)offset;

    cs_low();
    rc = spi_tx(cmd, sizeof(cmd));
    cs_high();
    if (rc != FB_OK) {
        return rc;
    }
    return wait_not_busy(W25Q_ERASE_TIMEOUT_MS);
}

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

static fboot_status_t ota_sink_erase_until(void *ctx, uint32_t end_offset)
{
    w25q_sink_ctx_t *sink = (w25q_sink_ctx_t *)ctx;

    if (!sink || end_offset > sink->active_size) {
        return FB_ERR_RANGE;
    }
    while (sink->erased_until < end_offset) {
        fboot_status_t rc;
        uint32_t erase_offset = sink->base + sink->erased_until;

        rc = fastboot_w25q64_erase_sector(erase_offset);
        if (rc != FB_OK) {
            return rc;
        }
        sink->erased_until += W25Q_SECTOR_SIZE;
        if (sink->erased_until > sink->active_size) {
            sink->erased_until = sink->active_size;
        }
    }
    return FB_OK;
}

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

static bool ota_sink_busy(void *ctx)
{
    (void)ctx;
    return s_ota_writer.state != W25Q_ASYNC_IDLE;
}

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
                s_ota_writer.erase_addr = sink->base + sink->erased_until;
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
            uint8_t cmd[4];

            cmd[0] = W25Q_CMD_SECTOR_ERASE;
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
            s_ota_writer.wait_start = HAL_GetTick();
            s_ota_writer.state = W25Q_ASYNC_WAIT_ERASE_DONE;
            return FB_BUSY;
        }

        case W25Q_ASYNC_WAIT_ERASE_DONE:
            rc = check_not_busy(W25Q_ERASE_TIMEOUT_MS, s_ota_writer.wait_start,
                                &ready);
            if (rc == FB_BUSY) {
                return FB_BUSY;
            }
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
            sink->erased_until += W25Q_SECTOR_SIZE;
            if (sink->erased_until > sink->active_size) {
                sink->erased_until = sink->active_size;
            }
            if ((s_ota_writer.offset - sink->base) +
                    (uint32_t)s_ota_writer.len >
                sink->erased_until) {
                s_ota_writer.erase_addr = sink->base + sink->erased_until;
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
            s_ota_writer.wait_start = HAL_GetTick();
            s_ota_writer.state = W25Q_ASYNC_WAIT_PROGRAM_DONE;
            return FB_BUSY;
        }

        case W25Q_ASYNC_WAIT_PROGRAM_DONE:
            rc = check_not_busy(W25Q_SPI_TIMEOUT_MS, s_ota_writer.wait_start,
                                &ready);
            if (rc == FB_BUSY) {
                return FB_BUSY;
            }
            if (rc != FB_OK) {
                s_ota_writer.state = W25Q_ASYNC_IDLE;
                return rc;
            }
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

const fboot_sink_t *fastboot_w25q64_ota_sink(void)
{
    static const fboot_sink_t sink = {
        ota_sink_begin,
        ota_sink_write,
        ota_sink_write_start,
        ota_sink_poll,
        ota_sink_busy,
        &s_ota_sink_ctx,
    };

    return &sink;
}
