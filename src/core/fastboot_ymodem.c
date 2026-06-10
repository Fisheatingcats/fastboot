#include "fastboot_ymodem.h"
#include "fastboot_config.h"
#include "fastboot_port.h"
#include "fastboot_queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── YMODEM protocol constants ────────────────────────────────────────────── */

#define SOH       0x01u
#define STX       0x02u
#define EOT       0x04u
#define ACK       0x06u
#define NAK       0x15u
#define CAN       0x18u
#define CRC16_CH  0x43u
#define ABORT1    0x41u
#define ABORT2    0x61u

#define PACKET_HEADER   3u
#define PACKET_TRAILER  2u
#define PACKET_OVERHEAD (PACKET_HEADER + PACKET_TRAILER)
#define PACKET_SIZE     128u
#define PACKET_1K_SIZE  1024u
#define PACKET_BUF_SIZE (PACKET_1K_SIZE + PACKET_OVERHEAD)
#define MAX_ERRORS      10u
#define RX_TIMEOUT_MS   1000u
#define SESSION_POLL_MS 3000u

/* ── Internal state ───────────────────────────────────────────────────────── */

/* One control packet for handshake (filename/size). */
static uint8_t s_control_packet[PACKET_BUF_SIZE];

/* Global queue instance (static allocation, no malloc). */
static fboot_queue_t s_queue;

/* Service writer context. */
static const fboot_sink_t *s_service_sink;
static uint32_t           *s_service_written;
static fboot_status_t      s_service_rc;

/* ── Service writer (drains queue during I/O waits) ───────────────────────── */

static bool service_writer(void)
{
    fboot_status_t rc;

    if (!s_service_sink || fastboot_queue_empty(&s_queue)) {
        return true;
    }
    rc = fastboot_queue_drain_one(&s_queue, s_service_sink, s_service_written);
    if (rc == FB_OK || rc == FB_NO_UPDATE || rc == FB_BUSY) {
        return true;
    }
    s_service_rc = rc;
    return false;
}

/* ── Low-level I/O helpers ────────────────────────────────────────────────── */

static bool io_read_exact(const fboot_io_t *io, uint8_t *data, size_t len,
                          uint32_t timeout_ms)
{
    uint32_t start = fastboot_port_tick_ms();
    size_t done = 0u;

    while (done < len) {
        uint32_t elapsed = fastboot_port_tick_ms() - start;
        uint32_t remaining;
        size_t got;

        if (elapsed >= timeout_ms) {
            return false;
        }
        remaining = timeout_ms - elapsed;
        got = io->read(io->ctx, data + done, len - done, remaining);
        if (!service_writer()) {
            return false;
        }
        if (got == 0u) {
            return false;
        }
        done += got;
        if (!service_writer()) {
            return false;
        }
    }
    return true;
}

static bool rx_byte(const fboot_io_t *io, uint8_t *out, uint32_t timeout_ms)
{
    return io_read_exact(io, out, 1u, timeout_ms);
}

static void tx_byte(const fboot_io_t *io, uint8_t c)
{
    io->write_byte(io->ctx, c);
}

/* ── CRC-16 XMODEM ────────────────────────────────────────────────────────── */

static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;

    while (len-- > 0u) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint32_t i = 0u; i < 8u; ++i) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u)
                                  : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

/* ── Size string parser ───────────────────────────────────────────────────── */

static fboot_status_t parse_decimal_size(const uint8_t *s, uint32_t *out)
{
    uint32_t value = 0u;

    if (!s || !out || *s == 0u) {
        return FB_ERR_FORMAT;
    }
    while (*s != 0u && *s != ' ') {
        if (*s < '0' || *s > '9') {
            return FB_ERR_FORMAT;
        }
        value = (value * 10u) + (uint32_t)(*s - '0');
        ++s;
    }
    *out = value;
    return FB_OK;
}

/* ── Packet reception ─────────────────────────────────────────────────────── */

