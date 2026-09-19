---
name: device-skill-authoring
description: 为 openvela ai_agent 框架编写设备端 Skill（Markdown 操作手册）、配置 cron 定时任务、通过串口部署资产到开发板。当需要给嵌入式 Agent 增加新能力、定义定时推送、或部署 SOUL/USER/MEMORY/Skill 资产时使用。
---

# 设备端 Skill 编写与资产部署

## 何时使用

- 给 ai_agent 增加新能力（写 Skill）
- 配置 cron 定时任务（主动推送）
- 通过串口把人格/记忆/Skill/cron 推到开发板

## 一、Skill 的本质与格式

框架把每个 Skill 的**标题+摘要注入 system prompt**，Agent 按需用 `read_file` 读取全文照步骤执行——所以 Skill 是"操作手册"，不是代码。

标准结构（以仓内 `assets/agent_deploy/daily-briefing.md` 为参考实现）：

```markdown
# <技能名>
<一句话职责>

## 何时使用
- <触发场景：用户说什么 / cron 推送后回复什么>

## 执行步骤
1. <每步是框架已注册的工具调用：get_current_time / get_weather / write_file / web_search ...>
   - 参数与格式约束写在步骤内（见下方"数据源文件格式"）

## 输出格式
<期望的回复模板>
```

编写要点（真实经验）：
- 步骤只能引用**框架已注册的工具**，先查 `src/tools/tool_registry.c` 的注册表再写。
- 与 UI 联动的数据源文件有**严格格式约束**，写错 UI 就显示不出来（见下表）。

## 二、UI 数据源文件格式（踩坑提炼）

| 文件 | 格式约束 | 违反后果 |
|------|----------|----------|
| `WEATHER.md` | **单行紧凑**如 `北京 12℃ 晴`；不要 markdown 标题、不要空行开头 | UI 只读第一个非空行，多行=丢数据 |
| `REMINDER.md` | 单行提醒文案 | 同上 |
| `NET_STATUS` | 网络状态字符串 | UI 轮询显示 |
| `TASKS.md` | 待办列表 | UI 计数 |

## 三、cron 定时配置

`cron.json` 的 job 结构（v1 schema）：

```json
{
  "id": "wake", "name": "早安闹钟",
  "kind": "daily", "hour": 8, "minute": 0,
  "channel": "system", "enabled": true,
  "action": "write_file",
  "action_args": "{\"path\":\"/data/ai_agent/REMINDER.md\",\"content\":\"...\"}"
}
```

**关键认知（真实踩坑）**：
1. `kind` 原生只有 `every`（纯间隔，从进程启动起算）和 `at`（一次性）；"每天早 8 点"需要 `daily`——这是我们给 `packages/ai_agent` 提交的补丁（0005），用 `gmtime_r` + 固定 UTC+8 偏移（遵循框架避免 `localtime_r`/`mktime` 的约定，NuttX romfs 时区问题）。
2. `channel:"system"` 只写 syslog，**用户在屏幕上完全看不到**。让提醒可见的正确做法：用 `action:"write_file"` 把文案落到 `REMINDER.md`，UI 轮询显示——零框架改动打通"定时→可见"。
3. `action_args` 是 JSON **字符串**，内层引号要转义；schema 不匹配会导致整个 cron 加载失败。

## 四、串口资产部署（deploy_assets.ps1 的设计）

**为什么不用框架自带方式**（源码实证）：
- `install_skill` 只支持 https URL，stdin 模式未实现——离线串口推不了；
- `memory_write` 按空白分词只取首个参数——写不了多行中文。

**绕开方案**：NSH `echo` 逐行写入 + `cat` 回读逐字节校验，校验失败自动重试（最多 3 轮，应对 921600 下偶发单比特错误）。

**必须绕开的两个 NSH 限制（实测）**：
1. readline 行缓冲仅 **80 字节**，超限截断执行——长行要拆分；
2. 双引号字符串内**不支持 `\"` 转义**——含引号的行改用单引号包裹。

**覆盖语义**：同名 skill 部署直接覆盖设备端内容（`>` 重定向），无需关心内置文件。

**注意**：/data 为 tmpfs（分区表无 data 分区）时，重启/烧录后资产全丢，需重跑部署脚本；`<BASE>` 数据根目录新旧固件不同（`/data/agent` vs `/data/ai_agent`），部署脚本会自动探测，也可 `-BaseDir` 显式指定。

## 五、人格三件套

`SOUL.md`（人格：温和、不唠叨）、`USER.md`（用户画像：城市/偏好，Skill 执行时读取）、`MEMORY.md`（长期记忆）与 Skill 配合构成完整的"人格+记忆+技能"体系。改 SOUL 人设会影响所有 Skill 的输出风格，作为一组资产统一部署。
