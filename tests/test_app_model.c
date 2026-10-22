// tests/test_app_model.c —— 比赛模型与文案的纯逻辑测试(不需要硬件/ESP-IDF)。
#include "app_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 2026-10-18T04:00:00Z = 北京时间 10-18 12:00,作为"现在"。
#define NOW_UTC 1792296000LL

static app_match_t make_match(const char *id, uint8_t state, int64_t kickoff) {
    app_match_t match;
    memset(&match, 0, sizeof(match));
    snprintf(match.id, sizeof(match.id), "%s", id);
    snprintf(match.home, sizeof(match.home), "主队");
    snprintf(match.away, sizeof(match.away), "客队");
    snprintf(match.league, sizeof(match.league), "联赛");
    match.state = state;
    match.kickoff_utc = kickoff;
    match.home_score = 0;
    match.away_score = 0;
    return match;
}

static void test_upsert_dedupes_by_id(void) {
    app_match_list_t list;
    app_match_list_reset(&list);

    app_match_t first = make_match("100", APP_STATE_FIRST_HALF, 1000);
    assert(app_match_list_upsert(&list, &first) == 0);
    assert(list.count == 1);

    // 同一个 id 再来一次:整条替换,比分跟着更新,不新增。
    first.home_score = 2;
    first.state = APP_STATE_SECOND_HALF;
    assert(app_match_list_upsert(&list, &first) == 0);
    assert(list.count == 1);
    assert(list.items[0].home_score == 2);
    assert(list.items[0].state == APP_STATE_SECOND_HALF);

    // 没有 id 的记录直接拒绝,避免界面出现无法识别的行。
    app_match_t anon = make_match("", APP_STATE_FIXTURE, 1000);
    assert(app_match_list_upsert(&list, &anon) == -1);
}

static void test_upsert_capacity(void) {
    app_match_list_t list;
    app_match_list_reset(&list);
    for (int i = 0; i < APP_MAX_MATCHES; i++) {
        char id[APP_ID_LEN];
        snprintf(id, sizeof(id), "%d", i + 1);
        app_match_t match = make_match(id, APP_STATE_FIXTURE, 1000 + i);
        assert(app_match_list_upsert(&list, &match) == i);
    }
    assert(list.count == APP_MAX_MATCHES);

    app_match_t extra = make_match("999", APP_STATE_FIXTURE, 2000);
    assert(app_match_list_upsert(&list, &extra) == -1);
    assert(list.count == APP_MAX_MATCHES);
}

static void test_sort_order(void) {
    app_match_list_t list;
    app_match_list_reset(&list);

    app_match_t finished = make_match("1", APP_STATE_FULL_TIME, 3000);
    app_match_t upcoming_late = make_match("2", APP_STATE_FIXTURE, 5000);
    app_match_t live = make_match("3", APP_STATE_FIRST_HALF, 9000);
    app_match_t postponed = make_match("4", APP_STATE_POSTPONED, 1000);
    app_match_t upcoming_soon = make_match("5", APP_STATE_FIXTURE, 4000);
    (void)app_match_list_upsert(&list, &finished);
    (void)app_match_list_upsert(&list, &upcoming_late);
    (void)app_match_list_upsert(&list, &live);
    (void)app_match_list_upsert(&list, &postponed);
    (void)app_match_list_upsert(&list, &upcoming_soon);

    app_match_list_sort(&list);
    // 直播 → 未开始(按开赛时间) → 推迟/取消/待定 → 完场
    assert(strcmp(list.items[0].id, "3") == 0);
    assert(strcmp(list.items[1].id, "5") == 0);
    assert(strcmp(list.items[2].id, "2") == 0);
    assert(strcmp(list.items[3].id, "4") == 0);
    assert(strcmp(list.items[4].id, "1") == 0);
}

static void test_state_from_status(void) {
    assert(app_state_from_status("Playing", "1H") == APP_STATE_FIRST_HALF);
    assert(app_state_from_status("Playing", "2H") == APP_STATE_SECOND_HALF);
    assert(app_state_from_status("Playing", "HT") == APP_STATE_HALF_TIME);
    assert(app_state_from_status("Playing", "") == APP_STATE_LIVE_OTHER);
    assert(app_state_from_status("Playing", NULL) == APP_STATE_LIVE_OTHER);
    assert(app_state_from_status("Fixture", "1H") == APP_STATE_FIXTURE);
    assert(app_state_from_status("Played", "FT") == APP_STATE_FULL_TIME);
    assert(app_state_from_status("Postponed", "") == APP_STATE_POSTPONED);
    assert(app_state_from_status("Cancelled", "") == APP_STATE_CANCELLED);
    assert(app_state_from_status("Uncertain", "") == APP_STATE_UNKNOWN);
    assert(app_state_from_status("SomethingNew", "") == APP_STATE_UNKNOWN);
    assert(app_state_from_status(NULL, NULL) == APP_STATE_UNKNOWN);
}

