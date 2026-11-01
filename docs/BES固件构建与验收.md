# BES 无屏固件构建与实板验收

本记录对应 2026 年 10 月 30 日至 11 月 1 日的单板集成，目标为问答、记事与待办、定时提醒。电脑供网已取得板端问答、文件业务、提醒和云端播报证据；两分钟真人自动对话完成三次回答，响应速度仍需优化。

## 当前电脑供网与交互修复版

当前烧入镜像见 [AP 固件](../firmware/headless_pc_link/nuttx_ap_headless.bin) 和 [来源清单](../firmware/headless_pc_link/nuttx_ap_headless.manifest.json)：

```text
大小：1322000 字节
SHA256：c19ddd07801675f8aafce01db45519fc2f6bdd5f2b2814b627bb83a0827d4c2c
烧录地址：0x30190000
```

此镜像包含电脑 PPP、任务内独立 TLS、文件工作流、中文报时及自动语音，并加入 0036/0037 的交互边界修复：1.2 秒静音封句、初次校准复用、可说话与收句短提示、超长及被提醒打断的整句拒绝、停止和迟到回复隔离、连续问答保留上下文、语音默认简短中文，以及合成与播放状态区分。开机默认不监听。

25 项 SDK 补丁从官方固定提交顺序应用，并与实际构建涉及的 21 个文件逐字节比较通过。实际 SDK 自动语音与业务边界 ASan/UBSan、手动录音顺序、主机 45 项回归及核心业务回归通过。完整 AP 已编译、链接并通过软件复位下载，仅更新 AP。

先前 `5e1bee6c…` 镜像实测两分钟真人自动对话：4 段中 3 段完成回答，1 段失败恢复。识别完成至开始播放分别约 4.7、13.0、30.1 秒；不能据此宣称新修复版的听感、延迟和识别率已经复测。见 [测量记录](bringup_logs/20261031_auto_voice.json)。

本轮最后实板检查只使用文字与只读状态，保持麦克风关闭。具体结果见 [收尾验收](bringup_logs/20261101_voice_logic.json)。完整语音业务验收标志仍为 false；本轮没有无人说话录音，也没有将旧镜像的真人结果移算到新镜像。

当前仍使用整段音频下载后播放；真正的音频流式播放和真人速度复测尚未完成。板载 Wi-Fi 的间歇联网、个别旧助手退出等待也仍需单独处理。日常交互见 [语音交互收尾](语音交互收尾.md)，运行入口见 [使用说明](../tools/pc_link/README_zh-cn.md)。

## R10 历史证据

| 项目 | 实际结果 | 范围 |
| --- | --- | --- |
| 完整 AP 编译、链接、烧录 | 通过 | 运行版本 headless_r10，AP 地址 `0x30190000`；不改 APC1 或引导固件 |
| 自动启动 | 通过 | 实板 `/etc/init.d/rcS` 含后台 Agent；保持 NSH 可用 |
| 本机音频播放 | 通过 | 真实 DMA、内部 DAC；用户明确听到两秒提示音；不是主机替身 |
| MiMo 文本、ASR、TTS 接口 | PC 侧 HTTP 200 | 文本返回、ASR 转写、24 kHz PCM16 均实测；尚未证明板端请求成功 |
| 备忘持久化 | 工具层通过 | 真实注册工具写入、回读；固件更新和 RESET 后仍保留 |
| 待办修改 | 工具层通过 | 独立验收目录中两项待办，仅第一项变为完成；另一项保留 |
| 提醒创建、查询、取消 | 工具层通过 | 同时创建两项，编号不同；仅取消指定项 |
| 实时调度 | 串口投递通过 | 短时提醒到期投递；已取消任务未触发；已触发的一次性任务从列表移除 |
| RTC | 重启保留通过 | 实际 UTC epoch 与电脑时间一致；本地业务时间为 UTC+8 |
| 网络 | 尚未稳定通过 | 曾对近距离手机热点完成关联、四次握手和 DHCP；后续关联失败，DNS/互联网未通过 |
| 自然语言业务和到期语音 | 待验收 | 不把工具诊断入口、排队提示或“正在分析”算作最终成功 |

