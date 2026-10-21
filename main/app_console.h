// main/app_console.h
// USB 串口配网与调试命令。
//
// 设备只有三个物理按键,输入不了 SSID/密码,所以配网走 USB Serial/JTAG 的
// 命令行。这也是本项目唯一需要电脑的环节:插上数据线、打开串口终端、
// 敲一条 wifi set 命令即可,之后看板自己联网刷新。
#pragma once

#include "app_config.h"

typedef struct {
    // 取当前配置(线程安全由实现方保证)。
    void (*get)(app_config_t *out);
    // 应用一份新配置。credentials_changed=true 时联网任务会重新连接。
    void (*apply)(const app_config_t *cfg, bool credentials_changed);
} app_console_hooks_t;

// 注册命令并启动 REPL。失败只记日志,不影响看板主流程。
void app_console_start(const app_console_hooks_t *hooks);
