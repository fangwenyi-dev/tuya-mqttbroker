# 智能开窗器 LoRa 网关桥接器 (TuyaOpen)

基于 TuyaOpen SDK 的 ESP32-S3 智能开窗器网关桥接器，替代原 Matter 方案。

> **完整使用说明请参阅 [使用说明.md](使用说明.md)**（含 API 适配详情、构建步骤、上线评估、故障排除）

## 架构概述

```
LoRa 网关 ──MQTT($SH)──→ mosquitto Broker ──→ 协议桥接层 ──→ 涂鸦 DP ──→ 涂鸦云
                          (本地 1883)         (app_protocol_bridge)  (app_tuya_bridge)   ↓
                                                                      涂鸦 App
```

## DP 分配方案（每个开窗器 4 个 DP，最多 12 个设备）

| DP ID | 功能 | 类型 | 值 |
|-------|------|------|-----|
| N*4+1 | 位置状态 | Value | 0-100 (0=关闭, 100=打开) |
| N*4+2 | 控制命令 | Enum | 0=停止, 1=打开, 2=关闭, **3=内倒** |
| N*4+3 | 电池百分比 | Value | 0-100 |
| N*4+4 | 风锁模式 | Enum | 0=内倒模式, 1=平开模式 |

**内倒按钮**：DP control=3 → $SH w_travel value=200（仅 SN 前缀 "5005" 的设备支持）
**风锁模式**：DP wind_lock → $SH rwp_wind_lock_mode（仅 SN 前缀 "5005" 的设备支持）

## 构建方法

### 1. 环境准备

```bash
# 克隆 TuyaOpen SDK
git clone https://github.com/tuya/TuyaOpen.git
cd TuyaOpen

# 安装 TuyaOpen CLI
python -m pip install -r tools/requirements.txt
```

### 2. 复制应用到 TuyaOpen 目录

```bash
# 将本项目复制到 TuyaOpen/apps/tuya_cloud/ 下
cp -r /path/to/tuya-mqttbroker apps/tuya_cloud/window_broker
```

### 3. 配置授权信息

```bash
# 复制授权模板并填入真实 UUID/AuthKey
cp src/tuya_config_secrets.h.example src/tuya_config_secrets.h
# 编辑 src/tuya_config_secrets.h，填入 PID/UUID/AuthKey
```

### 4. 添加 mosquitto 组件

将 mosquitto broker 组件复制到应用的 `components` 目录：

```bash
mkdir -p components
cp -r /path/to/esp-mqttbroker/components/espressif__mosquitto components/
```

### 5. 编译和烧录

```bash
# 选择 ESP32 平台和 ESP32-S3 芯片
tos.py config

# 编译
tos.py build

# 烧录
tos.py flash
```

## 按键功能

| 操作 | 功能 |
|------|------|
| 2击 | 启动 LoRa 配对模式（绿灯快闪） |
| 3击 | 删除所有 LoRa 子设备（红灯快闪） |
| 长按 5 秒 | 重置涂鸦配网（清除 WiFi + 云绑定） |

## LED 指示

| 颜色 | 模式 | 含义 |
|------|------|------|
| 蓝灯 | 慢闪 | 等待配网 |
| 蓝灯 | 常亮 | 已连接涂鸦云 |
| 绿灯 | 单闪 | LoRa 通信收发 |
| 绿灯 | 快闪 | LoRa 配对模式中 |
| 红灯 | 快闪 | 删除操作进行中 |

## 相比 Matter 方案的改进

1. **内倒按钮支持**：新增 control=3 → value=200 映射
2. **风锁模式 DP**：新增独立 wind_lock DP（原 Matter 方案遗漏了 005 中的 rwp_wind_lock_mode）
3. **位置无需反转**：涂鸦 DP 0=关闭 与 LoRa 一致
4. **BLE+AP 配网**：涂鸦原生配网方式，比 Matter 配网更成熟
5. **内置 LAN 控制**：涂鸦 SDK 自带局域网控制
6. **内存更小**：无 Matter 协议栈
7. **开发更简单**：无 StackLock、无端点管理、无 QueueSet
