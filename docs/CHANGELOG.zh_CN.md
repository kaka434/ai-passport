<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

> **由发布负责人维护：** 普通功能、应用和文档 PR 不修改本文件。创建发布 tag 前，
> 发布负责人检查已合并的用户可见变化，并与 `CHANGELOG.md` 同步更新。当前
> `Unreleased` 下的条目是下次发布的待核对内容，不是已经定稿的发布历史。

## 足球比赛看板 v0.1.0 — 2026-10-21

足球比赛看板的首次发布，开发于 `feature/football-live-board` 分支。

- 新增实时比分看板：通过 Wi-Fi 从懂球帝公开接口拉取当天比赛，直播中置顶、其后是
  今天未开始的、最后是今天已完场的，上限 24 场，只保留英超、西甲、意甲、德甲、法甲、
  中超、中甲七项赛事。
- 新增单场详情：进球时间轴（主队靠左、客队靠右、分钟居中，点球与乌龙有标注）、
  阵型、8 项技术统计。还没开赛的比赛显示「比赛未开始」，不再显示错误。
- 新增免电脑配网：屏上扫描热点并用 6×7 屏上键盘输密码，或让设备开热点、用手机填
  网页。连过的网络存入 NVS，带基础校验。
- 新增明确的空状态：当天没有比赛时自动切到最近的比赛日并挂琥珀色倒计时横幅；
  七天之内都没有赛事时直接说明，而不是留一个空列表。
- 全部网络响应改为流式处理——C3 没有 PSRAM，列表响应 1.6 MB、详情最大 37.9 KB，
  而运行时空闲堆只有约 25 KB。修复了未开赛比赛详情页一律显示解析失败的问题：
  接口在没有赛况时返回空数组，而解析器原先只接受对象。
- 新增串口命令 `wifi set/status/clear` 与 `board interval/refresh/detail`，
  60/120/300 秒自动刷新，以及两分钟无操作后背光降到 18%。
- TLS、Wi-Fi、lwIP 与 LVGL 内存池按本板实测值设定，实测数据记录在
  `docs/reference/kaka/`。

## 足球比赛看板 v0.2.0 — 2026-10-22

同样开发于 `feature/football-live-board` 分支。这一版修掉两个"看不到"的问题
（凌晨的比赛、球员名），并把日期从"只能看当天"扩展到"前后翻页"。

- 修复凌晨的比赛不显示。`match_list` 接口的窗口锚在 UTC 日上，从不返回北京时间
  00:00–07:30 这一段，而欧冠欧联的凌晨场正好全落在里面；现在改为从
  `schedule_list` 补这一段再按比赛 id 去重合并。`schedule_list` 完全不返回正在进行的
  比赛，所以它是 `match_list` 的补充而不是替代。补拉做了节流：07:00 前每次刷新都补
  （最晚的凌晨场可能还没踢完），07:00 之后当天只补一次，日常流量回到 264 KB 一轮。
- 修复比赛详情里球员名全部显示成 `?`。详情接口把中文整段转义成 `\uXXXX`，而解析器的
  转义分支只认 ASCII、其余码点一律降级成 `?`；现在真正解成 UTF-8，并支持代理对。
  零宽字符（U+200B 等）不再显示成孤立的问号。
- 完场上限从 4 提到 14，列表容量从 24 提到 40。欧冠一个比赛日就有九到十场凌晨场，
  周六一天有 30 场白名单赛事，旧上限会把它们截断。
- 新增前后翻页：比分页首项再按「上」回看前一天（页头 `回看 MM-DD`），末项再按「下」
  往后翻一天（`明天 MM-DD`），最多到第七天。补上凌晨段之后今天不再是空的，原先
  「自动跳到下一个比赛日」就不再触发，未来的比赛靠往后翻页来看。非当天的日子拉一次
  就缓存，翻回去不重复下载；拉取途中翻页会丢弃旧结果，不再出现"先闪一下旧列表"。
- 联赛白名单加入欧冠与欧联，看板覆盖九项赛事。匹配仍为全等，`欧协联`、`女足欧冠`、
  `欧青联`、`亚冠精英` 依旧被挡在外面。
- 新增冷启动 2.2 秒像素风 GOAL 开机动画：8 帧，足球弧线入网后 `GOAL!` 炸开。帧数据是
  flash 里的 L8 灰度位图，占 600 KB flash、运行时零 RAM；按键唤醒不播放。
