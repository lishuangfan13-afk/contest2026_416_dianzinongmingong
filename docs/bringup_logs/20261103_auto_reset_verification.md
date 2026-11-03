# 无需按 RESET 的烧录流程：实板通过

2026-11-03，按用户提供的后台延迟交接方式，成功复烧当前 BES2800BP AP 镜像。**全程无需人工 RESET，下载器退出码 0，烧录后自动启动并恢复唤醒监听。** 此结果修正了早期根据不完整时序得出的判断。

| 项目 | 结果 |
| --- | --- |
| 串口 | COM20，921600；pyserial DTR / RTS 均为 False |
| 重启安排 | 后台 `sleep 5`，随后 `reboot bootloader`；已收到 `sh [302:255]` |
| 交接 | 同一 Python 进程中关闭串口并立即启动 dldtool；从安排后台任务起约 266ms |
| 下载终端 | 继承终端，无 stdout 管道、无 CREATE_NO_WINDOW |
| 同步 | 下载器约 5.187 秒收到 Started、5.203 秒 Confirmed |
| 烧录 | 仅 AP，地址 0x30190000，1,364,168 字节；PROGRAMMING SUCCEEDED |
| 退出 | DOWNLOADER_EXIT 0；下载器总处理时间 13.766 秒 |
| 重启后 | Agent 运行、自动监听等待唤醒词，人设回读正常，原资讯文件仍在 |

镜像 SHA-256：`19e657b19148aad422c76a1f11849bbf4040451925fb7a9bf540d5fe65fc8db1`。复用已有镜像，未重新编译。烧录前已核对 manifest、文件大小、SHA256、末尾 AP 地址和 NTC 补丁。

## 正确时序

先正常结束 PPP 等串口会话、停止 Agent 并确认实际退出。通过 NSH 写入并回读临时脚本：

```sh
echo sleep 5 > /tmp/zhaoxi_flash_reset
echo reboot bootloader >> /tmp/zhaoxi_flash_reset
source /tmp/zhaoxi_flash_reset &
```

后台启动回执到达后，**同一个 Python 程序立即关闭串口并启动下载器**，不跨 AI 工具调用，不等待 5 秒。让 dldtool 提前占用串口等待后台重启触发的同步。

下载参数采用用户给出的成功组合：921600 初始速率、`-v --no-retry --reboot --sector-size-kib 64`、`programmer1700_dual.bin`、双芯片模式 1、只烧 AP、编程速率 2000000。`--reboot` 仍表示下载完成后重启；进入下载依靠板端后台命令。

本镜像未提供 NSH `sync` 命令，实际检查后记录不可用。此次正常退出 Agent 后仅创建 `/tmp` 临时脚本，没有修改持久配置或数据，随后复烧同一 AP 镜像。支持 `sync` 的固件应按流程执行。此差异未阻止本次成功。

## 结论修正与证据

之前先发送即时重启，再启动下载器，未复现用户强调的延迟交接流程；失败只能说明该试验没有完成握手，不能说明这块板子无法自动烧录。此轮成功重现完整流程，后续在 NSH 可用时优先采用免按键方式。

复现脚本：`work/test_delayed_bootloader_flash.py`；程序记录：`work/delayed-bootloader-flash-result.json:1`；烧录后状态：`work/delayed-bootloader-postflash-state.jsonl:2` 至 `:5`。下载器完整输出继承终端显示，没有另行伪造为原始日志文件；关键终端事件及退出码摘录在 `20261103_delayed_bootloader_flash_proof.json`。

仓库技能 `board-bringup` 和构建文档已更新。NSH 卡死、未开机或串口桥异常仍需要先恢复对应环节，本次通过不代表这些状态也能软件复位。

---

## 早期失败试验（未使用后台延迟交接）

2026-11-03，BES2800BP，COM20 / 921600。保留此前 AP 固件与 eMMC 数据。本轮未编译、未写入 Flash、未修改 eFuse。

### 早期事实清单

1. dldtool 3.20 的 `--reboot` 选项表示 `Reboot when finished`，作用于下载操作结束，不能据此断言它会复位正在运行的固件。证据：`work/dldtool-help.txt:48` 的 DESCRIPTION / `--reboot`。
2. 当前板端 NSH 支持 `reboot bootloader`：NSH 将 `bootloader` 解析为状态值 3。证据：`apps/nshlib/nsh_syscmds.c:115 / :449` 的 `g_resetflag` 和 `cmd_reboot`；当前 ELF 的 `board_reset` 分派到 `sys_bootloader_enter`。
3. 正常结束 Agent 后，发送普通 `reboot`，释放串口并启动 dldtool；下载工具未完成 SYNC，最终报告失败。证据：`work/auto-reset-readonly-download.log:128`。
4. 相同操作改为 `reboot bootloader`，仍未完成 SYNC。证据：`work/auto-reset-bootloader-readonly-download.log:128`。
5. 单独保持串口打开观察 `reboot bootloader`，日志记录 `reset mode:3`、`sw_bootmode:0x10200`，随后 `BOOT2A` 并进入正常 AP / Agent 启动。证据：`work/auto-reset-bootloader-boot.jsonl:2`。

只读下载测试参数为 `--efuse-read 0 --no-default-magic --reboot --retry 3`，使用厂商 `programmer1700_dual.bin`，未指定固件文件、擦除、写 eFuse、写内存或修改双芯片配置。握手未成功，因此尚未进入 eFuse 读取阶段。两次失败均不代表固件被写入或损坏。

### 当时结论及其适用范围

普通软件复位有效，但本次测试未验证通过自动进入下载模式。现有日志不足以判定是启动模式语义、下载窗口时序还是其他平台条件导致失败。不能承诺这套固件已能免按键烧录，也不能把 `CHIP=best1700` 当作成功握手证据。

当时暂用的烧录流程为：正常停止 Agent、释放串口、下载工具等待 SYNC、按 RESET。`--reboot` 可保留用于操作完成后重启。这一阶段尚未尝试后台延迟交接，因此结论仅适用于当时的测试时序，已被后续成功试验补充。

技能 `board-bringup` 已更正 `--reboot` 的含义，并将关闭进程限定为目标串口的占用者。当前核验不改变此前已通过的 5GHz 热点语音结果。

### 早期测试结束状态

串口状态确认自动语音已启用、运行并接受收音，处于等待唤醒词状态，人设回读正常。证据：`work/auto-reset-final-state.jsonl:2`、`:3`。串口已释放。
