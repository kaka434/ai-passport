// main/app_store.c
#include "app_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "app_store";

// 独立命名空间:即使以后 NVS 里还有别的组件的数据,也不会互相踩。
#define NS_NAME "fb_board"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define KEY_INTERVAL "interval"
#define KEY_SCREEN_OFF "screen_off"
#define KEY_BRIGHT "bright"
#define KEY_THEME "theme"
#define KEY_KNOWN "known"
#define KEY_FAV_ID "fav_id"
#define KEY_FAV_NAME "fav_name"
#define KEY_FAV_LEAGUE "fav_league"
#define KEY_FAV_LIST "fav_list"

// "连过的网络"整份存成一个 blob:最多 5 条,一条 98 字节,总共不到 500 字节,
// 拆成 5×2 个键反而更难维护。version 用来在结构变化时安全地丢弃旧数据。
#define KNOWN_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t count;
    app_known_net_t items[APP_KNOWN_MAX];
} known_blob_t;

// 关注球队多队 blob
#define FAV_LIST_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t count;
    uint8_t active_idx;
    app_fav_team_t teams[APP_FAV_MAX];
} fav_blob_t;

static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *out) {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    return nvs_open(NS_NAME, mode, out);
}

esp_err_t app_store_load(app_config_t *cfg) {
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    app_config_defaults(cfg);

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(cfg->ssid);
    if (nvs_get_str(handle, KEY_SSID, cfg->ssid, &len) != ESP_OK) {
        cfg->ssid[0] = '\0';
    }
    len = sizeof(cfg->pass);
    if (nvs_get_str(handle, KEY_PASS, cfg->pass, &len) != ESP_OK) {
        cfg->pass[0] = '\0';
    }

    uint16_t value = 0;
    if (nvs_get_u16(handle, KEY_INTERVAL, &value) == ESP_OK) {
        cfg->refresh_seconds = app_config_clamp_refresh(value);
    }
    // 老固件没有这两个键:nvs_get 失败就保留 app_config_defaults() 给的值。
    if (nvs_get_u16(handle, KEY_SCREEN_OFF, &value) == ESP_OK) {
        cfg->screen_off_seconds = app_config_clamp_screen_off(value);
    }
    uint8_t bright = 0;
    if (nvs_get_u8(handle, KEY_BRIGHT, &bright) == ESP_OK) {
        cfg->brightness_percent = app_config_clamp_brightness(bright);
    }
    uint8_t theme = 0;
    if (nvs_get_u8(handle, KEY_THEME, &theme) == ESP_OK) {
        cfg->theme_idx = app_config_clamp_theme(theme);
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "配置已读取:主题=%u 间隔=%us 息屏=%us 亮度=%u%% 配网=%s",
             (unsigned)cfg->theme_idx, (unsigned)cfg->refresh_seconds,
             (unsigned)cfg->screen_off_seconds, (unsigned)cfg->brightness_percent,
             app_config_has_credentials(cfg) ? "有" : "无");
    return app_config_has_credentials(cfg) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_store_save(const app_config_t *cfg) {
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_SSID, cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_PASS, cfg->pass);
    if (err == ESP_OK) err = nvs_set_u16(handle, KEY_INTERVAL, cfg->refresh_seconds);
    if (err == ESP_OK) err = nvs_set_u16(handle, KEY_SCREEN_OFF, cfg->screen_off_seconds);
    if (err == ESP_OK) err = nvs_set_u8(handle, KEY_BRIGHT, cfg->brightness_percent);
    if (err == ESP_OK) err = nvs_set_u8(handle, KEY_THEME, cfg->theme_idx);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存失败:%s", esp_err_to_name(err));
    return err;
}

esp_err_t app_store_clear_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(handle, KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    esp_err_t pass_err = nvs_erase_key(handle, KEY_PASS);
    if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) err = pass_err;
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

// ---------------------------------------------------------------- 连过的网络