static void test_is_live(void) {
    assert(app_match_is_live(APP_STATE_FIRST_HALF));
    assert(app_match_is_live(APP_STATE_HALF_TIME));
    assert(app_match_is_live(APP_STATE_SECOND_HALF));
    assert(app_match_is_live(APP_STATE_LIVE_OTHER));
    assert(!app_match_is_live(APP_STATE_FIXTURE));
    assert(!app_match_is_live(APP_STATE_FULL_TIME));
    assert(!app_match_is_live(APP_STATE_POSTPONED));
}

static void test_state_labels(void) {
    char buf[32];

    app_match_t match = make_match("1", APP_STATE_FIRST_HALF, NOW_UTC);
    match.minute = 32;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "上半场 32'") == 0);

    match.state = APP_STATE_SECOND_HALF;
    match.minute = 78;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "下半场 78'") == 0);

    // 分钟数缺失时不显示 "0'"。
    match.minute = 0;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "下半场") == 0);

    match.state = APP_STATE_HALF_TIME;
    match.minute = 45;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "中场") == 0);

    match.state = APP_STATE_LIVE_OTHER;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "直播中") == 0);

    match.state = APP_STATE_FULL_TIME;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "完场") == 0);

    match.state = APP_STATE_POSTPONED;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "推迟") == 0);

    match.state = APP_STATE_CANCELLED;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "取消") == 0);

    match.state = APP_STATE_UNKNOWN;
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "待定") == 0);

    // 未开始直接给开赛时间,比"未开始"三个字有用。
    match.state = APP_STATE_FIXTURE;
    match.kickoff_utc = 1792326600LL;   // 北京 10-18 20:30
    app_match_state_label(&match, buf, sizeof(buf));
    assert(strcmp(buf, "20:30") == 0);
}

static void test_state_labels_with_day(void) {
    char buf[32];
    app_match_t match = make_match("1", APP_STATE_FIXTURE, 1792326600LL);   // 今天 20:30

    app_match_state_label_at(&match, NOW_UTC, buf, sizeof(buf));
    assert(strcmp(buf, "今天 20:30") == 0);

    match.kickoff_utc = 1792407600LL;   // 10-19 19:00
    app_match_state_label_at(&match, NOW_UTC, buf, sizeof(buf));
    assert(strcmp(buf, "明天 19:00") == 0);

    match.kickoff_utc = 1792494000LL;   // 10-20 19:00
    app_match_state_label_at(&match, NOW_UTC, buf, sizeof(buf));
    assert(strcmp(buf, "后天 19:00") == 0);

    // 更远的日子给完整日期。
    match.kickoff_utc = 1792926000LL;   // 10-25 19:00
    app_match_state_label_at(&match, NOW_UTC, buf, sizeof(buf));
    assert(strcmp(buf, "10-25 19:00") == 0);

    // 直播中的比赛不吃日期前缀。
    match.state = APP_STATE_FIRST_HALF;
    match.minute = 12;
    app_match_state_label_at(&match, NOW_UTC, buf, sizeof(buf));
    assert(strcmp(buf, "上半场 12'") == 0);

    // now 无效时退化成不带日期的文案。
    match.state = APP_STATE_FIXTURE;
    match.kickoff_utc = 1792326600LL;
    app_match_state_label_at(&match, 0, buf, sizeof(buf));
    assert(strcmp(buf, "20:30") == 0);
}

static void test_day_offset(void) {
    assert(app_day_offset(NOW_UTC, NOW_UTC) == 0);
    assert(app_day_offset(NOW_UTC, NOW_UTC + 8 * 3600) == 0);    // 今天 20:00
    assert(app_day_offset(NOW_UTC, NOW_UTC + 24 * 3600) == 1);   // 明天 12:00
    assert(app_day_offset(NOW_UTC, NOW_UTC - 24 * 3600) == -1);  // 昨天 12:00
    assert(app_day_offset(NOW_UTC, 0) == 0);                     // 无效值不炸
    assert(app_day_offset(0, NOW_UTC) == 0);
}

