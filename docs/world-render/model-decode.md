# Aircraft, cockpit and prop models: decode, bake (.zvrm) and the COCKPIT-2 runtime design

Track COCKPIT-1 (D-302). Offline, read-only on game files, numpy + stdlib, no network. Game-derived outputs live only in
`D:\acz-extract\models` (baked .zvrm) and `D:\acz-extract\out` (preview PNGs); nothing derived from game data is committed.
Tags: **[V]** verified here (by a test, a render that matches the game, or an exact numeric identity), **[I]** inference.

Code (`research/world-render-data/`):

| file | role |
|---|---|
| `acm_lib.py` | `.mpl` part table, ACM v5 header/node tree/mesh VIF decode (strips), GIM 8-bit + CSM1 CLUT |
| `model_baker.py` | GIM (8/4-bit, CT32, external CLUT), pmtex, `.zvrm` writer/reader, bake commands `mpl`, `vrcockpit`, `pmmdl`, `acm`, `info` |
| `model_raster.py` | numpy rasteriser: near-plane clipping, perspective-correct UV, alpha test, back-face cull, translucent layer |
| `model_preview.py` | renders baked `.zvrm` files (cockpit eye from the file, any yaw/pitch/roll, exterior cut box, glass on/off) |
| `png_io.py` | minimal PNG read/write (zlib) for comparisons against the game's screenshots |
| `test_model_baker.py` | synthetic tests (no game data): node tree, transforms, strip winding, CSM1, GIM, .zvrm round trip + hash refusal |

## 1. Headline findings

1. **The cockpit in the mission-02 game/XR images is the J35J Draken (`plmodel/j35j?.mpl`), not an F-16 and not `itesu`.** [V] The baked
   `j35j` cockpit (ACM part 2 + GIM part 9) rendered from its eye point reproduces the replayed cockpit in
   `research/experiments/step56-terrain-run4/xr-f417-final-upright.png`: green riveted canopy bow, the two windscreen posts, the reflector
   gunsight with its angled glass, the alpha gauge left of the sight, the radar hood, tape instruments and side gauges, same layout
   (`D:\acz-extract\out\j35jk_vs_xr_f417.png`: ours / game / 50:50). The HUD silhouette in the game window (lower right) is a cranked delta
   with no tailplane, i.e. a Draken. The residual misregistration (a few % in scale and a small vertical shift) is from the null-driver eye
   projection used in our preview (90x90 deg assumed), not from the model. `itesu` (modern MFD cockpit atlas) is not this aircraft's cockpit.
2. **Cockpit eye point = `data/mis/mpl/<type>.pvb` floats at bytes 40/44/48.** [V] `j35j.pvb` holds (0, 3.575, -36.3); the live E6 camera offset
   from the aircraft (D-295: +25.67,+3.575,-25.67 at heading -45 deg = 36.3 u ahead, 3.575 u up) is exactly that point rotated by the heading.
   So model space == the aircraft frame, origin == the E6 aircraft position, 1 model unit == 1 world unit, **no 16x scale**. Other types:
   m21b (0, 3.875, -19.0), f16c (0, 3.8, -33.2), a10a (0, 7.9, -29.2); 66 `.pvb` files exist.
3. **Units: ~5.0 world units per metre** [V within real-aircraft tolerance]: Draken length 76.7 u / 15.35 m = 5.00, span 46.8 u / 9.42 m = 4.97;
   MiG-21 5.00 / 4.85; F-16C 5.26 / 5.14 (probe/tip rails included). This matches the stage estimate (~5.1 u/m). Consequence for 6DoF: head
   translation in a model-based cockpit must use **~5 u/m**, not the `upm:39.6` used for the replayed (scaled) cockpit; at 39.6 a 10 cm lean
   would move the eye ~4 u = the whole cockpit width.
4. **The game's cockpit model only covers the forward view.** [V] Part 2 contains the panel, consoles, canopy bow and front frame; there is no
   seat, floor, stick or rear canopy (`D:\acz-extract\out\j35jk_cockpit_only_{down,back}.png`). The exterior model (part 0) supplies the
   canopy frame, glass, airframe, wings and fin; its pilot+seat and low-detail cockpit tub must be hidden. The combined bake (`vrcockpit`)
   does this; looking back/down shows the real airframe but an empty seat area (section 6).

## 2. Container and model format

