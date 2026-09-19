---
name: openvela-dev
description: 朝夕项目的开发闭环总路由。当任务涉及 openvela/NuttX 嵌入式开发的全流程安排（调查、派工、构建、烧录、验证、部署、验收、入仓）时，先读本技能确定阶段与路由，再加载对应的专业技能。
---

# openvela 开发闭环（元技能）

## 何时使用

- 开始一个新的功能/修复任务，不确定该先做什么时
- 需要决定"先调查还是先动手"时
- 准备提交代码，检查 git 纪律时

## 开发闭环（七阶段）

每个阶段有明确的"完成判据"，判据不满足不得进入下一阶段：

| 阶段 | 做什么 | 完成判据 | 路由 |
|------|--------|----------|------|
| 1. 只读调查 | 读源码/日志，建立事实清单 | 每条结论带 file:line | `evidence-driven-debugging` |
| 2. 拆解派工 | 把任务切成单文件所有权的子任务 | 每个子任务边界清晰、禁 git | `scoped-agent-dispatch` |
| 3. 构建 | WSL 增量编译 | 无 error，产物 bin 更新 | `board-bringup` |
| 4. 烧录验证 | NTC 补丁 → AP-only 烧录 → 串口看 DIAG | DIAG 输出与预期一致 | `board-bringup` |
| 5. 资产部署 | 串口推送 Skill/cron/人格文件 | cat 回读逐字节一致 | `device-skill-authoring` |
| 6. 验收 | 对照任务书逐条核对 | 全部 ✅ 或明确记录未过项 | 本技能 |
| 7. 入仓 | 按纪律 commit + format-patch | diff 干净（无 CRLF 噪音） | 本技能"git 纪律" |

## 核心哲学（三条）

1. **证据优先于声称**："应该可以了"不算完成，串口输出才算。
2. **一次烧录、充分验证**：烧录成本高（全量构建 ~40 分钟），每次烧录前让 DIAG 层把所有疑点暴露出来。
3. **增量优先**：小步改、快编译（增量 8–10 分钟 vs 全量 ~40 分钟），defconfig 变更才会触发全量。

## git 纪律（红线，源自真实踩坑）

- **任何仓禁止 `git add -A` / `add -u`**，必须逐文件精确 add——曾因噪音文件差点把 234 个 CRLF 假 diff 提交入仓。
- **先设 `core.autocrlf=false` 再看 diff**；仍有噪音时用 `--ignore-cr-at-eol` 复核。曾出现 764/756 行的整文件假 diff，根因是编辑工具把 LF 写成了 CRLF。
- **智能体编辑文件必须保持原换行符（LF）**，任务书里要显式写明。
- **AI 不得执行 git 提交**：commit/push 由主控统一执行（派工时的默认禁令）。
- **对公共仓的改动走 `git format-patch` → 仓内 `patches/` → `git am` 复现**，保证评委可一键复现。
- **提交作者归属要检查**：曾出现 AI 用默认身份提交、事后需 commit-tree/rebase 重写的情况，预防成本远低于返工。

## WSL 调用要点

- 发行版 `Ubuntu-22.04`，构建命令：`wsl.exe -d Ubuntu-22.04 -- bash /mnt/c/openvela_ws/build_ap.sh`
- 从 Git Bash 调 WSL 必须加 `MSYS_NO_PATHCONV=1` 前缀，否则 `/mnt` 路径会被改写。
- 工作区路径硬引用（repo manifest、编译脚本、日志采集 hook），**不可移动**。

## 已知坑速查

| 症状 | 根因 | 处置 |
|------|------|------|
| 修改 rcS 后烧录无变化 | CMake 缓存了预处理后的 rcS | 删 `cmake_out/aos_evb_ap/etc/init.d/rcS` 后重编 |
| 时钟停在 1970、TLS 全挂 | 无 RTC，证书时间校验失败 | HTTP Date 头对时 + 编译期时间戳兜底（补丁 0001） |
| 重启后资产全丢 | /data 为 tmpfs（分区表无 data 分区） | 重新跑部署脚本；根治需分区表启用 LittleFS |
| DNS 解析失败 | 无 resolv.conf | defconfig 设默认 DNS 223.5.5.5（补丁 0003） |
