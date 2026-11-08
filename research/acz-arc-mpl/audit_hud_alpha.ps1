# Offline analysis of a matched pair of producer diagnostic PNGs. The colour
# PNG contains sRGB-encoded shaded RGB on black; the alpha PNG stores linear
# FP16 alpha visualised as 8-bit grayscale in RGB. PNG alpha is always opaque.
[CmdletBinding()]
param(
    [string]$Colour = 'research/experiments/step14-live-run11-full/png/acz-vr-png-4132-875315921-s2-hud-on-black.png',
    [string]$Alpha = 'research/experiments/step14-live-run11-full/png/acz-vr-png-4132-875315921-s2-hud-alpha-raw.png',
    [ValidateRange(1,64)][int]$Step = 4
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$colourBitmap = [System.Drawing.Bitmap]::new((Join-Path (Get-Location) $Colour))
$alphaBitmap = [System.Drawing.Bitmap]::new((Join-Path (Get-Location) $Alpha))
try {
    if ($colourBitmap.Width -ne $alphaBitmap.Width -or $colourBitmap.Height -ne $alphaBitmap.Height) { throw 'Diagnostic extents do not match' }
    if ($colourBitmap.Width -ne 2560 -or $colourBitmap.Height -ne 1440) { throw 'Profile rectangles require the recorded 2560x1440 extent' }
    $regions = @(
        @{ name = 'full'; x = 0; y = 0; width = 2560; height = 1440 },
        @{ name = 'radar'; x = 82; y = 868; width = 620; height = 560 },
        @{ name = 'weapons'; x = 2038; y = 900; width = 522; height = 540 }
    )
    $result = @()
    foreach ($region in $regions) {
        [long]$samples = 0; [long]$coloured = 0; [long]$storedZeroColour = 0; [long]$effectiveAboveStored = 0; [double]$maxDelta = 0
        for ($y = $region.y; $y -lt ($region.y + $region.height); $y += $Step) {
            for ($x = $region.x; $x -lt ($region.x + $region.width); $x += $Step) {
                $rgb = $colourBitmap.GetPixel($x, $y)
                $alphaPixel = $alphaBitmap.GetPixel($x, $y)
                if ($alphaPixel.R -ne $alphaPixel.G -or $alphaPixel.R -ne $alphaPixel.B) { throw "Alpha diagnostic is not grayscale at $x,$y" }
                $storedAlpha = $alphaPixel.R / 255.0
                $peak = 0.0
                foreach ($channel in @($rgb.R, $rgb.G, $rgb.B)) {
                    $encoded = $channel / 255.0
                    $linear = if ($encoded -le 0.04045) { $encoded / 12.92 } else { [Math]::Pow(($encoded + 0.055) / 1.055, 2.4) }
                    if ($linear -gt $peak) { $peak = $linear }
                }
                $samples++
                if ($peak -gt 0) { $coloured++; if ($alphaPixel.R -eq 0) { $storedZeroColour++ } }
                $delta = $peak - $storedAlpha
                if ($delta -gt (1.0 / 255.0)) { $effectiveAboveStored++ }
                if ($delta -gt $maxDelta) { $maxDelta = $delta }
            }
        }
        $result += [ordered]@{ region = $region.name; stride = $Step; samples = $samples; coloured = $coloured;
            coloured_with_stored_alpha_zero = $storedZeroColour; rgb_peak_exceeds_stored_alpha = $effectiveAboveStored;
            maximum_linear_rgb_minus_stored_alpha = [Math]::Round($maxDelta, 6) }
    }
    [ordered]@{ source = 'step14-live-run11-full sample s2 diagnostic PNGs'; observations = $result;
        limitations = @('RGB has been clamped, sRGB encoded and quantized; alpha is separately quantized.',
                        'The current consumer uses max(stored alpha, shaded linear RGB); this is not a reconstruction of the original FP16 image.',
                        'These sampled diagnostics do not prove the content or alpha of any other frame.') } | ConvertTo-Json -Depth 5
} finally {
    $colourBitmap.Dispose(); $alphaBitmap.Dispose()
}
