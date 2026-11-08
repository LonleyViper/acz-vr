# WORLD-CLOUDS: clouds around the whole sphere, per eye, in world space (`wcloud:1`, default OFF)

Headset tests 2026-10-28 item 9, 2026-10-29 item 5 and 2026-10-30 item 3 all say the same thing: clouds appear only inside the game's forward
rectangle. Tags: **[E]** shown by evidence or a test in this repo, **[I]** inference. Offline only: no game, SteamVR or headset was run for this.

## 1. Why replaying the game's clouds cannot work (the evidence)

* [E] (effects-outside-window.md 17.2) In the cloud-deck flight `D:\acz-evidence-archive\clouds\runT2` every dumped GREATER `4caf6e2d` puff draw
  (1,027 of 1,027) has a vertex exactly on the GS window border, and no draw has a vertex outside the window. The game clips its cloud sprites to its
  own window and culls the ones outside it before it submits them. A per-eye replay can only re-project what the game's camera saw.
* [E, new in this pass] runT2's fxsig dumps unprojected with the frame's game camera (camlag `ge/gf/gu`, lens 0.6152 / 0.346; z form
  `d = 2^24 / (z + 255.0078)`; script method as in `fx_unproject_check.py`). The camera was static on the k00a/m02a runway at y = 1,852 looking
  north. Results:
  * The 1,027 puff quads are screen-aligned rectangles with one depth per quad (camera-facing billboards). Their unprojected centres sit at altitudes
    5,530..13,586 (p10..p90, mean 9,560), at view distances 8,951..31,864 (p10..p90). Their world size is thousands of units: one example is a
    260-pixel quad at 25,500 units, about 15,900 units wide. In m03k the plan lines give `qm` 3,800..7,500.
  * The GREATER_EQUAL cloud card in the same frames is 42.8..47.4 k units away at about 10.4 k altitude.
  * m02a's `mapset` cloud-layer fields (+0x210) read 16000 / 15000 / 16500 / 18000. The measured puffs (5.5..13.6 k) do NOT sit at those altitudes,
    so the WORLD-2 reading "layer altitude" is doubtful [E for the mismatch, I for the meaning]. m03k's fields read 6000 / 6000 / 5000 / 5250.
  * Not decidable from the archive: whether the puffs are static in the world (the camera did not move during the 3 s dump window), how they are
    placed, and whether the clip changed their uv.

## 2. Routes considered

| route | what | verdict |
|---|---|---|
| (a) persist the game's puffs | Rebuild each clipped quad's full rectangle from its uv box, then keep a world cache of seen puffs and draw it per eye with ageing. | Not alone. It can only show puffs the forward camera has already seen: at takeoff and while looking back the cache is empty. It also needs the uv to have been clipped in proportion [I, unproven], and the game's culling hides every puff that never entered the window. It is still useful as calibration data (section 5). |
| (b) our own layer from game data | A world-space puff field in the terrain ray, using the stage's env data: cloud colours (mapset +0x260), coverage map (skytex 6), haze. | **Chosen.** It is VR-native (a genuine 3D world layer under D-291), covers every direction from the first frame, is stereo-correct and depth-correct, and has no game-side dependency. The game's exact placement rule is unknown [I], so altitude, size and spacing are named options. |
| (c) hybrid | (b) plus the game's own puffs removed from the per-eye replay (no double, no straight edge), plus a read-only census of the game's puffs to calibrate (b) per stage. | **What is implemented.** (b) plus the drop plus the census. |

## 3. What is built (all default OFF; with the options absent every image and log is unchanged)

Options go inside `xr_terrain_stage` (no new config key). They need an environment: `env:1`, or any of `sky/light/cloud/water`, on a v3 stage.

