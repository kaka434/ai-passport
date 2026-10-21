// tests/test_app_nav.c —— 两页导航状态机:选中项、滚动窗口、按键动作。
#include "app_nav.h"

#include <assert.h>
#include <stdio.h>

#define ROWS 5

static void test_init(void) {
    app_nav_t nav;
    app_nav_init(&nav);
    assert(nav.page == APP_PAGE_SCORES);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);

    app_nav_init(NULL);   // 不炸
}

static void test_selection_moves_and_clamps(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 3, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 1);
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 3, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 2);

    // 到底了:状态不变,界面不需要重画。
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 3, ROWS) == APP_ACT_NONE);
    assert(nav.sel == 2);

    assert(app_nav_handle(&nav, APP_INPUT_UP, 3, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 1);
    assert(app_nav_handle(&nav, APP_INPUT_UP, 3, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 0);
    assert(app_nav_handle(&nav, APP_INPUT_UP, 3, ROWS) == APP_ACT_NONE);
    assert(nav.sel == 0);
}

static void test_empty_list_ignores_updown(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 0, ROWS) == APP_ACT_NONE);
    assert(app_nav_handle(&nav, APP_INPUT_UP, 0, ROWS) == APP_ACT_NONE);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);

    // 空列表里按确定不给动作 —— 没有场次可开,界面会补一句提示。
    assert(app_nav_handle(&nav, APP_INPUT_OK, 0, ROWS) == APP_ACT_NONE);
}

static void test_scroll_window_follows_selection(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    // 12 条数据、一次看 5 行:选中项往下走到第 5 条时窗口才开始动。
    for (int i = 1; i < 5; i++) {
        assert(app_nav_handle(&nav, APP_INPUT_DOWN, 12, ROWS) == APP_ACT_REDRAW);
    }
    assert(nav.sel == 4);
    assert(nav.scroll == 0);

    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 12, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 5);
    assert(nav.scroll == 1);

    // 一路走到底:窗口停在 12 - 5 = 7。
    while (nav.sel < 11) {
        assert(app_nav_handle(&nav, APP_INPUT_DOWN, 12, ROWS) == APP_ACT_REDRAW);
    }
    assert(nav.sel == 11);
    assert(nav.scroll == 7);

    // 往回走,选中项一旦进入窗口上沿,窗口就跟着回来。
    while (nav.sel > 0) {
        (void)app_nav_handle(&nav, APP_INPUT_UP, 12, ROWS);
    }
    assert(nav.sel == 0);
    assert(nav.scroll == 0);
}

static void test_scroll_clamped_when_list_shrinks(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    // 先滚到窗口末尾(12 条 → scroll=7),数据刷新后只剩 6 条。
    for (int i = 0; i < 12; i++) (void)app_nav_handle(&nav, APP_INPUT_DOWN, 12, ROWS);
    assert(nav.sel == 11);
    assert(nav.scroll == 7);

    // 再按一下上:count 变成 6,选中项和窗口都必须夹回合法范围。
    (void)app_nav_handle(&nav, APP_INPUT_UP, 6, ROWS);
    assert(nav.sel == 5);
    assert(nav.scroll == 1);
    assert(nav.scroll + ROWS <= 6);
}

static void test_ok_actions_per_page(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    // 比分页:确定 = 打开选中场次的详情(手动刷新让位给了设置页那一行)。
    assert(app_nav_handle(&nav, APP_INPUT_OK, 8, ROWS) == APP_ACT_DETAIL_OPEN);
    assert(nav.page == APP_PAGE_SCORES);

    // 设置页:确定 = 执行当前行,行号就是 sel。
    nav.page = APP_PAGE_SETTINGS;
    nav.sel = 3;
    assert(app_nav_handle(&nav, APP_INPUT_OK, 5, ROWS) == APP_ACT_SETTING);
    assert(nav.sel == 3);
}

// 详情页:上下翻子页、确定重拉、长按回比分。
static void test_detail_page(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    app_nav_goto(&nav, APP_PAGE_DETAIL);
    assert(nav.sub == 0);

    // 两页时往下翻到 1、再翻回 0;往上翻则相反。
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 1, 1) == APP_ACT_REDRAW);
    assert(nav.sub == 1);
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 1, 1) == APP_ACT_REDRAW);
    assert(nav.sub == 0);
    assert(app_nav_handle(&nav, APP_INPUT_UP, 1, 1) == APP_ACT_REDRAW);
    assert(nav.sub == APP_DETAIL_PAGES - 1);

    // 详情页的上下键不该动 sel —— 它没有列表。
    assert(nav.sel == 0);

    assert(app_nav_handle(&nav, APP_INPUT_OK, 1, 1) == APP_ACT_DETAIL_REFRESH);
    assert(nav.page == APP_PAGE_DETAIL);

    assert(app_nav_handle(&nav, APP_INPUT_OK_LONG, 1, 1) == APP_ACT_DETAIL_BACK);
    assert(nav.page == APP_PAGE_SCORES);
}

