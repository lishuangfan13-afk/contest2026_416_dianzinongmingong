# 朝夕无屏核心资产

当前仅部署问答所需的人格配置、智能记事本和智能提醒。默认 cron.json 是空任务列表；每日简报、喝水和久坐等扩展资产保存在 legacy/agent_extensions，首版不部署。启用无屏配置后，原生 Skill 加载器只安装并发现 note-taker 与 reminder；设备残留的旧扩展文件不会进入当前摘要。

## 部署清单

| 文件 | 设备路径 |
| --- | --- |
| SOUL.md、USER.md | <BASE>/config/ |
| MEMORY.md | <BASE>/memory/MEMORY.md |
| cron.json | <BASE>/cron.json |
| note-taker.md、reminder.md | <BASE>/skills/ |

当前实板 <BASE> 是持久存储 /emmc/zhaoxi；历史固件可能使用 /data/ai_agent 或 /data/agent。资产中的 {{AGENT_DATA_DIR}} 是部署占位符，tools/deploy_assets.ps1 会在写入和回读校验时替换为同一个实际根目录。

## 业务执行

问答复用原 AI Agent；记事由现有 read_file、write_file 执行；提醒由 cron_add、cron_list、cron_remove 执行，到期消息通过 channel=voice 进入原语音输出。创建普通提醒不再使用屏幕文件作为出口。

部署前先执行 zhaoxi stop 并等待 Shutdown complete；部署后重新运行 ai_agent --headless &，使调度列表重新加载。首次尚未启动时可直接部署。不要同时启动两个 Agent。

部署更新人格配置和业务 Skill；MEMORY.md、cron.json 仅在目标文件不存在时初始化，已有用户数据保留。当前固件使用实测 exFAT 挂载；历史 tmpfs 上的资产和记录不能保证跨重启保留。

## 入口

应用 assets 部署完成并配置好原模型与语音服务后，以 ai_agent --headless 启动后台 Agent，并使用 zhaoxi 命令或原语音输入通道提交请求。--headless 和总线就绪检查需要 patches/0008。

首版成功的定义是实际工具完成操作、问答返回真实内容、到期提醒能听见。串口显示请求已提交只说明进入了消息队列。
