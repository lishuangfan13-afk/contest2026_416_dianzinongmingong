# zhaoxi 无屏入口

该应用将问答、备忘、待办和提醒请求交给原 AI Agent。默认回复通道为 voice，--text 改用 cli。文件与调度复用 SDK；BES 直连 PCM 和 MiMo 语音后端分别由 bes_audio.c、mimo_voice.c 实现，bes_clock.c 绑定实际 RTC。

## 接入要求

- flat 构建，与 packages/ai_agent 同一地址空间。
- AI Agent 应用当前完整 SDK 补丁序列，头文件来自 src 和 include，顺序见 patches/README.md。
- manifest 把 app/zhaoxi 链接到 packages/zhaoxi。
- 合并 configs/bes_headless.fragment，保持原音频服务配置。

Agent 以 ai_agent --headless 后台运行时不读取 NSH 的 stdin。zhaoxi stop 仅请求原优雅停止流程；看到 Shutdown complete 后才能替换资产或重新启动。status 只报告消息总线能否接收请求，不代表网络、模型或语音服务健康。

输入最多 511 个 UTF-8 字节，包含内部意图前缀；过长、缺参、控制字符和无效编号明确拒绝。队列提交失败时调用者释放消息。入队成功只报告请求已提交，业务成功由原工具结果确认。

## 主机检查

在 Linux/WSL 中，准备已经应用当前完整 SDK 补丁序列的 AI Agent 源码：

```bash
python3 tools/check_headless.py --agent-root /path/to/packages/ai_agent   --arm-prefix /path/to/arm-none-eabi-
```

arm-prefix 是工具路径加前缀，如 /opt/toolchain/bin/arm-none-eabi-。主机用 ASan/UBSan 编译真实 message_bus.c，测试命令路由、拒绝路径、队列满，以及生产者/消费者与停止流程并发。原生 Skill 加载器另外验证核心配置筛选、旧扩展文件排除、已有部署文件保留、普通配置和摘要边界。语音和 shutdown 调用有明确的测试替身，只验证 API 错误传播与调用关系。

ARM 步骤生成 Cortex-M55 对象，不能作为可烧录固件。只运行配置和资产静态检查可以省略 --agent-root。

完整实烧镜像、构建脚本、固定 BSP 库校验值，以及工具层与联网业务验收的区别，见 [构建与验收](../../docs/BES固件构建与验收.md)。当前产品只允许 --headless 模式，禁止重复 Agent；退出时等待真实后台任务结束再释放共享状态。

Windows 资产渲染与脚本语法检查：

```powershell
powershell -File tests/headless/test_assets.ps1
```

[功能与板上验收](../../docs/无屏核心功能.md) · [检查记录](../../docs/bringup_logs/20261030_headless_checks.json)

## 不依赖模型的本机提醒

R10 的 BES 配置可以从串口直接创建、查询和取消提醒，不调用 ASR 或模型：

```text
zhaoxi remind in 60 喝水
zhaoxi remind list
zhaoxi remind cancel <返回的八位编号>
```

秒数为 1 至 31536000 的整数。时钟未校准、保存失败或容量已满时不确认创建。`--text remind in ...` 到期输出串口文字；默认提醒在网络与语音可用时播报，否则尝试两秒本机提示音，同时输出提醒文字。提示音不代表已播报事项内容，播放器错误也会报告。调度器按最近的到期时间唤醒；队列拥堵时尚未投递的一次性任务保留重试。

实板已验证创建、查询、单项取消、三秒提醒在五秒观察窗口内投递并移除。音频后端此前已有用户听感确认；新到期提示音路径的主观听音以单独记录为准。

R10还实测了断网提醒到期后的真实DMA/DAC输出：两秒提示音写入全部完成，一次性任务移除。该次用户主观听音尚未确认，不将其等同于云端播报事项内容。

## 流式语音与连续播放

MiMo 产品播放入口增量接收 SSE，`mimo_sse.c` 负责有界解析和 PCM 样本对齐，固定队列分离网络接收与 DMA 播放。BES 后端使用完成中断唤醒等待线程。原批量合成接口继续保留；ASR 仍在本机封句后上传。实现、实板计时口径和当前听感边界见 [语音响应优化](../../docs/语音响应优化.md)。

以下检查使用真实源码，网络、时钟和 DMA 由明确替身控制，不打开麦克风或串口：

```bash
python3 tests/headless/pc_link/check_tls_stream.py --sdk /path/to/packages/ai_agent
python3 tests/headless/pc_link/check_mimo_fast_response.py --sdk /path/to/packages/ai_agent
python3 tests/headless/voice/check_mimo_sse.py --workspace /path/to/openvela
python3 tests/headless/voice/check_tts_queue.py --sdk /path/to/packages/ai_agent
python3 tests/headless/voice/check_bes_dma_irq.py --sdk /path/to/packages/ai_agent
```

TLS 检查覆盖分片、限长、错误、读取时间片恢复及总截止；SSE 检查覆盖事件拆分、base64、半个样本跨事件、结束验证和取消；播放检查覆盖有界背压、错误清理、初始缓冲和 DMA 完成唤醒。它们与完整 AP 编译、实板测量及真人试听分别记录。
