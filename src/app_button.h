/**
 * @file app_button.h
 * @brief 按键功能模块（TuyaOpen 版本）
 *
 * 支持：
 * - 2击：启动 LoRa 配对模式
 * - 3击：删除所有 LoRa 子设备（发送解绑命令）
 * - 长按 5 秒：重置涂鸦配网（清除 WiFi + 云绑定，重新进入配网模式）
 *
 * 相比 Matter 版本的变化：
 * - 移除 5击重置 Matter（TuyaOpen 无 Matter 概念）
 * - 长按改为调用 tuya_iot_reset 重置涂鸦配网
 * - 使用 TuyaOpen TAL API 替代 ESP-IDF GPIO API
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化按键模块
 *
 * 配置 GPIO 中断，启动按键检测 task。
 *
 * @param gpio_num 按键 GPIO 编号（默认 GPIO0，低电平有效）
 * @return 0 成功，-1 失败
 */
int app_button_init(int gpio_num);

#ifdef __cplusplus
}
#endif
