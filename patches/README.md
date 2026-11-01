# 朝夕集成补丁

当前完整无屏固件包含 SDK、BES 启动/挂载、NuttX 与 apps PPP 补丁。它们针对不同仓库，不能把整个目录无条件按数字顺序应用。

## SDK 固定基线与顺序

基线：`open-vela/packages_ai_agent`，`e65550f18759f086d7f544edcf17d1e31223244f`。以下序列已逐个应用到原始源码副本，并与实际编译源码逐文件比较。0008 的结尾已按真实归档修正。

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

后续补丁依赖前面的源码状态，逐个 `git apply --check /path/to/patch` 后再 `git apply /path/to/patch`。不要把历史 0001～0007 无条件叠加到此基线。

```bash
python3 tools/check_agent_patches.py \
  --pristine-agent /path/to/pristine-e65550f \
  --patched-agent /path/to/openvela/packages/ai_agent
```

此检查使用临时副本，不修改原始基线。完整构建与固件封装见 [构建脚本](../tools/build_bes_headless.py)。

独立原始副本来自官方固定提交归档。当前 SDK 顺序与构建源码逐文件比较使用上面的检查脚本，检查数量及最终结果在交付记录中报告。无屏命令、消息总线重建、worker 生命周期、Skill、cron 持久化和本机提醒均有主机回归；这些代码检查不代替实板录音与播报验收。

## apps PPP

apps 基线为 `550cd3ba60a03f8ebf9ac7b72f6eed6aea3bedbe`。0025 应用到 apps，支持当前产品的电脑直连 PPP 生命周期；普通 modem 脚本路径保留原行为。电脑直连模式按单调时钟判断连续 63 秒没有有效接收，而非以短时间连续发送的帧数判定掉线。

```bash
python3 tools/check_ppp.py --apps /path/to/openvela/apps
```

检查编译实际上游代码，覆盖短突发流量、有效接收续期、错误 CRC、失联超时与资源清理。它是主机代码回归；实际联网、模型、持久文件与音频结果另见 [电脑网络共享](../docs/电脑网络共享.md)。

## BES 启动与挂载

公共源码基线：`open-vela/vendor_bes`，`36465377d944a575ce8a0c7ba45259f5c6c61ea8`。0009 负责产品后台启动；0016 负责产品不自动格式化持久存储。已有显示启动修改应先按实际差异撤下。

实际 BSP 库来自本地已匹配配置，SHA256 和本地非公开提交边界见 [来源清单](../configs/bes_headless.sources.json)。不能仅凭公共 manifest 宣称得到相同完整工作区。

修改嵌套 `.ap` 启动文件后须触碰顶层 `rcS`/`rc.sysinit` 并检查封装 ROMFS；新构建脚本包含此操作。更改已有构建的 defconfig 后须 resetconfig。

## 历史与验收

0001～0003、0005、0007 保留历史时间、存储、DNS、daily 调度和模型适配的追溯用途，按各自基线核对。纯显示的 0004/0006 已转入 legacy，不属于当前序列。

API key、Wi-Fi 凭据及录音不进入补丁。当前电脑转发方案已有板端真实问答、备忘与待办文件回读和用户听清 TTS 的证据；完整真人录音、识别与业务链路仍在验证。各镜像结果见 [构建与验收](../docs/BES固件构建与验收.md) 和 [Wi-Fi 审计](../docs/WiFi固件审计.md)。

## NuttX 修复

基线为 `76354c637858ecb0aa4601629327acb6f44a26bb`。0022 补齐 rpmsg procfs 文件权限，避免属性查询调用空 stat 指针而重启。0024 修复 DNS TCP 分段收发指针推进和 UDP 截断回退时的 socket 泄漏。两项解析缺陷已复现，但不被认定为当前普通 UDP 查询失败的根因。

```bash
python3 tools/check_nuttx_patches.py --nuttx-root /path/to/openvela/nuttx
python3 tools/check_dns.py --nuttx-root /path/to/openvela/nuttx
```

补丁使用 Git 原始 LF 格式；工作区若自动转换了源码换行，先在干净 checkout 核对。实板对照见 [DNS 解析审计](../docs/DNS解析审计.md)。
