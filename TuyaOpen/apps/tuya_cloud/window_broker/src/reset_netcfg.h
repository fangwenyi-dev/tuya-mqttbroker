/**
 * @file reset_netcfg.h
 * @brief 重置配网功能模块
 *
 * 提供网络配置重置功能：通过检测连续上电次数判断是否需要重置配网。
 * 当连续 3 次上电（每次间隔不超过 5 秒）时，自动触发设备重置。
 *
 * 此文件从 TuyaOpen SDK switch_demo 示例移植。
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __RESET_NETCFG_H__
#define __RESET_NETCFG_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动网络配置重置检测
 *
 * 在设备启动时调用。读取 KV 存储中的上电计数器并递增，
 * 同时启动 5 秒定时器，超时后清零计数器。
 * 如果计数器达到阈值（3 次），则在 check 阶段触发重置。
 *
 * @return int OPRT_OK 成功，其他值失败
 */
int reset_netconfig_start(void);

/**
 * @brief 检查是否需要重置网络配置
 *
 * 在 tuya_iot_start() 之后调用。如果上电计数器达到阈值（3 次），
 * 则触发设备重置（tuya_iot_reset），清除 WiFi 和云绑定信息。
 *
 * @return int OPRT_OK 成功，其他值失败
 */
int reset_netconfig_check(void);

#ifdef __cplusplus
}
#endif

#endif /* __RESET_NETCFG_H__ */
