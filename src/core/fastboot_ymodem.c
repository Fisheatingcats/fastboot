/**
 * @file fastboot_ymodem.c
 * @brief YMODEM-1K 协议接收实现
 *
 * 实现 YMODEM-1K 文件接收状态机：
 *   header 包 → 数据包序列 → EOT → 结束包
 *
 * 特性：
 * - CRC-16 XMODEM 校验（256 项查表）
 * - 异步包队列：接收与 flash 写入重叠，提高吞吐
 * - 高水位主动排水：队列 3/4 满时提前消费
 * - 在 I/O 等待期间调用 service_writer() 排水队列
 */

#include "fastboot_ymodem.h"
#include "fastboot_config.h"
#include "fastboot_queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── YMODEM 协议常量 ────────────────────────────────────────────────────── */

#define SOH       0x01u  /**< @brief 128 字节数据包起始标记 */
#define STX       0x02u  /**< @brief 1024 字节数据包起始标记 */
#define EOT       0x04u  /**< @brief 传输结束标记 */
#define ACK       0x06u  /**< @brief 确认应答 */
#define NAK       0x15u  /**< @brief 否定应答（请求重传） */
#define CAN       0x18u  /**< @brief 取消传输 */
#define CRC16_CH  0x43u  /**< @brief 'C'，请求 CRC-16 校验模式 */
#define ABORT1    0x41u  /**< @brief 'A'，中止传输 */
#define ABORT2    0x61u  /**< @brief 'a'，中止传输 */

#define PACKET_HEADER   3u     /**< @brief 包头长度：类型(1) + 序号(1) + 反码(1) */
#define PACKET_TRAILER  2u     /**< @brief 包尾长度：CRC-16(2) */
#define PACKET_OVERHEAD (PACKET_HEADER + PACKET_TRAILER)  /**< @brief 包开销总计 */
#define PACKET_SIZE     128u   /**< @brief SOH 包载荷大小 */
#define PACKET_1K_SIZE  1024u  /**< @brief STX 包载荷大小 */
#define PACKET_MAX_SIZE FASTBOOT_CFG_YMODEM_PACKET_SIZE  /**< @brief 最大包载荷（配置决定） */
#define PACKET_BUF_SIZE (PACKET_MAX_SIZE + PACKET_OVERHEAD)  /**< @brief 完整包缓冲区大小 */
#define MAX_ERRORS      10u    /**< @brief 最大连续错误次数，超过则中止 */
#define RX_TIMEOUT_MS   1000u  /**< @brief 数据包接收超时（毫秒） */
#define SESSION_POLL_MS 3000u  /**< @brief 等待首包的轮询超时（毫秒） */

/* ── 内部状态 ───────────────────────────────────────────────────────────── */

/** @brief 控制包缓冲区，用于握手阶段接收文件名/大小信息 */
static uint8_t s_control_packet[PACKET_BUF_SIZE];

/** @brief 全局包队列实例（静态分配，无 malloc） */
static fboot_queue_t s_queue;

/** @brief service_writer 上下文：writer 接口 */
static const fastboot_writer_t  *s_service_writer;
/** @brief service_writer 上下文：runtime 接口 */
static const fastboot_runtime_t *s_service_runtime;
/** @brief service_writer 上下文：已写字节数指针 */
static uint32_t                 *s_service_written;
/** @brief service_writer 上下文：错误码 */
static fboot_status_t            s_service_rc;

/* ── 队列排水服务（在 I/O 等待期间消费队列） ───────────────────────────── */

/**
 * @brief 在 I/O 等待期间排水一个队列槽位
 *
 * 当 YMODEM 接收在等待数据到达时，调用此函数消费已排队的包，
 * 实现接收与 flash 写入的并行重叠。若排水出错，将错误码记录到
 * s_service_rc 供主循环检查。
 *
 * @return true 正常（队列空或排水成功）；false 排水出错
 */
