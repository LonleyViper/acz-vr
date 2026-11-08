param([string]$E,[string]$Times='30,50,70,90,110,130',[double]$Hold=2.0,[string]$MirrorDir,[double]$ShotEvery=0.25,[double]$ShotFor=3.0,[string]$StartPattern='autopilot start',[switch]$WithMirror)
# Gun-tracer test helper: waits for $StartPattern in $E\steps.txt (= T0), then holds J for $Hold s at each T0+t (SendInput, the same
# key path as run83/autopilot; foreground checked), logging wall time and GetTickCount64 (the log clock) of every key down/up.
# Batch M: the PrintWindow mirror capture of the Headset Window gave BLACK frames (L2a): it is OFF now (-WithMirror re-enables it). killcap.ps1 captures the
# mirror with CopyFromScreen (upright: view with mirror-sheet.ps1 -NoRotate) and goes to fast mode on every 'J DOWN' line of gunburst.log.
# During each burst: fast PrintWindow captures of the null 'Headset Window' every $ShotEvery s for $ShotFor s, plus game-window shots.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class GB{
[StructLayout(LayoutKind.Sequential)]public struct KI{public ushort vk,sc;public uint fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Sequential)]public struct MI{public int dx,dy;public uint md,fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Explicit)]public struct U{[FieldOffset(0)]public MI mi;[FieldOffset(0)]public KI ki;}
[StructLayout(LayoutKind.Sequential)]public struct IN{public uint type;public U u;}
[StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Rr,B;}
[DllImport("user32.dll")]public static extern uint SendInput(uint n,IN[] i,int s);
[DllImport("user32.dll")]public static extern uint MapVirtualKey(uint c,uint t);
[DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();
public static uint Key(ushort vk,bool up){IN i=new IN();i.type=1;i.u.ki.vk=vk;i.u.ki.sc=(ushort)MapVirtualKey(vk,0);i.u.ki.fl=up?2u:0u;return SendInput(1,new IN[]{i},Marshal.SizeOf(typeof(IN)));}
}
'@
[void][GB]::SetProcessDPIAware()
$log="$E\gunburst.log"; function L($m){ "{0} tick={1} {2}" -f (Get-Date -f HH:mm:ss.fff),[Environment]::TickCount64,$m | Add-Content $log }
New-Item -ItemType Directory -Force $MirrorDir | Out-Null; New-Item -ItemType Directory -Force "$E\gunshots" | Out-Null
function Cap($h,[string]$out,[switch]$Client){
  $r=New-Object GB+R; if($Client){ [void][GB]::GetClientRect($h,[ref]$r) } else { [void][GB]::GetWindowRect($h,[ref]$r); $r.Rr-=$r.L; $r.B-=$r.T }
  $b=New-Object System.Drawing.Bitmap $r.Rr,$r.B; $g=[System.Drawing.Graphics]::FromImage($b); $dc=$g.GetHdc(); [void][GB]::PrintWindow($h,$dc,2); $g.ReleaseHdc($dc); $g.Dispose()
  $b.Save($out,[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose() }
while(-not (Select-String -Path "$E\steps.txt" -Pattern $StartPattern -Quiet -ea 0)){ if(Test-Path "$E\stopN"){ exit 1 }; Start-Sleep -Milliseconds 200 }
$sw=[Diagnostics.Stopwatch]::StartNew(); L "T0 ($StartPattern seen)"
$vrw=(Get-Process vrcompositor -ea 0 | ? { $_.MainWindowTitle -eq 'Headset Window' } | select -First 1).MainWindowHandle
foreach($t in ($Times.Split(',') | % { [double]$_ })){
  while($sw.Elapsed.TotalSeconds -lt $t){ if(-not (Get-Process acz -ea 0)){ L 'acz gone'; exit 3 }; Start-Sleep -Milliseconds 20 }
  $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1; if(-not $p){ L 'no game window'; exit 3 }; $h=$p.MainWindowHandle
  [void][GB]::SetForegroundWindow($h); Start-Sleep -Milliseconds 60
  $fg=([GB]::GetForegroundWindow() -eq $h); [void][GB]::Key(0x4A,$false); L ("J DOWN at T0+{0:N2} fg={1}" -f $sw.Elapsed.TotalSeconds,$fg)
  $bs=[Diagnostics.Stopwatch]::StartNew(); $k=0; $up=$false; $gs=0
  while($bs.Elapsed.TotalSeconds -lt [math]::Max($Hold,$ShotFor)){
    if(-not $up -and $bs.Elapsed.TotalSeconds -ge $Hold){ [void][GB]::Key(0x4A,$true); $up=$true; L ("J UP at T0+{0:N2}" -f $sw.Elapsed.TotalSeconds) }
    if($bs.Elapsed.TotalSeconds -ge $k*$ShotEvery){ $nm=("b{0:D3}-k{1:D2}-{2}" -f [int]$t,$k,(Get-Date -f HHmmss.fff)); if($vrw -and $WithMirror){ try { Cap $vrw "$MirrorDir\$nm.png" } catch {} }; $k++ }
    if($gs -lt 2 -and $bs.Elapsed.TotalSeconds -ge 0.6+$gs*1.0){ try { Cap $h ("$E\gunshots\game-b{0:D3}-{1}.png" -f [int]$t,$gs) -Client } catch {}; $gs++ }
    Start-Sleep -Milliseconds 10 }
  if(-not $up){ [void][GB]::Key(0x4A,$true); L ("J UP (late) at T0+{0:N2}" -f $sw.Elapsed.TotalSeconds) }
  L ("burst {0}: {1} mirror shots" -f $t,$k)
}
L 'done'
