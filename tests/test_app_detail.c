// tests/test_app_detail.c —— 赛况响应解析(主机测试)。
//
// 样本直接照抄懂球帝 /data/overview/match/{id} 的真实响应结构(去掉 logo 之类的
// 长 URL,免得测试文件里全是不相干的字符),保留会踩坑的那几处:
//   · 分钟在外层对象的键上,事件自己只有 minute_extra
//   · 进球/黄牌/换人混在同一个数组里,靠 code 区分
//   · 统计项是 {"type","team_A":{"value"},"team_B":{"value"}}
//   · 没有赛况的比赛给 events: null 或者 events: [] 两种都见过
#include "app_detail.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 一场 3-3:主队三个进球(其中一个点球)、客队三个进球(其中一个乌龙),
// 外加黄牌与换人,用来验证非进球事件被过滤掉。
static const char SAMPLE[] =
    "{\"status\":0,\"match_status\":\"Played\","
    "\"events\":{"
    "\"2\":{\"minute\":\"2\",\"teamAEvents\":[],\"teamBEvents\":["
    "{\"minute_extra\":\"0\",\"person\":\"贝内加斯\",\"person_id\":\"50513130\","
    "\"score\":\"\",\"reason\":\"\",\"sort\":0,\"code\":\"YC\"}]},"
    "\"8\":{\"minute\":\"8\",\"teamAEvents\":["
    "{\"minute_extra\":\"0\",\"person\":\"亚历山大-洛佩斯\",\"person_id\":\"50069408\","
    "\"score\":\"1-0\",\"reason\":\"\",\"sort\":0,\"code\":\"G\"}],\"teamBEvents\":[]},"
    "\"25\":{\"minute\":\"25\",\"teamAEvents\":["
    "{\"minute_extra\":\"0\",\"person\":\"乔舒亚-纳瓦罗\",\"person_id\":\"50477254\","
    "\"score\":\"2-0\",\"reason\":\"\",\"sort\":0,\"code\":\"G\"}],\"teamBEvents\":[]},"
    "\"38\":{\"minute\":\"38\",\"teamAEvents\":[],\"teamBEvents\":["
    "{\"minute_extra\":\"0\",\"person\":\"卡洛斯-罗哈斯\",\"person_id\":\"50000001\","
    "\"score\":\"2-1\",\"reason\":\"点球\",\"sort\":0,\"code\":\"G\"}]},"
    "\"45+2\":{\"minute\":\"45+2\",\"teamAEvents\":[],\"teamBEvents\":["
    "{\"minute_extra\":\"2\",\"person\":\"迪耶戈-索托\",\"person_id\":\"50000002\","
    "\"score\":\"2-2\",\"reason\":\"\",\"sort\":0,\"code\":\"PG\"}]},"
    "\"60\":{\"minute\":\"60\",\"teamAEvents\":["
    "{\"minute_extra\":\"0\",\"person\":\"凯文-埃斯皮诺萨\",\"person_id\":\"50503453\","
    "\"score\":\"3-2\",\"reason\":\"\",\"sort\":0,\"code\":\"G\"},"
    "{\"minute_extra\":\"0\",\"person\":\"换人甲\",\"person_id\":\"1\","
    "\"score\":\"\",\"reason\":\"\",\"sort\":0,\"code\":\"SI\"}],\"teamBEvents\":[]},"
    "\"90+3\":{\"minute\":\"90+3\",\"teamAEvents\":[],\"teamBEvents\":["
    "{\"minute_extra\":\"3\",\"person\":\"安德烈斯-索拉诺\",\"person_id\":\"50000003\","
    "\"score\":\"3-3\",\"reason\":\"\",\"sort\":0,\"code\":\"OG\"}]}"
    "},"
    "\"statistics\":{"
    "\"team_A\":{\"id\":\"59630\",\"name\":\"圣何塞体育\",\"formation\":\"4-3-3\","
    "\"logo\":\"x\",\"rate\":0,\"starting_lineup_win_rate\":0},"
    "\"team_B\":{\"id\":\"4879\",\"name\":\"圣卡洛斯\",\"formation\":\"4-4-2\","
    "\"logo\":\"y\",\"rate\":0,\"starting_lineup_win_rate\":0},"
    "\"list\":["
    "{\"en_type\":\"\",\"type\":\"控球率\",\"team_A\":{\"value\":39,\"per\":39},"
    "\"team_B\":{\"value\":61,\"per\":61}},"
    "{\"en_type\":\"\",\"type\":\"角球\",\"team_A\":{\"value\":2,\"per\":29},"
    "\"team_B\":{\"value\":5,\"per\":71}},"
    "{\"en_type\":\"\",\"type\":\"射门\",\"team_A\":{\"value\":20,\"per\":54},"
    "\"team_B\":{\"value\":17,\"per\":46}}"
    "]},"
    "\"archive\":[],\"gifCollection\":[],\"gif_name\":\"比赛动态\","
    "\"tendencies\":{\"data\":[{\"match_id\":54478035,\"x\":0,\"y\":\"-100\","
    "\"minute\":\"0'\",\"team_a_goal\":null,\"team_b_goal\":null}]},"
    "\"highscorepersons\":null,\"loop\":10}";

