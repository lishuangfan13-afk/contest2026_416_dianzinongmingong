# Windows 供网与语音入口

电脑通过现有 USB2UART 数据线为朝夕提供网络。BES 自己完成语音识别、模型调用、文件操作、提醒调度和语音播放；Windows 与 WSL 负责转发网络和提供操作入口。

当前已配置设备开机先用个人训练的 MFCC/DTW 模板在本地选择“有线连接／无线连接”，不是通用 ASR；已有设备无需重录。询问、成功、失败使用三个本地 ZXP 提示音。无线联网、无 PC 时有线失败后改选无线成功及提示音流畅已确认；PPP console lease 与 SNTP 阻塞修复后，真实 PC 有线已通过 PPP、授时、成功播报、等待唤醒及正常停止全链路，板端／串口／网关清理均确认；Windows 登录任务真实启动至待命及正常停止清理已通过，最新 SNTP 固件无线开机复测也已通过，重新登录触发仍待观察。完整边界见 [开机语音选网](../../docs/开机语音选网.md)。

## 准备环境

- Windows 能识别 FTDI USB 串口。
- Python 3 和 pyserial 3.5。
- Ubuntu-22.04 WSL root 环境，准备 Python 3、pppd、ip、iptables、PPP/TUN 设备。
- 电脑已经能够正常上网，板子沿用已配置的 MiMo 服务凭据。

```powershell
python -m pip install pyserial==3.5
python tools/pc_link/launch.py --check-only
```

预检枚举设备并检查 WSL 依赖。只有一个匹配的 FTDI 设备时，正式入口会自动选择它；多设备环境用 `--port` 指定。

## 开机有线待命与 Windows 登录工具

开机选网需要 `boot_watch.py` 先在电脑待命：收到完整有线选择标记或 CRC 正确的 PPP LCP 请求才接管供网，无线选择不会启动 PPP。它释放串口后交给 boot attach，不发送 NSH 启动命令、不重启 Agent，自动语音由板端管理。

首次或换电脑时，在仓库根目录运行（`COM3` 替换为实际端口）：

```powershell
python tools/pc_link/boot_startup.py --install --port COM3 --state-dir out/pc_link_boot --start
python tools/pc_link/boot_startup.py --status
```

工具只注册当前用户登录触发、最低权限的计划任务，不保存密码、不要求管理员。`--start` 请求运行同一任务，并在 20 秒内确认真实运行心跳与串口／子进程状态，不能只靠调度成功就声称就绪。`--status` 分别显示任务归属与实际待命、运行、失败或停止状态；待命不代表板端已经联网。当前任务真实启动及正常停止已通过，重新登录触发仍待观察；直接 boot watcher 的有线全链路已通过。

日常维护与取消自动待命：

```powershell
python tools/pc_link/boot_startup.py --stop
python tools/pc_link/boot_startup.py --uninstall
```

`--stop` 正常停止本次 watcher／子供网会话，保留登录任务；`--uninstall` 先确认清理再删任务，清理未确认时保留并报错。烧录、串口调试、模板录入或切换旧手动入口前先停止；移动仓库或更换 Python 前先在原路径卸载，再重新安装。

state 默认是仓库下 `out/pc_link_boot`；安装时自定义相对路径按安装命令的当前目录解析并保存绝对路径。已安装时 `--stop` 自动读取任务记录的路径，显式传入不同路径会拒绝；`--status`、`--uninstall` 不接受 `--state-dir`。直接运行 watcher 则需使用同一 state 路径停止：

```powershell
python tools/pc_link/boot_watch.py --port COM3 --state-dir out/pc_link_boot
python tools/pc_link/boot_watch.py --state-dir out/pc_link_boot --stop
```

第一条在前台持续待命，第二条在另一窗口执行；不要与登录任务或其他串口程序并行运行。boot attach 默认持续到正常停止。选无线后 watcher 改为周期性短窗口只读监听，调试前仍应先停止。

## 原手动启动与结束

以下入口主动编排供网，保留作维护使用，不经过开机本地选网；先停止 boot watcher。

1. 接好供电和 USB2UART 数据线，关闭其他串口终端。完整断电后，如板子尚未启动，按 POWER 约两秒。
2. 双击 [开始自动语音.cmd](开始自动语音.cmd)，或在仓库根目录执行：

   ```powershell
   python tools/pc_link/launch.py --port COM3
   ```

