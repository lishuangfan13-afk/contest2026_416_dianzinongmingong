# 健康提醒

久坐提醒与健康关怀，帮助用户保持良好的工作节奏。

## 何时使用
- cron 定时触发（cron.json 中 id=sedent 每 90 分钟、id=water 每 2 小时）
- 用户主动问"我该休息了吗"、"久坐提醒"
- 用户说"开启久坐提醒"、"设置健康提醒"

## 功能

### 久坐提醒（主动触发）
每 90 分钟提醒用户起身活动：
- 站起来走动 2-3 分钟
- 做几个伸展动作
- 喝杯水
- 看看远处放松眼睛

### 饮水提醒（可选）
每隔 2 小时提醒喝水。

### 用眼提醒
每 45 分钟提醒远眺（20-20-20 法则：每20分钟看20英尺外20秒）。

## 执行步骤
1. 使用 get_current_time 获取当前时间
2. 判断时间段：
   - 工作时间（9:00-18:00）：正常提醒
   - 午休时间（12:00-13:30）：不打扰
   - 下班后：不提醒
3. 轮换不同的提醒内容，避免重复感
4. 推送温暖简短的提醒消息

## 提醒话术示例
- "坐了挺久了，起来活动一下吧 🚶"
- "记得喝口水 💧"
- "眼睛需要休息了，看看远处放松一下 👀"
- "站起来伸个懒腰，给身体充充电 ⚡"

## 设置方式
提醒要能被 UI 看到，必须由 cron 任务调用 `write_file` 写入 `/data/ai_agent/REMINDER.md`（UI 轮询该文件，取第一个非空行显示在聊天界面）。`channel:"system"` 只写 syslog，用户不可见，所以不能只靠 message。

cron.json 中已有三个固定任务（部署资产直接生效，无需 agent 创建）：
- `sedent`：`kind:"every"`, `interval_s:5400`，`action:"write_file"`
- `water`：`kind:"every"`, `interval_s:7200`，`action:"write_file"`
- `wake`：`kind:"daily"`, `hour:8, minute:0`（需框架补丁），`action:"write_file"`

其 `action_args` 形如：
`{"path":"/data/ai_agent/REMINDER.md","content":"久坐提醒：坐了挺久了，起来活动一下吧！"}`

用户说"开启久坐提醒"时：
→ 若任务已存在且 enabled=true，告知已开启；否则用 cron_add 重建：
  name="久坐提醒", schedule_type="every", interval_s=5400,
  action="write_file", action_args 同上
→ "已开启久坐提醒，每 90 分钟提醒你活动一下 🏃"
（饮水提醒同理：interval_s=7200）

注意 `id` 最长 8 字符（`char id[9]`），超长会被静默截断，cron_add 生成的 id 由框架自动分配。

## 注意
- 框架的 cron `every` 是纯间隔重复，**没有时段过滤（无 quiet hours）**，从进程启动时刻起算，可能在深夜触发提醒。上述"工作时间/午休不打扰"的时段判断仅在用户主动对话时生效。
- 要避免深夜打扰，可手动编辑 `/data/ai_agent/cron.json` 把对应任务 `enabled` 改为 `false`（或删除该任务），或用需要框架补丁的 `kind:"daily"` + `hour/minute` 指定固定时间。
- `/data` 为 tmpfs，重启后 cron.json 与 REMINDER.md 都会丢失，需重新运行 `tools/deploy_assets.ps1` 推送。
- 提醒语气温和，不唠叨
- 如果用户回复"好的"或"知道了"，表示收到，不追问
