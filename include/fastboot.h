/**
 * @file fastboot.h
 * @brief FastBoot OTA 引导库聚合头文件
 *
 * 此头文件聚合了 FastBoot 库的所有公共模块，用户只需包含此文件即可使用全部功能。
 * 同时定义了库的版本号宏。
 */

#ifndef FASTBOOT_H
#define FASTBOOT_H

#include "fastboot_config.h"
#include "fastboot_flash.h"
#include "fastboot_runtime.h"
#include "fastboot_status.h"
#include "fastboot_transport.h"
#include "fastboot_writer.h"

#include "fastboot_ota.h"
#include "fastboot_queue.h"
#include "fastboot_staging.h"
#include "fastboot_ymodem.h"

/** @brief FastBoot 主版本号 */
#define FASTBOOT_VERSION_MAJOR  1
/** @brief FastBoot 次版本号 */
#define FASTBOOT_VERSION_MINOR  0
/** @brief FastBoot 补丁版本号 */
#define FASTBOOT_VERSION_PATCH  0

#endif /* FASTBOOT_H */
