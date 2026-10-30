#Requires -Version 5.1
<#
  gen_cjk_fonts.ps1 - Regenerate the CJK subset LVGL fonts used by zhaoxi_ui.

  Produces zhaoxi_font_20/22/24/28.c next to zhaoxi_ui.c. The UI uses
  Montserrat (ASCII-only) for its default font, so every Chinese string
  rendered as tofu boxes. These subsets add the exact glyphs the UI needs
  plus the FontAwesome symbols embedded in the same font object.

  The Chinese character set is EXTRACTED AUTOMATICALLY from the UI sources
  under app\zhaoxi_ui\src (every .c/.h), so adding a new UI string no longer
  requires editing a hardcoded codepoint list here.

  Re-run after changing the UI strings:
      powershell -ExecutionPolicy Bypass -File tools\gen_cjk_fonts.ps1

  Verify the result with:
      powershell -ExecutionPolicy Bypass -File tools\check_cjk_coverage.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

# ---- Paths (derived from this script so the team can move the repo) -------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$SrcDir    = Join-Path $RepoRoot 'app\zhaoxi_ui\src'
$OutDir    = $SrcDir

$LvFontConv = 'C:\Users\27981\AppData\Roaming\npm\node_modules\lv_font_conv\lv_font_conv.js'
$CjkFont    = 'C:\Windows\Fonts\simhei.ttf'
$FaWoff     = 'C:\openvela_ws\openvela\apps\graphics\lvgl\lvgl\scripts\built_in_font\FontAwesome5-Solid+Brands+Regular.woff'
$SymbolDef  = Join-Path $RepoRoot '..\apps\graphics\lvgl\lvgl\src\font\lv_symbol_def.h'

# FontAwesome codepoints the UI has historically relied on. The set actually
# passed to lv_font_conv is the union of this list and every LV_SYMBOL_* that
# is referenced by the sources, so removing a UI symbol cannot silently drop
# coverage for another one.
$FaExplicit = @(61451, 61452, 61459, 61461, 61561, 61683, 61931)

