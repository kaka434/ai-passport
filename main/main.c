// main/main.c —— 足球比赛看板启动入口。
//
// 与基线 demo 的区别:这里没有测试菜单,开机直接进看板。按键事件只入队,
// 由输入任务在持有 LVGL 锁的情况下交给界面状态机;网络与电池读数都在
// 独立任务里,任何慢操作都不占用按键回调。
#include "app_config.h"
#include "app_console.h"
#include "app_net.h"
#include "app_settings.h"
#include "app_splash.h"
#include "app_store.h"
#include "app_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "football";

#define INPUT_QUEUE_DEPTH 8
#define BATTERY_POLL_MS 30000

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static SemaphoreHandle_t s_config_lock;
static app_config_t s_config;
static volatile bool s_input_ready;

// ---------------------------------------------------------------- 息屏与亮度
//
// 只关背光,不停联网:点亮的那一下看到的就是最新比分,不用等一次拉取。
// s_screen_off 由空闲任务置位、由按键回调清除,两边都是 32 位对齐读写,
// 不加锁也不会读到半个值。
static volatile int64_t s_last_input_us;
static volatile bool s_screen_off;
static uint8_t s_brightness = 100;

static void screen_wake(void) {
    if (!s_screen_off) return;
    s_screen_off = false;
    bsp_display_backlight(s_brightness);
    ESP_LOGI(TAG, "按键点亮屏幕");
}

// 500 ms 一次的粗粒度检查:息屏是"几十秒"量级的事,不需要更细。
static void idle_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_screen_off) continue;

        app_config_t cfg;
        app_settings_get(&cfg);
        const uint16_t off_seconds = cfg.screen_off_seconds;
        if (off_seconds == 0) continue;   // 常亮

        const int64_t idle_us = esp_timer_get_time() - s_last_input_us;
        if (idle_us < (int64_t)off_seconds * 1000000LL) continue;

        s_screen_off = true;
        bsp_display_backlight(0);
        ESP_LOGI(TAG, "无操作 %u 秒,息屏", (unsigned)off_seconds);
    }
}

// ---------------------------------------------------------------- 配置桥

void app_settings_get(app_config_t *out) {
    if (out == NULL) return;
    if (s_config_lock == NULL) {
        app_config_defaults(out);
        return;
    }
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_config_lock);
}

void app_settings_apply(const app_config_t *cfg, bool credentials_changed) {
    if (cfg == NULL) return;

    if (s_config_lock != NULL) {
        xSemaphoreTake(s_config_lock, portMAX_DELAY);
        s_config = *cfg;
        xSemaphoreGive(s_config_lock);
    }

    const esp_err_t err = app_store_save(&s_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "配置保存失败(本次运行仍然生效):%s", esp_err_to_name(err));
    }

    if (credentials_changed) {
        app_net_forget_credentials();
    }
    app_net_apply_config(&s_config);
    app_net_request_refresh();

    // 亮度立刻生效。息屏期间只记下新值,等按键点亮时再用——否则在设置页
    // 调亮度会把已经熄掉的屏幕点着,看着像误触。
    s_brightness = s_config.brightness_percent;
    if (!s_screen_off) bsp_display_backlight(s_brightness);

    // 串口任务也会走到这里,所以只置标志,由界面的定时器在 LVGL 上下文里重画。
    app_ui_notify_config_changed();
}

// ---------------------------------------------------------------- 按键

// 深睡按键唤醒后,芯片是"唤醒键还按着"的状态重启的。按键组件晚几百毫秒才
// 启动,它看到的是一个已经按下的电平,按住超过门限(本板 500 ms)就判成长按,
// 于是看板会在开机瞬间自己切一次页。守卫在唤醒键松手前吞掉所有按键动作,
// deadline 只是按键卡住时的兜底。状态机在 key_guard.h 里,可单独做主机测试。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 运行在 button 组件的共享 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;

    // 守卫必须在就绪检查之前消费:松手事件可能落在"按键组件初始化完成"和
    // "输入派发就绪"之间,在这里丢掉它会让守卫一直武装着,把下一次真正的
    // 按下也吞掉。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }

    // 任何一次按键都算"有操作",把息屏倒计时推后。放在守卫之后:被守卫
    // 吞掉的唤醒键松手不该算用户操作。
    s_last_input_us = esp_timer_get_time();

    // 息屏时第一下按键只负责点亮,不交给界面。否则用户看不到屏幕已经亮,
    // 会再按一次,那一下就把页面翻走了。
    if (s_screen_off) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) screen_wake();
        return;
    }

    if (!s_input_ready || s_input_queue == NULL) return;

    // 上下键走"按下即响应",不等 click。
    //
    // iot_button 的单击是在**松手之后**再等一个 short_press_time(本板 180 ms)
    // 的双击判定窗口才发出来的,所以每次翻行都固定慢 180 ms;更糟的是连按两下
    // 会被 repeat 计数并成 BUTTON_DOUBLE_CLICK,而双击我们没注册处理,那两下
    // 就整个丢了——手感就是"按了没反应,再按又跳两格"。按下事件只有 2×5 ms
    // 的消抖延迟,菜单跟手的问题在这里解决。
    if (ev == BSP_BTN_PRESS) {
        if (btn == BSP_BTN_OK) return;   // OK 要区分长按,仍旧走 click/long
        const input_event_t input = { .btn = btn, .event = BSP_BTN_PRESS };
        (void)xQueueSend(s_input_queue, &input, 0);
        return;
    }

    // 上下键的 click/double 已经没用了(按下时处理过),再处理一次会走两格。
    if (btn != BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) return;

    // OK 的双击: 映射为双击事件投递给输入队列
    if (btn == BSP_BTN_OK && ev == BSP_BTN_DOUBLE) {
        const input_event_t input = { .btn = btn, .event = BSP_BTN_DOUBLE };
        (void)xQueueSend(s_input_queue, &input, 0);
        return;
    }

    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static bool map_input(const input_event_t *input, app_input_t *out) {
    if (input->event == BSP_BTN_DOUBLE && input->btn == BSP_BTN_OK) {
        *out = APP_INPUT_OK_DOUBLE;
        return true;
    }
    if (input->event == BSP_BTN_LONG) {
        // 只有"确定"长按有意义:切页 / 从详情返回。上下的长按忽略,避免误触。
        if (input->btn != BSP_BTN_OK) return false;
        *out = APP_INPUT_OK_LONG;
        return true;
    }
    switch (input->btn) {
        case BSP_BTN_UP: *out = APP_INPUT_UP; return true;
        case BSP_BTN_DOWN: *out = APP_INPUT_DOWN; return true;
        case BSP_BTN_OK: *out = APP_INPUT_OK; return true;
        default: return false;
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        app_input_t mapped;
        if (map_input(&input, &mapped)) {
            app_ui_handle_input(mapped);
        }
    }
}