### 2.1 `.mpl` (data/plmodel, a.arc)
`u32 n` (10) then `n` u32 part offsets [V]. Roles (all 36 player types share the layout; skins `e/k/l/m/n/u/v` differ ONLY in part 8) [V by hash on j35j]:

| part | content | evidence |
|---|---|---|
| 0 | exterior airframe, full detail (pilot, canopy frame/glass, low-detail cockpit tub included) | render [V] |
| 1 | landing gear legs, wheels, doors (gear-down pose) | render [V] |
| 2 | detailed cockpit (forward view only), uses GIM part 9 | render vs game XR image [V] |
| 3 | separated wing/fin pieces (damage/debris set) | render [V]; role [I] |
| 4 | `AHM` v1: same node tree format, meshes are VIF V4-32 position-only hulls per articulated part | header/records [V]; collision ("atari") [I] |
| 5 | ACM with an empty mesh list and a non-mesh table at pair 6 (shadow/collision data?) | [I] |
| 6 | low-LOD exterior (~100 tris) | render [V] |
| 7 | external stores / drop tanks | render [V] |
| 8 | exterior skin GIM 512x512 8-bit; palette alpha 6..96/128 is NOT coverage (reflection/spec mask [I]) | [V] opaque renders match |
| 9 | cockpit atlas GIM (128x128 on j35j/m21b/f16c, 512x512 on itesu); alpha IS coverage (mostly 0x80, glass texels ~0x12) | [V] |

### 2.2 ACM v5 header (80 bytes)
`'ACM\0'`, u32 5, f32 bounding radius, f32 (small; height/offset [I]), then 8 (count, offset) pairs: nodes (count, 80), mesh table
(count, offset), (1, 0), locator/attachment tables (pairs 3-5; e.g. 12 entries on the Draken exterior [I]), an alternate table (pair 6,
used by part 5), and (1, end-32). Mesh table entries are offsets relative to the 16-aligned end of the table [V].

### 2.3 Node (64 bytes) and hierarchy [V across all 205 plmodels: the child-count parse consumes every node exactly]
`u32 flags, u32 child_count, i32 mesh (-1 = locator), u32 tex_slot (r3), u32 group/locator id, i32 a, i32 anim_channel, i32 b,
f32 t[3], f32 1.0, f32 rot[3], f32 0`.
* Depth-first pre-order tree: `child_count` children follow each node.
* Vertices are node-local; `world = parent * T(t) * Rz*Ry*Rx(rot)` [V: aileron/flap/rudder hinge parts land exactly (0.000 nearest-vertex
  distance) on the wing/fin vertices with order x,y,z applied first-to-last and rot[0] ignored].
* `anim_channel >= 0` marks animated parts (control surfaces 0..7, airbrakes, gear doors, canopy). Their `rot[0]` is 0.349 (20 deg) on most
  control surfaces and is NOT a rest rotation (applying it breaks the coincidence above) -> deflection range [I]. **COCKPIT-4 correction
  (cockpit4-impl.md section 3):** stored rot[0] = rest + range; the rest is not always 0 (Draken outer elevons 180, rudder -90, upper-right airbrake
  180 [V by fit]); the baker now fits the rest per node (`acm_lib.fit_rest_angles`, `.zvrm` flag 8). The node GROUP id names the role (1/2 elevator,
  3 rudder, 5/6 aileron, 7 flaps, 0x11 LE flaps, 0xf airbrakes, 0x17 fin) [V on three types by position].
* `tex_slot` (r3) = texture index for stage props (pmmdl) [V: buildings/towers/bridges texture correctly]; always 0 in plmodels (one GIM per part).
* Locator `group` ids: 0x60/0x65/0x66/0x67xxxx ~ engine, wingtip, weapon stations; 0x41-0x4c pylons; 0x50 a cockpit/pilot point [I].
* Cockpit part groups: 0x2e/0x2f/0x30/0x31/0x32 are locator roots for front panel / left / right / left-rear / right-rear consoles (the game
  culls these to its 63x38 view [I]); mesh groups 51 and 52 are duplicate geometry with flags 0x0/0x2001 vs 0x8/0x2009 (alternate state,
  e.g. lit/unlit or damaged [I]); we show 52 and hide 51.
* `flags`: bit0 = translucent (canopy glass, gauge glass, lights) [V visually]; bit1 = articulated/hinged [V correlation]; bit3 set on most
  lit geometry, clear on flat-colour interiors (lighting off [I]); 0x2000 = glass/reflective overlay with UVs in a tiny tint corner of the
  atlas [V UVs; env-mapping I].