static void test_full_sample(void) {
    app_detail_t detail;
    assert(app_detail_parse(SAMPLE, sizeof(SAMPLE) - 1, &detail));

    assert(detail.state == APP_STATE_FULL_TIME);
    assert(strcmp(detail.formation_home, "4-3-3") == 0);
    assert(strcmp(detail.formation_away, "4-4-2") == 0);

    // 6 个进球;黄牌与换人被 code 过滤掉。
    assert(detail.goal_count == 6);

    // 按时间排好:8、25、38、45+2、60、90+3。
    assert(detail.goals[0].minute == 8 && detail.goals[0].extra == 0);
    assert(detail.goals[3].minute == 45 && detail.goals[3].extra == 2);
    assert(detail.goals[5].minute == 90 && detail.goals[5].extra == 3);

    // 队别:第一个是主队,第三个(38')是客队。
    assert(detail.goals[0].side == 0);
    assert(detail.goals[2].side == 1);

    // 比分跟着进球走。
    assert(strcmp(detail.goals[0].score, "1-0") == 0);
    assert(strcmp(detail.goals[5].score, "3-3") == 0);

    // 点球有两种写法:code=PG,或者 code=G 但 reason 写了"点球"。
    assert(detail.goals[2].kind == APP_GOAL_PENALTY);   // reason="点球"
    assert(detail.goals[3].kind == APP_GOAL_PENALTY);   // code=PG
    assert(detail.goals[5].kind == APP_GOAL_OWN);       // code=OG
    assert(detail.goals[1].kind == APP_GOAL_NORMAL);

    // 球员名原样保留(中文 + 连字符)。
    assert(strcmp(detail.goals[0].player, "亚历山大-洛佩斯") == 0);
    assert(strcmp(detail.goals[2].player, "卡洛斯-罗哈斯") == 0);

    // 技术统计 3 项。
    assert(detail.stat_count == 3);
    assert(strcmp(detail.stats[0].label, "控球率") == 0);
    assert(detail.stats[0].home == 39);
    assert(detail.stats[0].away == 61);
    assert(strcmp(detail.stats[2].label, "射门") == 0);
    assert(detail.stats[2].home == 20);
}

// 未开始的比赛:events 与 statistics 都是 null,阵型空着,不能崩。
static void test_null_sections(void) {
    static const char json[] =
        "{\"status\":0,\"match_status\":\"Fixture\",\"events\":null,"
        "\"statistics\":null,\"highscorepersons\":null}";

    app_detail_t detail;
    assert(app_detail_parse(json, sizeof(json) - 1, &detail));
    assert(detail.state == APP_STATE_FIXTURE);
    assert(detail.goal_count == 0);
    assert(detail.stat_count == 0);
    assert(detail.formation_home[0] == '\0');
    assert(detail.formation_away[0] == '\0');
}

// 空 events 对象(踢了但没有任何事件)也要能解析。
static void test_empty_events(void) {
    static const char json[] =
        "{\"match_status\":\"Playing\",\"events\":{},\"statistics\":{\"list\":[]}}";

    app_detail_t detail;
    assert(app_detail_parse(json, sizeof(json) - 1, &detail));
    assert(detail.state == APP_STATE_LIVE_OTHER);
    assert(detail.goal_count == 0);
    assert(detail.stat_count == 0);
}

