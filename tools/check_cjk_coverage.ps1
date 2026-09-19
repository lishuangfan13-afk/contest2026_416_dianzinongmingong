#Requires -Version 5.1
<#
  check_cjk_coverage.ps1 - Standalone verifier for the generated zhaoxi fonts.

  Re-extracts every character the UI sources need and confirms each generated
  zhaoxi_font_<N>.c actually contains it. The check parses the real cmap data
  structures (not the human-readable "Opts:" comment, which is ANSI-mangled in
  these files and cannot be trusted).

  Usage:
      powershell -ExecutionPolicy Bypass -File tools\check_cjk_coverage.ps1

  Exits 0 and prints PASS when every font covers every required character,
  otherwise exits 1 and prints FAIL plus the missing list.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$SrcDir    = Join-Path $RepoRoot 'app\zhaoxi_ui\src'
$SymbolDef = Join-Path $RepoRoot '..\apps\graphics\lvgl\lvgl\src\font\lv_symbol_def.h'

$Sizes = @(20, 22, 24, 28)
$AsciiStart = 0x20
# lv_font_conv's --range 0x20-0x7F yields range_length 95, i.e. U+0020..U+007E.
# U+007F (DEL) is a non-printing control code and is intentionally excluded.
$AsciiEnd   = 0x7E

# FontAwesome codepoints that must always be present (matches the generator).
$FaExplicit = @(61451, 61452, 61459, 61461, 61561, 61683, 61931)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Read-Utf8Text([string]$Path) {
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

function Test-IsWantedCodepoint([int]$c) {
    return (($c -ge 0x4E00 -and $c -le 0x9FFF) -or
            ($c -ge 0x3000 -and $c -le 0x303F) -or
            ($c -ge 0xFF00 -and $c -le 0xFFEF) -or
            ($c -ge 0x2018 -and $c -le 0x201D))
}

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
    return @(@($set) | Sort-Object)
}

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

function Get-UsedFaCodepoints([string]$Dir, $SymbolMap) {
    $used = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($f in Get-SourceFiles $Dir) {
        $text = Read-Utf8Text $f.FullName
        foreach ($m in [regex]::Matches($text, 'LV_SYMBOL_([A-Za-z0-9_]+)')) {
            $name = 'LV_SYMBOL_' + $m.Groups[1].Value
            if (-not $SymbolMap.ContainsKey($name)) {
                throw "Unmapped LV_SYMBOL used in $($f.Name): $name"
            }
            [void]$used.Add([int]$SymbolMap[$name])
        }
    }
    return $used
}

