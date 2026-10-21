<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 足球看板字体子集

`feature/football-live-board` 应用带四套生成的 LVGL 字体，均来自
**思源黑体 Noto Sans CJK SC Regular**（SIL Open Font License 1.1），
用 [`lv_font_conv`](https://github.com/lvgl/lv_font_conv) 1.5.3 生成，
4 bpp、不压缩、不做字距调整。

| 文件 | 字号 | 覆盖范围 | 用途 |
| --- | --- | --- | --- |
| `fonts/app_font_12.c` | 12 px | ASCII、Latin-1、源字体具备的 Latin Extended-A、GB2312 全集，以及比分数据里出现的其它汉字 | 联赛名、比赛状态、页脚 |
| `fonts/app_font_15.c` | 15 px | 同上 | 队名、设置项、电量 |
| `fonts/app_font_19.c` | 19 px | ASCII、Latin-1，以及界面静态文案用到的字 | 页头标题 |
| `fonts/app_font_22.c` | 22 px | `0-9`、`-`、`v`、`s` | 比分 |
| `fonts/football_ui_chars.txt` | — | 12/15 px 用的完整清单 | 重新生成与覆盖校验的输入 |
| `fonts/title_chars.txt` | — | 19 px 用的清单 | 重新生成与覆盖校验的输入 |
| `fonts/data_chars.txt` | — | 从真实 `match_list` 响应里扫出的汉字 | 让冷门球队/联赛名不缺字 |

- 完整清单由三部分并起来：`main/main.c` 与 `main/app_*.c` 里的字符串字面量、
  GB2312 全集、`data_chars.txt`。改动界面文案后重跑 `tools/gen_fonts.py`；
  加 `--data-json <抓包文件>` 可以从新的响应刷新 `data_chars.txt`。
- `tools/check_fonts.py`（已接入 `tools/validate.sh --static`）会在清单过期或
  生成字体缺字时让校验失败。
- 子集之外的字符由 `app_fonts_sanitize()` 显示成 `?`，而不是空心方框。这是
  明确的降级策略：当前数据里唯一命中的是一条土耳其语写法的队名，而任何一版
  思源黑体都没有那个字形。
- 约 16 MB 的源 OTF 不提交到仓库（`assets/fonts/source/` 已在 .gitignore 里）。
  需要时从
  `https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`
  下载并用 `--font` 传入；OFL-1.1 允许分发生成后的位图。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
