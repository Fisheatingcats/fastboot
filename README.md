# FastBoot 库

## 概述

FastBoot 是一个可移植的 OTA Bootloader 库，适用于 STM32 等嵌入式平台。采用分层架构设计，通过接口抽象实现硬件解耦，核心代码完全平台无关。

主要特性：
- YMODEM-1K 固件接收（CRC-16 校验、错误重传）
- FWOT 格式 OTA 包解析与安装（流式 CRC32 + Readback 双重验证）
- 外部 Flash 暂存区管理（Staging）
- 异步写入队列（接收与 Flash 编程并行）
- 可裁剪的编译配置（`FASTBOOT_CFG_*` 宏控制功能开关）

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│                     应用层 (Application)                      │
│         调用 fastboot_ymodem_receive / fastboot_ota_install   │
├──────────────────────────────────────────────────────────────┤
│                     FastBoot 库                               │
│  ┌────────────────────────────────────────────────────────┐  │
│  │                   公共接口层                            │  │
│  │   fastboot_transport_t   传输接口 (read/write_byte)    │  │
│  │   fastboot_writer_t      写入接口 (begin/write/poll)   │  │
│  │   fastboot_flash_area_t  Flash 区域 (offset/size/ops)  │  │
│  │   fastboot_runtime_t     运行时 (tick_ms/feed_wdg)     │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │                   核心层 (Core)                         │  │
│  │   fastboot_ymodem.c    YMODEM-1K 协议                 │  │
│  │   fastboot_ota.c       FWOT 解析与安装                 │  │
│  │   fastboot_staging.c   Staging 区管理                  │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │                   队列层 (Queue)                        │  │
│  │   fastboot_queue.c     异步写入环形队列                │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │                   配置层 (Config)                       │  │
│  │   fastboot_config.h    校验必需宏定义                   │  │
│  │   fastboot_board_config.h  板级配置 (用户提供)          │  │
│  └────────────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────────────┤
│                     端口层 (Port)                             │
│   fastboot_stm32_runtime.c   tick_ms / feed_watchdog 实现    │
│   fastboot_uart.c            UART + DMA 环形接收             │
│   fastboot_iflash.c          内部 Flash 读写擦除             │
│   fastboot_w25q64.c          W25Q64 SPI Flash 驱动          │
└──────────────────────────────────────────────────────────────┘
```

## 核心接口

### fastboot_runtime_t — 运行时服务

提供系统 tick 和看门狗喂狗功能。

```c
typedef struct {
    uint32_t (*tick_ms)(void *ctx);
    void (*feed_watchdog)(void *ctx);
    void *ctx;
} fastboot_runtime_t;
```

STM32F4 平台示例：

```c
static uint32_t my_tick(void *ctx)      { (void)ctx; return HAL_GetTick(); }
static void     my_feed_wdg(void *ctx)  { (void)ctx; HAL_IWDG_Refresh(&hiwdg); }

const fastboot_runtime_t runtime = { .tick_ms = my_tick, .feed_watchdog = my_feed_wdg, .ctx = NULL };
```

### fastboot_transport_t — 传输接口

YMODEM 协议使用的字节级 I/O 接口。

```c
typedef struct {
    size_t (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
    void (*write_byte)(void *ctx, uint8_t byte);
    void *ctx;
} fastboot_transport_t;
```

- `read`：从传输介质读取数据，返回实际读取字节数，支持超时
- `write_byte`：发送单字节（用于 YMODEM 握手/应答）

### fastboot_flash_area_t — Flash 区域

描述一块 Flash 区域的范围和操作集，用于 staging 和 primary 分区。

```c
typedef struct {
    fboot_status_t (*read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *buf, size_t len);
    fboot_status_t (*erase)(void *ctx, uint32_t offset, uint32_t len);
} fastboot_flash_ops_t;

typedef struct {
    uint32_t offset;           // 绝对起始地址
    uint32_t size;             // 区域大小
    const fastboot_flash_ops_t *ops;
    void *ctx;
} fastboot_flash_area_t;
```

提供内联辅助函数：`fastboot_flash_area_read()`、`fastboot_flash_area_write()`、`fastboot_flash_area_erase()`，自动进行边界检查并加上 `area->offset` 偏移。

### fastboot_writer_t — 写入接口

支持同步和异步写入模式。

```c
typedef struct {
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*write_start)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*poll)(void *ctx);
    bool (*busy)(void *ctx);
    void *ctx;
} fastboot_writer_t;
```

- `begin`：通知写入目标总大小
- `write`：同步写入数据块
- `write_start`：启动异步写入
- `poll`：轮询异步写入状态
- `busy`：查询是否正在写入

### fboot_log_t — 日志接口

可选的日志输出接口，用于 OTA 安装过程中的状态输出。传 `NULL` 时所有日志调用编译为空操作。

```c
typedef struct {
    void (*puts)(void *ctx, const char *s);
    void (*dec32)(void *ctx, const char *label, uint32_t value);
    void *ctx;
} fboot_log_t;
```

当 `FASTBOOT_CFG_ENABLE_LOG` 为 0 时，`fboot_log_puts()` 和 `fboot_log_dec32()` 编译为空操作，零开销。

## 模块详解

### fastboot_ota.c — FWOT 安装流程

解析 FWOT 格式 OTA 包并写入主 Flash。

**流程：** 读取 Header → 验证 Magic (0x544F5746) → 验证 Header CRC32 → 验证向量表 → 擦除主 Flash → 分块写入（流式 CRC32）→ 验证 CRC → Readback 验证 → 完成

```c
fboot_status_t fastboot_ota_install(
    const fastboot_flash_area_t *staging,   // 暂存区（OTA 包来源）
    const fastboot_flash_area_t *primary,   // 主 Flash（安装目标）
    const fastboot_image_policy_t *policy,  // 向量表验证策略
    const fastboot_runtime_t *runtime,      // 运行时服务
    const fboot_log_t *log);                // 日志接口（可为 NULL）
