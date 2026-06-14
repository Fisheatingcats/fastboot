# YMODEM-1K 协议实现

## 目录

1. [协议概述](#1-协议概述)
2. [包格式](#2-包格式)
3. [CRC-16 XMODEM 查表算法](#3-crc-16-xmodem-查表算法)
4. [接收状态机](#4-接收状态机)
5. [队列排水策略](#5-队列排水策略)
6. [错误处理和重传机制](#6-错误处理和重传机制)
7. [状态机总览](#7-状态机总览)

---

## 1. 协议概述

YMODEM-1K 是 YMODEM 协议的变体，使用 1024 字节数据包（STX）替代标准 128 字节（SOH），提高传输效率。FastBoot 的 YMODEM 实现位于 `src/core/fastboot_ymodem.c`。

**关键特性**：
- CRC-16 XMODEM 校验（256 项预计算查找表）
- 异步包队列：接收与 Flash 写入流水线重叠
- 高水位主动排水：队列 3/4 满时提前消费
- I/O 等待间隙隐式排水

**协议控制字符**：

| 字符 | 值 | 含义 |
|------|-----|------|
| SOH | 0x01 | 128 字节数据包起始 |
| STX | 0x02 | 1024 字节数据包起始 |
| EOT | 0x04 | 传输结束 |
| ACK | 0x06 | 确认应答 |
| NAK | 0x15 | 否定应答（请求重传） |
| CAN | 0x18 | 取消传输 |
| 'C' | 0x43 | 请求 CRC-16 校验模式 |

## 2. 包格式

```
+------+--------+---------+---------------------------+----------+----------+
| 类型  | 序号 SEQ | 反码 ~SEQ |         数据载荷           | CRC-16 高 | CRC-16 低 |
+------+--------+---------+---------------------------+----------+----------+
| 1B   | 1B     | 1B      | 128B (SOH) 或 1024B (STX) | 1B       | 1B       |
+------+--------+---------+---------------------------+----------+----------+

总开销 = 3 (头) + 2 (尾) = 5 字节
SOH 包总长 = 133 字节
STX 包总长 = 1029 字节
```

**首包（文件名包）格式**：

```
载荷内容：
+-------------------+----+-------------------+----+-----+
| 文件名 (ASCII)     | \0 | 文件大小 (十进制)   | \0 | 填充 |
+-------------------+----+-------------------+----+-----+
```

首包序号为 0x00，包含文件名和文件大小字符串。

## 3. CRC-16 XMODEM 查表算法

### 3.1 算法原理

使用预计算的 256 项查找表，多项式为 `x^16 + x^12 + x^5 + 1`（0x1021），初始值为 0x0000。

**逐字节处理公式**：
```
crc = (crc << 8) ^ table[((crc >> 8) ^ byte) & 0xFF]
```

### 3.2 查找表定义

```c
static const uint16_t s_crc16_table[256] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
    // ... 共 256 项
};
```

### 3.3 计算函数

```c
static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;
    while (len-- > 0u) {
        crc = (uint16_t)((crc << 8u) ^
                         s_crc16_table[((crc >> 8u) ^ *data++) & 0xFFu]);
    }
    return crc;
}
```

**计算流程**：
1. 初始值 crc = 0
2. 取 crc 高字节与当前数据字节异或
3. 以异或结果为索引查表
4. 将 crc 左移 8 位与查表结果异或
5. 重复直到所有字节处理完毕

## 4. 接收状态机

### 4.1 状态定义

```
              ┌──────────┐
              │  IDLE    │  等待首包
              │ (header) │
              └────┬─────┘
                   │ 收到首包，解析文件名/大小
                   v
              ┌──────────┐
              │ RECEIVING│  接收数据包
              │  (data)  │
              └────┬─────┘
                   │ 收到 EOT
                   v
              ┌──────────┐
              │ EOT_WAIT │  等待结束包
              │  (eot)   │
              └────┬─────┘
                   │ 收到空包
                   v
              ┌──────────┐
              │   DONE   │  传输完成
              └──────────┘
```

### 4.2 等待阶段（IDLE）

```c
// 超时时间：SESSION_POLL_MS = 3000ms
pr = receive_packet(transport, runtime, packet, &payload_len, SESSION_POLL_MS);
```

- 发送 'C' 请求 CRC-16 模式
- 等待首包（序号 0x00）
- 超时后重发 'C'，最多重试 MAX_ERRORS 次

### 4.3 文件名包解析

```c
if (!receiving && expected_seq == 0u) {
    const uint8_t *name = &packet[PACKET_HEADER];     // 文件名起始
    const uint8_t *size_str = name;
    // 跳过文件名，定位到 \0 后的大小字符串
    while (*size_str != 0u) ++size_str;
    ++size_str;
    // 解析十进制文件大小
    parse_decimal_size(size_str, &expected_size);
    // 初始化写入器
    writer->begin(writer->ctx, expected_size);
    receiving = true;
    expected_seq = 1u;
}
```

**解析流程**：
1. 从载荷起始读取文件名（以 `\0` 结尾）
2. 跳过文件名，定位到大小字符串
3. 调用 `parse_decimal_size()` 解析十进制数字
4. 调用 `writer->begin()` 初始化写入上下文
5. 进入数据接收状态

### 4.4 数据包接收

```c
// 1. 分配队列槽位
slot = fastboot_queue_alloc(&s_queue);
packet = slot->packet;

// 2. 接收数据包（已在 receive_packet 中校验 CRC）
pr = receive_packet(transport, runtime, packet, &payload_len, RX_TIMEOUT_MS);

// 3. 计算实际载荷长度（最后一个包可能截断）
uint32_t chunk = expected_size - received;
if (chunk > payload_len) chunk = payload_len;

// 4. 填充槽位元数据并提交
slot->offset = received;
slot->len = chunk;
fastboot_queue_commit(&s_queue);

// 5. 推进计数器，发送 ACK
received += chunk;
expected_seq++;
tx_byte(transport, ACK);
```

### 4.5 EOT 处理

```c
if (pr == 0) {          // 收到 EOT
    tx_byte(transport, ACK);
    final_packet_expected = true;   // 标记等待结束包
    expected_seq = 0u;
    request_crc = true;             // 下次发送 'C'
    continue;
}
```

### 4.6 结束包处理

```c
if (final_packet_expected) {
    if (packet[1] == 0u && packet[PACKET_HEADER] == 0u) {
        // 空包：排空所有剩余队列
        fastboot_queue_drain_all(&s_queue, writer, runtime, &written);
        tx_byte(transport, ACK);
        return FB_OK;
    }
    tx_byte(transport, NAK);    // 非空包，请求重传
}
```

## 5. 队列排水策略

YMODEM 接收与 Flash 写入通过环形队列解耦，排水策略直接影响传输性能。

### 5.1 service_writer()：I/O 等待间隙排水

```c
static bool service_writer(void)
{
    if (!s_service_writer || fastboot_queue_empty(&s_queue)) {
        return true;
    }
    rc = fastboot_queue_drain_one(&s_queue, s_service_writer,
                                  s_service_runtime, s_service_written);
    return (rc == FB_OK || rc == FB_NO_UPDATE || rc == FB_BUSY);
}
```

**调用时机**：
- `io_read_exact()` 中等待数据到达时
- 主循环中等待下一个包时

### 5.2 高水位标记（QUEUE_HIGH_WATER）

```c
#define QUEUE_HIGH_WATER  (FASTBOOT_QUEUE_DEPTH * 3u / 4u)

// 主循环中检查
if (fastboot_queue_count(&s_queue) >= QUEUE_HIGH_WATER) {
    fastboot_queue_drain_one(&s_queue, writer, runtime, &written);
}
```

当队列中待排水槽位达到深度的 3/4 时，主动消费一个槽位，防止队列满导致阻塞。

### 5.3 队列满：强制排水

```c
if (fastboot_queue_full(&s_queue)) {
    fastboot_queue_drain_one(&s_queue, writer, runtime, &written);
    continue;   // 跳过本轮接收，腾出空间
}
```

### 5.4 drain_all：传输结束

```c
// 结束包到达后
fastboot_queue_drain_all(&s_queue, writer, runtime, &written);
```

### 5.5 排水策略总览

```
队列状态          动作                    触发条件
─────────────────────────────────────────────────────
空 (count=0)      跳过                    service_writer()
非空              drain_one               service_writer() [I/O 等待]
≥ 3/4 深度        drain_one (主动)        主循环检查
满 (count=DEPTH)  drain_one + continue    主循环检查
传输结束          drain_all               收到结束包后
```

## 6. 错误处理和重传机制

### 6.1 错误计数器

```c
#define MAX_ERRORS  10u
uint32_t errors = 0u;

while (errors < MAX_ERRORS) {
    // ... 接收循环
    if (pr < 0) {
        ++errors;
        // 发送 NAK 或重发 'C'
        continue;
    }
    errors = 0u;    // 成功接收，清零计数器
}
```

### 6.2 错误恢复策略

| 错误类型 | 处理方式 |
|----------|----------|
| 接收超时 | 发送 NAK（数据阶段）或重发 'C'（握手阶段） |
| CRC 校验失败 | 发送 NAK 请求重传 |
| 序号不匹配 | 发送 NAK 请求重传 |
| service_writer 错误 | 发送 CAN CAN 中止传输 |
| 对端取消（CAN×2） | 返回 FB_ERR_IO |
| 连续错误超限 | 发送 CAN CAN，返回 FB_ERR_IO |

### 6.3 超时参数

| 参数 | 值 | 用途 |
|------|-----|------|
| SESSION_POLL_MS | 3000ms | 等待首包的轮询超时 |
| RX_TIMEOUT_MS | 1000ms | 数据包接收超时 |

## 7. 状态机总览

```
                 ┌──────────────────────────────────────────────┐
                 │                                              │
                 v                                              │
+---------+  发送'C'  +----------+  收到首包  +-----------+     │
|  IDLE   | ────────→ | WAIT_HDR | ────────→ | RECEIVING |     │
+---------+           +----------+           +-----+-----+     │
    │                   │    │                    │             │
    │ 超时              │    │ CRC错              │ EOT         │
    │ errors++          │    │ errors++           v             │
    │←──────────────────┘    │              +----------+        │
    │                        │              | EOT_WAIT |        │
    │                        │              +----+-----+        │
    │                        │                   │ 空包         │
    │                        │                   v             │
    │                        │              +----------+        │
    │                        │              |   DONE   |        │
    │                        │              +----------+        │
    │                        │                                  │
    │  errors >= MAX_ERRORS  │                                  │
    └────────────────────────┘                                  │
         发送 CAN CAN，返回 FB_ERR_IO                           │
                 ───────────────────────────────────────────────┘
```

### 接收单个包流程（receive_packet）

```
读取首字节 (rx_byte)
    │
    ├── SOH (0x01) → size = 128
    ├── STX (0x02) → size = 1024
    ├── EOT (0x04) → return 0 (传输结束)
    ├── CAN (0x18) → 读第二字节，双 CAN → return -2
    ├── ABORT      → return -2
    └── 其他       → return -1
            │
            v
    读取剩余字节 (io_read_exact)
        [SEQ][~SEQ][DATA...][CRC_H][CRC_L]
            │
            v
    校验序号反码: packet[1] == ~packet[2] ?
            │
            ├── 不匹配 → return -1
            │
            v
    校验 CRC-16: crc16_xmodem(data, size) == recv_crc ?
            │
            ├── 不匹配 → return -1
            │
            v
    *payload_len = size
    return 1 (成功)
```
