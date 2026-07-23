/**
 * @file app_tuya_bridge.h
 * @brief 涂鸦 DP 桥接层 - 替代 Matter Bridge
 *
 * 管理 LoRa 子设备与涂鸦 DP 的映射关系，处理 DP 上报和接收。
 *
 * 核心设计（相比 Matter 的优势）：
 * 1. 位置语义无需反转：涂鸦 DP 0=关闭, 100=打开，与 LoRa 协议一致
 * 2. 内倒按钮支持：control 枚举扩展 value=3 对应 LoRa 内倒命令(value=200)
 * 3. 风锁模式支持：独立 wind_lock DP，对应 rwp_wind_lock_mode 属性
 * 4. 仅 SN 前缀 "5005" 的设备支持内倒和风锁模式
 *
 * DP 分配方案（每个开窗器 4 个 DP，最多 12 个开窗器 = 48 个 DP）：
 *   设备 N (0-indexed) 的 DP ID:
 *     位置状态:  N*4 + 1  (Value, 0-100, 0=关闭 100=打开)
 *     控制命令:  N*4 + 2  (Enum: 0=停止 1=打开 2=关闭 3=内倒)
 *     电池百分比: N*4 + 3  (Value, 0-100)
 *     风锁模式:  N*4 + 4  (Enum: 0=内倒模式 1=平开模式)
 *
 * 控制命令映射（参考 HA 集成 const.py）：
 *   DP control=0 (停止) → $SH 004, attribute=w_travel, value=101
 *   DP control=1 (打开) → $SH 004, attribute=w_travel, value=100
 *   DP control=2 (关闭) → $SH 004, attribute=w_travel, value=0
 *   DP control=3 (内倒) → $SH 004, attribute=w_travel, value=200
 *
 * 风锁模式映射（参考 HA 集成 const.py COMMAND_VALUE_WIND_LOCK_*）：
 *   DP wind_lock=0 (内倒模式) → $SH 004, attribute=rwp_wind_lock_mode, value=0
 *   DP wind_lock=1 (平开模式) → $SH 004, attribute=rwp_wind_lock_mode, value=1
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tuya_iot.h"
#include "tuya_iot_dp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 常量定义 ==================== */

/** 最大支持的 LoRa 子设备数量（12 设备 × 4 DP = 48 DP，DP ID 范围 1-48） */
#define MAX_TUYA_DEVICES        12

/** 每个设备的 DP 数量 */
#define DP_PER_DEVICE           4

/** 设备哈希表大小（2 的幂次，> MAX_TUYA_DEVICES）*/
#define DEVICE_HASH_SIZE        32

/** 支持 wind_lock 功能的设备 SN 前缀（内倒/平开模式），参考 const.py DEVICE_SN_PREFIX_WIND_LOCK */
#define WIND_LOCK_SN_PREFIX     "5005"

/** SN 前缀长度 */
#define WIND_LOCK_SN_PREFIX_LEN 4

/* ==================== 设备生命周期状态机 ==================== */

typedef enum {
    DEV_STATE_IDLE        = 0,    /**< 未注册 */
    DEV_STATE_REGISTERING = 1,    /**< 正在注册 DP */
    DEV_STATE_ONLINE      = 2,    /**< 在线 */
    DEV_STATE_OFFLINE     = 3,    /**< 离线（网关超时） */
    DEV_STATE_REMOVING    = 4,    /**< 正在解绑 */
} device_state_t;

/* ==================== DP 偏移量定义 ==================== */

/** DP 偏移量（设备 N 的 DP ID = N*4 + offset + 1） */
typedef enum {
    DP_OFFSET_POSITION    = 0,  /**< 位置状态 DP (Value 0-100) */
    DP_OFFSET_CONTROL     = 1,  /**< 控制命令 DP (Enum: 0=停止 1=打开 2=关闭 3=内倒) */
    DP_OFFSET_BATTERY     = 2,  /**< 电池百分比 DP (Value 0-100) */
    DP_OFFSET_WIND_LOCK   = 3,  /**< 风锁模式 DP (Enum: 0=内倒模式 1=平开模式) */
} dp_offset_t;

/* ==================== 控制类型枚举 ==================== */

/** 控制命令类型（对应 DP control 枚举值） */
typedef enum {
    CTRL_STOP   = 0,  /**< 停止 → $SH w_travel value=101 */
    CTRL_OPEN   = 1,  /**< 打开 → $SH w_travel value=100 */
    CTRL_CLOSE  = 2,  /**< 关闭 → $SH w_travel value=0   */
    CTRL_TILT   = 3,  /**< 内倒 → $SH w_travel value=200 */
} control_type_t;

/** 风锁模式（对应 DP wind_lock 枚举值） */
typedef enum {
    WIND_LOCK_TILT_MODE = 0,  /**< 内倒模式 → $SH rwp_wind_lock_mode value=0 */
    WIND_LOCK_FLAT_MODE = 1,  /**< 平开模式 → $SH rwp_wind_lock_mode value=1 */
} wind_lock_mode_t;

