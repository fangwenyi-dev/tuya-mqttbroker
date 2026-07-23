/**
 * @file app_protocol_bridge.c
 * @brief $SH MQTT 协议 ↔ 涂鸦 DP 桥接层实现
 *
 * 完整 $SH 协议处理，将 LoRa 网关上报的状态转换为涂鸦 DP 上报，
 * 将涂鸦 App 下发的 DP 控制命令转换为 $SH 004 控制命令。
 *
 * 完全基于 TuyaOpen SDK API：
 * 1. MQTT 客户端使用 TuyaOpen libmqtt（mqtt_client_interface.h）
 * 2. 线程/互斥锁使用 TAL API（tal_thread、tal_mutex）
 * 3. 日志使用 PR_* 宏（tal_log.h）
 * 4. 无 ESP-IDF 依赖（无 esp_mqtt_client、无 esp_event、无 esp_err_t）
 *
 * 架构：
 * - mosquitto broker（本地 1883 端口）接收 LoRa 网关消息，通过回调推入队列
 * - TuyaOpen MQTT 客户端连接到本地 broker，用于发布 $SH 命令
 * - bridge_task 处理消息队列，分发到 001/002/003/005 处理函数
 */

#include "app_protocol_bridge.h"
#include "app_tuya_bridge.h"
#include "app_led.h"
#include "tal_api.h"
#include "tal_log.h"
#include "tkl_output.h"
#include "cJSON.h"
#include "mqtt_client_interface.h"
#include "mosq_broker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* TuyaOpen SDK 头文件 */
#include "tuya_iot.h"
#include "netmgr.h"

/* ==================== 配置常量 ==================== */

#ifndef MQTT_MSG_QUEUE_SIZE
#define MQTT_MSG_QUEUE_SIZE     10
#endif

/* MQTT Broker 连接速率限制（防止 DoS）*/
#define MAX_CONNECTS_PER_MINUTE  30

/* MQTT 消息大小限制（字节）*/
#define MQTT_MAX_PAYLOAD_SIZE    8192

/* MQTT 发布重试次数 */
#define MQTT_PUBLISH_RETRY       2
#define MQTT_PUBLISH_RETRY_DELAY 100

/* $SYS 监控主题前缀 */
#define SYS_TOPIC_PREFIX         "$SYS/broker"

/* cJSON 解析最大深度（防止恶意深层嵌套）*/
#define CJSON_MAX_DEPTH          20

/* $SH 协议常量（参考 huijian-gateway const.py） */
#define PROTOCOL_HEAD               "$SH"
#define TOPIC_GATEWAY_REQ_FMT       "gateway/%s/req"
#define TOPIC_GATEWAY_RSP           "gateway/rpt_rsp"
#define ATTRIBUTE_W_TRAVEL          "w_travel"
#define ATTRIBUTE_WIND_LOCK_MODE    "rwp_wind_lock_mode"
#define MAX_COMMAND_ID              999999
#define DEVICE_SN_PREFIX_GATEWAY    "1001"

/* 003 操作追踪 */
#define MAX_PENDING_003_OPS         4
#define PENDING_003_TIMEOUT_SEC     120

/* 电池电压原始值有效范围（12V 锂电池：正常 9.5V-12.6V，放宽 8V-14V） */
#define BATTERY_RAW_MIN             80
#define BATTERY_RAW_MAX             140

/* Task 配置 */
#define BRIDGE_TASK_PRIORITY        THREAD_PRIO_2
#define BRIDGE_TASK_STACK_SIZE      (1024 * 12)

/* MQTT 客户端重连间隔 */
#define MQTT_RECONNECT_DELAY_MS    3000
/* MQTT 客户端 yield 超时 */
#define MQTT_YIELD_TIMEOUT_MS      1000

/* ==================== 消息结构 ==================== */

typedef struct {
    char client_id[64];
    char topic[128];
    char data[4096];
    int  data_len;
    int  qos;
    int  retain;
} mqtt_message_t;

/* ==================== 内部状态 ==================== */

static const char *TAG = "proto_bridge";

static char s_bridge_sn[32] = {0};

/* MQTT 消息队列（来自 mosquitto broker 的回调推入） */
static QUEUE_HANDLE s_mqtt_msg_queue = NULL;

/* 本地 MQTT 客户端（连接到本地 broker，用于发布 $SH 命令） */
static void *s_mqtt_client = NULL;  /* TuyaOpen mqtt_client 句柄 */
static volatile bool s_mqtt_client_connected = false;
static volatile bool s_mqtt_client_should_run = false;
static MUTEX_HANDLE s_mqtt_client_mutex = NULL;
static THREAD_HANDLE s_mqtt_client_thread = NULL;

/* 命令 ID 生成器 */
static int s_command_id = 1;
static MUTEX_HANDLE s_command_id_mutex = NULL;

/* 003 操作追踪 */
typedef struct {
    int command_id;
    int bind;
    uint32_t timestamp_ms;
    bool in_use;
} pending_003_op_t;

static pending_003_op_t s_pending_003_ops[MAX_PENDING_003_OPS] = {0};
static MUTEX_HANDLE s_pending_003_mutex = NULL;

/* 多网关管理 */
typedef struct {
    char gateway_sn[32];
    bool online;
    uint32_t last_seen_ms;
    bool in_use;
} gateway_entry_t;

static gateway_entry_t s_gateways[MAX_GATEWAYS] = {0};
static MUTEX_HANDLE s_gateways_mutex = NULL;

/* 设备→网关映射 */
typedef struct {
    char device_sn[32];
    char gateway_sn[32];
    uint16_t voltage_mv;
    uint8_t  state;
    uint32_t add_seq;
    bool in_use;
} device_gateway_entry_t;

static device_gateway_entry_t s_device_gateway_map[MAX_BRIDGED_DEVICES] = {0};
static uint32_t s_add_seq_counter = 0;
static MUTEX_HANDLE s_device_map_mutex = NULL;

/* Bridge task */
static THREAD_HANDLE s_bridge_thread = NULL;
static volatile bool s_bridge_running = false;

/* Broker task（mosq_broker_run 是阻塞函数，需独立 task） */
static THREAD_HANDLE s_broker_thread = NULL;
static volatile bool s_broker_running = false;

/* 系统启动时间（用于 $SYS 上报运行时间）*/
static uint32_t s_boot_time_ms = 0;

/* Broker 连接速率限制状态 */
static uint32_t s_connect_window_start_ms = 0;
static int s_connect_count_in_window = 0;

/* ==================== 互斥锁辅助 ==================== */

static void mutex_lock(MUTEX_HANDLE mutex)
{
    if (mutex) tal_mutex_lock(mutex);
}

static void mutex_unlock(MUTEX_HANDLE mutex)
{
    if (mutex) tal_mutex_unlock(mutex);
}

/* ==================== 辅助函数 ==================== */

/**
 * @brief 从 cJSON 字段解析数值（兼容字符串和数字类型）
 *
 * 网关上报的 battery/voltage 字段可能是数字或字符串。
 * 参考 HA 集成 mqtt_handler.py 用 float() 兼容两种。
 */
static int parse_number_field(cJSON *obj)
{
    if (obj == NULL) return -1;
    if (cJSON_IsNumber(obj)) return obj->valueint;
    if (cJSON_IsString(obj)) {
        const char *start = obj->valuestring;
        char *endptr = NULL;
        errno = 0;
        long val = strtol(start, &endptr, 10);
        if (endptr == start) return -1;
        if (errno == ERANGE) return -1;
        return (int)val;
    }
    return -1;
}