static int receive_packet(const fboot_io_t *io, uint8_t *packet,
                          uint32_t *payload_len, uint32_t timeout_ms)
{
    uint8_t c = 0u;
    uint32_t size;
    uint16_t crc_recv;
    uint16_t crc_calc;

    *payload_len = 0u;
    if (!rx_byte(io, &c, timeout_ms)) {
        return -1;
    }

    if (c == EOT) {
        return 0;
    }
    if (c == CAN) {
        uint8_t c2 = 0u;
        if (rx_byte(io, &c2, RX_TIMEOUT_MS) && c2 == CAN) {
            return -2;
        }
        return -1;
    }
    if (c == ABORT1 || c == ABORT2) {
        return -2;
    }
    if (c == SOH) {
        size = PACKET_SIZE;
    } else if (c == STX) {
        size = PACKET_1K_SIZE;
    } else {
        return -1;
    }

    packet[0] = c;
    if (!io_read_exact(io, &packet[1], (size + PACKET_OVERHEAD) - 1u,
                       RX_TIMEOUT_MS)) {
        return -1;
    }

    if (packet[1] != (uint8_t)~packet[2]) {
        return -1;
    }
    crc_recv = ((uint16_t)packet[PACKET_HEADER + size] << 8) |
               packet[PACKET_HEADER + size + 1u];
    crc_calc = crc16_xmodem(&packet[PACKET_HEADER], size);
    if (crc_recv != crc_calc) {
        return -1;
    }

    *payload_len = size;
    return 1;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

fboot_status_t fastboot_ymodem_receive(const fboot_io_t *io,
                                       const fboot_sink_t *sink,
                                       uint32_t *out_size)
{
    uint32_t expected_size = 0u;
    uint32_t received = 0u;
    uint32_t written = 0u;
    uint8_t expected_seq = 0u;
    bool receiving = false;
    bool final_packet_expected = false;
    bool request_crc = true;
    uint32_t errors = 0u;

    if (!io || !io->read || !io->write_byte || !sink || !sink->begin ||
        !sink->write) {
        return FB_ERR_PARAM;
    }
    if (out_size) {
        *out_size = 0u;
    }

    /* Initialize queue and service context. */
    fastboot_queue_reset(&s_queue);
    s_service_sink = sink;
    s_service_written = &written;
    s_service_rc = FB_OK;

    while (errors < MAX_ERRORS) {
        uint32_t payload_len = 0u;
        uint8_t *packet = s_control_packet;
        fboot_queue_slot_t *slot = NULL;
        int pr;

        if (receiving && !final_packet_expected) {
            if (!service_writer()) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return s_service_rc;
            }
            if (fastboot_queue_full(&s_queue)) {
                fboot_status_t rc =
                    fastboot_queue_drain_one(&s_queue, sink, &written);
                if (rc != FB_OK && rc != FB_NO_UPDATE && rc != FB_BUSY) {
                    tx_byte(io, CAN);
                    tx_byte(io, CAN);
                    return rc;
                }
                continue;
            }
            slot = fastboot_queue_alloc(&s_queue);
            if (!slot) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return FB_ERR_IO;
            }
            packet = slot->packet;
        }

        if (request_crc) {
            tx_byte(io, CRC16_CH);
            request_crc = false;
        }

        pr = receive_packet(io, packet, &payload_len,
                            receiving ? RX_TIMEOUT_MS : SESSION_POLL_MS);
        if (pr < 0) {
            if (s_service_rc != FB_OK) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return s_service_rc;
            }
            if (pr == -2) {
                return FB_ERR_IO;
            }
            if (receiving && !fastboot_queue_empty(&s_queue)) {
                fboot_status_t rc =
                    fastboot_queue_drain_one(&s_queue, sink, &written);
                if (rc != FB_OK && rc != FB_NO_UPDATE && rc != FB_BUSY) {
                    tx_byte(io, CAN);
                    tx_byte(io, CAN);
                    return rc;
                }
                continue;
            }
            if (!receiving || final_packet_expected) {
                request_crc = true;
            } else {
                tx_byte(io, NAK);
            }
            ++errors;
            continue;
        }
        errors = 0u;

        /* EOT – end of transmission. */
        if (pr == 0) {
            tx_byte(io, ACK);
            final_packet_expected = true;
            expected_seq = 0u;
            request_crc = true;
            continue;
        }

        /* Final empty packet after EOT. */
        if (final_packet_expected) {
            if (packet[1] == 0u && packet[PACKET_HEADER] == 0u) {
                fboot_status_t rc =
                    fastboot_queue_drain_all(&s_queue, sink, &written);
                if (rc != FB_OK) {
                    tx_byte(io, CAN);
                    tx_byte(io, CAN);
                    return rc;
                }
                tx_byte(io, ACK);
                if (out_size) {
                    *out_size = received;
                }
                return (receiving &&
                        received == expected_size &&
                        written == expected_size)
                           ? FB_OK
                           : FB_ERR_FORMAT;
            }
            tx_byte(io, NAK);
            continue;
        }

        /* Sequence number mismatch. */
        if (packet[1] != expected_seq) {
            tx_byte(io, NAK);
            continue;
        }

        /* First data packet – parse filename and size. */
        if (!receiving && expected_seq == 0u) {
            const uint8_t *name = &packet[PACKET_HEADER];
            const uint8_t *size_str = name;

            if (name[0] == 0u) {
                tx_byte(io, ACK);
                return receiving ? FB_OK : FB_NO_UPDATE;
            }
            while ((size_str - name) < (ptrdiff_t)payload_len &&
                   *size_str != 0u) {
                ++size_str;
            }
            if ((size_str - name) >= (ptrdiff_t)payload_len) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return FB_ERR_FORMAT;
            }
            ++size_str;
            if (parse_decimal_size(size_str, &expected_size) != FB_OK ||
                expected_size == 0u ||
                expected_size > FASTBOOT_EXTFLASH_OTA_SIZE) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return FB_ERR_RANGE;
            }
            {
                fboot_status_t rc = sink->begin(sink->ctx, expected_size);
                if (rc != FB_OK) {
                    tx_byte(io, CAN);
                    tx_byte(io, CAN);
                    return rc;
                }
            }
            receiving = true;
            expected_seq = 1u;
            tx_byte(io, ACK);
            request_crc = true;
            continue;
        }

        /* Data packet – queue for async programming. */
        {
            uint32_t chunk = expected_size - received;
            if (chunk > payload_len) {
                chunk = payload_len;
            }

            if (!slot) {
                tx_byte(io, CAN);
                tx_byte(io, CAN);
                return FB_ERR_IO;
            }
            slot->offset = received;
            slot->len = chunk;
            fastboot_queue_commit(&s_queue);
            received += chunk;
            expected_seq++;
            tx_byte(io, ACK);

            fastboot_port_feed_watchdog();

            if (received >= expected_size) {
                if (out_size) {
                    *out_size = received;
                }
            }
        }
    }

    /* Max errors exceeded. */
    (void)fastboot_queue_drain_all(&s_queue, sink, &written);
    tx_byte(io, CAN);
    tx_byte(io, CAN);
    return FB_ERR_IO;
}
