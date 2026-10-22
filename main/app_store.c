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
#define KEY_KNOWN "known"

// "连过的网络"整份存成一个 blob:最多 5 条,一条 98 字节,总共不到 500 字节,
// 拆成 5×2 个键反而更难维护。version 用来在结构变化时安全地丢弃旧数据。
#define KNOWN_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t count;
    app_known_net_t items[APP_KNOWN_MAX];
} known_blob_t;

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

    nvs_close(handle);
    ESP_LOGI(TAG, "配置已读取:间隔=%us 息屏=%us 亮度=%u%% 配网=%s",
             (unsigned)cfg->refresh_seconds, (unsigned)cfg->screen_off_seconds,
             (unsigned)cfg->brightness_percent,
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
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    err = nvs_get_blob(handle, KEY_KNOWN, &blob, &len);
    nvs_close(handle);

    if (err != ESP_OK) return err;

    // 长度或版本对不上就当作没有历史:宁可让用户重输一次密码,
    // 也不要把半截结构当成有效数据用。
    if (len != sizeof(blob) || blob.version != KNOWN_VERSION || blob.count > APP_KNOWN_MAX) {
        ESP_LOGW(TAG, "已保存网络格式不符(长度 %u),忽略", (unsigned)len);
        return ESP_ERR_INVALID_VERSION;
    }

    list->count = blob.count;
    memcpy(list->items, blob.items, sizeof(blob.items));
    ESP_LOGI(TAG, "已读取 %u 个连过的网络", (unsigned)list->count);
    return ESP_OK;
}

esp_err_t app_store_save_known(const app_known_list_t *list) {
    if (list == NULL) return ESP_ERR_INVALID_ARG;

    known_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = KNOWN_VERSION;
    blob.count = (uint8_t)(list->count > APP_KNOWN_MAX ? APP_KNOWN_MAX : list->count);
    memcpy(blob.items, list->items, sizeof(blob.items));

    nvs_handle_t handle;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_KNOWN, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) ESP_LOGE(TAG, "保存已连接网络失败:%s", esp_err_to_name(err));
    return err;
}

esp_err_t app_store_remember(const char *ssid, const char *pass) {
    if (!app_config_validate_credentials(ssid, pass)) return ESP_ERR_INVALID_ARG;

    app_known_list_t list;
    // 读失败(首次开机)就当空表,不影响写入。
    (void)app_store_load_known(&list);
    if (app_known_upsert(&list, ssid, pass) < 0) return ESP_ERR_INVALID_ARG;
    return app_store_save_known(&list);
}
