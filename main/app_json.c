// main/app_json.c
#include "app_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool token_equals(const char *text, size_t len, const char *want) {
    const size_t want_len = strlen(want);
    return len == want_len && memcmp(text, want, len) == 0;
}

static void copy_raw(char *dst, size_t cap, const char *src, size_t len) {
    if (cap == 0) return;
    if (len >= cap) {
        // 宁可截断也不要把一个 UTF-8 字符劈开。
        len = cap - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// 把 JSON 字符串字面量(从引号后开始)解码进 dst。
// 只处理 \\ \" \/ \n \r \t \uXXXX —— 队名里不会出现别的转义。
static size_t decode_string(const char *src, size_t len, char *dst, size_t cap) {
    size_t i = 0;
    size_t o = 0;
    while (i < len && o + 1 < cap) {
        const unsigned char c = (unsigned char)src[i];
        if (c != '\\') {
            dst[o++] = (char)c;
            i++;
            continue;
        }
        i++;
        if (i >= len) break;
        const char esc = src[i++];
        switch (esc) {
            case 'n': dst[o++] = '\n'; break;
            case 'r': dst[o++] = '\r'; break;
            case 't': dst[o++] = '\t'; break;
            case '"': dst[o++] = '"'; break;
            case '\\': dst[o++] = '\\'; break;
            case '/': dst[o++] = '/'; break;
            case 'u': {
                if (i + 4 > len) { i = len; break; }
                unsigned value = 0;
                bool ok = true;
                for (int k = 0; k < 4; k++) {
                    const char h = src[i + (size_t)k];
                    unsigned digit;
                    if (h >= '0' && h <= '9') digit = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') digit = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') digit = (unsigned)(h - 'A' + 10);
                    else { ok = false; break; }
                    value = value * 16u + digit;
                }
                i += 4;
                if (!ok) break;
                // 只处理 BMP 内的字符;懂球帝的队名都是中文,不会用到代理对。
                if (value < 0x80) {
                    dst[o++] = (char)value;
                } else if (value < 0x800) {
                    if (o + 2 >= cap) { i = len; break; }
                    dst[o++] = (char)(0xC0 | (value >> 6));
                    dst[o++] = (char)(0x80 | (value & 0x3F));
                } else {
                    if (o + 3 >= cap) { i = len; break; }
                    dst[o++] = (char)(0xE0 | (value >> 12));
                    dst[o++] = (char)(0x80 | ((value >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (value & 0x3F));
                }
                break;
            }
            default:
                dst[o++] = esc;
                break;
        }
    }
    dst[o] = '\0';
    return o;
}

// 在对象文本里按层级取值:
//   section == NULL  取顶层键(深度 1)
//   section == "team_A" 取 team_A 子对象里的键
// 找到就写进 dst 并返回 true。值可以是字符串,也可以是数字/裸值。
static bool obj_get(const char *obj, size_t len, const char *section, const char *key,
                    char *dst, size_t cap) {
    size_t i = 0;
    int depth = 0;

    while (i < len) {
        const char c = obj[i];
        if (c != '"') {
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
            i++;
            continue;
        }

        // 读一个字符串字面量
        const size_t start = ++i;
        bool esc = false;
        while (i < len) {
            if (esc) { esc = false; i++; continue; }
            if (obj[i] == '\\') { esc = true; i++; continue; }
            if (obj[i] == '"') break;
            i++;
        }
        const size_t end = i;
        if (i < len) i++;

        // 只有后面跟着 ':' 的才是键
        size_t j = i;
        while (j < len && is_space(obj[j])) j++;
        if (j >= len || obj[j] != ':') continue;
        j++;
        while (j < len && is_space(obj[j])) j++;
        if (j >= len) break;

        if (section == NULL) {
            if (depth == 1 && token_equals(obj + start, end - start, key)) {
                if (obj[j] == '"') {
                    // 字符串值:找到收尾引号后解码
                    size_t k = j + 1;
                    bool e2 = false;
                    while (k < len) {
                        if (e2) { e2 = false; k++; continue; }
                        if (obj[k] == '\\') { e2 = true; k++; continue; }
                        if (obj[k] == '"') break;
                        k++;
                    }
                    decode_string(obj + j + 1, k - (j + 1), dst, cap);
                    return true;
                }
                // 数字 / true / false / null:原样拷到分隔符
                size_t k = j;
                while (k < len && obj[k] != ',' && obj[k] != '}' && obj[k] != ']') k++;
                size_t e = k;
                while (e > j && is_space(obj[e - 1])) e--;
                copy_raw(dst, cap, obj + j, e - j);
                return true;
            }
        } else if (depth == 1 && token_equals(obj + start, end - start, section)
                   && obj[j] == '{') {
            // 进入子对象重新扫描(从 '{' 开始,深度会重新变成 1)
            return obj_get(obj + j, len - j, NULL, key, dst, cap);
        }
        i = j;
    }
    return false;
}

static int parse_int_or(const char *text, int fallback) {
    if (text == NULL || text[0] == '\0') return fallback;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text) return fallback;
    return (int)value;
}

bool app_parse_match_object(const char *obj, size_t len, app_match_t *out) {
    if (obj == NULL || out == NULL || len == 0) return false;
    memset(out, 0, sizeof(*out));
    out->home_score = -1;
    out->away_score = -1;

    char status[16] = "";
    char period[8] = "";
    char id[APP_ID_LEN] = "";
    char ts[24] = "";
    char score_a[8] = "";
    char score_b[8] = "";
    char minute[8] = "";

    if (!obj_get(obj, len, NULL, "match_id", id, sizeof(id))) return false;
    if (id[0] == '\0') return false;
    if (!obj_get(obj, len, NULL, "status", status, sizeof(status))) return false;

    // 只关心足球;接口用 cmp_type 区分项目,顺手再挡一次。
    char cmp[16] = "";
    if (obj_get(obj, len, NULL, "cmp_type", cmp, sizeof(cmp)) && cmp[0] != '\0'
        && strcmp(cmp, "soccer") != 0) {
        return false;
    }

    obj_get(obj, len, NULL, "period", period, sizeof(period));
    obj_get(obj, len, NULL, "minute", minute, sizeof(minute));
    obj_get(obj, len, NULL, "match_timestamp", ts, sizeof(ts));
    obj_get(obj, len, "competition", "name", out->league, sizeof(out->league));
    obj_get(obj, len, "team_A", "name", out->home, sizeof(out->home));
    obj_get(obj, len, "team_B", "name", out->away, sizeof(out->away));
    obj_get(obj, len, "team_A", "fs", score_a, sizeof(score_a));
    obj_get(obj, len, "team_B", "fs", score_b, sizeof(score_b));

    copy_raw(out->id, sizeof(out->id), id, strlen(id));
    out->kickoff_utc = (int64_t)strtoll(ts, NULL, 10);
    out->state = app_state_from_status(status, period);
    out->minute = (uint8_t)parse_int_or(minute, 0);
    if (score_a[0] != '\0') out->home_score = (int8_t)parse_int_or(score_a, -1);
    if (score_b[0] != '\0') out->away_score = (int8_t)parse_int_or(score_b, -1);

    // 名字缺失的条目直接丢掉,界面上没法显示。
    if (out->home[0] == '\0' || out->away[0] == '\0') return false;
    return true;
}

// 决定这条比赛值不值得留下,并写进列表。
//
// 三道筛子,顺序从便宜到贵:
//   1. 联赛:只看五大联赛 + 中超 + 中甲,1.6 MB 里绝大多数条目死在这一步;
//   2. 日期:只看今天(北京时间)的,直播中的例外(跨零点的那几场仍在踢);
//   3. 分类上限:直播 / 未开始 / 完场各自封顶,合计不超过列表容量。
static void feed_accept(app_feed_t *feed, const app_match_t *match) {
    if (!app_league_allowed(match->league)) {
        feed->dropped_league++;
        return;
    }

    const bool filter_day = feed->now_utc > 0;
    const bool on_target_day =
        !filter_day || app_match_day_offset(match, feed->now_utc) == feed->target_day;

    // 目标日不是今天时(无比赛日要显示下一个比赛日),整份响应都是那一天,
    // 不在目标日的直接丢。今天这一档反过来——直播中的比赛豁免,因为
    // 23:30 开球、跨过零点还在踢的那几场正是此刻最该看的。
    if (filter_day && feed->target_day > 0 && !on_target_day) {
        feed->dropped_day++;
        return;
    }

    if (app_match_is_live(match->state)) {
        if (feed->live_kept >= feed->live_cap) return;
        feed->live_kept++;
    } else if (match->state == APP_STATE_FIXTURE) {
        if (!on_target_day) {
            feed->dropped_day++;
            return;
        }
        // 只有看"今天"时才需要排掉"已过开赛时间却仍标着未开始"的滞后数据;
        // 看未来某一天时,所有比赛都还没开球,这条判据会把它们全砍掉。
        if (filter_day && feed->target_day == 0 && match->kickoff_utc <= feed->now_utc) {
            return;
        }
        if (feed->upcoming_kept >= feed->upcoming_cap) return;
        feed->upcoming_kept++;
    } else if (match->state == APP_STATE_FULL_TIME) {
        // 当天已完场的也留着:一天看下来,前面的比分同样是信息。
        if (!on_target_day) {
            feed->dropped_day++;
            return;
        }
        if (feed->finished_kept >= feed->finished_cap) return;
        feed->finished_kept++;
    } else {
        return;   // 推迟/取消/待定:看板不显示
    }
    (void)app_match_list_upsert(feed->out, match);
}

// 按目标日分配三类名额,三者之和恒等于 APP_MAX_MATCHES。
//
//   今天(0)   —— 直播 6 / 未开始 20 / 完场 14。完场必须给到十几场:
//                欧冠一个比赛日就有 9~10 场落在凌晨,按旧的 4 场上限,
//                用户白天打开只看得到 4 场,这就是"凌晨的比赛没显示"的第二层原因。
//   未来日(>0) —— 整份响应都是那一天且全是未开始,名额全给它。
//   前日(<0)   —— 全是完场,直播和未开始一个都不要(昨天的比赛不可能还在踢,
//                万一上游数据脏了给了个 Playing,也不该混进回看列表)。
#define TODAY_LIVE_CAP 6
#define TODAY_UPCOMING_CAP 20
#define TODAY_FINISHED_CAP (APP_MAX_MATCHES - TODAY_LIVE_CAP - TODAY_UPCOMING_CAP)

static void feed_apply_day_caps(app_feed_t *feed, int target_day) {
    if (target_day > 0) {
        feed->live_cap = 0;
        feed->upcoming_cap = APP_MAX_MATCHES;
        feed->finished_cap = 0;
    } else if (target_day < 0) {
        feed->live_cap = 0;
        feed->upcoming_cap = 0;
        feed->finished_cap = APP_MAX_MATCHES;
    } else {
        feed->live_cap = TODAY_LIVE_CAP;
        feed->upcoming_cap = TODAY_UPCOMING_CAP;
        feed->finished_cap = TODAY_FINISHED_CAP;
    }
}

void app_feed_init(app_feed_t *feed, app_match_list_t *out, int64_t now_utc) {
    if (feed == NULL) return;
    memset(feed, 0, sizeof(*feed));
    feed->out = out;
    feed->now_utc = now_utc;
    feed->target_day = 0;   // 默认看今天;拉未来赛程/回看前日时再改
    feed_apply_day_caps(feed, 0);
    app_match_list_reset(out);
}

void app_feed_set_day(app_feed_t *feed, int target_day) {
    if (feed == NULL) return;
    feed->target_day = target_day;
    feed_apply_day_caps(feed, target_day);
}

void app_feed_init_append(app_feed_t *feed, app_match_list_t *out, int64_t now_utc) {
    if (feed == NULL) return;
    memset(feed, 0, sizeof(*feed));
    feed->out = out;
    feed->now_utc = now_utc;
    feed->target_day = 0;
    // 只补完场。直播和未开始 match_list 那份已经拿到了,这里再来一遍只会
    // 把完场的名额抢走。
    feed->live_cap = 0;
    feed->upcoming_cap = 0;
    feed->finished_cap = APP_MAX_MATCHES;
    // 不清空 out:调用方要的正是"并进已有列表"。
}

static void feed_start_object(app_feed_t *feed) {
    feed->depth = 1;
    feed->obj_len = 0;
    feed->overflow = false;
    feed->in_string = false;
    feed->escaped = false;
    feed->obj[0] = '\0';
}

static void feed_close_object(app_feed_t *feed) {
    feed->depth = 0;
    if (feed->overflow) return;

    app_match_t match;
    if (app_parse_match_object(feed->obj, feed->obj_len, &match)) {
        feed->objects_seen++;
        feed_accept(feed, &match);
    }
}

static void feed_byte(app_feed_t *feed, char c) {
    if (!feed->in_array) {
        // 找 "matches" 关键字(带引号),找到后等 '[' 出现。
        static const char KEY[] = "\"matches\"";
        const int key_len = (int)sizeof(KEY) - 1;
        if (c == KEY[feed->key_hits]) {
            feed->key_hits++;
            if (feed->key_hits == key_len) {
                feed->in_array = true;
                feed->key_hits = 0;
                feed->depth = 0;
            }
        } else {
            feed->key_hits = (c == '"') ? 1 : 0;
        }
        return;
    }

    if (feed->depth == 0) {
        if (c == '{') {
            feed_start_object(feed);
            feed->obj[feed->obj_len++] = c;
        }
        // 其它字符(逗号、空白、']')忽略
        return;
    }

    if (feed->obj_len + 1 < APP_JSON_OBJ_MAX) {
        feed->obj[feed->obj_len++] = c;
        feed->obj[feed->obj_len] = '\0';
    } else {
        feed->overflow = true;
    }

    if (feed->in_string) {
        if (feed->escaped) {
            feed->escaped = false;
        } else if (c == '\\') {
            feed->escaped = true;
        } else if (c == '"') {
            feed->in_string = false;
        }
        return;
    }

    if (c == '"') {
        feed->in_string = true;
    } else if (c == '{' || c == '[') {
        feed->depth++;
    } else if (c == '}' || c == ']') {
        feed->depth--;
        if (feed->depth == 0) feed_close_object(feed);
    }
}

void app_feed_write(app_feed_t *feed, const char *data, size_t len) {
    if (feed == NULL || data == NULL) return;
    for (size_t i = 0; i < len; i++) {
        feed_byte(feed, data[i]);
    }
}

void app_feed_finish(app_feed_t *feed) {
    if (feed == NULL) return;
    // 响应被截断时,最后那个对象大概率是半截的,直接丢弃。
    feed->depth = 0;
    feed->in_array = false;
}
