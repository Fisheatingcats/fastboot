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
 * 首先检查 staging 起始位置的 FWOT magic；匹配时再调用 OTA 安装 API 做完整校验。
 * magic 不匹配或 staging 功能禁用时返回 FB_NO_UPDATE；匹配但包损坏时返回错误。
 * 安装成功后请求擦除 staging 起始的 1 字节，具体擦除粒度由端口决定。
 *
 * @param[in] staging  staging Flash 区域描述
 * @param[in] primary  primary Flash 区域描述
 * @param[in] policy   镜像安装策略
 * @param[in] runtime  运行时服务实例
 * @param[in] log      日志输出实例，可为 NULL
 * @retval FB_OK        安装成功
 * @retval FB_NO_UPDATE staging 区无待安装固件
 * @return 其他错误由读取和安装流程传播。
 * @note 当前实现忽略安装后 staging 清理的返回值；FB_OK 不保证 magic 已清除。
 */
fboot_status_t fastboot_staging_install_if_pending(
    const fastboot_flash_area_t *staging,
    const fastboot_flash_area_t *primary,
    const fastboot_image_policy_t *policy,
    const fastboot_runtime_t *runtime,
    const fboot_log_t *log);

#endif /* FASTBOOT_STAGING_H */