static int next_command_id(void)
{
    int id;
    mutex_lock(s_command_id_mutex);
    id = s_command_id++;
    if (s_command_id > MAX_COMMAND_ID) s_command_id = 1;
    mutex_unlock(s_command_id_mutex);
    return id;
}

/* ==================== 003 操作追踪 ==================== */

static void record_pending_003(int command_id, int bind)
{
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    mutex_lock(s_pending_003_mutex);
    int free_slot = -1;
    for (int i = 0; i < MAX_PENDING_003_OPS; i++) {
        if (s_pending_003_ops[i].in_use && s_pending_003_ops[i].command_id == command_id) {
            s_pending_003_ops[i].bind = bind;
            s_pending_003_ops[i].timestamp_ms = now;
            mutex_unlock(s_pending_003_mutex);
            return;
        }
        if (!s_pending_003_ops[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        s_pending_003_ops[free_slot].command_id = command_id;
        s_pending_003_ops[free_slot].bind = bind;
        s_pending_003_ops[free_slot].timestamp_ms = now;
        s_pending_003_ops[free_slot].in_use = true;
    }
    /* 顺便清理过期记录 */
    for (int i = 0; i < MAX_PENDING_003_OPS; i++) {
        if (s_pending_003_ops[i].in_use &&
            (now - s_pending_003_ops[i].timestamp_ms) > (uint32_t)(PENDING_003_TIMEOUT_SEC * 1000)) {
            s_pending_003_ops[i].in_use = false;
        }
    }
    mutex_unlock(s_pending_003_mutex);
}

static int lookup_pending_003(int command_id)
{
    int bind = -1;
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    mutex_lock(s_pending_003_mutex);
    for (int i = 0; i < MAX_PENDING_003_OPS; i++) {
        if (s_pending_003_ops[i].in_use && s_pending_003_ops[i].command_id == command_id) {
            if ((now - s_pending_003_ops[i].timestamp_ms) <= (uint32_t)(PENDING_003_TIMEOUT_SEC * 1000)) {
                bind = s_pending_003_ops[i].bind;
            }
            s_pending_003_ops[i].in_use = false;
            break;
        }
    }
    mutex_unlock(s_pending_003_mutex);
    return bind;
}

/* ==================== 网关管理 ==================== */

static void register_gateway(const char *gw_sn)
{
    bool found_existing = false;
    bool was_offline = false;
    int new_slot = -1;

    mutex_lock(s_gateways_mutex);
    for (int i = 0; i < MAX_GATEWAYS; i++) {
        if (s_gateways[i].in_use && strcmp(s_gateways[i].gateway_sn, gw_sn) == 0) {
            found_existing = true;
            was_offline = !s_gateways[i].online;
            s_gateways[i].online = true;
            s_gateways[i].last_seen_ms = (uint32_t)tal_system_get_millisecond();
            break;
        }
        if (!s_gateways[i].in_use && new_slot < 0) new_slot = i;
    }
    if (!found_existing && new_slot >= 0) {
        strncpy(s_gateways[new_slot].gateway_sn, gw_sn, sizeof(s_gateways[new_slot].gateway_sn) - 1);
        s_gateways[new_slot].gateway_sn[sizeof(s_gateways[new_slot].gateway_sn) - 1] = '\0';
        s_gateways[new_slot].online = true;
        s_gateways[new_slot].last_seen_ms = (uint32_t)tal_system_get_millisecond();
        s_gateways[new_slot].in_use = true;
    }
    mutex_unlock(s_gateways_mutex);

    if (found_existing) {
        if (was_offline) {
            PR_INFO("LoRa 网关恢复在线: %s", gw_sn);
        }
        return;
    }

    if (new_slot < 0) {
        PR_WARN("网关表已满，无法注册 %s", gw_sn);
        return;
    }

    PR_INFO("注册 LoRa 网关: %s", gw_sn);
}

static void register_device_gateway(const char *device_sn, const char *gw_sn)
{
    bool table_full = false;
    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use &&
            strcmp(s_device_gateway_map[i].device_sn, device_sn) == 0) {
            strncpy(s_device_gateway_map[i].gateway_sn, gw_sn,
                    sizeof(s_device_gateway_map[i].gateway_sn) - 1);
            s_device_gateway_map[i].gateway_sn[sizeof(s_device_gateway_map[i].gateway_sn) - 1] = '\0';
            mutex_unlock(s_device_map_mutex);
            return;
        }
    }
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (!s_device_gateway_map[i].in_use) {
            strncpy(s_device_gateway_map[i].device_sn, device_sn,
                    sizeof(s_device_gateway_map[i].device_sn) - 1);
            s_device_gateway_map[i].device_sn[sizeof(s_device_gateway_map[i].device_sn) - 1] = '\0';
            strncpy(s_device_gateway_map[i].gateway_sn, gw_sn,
                    sizeof(s_device_gateway_map[i].gateway_sn) - 1);
            s_device_gateway_map[i].gateway_sn[sizeof(s_device_gateway_map[i].gateway_sn) - 1] = '\0';
            s_device_gateway_map[i].add_seq = ++s_add_seq_counter;
            s_device_gateway_map[i].in_use = true;
            mutex_unlock(s_device_map_mutex);
            return;
        }
    }
    table_full = true;
    mutex_unlock(s_device_map_mutex);
    if (table_full) {
        PR_WARN("设备→网关映射表已满，无法注册 dev=%s gw=%s", device_sn, gw_sn);
    }
}

static bool find_gateway_for_device(const char *device_sn, char *gw_sn_buf, size_t buf_size)
{
    if (gw_sn_buf == NULL || buf_size == 0) return false;
    gw_sn_buf[0] = '\0';
    bool found = false;
    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use &&
            strcmp(s_device_gateway_map[i].device_sn, device_sn) == 0) {
            strncpy(gw_sn_buf, s_device_gateway_map[i].gateway_sn, buf_size - 1);
            gw_sn_buf[buf_size - 1] = '\0';
            found = true;
            break;
        }
    }
    mutex_unlock(s_device_map_mutex);
    return found;
}

static bool is_device_in_map(const char *dev_sn)
{
    bool found = false;
    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use &&
            strcmp(s_device_gateway_map[i].device_sn, dev_sn) == 0) {
            found = true;
            break;
        }
    }
    mutex_unlock(s_device_map_mutex);
    return found;
}

static void local_remove_device_from_map(const char *dev_sn)
{
    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use &&
            strcmp(s_device_gateway_map[i].device_sn, dev_sn) == 0) {
            s_device_gateway_map[i].in_use = false;
            s_device_gateway_map[i].device_sn[0] = '\0';
            s_device_gateway_map[i].gateway_sn[0] = '\0';
            s_device_gateway_map[i].voltage_mv = 0;
            s_device_gateway_map[i].state = 0;
            s_device_gateway_map[i].add_seq = 0;
            break;
        }
    }
    mutex_unlock(s_device_map_mutex);
}

static bool should_skip_device(const char *device_sn, const char *model)
{
    if (strncmp(device_sn, DEVICE_SN_PREFIX_GATEWAY, 4) == 0) return true;
    if (model) {
        if (strstr(model, "gateway") != NULL || strstr(model, "Gateway") != NULL ||
            strstr(model, "GATEWAY") != NULL || strstr(model, "网关") != NULL) {
            return true;
        }
    }
    return false;
}

/* ==================== $SH 消息发布（TuyaOpen MQTT 客户端） ==================== */

