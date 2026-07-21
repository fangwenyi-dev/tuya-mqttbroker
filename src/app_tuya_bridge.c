/**
 * @file app_tuya_bridge.c
 * @brief 涂鸦 DP 桥接层实现 - 替代 Matter Bridge
 *
 * 管理 LoRa 子设备与涂鸦 DP 的映射，处理双向数据流：
 * - DP 接收 → $SH 控制命令（App → LoRa 设备）
 * - $SH 状态上报 → DP 上报（LoRa 设备 → App）
 *
 * 相比 Matter 的改进：
 * 1. 无位置反转（涂鸦 DP 0=关闭 与 LoRa 一致）
 * 2. 支持内倒按钮（control=3 → value=200，参考 HA 集成 COMMAND_VALUE_TOGGLE）
 * 3. 支持风锁模式（独立 wind_lock DP，参考 HA 集成 rwp_wind_lock_mode）
 * 4. 无需 StackLock（TuyaOpen SDK 内部管理线程安全）
 * 5. 无事件队列（直接函数调用，架构更简洁）
 */

#include "app_tuya_bridge.h"
#include "app_protocol_bridge.h"
#include "tal_api.h"
#include "tal_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tuya_bridge";

/* ==================== 内部状态 ==================== */

/** 设备-DP 映射表 */
typedef struct {
    char    device_sn[32];   /**< LoRa 设备 SN */
    bool    in_use;          /**< 条目是否在用 */
    bool    online;          /**< 设备在线状态 */
} tuya_device_entry_t;

static tuya_device_entry_t s_devices[MAX_TUYA_DEVICES] = {0};
static tuya_iot_client_t  *s_client = NULL;

/** 映射表访问互斥锁（DP 接收在 yield 线程，状态上报在 bridge task） */
static MUTEX_HANDLE       s_map_mutex = NULL;

/* ==================== 内部辅助函数 ==================== */

/**
 * @brief 加锁访问映射表
 */
static void map_lock(void)
{
    if (s_map_mutex) {
        tal_mutex_lock(s_map_mutex);
    }
}

static void map_unlock(void)
{
    if (s_map_mutex) {
        tal_mutex_unlock(s_map_mutex);
    }
}

/**
 * @brief 上报单个 DP（Value 类型）
 */
static void report_dp_value(uint8_t dp_id, int32_t value)
{
    if (s_client == NULL) {
        PR_ERR("client 未初始化，无法上报 DP %d", dp_id);
        return;
    }

    dp_obj_t dp = {0};
    dp.id = dp_id;
    dp.type = PROP_VALUE;
    dp.value.dp_value = value;
    dp.time_stamp = tal_time_get_posix();

    int rt = tuya_iot_dp_obj_report(s_client, NULL, &dp, 1, 0);
    if (rt != OPRT_OK) {
        PR_ERR("DP %d 上报失败: %d", dp_id, rt);
    } else {
        PR_DEBUG("DP %d 上报成功: value=%d", dp_id, value);
    }
}

/**
 * @brief 上报单个 DP（Enum 类型）
 */
static void report_dp_enum(uint8_t dp_id, uint32_t value)
{
    if (s_client == NULL) {
        PR_ERR("client 未初始化，无法上报 DP %d", dp_id);
        return;
    }

    dp_obj_t dp = {0};
    dp.id = dp_id;
    dp.type = PROP_ENUM;
    dp.value.dp_enum = value;
    dp.time_stamp = tal_time_get_posix();

    int rt = tuya_iot_dp_obj_report(s_client, NULL, &dp, 1, 0);
    if (rt != OPRT_OK) {
        PR_ERR("DP %d 上报失败: %d", dp_id, rt);
    } else {
        PR_DEBUG("DP %d 上报成功: enum=%u", dp_id, value);
    }
}

/**
 * @brief 处理控制命令 DP（DP type = Enum）
 *
 * 将涂鸦控制枚举映射为 $SH 004 命令值。
 * 参考 HA 集成 mqtt_handler.py send_command() 中 open/close/stop/a 的映射。
 *
 * @param device_index 设备索引
 * @param value 控制枚举值（0=停止, 1=打开, 2=关闭, 3=内倒）
 */
