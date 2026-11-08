# Headset test 16 (terrain m02a): inverted view, unregistered objects/markers, fog/haze (offline diagnosis)

Tags: **[E]** measured in the logs/images here, **[I]** inference. Read-only analysis; nothing was built or run. Evidence: `D:\acz-evidence-archive\headset-test16`
(`xr-stereo.log`, `step5-producer.log`, `ee-camera.log`; no screenshots or eye dumps were archived for this test), `D:\acz-evidence-archive\step56`
(game-window shots `nosedown-all`, eye dump `xr-f417-worldfx-upright.png`). Script: `research/world-render-data/e6_roll_sign_check.py`.

## 1. What the user flew [E]
`ee-camera.log` timeline slot 2 (E6 block at `243af3ca000`; rot = pitch, heading, roll; the `H=` field equals rot[1] except during game look events). Mission samples
(193, one per ~0.26 s, mission clock 8.9..58.8 s; the first rows are menu/load garbage, the aircraft is at (-35266, 4170, 142927) at 10.2 s):

| field | min | max |
|---|---|---|
| pitch | -0.335 rad (-19 deg) | +0.278 rad (+16 deg) |
| heading | -0.923 | -0.016 (NE track, -0.8..-0.4 typical) |
| **roll** | **-2.96 rad (-170 deg)** | **+3.05 rad (+175 deg)** |

Roll episodes (|roll| > 0.35 rad): 11.2-12.0 s (+0.56..+1.00), 14.1-14.9 (-0.56), 16.1-18.5 (-0.68..-0.47), **19.0-27.1 s (full rolls, -2.96..+3.05)**, 34.9-37.2 (+0.69),
38.6-40.9 (-1.04), 52.9-53.6 (-1.05), **54.4-58.8 (+0.71..+1.47 spiral dive: altitude 1442 -> 573 u)**; the aircraft then crashed (terrain frames fall to 0 after ~65 s of the xr log).
So, unlike D-300 (roll ~0), this test exercised large roll almost continuously. 20 of 193 samples (30.7-34 s, `ev=look`) have rot[1] != H (game look events; rot[1] carries the look yaw).

## 2. Control inversion = roll sign (high confidence)
**Evidence [E].** The cockpit camera position is `campos = air + F*fwd + K*up_actual` with F = 36.3 u, K = 3.57 u. Projecting `campos - air` on the axes `camera_frame()`
builds for roll 0 (fwd from pitch/heading, right0, up0) for 173 samples (heading == H):
* forward component 36.21..36.38 for every pitch (-19..+16 deg); with the pitch negated it spreads 28.7..36.5, so **pitch sign +1 is confirmed** (and heading sign +1: the horizontal part is consistent for h in -0.92..-0.02);
* up component = **+3.571 cos(roll)** (e.g. roll 3.05 inverted: camera 13 u BELOW the aircraft; hand check: pitch -0.277, 36*sin = -9.8, up -3.6 -> -13.3 observed);
* right component = **-3.572 sin(roll)**: roll +0.977 -> -3.0, roll -0.55 -> +1.9, 93 samples with |sin roll| > 0.3, all with the opposite sign to what `camera_frame` assumes.

So in the game the aircraft's up axis is `up = up0*cos(r) - right0*sin(r)`: **E6 roll > 0 tilts the up axis to the LEFT (left bank)**. `camera_frame()` (`src/acz-vr/terrain_stage.cpp:276-280`)
uses `U = u0*cr + r0*sr`, `R = r0*cr - u0*sr` with `roll_sign = 1`, i.e. the opposite rotation sense. The terrain therefore rolls the wrong way (horizon tilts the same way as the bank
instead of against it): exactly "controls inverted" in a roll, and a full roll looks like a reverse roll. Pitch (nose-down at D-300; confirmed above) and heading (D-297/298) are right.
**Uncertainty [I]:** assumes the game builds its camera offset from the same up axis that orients its camera (natural; the render matrix itself was not compared at roll != 0). One
direct confirmation is cheap: a headset/null run with the unchanged build but `xr_terrain_stage=m02a;rsgn:-1` (option exists, `terrain_stage.cpp:231`, no rebuild) and a 30 deg bank.

