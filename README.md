# 「朝夕」—— 主动式 AI 生活管家

## 一、作品简介

面向工作繁忙的都市白领与家庭桌面场景，解决传统智能音箱「被动问答、不问就静默」的痛点：基于时间、事件与上下文主动推送提醒、播报信息、执行任务，把 AI 从「问答工具」变成「主动干活的生活伙伴」。

**亮点**：
- 主动式 AI：定时/事件/上下文触发，不等用户提问
- LVGL 图形界面：深色主题，时钟/天气/待办/对话四屏
- 云端 LLM 对话：集成 Xiaomi MiMo 大模型
- WiFi 联网：实时获取天气、新闻等信息

## 二、选题方向

**AI 硬件产品创新** — 基于 BES2800BP 开发板，利用其 WiFi 6 + BT 6.0 + Audio CODEC + Cortex-M55 的硬件能力，打造端侧 AI 生活管家。

## 三、目录结构

```
app/
├── zhaoxi_ui/          — 主应用：LVGL 图形界面（时钟/天气/待办/对话）
│   ├── src/zhaoxi_ui.c — UI 主程序
│   ├── Kconfig          — 编译配置
│   ├── CMakeLists.txt   — CMake 构建
│   └── Makefile         — Make 构建
├── hello_app/          — 示例应用（模板自带）
board/contest_board/    — 板级适配（模板自带）
logs/yuk1-r/            — AI Coding 日志（Codex Desktop 会话记录）
```

## 四、运行方式

### 环境要求
- Windows + WSL (Ubuntu 22.04)
- BES2800BP_ZE7/JE6_EVB_V1.2 开发板
- USB 串口线（COM20）

### 编译
```bash
export PATH="/mnt/d/XI/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
export PATH="/mnt/d/XI/openvela/prebuilts/cmake/linux-x86_64/bin:$PATH"
export PATH="/mnt/d/XI/openvela/prebuilts/build-tools/linux-x86_64/bin:$PATH"
export CCACHE_DISABLE=1; export TMPDIR="/tmp"
cd /mnt/d/XI/openvela
bash vendor/bes/readme/1700_ap.sh
```

### 烧录
```powershell
cd D:\XI\openvela\flash\vela_2800bp\vela_2800bp
.\dldtool.exe 20 .\programmer1700_dual.bin --set-dual-chip 1 -M .\nuttx_ap.bin --pgm-rate 2000000
```

## 五、AI Coding 使用说明

使用 **Codex Desktop**（VS Code 扩展）进行全程 AI 辅助开发。完整对话日志见 `logs/yuk1-r/` 目录（共10个会话，48MB）。
