# ASR 唤醒短语与连续对话

自动语音开启后，默认等待“你好，openvela”或“Hello，openvela”。可以直接说“你好，openvela，帮我记一下明天带书”，也可以只说唤醒短语，听到本机短提示音后再说需求。产品名称仍为“朝夕”，唤醒短语按 [官方比赛规则](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/contest_overview.md#参赛须知与规则) 统一设置。

这项功能采用 **VAD 检测声音、云端 ASR 识别、板端匹配唤醒短语**的流程。开启自动语音后，检测到的语句会发送给语音识别服务；板端从识别结果中确认唤醒短语，再把需求交给问答助手。

## 使用规则

| 说法或状态 | 行为 |
| --- | --- |
| 等待唤醒时说“今天有多少任务” | 保持等待唤醒 |
| “你好，openvela” | 开启会话，只播放本机短提示音，不调用 TTS |
| “Hello，openvela，添加待办买牛奶” | 剥去唤醒前缀，把“添加待办买牛奶”交给原助手 |
| “我刚才说了你好openvela” | 唤醒词不在前缀位置，不开启会话 |
| 会话内继续提问 | 不必每句重复唤醒短语 |
| 空闲达到 60 秒 | 回到等待唤醒；仍继续 VAD 和 ASR |
| 收句、ASR、等待回答、合成及播放期间 | 不消耗会话空闲窗口；回答完成后重新给出 60 秒 |
| 会话内说“再见”“休息吧”“结束对话” | 本机结束会话，回到等待唤醒，麦克风继续自动模式 |
| 说“停止监听” | 实际请求停止自动监听；需要重新启动自动语音才能继续 |

前缀允许常见空白和标点，英文名称匹配不区分大小写，例如“你好，OpenVela”和“Hello, openvela”。匹配只发生在句首，不进行全文关键词搜索。对话仍为半双工：回答处理和播放期间不收下一条命令，不支持用语音打断播放。

麦克风仍默认关闭，只有原来的自动语音入口或 `voice_channel_auto_start()` 才会开启。设置唤醒功能本身不会启动采集。

## 查看当前状态

```powershell
python tools/pc_link/control.py --auto-status
```

这个只读入口返回 `enabled`、`running`、`accepting_speech`、`wake_enabled`、`awake` 和 `wake_phrase` 等字段，本身不会开启麦克风。自动语音入口的窗口也会显示当前处理阶段。

## 供产品入口调用的接口

```c
int voice_channel_auto_wake_set_enabled(bool enabled);
int voice_channel_auto_wake_get_state(bool *enabled, bool *awake);
```

开关仅保存在 RAM。启用时清除旧会话，回到等待唤醒；关闭后恢复调试用的自由对话，不再应用唤醒前缀、空闲超时和本机会话休眠规则。“停止监听”仍会实际停止自动模式。停止再启动自动语音会清除会话，但保留本次 RAM 开关；整板重新启动后默认启用唤醒 gate。

原 `voice_channel_auto_start()`、`voice_channel_auto_stop()` 接口不变。不支持自动模式的后端返回 `-ENOSYS`。

## 状态字段

原自动语音状态增加下列字段：

| 字段 | 含义 |
| --- | --- |
| `wake_enabled` | RAM 中的唤醒 gate 开关；不表示麦克风已开启 |
| `awake` | 当前自动模式中的会话是否可接收命令 |
| `wake_phrase` | 支持的提示短语“你好，openvela / Hello，openvela” |
| `accepting_commands` | 当前正在接收语音，且已唤醒或 gate 已关闭 |
| `output_active` | 正在合成或输出语音，独立于自动监听是否开启 |
| `output_playing` | PCM 消费者已经进入实际播放阶段，独立于自动监听是否开启 |

`accepting_speech=true` 只表示 VAD 采集入口可接收语音。在等待唤醒时它可以为 true，而 `accepting_commands` 为 false。上层应结合这两个字段提示用户先说唤醒短语。

## 检查

```bash
python3 tests/headless/voice/check_voice_wake.py --sdk /path/to/openvela/packages/ai_agent
python3 tests/headless/voice/check_auto_voice.py --sdk /path/to/openvela/packages/ai_agent
```

第一项编译实际的短语匹配模块，覆盖前缀、中文标点与空白、同句命令、空闲到期、忙时续期、休眠、停止及调试开关。第二项运行实际自动语音线程，用合成 PCM 和 ASR 测试接口检查唤醒与会话处理、回答完成续期，以及停止后的资源回收。
