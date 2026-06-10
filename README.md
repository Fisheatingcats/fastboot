# FastBoot

轻量级 OTA Bootloader 库，适用于 STM32 等嵌入式平台。

## 特性

- **硬件解耦**：核心协议与硬件驱动分离，易于移植
- **YMODEM-1K**：可靠的固件传输协议
- **异步队列**：接收与编程并行，提高吞吐量
- **OTA 支持**：支持外部 Flash 暂存区安装
- **体积小**：核心库 < 10KB

## 架构

```
FastBoot/
├── include/          # 公共头文件
│   ├── fastboot.h    # 主 API（包含所有公共头）
│   ├── fastboot_port.h   # 硬件抽象接口
│   └── fastboot_config.h # 配置接口
├── src/
│   ├── core/         # 硬件无关的核心逻辑
│   │   ├── fastboot_ymodem.c   # YMODEM 协议
│   │   ├── fastboot_ota.c      # OTA 包解析
│   │   └── fastboot_staging.c  # Staging 安装逻辑
│   └── queue/        # 异步写入队列
│       └── fastboot_queue.c
└── port/             # 平台相关实现
    └── stm32f4/      # STM32F4 HAL 移植
```

## 移植指南

### 1. 创建 `fastboot_config.h`

```c
#ifndef FASTBOOT_CONFIG_H
#define FASTBOOT_CONFIG_H

#define FASTBOOT_FLASH_BASE          0x08000000u
#define FASTBOOT_FLASH_SIZE          0x00008000u

#define FASTBOOT_APP_FLASH_BASE      0x08008000u
#define FASTBOOT_APP_FLASH_SIZE      0x00078000u
#define FASTBOOT_APP_FLASH_END       (FASTBOOT_APP_FLASH_BASE + FASTBOOT_APP_FLASH_SIZE)

#define FASTBOOT_SRAM_BASE           0x20000000u
#define FASTBOOT_SRAM_SIZE           0x00020000u
#define FASTBOOT_SRAM_END            (FASTBOOT_SRAM_BASE + FASTBOOT_SRAM_SIZE)

#define FASTBOOT_EXTFLASH_OTA_OFFSET 0x000000u
#define FASTBOOT_EXTFLASH_OTA_SIZE   0x400000u

#endif
```

### 2. 实现 `fastboot_port_*` 函数

```c
#include "fastboot_port.h"
#include "your_platform_hal.h"

uint32_t fastboot_port_tick_ms(void)    { return HAL_GetTick(); }
void fastboot_port_delay_ms(uint32_t ms){ HAL_Delay(ms); }
void fastboot_port_feed_watchdog(void)  { /* kick IWDG */ }
void fastboot_port_reset(void)          { NVIC_SystemReset(); }
bool fastboot_port_button_pressed(void) { /* read GPIO */ }
void fastboot_port_led_set(bool on)     { /* write GPIO */ }
void fastboot_port_led_toggle(void)     { /* toggle GPIO */ }
```

### 3. 实现硬件驱动

- `fastboot_uart.c/h` — UART 收发
- `fastboot_iflash.c/h` — 内部 Flash 读写擦除
- `fastboot_w25q64.c/h` — 外部 SPI Flash 驱动

### 4. 集成到项目

```cmake
add_subdirectory(FastBoot)
target_link_libraries(your_bootloader PRIVATE fastboot)
```

## 使用示例

```c
#include "fastboot.h"
#include "fastboot_uart.h"
#include "fastboot_w25q64.h"

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    fastboot_uart_init();

    // 检查 OTA 暂存区
    fboot_status_t rc = fastboot_staging_install_if_pending();
    if (rc == FB_OK) {
        fastboot_port_reset();
    }

    // 检查 App 有效性
    if (fastboot_app_vector_is_valid(FASTBOOT_APP_FLASH_BASE)) {
        jump_to_app(FASTBOOT_APP_FLASH_BASE);
    }

    // 进入 YMODEM 接收
    uint32_t size = 0;
    fastboot_ymodem_receive(fastboot_uart_io(),
                            fastboot_w25q64_ota_sink(), &size);
    // ...
}
```

## 许可证

MIT License
