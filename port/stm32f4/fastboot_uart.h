#ifndef FASTBOOT_UART_H
#define FASTBOOT_UART_H

#include "fastboot_io.h"
#include "fastboot_log.h"
#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void fastboot_uart_init(void);
void fastboot_uart_deinit(void);
bool fastboot_uart_rx_byte(uint8_t *out, uint32_t timeout_ms);
size_t fastboot_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms);
void fastboot_uart_tx_byte(uint8_t c);
void fastboot_uart_write(const char *s);
void fastboot_uart_puts(const char *s);
void fastboot_uart_status(const char *label, fboot_status_t status);
void fastboot_uart_hex32(const char *label, uint32_t value);
void fastboot_uart_dec32(const char *label, uint32_t value);
const fboot_io_t *fastboot_uart_io(void);
const fboot_log_t *fastboot_uart_log(void);

#endif /* FASTBOOT_UART_H */
