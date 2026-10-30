# 朝夕集成补丁

当前完整无屏固件包含 SDK 与 BES 启动/挂载两组补丁。它们针对不同仓库，不能把整个目录无条件按数字顺序应用。

## SDK 固定基线与顺序

基线：`open-vela/packages_ai_agent`，`e65550f18759f086d7f544edcf17d1e31223244f`。以下序列已逐个应用到原始源码副本，并与实际编译源码逐文件比较。0008 的结尾已按真实归档修正。

| 顺序 | 作用 |
| --- | --- |
| 0008 | 无屏入口、核心 Skill 与消息总线 |
| 0010 | BES 真实 PCM 后端与构建适配 |
| 0011 | MiMo ASR/TTS 后端和失败传播 |
| 0012 | 检查真实 WAPI 命令返回值，先启动核心业务循环 |
| 0013 | 真实 exFAT 持久挂载和 /emmc/zhaoxi |
| 0014 | checked 写入、fsync、cron 回滚与恢复副本 |
| 0015 | 不依赖 rand 的唯一提醒编号 |
| 0017 | 核心八项工具、RTC 和未授时保护 |
| 0018 | 保留已连接网络，不重复 DHCP |
| 0019 | 无线就绪等待与自动重连 |
| 0020 | station/carrier 检查、IPv4 DNS 与 TCP 截止时间 |
| 0021 | 单实例、worker 退出屏障、cron 停止唤醒，产品不启动 WS/阻塞 CLI |

后续补丁依赖前面的源码状态，逐个 `git apply --check /path/to/patch` 后再 `git apply /path/to/patch`。不要把历史 0001～0007 无条件叠加到此基线。

```bash
python3 tools/check_agent_patches.py \
  --pristine-agent /path/to/pristine-e65550f \
  --patched-agent /path/to/openvela/packages/ai_agent
```

此检查使用临时副本，不修改原始基线。完整构建与固件封装见 [构建脚本](../tools/build_bes_headless.py)。

## BES 启动与挂载

公共源码基线：`open-vela/vendor_bes`，`36465377d944a575ce8a0c7ba45259f5c6c61ea8`。0009 负责产品后台启动；0016 负责产品不自动格式化持久存储。已有显示启动修改应先按实际差异撤下。

实际 BSP 库来自本地已匹配配置，SHA256 和本地非公开提交边界见 [来源清单](../configs/bes_headless.sources.json)。不能仅凭公共 manifest 宣称得到相同完整工作区。

修改嵌套 `.ap` 启动文件后须触碰顶层 `rcS`/`rc.sysinit` 并检查封装 ROMFS；新构建脚本包含此操作。更改已有构建的 defconfig 后须 resetconfig。

## 历史与验收

0001～0003、0005、0007 保留历史时间、存储、DNS、daily 调度和模型适配的追溯用途，按各自基线核对。纯显示的 0004/0006 已转入 legacy，不属于当前序列。

API key、Wi-Fi 凭据及录音不进入补丁。当前已生成并烧录完整镜像，但板端云问答和完整语音业务尚未通过；见 [构建与验收](../docs/BES固件构建与验收.md) 和 [Wi-Fi 审计](../docs/WiFi固件审计.md)。
