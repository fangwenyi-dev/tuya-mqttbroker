/**
 * @file tuya_main.c
 * @brief 智能开窗器 LoRa 网关桥接器 - TuyaOpen 主入口
 *
 * 替代 Matter 方案的 ESP-IDF main.cpp，使用 TuyaOpen SDK 接入涂鸦云。
 *
 * 架构概述：
 * 1. TuyaOpen SDK 管理 WiFi 配网、涂鸦云连接、DP 收发
 * 2. mosquitto MQTT Broker 接收 LoRa 网关的 $SH 协议消息
 * 3. 协议桥接层（app_protocol_bridge）处理 $SH ↔ 涂鸦 DP 双向转换
 * 4. DP 桥接层（app_tuya_bridge）管理设备-DP 映射，支持内倒按钮和风锁模式
 *
 * 相比 Matter 方案的优势：
 * - 无需 StackLock（TuyaOpen SDK 内部管理线程安全）
 * - 无需端点创建延迟（涂鸦 DP 是虚拟的，无 HomeKit ReportData 大小限制）
 * - BLE+AP 配网（涂鸦原生方式，比 Matter 配网更成熟）
 * - 内置 LAN 控制（涂鸦 SDK 自带，局域网内可控制设备）
 * - 支持 OTA（涂鸦 SDK 自带，无需手动实现）
 * - 内存占用更小（无 Matter 协议栈，无 ESP-Matter 库）
 *
 * DP 分配方案（每个开窗器 4 个 DP）：
 *   设备 N (0-indexed) 的 DP ID:
 *     位置状态:  N*4 + 1  (Value, 0-100)
 *     控制命令:  N*4 + 2  (Enum: 0=停止 1=打开 2=关闭 3=内倒)
 *     电池百分比: N*4 + 3  (Value, 0-100)
 *     风锁模式:  N*4 + 4  (Enum: 0=内倒模式 1=平开模式)
 *
 * @copyright Copyright (c) 2024-2025. All Rights Reserved.
 */

#include "cJSON.h"
#include "netmgr.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "tal_cli.h"
#include "tuya_authorize.h"
#include "reset_netcfg.h"
#include "tkl_wifi.h"
#include <assert.h>

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

/* 应用模块 */
#include "app_tuya_bridge.h"
#include "app_protocol_bridge.h"
#include "app_led.h"
#include "app_button.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

/* ==================== GPIO 定义 ==================== */

/* LED GPIO（根据实际硬件修改） */
#define LED_GPIO_NUM        48      /* WS2812 或普通 LED 数据线 */

/* 按键 GPIO */
#define BUTTON_GPIO_NUM     0       /* BOOT 按键（GPIO0，低电平有效） */

/* ==================== 全局变量 ==================== */

/* TuyaOpen IoT 客户端（全局，供 app_protocol_bridge_reset_tuya 访问） */
tuya_iot_client_t client;

/* TuyaOpen 授权信息 */
tuya_iot_license_t license;

/* 系统监控 task 句柄 */
static THREAD_HANDLE s_monitor_thread = NULL;
static volatile bool s_monitor_running = false;

/* WiFi 连接状态 */
static volatile bool s_wifi_connected = false;

/* ==================== 用户日志输出回调 ==================== */

void user_log_output_cb(const char *str)
{
    tkl_log_output(str);
}

/* ==================== OTA 升级通知回调 ==================== */

void user_upgrade_notify_on(tuya_iot_client_t *client, cJSON *upgrade)
{
    PR_INFO("===== OTA 升级通知 =====");
    if (!upgrade) {
        PR_WARN("upgrade JSON is NULL");
        return;
    }

    cJSON *version_item = cJSON_GetObjectItem(upgrade, "version");
    cJSON *size_item    = cJSON_GetObjectItem(upgrade, "size");
    cJSON *type_item    = cJSON_GetObjectItem(upgrade, "type");

    PR_INFO("版本: %s", cJSON_IsString(version_item) ? version_item->valuestring : "N/A");
    PR_INFO("大小: %s", cJSON_IsString(size_item) ? size_item->valuestring : "N/A");
    PR_INFO("通道: %d", cJSON_IsNumber(type_item) ? type_item->valueint : -1);
}

/* ==================== TuyaOpen 事件处理器 ==================== */

