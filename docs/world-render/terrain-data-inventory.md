# Terrain / stage data inventory (offline, read-only, D-291 research)

Scope: the user's own installed `D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\{a,b,c,d,e}.arc`.
Nothing was written to the game directory. Samples (28.4 MB) were extracted to `D:\acz-extract` (outside the
repo; no game data is committed). No game launch, no memory access, no installs.
Evidence tags: **[E]** = evidence-backed (measured by a script run in this pass), **[I]** = inferred, **[U]** = UNKNOWN.

## 0. Tooling note (important)

`aczarc.py` (ModDB) is NOT on this machine and no venv with `lz4` exists (numpy 2.5.3 is installed in the system
Python; `lz4` is not). Per the brief nothing was installed. I re-derived the container from the bytes and wrote an
independent pure-Python + numpy reader: `research/world-render-data/arcread.py` (`list`, `extract`; ~110 lines,
own LZ4 block decoder). Its list output matches the handoff (a.arc 4816 files, b 72, c 6, d 162, e 3) [E].
(Side note: I accidentally ran `pip download lz4` once, which only contacted the package index and saved nothing in the
repo or game folder; no package was installed. Flagging it for transparency.)

### Archive format as recovered [E unless marked]
- Whole file XORed word-wise (u32 LE) with `key_i = (A + B*i) mod 2^32`, `i` = absolute word index. A,B are per archive
  (e: A=0x51558d13 B=0x4e7337f5; d: 0xb97a10e6/0xfcd18947; a: 0xfb7d5edb/0xefac2dd5). Found without any key source:
  zero-plaintext runs make the ciphertext an arithmetic progression with step B (verified over the whole e.arc, 3 runs).
  How A,B derive from header words 1-3 was not worked out (not needed; recovered statistically) [U].
- After decryption: `u32 count, u32 name_table_bytes, u32 version` at byte 16, names from byte 28, then entry table.
  v0 (b, c, e): names NUL-terminated, entries `(offset,size)`. v1 (a, d): every index word and name byte is bitwise-NOT,
  entries `(offset, uncompressed_size, stored_size)`, and file payloads are also stored bitwise-NOT (after the XOR layer).
  File position = stored offset + 12. Stored size == uncompressed size means raw, otherwise a raw LZ4 block.
  All 4 extracted samples decompress to the exact declared size (hard assertion) and parse as sane PNG/GIM/ACM [E].
- b.arc/c.arc (webm movies, BGM.PAC 806 MB, stream pack) are irrelevant to terrain; not extracted.

## 1. File inventory (counts by extension per archive) [E]

Totals: a.arc 4816 files, 826 MB uncompressed; b 72 (1.74 GB webm); c 6 (1.19 GB: 3x .pac + 3x .tbl); d 162; e 3 (launcher PNGs).
Full listings: `D:\acz-extract\lists\{a,b,c,d,e}.txt` (not committed).

| ext (a.arc) | n | uncompressed | role (E = observed, I = inferred) |
|---|---:|---:|---|
| .bin | 2332 | 380.7 MB | misc (mission scripts, radio, packs) |
| .mtd | 39 | 200.4 MB | **terrain texture tiles + stage tile atlas (E, section 2)** |
| .mpl | 205 | 172.3 MB | aircraft models (known from earlier pass) |
| .swa | 31 | 33.1 MB | UI |
| .mmb | 70 | 18.5 MB | mission object models (per `data/mis/pack`) [I] |
| .gim | 589 | 17.7 MB | textures (incl. per-stage `_dt`, `_ref`, `_sea`, `_treeN`) |
| .pbd/.pld/.p3d/.wbd... | ~170 | ~11 MB | mostly sound banks; **not** placement despite names (`.pbd`/`.pld` under `data/sound`) |
| .mhd | 39 | 2.95 MB | **terrain heightmap chunks (E)** |
| .mti / .mtc / .mta / .mha / .mia / .mii / .mid | 39 each | small | terrain tile index / CLUT / attr / chunk map (section 2) |
| .pdl / .pml / .pmi | 39 each | 39 KB / 36 KB / 3.3 MB | **static object placements (E)** |
| .wad/.wbd/.wpd | 39 each | 0.2/0.7/0.4 MB | water-related tables [I, unexamined] |
| .efd/.efa | 87/39 | 2.6 MB/0.6 MB | effects placement [I] |
| .acm | 117 | 0.23 MB | tree models `<stage>_treeN.acm` (+ pmmdl contents) |
| .sph | 39 | 14 KB | `data/spheredata/map/sph<stage>.sph` 352 bytes [U] (sky sphere params?) |
| .cso | 59 | 0.2 MB | D3D shaders (`shader/*.cso`) |
| remaining (pmt, pvt, bfn, pmb, ...) | | | UI/briefing/weapon data |

