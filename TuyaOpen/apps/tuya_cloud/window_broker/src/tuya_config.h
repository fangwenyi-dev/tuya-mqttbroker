/**
 * @file tuya_config.h
 * @brief 智能开窗器网关 - TuyaOpen 配置
 *
 * 定义产品 ID（PID），UUID/AuthKey 在 tuya_config_secrets.h 中配置。
 *
 * @copyright Copyright (c) 2024-2025. All Rights Reserved.
 */

#ifndef TUYA_CONFIG_H_
#define TUYA_CONFIG_H_

#if __has_include("tuya_config_secrets.h")
#include "tuya_config_secrets.h"
#endif

/**
 * @brief 产品信息配置
 *
 * TUYA_PRODUCT_ID: PID, 在涂鸦 IoT 平台创建产品后获取
 * TUYA_OPENSDK_UUID: UUID, 在涂鸦平台申请 open-sdk 授权码获取
 * TUYA_OPENSDK_AUTHKEY: AUTHKEY, 同上
 *
 * 详细步骤参考使用说明文档第四章"涂鸦平台配置"
 *
 * warning: 请替换为你的产品 ID 和授权信息，否则设备无法连接涂鸦云
 */
// clang-format off
#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID      "your_product_id_here"                    // 请替换为你的产品 ID
#endif

#ifndef TUYA_OPENSDK_UUID
#define TUYA_OPENSDK_UUID      "uuidxxxxxxxxxxxxxxxx"                    // 请替换为正确的 UUID
#endif
#ifndef TUYA_OPENSDK_AUTHKEY
#define TUYA_OPENSDK_AUTHKEY   "keyxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"        // 请替换为正确的 AuthKey
#endif

/**
 * @brief AP 配网 PINCODE
 *
 * TUYA_NETCFG_PINCODE: AP 配网的随机 PIN 码，由涂鸦 PMS 系统生成。
 * WARNING: AP 配网需要 PINCODE
 */
// #define TUYA_NETCFG_PINCODE   "69832860"

// clang-format on

#endif /* TUYA_CONFIG_H_ */
