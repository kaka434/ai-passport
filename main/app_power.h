// main/app_power.h
// 电池充放电曲线与系统能耗分账模型
// 零 Flash 写入损耗, 极简 RAM 环形采样, 零后台多余功耗
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_POWER_CURVE_POINTS 72 // 12 小时, 每 10 分钟采 1 点 (12 * 6)

// 电池采样点
typedef struct {
    int8_t soc;           // 电量 0..100, -1 为无效
    uint8_t is_charging;  // 1 为充电中 / USB 供电, 0 为电池放电
} app_power_point_t;

// 能耗组件统计数据
typedef struct {
    char name[16];        // 组件名称, 如 "屏幕背光", "Wi-Fi联网", "系统运算", "待机底噪"
    uint32_t active_sec;  // 活跃时间 (秒)
    uint32_t score;       // 能耗加权积分 (相对单位)
    uint8_t percent;      // 能耗占比百分比 (0..100)
    uint32_t color_hex;   // 推荐条形图显示颜色
} app_power_item_t;

typedef struct {
    app_power_item_t items[4];
    int count;
} app_power_breakdown_t;

// 初始化能耗统计模块
void app_power_init(void);

// 记录一次电池读数 (由 battery_task 定期调用, 每 30 秒轮询一次)
void app_power_record_battery(int soc, int mv, bool is_charging);

// 硬件与模块活跃计时
void app_power_track_screen(bool screen_on);
void app_power_track_wifi_active(uint32_t duration_ms);

// 获取过去 12 小时的曲线点队列 (按时间由旧到新排序, 最新的点在末尾)
// valid_count 返回队列中当前已有的采样点数 (刚开机可能小于 72 点)
void app_power_get_curve(app_power_point_t out_points[APP_POWER_CURVE_POINTS], int *valid_count);

// 获取 4 大组件的能耗排行 (按消耗百分比降序排列)
void app_power_get_breakdown(app_power_breakdown_t *out_breakdown);
