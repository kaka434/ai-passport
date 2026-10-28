// main/app_power.c
// 电池充放电曲线与能耗分账模型实现
#include "app_power.h"

#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CURVE_INTERVAL_SEC 600 // 10 分钟 1 点

static SemaphoreHandle_t s_lock = NULL;

// 12 小时环形采样缓冲区
static app_power_point_t s_curve[APP_POWER_CURVE_POINTS];
static int s_curve_count = 0;
static int64_t s_last_curve_sec = 0;

// 硬件活跃时长 (秒)
static bool s_screen_is_on = true;
static int64_t s_screen_last_change_sec = 0;
static uint32_t s_screen_on_sec = 0;

static uint32_t s_wifi_active_ms = 0;

void app_power_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    const int64_t now_sec = esp_timer_get_time() / 1000000LL;
    s_screen_is_on = true;
    s_screen_last_change_sec = now_sec;
    s_screen_on_sec = 0;
    s_wifi_active_ms = 0;
    s_curve_count = 0;
    s_last_curve_sec = 0;
    memset(s_curve, 0, sizeof(s_curve));
}

void app_power_record_battery(int soc, int mv, bool is_charging) {
    (void)mv;
    if (soc < 0 || soc > 100) return;
    if (s_lock == NULL) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now_sec = esp_timer_get_time() / 1000000LL;

    if (s_curve_count == 0) {
        // 开机首个采样点
        s_curve[0].soc = (int8_t)soc;
        s_curve[0].is_charging = is_charging ? 1 : 0;
        s_curve_count = 1;
        s_last_curve_sec = now_sec;
    } else {
        // 距上一次采入曲线超过 10 分钟
        if (now_sec - s_last_curve_sec >= CURVE_INTERVAL_SEC) {
            if (s_curve_count < APP_POWER_CURVE_POINTS) {
                s_curve[s_curve_count].soc = (int8_t)soc;
                s_curve[s_curve_count].is_charging = is_charging ? 1 : 0;
                s_curve_count++;
            } else {
                // 队列已满 72 点 (12h), 整体左移出最老点, 压入最新点
                for (int i = 0; i < APP_POWER_CURVE_POINTS - 1; i++) {
                    s_curve[i] = s_curve[i + 1];
                }
                s_curve[APP_POWER_CURVE_POINTS - 1].soc = (int8_t)soc;
                s_curve[APP_POWER_CURVE_POINTS - 1].is_charging = is_charging ? 1 : 0;
            }
            s_last_curve_sec = now_sec;
        } else {
            // 更新当前最新采样的瞬时值
            s_curve[s_curve_count - 1].soc = (int8_t)soc;
            s_curve[s_curve_count - 1].is_charging = is_charging ? 1 : 0;
        }
    }
    xSemaphoreGive(s_lock);
}

void app_power_track_screen(bool screen_on) {
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now_sec = esp_timer_get_time() / 1000000LL;
    if (s_screen_is_on) {
        const int64_t dur = now_sec - s_screen_last_change_sec;
        if (dur > 0) s_screen_on_sec += (uint32_t)dur;
    }
    s_screen_is_on = screen_on;
    s_screen_last_change_sec = now_sec;
    xSemaphoreGive(s_lock);
}

void app_power_track_wifi_active(uint32_t duration_ms) {
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_active_ms += duration_ms;
    xSemaphoreGive(s_lock);
}

