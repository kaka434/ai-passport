#include "app_standings.h"
#include "app_favorite.h"
// main/app_ui.c
// 看板界面实现。
//
// 设计目标是"一眼看清":主界面只有两页(比分 / 设置),比分页就是一列比赛,
// 每行两行字——上面是联赛和状态,下面是主队、比分、客队。没有页签栏、
// 没有焦点卡,减少一切需要二次点击才能看到的信息。
//
// 比分页按 OK 打开选中场次的详情,详情里两页:
//   进球   比分条 + 进球时间轴(主队靠左、客队靠右、分钟居中)
//   统计   阵型 + 技术统计(控球率、射门…)
// 上下键翻页,OK 重拉,长按返回列表。
//
// 配网是主界面之外的一段线性流程,不需要外部工具:
//   设置 → Wi-Fi 配网(OK) → 热点列表(上下选、OK 选) → 屏上键盘输密码
//   → 长按 OK 提交并连接 → 回设置页看连接结果。
// 连上过的热点会写进 NVS,下次进列表时固定排在最上面,选中即连。
//
// 视觉语言:深绿球场底 + 记分牌式比分,与基线 demo 的像素卡片菜单没有
// 任何共用结构(不复用 ui_pixel_*)。
#include "app_ui.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

#include "app_config.h"
#include "app_crest.h"
#include "app_fonts.h"
#include "app_keyboard.h"
#include "app_known.h"
#include "app_model.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_settings.h"
#include "app_store.h"

#include "bsp_battery.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

static app_standings_snapshot_t s_standings_snap;
static uint32_t s_standings_rev = 0;
static uint32_t s_curr_season_id = 27502;
static char s_curr_league_name[16] = "英超";

static app_fav_snapshot_t s_fav_snap;
static uint32_t s_fav_rev = 0;
static app_fav_team_t s_fav_team;



// ---------------------------------------------------------------- 配色与尺寸

#define C_BG        0x07120C
#define C_HEADER    0x0B1B13
#define C_PANEL     0x11291D
#define C_PANEL_HI  0x1B4631
#define C_TEXT      0xF2F8F3
#define C_TEAM      0xDCEBE2
#define C_MUTED     0x7FA894
#define C_ACCENT    0x3FD07A
#define C_LIVE      0xFF5D5D
#define C_AMBER     0xFFC24B

#define SCR_W 240
#define SCR_H 320
#define HEADER_H 40
#define FOOTER_H 24
#define BODY_Y HEADER_H
#define BODY_H (SCR_H - BODY_Y - FOOTER_H)

// 比赛行:48 px 高、50 px 间距,正文区一次放 5 行(3 + 4*50 + 48 = 251 < 256)。
// 行内三列:主队(左) / 比分(中,22 px) / 客队(右),各留 8 px 内边距。
#define ROW_H 48
#define ROW_PITCH 50
#define ROW_Y0 3
#define LIST_ROWS 5

// 今天没有比赛时,顶部多一条"下一个比赛日"横幅:22 px 高,列表让出一行。
// 28 + 3*50 + 48 = 226 < 256,四条仍然完整。
#define BANNER_Y 2
#define BANNER_H 22
#define LIST_Y0_BANNER 28

// 设置行:29 px 高、31 px 间距,8 行(3 + 7*31 + 29 = 249 < 256)。
// 从 6 行加到 8 行(多了「自动息屏」和「屏幕亮度」),行高只能从 38 收到 29;
// 19 px 的字放进 29 px 的行里还剩 5 px 上下留白,触摸不到但看着不挤。
#define SET_ROW_H 29
#define SET_ROW_PITCH 31
#define SET_ROW_Y0 3
#define SET_ROW_TEXT_Y 5

// 热点行:44 px 高、46 px 间距,5 行(4 + 4*46 + 44 = 232 < 256)。
#define AP_ROW_H 44
#define AP_ROW_PITCH 46
#define AP_ROW_Y0 4
#define AP_ROWS 5

// 键盘:6 列 × 7 行,键 34×24、间距 3/2。
// 10 + 6*34 + 5*3 = 229 → 右边留 11 px;62 + 6*26 + 24 = 242 < 256。
#define KB_KEY_W 34
#define KB_KEY_H 24
#define KB_KEY_GAP_X 3
#define KB_KEY_GAP_Y 2
#define KB_X0 10
#define KB_Y0 62

// 详情页顶部比分条,68 px:联赛/状态 14 + 队徽 32 + 队名 16,加上下余量。
// 队徽放在队名上方而不是下方——同样的高度里,徽章在上、名字在下更符合看球时
// 的读法,而且能把整条压到 68 px,下面 8 行进球才放得下(72 + 8×21 = 240)。
#define DETAIL_BAR_H 68
#define CREST_SIZE 32
// 队徽水平位置对齐各自队名的中心:主队名 6..80 的中心是 43,客队名
// 144..218 的中心是 181,各往左退半个徽章。
#define CREST_X_HOME 27
#define CREST_X_AWAY 173
#define GOAL_ROW_H 20
#define GOAL_ROW_PITCH 21
#define GOAL_ROW_Y0 88
#define GOAL_ROWS 8

// 详情第 2 页:顶部就是同一个带队徽的比分条,下面 8 项统计。
// 74 = 68 的比分条 + 6 的间距;22 的行距塞得下 8 项(74 + 7×22 + 20 = 248)。
#define STAT_ROW_PITCH 22
#define STAT_ROW_Y0 74

// 长时间无操作后的背光处理统一由 main.c 的 app_idle 任务负责(它才知道
// 「自动息屏」和「屏幕亮度」这两个设置)。界面这边不再自己压背光,否则
// 每次按键都把亮度写回 100%,用户设的 25% 会立刻失效。



static lv_obj_t *s_scr;
static lv_obj_t *s_body;
static lv_obj_t *s_battery;
static lv_obj_t *s_wifi_bars[3];
static lv_obj_t *s_footer;
static lv_obj_t *s_title;      // 页头标题:随页面变(比赛看板 / 联赛名 / 设置)
static lv_timer_t *s_timer;

static app_nav_t s_nav;
static app_config_t s_cfg;
static app_snapshot_t s_snap;      // 界面自己的快照副本
static uint32_t s_seen_revision;
static int s_battery_soc = -1;
static int s_battery_mv = -1;
static bool s_config_dirty;

// 设置页 8 行的句柄。翻行时只改这 8 行的颜色,不重建整页——重建一次要
// 删掉再建 30 多个对象,是菜单"跟不上手指"的另一半原因。
static lv_obj_t *s_set_rows[APP_SET_ROW_COUNT];
static lv_obj_t *s_set_bars[APP_SET_ROW_COUNT];
static lv_obj_t *s_set_labels[APP_SET_ROW_COUNT];
static lv_obj_t *s_set_values[APP_SET_ROW_COUNT];

// 临时提示(如"正在连接…")。缓冲按最长的一条留够,免得编译器抱怨截断。
static char s_toast[48];
static int s_toast_ticks;

// 配网流程的状态。约 2 KB 静态区,不占任务栈。
static app_known_list_t s_known;
static app_ap_row_t s_ap_rows[APP_AP_ROW_MAX];
static size_t s_ap_count;
static app_scan_result_t s_scan;
static uint32_t s_scan_revision;
static app_keyboard_t s_kb;
static char s_target_ssid[APP_SSID_MAX + 1];
static bool s_target_secure;
static bool s_target_saved;   // 这一条的密码是从 NVS 里预填的

// 键盘对象句柄。只在密码页有效,页面重建时先清空再 lv_obj_clean()。
// 一个键就是一个对象(带底色的标签),不再拆成"面板 + 标签"两层:42 个键
// 曾经要 84 个对象,把 LVGL 的内存池顶爆,表现是进密码页直接卡死。
static lv_obj_t *s_kb_keys[APP_KB_KEYS];
static lv_obj_t *s_kb_field;

// 详情页状态。s_detail_match 是打开时那份列表快照的副本:详情拉取要几秒,
// 期间列表可能已经刷新过,不能让"比分条"跟着变来变去。
static app_match_t s_detail_match;
static int s_detail_index;              // 打开前的选中项,返回时恢复
static app_detail_snapshot_t s_detail;
static uint32_t s_detail_revision;
// 队徽是异步补的,版本号和赛况分开记:图下完要重画一次比分条,但不该把整个
// 详情页的数据再拷一遍。
static uint32_t s_crest_revision;

// 手机配网页的状态。约 80 字节。
static app_portal_snapshot_t s_portal;
static uint32_t s_portal_revision;

// 页面重建。详情页与配网流程都在它之前定义,先声明。
static void build_body(void);

// ---------------------------------------------------------------- 小工具

// LVGL 在堆紧张时会返回 NULL(对象分配失败)。这块板没有 PSRAM,TLS 握手
// 期间堆确实会紧,所以每个创建入口都挡一下:少画一个控件好过卡死在断言里。
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) return NULL;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                      const char *value) {
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) return NULL;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value);
    return label;
}

// 定宽单行 + 超出省略号:队名长短差别很大,不能让标签把行撑破。
static lv_obj_t *clipped_text(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                              const char *value, int x, int y, int w, int h,
                              lv_text_align_t align) {
    if (parent == NULL) return NULL;
    lv_obj_t *label = text(parent, font, color, value);
    if (label == NULL) return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

// 定长拷贝:SSID / 密码来自网络与键盘,长度不受编译器约束,直接 snprintf
// 会触发 -Wformat-truncation。
static void copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0 || dst == NULL || src == NULL) return;
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void toast(const char *message) {
    snprintf(s_toast, sizeof(s_toast), "%s", message != NULL ? message : "");
    s_toast_ticks = 4;
}

static uint32_t state_color(const app_match_t *match) {
    if (app_match_is_live(match->state)) return C_LIVE;
    if (match->state == APP_STATE_FIXTURE) return C_ACCENT;
    if (match->state == APP_STATE_POSTPONED || match->state == APP_STATE_CANCELLED
        || match->state == APP_STATE_UNKNOWN) {
        return C_AMBER;
    }
    return C_MUTED;   // 完场
}

// ---------------------------------------------------------------- 页头 / 页脚

static void update_header(void) {
    if (s_battery == NULL) return;

    if (s_battery_soc < 0) {
        lv_label_set_text(s_battery, "--%");
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", s_battery_soc);
    }

    if (s_wifi_bars[0] != NULL) {
        if (!s_snap.wifi_connected) {
            uint32_t c = C_MUTED;
            if (s_snap.state == APP_NET_ERROR) {
                c = C_LIVE;
            } else if (s_snap.state == APP_NET_CONNECTING || s_snap.state == APP_NET_SYNCING
                       || s_snap.state == APP_NET_FETCHING) {
                c = C_AMBER;
            }
            for (int i = 0; i < 3; i++) {
                if (s_wifi_bars[i] != NULL) {
                    lv_obj_set_style_bg_color(s_wifi_bars[i], lv_color_hex(c), 0);
                }
            }
        } else {
            // 已连接: 根据 RSSI 显示阶梯强度
            int active_bars = 1;
            if (s_snap.rssi >= -60) {
                active_bars = 3;
            } else if (s_snap.rssi >= -75) {
                active_bars = 2;
            }
            uint32_t active_color = (s_snap.state == APP_NET_FETCHING) ? C_AMBER : C_ACCENT;
            for (int i = 0; i < 3; i++) {
                if (s_wifi_bars[i] != NULL) {
                    lv_obj_set_style_bg_color(s_wifi_bars[i],
                        lv_color_hex(i < active_bars ? active_color : 0x334155), 0);
                }
            }
        }
    }
}

