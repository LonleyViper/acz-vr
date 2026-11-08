param([ValidateSet('on','off')][string]$Mode,[string]$E,[switch]$NoDashboard,[ValidateSet('null','acz_testhmd')][string]$Driver='null')   # acz_testhmd: the turnable test HMD (docs/world-render/test-hmd-driver.md)
# D-279 null SteamVR: 'on' backs up steamvr.vrsettings to $E and adds forcedDriver=null + the driver_null block (D-207 values);
# 'off' stops vr* processes and restores the byte-identical backup (sha checked). SteamVR is left STOPPED.
$ErrorActionPreference='Stop'
$cfg='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$bak=Join-Path $E 'steamvr.vrsettings.pre-run-backup'
function StopVr { Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup,fpsVR -ea 0 | Stop-Process -Force -ea 0; Start-Sleep 3 }
if($Mode -eq 'on'){
  if(Test-Path $bak){ throw "backup exists already: $bak (restore first)" }
  StopVr
  Copy-Item -LiteralPath $cfg -Destination $bak
  (Get-FileHash $bak).Hash | Set-Content (Join-Path $E 'vrsettings-prerun.sha')
  $j=Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json
  if($j.steamvr.forcedDriver){ throw 'forcedDriver already set: not a clean pre-run file' }
  $j.steamvr | Add-Member -NotePropertyName forcedDriver -NotePropertyValue $Driver
  # 2026-10-29 night: a full-screen 'VR View' (showMirrorView) covered the null 'Headset Window': its presents were throttled to ~32 Hz
  # and PrintWindow returned the game image instead of the eyes. Null runs never need the VR View; the pre-run file is restored after.
  $j.steamvr | Add-Member -NotePropertyName showMirrorView -NotePropertyValue $false -Force
  $blk=[pscustomobject][ordered]@{displayFrequency=90;enable=$true;modelNumber=$(if($Driver -eq 'null'){'Null Model'}else{'ACZ Test HMD'});renderHeight=1680;renderWidth=1512;secondsFromVsyncToPhotons=0.011;serialNumber=$(if($Driver -eq 'null'){'Null Serial'}else{'ACZ-TESTHMD-0001'});windowHeight=720;windowWidth=1280;windowX=0;windowY=0}
  $j | Add-Member -NotePropertyName ("driver_$Driver") -NotePropertyValue $blk
  # testhmd-first-flights.md section 2: on the test HMD a translucent vertical plane with a red edge line appeared in ONE eye during 90 deg head turns,
  # composited by SteamVR (absent from our eye images, present with no quad layer submitted): most likely the chaperone (collision bounds) near the
  # driver's fixed head position. Diagnostic for the next test-HMD flights: hide the bounds for the run (restored with the byte-identical backup).
  if($Driver -eq 'acz_testhmd'){ $cb=[pscustomobject][ordered]@{CollisionBoundsColorGammaA=0;CollisionBoundsFadeDistance=0.0;CollisionBoundsGroundPerimeterOn=$false;CollisionBoundsCenterMarkerOn=$false;CollisionBoundsPlaySpaceOn=$false}
    if($j.collisionBounds){ foreach($p in $cb.PSObject.Properties){ $j.collisionBounds | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value -Force } } else { $j | Add-Member -NotePropertyName collisionBounds -NotePropertyValue $cb } }
  if($NoDashboard){ if($j.dashboard){ $j.dashboard | Add-Member -NotePropertyName enableDashboard -NotePropertyValue $false -Force } else { $j | Add-Member -NotePropertyName dashboard -NotePropertyValue ([pscustomobject]@{enableDashboard=$false}) } }
  $j | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $cfg -Encoding utf8NoBOM
  "$Driver on; backup sha $((Get-FileHash $bak).Hash.Substring(0,8))"
} else {
  StopVr
  Copy-Item -LiteralPath $bak -Destination $cfg -Force
  $a=(Get-FileHash $cfg).Hash; $b=(Get-FileHash $bak).Hash
  "null off; restored sha $($a.Substring(0,8)) backup $($b.Substring(0,8)) equal=$($a -eq $b)"
}