void app_power_get_curve(app_power_point_t out_points[APP_POWER_CURVE_POINTS], int *valid_count) {
    if (out_points == NULL || valid_count == NULL) return;
    if (s_lock == NULL) {
        *valid_count = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int count = s_curve_count;
    for (int i = 0; i < count; i++) {
        out_points[i] = s_curve[i];
    }
    *valid_count = count;
    xSemaphoreGive(s_lock);
}

void app_power_get_breakdown(app_power_breakdown_t *out_breakdown) {
    if (out_breakdown == NULL) return;
    if (s_lock == NULL) {
        memset(out_breakdown, 0, sizeof(*out_breakdown));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now_sec = esp_timer_get_time() / 1000000LL;
    uint32_t screen_sec = s_screen_on_sec;
    if (s_screen_is_on) {
        const int64_t dur = now_sec - s_screen_last_change_sec;
        if (dur > 0) screen_sec += (uint32_t)dur;
    }

    uint32_t wifi_sec = s_wifi_active_ms / 1000;
    uint32_t uptime_sec = (uint32_t)now_sec;
    if (uptime_sec < 1) uptime_sec = 1;

    // 待机时长 = 总运行时间 - 亮屏运算时间
    uint32_t standby_sec = (uptime_sec > screen_sec) ? (uptime_sec - screen_sec) : 0;
    // CPU 活跃时长 = 亮屏时间 + 网络时间 + 基础处理时间
    uint32_t cpu_sec = screen_sec + wifi_sec + (uptime_sec / 10);
    xSemaphoreGive(s_lock);

    // 能耗加权估算 (毫安秒: mA * sec)
    // 屏幕背光: ~35 mA
    // Wi-Fi 射频: ~85 mA
    // CPU 运算: ~18 mA
    // 待机底噪: ~3 mA
    uint64_t score_screen  = (uint64_t)screen_sec * 35ULL;
    uint64_t score_wifi    = (uint64_t)wifi_sec   * 85ULL;
    uint64_t score_cpu     = (uint64_t)cpu_sec    * 18ULL;
    uint64_t score_standby = (uint64_t)standby_sec * 3ULL;

    uint64_t total = score_screen + score_wifi + score_cpu + score_standby;
    if (total == 0) total = 1;

    app_power_item_t items[4] = {
        {
            .name = "屏幕背光",
            .active_sec = screen_sec,
            .score = (uint32_t)(score_screen / 1000ULL),
            .percent = (uint8_t)((score_screen * 100ULL + total / 2ULL) / total),
            .color_hex = 0xEAB308, // 琥珀黄
        },
        {
            .name = "Wi-Fi联网",
            .active_sec = wifi_sec,
            .score = (uint32_t)(score_wifi / 1000ULL),
            .percent = (uint8_t)((score_wifi * 100ULL + total / 2ULL) / total),
            .color_hex = 0x38BDF8, // 天空蓝
        },
        {
            .name = "系统运算",
            .active_sec = cpu_sec,
            .score = (uint32_t)(score_cpu / 1000ULL),
            .percent = (uint8_t)((score_cpu * 100ULL + total / 2ULL) / total),
            .color_hex = 0xA855F7, // 紫罗兰
        },
        {
            .name = "待机底噪",
            .active_sec = standby_sec,
            .score = (uint32_t)(score_standby / 1000ULL),
            .percent = (uint8_t)((score_standby * 100ULL + total / 2ULL) / total),
            .color_hex = 0x94A3B8, // 冷灰色
        },
    };

    // 归一化校验, 保证百分比之和严格为 100%
    int sum = items[0].percent + items[1].percent + items[2].percent + items[3].percent;
    if (sum != 100) {
        // 差异补偿给占比最大的一项
        int max_idx = 0;
        for (int i = 1; i < 4; i++) {
            if (items[i].percent > items[max_idx].percent) max_idx = i;
        }
        items[max_idx].percent = (uint8_t)(items[max_idx].percent + (100 - sum));
    }

    // 按能耗占比降序排序 (Bubble Sort 4 items)
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3 - i; j++) {
            if (items[j].percent < items[j + 1].percent) {
                app_power_item_t tmp = items[j];
                items[j] = items[j + 1];
                items[j + 1] = tmp;
            }
        }
    }

    out_breakdown->count = 4;
    for (int i = 0; i < 4; i++) {
        out_breakdown->items[i] = items[i];
    }
}
