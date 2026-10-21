// main/app_portal.h
// 手机配网门户:设备开一个热点,手机连上后用浏览器输 Wi-Fi 密码。
//
// 为什么另开这条路:这块板只有三个按键,屏上键盘输密码要按几十下,而且
// 一按错就得整段重来。手机上输密码是顺手的事。
//
// 线程模型:本模块自己有一个守护任务,负责"连上了没 / 超时了没 / 该关热点了没"。
// HTTP 服务由 esp_http_server 的任务跑。两者都只碰自己的状态,对外只暴露一份
// 受互斥量保护的快照,界面按 revision 变化去读。
#pragma once

#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_PORTAL_AP_SSID "FoloToy-Setup"
#define APP_PORTAL_AP_PASS "folotoy123"   // WPA2 至少 8 位
#define APP_PORTAL_AP_IP "192.168.4.1"

typedef enum {
    APP_PORTAL_IDLE = 0,     // 没在跑
    APP_PORTAL_WAITING,      // 热点已开,等手机连上来提交
    APP_PORTAL_CONNECTING,   // 收到凭证,正在连路由器
    APP_PORTAL_OK,           // 连上了
    APP_PORTAL_FAILED,       // 连不上(多半是密码错)
} app_portal_state_t;

#define APP_PORTAL_MSG_LEN 40

typedef struct {
    uint8_t state;
    char ssid[APP_SSID_MAX + 1];   // 正在连/已连上的那个热点
    char message[APP_PORTAL_MSG_LEN];
    uint32_t revision;
} app_portal_snapshot_t;

// 开热点并启动网页服务。重复调用等于重启一次。失败返回 false(界面要能说原因)。
bool app_portal_start(void);

// 关热点、停网页服务。没在跑时是空操作。
void app_portal_stop(void);

// 是否正在跑(热点开着)。
bool app_portal_active(void);

// 拷一份状态(线程安全)。
void app_portal_snapshot_copy(app_portal_snapshot_t *out);

// 状态版本号,界面据此决定要不要重画。
uint32_t app_portal_revision(void);
