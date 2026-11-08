# Terrain tile mapping, offline preview renderer and runtime design (D-292 follow-up, first milestone)

Offline only: user's installed data via `D:\acz-extract` (extracted with `research/world-render-data/arcread.py`). No game launch, no
network, no installs, no game data committed (rendered PNGs stay in `D:\acz-extract\out\`, not in the repo).
Tags: **[E]** measured/seen in this pass, **[I]** inference, **[U]** unknown.
Code: `research/world-render-data/terrain_lib.py` (loader, tile mapping, PNG writer), `terrain_render.py` (z-buffer rasteriser).

## 1. The `.mti` mapping (D-292's main unknown) -- SOLVED structurally

Result [E]:
- `.mti` is **B blocks of 16x16 u16 words** (B*256 entries: m01a 4, m02a 17, m07a 25, m16n 25; 1024/4352/6400/6400 / 256).
- `.mta` (16x16 u8, one per height chunk) is the **texture block number** of each 32x32-cell chunk: `B-1 == max(mta)` in all 5 stages tested.
- A chunk is therefore covered by 16x16 tiles of 64x64 px = **2 cells per tile, 32 texels per cell**, so the stage is a 256x256 tile grid
  (`Stage.tile_words()`): `word(ty,tx) = mti[mta[ty//16, tx//16]*256 + (ty%16)*16 + tx%16]`, +tx = +X, +ty = +Z (same axes as the height grid).
- Word = `tile_id` (low 10 bits, always `< N` = number of tiles in `.mtd`, checked in 5 stages) + **flag bits** (bit 10, and bits 12/13 in m02a).
  The "ids exceed N" puzzle of the inventory is just the flag bits. No palette bank, no rotation/mirror: testing flip-X/flip-Y/transpose on
  flagged tiles raised the seam error vs neighbours (plain 5.2 vs 9.4-10.5 in m16n; m02a plain 11.7, flipY 14.0, flipX 34) [E, weak metric].
  What the flags mean is **[U]** (candidates: overlay/road layer, alpha, second layer; m02a has pairs like 4312/5337 forming a column that
  looks like a road/runway strip, i.e. detail overlays; m07a has only 48 flagged words, m16n 261).
- Block 0 is the generic **filler** and most chunks use it (`mta==0` for 61% of m16n, almost all of m07a); the height chunks `.mha` are likewise a
  repeating filler (m07a `.mha` rows read `1,2,1,2,...`). Unique-art blocks sit exactly where `.mha` chunks are unique: m07a 20 of 25 unique-use
  chunks have `mta!=0`; m02a 6 of 6. So each stage = tiled filler + a few unique islands (cities, coast, runways), and the in-game horizon
  is probably this tiled filler too [I].

Validation (honest):
- **m16n coastline [E, strong]**: the mosaic built with this mapping (`D:\acz-extract\out\mosaic_m16n.png`) and the assembled heightfield
  (`height_m16n.png`) show the identical diagonal coast, the same ~35 islet positions and the same sea/land split, with no flips -> the axis
  convention and the `mta -> block -> tile` indexing are right at chunk level.
- **m07a [E, weak]**: mosaic is a tiled mountain filler with unique patches; its pattern period matches the `.mha` repeat. The first block (identity
  tile ids 0..255) is a seamless 1024x1024 aerial photo with built-in 3-tile-wide seams, which are inherent in the data.
- Tile-to-neighbour seam metrics were useless (adjacent filler tiles are not seam-matched by design); the visual/chunk checks above are what I rely on.
- **Game screenshots [E honest]**: the repo has only HUD/cockpit mirror shots (`research/experiments/harness-runs/mission-look-sweep-observation-*/contact-sheet.png`,
  `step14-live-run11-full/mirror-shots/*`) of a Belkan countryside mission (green/brown field patchwork, dark woods, hazy low hills, ALT 2563). None record the
  stage id or camera pose (E6 records pose but not the stage), so **no registered comparison is possible**. Qualitatively the E6-pose render of **m02a**
  (`r_e6_m02a.png`) shows the same look: field patchwork, tree-speckled woodland, hazy low hills on the horizon. That is style agreement only; it does not
  prove m02a is the screenshot's stage. Colours in game look more saturated green than the raw CLUT (vertex colour/tint or `_dt` detail is applied) [U].

## 2. Preview rasteriser (throwaway, numpy only)

`python -I research/world-render-data/terrain_render.py <stage> out.png x y z yaw_deg [pitch_deg] [--hfov 63.2 --vfov 38.1 --range 160]`
Triangulates the 513x513 heightfield (2 tris/cell, window +-160 cells), projects, rasterises into an inverse-depth z-buffer (vectorised barycentric fill
bucketed by screen bbox; ~1 s at 960x539 for ~200k triangles), then shades from the reconstructed world xz: tile word -> mtd tile (mip by distance) -> CLUT,
hillshade, fog, flat sky. `.pdl` objects are 1800x3600x1800-unit boxes (colour = model id): **markers, not models**.
Examples produced [E] (in `D:\acz-extract\out\`): `r_e6_m02a.png`, `r_e6_m16n.png` (E6 camera (-29899, 4003.6, 137560), yaw 97.3 deg = 1.6982 rad,
pitch -4), `r_fly_m07a_1/2.png` (mountain flyovers with 372-445 object boxes), `r_fly_m16n_coast.png`.
Findings from the renders:
- Camera/terrain heights: E6 eye y=4003.6 is *below* terrain in m01a (ground 4241) and m07a (6756), above m02a (480) and m16n (32). The relief
  (max 20400 units) vs the 4000-unit flight altitude is consistent with a low-level mission; which stage E6 ran is unrecorded [U].
- Object placements land on the terrain at the right height (corr 0.99, D-292); boxes in m07a cluster on ridges (towns/bases) [E visual].
- m16n "sea" renders as grey rippled sea-floor texture: real water uses `_sea.gim`/`.wad` etc. [U].
- Mirror/handedness: SUPERSEDED by `terrain-calibration.md`: from compass+motion the game frame is right-handed, forward=(-sin yaw,0,-cos yaw); the +Z-forward default here is a mirror (use `terrain_calibrate.py`). Stage m02a, 1 u ~ 0.195 m (HUD ALT in ft) [I].
- Near plane: triangles with a vertex behind the near plane are dropped (holes at very low altitude); a runtime version needs real clipping.

## 3. Still unknown
Sea/water (`_sea.gim`, `.wad/.wbd/.wpd`); sky (`skytex*.bin` GIMs, 352-byte `.sph`, fog/haze colours); `.mta` beyond the block index (done), `.mii` (all-zero in 5 stages),
`.mia` (all zero), `.mid` (4 bytes: 90,90,90,128 -- maybe ambient colour/fog [I]); tile flag bits; `_dt.gim` detail texture and tint; sprite props/trees
(`_treeN.acm/.gim`, scatter rule); ACM prop topology/UV and pdl orientation/scale; LOD policy of the original; pitch/yaw/handedness conventions and
the real stage-coordinate scale (65 u/m only measured for the cockpit).

## 4. Runtime design (all [I] until a live capture confirms)

**Stage detection.** (a) Preferred: hook the engine file open/read for `data/map/<id>/<id>.mhd|.mha|.mtd` (first hit after a mission loads gives `<id>`;
fall back to `mapset/<id>`/`skytex<id>.bin`). The VR DLL already runs inside the game, so a `CreateFileW/ReadFile` or the archive layer's open call
is the interception point; log the id once into the existing status channel. (b) Fallback: config/CLI override `-WorldStage m02a`, and a probe that
compares recovered camera y against terrain (underground => wrong stage). Terrain data is loaded by our own code from `a.arc` using the ported
arcread logic (XOR + NOT + LZ4; port to C++: ~150 lines) at first stage detection, cached as a small baked file (heights 263 KB, tile words 128 KB,
tiles N*4 KB + CLUT) beside the DLL.

**Camera.** Use the game's own world frame: terrain local = world (-262144..+262144 in X/Z, Y = byte*80), E6 camera fields
(`+0xFDD860..` position, `+0xFDD870..` pitch/yaw/roll) or the VP decomposition `world_to_clip = inverse(M+256)*M+64` already used by the project
(the "16x" is the PS2 model scale, i.e. model units*16 = world units; world->metres is the unverified 65 u/m). Calibration step, needs one live run:
log `(stage id, eye xyz, yaw)` together with a screenshot of a known landmark (a pdl object such as m02a's tower, or the runway block), render the same
pose with `terrain_render.py` and fit yaw sign/handedness and the Y offset; accept when landmark and horizon registration agree within a few px.
Per eye: `eye_world = aircraft_cam_world + head_pose_in_cockpit * 65-scale`, so the HMD pose offsets the recovered camera, as with the cockpit.

**Drawing (D3D12, existing XR consumer, beneath the cockpit).** Terrain only needs: a static 513x513 R8 heights texture (or 16x16 chunk meshes), a
256x256 R16 tile-word texture, an 8-bit tile array texture (N x 64x64 + mips, R8) + 256x1 CLUT, per-eye VS that displaces a clipmap/chunk grid by height, PS that
reconstructs tile/texel from `xz` exactly as `terrain_render.shade` does (point-sample the index, palette lookup in the shader, bilinear done on the RGB
after lookup or by expanding to RGBA8 BC1/array at load), constant sun, distance fog, sky gradient/`skytex` clear. LOD: chunk rings (33x33 chunks at 1x, 2x, 4x
beyond ~24, 64 cells), tile mip by distance (mips are in the .mtd). Cost: <=300k tris/eye, trivial. Composition: render into the xr_stereo_output
**world layer** (already below the cockpit layer), depth-cleared per eye, with the cockpit/HUD layers composited above as today; the game's
own pre-projected ground draws (which the DLL currently captures) must be hidden/ignored in this layer so the two grounds do not fight. Objects: boxes
first from `.pdl`, ACM props later via the existing ACM VIF decoder.

**Sequence.** (1) C++ stage detect + data bake log (no render), (2) static mono render of terrain at the recovered pose into the world layer, compared to the
desktop window; (3) per-eye with head offset; (4) sea/sky/haze; (5) props. Gate each on a live headset/desktop capture; nothing here has run in the game.
