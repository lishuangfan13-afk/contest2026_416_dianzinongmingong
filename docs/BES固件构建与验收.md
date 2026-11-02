# BES 固件构建、安装与检查

本页面向接手朝夕的软件维护者，说明当前 AP 镜像、完整工作区要求、源码集成、烧录与资产安装。日常体验从 [作品首页](../README.md) 和 [核心功能说明](无屏核心功能.md) 开始。

## 当前 AP 构建

| 项目 | 配置 |
| --- | --- |
| 开发板 | BES2800BP EVB，板级目录 `vendor/bes/boards/best1700_ep/aos_evb` |
| 产品配置 | `apheadless`，匹配 `apaudio3` BSP 库 |
| 镜像 | [nuttx_ap_headless.bin](../firmware/headless_pc_link/nuttx_ap_headless.bin) |
| 镜像大小 | 1,362,288 字节 |
| SHA256 | `0f4078aafd755bc0cf48d246c02736cbe5493bb05edcc6f4fe49884fd5474e64` |
| AP 烧录地址 | `0x30190000` |
| 持久数据目录 | `/emmc/zhaoxi` |
| 来源与校验 | [固件 manifest](../firmware/headless_pc_link/nuttx_ap_headless.manifest.json) |

BES 源码现已整合电脑 PPP、实际音频、MiMo ASR／增量 TTS、本地备忘与待办、明确中文指令、提醒调度、喝水任务、开机唤醒、长期人设、晨间资讯和固定目标飞书通知。EasyMatrix 作为独立显示终端，由电脑主机桥连接。2026-11-02 发布固件和 manifest 已成对更新，完成 AP 烧录、人设与资讯重启保存、资讯同意／拒绝／复播实测，见 [本轮验收](2026-11-02唤醒与晨间资讯验收.md)。热点独立供网、持续在线资讯接口及真实七点无人值守仍待验证。

## 工作区与固定依赖

本仓库提供产品应用、业务资产、配置和集成补丁。完整编译还需要 openvela 工作区、相应子仓库，以及与板级 ABI 匹配的 BES 预编译库。

| 组件 | 固定版本或要求 |
| --- | --- |
| NuttX | `76354c637858ecb0aa4601629327acb6f44a26bb` |
| apps | `550cd3ba60a03f8ebf9ac7b72f6eed6aea3bedbe` |
| AI Agent | `e65550f18759f086d7f544edcf17d1e31223244f` |
| BSP | `apaudio3` 三个静态库，按来源清单核对 SHA256 |
| FatFs | `6e433f195f15e0f5d8a3b170f53694b9c4dc6f4b`，完整展开到 NuttX 的 FatFs 子目录 |
| 已使用工具 | ARM GCC 13.4.0、CMake 3.22.1、Ninja 1.10.1，Linux／WSL |

[来源清单](../configs/bes_headless.sources.json) 记录每个 BSP 库、FatFs 及其他依赖的路径和校验值。`vendor_bes_local_commit` 来自原本地工作区。已核对：公开 vendor 基线 `36465377d944a575ce8a0c7ba45259f5c6c61ea8` 的 `configs/ap` 三份库与该清单哈希完全一致；复制配置为 `apaudio3`，将 board/chip 库后缀 `_ap` 改为 `_apaudio3` 后仍逐个校验；公开 vendor 基线与所用补丁见 [补丁说明](../patches/README.md)。

## 集成与完整构建

以下命令在本仓库根目录运行，`/path/to/openvela` 替换为完整工作区。