- 设置页与串口命令新增「自动息屏」（常亮 / 30 / 60 / 120 秒，默认 60 秒）和
  「屏幕亮度」（25/50/75/100%，默认 100%）。息屏只关背光、不停联网，按键点亮时看到的
  就是最新比分。
- 设置页改为按下即响应，不再等 180 ms 的单击判定窗口；翻行只重绘选中行，不重建整页。
  连按两下也不会再被并成无人处理的双击而丢键。
- 加固比分拉取：失败后先隔 1.2 秒重试一次，仍失败则等 30 秒再试，而不是睡满整个刷新
  间隔；失败日志同时输出空闲堆与最大连续块，便于区分 TLS 握手失败与网络故障。
- 修掉顶部横幅的标签被截断成「下一个比…」：省略号要占一格，五个 12 px 汉字需要
  72 px 的标签，原来只给了 70。

## v0.3.0-football-live-board — 2026-10-24

同样开发于 `feature/football-live-board` 分支。这一版给详情页加上两队队徽，并修掉两个
"看不见"的问题：凌晨的比赛在第二次刷新后消失、未开赛的场次没有队徽。

- 修复当天第一次刷新之后看板变空、直接翻到明天的问题。凌晨段补拉原先做了节流，用一个「补过了」的标志控制，可每次刷新都会清空解析缓冲，凌晨那批比赛跟着没了，于是第二次刷新今天解析出 0 场。现在每次刷新都补拉——用 130 KB 换确定性，而不是留一个会和数据对不上的标志；失败就地重试一次，再次失败则作废整次刷新，不再发布半天的列表。

- 把开机偶发的 `PK verify failed with error 0x4290` 降下来：LVGL 池从 40 KB 收到 32 KB。这个错误码解出来是 `MBEDTLS_ERR_RSA_PUBLIC_FAILED + MBEDTLS_ERR_MPI_ALLOC_FAILED`，也就是 RSA 公钥运算里一次内存分配失败，不是签名算错：`esp_crt_bundle` 给证书链每一环都新建一个公钥 context，而 `mbedtls_rsa_public()` 走的是 IDF 的硬件 MPI 封装，每次都要现算 Montgomery 常数（一个 4097 位的临时大数加一次纯软件除法）。这一下叠在 16 KB 入缓冲、4 KB 出缓冲、解析后的证书链和 lwIP/Wi-Fi 收包缓冲之上，把堆压到只剩几百字节。实测连接建立后空闲堆从约 7.8 KB 涨到约 19 KB，连续 10 次冷启动的证书失败次数从 1 次变成 0 次。16 KB 入缓冲降不了（服务端按 16,401 字节一条记录下发，8 KB 会在第 8 条记录上失败），所以这 8 KB 只能从别处让出来。

- 详情页加队徽。进球页的比分条从 46 px 涨到 68 px，队名上方放 32×32 队徽；统计页删掉阵型条——它和比分条同在 `y=0`，一直把比分条盖住——换成同一个带队徽的比分条，统计行距 24 → 22、起始行 48 → 74。队徽 URL 取自详情响应的 `statistics.team_A.logo` / `team_B.logo`，原图 150×150（解一张要 90 KB，C3 上不可行），改用 CDN 的 `?imageView2/2/w/32` 缩到 32×32，实测 120 个样本 p99 3295 B、最大 3451 B。

- 新增 `main/app_png.c`：手写的极简 PNG 解码器（DEFLATE inflate + 五种反过滤 + 调色板与 tRNS），只认 32×32 以内、位深 8、不隔行、色彩类型 0/2/3/4/6。抽样 45 个队徽里 CDN 实际给出过调色板、RGBA、灰度+Alpha 三类，认不全就会有一批队徽变成占位块。不挂 lodepng 是因为它解一张 32×32 的图要从堆上要 4 KB 输出加近 3 KB 霍夫曼树，而这里除了调用方给的出图缓冲只用一个临时解压缓冲。

- 新增 `main/app_crest.c`：按 URL 缓存两个槽位，同一场反复进出详情页不会重打 CDN；下载走 `app_net_fetch_binary()`，和赛况共用同一把 HTTP 互斥量，不会出现两条 TLS 同时攥着 16 KB 入缓冲。

- 队徽的下载缓冲与解压缓冲临时从堆上要、用完立刻还，静态区只留两块 32×32 像素（4.1 KB）。这两块加起来 8 KB，钉成 `.bss` 等于让每次 TLS 握手都少 8 KB 余量——那正是上一版花力气修掉的 `0x4290` 成因。实测 `.bss` 从 94,296 降到 86,072 字节。

