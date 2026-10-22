#!/usr/bin/env python3
"""生成开机动画的 8 帧位图(240x320),输出 main/splash_frames.c。

为什么是 L8(1 字节灰度)而不是 1bpp:
  * 1bpp(I1)最省 flash,一帧只要 9600 字节,但 LVGL 9.5 对**变量形式的索引图**
    会走 decode_indexed(),先把整幅展开成 ARGB8888(240*320*4 = 307 KB)再渲染。
    这块板没有 PSRAM,必然分配失败,画面全黑,日志里只留一句
    "No memory for indexed image"。
  * RGB565 能直接给指针,但要 150 KB/帧,8 帧 1.2 MB。
  * L8 在变量分支里是"非索引、非纯 alpha"→ 直接把指针交给渲染器,零拷贝零解码;
    混合走 l8_image_blend(纯亮度转 RGB565,没有 alpha 运算)。1 字节/像素,
    8 帧 614 KB flash、0 字节 RAM。画面本来就是纯黑白,灰度一点不浪费。

用法:
    python tools/gen_splash.py            # 写 main/splash_frames.c
    python tools/gen_splash.py --preview  # 另外往 build/ 写 PNG 便于肉眼检查
"""
from __future__ import annotations

import argparse
import math
import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent

W, H = 240, 320
STRIDE = W                     # L8:一像素一字节
FRAME_BYTES = STRIDE * H       # 76800

# ---- 场景常量(全部按 240x320 竖屏排过)------------------------------------
GOAL_L, GOAL_R = 92, 206       # 球门立柱外沿
GOAL_T, GOAL_B = 152, 292      # 横梁上沿 / 门线下沿
POST = 5                       # 立柱与横梁的粗细
NET_STEP = 14                  # 网眼间距
SIDE_W = 18                    # 右侧网的进深

BALL_R = 10
IMPACT = (152, 208)            # 球撞网的位置,后面的网面形变以它为中心
START = (-4, 318)              # 球的起点(略出画,看起来是从屏幕外飞进来的)

# 每帧停留时长(毫秒)。飞行段走快、GOAL! 段留白,才是 8-bit 手感。
#
# 这里的数字是**纯停留**,不含渲染时间:实测每帧换图后 lv_refr_now() 还要约
# 70 ms(全屏 240x320 走 SPI + 圆角遮罩),8 帧合计多出 ~560 ms。所以把每档
# 都减掉这 70 ms,真实节奏才和设计值(220/100/100/100/120/130/320/1110)对得上,
# 整段落地在 2.2 秒左右。
FRAME_MS = [150, 30, 30, 30, 50, 60, 250, 1040]

# 5x7 点阵字模,只做 GOAL! 五个字形。
GLYPHS = {
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01110"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "!": ("00100", "00100", "00100", "00100", "00100", "00000", "00100"),
}
TEXT = "GOAL!"


def new_frame() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    img = Image.new("1", (W, H), 0)
    return img, ImageDraw.Draw(img)


def draw_goal(d: ImageDraw.ImageDraw, bulge: float) -> None:
    """球门:前面一层网 + 右侧进深。bulge > 0 时网面被球顶得向右鼓。"""
    ix, iy = IMPACT

    def push(x: float, y: float) -> tuple[float, float]:
        if bulge <= 0:
            return x, y
        # 高斯衰减:离撞击点越近,网被推得越深。
        d2 = (x - ix) ** 2 + (y - iy) ** 2
        k = bulge * math.exp(-d2 / (2 * 26.0 ** 2))
        return x + k, y + k * 0.35

    # 前网:竖线与横线逐段采样,才能跟着形变弯曲。
    for gx in range(GOAL_L + POST, GOAL_R - POST + 1, NET_STEP):
        pts = [push(gx, y) for y in range(GOAL_T + POST, GOAL_B + 1, 4)]
        d.line(pts, fill=1, width=1)
    for gy in range(GOAL_T + POST, GOAL_B + 1, NET_STEP):
        pts = [push(x, gy) for x in range(GOAL_L + POST, GOAL_R - POST + 1, 4)]
        d.line(pts, fill=1, width=1)

    # 右侧进深网:从前框右上/右下往后退,再连几条竖线。
    for i in range(1, 5):
        t = i / 4.0
        x_top = GOAL_R + SIDE_W * t
        y_top = GOAL_T + 14 * t
        x_bot = GOAL_R + SIDE_W * t
        y_bot = GOAL_B - 14 * t
        d.line([(GOAL_R, GOAL_T), (x_top, y_top)], fill=1, width=1)
        d.line([(GOAL_R, GOAL_B), (x_bot, y_bot)], fill=1, width=1)
        d.line([(x_top, y_top), (x_bot, y_bot)], fill=1, width=1)

    # 立柱与横梁最后画,盖住网线,边缘才干净。
    d.rectangle([GOAL_L, GOAL_T, GOAL_L + POST - 1, GOAL_B], fill=1)
    d.rectangle([GOAL_R - POST + 1, GOAL_T, GOAL_R, GOAL_B], fill=1)
    d.rectangle([GOAL_L, GOAL_T, GOAL_R, GOAL_T + POST - 1], fill=1)


