# 朝夕 · 主动式 AI 生活管家

## 一、作品简介

「朝夕」是一个运行在 BES2800BP 开发板上的主动式 AI 生活管家。它不等用户提问，而是通过 cron 定时引擎主动推送每日简报、健康提醒和待办提醒；用户也可以通过自然语言与它交互，让它记事、设提醒、查天气。核心亮点：

- **主动式服务**：基于 ai_agent 框架的 cron_service，每日定时推送天气+待办+新闻简报
- **4 个自定义设备端 Skill**：daily-briefing、note-taker、reminder、health-reminder，配合 SOUL.md 人格与 MEMORY.md 记忆系统
- **LVGL 桌面 UI**：zhaoxi_ui 应用独占 454x454 屏幕（RM69330，深色主题，实时钟表+状态栏+导航栏）
- **MiMo v2.5 Pro 后端**：通过小米 MiMo API 提供 LLM 推理能力
- **存储层**：实现 /data 的 LittleFS 挂载与 tmpfs 自动回退（板级分区表暂无 data 分区，当前以 tmpfs 运行；分区表启用后配置零改动生效）

## 二、选题方向

**AI 硬件产品创新**

基于 openvela（NuttX 内核）+ packages/ai_agent 框架 + 自定义 Skill 系统，在 BES2800BP_EVB 开发板上构建一个真正"主动干活"的 AI 生活管家，区别于被动问答式的传统智能助手。

## 三、目录结构

```
contest2026_416_dianzinongmingong/
├── app/
│   ├── hello_app/          # 组委会示例应用（保留）
│   └── zhaoxi_ui/          # 朝夕 LVGL 桌面 UI 应用
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── Make.defs
│       ├── Makefile
│       └── src/
│           └── zhaoxi_ui.c # UI 主程序（454x454 深色主题）
├── assets/
│   └── agent_deploy/       # 设备端 Agent 部署资产
│       ├── SOUL.md         # Agent 人格设定
│       ├── USER.md         # 用户信息模板
│       ├── MEMORY.md       # 长期记忆模板
│       ├── cron.json       # 定时任务配置
│       ├── daily-briefing.md   # 每日简报 Skill
│       ├── health-reminder.md  # 健康提醒 Skill
│       ├── note-taker.md       # 语音记事 Skill
│       └── reminder.md         # 定时提醒 Skill
├── board/
│   └── contest_board/      # 组委会示例板级适配（保留）
├── docs/
│   ├── development_plan.md         # 完整开发计划
│   └── BES2800BP_development_summary.md  # 硬件/构建/烧录经验总结
├── logs/                   # AI Coding 日志（由 contest-log-collector 自动归集）
├── patches/
│   ├── README.md           # 补丁说明与应用方法
│   ├── 0001-packages_ai_agent-fix-HTTP-Date-time-sync.patch
│   ├── 0002-packages_ai_agent-mount-littlefs-on-data.patch
│   ├── 0003-vendor_bes-enable-littlefs-driver-and-DNS.patch
│   └── 0004-vendor_bes-boot-into-zhaoxi_ui.patch
├── tools/
│   └── ntc_patch.py        # NTC 断言跳过补丁（符号级定位 + 0xb955 硬校验）
├── quickapp/
│   └── hello_quickapp/     # 组委会示例快应用（保留）
├── contest2026_416_dianzinongmingong.xml  # repo manifest
├── openvela.xml            # openvela 基础 manifest
├── .gitignore
└── README.md               # 本文件
```

## 四、运行方式

### 4.1 拉取工程

```bash
repo init -u https://github.com/yuk1-r/contest2026_416_dianzinongmingong \
  -b dev-ai-contest-2026 -m contest2026_416_dianzinongmingong.xml
repo sync -c -j8
```

### 4.2 应用公共仓补丁

本作品对 `packages/ai_agent` 和 `vendor/bes` 两个公共仓有改动，已生成 `patches/` 目录下的 4 个补丁。正式 PR 流程进行中；如需本地复现，可按顺序应用：

