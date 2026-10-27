#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_FAV_TEAM_NAME_MAX 32
#define APP_FAV_LEAGUE_NAME_MAX 16

typedef struct {
    char comp[16];        // 赛事名 "英超"
    char team_A[24];      // 主队
    char team_B[24];      // 客队
    int8_t score_A;       // 主队得分 (-1 为未赛)
    int8_t score_B;       // 客队得分 (-1 为未赛)
    char date_str[24];    // "10-24 22:00"
    char status[16];      // "Played" / "Fixture" / "Playing"
    bool is_home;         // 关注的主队是否为主场
    int8_t result;        // 1: 胜, 0: 平, -1: 负, -2: 赛前/进行中/未知
} app_fav_match_t;

typedef struct {
    bool has_fav;
    uint32_t team_id;
    char team_name[APP_FAV_TEAM_NAME_MAX];
    char team_en_name[APP_FAV_TEAM_NAME_MAX];
    char league_name[APP_FAV_LEAGUE_NAME_MAX];
    char rank_desc[32];      // "第 2 名"
    char record_desc[32];    // "7胜 0平 1负"
    char form_desc[24];      // "胜 胜 胜 平 胜"
    char venue[32];          // "酋长球场"
    app_fav_match_t last_match;
    app_fav_match_t next_match;
    bool loading;
    bool error;
    uint32_t updated_at;
} app_fav_snapshot_t;

void app_favorite_init(void);
void app_favorite_snapshot_copy(app_fav_snapshot_t *dst);
uint32_t app_favorite_revision(void);
void app_favorite_fetch(uint32_t team_id, const char *team_name, const char *league_name);
