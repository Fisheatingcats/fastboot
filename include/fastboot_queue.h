#ifndef FASTBOOT_QUEUE_H
#define FASTBOOT_QUEUE_H

#include "fastboot_sink.h"
#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file fastboot_queue.h
 * @brief Asynchronous write queue for streaming data to a sink.
 *
 * Decouples packet reception from flash programming. Packets are queued
 * during YMODEM reception and drained when the sink is ready.
 */

#define FBOOT_QUEUE_DEPTH  8u
#define FBOOT_QUEUE_PACKET_SIZE  1024u
#define FBOOT_QUEUE_HEADER_SIZE  3u
#define FBOOT_QUEUE_BUF_SIZE  (FBOOT_QUEUE_PACKET_SIZE + FBOOT_QUEUE_HEADER_SIZE + 2u)

typedef struct {
    uint8_t  packet[FBOOT_QUEUE_BUF_SIZE];
    uint32_t offset;
    uint32_t len;
    bool     writing;
} fboot_queue_slot_t;

typedef struct {
    fboot_queue_slot_t slots[FBOOT_QUEUE_DEPTH];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} fboot_queue_t;

/** Reset queue to empty state. */
void fastboot_queue_reset(fboot_queue_t *q);

/** Return true if queue is full. */
bool fastboot_queue_full(const fboot_queue_t *q);

/** Return true if queue is empty. */
bool fastboot_queue_empty(const fboot_queue_t *q);

/**
 * Allocate a slot for the next packet.
 * @return Pointer to the slot, or NULL if queue is full.
 */
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q);

/** Commit the last allocated slot (make it visible to drain). */
void fastboot_queue_commit(fboot_queue_t *q);

/**
 * Drain one slot to the sink.
 * @param q      Queue instance.
 * @param sink   Target sink (flash, etc.).
 * @param[out] written  Updated with bytes written (may be NULL).
 * @return FB_OK, FB_BUSY, FB_NO_UPDATE, or error.
 */
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
                                        const fboot_sink_t *sink,
                                        uint32_t *written);

/**
 * Drain all queued slots to the sink (blocking).
 * Calls fastboot_port_feed_watchdog() between slots.
 */
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q,
                                        const fboot_sink_t *sink,
                                        uint32_t *written);

#endif /* FASTBOOT_QUEUE_H */