static void update_footer(void) {
    if (s_footer == NULL) return;

    if (s_toast_ticks > 0) {
        lv_label_set_text(s_footer, s_toast);
        lv_obj_set_style_text_color(s_footer, lv_color_hex(C_ACCENT), 0);
        return;
    }

    switch (s_nav.page) {
        case APP_PAGE_HOME:
            lv_label_set_text(s_footer, "▲/▼ 切换 · OK 进入 · 长按设置");
            break;
        case APP_PAGE_LEAGUES:
            lv_label_set_text(s_footer, "▲/▼ 切换 · OK 查看 · 长按返回");
            break;
        case APP_PAGE_STANDINGS: {
            char f_str[64];
            if (s_standings_snap.loading) {
                snprintf(f_str, sizeof(f_str), "获取中 · ▲/▼ 或长按返回");
            } else if (s_standings_snap.error || s_standings_snap.team_count == 0) {
                snprintf(f_str, sizeof(f_str), "OK 重试 · ▲/▼ 返回");
            } else {
                snprintf(f_str, sizeof(f_str), "%d/%d · OK 设主队 · 长按返回",
                         s_nav.sel + 1, s_standings_snap.team_count);
            }
            lv_label_set_text(s_footer, f_str);
            break;
        }
        case APP_PAGE_FAVORITE: {
            if (s_fav_snap.loading) {
                lv_label_set_text(s_footer, "加载中 · ▲/▼ 或长按返回");
            } else if (!s_fav_snap.has_fav) {
                lv_label_set_text(s_footer, "长按返回 · 积分榜设主队");
            } else {
                lv_label_set_text(s_footer, "OK 刷新 · 长按返回 · 积分榜重设");
            }
            break;
        }
        case APP_PAGE_SETTINGS:
            lv_label_set_text(s_footer, "OK 执行 · 长按回比分");
            break;
        case APP_PAGE_STATUS:
            lv_label_set_text(s_footer, "OK 返回 · 长按回比分");
            break;
        case APP_PAGE_DETAIL: {
            // 左:第几子页;右:三个键各干什么。详情页的上下键是翻页,
            // 不写出来用户会以为是移动列表。
            char line[64];
            snprintf(line, sizeof(line), "%s %d/%d · OK 刷新 · 长按返回",
                     s_nav.sub == 0 ? "进球" : "统计", s_nav.sub + 1, APP_DETAIL_PAGES);
            lv_label_set_text(s_footer, line);
            break;
        }
        case APP_PAGE_WIFI_LIST:
            lv_label_set_text(s_footer, s_scan.scanning ? "正在扫描附近网络…" : "OK 连接 · 长按返回");
            break;
        case APP_PAGE_WIFI_PASS:
            lv_label_set_text(s_footer, "OK 输入 · 长按连接");
            break;
        case APP_PAGE_PORTAL:
            lv_label_set_text(s_footer, "OK 重开热点 · 长按退出");
            break;
        case APP_PAGE_SCORES:
        default: {
            // 比分页:左边"第几场/共几场",右边最近一次刷新时间。
            // 缓冲区按 int 的最坏宽度留够,免得 snprintf 被 -Wformat-truncation 判为可能截断。
            char position[40] = "";
            if (s_snap.matches.count > 0) {
                snprintf(position, sizeof(position), "%d/%d · OK 详情 · ", s_nav.sel + 1,
                         (int)s_snap.matches.count);
            }

            char stamp[40] = "";
            if (s_snap.updated_at > 0) {
                char clock[12];
                app_format_clock(s_snap.updated_at, clock, sizeof(clock));
                snprintf(stamp, sizeof(stamp), "更新 %s", clock);
            }

            char line[96];
            snprintf(line, sizeof(line), "%s%s", position, stamp);
            lv_label_set_text(s_footer, line);
            break;
        }
    }
    lv_obj_set_style_text_color(s_footer, lv_color_hex(C_MUTED), 0);
}


// ---------------------------------------------------------------- 页面:引导主菜单

static const char *s_home_titles[APP_MENU_ITEM_COUNT] = {
    "比赛看板",
    "联赛积分榜",
    "关注球队",
};

static const char *s_home_descs[APP_MENU_ITEM_COUNT] = {
    "实时比分 / 近期赛程 / 赛况详情",
    "五大联赛 · 欧冠 · 中超中甲",
    "主队赛果与赛季全比分数据",
};

static lv_obj_t *s_home_cards[APP_MENU_ITEM_COUNT];

static void home_select(int index) {
    for (int i = 0; i < APP_MENU_ITEM_COUNT; i++) {
        if (s_home_cards[i] == NULL) continue;
        const bool selected = (i == index);
        lv_obj_set_style_bg_color(s_home_cards[i], lv_color_hex(selected ? C_PANEL_HI : C_PANEL), 0);
        lv_obj_set_style_border_color(s_home_cards[i], lv_color_hex(selected ? C_ACCENT : C_PANEL_HI), 0);
        lv_obj_set_style_border_width(s_home_cards[i], selected ? 2 : 1, 0);
    }
}

static void build_home(void) {
    memset(s_home_cards, 0, sizeof(s_home_cards));
    const int card_h = 70;
    const int gap = 8;
    const int start_y = 6;

    for (int i = 0; i < APP_MENU_ITEM_COUNT; i++) {
        const int y = start_y + i * (card_h + gap);
        const bool selected = (i == s_nav.sel);
        lv_obj_t *card = panel(s_body, 8, y, SCR_W - 16, card_h,
                               selected ? C_PANEL_HI : C_PANEL, 8);
        if (card == NULL) continue;
        s_home_cards[i] = card;

        lv_obj_set_style_border_color(card, lv_color_hex(selected ? C_ACCENT : C_PANEL_HI), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);

        clipped_text(card, APP_FONT_BODY, selected ? C_ACCENT : C_TEXT,
                     s_home_titles[i], 12, 12, SCR_W - 40, 20, LV_TEXT_ALIGN_LEFT);

        clipped_text(card, APP_FONT_SMALL, C_MUTED,
                     s_home_descs[i], 12, 38, SCR_W - 40, 18, LV_TEXT_ALIGN_LEFT);
    }
}


// ---------------------------------------------------------------- 页面:联赛选择与积分榜
static lv_obj_t *s_league_rows[APP_LEAGUES_COUNT];
#define STANDINGS_VISIBLE_ROWS 8

static void build_leagues(void) {
    memset(s_league_rows, 0, sizeof(s_league_rows));
    const int row_h = 28;
    const int gap = 4;
    const int y0 = 6;

    for (int i = 0; i < APP_LEAGUES_COUNT; i++) {
        const int y = y0 + i * (row_h + gap);
        const bool sel = (i == s_nav.sel);
        lv_obj_t *r = panel(s_body, 8, y, SCR_W - 16, row_h, sel ? C_PANEL_HI : C_PANEL, 6);
        if (r == NULL) continue;
        s_league_rows[i] = r;

        lv_obj_set_style_border_color(r, lv_color_hex(sel ? C_ACCENT : C_PANEL_HI), 0);
        lv_obj_set_style_border_width(r, sel ? 2 : 1, 0);

        clipped_text(r, APP_FONT_BODY, sel ? C_ACCENT : C_TEXT,
                     APP_LEAGUES[i].name, 12, 4, 120, 20, LV_TEXT_ALIGN_LEFT);

        clipped_text(r, APP_FONT_SMALL, C_MUTED,
                     "积分榜", SCR_W - 70, 6, 50, 16, LV_TEXT_ALIGN_RIGHT);
    }
}

static void leagues_select(int idx) {
    for (int i = 0; i < APP_LEAGUES_COUNT; i++) {
        if (s_league_rows[i] == NULL) continue;
        const bool sel = (i == idx);
        lv_obj_set_style_bg_color(s_league_rows[i], lv_color_hex(sel ? C_PANEL_HI : C_PANEL), 0);
        lv_obj_set_style_border_color(s_league_rows[i], lv_color_hex(sel ? C_ACCENT : C_PANEL_HI), 0);
        lv_obj_set_style_border_width(s_league_rows[i], sel ? 2 : 1, 0);
    }
}

static void build_standings(void) {
    app_standings_snapshot_copy(&s_standings_snap);
    s_standings_rev = app_standings_revision();

    if (s_standings_snap.loading) {
        clipped_text(s_body, APP_FONT_BODY, C_ACCENT, "正在获取积分榜...", 0, 95, SCR_W, 30, LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "按 ▲/▼ 或长按 OK 返回", 0, 130, SCR_W, 20, LV_TEXT_ALIGN_CENTER);
        return;
    }
    if (s_standings_snap.error || s_standings_snap.team_count == 0) {
        clipped_text(s_body, APP_FONT_BODY, C_LIVE, "获取积分榜失败", 0, 85, SCR_W, 30, LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "OK 重试 · 按 ▲/▼ 返回", 0, 120, SCR_W, 20, LV_TEXT_ALIGN_CENTER);
        return;
    }

    // 表头: 排名 / 球队 / 赛 / 胜平负 / 积分
    lv_obj_t *th = panel(s_body, 4, 2, SCR_W - 8, 20, C_HEADER, 4);
    if (th != NULL) {
        clipped_text(th, APP_FONT_SMALL, C_MUTED, "#", 4, 2, 20, 16, LV_TEXT_ALIGN_CENTER);
        clipped_text(th, APP_FONT_SMALL, C_MUTED, "球队", 28, 2, 90, 16, LV_TEXT_ALIGN_LEFT);
        clipped_text(th, APP_FONT_SMALL, C_MUTED, "赛", 122, 2, 24, 16, LV_TEXT_ALIGN_CENTER);
        clipped_text(th, APP_FONT_SMALL, C_MUTED, "胜/平/负", 148, 2, 48, 16, LV_TEXT_ALIGN_CENTER);
        clipped_text(th, APP_FONT_SMALL, C_ACCENT, "分", 200, 2, 28, 16, LV_TEXT_ALIGN_CENTER);
    }

    const int row_h = 26;
    const int start_y = 26;
    const int start_idx = s_nav.scroll;
    const int count = s_standings_snap.team_count;

    for (int r = 0; r < STANDINGS_VISIBLE_ROWS; r++) {
        const int idx = start_idx + r;
        if (idx >= count) break;
        const app_standing_team_t *tm = &s_standings_snap.teams[idx];
        const bool sel = (idx == s_nav.sel);
        const int y = start_y + r * (row_h + 2);

        lv_obj_t *row = panel(s_body, 4, y, SCR_W - 8, row_h, sel ? C_PANEL_HI : C_PANEL, 4);
        if (row == NULL) continue;
        if (sel) {
            lv_obj_set_style_border_color(row, lv_color_hex(C_ACCENT), 0);
            lv_obj_set_style_border_width(row, 1, 0);
        }

        // 排名 (主队显示 ★ 标记)
        char rank_str[12];
        const bool is_fav = (s_fav_team.team_id > 0 && tm->team_id == s_fav_team.team_id);
        if (is_fav) {
            snprintf(rank_str, sizeof(rank_str), "%d*", tm->rank);
        } else {
            snprintf(rank_str, sizeof(rank_str), "%d", tm->rank);
        }
        uint32_t rank_col = is_fav ? C_ACCENT : ((tm->rank <= 3) ? C_AMBER : C_MUTED);
        clipped_text(row, APP_FONT_SMALL, rank_col, rank_str, 2, 5, 24, 16, LV_TEXT_ALIGN_CENTER);

        // 队名
        char name_buf[32];
        app_fonts_sanitize(tm->team_name, name_buf, sizeof(name_buf));
        clipped_text(row, APP_FONT_SMALL, sel ? C_ACCENT : C_TEXT, name_buf, 28, 5, 92, 16, LV_TEXT_ALIGN_LEFT);

        // 已赛场次
        char p_str[8];
        snprintf(p_str, sizeof(p_str), "%d", tm->played);
        clipped_text(row, APP_FONT_SMALL, C_MUTED, p_str, 122, 5, 24, 16, LV_TEXT_ALIGN_CENTER);

        // 胜/平/负
        char record[16];
        snprintf(record, sizeof(record), "%d/%d/%d", tm->won, tm->draw, tm->lost);
        clipped_text(row, APP_FONT_SMALL, C_MUTED, record, 146, 5, 52, 16, LV_TEXT_ALIGN_CENTER);

        // 积分
        char pts_str[8];
        snprintf(pts_str, sizeof(pts_str), "%d", tm->points);
        clipped_text(row, APP_FONT_BODY, C_ACCENT, pts_str, 198, 3, 30, 20, LV_TEXT_ALIGN_CENTER);
    }
}

