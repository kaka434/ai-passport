// main/app_store.h
// 配置持久化(NVS)。只做读写,不做校验逻辑——校验在 app_config / app_known 里,
// 便于主机测试。
#pragma once

#include "app_config.h"
#include "app_known.h"

#include "esp_err.h"

// 初始化 NVS 并读取配置。首次启动或数据损坏时回落到默认值并返回 ESP_ERR_NOT_FOUND。
esp_err_t app_store_load(app_config_t *cfg);

esp_err_t app_store_save(const app_config_t *cfg);

// 只清凭证,保留联赛/刷新间隔等偏好。
esp_err_t app_store_clear_credentials(void);

// "连过的网络"历史。没有历史时返回 ESP_ERR_NVS_NOT_FOUND 并把 list 清空。
esp_err_t app_store_load_known(app_known_list_t *list);

esp_err_t app_store_save_known(const app_known_list_t *list);

// 记一条成功连上的网络(读-改-写)。ssid 非法时返回 ESP_ERR_INVALID_ARG。
esp_err_t app_store_remember(const char *ssid, const char *pass);

// ---------------------------------------------------------------- 关注球队
typedef struct {
    uint32_t team_id;
    char team_name[32];
    char league_name[16];
} app_fav_team_t;

esp_err_t app_store_load_fav(app_fav_team_t *fav);
esp_err_t app_store_save_fav(const app_fav_team_t *fav);
esp_err_t app_store_clear_fav(void);
