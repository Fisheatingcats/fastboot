#include "fastboot_uart.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>
#include <string.h>

#define UART_RX_DMA_BUF_SIZE 4096u
#define UART_RX_WAIT_STEP_MS 1u

static UART_HandleTypeDef s_huart1;
static DMA_HandleTypeDef s_hdma_usart1_rx;
static bool s_uart_ready;
static uint8_t s_rx_dma_buf[UART_RX_DMA_BUF_SIZE];
static size_t s_rx_tail;

static size_t cstr_len(const char *s)
{
    size_t len = 0u;

    if (!s) {
        return 0u;
    }
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

static size_t dma_rx_head(void)
{
    size_t head = UART_RX_DMA_BUF_SIZE -
                  (size_t)__HAL_DMA_GET_COUNTER(&s_hdma_usart1_rx);

    return head >= UART_RX_DMA_BUF_SIZE ? 0u : head;
}

static size_t dma_rx_available(void)
{
    size_t head = dma_rx_head();

    if (head >= s_rx_tail) {
        return head - s_rx_tail;
    }
    return UART_RX_DMA_BUF_SIZE - s_rx_tail + head;
}

static size_t dma_rx_copy(uint8_t *data, size_t len)
{
    size_t available = dma_rx_available();
    size_t copied = 0u;

    if (len > available) {
        len = available;
    }

    while (copied < len) {
        size_t chunk = UART_RX_DMA_BUF_SIZE - s_rx_tail;
        size_t remaining = len - copied;

        if (chunk > remaining) {
            chunk = remaining;
        }
        memcpy(data + copied, &s_rx_dma_buf[s_rx_tail], chunk);
        s_rx_tail += chunk;
        if (s_rx_tail >= UART_RX_DMA_BUF_SIZE) {
            s_rx_tail = 0u;
        }
        copied += chunk;
    }
    return copied;
}

static const char *status_name(fboot_status_t status)
{
    switch (status) {
    case FB_OK:
        return "OK";
    case FB_NO_UPDATE:
        return "NO_UPDATE";
    case FB_BUSY:
        return "BUSY";
    case FB_ERR_PARAM:
        return "ERR_PARAM";
    case FB_ERR_RANGE:
        return "ERR_RANGE";
    case FB_ERR_FLASH:
        return "ERR_FLASH";
    case FB_ERR_VERIFY:
        return "ERR_VERIFY";
    case FB_ERR_FORMAT:
        return "ERR_FORMAT";
    case FB_ERR_CRC:
        return "ERR_CRC";
    case FB_ERR_IO:
        return "ERR_IO";
    case FB_ERR_ID:
        return "ERR_ID";
    default:
        return "ERR_UNKNOWN";
    }
}

void fastboot_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (s_uart_ready) {
        return;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    s_huart1.Instance = USART1;
    s_huart1.Init.BaudRate = 460800u;
    s_huart1.Init.WordLength = UART_WORDLENGTH_8B;
    s_huart1.Init.StopBits = UART_STOPBITS_1;
    s_huart1.Init.Parity = UART_PARITY_NONE;
    s_huart1.Init.Mode = UART_MODE_TX_RX;
    s_huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart1) != HAL_OK) {
        return;
    }

    s_hdma_usart1_rx.Instance = DMA2_Stream2;
    s_hdma_usart1_rx.Init.Channel = DMA_CHANNEL_4;
    s_hdma_usart1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    s_hdma_usart1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    s_hdma_usart1_rx.Init.MemInc = DMA_MINC_ENABLE;
    s_hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_usart1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_hdma_usart1_rx.Init.Mode = DMA_CIRCULAR;
    s_hdma_usart1_rx.Init.Priority = DMA_PRIORITY_HIGH;
    s_hdma_usart1_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma_usart1_rx) != HAL_OK) {
        return;
    }
    __HAL_LINKDMA(&s_huart1, hdmarx, s_hdma_usart1_rx);

    s_rx_tail = 0u;
    if (HAL_UART_Receive_DMA(&s_huart1, s_rx_dma_buf,
                             (uint16_t)sizeof(s_rx_dma_buf)) != HAL_OK) {
        return;
    }

    s_uart_ready = true;
}

