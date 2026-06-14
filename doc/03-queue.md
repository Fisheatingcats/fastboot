# 数据包队列

## 目录

1. [设计目标](#1-设计目标)
2. [数据结构](#2-数据结构)
3. [环形缓冲区原理](#3-环形缓冲区原理)
4. [操作流程详解](#4-操作流程详解)
5. [同步与异步写入路径](#5-同步与异步写入路径)
6. [与 YMODEM 接收的协作](#6-与-ymodem-接收的协作)
7. [流程总览](#7-流程总览)

---

## 1. 设计目标

`fastboot_queue` 实现了一个固定深度的环形包队列，核心目标：

- **解耦接收与写入**：YMODEM 接收速率和 Flash 写入速率不同步，队列缓冲差异
- **零动态分配**：所有槽位静态分配在 `.bss` 段
- **支持异步写入**：`drain_one()` 可在异步写入未完成时返回 `FB_BUSY`
- **生产者-消费者模式**：alloc/commit 为生产端，drain_one/drain_all 为消费端

## 2. 数据结构

### 2.1 槽位结构 fboot_queue_slot_t

```c
typedef struct {
    uint8_t  packet[FASTBOOT_QUEUE_BUF_SIZE]; // 数据包缓冲区
    uint32_t offset;     // 写入目标 Flash 偏移
    uint32_t len;        // 有效数据长度
    bool     writing;    // 异步写入进行中标记
} fboot_queue_slot_t;
```

**缓冲区大小计算**：
```
FASTBOOT_QUEUE_BUF_SIZE = FASTBOOT_CFG_YMODEM_PACKET_SIZE + 3 (头) + 2 (尾)
                        = 1024 + 5 = 1029 字节 (默认配置)
```

每个槽位存储一个完整的 YMODEM 包（含协议头），`offset` 和 `len` 记录有效载荷的写入目标和长度。

### 2.2 队列结构 fboot_queue_t

```c
typedef struct {
    fboot_queue_slot_t slots[FASTBOOT_QUEUE_DEPTH]; // 槽位数组
    uint32_t head;    // 队头索引（下一个消费位置）
    uint32_t tail;    // 队尾索引（下一个分配位置）
    uint32_t count;   // 当前队列中的数据包数量
} fboot_queue_t;
```

### 2.3 内存布局

```
slots[0]  slots[1]  slots[2]  slots[3]    (FASTBOOT_QUEUE_DEPTH=4)
┌────────┬────────┬────────┬────────┐
│ slot 0 │ slot 1 │ slot 2 │ slot 3 │
│        │        │        │        │
│ packet │ packet │ packet │ packet │  每个 ~1029B
│ offset │ offset │ offset │ offset │
│ len    │ len    │ len    │ len    │
│ writing│ writing│ writing│ writing│
└────────┴────────┴────────┴────────┘
  ↑ head              ↑ tail
  (待消费)            (下一个分配位置)
```

## 3. 环形缓冲区原理

### 3.1 三指针管理

```
         消费方向 ←──────────────── 分配方向
                    head        tail
                      ↓           ↓
    ┌────┬────┬────┬────┬────┬────┬────┬────┐
    │    │    │ 已 │ 已 │ 已 │    │    │    │
    │ 空 │ 空 │ 消 │ 消 │ 填 │ 空 │ 空 │ 空 │
    │    │    │ 费 │ 费 │ 充 │    │    │    │
    └────┴────┴────┴────┴────┴────┴────┴────┘
                   ← count=3 →
```

- **head**：下一个待消费的槽位索引
- **tail**：下一个可分配的槽位索引
- **count**：当前队列中的有效槽位数

### 3.2 状态判断

```c
bool full  = (count >= FASTBOOT_QUEUE_DEPTH);
bool empty = (count == 0);
```

### 3.3 索引回绕

```c
tail = (tail + 1u) % FASTBOOT_QUEUE_DEPTH;  // commit 时
head = (head + 1u) % FASTBOOT_QUEUE_DEPTH;  // drain_one 时
```

## 4. 操作流程详解

### 4.1 reset() — 重置队列

```c
void fastboot_queue_reset(fboot_queue_t *q)
{
    q->head  = 0u;
    q->tail  = 0u;
    q->count = 0u;
}
```

在 YMODEM 会话开始时调用，将队列清空。

### 4.2 alloc() — 分配尾部槽位

```c
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q)
{
    if (fastboot_queue_full(q)) {
        return NULL;        // 队列满，分配失败
    }
    q->slots[q->tail].writing = false;
    return &q->slots[q->tail];  // 返回 tail 位置的槽位指针
}
```

**流程**：
1. 检查队列是否已满
2. 清除 `writing` 标记
3. 返回 tail 位置的槽位指针

**注意**：alloc 后 tail 指针不变，需调用 commit() 推进。

### 4.3 commit() — 提交已分配槽位

```c
void fastboot_queue_commit(fboot_queue_t *q)
{
    q->tail = (q->tail + 1u) % FASTBOOT_QUEUE_DEPTH;
    ++q->count;
}
```

**流程**：
1. tail 指针前移（取模回绕）
2. count 加 1
3. 槽位对 drain 操作可见

### 4.4 drain_one() — 消费头部槽位

```c
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
                                        const fastboot_writer_t *writer,
                                        const fastboot_runtime_t *runtime,
                                        uint32_t *written)
{
    slot = &q->slots[q->head];

    // 三种写入路径（详见第 5 节）
    if (slot->writing) {
        rc = writer->poll(writer->ctx);         // 异步：轮询
    } else if (async_ready) {
        rc = writer->write_start(...);           // 异步：启动
    } else {
        rc = writer->write(...);                 // 同步：直接写
    }

    // 成功：前移 head，减少 count
    q->head = (q->head + 1u) % FASTBOOT_QUEUE_DEPTH;
    --q->count;
    fastboot_runtime_feed_watchdog(runtime);
    return FB_OK;
}
```

### 4.5 drain_all() — 清空所有槽位

```c
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q, ...)
{
    while (!fastboot_queue_empty(q)) {
        rc = fastboot_queue_drain_one(q, writer, runtime, written);
        if (rc == FB_BUSY) {
            fastboot_runtime_feed_watchdog(runtime);
            continue;           // 异步未完成，继续轮询
        }
        if (rc != FB_OK && rc != FB_NO_UPDATE) {
            return rc;          // 错误，立即返回
        }
    }
    return FB_OK;
}
```

## 5. 同步与异步写入路径

### 5.1 同步写入路径

```
drain_one()
    │
    ├── slot->writing == false
    ├── 无 write_start/poll/busy 回调
    │
    v
writer->write(ctx, offset, data, len)
    │
    ├── FB_OK → 推进 head，减少 count
    └── 其他  → 返回错误
```

### 5.2 异步写入路径（FASTBOOT_CFG_ENABLE_ASYNC_SINK=1）

```
drain_one() 第一次调用
    │
    ├── slot->writing == false
    ├── write_start/poll/busy 均可用
    │
    v
writer->write_start(ctx, offset, data, len)
    │
    ├── FB_BUSY 或 busy()==true
    │   └── slot->writing = true
    │       └── return FB_BUSY (不推进 head)
    │
    └── FB_OK
        └── 推进 head，减少 count

drain_one() 后续调用 (slot->writing == true)
    │
    v
writer->poll(ctx)
    │
    ├── FB_BUSY → return FB_BUSY (继续等待)
    └── FB_OK   → 检查 busy()
                   ├── true  → return FB_BUSY
                   └── false → 推进 head，减少 count
```

### 5.3 写入路径选择逻辑

```c
if (slot->writing) {
    // 路径 A：异步写入进行中，轮询完成状态
    rc = writer->poll ? writer->poll(writer->ctx) : FB_OK;
}
#if FASTBOOT_CFG_ENABLE_ASYNC_SINK
else if (writer->write_start && writer->poll && writer->busy) {
    // 路径 B：异步写入就绪，启动 write_start
    rc = writer->write_start(writer->ctx, slot->offset,
                             &slot->packet[PACKET_HEADER], slot->len);
    if (rc == FB_BUSY || writer->busy(writer->ctx)) {
        slot->writing = true;
        return FB_BUSY;
    }
}
#endif
else {
    // 路径 C：同步写入
    rc = writer->write(writer->ctx, slot->offset,
                       &slot->packet[PACKET_HEADER], slot->len);
}
```

## 6. 与 YMODEM 接收的协作

### 6.1 生产者（YMODEM 接收）

```c
// 1. 分配槽位
slot = fastboot_queue_alloc(&s_queue);
packet = slot->packet;

// 2. 接收数据包到 slot->packet
receive_packet(transport, runtime, packet, &payload_len, timeout);

// 3. 填充元数据
slot->offset = received;
slot->len = chunk;

// 4. 提交到队列
fastboot_queue_commit(&s_queue);
```

### 6.2 消费者（队列排水）

```c
// 在 I/O 等待间隙
service_writer()  →  fastboot_queue_drain_one()

// 高水位主动排水
if (count >= QUEUE_HIGH_WATER)  →  fastboot_queue_drain_one()

// 队列满强制排水
if (full)  →  fastboot_queue_drain_one() + continue

// 传输结束
fastboot_queue_drain_all()
```

### 6.3 协作时序

```
时间轴 ──────────────────────────────────────────────→

YMODEM:  [接收包0] [接收包1] [接收包2] [等待...] [接收包3] [EOT]
             │         │         │         │         │       │
             v         v         v         v         v       v
队列:     alloc     alloc     alloc    drain_one  alloc   drain_all
          commit    commit    commit              commit
             │         │         │         │
             └────┬────┘         └────┬────┘
                  v                   v
Writer:      [写入包0]           [写入包1]  [写入包2] [写入包3]
```

**关键点**：YMODEM 接收和 Flash 写入可以重叠执行，队列深度决定了最大重叠窗口。

## 7. 流程总览

### 7.1 完整生命周期

```
YMODEM 会话开始
    │
    v
fastboot_queue_reset()          ← 清空队列
    │
    v
┌───────────────────────────────┐
│  主循环                        │
│  ├─ service_writer()          │  ← I/O 等待时排水
│  ├─ 高水位检查 → drain_one    │  ← 主动排水
│  ├─ 队列满检查 → drain_one    │  ← 强制排水
│  ├─ alloc()                   │  ← 分配槽位
│  ├─ receive_packet()          │  ← 接收数据
│  ├─ commit()                  │  ← 提交到队列
│  └─ tx_byte(ACK)              │  ← 确认应答
└───────────────────────────────┘
    │
    v
fastboot_queue_drain_all()      ← 清空剩余
    │
    v
YMODEM 会话结束
```

### 7.2 状态转换表

| 操作 | head | tail | count | 返回值 |
|------|------|------|-------|--------|
| reset() | 0 | 0 | 0 | - |
| alloc() [有空位] | 不变 | 不变 | 不变 | slot 指针 |
| alloc() [满] | 不变 | 不变 | 不变 | NULL |
| commit() | 不变 | +1 | +1 | - |
| drain_one() [成功] | +1 | 不变 | -1 | FB_OK |
| drain_one() [异步中] | 不变 | 不变 | 不变 | FB_BUSY |
| drain_one() [空] | 不变 | 不变 | 不变 | FB_NO_UPDATE |
| drain_all() | →tail | 不变 | 0 | FB_OK |
