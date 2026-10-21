// tests/test_app_json.c —— 懂球帝比分接口的流式解析测试。
//
// 夹具按真实响应的形状手写:顶层 {"matches":[...]},每条比赛有 match_id /
// status / period / minute / match_timestamp / cmp_type,联赛与队名在
// competition / team_A / team_B 三个子对象里。
#include "app_json.h"
#include "app_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 2026-10-18T04:00:00Z = 北京时间 10-18 12:00,作为"现在"。
#define NOW_UTC 1792296000LL

// 一场正在直播的英超比赛。
static const char *LIVE_OBJECT =
    "{\"match_id\":\"5001\",\"status\":\"Playing\",\"period\":\"2H\",\"minute\":67,"
    "\"injury_time\":0,\"match_timestamp\":1792323000,\"cmp_type\":\"soccer\","
    "\"competition\":{\"name\":\"英超\",\"id\":\"3\"},"
    "\"team_A\":{\"name\":\"曼城\",\"fs\":2,\"hts\":1,\"logo\":\"https://x/a.png\"},"
    "\"team_B\":{\"name\":\"阿森纳\",\"fs\":1,\"hts\":0,\"logo\":\"https://x/b.png\"}}";

// 一场未开始的比赛,比分字段是 null。
static const char *FIXTURE_OBJECT =
    "{\"match_id\":\"5002\",\"status\":\"Fixture\",\"period\":\"\",\"minute\":0,"
    "\"match_timestamp\":1792326600,\"cmp_type\":\"soccer\","
    "\"competition\":{\"name\":\"西甲\"},"
    "\"team_A\":{\"name\":\"皇马\",\"fs\":null},"
    "\"team_B\":{\"name\":\"巴萨\",\"fs\":null}}";

static void test_parse_live_object(void) {
    app_match_t match;
    assert(app_parse_match_object(LIVE_OBJECT, strlen(LIVE_OBJECT), &match));

    assert(strcmp(match.id, "5001") == 0);
    assert(strcmp(match.league, "英超") == 0);
    assert(strcmp(match.home, "曼城") == 0);
    assert(strcmp(match.away, "阿森纳") == 0);
    assert(match.home_score == 2);
    assert(match.away_score == 1);
    assert(match.state == APP_STATE_SECOND_HALF);
    assert(match.minute == 67);
    assert(match.kickoff_utc == 1792323000LL);
}

static void test_parse_fixture_object(void) {
    app_match_t match;
    assert(app_parse_match_object(FIXTURE_OBJECT, strlen(FIXTURE_OBJECT), &match));

    assert(strcmp(match.id, "5002") == 0);
    assert(match.state == APP_STATE_FIXTURE);
    assert(match.minute == 0);
    // null 比分 = 未知(-1),界面显示 "vs" 而不是 0-0。
    assert(match.home_score == -1);
    assert(match.away_score == -1);

    char score[16];
    app_match_score_text(&match, score, sizeof(score));
    assert(strcmp(score, "vs") == 0);
}

static void test_unicode_escape(void) {
    const char *obj =
        "{\"match_id\":\"7\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
        "\"competition\":{\"name\":\"\\u4e2d\\u8d85\"},"
        "\"team_A\":{\"name\":\"A\\u0026B\"},\"team_B\":{\"name\":\"C\"}}";
    app_match_t match;
    assert(app_parse_match_object(obj, strlen(obj), &match));
    assert(strcmp(match.league, "中超") == 0);
    assert(strcmp(match.home, "A&B") == 0);
    assert(strcmp(match.away, "C") == 0);
}

static void test_rejects_non_soccer(void) {
    const char *obj =
        "{\"match_id\":\"9\",\"status\":\"Playing\",\"cmp_type\":\"basketball\","
        "\"competition\":{\"name\":\"NBA\"},"
        "\"team_A\":{\"name\":\"湖人\"},\"team_B\":{\"name\":\"勇士\"}}";
    app_match_t match;
    assert(!app_parse_match_object(obj, strlen(obj), &match));
}

static void test_rejects_incomplete(void) {
    app_match_t match;

    // 没有 match_id:无法去重,直接丢。
    const char *no_id =
        "{\"status\":\"Playing\",\"team_A\":{\"name\":\"A\"},\"team_B\":{\"name\":\"B\"}}";
    assert(!app_parse_match_object(no_id, strlen(no_id), &match));

    // 没有 status:不知道该显示什么。
    const char *no_status =
        "{\"match_id\":\"1\",\"team_A\":{\"name\":\"A\"},\"team_B\":{\"name\":\"B\"}}";
    assert(!app_parse_match_object(no_status, strlen(no_status), &match));

    // 缺队名:界面上没法显示。
    const char *no_away =
        "{\"match_id\":\"1\",\"status\":\"Playing\",\"team_A\":{\"name\":\"A\"}}";
    assert(!app_parse_match_object(no_away, strlen(no_away), &match));

    assert(!app_parse_match_object(NULL, 0, &match));
    assert(!app_parse_match_object("", 0, &match));
}

