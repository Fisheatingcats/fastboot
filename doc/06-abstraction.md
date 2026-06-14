# 硬件抽象层设计

## 目录

1. [设计理念](#1-设计理念)
2. [fastboot_runtime_t — 系统服务](#2-fastboot_runtime_t--系统服务)
3. [fastboot_transport_t — 通信层](#3-fastboot_transport_t--通信层)
4. [fastboot_flash_area_t + fastboot_flash_ops_t — 存储层](#4-fastboot_flash_area_t--fastboot_flash_ops_t--存储层)
5. [fastboot_writer_t — 数据写入器](#5-fastboot_writer_t--数据写入器)
6. [flash_area_t 地址映射机制](#6-flash_area_t-地址映射机制)
7. [为什么分离 transport 和 writer](#7-为什么分离-transport-和-writer)
8. [移植新平台的步骤](#8-移植新平台的步骤)
9. [接口组合示例](#9-接口组合示例)

---

## 1. 设计理念

FastBoot 通过四组函数表（函数指针结构体）实现硬件抽象，遵循以下原则：

- **接口与实现分离**：core 层只依赖接口定义，不依赖具体实现
- **函数表模式**：每个接口是一组函数指针 + 上下文指针
- **零开销抽象**：通过 `static inline` 辅助函数封装参数检查和地址转换
- **编译时裁剪**：通过宏开关禁用未使用的接口回调

### 四大接口总览

```
┌─────────────────────────────────────────────────────────────┐
│                     FastBoot Core 层                         │
│                                                             │
│   YMODEM ──→ 队列 ──→ Writer ──→ Flash                     │
│     │                         │                             │
│     └──→ Runtime (tick/wdt) ←─┘                             │
└─────────────────────────────────────────────────────────────┘
         │            │            │            │
         v            v            v            v
   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
   │transport │ │ runtime  │ │  flash   │ │  writer  │
   │ 字节 I/O │ │ tick/wdt │ │ 区域操作 │ │ 数据写入 │
   └──────────┘ └──────────┘ └──────────┘ └──────────┘
```

## 2. fastboot_runtime_t — 系统服务

### 2.1 接口定义

```c
typedef struct {
    uint32_t (*tick_ms)(void *ctx);       // 获取毫秒计时
    void     (*feed_watchdog)(void *ctx); // 喂看门狗
    void     *ctx;                        // 平台上下文
} fastboot_runtime_t;
```

### 2.2 安全包装函数

```c
static inline uint32_t fastboot_runtime_tick_ms(
    const fastboot_runtime_t *runtime)
{
    return (runtime && runtime->tick_ms)
        ? runtime->tick_ms(runtime->ctx) : 0u;
}

static inline void fastboot_runtime_feed_watchdog(
    const fastboot_runtime_t *runtime)
{
#if FASTBOOT_CFG_ENABLE_WATCHDOG
    if (runtime && runtime->feed_watchdog) {
        runtime->feed_watchdog(runtime->ctx);
    }
#else
    (void)runtime;
#endif
}
```

### 2.3 设计要点

- `tick_ms` 用于超时检测（YMODEM 接收超时、会话轮询超时）
- `feed_watchdog` 在长时间操作中保活（Flash 擦除、OTA 安装）
- 看门狗可通过 `FASTBOOT_CFG_ENABLE_WATCHDOG=0` 编译时禁用
- 安全包装函数处理 NULL 指针，避免 core 层空指针检查

### 2.4 典型实现

```c
// STM32 SysTick 实现
static uint32_t stm32_tick_ms(void *ctx) { return HAL_GetTick(); }
static void stm32_feed_wdt(void *ctx) { HAL_IWDG_Refresh(&hiwdg); }

const fastboot_runtime_t board_runtime = {
    .tick_ms = stm32_tick_ms,
    .feed_watchdog = stm32_feed_wdt,
    .ctx = NULL,
};
```

## 3. fastboot_transport_t — 通信层

### 3.1 接口定义

```c
typedef struct {
    size_t (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
    void   (*write_byte)(void *ctx, uint8_t byte);
    void   *ctx;
} fastboot_transport_t;
```

### 3.2 设计要点

- **字节级粒度**：`read` 返回实际读取字节数，`write_byte` 发送单字节
- **超时机制**：`read` 的 `timeout_ms` 参数，0 表示不等待
- **阻塞式**：`write_byte` 同步阻塞直到发送完成
- **无缓冲**：core 层不维护传输缓冲区，由平台实现负责

### 3.3 典型实现

```c
// UART 实现
static size_t uart_read(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;
    if (HAL_UART_Receive(huart, buf, len, timeout_ms) == HAL_OK) {
        return len;
    }
    return 0;
}

static void uart_write_byte(void *ctx, uint8_t byte)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;
    HAL_UART_Transmit(huart, &byte, 1, HAL_MAX_DELAY);
}

const fastboot_transport_t uart_transport = {
    .read = uart_read,
    .write_byte = uart_write_byte,
    .ctx = &huart2,
};
```

### 3.4 使用场景

```
YMODEM 接收
    │
    ├── rx_byte()     → transport->read(ctx, &c, 1, timeout)
    ├── io_read_exact() → transport->read(ctx, buf, len, 0) 循环
    └── tx_byte()      → transport->write_byte(ctx, c)
```

## 4. fastboot_flash_area_t + fastboot_flash_ops_t — 存储层

### 4.1 接口定义

```c
// 操作函数表
typedef struct fastboot_flash_ops {
    fboot_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *buf, size_t len);
    fboot_status_t (*erase)(void *ctx, uint32_t offset, uint32_t len);
} fastboot_flash_ops_t;

// 区域描述
typedef struct {
    uint32_t offset;                  // 绝对起始偏移
    uint32_t size;                    // 区域大小
    const fastboot_flash_ops_t *ops;  // 操作函数表
    void *ctx;                        // 平台上下文
} fastboot_flash_area_t;
```

### 4.2 设计要点

- **区域抽象**：`flash_area_t` 描述一个连续的 Flash 区域，包含地址范围和操作集
- **地址映射**：内联辅助函数自动将相对偏移转换为绝对地址
- **范围检查**：每次操作前校验 offset+len 是否在区域内
- **多实例**：同一个 `flash_ops` 可被多个 `flash_area` 引用（如 staging 和 backup 共用 W25Q64 驱动）

### 4.3 典型实现

```c
// W25Q64 SPI Flash 实现
static fboot_status_t w25q_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
    W25Q_Handle *h = (W25Q_Handle *)ctx;
    W25Q_ReadData(h, offset, buf, len);
    return FB_OK;
}

const fastboot_flash_ops_t w25q_ops = {
    .read = w25q_read,
    .write = w25q_write,
    .erase = w25q_erase,
};
```

## 5. fastboot_writer_t — 数据写入器

### 5.1 接口定义

```c
typedef struct {
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    fboot_status_t (*write)(void *ctx, uint32_t offset,
                            const uint8_t *data, size_t len);
    fboot_status_t (*write_start)(void *ctx, uint32_t offset,
                                  const uint8_t *data, size_t len);
    fboot_status_t (*poll)(void *ctx);
    bool           (*busy)(void *ctx);
    void           *ctx;
} fastboot_writer_t;
```

### 5.2 同步模式

```
begin(ctx, size)           ← 初始化写入上下文
    │
    v
write(ctx, off, data, len) ← 同步写入，阻塞直到完成
    │
    v
write(ctx, off, data, len) ← 继续写入
    │
    ...
```

只需实现 `begin` 和 `write` 两个回调。

### 5.3 异步模式

```
begin(ctx, size)               ← 初始化
    │
    v
write_start(ctx, off, data, len) ← 启动异步写入
    │
    ├── FB_BUSY → 标记 writing=true
    │
    v
poll(ctx)                       ← 轮询完成状态
    │
    ├── FB_BUSY → 继续等待
    └── FB_OK   → 检查 busy()
                   └── false → 写入完成
```

需要实现 `begin`、`write_start`、`poll`、`busy` 四个回调。

### 5.4 与队列的协作

```c
// drain_one 中的写入路径选择
if (slot->writing) {
    rc = writer->poll(writer->ctx);              // 异步：轮询
} else if (writer->write_start && writer->poll && writer->busy) {
    rc = writer->write_start(writer->ctx, ...);  // 异步：启动
} else {
    rc = writer->write(writer->ctx, ...);        // 同步：直接写
}
```

## 6. flash_area_t 地址映射机制

### 6.1 映射原理

```
用户视角（相对偏移）           底层视角（绝对地址）
+-------------------+         +-------------------+
| offset=0          |         | area.offset=0x08010000
| size=480KB        |   →    | 实际地址=0x08010000+user_off
| ops=stm32_flash   |         | ops->read(ctx, abs_addr, ...)
+-------------------+         +-------------------+
```

### 6.2 地址转换代码

```c
// fastboot_flash_area_read 内部
return area->ops->read(area->ctx, area->offset + offset, buf, len);
//                              ^^^^^^^^^^^^^^^^^^^^^^^^^^^
//                              绝对地址 = 区域基址 + 相对偏移
```

### 6.3 安全检查

```c
static inline fboot_status_t fastboot_flash_area_check(
    const fastboot_flash_area_t *area, uint32_t offset, size_t len)
{
    if (!area || !area->ops) return FB_ERR_PARAM;
    // 范围检查：offset 不超过 size，len 不超过剩余空间
    if (offset > area->size || len > (size_t)(area->size - offset))
        return FB_ERR_RANGE;
    // 溢出检查：绝对地址不回绕
    if (area->offset + offset < area->offset)
        return FB_ERR_RANGE;
    return FB_OK;
}
```

### 6.4 多区域统一操作

```c
// staging 和 primary 使用相同的 API，不同的 ops
fastboot_flash_area_read(&staging, 0, buf, 64);   // → w25q64_read(0x00000000, ...)
fastboot_flash_area_read(&primary, 0, buf, 64);   // → stm32_flash_read(0x08010000, ...)
```

## 7. 为什么分离 transport 和 writer

### 7.1 速率不匹配

```
UART 接收速率: ~115KB/s (921600 baud)
Flash 写入速率: ~50KB/s (内部 Flash) 或 ~200KB/s (W25Q64)
```

如果同步处理：接收一个包 → 等待写入完成 → 接收下一个包，总时间 = 接收时间 + 写入时间。

### 7.2 流水线重叠

```
分离后：
    接收: [包0] [包1] [包2] [包3] [包4] ...
    写入:       [包0] [包1] [包2] [包3] ...

总时间 ≈ max(接收时间, 写入时间)
```

### 7.3 解耦收益

| 方面 | 同步耦合 | 分离解耦 |
|------|----------|----------|
| 吞吐量 | 受限于较慢者 | 接近较快者 |
| 缓冲需求 | 无 | 队列深度个包 |
| 复杂度 | 低 | 中（需队列管理） |
| 可扩展性 | 差 | 好（可换 writer 实现） |

## 8. 移植新平台的步骤

### 8.1 创建板级配置

```c
// fastboot_board_config.h
#define FASTBOOT_BOARD_CONFIG_INCLUDED    1
#define FASTBOOT_CFG_ENABLE_YMODEM        1
#define FASTBOOT_CFG_ENABLE_FWOT          1
#define FASTBOOT_CFG_ENABLE_STAGING       1
#define FASTBOOT_CFG_ENABLE_ASYNC_SINK    0
#define FASTBOOT_CFG_ENABLE_READBACK_VERIFY 1
#define FASTBOOT_CFG_ENABLE_LOG           1
#define FASTBOOT_CFG_ENABLE_WATCHDOG      1
#define FASTBOOT_CFG_QUEUE_DEPTH          4
#define FASTBOOT_CFG_YMODEM_PACKET_SIZE   1024
#define FASTBOOT_CFG_STAGING_CAPACITY     (256*1024)
```

### 8.2 实现 transport

```c
static size_t my_read(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
static void my_write_byte(void *ctx, uint8_t byte);

const fastboot_transport_t my_transport = {
    .read = my_read,
    .write_byte = my_write_byte,
    .ctx = &my_uart_handle,
};
```

### 8.3 实现 runtime

```c
static uint32_t my_tick_ms(void *ctx);
static void my_feed_watchdog(void *ctx);

const fastboot_runtime_t my_runtime = {
    .tick_ms = my_tick_ms,
    .feed_watchdog = my_feed_watchdog,
    .ctx = NULL,
};
```

### 8.4 实现 flash_ops

```c
static fboot_status_t my_flash_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
static fboot_status_t my_flash_write(void *ctx, uint32_t offset, const uint8_t *buf, size_t len);
static fboot_status_t my_flash_erase(void *ctx, uint32_t offset, uint32_t len);

const fastboot_flash_ops_t my_flash_ops = {
    .read = my_flash_read,
    .write = my_flash_write,
    .erase = my_flash_erase,
};
```

### 8.5 实现 writer（可选异步）

```c
static fboot_status_t my_writer_begin(void *ctx, uint32_t size);
static fboot_status_t my_writer_write(void *ctx, uint32_t offset,
                                      const uint8_t *data, size_t len);

const fastboot_writer_t my_writer = {
    .begin = my_writer_begin,
    .write = my_writer_write,
    .ctx = &my_flash_area,
};
```

### 8.6 组装 flash_area

```c
fastboot_flash_area_t staging = {
    .offset = 0,
    .size = STAGING_CAPACITY,
    .ops = &my_flash_ops,
    .ctx = &my_flash_ctx,
};

fastboot_flash_area_t primary = {
    .offset = APP_FLASH_BASE,
    .size = APP_FLASH_SIZE,
    .ops = &my_flash_ops,
    .ctx = &my_flash_ctx,
};
```

## 9. 接口组合示例

### 9.1 YMODEM 接收组合

```c
fastboot_ymodem_receive(&transport, &writer, &runtime, &size);
//                     ─────┬─────  ───┬───  ───┬───
//                          │          │        │
//                     字节 I/O    数据写入   系统服务
```

### 9.2 OTA 安装组合

```c
fastboot_ota_install(&staging, &primary, &policy, &runtime, &log);
//                  ────┬───  ────┬───  ───┬───  ───┬───  ──┬─
//                      │        │        │        │       │
//                 源 Flash  目标 Flash  策略    系统服务  日志
```

### 9.3 Staging 检测组合

```c
fastboot_staging_install_if_pending(&staging, &primary, &policy, &runtime, &log);
// 内部调用：fastboot_ota_install(staging, primary, policy, runtime, log)
```

### 9.4 完整启动流程

```
main()
    │
    ├─ 初始化硬件
    │
    ├─ fastboot_staging_install_if_pending(
    │      &staging,      ← W25Q64 flash_area
    │      &primary,      ← STM32 内部 flash_area
    │      &policy,       ← 向量校验回调
    │      &runtime,      ← SysTick + IWDG
    │      &log)          ← UART 日志
    │
    ├─ fastboot_ymodem_receive(
    │      &transport,    ← UART 字节 I/O
    │      &staging_writer, ← 写入 staging 区的 writer
    │      &runtime,      ← SysTick + IWDG
    │      &size)
    │
    └─ jump_to_app()
```

### 9.5 接口依赖关系图

```
fastboot_ymodem.h
    ├── fastboot_transport.h   (read/write_byte)
    ├── fastboot_writer.h      (begin/write/poll/busy)
    └── fastboot_runtime.h     (tick_ms/feed_watchdog)

fastboot_ota.h
    ├── fastboot_flash.h       (flash_area read/write/erase)
    ├── fastboot_runtime.h     (tick_ms/feed_watchdog)
    └── fastboot_log.h         (puts/dec32)

fastboot_queue.h
    ├── fastboot_writer.h      (write/poll/busy)
    └── fastboot_runtime.h     (feed_watchdog)

fastboot_staging.h
    ├── fastboot_flash.h       (flash_area)
    ├── fastboot_ota.h         (ota_install)
    └── fastboot_runtime.h     (runtime)
```

**核心依赖链**：status → config → flash/runtime/transport/writer → queue → ymodem/ota → staging
