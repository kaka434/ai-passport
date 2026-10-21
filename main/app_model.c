// main/app_model.c
#include "app_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 设备固定使用中国标准时间;不用 localtime,主机测试与固件行为一致。
#define APP_TZ_OFFSET_SECONDS (8 * 3600)

static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void app_match_list_reset(app_match_list_t *list) {
    if (list == NULL) return;
    list->count = 0;
}

int app_match_list_upsert(app_match_list_t *list, const app_match_t *match) {
    if (list == NULL || match == NULL) return -1;
    if (match->id[0] == '\0') return -1;

    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].id, match->id) == 0) {
            list->items[i] = *match;
            return (int)i;
        }
    }
    if (list->count >= APP_MAX_MATCHES) return -1;
    list->items[list->count] = *match;
    return (int)list->count++;
}

bool app_match_is_live(uint8_t state) {
    switch (state) {
        case APP_STATE_FIRST_HALF:
        case APP_STATE_HALF_TIME:
        case APP_STATE_SECOND_HALF:
        case APP_STATE_LIVE_OTHER:
            return true;
        default:
            return false;
    }
}

// 只关心这七项赛事。中文联赛名不会互相包含,所以全等比较就够;
// 但接口有时给"英超联赛"这种带后缀的写法,统一先把尾部"联赛"去掉。
static const char *const ALLOWED_LEAGUES[] = {
    "英超", "西甲", "意甲", "德甲", "法甲", "中超", "中甲",
};
#define ALLOWED_LEAGUE_COUNT (sizeof(ALLOWED_LEAGUES) / sizeof(ALLOWED_LEAGUES[0]))

static bool name_equals(const char *text, size_t len, const char *want) {
    const size_t want_len = strlen(want);
    return len == want_len && memcmp(text, want, len) == 0;
}

bool app_league_allowed(const char *league) {
    if (league == NULL) return false;

    size_t len = strlen(league);
    static const char SUFFIX[] = "联赛";
    const size_t suffix_len = sizeof(SUFFIX) - 1;
    if (len > suffix_len && memcmp(league + len - suffix_len, SUFFIX, suffix_len) == 0) {
        len -= suffix_len;
    }

    for (size_t i = 0; i < ALLOWED_LEAGUE_COUNT; i++) {
        if (name_equals(league, len, ALLOWED_LEAGUES[i])) return true;
    }
    return false;
}

bool app_match_is_today(const app_match_t *match, int64_t now_utc) {
    if (match == NULL) return false;
    // 还没校时:没有"今天"这个概念,不要拿 0 去比较,一律放行。
    if (now_utc <= 0) return true;
    if (match->kickoff_utc <= 0) return false;
    return app_day_offset(now_utc, match->kickoff_utc) == 0;
}

uint8_t app_state_from_status(const char *status, const char *period) {
    if (status == NULL) return APP_STATE_UNKNOWN;

    if (strcmp(status, "Playing") == 0) {
        if (period != NULL) {
            if (strcmp(period, "1H") == 0) return APP_STATE_FIRST_HALF;
            if (strcmp(period, "2H") == 0) return APP_STATE_SECOND_HALF;
            if (strcmp(period, "HT") == 0) return APP_STATE_HALF_TIME;
        }
        return APP_STATE_LIVE_OTHER;
    }
    if (strcmp(status, "Played") == 0) return APP_STATE_FULL_TIME;
    if (strcmp(status, "Fixture") == 0) return APP_STATE_FIXTURE;
    if (strcmp(status, "Postponed") == 0) return APP_STATE_POSTPONED;
    if (strcmp(status, "Cancelled") == 0) return APP_STATE_CANCELLED;
    return APP_STATE_UNKNOWN;
}

// 排序优先级:直播中 → 未开始 → 推迟/取消/待定 → 完场。
// 完场排在最后,因为看板主要看"正在发生"和"即将开始"。
static int match_rank(const app_match_t *m) {
    if (app_match_is_live(m->state)) return 0;
    if (m->state == APP_STATE_FIXTURE) return 1;
    if (m->state == APP_STATE_POSTPONED || m->state == APP_STATE_CANCELLED
        || m->state == APP_STATE_UNKNOWN) {
        return 2;
    }
    return 3;   // 完场
}

