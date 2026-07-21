/**
 * @file app_led.c
 * @brief LED 指示灯模块实现（TuyaOpen 版本）
 *
 * 使用 TuyaOpen TKL GPIO 驱动 LED。
 *
 * 单颗 LED 优先级显示策略：
 * - 每个"逻辑颜色"（蓝/绿/红）独立维护状态
 * - 每个 tick 根据优先级选出当前应显示的颜色
 * - 优先级：FLASH(4) > FAST_BLINK(3) > SLOW_BLINK(2) > ON(1) > OFF(0)
 * - 同模式时颜色优先级：RED(3) > GREEN(2) > BLUE(1)
 * - 闪烁/单闪的"灭"相位期间，该颜色不参与显示竞争
 *
 * 线程安全：app_led_set / app_led_off / app_led_flash 使用 TAL mutex 保护。
 *
 * 注意：单颗 LED 只能显示一种颜色。如果硬件使用 WS2812（幻彩 LED），
 * 可替换 ws2812_send 函数实现 RGB 驱动。当前版本使用单色 LED（GPIO 直驱）。
 */

#include "app_led.h"
#include "tal_api.h"
#include "tal_log.h"
#include "tkl_gpio.h"

static const char *TAG = "app_led";

#define LED_TASK_STACK      2048
#define LED_TASK_PRIORITY   THREAD_PRIO_1
#define LED_TICK_MS         50

#define FAST_BLINK_PERIOD_MS  200
#define SLOW_BLINK_PERIOD_MS  1000
#define FLASH_DURATION_MS     50

typedef struct {
    led_mode_t mode;
    uint32_t duration_ms;
    uint32_t start_time_ms;
    bool valid;
} led_state_t;

static led_state_t s_leds[LED_COLOR_COUNT] = {0};
static MUTEX_HANDLE s_led_mutex = NULL;
static THREAD_HANDLE s_led_thread = NULL;
static volatile bool s_led_running = false;
static int s_gpio_num = -1;
static bool s_initialized = false;

/**
 * @brief 设置 LED 物理状态（开/关）
 */
static void led_physical_set(bool on)
{
    if (s_gpio_num < 0) return;
    tkl_gpio_write(s_gpio_num, on ? TUYA_GPIO_LEVEL_LOW : TUYA_GPIO_LEVEL_HIGH);
}

/**
 * @brief 计算指定颜色在当前时刻是否应该亮起
 */
static bool led_should_be_on(led_color_t color, uint32_t now_ms)
{
    if (!s_leds[color].valid || s_leds[color].mode == LED_MODE_OFF) return false;

    uint32_t elapsed = now_ms - s_leds[color].start_time_ms;

    /* 持续时间检查 */
    if (s_leds[color].duration_ms > 0 && elapsed >= s_leds[color].duration_ms) {
        s_leds[color].mode = LED_MODE_OFF;
        s_leds[color].valid = false;
        return false;
    }

    switch (s_leds[color].mode) {
    case LED_MODE_ON:
        return true;
    case LED_MODE_FAST_BLINK:
        return ((elapsed / (FAST_BLINK_PERIOD_MS / 2)) % 2) == 0;
    case LED_MODE_SLOW_BLINK:
        return ((elapsed / (SLOW_BLINK_PERIOD_MS / 2)) % 2) == 0;
    case LED_MODE_FLASH:
        return elapsed < FLASH_DURATION_MS;
    default:
        return false;
    }
}

/**
 * @brief 获取模式优先级数值（越大越高）
 */
static int mode_priority(led_mode_t mode)
{
    switch (mode) {
    case LED_MODE_FLASH:       return 4;
    case LED_MODE_FAST_BLINK:  return 3;
    case LED_MODE_SLOW_BLINK:  return 2;
    case LED_MODE_ON:          return 1;
    default:                   return 0;
    }
}

/**
 * @brief LED 刷新 task
 *
 * 每 50ms 刷新一次，按优先级选择当前应显示的颜色。
 */