static void handle_control_dp(int device_index, uint32_t value)
{
    const char *sn = s_devices[device_index].device_sn;

    /* 控制命令值映射（参考 HA 集成 const.py COMMAND_VALUE_*） */
    int lora_value;
    const char *action_name;

    switch (value) {
    case CTRL_STOP:
        lora_value = 101;   /* COMMAND_VALUE_STOP */
        action_name = "停止";
        break;
    case CTRL_OPEN:
        lora_value = 100;   /* COMMAND_VALUE_OPEN */
        action_name = "打开";
        break;
    case CTRL_CLOSE:
        lora_value = 0;     /* COMMAND_VALUE_CLOSE */
        action_name = "关闭";
        break;
    case CTRL_TILT:
        lora_value = 200;   /* COMMAND_VALUE_TOGGLE (内倒) */
        action_name = "内倒";
        break;
    default:
        PR_WARN("未知的控制命令: %u, 设备: %s", value, sn);
        return;
    }

    PR_INFO("DP 控制命令: 设备=%s 动作=%s → $SH value=%d", sn, action_name, lora_value);

    /* 通过协议桥接层发送 $SH 004 控制命令 */
    app_protocol_bridge_send_control(sn, lora_value);
}

/**
 * @brief 处理风锁模式 DP（DP type = Enum）
 *
 * 将涂鸦风锁模式枚举映射为 $SH 004 rwp_wind_lock_mode 命令。
 * 参考 HA 集成 mqtt_handler.py wind_lock_tilt/wind_lock_flat 的映射。
 *
 * @param device_index 设备索引
 * @param value 风锁模式（0=内倒模式, 1=平开模式）
 */
static void handle_wind_lock_dp(int device_index, uint32_t value)
{
    const char *sn = s_devices[device_index].device_sn;

    /* 仅支持风锁模式的设备才处理 */
    if (!app_tuya_bridge_supports_wind_lock(sn)) {
        PR_WARN("设备 %s 不支持风锁模式，忽略 DP 命令", sn);
        return;
    }

    if (value != WIND_LOCK_TILT_MODE && value != WIND_LOCK_FLAT_MODE) {
        PR_WARN("无效的风锁模式: %u, 设备: %s", value, sn);
        return;
    }

    const char *mode_name = (value == WIND_LOCK_TILT_MODE) ? "内倒模式" : "平开模式";
    PR_INFO("DP 风锁模式: 设备=%s 模式=%s → $SH rwp_wind_lock_mode value=%u", sn, mode_name, value);

    /* 通过协议桥接层发送 $SH 004 风锁模式命令 */
    app_protocol_bridge_send_wind_lock(sn, (uint8_t)value);
}

/* ==================== 公共 API 实现 ==================== */

void app_tuya_bridge_init(tuya_iot_client_t *client)
{
    s_client = client;

    /* 创建映射表互斥锁 */
    if (s_map_mutex == NULL) {
        if (tal_mutex_create_init(&s_map_mutex) != OPRT_OK) {
            PR_ERR("创建映射表互斥锁失败");
        }
    }

    /* 清空设备表 */
    memset(s_devices, 0, sizeof(s_devices));

    PR_NOTICE("涂鸦 DP 桥接层已初始化 (最大设备数: %d, 每设备 %d DP)", MAX_TUYA_DEVICES, DP_PER_DEVICE);
}