a.arc top-level data dirs (file counts): map 1145, mis 940, caption 860, radio 275, selaircraft 263, plmodel 208, lodaircraft 193,
effect 192, sound 157, mapset 78, hud 75, dmap 71, spheredata 39, planim 38, ...

| archive | files | ext breakdown |
|---|---:|---|
| b.arc | 72 | .webm 72 (1.74 GB) |
| c.arc | 6 | .pac 3 (1.19 GB), .tbl 3 |
| d.arc | 162 | .gim 95 (21.9 MB), .png 41 (20.3 MB), .msa 7, .fhm 14, .bin 4, .swa 1 (`lang/*` overrides: no terrain) |
| e.arc | 3 | .png 3 (launcher) |

**Stage layout.** `data/map/<stage>/` exists for 39 stages: k00a, m01a, m02a, m03k/m/n, m04k/m/n, m05a, m06a, m07a, m08k/m/n, m09a,
m10k/m/n, m11a-m13a, m14k/m/n, m15a, m16k/m/n, m17a, m18a, r01a, v01a-v08a.
Each has the same file set (e.g. m01a): `.mhd .mha .mia .mid .mii .mta .mtc .mtd .mti .pdl .pmi .pml .wad .wbd .wpd .efa .efd`,
`<stage>_dt.gim`, `_ref.gim`, `_sea.gim`, `_tree1..3.{acm,gim}`, `pmmdl<stage>.bin`, `pmtex<stage>.bin`, `skytex<stage>.bin`.
Stages with k/m/n suffix are variants of one map (m03k/m/n share identical .mtd sizes) [E for sizes, I for "variants"].
No file is literally named terrain/landscape; the names are the three-letter stage-extension scheme above (`mhd` = map height data [I]).

## 2. Terrain format findings

### 2.1 Heightmap: `<stage>.mha` + `<stage>.mhd` -- REAL 3D heightfield [E]
- `.mhd` = N chunks, each **33x33 unsigned bytes (1089 B)**, row-major. N is stage specific: m01a 25, m02a 16, m07a 42, m16n 154
  (file sizes 27225, 17424, 45738, 167706 are exactly N*1089).
- `.mha` (256 B) = **16x16 byte grid of chunk indices** (values 0..N-1; the same chunk repeats). 16x16 chunks x 32 cells = **512x512 cells**,
  heightfield 513x513 with adjacent chunks sharing their edge vertex. Edge-sharing test over all adjacent chunk pairs: mean abs error
  0.0004-0.013 height steps (m01a, m02a, m07a, m16n) -> the layout is right [E]. Assembled images: `D:\acz-extract\out\*_height.png`.
  m16n renders as a recognisable coastline with islands; m07a is mountainous (heights 23..148). m01a/m02a look like a tiled repeating
  filler (m01a: 25 unique chunks arranged in a 2x2 repeating interior pattern; m02a heights 0..6, nearly flat) [E].
- **Heightmap, not a mesh.** No vertex/index buffers for terrain exist in these files [E: the only geometry files are the 33x33 grids and the small ACM props].
- **Scale/origin (cross-checked against object placements, section 3):** world X = -262144 + col*1024, world Z = -262144 + row*1024,
  world Y = byte*80. I.e. terrain spans [-262144, +262144] on X and Z (524288 units = 8 * 65536), cell = 1024 units, vertical step = 80 units.
  Evidence: placements in m07a (n=91, y>1 filter) vs terrain height at the placement's (x,z): correlation **0.9906**,
  y/h ratio median **80.09, std 2.1**; the other 7 axis flips/swaps give <=0.90. m02a (flat) corr 0.88, consistent but weakly determined. [E for m07a, I for the exact origin/orientation conventions because only one rich stage was tested.]
  Assumes +col = +X and +row = +Z (the winning orientation).
