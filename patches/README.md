# 朝夕源码集成补丁

本目录把朝夕的无屏业务、实际 BES 音频和电脑供网接入 openvela。补丁按目标仓库分组：先准备匹配基线，再在各自仓库按下列顺序应用。产品应用源码位于 [app/zhaoxi](../app/zhaoxi/README.md)，构建入口见 [构建与安装](../docs/BES固件构建与验收.md)。

## 目标仓库与基线

| 目标 | 基线 | 当前补丁 |
| --- | --- | --- |
| `packages/ai_agent` | `e65550f18759f086d7f544edcf17d1e31223244f` | 下表中的 53 项，依次应用 |
| `apps` | `550cd3ba60a03f8ebf9ac7b72f6eed6aea3bedbe` | `0025 → 0042 → 0046 → 0047` |
| `nuttx` | `76354c637858ecb0aa4601629327acb6f44a26bb` | `0022 → 0024` |
| `vendor/bes` | 公共基线 `36465377d944a575ce8a0c7ba45259f5c6c61ea8` | `0009 → 0016`，再核对匹配 BSP 与本地 vendor 修改 |

BSP 库、FatFs 和依赖的准确路径、提交及 SHA256 见 [来源清单](../configs/bes_headless.sources.json)。公开 vendor 基线的 `configs/ap` 三份库与来源清单哈希一致，可复制到 `configs/apaudio3` 并把 board/chip 库后缀从 `_ap` 改为 `_apaudio3`；配置沿用 `ap` 的 CMakeLists、Make.defs 和链接脚本。仍须逐个核对哈希，不能仅凭目录名称判断兼容性。

## AI Agent 顺序

