# Compare two approximate reconstructions against the captured producer's
# HUD-over-world diagnostic. All inputs are quantized sRGB/alpha diagnostics;
# neither reconstruction is a recovery of the original FP16 frame.
[CmdletBinding()]
param([ValidateRange(1,64)][int]$Step = 4)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Join-Path (Get-Location) 'research/experiments/step14-live-run11-full/png'
$prefix = 'acz-vr-png-4132-875315921-s2-'
$sources = @{}
foreach ($suffix in @('hud-on-black','hud-alpha-raw','world','hud-over-world')) {
    $sources[$suffix] = [System.Drawing.Bitmap]::new((Join-Path $root ($prefix + $suffix + '.png')))
}
function DecodeSrgb([double]$value) {
    $v = $value / 255.0
    if ($v -le 0.04045) { return $v / 12.92 }
    return [Math]::Pow(($v + 0.055) / 1.055, 2.4)
}
function EncodeSrgb([double]$value) {
    $v = [Math]::Min(1.0, [Math]::Max(0.0, $value))
    if ($v -le 0.0031308) { return 255.0 * 12.92 * $v }
    return 255.0 * (1.055 * [Math]::Pow($v, 1.0 / 2.4) - 0.055)
}
try {
    foreach ($image in $sources.Values) { if ($image.Width -ne 2560 -or $image.Height -ne 1440) { throw 'Unexpected diagnostic extent' } }
    $regions = @(@{name='radar'; x=82; y=868; w=620; h=560}, @{name='weapons'; x=2038; y=900; w=522; h=540})
    $report = foreach ($region in $regions) {
        [long]$count=0; [double]$effectiveError=0; [double]$storedError=0; [long]$effectiveCloser=0; [long]$storedCloser=0
        for ($y=$region.y; $y -lt $region.y+$region.h; $y += $Step) {
            for ($x=$region.x; $x -lt $region.x+$region.w; $x += $Step) {
                $h=$sources['hud-on-black'].GetPixel($x,$y); $a=$sources['hud-alpha-raw'].GetPixel($x,$y)
                $w=$sources['world'].GetPixel($x,$y); $observed=$sources['hud-over-world'].GetPixel($x,$y)
                $hudLinear=@((DecodeSrgb $h.R),(DecodeSrgb $h.G),(DecodeSrgb $h.B))
                if (($h.R -bor $h.G -bor $h.B) -eq 0) { continue }
                $worldLinear=@((DecodeSrgb $w.R),(DecodeSrgb $w.G),(DecodeSrgb $w.B))
                $actual=@($observed.R,$observed.G,$observed.B)
                $stored=$a.R / 255.0
                $effective=[Math]::Max($stored,[Math]::Max($hudLinear[0],[Math]::Max($hudLinear[1],$hudLinear[2])))
                $errorA=0.0; $errorB=0.0
                for($i=0;$i -lt 3;$i++) {
                    $errorA += [Math]::Abs((EncodeSrgb ($hudLinear[$i]+$worldLinear[$i]*(1-$effective)))-$actual[$i])
                    $errorB += [Math]::Abs((EncodeSrgb ($hudLinear[$i]+$worldLinear[$i]*(1-$stored)))-$actual[$i])
                }
                $count++; $effectiveError += $errorA; $storedError += $errorB
                if ($errorA + 0.01 -lt $errorB) { $effectiveCloser++ } elseif ($errorB + 0.01 -lt $errorA) { $storedCloser++ }
            }
        }
        if ($count -eq 0) { throw "No coloured HUD pixels sampled in $($region.name)" }
        [ordered]@{region=$region.name; sampled_coloured_pixels=$count; mean_effective_error_rgb8=[Math]::Round($effectiveError/($count*3),4);
          mean_stored_error_rgb8=[Math]::Round($storedError/($count*3),4); effective_closer=$effectiveCloser; stored_closer=$storedCloser}
    }
    [ordered]@{stride=$Step; comparison=$report; limitations=@('Both inputs and reference are independently quantized 8-bit diagnostics.',
      'Coloured sample positions are selected by HUD RGB, and errors are mean absolute encoded-channel differences.',
      'This is a comparison to an existing captured diagnostic, not proof of headset behaviour or runtime instrument placement.')} | ConvertTo-Json -Depth 4
} finally { foreach ($image in $sources.Values) { $image.Dispose() } }