1. 将 `app/zhaoxi` 链接为工作区的 `packages/zhaoxi`，让 packages 的 CMake／Kconfig 发现 `ai_agent` 和 `zhaoxi`。原圆屏 `zhaoxi_ui` 保存在 [legacy](../legacy/README.md)，当前构建使用新的无屏入口。
2. 按 [补丁顺序](../patches/README.md) 分别更新 SDK、apps、vendor 与 NuttX。SDK 顺序以 `tools/check_agent_patches.py` 的 `SERIES` 为准，apps 顺序为 `0025 → 0042 → 0046 → 0047`。
3. 核对实际源码与完整补丁序列：

   ```bash
   python3 tools/check_agent_patches.py \
     --pristine-agent /path/to/pristine-e65550f \
     --patched-agent /path/to/openvela/packages/ai_agent

   python3 tools/check_ppp.py --apps /path/to/openvela/apps
   ```

4. 执行完整 AP 构建与封装：

   ```bash
   python3 tools/build_bes_headless.py \
     --workspace /path/to/openvela \
     --output firmware/headless_pc_link
   ```

已有成功构建目录后，普通 C 代码修改使用增量入口，保留已有对象，不运行 `resetconfig`：

```bash
python3 tools/build_bes_headless.py \
  --workspace /path/to/openvela \
  --output /path/to/candidate \
  --incremental
```

修改 defconfig、板级配置或首次建立工作区时使用上面的完整配置入口。`--stage-only` 只重新封装，不能替代修改源码后的编译。

脚本检查 BSP 库和 FatFs，安装 [完整 defconfig](../configs/bes_headless.defconfig)，完成 `resetconfig`、CMake、编译和链接。随后检查生成的 ROMFS，确认后台入口为 `ai_agent --headless &`，持久挂载配置保留现有数据。

封装阶段依据 ELF 中的 `pmu_open` 定位已验证的 NTC 指令，检查原值后应用产品调整，并核对 BES 尾部 AP 地址，生成 BIN 与 manifest。维护已有构建、只需重新封装时使用：

```bash
python3 tools/build_bes_headless.py \
  --workspace /path/to/openvela \
  --output firmware/headless_pc_link \
  --stage-only
```

`--stage-only` 使用已经完成的构建。修改普通代码后先增量编译，修改配置后重新配置和编译；修改 `.ap` 启动包含文件后要更新顶层启动文件，完整配置入口已处理这一关系。`firmware/headless` 保留历史 R10，当前电脑供网镜像写入 `firmware/headless_pc_link`。

## 烧录与安装

### 1. 写入 AP

准备厂商 `dldtool.exe` 和 `programmer1700_dual.bin`，选择双芯片模式，将封装后的 `nuttx_ap_headless.bin` 写到 AP 地址 `0x30190000`。使用当前正常的 USB2UART 与供电接法，引导同步期间按下载工具提示操作；正常系统也可通过软件重启进入下载流程。

