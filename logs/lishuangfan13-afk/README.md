# AI Coding 日志 — lishuangfan13-afk

- **参与者 GitHub 登录**：`lishuangfan13-afk`
- **团队**：contest2026_416_dianzinongmingong（朝夕 · 主动式 AI 生活管家）

## 状态说明

**本目录暂无原始会话日志（JSONL）。** 在提交本文件时，本机（Windows，用户 `27981`）上未找到属于 `lishuangfan13-afk` 的、可导出的 Codex / Claude Code / opencode 会话记录：

- `C:\Users\27981\.codex\` 下无任何 `rollout-*.jsonl`（仅插件缓存、sqlite 状态库，不含可提交的会话事件流）。
- `C:\Users\27981\.claude\sessions\` 为空，`contest-collector-staging\` 为空。
- `C:\Users\27981\.local\share\opencode\` 下只有 opencode 自身的 `storage/session_diff` 元数据与 `log/` 文本日志，不是归集工具要求的事件级 JSONL。

因此这里只提交目录与说明，不伪造日志内容。真实日志将由官方 `contest-log-collector` 在后续开发会话中归集后补入。

## 目录结构（待补入日志时遵循 `logs/README.md`）

```text
logs/lishuangfan13-afk/
├── README.md                  # 本文件
├── manifest.json              # 会话清单（由归集工具生成）
└── <YYYY-MM-DD>/              # 日期目录
    └── <tool>__<sid>.jsonl    # 一个会话一个文件
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，由组委会日志归集工具导出，只提交 JSONL 本身。

> 注：队友 `logs/yuk1-r/` 目录中的文件由 Codex 导出，命名为 `rollout-<ISO时间>-<uuid>.jsonl` 并直接放在日期目录下（无 `manifest.json`）。待本机归集工具产出的文件命名确定后，会与 `logs/README.md` 的规范保持一致。

## 归集配置

本机归集工具配置位于 `C:\Users\27981\.claude\contest-collector.env`：

```text
TEAM_ID=contest2026_416_dianzinongmingong
GITHUB_LOGIN=lishuangfan13-afk
```

## 覆盖的开发会话

一旦归集工具产出日志，将覆盖「朝夕」项目以下阶段的 AI 辅助开发会话：

- 需求拆解与方案设计：Skill 系统、cron 引擎、LVGL UI 三模块划分
- 设备端资产编写：`SOUL.md` / `USER.md` / `MEMORY.md` / `cron.json` 与 4 个自定义 skill
- 编码实现：`app/zhaoxi_ui/src/zhaoxi_ui.c`、`tools/deploy_assets.ps1`、`tools/ntc_patch.py`
- 调试排障：NTC 断言崩溃的二进制补丁、HTTP Date 对时、/data LittleFS 挂载与 tmpfs 回退
- 文档与仓库卫生：`README.md`、`docs/`、`assets/agent_deploy/README.md`、密钥脱敏
