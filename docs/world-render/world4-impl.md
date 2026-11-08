# WORLD-4: world fidelity toward the game's own look (overnight track, D-302)

Offline only: no game, SteamVR or headset run; archives read read-only; game-derived outputs only in `D:\acz-extract` (`out\world4`, `stage-w4`,
`mis-pack`). Tags: **[E]** measured / tested here, **[I]** inference. Build `D:\b-w4` (Release, Ninja, -j2 twice, after merging master
`16fad7d`): ctest **299/299**, terrain_offline 850 checks, props_offline 475, WARP harness 724 (0 failed).

## 1. Evidence base and tools (`research/world-render-data/`)
| file | what |
|---|---|
| `world4_trace.py` | numpy port of the runtime ray march (`trace()`: same height_bilinear, step law, bisection, tile/mip lookup): per pixel distance t + RAW texel |
| `world4_compare.py` | game client crop (rows 31..1382 of the 2421x1392 screenshots, width from the logged lens aspect), HUD/cockpit mask, local-contrast pose registration, band fits |
| `world4_fit.py` | the look:1 parameter fit (sky rows, canopy glass, ground gain / haze / fog colour) |
| `world4_check.py` | C++ CPU reference renders (terrain_offline_tests, `ACZ_W4_RENDER_DIR`) vs the game window: the acceptance table in section 3 |
| `world4_trees.py` | dark-blob tree counter per distance band (crude; see 2.6) |
| `mispack_probe.py` | mission pack extraction + layout probe (section 2.9) |

Shots (mission 02, stage m02a, heading NE, game lens tan 0.6152 x 0.3460 = the logged `lens-fov-deg=63.20x38.17`):
* **step62** `shots/acz-20261027-004711-116-T6.75.png` (cockpit view, level) with our eye dump of the same frame (`xr-f403`, png 20 ms earlier) and the
  E6 pose (-14340, 4003.575, 122000) from `ee-camera.log` at the png tick. Registration [E]: the local-contrast correlation of game vs trace peaks
  at the logged pose (0.54-0.70 depending on the crop; +-500 units along track and +-1000 across are all lower).
* **step56 pre-V** (HUD-only view, no cockpit model, no canopy glass, level, 7.4 s earlier on the same track): the SKY reference.
* **step56 T7.03** (cockpit view, pitch -29.3 deg): near ground 4-14 km (pose from E6; its registration is weaker, 0.2-0.3).

## 2. Findings
### 2.1 Moon: placement is right; brightness was 9x too strong [E]
In the registered step62 pair the crescent is at **8.29 deg right / 10.45 deg up** of the game's view axis and at **8.25 / 10.36 deg** in our eye
dump (both measured from the blob centroid, ours with the eye's 90x90 deg tangent grid). Bearing 53 deg / elevation 10 deg from mapset +0xBC/+0xC0 is
therefore exact; "ours right of centre, the game's nearer the upper right" is only the lens: 10 deg up is half-way to the top of the game's 38 deg
window but only ~1/5 of the way in our 90 deg eye. WORLD-2's "~2 deg high" note (unregistered pre-V estimate) is superseded. Brightness: the
game's crescent is a faint excess of **~10/255** over the sky (pre-V 10, step62 9), ours **93/255** -> `kLookMoon = 0.11` (+-0.03).

### 2.2 Canopy glass in the game's cockpit view [E]
The same sky elevations in the cockpit view (step62) vs the HUD view (pre-V) differ by a constant factor **(0.951, 0.940, 0.935)** (29 rows
2..12 deg, interquartile +-0.01): the game's canopy glass tints the outside slightly green-grey. Our model cockpit has no glass, so every
world fit below divides cockpit-view pixels by this factor (the world WITHOUT glass is the target). For COCKPIT: a glass layer of that
transmission would complete the match [I].

### 2.3 Ground: ours was BRIGHTER than the game, not darker [E]
Registered step62, mean over usable ground pixels (game / glass): 11-16 km game (61,70,58) vs ours (83,91,73); 30-42 km (77,93,86) vs
(105,117,101). The game's lit ground is **~0.6 x the raw CLUT texel** (fit gain **(0.64, 0.61, 0.50) +-0.06**; plausible PS2 vertex colour
~0x50 [I]). The earlier "ground slightly darker" (D-304: G 57 vs 69-83) compared different poses and patches.

### 2.4 Haze: exponential, toward a SKY GRADIENT that continues below the horizon [E]
* The game's ground fades toward a colour that depends on the view elevation: looking down it is **F = (85,105,101) +-4** (data candidate: sph row 2
  horizon (85,102,98)); toward the horizon it brightens into the sky's horizon glow; there is no hard horizon line (rows -1.2..+2.5 deg:
  (100,120,116) -> (125,147,140)). WORLD-1/2 hazed toward the horizon colour (135,156,148) for every ray, which made the far ground too bright.
* Law: `1 - exp(-t / L)` with **L = 21500** (joint fit; the minimum is flat over ~20000..35000 because gain and L trade off without near
  ground in the cockpit window). A linear PS2 fog between the sph block-0 floats (16896 .. 40960) was tested and REJECTED (rms 7.8 vs 3.3).
