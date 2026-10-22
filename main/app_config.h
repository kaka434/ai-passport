// main/app_config.h
// 应用配置(网络凭证 + 看板偏好)的结构与纯逻辑。
//
// 凭证保存在 NVS 的独立命名空间里,不写进代码、不打印到日志。设备没有
// 中文输入能力,配网走 USB 串口命令(见 app_console.c),这是本项目明确
// 选择的配网方式。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_SSID_MAX 32
#define APP_PASS_MAX 64

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    char pass[APP_PASS_MAX + 1];
    uint16_t refresh_seconds;
    // 无按键动作多少秒后关背光。0 = 常亮。只关背光,不停联网:点亮瞬间
    // 看到的就是最新比分,不用等一次拉取。
    uint16_t screen_off_seconds;
    // 背光亮度百分比,只有 25/50/75/100 四档(见 app_config_cycle_brightness)。
    uint8_t brightness_percent;
} app_config_t;

// 设置页的行顺序。界面按这个顺序画,按键按这个顺序命中。
typedef enum {
    APP_SET_ROW_WIFI = 0,
    APP_SET_ROW_PORTAL,      // 手机配网:开热点,手机浏览器里输密码
    APP_SET_ROW_IP,
    APP_SET_ROW_INTERVAL,
    APP_SET_ROW_SCREEN_OFF,  // 自动息屏
    APP_SET_ROW_BRIGHTNESS,  // 屏幕亮度
    APP_SET_ROW_REFRESH,
    APP_SET_ROW_CLEAR,
    APP_SET_ROW_COUNT,
} app_set_row_t;

void app_config_defaults(app_config_t *cfg);

// ssid 非空即视为已配网;密码允许为空(开放网络)。
bool app_config_has_credentials(const app_config_t *cfg);

// 在 60/120/300 秒之间循环。默认 120:比分接口一次要下 1.6 MB,
// 间隔太短既费电也容易被上游限速。
void app_config_cycle_refresh(app_config_t *cfg);

// 把任意秒数收敛到允许的档位(取最接近的较大档),用于从 NVS 读回脏数据。
uint16_t app_config_clamp_refresh(uint16_t seconds);

// 自动息屏档位:0(常亮)/30/60/120 秒,循环。
void app_config_cycle_screen_off(app_config_t *cfg);
uint16_t app_config_clamp_screen_off(uint16_t seconds);

// 亮度档位:25/50/75/100,循环。
void app_config_cycle_brightness(app_config_t *cfg);
uint8_t app_config_clamp_brightness(uint8_t percent);

// 联网前的最小校验:长度、非空、不含控制字符。
bool app_config_validate_credentials(const char *ssid, const char *pass);
