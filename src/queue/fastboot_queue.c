#include "fastboot_queue.h"
#include "fastboot_port.h"

#define PACKET_HEADER  3u
#define PACKET_TRAILER 2u

void fastboot_queue_reset(fboot_queue_t *q)
{
    q->head  = 0u;
    q->tail  = 0u;
    q->count = 0u;
}

bool fastboot_queue_full(const fboot_queue_t *q)
{
    return q->count >= FBOOT_QUEUE_DEPTH;
}

bool fastboot_queue_empty(const fboot_queue_t *q)
{
    return q->count == 0u;
}

fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q)
{
    if (fastboot_queue_full(q)) {
        return NULL;
    }
    q->slots[q->tail].writing = false;
    return &q->slots[q->tail];
}

void fastboot_queue_commit(fboot_queue_t *q)
{
    q->tail = (q->tail + 1u) % FBOOT_QUEUE_DEPTH;
    ++q->count;
}

fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
                                        const fboot_sink_t *sink,
                                        uint32_t *written)
{
    fboot_queue_slot_t *slot;
    fboot_status_t rc;

    if (fastboot_queue_empty(q)) {
        return FB_NO_UPDATE;
    }

    slot = &q->slots[q->head];

    if (slot->writing) {
        /* Async write in progress – poll for completion. */
        rc = sink->poll ? sink->poll(sink->ctx) : FB_OK;
    } else if (sink->write_start && sink->poll && sink->busy) {
        /* Async path available. */
        rc = sink->write_start(sink->ctx, slot->offset,
                               &slot->packet[PACKET_HEADER], slot->len);
        if (rc == FB_BUSY || sink->busy(sink->ctx)) {
            slot->writing = true;
            return FB_BUSY;
        }
    } else {
        /* Synchronous path. */
        rc = sink->write(sink->ctx, slot->offset,
                         &slot->packet[PACKET_HEADER], slot->len);
    }

    if (rc == FB_BUSY) {
        return FB_BUSY;
    }
    if (rc != FB_OK) {
        return rc;
    }
    if (slot->writing && sink->busy && sink->busy(sink->ctx)) {
        return FB_BUSY;
    }

    if (written && *written < slot->offset + slot->len) {
        *written = slot->offset + slot->len;
    }

    q->head = (q->head + 1u) % FBOOT_QUEUE_DEPTH;
    --q->count;
    fastboot_port_feed_watchdog();
    return FB_OK;
}

fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q,
                                        const fboot_sink_t *sink,
                                        uint32_t *written)
{
    while (!fastboot_queue_empty(q)) {
        fboot_status_t rc = fastboot_queue_drain_one(q, sink, written);
        if (rc == FB_BUSY) {
            fastboot_port_feed_watchdog();
            continue;
        }
        if (rc != FB_OK && rc != FB_NO_UPDATE) {
            return rc;
        }
    }
    return FB_OK;
}