static void publish_mqtt_json(const char *topic, const char *json_str, const char *log_label)
{
    int data_len = strlen(json_str);
    mutex_lock(s_mqtt_client_mutex);
    void *client = s_mqtt_client;
    bool connected = s_mqtt_client_connected;
    mutex_unlock(s_mqtt_client_mutex);

    if (client == NULL) {
        PR_WARN("MQTT 客户端未创建，%s丢失: topic=%s", log_label, topic);
        return;
    }

    if (!connected) {
        PR_WARN("MQTT 客户端未连接，%s可能丢失: topic=%s", log_label, topic);
        return;
    }

    /* 带重试的发布 */
    uint16_t rc = 0;
    for (int retry = 0; retry <= MQTT_PUBLISH_RETRY; retry++) {
        rc = mqtt_client_publish(client, topic, (const uint8_t *)json_str, data_len, 1);
        if (rc != 0) break;
        if (retry < MQTT_PUBLISH_RETRY) {
            PR_WARN("发布失败 (retry %d/%d)，%s: topic=%s",
                    retry + 1, MQTT_PUBLISH_RETRY, log_label, topic);
            tal_system_sleep(MQTT_PUBLISH_RETRY_DELAY);
        }
    }

    if (rc == 0) {
        PR_WARN("发布最终失败，%s丢失: topic=%s", log_label, topic);
    } else {
        PR_DEBUG("发布%s: topic=%s msgid=%d", log_label, topic, rc);
        app_led_flash(LED_GREEN);
    }
}

static void publish_sh_to_gateway(const char *gw_sn, const char *ctype, cJSON *data)
{
    if (s_mqtt_client == NULL || data == NULL || gw_sn == NULL) {
        if (data) cJSON_Delete(data);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddStringToObject(root, "head", PROTOCOL_HEAD);
    cJSON_AddStringToObject(root, "ctype", ctype);
    cJSON_AddNumberToObject(root, "id", next_command_id());
    cJSON_AddStringToObject(root, "sn", gw_sn);
    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        char topic[128];
        snprintf(topic, sizeof(topic), TOPIC_GATEWAY_REQ_FMT, gw_sn);
        publish_mqtt_json(topic, json_str, "$SH 消息");
        tal_free(json_str);
    }
    cJSON_Delete(root);
}

static void send_device_control(const char *device_sn, const char *gw_sn, const char *value)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return;
    cJSON_AddStringToObject(data, "sn", device_sn);
    cJSON_AddStringToObject(data, "attribute", ATTRIBUTE_W_TRAVEL);
    cJSON_AddStringToObject(data, "value", value);
    publish_sh_to_gateway(gw_sn, "004", data);
    PR_INFO("发送控制命令: gw=%s dev=%s value=%s", gw_sn, device_sn, value);
}

static void send_bind_command(const char *gw_sn, const char *dev_sn, int bind)
{
    if (s_mqtt_client == NULL || gw_sn == NULL || dev_sn == NULL) return;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON_AddStringToObject(root, "head", PROTOCOL_HEAD);
    cJSON_AddStringToObject(root, "ctype", "003");
    int cmd_id = next_command_id();
    record_pending_003(cmd_id, bind);
    cJSON_AddNumberToObject(root, "id", cmd_id);
    cJSON_AddStringToObject(root, "sn", gw_sn);
    cJSON_AddNumberToObject(root, "bind", bind);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(data, "bind", bind);
    cJSON_AddStringToObject(data, "devtype", "curtain_ctr");
    cJSON_AddStringToObject(data, "sn", dev_sn);
    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        char topic[128];
        snprintf(topic, sizeof(topic), TOPIC_GATEWAY_REQ_FMT, gw_sn);
        publish_mqtt_json(topic, json_str, bind ? "配对命令" : "解绑命令");
        tal_free(json_str);
    }
    cJSON_Delete(root);
}

static void send_start_pairing(const char *gw_sn)
{
    send_bind_command(gw_sn, "FFFFFFFFFFFF", 1);
}

static void send_unbind_device(const char *gw_sn, const char *dev_sn)
{
    send_bind_command(gw_sn, dev_sn, 0);
}

/* ==================== MQTT→涂鸦 DP 方向处理 ==================== */

/**
 * @brief 处理 ctype=001 网关绑定
 */
static void handle_ctype_001(const char *gw_sn, cJSON *data, int msg_id)
{
    bool is_gateway_info = (cJSON_GetObjectItem(data, "version") != NULL ||
                            cJSON_GetObjectItem(data, "model") != NULL ||
                            cJSON_GetObjectItem(data, "userid") != NULL ||
                            cJSON_GetObjectItem(data, "vesion") != NULL);

    if (is_gateway_info) {
        PR_INFO("收到网关绑定: %s", gw_sn);
        register_gateway(gw_sn);

        cJSON *resp_data = cJSON_CreateObject();
        if (resp_data) {
            cJSON_AddNumberToObject(resp_data, "errcode", 0);
            cJSON_AddStringToObject(resp_data, "uuid", s_bridge_sn);
            publish_sh_to_gateway(gw_sn, "001", resp_data);
            PR_INFO("已回复网关绑定确认: %s", gw_sn);
        }

        /* 主动请求设备列表 */
        cJSON *req_data = cJSON_CreateObject();
        if (req_data) {
            publish_sh_to_gateway(gw_sn, "002", req_data);
            PR_INFO("已请求设备列表: %s", gw_sn);
        }
    } else {
        cJSON *errcode = cJSON_GetObjectItem(data, "errcode");
        if (errcode) {
            int err_val = parse_number_field(errcode);
            if (err_val == 0) {
                PR_INFO("网关绑定成功: %s", gw_sn);
                register_gateway(gw_sn);
            } else {
                PR_WARN("网关绑定失败: %s errcode=%d", gw_sn, err_val);
            }
        } else {
            PR_INFO("收到网关纯绑定请求: %s", gw_sn);
            register_gateway(gw_sn);
            cJSON *resp_data = cJSON_CreateObject();
            if (resp_data) {
                cJSON_AddNumberToObject(resp_data, "errcode", 0);
                cJSON_AddStringToObject(resp_data, "uuid", s_bridge_sn);
                publish_sh_to_gateway(gw_sn, "001", resp_data);
            }
        }
    }
}

/**
 * @brief 处理 ctype=002 设备列表
 */