3. 程序先结束旧助手，使用电脑 UTC 时间校准板子，再建立 PPP。看到 `PPP_UPLINK_READY` 后，在第二个 PowerShell 窗口操作业务。
4. 保留启动窗口。结束时双击 [停止朝夕语音.cmd](停止朝夕语音.cmd)，或执行 `python tools/pc_link/launch.py --stop`，等会话退出并清理转发规则。

电脑日期应准确；每次正式启动都会重新授时，给提醒恢复正确日期。默认会话最长一小时，也可用 `--duration 1200` 设置二十分钟。

## 文字操作与语音播报

```powershell
python tools/pc_link/control.py --cmd ask 用一句话解释DMA
python tools/pc_link/control.py --cmd note 明天带学生证
python tools/pc_link/control.py --notes
python tools/pc_link/control.py --cmd --text todo add 交作业
python tools/pc_link/control.py --tasks
python tools/pc_link/control.py --cmd remind in 30 喝水
python tools/pc_link/control.py --reminders
python tools/pc_link/control.py --time
```

普通业务命令使用语音回复；加 `--text` 可选择文字。待办完成与提醒取消使用返回的真实编号，完整步骤见 [核心功能使用说明](../../docs/无屏核心功能.md)。供网期间由启动程序管理串口，业务统一通过 `control.py` 发送。

## 自动语音体验

默认入口配合板端开机唤醒，也可明确使用：

```powershell
python tools/pc_link/launch.py --auto-voice
```

窗口确认可以说话后，先说“你好，夕夕”或“Hello，xixi”，也可以紧接着说出需求。识别到唤醒词后先播报“你好”，播完再提问。唤醒后可继续提问，空闲 60 秒会回到等待唤醒。板端采用声音检测和约 1.2 秒静音封句，识别、处理和播放期间暂停收音；等喇叭回答结束，再说下一句。

源码补丁 0065 在 Agent 初始化后自动进入等待唤醒；2026-11-02 发布的 AP 镜像已包含本功能，烧录后生效。需要供网同时停止自动监听时，使用 `launch.py --manual-voice` 或 [仅共享网络.cmd](仅共享网络.cmd)，控制通道就绪后会实际发送 `auto stop`。自动监听开启后，检测到的语句会发送给云端 ASR，再由板端检查唤醒前缀。`python tools/pc_link/control.py --cmd auto wake on` 可启用前缀检查，`--cmd auto wake off` 用于现场调试的自由对话，`python tools/pc_link/control.py --auto-status` 查询当前开关与完整收音状态；这些命令本身不会启动录音。完整使用规则见 [ASR 唤醒短语](../../docs/ASR唤醒短语.md)。

窗口会显示环境校准、等待说话、收音、识别、等待回答、合成与播放阶段。结束使用同一个停止入口。两种启动模式各管理一个会话，切换前先正常结束当前会话。

## 故障恢复

晨间资讯的采集器随供网会话启动和关闭。首次设置人设与接口后，板端负责每日调度、缓存和播放许可，详见 [长期人设与晨间资讯](../../docs/长期人设与晨间资讯.md)。旧手动入口的会话上限仍为一小时；boot attach 默认持续至正常停止，长时间无人值守运行仍待观察。

- 串口占用：关闭其他终端，确认使用 USB2UART 数据口及正确 COM 编号。
- COM 已识别而板端静默：检查供电，按 POWER 启动板子；RESET 用于运行中的复位。
- 旧助手退出超时：保留已保存数据，复位板子后重新启动供网。
- Windows 串口写超时：退出当前供网程序，重新连接 USB2UART 数据线后再启动。

## 运行记录与配置

旧手动入口状态默认在本机 `out/pc_link`；boot watcher 默认在 `out/pc_link_boot`，包含 `watch.json`、`attach.log` 与 `sessions/<会话编号>`。`control.py` 经固定 PPP 控制地址访问板端，不接受 `--state-dir`；停止和清理应使用上述 watcher／登录任务入口，不能拿旧手动会话状态代替。状态文件记录授时、控制连接、自动语音阶段和退出结果。临时通道令牌在退出时清理，API 凭据继续保存在板上配置中；运行目录、private.json 和未经筛选的串口日志不得打包公开。

原始串口记录由操作者本机保管。作品资料使用整理后的版本、功能和测试结果。网络结构见 [电脑网络共享](../../docs/电脑网络共享.md)，语音传输优化见 [语音响应优化](../../docs/语音响应优化.md)。
