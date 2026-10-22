// main/app_splash.c —— 冷启动的 GOAL 开机动画播放器。
//
// 8 帧 240x320 的 L8 灰度位图存在 flash 的 rodata 里(splash_frames.c),这里
// 只负责按时序换帧。
//
// 为什么是 L8 而不是更省的 1bpp:LVGL 9.5 对**变量形式的索引图**会走
// decode_indexed(),先把整幅展开成 ARGB8888(240x320x4 = 307 KB)再交给渲染器。
// 这块板没有 PSRAM,必然分配失败,画面全黑,日志里只留一句
// "No memory for indexed image"。L8 走的是同一条分支里的"非索引格式"兜底:
// 直接把 rodata 指针交给渲染器,零拷贝零解码,混合走 l8_image_blend。
//
// 内存:帧数据在 flash,运行时不占堆也不占 LVGL 的池子——开机画面因此不可能
// 因为分配失败把设备卡死(LV_USE_ASSERT_MALLOC 下是断言,不是返回 NULL)。
#include "app_splash.h"

#include "splash_frames.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "splash";

// 每帧一个描述符。lv_image_dsc_t 要求 data 指针在对象生命周期内有效,
// 所以描述符放静态区,而不是在函数栈上临时拼。
static lv_image_dsc_t s_desc[SPLASH_FRAME_COUNT];
static bool s_desc_ready;

static void build_descriptors(void) {
    if (s_desc_ready) return;
    for (int i = 0; i < SPLASH_FRAME_COUNT; i++) {
        lv_image_dsc_t *d = &s_desc[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_L8;
        d->header.flags = 0;
        d->header.w = SPLASH_FRAME_W;
        d->header.h = SPLASH_FRAME_H;
        d->header.stride = SPLASH_FRAME_STRIDE;
        d->data_size = SPLASH_FRAME_BYTES;
        d->data = splash_frame_data[i];
    }
    s_desc_ready = true;
}

void app_splash_show(void) {
    build_descriptors();

    if (!bsp_lvgl_lock(1000)) {
        // 拿不到锁就跳过动画。开机画面是锦上添花,不能因为它挡住主界面。
        ESP_LOGW(TAG, "取不到 LVGL 锁,跳过开机动画");
        return;
    }

    lv_obj_t *prev = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *img = (scr != NULL) ? lv_image_create(scr) : NULL;
    if (img == NULL) {
        if (scr != NULL) lv_obj_delete(scr);
        bsp_lvgl_unlock();
        ESP_LOGW(TAG, "开机画面创建失败,跳过");
        return;
    }

    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_image_set_src(img, &s_desc[0]);
    lv_obj_center(img);
    lv_screen_load(scr);
    // 先把第 1 帧推上屏再进循环,否则第一帧要等下一个 LVGL 周期,用户会先
    // 看到一帧默认白屏。
    lv_refr_now(NULL);
    bsp_lvgl_unlock();

    const int64_t started = esp_timer_get_time();
    for (int i = 0; i < SPLASH_FRAME_COUNT; i++) {
        if (i > 0) {
            if (!bsp_lvgl_lock(500)) break;
            lv_image_set_src(img, &s_desc[i]);
            lv_refr_now(NULL);
            bsp_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(splash_frame_ms[i]));
    }
    ESP_LOGI(TAG, "开机动画播放完毕,%d 帧 %d ms", SPLASH_FRAME_COUNT,
             (int)((esp_timer_get_time() - started) / 1000));

    // 回到原来的屏幕再删动画屏:直接删当前活动屏,LVGL 会立刻切到别的屏,
    // 顺序上容易和随后 app_ui_init() 的建屏撞在一起。
    if (bsp_lvgl_lock(1000)) {
        lv_screen_load(prev);
        lv_obj_delete(scr);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGW(TAG, "收尾取不到 LVGL 锁,动画屏留给界面覆盖");
    }
}
