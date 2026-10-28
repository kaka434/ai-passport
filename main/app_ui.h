// main/app_ui.h
// 看板界面。整套页面是重新设计的,不使用基线 demo 的测试菜单或 ui_pixel 视觉外壳。
//
// 只有两页:
//   比分   一列比赛,直播中置顶,其次是接下来要打的;每行"联赛+状态 / 主队 比分 客队"
//   设置   配网状态、网络地址、刷新间隔、立即刷新、清除配网
//
// 按键:上/下移动选中项;确定在比分页=立即刷新、在设置页=执行当前行;
// 长按确定在两页之间切换。
#pragma once

#include "app_nav.h"

// 建屏、起刷新定时器。必须在 LVGL 初始化之后、持有 LVGL 锁时调用。
void app_ui_init(void);

// 处理一次按键。内部按需加锁,调用方不要持锁。
void app_ui_handle_input(app_input_t input);

// 配置被外部改动(串口命令/设置页)后调用,让设置页重画。
void app_ui_notify_config_changed(void);

// 电池读数不可用时传 -1,界面显示 "--" 而不是 0%。mv 为电压毫伏。
void app_ui_set_battery(int soc, int mv);
void app_ui_set_power_stats(float drain_rate_per_hour, int est_hours);
