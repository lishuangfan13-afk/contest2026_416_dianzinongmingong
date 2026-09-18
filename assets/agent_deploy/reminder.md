# 智能提醒

设置定时提醒，到期主动推送通知。

## 何时使用
- 用户说"提醒我"、"X分钟后叫我"、"每天X点提醒"
- 用户说"设个闹钟"、"定时提醒"

## 执行步骤
1. 使用 get_current_time 获取当前时间
2. 解析用户的时间表达：
   - "5分钟后" → at, at_epoch = now + 300
   - "每天早上8点" → every, interval_s = 86400（框架的 every 是纯间隔重复，首次触发从创建时刻起算，无法定点在 8:00）
   - "明天下午3点" → at, at_epoch = 明天15:00 的 epoch
   - "每周一上午9点" → every, interval_s = 604800（7天）
3. 使用 cron_add 创建定时任务：
   - name: 简短的任务名（必填，如 "drink_water"）
   - schedule_type: "at"（一次性）或 "every"（重复）
   - message: 提醒内容（用户原话提炼，必填，触发时框架直接推送给用户）
   - 不要传 action 字段，除非明确要调用某个已注册工具（如 music_play）
4. 确认提醒已设置，告知具体触发时间

## cron_add 参数格式
一次性提醒（schedule_type = "at"，需提供未来的 at_epoch）：
```
cron_add {
  "name": "drink_water",
  "schedule_type": "at",
  "at_epoch": 1711613100,
  "message": "该喝水了！",
  "channel": "system"
}
```
重复提醒（schedule_type = "every"，需提供正数 interval_s，单位秒）：
```
cron_add {
  "name": "drink_water_daily",
  "schedule_type": "every",
  "interval_s": 86400,
  "message": "该喝水了！",
  "channel": "system"
}
```

## 注意事项
- 时间表达要转换为具体 epoch（at_epoch）或间隔秒数（interval_s）
- 一次性任务用 schedule_type="at"（框架没有 "once"），且 at_epoch 必须是未来时间
- at 类型任务触发后默认自动删除（delete_after_run 默认为 true）
- 用自然语言确认，如"好的，5分钟后提醒你喝水"
- 如果用户没说具体提醒内容，从上下文推断
- 默认通知渠道为 system（本地推送），channel 可省略

## 示例
用户："提醒我下午2点开会"
→ get_current_time → 计算今天14:00的epoch
→ cron_add（schedule_type="at", at_epoch=算出的epoch）
→ "好的，下午2点提醒你开会 ⏰"

用户："每天早上8点提醒我喝水"
→ cron_add（schedule_type="every", interval_s=86400）
→ "已设置每天喝水提醒 💧"