# PROPS-SHADING: "the objects on the ground don't have shaders applied" (headset test 17, D-330)

Offline only (null-driver recipe for a later live run in section 6): no game, SteamVR or headset run here; archives and logs read-only; game-derived
outputs only in `D:\acz-extract` (`out\props-shading`, `stage-ps`, `stage-ps-all`, `gpu-bench\props-shading`). Tags: **[E]** evidence (measured,
decoded, tested here), **[I]** inference.

Tools: `research/world-render-data/props_shading_probe.py` (`mispack` contact sheet, `records` = decode of the logged DrawACM SrtData records,
`poses` = close-up camera recipes), `tests/acz-vr/xr_gpu_bench.cpp` (`--pose/--shade/--no-cockpit`: the REAL composite + props shaders on the real
GPU with the real m02a stage, props and trees), tests listed in section 5.

## 1. Which pipeline draws the windmill, the tanks and the bridge

| object | drawn by | evidence |
|---|---|---|
| **bridge** (4 spans, `.pdl` #45-48, pmmdl 18-21, centre ~(14500, 240, 14500), 481 u tall piers) | **our props pass** (`xr_props.hlsl` + `xr_props_composite.hlsli`) | [E] the spans are `.pdl` placements of `pmmdlm02a.bin` (world3-impl 1.1); with `propdedupe:1` (the installed string) the game's replayed copy of every `.pdl` record is refused (`world-dedupe-prop`, test-17 log: 108 refusals, `props-dedupe;placements=100`) and ours is the only one drawn |
| houses, ruins, radio towers, water tower | our props pass | [E] same (pmmdl 0-17, 22-42) |
| **tanks** (and APCs, SAM launchers, trucks, the mission's aircraft) | **the game's own replayed DrawACM_v draws** (game VS/PS, inline per eye) | [E] they are NOT in `pmmdlm02a.bin` (44 models, contact sheet `D:\acz-extract\out\props_m02a_sheet.png`: houses/ruins/towers/water tower/bridges only); they ARE in the mission pack `data/mis/pack/m02a/m02ald.bin`: packages 1-6, 8, 9, 12 = tank, turreted APC, burnt hulls, SAM launchers; 21-22 = trucks (`props_shading_probe.py mispack`, `D:\acz-extract\out\props-shading\mispack_m02a_sheet.png`) |
| **windmill** | **the game's replay** (mission pack package 17: 346 x 362 x 125 u, tower + four lattice sails) | [E] package 17 of `m02ald.bin` renders as a windmill; no pmmdl model is one. Its world position comes from the mission script `m02a.mmb` (not decoded: no fixed-camera recipe, section 6.3) |

So two of the three named objects are the game's own draws, one is ours, and the causes differ (sections 2 and 3).

## 2. The replayed objects (tanks, windmill): the game's shading is intact, the colour is encoded twice

* [E code] The replay records the game's own DrawACM draw inline (same PSO = the game's `DrawACM_v`/`DrawACM_p`, same root signature, descriptor
  tables, textures, cb10 root constants; only the t10 SrtData record is substituted, and only A (+0..+63) is rewritten): lighting, fog, texture
  and the PS hash gate are the game's (`J-world-objects-v2-design.md` 4, `gpu_copy_step5_world.h`). Nothing in the replay strips shading.
* [E logs] The records carry the game's lighting: the logged aircraft records have the VS lighting branch ON (+352.x = 1) with the light matrix
  column 0 = (0.492, 0.766, 0.413) = the m02a sun 50 deg up (= WORLD-2's mapset light), sun colour (0.325, 0.325, 0.267), a down light
  (0, -1, 0) with colour (0.075, 0.082, 0.082), ambient (0.431, 0.475, 0.412) (`props_shading_probe.py records`, test 17 / step 77 / step 79 logs).
  No ground-unit record was among the logged samples (3 per window), so their exact lighting values are not shown [I: same branch as the aircraft].
* [E D-311 + code] The producer's FP16 eye target holds the game's DISPLAY values; `ps_main` writes them as linear into the sRGB swapchain RTV,
  so they are sRGB-encoded twice. D-311 proved this for the replayed cockpit on 7 landmarks (replay = exactly `srgb_encode(game window)`); replayed
  objects live in the same target and take the same `ps_main` path (object band 0 < depth < 2^-10 of the depth fix). Effect: a dark tone of 20/255
  shows as 75/255, mid tones lift less: the shaded sides of a tank or of the windmill tower come out nearly as bright as the lit sides = "flat,
  unlit". Our terrain/props/model cockpit are decoded once (WORLD-1/3, COCKPIT-3), so the replayed objects now look washed out NEXT to them; in
  headset tests before WORLD-1 everything was double-encoded alike, which is why this was not noticed earlier [I].
* **Fix (option, default off):** `objdom:display` inside `xr_terrain_stage` (t_bits bit 11): object-band pixels of the eye source are decoded
  once (`t_srgb_to_linear`) before the composite. Needs the terrain, the producer's depth fix (`xr_world_objects_depth_fix`, -DepthFix) and a
  float depth format (the same conditions as WORLD-1's object occlusion, without `occl:`). Everything else is byte-identical (WARP test).
  Default off because it changes the replayed objects of every terrain configuration; the driver A/B (section 6.2) should decide the default.
  Not changed: the replayed cockpit band (hidden under the model cockpit; without the model it is still double-encoded like the world window),
  the world snapshot window (`world_image`, consistent with itself), the effects layer (EFFECTS-OUTSIDE track).

## 3. Our props (bridge, houses, towers): the game does NOT light them; we did, then darkened and over-hazed them

### 3.1 What the game does [E]
* All 36 logged replayed building records (`.pdl` houses at y 491-513, test 17 + steps 77/79) have **+352.x = 0: the DrawACM_v lighting branch is
  OFF** (cockpit VS DXIL `tools/evidence/e11-drawcpt-dxil/drawcpt-vs.dxil.txt` l.184: the same SrtData layout; without the branch COLOR = vertex
  colour / 128). The props' vertex colours ARE their lighting (baked: house roof ~0x5a, walls ~0x48, bridge underside 0x15-0x46 vs deck up to 0x75;
  `acm_lib.decode_acm` of pmmdl 0, 3, 5, 8, 18-21).
* `DrawACM_p` (acm-shader-dataflow.md 2) = texture x COLOR, then fog: `TEXCOORD.z = saturate((w * k1 + k0) / 255)` with k0 = +336 = 255 and
  k1 = +340 = -0.00553385 (0xBBB55555) in every building and aircraft record: a LINEAR fog over the view depth reaching full fog at
  **255 / 0.00553385 = 46080 units**, toward the record fog colour (0.329, 0.408, 0.396) = (84, 104, 101) = WORLD-4's fitted fog colour F
  (85, 105, 101) [E]. (The doc writes `lerp(colour, fog, z)`; with z = 1 at w = 0 that would fog the nearest objects fully, so z is the
  PS2-style "colour weight" [I].)
* Colour order: the pmtex CLUT has always been read RGBA (COCKPIT-1 `pmtex_textures(.., bgra=False)`): the five m02a prop textures are
  byte-identical in every bake [E]. The BGRA bug of COCKPIT-3 hit only the TREE textures (own GIM CLUT): the installed `stage-all` bakes (00:59,
  before the fix) carry R/B-swapped trees (teal instead of olive: layers 5-7 differ by up to 32 levels and equal the new texels with R/B swapped).
  Rebaked: `D:\acz-extract\stage-ps-all` (all 39 stages; .zvrs byte-identical, .zvro differ only in tree texels).

### 3.2 What WORLD-3/4 did and why it looked unshaded [E numbers, I perception]
`albedo (texture x vertex colour) x clamp((ambient + sun * max(n.L, 0)) / (ambient + sun * L.y), 0.4, 1.4) x look gain (0.64, 0.61, 0.50)`, then
the terrain's exponential haze `1 - exp(-t / 21500)`. Three departures from the game:
1. a second lighting on top of the baked one (sun-facing walls brightened, others darkened by up to 0.4x);
2. the WORLD-4 GROUND gain (fitted on terrain texels, whose game vertex colour is ~0x50) applied to props whose vertex colours are already in the
   albedo: ~0.6x darker and bluish-grey (blue x 0.50 vs red x 0.64);
3. exponential haze 21500 instead of the game's linear 46080: at 6.4 km 26% instead of 14%, at 20 km 60% instead of 43%.
Net effect on the step56 centre house roof (texture (98, 76, 60) x vertex 0.70): game model (tex x vcol + 14% fog) (71, 61, 51); the game window
measures (66, 54, 41) (glass removed; blurred into the dark ground around it); WORLD-3/4 gives about (55, 52, 42): the red-brown roof turns grey-brown
(R-G 3 instead of the game's ~12) and every face is dimmed toward the haze: flat, low-contrast. [E: arithmetic on the decoded data; the game
crop is ~20 px wide and blurred, so it only supports the direction, not a fit.]

### 3.3 Fix: `propshade:game` (default) [E code + tests]
`xr_props_composite.hlsli` with t_bits bit 12 (set when props are drawn and the option is game): `disp = lerp(saturate(albedo), haze, saturate(tp /
46080))`, WORLD-2 clouds in front, one `t_srgb_to_linear`. No normal, no stage light, no look gain. Haze colour = the terrain's (`te_haze(d)` with
sky on: F looking down, which the record fog colour equals [E]; `kTHaze` without the environment). The fog uses the RAY distance tp instead of the
game's view depth w: identical on the view axis, and it does not change when the head turns (the game's w-fog would make props pulse with head
rotation in a headset) [I: design choice]. `propshade:w3` keeps WORLD-3/4 exactly (bit 12 clear = the old shader path). Props off / no props in
view: bit 12 is never set, byte-identical (existing WARP tests).
Not changed: alpha test 0.5 (tower lattice / ruins / trees cut out correctly; box-filtered mips thin the lattice at distance [I]), two-sided
drawing (cull none, normal faces the eye: only used by w3 now), texture resolution (all resampled to 128^2, the game's 64^2 ones are magnified
nearest), no blend flags on m02a props (all node flags 0 or 8).