static void test_league_whitelist(void) {
    // 五大联赛 + 中超 + 中甲 + 欧战两项。
    assert(app_league_allowed("英超"));
    assert(app_league_allowed("西甲"));
    assert(app_league_allowed("意甲"));
    assert(app_league_allowed("德甲"));
    assert(app_league_allowed("法甲"));
    assert(app_league_allowed("中超"));
    assert(app_league_allowed("中甲"));
    assert(app_league_allowed("欧冠"));
    assert(app_league_allowed("欧联"));

    // 接口偶尔给带后缀的写法,去掉"联赛"后应当照样命中。
    assert(app_league_allowed("英超联赛"));
    assert(app_league_allowed("中超联赛"));
    assert(app_league_allowed("欧冠联赛"));

    // 其它赛事一律不收。
    assert(!app_league_allowed("英冠"));
    assert(!app_league_allowed("中乙"));
    assert(!app_league_allowed("巴甲"));
    assert(!app_league_allowed("葡超"));
    assert(!app_league_allowed("NBA"));
    // 名字里带"欧"或"冠"的邻居赛事不能被全等匹配放过:欧协联(Conference
    // League)、女足欧冠、欧青联、亚冠精英都不是欧冠。
    assert(!app_league_allowed("欧协联"));
    assert(!app_league_allowed("女足欧冠"));
    assert(!app_league_allowed("欧青联"));
    assert(!app_league_allowed("亚冠精英"));
    // "巴西甲级联赛"里含"西甲"三个字,但不能被当成西甲(这正是用全等而不是
    // 子串匹配的原因)。
    assert(!app_league_allowed("巴西甲级联赛"));

    assert(!app_league_allowed(""));
    assert(!app_league_allowed(NULL));
}

static void test_is_today(void) {
    app_match_t match = make_match("1", APP_STATE_FIXTURE, NOW_UTC);   // 今天 12:00
    assert(app_match_is_today(&match, NOW_UTC));

    match.kickoff_utc = NOW_UTC + 8 * 3600;    // 今天 20:00
    assert(app_match_is_today(&match, NOW_UTC));

    match.kickoff_utc = NOW_UTC + 12 * 3600;   // 明天 00:00(北京),已经不是今天
    assert(!app_match_is_today(&match, NOW_UTC));

    match.kickoff_utc = NOW_UTC - 24 * 3600;   // 昨天 12:00
    assert(!app_match_is_today(&match, NOW_UTC));

    // 还没校时:没有"今天"可判断,不能把整份数据都当过期。
    assert(app_match_is_today(&match, 0));

    // 接口没给开赛时间:无法判断,校时后一律当作不是今天。
    match.kickoff_utc = 0;
    assert(!app_match_is_today(&match, NOW_UTC));
    assert(app_match_is_today(&match, 0));

    assert(!app_match_is_today(NULL, NOW_UTC));
}

static void test_score_text(void) {
    char buf[16];

    app_match_t match = make_match("1", APP_STATE_FIRST_HALF, NOW_UTC);
    match.home_score = 2;
    match.away_score = 1;
    app_match_score_text(&match, buf, sizeof(buf));
    assert(strcmp(buf, "2-1") == 0);

    // 没有比分(未开始 / 接口没给)显示 vs,而不是 0-0。
    match.home_score = -1;
    match.away_score = -1;
    app_match_score_text(&match, buf, sizeof(buf));
    assert(strcmp(buf, "vs") == 0);

    match.home_score = 0;
    match.away_score = -1;
    app_match_score_text(&match, buf, sizeof(buf));
    assert(strcmp(buf, "vs") == 0);

    // 0-0 是合法比分,不能和"未知"混在一起。
    match.away_score = 0;
    app_match_score_text(&match, buf, sizeof(buf));
    assert(strcmp(buf, "0-0") == 0);
}

static void test_datetime_format(void) {
    char buf[24];

    // 2026-10-18T05:00:00Z 在东八区是 10-18 13:00。
    app_format_kickoff(1792299600LL, buf, sizeof(buf));
    assert(strcmp(buf, "10-18 13:00") == 0);

    app_format_clock(1792299600LL, buf, sizeof(buf));
    assert(strcmp(buf, "13:00") == 0);

    // 跨日:UTC 20:30 → 次日 04:30。
    app_format_kickoff(1792269000LL, buf, sizeof(buf));
    assert(strcmp(buf, "10-18 04:30") == 0);

    // 0 / 负数当成"没给",界面显示 "--"。
    app_format_kickoff(0, buf, sizeof(buf));
    assert(strcmp(buf, "--") == 0);
    app_format_kickoff(-5, buf, sizeof(buf));
    assert(strcmp(buf, "--") == 0);
}

