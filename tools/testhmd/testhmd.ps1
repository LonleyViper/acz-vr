<#
acz_testhmd helper (docs/world-render/test-hmd-driver.md).

  install   copy the built driver folder to -InstallDir; prints the vrpathreg change; registers it only with -Apply
  uninstall prints the vrpathreg removal; applies it only with -Apply
  enable    -E <run dir>: back up steamvr.vrsettings to <run dir> and force acz_testhmd (like null-vr.ps1 'on'); only with -Apply
  disable   -E <run dir>: stop SteamVR and restore the byte-identical backup (like null-vr.ps1 'off'); only with -Apply
  pose      -Yaw -Pitch -Roll [-X -Y -Z] [-Slew]: write the pose file atomically (no SteamVR change; no -Apply needed)
  status    driver folder, registration, vrsettings keys, pose file, last log lines

Without -Apply nothing outside the driver folder / pose file is changed: the commands print what they WOULD do.
Sign convention (SteamVR): +yaw = head turns LEFT, +pitch = looks UP, +roll = tilts LEFT; degrees.
#>
param(
  [Parameter(Mandatory, Position = 0)][ValidateSet('install', 'uninstall', 'enable', 'disable', 'pose', 'status')][string]$Command,
  [switch]$Apply,
  [string]$E,                                                       # enable/disable: run evidence dir that holds the vrsettings backup
  [string]$BuiltDir = 'E:\acz-testhmd\driver\acz_testhmd',
  [string]$InstallDir = 'E:\acz-testhmd\installed\acz_testhmd',
  [double]$Yaw = 0, [double]$Pitch = 0, [double]$Roll = 0,
  [Nullable[double]]$X, [Nullable[double]]$Y, [Nullable[double]]$Z,
  [Nullable[double]]$Slew,
  [int]$WindowWidth = 1280, [int]$WindowHeight = 720,               # same as null-vr.ps1's driver_null block (the runs' null window)
  [switch]$NoDashboard
)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$steamvr = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'
$vrpathreg = Join-Path $steamvr 'bin\win64\vrpathreg.exe'
$vrset = 'C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$vrpaths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
$aczDir = Join-Path $env:LOCALAPPDATA 'acz-vr'
$poseFile = Join-Path $aczDir 'testhmd-pose.txt'
$logFile = Join-Path $aczDir 'testhmd.log'
function Hash8([string]$p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p).Hash.Substring(0, 8) } else { '(none)' } }
function StopVr { Get-Process vrmonitor, vrserver, vrcompositor, vrdashboard, vrwebhelper, vrstartup, fpsVR -ea 0 | Stop-Process -Force -ea 0; Start-Sleep 3 }
function Would([string]$s) { Write-Host "WOULD: $s" }

