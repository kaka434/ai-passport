// main/app_settings.h
// 配置的单一入口:界面和串口命令都通过它读写,避免两处各存一份配置。
// 实现在 main.c —— 那里才知道持久化、联网任务和界面三者的更新顺序。
#pragma once

#include "app_config.h"

// 取当前配置(线程安全)。
void app_settings_get(app_config_t *out);

// 应用并持久化一份配置。credentials_changed=true 时联网任务会重连。
void app_settings_apply(const app_config_t *cfg, bool credentials_changed);