static bool service_writer(void)
{
    fboot_status_t rc;

    if (!s_service_writer || fastboot_queue_empty(&s_queue)) {
        return true;
    }
    rc = fastboot_queue_drain_one(&s_queue, s_service_writer,
                                  s_service_runtime, s_service_written);
    if (rc == FB_OK || rc == FB_NO_UPDATE || rc == FB_BUSY) {
        return true;
    }
    s_service_rc = rc;
    return false;
}

/** @brief 高水位标记：队列深度的 3/4，超过时主动排水 */
#define QUEUE_HIGH_WATER  (FASTBOOT_QUEUE_DEPTH * 3u / 4u)

/* ── 底层 I/O 辅助 ─────────────────────────────────────────────────────── */

/**
 * @brief 精确读取指定字节数（带超时）
 *
 * 循环调用 transport->read() 直到读满 len 字节或超时。
 * 在等待间隙调用 service_writer() 排水队列。
 *
 * @param transport   传输接口
 * @param runtime     运行时接口（tick）
 * @param data        目标缓冲区
 * @param len         需要读取的字节数
 * @param timeout_ms  超时时间（毫秒）
 * @return            true 读满；false 超时或排水出错
 */
static bool io_read_exact(const fastboot_transport_t *transport,
                          const fastboot_runtime_t *runtime,
                          uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint32_t start = fastboot_runtime_tick_ms(runtime);
    size_t done = 0u;

    while (done < len) {
        size_t got;

        if (fastboot_runtime_tick_ms(runtime) - start >= timeout_ms) {
            return false;
        }
        got = transport->read(transport->ctx, data + done, len - done, 0u);
        if (got > 0u) {
            done += got;
            continue;
        }
        /* 无数据可读时，利用等待时间排水队列 */
        if (!service_writer()) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 接收单个字节（带超时）
 *
 * @param transport   传输接口
 * @param runtime     运行时接口
 * @param out         输出字节指针
 * @param timeout_ms  超时时间（毫秒）
 * @return            true 成功；false 超时
 */
static bool rx_byte(const fastboot_transport_t *transport,
                    const fastboot_runtime_t *runtime,
                    uint8_t *out, uint32_t timeout_ms)
{
    return io_read_exact(transport, runtime, out, 1u, timeout_ms);
}

/**
 * @brief 发送单个字节
 *
 * @param transport  传输接口
 * @param c          待发送字节
 */
static void tx_byte(const fastboot_transport_t *transport, uint8_t c)
{
    transport->write_byte(transport->ctx, c);
}

/* ── CRC-16 XMODEM 查表 ────────────────────────────────────────────────── */

/**
 * @brief CRC-16 XMODEM 查找表（256 项，多项式 0x1021）
 *
 * 预计算的 CRC-16/XMODEM 查表，用于快速 CRC 校验。
 * 初始值为 0x0000，多项式为 x^16 + x^12 + x^5 + 1。
 */
static const uint16_t s_crc16_table[256] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
    0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52B5u, 0x4294u, 0x72F7u, 0x62D6u,
    0x9339u, 0x8318u, 0xB37Bu, 0xA35Au, 0xD3BDu, 0xC39Cu, 0xF3FFu, 0xE3DEu,
    0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64E6u, 0x74C7u, 0x44A4u, 0x5485u,
    0xA56Au, 0xB54Bu, 0x8528u, 0x9509u, 0xE5EEu, 0xF5CFu, 0xC5ACu, 0xD58Du,
    0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76D7u, 0x66F6u, 0x5695u, 0x46B4u,
    0xB75Bu, 0xA77Au, 0x9719u, 0x8738u, 0xF7DFu, 0xE7FEu, 0xD79Du, 0xC7BCu,
    0x48C4u, 0x58E5u, 0x6886u, 0x78A7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
    0xC9CCu, 0xD9EDu, 0xE98Eu, 0xF9AFu, 0x8948u, 0x9969u, 0xA90Au, 0xB92Bu,
    0x5AF5u, 0x4AD4u, 0x7AB7u, 0x6A96u, 0x1A71u, 0x0A50u, 0x3A33u, 0x2A12u,
    0xDBFDu, 0xCBDCu, 0xFBBFu, 0xEB9Eu, 0x9B79u, 0x8B58u, 0xBB3Bu, 0xAB1Au,
    0x6CA6u, 0x7C87u, 0x4CE4u, 0x5CC5u, 0x2C22u, 0x3C03u, 0x0C60u, 0x1C41u,
    0xEDAEu, 0xFD8Fu, 0xCDECu, 0xDDCDu, 0xAD2Au, 0xBD0Bu, 0x8D68u, 0x9D49u,
    0x7E97u, 0x6EB6u, 0x5ED5u, 0x4EF4u, 0x3E13u, 0x2E32u, 0x1E51u, 0x0E70u,
    0xFF9Fu, 0xEFBEu, 0xDFDDu, 0xCFFCu, 0xBF1Bu, 0xAF3Au, 0x9F59u, 0x8F78u,
    0x9188u, 0x81A9u, 0xB1CAu, 0xA1EBu, 0xD10Cu, 0xC12Du, 0xF14Eu, 0xE16Fu,
    0x1080u, 0x00A1u, 0x30C2u, 0x20E3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
    0x83B9u, 0x9398u, 0xA3FBu, 0xB3DAu, 0xC33Du, 0xD31Cu, 0xE37Fu, 0xF35Eu,
    0x02B1u, 0x1290u, 0x22F3u, 0x32D2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
    0xB5EAu, 0xA5CBu, 0x95A8u, 0x8589u, 0xF56Eu, 0xE54Fu, 0xD52Cu, 0xC50Du,
    0x34E2u, 0x24C3u, 0x14A0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
    0xA7DBu, 0xB7FAu, 0x8799u, 0x97B8u, 0xE75Fu, 0xF77Eu, 0xC71Du, 0xD73Cu,
    0x26D3u, 0x36F2u, 0x0691u, 0x16B0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
    0xD94Cu, 0xC96Du, 0xF90Eu, 0xE92Fu, 0x99C8u, 0x89E9u, 0xB98Au, 0xA9ABu,
    0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18C0u, 0x08E1u, 0x3882u, 0x28A3u,
    0xCB7Du, 0xDB5Cu, 0xEB3Fu, 0xFB1Eu, 0x8BF9u, 0x9BD8u, 0xABBBu, 0xBB9Au,
    0x4A75u, 0x5A54u, 0x6A37u, 0x7A16u, 0x0AF1u, 0x1AD0u, 0x2AB3u, 0x3A92u,
    0xFD2Eu, 0xED0Fu, 0xDD6Cu, 0xCD4Du, 0xBDAAu, 0xAD8Bu, 0x9DE8u, 0x8DC9u,
    0x7C26u, 0x6C07u, 0x5C64u, 0x4C45u, 0x3CA2u, 0x2C83u, 0x1CE0u, 0x0CC1u,
    0xEF1Fu, 0xFF3Eu, 0xCF5Du, 0xDF7Cu, 0xAF9Bu, 0xBFBAu, 0x8FD9u, 0x9FF8u,
    0x6E17u, 0x7E36u, 0x4E55u, 0x5E74u, 0x2E93u, 0x3EB2u, 0x0ED1u, 0x1EF0u,
};

/**
 * @brief CRC-16 XMODEM 查表计算
 *
 * 使用预计算查找表，逐字节计算 CRC-16/XMODEM。
 * 算法：crc = (crc << 8) ^ table[((crc >> 8) ^ byte) & 0xFF]
 *
 * @param data  数据指针
 * @param len   数据长度（字节）
 * @return      CRC-16 结果
 */
static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;

    while (len-- > 0u) {
        /* 高字节异或当前数据字节，查表得到新的高字节值 */
        crc = (uint16_t)((crc << 8u) ^
                         s_crc16_table[((crc >> 8u) ^ *data++) & 0xFFu]);
    }
    return crc;
}

