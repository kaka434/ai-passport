// main/splash_frames.h —— 开机动画帧数据(由 tools/gen_splash.py 生成)。
#pragma once

#include <stdint.h>

#define SPLASH_FRAME_W 240
#define SPLASH_FRAME_H 320
#define SPLASH_FRAME_STRIDE 240
#define SPLASH_FRAME_BYTES 76800
#define SPLASH_FRAME_COUNT 8

// 每帧的 L8 灰度位图(一像素一字节,0=黑 255=白)。
// 总占用 SPLASH_FRAME_COUNT * 76800 字节 = 600 KB flash。
extern const uint8_t *const splash_frame_data[SPLASH_FRAME_COUNT];

// 每帧停留时长(毫秒),合计 2200 ms。
extern const uint16_t splash_frame_ms[SPLASH_FRAME_COUNT];
