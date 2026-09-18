# 智能记事本

将用户的自然语言转化为结构化的待办、备忘或日程。

## 何时使用
- 用户说"记一下"、"帮我记"、"备忘"、"提醒我"
- 用户说"添加待办"、"我要做XXX"
- 用户说"今天的日程"、"我有什么待办"

## 执行步骤

### 录入模式
1. 使用 get_current_time 获取当前时间
2. 分析用户意图，分类为：
   - **待办**（TODO）：有明确行动项 → 存入 /data/agent/TASKS.md
   - **备忘**（MEMO）：纯记录信息 → 存入 /data/agent/memory/daily/YYYY-MM-DD.md
   - **提醒**（REMINDER）：有时间约束 → 调用 cron_add 创建定时提醒（必填参数：name、schedule_type（"at" 一次性 / "every" 重复）、message；at 用 at_epoch，every 用 interval_s）
3. 写入对应文件，格式：
   - 待办：- [ ] [YYYY-MM-DD] 描述
   - 备忘：- [HH:MM] 内容
4. 确认已记录，用简短友好的语气回复

### 查询模式
1. 读取 /data/agent/TASKS.md 显示待办
2. 读取 /data/agent/memory/daily/ 当天的日记
3. 汇总展示

## 示例
用户："记一下明天下午3点开会"
→ 分类：提醒（有时间约束）
→ get_current_time → 计算明天15:00的epoch
→ cron_add 创建提醒
→ "好的，已设置明天下午3点开会提醒 ✅"

用户："帮我记一下，张三的电话是13800138000"
→ 分类：备忘（纯信息记录）
→ write_file 追加到今天的日记
→ "已记录 📝"

用户："我有什么待办？"
→ read_file /data/agent/TASKS.md
→ 列出未完成的待办项

## 语言
- 默认中文
- 确认消息简短，如"已记录"、"已添加待办"