/* ── 文件大小字符串解析 ─────────────────────────────────────────────────── */

/**
 * @brief 解析十进制文件大小字符串
 *
 * 解析 YMODEM 首包中的文件大小字段（以空格或 '\0' 结尾的十进制字符串）。
 *
 * @param s    输入字符串（十进制数字）
 * @param out  输出解析结果
 * @return     FB_OK 成功；FB_ERR_FORMAT 格式错误
 */
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

/* ── 包接收 ─────────────────────────────────────────────────────────────── */

/**
 * @brief 接收单个 YMODEM 包
 *
 * 处理流程：
 * 1. 读取首字节判断包类型（SOH/STX/EOT/CAN/ABORT）
 * 2. 对于数据包：读取剩余字节 → 校验序号反码 → 校验 CRC-16
 *
 * @param transport    传输接口
 * @param runtime      运行时接口
 * @param packet       包缓冲区（含头部 + 载荷 + CRC）
 * @param payload_len  [out] 载荷长度（SOH=128, STX=1024）
 * @param timeout_ms   首字节超时（毫秒）
 * @return             1 数据包成功；0 EOT；-1 错误/超时；-2 对端取消
 */
static int receive_packet(const fastboot_transport_t *transport,
                          const fastboot_runtime_t *runtime,
                          uint8_t *packet, uint32_t *payload_len,
                          uint32_t timeout_ms)
{
    uint8_t c = 0u;
    uint32_t size;
    uint16_t crc_recv;
    uint16_t crc_calc;

    *payload_len = 0u;
    if (!rx_byte(transport, runtime, &c, timeout_ms)) {
        return -1;
    }

    /* EOT：传输结束 */
    if (c == EOT) {
        return 0;
    }
    /* CAN：对端取消（需连续两个 CAN 才确认） */
    if (c == CAN) {
        uint8_t c2 = 0u;
        if (rx_byte(transport, runtime, &c2, RX_TIMEOUT_MS) && c2 == CAN) {
            return -2;
        }
        return -1;
    }
    /* ABORT：用户中止 */
    if (c == ABORT1 || c == ABORT2) {
        return -2;
    }
    /* 根据首字节确定包载荷大小 */
    if (c == SOH) {
        size = PACKET_SIZE;
    } else if (c == STX && PACKET_MAX_SIZE >= PACKET_1K_SIZE) {
        size = PACKET_1K_SIZE;
    } else {
        return -1;
    }
    if (size > PACKET_MAX_SIZE) {
        return -1;
    }

    /* 读取剩余字节：序号(1) + 反码(1) + 载荷(size) + CRC(2) */
    packet[0] = c;
    if (!io_read_exact(transport, runtime, &packet[1],
                       (size + PACKET_OVERHEAD) - 1u, RX_TIMEOUT_MS)) {
        return -1;
    }

    /* 校验序号反码：packet[1] 应等于 ~packet[2] */
    if (packet[1] != (uint8_t)~packet[2]) {
        return -1;
    }
    /* 校验 CRC-16：高字节在前 */
    crc_recv = ((uint16_t)packet[PACKET_HEADER + size] << 8) |
               packet[PACKET_HEADER + size + 1u];
    crc_calc = crc16_xmodem(&packet[PACKET_HEADER], size);
    if (crc_recv != crc_calc) {
        return -1;
    }

    *payload_len = size;
    return 1;
}