static void handle_ctype_002(const char *gw_sn, cJSON *data, int msg_id)
{
    typedef struct {
        char sn[32];
        int r_travel;
        int battery;
    } pre_parsed_device_t;

    pre_parsed_device_t parsed_devs[MAX_BRIDGED_DEVICES];
    int device_count = 0;

    cJSON *devices = cJSON_GetObjectItem(data, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON *device;
        cJSON_ArrayForEach(device, devices) {
            if (device_count >= MAX_BRIDGED_DEVICES) break;

            cJSON *sn_obj = cJSON_GetObjectItem(device, "sn");
            if (!cJSON_IsString(sn_obj)) continue;

            const char *dev_sn = sn_obj->valuestring;
            const char *model = NULL;
            cJSON *model_obj = cJSON_GetObjectItem(device, "model");
            if (cJSON_IsString(model_obj)) model = model_obj->valuestring;

            if (should_skip_device(dev_sn, model)) continue;

            strncpy(parsed_devs[device_count].sn, dev_sn, sizeof(parsed_devs[0].sn) - 1);
            parsed_devs[device_count].sn[sizeof(parsed_devs[0].sn) - 1] = '\0';

            int pos = parse_number_field(cJSON_GetObjectItem(device, "r_travel"));
            parsed_devs[device_count].r_travel = (pos >= 0 && pos <= 100) ? pos : -1;

            int volt = parse_number_field(cJSON_GetObjectItem(device, "battery"));
            if (volt >= 0 && (volt < BATTERY_RAW_MIN || volt > BATTERY_RAW_MAX)) {
                PR_WARN("设备 %s 电池电压 %d 超范围，丢弃", dev_sn, volt);
                volt = -1;
            }
            parsed_devs[device_count].battery = volt;
            device_count++;
        }
    } else if (devices != NULL) {
        PR_WARN("002 devices 字段非数组: gw=%s", gw_sn);
        cJSON *err_resp = cJSON_CreateObject();
        if (err_resp) {
            cJSON_AddNumberToObject(err_resp, "errcode", 1);
            publish_sh_to_gateway(gw_sn, "002", err_resp);
        }
        return;
    }

    /* 注册网关 */
    register_gateway(gw_sn);

    /* 逐设备处理 */
    int new_count = 0;
    for (int i = 0; i < device_count; i++) {
        const char *dev_sn = parsed_devs[i].sn;
        int init_pos = parsed_devs[i].r_travel;
        int init_voltage = parsed_devs[i].battery;

        register_device_gateway(dev_sn, gw_sn);

        /* 注册涂鸦 DP */
        int idx = app_tuya_bridge_find_device(dev_sn);
        if (idx < 0) {
            idx = app_tuya_bridge_add_device(dev_sn);
            if (idx >= 0) {
                new_count++;
                PR_INFO("新设备已注册 DP: sn=%s 索引=%d", dev_sn, idx);
            } else {
                PR_WARN("注册 DP 失败: dev=%s gw=%s", dev_sn, gw_sn);
                continue;
            }
        }

        /* 上报初始位置和电池 */
        if (init_pos >= 0) {
            app_tuya_bridge_update_position(dev_sn, (uint8_t)init_pos);
        }
        if (init_voltage >= 0) {
            /* 电压原始值 → 百分比（简单线性映射：80-140 → 0-100） */
            uint8_t percent = 0;
            if (init_voltage <= BATTERY_RAW_MIN) {
                percent = 0;
            } else if (init_voltage >= BATTERY_RAW_MAX) {
                percent = 100;
            } else {
                percent = (uint8_t)((init_voltage - BATTERY_RAW_MIN) * 100 / (BATTERY_RAW_MAX - BATTERY_RAW_MIN));
            }
            app_tuya_bridge_update_battery(dev_sn, percent);
        }
    }

    PR_INFO("设备列表处理完成: gw=%s 总计=%d 新增=%d", gw_sn, device_count, new_count);

    /* 回复 002(errcode=0) */
    cJSON *resp_data = cJSON_CreateObject();
    if (resp_data) {
        cJSON_AddNumberToObject(resp_data, "errcode", 0);
        publish_sh_to_gateway(gw_sn, "002", resp_data);
    }
}

/**
 * @brief 处理 ctype=003 设备配对/解绑响应
 */
static void handle_ctype_003(const char *gw_sn, cJSON *data, int msg_id)
{
    cJSON *errcode = cJSON_GetObjectItem(data, "errcode");
    cJSON *sn_obj = cJSON_GetObjectItem(data, "sn");
    cJSON *bind_obj = cJSON_GetObjectItem(data, "bind");

    int err_val = parse_number_field(errcode);
    if (!errcode || err_val != 0) {
        PR_WARN("配对/解绑失败: gw=%s errcode=%d", gw_sn, err_val);
        return;
    }

    if (!cJSON_IsString(sn_obj)) {
        PR_WARN("003 响应缺少 sn 字段");
        return;
    }

    const char *dev_sn = sn_obj->valuestring;

    if (strcmp(dev_sn, "FFFFFFFFFFFF") == 0) {
        PR_WARN("配对响应 sn=FFFFFFFFFFFF，等待 002/005 上报实际设备 SN");
        return;
    }

    int bind_val = parse_number_field(bind_obj);
    if (bind_val < 0) {
        bind_val = lookup_pending_003(msg_id);
        if (bind_val >= 0) {
            PR_INFO("003 响应无 bind 字段，通过命令ID查找: bind=%d (id=%d)", bind_val, msg_id);
        } else {
            char existing_gw[32] = {0};
            bool device_exists = find_gateway_for_device(dev_sn, existing_gw, sizeof(existing_gw));
            bind_val = device_exists ? 0 : 1;
            PR_WARN("003 响应无 bind 字段且无命令记录，推断为 %s (dev=%s exists=%d)",
                     bind_val == 0 ? "解绑" : "配对", dev_sn, device_exists ? 1 : 0);
        }
    }

    if (bind_val == 0) {
        /* 解绑成功：移除涂鸦 DP + 清理映射表 */
        PR_INFO("设备解绑成功: gw=%s dev=%s", gw_sn, dev_sn);
        app_tuya_bridge_remove_device(dev_sn);
        local_remove_device_from_map(dev_sn);
    } else {
        /* 配对成功：注册涂鸦 DP */
        PR_INFO("设备配对成功: gw=%s dev=%s", gw_sn, dev_sn);
        app_led_off(LED_GREEN);
        register_device_gateway(dev_sn, gw_sn);
        app_tuya_bridge_add_device(dev_sn);
    }
}

/**
 * @brief 处理 ctype=005 设备状态上报
 */