**Fix.** `terrain_stage.h:66` `roll_sign = -1.0f` (or negate in `camera_frame`: `R = r0*cr + u0*sr`, `U = u0*cr - r0*sr`), keep `rsgn:` as the override; update `terrain-render-impl.md`
section 3/4 text and the `terrain event` log line (already prints `roll-sign`).
**Tests.**
1. `tests/acz-vr/terrain_offline_tests.cpp test_camera` (line ~115 only checks orthonormality): add the logged samples as a regression, `(air, campos, pitch, heading, roll)` =
   roll 0.977 / pitch 0.071 / h -0.708: `campos-air = (21.2, 4.6, -29.3)` (rounded from the log row at 11.7 s) and roll -0.548 / pitch -0.090 / h -0.698: `(24.8, -0.2, -26.7)`;
   assert `normalize(campos - air - 36.3*fwd)` equals `cf.up` within 0.02 for the default Conventions, and for 3 more attitude combos with |roll| up to 3.05 rad.
2. Add the same as an explicit sign assertion: roll +0.5, pitch 0, h 0 -> `up.x < 0` (up leans to -X = west = left of a northward heading) and `right.y > 0`.
3. Harness (`tests/acz-vr/xr_composite_warp_harness.cpp`, extend the `terrain orientation` case near line 1652): reuse `orient_stage` (red block left, green block right at the same range and height).
   For (pitch, roll, heading) in {(0,+0.5,-0.785), (0,-0.5,0), (+0.2,+1.0,1.2), (-0.3,3.0,-0.5)} and head yaw 0/25 deg: the expected image is derived BY HAND (no `camera_frame`):
   rotate the level-flight picture about the view axis by -roll (positive E6 roll = left bank = the world turns clockwise on screen, left horizon goes UP): after roll +0.5 the red (left) patch
   centroid row must be ABOVE the green (right) one; for -0.5 below; for 3.0 (inverted) the red patch is in the right half. Sides/rows are checked from the real shader on WARP (503 existing checks pass the same way).
4. Optional robust cross-check at runtime (not needed if 1-3 pass): derive up from `campos - air` once per second and log the disagreement with `camera_frame` (would have caught this).