def draw_ball(d: ImageDraw.ImageDraw, cx: int, cy: int) -> None:
    """白球 + 中心黑五边形 + 外圈五块黑斑。

    1-bit 下这个尺寸(直径 18)只有靠"白底 + 中心黑块 + 边上几块黑"才读得出是
    足球;少一块就变成钥匙孔。
    """
    d.ellipse([cx - BALL_R, cy - BALL_R, cx + BALL_R, cy + BALL_R], fill=1)

    def pentagon(r: float) -> list[tuple[float, float]]:
        pts = []
        for k in range(5):
            ang = math.radians(-90 + k * 72)
            pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
        return pts

    d.polygon(pentagon(3.0), fill=0)
    for k in range(5):
        ang = math.radians(-90 + 36 + k * 72)
        px = cx + 7.4 * math.cos(ang)
        py = cy + 7.4 * math.sin(ang)
        d.rectangle([round(px) - 1, round(py) - 1, round(px) + 1, round(py) + 1], fill=0)


def trail_points(t: float, count: int, step: float) -> list[tuple[float, float]]:
    """球的抛物线轨迹:从屏幕左下往球门右上,中途拱起。t=0 起点,t=1 入网。"""
    out = []
    for i in range(count):
        s = t - i * step
        if s < 0:
            break
        x = START[0] + (IMPACT[0] - START[0]) * s
        y = START[1] - (START[1] - IMPACT[1]) * s - 55 * math.sin(math.pi * s)
        out.append((x, y))
    return out


def draw_trail(d: ImageDraw.ImageDraw, t: float) -> None:
    """拖尾:靠近球的那段实心、越远越细。1-bit 没有灰度,衰减靠粗细和点距。"""
    pts = trail_points(t, 34, 0.010)
    for i, (x, y) in enumerate(pts):
        if i == 0:
            continue
        r = 4 if i < 4 else (3 if i < 10 else (2 if i < 18 else (1 if i < 26 else 0)))
        # 尾端用棋盘格抖动,视觉上自然淡出。
        if i >= 28 and (round(x) + round(y)) % 2 == 0:
            continue
        d.rectangle([round(x) - r, round(y) - r, round(x) + r, round(y) + r], fill=1)


def draw_text(d: ImageDraw.ImageDraw, scale: int, cx: int, top: int) -> None:
    """把 5x7 字模按 scale 放大拼成 GOAL!,整体水平居中。"""
    adv = 5 * scale + scale            # 字宽 + 字距
    total = adv * len(TEXT) - scale
    x0 = cx - total // 2
    for idx, ch in enumerate(TEXT):
        rows = GLYPHS[ch]
        gx = x0 + idx * adv
        for ry, row in enumerate(rows):
            for rx, bit in enumerate(row):
                if bit == "1":
                    d.rectangle([gx + rx * scale, top + ry * scale,
                                 gx + (rx + 1) * scale - 1, top + (ry + 1) * scale - 1],
                                fill=1)


def draw_sparks(d: ImageDraw.ImageDraw, cx: int, cy: int, reach: float,
                count: int, seed: int) -> None:
    """GOAL! 周围炸开的爆点:放射状短线 + 零散星点,用固定种子保证每次生成一致。"""
    import random
    rng = random.Random(seed)
    for i in range(count):
        ang = (i / count) * math.tau + rng.uniform(-0.08, 0.08)
        r0 = reach * rng.uniform(0.78, 1.0)
        r1 = r0 + rng.uniform(6, 20)
        x0 = cx + r0 * math.cos(ang) * 1.35
        y0 = cy + r0 * math.sin(ang) * 0.78
        x1 = cx + r1 * math.cos(ang) * 1.35
        y1 = cy + r1 * math.sin(ang) * 0.78
        d.line([(x0, y0), (x1, y1)], fill=1, width=2)
        if rng.random() < 0.45:
            sx = cx + (r1 + 10) * math.cos(ang) * 1.35
            sy = cy + (r1 + 10) * math.sin(ang) * 0.78
            d.rectangle([sx - 1, sy - 1, sx + 1, sy + 1], fill=1)


