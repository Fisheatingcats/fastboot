# OTA 安装流程

## 目录

1. [概述](#1-概述)
2. [FWOT 包格式](#2-fwot-包格式)
3. [OTA Header 结构](#3-ota-header-结构)
4. [安装流程详解](#4-安装流程详解)
5. [CRC-32 算法](#5-crc-32-算法)
6. [flash_area_t 地址映射](#6-flash_area_t-地址映射)
7. [image_policy_t 策略模式](#7-image_policy_t-策略模式)
8. [错误处理](#8-错误处理)
9. [流程总览](#9-流程总览)

---

## 1. 概述

OTA 安装模块（`src/core/fastboot_ota.c`）负责从 staging 区读取 FWOT 包并安装到 primary flash。安装流程包含多重校验：header 校验、向量表验证、流式 CRC-32、可选 readback 验证。

**核心函数**：
```c
fboot_status_t fastboot_ota_install(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);
```

## 2. FWOT 包格式

FWOT（FreeWatch OTA）是 FastBoot 定义的固件包格式：

```
+---------------------------------------------------+
|                 OTA Header (64 字节)                |
+---------------------------------------------------+
|                                                   |
|    Image Data (固件镜像数据)                        |
|                                                   |
+---------------------------------------------------+

偏移 0                    偏移 image_offset        偏移 image_offset + image_size
```

**特点**：
- 头部固定 64 字节，含 CRC-32 自校验
- 镜像数据紧跟头部（或在指定偏移处）
- 支持版本号、加载地址、镜像 CRC-32 等元数据

## 3. OTA Header 结构

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 魔数：0x544F5746 ("FWOT" 小端)
    uint16_t header_size;     // 头部大小：64 字节
    uint16_t header_version;  // 头部版本：1
    uint32_t flags;           // 标志位（保留）
    uint32_t image_offset;    // 镜像数据偏移（相对包头起始）
    uint32_t image_size;      // 镜像数据大小（字节）
    uint32_t image_crc32;     // 镜像数据 CRC-32
    uint32_t load_addr;       // 镜像加载地址
    uint32_t image_version;   // 镜像版本号
    uint32_t reserved[7];     // 保留字段（28 字节）
    uint32_t header_crc32;    // 头部 CRC-32（不含此字段）
} fastboot_ota_header_t;
#pragma pack(pop)
```

### 3.1 字段说明

| 字段 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| magic | 0 | 4 | 必须为 0x544F5746（"FWOT"） |
| header_size | 4 | 2 | 必须为 sizeof(header) = 64 |
| header_version | 6 | 2 | 必须为 1 |
| flags | 8 | 4 | 保留，当前未使用 |
| image_offset | 12 | 4 | 镜像数据相对偏移，≥ 64 |
| image_size | 16 | 4 | 镜像数据大小，≥ 8 |
| image_crc32 | 20 | 4 | 镜像数据的 CRC-32 校验值 |
| load_addr | 24 | 4 | 镜像加载地址（如 0x08010000） |
| image_version | 28 | 4 | 镜像版本号 |
| reserved | 32 | 28 | 保留，填零 |
| header_crc32 | 60 | 4 | 头部（此字段置零后）的 CRC-32 |

### 3.2 头部 CRC-32 计算

```c
static uint32_t header_crc32(const fastboot_ota_header_t *header)
{
    fastboot_ota_header_t tmp = *header;
    tmp.header_crc32 = 0u;    // 将 CRC 字段置零
    return fastboot_crc32((const uint8_t *)&tmp, sizeof(tmp), 0u);
}
```

**原理**：将头部结构体复制到临时缓冲区，将 `header_crc32` 字段置零后计算整个头部的 CRC-32。

## 4. 安装流程详解

### 步骤 1：读取 Header

```c
rc = fastboot_flash_area_read(staging, 0u, (uint8_t *)&header, sizeof(header));
```

从 staging 区偏移 0 读取 64 字节头部到 RAM。

### 步骤 2：校验 Header

```c
static fboot_status_t validate_header(
    const fastboot_ota_header_t *header,
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy)
{
    // 1. 参数有效性
    if (!header || !staging || !primary || !policy || !policy->vector_is_valid)
        return FB_ERR_PARAM;

    // 2. magic、header_size、header_version
    if (header->magic != FASTBOOT_OTA_MAGIC ||
        header->header_size != sizeof(*header) ||
        header->header_version != FASTBOOT_OTA_HEADER_VER)
        return FB_ERR_FORMAT;

    // 3. image_offset 和 image_size 合理性
    if (header->image_offset < sizeof(*header) ||
        header->image_size < 8u ||
        header->image_size > primary->size ||
        header->image_size > policy->max_image_size)
        return FB_ERR_FORMAT;

    // 4. load_addr 匹配策略
    if (header->load_addr != policy->load_addr)
        return FB_ERR_FORMAT;

    // 5. image 在 staging 区范围内
    if (!range_fits(header->image_offset, header->image_size, staging->size))
        return FB_ERR_FORMAT;

    // 6. 头部 CRC-32 校验
    if (header_crc32(header) != header->header_crc32)
        return FB_ERR_CRC;

    return FB_OK;
}
```

**校验项**：
1. 指针有效性（非 NULL）
2. magic 为 0x544F5746
3. header_size 等于 64
4. header_version 等于 1
5. image_offset ≥ 64（不小于头部大小）
6. image_size ≥ 8 且不超过 primary 容量和策略限制
7. load_addr 与策略匹配
8. image 数据在 staging 区范围内（无溢出）
9. 头部 CRC-32 校验通过

### 步骤 3：向量表验证

```c
rc = fastboot_flash_area_read(staging, header.image_offset, s_chunk, 8u);
if (!policy->vector_is_valid(s_chunk, 8u, header.load_addr,
                             header.image_size, policy->ctx)) {
    return FB_ERR_FORMAT;
}
```

读取镜像前 8 字节（中断向量表的初始 SP 和 Reset 向量），调用策略回调验证：
- 栈指针是否在合法 RAM 范围内
- 复位向量是否在镜像地址范围内

### 步骤 4：擦除 Primary Flash

```c
rc = fastboot_flash_area_erase(primary, 0u, primary->size);
```

擦除 primary flash 全区，为写入做准备。

### 步骤 5：分块写入（流式 CRC-32）

```c
#define OTA_CHUNK_SIZE 1024u
static uint8_t s_chunk[OTA_CHUNK_SIZE];

remaining = header.image_size;
while (remaining > 0u) {
    size_t chunk = remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining;
    uint32_t src = header.image_offset + image_pos;

    // 从 staging 读取
    rc = fastboot_flash_area_read(staging, src, s_chunk, chunk);

    // 流式累积 CRC-32
    image_crc = fastboot_crc32(s_chunk, chunk, image_crc);

    // 写入 primary
    rc = fastboot_flash_area_write(primary, image_pos, s_chunk, chunk);

    image_pos += (uint32_t)chunk;
    remaining -= (uint32_t)chunk;
    fastboot_runtime_feed_watchdog(runtime);  // 喂看门狗
}
```

**分块大小**：1024 字节（`OTA_CHUNK_SIZE`），使用静态缓冲区避免栈溢出。

### 步骤 6：流式 CRC 校验

```c
if (image_crc != header.image_crc32) {
    return FB_ERR_CRC;
}
```

比对流式计算的 CRC-32 与头部记录的 `image_crc32`。

### 步骤 7：Readback 验证（可选）

```c
#if FASTBOOT_CFG_ENABLE_READBACK_VERIFY
{
    uint32_t readback_crc = 0u;
    while (rb_remaining > 0u) {
        rc = fastboot_flash_area_read(primary, rb_pos, s_chunk, rb_chunk);
        readback_crc = fastboot_crc32(s_chunk, rb_chunk, readback_crc);
        // ...
    }
    if (readback_crc != header.image_crc32) {
        return FB_ERR_VERIFY;
    }
}
#endif
```

从 primary flash 重新读取全部数据并计算 CRC-32，验证写入正确性。

## 5. CRC-32 算法

### 5.1 算法参数

| 参数 | 值 |
|------|-----|
| 多项式 | 0xEDB88320（反转形式） |
| 初始值 | 0xFFFFFFFF |
| 最终异或 | 0xFFFFFFFF |
| 输入反转 | 是（逐位处理时） |

### 5.2 实现代码

```c
uint32_t fastboot_crc32(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t crc = seed ^ 0xFFFFFFFFu;

    for (size_t i = 0u; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
```

### 5.3 逐位处理原理

```
对于每个数据字节的每一位：
    1. crc 与数据字节异或
    2. 检查 crc 最低位：
       - 若为 1：crc 右移 1 位后与多项式异或
       - 若为 0：crc 仅右移 1 位
    3. 重复 8 次处理完一个字节

mask 技巧：
    mask = 0u - (crc & 1u)
    - crc 最低位为 1 → mask = 0xFFFFFFFF → 保留多项式
    - crc 最低位为 0 → mask = 0x00000000 → 屏蔽多项式
```

### 5.4 流式计算

CRC-32 支持流式计算：首次调用传 `seed=0`，后续调用传上次返回值：

```c
uint32_t crc = 0;
crc = fastboot_crc32(chunk1, len1, crc);  // 第一块
crc = fastboot_crc32(chunk2, len2, crc);  // 第二块，续算
// crc 等价于 fastboot_crc32(完整数据, 总长度, 0)
```

## 6. flash_area_t 地址映射

### 6.1 映射原理

```c
// 用户调用（相对偏移）
fastboot_flash_area_read(area, user_offset, buf, len);

// 内部转换（绝对地址）
area->ops->read(area->ctx, area->offset + user_offset, buf, len);
```

### 6.2 范围检查

```c
static inline fboot_status_t fastboot_flash_area_check(
    const fastboot_flash_area_t *area, uint32_t offset, size_t len)
{
    if (!area || !area->ops) return FB_ERR_PARAM;
    if (offset > area->size || len > (size_t)(area->size - offset))
        return FB_ERR_RANGE;
    if (area->offset + offset < area->offset)  // 溢出检查
        return FB_ERR_RANGE;
    return FB_OK;
}
```

### 6.3 统一 staging 和 primary

```
staging 区                          primary 区
offset = 0x00000000                 offset = 0x08010000
size   = 256KB                      size   = 480KB
ops    = w25q64_ops                 ops    = stm32_flash_ops

fastboot_flash_area_read(staging, 0, ...)     → w25q64_read(0x00000000, ...)
fastboot_flash_area_read(primary, 0, ...)     → stm32_flash_read(0x08010000, ...)
```

## 7. image_policy_t 策略模式

### 7.1 策略结构

```c
typedef struct {
    uint32_t load_addr;                   // 预期加载地址
    uint32_t max_image_size;              // 最大镜像大小
    fastboot_vector_validate_fn vector_is_valid;  // 向量表校验回调
    void *ctx;                            // 用户上下文
} fastboot_image_policy_t;
```

### 7.2 向量表校验回调

```c
typedef bool (*fastboot_vector_validate_fn)(
    const uint8_t *vector,     // 向量表数据（前 8 字节）
    size_t len,                // 数据长度
    uint32_t load_addr,        // 加载地址
    uint32_t image_size,       // 镜像总大小
    void *ctx);                // 用户上下文
```

**典型实现**：
```c
bool my_vector_check(const uint8_t *v, size_t len,
                     uint32_t load_addr, uint32_t image_size, void *ctx)
{
    uint32_t sp = *(uint32_t *)&v[0];   // 初始栈指针
    uint32_t rv = *(uint32_t *)&v[4];   // 复位向量
    // 栈指针在 SRAM 范围内
    if (sp < 0x20000000 || sp > 0x20020000) return false;
    // 复位向量在镜像范围内
    if (rv < load_addr || rv >= load_addr + image_size) return false;
    return true;
}
```

### 7.3 策略的解耦作用

OTA 模块不关心具体的向量表校验逻辑，通过回调实现策略解耦：
- 不同 MCU 有不同的 RAM 布局
- 不同应用有不同的地址约束
- 回调由板级代码提供，core 层无需修改

## 8. 错误处理

### 8.1 错误传播

```c
fboot_status_t fastboot_ota_install(...)
{
    // 每个步骤检查返回值
    rc = fastboot_flash_area_read(...);
    if (rc != FB_OK) return rc;

    rc = validate_header(...);
    if (rc != FB_OK) return rc;

    // ... 任一步骤失败立即返回
}
```

### 8.2 错误码映射

| 步骤 | 可能的错误码 |
|------|-------------|
| 读取 header | FB_ERR_FLASH |
| 校验 header | FB_ERR_PARAM, FB_ERR_FORMAT, FB_ERR_CRC |
| 向量验证 | FB_ERR_FORMAT |
| 擦除 primary | FB_ERR_FLASH |
| 写入 primary | FB_ERR_FLASH |
| 流式 CRC | FB_ERR_CRC |
| Readback 验证 | FB_ERR_VERIFY |

### 8.3 编译时裁剪

```c
#if FASTBOOT_CFG_ENABLE_FWOT
    // 完整实现
#else
    return FB_ERR_FORMAT;  // 禁用时直接返回错误
#endif
```

## 9. 流程总览

```
fastboot_ota_install(staging, primary, policy, runtime, log)
    │
    v
[1] 读取 header (64B)
    │
    v
[2] validate_header()
    ├─ magic == 0x544F5746 ?
    ├─ header_size == 64 ?
    ├─ header_version == 1 ?
    ├─ image_offset >= 64 ?
    ├─ image_size 合理 ?
    ├─ load_addr 匹配 ?
    ├─ image 在 staging 范围内 ?
    └─ header_crc32 校验 ?
    │
    ├── 任一失败 → return 错误码
    │
    v
[3] 读取 image 前 8B → vector_is_valid()
    │
    ├── 失败 → return FB_ERR_FORMAT
    │
    v
[4] erase(primary, 0, size)
    │
    ├── 失败 → return FB_ERR_FLASH
    │
    v
[5] 循环分块 (1KB)
    │   ├─ read(staging, src, chunk)
    │   ├─ fastboot_crc32(chunk) → 累积 image_crc
    │   ├─ write(primary, dst, chunk)
    │   └─ feed_watchdog()
    │
    v
[6] image_crc == header.image_crc32 ?
    │
    ├── 不等 → return FB_ERR_CRC
    │
    v
[7] [可选] readback 验证
    │   ├─ read(primary, ...) → 计算 readback_crc
    │   └─ readback_crc == image_crc32 ?
    │
    ├── 不等 → return FB_ERR_VERIFY
    │
    v
return FB_OK
```
