# BES 固件构建、安装与检查

本页面向接手朝夕的软件维护者，说明当前 AP 镜像、完整工作区要求、源码集成、烧录与资产安装。日常体验从 [作品首页](../README.md) 和 [核心功能说明](无屏核心功能.md) 开始。

## 当前 AP 构建

| 项目 | 配置 |
| --- | --- |
| 开发板 | BES2800BP EVB，板级目录 `vendor/bes/boards/best1700_ep/aos_evb` |
| 产品配置 | `apheadless`，匹配 `apaudio3` BSP 库 |
| 镜像 | [nuttx_ap_headless.bin](../firmware/headless_pc_link/nuttx_ap_headless.bin) |
| 镜像大小 | 1,337,384 字节 |
| AP 烧录地址 | `0x30190000` |
| 持久数据目录 | `/emmc/zhaoxi` |
| 来源与校验 | [固件 manifest](../firmware/headless_pc_link/nuttx_ap_headless.manifest.json) |

本轮已编译并烧入的 AP SHA256：

```text
0c915c384692083bff6b78a0d18f73b9d462b3a649ca49908a05a45928119704
```

镜像包含电脑 PPP、BES 实际音频、MiMo ASR／增量 TTS、本地待办、提醒调度和 0050／0051 业务执行确认。最新镜像的统一实物流程正在复测；当前演示以电脑供网、文字业务与语音播报为主，连续自动语音和板载 Wi-Fi 独立联网继续联调。

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

[来源清单](../configs/bes_headless.sources.json) 记录每个 BSP 库、FatFs 及其他依赖的路径和校验值。`vendor_bes_local_commit` 来自本地工作区，复现时需要按清单准备对应库和 vendor 修改；公开 vendor 基线与所用补丁见 [补丁说明](../patches/README.md)。

## 集成与完整构建

以下命令在本仓库根目录运行，`/path/to/openvela` 替换为完整工作区。

1. 将 `app/zhaoxi` 链接为工作区的 `packages/zhaoxi`，让 packages 的 CMake／Kconfig 发现 `ai_agent` 和 `zhaoxi`。原圆屏 `zhaoxi_ui` 保存在 [legacy](../legacy/README.md)，当前构建使用新的无屏入口。
2. 按 [补丁顺序](../patches/README.md) 分别更新 SDK、apps、vendor 与 NuttX。SDK 当前 36 项，apps 顺序为 `0025 → 0042 → 0046 → 0047`。
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

脚本检查 BSP 库和 FatFs，安装 [完整 defconfig](../configs/bes_headless.defconfig)，完成 `resetconfig`、CMake、编译和链接。随后检查生成的 ROMFS，确认后台入口为 `ai_agent --headless &`，持久挂载配置保留现有数据。

封装阶段依据 ELF 中的 `pmu_open` 定位已验证的 NTC 指令，检查原值后应用产品调整，并核对 BES 尾部 AP 地址，生成 BIN 与 manifest。维护已有构建、只需重新封装时使用：

```bash
python3 tools/build_bes_headless.py \
  --workspace /path/to/openvela \
  --output firmware/headless_pc_link \
  --stage-only
```

`--stage-only` 使用已经完成的构建。修改代码或配置后，先完成完整构建；修改 `.ap` 启动包含文件后要更新顶层启动文件，脚本已处理这一关系。`firmware/headless` 保留历史 R10，当前电脑供网镜像写入 `firmware/headless_pc_link`。

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

## 源码检查

当前 SDK 的 36 个补丁已从固定提交回放，并与实际工作区涉及的 23 个文件比较。Windows 主机已有 63 项回归，apps 有 5 组源码回归及补丁回放。

| 检查入口 | 覆盖内容 |
| --- | --- |
| `tools/check_headless.py` | 产品命令、真实消息总线、本地待办、生命周期、Skill、持久化和 ARM 对象 |
| `tests/headless/pc_link/check_mutation_guard.py` | 0050／0051 的本轮文件／提醒工具执行确认 |
| `tests/headless/check_cron_edges.py` | 提醒参数、UTC、周期恢复、未授时等待和保存错误回滚 |
| `tests/headless/voice/check_bes_dma_irq.py` | DMA 完成唤醒、取消、超时与旧信号边界 |
| `tests/headless/voice/check_mimo_sse.py` | 实际 SSE、base64、PCM 分段与结束边界 |
| `tests/headless/voice/check_tts_queue.py` | 有界播放队列、背压、停止与预缓冲 |
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

交接时按 [交付清单](交付核对清单.md) 依次检查时间、问答、记录回读、待办精确完成和提醒到期／取消。前一 `f006b24e…` 镜像已完成文字问答、备忘、本地待办和本机提醒操作；当前 `0c915c38…` 加入自然语言提醒的工具确认，统一流程复测仍在进行。

[历史语音优化记录](语音响应优化.md) 保存 `4e22a196…` 等镜像的首声与队列测量，便于后续调优。当前维护重点为连续自动语音的响应和恢复，以及板载 Wi-Fi 独立联网。日常演示继续采用电脑供网方案。