| 顺序 | 作用 |
| --- | --- |
| 0008 | 无屏入口、核心 Skill 与消息总线 |
| 0010 | BES 真实 PCM 后端与构建适配 |
| 0011 | MiMo ASR/TTS 后端和失败传播 |
| 0012 | 检查真实 WAPI 命令返回值，先启动核心业务循环 |
| 0013 | 真实 exFAT 持久挂载和 /emmc/zhaoxi |
| 0014 | checked 写入、fsync、cron 回滚与恢复副本 |
| 0015 | 不依赖 rand 的唯一提醒编号 |
| 0017 | 核心八项工具、RTC 和未授时保护 |
| 0018 | 保留已连接网络，不重复 DHCP |
| 0019 | 无线就绪等待与自动重连 |
| 0020 | station/carrier 检查、IPv4 DNS 与 TCP 截止时间 |
| 0021 | 单实例、worker 退出屏障、cron 停止唤醒，产品不启动 WS/阻塞 CLI |
| 0023 | 提醒独立投递、语音失败后的本机提示音、拥堵保留和最近到期时间唤醒 |
| 0026 | 网络就绪接受启用的 PPP 接口，保留 carrier 和有效 IPv4 检查 |
| 0027 | Agent 停止再启动时重建工具 schema，避免工具列表为空 |
| 0028 | BES TLS 接收失败时保留 errno、FD 与 flags 诊断，不记录密钥或网络内容 |
| 0029 | 产品文件工具结果返回下一模型轮，支持首次文件创建、读后写和真实结果确认 |
| 0030 | 产品 TLS 清理使用非阻塞关闭通知，增加停机阶段与工作线程计数诊断 |
| 0031 | 录音工作线程先启动采集再读取 PCM；启动失败唤醒调用方并回收线程 |
| 0032 | 语音报时使用中文自然表达，内部时间工具保留真实时间与 epoch |
| 0033 | 产品 TLS 请求由调用任务独立拥有连接，避免跨任务 FD 复用 |
| 0034 | 本机能量与停顿检测、自动识别与重听，播报期间关闭采集 |
| 0035 | 自动语音最终出站完成回调，恢复下一轮收音 |
| 0036 | 1.2 秒封句、提示音、校准复用、超长整句与残句拒绝、停止与轮次隔离、真实语音阶段 |
| 0037 | 入站和出站失败收束、取消轮次检查、连续语音共享上下文、语音简短中文回答 |
| 0038 | HTTPS 正文有界增量接收，支持 TTS SSE 分片与截止、错误传播 |
| 0039 | 固定 64 KiB PCM 队列分离网络接收与 DMA 播放，保留原任务的连接所有权 |
| 0040 | 本产品官方 MiMo v2.6-flash 请求显式关闭深度思考，覆盖普通与工具入口 |
| 0041 | 播放短碎片合并、首播预算与 EOF 冲刷，增加队列欠载和 DMA 时间统计 |
| 0043 | NuttX PCM 消费线程显式使用 RR 优先级 110，及时处理 DMA 完成，保留属性与线程创建失败检查 |
| 0044 | 流式 TLS 的接收超时切片、暂时无数据和中断可重试，仍遵守绝对响应截止时间 |
| 0045 | 首播改为 30,720 字节或最多 640ms 的抖动缓冲，EOF 仍即时冲刷短音频 |
| 0048 | TLS 请求完整写入与 socket 队列、状态诊断；不记录请求内容或凭据，TLS 接受字节不等于对端已确认 |
| 0049 | 拒绝小数及溢出的提醒参数；产品普通周期提醒重启后跳过错过次数并继续；未授时等待、恢复保存失败回滚 |
| 0050 | 产品明确记事／待办修改必须有本轮对应文件的成功写入证据，避免口头虚报与缓存复用；edit_file 保存失败传播 |
| 0051 | 明确的自然语言提醒创建／取消也要求本轮 cron_add／cron_remove 成功证据；一次纠正仍无证据则不确认完成 |
| 0052 | 先收紧回答缓存，仅允许满足条件的查询答案，拒绝副作用请求与未验证结果 |
| 0053 | 保留 daily 与旧记忆读取，按 JSON 结构恢复会话历史 |
| 0054 | 在自动语音识别后匹配唤醒前缀、管理会话空闲窗口与本机休眠 |
| 0055 | 电脑会话先结束自己启动的 Agent，再清理 PPP，明确退出所有权 |
| 0056 | 产品默认等待电脑 PPP，保留显式选择 Wi-Fi 的运行模式 |
| 0057 | 明确中文记事、待办与一次性相对提醒走本地 hook，复用历史、分发和轮次逻辑 |
| 0058 | 增加显式启用的 PPP TLS 接收窗口实验，默认保持原行为 |
| 0059 | 朝夕产品配置关闭回答缓存，每次问答按本轮请求处理，通用配置保留缓存 |
| 0060 | `/remind` 使用完整整数与 JSON 参数，保存后读回确认，拒绝截断和无效时间 |
| 0061 | 用稳定清理编号回收工作线程，去重退出回调并保护线程计数与取消临界区 |
| 0062 | 统一比赛指定的双语 ASR 唤醒短语，短噪声被丢弃时释放会话忙状态 |
| 0063 | 本机提示音与云端播放共用采集互斥，串行处理并恢复失败及停止状态 |
| 0064 | 普通 HTTP 复用严格响应解析，处理合法分块、尾字段、临时响应与 HEAD/304，拒绝截断、非法长度和容量超限 |
| 0065 | Agent 初始化后自动检测“你好，夕夕”及同音转写；完整播报“你好”后接受问题，取消与失败恢复，默认关闭的诊断开关 |
| 0066 | 晨间任务生命周期、批次许可的语音入口与音频互斥、可取消且有总时限的独立后台 TLS、当天资讯的对话定位 |
| 0067 | 语音 JSON／SSE 全流程预算与取消、失败请求立即释放 TCP 队列、显式 Wi-Fi 模式下可选发送队列／TLS 记录／TCP MSS 对照配置 |
| 0068 | 独立的可取消 HTTPS GET，限定连接与完整请求预算，供公开资讯静态 JSON 下载使用 |

在 `packages/ai_agent` 中逐个执行 `git apply --check /path/to/patch`，检查成功后再执行 `git apply /path/to/patch`。完整顺序以 [check_agent_patches.py](../tools/check_agent_patches.py) 的 `SERIES` 为准。

应用后在本仓库根目录核对实际源码：

```bash
python3 tools/check_agent_patches.py \
  --pristine-agent /path/to/pristine-e65550f \
  --patched-agent /path/to/openvela/packages/ai_agent
```

