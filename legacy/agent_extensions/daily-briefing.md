# 每日简报

每天早上主动为用户生成个性化简报。

## 何时使用
- 用户问"今天有什么安排"、"早安"、"今日简报"
- 用户收到 cron 定时推送的简报提醒后回复触发（cron.json 中 id=brief，kind=every，interval_s=86400；框架触发时通过 action=write_file 把提醒文案写入 /data/ai_agent/REMINDER.md 供 UI 显示，但不会自动调用本 skill；every 从进程启动时刻起算，不保证恰在早 8 点。另有 id=wake 的 kind=daily 任务可定点触发，但需框架补丁支持）

## 执行步骤
1. 使用 get_current_time 获取今天的日期和时间
2. 读取 /data/ai_agent/config/USER.md 了解用户城市和偏好
3. 使用 get_weather（或 web_search）查询用户所在城市的天气
   - 搜索关键词："XX城市 天气 今天"
   - 提取温度、天气状况、建议
   - 查询成功后调用 write_file 把天气持久化，供 UI 天气卡片显示：
     - path="/data/ai_agent/WEATHER.md"
     - content 为**单行、紧凑**的文本，如 `北京 12℃ 晴`
     - **只写第一行**：不要 markdown 标题（不要 `#`）、不要以空行开头，因为 UI 只读取第一个非空行；城市、温度、天气状况三者齐全即可
     - 示例：`write_file(path="/data/ai_agent/WEATHER.md", content="北京 12℃ 晴")`
4. 读取 /data/ai_agent/TASKS.md 获取待办事项
5. 使用 web_search 查询今日科技/AI 新闻（用户偏好）
   - 搜索关键词："科技新闻 今天" 或 "AI 新闻 今天"
6. 汇总成简洁的简报

## 输出格式
📋 朝夕早报 · MM月DD日 周X

☀️ 天气：城市 温度 天气状况，建议穿XXX

📝 今日待办：
- 待办项1
- 待办项2
（如无待办则显示"今天没有待办事项，可以轻松一些"）

📰 新闻速览：
1. 新闻标题 — 一句话摘要
2. ...
（3条即可）

💡 今日提醒：（如有cron提醒则列出，否则省略此节）

## 注意
- 必须执行工具获取真实数据，不要输出模板
- 回复用中文，简洁友好
- 如果天气查询失败，跳过天气部分，不要编造；也不要写 WEATHER.md（避免 UI 显示错误数据）
- WEATHER.md 的 content 必须单行且非空行开头
