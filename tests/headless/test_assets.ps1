$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'tools\asset_profile.ps1')
$names = @(Get-ZhaoxiCoreAssets)
if ($names.Count -ne 6) { throw 'Expected six core assets.' }
foreach ($base in @('/emmc/zhaoxi', '/data/agent', '/data/ai_agent')) {
    foreach ($name in $names) {
        $file = Join-Path $repo "assets\agent_deploy\$name"
        $text = Read-ZhaoxiAssetText $file $base
        if ($text.Contains('{{AGENT_')) { throw 'Unresolved root token.' }
        $other = if ($base -eq '/data/agent') { '/data/ai_agent/' } else { '/data/agent/' }
        if ($text.Contains($other)) { throw 'Mixed data roots.' }
    }
}
$failed = $false
try { Read-ZhaoxiAssetText (Join-Path $repo 'assets\agent_deploy\SOUL.md') '/other' | Out-Null }
catch { $failed = $true }
if (-not $failed) { throw 'Invalid data root accepted.' }
$tokens = $null
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools\deploy_assets.ps1'), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
Write-Output 'PASS: persistent and historical data roots, six assets, invalid root and deployment syntax.'
