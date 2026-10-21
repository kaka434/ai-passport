// main/app_model.h —— 比赛数据模型(纯逻辑,可在主机上测试)。
//
// 数据源是懂球帝(magicball/v1/list/match_list),队名与联赛名本身就是中文,
// 所以这里不再需要"英文→中文"映射表:字符串原样存下来直接显示。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_ID_LEN 16
// 队名/联赛名按 UTF-8 存。40 字节够放 13 个汉字,懂球帝最长的名字也在这之内。
#define APP_NAME_LEN 40

// 列表容量。看板只看当天:直播中 + 今天还没开始的 + 今天已完场的,
// 24 条够放下一个周末比赛日的主力场次,又不至于翻太多屏。
#define APP_MAX_MATCHES 24

// 比赛状态。懂球帝给的是 status(Fixture/Playing/Played/...)加 period(1H/2H/HT/FT),
// 这里合并成界面需要的几档。
typedef enum {
    APP_STATE_FIXTURE = 0,   // 未开始
    APP_STATE_FIRST_HALF,    // 上半场
    APP_STATE_HALF_TIME,     // 中场
    APP_STATE_SECOND_HALF,   // 下半场
    APP_STATE_LIVE_OTHER,    // 直播中,但没给具体阶段
    APP_STATE_FULL_TIME,     // 完场
    APP_STATE_POSTPONED,     // 推迟
    APP_STATE_CANCELLED,     // 取消
    APP_STATE_UNKNOWN,       // 待定
} app_match_state_t;

typedef struct {
    char id[APP_ID_LEN];
    char league[APP_NAME_LEN];   // 联赛中文名,如"英超"
    char home[APP_NAME_LEN];     // 主队中文名
    char away[APP_NAME_LEN];     // 客队中文名
    int64_t kickoff_utc;         // UTC 秒;0 表示接口没给
    int8_t home_score;           // -1 = 还没有比分
    int8_t away_score;
    uint8_t minute;              // 直播分钟数,0 = 未知
    uint8_t state;               // app_match_state_t
} app_match_t;

typedef struct {
    app_match_t items[APP_MAX_MATCHES];
    size_t count;
} app_match_list_t;

void app_match_list_reset(app_match_list_t *list);

// 按 id 去重写入:已存在则整条替换(比分会被刷新),否则追加。
// 返回写入的下标,列表已满返回 -1。
int app_match_list_upsert(app_match_list_t *list, const app_match_t *match);

// 直播中的排最前,其余按开赛时间;同时间用 id 兜底,保证两次刷新顺序稳定。
void app_match_list_sort(app_match_list_t *list);

bool app_match_is_live(uint8_t state);

// 只保留这几项赛事:英超、西甲、意甲、德甲、法甲、中超、中甲。
// 匹配是"去掉尾部'联赛'后全等",所以"英超"和"英超联赛"都算,
// 而"巴西甲级联赛"不会被当成西甲。
bool app_league_allowed(const char *league);

// 这场比赛是否属于"今天"(按北京时间)。now_utc <= 0 时不做过滤,返回 true。
bool app_match_is_today(const app_match_t *match, int64_t now_utc);

// 把接口的 status/period 折成 app_match_state_t。
uint8_t app_state_from_status(const char *status, const char *period);

// 状态文案:"上半场 32'" / "中场" / "完场" / "20:30"(未开始显示开赛时间)。
void app_match_state_label(const app_match_t *match, char *buf, size_t cap);

// 同上,但未开始的比赛会带上日期前缀:"今天 20:30"/"明天 03:00"/"10-20 20:30"。
// now_utc 用来判断今天还是明天;传 0 等价于 app_match_state_label。
// 看板现在只显示当天的比赛,列表里用不带前缀的版本;将来放宽日期筛选时改回这个。
void app_match_state_label_at(const app_match_t *match, int64_t now_utc, char *buf, size_t cap);

// 比分文案:"2-1";没有比分时给 "vs"。
void app_match_score_text(const app_match_t *match, char *buf, size_t cap);

// to 比 from 晚几个自然日(按北京时间算):0=同一天,1=明天,-1=昨天。
// 任一方无效时返回 0。
int app_day_offset(int64_t from_utc, int64_t to_utc);

// 这场比赛离 now_utc 那天有几个自然日(按北京时间):0=今天,1=明天。
// 开赛时间缺失时返回 0,当作"今天",让它走正常过滤而不是被当成未来。
int app_match_day_offset(const app_match_t *match, int64_t now_utc);

// 北京时间"今天 00:00"再往后 days 天的那个瞬间,返回 UTC 秒。
// 没有比赛的日子要按天去拉赛程,靠它算出目标日的边界。
int64_t app_cst_day_start(int64_t now_utc, int days);

// 把"北京时间某天 00:00"的 UTC 秒换算成 schedule_list 接口的 start 参数:
// "YYYY-MM-DD HH:00:00"(UTC)。例:2026-10-22 00:00 CST → "2026-10-21 16:00:00"。
void app_schedule_start_text(int64_t cst_day_start_utc, char *buf, size_t cap);

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int weekday;   // 0 = 周日
} app_datetime_t;

// UTC 秒 → 北京时间(UTC+8)。不用 localtime,主机测试与固件结果一致。
bool app_datetime_from_utc(int64_t utc, app_datetime_t *out);

void app_format_kickoff(int64_t kickoff_utc, char *buf, size_t cap);   // "10-18 20:30"
void app_format_clock(int64_t kickoff_utc, char *buf, size_t cap);     // "20:30"

// UTF-8 安全截断:保证不会把一个多字节字符劈成两半。
size_t app_utf8_truncate(char *text, size_t cap);

// 显示宽度:ASCII 记 1,其它记 2(汉字/全角标点)。用于判断一行放不放得下。
size_t app_text_width(const char *text);