static void handle_ctype_005(const char *gw_sn, cJSON *data, int msg_id)
{
    register_gateway(gw_sn);

    cJSON *dev_sn_obj = cJSON_GetObjectItem(data, "sn");
    if (!cJSON_IsString(dev_sn_obj)) {
        PR_WARN("005 上报缺少 sn 字段");
        return;
    }

    const char *dev_sn = dev_sn_obj->valuestring;

    if (should_skip_device(dev_sn, NULL)) return;

    int position = -1;
    int voltage = -1;
    int state = -1;
    int wind_lock_mode = -1;

    /* 格式1：直接字段 */
    cJSON *position_obj = cJSON_GetObjectItem(data, "position");
    int pos_val = parse_number_field(position_obj);
    if (pos_val >= 0 && pos_val <= 100) position = pos_val;

    cJSON *r_travel_obj = cJSON_GetObjectItem(data, "r_travel");
    int rt_val = parse_number_field(r_travel_obj);
    if (rt_val >= 0 && rt_val <= 100) {
        if (position >= 0 && rt_val != pos_val) {
            PR_WARN("r_travel(%d) 覆盖 position(%d): dev=%s", rt_val, pos_val, dev_sn);
        }
        position = rt_val;
    }

    cJSON *battery_obj = cJSON_GetObjectItem(data, "battery");
    int bat_val = parse_number_field(battery_obj);
    cJSON *voltage_obj = cJSON_GetObjectItem(data, "voltage");
    int vol_val = parse_number_field(voltage_obj);

    if (vol_val >= 0 && (vol_val < BATTERY_RAW_MIN || vol_val > BATTERY_RAW_MAX)) vol_val = -1;
    if (bat_val >= 0 && (bat_val < BATTERY_RAW_MIN || bat_val > BATTERY_RAW_MAX)) bat_val = -1;
    if (bat_val >= 0 && vol_val >= 0 && bat_val != vol_val) {
        PR_WARN("battery(%d) 与 voltage(%d) 不一致，采用 voltage", bat_val, vol_val);
    }
    if (vol_val >= 0) voltage = vol_val;
    else if (bat_val >= 0) voltage = bat_val;

    cJSON *state_obj = cJSON_GetObjectItem(data, "state");
    int st_val = parse_number_field(state_obj);
    if (st_val >= 0 && st_val <= 1) state = st_val;

    /* 格式2：attrs 数组 */
    cJSON *attrs = cJSON_GetObjectItem(data, "attrs");
    if (cJSON_IsArray(attrs)) {
        cJSON *attr;
        cJSON_ArrayForEach(attr, attrs) {
            cJSON *attr_name = cJSON_GetObjectItem(attr, "attribute");
            cJSON *attr_value = cJSON_GetObjectItem(attr, "value");
            if (cJSON_IsString(attr_name) && attr_value) {
                const char *name = attr_name->valuestring;
                int val_int = parse_number_field(attr_value);

                if (strcmp(name, "r_travel") == 0) {
                    if (val_int >= 0 && val_int <= 100) position = val_int;
                } else if (strcmp(name, "voltage") == 0) {
                    if (val_int >= BATTERY_RAW_MIN && val_int <= BATTERY_RAW_MAX) voltage = val_int;
                } else if (strcmp(name, "state") == 0) {
                    if (val_int >= 0 && val_int <= 1) state = val_int;
                } else if (strcmp(name, ATTRIBUTE_WIND_LOCK_MODE) == 0) {
                    /* 风锁模式上报（参考 HA 集成 mqtt_handler.py L1233-1237）
                     * 0=内倒模式, 1=平开模式 */
                    if (val_int == 0 || val_int == 1) {
                        wind_lock_mode = val_int;
                        PR_INFO("设备 %s 风锁模式上报: %s (值=%d)", dev_sn,
                                val_int == 0 ? "内倒模式" : "平开模式", val_int);
                    }
                }
            }
        }
    }

    /* 记录设备→网关映射 */
    register_device_gateway(dev_sn, gw_sn);

    /* 缓存 voltage 和 state */
    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use &&
            strcmp(s_device_gateway_map[i].device_sn, dev_sn) == 0) {
            if (voltage >= 0) s_device_gateway_map[i].voltage_mv = (uint16_t)(voltage * 100);
            if (state >= 0) s_device_gateway_map[i].state = (uint8_t)state;
            break;
        }
    }
    mutex_unlock(s_device_map_mutex);

    PR_INFO("状态上报: gw=%s dev=%s pos=%d volt=%d state=%d wind_lock=%d",
            gw_sn, dev_sn, position, voltage, state, wind_lock_mode);

    /* 确保设备已注册涂鸦 DP */
    int idx = app_tuya_bridge_find_device(dev_sn);
    if (idx < 0) {
        PR_WARN("收到未知设备状态: dev=%s，尝试注册 DP", dev_sn);
        idx = app_tuya_bridge_add_device(dev_sn);
        if (idx < 0) {
            PR_WARN("注册 DP 失败: dev=%s（设备表可能已满）", dev_sn);
            return;
        }
    }

    /* 涂鸦 DP 上报 */
    if (position >= 0 && position <= 100) {
        app_tuya_bridge_update_position(dev_sn, (uint8_t)position);
    }
    if (voltage >= 0) {
        /* 电压原始值 → 百分比 */
        uint8_t percent = 0;
        if (voltage <= BATTERY_RAW_MIN) percent = 0;
        else if (voltage >= BATTERY_RAW_MAX) percent = 100;
        else percent = (uint8_t)((voltage - BATTERY_RAW_MIN) * 100 / (BATTERY_RAW_MAX - BATTERY_RAW_MIN));
        app_tuya_bridge_update_battery(dev_sn, percent);
    }
    /* 风锁模式 DP 上报 */
    if (wind_lock_mode >= 0) {
        app_tuya_bridge_update_wind_lock(dev_sn, (uint8_t)wind_lock_mode);
    }
}

/* ==================== MQTT 消息解析与分发 ==================== */

static void handle_mqtt_message(const mqtt_message_t *msg)
{
    if (msg->data_len <= 0) return;

    /* 过滤自身发布的消息 */
    if (s_bridge_sn[0] != '\0' && strcmp(msg->client_id, s_bridge_sn) == 0) {
        return;
    }

    PR_DEBUG("收到 MQTT: topic=%s data=%.*s", msg->topic, msg->data_len, msg->data);
    app_led_flash(LED_GREEN);

    cJSON *root = cJSON_ParseWithLength(msg->data, msg->data_len);
    if (root == NULL) {
        PR_WARN("JSON 解析失败 (len=%d)", msg->data_len);
        return;
    }

    /* cJSON 深度检查（防止恶意深层嵌套导致栈溢出）*/
    cJSON *depth_check = root;
    int depth = 0;
    while (depth_check && depth < CJSON_MAX_DEPTH + 1) {
        depth_check = depth_check->child;
        depth++;
    }
    if (depth > CJSON_MAX_DEPTH) {
        PR_WARN("JSON 嵌套深度 %d 超限 (%d)，丢弃", depth, CJSON_MAX_DEPTH);
        cJSON_Delete(root);
        return;
    }

    cJSON *head = cJSON_GetObjectItem(root, "head");
    if (!cJSON_IsString(head) || strcmp(head->valuestring, PROTOCOL_HEAD) != 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *ctype_obj = cJSON_GetObjectItem(root, "ctype");
    cJSON *sn_obj = cJSON_GetObjectItem(root, "sn");
    cJSON *id_obj = cJSON_GetObjectItem(root, "id");
    cJSON *data = cJSON_GetObjectItem(root, "data");

    if (!cJSON_IsString(ctype_obj) || !cJSON_IsString(sn_obj)) {
        cJSON_Delete(root);
        return;
    }

    const char *ctype = ctype_obj->valuestring;
    const char *gw_sn = sn_obj->valuestring;
    int msg_id = cJSON_IsNumber(id_obj) ? id_obj->valueint : 0;

    bool data_created_locally = false;
    if (!cJSON_IsObject(data)) {
        data = cJSON_CreateObject();
        if (data == NULL) {
            cJSON_Delete(root);
            return;
        }
        data_created_locally = true;
    }

    /* 分发到对应的处理函数 */
    if (strcmp(ctype, "001") == 0) {
        handle_ctype_001(gw_sn, data, msg_id);
    } else if (strcmp(ctype, "002") == 0) {
        handle_ctype_002(gw_sn, data, msg_id);
    } else if (strcmp(ctype, "003") == 0) {
        handle_ctype_003(gw_sn, data, msg_id);
    } else if (strcmp(ctype, "005") == 0) {
        handle_ctype_005(gw_sn, data, msg_id);
    } else if (strcmp(ctype, "004") == 0) {
        cJSON *errcode = cJSON_GetObjectItem(data, "errcode");
        int err_val = parse_number_field(errcode);
        PR_DEBUG("控制响应: gw=%s errcode=%d", gw_sn, err_val);
    }

    if (data_created_locally) cJSON_Delete(data);
    cJSON_Delete(root);
}

/* ==================== TuyaOpen MQTT 客户端回调 ==================== */

/**
 * @brief MQTT 客户端连接成功回调
 */
static void on_mqtt_connected(void *client, void *userdata)
{
    (void)userdata;
    mutex_lock(s_mqtt_client_mutex);
    s_mqtt_client_connected = true;
    mutex_unlock(s_mqtt_client_mutex);
    PR_INFO("本地 MQTT 客户端已连接（TuyaOpen libmqtt）");
}

/**
 * @brief MQTT 客户端断开回调
 */
static void on_mqtt_disconnected(void *client, void *userdata)
{
    (void)userdata;
    mutex_lock(s_mqtt_client_mutex);
    s_mqtt_client_connected = false;
    mutex_unlock(s_mqtt_client_mutex);
    PR_WARN("本地 MQTT 客户端断开，将自动重连");
}

/* ==================== Broker 消息回调 ==================== */

/**
 * @brief mosquitto broker 消息回调
 *
 * 由 broker task 调用，将消息推入 TAL 消息队列。
 */
static void on_broker_message(char *client, char *topic, char *data, int len, int qos, int retain)
{
    if (s_mqtt_msg_queue == NULL) return;

    /* 消息大小限制（DoS 防护）*/
    if (len > MQTT_MAX_PAYLOAD_SIZE) {
        PR_ERR("消息过大 (%d > %d)，丢弃: topic=%s", len, MQTT_MAX_PAYLOAD_SIZE,
               topic ? topic : "(null)");
        return;
    }

    /* 过滤 $SYS 主题（不作为 $SH 协议处理）*/
    if (topic && strncmp(topic, "$SYS/", 5) == 0) {
        return;
    }

    /* mosquitto broker 的 main loop 是单线程的，
     * 此回调不会被并发调用，使用 static 缓冲区安全。*/
    static mqtt_message_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.client_id, client ? client : "", sizeof(msg.client_id) - 1);
    msg.client_id[sizeof(msg.client_id) - 1] = '\0';
    strncpy(msg.topic, topic ? topic : "", sizeof(msg.topic) - 1);
    msg.topic[sizeof(msg.topic) - 1] = '\0';

    int copy_len = (data != NULL && len > 0) ? len : 0;
    if (copy_len >= (int)sizeof(msg.data)) {
        copy_len = sizeof(msg.data) - 1;
        PR_ERR("消息超长已截断: topic=%s len=%d", topic ? topic : "(null)", len);
    }
    if (copy_len > 0) memcpy(msg.data, data, copy_len);
    msg.data[copy_len] = '\0';
    msg.data_len = copy_len;
    msg.qos = qos;
    msg.retain = retain;

    /* 非阻塞推入队列 */
    if (tal_queue_post(s_mqtt_msg_queue, &msg, 0) != OPRT_OK) {
        PR_WARN("消息队列已满，丢弃消息: topic=%s", msg.topic);
    }
}

