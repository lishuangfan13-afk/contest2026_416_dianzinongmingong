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
- 烧录前正常关闭占用目标 COM 口的诊断脚本、供网会话和终端，确认串口已释放；不要结束无关 Python 进程。
- 烧录时不要运行任何串口工具。
- `--reboot` 表示**下载操作完成后重启**，不是让运行中的固件主动进入下载模式。未使用该选项时，完成后可能进入 SYS SHUTDOWN；根据实际状态用 POWER 或重新供电恢复。

**进入下载模式（板子只有 POWER/RESET 两键，无 BOOT/DL 键）**：
| 板子状态 | 方法 |
|----------|------|
| 正常运行且 NSH 可用 | 正常停止 Agent，按下节安排后台延迟 `reboot bootloader`，立即释放串口并在同一进程内启动 dldtool；已实板验证无需按 RESET |
| 崩溃循环 | 直接启动 dldtool，按 RESET 触发 SYNC |
| OTA 引导态（串口输出 CHIP=best1700） | 直接启动 dldtool 自动 SYNC |

`CHIP=best1700` 本身只说明运行到了引导代码，不保证停留在下载模式；须看到下载工具完成 SYNC 确认。

### 无需按 RESET 的已验证流程

前提是板子已开机且 NSH 能执行命令。PPP 或其他串口会话须先正常结束；DTR、RTS 均为 False，不用于硬件复位。

1. 通过 NSH 正常停止 Agent，等待 `Shutdown complete`，结合 `zhaoxi status` 和 `ps` 确认已退出。支持 `sync` 的固件执行 `sync`；当前候选镜像未编入此命令，本次复用原镜像且不修改持久文件，临时脚本只写 `/tmp`。
2. 在板端执行以下命令，并确认后台启动回执 `sh [`：

   ```sh
   echo sleep 5 > /tmp/zhaoxi_flash_reset
   echo reboot bootloader >> /tmp/zhaoxi_flash_reset
   source /tmp/zhaoxi_flash_reset &
   ```

3. **在同一个 Python 程序内连续执行**关闭 pyserial 串口句柄、立即 `subprocess.Popen(args, cwd=runtime_directory)`、`wait(timeout=90)`。下载器继承真实终端，不使用 stdout 管道或 `CREATE_NO_WINDOW`。不要拆成两次 AI 工具调用，也不要关闭串口后等待 5 秒。
4. 参数列表使用 `-b 921600 <COM数字> -v --no-retry --reboot --sector-size-kib 64 <programmer1700_dual.bin> --set-dual-chip 1 -M <AP镜像绝对路径> --addr 0x30190000 --pgm-rate 2000000`。启动前核对镜像大小、SHA256、manifest 地址、末尾 AP 地址及 NTC 补丁。`--reboot` 只负责完成后重启。
5. 必须看到 `[SYNC]: Started`、`[SYNC]: Confirmed`、`Burn File starts`、`PROGRAMMING SUCCEEDED` 和退出码 0。下载器释放串口后回读启动、Agent 和自动语音状态，并确认持久数据保留。

**2026-11-03 实板通过**：COM20 / 921600，复用 AP 镜像 SHA-256 `19e657b19148aad422c76a1f11849bbf4040451925fb7a9bf540d5fe65fc8db1`。后台任务启动至下载器创建约 266ms，5 秒后完成 SYNC，13.766 秒完成下载操作，退出码 0，`manual_reset_required=false`。板子自动重启，Agent、唤醒监听、人设和原资讯文件均已确认。先前“立即发 reboot 后才启动下载器”的失败测试未复现正确时序，不能用来否定免按键方案。完整记录见 [自动复位核验](../../../docs/bringup_logs/20261103_auto_reset_verification.md)。

NSH 卡死、未开机或 FTDI 串口桥异常时不能套用此流程；分别恢复主控、使用 POWER 开机或恢复电脑端 USB 串口，再判断是否具备上述前提。

## 四、启动验证清单

烧录重启后，串口（921600）应看到：
1. 无 NTC 断言、系统正常进 NSH
2. `zhaoxi_ui` 界面亮起（rcS.ap 已改为启动 zhaoxi_ui 替代 lvgldemo）
3. `DIAG READY` 自检输出（各数据源就绪状态）

任何一条不满足 → 先看 `evidence-driven-debugging` 技能，不要盲目重烧。