void fastboot_uart_deinit(void)
{
    if (!s_uart_ready) {
        return;
    }

    (void)HAL_UART_DMAStop(&s_huart1);
    (void)HAL_DMA_DeInit(&s_hdma_usart1_rx);
    (void)HAL_UART_DeInit(&s_huart1);

    s_rx_tail = 0u;
    s_uart_ready = false;
}

bool fastboot_uart_rx_byte(uint8_t *out, uint32_t timeout_ms)
{
    return fastboot_uart_read(out, 1u, timeout_ms) == 1u;
}

size_t fastboot_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint32_t start;
    size_t copied;

    fastboot_uart_init();
    if (!s_uart_ready || (!data && len > 0u)) {
        return 0u;
    }
    if (len == 0u) {
        return 0u;
    }

    start = HAL_GetTick();
    do {
        copied = dma_rx_copy(data, len);
        if (copied > 0u) {
            return copied;
        }
        if (timeout_ms == 0u) {
            return 0u;
        }
        HAL_Delay(UART_RX_WAIT_STEP_MS);
    } while ((HAL_GetTick() - start) < timeout_ms);

    return dma_rx_copy(data, len);
}

void fastboot_uart_tx_byte(uint8_t c)
{
    fastboot_uart_init();
    if (!s_uart_ready) {
        return;
    }
    (void)HAL_UART_Transmit(&s_huart1, &c, 1u, 100u);
}

void fastboot_uart_write(const char *s)
{
    size_t len = cstr_len(s);

    fastboot_uart_init();
    if (!s_uart_ready || len == 0u) {
        return;
    }
    (void)HAL_UART_Transmit(&s_huart1, (uint8_t *)s, (uint16_t)len, 500u);
}

void fastboot_uart_puts(const char *s)
{
    fastboot_uart_write(s);
    fastboot_uart_write("\r\n");
}

void fastboot_uart_status(const char *label, fboot_status_t status)
{
    fastboot_uart_write("[FB] ");
    fastboot_uart_write(label);
    fastboot_uart_write(": ");
    fastboot_uart_puts(status_name(status));
}

void fastboot_uart_hex32(const char *label, uint32_t value)
{
    char buf[11] = "0x00000000";
    static const char hex[] = "0123456789ABCDEF";

    for (uint32_t i = 0u; i < 8u; ++i) {
        uint32_t shift = 28u - (i * 4u);
        buf[2u + i] = hex[(value >> shift) & 0xFu];
    }

    fastboot_uart_write("[FB] ");
    fastboot_uart_write(label);
    fastboot_uart_write(": ");
    fastboot_uart_puts(buf);
}

void fastboot_uart_dec32(const char *label, uint32_t value)
{
    char buf[11];
    uint32_t pos = sizeof(buf);

    buf[--pos] = '\0';
    do {
        buf[--pos] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u && pos > 0u);

    fastboot_uart_write("[FB] ");
    fastboot_uart_write(label);
    fastboot_uart_write(": ");
    fastboot_uart_puts(&buf[pos]);
}

static size_t uart_io_read(void *ctx, uint8_t *data, size_t len,
                           uint32_t timeout_ms)
{
    (void)ctx;
    return fastboot_uart_read(data, len, timeout_ms);
}

static void uart_io_write_byte(void *ctx, uint8_t byte)
{
    (void)ctx;
    fastboot_uart_tx_byte(byte);
}

const fboot_io_t *fastboot_uart_io(void)
{
    static const fboot_io_t io = {
        uart_io_read,
        uart_io_write_byte,
        NULL,
    };

    return &io;
}

static void uart_log_puts(void *ctx, const char *s)
{
    (void)ctx;
    fastboot_uart_puts(s);
}

static void uart_log_dec32(void *ctx, const char *label, uint32_t value)
{
    (void)ctx;
    fastboot_uart_dec32(label, value);
}

const fboot_log_t *fastboot_uart_log(void)
{
    static const fboot_log_t log = {
        uart_log_puts,
        uart_log_dec32,
        NULL,
    };

    return &log;
}