static void build_favorite(void) {
    app_favorite_snapshot_copy(&s_fav_snap);
    s_fav_rev = app_favorite_revision();

    if (!s_fav_snap.has_fav || s_fav_snap.team_id == 0) {
        clipped_text(s_body, APP_FONT_BODY, C_ACCENT, "尚未关注球队", 0, 90, SCR_W, 30, LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "请在积分榜中选中球队按OK设为主队", 0, 125, SCR_W, 20, LV_TEXT_ALIGN_CENTER);
        return;
    }

    if (s_fav_snap.loading) {
        clipped_text(s_body, APP_FONT_BODY, C_ACCENT, "正在获取主队战报...", 0, 95, SCR_W, 30, LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "按 ▲/▼ 或长按 OK 返回", 0, 130, SCR_W, 20, LV_TEXT_ALIGN_CENTER);
        return;
    }

    // 卡片 1: 联赛概况 (y=4, h=66)
    lv_obj_t *c1 = panel(s_body, 6, 4, SCR_W - 12, 66, C_PANEL, 6);
    if (c1 != NULL) {
        char title_buf[64];
        if (s_fav_snap.league_name[0] != '\0') {
            snprintf(title_buf, sizeof(title_buf), "%s · %s", s_fav_snap.team_name, s_fav_snap.league_name);
        } else {
            snprintf(title_buf, sizeof(title_buf), "%s", s_fav_snap.team_name);
        }
        char clean_title[64];
        app_fonts_sanitize(title_buf, clean_title, sizeof(clean_title));
        clipped_text(c1, APP_FONT_BODY, C_ACCENT, clean_title, 10, 6, 140, 20, LV_TEXT_ALIGN_LEFT);

        char clean_rank[32];
        app_fonts_sanitize(s_fav_snap.rank_desc[0] ? s_fav_snap.rank_desc : "参赛中", clean_rank, sizeof(clean_rank));
        clipped_text(c1, APP_FONT_BODY, C_AMBER, clean_rank,
                     SCR_W - 12 - 80, 6, 70, 20, LV_TEXT_ALIGN_RIGHT);

        char rec[48] = "";
        if (s_fav_snap.record_desc[0]) {
            snprintf(rec, sizeof(rec), "战绩: %s", s_fav_snap.record_desc);
        } else if (s_fav_snap.venue[0]) {
            snprintf(rec, sizeof(rec), "主场: %s", s_fav_snap.venue);
        }
        char clean_rec[48];
        app_fonts_sanitize(rec, clean_rec, sizeof(clean_rec));
        clipped_text(c1, APP_FONT_SMALL, C_TEXT, clean_rec, 10, 26, SCR_W - 32, 16, LV_TEXT_ALIGN_LEFT);

        char form[48] = "";
        if (s_fav_snap.form_desc[0]) {
            snprintf(form, sizeof(form), "近5场: %s", s_fav_snap.form_desc);
        }
        char clean_form[48];
        app_fonts_sanitize(form, clean_form, sizeof(clean_form));
        clipped_text(c1, APP_FONT_SMALL, C_MUTED, clean_form, 10, 44, SCR_W - 32, 16, LV_TEXT_ALIGN_LEFT);
    }

    // 卡片 2: 上一场比赛 (y=74, h=78)
    lv_obj_t *c2 = panel(s_body, 6, 74, SCR_W - 12, 78, C_PANEL, 6);
    if (c2 != NULL) {
        char sub_title[48];
        char comp_clean[32];
        app_fonts_sanitize(s_fav_snap.last_match.comp[0] ? s_fav_snap.last_match.comp : "完赛", comp_clean, sizeof(comp_clean));
        snprintf(sub_title, sizeof(sub_title), "上一场 · %s", comp_clean);
        clipped_text(c2, APP_FONT_SMALL, C_MUTED, sub_title, 10, 5, 120, 16, LV_TEXT_ALIGN_LEFT);
        clipped_text(c2, APP_FONT_SMALL, C_MUTED, s_fav_snap.last_match.date_str, SCR_W - 12 - 90, 5, 80, 16, LV_TEXT_ALIGN_RIGHT);

        if (s_fav_snap.last_match.team_A[0]) {
            char team_a[APP_NAME_LEN];
            char team_b[APP_NAME_LEN];
            app_fonts_sanitize(s_fav_snap.last_match.team_A, team_a, sizeof(team_a));
            app_fonts_sanitize(s_fav_snap.last_match.team_B, team_b, sizeof(team_b));
            char vs_text[96];
            snprintf(vs_text, sizeof(vs_text), "%s %d - %d %s",
                     team_a,
                     s_fav_snap.last_match.score_A >= 0 ? s_fav_snap.last_match.score_A : 0,
                     s_fav_snap.last_match.score_B >= 0 ? s_fav_snap.last_match.score_B : 0,
                     team_b);
            clipped_text(c2, APP_FONT_BODY, C_TEXT, vs_text, 10, 26, SCR_W - 32, 22, LV_TEXT_ALIGN_CENTER);

            const char *res_str = (s_fav_snap.last_match.result == 1) ? "主队 胜" :
                                  (s_fav_snap.last_match.result == 0) ? "平 局" : "主队 负";
            uint32_t res_col = (s_fav_snap.last_match.result == 1) ? C_LIVE :
                               (s_fav_snap.last_match.result == 0) ? C_MUTED : C_TEXT;
            clipped_text(c2, APP_FONT_SMALL, res_col, res_str, 0, 52, SCR_W - 12, 16, LV_TEXT_ALIGN_CENTER);
        } else {
            clipped_text(c2, APP_FONT_SMALL, C_MUTED, "暂无近期比赛记录", 0, 34, SCR_W - 12, 20, LV_TEXT_ALIGN_CENTER);
        }
    }

    // 卡片 3: 下一场比赛 (y=156, h=78)
    lv_obj_t *c3 = panel(s_body, 6, 156, SCR_W - 12, 78, C_PANEL, 6);
    if (c3 != NULL) {
        char sub_title[48];
        char comp_clean[32];
        app_fonts_sanitize(s_fav_snap.next_match.comp[0] ? s_fav_snap.next_match.comp : "赛前", comp_clean, sizeof(comp_clean));
        snprintf(sub_title, sizeof(sub_title), "下一场 · %s", comp_clean);
        clipped_text(c3, APP_FONT_SMALL, C_MUTED, sub_title, 10, 5, 120, 16, LV_TEXT_ALIGN_LEFT);
        clipped_text(c3, APP_FONT_SMALL, C_ACCENT, s_fav_snap.next_match.date_str, SCR_W - 12 - 90, 5, 80, 16, LV_TEXT_ALIGN_RIGHT);

        if (s_fav_snap.next_match.team_A[0]) {
            char team_a[APP_NAME_LEN];
            char team_b[APP_NAME_LEN];
            app_fonts_sanitize(s_fav_snap.next_match.team_A, team_a, sizeof(team_a));
            app_fonts_sanitize(s_fav_snap.next_match.team_B, team_b, sizeof(team_b));
            char vs_text[96];
            snprintf(vs_text, sizeof(vs_text), "%s vs %s", team_a, team_b);
            clipped_text(c3, APP_FONT_BODY, C_TEXT, vs_text, 10, 26, SCR_W - 32, 22, LV_TEXT_ALIGN_CENTER);

            const char *loc = s_fav_snap.next_match.is_home ? "主场出战" : "客场远征";
            clipped_text(c3, APP_FONT_SMALL, C_MUTED, loc, 0, 52, SCR_W - 12, 16, LV_TEXT_ALIGN_CENTER);
        } else {
            clipped_text(c3, APP_FONT_SMALL, C_MUTED, "未来赛程待定", 0, 34, SCR_W - 12, 20, LV_TEXT_ALIGN_CENTER);
        }
    }
}

// ---------------------------------------------------------------- 页面:比分

// 今天没有白名单赛事时,快照里装的其实是"下一个比赛日"的数据,
// day_offset 就不是 0。此时顶部要显示倒计时,列表让出一行。
// day_offset < 0 是"七天都没有",那种情况由空状态文案负责,不再挂横幅——
// 横幅和正文说同一句话是浪费屏幕。
static bool show_banner(void) {
    return s_snap.state == APP_NET_READY && s_snap.day_offset > 0;
}

static int list_rows(void) {
    return show_banner() ? LIST_ROWS - 1 : LIST_ROWS;
}

static int list_y0(void) {
    return show_banner() ? LIST_Y0_BANNER : ROW_Y0;
}

// "下一个比赛日"横幅。左边说是哪一天,右边说还有几天。
static void build_banner(void) {
    if (!show_banner()) return;

    lv_obj_t *bar = panel(s_body, 8, BANNER_Y, SCR_W - 16, BANNER_H, C_PANEL, 6);
    if (bar == NULL) return;

    // 48 字节:"后天 " + 日期 + " · 还有 N 天" 最坏 39 字节,GCC 的
    // -Wformat-truncation 会按最坏情况算,给窄了直接编译失败。
    char right[48];
    // 目标日 = 刷新时刻那天 + day_offset。日期用月-日,不占宽度。
    char date[16] = "--";
    app_datetime_t dt;
    if (app_datetime_from_utc(app_cst_day_start(s_snap.updated_at, s_snap.day_offset), &dt)) {
        snprintf(date, sizeof(date), "%d-%02d", dt.month, dt.day);
    }

    if (s_snap.day_offset == 1) {
        snprintf(right, sizeof(right), "明天 %s · 还有 1 天", date);
    } else if (s_snap.day_offset == 2) {
        snprintf(right, sizeof(right), "后天 %s · 还有 2 天", date);
    } else {
        snprintf(right, sizeof(right), "%s · 还有 %d 天", date, (int)s_snap.day_offset);
    }

    // 宽度要给"字宽 + 省略号"留够:DOTS 模式先扣掉省略号那一格再排字,
    // 五个 12 px 汉字是 60 px,给 70 只剩 58,于是永远显示成「下一个比…」。
    clipped_text(bar, APP_FONT_SMALL, C_MUTED, "下一个比赛日", 8, 3, 76, 16,
                 LV_TEXT_ALIGN_LEFT);
    clipped_text(bar, APP_FONT_SMALL, C_AMBER, right, 84, 3, SCR_W - 100, 16,
                 LV_TEXT_ALIGN_RIGHT);
}

static void build_empty_state(void) {
    // 默认文案解释"为什么是空的":看板只收当天 + 九项赛事,
    // 用户看到空列表时要能立刻明白这是筛选结果,不是没联网。
    const char *title = "今天暂无比赛";
    const char *why = "只显示五大联赛、中超中甲、欧冠欧联";
    // 32 字节:"%s 没有比赛" 里的 %s 最坏是 15 字节的 date,GCC 按最坏情况
    // 算要 29 字节,给 24 会直接 -Werror=format-truncation 编译失败。
    char replay_title[32];

    switch (s_snap.state) {
        case APP_NET_OFF:
            title = "未配网";
            why = "长按 OK 进设置,选 Wi-Fi 配网";
            break;
        case APP_NET_CONNECTING:
            title = "正在连接";
            why = s_snap.ssid[0] != '\0' ? s_snap.ssid : "请稍候";
            break;
        case APP_NET_SYNCING:
            title = "正在校时";
            why = "TLS 需要正确的系统时间";
            break;
        case APP_NET_FETCHING:
            // 翻页是用户主动切过去的,得说清"在拉哪一天",否则和普通刷新
            // 看起来一样,用户会以为按错了。
            if (s_snap.view_day < 0) title = "正在拉取前日比分";
            else if (s_snap.view_day > 0) title = "正在拉取未来赛程";
            else title = "正在拉取比分";
            why = "首次加载约需几秒";
            break;
        case APP_NET_ERROR:
            title = "刷新失败";
            why = s_snap.error[0] != '\0' ? s_snap.error : "按 OK 重试";
            break;
        default:
            break;
    }

    // 翻到别的日子时,"空"和今天不是一回事:不是"还没到比赛日",而是"那一天
    // 这几项赛事没有比赛"。标题直接带上日期。
    if (s_snap.view_day != 0 && s_snap.state == APP_NET_READY) {
        char date[16] = "--";
        app_datetime_t dt;
        if (app_datetime_from_utc(
                app_cst_day_start(s_snap.updated_at, s_snap.view_day), &dt)) {
            snprintf(date, sizeof(date), "%d-%02d", dt.month, dt.day);
        }
        snprintf(replay_title, sizeof(replay_title), "%s 没有比赛", date);
        title = replay_title;
    }

    // 这两条要压过状态机文案,所以放在 switch 之后:
    //   · 正在往后翻日子——要连下几份响应,不说清楚用户会以为卡住;
    //   · 已经翻完七天、确实一场都没有——这是结论,不是还在等。
    if (s_snap.probing_next_day) {
        title = "今天没有比赛";
        why = "正在查找下一个比赛日…";
    } else if (s_snap.state == APP_NET_READY && s_snap.day_offset < 0) {
        title = "近期没有比赛";
        why = "未来七天没有五大联赛赛事";
    }

    clipped_text(s_body, APP_FONT_TITLE, C_TEXT, title, 10, 78, SCR_W - 20, 26,
                 LV_TEXT_ALIGN_CENTER);
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, why, 16, 110, SCR_W - 32, 40,
                 LV_TEXT_ALIGN_CENTER);
}