实板验收使用全新的 `/emmc/zhaoxi/validation-*` 目录。没有覆盖正式待办；测试提醒按其唯一 ID 清理。公开证据见 [本轮实板记录](bringup_logs/20261030_headless_board.json)。

## 历史 R10 镜像

仓库交付 [nuttx_ap_headless.bin](../firmware/headless/nuttx_ap_headless.bin) 及同目录 manifest。它与此前实际烧入的 `nuttx_ap_headless_r10.bin` 字节一致：

```text
大小：1305328 字节
SHA256：0ebb41634cd6ba1796de5064990cba93a9b7bed6dfc52462654779a063598300
烧录地址：0x30190000
```

`headless_r10` 是本轮集成版本名，与历史“V5”宣传演示固件无关。镜像不携带用户 API key、Wi-Fi 凭据或录音。这些配置位于设备的 `/emmc/zhaoxi/config/config.json`，不提交 Git。

## 构建输入

[完整配置](../configs/bes_headless.defconfig) 保存了实际成功编译所用配置；[来源清单](../configs/bes_headless.sources.json) 记录 NuttX、apps、SDK、FatFs 等依赖提交和 BSP 库 SHA256。

实际环境是 Linux/WSL、ARM GCC 13.4.0、CMake 3.22.1、Ninja 1.10.1。使用 `apaudio3` 的三份已匹配 BSP 库，新增配置目录名为 `apheadless`。不要把配置目录名与底层库 ABI 名称混用。

本地 vendor 工作区包含未公开的历史提交。清单中的该提交不被宣称为可从公共远端直接获取；复现需要取得 SHA256 匹配的 BSP 库，并核对本地 vendor 修改。仅同步公共 manifest 尚不能证明得到同一固件。

工作区必须有实际 FatFs 子仓库 `nuttx/fs/fatfs/fatfs/source/ff.c`，版本见清单。设备 eMMC 是 exFAT；不能用内置 FAT 驱动挂载失败后格式化的方法处理。产品启动脚本不包含 `autoformat`。

## 源码集成与构建

1. 准备所列依赖，将本仓库 `app/zhaoxi` 链接为 `packages/zhaoxi`，移除旧的显示应用构建链接。
2. 按 [补丁说明](../patches/README.md) 分别应用 SDK、vendor 与 NuttX 补丁；不要对整个补丁目录无条件排序应用。
3. 确认 packages 根目录的 CMake/Kconfig/Make 入口能发现 `ai_agent` 和 `zhaoxi`。本地使用 `nuttx_add_subdirectory()`、`nuttx_generate_kconfig()`，Kconfig 包含两项应用；已有 packages 入口应合并，不能覆盖其他工程。
4. 执行下面的完整构建命令。脚本会检查 BSP 库校验值、安装已验证配置、重载配置、触发 ROMFS 重新生成并检查封装内容，再输出 AP 镜像。

```bash
python3 contest2026_416_dianzinongmingong/tools/build_bes_headless.py \
  --workspace /path/to/openvela
```

已有构建只重新检查和封装：

```bash
python3 contest2026_416_dianzinongmingong/tools/build_bes_headless.py \
  --workspace /path/to/openvela --stage-only
```

脚本只构建和封装，不下载依赖、不自动应用补丁、不格式化、不烧录。NTC 补丁根据 ELF 中 `pmu_open` 定位，先检查原指令 `0xb955`，再改为 `0xe011`；保留 BES 包装尾部和 AP 地址。不能用旧工具直接 `objcopy` 重造并丢失包装元数据。

修改 `rcS.ap` 或 `rc.sysinit.ap` 后必须触碰其顶层包含文件，保证 CMake 重新生成 ROMFS。已有构建更改 defconfig 后应执行 `resetconfig`；仅重新 configure 不能证明新配置已生效。这两项已在构建脚本中处理。

## 配置和资产

当前数据根目录是持久存储 `/emmc/zhaoxi`。固件只有确认它挂载为 FatFs 后才启动 Agent，避免把备忘或提醒误存到 tmpfs。更新资产前先执行：

```text
zhaoxi stop
```