## 4. Close-up debug camera: `cam:x,y,z,yaw[,pitch[,roll]]`
The fixed test camera now takes optional pitch and roll (degrees; |pitch| <= 89, |roll| <= 180; 4 values = the old level camera exactly). They
go through `camera_frame` like a live E6 sample (forward = (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch), roll with the -1 roll sign).
A line `xr-stereo;event=terrain-debug;fixed-cam=1;cam=..;yaw-deg=..;pitch-deg=..;roll-deg=..;objdom=..` is logged when cam: or objdom: is given
(no line with the defaults). The fixed camera drives the terrain AND our props (they share every ray), not the game's replayed objects (those
keep the game camera: at a fixed camera they float at wrong places; use `-DebugView 3` = world layer only, which drops them and the cockpit).

## 5. Tests (golden images: CPU reference vs the real shaders on WARP)
* WARP harness `case_props`: default (game) shading of the real props pass + composite vs `acz::props::prop_color_game` (analytic boxes on the
  synthetic slope); `t_props_shade_w3` = the w3 path vs the unchanged `prop_color`; both differ only on prop pixels; buried / out of range /
  key-off / async cases unchanged (byte-identical to terrain only).
* WARP harness `case_terrain_occlusion` + objdom: an object-band rectangle of display value (0.20, 0.50, 0.05): with objdom the stored sRGB byte
  equals the display value (decoded once), without it equals srgb_encode(value) (the old double encode), every other pixel byte-identical.
