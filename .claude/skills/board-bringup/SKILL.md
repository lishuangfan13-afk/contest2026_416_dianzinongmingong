---
name: board-bringup
description: BES2800BP/best1700 平台的固件构建、NTC 二进制补丁与 AP-only 烧录流程。当任务涉及编译 openvela 固件（1700_ap.sh）、打 NTC 断言补丁、dldtool 烧录、或板子启动崩溃/无法进入下载模式时使用。
---

# BES2800BP 构建-补丁-烧录

## 何时使用

- 首次在新环境编译 BES2800BP 固件，或构建失败
- 固件启动即崩溃（串口刷 assertion failed）
- 需要烧录、或板子不进下载模式
- 每次重新编译后需要重打 NTC 补丁

## 一、构建环境（Windows 宿主 + WSL）

- **必须用 WSL（Ubuntu-22.04）**。曾尝试 MSYS2/UCRT64 的 GCC 16.2 编译 NuttX host tools，`cc incdir.c` 返回 rc=1 且无错误输出——不兼容，不要重复踩。
- PATH 需含 openvela 自带 prebuilts：`prebuilts/gcc/linux-x86_64/arm-none-eabi/bin`、`prebuilts/cmake/linux-x86_64/bin`、`prebuilts/build-tools/linux-x86_64/bin`
- 环境变量：`CCACHE_DISABLE=1`、`TMPDIR=/tmp`
- 命令：`bash vendor/bes/readme/1700_ap.sh`，产物 `cmake_out/aos_evb_ap/nuttx_ap.bin`
- 耗时参考（实测）：全量 ~40 分钟（2629 步）；增量 8–10 分钟（19 步）

## 二、NTC 二进制补丁（每次重编后必做）

**背景**：板子无 NTC 热敏电阻，GPADC 通道读数为 0，启动时触发 `pmu_ntc_monitor_init` 的 `valid_cnt` 断言，板子循环崩溃。

**方案**：对固件做二进制补丁，把 `pmu_open+0x1a78` 处的 `cbnz r5`（半字 0xb955）改写为无条件跳转（0xe011）。

**防呆设计**（tools/ntc_patch.py，可直接复用）：

1. **符号级定位**：从 `nuttx_ap.elf` 解析 `pmu_open` 符号算出地址，不硬编码偏移。
2. **原指令硬校验**：改写前验证目标位置确为 0xb955，布局漂移时拒绝执行而非打错。
3. 自动同步补丁后的 bin 到 `flash/vela_2800bp/vela_2800bp/nuttx_ap.bin`。

**已知坑（真实事故）**：脚本打印的 0x57bc 是 **ELF 文件内偏移**；objcopy 平铺出的 bin 中真实偏移是 **0x47bc**（虚拟地址 0x101947bc − 基址 0x10190000）。校验固件请查 bin 的 0x47bc 处应为 0xe011。

## 三、烧录（红线区）

```powershell
cd flash\vela_2800bp\vela_2800bp
.\dldtool.exe 20 --reboot .\programmer1700_dual.bin --set-dual-chip 1 -M .\nuttx_ap.bin --pgm-rate 2000000
```

**红线（违反可能变砖）**：
- **只许 AP-only 烧录（`-M nuttx_ap.bin`）**，严禁烧 `nuttx_ota.bin` / `nuttx_bl.bin`。
- 烧录前**杀掉所有 Python 进程**，避免 COM 口被诊断脚本抢占导致烧录中断。
- 烧录时不要运行任何串口工具。
- 烧录完成后板子进入 SYS SHUTDOWN 状态，需**拔插 USB** 重启。

**进入下载模式（板子只有 POWER/RESET 两键，无 BOOT/DL 键）**：
| 板子状态 | 方法 |
|----------|------|
| 正常运行 | dldtool 加 `--reboot`（串口发复位命令） |
| 崩溃循环 | 直接启动 dldtool，按 RESET 触发 SYNC |
| OTA 引导态（串口输出 CHIP=best1700） | 直接启动 dldtool 自动 SYNC |

## 四、启动验证清单

烧录重启后，串口（921600）应看到：
1. 无 NTC 断言、系统正常进 NSH
2. `zhaoxi_ui` 界面亮起（rcS.ap 已改为启动 zhaoxi_ui 替代 lvgldemo）
3. `DIAG READY` 自检输出（各数据源就绪状态）

任何一条不满足 → 先看 `evidence-driven-debugging` 技能，不要盲目重烧。
