# BES2800BP 开发进度总结 — 「朝夕」AI 生活管家

## 一、项目概述

- **项目名称**：「朝夕」—— 主动式 AI 生活管家
- **开发板**：BES2800BP_ZE7/JE6_EVB_V1.2（best1700_ep 平台）
- **系统**：openvela（NuttX 内核，分支 dev-ai-contest-2026）
- **屏幕**：464×454 @ 32bpp，DSI 接口
- **触摸**：TMA525C 触摸面板，/dev/input0
- **串口**：COM20 @ 921600 波特率（USB Serial Port）
- **比赛截止**：2026年9月20日

---

## 二、固件版本历史

| 版本 | 日期 | 内容 | 备份路径 |
|------|------|------|----------|
| v1 | 9/5 | 基础固件（lvgldemo） | work/backup/v1_working_20260905/ |
| v2 | 9/5 | + AI Agent | work/backup/v2_ai_agent_20260905/ |
| v3 | 9/6 | + TLS 网络对时 + NTC 补丁 | work/backup/v3_tls_fix_20260906/ |
| v5 | 9/6 | + zhaoxi_ui 独占显示 + rcS 修复 | work/backup/v5_zhaoxi_ui_20260906/ |

---

## 三、已解决的关键问题

### 3.1 MSYS2 下 GCC 16.2 编译失败
- **现象**：cc 编译 incdir.c 返回 rc=1 但无错误输出
- **根因**：UCRT64 的 GCC 16.2 与 NuttX host tools 不兼容
- **解决**：改用 WSL（Ubuntu-22.04）+ 官方 1700_ap.sh 构建脚本

### 3.2 NTC 断言崩溃（pmu_ntc_monitor_init）
- **现象**：固件启动时 assertion failed valid_cnt，板子循环崩溃
- **根因**：板子无 NTC 热敏电阻，GPADC 通道读数为 0，触发断言
- **解决**：二进制补丁跳过断言检查
  - 地址：0x101947bc（文件偏移 0x47bc）
  - 原始字节：0xb955（cbnz r5）
  - 补丁字节：0xe011（b 跳转）
  - 工具：Node.js Buffer 操作（避免 shell 转义问题）

### 3.3 网络对时（vela_tls.c）
- **现象**：系统时钟卡在 1970 年，TLS 证书验证失败
- **根因**：vela_tls.c 硬编码 clock_settime(1772275200) = 2026-02-28
- **解决**：添加 sync_time_via_http_date() 函数，HTTP HEAD 请求获取 Date 头

### 3.4 rcS 启动脚本未更新
- **现象**：修改 defconfig 后 rcS 仍启动 lvgldemo 而非 zhaoxi_ui
- **根因**：CMake 缓存了预处理后的 rcS 文件
- **解决**：删除 cmake_out/aos_evb_ap/etc/init.d/rcS 后重编
- **修改文件**：vendor/bes/boards/best1700_ep/aos_evb/src/etc/init.d/rcS.ap
  - 将 lvgldemo widgets & 改为 zhaoxi_ui &

---

## 四、烧录方法（重要经验）

### 4.1 烧录命令
```powershell
cd D:\XI\openvela\flash\vela_2800bp\vela_2800bp
.\dldtool.exe 20 .\programmer1700_dual.bin --set-dual-chip 1 -M .\nuttx_ap.bin --pgm-rate 2000000
```

### 4.2 进入下载模式的方法
- **板子只有 POWER 和 RESET 两个按钮**，没有 BOOT/DL 按钮
- **方法一（板子在崩溃循环中）**：直接启动 dldtool，按 RESET 即可触发 SYNC
- **方法二（板子正常运行时）**：使用 dldtool 的 --reboot 参数（通过串口发复位命令）
- **方法三（最可靠）**：如果板子在 OTA 引导状态（串口输出 CHIP=best1700 等信息），直接启动 dldtool 即可自动 SYNC

### 4.3 烧录注意事项
- **只烧 nuttx_ap.bin**（-M 参数），不要烧 nuttx_ota.bin 或 nuttx_bl.bin
- **烧录前杀掉所有 Python 进程**，避免 COM 口被抢占导致烧录中断
- **烧录时不要运行任何串口诊断脚本**
- **烧录后需要拔插 USB 重启板子**（烧录完成后板子进入 SYS SHUTDOWN 状态）

### 4.4 NTC 补丁流程
```javascript
// Node.js 补丁脚本
const fs = require('fs');
const bin = 'cmake_out/aos_evb_ap/nuttx_ap.bin';
const out = 'flash/vela_2800bp/vela_2800bp/nuttx_ap.bin';
const data = fs.readFileSync(bin);
const off = 0x101947bc - 0x10190000; // = 0x47bc
if (data.readUInt16LE(off) === 0xb955) {
    const patched = Buffer.from(data);
    patched.writeUInt16LE(0xe011, off);
    fs.writeFileSync(out, patched);
}
```

---

## 五、构建流程

### 5.1 环境要求
- WSL（Ubuntu-22.04）
- ARM 工具链：prebuilts/gcc/linux-x86_64/arm-none-eabi/bin
- CMake：prebuilts/cmake/linux-x86_64/bin
- Build tools：prebuilts/build-tools/linux-x86_64/bin

