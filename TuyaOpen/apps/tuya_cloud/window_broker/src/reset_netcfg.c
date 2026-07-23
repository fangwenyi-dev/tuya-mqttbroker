/**
 * @file reset_netcfg.c
 * @brief 重置配网功能模块实现
 *
 * 通过 KV 存储记录连续上电次数，当短时间内连续上电 3 次时触发设备重置。
 * 此实现从 TuyaOpen SDK switch_demo 示例移植，完全使用 TAL API。
 *
 * 工作原理：
 * 1. 每次启动调用 reset_netconfig_start()，读取并递增计数器
 * 2. 启动 5 秒定时器，超时后清零计数器
 * 3. 调用 reset_netconfig_check()，如果计数器 >= 3，触发 tuya_iot_reset()
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "reset_netcfg.h"
#include "tal_api.h"
#include "tuya_iot.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define RESET_NETCNT_NAME "rst_cnt"
#define RESET_NETCNT_MAX  3

/***********************************************************
********************function define*************************
***********************************************************/

static int reset_count_read(uint8_t *count)
{
    int rt = OPRT_OK;

    uint8_t *read_buf = NULL;
    size_t read_len;

    TUYA_CALL_ERR_RETURN(tal_kv_get(RESET_NETCNT_NAME, &read_buf, &read_len));
    if (read_len == 0 || read_buf == NULL) {
        PR_WARN("reset count KV exists but empty, treat as 0");
        *count = 0;
        if (read_buf != NULL) {
            tal_kv_free(read_buf);
        }
        return OPRT_OK;
    }
    *count = read_buf[0];

    PR_DEBUG("reset count is %d", *count);

    if (NULL != read_buf) {
        tal_kv_free(read_buf);
        read_buf = NULL;
    }

    return rt;
}

static int reset_count_write(uint8_t count)
{
    PR_DEBUG("reset count write %d", count);
    return tal_kv_set(RESET_NETCNT_NAME, &count, 1);
}

static void reset_netconfig_timer(TIMER_ID timer_id, void *arg)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear!");
}

static OPERATE_RET __reset_netconfig_clear(void *data)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear by reset event!");
    return OPRT_OK;
}

int reset_netconfig_check(void)
{
    int rt = OPRT_OK;
    uint8_t rst_cnt = 0;

    TUYA_CALL_ERR_LOG(reset_count_read(&rst_cnt));
    if (rst_cnt < RESET_NETCNT_MAX) {
        return OPRT_OK;
    }

    tal_event_subscribe(EVENT_RESET, "reset_netconfig", __reset_netconfig_clear, SUBSCRIBE_TYPE_NORMAL);

    PR_NOTICE("连续上电 %d 次，触发设备重置！", rst_cnt);
    rt = tuya_iot_reset(tuya_iot_client_get());
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_reset 失败: %d", rt);
    }

    return OPRT_OK;
}

int reset_netconfig_start(void)
{
    int rt = OPRT_OK;
    uint8_t rst_cnt = 0;

    TUYA_CALL_ERR_LOG(reset_count_read(&rst_cnt));
    TUYA_CALL_ERR_LOG(reset_count_write(++rst_cnt));

    PR_DEBUG("start reset cnt clear timer!!!!!");
    TIMER_ID rst_config_timer;
    tal_sw_timer_create(reset_netconfig_timer, NULL, &rst_config_timer);
    tal_sw_timer_start(rst_config_timer, 5000, TAL_TIMER_ONCE);

    return OPRT_OK;
}
