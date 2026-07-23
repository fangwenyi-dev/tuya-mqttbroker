# ADR-001: 选择 TuyaOpen SDK 替代 Matter 协议

**日期**: 2025-01-15  
**状态**: 已采纳  
**决策者**: 项目团队

## 背景

原方案使用 ESP-Matter 协议接入 Apple HomeKit。但 Matter 协议存在以下问题：
1. 内存占用大（Matter 协议栈 + ESP-Matter 库 > 500KB）
2. 端点创建延迟（HomeKit ReportData 大小限制）
3. 配网复杂（Matter 配网不如涂鸦原生方式成熟）
4. 无内置 OTA（需手动实现）

## 决策

改用 TuyaOpen SDK 接入涂鸦云，原因：
1. **内存优化**：无 Matter 协议栈，内存占用减少 ~300KB
2. **配网成熟**：BLE+AP 配网为涂鸦原生方式，用户体验好
3. **内置功能**：TuyaOpen SDK 自带 LAN 控制、OTA、DP 收发
4. **DP 虚拟化**：涂鸦 DP 是虚拟的，无 HomeKit ReportData 限制
5. **无需 StackLock**：TuyaOpen SDK 内部管理线程安全

## 影响

- 移除 `app_matter_bridge.*` 相关代码
- 新增 `app_tuya_bridge.*` 和 `app_protocol_bridge.*`
- DP 分配方案：每设备 4 个 DP（位置、控制、电池、风锁模式）
- mDNS 主机名统一为 `matter-broker.local`（兼容现有网关）

## 验证

- 编译通过，内存占用减少 ~280KB
- BLE+AP 配网正常
- DP 收发延迟 < 500ms
- OTA 升级流程正常
