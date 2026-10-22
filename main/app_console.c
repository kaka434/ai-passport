// main/app_console.c
#include "app_console.h"

#include "app_net.h"

#include "esp_console.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_console";

static const app_console_hooks_t *s_hooks;

static void print_usage(void) {
    printf("可用命令:\n");
    printf("  wifi set <ssid> <password>   保存 Wi-Fi 并立即连接(密码可为空表示开放网络)\n");
    printf("  wifi status                  查看当前配网状态\n");
    printf("  wifi clear                   清除已保存的 Wi-Fi\n");
    printf("  board interval <秒>           设置刷新间隔(60/120/300)\n");
    printf("  board refresh                立即刷新一次\n");
    printf("  board day [n]                查看/切换日期视图(0=今天,-1=昨天,1..7)\n");
    printf("  board detail <match_id>      拉取某一场的赛况(排查用)\n");
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    app_config_t cfg;
    if (s_hooks != NULL && s_hooks->get != NULL) {
        s_hooks->get(&cfg);
    } else {
        app_config_defaults(&cfg);
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            printf("用法: wifi set <ssid> <password>\n");
            return 1;
        }
        const char *ssid = argv[2];
        const char *pass = (argc >= 4) ? argv[3] : "";
        if (!app_config_validate_credentials(ssid, pass)) {
            printf("参数不合法: SSID 需 1-%d 字节,密码不超过 %d 字节,且不能含控制字符\n",
                   APP_SSID_MAX, APP_PASS_MAX);
            return 1;
        }
        snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
        snprintf(cfg.pass, sizeof(cfg.pass), "%s", pass);
        if (s_hooks != NULL && s_hooks->apply != NULL) {
            s_hooks->apply(&cfg, true);
        }
        // 只回显 SSID,密码永远不进日志/终端历史回显。
        printf("已保存 Wi-Fi \"%s\",正在连接…\n", cfg.ssid);
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        memset(cfg.ssid, 0, sizeof(cfg.ssid));
        memset(cfg.pass, 0, sizeof(cfg.pass));
        if (s_hooks != NULL && s_hooks->apply != NULL) {
            s_hooks->apply(&cfg, true);
        }
        printf("已清除 Wi-Fi 凭证\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        printf("SSID: %s\n", cfg.ssid[0] != '\0' ? cfg.ssid : "(未配网)");
        printf("密码: %s\n", cfg.pass[0] != '\0' ? "(已保存)" : "(空)");
        printf("刷新间隔: %u 秒\n", (unsigned)cfg.refresh_seconds);
        if (cfg.screen_off_seconds == 0) printf("自动息屏: 常亮\n");
        else printf("自动息屏: %u 秒\n", (unsigned)cfg.screen_off_seconds);
        printf("屏幕亮度: %u%%\n", (unsigned)cfg.brightness_percent);
        return 0;
    }

    print_usage();
    return 1;
}

