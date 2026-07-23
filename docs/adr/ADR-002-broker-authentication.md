# ADR-002: 本地 MQTT Broker 认证方案

**日期**: 2025-07-23  
**状态**: 已采纳  
**决策者**: 项目团队

## 背景

本地 MQTT Broker（mosquitto）默认允许匿名连接。在局域网环境中，任何设备均可连接 Broker 发布/订阅消息，存在安全风险：
1. 恶意设备可注入虚假 $SH 协议消息
2. 可嗅探所有 LoRa 设备状态
3. 可下发控制命令操作开窗器

## 决策

采用基于 `bridge_sn`（MAC 地址）派生的用户名/密码认证方案：
1. **用户名**：固定为 `broker`
2. **密码**：从 `bridge_sn` 通过 FNV-1a 哈希确定性派生（16 字符 hex）
3. **Broker 端**：`on_broker_connect` 回调验证用户名/密码
4. **客户端端**：TuyaOpen MQTT 客户端连接时携带凭据

## 设计考量

### 为什么不用 TLS 证书认证？
- 嵌入式设备生成/存储证书复杂
- TLS 握手增加连接延迟（~2s）
- 局域网环境下密码认证已提供足够保护
- 可作为后续 P2 级增强叠加使用

### 为什么密码从 bridge_sn 派生而非随机生成？
- 确定性派生无需额外存储（Flash 空间有限）
- 每台设备密码不同（MAC 唯一）
- LoRa 网关可通过 mDNS 发现 Broker 后，从已知的 bridge_sn 推导密码
- 无需网络传输密码（网关和 Broker 独立派生）

## 实现

```c
// 密码派生（FNV-1a 哈希）
static void derive_broker_password(void) {
    uint32_t h = 2166136261U;
    for (int i = 0; s_bridge_sn[i] && i < 32; i++) {
        h ^= (uint8_t)s_bridge_sn[i];
        h *= 16777619U;
    }
    snprintf(s_broker_password, sizeof(s_broker_password), "%08x%02x", ...);
}

// Broker 端认证回调
static int on_broker_connect(...) {
    if (s_enable_broker_auth) {
        if (strcmp(username, "broker") != 0) return -1;
        if (memcmp(password, s_broker_password, password_len) != 0) return -1;
    }
    return 0;
}
```

## 影响

- 新增 `enable_broker_auth` 配置项
- LoRa 网关需更新固件以携带 Broker 凭据
- 安全性提升：阻止未授权设备连接 Broker

## 验证

- 错误密码连接被拒绝
- 正确密码连接正常
- 速率限制仍生效（DoS 防护）