void app_tuya_bridge_handle_dp_recv(dp_obj_recv_t *dpobj)
{
    if (dpobj == NULL || dpobj->dpscnt == 0) {
        return;
    }

    for (uint32_t i = 0; i < dpobj->dpscnt; i++) {
        dp_obj_t *dp = &dpobj->dps[i];
        PR_DEBUG("收到 DP: id=%d type=%d", dp->id, dp->type);

        /* 计算 DP 属于哪个设备和哪个偏移 */
        /* DP ID 范围 1-48，设备索引 = (dp_id - 1) / 4，偏移 = (dp_id - 1) % 4 */
        if (dp->id < 1 || dp->id > MAX_TUYA_DEVICES * DP_PER_DEVICE) {
            PR_WARN("DP ID %d 超出范围 [1, %d]，忽略", dp->id, MAX_TUYA_DEVICES * DP_PER_DEVICE);
            continue;
        }

        int device_index = (dp->id - 1) / DP_PER_DEVICE;
        int dp_offset    = (dp->id - 1) % DP_PER_DEVICE;

        /* 检查设备是否已注册 */
        map_lock();
        if (!s_devices[device_index].in_use) {
            map_unlock();
            PR_WARN("DP %d 对应的设备 %d 未注册，忽略", dp->id, device_index);
            continue;
        }
        map_unlock();

        switch (dp_offset) {
        case DP_OFFSET_CONTROL:
            /* 控制命令 DP（Enum 类型） */
            if (dp->type == PROP_ENUM) {
                handle_control_dp(device_index, dp->value.dp_enum);
            } else {
                PR_WARN("控制 DP %d 类型错误: 期望 Enum(%d), 实际 %d", dp->id, PROP_ENUM, dp->type);
            }
            break;

        case DP_OFFSET_WIND_LOCK:
            /* 风锁模式 DP（Enum 类型） */
            if (dp->type == PROP_ENUM) {
                handle_wind_lock_dp(device_index, dp->value.dp_enum);
            } else {
                PR_WARN("风锁 DP %d 类型错误: 期望 Enum(%d), 实际 %d", dp->id, PROP_ENUM, dp->type);
            }
            break;

        case DP_OFFSET_POSITION:
        case DP_OFFSET_BATTERY:
            /* 位置和电池是只读 DP，App 不应下发，忽略 */
            PR_DEBUG("只读 DP %d 收到下发命令，忽略", dp->id);
            break;

        default:
            PR_WARN("未知 DP 偏移: %d (DP ID=%d)", dp_offset, dp->id);
            break;
        }
    }
}

int app_tuya_bridge_add_device(const char *device_sn)
{
    if (device_sn == NULL || device_sn[0] == '\0') {
        return -1;
    }

    map_lock();

    /* 检查是否已存在 */
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use && strcmp(s_devices[i].device_sn, device_sn) == 0) {
            map_unlock();
            PR_DEBUG("设备 %s 已存在 (索引 %d)", device_sn, i);
            return -2;
        }
    }

    /* 查找空闲槽位 */
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            strncpy(s_devices[i].device_sn, device_sn, sizeof(s_devices[i].device_sn) - 1);
            s_devices[i].device_sn[sizeof(s_devices[i].device_sn) - 1] = '\0';
            s_devices[i].in_use = true;
            s_devices[i].online = true;

            bool has_wind_lock = app_tuya_bridge_supports_wind_lock(device_sn);
            PR_NOTICE("设备已注册: sn=%s 索引=%d DP范围=[%d-%d] 风锁=%s",
                      device_sn, i,
                      app_tuya_bridge_dp_id(i, 0),
                      app_tuya_bridge_dp_id(i, DP_PER_DEVICE - 1),
                      has_wind_lock ? "支持" : "不支持");

            map_unlock();
            return i;
        }
    }

    map_unlock();
    PR_ERR("设备列表已满 (最大 %d)，无法添加 %s", MAX_TUYA_DEVICES, device_sn);
    return -1;
}

void app_tuya_bridge_remove_device(const char *device_sn)
{
    if (device_sn == NULL) return;

    map_lock();
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use && strcmp(s_devices[i].device_sn, device_sn) == 0) {
            PR_NOTICE("设备已移除: sn=%s 索引=%d", device_sn, i);
            s_devices[i].in_use = false;
            s_devices[i].online = false;
            s_devices[i].device_sn[0] = '\0';
            break;
        }
    }
    map_unlock();
}

