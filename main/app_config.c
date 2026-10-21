// main/app_config.c
#include "app_config.h"

#include <string.h>

#define APP_REFRESH_DEFAULT 120

// 比分接口一次要下 1.6 MB,所以档位从 60 秒起;30 秒会把电和流量都耗在重复
// 下载同一份数据上。
static const uint16_t REFRESH_OPTIONS[] = { 60, 120, 300 };
#define REFRESH_OPTION_COUNT (sizeof(REFRESH_OPTIONS) / sizeof(REFRESH_OPTIONS[0]))

void app_config_defaults(app_config_t *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->refresh_seconds = APP_REFRESH_DEFAULT;
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
