# 朝夕集成补丁

当前无屏配置使用 0008、0009，分别修改 AI Agent 和 BES 启动入口。所有文件属于针对特定基线的源码补丁，不应把整个目录按数字顺序无条件应用。

## 当前补丁和固定基线

| 补丁 | 仓库与已检查基线 | 作用 |
| --- | --- | --- |
| 0008-packages_ai_agent-headless-entry.patch | open-vela/packages_ai_agent · e65550f18759f086d7f544edcf17d1e31223244f | --headless；关闭后台 heartbeat；只安装和发现核心 Skill；可查询的消息总线状态；关闭时停止接收请求并等待队列操作退出 |
| 0009-vendor_bes-headless-startup.patch | open-vela/vendor_bes · 36465377d944a575ce8a0c7ba45259f5c6c61ea8 | 配置启用 zhaoxi 时启动后台 Agent；其他配置保留原启动行为 |

上述两份补丁均在对应原始源码上执行 git apply --check。0009 是可选的自动启动方式；第一次部署资产时可先只使用 0008，从串口手动启动。

```bash
# 在已经同步的 openvela 工作区；先确认各仓库版本与本地改动。
cd packages/ai_agent
git apply --check ../../contest2026_416_dianzinongmingong/patches/0008-packages_ai_agent-headless-entry.patch
git apply ../../contest2026_416_dianzinongmingong/patches/0008-packages_ai_agent-headless-entry.patch

cd ../../vendor/bes
git apply --check ../../contest2026_416_dianzinongmingong/patches/0009-vendor_bes-headless-startup.patch
git apply ../../contest2026_416_dianzinongmingong/patches/0009-vendor_bes-headless-startup.patch
```

若本地 AI Agent 比固定提交早，先检查 voice 接口、message_bus 和 agent_main 的差异，再移植相同改动；不能靠强行忽略补丁冲突判断兼容。既有 0004 的 UI 启动改动需要先按实际差异撤掉，再集成 0009。

## 既有公共仓库修改

这些补丁保留供对应历史基线复现。当前 SDK 是否已包含修复，必须检查实际源码；未在本轮重新验证它们对新基线的全部应用组合。

| 编号 | 修改位置 | 内容 | 当前用途 |
| --- | --- | --- | --- |
| 0001 | packages/ai_agent | HTTP Date 时间同步及构建时间回退 | 按实际基线核对时间能力 |
| 0002 | packages/ai_agent | /data LittleFS 挂载与 tmpfs 回退 | 按实际基线核对存储 |
| 0003 | vendor/bes | LittleFS 与 DNS 配置 | 原配置参考；合并后以无屏 fragment 覆盖显示选项 |
| 0005 | packages/ai_agent | daily 固定时刻调度 | 可选；只有工具 schema 支持才开放对应提醒 |
| 0007 | packages/ai_agent | cron socket 与 MiMo thinking 修复 | 原运行修复，按实际源码保留 |

纯显示启动 0004 和写 NET_STATUS 给 UI 轮询的 0006 移至 legacy/display/patches。当前无屏版本不应用它们。

模型、ASR/TTS 的凭据继续存于设备配置，不进入补丁。完整固件仍需在真实 BSP、原音频配置和当前构建工具链中链接和验收。