void app_tuya_bridge_remove_all_devices(void)
{
    map_lock();
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use) {
            PR_NOTICE("移除设备: sn=%s 索引=%d", s_devices[i].device_sn, i);
        }
        s_devices[i].in_use = false;
        s_devices[i].online = false;
        s_devices[i].device_sn[0] = '\0';
    }
    map_unlock();
}

void app_tuya_bridge_update_position(const char *device_sn, uint8_t position)
{
    if (device_sn == NULL) return;

    /* 范围校验：>100 表示未校准/离线标记，丢弃（与 Matter 方案一致） */
    if (position > 100) {
        PR_WARN("位置值 %d 超出范围 [0,100]，丢弃: 设备=%s", position, device_sn);
        return;
    }

    int idx = app_tuya_bridge_find_device(device_sn);
    if (idx < 0) {
        PR_WARN("设备 %s 未注册，无法上报位置", device_sn);
        return;
    }

    /* 位置语义无需反转：涂鸦 DP 0=关闭, 100=打开，与 LoRa 一致 */
    uint8_t dp_id = app_tuya_bridge_dp_id(idx, DP_OFFSET_POSITION);
    report_dp_value(dp_id, (int32_t)position);
}

void app_tuya_bridge_update_battery(const char *device_sn, uint8_t percent)
{
    if (device_sn == NULL) return;

    /* 范围校验 */
    if (percent > 100) {
        PR_WARN("电池百分比 %d 超出范围 [0,100]，丢弃: 设备=%s", percent, device_sn);
        return;
    }

    int idx = app_tuya_bridge_find_device(device_sn);
    if (idx < 0) {
        PR_WARN("设备 %s 未注册，无法上报电池", device_sn);
        return;
    }

    uint8_t dp_id = app_tuya_bridge_dp_id(idx, DP_OFFSET_BATTERY);
    report_dp_value(dp_id, (int32_t)percent);
}

void app_tuya_bridge_update_wind_lock(const char *device_sn, uint8_t mode)
{
    if (device_sn == NULL) return;

    /* 仅支持风锁模式的设备 */
    if (!app_tuya_bridge_supports_wind_lock(device_sn)) {
        return;
    }

    if (mode != WIND_LOCK_TILT_MODE && mode != WIND_LOCK_FLAT_MODE) {
        PR_WARN("无效的风锁模式: %u, 设备: %s", mode, device_sn);
        return;
    }

    int idx = app_tuya_bridge_find_device(device_sn);
    if (idx < 0) {
        PR_WARN("设备 %s 未注册，无法上报风锁模式", device_sn);
        return;
    }

    uint8_t dp_id = app_tuya_bridge_dp_id(idx, DP_OFFSET_WIND_LOCK);
    report_dp_enum(dp_id, (uint32_t)mode);
}

void app_tuya_bridge_update_online(const char *device_sn, bool online)
{
    if (device_sn == NULL) return;

    map_lock();
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use && strcmp(s_devices[i].device_sn, device_sn) == 0) {
            if (s_devices[i].online != online) {
                s_devices[i].online = online;
                PR_NOTICE("设备在线状态变更: sn=%s online=%d", device_sn, online);
            }
            break;
        }
    }
    map_unlock();
}

bool app_tuya_bridge_supports_wind_lock(const char *device_sn)
{
    if (device_sn == NULL) return false;
    /* 参考 const.py supports_wind_lock_mode(): 只有 SN 前四位为 "5005" 才支持 */
    return strncmp(device_sn, WIND_LOCK_SN_PREFIX, WIND_LOCK_SN_PREFIX_LEN) == 0;
}

int app_tuya_bridge_device_count(void)
{
    int count = 0;
    map_lock();
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use) count++;
    }
    map_unlock();
    return count;
}

int app_tuya_bridge_find_device(const char *device_sn)
{
    if (device_sn == NULL) return -1;

    int found = -1;
    map_lock();
    for (int i = 0; i < MAX_TUYA_DEVICES; i++) {
        if (s_devices[i].in_use && strcmp(s_devices[i].device_sn, device_sn) == 0) {
            found = i;
            break;
        }
    }
    map_unlock();
    return found;
}