static void build_scores(void) {
    const int count = (int)s_snap.matches.count;

    build_banner();

    if (count == 0) {
        s_nav.sel = 0;
        s_nav.scroll = 0;
        build_empty_state();
        return;
    }

    const int rows = list_rows();
    const int y0 = list_y0();

    if (s_nav.sel > count - 1) s_nav.sel = count - 1;
    if (s_nav.sel < 0) s_nav.sel = 0;
    const int max_scroll = count > rows ? count - rows : 0;
    if (s_nav.scroll > max_scroll) s_nav.scroll = max_scroll;
    if (s_nav.scroll < 0) s_nav.scroll = 0;

    // 从详情页返回时 sel 被直接改回原值(scroll 已复位),这里把窗口跟上去,
    // 否则高亮项会落在可视区外面。
    if (s_nav.sel < s_nav.scroll) s_nav.scroll = s_nav.sel;
    if (s_nav.sel >= s_nav.scroll + rows) s_nav.scroll = s_nav.sel - rows + 1;
    if (s_nav.scroll > max_scroll) s_nav.scroll = max_scroll;
    if (s_nav.scroll < 0) s_nav.scroll = 0;

    for (int row = 0; row < rows; row++) {
        const int index = s_nav.scroll + row;
        if (index >= count) break;

        const app_match_t *match = &s_snap.matches.items[index];
        const bool selected = (index == s_nav.sel);
        const int y = y0 + row * ROW_PITCH;

        lv_obj_t *item = panel(s_body, 8, y, SCR_W - 16, ROW_H,
                               selected ? C_PANEL_HI : C_PANEL, 8);
        if (selected) {
            (void)panel(item, 0, 8, 3, ROW_H - 16, C_ACCENT, 2);
        }

        // 列表里全是今天的比赛,再给每行加"今天"前缀只是噪音,还挤占
        // 状态列那 84 px。直接显示开赛时间。
        char state[24];
        app_match_state_label(match, state, sizeof(state));

        char score[12];
        app_match_score_text(match, score, sizeof(score));

        // 网络来的字符串先过一遍字体覆盖检查:子集里没有的字显示 '?',
        // 而不是一个空心方框。
        char league[APP_NAME_LEN];
        char home[APP_NAME_LEN];
        char away[APP_NAME_LEN];
        app_fonts_sanitize(match->league, league, sizeof(league));
        app_fonts_sanitize(match->home, home, sizeof(home));
        app_fonts_sanitize(match->away, away, sizeof(away));

        clipped_text(item, APP_FONT_SMALL, C_MUTED, league, 8, 4, 120, 14,
                     LV_TEXT_ALIGN_LEFT);
        clipped_text(item, APP_FONT_SMALL, state_color(match), state, 128, 4, 84, 14,
                     LV_TEXT_ALIGN_RIGHT);
        clipped_text(item, APP_FONT_BODY, selected ? C_TEXT : C_TEAM, home, 8, 24, 74, 20,
                     LV_TEXT_ALIGN_LEFT);
        clipped_text(item, APP_FONT_SCORE, C_TEXT, score, 86, 19, 52, 26,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(item, APP_FONT_BODY, selected ? C_TEXT : C_TEAM, away, 142, 24, 74, 20,
                     LV_TEXT_ALIGN_RIGHT);
    }
}

// ---------------------------------------------------------------- 页面:详情

// "45'"、"90+3'"。补时写在分钟后面,不另占一列。
static void goal_minute_text(const app_goal_t *goal, char *buf, size_t cap) {
    if (goal->extra > 0) {
        snprintf(buf, cap, "%u+%u'", (unsigned)goal->minute, (unsigned)goal->extra);
    } else {
        snprintf(buf, cap, "%u'", (unsigned)goal->minute);
    }
}

// 队徽的贴图描述符。每个槽位一份:详情页上两队徽同时可见,而 lv_image 只存
// 指针不拷数据,所以描述符必须和像素缓冲一样活得比界面对象长——放静态区。
//
// 用 lv_image 而不是 lv_canvas:canvas 每个实例要配一块绘制缓冲(32×32 就是
// 2 KB × 2),而 image 只要这 32 字节的描述符,像素直接指向 app_crest 解好的
// 那份 RGB565。
static lv_image_dsc_t s_crest_dsc[APP_CREST_SLOTS];

// 把某个槽位的队徽贴到 parent 上。拿不到图(没 URL、还在下、下失败)时画一块
// 和徽章等大的亮面色块:版面不会因为图没到就塌下去,用户也能看出"这里该有个
// 徽章,只是还没来"。
static void draw_crest(lv_obj_t *parent, int slot, int x, int y) {
    if (parent == NULL || slot < 0 || slot >= APP_CREST_SLOTS) return;

    const app_png_image_t *img = app_crest_image(slot);
    if (img == NULL || img->w == 0 || img->h == 0) {
        (void)panel(parent, x, y, CREST_SIZE, CREST_SIZE, C_PANEL_HI, 4);
        return;
    }

    lv_image_dsc_t *dsc = &s_crest_dsc[slot];
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0;
    dsc->header.w = img->w;
    dsc->header.h = img->h;
    dsc->header.stride = (uint32_t)img->w * 2u;
    dsc->data_size = (uint32_t)img->w * (uint32_t)img->h * 2u;
    dsc->data = (const uint8_t *)img->pixels;
    dsc->reserved = NULL;
    dsc->reserved_2 = NULL;

    lv_obj_t *o = lv_image_create(parent);
    if (o == NULL) return;
    lv_image_set_src(o, dsc);
    lv_obj_set_pos(o, x, y);
}

// 比分条:联赛/状态 + 两队徽 + 主队 · 比分 · 客队。详情页两页共用同一份数据格式,
// 但这里用的是"打开那一刻"的副本,所以拉取过程中数字不会跳。
static void build_detail_bar(void) {
    char league[APP_NAME_LEN];
    char home[APP_NAME_LEN];
    char away[APP_NAME_LEN];
    app_fonts_sanitize(s_detail_match.league, league, sizeof(league));
    app_fonts_sanitize(s_detail_match.home, home, sizeof(home));
    app_fonts_sanitize(s_detail_match.away, away, sizeof(away));

    char score[12];
    app_match_score_text(&s_detail_match, score, sizeof(score));

    char state[24];
    app_match_state_label(&s_detail_match, state, sizeof(state));

    lv_obj_t *bar = panel(s_body, 8, 0, SCR_W - 16, DETAIL_BAR_H, C_PANEL, 8);
    if (bar == NULL) return;

    // 队徽的透明像素在解码时就按 APP_CREST_BG_RGB565 混成了这块面板的颜色,
    // 所以这里直接贴上去就行,不需要再做 alpha 合成。
    draw_crest(bar, 0, CREST_X_HOME, 18);
    draw_crest(bar, 1, CREST_X_AWAY, 18);

    clipped_text(bar, APP_FONT_SMALL, C_MUTED, league, 8, 3, 130, 14, LV_TEXT_ALIGN_LEFT);
    clipped_text(bar, APP_FONT_SMALL, state_color(&s_detail_match), state, 118, 3, 94, 14,
                 LV_TEXT_ALIGN_RIGHT);
    // 比分和队徽同一行,垂直居中于那 32 px。
    clipped_text(bar, APP_FONT_SCORE, C_TEXT, score, 88, 20, 48, 32, LV_TEXT_ALIGN_CENTER);
    clipped_text(bar, APP_FONT_SMALL, C_TEAM, home, 6, 51, 74, 16, LV_TEXT_ALIGN_LEFT);
    clipped_text(bar, APP_FONT_SMALL, C_TEAM, away, 144, 51, 74, 16, LV_TEXT_ALIGN_RIGHT);
}

// 未开赛的比赛,详情接口里就是空的(187 字节、events/statistics 都是 [])。
// 这是"还没有",不是"加载失败",也不能显示成"0 个进球"——两种都得单独说清楚。
//
// 只判 FIXTURE:推迟/取消/待定在 app_json 里就被筛掉了(见 app_json.c 的分流),
// 根本进不了列表,更进不了详情页。
// 另加一道"真的一份数据都没有":列表快照可能落后于接口,万一这场已经开踢,
// 就不能拿"未开始"把真实赛况盖掉。
static bool detail_not_started(void) {
    if (s_detail.detail.goal_count > 0 || s_detail.detail.stat_count > 0) return false;
    return s_detail_match.state == APP_STATE_FIXTURE;
}

static void build_detail_pending(void) {
    clipped_text(s_body, APP_FONT_TITLE, C_TEXT, "比赛未开始", 10, 92, SCR_W - 20, 26,
                 LV_TEXT_ALIGN_CENTER);
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "开赛后显示进球与技术统计", 16, 126,
                 SCR_W - 32, 20, LV_TEXT_ALIGN_CENTER);
}

static void build_detail_goals(void) {
    build_detail_bar();

    const app_detail_t *d = &s_detail.detail;

    if (s_detail.state == APP_DETAIL_LOADING) {
        clipped_text(s_body, APP_FONT_TITLE, C_TEXT, "正在加载赛况…", 10, 96, SCR_W - 20, 26,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "进球与技术统计", 16, 128, SCR_W - 32, 20,
                     LV_TEXT_ALIGN_CENTER);
        return;
    }
    if (s_detail.state == APP_DETAIL_FAILED) {
        clipped_text(s_body, APP_FONT_TITLE, C_TEXT, "赛况加载失败", 10, 92, SCR_W - 20, 26,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_LIVE,
                     s_detail.error[0] != '\0' ? s_detail.error : "网络不给力",
                     16, 122, SCR_W - 32, 20, LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "按 OK 重试", 16, 148, SCR_W - 32, 20,
                     LV_TEXT_ALIGN_CENTER);
        return;
    }
    if (detail_not_started()) {
        build_detail_pending();
        return;
    }

    // 小节标题:左"进球",右进球数。列表为空时也要有一行说明,否则
    // 0-0 的比赛看起来像"页面没加载出来"。
    char count_text[16];
    snprintf(count_text, sizeof(count_text), "%u 个", (unsigned)d->goal_count);
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "进球", 12, 72, 60, 14, LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, count_text, SCR_W - 72, 72, 60, 14,
                 LV_TEXT_ALIGN_RIGHT);

    if (d->goal_count == 0) {
        const char *why = s_detail_match.state == APP_STATE_FIXTURE ? "还没开球" : "还没有进球";
        clipped_text(s_body, APP_FONT_BODY, C_MUTED, why, 16, 96, SCR_W - 32, 22,
                     LV_TEXT_ALIGN_CENTER);
        return;
    }

    // 超过 8 个进球时,最后一行的位置留给"还有几个",比硬截断诚实。
    const size_t shown = d->goal_count > (size_t)GOAL_ROWS ? (size_t)GOAL_ROWS - 1
                                                           : d->goal_count;

    for (size_t i = 0; i < shown; i++) {
        const app_goal_t *goal = &d->goals[i];
        const int y = GOAL_ROW_Y0 + (int)i * GOAL_ROW_PITCH;

        if ((i % 2) == 0) {
            (void)panel(s_body, 8, y - 2, SCR_W - 16, GOAL_ROW_H, C_PANEL, 4);
        }

        char minute[16];
        goal_minute_text(goal, minute, sizeof(minute));
        if (goal->kind == APP_GOAL_PENALTY) {
            strncat(minute, "点", sizeof(minute) - strlen(minute) - 1);
        } else if (goal->kind == APP_GOAL_OWN) {
            strncat(minute, "乌", sizeof(minute) - strlen(minute) - 1);
        }

        const uint32_t minute_color = (goal->kind == APP_GOAL_PENALTY) ? C_AMBER
                                      : (goal->kind == APP_GOAL_OWN) ? C_LIVE
                                                                     : C_ACCENT;

        // 只有进球的一方出现在自己那半边,中间是分钟。左右对称,不用画箭头。
        char player[APP_PLAYER_LEN];
        app_fonts_sanitize(goal->player, player, sizeof(player));

        clipped_text(s_body, APP_FONT_SMALL, C_TEAM,
                     goal->side == 0 ? player : "", 10, y, 82, 16, LV_TEXT_ALIGN_RIGHT);
        clipped_text(s_body, APP_FONT_SMALL, minute_color, minute, 96, y, 48, 16,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_TEAM,
                     goal->side == 1 ? player : "", 148, y, 82, 16, LV_TEXT_ALIGN_LEFT);
    }

    if (d->goal_count > GOAL_ROWS) {
        char more[32];
        snprintf(more, sizeof(more), "还有 %u 个进球", (unsigned)(d->goal_count - shown));
        const int y = GOAL_ROW_Y0 + (int)shown * GOAL_ROW_PITCH;
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, more, 10, y, SCR_W - 20, 16,
                     LV_TEXT_ALIGN_CENTER);
    }
}

