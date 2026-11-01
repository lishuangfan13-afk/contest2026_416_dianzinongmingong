# EasyMatrix 朝夕表情扩展

这份程序基于原 EasyMatrix 像素钟增加朝夕表情层，通过电脑 USB 串口显示助手状态，保留原时钟、动画和按键操作。BES 与像素钟各自连接电脑，由电脑将助手状态同步到屏上。

实物为 ESP32-C3、4MB Flash、32×8 WS2812 像素钟，GPIO6 驱动，通过 CH343 使用 115200 串口。源码来自用户提供的 EasyMatrix 工程，原始来源及逐文件哈希在 `SOURCE_PROVENANCE.json`；固定构建配置使用 AirM2M CORE ESP32C3。

## 交互

串口使用原来的 UART0 / USB-UART 路径，115200、8N1、无流控。每条请求是以 LF 结束的一行 ASCII JSON，最长192字节（不含LF，CRLF也可）。`id` 为必填的0～4294967295整数，客户端用它匹配回复。只支持这里列出的平面字段；未知字段、重复字段、嵌套值、字符串转义、负数、小数和过长帧均拒绝，不执行显示操作。

```json
{"id":1,"cmd":"ping"}
{"id":2,"cmd":"status"}
{"id":3,"cmd":"emotion","state":"listening","ttl_ms":30000}
{"id":4,"cmd":"emotion","state":"clock"}
```

| state | 颜文字与表现 |
|---|---|
| idle | `(^_^)`，偶尔眨眼，紫色 |
| listening | `(o_o)`，下方节拍点，青色 |
| thinking | `(-_-)`，下方轮转省略点，橙色 |
| speaking | `(^o^)` / `(^-^)` 开合嘴，浅蓝色 |
| success | `(^v^)`，绿色 |
| error | `(x_x)`，红色 |
| sleep | `(z_z)`，低亮蓝紫色 |
| clock | 撤销表情覆盖，恢复原像素钟当前页面 |

`ttl_ms` 可省略，默认30000毫秒；1～300000毫秒到期后恢复原页面；0表示保持到下一条表情命令、按键操作或重启。覆盖状态只存在RAM，不写NVS。长时间由电脑维持的状态应定期续租，断开电脑后默认30秒回到时钟。

成功回复示例：

```json
{"type":"zhaoxi.display","v":1,"id":3,"ok":true,"cmd":"emotion","device":"EasyMatrix","fw":"zhaoxi-display-1.0.0","width":32,"height":8,"state":"listening","active":true,"frame":1,"uptime_ms":70000,"base_page":1,"ttl_ms":30000}
```

`frame` 是表情层（含释放覆盖）完成 `matrix.show()` 的帧计数；表情 ACK 在刷新调用返回后发出。`base_page` 是原程序页面枚举；`ttl_ms` 是配置的租期。`ping/status` 返回状态，错误回复为同一 `type/v`、`id:null`、`ok:false` 和固定 `error` 代码。

串口也会出现原程序启动、配网、天气和旧菜单等普通日志，主机只把 `type=zhaoxi.display` 的JSON行视为协议回复。原 `config`（写入配网标记并重启）与 `icons`（图案演示）保留；这两条以及实体按键操作会撤销表情覆盖。

## 原功能与配置

表情层使用同一32×8矩阵，在覆盖期间暂停旧页面绘制；后台启动文字、配网IP轮播通过同一互斥锁让出矩阵，避免盖掉表情。停止这两类后台任务时同样先取得锁，避免删除持锁任务造成永久等待。释放覆盖后回到原页面。原WiFi、颜色、亮度、闹钟、时钟子页和自定义图案的NVS读取/写入代码保留。

表情使用固定5×7位图，无中文字体依赖；临时显示亮度约束在12～32，原来的亮度设置不被保存修改。`emotion_face.cpp` 是实际目标栅格源，可用宿主测试导出相同像素预览。

