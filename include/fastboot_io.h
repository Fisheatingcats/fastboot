#ifndef FASTBOOT_IO_H
#define FASTBOOT_IO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t (*read)(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms);
    void (*write_byte)(void *ctx, uint8_t byte);
    void *ctx;
} fboot_io_t;

#endif /* FASTBOOT_IO_H */