## 3. Why objects and markers do not stay registered [mostly E/I]
**Where each layer gets its camera.**
* Replayed world objects (DrawACM_v): the producer recovers the **game's own** camera from the draw's matrix A (`gpu_copy_step5_world.h camera_setup`, `J-world-objects-findings.md`): orientation, lens and
  position come from the game's per-draw matrices and therefore embed its roll exactly; the eye is `camera_eye + units_per_metre * head offset in the camera's right/up/forward axes`. No E6 involved.
* HUD marker layer (FRND boxes): the game's 2D HUD pass, i.e. screen-space positions computed by the game's camera, placed on a quad "EXACTLY the world quad" (500 m, 63 x 38 deg lens) (`xr_stereo_output.cpp layer_from`, markers branch ~line 913). Game camera again.
* Terrain: E6 `campos` + Euler (pitch, heading, roll) through `camera_frame()` (feed at `xr_stereo_output.cpp:1183-1199`), a **different, hand-derived camera**.
Objects and markers are registered with the *game's* world image by construction; the terrain is registered with it only if `camera_frame()` equals the game camera. At roll 0 it does (D-300 static pose). With the
roll sign wrong the two disagree by **2 x roll** (barrel roll at 1 rad: 115 deg apart): the terrain horizon tilts one way, objects and markers (and the cockpit-relative HUD frame) the other, which reads as "they
rotate with the world / do not belong to it". **[I]** The same root cause explains it; it was not separately observed (no eye dumps archived).
Secondary contributors, each smaller than the sign error:
1. **Latency skew [I].** The E6 mailbox is sampled once per game Present (`ee_camera_probe_win.cpp terrain_tick`) and the newest sample is used when the consumer composites a pipelined producer frame, while the objects/markers
   in that frame carry the camera of the frame they were recorded in (typically 1-3 game frames older, ~20-50 ms). At the roll rates seen (full rolls in <3 s, ~2 rad/s) this is 2-6 deg of relative rotation during manoeuvres. Fix idea: stamp the producer frame
   with the E6 sample sequence read at record time (or keep a 4-entry history in `terrain_camera.h` keyed by the producer frame id) and have the consumer use the sample belonging to the frame it composites.
2. **Head-offset scale mismatch [E].** Objects use `units-per-metre=65.0` (step5-producer.log, world-objects first-published) while the terrain logged `head-units-per-metre=39.6`. Head translation therefore shifts the eye 65 u/m for objects and 39.6 u/m for the terrain: no effect for rotation, a small
   parallax error for near objects (0.2 m lean = 13 vs 8 u). Fix: one value; set `upm:65` in `xr_terrain_stage` (or read the producer key in `xr_stereo_proxy.cpp` where `head_units_per_metre` is parsed).
3. **Objects partly missing/popping [E].** `continuous-world`: seen=342217, `before-frame`=177833 (52% of DrawACM_v draws precede the replayed frame and are NOT replayed), refused target 8096 (offscreen), cap 545, camera failed 173/3825 (`world-pose-unavailable`).
   In terrain mode the game's world window (which contains exactly those non-replayed objects/buildings) is dropped, so those buildings and trees vanish and the replayed ones flicker; the 105 baked `.zvrs` objects are not drawn. This is a coverage gap, not a registration error.
4. **Look events [E].** During game look (30.7-34 s, 11 `look` samples) rot[1] != aircraft heading; the terrain follows rot[1] (as the game camera does), so it stays registered, but any consumer assuming heading == rot[1] is wrong in that window.
5. Fog is applied to the terrain only; objects are unfogged (see 4), which also makes far objects look pasted on.

## 4. Fog / haze model fitted to the game [E measured, curve fit I]
**Why it looked wrong [E].** (a) `kTSkyHorizon = (0.66,0.74,0.82)`, `kTSkyZenith = (0.28,0.46,0.80)` and the ground texels are written as raw numbers but the pipeline encodes them **again** with the sRGB RTV: the eye dump `xr-f417-worldfx-upright.png` top row is
(167,194,232) = sRGB_encode of the computed (0.379,0.533,0.805) exactly. So the horizon/haze shows as (168,189,209)-ish bright blue-white; the game's horizon haze is **(130,148,141)** (grey-green) and its sky is much darker. (b) Ground texels get the same double encoding and no lighting darkening:
matched view (step56, 90 deg eye vs game window centre, pitch -29 deg): game centre (87,103,94) vs eye (109,116,103); game lower ground (69,83,77) vs eye (150,157,133). (c) Our fog is smoothstep 20..58 km with the march ending at 58 km (the `t >= t_fog1` break) and a sky-gradient fallback: terrain between 58 km and the true horizon
(the band from the horizon to ~3.4 deg below it at 3500 u above ground) is replaced by a flat bright band, and everything inside 20 km is perfectly clear, whereas the game has haze from the first kilometres and still shows terrain at >100 km. This band is the likely "strange fog effect"; the missing grey haze on the ground is "haze absent".

**Measurement.** Game shot `acz-20261026-231030-441-pre-V.png` (step56, level flight at mission start, y = 4000, terrain ~480 u below, E6 pitch +0.04 rad; 2421x1362 window, vertical FOV 38.1 deg, f = 1971 px; the sky brightest row sits 2.2 deg above the image centre = the horizon).
Median over unobstructed columns (display sRGB 0-255). Distance d = 3500 / tan(depression below the horizon), flat-ground estimate (+-15% from terrain relief):

| depression | d (u) | G of the ground | haze fraction f (ground G0 ~54, haze G 148) |
|---|---|---|---|
| 0 (horizon) | inf | 148 | 1.00 |
| 1.5 deg | 133 000 | 138 | 0.89 |
| 2.2 | 91 000 | 129 | 0.80 |
| 2.9 | 68 000 | 122 | 0.72 |
| 4.4 | 45 000 | 111 | 0.61 |
| 5.8 | 34 000 | 104 | 0.53 |
| 7.3-8.0 | 25-27 000 | 83-97 | 0.31-0.46 (texture noise +-0.15) |
| 15-20 | 9-13 000 | 56-72 (field/forest) | ~0.1-0.2 (G0 is itself fitted here) |

Fit: **f(d) = 1 - exp(-d / L), L = 55 000 u** (per-row implied L 45-67 k, within +-0.05 of f for d >= 25 k; ground colour noise dominates below). Half-haze at 38 000 u; 90% at 127 000 u. **Colour** of the haze = the sky/horizon colour
**(130,148,141) display = (0.51,0.58,0.55)**. Sky elevation profile (display RGB, elevation above the true horizon; same shot, columns left/right of the HUD): 0: (130,148,141); 5.6: (101,133,136); 7.9: (90,127,134); 10.2: (82,120,130); 12.4: (77,115,125); 14.5: (70,108,119); 16.6: (64,103,115);
a slope of about -3.0/-2.0/-1.2 per degree (R/G/B) and decaying; nothing was measured above 17 deg (extrapolate and clamp near (45,90,100) at 30 deg [I]).

**Proposed model (implement in `terrain_stage.h` `k*`, `terrain_stage.cpp trace`, `xr_stereo_blit.hlsl terrain_trace`; keep the WARP comparison):**
1. Decode before shading: `tex_lin = pow(texel, 2.2)` (or the exact sRGB EOTF) for terrain texels, sky, haze and river colours, so the final sRGB encode gives the display values above. Expect the ground to drop to roughly 0.6x of now (closer to the game); then fit a single global ground gain `kTAmbient/kTDiffuse` against a registered pair (game lower ground 69..83 G at ~3.5-6 km range).
2. Fog: `f = 1 - exp(-t / 55000)` (no start distance), `col = lerp(ground, kHaze, f)` with `kHaze = decode(0.51,0.58,0.55)`; raise the march limit to the end of useful terrain (`fog1 ~ 150000-180000`; at 3500 u AGL the ray reaches 1.3 deg below the horizon there; cost is the open question: adaptive step grows 0.4*dy, the 320-step cap may bind, measure the WARP/GPU time) and
   for a ray that exhausts the march or leaves the stage return `kHaze` blended into the sky (not a second colour): the haze colour is the sky colour at elevation 0 so there is no seam.
3. Sky: `sky(e) = lerp(kHaze, kZenith, 1 - exp(-e / 14 deg))` with `kZenith = decode(0.10,0.28,0.36)` (fit to the table above; refine after one upward-looking game shot at +35 deg).
4. Keep `fog0/fog1` as options but redefine as `fogl:<L>` (default 55000) and `far:<u>`; the old `fog0` start is not supported by the data.
5. Objects: apply the same `f(d)` in the object path (distance from the camera, e.g. from the DrawACM_v A-matrix w) or at least a haze tint, otherwise far buildings stay sharper than the terrain.
Tests: CPU `trace` vs shader at three depression angles (f within 0.01 of the formula), sky(e) table points within 3/255, no seam between a missed ray and the last terrain sample (colour difference < 2/255), key-off images still byte-identical.
**Uncertainty:** one level shot, flat-ground distances, ground colour varies +-15 G, ground gain is not calibrated, the game's own haze may include altitude-dependent terms (camera at 3500 u above ground; low-level flight may show a shorter L); verify with a second shot at low altitude.

## 5. Recommended next headset run (cheap)
`-TerrainRender 'm02a;rsgn:-1;upm:65'`, bank +-30 deg left/right for 3 s each (never a dive), archive eye dumps and game shots 1 s apart during the banks, so roll sign, object/marker registration and the latency skew can be separated;
then land the code fix (section 2), fog model (section 4) and re-test. Not verified: anything about the headset view itself (no eye dumps for test 16), the roll-sign conclusion's one remaining assumption, the fog numbers beyond one frame.