```

### fastboot_staging.c — Staging 区管理

检查外部 Flash 暂存区是否有待安装固件，有则调用 `fastboot_ota_install()` 安装。

```c
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

### fastboot_ymodem.c — YMODEM-1K 协议

实现 YMODEM-1K 协议接收固件。

```c
fboot_status_t fastboot_ymodem_receive(
    const fastboot_transport_t *transport,  // 传输接口
    const fastboot_writer_t *writer,        // 写入目标
    const fastboot_runtime_t *runtime,      // 运行时服务
    uint32_t *out_size);                    // 接收字节数（输出）
```

**协议状态机：** IDLE → WAIT_HEADER（接收文件名包）→ RECEIVE_DATA（CRC 校验 + 数据接收）→ WAIT_EOT → WAIT_END → DONE

### fastboot_queue.c — 异步写入队列

环形缓冲区，实现 YMODEM 接收与 Flash 编程并行。

```c
void fastboot_queue_reset(fboot_queue_t *q);
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q);
void fastboot_queue_commit(fboot_queue_t *q);
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q,
    const fastboot_writer_t *writer, const fastboot_runtime_t *runtime,
    uint32_t *written);
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q,
    const fastboot_writer_t *writer, const fastboot_runtime_t *runtime,
    uint32_t *written);
```

## 异步工作原理

FastBoot 的异步机制是**单线程协作式调度**，不依赖 RTOS 或中断回调。核心思想：在等待串口数据的"空闲时间"里，去做 Flash 写入。

### 数据流概览

```
PC ──UART──→ DMA 环形缓冲区 ──CPU 拷贝──→ 队列槽位 ──CPU──→ Flash 写入
   (硬件自动)     (4096 B)       (alloc)    (commit)    (drain)
```

### 三层机制

**1. DMA 环形缓冲区（硬件层）**

STM32F4 端口使用 USART1 + DMA2 Stream2 循环模式接收。DMA 启动后永远在后台自动收数据，CPU 完全不参与。CPU 通过读取 NDTR 寄存器获取当前写入位置（head），与软件维护的读指针（tail）配合完成零拷贝读取。

```c
// port/stm32f4/fastboot_uart.c
#define UART_RX_DMA_BUF_SIZE 4096u   // 可缓冲约 4 个 YMODEM-1K 包
s_hdma_usart1_rx.Init.Mode = DMA_CIRCULAR;  // 循环模式，永不停止
```

**2. 包队列（协议层）**

YMODEM 接收到的数据包不直接写 Flash，而是存入环形队列。生产者（YMODEM）通过 `alloc → 填充 → commit` 提交包，消费者（Flash 写入）通过 `drain_one` 取出并写入。

```
队列槽位: [slot0] [slot1] [slot2] ... [slot7]
           ↑ head(消费)        tail(生产) ↑
```

**3. 见缝插针调度（应用层）**

状态机在等待串口数据时调用 `service_writer()` 消费队列：

```c
// src/core/fastboot_ymodem.c — io_read_exact()
while (done < len) {
    got = transport->read(...);       // 尝试读串口
    if (got > 0) { done += got; continue; }
    service_writer();                 // 没数据？去写 Flash
}
```