* Low sky model (also the haze target, at the ray's SIGNED elevation): `S(el) = lerp(F, k*C1, smoothstep(e_lo, e1, el))` below e1, then
  C1 -> C2 -> C3 (sph rows) with stops **k 0.90 +-0.02, e_lo -3.5, e1 2.5, e2 8.5, e3 20 deg** (fit rms 2.3/255 on the pre-V rows).

### 2.5 Clouds and water [E/I]
The game's +-19 deg window shows no distinct cloud sheet, only a faint stratus haze near the horizon; with look:1 our layer-0 sheet (16000 u) is
hazed out at low elevations (tc large) -> consistent [E: step62 sheet]; the blotchy clouds overhead (worldfx dump, 30-50 deg) cannot be compared
(no game shot looks up) [I]. Water keeps the stage water colour (70,89,84) = the measured far river (WORLD-2); the gain is not applied to it [I].

### 2.6 Trees: forests are a separate mask, and the game fills them [E/I]
* **`.wbd` value 1 (512-unit cells) = FOREST** [E]: 96% of their texels are dark forest (mean CLUT luminance 29.8 vs 73.2 elsewhere; the
  transposed / high-bits-first readings give 49-36), 327,587 cells (31% of m02a), and they contain **none** of the 174,302 listed trees.
* The step56 window shows ~30 trees in an 800x400 crop (about 28 cells of ground at 6.4 km), many inside a forest cell; our listed trees give
  12 there, all on the forest EDGES [E visual]. The listed (.wpd) trees are hedgerow/edge trees; the game adds trees inside forest cells by a
  rule we did not decode [I]. `objects_baker.py --forest N` fills each forest cell with N tree-1 instances at hashed positions; **N = 3**
  reproduces the crop (~30 visible, `D:\acz-extract\out\world4\s56_forest3.png`) [E visual, density +-2x]. m02a: 1,157,063 instances (28 MB),
  per-frame selection 0.13 ms on the CPU, ~900 drawn at the step56/62 poses (cap 16384 not reached) [E].
* Tree draw distance **16000** (was 12000): the game's blobs reach the 12-16 km band; mapset +0x14 holds 16000 (draw distance candidate) [I].
* Tree scale 4.5 kept: in the centre crop the game's trees look ~0.75x ours, but their dark trunks/feet are hard to see on dark ground (two-tree
  fit in WORLD-3 gave 4.2-5.1) [open]. The game's far trees could be billboards; nothing visible beyond ~16 km either way.
* The blob counter (`world4_trees.py`) is too crude for density (our nearest-neighbour texture makes false blobs; trees on dark forest have
  little contrast); the visual crop is the evidence used.

### 2.7 Buildings: the replay DOES draw the .pdl buildings -> double draw with props [E]
A logged replayed DrawACM_v record's model translation (12120, 512, 132150) is m02a .pdl #83 (12120.09, 512.18, 132150.48), 0.49 units. With
props:1 the same building is drawn by our props pass AND by the game's replay. Fix (option, default off): `propdedupe:1`. The red-roofed
village at the left of the step56 view is orthophoto texture (WORLD-3); the extra houses of the right-hand cluster are not in the .pdl and not
among the mission-pack models (2.9) [I: possibly more pmmdl detail not decoded].

### 2.8 Objects and markers use the shared game camera (code reading) [E code / I live]
* Terrain: `csrc:game` builds the frame from this frame's game camera axes (decomposed from the first cockpit member) and `game_eye` (the
  DrawACM_v camera's absolute eye) (WORLD-1); props: the props pass projection is the inverse of the composite's own pixel ray (`make_clip`),
  so props and terrain share every ray by construction (WORLD-3 test 1.8e-7 uv).