static void build_detail_stats(void) {
    build_detail_bar();

    const app_detail_t *d = &s_detail.detail;

    if (s_detail.state != APP_DETAIL_READY) {
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED,
                     s_detail.state == APP_DETAIL_LOADING ? "正在加载赛况…" : "赛况未就绪",
                     16, 110, SCR_W - 32, 20, LV_TEXT_ALIGN_CENTER);
        return;
    }

    if (detail_not_started()) {
        build_detail_pending();
        return;
    }

    if (d->stat_count == 0) {
        clipped_text(s_body, APP_FONT_BODY, C_MUTED, "暂无技术统计", 16, 116, SCR_W - 32, 22,
                     LV_TEXT_ALIGN_CENTER);
        return;
    }

    for (size_t i = 0; i < d->stat_count && i < 8; i++) {
        const app_stat_t *stat = &d->stats[i];
        const int y = STAT_ROW_Y0 + (int)i * STAT_ROW_PITCH;

        char label[APP_STAT_LABEL_LEN];
        app_fonts_sanitize(stat->label, label, sizeof(label));

        char home[8];
        char away[8];
        snprintf(home, sizeof(home), "%d", (int)stat->home);
        snprintf(away, sizeof(away), "%d", (int)stat->away);

        clipped_text(s_body, APP_FONT_BODY, C_TEXT, home, 10, y, 44, 20, LV_TEXT_ALIGN_LEFT);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, label, 58, y + 2, 124, 16,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_BODY, C_TEXT, away, 186, y, 44, 20, LV_TEXT_ALIGN_RIGHT);
    }
}

static void build_detail(void) {
    if (s_nav.sub == 1) build_detail_stats();
    else build_detail_goals();
}

// 打开选中场次的详情。先切页再请求,用户立刻能看到比分条与"正在加载"。
static void open_detail(void) {
    if (s_snap.matches.count == 0 || s_nav.sel < 0
        || s_nav.sel >= (int)s_snap.matches.count) {
        toast("今天暂无比赛");
        update_footer();
        return;
    }

    s_detail_index = s_nav.sel;
    s_detail_match = s_snap.matches.items[s_nav.sel];

    // 立刻作废上一场的队徽。新一场的 URL 要等详情响应到了才知道,而屏幕这一
    // 刻就已经重画了——不先撤掉,旧徽章会在新页面上闪一下。
    app_crest_request(0, NULL);
    app_crest_request(1, NULL);
    s_crest_revision = app_crest_revision();

    app_detail_reset(&s_detail.detail);
    s_detail.state = APP_DETAIL_LOADING;
    s_detail.error[0] = '\0';

    app_nav_goto(&s_nav, APP_PAGE_DETAIL);
    build_body();

    if (!app_net_detail_request(s_detail_match.id)) {
        s_detail.state = APP_DETAIL_FAILED;
        copy_text(s_detail.error, sizeof(s_detail.error), "网络未就绪");
        build_body();
        return;
    }
    s_detail_revision = app_net_detail_revision();
    ESP_LOGI(TAG, "打开详情:%s", s_detail_match.id);
}

// 详情页按 OK:重拉这一场。列表可能在刷新,但详情只认自己那场的 id。
static void reload_detail(void) {
    s_detail.state = APP_DETAIL_LOADING;
    s_detail.error[0] = '\0';
    build_body();

    if (!app_net_detail_request(s_detail_match.id)) {
        s_detail.state = APP_DETAIL_FAILED;
        copy_text(s_detail.error, sizeof(s_detail.error), "网络未就绪");
        build_body();
        return;
    }
    s_detail_revision = app_net_detail_revision();
}

// ---------------------------------------------------------------- 页面:设置

static void settings_row_text(app_set_row_t row, char *value, size_t cap) {
    switch (row) {
        case APP_SET_ROW_WIFI:
            // "连接失败"只能用在 Wi-Fi 本身连不上时。拉取失败、校时失败也会把
            // state 置成 APP_NET_ERROR,但那时链路是通的(IP 都拿到了),所以先看
            // wifi_connected,否则会出现"地址栏写着 192.168.8.131、状态说连接失败"
            // 这种自相矛盾的画面。拉取类的错误由比分页的空状态负责说明。
            if (s_snap.wifi_connected) {
                snprintf(value, cap, "%s", s_snap.ssid[0] ? s_snap.ssid : "已连接");
            } else if (s_snap.state == APP_NET_CONNECTING || s_snap.state == APP_NET_SYNCING
                       || s_snap.state == APP_NET_FETCHING) {
                snprintf(value, cap, "连接中");
            } else if (s_snap.state == APP_NET_ERROR) {
                snprintf(value, cap, "连接失败");
            } else {
                snprintf(value, cap, "未配网");
            }
            break;
        case APP_SET_ROW_PORTAL:
            // 手机上配网,屏上只需要说"怎么做",所以这一行的右值给一句提示。
            snprintf(value, cap, "手机网页");
            break;
        case APP_SET_ROW_STATUS:
            snprintf(value, cap, "OK 查看");
            break;
        case APP_SET_ROW_INTERVAL:
            snprintf(value, cap, "%u 秒", (unsigned)s_cfg.refresh_seconds);
            break;
        case APP_SET_ROW_SCREEN_OFF:
            // 0 是"常亮"而不是"0 秒",这一档要说人话。
            if (s_cfg.screen_off_seconds == 0) snprintf(value, cap, "常亮");
            else snprintf(value, cap, "%u 秒", (unsigned)s_cfg.screen_off_seconds);
            break;
        case APP_SET_ROW_BRIGHTNESS:
            snprintf(value, cap, "%u%%", (unsigned)s_cfg.brightness_percent);
            break;
        case APP_SET_ROW_CLEAR:
            snprintf(value, cap, "OK 执行");
            break;
        default:
            value[0] = '\0';
            break;
    }
}