- At 65 units/m (user's cockpit scale, "feels right") a cell is 15.75 m, a height step 1.23 m, max relief 314 m, map 8.1 km square [I: the
  65 u/m applies to the cockpit; the world scale was never measured; the "16x" factor (section 3) hints models are stored at 1/16 world scale].

### 2.2 Terrain texture: `.mtd` + `.mtc` + `.mti` -- orthophoto-like tiles, decodes cleanly [E]
- `.mtc` (1024 B) = 256-entry RGBA CLUT, alpha always 0x80 (PS2 convention).
- `.mtd` header: `u32 N, u32 0x20(first section), then offsets of three more sections`; payload = **4 mip levels of N 64x64 / 32x32 / 16x16 / 8x8
  8-bit palette-index tiles** (sections of N*4096, N*1024, N*256, N*64 bytes; verified exactly for m02a: 0x38c020, 0x46f020, 0x4a7c20, end 0x4b6...).
  N = 1024 (m01a), 908 (m02a), 907 (m07a), 990 (m16n). Linear (no GS swizzle needed).
  Rendering the first 256 tiles of m07a through the CLUT as a 16x16 mosaic gives a seamless aerial-photo-looking image
  (`D:\acz-extract\out\m07a_tex0.png`) [E]; tiles are laid out so consecutive tile ids tile a 16-wide raster.
- `.mti` (u16 array: 1024 / 4352 / 6400 / 6400 entries for m01a / m02a / m07a / m16n) maps something to tile ids; **values exceed N** (m07a max 1803 > 907),
  so some ids are not plain mtd tile indices. The first 256 entries of m01a/m07a are the identity run. **The mapping from terrain cell/chunk to tile, and what ids >= N mean, are UNKNOWN [U].**
  `.mta` (256 B, 16x16, values 0..3) and `.mii` (1089 B, 33x33, all zero in m01a) are unidentified [U]. `.mia` 256 B (mostly 0) [U].
- Not examined: `_sea.gim`, `_ref.gim`, `.wad/.wbd/.wpd` (water) [U]. `<stage>_dt.gim` (detail texture): decoded and drawn behind `dtex:1`, see detail-texture.md.

### 2.3 Sky: `skytex<stage>.bin` (104,192 B) [E for container, U for use]
12-entry offset table then GIM images ("GIM\0" header, 0x20x0x20 first image, 4-8 bit indexed). Sky is texture-only data here; the dome
geometry/parameters are probably in `spheredata/map/sph<stage>.sph` (352 B) or hard-coded [U].

### 2.4 Object placement: `<stage>.pdl`, `.pml`, `.pmi`, `pmmdl<stage>.bin`, `pmtex<stage>.bin` [E]
- `.pdl`: `u32 count`, 12 pad bytes, then `count` x 16-byte records `{f32 x, f32 y, f32 z, u32 model_id}` (size = 16+16*count verified for m02a count 105 and m07a 452).
  Positions are world units in the terrain frame above (x in [-196608, 23273], z in [-261065, 261603] in m02a; objects repeat on a 32768-unit lattice in z [E]).
  **No rotation/scale in the record** (orientation, if any, is elsewhere or none) [U]. Stages m01a, m10m, m16n have an empty (4-byte) pdl: no static placements; their ground units come from `data/mis/pack` [I].
- `.pml`: u16 table (848 entries in m02a, 0xFFFF = none) mapping something to model ids [U].
- `.pmi` (131072 B in m02a/m07a): 65536 u16 = **256x256 spatial grid of placement ids** (m02a: 113 non-zero cells, max value 105 = count) -> culling/lookup grid, cell = 2048 units [I].
- `pmmdl<stage>.bin`: `u32 n; u32 offsets[n]`; entries are `ACM\0` v5 models (the same VIF-packet container as aircraft, section 'acz-arc-mpl-format-notes').
  m02a: 44 models, 2,224 vertices total, props of 26-118 vertices each (tower 44x328x45 model units; building 115x23x76). `pmtex<stage>.bin` = GIM textures (offset table, 5 in m02a).
  Using `research/acz-arc-mpl/acmscan.py`'s position scan (positions trustworthy, faces not) [E].
  If model units are 1/16 of world units (the "16x scale" in the brief) the 328-unit tower is 5248 world units = 81 m at 65 u/m, which is a plausible tower [I, not tested].
