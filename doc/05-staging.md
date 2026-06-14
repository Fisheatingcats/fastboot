# Staging 区管理

## 目录

1. [概述](#1-概述)
2. [Staging 区的角色](#2-staging-区的角色)
3. [检测流程](#3-检测流程)
4. [与 OTA 模块的协作](#4-与-ota-模块的协作)
5. [安装成功后的清除策略](#5-安装成功后的清除策略)
6. [编译时开关](#6-编译时开关)
7. [使用示例](#7-使用示例)
8. [流程总览](#8-流程总览)

---

## 1. 概述

Staging 区管理模块（`src/core/fastboot_staging.c`）提供开机时自动检测 staging 区是否存在待安装固件的功能。若检测到有效的 FWOT 包（通过 magic 标识），则调用 OTA 安装流程将固件安装到 primary 区。

**核心函数**：
```c
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

## 2. Staging 区的角色

### 2.1 存储介质

Staging 区通常映射到外部 SPI Flash（如 W25Q64），具有以下特点：

- **容量大**：足以容纳完整固件镜像（如 256KB ~ 1MB）
- **掉电保持**：固件包在传输完成后持久存储
- **独立于主 Flash**：不影响 primary 区的运行固件

### 2.2 生命周期

```
┌─────────────┐    YMODEM 接收    ┌─────────────┐    OTA 安装    ┌─────────────┐
│   空闲状态   │ ───────────────→ │  待安装状态  │ ────────────→ │  安装完成    │
│  (无 magic)  │                  │ (有 magic)  │               │ (清除 magic) │
└─────────────┘                  └─────────────┘               └─────────────┘
       ↑                                                              │
       └──────────────────────────────────────────────────────────────┘
                            下次开机回到空闲状态
```

### 2.3 与 Primary 区的分工

| 区域 | 介质 | 用途 | 操作 |
|------|------|------|------|
| Staging | 外部 SPI Flash | 暂存 OTA 包 | 接收 → 检测 → 安装 → 清除 |
| Primary | 内部 Flash | 运行固件 | 擦除 → 写入 → 校验 → 运行 |

## 3. 检测流程

### 3.1 读取 Magic

```c
uint32_t magic = 0u;
rc = fastboot_flash_area_read(staging, 0u, (uint8_t *)&magic, sizeof(magic));
```

从 staging 区偏移 0 读取 4 字节，作为 FWOT 包的魔数标识。

### 3.2 Magic 比对

```c
#define FASTBOOT_OTA_MAGIC  0x544F5746u  // "FWOT" 小端序

if (magic != FASTBOOT_OTA_MAGIC) {
    return FB_NO_UPDATE;    // 无待安装固件
}
```

**注意**：仅检查 magic 不足以确认固件有效性，完整的校验由 `fastboot_ota_install()` 负责。

### 3.3 参数检查

```c
if (!staging || !primary || !policy) {
    return FB_ERR_PARAM;
}
```

在读取 magic 之前先检查指针有效性。

## 4. 与 OTA 模块的协作

### 4.1 调用链

```c
fboot_status_t fastboot_staging_install_if_pending(...)
{
    // 1. 检测 magic
    if (magic != FASTBOOT_OTA_MAGIC) {
        return FB_NO_UPDATE;
    }

    // 2. 调用 OTA 安装
    rc = fastboot_ota_install(staging, primary, policy, runtime, log);

    // 3. 安装成功后清除 staging
    if (rc == FB_OK) {
        fastboot_flash_area_erase(staging, 0u, 1u);
    }
    return rc;
}
```

### 4.2 职责划分

| 模块 | 职责 |
|------|------|
| staging | 检测 magic、调用 OTA、清除 staging |
| ota | 读取 header、校验、擦除 primary、写入、验证 |

Staging 模块是 OTA 模块的"门卫"：只有检测到有效 magic 才会触发安装。

## 5. 安装成功后的清除策略

### 5.1 清除方式

```c
(void)fastboot_flash_area_erase(staging, 0u, 1u);
```

擦除 staging 区的**第 1 个扇区**（`len=1` 表示 1 个扇区），破坏 magic 字段，防止下次开机重复安装。

### 5.2 为什么只擦除第一个扇区

- **最小化操作**：扇区擦除是耗时操作，只擦除第一个扇区最快
- **破坏 magic 即可**：只要 magic 不匹配，staging 模块就不会触发安装
- **保留数据**：其余数据仍在，便于调试或故障恢复

### 5.3 清除时机

```
安装成功 (FB_OK)
    │
    v
erase(staging, 0, 1)    ← 破坏 magic
    │
    v
return FB_OK             ← 下次开机不会重复安装
```

**注意**：安装失败时不清除 staging 区，下次开机会重试安装。

### 5.4 错误忽略

```c
(void)fastboot_flash_area_erase(staging, 0u, 1u);
```

清除操作的返回值被显式忽略（`(void)` 转换）。即使清除失败，安装已经成功完成，不应影响启动流程。

## 6. 编译时开关

### 6.1 FASTBOOT_CFG_ENABLE_STAGING

```c
#if FASTBOOT_CFG_ENABLE_STAGING
    // 完整实现
#else
    (void)staging;
    (void)primary;
    (void)policy;
    (void)runtime;
    (void)log;
    return FB_NO_UPDATE;    // 禁用时始终返回"无更新"
#endif
```

### 6.2 禁用场景

- **纯 YMODEM 模式**：只通过 UART 接收固件，不需要 staging 区
- **资源受限**：无外部 Flash，staging 区不可用
- **开发调试**：跳过 staging 检测，加快启动速度

### 6.3 与其他开关的配合

| 配置组合 | 行为 |
|----------|------|
| STAGING=1, FWOT=1 | 完整 OTA 流程 |
| STAGING=1, FWOT=0 | staging 检测会触发，但 ota_install 返回 FB_ERR_FORMAT |
| STAGING=0 | 始终返回 FB_NO_UPDATE |

## 7. 使用示例

### 7.1 典型启动流程

```c
int main(void)
{
    // 1. 硬件初始化
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    // 2. 定义 flash 区域
    fastboot_flash_area_t staging = {
        .offset = 0,
        .size = STAGING_CAPACITY,
        .ops = &w25q64_ops,
        .ctx = &w25q64_handle,
    };
    fastboot_flash_area_t primary = {
        .offset = APP_FLASH_ADDR,
        .size = APP_FLASH_SIZE,
        .ops = &stm32_flash_ops,
        .ctx = NULL,
    };

    // 3. 检查 staging 区
    fboot_status_t rc = fastboot_staging_install_if_pending(
        &staging, &primary, &app_policy, &board_runtime, &board_log);

    if (rc == FB_OK) {
        log("OTA install success");
    } else if (rc == FB_NO_UPDATE) {
        log("No pending update");
    } else {
        log("OTA install failed");
    }

    // 4. 跳转到应用
    jump_to_app(APP_FLASH_ADDR);
}
```

### 7.2 与 YMODEM 的配合

```c
// 场景：先通过 YMODEM 接收固件到 staging，再安装
fastboot_ymodem_receive(&transport, &staging_writer, &runtime, &size);
fastboot_staging_install_if_pending(&staging, &primary, &policy, &runtime, &log);
```

## 8. 流程总览

```
fastboot_staging_install_if_pending(staging, primary, policy, runtime, log)
    │
    v
[1] 参数检查 (!staging || !primary || !policy)
    │
    ├── 无效 → return FB_ERR_PARAM
    │
    v
[2] read(staging, 0, &magic, 4)
    │
    ├── 读取失败 → return FB_ERR_FLASH
    │
    v
[3] magic == FASTBOOT_OTA_MAGIC ?
    │
    ├── 不匹配 → return FB_NO_UPDATE
    │
    v
[4] fastboot_ota_install(staging, primary, policy, runtime, log)
    │
    ├── 失败 → return 错误码 (staging 保留，下次重试)
    │
    v
[5] erase(staging, 0, 1)    ← 清除 magic
    │
    v
return FB_OK
```

### 状态转移图

```
                    开机
                     │
                     v
              ┌──────────────┐
              │ 读取 magic   │
              └──────┬───────┘
                     │
           ┌─────────┴─────────┐
           │                   │
           v                   v
    magic 不匹配          magic 匹配
           │                   │
           v                   v
    ┌─────────────┐    ┌─────────────┐
    │ FB_NO_UPDATE│    │ ota_install │
    │ (直接启动)  │    └──────┬──────┘
    └─────────────┘           │
                    ┌─────────┴─────────┐
                    │                   │
                    v                   v
               安装成功             安装失败
                    │                   │
                    v                   v
            ┌──────────────┐    ┌──────────────┐
            │ 清除 magic   │    │ 保留 staging │
            │ return FB_OK │    │ return 错误  │
            └──────────────┘    └──────────────┘
```