```bash
cd packages/ai_agent
git am ../../contest2026_416_dianzinongmingong/patches/0001-*.patch
git am ../../contest2026_416_dianzinongmingong/patches/0002-*.patch

cd ../vendor/bes
git am ../../contest2026_416_dianzinongmingong/patches/0003-*.patch
git am ../../contest2026_416_dianzinongmingong/patches/0004-*.patch
```

补丁作用：
- `0001` / `0002`：ai_agent 的 HTTP Date 对时 + /data LittleFS 挂载
- `0003`：defconfig 启用 LittleFS 驱动 + DNS 服务器 223.5.5.5
- `0004`：rcS.ap 启动 zhaoxi_ui 替代 lvgldemo

### 4.3 编译

环境要求：WSL Ubuntu-22.04 + ARM 工具链（openvela prebuilts 自带）

```bash
cd /path/to/openvela   # 工作区根目录（仓的上一级）
bash vendor/bes/readme/1700_ap.sh
```

编译产物：`cmake_out/aos_evb_ap/nuttx_ap.bin`

### 4.4 NTC 补丁

板子无 NTC 热敏电阻，需对固件做二进制补丁跳过断言：以符号级方式定位 `pmu_open+0x1a78` 的 `cbnz r5`（0xb955），改写为跳转指令（0xe011）。补丁前会硬校验原指令，布局漂移时拒绝执行。

```bash
python3 contest2026_416_dianzinongmingong/tools/ntc_patch.py
```

脚本读取 `cmake_out/best1700_ep/aos_evb/out/nuttx_ap.elf`（符号定位）、重新生成 `nuttx_ap.bin` 并同步到 `flash/vela_2800bp/vela_2800bp/nuttx_ap.bin`。

### 4.5 烧录

**红线：只许 AP-only 烧录，严禁 nuttx_ota.bin / nuttx_bl.bin**

```powershell
cd flash\vela_2800bp\vela_2800bp
.\dldtool.exe 20 --reboot .\programmer1700_dual.bin --set-dual-chip 1 -M .\nuttx_ap.bin --pgm-rate 2000000
```

`--reboot` 通过串口让板子自动复位进入下载模式；若板子已在崩溃循环/OTA 引导状态，可省略 `--reboot` 直接启动 dldtool 后按 RESET 触发 SYNC。烧录完成后拔插 USB 重启。

### 4.6 串口验证

```bash
# 串口 COM20, 波特率 921600
# 板子启动后应看到 zhaoxi_ui 界面（454x454 深色桌面）
# NSH 控制台可执行：
nsh> ifup wlan0
nsh> wapi psk wlan0 <密码> 3
nsh> wapi essid wlan0 <SSID> 1
nsh> renew wlan0
```

## 五、AI Coding 使用说明

### 开发工具

前一阶段开发使用 Codex CLI / Qoder 进行 AI 辅助编码，涵盖：
- **需求拆解**：将"主动式 AI 管家"拆解为 Skill 系统 + cron 引擎 + LVGL UI 三个模块
- **方案设计**：AI 协助设计 Skill 文件格式、cron.json 配置结构、SOUL.md 人格模板
- **编码实现**：zhaoxi_ui.c 的 LVGL 布局代码、Skill markdown 文件、defconfig 配置
- **调试排障**：NTC 断言崩溃的二进制补丁方案、网络对时问题的 HTTP Date 方案

### 日志状态

AI 对话日志正在整理中。比赛仓已安装官方 `contest-log-collector` 工具，后续开发会话将自动归集到 `logs/` 目录。

### 公共仓改动说明

以下改动在公共仓中，已附 patches/ 补丁，正式 PR 流程进行中：

| 仓库 | 补丁 | 改动内容 |
|------|------|----------|
| `packages/ai_agent` | 0001, 0002 | HTTP Date 对时 + LittleFS /data 挂载 |
| `vendor/bes` | 0003, 0004 | defconfig 启用 LittleFS/DNS + rcS.ap 启动 zhaoxi_ui |
