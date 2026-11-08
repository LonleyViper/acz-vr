# cloudshots.ps1 (2026-10-30, effects-outside-window.md 20.8): mid-cruise eye dumps for the fxcloud cache check. Start it AFTER a batch run has started
# (e.g. in a second window right after runBatchAG.ps1 launched AGC1). It waits for the run's autopilot-1.log, then from autopilot T+$Start s writes
# %LOCALAPPDATA%\acz-vr\png-eye.now every $Every s, $Count times. The DLL (FLIGHT-DUMP, step147+) consumes the file on the next fresh flight frame and
# re-arms xr_png_once for one more capture: acz-vr-xr-eye0-f<frame>-final/alpha/worldfx.png in %LOCALAPPDATA%\acz-vr\logs\png (the batch's own
# post-run step copies logs\png and writes the upright\ copies, as for the f448 dump). Needs xr_png_once=true in the run cfg
# (Batch AG: the -Png runs AGC1/AGC2). png-screen.now does NOT work in flight (it dumps the virtual screen, menus only). The new pngs are copied into
# <RunDir>\cloudshots after each shot; xr-stereo.log shows `event=png-eye-armed` and `event=png-once;outcome=taken` per shot.
# Example: pwsh -NoProfile -File cloudshots.ps1 -RunDir D:\acz-evidence-archive\step147\batchAG-fx\runAGC1
param([Parameter(Mandatory)][string]$RunDir,   # the run's evidence dir (where the batch writes autopilot-1.log)
      [double]$Start=60, [double]$Every=20, [int]$Count=4,
      [int]$WaitLogS=900)                       # give up if no autopilot log appears within this many seconds
$ErrorActionPreference='Stop'
$dir=Join-Path $env:LOCALAPPDATA 'acz-vr'; $now=Join-Path $dir 'png-eye.now'; $out=Join-Path $RunDir 'cloudshots'; $pd=Join-Path $dir 'logs\png'
$al=Join-Path $RunDir 'autopilot-1.log'
function Log($m){ $l=('{0} {1}' -f (Get-Date -Format 'HH:mm:ss.fff'),$m); Write-Host $l; try { Add-Content (Join-Path $RunDir 'cloudshots.log') $l } catch {} }
$t0=Get-Date; while(-not (Test-Path $al)){ if(((Get-Date)-$t0).TotalSeconds -gt $WaitLogS){ Log "no autopilot log after $WaitLogS s: stop"; exit 1 }; Start-Sleep -Milliseconds 500 }
$ap=Get-Date; Log "autopilot log seen: T0 (shots at T+$Start s every $Every s, $Count shots)"
New-Item -ItemType Directory -Force $out | Out-Null
if(Test-Path $now){ Remove-Item $now -Force; Log 'stale png-eye.now removed' }
$seen=@{}; Get-ChildItem $pd -Filter 'acz-vr-xr-eye0-f*.png' -ea 0 | % { $seen[$_.Name]=$true }
for($i=0;$i -lt $Count;$i++){
  $at=$Start+$i*$Every
  while(((Get-Date)-$ap).TotalSeconds -lt $at){
    if(-not (Get-Process acz -ea 0)){ Log 'acz gone: stop'; exit 0 }
    Start-Sleep -Milliseconds 200
  }
  Set-Content $now (Get-Date -Format o); Log ("shot {0}: png-eye.now written at T+{1:N1}" -f ($i+1),((Get-Date)-$ap).TotalSeconds)
  $w=Get-Date; while((Test-Path $now) -and ((Get-Date)-$w).TotalSeconds -lt 10){ Start-Sleep -Milliseconds 200 }
  if(Test-Path $now){ Log '  not consumed within 10 s (no fresh flight frame, xr_png_once off, or a DLL without FLIGHT-DUMP): removed'; Remove-Item $now -Force; continue }
  Start-Sleep 3   # the capture is taken with the frame's completion; the writer thread encodes after it
  Get-ChildItem $pd -Filter 'acz-vr-xr-eye0-f*.png' -ea 0 | ? { -not $seen[$_.Name] } | % { $seen[$_.Name]=$true; Copy-Item $_.FullName $out; Log "  copied $($_.Name)" }
}
Log 'done'
