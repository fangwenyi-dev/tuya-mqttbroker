/**
 * @file app_button.c
 * @brief 按键功能模块实现（TuyaOpen 版本）
 *
 * 使用 TuyaOpen TKL GPIO API 检测按键，支持多击和长按。
 *
 * 相比 Matter 版本的变化：
 * - 移除 5击重置 Matter（TuyaOpen 无 Matter 概念）
 * - 长按改为调用 app_protocol_bridge_reset_tuya() 重置涂鸦配网
 * - 使用 TAL API（tal_system_gettick, tal_thread_create）替代 ESP-IDF API
 */

#include "app_button.h"
#include "app_protocol_bridge.h"
#include "app_led.h"
#include "tal_api.h"
#include "tal_log.h"
#include "tkl_gpio.h"
#include <string.h>

static const char *TAG = "app_button";

/* 按键检测参数 */
#define BUTTON_DEBOUNCE_MS      50     /* 消抖时间 */
#define BUTTON_MULTI_CLICK_GAP  400    /* 多击间隔（超时后判定点击次数） */
#define BUTTON_LONG_PRESS_MS    5000   /* 长按阈值 5 秒 */

/* 内部状态 */
static int s_gpio_num = -1;
static THREAD_HANDLE s_button_thread = NULL;
static volatile bool s_button_running = false;

/**
 * @brief 按键检测 task
 *
 * 轮询 GPIO 电平，检测按下/释放边沿，统计点击次数和长按。
 */
static void button_task(void *arg)
{
    PR_INFO("按键检测 task 已启动: GPIO=%d", s_gpio_num);

    bool last_pressed = false;
    uint32_t press_start = 0;
    int click_count = 0;
    uint32_t last_release_time = 0;

    while (s_button_running) {
        /* 读取 GPIO 电平（低电平=按下） */
        TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
        tkl_gpio_read(s_gpio_num, &level);

        bool pressed = (level == TUYA_GPIO_LEVEL_LOW);
        uint32_t now = tal_system_gettick();

        if (pressed && !last_pressed) {
            /* 按下边沿（消抖） */
            press_start = now;
        } else if (!pressed && last_pressed) {
            /* 释放边沿 */
            uint32_t press_duration = now - press_start;
            if (press_duration >= BUTTON_DEBOUNCE_MS) {
                if (press_duration >= BUTTON_LONG_PRESS_MS) {
                    /* 长按：直接处理，忽略之前的点击计数 */
                    PR_INFO("长按 5 秒 → 重置涂鸦配网");
                    app_led_set(LED_RED, LED_MODE_FAST_BLINK, 0);
                    app_protocol_bridge_reset_tuya();
                    click_count = 0;
                } else {
                    /* 短按：累计点击次数 */
                    click_count++;
                    last_release_time = now;
                }
            }
        }

        /* 多击超时判定 */
        if (click_count > 0 && (now - last_release_time) > BUTTON_MULTI_CLICK_GAP) {
            switch (click_count) {
            case 2:
                PR_INFO("2击 → 启动 LoRa 配对模式");
                app_led_set(LED_GREEN, LED_MODE_FAST_BLINK, 0);
                app_protocol_bridge_start_pairing();
                /* 3 秒后恢复绿灯状态 */
                tal_system_sleep(3000);
                app_led_off(LED_GREEN);
                break;
            case 3:
                PR_INFO("3击 → 删除所有 LoRa 子设备");
                app_led_set(LED_RED, LED_MODE_FAST_BLINK, 0);
                app_protocol_bridge_delete_all_devices();
                tal_system_sleep(1000);
                app_led_off(LED_RED);
                break;
            default:
                PR_DEBUG("点击 %d 次（无绑定操作）", click_count);
                break;
            }
            click_count = 0;
        }

        last_pressed = pressed;
        tal_system_sleep(10); /* 10ms 轮询 */
    }

    PR_INFO("按键检测 task 退出");
    s_button_thread = NULL;
    tal_thread_delete(NULL);
}

int app_button_init(int gpio_num)
{
    if (gpio_num < 0) {
        PR_ERR("无效的 GPIO 编号: %d", gpio_num);
        return -1;
    }

    s_gpio_num = gpio_num;

    /* 配置 GPIO 为输入模式（上拉） */
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_MODE_INPUT,
        .pull = TUYA_GPIO_PULLUP,
    };
    int rt = tkl_gpio_init(s_gpio_num, gpio_cfg);
    if (rt != 0) {
        PR_ERR("GPIO %d 初始化失败: %d", gpio_num, rt);
        return -1;
    }

    s_button_running = true;

    /* 创建按键检测 task */
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "button_task";

    rt = tal_thread_create_and_start(&s_button_thread, NULL, NULL,
                                      button_task, NULL, &thrd_param);
    if (rt != 0) {
        PR_ERR("创建按键检测 task 失败: %d", rt);
        s_button_running = false;
        return -1;
    }

    PR_INFO("按键模块已初始化: GPIO=%d", gpio_num);
    return 0;
}
