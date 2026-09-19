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

### 4.6 资产部署

`assets/agent_deploy/` 下的 8 个资产需推送到开发板才能生效。路径映射（`<BASE>` 为 ai_agent 数据根目录）：

| 本地文件 | 设备端路径 | 说明 |
|----------|-----------|------|
| `SOUL.md` | `<BASE>/config/SOUL.md` | Agent 人格 |
| `USER.md` | `<BASE>/config/USER.md` | 用户信息 |
| `MEMORY.md` | `<BASE>/memory/MEMORY.md` | 长期记忆 |
| `cron.json` | `<BASE>/cron.json` | 定时任务（cron_service 读这里，不是 `config/` 下） |
| 4 个 skill `.md` | `<BASE>/skills/` | 自定义 Skill |

> **注意**：`<BASE>` 在旧固件（如 9 月 6 日构建）为 `/data/ai_agent`，工作区最新源码已改为 `/data/agent`（`agent_config.h` 的 `CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR`）。部署脚本会自动探测，也可用 `-BaseDir` 显式指定。

使用串口部署脚本（Windows PowerShell 5.1+，板子处于 NSH 控制台或被 ai_agent 占用均可，脚本会自动 `quit` 退回 NSH）：

```powershell
powershell -File contest2026_416_dianzinongmingong\tools\deploy_assets.ps1 -Port COM3 -Baud 921600
```

脚本通过 NSH `echo` 逐行写入并 `cat` 回读逐字节校验，校验失败自动重试（最多 3 轮，应对 921600 高波特率下偶发的单比特传输错误）。实现上绕开了两个坑：本固件 NSH readline 行缓冲仅 80 字节（超限截断执行），且双引号字符串内不支持 `\"` 转义（含引号的行自动改用单引号包裹）。未采用 `install_skill`（源码仅支持 https URL，stdin 模式未实现）与 `memory_write`（按空白分词只取首个参数，写不了多行中文），详见脚本头部注释。

注意事项：
- **/data 为 tmpfs**：LittleFS 分区启用前，每次重启/烧录后资产全部丢失，需重新运行本脚本
- **覆盖语义**：同名 skill 部署会直接覆盖设备端文件内容（`>` 重定向），无需关心内置 skill 是否已写入

### 4.7 串口验证

```bash
# 串口 COM3, 波特率 921600
# 板子启动后应看到 zhaoxi_ui 界面（454x454 深色桌面）
# NSH 控制台可执行（注意 mode 2 不可少，否则 set_ssid 报 -22）：
nsh> ifup wlan0
nsh> wapi mode wlan0 2
nsh> wapi psk wlan0 <密码> 3
nsh> wapi essid wlan0 <SSID> 1
nsh> renew wlan0
```

### 4.8 天气/新闻工具所需的 Tavily API Key

设备端的 `get_weather` 与 `news_search` 工具默认以 **Tavily** 为主后端（工具注册见 `src/tools/tool_registry.c:191,201`），需先配置 Tavily API Key，否则天气/新闻会失败（`tool_web_search.c` 中天气链路为 Tavily → SerpAPI，新闻链路为 Tavily → NewsAPI）。

配置方式（二选一，**运行期配置为推荐做法**）：

- **NSH 命令行**（写入设备端 config store，重启后仍生效）：

  ```text
  nsh> set_tavily_key <YOUR_TAVILY_KEY>
  nsh> config_show       # 可回读确认（Tavily Key 已脱敏显示）
  ```

- **设备端配置文件**：`<BASE>/config/config.json` 中的 `tavily_key` 字段（`<BASE>` 默认为 `/data/agent`，旧固件为 `/data/ai_agent`，见 4.6 节）。该文件由框架以 `0600` 权限创建。

机制来源（源码实证）：

- 键名宏 `AGENT_CFG_KEY_TAVILY_KEY` = `"tavily_key"`，见 `apps/packages/ai_agent/include/agent_config.h:243`。
- 运行期从 config store 读取：`tool_web_search.c:106` 调用 `claw_config_get(AGENT_CFG_KEY_TAVILY_KEY, ...)`；配置持久化到 `AGENT_CONFIG_FILE`（`agent_config.h:96`，即 `<AGENT_DATA_DIR>/config/config.json`，`AGENT_DATA_DIR` 默认 `/data/agent`，`agent_config.h:85-89`）。
- NSH 命令 `set_tavily_key <key>` → `cmd_set_tavily_key()`（`src/channels/nsh_commands.c:423`）→ `tool_web_search_set_tavily_key()` → `claw_config_set()`（`src/tools/tool_web_search.c:529`）。
- 另有编译期兜底宏 `AGENT_SECRET_TAVILY_KEY`（`agent_config.h:71`），默认空字符串。

> **安全红线：不要把 Tavily Key（或任何 API Key）写进源码、资产或提交进仓库。** 上述编译期宏保持默认空值，密钥只通过 NSH 命令或设备端 `config.json` 在运行时配置；`config.json` 位于设备 `/data` 下，不属于本仓库。

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
