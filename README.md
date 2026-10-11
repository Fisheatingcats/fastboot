<div align="center">
  <img src="doc/assets/fastboot-logo.png" alt="FastBoot 芯片与快速启动标志" width="360">
  <h1>FastBoot</h1>
  <p>面向嵌入式设备的可移植 OTA Bootloader 核心库</p>
</div>

FastBoot 将 YMODEM 固件接收、FWOT 包校验与安装、暂存区管理和异步写入队列组织为可复用 C 库。核心代码通过接口表访问传输、Flash、运行时和日志服务；STM32F4 HAL 代码集中在参考端口中。

## 功能

- YMODEM-1K 接收，包含 CRC-16、序号检查、ACK/NAK 和错误恢复。
- FWOT 包解析、镜像策略校验、流式 CRC32 和安装后回读校验。
- Staging 到主 Flash 的安装流程；无更新时返回 `FB_NO_UPDATE`。
- 固定内存的环形队列，让串口接收和 Flash 写入可以协作运行，不依赖堆分配或 RTOS。
- `FASTBOOT_CFG_*` 编译配置，可按目标裁剪功能。
- STM32F4 参考端口：UART DMA、片上 Flash、W25Q64 和运行时适配。
- Host 构建与协议/队列/OTA 单元测试，不依赖 MCU 硬件。

STM32F411CEU6 demo 的 921600 baud 实测基线为 80.4 KiB/s，零重传。该配置使用 32 个 YMODEM 队列槽和 8 KiB UART DMA 环形缓冲；其他板卡应结合 SRAM 和 Flash 特性重新选取。测试条件与数据见 [STM32F4 demo 基线](doc/07-stm32f4-reference.md)。

## 架构与目录

```text
应用 / Bootloader 装配层
        │ 调用公开 API，提供平台对象
        ▼
include/       稳定的公共接口和配置校验
src/core/      YMODEM、FWOT 安装和 staging 状态流程
src/queue/     固定容量的异步写入队列
port/          芯片、UART 与 Flash 的具体实现
tests/         Host 端单元测试
doc/           架构与模块设计说明
```

依赖方向为 `应用 → FastBoot 公共 API → core/queue → 平台回调`。`src/core/` 不应直接依赖 HAL、GPIO、USART、SPI 或具体 Flash 芯片；新硬件实现放在 `port/<平台>/`。当前 STM32F4 端口是参考实现，其他 MCU 可复用核心并提供自己的回调。

## 快速开始：Host 构建与测试

需要 CMake 3.22+、Ninja 和 C 编译器。Host 模式不编译 STM32F4 端口：

```powershell
cmake -S . -B build-host -G Ninja -DFASTBOOT_PORT=none -DFASTBOOT_BUILD_TESTS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

STM32F4 集成需要目标工程提供 HAL/CMSIS 头文件、`fastboot_board_config.h` 和板级驱动依赖。配置入口及接口契约见 [移植与抽象指南](doc/06-abstraction.md)。

## 集成入口

### 接收 YMODEM 固件

应用需要提供字节传输、写入器和运行时对象：

```c
fboot_status_t fastboot_ymodem_receive(
    const fastboot_transport_t *transport,
    const fastboot_writer_t *writer,
    const fastboot_runtime_t *runtime,
    uint32_t *out_size);
```

写入器可以同步写入，也可以使用 `write_start`、`poll` 和 `busy` 回调实现异步写入。

### 安装 FWOT 包

```c
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

调用方负责提供 staging/primary Flash 区域和镜像校验策略。`fastboot_ota_install()` 校验包头与镜像、验证向量表、安装镜像，并按配置回读验证结果。完整参数说明见 [OTA 安装流程](doc/04-ota.md) 与 [Staging 流程](doc/05-staging.md)。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [架构概览](doc/01-architecture.md) | 模块边界、依赖关系和调用流程 |
| [YMODEM 接收](doc/02-ymodem.md) | 协议状态机、传输接口和错误处理 |
| [异步队列](doc/03-queue.md) | 固定容量队列与协作式写入 |
| [OTA 安装](doc/04-ota.md) | FWOT 格式、镜像校验和安装过程 |
| [Staging 管理](doc/05-staging.md) | 暂存区检查和待安装状态 |
| [移植与抽象](doc/06-abstraction.md) | 配置、接口实现和新平台接入 |
| [STM32F4 demo 基线](doc/07-stm32f4-reference.md) | 经实测的队列、串口配置和 OTA 指标 |

## 配置

板级 `fastboot_board_config.h` 定义功能开关和尺寸参数，例如 YMODEM/FWOT/Staging 是否启用、队列深度、包尺寸及 staging 容量。STM32F4 参考配置位于 `port/stm32f4/fastboot_board_config.h`；产品集成时应使用目标板自己的配置，不要将参考板内存布局直接套用到其他芯片。

公共配置检查在 `include/fastboot_config.h`。配置宏名和有效范围以该头文件及 [移植指南](doc/06-abstraction.md) 为准。
