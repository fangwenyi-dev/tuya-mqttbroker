# ADR-003: 指数退避重连与熔断机制

**日期**: 2025-07-23  
**状态**: 已采纳  
**决策者**: 项目团队

## 背景

MQTT 客户端断线后使用固定 3 秒重连间隔。在网络不稳定或 Broker 宕机时：
1. 固定间隔重连导致不必要的网络风暴
2. 无失败计数，持续重连浪费 CPU 和电量
3. 无熔断保护，极端情况下可能无限重连

## 决策

实现指数退避 + 熔断器双重保护：

### 指数退避
- 初始退避：3s（`MQTT_BACKOFF_MIN_MS`）
- 退避倍数：2x（`MQTT_BACKOFF_FACTOR`）
- 最大退避：60s（`MQTT_BACKOFF_MAX_MS`）
- 序列：3s → 6s → 12s → 24s → 48s → 60s → 60s → ...

### 熔断器
- 阈值：连续失败 5 次（`CIRCUIT_BREAKER_THRESHOLD`）
- 冷却：30s（`CIRCUIT_BREAKER_COOLDOWN_MS`）
- 熔断期间不尝试连接，冷却后重置退避

## 实现

```c
uint32_t backoff_ms = MQTT_BACKOFF_MIN_MS;
int consecutive_failures = 0;
uint32_t circuit_breaker_until_ms = 0;

// 连接失败时
consecutive_failures++;
backoff_ms *= MQTT_BACKOFF_FACTOR;
if (backoff_ms > MQTT_BACKOFF_MAX_MS) backoff_ms = MQTT_BACKOFF_MAX_MS;

// 熔断触发
if (consecutive_failures >= CIRCUIT_BREAKER_THRESHOLD) {
    circuit_breaker_until_ms = now + CIRCUIT_BREAKER_COOLDOWN_MS;
}

// 连接成功时
consecutive_failures = 0;
backoff_ms = MQTT_BACKOFF_MIN_MS;
circuit_breaker_until_ms = 0;
```

## 影响

- 减少网络风暴：最坏情况下 60s 才重连一次
- 保护系统资源：熔断期间完全停止连接尝试
- 自动恢复：连接成功后立即重置退避参数
- 日志可观测：退避时间和失败次数完整记录

## 验证

- 模拟 Broker 宕机，验证退避间隔正确增长
- 模拟 5 次连续失败，验证熔断器触发
- 模拟 Broker 恢复，验证连接成功后重置
