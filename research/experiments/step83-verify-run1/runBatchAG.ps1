param([string[]]$Only=@('flare','boxes','clouds','ui3d'),   # check groups to run, in this fixed order: flare, ui3d, boxes, clouds
      [string]$Root='D:\acz-evidence-archive\step133\batchAG',
      [double]$SunHdg=-2.618,          # m03k light heading -150 deg (baked, notes.md 'm03k sun direction'); -2.269 is m02a (-130 deg): the AC/AD/AF batches used the m02a value on m03k
      [switch]$SkipKill,               # boxes: skip the Kill-mode flight (lock diamond / sat counter)
      [switch]$SkipCloudCam,           # clouds: skip the held-camera eye dump
      [switch]$CloudVertexDump,        # clouds: extra flight with fxsig:4caf6e2d (effects-outside-window.md 16.4 per-draw vertex dump)
      [string]$CloudAppend='',
      [string]$BoxesApAppend='',         # boxes: extra autopilot args for AGB1/AGB2, e.g. '-LookMaxDeg 180 -LookRange 30000'
      [string]$CloudApAppend='',         # clouds: extra autopilot args for AGC1, e.g. '-HeadLookPitch 53'
      [string]$CockpitAppend='',      # every run: extra xr_cockpit_model options, e.g. ';stwatch:1' (aircraft-auto.md 13 launch-signal search)
      [string]$AllAppend='',          # every run: extra terrain options, e.g. ';fxtrail:8' (effects-outside-window.md 19)         # clouds: extra terrain options for AGC1/AGC2, e.g. ';wcloud:1;wcdiag:1' (world-clouds.md section 5)
      [string]$ExpectDxgi='03308883',   # expected installed dxgi.dll hash prefix (step133 default; step134: pass the value from the build record)
      [string]$ExpectD3d12='F32B2E17',   # expected installed d3d12.dll hash prefix
      [switch]$AllowOtherBuild,        # only to run on DLLs that do NOT match -ExpectDxgi/-ExpectD3d12 (normally pass the expected hashes instead)
      [switch]$ExpectFlareOff,         # -Analyze -Kind flarecam: the run was lflare:0 (expect summary-lensflare total-active 0)
      [int]$MaxMemPct=86,              # commit % allowed before a launch (waits up to 5 min, then aborts); the per-run sampler stops a run at 95 % twice / 97 %
      [string]$Analyze,                # analysis only (no launch): an existing run dir, with -Kind
      [ValidateSet('sun','flarecam','boxes','clouds','ui3d','brief3d','debrief','stores','common')][string]$Kind='common',
      [switch]$BriefMap,               # OPT-IN (2026-10-31): ONE run AGM1 that reaches the briefing MAP TABLE (3D overview, then the BRIEFING menu item), holds -BriefHold s on each, png-screen.now every -BriefPngEvery s + one png-eye.now; same as -Only brief
      [int]$BriefHold=30,              # -BriefMap: seconds held on each map screen
      [int]$BriefPngEvery=3,           # -BriefMap: png-screen.now cadence during a hold (the game polls once per second; each trigger writes screen + eye0-table PNGs of ~13 MB each)
      [switch]$BriefOverviewOnly,      # -BriefMap: skip the second leg (mission menu -> BRIEFING -> back -> 3D map)
      [switch]$Debrief,                # OPT-IN (2026-11-01): ONE run DBR1 that flies (-Mode Gun -Total 200), lets the mission END (crash -> REPLAY screen, or the pause-menu QUIT MISSION fallback), leaves the replay with Esc, reaches the post-mission DEBRIEF (dbr3d armed), holds -DebriefHold s with png-screen.now every -DebriefPngEvery s + png-eye.now, pages with confirm; same as -Only debrief
      [int]$DebriefHold=30,            # -Debrief: seconds held on the first debrief page
      [int]$DebriefPngEvery=3,         # -Debrief: png-screen.now cadence during a hold
      [int]$DebriefPageHold=10,        # -Debrief: seconds held on each further page (after a confirm)
      [int]$DebriefMaxPages=4,         # -Debrief: at most this many pages (confirm only while dbr3d is still armed)
      [int]$DebriefEndWait=240,        # -Debrief: seconds to wait after the autopilot for the REPLAY screen before the quit-route fallback
      [int]$DebriefWait=120,           # -Debrief: seconds to wait for dbr3d armed=1 after the replay was left
      [switch]$DebriefQuit,            # -Debrief: do not wait for a crash: Esc pause menu -> QUIT MISSION (highlight verified by image); what follows QUIT is UNVERIFIED
      [int]$DebriefQuitConfirms=0,     # -Debrief -DebriefQuit: extra confirms after QUIT MISSION (a dialog; unknown default item)
      [int]$DebriefProbeConfirm=0,     # -Debrief: guarded confirms on a STATIC unknown screen while waiting for the debrief (0 = none; a mission-failed menu may default to RETRY: a new flight aborts the run)
      [string]$Pair,                   # -Analyze -Kind flarecam: the flare-OFF run dir to compare against
      [switch]$FxAdd,                  # EFFECTS-OUTSIDE (P) docs effects-outside-window.md 21: every run gets ';fxadd:1' (additive 4caf6e2d family + point lists per eye), AGB2 gets png_once
      [int]$FireSalvo=0,               # OPT-IN (2026-10-31, launch-signal search aircraft-auto.md 13): AGB2 fires this many missile taps regardless of lock (autopilot -FireSalvo); use with -CockpitAppend ';stwatch:1' -AllAppend ';vrn:1'
      [double]$FireEvery=3.0,          # -FireSalvo: seconds between taps (the stwatch poll is 250 ms; periodic noise repeats every 15 s, so keep the taps off 15 s multiples)
      [double]$FireAt=60.0,            # -FireSalvo: flight second T of the first tap
      [int]$FireSp=0,                  # OPT-IN (SP-STORES, aircraft-auto.md 15): AGB2 fires this many SPECIAL-weapon taps (HUD SP row, SAAM ---8) after the -FireSalvo taps (autopilot -FireSp); the step172-175 salvos never fired it
      [double]$FireSpEvery=7.0,        # -FireSp: seconds between SP taps
      [double]$FireSpAt=-1,            # -FireSp: flight second of the first SP tap; -1 = the last -FireSalvo tap + 10 s (60 without -FireSalvo)
      [string]$SpSwitchKey='',         # -FireSp: the in-flight special-weapon select key ('E' or '0x45'); UNKNOWN for this game: read the in-game Controls screen first. Empty = no switch tap (the taps then fire whatever is selected: the analysis tells MISSILE from SAAM by the HUD counts)
      [double]$StoreWindow=1.5,        # -Analyze -Kind stores: a decrement matches a tap when it comes 0..this many seconds after it
      [switch]$TestHmd,                # (2026-11-02) run on the turnable TEST HMD driver acz_testhmd instead of driver_null; head looks turn the REAL runtime pose (autopilot -HeadPose, no nh:1). docs/world-render/test-hmd-driver.md
[switch]$KeyLook,                # OLD look method for boxes/clouds: press the GAME's numpad look keys (turns the game camera AND the forward window). Default (2026-11-02):
                                       # -HeadLook = the synthetic HMD head turn (nh:1 + head.now, docs/world-render/null-head-look.md): the game camera stays forward, the
                                       # turned view must come from our own rendering; these runs also get xr_png_once (-Png) so the head-turn eye dumps fire.
      [switch]$NoCloudTrigger,         # clouds: DEFAULT ON (2026-10-31): cloudtrig.ps1 watches step5-producer.log and writes png-eye.now (+ png-screen.now) when off-window cloud cards are live, during/after a look tap; this switch restores the old time-based dump only
      [int]$CloudDumps=6,              # cloudtrig: max trigger dumps per flight
      [double]$CloudDumpGap=10,        # cloudtrig: min seconds between dumps
      [switch]$CloudDip,               # OPT-IN: AGC1 dips to -CloudDipAlt from flight second -CloudDipFrom for -CloudDipFor s (autopilot -DipAlt), to fly through the 5000-6000 decks
      [double]$CloudDipAlt=5800,       # <= 5800 (the ALT dead band of 700 u keeps it below the y~6500 blind limit) and >= 4500 (terrain / AltFloor 2500)
      [double]$CloudDipFrom=90,
      [double]$CloudDipFor=120,        # the descent from the 11000 target (<= 150 u/s) takes ~35 s of this
      [switch]$WhatIf)