本镜像更新 AP，APC1 与引导固件沿用板上已有版本。烧录前用 manifest 核对镜像大小、SHA256 和地址。厂商板级资料见 [aos_evb 开发说明](https://github.com/open-vela/vendor_bes/blob/dev-ai-contest-2026/boards/best1700_ep/aos_evb/README_zh-cn.md)。

### 2. 部署业务资产

先用 `python tools/pc_link/launch.py --stop` 正常结束电脑供网。串口回到 NSH 后，若后台助手仍在运行，执行 `zhaoxi stop` 并等到 `Shutdown complete`。关闭占用串口的终端，再在 PowerShell 部署：

```powershell
powershell -File tools/deploy_assets.ps1 -Port COM3 -Baud 921600 -BaseDir /emmc/zhaoxi
```

部署内容为 SOUL、USER、MEMORY、空白 cron 模板和两个业务 Skill。脚本替换数据根目录并回读检查，已有 MEMORY 和 cron 保留；TASKS 与每日备忘由业务操作维护。

### 3. 配置服务并启动

服务凭据保存在板端 `/emmc/zhaoxi/config/config.json`，沿用 AI Agent 的运行时配置。准备可用的文本、ASR 和 TTS 服务账号后，配置 MiMo 文本模型与语音服务端点；产品适配使用 `mimo-v2.6-flash`、`mimo-v2.5-asr` 和 `mimo-v2.5-tts`。

API key、Wi-Fi 密码和设备上的真实记录保留在本机或板子中，公开仓库使用干净的资产模板。已配置板子按 [Windows 入口说明](../tools/pc_link/README_zh-cn.md) 启动电脑供网，主机在建立业务前完成 UTC 授时。

### 4. 冷启动与运行

1. 完全断电后重新接好供电与 USB2UART，按住 **POWER 约 2 秒**开机，看到启动输出后松开。
2. 在电脑运行 `python tools/pc_link/launch.py --port COM3`，等待 UTC 授时和 `PPP_UPLINK_READY`。
3. 通过 `control.py --time` 核对时间，再发送业务命令。结束用 `launch.py --stop`，等待正常结束。

POWER 与 RESET 在外壳上分别保留可触达位置；POWER 用于冷启动开机，RESET 用于运行中的复位恢复。

## 本地中文指令与回答处理

0057 把有限的明确中文记事、待办和一次性相对提醒，直接交给产品本地存储与调度；产品 hook 还支持喝水提醒的开启、查询与关闭。例如“三十秒后提醒我喝水”无需请求文本模型；命中后使用原消息分发和语音输出。普通问题、复合指令及未支持的日期继续走 Agent。句式表见 [本地中文指令](本地中文指令.md)，追加与回读机制见 [本地记事](本地记事.md)。

0059 对朝夕产品配置关闭回答结果缓存，每轮按当前问题和会话上下文生成回答，常用本地意图直接处理。工具与 schema 缓存继续保留，通用 SDK 配置沿用原回答缓存行为。

## 扩展屏与模块配置

- [喝水提醒](喝水提醒.md)：使用独立本机周期任务，明确中文整句或 `drink start/stop/status` 管理，默认间隔 60 分钟。
- [ASR 唤醒短语](ASR唤醒短语.md)：自动语音开启后匹配名称前缀，会话内保持 60 秒空闲窗口。
- [飞书通知](飞书通知接口.md)：显式启用设备配置中的固定目标，通过 `notify` 命令发送并读取服务回执。
- [联网方式](联网方式配置.md)：产品默认等待电脑 PPP；`uplink_mode` 可明确选择 Wi-Fi。
- [颜文字屏](颜文字屏使用.md)：EasyMatrix 经独立 CH343 USB 串口显示状态，`tools/display_link.py` 管理表情与恢复时钟。

EasyMatrix 源码、依赖锁和分区说明位于 [extension/EasyMatrix](../extension/EasyMatrix/README_zh-cn.md)。[屏端发布应用](../firmware/easymatrix/EasyMatrix.ino.bin) 大小 1,194,544 字节，SHA256 为 `6f8fd7b2335664e55538f56e0a1e32bfcde8f574922259607ec628732d8da5ba`；[配套 manifest](../firmware/easymatrix/EasyMatrix.ino.manifest.json) 记录 `0x10000` 应用地址与源码校验。原板 Flash 备份留在私密工作目录。

## 源码检查

当前 SDK 的集成补丁已从固定提交回放，并与实际构建工作区的 28 个目标文件比较。Windows 主机回归 87 项、显示桥回归 53 项通过；apps、存储、语音与通知模块各自保留检查入口，运行版本由发布 manifest 对齐。

| 检查入口 | 覆盖内容 |
| --- | --- |
| `tests/headless/check_local_intent.py` | 明确中文句式、本地执行、提醒读回、取消与真实线程路由 |
| `tests/headless/check_journal.py` | 北京日期、本地追加、回读与写入错误 |
| `tests/headless/check_drink.py` | 喝水任务归属、周期设置、并发与存储错误 |
| `tests/headless/voice/check_voice_wake.py` | 指定双语 ASR 前缀、会话窗口、短噪声恢复、休眠与停止 |
| `tests/headless/voice/check_local_alert_auto.py` | 实际提醒与自动采集互斥、错误恢复、并发播放及播放中停止监听 |
| `tests/headless/pc_link/check_memory_history.py` | 每日记忆保留与 JSON 历史读取 |
| `tests/headless/pc_link/check_network_policy.py` | 产品默认电脑供网与显式 Wi-Fi 策略 |
| `tests/headless/pc_link/check_owned_shutdown.py` | 有线会话与 Agent 的退出顺序 |
| `tools/check_headless.py` | 产品命令、真实消息总线、本地待办、生命周期、Skill、持久化和 ARM 对象 |
| `tests/headless/pc_link/check_mutation_guard.py` | 0050／0051 的本轮文件／提醒工具执行确认 |
| `tests/headless/check_cron_edges.py` | 提醒参数、UTC、周期恢复、未授时等待和保存错误回滚 |
| `tests/headless/voice/check_bes_dma_irq.py` | DMA 完成唤醒、取消、超时与旧信号边界 |
| `tests/headless/voice/check_mimo_sse.py` | 实际 SSE、base64、PCM 分段与结束边界 |
| `tests/headless/voice/check_tts_queue.py` | 有界播放队列、背压、停止与预缓冲 |
| `tests/headless/pc_link/check_tls_buffered_framing.py` | 整份生产 HTTP 解析、合法分块扩展与尾字段、长度与截断错误、1xx／HEAD／304 |
| `tests/headless/pc_link/check_tls_stream.py` | HTTPS 分片、接收恢复与绝对截止时间 |
| `tests/headless/pc_link/check_tls_write_diagnostics.py` | 请求完整写入、socket 状态与失败反馈 |
| `tools/check_ppp.py` | 生命周期、批量接收、转义协商和整帧发送 |

主要运行方式：

```bash
python3 tools/check_headless.py \
  --agent-root /path/to/openvela/packages/ai_agent \
  --arm-prefix /path/to/arm-none-eabi-

python3 tests/headless/pc_link/check_mutation_guard.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON

python3 tests/headless/check_cron_edges.py \
  --sdk /path/to/openvela/packages/ai_agent \
  --cjson /path/to/openvela/apps/netutils/cjson/cJSON
```

Windows 主机检查：

```powershell
python tools/pc_link/test_transport.py
python tools/pc_link/test_launcher.py
powershell -File tests/headless/test_assets.ps1
```

## 实物检查与版本记录

本轮最终 AP 在麦克风始终关闭的条件下完成 37 项布尔检查，全部通过：

| 检查范围 | 实际结果 |
| --- | --- |
| 电脑供网与 UTC | 正式入口完成授时、控制通信和真实模型请求 |
| 直接备忘与待办 | 追加、查询、精确完成及旧内容保留通过；直接备忘约 0.346s，待办约 0.337～0.397s |
| 中文快捷指令 | 记事、待办、三十秒相对提醒、喝水开启／状态／重复开启／关闭均走本地，文本模型调用为零 |
| 定时提醒 | 创建、同编号回读、到期移除、取消后不投递，以及既有任务保留通过 |
| 问答 | 15 次真实请求均取得对应回答；板端模型阶段中位数 2.139s，范围 1.754～3.830s |
| 退出与串口 | `quit` 返回 0，主机确认真实关闭串口 |
| 连续启停 | 同一固件连续 10 轮完成启动、对应问题的正确回答与正常退出，无回答缓存命中；串口、接收线程及网关均完成清理，退出后无残留 Agent |

10 轮启停均无需按 RESET，每轮完整启动至停机约 19.868～24.908 秒；这个时间包含启动与清理，问答耗时按上表单独记录。

EasyMatrix 的七种表情、协议回执、帧更新、TTL 恢复和状态读回通过，原有 23 项像素钟配置保持。实物交接按 [交付清单](交付核对清单.md) 复核供电、POWER、接口、数据回读和合盖后的声音。

[历史语音优化记录](语音响应优化.md) 保存早期镜像的首声与队列测量，便于后续调优。本轮文字结果与当前发布版本按 manifest 对齐，现场语音和外部通知配置见首页状态说明。
