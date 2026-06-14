/**
 * @file fastboot_uart.c
 * @brief STM32F4 UART DMA 环形缓冲区驱动
 *
 * 使用 USART1 + DMA2 Stream2 实现环形缓冲区接收：
 * - DMA 循环模式自动接收，CPU 通过 NDTR 寄存器获取写入位置
 * - 环形缓冲区读取：处理回绕分段拷贝
 * - 提供 transport_t 实例供 YMODEM 协议层使用
 */

#include "fastboot_uart.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>
#include <string.h>

/** @brief DMA 接收环形缓冲区大小（字节） */
#define UART_RX_DMA_BUF_SIZE 4096u
/** @brief 轮询等待步长（毫秒） */
#define UART_RX_WAIT_STEP_MS 1u

/** @brief USART1 句柄 */
static UART_HandleTypeDef s_huart1;
/** @brief DMA 接收句柄 */
static DMA_HandleTypeDef s_hdma_usart1_rx;
/** @brief UART 初始化完成标志 */
static bool s_uart_ready;
/** @brief DMA 接收环形缓冲区 */
static uint8_t s_rx_dma_buf[UART_RX_DMA_BUF_SIZE];
/** @brief 软件读指针（tail），DMA 写指针（head）由 NDTR 隐含 */
static size_t s_rx_tail;

/**
 * @brief 简单字符串长度计算（避免依赖 libc strlen）
 *
 * @param s  以 '\0' 结尾的字符串（可为 NULL）
 * @return   字符串长度；NULL 返回 0
 */
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

/**
 * @brief 获取 DMA 接收缓冲区的写入位置（head）
 *
 * 通过 DMA 剩余计数寄存器（NDTR）计算 head：
 *   head = 缓冲区大小 - DMA 剩余计数
 * 当 NDTR 等于缓冲区大小时（DMA 刚回绕），head 归零。
 *
 * @return  当前 head 位置（0 ~ UART_RX_DMA_BUF_SIZE-1）
 */
static size_t dma_rx_head(void)
{
    size_t head = UART_RX_DMA_BUF_SIZE -
                  (size_t)__HAL_DMA_GET_COUNTER(&s_hdma_usart1_rx);

    return head >= UART_RX_DMA_BUF_SIZE ? 0u : head;
}

/**
 * @brief 获取环形缓冲区中可读字节数
 *
 * 处理 head 可能回绕到 tail 之前的情况：
 * - head >= tail：可用 = head - tail
 * - head < tail：可用 = 缓冲区大小 - tail + head
 *
 * @return  可读字节数
 */
static size_t dma_rx_available(void)
{
    size_t head = dma_rx_head();

    if (head >= s_rx_tail) {
        return head - s_rx_tail;
    }
    return UART_RX_DMA_BUF_SIZE - s_rx_tail + head;
}

/**
 * @brief 从环形缓冲区拷贝数据（处理回绕）
 *
 * 从 s_rx_tail 开始拷贝 len 字节到 data。若 tail 到缓冲区末尾
 * 不足 len 字节，分两段拷贝：先拷到末尾，再从头部拷贝剩余。
 *
 * @param data  目标缓冲区
 * @param len   期望拷贝字节数
 * @return      实际拷贝字节数（可能小于 len）
 */
static size_t dma_rx_copy(uint8_t *data, size_t len)
{
    size_t available = dma_rx_available();
    size_t copied = 0u;

    if (len > available) {
        len = available;
    }

    while (copied < len) {
        /* 计算从 tail 到缓冲区末尾的连续可用长度 */
        size_t chunk = UART_RX_DMA_BUF_SIZE - s_rx_tail;
        size_t remaining = len - copied;

        if (chunk > remaining) {
            chunk = remaining;
        }
        memcpy(data + copied, &s_rx_dma_buf[s_rx_tail], chunk);
        s_rx_tail += chunk;
        /* tail 到达缓冲区末尾时回绕到开头 */
        if (s_rx_tail >= UART_RX_DMA_BUF_SIZE) {
            s_rx_tail = 0u;
        }
        copied += chunk;
    }
    return copied;
}

/**
 * @brief 将 fboot_status_t 转换为可读字符串
 *
 * @param status  状态码
 * @return        状态名称字符串
 */
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

/**
 * @brief 初始化 USART1 + DMA2 Stream2 环形接收
 *
 * 配置：
 * - PA9(TX)/PA10(RX)，AF7，921600 波特率，8N1
 * - DMA2 Stream2 Channel4，外设→内存，循环模式
 * - 启动 DMA 接收，目标为 s_rx_dma_buf
 *
 * @note  若已初始化（s_uart_ready 为 true）则直接返回。
 */
