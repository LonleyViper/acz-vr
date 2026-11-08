param([string]$Tag='N2')
# Batch N2: briefing zoom probe, cfg-N, brief3d-capture.arm + briefing-observe.arm, null SteamVR -NoDashboard, try/finally restore. Evidence D:\acz-evidence-archive\step115\run$Tag
$X=$PSScriptRoot; $E="D:\acz-evidence-archive\step115\run$Tag"; $n=$Tag; $B="$X\runNbatch.log"; $vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'; $acz="$env:LOCALAPPDATA\acz-vr"
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
function BL($m){ "{0} [{1}] {2}" -f (Get-Date -f HH:mm:ss),$n,$m | Add-Content $B }
try {
  BL 'start'
  Copy-Item "$X\cfg-N.cfg" "$acz\runtime.cfg" -Force
  Get-ChildItem "$acz\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Set-Content "$acz\brief3d-capture.arm" 'n2' -Encoding ascii; Set-Content "$acz\briefing-observe.arm" 'n2' -Encoding ascii
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }; Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  & pwsh -NoProfile -File "$X\runN2route.ps1" -E $E
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3; New-Item -ItemType Directory -Force "$E\logs" | Out-Null
  Get-ChildItem "$acz\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
  Remove-Item "$acz\brief3d-capture.arm","$acz\briefing-observe.arm" -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  BL ("vrsettings restored hash " + (Get-FileHash $vrset).Hash.Substring(0,8)); BL 'N2 done'
}