* `props_offline_tests`: `prop_color_game` (no light, linear fog, saturation), `prop_fog`, 0xBBB55555 -> kPropFogEnd, the propshade option.
* `terrain_offline_tests`: cam with 4/5/6 values, range checks, objdom parse; a pitched camera_frame looks down.
* `xr_stereo_output_offline_tests`: terrain_obj_display only with objdom + depth fix + float depth; the terrain-debug line; the fixed cam's
  pitch/roll reach the eye cameras exactly like an E6 sample.
* Real-GPU renders (not a test): `xr_gpu_bench --pose ... --shade both --no-cockpit --png` (section 6.1 table), `D:\acz-extract\gpu-bench\props-shading`.

**Results** (Release, Ninja, `D:\b-ps`, -j2, built first time): ctest **305/305**, again 305/305 after merging master 87b4c00 (COCKPIT-FIRST; this build dir registers 305 tests, not the 307 of the brief: probably the d3d12 proxy option, which is off here [I]); WARP harness 867 checks 0 failed (props game: max error
0.0044 / 0.0131 per eye, 0 of 57,600 pixels over 0.06; w3 the same; the two modes differ on exactly the 2,625 prop pixels; objdom: display max error
1/255, raw 0/255 against srgb_encode, 0 bytes differ outside the object); props_offline 1,158,764 checks; terrain_offline 859; xr_stereo_output
269,625. Real GPU (RTX 4090, 1600x900, 70 deg, env+look+props+trees, no cockpit) at the six poses of section 6.1, `D:\acz-extract\gpu-bench\props-shading`
(per pose `<pose>_q0_env+look+props+trees+{game,w3}.png`), sheet w3 | game: `D:\acz-extract\out\props-shading\closeups_w3_vs_game.png`. w3: dark,
muddy, grey-brown roofs, the bridge deck and piers nearly the colour of the hazy ground; game: the textures read clearly (roof tiles, window rows,
pier stonework), the baked vertex shading separates the faces (house gable vs long wall, deck vs pier shadow side). Props cost unchanged
(0.019-0.037 ms props pass, composite 0.17-0.20 ms per eye in both modes).

## 6. Driver recipe (null driver; no flying low: the camera is fixed, the aircraft keeps flying the mission normally)

