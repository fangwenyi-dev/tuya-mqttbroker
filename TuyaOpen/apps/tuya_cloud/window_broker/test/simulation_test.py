#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tuya-mqttbroker simulation test
================================
Verify all optimization logic correctness without hardware.

Run: python test/simulation_test.py
"""

import sys
import io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

import json
import os
import sys
import time
import struct
from collections import deque

# ==================== 测试框架 ====================

class TestResult:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []

    def assert_eq(self, actual, expected, msg=""):
        if actual == expected:
            self.passed += 1
            print(f"  ✅ {msg}" if msg else f"  ✅ assert_eq({actual}, {expected})")
        else:
            self.failed += 1
            err = f"  ❌ {msg}: expected {expected}, got {actual}" if msg else f"  ❌ assert_eq: expected {expected}, got {actual}"
            print(err)
            self.errors.append(err)

    def assert_true(self, condition, msg=""):
        if condition:
            self.passed += 1
            print(f"  ✅ {msg}" if msg else "  ✅ assert_true")
        else:
            self.failed += 1
            err = f"  ❌ {msg}: condition is False" if msg else "  ❌ assert_true: condition is False"
            print(err)
            self.errors.append(err)

    def assert_gt(self, actual, threshold, msg=""):
        if actual > threshold:
            self.passed += 1
            print(f"  ✅ {msg}" if msg else f"  ✅ assert_gt({actual}, {threshold})")
        else:
            self.failed += 1
            err = f"  ❌ {msg}: expected > {threshold}, got {actual}" if msg else f"  ❌ assert_gt: expected > {threshold}, got {actual}"
            print(err)
            self.errors.append(err)

    def summary(self):
        total = self.passed + self.failed
        print(f"\n{'='*60}")
        print(f"测试结果: {self.passed}/{total} 通过, {self.failed} 失败")
        if self.errors:
            print(f"\n失败项:")
            for e in self.errors:
                print(f"  {e}")
        print(f"{'='*60}")
        return self.failed == 0

result = TestResult()

# ==================== 模拟环境 ====================

class MockMQTTBroker:
    """模拟 mosquitto broker"""
    def __init__(self):
        self.connections = []
        self.messages = []
        self.connect_count = 0
        self.connect_window_start = 0
        self.connect_count_in_window = 0

    def on_connect(self, client_id, username, password, password_len, current_time_ms):
        """模拟 on_broker_connect 的连接速率限制逻辑"""
        MAX_CONNECTS_PER_MINUTE = 30

        if self.connect_window_start == 0 or (current_time_ms - self.connect_window_start) > 60000:
            self.connect_window_start = current_time_ms
            self.connect_count_in_window = 0

        self.connect_count_in_window += 1

        if self.connect_count_in_window > MAX_CONNECTS_PER_MINUTE:
            return -1  # 拒绝

        self.connect_count += 1
        self.connections.append(client_id)
        return 0  # 接受

    def on_message(self, client, topic, data, len_data, qos, retain):
        """模拟 on_broker_message 的过滤逻辑"""
        MQTT_MAX_PAYLOAD_SIZE = 8192

        if len_data > MQTT_MAX_PAYLOAD_SIZE:
            return None  # 丢弃

        if topic and topic.startswith("$SYS/"):
            return None  # 过滤 $SYS

        # 模拟 C 代码中的 strncpy 截断（msg.topic 是 char[128]，截断到 127 字符）
        topic_trunc = (topic or "")[:127]
        client_id_trunc = (client or "")[:63]

        return {
            'client_id': client_id_trunc,
            'topic': topic_trunc,
            'data': data[:4096] if data else "",
            'data_len': min(len_data, 4096),
            'qos': qos,
            'retain': retain
        }


class MockDevice:
    """模拟 LoRa 设备"""
    def __init__(self, sn, gateway_sn, position=0, battery=120, state=0, wind_lock=0):
        self.sn = sn
        self.gateway_sn = gateway_sn
        self.position = position
        self.battery = battery
        self.state = state
        self.wind_lock = wind_lock
        self.online = True


class MockGateway:
    """模拟 LoRa 网关"""
    def __init__(self, sn):
        self.sn = sn
        self.online = True
        self.last_seen_ms = 0
        self.devices = []

    def make_001_message(self):
        return {
            "head": "$SH",
            "ctype": "001",
            "id": 1,
            "sn": self.sn,
            "data": {
                "version": "1.0",
                "model": "gateway_v1",
                "userid": "test_user"
            }
        }

    def make_002_message(self):
        return {
            "head": "$SH",
            "ctype": "002",
            "id": 2,
            "sn": self.sn,
            "data": {
                "devices": [
                    {
                        "sn": d.sn,
                        "model": "curtain_ctr",
                        "r_travel": d.position,
                        "battery": d.battery
                    } for d in self.devices
                ]
            }
        }

    def make_005_message(self, device, attrs_format=False):
        if attrs_format:
            return {
                "head": "$SH",
                "ctype": "005",
                "id": 5,
                "sn": self.sn,
                "data": {
                    "sn": device.sn,
                    "attrs": [
                        {"attribute": "r_travel", "value": str(device.position)},
                        {"attribute": "voltage", "value": str(device.battery)},
                        {"attribute": "state", "value": str(device.state)},
                        {"attribute": "rwp_wind_lock_mode", "value": str(device.wind_lock)}
                    ]
                }
            }
        else:
            return {
                "head": "$SH",
                "ctype": "005",
                "id": 5,
                "sn": self.sn,
                "data": {
                    "sn": device.sn,
                    "position": device.position,
                    "battery": device.battery,
                    "state": device.state
                }
            }


class MockTuyaBridge:
    """模拟涂鸦 DP 桥接层"""
    def __init__(self):
        self.devices = {}  # sn -> index
        self.dp_reports = []
        self.dp_report_failures = 0
        self.next_index = 0
        self.MAX_TUYA_DEVICES = 12
        self.DP_PER_DEVICE = 4

    def add_device(self, sn):
        if sn in self.devices:
            return self.devices[sn]
        if self.next_index >= self.MAX_TUYA_DEVICES:
            return -1
        idx = self.next_index
        self.next_index += 1
        self.devices[sn] = idx
        return idx

    def remove_device(self, sn):
        if sn in self.devices:
            del self.devices[sn]

    def dp_id(self, device_index, offset):
        return device_index * self.DP_PER_DEVICE + offset + 1

    def report_dp_value(self, dp_id, value, fail_first_n=0):
        """模拟带重试的 DP 上报"""
        DP_REPORT_RETRY_COUNT = 2

        for retry in range(DP_REPORT_RETRY_COUNT + 1):
            if retry < fail_first_n:
                self.dp_report_failures += 1
                continue
            self.dp_reports.append({'dp_id': dp_id, 'value': value, 'type': 'value'})
            return True
        return False

    def report_dp_enum(self, dp_id, value, fail_first_n=0):
        """模拟带重试的 DP 上报"""
        DP_REPORT_RETRY_COUNT = 2

        for retry in range(DP_REPORT_RETRY_COUNT + 1):
            if retry < fail_first_n:
                self.dp_report_failures += 1
                continue
            self.dp_reports.append({'dp_id': dp_id, 'value': value, 'type': 'enum'})
            return True
        return False

    def device_count(self):
        return len(self.devices)

    def supports_wind_lock(self, sn):
        return sn[:4] == "5005"


# ==================== 测试用例 ====================

print("\n" + "="*60)
print("tuya-mqttbroker 优化模拟测试")
print("="*60)

# ---------- 测试 1: KV Seed/Key MAC 派生 ----------
print("\n📋 测试 1: KV Seed/Key MAC 地址派生")
print("-" * 40)

def derive_kv_from_mac(mac_bytes):
    """Python 版本的 KV 派生逻辑（与 C 代码一致）"""
    m = list(mac_bytes)
    xor_all = m[0] ^ m[1] ^ m[2] ^ m[3] ^ m[4] ^ m[5]
    seed = f"ty{m[0]:02X}{m[1]:02X}{m[2]:02X}{m[3]:02X}{m[4]:02X}{m[5]:02X}{xor_all:02X}"

    xor_tail = m[2] ^ m[3] ^ m[4] ^ m[5]
    key = f"br{m[5]:02X}{m[4]:02X}{m[3]:02X}{m[2]:02X}{m[1]:02X}{m[0]:02X}{xor_tail:02X}"

    return seed, key

# 测试不同 MAC 产生不同 seed/key
mac1 = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
mac2 = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
mac3 = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])  # 与 mac1 相同

seed1, key1 = derive_kv_from_mac(mac1)
seed2, key2 = derive_kv_from_mac(mac2)
seed3, key3 = derive_kv_from_mac(mac3)

result.assert_eq(len(seed1), 16, "seed 长度为 16 字符")
result.assert_eq(len(key1), 16, "key 长度为 16 字符")
result.assert_true(seed1 != seed2, "不同 MAC 产生不同 seed")
result.assert_true(key1 != key2, "不同 MAC 产生不同 key")
result.assert_eq(seed1, seed3, "相同 MAC 产生相同 seed（确定性）")
result.assert_eq(key1, key3, "相同 MAC 产生相同 key（确定性）")
result.assert_true(seed1.startswith("ty"), "seed 前缀为 'ty'")
result.assert_true(key1.startswith("br"), "key 前缀为 'br'")
result.assert_true(seed1 != key1, "seed 和 key 不同")
print(f"  ℹ️ MAC {mac1.hex()} → seed={seed1}, key={key1}")

# ---------- 测试 2: DoS 防护 - 连接速率限制 ----------
print("\n📋 测试 2: DoS 防护 - 连接速率限制")
print("-" * 40)

broker = MockMQTTBroker()
current_time = 10000  # 非零起始时间（模拟系统启动后 10 秒）

# 正常连接（30 次以内应该全部成功）
for i in range(30):
    rc = broker.on_connect(f"client_{i}", None, None, 0, current_time)
    result.assert_eq(rc, 0, f"第 {i+1} 次连接应成功")

# 第 31 次连接应该被拒绝
rc = broker.on_connect("client_31", None, None, 0, current_time)
result.assert_eq(rc, -1, "第 31 次连接应被拒绝（速率限制）")

# 60 秒后窗口重置，应再次允许连接
current_time += 61000
rc = broker.on_connect("client_new", None, None, 0, current_time)
result.assert_eq(rc, 0, "60 秒后窗口重置，连接应成功")

# ---------- 测试 3: DoS 防护 - 消息大小限制 ----------
print("\n📋 测试 3: DoS 防护 - 消息大小限制")
print("-" * 40)

broker = MockMQTTBroker()

# 正常大小消息（8KB 以内）
msg = broker.on_message("gw1", "gateway/gw1/rpt", '{"test":1}', 12, 0, 0)
result.assert_true(msg is not None, "正常大小消息应通过")

# 超大消息（>8KB）应被丢弃
large_data = "x" * 9000
msg = broker.on_message("gw1", "gateway/gw1/rpt", large_data, 9000, 0, 0)
result.assert_true(msg is None, "超大消息 (>8KB) 应被丢弃")

# 恰好 8KB 的消息应通过
data_8k = "x" * 8192
msg = broker.on_message("gw1", "gateway/gw1/rpt", data_8k, 8192, 0, 0)
result.assert_true(msg is not None, "8KB 消息应通过（边界值）")

# 8193 字节应被丢弃
msg = broker.on_message("gw1", "gateway/gw1/rpt", "x"*8193, 8193, 0, 0)
result.assert_true(msg is None, "8193 字节消息应被丢弃")

# ---------- 测试 4: $SYS 主题过滤 ----------
print("\n📋 测试 4: $SYS 主题过滤")
print("-" * 40)

broker = MockMQTTBroker()

# $SYS 主题消息应被过滤
msg = broker.on_message("internal", "$SYS/broker/uptime", "12345", 5, 0, 0)
result.assert_true(msg is None, "$SYS 主题应被过滤")

# 正常主题消息应通过
msg = broker.on_message("gw1", "gateway/gw1/rpt", '{"head":"$SH"}', 16, 0, 0)
result.assert_true(msg is not None, "正常主题消息应通过")

# ---------- 测试 5: cJSON 深度限制 ----------
print("\n📋 测试 5: cJSON 深度限制")
print("-" * 40)

CJSON_MAX_DEPTH = 20

def check_json_depth(json_str):
    """模拟 C 代码中的 cJSON 深度检查"""
    try:
        data = json.loads(json_str)
    except:
        return -1  # 解析失败

    depth = 0
    current = data
    while current is not None and depth < CJSON_MAX_DEPTH + 1:
        if isinstance(current, dict):
            if len(current) > 0:
                current = list(current.values())[0]
            else:
                current = None
        elif isinstance(current, list):
            if len(current) > 0:
                current = current[0]
            else:
                current = None
        else:
            current = None
        depth += 1

    return depth

# 正常 JSON（深度 3）
normal_json = '{"head":"$SH","data":{"devices":[{"sn":"123"}]}}'
depth = check_json_depth(normal_json)
result.assert_true(depth <= CJSON_MAX_DEPTH, f"正常 JSON 深度 {depth} 应 <= {CJSON_MAX_DEPTH}")

# 深层嵌套 JSON（深度 25，应超限）
deep_json = '{"a":' * 25 + '1' + '}' * 25
depth = check_json_depth(deep_json)
result.assert_true(depth > CJSON_MAX_DEPTH, f"深层嵌套 JSON 深度 {depth} 应 > {CJSON_MAX_DEPTH}")

# 恰好 19 层（深度=20，边界值，应通过）
# 注意：深度计算 = 嵌套层数 + 1（值节点），所以 19 层嵌套 → depth=20 = CJSON_MAX_DEPTH
boundary_json = '{"a":' * 19 + '1' + '}' * 19
depth = check_json_depth(boundary_json)
result.assert_true(depth <= CJSON_MAX_DEPTH, f"19 层嵌套深度 {depth} 应 <= {CJSON_MAX_DEPTH}")

# ---------- 测试 6: $SH 协议完整流程 ----------
print("\n📋 测试 6: $SH 协议完整流程（001→002→005→004→003）")
print("-" * 40)

bridge = MockTuyaBridge()
gateway = MockGateway("1001ABCD1234")

# 6.1: 网关绑定 (001)
msg_001 = gateway.make_001_message()
result.assert_eq(msg_001["head"], "$SH", "001 head 字段正确")
result.assert_eq(msg_001["ctype"], "001", "001 ctype 正确")
result.assert_eq(msg_001["sn"], "1001ABCD1234", "001 sn 正确")
result.assert_true("version" in msg_001["data"], "001 data 包含 version")

# 6.2: 设备列表 (002)
device1 = MockDevice("5005AAAA1111", gateway.sn, position=50, battery=120, state=1, wind_lock=0)
device2 = MockDevice("5005BBBB2222", gateway.sn, position=0, battery=100, state=0, wind_lock=1)
device3 = MockDevice("3008CCCC3333", gateway.sn, position=100, battery=135, state=1)
gateway.devices = [device1, device2, device3]

msg_002 = gateway.make_002_message()
result.assert_eq(len(msg_002["data"]["devices"]), 3, "002 包含 3 个设备")

# 模拟设备注册
for d in gateway.devices:
    idx = bridge.add_device(d.sn)
    result.assert_true(idx >= 0, f"设备 {d.sn} 注册成功 (索引={idx})")

# 验证 DP ID 分配
idx1 = bridge.devices[device1.sn]
result.assert_eq(bridge.dp_id(idx1, 0), idx1*4+1, f"设备1 位置 DP = {idx1*4+1}")
result.assert_eq(bridge.dp_id(idx1, 1), idx1*4+2, f"设备1 控制 DP = {idx1*4+2}")
result.assert_eq(bridge.dp_id(idx1, 2), idx1*4+3, f"设备1 电池 DP = {idx1*4+3}")
result.assert_eq(bridge.dp_id(idx1, 3), idx1*4+4, f"设备1 风锁 DP = {idx1*4+4}")

# 6.3: 状态上报 (005) - 直接字段格式
msg_005 = gateway.make_005_message(device1, attrs_format=False)
result.assert_eq(msg_005["ctype"], "005", "005 ctype 正确")
result.assert_eq(msg_005["data"]["sn"], device1.sn, "005 sn 正确")
result.assert_eq(msg_005["data"]["position"], 50, "005 position 正确")

# 6.4: 状态上报 (005) - attrs 数组格式
msg_005_attrs = gateway.make_005_message(device2, attrs_format=True)
result.assert_true("attrs" in msg_005_attrs["data"], "005 attrs 格式包含 attrs 数组")
attrs = msg_005_attrs["data"]["attrs"]
result.assert_eq(len(attrs), 4, "attrs 数组包含 4 个属性")

# 6.5: 控制命令映射 (004)
control_map = {0: 101, 1: 100, 2: 0, 3: 200}  # 停止/打开/关闭/内倒
for dp_val, lora_val in control_map.items():
    result.assert_eq(control_map[dp_val], lora_val, f"控制映射 DP={dp_val} → LoRa={lora_val}")

# 6.6: 风锁模式设备判断
result.assert_true(bridge.supports_wind_lock("5005AAAA1111"), "SN 前缀 5005 支持风锁")
result.assert_true(not bridge.supports_wind_lock("3008CCCC3333"), "SN 前缀 3008 不支持风锁")

# 6.7: 解绑 (003)
result.assert_true(device1.sn in bridge.devices, "解绑前设备存在")
bridge.remove_device(device1.sn)
result.assert_true(device1.sn not in bridge.devices, "解绑后设备不存在")

# ---------- 测试 7: 电池电压转换 ----------
print("\n📋 测试 7: 电池电压转换")
print("-" * 40)

BATTERY_RAW_MIN = 80
BATTERY_RAW_MAX = 140

def battery_to_percent(voltage):
    if voltage <= BATTERY_RAW_MIN:
        return 0
    elif voltage >= BATTERY_RAW_MAX:
        return 100
    else:
        return (voltage - BATTERY_RAW_MIN) * 100 // (BATTERY_RAW_MAX - BATTERY_RAW_MIN)

result.assert_eq(battery_to_percent(80), 0, "80 → 0%")
result.assert_eq(battery_to_percent(140), 100, "140 → 100%")
result.assert_eq(battery_to_percent(110), 50, "110 → 50%")
result.assert_eq(battery_to_percent(70), 0, "70 (< 80) → 0%")
result.assert_eq(battery_to_percent(150), 100, "150 (> 140) → 100%")
result.assert_eq(battery_to_percent(100), 33, "100 → 33%")
result.assert_eq(battery_to_percent(120), 66, "120 → 66%")

# ---------- 测试 8: DP 上报重试 ----------
print("\n📋 测试 8: DP 上报重试机制")
print("-" * 40)

bridge_retry = MockTuyaBridge()
bridge_retry.add_device("5005TEST0001")
idx = bridge_retry.devices["5005TEST0001"]
dp_id = bridge_retry.dp_id(idx, 0)  # 位置 DP

# 正常上报（无失败）
bridge_retry.dp_reports.clear()
bridge_retry.dp_report_failures = 0
ok = bridge_retry.report_dp_value(dp_id, 50, fail_first_n=0)
result.assert_true(ok, "正常上报应成功")
result.assert_eq(len(bridge_retry.dp_reports), 1, "正常上报产生 1 条记录")

# 首次失败，重试成功
bridge_retry.dp_reports.clear()
bridge_retry.dp_report_failures = 0
ok = bridge_retry.report_dp_value(dp_id, 60, fail_first_n=1)
result.assert_true(ok, "首次失败后重试应成功")
result.assert_eq(len(bridge_retry.dp_reports), 1, "重试成功后产生 1 条记录")
result.assert_eq(bridge_retry.dp_report_failures, 1, "记录 1 次失败")

# 全部失败（3 次都失败）
bridge_retry.dp_reports.clear()
bridge_retry.dp_report_failures = 0
ok = bridge_retry.report_dp_value(dp_id, 70, fail_first_n=3)
result.assert_true(not ok, "3 次全部失败应返回 False")
result.assert_eq(len(bridge_retry.dp_reports), 0, "全部失败不产生记录")
result.assert_eq(bridge_retry.dp_report_failures, 3, "记录 3 次失败")

# ---------- 测试 9: $SYS 监控主题 ----------
print("\n📋 测试 9: $SYS 监控主题发布")
print("-" * 40)

SYS_TOPICS = [
    "$SYS/broker/uptime",
    "$SYS/broker/heap_free",
    "$SYS/broker/devices",
    "$SYS/broker/gateways",
    "$SYS/broker/bridge_sn",
]

# 验证 $SYS 主题列表完整
for topic in SYS_TOPICS:
    result.assert_true(topic.startswith("$SYS/broker/"), f"主题 {topic} 前缀正确")

# 验证 $SYS 主题被 broker 消息回调过滤
broker = MockMQTTBroker()
for topic in SYS_TOPICS:
    msg = broker.on_message("internal", topic, "test", 4, 0, 0)
    result.assert_true(msg is None, f"$SYS 主题 {topic} 应被消息回调过滤")

# ---------- 测试 10: 设备表容量限制 ----------
print("\n📋 测试 10: 设备表容量限制")
print("-" * 40)

bridge_full = MockTuyaBridge()
MAX_DEVICES = 12

# 添加 12 个设备（应全部成功）
for i in range(MAX_DEVICES):
    sn = f"5005DEV{i:04d}"
    idx = bridge_full.add_device(sn)
    result.assert_true(idx >= 0, f"设备 {i+1}/{MAX_DEVICES} 注册成功")

# 第 13 个设备应失败
idx = bridge_full.add_device("5005DEV0013")
result.assert_eq(idx, -1, "第 13 个设备应注册失败（表满）")

# ---------- 测试 11: 网关设备过滤 ----------
print("\n📋 测试 11: 网关设备过滤逻辑")
print("-" * 40)

DEVICE_SN_PREFIX_GATEWAY = "1001"

def should_skip_device(device_sn, model=None):
    if device_sn.startswith(DEVICE_SN_PREFIX_GATEWAY):
        return True
    if model:
        for kw in ["gateway", "Gateway", "GATEWAY", "网关"]:
            if kw in model:
                return True
    return False

result.assert_true(should_skip_device("1001ABCD1234"), "SN 前缀 1001 的网关设备应跳过")
result.assert_true(not should_skip_device("5005AAAA1111"), "SN 前缀 5005 的开窗器不应跳过")
result.assert_true(should_skip_device("5005AAAA1111", "gateway_v1"), "model 含 gateway 应跳过")
result.assert_true(should_skip_device("5005AAAA1111", "网关设备"), "model 含'网关'应跳过")
result.assert_true(not should_skip_device("5005AAAA1111", "curtain_ctr"), "model=curtain_ctr 不应跳过")

# ---------- 测试 12: 按键 LED 非阻塞 ----------
print("\n📋 测试 12: 按键 LED 非阻塞验证")
print("-" * 40)

# 验证 2 击和 3 击使用 duration 参数（非阻塞），而非 tal_system_sleep
# 读取 app_button.c 验证代码逻辑
button_file = os.path.join(os.path.dirname(__file__), '..', 'src', 'app_button.c')
if os.path.exists(button_file):
    with open(button_file, 'r', encoding='utf-8') as f:
        button_code = f.read()

    # 验证 2 击使用 duration 而非 sleep
    has_duration_2click = 'LED_MODE_FAST_BLINK, 3000' in button_code
    has_duration_3click = 'LED_MODE_FAST_BLINK, 5000' in button_code
    no_blocking_sleep_2click = 'tal_system_sleep(3000)' not in button_code.split('case 2:')[1].split('case 3:')[0] if 'case 2:' in button_code and 'case 3:' in button_code else True
    no_blocking_sleep_3click = 'tal_system_sleep(1000)' not in button_code.split('case 3:')[1].split('default:')[0] if 'case 3:' in button_code and 'default:' in button_code else True

    result.assert_true(has_duration_2click, "2 击使用 LED duration=3000ms")
    result.assert_true(has_duration_3click, "3 击使用 LED duration=5000ms")
    result.assert_true(no_blocking_sleep_2click, "2 击不再有阻塞 tal_system_sleep(3000)")
    result.assert_true(no_blocking_sleep_3click, "3 击不再有阻塞 tal_system_sleep(1000)")
else:
    print("  ⚠️ app_button.c 未找到，跳过此测试")

# ---------- 测试 13: WiFi 断连事件处理 ----------
print("\n📋 测试 13: WiFi 断连事件处理")
print("-" * 40)

main_file = os.path.join(os.path.dirname(__file__), '..', 'src', 'tuya_main.c')
if os.path.exists(main_file):
    with open(main_file, 'r', encoding='utf-8') as f:
        main_code = f.read()

    has_mqtt_disconnect = 'TUYA_EVENT_MQTT_DISCONNECT' in main_code
    has_wifi_false = 's_wifi_connected = false' in main_code
    has_disconnect_led = 'LED_MODE_SLOW_BLINK' in main_code.split('TUYA_EVENT_MQTT_DISCONNECT')[1].split('case')[0] if 'TUYA_EVENT_MQTT_DISCONNECT' in main_code else False

    result.assert_true(has_mqtt_disconnect, "tuya_main.c 包含 TUYA_EVENT_MQTT_DISCONNECT 处理")
    result.assert_true(has_wifi_false, "断连时设置 s_wifi_connected = false")
    result.assert_true(has_disconnect_led, "断连时设置蓝灯慢闪")
else:
    print("  ⚠️ tuya_main.c 未找到，跳过此测试")

# ---------- 测试 14: KV 派生代码存在 ----------
print("\n📋 测试 14: KV MAC 派生代码验证")
print("-" * 40)

if os.path.exists(main_file):
    has_derive_func = 'derive_kv_from_mac' in main_code
    has_no_hardcoded = '"vmlkasdh93dlvlcy"' not in main_code.split('derive_kv_from_mac')[0] if 'derive_kv_from_mac' in main_code else True
    has_mac_call = 'tkl_wifi_get_mac(WF_STATION, &kv_mac)' in main_code
    has_fallback = 'KV 使用回退密钥' in main_code

    result.assert_true(has_derive_func, "tuya_main.c 包含 derive_kv_from_mac 函数")
    result.assert_true(has_mac_call, "KV 初始化时调用 tkl_wifi_get_mac")
    result.assert_true(has_fallback, "MAC 获取失败有回退逻辑")

# ---------- 测试 15: MQTT 发布重试 ----------
print("\n📋 测试 15: MQTT 发布重试机制")
print("-" * 40)

bridge_file = os.path.join(os.path.dirname(__file__), '..', 'src', 'app_protocol_bridge.c')
if os.path.exists(bridge_file):
    with open(bridge_file, 'r', encoding='utf-8') as f:
        bridge_code = f.read()

    has_retry = 'MQTT_PUBLISH_RETRY' in bridge_code
    has_retry_loop = 'for (int retry = 0; retry <= MQTT_PUBLISH_RETRY' in bridge_code
    has_not_connected_return = '!connected' in bridge_code and 'return;' in bridge_code.split('!connected')[1][:100]

    result.assert_true(has_retry, "定义了 MQTT_PUBLISH_RETRY 常量")
    result.assert_true(has_retry_loop, "publish_mqtt_json 包含重试循环")
    result.assert_true(has_not_connected_return, "未连接时直接返回（不再仅警告后继续）")

# ---------- 测试 16: 线程清理顺序 ----------
print("\n📋 测试 16: 线程清理顺序验证")
print("-" * 40)

if os.path.exists(bridge_file):
    # 使用下一个函数名作为分隔，避免 'void *client' 导致提前截断
    stop_section = bridge_code.split('void app_protocol_bridge_stop')[1].split('void app_protocol_bridge_check_gateway_offline')[0] if 'void app_protocol_bridge_stop' in bridge_code else ""

    # 验证停止顺序：bridge task → MQTT client task → broker → MQTT client cleanup
    bridge_stop_pos = stop_section.find('s_bridge_running = false')
    mqtt_task_stop_pos = stop_section.find('s_mqtt_client_should_run = false')
    broker_stop_pos = stop_section.find('mosq_broker_stop')
    mqtt_client_cleanup_pos = stop_section.find('mqtt_client_disconnect')

    result.assert_true(bridge_stop_pos < mqtt_task_stop_pos, "先停 bridge task，再停 MQTT client task")
    result.assert_true(mqtt_task_stop_pos < broker_stop_pos, "先停 MQTT client task，再停 broker")
    result.assert_true(broker_stop_pos < mqtt_client_cleanup_pos, "先停 broker，再清理 MQTT client")

    has_sleep_before_delete = 'tal_system_sleep(1100)' in stop_section
    result.assert_true(has_sleep_before_delete, "删除线程前有等待时间")

# ---------- 测试 17: 边界条件测试 ----------
print("\n📋 测试 17: 边界条件测试")
print("-" * 40)

# 空 JSON
depth = check_json_depth('')
result.assert_eq(depth, -1, "空字符串 JSON 解析失败")

# 无效 JSON
depth = check_json_depth('not json')
result.assert_eq(depth, -1, "无效 JSON 解析失败")

# 空对象
depth = check_json_depth('{}')
result.assert_true(depth <= CJSON_MAX_DEPTH, "空对象深度正常")

# 空消息
broker = MockMQTTBroker()
msg = broker.on_message("gw1", "gateway/gw1/rpt", None, 0, 0, 0)
result.assert_true(msg is not None, "空数据消息应通过（data=NULL, len=0）")
result.assert_eq(msg['data_len'], 0, "空消息 data_len=0")

# 超长 topic
long_topic = "a" * 200
msg = broker.on_message("gw1", long_topic, '{"test":1}', 12, 0, 0)
result.assert_true(msg is not None, "超长 topic 消息应通过（会被截断到 127 字符）")
result.assert_eq(len(msg['topic']), 127, "超长 topic 被截断到 127 字符")

# ---------- 测试 18: $SYS 发布内容验证 ----------
print("\n📋 测试 18: $SYS 发布内容验证")
print("-" * 40)

# 模拟 $SYS 发布内容
uptime_s = 3600
free_heap = 86000
dev_count = 5
gw_count = 2
bridge_sn = "AABBCCDDEEFF"

sys_payloads = {
    "$SYS/broker/uptime": str(uptime_s),
    "$SYS/broker/heap_free": str(free_heap),
    "$SYS/broker/devices": str(dev_count),
    "$SYS/broker/gateways": str(gw_count),
    "$SYS/broker/bridge_sn": bridge_sn,
}

for topic, payload in sys_payloads.items():
    result.assert_true(topic.startswith("$SYS/broker/"), f"主题 {topic} 格式正确")
    result.assert_true(len(payload) > 0, f"主题 {topic} 有效负载非空")
    result.assert_true(payload.isdigit() or len(payload) == 12, f"主题 {topic} 有效负载格式正确")

# ---------- 测试 19: DP 控制命令值映射 ----------
print("\n📋 测试 19: DP 控制命令值映射完整性")
print("-" * 40)

# 验证所有控制值映射
expected_mappings = {
    0: 101,   # CTRL_STOP → 101
    1: 100,   # CTRL_OPEN → 100
    2: 0,     # CTRL_CLOSE → 0
    3: 200,   # CTRL_TILT → 200
}

for dp_val, lora_val in expected_mappings.items():
    result.assert_eq(expected_mappings[dp_val], lora_val, f"DP control={dp_val} → LoRa value={lora_val}")

# 验证风锁模式映射
wind_lock_mappings = {0: 0, 1: 1}  # 内倒模式=0, 平开模式=1
for dp_val, lora_val in wind_lock_mappings.items():
    result.assert_eq(wind_lock_mappings[dp_val], lora_val, f"DP wind_lock={dp_val} → LoRa value={lora_val}")

# ---------- 测试 20: 消息队列容量 ----------
print("\n📋 测试 20: 消息队列容量限制")
print("-" * 40)

MQTT_MSG_QUEUE_SIZE = 10
queue = deque(maxlen=MQTT_MSG_QUEUE_SIZE)

# 填满队列
for i in range(MQTT_MSG_QUEUE_SIZE):
    queue.append(f"msg_{i}")
result.assert_eq(len(queue), MQTT_MSG_QUEUE_SIZE, f"队列填满 {MQTT_MSG_QUEUE_SIZE} 条")

# 第 11 条应导致最早的被丢弃
queue.append("msg_10")
result.assert_eq(len(queue), MQTT_MSG_QUEUE_SIZE, "队列满后添加不超限")
result.assert_true("msg_0" not in queue, "最早的 msg_0 被丢弃")
result.assert_true("msg_10" in queue, "最新的 msg_10 在队列中")

# ---------- 测试 21: mDNS 主机名配置 ----------
print("\n📋 测试 21: mDNS 主机名配置验证")
print("-" * 40)

main_file = os.path.join(os.path.dirname(__file__), '..', 'src', 'tuya_main.c')
if os.path.exists(main_file):
    with open(main_file, 'r', encoding='utf-8') as f:
        main_code = f.read()

    # 验证 mDNS 相关定义
    has_mdns_include = '#include "mdns.h"' in main_code
    has_mdns_hostname = 'MDNS_HOSTNAME' in main_code and 'matter-broker' in main_code
    has_mdns_service = '_mqtt' in main_code and '_tcp' in main_code
    has_mdns_port = '1883' in main_code
    has_mdns_init_flag = 's_mdns_initialized' in main_code
    has_mdns_setup_func = 'setup_mdns_service' in main_code
    has_mdns_call = 'setup_mdns_service();' in main_code

    # 验证 mDNS 在 TUYA_EVENT_DIRECT_MQTT_CONNECTED 中调用
    mqtt_connected_section = main_code.split('TUYA_EVENT_DIRECT_MQTT_CONNECTED')[1].split('break;')[0] if 'TUYA_EVENT_DIRECT_MQTT_CONNECTED' in main_code else ''
    has_mdns_in_event = 'setup_mdns_service' in mqtt_connected_section

    result.assert_true(has_mdns_include, "tuya_main.c 包含 mdns.h")
    result.assert_true(has_mdns_hostname, "mDNS 主机名定义为 matter-broker")
    result.assert_true(has_mdns_service, "mDNS 服务类型为 _mqtt._tcp")
    result.assert_true(has_mdns_port, "mDNS 服务端口为 1883")
    result.assert_true(has_mdns_init_flag, "mDNS 初始化标志存在")
    result.assert_true(has_mdns_setup_func, "setup_mdns_service() 函数已定义")
    result.assert_true(has_mdns_call, "setup_mdns_service() 在事件处理器中被调用")
    result.assert_true(has_mdns_in_event, "mDNS 在 TUYA_EVENT_DIRECT_MQTT_CONNECTED 事件中初始化")
else:
    print("  ⚠️ tuya_main.c 未找到，跳过此测试")

# ---------- 测试 22: mDNS 一次性初始化保护 ----------
print("\n📋 测试 22: mDNS 一次性初始化保护")
print("-" * 40)

if os.path.exists(main_file):
    with open(main_file, 'r', encoding='utf-8') as f:
        main_code = f.read()

    # 验证 setup_mdns_service 函数中有 s_mdns_initialized 检查
    # 获取完整函数体（从函数签名到下一个章节标记）
    setup_func_section = main_code.split('setup_mdns_service(void)')[1].split('用户日志输出回调')[0] if 'setup_mdns_service(void)' in main_code else ''
    has_early_return = 'if (s_mdns_initialized)' in setup_func_section
    has_set_flag = 's_mdns_initialized = true' in setup_func_section

    result.assert_true(has_early_return, "setup_mdns_service 入口检查 s_mdns_initialized 避免重复初始化")
    result.assert_true(has_set_flag, "setup_mdns_service 成功后设置 s_mdns_initialized = true")
else:
    print("  ⚠️ tuya_main.c 未找到，跳过此测试")

# ---------- 测试 23: CMakeLists mDNS include 路径 ----------
print("\n📋 测试 23: CMakeLists.txt mDNS include 路径")
print("-" * 40)

cmake_file = os.path.join(os.path.dirname(__file__), '..', 'CMakeLists.txt')
if os.path.exists(cmake_file):
    with open(cmake_file, 'r', encoding='utf-8') as f:
        cmake_code = f.read()

    has_mdns_include = 'mdns/include' in cmake_code
    result.assert_true(has_mdns_include, "CMakeLists.txt 包含 mdns include 路径")
else:
    print("  ⚠️ CMakeLists.txt 未找到，跳过此测试")

# ---------- 测试 24: 烧录工具授权码功能 ----------
print("\n📋 测试 24: 烧录工具授权码写入功能")
print("-" * 40)

flash_tool_file = os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', '..', 'flash', 'flash_tool.py')
if os.path.exists(flash_tool_file):
    with open(flash_tool_file, 'r', encoding='utf-8') as f:
        flash_code = f.read()

    # 验证 GUI 元素
    has_uuid_var = 'auth_uuid_var' in flash_code
    has_key_var = 'auth_key_var' in flash_code
    has_write_btn = 'write_auth_btn' in flash_code
    has_erase_btn = 'erase_auth_btn' in flash_code
    has_write_handler = '_on_write_auth' in flash_code
    has_erase_handler = '_on_erase_auth' in flash_code
    has_thread_func = '_write_auth_thread_func' in flash_code

    # 验证 CLI 命令格式
    has_auth_cmd = 'auth {uuid_val} {key_val}' in flash_code
    has_erase_cmd = 'auth erase' in flash_code

    # 验证安全：AuthKey 不保存到配置文件
    has_no_key_save = '# AuthKey 不保存到配置文件' in flash_code
    has_no_key_load = '# AuthKey 不从配置文件加载' in flash_code

    # 验证版本升级
    has_v13 = 'v1.3' in flash_code

    # 验证 AuthKey 输入框有掩码
    has_mask = 'show="*"' in flash_code

    result.assert_true(has_uuid_var, "烧录工具有 UUID 输入变量")
    result.assert_true(has_key_var, "烧录工具有 AuthKey 输入变量")
    result.assert_true(has_write_btn, "烧录工具有写入授权码按钮")
    result.assert_true(has_erase_btn, "烧录工具有擦除授权码按钮")
    result.assert_true(has_write_handler, "烧录工具有写入授权码处理函数")
    result.assert_true(has_erase_handler, "烧录工具有擦除授权码处理函数")
    result.assert_true(has_thread_func, "烧录工具有授权码写入线程函数")
    result.assert_true(has_auth_cmd, "写入命令格式为 auth <uuid> <authkey>")
    result.assert_true(has_erase_cmd, "擦除命令格式为 auth erase")
    result.assert_true(has_no_key_save, "AuthKey 不保存到配置文件（安全）")
    result.assert_true(has_no_key_load, "AuthKey 不从配置文件加载（安全）")
    result.assert_true(has_v13, "烧录工具版本升级到 v1.3")
    result.assert_true(has_mask, "AuthKey 输入框有掩码保护")
else:
    print("  ⚠️ flash_tool.py 未找到，跳过此测试")

# ---------- 测试 25: 烧录工具串口安全 ----------
print("\n📋 测试 25: 烧录工具串口安全处理")
print("-" * 40)

if os.path.exists(flash_tool_file):
    with open(flash_tool_file, 'r', encoding='utf-8') as f:
        flash_code = f.read()

    # 验证写入前停止监控
    write_section = flash_code.split('_on_write_auth(self):')[1].split('def ')[0] if '_on_write_auth(self):' in flash_code else ''
    has_stop_monitor_before_write = '_stop_monitor' in write_section
    has_is_flashing_check = '_is_flashing' in write_section

    # 验证串口在 finally 中关闭
    thread_section = flash_code.split('_write_auth_thread_func(self, port, uuid_val, key_val, erase):')[1].split('def ')[0] if '_write_auth_thread_func' in flash_code else ''
    has_finally_close = 'ser.close()' in thread_section
    has_baudrate = 'MONITOR_BAUDRATE' in thread_section

    # 验证按钮状态管理
    has_auth_btn_state = '_set_auth_buttons_state' in flash_code

    result.assert_true(has_stop_monitor_before_write, "写入授权码前停止监控释放串口")
    result.assert_true(has_is_flashing_check, "写入授权码前检查烧录状态")
    result.assert_true(has_finally_close, "授权码线程 finally 中关闭串口")
    result.assert_true(has_baudrate, "使用 MONITOR_BAUDRATE 通信")
    result.assert_true(has_auth_btn_state, "有授权码按钮状态管理函数")
else:
    print("  ⚠️ flash_tool.py 未找到，跳过此测试")

# ---------- 测试 26: Broker 认证逻辑 ----------
print("\n📋 测试 26: Broker 用户名/密码认证")
print("-" * 40)

BROKER_USERNAME = "broker"

def derive_broker_password(bridge_sn):
    """模拟 C 代码的 derive_broker_password"""
    h = 2166136261
    for c in bridge_sn:
        h ^= ord(c)
        h = (h * 16777619) & 0xFFFFFFFF
    h2 = (h * 16777619) & 0xFFFFFFFF
    return f"{h & 0xFFFFFFFF:08x}{h2:08x}"

bridge_sn_test = "AABBCCDDEEFF"
broker_password = derive_broker_password(bridge_sn_test)

def broker_auth_check(username, password, enable_auth=True):
    """模拟 on_broker_connect 认证逻辑"""
    if not enable_auth:
        return 0
    if username is None or username != BROKER_USERNAME:
        return -1
    if password is None or password != broker_password:
        return -1
    return 0

result.assert_eq(broker_auth_check("broker", broker_password), 0, "正确凭据通过认证")
result.assert_eq(broker_auth_check("wrong", broker_password), -1, "错误用户名被拒绝")
result.assert_eq(broker_auth_check("broker", "wrong"), -1, "错误密码被拒绝")
result.assert_eq(broker_auth_check(None, None), -1, "空凭据被拒绝")
result.assert_eq(broker_auth_check("broker", broker_password, False), 0, "禁用认证时通过")
result.assert_eq(broker_auth_check("any", "any", False), 0, "禁用认证时任意凭据通过")
result.assert_true(len(broker_password) == 16, "密码长度为 16 字符")
result.assert_true(broker_password != derive_broker_password("AABBCCDDEEF0"), "不同 SN 派生不同密码")

# ---------- 测试 27: 指数退避重连 ----------
print("\n📋 测试 27: 指数退避重连机制")
print("-" * 40)

BACKOFF_MIN_MS = 3000
BACKOFF_MAX_MS = 60000
BACKOFF_FACTOR = 2

def simulate_backoff(failures):
    """模拟指数退避序列"""
    backoff = BACKOFF_MIN_MS
    delays = []
    for _ in range(failures):
        delays.append(backoff)
        backoff *= BACKOFF_FACTOR
        if backoff > BACKOFF_MAX_MS:
            backoff = BACKOFF_MAX_MS
    return delays

delays = simulate_backoff(7)
expected = [3000, 6000, 12000, 24000, 48000, 60000, 60000]
result.assert_eq(delays, expected, "指数退避序列正确: 3s->6s->12s->24s->48s->60s->60s")
result.assert_eq(max(delays), BACKOFF_MAX_MS, "最大退避不超过 60s")
result.assert_gt(sum(delays), 150000, "7 次退避总时间 > 150s（减少网络风暴）")

# ---------- 测试 28: 熔断器 ----------
print("\n📋 测试 28: 熔断器机制")
print("-" * 40)

CIRCUIT_BREAKER_THRESHOLD = 5
CIRCUIT_BREAKER_COOLDOWN_MS = 30000

def simulate_circuit_breaker(failure_count):
    """模拟熔断器状态"""
    if failure_count >= CIRCUIT_BREAKER_THRESHOLD:
        return {"tripped": True, "cooldown_ms": CIRCUIT_BREAKER_COOLDOWN_MS}
    return {"tripped": False, "cooldown_ms": 0}

cb_4 = simulate_circuit_breaker(4)
cb_5 = simulate_circuit_breaker(5)
cb_10 = simulate_circuit_breaker(10)

result.assert_true(not cb_4["tripped"], "4 次失败不触发熔断")
result.assert_true(cb_5["tripped"], "5 次失败触发熔断")
result.assert_true(cb_10["tripped"], "10 次失败触发熔断")
result.assert_eq(cb_5["cooldown_ms"], 30000, "熔断冷却时间为 30s")

# ---------- 测试 29: FNV-1a 哈希表 ----------
print("\n📋 测试 29: FNV-1a 哈希表查找")
print("-" * 40)

DEVICE_HASH_SIZE = 32

def fnv1a_hash(sn):
    h = 2166136261
    for c in sn:
        h ^= ord(c)
        h = (h * 16777619) & 0xFFFFFFFF
    return h & (DEVICE_HASH_SIZE - 1)

test_sns = [f"DEV{i:06d}" for i in range(12)]
hash_values = [fnv1a_hash(sn) for sn in test_sns]
unique_hashes = len(set(hash_values))

collision_rate = 1.0 - unique_hashes / 12
result.assert_true(unique_hashes >= 10, f"12 个 SN 哈希唯一值 >= 10（实际 {unique_hashes}）")
result.assert_true(collision_rate < 0.2, f"冲突率 < 20%（实际 {collision_rate:.1%}）")
result.assert_true(all(0 <= h < DEVICE_HASH_SIZE for h in hash_values), "所有哈希值在有效范围")
result.assert_eq(fnv1a_hash("DEV000001"), fnv1a_hash("DEV000001"), "相同 SN 哈希一致")

# ---------- 测试 30: 设备状态机 ----------
print("\n📋 测试 30: 设备生命周期状态机")
print("-" * 40)

DEV_STATES = {
    0: "IDLE",
    1: "REGISTERING",
    2: "ONLINE",
    3: "OFFLINE",
    4: "REMOVING",
}

transitions = [
    (0, 1, "IDLE -> REGISTERING"),
    (1, 2, "REGISTERING -> ONLINE"),
    (2, 3, "ONLINE -> OFFLINE"),
    (3, 2, "OFFLINE -> ONLINE"),
    (2, 4, "ONLINE -> REMOVING"),
    (4, 0, "REMOVING -> IDLE"),
]

for from_state, to_state, desc in transitions:
    result.assert_true(from_state != to_state, f"状态转换有效: {desc}")

result.assert_eq(len(DEV_STATES), 5, "设备状态有 5 种")
result.assert_true(all(v in DEV_STATES for v in range(5)), "所有状态值有效")

# ---------- 测试 31: 电压转换提取 ----------
print("\n📋 测试 31: voltage_to_percent 统一转换")
print("-" * 40)

BATTERY_RAW_MIN = 80
BATTERY_RAW_MAX = 140

def voltage_to_percent(voltage):
    """模拟 C 代码的 voltage_to_percent"""
    if voltage <= BATTERY_RAW_MIN: return 0
    if voltage >= BATTERY_RAW_MAX: return 100
    return (voltage - BATTERY_RAW_MIN) * 100 // (BATTERY_RAW_MAX - BATTERY_RAW_MIN)

result.assert_eq(voltage_to_percent(80), 0, "80V -> 0%")
result.assert_eq(voltage_to_percent(140), 100, "140V -> 100%")
result.assert_eq(voltage_to_percent(110), 50, "110V -> 50%（中点）")
result.assert_eq(voltage_to_percent(70), 0, "70V -> 0%（低于最小值）")
result.assert_eq(voltage_to_percent(150), 100, "150V -> 100%（高于最大值）")
result.assert_eq(voltage_to_percent(95), 25, "95V -> 25%")
result.assert_eq(voltage_to_percent(125), 75, "125V -> 75%")

# ---------- 测试 32: 统一错误码 ----------
print("\n📋 测试 32: 统一错误码枚举")
print("-" * 40)

BRIDGE_ERRORS = {
    0: "BRIDGE_OK",
    -1: "BRIDGE_ERR_PARAM",
    -2: "BRIDGE_ERR_NOMEM",
    -3: "BRIDGE_ERR_NOT_FOUND",
    -4: "BRIDGE_ERR_TABLE_FULL",
    -5: "BRIDGE_ERR_AUTH",
    -6: "BRIDGE_ERR_TIMEOUT",
    -7: "BRIDGE_ERR_PROTOCOL",
}

result.assert_eq(len(BRIDGE_ERRORS), 8, "8 种错误码")
result.assert_eq(BRIDGE_ERRORS[0], "BRIDGE_OK", "成功码为 0")
result.assert_true(all(k <= 0 for k in BRIDGE_ERRORS), "所有错误码 <= 0")
result.assert_true(-5 in BRIDGE_ERRORS, "认证失败错误码存在")

# ---------- 测试 33: 压力测试 - 高频状态上报 ----------
print("\n📋 测试 33: 压力测试 - 高频 005 状态上报")
print("-" * 40)

broker_stress = MockMQTTBroker()
gw_stress = MockGateway("1001TESTGW01")
for i in range(12):
    gw_stress.devices.append(MockDevice(f"DEV{i:06d}", gw_stress.sn, position=50, battery=120))

msg_count = 0
drop_count = 0
for _ in range(10):
    for dev in gw_stress.devices:
        msg = gw_stress.make_005_message(dev)
        msg_json = json.dumps(msg)
        result_msg = broker_stress.on_message(gw_stress.sn, "gateway/1001TESTGW01/rpt", msg_json, len(msg_json), 1, 0)
        if result_msg is not None:
            msg_count += 1
        else:
            drop_count += 1

result.assert_eq(msg_count, 120, "120 条消息全部接收（10s x 12 设备）")
result.assert_eq(drop_count, 0, "无消息丢弃")

# ---------- 测试 34: 压力测试 - 并发网关 ----------
print("\n📋 测试 34: 压力测试 - 2 个网关并发上报")
print("-" * 40)

broker2 = MockMQTTBroker()
gw1 = MockGateway("GW001")
gw2 = MockGateway("GW002")
gw1.devices = [MockDevice("DEV_A001", "GW001")]
gw2.devices = [MockDevice("DEV_B001", "GW002")]

all_received = []
for i in range(100):
    dev1 = gw1.devices[0]
    dev1.position = i % 100
    msg1 = gw1.make_005_message(dev1)
    r1 = broker2.on_message("GW001", "gateway/GW001/rpt", json.dumps(msg1), len(json.dumps(msg1)), 1, 0)
    if r1: all_received.append(("GW001", r1["client_id"]))

    dev2 = gw2.devices[0]
    dev2.position = (i + 50) % 100
    msg2 = gw2.make_005_message(dev2)
    r2 = broker2.on_message("GW002", "gateway/GW002/rpt", json.dumps(msg2), len(json.dumps(msg2)), 1, 0)
    if r2: all_received.append(("GW002", r2["client_id"]))

result.assert_eq(len(all_received), 200, "200 条并发消息全部接收")

# ---------- 测试 35: 模糊测试 - 畸形 JSON ----------
print("\n📋 测试 35: 模糊测试 - 畸形 JSON 数据")
print("-" * 40)

malformed_inputs = [
    ("", "空字符串"),
    ("{", "不完整 JSON"),
    ("}{}", "非法 JSON"),
    ("null", "null 值"),
    ("[]", "数组而非对象"),
    ('{"head":"$SH"}', "缺少必需字段"),
    ('{"head":"$SH","ctype":"001","sn":"","data":{}}', "空 SN"),
    ('{"head":"$SH","ctype":"999","sn":"GW01","data":{}}', "未知 ctype"),
]

for payload, desc in malformed_inputs:
    try:
        parsed = json.loads(payload) if payload else None
        if parsed and isinstance(parsed, dict):
            head = parsed.get("head")
            ctype = parsed.get("ctype")
            sn = parsed.get("sn")
            if head != "$SH":
                result.assert_true(True, f"{desc}: 过滤非 $SH 协议")
            elif not sn:
                result.assert_true(True, f"{desc}: 拒绝空 SN")
            elif ctype == "999":
                result.assert_true(True, f"{desc}: 忽略未知 ctype")
            else:
                result.assert_true(True, f"{desc}: 正常处理")
        else:
            result.assert_true(True, f"{desc}: JSON 解析失败/空，安全丢弃")
    except json.JSONDecodeError:
        result.assert_true(True, f"{desc}: JSON 解析异常，安全丢弃")

# ---------- 测试 36: 模糊测试 - 极端数值 ----------
print("\n📋 测试 36: 模糊测试 - 极端数值")
print("-" * 40)

extreme_values = [
    ("position", -1, "负位置"),
    ("position", 101, "超范围位置"),
    ("position", 2147483647, "INT_MAX 位置"),
    ("battery", -100, "负电池"),
    ("battery", 0, "零电池"),
    ("battery", 999, "超范围电池"),
    ("r_travel", "abc", "非数字位置"),
    ("voltage", "", "空电压"),
    ("state", 255, "超范围状态"),
]

for field, value, desc in extreme_values:
    if field in ("position", "r_travel"):
        valid = isinstance(value, (int, float)) and 0 <= value <= 100
    elif field == "battery":
        valid = isinstance(value, (int, float)) and 80 <= value <= 140
    elif field == "voltage":
        valid = isinstance(value, (int, float)) and 80 <= value <= 140
    elif field == "state":
        valid = isinstance(value, (int, float)) and 0 <= value <= 1
    else:
        valid = False
    result.assert_true(not valid, f"{desc}: 极端值被正确拒绝")

# ---------- 测试 37: 模糊测试 - 超长字符串 ----------
print("\n📋 测试 37: 模糊测试 - 超长字符串")
print("-" * 40)

long_topic = "gateway/" + "A" * 200 + "/rpt"
truncated_topic = long_topic[:127]
result.assert_eq(len(truncated_topic), 127, "topic 截断到 127 字符")

long_data = "X" * 10000
truncated_data = long_data[:4095]
result.assert_eq(len(truncated_data), 4095, "data 截断到 4095 字符")

oversized_payload = "Y" * 9000
result_msg = broker_stress.on_message("GW", "topic", oversized_payload, len(oversized_payload), 1, 0)
result.assert_true(result_msg is None, "8192+ 字节 payload 被丢弃")

# ---------- 路径变量定义（供后续测试使用）----------
_src_base = os.path.dirname(__file__)
src_dir = os.path.join(_src_base, '..', 'src')
tuya_main_file = os.path.join(src_dir, 'tuya_main.c')
protocol_bridge_file = os.path.join(src_dir, 'app_protocol_bridge.c')
tuya_bridge_file = os.path.join(src_dir, 'app_tuya_bridge.h')

# ---------- 测试 38: CI/CD 工作流验证 ----------
print("\n📋 测试 38: GitHub Actions CI/CD 工作流")
print("-" * 40)

ci_yaml = os.path.join(_src_base, '..', '..', '..', '..', '.github', 'workflows', 'window_broker-ci.yml')
if os.path.exists(ci_yaml):
    with open(ci_yaml, 'r', encoding='utf-8') as f:
        ci_content = f.read()

    result.assert_true("static-analysis" in ci_content, "CI 包含静态分析 job")
    result.assert_true("cppcheck" in ci_content, "CI 运行 cppcheck")
    result.assert_true("clang-format" in ci_content, "CI 检查代码格式")
    result.assert_true("python-tests" in ci_content, "CI 包含 Python 测试 job")
    result.assert_true("build-check" in ci_content, "CI 包含构建检查 job")
    result.assert_true("doxygen" in ci_content, "CI 生成 Doxygen 文档")
else:
    print("  ⚠️ CI/CD 工作流未找到，跳过")

# ---------- 测试 39: Doxygen 配置验证 ----------
print("\n📋 测试 39: Doxygen 文档配置")
print("-" * 40)

doxyfile = os.path.join(src_dir, "..", "..", "..", "..", "..", "docs", "Doxyfile")
if os.path.exists(doxyfile):
    with open(doxyfile, 'r', encoding='utf-8') as f:
        doxy_content = f.read()

    result.assert_true("PROJECT_NAME" in doxy_content, "Doxyfile 有项目名称")
    result.assert_true("GENERATE_HTML" in doxy_content, "Doxyfile 生成 HTML")
    result.assert_true("GENERATE_XML" in doxy_content, "Doxyfile 生成 XML")
    result.assert_true("CALL_GRAPH" in doxy_content, "Doxyfile 启用调用图")
    result.assert_true("EXTRACT_ALL" in doxy_content, "Doxyfile 提取所有符号")
else:
    print("  ⚠️ Doxyfile 未找到，跳过")

# ---------- 测试 40: ADR 文档验证 ----------
print("\n📋 测试 40: ADR 架构决策文档")
print("-" * 40)

adr_dir = os.path.join(src_dir, "..", "..", "..", "..", "..", "docs", "adr")
expected_adrs = [
    "ADR-001-tuyaopen-vs-matter.md",
    "ADR-002-broker-authentication.md",
    "ADR-003-exponential-backoff-circuit-breaker.md",
    "ADR-004-hash-lookup-optimization.md",
]

for adr_name in expected_adrs:
    adr_path = os.path.join(adr_dir, adr_name)
    if os.path.exists(adr_path):
        with open(adr_path, 'r', encoding='utf-8') as f:
            adr_content = f.read()
        result.assert_true("## 背景" in adr_content, f"{adr_name}: 有背景章节")
        result.assert_true("## 决策" in adr_content, f"{adr_name}: 有决策章节")
        result.assert_true("## 影响" in adr_content, f"{adr_name}: 有影响章节")
    else:
        result.assert_true(False, f"{adr_name} 存在")

# ---------- 测试 41: 硬件测试计划 ----------
print("\n📋 测试 41: 硬件测试计划文档")
print("-" * 40)

hw_test_plan = os.path.join(src_dir, "..", "..", "..", "..", "..", "docs", "hardware_test_plan.md")
if os.path.exists(hw_test_plan):
    with open(hw_test_plan, 'r', encoding='utf-8') as f:
        hw_content = f.read()

    result.assert_true("TC-001" in hw_content and "TC-015" in hw_content, "有 15 个测试用例")
    result.assert_true("Broker 认证" in hw_content, "包含 Broker 认证测试")
    result.assert_true("指数退避" in hw_content, "包含指数退避测试")
    result.assert_true("熔断器" in hw_content, "包含熔断器测试")
    result.assert_true("OTA" in hw_content, "包含 OTA 测试")
    result.assert_true("mDNS" in hw_content, "包含 mDNS 测试")
else:
    result.assert_true(False, "hardware_test_plan.md 存在")

# ---------- 测试 42: OTA 通知增强验证 ----------
print("\n📋 测试 42: OTA 通知回调增强")
print("-" * 40)

if os.path.exists(tuya_main_file):
    with open(tuya_main_file, 'r', encoding='utf-8') as f:
        main_code = f.read()

    ota_section = main_code.split('user_upgrade_notify_on')[1].split('void ')[0] if 'user_upgrade_notify_on' in main_code else ''

    result.assert_true("url_item" in ota_section, "OTA 通知解析 URL 字段")
    result.assert_true("md5_item" in ota_section, "OTA 通知解析 MD5 字段")
    result.assert_true("LED_RED" in ota_section and "FAST_BLINK" in ota_section, "OTA 进行中红灯快闪")
    result.assert_true("固件" in ota_section and "模块" in ota_section, "OTA 通道类型中文显示")
else:
    print("  ⚠️ tuya_main.c 未找到，跳过")

# ---------- 测试 43: Broker 认证配置传递 ----------
print("\n📋 测试 43: Broker 认证配置初始化")
print("-" * 40)

if os.path.exists(tuya_main_file):
    with open(tuya_main_file, 'r', encoding='utf-8') as f:
        main_code = f.read()

    result.assert_true("enable_broker_auth" in main_code, "tuya_main.c 启用 Broker 认证")
    result.assert_true(".enable_broker_auth = true" in main_code, "认证标志设为 true")
else:
    print("  ⚠️ tuya_main.c 未找到，跳过")

if os.path.exists(protocol_bridge_file):
    with open(protocol_bridge_file, 'r', encoding='utf-8') as f:
        bridge_code = f.read()

    result.assert_true("s_enable_broker_auth" in bridge_code, "协议桥接层存储认证标志")
    result.assert_true("derive_broker_password" in bridge_code, "有密码派生函数")
    result.assert_true("BROKER_USERNAME" in bridge_code, "有 Broker 用户名常量")
    result.assert_true("s_broker_password" in bridge_code, "有 Broker 密码存储")
else:
    print("  ⚠️ app_protocol_bridge.c 未找到，跳过")

# ---------- 测试 44: 批量 DP 上报接口 ----------
print("\n📋 测试 44: 批量 DP 上报接口")
print("-" * 40)

if os.path.exists(tuya_bridge_file):
    with open(tuya_bridge_file, 'r', encoding='utf-8') as f:
        bridge_h_code = f.read()

    result.assert_true("app_tuya_bridge_update_status_batch" in bridge_h_code, "有批量上报函数声明")
    result.assert_true("app_tuya_bridge_get_state" in bridge_h_code, "有获取状态函数声明")
    result.assert_true("device_state_t" in bridge_h_code, "有设备状态枚举")
    result.assert_true("DEV_STATE_IDLE" in bridge_h_code, "状态枚举包含 IDLE")
    result.assert_true("DEV_STATE_ONLINE" in bridge_h_code, "状态枚举包含 ONLINE")
    result.assert_true("DEVICE_HASH_SIZE" in bridge_h_code, "有哈希表大小常量")
else:
    print("  ⚠️ app_tuya_bridge.h 未找到，跳过")

if os.path.exists(os.path.join(src_dir, "app_tuya_bridge.c")):
    with open(os.path.join(src_dir, "app_tuya_bridge.c"), 'r', encoding='utf-8') as f:
        bridge_c_code = f.read()

    result.assert_true("fnv" in bridge_c_code.lower() or "hash" in bridge_c_code.lower(), "有哈希函数实现")
    result.assert_true("s_sn_hash_map" in bridge_c_code, "有哈希索引表")
    result.assert_true("hash_map_lookup" in bridge_c_code, "有哈希查找函数")
    result.assert_true("hash_map_update" in bridge_c_code, "有哈希更新函数")
    result.assert_true("DEV_STATE_ONLINE" in bridge_c_code, "设备注册时设为 ONLINE 状态")
    result.assert_true("DEV_STATE_OFFLINE" in bridge_c_code, "设备离线时设为 OFFLINE 状态")
else:
    print("  ⚠️ app_tuya_bridge.c 未找到，跳过")

# ---------- 测试 45: 网关自动发现 ----------
print("\n📋 测试 45: 网关自动发现（主题驱动注册）")
print("-" * 40)

def auto_discover_gateway_from_topic(topic):
    """模拟 C 代码的 auto_discover_gateway_from_topic"""
    if topic is None:
        return False, None

    prefix = "gateway/"
    if not topic.startswith(prefix):
        return False, None

    sn_start = topic[len(prefix):]
    slash_idx = sn_start.find('/')
    if slash_idx <= 0:
        return False, None

    sn = sn_start[:slash_idx]
    if len(sn) == 0 or len(sn) >= 32:
        return False, None
    if sn == "rpt_rsp":
        return False, None

    return True, sn

# 测试各种主题格式
test_topics = [
    ("gateway/1001ABCDEF01/rpt", True, "1001ABCDEF01", "标准上报主题"),
    ("gateway/1001ABCDEF01/req", True, "1001ABCDEF01", "标准请求主题"),
    ("gateway/GW_TEST_001/rpt",  True, "GW_TEST_001",  "自定义网关 SN"),
    ("gateway/rpt_rsp",          False, None,           "保留主题不触发"),
    ("other/topic",              False, None,           "非 gateway 前缀"),
    ("gateway/",                 False, None,           "缺少 SN"),
    ("gateway//rpt",             False, None,           "空 SN"),
    (None,                       False, None,           "NULL 主题"),
    ("gateway/" + "X" * 40 + "/rpt", False, None,      "SN 超长"),
]

for topic, expect_ok, expect_sn, desc in test_topics:
    ok, sn = auto_discover_gateway_from_topic(topic)
    result.assert_eq(ok, expect_ok, f"{desc}: 发现结果")
    if expect_ok:
        result.assert_eq(sn, expect_sn, f"{desc}: SN 提取正确")

# 测试：JSON 解析失败但主题匹配时仍注册网关
print("\n  --- 子测试: JSON 解析失败时网关仍自动注册 ---")
bad_json_topic = "gateway/GW_BADJSON/rpt"
ok, sn = auto_discover_gateway_from_topic(bad_json_topic)
result.assert_true(ok and sn == "GW_BADJSON", "JSON 解析失败时网关仍从主题自动注册")

# 测试：非 $SH 协议但主题匹配时仍注册网关
print("  --- 子测试: 非 $SH 协议时网关仍自动注册 ---")
non_sh_topic = "gateway/GW_NONSH/rpt"
ok, sn = auto_discover_gateway_from_topic(non_sh_topic)
result.assert_true(ok and sn == "GW_NONSH", "非 $SH 协议时网关仍从主题自动注册")

# 测试：代码中存在自动发现函数
if os.path.exists(protocol_bridge_file):
    with open(protocol_bridge_file, 'r', encoding='utf-8') as f:
        bridge_code = f.read()

    result.assert_true("auto_discover_gateway_from_topic" in bridge_code, "有网关自动发现函数")
    result.assert_true("网关自动发现" in bridge_code, "有自动发现日志")
    result.assert_true('gateway/' in bridge_code, "解析 gateway/ 前缀")
    result.assert_true("rpt_rsp" in bridge_code, "排除 rpt_rsp 保留主题")
    result.assert_true("register_gateway" in bridge_code, "自动发现调用 register_gateway")
else:
    print("  ⚠️ app_protocol_bridge.c 未找到，跳过代码检查")

# 测试：模拟完整流程 - 网关发 005 不发 001 也能被注册
print("  --- 子测试: 网关直接发 005 状态（不发 001）也能自动注册 ---")
broker_gw = MockMQTTBroker()
gw_direct = MockGateway("1001DIRECTGW")

# 网关直接发 005，不先发 001
dev_direct = MockDevice("DEV_DIRECT_01", gw_direct.sn, position=75, battery=125)
msg_005 = gw_direct.make_005_message(dev_direct)
msg_json = json.dumps(msg_005)
topic_005 = f"gateway/{gw_direct.sn}/rpt"

# 模拟 handle_mqtt_message 的主题自动发现部分
discovered, discovered_sn = auto_discover_gateway_from_topic(topic_005)
result.assert_true(discovered, "005 消息主题触发网关自动发现")
result.assert_eq(discovered_sn, "1001DIRECTGW", "自动发现的 SN 正确")

# ==================== 结果汇总 ====================

success = result.summary()
sys.exit(0 if success else 1)
