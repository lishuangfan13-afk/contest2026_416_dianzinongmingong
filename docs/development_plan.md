# 「朝夕」—— 主动式 AI 生活管家
## 完整开发计划与实施步骤

---

## 一、项目概述

**项目名称**：朝夕（Zhao Xi）
**一句话定位**：面向都市白领与家庭桌面场景的主动式 AI 生活管家
**核心理念**：把 AI 从「问答工具」变成「主动干活的生活伙伴」

**目标开发板**：BES 2800BP（恒玄科技）
**系统平台**：openvela（NuttX 内核）+ packages/ai_agent 框架
**LLM 后端**：Xiaomi MiMo v2.5 Pro（api.xiaomimimo.com）

---

## 二、功能清单与优先级

| 优先级 | 功能 | 依赖 | 预计工期 |
|--------|------|------|----------|
| P0 | 1. 自定义 Skill 系统 | ai_agent 框架 | 1天 |
| P0 | 2. 每日简报（daily-briefing） | Skill + cron + LLM | 1天 |
| P0 | 3. 语音记事/待办（note-taker） | Skill + LLM | 1天 |
| P0 | 4. 定时提醒（reminder） | cron_service + LLM | 1天 |
| P1 | 5. 久坐/健康提醒 | cron + 事件触发 | 0.5天 |
| P1 | 6. 飞书消息接入 | feishu channel | 1天 |
| P1 | 7. 微信消息接入 | weixin channel | 0.5天 |
| P2 | 8. 断网本地降级 | 关键词 fallback | 1天 |
| P2 | 9. LVGL 图形界面 | LVGL + 消息总线 | 2天 |
| P2 | 10. 语音交互（PTT/ASR/TTS） | voice channel | 2天 |

---

## 三、详细开发步骤

### 阶段一：基础框架搭建（已完成 ✅）

- [x] openvela 源码同步与编译环境搭建
- [x] BES2800BP 固件编译（1700_ap.sh + NTC 补丁）
- [x] AP-only 烧录 nuttx_ap.bin（dldtool + programmer1700_dual.bin，严禁全量烧录）
- [x] NSH 控制台验证（COM20, 921600）
- [x] WiFi 连接（OnePlus13T52D6）
- [x] AI Agent 启用（CONFIG_EXAMPLES_AI_AGENT_VELA=y）
- [x] LLM 后端配置（MiMo v2.5 Pro）

### 阶段二：核心 Skill 开发（当前阶段）

#### 步骤 2.1：搭建 Skill 开发框架
- 创建自定义 Skill 目录结构
- 编写 Skill 模板
- 测试 Skill 加载机制

#### 步骤 2.2：开发 daily-briefing Skill（每日简报）
- 功能：每天定时主动推送天气、日程、新闻摘要
- 触发方式：cron 定时任务（默认早8点）
- 输出格式：结构化文本（天气+新闻+待办）

#### 步骤 2.3：开发 note-taker Skill（语音记事/待办）
- 功能：自然语言录入，自动分类为备忘/待办/日程
- 存储：/data/ai_agent/notes/ 目录下的 JSON 文件
- 支持查询：「我有什么待办？」「今天的日程」

#### 步骤 2.4：开发 reminder Skill（定时提醒）
- 功能：设置定时提醒，到期主动推送
- 触发方式：cron 定时任务
- 支持自然语言设置：「明天下午3点提醒我开会」

### 阶段三：主动任务引擎

#### 步骤 3.1：配置 cron_service 定时任务
- 每日简报定时触发
- 提醒任务定时检查
- 久坐提醒定时触发

#### 步骤 3.2：实现事件监听与上下文触发
- 消息到达事件
- 时间阈值事件
- 用户行为上下文

### 阶段四：消息通道接入

#### 步骤 4.1：飞书 Bot 接入
- 配置飞书应用凭证
- 测试消息收发
- 实现消息摘要播报

#### 步骤 4.2：微信 Bot 接入
- 配置微信凭证
- 测试消息收发

### 阶段五：离线降级与 UI

#### 步骤 5.1：断网本地降级
- 关键词意图分类器
- 本地备忘/待办查询
- 离线提示

#### 步骤 5.2：LVGL 图形界面
- 日程/待办列表视图
- Agent 状态显示
- 消息总线交互

### 阶段六：语音交互（可选）

#### 步骤 6.1：PTT 对讲 + ASR
#### 步骤 6.2：TTS 播报回复

---

## 四、文件结构

```
/data/ai_agent/
├── config/
│   ├── config.json          # 主配置
│   ├── SOUL.md              # Agent 人格设定
│   ├── USER.md              # 用户信息
│   └── cron.json            # 定时任务
├── skills/
│   ├── weather.md           # 天气查询
│   ├── daily-briefing.md    # 每日简报 ← 自定义
│   ├── note-taker.md        # 语音记事 ← 自定义
│   ├── reminder.md          # 定时提醒 ← 自定义
│   ├── health-reminder.md   # 健康提醒 ← 自定义
│   └── family-notify.md     # 消息播报 ← 自定义
├── notes/
│   ├── todos.json           # 待办列表
│   ├── memos.json           # 备忘录
│   └── schedule.json        # 日程表
├── memory/
│   ├── MEMORY.md            # 长期记忆
│   └── daily/               # 每日记忆
└── sessions/                # 会话记录
```

---

## 五、验收标准

1. **每日简报**：语音/文字输入「今天有什么安排」→ 返回天气+待办+新闻
2. **语音记事**：「记一下明天下午开会」→ 自动创建待办
3. **定时提醒**：「5分钟后提醒我喝水」→ 到期主动推送
4. **消息播报**：飞书/微信新消息 → 语音摘要播报
5. **离线模式**：断网后基础查询仍可用
6. **LVGL UI**：屏幕显示日程、待办、Agent 状态

---

*文档版本：v1.0 | 创建时间：2026-09-05*

