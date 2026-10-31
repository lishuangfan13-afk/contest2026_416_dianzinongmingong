# 朝夕 · 无屏 AI 生活管家

2026 年 10 月 30 日的当前范围：在 BES2800BP 单板上完成**问答、记事与待办、定时提醒**。复用原有 AI Agent 和音频服务，以语音交互，串口用于调试。像素钟、圆屏、每日简报、健康提醒、飞书和微信退出首版范围。

## 当前功能

| 功能 | 入口示例 | 处理方式 |
| --- | --- | --- |
| 问答 | “解释一下 DMA” | 原模型处理文字或识别结果，经现有语音通道回答 |
| 记事与待办 | “记一下明天带学生证”“添加待办交作业”“完成第二项” | 读取文件后修改；稳定编号；支持查询；写入成功才确认 |
| 定时提醒 | “十分钟后提醒我交作业”“查看提醒”“取消这个提醒” | 原 cron 工具创建、查询、取消；到期直接发送语音消息 |

默认不预置任何定时任务。问答、文件操作和提醒调度复用 `packages/ai_agent`；当前仓库提供产品命令入口、两项业务 Skill、部署资产和对应集成补丁。

## 使用入口

完成下述构建和资产部署后，在 NSH 中运行：

```text
ai_agent --headless &
zhaoxi status
zhaoxi listen start
zhaoxi listen stop
```

说话后结束录音，后续交给原识别、模型与播放服务处理。也可以直接从串口提交：

```text
zhaoxi ask 什么是DMA
zhaoxi note 明天带学生证
zhaoxi note list
zhaoxi todo add 交作业
zhaoxi todo list
zhaoxi todo done 2
zhaoxi remind 十分钟后提醒我交作业
zhaoxi remind list
zhaoxi remind cancel a123b456
```

默认回复走 `voice` 通道；`zhaoxi --text ask 你好` 将回复改到串口。`请求已提交` 只表示进入队列，实际操作结果由助手依据工具返回确认。

## 构建与部署

使用比赛 openvela 工作区及原 BES AP 构建入口，保留已验证的音频配置。

1. 同步本仓库 manifest，使 `app/zhaoxi` 链接到 `packages/zhaoxi`；旧 `zhaoxi_ui` 链接退出构建。
2. 按 [补丁说明](patches/README.md) 应用当前无屏补丁，核对 AI Agent 与 BES BSP 的基线版本。
3. 合并 [无屏配置](configs/bes_headless.fragment)。当前入口要求 flat 构建，与 Agent 共用地址空间；不适用于独立进程构建。
4. 构建实际 AP 固件，完成原模型、联网及语音服务配置。
5. 当前无屏配置使用实际挂载的持久存储 `/emmc/zhaoxi`。停止后台 Agent，等待日志中的实际退出确认后，再更新资产；已有备忘和提醒默认保留：

```powershell
pwsh -File tools/deploy_assets.ps1 -Port COM3 -Baud 921600 -BaseDir /emmc/zhaoxi
```

部署后重新运行 `ai_agent --headless &`，让 Skill 和提醒列表重新加载。不要同时启动第二个 Agent 实例。完整步骤、数据保留规则及验收场景见 [无屏核心功能](docs/无屏核心功能.md)。

## 代码与资料

- [app/zhaoxi](app/zhaoxi/)：无 LVGL 依赖的命令入口。
- [assets/agent_deploy](assets/agent_deploy/README.md)：六个核心资产、两项业务 Skill、空白默认调度列表。
- [开发计划](docs/development_plan.md)：当前范围、实现和验收顺序。
- [BES 最小交付](docs/BES最小交付计划.md)：当前功能阶段与硬件资料的关系。
- [legacy](legacy/README.md)：原屏幕应用、字体、显示补丁和扩展 Skill。它们保留历史价值，当前不编译、不部署。
- [音频诊断基线](diagnostics/bes_audio/README.md)：已有诊断源码、重建工具和镜像。
- [既往硬件证据](docs/bringup_logs/20261027_audio_evidence.json)、[本轮检查记录](docs/bringup_logs/20261030_headless_checks.json)。

## 本轮检查范围

命令入口已经做 Cortex-M55 对象编译；主机测试连接真实上游消息总线，覆盖业务路由、中文、错误输入、队列满和关闭期间并发请求；两种数据根目录的资产渲染与部署脚本语法也有检查。原生 Skill 加载检查确认旧扩展不会进入当前配置，普通配置仍保留原功能。重现方法见 [应用说明](app/zhaoxi/README.md)。

完整 AP 固件已编译、烧录。当前实板版本为 headless_r10：本机音频发声、持久化文件、按编号取消提醒、真实定时投递、RTC、单实例及退出后重启已有实测；Pro 热点曾取得 IP，但 DNS/互联网尚未通过，板端联网问答和完整语音业务仍待验收。工具诊断通过不等于自然语言业务已经完成。

[完整构建、镜像校验和实板结果](docs/BES固件构建与验收.md) · [Wi-Fi 独立审计](docs/WiFi固件审计.md)

R10 增量：真实本机 DNS 对照通过；原始查询诊断已接入，本机提醒可直接创建、查询、取消和按期串口投递。具体结果见 [DNS 解析审计](docs/DNS解析审计.md)。
