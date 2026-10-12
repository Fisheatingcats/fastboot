/**
 * @file fastboot_status.h
 * @brief FastBoot 状态码定义
 *
 * 定义了 FastBoot 库统一使用的状态码枚举类型。
 * 可失败操作通常返回 fboot_status_t；查询和轻量包装函数也可能返回指针、布尔值或 void。
 * 非负值表示成功或控制状态，负值表示错误。
 */

#ifndef FASTBOOT_STATUS_H
#define FASTBOOT_STATUS_H

/**
 * @brief FastBoot 统一状态码
 *
 * 正值（含零）表示成功或信息性状态，负值表示错误。
 */
typedef enum {
    FB_OK = 0,            /**< @brief 操作成功 */
    FB_NO_UPDATE = 1,     /**< @brief 无待安装的固件更新 */
    FB_BUSY = 2,          /**< @brief 异步操作进行中，尚未完成 */
    FB_ERR_PARAM = -1,    /**< @brief 参数无效（空指针、类型不匹配等） */
    FB_ERR_RANGE = -2,    /**< @brief 偏移或长度超出 Flash 区域范围 */
    FB_ERR_FLASH = -3,    /**< @brief Flash 底层读/写/擦除失败 */
    FB_ERR_VERIFY = -4,   /**< @brief 回读校验失败 */
    FB_ERR_FORMAT = -5,   /**< @brief 数据格式错误（包头魔数、版本号等） */
    FB_ERR_CRC = -6,      /**< @brief CRC32 校验不通过 */
    FB_ERR_IO = -7,       /**< @brief I/O 传输错误（UART 超时、断帧等） */
    FB_ERR_ID = -8,       /**< @brief 设备或区域标识不匹配 */
} fboot_status_t;

#endif /* FASTBOOT_STATUS_H */
