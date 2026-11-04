# logs/ — AI Coding 日志目录

2026 年 10 月 30 日的单板开发接手与硬件验证摘要见 [BES 单板开发接手记录](bes_mvp_20261030/README.md)。该摘要与原始 AI 日志分开标明，原有历史会话继续保留。

存放你在开发中与 AI 工具的对话日志，和作品代码一并提交。

本目录同时保留官方采集格式的历史日志与本会话的定向补录。各会话在 manifest 中记录来源与处理方式。

- [本会话 Codex 历史补录说明](lishuangfan13-afk/MANUAL_CODEX_EXPORT.md)：定向导出的可见开发对话，已过滤敏感内容及近期导出方法讨论；`export_mode=manual-scoped-redacted`。
- [OpenCode 历史采集说明](lishuangfan13-afk/README.md)：原有采集记录继续保留。

## 目录结构

```text
logs/
└── <github_login>/              # 你的 GitHub 用户名，一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，由组委会提供的日志归集工具导出，**只提交 JSONL 本身**。

导出与提交的完整步骤、字段定义见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
