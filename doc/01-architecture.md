# FastBoot 整体架构

## 目录

1. [设计目标](#1-设计目标)
2. [分层架构](#2-分层架构)
3. [接口抽象原则](#3-接口抽象原则)
4. [数据流](#4-数据流)
5. [编译时配置机制](#5-编译时配置机制)
6. [模块依赖关系](#6-模块依赖关系)
7. [状态码体系](#7-状态码体系)

---

## 1. 设计目标

FastBoot 是一个面向 STM32 等嵌入式平台的轻量级 OTA 引导库，核心设计目标：

- **平台无关**：core 层不依赖任何 HAL 或硬件寄存器
- **零动态内存分配**：所有缓冲区静态分配
- **可裁剪**：通过编译时开关启用/禁用各功能模块
- **接收与存储解耦**：YMODEM 接收和 Flash 写入通过队列异步协作

## 2. 分层架构

```
+-------------------------------------------------------------+
|                     应用层 (bootloader)                       |
|  调用 fastboot_ymodem_receive() / fastboot_staging_install() |
+-------------------------------------------------------------+
         |                    |                    |
         v                    v                    v
+----------------+  +------------------+  +------------------+
|   core 层      |  |   queue 层       |  |   port 层        |
|                |  |                  |  |                  |
| fastboot_ota   |  | fastboot_queue   |  | stm32f4/         |
| fastboot_ymodem|  | (环形缓冲区)      |  | uart, flash,     |
| fastboot_staging|  |                  |  | w25q64, tick     |
+----------------+  +------------------+  +------------------+
         |                    |                    |
         v                    v                    v
+-------------------------------------------------------------+
|                  硬件抽象层 (HAL 接口)                        |
|  fastboot_transport_t  |  fastboot_writer_t                 |
|  fastboot_flash_area_t |  fastboot_runtime_t                |
|  fastboot_log_t                                         |
+-------------------------------------------------------------+
```

**分层职责**：

| 层级 | 目录 | 职责 |
|------|------|------|
| core | `src/core/` | 协议解析、OTA 安装、staging 检测（硬件无关） |
| queue | `src/queue/` | 环形包队列，解耦接收与写入 |
| port | `port/stm32f4/` | 平台适配：UART、内部 Flash、W25Q64、SysTick |
| include | `include/` | 公共接口定义、配置校验 |

## 3. 接口抽象原则

FastBoot 通过四组函数表（函数指针结构体）实现硬件抽象：

### 3.1 fastboot_transport_t — 通信层

```c
typedef struct {
    size_t (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
    void   (*write_byte)(void *ctx, uint8_t byte);
    void   *ctx;
} fastboot_transport_t;
```

字节级 I/O 抽象，YMODEM 协议通过此接口收发数据。可适配 UART、USB CDC、SPI 等。

### 3.2 fastboot_writer_t — 写入器

```c
typedef struct {
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*write_start)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*poll)(void *ctx);
    bool           (*busy)(void *ctx);
    void           *ctx;
} fastboot_writer_t;
```

支持同步（`write`）和异步（`write_start` + `poll` + `busy`）两种模式。

### 3.3 fastboot_flash_area_t — 存储层

```c
typedef struct {
    uint32_t offset;                  // 区域绝对起始偏移
    uint32_t size;                    // 区域大小
    const fastboot_flash_ops_t *ops;  // read/write/erase 函数表
    void *ctx;                        // 平台上下文
} fastboot_flash_area_t;
```

将 Flash 区域抽象为"带地址映射的操作对象"，内部自动将相对偏移转换为绝对地址。

### 3.4 fastboot_runtime_t — 系统服务

```c
typedef struct {
    uint32_t (*tick_ms)(void *ctx);
    void     (*feed_watchdog)(void *ctx);
    void     *ctx;
} fastboot_runtime_t;
```

提供毫秒计时和看门狗喂狗，用于超时检测和长时间操作保活。

### 设计理念

```
transport (字节 I/O)  ──→  YMODEM 协议  ──→  队列  ──→  writer (数据写入)
                                                         |
flash_area (Flash 操作) ←── OTA 安装 ←── staging 检测    |
                                                         |
runtime (tick/wdt) ←── 超时检测、喂狗保活 ←──────────────┘
```

**为什么分离 transport 和 writer？** 接收（UART 字节流）和存储（Flash 写入）速率不同，队列在中间解耦，实现流水线重叠。

## 4. 数据流

### 4.1 YMODEM 接收数据流

```
发送端 (PC)                    接收端 (MCU)
    |                              |
    |  SOH/STX [SEQ][DATA][CRC]   |
    | ──────────────────────────→  |
    |                              |── receive_packet()
    |                              |   ├─ 校验序号反码
    |                              |   └─ 校验 CRC-16
    |         ACK                  |
    | ←──────────────────────────  |
    |                              |── queue_alloc() → 填充 slot
    |                              |── queue_commit() → 推进 tail
    |                              |
    |                              |── service_writer() [I/O 等待间隙]
    |                              |   └─ queue_drain_one()
    |                              |       └─ writer->write() → Flash
```

### 4.2 OTA 安装数据流

```
staging Flash                  primary Flash
    |                              |
    |  读取 header (64B)           |
    |── validate_header()          |
    |  读取 image 前 8B            |
    |── vector_is_valid()          |
    |                              |
    |  分块读取 (1KB)              |  擦除全区
    |── fastboot_crc32() ─────────→|── fastboot_flash_area_write()
    |  ...循环...                  |
    |                              |
    |── CRC 比对 ─────────────────→|  可选 readback 验证
```

## 5. 编译时配置机制

FastBoot 采用两级配置：板级定义 → 库级校验。

### 5.1 板级配置文件

BSP 必须提供 `fastboot_board_config.h`，定义所有必需宏：

```c
// fastboot_board_config.h (板级提供)
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

### 5.2 库级校验

`fastboot_config.h` 包含板级配置并进行编译期断言：

```c
#include "fastboot_board_config.h"

#ifndef FASTBOOT_BOARD_CONFIG_INCLUDED
#error "Board must provide fastboot_board_config.h"
#endif

#ifndef FASTBOOT_CFG_ENABLE_YMODEM
#error "FASTBOOT_CFG_ENABLE_YMODEM is required"
#endif
// ... 其他必需宏 ...

#if FASTBOOT_CFG_QUEUE_DEPTH < 2u
#error "FASTBOOT_CFG_QUEUE_DEPTH must be at least 2"
#endif
```

### 5.3 配置项一览

| 宏名 | 类型 | 说明 |
|------|------|------|
| `FASTBOOT_CFG_ENABLE_YMODEM` | 0/1 | 启用 YMODEM 接收 |
| `FASTBOOT_CFG_ENABLE_FWOT` | 0/1 | 启用 FWOT 包解析和 OTA 安装 |
| `FASTBOOT_CFG_ENABLE_STAGING` | 0/1 | 启用 staging 区自动安装 |
| `FASTBOOT_CFG_ENABLE_ASYNC_SINK` | 0/1 | 启用异步写入器支持 |
| `FASTBOOT_CFG_ENABLE_READBACK_VERIFY` | 0/1 | 安装后回读 CRC 验证 |
| `FASTBOOT_CFG_ENABLE_LOG` | 0/1 | 启用日志输出 |
| `FASTBOOT_CFG_ENABLE_WATCHDOG` | 0/1 | 启用看门狗喂狗 |
| `FASTBOOT_CFG_QUEUE_DEPTH` | ≥2 | 包队列深度 |
| `FASTBOOT_CFG_YMODEM_PACKET_SIZE` | ≥128 | YMODEM 最大包载荷 |
| `FASTBOOT_CFG_STAGING_CAPACITY` | 字节 | staging 区总容量 |

## 6. 模块依赖关系

```
fastboot.h (聚合头文件)
    ├── fastboot_config.h
    │   └── fastboot_board_config.h (板级提供)
    ├── fastboot_status.h          (状态码，无依赖)
    ├── fastboot_flash.h           (Flash 抽象，依赖 status)
    ├── fastboot_runtime.h         (运行时抽象，依赖 config)
    ├── fastboot_transport.h       (传输抽象，无依赖)
    ├── fastboot_writer.h          (写入器抽象，依赖 status)
    ├── fastboot_log.h             (日志抽象，依赖 config)
    ├── fastboot_ota.h             (OTA 安装，依赖 flash/runtime/log)
    ├── fastboot_queue.h           (包队列，依赖 config/runtime/writer)
    ├── fastboot_staging.h         (staging 检测，依赖 flash/ota/runtime)
    └── fastboot_ymodem.h          (YMODEM 接收，依赖 transport/writer/runtime)
```

**依赖规则**：
- `include/` 中的抽象接口只依赖 `fastboot_status.h` 和 `fastboot_config.h`
- `src/core/` 模块依赖接口层，不依赖 port 层
- `src/queue/` 依赖 writer 接口，不依赖具体实现
- port 层实现接口，可依赖 HAL

## 7. 状态码体系

所有公共 API 统一返回 `fboot_status_t`：

```c
typedef enum {
    FB_OK         =  0,   // 操作成功
    FB_NO_UPDATE  =  1,   // 无待安装固件
    FB_BUSY       =  2,   // 异步操作进行中
    FB_ERR_PARAM  = -1,   // 参数无效
    FB_ERR_RANGE  = -2,   // 越界
    FB_ERR_FLASH  = -3,   // Flash 底层错误
    FB_ERR_VERIFY = -4,   // 回读校验失败
    FB_ERR_FORMAT = -5,   // 数据格式错误
    FB_ERR_CRC    = -6,   // CRC 校验失败
    FB_ERR_IO     = -7,   // I/O 传输错误
    FB_ERR_ID     = -8,   // 标识不匹配
} fboot_status_t;
```

**约定**：正值（含零）为成功/信息，负值为错误。各模块可按需忽略不相关的状态码，但不应引入新的状态码。
