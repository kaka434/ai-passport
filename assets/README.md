<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Football live board font subset

The `feature/football-live-board` application ships four generated LVGL fonts.
They are produced from **Noto Sans CJK SC Regular** (Source Han Sans, SIL Open
Font License 1.1) with [`lv_font_conv`](https://github.com/lvgl/lv_font_conv)
1.5.3, 4 bpp, no compression, no kerning.

| File | Size | Coverage | Used for |
| --- | --- | --- | --- |
| `fonts/app_font_12.c` | 12 px | ASCII, Latin-1, Latin Extended-A where the source font has it, GB2312 plus every extra CJK character seen in the live feed | league names, match state, footer |
| `fonts/app_font_15.c` | 15 px | same as above | team names, settings rows, battery |
| `fonts/app_font_19.c` | 19 px | ASCII, Latin-1 and the characters used by static UI text | header title |
| `fonts/app_font_22.c` | 22 px | `0-9`, `-`, `v`, `s` | score |
| `fonts/football_ui_chars.txt` | — | the full inventory used by the 12/15 px fonts | regeneration and coverage gate |
| `fonts/title_chars.txt` | — | the inventory used by the 19 px font | regeneration and coverage gate |
| `fonts/data_chars.txt` | — | CJK characters observed in a real `match_list` response | keeps rare club and league characters in the subset |

- The full inventory is the union of three sources: string literals in the
  application sources (`main/main.c` and `main/app_*.c`), the whole GB2312
  repertoire, and `data_chars.txt`. Re-run `tools/gen_fonts.py` after changing
  UI text; pass `--data-json <captured response>` to refresh `data_chars.txt`
  from a new capture.
- `tools/check_fonts.py` (part of `tools/validate.sh --static`) fails the gate
  when an inventory is stale or a generated font misses a required code point.
- Characters outside the subset degrade to `?` through
  `app_fonts_sanitize()` instead of rendering as an empty box. This is a
  deliberate, documented fallback: the only case in the current feed is one
  club name written in Turkish, and no Noto Sans CJK release carries that
  glyph.
- The source OTF (~16 MB) is deliberately not committed;
  `assets/fonts/source/` is git-ignored. Download it from
  `https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`
  and pass it with `--font`; the license is OFL-1.1, which permits
  redistribution of the generated bitmaps.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
