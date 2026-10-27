// main/app_crest.h —— 队徽的下载、解码与缓存。
//
// 一场比赛两个队,所以只有两个槽位。槽位按 URL 记忆:同一场比赛反复进出
// 详情页不会把 CDN 再打一遍;换了一场就整个换掉。
//
// 线程模型:请求方是界面任务(只改状态、不动网络),真正下载和解码的是
// app_net 的详情任务——见 app_crest_pump()。之所以不自己开任务:那条 8 KB
// 的栈是从 TLS 握手的堆余量里硬扣的,而队徽只在用户点开比赛时才需要,
// 那时详情任务本来就在,复用它一份内存都不多花。
#pragma once

#include "app_png.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_CREST_SLOTS 3
#define APP_CREST_SLOT_HOME 0
#define APP_CREST_SLOT_AWAY 1
#define APP_CREST_SLOT_FAV  2

// 队徽背后的面板色,和 app_ui.c 的 C_PANEL(0x11291D)同值,换算成 RGB565。
//
// 解码时就把透明像素按这个颜色混掉,而不是留到绘制时做 alpha 合成:LVGL 的
// RGB565 图不带 alpha 通道,混晚了队徽四角会出一圈黑边。放在这里而不是当参数
// 传,是因为它只由"贴在哪块面板上"决定,让调用方各传一个反而多一个能写错的
// 地方——这个值错了画面只是难看,但两份不一致会很难查。
#define APP_CREST_BG_RGB565 0x1143u

// 要求某个槽位显示 logo_url 对应的队徽。
//
// logo_url 为 NULL 或空串表示"这场没有队徽 URL"(未开赛的接口经常整个
// statistics 都是 []),槽位直接作废,界面画占位块。
//
// 同一个 URL 已经有结果、或者正在下载时,这个调用是空操作。下载失败过的
// 槽位例外:再请求一次会重试,免得一次网络抖动就让这场球整场没队徽。
//
// 由 app_net 在详情拉成功之后调用(队徽 URL 就在详情响应里),界面这边只读
// app_crest_image() / app_crest_revision()。
void app_crest_request(int slot, const char *logo_url);

// 取槽位当前解好的队徽;没有(没请求过 / 还在下 / 失败了)返回 NULL。
const app_png_image_t *app_crest_image(int slot);

// 版本号。任何槽位状态变化(开始下、下完、失败、作废)都 +1,
// 界面据此决定要不要重建详情页。
uint32_t app_crest_revision(void);

// 还有槽位在等图。界面可以据此显示"队徽加载中",而不是画一块空占位。
bool app_crest_busy(void);

// 干一份活:挑一个在等的槽位,下载 + 解码,更新状态,返回 true。
// 一份都不剩时返回 false。由 app_net 的详情任务调用,不要在界面任务里调
// ——它内部会阻塞在 TLS 上。
bool app_crest_pump(void);
