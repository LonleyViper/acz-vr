# LAUNCH-MODE companion (docs/world-render/launch-mode.md): shows or sets the VR Mode that the dxgi.dll proxy reads at game start,
# the same file the "VR Mode" combo in the game's launcher writes. Use it when the combo is not shown (launch-mode.log says why) or
# with SkipLauncher=True. Never touches runtime.cfg.
#   pwsh -NoProfile -File tools/acz-vr-mode.ps1            # show
#   pwsh -NoProfile -File tools/acz-vr-mode.ps1 -Mode off  # next launch: plain flat game (no XR session, proxies forward only)
#   pwsh -NoProfile -File tools/acz-vr-mode.ps1 -Mode on   # next launch: VR with runtime.cfg as it is (also the default without the file)
param([ValidateSet('show', 'on', 'off')][string]$Mode = 'show')
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA 'acz-vr'
$file = Join-Path $dir 'launch-mode.txt'
if ($Mode -eq 'show') {
    if (Test-Path $file) {
        $line = Get-Content $file | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') -and -not $_.TrimStart().StartsWith(';') } | Select-Object -First 1
        "launch-mode.txt: $line ($file)"
    } else { "launch-mode.txt: missing -> VR On (default)" }
    $log = Join-Path $dir 'logs\launch-mode.log'
    if (Test-Path $log) { "last game start (launch-mode.log):"; Get-Content $log | Select-Object -Last 8 }
    return
}
New-Item -ItemType Directory -Force $dir | Out-Null
$text = "vr=$Mode`r`n# ACE COMBAT ZERO VR mode (on|off), written by tools/acz-vr-mode.ps1 (the launcher's VR Mode combo writes the same file).`r`n" +
        "# on = VR with runtime.cfg as it is; off = the plain flat game (no XR session, the proxies only forward).`r`n" +
        "# Delete this file for the default (on). docs/world-render/launch-mode.md`r`n"
$tmp = "$file.tmp"
[IO.File]::WriteAllText($tmp, $text, [Text.Encoding]::ASCII)
Move-Item -Force $tmp $file
"launch-mode.txt: vr=$Mode (applies at the next game start)"
