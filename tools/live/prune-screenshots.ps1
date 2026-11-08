# Prune raw live-run screenshots (git-ignored, large). Keeps contact sheets (*sheet.png),
# run.json/status/log files, and every PNG in the newest $KeepRuns run folders.
# Called by acz-test-harness.ps1 after each live run; safe to run by hand.
[CmdletBinding()]
param([int]$OlderThanDays = 3, [int]$KeepRuns = 5, [switch]$WhatIf)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$exp = (Resolve-Path (Join-Path $PSScriptRoot '..\..\research\experiments')).Path
$cutoff = (Get-Date).AddDays(-$OlderThanDays)
$roots = @(Join-Path $exp 'harness-runs'; Join-Path $exp 'L1b-screenshots') + @(Get-ChildItem $exp -Directory -Filter 'ui-route-keyed-*' | ForEach-Object FullName)
$keep = @(Get-ChildItem (Join-Path $exp 'harness-runs') -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First $KeepRuns | ForEach-Object FullName)
$freed = 0L; $count = 0
foreach ($root in $roots | Where-Object { Test-Path $_ }) {
    Get-ChildItem $root -Recurse -File -Filter '*.png' | Where-Object {
        $f = $_
        $f.Name -notlike '*sheet.png' -and $f.LastWriteTime -lt $cutoff -and
            -not ($keep | Where-Object { $f.FullName.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase) })
    } | ForEach-Object {
        $freed += $_.Length; $count++
        if (-not $WhatIf) { Remove-Item -LiteralPath $_.FullName }
    }
}
"{0} {1} raw screenshots ({2:N1} MB) older than {3} days; kept contact sheets, run records and the newest {4} runs." -f ($(if ($WhatIf) { 'Would prune' } else { 'Pruned' })), $count, ($freed / 1MB), $OlderThanDays, $KeepRuns