# Parse the actual cmap tables out of a generated font and return the set of
# codepoints it covers.
function Get-FontCodepoints([string]$Path) {
    $text = Read-Utf8Text $Path
    $covered = New-Object 'System.Collections.Generic.HashSet[int]'

    $patternParts = @(
        '\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)\s*,'
        '\s*\.glyph_id_start\s*=\s*(\d+)\s*,\s*'
        '\.unicode_list\s*=\s*(\w+)\s*,\s*'
        '\.glyph_id_ofs_list\s*=\s*(\w+)\s*,\s*'
        '\.list_length\s*=\s*(\d+)\s*,\s*'
        '\.type\s*=\s*(LV_FONT_FMT_TXT_CMAP_\w+)'
    )
    $cmapRe = New-Object 'Text.RegularExpressions.Regex' (($patternParts -join ''), [Text.RegularExpressions.RegexOptions]::Singleline)

    $matches = $cmapRe.Matches($text)
    if ($matches.Count -eq 0) {
        throw "No cmap entries found in $Path - is this a generated lv_font_conv file?"
    }

    foreach ($m in $matches) {
        $rangeStart  = [int]$m.Groups[1].Value
        $rangeLength = [int]$m.Groups[2].Value
        $listVar     = $m.Groups[4].Value
        $listLength  = [int]$m.Groups[6].Value
        $type        = $m.Groups[7].Value

        if ($type -like '*SPARSE*') {
            $listRe = [regex]("static\s+const\s+uint16_t\s+" + [regex]::Escape($listVar) +
                              "\s*\[\s*\]\s*=\s*\{([^}]*)\}")
            $lm = $listRe.Match($text)
            if (-not $lm.Success) {
                throw "Could not find unicode list '$listVar' in $Path"
            }
            $offsets = [regex]::Matches($lm.Groups[1].Value, '0x([0-9A-Fa-f]+)')
            if ($offsets.Count -ne $listLength) {
                Write-Warning ("{0}: list_length={1} but found {2} entries in {3}" -f `
                    (Split-Path -Leaf $Path), $listLength, $offsets.Count, $listVar)
            }
            foreach ($om in $offsets) {
                [void]$covered.Add($rangeStart + [Convert]::ToInt32($om.Groups[1].Value, 16))
            }
        }
        else {
            for ($c = $rangeStart; $c -lt ($rangeStart + $rangeLength); $c++) {
                [void]$covered.Add($c)
            }
        }
    }
    return $covered
}

function Format-CpList($Codepoints, [int]$Max = 40) {
    $arr = @($Codepoints | Sort-Object)
    $shown = @($arr | Select-Object -First $Max | ForEach-Object { '0x{0:X4}' -f $_ })
    $s = $shown -join ' '
    if ($arr.Count -gt $Max) { $s += (" ... (+{0} more)" -f ($arr.Count - $Max)) }
    return $s
}

# ---------------------------------------------------------------------------
# Gather requirements
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $SrcDir))    { throw "UI source dir not found: $SrcDir" }
if (-not (Test-Path -LiteralPath $SymbolDef)) { throw "lv_symbol_def.h not found: $SymbolDef" }

$requiredCjk = Get-CjkCodepoints $SrcDir
if ($requiredCjk.Count -eq 0) {
    throw "No CJK characters extracted from $SrcDir - nothing to verify."
}

$SymbolMap = Get-LvSymbolMap $SymbolDef
$usedFa    = Get-UsedFaCodepoints $SrcDir $SymbolMap
$faAll     = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($c in $FaExplicit) { [void]$faAll.Add($c) }
foreach ($c in $usedFa)     { [void]$faAll.Add($c) }
$requiredFa = @(@($faAll) | Sort-Object)

Write-Host ''
Write-Host '=== zhaoxi CJK font coverage check ==='
Write-Host ("UI source dir       : {0}" -f $SrcDir)
Write-Host ("Required CJK chars  : {0}" -f $requiredCjk.Count)
Write-Host ("Required FontAwesome: {0}" -f (($requiredFa | ForEach-Object { '0x{0:X4}' -f $_ }) -join ' '))
Write-Host ''

$anyMissing = $false
$rows = @()

foreach ($size in $Sizes) {
    $fontPath = Join-Path $SrcDir ("zhaoxi_font_{0}.c" -f $size)
    if (-not (Test-Path -LiteralPath $fontPath)) {
        Write-Host ("FAIL  zhaoxi_font_{0}.c : file missing ({1})" -f $size, $fontPath)
        $anyMissing = $true
        continue
    }

    $covered = Get-FontCodepoints $fontPath

    $missingCjk = @($requiredCjk | Where-Object { -not $covered.Contains($_) })
    $missingFa  = @($requiredFa  | Where-Object { -not $covered.Contains($_) })
    $missingAscii = @()
    for ($c = $AsciiStart; $c -le $AsciiEnd; $c++) {
        if (-not $covered.Contains($c)) { $missingAscii += $c }
    }

    $status = if ($missingCjk.Count -eq 0 -and $missingFa.Count -eq 0 -and $missingAscii.Count -eq 0) { 'PASS' } else { 'FAIL' }
    if ($status -eq 'FAIL') { $anyMissing = $true }

    $rows += [pscustomobject]@{
        Font        = "zhaoxi_font_$size.c"
        Status      = $status
        TotalCps    = $covered.Count
        RequiredCjk = $requiredCjk.Count
        CoveredCjk  = $requiredCjk.Count - $missingCjk.Count
        MissingCjk  = $missingCjk.Count
        Ascii       = if ($missingAscii.Count -eq 0) { 'OK' } else { "MISS $($missingAscii.Count)" }
        Fa          = if ($missingFa.Count -eq 0) { 'OK' } else { "MISS $($missingFa.Count)" }
    }

    if ($missingCjk.Count -gt 0) {
        Write-Host ("MISSING CJK in zhaoxi_font_{0}.c : {1}" -f $size, (Format-CpList $missingCjk))
    }
    if ($missingFa.Count -gt 0) {
        Write-Host ("MISSING FontAwesome in zhaoxi_font_{0}.c : {1}" -f $size, (Format-CpList $missingFa))
    }
    if ($missingAscii.Count -gt 0) {
        Write-Host ("MISSING ASCII in zhaoxi_font_{0}.c : {1}" -f $size, (Format-CpList $missingAscii))
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host

if ($anyMissing) {
    Write-Host 'RESULT: FAIL - at least one generated font is missing required characters.'
    exit 1
}

Write-Host 'RESULT: PASS - all generated fonts cover every required character.'
exit 0
