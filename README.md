# 朝夕 · 无屏 AI 生活管家

2026 年 10 月 31 日至 11 月 1 日的当前范围：在 BES2800BP 单板上完成**问答、记事与待办、定时提醒**。复用原有 AI Agent 和音频服务，以语音交互。电脑可经 USB2UART 为板子提供网络；ASR、模型、工具和 TTS 仍在板上调用。像素钟、圆屏、每日简报、健康提醒、飞书和微信退出首版范围。

## 当前功能

| 功能 | 入口示例 | 处理方式 |
| --- | --- | --- |
| 问答 | “解释一下 DMA” | 原模型处理文字或识别结果，经现有语音通道回答 |
| 记事与待办 | “记一下明天带学生证”“添加待办交作业”“完成第二项” | 读取文件后修改；稳定编号；支持查询；写入成功才确认 |
| 定时提醒 | “十分钟后提醒我交作业”“查看提醒”“取消这个提醒” | 原 cron 工具创建、查询、取消；到期直接发送语音消息 |

默认不预置任何定时任务。问答、文件操作和提醒调度复用 `packages/ai_agent`；当前仓库提供产品命令入口、两项业务 Skill、部署资产和对应集成补丁。

## 使用入口

产品交互采用一次启动后的自动循环：本机检测说话和停顿，识别并处理，播报结束后重新等待。播报时暂停采集；静默时不调用云端 ASR。实现和主机回归见 [自动语音终端](docs/自动语音终端.md)，Windows 一键供网与主动开启入口见 [使用说明](tools/pc_link/README_zh-cn.md)。

**已进行两分钟真人自动对话：四段语音中三段完成回答，一段失败后恢复。** 该次镜像为 `5e1bee6c…`，用户指出响应明显偏慢；不能据此认定识别率、延迟和所有语音业务已验收。后续交互修复与使用规则见 [语音交互收尾](docs/语音交互收尾.md)。监听默认关闭，只在用户主动开启后运行。

下面保留开发诊断入口，供隔离录音或服务问题使用。完成下述构建和资产部署后，在 NSH 中运行：

```text
ai_agent --headless &
zhaoxi status
zhaoxi listen start
zhaoxi listen stop
```

手动录音结束后，后续交给原识别、模型与播放服务处理；正常自动语音模式不需要逐句调用这两个命令。也可以直接从串口提交文字诊断：

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

完整 AP 固件已多次实际编译和烧录。电脑 PPP 供网已完成板端原生 DNS、TLS 和真实模型请求，并在持续会话中通过文字问答、备忘写入回读、两项待办中只完成指定项、到期提醒及取消后不投递。备忘在 AP 更新后仍保留。用户已确认云端合成的语音可以清楚听到，并确认一次语音提问后收到了时间回答。

后续已修正录音启动竞争、产品文件工具提前结束、TLS 跨任务连接复用和原始时间格式直接口播等问题。自动对话实板测量见 [本轮记录](docs/bringup_logs/20261031_auto_voice.json)；每个结果的镜像与入口归属见 [持续实板记录](docs/bringup_logs/20261031_pc_ppp_continuous.json)。板载 Wi-Fi 稳定性没有因电脑供网方案而被证明已修复。

[完整构建、镜像校验和实板结果](docs/BES固件构建与验收.md) · [Wi-Fi 独立审计](docs/WiFi固件审计.md)

R10 增量：真实本机 DNS 对照通过；原始查询诊断已接入，本机提醒可直接创建、查询、取消和按期串口投递。具体结果见 [DNS 解析审计](docs/DNS解析审计.md)。