static void test_long_press_toggles_pages(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    nav.sel = 4;
    nav.scroll = 2;
    assert(app_nav_handle(&nav, APP_INPUT_OK_LONG, 0, ROWS) == APP_ACT_PAGE_CHANGED);
    assert(nav.page == APP_PAGE_SETTINGS);
    // 切页必须复位选中项,否则设置页会从"第 5 行"开始,看起来像随机跳。
    assert(nav.sel == 0);
    assert(nav.scroll == 0);

    nav.sel = 3;
    assert(app_nav_handle(&nav, APP_INPUT_OK_LONG, 0, ROWS) == APP_ACT_PAGE_CHANGED);
    assert(nav.page == APP_PAGE_SCORES);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);
}

static void test_reset_selection(void) {
    app_nav_t nav;
    app_nav_init(&nav);
    nav.sel = 6;
    nav.scroll = 3;
    app_nav_reset_selection(&nav);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);
    app_nav_reset_selection(NULL);   // 不炸
}

// 配网流程:设置页 → 热点列表 → 密码页 → 长按提交。
static void test_wifi_pages(void) {
    app_nav_t nav;
    app_nav_init(&nav);

    app_nav_goto(&nav, APP_PAGE_WIFI_LIST);
    assert(nav.page == APP_PAGE_WIFI_LIST);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);

    // 列表里上下移动、确定选中。
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 4, ROWS) == APP_ACT_REDRAW);
    assert(nav.sel == 1);
    assert(app_nav_handle(&nav, APP_INPUT_OK, 4, ROWS) == APP_ACT_WIFI_PICK);
    assert(nav.page == APP_PAGE_WIFI_LIST);

    // 长按 = 返回设置页,并且复位选中项(设置页第 0 行就是 Wi-Fi 配网)。
    assert(app_nav_handle(&nav, APP_INPUT_OK_LONG, 4, ROWS) == APP_ACT_WIFI_BACK);
    assert(nav.page == APP_PAGE_SETTINGS);
    assert(nav.sel == 0);

    // 密码页:方向键与确定键交给键盘模型,导航一律不响应;
    // 长按 = 提交并连接,页面停在原地。
    app_nav_goto(&nav, APP_PAGE_WIFI_PASS);
    assert(app_nav_handle(&nav, APP_INPUT_UP, 42, 42) == APP_ACT_NONE);
    assert(app_nav_handle(&nav, APP_INPUT_DOWN, 42, 42) == APP_ACT_NONE);
    assert(app_nav_handle(&nav, APP_INPUT_OK, 42, 42) == APP_ACT_NONE);
    assert(app_nav_handle(&nav, APP_INPUT_OK_LONG, 42, 42) == APP_ACT_WIFI_CONNECT);
    assert(nav.page == APP_PAGE_WIFI_PASS);

    app_nav_goto(NULL, APP_PAGE_SCORES);   // 不炸
}

static void test_goto_resets_selection(void) {
    app_nav_t nav;
    app_nav_init(&nav);
    nav.page = APP_PAGE_SETTINGS;
    nav.sel = 4;
    nav.scroll = 2;

    app_nav_goto(&nav, APP_PAGE_SCORES);
    assert(nav.page == APP_PAGE_SCORES);
    assert(nav.sel == 0);
    assert(nav.scroll == 0);
}

static void test_null_nav(void) {
    assert(app_nav_handle(NULL, APP_INPUT_DOWN, 5, ROWS) == APP_ACT_NONE);
}

int main(void) {
    test_init();
    test_selection_moves_and_clamps();
    test_empty_list_ignores_updown();
    test_scroll_window_follows_selection();
    test_scroll_clamped_when_list_shrinks();
    test_ok_actions_per_page();
    test_detail_page();
    test_long_press_toggles_pages();
    test_reset_selection();
    test_wifi_pages();
    test_goto_resets_selection();
    test_null_nav();
    puts("test_app_nav: OK");
    return 0;
}
