// main/app_detail.h —— 单场比赛的赛况数据(进球时间轴 + 阵型 + 技术统计)。
//
// 数据源:https://api.dongdianqiu.com/data/overview/match/{match_id}
// 一次 6~37 KB。**必须流式解析**:C3 跑起来空闲堆只有 45 KB,TLS 还要占 20 KB,
// 放不下一整段 37 KB 的响应(实测最大 36.7 KB)。解析器是纯顺序扫描,所以底下
// 挂一个 1 KB 的可重填窗口就够,内存占用与响应大小无关。
//
// 接口给的东西与看板需要的东西的对应关系:
//   events.{分钟}.teamAEvents[] / teamBEvents[]   进球、黄牌、换人…
//      code: G=进球 PG=点球 OG/UG=乌龙 YC=黄牌 RC=红牌 SI/SO=换人 AS=助攻
//      分钟不在事件里,而在外层对象的键上("35"、"90+2")
//   statistics.team_A.formation / team_B.formation  阵型,很多比赛是空串
//   statistics.list[]                               控球率、射门等,带两队数值
//
// 注意 events / statistics 这两项的类型不固定:有数据时是对象 {},"还没有数据"
// (未开赛、延期、统计未出)时是空数组 [] 或 null。实测 187 字节的未开赛响应
// 就是 {"events":[],"statistics":[]} —— 只认对象会把整份响应判成解析失败。
//
// 球场名与天气不在这份响应里,看板不显示(宁可少一块信息,也不编)。
#pragma once

#include "app_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一场比赛的进球很少超过两位数;16 条足够,再多也翻不完。
#define APP_GOAL_MAX 16
// 懂球帝的技术统计固定 8 项(控球率/角球/射门/射正/进攻/危险进攻/黄牌/红牌)。
#define APP_STAT_MAX 8
// 球员名 24 字节 = 8 个汉字,够放"亚历山大-洛佩斯"这类音译名。
#define APP_PLAYER_LEN 24
#define APP_STAT_LABEL_LEN 20
// "4-3-3" 这种字符串;留宽一点,防止接口给 "4-2-3-1" 之外的花样。
#define APP_FORMATION_LEN 12

typedef enum {
    APP_GOAL_NONE = 0,   // 不是进球(黄牌、换人…),解析时用来过滤
    APP_GOAL_NORMAL,
    APP_GOAL_PENALTY,    // 点球
    APP_GOAL_OWN,        // 乌龙
} app_goal_kind_t;

typedef struct {
    uint8_t minute;   // 0~130
    uint8_t extra;    // 补时分钟,0 = 没有(45+2' 里的 2)
    uint8_t kind;     // app_goal_kind_t
    uint8_t side;     // 0 = 主队,1 = 客队
    char player[APP_PLAYER_LEN];
    char score[8];    // 进球后的比分,如 "2-0";接口没给时是空串
} app_goal_t;

typedef struct {
    char label[APP_STAT_LABEL_LEN];   // "控球率"
    int16_t home;
    int16_t away;
} app_stat_t;

typedef struct {
    uint8_t state;   // app_match_state_t,来自响应里的 match_status
    char formation_home[APP_FORMATION_LEN];
    char formation_away[APP_FORMATION_LEN];
    app_goal_t goals[APP_GOAL_MAX];
    size_t goal_count;
    app_stat_t stats[APP_STAT_MAX];
    size_t stat_count;
} app_detail_t;

// 正文来源:把最多 cap 字节读进 buf。返回 >0 字节数,0 = 正常结束,<0 = 出错。
// 解析器只在需要时调用,不要求整段响应常驻内存。
typedef int (*app_detail_read_fn)(void *ctx, char *buf, size_t cap);

void app_detail_reset(app_detail_t *detail);

// 流式解析 /data/overview/match/{id} 的响应。成功返回 true。
// 解析失败时 out 里可能已经有部分结果,调用方按失败处理即可。
bool app_detail_parse_stream(app_detail_read_fn read, void *ctx, app_detail_t *out);

// 内存版:整段 JSON 已经在手里时用(主机测试走这条)。
bool app_detail_parse(const char *json, size_t len, app_detail_t *out);