static int cmd_board(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    app_config_t cfg;
    if (s_hooks != NULL && s_hooks->get != NULL) {
        s_hooks->get(&cfg);
    } else {
        app_config_defaults(&cfg);
    }

    if (strcmp(argv[1], "interval") == 0) {
        if (argc < 3) {
            printf("当前刷新间隔: %u 秒\n", (unsigned)cfg.refresh_seconds);
            return 0;
        }
        const long value = strtol(argv[2], NULL, 10);
        if (value <= 0) {
            printf("间隔必须是正整数秒\n");
            return 1;
        }
        cfg.refresh_seconds = app_config_clamp_refresh((uint16_t)value);
        if (s_hooks != NULL && s_hooks->apply != NULL) s_hooks->apply(&cfg, false);
        printf("刷新间隔: %u 秒\n", (unsigned)cfg.refresh_seconds);
        return 0;
    }

    if (strcmp(argv[1], "refresh") == 0) {
        if (s_hooks != NULL && s_hooks->apply != NULL) s_hooks->apply(&cfg, false);
        printf("已请求刷新\n");
        return 0;
    }

    // 翻页也留一条串口命令:按键翻日期要盯着屏幕才知道翻到哪天,串口里能
    // 直接看到拉取日志和场次数。
    if (strcmp(argv[1], "day") == 0) {
        if (argc < 3) {
            printf("当前视图: 第 %d 天(0=今天,-1=昨天)\n", (int)app_net_view_day());
            return 0;
        }
        const long value = strtol(argv[2], NULL, 10);
        if (value < APP_VIEW_DAY_MIN || value > APP_VIEW_DAY_MAX) {
            printf("可选范围 %d~%d(0=今天,-1=昨天,1..%d=未来第 N 天)\n",
                   APP_VIEW_DAY_MIN, APP_VIEW_DAY_MAX, APP_VIEW_DAY_MAX);
            return 1;
        }
        app_net_set_view_day((int8_t)value);
        printf("已切到第 %d 天,正在拉取\n", (int)value);
        return 0;
    }

    // 亮度与息屏走串口,是为了不按键也能验证:屏幕贴在壳里,按键反馈要
    // 盯着看很久才知道对不对。
    if (strcmp(argv[1], "bright") == 0) {
        if (argc < 3) {
            printf("当前亮度: %u%%\n", (unsigned)cfg.brightness_percent);
            return 0;
        }
        const long value = strtol(argv[2], NULL, 10);
        if (value <= 0 || value > 100) {
            printf("亮度取值 1~100,会就近收敛到 25/50/75/100\n");
            return 1;
        }
        cfg.brightness_percent = app_config_clamp_brightness((uint8_t)value);
        if (s_hooks != NULL && s_hooks->apply != NULL) s_hooks->apply(&cfg, false);
        printf("屏幕亮度: %u%%\n", (unsigned)cfg.brightness_percent);
        return 0;
    }

    if (strcmp(argv[1], "screenoff") == 0) {
        if (argc < 3) {
            if (cfg.screen_off_seconds == 0) printf("当前自动息屏: 常亮\n");
            else printf("当前自动息屏: %u 秒\n", (unsigned)cfg.screen_off_seconds);
            return 0;
        }
        const long value = strtol(argv[2], NULL, 10);
        if (value < 0) {
            printf("息屏时间不能为负;0 表示常亮\n");
            return 1;
        }
        cfg.screen_off_seconds = app_config_clamp_screen_off((uint16_t)value);
        if (s_hooks != NULL && s_hooks->apply != NULL) s_hooks->apply(&cfg, false);
        if (cfg.screen_off_seconds == 0) printf("自动息屏: 常亮\n");
        else printf("自动息屏: %u 秒\n", (unsigned)cfg.screen_off_seconds);
        return 0;
    }

    // 现场排查用:不必按键,直接把某一场的赛况拉一遍,日志里就能看到
    // 缓冲拿了多大、解析出几个进球。参数是懂球帝的 match id。
    if (strcmp(argv[1], "detail") == 0) {        if (argc < 3) {
            printf("用法: board detail <match_id>\n");
            return 1;
        }
        printf("%s\n", app_net_detail_request(argv[2]) ? "已请求详情" : "详情任务不可用");
        return 0;
    }

    print_usage();
    return 1;
}

void app_console_start(const app_console_hooks_t *hooks) {
    s_hooks = hooks;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "football>";
    repl_config.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "串口命令行不可用:%s", esp_err_to_name(err));
        return;
    }

    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Wi-Fi 配网: set/status/clear",
        .hint = NULL,
        .func = cmd_wifi,
    };
    const esp_console_cmd_t board_cmd = {
        .command = "board",
        .help = "看板设置: interval/bright/screenoff/refresh/detail",
        .hint = NULL,
        .func = cmd_board,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&board_cmd));
    ESP_ERROR_CHECK(esp_console_register_help_command());

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "命令行启动失败:%s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "串口命令行就绪:输入 help 查看命令");
}
