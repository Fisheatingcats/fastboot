#ifndef FASTBOOT_YMODEM_H
#define FASTBOOT_YMODEM_H

#include "fastboot_io.h"
#include "fastboot_sink.h"
#include "fastboot_status.h"
#include <stdint.h>

/**
 * @file fastboot_ymodem.h
 * @brief YMODEM-1K protocol receiver (hardware independent).
 *
 * Receives a firmware image via YMODEM and writes it to a sink.
 * Uses fastboot_queue internally for async flash programming.
 *
 * The caller provides a fastboot_queue_t instance (allocated statically
 * or on the stack) to avoid global state.
 */

typedef struct {
    /* Internal state – do not access directly. */
    void *_priv;
} fastboot_ymodem_ctx_t;

/**
 * Receive a firmware image via YMODEM-1K.
 *
 * @param io    Byte-level I/O (UART wrapper).
 * @param sink  Data sink (flash write target).
 * @param[out] out_size  Total bytes received (may be NULL).
 * @return FB_OK on success, error code otherwise.
 */
fboot_status_t fastboot_ymodem_receive(const fboot_io_t *io,
                                       const fboot_sink_t *sink,
                                       uint32_t *out_size);

#endif /* FASTBOOT_YMODEM_H */
