// main/app_fonts.h
// 应用专用中文字体。
//
// 字体由 tools/gen_fonts.py 从思源黑体(Noto Sans CJK SC,OFL)裁出子集:
//   12 px  联赛名/状态/页脚 —— ASCII + Latin-1 + 全部中文
//   15 px  队名/设置项     —— ASCII + Latin-1 + 全部中文
//   19 px  页头标题        —— 只有"比赛看板"四个字
//   22 px  比分数字        —— 只有数字与 "-vs"
//
// "全部中文" = GB2312 全集 + 懂球帝数据里实际出现过的非 GB2312 字
// (見 assets/fonts/data_chars.txt)。覆盖范围由 tools/check_fonts.py 校验,
// 改了界面文案必须重新生成。
#pragma once

#include "lvgl.h"

#include <stddef.h>

LV_FONT_DECLARE(app_font_12);
LV_FONT_DECLARE(app_font_15);
LV_FONT_DECLARE(app_font_19);
LV_FONT_DECLARE(app_font_22);

#define APP_FONT_SMALL (&app_font_12)
#define APP_FONT_BODY (&app_font_15)
#define APP_FONT_TITLE (&app_font_19)
#define APP_FONT_SCORE (&app_font_22)

// 把字体没有字形的码点替换成 '?'。
//
// 这是本项目对"动态文本"的明确策略:接口返回的队名如果带了子集之外的字
// (罕见地名、日文汉字、其它语种字母),界面显示 '?' 而不是一个空心方框,
// 也不会静默吞掉字符。界面所有来自网络的文本都先过这一道。
const char *app_fonts_sanitize(const char *src, char *dst, size_t cap);