static void test_datetime_fields(void) {
    app_datetime_t dt;
    assert(app_datetime_from_utc(1792296000LL, &dt));   // 北京 2026-10-18 12:00 周日
    assert(dt.year == 2026);
    assert(dt.month == 10);
    assert(dt.day == 18);
    assert(dt.hour == 12);
    assert(dt.minute == 0);
    assert(dt.weekday == 0);

    assert(!app_datetime_from_utc(0, &dt));
    assert(!app_datetime_from_utc(0, NULL));
}

static void test_utf8_truncate(void) {
    // "中文" = 6 字节;cap=4 时只能留下一个完整字符 + NUL。
    char text[8] = { (char)0xE4, (char)0xB8, (char)0xAD, (char)0xE6, (char)0x96, (char)0x87, 0, 0 };
    const size_t len = app_utf8_truncate(text, 4);
    assert(len == 3);
    assert(strcmp(text, "中") == 0);

    char fits[8] = "abc";
    assert(app_utf8_truncate(fits, sizeof(fits)) == 3);
    assert(strcmp(fits, "abc") == 0);
}

static void test_text_width(void) {
    assert(app_text_width("abc") == 3);
    assert(app_text_width("中文") == 4);
    assert(app_text_width("a中") == 3);
    assert(app_text_width("") == 0);
    assert(app_text_width(NULL) == 0);
}

// "没有比赛的日子要按天去拉赛程",靠这两个函数算边界。
// 基准:2026-10-21 00:55 北京时间 = 2026-10-20 16:55 UTC。
static void test_cst_day_start(void) {
    const int64_t now = 1792515300;   // 2026-10-20 16:55 UTC

    // 今天 00:00 CST = 2026-10-20 16:00 UTC
    assert(app_cst_day_start(now, 0) == 1792512000);
    assert(app_cst_day_start(now, 1) == 1792598400);
    assert(app_cst_day_start(now, 2) == 1792684800);
    assert(app_cst_day_start(now, -1) == 1792425600);

    // 无效时间不做换算。
    assert(app_cst_day_start(0, 3) == 0);
}

static void test_schedule_start_text(void) {
    char buf[32];

    // 北京时间 10-22 00:00 的 UTC 瞬间,正是接口要的 "2026-10-21 16:00:00"。
    app_schedule_start_text(1792598400, buf, sizeof(buf));
    assert(strcmp(buf, "2026-10-21 16:00:00") == 0);

    // 今天(10-21 00:00 CST)对应前一天 16:00 UTC,跨月也要对。
    app_schedule_start_text(1792512000, buf, sizeof(buf));
    assert(strcmp(buf, "2026-10-20 16:00:00") == 0);

    // 跨年边界:2027-01-01 00:00 CST = 2026-12-31 16:00 UTC
    const int64_t new_year = 1798732800;
    app_schedule_start_text(new_year, buf, sizeof(buf));
    assert(strcmp(buf, "2026-12-31 16:00:00") == 0);

    // 缓冲太小只能截断,不能越界。
    char tiny[8];
    app_schedule_start_text(1792598400, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
}

static void test_match_day_offset(void) {
    const int64_t now = 1792515300;   // 2026-10-21 00:55 CST

    app_match_t match;
    memset(&match, 0, sizeof(match));

    match.kickoff_utc = 1792670400;   // 2026-10-22 20:00 CST
    assert(app_match_day_offset(&match, now) == 1);

    match.kickoff_utc = 1792555200;   // 2026-10-21 12:00 CST,当天
    assert(app_match_day_offset(&match, now) == 0);

    match.kickoff_utc = 1792598400;   // 2026-10-22 00:00 CST,刚过零点
    assert(app_match_day_offset(&match, now) == 1);

    // 没有开赛时间:当作今天,走正常过滤而不是被当成未来。
    match.kickoff_utc = 0;
    assert(app_match_day_offset(&match, now) == 0);

    assert(app_match_day_offset(NULL, now) == 0);
}

int main(void) {
    test_upsert_dedupes_by_id();
    test_upsert_capacity();
    test_sort_order();
    test_state_from_status();
    test_is_live();
    test_state_labels();
    test_state_labels_with_day();
    test_day_offset();
    test_league_whitelist();
    test_is_today();
    test_score_text();
    test_datetime_format();
    test_datetime_fields();
    test_utf8_truncate();
    test_text_width();
    test_cst_day_start();
    test_schedule_start_text();
    test_match_day_offset();
    puts("test_app_model: OK");
    return 0;
}