/**
 * @brief 用户事件处理器
 *
 * 处理涂鸦云事件：配网、DP 接收、重置、OTA 等。
 *
 * 关键事件：
 * - TUYA_EVENT_BIND_START: 配网开始
 * - TUYA_EVENT_DIRECT_MQTT_CONNECTED: 涂鸦云连接成功
 * - TUYA_EVENT_DP_RECEIVE_OBJ: 收到 DP 下发（App 控制命令）
 * - TUYA_EVENT_RESET: 重置请求
 * - TUYA_EVENT_RESET_COMPLETE: 重置完成
 * - TUYA_EVENT_UPGRADE_NOTIFY: OTA 升级通知
 */
void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("配网开始");
        /* 蓝灯快闪：配网模式 */
        app_led_set(LED_BLUE, LED_MODE_FAST_BLINK, 0);
        break;

    case TUYA_EVENT_DIRECT_MQTT_CONNECTED:
        PR_INFO("涂鸦云连接成功");
        s_wifi_connected = true;
        /* 蓝灯常亮：已连接涂鸦云 */
        app_led_set(LED_BLUE, LED_MODE_ON, 0);
        /* 通知协议桥接层 WiFi 已连接，启动本地 MQTT 客户端 */
        app_protocol_bridge_on_wifi_connected();
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        /* 收到 DP 下发命令（App → 设备） */
        dp_obj_recv_t *dpobj = event->value.dpobj;
        PR_DEBUG("收到 DP: cmd_tp=%d dtt_tp=%d count=%u",
                 dpobj->cmd_tp, dpobj->dtt_tp, dpobj->dpscnt);

        /* 交给 DP 桥接层处理（解析控制命令、内倒、风锁模式） */
        app_tuya_bridge_handle_dp_recv(dpobj);
        break;
    }

    case TUYA_EVENT_UPGRADE_NOTIFY:
        user_upgrade_notify_on(client, event->value.asJSON);
        break;

    case TUYA_EVENT_RESET: {
        tuya_reset_type_t reset_type = (tuya_reset_type_t)event->value.asInteger;
        PR_INFO("设备重置: type=%d", reset_type);
        /* 清理本地状态 */
        app_tuya_bridge_remove_all_devices();
        break;
    }

    case TUYA_EVENT_RESET_COMPLETE:
        PR_INFO("重置完成，重启设备");
        tal_system_reset();
        break;

    default:
        break;
    }
}

/* ==================== 网络检查回调 ==================== */

bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status == NETMGR_LINK_DOWN ? false : true;
}

/* ==================== 系统监控 Task ==================== */

/**
 * @brief 系统监控 task
 *
 * 定期检查：
 * 1. LoRa 网关离线状态（超过 15 分钟无消息标记离线）
 * 2. WiFi 连接状态（LED 指示）
 * 3. 喂狗（如有）
 */
static void system_monitor_task(void *arg)
{
    PR_INFO("系统监控 task 已启动");

    uint32_t last_check = 0;

    while (s_monitor_running) {
        uint32_t now = tal_system_gettick();

        /* 每 60 秒检查一次网关离线状态 */
        if ((now - last_check) >= 60000) {
            last_check = now;
            app_protocol_bridge_check_gateway_offline();

            /* 打印设备数量 */
            int dev_count = app_tuya_bridge_device_count();
            PR_DEBUG("系统监控: 已注册设备=%d, WiFi=%s, 云连接=%s",
                     dev_count,
                     s_wifi_connected ? "已连接" : "未连接",
                     s_wifi_connected ? "已连接" : "未连接");
        }

        /* WiFi 断开时蓝灯慢闪 */
        if (!s_wifi_connected) {
            app_led_set(LED_BLUE, LED_MODE_SLOW_BLINK, 0);
        }

        tal_system_sleep(1000);
    }

    PR_INFO("系统监控 task 退出");
    s_monitor_thread = NULL;
    tal_thread_delete(NULL);
}

/* ==================== 用户主函数 ==================== */

void user_main(void)
{
    int rt = OPRT_OK;

    /* 1. TuyaOpen SDK 运行时初始化 */
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("========================================");
    PR_NOTICE("智能开窗器 LoRa 网关桥接器 (TuyaOpen)");
    PR_NOTICE("========================================");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);
    PR_NOTICE("PID:                 %s", TUYA_PRODUCT_ID);
    PR_NOTICE("========================================");

    /* 2. TAL 模块初始化 */
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    /* 3. CLI 和授权初始化 */
#if !defined(PLATFORM_UBUNTU) || (PLATFORM_UBUNTU == 0)
    tal_cli_init();
    tuya_authorize_init();