$Sizes = @(20, 22, 24, 28)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Read a text file as UTF-8. Get-Content mangles BOM-less UTF-8 under PS 5.1.
function Read-Utf8Text([string]$Path) {
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

# True for the Unicode ranges the UI may render.
function Test-IsWantedCodepoint([int]$c) {
    return (($c -ge 0x4E00 -and $c -le 0x9FFF) -or   # CJK Unified Ideographs
            ($c -ge 0x3000 -and $c -le 0x303F) -or   # CJK punctuation
            ($c -ge 0xFF00 -and $c -le 0xFFEF) -or   # Fullwidth forms
            ($c -ge 0x2018 -and $c -le 0x201D))      # curly quotes
}

# Collect the .c/.h source files, excluding the generated font files (their
# header comment is ANSI-mangled text and must never feed the extractor).
function Get-SourceFiles([string]$Dir) {
    $all = Get-ChildItem -Path $Dir -Recurse -File -Include '*.c', '*.h'
    return @($all | Where-Object { $_.Name -notlike 'zhaoxi_font_*' })
}

function Get-CjkCodepoints([string]$Dir) {
    $set = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($f in Get-SourceFiles $Dir) {
        $text = Read-Utf8Text $f.FullName
        foreach ($ch in $text.ToCharArray()) {
            $c = [int][char]$ch
            if (Test-IsWantedCodepoint $c) { [void]$set.Add($c) }
        }
    }
    $arr = @($set) | Sort-Object
    return , $arr
}

# Characters that can reach the display at RUNTIME from data files the UI
# reads (cron.json reminder text, WEATHER.md, REMINDER.md, NET_STATUS) plus
# a small margin for weather descriptions returned by the LLM.  Without
# these the UI shows tofu boxes for content it did not hardcode.
# Kept as an explicit literal so the set is stable and reviewable.
function Get-ExtraRuntimeChars() {
    # Weather / reminder vocabulary the agent may write into WEATHER.md and
    # REMINDER.md, plus common cron/notification characters.  Written as
    # codepoints (not literal CJK) so Windows PowerShell 5.1's ANSI decoding
    # of BOM-less UTF-8 cannot mangle them.
    $extraCps = @(
        0x4E00,0x4E09,0x4E0A,0x4E0B,0x4E1C,0x4E2D,0x4E45,0x4E8C,0x4E91,0x4E94,
        0x4EAC,0x4ECA,0x4F11,0x4F1E,0x4F4E,0x4F53,0x4FDD,0x516D,0x51B0,0x51B7,
        0x51BB,0x51C9,0x5206,0x5220,0x529E,0x52A0,0x52A8,0x5317,0x5347,0x5348,
        0x5357,0x53F7,0x5403,0x540E,0x5468,0x559D,0x56DB,0x5733,0x5747,0x5750,
        0x5907,0x5916,0x591A,0x591C,0x5927,0x5929,0x5957,0x5B89,0x5B8C,0x5BD2,
        0x5C0F,0x5C9B,0x5DDE,0x5DF2,0x5E26,0x5E3D,0x5E73,0x5E74,0x5E7F,0x5E86,
        0x5E8A,0x5EA6,0x5EFA,0x5F85,0x5FD8,0x606F,0x611F,0x6210,0x62A5,0x63D0,
        0x65B0,0x65E5,0x65E9,0x65F6,0x660E,0x661F,0x6628,0x6652,0x665A,0x6668,
        0x6674,0x6696,0x66B4,0x6700,0x6708,0x670D,0x671F,0x676D,0x67E5,0x6B66,
        0x6C14,0x6C34,0x6C49,0x6C99,0x6D25,0x6D3B,0x6D77,0x6DF1,0x6DFB,0x6E29,
        0x6E7F,0x6F6E,0x70ED,0x767D,0x7761,0x77ED,0x79D2,0x7A7A,0x7A7F,0x7B80,
        0x7D2B,0x7EA7,0x7EBF,0x7ED2,0x7FBD,0x8212,0x82CF,0x836F,0x8863,0x8896,
        0x88E4,0x897F,0x89C9,0x8BAE,0x8BB0,0x8BE2,0x8D77,0x8F6C,0x9002,0x90D1,
        0x90FD,0x9192,0x91CD,0x957F,0x95FB,0x9632,0x9634,0x964D,0x9664,0x96E8,
        0x96EA,0x96F7,0x96F9,0x96FE,0x971C,0x973E,0x9752,0x98CE,0x9910,0x9AD8
    )
    $set = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($c in $extraCps) {
        if (Test-IsWantedCodepoint $c) { [void]$set.Add($c) }
    }
    return , $set
}

# CJK characters that the UI can actually RENDER from runtime files.  The
# UI only displays the first line of WEATHER.md / REMINDER.md and the
# NET_STATUS line.  REMINDER.md is written from cron.json's message/
# action_args text, and WEATHER.md is written by the LLM using the weather
# vocabulary in Get-ExtraRuntimeChars.  The other asset files (skills, SOUL,
# USER, MEMORY, README) are read by the LLM, never drawn by the UI, so
# including them would needlessly bloat the fonts.
function Get-AssetCjkCodepoints([string]$Dir) {
    $set = New-Object 'System.Collections.Generic.HashSet[int]'
    $cron = Join-Path $Dir 'cron.json'
    if (-not (Test-Path -LiteralPath $cron)) { return , $set }
    $text = Read-Utf8Text $cron
    foreach ($ch in $text.ToCharArray()) {
        $c = [int][char]$ch
        if (Test-IsWantedCodepoint $c) { [void]$set.Add($c) }
    }
    return , $set
}

# Parse lv_symbol_def.h into a name -> codepoint map by decoding the UTF-8
# byte escapes in each #define (immune to the decimal typos in comments).
function Get-LvSymbolMap([string]$Path) {
    $map = @{}
    $text = Read-Utf8Text $Path
    $re = [regex]'#define\s+(LV_SYMBOL_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})*)"'
    foreach ($m in $re.Matches($text)) {
        $name = $m.Groups[1].Value
        $bytes = New-Object 'System.Collections.Generic.List[byte]'
        foreach ($bm in [regex]::Matches($m.Groups[2].Value, '\\x([0-9A-Fa-f]{2})')) {
            $bytes.Add([Convert]::ToByte($bm.Groups[1].Value, 16))
        }
        if ($bytes.Count -eq 0) { continue }
        $decoded = [Text.Encoding]::UTF8.GetString($bytes.ToArray())
        if ($decoded.Length -ge 1) { $map[$name] = [int][char]$decoded[0] }
    }
    return $map
}

# Every LV_SYMBOL_* referenced in the sources, mapped to its codepoint.
function Get-UsedFaCodepoints([string]$Dir, $SymbolMap) {
    $used = New-Object 'System.Collections.Generic.HashSet[int]'
    $names = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($f in Get-SourceFiles $Dir) {
        $text = Read-Utf8Text $f.FullName
        foreach ($m in [regex]::Matches($text, 'LV_SYMBOL_([A-Za-z0-9_]+)')) {
            $name = 'LV_SYMBOL_' + $m.Groups[1].Value
            if (-not $SymbolMap.ContainsKey($name)) {
                throw "Unmapped LV_SYMBOL used in $($f.Name): $name (add it to lv_symbol_def.h or the explicit list)"
            }
            [void]$names.Add($name)
            [void]$used.Add([int]$SymbolMap[$name])
        }
    }
    return [pscustomobject]@{ Codepoints = $used; Names = $names }
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
foreach ($p in @($LvFontConv, $CjkFont, $FaWoff, $SymbolDef)) {
    if (-not (Test-Path -LiteralPath $p)) {
        throw "Required file not found: $p"
    }
}
if (-not (Test-Path -LiteralPath $SrcDir)) {
    throw "UI source directory not found: $SrcDir"
}
if (-not (Test-Path -LiteralPath $OutDir)) {
    throw "Output directory not found: $OutDir"
}
if (-not (Test-Path -LiteralPath (Join-Path $SrcDir 'zhaoxi_ui.c'))) {
    throw "UI source file not found: $(Join-Path $SrcDir 'zhaoxi_ui.c')"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    throw 'node was not found on PATH. Install Node.js and lv_font_conv first.'
}

# ---------------------------------------------------------------------------
# Extract the character set
# ---------------------------------------------------------------------------
$CjkCodepoints = Get-CjkCodepoints $SrcDir
if ($CjkCodepoints.Count -eq 0) {
    throw "No CJK characters extracted from $SrcDir - refusing to generate empty fonts."
}

# Union with runtime data-file characters and the weather/reminder margin so
# strings the UI reads from /data at runtime also render.
$AssetDir = Join-Path $RepoRoot 'assets\agent_deploy'
$allCjk = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($c in $CjkCodepoints) { [void]$allCjk.Add($c) }
$assetCount = 0
foreach ($c in (Get-AssetCjkCodepoints $AssetDir)) { if ($allCjk.Add($c)) { $assetCount++ } }
$extraCount = 0
foreach ($c in (Get-ExtraRuntimeChars)) { if ($allCjk.Add($c)) { $extraCount++ } }
$CjkCodepoints = @($allCjk) | Sort-Object

$CjkChars = -join [char[]]$CjkCodepoints

$SymbolMap = Get-LvSymbolMap $SymbolDef
$UsedFa    = Get-UsedFaCodepoints $SrcDir $SymbolMap
$FaAll     = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($c in $FaExplicit) { [void]$FaAll.Add($c) }
foreach ($c in $UsedFa.Codepoints) { [void]$FaAll.Add($c) }
$FaList    = (@($FaAll) | Sort-Object)
$FaRanges  = ($FaList | ForEach-Object { "$_" }) -join ','

Write-Host ''
Write-Host ("[scan] UI source dir      : {0}" -f $SrcDir)
Write-Host ("[scan] asset dir          : {0}" -f $AssetDir)
Write-Host ("[scan] CJK from source    : {0}" -f (@(Get-CjkCodepoints $SrcDir).Count))
Write-Host ("[scan] CJK added by assets: {0}" -f $assetCount)
Write-Host ("[scan] CJK added by margin: {0}" -f $extraCount)
Write-Host ("[scan] CJK codepoints     : {0}" -f $CjkCodepoints.Count)
Write-Host ("[scan] CJK characters     : {0}" -f $CjkChars)
Write-Host ("[scan] FontAwesome used   : {0}" -f (($UsedFa.Names | Sort-Object) -join ', '))
Write-Host ("[scan] FontAwesome ranges : {0}" -f $FaRanges)
Write-Host ''

# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------
foreach ($size in $Sizes) {
    $out = Join-Path $OutDir ("zhaoxi_font_{0}.c" -f $size)
    $name = "zhaoxi_font_$size"

    $argv = @(
        $LvFontConv,
        '--font', $CjkFont,
        '--range', '0x20-0x7F',
        '--symbols', $CjkChars,
        '--font', $FaWoff,
        '-r', $FaRanges,
        '--size', "$size",
        '--bpp', '4',
        '--format', 'lvgl',
        '-o', $out,
        '--lv-font-name', $name,
        '--no-compress'
    )

    Write-Host ("[gen] {0} (size {1}) -> {2}" -f $name, $size, $out)
    & node @argv
    if ($LASTEXITCODE -ne 0) {
        throw "lv_font_conv failed for size $size (exit $LASTEXITCODE)"
    }

    $info = Get-Item -LiteralPath $out
    if ($info.Length -le 50KB) {
        throw "Generated $out is unexpectedly small ($($info.Length) bytes)"
    }
    Write-Host ("[ok ] {0} = {1} bytes" -f $info.Name, $info.Length)
}

Write-Host ''
Write-Host 'Done. All CJK subset fonts regenerated.'
Write-Host ("Summary: {0} CJK codepoints, {1} FontAwesome codepoints, sizes {2}" -f `
    $CjkCodepoints.Count, $FaList.Count, ($Sizes -join '/'))
