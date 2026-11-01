# EasyMatrix 颜文字屏固件

这份应用用于现有 ESP32-C3、4MB Flash、32×8 WS2812 像素钟，经 CH343 USB 串口接收朝夕的表情状态。源码和固定依赖见 [EasyMatrix 工程](../../extension/EasyMatrix/README_zh-cn.md)，日常使用见 [颜文字屏说明](../../docs/颜文字屏使用.md)。

| 文件 | 用途 |
| --- | --- |
| `EasyMatrix.ino.bin` | 已编译的 ESP32-C3 应用 |
| `EasyMatrix.ino.manifest.json` | 应用大小、SHA256、源码哈希、构建配置和实板检查记录 |

当前实物使用 4MB Flash，唯一 app0 位于 `0x10000`，容量 3MB。更新时先备份原设备 Flash、核对分区，再只写这份应用；保留原 bootloader、分区表、NVS 和文件系统。

在仓库根目录，关闭占用像素钟串口的程序后，可使用已安装的 esptool 执行：

```powershell
python -m esptool --port COM4 --baud 460800 --after hard-reset write-flash 0x10000 firmware/easymatrix/EasyMatrix.ino.bin
```

将 `COM4` 换成实际 CH343 端口。BES 使用另一条 FTDI 串口；两个设备的固件和烧录工具各自独立。

完成后等待像素钟开机流程结束，再运行：

```powershell
python tools/display_link.py --port COM4 --show thinking
```

该入口显示五秒后恢复时钟。正式运行时先启动电脑供网，再打开 `tools/pc_link/启动颜文字屏.cmd`，让屏幕跟随朝夕状态。

2026-11-02 已完成应用写入与完整读回校验，七种表情均收到实板确认、帧计数递增，租期结束恢复时钟。原有 23 项像素钟配置保持一致。
