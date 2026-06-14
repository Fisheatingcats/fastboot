/**
 * @file fastboot_staging.c
 * @brief Staging 区待安装固件检测与自动安装
 *
 * 提供开机时自动检查 staging 区是否存在待安装固件（通过 magic 标识），
 * 若存在则调用 OTA 安装流程，安装成功后清除 staging 区。
 */

#include "fastboot_staging.h"

/**
 * @brief 检查 staging 区是否有待安装固件，若有则执行安装
 *
 * 流程：
 * 1. 从 staging 区偏移 0 读取 4 字节 magic
 * 2. 若 magic 不匹配 FASTBOOT_OTA_MAGIC，返回 FB_NO_UPDATE
 * 3. 调用 fastboot_ota_install() 执行完整安装
 * 4. 安装成功后擦除 staging 区第 1 个扇区（清除 magic，防止重复安装）
 *
 * @note  当 FASTBOOT_CFG_ENABLE_STAGING 为 0 时，直接返回 FB_NO_UPDATE。
 *
 * @param staging   staging flash 区域描述
 * @param primary   primary flash 区域描述（安装目标）
 * @param policy    镜像策略
 * @param runtime   运行时接口（tick、看门狗）
 * @param log       日志输出接口（可为 NULL）
 * @return          FB_OK 安装成功；FB_NO_UPDATE 无待安装固件；其他为错误码
 */
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log)
{
#if FASTBOOT_CFG_ENABLE_STAGING
    uint32_t magic = 0u;
    fboot_status_t rc;

    if (!staging || !primary || !policy) {
        return FB_ERR_PARAM;
    }

    rc = fastboot_flash_area_read(staging, 0u, (uint8_t *)&magic,
                                  sizeof(magic));
    if (rc != FB_OK) {
        return rc;
    }
    if (magic != FASTBOOT_OTA_MAGIC) {
        return FB_NO_UPDATE;
    }

    rc = fastboot_ota_install(staging, primary, policy, runtime, log);
    if (rc == FB_OK) {
        /* 安装成功，擦除 staging 区第 1 个扇区以清除 magic */
        (void)fastboot_flash_area_erase(staging, 0u, 1u);
    }
    return rc;
#else
    (void)staging;
    (void)primary;
    (void)policy;
    (void)runtime;
    (void)log;
    return FB_NO_UPDATE;
#endif
}
