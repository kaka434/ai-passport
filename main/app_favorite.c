#include "app_favorite.h"
#include "app_crest.h"
#include "app_net.h"
#include "app_fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "app_favorite";

static app_fav_snapshot_t s_snap;
static SemaphoreHandle_t s_lock;
static uint32_t s_revision = 1;

static uint32_t s_req_team_id = 0;
static char s_req_team_name[APP_FAV_TEAM_NAME_MAX] = {0};
static char s_req_league_name[APP_FAV_LEAGUE_NAME_MAX] = {0};
static bool s_has_req = false;
static TaskHandle_t s_task_handle = NULL;

void app_favorite_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        memset(&s_snap, 0, sizeof(s_snap));
    }
}

void app_favorite_snapshot_copy(app_fav_snapshot_t *dst) {
    if (dst == NULL) return;
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(dst, &s_snap, sizeof(app_fav_snapshot_t));
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

uint32_t app_favorite_revision(void) {
    return s_revision;
}

static uint32_t to_short_id(uint32_t tid) {
    if (tid >= 50000000) return tid % 10000000;
    return tid;
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



static char s_desc_block[512];
static char s_stream_win[2048];
static char s_match_buf[640];
static void format_date_short(const char *full, char *out, size_t max_len) {
    if (full == NULL || out == NULL || max_len == 0) return;
    if (strlen(full) >= 16 && full[4] == '-') {
        // 跳过前4位年份 "2026-"
        snprintf(out, max_len, "%.11s", full + 5);
    } else {
        snprintf(out, max_len, "%s", full);
    }
}

static void parse_team_sample(const char *buf) {
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);

    extract_json_str(buf, "team_en_name", s_snap.team_en_name, sizeof(s_snap.team_en_name));
    extract_json_str(buf, "venue_name", s_snap.venue, sizeof(s_snap.venue));
    extract_json_str(buf, "team_logo", s_snap.team_logo, sizeof(s_snap.team_logo));
    if (s_snap.team_logo[0] != '\0') {
        app_crest_request(APP_CREST_SLOT_FAV, s_snap.team_logo);
    }

    // 解析排名与描述
    const char *p_desc = strstr(buf, "\"description\":");
    if (p_desc) {
        const char *p_end = strchr(p_desc, ']');
        if (p_end) {
            size_t len = p_end - p_desc + 1;
            if (len >= sizeof(s_desc_block)) len = sizeof(s_desc_block) - 1;
            memcpy(s_desc_block, p_desc, len);
            s_desc_block[len] = '\0';

            // 搜索 "近5场"
            const char *p_form = strstr(s_desc_block, "近5场");
            if (p_form) {
                const char *v = strstr(p_form, "\"value\":");
                if (v) extract_json_str(v - 1, "value", s_snap.form_desc, sizeof(s_snap.form_desc));
            }

            // 搜索联赛排名
            const char *p_lg = strstr(s_desc_block, s_snap.league_name);
            if (p_lg) {
                const char *v = strstr(p_lg, "\"value\":");
                if (v) {
                    char r_val[16];
                    extract_json_str(v - 1, "value", r_val, sizeof(r_val));
                    snprintf(s_snap.rank_desc, sizeof(s_snap.rank_desc), "%s名", r_val);
                }
            }

            // 搜索战绩 (胜 平 负)
            const char *p_rec = strstr(s_desc_block, "战绩");
            if (!p_rec) p_rec = strstr(s_desc_block, "主客");
            if (p_rec) {
                const char *v = strstr(p_rec, "\"value\":");
                if (v) extract_json_str(v - 1, "value", s_snap.record_desc, sizeof(s_snap.record_desc));
            }
        }
    }

    if (s_snap.rank_desc[0] == '\0') {
        char rank_str[32];
        if (extract_json_str(buf, "rank", rank_str, sizeof(rank_str))) {
            snprintf(s_snap.rank_desc, sizeof(s_snap.rank_desc), "%s", rank_str);
        }
    }

    if (s_lock != NULL) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Sample解析: 排名=%s, 战绩=%s, 近5场=%s, 球场=%s",
             s_snap.rank_desc, s_snap.record_desc, s_snap.form_desc, s_snap.venue);
}