switch ($Command) {
  'install' {
    if (-not (Test-Path (Join-Path $BuiltDir 'bin\win64\driver_acz_testhmd.dll'))) { throw "no built driver in $BuiltDir (build tools/testhmd first)" }
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    Copy-Item -Path (Join-Path $BuiltDir '*') -Destination $InstallDir -Recurse -Force
    Write-Host "copied $BuiltDir -> $InstallDir (dll $(Hash8 (Join-Path $InstallDir 'bin\win64\driver_acz_testhmd.dll')))"
    $cmd = "`"$vrpathreg`" adddriver `"$InstallDir`""
    if ($Apply) {
      if (Get-Process vrserver -ea 0) { throw 'SteamVR is running: close it before registering a driver' }
      & $vrpathreg adddriver $InstallDir; Write-Host "registered (exit $LASTEXITCODE); external_drivers now:"; & $vrpathreg show
    } else {
      Would $cmd
      Would "-> adds `"$InstallDir`" to external_drivers in $vrpaths (current hash $(Hash8 $vrpaths)); the driver stays disabled until 'enable' (default.vrsettings enable=false)"
    }
  }
  'uninstall' {
    $cmd = "`"$vrpathreg`" removedriver `"$InstallDir`""
    if ($Apply) { & $vrpathreg removedriver $InstallDir; Write-Host "removed (exit $LASTEXITCODE)" } else { Would $cmd }
  }
  'enable' {
    if (-not $E) { throw '-E <run dir> is required (the vrsettings backup lives there)' }
    $bak = Join-Path $E 'steamvr.vrsettings.pre-run-backup'
    $j = Get-Content -LiteralPath $vrset -Raw | ConvertFrom-Json
    if ($j.steamvr.forcedDriver) { throw "forcedDriver already set ('$($j.steamvr.forcedDriver)'): not a clean pre-run file" }
    $block = [pscustomobject][ordered]@{ enable = $true; windowX = 0; windowY = 0; windowWidth = $WindowWidth; windowHeight = $WindowHeight; renderWidth = 1512; renderHeight = 1680; displayFrequency = 90 }
    if (-not $Apply) {
      Would "stop SteamVR processes; copy $vrset -> $bak (hash $(Hash8 $vrset))"
      Would "steamvr.forcedDriver = 'acz_testhmd'; steamvr.showMirrorView = false"
      Would ("driver_acz_testhmd = " + ($block | ConvertTo-Json -Compress))
      if ($NoDashboard) { Would 'dashboard.enableDashboard = false' }
      Would "write $vrset (UTF-8, no BOM); 'disable -E $E -Apply' restores the backup byte for byte"
      break
    }
    if (Test-Path $bak) { throw "backup exists already: $bak (disable first)" }
    New-Item -ItemType Directory -Force $E | Out-Null
    StopVr
    Copy-Item -LiteralPath $vrset -Destination $bak
    (Get-FileHash $bak).Hash | Set-Content (Join-Path $E 'vrsettings-prerun.sha')
    $j.steamvr | Add-Member -NotePropertyName forcedDriver -NotePropertyValue 'acz_testhmd'
    $j.steamvr | Add-Member -NotePropertyName showMirrorView -NotePropertyValue $false -Force
    $j | Add-Member -NotePropertyName driver_acz_testhmd -NotePropertyValue $block -Force
    if ($NoDashboard) { if ($j.dashboard) { $j.dashboard | Add-Member -NotePropertyName enableDashboard -NotePropertyValue $false -Force } else { $j | Add-Member -NotePropertyName dashboard -NotePropertyValue ([pscustomobject]@{ enableDashboard = $false }) } }
    $j | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $vrset -Encoding utf8NoBOM
    "testhmd on; backup sha $((Get-FileHash $bak).Hash.Substring(0,8))"
  }
  'disable' {
    if (-not $E) { throw '-E <run dir> is required' }
    $bak = Join-Path $E 'steamvr.vrsettings.pre-run-backup'
    if (-not (Test-Path $bak)) { throw "no backup at $bak" }
    if (-not $Apply) { Would "stop SteamVR processes; copy $bak (hash $(Hash8 $bak)) -> $vrset (now $(Hash8 $vrset))"; break }
    StopVr
    Copy-Item -LiteralPath $bak -Destination $vrset -Force
    $a = (Get-FileHash $vrset).Hash; $b = (Get-FileHash $bak).Hash
    "testhmd off; restored sha $($a.Substring(0,8)) backup $($b.Substring(0,8)) equal=$($a -eq $b)"
  }
  'pose' {
    $f = @($Yaw, $Pitch, $Roll)
    $hasPos = ($null -ne $X) -or ($null -ne $Y) -or ($null -ne $Z)
    if ($hasPos) { $f += @($(if ($null -ne $X) { $X } else { 0 }), $(if ($null -ne $Y) { $Y } else { 0 }), $(if ($null -ne $Z) { $Z } else { 0 })) }
    if ($null -ne $Slew) { $f += $Slew }
    $line = ($f | ForEach-Object { ([double]$_).ToString('0.###', $inv) }) -join ' '
    New-Item -ItemType Directory -Force $aczDir | Out-Null
    $tmp = "$poseFile.tmp"
    [IO.File]::WriteAllText($tmp, "$line`r`n", (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $poseFile -Force      # atomic replace: the driver never reads a half-written line
    "pose file: $line  ($poseFile)"
  }
  'status' {
    $dll = Join-Path $InstallDir 'bin\win64\driver_acz_testhmd.dll'
    Write-Host "built dll     $(Hash8 (Join-Path $BuiltDir 'bin\win64\driver_acz_testhmd.dll'))  ($BuiltDir)"
    Write-Host "installed dll $(Hash8 $dll)  ($InstallDir)"
    $reg = $false
    if (Test-Path $vrpaths) { try { $p = Get-Content $vrpaths -Raw | ConvertFrom-Json; $reg = @($p.external_drivers | Where-Object { $_ -and ([IO.Path]::GetFullPath($_).TrimEnd('\') -ieq [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')) }).Count -gt 0 } catch { } }
    Write-Host "registered    $reg  ($vrpaths)"
    try { $j = Get-Content -LiteralPath $vrset -Raw | ConvertFrom-Json; Write-Host "vrsettings    $(Hash8 $vrset) forcedDriver='$($j.steamvr.forcedDriver)' driver_acz_testhmd=$(if($j.driver_acz_testhmd){$j.driver_acz_testhmd | ConvertTo-Json -Compress}else{'(none)'})" } catch { Write-Host "vrsettings    unreadable: $_" }
    Write-Host "SteamVR       $(if (Get-Process vrserver -ea 0) { 'running' } else { 'not running' })"
    Write-Host "pose file     $(if (Test-Path $poseFile) { (Get-Content $poseFile -Raw).Trim() } else { '(absent: identity)' })"
    if (Test-Path $logFile) { Write-Host 'log tail:'; Get-Content $logFile -Tail 5 | ForEach-Object { "  $_" } } else { Write-Host 'log           (none yet)' }
  }
}