static int on_broker_connect(const char *client_id, const char *username,
                             const char *password, int password_len)
{
    /* 连接速率限制（DoS 防护）：每分钟最多 30 次连接 */
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    if (s_connect_window_start_ms == 0 || (now - s_connect_window_start_ms) > 60000) {
        s_connect_window_start_ms = now;
        s_connect_count_in_window = 0;
    }
    s_connect_count_in_window++;

    if (s_connect_count_in_window > MAX_CONNECTS_PER_MINUTE) {
        PR_WARN("Broker 连接速率超限（%d次/分钟），拒绝: client=%s",
                s_connect_count_in_window, client_id ? client_id : "(null)");
        return -1;
    }

    PR_DEBUG("Broker 连接: client=%s user=%s",
             client_id ? client_id : "(null)",
             username ? username : "(anonymous)");

    return 0; /* 接受连接 */
}

/* ==================== MQTT 客户端 Task（TuyaOpen libmqtt） ==================== */

/**
 * @brief MQTT 客户端 task
 *
 * TuyaOpen libmqtt 没有自动重连和后台 yield，需要：
 * 1. 连接到本地 broker
 * 2. 循环调用 mqtt_client_yield() 处理网络事件
 * 3. 断开后延迟重连
 */
static void mqtt_client_task(void *arg)
{
    PR_INFO("MQTT 客户端 task 已启动");

    while (s_mqtt_client_should_run) {
        if (s_mqtt_client == NULL) {
            tal_system_sleep(1000);
            continue;
        }

        /* 检查是否已连接 */
        bool need_connect = false;
        mutex_lock(s_mqtt_client_mutex);
        need_connect = !s_mqtt_client_connected;
        mutex_unlock(s_mqtt_client_mutex);

        if (need_connect) {
            PR_INFO("MQTT 客户端正在连接 127.0.0.1:1883...");
            mqtt_client_status_t status = mqtt_client_connect(s_mqtt_client);
            if (status != MQTT_STATUS_SUCCESS) {
                PR_WARN("MQTT 连接失败: %d，%dms 后重试", status, MQTT_RECONNECT_DELAY_MS);
                tal_system_sleep(MQTT_RECONNECT_DELAY_MS);
                continue;
            }
        }

        /* 处理 MQTT 网络事件（接收 PUBACK、保持连接等） */
        mqtt_client_yield(s_mqtt_client);
        tal_system_sleep(MQTT_YIELD_TIMEOUT_MS);
    }

    PR_INFO("MQTT 客户端 task 退出");
    s_mqtt_client_thread = NULL;
    tal_thread_delete(NULL);
}

/* ==================== Broker Task（mosquitto broker 独立 task） ==================== */

/**
 * @brief mosquitto broker task
 *
 * mosq_broker_run() 是阻塞函数，必须运行在独立 task 中。
 * mosq_broker_stop() 可使其退出。
 */
static void broker_task(void *arg)
{
    struct mosq_broker_config broker_cfg = {
        .host = "0.0.0.0",
        .port = 1883,
        .tls_cfg = NULL,
        .handle_message_cb = on_broker_message,
        .handle_connect_cb = on_broker_connect,
    };

    PR_INFO("启动 mosquitto broker: %s:%d", broker_cfg.host, broker_cfg.port);

    int rc = mosq_broker_run(&broker_cfg);
    PR_INFO("mosquitto broker 退出, rc=%d", rc);

    s_broker_running = false;
    s_broker_thread = NULL;
    tal_thread_delete(NULL);
}

/* ==================== Bridge Task ==================== */

static void bridge_task(void *arg)
{
    PR_INFO("协议桥接 task 已启动");

    mqtt_message_t mqtt_msg;

    while (s_bridge_running) {
        /* 仅监听 MQTT 消息队列 */
        int rt = tal_queue_fetch(s_mqtt_msg_queue, &mqtt_msg, 1000);
        if (rt == OPRT_OK) {
            handle_mqtt_message(&mqtt_msg);
        }
    }

    PR_INFO("协议桥接 task 退出");
    s_bridge_thread = NULL;
    tal_thread_delete(NULL);
}

/* ==================== 公共 API 实现 ==================== */

int app_protocol_bridge_init(const protocol_bridge_config_t *config)
{
    if (config == NULL || config->bridge_sn == NULL) {
        PR_ERR("无效的配置参数");
        return -1;
    }

    strncpy(s_bridge_sn, config->bridge_sn, sizeof(s_bridge_sn) - 1);
    s_bridge_sn[sizeof(s_bridge_sn) - 1] = '\0';

    /* 创建互斥锁 */
    if (s_mqtt_client_mutex == NULL) tal_mutex_create_init(&s_mqtt_client_mutex);
    if (s_command_id_mutex == NULL) tal_mutex_create_init(&s_command_id_mutex);
    if (s_pending_003_mutex == NULL) tal_mutex_create_init(&s_pending_003_mutex);
    if (s_gateways_mutex == NULL) tal_mutex_create_init(&s_gateways_mutex);
    if (s_device_map_mutex == NULL) tal_mutex_create_init(&s_device_map_mutex);

    /* 创建 MQTT 消息队列 */
    if (s_mqtt_msg_queue == NULL) {
        if (tal_queue_create_init(&s_mqtt_msg_queue, sizeof(mqtt_message_t), MQTT_MSG_QUEUE_SIZE) != OPRT_OK) {
            PR_ERR("创建 MQTT 消息队列失败");
            return -1;
        }
    }

    /* 创建 TuyaOpen MQTT 客户端（连接到本地 broker，用于发布命令） */
    s_mqtt_client = mqtt_client_new();
    if (s_mqtt_client == NULL) {
        PR_ERR("创建 MQTT 客户端失败");
        return -1;
    }

    mqtt_client_config_t mqtt_cfg = {
        .host = "127.0.0.1",
        .port = 1883,
        .keepalive = 60,
        .timeout_ms = 5000,
        .clientid = s_bridge_sn,
        .username = NULL,
        .password = NULL,
        .cacert = NULL,
        .cacert_len = 0,
        .userdata = NULL,
        .on_connected = on_mqtt_connected,
        .on_disconnected = on_mqtt_disconnected,
        .on_message = NULL,
        .on_published = NULL,
        .on_subscribed = NULL,
        .on_unsubscribed = NULL,
    };

    mqtt_client_status_t status = mqtt_client_init(s_mqtt_client, &mqtt_cfg);
    if (status != MQTT_STATUS_SUCCESS) {
        PR_ERR("MQTT 客户端初始化失败: %d", status);
        mqtt_client_free(s_mqtt_client);
        s_mqtt_client = NULL;
        return -1;
    }

    /* 记录启动时间 */
    s_boot_time_ms = (uint32_t)tal_system_get_millisecond();

    PR_INFO("协议桥接层已初始化: bridge_sn=%s", s_bridge_sn);
    return 0;
}

