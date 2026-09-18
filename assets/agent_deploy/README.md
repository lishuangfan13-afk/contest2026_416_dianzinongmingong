# agent_deploy · 设备端 Agent 部署资产

本目录是「朝夕」Agent 的完整部署资产，刷机后用 `tools/deploy_assets.ps1` 推送到开发板即可使用。

> **数据根目录**：ai_agent 的数据根目录（下称 `<BASE>`）在旧固件为 `/data/ai_agent`，工作区最新源码已改为 `/data/agent`（`agent_config.h`）。部署脚本会自动探测，下表以 `<BASE>` 代指。

## 文件清单

| 文件 | 部署路径 | 用途 |
|------|----------|------|
| `SOUL.md` | `<BASE>/config/SOUL.md` | Agent 人格设定（语气、价值观、行为准则） |
| `USER.md` | `<BASE>/config/USER.md` | 用户信息模板（姓名、城市、职业、偏好） |
| `MEMORY.md` | `<BASE>/memory/MEMORY.md` | 长期记忆模板（项目信息、用户习惯，Agent 运行时读写） |
| `cron.json` | `<BASE>/cron.json` | 定时任务配置（每日简报、久坐提醒、饮水提醒） |
| `daily-briefing.md` | `<BASE>/skills/daily-briefing.md` | 每日简报 Skill（覆盖内置同名） |
| `reminder.md` | `<BASE>/skills/reminder.md` | 定时提醒 Skill（覆盖内置同名） |
| `note-taker.md` | `<BASE>/skills/note-taker.md` | 语音记事 Skill（覆盖内置同名） |
| `health-reminder.md` | `<BASE>/skills/health-reminder.md` | 健康提醒 Skill（纯新增，内置无此 skill） |

## Skill 加载原理

框架的 skill 机制在 `packages/ai_agent/src/tools/skill_loader.c`，流程为：

1. **启动写入**：`skill_loader_init()` 将 10 个内置 skill（weather、daily-briefing、skill-creator、system-health、reminder、note-taker、translate、news-digest、feishu-test、task-manager）写入 `/data/agent/skills/`；**若同名文件已存在则直接跳过，不会覆盖**（`install_builtin()` 中 `fopen(path, "r")` 成功即返回）。
2. **摘要注入**：每次构建 system prompt 时，`skill_loader_build_summary()` 扫描 skills 目录全部 `.md`，取每个文件的标题（`# 首行`）和首段描述（标题到首个空行之间）拼成 skill 清单注入提示词。
3. **意图匹配执行**：LLM 根据摘要匹配用户意图，再用 `read_file` 读取对应 skill 全文并照其步骤执行。
4. **热加载**：`skill_loader_check_changed()` 对目录内文件名 + size + mtime 做哈希，检测到变化即重建摘要，无需重启 Agent。

## 同名覆盖语义

团队自研的 `daily-briefing.md`、`reminder.md`、`note-taker.md` 与内置 skill 同名，`health-reminder.md` 为纯新增。

**覆盖依赖于"内置只写不覆盖"这一行为**：由于 `install_builtin()` 对已存在的同名文件直接跳过，只要团队版先于 Agent 首次启动部署到 `<BASE>/skills/`，内置版就永远不会被写入，等效于屏蔽。

**注意**：若 Agent 已启动过，设备上可能已存在内置版同名文件。用 `deploy_assets.ps1` 部署时首条写入以 `>` 覆盖，会整体替换设备端文件内容，因此任何时机部署都能得到团队版；若用其他方式（如手工追加写入）部署，则需先删除设备上的同名文件再写入，确保落盘的是团队版。

`health-reminder.md` 与内置列表不重名，直接放入 skills 目录即为纯新增，无任何冲突。

## 与 cron / SOUL / USER / MEMORY 的关系

- `SOUL.md`、`USER.md`、`MEMORY.md` 不走 skill 机制，由 `context_builder.c` 直接拼入 system prompt（人格、用户、长期记忆），路径见上表。
- `cron.json` 由 cron_service 加载（schema 见 `cron_service.c` 的 `cron_parse_job_item`：`id`/`name`/`kind`/`message` 必填）。到期触发时，若 job 带 `action` 则直接执行同名**已注册工具**；无 `action` 则把 `message` 原文推送到 channel——**cron 触发不经过 LLM**。因此每日简报任务推的是引导文案，用户回复后由 LLM 匹配到 daily-briefing skill 生成真实简报——**cron 是触发器，skill 是执行脚本**。
- 各 skill 内部的步骤（读写 MEMORY.md、cron_add 创建提醒等）与上述文件协同，因此部署时建议整套一起推送，避免版本错配。

## 部署验证

推送后可通过以下方式确认生效：

1. 查看日志确认内置安装跳过：目标 skill 出现 `Skill exists: <BASE>/skills/daily-briefing.md` 而非 `Installed built-in skill`。
2. 直接向 Agent 说"给我一份今日简报"，观察执行步骤是否为团队版（读取 USER.md 偏好、推送格式等）。
