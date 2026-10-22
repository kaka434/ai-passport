// main/app_json.h —— 懂球帝比分接口的流式解析。
//
// 接口一次返回全部足球比赛(实测 1.6 MB),C3 上没有 PSRAM,不可能整段缓冲。
// 所以这里不"收完再解析",而是边收边扫:只把数组里的单个比赛对象(约 1.2 KB)
// 攒进一个固定缓冲,解析完就丢掉,内存占用与响应大小无关。
#pragma once

#include "app_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 单个比赛对象的缓冲上限。实测最大的对象约 1.3 KB(含 logo URL),2.5 KB 留了
// 近一倍余量。这个缓冲是联网任务栈上的局部变量,给大了会挤掉 TLS 握手要用的栈。
#define APP_JSON_OBJ_MAX 2560

typedef struct {
    app_match_list_t *out;
    int64_t now_utc;        // 用来判断"今天"和"开赛时间已过"
    // 分类上限之和必须 == APP_MAX_MATCHES,否则先收进来的完场会把
    // 后面的未开始比赛挤出列表(流式解析没法回头腾位置)。
    // 具体怎么分由 app_feed_init() / app_feed_set_day() 按目标日决定。
    int live_cap;
    int upcoming_cap;
    int finished_cap;
    int live_kept;
    int upcoming_kept;
    int finished_kept;
    // 只留哪一天的比赛:0 = 今天,-1 = 昨天(回看),1..7 = 未来第 N 天。
    // now_utc 为 0 时不起作用。
    int target_day;
    // 被过滤掉的数量,只用于日志:联赛名对不上时一眼能看出来。
    size_t dropped_league;
    size_t dropped_day;
    size_t objects_seen;    // 解析过的对象数,便于日志

    // ---- 扫描状态 ----
    bool in_array;          // 已定位到 "matches":[ 之后
    int depth;              // 当前对象内嵌深度,0 = 位于数组元素之间
    bool in_string;
    bool escaped;
    bool overflow;          // 单个对象超出缓冲,丢弃这个对象
    size_t obj_len;
    char obj[APP_JSON_OBJ_MAX];
    // 跨数据块匹配 "matches" 关键字
    int key_hits;
} app_feed_t;

// 开始解析。now_utc 为 0 时不做时间窗口过滤(主机测试方便)。
// 分类上限按"今天"分档,用完再改目标日的话要跟一次 app_feed_set_day()。
void app_feed_init(app_feed_t *feed, app_match_list_t *out, int64_t now_utc);

// 改目标日,并按目标日重算三类名额(三者之和恒为 APP_MAX_MATCHES)。
// 必须在 app_feed_write() 之前调用:名额是按"流式先到先得"扣的,喂过数据
// 再改就没意义了。
void app_feed_set_day(app_feed_t *feed, int target_day);

// 往一份**已经填过**的列表里补充历史比赛:不清空 out,只收已完场的。
//
// 用来把 schedule_list 的"凌晨段"并进 match_list 的结果——两份响应的窗口
// 有重叠,靠 app_match_list_upsert 按 id 去重,这里不需要再判重。
// 直播和未开始的名额都设成 0:那些 match_list 已经拿到了,这里再来一遍只会
// 把完场的名额抢走。
void app_feed_init_append(app_feed_t *feed, app_match_list_t *out, int64_t now_utc);

// 喂入一段原始 HTTP 响应。可以任意切块,不需要按行/按对象对齐。
void app_feed_write(app_feed_t *feed, const char *data, size_t len);

// 结束:丢弃没写完的半个对象。
void app_feed_finish(app_feed_t *feed);

// 把一个完整的比赛对象文本解析成 app_match_t。返回是否成功。
// 单独暴露出来是为了主机测试能直接喂对象文本。
bool app_parse_match_object(const char *obj, size_t len, app_match_t *out);
