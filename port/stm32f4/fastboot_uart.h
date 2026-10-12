/**
 * @file fastboot_uart.h
 * @brief STM32F4 USART1 DMA 接收、协议传输与调试输出接口。
 * @note 使用静态外设对象；同一实例的协议收发与日志输出需由调用方协调。
 */
#ifndef FASTBOOT_UART_H
#define FASTBOOT_UART_H

#include "fastboot_status.h"
#include "fastboot_transport.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 初始化 USART1 与 DMA 环形接收。
 * @note 参考配置为 PA9/PA10、921600 baud、8N1；重复初始化直接返回。
 * @note 此接口不返回初始化错误；初始化失败时读取返回 0，发送不执行。
 */
void fastboot_uart_init(void);
/**
 * @brief 停止 UART DMA 接收并释放 UART/DMA 状态。
 * @note 跳转 App 前由板级装配层调用，避免与 App 外设初始化冲突。
 */
void fastboot_uart_deinit(void);
/**
 * @brief 从 UART 接收一个字节。
 * @param[out] out 输出字节指针。
 * @param[in] timeout_ms 最大等待时间，单位毫秒；0 表示不等待。
 * @return true 收到一个字节；false 无数据、超时或参数无效。
 */
bool fastboot_uart_rx_byte(uint8_t *out, uint32_t timeout_ms);
/**
 * @brief 从 DMA 环形缓冲区读取可用字节。
 * @param[out] data 输出缓冲区，len 大于 0 时必须有效。
 * @param[in] len 缓冲区容量。
 * @param[in] timeout_ms 无数据时的等待上限，单位毫秒；0 表示不等待。
 * @return 实际读取字节数，可小于 len；0 表示无数据或初始化/参数失败。
 * @note 有数据即返回，调用方需要自行处理短读。
 */
size_t fastboot_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms);
/**
 * @brief 阻塞发送一个字节。
 * @param[in] c 待发送字节。
 * @note 使用 HAL 超时发送，此接口不传播发送错误。
 */
void fastboot_uart_tx_byte(uint8_t c);
/**
 * @brief 阻塞发送字符串，不追加换行。
 * @param[in] s 以 NUL 结束的字符串，NULL 或空字符串不发送。
 * @note 字符串长度应能表示为 uint16_t；不传播 HAL 发送错误。
 */
void fastboot_uart_write(const char *s);
/**
 * @brief 输出字符串并追加 CRLF。
 * @param[in] s 以 NUL 结束的字符串；NULL 时仅输出换行。
 */
void fastboot_uart_puts(const char *s);
/**
 * @brief 输出带标签的 FastBoot 状态名称。
 * @param[in] label 日志标签字符串。
 * @param[in] status 要显示的状态码。
 */
void fastboot_uart_status(const char *label, fboot_status_t status);
/**
 * @brief 输出带标签的 32 位十六进制数。
 * @param[in] label 日志标签字符串。
 * @param[in] value 要显示的数值。
 */
void fastboot_uart_hex32(const char *label, uint32_t value);
/**
 * @brief 输出带标签的 32 位十进制数。
 * @param[in] label 日志标签字符串。
 * @param[in] value 要显示的数值。
 */
void fastboot_uart_dec32(const char *label, uint32_t value);
/**
 * @brief 获取 UART 字节传输函数表。
 * @return 静态传输对象的只读指针，调用方不得释放。
 * @note 对象通过 read/write_byte 适配 YMODEM，取得对象不代表硬件已初始化。
 */
const fastboot_transport_t *fastboot_uart_transport(void);

#endif /* FASTBOOT_UART_H */
