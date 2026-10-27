// main/app_nav.c
#include "app_nav.h"

#include <stddef.h>

void app_nav_init(app_nav_t *nav) {
    if (nav == NULL) return;
    nav->page = APP_PAGE_HOME;
    nav->sel = 0;
    nav->scroll = 0;
    nav->sub = 0;
}

void app_nav_reset_selection(app_nav_t *nav) {
    if (nav == NULL) return;
    nav->sel = 0;
    nav->scroll = 0;
    nav->sub = 0;
}

void app_nav_goto(app_nav_t *nav, app_page_t page) {
    if (nav == NULL) return;
    nav->page = page;
    app_nav_reset_selection(nav);
}

// 让选中项始终落在可见窗口里,并夹住窗口边界。
static void follow_selection(app_nav_t *nav, int count, int rows) {
    if (rows < 1) rows = 1;
    if (nav->sel < 0) nav->sel = 0;
    if (nav->sel > count - 1) nav->sel = count > 0 ? count - 1 : 0;

    if (nav->sel < nav->scroll) nav->scroll = nav->sel;
    if (nav->sel >= nav->scroll + rows) nav->scroll = nav->sel - rows + 1;

    const int max_scroll = count > rows ? count - rows : 0;
    if (nav->scroll > max_scroll) nav->scroll = max_scroll;
    if (nav->scroll < 0) nav->scroll = 0;
}

// 详情页只有两个子页,上下键都当作"翻一页"——两页的时候往上翻和往下翻
// 效果一样,但代码保持可扩展(以后加第三页也不用改按键语义)。
static app_act_t detail_flip(app_nav_t *nav, app_input_t input) {
    const int before = nav->sub;
    if (input == APP_INPUT_DOWN) {
        nav->sub = (nav->sub + 1) % APP_DETAIL_PAGES;
    } else {
        nav->sub = (nav->sub + APP_DETAIL_PAGES - 1) % APP_DETAIL_PAGES;
    }
    return nav->sub == before ? APP_ACT_NONE : APP_ACT_REDRAW;
}

// 长按确定:比分/设置互切,详情退回比分,热点页退回设置,密码页提交。
static app_act_t long_press(app_nav_t *nav) {
    switch (nav->page) {
        case APP_PAGE_HOME:
            app_nav_goto(nav, APP_PAGE_SETTINGS);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_SCORES:
            app_nav_goto(nav, APP_PAGE_HOME);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_DETAIL:
            app_nav_goto(nav, APP_PAGE_SCORES);
            return APP_ACT_DETAIL_BACK;
        case APP_PAGE_LEAGUES:
            app_nav_goto(nav, APP_PAGE_HOME);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_STANDINGS:
            app_nav_goto(nav, APP_PAGE_LEAGUES);
            return APP_ACT_STANDINGS_BACK;
        case APP_PAGE_FAVORITE:
            app_nav_goto(nav, APP_PAGE_HOME);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_SETTINGS:
            app_nav_goto(nav, APP_PAGE_HOME);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_STATUS:
            app_nav_goto(nav, APP_PAGE_SETTINGS);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_WIFI_LIST:
            app_nav_goto(nav, APP_PAGE_SETTINGS);
            return APP_ACT_WIFI_BACK;
        case APP_PAGE_WIFI_PASS:
            // 页面留在原地:连上之后再切页,用户才能看到"连接中"的状态。
            return APP_ACT_WIFI_CONNECT;
        case APP_PAGE_PORTAL:
            app_nav_goto(nav, APP_PAGE_SETTINGS);
            return APP_ACT_PORTAL_BACK;
        default:
            return APP_ACT_NONE;
    }
}

