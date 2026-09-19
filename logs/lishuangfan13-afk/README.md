# AI Coding 日志 · lishuangfan13-afk

- **参与者 GitHub 登录**：`lishuangfan13-afk`
- **团队**：contest2026_416_dianzinongmingong（朝夕 · 主动式 AI 生活管家）

## 内容说明

本目录收录「朝夕」项目开发过程中的 AI Coding 会话日志（opencode）。

```
logs/lishuangfan13-afk/
├── README.md                  # 本文件
├── manifest.json              # 会话清单（schema v1.0）
└── <YYYY-MM-DD>/              # 按会话开始日期归档
    └── opencode__<sid>.jsonl  # 一个会话一个文件，每行一个事件
```

- 事件格式遵循组委会 `event.schema.json`（v1.0），每行包含
  `schema_version` / `session_id` / `team_id` / `github_login` / `tool` /
  `ts` / `role` / `seq`，并按事件类型附带 `text`、`thinking`、
  `tool_name`/`input`/`output`、`model` 等字段。
- `seq` 为会话内单调递增序号，已通过组委会 `validate-log.py` 校验
  （`ALL OK`）。

## 采集方式

会话由组委会官方 `contest-log-collector` 采集。本次因历史会话早于采集
钩子安装，使用官方 `export-session.py` 的 SQLite 回溯能力从本机
opencode 数据库导出（`collection_mode: backfill-sqlite`），仅导出
工作区（`C:/openvela_ws`，含 `.repo/` 工作区标识）内的会话；工作区外的
个人会话完全不采集。

## 敏感信息处理

导出前对全部会话内容做了密钥扫描与脱敏，覆盖：

- MiMo / OpenAI 风格 API Key（`sk-...`）
- GitHub Token（`ghp_...` / `github_pat_...`）
- 通用 `Authorization: Bearer ...`
- Tavily / 搜索类 Key（`tvly-...`）
- WiFi 口令（`wapi psk` / `set_wifi` 命令中的 PSK）

脱敏后的内容以 `***REDACTED***` / `***WIFI_PSK_REDACTED***` 标记替换，
并经两轮独立扫描确认无残留真实凭据。

## 覆盖的开发阶段

- 需求拆解与方案设计：Skill 系统、cron 引擎、LVGL UI 三模块划分
- 设备端资产编写：`SOUL.md` / `USER.md` / `MEMORY.md` / `cron.json` 与 4 个自定义 Skill
- 编码实现：`zhaoxi_ui.c` 三屏 UI、中文字体流水线、`deploy_assets.ps1`、`ntc_patch.py`
- 框架补丁：HTTP Date 对时、/data 挂载、`kind:"daily"` 调度、网络状态文件
- 调试排障：NTC 断言崩溃、中文豆腐块、触摸与三屏切换验证
- 文档与仓库卫生：`README.md`、`docs/`、密钥脱敏、分类别提交
