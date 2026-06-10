# FastBoot 库

轻量级 OTA Bootloader 库，适用于 STM32 等嵌入式平台。

## 目录

- [库概述](#库概述)
- [架构设计](#架构设计)
- [模块详解](#模块详解)
- [移植指南](#移植指南)
- [API 参考](#api-参考)
- [内部实现](#内部实现)

## 库概述

FastBoot 是一个可移植的 OTA Bootloader 库，采用分层架构设计：

- **核心层 (Core)**: 硬件无关的协议和逻辑
- **队列层 (Queue)**: 异步写入队列
- **端口层 (Port)**: 平台相关的硬件驱动

### 设计原则

1. **硬件抽象**: 通过 Port 接口隔离硬件依赖
2. **异步处理**: 队列机制实现接收与写入并行
3. **接口驱动**: 使用函数指针实现多态
4. **最小依赖**: 核心层仅依赖标准 C 库

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 (Application)                       │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  fastboot_main.c                                      │  │
│  │  - 调用 FastBoot API                                  │  │
│  │  - 实现具体业务逻辑                                    │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    FastBoot 库                               │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                    公共 API 层                         │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  fastboot.h (聚合头文件)                        │  │  │
│  │  │  fastboot_staging.h (OTA 安装)                  │  │  │
│  │  │  fastboot_ymodem.h (YMODEM 接收)                │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  ├───────────────────────────────────────────────────────┤  │
│  │                    核心层 (Core)                       │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  fastboot_ota.c      # OTA 包解析和安装         │  │  │
│  │  │  fastboot_staging.c  # Staging 区管理           │  │  │
│  │  │  fastboot_ymodem.c   # YMODEM-1K 协议          │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  ├───────────────────────────────────────────────────────┤  │
│  │                    队列层 (Queue)                      │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  fastboot_queue.c    # 异步写入队列             │  │  │
│  │  │  - 环形缓冲区                                   │  │  │
│  │  │  - 异步状态机                                   │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  ├───────────────────────────────────────────────────────┤  │
│  │                    接口层 (Interface)                  │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  fastboot_io.h       # I/O 接口                 │  │  │
│  │  │  fastboot_sink.h     # 数据写入接口             │  │  │
│  │  │  fastboot_port.h     # 硬件抽象接口             │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  ├───────────────────────────────────────────────────────┤  │
│  │                    端口层 (Port)                       │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  fastboot_port.c     # 平台抽象实现             │  │  │
│  │  │  fastboot_uart.c     # UART 驱动                │  │  │
│  │  │  fastboot_iflash.c   # 内部 Flash 驱动          │  │  │
│  │  │  fastboot_w25q64.c   # W25Q64 SPI Flash 驱动   │  │  │
│  │  │  hal_msp_boot.c      # HAL MSP 初始化          │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## 模块详解

### 1. 核心模块 (Core)

#### fastboot_ota.c

OTA 包解析和安装模块。

**功能:**
- 解析 FWOT 格式的 OTA 包
- 验证 magic、header CRC32、image CRC32
- 验证 App 向量表有效性
- 分块写入内部 Flash (2KB/次)
- Readback 验证确保写入正确

**关键流程:**
```
读取 OTA Header
    │
    ▼
验证 Magic (0x4F544131)
    │
    ▼
验证 Header CRC32
    │
    ▼
验证 App 向量表
    │
    ▼
擦除 App Flash (Sector 2-7)
    │
    ▼
分块写入 (2KB/次)
    │
    ├─→ 计算流式 CRC32
    │
    ▼
验证流式 CRC32
    │
    ▼
Readback 验证
    │
    ▼
安装完成
```

**关键函数:**
```c
fboot_status_t fastboot_ota_install(fastboot_ota_read_fn read_fn, void *ctx);
```

#### fastboot_staging.c

Staging 区管理模块。

**功能:**
- 检查 W25Q64 OTA 暂存区
- 读取并验证 magic
- 调用 OTA 模块安装
- 安装成功后清除暂存区

**关键流程:**
```
初始化 W25Q64
    │
    ▼
读取暂存区头部 (4B)
    │
    ▼
验证 Magic (0x4F544131)
    │
    ▼
调用 fastboot_ota_install()
    │
    ▼
安装成功?
    │
    ├─ 是 → 擦除暂存区
    │
    └─ 否 → 返回错误
```

**关键函数:**
```c
fboot_status_t fastboot_staging_install_if_pending(void);
```

#### fastboot_ymodem.c

YMODEM-1K 协议接收模块。

**功能:**
- 实现 YMODEM-1K 协议
- 支持 CRC-16 校验
- 异步写入 (接收与 Flash 编程并行)
- 错误重传机制

**协议状态机:**
```
┌─────────┐
│  IDLE   │
└────┬────┘
     │
     ▼
┌─────────┐     接收文件名包
│ WAIT    │◄─────────────────────┐
│ HEADER  │                      │
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     CRC 验证通过     │
│ RECEIVE │──────────────────────┤
│ DATA    │                      │
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     接收 EOT         │
│ WAIT    │──────────────────────┤
│ EOT     │                      │
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     接收结束包       │
│ WAIT    │──────────────────────┘
│ END     │
└────┬────┘
     │
     ▼
┌─────────┐
│  DONE   │
└─────────┘
```

**关键函数:**
```c
fboot_status_t fastboot_ymodem_receive(const fboot_io_t *io,
                                       const fboot_sink_t *sink,
                                       uint32_t *out_size);
```

### 2. 队列模块 (Queue)

#### fastboot_queue.c

异步写入队列模块。

**功能:**
- 环形缓冲区管理
- 异步状态机
- 接收与写入并行

**数据结构:**
```c
typedef struct {
    fboot_queue_slot_t slots[FBOOT_QUEUE_DEPTH];  // 8 个槽位
    uint32_t head;                                 // 读指针
    uint32_t tail;                                 // 写指针
    uint32_t count;                                // 当前数量
} fboot_queue_t;

typedef struct {
    uint8_t  packet[FBOOT_QUEUE_BUF_SIZE];  // 包数据 (1KB + 头部)
    uint32_t offset;                         // Flash 偏移
    uint32_t len;                            // 数据长度
    bool     writing;                        // 正在写入标志
} fboot_queue_slot_t;
```

**异步状态机:**
```
┌─────────┐
│  IDLE   │
└────┬────┘
     │
     ▼
┌─────────┐     alloc_slot()
│ ALLOC   │──────────────────────┐
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     commit()         │
│ COMMIT  │──────────────────────┤
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     drain_one()      │
│ DRAIN   │──────────────────────┤
└────┬────┘                      │
     │                           │
     ▼                           │
┌─────────┐     写入完成         │
│  DONE   │──────────────────────┘
└─────────┘
```

**关键函数:**
```c
void fastboot_queue_reset(fboot_queue_t *q);
bool fastboot_queue_full(const fboot_queue_t *q);
bool fastboot_queue_empty(const fboot_queue_t *q);
fboot_queue_slot_t *fastboot_queue_alloc(fboot_queue_t *q);
void fastboot_queue_commit(fboot_queue_t *q);
fboot_status_t fastboot_queue_drain_one(fboot_queue_t *q, const fboot_sink_t *sink, uint32_t *written);
fboot_status_t fastboot_queue_drain_all(fboot_queue_t *q, const fboot_sink_t *sink, uint32_t *written);
```

### 3. 接口模块 (Interface)

#### fastboot_io.h

I/O 接口定义。

```c
typedef struct {
    size_t (*read)(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms);
    void (*write_byte)(void *ctx, uint8_t byte);
    void *ctx;
} fboot_io_t;
```

**用途:**
- YMODEM 协议的字节级 I/O
- 支持超时机制
- 支持上下文传递

#### fastboot_sink.h

数据写入接口定义。

```c
typedef struct {
    fboot_status_t (*begin)(void *ctx, uint32_t size);
    fboot_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*write_start)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    fboot_status_t (*poll)(void *ctx);
    bool (*busy)(void *ctx);
    void *ctx;
} fboot_sink_t;
```

**用途:**
- 支持同步和异步写入
- 支持状态查询
- 支持上下文传递

#### fastboot_port.h

硬件抽象接口定义。

```c
/* Tick / Delay */
uint32_t fastboot_port_tick_ms(void);
void fastboot_port_delay_ms(uint32_t ms);

/* Watchdog */
void fastboot_port_feed_watchdog(void);

/* System */
void fastboot_port_reset(void) __attribute__((noreturn));

/* GPIO */
bool fastboot_port_button_pressed(void);
void fastboot_port_led_set(bool on);
void fastboot_port_led_toggle(void);
```

**用途:**
- 隔离硬件依赖
- 支持多平台移植
- 提供统一的硬件访问接口

### 4. 端口模块 (Port)

#### fastboot_port.c

平台抽象实现。

**STM32F4 实现:**
```c
uint32_t fastboot_port_tick_ms(void) {
    return HAL_GetTick();
}

void fastboot_port_delay_ms(uint32_t ms) {
    HAL_Delay(ms);
}

void fastboot_port_reset(void) {
    NVIC_SystemReset();
}

bool fastboot_port_button_pressed(void) {
    return HAL_GPIO_ReadPin(PB1_USER_KEY1_GPIO_Port,
                            PB1_USER_KEY1_Pin) == GPIO_PIN_RESET;
}

void fastboot_port_led_toggle(void) {
    HAL_GPIO_TogglePin(PC13_LED_EN_GPIO_Port, PC13_LED_EN_Pin);
}
```

#### fastboot_uart.c

UART 驱动模块。

**功能:**
- USART1 初始化 (460800 baud)
- DMA 接收 (环形缓冲区)
- 字节级读写
- 调试输出

**DMA 接收机制:**
```
┌─────────────────────────────────────────────────────────────┐
│  DMA 环形缓冲区                                              │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Buffer (4KB)                                         │  │
│  │  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐  │  │
│  │  │     │     │     │     │     │     │     │     │  │  │
│  │  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘  │  │
│  │    ▲                           ▲                     │  │
│  │    │                           │                     │  │
│  │  tail (软件读指针)           head (DMA写指针)        │  │
│  │    │                           │                     │  │
│  │    └───────── NDTR ────────────┘                     │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  head = BUF_SIZE - NDTR                                     │
│  tail = 软件维护                                            │
│  可读数据 = (head - tail + BUF_SIZE) % BUF_SIZE            │
└─────────────────────────────────────────────────────────────┘
```

**关键函数:**
```c
void fastboot_uart_init(void);
void fastboot_uart_deinit(void);
const fboot_io_t *fastboot_uart_io(void);
void fastboot_uart_puts(const char *s);
void fastboot_uart_hex32(const char *label, uint32_t val);
void fastboot_uart_dec32(const char *label, uint32_t val);
void fastboot_uart_status(const char *label, fboot_status_t rc);
```

#### fastboot_iflash.c

内部 Flash 驱动模块。

**功能:**
- Sector 擦除
- 2KB 编程
- CRC32 校验
- Readback 验证

**Flash 操作流程:**
```
解锁 Flash
    │
    ▼
擦除 Sector
    │
    ▼
编程 (2KB/次)
    │
    ▼
验证 (Readback + CRC)
    │
    ▼
锁定 Flash
```

**关键函数:**
```c
fboot_status_t fastboot_iflash_erase_app(void);
fboot_status_t fastboot_iflash_write(uint32_t addr, const uint8_t *data, size_t len);
fboot_status_t fastboot_iflash_verify(uint32_t addr, const uint8_t *data, size_t len);
```

#### fastboot_w25q64.c

W25Q64 SPI Flash 驱动模块。

**功能:**
- JEDEC ID 读取
- 4KB Sector 擦除
- 256B Page 编程
- 数据读取
- 异步写入支持

**SPI 通信协议:**
```
┌─────────────────────────────────────────────────────────────┐
│  命令格式                                                    │
│                                                             │
│  读取 JEDEC ID:                                             │
│    发送: 0x9F                                               │
│    接收: [制造商ID] [设备ID高] [设备ID低]                   │
│                                                             │
│  读取数据:                                                  │
│    发送: 0x03 [地址23-16] [地址15-8] [地址7-0]             │
│    接收: [数据...]                                          │
│                                                             │
│  擦除 Sector (4KB):                                         │
│    发送: 0x20 [地址23-16] [地址15-8] [地址7-0]             │
│    等待: 忙标志清除                                         │
│                                                             │
│  编程 Page (256B):                                          │
│    发送: 0x02 [地址23-16] [地址15-8] [地址7-0] [数据...]   │
│    等待: 忙标志清除                                         │
└─────────────────────────────────────────────────────────────┘
```

**关键函数:**
```c
fboot_status_t fastboot_w25q64_init(void);
uint32_t fastboot_w25q64_jedec_id(void);
fboot_status_t fastboot_w25q64_read(uint32_t offset, uint8_t *data, size_t len);
fboot_status_t fastboot_w25q64_write(uint32_t offset, const uint8_t *data, size_t len);
fboot_status_t fastboot_w25q64_erase_sector(uint32_t offset);
const fboot_sink_t *fastboot_w25q64_ota_sink(void);
```

## 移植指南

### 1. 创建 fastboot_config.h

```c
#ifndef FASTBOOT_CONFIG_H
#define FASTBOOT_CONFIG_H

/* 内部 Flash */
#define FASTBOOT_FLASH_BASE          0x08000000u
#define FASTBOOT_FLASH_SIZE          0x00008000u  /* 32 KiB */

#define FASTBOOT_APP_FLASH_BASE      0x08008000u
#define FASTBOOT_APP_FLASH_SIZE      0x00078000u  /* 480 KiB */
#define FASTBOOT_APP_FLASH_END       (FASTBOOT_APP_FLASH_BASE + FASTBOOT_APP_FLASH_SIZE)

/* SRAM */
#define FASTBOOT_SRAM_BASE           0x20000000u
#define FASTBOOT_SRAM_SIZE           0x00020000u  /* 128 KiB */
#define FASTBOOT_SRAM_END            (FASTBOOT_SRAM_BASE + FASTBOOT_SRAM_SIZE)

/* 外部 Flash */
#define FASTBOOT_EXTFLASH_OTA_OFFSET 0x000000u
#define FASTBOOT_EXTFLASH_OTA_SIZE   0x400000u   /* 4 MiB */

#endif
```

### 2. 实现 fastboot_port_* 函数

```c
#include "fastboot_port.h"
#include "your_platform_hal.h"

uint32_t fastboot_port_tick_ms(void) {
    return HAL_GetTick();
}

void fastboot_port_delay_ms(uint32_t ms) {
    HAL_Delay(ms);
}

void fastboot_port_feed_watchdog(void) {
    /* 喂狗 */
}

void fastboot_port_reset(void) {
    NVIC_SystemReset();
}

bool fastboot_port_button_pressed(void) {
    return HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_RESET;
}

void fastboot_port_led_set(bool on) {
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void fastboot_port_led_toggle(void) {
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}
```

### 3. 实现硬件驱动

- `fastboot_uart.c/h` — UART 收发
- `fastboot_iflash.c/h` — 内部 Flash 读写擦除
- `fastboot_w25q64.c/h` — 外部 SPI Flash 驱动

### 4. 集成到项目

```cmake
# CMakeLists.txt
set(FASTBOOT_PORT "stm32f4" CACHE STRING "" FORCE)
set(FASTBOOT_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/00_Config" CACHE PATH "" FORCE)
set(FASTBOOT_PLATFORM_INCLUDES ... CACHE PATH "" FORCE)
add_subdirectory(fastboot)

target_link_libraries(your_target PRIVATE fastboot)
```

## API 参考

### 核心 API

#### fastboot_staging_install_if_pending()

```c
fboot_status_t fastboot_staging_install_if_pending(void);
```

检查 W25Q64 OTA 暂存区，如果有新固件则安装到内部 Flash。

**返回值:**
- `FB_OK`: 安装成功
- `FB_NO_UPDATE`: 无更新
- 其他: 错误码

#### fastboot_ymodem_receive()

```c
fboot_status_t fastboot_ymodem_receive(const fboot_io_t *io,
                                       const fboot_sink_t *sink,
                                       uint32_t *out_size);
```

通过 YMODEM 协议接收固件。

**参数:**
- `io`: I/O 接口 (UART)
- `sink`: 写入目标 (W25Q64)
- `out_size`: 接收字节数 (输出)

**返回值:**
- `FB_OK`: 接收成功
- 其他: 错误码

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

## 内部实现

### CRC32 算法

```c
uint32_t fastboot_crc32(const uint8_t *data, size_t len, uint32_t seed) {
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0 - (crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
```

### CRC-16 XMODEM 算法

```c
uint16_t crc16_xmodem(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    while (len-- > 0) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint32_t i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

### 向量表验证

```c
bool app_vector_is_valid(uint32_t app_base) {
    uint32_t initial_sp = *(const uint32_t *)app_base;
    uint32_t reset_handler = *(const uint32_t *)(app_base + 4);

    /* SP 在 SRAM 范围内 */
    if (initial_sp < FASTBOOT_SRAM_BASE || initial_sp > FASTBOOT_SRAM_END)
        return false;

    /* Reset_Handler 在 App Flash 范围内 */
    if (reset_handler < FASTBOOT_APP_FLASH_BASE || reset_handler >= FASTBOOT_APP_FLASH_END)
        return false;

    /* Thumb 指令位 */
    if ((reset_handler & 1) == 0)
        return false;

    return true;
}
```

## 许可证

MIT License
