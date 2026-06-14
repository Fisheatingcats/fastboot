#include "fastboot_ymodem.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SOH       0x01u
#define STX       0x02u
#define EOT       0x04u
#define ACK       0x06u
#define CRC16_CH  0x43u
#define CPMEOF    0x1Au

#define PACKET_HEADER   3u
#define PACKET_SIZE     128u
#define PACKET_1K_SIZE  1024u
#define PACKET_TRAILER  2u

static uint32_t s_tick;
static uint32_t s_watchdog_count;

static uint32_t runtime_tick_ms(void *ctx)
{
    (void)ctx;
    return s_tick++;
}

static void runtime_feed_watchdog(void *ctx)
{
    (void)ctx;
    ++s_watchdog_count;
}

static const fastboot_runtime_t s_runtime = {
    runtime_tick_ms,
    runtime_feed_watchdog,
    NULL,
};

typedef struct {
    const uint8_t *input;
    size_t input_len;
    size_t input_pos;
    uint8_t output[32];
    size_t output_len;
} fake_io_ctx_t;

typedef struct {
    uint8_t data[32];
    uint32_t begin_size;
    uint32_t written;
} fake_sink_ctx_t;

static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;

    while (len-- > 0u) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }
    return crc;
}

static void make_packet(uint8_t start, uint8_t seq, const uint8_t *payload,
                        size_t payload_len, uint8_t pad, uint8_t *out,
                        size_t *out_len)
{
    size_t packet_size = start == STX ? PACKET_1K_SIZE : PACKET_SIZE;
    uint16_t crc;

    assert(payload_len <= packet_size);
    out[0] = start;
    out[1] = seq;
    out[2] = (uint8_t)~seq;
    memset(&out[PACKET_HEADER], pad, packet_size);
    if (payload_len > 0u) {
        memcpy(&out[PACKET_HEADER], payload, payload_len);
    }
    crc = crc16_xmodem(&out[PACKET_HEADER], packet_size);
    out[PACKET_HEADER + packet_size] = (uint8_t)(crc >> 8);
    out[PACKET_HEADER + packet_size + 1u] = (uint8_t)crc;
    *out_len = PACKET_HEADER + packet_size + PACKET_TRAILER;
}

static size_t fake_read(void *ctx, uint8_t *data, size_t len,
                        uint32_t timeout_ms)
{
    fake_io_ctx_t *io = (fake_io_ctx_t *)ctx;
    size_t available;

    (void)timeout_ms;
    if (io->input_pos >= io->input_len) {
        return 0u;
    }
    available = io->input_len - io->input_pos;
    if (len > available) {
        len = available;
    }
    memcpy(data, &io->input[io->input_pos], len);
    io->input_pos += len;
    return len;
}

static void fake_write_byte(void *ctx, uint8_t byte)
{
    fake_io_ctx_t *io = (fake_io_ctx_t *)ctx;

    assert(io->output_len < sizeof(io->output));
    io->output[io->output_len++] = byte;
}

static fboot_status_t fake_begin(void *ctx, uint32_t size)
{
    fake_sink_ctx_t *sink = (fake_sink_ctx_t *)ctx;

    sink->begin_size = size;
    return FB_OK;
}

static fboot_status_t fake_write(void *ctx, uint32_t offset,
                                 const uint8_t *data, size_t len)
{
    fake_sink_ctx_t *sink = (fake_sink_ctx_t *)ctx;

    assert(offset + len <= sizeof(sink->data));
    memcpy(&sink->data[offset], data, len);
    if (sink->written < offset + len) {
        sink->written = offset + (uint32_t)len;
    }
    return FB_OK;
}

static void append(uint8_t *dst, size_t *dst_len, const uint8_t *src,
                   size_t src_len)
{
    memcpy(&dst[*dst_len], src, src_len);
    *dst_len += src_len;
}

static void build_session(uint8_t *session, size_t *session_len)
{
    uint8_t packet[PACKET_HEADER + PACKET_1K_SIZE + PACKET_TRAILER];
    uint8_t payload[PACKET_1K_SIZE];
    size_t packet_len;
    const char header[] = "x.fwot";
    const char size[] = "3";

    *session_len = 0u;

    memset(payload, 0, sizeof(payload));
    memcpy(payload, header, sizeof(header));
    memcpy(&payload[sizeof(header)], size, sizeof(size));
    make_packet(SOH, 0u, payload, PACKET_SIZE, 0u, packet, &packet_len);
    append(session, session_len, packet, packet_len);

    make_packet(STX, 1u, (const uint8_t *)"abc", 3u, CPMEOF,
                packet, &packet_len);
    append(session, session_len, packet, packet_len);

    session[(*session_len)++] = EOT;

    make_packet(SOH, 0u, NULL, 0u, 0u, packet, &packet_len);
    append(session, session_len, packet, packet_len);
}

static void test_receive_minimal_session(void)
{
    uint8_t session[PACKET_HEADER + PACKET_SIZE + PACKET_TRAILER +
                    PACKET_HEADER + PACKET_1K_SIZE + PACKET_TRAILER +
                    1u +
                    PACKET_HEADER + PACKET_SIZE + PACKET_TRAILER];
    size_t session_len;
    fake_io_ctx_t io_ctx;
    fake_sink_ctx_t sink_ctx = {0};
    uint32_t out_size = 0u;
    const fastboot_transport_t transport = {
        fake_read,
        fake_write_byte,
        &io_ctx,
    };
    const fastboot_writer_t writer = {
        fake_begin,
        fake_write,
        NULL,
        NULL,
        NULL,
        &sink_ctx,
    };

    build_session(session, &session_len);
    memset(&io_ctx, 0, sizeof(io_ctx));
    io_ctx.input = session;
    io_ctx.input_len = session_len;

    assert(fastboot_ymodem_receive(&transport, &writer, &s_runtime, &out_size) == FB_OK);
    assert(out_size == 3u);
    assert(sink_ctx.begin_size == 3u);
    assert(sink_ctx.written == 3u);
    assert(memcmp(sink_ctx.data, "abc", 3u) == 0);
    assert(io_ctx.output_len >= 6u);
    assert(io_ctx.output[0] == CRC16_CH);
    assert(io_ctx.output[1] == ACK);
    assert(io_ctx.output[2] == CRC16_CH);
    assert(io_ctx.output[3] == ACK);
    assert(io_ctx.output[4] == ACK);
    assert(io_ctx.output[5] == CRC16_CH);
}

int main(void)
{
    test_receive_minimal_session();
    assert(s_watchdog_count > 0u);
    return 0;
}
