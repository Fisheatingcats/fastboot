# STM32F411CEU6 参考 demo 基线

本文记录 FreeWatchBoot demo 在 STM32F411CEU6 + W25Q64 上完成的 OTA 实测配置，供移植者复现实验和评估默认参数。数据来自该参考板，不代表所有 STM32F4 板卡都能达到相同吞吐率。

## 配置

| 参数 | 参考值 |
| --- | --- |
| UART | USART1，921600 baud，8N1，DMA circular RX |
| UART 接收环形缓冲 | 8192 bytes |
| YMODEM | YMODEM-1K，包载荷 1024 bytes |
| 写入队列 | 32 个槽位；每槽保存包头、载荷和 CRC |
| 外部 Flash | W25Q64，SPI2 `/2`，按需擦除与异步写入 |
| 内部 Flash 安装 | App 起址 `0x08008000`，FWOT 流式 CRC 和安装后 readback 校验 |
| 优化构建 | Release `-Os -flto` |

队列和 DMA 环形缓冲属于 RAM 配置。参考配置的队列占用约 32 KiB，UART 环形缓冲另占 8 KiB；移植时还要为栈、全局变量和平台状态保留足够 SRAM。若 RAM 紧张，应测量不同 `FASTBOOT_CFG_QUEUE_DEPTH` 和 DMA 缓冲大小下的传输重试与吞吐率。

## OTA 实测

| 指标 | 结果 |
| --- | ---: |
| FWOT 包大小 | 277,976 bytes |
| UART payload 吞吐率 | 80.4 KiB/s |
| 921600 baud 8N1 理论 payload 上限 | 90.0 KiB/s |
| 链路效率 | 89.3% |
| 含安装与跳转的端到端速率 | 67.3 KiB/s |
| YMODEM 重传 | 0 |
| W25Q64 擦除 / 编程 | 8 次 / 1,086 次 |
| 安装验证 | FWOT stream CRC 和内部 Flash readback verify 均通过 |
| Bootloader 体积 | 23,080 bytes / 32 KiB（70.4%） |

这些数值用于对照 demo 的已测结果。不同 HAL、串口桥接器、晶振、Flash 型号和擦写等待策略都会影响实际结果；上线前应在目标硬件上重新测量并保存传输日志。

## 复测入口

FreeWatchBoot demo 的一键检查会构建 Release 固件、执行 YMODEM 主机自测并检查 32 KiB Bootloader 体积。硬件测试还应记录重传次数、payload 速率、总时间、外部 Flash 擦除/编程计数以及安装校验结果。
