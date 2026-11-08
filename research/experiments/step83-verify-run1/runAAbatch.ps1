# Batch AB (2026-10-29 night): m03k patchy terrain A/B at one fixed camera pose (cam:), cloud:0 / dtex:0 / env:0
$X=$PSScriptRoot
foreach($t in 'P0','P1','P2','P3'){
  & pwsh -NoProfile -File "$X\runAA1.ps1" -Tag $t -Cfg "cfg-$t.cfg" -AP '-Mode Gun -Total 45'
  Start-Sleep 20
}
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AB] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
