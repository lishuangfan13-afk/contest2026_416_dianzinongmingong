#requires -Version 5.1
<#
.SYNOPSIS
  朝夕 Agent 设备端资产部署脚本（半自动，串口 + NSH echo 重定向）。

.DESCRIPTION
  用法（在 contest2026_416_dianzinongmingong 仓库任意目录下，PowerShell 5.1+）：
      powershell -File tools\deploy_assets.ps1                 # 默认 COM3, 921600
      powershell -File tools\deploy_assets.ps1 -Port COM20 -Baud 921600
      powershell -File tools\deploy_assets.ps1 -NoVerify       # 跳过 cat 回读校验

  实现策略说明（为什么不用 vela> CLI 的 install_skill / memory_write）：
  - 已核实 packages/ai_agent/src/channels/nsh_commands.c：
    * install_skill <name> <url> 只接受 https:// URL，帮助里的 stdin 模式("-")并未实现；
    * memory_write 经 tokenise() 按空白分词后只取 argv[1]，且行缓冲 LINE_LEN=256，
      无法写入多行中文 MEMORY.md；
    * ask + LLM write_file 依赖网络与模型行为，不可靠。
  - 因此选择在 NSH 控制台（nsh>）下用 `echo "行" >> 文件` 逐行写文件：
    内容无 $ 与反斜杠（已扫描确认），只需把 " 转义为 \"；
    CONFIG_LINE_MAX=256，长行按 UTF-8 字符边界切成 <=160 字节块，
    中间块用 `echo -n`（不换行）、最后一块用 `echo` 换行。
  - 若控制台当前在 vela>（ai_agent 前台运行），脚本会先发送 quit 退回 nsh>。
  - 注意：内置 skill 是“只写不覆盖”（skill_loader.c install_builtin），
    本脚本需在 ai_agent 首次启动前部署，同名团队版 skill 才能屏蔽内置版。

  /data 当前为 tmpfs（LittleFS 分区未启用），每次重启/烧录后需重新运行本脚本。
#>