检查使用临时副本回放完整序列，再比较实际工作区。当前序列为 53 项，按完整序列回放并与实际构建工作区核对。

0050／0051 把业务完成确认与本轮工具执行连接起来：文件修改匹配目标业务路径、调用 ID 和成功保存结果；提醒创建／取消匹配成功的 `cron_add`／`cron_remove`。修改请求跳过回答缓存，缺少结果时纠正一次，再返回实际状态。产品的本地 `note` 与 `todo add/list/done` 分别由 `journal.c` 和 `todo_store.c` 执行。0057 用 weak hook 将有限的中文快捷指令接到这些模块与真实 cron，匹配后直接保存并回读；说明见 [本地中文指令](../docs/本地中文指令.md)。0059 在产品配置中完全关闭回答缓存，会话历史继续用于对话上下文。

0058 的接收窗口仅供有意选择的传输实验，默认值与生产入口保持原行为。使用文档不依赖这一实验参数。

## apps PPP

按 `0025 → 0042 → 0046 → 0047` 应用。

| 补丁 | 作用 |
| --- | --- |
| 0025 | 电脑直连 PPP 生命周期、资源清理与按有效接收时间判断链路存活 |
| 0042 | 每实例 512 字节串口接收缓冲，立即消费剩余数据，区分可重试与真实读取错误 |
| 0046 | 电脑直连模式协商 asyncmap 0，减少额外转义，主机网关同步支持 |
| 0047 | 收齐转义帧，等待串口空间后一次提交，限制普通日志与 PPP 帧交叉 |

0047 的发送边界对应遵守串口 `write` 锁的普通任务；中断级直接输出仍由底层驱动处理。正数短写后结束会话，避免重发已输出的帧前缀。等待发送空间时保留停止检查与单调时钟截止时间。

普通 modem 使用原接收和发送路径。电脑的 [start.py](../tools/pc_link/start.py) 与 [gateway.py](../tools/pc_link/gateway.py) 两端启用 `TCP_NODELAY`，及时提交控制帧和 ACK；开发板固件与主机工具一起更新。

```bash
python3 tools/check_ppp.py --apps /path/to/openvela/apps
```

该入口覆盖 5 组实际源码回归和补丁回放，包括生命周期、突发帧、接收缓存、转义协商、整帧发送与清理。

## BES 启动与 NuttX

0009 在启动脚本中后台运行产品 Agent，0016 使用真实持久挂载并保留现有存储。修改 `.ap` 启动包含文件后需更新顶层 `rcS`／`rc.sysinit`，让 CMake 重新封装 ROMFS；[构建脚本](../tools/build_bes_headless.py) 已包含该步骤。

NuttX 的 0022 补全 rpmsg procfs 的属性查询，0024 修复 DNS TCP 分段收发与截断回退的 socket 清理。

```bash
python3 tools/check_nuttx_patches.py --nuttx-root /path/to/openvela/nuttx
python3 tools/check_dns.py --nuttx-root /path/to/openvela/nuttx
```

## 产品检查入口

```bash
python3 tools/check_headless.py \
  --agent-root /path/to/openvela/packages/ai_agent \
  --arm-prefix /path/to/arm-none-eabi-

python3 tests/headless/pc_link/check_tls_write_diagnostics.py \
  --sdk /path/to/openvela/packages/ai_agent

python3 tests/headless/check_cron_edges.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON

python3 tests/headless/pc_link/check_mutation_guard.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON
```

这些入口分别检查产品命令与本地存储、TLS 写入诊断、提醒恢复和业务执行确认。本地中文与喝水分支还通过实际 Agent 工作线程检查路由、单次语音回复与取消后的轮次回收。当前镜像、实际安装和演示流程见 [构建与验收](../docs/BES固件构建与验收.md)。

## 历史补丁

0001～0003、0005、0007 留作早期模型、时间与存储实现的参考；0004、0006 的显示相关内容位于 [legacy](../legacy/README.md)。当前无屏构建使用上表中的完整序列。

补丁采用 Git 原始 LF 格式。密钥和设备数据由运行时配置提供，源代码与资产模板保持可公开复用。