#endif

    /* 4. 重置配网检查 */
    reset_netconfig_start();

    /* 5. 读取授权信息 */
    if (OPRT_OK != tuya_authorize_read(&license)) {
        license.uuid    = TUYA_OPENSDK_UUID;
        license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("未找到授权信息，使用 tuya_config.h 中的 UUID/AuthKey");
        PR_WARN("请替换 TUYA_OPENSDK_UUID 和 TUYA_OPENSDK_AUTHKEY");
        PR_WARN("获取授权码: https://platform.tuya.com/purchase/index?type=6");
    }

    /* 6. 初始化 TuyaOpen IoT 客户端 */
    rt = tuya_iot_init(&client, &(const tuya_iot_config_t){
        .software_ver  = PROJECT_VERSION,
        .productkey    = TUYA_PRODUCT_ID,
        .uuid          = license.uuid,
        .authkey       = license.authkey,
        .event_handler = user_event_handler_on,
        .network_check = user_network_check,
    });
    assert(rt == OPRT_OK);
    PR_INFO("TuyaOpen IoT 客户端初始化成功");

    /* 7. 初始化 LwIP（WiFi/MQTT 需要） */
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    /* 8. 网络管理初始化 */
    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
    netmgr_init(type);

    /* 9. 配置 BLE+AP 配网（涂鸦原生配网方式） */
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                    &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
#endif

    /* 10. 初始化应用模块 */
    /* LED 指示灯（蓝灯慢闪：等待配网） */
    app_led_init(LED_GPIO_NUM);
    app_led_set(LED_BLUE, LED_MODE_SLOW_BLINK, 0);

    /* 涂鸦 DP 桥接层 */
    app_tuya_bridge_init(&client);

    /* 获取设备 MAC 地址作为 bridge_sn（用于 $SH 001 响应的 uuid 字段）
     * 注意：tkl_wifi_get_mac 需要两个参数：WF_IF_E 接口类型 + NW_MAC_S 指针 */
    char bridge_sn[32] = {0};
    NW_MAC_S mac = {0};
    if (tkl_wifi_get_mac(WF_STATION, &mac) == OPRT_OK) {
        snprintf(bridge_sn, sizeof(bridge_sn), "%02X%02X%02X%02X%02X%02X",
                 mac.mac[0], mac.mac[1], mac.mac[2],
                 mac.mac[3], mac.mac[4], mac.mac[5]);
    } else {
        PR_WARN("获取 MAC 地址失败，使用默认 bridge_sn");
        strncpy(bridge_sn, "tuya_broker", sizeof(bridge_sn) - 1);
    }
    PR_INFO("bridge_sn = %s", bridge_sn);

    protocol_bridge_config_t bridge_config = {
        .bridge_sn = bridge_sn,
    };
    app_protocol_bridge_init(&bridge_config);

    /* 按键模块 */
    app_button_init(BUTTON_GPIO_NUM);

    /* 11. 启动协议桥接层（mosquitto broker + bridge task） */
    app_protocol_bridge_start();

    /* 12. 启动系统监控 task */
    s_monitor_running = true;
    THREAD_CFG_T monitor_param = {0};
    monitor_param.stackDepth = 1024 * 4;
    monitor_param.priority = THREAD_PRIO_1;
    monitor_param.thrdname = "sys_monitor";
    tal_thread_create_and_start(&s_monitor_thread, NULL, NULL,
                                 system_monitor_task, NULL, &monitor_param);

    /* 13. 启动 TuyaOpen IoT task（必须在 reset_netconfig_check 之前） */
    PR_INFO("应用初始化完成，启动 TuyaOpen IoT 主循环");
    tuya_iot_start(&client);

    /* 14. 检查重置配网（必须在 tuya_iot_start 之后调用） */
    reset_netconfig_check();

    for (;;) {
        /* 主循环：接收涂鸦云数据包，处理 keepalive */
        tuya_iot_yield(&client);
    }
}

/* ==================== 平台入口 ==================== */

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* TuyaOpen 应用线程句柄 */
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief TuyaOpen 应用入口（由 TuyaOpen SDK 启动时调用）
 */
void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth   = 1024 * 16;  /* 16KB 栈（mosquitto + cJSON 需要较大栈） */
    thrd_param.priority     = THREAD_PRIO_1;
    thrd_param.thrdname     = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
