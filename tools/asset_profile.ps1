# Shared, side-effect-free asset rendering; this file never opens a serial port.
function Get-ZhaoxiCoreAssets {
    return @('SOUL.md', 'USER.md', 'MEMORY.md', 'cron.json', 'note-taker.md', 'reminder.md')
}

function Read-ZhaoxiAssetText([string]$Path, [string]$DataRoot) {
    if ($DataRoot -notin @('/emmc/zhaoxi', '/data/agent', '/data/ai_agent')) {
        throw 'DataRoot must be /emmc/zhaoxi, /data/agent or /data/ai_agent.'
    }
    $text = [System.IO.File]::ReadAllText($Path, (New-Object System.Text.UTF8Encoding($false)))
    $text = $text.Replace('{{AGENT_DATA_DIR}}', $DataRoot)
    if ($text.Contains('{{AGENT_')) {
        throw "Unresolved asset token: $Path"
    }
    return ($text -replace "`r`n", "`n" -replace "`r", "`n")
}