/* ── 公共 API ───────────────────────────────────────────────────────────── */

/**
 * @brief YMODEM 文件接收主循环
 *
 * 状态机流程：
 * 1. header 阶段：等待首包，解析文件名和大小，调用 writer->begin()
 * 2. data 阶段：接收数据包，分配队列槽位，提交异步写入
 * 3. eot 阶段：收到 EOT 后发送 ACK，等待结束包
 * 4. end 阶段：收到空结束包，排水所有队列，发送 ACK
 *
 * 队列管理策略：
 * - 高水位（3/4 深度）：主动排水一个槽位
 * - 队列满：强制排水一个槽位后再接收
 * - I/O 等待间隙：调用 service_writer() 隐式排水
 *
 * @note  当 FASTBOOT_CFG_ENABLE_YMODEM 为 0 时，直接返回 FB_ERR_PARAM。
 *
 * @param transport  传输接口（read、write_byte）
 * @param writer     写入接口（begin、write）
 * @param runtime    运行时接口（tick_ms、feed_watchdog）
 * @param out_size   [out] 接收的文件大小（可为 NULL）
 * @return           FB_OK 成功；FB_ERR_IO 错误/超时；FB_ERR_FORMAT 格式错误；其他为错误码
 */
fboot_status_t fastboot_ymodem_receive(const fastboot_transport_t *transport,
                                       const fastboot_writer_t *writer,
                                       const fastboot_runtime_t *runtime,
                                       uint32_t *out_size)
{
#if FASTBOOT_CFG_ENABLE_YMODEM
    uint32_t expected_size = 0u;
    uint32_t received = 0u;
    uint32_t written = 0u;
    uint8_t expected_seq = 0u;
    bool receiving = false;
    bool final_packet_expected = false;
    bool request_crc = true;
    uint32_t errors = 0u;

    if (!transport || !transport->read || !transport->write_byte ||
        !writer || !writer->begin || !writer->write ||
        !runtime || !runtime->tick_ms) {
        return FB_ERR_PARAM;
    }
    if (out_size) {
        *out_size = 0u;
    }

    /* 初始化队列和 service_writer 上下文 */
    fastboot_queue_reset(&s_queue);
    s_service_writer = writer;
    s_service_runtime = runtime;
    s_service_written = &written;
    s_service_rc = FB_OK;

    while (errors < MAX_ERRORS) {
        uint32_t payload_len = 0u;
        uint8_t *packet = s_control_packet;
        fboot_queue_slot_t *slot = NULL;
        int pr;

        if (receiving && !final_packet_expected) {
            /* 利用等待时间排水队列 */
            if (!service_writer()) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return s_service_rc;
            }
            /* 高水位主动排水：队列 3/4 满时提前消费一个槽位 */
            if (fastboot_queue_count(&s_queue) >= QUEUE_HIGH_WATER) {
                fboot_status_t rc =
                    fastboot_queue_drain_one(&s_queue, writer, runtime,
                                             &written);
                if (rc != FB_OK && rc != FB_NO_UPDATE && rc != FB_BUSY) {
                    tx_byte(transport, CAN);
                    tx_byte(transport, CAN);
                    return rc;
                }
            }
            /* 队列满：强制排水一个槽位后再继续 */
            if (fastboot_queue_full(&s_queue)) {
                fboot_status_t rc =
                    fastboot_queue_drain_one(&s_queue, writer, runtime,
                                             &written);
                if (rc != FB_OK && rc != FB_NO_UPDATE && rc != FB_BUSY) {
                    tx_byte(transport, CAN);
                    tx_byte(transport, CAN);
                    return rc;
                }
                continue;
            }
            /* 分配队列槽位用于存放即将接收的包 */
            slot = fastboot_queue_alloc(&s_queue);
            if (!slot) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return FB_ERR_IO;
            }
            packet = slot->packet;
        }

        /* 首次或重试时发送 'C' 请求 CRC-16 校验模式 */
        if (request_crc) {
            tx_byte(transport, CRC16_CH);
            request_crc = false;
        }

        pr = receive_packet(transport, runtime, packet, &payload_len,
                            receiving ? RX_TIMEOUT_MS : SESSION_POLL_MS);
        if (pr < 0) {
            /* 检查 service_writer 是否记录了错误 */
            if (s_service_rc != FB_OK) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return s_service_rc;
            }
            if (pr == -2) {
                return FB_ERR_IO;
            }
            /* 接收中且队列非空时，利用错误恢复间隙排水 */
            if (receiving && !fastboot_queue_empty(&s_queue)) {
                fboot_status_t rc =
                    fastboot_queue_drain_one(&s_queue, writer, runtime,
                                             &written);
                if (rc != FB_OK && rc != FB_NO_UPDATE && rc != FB_BUSY) {
                    tx_byte(transport, CAN);
                    tx_byte(transport, CAN);
                    return rc;
                }
                continue;
            }
            if (!receiving || final_packet_expected) {
                request_crc = true;
            } else {
                tx_byte(transport, NAK);
            }
            ++errors;
            continue;
        }
        errors = 0u;

        /* EOT：传输结束，发送 ACK 并标记等待结束包 */
        if (pr == 0) {
            tx_byte(transport, ACK);
            final_packet_expected = true;
            expected_seq = 0u;
            request_crc = true;
            continue;
        }

        /* 结束包：空包表示传输完成 */
        if (final_packet_expected) {
            if (packet[1] == 0u && packet[PACKET_HEADER] == 0u) {
                /* 排空所有剩余队列 */
                fboot_status_t rc =
                    fastboot_queue_drain_all(&s_queue, writer, runtime,
                                             &written);
                if (rc != FB_OK) {
                    tx_byte(transport, CAN);
                    tx_byte(transport, CAN);
                    return rc;
                }
                tx_byte(transport, ACK);
                if (out_size) {
                    *out_size = received;
                }
                return (receiving &&
                        received == expected_size &&
                        written == expected_size)
                           ? FB_OK
                           : FB_ERR_FORMAT;
            }
            tx_byte(transport, NAK);
            continue;
        }

        /* 序号不匹配：请求重传 */
        if (packet[1] != expected_seq) {
            tx_byte(transport, NAK);
            continue;
        }

        /* 首包：解析文件名和大小 */
        if (!receiving && expected_seq == 0u) {
            const uint8_t *name = &packet[PACKET_HEADER];
            const uint8_t *size_str = name;

            /* 空文件名表示无待传输文件 */
            if (name[0] == 0u) {
                tx_byte(transport, ACK);
                return receiving ? FB_OK : FB_NO_UPDATE;
            }
            /* 跳过文件名，定位到大小字符串 */
            while ((size_str - name) < (ptrdiff_t)payload_len &&
                   *size_str != 0u) {
                ++size_str;
            }
            if ((size_str - name) >= (ptrdiff_t)payload_len) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return FB_ERR_FORMAT;
            }
            ++size_str;
            if (parse_decimal_size(size_str, &expected_size) != FB_OK ||
                expected_size == 0u) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return FB_ERR_RANGE;
            }
            {
                /* 调用 writer->begin() 初始化写入上下文 */
                fboot_status_t rc = writer->begin(writer->ctx, expected_size);
                if (rc != FB_OK) {
                    tx_byte(transport, CAN);
                    tx_byte(transport, CAN);
                    return rc;
                }
            }
            receiving = true;
            expected_seq = 1u;
            tx_byte(transport, ACK);
            request_crc = true;
            continue;
        }

        /* 数据包：计算实际载荷长度，提交到队列 */
        {
            /* 最后一个包可能不满，截断到实际文件大小 */
            uint32_t chunk = expected_size - received;
            if (chunk > payload_len) {
                chunk = payload_len;
            }

            if (!slot) {
                tx_byte(transport, CAN);
                tx_byte(transport, CAN);
                return FB_ERR_IO;
            }
            slot->offset = received;
            slot->len = chunk;
            fastboot_queue_commit(&s_queue);
            received += chunk;
            expected_seq++;
            tx_byte(transport, ACK);

            fastboot_runtime_feed_watchdog(runtime);

            if (received >= expected_size) {
                if (out_size) {
                    *out_size = received;
                }
            }
        }
    }

    /* 连续错误次数超限，中止传输 */
    (void)fastboot_queue_drain_all(&s_queue, writer, runtime, &written);
    tx_byte(transport, CAN);
    tx_byte(transport, CAN);
    return FB_ERR_IO;
#else
    (void)transport;
    (void)writer;
    (void)runtime;
    (void)out_size;
    return FB_ERR_PARAM;
#endif
}
