#include "app_standings.h"
#include "app_net.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "app_standings";

// 懂球帝官方精确 season_id 映射
const app_league_info_t APP_LEAGUES[APP_LEAGUES_COUNT] = {
    {"英超", 27502},
    {"西甲", 27616},
    {"意甲", 27361},
    {"德甲", 27622},
    {"法甲", 27400},
    {"中超", 26322},
    {"欧冠", 27456},
    {"中甲", 26350},
};

static app_standings_snapshot_t s_snap;
static SemaphoreHandle_t s_lock;
static uint32_t s_revision = 1;

static uint32_t s_req_season_id = 0;
static char s_req_title[16] = {0};
static bool s_has_req = false;
static TaskHandle_t s_task_handle = NULL;

void app_standings_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        memset(&s_snap, 0, sizeof(s_snap));
        s_snap.season_id = APP_LEAGUES[0].season_id;
        snprintf(s_snap.title, sizeof(s_snap.title), "%s", APP_LEAGUES[0].name);
    }
}

void app_standings_snapshot_copy(app_standings_snapshot_t *dst) {
    if (dst == NULL) return;
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(dst, &s_snap, sizeof(app_standings_snapshot_t));
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

uint32_t app_standings_revision(void) {
    return s_revision;
}

static bool extract_json_str(const char *json, const char *key, char *out, size_t max_len) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        snprintf(search, sizeof(search), "\"%s\": \"", key);
        p = strstr(json, search);
    }
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < max_len) {
        if (*p == '\\' && *(p+1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static int extract_json_int(const char *json, const char *key, int def_val) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def_val;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\"') p++;
    return atoi(p);
}

static char s_item_buf[768];

static void parse_standings_buffer(const char *buf) {
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.team_count = 0;

    const char *p = strstr(buf, "\"data\":[");
    if (!p) p = strstr(buf, "\"data\": [");

    if (p) {
        p = strchr(p, '[');
        if (p) p++;

        while (p && *p && s_snap.team_count < APP_STANDINGS_MAX_TEAMS) {
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' || *p == ',') p++;
            if (*p == ']' || *p == '\0') break; // 数组边界

            const char *obj_start = strchr(p, '{');
            if (!obj_start) break;

            // 括号深度计数,防止嵌套对象(例如中超的扣分说明 instruction)提前打断
            int depth = 0;
            bool in_str = false;
            const char *obj_end = NULL;
            const char *q = obj_start;

            while (*q) {
                if (*q == '\"' && (q == obj_start || *(q - 1) != '\\')) {
                    in_str = !in_str;
                } else if (!in_str) {
                    if (*q == '{') {
                        depth++;
                    } else if (*q == '}') {
                        depth--;
                        if (depth == 0) {
                            obj_end = q;
                            break;
                        }
                    }
                }
                q++;
            }

            if (!obj_end) break;

            size_t item_len = obj_end - obj_start + 1;
            if (item_len >= sizeof(s_item_buf)) item_len = sizeof(s_item_buf) - 1;
            memcpy(s_item_buf, obj_start, item_len);
            s_item_buf[item_len] = '\0';

            app_standing_team_t *t = &s_snap.teams[s_snap.team_count];
            memset(t, 0, sizeof(*t));

            t->rank = (uint8_t)extract_json_int(s_item_buf, "rank", s_snap.team_count + 1);
            t->team_id = (uint32_t)extract_json_int(s_item_buf, "team_id", 0);
            extract_json_str(s_item_buf, "team_name", t->team_name, sizeof(t->team_name));
            t->played = (uint8_t)extract_json_int(s_item_buf, "matches_total", 0);
            t->won = (uint8_t)extract_json_int(s_item_buf, "matches_won", 0);
            t->draw = (uint8_t)extract_json_int(s_item_buf, "matches_draw", 0);
            t->lost = (uint8_t)extract_json_int(s_item_buf, "matches_lost", 0);
            t->points = (uint8_t)extract_json_int(s_item_buf, "points", 0);
            int diff = extract_json_int(s_item_buf, "goals_diff", 0);
            if (diff == 0) {
                diff = extract_json_int(s_item_buf, "goals_pro", 0) - extract_json_int(s_item_buf, "goals_against", 0);
            }
            t->gd = (int16_t)diff;

            s_snap.team_count++;
            p = obj_end + 1;
        }
    }

    s_snap.loading = false;
    s_snap.error = (s_snap.team_count == 0);
    s_snap.updated_at = esp_timer_get_time() / 1000000;
    s_revision++;
    if (s_lock != NULL) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "解析完成: 联赛 %s, 球队数 %d", s_snap.title, s_snap.team_count);
}

// 接收缓冲: 12288 字节(12KB)。五大联赛及中超中甲全文约 9-10KB 完全容纳,欧冠容纳前 26 名出线区。
// 动态申请并用完即释放,绝不常驻内存!
#define STANDINGS_BUF_SIZE 12288

// 任务栈: 4096 字节
#define STANDINGS_STACK_SIZE 4096
static StackType_t s_standings_stack[STANDINGS_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_standings_tcb;

static void standings_task(void *arg) {
    while (1) {
        if (!s_has_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t sid = s_req_season_id;
        char title[16];
        snprintf(title, sizeof(title), "%s", s_req_title);
        s_has_req = false;

        if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.season_id = sid;
        snprintf(s_snap.title, sizeof(s_snap.title), "%s", title);
        s_snap.loading = true;
        s_snap.error = false;
        s_revision++;
        if (s_lock != NULL) xSemaphoreGive(s_lock);

        char url[160];
        snprintf(url, sizeof(url), "http://sport-data.dongqiudi.com/soccer/biz/data/standing?version=0&platform=iphone&season_id=%lu", (unsigned long)sid);
        ESP_LOGI(TAG, "拉取联赛积分榜: %s (%s)", title, url);

        char *http_buf = NULL;
        int got = app_net_fetch_alloc(url, "https://www.dongqiudi.com/data",
                                      &http_buf, STANDINGS_BUF_SIZE);
        if (got > 0 && http_buf != NULL) {
            ESP_LOGI(TAG, "积分榜数据获取成功: %d 字节", got);
            parse_standings_buffer(http_buf);
            free(http_buf);
        } else {
            ESP_LOGW(TAG, "积分榜拉取失败, 错误码: %d", got);
            if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snap.loading = false;
            s_snap.error = true;
            s_revision++;
            if (s_lock != NULL) xSemaphoreGive(s_lock);
        }
    }
}

void app_standings_fetch(uint32_t season_id, const char *title) {
    s_req_season_id = season_id;
    snprintf(s_req_title, sizeof(s_req_title), "%s", title != NULL ? title : "");
    s_has_req = true;

    if (s_task_handle == NULL) {
        s_task_handle = xTaskCreateStatic(standings_task, "standings",
                                          STANDINGS_STACK_SIZE,
                                          NULL, 3, s_standings_stack, &s_standings_tcb);
    }
}