static void parse_match_object(const char *obj, app_fav_match_t *m, const char *fav_name) {
    memset(m, 0, sizeof(*m));
    extract_json_str(obj, "competition_name", m->comp, sizeof(m->comp));
    extract_json_str(obj, "team_A_name", m->team_A, sizeof(m->team_A));
    extract_json_str(obj, "team_B_name", m->team_B, sizeof(m->team_B));

    char raw_date[32];
    extract_json_str(obj, "start_play", raw_date, sizeof(raw_date));
    format_date_short(raw_date, m->date_str, sizeof(m->date_str));

    extract_json_str(obj, "status", m->status, sizeof(m->status));

    char fs_a[8] = "", fs_b[8] = "";
    extract_json_str(obj, "fs_A", fs_a, sizeof(fs_a));
    extract_json_str(obj, "fs_B", fs_b, sizeof(fs_b));
    m->score_A = (fs_a[0] >= '0' && fs_a[0] <= '9') ? (int8_t)atoi(fs_a) : -1;
    m->score_B = (fs_b[0] >= '0' && fs_b[0] <= '9') ? (int8_t)atoi(fs_b) : -1;

    m->is_home = (strstr(m->team_A, fav_name) != NULL);

    if (strcmp(m->status, "Played") == 0 && m->score_A >= 0 && m->score_B >= 0) {
        if (m->score_A == m->score_B) {
            m->result = 0; // 平
        } else if (m->is_home) {
            m->result = (m->score_A > m->score_B) ? 1 : -1;
        } else {
            m->result = (m->score_B > m->score_A) ? 1 : -1;
        }
    } else {
        m->result = -2; // 赛前
    }
}

