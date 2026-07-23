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
#include "tal_memory.h"
#include "tkl_output.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "tal_cli.h"
#include "tuya_authorize.h"
#include "reset_netcfg.h"
#include "tkl_wifi.h"
#include <assert.h>

/* ESP-IDF mDNS（用于 matter-broker.local 局域网寻址） */
#if !defined(PLATFORM_UBUNTU) || (PLATFORM_UBUNTU == 0)
#include "mdns.h"
#endif

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

/* ==================== mDNS 配置 ==================== */

/* mDNS 主机名（与 Matter 方案统一，LoRa 网关可用 matter-broker.local 寻址） */
#define MDNS_HOSTNAME       "matter-broker"
#define MDNS_MQTT_SERVICE   "_mqtt"
#define MDNS_MQTT_PROTO     "_tcp"
#define MDNS_MQTT_PORT      1883

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

/* mDNS 初始化标志（确保仅初始化一次） */
static bool s_mdns_initialized = false;

/* ==================== KV Seed/Key 派生 ==================== */

/**
 * @brief 从 MAC 地址派生 KV 加密 seed 和 key
 *
 * 原方案使用硬编码 seed/key，所有设备相同，存在安全隐患。
 * 改为从设备唯一 MAC 地址派生，确保每台设备 KV 加密密钥不同。
 * 注意：更改 seed/key 后，已有 KV 数据将无法读取（需重新配网）。*/
static void derive_kv_from_mac(const NW_MAC_S *mac, char *seed, char *key, size_t buf_size)
{
    if (mac == NULL || seed == NULL || key == NULL || buf_size < 17) return;

    uint8_t m[6];
    memcpy(m, mac->mac, 6);

    /* seed: "ty" + MAC 正序 hex + XOR校验 = 2 + 14 = 16 字符 */
    uint8_t xor_all = m[0] ^ m[1] ^ m[2] ^ m[3] ^ m[4] ^ m[5];
    snprintf(seed, buf_size, "ty%02X%02X%02X%02X%02X%02X%02X",
             m[0], m[1], m[2], m[3], m[4], m[5], xor_all);

    /* key: "br" + MAC 逆序 hex + 异或校验 = 2 + 14 = 16 字符 */
    uint8_t xor_tail = m[2] ^ m[3] ^ m[4] ^ m[5];
    snprintf(key, buf_size, "br%02X%02X%02X%02X%02X%02X%02X",
             m[5], m[4], m[3], m[2], m[1], m[0], xor_tail);
}

/* ==================== mDNS 服务初始化 ==================== */

/**
 * @brief 初始化 mDNS 服务，注册 hostname 和 MQTT broker 服务
 *
 * 使 LoRa 网关可通过 matter-broker.local:1883 寻址 ESP32 上的 MQTT Broker，
 * 与 Matter 方案统一，无需硬编码 IP 地址。
 * 仅在 WiFi 连接成功后调用一次（由 TUYA_EVENT_DIRECT_MQTT_CONNECTED 触发）。
 */
static void setup_mdns_service(void)
{
    if (s_mdns_initialized) {
        return;
    }

#if !defined(PLATFORM_UBUNTU) || (PLATFORM_UBUNTU == 0)
    /* 初始化 mDNS（如果 SDK 已初始化，mdns_init 返回 ESP_ERR_INVALID_STATE，忽略即可） */
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        PR_DEBUG("mDNS init 返回 %d（可能已由 SDK 初始化，继续设置 hostname）", ret);
    }

    /* 设置主机名为 matter-broker */
    ret = mdns_hostname_set(MDNS_HOSTNAME);
    if (ret != ESP_OK) {
        PR_WARN("mDNS 设置主机名失败: %d", ret);
        return;
    }

    /* 注册 MQTT broker 服务，使网关可通过 _mqtt._tcp 服务发现 */
    ret = mdns_service_add(NULL, MDNS_MQTT_SERVICE, MDNS_MQTT_PROTO,
                           MDNS_MQTT_PORT, NULL, 0);
    if (ret != ESP_OK) {
        PR_WARN("mDNS 添加 MQTT 服务失败: %d", ret);
    }

    PR_INFO("mDNS 已启动: %s.local | %s.%s @ port %d",
            MDNS_HOSTNAME, MDNS_MQTT_SERVICE, MDNS_MQTT_PROTO, MDNS_MQTT_PORT);
    s_mdns_initialized = true;
