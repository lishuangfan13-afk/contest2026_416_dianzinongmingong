# Codex 本会话历史补录

本记录由用户授权从桌面端可见会话历史定向导出，使用赛事 JSONL 事件格式，与既有日志在同一账号目录管理。

- 来源：Codex 桌面端本会话，共读取 145 轮历史分页；保留 935 条可见消息。
- 采集方式：`manual-scoped-redacted`，属于经过筛选和脱敏的历史补录，与自动 hook 采集区分。
- 时间戳：对应轮次开始时间，消息按会话顺序排列；同一轮的消息共享轮次时间。
- 内容处理：剔除含凭据的敏感消息，脱敏邮箱、MAC、电话、本机账号及热点标识，排除近期导出方法讨论；工具原始参数、输出、系统配置与内部记录不进入本文件。
- 原始工作目录保留为本会话的实际目录形式，未伪装为 openvela 工作区。
- manifest 的 `collection_mode=backfill-sqlite` 沿用官方 Codex 补录适配器当前使用的兼容字段值；实际来源由 `source_format=codex-desktop-visible-thread` 和 `export_mode` 说明，并非 SQLite。
- 导出脚本：[定向历史导出](../../tools/export_scoped_codex_log.py)。原始输入仅留本机。
- 记录：[codex__01a161c0-ccea-7a13-a3f5-5f5e30675db9.jsonl](2026-10-22/codex__01a161c0-ccea-7a13-a3f5-5f5e30675db9.jsonl)。

保留的对话包含开发过程、技术判断、失败尝试、用户反馈和修复结果。完整性清单记录本次筛选数量与文件校验值。
