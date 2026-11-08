# WORLD-3: buildings, trees and static props on the terrain (decode, offline check, runtime pass)

Track WORLD-3 of the overnight mandate (D-302). Offline, read-only on game files, no game/SteamVR launch, no network. Game-derived outputs live only in
`D:\acz-extract` (`stage\<id>.zvro`, `out\world3\*.png`); nothing derived from game data is committed (the committed fixture
`tests/acz-vr/fixtures/props_synth.zvro` is synthetic). Tags: **[V]** verified here (exact identity, a statistic far from chance, a test, or a render that
matches the game window), **[I]** inference.

| file | role |
|---|---|
| `research/world-render-data/world_objects.py` | decoders (.pdl/.pmi/.pml/.wad/.wpd/.wbd) + the verification statistics (`python -I world_objects.py <stage>`) |
| `research/world-render-data/world3_render.py` | offline render: terrain + props + trees from a game pose, sheet against a game-window screenshot |
| `research/world-render-data/objects_baker.py` | `<stage>.zvro` baker (instances, models, textures), `--selftest`, `--fixture` |
| `research/world-render-data/test_world_objects.py` | synthetic decoder tests (9) |
| `src/acz-vr/world_props.{h,cpp}` | SDK-free: .zvro loader, options, ground snap, grid + per-frame selection, CPU references (projection, occlusion, shading) |
| `src/acz-vr/xr_props_d3d12.{h,cpp}`, `xr_props.hlsl` | the GPU pass (new files, new entry points) |
| `src/acz-vr/xr_props_composite.hlsli` | the merge into the terrain composite (included by `xr_stereo_blit.hlsl`) |
| `tests/acz-vr/props_offline_tests.cpp`, `xr_composite_warp_harness.cpp` `case_props` | tests |

## 1. Placement data (decoded)

### 1.1 `.pdl` = `u32 count, 12 pad, count x {f32 x, y, z, u32 model}` and NOTHING else [V]
* `model` is the index into `pmmdl<stage>.bin`: max id = n-1 and every model is used exactly by id (m02a 44/44, m07a 415/415) [V].
* **There is no rotation or scale in the record because each placement has its own pmmdl model with the rotation baked into its vertices** [V]:
  the m02a house mesh (10 unique vertices) appears as models 0,1,2,10..14,22,24,25,29,31..33,39,42 related by PURE yaw rotations
  of 0 / 86.75 / 88.5 / 164 / 176 degrees (correspondence-free fit, symmetric max nearest-neighbour error 0.00-0.23 units; no mirror needed);
  other shapes repeat with yaw 0 (m02a 3/7/26, 4/16/17/27, 6/30, 23/41; m07a 22 groups). This is why most ids are used once.
  So `world vertex = (x, y, z) + model vertex` (no transform), which is what the runtime does.
* m07a mixes two kinds of records [V]: centred models with the record y on the ground, and **anchor records with y = 0 on a 512-unit lattice whose
  model vertices carry absolute heights** (vertex y 2500..11500 = the terrain there) -- terrain-hugging clusters. The plain translation handles both.
* Height check [V]: model base (record y + model min y) minus the bilinear terrain under the anchor: m02a median +0.1 u (p10 -94, p90 +0.6);
  m07a median -81 u (one height step; p10 -598 on slopes), corr(base, terrain) 0.9966 over 452 records. Bases sit at or slightly below the bilinear
  surface (buildings are sunk into slopes, never floating).
* m02a: 105 records, 5 of them use model 43 (locator-only, no geometry: not drawn). m16n, m01a, m10m: empty .pdl (4 bytes) [V].
* m02a objects repeat on a 32768-unit z lattice together with the repeating m02a heightfield chunks (D-292) [V].

### 1.2 `.pmi` + `.pml` = spatial lookup grid [V]
`.pmi` = 256x256 u16, cell 2048 u, `[row = z, col = x]`, value = a **record number in `.pml`**; `.pml` = 16-byte records of up to 8 u16 pdl indices
(0xFFFF unused), record 0 empty. Check: the placement is listed in `pml[pmi[cell of its anchor]]` for 100/105 (m02a; the 5 misses are the
locator-only model) and 442/452 (m07a). Not needed for drawing (our own grid replaces it).

