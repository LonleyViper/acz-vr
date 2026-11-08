# Fixture tests for tools/live/l1.ps1 (install/remove transactionality and ownership). NEVER touches the real game install:
# every case uses a temporary -GameDir and a temporary LOCALAPPDATA. Exits non-zero if ANY assertion is false
# (D-248, Astra issue 29 finding 8). The installer under test is the sibling of THIS script, not a hardcoded checkout.
$ErrorActionPreference = 'Stop'
$l1 = Join-Path $PSScriptRoot 'l1.ps1'
if (-not (Test-Path -LiteralPath $l1)) { Write-Error "installer not found: $l1"; exit 2 }
$t = Join-Path $env:TEMP ('l1fx-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$game = Join-Path $t 'game'; $la = Join-Path $t 'localappdata'; $cfg = Join-Path $la 'acz-vr'
New-Item -ItemType Directory -Force $game, $cfg | Out-Null
$realLocal = $env:LOCALAPPDATA
$env:LOCALAPPDATA = $la
$dxgi = Join-Path $t 'dxgi.dll'; $d3d = Join-Path $t 'd3d12.dll'
Set-Content $dxgi 'dxgi-fixture' -Encoding ascii; Set-Content $d3d 'd3d12-fixture' -Encoding ascii
$startTime = Get-Date
$results = [ordered]@{}
function Run([string[]]$a) {
    $out = & pwsh -NoProfile -File $l1 @a 2>&1 | Out-String
    $out = ($out -replace '\[[0-9;]*m', '') -replace '\s+', ' '   # strip ANSI and console line wrapping before matching
    [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}
function Expect([string]$name, [bool]$cond) { $script:results[$name] = $cond }
function G([string]$f) { Join-Path $game $f }
$marker = Join-Path $cfg 'install-marker.json'
$cfgFile = Join-Path $cfg 'runtime.cfg'
try {
    # 1 happy path
    Set-Content $cfgFile 'old_config=1' -Encoding ascii
    $r = Run @('-Action','install','-Proxy',$dxgi,'-D3d12Proxy',$d3d,'-GameDir',$game)
    Expect '1-install-ok' (($r.Code -eq 0) -and (Test-Path (G 'dxgi.dll')) -and (Test-Path (G 'd3d12.dll')) -and (Test-Path $marker) -and ((Get-Content $cfgFile -Raw).Trim() -eq 'log_enabled=true'))
    # 2 re-install refused
    $r = Run @('-Action','install','-Proxy',$dxgi,'-GameDir',$game)
    Expect '2-reinstall-refused' (($r.Code -ne 0) -and ($r.Out -match 'already exists|marker already'))
    # 3 replaced DLL: remove refuses and deletes nothing
    Set-Content (G 'd3d12.dll') 'REPLACED' -Encoding ascii
    $r = Run @('-Action','remove','-GameDir',$game)
    Expect '3-mismatch-refused' (($r.Code -ne 0) -and ($r.Out -match 'refusing to delete') -and (Test-Path (G 'd3d12.dll')) -and (Test-Path (G 'dxgi.dll')))
    # 4 -ForceRemove recovers
    $r = Run @('-Action','remove','-GameDir',$game,'-ForceRemove')
    Expect '4-force-remove' (($r.Code -eq 0) -and -not (Test-Path (G 'd3d12.dll')) -and -not (Test-Path (G 'dxgi.dll')) -and -not (Test-Path $marker))
    # 5 validate-before-mutate
    Set-Content $cfgFile 'keep_me=1' -Encoding ascii
    $r = Run @('-Action','install','-Proxy',$dxgi,'-D3d12Proxy',(Join-Path $t 'missing.dll'),'-GameDir',$game)
    Expect '5-validate-before-mutate' (($r.Code -ne 0) -and -not (Test-Path (G 'dxgi.dll')) -and ((Get-Content $cfgFile -Raw).Trim() -eq 'keep_me=1') -and -not (Test-Path $marker))
    # 6 foreign dxgi.dll with no marker: remove refuses
    Set-Content (G 'dxgi.dll') 'foreign' -Encoding ascii
    $r = Run @('-Action','remove','-GameDir',$game)
    Expect '6-foreign-refused' (($r.Code -ne 0) -and ($r.Out -match 'refusing to delete') -and (Test-Path (G 'dxgi.dll')))
    Remove-Item (G 'dxgi.dll') -Force
    # 7 failure after dxgi creation: rolled back, verified, marker removed, safe config
    Set-Content $cfgFile 'experiment=1' -Encoding ascii
    $r = Run @('-Action','install','-Proxy',$dxgi,'-D3d12Proxy',$d3d,'-GameDir',$game,'-InjectFailure','after-dxgi-create')
    Expect '7-rollback-after-dxgi' (($r.Code -ne 0) -and ($r.Out -match 'rolled back') -and -not (Test-Path (G 'dxgi.dll')) -and -not (Test-Path (G 'd3d12.dll')) -and -not (Test-Path $marker) -and ((Get-Content $cfgFile -Raw).Trim() -eq 'log_enabled=true'))
    # 8 failure after d3d12 creation (partial second destination): both removed
    $r = Run @('-Action','install','-Proxy',$dxgi,'-D3d12Proxy',$d3d,'-GameDir',$game,'-InjectFailure','after-d3d12-create')
    Expect '8-rollback-after-d3d12' (($r.Code -ne 0) -and ($r.Out -match 'rolled back') -and -not (Test-Path (G 'dxgi.dll')) -and -not (Test-Path (G 'd3d12.dll')) -and -not (Test-Path $marker))
    # 9 cleanup failure: explicit INCOMPLETE result, marker retained, file remains
    $r = Run @('-Action','install','-Proxy',$dxgi,'-D3d12Proxy',$d3d,'-GameDir',$game,'-InjectFailure','cleanup')
    Expect '9-incomplete-rollback-reported' (($r.Code -ne 0) -and ($r.Out -match 'INCOMPLETE') -and (Test-Path (G 'dxgi.dll')) -and (Test-Path $marker))
    # 10 recovery: a normal remove cleans the retained state
    $r = Run @('-Action','remove','-GameDir',$game)
    Expect '10-recover-after-incomplete' (($r.Code -eq 0) -and -not (Test-Path (G 'dxgi.dll')) -and -not (Test-Path $marker))
    # 11 marker bound to another game directory is refused
    $r = Run @('-Action','install','-Proxy',$dxgi,'-GameDir',$game)
    $other = Join-Path $t 'othergame'; New-Item -ItemType Directory -Force $other | Out-Null
    $r2 = Run @('-Action','remove','-GameDir',$other)
    Expect '11-marker-bound-to-gamedir' (($r.Code -eq 0) -and ($r2.Code -ne 0) -and ($r2.Out -match 'selected game\W+directory') -and (Test-Path (G 'dxgi.dll')))
    $null = Run @('-Action','remove','-GameDir',$game)
    # 12 -InjectFailure without -GameDir is refused (never usable against the real install)
    $r = Run @('-Action','install','-Proxy',$dxgi,'-InjectFailure','after-dxgi-create')
    Expect '12-inject-requires-gamedir' (($r.Code -ne 0) -and ($r.Out -match 'fixture-only'))
} finally {
    $env:LOCALAPPDATA = $realLocal
    # `remove` creates an (empty, in fixture runs) timestamped L1-logs folder in the repo: delete the empty ones created by this run.
    $logRoot = Join-Path $PSScriptRoot '..\..\research\experiments\L1-logs'
    if (Test-Path -LiteralPath $logRoot) { Get-ChildItem -LiteralPath $logRoot -Directory | Where-Object { $_.CreationTime -ge $startTime -and -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1) } | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force } }
    Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue
}
$failed = @()
foreach ($k in $results.Keys) { '{0} = {1}' -f $k, $results[$k]; if (-not $results[$k]) { $failed += $k } }
if ($failed.Count) { Write-Error ('FIXTURE FAILURES: ' + ($failed -join ', ')); exit 1 }
"ALL $($results.Count) FIXTURE CASES PASSED"
exit 0
