# L1 desktop load smoke helper (docs/proposals/live-runtime-campaign.md).
# Run ONLY with written user approval for gates 1, 3 and 6. It never launches
# the game: the user launches and plays (locally or over Steam Remote Play).
#   install : check the baseline, copy the proxy into the game directory, write the diagnostics config
#             (-D3d12Proxy <path>: also copy the optional J-3b4 forwarding d3d12.dll proxy)
#   remove  : delete the proxy (and any d3d12.dll proxy), check the baseline, collect logs into research/experiments/L1-logs
#   status  : show baseline state, proxy presence and log files
param([Parameter(Mandatory)][ValidateSet('install','remove','status')][string]$Action,
      [string]$Proxy = (Join-Path $PSScriptRoot '..\..\build\bin\Release\dxgi.dll'),
      [string]$D3d12Proxy,
      [string]$GameDir,       # TEST FIXTURES ONLY (never the real install): overrides the game dir and skips the baseline check
      [switch]$ForceRemove,
      [ValidateSet('','after-dxgi-create','after-d3d12-create','cleanup')][string]$InjectFailure = '')  # FIXTURE-ONLY failure injection (requires -GameDir)   # explicit recovery: remove proxy files whose hashes do not match the install marker
$ErrorActionPreference = 'Stop'
$game = if ($GameDir) { $GameDir } else { 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero' }
$target = Join-Path $game 'dxgi.dll'
$d3d12Target = Join-Path $game 'd3d12.dll'
$cfgDir = Join-Path $env:LOCALAPPDATA 'acz-vr'
$d3d12Marker = Join-Path $cfgDir 'l1-d3d12-proxy.sha256'
$installMarker = Join-Path $cfgDir 'install-marker.json'   # D-242: ownership record for BOTH proxy files
$baseline = @{
  'acz.exe'='8aa8ffc9a185c58d45b34bc1116d8e1880ea785cc6fa3fa7d6a421fffeb9c739'
  'a.arc'='82c05ed08ca584b551776f916ba06e9bfd21c01e0cfede8afe6ffbd276745f80'
  'b.arc'='4c4a97f5783cc925002d642306d3b25de2fa87e4e10a4f5a82e0cc96ec00d7ff'
  'c.arc'='a84efdbba33046105134111197d1aa7babecb16dc3f15f158f7689dd25299bce'
  'd.arc'='c28e41acdc6ba6699481169e168381234b920191f0189384fca751c04538d0e2'
  'e.arc'='63d8f45fc98cc5fbeca71de8e98727f8662a1e7d6aeb291b305d5503bc6de3d5'
  'steam_api64.dll'='e3fe06e2d802c2f753aeb46869e2af898f34715ce85f08001456fd8a80ecd21e'
}
function Test-Baseline {
  if ($GameDir) { return 'Baseline check skipped (fixture GameDir).' }
  $bad = @(); foreach ($f in $baseline.Keys) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $game $f)).Hash.ToLower()
    if ($h -ne $baseline[$f]) { $bad += "$f $h" } }
  if ($bad) { throw "Baseline mismatch: $($bad -join '; ')" }
  'Baseline OK (7 files).'
}
switch ($Action) {
  'install' {
    # D-242 (Astra, issue 29 findings 6-7): transactional install with an ownership marker.
    # Astra r3 F7: refuse fixture-only arguments BEFORE any real-game access (process check, baseline hashing).
    if ($InjectFailure -and -not $GameDir) { throw '-InjectFailure is fixture-only and requires -GameDir.' }
    if (Get-Process acz -ErrorAction SilentlyContinue) { throw 'acz.exe is running; close the game first.' }
    Test-Baseline | Out-Null
    # 1. Validate everything BEFORE mutating anything.
    if (Test-Path -LiteralPath $target) { throw "A dxgi.dll already exists in the game directory; not overwriting." }
    if (Test-Path -LiteralPath $d3d12Target) { throw "A d3d12.dll already exists in the game directory; not overwriting." }
    if (Test-Path -LiteralPath $installMarker) { throw "An install marker already exists ($installMarker): run -Action remove first." }
    if (-not (Test-Path -LiteralPath $Proxy)) { throw "dxgi proxy not found: $Proxy" }
    if ($D3d12Proxy -and -not (Test-Path -LiteralPath $D3d12Proxy)) { throw "d3d12 proxy not found: $D3d12Proxy" }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Proxy).Hash.ToLower()
    $d3d12Hash = if ($D3d12Proxy) { (Get-FileHash -Algorithm SHA256 -LiteralPath $D3d12Proxy).Hash.ToLower() } else { $null }
    $cfgPath = Join-Path $cfgDir 'runtime.cfg'
    $cfgBackup = Join-Path $cfgDir 'runtime.cfg.pre-install'
    $attempted = @(); $cfgSaved = $false; $hadCfg = $false
    try {
      New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
      # 2. Record intent first (a crash mid-install leaves a marker that remove can use).
      $marker = [ordered]@{ gameDir = $game; dxgi = [ordered]@{ path = $target; sha256 = $hash }; d3d12 = $null; time = (Get-Date -Format o) }
      if ($D3d12Proxy) { $marker.d3d12 = [ordered]@{ path = $d3d12Target; sha256 = $d3d12Hash } }
      ($marker | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $installMarker -Encoding ascii
      # 3. Back up the previous config, then copy and verify each binary. A destination counts as ATTEMPTED
      #    BEFORE the copy so a partially created file is cleaned up too.
      if (Test-Path -LiteralPath $cfgPath) { Copy-Item -LiteralPath $cfgPath -Destination $cfgBackup -Force; $hadCfg = $true }
      $cfgSaved = $true
      $attempted += $target
      Copy-Item -LiteralPath $Proxy -Destination $target
      if ($InjectFailure -in 'after-dxgi-create','cleanup') { throw 'injected failure after dxgi.dll creation' }
      if ((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash.ToLower() -ne $hash) { throw 'installed dxgi.dll hash mismatch' }
      if ($D3d12Proxy) {
        $attempted += $d3d12Target
        Copy-Item -LiteralPath $D3d12Proxy -Destination $d3d12Target
        if ($InjectFailure -eq 'after-d3d12-create') { throw 'injected failure after d3d12.dll creation' }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $d3d12Target).Hash.ToLower() -ne $d3d12Hash) { throw 'installed d3d12.dll hash mismatch' }
      }
      # 4. Only now write the safe config.
      Set-Content -LiteralPath $cfgPath -Value 'log_enabled=true' -Encoding ascii
    } catch {
      $err = $_
      # D-248 (Astra issue 29 finding 7): roll back EVERY mutation, VERIFY the result, and never claim success or
      # delete the recovery record while anything remains. The safe config is written instead of restoring an old
      # (possibly experiment-enabled) one.
      $leftover = @()
      foreach ($f in $attempted) {
        if (Test-Path -LiteralPath $f) {
          try {
            if ($InjectFailure -eq 'cleanup') { throw 'injected cleanup failure' }
            Remove-Item -LiteralPath $f -Force -ErrorAction Stop
          } catch { }
          if (Test-Path -LiteralPath $f) { $leftover += $f }
        }
      }
      if ($cfgSaved) {
        try { Set-Content -LiteralPath $cfgPath -Value 'log_enabled=true' -Encoding ascii -ErrorAction Stop } catch { $leftover += "$cfgPath (could not write the safe config)" }
      }
      if ($leftover.Count -gt 0) {
        # Keep the marker: it is the recovery record for `-Action remove`.
        throw "install FAILED and the rollback is INCOMPLETE; remaining: $($leftover -join '; '). The install marker is retained ($installMarker): resolve the locked/remaining file(s), then run -Action remove. Original error: $err"
      }
      if (Test-Path -LiteralPath $installMarker) { Remove-Item -LiteralPath $installMarker -Force }
      throw "install failed and was rolled back (verified: no proxy files remain): $err"
    }
    if ($D3d12Proxy) { "Installed d3d12 proxy sha256 $d3d12Hash (forwarding; acts only with gpu_copy_early_device_hook=true and the step-1 opt-ins)." }
    "Installed proxy sha256 $hash. Capture stays disabled; the config can only enable logging."
  }
  'remove' {
    if (Get-Process acz -ErrorAction SilentlyContinue) { throw 'acz.exe is running; close the game first.' }
    # D-242: prove ownership BEFORE deleting anything.
    $m = $null
    if (Test-Path -LiteralPath $installMarker) { $m = Get-Content -LiteralPath $installMarker -Raw | ConvertFrom-Json }
    if ($m -and $m.gameDir -and ([string]$m.gameDir -ne $game) -and -not $ForceRemove) {
      throw "The install marker was written for '$($m.gameDir)' but the selected game directory is '$game'; refusing (re-run with the same -GameDir, or -ForceRemove)."
    }
    foreach ($k in 'dxgi','d3d12') {
      if ($m -and $m.$k -and ([System.IO.Path]::GetDirectoryName([string]$m.$k.path) -ne $game) -and -not $ForceRemove) {
        throw "The marker's recorded $k path '$($m.$k.path)' is outside the selected game directory '$game'; refusing."
      }
    }
    $plan = @()
    foreach ($e in @(@{ n = 'dxgi.dll'; p = $target; k = 'dxgi' }, @{ n = 'd3d12.dll'; p = $d3d12Target; k = 'd3d12' })) {
      if (-not (Test-Path -LiteralPath $e.p)) { continue }
      $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $e.p).Hash.ToLower()
      $recorded = if ($m -and $m.($e.k)) { [string]$m.($e.k).sha256 } else { '' }
      if ($recorded -and $h -eq $recorded) { $plan += $e.p }
      elseif ($ForceRemove) { Write-Warning "$($e.n): hash $h does not match the install marker ($recorded); removing because -ForceRemove was given."; $plan += $e.p }
      else { throw "$($e.n) in the game directory does not match the install marker (found $h, marker '$recorded'); refusing to delete. Inspect it, or re-run with -ForceRemove." }
    }
    foreach ($f in $plan) { Remove-Item -LiteralPath $f; "$([System.IO.Path]::GetFileName($f)) removed (ownership verified)." }
    if (-not $plan) { 'No proxy files present.' }
    if (Test-Path -LiteralPath $installMarker) { Remove-Item -LiteralPath $installMarker }
    if (Test-Path -LiteralPath $d3d12Marker) { Remove-Item -LiteralPath $d3d12Marker }   # legacy marker from earlier installs
    if (Test-Path -LiteralPath $target) { throw 'dxgi.dll still present in the game directory after remove.' }
    if (Test-Path -LiteralPath $d3d12Target) { throw 'd3d12.dll still present in the game directory after remove.' }
    Test-Baseline
    'No dxgi.dll or d3d12.dll remains in the game directory.'
    $out = Join-Path $PSScriptRoot "..\..\research\experiments\L1-logs\$(Get-Date -Format yyyyMMdd-HHmmss)"
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $logs = Join-Path $cfgDir 'logs'
    if (Test-Path $logs) { Copy-Item -Path (Join-Path $logs '*') -Destination $out -Recurse; "Logs copied to $out" } else { 'No logs were written.' }
  }
  'status' {
    Test-Baseline
    "Proxy present: $(Test-Path -LiteralPath $target)"
    "d3d12 proxy present: $(Test-Path -LiteralPath $d3d12Target)"
    if (Test-Path (Join-Path $cfgDir 'logs')) { Get-ChildItem (Join-Path $cfgDir 'logs') | Select-Object Name,Length,LastWriteTime | Format-Table | Out-String }
  }
}