原开机流程仍会联网对时，最坏可能在进入主循环前等待约60秒，期间尚不处理串口JSON。电脑应等到一次 `ping` 成功后才认为表情服务可用，不能只凭USB端口出现判断完成。

## 构建

本次已在Windows实际安装并使用 Arduino CLI 1.5.1、官方 ESP32 Arduino core 3.3.12 完成完整目标编译。依赖固定为 NeoMatrix 1.3.3、NeoPixel 1.15.5、GFX 1.13.0、BusIO 1.17.4、arduinoFFT 2.0.4、OneButton 2.6.2；官方包来源、校验值、实际FQBN与源码哈希在 `BUILD_LOCK.json`。原归档3.3.10-cn的构建记录只用于来源追溯。

```powershell
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "Adafruit NeoMatrix@1.3.3" "Adafruit NeoPixel@1.15.5" "Adafruit GFX Library@1.13.0" "Adafruit BusIO@1.17.4" "arduinoFFT@2.0.4" "OneButton@2.6.2"
arduino-cli compile --fqbn esp32:esp32:AirM2M_CORE_ESP32C3:CDCOnBoot=default,PartitionScheme=huge_app,EraseFlash=none --jobs 2 --build-path <私密构建目录> --output-dir <私密产物目录> <EasyMatrix源码目录>
```

原物理设备使用CH343，保持UART0的默认构建选项，不要启用USB CDC来替换 `Serial0`。构建完成后保存core/library实际版本、完整编译日志、应用大小和SHA256。

本次应用大小 1,194,544 字节，SHA256 为 `6f8fd7b2335664e55538f56e0a1e32bfcde8f574922259607ec628732d8da5ba`。已核对 ELF 为 32 位 RISC-V、UART0 与绘制任务管理函数实际链接、生成分区表与实板一致、发布源码与构建源码一致。应用已写入像素钟，七种表情均收到实板确认并推进帧计数，十秒租期后的恢复时钟检查通过。

Windows下工具安装路径过长可能导致GCC找不到多级C++标准库头文件。本次保持包管理目录原位置，只在本机编译属性中为GCC/SDK使用已验证的短路径别名；没有修改系统代理或SDK源码。其他电脑使用较短的Arduino数据目录也可避免这一问题。

本目录的 `partitions.csv` 已以2026-11-02实板完整Flash备份解析结果更新：NVS为0x9000/0x5000，otadata为0xe000/0x2000，唯一app0为0x10000/0x300000，spiffs为0x310000/0xe0000，coredump为0x3f0000/0x10000。它与旧归档的双应用1.3MB布局不同，不能混用。根代理保留原始4MB私密备份，本轮只写应用镜像到0x10000，不写bootloader、分区表、NVS或文件系统，不执行全擦，不烧合并镜像。

## 离线验证

纯协议和像素栅格不依赖Arduino，可用独立的 `tests/test_emotion.cpp` 与 g++ 编译验证。`tests/test_display.cpp` 另外用替身矩阵、串口和时钟检查 `matrix.show()` 先于ACK、只读查询、错误不改显示、租期和计时回绕、原页面/亮度恢复、矩阵互斥锁。测试替身不读取或驱动真实串口。仓库目录下可运行：

```powershell
g++ -std=c++11 -Wall -Wextra -Werror -I . tests/test_emotion.cpp emotion_protocol.cpp emotion_face.cpp -o <私密目录>/test_emotion.exe
g++ -std=c++11 -Wall -Wextra -Werror -I tests/stubs -I . tests/test_display.cpp emotion_protocol.cpp emotion_face.cpp emotion_display.cpp -o <私密目录>/test_display.exe
```

当前已通过合法与错误字段、整数／TTL 边界、七个 32×8 像素帧、缓冲区边界、6000 条畸形输入和显示适配检查。`docs/emotion_preview.png` 展示同一 `emotionRaster` 输出的像素预览。

本扩展没有把私人WiFi配置、Flash备份或设备日志放入源码。完整Flash备份包含设备设置，应留在私密工作目录，不提交仓库。
