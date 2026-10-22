// main/app_config.c
#include "app_config.h"

#include <string.h>

#define APP_REFRESH_DEFAULT 120
// 默认一分钟熄屏:看球时会按键,不看时省电。0 = 常亮放在第一档。
#define APP_SCREEN_OFF_DEFAULT 60
#define APP_BRIGHTNESS_DEFAULT 100

// 比分接口一次要下 1.6 MB,所以档位从 60 秒起;30 秒会把电和流量都耗在重复
// 下载同一份数据上。
static const uint16_t REFRESH_OPTIONS[] = { 60, 120, 300 };
#define REFRESH_OPTION_COUNT (sizeof(REFRESH_OPTIONS) / sizeof(REFRESH_OPTIONS[0]))

static const uint16_t SCREEN_OFF_OPTIONS[] = { 0, 30, 60, 120 };
#define SCREEN_OFF_OPTION_COUNT (sizeof(SCREEN_OFF_OPTIONS) / sizeof(SCREEN_OFF_OPTIONS[0]))

static const uint8_t BRIGHTNESS_OPTIONS[] = { 25, 50, 75, 100 };
#define BRIGHTNESS_OPTION_COUNT (sizeof(BRIGHTNESS_OPTIONS) / sizeof(BRIGHTNESS_OPTIONS[0]))

void app_config_defaults(app_config_t *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->refresh_seconds = APP_REFRESH_DEFAULT;
    cfg->screen_off_seconds = APP_SCREEN_OFF_DEFAULT;
    cfg->brightness_percent = APP_BRIGHTNESS_DEFAULT;
}

bool app_config_has_credentials(const app_config_t *cfg) {
    return cfg != NULL && cfg->ssid[0] != '\0';
}

void app_config_cycle_refresh(app_config_t *cfg) {
    if (cfg == NULL) return;
    const uint16_t current = app_config_clamp_refresh(cfg->refresh_seconds);
    size_t index = 0;
    for (size_t i = 0; i < REFRESH_OPTION_COUNT; i++) {
        if (REFRESH_OPTIONS[i] == current) {
            index = i;
            break;
        }
    }
    cfg->refresh_seconds = REFRESH_OPTIONS[(index + 1) % REFRESH_OPTION_COUNT];
}

uint16_t app_config_clamp_refresh(uint16_t seconds) {
    for (size_t i = 0; i < REFRESH_OPTION_COUNT; i++) {
        if (seconds <= REFRESH_OPTIONS[i]) return REFRESH_OPTIONS[i];
    }
    return REFRESH_OPTIONS[REFRESH_OPTION_COUNT - 1];
}

// 三个档位表共用同一套"取最接近的较大档"逻辑:这样 NVS 里存着旧固件的
// 5 秒、或者手改出来的 45 秒,读回来都会落到一个合法值上,不会出现
// 界面上写着 45 秒、实际按 45 秒跑的情况。
static size_t option_index_u16(const uint16_t *options, size_t count, uint16_t value) {
    for (size_t i = 0; i < count; i++) {
        if (value == options[i]) return i;
    }
    return count;   // 不在表里
}

void app_config_cycle_screen_off(app_config_t *cfg) {
    if (cfg == NULL) return;
    const uint16_t current = app_config_clamp_screen_off(cfg->screen_off_seconds);
    size_t index = option_index_u16(SCREEN_OFF_OPTIONS, SCREEN_OFF_OPTION_COUNT, current);
    if (index >= SCREEN_OFF_OPTION_COUNT) index = 0;
    cfg->screen_off_seconds = SCREEN_OFF_OPTIONS[(index + 1) % SCREEN_OFF_OPTION_COUNT];
}

uint16_t app_config_clamp_screen_off(uint16_t seconds) {
    for (size_t i = 0; i < SCREEN_OFF_OPTION_COUNT; i++) {
        if (seconds <= SCREEN_OFF_OPTIONS[i]) return SCREEN_OFF_OPTIONS[i];
    }
    return SCREEN_OFF_OPTIONS[SCREEN_OFF_OPTION_COUNT - 1];
}

void app_config_cycle_brightness(app_config_t *cfg) {
    if (cfg == NULL) return;
    const uint8_t current = app_config_clamp_brightness(cfg->brightness_percent);
    size_t index = 0;
    for (size_t i = 0; i < BRIGHTNESS_OPTION_COUNT; i++) {
        if (BRIGHTNESS_OPTIONS[i] == current) {
            index = i;
            break;
        }
    }
    cfg->brightness_percent = BRIGHTNESS_OPTIONS[(index + 1) % BRIGHTNESS_OPTION_COUNT];
}

uint8_t app_config_clamp_brightness(uint8_t percent) {
    for (size_t i = 0; i < BRIGHTNESS_OPTION_COUNT; i++) {
        if (percent <= BRIGHTNESS_OPTIONS[i]) return BRIGHTNESS_OPTIONS[i];
    }
    return BRIGHTNESS_OPTIONS[BRIGHTNESS_OPTION_COUNT - 1];
}

bool app_config_validate_credentials(const char *ssid, const char *pass) {
    if (ssid == NULL || pass == NULL) return false;
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > APP_SSID_MAX) return false;
    if (pass_len > APP_PASS_MAX) return false;

    for (const char *p = ssid; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20) return false;
    }
    for (const char *p = pass; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20) return false;
    }
    return true;
}