void fastboot_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (s_uart_ready) {
        return;
    }

    /* 使能 GPIOA、USART1、DMA2 时钟 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* 配置 PA9(TX)/PA10(RX) 为复用推挽，上拉，高速 */
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* USART1 配置：921600 波特率，8N1，无流控 */
    s_huart1.Instance = USART1;
    s_huart1.Init.BaudRate = 921600u;
    s_huart1.Init.WordLength = UART_WORDLENGTH_8B;
    s_huart1.Init.StopBits = UART_STOPBITS_1;
    s_huart1.Init.Parity = UART_PARITY_NONE;
    s_huart1.Init.Mode = UART_MODE_TX_RX;
    s_huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_huart1) != HAL_OK) {
        return;
    }

    /* DMA2 Stream2 Channel4 配置：外设→内存，循环模式 */
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

    /* 启动 DMA 接收，目标为环形缓冲区 */
    s_rx_tail = 0u;
    if (HAL_UART_Receive_DMA(&s_huart1, s_rx_dma_buf,
                             (uint16_t)sizeof(s_rx_dma_buf)) != HAL_OK) {
        return;
    }

    s_uart_ready = true;
}

/**
 * @brief 反初始化 USART1，释放 DMA 和外设资源
 *
 * 停止 DMA 接收、反初始化 DMA 和 UART，重置状态。
 */
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

/**
 * @brief 接收单个字节
 *
 * @param out         输出字节指针
 * @param timeout_ms  超时时间（毫秒）
 * @return            true 成功；false 超时
 */
bool fastboot_uart_rx_byte(uint8_t *out, uint32_t timeout_ms)
{
    return fastboot_uart_read(out, 1u, timeout_ms) == 1u;
}

/**
 * @brief 从 UART 接收指定字节数（带超时）
 *
 * 轮询 DMA 环形缓冲区，有数据时立即返回已拷贝字节数。
 * 无数据时以 UART_RX_WAIT_STEP_MS 步长等待，直到超时。
 *
 * @param data        目标缓冲区
 * @param len         期望读取字节数
 * @param timeout_ms  超时时间（毫秒），0 表示非阻塞
 * @return            实际读取字节数（0 表示无数据或超时）
 */
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

/**
 * @brief 发送单个字节（阻塞）
 *
 * @param c  待发送字节
 */
void fastboot_uart_tx_byte(uint8_t c)
{
    fastboot_uart_init();
    if (!s_uart_ready) {
        return;
    }
    (void)HAL_UART_Transmit(&s_huart1, &c, 1u, 100u);
}

/**
 * @brief 发送字符串（阻塞）
 *
 * @param s  以 '\0' 结尾的字符串
 */
void fastboot_uart_write(const char *s)
{
    size_t len = cstr_len(s);

    fastboot_uart_init();
    if (!s_uart_ready || len == 0u) {
        return;
    }
    (void)HAL_UART_Transmit(&s_huart1, (uint8_t *)s, (uint16_t)len, 500u);
}

/**
 * @brief 发送字符串并追加换行（"\r\n"）
 *
 * @param s  以 '\0' 结尾的字符串
 */
void fastboot_uart_puts(const char *s)
{
    fastboot_uart_write(s);
    fastboot_uart_write("\r\n");
}

/**
 * @brief 输出带标签的状态信息
 *
 * 格式：[FB] <label>: <status_name>\r\n
 *
 * @param label   标签字符串
 * @param status  状态码
 */
void fastboot_uart_status(const char *label, fboot_status_t status)
{
    fastboot_uart_write("[FB] ");
    fastboot_uart_write(label);
    fastboot_uart_write(": ");
    fastboot_uart_puts(status_name(status));
}

/**
 * @brief 输出带标签的 32 位十六进制值
 *
 * 格式：[FB] <label>: 0x00000000\r\n
 *
 * @param label  标签字符串
 * @param value  待输出的 32 位值
 */
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

/**
 * @brief 输出带标签的 32 位十进制值
 *
 * 格式：[FB] <label>: <decimal>\r\n
 *
 * @param label  标签字符串
 * @param value  待输出的 32 位值
 */
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

/**
 * @brief transport_t 读取回调
 *
 * @param ctx         未使用
 * @param data        目标缓冲区
 * @param len         期望字节数
 * @param timeout_ms  超时时间
 * @return            实际读取字节数
 */
static size_t uart_io_read(void *ctx, uint8_t *data, size_t len,
                           uint32_t timeout_ms)
{
    (void)ctx;
    return fastboot_uart_read(data, len, timeout_ms);
}

/**
 * @brief transport_t 写字节回调
 *
 * @param ctx   未使用
 * @param byte  待发送字节
 */
static void uart_io_write_byte(void *ctx, uint8_t byte)
{
    (void)ctx;
    fastboot_uart_tx_byte(byte);
}

/**
 * @brief 获取 UART 传输接口的 transport_t 实例
 *
 * 返回静态分配的 transport_t，包含 read 和 write_byte 回调。
 * 供 YMODEM 协议层使用。
 *
 * @return  指向静态 transport_t 的常量指针
 */
const fastboot_transport_t *fastboot_uart_transport(void)
{
    static const fastboot_transport_t transport = {
        uart_io_read,
        uart_io_write_byte,
        NULL,
    };

    return &transport;
}