### 5.2 构建命令
```bash
export PATH="/mnt/d/XI/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
export PATH="/mnt/d/XI/openvela/prebuilts/cmake/linux-x86_64/bin:$PATH"
export PATH="/mnt/d/XI/openvela/prebuilts/build-tools/linux-x86_64/bin:$PATH"
export CCACHE_DISABLE=1
export TMPDIR=/tmp
cd /mnt/d/XI/openvela
bash vendor/bes/readme/1700_ap.sh
```

### 5.3 构建时间
- 全量构建：约 40 分钟
- 增量构建（仅修改 rcS）：约 8-10 分钟

---

## 六、当前状态（v5 固件）

### 6.1 已工作
- ✅ 系统启动正常（无 NTC 崩溃）
- ✅ zhaoxi_ui 独占显示（lvgldemo 已禁用）
- ✅ rcS 启动脚本正确
- ✅ AI Agent 编译进固件
- ✅ WiFi 连接正常
- ✅ /dev/input0 设备存在
- ✅ /dev/fb0 帧缓冲存在

### 6.2 未解决
- ❌ 触摸屏无响应（设备存在但无事件产生）
- ❌ DNS 解析失败（无 /etc/resolv.conf）
- ❌ 系统时钟不准确（网络对时未验证）

---

## 七、触摸问题详细分析

### 7.1 现象
- /dev/input0 设备存在（crw-rw-rw- 权限正常）
- lvgldemo 下触摸正常工作
- zhaoxi_ui 下触摸完全无响应（cat /dev/input0 无数据）

### 7.2 已排除的原因
- 硬件连接：lvgldemo 下触摸正常，排除硬件问题
- 设备节点：/dev/input0 存在且权限正确
- defconfig：CONFIG_INPUT_TOUCHSCREEN=y、CONFIG_BES_TP_TMA525C=y、CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y 均已启用
- 代码初始化：zhaoxi_ui.c 中 info.input_path = "/dev/input0" 正确设置

### 7.3 可能的原因
- lvgldemo 使用 lv_nuttx_uv_loop（libuv 事件循环）处理输入
- zhaoxi_ui 使用 lv_timer_handler 轮询
- 可能是 LVGL 输入驱动注册方式差异

---

## 八、下一步计划

1. 修复触摸屏问题（对比 lvgldemo 的输入初始化方式）
2. 实现屏幕切换（导航栏按钮切换 Home/Chat/Settings 视图）
3. 构建 Chat 界面（LVGL 文本输入 + AI Agent 对话）
4. 修复 DNS 和时钟问题
5. 部署 Skills 和 cron 定时任务
6. 备份当前可用固件

---

## 九、关键文件路径

| 文件 | 路径 |
|------|------|
| defconfig | vendor/bes/boards/best1700_ep/aos_evb/configs/ap/defconfig |
| rcS.ap | vendor/bes/boards/best1700_ep/aos_evb/src/etc/init.d/rcS.ap |
| zhaoxi_ui 源码 | apps/packages/zhaoxi_ui/src/zhaoxi_ui.c |
| AI Agent 源码 | apps/packages/ai_agent/ |
| 构建脚本 | vendor/bes/readme/1700_ap.sh |
| 烧录工具 | vendor/bes/prebuild/m1/dldtool.exe |
| Programmer | vendor/bes/prebuild/programmer1700_dual.bin |
| 固件输出 | cmake_out/aos_evb_ap/nuttx_ap.bin |
| 烧录目录 | D:\XI\openvela\flash\vela_2800bp\vela_2800bp\ |
| 备份目录 | D:\XI\openvela\work\backup\ |

---

## 十、WiFi 和 AI Agent 配置

### WiFi 连接
```
# NSH 命令
ifup wlan0
wapi mode wlan0 2
wapi psk wlan0 <密码> 3
wapi essid wlan0 <SSID> 1
renew wlan0
```

### AI Agent LLM 配置
```
# ai_agent shell 命令
set_llm https://api.xiaomimimo.com/v1 mimo-v2.5-pro sk-c26emduem3clnuqn4ys4tqa6jrgs0jkz9i1ltb0e2nytwxef
set_wifi <SSID> <密码>
```

---

*文档生成时间：2026-09-06*

---

## 十一、触摸问题修复尝试（进行中）

### 11.1 发现的关键差异
- lvgldemo 使用 `lv_nuttx_uv_init` + libuv 事件循环处理输入
- zhaoxi_ui 使用 `lv_timer_handler` 轮询
- libuv 通过文件描述符回调驱动输入，lv_timer_handler 通过轮询

### 11.2 修复方案
- 在 defconfig 中重新启用 CONFIG_EXAMPLES_LVGLDEMO=y（提供 libuv 支持）
- rcS.ap 保持只启动 zhaoxi_ui（lvgldemo 编译但不启动）
- zhaoxi_ui 先初始化 LVGL，lvgldemo 检测到已初始化会自动退出

### 11.3 构建状态
- 正在全量重编（预计 40 分钟）
- 构建完成后需要：NTC 补丁 → 烧录 → 验证触摸

---

*最后更新：2026-09-06 18:50*