// 只改选中态,不重建对象。翻行走这条路:改 7 行颜色是几十微秒的事,
// 而重建整页要删掉再新建 30 多个对象、再全屏重绘一遍。
static void settings_select(int sel) {
    for (int row = 0; row < APP_SET_ROW_COUNT; row++) {
        const bool on = (row == sel);
        if (s_set_rows[row] != NULL) {
            lv_obj_set_style_bg_color(s_set_rows[row],
                                      lv_color_hex(on ? C_PANEL_HI : C_PANEL), 0);
        }
        if (s_set_bars[row] != NULL) {
            lv_obj_set_style_bg_opa(s_set_bars[row], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (s_set_labels[row] != NULL) {
            lv_obj_set_style_text_color(s_set_labels[row],
                                        lv_color_hex(on ? C_TEXT : C_TEAM), 0);
        }
        if (s_set_values[row] != NULL) {
            lv_obj_set_style_text_color(s_set_values[row],
                                        lv_color_hex(on ? C_ACCENT : C_MUTED), 0);
        }
    }
}

static void build_settings(void) {
    static const char *const LABELS[APP_SET_ROW_COUNT] = {
        "Wi-Fi 配网", "手机配网", "系统状态", "刷新间隔",
        "自动息屏", "屏幕亮度", "清除配网",
    };

    for (int row = 0; row < APP_SET_ROW_COUNT; row++) {
        const int y = SET_ROW_Y0 + row * SET_ROW_PITCH;
        const bool selected = (row == s_nav.sel);
        lv_obj_t *item = panel(s_body, 10, y, SCR_W - 20, SET_ROW_H,
                               selected ? C_PANEL_HI : C_PANEL, 8);
        s_set_rows[row] = item;
        // 左侧那根 3 px 的高亮条一直建出来,只是不选中时透明。翻行时改
        // 不透明度,比每次删了重建省一次分配(见 settings_select)。
        s_set_bars[row] = item ? panel(item, 0, 7, 3, SET_ROW_H - 14, C_ACCENT, 2) : NULL;
        if (s_set_bars[row] != NULL) {
            lv_obj_set_style_bg_opa(s_set_bars[row], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }

        char value[40];
        settings_row_text((app_set_row_t)row, value, sizeof(value));

        s_set_labels[row] = clipped_text(item, APP_FONT_BODY,
                                         selected ? C_TEXT : C_TEAM, LABELS[row], 10,
                                         SET_ROW_TEXT_Y, 96, 20, LV_TEXT_ALIGN_LEFT);
        s_set_values[row] = clipped_text(item, APP_FONT_BODY,
                                         selected ? C_ACCENT : C_MUTED, value, 108,
                                         SET_ROW_TEXT_Y, 100, 20, LV_TEXT_ALIGN_RIGHT);
    }
}

// ---------------------------------------------------------------- 页面:系统状态 (二级菜单)

static void build_status(void) {
    char s_clean[96];

    // 卡片 1: Wi-Fi 网络
    lv_obj_t *c1 = panel(s_body, 10, 4, SCR_W - 20, 48, C_PANEL, 8);
    if (c1 != NULL) {
        clipped_text(c1, APP_FONT_BODY, C_TEXT, "Wi-Fi 网络", 10, 5, 100, 18, LV_TEXT_ALIGN_LEFT);

        char rssi_str[32];
        if (!s_snap.wifi_connected) {
            snprintf(rssi_str, sizeof(rssi_str), "未连接");
        } else if (s_snap.rssi >= -60) {
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm 极佳", s_snap.rssi);
        } else if (s_snap.rssi >= -75) {
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm 良好", s_snap.rssi);
        } else {
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm 较弱", s_snap.rssi);
        }
        clipped_text(c1, APP_FONT_SMALL, s_snap.wifi_connected ? C_ACCENT : C_MUTED,
                     rssi_str, 110, 6, 96, 16, LV_TEXT_ALIGN_RIGHT);

        char line1[96];
        if (s_snap.wifi_connected) {
            snprintf(line1, sizeof(line1), "SSID: %s  IP: %s",
                     s_snap.ssid[0] ? s_snap.ssid : "已连",
                     s_snap.ip[0] ? s_snap.ip : "--");
        } else {
            snprintf(line1, sizeof(line1), "未连接路由器, 请在设置中配网");
        }
        app_fonts_sanitize(line1, s_clean, sizeof(s_clean));
        clipped_text(c1, APP_FONT_SMALL, C_MUTED, s_clean, 10, 26, 200, 16, LV_TEXT_ALIGN_LEFT);
    }

    // 卡片 2: 懂球帝数据源
    lv_obj_t *c2 = panel(s_body, 10, 56, SCR_W - 20, 48, C_PANEL, 8);
    if (c2 != NULL) {
        clipped_text(c2, APP_FONT_BODY, C_TEXT, "懂球帝数据源", 10, 5, 110, 18, LV_TEXT_ALIGN_LEFT);

        const char *state_desc = "正常通信";
        uint32_t state_col = C_ACCENT;
        if (!s_snap.wifi_connected) {
            state_desc = "网络断开";
            state_col = C_MUTED;
        } else if (s_snap.state == APP_NET_ERROR) {
            state_desc = "连接失败";
            state_col = C_LIVE;
        } else if (s_snap.state == APP_NET_FETCHING) {
            state_desc = "同步中…";
            state_col = C_AMBER;
        } else if (s_snap.state == APP_NET_SYNCING) {
            state_desc = "校时中…";
            state_col = C_AMBER;
        } else if (s_snap.updated_at == 0) {
            state_desc = "等待拉取";
            state_col = C_MUTED;
        }
        clipped_text(c2, APP_FONT_SMALL, state_col, state_desc, 120, 6, 86, 16, LV_TEXT_ALIGN_RIGHT);

        char line2[96];
        if (s_snap.updated_at > 0) {
            char clock[12];
            app_format_clock(s_snap.updated_at, clock, sizeof(clock));
            snprintf(line2, sizeof(line2), "sd.dongdianqiu.com 更新于 %s", clock);
        } else if (s_snap.error[0]) {
            snprintf(line2, sizeof(line2), "错误: %s", s_snap.error);
        } else {
            snprintf(line2, sizeof(line2), "sd.dongdianqiu.com (HTTP)");
        }
        app_fonts_sanitize(line2, s_clean, sizeof(s_clean));
        clipped_text(c2, APP_FONT_SMALL, C_MUTED, s_clean, 10, 26, 200, 16, LV_TEXT_ALIGN_LEFT);
    }

    // 卡片 3: 电池与供电
    lv_obj_t *c3 = panel(s_body, 10, 108, SCR_W - 20, 48, C_PANEL, 8);
    if (c3 != NULL) {
        clipped_text(c3, APP_FONT_BODY, C_TEXT, "电池与供电", 10, 5, 100, 18, LV_TEXT_ALIGN_LEFT);

        char soc_str[24];
        if (s_battery_soc < 0) {
            snprintf(soc_str, sizeof(soc_str), "--%%");
        } else {
            snprintf(soc_str, sizeof(soc_str), "%d%%", s_battery_soc);
        }
        clipped_text(c3, APP_FONT_SMALL,
                     (s_battery_soc > 20 || s_battery_soc < 0) ? C_ACCENT : C_LIVE,
                     soc_str, 120, 6, 86, 16, LV_TEXT_ALIGN_RIGHT);

        char line3[96];
        if (s_battery_mv > 0) {
            snprintf(line3, sizeof(line3), "电量计:CW2017  电压: %d mV", s_battery_mv);
        } else {
            snprintf(line3, sizeof(line3), "电量计:CW2017 (USB供电/检测中)");
        }
        app_fonts_sanitize(line3, s_clean, sizeof(s_clean));
        clipped_text(c3, APP_FONT_SMALL, C_MUTED, s_clean, 10, 26, 200, 16, LV_TEXT_ALIGN_LEFT);
    }

    // 卡片 4: 设备运行状态
    lv_obj_t *c4 = panel(s_body, 10, 160, SCR_W - 20, 48, C_PANEL, 8);
    if (c4 != NULL) {
        clipped_text(c4, APP_FONT_BODY, C_TEXT, "设备状态", 10, 5, 90, 18, LV_TEXT_ALIGN_LEFT);

        uint32_t free_heap = esp_get_free_heap_size() / 1024;
        char heap_str[24];
        snprintf(heap_str, sizeof(heap_str), "堆 %u KB", (unsigned)free_heap);
        clipped_text(c4, APP_FONT_SMALL, C_ACCENT, heap_str, 100, 6, 106, 16, LV_TEXT_ALIGN_RIGHT);

        int64_t uptime_sec = esp_timer_get_time() / 1000000;
        int up_h = (int)(uptime_sec / 3600);
        int up_m = (int)((uptime_sec % 3600) / 60);
        int up_s = (int)(uptime_sec % 60);

        char line4[96];
        snprintf(line4, sizeof(line4), "已运行 %02d:%02d:%02d  刷新周期:%us",
                 up_h, up_m, up_s, (unsigned)s_cfg.refresh_seconds);
        app_fonts_sanitize(line4, s_clean, sizeof(s_clean));
        clipped_text(c4, APP_FONT_SMALL, C_MUTED, s_clean, 10, 26, 200, 16, LV_TEXT_ALIGN_LEFT);
    }
}

// ---------------------------------------------------------------- 页面:热点列表

static void rebuild_ap_rows(void) {
    s_ap_count = app_known_merge_rows(&s_known, s_scan.items, s_scan.count,
                                      s_ap_rows, APP_AP_ROW_MAX);

    if (s_ap_count == 0) {
        s_nav.sel = 0;
        s_nav.scroll = 0;
        return;
    }
    if (s_nav.sel > (int)s_ap_count - 1) s_nav.sel = (int)s_ap_count - 1;
    if (s_nav.sel < 0) s_nav.sel = 0;

    const int max_scroll = s_ap_count > AP_ROWS ? (int)s_ap_count - AP_ROWS : 0;
    if (s_nav.scroll > max_scroll) s_nav.scroll = max_scroll;
    if (s_nav.scroll < 0) s_nav.scroll = 0;
}

static void build_wifi_list(void) {
    if (s_ap_count == 0) {
        // 空列表也必须说清"为什么空、下一步做什么",否则按 OK 看起来就是没反应。
        const char *title;
        const char *why;
        if (s_scan.scanning) {
            title = "正在扫描…";
            why = "附近的热点会显示在这里";
        } else if (s_scan.failed) {
            title = "扫描失败";
            why = "Wi-Fi 未就绪,按 OK 重试";
        } else if (s_scan.done) {
            title = "没有发现网络";
            why = "按 OK 重新扫描";
        } else {
            title = "还没扫描";
            why = "按 OK 开始扫描";
        }
        clipped_text(s_body, APP_FONT_TITLE, C_TEXT, title, 10, 78, SCR_W - 20, 26,
                     LV_TEXT_ALIGN_CENTER);
        clipped_text(s_body, APP_FONT_SMALL, C_MUTED, why, 16, 110, SCR_W - 32, 40,
                     LV_TEXT_ALIGN_CENTER);
        return;
    }

    const int count = (int)s_ap_count;
    for (int row = 0; row < AP_ROWS; row++) {
        const int index = s_nav.scroll + row;
        if (index >= count) break;

        const app_ap_row_t *ap = &s_ap_rows[index];
        const bool selected = (index == s_nav.sel);
        const int y = AP_ROW_Y0 + row * AP_ROW_PITCH;

        lv_obj_t *item = panel(s_body, 8, y, SCR_W - 16, AP_ROW_H,
                               selected ? C_PANEL_HI : C_PANEL, 8);
        if (selected) {
            (void)panel(item, 0, 8, 3, AP_ROW_H - 16, C_ACCENT, 2);
        }

        // 热点名可能是任意 UTF-8(带表情符号的 SSID 很常见),同样过一遍覆盖检查。
        char name[APP_SSID_MAX + 1];
        app_fonts_sanitize(ap->ssid, name, sizeof(name));

        const char *tag = ap->saved ? "已存" : (ap->secure ? "加密" : "开放");
        const uint32_t tag_color = ap->saved ? C_ACCENT : (ap->secure ? C_MUTED : C_AMBER);

        clipped_text(item, APP_FONT_BODY, selected ? C_TEXT : C_TEAM, name, 12, 12, 128, 20,
                     LV_TEXT_ALIGN_LEFT);
        clipped_text(item, APP_FONT_SMALL, tag_color, tag, 148, 14, 80, 16,
                     LV_TEXT_ALIGN_RIGHT);
    }
}

// ---------------------------------------------------------------- 页面:密码输入

// 密码回显:只露最后一个字符,前面用 '*' 代替。这样既能确认刚按下的键对不对,
// 又不会把整串口令摊在屏幕上;超过一屏宽时左边折成省略号。
static void password_display(const app_keyboard_t *kb, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    if (kb->len == 0) {
        snprintf(out, cap, "输入密码");
        return;
    }

    const size_t max_mask = 12;
    const size_t start = kb->len > max_mask ? kb->len - max_mask : 0;
    size_t n = 0;
    // 省略号是 3 字节的 UTF-8,必须整段写进去。写成 '…' 会被当成多字节
    // 字符常量截成单字节,屏幕上就是一个乱码方块。
    if (start > 0 && n + 3 < cap) {
        memcpy(out + n, "\xE2\x80\xA6", 3);
        n += 3;
    }
    for (size_t i = start; i + 1 < kb->len && n + 1 < cap; i++) out[n++] = '*';
    if (n + 1 < cap) out[n++] = kb->text[kb->len - 1];
    out[n] = '\0';
}

// 只更新键盘与密码框,不重建对象:每按一键重建 42 个对象太浪费堆。
static void refresh_keyboard(void) {
    for (int i = 0; i < APP_KB_KEYS; i++) {
        // 堆紧张时 lv_obj_create 会返回 NULL,少画一个键好过写到野指针上。
        if (s_kb_keys[i] == NULL) continue;

        char label[8];
        app_keyboard_label(&s_kb, i, label, sizeof(label));
        lv_label_set_text(s_kb_keys[i], label);

        const bool selected = ((int)s_kb.cursor == i);
        lv_obj_set_style_bg_color(s_kb_keys[i], lv_color_hex(selected ? C_PANEL_HI : C_PANEL), 0);
        lv_obj_set_style_border_width(s_kb_keys[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_kb_keys[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_color(s_kb_keys[i],
                                    lv_color_hex(app_keyboard_is_char(i) ? C_TEXT : C_AMBER), 0);
    }

    if (s_kb_field != NULL) {
        char masked[32];
        password_display(&s_kb, masked, sizeof(masked));
        lv_label_set_text(s_kb_field, masked);
        lv_obj_set_style_text_color(s_kb_field, lv_color_hex(s_kb.len == 0 ? C_MUTED : C_TEXT), 0);
    }
}

// 一个键 = 一个带底色的标签。标签本身能画背景和边框,不必再套一层面板,
// 42 个键就只花 42 个对象——密码页曾经是 87 个对象,是整块板子上最重的一页。
static lv_obj_t *key_cell(int x, int y) {
    lv_obj_t *key = lv_label_create(s_body);
    if (key == NULL) return NULL;
    lv_obj_set_pos(key, x, y);
    lv_obj_set_size(key, KB_KEY_W, KB_KEY_H);
    lv_label_set_long_mode(key, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(key, APP_FONT_BODY, 0);
    lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(key, 0, 0);
    lv_obj_set_style_pad_top(key, 2, 0);   // 15 px 字在 24 px 格里垂直居中
    lv_obj_set_style_bg_color(key, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(key, 0, 0);
    lv_obj_set_style_radius(key, 5, 0);
    return key;
}

static void build_wifi_pass(void) {
    char ssid[APP_SSID_MAX + 1];
    app_fonts_sanitize(s_target_ssid, ssid, sizeof(ssid));
    // 左边 SSID,右边标出密码来源:已存 = 密码框已经填好,长按就能连。
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, ssid, 12, 2, 148, 16,
                 LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_SMALL, s_target_saved ? C_ACCENT : C_AMBER,
                 s_target_saved ? "已存密码" : "需输密码", 162, 2, 66, 16,
                 LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *field = panel(s_body, 10, 20, SCR_W - 20, 30, C_PANEL, 6);
    s_kb_field = clipped_text(field, APP_FONT_BODY, C_MUTED, "", 8, 6, SCR_W - 36, 20,
                              LV_TEXT_ALIGN_LEFT);

    for (int i = 0; i < APP_KB_KEYS; i++) {
        const int col = i % APP_KB_COLS;
        const int row = i / APP_KB_COLS;
        const int x = KB_X0 + col * (KB_KEY_W + KB_KEY_GAP_X);
        const int y = KB_Y0 + row * (KB_KEY_H + KB_KEY_GAP_Y);

        s_kb_keys[i] = key_cell(x, y);
    }

    refresh_keyboard();
}

// ---------------------------------------------------------------- 配网流程

static void build_body(void);

// 用给定凭证连网:落盘、通知联网任务、回设置页看结果。
static void connect_to(const char *ssid, const char *pass) {
    app_config_t cfg;
    app_settings_get(&cfg);
    copy_text(cfg.ssid, sizeof(cfg.ssid), ssid);
    copy_text(cfg.pass, sizeof(cfg.pass), pass);

    app_settings_apply(&cfg, true);
    s_cfg = cfg;
    s_config_dirty = false;   // 刚应用的配置就是最新的,不必再拉一次

    app_nav_goto(&s_nav, APP_PAGE_SETTINGS);
    s_nav.sel = APP_SET_ROW_WIFI;
    toast("正在连接…");

    app_net_snapshot_copy(&s_snap);
    build_body();
    ESP_LOGI(TAG, "开始连接:%s", cfg.ssid);
}

static void enter_wifi_list(void) {
    (void)app_store_load_known(&s_known);
    app_net_scan_copy(&s_scan);
    s_scan_revision = s_scan.revision;
    rebuild_ap_rows();

    app_nav_goto(&s_nav, APP_PAGE_WIFI_LIST);
    // 进来就扫一次:用户按 OK 的意图就是"看看有哪些热点"。
    // 但开机预扫的结果如果还新鲜(5 分钟内),直接拿来用——否则这一页会先
    // 空着 3~5 秒,用户会以为按键没反应。
    const int64_t now_us = esp_timer_get_time();
    const bool fresh = s_scan.done && !s_scan.failed && s_scan.done_us > 0
                       && (now_us - s_scan.done_us) < 300LL * 1000 * 1000;
    if (!fresh) app_net_scan_start();
    build_body();
}

static void pick_ap(void) {
    if (s_ap_count == 0 || s_nav.sel < 0 || s_nav.sel >= (int)s_ap_count) {
        // 列表还空着:除了重新扫,必须把空态也重画一遍。只改页脚的话,
        // 用户看到的就是"按了 OK 什么都没发生"。
        app_net_scan_start();
        toast("正在扫描附近网络…");
        app_net_scan_copy(&s_scan);
        s_scan_revision = s_scan.revision;
        build_body();
        return;
    }

    const app_ap_row_t *row = &s_ap_rows[s_nav.sel];
    ESP_LOGI(TAG, "选择热点:%s(已存=%d 加密=%d)", row->ssid, (int)row->saved,
             (int)row->secure);

    if (!row->secure) {
        // 开放网络没有密码可输,直接连。
        connect_to(row->ssid, "");
        return;
    }

    // 加密网络一律进密码页。以前"已存就直接连、跳过密码页",结果用户选中
    // 第一条(往往正是上次连过的网络)按 OK 会被直接弹回设置页,看起来就是
    // OK 没生效。现在改成:已存的把密码预填进去,长按 OK 即连,想改也能改。
    copy_text(s_target_ssid, sizeof(s_target_ssid), row->ssid);
    s_target_secure = true;
    s_target_saved = row->saved;

    app_keyboard_reset(&s_kb);
    if (row->saved) {
        const char *pass = app_known_password(&s_known, row->ssid);
        if (pass != NULL) app_keyboard_set_text(&s_kb, pass);
    }

    app_nav_goto(&s_nav, APP_PAGE_WIFI_PASS);
    build_body();
}

static void submit_password(void) {
    if (s_kb.len == 0 && s_target_secure) {
        toast("密码不能为空");
        update_footer();
        return;
    }
    connect_to(s_target_ssid, s_kb.text);
}

// ---------------------------------------------------------------- 页面:手机配网

// 这一页只讲两件事:连哪个热点、打开哪个网址。剩下的全在手机上完成,
// 屏上不用按键,所以三个键在这里只负责"重来"和"退出"。
static void build_portal(void) {
    const bool ok = s_portal.state == APP_PORTAL_OK;
    const bool bad = s_portal.state == APP_PORTAL_FAILED;

    lv_obj_t *bar = panel(s_body, 10, 2, SCR_W - 20, 30,
                          bad ? C_LIVE : (ok ? C_ACCENT : C_AMBER), 8);
    if (bar != NULL) {
        clipped_text(bar, APP_FONT_BODY, C_BG, s_portal.message, 8, 6, SCR_W - 36, 20,
                     LV_TEXT_ALIGN_CENTER);
    }

    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "第 1 步  手机连接这个 Wi-Fi", 12, 42, 216,
                 16, LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_TITLE, C_TEXT, APP_PORTAL_AP_SSID, 12, 60, 216, 24,
                 LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_BODY, C_MUTED, "密码 folotoy123", 12, 88, 216, 20,
                 LV_TEXT_ALIGN_LEFT);

    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "第 2 步  浏览器打开", 12, 118, 216, 16,
                 LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_TITLE, C_ACCENT, APP_PORTAL_AP_IP, 12, 136, 216, 24,
                 LV_TEXT_ALIGN_LEFT);

    (void)panel(s_body, 12, 172, SCR_W - 24, 1, C_PANEL_HI, 0);

    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "在手机上选网络、输密码", 12, 182, 216, 16,
                 LV_TEXT_ALIGN_LEFT);
    clipped_text(s_body, APP_FONT_SMALL, C_MUTED, "设备会自动连接并记住密码", 12, 202, 216, 16,
                 LV_TEXT_ALIGN_LEFT);

    if (s_portal.ssid[0] != '\0' && !ok) {
        char line[APP_SSID_MAX + 8];
        snprintf(line, sizeof(line), "目标:%s", s_portal.ssid);
        clipped_text(s_body, APP_FONT_SMALL, C_ACCENT, line, 12, 226, 216, 16,
                     LV_TEXT_ALIGN_LEFT);
    }
}

static void enter_portal(void) {
    app_nav_goto(&s_nav, APP_PAGE_PORTAL);
    // 幂等:已经在跑就直接复用,不会把手机踢下线。
    (void)app_portal_start();
    app_portal_snapshot_copy(&s_portal);
    s_portal_revision = app_portal_revision();
    build_body();
}

static void leave_portal(void) {
    app_portal_stop();
    app_nav_goto(&s_nav, APP_PAGE_SETTINGS);
    s_nav.sel = APP_SET_ROW_PORTAL;
    build_body();
}

// ---------------------------------------------------------------- 重画

static int visible_rows(void) {
    switch (s_nav.page) {
        case APP_PAGE_HOME: return APP_MENU_ITEM_COUNT;
        case APP_PAGE_LEAGUES: return APP_LEAGUES_COUNT;
        case APP_PAGE_STANDINGS: return STANDINGS_VISIBLE_ROWS;
        case APP_PAGE_FAVORITE: return 1;
        case APP_PAGE_SETTINGS: return APP_SET_ROW_COUNT;
        case APP_PAGE_STATUS: return 1;
        case APP_PAGE_WIFI_LIST: return AP_ROWS;
        case APP_PAGE_WIFI_PASS: return APP_KB_KEYS;
        case APP_PAGE_DETAIL: return 1;   // 上下键是翻子页,不移动选中项
        case APP_PAGE_PORTAL: return 1;   // 没有列表,上下键不做事
        case APP_PAGE_SCORES:
        default: return list_rows();
    }
}

static int current_item_count(void) {
    switch (s_nav.page) {
        case APP_PAGE_HOME: return APP_MENU_ITEM_COUNT;
        case APP_PAGE_LEAGUES: return APP_LEAGUES_COUNT;
        case APP_PAGE_STANDINGS: return s_standings_snap.team_count;
        case APP_PAGE_FAVORITE: return 1;
        case APP_PAGE_SETTINGS: return APP_SET_ROW_COUNT;
        case APP_PAGE_STATUS: return 1;
        case APP_PAGE_WIFI_LIST: return (int)s_ap_count;
        case APP_PAGE_WIFI_PASS: return APP_KB_KEYS;
        case APP_PAGE_DETAIL: return 1;
        case APP_PAGE_PORTAL: return 0;
        case APP_PAGE_SCORES:
        default: return (int)s_snap.matches.count;
    }
}

// 页头标题跟着页面走:比分页是产品名,详情页是联赛名(用户最需要的那条
// 上下文),配网页是"Wi-Fi"。标题栏只有 110 px,长名字靠省略号收尾。
static void update_title(void) {
    if (s_title == NULL) return;

    const char *value = "比赛看板";
    char league[APP_NAME_LEN];
    char day_label[40];

    switch (s_nav.page) {
        case APP_PAGE_HOME:
            value = "首页";
            break;
        case APP_PAGE_LEAGUES:
            value = "选择联赛";
            break;
        case APP_PAGE_STANDINGS:
            value = s_curr_league_name;
            break;
        case APP_PAGE_FAVORITE:
            value = "关注球队";
            break;
        case APP_PAGE_DETAIL:
            app_fonts_sanitize(s_detail_match.league, league, sizeof(league));
            value = league[0] != '\0' ? league : "比赛详情";
            break;
        case APP_PAGE_SETTINGS:
            value = "设置";
            break;
        case APP_PAGE_STATUS:
            value = "系统状态";
            break;
        case APP_PAGE_WIFI_LIST:
            value = "选择网络";
            break;
        case APP_PAGE_WIFI_PASS:
            value = "输入密码";
            break;
        case APP_PAGE_PORTAL:
            value = "手机配网";
            break;
        case APP_PAGE_SCORES:
        default:
            // 翻到别的日子时标题直接换成日期。只在页头挂个小角标不够:看板是
            // "扫一眼就走"的设备,用户很容易把昨天的比分当成今天的。
            if (s_snap.view_day != 0) {
                char date[16] = "--";
                app_datetime_t dt;
                if (app_datetime_from_utc(
                        app_cst_day_start(s_snap.updated_at, s_snap.view_day), &dt)) {
                    snprintf(date, sizeof(date), "%d-%02d", dt.month, dt.day);
                }
                if (s_snap.view_day < 0) {
                    snprintf(day_label, sizeof(day_label), "回看 %s", date);
                } else if (s_snap.view_day == 1) {
                    snprintf(day_label, sizeof(day_label), "明天 %s", date);
                } else if (s_snap.view_day == 2) {
                    snprintf(day_label, sizeof(day_label), "后天 %s", date);
                } else {
                    snprintf(day_label, sizeof(day_label), "%d 天后 %s",
                             (int)s_snap.view_day, date);
                }
                value = day_label;
            }
            break;
    }
    lv_label_set_text(s_title, value);
}

static void build_body(void) {
    if (s_body == NULL) return;

    // 键盘对象就挂在 s_body 下,清空之后句柄必须作废,否则下一次按键会写到
    // 已经释放的对象上。
    memset(s_kb_keys, 0, sizeof(s_kb_keys));
    s_kb_field = NULL;
    // 设置页的行句柄也挂在 s_body 下,clean 之后一并作废。
    memset(s_set_rows, 0, sizeof(s_set_rows));
    memset(s_set_bars, 0, sizeof(s_set_bars));
    memset(s_set_labels, 0, sizeof(s_set_labels));
    memset(s_set_values, 0, sizeof(s_set_values));
    lv_obj_clean(s_body);

    switch (s_nav.page) {
        case APP_PAGE_HOME:
            if (s_nav.sel > APP_MENU_ITEM_COUNT - 1) s_nav.sel = APP_MENU_ITEM_COUNT - 1;
            build_home();
            break;
        case APP_PAGE_LEAGUES:
            if (s_nav.sel > APP_LEAGUES_COUNT - 1) s_nav.sel = APP_LEAGUES_COUNT - 1;
            build_leagues();
            break;
        case APP_PAGE_STANDINGS:
            build_standings();
            break;
        case APP_PAGE_FAVORITE:
            build_favorite();
            break;
        case APP_PAGE_SETTINGS:
            if (s_nav.sel > APP_SET_ROW_COUNT - 1) s_nav.sel = APP_SET_ROW_COUNT - 1;
            build_settings();
            break;
        case APP_PAGE_STATUS:
            build_status();
            break;
        case APP_PAGE_DETAIL:
            build_detail();
            break;
        case APP_PAGE_WIFI_LIST:
            build_wifi_list();
            break;
        case APP_PAGE_WIFI_PASS:
            build_wifi_pass();
            break;
        case APP_PAGE_PORTAL:
            build_portal();
            break;
        case APP_PAGE_SCORES:
        default:
            build_scores();
            break;
    }

    update_title();
    update_footer();

    // 每建完一页就把 LVGL 池的占用打到日志里。这块板没有 PSRAM,池子配小了
    // 会在某个页面分配失败,而 LVGL 的失败是断言不是返回 NULL——屏上只表现为
    // 卡死。有这个数字,下次谁把某个页面写重了能第一时间看出来。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    // 系统堆也一起打:LVGL 池是静态数组,池子每多占 1 KB,堆就少 1 KB,而堆是
    // TLS 握手要用的那点余量。两个数并排看,才知道加界面的代价花在哪边。
    ESP_LOGI(TAG, "页面 %d:LVGL 池 %u/%u 已用(%u%%),峰值 %u,碎片 %u%%;系统堆 %u,最大块 %u",
             (int)s_nav.page,
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.frag_pct,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------- 设置项动作

static void handle_setting(int row) {
    app_config_t cfg;
    app_settings_get(&cfg);

    switch ((app_set_row_t)row) {
        case APP_SET_ROW_WIFI:
            enter_wifi_list();
            return;
        case APP_SET_ROW_PORTAL:
            enter_portal();
            return;
        case APP_SET_ROW_STATUS:
            app_nav_goto(&s_nav, APP_PAGE_STATUS);
            build_body();
            return;
        case APP_SET_ROW_INTERVAL:
            app_config_cycle_refresh(&cfg);
            app_settings_apply(&cfg, false);
            s_cfg = cfg;
            break;
        case APP_SET_ROW_SCREEN_OFF:
            app_config_cycle_screen_off(&cfg);
            app_settings_apply(&cfg, false);
            s_cfg = cfg;
            break;
        case APP_SET_ROW_BRIGHTNESS:
            app_config_cycle_brightness(&cfg);
            app_settings_apply(&cfg, false);
            s_cfg = cfg;
            break;
        case APP_SET_ROW_CLEAR:
            memset(cfg.ssid, 0, sizeof(cfg.ssid));
            memset(cfg.pass, 0, sizeof(cfg.pass));
            app_settings_apply(&cfg, true);
            s_cfg = cfg;
            break;
        default:
            break;
    }

    // 设置项动作可能立刻改变数据源,重新拉一次快照再画。
    app_net_snapshot_copy(&s_snap);
    build_body();
}

// ---------------------------------------------------------------- 定时刷新

static void on_tick(lv_timer_t *timer) {
    (void)timer;

    const uint32_t revision = app_net_revision();
    bool dirty = s_config_dirty;
    if (revision != s_seen_revision) {
        app_net_snapshot_copy(&s_snap);
        s_seen_revision = revision;
        s_toast_ticks = 0;
        dirty = true;
    }
    if (s_config_dirty) {
        app_settings_get(&s_cfg);
        s_config_dirty = false;
        dirty = true;
    }

    // 配网页:扫描是异步的,靠 revision 变化把结果贴上来。
    if (s_nav.page == APP_PAGE_WIFI_LIST) {
        const uint32_t scan_rev = app_net_scan_revision();
        if (scan_rev != s_scan_revision) {
            app_net_scan_copy(&s_scan);
            s_scan_revision = scan_rev;
            rebuild_ap_rows();
            dirty = true;
        }
    }

    // 详情页:拉取同样是异步的,result 到了就重画。
    if (s_nav.page == APP_PAGE_DETAIL) {
        const uint32_t detail_rev = app_net_detail_revision();
        if (detail_rev != s_detail_revision) {
            app_net_detail_copy(&s_detail);
            s_detail_revision = detail_rev;
            dirty = true;
        }

        // 队徽的排队在 app_net 里(详情拉成功就地排),这边只跟着它的版本号
        // 重画:图下完、下失败、被作废都会 +1。
        const uint32_t crest_rev = app_crest_revision();
        if (crest_rev != s_crest_revision) {
            s_crest_revision = crest_rev;
            dirty = true;
        }
    }

    // 手机配网页:连接结果由门户的守护任务推进,这里只跟着版本号重画。
    if (s_nav.page == APP_PAGE_STANDINGS) {
        const uint32_t rev = app_standings_revision();
        if (rev != s_standings_rev) {
            build_body();
        }
    }
    if (s_nav.page == APP_PAGE_FAVORITE) {
        const uint32_t fav_rev = app_favorite_revision();
        if (fav_rev != s_fav_rev) {
            build_body();
        }
    }
    if (s_nav.page == APP_PAGE_PORTAL) {
        const uint32_t portal_rev = app_portal_revision();
        if (portal_rev != s_portal_revision) {
            app_portal_snapshot_copy(&s_portal);
            s_portal_revision = portal_rev;
            dirty = true;
        }
    }

    if (s_toast_ticks > 0) {
        s_toast_ticks--;
        dirty = true;
    }
    if (dirty) build_body();

    update_header();
}

// ---------------------------------------------------------------- 对外接口

static void handle_keyboard_input(app_input_t input) {
    switch (input) {
        case APP_INPUT_UP:
            app_keyboard_move(&s_kb, -1);
            break;
        case APP_INPUT_DOWN:
            app_keyboard_move(&s_kb, 1);
            break;
        case APP_INPUT_OK:
            (void)app_keyboard_press(&s_kb);
            break;
        default:
            return;
    }
    refresh_keyboard();
}

// 切换日期视图(0 = 今天,-1 = 回看前一天,1..7 = 未来第 N 天)。
//
// 联网层会立刻把快照的 view_day 改掉、清空旧列表并压成"正在拉取",所以这里
// 同步一份快照再重画,屏幕上是"回看 MM-DD / 明天 MM-DD + 正在拉取",而不是
// 旧日期那批比赛。拉到之后由定时器按 revision 变化重画。
static void switch_day(int8_t day) {
    app_net_set_view_day(day);
    app_net_snapshot_copy(&s_snap);
    s_seen_revision = s_snap.revision;
    s_nav.sel = 0;
    s_nav.scroll = 0;
    update_title();
    build_body();
}

void app_ui_handle_input(app_input_t input) {
    // 重建密码页要一口气建 84 个对象,LVGL 任务会短暂占着锁。300 ms 偏紧:
    // 拿不到锁就等于把这次按键丢掉,用户看到的就是"按键没反应"。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGW(TAG, "取不到 LVGL 锁,丢弃一次按键");
        return;
    }

    // 有按键就先把背光提回来,再处理动作。
    // 息屏/点亮由 main.c 的按键回调统一处理,这里不再碰背光。

    const app_act_t action = app_nav_handle(&s_nav, input, current_item_count(),
                                           visible_rows());

    // 密码页的方向键/确定键归键盘模型,导航在那里一律返回 NONE。
    if (s_nav.page == APP_PAGE_WIFI_PASS && action == APP_ACT_NONE) {
        handle_keyboard_input(input);
        bsp_lvgl_unlock();
        return;
    }

    switch (action) {
        case APP_ACT_PAGE_CHANGED:
            build_body();
            break;
        case APP_ACT_REDRAW:
            // 设置页里上下翻行只是换了选中项:直接改颜色,不重建。
            // 其它页面的 REDRAW 一律走整页重建。
            if (s_nav.page == APP_PAGE_HOME && s_home_cards[0] != NULL) {
                home_select((int)s_nav.sel);
                break;
            }
            if (s_nav.page == APP_PAGE_LEAGUES && s_league_rows[0] != NULL) {
                leagues_select((int)s_nav.sel);
                break;
            }
            if (s_nav.page == APP_PAGE_SETTINGS && s_set_rows[0] != NULL) {
                settings_select((int)s_nav.sel);
                break;
            }
            build_body();
            break;
        case APP_ACT_SETTING:
            handle_setting(s_nav.sel);
            break;
        case APP_ACT_LEAGUE_PICK:
            if (s_nav.sel >= 0 && s_nav.sel < APP_LEAGUES_COUNT) {
                s_curr_season_id = APP_LEAGUES[s_nav.sel].season_id;
                snprintf(s_curr_league_name, sizeof(s_curr_league_name), "%s", APP_LEAGUES[s_nav.sel].name);
                app_standings_fetch(s_curr_season_id, s_curr_league_name);
                app_nav_goto(&s_nav, APP_PAGE_STANDINGS);
                build_body();
            }
            break;
        case APP_ACT_STANDINGS_BACK:
            build_body();
            break;
        case APP_ACT_FAVORITE_PICK: {
            if (s_nav.page == APP_PAGE_STANDINGS &&
                s_nav.sel >= 0 && s_nav.sel < s_standings_snap.team_count) {
                const app_standing_team_t *tm = &s_standings_snap.teams[s_nav.sel];
                s_fav_team.team_id = tm->team_id;
                snprintf(s_fav_team.team_name, sizeof(s_fav_team.team_name), "%s", tm->team_name);
                snprintf(s_fav_team.league_name, sizeof(s_fav_team.league_name), "%s", s_curr_league_name);
                app_store_save_fav(&s_fav_team);
                app_favorite_fetch(s_fav_team.team_id, s_fav_team.team_name, s_fav_team.league_name);

                char msg[48];
                snprintf(msg, sizeof(msg), "已设主队: %s", tm->team_name);
                toast(msg);
                build_body();
            }
            break;
        }
        case APP_ACT_FAVORITE_OPEN:
            if (s_fav_team.team_id > 0) {
                app_favorite_fetch(s_fav_team.team_id, s_fav_team.team_name, s_fav_team.league_name);
                app_nav_goto(&s_nav, APP_PAGE_FAVORITE);
                build_body();
            } else {
                toast("请在积分榜选定主队");
                app_nav_goto(&s_nav, APP_PAGE_LEAGUES);
                build_body();
            }
            break;
        case APP_ACT_REFRESH:
            if (s_nav.page == APP_PAGE_STANDINGS) {
                app_standings_fetch(s_curr_season_id, s_curr_league_name);
                toast("正在刷新积分榜…");
                update_footer();
                break;
            }
            if (s_nav.page == APP_PAGE_FAVORITE) {
                if (s_fav_team.team_id > 0) {
                    app_favorite_fetch(s_fav_team.team_id, s_fav_team.team_name, s_fav_team.league_name);
                    toast("正在刷新主队战报…");
                }
                update_footer();
                break;
            }
            app_net_request_refresh();
            toast("正在刷新比分…");
            update_footer();
            break;
        case APP_ACT_WIFI_PICK:
            pick_ap();
            break;
        case APP_ACT_WIFI_BACK:
            app_nav_goto(&s_nav, APP_PAGE_SETTINGS);
            s_nav.sel = APP_SET_ROW_WIFI;
            build_body();
            break;
        case APP_ACT_WIFI_CONNECT:
            submit_password();
            break;
        case APP_ACT_DETAIL_OPEN:
            open_detail();
            break;
        case APP_ACT_DETAIL_REFRESH:
            reload_detail();
            break;
        case APP_ACT_DETAIL_BACK:
            // 返回列表时回到原来那一场,别把用户丢回第一条。
            s_nav.sel = s_detail_index;
            build_body();
            break;
        case APP_ACT_PORTAL_RETRY:
            // 重新开一次热点。失败多半是上一步没起来,再试一次不花钱。
            (void)app_portal_start();
            app_portal_snapshot_copy(&s_portal);
            s_portal_revision = app_portal_revision();
            build_body();
            break;
        case APP_ACT_PORTAL_BACK:
            leave_portal();
            break;
        case APP_ACT_DAY_PREV:
            // 翻到头就不再动:最早只回看到前一天。
            if (s_snap.view_day > APP_VIEW_DAY_MIN) {
                switch_day((int8_t)(s_snap.view_day - 1));
            }
            break;
        case APP_ACT_DAY_NEXT:
            // 往后可以一直翻到第 7 天,和"下一个比赛日"探测的上限一致。
            if (s_snap.view_day < APP_VIEW_DAY_MAX) {
                switch_day((int8_t)(s_snap.view_day + 1));
            }
            break;
        case APP_ACT_NONE:
        default:
            // 比分页列表为空时按 OK:导航层不会给动作,这里补一句提示,
            // 否则按下去屏幕毫无变化,像是按键坏了。
            if (input == APP_INPUT_OK && s_nav.page == APP_PAGE_SCORES) {
                toast(s_snap.view_day != 0 ? "这天没有比赛" : "今天暂无比赛");
                update_footer();
            }
            break;
    }

    bsp_lvgl_unlock();
}

void app_ui_notify_config_changed(void) {
    s_config_dirty = true;
}

void app_ui_set_battery(int soc, int mv) {
    if (!bsp_lvgl_lock(200)) return;
    s_battery_soc = soc;
    s_battery_mv = mv;
    update_header();
    bsp_lvgl_unlock();
}

void app_ui_init(void) {
    app_standings_init();
    app_favorite_init();
    if (app_store_load_fav(&s_fav_team) == ESP_OK && s_fav_team.team_id > 0) {
        app_favorite_fetch(s_fav_team.team_id, s_fav_team.team_name, s_fav_team.league_name);
    }
    app_nav_init(&s_nav);
    app_settings_get(&s_cfg);
    app_net_snapshot_copy(&s_snap);
    app_net_scan_copy(&s_scan);
    s_seen_revision = s_snap.revision;
    s_scan_revision = s_scan.revision;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // 页头:标题 + 阶梯 Wi-Fi 信号 + 电量
    lv_obj_t *header = panel(s_scr, 0, 0, SCR_W, HEADER_H, C_HEADER, 0);
    s_title = clipped_text(header, APP_FONT_BODY, C_TEXT, "比赛看板", 14, 10, 140, 22,
                           LV_TEXT_ALIGN_LEFT);

    // 阶梯式 Wi-Fi 图标: 3 根竖柱, 从左到右高度 5, 9, 13, 底部在 y=25 对齐
    s_wifi_bars[0] = panel(header, 166, 20, 3, 5, C_MUTED, 1);
    s_wifi_bars[1] = panel(header, 171, 16, 3, 9, C_MUTED, 1);
    s_wifi_bars[2] = panel(header, 176, 12, 3, 13, C_MUTED, 1);

    s_battery = clipped_text(header, APP_FONT_BODY, C_MUTED, "--%", 184, 10, 44, 22,
                             LV_TEXT_ALIGN_RIGHT);

    s_body = panel(s_scr, 0, BODY_Y, SCR_W, BODY_H, C_BG, 0);

    s_footer = clipped_text(s_scr, APP_FONT_SMALL, C_MUTED, "", 8, SCR_H - FOOTER_H + 4,
                            SCR_W - 16, 16, LV_TEXT_ALIGN_CENTER);

    lv_screen_load(s_scr);
    build_body();
    update_header();

    s_timer = lv_timer_create(on_tick, 1000, NULL);
    ESP_LOGI(TAG, "看板界面就绪");
}