static void led_task(void *arg)
{
    PR_INFO("LED 刷新 task 已启动: GPIO=%d", s_gpio_num);

    while (s_led_running) {
        uint32_t now = tal_system_gettick();

        tal_mutex_lock(s_led_mutex);

        /* 按优先级选出当前应该亮起的颜色 */
        led_color_t best_color = LED_COLOR_COUNT;
        int best_priority = 0;
        int best_color_priority = 0;

        for (int c = 0; c < LED_COLOR_COUNT; c++) {
            if (!led_should_be_on((led_color_t)c, now)) continue;

            int mp = mode_priority(s_leds[c].mode);
            int cp = c; /* 颜色优先级：RED(2) > GREEN(1) > BLUE(0) */

            if (mp > best_priority || (mp == best_priority && cp > best_color_priority)) {
                best_priority = mp;
                best_color_priority = cp;
                best_color = (led_color_t)c;
            }
        }

        tal_mutex_unlock(s_led_mutex);

        /* 设置 LED 物理状态 */
        led_physical_set(best_color < LED_COLOR_COUNT);

        tal_system_sleep(LED_TICK_MS);
    }

    PR_INFO("LED 刷新 task 退出");
    s_led_thread = NULL;
    tal_thread_delete(NULL);
}

int app_led_init(int gpio_num)
{
    if (gpio_num < 0) {
        PR_WARN("LED GPIO 未配置，LED 功能禁用");
        return 0;
    }

    s_gpio_num = gpio_num;

    /* 配置 GPIO 为输出模式 */
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_MODE_OUTPUT,
        .pull = TUYA_GPIO_FLOATING,
    };
    int rt = tkl_gpio_init(s_gpio_num, gpio_cfg);
    if (rt != 0) {
        PR_ERR("LED GPIO %d 初始化失败: %d", gpio_num, rt);
        return -1;
    }

    /* 初始关闭 LED */
    led_physical_set(false);

    /* 创建互斥锁 */
    if (s_led_mutex == NULL) {
        if (tal_mutex_create_init(&s_led_mutex) != OPRT_OK) {
            PR_ERR("创建 LED mutex 失败");
            return -1;
        }
    }

    s_led_running = true;
    s_initialized = true;

    /* 创建 LED 刷新 task */
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = LED_TASK_STACK;
    thrd_param.priority = LED_TASK_PRIORITY;
    thrd_param.thrdname = "led_task";

    rt = tal_thread_create_and_start(&s_led_thread, NULL, NULL,
                                      led_task, NULL, &thrd_param);
    if (rt != 0) {
        PR_ERR("创建 LED task 失败: %d", rt);
        s_led_running = false;
        return -1;
    }

    PR_INFO("LED 模块已初始化: GPIO=%d", gpio_num);
    return 0;
}

void app_led_set(led_color_t color, led_mode_t mode, uint32_t duration_ms)
{
    if (!s_initialized || color >= LED_COLOR_COUNT) return;

    tal_mutex_lock(s_led_mutex);
    s_leds[color].mode = mode;
    s_leds[color].duration_ms = duration_ms;
    s_leds[color].start_time_ms = tal_system_gettick();
    s_leds[color].valid = true;
    tal_mutex_unlock(s_led_mutex);
}

void app_led_off(led_color_t color)
{
    if (!s_initialized || color >= LED_COLOR_COUNT) return;

    tal_mutex_lock(s_led_mutex);
    s_leds[color].mode = LED_MODE_OFF;
    s_leds[color].valid = false;
    tal_mutex_unlock(s_led_mutex);
}

void app_led_flash(led_color_t color)
{
    if (!s_initialized || color >= LED_COLOR_COUNT) return;

    tal_mutex_lock(s_led_mutex);

    /* 快闪/慢闪模式时不干扰 */
    if (s_leds[color].valid &&
        (s_leds[color].mode == LED_MODE_FAST_BLINK || s_leds[color].mode == LED_MODE_SLOW_BLINK)) {
        tal_mutex_unlock(s_led_mutex);
        return;
    }

    s_leds[color].mode = LED_MODE_FLASH;
    s_leds[color].duration_ms = 0;
    s_leds[color].start_time_ms = tal_system_gettick();
    s_leds[color].valid = true;

    tal_mutex_unlock(s_led_mutex);
}