此外还有两个水位保护：
- **高水位（3/4 满）**：主动排水一个槽位
- **队列满**：强制排水，否则不接收新包

### 时间线示例

```
MCU:  [收包1] [收包2] [收包3] [写Flash#0] [收包4] [写Flash#1] [收包5] ...
DMA:       [自动收PC数据→填入缓冲区]           [继续自动收...]
PC:   [发包1→→→] [发包2→→→] [发包3→→→] [发包4→→→] [发包5→→→]
```

MCU 发完 ACK 后去写 Flash，此时 PC 发出的下一个包由 DMA 硬件自动收入缓冲区，不会丢失。MCU 写完 Flash 回来后，从 DMA 缓冲区读出已收的数据继续处理。

### 同步 vs 异步写入

`fastboot_writer_t` 支持两种模式，`drain_one` 自动选择：

| 模式 | 回调 | 行为 |
|---|---|---|
| 同步 | `write` | 阻塞直到写入完成 |
| 异步 | `write_start` + `poll` + `busy` | 启动后立即返回，后续 poll 轮询完成状态 |

## 配置

所有功能由 `fastboot_board_config.h` 中的宏控制，`fastboot_config.h` 负责校验必需宏是否存在。

| 宏 | 类型 | 说明 |
|---|---|---|
| `FASTBOOT_BOARD_CONFIG_INCLUDED` | define | 板级配置标记，必须定义 |
| `FASTBOOT_CFG_ENABLE_YMODEM` | 0/1 | 启用 YMODEM 接收模块 |
| `FASTBOOT_CFG_ENABLE_FWOT` | 0/1 | 启用 FWOT 包解析与安装 |
| `FASTBOOT_CFG_ENABLE_STAGING` | 0/1 | 启用 Staging 区管理 |
| `FASTBOOT_CFG_ENABLE_ASYNC_SINK` | 0/1 | 启用异步写入（队列模式） |
| `FASTBOOT_CFG_ENABLE_READBACK_VERIFY` | 0/1 | 启用写入后回读验证 |
| `FASTBOOT_CFG_ENABLE_LOG` | 0/1 | 启用日志输出 |
| `FASTBOOT_CFG_ENABLE_WATCHDOG` | 0/1 | 启用看门狗喂狗 |
| `FASTBOOT_CFG_QUEUE_DEPTH` | ≥2 | 队列槽位数 |
| `FASTBOOT_CFG_YMODEM_PACKET_SIZE` | ≥128 | YMODEM 包大小（字节） |
| `FASTBOOT_CFG_STAGING_CAPACITY` | uint32 | 暂存区总容量（字节） |

板级配置示例（`fastboot_board_config.h`）：

```c
#ifndef FASTBOOT_BOARD_CONFIG_H
#define FASTBOOT_BOARD_CONFIG_H

#define FASTBOOT_BOARD_CONFIG_INCLUDED     1
#define FASTBOOT_CFG_ENABLE_YMODEM         1
#define FASTBOOT_CFG_ENABLE_FWOT           1
#define FASTBOOT_CFG_ENABLE_STAGING        1
#define FASTBOOT_CFG_ENABLE_ASYNC_SINK     1
#define FASTBOOT_CFG_ENABLE_READBACK_VERIFY 1
#define FASTBOOT_CFG_ENABLE_LOG            1
#define FASTBOOT_CFG_ENABLE_WATCHDOG       1
#define FASTBOOT_CFG_QUEUE_DEPTH           8
#define FASTBOOT_CFG_YMODEM_PACKET_SIZE    1024
#define FASTBOOT_CFG_STAGING_CAPACITY      0x400000u

#define FASTBOOT_EXTFLASH_OTA_SIZE         0x400000u
#define FASTBOOT_EXTFLASH_OTA_OFFSET       0x000000u
#define FASTBOOT_APP_FLASH_BASE            0x08008000u
#define FASTBOOT_APP_FLASH_SIZE            0x00078000u
#define FASTBOOT_APP_FLASH_END             0x08080000u
#define FASTBOOT_SRAM_BASE                 0x20000000u
#define FASTBOOT_SRAM_END                  0x20020000u

#endif
```

## 移植指南

### 1. 创建 fastboot_board_config.h

在项目配置目录中创建 `fastboot_board_config.h`，定义所有 `FASTBOOT_CFG_*` 宏和内存布局宏（参见上方配置章节）。

### 2. 实现 fastboot_runtime_t

