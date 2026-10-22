// tests/test_app_config.c —— 配置默认值、档位循环与凭证校验。
#include "app_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults(void) {
    app_config_t cfg;
    app_config_defaults(&cfg);
    assert(cfg.ssid[0] == '\0');
    assert(cfg.pass[0] == '\0');
    // 默认 120 秒:比分接口一次要下 1.6 MB,60 秒太费电也容易被上游限速。
    assert(cfg.refresh_seconds == 120);
    // 默认一分钟熄屏、全亮。
    assert(cfg.screen_off_seconds == 60);
    assert(cfg.brightness_percent == 100);
    assert(!app_config_has_credentials(&cfg));
}

static void test_defaults_null_safe(void) {
    app_config_defaults(NULL);   // 不炸
    assert(!app_config_has_credentials(NULL));
}

static void test_credentials_presence(void) {
    app_config_t cfg;
    app_config_defaults(&cfg);
    snprintf(cfg.ssid, sizeof(cfg.ssid), "MyWiFi");
    assert(app_config_has_credentials(&cfg));
    // 开放网络:有 SSID、空密码也算已配网。
    assert(cfg.pass[0] == '\0');
}

static void test_refresh_cycle(void) {
    app_config_t cfg;
    app_config_defaults(&cfg);
    assert(cfg.refresh_seconds == 120);

    app_config_cycle_refresh(&cfg);
    assert(cfg.refresh_seconds == 300);
    app_config_cycle_refresh(&cfg);
    assert(cfg.refresh_seconds == 60);
    app_config_cycle_refresh(&cfg);
    assert(cfg.refresh_seconds == 120);
}

static void test_refresh_cycle_from_dirty_value(void) {
    // NVS 里读回一个不在档位上的值,循环时先收敛再前进,不会卡住。
    app_config_t cfg;
    app_config_defaults(&cfg);
    cfg.refresh_seconds = 77;
    app_config_cycle_refresh(&cfg);
    assert(cfg.refresh_seconds == 300);
}

static void test_refresh_clamp(void) {
    assert(app_config_clamp_refresh(0) == 60);
    assert(app_config_clamp_refresh(1) == 60);
    assert(app_config_clamp_refresh(45) == 60);
    assert(app_config_clamp_refresh(60) == 60);
    assert(app_config_clamp_refresh(61) == 120);
    assert(app_config_clamp_refresh(121) == 300);
    assert(app_config_clamp_refresh(9999) == 300);
}

static void test_validate_credentials(void) {
    assert(app_config_validate_credentials("home", "secret123"));
    assert(app_config_validate_credentials("home", ""));          // 开放网络
    assert(!app_config_validate_credentials("", "secret"));
    assert(!app_config_validate_credentials(NULL, "secret"));
    assert(!app_config_validate_credentials("home", NULL));

    char too_long_ssid[APP_SSID_MAX + 8];
    memset(too_long_ssid, 'a', sizeof(too_long_ssid) - 1);
    too_long_ssid[sizeof(too_long_ssid) - 1] = '\0';
    assert(!app_config_validate_credentials(too_long_ssid, ""));

    char too_long_pass[APP_PASS_MAX + 8];
    memset(too_long_pass, 'b', sizeof(too_long_pass) - 1);
    too_long_pass[sizeof(too_long_pass) - 1] = '\0';
    assert(!app_config_validate_credentials("home", too_long_pass));

    // 控制字符会破坏命令行解析与 NVS 字符串,直接拒绝。
    assert(!app_config_validate_credentials("ho\nme", ""));
    assert(!app_config_validate_credentials("home", "pa\tss"));

    // 正好等于上限是合法的。
    char exact_ssid[APP_SSID_MAX + 1];
    memset(exact_ssid, 'c', APP_SSID_MAX);
    exact_ssid[APP_SSID_MAX] = '\0';
    assert(app_config_validate_credentials(exact_ssid, ""));
}

static void test_setting_rows(void) {
    // 界面按这个顺序画,按键按这个顺序命中;两页布局都依赖这个顺序稳定。
    assert(APP_SET_ROW_WIFI == 0);
    assert(APP_SET_ROW_PORTAL == 1);
    assert(APP_SET_ROW_NETWORK == 2);
    assert(APP_SET_ROW_INTERVAL == 3);
    assert(APP_SET_ROW_SCREEN_OFF == 4);
    assert(APP_SET_ROW_BRIGHTNESS == 5);
    assert(APP_SET_ROW_REFRESH == 6);
    assert(APP_SET_ROW_CLEAR == 7);
    assert(APP_SET_ROW_COUNT == 8);
}

static void test_screen_off_cycle(void) {
    app_config_t cfg;
    app_config_defaults(&cfg);
    assert(cfg.screen_off_seconds == 60);

    // 60 → 120 → 0(常亮)→ 30 → 60
    app_config_cycle_screen_off(&cfg);
    assert(cfg.screen_off_seconds == 120);
    app_config_cycle_screen_off(&cfg);
    assert(cfg.screen_off_seconds == 0);
    app_config_cycle_screen_off(&cfg);
    assert(cfg.screen_off_seconds == 30);
    app_config_cycle_screen_off(&cfg);
    assert(cfg.screen_off_seconds == 60);
}

static void test_screen_off_clamp(void) {
    // 0 是合法档位(常亮),不能被"收敛到最接近的较大档"吃掉。
    assert(app_config_clamp_screen_off(0) == 0);
    assert(app_config_clamp_screen_off(1) == 30);
    assert(app_config_clamp_screen_off(30) == 30);
    assert(app_config_clamp_screen_off(31) == 60);
    assert(app_config_clamp_screen_off(121) == 120);
    assert(app_config_clamp_screen_off(9999) == 120);
}

static void test_brightness_cycle(void) {
    app_config_t cfg;
    app_config_defaults(&cfg);
    assert(cfg.brightness_percent == 100);

    // 100 → 25 → 50 → 75 → 100
    app_config_cycle_brightness(&cfg);
    assert(cfg.brightness_percent == 25);
    app_config_cycle_brightness(&cfg);
    assert(cfg.brightness_percent == 50);
    app_config_cycle_brightness(&cfg);
    assert(cfg.brightness_percent == 75);
    app_config_cycle_brightness(&cfg);
    assert(cfg.brightness_percent == 100);
}

static void test_brightness_clamp(void) {
    assert(app_config_clamp_brightness(0) == 25);
    assert(app_config_clamp_brightness(25) == 25);
    assert(app_config_clamp_brightness(26) == 50);
    assert(app_config_clamp_brightness(99) == 100);
    assert(app_config_clamp_brightness(255) == 100);
}

int main(void) {
    test_defaults();
    test_defaults_null_safe();
    test_credentials_presence();
    test_refresh_cycle();
    test_refresh_cycle_from_dirty_value();
    test_refresh_clamp();
    test_validate_credentials();
    test_setting_rows();
    test_screen_off_cycle();
    test_screen_off_clamp();
    test_brightness_cycle();
    test_brightness_clamp();
    puts("test_app_config: OK");
    return 0;
}
