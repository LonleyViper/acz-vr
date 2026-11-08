param([ValidateSet('start','stop')][string]$Mode,[string]$E='D:\acz-evidence-archive\step136\batchAG2\calib',[string]$Cfg='D:\acz-evidence-archive\step136\batchAG\runAGF2\run.cfg')
# AG2 calibration helper (2026-10-30): start = null SteamVR + launch game + click Launch (no taps); stop = close game, SteamVR, restore cfg + vrsettings.
$X=$PSScriptRoot; $LD="$env:LOCALAPPDATA\acz-vr"; $rt="$LD\runtime.cfg"; $vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
if($Mode -eq 'start'){
  if(Get-Process acz,vrserver,vrcompositor -ea 0){ throw 'busy' }
  Copy-Item $rt "$E\runtime.cfg.pre-calib" -Force; Copy-Item $Cfg $rt -Force
  Get-ChildItem $LD\logs -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item "$E\steamvr.vrsettings.pre-run-backup" -ea 0
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
  Start-Sleep 8
  Start-Process 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\acz.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
  $p=$null; for($k=0;$k -lt 60 -and -not $p;$k++){ Start-Sleep -Milliseconds 500; $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1 }
  Start-Sleep 5; Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class WV{[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);}'; [void][WV]::SetProcessDPIAware(); $r=New-Object WV+RECT; [void][WV]::GetWindowRect($p.MainWindowHandle,[ref]$r); [void][WV]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 800
  if([math]::Abs($r.L-880) -gt 12 -or [math]::Abs($r.T-380) -gt 12){ "launcher rect $($r.L),$($r.T): abort"; exit 1 }
  pwsh -NoProfile -File "$X\shot.ps1" -ClickX 1368 -ClickY 886 | Out-Null; "launched $(Get-Date -f HH:mm:ss)"
} else {
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0; Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  Copy-Item "$E\runtime.cfg.pre-calib" $rt -Force
  "cfg $((Get-FileHash $rt).Hash.Substring(0,8)) vr $((Get-FileHash $vrset).Hash.Substring(0,8))"
}