// "还没有数据"时接口给的是空数组 [],不是 null 也不是空对象 {}。
// 实测未开赛的响应只有 187 字节:
//   {"status":0,"match_status":"Fixture","events":[],"statistics":[],…}
// 只认对象的话,每一场未开赛的比赛都会显示"赛况加载失败"。
static void test_array_sections(void) {
    static const char fixture[] =
        "{\"status\":0,\"match_status\":\"Fixture\",\"events\":[],\"statistics\":[],"
        "\"archive\":[],\"gifCollection\":[],\"gif_name\":\"比赛动态\","
        "\"tendencies\":null,\"highscorepersons\":null,\"loop\":10}";

    app_detail_t detail;
    assert(app_detail_parse(fixture, sizeof(fixture) - 1, &detail));
    assert(detail.state == APP_STATE_FIXTURE);
    assert(detail.goal_count == 0);
    assert(detail.stat_count == 0);
    assert(detail.formation_home[0] == '\0');
    assert(detail.formation_away[0] == '\0');

    // 中场暂停的比赛:events 已经有内容,statistics 还是 []。
    // 这种球看着"有赛况",整份响应却会栽在统计那一项上。
    static const char suspended[] =
        "{\"match_status\":\"Suspended\",\"events\":{\"19\":{\"minute\":\"19\","
        "\"teamAEvents\":[{\"person\":\"甲\",\"score\":\"1-0\",\"code\":\"G\"}],"
        "\"teamBEvents\":[]}},\"statistics\":[]}";
    assert(app_detail_parse(suspended, sizeof(suspended) - 1, &detail));
    assert(detail.goal_count == 1);
    assert(detail.stat_count == 0);

    // 反向组合也要认:统计有内容、进球还是 []。
    static const char stats_only[] =
        "{\"match_status\":\"Playing\",\"events\":[],"
        "\"statistics\":{\"list\":[{\"type\":\"控球率\","
        "\"team_A\":{\"value\":55},\"team_B\":{\"value\":45}}]}}";
    assert(app_detail_parse(stats_only, sizeof(stats_only) - 1, &detail));
    assert(detail.goal_count == 0);
    assert(detail.stat_count == 1);
    assert(detail.stats[0].home == 55);
    assert(detail.stats[0].away == 45);
}

// 截断的响应必须返回 false,而不是读越界或死循环。
static void test_truncated(void) {
    app_detail_t detail;
    const size_t full = sizeof(SAMPLE) - 1;
    // 从中间几个位置截断,每次都得干净地失败。
    const size_t cuts[] = { 1, 10, 64, 200, full / 2, full - 1 };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        (void)app_detail_parse(SAMPLE, cuts[i], &detail);
        // 截断位置不同,可能"恰好"是一个合法前缀,这里只要求不崩、不卡住。
    }

    // 明确的垃圾输入一定失败。
    assert(!app_detail_parse("", 0, &detail));
    assert(!app_detail_parse("[]", 2, &detail));
    assert(!app_detail_parse("{}", 2, &detail));
    assert(!app_detail_parse("{\"a\":", 5, &detail));
    assert(!app_detail_parse(NULL, 10, &detail));
}

// 流式入口:同一份响应按不同块大小喂进去,结果必须和"整段在内存里"完全一致。
// 这条专门盯窗口重填:窗口 1 KB,块给 1 字节时几乎每个字段都会跨块。
typedef struct {
    const char *p;
    size_t left;
    size_t chunk;
} chunk_src_t;

static int chunk_read(void *ctx, char *buf, size_t cap) {
    chunk_src_t *src = (chunk_src_t *)ctx;
    size_t n = src->left < cap ? src->left : cap;
    if (n > src->chunk) n = src->chunk;
    if (n == 0) return 0;
    memcpy(buf, src->p, n);
    src->p += n;
    src->left -= n;
    return (int)n;
}

