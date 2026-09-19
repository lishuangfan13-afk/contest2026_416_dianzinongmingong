# 朝夕开发技能集（zhaoxi dev skills）

本目录是「朝夕」项目在 openvela/BES2800BP 嵌入式 AI 硬件开发中沉淀的 **6 个可复用开发经验 Skill**，全部内容提炼自本项目 34 段 AI Coding 会话日志、5 轮真机固件验证与任务台账的派工/验收记录，不含任何虚构案例。

## 设计理念

技能库的组织方式参考了社区流行的 agent skill 体系的三条通用设计：

1. **SKILL.md 规范**：每个技能一个目录，frontmatter 声明 `name` 与触发条件（`description`），正文按"何时使用 → 流程 → 已知坑 → 红线"组织，供 AI 按需加载（渐进披露）。
2. **工作流链**：技能不是孤岛，按开发闭环串联（见下方"标准工作流"），前一个技能的产出是后一个的输入。
3. **证据优先**：所有结论必须落到可验证的证据上（串口 DIAG 输出、file:line 引用、回读校验），与"声称完成"严格区分。

## 技能清单

| 技能 | 类型 | 一句话 |
|------|------|--------|
| `openvela-dev` | 元技能 | 开发闭环总路由：调查→派工→构建→烧录→验证→部署→验收，含 git 纪律 |
| `board-bringup` | 平台 | BES2800BP/best1700 构建、NTC 二进制补丁、AP-only 烧录红线 |
| `evidence-driven-debugging` | 调试 | DIAG 自诊断层 + 只读调查先行 + file:line 证据强制 |
| `scoped-agent-dispatch` | AI 协作 | 权限收窄的多智能体派工模式（角色声明/文件所有权/禁 git/统一 review） |
| `device-skill-authoring` | 框架 | 为 ai_agent 编写设备端 Skill 与串口资产部署，绕开框架已知限制 |
| `cjk-font-pipeline` | UI | LVGL 中文字体子集自动提取与覆盖率构建闸门 |

## 标准工作流

```text
① 只读调查（evidence-driven-debugging）
      ↓ 产出：file:line 级事实清单
② 派工拆解（scoped-agent-dispatch）
      ↓ 产出：单文件所有权 + 禁 git 的子任务
③ 构建（board-bringup）
      ↓ 产出：nuttx_ap.bin + NTC 补丁
④ 烧录 + 串口验证（board-bringup / DIAG）
      ↓ 产出：DIAG 输出证据
⑤ 资产部署（device-skill-authoring）
      ↓ 产出：回读校验通过的设备端文件
⑥ 验收与入仓（openvela-dev 的 git 纪律）
      ↓ 产出：干净提交 + format-patch
```

## 如何使用

- **Claude Code / 支持 SKILL.md 的工具**：将本仓库作为项目打开，技能会被自动发现；也可直接把对应 `SKILL.md` 内容粘贴给任意 AI 编程助手作为任务指引。
- **人类工程师**：每个技能的"已知坑"表格本身就是一份踩坑手册，按表排查即可。

## 如何扩展

新技能遵循同一规范：新建目录 → `SKILL.md`（frontmatter + 何时使用/流程/已知坑/红线）→ 在本 README 登记 → 在 `openvela-dev` 的路由表中登记。**要求：只沉淀被日志或串口证据支撑过的经验，不写猜测。**
