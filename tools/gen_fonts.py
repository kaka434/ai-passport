#!/usr/bin/env python3
"""生成看板使用的中文字体子集。

字符清单由三部分并起来:

1. 源码字符串字面量里的非 ASCII 字符(main/ 下的 *.[ch]),保证界面上的
   静态文案一定有字形;
2. GB2312 全集,保证接口随时冒出来的新队名/新联赛名不会缺字;
3. assets/fonts/data_chars.txt —— 从懂球帝真实响应里扫出来的字,
   用来补上 GB2312 覆盖不到的那部分(日文汉字、繁体异体字等)。

三部分会落盘成 assets/fonts/football_ui_chars.txt,供 tools/check_fonts.py
校验。改了界面文案之后重跑本脚本即可,不需要手工维护字符表。

字号分工(见 main/app_fonts.h):
  12 / 15 px  完整字符集(队名、联赛名、状态、设置项)
  19 px       只有源码字面量里的字(页头标题)
  22 px       只有数字与 "-vs"(比分)

依赖:
  * 思源黑体 Noto Sans SC(OFL-1.1),默认读 $NOTO_SANS_SC 或
    assets/fonts/source/NotoSansSC-Regular.otf
  * lv_font_conv(https://github.com/lvgl/lv_font_conv),通过 $LV_FONT_CONV
    指定可执行文件,默认取 PATH 里的 lv_font_conv

用法:
  python3 tools/gen_fonts.py --font /path/to/NotoSansSC-Regular.otf
  python3 tools/gen_fonts.py --data-json /path/to/match_list.json   # 顺带刷新 data_chars.txt
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"
OUT_DIR = ROOT / "assets" / "fonts"

# lv_font_conv 只在文件开头写这几行注释,里面带绝对路径,需要洗掉(见 scrub_header_paths)。
PATH_HEADER_LINES = 8

# 界面文字用到的标点。ASCII 与 Latin-1 由 -r 范围覆盖,这里只补全角/中文标点。
EXTRA_PUNCTUATION = "，。、：；！？（）《》—…·“”‘’"

# 比分区字号只需要这些字符。
SCORE_SYMBOLS = "0123456789-vs"

TEXT_SIZES = (12, 15)
TITLE_SIZE = 19
SCORE_SIZE = 22

# 正文字号除了中文还要覆盖西文。Latin-1 之外再带上 Latin Extended-A:
# 懂球帝的队名里有土耳其语、捷克语之类的写法(如 "Beşiktaş" 的 ş)。
# 注意思源黑体本身只覆盖 Latin Extended-A 的一部分,所以这一区间是"尽量给",
# 校验时只强制要求 ASCII + Latin-1 + 实际用到的字符。
FULL_LATIN_RANGES = ("0x20-0x7E", "0xA0-0x17F")
# 校验时真正强制的西文范围。
REQUIRED_LATIN_RANGES = ("0x20-0x7E", "0xA0-0xFF")
# 标题只有"足球比分"四个字,西文给到 Latin-1 就够。
TITLE_LATIN_RANGES = REQUIRED_LATIN_RANGES


def latin_codepoints(ranges: tuple[str, ...]) -> set[int]:
    """把 ["0x20-0x7E", ...] 展开成码点集合。"""
    out: set[int] = set()
    for item in ranges:
        low, high = item.split("-")
        out.update(range(int(low, 16), int(high, 16) + 1))
    return out


def string_literals(source: str) -> list[str]:
    """粗略提取 C 字符串字面量;注释里的中文不计入,避免子集虚胖。"""
    literals: list[str] = []
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        if ch == "/" and i + 1 < n and source[i + 1] == "/":
            i = source.find("\n", i)
            if i < 0:
                break
            continue
        if ch == "/" and i + 1 < n and source[i + 1] == "*":
            end = source.find("*/", i + 2)
            if end < 0:
                break
            i = end + 2
            continue
        if ch == '"':
            i += 1
            buf: list[str] = []
            while i < n and source[i] != '"':
                if source[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                buf.append(source[i])
                i += 1
            literals.append("".join(buf))
            i += 1
            continue
        i += 1
    return literals


def app_sources() -> list[Path]:
    """本应用真正编进固件的源码。

    main/ 下还留着基线的 demo_*.c / ui_pixel*.c(静态检查会编译它们做故障
    注入测试),但它们的文案不会显示在看板上,所以不参与字符收集。
    """
    return [path for path in sorted(MAIN_DIR.glob("*.[ch]"))
            if path.name == "main.c" or path.name.startswith("app_")]


def ui_characters() -> set[str]:
    """界面静态文案用到的字符。19 px 标题字体只要这一份。"""
    chars: set[str] = set()
    for path in app_sources():
        for literal in string_literals(path.read_text(encoding="utf-8")):
            for ch in literal:
                # 只收非 ASCII:ASCII/Latin-1 交给 -r 范围。
                if ord(ch) > 0xFF:
                    chars.add(ch)
    chars.update(EXTRA_PUNCTUATION)
    return chars


def gb2312_characters() -> set[str]:
    """GB2312 全集(6763 汉字 + 682 符号)。

    这是简体中文的"够用"边界:队名、联赛名里出现的常用字几乎全在里面,
    而且是按编码枚举出来的确定集合,不依赖某一次抓包。
    """
    chars: set[str] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes((high, low)).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def data_characters() -> set[str]:
    """从真实响应里扫出来的 CJK 字符(GB2312 之外的补充)。

    只收 CJK 区段:思源黑体的西文只覆盖 Latin-1(外加少量 Latin Extended-A),
    土耳其语这类写法(实测有一条队名叫 "Akşehirspor",ş = U+015F)在
    任何一版 Noto Sans CJK 里都没有字形。与其把它写进"必须覆盖"的清单让
    门禁永远红着,不如明确降级:界面上由 app_fonts_sanitize 显示成 '?'。
    """
    path = OUT_DIR / "data_chars.txt"
    if not path.is_file():
        return set()
    return {ch for ch in path.read_text(encoding="utf-8") if is_cjk(ord(ch))}


# 思源黑体确实有字形的 CJK 区段(含中文标点、假名、全角形式)。
CJK_RANGES = (
    (0x2E80, 0x2EFF),   # CJK 部首补充
    (0x3000, 0x303F),   # CJK 标点
    (0x3040, 0x30FF),   # 平假名 / 片假名
    (0x3400, 0x4DBF),   # CJK 扩展 A
    (0x4E00, 0x9FFF),   # CJK 基本区
    (0xF900, 0xFAFF),   # 兼容汉字
    (0xFF00, 0xFFEF),   # 全角形式
)


def is_cjk(cp: int) -> bool:
    return any(low <= cp <= high for low, high in CJK_RANGES)


def collect_characters() -> str:
    """12/15 px 用的完整字符集(排序后的字符串)。"""
    chars = ui_characters() | gb2312_characters() | data_characters()
    return "".join(sorted(chars))


def title_characters() -> str:
    """19 px 用的字符集:只有界面静态文案。"""
    return "".join(sorted(ui_characters()))


def refresh_data_chars(json_path: Path) -> int:
    """从一份懂球帝 match_list 响应里扫出联赛名/队名用到的字。"""
    payload = json.loads(json_path.read_text(encoding="utf-8"))
    matches = payload.get("data", {}).get("matches")
    if matches is None:
        matches = payload.get("matches", [])

    chars: set[str] = set()
    for match in matches:
        for text in (
            match.get("competition", {}).get("name", ""),
            match.get("team_A", {}).get("name", ""),
            match.get("team_B", {}).get("name", ""),
        ):
            chars.update(ch for ch in text if ord(ch) > 0xFF)

    target = OUT_DIR / "data_chars.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("".join(sorted(chars)) + "\n", encoding="utf-8")
    print(f"data_chars.txt:{len(matches)} 场比赛,{len(chars)} 个非 ASCII 字")
    return 0


def scrub_header_paths(path: Path) -> None:
    """把 lv_font_conv 抄进文件头的绝对路径改回仓库相对路径。

    这个工具会把 argv 原样写进注释,里面带着 --font 与 -o 的完整路径。直接提交
    就等于把构建机的用户名和目录结构写进公开仓库,而且同一套字体在不同机器上
    生成的文件无法逐字节比对。注释只在文件开头,字体数据本身不含路径,所以只
    改写头部几行,数据一个字节都不动。
    """
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    head = "".join(lines[:PATH_HEADER_LINES])

    fixed = head
    for prefix in (str(ROOT), str(ROOT).replace("\\", "/")):
        fixed = fixed.replace(prefix + "\\", "").replace(prefix + "/", "")
    fixed = fixed.replace("\\", "/")

    if fixed == head:
        return
    lines[:PATH_HEADER_LINES] = fixed.splitlines(keepends=True)
    path.write_text("".join(lines), encoding="utf-8")


def run_convert(converter: str, font: Path, size: int, symbols: str, name: str,
                out_path: Path, latin_ranges: tuple[str, ...] | None = None) -> None:
    cmd = shlex.split(converter) + [
        "--font", str(font),
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--no-kerning",
        "--lv-font-name", name,
        "--lv-include", "lvgl.h",
    ]
    for item in latin_ranges or ():
        cmd += ["-r", item]
    cmd += ["--symbols", symbols, "-o", str(out_path)]
    print(f"  {name}: {size}px, {len(symbols)} 个字符")
    subprocess.run(cmd, check=True)
    scrub_header_paths(out_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default=os.environ.get(
        "NOTO_SANS_SC", str(OUT_DIR / "source" / "NotoSansSC-Regular.otf")))
    parser.add_argument("--converter", default=os.environ.get("LV_FONT_CONV", "lv_font_conv"))
    parser.add_argument("--data-json", default=None,
                        help="懂球帝 match_list 响应,用于刷新 data_chars.txt")
    parser.add_argument("--chars-only", action="store_true",
                        help="只刷新字符清单,不调用 lv_font_conv")
    args = parser.parse_args(argv)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if args.data_json:
        refresh_data_chars(Path(args.data_json))

    symbols = collect_characters()
    title = title_characters()
    print(f"完整字符集 {len(symbols)} 个;标题字符集 {len(title)} 个")

    # 字符清单落盘,便于 review 与覆盖校验。
    (OUT_DIR / "football_ui_chars.txt").write_text(symbols + "\n", encoding="utf-8")
    (OUT_DIR / "title_chars.txt").write_text(title + "\n", encoding="utf-8")
    if args.chars_only:
        return 0

    font = Path(args.font)
    if not font.is_file():
        print(f"找不到字体文件:{font}", file=sys.stderr)
        return 1

    converter = args.converter
    executable = shlex.split(converter)[0]
    if shutil.which(executable) is None and not Path(executable).is_file():
        print(f"找不到 lv_font_conv:{converter}", file=sys.stderr)
        return 1

    for size in TEXT_SIZES:
        run_convert(converter, font, size, symbols, f"app_font_{size}",
                    OUT_DIR / f"app_font_{size}.c", FULL_LATIN_RANGES)
    run_convert(converter, font, TITLE_SIZE, title, "app_font_19",
                OUT_DIR / "app_font_19.c", TITLE_LATIN_RANGES)
    # 比分区只画数字和几个符号,不需要整段 ASCII,省下来的 Flash 很可观。
    run_convert(converter, font, SCORE_SIZE, SCORE_SYMBOLS, "app_font_22",
                OUT_DIR / "app_font_22.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
