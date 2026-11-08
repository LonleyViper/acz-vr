param([string]$Out)
# Full-resolution capture of the ACZ game window client area (PrintWindow, no activation), for registered A/B with the XR dump / mirror.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class GS{[StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Rr,B;}[DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out R r);[DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();}'
[void][GS]::SetProcessDPIAware()
$p=Get-Process acz -ea 0 | ? { $_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR' } | select -First 1; if(-not $p){ 'no game window'; exit 2 }
$r=New-Object GS+R; [void][GS]::GetClientRect($p.MainWindowHandle,[ref]$r); $w=$r.Rr; $h=$r.B
$b=New-Object System.Drawing.Bitmap $w,$h; $g=[System.Drawing.Graphics]::FromImage($b); $dc=$g.GetHdc(); $ok=[GS]::PrintWindow($p.MainWindowHandle,$dc,3); $g.ReleaseHdc($dc); $g.Dispose()
$b.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); "gameshot ok=$ok ${w}x$h $Out"