int app_protocol_bridge_on_wifi_connected(void)
{
    if (s_mqtt_client == NULL) {
        PR_ERR("MQTT 客户端未初始化，忽略 WiFi 连接事件");
        return -1;
    }

    mutex_lock(s_mqtt_client_mutex);
    if (s_mqtt_client_should_run) {
        mutex_unlock(s_mqtt_client_mutex);
        return 0;
    }
    s_mqtt_client_should_run = true;
    mutex_unlock(s_mqtt_client_mutex);

    PR_INFO("WiFi 已连接，启动本地 MQTT 客户端 task...");

    /* 创建 MQTT 客户端 task（处理连接、yield、重连） */
    THREAD_CFG_T mqtt_param = {0};
    mqtt_param.stackDepth = 1024 * 4;
    mqtt_param.priority = THREAD_PRIO_2;
    mqtt_param.thrdname = "mqtt_client";

    int rt = tal_thread_create_and_start(&s_mqtt_client_thread, NULL, NULL,
                                          mqtt_client_task, NULL, &mqtt_param);
    if (rt != OPRT_OK) {
        PR_ERR("创建 MQTT 客户端 task 失败: %d", rt);
        s_mqtt_client_should_run = false;
        return -1;
    }

    PR_INFO("本地 MQTT 客户端 task 已启动");
    return 0;
}

int app_protocol_bridge_start(void)
{
    if (s_bridge_running) return 0;

    /* 启动 mosquitto broker（独立 task，因为 mosq_broker_run 是阻塞函数） */
    s_broker_running = true;
    THREAD_CFG_T broker_param = {0};
    broker_param.stackDepth = 1024 * 16;  /* mosquitto 需要较大栈 */
    broker_param.priority = THREAD_PRIO_3;
    broker_param.thrdname = "mqtt_broker";

    int rt = tal_thread_create_and_start(&s_broker_thread, NULL, NULL,
                                          broker_task, NULL, &broker_param);
    if (rt != OPRT_OK) {
        PR_ERR("创建 broker task 失败: %d", rt);
        s_broker_running = false;
        return -1;
    }

    /* 等待 broker 启动（短暂延迟让 broker 绑定端口） */
    tal_system_sleep(500);

    s_bridge_running = true;

    /* 创建 bridge task（处理 MQTT 消息队列） */
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = BRIDGE_TASK_STACK_SIZE;
    thrd_param.priority = BRIDGE_TASK_PRIORITY;
    thrd_param.thrdname = "proto_bridge";

    rt = tal_thread_create_and_start(&s_bridge_thread, NULL, NULL,
                                      bridge_task, NULL, &thrd_param);
    if (rt != OPRT_OK) {
        PR_ERR("创建 bridge task 失败: %d", rt);
        s_bridge_running = false;
        mosq_broker_stop();
        return -1;
    }

    PR_INFO("协议桥接已启动（broker + bridge task）");
    return 0;
}

void app_protocol_bridge_stop(void)
{
    /* 1. 停止 bridge task */
    s_bridge_running = false;
    /* bridge task 在 tal_queue_fetch 超时后会检查 s_bridge_running 并退出 */
    tal_system_sleep(1100); /* 等待队列超时 + 少量余量 */
    if (s_bridge_thread != NULL) {
        tal_thread_delete(s_bridge_thread);
        s_bridge_thread = NULL;
    }

    /* 2. 停止 MQTT 客户端 task */
    mutex_lock(s_mqtt_client_mutex);
    s_mqtt_client_should_run = false;
    mutex_unlock(s_mqtt_client_mutex);
    tal_system_sleep(MQTT_YIELD_TIMEOUT_MS + 100);
    if (s_mqtt_client_thread != NULL) {
        tal_thread_delete(s_mqtt_client_thread);
        s_mqtt_client_thread = NULL;
    }

    /* 3. 停止 broker */
    if (s_broker_running) {
        mosq_broker_stop();
        s_broker_running = false;
    }
    tal_system_sleep(200);
    if (s_broker_thread != NULL) {
        tal_thread_delete(s_broker_thread);
        s_broker_thread = NULL;
    }

    /* 4. 断开并销毁 MQTT 客户端 */
    if (s_mqtt_client) {
        mutex_lock(s_mqtt_client_mutex);
        void *client = s_mqtt_client;
        s_mqtt_client = NULL;
        s_mqtt_client_connected = false;
        s_mqtt_client_should_run = false;
        mutex_unlock(s_mqtt_client_mutex);

        mqtt_client_disconnect(client);
        mqtt_client_deinit(client);
        mqtt_client_free(client);
    }
}

void app_protocol_bridge_check_gateway_offline(void)
{
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    uint32_t timeout_ms = GATEWAY_OFFLINE_TIMEOUT_SEC * 1000;
    uint32_t cleanup_ms = 3600 * 1000;

    for (int i = 0; i < MAX_GATEWAYS; i++) {
        char gw_sn_buf[32] = {0};
        bool should_mark_offline = false;
        bool should_cleanup = false;

        mutex_lock(s_gateways_mutex);
        if (s_gateways[i].in_use && s_gateways[i].online &&
            (now - s_gateways[i].last_seen_ms) > timeout_ms) {
            s_gateways[i].online = false;
            should_mark_offline = true;
            strncpy(gw_sn_buf, s_gateways[i].gateway_sn, sizeof(gw_sn_buf) - 1);
            gw_sn_buf[sizeof(gw_sn_buf) - 1] = '\0';
        }
        if (s_gateways[i].in_use && !s_gateways[i].online &&
            (now - s_gateways[i].last_seen_ms) > cleanup_ms) {
            if (!should_mark_offline) {
                strncpy(gw_sn_buf, s_gateways[i].gateway_sn, sizeof(gw_sn_buf) - 1);
                gw_sn_buf[sizeof(gw_sn_buf) - 1] = '\0';
            }
            s_gateways[i].in_use = false;
            s_gateways[i].gateway_sn[0] = '\0';
            should_cleanup = true;
        }
        mutex_unlock(s_gateways_mutex);

        if (should_cleanup) {
            PR_INFO("LoRa 网关离线超过 1 小时，清理网关表项: %s", gw_sn_buf);
        }

        if (!should_mark_offline) continue;

        PR_WARN("LoRa 网关离线（%ds 无消息）: %s", GATEWAY_OFFLINE_TIMEOUT_SEC, gw_sn_buf);

        /* 标记该网关下所有子设备为离线（涂鸦 DP 层面） */
        char dev_sns[MAX_BRIDGED_DEVICES][32];
        int dev_count = 0;
        mutex_lock(s_device_map_mutex);
        for (int j = 0; j < MAX_BRIDGED_DEVICES; j++) {
            if (s_device_gateway_map[j].in_use &&
                strcmp(s_device_gateway_map[j].gateway_sn, gw_sn_buf) == 0) {
                strncpy(dev_sns[dev_count], s_device_gateway_map[j].device_sn,
                        sizeof(dev_sns[dev_count]) - 1);
                dev_sns[dev_count][sizeof(dev_sns[dev_count]) - 1] = '\0';
                dev_count++;
            }
        }
        mutex_unlock(s_device_map_mutex);

        for (int k = 0; k < dev_count; k++) {
            app_tuya_bridge_update_online(dev_sns[k], false);
        }
    }
}