| option | default | meaning |
|---|---|---|
| `wcloud:0\|1` | 0 | the world puff field |
| `wcalt:<u>` | the stage's measured puff band (section 6; m03k 10600, m02a 9500), else layer-0 altitude P[18].x | band centre altitude (-2000..60000) |
| `wcthk:<u>` | the measured band (m03k 4400, m02a 8000), else 3000 | band thickness (centres spread over it) |
| `wcsize:<u>` | 3000 | puff radius R (0.6..1.0 R per puff) |
| `wccell:<u>` | max(7500, 2.5 x wcsize) (was 12000) | grid cell; must be >= 2.5 x wcsize |
| `wcop:<0..1>` | 0.9 (was 0.6) | opacity |
| `wcfill:<0..1>` | 0.8 (new) | fraction of cells holding a puff (hash gate); the coverage map only modulates density (0.7..1) |
| `wcdrop:<u>` | 2000 | fxmem:6 only: a class-1 (GREATER 4caf6e2d) quad whose longest WORLD edge is >= this is a game puff and is collapsed in the per-eye sprite copies (0 = keep them) |
| `wcdiag:0\|1\|2` | 0 | fxmem:5/6: read-only `fx-cloud-puff` census lines; 2 = also the magenta coverage proof tint (section 7) |

