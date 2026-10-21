#!/usr/bin/env python3
"""校验生成的中文字体是否覆盖界面实际会显示的每一个字符。

检查四件事:
  1. 字符清单(assets/fonts/football_ui_chars.txt)与当前源码 + GB2312 +
     data_chars.txt 算出来的集合仍然一致,防止改了文案却忘记重新生成字体;
  2. 12/15 px 覆盖 ASCII、Latin-1 与完整字符集;
  3. 19 px 覆盖源码字面量里的字(页头标题);
  4. 22 px 覆盖数字与 "-vs"。

字体文件是 lv_font_conv 生成的 C 源码,这里直接读它的 cmap 表判断覆盖,
不依赖 LVGL 运行时,也不需要设备。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_fonts  # noqa: E402  (同目录脚本,复用字符收集逻辑)

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
INVENTORY = FONT_DIR / "football_ui_chars.txt"
TITLE_INVENTORY = FONT_DIR / "title_chars.txt"

FULL_FONTS = ("app_font_12", "app_font_15")
TITLE_FONT = "app_font_19"
SCORE_FONT = "app_font_22"
SCORE_SYMBOLS = gen_fonts.SCORE_SYMBOLS

FULL_LATIN = gen_fonts.latin_codepoints(gen_fonts.REQUIRED_LATIN_RANGES)
TITLE_LATIN = gen_fonts.latin_codepoints(gen_fonts.TITLE_LATIN_RANGES)


def parse_coverage(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")

    lists: dict[str, list[int]] = {}
    for match in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};",
                             text, re.S):
        lists[match.group(1)] = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", match.group(2))]

    table = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if table is None:
        raise SystemExit(f"{path.name}: 找不到 cmaps 表")

    covered: set[int] = set()
    for entry in re.findall(r"\{(.*?)\}", table.group(1), re.S):
        start = int(re.search(r"\.range_start\s*=\s*(\d+)", entry).group(1))
        length = int(re.search(r"\.range_length\s*=\s*(\d+)", entry).group(1))
        kind = re.search(r"\.type\s*=\s*(\w+)", entry).group(1)
        list_name = re.search(r"\.unicode_list\s*=\s*(\w+)", entry).group(1)

        if "SPARSE" in kind:
            # LVGL 的稀疏表存的是相对 range_start 的绝对偏移(16 位),不是增量。
            for value in lists.get(list_name, []):
                covered.add(start + value)
        else:
            covered.update(range(start, start + length))
    return covered


def report_missing(name: str, required: set[int], covered: set[int]) -> list[str]:
    missing = sorted(required - covered)
    if not missing:
        return []
    shown = ", ".join(f"U+{cp:04X}" for cp in missing[:12])
    more = "" if len(missing) <= 12 else f" …共 {len(missing)} 个"
    return [f"{name}: 缺少字形 {shown}{more}"]


def check_inventory(path: Path, expected: str, label: str, errors: list[str]) -> str:
    if not path.is_file():
        errors.append(f"缺少{label}清单:{path.name},先运行 tools/gen_fonts.py")
        return ""
    stored = path.read_text(encoding="utf-8").strip()
    if stored != expected:
        added = sorted(set(expected) - set(stored))
        removed = sorted(set(stored) - set(expected))
        detail = []
        if added:
            detail.append("新增 " + "".join(added[:40]))
        if removed:
            detail.append("多余 " + "".join(removed[:40]))
        errors.append(f"{label}清单已过期(" + ";".join(detail) + "),请重新运行 tools/gen_fonts.py")
    return stored


def main() -> int:
    errors: list[str] = []

    inventory = check_inventory(INVENTORY, gen_fonts.collect_characters(), "字符", errors)
    title_inventory = check_inventory(TITLE_INVENTORY, gen_fonts.title_characters(), "标题字符",
                                      errors)

    required_full = FULL_LATIN | {ord(c) for c in inventory}
    for name in FULL_FONTS:
        path = FONT_DIR / f"{name}.c"
        if not path.is_file():
            errors.append(f"缺少字体文件 {path.name}")
            continue
        errors += report_missing(name, required_full, parse_coverage(path))

    title_path = FONT_DIR / f"{TITLE_FONT}.c"
    if not title_path.is_file():
        errors.append(f"缺少字体文件 {title_path.name}")
    else:
        errors += report_missing(TITLE_FONT, TITLE_LATIN | {ord(c) for c in title_inventory},
                                 parse_coverage(title_path))

    score_path = FONT_DIR / f"{SCORE_FONT}.c"
    if not score_path.is_file():
        errors.append(f"缺少字体文件 {score_path.name}")
    else:
        errors += report_missing(SCORE_FONT, {ord(c) for c in SCORE_SYMBOLS},
                                 parse_coverage(score_path))

    if errors:
        for line in errors:
            print(f"FAIL {line}", file=sys.stderr)
        return 1

    print(f"Font coverage: PASS ({len(required_full)} 个码点 × {len(FULL_FONTS)} 套正文字号"
          f" + {len(title_inventory)} 个标题字 + {len(SCORE_SYMBOLS)} 个比分符号)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
