param([string]$E,[string]$AP='-Mode Cruise -Total 120',[string]$AP2='',[string]$MissionKeys='',[int]$PostWait=0,[int]$PostTapEvery=45,[int]$PostTapMax=3,
      [switch]$HeadSweep,   # -BriefMap on the test HMD: step the head pose (yaw/pitch/lean) once per png-screen trigger so the table anchor can be compared across frames
      [switch]$BriefMap,[int]$BriefHold=30,[int]$BriefPngEvery=3,[switch]$BriefOverviewOnly,
      [switch]$Debrief,[int]$DebriefHold=30,[int]$DebriefPngEvery=3,[int]$DebriefPageHold=10,[int]$DebriefMaxPages=4,[int]$DebriefEndWait=240,[int]$DebriefWait=120,
      [switch]$DebriefQuit,[int]$DebriefQuitConfirms=0,[int]$DebriefProbeConfirm=0)
# -Debrief (2026-11-01, runBatchAG -Debrief, tag DBR1): the POST-MISSION DEBRIEF route (dbr3d:1). After the autopilot flight it (1) waits for the mission to END: the unflown / diving aircraft crashes ("Cipher's crashed!", MISSION FAILED, then the
# REPLAY screen, "PRESS MENU BUTTON TO EXIT REPLAY"; step174 runAGU1 g0066..g0115 shows exactly that and the old -PostWait taps were ENTER, which does not leave the replay), or -DebriefQuit: Esc pause menu -> QUIT MISSION (highlight row verified by image);
# (2) leaves the replay with Esc ('back' = the menu button, the key that pauses in flight), every state read from the window image (debrieflib.ps1: BLACK / REPLAY / PAUSE+row / OTHER), never blind;
# (3) waits for the debrief itself = ee-camera.log summary-dbr3d;armed=1 (the producer sees the results-panel window quad), holds -DebriefHold s with png-screen.now every -DebriefPngEvery s + ONE png-eye.now (+ census re-armed every 15 s, read-only arms),
# (4) pages: 'confirm' only while the debrief is still armed and fresh, max -DebriefMaxPages pages, -DebriefPageHold s each, stops when armed drops or a confirm changes nothing.
# No tap is ever sent on a screen the image/log checks do not identify, except the opt-in -DebriefProbeConfirm N (guarded confirms on a static unknown screen) and -DebriefQuitConfirms N (extra confirms after QUIT MISSION).
# A flight-stage-detect after the first flight (a new mission started) aborts the run (marker MISSION-STARTED).
# -BriefMap (2026-10-31, runBatchAG -BriefMap, tag AGM1): the BRIEFING MAP TABLE route. After the loading cinematic it takes ONLY 'back' taps (cinematic -> 3D overview), waits until the game's own
# map draws are seen in ee-camera.log (brief3d-capture;sel>0 lines every 1 s, summary-brief3d selmap growth every 5 s), holds -BriefHold s with png-screen.now every -BriefPngEvery s and ONE png-eye.now,
# then 'back' (-> mission menu) and, unless -BriefOverviewOnly, a second leg: down, down (each verified by a luma-grid change), confirm on BRIEFING (map presentation card), back (3D map), hold again.
# It never taps 'accelerate' (START MISSION) and never starts a flight; a flight-stage-detect line aborts the run. Not reaching the map = ROUTE-FAILED marker (the batch retries once).
# runK3ag.ps1 = runK3.ps1 (2026-10-30, Batch AG) + a POST-FLIGHT WAIT: -PostWait N keeps the game open N s after the autopilot ends (mission over / results / debrief screens).
# During it: briefing-observe.arm + brief3d-capture.arm (docs/world-render/debrief-3d-map.md section 5, read only), ui3d-census.arm re-armed every 15 s (it latches after
# 60 samples), png-screen.now every 8 s, a screenshot + 'confirm' tap every -PostTapEvery s (max -PostTapMax taps) to walk the post-mission screens. All logged in steps.txt.
# -PostWait 0 (default) is byte-for-byte the old runK3 behaviour.
# K2 (autopilot run, R4 menu route: runway, skip, skip, 90 s flight): launch, route83-noskip to mission 02 (no menu walk) with XR already running (R4 path), start mission 02, takeoff 25 s, Esc skip, V, fly without input until
# MISSION FAILED (off course), RESTART (flight 2), 60 s, close. Screenshots + step times in $E. Stops early if GAMEOVER (sampler stop rule) appears.
$X=$PSScriptRoot; $drv=Join-Path (Split-Path (Split-Path $X)) '..\tools\live\acz-window-driver.ps1'; $drv=(Resolve-Path $drv).Path
$cdrv=$drv -replace 'acz-window-driver.ps1','acz-window-driver-combat.ps1'
function L($m){ "{0} {1}" -f (Get-Date -f HH:mm:ss.fff),$m | Add-Content "$E\steps.txt" }
function Shot($n){ pwsh -NoProfile -File "$X\shot.ps1" -Out "$E\shots\$n.png" | Out-Null; L "shot $n" }
function Stop?{ if(Test-Path "$E\GAMEOVER"){ L 'GAMEOVER: stop'; exit 3 }; if(-not (Get-Process acz -ea 0)){ L 'acz gone'; exit 4 } }
# AG2 fix 2026-10-30 (AGF3, 17:08): SteamVR's 'SteamVR Room Setup' wizard (steamvr_room_setup.exe, started by the null-HMD SteamVR the batch itself launched) popped up over the game ~40 s after launch;
# the window driver then refused every tap ('ACZero did not become the foreground window', exit 1) and no flight ever started. Close that process (nothing else) before the route and on every refused tap.
function KillRoomSetup { $r=@(Get-Process steamvr_room_setup -ea 0); if($r){ $r | Stop-Process -Force -ea 0; L ("closed SteamVR Room Setup window ({0} process)" -f $r.Count); Start-Sleep -Milliseconds 700 } }
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class WF{[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);[DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int c);[DllImport("user32.dll")]public static extern bool IsIconic(IntPtr h);[DllImport("user32.dll")]public static extern bool BringWindowToTop(IntPtr h);[DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);[DllImport("user32.dll")]public static extern void keybd_event(byte k,byte s,uint f,UIntPtr e);[DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();}'
# AG2 (17:23): after the Room Setup window was closed the game was still BEHIND the Steam library / 'OpenVRStartup' console windows and SetForegroundWindow alone was refused (foreground lock): restore if minimized, ALT tap (releases the lock), topmost toggle, then foreground.
function FgGame { $g=Get-Process acz -ea 0 | ? { $_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR' -and $_.MainWindowHandle -ne 0 } | select -Last 1; if(-not $g){ return }; $h=$g.MainWindowHandle
  if([WF]::GetForegroundWindow() -eq $h){ return }
  if([WF]::IsIconic($h)){ [void][WF]::ShowWindow($h,9) }
  [WF]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [WF]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  [void][WF]::SetWindowPos($h,[IntPtr](-1),0,0,0,0,0x53); [void][WF]::SetWindowPos($h,[IntPtr](-2),0,0,0,0,0x53); [void][WF]::BringWindowToTop($h); [void][WF]::SetForegroundWindow($h); Start-Sleep -Milliseconds 400
  if([WF]::GetForegroundWindow() -ne $h){ L 'FgGame: game window still not foreground' } }
function T($k){ Stop?; FgGame; $t0=Get-Date -f HH:mm:ss.fff; $o=pwsh -NoProfile -File $drv -Action tap -Key $k -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" 2>&1; $ec=$LASTEXITCODE
  Add-Content "$E\driver.log" ("{0} tap {1} exit={2} (started {4}) {3}" -f (Get-Date -f HH:mm:ss.fff),$k,$ec,(($o | Out-String).Trim() -replace '\s+',' '),$t0); L "tap $k (driver exit $ec)"; if($ec -ne 0){ KillRoomSetup; FgGame; $o=pwsh -NoProfile -File $drv -Action tap -Key $k -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" 2>&1; $ec=$LASTEXITCODE; Add-Content "$E\driver.log" ("{0} tap {1} RETRY exit={2} {3}" -f (Get-Date -f HH:mm:ss.fff),$k,$ec,(($o | Out-String).Trim() -replace '\s+',' ')); L "tap $k retry after refusal (driver exit $ec)" } }   # AG fix 2026-10-30: the driver output was discarded; a refused tap ('ACZero did not become the foreground window') left no trace in AGF2
function W($s){ $t=[Diagnostics.Stopwatch]::StartNew(); while($t.Elapsed.TotalSeconds -lt $s){ Stop?; Start-Sleep -Milliseconds 250 } }
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class WV{[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);}'
[void][WV]::SetProcessDPIAware()
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
$script:rsJob=Start-Job { while($true){ Get-Process steamvr_room_setup -ea 0 | Stop-Process -Force -ea 0; Start-Sleep -Seconds 2 } }   # AG2: watchdog, closes the SteamVR Room Setup wizard whenever it appears (it steals the foreground from the game)
Start-Process 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\acz.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$p=$null; for($k=0;$k -lt 60 -and -not $p;$k++){ Start-Sleep -Milliseconds 500; $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1 }
if(-not $p){ L 'no launcher'; exit 1 }
Start-Sleep 4; $r=New-Object WV+RECT; [void][WV]::GetWindowRect($p.MainWindowHandle,[ref]$r); [void][WV]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 800
if([math]::Abs($r.L-880) -gt 12 -or [math]::Abs($r.T-380) -gt 12){ L "launcher rect $($r.L),$($r.T): not at 880,380 (the click loop re-checks)" }
# AG2 fix (AGF1 17:38): the launcher's 'Launch Game' click can be swallowed when the launcher is behind other windows (the first click only activated it) and the launcher stayed on screen for 40+ s
# (the launcher window has the same title as the game, so the old luma check passed). Foreground first, click, and verify that the window grew to game size (> 1500 px wide); up to 4 clicks.
function GameWin { Get-Process acz -ea 0 | ? { $_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR' -and $_.MainWindowHandle -ne 0 } | select -Last 1 }
function WinW { $g=GameWin; if(-not $g){ return 0 }; $rc=New-Object WV+RECT; [void][WV]::GetWindowRect($g.MainWindowHandle,[ref]$rc); $rc.R-$rc.L }
for($lc=1;$lc -le 4;$lc++){ KillRoomSetup; FgGame; Start-Sleep -Milliseconds 700; $r2=New-Object WV+RECT; [void][WV]::GetWindowRect($p.MainWindowHandle,[ref]$r2); if([math]::Abs($r2.L-880) -gt 12 -or [math]::Abs($r2.T-380) -gt 12){ if((WinW) -gt 1500){ break }; L "launcher rect $($r2.L),$($r2.T): not where expected (try $lc)"; Start-Sleep 5; continue }
  pwsh -NoProfile -File "$X\shot.ps1" -ClickX 1368 -ClickY 886 | Out-Null; L "clicked Launch Game (try $lc)"
  $ok=$false; for($w=0;$w -lt 14;$w++){ Start-Sleep -Milliseconds 700; if((WinW) -gt 1500){ $ok=$true; break } }; if($ok){ L 'launcher closed (game window is full size)'; break } else { L 'launcher still showing after the click: retry' } }
L 'menu route (R4 path)'; W 40; KillRoomSetup; FgGame; Start-Sleep 1; Shot '01-mainmenu'
# AG fix 2026-10-30 (AGF2/AGF3 on step133: all of shots 02-05 were the game's Controls help page, white; the route taps never moved it and no flight ever started):
# look at the GAME window (mean luma of a sample grid): a bright page (Controls diagram, mean > 150; the main menu is dark red ~60) gets up to 3 'confirm' taps before the route starts;
# a missing/small game window gets up to 60 s more. Everything is logged (driver.log has the driver's own output, e.g. a foreground refusal).
function GameWin { Get-Process acz -ea 0 | ? { $_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR' -and $_.MainWindowHandle -ne 0 } | select -Last 1 }
function GameLuma { $g=GameWin; if(-not $g){ return -1.0 }; $rc=New-Object WV+RECT; [void][WV]::GetWindowRect($g.MainWindowHandle,[ref]$rc); $w=$rc.R-$rc.L; $h=$rc.B-$rc.T; if($w -lt 800 -or $h -lt 450){ return -1.0 }
  try { Add-Type -AssemblyName System.Drawing; $bm=New-Object Drawing.Bitmap $w,$h; $gr=[Drawing.Graphics]::FromImage($bm); $gr.CopyFromScreen($rc.L,$rc.T,0,0,$bm.Size); $s=0.0;$n=0
    for($yy=0;$yy -lt 12;$yy++){ for($xx=0;$xx -lt 20;$xx++){ $c=$bm.GetPixel([int](($xx+0.5)*$w/20),[int](($yy+0.5)*$h/12)); $s+=0.2126*$c.R+0.7152*$c.G+0.0722*$c.B; $n++ } }; $gr.Dispose(); $bm.Dispose(); $s/$n } catch { -1.0 } }
KillRoomSetup; FgGame
for($k=0;$k -lt 12 -and (GameLuma) -lt 0;$k++){ L 'game window not ready: waiting 5 s'; W 5 }
for($k=0;$k -lt 3;$k++){ $lu=GameLuma; L ("game window mean luma {0:N0}" -f $lu); if($lu -gt 150){ L 'bright page (Controls help screen?): confirm tap'; T 'confirm'; W 4 } else { break } }
W 4
# AG2 fix 2026-10-30 (AGF2 on step136: both 'accelerate' taps changed nothing, the later 'back' went to the title screen; manual single launches with the identical cfg and taps at 38 s / 52 s after
# the Launch click worked, so the loss is intermittent): every main-menu tap is verified by a change in a 40x24 luma grid of the game window and retried with the alternative key
# (accelerate <-> confirm, up to 4 taps per step), so a swallowed tap cannot derail the route. Logged as 'sig-diff'.
function GameSig { $g=GameWin; if(-not $g){ return $null }; $rc=New-Object WV+RECT; [void][WV]::GetWindowRect($g.MainWindowHandle,[ref]$rc); $w=$rc.R-$rc.L; $h=$rc.B-$rc.T; if($w -lt 800 -or $h -lt 450){ return $null }
  try { $bm=New-Object Drawing.Bitmap $w,$h; $gr=[Drawing.Graphics]::FromImage($bm); $gr.CopyFromScreen($rc.L,$rc.T,0,0,$bm.Size); $a=New-Object double[] 960
    for($yy=0;$yy -lt 24;$yy++){ for($xx=0;$xx -lt 40;$xx++){ $c=$bm.GetPixel([int](($xx+0.5)*$w/40),[int](($yy+0.5)*$h/24)); $a[$yy*40+$xx]=0.2126*$c.R+0.7152*$c.G+0.0722*$c.B } }; $gr.Dispose(); $bm.Dispose(); ,$a } catch { $null } }
function SigDiff($a,$b){ if(-not $a -or -not $b){ return -1.0 }; $n=0; for($i=0;$i -lt 960;$i++){ if([math]::Abs($a[$i]-$b[$i]) -gt 25){ $n++ } }; 100.0*$n/960 }   # % of cells that changed by > 25 luma
function TapChg([string[]]$keys,[double]$settle=2.5){ FgGame; Start-Sleep -Milliseconds 800; $s0=GameSig; $i=0   # foreground FIRST: a window raise alone changes the grid (AGF1 17:34: 83 % = the game coming out from behind the OpenVRStartup console, not a menu change)
  foreach($k in $keys){ $i++; T $k; W $settle; $s1=GameSig; $d=SigDiff $s0 $s1; L ("sig-diff after '{0}' (try {1}): {2:N1} % cells changed" -f $k,$i,$d); if($d -ge 4.0){ return $true } }
  L 'route: no screen change after all tries'; return $false }
if(-not (TapChg @('accelerate','confirm','accelerate','confirm'))){ L 'main menu did not open Campaign' }   # opens Campaign (menu -> NEW GAME/CONTINUE list)
W 1; if(-not (TapChg @('accelerate','confirm','accelerate','confirm') 3.5)){ L 'Continue did not start the loading cinematic' }
W 3; Shot '02-cinematic'
if($BriefMap){   # ---- BRIEFING MAP TABLE route (see the header comment): back taps only until the map is seen, no 'accelerate', no flight
  $bld="$env:LOCALAPPDATA\acz-vr"; $bl="$bld\logs"; $barms='briefing-observe.arm','brief3d-capture.arm'
  function BTail([string]$f,[int]$kb){ $txt=''; try { $fs=[IO.File]::Open($f,'Open','Read','ReadWrite,Delete'); try { [void]$fs.Seek([math]::Max(0,$fs.Length-$kb*1024),'Begin'); $sr=New-Object IO.StreamReader($fs,[Text.Encoding]::Latin1); $txt=$sr.ReadToEnd() } finally { $fs.Dispose() } } catch { }; $txt -split "`n" }
  # map mark: newest brief3d-capture line with sel>0 (1 per s while armed) and the last summary-brief3d selmap total (5 s windows), both from the game's ee-camera.log (shared read)
  function MapMark { $cap=0.0; $sel=-1.0
    foreach($ln in (BTail "$bl\ee-camera.log" 600)){
      if($ln -match '^(\d+(?:\.\d+)?)\|brief3d-capture;sel=([1-9]\d*)'){ $t=[double]$Matches[1]; if($t -gt $cap){ $cap=$t } }
      elseif($ln -match '^\d+(?:\.\d+)?\|summary-brief3d;.*?selmap=(\d+)'){ $sel=[double]$Matches[1] } }
    [pscustomobject]@{cap=$cap;sel=$sel} }
  function MapSeen($b){ $m=MapMark; (($m.cap -gt $b.cap) -or ($m.sel -gt [math]::Max($b.sel,0))) }
  function WaitMap($b,[int]$s){ $sw=[Diagnostics.Stopwatch]::StartNew(); while($sw.Elapsed.TotalSeconds -lt $s){ Stop?; if(MapSeen $b){ return $true }; Start-Sleep -Milliseconds 700 }; return $false }
  function BEnd([int]$code){ foreach($a in $barms){ Remove-Item "$bld\$a" -ea 0 }; Remove-Item "$bld\png-screen.now","$bld\png-eye.now" -ea 0
    if($script:rsJob){ Stop-Job $script:rsJob -ea 0; Remove-Job $script:rsJob -Force -ea 0 }
    L 'closing game'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }; L 'done'; exit $code }
  # a flight must never start in this route (the log is cleared at every launch, so any flight-stage-detect line means a mission began)
  function FlightGuard { if((BTail "$bl\xr-stereo.log" 4096) -join "`n" -match 'flight-stage-detect'){ L 'BRIEFMAP: flight-stage-detect seen = a mission started: ABORT, no more taps'; New-Item -ItemType File "$E\MISSION-STARTED" -Force | Out-Null; BEnd 6 } }
  function BHold([string]$name){
    L ("BRIEFMAP hold start ({0}): {1} s, png-screen.now every {2} s, one png-eye.now" -f $name,$BriefHold,$BriefPngEvery)
    $hsPoses=@('0 0 0 0 0 0','25 0 0 0 0 0','-25 0 0 0 0 0','0 15 0 0 0 0','0 0 0 0.3 0 0','0 0 0 -0.3 0 0','0 0 0 0 0 0.3','0 0 0 0 0 0'); $hsPath=Join-Path $env:LOCALAPPDATA 'acz-vr\testhmd-pose.txt'
    $hb=MapMark; $sw=[Diagnostics.Stopwatch]::StartNew(); $nextPng=1.0; $nextShot=6.0; $eyeAt=[math]::Min(10.5,[math]::Max(1.5,$BriefHold/2.0)); $eye=$false; $np=0
    while($sw.Elapsed.TotalSeconds -lt $BriefHold){ Stop?; $t=$sw.Elapsed.TotalSeconds
      if($t -ge $nextPng){ if($HeadSweep){ $hp=$hsPoses[($np+1) % $hsPoses.Count]; Set-Content $hsPath $hp -Encoding ascii; L ("BRIEFMAP headsweep pose (next frame) -> {0}" -f $hp) }; Set-Content "$bld\png-screen.now" "BM-$name" -Encoding ascii; $np++; $nextPng+=$BriefPngEvery; L ("BRIEFMAP png-screen.now {0} ({1}) +{2:N1} s" -f $np,$name,$t) }
      if(-not $eye -and $t -ge $eyeAt){ Set-Content "$bld\png-eye.now" "BM-$name" -Encoding ascii; $eye=$true; L ("BRIEFMAP png-eye.now ({0}) +{1:N1} s" -f $name,$t) }
      if($t -ge $nextShot){ Shot ("brief-{0}-hold{1:N0}" -f $name,$t); $nextShot+=10 }
      Start-Sleep -Milliseconds 250 }
    L ("BRIEFMAP hold end ({0}): {1} png-screen triggers, map still drawn at the end = {2}" -f $name,$np,(MapSeen $hb)) }
  L 'BRIEFMAP: read-only arms (briefing-observe, brief3d-capture) set; waiting 7 s for the 5 s poll'
  foreach($a in $barms){ Set-Content "$bld\$a" 'BM' -Encoding ascii }; W 7
  $okA=$false; $okB=$false; $still=$true
  # leg A: the 3D overview that follows the cinematic (AGU1 2026-10-31: back 1 -> map draws for ~6 s, back 2 -> mission menu)
  $b=MapMark; T 'back'; FlightGuard; L 'BRIEFMAP overview: back 1 (cinematic -> 3D overview expected)'; $okA=WaitMap $b 14
  if(-not $okA){ L 'BRIEFMAP overview: no map draws after back 1 (an extra title card in this save?): ONE more back'; $b=MapMark; T 'back'; FlightGuard; $okA=WaitMap $b 14 }
  if($okA){ L 'BRIEFMAP overview: map draws seen'; Shot 'brief-overview-seen'; BHold 'overview'
    $b=MapMark; T 'back'; FlightGuard; L 'BRIEFMAP overview: back (3D overview -> mission menu expected)'; $still=WaitMap $b 7; L ("BRIEFMAP overview left: map still drawn after back = {0}" -f $still); Shot 'brief-mission-menu'
  } else { L 'BRIEFMAP overview: MAP NOT REACHED'; Shot 'brief-overview-missing' }
  # leg B: BRIEFING item of the mission menu (cursor starts on START MISSION; down, down -> BRIEFING). Each 'down' must change the luma grid (the highlight moved), else NO confirm.
  if($okA -and -not $BriefOverviewOnly -and -not $still){
    W 1; $s0=GameSig; T 'down'; W 1.5; $s1=GameSig; $d1=SigDiff $s0 $s1; T 'down'; W 1.5; $s2=GameSig; $d2=SigDiff $s1 $s2
    L ("BRIEFMAP briefing: menu highlight change after down 1 / down 2 = {0:N1} % / {1:N1} % cells (need >= 1.5 each)" -f $d1,$d2); Shot 'brief-menu-sel'
    if($d1 -ge 1.5 -and $d2 -ge 1.5){
      $b=MapMark; T 'confirm'; FlightGuard; L 'BRIEFMAP briefing: confirm on BRIEFING (map presentation card expected)'; $okB=WaitMap $b 8; Shot 'brief-menu-after-confirm'
      if(-not $okB){ $b=MapMark; T 'back'; FlightGuard; L 'BRIEFMAP briefing: back (map presentation card -> 3D map expected)'; $okB=WaitMap $b 14 }
      if($okB){ L 'BRIEFMAP briefing: map draws seen'; Shot 'brief-briefing-seen'; BHold 'briefing' } else { L 'BRIEFMAP briefing: MAP NOT REACHED'; Shot 'brief-briefing-missing' }
    } else { L 'BRIEFMAP briefing: down taps not verified: NO confirm (a confirm on the wrong item could start the mission)' }
  } elseif($okA -and -not $BriefOverviewOnly){ L 'BRIEFMAP briefing: the overview still drew after back (not at the mission menu): leg skipped, no taps' }
  if(-not $okA -and -not $okB){ L 'ROUTE FAILED: the briefing map was never reached'; New-Item -ItemType File "$E\ROUTE-FAILED" -Force | Out-Null; BEnd 5 }
  L ("BRIEFMAP done: overview={0} briefing={1}" -f $okA,$okB); BEnd 0
}
T 'back'; W 6; T 'back'; W 6; Shot '03-overview'
T 'back'; W 4; Shot '04-mission-menu'
if($MissionKeys){ $mi=0; foreach($mk in ($MissionKeys -split '[,; ]+' | ? {$_})){ $mi++; T $mk; W 2; Shot ("04-mission-sel-$mi-$mk") }; L "mission-select keys: $MissionKeys" }   # runMission.ps1: UNVERIFIED mission-select keys before START MISSION; screenshots show where they landed
T 'accelerate'; L 'START MISSION'; W 6; T 'confirm'; W 7; T 'confirm'; W 6; T 'confirm'; W 8; T 'confirm'; L 'deploy confirmed'; W 20; Shot '05-loaded'
W 8
function DetN { try { (Select-String -Path "$env:LOCALAPPDATA\acz-vr\logs\xr-stereo.log" -Pattern 'flight-stage-detect' -ea Stop).Count } catch { 0 } }
function EnsureCockpit { $b0=DetN; if($b0 -gt $script:det0){ L 'flight already detected'; $script:det0=$b0; return }; for($v=1;$v -le 3;$v++){ Stop?; pwsh -NoProfile -File $cdrv -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" *> $null; L "V try $v"; W 4; $n1=DetN; if($n1 -gt $script:det0){ L "flight detected after V try $v"; $script:det0=$n1; return } }; L 'no flight detected after 3 V' }
$script:det0=0; EnsureCockpit
W 15; Shot '13-runway'; W 25; Shot '14-roll'
L 'm03 air start: no takeoff skip'; W 5
$script:det0=0; EnsureCockpit
if((DetN) -eq 0){ L 'ROUTE FAILED: no flight detected after two V rounds (the menu route did not reach a flight): stop instead of a 120 s blind autopilot'; New-Item -ItemType File "$E\ROUTE-FAILED" -Force | Out-Null; Shot '99-route-failed'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; exit 5 }
L "autopilot start: $AP"; $apl="$E\autopilot-1.log"; Invoke-Expression "& pwsh -NoProfile -File `"$X\autopilot.ps1`" $AP -DecisionLog `"$apl`"" *> "$E\autopilot-1-console.txt"; L "autopilot 1 exit=$LASTEXITCODE"
if($AP2 -and (Get-Process acz -ea 0)){ L "autopilot 2 start: $AP2"; $apl="$E\autopilot-2.log"; Invoke-Expression "& pwsh -NoProfile -File `"$X\autopilot.ps1`" $AP2 -DecisionLog `"$apl`"" *> "$E\autopilot-2-console.txt"; L "autopilot 2 exit=$LASTEXITCODE" }

if($Debrief){   # ---- POST-MISSION DEBRIEF route (see the header comment)
  . "$X\debrieflib.ps1"
  $bld="$env:LOCALAPPDATA\acz-vr"; $bl="$bld\logs"; $darms='briefing-observe.arm','brief3d-capture.arm'
  function DTail([string]$f,[int]$kb){ $txt=''; try { $fs=[IO.File]::Open($f,'Open','Read','ReadWrite,Delete'); try { [void]$fs.Seek([math]::Max(0,$fs.Length-$kb*1024),'Begin'); $sr=New-Object IO.StreamReader($fs,[Text.Encoding]::Latin1); $txt=$sr.ReadToEnd() } finally { $fs.Dispose() } } catch { }; $txt -split "`n" }
  function DScreen { $g=GameWin; if(-not $g){ return $null }; $rc=New-Object WV+RECT; [void][WV]::GetWindowRect($g.MainWindowHandle,[ref]$rc); $w=$rc.R-$rc.L; $h=$rc.B-$rc.T; if($w -lt 800 -or $h -lt 450){ return $null }
    try { $bm=New-Object Drawing.Bitmap $w,$h; $gr=[Drawing.Graphics]::FromImage($bm); $gr.CopyFromScreen($rc.L,$rc.T,0,0,$bm.Size); $gr.Dispose(); try { DbrClassify $bm } finally { $bm.Dispose() } } catch { $null } }
  # newest summary-dbr3d line of the game's ee-camera.log: armed / arms / screens / frames / published and its age (vs the newest line of any kind)
  function DMark { $m=[pscustomobject]@{armed=-1;arms=-1;screens=-1;frames=-1;published=-1;age=1e9}; $tick=0.0; $mt=0.0
    foreach($ln in (DTail "$bl\ee-camera.log" 400)){ if($ln -match '^(\d+(?:\.\d+)?)\|'){ $v=[double]$Matches[1]; if($v -gt $tick){ $tick=$v } }
      if($ln -match '^(\d+(?:\.\d+)?)\|summary-dbr3d;armed=(\d+);arms=(\d+);screens=(\d+);frames=(\d+);usable=\d+;published=(\d+)'){ $mt=[double]$Matches[1]; $m=[pscustomobject]@{armed=[int]$Matches[2];arms=[int]$Matches[3];screens=[int]$Matches[4];frames=[int]$Matches[5];published=[int]$Matches[6];age=0.0} } }
    if($mt -gt 0){ $m.age=[math]::Max(0.0,($tick-$mt)/1000.0) }   # age relative to the newest line of ANY kind (both clocks are the game's own; the summary comes every 5 s)
    $m }
  function DDet { try { @(Select-String -Path "$bl\xr-stereo.log" -Pattern 'flight-stage-detect' -ea Stop).Count } catch { 0 } }
  $script:dDet0=DDet
  function DEnd([int]$code){ foreach($a in $darms){ Remove-Item "$bld\$a" -ea 0 }; Remove-Item "$bld\ui3d-census.arm","$bld\png-screen.now","$bld\png-eye.now" -ea 0
    if($script:rsJob){ Stop-Job $script:rsJob -ea 0; Remove-Job $script:rsJob -Force -ea 0 }
    L 'closing game'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }; L 'done'; exit $code }
  function DGuard { if((DDet) -gt $script:dDet0){ L 'DEBRIEF: flight-stage-detect after the first flight = a new mission started: ABORT, no more taps'; New-Item -ItemType File "$E\MISSION-STARTED" -Force | Out-Null; DEnd 6 } }
  $script:dCensus=0.0
  function DCensus([double]$t){ if($t -ge $script:dCensus){ Remove-Item "$bld\ui3d-census.arm" -ea 0; Start-Sleep -Milliseconds 1500; Set-Content "$bld\ui3d-census.arm" 'DBR' -Encoding ascii; $script:dCensus=$t+15 } }
  function DLog([string]$what){ $c=DScreen; $m=DMark; L ("DEBRIEF {0}: screen {1}{2} luma {3} | dbr3d armed {4} arms {5} screens {6} frames {7} published {8} (line age {9:N0} s)" -f $what,$(if($c){$c.cls}else{'?'}),$(if($c -and $c.cls -eq 'PAUSE'){" row $($c.row)"}else{''}),$(if($c){$c.luma}else{'?'}),$m.armed,$m.arms,$m.screens,$m.frames,$m.published,[math]::Min($m.age,9999)); $c }
  function DHold([string]$name,[int]$secs){
    L ("DEBRIEF hold start ({0}): {1} s, png-screen.now every {2} s, one png-eye.now" -f $name,$secs,$DebriefPngEvery)
    $sw=[Diagnostics.Stopwatch]::StartNew(); $nextPng=1.0; $nextShot=6.0; $eyeAt=[math]::Min(10.5,[math]::Max(1.5,$secs/2.0)); $eye=$false; $np=0; $nextLog=5.0
    while($sw.Elapsed.TotalSeconds -lt $secs){ Stop?; $t=$sw.Elapsed.TotalSeconds
      if($t -ge $nextPng){ Set-Content "$bld\png-screen.now" "DB-$name" -Encoding ascii; $np++; $nextPng+=$DebriefPngEvery; L ("DEBRIEF png-screen.now {0} ({1}) +{2:N1} s" -f $np,$name,$t) }
      if(-not $eye -and $t -ge $eyeAt){ Set-Content "$bld\png-eye.now" "DB-$name" -Encoding ascii; $eye=$true; L ("DEBRIEF png-eye.now ({0}) +{1:N1} s" -f $name,$t) }
      if($t -ge $nextShot){ Shot ("debrief-{0}-hold{1:N0}" -f $name,$t); $nextShot+=10 }
      if($t -ge $nextLog){ [void](DLog "hold $name +$([int]$t) s"); $nextLog+=10 }
      DCensus $t; Start-Sleep -Milliseconds 250 }
    L ("DEBRIEF hold end ({0}): {1} png-screen triggers" -f $name,$np) }
  # pause menu -> QUIT MISSION (items: RETURN TO GAME / REPLAY / RETRY / QUIT MISSION / FLIGHT OPTIONS = rows 0..4; RETRY restarts the mission, so a confirm is only ever sent with the highlight verified on row 3)
  function DQuitRoute { L 'DEBRIEF quit route: Esc (pause menu) -> QUIT MISSION'
    $c=DScreen; if($c -and $c.cls -eq 'PAUSE'){ L 'DEBRIEF quit: pause menu already open' } else { T 'back'; W 1.5; $c=DScreen; if(-not $c -or $c.cls -ne 'PAUSE'){ W 2; $c=DScreen } }
    if(-not $c -or $c.cls -ne 'PAUSE'){ L ("DEBRIEF quit: pause menu NOT seen after Esc (screen {0}): no more taps" -f $(if($c){$c.cls}else{'?'})); Shot 'debrief-quit-nopause'; return $false }
    Shot 'debrief-quit-pause'; $stuck=0
    for($i=0;$i -lt 7;$i++){ $c=DScreen; if(-not $c -or $c.cls -ne 'PAUSE'){ L 'DEBRIEF quit: pause menu lost while navigating: stop'; return $false }
      if($c.row -eq 3){ break }
      $k=$(if($c.row -lt 3){'down'}else{'up'}); $r0=$c.row; T $k; W 1.0; $c2=DScreen; if($c2 -and $c2.cls -eq 'PAUSE' -and $c2.row -eq $r0){ $stuck++; if($stuck -ge 2){ L 'DEBRIEF quit: the highlight does not move: stop'; return $false } } }
    $c=DScreen; if(-not $c -or $c.cls -ne 'PAUSE' -or $c.row -ne 3){ L 'DEBRIEF quit: QUIT MISSION (row 3) not highlighted: NO confirm'; Shot 'debrief-quit-notrow3'; return $false }
    Shot 'debrief-quit-row3'; T 'confirm'; DGuard; L 'DEBRIEF quit: confirm on QUIT MISSION (what follows is UNVERIFIED: dialog / results / mission menu)'; W 3; Shot 'debrief-quit-after-confirm'
    for($q=1;$q -le $DebriefQuitConfirms;$q++){ [void](DLog "quit follow-up $q before confirm"); T 'confirm'; DGuard; L "DEBRIEF quit: extra confirm $q (-DebriefQuitConfirms)"; W 3; Shot "debrief-quit-confirm$q" }
    return $true }

  L ("DEBRIEF: route start (end wait {0} s, debrief wait {1} s, hold {2} s, {3} pages max, quit-route {4}); read-only arms briefing-observe + brief3d-capture + ui3d-census" -f $DebriefEndWait,$DebriefWait,$DebriefHold,$DebriefMaxPages,[bool]$DebriefQuit)
  foreach($a in $darms){ Set-Content "$bld\$a" 'DBR' -Encoding ascii }
  $ok=$false; $replay=$false; $dbr=$false
  # 1) mission end: REPLAY seen on two samples in a row (or the explicit quit route)
  if($DebriefQuit){ if(DQuitRoute){ $ok=$true } }
  $sw=[Diagnostics.Stopwatch]::StartNew(); $nlog=0.0; $r1=0
  while(-not $ok -and $sw.Elapsed.TotalSeconds -lt $DebriefEndWait -and (Get-Process acz -ea 0)){ Stop?; DGuard; $t=$sw.Elapsed.TotalSeconds; DCensus $t
    $c=DScreen; if($c -and $c.cls -eq 'REPLAY'){ $r1++ } else { $r1=0 }
    if($r1 -ge 2){ $replay=$true; $ok=$true; L ("DEBRIEF: REPLAY screen seen (mission ended: crash replay) after {0:N0} s of waiting" -f $t); Shot 'debrief-replay-seen'; break }
    if($t -ge $nlog){ [void](DLog ("waiting for the mission end +{0:N0} s" -f $t)); $nlog+=15 }
    Start-Sleep -Milliseconds 800 }
  if(-not $ok -and -not $DebriefQuit -and (Get-Process acz -ea 0)){ L ("DEBRIEF: no mission end within {0} s: fallback = the pause-menu QUIT MISSION route" -f $DebriefEndWait); if(DQuitRoute){ $ok=$true } }
  if(-not $ok){ L 'DEBRIEF: MISSION END NOT REACHED'; New-Item -ItemType File "$E\DEBRIEF-NOT-REACHED" -Force | Out-Null; Shot 'debrief-no-end'; DEnd 5 }
  # 2) leave the replay with Esc ('menu button'): only on a verified REPLAY screen, max 3 attempts 10 s apart
  if($replay){ $left=$false
    for($try=1;$try -le 3 -and -not $left;$try++){
      $c=DScreen; if(-not $c -or $c.cls -ne 'REPLAY'){ $c2=$null; for($z=0;$z -lt 6 -and (-not $c2 -or $c2.cls -ne 'REPLAY');$z++){ Start-Sleep -Milliseconds 700; $c2=DScreen }; if(-not $c2 -or $c2.cls -ne 'REPLAY'){ L 'DEBRIEF: replay no longer seen (cut fade / already left): not tapping'; $left=$true; break } }
      T 'back'; DGuard; L ("DEBRIEF: Esc ({0}) on the REPLAY screen" -f $try)
      $nonrep=0; $sw2=[Diagnostics.Stopwatch]::StartNew(); while($sw2.Elapsed.TotalSeconds -lt 10 -and $nonrep -lt 3){ Stop?; Start-Sleep -Milliseconds 1000; $c=DScreen; if($c -and $c.cls -ne 'REPLAY'){ $nonrep++ } else { $nonrep=0 } }
      if($nonrep -ge 3){ $left=$true; L 'DEBRIEF: replay left'; Shot 'debrief-replay-left' } else { L 'DEBRIEF: still on the replay'; Shot ("debrief-replay-esc{0}" -f $try) } }
    if(-not $left){ L 'DEBRIEF: REPLAY NOT EXITED by Esc x3 (ROUTE-INCOMPLETE): no other key is tried'; New-Item -ItemType File "$E\DEBRIEF-NOT-REACHED" -Force | Out-Null; DEnd 5 } }
  # 3) wait for the debrief itself: dbr3d armed with a fresh line (the producer saw the results-panel window quad). Unknown screens are only logged + shot; opt-in probes confirm on a static one.
  $sw=[Diagnostics.Stopwatch]::StartNew(); $nlog=0.0; $nshot=0.0; $probes=0; $prev=$null; $prevAt=0.0
  while($sw.Elapsed.TotalSeconds -lt $DebriefWait -and (Get-Process acz -ea 0)){ Stop?; DGuard; $t=$sw.Elapsed.TotalSeconds; DCensus $t
    $m=DMark; if($m.armed -eq 1 -and $m.age -lt 8){ $dbr=$true; break }
    if($t -ge $nlog){ [void](DLog ("waiting for the debrief +{0:N0} s" -f $t)); $nlog+=10 }
    if($t -ge $nshot){ Shot ("debrief-wait{0:N0}" -f $t); $nshot+=15 }
    if($DebriefProbeConfirm -gt 0 -and $probes -lt $DebriefProbeConfirm -and $t -ge ($prevAt+12)){ $s=GameSig; if($prev -and $s){ $d=SigDiff $prev $s; $c=DScreen
        if($d -lt 1.5 -and $c -and $c.cls -eq 'OTHER'){ T 'confirm'; DGuard; $probes++; L ("DEBRIEF probe confirm {0}/{1}: the unknown screen was static ({2:N1} % cells changed in 12 s)" -f $probes,$DebriefProbeConfirm,$d); Shot ("debrief-probe{0}" -f $probes) } }
      $prev=$s; $prevAt=$t }
    Start-Sleep -Milliseconds 700 }
  if(-not $dbr){ L ("DEBRIEF: DEBRIEF NOT REACHED within {0} s after the mission end (no summary-dbr3d armed=1); see the debrief-wait* shots for the screen it sits on" -f $DebriefWait); New-Item -ItemType File "$E\DEBRIEF-NOT-REACHED" -Force | Out-Null; Shot 'debrief-not-reached'; DEnd 5 }
  [void](DLog 'debrief armed'); Shot 'debrief-seen'
  # 4) hold + pages
  $page=1; DHold 'p1' $DebriefHold
  while($page -lt $DebriefMaxPages -and (Get-Process acz -ea 0)){ $m=DMark; if(-not ($m.armed -eq 1 -and $m.age -lt 8)){ L 'DEBRIEF: not armed (or no fresh line) before the next page tap: debrief left, no tap'; break }
    $s0=GameSig; $sc0=$m.screens; Shot ("debrief-page{0}-before-confirm" -f $page); T 'confirm'; DGuard; L ("DEBRIEF: confirm to advance from page {0}" -f $page); W 4; $s1=GameSig; $d=SigDiff $s0 $s1; $m2=DMark
    L ("DEBRIEF: after confirm: {0:N1} % cells changed, armed {1}, screens {2} (was {3})" -f $d,$m2.armed,$m2.screens,$sc0); Shot ("debrief-page{0}-after-confirm" -f $page)
    if($d -lt 4.0 -and $m2.screens -le $sc0){ L 'DEBRIEF: the confirm changed nothing: stop paging'; break }
    $page++; if($m2.armed -eq 1 -or $m2.age -ge 8){ DHold ("p{0}" -f $page) $DebriefPageHold } else { L 'DEBRIEF: armed dropped after the confirm (left the debrief)'; break } }
  [void](DLog 'end of debrief route'); L ("DEBRIEF done: replay={0} reached={1} pages={2}" -f $replay,$dbr,$page)
  DEnd 0
}

if($PostWait -gt 0 -and (Get-Process acz -ea 0)){
  $ld="$env:LOCALAPPDATA\acz-vr"; $arms='briefing-observe.arm','brief3d-capture.arm'
  L "post-flight wait $PostWait s: debrief capture armed (briefing-observe, brief3d-capture, ui3d-census every 15 s, png-screen.now every 8 s, confirm tap every $PostTapEvery s max $PostTapMax)"
  try {
    foreach($a in $arms){ Set-Content "$ld\$a" 'AG' -Encoding ascii }
    $tw=[Diagnostics.Stopwatch]::StartNew(); $nextCensus=0.0; $nextPng=3.0; $nextTap=[double]$PostTapEvery; $nTap=0
    while($tw.Elapsed.TotalSeconds -lt $PostWait -and (Get-Process acz -ea 0)){
      if(Test-Path "$E\GAMEOVER"){ L 'GAMEOVER during post-wait: stop'; break }
      $t=$tw.Elapsed.TotalSeconds
      if($t -ge $nextCensus){ Remove-Item "$ld\ui3d-census.arm" -ea 0; Start-Sleep -Milliseconds 1500; Set-Content "$ld\ui3d-census.arm" 'AG' -Encoding ascii; $nextCensus=$t+15; L 'post census re-armed' }
      if($t -ge $nextPng){ Set-Content "$ld\png-screen.now" 'AG' -Encoding ascii; $nextPng=$t+8 }
      if($nTap -lt $PostTapMax -and $t -ge $nextTap){ Shot ("post-{0}-before-confirm" -f $nTap); T 'confirm'; $nTap++; $nextTap=$t+$PostTapEvery; Start-Sleep 2; Shot ("post-{0}-after-confirm" -f ($nTap-1)) }
      Start-Sleep -Milliseconds 500 }
  } finally {
    foreach($a in $arms){ Remove-Item "$ld\$a" -ea 0 }; Remove-Item "$ld\ui3d-census.arm","$ld\png-screen.now" -ea 0; L 'post-flight wait over, arm files removed' }
}

if($script:rsJob){ Stop-Job $script:rsJob -ea 0; Remove-Job $script:rsJob -Force -ea 0 }; L 'closing game'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }
L 'done'