```c
#include "fastboot_runtime.h"
#include "your_hal.h"

static uint32_t port_tick_ms(void *ctx) { (void)ctx; return HAL_GetTick(); }
static void port_feed_wdg(void *ctx)    { (void)ctx; /* 喂狗 */ }

const fastboot_runtime_t g_runtime = {
    .tick_ms      = port_tick_ms,
    .feed_watchdog = port_feed_wdg,
    .ctx           = NULL,
};
```

STM32F4 移植可直接使用 `port/stm32f4/fastboot_stm32_runtime.c` 提供的 `fastboot_stm32_runtime()`。

### 3. 实现 fastboot_transport_t

```c
#include "fastboot_transport.h"

// 基于 UART DMA 环形缓冲区的实现
static size_t uart_read(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms) {
    // 从 DMA 环形缓冲区读取，超时返回实际读取字节数
}
static void uart_write_byte(void *ctx, uint8_t byte) {
    // 发送单字节
}

const fastboot_transport_t g_transport = {
    .read       = uart_read,
    .write_byte = uart_write_byte,
    .ctx        = NULL,
};
```

### 4. 实现 fastboot_flash_area_t

```c
#include "fastboot_flash.h"

// 内部 Flash 操作
static fboot_status_t iflash_read(void *ctx, uint32_t off, uint8_t *b, size_t l)  { /* ... */ }
static fboot_status_t iflash_write(void *ctx, uint32_t off, const uint8_t *b, size_t l) { /* ... */ }
static fboot_status_t iflash_erase(void *ctx, uint32_t off, uint32_t l) { /* ... */ }

static const fastboot_flash_ops_t iflash_ops = {
    .read = iflash_read, .write = iflash_write, .erase = iflash_erase,
};

const fastboot_flash_area_t primary = {
    .offset = FASTBOOT_APP_FLASH_BASE,
    .size   = FASTBOOT_APP_FLASH_SIZE,
    .ops    = &iflash_ops,
    .ctx    = NULL,
};
```

### 5. 实现 fastboot_writer_t

可使用队列层提供的 writer，或自行实现同步 writer。

### 6. 集成到项目

```cmake
set(FASTBOOT_PORT "stm32f4" CACHE STRING "" FORCE)
set(FASTBOOT_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/00_Config" CACHE PATH "" FORCE)
set(FASTBOOT_PLATFORM_INCLUDES ... CACHE STRING "" FORCE)
set(FASTBOOT_ENABLE_OTA ON CACHE BOOL "" FORCE)
add_subdirectory(fastboot)
target_link_libraries(your_target PRIVATE fastboot)
```

## API 参考

### fastboot_ymodem_receive()

```c
fboot_status_t fastboot_ymodem_receive(
    const fastboot_transport_t *transport,
    const fastboot_writer_t *writer,
    const fastboot_runtime_t *runtime,
    uint32_t *out_size);
```

通过 YMODEM-1K 协议接收固件数据并写入目标。返回 `FB_OK` 表示成功。

### fastboot_staging_install_if_pending()

```c
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

检查暂存区是否有待安装固件（Magic = 0x544F5746），有则调用 OTA 安装。返回 `FB_OK` 安装成功，`FB_NO_UPDATE` 无更新。

### fastboot_ota_install()

```c
fboot_status_t fastboot_ota_install(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

从暂存区读取 FWOT 包并安装到主 Flash。

### 状态码

```c
typedef enum {
    FB_OK = 0,           // 成功
    FB_NO_UPDATE = 1,    // 无更新
    FB_BUSY = 2,         // 忙
    FB_ERR_PARAM = -1,   // 参数错误
    FB_ERR_RANGE = -2,   // 范围错误
    FB_ERR_FLASH = -3,   // Flash 错误
    FB_ERR_VERIFY = -4,  // 验证失败
    FB_ERR_FORMAT = -5,  // 格式错误
    FB_ERR_CRC = -6,     // CRC 错误
    FB_ERR_IO = -7,      // I/O 错误
    FB_ERR_ID = -8,      // ID 错误
} fboot_status_t;
```

## 构建与测试

### CMake 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### Host 端测试

```bash
cmake -S . -B build-host -DFASTBOOT_BUILD_TESTS=ON
cmake --build build-host
ctest --test-dir build-host -C Debug --output-on-failure
```

`FASTBOOT_PORT=none` 时，CMake 自动生成 `fastboot_board_config.h` 到构建目录，提供测试所需的全部配置宏。OTA/Staging 源码仍会编译（`FASTBOOT_ENABLE_OTA` 在 tests 模式下由 CMake 强制开启）。

### 集成到已有项目

参见上方 [移植指南](#移植指南) 第 6 步的 CMake 配置。

## 许可证

MIT License