app_act_t app_nav_handle(app_nav_t *nav, app_input_t input, int item_count, int visible_rows) {
    if (nav == NULL) return APP_ACT_NONE;
    if (item_count < 0) item_count = 0;
    if (visible_rows < 1) visible_rows = 1;

    if (input == APP_INPUT_OK_DOUBLE) {
        if (nav->page == APP_PAGE_HOME || nav->page == APP_PAGE_SCORES) {
            return APP_ACT_REFRESH;
        }
        return APP_ACT_NONE;
    }

    if (input == APP_INPUT_OK_LONG) return long_press(nav);

    // 密码页的方向键与确定键由界面交给键盘模型,这里只挡掉它们。
    if (nav->page == APP_PAGE_WIFI_PASS) return APP_ACT_NONE;

    // 系统状态页单屏展示: 按任意方向键退出回设置页
    if (nav->page == APP_PAGE_STATUS && (input == APP_INPUT_UP || input == APP_INPUT_DOWN)) {
        app_nav_goto(nav, APP_PAGE_SETTINGS);
        return APP_ACT_PAGE_CHANGED;
    }

    // 详情页的上下键是翻子页,不是移动列表选中项。
    if (nav->page == APP_PAGE_DETAIL && (input == APP_INPUT_UP || input == APP_INPUT_DOWN)) {
        return detail_flip(nav, input);
    }

    if (input == APP_INPUT_UP || input == APP_INPUT_DOWN) {
        // 比分页翻到头再按一次 = 翻日期:首项按上往前一天,末项按下往后一天。
        // 列表为空时两个方向都放行——否则"今天一场都没有"的日子根本进不去
        // 回看,也翻不到后面几天。
        //
        // 这里只负责发出"想翻日期"的意图,到底能翻到哪天(最早前一天、最晚
        // 第七天)由界面根据当前视图夹取,导航层不持有日期状态。
        if (nav->page == APP_PAGE_SCORES) {
            const bool at_top = (input == APP_INPUT_UP)
                                && (item_count == 0 || nav->sel == 0);
            const bool at_bottom = (input == APP_INPUT_DOWN)
                                   && (item_count == 0 || nav->sel >= item_count - 1);
            if (at_top || at_bottom) {
                // 换了日期就是换了一份列表,选中项和滚动窗口都要归零。
                app_nav_reset_selection(nav);
                return at_top ? APP_ACT_DAY_PREV : APP_ACT_DAY_NEXT;
            }
        }

        if (item_count == 0) {
            if (nav->page == APP_PAGE_STANDINGS) {
                app_nav_goto(nav, APP_PAGE_LEAGUES);
                return APP_ACT_STANDINGS_BACK;
            }
            if (nav->page == APP_PAGE_FAVORITE) {
                app_nav_goto(nav, APP_PAGE_HOME);
                return APP_ACT_PAGE_CHANGED;
            }
            return APP_ACT_NONE;
        }
        const int before = nav->sel;
        nav->sel += (input == APP_INPUT_UP) ? -1 : 1;
        if (nav->sel < 0) nav->sel = 0;
        if (nav->sel > item_count - 1) nav->sel = item_count - 1;
        follow_selection(nav, item_count, visible_rows);
        return nav->sel == before ? APP_ACT_NONE : APP_ACT_REDRAW;
    }

    // 单击确定。每页的语义是固定的,所以这里不需要 item_count 参与判断。
    switch (nav->page) {
        case APP_PAGE_HOME:
            if (nav->sel == APP_MENU_SCORES) {
                app_nav_goto(nav, APP_PAGE_SCORES);
                return APP_ACT_PAGE_CHANGED;
            } else if (nav->sel == APP_MENU_STANDINGS) {
                app_nav_goto(nav, APP_PAGE_LEAGUES);
                return APP_ACT_PAGE_CHANGED;
            }
            return APP_ACT_FAVORITE_OPEN; // APP_MENU_FAVORITE
        case APP_PAGE_FAVORITE:
            return APP_ACT_REFRESH; // 主队看板单击 OK 刷新
        case APP_PAGE_LEAGUES:
            return APP_ACT_LEAGUE_PICK;
        case APP_PAGE_STANDINGS:
            return APP_ACT_FAVORITE_PICK; // 积分榜里单击 OK 设为主队
        case APP_PAGE_SETTINGS:
            return APP_ACT_SETTING;
        case APP_PAGE_STATUS:
            app_nav_goto(nav, APP_PAGE_SETTINGS);
            return APP_ACT_PAGE_CHANGED;
        case APP_PAGE_WIFI_LIST:
            return APP_ACT_WIFI_PICK;
        case APP_PAGE_DETAIL:
            return APP_ACT_DETAIL_REFRESH;
        case APP_PAGE_PORTAL:
            return APP_ACT_PORTAL_RETRY;
        case APP_PAGE_SCORES:
        default:
            // 列表为空时没有"选中场次"可开,交给界面去提示。
            return item_count > 0 ? APP_ACT_DETAIL_OPEN : APP_ACT_NONE;
    }
}
