/**
 * @file app_protocol_bridge.h
 * @brief $SH MQTT 协议 ↔ 涂鸦 DP 桥接层
 *
 * 实现 $SH MQTT 协议的完整处理，将 LoRa 网关上报的状态转换为涂鸦 DP 上报，
 * 将涂鸦 App 下发的 DP 控制命令转换为 $SH 004 控制命令。
 *
 * 完整 $SH 协议处理：
 * - 001：网关绑定（自动回复 errcode=0，记录网关 SN）
 * - 002：设备列表（解析 devices 数组，为每个设备注册涂鸦 DP）
 * - 003：设备配对/解绑
 * - 004：设备控制响应
 * - 005：设备状态上报（双格式：直接字段 + attrs 数组）
 *
 * 相比 Matter 方案的变化：
 * - 移除 Matter 依赖（StackLock、matter_event_t、app_matter_bridge_*）
 * - 005 状态上报改为调用 app_tuya_bridge 的 DP 上报函数
 * - DP 接收通过 app_tuya_bridge 直接调用本层发送函数
 * - 无事件队列，直接函数调用
 *
 * 协议参考：e:\AI\huijian-gateway\ha-window-controller-gateway\custom_components\window_controller_gateway
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tal_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 配置常量 ==================== */

/** 最大网关数量 */
#define MAX_GATEWAYS                10

/** 最大子设备数量（与 app_tuya_bridge MAX_TUYA_DEVICES 一致） */
#define MAX_BRIDGED_DEVICES         12

/** 网关离线超时（秒），参考 HA 集成 GATEWAY_TIMEOUT_SECONDS */
#define GATEWAY_OFFLINE_TIMEOUT_SEC 900

/* ==================== 数据结构 ==================== */

/**
 * @brief 协议桥接配置
 */
typedef struct {
    const char *bridge_sn;          /**< 本桥接器的序列号（用于 001 响应的 uuid 字段，建议用 MAC 地址） */
} protocol_bridge_config_t;

/* ==================== API 函数 ==================== */

/**
 * @brief 初始化协议桥接层（创建 MQTT 客户端，但不启动）
 */
int app_protocol_bridge_init(const protocol_bridge_config_t *config);

/**
 * @brief 启动协议桥接 task（等待 WiFi 连接后自动启动 MQTT 客户端）
 */
int app_protocol_bridge_start(void);

/**
 * @brief WiFi 连接成功后启动 MQTT 客户端（内部调用，由事件处理器触发）
 */
int app_protocol_bridge_on_wifi_connected(void);

/**
 * @brief 停止协议桥接
 */
void app_protocol_bridge_stop(void);

/**
 * @brief 检查 LoRa 网关离线状态（超时未上报则标记离线）
 *
 * 由系统监控 task 定期调用。网关超过 GATEWAY_OFFLINE_TIMEOUT_SEC 秒未上报
 * 任何消息则标记为离线（online=false）。
 * 注意：不删除已离线网关的 DP 映射，避免设备恢复时重新分配 DP。
 */
void app_protocol_bridge_check_gateway_offline(void);

/**
 * @brief 启动所有已注册网关的 LoRa 配对模式
 *
 * 发送 003 类型（ctype=003, bind=1）命令到所有在线网关，
 * 触发网关进入配对模式（60 秒内可配对子设备）。
 */
void app_protocol_bridge_start_pairing(void);

/**
 * @brief 删除所有 LoRa 子设备
 *
 * 遍历所有已注册网关下的所有子设备，逐个发送解绑命令（ctype=003, bind=0）。
 * 设备间延迟 200ms 避免 MQTT 报文拥堵。
 */
void app_protocol_bridge_delete_all_devices(void);

/**
 * @brief 发送设备控制命令（涂鸦 DP → $SH 004）
 *
 * 由 app_tuya_bridge 在收到 DP 控制命令时调用。
 * 发送 ctype=004, attribute=w_travel, value=<lora_value> 到设备所属网关。
 *
 * @param device_sn LoRa 设备 SN
 * @param lora_value LoRa 控制值（0=关闭, 100=打开, 101=停止, 200=内倒）
 */
void app_protocol_bridge_send_control(const char *device_sn, int lora_value);

/**
 * @brief 发送风锁模式命令（涂鸦 DP → $SH 004）
 *
 * 由 app_tuya_bridge 在收到风锁模式 DP 命令时调用。
 * 发送 ctype=004, attribute=rwp_wind_lock_mode, value=<mode> 到设备所属网关。
 *
 * @param device_sn LoRa 设备 SN
 * @param mode 风锁模式（0=内倒模式, 1=平开模式）
 */
void app_protocol_bridge_send_wind_lock(const char *device_sn, uint8_t mode);

/**
 * @brief 重置涂鸦配网（长按按键触发）
 *
 * 清除涂鸦云绑定信息，重启进入配网模式。
 * 由 app_button 在长按时调用。
 */
void app_protocol_bridge_reset_tuya(void);

#ifdef __cplusplus
}
#endif
