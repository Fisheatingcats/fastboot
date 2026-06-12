#include "fastboot_queue.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PACKET_HEADER 3u

static uint32_t s_watchdog_count;

void fastboot_port_feed_watchdog(void)
{
    ++s_watchdog_count;
}

typedef struct {
    uint8_t data[64];
    uint32_t len;
} sync_ctx_t;

static fboot_status_t sync_begin(void *ctx, uint32_t size)
{
    (void)ctx;
    (void)size;
    return FB_OK;
}

static fboot_status_t sync_write(void *ctx, uint32_t offset,
                                 const uint8_t *data, size_t len)
{
    sync_ctx_t *sync = (sync_ctx_t *)ctx;

    assert(offset + len <= sizeof(sync->data));
    memcpy(&sync->data[offset], data, len);
    if (sync->len < offset + len) {
        sync->len = offset + (uint32_t)len;
    }
    return FB_OK;
}

typedef struct {
    uint8_t data[64];
    const uint8_t *pending_data;
    uint32_t pending_offset;
    size_t pending_len;
    bool busy;
} async_ctx_t;

static fboot_status_t async_write_start(void *ctx, uint32_t offset,
                                        const uint8_t *data, size_t len)
{
    async_ctx_t *async = (async_ctx_t *)ctx;

    assert(!async->busy);
    async->pending_offset = offset;
    async->pending_data = data;
    async->pending_len = len;
    async->busy = true;
    return FB_BUSY;
}

static fboot_status_t async_poll(void *ctx)
{
    async_ctx_t *async = (async_ctx_t *)ctx;

    if (!async->busy) {
        return FB_OK;
    }
    assert(async->pending_offset + async->pending_len <= sizeof(async->data));
    memcpy(&async->data[async->pending_offset], async->pending_data,
           async->pending_len);
    async->busy = false;
    return FB_OK;
}

static bool async_busy(void *ctx)
{
    return ((async_ctx_t *)ctx)->busy;
}

static void fill_slot(fboot_queue_slot_t *slot, uint32_t offset,
                      const char *data)
{
    size_t len = strlen(data);

    slot->offset = offset;
    slot->len = (uint32_t)len;
    memcpy(&slot->packet[PACKET_HEADER], data, len);
}

static void test_sync_drain(void)
{
    fboot_queue_t queue;
    sync_ctx_t sync = {0};
    uint32_t written = 0u;
    fboot_queue_slot_t *slot;
    const fboot_sink_t sink = {
        sync_begin,
        sync_write,
        NULL,
        NULL,
        NULL,
        &sync,
    };

    fastboot_queue_reset(&queue);
    assert(fastboot_queue_empty(&queue));
    slot = fastboot_queue_alloc(&queue);
    assert(slot != NULL);
    fill_slot(slot, 4u, "abc");
    fastboot_queue_commit(&queue);
    assert(!fastboot_queue_empty(&queue));

    assert(fastboot_queue_drain_one(&queue, &sink, &written) == FB_OK);
    assert(fastboot_queue_empty(&queue));
    assert(written == 7u);
    assert(memcmp(&sync.data[4], "abc", 3u) == 0);
}

static void test_async_drain_keeps_slot_until_poll_done(void)
{
    fboot_queue_t queue;
    async_ctx_t async = {0};
    uint32_t written = 0u;
    fboot_queue_slot_t *slot;
    const fboot_sink_t sink = {
        NULL,
        NULL,
        async_write_start,
        async_poll,
        async_busy,
        &async,
    };

    fastboot_queue_reset(&queue);
    slot = fastboot_queue_alloc(&queue);
    assert(slot != NULL);
    fill_slot(slot, 0u, "xyz");
    fastboot_queue_commit(&queue);

    assert(fastboot_queue_drain_one(&queue, &sink, &written) == FB_BUSY);
    assert(!fastboot_queue_empty(&queue));
    assert(fastboot_queue_drain_one(&queue, &sink, &written) == FB_OK);
    assert(fastboot_queue_empty(&queue));
    assert(written == 3u);
    assert(memcmp(async.data, "xyz", 3u) == 0);
}

int main(void)
{
    test_sync_drain();
    test_async_drain_keeps_slot_until_poll_done();
    assert(s_watchdog_count >= 2u);
    return 0;
}
