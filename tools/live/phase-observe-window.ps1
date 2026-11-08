# Armed capture windows of the phase observer (gpu_copy_step5_phase_observe, J-phase-observe-findings.md "Armed windows").
#   -Action arm   create/touch %LOCALAPPDATA%\acz-vr\phase-observe.arm  (opens a 20 s window; do it in the unpaused cockpit, then wait >= 2 s)
#   -Action mark  create/touch %LOCALAPPDATA%\acz-vr\phase-observe.mark (right before firing; logs phase-observe-mark)
#   -Action status  show both files (last write time) and the last phase-observe window lines of the step5-producer log
#   -Action clear   remove both files (they are only read while the game runs; a file present at the game's first poll is ignored)
# The DLL never writes or deletes these files. Nothing else is touched.
param([ValidateSet('arm','mark','status','clear')][string]$Action='status')
$dir = Join-Path $env:LOCALAPPDATA 'acz-vr'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$arm = Join-Path $dir 'phase-observe.arm'
$mark = Join-Path $dir 'phase-observe.mark'
function Touch([string]$path) {
    $stamp = [DateTime]::UtcNow.ToString('o')
    Set-Content -Path $path -Value $stamp -Encoding ascii   # new content + new write time on every call
    "{0} touched {1}" -f $stamp, $path
}
switch ($Action) {
    'arm'   { Touch $arm }
    'mark'  { Touch $mark }
    'clear' { Remove-Item -Path $arm, $mark -ErrorAction SilentlyContinue; 'cleared' }
    'status' {
        foreach ($f in @($arm, $mark)) {
            if (Test-Path $f) { "{0}  last-write {1:o}" -f $f, (Get-Item $f).LastWriteTimeUtc } else { "{0}  absent" -f $f }
        }
        $log = Join-Path $dir 'logs\step5-producer.log'
        if (Test-Path $log) {
            Select-String -Path $log -Pattern 'phase-observe-(armed|mark|window-end)|continuous-phase-observe-window' | Select-Object -Last 12 | ForEach-Object { $_.Line }
        }
    }
}
