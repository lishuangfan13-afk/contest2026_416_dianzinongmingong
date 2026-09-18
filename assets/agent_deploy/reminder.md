# 智能提醒

设置定时提醒，到期主动推送通知。

## 何时使用
- 用户说"提醒我"、"X分钟后叫我"、"每天X点提醒"
- 用户说"设个闹钟"、"定时提醒"

## 执行步骤
1. 使用 get_current_time 获取当前时间
2. 解析用户的时间表达：
   - "5分钟后" → once, trigger_epoch = now + 300
   - "每天早上8点" → every, interval_sec = 计算到明天8点的秒数
   - "明天下午3点" → once, trigger_epoch = 明天15:00
   - "每周一上午9点" → every, interval_sec = 7天
3. 使用 cron_add 创建定时任务：
   - action: "notify"
   - message: 提醒内容（用户原话提炼）
   - schedule_type: "once" 或 "every"
4. 确认提醒已设置，告知具体触发时间

## cron_add 参数格式
`
cron_add {
  "action": "notify",
  "message": "该喝水了！",
  "schedule_type": "once",
  "trigger_epoch": 1711613100,
  "channel": "system"
}
`

## 注意事项
- 时间表达要转换为具体 epoch 或间隔秒数
- 用自然语言确认，如"好的，5分钟后提醒你喝水"
- 如果用户没说具体提醒内容，从上下文推断
- 默认通知渠道为 system（本地推送）

## 示例
用户："提醒我下午2点开会"
→ get_current_time → 计算今天14:00的epoch
→ cron_add
→ "好的，下午2点提醒你开会 ⏰"

用户："每天早上8点提醒我喝水"
→ cron_add schedule_type=every
→ "已设置每天早上8点喝水提醒 💧"