static int compare_matches(const void *lhs, const void *rhs) {
    const app_match_t *a = (const app_match_t *)lhs;
    const app_match_t *b = (const app_match_t *)rhs;

    const int ra = match_rank(a);
    const int rb = match_rank(b);
    if (ra != rb) return ra - rb;

    if (a->kickoff_utc != b->kickoff_utc) {
        return a->kickoff_utc < b->kickoff_utc ? -1 : 1;
    }
    return strcmp(a->id, b->id);
}

void app_match_list_sort(app_match_list_t *list) {
    if (list == NULL || list->count < 2) return;
    qsort(list->items, list->count, sizeof(list->items[0]), compare_matches);
}

void app_match_state_label(const app_match_t *match, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    if (match == NULL) {
        copy_str(buf, cap, "--");
        return;
    }

    switch (match->state) {
        case APP_STATE_FIRST_HALF:
            if (match->minute > 0) {
                snprintf(buf, cap, "上半场 %u'", (unsigned)match->minute);
            } else {
                copy_str(buf, cap, "上半场");
            }
            return;
        case APP_STATE_SECOND_HALF:
            if (match->minute > 0) {
                snprintf(buf, cap, "下半场 %u'", (unsigned)match->minute);
            } else {
                copy_str(buf, cap, "下半场");
            }
            return;
        case APP_STATE_HALF_TIME:   copy_str(buf, cap, "中场"); return;
        case APP_STATE_LIVE_OTHER:  copy_str(buf, cap, "直播中"); return;
        case APP_STATE_FULL_TIME:   copy_str(buf, cap, "完场"); return;
        case APP_STATE_POSTPONED:   copy_str(buf, cap, "推迟"); return;
        case APP_STATE_CANCELLED:   copy_str(buf, cap, "取消"); return;
        case APP_STATE_UNKNOWN:     copy_str(buf, cap, "待定"); return;
        case APP_STATE_FIXTURE:
        default:
            // 未开始就直接显示开赛时间,比"未开始"三个字有用。
            app_format_clock(match->kickoff_utc, buf, cap);
            return;
    }
}

void app_match_state_label_at(const app_match_t *match, int64_t now_utc, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    if (match == NULL || now_utc <= 0) {
        app_match_state_label(match, buf, cap);
        return;
    }
    if (match->state != APP_STATE_FIXTURE) {
        app_match_state_label(match, buf, cap);
        return;
    }

    char clock[8];
    app_format_clock(match->kickoff_utc, clock, sizeof(clock));

    switch (app_day_offset(now_utc, match->kickoff_utc)) {
        case 0:  snprintf(buf, cap, "今天 %s", clock); return;
        case 1:  snprintf(buf, cap, "明天 %s", clock); return;
        case 2:  snprintf(buf, cap, "后天 %s", clock); return;
        default: break;
    }

    // 更远的日子直接给日期,比"3 天后"更明确。
    char full[16];
    app_format_kickoff(match->kickoff_utc, full, sizeof(full));
    snprintf(buf, cap, "%s", full);
}

void app_match_score_text(const app_match_t *match, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    if (match == NULL || match->home_score < 0 || match->away_score < 0) {
        copy_str(buf, cap, "vs");
        return;
    }
    snprintf(buf, cap, "%d-%d", (int)match->home_score, (int)match->away_score);
}

// Howard Hinnant 的 days_from_civil / civil_from_days:公历日期与 1970-01-01
// 起的天数互转。用它而不是 mktime,是为了不受主机时区与夏令时影响。
static int64_t days_from_civil(int year, unsigned month, unsigned day) {
    int64_t y = year;
    y -= (month <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                       // [0, 399]
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day) {
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
    y += (m <= 2u) ? 1 : 0;
    *year = (int)y;
    *month = m;
    *day = d;
}

bool app_datetime_from_utc(int64_t utc, app_datetime_t *out) {
    if (out == NULL || utc <= 0) return false;

    const int64_t local = utc + APP_TZ_OFFSET_SECONDS;
    int64_t days = local / 86400;
    int64_t secs = local % 86400;
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, &year, &month, &day);

    out->year = year;
    out->month = (int)month;
    out->day = (int)day;
    out->hour = (int)(secs / 3600);
    out->minute = (int)((secs % 3600) / 60);
    // 1970-01-01 是周四,所以 (days + 4) % 7 得到 0=周日。
    int64_t weekday = (days + 4) % 7;
    if (weekday < 0) weekday += 7;
    out->weekday = (int)weekday;
    return true;
}