# Batch AG (2026-10-30): ONE unattended null-HMD live batch on the INSTALLED day build step133 (no DLL change) for the features merged 2026-10-29/30 that were never live-verified:
#   FLARE  head-look lens flare (docs/world-render/lens-flare.md)            runs AGF1 (autopilot Sun zoom), AGF2/AGF3 (held sun camera flare ON / lflare:0), AGF4 (only if AGF2 shows no flare: other yaw sign)
#   UI3D   menus / hangar room / in-flight pause 3D (docs/world-render/ui-3d.md) + debrief census   run AGU1  (cfg = day + ;ui3d:1;u3p:1;u3a:1 = the CURRENTLY installed user-check cfg)
#   BOXES  NBOX outside the forward rectangle + lock diamond grouping + roll (markers-360.md 12-13)  runs AGB1 (Gun + look-at-contacts), AGB2 (Kill: lock)
#   CLOUDS non-indexed GREATER quads / explosion ordering (effects-outside-window.md 15-16)          runs AGC1 (cruise with look taps), AGC2 (held camera, eye dump), [AGC3 vertex dump]
# Every run: its own launch + cfg built from cfg-U.cfg (the day cfg, hash 1AE3C990; the 255-char terrain value and the 32-key limit are CHECKED before a launch), null SteamVR on/off
# (vrsettings backup/restore per run, byte-identical), sampler (memory stop rule), killcap (1 fps mirror + game shots), evidence in $Root\run<Tag>, an analysis block appended to
# $Root\RESULTS.txt right after the run. The runtime.cfg that was INSTALLED before the batch (it may carry ;ui3d:1;u3p:1;u3a:1) is byte-copied to $Root\runtime.cfg.pre-batch and
# restored exactly (hash checked) in the outer finally block, also after an abort. Nothing here edits an existing script (runK3ag.ps1 = runK3.ps1 + -PostWait).
# Usage:  pwsh -NoProfile -File runBatchAG.ps1                       (all groups, ~75-90 min)    -Only flare,ui3d (subset)    -WhatIf (plan + cfg checks only, launches nothing)
#         pwsh -NoProfile -File runBatchAG.ps1 -Analyze <runDir> -Kind sun|flarecam|boxes|clouds|ui3d [-Pair <flare-off runDir>]   (re-run an analysis, launches nothing)
$ErrorActionPreference='Stop'
if($BriefMap -and $Only -notcontains 'brief'){ $Only=@('brief') }
if($Debrief -and $Only -notcontains 'debrief'){ $Only=@('debrief') }   # -Debrief alone = the debrief run only   # -BriefMap alone = the briefing-map run only (it never flies)
if($FxAdd -and $AllAppend -notmatch 'fxadd:'){ $AllAppend+=';fxadd:1' }   # (P) the FXADD checks run on every run whose xr-stereo.log has the fx-additive-option line
$X=$PSScriptRoot; $Repo=(Resolve-Path (Join-Path $X '..\..\..')).Path
$LD="$env:LOCALAPPDATA\acz-vr"; $rt="$LD\runtime.cfg"
$vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$GameDir='D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$drv=(Resolve-Path (Join-Path $Repo 'tools\live\acz-window-driver.ps1')).Path
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
public static class AGImg {
  // a = BGRA bytes (Format32bppArgb, stride w*4)
  // {w, h, white (min(R,G,B) >= 235), warm-bright (R >= 220 && G >= 190 && B < 235), maxLuma*1000, white-centroid x, y}
  public static double[] Stats(byte[] a, int w, int h) { long white = 0, warm = 0; double sx = 0, sy = 0, mx = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = y * w * 4 + x * 4; int bl = a[i], g = a[i+1], r = a[i+2]; int mn = Math.Min(r, Math.Min(g, bl));
      double l = (0.2126*r + 0.7152*g + 0.0722*bl); if (l > mx) mx = l;
      if (mn >= 235) { white++; sx += x; sy += y; } if (r >= 220 && g >= 190 && bl < 235) warm++; }
    return new double[] { w, h, white, warm, mx * 1000, white > 0 ? sx / white : -1, white > 0 ? sy / white : -1 }; }
  // {w, h, pixels with luma > thr, bbox x0,y0,x1,y1, maxLuma*1000}  (non-black content of a dump)
  public static double[] Lit(byte[] a, int w, int h, int thr) { long n = 0; int x0 = w, y0 = h, x1 = -1, y1 = -1; double mx = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = y * w * 4 + x * 4; double l = 0.2126*a[i+2] + 0.7152*a[i+1] + 0.0722*a[i]; if (l > mx) mx = l;
      if (l > thr) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; } }
    return new double[] { w, h, n, x0, y0, x1, y1, mx * 1000 }; }
  // {w, h, pixels whose summed |dRGB| > thr, bbox x0,y0,x1,y1 of those pixels}
  public static double[] Diff(byte[] a, byte[] b, int w, int h, int thr) { long n = 0; int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int i = y * w * 4 + x * 4; int d = Math.Abs(a[i]-b[i]) + Math.Abs(a[i+1]-b[i+1]) + Math.Abs(a[i+2]-b[i+2]);
      if (d > thr) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; } }
    return new double[] { w, h, n, x0, y0, x1, y1 }; }
}
'@
function Load-Px([string]$p){ $b=New-Object Drawing.Bitmap $p; try { $w=$b.Width; $h=$b.Height; $d=$b.LockBits((New-Object Drawing.Rectangle 0,0,$w,$h),[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $a=New-Object byte[] ($d.Stride*$h); [Runtime.InteropServices.Marshal]::Copy($d.Scan0,$a,0,$a.Length); $b.UnlockBits($d); [pscustomobject]@{w=$w;h=$h;a=$a} } finally { $b.Dispose() } }
function Img-Stats([string]$p){ $i=Load-Px $p; [AGImg]::Stats($i.a,$i.w,$i.h) }
function Img-Diff([string]$pa,[string]$pb,[int]$thr){ $a=Load-Px $pa; $b=Load-Px $pb; if($a.w -ne $b.w -or $a.h -ne $b.h){ return @($a.w,$a.h,-1,0,0,0,0) }; [AGImg]::Diff($a.a,$b.a,$a.w,$a.h,$thr) }

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- helpers
function BL($m){ $line="{0} [AG] {1}" -f (Get-Date -f HH:mm:ss),$m; Write-Host $line; if(-not $WhatIf -and $script:Root_ok){ Add-Content "$Root\batchAG.log" $line } }
$script:Out=New-Object System.Collections.Generic.List[string]
function Say($m){ $script:Out.Add($m); Write-Host $m }
function Chk([string]$state,[string]$name,[string]$detail){ Say ("  [{0}] {1}: {2}" -f $state,$name,$detail) }
function Read-Matching([string]$f,[string]$rx){ $o=New-Object System.Collections.Generic.List[string]
  if(-not (Test-Path -LiteralPath $f)){ return ,$o }
  try { $fs=[IO.File]::Open($f,'Open','Read','ReadWrite,Delete'); $sr=New-Object IO.StreamReader($fs,[Text.Encoding]::Latin1); while($null -ne ($l=$sr.ReadLine())){ if($l -match $rx){ $o.Add($l) } }; $sr.Dispose() } catch { }
  ,$o }
function Get-Kv([string]$line){ $h=@{}; if($line){ foreach($p in ($line -split ';')){ $i=$p.IndexOf('='); if($i -gt 0){ $h[$p.Substring(0,$i)]=$p.Substring($i+1) } } }; $h }
function Kn($h,[string]$k){ $v=$h[$k]; if($null -eq $v){ return -1.0 }; $m=[regex]::Match([string]$v,'^-?[\d.]+'); if($m.Success){ [double]$m.Value } else { -1.0 } }
function Kvmax($lines,[string]$k){ $m=-1.0; foreach($l in $lines){ $v=Kn (Get-Kv $l) $k; if($v -gt $m){ $m=$v } }; $m }
function Rot180([string]$in,[string]$out){ try { $i=[Drawing.Bitmap]::FromFile($in); $i.RotateFlip([Drawing.RotateFlipType]::Rotate180FlipNone); $i.Save($out,[Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); $true } catch { $false } }

# cfg builder: base = cfg-U.cfg (the day cfg); appends to xr_terrain_stage; -Png swaps xr_edge_feather for xr_png_once (the 32-key cfg is full; diagnostic_ownership_pins stays);
# -FxSig replaces the flare/fx signature. Validates the 255-char terrain buffer (margin: 250) and the 32-key limit.
$BaseCfgPath="$X\cfg-U.cfg"; $script:BaseCfg=[IO.File]::ReadAllText($BaseCfgPath)
function New-RunCfg([string]$Append='',[switch]$Png,[string]$FxSig=''){
  $t=$script:BaseCfg
  $Append=$AllAppend+$Append
  if($Append){ $t=[regex]::Replace($t,'(?m)^(xr_terrain_stage=[^\r\n]*)',{ param($m) $m.Groups[1].Value+$Append }) }
  if($CockpitAppend){ $t=[regex]::Replace($t,'(?m)^(xr_cockpit_model=[^\r\n]*)',{ param($m) $m.Groups[1].Value+$CockpitAppend }) }
  $cm=([regex]::Match($t,'(?m)^xr_cockpit_model=([^\r\n]*)')).Groups[1].Value
  if($cm.Length -ge 100){ throw "xr_cockpit_model value is $($cm.Length) chars: step168 and older DLLs refuse >= 100 (step170+: 192) and then the WHOLE config is refused (step169 runs had no logs)" }
  if($FxSig){ $t=$t -replace 'fxsig:[0-9a-fA-F]+',"fxsig:$FxSig" }
  if($Png){ if($t -notmatch 'xr_edge_feather=0\.06'){ throw 'cfg-U has no xr_edge_feather=0.06 to swap' }; $t=$t -replace 'xr_edge_feather=0\.06','xr_png_once=true' }
  $terr=([regex]::Match($t,'(?m)^xr_terrain_stage=([^\r\n]*)')).Groups[1].Value
  if($terr.Length -gt 1000){ throw "terrain value $($terr.Length) chars > 1000 (buffer 1023 since step158)" }
  $keys=@($t -split "`r?`n" | ? { $_ -match '^[A-Za-z0-9_]+=' }).Count
  if($keys -gt 32){ throw "cfg has $keys keys > 32" }
  if($t -notmatch 'diagnostic_ownership_pins=true'){ throw 'diagnostic_ownership_pins missing (the producer needs it)' }
  $t }
function Hash8([string]$f){ if(Test-Path -LiteralPath $f){ (Get-FileHash -LiteralPath $f).Hash.Substring(0,8) } else { '--------' } }

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- run plan
$AP_sun  ="-Mode Sun -SunZoom -SunHdg $SunHdg -ZoomPitch 0.7 -ZoomBase 3500 -ZoomTop 4600 -AltCeil 5800 -SunAltMax 6000 -SunStartAlt 3500 -NoArmFx -FreezeMax 120 -Total 480"
$AP_cam  ='-Mode Cruise -Total 40'
$AP_gun  ='-Mode Gun -Total 300 -LookAtContacts -LookRange 14000 -LookMinDeg 45 -LookMaxDeg 115 -LookHold 3'
$AP_kill ='-Mode Kill -Total 300 -LookAtContacts -LookRange 14000'
if($FireSalvo -gt 0){ $AP_kill+=(' -FireSalvo {0} -FireEvery {1} -FireAt {2}' -f $FireSalvo,$FireEvery.ToString([Globalization.CultureInfo]::InvariantCulture),$FireAt.ToString([Globalization.CultureInfo]::InvariantCulture)); if($FireAt+($FireSalvo-1)*$FireEvery+10 -gt 290){ throw "FireSalvo $FireSalvo x $FireEvery s from T=$FireAt does not fit the 300 s flight" } }
if($FireSp -gt 0){
  $ci=[Globalization.CultureInfo]::InvariantCulture
  $spAt=$(if($FireSpAt -ge 0){ $FireSpAt }elseif($FireSalvo -gt 0){ $FireAt+($FireSalvo-1)*$FireEvery+10.0 }else{ 60.0 })
  if($spAt+($FireSp-1)*$FireSpEvery+10 -gt 290){ throw "FireSp $FireSp x $FireSpEvery s from T=$spAt does not fit the 300 s flight" }
  if($FireSalvo -gt 0 -and $spAt -lt $FireAt+($FireSalvo-1)*$FireEvery+3.0){ throw "FireSp starts at T=$spAt, within 3 s of the last -FireSalvo tap: the HUD counts cannot be told apart" }
  if($SpSwitchKey -and $SpSwitchKey -notmatch '^(0[xX][0-9a-fA-F]{1,2}|[A-Za-z0-9])$'){ throw "SpSwitchKey '$SpSwitchKey' is not a single character or a 0x.. virtual-key code" }
  $AP_kill+=(' -FireSp {0} -FireSpEvery {1} -FireSpAt {2}' -f $FireSp,$FireSpEvery.ToString($ci),$spAt.ToString($ci)); if($SpSwitchKey){ $AP_kill+=" -SpSwitchKey $SpSwitchKey" }
}
$AP_cloud='-Mode Cruise -Total 330 -TargetAlt 11000 -AltCeil 13000 -LookEvery 20 -LookHold 3 -LookStart 40'   # D-404: m03k puffs sit at y 8.8k-13.2k (wcdiag census), not the 5000-6000 layer field
# NULL-HEAD (2026-11-02): boxes and clouds look by turning the synthetic HMD head (autopilot -HeadLook writes head.now; cfg nh:1), not the game camera; -KeyLook = the old numpad keys.
$nhCfg=$(if($KeyLook -or $TestHmd){ '' }else{ ';nh:1' }); $headPng=(-not $KeyLook)
if(-not $KeyLook){ $AP_gun+=' -HeadLook'; $AP_kill+=' -HeadLook'; $AP_cloud+=' -HeadLook' }
if(-not $KeyLook -and $TestHmd){ $AP_gun+=' -HeadPose'; $AP_kill+=' -HeadPose'; $AP_cloud+=' -HeadPose' }
if($CloudApAppend){ $AP_cloud+=' '+$CloudApAppend }
if($BoxesApAppend){ foreach($k in ([regex]::Matches($BoxesApAppend,'-(\w+)') | % { $_.Groups[1].Value })){ $AP_gun=[regex]::Replace($AP_gun,"\s-$k(\s+[^\s-][^\s]*)?",''); $AP_kill=[regex]::Replace($AP_kill,"\s-$k(\s+[^\s-][^\s]*)?",'') }; $AP_gun+=' '+$BoxesApAppend; $AP_kill+=' '+$BoxesApAppend }   # appended switches override the same-named defaults
if($CloudDip){
  if($CloudDipAlt -lt 4500 -or $CloudDipAlt -gt 5800){ throw "CloudDipAlt $CloudDipAlt outside 4500..5800 (terrain / y~6500 aircraft-struct blind limit, playbook section 7)" }
  if($CloudDipFrom -lt 40 -or $CloudDipFrom+$CloudDipFor -gt 300){ throw 'cloud dip must start after the first look tap (T>=40) and end by T=300 of the 330 s flight' }
  $AP_cloud+=(' -DipAlt {0} -DipFrom {1} -DipFor {2}' -f $CloudDipAlt.ToString([Globalization.CultureInfo]::InvariantCulture),$CloudDipFrom.ToString([Globalization.CultureInfo]::InvariantCulture),$CloudDipFor.ToString([Globalization.CultureInfo]::InvariantCulture)) }
$AP_ui   ='-Mode Gun -Total 200'
$capDef  ='-Slow 1 -Fast 0.5 -FastFor 40 -GameSlow 2 -GameFast 1'
$camOn   =';cam:107113,9000,108136,-150,50'        # cfg-F2cam: m03k light heading -150 deg, elevation 50 -> yaw -150 pitch +50 looks AT the sun (notes.md 'm03k sun direction')
$plan=@()
if($Only -contains 'flare'){
  $plan+=@{Tag='AGF2';Group='flare';Kind='flarecam';Desc='held sun camera, lens flare ON (default)';Cfg=(New-RunCfg -Append $camOn -Png);AP=$AP_cam;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=6}
  $plan+=@{Tag='AGF3';Group='flare';Kind='flarecam';Desc='held sun camera, lflare:0 (A/B reference)';Cfg=(New-RunCfg -Append ($camOn+';lflare:0') -Png);AP=$AP_cam;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=6}
  $plan+=@{Tag='AGF1';Group='flare';Kind='sun';Desc="autopilot Sun zoom toward heading $SunHdg, el 50 deg, ceiling 5800 (y <= ~6500)";Cfg=(New-RunCfg);AP=$AP_sun;Cap='-Slow 1 -Fast 0.5 -FastFor 12 -GameSlow 2 -GameFast 1';MaxMin=12}
  $plan+=@{Tag='AGF4';Group='flare';Kind='flarecam';Desc='CONDITIONAL (only if AGF2 vs AGF3 shows no flare): sun camera with the mirrored yaw sign';Cfg=(New-RunCfg -Append ';cam:107113,9000,108136,150,50' -Png);AP=$AP_cam;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=6;Conditional=$true}
}
if($Only -contains 'ui3d'){
  $plan+=@{Tag='AGU1';Group='ui3d';Kind='ui3d';Desc='menus + hangar room + in-flight pause (2 x 20 s) + post-flight debrief census, cfg = day + ui3d:1;u3p:1;u3a:1';Cfg=(New-RunCfg -Append ';ui3d:1;u3p:1;u3a:1');AP=$AP_ui;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=16;Menu=$true;Pause=$true;PostWait=150}
}
if($Only -contains 'brief'){
  # AGM1: the user's real feature set needs vrn:1 (playbook 2026-10-31; runBatchAG's base cfg has none) unless -AllAppend already carries it; xr_png_once replaces xr_edge_feather so png-eye.now can re-arm an eye dump.
  $vrn=$(if($AllAppend -match 'vrn:'){ '' } else { ';vrn:1' })
  $plan+=@{Tag='AGM1';Group='brief';Kind='brief3d';Desc="briefing MAP TABLE: 3D overview then BRIEFING menu item, hold $BriefHold s each, png-screen.now every $BriefPngEvery s + one png-eye.now, no flight";Cfg=(New-RunCfg -Append $vrn -Png);AP='-Mode Cruise -Total 40';Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3 -StartPattern BRIEFMAP.hold.start';MaxMin=12;BriefMap=$true}
}
if($Only -contains 'debrief'){
  # DBR1: vrn:1 expands to dbr3d:1;ui3d:1;... (2026-10-31 handoff); xr_png_once replaces xr_edge_feather so png-eye.now can dump the eye images. The flight is the AGU1 flight that ended in a crash (-Mode Gun -Total 200).
  $vrn=$(if($AllAppend -match 'vrn:'){ '' } else { ';vrn:1' })
  $plan+=@{Tag='DBR1';Group='debrief';Kind='debrief';Desc="post-mission DEBRIEF (dbr3d): Gun flight 200 s, mission end (crash replay or pause-menu quit), Esc out of the replay, hold $DebriefHold s with png-screen.now every $DebriefPngEvery s + png-eye.now, page with confirm";Cfg=(New-RunCfg -Append $vrn -Png);AP=$AP_ui;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=40;Menu=$false;Debrief=$true}
}
if($Only -contains 'boxes'){
  $plan+=@{Tag='AGB1';Group='boxes';Kind='boxes';Desc="Gun + look-at-contacts $(if($KeyLook){'(numpad keys)'}else{'(HEAD turn, game camera forward)'}) (NBOX outside the window, roll while banking)";Cfg=(New-RunCfg -Append $nhCfg -Png:$headPng);AP=$AP_gun;Cap=$capDef;MaxMin=14}
  if(-not $SkipKill){ $plan+=@{Tag='AGB2';Group='boxes';Kind='boxes';Desc="Kill (missiles, lock diamond grouping ``sat``) + look-at-contacts $(if($KeyLook){'(numpad keys)'}else{'(HEAD turn)'})";Cfg=(New-RunCfg -Append $nhCfg -Png:($FxAdd -or $headPng));AP=$AP_kill;Cap=$capDef;MaxMin=14} }   # -FxAdd: png_once for the fx-additive eye dump at the first explosion
}
if($Only -contains 'clouds'){
  $plan+=@{Tag='AGC1';Group='clouds';Kind='clouds';Desc='cruise through the m03k cloud decks (5000-6000) with look-left/right taps; png_once (swap for xr_edge_feather) at flight frame 240, fxsig:4caf6e2d (effects-outside-window.md 17.5)';Cfg=(New-RunCfg -Append ($CloudAppend+$nhCfg) -Png -FxSig '4caf6e2d');AP=$AP_cloud;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 2';MaxMin=12;CloudTrig=(-not $NoCloudTrigger)}   # CloudTrig: telemetry-triggered eye dumps (cloudtrig.ps1), gated on look taps
  if(-not $SkipCloudCam){ $plan+=@{Tag='AGC2';Group='clouds';Kind='cloudcam';Desc='BEST EFFORT held camera 80 deg off the nose, pitched down onto the decks, eye dump (pose is a guess)';Cfg=(New-RunCfg -Append ";cam:101000,20000,101500,125,-35$CloudAppend" -Png);AP=$AP_cam;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 3';MaxMin=6;CloudTrig=(-not $NoCloudTrigger);CloudNoLook=$true} }   # held camera: no look taps, so no look gate
  if($CloudVertexDump){ $plan+=@{Tag='AGC3';Group='clouds';Kind='clouds';Desc='fxsig:4caf6e2d per-draw vertex dump of the cut cloud quads (16.4)';Cfg=(New-RunCfg -Append $nhCfg -FxSig '4caf6e2d');AP=$AP_cloud;Cap='-Slow 1 -Fast 1 -FastFor 0 -GameSlow 2';MaxMin=12} }
}

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- analysis
function Analyze-Common([string]$E){
  $xl="$E\logs\xr-stereo.log"
  Say "  --- common ($E)"
  if(-not (Test-Path $xl)){ Chk 'FAIL' 'logs' 'no logs\xr-stereo.log copied: the run produced no evidence'; return }
  $st=Read-Matching $xl 'flight-stage-installed;flight=\d+;stage=(\w+)'; $stage=''; if($st.Count){ $stage=[regex]::Match($st[$st.Count-1],'stage=(\w+)').Groups[1].Value }
  Chk $(if($stage -eq 'm03k'){'PASS'}elseif($stage -or $script:AKind -eq 'brief3d'){'INFO'}else{'FAIL'}) 'flight stage' "'$stage' ($($st.Count) installs; the cam:/SunHdg constants assume m03k; for m02a use -SunHdg -2.269)"
  $script:NoFlight=(-not $stage)
  if($script:NoFlight -and $script:AKind -eq 'brief3d'){ Chk 'INFO' 'no flight' 'expected: the briefing-map route stays in the menus (a flight-stage-detect line would be a bug: steps.txt MISSION-STARTED)' }
  elseif($script:NoFlight){   # AGF2/AGF3 2026-10-30 13:22: the menu route never left the first screen (Controls help page), so there was no flight, no composite, no producer log
    $stp=@(Get-Content "$E\steps.txt" -ea 0 | ? { $_ -match 'no flight detected|ROUTE FAILED|no launcher|abort|Controls screen' })
    $sz=@('02-cinematic','03-overview','04-mission-menu','05-loaded' | % { $f="$E\shots\$_.png"; if(Test-Path $f){ (Get-Item $f).Length } }); $stuck=$false
    if($sz.Count -ge 3){ $mn=($sz | measure -Minimum).Minimum; $mx=($sz | measure -Maximum).Maximum; $stuck=(($mx-$mn) -lt 0.01*$mx) }
    $ws=Read-Matching $xl 'event=waiting-for-source'; $nw=@(Read-Matching $xl 'event=summary;t-ms=' | % { Kn (Get-Kv $_) 'new' } | ? { $_ -gt 0 }).Count
    Chk 'FAIL' 'FLIGHT NEVER STARTED' ("no flight-stage-installed; XR summary windows with new>0: $nw; waiting-for-source lines: $($ws.Count); route screenshots 02-05 sizes $($sz -join '/') -> " + $(if($stuck){'IDENTICAL = the menu route is stuck on its first screen (look at shots\02-cinematic.png: the Controls help page means the taps never reached the game or the screen needs a key); '}else{'differ; '}) + "steps: $($stp -join ' | '). Everything below is meaningless for the feature under test.")
  }
  $sm=Read-Matching $xl 'event=summary;t-ms='; $fr=@($sm | % { Kn (Get-Kv $_) 'frames' } | ? { $_ -ge 0 } | sort); $med= if($fr.Count){ $fr[[int]($fr.Count/2)] } else { 0 }
  Chk $(if($med -ge 300){'PASS'}elseif($med -gt 0){'WARN'}else{'FAIL'}) 'XR rate (event=summary frames per 5 s, median)' "$med over $($fr.Count) windows (healthy ~600; ~160 = the 32 Hz null runtime: mirror frames are then UNRELIABLE, pre-compositor dumps are fine)"
  $dc=Read-Matching $xl 'event=summary-decouple'; $cps=@($dc | % { Kn (Get-Kv $_) 'composites-per-s' } | ? { $_ -ge 0 } | sort); $cm= if($cps.Count){ $cps[[int]($cps.Count/2)] } else { 0 }
  Chk $(if($cm -ge 100){'PASS'}elseif($cm -gt 0){'WARN'}else{'FAIL'}) 'composites-per-s (summary-decouple, median)' "$cm (healthy ~120; ~33 = the degraded null runtime of the 2026-10-29 night: judge mirror frames with care)"
  $pl="$E\logs\step5-producer.log"; Chk $(if((Test-Path $pl) -and (Get-Item $pl).Length -gt 0){'PASS'}else{'FAIL'}) 'step5-producer.log' $(if(Test-Path $pl){ "$((Get-Item $pl).Length) bytes" } else { 'missing (diagnostic_ownership_pins?)' })
  $xs=Read-Matching $xl 'event=diagnostics-config'; $tdis=Read-Matching $xl 'terrain-disabled'; Chk $(if($tdis.Count -eq 0){'PASS'}else{'FAIL'}) 'terrain-disabled' "$($tdis.Count) lines $(if($tdis.Count){ $tdis[0].Substring(0,[math]::Min(200,$tdis[0].Length)) })"
  if(Test-Path "$E\GAMEOVER"){ Chk 'FAIL' 'memory stop rule' "GAMEOVER marker: $((Get-Content "$E\GAMEOVER" -Raw).Trim()) (>= 95 % twice or 97 %)" }
  if(Test-Path "$E\steps.txt"){ $ex=@(Get-Content "$E\steps.txt" | ? { $_ -match 'autopilot \d exit=|GAMEOVER|acz gone|no launcher|abort' }); if($ex){ Say ("  [INFO] steps: " + ($ex -join ' | ')) } }
  $nhc=Read-Matching $xl 'event=null-head-config'   # NULL-HEAD (autopilot -HeadLook): the synthetic head turns, the game camera stays forward
  if($nhc.Count){ $nhk=Read-Matching $xl 'event=null-head;stage=command'; $nhr=Read-Matching $xl 'event=null-head;stage=refused'; $nhx=Read-Matching $xl 'event=null-head;stage=reached'; $nhs=Read-Matching $xl 'event=summary-null-head'
    $hsh=@(Get-Content "$E\autopilot-1.log" -ea 0 | ? { $_ -match 'HEADSHOT png-eye' }); $hlk=@(Get-Content "$E\autopilot-1.log" -ea 0 | ? { $_ -match ' HEADLOOKC? ' })
    Chk $(if($nhx.Count -gt 0 -and $nhr.Count -eq 0){'PASS'}elseif($hlk.Count -eq 0){'INFO'}else{'FAIL'}) 'NULL-HEAD head turns (nh:1, head.now)' ("config: $([regex]::Match($nhc[0],'ok=\d;null-head=\d;slew-dps=\d+').Value); autopilot head looks $($hlk.Count), DLL commands $($nhk.Count), refused $($nhr.Count), reached $($nhx.Count), summaries $($nhs.Count), HEADSHOT eye dumps $($hsh.Count). " +
      "[LOOK] upright\*-final.png of the HEADSHOT dumps and the mirror at the HEADLOOK times: the forward window must sit OFF-centre (or out of view) and the turned view must be our own world/boxes/clouds/cockpit, not the game camera") }
  $kc=@(Get-ChildItem "$E\mirror" -ea 0).Count; $gc=@(Get-ChildItem "$E\gameshots" -ea 0).Count; Say "  [INFO] captures: mirror $kc, game $gc, eye png dumps $(@(Get-ChildItem "$E\logs\png" -Filter *.png -ea 0).Count)"
  # upright copies of every png dump (xr_png_once / png-screen are stored rotated 180 degrees)
  $pd="$E\logs\png"; if(Test-Path $pd){ New-Item -ItemType Directory -Force "$E\upright" | Out-Null; $n=0; foreach($f in (Get-ChildItem $pd -Filter *.png | sort Name)){ if($n -ge 100){ break }; if(Rot180 $f.FullName "$E\upright\$($f.Name)"){ $n++ } }; Say "  [INFO] upright copies: $n in $E\upright" }
}
function Wall-From([string]$E,[double]$T){ # wall clock of autopilot T seconds (steps.txt 'autopilot start' stamp)
  $s=@(Get-Content "$E\steps.txt" -ea 0 | ? { $_ -match '^(\d\d):(\d\d):(\d\d)\.\d+ autopilot start' } | select -First 1); if(-not $s){ return $null }
  $m=[regex]::Match($s[0],'^(\d\d):(\d\d):(\d\d)'); (New-TimeSpan -Hours $m.Groups[1].Value -Minutes $m.Groups[2].Value -Seconds $m.Groups[3].Value).Add([TimeSpan]::FromSeconds($T)) }
function Near-Mirror([string]$E,$wall,[int]$n=2){ if($null -eq $wall){ return '' }; $best=@(Get-ChildItem "$E\mirror" -ea 0 | % { $m=[regex]::Match($_.Name,'-(\d\d)(\d\d)(\d\d)'); if($m.Success){ $t=(New-TimeSpan -Hours $m.Groups[1].Value -Minutes $m.Groups[2].Value -Seconds $m.Groups[3].Value); [pscustomobject]@{n=$_.Name;d=[math]::Abs(($t-$wall).TotalSeconds)} } } | sort d | select -First $n); ($best | % { $_.n }) -join ',' }
# autopilot 'pos=x,y,z' is printed with thousands separators ('104,457,20,000,105,481'): split the tokens into three numbers, y plausible (0..60000)
function Pos-Y([string]$s){ $m=[regex]::Match($s,'pos=([-\d,]+) h='); if(-not $m.Success){ return $null }; $t=@($m.Groups[1].Value -split ','); $n=$t.Count
  function Okn($g){ if($g.Count -lt 1 -or $g[0] -notmatch '^-?\d{1,3}$'){ return $false }; for($k=1;$k -lt $g.Count;$k++){ if($g[$k] -notmatch '^\d{3}$'){ return $false } }; $true }
  $best=$null   # several splits can be valid ('-790,6,385,105,703' = y 6 or 6385): the LARGEST plausible y is the real one (a split that makes y part of x/z gives a tiny y)
  for($i=1;$i -lt $n-1;$i++){ for($j=$i+1;$j -lt $n;$j++){ $a=@($t[0..($i-1)]); $b=@($t[$i..($j-1)]); $c=@($t[$j..($n-1)])
      if((Okn $a) -and (Okn $b) -and (Okn $c)){ $y=[double]($b -join ''); if($y -ge 0 -and $y -le 60000 -and ($null -eq $best -or $y -gt $best)){ $best=$y } } } }
  $best }
function Analyze-Sun([string]$E){
  Say "  --- SUN autopilot flight (lens flare, head forward; the flare needs the sun inside the eye image)"
  $al="$E\autopilot-1.log"; if(-not (Test-Path $al)){ Chk 'FAIL' 'autopilot log' 'missing'; return }
  $L=@(Get-Content $al); $rows=@($L | % { $m=[regex]::Match($_,'^T=\s*([\d.]+) pos=.* sunRelBrg=(-?\d+) sunRelEl=(-?\d+) ph=(\d)'); if($m.Success){ [pscustomobject]@{T=[double]$m.Groups[1].Value;y=(Pos-Y $_);brg=[double]$m.Groups[2].Value;el=[double]$m.Groups[3].Value;ph=[int]$m.Groups[4].Value} } })
  $zf=@($L | ? { $_ -match 'ZOOM FIRE' }); Chk $(if($zf.Count){'PASS'}else{'FAIL'}) 'zoom climbs fired' "$($zf.Count) 'ZOOM FIRE' lines (0 = never aligned on the sun heading: sim says the first alignment is >T150 s; a cutscene freeze at T110-160 ends many m03k flights)"
  $inView=@($rows | ? { [math]::Abs($_.brg) -le 25 -and [math]::Abs($_.el) -le 30 }); Chk $(if($inView.Count){'PASS'}else{'FAIL'}) 'sun inside +-25 deg bearing / +-30 deg elevation of the nose (decision rows)' "$($inView.Count) of $($rows.Count) rows"
  if($rows.Count){ $mx=($rows | measure y -Maximum).Maximum; $mn=($rows | measure y -Minimum).Minimum; Chk 'INFO' 'altitude y' "min $mn max $mx (the ee-camera struct reads zero above ~6500 per the playbook, valid from 20000 in the m03k logs: AB notes dispute it)" }
  foreach($z in ($zf | select -First 4)){ $t=[double][regex]::Match($z,'^T=\s*([\d.]+)').Groups[1].Value; $w=Wall-From $E $t; if($w){ $w=$w.Add([TimeSpan]::FromSeconds(4)) }; Say ("    zoom at T={0:N0}: mirror frames {1}" -f $t,(Near-Mirror $E $w 4)) }
  $fm=@(Get-ChildItem "$E\mirror" -Filter '*-F.png' -ea 0); Say "  [LOOK] eye images to LOOK AT: mirror\*-F.png (fast frames around each zoom, $($fm.Count) files), compare with gameshots\*-F.png of the same second: PASS = a bright core + 6-ray starburst + faint ring + 4 ghosts on the sun in BOTH eyes, no dark ring, fading when the sun leaves the view; FAIL = bare sun disc only or a black ring."
}
function Analyze-FlareCam([string]$E,[string]$PairE){
  Say "  --- FLARE held sun camera ($E)"
  $cam=Read-Matching "$E\logs\xr-stereo.log" 'event=summary-decouple'; $held=Kvmax $cam 'cam-held'; Chk $(if($held -gt 0){'PASS'}else{'FAIL'}) 'debug camera held (summary-decouple cam-held > 0: the cam: pose is in use)' "cam-held max $held over $($cam.Count) lines"
  $po=Read-Matching "$E\logs\xr-stereo.log" 'event=png-once'; Chk $(if(($po | ? { $_ -match 'outcome=taken' }).Count){'PASS'}else{'FAIL'}) 'png-once' ($(if($po.Count){ $po[$po.Count-1].Substring(0,[math]::Min(220,$po[$po.Count-1].Length)) } else { 'no event=png-once line' }))
  $f=@(Get-ChildItem "$E\logs\png" -Filter '*-final.png' -ea 0 | select -First 1); if(-not $f){ Chk 'FAIL' 'eye dump' 'no *-final.png'; return }
  $s=Img-Stats $f.FullName; Say ("  [INFO] {0}: {1}x{2} white(min>=235) {3} warm-bright {4} max-luma {5:N1} bright-centroid {6:N0},{7:N0} (dump is rotated 180: upright copy in upright\)" -f $f.Name,$s[0],$s[1],$s[2],$s[3],($s[4]/1000),$s[5],$s[6])
  if($PairE){ $g=@(Get-ChildItem "$PairE\logs\png" -Filter '*-final.png' -ea 0 | select -First 1)
    if($g){ $t=Img-Stats $g.FullName; $d=Img-Diff $f.FullName $g.FullName 48
      $dw=$s[2]-$t[2]; $ok=($dw -ge 300 -or $d[2] -ge 2000)
      Chk $(if($ok){'PASS'}else{'FAIL'}) 'flare ON vs OFF (same pose, frame 240)' ("white pixels on {0} vs off {1} (delta {2}); pixels differing by >48 summed levels: {3} in box {4:N0},{5:N0}..{6:N0},{7:N0}. HEURISTIC thresholds: delta white >= 300 or >= 2000 differing pixels; clouds drift between runs, CONFIRM BY EYE (upright\ copies of both)" -f $s[2],$t[2],$dw,$d[2],$d[3],$d[4],$d[5],$d[6]) }
    else { Chk 'FAIL' 'pair' "no eye dump in $PairE" } }
  Say "  [LOOK] upright\*-final.png: PASS = bright core + starburst + ring + ghosts around the sun disc ON, none OFF; world-with-effects dump (-worldfx) has no flare (the flare is applied at the eye composite)."
}
function Analyze-Boxes([string]$E){
  Say "  --- NBOX / lock / roll"
  $xl="$E\logs\xr-stereo.log"; $nb=Read-Matching $xl 'event=summary-mk360-nbox;'; $bk=Read-Matching $xl 'event=summary-mk360-nbox-backend;'
  if(-not $nb.Count){ Chk 'FAIL' 'summary-mk360-nbox' 'no line (mk360:3 not active?)'; return }
  $h=Get-Kv $nb[$nb.Count-1]; $b=Get-Kv $bk[$bk.Count-1]
  Say ("  [INFO] last nbox: frames {0} live {1} cached {2} copies {3} cuts {4} no-cell {5} no-image {6} edge-kept {7} invalidated {8} no-atlas {9} rejected {10} sat {11} second {12} cut-kept {13}" -f $h.frames,$h.live,$h.cached,$h.copies,$h.cuts,$h['no-cell'],$h['no-image'],$h['edge-kept'],$h.invalidated,$h['no-atlas'],$h.rejected,$h.sat,$h.second,$h['cut-kept'])
  Chk $(if((Kn $h 'live') -gt 0){'PASS'}else{'FAIL'}) 'live stamps' "live=$($h.live) (boxes in view were stamped)"
  Chk $(if((Kn $h 'cached') -gt 0){'PASS'}else{'FAIL'}) 'cached stamps (drawn from the cache = contacts outside the window)' "cached=$($h.cached)"
  Chk $(if((Kn $h 'no-cell') -eq 0 -and (Kn $h 'no-atlas') -eq 0 -and (Kn $h 'rejected') -eq 0){'PASS'}else{'FAIL'}) 'no-cell / no-atlas / rejected all 0' "no-cell=$($h['no-cell']) no-atlas=$($h['no-atlas']) rejected=$($h.rejected)"
  Chk $(if((Kn $h 'invalidated') -eq 0){'PASS'}else{'WARN'}) 'invalidated 0 (>0 only after refusals)' "invalidated=$($h.invalidated)"
  if($bk.Count){ $okb=((Kn $b 'refused-atlas') -eq 0 -and (Kn $b 'refused-copy') -eq 0 -and (Kn $b 'refused-pipeline') -eq 0); Chk $(if($okb){'PASS'}else{'FAIL'}) 'backend refusals only refused-view' "used=$($b.used) refused=$($b.refused) refused-view=$($b['refused-view']) atlas=$($b['refused-atlas']) copy=$($b['refused-copy']) pipeline=$($b['refused-pipeline']) copies=$($b.copies) clears=$($b.clears)" }
  $cuts=Kn $h 'cuts'; $lv=Kn $h 'live'; if($lv -gt 0){ Chk 'INFO' 'cuts / live (step123 headset: 10.9 % during locks; must be well below it with a lock)' ("{0:P1}" -f ($cuts/$lv)) }
  $sat=Kvmax $nb 'sat'; $sec=Kvmax $nb 'second'; Chk 'INFO' 'sat (satellite groups joined = lock diamond grouped with its box) max / second max' "sat=$sat second=$sec (sat > 0 WHILE a lock diamond is on screen = PASS for the diamond; 0 in a flight without a lock proves nothing)"
  $cl=Read-Matching $xl 'event=mk360-contacts;'; $g14=0; $outbox=0; $glyph=@{}
  foreach($l in $cl){ foreach($m in [regex]::Matches($l,'(\d+):(win|out)(\+box)?:r(\d+):b(-?[\d.]+):e(-?[\d.]+):v(\d+):([^:]*):g(\d+)')){ $g=[int]$m.Groups[9].Value; if($g -ge 14){ $g14++ }; if($m.Groups[2].Value -eq 'out' -and $m.Groups[3].Success){ $outbox++ }; $glyph[$g]=1+$(if($glyph.ContainsKey($g)){$glyph[$g]}else{0}) } }
  Chk 'INFO' 'mk360-contacts' "$($cl.Count) snapshots; contact entries with g>=14 (box + diamond text): $g14; 'out+box' (outside the window WITH a box): $outbox; glyph counts seen: $((($glyph.GetEnumerator() | sort Name | % { "g$($_.Name)=$($_.Value)" }) -join ' '))"
  $al="$E\autopilot-1.log"; if(Test-Path $al){ $L=@(Get-Content $al); $lk=@($L | ? { $_ -match 'LOOKC' }); $fi=@($L | ? { $_ -match ' FIRE' }); $gn=@($L | ? { $_ -match ' GUN' })
    Chk $(if($lk.Count){'PASS'}else{'WARN'}) 'LOOKC look-at-contact taps' "$($lk.Count) (0 = no contact 45-115 deg off the nose within 14 km: nothing to look at; FIRE decisions $($fi.Count), GUN $($gn.Count))"
    foreach($z in ($lk | select -First 5)){ $mm=[regex]::Match($z,'^T=\s*([\d.,]+) LOOKC (\w+) hold [\d.]+ contact id=(\d+) col=(\S+) r=([\d,]+) b=(-?[\d.]+)'); if($mm.Success){ $w=Wall-From $E ([double]($mm.Groups[1].Value -replace ',','')+1.5); Say ("    LOOKC T={0} key {1} id {2} col {3} r {4} b {5}: mirror {6}; game {7}" -f $mm.Groups[1].Value,$mm.Groups[2].Value,$mm.Groups[3].Value,$mm.Groups[4].Value,$mm.Groups[5].Value,$mm.Groups[6].Value,(Near-Mirror $E $w 3),'gameshots same second') } }
    $rolls=@($L | % { $m=[regex]::Match($_,' r=(-?[\d.]+) \|'); if($m.Success){ [math]::Abs([double]$m.Groups[1].Value) } }); if($rolls.Count){ Chk 'INFO' 'roll in the decision rows' ("max |roll| {0:N2} rad, rows with |roll| > 0.3: {1}" -f ($rolls | measure -Maximum).Maximum,@($rolls | ? { $_ -gt 0.3 }).Count) } }
  Say "  [LOOK] mirror\* around each LOOKC (and a bank > 0.3 rad), vs gameshots: PASS = the game's own hexagon/TGT/F-4E box + label drawn OUTSIDE the forward rectangle exactly once on the contact, edges parallel to the cockpit frame when banked, no shear, no doubled copy, diamond + box + range one picture (lock only). NOT scripted: rolling 360 deg with a target in view, the head tilted 30 deg, lock-diamond disparity (needs the headset: markers-360.md 13.5 items 3-4)."
}
function Analyze-CloudTrig([string]$E){   # each trigger dump with its telemetry line (live run: cloudtrig.tsv; older evidence: replay of the rule + frame match of the dumps it has)
  try { & pwsh -NoProfile -File "$X\cloudtrig.ps1" -RunDir $E -Analyze 2>&1 | % { Say ([string]$_) } } catch { Chk 'WARN' 'cloudtrig analysis' $_.Exception.Message } }
function Analyze-Clouds([string]$E){
  Say "  --- CLOUDS / explosion ordering (effects-outside-window.md 15.5, 16)"
  $pl="$E\logs\step5-producer.log"; $xl="$E\logs\xr-stereo.log"
  $pp=Read-Matching $pl 'event=fx-sprite-plan;[^|]*class=sprite'; $ni=@($pp | ? { $_ -match ';ni=(\d+);qm=(\d+)' }); $maxni=0; $maxqm=0; $big=0; foreach($l in $ni){ $m=[regex]::Match($l,';ni=(\d+);qm=(\d+)'); $a=[int]$m.Groups[1].Value; $q=[int]$m.Groups[2].Value; if($a -gt $maxni){$maxni=$a}; if($q -gt $maxqm){$maxqm=$q}; if($q -ge 2000){$big++} }
  Chk $(if($ni.Count){'PASS'}else{'FAIL'}) "plan lines with ';ni=N;qm=L' (class=sprite)" "$($ni.Count) of $($pp.Count) class=sprite plan lines; max ni $maxni max qm $maxqm"
  Chk $(if($big){'PASS'}else{'WARN'}) 'cloud-puff sized quads (qm >= 2000 units; step124 C1: ni=1;qm~7600)' "$big lines (0 = no cloud puff was drawn: the decks were not in view; debris would be tens)"
  $pe=Read-Matching $pl 'event=fx-sprite-plan;[^|]*class=explosion'; $rf=@($pp | ? { $_ -notmatch 'refused=0' }); Chk $(if($rf.Count -eq 0){'PASS'}else{'WARN'}) 'class=sprite plan lines with refused>0' "$($rf.Count) (explosion plan lines in this flight: $($pe.Count); ordering is only tested when a kill/explosion happens: Kill runs, not this one)"
  $gp=Read-Matching $pl 'event=fx-sprite-gpu;frame='; $wf=@($gp | ? { (Kn (Get-Kv $_) 'window-fallback') -gt 0 }); $cap=Read-Matching $pl 'sprite-gpu-draw-cap'
  Chk $(if($gp.Count -and $wf.Count -le [math]::Max(2,$gp.Count*0.01)){'PASS'}elseif($gp.Count){'WARN'}else{'FAIL'}) 'fx-sprite-gpu window-fallback (<=1 % of lines; step124: 18 in 42994 replays)' "$($wf.Count) of $($gp.Count) lines have window-fallback>0"
  Chk $(if($cap.Count -eq 0){'PASS'}else{'WARN'}) "'sprite-gpu-draw-cap' in quiet flight" "$($cap.Count) lines"
  # effects-outside-window.md section 17.5 (step134): sprite draw cap 96, strip/list cloud sprites captured, nir suffix, dual-refused telemetry
  $mxr=0; foreach($l in $gp){ $m=[regex]::Match($l,';replays=(\d+)'); if($m.Success -and [int]$m.Groups[1].Value -gt $mxr){ $mxr=[int]$m.Groups[1].Value } }
  $dnc=@($gp | ? { $_ -match 'last-why=sprite-gpu-draw-cap' }).Count; Chk $(if($mxr -gt 32 -and $dnc -eq 0){'PASS'}elseif($mxr -gt 32){'WARN'}else{'WARN'}) '17.5.1 fx-sprite-gpu replays above 32 in dense frames, last-why never sprite-gpu-draw-cap' "max replays/frame $mxr; lines with last-why=draw-cap $dnc (replays <= 32 only means no dense frame happened)"
  $gt=Read-Matching $pl 'event=fx-sprite-gpu-totals'; if($gt.Count){ $q=Get-Kv $gt[$gt.Count-1]; $wr=(Kn $q 'window-fallback'); $rp=(Kn $q 'replays'); Chk $(if($rp -gt 0 -and $wr -le [math]::Max(5,0.01*$rp)){'PASS'}else{'WARN'}) '17.5.1 fx-sprite-gpu-totals window-fallback near 0 (was 600-1,880 per s)' "window-fallback $wr of replays $rp; last-why $($q['last-why'])" }
  $fxaOn=(Read-Matching $xl 'event=fx-additive-option;requested=1;active=1').Count -gt 0   # (P) with fxadd the point lists are captured: a topology refusal is then a FAIL
  $r27l=@($pp | ? { $_ -match 'refused=27' }); $r27=$r27l.Count; $r27pt=@($r27l | ? { $_ -match 'last-why=sprite-nonindexed-topology' }).Count; Chk $(if($r27 -eq 0){'PASS'}elseif($r27 -eq $r27pt -and -not $fxaOn){'WARN'}else{'FAIL'}) '17.5.2 no refused=27 per dense class=sprite plan frame (27 one-vertex point lists, topology 1, are the game own draws: WARN, docs 17.6)' "$r27 lines; $r27pt of them last-why=sprite-nonindexed-topology"
  $tt=Read-Matching $pl 'event=fx-sprite-totals;class=sprite'; $nir=@($tt | ? { $_ -match ';nir=' }); Chk 'INFO' "17.5.2 fx-sprite-totals ';nir=N(c:C,t:T)' (non-indexed draws still refused by shape/topology)" "$($nir.Count) of $($tt.Count) lines; last: $(if($nir.Count){ [regex]::Match($nir[$nir.Count-1],';nir=[^;]*').Value } else { 'none (nothing refused by shape)' })"
  $hf=Read-Matching $pl 'event=continuous-hud-effects;'; $hfr=@($hf | ? { (Kn (Get-Kv $_) 'refused') -gt 0 }).Count; $dual=Read-Matching $pl 'continuous-hud-effects-dual-refused'
  Chk $(if(($hfr -gt 0) -eq ($dual.Count -gt 0)){'PASS'}else{'FAIL'}) '17.5.3 continuous-hud-effects-dual-refused present iff continuous-hud-effects shows refused>0' "effects lines with refused>0: $hfr; dual-refused lines: $($dual.Count); last: $(if($dual.Count){ [regex]::Match($dual[$dual.Count-1],'last=[^;]*;blend=[^;]*').Value })"
  $fxs=Read-Matching $pl 'fx-sig-draw'; Chk 'INFO' '17.5.4 fx-sig-draw (only with fxsig:4caf6e2d, AGC3): vertices on x 1792/2304 or y 1824/2272 = the game clips the cloud at the window' "$($fxs.Count) lines"
  $gc=Read-Matching $xl 'game-clouds;replay=off'; $ig=Read-Matching $pl 'effects-gamecloud;[^|]*ignored'; Chk 'INFO' 'game cloud cards ignored (replay=off lines / producer ignored lines)' "$($gc.Count) / $($ig.Count)"
  $tm=Read-Matching $xl 'event=summary-timing'; if($tm.Count){ Say ("  [INFO] last summary-timing: " + $tm[$tm.Count-1].Substring(0,[math]::Min(300,$tm[$tm.Count-1].Length))) }
  $al="$E\autopilot-1.log"; if(Test-Path $al){ $lk=@(Get-Content $al | ? { $_ -match 'LOOK[ L]' -or $_ -match 'numpad|look-(left|right)' }); Say "  [INFO] look taps in the autopilot log: $($lk.Count)"; $ys=@(Get-Content $al | % { if($_ -match '^T=') { $y=Pos-Y $_; if($null -ne $y){ $y } } }); if($ys.Count){ Say ("  [INFO] altitude y min {0} max {1} (m03k decks 6000/6000/5000/5250)" -f ($ys | measure -Minimum).Minimum,($ys | measure -Maximum).Maximum) } }
  Analyze-CloudTrig $E
  Say "  [LOOK] mirror\* at the look-left/right taps and upright\*-final.png (flight frame 240): PASS = cloud puffs OUTSIDE the forward rectangle at stable world positions, NO straight vertical/horizontal edge at the window border (the 'C1 cut'); then if a kill happened, smoke/debris hides the wreck behind it. A straight edge that persists with ni>0 = the game clips at the window (16.2 hypothesis 1): run -CloudVertexDump."
}
# EFFECTS-OUTSIDE (P) fxadd:1 (docs/world-render/effects-outside-window.md 21): the additive 4caf6e2d family (blend 5,2,1/18,19,1) admitted through its clone and replayed
# per eye; its one-vertex point lists (topology 1) captured and replayed per eye (behind-eye points culled). Runs only when the run's xr-stereo.log has the option line.
function Analyze-FxAdd([string]$E){
  $pl="$E\logs\step5-producer.log"; $xl="$E\logs\xr-stereo.log"
  $opt=Read-Matching $xl 'event=fx-additive-option'; if(-not $opt.Count){ return }
  Say "  --- FXADD additive 4caf6e2d family + point lists per eye (effects-outside-window.md 21)"
  $on=@($opt | ? { $_ -match 'requested=1;active=1' }).Count
  Chk $(if($on){'PASS'}else{'FAIL'}) 'P.1 fx-additive-option active=1 (needs fxmem:6)' "$($opt.Count) lines; last: $([regex]::Match($opt[$opt.Count-1],'active=\d;reason=[^;]*').Value)"
  if(-not $on){ return }
  $al=Read-Matching $pl 'continuous-hud-effects-additive;'
  if(-not $al.Count){ Chk 'FAIL' 'P.2 continuous-hud-effects-additive status line' 'absent (the producer never logged it: fxadd flag not set in the producer?)'; return }
  $q=Get-Kv $al[$al.Count-1]; $adm=Kn $q 'admitted'; $pe=Kn $q 'per-eye'; $pts=Kn $q 'points'; $pep=Kn $q 'per-eye-points'; $fr=Kn $q 'frames'; $sh=Kn $q 'shots'
  Chk $(if($adm -gt 0){'PASS'}else{'WARN'}) 'P.2 additive draws admitted by the effects layer (admitted>0)' "admitted $adm (0 = no additive spark/glow/fire drawn: no explosion / crash in this run)"
  Chk $(if($pe -gt 0){'PASS'}elseif($adm -gt 0){'FAIL'}else{'WARN'}) 'P.3 additive draws replayed PER EYE (per-eye>0)' "per-eye $pe in $fr frames; admitted $adm"
  Chk $(if($pep -gt 0){'PASS'}elseif($pts -gt 0){'FAIL'}else{'INFO'}) 'P.4 point lists (topology 1) replayed per eye' "points admitted $pts, per eye $pep (0/0 = no point-list frame: they come with the crash camera / mission failed)"
  $dr=Read-Matching $pl 'continuous-hud-effects-dual-refused'; $dual=@($dr | ? { $_ -match 'blend=5,2,1/18,19,1' })
  Chk $(if($dual.Count -eq 0){'PASS'}else{'FAIL'}) 'P.5 no dual-refused line naming blend=5,2,1/18,19,1 (the additive tuple is no longer refused)' "$($dual.Count) lines"
  $st=Read-Matching $pl 'event=fx-sprite-totals;class=sprite'; $tt=@($st | ? { $_ -match ';pts=' }); $nirpt=@($st | ? { $_ -match ';nir=\d+\(c:1,t:1\)' })
  Chk $(if($nirpt.Count -eq 0){'PASS'}else{'FAIL'}) "P.6 no ';nir=N(c:1,t:1)' (point lists no longer refused by topology)" "$($nirpt.Count) lines"
  Chk 'INFO' "P.6 fx-sprite-totals ';pts=N(cull:C)' (points per eye / culled behind an eye)" "$($tt.Count) lines; last: $(if($tt.Count){ [regex]::Match($tt[$tt.Count-1],';pts=[^;]*').Value } else { 'none' })"
  $gp=Read-Matching $pl 'event=fx-sprite-gpu;frame='; $dc=@($gp | ? { $_ -match 'last-why=sprite-gpu-draw-cap|last-why=sprite-gpu-span-full' }).Count; $ec=(Read-Matching $pl 'last-refusal=effects-cap').Count
  Chk $(if($dc -eq 0 -and $ec -eq 0){'PASS'}else{'WARN'}) 'P.7 the extra draws fit: no sprite-gpu-draw-cap / span-full, no effects-cap' "fx-sprite-gpu lines with cap/span $dc; effects-cap $ec"
  $shot=Read-Matching $pl 'fx-additive-shot;'; $arm=Read-Matching $xl 'png-eye-armed;outcome=armed;[^|]*trigger=fx-additive'
  $pngs=@(Get-ChildItem "$E\logs\png" -Filter '*-final.png' -ea 0 | sort LastWriteTime | % { $_.Name })
  Chk $(if($arm.Count){'PASS'}elseif($shot.Count){'WARN'}else{'INFO'}) 'P.8 eye dump at the first additive per-eye frame (fx-additive-shot -> png-eye-armed trigger=fx-additive; needs xr_png_once)' "shots requested $($shot.Count) (status $sh), armed $($arm.Count); final pngs: $($pngs -join ',')"
  $tm=Read-Matching $xl 'event=summary-timing'; if($tm.Count){ Say ("  [INFO] P.9 last summary-timing (compare with a run without fxadd): " + $tm[$tm.Count-1].Substring(0,[math]::Min(240,$tm[$tm.Count-1].Length))) }
  Say "  [LOOK] P.10 the png(s) after the fx-additive arm: orange sparks / glow around the explosion in the eye, outside the forward rectangle when it is off the nose, drawn once, additive (no dark box), occluded by aircraft in front of them; points are single eye pixels."
}
function Analyze-Ui3d([string]$E){
  Say "  --- UI3D menus / hangar / pause / debrief census"
  $xl="$E\logs\xr-stereo.log"; $ec="$E\logs\ee-camera.log"
  $op=Read-Matching $xl 'event=ui3d-option'; Chk $(if($op.Count -and $op[0] -match 'ok=1' -and $op[0] -match 'u3p=1' -and $op[0] -match 'u3a=1' -and $op[0] -match 'prerequisites=ok'){'PASS'}else{'FAIL'}) 'ui3d-option ok=1;u3p=1;u3a=1;prerequisites=ok' ($(if($op.Count){ $op[0].Substring($op[0].IndexOf('event=')) } else { 'missing' }))
  $su=Read-Matching $ec 'summary-ui3d;frames='; if(-not $su.Count){ Chk 'FAIL' 'summary-ui3d (ee-camera.log)' 'no lines'; return }
  $h=Get-Kv $su[$su.Count-1]; Say ("  [INFO] last summary-ui3d: frames {0} usable {1} refused {2} published {3} rejected {4} held {5} last-refusal {6} last-why {7}" -f $h.frames,$h.usable,$h.refused,$h.published,$h.rejected,$h.held,$h['last-refusal'],$h['last-why'])
  Chk $(if((Kn $h 'published') -gt 0){'PASS'}else{'FAIL'}) 'stage-2 menus published (main/mission menus)' "published=$($h.published) far=$($h.far) mid=$($h.mid) near=$($h.near) culled=$($h.culled)"
  Chk $(if((Kvmax $su 'u3a-published') -gt 100){'PASS'}elseif((Kvmax $su 'u3a-published') -gt 0){'WARN'}else{'FAIL'}) 'hangar room: u3a-published (~60/s for the whole visit; L13: 1383)' "u3a-frames $(Kvmax $su 'u3a-frames') published $(Kvmax $su 'u3a-published') rejected $(Kvmax $su 'u3a-rejected') (L13: 7) skips $(Kvmax $su 'u3a-skips') late $(Kvmax $su 'u3a-late') latches $(Kvmax $su 'u3a-latches') cuts $(Kvmax $su 'u3a-cuts') why $($h['u3a-why'])"
  # ui-3d.md section 10 (step134): floating panel options, late indexed overlay draws DROPPED (not rejected), moving camera does not re-latch, foreign ACM draws do not mask the reason
  $o0=if($op.Count){ $op[0] }else{ '' }; Chk $(if($o0 -match 'u3mn=1\.10' -and $o0 -match 'u3mf=6\.00' -and $o0 -match 'u3s=0\.70'){'PASS'}else{'FAIL'}) 'ui3d-option ends u3mn=1.10;u3mf=6.00;u3s=0.70 (section 10.3; absent = a build older than the section 10 fixes)' ($o0 -replace '^.*event=','event=')
  $dr=Kvmax $su 'u3a-dropped'; $mv=Kvmax $su 'u3a-moving'; $fo=Kvmax $su 'u3a-foreign'; $rj=Kvmax $su 'u3a-rejected'
  Chk $(if($dr -lt 0){'FAIL'}else{'PASS'}) 'summary-ui3d carries u3a-dropped / u3a-moving / u3a-foreign (section 10.2 / 10.6)' "dropped $dr moving $mv foreign $fo rejected $rj (10.5: dropped > 0 = the late indexed draws were dropped without rejecting; rejected 0..few; moving > 0 only during the deploy camera sweep; foreign grows only when the load starts)"
  Chk $(if($dr -gt 0 -and $rj -le 5){'PASS'}elseif($dr -eq 0){'WARN'}else{'FAIL'}) 'hangar: late draws dropped, not rejected (10.5 item 2: u3a-dropped > 0 and u3a-rejected <= 5)' "dropped $dr rejected $rj held(last) $($h.held) why $($h['u3a-why'])"
  $sw=Read-Matching $xl 'event=summary;t-ms='; $nwin=$sw.Count; Chk $(if($nwin -eq 0 -or $su.Count -ge 0.8*$nwin){'PASS'}else{'WARN'}) 'ee-camera.log summary-ui3d every 5 s for the whole session (10.5 item 4)' "$($su.Count) summary-ui3d vs $nwin XR summary windows"
  $pf=Kvmax $su 'pause-frames'; $pp=Kvmax $su 'pause-published'; $pl=Kvmax $su 'pause-placements'
  Chk $(if($pf -gt 0 -and $pp -gt 0){'PASS'}else{'FAIL'}) 'in-flight pause: pause-frames / pause-published > 0, pause-placements >= 2 (two pauses)' "pause-frames $pf published $pp placements $pl rejected(final) $($h.rejected)"
  $pa=Read-Matching $xl 'event=summary-ui3d-pause'; if($pa.Count){ $q=Get-Kv $pa[$pa.Count-1]; Chk $(if((Kn $q 'blends') -gt 0 -and (Kn $q 'stale') -eq 0 -and (Kn $q 'blit-failed') -eq 0 -and (Kn $q 'poisoned') -eq 0){'PASS'}else{'FAIL'}) 'summary-ui3d-pause blends>0, stale 0, blit-failed 0, poisoned 0' "blends $($q.blends) slots $($q.slots) stale $($q.stale) blit-failed $($q['blit-failed']) poisoned $($q.poisoned) held $($q.held) why $($q.why)" } else { Chk 'FAIL' 'summary-ui3d-pause' 'no line' }
  $he=Read-Matching $xl 'event=hud-menu;[^|]*state=enter'; $hx=Read-Matching $xl 'event=hud-menu;[^|]*state=exit'; Chk $(if($he.Count -ge 2 -and $hx.Count -ge 2){'PASS'}else{'WARN'}) 'hud-menu enter / exit (two Esc pauses)' "enter $($he.Count) exit $($hx.Count)"
  $png=Read-Matching $xl 'event=png-screen;[^|]*outcome=(written|queued)'; $et=@($png | ? { $_ -match 'part=eye0-table' }).Count; Chk $(if($et){'PASS'}else{'FAIL'}) 'png-screen eye0-table dumps (the hangar room / table in the eye)' "$et eye0-table, $(@($png | ? { $_ -match 'part=screen' }).Count) screen PNGs (upright copies in upright\; eye0-table + screen are rotated 180)"
  $cn=Read-Matching $ec 'event=ui3d-census;'; Chk $(if($cn.Count){'PASS'}else{'WARN'}) 'ui3d-census lines' "$($cn.Count) lines, latched: $(@($cn | ? { $_ -match 'latched' }).Count), frames: $(@($cn | ? { $_ -match 'kind=frame' }).Count)"
  $bo=Read-Matching $ec 'briefing-observe;event=(window|top|rest)'; $bc=Read-Matching $ec 'brief3d-capture'; $sb=Read-Matching $xl 'event=summary-brief3d'
  Chk 'INFO' 'DEBRIEF capture (debrief-3d-map.md 5: only if a mission end / results / debrief screen was reached)' "briefing-observe window/top/rest lines: $($bo.Count); brief3d-capture lines: $($bc.Count); summary-brief3d lines: $($sb.Count)"
  $post=@(Get-Content "$E\steps.txt" -ea 0 | ? { $_ -match 'post-flight|post-\d|post census' }); Say "  [INFO] post-flight steps: $($post.Count) lines; screenshots shots\post-*.png (LOOK at them: which screens appeared, in what order, with the wall-clock of each in steps.txt)"
  Say "  [LOOK] upright\*-eye0-table.png (hangar room around the screen overlay: textures, aircraft in front of the walls, level floor, no mirrored geometry; text layer nearer than the backdrop), shots\post-*.png, mirror\* around the two pauses (menu over the dimmed cockpit, no flat HUD quad). Debrief: any briefing-observe window with selmap>0 or modelicon>0 = case A, else case B (debrief-3d-map.md 5 'Reading the result')."
}
# E6 locator + implausible-E6 guard (e6-anchor-free.md 11.4, lens-flare.md 2026-10-30): the camera everything else depends on. ee-camera.log carries e6-locate lines, xr-stereo.log the guard.
function Analyze-E6([string]$E){
  Say "  --- E6 camera locator / implausible guard (e6-anchor-free.md section 11.4)"
  $ec="$E\logs\ee-camera.log"; $xl="$E\logs\xr-stereo.log"
  $ss=Read-Matching $ec 'e6-locate;event=struct-scan'; $hits=Kvmax $ss 'hits'; $mxms=Kvmax $ss 'ms'
  Chk $(if($ss.Count -gt 0 -and $hits -gt 0){'PASS'}elseif($ss.Count){'FAIL'}else{'FAIL'}) 'e6-locate struct-scan passes with a hit (block+CA811C J35 / +CAAC6C F-5E; any +0xC.. offset is fine)' "$($ss.Count) pass lines, max hits $hits, max ms/pass $mxms (0 pass lines = the scanner never ran; 0 hits = the structure was not found)"
  $lk=Read-Matching $ec 'e6-locate;event=locked'; Chk $(if($lk.Count){'PASS'}else{'FAIL'}) 'e6-locate event=locked (eye of the flown aircraft)' "$($lk.Count) lines; kinds: $((($lk | % { [regex]::Match($_,'kind=(\w+)').Groups[1].Value } | group | % { "$($_.Name)x$($_.Count)" }) -join ' ')) (kind=s = relocated structure found by the scan, r = fixed offset)"
  # D-401: a lock on origin junk (cam=0,0,0) passed the line above in step134; require a real aircraft match too.
  $junk=@($lk | ? { $_ -match 'cam=0\.00,0\.00,0\.00' }); $mt=Read-Matching $xl 'event=cockpit-auto;window=\d+;verdict=match'   # the detector logs to xr-stereo.log, not ee-camera.log
  Chk $(if($lk.Count -and -not $junk.Count -and $mt.Count){'PASS'}else{'FAIL'}) 'E6 lock is the real aircraft (no cam=0,0,0 lock; cockpit-auto verdict=match)' "junk locks $($junk.Count), match windows $($mt.Count) (D-401: step134 locked origin junk and never matched an aircraft eye)"
  $su=Read-Matching $ec 'e6-locate;event=summary'; if($su.Count){ $h=Get-Kv $su[$su.Count-1]; Chk $(if((Kn $h 'locks') -ge 1 -and (Kn $h 'losses') -eq 0){'PASS'}elseif((Kn $h 'locks') -ge 1){'WARN'}else{'FAIL'}) 'e6-locate summary locks>=1, losses=0 (last line)' "state $($h.state) locks $($h.locks) losses $($h.losses) valid $($h.valid) rejected $($h.rejected) fallback-published $($h['fallback-published']) fallback-rejected $($h['fallback-rejected']) reasons $($h.reasons)" } else { Chk 'FAIL' 'e6-locate summary' 'no line' }
  $fg=Read-Matching $ec 'e6-locate;event=fallback-gate'; $fq=@($fg | ? { $_ -match 'state=qualified' }).Count; $fw=@($fg | ? { $_ -match 'state=withdrawn' }).Count
  Chk $(if($fq -eq 0){'PASS'}else{'WARN'}) 'fallback-gate: no qualified base unless its samples move' "qualified $fq withdrawn $fw (a qualified base that does not move = static junk published: check camlag E6 eye)"
  $ie=Read-Matching $xl 'event=e6-implausible'; $ien=@($ie | ? { $_ -match 'state=enter' }).Count; $iex=@($ie | ? { $_ -match 'state=exit' }).Count
  $tc=Read-Matching $xl 'event=summary-terrain-camera'; $imax=Kvmax $tc 'e6-implausible'; $gref=Kvmax $tc 'game-refused'; $refused=@(Read-Matching $xl 'mode=e6-only-game-refused').Count
  Chk $(if($refused -le 10){'PASS'}else{'FAIL'}) "no long 'mode=e6-only-game-refused' stretch (the black-world/frozen-camera signature, lens-flare.md)" "mode lines $refused; summary-terrain-camera game-refused max $gref; e6-implausible enter $ien exit $iex (counter max $imax; enter>0 = the guard fired: the E6 sample was garbage, the game camera was used instead)"
  $ca=Read-Matching $xl 'event=camera-admit'; $hy=@($ca | ? { $_ -match 'correlated-hybrid' }).Count; Chk $(if($hy){'PASS'}else{'WARN'}) 'camera-admit reached correlated-hybrid (object-registration=allowed)' "$hy of $($ca.Count) lines; last: $(if($ca.Count){ $ca[$ca.Count-1].Substring(0,[math]::Min(160,$ca[$ca.Count-1].Length)) })"
  $buried=@(Read-Matching $ec 'cam=2,2,2').Count; Chk $(if($buried -eq 0){'PASS'}elseif($buried -le 2){'WARN'}else{'FAIL'}) 'no ee-camera rows with the stale block eye cam=2,2,2 (1-2 rows = the known startup row, same in step183-186: WARN; more = FAIL)' "$buried rows"
  $st=Read-Matching $xl 'stale-forget'; Chk $(if($st.Count -eq 0){'PASS'}else{'WARN'}) 'cockpit-auto no stale-forget' "$($st.Count) lines"
  $ca2=Read-Matching $xl 'event=cockpit-auto;[^|]*state=(loaded|model)'; Chk 'INFO' 'cockpit-auto model lines' "$($ca2.Count) $(if($ca2.Count){ $ca2[$ca2.Count-1].Substring(0,[math]::Min(160,$ca2[$ca2.Count-1].Length)) })"
}
# lens flare telemetry (lens-flare.md 2026-10-30): lensflare-config once per stage generation, summary-lensflare every window. -ExpectOn: the sun is meant to be in view; -ExpectOff: lflare:0.
function Analyze-Lensflare([string]$E,[bool]$ExpectOn,[bool]$ExpectOff){
  Say "  --- lens flare telemetry (lens-flare.md, 2026-10-30 section)"
  $xl="$E\logs\xr-stereo.log"; $cf=Read-Matching $xl 'event=lensflare-config'
  if($cf.Count){ $c=Get-Kv $cf[$cf.Count-1]; $okc=($c.env -eq '1' -and $c.'light-is-moon' -eq '0' -and [math]::Abs((Kn $c 'sun-elevation-deg')-50) -lt 1)
    if($ExpectOff){ $okc=($c.switch -eq '0' -or (Kn $c 'k') -eq 0) }
    Chk $(if($okc){'PASS'}else{'FAIL'}) 'lensflare-config' ("env $($c.env) sky-bit $($c['sky-bit']) switch $($c.switch) k $($c.k) light $($c.light) moon $($c['light-is-moon']) sun-elevation-deg $($c['sun-elevation-deg']) (m03k: env 1, moon 0, elevation 50.0; lflare:0 = switch 0)") }
  else { Chk 'FAIL' 'lensflare-config' 'no line (no stage generation with env, or a build without the telemetry)' }
  $sl=Read-Matching $xl 'event=summary-lensflare'; if(-not $sl.Count){ Chk 'FAIL' 'summary-lensflare' 'no lines'; return }
  $h=Get-Kv $sl[$sl.Count-1]; $tot=Kn $h 'total-active'; $fr=Kn $h 'total-frames'
  $why=@{}; foreach($k in 'active','off','no-env','no-light','moon','below-horizon','behind','out-of-view'){ $why[$k]=Kvmax $sl $k }
  Say ("  [INFO] summary-lensflare ({0} windows): total-active {1} of total-frames {2}; per-window maxima active {3} off {4} no-env {5} no-light {6} moon {7} below-horizon {8} behind {9} out-of-view {10}; peak-gate max {11} best-cos max {12} sun-sin-el {13}" -f $sl.Count,$tot,$fr,$why['active'],$why['off'],$why['no-env'],$why['no-light'],$why['moon'],$why['below-horizon'],$why['behind'],$why['out-of-view'],(Kvmax $sl 'peak-gate'),(Kvmax $sl 'best-cos'),$h['sun-sin-el'])
  if($ExpectOff){ Chk $(if($tot -eq 0 -and $why['off'] -gt 0){'PASS'}else{'FAIL'}) 'lflare:0 -> total-active 0 and reason off' "total-active $tot off max $($why['off'])" }
  elseif($ExpectOn){ Chk $(if($tot -gt 0){'PASS'}else{'FAIL'}) 'flare fired while the sun was meant to be in view: total-active > 0' "total-active $tot (0 with behind/out-of-view dominating = the head/camera never faced the sun: pose or SunHdg wrong; no-env/no-light/moon = stage problem)" }
  else { Chk 'INFO' 'flare reasons' "total-active $tot" }
}
# BRIEF3D (2026-10-31, -BriefMap, run AGM1): the briefing map table. Reads steps.txt (BRIEFMAP lines of runK3ag), xr-stereo.log (summary-brief3d-latch / -table / -producer, png lines) and ee-camera.log
# (brief3d-latch, brief3d-fit, brief3d-capture, briefing-observe); judges nothing about looks: the eye0-table / screen dumps (upright\) are for the eye ([LOOK]).
function Img-Lit([string]$p,[int]$thr){ $i=Load-Px $p; [AGImg]::Lit($i.a,$i.w,$i.h,$thr) }
function Analyze-Brief3d([string]$E){
  Say "  --- BRIEF3D briefing map table (3D overview, then the BRIEFING menu item; the run never flies)"
  $xl="$E\logs\xr-stereo.log"; $ec="$E\logs\ee-camera.log"
  if(-not (Test-Path $xl)){ Chk 'FAIL' 'xr-stereo.log' 'missing: nothing to analyse'; return }
  $bm=@(Get-Content "$E\steps.txt" -ea 0 | ? { $_ -match 'BRIEFMAP|ROUTE FAILED' })
  $hs=@($bm | ? { $_ -match 'BRIEFMAP hold start' }); $he=@($bm | ? { $_ -match 'BRIEFMAP hold end' })
  $ov=@($bm | ? { $_ -match 'overview: map draws seen' }).Count; $br=@($bm | ? { $_ -match 'briefing: map draws seen' }).Count
  Chk $(if($hs.Count -ge 1 -and $he.Count -ge 1){'PASS'}else{'FAIL'}) 'map screen reached and held (steps.txt BRIEFMAP hold start / end)' "$($hs.Count) hold(s) started, $($he.Count) finished; overview map seen $ov, BRIEFING-menu map seen $br (a skipped second leg is logged as 'BRIEFING' lines)"
  foreach($l in ($bm | ? { $_ -notmatch 'BRIEFMAP png-' })){ Say ("  [INFO] steps: " + ($l -replace '^(\d\d:\d\d:\d\d)\.\d+ ','$1 ')) }
  Say ("  [INFO] triggers written: png-screen.now {0}, png-eye.now {1}" -f @($bm | ? { $_ -match 'BRIEFMAP png-screen\.now \d' }).Count,@($bm | ? { $_ -match 'BRIEFMAP png-eye\.now \(' }).Count)
  Chk $(if((Test-Path "$E\MISSION-STARTED") -or @(Read-Matching $xl 'flight-stage-detect').Count){'FAIL'}else{'PASS'}) 'no mission started (no flight-stage-detect, no MISSION-STARTED marker)' ''
  $po=Read-Matching $xl 'event=brief3d-option'; Chk $(if($po.Count -and $po[0] -match 'ok=1' -and $po[0] -match 'prerequisites=ok'){'PASS'}elseif($po.Count){'WARN'}else{'FAIL'}) 'brief3d-option ok=1;prerequisites=ok (live=producer-configured is the b3l:1 mode)' $(if($po.Count){ $po[0].Substring($po[0].IndexOf('event=')) } else { 'missing: brief3d off in this run' })
  $fit=Read-Matching $ec 'brief3d-fit;'; $fok=@($fit | ? { $_ -match 'outcome=ok' }).Count
  Chk $(if($fok -ge 1){'PASS'}else{'FAIL'}) 'brief3d-fit outcome=ok (one per map screen)' "$fok ok of $($fit.Count) fit lines; last: $(if($fit.Count){ $fit[$fit.Count-1] -replace '^.*brief3d-fit;','' })"
  # producer (summary-brief3d-producer): the game's map draws replayed/latched
  $pr=Read-Matching $xl 'event=summary-brief3d-producer'
  if(-not $pr.Count){ Chk 'FAIL' 'summary-brief3d-producer' 'no lines' } else { $q=Get-Kv $pr[$pr.Count-1]
    Chk $(if((Kn $q 'selmap') -gt 0 -and (Kn $q 'published') -gt 0 -and (Kn $q 'aborted') -le 0 -and (Kn $q 'poisoned') -le 0){'PASS'}else{'FAIL'}) 'summary-brief3d-producer: selmap > 0, published > 0, aborted 0, poisoned 0' "frames $($q.frames) published $($q.published) aborted $($q.aborted) replays $($q.replays) selmap $($q.selmap) icons $($q.icons) icons-unverified $($q['icons-unverified']) refused $($q.refused) lag-max $($q['lag-max']) age-max-ms $($q['age-max-ms']) poisoned $($q.poisoned) why $($q.why) ($($pr.Count) lines)"
    $sm=Kn $q 'selmap'; $rf=Kn $q 'refused'; if($sm -gt 0){ Chk $(if($rf -le 0.25*$sm){'PASS'}else{'WARN'}) 'producer refusals relative to map draws (refused <= 25 % of selmap)' ("{0:N0} refused of {1:N0} selmap draws ({2:N0} %); icons {3}" -f $rf,$sm,(100.0*$rf/$sm),$q.icons) } }
  # latch (summary-brief3d-latch): the per-XR-frame table render
  $la=Read-Matching $xl 'event=summary-brief3d-latch'
  if(-not $la.Count){ Chk 'FAIL' 'summary-brief3d-latch' 'no lines (b3l:0 or the table core is not running)' } else { $q=Get-Kv $la[$la.Count-1]; $rmax=Kvmax $la 'renders'
    Chk $(if($rmax -gt 0 -and (Kvmax $la 'render-failed') -le 0 -and (Kn $q 'poisoned') -le 0 -and (Kn $q 'dead') -le 0){'PASS'}else{'FAIL'}) 'summary-brief3d-latch: renders > 0, render-failed 0, poisoned 0, dead 0' "renders $($q.renders) render-failed $($q['render-failed']) layers $($q.layers) scenes $($q.scenes) no-scene $($q['no-scene']) stale $($q.stale) age-max-ms $($q['age-max-ms']) plan-failed $($q['plan-failed']) alloc-busy $($q['alloc-busy']) resubmits $($q.resubmits) lost $($q.lost) clipped $($q.clipped) culled-copies $($q['culled-copies']) skipped $($q.skipped) poisoned $($q.poisoned) dead $($q.dead) why $($q.why) plan-why $($q['plan-why']) ($($la.Count) lines)"
    $ns=Kn $q 'no-scene'; $sc=Kn $q 'scenes'; if($ns -ge 0 -and $sc -gt 0){ Chk $(if($ns -le $sc){'PASS'}else{'WARN'}) 'latch frames without a scene (no-scene <= scenes; a large no-scene = the table layer starves between producer scenes)' "no-scene $ns vs scenes $sc" } }
  # table consumer (summary-brief3d-table)
  $ta=Read-Matching $xl 'event=summary-brief3d-table'
  if(-not $ta.Count){ Chk 'FAIL' 'summary-brief3d-table' 'no lines' } else { $q=Get-Kv $ta[$ta.Count-1]
    Chk $(if((Kvmax $ta 'blits') -ge 0 -and (Kvmax $ta 'blit-failed') -le 0 -and (Kn $q 'poisoned') -le 0 -and (Kn $q 'dead') -le 0 -and (Kn $q 'quarantined') -le 0){'PASS'}else{'FAIL'}) 'summary-brief3d-table: blit-failed 0, poisoned 0, dead 0, quarantined 0' "blits $($q.blits) layers $($q.layers) no-image $($q['no-image']) stale $($q.stale) content $($q.content) holding $($q.holding) invalidated $($q.invalidated) fits $($q.fits) placements $($q.placements) no-pose $($q['no-pose']) no-slot $($q['no-slot']) screens $($q.screens) why $($q.why) fit-why $($q['fit-why']) ($($ta.Count) lines)" }
  # producer-side latch core (ee-camera.log brief3d-latch;): scenes vs incomplete/refused geometry = how much of the map the game submitted was usable
  $el=Read-Matching $ec '\|brief3d-latch;'
  if($el.Count){ $q=Get-Kv $el[$el.Count-1]; $sc=Kn $q 'scenes'; $inc=Kn $q 'incomplete'; $den=$sc+$inc
    Chk $(if($den -le 0){'WARN'}elseif($inc -le 0.2*$den){'PASS'}else{'WARN'}) 'ee-camera brief3d-latch: incomplete scenes <= 20 % (headset test 7 had 122 of 302, why=b3l-read-rate)' ("scenes {0} incomplete {1} draws {2} refused {3} geom-refused {4} reads {5} hits {6} rate {7} torn {8} faults {9} fallback {10} why {11} ({12} lines)" -f $q.scenes,$q.incomplete,$q.draws,$q.refused,$q['geom-refused'],$q.reads,$q.hits,$q.rate,$q.torn,$q.faults,$q.fallback,$q.why,$el.Count) } else { Chk 'WARN' 'ee-camera brief3d-latch' 'no lines' }
  $cl=Read-Matching $ec '\|brief3d-content;'; $cp=Read-Matching $ec 'brief3d-capture;'; $bo=Read-Matching $ec 'briefing-observe;event=window'
  Chk 'INFO' 'capture / observe (read only)' "brief3d-capture lines $($cp.Count), brief3d-content lines $($cl.Count), briefing-observe windows $($bo.Count) (max selmap $(Kvmax $bo 'selmap') modelicon $(Kvmax $bo 'modelicon'))"
  # dumps
  $pn=Read-Matching $xl 'event=png-screen;'; $pa=Read-Matching $xl 'event=png-screen-armed'; $pe=Read-Matching $xl 'event=png-eye-armed'
  $sq=@($pn | ? { $_ -match 'part=screen' -and $_ -match 'outcome=queued' }).Count; $eq=@($pn | ? { $_ -match 'part=eye0-table' -and $_ -match 'outcome=queued' }).Count
  Chk $(if($eq -ge 1){'PASS'}else{'FAIL'}) 'png-screen.now dumps (screen + eye0-table, 2400x1350, every 3 s in a hold)' "armed $(@($pa | ? { $_ -match 'outcome=armed' }).Count) refused $(@($pa | ? { $_ -match 'outcome=refused' }).Count); screen PNG queued $sq, eye0-table PNG queued $eq"
  $pgs=@(Get-ChildItem "$E\logs\png" -Filter *.png -ea 0 | ? { $_.Name -notmatch '-(screen|eye0-table)\.png$' })
  Chk 'INFO' 'png-eye.now (xr_png_once re-arm)' "png-eye-armed lines $($pe.Count) ($(@($pe | ? { $_ -match 'outcome=armed' }).Count) armed); other PNG dumps $($pgs.Count): $(($pgs | % { $_.Name }) -join ', ') (xr_png_once only acts on FRESH flight composites; zero here in the menus is the expected outcome, the table dumps above are the content)"
  $et=@(Get-ChildItem "$E\logs\png" -Filter *eye0-table.png -ea 0 | sort { [int64]([regex]::Match($_.Name,'-t(\d+)-').Groups[1].Value) }); $lit=0
  foreach($f in $et){ $s=Img-Lit $f.FullName 24; $t=[regex]::Match($f.Name,'-t(\d+)-').Groups[1].Value; if($s[2] -gt 2000){ $lit++ }
    Say ("  [INFO] eye0-table t{0}: lit pixels (luma > 24) {1:N0} of {2:N0}, bbox {3},{4}..{5},{6} (stored rotated 180), max luma {7:N0}" -f $t,$s[2],($s[0]*$s[1]),$s[3],$s[4],$s[5],$s[6],($s[7]/1000)) }
  Chk $(if($et.Count -eq 0){'FAIL'}elseif($lit -ge 1){'PASS'}else{'FAIL'}) 'eye0-table dumps hold visible content (some dump with > 2000 lit pixels)' "$lit of $($et.Count) dumps"
  Say "  [LOOK] upright\*-eye0-table.png (the 3D table seen from the eye: whole map grid, icons, height; edge-on thin line = the table seen from table level; compare t-ms with steps.txt BRIEFMAP hold times) and upright\*-screen.png (what the 2D panels show at the same time), shots\brief-*.png (window captures). The map was NOT fully rendered if the grid is cut, flickers between dumps (compare consecutive 3 s dumps) or icons are missing."
}
# DEBRIEF (2026-11-01, -Debrief, run DBR1): the post-mission debrief (dbr3d). Reads steps.txt (DEBRIEF lines of runK3ag), ee-camera.log (summary-dbr3d, summary-ui3d, briefing-observe), xr-stereo.log (dbr3d-option, png lines), the
# census lines of diagnostics.evidence.log, and classifies the killcap gameshots with debrieflib.ps1 (BLACK / REPLAY / PAUSE+row / OTHER) into a screen timeline (works on archived runs: step174 runAGU1 = crash -> REPLAY, never left).
. "$X\debrieflib.ps1"
function Analyze-Debrief([string]$E){
  Say "  --- DEBRIEF post-mission debrief (dbr3d:1: results panel floating, map wrapped around the user)"
  $xl="$E\logs\xr-stereo.log"; $ec="$E\logs\ee-camera.log"
  $dl=@(Get-Content "$E\steps.txt" -ea 0 | ? { $_ -match 'DEBRIEF|ROUTE FAILED|MISSION-STARTED' })
  if($dl.Count){
    $rs=@($dl | ? { $_ -match 'REPLAY screen seen' }).Count; $rl=@($dl | ? { $_ -match 'DEBRIEF: replay left' }).Count; $nr=@($dl | ? { $_ -match 'DEBRIEF NOT REACHED|REPLAY NOT EXITED|MISSION END NOT REACHED' }).Count
    $hs=@($dl | ? { $_ -match 'DEBRIEF hold start' }).Count; $he=@($dl | ? { $_ -match 'DEBRIEF hold end' }).Count; $pt=@($dl | ? { $_ -match 'confirm to advance' }).Count
    Chk $(if($hs -ge 1 -and $he -ge 1 -and $nr -eq 0){'PASS'}else{'FAIL'}) 'debrief reached and held (steps.txt DEBRIEF hold start / end)' "mission end: replay seen $rs, replay left $rl; holds $hs started / $he finished; page confirms $pt; not-reached markers $nr"
    foreach($l in ($dl | ? { $_ -notmatch 'png-screen\.now \d|png-eye\.now \(' })){ Say ("  [INFO] steps: " + ($l -replace '^(\d\d:\d\d:\d\d)\.\d+ ','$1 ')) }
    Say ("  [INFO] triggers written: png-screen.now {0}, png-eye.now {1}" -f @($dl | ? { $_ -match 'DEBRIEF png-screen\.now \d' }).Count,@($dl | ? { $_ -match 'DEBRIEF png-eye\.now \(' }).Count)
    Chk $(if(Test-Path "$E\MISSION-STARTED"){'FAIL'}else{'PASS'}) 'no new mission started after the first flight (MISSION-STARTED marker)' ''
  } else { Chk 'INFO' 'steps.txt DEBRIEF lines' 'none: this run did not use -Debrief (older evidence); the screen timeline and the log lines below still apply' }
  # screen timeline from the game window shots
  $gs=@(Get-ChildItem "$E\gameshots" -Filter *.png -ea 0 | sort Name)
  if($gs.Count){
    $stride=[math]::Max(1,[int][math]::Ceiling($gs.Count/300.0)); $tl=@(); $cnt=@{}
    for($i=0;$i -lt $gs.Count;$i+=$stride){ $f=$gs[$i]; $tm=[regex]::Match($f.Name,'-(\d\d)(\d\d)(\d\d)'); $c=$null; try { $c=DbrClassifyFile $f.FullName } catch { }; if(-not $c){ continue }
      $lab=$c.cls+$(if($c.cls -eq 'PAUSE'){"/row$($c.row)"}else{''}); $cnt[$lab]=1+[int]$cnt[$lab]; $t=$(if($tm.Success){ "$($tm.Groups[1].Value):$($tm.Groups[2].Value):$($tm.Groups[3].Value)" }else{ $f.Name })
      if($tl.Count -and $tl[$tl.Count-1].lab -eq $lab){ $tl[$tl.Count-1].end=$t; $tl[$tl.Count-1].n++ } else { $tl+=[pscustomobject]@{lab=$lab;start=$t;end=$t;n=1} } }
    Say ("  [INFO] gameshot screen classes ({0} of {1} shots, stride {2}): {3}" -f ($cnt.Values | measure -Sum).Sum,$gs.Count,$stride,(($cnt.GetEnumerator() | sort Name | % { "$($_.Name)=$($_.Value)" }) -join ' '))
    foreach($seg in ($tl | select -Last 40)){ Say ("  [INFO] screen {0,-11} {1}-{2} ({3} shots)" -f $seg.lab,$seg.start,$seg.end,$seg.n) }
    $lastRep=@($tl | ? { $_.lab -eq 'REPLAY' }); $afterRep=$false; if($lastRep.Count){ $ri=[array]::LastIndexOf(@($tl | % { $_.lab }),'REPLAY'); $afterRep=($ri -lt $tl.Count-1 -and @($tl[($ri+1)..($tl.Count-1)] | ? { $_.lab -eq 'OTHER' -and $_.n -ge 3 }).Count -gt 0) }
    if($lastRep.Count){ Chk $(if($afterRep){'PASS'}else{'WARN'}) 'a screen other than REPLAY / BLACK follows the last REPLAY segment (replay was left)' $(if($afterRep){'yes'}else{'no: the run ended on the replay (step174 runAGU1 case: ENTER does not leave it; -Debrief uses Esc)'}) }
  } else { Chk 'WARN' 'gameshots' 'none: no screen timeline' }
  # the game / producer side
  $po=Read-Matching $xl 'event=dbr3d-option'; Chk $(if($po.Count -and $po[0] -match 'ok=1' -and $po[0] -match 'prerequisites=ok'){'PASS'}elseif($po.Count){'WARN'}else{'FAIL'}) 'dbr3d-option ok=1;prerequisites=ok (vrn:1 or dbr3d:1 in the cfg)' $(if($po.Count){ $po[0].Substring($po[0].IndexOf('event=dbr3d-option'),[math]::Min(220,$po[0].Length-$po[0].IndexOf('event=dbr3d-option'))) }else{ 'no option line' })
  $ds=Read-Matching $ec 'summary-dbr3d;armed='
  if(-not $ds.Count){ Chk 'FAIL' 'summary-dbr3d (ee-camera.log)' 'no lines (dbr3d not configured, or the log is from an older build)' }
  else { $q=Get-Kv $ds[$ds.Count-1]; $am=Kvmax $ds 'armed'; $ar=Kvmax $ds 'arms'; $sc=Kvmax $ds 'screens'; $fr=Kvmax $ds 'frames'; $pu=Kvmax $ds 'published'; $mp=Kvmax $ds 'map-draws'; $pp=Kvmax $ds 'panel-draws'; $ix=Kvmax $ds 'indexed'; $rj=Kvmax $ds 'rejected'
    Chk $(if($am -ge 1 -and $ar -ge 1 -and $sc -ge 1){'PASS'}else{'FAIL'}) 'summary-dbr3d armed=1, arms >= 1, screens >= 1 (the debrief was seen)' "$($ds.Count) lines; max armed $am arms $ar screens $sc"
    Chk $(if($pu -gt 0 -and $fr -gt 0){'PASS'}elseif($am -ge 1){'FAIL'}else{'INFO'}) 'summary-dbr3d frames / published grow while armed' "max frames $fr usable $(Kvmax $ds 'usable') published $pu rejected $rj"
    Chk $(if($mp -gt 0 -and $pp -gt 0){'PASS'}elseif($am -ge 1){'FAIL'}else{'INFO'}) 'summary-dbr3d map-draws, panel-draws (indexed) > 0' "map-draws $mp panel-draws $pp indexed $ix vertices $(Kvmax $ds 'vertices') culled $(Kvmax $ds 'culled') clipped $(Kvmax $ds 'clipped') gaps $(Kvmax $ds 'gaps') refused-draws $(Kvmax $ds 'refused-draws')"
    Say ("  [INFO] last summary-dbr3d: " + ($ds[$ds.Count-1] -replace '^[\d.]+\|','')) }
  $us=Read-Matching $ec 'summary-ui3d;frames='; if($us.Count){ $q=Get-Kv $us[$us.Count-1]; Chk 'INFO' 'summary-ui3d (last line)' ("frames {0} usable {1} refused {2} published {3} rejected {4} last-why {5}; max published {6}" -f $q.frames,$q.usable,$q.refused,$q.published,$q.rejected,$q['last-why'],(Kvmax $us 'published')) }
  $ev="$E\logs\diagnostics.evidence.log"; $cf=@(Read-Matching $ev 'event=ui3d-census;kind=frame'); Chk 'INFO' 'ui3d-census frame lines (re-armed every 15 s; the debrief screens are the later ones)' "$($cf.Count) frame lines; last: $(if($cf.Count){ $cf[$cf.Count-1] -replace '^.*event=ui3d-census;','' })"
  $bo=Read-Matching $ec 'briefing-observe;event=(window|top|rest)'; $bc=Read-Matching $ec 'brief3d-capture'; Chk 'INFO' 'briefing-observe / brief3d-capture (read only)' "observe lines $($bo.Count), capture lines $($bc.Count)"
  $pn=Read-Matching $xl 'event=png-screen;'; $pe=Read-Matching $xl 'event=png-eye-armed'
  $sq=@($pn | ? { $_ -match 'part=screen' -and $_ -match 'outcome=queued' }).Count; $eq=@($pn | ? { $_ -match 'part=eye0-table' -and $_ -match 'outcome=queued' }).Count
  Chk 'INFO' 'png dumps' "png-screen: screen PNG queued $sq, eye0-table PNG queued $eq; png-eye-armed lines $($pe.Count) ($(@($pe | ? { $_ -match 'outcome=armed' }).Count) armed); files in logs\png: $(@(Get-ChildItem "$E\logs\png" -Filter *.png -ea 0).Count)"
  Say "  [LOOK] upright\*-screen.png (the flat debrief image: results panel over the wireframe map) and upright\*-eye0*.png / *eye*.png (the eye views: floating panel + wraparound map when dbr3d armed); shots\debrief-*.png (game window); compare the dump times with steps.txt DEBRIEF hold lines."
}
function Analyze-Run($r,[string]$E){
  $script:Out.Clear(); Say ""; Say ("=== {0} {1} ({2}) {3}" -f $r.Tag,$r.Kind,(Get-Date -f 'yyyy-MM-dd HH:mm'),$r.Desc); Say "    evidence: $E"
  $script:AKind=$r.Kind; Analyze-Common $E
  if($r.Kind -eq 'brief3d'){ Analyze-Brief3d $E; return }
  if($r.Kind -eq 'debrief'){ Analyze-Debrief $E; return }
  if($script:NoFlight -and $r.Kind -ne 'ui3d'){ Say "  [FAIL] no flight: feature checks skipped"; return }
  Analyze-E6 $E
  switch($r.Kind){ 'sun'{ Analyze-Sun $E; Analyze-Lensflare $E $true $false } 'flarecam'{ Analyze-Lensflare $E ($r.Tag -ne 'AGF3') ($r.Tag -eq 'AGF3') } 'boxes'{ Analyze-Boxes $E } 'clouds'{ Analyze-Clouds $E } 'cloudcam'{ Analyze-CloudTrig $E }'ui3d'{ Analyze-Ui3d $E } }
  Analyze-FxAdd $E   # (P) only when the run had fxadd:1 (option line)
}

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- analysis-only mode
# Wide counter search (ee_counter_search.h, ';stwatch:1'): 'store-search;event=survivors;...;list=<blockoff>(ee=..|rel=..|w=124f|v=46|d=4);...' lines (both logs carry them).
# The counter = a survivor whose total decrease d equals the number of taps and whose value v = target-d. The LAST survivors line holds the final state.
function Analyze-StoreSearch([string]$E,[int]$TapCount){
  $sv=@{}; foreach($f in 'xr-stereo.log','ee-camera.log'){ foreach($l in (Read-Matching "$E\logs\$f" 'store-search;event=survivors;')){ $m=[regex]::Match($l,'event=survivors;scan=(\d+);t-ms=(\d+);target=(\d+);alive=(\d+);moved=(\d+);groups=(\d+);list=(.*)$'); if($m.Success){ $sv["$($m.Groups[1].Value)"]=[pscustomobject]@{scan=[int]$m.Groups[1].Value;t=[double]$m.Groups[2].Value;target=[int]$m.Groups[3].Value;alive=[int]$m.Groups[4].Value;moved=[int]$m.Groups[5].Value;groups=[int]$m.Groups[6].Value;list=$m.Groups[7].Value} } } }
  # the search's lines go to ee-camera.log (step168: xr-stereo.log held none, which looked like a dead search); read both
  $sm=@(); foreach($f in 'ee-camera.log','xr-stereo.log'){ $sm+=@(Read-Matching "$E\logs\$f" 'store-search;event=summary;') }
  if(-not $sm.Count){ Say "  [INFO] no store-search summary line in ee-camera.log/xr-stereo.log: the wide search did not run (';stwatch:1' missing or no E6 lock)"; return }
  $last=$sm[-1]; Say ("  [INFO] store-search {0} summary lines; last: {1}" -f $sm.Count,(($last -split 'event=summary;')[1]))
  $tr=@(); foreach($f in 'ee-camera.log','xr-stereo.log'){ foreach($l in (Read-Matching "$E\logs\$f" 'store-search;event=track;')){ $m=[regex]::Match($l,'event=track;scan=(\d+);t-ms=(\d+);list=(.*)$'); if($m.Success){ $tr+=[pscustomobject]@{scan=[int]$m.Groups[1].Value;t=[double]$m.Groups[2].Value;list=$m.Groups[3].Value} } } }
  if($tr.Count){ Say "  [INFO] tracked u8 offsets (scan, t-ms: off=value), $($tr.Count) lines:"; foreach($x in ($tr | sort scan -Unique | select -First 40)){ Say ("    scan {0} t={1}: {2}" -f $x.scan,$x.t,$x.list) } }
  $S=@($sv.Values | sort scan); if(-not $S.Count){ Chk 'WARN' 'store-search survivors' 'no survivor line: no 50-valued u8/u16/u32/float position ever counted down (the HUD count is not a plain small int in this block, or the missiles were never launched; check -FireSalvo and the gameshots)'; return }
  $fin=$S[-1]; $entries=@(); foreach($e in ($fin.list -split ';(?=[0-9A-F]+\()')){ $m=[regex]::Match($e,'^([0-9A-F]+)\(ee=(\w+)\|rel=([+-]\d+)\|w=(\w+)\|v=(\d+)\|d=(\d+)\)'); if($m.Success){ $entries+=[pscustomobject]@{blk=$m.Groups[1].Value;ee=$m.Groups[2].Value;rel=[int]$m.Groups[3].Value;w=$m.Groups[4].Value;v=[int]$m.Groups[5].Value;d=[int]$m.Groups[6].Value} } }
  Say ("  [INFO] store-search: {0} survivor lines, final (scan {1}, target {2}): alive {3}, moved {4}, {5} offsets" -f $S.Count,$fin.scan,$fin.target,$fin.alive,$fin.moved,$fin.groups)
  foreach($x in $entries){ Say ("    block+0x{0} ee=0x{1} rel-to-E6-structure={2:+#;-#;0} width={3} value={4} decrease={5}" -f $x.blk,$x.ee,$x.rel,$x.w,$x.v,$x.d) }
  if($TapCount -gt 0){ $hit=@($entries | ? { $_.d -eq $TapCount -and $_.v -eq ($fin.target-$TapCount) })
    if($hit.Count){ Chk 'PASS' 'store-search counter survivors (value fell by exactly the taps)' ((($hit | % { "block+0x$($_.blk) ee=0x$($_.ee) rel=$($_.rel) w=$($_.w) v=$($_.v)" }) -join ' | ') + ". Verify against the gameshots (HUD MSSL $($fin.target) -> $($fin.target-$TapCount)); wire with ';stctr:<blk hex>,<width 1|2|4>'. The address is stable only if it repeats in a second run (E6-relative or fixed EE).") }
    else { Chk 'FAIL' 'store-search counter survivors' "no survivor fell by exactly $TapCount (taps) to $($fin.target-$TapCount); listed above are partial movers (d != taps)" } }
  else { Chk 'WARN' 'store-search survivors' 'no taps logged: compare the survivors with the HUD count by hand' }
}
function Analyze-MslHud([string]$E,$Taps){
  # MSL-HUD (hud_msl.h, aircraft-auto.md 14): the HUD MSSL count read from the game's glyph quads (step5-producer.log), its launch events vs the taps
  $W=[math]::Max($StoreWindow,2.5)
  Say "  --- MSL-HUD: summary-msl-hud / msl-hud events (step5-producer.log) vs taps (window 0..$W s)"
  $pl="$E\logs\step5-producer.log"
  $sm=(Read-Matching $pl 'event=summary-msl-hud;')
  if(-not $sm.Count){ Chk 'FAIL' 'msl-hud running' "no summary-msl-hud line in step5-producer.log: mslhud is OFF by default (pass -CockpitAppend ';mslhud:2' or ';mslhud:1'), no xr_cockpit_model, the HUD replay off, or a DLL older than the MSL-HUD build"; return }
  $vals=@(); foreach($l in $sm){ $m=[regex]::Match($l,';t-ms=(\d+);.*;value=(\w+);last=(\w+);sp=(-?\d+);dmg=(-?\d+);anchor=(\w+);.*;why=(.*)$'); if($m.Success){ $vals+=[pscustomobject]@{t=[double]$m.Groups[1].Value;value=$m.Groups[2].Value;last=$m.Groups[3].Value;sp=$m.Groups[4].Value;dmg=$m.Groups[5].Value;anchor=$m.Groups[6].Value;why=$m.Groups[7].Value} } }
  Say ("  [INFO] {0} summary lines; last: {1}" -f $sm.Count,(($sm[-1] -split 'event=summary-msl-hud;')[1]))
  $seq=@($vals | % { $_.value } | ? { $_ -ne 'none' }); $chg=@(); $p=$null; foreach($v in $seq){ if($v -ne $p){ $chg+=$v; $p=$v } }
  Say ("  [INFO] stable value sequence (5 s summaries): {0}" -f $(if($chg.Count){ $chg -join ' -> ' }else{ 'none (never decoded)' }))
  $fv=@($vals | ? { $_.value -ne 'none' } | select -First 1); if($fv){ Say ("  [INFO] first decoded: value={0} sp={1} dmg={2} anchor={3} (gameshot g0070 reads MSSL 50, SAAM 8, DMG 0%)" -f $fv[0].value,$fv[0].sp,$fv[0].dmg,$fv[0].anchor) }
  else { $why=@($vals | % { $_.why } | ? { $_ -ne 'none' } | select -Last 3); Chk 'FAIL' 'msl-hud decoded a value' ("never: refusal reasons {0}. Run with ';mslhud:2' and replay the dumps: hud_msl_offline_tests --replay $pl" -f ($why -join ' | ')) }
  $ev=@(); foreach($l in (Read-Matching $pl 'msl-hud;event=')){ $m=[regex]::Match($l,'msl-hud;event=(\w+);old=(\d+);new=(\d+);launches=(\d+);t-ms=(\d+)'); if($m.Success){ $ev+=[pscustomobject]@{kind=$m.Groups[1].Value;old=[int]$m.Groups[2].Value;new=[int]$m.Groups[3].Value;n=[int]$m.Groups[4].Value;t=[double]$m.Groups[5].Value} } }
  foreach($x in $ev){ $tp=@($Taps | ? { ($x.t-$_.tick) -ge 0 -and ($x.t-$_.tick) -le $W*1000 } | sort { $x.t-$_.tick } | select -First 1)
    Say ("    {0} {1}->{2} (launches {3}) t-ms={4}{5}" -f $x.kind,$x.old,$x.new,$x.n,$x.t,$(if($tp){ " [+{0:N2} s after tap {1}]" -f (($x.t-$tp[0].tick)/1000.0),$tp[0].n }else{ ' [no tap within the window]' })) }
  $dumps=(Read-Matching $pl 'msl-hud-dump;'); if($dumps.Count){ Say "  [INFO] $($dumps.Count) glyph dumps (mslhud:2): replay with hud_msl_offline_tests --replay `"$pl`"" }
  $ss=(Read-Matching "$E\logs\xr-stereo.log" 'event=summary-stores;'); if($ss.Count){ Say ("  [INFO] last summary-stores: {0}" -f (($ss[-1] -split 'event=summary-stores;')[1])) }
  $L=@($ev | ? { $_.kind -eq 'launch' }); $nl=($L | measure -Property n -Sum).Sum; if(-not $nl){ $nl=0 }
  if(-not $Taps.Count){ Chk 'WARN' 'msl-hud vs taps' "no taps logged: $nl launch(es) decoded; compare with the gameshots"; return }
  $tapsHit=@($Taps | ? { $t=$_; @($L | ? { ($_.t-$t.tick) -ge 0 -and ($_.t-$t.tick) -le $W*1000 }).Count -gt 0 }).Count
  $v0= if($fv){ [int]$fv[0].value }else{ -1 }; $vEnd= if($chg.Count){ [int]$chg[-1] }else{ -1 }
  if($nl -eq $Taps.Count -and $tapsHit -eq $Taps.Count -and $vEnd -eq $v0-$Taps.Count){ Chk 'PASS' 'msl-hud launches follow the taps' "$nl launches, every tap matched within $W s, value $v0 -> $vEnd (gameshots: MSSL 50 -> 46 for 4 taps)" }
  else { Chk 'FAIL' 'msl-hud launches follow the taps' "launches $nl / taps $($Taps.Count), taps matched $tapsHit, value $v0 -> $vEnd (expected $($v0-$Taps.Count)); check the gameshots and the refusal reasons" }
}
function Analyze-SpHud([string]$E,$MsslTaps){
  # SP-STORES (hud_msl.h StableCount, aircraft-auto.md 15): the HUD special-weapon count (SAAM ---8) -> sp-launch events -> the SP pylons, vs the SPSALVO taps
  $W=[math]::Max($StoreWindow,2.5)
  Say "  --- SP-HUD: summary-msl-hud-sp / msl-hud sp events (step5-producer.log), summary-sp-stores (xr-stereo.log) vs SPSALVO taps (window 0..$W s)"
  $sp=New-Object System.Collections.Generic.List[object]
  foreach($al in @(Get-ChildItem -LiteralPath $E -Filter 'autopilot-*.log' -ea 0 | sort Name)){ foreach($l in (Read-Matching $al.FullName 'SPSALVO tap \d+/\d+ missile tick=\d+')){ $m=[regex]::Match($l,'^T=\s*([\d.,]+) SPSALVO tap (\d+)/(\d+) missile tick=(\d+)'); if($m.Success){ $sp.Add([pscustomobject]@{n=[int]$m.Groups[2].Value;T=[double]($m.Groups[1].Value -replace ',','');tick=[double]$m.Groups[4].Value}) } }
    foreach($l in (Read-Matching $al.FullName 'SPSWITCH tap')){ Say "    $l" } }
  $pl="$E\logs\step5-producer.log"
  $ss=(Read-Matching $pl 'event=summary-msl-hud-sp;')
  if(-not $ss.Count){ if($sp.Count){ Chk 'FAIL' 'sp-hud running' "no summary-msl-hud-sp line in step5-producer.log: spstores:0, mslhud:0, stores:0, or a DLL older than the SP-STORES build" } else { Say "  [INFO] no summary-msl-hud-sp line (spstores:0 / mslhud:0 / old DLL) and no SPSALVO taps: nothing to check" }; return }
  Say ("  [INFO] {0} sp summary lines; last: {1}" -f $ss.Count,(($ss[-1] -split 'event=summary-msl-hud-sp;')[1]))
  $seq=@(); foreach($l in $ss){ $m=[regex]::Match($l,'sp-value=(\w+);'); if($m.Success -and $m.Groups[1].Value -ne 'none'){ $seq+=$m.Groups[1].Value } }; $chg=@(); $p=$null; foreach($v in $seq){ if($v -ne $p){ $chg+=$v; $p=$v } }
  Say ("  [INFO] SP stable value sequence (5 s summaries): {0}" -f $(if($chg.Count){ $chg -join ' -> ' }else{ 'none (never decoded: the SP field was dashes or not in the font table)' }))
  $ev=@(); foreach($l in (Read-Matching $pl 'msl-hud;event=sp-')){ $m=[regex]::Match($l,'msl-hud;event=sp-(\w+);old=(\d+);new=(\d+);launches=(\d+);t-ms=(\d+)'); if($m.Success){ $ev+=[pscustomobject]@{kind=$m.Groups[1].Value;old=[int]$m.Groups[2].Value;new=[int]$m.Groups[3].Value;n=[int]$m.Groups[4].Value;t=[double]$m.Groups[5].Value} } }
  foreach($x in $ev){ $tp=@($sp | ? { ($x.t-$_.tick) -ge 0 -and ($x.t-$_.tick) -le $W*1000 } | sort { $x.t-$_.tick } | select -First 1)
    Say ("    sp-{0} {1}->{2} (launches {3}) t-ms={4}{5}" -f $x.kind,$x.old,$x.new,$x.n,$x.t,$(if($tp){ " [+{0:N2} s after SP tap {1}]" -f (($x.t-$tp[0].tick)/1000.0),$tp[0].n }else{ ' [no SP tap within the window]' })) }
  $st=(Read-Matching "$E\logs\xr-stereo.log" 'event=summary-sp-stores;'); if($st.Count){ Say ("  [INFO] last summary-sp-stores: {0}" -f (($st[-1] -split 'event=summary-sp-stores;')[1])) } else { Say "  [INFO] no summary-sp-stores line in xr-stereo.log (the model has no sp<station> parts: bake with the SP-STORES baker, or spstores:0)" }
  if(-not $sp.Count){ Chk 'INFO' 'sp taps' "none: no 'SPSALVO tap' line (run AGB2 with -FireSp N [-SpSwitchKey K]); the SP count sequence above is whatever the flight did. UNVERIFIED live."; return }
  $L=@($ev | ? { $_.kind -eq 'launch' }); $nl=($L | measure -Property n -Sum).Sum; if(-not $nl){ $nl=0 }
  $hit=@($sp | ? { $t=$_; @($L | ? { ($_.t-$t.tick) -ge 0 -and ($_.t-$t.tick) -le $W*1000 }).Count -gt 0 }).Count
  $v0= if($seq.Count){ [int]$seq[0] }else{ -1 }; $vEnd= if($chg.Count){ [int]$chg[-1] }else{ -1 }
  # the wrong weapon fired (no switch key / switch not applied): the MSSL count fell by the SP taps and the SP count never moved
  $mslEv=@(); foreach($l in (Read-Matching $pl 'msl-hud;event=launch;')){ $m=[regex]::Match($l,'launches=(\d+);t-ms=(\d+)'); if($m.Success){ $mslEv+=[pscustomobject]@{n=[int]$m.Groups[1].Value;t=[double]$m.Groups[2].Value} } }
  $mslAfter=@($mslEv | ? { $mv=$_; @($sp | ? { ($mv.t-$_.tick) -ge 0 -and ($mv.t-$_.tick) -le $W*1000 }).Count -gt 0 }).Count   # ($mv, not $e: PowerShell names are case-insensitive and $E is the run directory)
  if($nl -eq 0 -and $mslAfter -gt 0){ Chk 'FAIL' 'sp taps fired the special weapon' "the SP taps moved the MSSL count ($mslAfter launch event(s) within $W s of an SP tap) and the SP count never fell: the missile key fired MISSILE, not the special weapon. Find the weapon-select key on the in-game Controls screen and pass -SpSwitchKey." }
  elseif($nl -eq $sp.Count -and $hit -eq $sp.Count -and $vEnd -eq $v0-$sp.Count){ Chk 'PASS' 'sp launches follow the SP taps' "$nl sp launches, every tap matched within $W s, SP value $v0 -> $vEnd. Now check summary-sp-stores (hidden-mask / mask-changes) and the gameshots/eye dumps: the SP pylons must empty and return after strel s." }
  else { Chk 'FAIL' 'sp launches follow the SP taps' "sp launches $nl / SP taps $($sp.Count), taps matched $hit, SP value $v0 -> $vEnd (expected $($v0-$sp.Count)); check the gameshots (SAAM count) and the refusal reasons" }
}
function Analyze-Stores([string]$E){
  Say "  --- STORES: store-watch decrements vs autopilot missile taps (window 0..$StoreWindow s after a tap; aircraft-auto.md 13)"
  # taps: 'SALVO tap n/N missile tick=<ms>' lines of autopilot-*.log (tick = the game's TickCount64 clock, the same as store-watch t-ms)
  $taps=New-Object System.Collections.Generic.List[object]
  foreach($al in @(Get-ChildItem -LiteralPath $E -Filter 'autopilot-*.log' -ea 0 | sort Name)){ foreach($l in (Read-Matching $al.FullName 'SALVO tap \d+/\d+ missile tick=\d+')){ $m=[regex]::Match($l,'^T=\s*([\d.,]+) SALVO tap (\d+)/(\d+) missile tick=(\d+)'); if($m.Success){ $taps.Add([pscustomobject]@{n=[int]$m.Groups[2].Value;T=[double]($m.Groups[1].Value -replace ',','');tick=[double]$m.Groups[4].Value;line=$l}) } } }
  # decrements: both logs carry the same lines; de-duplicate on off + t-ms
  $dec=@{}; foreach($f in 'xr-stereo.log','ee-camera.log'){ foreach($l in (Read-Matching "$E\logs\$f" 'store-watch;event=decrement;')){ $m=[regex]::Match($l,'store-watch;event=decrement;off=([+-]?\d+);addr=\w+;old=(\d+);new=(\d+);changes=(\d+);t-ms=(\d+)'); if($m.Success){ $k="$($m.Groups[1].Value)@$($m.Groups[5].Value)"; if(-not $dec.ContainsKey($k)){ $dec[$k]=[pscustomobject]@{off=[int]$m.Groups[1].Value;old=[int]$m.Groups[2].Value;new=[int]$m.Groups[3].Value;changes=[int]$m.Groups[4].Value;t=[double]$m.Groups[5].Value} } } } }
  $D=@($dec.Values | sort t,off); $sm=Read-Matching "$E\logs\xr-stereo.log" 'store-watch;event=summary;'; $ft=Read-Matching "$E\logs\xr-stereo.log" 'event=store-watch;kind=fire-wedge;state=on'
  Say ("  [INFO] taps {0} (autopilot SALVO lines), decrement lines {1} over {2} offsets, store-watch summaries {3}, fire-wedge ON edges {4} (tac:1)" -f $taps.Count,$D.Count,@($D | group off).Count,$sm.Count,$ft.Count)
  if(-not $sm.Count){ Chk 'FAIL' 'store-watch running' "no store-watch summary line: ';stwatch:1' not in -CockpitAppend, or the E6 locator never locked" }
  if(-not $taps.Count){ Chk 'WARN' 'missile taps' "none: no 'SALVO tap' line in autopilot-*.log (run AGB2 with -FireSalvo N; step165/166 evidence predates it). Decrements below are listed with their periodicity only." }
  foreach($t in $taps){ Say ("    tap {0}/{1} T={2:N1} tick={3}" -f $t.n,$taps.Count,$t.T,$t.tick) }
  Analyze-MslHud $E $taps
  Analyze-SpHud $E $taps
  Analyze-StoreSearch $E $taps.Count
  if(-not $D.Count){ Chk 'WARN' 'decrements' 'none: the 32 KiB store-watch saw no word count down 1..99 (counter outside the window or not a plain int; see the wide store-search survivors above)'; return }
  $res=@(); foreach($g in ($D | group off)){
    $ev=@($g.Group | sort t); $n=$ev.Count; $hit=@(); $ivs=@()
    for($i=1;$i -lt $n;$i++){ $ivs+=($ev[$i].t-$ev[$i-1].t)/1000.0 }
    foreach($x in $ev){ $tp=@($taps | ? { ($x.t-$_.tick) -ge 0 -and ($x.t-$_.tick) -le $StoreWindow*1000 } | sort { $x.t-$_.tick } | select -First 1); if($tp){ $hit+=[pscustomobject]@{x=$x;tap=$tp[0];dt=($x.t-$tp[0].tick)/1000.0} } }
    $tapsHit=@($hit | % { $_.tap.n } | sort -Unique).Count
    $per=$false; $pmean=0.0; $pcv=0.0
    if($ivs.Count -ge 3){ $pmean=($ivs | measure -Average).Average; $sd=[math]::Sqrt((($ivs | % { ($_-$pmean)*($_-$pmean) }) | measure -Sum).Sum/$ivs.Count); if($pmean -gt 0){ $pcv=$sd/$pmean }; $per=($pcv -lt 0.05) }   # constant period = timer-like noise (15 s in step166)
    $stepOk=(@($ev | ? { $_.new -eq $_.old-1 }).Count -eq $n)
    $cls= if($taps.Count -gt 0 -and $tapsHit -ge [math]::Max(1,[math]::Ceiling(0.6*$taps.Count)) -and $hit.Count -ge [math]::Ceiling(0.8*$n) -and -not ($per -and $n -gt $taps.Count)){ 'TAP-MATCH' } elseif($per){ 'PERIODIC-NOISE' } elseif($taps.Count -gt 0 -and $hit.Count -gt 0){ 'PARTIAL' } else { 'UNRELATED' }
    $res+=[pscustomobject]@{off=$g.Name;n=$n;hit=$hit.Count;tapsHit=$tapsHit;per=$per;pmean=$pmean;pcv=$pcv;stepOk=$stepOk;cls=$cls;ev=$ev;hits=$hit} }
  Say ("  {0,-7} {1,3} {2,4} {3,9} {4,-15} {5}" -f 'off','n','hit','taps-hit',"class","sequence old>new (t-ms) [dt after tap s]")
  foreach($r in ($res | sort @{e={ if($_.cls -eq 'TAP-MATCH'){0}elseif($_.cls -eq 'PARTIAL'){1}elseif($_.cls -eq 'UNRELATED'){2}else{3} }},off)){
    $seq=(($r.ev | select -First 8 | % { $x=$_; $h=@($r.hits | ? { $_.x -eq $x }); "{0}>{1} ({2}){3}" -f $x.old,$x.new,$x.t,$(if($h){ " [+{0:N2}]" -f $h[0].dt }else{ '' }) }) -join '; ')+$(if($r.n -gt 8){ "; ... (+$($r.n-8))" }else{ '' })
    Say ("  {0,-7} {1,3} {2,4} {3,4}/{4,-4} {5,-15} {6}" -f $r.off,$r.n,$r.hit,$r.tapsHit,$taps.Count,$r.cls,$seq)
    if($r.per){ Say ("          constant period {0:N2} s (cv {1:P1}) = timer-like noise, not launches" -f $r.pmean,$r.pcv) } }
  $match=@($res | ? { $_.cls -eq 'TAP-MATCH' })
  if($taps.Count){ if($match.Count){ Chk 'PASS' 'candidate counter offsets (decrements follow the taps, not periodic)' (($match | % { "off=$($_.off) ($($_.hit)/$($_.n) decrements after taps, $($_.tapsHit)/$($taps.Count) taps, old>new step 1: $($_.stepOk))" }) -join ' | ') } else { Chk 'FAIL' 'candidate counter offsets' 'no offset whose decrements follow the taps (counter outside the window, not a plain int, or the missiles were not launched: check the HUD/gameshots)' } }
  $noise=@($res | ? { $_.cls -eq 'PERIODIC-NOISE' }); if($noise.Count){ Chk 'INFO' 'periodic noise offsets (constant period)' (($noise | % { "off=$($_.off) period $('{0:N1}' -f $_.pmean) s x$($_.n)" }) -join ' | ') }
  Say "  [NOTE] AGB2 flies the J35 (Draken): the HUD MSL count is shown in the HUD but not logged; a counter offset is confirmed only if old>new steps equal the taps AND gameshots show the HUD count falling. A gun burst must not move it."
}

if($Analyze){
  $script:Root_ok=$false; $r=@{Tag=(Split-Path $Analyze -Leaf);Kind=$Kind;Desc='(re-analysis)'}
  $script:Out.Clear(); Say "=== $($r.Tag) $Kind"; $script:AKind=$Kind
  if($Kind -eq 'stores'){ Analyze-Stores $Analyze; return }   # no E6/flight checks: only the launch-signal correlation
  Analyze-Common $Analyze
  if($Kind -eq 'brief3d'){ Analyze-Brief3d $Analyze; return }
  if($Kind -eq 'debrief'){ Analyze-Debrief $Analyze; return }
  if($script:NoFlight -and $Kind -ne 'ui3d'){ Say '  [FAIL] no flight: feature checks skipped'; return }
  if($Kind -ne 'common'){ Analyze-E6 $Analyze }
  switch($Kind){ 'sun'{ Analyze-Sun $Analyze; Analyze-Lensflare $Analyze $true $false } 'flarecam'{ Analyze-Lensflare $Analyze (-not $ExpectFlareOff) ([bool]$ExpectFlareOff); Analyze-FlareCam $Analyze $Pair } 'boxes'{ Analyze-Boxes $Analyze } 'clouds'{ Analyze-Clouds $Analyze } 'ui3d'{ Analyze-Ui3d $Analyze } }
  Analyze-FxAdd $Analyze   # (P) only when the run had fxadd:1
  return
}

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- WhatIf
$script:Root_ok=$false
if($WhatIf){
  Write-Host "WhatIf: Batch AG plan (nothing is launched, written or restored)"
  Write-Host ("  repo {0}  evidence root {1}  installed runtime.cfg hash {2}  cfg-U hash {3} (day cfg 1AE3C990)" -f $Repo,$Root,(Hash8 $rt),(Hash8 $BaseCfgPath))
  Write-Host ("  installed dxgi {0} (expected {3}) d3d12 {1} (expected {4})  vrsettings {2}" -f (Hash8 "$GameDir\dxgi.dll"),(Hash8 "$GameDir\d3d12.dll"),(Hash8 $vrset),$ExpectDxgi,$ExpectD3d12)
  foreach($r in $plan){ $terr=([regex]::Match($r.Cfg,'(?m)^xr_terrain_stage=([^\r\n]*)')).Groups[1].Value; $keys=@($r.Cfg -split "`r?`n" | ? { $_ -match '^[A-Za-z0-9_]+=' }).Count
    Write-Host ("  {0,-5} {1,-9} {2}" -f $r.Tag,$r.Kind,$r.Desc); Write-Host ("        AP: {0}   killcap: {1}   post-wait {2}   terrain {3} chars, {4} keys" -f $r.AP,$r.Cap,$r.PostWait,$terr.Length,$keys); Write-Host ("        terrain: {0}" -f $terr)
    if($r.CloudTrig){ Write-Host ("        cloudtrig: telemetry-triggered png-eye.now + png-screen.now, max {0} dumps, gap {1} s, look gate {2}" -f $CloudDumps,$CloudDumpGap,$(if($r.CloudNoLook){'off (held camera)'}else{'on (during/after a look tap)'})) } }
  foreach($f in 'null-vr.ps1','sampler.ps1','killcap.ps1','runK3ag.ps1','autopilot.ps1','debrieflib.ps1','cfg-U.cfg'){ Write-Host ("  helper {0}: {1}" -f $f,(Test-Path "$X\$f")) }
  Write-Host ("  window driver {0}: {1}" -f $drv,(Test-Path $drv)); return }

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- preflight
New-Item -ItemType Directory -Force $Root | Out-Null; $script:Root_ok=$true
BL "start; groups $($Only -join ','); runs $(($plan | % { $_.Tag }) -join ',')"
$busy=@(Get-Process acz,vrserver,vrcompositor,vrmonitor,vrstartup -ea 0)
if($busy){ throw "refusing to start: $(($busy | % { $_.Name }) -join ',') is running (one orchestrator, hands off while a game/SteamVR runs)" }
$ppid=(Get-CimInstance Win32_Process -Filter "ProcessId=$PID" -ea 0).ParentProcessId
$other=@(Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" -ea 0 | ? { $_.ProcessId -ne $PID -and $_.ProcessId -ne $ppid -and $_.CommandLine -match 'run[A-Za-z0-9]*batch\.ps1|runAA1\.ps1|runK3(ag)?\.ps1|runBatchAG\.ps1|autopilot\.ps1|runMission\.ps1' })
if($other){ throw "refusing to start: another orchestrator/run script is alive: $(($other | % { $_.CommandLine.Substring(0,[math]::Min(120,$_.CommandLine.Length)) }) -join ' || ')" }
$dx=Hash8 "$GameDir\dxgi.dll"; $d3=Hash8 "$GameDir\d3d12.dll"; BL "installed dxgi $dx d3d12 $d3 (expected $ExpectDxgi / $ExpectD3d12); cfg-U $(Hash8 $BaseCfgPath) (day 1AE3C990); vrsettings $(Hash8 $vrset)"
$ExpectDxgi=$ExpectDxgi.ToUpper().Substring(0,[math]::Min(8,$ExpectDxgi.Length)); $ExpectD3d12=$ExpectD3d12.ToUpper().Substring(0,[math]::Min(8,$ExpectD3d12.Length))
if(-not $AllowOtherBuild -and ($dx -ne $ExpectDxgi -or $d3 -ne $ExpectD3d12)){ throw "installed DLLs (dxgi $dx d3d12 $d3) do not match -ExpectDxgi $ExpectDxgi / -ExpectD3d12 $ExpectD3d12 (pass the build's hashes, e.g. for step134)" }
if((Hash8 $BaseCfgPath) -ne '1AE3C990'){ BL "WARNING cfg-U.cfg hash $(Hash8 $BaseCfgPath) is not the day cfg 1AE3C990" }
$cFree=[math]::Round((Get-PSDrive C).Free/1GB,1); $dFree=[math]::Round((Get-PSDrive D).Free/1GB,1); BL "free space C: $cFree GB, D: $dFree GB"
if($dFree -lt 25){ throw "D: has only $dFree GB free (a batch writes several GB of mirror frames)" }
if($cFree -lt 1.5){ throw "C: has only $cFree GB free (logs, png dumps and the pagefile live there)" }
function Commit-Pct { try { [double](Get-Counter '\Memory\% Committed Bytes In Use' -ea Stop).CounterSamples[0].CookedValue } catch { -1.0 } }
function Wait-Mem([string]$why){ $t0=Get-Date; while($true){ $p=Commit-Pct; if($p -lt 0){ BL "commit % unreadable ($why): continuing"; return }; if($p -lt $MaxMemPct){ BL ("commit {0:N1} % ok ({1})" -f $p,$why); return }
    if(((Get-Date)-$t0).TotalMinutes -ge 5){ throw ("commit {0:N1} % >= {1} % for 5 min before {2}: stop (memory stop rule)" -f $p,$MaxMemPct,$why) }; BL ("commit {0:N1} % >= {1} %: waiting ({2})" -f $p,$MaxMemPct,$why); Start-Sleep 20 } }
$mg=& pwsh -NoProfile -File (Join-Path $Repo 'tools\mem-guard.ps1') -Report 2>&1; $mgx=$LASTEXITCODE; BL "mem-guard -Report exit $mgx (2 = commit >= 88 %: abort)"; ($mg | Select-Object -Last 6) | % { BL "  mem-guard: $_" }
if($mgx -ge 2){ throw "mem-guard reports commit >= 88 %: abort (close the ChatGPT app / Steam web helpers / qBittorrent, then rerun)" }

# byte copy of what is INSTALLED now (may carry ;ui3d:1;u3p:1;u3a:1); restored exactly in the outer finally
$pre="$Root\runtime.cfg.pre-batch"; if(Test-Path $pre){ $old="$pre.prev-$(Get-Date -f yyyyMMdd-HHmmss)"; Move-Item -LiteralPath $pre -Destination $old -Force; BL "an older $pre was kept as $old (compare its hash with the installed cfg if a previous batch aborted)" }
Copy-Item -LiteralPath $rt -Destination $pre -Force; $preHash=(Get-FileHash $pre).Hash
# The null flights (J35) overwrite the remembered aircraft; keep the user's (headset test 6: started as the Draken).
$la=Join-Path (Split-Path $rt) 'last-aircraft.json'; $laPre="$Root\last-aircraft.json.pre-batch"; if(Test-Path $la){ Copy-Item -LiteralPath $la -Destination $laPre -Force } BL "installed runtime.cfg saved: $pre hash $($preHash.Substring(0,8)); vrsettings pre-batch $(Hash8 $vrset)"
$vrPre=(Get-FileHash $vrset).Hash

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- one run
function Invoke-AGRun($r){
  $n=$r.Tag; $E="$Root\run$n"; if(-not $E.StartsWith($Root)){ throw 'bad evidence path' }
  Remove-Item -LiteralPath $E -Recurse -Force -ea 0; New-Item -ItemType Directory -Force "$E\shots" | Out-Null
  Wait-Mem "before $n"
  $jobs=@(); $tail="$LD\ui3d-census-tail.arm"; $now="$LD\png-screen.now"; $arm="$LD\ui3d-census.arm"
  try {
    BL "$n start: $($r.Desc)"; BL "$n AP: $($r.AP)"
    [IO.File]::WriteAllText("$E\run.cfg",$r.Cfg,[Text.Encoding]::ASCII); Copy-Item "$E\run.cfg" $rt -Force
    Get-ChildItem $LD\logs -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
    Remove-Item "$LD\png-burst.now","$LD\png-eye.now","$LD\head.now","$LD\fxdiag.arm",$now,$arm,$tail,"$LD\briefing-observe.arm","$LD\brief3d-capture.arm" -ea 0
    if($TestHmd){ try { Set-Content -Path (Join-Path $env:LOCALAPPDATA 'acz-vr\testhmd-pose.txt') -Value '0 0 0 0' -Encoding ascii } catch {} }
& pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard -Driver $(if($TestHmd){'acz_testhmd'}else{'null'}) | Add-Content "$E\steps.txt"
    Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
    for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
    Start-Sleep 8
    Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
    $kc=@('-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$E\mirror`"") + ($r.Cap -split ' ') + @('-MaxMin',"$($r.MaxMin)")
    Start-Process pwsh -ArgumentList $kc -WindowStyle Hidden
    if($r.CloudTrig){   # telemetry-triggered eye dumps: writes png-eye.now + png-screen.now when off-window cloud cards are live (cloudtrig.ps1); exits on $E\stopN
      $ct=@('-NoProfile','-File',"`"$X\cloudtrig.ps1`"",'-RunDir',"`"$E`"",'-Max',"$CloudDumps",'-MinGap',$CloudDumpGap.ToString([Globalization.CultureInfo]::InvariantCulture)); if($r.CloudNoLook){ $ct+='-NoLookGate' }
      Start-Process pwsh -ArgumentList $ct -WindowStyle Hidden; BL "$n cloudtrig started (max $CloudDumps dumps, gap $CloudDumpGap s)" }
    if($r.Menu){   # png-screen.now every 8 s + census re-arm every 15 s from launch until the flight (autopilot log) exists
      $jobs+=Start-Job { param($E,$arm,$now,$B) $t=0; while(-not (Test-Path "$E\autopilot-1.log") -and $t -lt 600){
          if(($t % 15) -eq 0){ Remove-Item $arm -ea 0; Start-Sleep 3; $t+=3; Set-Content $arm 'AG' -Encoding ascii; Add-Content $B ("{0} [AG] census re-armed" -f (Get-Date -f HH:mm:ss)) }
          if(($t % 8) -eq 0){ Set-Content $now 'AG' -Encoding ascii }
          Start-Sleep 1; $t+=1 }
        Remove-Item $arm,$now -ea 0 } -ArgumentList $E,$arm,$now,"$Root\batchAG.log" }
    if($r.Pause){   # AJ2: flight + 40 s, then Esc pause 20 s / resume, twice; the tail census arms itself for the pause
      Set-Content $tail 'AG' -Encoding ascii
      $jobs+=Start-Job { param($E,$drv,$B) while(-not (Test-Path "$E\autopilot-1.log")){ Start-Sleep 2 }
          Add-Content $B ("{0} [AG] pause helper: autopilot log seen" -f (Get-Date -f HH:mm:ss)); Start-Sleep 40
          foreach($k in 1,2){ & pwsh -NoProfile -File $drv -Action tap -Key back -AcknowledgeLiveInput | Out-Null; Add-Content $B ("{0} [AG] Esc pause {1}" -f (Get-Date -f HH:mm:ss),$k); Start-Sleep 20
            & pwsh -NoProfile -File $drv -Action tap -Key back -AcknowledgeLiveInput | Out-Null; Add-Content $B ("{0} [AG] Esc resume {1}" -f (Get-Date -f HH:mm:ss),$k); Start-Sleep 25 } } -ArgumentList $E,$drv,"$Root\batchAG.log" }
    Start-Sleep 3
    $pw=0; if($r.PostWait){ $pw=[int]$r.PostWait }
    if($r.BriefMap){ & pwsh -NoProfile -File "$X\runK3ag.ps1" -E $E -AP $r.AP -BriefMap -BriefHold $BriefHold -BriefPngEvery $BriefPngEvery $(if($BriefOverviewOnly){'-BriefOverviewOnly'}) $(if($TestHmd){'-HeadSweep'}) }
    elseif($r.Debrief){ & pwsh -NoProfile -File "$X\runK3ag.ps1" -E $E -AP $r.AP -Debrief -DebriefHold $DebriefHold -DebriefPngEvery $DebriefPngEvery -DebriefPageHold $DebriefPageHold -DebriefMaxPages $DebriefMaxPages -DebriefEndWait $DebriefEndWait -DebriefWait $DebriefWait -DebriefQuitConfirms $DebriefQuitConfirms -DebriefProbeConfirm $DebriefProbeConfirm $(if($DebriefQuit){'-DebriefQuit'}) }
    else { & pwsh -NoProfile -File "$X\runK3ag.ps1" -E $E -AP $r.AP -PostWait $pw }
  } catch { BL "$n ERROR $($_.Exception.Message)" }
  finally {
    BL "$n cleanup"
    foreach($j in $jobs){ Stop-Job $j -ea 0; Remove-Job $j -Force -ea 0 }
    New-Item -ItemType File "$E\stopN" -Force | Out-Null
    Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
    Start-Sleep 3
    New-Item -ItemType Directory -Force "$E\logs" | Out-Null
    Get-ChildItem "$LD\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
    Remove-Item "$LD\fxdiag.arm","$LD\png-eye.now","$LD\head.now",$now,$arm,$tail,"$LD\briefing-observe.arm","$LD\brief3d-capture.arm" -ea 0
    Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
    Start-Sleep 3
    if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
    BL "$n vrsettings restored hash $(Hash8 $vrset)"
  }
  $E }

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- batch
$results="$Root\RESULTS.txt"; $aborted=$false; $f2=$null; $f3=$null; $flareVerdictOn=$null
try {
  foreach($r in $plan){
    if($aborted){ break }
    if($r.Conditional){
      if(-not $flareVerdictOn -or $flareVerdictOn -eq 'PASS'){ BL "$($r.Tag) skipped (AGF2 vs AGF3 did not fail: the camera sign is right, or not run)"; continue } }
    Start-Sleep 20
    $E=Invoke-AGRun $r
    if((Test-Path "$E\ROUTE-FAILED") -and -not (Test-Path "$E\GAMEOVER")){   # the menu route never reached a flight (AGF2 2026-10-30): keep that evidence and try the run ONCE more
      BL "$($r.Tag) ROUTE FAILED (no flight): evidence kept as run$($r.Tag).failed1, retrying once"; Remove-Item "$E.failed1" -Recurse -Force -ea 0; Move-Item $E "$E.failed1"; Start-Sleep 30
      $E=Invoke-AGRun $r }
    Analyze-Run $r $E
    if($r.Kind -eq 'flarecam'){
      if($r.Tag -eq 'AGF2'){ $f2=$E }
      if($r.Tag -eq 'AGF3'){ $f3=$E; Analyze-FlareCam $f2 $f3; $flareVerdictOn=$(if(($script:Out | ? { $_ -match '\[PASS\] flare ON vs OFF' })){'PASS'}else{'FAIL'}) }
      elseif($r.Tag -eq 'AGF4'){ Analyze-FlareCam $E $f3 }
      elseif($r.Tag -eq 'AGF2'){ Analyze-FlareCam $E $null } }
    if($r.Kind -eq 'cloudcam'){ Analyze-FlareCam $E $null }   # same eye-dump helper: png-once + image statistics; the verdict is by eye
    $script:Out | Add-Content $results
    if(Test-Path "$E\GAMEOVER"){ BL "$($r.Tag) hit the memory stop rule (GAMEOVER): batch stops here"; $aborted=$true }
  }
} catch { BL "BATCH ERROR $($_.Exception.Message)"; $script:Out | Add-Content $results }
finally {
  Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Remove-Item "$LD\fxdiag.arm","$LD\png-screen.now","$LD\ui3d-census.arm","$LD\ui3d-census-tail.arm","$LD\briefing-observe.arm","$LD\brief3d-capture.arm" -ea 0
  Copy-Item -LiteralPath $pre -Destination $rt -Force
  if(Test-Path $laPre){ Copy-Item -LiteralPath $laPre -Destination $la -Force; BL "last-aircraft.json restored: $(Get-Content $la -Raw)" }
  $same=((Get-FileHash $rt).Hash -eq $preHash); BL "installed runtime.cfg restored from byte copy: hash $(Hash8 $rt) identical=$same"
  $vrNow=(Get-FileHash $vrset).Hash; BL "vrsettings now $($vrNow.Substring(0,8)) pre-batch $($vrPre.Substring(0,8)) identical=$($vrNow -eq $vrPre)"
  BL "[AG-2026-10-30] DONE; results $results"
}
