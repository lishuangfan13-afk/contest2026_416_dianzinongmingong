# 智能提醒
创建、查询和取消定时提醒，到期通过现有语音通道播报。

## 创建
1. 使用 get_current_time 获取当前日期、时间和时区。
2. 核对用户要提醒的事项与时间。缺少任一项时先询问。
3. 一次性提醒使用 cron_add，schedule_type 为 at，at_epoch 必须在未来。
4. 固定间隔重复使用 every 和 interval_s；间隔必须为正整数。
5. “每天八点”只能在工具 schema 明确支持 daily 时使用 hour 和 minute。不能用每 86400 秒冒充每天固定时间。不支持的周期或时间表达应说明并询问替代方式。
6. 所有新提醒使用 channel 为 voice、chat_id 为 zhaoxi。普通提醒不设置 action 或 action_args，不再写 REMINDER.md。
7. 检查 cron_add 结果。成功后复述事项、实际时间和任务编号；失败则说明未创建。用户撤销时用该编号取消。

## 示例参数
以下 at_epoch 仅表示字段格式，执行时必须根据当前时间重新计算，不能直接使用示例数字。

一次性：
    {"name":"交作业","schedule_type":"at","at_epoch":2000000000,"message":"该交作业了。","channel":"voice","chat_id":"zhaoxi"}

每两小时：
    {"name":"喝水","schedule_type":"every","interval_s":7200,"message":"该喝水了。","channel":"voice","chat_id":"zhaoxi"}

## 查询与取消
使用 cron_list 读取当前任务。取消前核对任务编号和事项；调用 cron_remove 后检查实际结果。不要通过清空 cron.json 来取消单个任务。

## 交付边界
创建成功表示调度器接收了任务。到期播报需要语音合成服务和播放通道可用。播放失败不能声称用户已经听到提醒。当前首版不自动建立任何健康提醒或每日简报。