// 把一段 JSON 按 chunk 字节一块喂进去,模拟 HTTP 分块读取。
static void feed_in_chunks(app_feed_t *feed, const char *json, size_t chunk) {
    const size_t len = strlen(json);
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        app_feed_write(feed, json + off, n);
    }
    app_feed_finish(feed);
}

static void test_feed_finds_matches_array(void) {
    // 前面有别的顶层键,解析器必须能跳过它们找到 "matches"。
    char json[2048];
    snprintf(json, sizeof(json),
             "{\"code\":0,\"message\":\"ok\",\"meta\":{\"total\":2},\"matches\":[%s,%s]}",
             LIVE_OBJECT, FIXTURE_OBJECT);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 5);   // 5 字节一块:几乎每个对象都被切开

    assert(feed.objects_seen == 2);
    assert(list.count == 2);

    app_match_list_sort(&list);
    // 直播中的排最前。
    assert(strcmp(list.items[0].id, "5001") == 0);
    assert(strcmp(list.items[1].id, "5002") == 0);
}

static void test_feed_filters_by_state_and_day(void) {
    char json[4096];
    snprintf(json, sizeof(json),
             "{\"matches\":["
             "%s,"                                                      // 直播:留
             "%s,"                                                      // 未开始(今天):留
             // 昨天已完场:丢(只看当天)
             "{\"match_id\":\"6001\",\"status\":\"Played\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":1792200000,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"A\",\"fs\":3},\"team_B\":{\"name\":\"B\",\"fs\":0}},"
             // 推迟:丢
             "{\"match_id\":\"6002\",\"status\":\"Postponed\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":1792326600,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"C\"},\"team_B\":{\"name\":\"D\"}},"
             // 未开始但开赛时间已过(数据滞后):丢
             "{\"match_id\":\"6003\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":1792295000,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"E\"},\"team_B\":{\"name\":\"F\"}},"
             // 未开始但不是今天:丢
             "{\"match_id\":\"6004\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":1792500000,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"G\"},\"team_B\":{\"name\":\"H\"}},"
             // 篮球:丢
             "{\"match_id\":\"6005\",\"status\":\"Playing\",\"cmp_type\":\"basketball\","
             "\"match_timestamp\":1792326600,\"competition\":{\"name\":\"NBA\"},"
             "\"team_A\":{\"name\":\"I\"},\"team_B\":{\"name\":\"J\"}}"
             "]}",
             LIVE_OBJECT, FIXTURE_OBJECT);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 64);

    assert(list.count == 2);
    // 6 个对象能解析成比赛(篮球那条在 cmp_type 检查就被拒了),
    // 但只有直播和今天窗口内的未开始能留下来。
    assert(feed.objects_seen == 6);
    assert(feed.dropped_day == 2);   // 6001 是昨天,6004 不是今天
    for (size_t i = 0; i < list.count; i++) {
        assert(strcmp(list.items[i].id, "6001") != 0);
        assert(strcmp(list.items[i].id, "6002") != 0);
        assert(strcmp(list.items[i].id, "6003") != 0);
        assert(strcmp(list.items[i].id, "6004") != 0);
        assert(strcmp(list.items[i].id, "6005") != 0);
    }
}

static void test_feed_league_whitelist(void) {
    // 五大联赛与中甲都留,其它赛事(欧冠/英冠/中乙/巴甲)一律丢。
    char json[4096];
    snprintf(json, sizeof(json),
             "{\"matches\":["
             "{\"match_id\":\"7101\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"A\"},\"team_B\":{\"name\":\"B\"}},"
             "{\"match_id\":\"7102\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"中甲\"},"
             "\"team_A\":{\"name\":\"C\"},\"team_B\":{\"name\":\"D\"}},"
             "{\"match_id\":\"7103\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"欧冠\"},"
             "\"team_A\":{\"name\":\"E\"},\"team_B\":{\"name\":\"F\"}},"
             "{\"match_id\":\"7104\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"英冠\"},"
             "\"team_A\":{\"name\":\"G\"},\"team_B\":{\"name\":\"H\"}},"
             "{\"match_id\":\"7105\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"中乙\"},"
             "\"team_A\":{\"name\":\"I\"},\"team_B\":{\"name\":\"J\"}},"
             "{\"match_id\":\"7106\",\"status\":\"Playing\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"巴西甲级联赛\"},"
             "\"team_A\":{\"name\":\"K\"},\"team_B\":{\"name\":\"L\"}}"
             "]}",
             (long long)NOW_UTC, (long long)NOW_UTC, (long long)NOW_UTC,
             (long long)NOW_UTC, (long long)NOW_UTC, (long long)NOW_UTC);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 53);

    assert(list.count == 2);
    assert(feed.dropped_league == 4);
    assert(strcmp(list.items[0].league, "英超") == 0);
    assert(strcmp(list.items[1].league, "中甲") == 0);
}

