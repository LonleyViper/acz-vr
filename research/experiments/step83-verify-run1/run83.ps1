param([string]$ShotDir,[string]$LogFile,[string]$Sampler,[double]$Total=420.0,[double]$FirstTap=20.0,[double]$TapEvery=30.0,[double]$TapLen=0.27,[double]$LevelAfter=1.5,[string]$PitchKey='',[double]$PitchLen=0.15,[double]$PitchFirst=10.0,[double]$PitchEvery=15.0,[double]$ShotEvery=30.0,[string]$MissileAt='',[double]$MissileLen=0.3,[string]$BurstAt='',[string]$TabAt='',[double]$ThrottleAt=-1,[double]$ThrottleLen=20,[string]$RotateAt='',[string]$StartAt='',[double]$TurnUntil=99999,[string]$GunAt='',[double]$GunLen=2.0,[string]$T0Pattern='source-ready',[string]$HoldSeq='',[string]$ScreenPngAt='')
# step83: V -> wait for xr source-ready (T0). No look input. Gentle turns (step82 run82d schedule that survived 585 s):
# right tap 0.27 s every 30 s from T0+20, left LEVEL tap 1.5 s later. Game-window shots every 30 s. No game-memory writes.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class RB{
[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
[StructLayout(LayoutKind.Sequential)]public struct KI{public ushort vk,sc;public uint fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Sequential)]public struct MI{public int dx,dy;public uint md,fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Explicit)]public struct U{[FieldOffset(0)]public MI mi;[FieldOffset(0)]public KI ki;}
[StructLayout(LayoutKind.Sequential)]public struct IN{public uint type;public U u;}
[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);
[DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")]public static extern uint SendInput(uint n,IN[] i,int s);
[DllImport("user32.dll")]public static extern uint MapVirtualKey(uint c,uint t);
public static uint Key(ushort vk,bool up){IN i=new IN();i.type=1;i.u.ki.vk=vk;i.u.ki.sc=(ushort)MapVirtualKey(vk,0);i.u.ki.fl=up?2u:0u;return SendInput(1,new IN[]{i},Marshal.SizeOf(typeof(IN)));}
}
'@
$codes=@{up=0x57;down=0x53;left=0x41;right=0x44;cockpit=0x56;missile=0x4C;tab=0x09;throttle=0x20;start=0x1B;gun=0x4A}
$p=Get-Process acz | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1
$h=$p.MainWindowHandle
function Fg { [void][RB]::SetForegroundWindow($h); Start-Sleep -Milliseconds 100; if([RB]::GetForegroundWindow() -ne $h){ throw 'not foreground' } }
function Shot($tag){ try { ShotInner $tag } catch { } }
function ShotInner($tag){
  $r=New-Object RB+RECT; [void][RB]::GetWindowRect($h,[ref]$r); $w=$r.R-$r.L; $hh=$r.B-$r.T; if($w -le 0 -or $hh -le 0){ return }
  $b=New-Object System.Drawing.Bitmap $w,$hh; $g=[System.Drawing.Graphics]::FromImage($b)
  $g.CopyFromScreen($r.L,$r.T,0,0,$b.Size); $f=Join-Path $ShotDir ("acz-{0}-{1}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),$tag)
  $b.Save($f,[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $b.Dispose()
}
function Commit { try { [double](((Get-Content $Sampler -Tail 1) -split ',')[1]) } catch { 0 } }
function Hold($name,$ms){ Fg; [void][RB]::Key([uint16]$codes[$name],$false); Start-Sleep -Milliseconds $ms; [void][RB]::Key([uint16]$codes[$name],$true) }
$ev=New-Object System.Collections.Generic.List[string]
function Log($m){ $s="{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'),$m; $ev.Add($s); Write-Output $s }
$evFile=Join-Path (Split-Path $ShotDir) 'run83-events.txt'
Shot 'pre-V'
$base=0; try { $base=(Get-Content $LogFile -ErrorAction Stop | Select-String $T0Pattern).Count } catch {}
Fg; Log 'V (cockpit view) sent'; Hold 'cockpit' 80
$sw=$null; $deadline=(Get-Date).AddSeconds(25)
$vtry=1; $vnext=(Get-Date).AddSeconds(5)
while((Get-Date) -lt $deadline){ if($vtry -lt 3 -and (Get-Date) -gt $vnext){ Fg; Hold 'cockpit' 80; $vtry++; Log "V retry $vtry (no $T0Pattern yet)"; $vnext=(Get-Date).AddSeconds(5) }; try { if((Get-Content $LogFile -ErrorAction Stop | Select-String $T0Pattern).Count -gt $base){ $sw=[Diagnostics.Stopwatch]::StartNew(); Log "$T0Pattern seen (T0)"; break } } catch {}; Start-Sleep -Milliseconds 100 }
if(-not $sw){ Shot 'no-source'; Log 'source-ready NOT seen: abort, no flight input'; New-Item -ItemType File (Join-Path $ShotDir '..\..\NOMISSION') -Force | Out-Null; $ev | Set-Content $evFile; exit 2 }
$mq=New-Object System.Collections.Generic.Queue[double]; if($MissileAt){ $MissileAt.Split(',') | % { $mq.Enqueue([double]$_) } }
$bq=New-Object System.Collections.Generic.Queue[double]; if($BurstAt){ $BurstAt.Split(',') | % { $bq.Enqueue([double]$_) } }
$tq=New-Object System.Collections.Generic.Queue[double]; if($TabAt){ $TabAt.Split(',') | % { $tq.Enqueue([double]$_) } }
$rq=New-Object System.Collections.Generic.Queue[double]; if($RotateAt){ $RotateAt.Split(',') | % { $rq.Enqueue([double]$_) } }
$sq=New-Object System.Collections.Generic.Queue[double]; if($StartAt){ $StartAt.Split(',') | % { $sq.Enqueue([double]$_) } }
$thrDown=$false;$thrDone=$false
$sp=New-Object System.Collections.Generic.Queue[double]; if($ScreenPngAt){ $ScreenPngAt.Split(',') | % { $sp.Enqueue([double]$_) } }
$hq=New-Object System.Collections.Generic.Queue[object]; if($HoldSeq){ $HoldSeq.Split('/') | % { $a=$_.Split(':'); $hq.Enqueue(@([double]$a[0],$a[1],[double]$a[2])) } }
$gq=New-Object System.Collections.Generic.Queue[double]; if($GunAt){ $GunAt.Split(',') | % { $gq.Enqueue([double]$_) } }
$tapN=0;$nextPitch=$PitchFirst;$nextTap=$FirstTap;$tapUp=$null;$cur=$null;$counterAt=$null;$counterKey=$null;$nextShot=10.0
Log ("WALLCLOCK cockpit control T0 = {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'))
while($sw.Elapsed.TotalSeconds -lt $Total){
  $t=$sw.Elapsed.TotalSeconds
  if((Commit) -ge 97){ Log 'COMMIT >= 97: releasing keys and stopping'; if($cur){[void][RB]::Key([uint16]$codes[$cur],$true)}; break }
  if(-not (Get-Process acz -ea 0)){ Log ('acz gone at T0+{0:N1}' -f $t); break }
  if(Test-Path (Join-Path $ShotDir '..\..\GAMEOVER')){ Log 'GAMEOVER file: stop'; break }
  if($cur -and $t -ge $tapUp){ [void][RB]::Key([uint16]$codes[$cur],$true); Log ("tap key {0} UP at T0+{1:N2}" -f $cur,$t); $cur=$null }
  if(-not $cur -and $t -ge $nextTap -and $t -lt $TurnUntil){
    try { Fg; [void][RB]::Key([uint16]$codes['right'],$false); $cur='right'; $tapUp=$t+$TapLen; Log ("tap {0} key right DOWN at T0+{1:N2}" -f $tapN,$t) } catch { Log ('tap skipped: ' + $_) }
    $counterKey='left'; $counterAt=$t+$LevelAfter; $tapN++; $nextTap+=$TapEvery
  }
  if($counterAt -and -not $cur -and $t -ge $counterAt){
    try { Fg; [void][RB]::Key([uint16]$codes[$counterKey],$false); $cur=$counterKey; $tapUp=$t+$TapLen; Log ("LEVEL tap key {0} DOWN at T0+{1:N2}" -f $counterKey,$t) } catch { Log ('level tap skipped: ' + $_) }
    $counterAt=$null
  }
  if($ThrottleAt -ge 0 -and -not $thrDown -and -not $thrDone -and $t -ge $ThrottleAt){ try { Fg; [void][RB]::Key([uint16]$codes['throttle'],$false); $thrDown=$true; Log ("THROTTLE Space DOWN at T0+{0:N2}" -f $t) } catch { Log ('throttle skipped: ' + $_) } }
  if($thrDown -and $t -ge $ThrottleAt+$ThrottleLen){ [void][RB]::Key([uint16]$codes['throttle'],$true); $thrDown=$false; $thrDone=$true; Log ("THROTTLE Space UP at T0+{0:N2}" -f $t) }
  if($sp.Count -and $t -ge $sp.Peek()){ [void]$sp.Dequeue(); Set-Content (Join-Path $env:LOCALAPPDATA 'acz-vr\png-screen.now') (Get-Date -Format o); Shot ('screenpng-T{0:N0}' -f $t); Log ("png-screen.now written at T0+{0:N2}" -f $t) }
  if($hq.Count -and -not $cur -and $t -ge $hq.Peek()[0]){ $hsq=$hq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes[$hsq[1]],$false); $cur=$hsq[1]; $tapUp=$t+$hsq[2]; Log ("HOLD {0} {1}s DOWN at T0+{2:N2}" -f $hsq[1],$hsq[2],$t) } catch { Log ('hold skipped: ' + $_) } }
  if($gq.Count -and -not $cur -and $t -ge $gq.Peek()){ [void]$gq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes['gun'],$false); $cur='gun'; $tapUp=$t+$GunLen; Log ("GUN J DOWN at T0+{0:N2}" -f $t) } catch { Log ('gun skipped: ' + $_) } }
  if($rq.Count -and -not $cur -and $t -ge $rq.Peek()){ [void]$rq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes['down'],$false); $cur='down'; $tapUp=$t+0.3; Log ("ROTATE S DOWN at T0+{0:N2}" -f $t) } catch { Log ('rotate skipped: ' + $_) } }
  if($sq.Count -and -not $cur -and $t -ge $sq.Peek()){ [void]$sq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes['start'],$false); $cur='start'; $tapUp=$t+0.15; Shot ('start-T{0:N0}' -f $t); Log ("START(Esc) DOWN at T0+{0:N2}" -f $t) } catch { Log ('start skipped: ' + $_) } }
  if($bq.Count -and $t -ge $bq.Peek()){ [void]$bq.Dequeue(); Set-Content (Join-Path $env:LOCALAPPDATA 'acz-vr\png-burst.now') (Get-Date -Format o); Shot ('burst-T{0:N0}' -f $t); Log ("png-burst.now written at T0+{0:N2}" -f $t) }
  if($tq.Count -and -not $cur -and $t -ge $tq.Peek()){ [void]$tq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes['tab'],$false); $cur='tab'; $tapUp=$t+0.15; Log ("TAB DOWN at T0+{0:N2}" -f $t) } catch { Log ('tab skipped: ' + $_) } }
  if($mq.Count -and -not $cur -and $t -ge $mq.Peek()){ [void]$mq.Dequeue(); try { Fg; [void][RB]::Key([uint16]$codes['missile'],$false); $cur='missile'; $tapUp=$t+$MissileLen; Log ("MISSILE key L DOWN at T0+{0:N2}" -f $t) } catch { Log ('missile skipped: ' + $_) } }
  if($PitchKey -and -not $cur -and -not $counterAt -and $t -ge $nextPitch -and [math]::Abs($t-$nextTap) -gt 3){
    try { Fg; [void][RB]::Key([uint16]$codes[$PitchKey],$false); $cur=$PitchKey; $tapUp=$t+$PitchLen; Log ("PITCH tap key {0} DOWN at T0+{1:N2}" -f $PitchKey,$t) } catch { Log ('pitch tap skipped: ' + $_) }
    $nextPitch+=$PitchEvery
  }
  if($t -ge $nextShot){ Shot ('T{0:N0}' -f $t); $nextShot+=$ShotEvery }
  Start-Sleep -Milliseconds 20
}
if($cur){ [void][RB]::Key([uint16]$codes[$cur],$true); Log 'safety key up' }
if($thrDown){ [void][RB]::Key([uint16]$codes['throttle'],$true); Log 'safety throttle up' }
Log ("done at T0+{0:N1}; commit now {1}" -f $sw.Elapsed.TotalSeconds,(Commit))
$ev | Set-Content $evFile
