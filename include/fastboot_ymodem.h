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
 * FB_OK 表示文件接收与 writer 写入均已完成，不表示 FWOT 完整镜像已校验或安装。
 *
 * @param[in]  transport  字节传输接口实例
 * @param[in]  writer     写入器实例；当前入口要求 begin 和 write 均有效，异步回调可选
 * @param[in]  runtime    运行时服务实例（用于喂狗和超时检测）
 * @param[out] out_size   已接收的文件字节数，可为 NULL；仅在 FB_OK 时作为成功结果使用
 * @retval FB_OK    接收并写入成功
 * @retval FB_ERR_IO    传输超时或断帧
 * @retval FB_NO_UPDATE 首包文件名为空，无文件可接收
 * @retval FB_ERR_PARAM 必需回调无效，或 YMODEM 功能禁用
 * @retval FB_ERR_FORMAT 文件元数据或完成时的字节数不匹配
 * @retval FB_ERR_RANGE 文件大小无效或 writer 拒绝容量
 * @return 其他错误由 writer 传播；包 CRC16 错误进入协议重试，超限返回 FB_ERR_IO。
 * @note 使用静态队列和接收上下文，仅允许一个活动接收会话，不可重入。
 */
fboot_status_t fastboot_ymodem_receive(const fastboot_transport_t *transport,
                                       const fastboot_writer_t *writer,
                                       const fastboot_runtime_t *runtime,
                                       uint32_t *out_size);

#endif /* FASTBOOT_YMODEM_H */
