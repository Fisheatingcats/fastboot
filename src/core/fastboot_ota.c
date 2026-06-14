/**
 * @file fastboot_ota.c
 * @brief OTA 固件安装实现
 *
 * 本模块实现从 staging 区读取 FWOT 包并安装到 primary flash 的完整流程：
 * 读取头部 → 校验 magic/version/size/CRC → 向量验证 → 擦除目标区 →
 * 分块写入（流式 CRC-32）→ 可选 readback CRC 验证。
 */

#include "fastboot_ota.h"
#include "fastboot_config.h"
#include <string.h>

/** @brief 单次读写的分块大小（字节） */
#define OTA_CHUNK_SIZE 1024u
/** @brief 进度日志输出间隔（字节） */
#define OTA_PROGRESS_STEP 0x10000u

/** @brief 临时分块缓冲区，避免栈上分配大数组 */
static uint8_t s_chunk[OTA_CHUNK_SIZE];

/**
 * @brief 计算 CRC-32（逐字节，多项式 0xEDB88320）
 *
 * 使用标准 CRC-32 算法，初始值和最终异或均为 0xFFFFFFFF。
 * 逐字节处理，内部按位移位查表。
 *
 * @param data   待计算数据指针
 * @param len    数据长度（字节）
 * @param seed   累积种子值（首次调用传 0，续算传上次返回值）
 * @return       CRC-32 结果
 */
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

/**
 * @brief 计算 OTA 头部的 CRC-32
 *
 * 将头部结构体复制到临时缓冲区，将 header_crc32 字段置零后计算 CRC。
 * 用于校验头部完整性。
 *
 * @param header  OTA 头部指针
 * @return        头部 CRC-32 值
 */
static uint32_t header_crc32(const fastboot_ota_header_t *header)
{
    fastboot_ota_header_t tmp = *header;

    tmp.header_crc32 = 0u;
    return fastboot_crc32((const uint8_t *)&tmp, sizeof(tmp), 0u);
}

/**
 * @brief 检查 [offset, offset+size) 是否在 [0, capacity) 范围内
 *
 * 使用无溢出的安全比较：offset <= capacity && size <= (capacity - offset)。
 *
 * @param offset    起始偏移
 * @param size      数据大小
 * @param capacity  总容量
 * @return          true 表示范围合法，false 表示越界
 */
static bool range_fits(uint32_t offset, uint32_t size, uint32_t capacity)
{
    return offset <= capacity && size <= (capacity - offset);
}

/**
 * @brief 校验 OTA 头部合法性
 *
 * 依次检查：
 * 1. 参数有效性（非空指针、policy 向量回调有效）
 * 2. magic、header_size、header_version 一致性
 * 3. image_offset 不小于头部大小、image_size 合理
 * 4. load_addr 与策略匹配
 * 5. image 在 staging 区范围内
 * 6. 头部 CRC-32 校验
 *
 * @param header   OTA 头部指针
 * @param staging  staging flash 区域描述
 * @param primary  primary flash 区域描述
 * @param policy   镜像策略（load_addr、max_image_size、vector_is_valid）
 * @return         FB_OK 成功；FB_ERR_PARAM 参数无效；FB_ERR_FORMAT 格式错误；FB_ERR_CRC CRC 不匹配
 */
static fboot_status_t validate_header(
    const fastboot_ota_header_t *header,
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy)
{
    if (!header || !staging || !primary || !policy ||
        !policy->vector_is_valid) {
        return FB_ERR_PARAM;
    }
    if (header->magic != FASTBOOT_OTA_MAGIC ||
        header->header_size != sizeof(*header) ||
        header->header_version != FASTBOOT_OTA_HEADER_VER ||
        header->image_offset < sizeof(*header) ||
        header->image_size < 8u ||
        header->image_size > primary->size ||
        header->image_size > policy->max_image_size ||
        header->load_addr != policy->load_addr ||
        !range_fits(header->image_offset, header->image_size,
                    staging->size)) {
        return FB_ERR_FORMAT;
    }
    if (header_crc32(header) != header->header_crc32) {
        return FB_ERR_CRC;
    }
    return FB_OK;
}

/**
 * @brief 主 OTA 安装流程
 *
 * 完整安装流程：
 * 1. 从 staging 区偏移 0 读取 OTA 头部
 * 2. 校验头部（magic、version、size、CRC）
 * 3. 读取镜像前 8 字节进行向量合法性验证
 * 4. 擦除 primary flash 全区
 * 5. 以 OTA_CHUNK_SIZE 为单位分块：从 staging 读 → 流式 CRC-32 → 写入 primary
 * 6. 流式 CRC 与头部 image_crc32 比对
 * 7. （可选）readback 验证：从 primary 重新读取并计算 CRC
 *
 * @note  每个分块写入后会喂看门狗；每隔 OTA_PROGRESS_STEP 输出进度日志。
 * @note  当 FASTBOOT_CFG_ENABLE_FWOT 为 0 时，函数直接返回 FB_ERR_FORMAT。
 *
 * @param staging   staging flash 区域描述
 * @param primary   primary flash 区域描述（安装目标）
 * @param policy    镜像策略
 * @param runtime   运行时接口（tick、看门狗）
 * @param log       日志输出接口（可为 NULL）
 * @return          FB_OK 成功；其他为错误码
 */