#else
    PR_INFO("mDNS 在 Ubuntu 平台不可用，跳过");
    s_mdns_initialized = true;
#endif
}

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
    cJSON *url_item     = cJSON_GetObjectItem(upgrade, "url");
    cJSON *md5_item     = cJSON_GetObjectItem(upgrade, "md5");

    PR_INFO("版本: %s", cJSON_IsString(version_item) ? version_item->valuestring : "N/A");
    PR_INFO("大小: %s bytes", cJSON_IsString(size_item) ? size_item->valuestring : "N/A");
    PR_INFO("通道: %d (%s)", cJSON_IsNumber(type_item) ? type_item->valueint : -1,
            (cJSON_IsNumber(type_item) && type_item->valueint == 0) ? "固件" : "模块");
    PR_INFO("URL:  %s", cJSON_IsString(url_item) ? url_item->valuestring : "N/A");
    PR_INFO("MD5:  %s", cJSON_IsString(md5_item) ? md5_item->valuestring : "N/A");

    /* 红灯快闪表示 OTA 进行中 */
    app_led_set(LED_RED, LED_MODE_FAST_BLINK, 0);
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
        /* 初始化 mDNS 服务（仅一次），使 LoRa 网关可通过 matter-broker.local 寻址 */
        setup_mdns_service();
        /* 通知协议桥接层 WiFi 已连接，启动本地 MQTT 客户端 */
        app_protocol_bridge_on_wifi_connected();
        break;

    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_WARN("涂鸦云连接断开");
        s_wifi_connected = false;
        /* 蓝灯慢闪：等待重连 */
        app_led_set(LED_BLUE, LED_MODE_SLOW_BLINK, 0);
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
    uint32_t last_sys_publish = 0;
    uint32_t last_heap_check = 0;
    uint32_t boot_time = (uint32_t)tal_system_get_millisecond();

    while (s_monitor_running) {
        uint32_t now = (uint32_t)tal_system_get_millisecond();

        /* 每 60 秒检查一次网关离线状态 */
        if ((now - last_check) >= 60000) {
            last_check = now;
            app_protocol_bridge_check_gateway_offline();

            /* 打印设备数量和内存状态 */
            int dev_count = app_tuya_bridge_device_count();
            int free_heap = tal_system_get_free_heap_size();
            uint32_t uptime_s = (now - boot_time) / 1000;
            PR_DEBUG("系统监控: uptime=%lus 设备=%d 内存=%dB 云连接=%s",
                     (unsigned long)uptime_s, dev_count, free_heap,
                     s_wifi_connected ? "已连接" : "未连接");
        }

        /* 每 30 秒发布 $SYS 监控主题 */
        if ((now - last_sys_publish) >= 30000) {
            last_sys_publish = now;
            app_protocol_bridge_publish_sys_stats();
        }

        /* 每 10 秒检查内存，低于阈值时告警 */
        if ((now - last_heap_check) >= 10000) {
            last_heap_check = now;
            int free_heap = tal_system_get_free_heap_size();
            if (free_heap < 1024 * 10) {
                PR_ERR("⚠ 可用内存过低: %d bytes (< 10KB)，存在 OOM 风险", free_heap);
            }
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
    /* 从 MAC 地址派生唯一的 KV seed/key（避免所有设备使用相同密钥）*/
    NW_MAC_S kv_mac = {0};
    tal_kv_cfg_t kv_cfg = {0};
    if (tkl_wifi_get_mac(WF_STATION, &kv_mac) == OPRT_OK) {
        char kv_seed[17] = {0};
        char kv_key[17]  = {0};
        derive_kv_from_mac(&kv_mac, kv_seed, kv_key, sizeof(kv_seed));
        snprintf(kv_cfg.seed, sizeof(kv_cfg.seed), "%s", kv_seed);
        snprintf(kv_cfg.key, sizeof(kv_cfg.key), "%s", kv_key);
    } else {
        PR_WARN("获取 MAC 失败，KV 使用回退密钥（不推荐）");
        snprintf(kv_cfg.seed, sizeof(kv_cfg.seed), "%s", "vmlkasdh93dlvlcy");
        snprintf(kv_cfg.key, sizeof(kv_cfg.key), "%s", "dflfuap134ddlduq");
    }
    PR_INFO("KV seed/key 已从 MAC 地址派生");
    tal_kv_init(&kv_cfg);
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
        .enable_broker_auth = true,  /* 启用 Broker 用户名/密码认证 */
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