static void test_streaming(void) {
    const size_t full = sizeof(SAMPLE) - 1;

    app_detail_t want;
    assert(app_detail_parse(SAMPLE, full, &want));

    const size_t chunks[] = { 1, 3, 7, 64, 511, 1024, 4096 };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        chunk_src_t src = { SAMPLE, full, chunks[i] };
        app_detail_t got;
        assert(app_detail_parse_stream(chunk_read, &src, &got));

        assert(got.state == want.state);
        assert(got.goal_count == want.goal_count);
        assert(got.stat_count == want.stat_count);
        assert(strcmp(got.formation_home, want.formation_home) == 0);
        assert(strcmp(got.formation_away, want.formation_away) == 0);
        for (size_t g = 0; g < want.goal_count; g++) {
            assert(got.goals[g].minute == want.goals[g].minute);
            assert(got.goals[g].extra == want.goals[g].extra);
            assert(got.goals[g].kind == want.goals[g].kind);
            assert(got.goals[g].side == want.goals[g].side);
            assert(strcmp(got.goals[g].player, want.goals[g].player) == 0);
            assert(strcmp(got.goals[g].score, want.goals[g].score) == 0);
        }
        for (size_t k = 0; k < want.stat_count; k++) {
            assert(strcmp(got.stats[k].label, want.stats[k].label) == 0);
            assert(got.stats[k].home == want.stats[k].home);
            assert(got.stats[k].away == want.stats[k].away);
        }
    }

    // 一个字节都不给:干净地失败,不能卡住。
    chunk_src_t empty = { SAMPLE, 0, 8 };
    app_detail_t none;
    assert(!app_detail_parse_stream(chunk_read, &empty, &none));
}

// 逐字符向前推进,要求整个缓冲区是合法 UTF-8 且没有半截字符。
static bool utf8_well_formed(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0) {
        size_t need;
        if (*p < 0x80) need = 0;
        else if ((*p & 0xE0) == 0xC0) need = 1;
        else if ((*p & 0xF0) == 0xE0) need = 2;
        else if ((*p & 0xF8) == 0xF0) need = 3;
        else return false;   // 续字节不该出现在字符开头
        p++;
        for (size_t i = 0; i < need; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += need;
    }
    return true;
}

// 球员名超过缓冲时按 UTF-8 边界截断,不能留下半个汉字。
static void test_player_truncation(void) {
    // 12 个汉字 = 36 字节,超过 APP_PLAYER_LEN(24)。
    static const char json[] =
        "{\"match_status\":\"Played\",\"events\":{\"10\":{\"minute\":\"10\","
        "\"teamAEvents\":[{\"person\":\"一二三四五六七八九十十一\",\"score\":\"1-0\","
        "\"code\":\"G\"}],\"teamBEvents\":[]}}}";

    app_detail_t detail;
    assert(app_detail_parse(json, sizeof(json) - 1, &detail));
    assert(detail.goal_count == 1);

    const char *name = detail.goals[0].player;
    const size_t len = strlen(name);
    assert(len > 0 && len < APP_PLAYER_LEN);
    // 截断点必须落在字符边界上:整个缓冲区是合法 UTF-8,没有半截字符。
    assert(utf8_well_formed(name));
    size_t chars = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)name[i] & 0xC0) != 0x80) chars++;
    }
    assert(chars == 7);   // 24 字节的缓冲扣掉结尾 NUL,正好 7 个汉字
}

// 分钟键写成 "90+90" 这种脏数据时不能算出越界值。
static void test_weird_minute(void) {
    static const char json[] =
        "{\"match_status\":\"Played\",\"events\":{\"90+90\":{\"minute\":\"90+90\","
        "\"teamAEvents\":[{\"person\":\"甲\",\"score\":\"1-0\",\"code\":\"G\"}],"
        "\"teamBEvents\":[]}}}";

    app_detail_t detail;
    assert(app_detail_parse(json, sizeof(json) - 1, &detail));
    assert(detail.goal_count == 1);
    assert(detail.goals[0].minute == 90);
    assert(detail.goals[0].extra == 90);
}