fboot_status_t fastboot_ota_install(const fastboot_flash_area_t *staging,
                                    const fastboot_flash_area_t *primary,
                                    const fastboot_image_policy_t *policy,
                                    const fastboot_runtime_t *runtime,
                                    const fboot_log_t *log)
{
#if FASTBOOT_CFG_ENABLE_FWOT
    fastboot_ota_header_t header;
    uint32_t image_crc = 0u;
    uint32_t remaining;
    uint32_t image_pos = 0u;
    uint32_t next_progress = OTA_PROGRESS_STEP;
    fboot_status_t rc;

    if (!staging || !primary || !policy || !policy->vector_is_valid ||
        !runtime || !runtime->tick_ms) {
        return FB_ERR_PARAM;
    }

    rc = fastboot_flash_area_read(staging, 0u, (uint8_t *)&header,
                                  sizeof(header));
    if (rc != FB_OK) {
        return rc;
    }
    rc = validate_header(&header, staging, primary, policy);
    if (rc != FB_OK) {
        return rc;
    }
    /* 读取镜像前 8 字节，调用策略回调验证向量合法性 */
    rc = fastboot_flash_area_read(staging, header.image_offset, s_chunk, 8u);
    if (rc != FB_OK) {
        return rc;
    }
    if (!policy->vector_is_valid(s_chunk, 8u, header.load_addr,
                                 header.image_size, policy->ctx)) {
        return FB_ERR_FORMAT;
    }

    fboot_log_dec32(log, "install image bytes", header.image_size);
    fboot_log_puts(log, "[FB] erase app flash");
    rc = fastboot_flash_area_erase(primary, 0u, primary->size);
    if (rc != FB_OK) {
        return rc;
    }
    fboot_log_puts(log, "[FB] write app flash");

    remaining = header.image_size;
    while (remaining > 0u) {
        size_t chunk = remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining;
        uint32_t src = header.image_offset + image_pos;

        /* 从 staging 区读取当前分块 */
        rc = fastboot_flash_area_read(staging, src, s_chunk, chunk);
        if (rc != FB_OK) {
            return rc;
        }

        /* 流式累积 CRC-32 并写入 primary */
        image_crc = fastboot_crc32(s_chunk, chunk, image_crc);
        rc = fastboot_flash_area_write(primary, image_pos, s_chunk, chunk);
        if (rc != FB_OK) {
            return rc;
        }

        image_pos += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
        fastboot_runtime_feed_watchdog(runtime);
        /* 每写满 OTA_PROGRESS_STEP 字节输出一次进度 */
        if (image_pos >= next_progress || remaining == 0u) {
            fboot_log_dec32(log, "install written", image_pos);
            while (next_progress <= image_pos) {
                next_progress += OTA_PROGRESS_STEP;
            }
        }
    }

    /* 流式 CRC 校验 */
    if (image_crc != header.image_crc32) {
        return FB_ERR_CRC;
    }
    fboot_log_puts(log, "[FB] stream crc ok");

#if FASTBOOT_CFG_ENABLE_READBACK_VERIFY
    {
        /* 从 primary 重新读取并计算 CRC，验证写入正确性 */
        uint32_t readback_crc = 0u;
        uint32_t rb_pos = 0u;
        uint32_t rb_remaining = header.image_size;

        fboot_log_puts(log, "[FB] readback verify");
        while (rb_remaining > 0u) {
            size_t rb_chunk = rb_remaining > OTA_CHUNK_SIZE
                                  ? OTA_CHUNK_SIZE
                                  : (size_t)rb_remaining;

            rc = fastboot_flash_area_read(primary, rb_pos, s_chunk, rb_chunk);
            if (rc != FB_OK) {
                return rc;
            }
            readback_crc = fastboot_crc32(s_chunk, rb_chunk, readback_crc);
            rb_pos += (uint32_t)rb_chunk;
            rb_remaining -= (uint32_t)rb_chunk;
            fastboot_runtime_feed_watchdog(runtime);
        }
        if (readback_crc != header.image_crc32) {
            fboot_log_puts(log, "[FB] readback CRC MISMATCH");
            return FB_ERR_VERIFY;
        }
    }
#endif

    fboot_log_puts(log, "[FB] install verified ok");
    return FB_OK;
#else
    (void)staging;
    (void)primary;
    (void)policy;
    (void)runtime;
    (void)log;
    return FB_ERR_FORMAT;
#endif
}
