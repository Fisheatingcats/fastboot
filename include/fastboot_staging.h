/**
 * @file fastboot_staging.h
 * @brief Staging 区固件安装接口
 *
 * 提供检测 staging 区是否存在待安装固件并自动安装到 primary 区的功能。
 */

#ifndef FASTBOOT_STAGING_H
#define FASTBOOT_STAGING_H

#include "fastboot_flash.h"
#include "fastboot_log.h"
#include "fastboot_ota.h"
#include "fastboot_runtime.h"
#include "fastboot_status.h"

/**
 * @brief 若 staging 区有待安装固件则执行安装
 *
 * 检查 staging 区是否包含合法的 FWOT 固件包，
 * 若有则校验并安装到 primary 区。
 * 无有效固件时返回 FB_NO_UPDATE。
 *
 * @param[in] staging  staging Flash 区域描述
 * @param[in] primary  primary Flash 区域描述
 * @param[in] policy   镜像安装策略
 * @param[in] runtime  运行时服务实例
 * @param[in] log      日志输出实例
 * @retval FB_OK        安装成功
 * @retval FB_NO_UPDATE staging 区无待安装固件
 * @retval FB_ERR_*     校验或写入失败
 */
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);

#endif /* FASTBOOT_STAGING_H */
