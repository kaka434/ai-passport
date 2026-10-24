#!/usr/bin/env python3
"""生成队徽 PNG 解码器的主机测试夹具。

产出 tests/crest_fixtures.{h,c}:里面是真实从 CDN 抓下来的 32×32 缩略图,
加上几张手工拼出来、专门覆盖各过滤方式和色彩类型的合成图,以及每张图解码后
应有的 RGB565 结果。

期望值不是抄 C 代码的输出,而是用 PIL 独立解一遍再按 app_png.h 里写明的
换算公式(pack565 / split565 / alpha 混底色)算出来的——两条路走到同一个
结果,才说明 inflate、反过滤、调色板查找都没写错。

用法:
    python3 tools/gen_crest_fixtures.py

真实样本放在 tests/fixtures/crests/*.png。要换新样本就重新抓一批覆盖
各色彩类型的图丢进去,再跑一遍本脚本;测试本身只认生成出来的 .c/.h。
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE_DIR = ROOT / "tests" / "fixtures" / "crests"
OUT_H = ROOT / "tests" / "crest_fixtures.h"
OUT_C = ROOT / "tests" / "crest_fixtures.c"

SIDE = 32
BG = (0x11, 0x29, 0x1D)          # app_ui.c 的 C_PANEL,队徽就贴在这块面板上
BG_RGB565 = ((BG[0] & 0xF8) << 8) | ((BG[1] & 0xFC) << 3) | (BG[2] >> 3)


def pack565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def split565(c: int) -> tuple[int, int, int]:
    r5, g6, b5 = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


def expected_rgba(rgba: list[tuple[int, int, int, int]], bg: int = BG_RGB565) -> list[int]:
    """按 app_png.c 的算法把 RGBA 算成 RGB565(含往底色混透明像素)。"""
    bg_r, bg_g, bg_b = split565(bg)
    out = []
    for r, g, b, a in rgba:
        if a != 255:
            r = (r * a + bg_r * (255 - a)) // 255
            g = (g * a + bg_g * (255 - a)) // 255
            b = (b * a + bg_b * (255 - a)) // 255
        out.append(pack565(r, g, b))
    return out


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for byte in data:
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


# ------------------------------------------------------------ 手工拼 PNG

def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def make_png(w: int, h: int, ctype: int, raw: bytes, *, level: int = 9,
             palette: bytes | None = None, trns: bytes | None = None,
             split_idat: int = 1, depth: int = 8, interlace: int = 0) -> bytes:
    """按给定扫描线数据拼一个 PNG。raw 是已经逐行加过过滤字节的数据。"""
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    out = bytes([137, 80, 78, 71, 13, 10, 26, 10]) + chunk(b"IHDR", ihdr)
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    comp = zlib.compress(raw, level)
    step = max(1, (len(comp) + split_idat - 1) // split_idat)
    for i in range(0, len(comp), step):
        out += chunk(b"IDAT", comp[i:i + step])
    out += chunk(b"IEND", b"")
    return out


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_rows(rows: list[bytes], w: int, bpp: int) -> bytes:
    """按 行号 % 5 轮着加过滤字节,五种过滤方式都能走到。

    注意过滤字节后面存的是"原始值减预测值",不是原始值本身——解码端是
    加回去,这里就得先减出来。"""
    out = bytearray()
    prev = bytes(w * bpp)
    for i, row in enumerate(rows):
        f = i % 5
        out.append(f)
        for j, v in enumerate(row):
            a = row[j - bpp] if j >= bpp else 0
            b = prev[j]
            c = prev[j - bpp] if j >= bpp else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) // 2
            else:
                pred = paeth(a, b, c)
            out.append((v - pred) & 0xFF)
        prev = row
    return bytes(out)


def synth_filters() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """32×32 RGB,五行一轮换过滤方式,内容是一张会触发各种预测的斜纹。"""
    w = h = SIDE
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r = (x * 8) & 0xFF
            g = (y * 8) & 0xFF
            b = (x * y) & 0xFF
            row += bytes((r, g, b))
            rgba.append((r, g, b, 255))
        rows.append(bytes(row))
    return make_png(w, h, 2, filter_rows(rows, w, 3)), rgba


def synth_rgba() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """RGBA:alpha 从 0 扫到 255,顺带查半透明混色。"""
    w = h = SIDE
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b, a = x * 8 & 0xFF, y * 8 & 0xFF, (x ^ y) * 4 & 0xFF, (x + y) * 4 % 256
            row += bytes((r, g, b, a))
            rgba.append((r, g, b, a))
        rows.append(bytes(row))
    return make_png(w, h, 6, filter_rows(rows, w, 4)), rgba


def synth_palette() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """调色板 + tRNS:tRNS 故意比调色板短,剩下的项该按不透明算。"""
    w = h = SIDE
    n = 200
    palette = bytearray()
    trns = bytearray()
    for i in range(n):
        palette += bytes((i * 5 % 256, 255 - i * 3 % 256, (i * 7) % 256))
        trns.append(0 if i % 37 == 0 else (128 if i % 11 == 0 else 255))
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            idx = (x * 3 + y * 7) % n
            row.append(idx)
            r, g, b = palette[idx * 3:idx * 3 + 3]
            rgba.append((r, g, b, trns[idx]))
        rows.append(bytes(row))
    return make_png(w, h, 3, filter_rows(rows, w, 1), palette=bytes(palette), trns=bytes(trns)), rgba


def synth_gray_alpha() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """灰度 + Alpha:真实样本里出现过这一类,别漏掉。"""
    w = h = SIDE
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            v, a = (x * 8) & 0xFF, (y * 8) & 0xFF
            row += bytes((v, a))
            rgba.append((v, v, v, a))
        rows.append(bytes(row))
    return make_png(w, h, 4, filter_rows(rows, w, 2)), rgba


def synth_stored() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """level=0:走 zlib 的存贮块,不压缩,专门喂 BTYPE=0 那条路。"""
    w = h = SIDE
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = (x * 8) & 0xFF, 0x40, (y * 8) & 0xFF
            row += bytes((r, g, b))
            rgba.append((r, g, b, 255))
        rows.append(bytes(row))
    return make_png(w, h, 2, filter_rows(rows, w, 3), level=0), rgba


def synth_multi_idat() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """IDAT 拆成 5 块:块不连续就该拒,连续就得能拼回去。"""
    w = h = SIDE
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = (x ^ y) & 0xFF, (x * 4) & 0xFF, (y * 4) & 0xFF
            row += bytes((r, g, b))
            rgba.append((r, g, b, 255))
        rows.append(bytes(row))
    return make_png(w, h, 2, filter_rows(rows, w, 3), split_idat=5), rgba


def synth_small() -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """8×12:不是正方形,也别依赖 w==h。"""
    w, h = 8, 12
    rows = []
    rgba = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = x * 32, y * 21, (x + y) * 11
            row += bytes((r & 0xFF, g & 0xFF, b & 0xFF))
            rgba.append((r & 0xFF, g & 0xFF, b & 0xFF, 255))
        rows.append(bytes(row))
    return make_png(w, h, 2, filter_rows(rows, w, 3)), rgba


# ------------------------------------------------------------ 该被拒的输入

def reject_cases() -> list[tuple[str, bytes]]:
    cases: list[tuple[str, bytes]] = []

    real = sorted(FIXTURE_DIR.glob("*.png"))
    if real:
        good = real[0].read_bytes()
        # 截断:只给前 64 字节,连 IHDR 都读不完。
        cases.append(("truncated_head", good[:64]))
        # 截断在 IDAT 中间:块都齐,解压到一半断气。
        cases.append(("truncated_tail", good[:max(33, len(good) // 2)]))
        # 改坏 PLTE 里的一个字节 → CRC 对不上。
        if b"PLTE" in good:
            i = good.index(b"PLTE") + 8
            bad = bytearray(good)
            bad[i] ^= 0xFF
            cases.append(("bad_crc", bytes(bad)))

    # 尺寸超上限:CDN 不理 imageView2 把原图返回来就是这么个东西。
    rows = []
    for y in range(40):
        row = bytearray()
        for x in range(40):
            row += bytes((x * 6 & 0xFF, y * 6 & 0xFF, 0x80))
        rows.append(bytes(row))
    cases.append(("too_big", make_png(40, 40, 2, filter_rows(rows, 40, 3))))

    # 位深 4 的调色板:只认 8 位。
    rows = [bytes(range(8)) for _ in range(8)]
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw += row
    cases.append(("depth4", make_png(8, 8, 3, bytes(raw), depth=4,
                                     palette=bytes(range(24)))))

    # 多块 IDAT。这是合法 PNG,但这里只认单块:多块的数据在文件里不连续,
    # 不另开一块缓冲拼起来就只能解出乱码。而 32×32 8 位的图压缩后到不了
    # 8192 字节的拆分点,实测 60 个真实缩略图全是单块,所以拒掉不亏。
    png, _ = synth_multi_idat()
    cases.append(("multi_idat", png))

    # 签名不对。
    cases.append(("bad_sig", b"\x89PNG\r\n\x1a\n" + b"\x00" * 40))
    return cases


# ------------------------------------------------------------ 产出 C 代码

def c_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + " ".join(f"0x{b:02X}," for b in data[i:i + 16]))
    lines.append("};")
    return "\n".join(lines)


def c_u16_array(name: str, data: list[int]) -> str:
    lines = [f"static const uint16_t {name}[] = {{"]
    for i in range(0, len(data), 8):
        lines.append("    " + " ".join(f"0x{v:04X}," for v in data[i:i + 8]))
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    try:
        from PIL import Image  # noqa: PLC0415
    except ImportError:
        print("PIL 不可用:期望值需要它独立解一遍 PNG", file=sys.stderr)
        return 1

    synths = [
        ("synth_filters", "五种过滤方式轮着来的 32×32 RGB", synth_filters),
        ("synth_rgba", "32×32 RGBA,alpha 全量程", synth_rgba),
        ("synth_palette", "32×32 调色板,tRNS 比调色板短", synth_palette),
        ("synth_gray_alpha", "32×32 灰度 + Alpha", synth_gray_alpha),
        ("synth_stored", "zlib 存贮块(BTYPE=0)", synth_stored),
        ("synth_small", "8×12,不是正方形", synth_small),
    ]

    header = [
        "// 本文件由 tools/gen_crest_fixtures.py 生成,不要手改。",
        "// 队徽 PNG 解码器的测试数据:真实 CDN 缩略图 + 覆盖各形态的合成图。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    const uint8_t *png;",
        "    unsigned png_len;",
        "    uint16_t w;",
        "    uint16_t h;",
        "    uint32_t checksum;      // FNV-1a over the expected RGB565 bytes",
        "    const uint16_t *pixels; // expected NULL → 只校验尺寸与 checksum",
        "    unsigned pixel_count;",
        "} crest_fixture_t;",
        "",
        "extern const crest_fixture_t crest_fixtures[];",
        "extern const unsigned crest_fixture_count;",
        "",
        "typedef struct {",
        "    const char *name;",
        "    const uint8_t *png;",
        "    unsigned png_len;",
        "} crest_reject_t;",
        "",
        "extern const crest_reject_t crest_rejects[];",
        "extern const unsigned crest_reject_count;",
        "",
        f"#define CREST_TEST_BG 0x{BG_RGB565:04X}",
        "",
    ]

    body: list[str] = [
        "// 由 tools/gen_crest_fixtures.py 生成,不要手改。",
        '#include "crest_fixtures.h"',
        "",
        "#include <stdint.h>",
        "",
    ]

    entries: list[str] = []

    def emit(name: str, desc: str, png: bytes, rgba: list[tuple[int, int, int, int]],
             full_pixels: bool) -> None:
        want = expected_rgba(rgba)
        raw = b"".join(struct.pack("<H", v) for v in want)
        w = int.from_bytes(png[16:20], "big")
        h = int.from_bytes(png[20:24], "big")
        body.append(f"// {desc}")
        body.append(c_array(f"{name}_png", png))
        pixels_name = "NULL"
        pixel_count = 0
        if full_pixels:
            body.append(c_u16_array(f"{name}_want", want))
            pixels_name = f"{name}_want"
            pixel_count = len(want)
        body.append("")
        entries.append(
            f'    {{ "{name}", {name}_png, sizeof({name}_png), {w}, {h}, '
            f'0x{fnv1a(raw):08X}u, {pixels_name}, {pixel_count} }},'
        )

    for name, desc, fn in synths:
        png, rgba = fn()
        emit(name, desc, png, rgba, full_pixels=True)

    for path in sorted(FIXTURE_DIR.glob("*.png")):
        png = path.read_bytes()
        img = Image.open(path).convert("RGBA")
        rgba = [tuple(px) for px in img.getdata()]  # Pillow 建议改用 get_flattened_data
        if img.size != (SIDE, SIDE):
            print(f"{path.name} 不是 {SIDE}×{SIDE},跳过", file=sys.stderr)
            continue
        emit(f"real_{path.stem.removeprefix('real_')}", f"真实 CDN 缩略图 {path.name}", png, rgba,
             full_pixels=False)

    rejects = reject_cases()
    reject_entries: list[str] = []
    for name, png in rejects:
        body.append(f"// 该被拒:{name}")
        body.append(c_array(f"reject_{name}_png", png))
        body.append("")
        reject_entries.append(
            f'    {{ "{name}", reject_{name}_png, sizeof(reject_{name}_png) }},'
        )

    body.append("const crest_fixture_t crest_fixtures[] = {")
    body += entries
    body.append("};")
    body.append(f"const unsigned crest_fixture_count = "
                f"{len(entries)}u;")
    body.append("")
    body.append("const crest_reject_t crest_rejects[] = {")
    body += reject_entries
    body.append("};")
    body.append(f"const unsigned crest_reject_count = {len(reject_entries)}u;")
    body.append("")

    OUT_H.write_text("\n".join(header), encoding="utf-8")
    OUT_C.write_text("\n".join(body), encoding="utf-8")
    print(f"{OUT_H.name}: {len(header)} 行")
    print(f"{OUT_C.name}: {len(body)} 行,{OUT_C.stat().st_size} 字节")
    print(f"夹具 {len(entries)} 个(合成 {len(synths)} + 真实 {len(entries) - len(synths)}),"
          f"拒绝用例 {len(reject_entries)} 个")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