// 任务栈: 5120 字节 (大缓冲区已移至静态 BSS，栈空间充裕)
#define FAV_STACK_SIZE 5120
static StackType_t s_fav_stack[FAV_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_fav_tcb;

static void favorite_task(void *arg) {
    while (1) {
        if (!s_has_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t tid = s_req_team_id;
        char name[APP_FAV_TEAM_NAME_MAX];
        char league[APP_FAV_LEAGUE_NAME_MAX];
        snprintf(name, sizeof(name), "%s", s_req_team_name);
        snprintf(league, sizeof(league), "%s", s_req_league_name);
        s_has_req = false;

        if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.has_fav = (tid > 0);
        s_snap.team_id = tid;
        snprintf(s_snap.team_name, sizeof(s_snap.team_name), "%s", name);
        snprintf(s_snap.league_name, sizeof(s_snap.league_name), "%s", league);
        s_snap.loading = true;
        s_snap.error = false;
        s_revision++;
        if (s_lock != NULL) xSemaphoreGive(s_lock);

        uint32_t sid = to_short_id(tid);
        ESP_LOGI(TAG, "拉取主队数据: %s (id=%lu, short=%lu)", name, (unsigned long)tid, (unsigned long)sid);

        // 1. 拉取 Sample 概览 (约 2KB)
        char sample_url[128];
        snprintf(sample_url, sizeof(sample_url),
                 "http://sport-data.dongqiudi.com/soccer/biz/dqd/team/sample/%lu?app=dqd", (unsigned long)sid);

        char *sample_buf = NULL;
        int got = app_net_fetch_alloc(sample_url, "https://www.dongqiudi.com/", &sample_buf, 3072);
        if (got > 0 && sample_buf != NULL) {
            parse_team_sample(sample_buf);
            free(sample_buf);
        } else {
            ESP_LOGW(TAG, "Sample 拉取失败: %d", got);
        }

        // 2. 流式拉取赛程 (滑动窗口解析上一场与下一场，找到即止，绝不缓冲全文!)
        char sched_url[128];
        snprintf(sched_url, sizeof(sched_url),
                 "http://sport-data.dongqiudi.com/soccer/biz/dqd/team/schedule/%lu?app=dqd", (unsigned long)sid);

        app_fav_match_t last_m;
        app_fav_match_t next_m;
        bool has_last = false;
        bool has_next = false;
        memset(&last_m, 0, sizeof(last_m));
        memset(&next_m, 0, sizeof(next_m));

        if (app_net_http_lock_take(30000)) {
            esp_http_client_config_t cfg = {
                .url = sched_url,
                .timeout_ms = 8000,
                .buffer_size = 1024,
                .buffer_size_tx = 512,
                .user_agent = "Mozilla/5.0",
            };
            esp_http_client_handle_t client = esp_http_client_init(&cfg);

            if (client != NULL) {
                esp_http_client_set_header(client, "Referer", "https://www.dongqiudi.com/");
                if (esp_http_client_open(client, 0) == ESP_OK) {
                    esp_http_client_fetch_headers(client);
                    if (esp_http_client_get_status_code(client) == 200) {
                        size_t win_len = 0;

                        while (!has_next) {
                            int n = esp_http_client_read(client, s_stream_win + win_len, (int)(sizeof(s_stream_win) - 1 - win_len));
                            if (n <= 0) break;
                            win_len += (size_t)n;
                            s_stream_win[win_len] = '\0';

                            // 循环解析完整对象
                            while (1) {
                                char *start = strstr(s_stream_win, "{\"match_id\"");
                                if (!start) start = strstr(s_stream_win, "{\"competition_id\"");
                                if (!start) {
                                    if (win_len > 1024) {
                                        memmove(s_stream_win, s_stream_win + win_len - 512, 512);
                                        win_len = 512;
                                        s_stream_win[win_len] = '\0';
                                    }
                                    break;
                                }

                                char *end = strchr(start, '}');
                                if (!end) {
                                    size_t remain = strlen(start);
                                    memmove(s_stream_win, start, remain);
                                    win_len = remain;
                                    s_stream_win[win_len] = '\0';
                                    break;
                                }

                                size_t obj_len = end - start + 1;
                                if (obj_len >= sizeof(s_match_buf)) obj_len = sizeof(s_match_buf) - 1;
                                memcpy(s_match_buf, start, obj_len);
                                s_match_buf[obj_len] = '\0';

                                app_fav_match_t cur;
                                parse_match_object(s_match_buf, &cur, name);

                                if (strcmp(cur.status, "Played") == 0) {
                                    last_m = cur;
                                    has_last = true;
                                } else if (strcmp(cur.status, "Fixture") == 0 ||
                                           strcmp(cur.status, "Playing") == 0 ||
                                           strcmp(cur.status, "Live") == 0) {
                                    next_m = cur;
                                    has_next = true;
                                    break; // 已经找到最近未来场次，提前终止流式读取！
                                }

                                size_t consumed = (end + 1) - s_stream_win;
                                if (consumed < win_len) {
                                    memmove(s_stream_win, s_stream_win + consumed, win_len - consumed);
                                    win_len -= consumed;
                                    s_stream_win[win_len] = '\0';
                                } else {
                                    win_len = 0;
                                    s_stream_win[0] = '\0';
                                }
                            }
                        }
                    }
                    esp_http_client_close(client);
                }
                esp_http_client_cleanup(client);
            }
            app_net_http_lock_give();
        } else {
            ESP_LOGW(TAG, "等待 HTTP 锁超时(赛程)");
        }

        if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (has_last) s_snap.last_match = last_m;
        if (has_next) s_snap.next_match = next_m;
        s_snap.loading = false;
        s_snap.error = false;
        s_snap.updated_at = esp_timer_get_time() / 1000000;
        s_revision++;
        if (s_lock != NULL) xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "主队更新完成: %s | 上场: %s %d-%d %s (%s) | 下场: %s vs %s (%s)",
                 name,
                 last_m.team_A, last_m.score_A, last_m.score_B, last_m.team_B, last_m.date_str,
                 next_m.team_A, next_m.team_B, next_m.date_str);
    }
}

void app_favorite_fetch(uint32_t team_id, const char *team_name, const char *league_name) {
    if (team_id == 0) return;
    if (s_snap.loading && s_req_team_id == team_id) return;

    s_req_team_id = team_id;
    snprintf(s_req_team_name, sizeof(s_req_team_name), "%s", team_name != NULL ? team_name : "");
    snprintf(s_req_league_name, sizeof(s_req_league_name), "%s", league_name != NULL ? league_name : "");
    s_has_req = true;

    if (s_task_handle == NULL) {
        s_task_handle = xTaskCreateStatic(favorite_task, "app_fav",
                                          FAV_STACK_SIZE,
                                          NULL, 3, s_fav_stack, &s_fav_tcb);
    }
}