void app_format_kickoff(int64_t kickoff_utc, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    app_datetime_t dt;
    if (!app_datetime_from_utc(kickoff_utc, &dt)) {
        copy_str(buf, cap, "--");
        return;
    }
    snprintf(buf, cap, "%02d-%02d %02d:%02d", dt.month, dt.day, dt.hour, dt.minute);
}

void app_format_clock(int64_t kickoff_utc, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;
    app_datetime_t dt;
    if (!app_datetime_from_utc(kickoff_utc, &dt)) {
        copy_str(buf, cap, "--");
        return;
    }
    snprintf(buf, cap, "%02d:%02d", dt.hour, dt.minute);
}

int app_day_offset(int64_t from_utc, int64_t to_utc) {
    app_datetime_t a;
    app_datetime_t b;
    if (!app_datetime_from_utc(from_utc, &a)) return 0;
    if (!app_datetime_from_utc(to_utc, &b)) return 0;

    const int64_t days_a = days_from_civil(a.year, (unsigned)a.month, (unsigned)a.day);
    const int64_t days_b = days_from_civil(b.year, (unsigned)b.month, (unsigned)b.day);
    const int64_t diff = days_b - days_a;
    // 窗口只有几十小时,不会溢出 int;这里仍做一次夹取,防止脏数据把它撑爆。
    if (diff > 9999) return 9999;
    if (diff < -9999) return -9999;
    return (int)diff;
}

int app_match_day_offset(const app_match_t *match, int64_t now_utc) {
    if (match == NULL) return 0;
    if (match->kickoff_utc <= 0) return 0;
    return app_day_offset(now_utc, match->kickoff_utc);
}

int64_t app_cst_day_start(int64_t now_utc, int days) {
    if (now_utc <= 0) return 0;
    // 先挪到北京时间的"日内秒",减掉它就是这个自然日的 00:00(CST),
    // 再减 8 小时换回 UTC。
    const int64_t local = now_utc + APP_TZ_OFFSET_SECONDS;
    const int64_t day_start_local = local - (local % 86400);
    return day_start_local - APP_TZ_OFFSET_SECONDS + (int64_t)days * 86400;
}

void app_schedule_start_text(int64_t cst_day_start_utc, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return;

    // 接口要的是 UTC,而 app_datetime_from_utc 给的是北京时间,所以这里
    // 先按 UTC 自己拆一次:把秒数直接当成"不带时区的本地时间"处理。
    int64_t secs_total = cst_day_start_utc;
    int64_t days = secs_total / 86400;
    int64_t secs = secs_total % 86400;
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, &year, &month, &day);

    snprintf(buf, cap, "%04d-%02u-%02u %02d:00:00", year, month, day, (int)(secs / 3600));
}

// 判断某个字节是不是 UTF-8 的续字节(10xxxxxx)。截断时只能停在非续字节处。
static bool is_utf8_continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

size_t app_utf8_truncate(char *text, size_t cap) {
    if (text == NULL || cap == 0) return 0;

    // 先找 NUL。手写循环而不是 strnlen:后者是 POSIX,主机用 -std=c11 编译时
    // 可能没有声明,固件与主机测试要保持同一套代码。
    size_t len = 0;
    while (len < cap && text[len] != '\0') len++;
    if (len < cap) return len;

    // 没有 NUL 可用:往回退到字符边界,再补 '\0'。
    size_t cut = cap - 1;
    while (cut > 0 && is_utf8_continuation(text[cut])) cut--;
    text[cut] = '\0';
    return cut;
}

size_t app_text_width(const char *text) {
    if (text == NULL) return 0;
    size_t width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (is_utf8_continuation((char)*p)) continue;
        width += (*p < 0x80) ? 1u : 2u;
    }
    return width;
}