- Trees: `<stage>_tree1..3.acm/.gim` (~2 KB ACM each) - how trees are scattered (mtd/mia/mii? efd?) is UNKNOWN [U].
- Buildings/cities for stages with large city areas live either in pdl (m02a/m07a) or in mission packs; not enumerated across all 39 stages [U].

## 3. Correlation with the live camera numbers [E/I]
- Camera position fields in E6 (research/experiments/E6-2026-10-12.md:44-52) are `(-29899, 4003.6, 137560)`, aircraft altitude `y=4000`. They are within the +-262144 range and
  `y/80 = 50` is inside every stage's height-byte range, so the units are **plausibly the same world frame** [I]. Not verified: the stage of that E6 run is not recorded here;
  m01a terrain at that x,z is y=4080 (just above the camera), m07a 6800, m02a 480 -> inconclusive.
- `data/cam/*.bin` (cutscene cameras, absolute positions ~17127,1843,21435 per the handoff) are also in range: grid (272.7,276.9), m01a terrain 3680 -> camera below ground there,
  but those belong to specific cutscene stages (m02a/m07a/...) not m01a, so no conclusion [I].
- `world_to_clip = inverse(M+256)*M+64` works in the game's world units, i.e. the same units as above if the position fields above are that frame (they were recovered with the aircraft position `x,y,z` and Euler angles) [I].
- Vertical convention: Y up (heights positive up; aircraft y=4000 vs object y 0..11231) [E for pdl, I for the game's camera].
- The 65 u/m figure and the "16x" figure are NOT measured against terrain here: a calibration would need the true ground speed in units/s [U].

## 4. UNKNOWN (list)
1. How `.mti` ids (incl. ids >= N) map terrain cells/chunks to tiles; the use of `.mta`, `.mii`, `.mia`, `.mid`.
2. Per-stage detail/sea/water rendering (`_dt.gim`, `_sea.gim`, `.wad/.wbd/.wpd`).
3. Sky geometry and the 352-byte `.sph`.
4. Triangle topology and UVs of ACM props (positions OK); pdl orientation/scale; the pml table.
5. LOD/chunk streaming policy of the original renderer (the data is 513x513 cells; the game likely draws a radius around the camera; unknown which LOD tiles).
6. Whether heights between the 8-bit steps (80 units) are smoothed in-engine (probably bilinear, but not observed).
7. Archive key derivation (A,B from the header); b/c not verified (not needed).
8. Which stage id is loaded at runtime and how the game addresses these files (stage id -> `data/map/<id>/`).
9. Mission-pack ground objects (`data/mis/pack/*.mmb` etc.) and how aircraft/ground units are placed in stage coordinates.

## 5. Feasibility verdict: rendering the stage terrain ourselves from the data
**Feasible, moderate effort, with one real unknown (texture mapping).** Because terrain is a **513x513 8-bit heightfield with a 64x64x8-bit texture atlas and a CLUT**,
no PS2 geometry decoding is needed. Per stage that is 263k vertices (~520k triangles at full density; 1.1 MB of heights) or a clipmap/chunk LOD. Rough effort:
- Offline preview (height + CLUT atlas) of one stage: ~1 day once `.mti` is understood (mti reverse-engineering is the main schedule risk: perhaps 0.5-2 days; the data are small and testable).
- Runtime renderer in the existing D3D12/OpenXR layer (heightfield chunks, 2 eyes, depth under the existing cockpit/HUD): ~3-5 days after the preview; GPU cost trivial (hundreds of thousands of triangles).
- Sky/sea/clouds: not understood; a temporary procedural gradient sky is simplest. Buildings: ACM props via `pdl` (a few thousand vertices) are cheap.
Risks: (a) `.mti` semantics; (b) the world frame/scale vs the recovered camera (needs a runtime calibration: place a marker at a pdl object / compare altitude over known terrain);
(c) 8-bit height quantisation (80 units = 1.2 m steps) may look terraced at low altitude: needs smoothing; (d) the game's real renderer may add detail the data do not show (sea, water, fog, dt-detail);
(e) pre-projected terrain draws still occur in the game: our render must replace them (hide/skip those draws); (f) stage variants k/m/n; (g) stages with a tiled filler heightmap (m01a, m02a) may not be what
the game draws at the horizon [U].
How to know the loaded stage and camera pose at runtime (all [I], needs a live capture): (1) hook the engine's archive file open/read for `data/map/<id>/<id>.mhd` (the first read after mission start names the stage), or read the stage id string/index from the mission state (mapset: `data/mapset/*/mapset<id>.bin`, 864 B, not yet decoded [U]);
(2) read camera position/Euler from the PS2-side block (E6: `+0xFDD860..868`, offsets in the 64 MB block, not stable addresses) or recover from the DrawCPT/ACM VP matrix as already done; (3) transform: `terrain_local = world - (-262144, 0, -262144)`, scaled per cell. Proof that the position and the stage frame agree = the aircraft altitude over a known flat cell (runway, sea at height 0) equals the recovered camera Y minus terrain.

## 6. Minimal first milestone (offline, no game)
`research/world-render-data/terrain_preview.py` (throwaway, numpy only, no installs), input: the already-extracted `D:\acz-extract\a\data\map\m07a\` (m07a has the richest relief and has placements):
1. Assemble the 513x513 height grid from `.mha`+`.mhd` (script logic already exists in this pass as `asm.py`), vertices `(-262144+col*1024, h*80, -262144+row*1024)`.
2. Colour with the first 256 tiles' average colour from `.mtd`+`.mtc` (or the `.mtc`-derived mean) and a Lambert hillshade; paint `.pdl` objects as boxes of the ACM bounding boxes.
3. Software-rasterise (z-buffer, numpy) one perspective view from a given eye (x,y,z, yaw/pitch, ~63x38 deg, then ~110 deg) to `m07a_view.png`, and a second render from the E6 camera `(-29899, 4003.6, 137560)` yaw 1.70 to eyeball plausibility.
Done when: the PNG shows recognisable ridges at plausible altitude, the mosaic order for the `.mti` question is resolved or documented as unresolved, and the object/terrain height agreement (corr >= 0.99, section 2.1) is reproduced by the script and committed as a test. Next step after that: a matching comparison of that render against a game screenshot taken at the same recovered camera (needs the user, a live run).

## 7. Hashes of inspected samples (SHA-256 prefix, 16 hex) [E]
| file | bytes | sha256[:16] |
|---|---:|---|
| m01a.mhd | 27225 | 50198c7fcd9d3a89 |
| m01a.mha | 256 | 7729737834a60bc2 |
| m01a.mtd | 5570592 | 7090b6518a1671b5 |
| m01a.mtc | 1024 | 9a4f0e1b3ab9b275 |
| m01a.mti | 2048 | 033bcf9f1e61d3d2 |
| m02a.mhd | 17424 | e9ac597806ebeebc |
| m02a.mha | 256 | a1f681795be00a3a |
| m02a.mtd | 4939552 | 257b90159f6032c7 |
| m02a.mti | 8704 | 867edf21561b60b3 |
| m02a.pdl | 1696 | 95b3add5b9d182ce |
| m02a.pmi | 131072 | 1ee97399a1ff5c08 |
| pmmdlm02a.bin | 160192 | 9f0ec9614093f917 |
| m07a.mhd | 45738 | af5f4a947a1e3b8f |
| m07a.mha | 256 | 383f32a23d6dc8e6 |
| m07a.mtd | 4934112 | 9f08b71b795a9445 |
| m07a.mtc | 1024 | e5d36c5003b6777e |
| m07a.mti | 12800 | aff539d3e57764e2 |
| m07a.pdl | 7248 | e094e21f9a566f37 |
| m16n.mhd | 167706 | b9f2e97e2ee6f75f |
| m16n.mha | 256 | b5b263b0c29a83d0 |
| m16n.mtd | 5385632 | 7fea81afbc5f1f00 |
| m16n.mti | 12800 | 732964df5eedde3d |

Reproduce: `python -I research/world-render-data/arcread.py extract "<game>\a.arc" <outdir> data/map/m07a/` (needs numpy only).
Scope statement: all of this is offline file data. It does not show how the running game draws the stage, nor any camera hook or VR behaviour.