* **The field.** The CPU reference is `src/acz-vr/world_clouds.h` plus `terrain_stage.cpp world_clouds/world_clouds_apply`; the shader is
  `xr_stereo_blit.hlsl wc_accum/wc_over`. The constants are mirrored as `k*` / `kTWc*`.
  * Each xz grid cell holds at most one sphere. Its centre (inside the cell's column, with a margin of its own radius) and its radius come from an
    integer hash of the cell, so the field is identical on the CPU and GPU, for both eyes and in every frame (world-anchored).
  * Density is the stage coverage map at the centre. The ray walks the cells it crosses (2D DDA, at most 40) while it is inside the band and nearer
    than its limit. Puffs lie in disjoint cell columns, so the walk meets them strictly front to back and the over-compositing is exact (tested
    against brute force).
  * Per puff: `alpha = dens * op * (1 - (b/r)^2)^1.5`, times a coverage-map noise at the closest point, times a near fade (250..1500 units, so
    flying through a puff does not pop to white). The colour runs from the stage's lit colour (top) to its dark colour (bottom), and the puff is
    hazed at its own distance with WORLD-1's law, like the WORLD-2 sheet. It is composited in the DISPLAY domain:
    `decode(C + (1 - A) encode(under))`.
* **Depth.**
  * Terrain and props: the ray limit is the terrain hit, or a nearer prop, after `props_over`.
  * Replayed objects: the world layer of an object pixel takes the puffs BEHIND the object (from its ray distance `object_ray_dist`). The puffs IN
    FRONT (from 0 to `min(r_obj, terrain)`) are composited over the object.
  * Cockpit: an opaque cockpit pixel never gets a puff, because there is no ray march under it.
  * Game sprite and flare layers stay on top (`eye_fx_over`, `lf_over`).
* **Producer.**
  * `stage_effects(..., wc_drop_edge, wc_diag)` sets `effects_world_cloud_drop_edge` and `effects_cloud_puff_diag_flag`.
  * `fx_sprite_replay.h mark_puff_quads` marks the puff quads and runs; `plan_draw(..., drop_edge)` collapses them in both eye copies. The
    `fx-sprite-totals` line gains `;wcd=N`.
  * Debris and tracers (edges of tens to hundreds of units) are untouched.
  * Residual: draws that fall back to the WINDOW effects image (`window-fallback`) still show the game's puff inside the rectangle.
* **Logs.**
  * `xr-stereo;event=world-clouds;requested=1;active=yes|no(...);alt=;thick=;cell=;radius=;opacity=;game-puff-drop-edge=;diag=`, written per stage
    install and on the monitor load path.
  * `gpu-copy-step1;event=fx-cloud-puff;frame=;eye=x,y,z;n=N;p0=cx,cy,cz,edgeU,edgeV,depth,uv:u0-u1/v0-v1,b0|1;...`: up to 4 puffs, at most one
    line every 250 ms, only with `wcdiag:1`.
* **A bug found and fixed while building.**
  * WARP produced nondeterministic garbage even in mode 0 when the object branch inlined two terrain traces and three cloud walks. It is now one
    trace plus one front walk.
  * `wc_on()` also requires the env bits: the env SRV is bound only with them, and a stale descriptor from an earlier job had turned the field on in
    unrelated jobs.

## 4. Tests (private build D:\b-wcl, Release, mem-guard 62-70 %)
* `world_clouds_offline` (new, 373,530 checks PASS):
  * options: defaults, 12 refusals, `wccell >= 2.5 wcsize`
  * env texels: only P[33..34] differ, zero when off
  * hash range and determinism; every puff inside its cell column
  * DDA vs brute force over every cell on 4,000 random rays: worst difference 0.0, 704 rays with clouds
  * two eyes and a moved camera see the same puff at the same world point; bytes are deterministic
  * the object split: front plus back equals the whole ray; a limit nearer than the puffs hides them; the near fade; lit above, dark below
  * the CPU trace path on the synthetic environment
  * producer: a 6,000-unit puff quad is collapsed per eye, a 40-unit speck is kept, `wcd=` is written, drop 0 is unchanged
  * census line values, the border flag and the 426-byte cap
  * the stage-switch table and apply/clear
* WARP harness (11,498 checks, 0 failed; 3 consecutive runs stable):
  * `t_wc_below`, `t_wc_inside`, `t_wc_above` and `t_wc_turn` (head 70 deg off the nose): the real shader vs the CPU mirror, max error 0.0027..0.0039,
    0 bad pixels; each changes the image; `wcloud:0` builds the WORLD-2 texture unchanged.
  * New object case: a replayed object 3 km away behind a puff, 324 of 324 pixels matched to the CPU mirror within 1/255 in both eyes.
* Also run and passing: `terrain_offline` (3,408), `lens_flare_offline` (81), `fx_census_offline` (1,616), `flight_stage_adapter_offline` (66), all
  `gpu_copy_step1_step5-effects*/world*/hud*` ctest scenarios, all 20 `xr_stereo*` ctests. The `acz-vr` DLL (dxgi.dll) compiles; it is NOT installed.
* NOT run: full build or ctest of the integration tree, game, SteamVR, headset, GPU cost (`xr_gpu_bench`).

## 5. What a null live run must record (pending capture; it decides the defaults and whether route (a) adds anything)

Use the day config plus `;wcloud:1;wcdiag:1` (fxmem:6 is already in it; check the 255-character option budget). Fly m02a and m03k, including a
climb through the cloud deck and a look 60..180 deg off the nose.

1. `event=world-clouds;...;active=yes` at each stage install.
2. Eye PNG or mirror: puffs in every direction (behind and below too), the same world positions in both eyes, no straight edge at the window border,
   and the game's puffs gone from the rectangle (`fx-sprite-totals` `wcd=` > 0 while flying in clouds; `window-fallback` low).
3. `fx-cloud-puff` census, per stage:
   * (i) Altitude distribution of `p*` cy (sets `wcalt`/`wcthk`); edge lengths (sets `wcsize`); spacing (sets `wccell`).
   * (ii) The same puff (centre within ~100 units) in lines seconds apart while the aircraft moves = world-static puffs. Then a seen-puff cache
     (route a) or seeding the field from the census is possible.
   * (iii) `b1` puffs with a uv box narrower than (0..1) = the clip moved the uv, so un-clipping by uv is possible.
4. Objects: a ground vehicle seen through a puff is veiled; a vehicle in front of a puff covers it. The cockpit frame always covers the puffs.
5. Perf: summary-timing with vs without `wcloud:1` while looking level inside the band (the worst case: up to 40 cells per pixel). If it is too
   slow: raise `wccell` (fewer cells) or lower the band.

Promote measured per-stage values to defaults (a stage table like `StageLook`) only after step 3. Until then `wcalt` defaults to the layer-0 field,
which section 1 shows does not match m02a [E]. For an m02a A/B use `wcalt:9500;wcthk:8000` [I: from the runway dump].

## 6. WCLOUD-FIX (step138): the null runs drew nothing (root cause and fix)

Evidence: `D:\acz-evidence-archive\step137\batchAG\runAGC1` and `runAGC2` (m03k, step137, `wcloud:1;wcdiag:1`). Both logged
`event=world-clouds;requested=1;active=yes;alt=6000;thick=3000;cell=12000;radius=3000;opacity=0.60`, the game's puffs were dropped from the eye copies
(`fx-sprite-totals` `wcd=1..6`), and the eye dumps (`upright\*-final.png`) show clear sky (AGC1) and bare terrain from above (AGC2): neither the game's
puffs nor ours.

1. **[E] The band was at the wrong altitude.** `wcalt` defaulted to the layer-0 field P[18].x = 6000 (m03k's `mapset` fields 6000/6000/5000/5250). The
   wcdiag census (`gpu-copy-step1;event=fx-cloud-puff` in `logs\step5-producer.log`, 626 + 279 puffs) puts the game's own puffs (atlas v range ending
   <= 0.8) at p10 8.8 k, p50 10.6 k, p90 13.2 k while the eye flew from y 20,000 down to 12,000; the eye stayed at y ~20,000 for the first ~50 s
   (E6 `air=...,20000,...`, autopilot `pos=` column). The "5000-6000 deck" of the batch description is the layer field, not the puffs. (A second puff
   class, atlas v up to 0.95, appears only in the steep dive at frames 9232+ and tracks the eye down to y 2,000: not the deck.)
2. **[E] The field was nearly empty even where it was.** m03k's coverage map is binary: 18.8 % of its 256 x 256 texels are 255, the rest 0, at a
   65,536-unit repeat (blobs ~1 k units). The old density law `smoothstep(0.2, 0.6, map at the puff centre)` left 22.6 % of the 12 km cells with a
   puff, each puff covering ~13 % of its cell: ~3 % of the deck seen from above, at a mean per-puff alpha of ~0.18 (0.6 x the (1 - x)^1.5 disc mean 0.4
   x noise 0.75). An offline replay of the CPU law on the real m03k map from the AGC2 pose (eye y 20,000, 35 deg down) gives 0.2 % of rays with
   coverage > 0.05 and a mean coverage of 0.0003, i.e. nothing visible. At the census band with the same law: still ~0 (the density, not only the
   altitude, was the problem).
3. Ruled out: the env-bit gating (`active=yes` is computed from the same texels the shader reads; `env:1` sets all four env bits), the cell walk
   (40 cells x 12 km covers the 160 k march), the depth test (AGC2's rays reach the terrain at ~35 k, beyond the deck), haze (f = 0.34 at 23 k with
   L = 55,000).

Fix (CPU `world_clouds.h` and shader `wc_accum` changed together; `wcloud` absent writes P[33..34] = 0 and no code runs, so every image is unchanged):
* `stage_band`: a per-stage table of the measured bands (m03k 10600 / 4400 from the census above; m02a 9500 / 8000 from section 1). Precedence:
  `wcalt`/`wcthk` option > table > layer-0 field and 3000. A takeoff stage that adopted the mission environment uses the mission's band (`env_id`).
  The config line gains `fill=` and `alt-src=option|stage-census|layer0`.
* Density: a cell holds a puff when `unit(hash(i, k, 5)) < fill` (default 0.8); its density is `0.7 + 0.3 x smoothstep(map)`. Falloff exponent
  1.5 -> 0.75, opacity 0.6 -> 0.9, cell 12000 -> max(7500, 2.5 x wcsize). The same replay from the AGC2 pose now gives ~43 % of rays > 0.05
  (18 % > 0.2), and level inside the band ~92 % of azimuths [offline only].
* Tests: `world_clouds_offline` gains the band table, option precedence, the takeoff adoption, the config line, the fill gate on an m03k-like binary
  map, the AGC2 pose and a level ray inside the band; the existing DDA/brute, anchoring, depth and WARP shader-vs-CPU cases run on the new law.

Null-run cfg for the A/B: the day `xr_terrain_stage` string plus `;wcloud:1;wcdiag:1` (no `wcalt`: the line must say `alt=10600;thick=4400;
alt-src=stage-census`). Fly level at y 9,000..13,000 for part of the run (the autopilot's TargetAlt is game y: `-TargetAlt 11000 -AltCeil 13000`), then
above the deck. NOT verified live: whether the deck is visible in the eye dumps, its look against the game's puffs, GPU cost (7.5 k cells: up to ~30
cells per level ray inside the band).

## 7. WCLOUD-FIX2 (after the step138 null runs): a deck at the right place that nobody could see

Evidence: `D:\acz-evidence-archive\step138\batchAG` runAGC1 / runAGC2. The line read `alt=10600;thick=4400;cell=7500;radius=3000;opacity=0.90;fill=0.80;
alt-src=stage-census`, yet the eye dumps (`upright\*-f244-*` AGC1, `*-f241-*` AGC2, worldfx = the world layer alone) show no puff.

* **AGC1 is not a test of the deck at frame 240.** Its autopilot log puts the aircraft at y 20,000 -> 19,900 for the first 4 s (`pos=` column; the
  dump is flight frame ~244). The band is 8,400..12,800 (puff tops up to ~15,800), so level and near-level rays meet it 40..90 k away, where the
  look:1 haze (below) is 85..98 %.
* **The live path does bind and read the field** [E from source, not from a GPU readback]. `xr_flight_stage.cpp build()` writes P[33..34] into
  `rt->env` (the same texels the config line reads); `xr_stereo_output.cpp` passes `dt.env` as `job.terrain_env` whenever env bits are set;
  `xr_stereo_d3d12.cpp` uploads all 257 rows (row 0 holds P[33..34]) and sets t_bits bits 4..7 only with the env texture bound; every terrain pixel,
  the worldfx debug view included, goes through `world_layer_terrain_t2`, where `wc_on()` gates the field. There is no other mode bit. There is no
  takeoff-stage switch in these runs (no `to-takeoff-base` line).
* **[E] Root cause: contrast.** The live day cfg has `look:1`, which sets the haze length to `kLookHazeL` = 21,500 (the tests and the earlier
  replays used 55,000). The step138 law then:
  1. hazed each puff twice: its colour went toward the haze colour and its alpha was also multiplied by (1 - f);
  2. shaded it by the height of the ray's closest approach to the centre, which is mid-grey seen from above, close to the haze colour below the
     horizon (the AGC2 worldfx shows ~(67,85,105));
  3. thinned it (falloff exponent 0.75, noise 0.5).

  Replaying that law over the AGC2 dump's own colours (haze ~(0.26,0.33,0.41), ground ~(0.31,0.32,0.34), m03k cloud colours) from the AGC2 pose
  gives a mean display luminance change of 1.5..2.1 / 255. Only 3..7 % of rays change by >= 8 / 255. That matches the dumps: nothing visible.
* **Fix (CPU `world_clouds.h` and shader `wc_accum` / `wc_over` together):**
  * the alpha is no longer hazed (the colour still is);
  * lit/dark comes from the height of the ray's ENTRY point into the sphere, so a deck seen from above shows lit tops;
  * falloff 0.5, noise 0.3.

  The same replay gives a mean of 5.7..6.9 / 255, and 31..37 % of rays change by >= 8 / 255 (offline only). The `world_clouds_offline` case
  "AGC2 pose, live haze 21500" pins both numbers.
* **Telemetry (one run proves which side is wrong):**
  * `wcdiag:2` (= `wcdiag:1` plus P[34].w = 1) makes the shader paint its puff coverage magenta, `min(4 A, 1)`. Any magenta in an eye dump
    proves that the GPU branch runs with live constants. The WARP case `t_wc_tint` checks the shader against the CPU mirror.
  * `xr-stereo;event=summary-wcloud` (every summary window, `wcloud:1` only) gives the CPU mirror on an 8 x 6 grid of eye 0's rays over the CPU
    terrain colour: `eye=`, `band=`, `with=` (coverage > 0.05), `mean-a`, `max-a`, `mean-dl` / `max-dl` (display luminance change in 1/255),
    `vis8` (rays >= 8/255), `haze-l`, `far`.
  * Reading: `with` > 0 with no magenta in the dump means the GPU side is broken. `with` = 0 means the camera or band is wrong.
    `vis8` = 0 with magenta present means a tuning problem.
* Side finding: the autopilot `pos=` frame and the census/E6 eye differ by about (+39.5 k, 0, +39.5 k) (AGC1 T=0 `pos=104,118,20,000,105,142` vs census
  `eye=143650,20004,144674`). The y values agree, so the band is unaffected.

Null-run cfg: the day `xr_terrain_stage` string plus `;wcloud:1;wcdiag:2` for the proof run (magenta), then `;wcloud:1;wcdiag:1` for the look. AGC1
must reach the band before the dump: start the dump later or descend first (from y 20,000 to the band takes ~200 s at the logged ~42 units/s
sink rate).

## 8. WCLOUD-LOOK (after the step140 magenta proof): visible, lumpy, lit cumulus

Evidence: `D:\acz-evidence-archive\step140\batchAG-magenta\runAGC2`. `wcdiag:2` painted the puffs magenta all around the cockpit, so the GPU branch runs
with the live constants. `summary-wcloud`: AGC2 `with=20/48;mean-a=0.202;mean-dl=3.58;max-dl=16.6;vis8=14`; AGC1 (eye y ~14,000) `with=6-10`,
mean-dl 2-3. The magenta also showed hard round discs.

Game reference [E, flat captures]:
* `clouds\runT2\sheetA.png` (m02a): the game's puffs are soft light-grey masses. Their bright parts are ~(130,148,140) against a sky of
  ~(95,112,106), i.e. about 1.3x the background.
* step137 AGC1 game shots (m03k, ALT 10462): soft grey-white masses below the horizon.
* m03k's own cloud colours (mapset P[21] lit 171,164,149; P[22] dark 98,92,78) are kept as the base. The game's puff texture is not reachable from
  the terrain/env path (it lives in the effects stream as pre-clipped GS quads), so the outline is procedural.

Changes (CPU `world_clouds.h accumulate` and shader `wc_accum` together, constants `k*` = `kTWc*`):
* Lumpy outline. `body = sat(((1 - (b/r)^2) - kEdge (1 - e)) / (1 - kEdge))`, alpha = `dens op body^kShapePow`. Here `e` is two octaves of the
  coverage map at the closest point (repeats 60,000 and 21,000 units, weights 0.6 / 0.4, offset per cell by hashes 1/2). The rim lies between
  0.74 r and r, in lumps (offline: 0.74..0.86 and 0.74..0.94 r over 16 azimuths, from straight above).
* Lit tops: the lit colour x `kLitGain` 1.3 (clamped to 1); entry-point shading as in section 7.
* Haze at `kHazeScale` 0.5 x the puff distance. Bright clouds keep contrast through look:1's L = 21,500, and distant puffs still go to the
  horizon colour. Offline, the same deck seen 4x farther gives less than 0.8x the contrast.
* AGC2 pose, Python replay on the real m03k map, display luminance change against the step138 dump's ground and haze:

  | | rays with coverage | their mean change | max change |
  |---|---|---|---|
  | section 7 law | 214 / 450 | 12.1 / 255 | 34.9 / 255 |
  | this law | 141 / 450 | 29.9 / 255 | 72.5 / 255 |

  The lumps trim the coverage; the contrast more than doubles. The `world_clouds_offline` case "WCLOUD-LOOK" pins mean > 15 and max > 60, plus
  the outline variation and the haze fade.
* `wcloud` absent: P[33..34] zero, no code runs (unchanged).

Null-run check: `;wcloud:1;wcdiag:1` (no magenta). Expect `summary-wcloud` mean-dl well above the step140 3.58 and max-dl near 60+ in AGC2. In the eye
dump, expect grey-white lumpy puffs with lighter tops over the terrain. NOT verified live; the GPU cost of the two extra map lookups per puff hit
is not measured.
