<p align="right">
  <strong>简体中文</strong> · <a href="readme.md">English</a>
</p>

# docs/assets

保存 fork 私有、用于补充根目录 `README.md` 的素材：产品照片、架构说明、设计稿与图片。
这些内容属于本 fork，不属于上游基线；上游在该目录下只保留 `.gitkeep` 占位。

## 目录结构

- 每个应用一个子目录或一组同前缀文件，按应用名的 lowercase-kebab-case 命名，
  例如 `football-live-board-scores.jpg`。
- 二进制素材统一放这里，不与 Markdown 文档混放。
- 涉及版权/授权的素材，在引用它的 README 里注明来源与许可。

## AI 使用方法

- `feature/*` 分支的根 `README.md` 链接到本目录，而不是把大图复制到正文旁边。
- 进入仓库时，把根 `README.md` 与本目录内容一起读；上游总体介绍仍在
  `docs/README.md`。
- 重新生成或替换素材是功能分支上的常规改动；保持文件名稳定，避免链接失效。

## 边界

- 本目录只存在于 fork，内容是 fork 私有的，**不得**作为 PR 提交回上游。
- 上游同步工作流只把上游 `main` 同步进本 fork 的 `main`；fork 私有素材应留在
  `feature/*` 分支上，避免同步时冲突。
- 如果某份文档对任何 AI Passport 用户都有用、而非只对本 fork 有用，就把它移出
  本目录，作为文档 PR 提交给上游。