static void test_feed_keeps_todays_finished(void) {
    // 当天已完场的要留着:一天看下来,前面的比分同样是信息。
    char json[2048];
    snprintf(json, sizeof(json),
             "{\"matches\":[{\"match_id\":\"8001\",\"status\":\"Played\",\"period\":\"FT\","
             "\"cmp_type\":\"soccer\",\"match_timestamp\":%lld,"
             "\"competition\":{\"name\":\"中超\"},"
             "\"team_A\":{\"name\":\"上海海港\",\"fs\":3},"
             "\"team_B\":{\"name\":\"北京国安\",\"fs\":1}}]}",
             (long long)(NOW_UTC - 3 * 3600));

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 41);

    assert(list.count == 1);
    assert(feed.finished_kept == 1);
    assert(list.items[0].state == APP_STATE_FULL_TIME);

    char score[8];
    app_match_score_text(&list.items[0], score, sizeof(score));
    assert(strcmp(score, "3-1") == 0);
}

static void test_feed_caps_live_and_upcoming(void) {
    // 15 场直播 + 15 场未开始:分类上限各 10 条,合计 20 条。
    // 开赛时间都落在今天之内,否则会先被"只看当天"筛掉。
    char json[16384];
    size_t used = 0;
    used += (size_t)snprintf(json + used, sizeof(json) - used, "{\"matches\":[");
    for (int i = 0; i < 15; i++) {
        used += (size_t)snprintf(
            json + used, sizeof(json) - used,
            "%s{\"match_id\":\"L%d\",\"status\":\"Playing\",\"period\":\"1H\",\"minute\":%d,"
            "\"cmp_type\":\"soccer\",\"match_timestamp\":%lld,"
            "\"competition\":{\"name\":\"英超\"},"
            "\"team_A\":{\"name\":\"主队\",\"fs\":0},\"team_B\":{\"name\":\"客队\",\"fs\":0}}",
            i == 0 ? "" : ",", i, i + 1, (long long)NOW_UTC);
    }
    for (int i = 0; i < 15; i++) {
        used += (size_t)snprintf(
            json + used, sizeof(json) - used,
            ",{\"match_id\":\"F%d\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
            "\"match_timestamp\":%lld,\"competition\":{\"name\":\"英超\"},"
            "\"team_A\":{\"name\":\"主队\"},\"team_B\":{\"name\":\"客队\"}}",
            i, (long long)(NOW_UTC + 60 * (i + 1)));
    }
    snprintf(json + used, sizeof(json) - used, "]}");

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 97);

    assert(feed.live_kept == 10);
    assert(feed.upcoming_kept == 10);
    assert(feed.finished_kept == 0);
    // 分类上限之和不超过列表容量,所以不会出现"先收进来的把后面的挤掉"。
    assert(list.count == 20);
}

static void test_feed_dedupes_across_chunks(void) {
    // 同一场比赛出现两次(接口里重复下发):按 id 去重,比分取最后一次。
    char json[2048];
    snprintf(json, sizeof(json), "{\"matches\":[%s,%s]}", LIVE_OBJECT, LIVE_OBJECT);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 13);

    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "5001") == 0);
}

