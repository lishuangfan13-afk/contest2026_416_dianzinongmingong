# 朝夕设备端业务资产

本目录提供朝夕的人格配置、智能记事本和智能提醒 Skill。固件内置本地记事、待办、相对时间提醒和喝水控制，常用明确指令直接在板上执行；这些资产为问答和其他自由表达提供业务规则。

首次部署的 `cron.json` 是空任务列表，喝水提醒由使用者主动开启。喝水功能已经包含在产品固件中，通过 `drink` 命令或明确中文指令管理，无需额外安装旧版健康提醒 Skill。每日简报和久坐等旧资产保留在 [legacy/agent_extensions](../../legacy/agent_extensions)，当前部署列表使用两个业务 Skill。

## 部署内容

| 文件 | 设备位置 | 作用 |
| --- | --- | --- |
| `SOUL.md`、`USER.md` | `<BASE>/config/` | 产品身份、对话方式与用户配置 |
| `MEMORY.md` | `<BASE>/memory/MEMORY.md` | 长期记忆初始文件 |
| `cron.json` | `<BASE>/cron.json` | 初始调度列表 |
| `note-taker.md`、`reminder.md` | `<BASE>/skills/` | 自由表达的记事、待办和提醒规则 |

清单由 [asset_profile.ps1](../../tools/asset_profile.ps1) 统一定义。当前数据根目录 `<BASE>` 为 `/emmc/zhaoxi`；[部署脚本](../../tools/deploy_assets.ps1) 将资产中的 `{{AGENT_DATA_DIR}}` 替换为实际目录，写入后逐文件回读校验。

## 部署步骤

1. 使用 `python tools/pc_link/launch.py --stop` 正常结束电脑供网，等待板子回到控制台、串口释放。若直接运行了后台 Agent，则在 NSH 执行 `zhaoxi stop`，等待 `Shutdown complete`。
2. 在仓库根目录运行部署脚本，按实际端口替换 `COM3`：

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools/deploy_assets.ps1 -Port COM3 -BaseDir /emmc/zhaoxi
   ```

3. 保留脚本的回读校验，完成后按日常入口启动：

   ```powershell
   python tools/pc_link/launch.py --port COM3
   ```

日常入口会校准板上 UTC、建立 PPP，再启动业务服务。维护时也可在 NSH 使用 `ai_agent --headless &`；同一时刻只运行一个 Agent，供网运行期间通过控制通道操作业务。

更新会替换人格配置和两个 Skill；`MEMORY.md`、`cron.json` 只在目标不存在时初始化，已有记忆与提醒保留。当天备忘和 `TASKS.md` 由业务模块维护，API 凭据另存设备配置。

## 业务怎样执行

- **问答：** 板端 Agent 调用当前模型，语音回答通过增量 TTS 和 PCM 播放输出。
- **记事与待办：** 明确命令和 [有限中文句式](../../docs/本地中文指令.md) 进入本地模块，保存并回读后确认；其他表达结合智能记事本 Skill 和文件工具处理。
- **定时提醒：** 明确相对时间直接创建并读回任务，复杂表达使用 `cron_add`、`cron_list`、`cron_remove`。普通到期提醒投递到 `voice` 通道。
- **喝水控制：** `drink start`、`drink status`、`drink stop` 与对应中文句式共用本地周期任务，关闭时只移除本功能的任务。

默认联网入口保持麦克风关闭。三项核心的完整操作见 [功能说明](../../docs/无屏核心功能.md)，喝水控制见 [喝水提醒](../../docs/喝水提醒.md)，版本与安装见 [BES 固件构建与验收](../../docs/BES固件构建与验收.md)。