// ---------------------------------------------------------------- 电量

static void battery_task(void *arg) {
    (void)arg;
    for (;;) {
        // 读电量走共享 I2C,必须离开 LVGL 上下文;界面只负责显示。
        const int soc = bsp_battery_soc();
        const int mv = bsp_battery_mv();
        app_ui_set_battery(soc, mv);
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

// ---------------------------------------------------------------- 联网回调

// 连上热点就把这次的凭证记进"连过的网络",下次进配网页可以直接选,
// 不用再在屏上敲一遍密码。回调在联网任务里跑,可以写 NVS。
static void on_wifi_connected(const char *ssid, const char *pass) {
    const esp_err_t err = app_store_remember(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "记住网络失败:%s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "已记住网络:%s", ssid);
}

// ---------------------------------------------------------------- 启动

void app_main(void) {
    ESP_LOGI(TAG, "足球比赛看板启动");

    // 按键唤醒时那颗键在整个启动期间还按着,先武装守卫挡住它的动作(见 on_key)。
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    s_config_lock = xSemaphoreCreateMutex();
    if (s_config_lock == NULL) {
        ESP_LOGE(TAG, "配置锁创建失败");
        return;
    }

    // NVS + 配置:失败也要继续启动,界面会显示"未配网"。
    if (app_store_load(&s_config) != ESP_OK) {
        ESP_LOGW(TAG, "没有可用的已保存配置,使用默认值");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,看板无法继续。检查 SPI 接线"
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    // 背光按保存的亮度点亮;息屏倒计时从这一刻开始算。
    s_brightness = s_config.brightness_percent;
    s_last_input_us = esp_timer_get_time();
    bsp_display_backlight(s_brightness);

    // 电量是可选能力:芯片不应答时界面显示 "--%",不影响比分。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,界面将显示 --%%");
    }

    // 回调要在联网任务之前装好:开机自动连上第一个热点时也得记进历史。
    app_net_set_connected_hook(on_wifi_connected);
    app_net_start(&s_config);

    // 开机动画放在联网任务启动之后:这两秒 Wi-Fi 正在连接,动画结束时往往
    // 已经拿到 IP,所以它不额外拖慢"看到比分"的时间。
    //
    // 按键唤醒不放:用户是按了键才看到屏幕的,再让他等 2.2 秒等于按了没反应。
    if (!(wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO))) {
        app_splash_show();
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "无法获取 LVGL 锁,界面未创建");
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) {
        ESP_LOGE(TAG, "按键队列创建失败");
    // 8 KB:按键任务会重建页面(LVGL 对象)、读/写 NVS 里的配网历史,
    // 还要在栈上放一份 known_blob(约 500 字节),4~6 KB 偏紧。
    } else if (xTaskCreate(input_task, "app_input", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    } else {
        const esp_err_t err = bsp_button_init(on_key, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "按键初始化失败:%s", esp_err_to_name(err));
        } else {
            s_input_ready = true;
            if (s_key_guard.armed) {
                // 唤醒键若在按键驱动启动前就松了手,组件不会产生 release 事件,
                // 守卫会一直拦到兜底。初始化完成后按真实电压同步一次。
                const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
                key_guard_sync_released(&s_key_guard, released);
                if (!s_key_guard.armed) {
                    ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
                }
            }
        }
    }

    (void)xTaskCreate(battery_task, "app_batt", 3072, NULL, 2, NULL);
    (void)xTaskCreate(idle_task, "app_idle", 3072, NULL, 2, NULL);

    // 串口命令保留作兜底(批量刷机、远程排查),屏上配网才是主路径。
    static const app_console_hooks_t hooks = {
        .get = app_settings_get,
        .apply = app_settings_apply,
    };
    app_console_start(&hooks);

    ESP_LOGI(TAG, "就绪:配网=%s,间隔=%us,息屏=%us,亮度=%u%%",
             app_config_has_credentials(&s_config) ? "已保存" : "无",
             (unsigned)s_config.refresh_seconds,
             (unsigned)s_config.screen_off_seconds,
             (unsigned)s_config.brightness_percent);
}
