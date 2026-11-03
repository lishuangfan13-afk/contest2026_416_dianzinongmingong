# zhaoxi 产品应用

`app/zhaoxi` 是朝夕在 openvela 上的产品入口，提供问答、记事与待办、定时提醒，以及麦克风、喇叭和电脑供网的接入。应用以 flat 模式与 `packages/ai_agent` 共用地址空间，后台 Agent 负责模型、工具、调度和语音服务。

## 模块分工

| 文件 | 作用 |
| --- | --- |
| [zhaoxi_main.c](zhaoxi_main.c) | 命令解析、业务路由、语音启停与状态查询 |
| [journal.c](journal.c) | 北京日期的本地备忘追加、同步与回读确认 |
| [drink.c](drink.c) | 独立喝水周期任务与启停状态 |
| [notifications](notifications/) | 固定收件目标的飞书通知、鉴权与服务回执 |
| [todo_store.c](todo_store.c) | 本地待办添加、查询、精确完成与持久保存 |
| [reminder.c](reminder.c) | 本机相对时间提醒、查询、取消和到期播报／提示音 |
| [bes_audio.c](bes_audio.c) | 实际 BES PCM／DMA／DAC 录音与播放 |
| [mimo_voice.c](mimo_voice.c)、[mimo_sse.c](mimo_sse.c) | MiMo ASR、增量 TTS、SSE 与 base64 音频解析 |
| [bes_clock.c](bes_clock.c) | 板端 RTC 与时间接口 |
| [pc_link.c](pc_link.c)、[pc_control.c](pc_control.c) | PPP 网络、控制命令、路由和 DNS 生命周期 |
| [dns_diag.c](dns_diag.c) | DNS 信息、本机解析对照和网络诊断 |
| [uplink_boot.c](uplink_boot.c)、[uplink_flow.c](uplink_flow.c) | 联网前本地选网、连接验证、失败重问与取消 |
| [uplink_keywords.c](uplink_keywords.c)、[uplink_voice.c](uplink_voice.c) | 个人 MFCC/DTW 模板、训练验证、录音与开机提示音播放 |

## 集成到工作区

1. 将本目录链接为 openvela 工作区中的 `packages/zhaoxi`。
2. 按 [补丁说明](../../patches/README.md) 应用完整 SDK 与相关仓库补丁。
3. 使用 [无屏 defconfig](../../configs/bes_headless.defconfig)，保留匹配的 BES 音频与 BSP 配置。
4. 完成业务资产和服务配置后，使用 `ai_agent --headless &` 启动后台 Agent。

后台模式保留 NSH 供维护。通过电脑供网启动时，主机入口会编排旧实例退出、UTC 授时、PPP 与业务启动。日常操作见 [作品首页](../../README.md)，完整重建见 [构建与验收](../../docs/BES固件构建与验收.md)。

## 命令接口

下面是板端 NSH 中的命令。PPP 运行期间，通过 `python tools/pc_link/control.py --cmd ...` 发送同样的参数，例如 `--cmd --text todo list`。

| 命令 | 行为 |
| --- | --- |
| `zhaoxi [--text] ask <问题>` | 向 Agent 提交问答 |
| `zhaoxi [--text] note [add] <内容>` | 本地追加当天备忘，保存后回读 |
| `zhaoxi [--text] note list` | 本地读取当天备忘 |
| `zhaoxi [--text] todo add <内容>` | 本地添加待办并返回编号 |
| `zhaoxi [--text] todo list` | 本地查询未完成待办 |
| `zhaoxi [--text] todo done <编号>` | 本地完成指定待办 |
| `zhaoxi [--text] remind <时间与事项>` | 通过 Agent 理解并创建提醒 |
| `zhaoxi [--text] remind in <秒数> <事项>` | 本机直接创建相对时间提醒 |
| `zhaoxi [--text] remind list` | 查询提醒 |
| `zhaoxi [--text] remind cancel <八位编号>` | 取消指定提醒并保存 |
| `zhaoxi say <文本>` | 直接合成并播放文本 |
| `zhaoxi test-speaker` | 播放两秒本机提示音 |
| `zhaoxi auto start\|stop\|status` | 管理自动语音循环 |
| `zhaoxi auto wake on\|off\|status` | 设置或查询 ASR 唤醒 gate |
| `zhaoxi [--text] drink start [分钟]`、`stop`、`status` | 管理独立喝水提醒 |
| `zhaoxi notify status`、`test <事件编号>`、`send <事件编号> <文本>` | 固定目标通知与服务回执 |
| `zhaoxi listen start\|stop` | 手动分段录音诊断 |
| `zhaoxi status`、`zhaoxi stop` | 查询消息总线或请求停止 Agent |

默认回复通道是 `voice`，`--text` 改为串口文字。模型类命令先入队，再由 Agent 返回业务结果；明确的备忘和待办命令直接执行存储操作。合并后的业务请求上限为 511 个 UTF-8 字节，包含应用增加的意图前缀。

## 本地备忘

`journal.c` 把明确的 `note add/list` 命令映射到北京时间对应的 `memory/daily/YYYY-MM-DD.md`。新增以追加方式保留旧内容，写入后 `fsync` 并回读比对原前缀和新增文本，检查文件路径仍对应同一对象，再返回确认。记录文本上限 511 个 UTF-8 字节，每日文件上限 64 KiB。

本地备忘需要有效 UTC 时钟，日期换算由模块统一完成。`--text` 可在 Agent 未启动时使用；默认 voice 入口将已验证的结果交给语音播报。错误回执区分未保存和存储状态待核对，便于通过当天文件查询实际内容。

## 本地待办

待办文件为 `AGENT_DATA_DIR/TASKS.md`，当前根目录为 `/emmc/zhaoxi`。格式示例：