### 6.1 Close-ups of our props (bridge, house, towers)
1. Assets: `pwsh -NoProfile -File tools/live/install-assets.ps1 -StageDir stage-ps-all` (the rebaked stages: corrected tree colours; props
   unchanged) on the new DLLs (they accept the new options; older DLLs refuse `propshade:`/`objdom:`/a 5-value cam = the whole terrain is refused).
2. One launch per pose, png-once, world layer only:
   `vr-test-start.ps1 -PngOnce -DebugView 3 -DepthFix -TerrainRender 'm02a;env:1;look:1;props:1;trees:1;propdedupe:1;cam:<pose>'`
   (no -CockpitModel auto: it needs the E6 locator, which the script does not start with cam:). Then the same with `;propshade:w3` for the A/B.

| pose | `cam:` value | what is in view |
|---|---|---|
| house83 | `12540,704,132450,54.5,-17.2` | the lone red-roofed house (.pdl #83, the propdedupe example) from 540 u, 224 u above the ground |
| house83-low | `12370,584,131970,125.8,-7.4` | the same house from the other side, 311 u, near eye level |
| bridge46 | `15960,831,12838,118.3,-13.3` | bridge span #46 (pmmdl 19) from 1.5 km, piers and deck |
| bridge-row | `17100,980,15100,77.0,-10.6` | all four spans from 2.7 km |
| radiotower55 | `12502,864,55367,47.9,-6.0` | lattice radio tower (pmmdl 5, alpha-tested) from 570 u |
| watertower58 | `10280,751,55151,49.1,-8.6` | water tower (pmmdl 8) from 400 u |

(`python -I research/world-render-data/props_shading_probe.py poses` prints these with the terrain clearance of every sight line: 65-495 u.)
3. Check: `event=props;...;shade=game(...)`, `event=terrain-debug;fixed-cam=1;...`, no `props-disabled`; compare the png with the bench render of
   the same pose (section 5 results): they must match (same shaders, same data; the null-driver eye FOV differs from the bench's 100 deg).

### 6.2 Replayed objects (tanks, windmill): objdom A/B in normal flight
Two launches on the normal recipe (`auto;env:1;look:1;props:1;trees:1;propdedupe:1` + `-DepthFix -CockpitModel auto`, png-once ~8 s after the
mission starts), the second with `;objdom:display` added. Expect: `event=terrain-debug;fixed-cam=0;...;objdom=display` in the second; the
replayed ground units / aircraft darker with more contrast; compare object pixels with the game-window screenshot of the same moment
(they should equal the game values, the raw run should equal srgb_encode of them). If it holds, make objdom:display the default.

### 6.3 Not possible with the fixed camera
The windmill and the tanks are mission-script objects drawn with the game camera; their positions are in `m02a.mmb` (not decoded) and the fixed
camera does not move them. Seeing them close requires flying near them (unsafe at low altitude) or decoding the script and baking the
mission-pack models (WORLD-4 2.9: ~1 day, would double-draw them unless the replayed copies are suppressed).

### 6.4 Headset test 18 (D-355): "3D objects in the world still don't have shaders"
* Same symptom as test 17 section 2, on the step86 day install (terrain `auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1`, no
  `objdom:`). Our props are already `shade=game(texture*vertex-colour,linear-fog)` (logged in run E10 and every step83 run) and the user calls trees,
  forest and colours fine, so the remaining "unshaded" objects are the game's REPLAYED draws (wingmen, enemy aircraft, ground units, windmill):
  double sRGB-encoded = shaded sides lifted toward the lit sides [I: no step83 dump has a replayed object in view (A f645, X, Z1 f424, Z2a f393,
  E10 f420), so there is no pixel measurement of the symptom; the cause rests on D-311 and section 2].
* `objdom:display` is live-proven to ACTIVATE on the day path (run Z2a, step86 DLLs, day config incl. decouple/interp: `terrain-debug;fixed-cam=0;
  ...;objdom=display` without the `(inactive:..)` suffix; its gate is the same as WORLD-1's object occlusion, which counted `occlude-frames` ~580
  per 5 s in the same run = float depth + depth fix present every frame), with no new errors over 60 s of flight. It adds one sRGB decode per
  object-band pixel (no new key: it goes inside the existing `xr_terrain_stage` string). Recommended day string:
  `auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display`.
* Still unverified: the visual A/B of 6.2 (object pixels display = game window, raw = srgb_encode(game window)) and the headset judgement.

## 7. Open
* objdom default (needs 6.2); ground-unit record lighting values (not among the logged samples); whether other stages share k1 = -0.00553385
  (the record carries it: a per-stage table could be read from live records).
* The game's terrain may use the same linear object fog (WORLD-4 rejected a 16896..40960 linear fit, not 0..46080) [open].
* Tree shading also switches to `game` (tree records not logged; tree vertex colours 0x46-0x61 grey = baked shading like the props [I]).