等待 `Shutdown complete` 后部署：

```powershell
pwsh -File tools/deploy_assets.ps1 -Port COM3 -Baud 921600 -BaseDir /emmc/zhaoxi
```

资产更新默认保留已有 `MEMORY.md` 和 `cron.json`，不清空用户记录。私密服务配置单独部署。之后运行一次 `ai_agent --headless &`；开机已启动时不要再启动第二份。

MiMo 文本模型使用 `mimo-v2.6-flash`；语音使用 `mimo-v2.5-asr`、`mimo-v2.5-tts`，接口为 `https://token-plan-cn.xiaomimimo.com/v1/chat/completions`。本轮依据比赛授权使用用户提供的 Token Plan 凭据，已完成 PC 协议对照。TTS 目前是获取完整 PCM 后分块送入播放接口，不宣称已经实现网络 SSE 流式播放。

## 独立联网审计与后续实测

0020 修复 station 判定、IPv4 DNS 查询及 TCP 超时，0021 修复重复启动和后台任务关闭竞争。R7 实板验证：拒绝第二实例；停止后 ps 中 Agent 线程归零；重新启动正常；Pro 取得地址。云问答随后在 DNS 阶段失败，并在约 15 秒给出最终错误，未算成功问答。

R8 仅增只读 MAC 对照：radio getter 与内核 getter 均 rc=0，六字节地址完全一致。PS255 已追到本地 APC1 库，明确为“使用默认配置”的哨兵值。已检查的网络结构 ABI、MAC 复制及 WPA2 参数未发现错误；闭源无线固件与空口数据通信仍不能据此保证无误。

USB 重新插拔后观察到整板重新启动、时钟回到 1970。当前 RTC 的暖重启保留不等于完整断电保留，断电后必须重新授时；当前新提醒创建有未授时保护。未实现板端自动联网授时验收。

R8 阶段重新验证了 15 秒延迟软件引导复位，下载约 11 秒完成，未需人工 RESET。当时配置优先手机热点；后续电脑供网结果见本文当前候选章节。

## 检查命令和边界

```bash
python3 tools/check_agent_patches.py \
  --pristine-agent /path/to/pristine-e65550f \
  --patched-agent /path/to/openvela/packages/ai_agent

python3 tools/check_headless.py \
  --agent-root /path/to/openvela/packages/ai_agent \
  --arm-prefix /path/to/arm-none-eabi-
```

SDK 补丁逐个应用到原始源码副本，并与实际构建源码逐文件比较通过。主机测试覆盖真实消息总线、40 次并发关闭/重建、核心 Skill 筛选及真实 cron 持久化；后者包括写满失败回滚、恢复副本、损坏文件拒绝和 `rand()` 恒零时编号仍唯一。ASan/UBSan 检查通过。Windows 资产检查使用 `tests/headless/test_assets.ps1`。

工具诊断入口 `zhaoxi --tool <name> /tmp/args.json` 调用真实工具注册表，用于读写与调度验收。它不经过 ASR 或 LLM。接下来仍需完成板端联网问答、自然语言记事/待办/提醒，以及到期语音播报，才能判定三项产品功能全部跑通。

## R10 DNS 与提醒增量

R10 已完整烧录。实板真实 resolver 的本机 A、CNAME+A、NXDOMAIN 对照通过；配置恢复及 Agent 重启通过。当前外部原始 DNS 查询在无IP条件下返回 ENETUNREACH，尚未接收到可用于解析的公网应答。历史有IP后的超时仍需重捕获，不把天线或解析器直接认定为唯一根因。详见 [DNS 解析审计](DNS解析审计.md) 及 [本轮证据](bringup_logs/20261031_dns_board.json)。

0022 修复已复现的 procfs属性查询重启，R9/R10实板验证通过。0023 增加本机提醒、提示音回退、拥堵保留及最近到期时间唤醒；真实三秒CLI提醒在五秒窗口内投递。0024 修复两项已复现的DNS TCP路径缺陷；本次小米UDP回复约150字节、不带TC，不将它们当成本次已证根因。全部自动检查与主观听音边界分别记录。