/* ==================== API 函数 ==================== */

/**
 * @brief 初始化涂鸦 DP 桥接层
 *
 * @param client TuyaOpen IoT 客户端句柄（用于 DP 上报）
 */
void app_tuya_bridge_init(tuya_iot_client_t *client);

/**
 * @brief 处理涂鸦 DP 接收事件
 *
 * 在 TUYA_EVENT_DP_RECEIVE_OBJ 事件回调中调用，解析 DP 命令并
 * 通过协议桥接层发送对应的 $SH 控制命令到 LoRa 网关。
 *
 * @param dpobj DP 接收数据（来自 TuyaOpen 事件）
 */
void app_tuya_bridge_handle_dp_recv(dp_obj_recv_t *dpobj);

/**
 * @brief 注册 LoRa 子设备并分配 DP
 *
 * 由协议桥接层在收到 002 设备列表时调用。为每个新设备分配
 * 一组 DP ID（位置/控制/电池/风锁模式）。
 *
 * @param device_sn LoRa 设备 SN
 * @return >=0 分配的设备索引，-1 设备列表已满，-2 设备已存在
 */
int app_tuya_bridge_add_device(const char *device_sn);

/**
 * @brief 移除已注册的 LoRa 子设备
 *
 * @param device_sn LoRa 设备 SN
 */
void app_tuya_bridge_remove_device(const char *device_sn);

/**
 * @brief 移除所有已注册的设备
 */
void app_tuya_bridge_remove_all_devices(void);

/**
 * @brief 更新位置 DP（$SH 005 → 涂鸦云）
 *
 * 位置语义与 LoRa 一致（0=关闭, 100=打开），无需反转。
 *
 * @param device_sn LoRa 设备 SN
 * @param position 位置值 0-100（>100 的异常值会被丢弃）
 */
void app_tuya_bridge_update_position(const char *device_sn, uint8_t position);

/**
 * @brief 更新电池百分比 DP（$SH 005 → 涂鸦云）
 *
 * @param device_sn LoRa 设备 SN
 * @param percent 电池百分比 0-100
 */
void app_tuya_bridge_update_battery(const char *device_sn, uint8_t percent);

/**
 * @brief 更新风锁模式 DP（$SH 005 → 涂鸦云）
 *
 * 仅对 SN 前缀为 "5005" 的设备有效。
 *
 * @param device_sn LoRa 设备 SN
 * @param mode 风锁模式（0=内倒模式, 1=平开模式）
 */
void app_tuya_bridge_update_wind_lock(const char *device_sn, uint8_t mode);

/**
 * @brief 更新设备在线状态
 *
 * @param device_sn LoRa 设备 SN
 * @param online true=在线, false=离线
 */
void app_tuya_bridge_update_online(const char *device_sn, bool online);

/**
 * @brief 批量上报位置+电池 DP（减少网络开销）
 *
 * 将位置和电池合并为一次 tuya_iot_dp_obj_report 调用。
 *
 * @param device_sn LoRa 设备 SN
 * @param position 位置值 0-100（<0 表示不更新）
 * @param battery 电池百分比 0-100（<0 表示不更新）
 */
void app_tuya_bridge_update_status_batch(const char *device_sn, int16_t position, int16_t battery);

/**
 * @brief 获取设备当前状态
 *
 * @param device_sn LoRa 设备 SN
 * @return 设备状态枚举值，DEV_STATE_IDLE 表示未注册
 */
device_state_t app_tuya_bridge_get_state(const char *device_sn);

/**
 * @brief 检查设备是否支持风锁模式（内倒/平开）
 *
 * 只有 SN 前四位为 "5005" 的 LoRa 子设备支持内倒功能。
 * 参考 const.py supports_wind_lock_mode()
 *
 * @param device_sn LoRa 设备 SN
 * @return true 支持, false 不支持
 */
bool app_tuya_bridge_supports_wind_lock(const char *device_sn);

/**
 * @brief 获取已注册设备数量
 */
int app_tuya_bridge_device_count(void);

/**
 * @brief 通过设备 SN 查找 DP 组索引
 *
 * @param device_sn LoRa 设备 SN
 * @return >=0 设备索引, -1 未找到
 */
int app_tuya_bridge_find_device(const char *device_sn);

/**
 * @brief 获取设备的 DP ID
 *
 * @param device_index 设备索引（0-11）
 * @param dp_offset DP 偏移（0=位置, 1=控制, 2=电池, 3=风锁）
 * @return DP ID（1-48）
 */
static inline uint8_t app_tuya_bridge_dp_id(int device_index, int dp_offset)
{
    return (uint8_t)(device_index * DP_PER_DEVICE + dp_offset + 1);
}

#ifdef __cplusplus
}
#endif
