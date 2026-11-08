param([string]$Out='',[switch]$List,[string]$TitleMatch='VR View|Headset Window|Mirror|SteamVR',[string]$Log='',[string]$ProcMatch='^vr')
# Capture the SteamVR mirror / null-driver headset window WITHOUT activating it (AGENT-BRIEF: never bring the mirror to the front).
# 1) PrintWindow(hwnd, PW_RENDERFULLCONTENT) into a bitmap; 2) if that is (almost) all black and the window is not covered, CopyFromScreen of its
# rectangle. -List prints the visible top-level windows of processes matching -ProcMatch. Logs one line per capture (title, size, method, mean luma).
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public static class MS{
 public delegate bool P(IntPtr h,IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(P p,IntPtr l);
 [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct R{public int L,T,Rr,B;}
 [StructLayout(LayoutKind.Sequential)] public struct PT{public int X,Y;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint f);
 [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
 [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h,uint f);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 public static List<IntPtr> All(){ var l=new List<IntPtr>(); EnumWindows((h,x)=>{ if(IsWindowVisible(h)) l.Add(h); return true; },IntPtr.Zero); return l; }
 public static string Title(IntPtr h){ var s=new StringBuilder(256); GetWindowText(h,s,256); return s.ToString(); }
 public static uint Pid(IntPtr h){ uint p; GetWindowThreadProcessId(h,out p); return p; }
}
'@
[void][MS]::SetProcessDPIAware()
function W($m){ if($Log){ "{0} {1}" -f (Get-Date -f HH:mm:ss.fff),$m | Add-Content $Log }; Write-Output $m }
$names=@{}; Get-Process | ? { $_.Name -match $ProcMatch } | % { $names[[uint32]$_.Id]=$_.Name }
$wins=foreach($h in [MS]::All()){ $p=[MS]::Pid($h); if($names.ContainsKey($p)){ $r=New-Object MS+R; [void][MS]::GetWindowRect($h,[ref]$r); [pscustomobject]@{H=$h;Proc=$names[$p];Title=[MS]::Title($h);L=$r.L;T=$r.T;W=$r.Rr-$r.L;Hgt=$r.B-$r.T;Min=[MS]::IsIconic($h)} } }
if($List){ $wins | % { "{0} h={1} '{2}' {3}x{4} at {5},{6} minimized={7}" -f $_.Proc,$_.H,$_.Title,$_.W,$_.Hgt,$_.L,$_.T,$_.Min }; return }
$w=$wins | ? { $_.Title -match $TitleMatch -and $_.W -gt 100 -and $_.Hgt -gt 100 } | sort { $_.W*$_.Hgt } -desc | select -First 1
if(-not $w){ W "mirror-shot: no window matching '$TitleMatch' (vr windows: $(($wins | % { "$($_.Proc):'$($_.Title)'" }) -join '; '))"; exit 2 }
function Luma($b){ $s=0;$n=0; for($y=0;$y -lt $b.Height;$y+=[math]::Max(1,[int]($b.Height/40))){ for($x=0;$x -lt $b.Width;$x+=[math]::Max(1,[int]($b.Width/40))){ $c=$b.GetPixel($x,$y); $s+=0.299*$c.R+0.587*$c.G+0.114*$c.B; $n++ } }; [math]::Round($s/[math]::Max(1,$n),1) }
$bmp=New-Object System.Drawing.Bitmap $w.W,$w.Hgt; $g=[System.Drawing.Graphics]::FromImage($bmp); $hdc=$g.GetHdc()
$ok=[MS]::PrintWindow($w.H,$hdc,2); $g.ReleaseHdc($hdc); $g.Dispose(); $method='PrintWindow(PW_RENDERFULLCONTENT)'; $lu=Luma $bmp
if(-not $ok -or $lu -lt 2){
  $pt=New-Object MS+PT; $pt.X=$w.L+[int]($w.W/2); $pt.Y=$w.T+[int]($w.Hgt/2); $top=[MS]::GetAncestor([MS]::WindowFromPoint($pt),2)
  if($top -eq $w.H){ $g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($w.L,$w.T,0,0,$bmp.Size); $g.Dispose(); $method="CopyFromScreen(printwindow ok=$ok luma=$lu)"; $lu=Luma $bmp }
  else { $method="PrintWindow-black(ok=$ok luma=$lu);covered(top=$top)" }
}
if($Out){ $bmp.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png) }; $bmp.Dispose()
W ("mirror-shot: {0} '{1}' {2}x{3} at {4},{5} minimized={6} method={7} mean-luma={8} out={9}" -f $w.Proc,$w.Title,$w.W,$w.Hgt,$w.L,$w.T,$w.Min,$method,$lu,$Out)