### 2.4 Mesh = one DMA RET tag + VIF stream of strip batches [V on all 619,679 batches of all plmodels: no other VIF command occurs]
`STCYCL cl=5 wl=1; UNPACK V2-32 @0 (nverts, 1); V3-32 @1 positions; V3-16 @2 normals (/4096, unit length [V]); V4-8 @3 colour (0x80 = 1.0,
unsigned); V2-32 @4 UV (0..1, top-left origin); FLUSH; MSCNT; STCYCL 4/4`. There are **no GIF tags**: the VU1 microprogram owns PRIM/texture.
Each batch is **one triangle strip** of `nverts` (3..~10) vertices [V: whole MiG-21/Draken/F-16 render watertight]; the first triangle (0,1,2)
is **clockwise when viewed from the side the vertex normals point to** (100% of 8,000+ triangles on three aircraft) [V]. The baker emits
CCW-front triangle lists; back-face culling is safe (cockpit renders identically with and without it) [V].

### 2.5 GIM (8-bit, 4-bit, 32-bit)
`'GIM\0'`, psm byte at 0x13 (0x13/0x1b PSMT8, 0x14 PSMT4, 0x00 PSMCT32), u16 w/h at 0x1c, pixels at 0x20 **linear (not GS-swizzled)** [V],
then optionally a 16-byte CLUT header and 256 (16) **RGBA** entries in **CSM1 order** (swap index bits 3/4) [V]. Alpha 0x80 = 1.0.
**COCKPIT-3 correction (D-311):** COCKPIT-1 read these entries as BGRA; they are R,G,B,A like every PS2 CT32 CLUT (evidence in cockpit-render-impl.md
section 10: R-B sign flip against the game's replayed cockpit on every landmark, olive trees, WORLD-2 env GIMs). Bakes now carry header flag 4.
Stage props: `pmtexXXXX.bin` = `[n, offsets]`; entry 0 = the shared CLUT as a 16x16 PSMCT32 GIM (RGBA, CSM1), entries 1..4 = mip levels,
each a container of the stage's prop textures (m02a: 5 textures 128/64/64/64/128 px); the 8-bit GIMs inside have no own palette [V].
Trees: `<stage>_treeN.acm` + `<stage>_treeN.gim` (64x128, own CLUT): crossed alpha-tested cards, ~40 u tall [V].

### 2.6 Props `pmmdlXXXX.bin`
`[n, offsets]` of ACM models (m02a: 44: houses x many variants, ruined/destroyed variants, lattice radio towers, a water tower, 4 bridge
spans, 1 locator-only model) [V by render]. `.pdl` placements reference these by index [I, world track].

## 3. `.zvrm` v1 (little endian; writer/reader `model_baker.py`, tests `test_model_baker.py`)

Header 160 bytes:

| off | field |
|---|---|
| 0 | `"ZVRM"`, u32 version=1, u32 header_bytes=160, u32 file_bytes |
| 16 | u32 part_count, texture_count, vertex_count, index_count |
| 32 | u32 parts_off, textures_off, vertices_off, indices_off, strings_off, strings_bytes, vertex_stride=32, flags (bit0 CCW-front lists, bit1 model-space rest pose, bit2 RGBA CLUT (COCKPIT-3; unflagged mpl/acm bakes have R/B swapped and are fixed at load)) |
| 64 | f32 bbox_min[3], bbox_max[3], eye[3]; u32 has_eye; u32 source_kind (1 mpl, 2 pmmdl, 3 acm) |
| 108 | SHA-256 of bytes [160, end) (the reader refuses a mismatch) |

Part record (128 B, one per source node, locators included so the tree is complete): `u32 name_off, i32 parent_part, u32 node, u32 game_flags,
u32 group, i32 anim_channel, u32 material_flags, i32 texture, u32 first_index, index_count, first_vertex, vertex_count, f32 local_to_model[16]
(row-major, column-vector convention), f32 hinge_range (raw rot[0]), u32 source_part, u32 0, u32 0`.
Material flags: 1 alpha-test (cut 0.5), 2 blend (draw after opaque, no depth write), 4 glass (game 0x2000), 8 hidden by default, 16 unlit
(game bit3 clear, [I]), 32 exterior skin (texture alpha is not coverage), 64 exterior part in a combined bake, 128 hidden cockpit duplicate
(exterior pilot/seat/tub).
Texture table: 16 B per texture (u32 w, h, format=1 RGBA8, data offset), data RGBA8 straight alpha (PS2 0x80 -> 255). Vertex (32 B):
`f32 pos[3]` (model space, rest pose), `f32 uv[2]`, `i16 nrm[3]` (snorm), `u16 part`, `u8 rgba[4]` (0x80 -> 255). Indices: u32 triangle list.
For animation, a part's vertices can be returned to its local frame with `inverse(local_to_model)` and re-posed.

## 4. Baked outputs (not committed; `D:\acz-extract\models`)

| file | content |
|---|---|
| `j35jk_vrcockpit.zvrm` | **the runtime asset**: cockpit part 2 + exterior part 0 (exterior n8 pilot/seat, n13 tub hidden), eye (0,3.575,-36.3), 5,929 tris, 1.56 MB |
| `j35jk_cockpit.zvrm`, `j35jk_exterior.zvrm`, `j35jk_gear_stores.zvrm` | parts 2 / 0 / 1+7 separately |
| `m21bk_*`, `f16ck_*` (`cockpit`, `exterior`, `vrcockpit`) | MiG-21bis and F-16C for comparison (hidden: m21b n28-31, f16c n10-11) |
| `props_m02a/pmmdlm02a_00..43.zvrm` | all m02a props (house = 00, ruined house = 03, radio tower = 05, water tower = 08, bridge = 18) |
| `m02a_tree1..3.zvrm` | m02a trees |

Commands (all `python -I research/world-render-data/...`):
```
model_baker.py vrcockpit E:\...\plmodel\j35jk.mpl E:\...\mis\mpl\j35j.pvb D:\acz-extract\models\j35jk_vrcockpit.zvrm [--hide n,.. --keep n,..]
model_baker.py mpl <x.mpl> <out> --parts 0 [--eye-pvb <t.pvb>]
model_baker.py pmmdl D:\acz-extract\a\data\map\m02a\pmmdlm02a.bin 0 D:\acz-extract\a\data\map\m02a\pmtexm02a.bin <out>
model_baker.py acm <m02a_tree1.acm> <m02a_tree1.gim> <out>
model_preview.py <out.png> <a.zvrm>[,<b.zvrm>] --cull [--ypr yaw,pitch,roll] [--eye pvb|x,y,z --look x,y,z] [--hfov 90 --vfov 90]
```
Previews (`D:\acz-extract\out`): `j35jk_cockpit_eye_fwd*.png`, `j35jk_vs_xr_f417.png`, `j35jk_cockpit_{only,ext}_{left,right,back,down}.png`,
`vr_sheet.png` (j35j/m21b/f16c combined cockpits, forward/back/down), `j35jk_exterior_{q,side}.png`, `j35jk_chase.png`,
`props_m02a_sheet.png`, `m02a_tree1.png`, `cockpit_sheet.png` (part 2 of all 36 types).

## 5. Unknowns (not decoded)
* Animation state: control surfaces/airbrakes/gear/canopy (channels known, deflection source in game memory not located), stick/throttle
  (the Draken cockpit has none; the MiG-21 throttle/periscope nodes exist), gauges/needles (no needle nodes identified in the j35j
  cockpit: gauges look texture-only [I]), the ADI.
* Glass: the 0x2000 overlay's real look (environment map? additive?); we draw it as a faint straight-alpha tint (cockpit atlas alpha ~0.14).
* Groups 51 vs 52 meaning; part 3 vs damage states; part 5 data; AHM use; locator id semantics; header f32 #2.
* Lighting: the exact PS2 lighting (VU) is unknown. COCKPIT-3: the "brighter/greener" look was the BGRA CLUT reading plus a double sRGB encode;
  in the display domain texture x vertex colour x (0.6 + 0.4 Lambert) x gain 1.0 x the stage light colour matches the game window's cockpit within ~1 level
  on average (cockpit-render-impl.md section 10).
* `.pvb` other fields (offset 36 = -14 / -3 / -13; 52 = 2; 56 = 90 (FOV?); 60 = 14/5/15; later 144.5/150 (yaw limits?) and 1.11/0.1/0.8).
* Which skin letter the player flies (`k` assumed; parts 0-7 are identical across skins, only part 8 differs).
* Other aircraft cockpits were only rendered on a contact sheet; the exterior hide heuristic was checked on three types only.

## 6. COCKPIT-2 runtime design (D3D12 consumer, 6DoF)

Goal: replace the replayed game cockpit (a 63x38 deg view, rear/side culled) with our own mesh, rendered per eye from the tracked head, while
the world layer (terrain renderer, `xr_terrain_render`) stays underneath and the HUD/markers/effects composite on top.

1. **Asset.** `%LOCALAPPDATA%\acz-vr\model\<type>.zvrm` (the `vrcockpit` bake), selected by a config string `xr_cockpit_model=<type>[;opt:val]`
   (like `xr_terrain_stage`; aircraft id cannot be read from memory today, D-295). SDK-free loader `cockpit_model.{h,cpp}` mirroring the
   `.zvrs` loader: size/offset/alignment/count checks, SHA-256, NaN refusal, texture <= 1024^2, parts' index ranges inside the buffers.
   Offline C++ test: byte-identical read of the Python-baked file (like terrain).
2. **Frames.** Cockpit space = model space (+X right, +Y up, nose -Z, units = world units). Per eye:
   `eye_model = pvb_eye + upm_model * R_anchor * (head_pos_in_anchor + eye_offset)` with **upm_model = 5.0 u/m** (option `upm:`; the
   replay-era 39.6 is wrong for this asset), `R_anchor` = the seated anchor (forward = -Z). View rotation = HMD orientation in the anchor frame.
   Projection = the OpenXR per-eye asymmetric FOV (tangents), near 0.05 u (1 cm), far 400 u (the model's extent), reversed-Z D32.
   The cockpit is rigid with the aircraft, so the aircraft attitude does NOT enter the cockpit pass; the world/terrain pass uses
   `E6 rotation * (same eye_model offset)` (its `camera_frame`), which keeps cockpit and terrain registered by construction: the world eye is
   `campos - R_e6 * pvb_eye + R_e6 * eye_model` (campos already includes `R_e6 * pvb_eye`).
3. **Pass.** One graphics PSO pair in `xr_stereo_d3d12.cpp`: VB/IB (one immutable upload, 32-byte vertices as in the file), one SRV per texture
   (2 for a vrcockpit: cockpit atlas, exterior skin; mips generated on upload), a per-eye CBV (view-proj, light dir, gain). Draw order per eye
   into a cockpit RT + depth: opaque parts (alpha-test 0.5 for cockpit-atlas parts, exterior skin alpha ignored, back-face cull on), then
   blend parts (glass) sorted back-to-front by part centroid, depth test on / write off. Parts with MF_HIDDEN are skipped (profile overrides).
   Output premultiplied RGBA = exactly the "cockpit layer" the composite already consumes (`clear_cockpit` transparent where no cockpit),
   so the existing world composite (terrain under, cockpit over, HUD mode 1 on top) needs no change. Glass alpha stays low so the terrain shows.
4. **Replace, do not mix.** With the key on, the producer's replayed cockpit draws are not used for the cockpit layer (the HUD/markers paths are
   unchanged). Key off = byte-identical to today (same rule as terrain). Any loader/GPU failure -> the replayed cockpit for the session + log
   `xr-stereo;event=cockpit-model-disabled`.
5. **Shading.** `rgb = tex.rgb * vcol.rgb * (ambient + diffuse*max(0,n.L)) * gain`, L = a fixed sun in model space initially (later: the stage
   sun rotated by `inverse(R_e6)`), in the same colour domain as the terrain (raw/255 then the usual tonemap/sRGB RTV). Unlit parts skip diffuse.
6. **Animation (phase 2).** Per part `local_to_model` and `anim_channel` are in the file; once a game-memory source for surface deflections /
   gear / canopy is found (read-only), pose with `M_model * Rx(angle)` in the part's local frame. Until then: rest pose, gear part not loaded.
7. **Exterior from outside (third-person, optional).** The same asset with `MF_COCKPIT_DUP` parts shown and the pilot present, placed at
   `campos - R_e6*pvb_eye` with `R_e6`; useful for a chase view and for the shadow/reflection later.
8. **Tests.** Offline: loader error cases, CPU reference raster of the eye view vs the Python preview (coverage mask IoU), WARP harness case
   (`case_cockpit_model`) comparing the shader to the CPU reference; key-off byte identity. Live (null driver): `-CockpitModel j35j` with
   `xr_png_once`; compare with `j35jk_vs_xr_f417.png` (game's replayed cockpit should register within a few pixels at neutral head).
9. **Profile per aircraft** (`<type>`: eye from `.pvb`, hidden exterior nodes, hidden groups, gain). j35j: hide exterior n8, n13; cockpit group 51.
