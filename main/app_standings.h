#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_STANDINGS_MAX_TEAMS 36
#define APP_STANDINGS_NAME_MAX 31

typedef struct {
    uint8_t rank;
    char team_name[APP_STANDINGS_NAME_MAX + 1];
    uint32_t team_id;
    uint8_t played;
    uint8_t won;
    uint8_t draw;
    uint8_t lost;
    int16_t gd;     // 净胜球
    uint8_t points; // 积分
} app_standing_team_t;

typedef struct {
    uint32_t season_id;
    char title[16];
    uint8_t team_count;
    app_standing_team_t teams[APP_STANDINGS_MAX_TEAMS];
    int64_t updated_at;
    bool loading;
    bool error;
} app_standings_snapshot_t;

// 支持的 8 大联赛定义(懂球帝官方 season_id 映射)
typedef struct {
    const char *name;
    uint32_t season_id;
} app_league_info_t;

#define APP_LEAGUES_COUNT 8
extern const app_league_info_t APP_LEAGUES[APP_LEAGUES_COUNT];

void app_standings_init(void);
void app_standings_fetch(uint32_t season_id, const char *title);
void app_standings_snapshot_copy(app_standings_snapshot_t *dst);
uint32_t app_standings_revision(void);