void app_protocol_bridge_start_pairing(void)
{
    for (int i = 0; i < MAX_GATEWAYS; i++) {
        char gw_sn[32] = {0};
        bool online = false;
        mutex_lock(s_gateways_mutex);
        if (s_gateways[i].in_use && s_gateways[i].online) {
            strncpy(gw_sn, s_gateways[i].gateway_sn, sizeof(gw_sn) - 1);
            gw_sn[sizeof(gw_sn) - 1] = '\0';
            online = true;
        }
        mutex_unlock(s_gateways_mutex);

        if (!online) continue;

        send_start_pairing(gw_sn);
        PR_INFO("按键触发网关配对模式: %s", gw_sn);
    }
}

void app_protocol_bridge_delete_all_devices(void)
{
    char dev_sns[MAX_BRIDGED_DEVICES][32];
    char gw_sns[MAX_BRIDGED_DEVICES][32];
    int dev_count = 0;

    mutex_lock(s_device_map_mutex);
    for (int i = 0; i < MAX_BRIDGED_DEVICES; i++) {
        if (s_device_gateway_map[i].in_use) {
            strncpy(dev_sns[dev_count], s_device_gateway_map[i].device_sn,
                    sizeof(dev_sns[dev_count]) - 1);
            dev_sns[dev_count][sizeof(dev_sns[dev_count]) - 1] = '\0';
            strncpy(gw_sns[dev_count], s_device_gateway_map[i].gateway_sn,
                    sizeof(gw_sns[dev_count]) - 1);
            gw_sns[dev_count][sizeof(gw_sns[dev_count]) - 1] = '\0';
            dev_count++;
        }
    }
    mutex_unlock(s_device_map_mutex);

    if (dev_count == 0) {
        PR_WARN("没有可删除的子设备");
        /* 清理涂鸦 DP 映射表中所有残留 */
        app_tuya_bridge_remove_all_devices();
        return;
    }

    PR_INFO("3击删除所有子设备: 共 %d 个", dev_count);
    for (int i = 0; i < dev_count; i++) {
        PR_INFO("发送解绑命令 [%d/%d]: gw=%s dev=%s", i + 1, dev_count, gw_sns[i], dev_sns[i]);
        send_unbind_device(gw_sns[i], dev_sns[i]);
        if (i < dev_count - 1) tal_system_sleep(200);
    }

    /* 兜底清理：等待 3 秒后强制移除 */
    PR_INFO("等待3秒后执行兜底清理...");
    tal_system_sleep(3000);

    int cleanup_count = 0;
    for (int i = 0; i < dev_count; i++) {
        if (is_device_in_map(dev_sns[i])) {
            PR_WARN("网关未回复解绑响应，本地强制移除: %s", dev_sns[i]);
            app_tuya_bridge_remove_device(dev_sns[i]);
            local_remove_device_from_map(dev_sns[i]);
            cleanup_count++;
        }
    }
    if (cleanup_count > 0) {
        PR_INFO("兜底清理完成: 强制移除 %d 个设备", cleanup_count);
    }

    /* 最终清理涂鸦 DP 映射表 */
    app_tuya_bridge_remove_all_devices();
}

void app_protocol_bridge_send_control(const char *device_sn, int lora_value)
{
    if (device_sn == NULL) return;

    char gw_sn[32];
    if (!find_gateway_for_device(device_sn, gw_sn, sizeof(gw_sn))) {
        PR_WARN("未找到设备 %s 的网关，无法发送控制命令", device_sn);
        return;
    }

    char value_str[8];
    snprintf(value_str, sizeof(value_str), "%d", lora_value);
    send_device_control(device_sn, gw_sn, value_str);
}

void app_protocol_bridge_send_wind_lock(const char *device_sn, uint8_t mode)
{
    if (device_sn == NULL) return;

    char gw_sn[32];
    if (!find_gateway_for_device(device_sn, gw_sn, sizeof(gw_sn))) {
        PR_WARN("未找到设备 %s 的网关，无法发送风锁模式命令", device_sn);
        return;
    }

    /* 构造风锁模式控制命令（attribute=rwp_wind_lock_mode）
     * 参考 HA 集成 mqtt_handler.py wind_lock_tilt/wind_lock_flat */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return;
    cJSON_AddStringToObject(data, "sn", device_sn);
    cJSON_AddStringToObject(data, "attribute", ATTRIBUTE_WIND_LOCK_MODE);
    char value_str[4];
    snprintf(value_str, sizeof(value_str), "%d", mode);
    cJSON_AddStringToObject(data, "value", value_str);
    publish_sh_to_gateway(gw_sn, "004", data);
    PR_INFO("发送风锁模式命令: gw=%s dev=%s mode=%s", gw_sn, device_sn, value_str);
}

void app_protocol_bridge_reset_tuya(void)
{
    PR_NOTICE("重置涂鸦配网...");
    /* 通过 TuyaOpen SDK 重置配网信息
     * tuya_iot_reset() 只接受 client 参数，调用后会触发
     * TUYA_EVENT_RESET → TUYA_EVENT_RESET_COMPLETE → tal_system_reset */
    tuya_iot_reset(tuya_iot_client_get());
}

/* ==================== $SYS 系统监控 ==================== */

int app_protocol_bridge_get_online_gateway_count(void)
{
    int count = 0;
    mutex_lock(s_gateways_mutex);
    for (int i = 0; i < MAX_GATEWAYS; i++) {
        if (s_gateways[i].in_use && s_gateways[i].online) count++;
    }
    mutex_unlock(s_gateways_mutex);
    return count;
}

void app_protocol_bridge_publish_sys_stats(void)
{
    /* 仅在 MQTT 客户端已连接时发布 */
    mutex_lock(s_mqtt_client_mutex);
    bool connected = s_mqtt_client_connected;
    void *client = s_mqtt_client;
    mutex_unlock(s_mqtt_client_mutex);

    if (!connected || client == NULL) return;

    char buf[128];
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    uint32_t uptime_s = (now - s_boot_time_ms) / 1000;
    int dev_count = app_tuya_bridge_device_count();
    int gw_count = app_protocol_bridge_get_online_gateway_count();

    /* 运行时间 */
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)uptime_s);
    publish_mqtt_json(SYS_TOPIC_PREFIX "/uptime", buf, "$SYS");

    /* 可用内存 */
    int free_heap = tal_system_get_free_heap_size();
    snprintf(buf, sizeof(buf), "%d", free_heap);
    publish_mqtt_json(SYS_TOPIC_PREFIX "/heap_free", buf, "$SYS");

    /* 已注册设备数 */
    snprintf(buf, sizeof(buf), "%d", dev_count);
    publish_mqtt_json(SYS_TOPIC_PREFIX "/devices", buf, "$SYS");

    /* 在线网关数 */
    snprintf(buf, sizeof(buf), "%d", gw_count);
    publish_mqtt_json(SYS_TOPIC_PREFIX "/gateways", buf, "$SYS");

    /* bridge_sn（设备标识） */
    publish_mqtt_json(SYS_TOPIC_PREFIX "/bridge_sn", s_bridge_sn, "$SYS");

    PR_DEBUG("$SYS 发布: uptime=%lus heap=%d devs=%d gws=%d",
             (unsigned long)uptime_s, free_heap, dev_count, gw_count);
}