[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [int]$Baud = 921600,
    [string]$AssetsDir = '',
    [string]$BaseDir = 'auto',
    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($AssetsDir)) {
    $scriptRoot = $PSScriptRoot
    if ([string]::IsNullOrEmpty($scriptRoot)) {
        $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $AssetsDir = Join-Path $scriptRoot '..\assets\agent_deploy'
}

# ── 资产 → 设备路径映射 ─────────────────────────────────────────
# 注意：ai_agent 的数据根目录存在两个版本——当前固件（9月6日构建）用 /data/ai_agent，
# 工作区最新源码（agent_config.h）已改为 /data/agent。脚本默认自动探测（-BaseDir auto），
# 也可显式指定：-BaseDir /data/agent
# 映射关系（<BASE> 为探测到的数据根目录）：
#   SOUL.md   → <BASE>/config/SOUL.md      USER.md  → <BASE>/config/USER.md
#   MEMORY.md → <BASE>/memory/MEMORY.md    cron.json → <BASE>/cron.json（cron_service 读这里，不是 config/ 下）
#   4 个 skill → <BASE>/skills/
$AssetNames = @('SOUL.md', 'USER.md', 'MEMORY.md', 'cron.json', 'daily-briefing.md', 'health-reminder.md', 'note-taker.md', 'reminder.md')

function Get-RemotePath([string]$base, [string]$name) {
    switch ($name) {
        'SOUL.md'   { return "$base/config/SOUL.md" }
        'USER.md'   { return "$base/config/USER.md" }
        'MEMORY.md' { return "$base/memory/MEMORY.md" }
        'cron.json' { return "$base/cron.json" }
        default     { return "$base/skills/$name" }
    }
}

$Script:Buffer = New-Object System.Text.StringBuilder

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# 读取串口直到出现目标提示符（nsh> 或 vela>），返回命中的提示符
function Wait-Prompt([System.IO.Ports.SerialPort]$sp, [int]$timeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        try {
            $chunk = $sp.ReadExisting()
        } catch {
            Start-Sleep -Milliseconds 50
            continue
        }
        if ($chunk) {
            [void]$Script:Buffer.Append($chunk)
            $tail = $Script:Buffer.ToString()
            if ($tail -match '(nsh|ap|vela)>[\s\x1b\[\]0-9;A-Za-z]*$') { return $Matches[1] }
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
    return $null
}

# 发送一条 NSH 命令并等待 nsh>/ap> 提示符返回
# 注意：板端串口无流控且 RX FIFO 很小，整条命令一次性写入会丢字节，
# 必须按 32 字节切片节流发送（实测 100+ 字节的命令会整行丢失）。
function Send-Nsh([System.IO.Ports.SerialPort]$sp, [string]$cmd, [int]$timeoutMs = 5000, [string]$AllowError = '') {
    [void]$Script:Buffer.Clear()
    $bytes = $sp.Encoding.GetBytes($cmd + "`n")
    for ($off = 0; $off -lt $bytes.Length; $off += 32) {
        $len = [Math]::Min(32, $bytes.Length - $off)
        $sp.Write($bytes, $off, $len)
        Start-Sleep -Milliseconds 30
    }
    $p = Wait-Prompt $sp $timeoutMs
    if ($null -eq $p) {
        Fail "等待提示符超时（命令: $($cmd.Substring(0, [Math]::Min(60, $cmd.Length)))...）。请检查串口连接与控制台状态。"
    }
    # NSH 解析失败（如不支持的转义）只打印 nsh: 错误但仍回提示符，必须显式检测
    $resp = $Script:Buffer.ToString()
    if ($resp -match '(?m)^nsh: .*$') {
        if ($AllowError -eq '' -or $Matches[0] -notmatch [regex]::Escape($AllowError)) {
            Fail "NSH 执行报错: $($Matches[0])（命令: $($cmd.Substring(0, [Math]::Min(60, $cmd.Length)))...）"
        }
    }
    return $p
}

# 把一行文本按 <=$maxBytes UTF-8 字节切块（不切断多字节字符、不切断 emoji 代理对）
function Split-Utf8([string]$line, [int]$maxBytes) {
    $chunks = New-Object System.Collections.Generic.List[string]
    $sb = New-Object System.Text.StringBuilder
    $bytes = 0
    for ($ci = 0; $ci -lt $line.Length; $ci++) {
        # 代理对（emoji 等增补平面字符）必须整体处理，拆开会被编码层替换成 '?'
        if ([char]::IsHighSurrogate($line[$ci]) -and $ci + 1 -lt $line.Length) {
            $unit = $line.Substring($ci, 2)
            $ci++
        } else {
            $unit = [string]$line[$ci]
        }
        $b = [Text.Encoding]::UTF8.GetByteCount($unit)
        if ($bytes + $b -gt $maxBytes) {
            $chunks.Add($sb.ToString())
            [void]$sb.Clear()
            $bytes = 0
        }
        [void]$sb.Append($unit)
        $bytes += $b
    }
    if ($sb.Length -gt 0) { $chunks.Add($sb.ToString()) }
    # 关键：必须加逗号防止 PowerShell 把单元素 List 展开成裸 string
    # （裸 string 的 .Count 恒为 1、[0] 会取首字符，曾导致单行块只写入首字符）
    return ,$chunks
}

# NSH 双引号内转义：实测本固件 NSH 不支持 \" 转义（报 no matching "）。
# 因此含 ASCII 双引号的行必须改用单引号包裹（单引号内全部字面，无需转义）；
# 本函数只用于不含双引号的行，防御性处理 \ 与 $。
function Escape-Nsh([string]$s) {
    return $s.Replace('\', '\\').Replace('$', '\$')
}

function Deploy-File([System.IO.Ports.SerialPort]$sp, [string]$local, [string]$remote) {
    $text = [System.IO.File]::ReadAllText($local, (New-Object System.Text.UTF8Encoding($false)))
    $text = $text -replace "`r`n", "`n" -replace "`r", "`n"
    $lines = $text.Split("`n")
    # 去掉末尾因文件结尾换行产生的空元素
    if ($lines.Count -gt 1 -and $lines[$lines.Count - 1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }

    # cd 到目标目录后用短文件名，给内容留出最大空间。
    # 背景：本固件 NSH readline 行缓冲只有 80 字节，超限会被截断并立即执行，
    # 因此每条命令总字节必须 <= 76（含 echo/引号/重定向/文件名/换行）。
    $dir = $remote.Substring(0, $remote.LastIndexOf('/'))
    $leaf = $remote.Substring($remote.LastIndexOf('/') + 1)
    Send-Nsh $sp "cd $dir" | Out-Null
    $wrapperBytes = [Text.Encoding]::UTF8.GetByteCount("echo -n `"`" >> $leaf`n")
    $maxContent = 76 - $wrapperBytes
    if ($maxContent -lt 8) { Fail "目标文件名过长，无法安全分块: $remote" }

    $first = $true
    foreach ($line in $lines) {
        if ($line -eq '') {
            $op = $(if ($first) { '>' } else { '>>' })
            Send-Nsh $sp "echo $op $leaf" | Out-Null
            $first = $false
            continue
        }
        $chunks = Split-Utf8 $line $maxContent
        # 含 ASCII 双引号的行整行改用单引号包裹（NSH 不支持 \" 转义）；
        # 同时含单双引号的行当前资产不存在，出现时直接报错而不是静默写错
        $useSingleQuote = $line.Contains('"')
        if ($useSingleQuote -and $line.Contains("'")) {
            Fail "行内同时包含单双引号，暂不支持部署：$line"
        }
        for ($i = 0; $i -lt $chunks.Count; $i++) {
            $op = $(if ($first) { '>' } else { '>>' })
            # 同一条逻辑行的中间块用 echo -n 不追加换行
            $nflag = $(if ($i -lt $chunks.Count - 1) { '-n ' } else { '' })
            if ($useSingleQuote) {
                Send-Nsh $sp "echo $nflag'$($chunks[$i])' $op $leaf" | Out-Null
            } else {
                Send-Nsh $sp "echo $nflag`"$(Escape-Nsh $chunks[$i])`" $op $leaf" | Out-Null
            }
            $first = $false
        }
    }
}

# ── 防御性检查 ────────────────────────────────────────────────────
if (-not (Test-Path -LiteralPath $AssetsDir -PathType Container)) {
    Fail "资产目录不存在: $AssetsDir（请用 -AssetsDir 指定 assets/agent_deploy 路径）"
}
$AssetsDir = (Resolve-Path -LiteralPath $AssetsDir).Path
foreach ($name in $AssetNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $AssetsDir $name) -PathType Leaf)) {
        Fail "缺少资产文件: $name（目录 $AssetsDir）"
    }
}

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)
$sp.NewLine = "`n"
$sp.ReadTimeout = 500
$sp.WriteTimeout = 2000
$sp.ReadBufferSize = 1048576
$sp.Handshake = [System.IO.Ports.Handshake]::None

try {
    $sp.Open()
} catch {
    Fail "打不开串口 $Port（$($_.Exception.Message)）。请确认端口存在、未被其他终端程序占用。"
}

try {
    Write-Host "串口 $Port @ $Baud 已打开，正在探测控制台提示符..."

    # 唤醒控制台并探测提示符
    [void]$Script:Buffer.Clear()
    $sp.Write("`n")
    $prompt = Wait-Prompt $sp 8000
    if ($null -eq $prompt) {
        $sp.Write("`n")
        $prompt = Wait-Prompt $sp 8000
    }
    if ($null -eq $prompt) {
        Fail "未检测到 nsh> 或 vela> 提示符。请确认板子已启动、波特率正确（当前 $Baud）。"
    }

    # 若在 vela>（ai_agent 前台占用控制台），先退出回 NSH
    if ($prompt -eq 'vela') {
        Write-Host "检测到 vela>（ai_agent 运行中），发送 quit 退回 NSH..."
        [void]$Script:Buffer.Clear()
        $sp.Write("quit`n")
        $prompt = Wait-Prompt $sp 15000
        if ($prompt -ne 'nsh' -and $prompt -ne 'ap') {
            Fail "quit 后仍未回到 NSH 提示符（nsh>/ap>）。请手动在串口终端退出 ai_agent 后重试。"
        }
    }
    Write-Host "已进入 NSH 控制台。"

    # 探测数据根目录：/data 下已有 ai_agent/（旧固件）或 agent/（新源码）则沿用；
    # 都没有（agent 从未运行）时默认 /data/ai_agent（与当前固件一致）
    if ($BaseDir -eq 'auto') {
        [void]$Script:Buffer.Clear()
        $sp.Write("ls /data`n")
        $null = Wait-Prompt $sp 5000
        $listing = $Script:Buffer.ToString()
        if ($listing -match 'ai_agent') {
            $BaseDir = '/data/ai_agent'
        } elseif ($listing -match '(?m)^\s*agent/?\s*$') {
            $BaseDir = '/data/agent'
        } else {
            $BaseDir = '/data/ai_agent'
            Write-Host "/data 下未发现已有 agent 目录，默认使用 $BaseDir（当前固件路径）"
        }
    }
    Write-Host "目标数据根目录: $BaseDir"

    # 构建文件映射
    $FileMap = [ordered]@{}
    foreach ($name in $AssetNames) {
        $FileMap[$name] = Get-RemotePath $BaseDir $name
    }

    # 确保目录存在（目录已存在时报 EEXIST=17，属预期，静默放行）
    foreach ($dir in @('/data', $BaseDir, "$BaseDir/config", "$BaseDir/memory", "$BaseDir/skills")) {
        Send-Nsh $sp "mkdir $dir" 5000 'failed: 17' | Out-Null
    }

    # 逐文件部署 + 校验，校验失败的文件自动重试（921600 串口偶有单比特传输错误，
    # 首写用 > 覆盖，重试同一文件是幂等安全的）
    $maxAttempts = 3
    $pending = @($FileMap.Keys)
    for ($attempt = 1; $attempt -le $maxAttempts -and $pending.Count -gt 0; $attempt++) {
        if ($attempt -gt 1) {
            Write-Host "`n第 $attempt 轮重试：$($pending -join ', ')"
        }
        foreach ($name in $pending) {
            $local = Join-Path $AssetsDir $name
            $remote = $FileMap[$name]
            Write-Host "部署 $name -> $remote ..."
            Deploy-File $sp $local $remote
        }

        if ($NoVerify) { $pending = @(); break }

        Write-Host "`n开始 cat 回读校验..."
        $failed = @()
        foreach ($name in $pending) {
            $local = Join-Path $AssetsDir $name
            $remote = $FileMap[$name]
            [void]$Script:Buffer.Clear()
            $sp.Write("cat $remote`n")
            $p = Wait-Prompt $sp 15000
            if ($p -ne 'nsh' -and $p -ne 'ap') {
                Write-Host "  FAIL $name（cat 无响应）" -ForegroundColor Red
                $failed += $name
                continue
            }
            $got = $Script:Buffer.ToString()
            # 去掉首尾提示符/回显噪声
            $got = $got -replace '(?s)^.*?cat [^\n]*\n', ''
            $got = $got -replace '(?s)(nsh|ap)>[\s\x1b\[\]0-9;A-Za-z]*$', ''
            $got = ($got -replace "`r`n", "`n" -replace "`r", "`n").Trim("`n", " ", "`t")
            $expected = ([System.IO.File]::ReadAllText($local, (New-Object System.Text.UTF8Encoding($false))) `
                -replace "`r`n", "`n" -replace "`r", "`n").Trim("`n", " ", "`t")
            if ($got -eq $expected) {
                Write-Host "  OK   $name" -ForegroundColor Green
            } else {
                Write-Host "  FAIL $name（内容不一致，设备端 $([Text.Encoding]::UTF8.GetByteCount($got)) 字节 / 本地 $([Text.Encoding]::UTF8.GetByteCount($expected)) 字节）" -ForegroundColor Red
                $failed += $name
            }
        }
        $pending = $failed
    }
    if ($pending.Count -gt 0) {
        Fail "以下文件经 $maxAttempts 轮仍校验失败：$($pending -join ', ')。串口链路误码率偏高，请检查 USB 线/接地后重试。"
    }

    Write-Host "`n全部 8 个资产部署完成。现在可以在 nsh> 启动 ai_agent（内置 skill 将因同名文件已存在而被跳过）。"
    Write-Host "注意：/data 为 tmpfs，板子重启后需重新运行本脚本。"
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
