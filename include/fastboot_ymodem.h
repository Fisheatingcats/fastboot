/**
 * @file fastboot_ymodem.h
 * @brief YMODEM-1K 固件接收接口
 *
 * 提供通过 YMODEM-1K 协议从传输介质接收固件数据并写入 Flash 的功能。
 */

#ifndef FASTBOOT_YMODEM_H
#define FASTBOOT_YMODEM_H

#include "fastboot_runtime.h"
#include "fastboot_status.h"
#include "fastboot_transport.h"
#include "fastboot_writer.h"
#include <stdint.h>

/**
 * @brief 通过 YMODEM-1K 协议接收固件
 *
 * 使用 YMODEM-1K 协议从传输介质接收固件数据包，
 * 经过 CRC 校验后通过 writer 写入 Flash。
 * 接收过程中会周期性喂看门狗。
 *
 * @param[in]  transport  字节传输接口实例
 * @param[in]  writer     数据写入器实例
 * @param[in]  runtime    运行时服务实例（用于喂狗和超时检测）
 * @param[out] out_size   接收完成后的固件总大小（字节），可为 NULL
 * @retval FB_OK    接收并写入成功
 * @retval FB_ERR_IO    传输超时或断帧
 * @retval FB_ERR_CRC   数据包 CRC 校验失败
 * @retval FB_ERR_VERIFY 固件整体校验失败
 * @retval FB_ERR_*     其他错误
 */
fboot_status_t fastboot_ymodem_receive(const fastboot_transport_t *transport,
                                       const fastboot_writer_t *writer,
                                       const fastboot_runtime_t *runtime,
                                       uint32_t *out_size);

#endif /* FASTBOOT_YMODEM_H */