* Replayed objects: `world::substitute` rewrites only A (+0..+63) with `D_e x S` from the SAME per-eye pose set-up as the cockpit
  (`setup_poses`, the producer's units per metre); the terrain eye uses `eye_world(cf, origin, upm)` with the producer's units per metre when
  `upm:` is not given (WORLD-1): same camera, same head offset.
* Markers: the game's own 2D pass; with `xr_hud_markers_layer` + world_in_projection they sit on a quad built by the same `world_quad(...)` as
  the world (game lens, neutral-yaw anchor), i.e. on the game camera's rays: registered with terrain/objects up to head-translation parallax [I].
* Double drawing: cockpit replay dropped with the model cockpit (COCKPIT-2); whether the game's trees are DrawACM_v records (then also replayed) was NOT checked (WORLD-1 lists them among the window-only scenery) [open];
  buildings: `propdedupe:1` (above).

### 2.9 Mission pack (`data/mis/pack/m02a`) feasibility [E layout / I meaning]
`m02ald.bin` (762 KB) = **28 unit model packages**: a u32 offset table, each package an 11-entry container = LOD distances (400/800/1600/6400
or 0/800/6000/10000) + an ACM LOD chain + GIM textures (ACM/GIM = the formats `acm_lib.py` / `model_baker.py` already decode; 17 of the 28 first
ACMs decode directly with the probe's approximate slicing, extents 27-87 x 16-58 x 49-99 units = vehicle size: tanks, trucks, AA). `m02a.mmb` (293 KB) = the mission script: world
triples such as (-36469, 4000, 144130) (the player start: alt 4000 = E6 y), (17110.4, 1852.7, 21348) (= the test-16 first world camera
(17110.4, 1851.9, 21311.9)) and waypoint grids at y 8000; a record spacing of 40 floats recurs 44 times. These are DYNAMIC ground units that
the game's replay already draws at their live positions (registered, shared camera), so baking them would double-draw them; decoding is
feasible (~1 day for the models, more for the script) but not useful for the static world. No static houses were found in the pack.

## 3. Implementation and acceptance
* **look:1** (inside `xr_terrain_stage`, needs `sky:1`/`env:1`, default 0; no new config key): `Conventions::look`; constants `kLook*`
  (terrain_stage.h, fitted values above); `build_env_texture` writes them into env texture slots P[26..29] (zero in every bake) and scales the
  moon strength; the haze length becomes `kLookHazeL` unless `fogl:` is given. CPU (`sky_at`, `env_sky`, `env_haze`, `look_gain`) and HLSL
  (`te_look_row`, `te_sky_at`, `te_sky`, `te_haze`, `te_look_gain`, props composite) mirror each other; look:0 is WORLD-2 byte for byte (the
  slots stay zero; WARP "env off" cases unchanged).
* Acceptance [E] (C++ CPU reference at the step62 pose, env:1, vs the game window / glass; `world4_check.py`):

| | game | look:0 (WORLD-2) | look:1 |
|---|---|---|---|
| ground 11-16 km | (61,70,58) | (83,91,73) | (64,74,63) |
| ground 16-22 km | (68,77,66) | (93,102,84) | (70,82,71) |
| ground 30-42 km | (77,93,86) | (105,117,101) | (78,95,87) |
| ground 42-60 km | (84,104,100) | (105,118,107) | (81,99,94) |
| beyond 60 km | (92,113,109) | (121,139,129) | (90,110,105) |
| sky +2.0 deg | (125,147,140) | (134,159,152) | (124,146,138) |
| sky -0.5 deg | (107,127,121) | (135,155,147) | (105,126,120) |
| **mean abs error, all usable pixels** | | **12.2/255** | **3.8/255** |
  Sheet: `D:\acz-extract\out\world4\step62_look_sheet.png` (game / look:0 / look:1).
* **propdedupe:1** (`world_dedupe.h`; options `propdedupe:0/1`, `dedupetol:<u>` 0.01..100, default 2): the proxy publishes the prop
  placements once per load (`event=props-dedupe;placements=..;tol=..`); the step-5 producer refuses a world record whose B translation
  (floats 76..78) matches one as `world-dedupe-prop`, category **target** (out of scope: never a loss, the frame stays world-complete), in
  both the member path and the early-open path (where it does not open the frame).
* **Forest fill**: `objects_baker.py <stage> --forest 3` (default 0 = WORLD-3 file byte for byte); pre-baked `D:\acz-extract\stage-w4\m02a.zvro`.
* Tests: terrain_offline `test_look` (options, texture slots, sky vs the pre-V rows **worst 4.9/255** (look:0 33/255), haze target F looking
  down, gain on flat ground, faint moon); WARP harness 3 look cases shader vs `trace_env` (max error 0.0028, 0 bad pixels) + look changes the
  image; props_offline `test_dedupe` + the real forest bake when present; step1 offline: dedupe category/loss and the record matcher; baker
  selftest (forest cells, hashed positions, N=0 identity).

## 4. How a driver runs it
1. Assets: the v3 stage (`D:\acz-extract\stage-w2\m02a.zvrs`, already installed) and the forest bake: copy `D:\acz-extract\stage-w4\m02a.zvro` to
   `%LOCALAPPDATA%\acz-vr\stage\` (or `python -I research/world-render-data/objects_baker.py m02a --forest 3`).
2. `-TerrainRender 'm02a;env:1;look:1;props:1;trees:1;propdedupe:1' -DepthFix` (+ the cockpit model as in step62).
3. Check: `event=terrain-env ... active=yes`, `event=props-dedupe;placements=100`, the -world status line's target-category count growing near
   buildings, no `props-disabled`. Compare a png-once dump with the game window as in section 3 (`world4_check.py` style: ground bands, sky rows).

## 5. Not verified / open
* Nothing ran in the game: the look is fitted on three m02a shots (one stage, one heading, daytime); other stages use their own sph/mapset
  colours through the same rule (F and the gain are m02a fits) [I]. Sky AWAY from the moon azimuth, clouds overhead, the sun: unverified.
* Forest fill density and pattern, tree scale, far-tree billboards; the right-hand house cluster; whether dedupe catches every building record
  (multi-part pmmdl models share one B? [I]); whether the replay also draws trees (check the step-5 world counts near a forest with trees:1).
* COCKPIT: canopy glass transmission (0.951, 0.940, 0.935) measured here.