esp_err_t app_store_load_known(app_known_list_t *list) {
    if (list == NULL) return ESP_ERR_INVALID_ARG;
    app_known_reset(list);

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    known_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, KEY_KNOWN, &blob, &size);
    nvs_close(handle);

    if (err != ESP_OK) return err;
    if (size != sizeof(known_blob_t)) {
        ESP_LOGW(TAG, "known blob 长度不匹配(%u != %u),作废", (unsigned)size, (unsigned)sizeof(known_blob_t));
        return ESP_ERR_INVALID_SIZE;
    }
    const known_blob_t *b = (const known_blob_t *)&blob;
    if (b->version != KNOWN_VERSION) {
        ESP_LOGW(TAG, "known blob 版本不匹配(%u != %u),作废", b->version, KNOWN_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    list->count = b->count > APP_KNOWN_MAX ? APP_KNOWN_MAX : b->count;
    memcpy(list->items, b->items, list->count * sizeof(list->items[0]));
    return ESP_OK;
}

esp_err_t app_store_save_known(const app_known_list_t *list) {
    if (list == NULL) return ESP_ERR_INVALID_ARG;

    known_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = KNOWN_VERSION;
    blob.count = list->count > APP_KNOWN_MAX ? APP_KNOWN_MAX : list->count;
    memcpy(blob.items, list->items, blob.count * sizeof(blob.items[0]));

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_KNOWN, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_store_remember(const char *ssid, const char *pass) {
    app_known_list_t list;
    esp_err_t load_err = app_store_load_known(&list);
    if (load_err != ESP_OK && load_err != ESP_ERR_NVS_NOT_FOUND) return load_err;

    if (app_known_upsert(&list, ssid, pass) < 0) return ESP_ERR_INVALID_ARG;
    return app_store_save_known(&list);
}

// ---------------------------------------------------------------- 关注球队(多队支持)

esp_err_t app_store_load_fav_list(app_fav_list_t *list) {
    if (list == NULL) return ESP_ERR_INVALID_ARG;
    memset(list, 0, sizeof(*list));

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    // 先读新的 blob 格式
    fav_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, KEY_FAV_LIST, &blob, &size);
    if (err == ESP_OK && size == sizeof(fav_blob_t) && blob.version == FAV_LIST_VERSION) {
        list->count = blob.count > APP_FAV_MAX ? APP_FAV_MAX : blob.count;
        list->active_idx = blob.active_idx < list->count ? blob.active_idx : 0;
        memcpy(list->teams, blob.teams, sizeof(list->teams));
        nvs_close(handle);
        return list->count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    // 没有 blob 时向下兼容老版本的单队结构
    uint32_t tid = 0;
    err = nvs_get_u32(handle, KEY_FAV_ID, &tid);
    if (err == ESP_OK && tid > 0) {
        list->count = 1;
        list->active_idx = 0;
        list->teams[0].team_id = tid;
        size_t len = sizeof(list->teams[0].team_name);
        (void)nvs_get_str(handle, KEY_FAV_NAME, list->teams[0].team_name, &len);
        len = sizeof(list->teams[0].league_name);
        (void)nvs_get_str(handle, KEY_FAV_LEAGUE, list->teams[0].league_name, &len);
        nvs_close(handle);
        // 保存升级为 blob
        (void)app_store_save_fav_list(list);
        return ESP_OK;
    }

    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_store_save_fav_list(const app_fav_list_t *list) {
    if (list == NULL) return ESP_ERR_INVALID_ARG;

    fav_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = FAV_LIST_VERSION;
    blob.count = list->count > APP_FAV_MAX ? APP_FAV_MAX : list->count;
    blob.active_idx = list->active_idx < blob.count ? list->active_idx : 0;
    memcpy(blob.teams, list->teams, sizeof(blob.teams));

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_FAV_LIST, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "关注球队列表已保存:共 %u 支,当前第 %u 支", blob.count, blob.active_idx + 1);
    }
    return err;
}

esp_err_t app_store_clear_fav_list(void) {
    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(handle, KEY_FAV_LIST);
    (void)nvs_erase_key(handle, KEY_FAV_ID);
    (void)nvs_erase_key(handle, KEY_FAV_NAME);
    (void)nvs_erase_key(handle, KEY_FAV_LEAGUE);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool app_fav_list_upsert(app_fav_list_t *list, const app_fav_team_t *team) {
    if (list == NULL || team == NULL || team->team_id == 0) return false;

    // 1. 检查是否已经在关注列表中
    for (uint8_t i = 0; i < list->count; i++) {
        if (list->teams[i].team_id == team->team_id) {
            list->active_idx = i;
            memcpy(&list->teams[i], team, sizeof(*team));
            return true;
        }
    }

    // 2. 如果未满 APP_FAV_MAX, 则新增
    if (list->count < APP_FAV_MAX) {
        memcpy(&list->teams[list->count], team, sizeof(*team));
        list->active_idx = list->count;
        list->count++;
        return true;
    }

    // 3. 已满 APP_FAV_MAX, 替换当前 active 球队
    if (list->active_idx >= APP_FAV_MAX) list->active_idx = 0;
    memcpy(&list->teams[list->active_idx], team, sizeof(*team));
    return true;
}

void app_fav_list_cycle(app_fav_list_t *list) {
    if (list == NULL || list->count <= 1) return;
    list->active_idx = (list->active_idx + 1) % list->count;
}

// 兼容单支旧接口
esp_err_t app_store_load_fav(app_fav_team_t *fav) {
    if (fav == NULL) return ESP_ERR_INVALID_ARG;
    app_fav_list_t list;
    esp_err_t err = app_store_load_fav_list(&list);
    if (err == ESP_OK && list.count > 0) {
        uint8_t idx = list.active_idx < list.count ? list.active_idx : 0;
        memcpy(fav, &list.teams[idx], sizeof(*fav));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_store_save_fav(const app_fav_team_t *fav) {
    app_fav_list_t list;
    (void)app_store_load_fav_list(&list);
    app_fav_list_upsert(&list, fav);
    return app_store_save_fav_list(&list);
}

esp_err_t app_store_clear_fav(void) {
    return app_store_clear_fav_list();
}