// 今天没有白名单赛事时,界面改显示"下一个比赛日"。解析器要能只留某一天:
// target_day=0 是今天(直播中的豁免),1..7 是未来第 N 天(整份响应都是那天,
// 所以连直播都不该混进来)。
static void test_feed_target_day(void) {
    const long long today = NOW_UTC + 3600;              // 今天 21:00 CST
    const long long tomorrow = NOW_UTC + 86400 + 3600;   // 明天同一时刻
    const long long day_after = NOW_UTC + 2 * 86400 + 3600;

    char json[2048];
    snprintf(json, sizeof(json),
             "{\"matches\":["
             "{\"match_id\":\"7001\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"甲\"},\"team_B\":{\"name\":\"乙\"}},"
             "{\"match_id\":\"7002\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"西甲\"},"
             "\"team_A\":{\"name\":\"丙\"},\"team_B\":{\"name\":\"丁\"}},"
             "{\"match_id\":\"7003\",\"status\":\"Fixture\",\"cmp_type\":\"soccer\","
             "\"match_timestamp\":%lld,\"competition\":{\"name\":\"意甲\"},"
             "\"team_A\":{\"name\":\"戊\"},\"team_B\":{\"name\":\"己\"}},"
             "{\"match_id\":\"7004\",\"status\":\"Playing\",\"period\":\"1H\",\"minute\":20,"
             "\"cmp_type\":\"soccer\",\"match_timestamp\":%lld,"
             "\"competition\":{\"name\":\"德甲\"},"
             "\"team_A\":{\"name\":\"庚\",\"fs\":1},\"team_B\":{\"name\":\"辛\",\"fs\":0}}"
             "]}",
             today, tomorrow, day_after, today);

    app_match_list_t list;
    app_feed_t feed;

    // 今天:7001 + 直播中的 7004。
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 47);
    assert(list.count == 2);

    // 明天:只要 7002。今天那场直播不该被算进"明天"。
    app_feed_init(&feed, &list, NOW_UTC);
    feed.target_day = 1;
    feed_in_chunks(&feed, json, 47);
    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "7002") == 0);

    // 后天。
    app_feed_init(&feed, &list, NOW_UTC);
    feed.target_day = 2;
    feed_in_chunks(&feed, json, 47);
    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "7003") == 0);

    // 目标日没有比赛:列表空着,界面据此说"近期没有比赛"。
    app_feed_init(&feed, &list, NOW_UTC);
    feed.target_day = 5;
    feed_in_chunks(&feed, json, 47);
    assert(list.count == 0);

    // 未来比赛日的未开始比赛,开赛时间当然晚于 now —— 不能被
    // "已过开赛时间却标着未开始"那条判据砍掉。
    assert(list.count == 0);
}

static void test_truncated_tail_is_dropped(void) {
    // 模拟连接中途断开:第二个对象只收了一半。
    char json[2048];
    snprintf(json, sizeof(json), "{\"matches\":[%s,{\"match_id\":\"5003\",\"status\":\"Play",
             LIVE_OBJECT);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, json, 31);

    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "5001") == 0);
}

static void test_garbage_input(void) {
    app_match_list_t list;
    app_feed_t feed;

    app_feed_init(&feed, &list, NOW_UTC);
    app_feed_write(&feed, "", 0);   // 空块不炸
    app_feed_finish(&feed);
    assert(list.count == 0);

    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, "not json at all", 4);
    assert(list.count == 0);

    app_feed_init(&feed, &list, NOW_UTC);
    feed_in_chunks(&feed, "{\"matches\":[", 4);
    assert(list.count == 0);

    // NULL 参数必须安全返回。
    app_feed_write(NULL, "x", 1);
    app_feed_finish(NULL);
    app_feed_init(NULL, &list, NOW_UTC);
}

static void test_no_time_filter_without_now(void) {
    // now_utc = 0 时不做日期过滤(主机测试/离线回放用);联赛白名单仍然生效。
    char json[4096];
    snprintf(json, sizeof(json),
             "{\"matches\":[%s,{\"match_id\":\"7001\",\"status\":\"Fixture\","
             "\"cmp_type\":\"soccer\",\"match_timestamp\":1792926000,"
             "\"competition\":{\"name\":\"英超\"},"
             "\"team_A\":{\"name\":\"A\"},\"team_B\":{\"name\":\"B\"}}]}",
             FIXTURE_OBJECT);

    app_match_list_t list;
    app_feed_t feed;
    app_feed_init(&feed, &list, 0);
    feed_in_chunks(&feed, json, 64);

    assert(list.count == 2);
}

int main(void) {
    test_parse_live_object();
    test_parse_fixture_object();
    test_unicode_escape();
    test_rejects_non_soccer();
    test_rejects_incomplete();
    test_feed_finds_matches_array();
    test_feed_filters_by_state_and_day();
    test_feed_league_whitelist();
    test_feed_keeps_todays_finished();
    test_feed_caps_live_and_upcoming();
    test_feed_dedupes_across_chunks();
    test_feed_target_day();
    test_truncated_tail_is_dropped();
    test_garbage_input();
    test_no_time_filter_without_now();
    puts("test_app_json: OK");
    return 0;
}
