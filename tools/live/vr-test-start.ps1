# One-shot setup for a headset test: stops fpsVR, installs the proxy DLL set, writes runtime.cfg.
# Then launch the game from Steam, get to the cockpit, put the headset on.  Afterwards run vr-test-stop.ps1.
# Default set = the newest DLLs tested on the null HMD (step40-dll) with the recommended key set.
param(
  [string]$DllDir = '%USERPROFILE%\.agent-jobboard-ps2-vr-support\measurements\step40-dll',
  [switch]$NoHud,          # omit the HUD layer keys
  [switch]$LensFov,        # keep the old 63x38 cockpit rectangle (omit hmd_fov)
  [switch]$NoMembers,      # omit members_by_class (first headset test behaviour: 7 cockpit draws)
  # J-world-in-projection (test 7): composite the world INTO the eye images under the cockpit instead of a world quad layer
  # (hypothesis: SteamVR draws quad layers above projection layers). Default off.
  [switch]$NoWorldInProjection,   # test 7 default is ON: world composited into the eye images (J-world-in-projection)
  [switch]$NoWorldObjects,        # omit gpu_copy_step5_world_objects (3D world object replay, DrawACM_v)
  [switch]$NoEffects,             # omit the effects layer keys (gpu_copy_step5_effects + xr_effects_layer: tracers/missile smoke/explosions between cockpit parts)
  [switch]$EffectsDual,           # add gpu_copy_step5_effects_dual (effects layer models the game's dual-source alpha blend; coverage_v1 image; default off; needs effects)
  [switch]$NoHudMarkers,          # omit the separate target-marker layer (J-hud-markers)
  [switch]$HudRadar,              # add gpu_copy_step5_hud_radar (the GAME'S OWN radar contacts in the HUD layer: segmented HUD region + scene-barrier resume, docs/world-render/radar-markers-game-original.md; default off; 32 keys with the 31-key day set, so not with -Lean)
  [switch]$PhaseObserve,          # add gpu_copy_step5_phase_observe (read-only per-draw frame-phase observer for missiles/explosions/tracers/radar; default off)
  [switch]$WorldObserve,          # add gpu_copy_step5_world_observe (read-only DrawACM_v list/target/Execute observer; default off)
  [switch]$NoWorldHold,           # omit xr_world_hold_last (default ON since test 8 feedback: flicker)
  [switch]$WorldHoldLegacy,             # add xr_world_hold_last (keep the last good world for 750 ms when a frame has none, fading to black; default off)
  [switch]$NoWorldExtend,         # omit xr_world_extend (default ON since test 8 feedback: black box around the world)
  [switch]$WorldCache,            # add xr_world_cache (periphery fill v2: world-registered environment cache + horizon-fit sky/ground fill; REPLACES xr_world_extend and the feather; needs xr_world_in_projection; default off; works best with -LookDrive; docs/review/world-cache-impl.md)
  [switch]$WorldExtendLegacy,         # add xr_world_extend (fill the periphery outside the world window with the clamped, darkened edge; default off)
  [switch]$DepthFix,              # add xr_world_objects_depth_fix (J-test8-fixes: replayed world objects write a compressed depth so cockpit pixels always win; default off; needs world objects)
  [switch]$OcclusionFix,          # add xr_world_window_occlude (J-occlusion-fix: hide replayed world objects INSIDE the world window, keep them in the periphery; IMPLIES -DepthFix; default off; needs world objects + world-in-projection)
  [switch]$PngOnce,              # add xr_png_once (J-test8-fixes: ONE capture of the final composited eye-0 image + coverage + world/effects PNGs into logs\png; default off)
  [ValidateSet('0','1','2','3','4')][string]$DebugView = '0',  # xr_debug_view (J-test8-fixes: 1 coverage grey, 2 cockpit colour, 3 world layer, 4 false colour; replaces the eye output, diagnostics only)
  [switch]$LookDrive,             # add xr_look_drive_keys (drive the game's look from the head with the NumPad look keys; needs the game window in the foreground; default off; kill switch: create look-drive.stop in the acz-vr folder of LOCALAPPDATA)
  [switch]$XInputLook,            # with -LookDrive: send the look as RIGHT-STICK input through the skip-intro XINPUT1_4.dll proxy (adds xr_look_drive_xinput; no window-focus dependence; needs look_xinput=1 in skip-intro.ini, Player 1 = CONTROLLER, a controller connected; kill switch look-drive.stop)
  [switch]$LookClosedLoop,        # with -LookDrive: opt in to the measured-look integral/gain correction (adds xr_look_drive_closed_loop; the default drive is open loop)
  [switch]$LookRefCluster,        # with -LookDrive: add xr_look_ref_cluster (look reference = majority orientation cluster of the replayed members; default off; the look-ref-diag log line is written whenever -LookDrive is on)
  [string]$NeckLever = '',        # add gpu_copy_step5_neck_lever_m=<m> (0.05..0.20, suggested 0.12): subtract the neck-pivot arc from the head position; world/marker quads follow the virtual eye. Combine with -HudFollow 1 for the HUD
  [string]$TranslationScale = '', # second value of gpu_copy_step5_neck_lever_m (0..1, default 1; 0 = rotation-only). Alone it writes lever 0 + this scale
  [ValidateSet('0','1','2')][string]$HudFollow = '0',  # xr_hud_follow: 1 = HUD fixed relative to the compensated virtual eye (only differs from today with -NeckLever/-TranslationScale), 2 = also turned by a rate-limited half of the look (needs the HUD layer)
  [switch]$LookSweepNoXr,         # NO-XR look-drive sweep test (no SteamVR/XR): writes ONLY the no-XR key set (log_enabled ... xr_look_drive_keys, xr_look_drive_test_sweep) and ignores every other switch except -EeCameraProbe
  [switch]$EeCameraProbe,         # with -LookSweepNoXr: add ee_camera_probe (logs the game's look L next to the measured look)
  [string]$TerrainRender = '',    # J-terrain-render (docs/world-render/terrain-render-impl.md): '<stage>[;opt:val...]', e.g. 'm02a' or 'm02a;yaw:10;mirror:1;blend:1' (use ':' not '=' inside; WORLD-1 options: rsgn: (default -1), fogl:/far: haze, csrc:game|e6, e6:frame|latest, gmax:<deg>, e6loc:auto|probe|content (E6 camera locator, docs/world-render/e6-anchor-free.md), occl:0|1 (object occlusion, active only with -DepthFix), upm: (default = the producer's units per metre); fog0: is refused); writes xr_terrain_render + xr_terrain_stage (+ ee_camera_probe for the E6 camera unless cam:x,y,z,deg is given); needs the world-in-projection composite and a baked <stage>.zvrs in LOCALAPPDATA\acz-vr\stage (research/world-render-data/stage_baker.py)
  [string]$Mission = '',          # STAGES (docs/world-render/stages-and-detection.md): mission number '01'..'18' (optional ace-style letter k/m/n; the variants are identical) or 'SP' -> replaces the stage id of -TerrainRender (e.g. -Mission 07 -TerrainRender 'auto;env:1' -> 'm07a;env:1'). -TerrainRender 'auto[;opt:val]' instead detects the stage at run time (read-only memory scan, needs fingerprints.json from tools/live/install-assets.ps1 -StageDir stage-all)
  [string]$CockpitModel = '',     # COCKPIT-2 (docs/world-render/cockpit-render-impl.md): '<model id>[;opt:val...]', e.g. 'j35jk_vrcockpit' or 'j35jk_vrcockpit;upm:5;replay:1', or 'auto;anim:1;default:j35jk_vrcockpit' (AIRCRAFT-AUTO + AIRCRAFT-AUTO-FAST: the remembered last aircraft, else default:, is drawn from the first XR frame; docs/world-render/aircraft-auto.md) (use ':' inside; options upm: gain: amb: cut: near: ext:0 glass:0 hudglass:1 replay:1 hide:i,j show:i eye:x,y,z; COCKPIT-3: dom:raw light:fixed floor:0; with -CockpitModel the HUD scale defaults to 1); writes ONE key xr_cockpit_model; needs the world-in-projection composite and <id>.zvrm in LOCALAPPDATA\acz-vr\models (tools/live/install-assets.ps1); -DepthFix keeps every replayed world object (without it objects nearer than ~1000 units are dropped with the replayed cockpit)
  [switch]$Ps2Probe,             # PS2 ADDRESS PROBE (docs/world-render/ps2-probe-impl.md): READ-ONLY probe of the EE-image block (mission byte, game id, camera struct signature, E6 comparison), NULL driver / producer-only, NO headset: writes ONLY the probe key set (no xr_stereo_output) and ignores every other switch; kill switch lens-scan.stop in the acz-vr folder of LOCALAPPDATA
  [switch]$LensScan,              # LENS PHASE 0 (docs/review/lens-scan-impl.md): READ-ONLY lens scan, NULL driver / producer-only, NO headset: writes ONLY the scan key set (no xr_stereo_output) and ignores every other switch; kill switch lens-scan.stop in the acz-vr folder of LOCALAPPDATA
  [switch]$LookSweep,             # add xr_look_drive_test_sweep (TEST MODE with -LookDrive: a scripted head sweep replaces the head; for the null HMD)
  [switch]$Lean,                  # FPS investigation (docs/world-render/fps-investigation.md): LEAN production profile = the same render keys + perf_lean (diagnostic log volume cuts only; nothing rendered changes). Every other key of the default set is a required latch or a render feature, so none is dropped. The summary-producer-cost line is logged with or without -Lean.
  # J-quad-yaw: 0.8 m / 1.0 puts the full-lens-angle HUD on the cockpit combiner glass seen from
  # the neutral head (same angular size as in the game; 39.6 units/m scale is an unmeasured assumption).
  [string]$HudDistanceM = '0.5',
  [string]$HudScale = '0.5',
  # Test-4 feedback: cockpit looked slightly oversized. Game units per metre for head offsets (default 39.6); larger = smaller cockpit.
  [string]$UnitsPerMetre = '65',
  # Test-5 feedback: cockpit+HUD at ~0.5; world further out than the cockpit (testing only; a real world render is needed).
  [string]$WorldDistanceM = '500'
)
$ErrorActionPreference = 'Stop'
# COCKPIT-3: with the model cockpit the HUD symbology registers with the gunsight glass and the world only at the game's own angular size (xr_hud_scale 1;
# docs/world-render/cockpit-render-impl.md section 14). The 0.5 default (test-5 feedback) belongs to the replayed cockpit; an explicit -HudScale still wins.
# HUD-MENU (D-362 test 19 "HUD too big"; docs/world-render/hud-menu.md): default 0.8, scaled about the model's gunsight-glass centre (the glass-centre
# symbology stays registered; points 10 deg off it read 2 deg nearer). -HudScale 1.0 restores exact angular registration everywhere.
if ($CockpitModel -and -not $PSBoundParameters.ContainsKey('HudScale')) {
  $HudScale = '0.8'
  Write-Host '-CockpitModel: xr_hud_scale=0.8 (HUD 80% of the game lens angles, pivoted on the gunsight-glass centre); pass -HudScale 1.0 for exact registration'
}
# Test 17 feedback: the HUD moved when turning the head (a near world-quad HUD at 0.5 m shifts by the neck lever: ~15 deg of parallax). A real gunsight HUD is collimated:
# with the model cockpit put the HUD quad far away at the same angular size (xr_hud_scale 1 keeps the game's lens angles at any distance). -HudDistanceM still wins.
if ($CockpitModel -and -not $PSBoundParameters.ContainsKey('HudDistanceM')) {
  $HudDistanceM = '10'
  Write-Host '-CockpitModel: xr_hud_quad_distance_m=10 (collimated HUD: no head-turn parallax; pass -HudDistanceM to override)'
}
Get-Process fpsVR -ErrorAction SilentlyContinue | Stop-Process -Force
$dxgi = Join-Path $DllDir 'dxgi.dll'; $d3d12 = Join-Path $DllDir 'd3d12.dll'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'l1.ps1') -Action install -Proxy $dxgi -D3d12Proxy $d3d12
# Step74: l1.ps1 refuses (and this script used to continue silently) when a proxy is already installed: fail loudly if the installed DLLs are not the requested set.
$gameDir = (Get-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\install-marker.json') -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json -ErrorAction SilentlyContinue).gameDir
if ($gameDir -and (Test-Path -LiteralPath (Join-Path $gameDir 'dxgi.dll'))) {
  $want = (Get-FileHash -LiteralPath $dxgi -Algorithm SHA256).Hash; $have = (Get-FileHash -LiteralPath (Join-Path $gameDir 'dxgi.dll') -Algorithm SHA256).Hash
  if ($want -ne $have) { throw "Installed dxgi.dll ($($have.Substring(0,8))) is not the requested DllDir set ($($want.Substring(0,8))): run tools/live/vr-test-stop.ps1 first, then retry." }
}
if ($LookSweepNoXr) {
  $keys = @('log_enabled=true','diagnostic_ownership_pins=true','gpu_copy_witness_observation=true','gpu_copy_early_device_hook=true',
            'gpu_copy_step3_experiment=true','gpu_copy_step3_whole_set=true','gpu_copy_step5_continuous=true','gpu_copy_step5_members_by_class=true',
            'xr_look_drive_keys=true','xr_look_drive_test_sweep=true')
  if ($EeCameraProbe) { $keys += 'ee_camera_probe=true' }
  Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr
untime.cfg') -Value $keys -Encoding ascii
  "Installed (NO XR, look-drive sweep). $($keys.Count) config keys written:"; $keys
  'Now: launch ACE COMBAT ZERO from Steam, reach the cockpit, keep the GAME WINDOW FOREGROUND (SteamVR not running). Kill switch: create look-drive.stop in LOCALAPPDATAcz-vr. Read step5-producer.log: look-drive; lines.'
  return
}
if ($Ps2Probe) {
  $keys = @('log_enabled=true','diagnostic_ownership_pins=true','gpu_copy_witness_observation=true','gpu_copy_early_device_hook=true',
            'gpu_copy_step3_experiment=true','gpu_copy_step3_whole_set=true','gpu_copy_step5_continuous=true','gpu_copy_step5_members_by_class=true',
            'ee_camera_probe=true','ee_ps2_probe=true')
  Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\runtime.cfg') -Value $keys -Encoding ascii
  "Installed (PS2 ADDRESS PROBE, read-only, no XR). $($keys.Count) config keys written:"; $keys
  'Null SteamVR driver only (never a headset). Launch ACZ, reach a mission COCKPIT view, level flight, look released. Probe starts ~1 s after the tracks-look anchor is stable: scans ~86 MiB (seconds), then samples for 60 s. Result: logs\ps2-probe.txt (last line sha256=) and ps2-probe; lines in ee-camera.log (both under LOCALAPPDATA\acz-vr). Kill switch: create lens-scan.stop in LOCALAPPDATA\acz-vr.'
  return
}
if ($LensScan) {
  $keys = @('log_enabled=true','diagnostic_ownership_pins=true','gpu_copy_witness_observation=true','gpu_copy_early_device_hook=true',
            'gpu_copy_step3_experiment=true','gpu_copy_step3_whole_set=true','gpu_copy_step5_continuous=true','gpu_copy_step5_members_by_class=true',
            'ee_camera_probe=true','ee_lens_scan=true')
  Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\runtime.cfg') -Value $keys -Encoding ascii
  "Installed (LENS SCAN, read-only, no XR). $($keys.Count) config keys written:"; $keys
  'Null SteamVR driver only (never a headset). Launch ACZ, reach a mission COCKPIT view, level flight, look released. Result: logs\lens-scan-candidates.txt (last line sha256=) and lens-scan; lines in ee-camera.log (both under LOCALAPPDATA\acz-vr). Kill switch: create lens-scan.stop in LOCALAPPDATA\acz-vr.'
  return
}
$keys = @('log_enabled=true','diagnostic_ownership_pins=true','gpu_copy_witness_observation=true','gpu_copy_early_device_hook=true',
          'gpu_copy_step3_experiment=true','gpu_copy_step3_whole_set=true','gpu_copy_step5_continuous=true','xr_stereo_output=true')
if ($NoWorldExtend -and -not $WorldCache) { $keys += 'xr_edge_feather=0.06' }   # the extend mode replaces the feather (and keeps the key count low; the cap is 32 since xr_look_drive_keys)
if (-not $NoMembers) { $keys += 'gpu_copy_step5_members_by_class=true' }
if (-not $LensFov) { $keys += 'gpu_copy_step5_hmd_fov=true' }
if (-not $NoHud)   { $keys += @('gpu_copy_step5_hud=true','xr_hud_layer=true',"xr_hud_quad_distance_m=$HudDistanceM","xr_hud_scale=$HudScale") }
if ($WorldDistanceM) { $keys += "xr_world_quad_distance_m=$WorldDistanceM" }
if ($UnitsPerMetre) { $keys += "gpu_copy_step5_units_per_metre=$UnitsPerMetre" }
if (-not $NoWorldInProjection) { $keys += 'xr_world_in_projection=true' }
if (-not $NoWorldObjects -and -not $NoMembers) { $keys += 'gpu_copy_step5_world_objects=true' }
if (-not $NoHudMarkers -and -not $NoHud) { $keys += @('gpu_copy_step5_hud_markers=true','xr_hud_markers_layer=true') }
if (-not $NoEffects -and -not $NoHud -and -not $NoWorldInProjection) { $keys += @('gpu_copy_step5_effects=true','xr_effects_layer=true') }
if ($EffectsDual -and -not $NoEffects -and -not $NoHud -and -not $NoWorldInProjection) { $keys += 'gpu_copy_step5_effects_dual=true' }
if ($WorldObserve) { $keys += 'gpu_copy_step5_world_observe=true' }
if ($HudRadar -and -not $NoHud) { $keys += 'gpu_copy_step5_hud_radar=true' }   # latched only with gpu_copy_step5_hud
if ($PhaseObserve) { $keys += 'gpu_copy_step5_phase_observe=true' }
if (-not $NoWorldHold) { $keys += 'xr_world_hold_last=true' }
if (-not $NoWorldExtend -and -not $WorldCache) { $keys += 'xr_world_extend=true' }
if ($WorldCache -and -not $NoWorldInProjection) { $keys += 'xr_world_cache=true' }   # replaces extend (+feather): the cache path draws its own periphery
if ($WorldCache -and $NoWorldInProjection) { Write-Warning '-WorldCache needs the world-in-projection composite (do not combine with -NoWorldInProjection); the key was not written' }
if ($WorldCache -and -not $LookDrive) { Write-Warning '-WorldCache without -LookDrive: the game window never moves, so the cache only holds the window and the periphery is the sky/ground fill' }
if (($DepthFix -or $OcclusionFix) -and -not $NoWorldObjects -and -not $NoMembers) { $keys += 'xr_world_objects_depth_fix=true' }
if ($OcclusionFix -and -not $NoWorldObjects -and -not $NoMembers -and -not $NoWorldInProjection) { $keys += 'xr_world_window_occlude=true' }
if ($PngOnce) { $keys += 'xr_png_once=true' }
if ($DebugView -ne '0') { $keys += "xr_debug_view=$DebugView" }
if ($LookDrive) { $keys += 'xr_look_drive_keys=true' }
if ($LookDrive -and $LookSweep) { $keys += 'xr_look_drive_test_sweep=true' }
if ($LookDrive -and $XInputLook) { $keys += 'xr_look_drive_xinput=true' }
if ($LookDrive -and $LookClosedLoop) { $keys += 'xr_look_drive_closed_loop=true' }
if ($LookDrive -and $LookRefCluster) { $keys += 'xr_look_ref_cluster=true' }
if ($LookRefCluster -and -not $LookDrive) { Write-Warning '-LookRefCluster has no effect without -LookDrive' }
if ($NeckLever -or $TranslationScale) { $lv = if ($NeckLever) { $NeckLever } else { '0' }; if ($TranslationScale) { $keys += "gpu_copy_step5_neck_lever_m=$lv,$TranslationScale" } else { $keys += "gpu_copy_step5_neck_lever_m=$lv" } }
if ($HudFollow -ne '0') { $keys += "xr_hud_follow=$HudFollow"; if ($NoHud) { Write-Warning '-HudFollow needs the HUD layer (do not combine with -NoHud)' } }
if ($XInputLook -and -not $LookDrive) { Write-Warning '-XInputLook has no effect without -LookDrive' }
if ($Mission) {   # STAGES: mission -> stage (research/world-render-data/mission_stages.json)
  if (-not $TerrainRender) { throw '-Mission needs -TerrainRender (e.g. -TerrainRender ''auto;env:1''; its stage id is replaced)' }
  $multi = @('03','04','08','10','14','16')
  if ($Mission -match '^(?i)sp$') { $mStage = 'r01a' }
  elseif ($Mission -match '^(0[1-9]|1[0-8])([kmn]?)$') { $n = $Matches[1]; $v = $Matches[2]; if ($multi -contains $n) { if (-not $v) { $v = 'k' } } elseif ($v) { throw "-Mission ${Mission}: mission $n has one stage (no k/m/n variant)" } else { $v = 'a' }; $mStage = "m$n$v" }
  else { throw "-Mission '$Mission' is not 01..18 (optionally k/m/n) or SP" }
  $parts = $TerrainRender.Split(';'); $parts[0] = $mStage; $TerrainRender = $parts -join ';'
  Write-Host "mission $Mission -> stage $mStage (-TerrainRender '$TerrainRender')"
}
if ($TerrainRender) {   # J-terrain-render (docs/world-render/terrain-render-impl.md)
  if ($NoWorldInProjection) { Write-Warning '-TerrainRender needs the world-in-projection composite (do not combine with -NoWorldInProjection); the keys were not written' }
  else {
    $stageValue = $TerrainRender.Replace('=', ':')
    if ($stageValue -notmatch '^[A-Za-z0-9_]{1,8}(;[A-Za-z0-9_]+:[A-Za-z0-9_.,-]+)*$') { throw "-TerrainRender '$TerrainRender' is not '<stage>[;option:value...]'" }
    if ($stageValue -match ';fog0:') { throw "-TerrainRender: fog0: was removed (WORLD-1 haze has no start distance); use fogl:<L> and far:<u>" }
    if ($stageValue -notmatch ';occl:0' -and -not ($DepthFix -or $OcclusionFix)) { Write-Host 'note: terrain object occlusion (WORLD-1) needs -DepthFix; without it replayed objects are drawn over the terrain unconditionally' }
    $keys += 'xr_terrain_render=true'; $keys += "xr_terrain_stage=$stageValue"
    # The read-only E6 camera feed runs inside ee_camera_probe. E6-ANCHOR-FREE (docs/world-render/e6-anchor-free.md): option e6loc:auto (default) locks onto the
    # E6 block by its content (no look events, no cockpit basis needed), e6loc:content = locator only, e6loc:probe = the previous probe-base feed.
    if ($stageValue -match ';e6loc:([^;]*)' -and $Matches[1] -notin @('auto','probe','content')) { throw "-TerrainRender: e6loc:$($Matches[1]) is not auto|probe|content" }
    # LOOK-STICK GATE (docs/world-render/look-stick-gate.md, D-355): lookstick:block (default) centres the right stick for the game while in-flight VR frames are
    # composed (needs the skip-intro XINPUT1_4.dll built with the gate; skip-intro.ini xr_block_look_stick=1 default); lookstick:game restores the game's look stick.
    if ($stageValue -match ';lookstick[:=]([^;]*)' -and $Matches[1] -notin @('block','game','0','1')) { throw "-TerrainRender: lookstick:$($Matches[1]) is not block|game" }
    if ($LookDrive -and $XInputLook -and $stageValue -notmatch ';lookstick[:=](game|0)') { Write-Host 'note: -XInputLook with lookstick:block: the physical right stick is centred in flight; only the look-drive offset reaches the game' }
    if ($stageValue -notmatch ';cam:' -and ($keys -notcontains 'ee_camera_probe=true')) { $keys += 'ee_camera_probe=true' }
    if ($stageValue -notmatch ';cam:') { Write-Host 'note: camera feed: read ee-camera.log e6-locate; lines (locked/lost/summary) and xr-stereo.log terrain-camera-feed; (no look input needed)' }
    $stageName = $stageValue.Split(';')[0]
    $stageFile = Join-Path $env:LOCALAPPDATA ('acz-vr\stage\' + $stageName + '.zvrs')
    if ($stageName -eq 'auto') { $stageFile = Join-Path $env:LOCALAPPDATA 'acz-vr\stage\fingerprints.json'; if (-not (Test-Path -LiteralPath $stageFile)) { Write-Warning "stage auto-detection table not found: $stageFile (python -I research/world-render-data/stage_fingerprints.py; tools/live/install-assets.ps1 -StageDir stage-all); the renderer then logs xr-stereo;event=terrain-disabled;reason=auto-table" } }
    elseif (-not (Test-Path -LiteralPath $stageFile)) { Write-Warning "baked stage not found: $stageFile (run: python -I research/world-render-data/stage_baker.py $stageName); the renderer then falls back to the game's own world and logs xr-stereo;event=terrain-disabled" }
  }
}
if ($CockpitModel) {   # COCKPIT-2 (docs/world-render/cockpit-render-impl.md)
  if ($NoWorldInProjection) { Write-Warning '-CockpitModel needs the world-in-projection composite (do not combine with -NoWorldInProjection); the key was not written' }
  else {
    $modelValue = $CockpitModel.Replace('=', ':')
    if ($modelValue -notmatch '^[A-Za-z0-9_]{1,24}(;[A-Za-z0-9_]+:[A-Za-z0-9_.,-]+)*$' -or $modelValue.Length -ge 100) { throw "-CockpitModel '$CockpitModel' is not '<model id>[;option:value...]' (< 100 characters)" }
    $keys += "xr_cockpit_model=$modelValue"
    $modelFile = Join-Path $env:LOCALAPPDATA ('acz-vr\models\' + $modelValue.Split(';')[0] + '.zvrm')
    if ($modelValue.Split(';')[0] -eq 'auto') {   # AIRCRAFT-AUTO (docs/world-render/aircraft-auto.md): the consumer picks <type><skin>_vrcockpit from the E6 eye offset
      $n = @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\models') -Filter '*_vrcockpit.zvrm' -File -ErrorAction SilentlyContinue).Count
      "-CockpitModel auto: $n *_vrcockpit.zvrm installed (36 expected: install-assets.ps1 -ModelDir models-all); needs -TerrainRender (the E6 locator, e6loc:auto|content)"
      if (-not $TerrainRender) { Write-Warning '-CockpitModel auto needs -TerrainRender (the anchor-free E6 locator runs with the terrain key); the proxy will log cockpit-model-disabled;reason=auto-needs-e6-locator' }
      # AIRCRAFT-AUTO-FAST: the model drawn from the first XR frame = the remembered last aircraft (written by the DLL), else default:<model>
      $lastFile = Join-Path $env:LOCALAPPDATA 'acz-vr\last-aircraft.json'
      if (Test-Path -LiteralPath $lastFile) { "-CockpitModel auto: remembered aircraft file: $((Get-Content -LiteralPath $lastFile -Raw -ErrorAction SilentlyContinue).Trim()) (assumed at XR start; the detector confirms or switches)" }
      else { '-CockpitModel auto: no remembered aircraft yet (last-aircraft.json is written when the detector confirms a type)' }
      if ($modelValue -match ';default:([A-Za-z0-9_]+)') {
        $defStem = $Matches[1]; if ($defStem -notmatch '_vrcockpit$') { $defStem = $defStem + 'k_vrcockpit' }
        $defFile = Join-Path $env:LOCALAPPDATA ('acz-vr\models\' + $defStem + '.zvrm')
        if (Test-Path -LiteralPath $defFile) { "-CockpitModel auto: default model $defStem installed (drawn from the first frame when no remembered aircraft exists)" }
        else { Write-Warning "default model not found: $defFile; with no remembered aircraft the proxy logs cockpit-auto;action=assume-failed and shows the replayed cockpit until detection" }
      } elseif (-not (Test-Path -LiteralPath $lastFile)) { 'note: add ;default:j35jk_vrcockpit (mission 02 = Draken) so the first run also starts with the model cockpit' }
    }
    elseif (-not (Test-Path -LiteralPath $modelFile)) { Write-Warning "baked model not found: $modelFile (run: pwsh -NoProfile -File tools/live/install-assets.ps1); the proxy then keeps the replayed cockpit and logs xr-stereo;event=cockpit-model-disabled" }
    if (-not ($DepthFix -or $OcclusionFix)) { Write-Host 'note: without -DepthFix the replayed world objects nearer than ~1000 units share the cockpit depth band and are dropped with the replayed cockpit' }
  }
}
if ($Lean) { $keys += 'perf_lean=true' }   # FPS investigation: appended last (32 keys with the 31-key morning set; the cap is 32)
Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\runtime.cfg') -Value $keys -Encoding ascii
"Installed. $($keys.Count) config keys written:"; $keys
$cfgBytes = ($keys | ForEach-Object { $_.Length + 2 } | Measure-Object -Sum).Sum
if ($cfgBytes -gt 2048) { Write-Warning "runtime.cfg is $cfgBytes bytes (> 2048): the runtime config is refused; drop optional switches" }
if ($keys.Count -gt 32) { Write-Warning "more than 32 config keys ($($keys.Count)): the runtime config is refused; drop optional switches" }
if ($LookDrive -and $XInputLook) { 'XInput look: the skip-intro proxy (XINPUT1_4.dll) must be installed with look_xinput=1 in skip-intro.ini; Player 1 must be the CONTROLLER in the launcher. Read step5-producer.log: look-drive; and look-drive-status; lines.' }
'Now: launch ACE COMBAT ZERO from Steam (windowed 2560x1440), go to the cockpit, put the headset on. Close the SteamVR dashboard.'
