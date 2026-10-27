// main/app_nav.h
// 三键导航状态机:页面、选中项、滚动窗口。纯逻辑,主机可测。
//
// 六个页面:前两个是主界面,中间是单场详情,后三个是配网流程:
//   比分     实时比分与接下来要打的比赛
//   详情     选中那一场的进球时间轴 / 技术统计(上下键翻子页)
//   设置     配网状态、手机配网、刷新间隔、立即刷新、清除配网
//   Wi-Fi   扫描到的热点列表(已保存的网络固定在最上面)
//   密码     屏上键盘,给选中的热点输密码
//   手机     开热点,手机浏览器里输密码(屏上不按键)
//
// 按键约定:
//   上/下 单击   比分页=移动选中项;详情页=翻子页;设置/热点页=移动选中项;
//                密码页=移动键盘光标
//   上 在首项   比分页=往前翻一天(回看),列表为空时直接按上也能翻
//   下 在末项   比分页=往后翻一天,列表为空时直接按下也能翻
//   确定 单击   比分页=打开选中场次的详情;详情页=重新拉取;
//                设置页=执行当前行;热点页=选择;密码页=按下键盘上当前键;
//                手机配网页=重新开始
//   确定 长按   比分/设置=两页互切;详情页=返回比分;热点页=返回设置;
//                密码页=确认连接并返回;手机配网页=停止热点并返回
//
// 比分页的"立即刷新"让位给了"打开详情":手动刷新改在设置页第 3 行,
// 而且列表本来就有 60/120/300 秒的自动刷新。
//
// 密码页的方向键与确定键不在这里处理:键盘光标属于 app_keyboard 的状态,
// 由界面直接驱动,导航只负责"长按=提交"这条页面级的语义。
#pragma once

#include <stdbool.h>

// 详情页的子页数量:0 = 进球时间轴,1 = 阵型与技术统计。
#define APP_DETAIL_PAGES 2

// 引导主菜单项
typedef enum {
    APP_MENU_SCORES = 0,     // 比赛看板
    APP_MENU_STANDINGS,      // 联赛积分榜
    APP_MENU_FAVORITE,       // 关注球队
    APP_MENU_ITEM_COUNT,
} app_menu_item_t;

typedef enum {
    APP_PAGE_HOME = 0,       // 引导主菜单页
    APP_PAGE_SCORES,
    APP_PAGE_DETAIL,
    APP_PAGE_LEAGUES,        // 联赛选择列表
    APP_PAGE_STANDINGS,      // 积分榜表格
    APP_PAGE_FAVORITE,       // 关注球队主队看板
    APP_PAGE_SETTINGS,
    APP_PAGE_STATUS,         // 系统状态二级页
    APP_PAGE_WIFI_LIST,
    APP_PAGE_WIFI_PASS,
    APP_PAGE_PORTAL,
} app_page_t;

typedef enum {
    APP_INPUT_UP = 0,
    APP_INPUT_DOWN,
    APP_INPUT_OK,
    APP_INPUT_OK_LONG,
    APP_INPUT_OK_DOUBLE,     // OK 双击
} app_input_t;

typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_REDRAW,          // 选中项/滚动窗口/子页变了,重画
    APP_ACT_PAGE_CHANGED,    // 切页,整页重建
    APP_ACT_SETTING,         // 设置页里按了确定,sel 是行号
    APP_ACT_REFRESH,         // 立即刷新
    APP_ACT_STATUS_OPEN,     // 打开系统状态二级页
    APP_ACT_WIFI_PICK,       // 热点列表里按了确定,sel 是行号
    APP_ACT_WIFI_BACK,       // 热点列表里长按,回到设置页
    APP_ACT_WIFI_CONNECT,    // 密码页里长按,提交密码并连接
    APP_ACT_DETAIL_OPEN,     // 比分页里按了确定,sel 是场次下标
    APP_ACT_DETAIL_REFRESH,  // 详情页里按了确定,重新拉取这一场
    APP_ACT_DETAIL_BACK,     // 详情页里长按,已切回比分页
    APP_ACT_PORTAL_RETRY,    // 手机配网页里按了确定,重新开热点
    APP_ACT_PORTAL_BACK,     // 手机配网页里长按,已切回设置页
    APP_ACT_DAY_PREV,        // 比分页首项按上,切到前一日(回看)
    APP_ACT_DAY_NEXT,        // 比分页末项按下,切回后一日
    APP_ACT_LEAGUE_PICK,     // 选中联赛进入积分榜
    APP_ACT_STANDINGS_BACK,  // 积分榜返回联赛选择
    APP_ACT_FAVORITE_OPEN,   // 主页点击关注球队
    APP_ACT_FAVORITE_BACK,   // 主队看板返回主页
    APP_ACT_FAVORITE_PICK,   // 积分榜设为主队
} app_act_t;

typedef struct {
    app_page_t page;
    int sel;      // 当前页选中项
    int scroll;   // 列表窗口起点
    int sub;      // 详情页的子页(0..APP_DETAIL_PAGES-1)
} app_nav_t;

void app_nav_init(app_nav_t *nav);

// item_count:当前页可选条目数;visible_rows:列表一次能显示几行。
// 返回界面该做什么。count 为 0 时上/下/确定都不改变状态。
app_act_t app_nav_handle(app_nav_t *nav, app_input_t input, int item_count, int visible_rows);

// 切换页面时把选中项和滚动窗口复位。
void app_nav_reset_selection(app_nav_t *nav);

// 直接跳页(配网流程里"进入列表""选中网络后去输密码"用),同样会复位选中项。
void app_nav_goto(app_nav_t *nav, app_page_t page);
