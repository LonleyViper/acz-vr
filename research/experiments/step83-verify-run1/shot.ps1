param([string]$Out,[int]$ClickX=-1,[int]$ClickY=-1)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class MC{
[DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
[DllImport("user32.dll")]public static extern void mouse_event(uint f,uint dx,uint dy,uint d,UIntPtr e);
[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();
}
'@
[void][MC]::SetProcessDPIAware()
if($ClickX -ge 0){ [void][MC]::SetCursorPos($ClickX,$ClickY); Start-Sleep -Milliseconds 150; [MC]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [MC]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 400 }
if($Out){
 $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
 $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height; $g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
 $sm=New-Object System.Drawing.Bitmap 1280,720; $g2=[System.Drawing.Graphics]::FromImage($sm); $g2.DrawImage($bmp,0,0,1280,720); $sm.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png)
 "{0}x{1}" -f $b.Width,$b.Height
}