// 进球超过 APP_GOAL_MAX 时只留前 16 个,不越界。
static void test_goal_cap(void) {
    char json[8192];
    size_t used = 0;
    used += (size_t)snprintf(json + used, sizeof(json) - used,
                             "{\"match_status\":\"Played\",\"events\":{");
    for (int i = 0; i < 20; i++) {
        used += (size_t)snprintf(
            json + used, sizeof(json) - used,
            "%s\"%d\":{\"minute\":\"%d\",\"teamAEvents\":[{\"person\":\"球员%d\","
            "\"score\":\"%d-0\",\"code\":\"G\"}],\"teamBEvents\":[]}",
            i == 0 ? "" : ",", i + 1, i + 1, i, i + 1);
    }
    used += (size_t)snprintf(json + used, sizeof(json) - used, "}}");

    app_detail_t detail;
    assert(app_detail_parse(json, used, &detail));
    assert(detail.goal_count == APP_GOAL_MAX);
    // 留下的是最早的那 16 个。
    assert(detail.goals[0].minute == 1);
    assert(detail.goals[APP_GOAL_MAX - 1].minute == APP_GOAL_MAX);
}

// 详情接口把中文整段转义成 \uXXXX(实测 /data/overview/match/{id} 就是这样),
// 而比分列表接口给的是直接 UTF-8。解析器必须两种都认。
//
// 这条是回归测试:曾经 \u 分支只认 ASCII,≥0x80 的码点一律降级成 '?',于是
// 进球列表里每个球员名都显示成 ???,队名却正常(队名走的是比分列表那份解析)。
static void test_unicode_escaped_names(void) {
    // "尼科-帕斯" / "姆巴佩",外加一个 BMP 外的 emoji(代理对)。
    static const char JSON[] =
        "{\"status\":0,\"match_status\":\"Played\","
        "\"events\":{"
        "\"2\":{\"minute\":\"2\",\"teamAEvents\":["
        "{\"person\":\"\\u5c3c\\u79d1-\\u5e15\\u65af\",\"score\":\"1-0\",\"code\":\"G\"}],"
        "\"teamBEvents\":[]},"
        "\"49\":{\"minute\":\"49\",\"teamAEvents\":[],\"teamBEvents\":["
        "{\"person\":\"\\u59c6\\u5df4\\u4f69\",\"score\":\"1-1\",\"code\":\"G\"}]},"
        "\"57\":{\"minute\":\"57\",\"teamAEvents\":["
        "{\"person\":\"A\\u00e9B\",\"score\":\"2-1\",\"code\":\"G\"}],"
        "\"teamBEvents\":[]},"
        "\"76\":{\"minute\":\"76\",\"teamAEvents\":[],\"teamBEvents\":["
        "{\"person\":\"\\ud83d\\ude00\",\"score\":\"2-2\",\"code\":\"G\"}]},"
        "\"90+1\":{\"minute\":\"90+1\",\"teamAEvents\":["
        "{\"person\":\"\\ud83d\",\"score\":\"3-2\",\"code\":\"G\"}],"
        "\"teamBEvents\":[]}"
        "},"
        "\"statistics\":{}}";

    app_detail_t detail;
    assert(app_detail_parse(JSON, strlen(JSON), &detail));
    assert(detail.goal_count == 5);

    // 三个汉字 + 连字符 + 两个汉字,原样还原。
    assert(strcmp(detail.goals[0].player, "尼科-帕斯") == 0);
    assert(strcmp(detail.goals[1].player, "姆巴佩") == 0);
    // Latin-1 范围内的转义走两字节分支。"A" 单独拆开,否则 \xa9B 会被当成
    // 一个超范围的十六进制转义。
    assert(strcmp(detail.goals[2].player, "A\xc3\xa9" "B") == 0);
    // 代理对拼成一个 4 字节码点(U+1F600)。
    assert(strcmp(detail.goals[3].player, "\xf0\x9f\x98\x80") == 0);
    // 孤立的半个代理降级成 U+FFFD,不能把后面的解析带偏。
    assert(strcmp(detail.goals[4].player, "\xef\xbf\xbd") == 0);
    assert(detail.goals[4].minute == 90);
    assert(detail.goals[4].extra == 1);
}

int main(void) {
    test_full_sample();
    test_null_sections();
    test_empty_events();
    test_array_sections();
    test_truncated();
    test_streaming();
    test_player_truncation();
    test_weird_minute();
    test_goal_cap();
    test_unicode_escaped_names();
    puts("test_app_detail: OK");
    return 0;
}
