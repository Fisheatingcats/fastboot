/**
 * @file fastboot_ota.h
 * @brief OTA 固件安装接口
 *
 * 定义了 FWOT（FreeWatch OTA）包头结构、镜像策略、CRC32 校验函数
 * 以及从 staging 区安装固件到 primary 区的核心 API。
 */

#ifndef FASTBOOT_OTA_H
#define FASTBOOT_OTA_H

#include "fastboot_flash.h"
#include "fastboot_log.h"
#include "fastboot_runtime.h"
#include "fastboot_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief FWOT 包头魔数 "FWOT"（小端序） */
#define FASTBOOT_OTA_MAGIC        0x544F5746u
/** @brief 当前支持的 FWOT 包头版本号 */
#define FASTBOOT_OTA_HEADER_VER   1u

#pragma pack(push, 1)
/**
 * @brief FWOT 包头结构（紧缩对齐）
 *
 * 描述一个 FWOT 固件包的元数据，包括镜像偏移、大小、CRC32、加载地址等。
 * 存储于 staging 区的起始位置，安装前需校验 header_crc32。
 */
typedef struct {
    uint32_t magic;           /**< @brief 魔数，必须为 FASTBOOT_OTA_MAGIC */
    uint16_t header_size;     /**< @brief 包头结构大小（字节） */
    uint16_t header_version;  /**< @brief 包头版本号 */
    uint32_t flags;           /**< @brief 标志位（保留） */
    uint32_t image_offset;    /**< @brief 镜像数据相对包头的偏移 */
    uint32_t image_size;      /**< @brief 镜像数据大小（字节） */
    uint32_t image_crc32;     /**< @brief 镜像数据的 CRC32 校验值 */
    uint32_t load_addr;       /**< @brief 镜像加载地址 */
    uint32_t image_version;   /**< @brief 镜像版本号 */
    uint32_t reserved[7];     /**< @brief 保留字段 */
    uint32_t header_crc32;    /**< @brief 包头（不含此字段本身）的 CRC32 校验值 */
} fastboot_ota_header_t;
#pragma pack(pop)

/**
 * @brief 向量表校验回调函数类型
 *
 * 用于验证镜像起始处的中断向量表是否合法（如栈指针范围、复位向量地址等）。
 *
 * @param[in] vector    向量表数据指针
 * @param[in] len       向量表数据长度
 * @param[in] load_addr 镜像加载地址
 * @param[in] image_size 镜像总大小
 * @param[in] ctx       用户上下文指针
 * @return true 向量表合法，false 不合法
 */
typedef bool (*fastboot_vector_validate_fn)(const uint8_t *vector,
                                            size_t len,
                                            uint32_t load_addr,
                                            uint32_t image_size,
                                            void *ctx);

/**
 * @brief 镜像安装策略
 *
 * 描述固件镜像的加载地址、最大大小限制和向量表校验回调。
 * 用于 OTA 安装前的合法性检查。
 */
typedef struct {
    uint32_t load_addr;                   /**< @brief 预期加载地址 */
    uint32_t max_image_size;              /**< @brief 允许的最大镜像大小 */
    fastboot_vector_validate_fn vector_is_valid; /**< @brief 向量表校验回调 */
    void *ctx;                            /**< @brief 用户上下文指针 */
} fastboot_image_policy_t;

/**
 * @brief 从 staging 区安装固件到 primary 区
 *
 * 校验 FWOT 包头合法性、镜像 CRC32、向量表后，将镜像从 staging 区拷贝到 primary 区。
 * 安装过程中会周期性喂看门狗和输出日志。
 *
 * @param[in] staging  staging Flash 区域描述
 * @param[in] primary  primary Flash 区域描述
 * @param[in] policy   镜像安装策略
 * @param[in] runtime  运行时服务实例
 * @param[in] log      日志输出实例
 * @retval FB_OK        安装成功
 * @retval FB_NO_UPDATE staging 区无有效固件包
 * @retval FB_ERR_*     校验或写入失败
 */
fboot_status_t fastboot_ota_install(const fastboot_flash_area_t *staging,
                                    const fastboot_flash_area_t *primary,
                                    const fastboot_image_policy_t *policy,
                                    const fastboot_runtime_t *runtime,
                                    const fboot_log_t *log);

/**
 * @brief 计算 CRC32 校验值
 *
 * 使用标准 CRC32（0x04C11DB7 多项式）计算数据的校验值。
 *
 * @param[in] data  数据缓冲区
 * @param[in] len   数据长度（字节）
 * @param[in] seed  初始值（续算时传入上次结果，首次传 0xFFFFFFFF）
 * @return CRC32 校验值
 */
uint32_t fastboot_crc32(const uint8_t *data, size_t len, uint32_t seed);

#endif /* FASTBOOT_OTA_H */