- 修掉 `detail_task` 只在 `detail_fetch()` 之后泵队徽的问题：队徽的 URL 来自详情响应，界面是等响应到了才去要图的，那时本次 `detail_fetch()` 早已跑完、任务已回去睡；后到的请求会被那次唤醒吞掉。改成每轮醒过来都先把排队的图拉完再睡，队徽的排队也移到 `detail_fetch()` 成功之后，串口的 `board detail <id>` 因此能完整走通这条链路。

- 实测：连续 6 次冷启动 6/6 拉到 25 场、0 次 `0x4290`；`board detail 54383914` 两个队徽分别 1108 B / 674 B 下载并解码成功。

- 修掉未开赛场次的详情页没有队徽。原因不在解码器：赛况响应里就没有队徽——实测那份响应只有 187~260 字节，`statistics` 是空数组、`tendencies` 是 `null`，两个 team 对象根本不在里面。补的办法是再问一次 `/data/detail/match/{id}`：它把球队信息摊平成 `matchSample.team_A_logo` / `team_B_logo`，未开赛的场次也有值，整份 5.7 KB，两个字段在偏移 1552 和 1833，所以只读前 4 KB（临时从堆上要、用完即还）。这次请求只在赛况响应没队徽时才发，已开赛/已完场的比赛一次都不会多发。

- 另一条更省事的路试过但放弃了：`match_list` 里每支球队都带 logo（未开赛的也带），解析列表时顺手存下来就不用二次请求。但 40 场 × 2 队的 URL 要 5.8 KB 静态内存，加完之后冷启动 6 次里有 2 次在证书验签时撞上 `0x4290`（`.bss` 86,072 → 91,904），而加之前是 0/6。静态内存正是 TLS 握手要用的余量，省一次请求不值得拿这个换；改回零静态内存的方案后 `.bss` 回到 86,064 字节，冷启动 6/6 干净。（按球队 id 取徽的接口不存在：`/data/team/{id}` 等 8 个候选路径全是 404。）

- 实测：`board detail 54389825`（中超 河南 vs 北京国安，未开赛）兜底从 `detail/match` 取到 4096 字节，两个队徽 2904 B / 2919 B 下载并解码成功；`board detail 54383914`（已完场）不发兜底请求，仍从赛况响应取到队徽。

- 拒收被截断的响应：正文短于 `Content-Length` 现在算失败，而不是把半份 JSON 解析完、静默发布残缺列表。握手失败会打印解析到的地址与系统时钟，建连失败会打印空闲堆与最大连续块，这样证书问题能和网络、内存问题区分开。

- 加入厂家为优特利 520mAh 电芯生成的 80 字节 CW2017 profile，并实现内容与更新标志检查、写入后校验、规定的重启时序以及有上限的 SOC 就绪等待。

- 扩充环境引导文档：新增乐鑫 Git 服务镜像（`git.espressif.com.cn`）作为中国大陆首选线路，覆盖 ESP-IDF v5.5.3 及其子模块；补充子模块长等待/超时处理、原地修复，以及 `esp32-wifi-lib` 等大仓的按钉死 commit 浅取；提示按仓库残留的 Jihulab `insteadOf` 旧配置；并把官方离线 release 压缩包加入兜底方案（经验来自 `esp-mosaico/esp-mosaico-vibe`）。

- 按功能域整理文档并采用双入口：根目录 `AGENTS.md` 变为薄路由（只保留硬约束与任务路由），详细的 AI 开发工作流下沉到 `docs/development/ai-guide.md`，`agent-guide.md` 并入其中。为 `docs/development/` 增加二级分区（`engineering/`、`ci/`、`release/`），把 `plays/` 应用档案与 `experiences/` 移入带专属 README 的 `docs/reference/` 参考区；删除 `docs/software-design/`（空脚手架）；把 `assets/{fonts,images,music}/README` 三个叶子 README 并入 `assets/` README；把 `project-completion` 的六个子文档压平为单文件；并把每个目录统一为单一 README，消除所有 `INDEX` 文件与一处重复经验索引。所有交叉引用与文献链接已更新；未丢弃任何内容。

- 删除位于 `0x700000` 的旧 app/test 分区，以及相关的 bootloader、校验和
  文档要求；固定的 `cardid` 保护分区及其 CI 校验保持不变。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。

## Unreleased