```markdown
- [ ] [1] 带学生证
- [x] [2] 交作业
```

新增使用现有最大编号加一，范围 1～9999；查询只列未完成项；完成只修改指定编号的复选框。重复文本可以有不同编号，已完成历史和其他原文继续保留。文件上限为 64 KiB，重复编号、无编号待办或格式错误会返回错误，保留原文件供整理。

同进程操作用互斥锁串行执行，保存采用临时文件、完整写入检查、刷新、`fsync`、关闭及重命名。`--text todo` 可在 Agent 未启动时使用；voice 入口先检查消息总线。保存成功而语音提交失败时，应用提示查询本地结果。

## Agent 的执行确认

0050 为明确的记事／待办修改请求检查本轮目标业务路径、工具调用 ID 与成功的 `write_file`／`edit_file` 结果。0051 对提醒创建／取消检查对应的 `cron_add`／`cron_remove` 成功结果。缺少结果时，Agent 纠正一次工具调用流程；仍未完成则回复未确认。查看与普通问答继续使用原流程。

修改请求跳过回答缓存，避免复用之前的完成确认。`edit_file` 对短写、刷新、同步和关闭失败返回错误。完成回复对应实际工具执行，内容可通过文件和提醒查询入口回读。

## 提醒与语音

本机相对时间提醒支持 1～31536000 秒的整数，先检查时钟和存储，再调用实际 cron 服务。到期时通过语音通道播报；网络或合成失败时输出提醒文字并尝试两秒本机提示音。周期恢复由 0049 实现：无外部 action 的固定间隔任务跳过错过的周期，继续安排未来一次；未授时则等待时间恢复。

TTS 使用 24 kHz PCM16LE 单声道，HTTPS 增量读取 SSE 后写入固定 64 KiB 队列，播放线程由 DMA 完成中断唤醒。ASR 在封句后上传整段音频。自动语音采用约 1.2 秒静音封句，识别与播报时暂停采集。联网后的 ASR 唤醒 gate 默认匹配“你好，夕夕”及配置的同音词，自动进入等待唤醒，会话内空闲 60 秒后返回；具体字段和调用接口见 [唤醒短语](../../docs/ASR唤醒短语.md)。自然唤醒、问答及新闻完整播报已有真人确认，长时间连续运行与真实早七点观察仍待验收。

扩展接口分别见 [喝水提醒](../../docs/喝水提醒.md)、[飞书通知](../../docs/飞书通知接口.md) 与 [颜文字屏](../../docs/颜文字屏使用.md)。

## 开机本地选网

已配置设备启用 `uplink_choice_enabled="true"` 后，联网前用个人训练的 MFCC/DTW 模板识别“有线连接／无线连接”，**不是通用 ASR**。三个 ZXP 资产 `uplink_choose`、`uplink_ok`、`uplink_fail` 分别本地播报询问、成功和失败；选网前不接热点、不调用云端 ASR／TTS，未知语句不选默认网络。选择后验证接口及外网往返，失败释放连接后重新询问。

现有设备的个人模板已训练并独立验证，无需重录。新板需要额外安装选网及训练提示音、完成每类三份模板与校准／独立验证、保留原配置后启用；目前没有一键完成这些步骤的通用安装工具。有线需要电脑 boot watcher 待命，可用当前用户 Windows 登录工具 `boot_startup.py` 的 `--install`、`--status`、`--stop`、`--uninstall` 管理，state 路径规则见 [电脑入口](../../tools/pc_link/README_zh-cn.md)。

本轮无线、无 PC 有线失败后改选无线成功及本地提示音流畅已确认；PPP console lease 与 SNTP 非阻塞共享三秒预算修复后，真实有线已通过 PPP、授时、成功本地播报、进入等待唤醒及正常停止全链路，板端／串口／网关清理均确认。Windows 登录任务已安装且配置验证通过，真实任务启动至待命及正常停止清理已通过；最新 SNTP 固件无线开机复测也已通过；重新登录触发仍待观察。部署、操作和验收边界见 [开机语音选网](../../docs/开机语音选网.md)；有线 PPP 下的语音连续播放策略、指标和限制见 [对话语音连续播放](../../docs/对话语音连续播放.md)；旧 `uplink_mode` 模式与退出规则见 [联网方式](../../docs/联网方式配置.md)。

## 检查入口

在 Linux／WSL 中运行，路径替换为实际工作区：

```bash
python3 tests/headless/check_journal.py

python3 tests/headless/check_drink.py --sdk /path/to/openvela/packages/ai_agent --cjson /path/to/openvela/apps/netutils/cjson/cJSON

python3 tools/check_headless.py \
  --agent-root /path/to/openvela/packages/ai_agent \
  --arm-prefix /path/to/arm-none-eabi-

python3 tests/headless/pc_link/check_mutation_guard.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON

python3 tests/headless/check_cron_edges.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON

python3 tests/headless/voice/check_bes_dma_irq.py \
  --sdk /path/to/openvela/packages/ai_agent
```

消息总线、本地待办、文件执行确认、提醒恢复、SSE、PCM 队列与 DMA 各有实际源码回归。其余检查和当前 AP 镜像信息汇总在 [构建与验收](../../docs/BES固件构建与验收.md)。

本版发布固件包含固定提示音本地播放、开机“联网成功”播报、可取消的公开资讯静态 JSON 下载，以及确认后修改长期人设。安装、配置和验收边界见 [本地语音与独立资讯](../../docs/本地语音与独立资讯.md)。用户个人 fork 的资讯工作流与 Pages 已上线，实板下载、去重及语音人设取消／确认已验收，真实早七点观察尚待进行。