def build_frames() -> list[Image.Image]:
    frames: list[Image.Image] = []

    # 1-4:飞行。bulge 保持 0,网还是平的。
    for t in (0.10, 0.40, 0.68, 0.90):
        img, d = new_frame()
        draw_goal(d, 0.0)
        draw_trail(d, t)
        x, y = trail_points(t, 1, 0.016)[0]
        draw_ball(d, round(x), round(y))
        frames.append(img)

    # 5:入网。球压在网面上,网被顶出一个小鼓包,还没有 GOAL!。
    img, d = new_frame()
    draw_goal(d, 7.0)
    x, y = trail_points(0.995, 1, 0.016)[0]
    draw_ball(d, round(x), round(y))
    frames.append(img)

    # 6:球落网底,GOAL! 从 4 倍放大弹出来,爆点还少。
    img, d = new_frame()
    draw_goal(d, 10.0)
    draw_ball(d, 150, 236)
    draw_text(d, 4, W // 2, 44)
    draw_sparks(d, W // 2, 74, 62, 10, seed=7)
    frames.append(img)

    # 7:GOAL! 撑满,爆点炸开。
    img, d = new_frame()
    draw_goal(d, 8.0)
    draw_ball(d, 150, 240)
    draw_text(d, 6, W // 2, 34)
    draw_sparks(d, W // 2, 74, 86, 16, seed=11)
    frames.append(img)

    # 8:定格。爆点散得更远、更稀,球沉到网底——留一秒给人看清。
    img, d = new_frame()
    draw_goal(d, 6.0)
    draw_ball(d, 150, 244)
    draw_text(d, 6, W // 2, 34)
    draw_sparks(d, W // 2, 74, 104, 12, seed=23)
    frames.append(img)

    return frames


def emit_c(frames: list[Image.Image]) -> str:
    lines = [
        "// main/splash_frames.c —— 由 tools/gen_splash.py 生成,不要手改。",
        "//",
        "// 8 帧 240x320 的 L8 灰度位图(LV_COLOR_FORMAT_L8,一像素一字节),",
        "// 直接从 flash 的 rodata 读,运行时不占 RAM。改画面请改脚本后重跑。",
        '#include "splash_frames.h"',
        "",
    ]
    for i, img in enumerate(frames):
        data = img.convert("L").tobytes()
        assert len(data) == FRAME_BYTES, f"frame {i} size {len(data)}"
        lines.append(f"static const uint8_t SPLASH_FRAME_{i}[SPLASH_FRAME_BYTES] =")
        # 32 字节一段:C99 只保证字符串字面量至少 4095 字符,分段拼接最稳。
        for off in range(0, len(data), 32):
            chunk = "".join(f"\\x{b:02x}" for b in data[off:off + 32])
            lines.append(f'    "{chunk}"')
        lines[-1] += ";"
        lines.append("")

    lines.append("const uint8_t *const splash_frame_data[SPLASH_FRAME_COUNT] = {")
    for i in range(len(frames)):
        lines.append(f"    SPLASH_FRAME_{i},")
    lines.append("};")
    lines.append("")
    lines.append("const uint16_t splash_frame_ms[SPLASH_FRAME_COUNT] = {")
    lines.append("    " + ", ".join(str(ms) for ms in FRAME_MS) + ",")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


HEADER = """// main/splash_frames.h —— 开机动画帧数据(由 tools/gen_splash.py 生成)。
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
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true", help="往 /tmp 写 PNG 预览")
    args = ap.parse_args()

    frames = build_frames()

    (ROOT / "main/splash_frames.h").write_text(HEADER, encoding="utf-8")
    (ROOT / "main/splash_frames.c").write_text(emit_c(frames), encoding="utf-8")

    total = FRAME_BYTES * len(frames)
    print(f"生成 {len(frames)} 帧,每帧 {FRAME_BYTES} 字节,合计 {total} 字节 "
          f"({total / 1024:.1f} KB)")

    if args.preview:
        sheet = Image.new("1", (W * 4 + 30, H * 2 + 10), 0)
        for i, img in enumerate(frames):
            sheet.paste(img, ((i % 4) * (W + 10), (i // 4) * (H + 10)))
        out = ROOT / "build/splash_preview.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(out)
        print(f"预览:{out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
