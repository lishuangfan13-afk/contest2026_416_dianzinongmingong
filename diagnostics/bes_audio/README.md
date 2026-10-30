# BES 音频诊断基线

本目录保存 2026 年 10 月 27 日已在 BES2800BP V1.2 板上完成的两项音频试验。P27 用于真实 PDM 采集，P30 用于真实 DAC 测试音播放。二者独立运行；当前开发目标是把输入与输出合并到同一份应用，见 [BES 单板最小交付计划](../../docs/BES最小交付计划.md)。

## 文件

| 文件 | 用途 |
| --- | --- |
| `pdm_capture_p27.c` | 16 kHz、单声道、16 bit 的板载数字麦克风采集；核对 DMA 完成、剩余量和错误；最长 5 秒 |
| `speaker_tone_p30.c` | 手动触发 1 kHz 测试音，100 至 2000 ms，PCM 峰值 128 至 8192；播放前后控制静音并检查 DMA |
| `probe.ld` | 历史诊断入口地址与布局 |
| `baseline.json` | 原镜像、源文件、链接符号表、诊断区域及成功镜像的固定校验值 |
| `../../firmware/bes_audio_diagnostics_20261027.tar.xz` | 原恢复镜像、P27、P30 和仅含符号的链接 ELF；每个成员都独立校验 |
| `../../tools/bes_audio/build_diagnostics.py` | 校验归档，或从源码重建并逐字节核对成功镜像；不连接板子、不烧录 |

这些程序依赖固定历史固件的 HAL ABI、符号地址和内存布局，包括替换入口 `0x101c7180`、运行基址 `0x10190000`、1876 字节可替换区域和原诊断数据区。只支持 `baseline.json` 中 SHA256 完全匹配的原镜像，不适合直接移植到其他固件。

## 校验和重建

只验证文件与镜像，不需要交叉编译器：

```sh
python3 tools/bes_audio/build_diagnostics.py --verify-only
python3 -m unittest discover -s tools/bes_audio -p 'test_*.py' -v
```

用历史构建所用的 ARM GCC 13.4.0 重建：

```sh
python3 tools/bes_audio/build_diagnostics.py \
  --toolchain-prefix /path/to/toolchain/bin/arm-none-eabi-
```

编译参数保留历史诊断使用的 `-mcpu=cortex-m33` Thumb 兼容指令集；该参数不用于推断整颗 SoC 的型号。脚本会拒绝与已验证 payload 或完整镜像不一致的结果。默认产物与编译日志位于仓库 `out/bes_audio/`，通过后会显示 `both rebuilt images match the archived verified images byte for byte`。

首次运行会校验并解包固件归档。归档仅含列出的平面文件名，不执行任何内置程序。脚本不会下载工具链或修改板上内容。

## 历史试验命令

P27 镜像提供：

```text
codecprobe 4096 100
codecprobe 8192 100
```

通道映射为 4096 或 8192，时长范围为 100 至 5000 ms。成功采集将原始 PCM 保存到设备 `/tmp/codex_dma.pcm`。板载麦克风对应数字 PDM 时钟 GPIO82、数据 GPIO83，依据 V1.3 原理图及 V1.2 实物响应核对；通用 `audio_fake` 节点不参与这个采集。

P30 镜像提供：

```text
codecprobe dac 200 128
codecprobe dac 2000 8192
```

最后一条是用户已确认可听见的历史测试参数，不是启动时自动执行的命令。测试喇叭接在实物 R185 下方的两针直出接口；按当时照片方向黑左红右。上方 R217 附近路径受未装 L3/L5 影响，测试无声。录音命令在 P30 中被播放命令替换。

播放试验验证了 2 秒测试音可听见。8 Ω、2 W 是喇叭的标称参数，本次未验证板端具有 2 W 输出能力或持续驱动该负载的额定能力。语音回放与持续使用要经过后续单板验收。

## 下载与恢复

归档中三个 BIN 均为 AP 单分区镜像，写入地址 `0x30190000`，禁止把本目录的 payload ELF、payload BIN 或符号 ELF 当作完整固件烧录。恢复镜像名为 `nuttx_ap_p17_original_for_recovery.bin`。

现有双向 NSH 正常响应时，可以先安排延迟执行 `reboot bootloader`，释放串口，再启动原有下载工具等待同步。它依然会重启板子；NSH 无响应时不能依赖这个方法。实际烧录应先核对当前端口、完整镜像校验值、下载器参数和 AP 地址。
