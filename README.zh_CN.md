<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 足球比赛看板（FoloToy-AI-Passport · feature/football-live-board）

基于 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport)
（ESP32-C3 + LVGL 中文屏 + 三键 ADC）做的一个实时比分看板。设备通过 Wi-Fi 从
[懂球帝](https://www.dongqiudi.com/match)公开接口拉取足球比赛，用一套专门设计的
中文界面显示；配网（屏上键盘 / 手机网页）不必连电脑。本仓库是上游的 fork，看板
属于 `feature/football-live-board` 分支自己的应用，仓库里只有这一份应用。

完整设计与踩坑记录见应用档案
[`docs/reference/kaka/football-live-board.zh_CN.md`](docs/reference/kaka/football-live-board.zh_CN.md)，
上游总体介绍见 [`docs/README.md`](docs/README.md)。

## 它做什么

- **只看当天**：直播中的排最前，然后是今天还没开始的，最后是今天已完场的；最多 24 场。
- **七大赛事**：英超、西甲、意甲、德甲、法甲、中超、中甲，其余一律不显示。
- **没有比赛会明说**：当天一场也没有时自动往后找最近的一个比赛日，并在顶部挂一条
  琥珀色横幅（`下一个比赛日 10-24 · 还有 3 天`）；七天内都没有就显示「近期没有比赛」，
  不会停在空白页让人以为坏了。
- **单场详情**：VIEW 键打开，翻页在「进球时间轴 / 阵型 + 8 项技术统计」之间切换。
  主队靠左、客队靠右、分钟居中，点球标“点”、乌龙标“乌”。未开赛的比赛显示
  「比赛未开始」，不伪装成错误。
- **两种配网**：屏上扫描热点 + 屏上键盘输密码；或者让设备开热点、用手机连上后填网页。
  连过的网络记在 NVS 里，重启免输。
- **省内存优先**：无 PSRAM。列表响应 1.6 MB、详情响应最大 37.9 KB，全部走流式解析，
  峰值只吃 1 KB 窗口，运行时空闲堆仍有约 25 KB。

## 硬件与前提

- ESP32-C3 复刻板，8 MB Flash，ST7789P3 240×320 竖屏，三个按键走 ADC。
- ESP-IDF **5.5.3**（LVGL 9.5 由组件管理器带入）。
- 刷机前需要设备处于 bootloader 或可下载状态；常规流程是 `idf.py -p PORT flash`。

## 界面与按键

| 页面 | 内容 |
| --- | --- |
| 比分 | 直播中 / 今天未开始 / 今天已完场；上行"联赛 + 状态"，下行"主队 比分 客队" |
| 详情 1 | 比分条 + 进球时间轴 |
| 详情 2 | 阵型 + 8 项技术统计 |
| 设置 | Wi-Fi 配网、手机配网、网络地址、刷新间隔（60/120/300 秒）、立即刷新、清除配网 |
| 热点 / 密码 / 手机 | 扫描到的 SSID 列表、屏上键盘、手机网页配网引导 |

| 按键 | 比分页 | 详情页 | 设置页 |
| --- | --- | --- | --- |
| 上 / 下 单击 | 移动选中行 | 翻子页 | 移动选中行 |
| 确定 单击 | 打开详情 | 重新拉取这一场 | 执行当前行 |
| 确定 长按 | 进设置 | 返回比分页 | 返回比分页 |

两分钟无操作背光降到 18%，按键恢复。

## 构建与烧录

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash                       # Windows 下换成实际串口
```

产出从 `0x0` 一次写全的合并镜像（bootloader + 分区表 + app）：

```bash
idf -B build merge-bin -o FoloToy-AI-Passport-full.bin
```

固件部署/回归用的校验工具见 `tools/verify_firmware.py` 与 `tools/archive_firmware.py`：
每次 build 后归档一份，方便按 SHA 回滚。

## 串口命令行

控制台是 USB Serial/JTAG，波特率 115200，启动完成后可输命令：

```text
wifi set <ssid> <password>   保存 Wi-Fi 并立即连接
wifi status / wifi clear     查看 / 清除配网
board interval <60|120|300>  设置自动刷新间隔
board refresh                立即刷新一次
board detail <match_id>      单独拉一场赛况，排查解析问题用
```

## 已验证 / 未验证

- **已实测**（真机）：Wi-Fi 连接与跨重启持久化、热点扫描、SNTP 校时、TLS 握手、
  401 场全量列表刷新、单场详情在 187 B（未开赛）到 37.9 KB（完场）各量级下的解析、
  中文点阵观感、按键导航、两种配网流程。
- **未测量**：长时间稳定性、功耗与电池读数。

## 上游与许可

- 上游仓库与主干基线：[`gitee.com/FoloToy/ai-passport`](https://gitee.com/FoloToy/ai-passport)，
  fork 工作流见 [`docs/fork-guide.zh_CN.md`](docs/fork-guide.zh_CN.md)。
- 字体子集由 `tools/gen_fonts.py` 依据思源黑体（SIL OFL 1.1，不进仓库）生成，
  说明见 [`assets/README.zh_CN.md`](assets/README.zh_CN.md)。
- 代码部分跟随上游 **MIT**，见 [`LICENSE`](LICENSE)。