### 1.3 Model id -> pmmdl node mapping [V, COCKPIT-1]
`pmmdl<stage>.bin` entry `id` = one ACM model; node `r3` indexes the stage's prop textures (pmtex mip 0); node transforms are identity in all m02a props.

### 1.4 Trees: `.wad` + `.wpd` (+ `.wbd`), NOT procedural [V]
The tree placement data was not in `.mii/.mia/.mid` (all constant zero / `5a5a5a80` in every sampled stage) and not in `.efa` (16 KB, byte-identical in all
stages). It is the `w*` trio ("wood" [I]):
* `.wad`: per texture block (the same block numbering as `.mti`, chosen by `.mta[chunk]`) 288 bytes = `u32 off[3][16]` (0xFFFFFFFF = empty) +
  `u16 cnt[3][16]`: for tree type t (`<stage>_tree{t+1}.acm/.gim`) and sub-square k (4x4 per 32768-unit block, kx = k % 4, kz = k // 4) a list in `.wpd`.
  Counts equal the offset differences exactly; the lists tile `.wpd` completely (m02a 13529 u16 = 13529 entries) [V].
* `.wpd`: sorted u16 positions, `x = v & 255`, `z = v >> 8`, 32-unit steps = **one terrain texel** inside the 8192-unit sub-square.
  World `x = -262144 + C*32768 + kx*8192 + x*32 + 16` (texel centre: [I]), same for z with the chunk row R.
* Evidence for the layout: mean CLUT luminance of the terrain texel under each tree is **31.3** for m02a type 1 (random texels 59.6; the x/z-swapped
  hypothesis 62.8; other sub-square orders 53-66), i.e. trees stand on the dark forest texels; 0 of 174,302 m02a trees on the river key colour;
  in the step56 render a decoded tree stands next to the game's tree beside the same building (section 2).
* Counts: m02a 174,302 (type 1: 173,171, type 2: 1,131, type 3: none); m16n 60,354 (15,832 / 27,166 / 17,356; types 2/3 stand on mid-luminance
  ground 57-60 [I: different vegetation]); m07a none (empty `.wpd`).
* `.wbd`: per block 1024 bytes = 64x64 2-bit values (512-unit cells), only values 0/1 seen; trees avoid the 1-cells (17 of 174,302 with low-bits-first
  order) -- a no-tree/open-ground mask [I]; not needed for placement.
* Tree models: crossed alpha-tested cards, 40 u tall (COCKPIT-1), base at y = 0. Tree y is NOT stored: the runtime snaps it to `height_bilinear(x, z)`
  of the same stage the ray march uses [I for the game's own rule; exact for our registration].
* **Tree scale [I, fitted]:** at the step56 pose the game's trees are 4.2-5.1x taller than the 40-unit model (two trees measured against the
  same-size house that matches 1:1); default `treescale:4.5`. No scale field exists in the files; the game may scale or use a LOD billboard.

## 2. Offline verification against the game (step56 nose-down pose)
`python -I research/world-render-data/world3_render.py D:\acz-extract\out\world3\step56_sheet.png --shot <game shot T7.03>` (E6 pose interpolated to the
png tick: campos (-13854, 3647, 121515), heading -45 deg, pitch -29.3 deg; 60 deg horizontal FOV; props depth-tested against the rasterised terrain).
* Pose/FOV: a luminance correlation scan (HUD/cockpit masked) peaks at the logged pose (pitch/yaw offsets of +-2..8 deg and altitudes 2200/2900 are
  worse); horizontal FOV 50-63 deg is flat (0.45-0.48) [V that the pose is right; FOV only loosely constrained].
* **The red-roofed village at the left is the orthophoto texture, not .pdl props** [V]: it is present in the texture-only render at the same place
  and shape; the m02a .pdl has nothing there. 3D houses in the game window that coincide with decoded props: the lone red-roofed building in the
  centre-bottom field (ours within ~10 px of 2421 at full resolution) and the red-roof cluster at the right edge (~40 px, same offset for its trees:
  an FOV/edge projection residual, not a placement error) [V]. The game draws several houses in that right cluster where m02a.pdl has 1-2: the
  extra buildings probably come from the mission pack (`data/mis/pack/*.mmb`, not decoded) [I].
* Trees: the decoded trees reproduce the game's isolated trees beside the centre building and the tree rows along the field edge; density ~340 trees
  within 16000 u in view, the same order as the game window [V qualitative]; the game shows more trees standing on the bright fields than ours [open].
* Outputs: `D:\acz-extract\out\world3\step56_sheet.png` (game | ours | 50:50), `crop_s4_a.png`, `crop_s4_b.png` (registration crops),
  `step56_terrain_only.png`.

## 3. Runtime (default OFF)

### 3.1 Data: `<stage>.zvro` next to `<stage>.zvrs` (objects_baker.py; layout in its docstring)
Models (32 B: index range, base vertex, kind prop/tree, bound sphere), textures (all resampled nearest to 128^2 RGBA8 for one Texture2DArray), vertices
(32 B, same as .zvrm with the texture layer instead of the part), u32 model-local indices, instances (24 B: position, scale, model, flag
ground-relative). SHA-256 of the payload; the C++ loader refuses short/oversized files, bad magic/version/header/sizes/hash, indices beyond the vertices,
layers beyond the textures, instances naming a missing model, non-finite values, unknown flags (all-or-nothing). Baked (not committed):
`D:\acz-extract\stage\m02a.zvro` (100 props + 174,302 trees, 47 models, 4.8 MB), `m07a.zvro` (452 props, 418 models, 1.4 MB), `m16n.zvro`
(60,354 trees, 1.6 MB). Install: `python -I research/world-render-data/objects_baker.py m02a` (writes `%LOCALAPPDATA%\acz-vr\stage\m02a.zvro`;
`--src <extracted map dir>` or `--arc <a.arc>`, `--out <dir>`).

### 3.2 Options (inside `xr_terrain_stage`, NO new config key: the key budget stays at 27-29 of 32, the value is a few bytes longer)
`props:1`, `trees:1` (both default 0), `treescale:<0.1..20>` (4.5), `treerange:<u>` (12000), `proprange:<u>` (58000 = the game's haze end), `propcap:<n>`
(16384 per frame). `acz::props::extract_options` removes them before the terrain parser; a bad value of one of them refuses the whole terrain like any
bad option. A missing/corrupt/mismatched .zvro logs `xr-stereo;event=props-disabled;...;fallback=terrain-only` and the terrain runs without props.
Log lines: `event=props-loaded`, `event=props` (counts, scale, ranges, cap).

### 3.3 Depth against the ray-marched terrain: the chosen design (and why)
The terrain is a full-screen ray march inside the eye composite pixel shader; it has no depth buffer. Options were (a) make the terrain pass write
SV_Depth and draw props after it, (b) render props into a private target with their own depth and let the composite compare. (a) needs the composite to
be split in two passes and the terrain shader to output depth (a large change to code WORLD-1/2 are editing). **(b) was chosen**: per eye, before the
composite, the props are rasterised (instanced, D32 reversed-Z GREATER among themselves) into two private eye-sized targets: albedo (RGBA8, display
domain) and the eye-facing normal + distance along the pixel ray (RGBA32F). In the composite (after the merge with WORLD-1/2: `world_layer_terrain_t`
calls `props_over` after `terrain_trace_t`), the prop replaces the terrain sample where its distance is smaller than the terrain hit of the same ray
(t_hit, 3e38 on a miss) and below the march end; t_hit then becomes the prop distance, so WORLD-1's object occlusion also treats props as solid.
This is exact with respect to the terrain the user sees (the same trace), and touches the existing shader in two lines (`#include` + the call).
Registers: t11 albedo, t12 normal+distance (t10 is WORLD-2's environment), b1 `t_bits` bit 8 (`TP_PROPS`; bit 3 = WORLD-1 object occlusion,
bits 4-7 = WORLD-2 environment). The props pass projection is DERIVED from the composite's own pixel ray
(`make_clip`: inverse of `d = base + u*ux + v*vy` mapped through right/up/fwd), so there is no second FOV/handedness convention to get wrong
(200 random rays reproduce their uv to 1.2e-7).

### 3.4 Culling, LOD, cap
CPU per frame (`acz::props::select`): a 128x128 grid (4096 u cells) over the stage; per instance the bound sphere is tested against the kind's range
(props 58000, trees 12000) and against both eye view planes (dropped only when behind both; the HMD can look anywhere, so no camera-forward frustum);
above the cap the nearest are kept (nth_element, ties by index: deterministic); survivors are sorted by model -> one DrawIndexedInstanced per model per eye.
LOD: none beyond the distance limits (the game's own prop LOD is not decoded); textures are mip-mapped. Instance data = 16 bytes (position, scale) in an
upload buffer written before the list opens (the backend proves the previous submission complete).

### 3.5 Shading (exactly like a terrain texel, WORLD-1 display domain)
The pass writes albedo = texture x vertex colour (alpha test 0.5) and the normal flipped to face the eye (two-sided cards and walls). The composite lights
it like `terrain_trace_t` lights a texel: WORLD-2 stage light + ambient normalised to flat ground with cloud shadows when `light` is on, else
`kAmbient + kDiffuse max(n.L, 0)`; then WORLD-1's haze `t_haze(tp)` toward `te_haze(d)` (sky on) or `kTHaze`, WORLD-2 clouds in front, and ONE
`t_srgb_to_linear` like the terrain. CPU reference for the default path: `acz::props::prop_color` / `prop_visible` (the env path is not separately
referenced; it reuses the terrain's tested te_* functions).

### 3.6 Model loader note (COCKPIT-2 coordination)
COCKPIT-2 (merged) owns the shared single-model loader (`cockpit_model.h`, .zvrm). The props pass keeps its own instanced-set loader (`world_props.cpp`,
.zvro: many models + instances + a texture array, which .zvrm does not describe), but the vertex record is the SAME 32-byte layout
(`pos[3] f32, uv[2] f32, nrm[3] i16, u16 (cockpit: part/material, props: texture layer), rgba u8[4]`), so either loader's vertex buffers can feed
the other's pipeline. Pass order per recording: props pass (before the eye composites) -> eye composite (terrain + props merge) -> COCKPIT-2 model.

## 4. Tests (all pass)
Release build D:-w3 (Ninja, -j2, clean twice) after merging master (WORLD-1 + WORLD-2); ctest result in the merge commit message.
* `props_offline_tests` (464 checks): the C++ serializer reproduces the Python-baked synthetic fixture byte for byte; 12 loader error cases;
  option extraction (defaults OFF, bad values refused, foreign keys passed through); ground snap on a slope (= `height_bilinear`, once); selection
  (range per kind, behind both eyes, cap keeps the nearest, props-only, determinism); projection vs the composite ray (worst uv error 1.2e-7);
  visibility against the terrain hit (ridge between eye and prop, 10 u above/below the ground, march end); colour reference (display domain, haze, linear).
* WARP harness `case_props` (real props pass + real composite on WARP vs a CPU reference = analytic ray/box hit kept where `prop_visible` against
  `terrain::trace`, coloured by `prop_color`, else `terrain::trace`): a box on a slope, a box half sunk into the slope and a buried box: **0 of 57,600 pixels** over 0.06 per eye (max
  error 0.005); the buried box alone gives byte-identical eye images to the terrain-only job; cap 1 draws 1 instance; nothing in range -> pass not run
  and byte-identical; key off (options off / no terrain) -> byte-identical; deterministic, no re-upload on reuse.
* `research/world-render-data/test_world_objects.py` (9 synthetic decoder tests), `objects_baker.py --selftest`.

## 5. Not verified / open
* Nothing here ran in the game or a headset; GPU cost on the real device is unmeasured (m02a: ~340 trees + ~15 props visible at the step56 pose;
  worst case = the cap). Registration in the headset depends on the terrain camera conventions (WORLD-1 roll fix).
* Tree scale 4.5 and the 16-unit texel-centre offset are fitted/assumed; tree type 2/3 meaning; `.wbd` meaning.
* Mission-pack objects (`data/mis/pack`, extra houses, ground units) are not decoded; the game's replayed DrawACM objects still appear in the
  cockpit layer (D-301 "buildings rotated with the world") until a later step hides them when the props pass is on.
* Lighting is the terrain's provisional sun; the game's prop lighting and its haze are approximations.
