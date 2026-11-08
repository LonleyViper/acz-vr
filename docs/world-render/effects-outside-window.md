# EFFECTS-OUTSIDE: game clouds and particles beyond the game window

Headset test 17 (D-330): "clouds and other particle effects like smoke seem to render only within the original rectangle of the game screen".
Everything else in the world is drawn by us for every direction; the effects layer (producer `gpu_copy_step5_effects`, consumer composite
mode 3/4) replays the game's own allowlisted effect draws into a private image the size of the game window, composited at the window
rectangle. Tags: **[E]** established from code, stored capture data or logs (cited); **[I]** inference; **[G]** guess.

## 1. What the two allowlisted pixel shaders draw

| PS (prefix) | Shader (debug name) | VS / GS | Depth | What it draws |
| --- | --- | --- | --- | --- |
| `4caf6e2d` | `ps2AtestCA_p` [E: HASH part of blob 000099 of the E10 export] | `534f29ca` = `ps2_v` (pre-projected GS-screen quads), no GS | GREATER_EQUAL family | **the game's cloud cards** [E, below] |
| `4caf6e2d` | same | same | GREATER family | small sprites: tracer-like streaks, specks [E stored frame; identity of all members not known] |
| `d2464ed2` | `EffDrawMissileSmoke_p` [E: live dump D-352, section 8] | VS `9eff1f92` = `EffDrawMissileSmoke_v` (passes only SV_VertexID; earlier called `DrawMesh_v` by analogy) + GS `bcb04cf9326de0739e17a13b34f46d00` = `EffDrawMissileSmoke_g` (8308 bytes) [E: diagnostics.evidence.log PSO stage records, e.g. headset-test9 lines 2922-2958; debug names from the dump] | GREATER | missile smoke trail [E: J-smoke-trace.md] |

Evidence for "GREATER_EQUAL `4caf6e2d` = clouds":
- [E] step69 run1 producer PNGs (`D:\acz-evidence-archive\step69\run1-logs\png\*-effects-on-black.png`): the quiet-flight effects image holds
  only cloud masses plus one or two small specks; every such frame has exactly two allowlisted draws, both `4caf6e2d` (`png-burst-ps`).
- [E] step69 phase-observe dumps (run1/run4 `step5-producer.log`): per frame one large draw (19..99 indices = 4..20 quads) whose count changes
  slowly (89 -> 69 -> 79 -> 59 -> 49 -> 54 over minutes) on PSOs with depth GREATER_EQUAL, and one small draw (4..24 indices) whose count
  flickers frame to frame on PSOs with depth GREATER.
- [E] Stored frame 11906 (E10 RenderDoc export, list 282): ordinal 70 = `ps2_v` + `ps2AtestCA_p`, GREATER_EQUAL, 12 quads; every vertex has
  z = 0 and q = w_in = 1.3e-5..4.1e-5. Unprojected with depth = 1/q (section 3) the 12 quads are 6 pairs of one HORIZONTAL sheet (four
  corners at one altitude within +-4 units, 22,940 x 17,200 units) and one vertical card of the same width through the same centre, at
  altitudes 4,847..8,908 and 24..78 thousand units away: cloud cards. Ordinal 108 = the same shaders, GREATER, 2 quads: a dashed
  tracer-like streak 430..900 units ahead.

## 2. (A) Quick fix: `gamecloud:0|1` inside `xr_terrain_stage` (implemented)

- With our own environment clouds (`cloud:1` or `env:1`, WORLD-2) the game's cloud cards drew a second cloud layer only inside the window.
  New sub-option `gamecloud:0|1`; not given = **0 when our clouds are really drawn** (cloud bit set AND the stage has the v3 env section,
  i.e. `terrain-env ... active=yes`), else 1. Terrain key off or the terrain not loaded: nothing changes (the switch stays false).
- Producer rule (`gpu_copy_step5.h` `effects_is_game_cloud`): PS `4caf6e2d` with depth GREATER_EQUAL is IGNORED by the effects class gate
  (`class-game-cloud`, counted in `class-ignored`), so it is never replayed into the effects image; the game's own draw is untouched. The
  GREATER `4caf6e2d` sprites and the smoke `d2464ed2` stay as before.
- Switch: process-wide atomic in `effects_game_clouds.h`, written by the XR proxy after the terrain loads (log
  `xr-stereo;event=game-clouds;replay=off|on;reason=...;our-clouds=yes|no`), read by the producer per candidate (one relaxed load).
  Producer status line `continuous-hud-effects-gamecloud;replay=off;class=ps-4caf6e2d+depth-greater-equal;ignored=N` only while active.
- Byte-identical when off: the class gate takes the new branch only with the switch on; no new log line otherwise.
- Tests: `gpu_copy_step1_step5-effects-gamecloud-rules_offline` (rule table: cloud class ignored only with the switch, GREATER sprites and smoke
  kept, earlier rules win), `gpu_copy_step1_step5-effects-gamecloud_offline` (fake GPU: switch off = 3 replays as before; on = the cloud draw not
  replayed, the game's own draw still recorded, a clouds-only frame publishes no effects layer, status line), `terrain_offline` `test_gamecloud`
  (parse, default rule, bad values).
- Risk [I]: if some other effect also uses `4caf6e2d` with GREATER_EQUAL it disappears with the clouds; the stored frame and the step69 frames
  show only cloud cards in that family. Live check: effects-on-black PNG with `gamecloud:0` (no clouds, specks/smoke still present).

## 3. (B) Can the 3D position of an effect sprite be recovered? Yes, for the `ps2_v` sprite family

How the producer replays them [E, `gpu_copy_step5_hooks.inl` `step5RecordEffect`]: the replay re-issues the game's own Draw/DrawIndexed with the
game's PSO, root arguments, vertex/index buffers and viewport into the private effects target; it never reads the vertices. The vertices live in
an UPLOAD buffer the game writes on the CPU (stored frame: resource 297, 8 MiB, persistently mapped), 64-byte `ps2_v` vertices
`(x_s, y_s, z_s, q) | COLOR0 | TEXCOORD | COLOR1` [E manifest 1, `ps2_v` DXIL: `SV_Position = ((x-2048)*2/512, (y-2048)*2/-448, z*2^-24, 1) / q`].

The per-frame camera: every `DrawACM_v` record carries the frame's world -> PS2-screen matrix A at +0 (absolute world units, row vectors;
the WORLD-OBJECTS path already reads it per frame and decomposes it, `gpu_copy_step5_world.h`) [E]. Its depth column is exactly
`Z0 e3 + b A3` with b = -255.0078 and Z0 = 16,777,470 ~ 2^24 (stored frame: ratio col2/col3 = -255.00778 on all three rows) [E].

Unprojection (`fx_memory.h`):
- view depth from z: `d = Z0 / (z_s - b)` for z_s > 0. This is the depth the game's sprites are tested against (they share the scene depth with
  the ACM objects, which write `d_buf = 1/w - 255 * 2^-24`); the GS z reaches 0 at 2^24/255 = 65,793 units, the far plane [E arithmetic].
- view depth from q: `d = 1/q` when z_s = 0 and 0 < q < 1 (perspective-correct texturing). The cloud cards use this form [E stored frame].
- then P solves `P.A0 = x_s d, P.A1 = y_s d, P.A3 = d` (3 linear equations; round trip 2.5e-9 GS px in the synthetic test).

Offline proof on real data [E, `fx_memory_offline` with ACZ_E10_BLOBS]:
- clouds (q form): 12 cards come out as 6 exactly level sheets (corner altitudes equal within +-4 units over 17,200 units) plus 6 vertical
  cards of the SAME width (22,940 units, all 12 within 1%) through the same centres. A wrong depth would tilt/scale them differently.
- streak (z form): two segments on one line from 430 to 900 units ahead, world lengths 152.3 and 153.5 units (equal within 0.8%); a linear-z
  alternative gives 6,304 vs 3,340 units (ratio 0.53) and a tangle off the line (`research/world-render-data/fx_unproject_check.py`;
  `fx_capture_draws.py` lists the stored frame's draws with shader hashes, `fx_capture_sprites.py` prints every sprite vertex; read-only on
  `%USERPROFILE%\ACZero-captures\E10`; DXIL listings in D:\acz-extract\fx-out\dxil).
- camera position from A = (-14359.7, 3491.8, 123218.1), identical to manifest 2 and the WORLD-OBJECTS decomposition.

Smoke is different: PS `d2464ed2` runs with VS `DrawMesh_v` (DXIL: it only forwards SV_VertexID) plus GS `bcb04cf9...` (8,308 bytes) [E].
Its positions are therefore made on the GPU by the GS from structured buffers, like the terrain mesh GS `DrawMesh_g` does (t10 SrtData with A at
+0, t11 per-point data, DXIL in D:\acz-extract\fx-out\dxil) [I, strong: same VS, same SrtData pattern]. **What blocks smoke**: the GS bytecode
is in no stored capture (E10 has no missile; the other .rdc are compressed; acz.exe is packed), so which buffer fields hold the smoke points is
unknown. Built to unblock it: the read-only diagnostic `shader_dump.h` (section 6). **Resolved by the live dump (D-352): section 8** - the GS reads
WORLD-space trail points from its per-draw record and does the window culling itself, so the smoke can be replayed from an eye (route S2).

## 4. Particle memory design

Per game frame (producer, at the effects seam where each allowlisted candidate is already classified):
1. For `4caf6e2d` GREATER draws (and clouds only when `gamecloud:1`): read the draw's index range (R16, restart 0xFFFF) and vertex range from the
   mapped UPLOAD buffer (`step3MappedSpan_locked` + `guardedCpuRead`, as the SRT reads do), at most ~100 vertices x 64 B per draw.
2. Take the frame's A (WORLD-OBJECTS record read; fallback: rebuild A from the E6 camera and the fixed 63.2 x 38.1 deg lens).
3. `extract_sprites`: every 4-vertex strip -> one sprite: world centre, world half extents, mean COLOR0, uv box, the depth source.
4. Publish the sprites with the frame (a small fixed array on StereoFrame, e.g. 64 sprites) plus each sprite's screen box in the effects image.

Consumer (XR thread):
5. `Memory::update(t, sprites, view)`: match each sprite to a remembered particle of its class (nearest centre within
   0.75 x max half extent + 2 units: a tracer moving 15 units/frame stays one particle); unmatched sprites are born; a remembered particle
   that is NOT seen although it projects inside the game window (8 GS units margin) DIED in the game and is dropped at once (no ghosts inside the
   window); one outside the window is kept and fades linearly to 0 over `decay_s` (4 s) after its last sighting, hard limit 30 s, cap 512
   (oldest sighting evicted).
6. Appearance: when a particle is born or refreshed inside the window, copy its screen box from that frame's effects image into a tile of a
   particle atlas (e.g. 1024 x 1024 RGBA16F = 256 tiles of 64 x 64, 8 MB; one CopyTextureRegion per new/refreshed particle). This keeps the
   game's own texture, colour and dual-source coverage (coverage_v1) without touching the game's textures.
7. Draw, per eye, every particle NOT seen this frame (seen ones are already in this frame's effects image inside the window) as a
   world-anchored billboard facing the eye (`billboard()`), textured from its atlas tile, premultiplied coverage blend x `fade`, depth-tested
   against our terrain/props distance like the WORLD-3 props (the cockpit occludes them as it occludes the world). All view directions, and
   while the aircraft flies through a trail: the billboards stay at their world positions.

Cost estimate [I]: CPU per frame < 0.1 ms (a few draws x <= 100 vertices; matching 30 x 512 distances); GPU <= 512 small alpha quads per eye
(fill-bound, comparable to the props pass 0.08-0.10 ms, D-320) + a few 32 KB tile copies; memory 8 MB atlas + ~100 KB state.

Limits (say it plainly): the game only ever shows us particles inside its window. Particles that never entered the window cannot be known.
The trail behind the aircraft is exactly what leaves the window, so a memory of positions observed while they were in the window is the best
possible without the game's own particle data; it shows how the trail WAS (no further drift/expansion/fade from the game after leaving the
window; our fade is a guess). Sprites at q-form/far depth (clouds) are fine; sprites with neither a z nor a q (flat 2D overlays, z = 0 and
q = 1) are never remembered. If the smoke GS turns out to read world positions from a buffer that holds the WHOLE trail (section 3), the memory
can be fed from that buffer instead and the limit disappears for smoke (route S1), or the smoke draw can be replayed per eye with a substituted
camera like the WORLD-OBJECTS records (route S2) if the game submits off-window points (to be measured).

## 5. First implementation (this branch) and what is not built

Built (default OFF, nothing in the runtime path):
- `src/acz-vr/fx_memory.h/.cpp`: screen map from A (validated depth model), project/unproject, z/q depth, `extract_sprites`, `Memory`
  (match, died-in-view, decay, cap), `billboard`, CPU reference eye + renderer.
- `tests/acz-vr/fx_memory_offline_tests.cpp` (ctest `fx_memory_offline`, 854 checks): round trip; extraction of game-style quads in both depth
  forms; **registration: a particle observed 16.7 deg right of the game axis, the game then turns 60 deg away, the head looks 40 deg away from the
  game window: the remembered centre projects within 0.05 px of the true position (512 px, 90 deg eye), the drawn billboard is pixel-identical to
  a billboard placed by hand at the true position (0 differing pixels), it lies 228 px right of the game window's edge, fade 0.75 after 1 s**;
  died-in-view removal; a 15-unit/frame mover stays one particle; fly-through (3 puffs behind the aircraft drawn at their true positions for a head
  looking back); cap/fade; the real stored frame (above) when ACZ_E10_BLOBS points at the E10 blobs.
- option `fxmem:0|1` inside `xr_terrain_stage` (no config key added: 31/32 unchanged). Since the smoke wiring (section 8.5) `fxmem:1` turns on the
  missile-smoke capture + log (`xr-stereo;event=fx-memory;requested=1;active=capture-log-only;...`); the sprite memory of this section still has no
  runtime path.

Not built (next steps, in order): (1) producer extraction + StereoFrame sprite array behind `fxmem:1`, first with a log line only
(`fx-memory-frame;sprites=;depth-src=;centre[0..3]=`) and a live check that cloud-card centres stay fixed in the world over seconds of flight
(they are static: the strongest live registration check, requires `gamecloud:1` for that run); (2) consumer memory + atlas copy + billboard
pass (HLSL) with a WARP harness case mirroring `test_registration_40deg` against the CPU reference; (3) smoke: the GS is read, see section 8.

## 6. Shader dump diagnostic (unblocks smoke)

`shader_dump.h` + 12 lines in `d3d12_hook.cpp` PSO creation: if `%LOCALAPPDATA%\acz-vr\dump-shaders.txt` lists 8-hex PS prefixes (e.g. `d2464ed2`),
the VS/PS/GS containers of every pipeline with such a PS are written once per hash to `%LOCALAPPDATA%\acz-vr\logs\shaders\<hash>.dxbc`
(CREATE_NEW: never overwritten, 64 files per process, 1 MiB per file) and `shader-dump;ps=;vs=;gs=;files-written=` goes to diagnostics.log.
No file = one failed open per process, nothing else. Read-only: copies the creation description's bytes; the game's pipeline is untouched.
Tested in `fx_memory_offline` (trigger parsing, hash validation, budget, never-overwrite).

## 7. Test plan for the next null run (driver)

1. `-TerrainRender 'auto;env:1;look:1;props:1;trees:1'` (gamecloud defaults to 0): xr-stereo.log `event=game-clouds;replay=off;reason=default(cloud:1-active);our-clouds=yes`;
   step5-producer.log `continuous-hud-effects-gamecloud;replay=off;...;ignored=N` with N growing ~1 per frame; with `gpu_copy_step5_png_once` the
   effects-on-black PNG holds no cloud cards (specks/smoke still there). Headset later: no cloud rectangle.
2. Before launch write `d2464ed2` into `%LOCALAPPDATA%\acz-vr\dump-shaders.txt`; fire one missile; collect `logs\shaders\*.dxbc` and the
   `shader-dump` lines; then offline `dxc -dumpbin` on the GS (bcb04cf9...) to decide smoke routes S1/S2. Delete the trigger file afterwards.
3. (superseded by section 8.5) `fxmem:1` now logs `fx-memory;active=capture-log-only` and, with the effects key, `fx-smoke-frame` lines.

Not verified by any of this: live behaviour, headset appearance, the VB bytes being final at record time (the SRT reads suggest the game writes
before recording [I]), explosion identity (not seen in any stored frame).

## 8. Missile smoke from the dumped shaders (D-352 run Z1) and route S2

Source: `D:\acz-evidence-archive\step83\runZ1-logs\shaders\{9eff1f92...,bcb04cf9...,d2464ed2...}.dxbc` (DXIL containers, not in git), disassembled offline
with the Windows SDK's `dxc.exe -dumpbin` (10.0.26100). Tags as above.

### 8.1 What the three shaders do [E: DXIL]

- VS `EffDrawMissileSmoke_v` (9eff1f92): `PASSINDEX = SV_VertexID`. No vertex buffer; the live draw is non-indexed (`d2464ed2:...:vtx` in the
  step5-producer candidate table, run Z1: 4,890 draws during one missile, all replayed into the window effects image).
- GS `EffDrawMissileSmoke_g` (bcb04cf9): input **point**, output triangle strip, max 64 vertices. Bindings: t10 `StructuredBuffer<SrtData>` (1,588-byte
  element, only element 0 read), t11 `StructuredBuffer<float4>` (random table, index 0..126), cb10 and cb11 (8 bytes = 2 floats each). One invocation =
  **one trail segment** between record points `vid` and `vid+1`:
  1. each point (record +944 + 32 n: world position xyz, drift xyz, seed w) is aged: `age = sat(((n*R176.y + R176.x)*R176.z)*(1 + 0.2 rnd) + R176.w)`,
     `pos = P + age*drift`; two 8-key curves of `age*255` give alpha (+368/+400, x R160.x) and size (+304/+336, x R160.y);
  2. the segment is rejected when both ends are outside the same plane, else clipped against `-|w| <= x, y, z <= |w|` of `[pos,1] x M(+64)` (6 planes);
  3. the clipped ends are projected with `M(+0)` (`x_s = h.x/h.w` etc.: the PS2-screen form of A); alpha x (R128.x + R128.y w), near fade below R192.x;
     size = curve / w; nothing is drawn when both alphas < 1 or both sizes < 1/16;
  4. the screen segment is split into 1/2/4/8/16 puffs (screen length vs size x R160.z), every puff is a 4-vertex **diamond** (offsets (-1,0), (0,-1),
     (0,1), (1,0) x size x R144.xy), colour = int4 corner colours (+224) / 128, uv from a 32-entry table (+432) picked by the random table;
  5. `SV_Position = ((X - cb11.x)*2/cb10.x, (Y - cb11.y)*2/cb10.y, z_s * 2^-24, 1)` (corrected in 8.6: b10 is the divisor, b11 the centre; first written
     with the names swapped).
- PS `EffDrawMissileSmoke_p` (d2464ed2): `t0(s0) x COLOR`, discard alpha <= 0, `SV_Target0 = colour`, `SV_Target1 = (1,1,1, cb12.x >= alpha ? 0 : 1)`
  (the dual-source tuple the effects-dual path already handles).

So: **the smoke positions are world points in a per-draw record and the window culling happens in the GS (the +64 clip matrix)**, not in a
pre-projected vertex stream like the `ps2_v` sprites. The trail points are NOT recovered from the image; they are read.

### 8.2 Evidenced vs inferred

- [E] Record layout (offsets above; `fx_smoke.h`), the clip test, the projection form, the depth output `z_s * 2^-24` (the same depth the DrawACM_v
  objects write), diamond expansion, the dual-source PS, one point = one segment, <= 20 points per record.
- [I, strong] +0 is the frame's world -> PS2-screen matrix A (same form as the DrawACM_v A: the GS divides by h.w and maps through the same 2048/512/-448
  constants if cb10 = (2048, 2048) and cb11 = (512, -448)). Not yet compared byte-for-byte with the same frame's ACM record.
- [I] +144.x/.y = A's screen focal lengths and .z/.w their inverses (only then is a puff world-sized). Matters for an eye with another lens.
- [I] cb10/cb11 are root constants (or a root CBV) holding the screen centre/scale; cb12.x the alpha reference.
- [UNKNOWN] whether the game puts trail points OUTSIDE its window into the record (route S2 needs them; the GS's own clipping suggests it does,
  but the CPU may cull too); whether one draw = one trail (10-12 draws per frame after one missile, 1 before); explosions (not this shader).

### 8.3 Design: capture + replay from the head pose (route S2)

Producer (game render thread, at the effects class gate where each `d2464ed2` candidate is already identified):
1. **Capture** (bounded, `fxsmoke::FrameCapture`, one per ring slot, 16 draws x ~1.66 KB = 26 KB): the 1,588-byte t10 record read from the game's
   CPU-mapped UPLOAD memory exactly as the ACM/cockpit SRT records are (`step3MappedSpan_locked` + guarded read; the record VA from the draw's
   geometry-stage root SRV), the draw arguments (DrawInstanced: count <= 19 segments, 1 instance), the t10/t11 VAs, the PSO, and cb10/cb11/cb12 when
   the root layout makes them readable. `record_check` refuses anything not proven (size, instancing, segment count, non-finite, A not of the depth form).
   Over the cap / refused: counted, the game's draw untouched. Nothing else is copied: there is no vertex data, and t11/t0/s0 stay the game's.
2. **Log-only first** (`fx-smoke-frame;...` once per second while smoke is captured, `format_summary`): points inside/outside the game window and
   behind the camera (from A), whether +0 differs between draws, the +144 lens ratios, cb10/cb11/cb12. This settles the open questions above.
3. **Replay per eye**, only for frames whose world camera was set up (world::Frame valid, objects path active): `A'_e = A + D_e x S` exactly as
   `world::substitute` builds the objects' A for that eye (same head pose, HMD lens), then `fxsmoke::substitute`: +0 = A'_e (z column x the objects'
   depth scale when `xr_world_objects_depth_fix` is on), +64 = `clip_from_a(A'_e, guard 1.25)` (the eye's frustum incl. a near plane at ~1 unit, so
   the GS clips for the EYE, not for the game window), +144 rescaled only when step 2 proved the lens reading (refused when asked otherwise). The
   copies go to a new per-slot UPLOAD region (16 draws x 2 eyes x 1,588 B = 51 KB); the replay is recorded inline right after the game's draw like
   the world objects (root arguments still bound; only OM, viewport and root SRV t10 change, t10 restored after): RT = a new per-eye FX image
   (RGBA16F, coverage_v1 via the existing dual clone PSO: alpha factors ONE/INV_SRC_ALPHA), DSV = that eye's objects depth (test only: the smoke
   PSO has no depth write), so smoke is hidden behind replayed aircraft and drawn over them in front.
4. While S2 is active the smoke class is not replayed into the window effects image (a `class-smoke-s2` ignore next to `class-game-cloud`), so the
   smoke is not doubled inside the window.

Consumer: composite the per-eye FX image (premultiplied coverage) after terrain/objects and before the cockpit, with the decouple rotation-only
re-projection the objects image gets. The head pose enters through A'_e, i.e. the same pose and lens as the objects.

Limits: the trail is drawn as far as the game keeps it in the record (the particle memory of section 4 is not needed for smoke); terrain does not
occlude smoke (the eye depth holds objects only; our terrain is drawn by the consumer; same as the effects layer today); the random-table index uses
the GPU's sin, so the puff texture choice stays the game's own. Cost [I]: <= 16 x 2 extra draws of <= 19 GS invocations (<= 16 puffs each),
fill-bound like the game's own smoke.

### 8.4 Built first (offline only; the live wiring of steps 1-2 followed in 8.5)

- `src/acz-vr/fx_smoke.h` (header only): layout constants, `record_check`, `lens_fit`, `clip_from_a`, `substitute`, a CPU emulation of the GS
  (`emulate_segment` / `emulate_draw`, step by step from the DXIL; exact except the hash's sin), `FrameCapture` (bounded), `summarise` + `format_summary`.
- `tests/acz-vr/fx_smoke_offline_tests.cpp` (ctest `fx_smoke_offline`): curve maths; a 20-point trail on an arc 0..76 deg right of the game camera:
  with the game's own matrices only segments 0..7 (inside the 31.6 deg window edge) produce puffs; substituted for a head 60 deg right (100 deg eye)
  puffs come from segments the game window never shows, and every point puff lands on the eye projection of its trail point (< 1e-3 GS px) with
  depth `(2^24/w - 255.0078) 2^-24`; identical bytes and identical GS output when the eye is the game camera; the depth scale changes only z; puffs
  are world-sized only with the lens rescale (refused when +144 is not A's focal length); a trail behind the game camera (fly-through) is invisible
  to the game and drawn for a head looking back, never behind the eye; record checks; the capture cap; the summary line.
- NOT RUN: this branch was written without building (lead builds after merge); `fx_smoke_offline` needs build + ctest.

### 8.5 Steps 1-2 wired into the producer (fxmem:1 only; capture + log, NO replay)

Switch: `fxmem:1` inside `xr_terrain_stage` sets `effects_smoke_capture_flag()` (effects_game_clouds.h; the proxy writes it on every terrain load and logs
`xr-stereo;event=fx-memory;requested=1;active=capture-log-only;reason=smoke-record-capture+fx-smoke-frame-lines(no-replay,sprite-memory-not-built);
needs=gpu_copy_step5_effects+hud+continuous`). It acts only while the effects layer is armed (keys `gpu_copy_step5_effects` + `_hud` + `_continuous`).
Flag off (default): no read, no line, nothing else changes (the per-frame `fxOpen=false` store is the only executed statement).

`gpu_copy_step5_hooks.inl`:
- identity: the effects candidate identity the class gate already reads (`Step5EffId`: PS `d2464ed2` AND VS `9eff1f92` creation-hash prefixes; the GS
  hash is not tracked per pipeline, and the VS is used by the smoke pipeline only);
- `step5FxSmokeRoots_locked` (HUD seam, hook-state mutex): cb10/cb11 (GS-visible) and cb12 (PS-visible) when each is exactly one root-constants parameter
  of the bound root layout whose first two words were written since that root was bound (`fx_smoke.h find_root_constants` / `root_pair`); a root CBV, a
  table, a denied stage, a missing / ambiguous / unwritten parameter is named (`roots-why=`), never dereferenced;
- `step5FxSmokeCapture` (right after `step5EffectsConsider_locked`, step-1 mutex, the replay decision unchanged): the cap (16) is tested BEFORE any read;
  refused before reading: an indexed draw, t10 not a single resolved root SRV (table / vertex binding / trackers disagree); then
  `step3MappedSpan_locked(t10 resource, t10 VA, 1588)` (pinned, mapped UPLOAD, span inside the resource) and two SEH-guarded copies that must be
  identical (`smoke-record-torn` otherwise); then `FrameCapture::add` (`record_check`: one instance, <= 19 segments, A of the depth form, finite). Every
  refusal is counted with its reason; the game's draw, PSO, bindings and memory are never written;
- `step5FxSmokeObjectA`: +0 of the frame's first ACCEPTED DrawACM_v record (world objects key) for the `a-vs-obj` comparison;
- `step5FxSmokeAccount` (per published effects-armed frame) + `step5EmitHudEvents` (worker): one `fx-smoke-frame` line per frame with smoke draws,
  rate-limited (`LogGate`: the first 3 such frames, then <= 1 per second; `skipped=` counts the frames in between), plus an `fx-smoke-totals` line.

Line (step5-producer.log, < 480 bytes for any values, offline-tested): `gpu-copy-step1;event=fx-smoke-frame;frame=;draws=;segments=;points=;
pts-in-window=;pts-outside-window=;pts-behind=;a-differs=;lens=rx,ry,rzx,rwy(proven|unproven);roots=known|unknown;cb10=;cb11=;cb12=;overflow=;refused=;
a-vs-obj=<max abs diff>(rel=<diff / max|A|>)|none;roots-why=;last-why=;skipped=` (reasons cut at 20 characters; the totals line
`fx-smoke-totals;smoke-frames=;draws=;stored=;unread=(last=);lines=;store-cap=16;mode=capture-log-only(no-replay)` carries the full last reason).

Tests (not run here: the lead builds after merge): `fx_smoke_offline` (+ the producer sequence: refusal before read, roots reason, a-vs-obj exact / perturbed /
absent / non-finite, the cap without a read, a fresh frame; the 480-byte worst case; the rate limit; the root-constants resolution); 
`gpu_copy_step1_step5-effects-smoke-capture_offline` (the fixture's smoke-identity draw: flag off = nothing; flag on = the same draws and replays, the
draw refused before any read as `smoke-draw-indexed`, one frame line + the totals line, a non-smoke draw never captured, no allocation on the Draw path).
The fixture cannot exercise a successful mapped read of a 1,588-byte record: the first live run is the first test of that path.

Next live run (fxmem:1 + the effects keys; world objects on for `a-vs-obj`): one missile, read `fx-smoke-frame` lines:
- `draws>0` and `refused=0` (else `last-why` / `fx-smoke-totals ...unread=(last=...)` names the blocker: e.g. `not-mapped`, an upload-pin reason,
  `smoke-t10-descriptor-table`, `smoke-draw-indexed`, `smoke-a-depth-column`);
- `pts-outside-window > 0` (S2 can show the off-window trail; 0 for a trail visibly leaving the window = the CPU culls, S2 is not enough);
- `a-differs=0` and `a-vs-obj` ~ 0 (rel < 1e-6: +0 is the frame's DrawACM_v A);
- `lens=...(proven)`; `roots=known;cb10=2048,2048;cb11=512,-448` (else `roots-why`);
- the game's smoke unchanged on screen and in the effects layer (capture only), no FPS change beyond noise.
Only then build step 3 + the consumer composite, and check in the headset that the trail stays in the world when the head turns away from the game
window.

### 8.6 After run Q (D-361): field meanings, replay core, fxmem:2 dry run

Run Q answered steps 1-2: 32 lines, `refused=0`, `a-differs=0`, `a-vs-obj=0(rel=0)` (+0 IS the frame's DrawACM_v A), `roots=known`, `cb10=512,-448`,
`cb11=2048,2048`, `cb12=0.31294,0`, 13 of 32 lines with `pts-outside-window>0` (the CPU does not cull: e.g. points=206, in-window 117, outside 19, behind 70),
`lens=0.0019,0.0019,1.0000,1.0000(unproven)`. Re-read of the GS DXIL (`dxc -dumpbin`, 10.0.26100) for the open questions [E]:

- **cb10 / cb11**: `createHandle(CBV, range 0, b10)` = `%4` is the divisor, `createHandle(CBV, range 1, b11)` = `%3` is subtracted:
  `SV.xy = (X - cb11.xy) * 2 / cb10.xy`. With the live values that is the DrawACM_v / `ps2_v` mapping `(X-2048)*2/512`, `(Y-2048)*2/-448`. The emulation's
  arithmetic was right for those numbers; the names are fixed (`ScreenConsts`), and the replay REQUIRES exactly these constants (`screen_consts_check`),
  since it keeps the game's root constants and maps each eye's frustum onto the same GS window.
- **+144**: `.x`/`.y` multiply the puff size (`curve(+304/+336)/255 x +160.y / w`) into the diamond's half extents (`%539`/`%540`); `.z`/`.w` multiply the
  screen dx/dy for the subdivision length (`%469`/`%470`). Live `|.x|/fx(A) = |.y|/fy(A) = 0.0019` and `.z x .x = .w x .y = 1`: +144 = `(g fx, g fy, 1/(g fx),
  1/(g fy))` with a world-size gain g ~ 0.0019 (A's focal lengths times g, not the focal lengths). A puff's world half size is `curve x +160.y x g / 255` for any
  lens, so an eye with focal lengths (ex, ey) needs `(g ex, g ey, 1/(g ex), 1/(g ey))`: `substitute(rescale_lens)` scales by ex/fx (g cancels) and keeps the
  game's subdivision count. `lens_fit` now proves the FORM (one common gain within 2%, .z/.w the inverses within 2%, .x > 0); the line prints rx, ry with 4
  significant digits. "(unproven)" in run Q came from the old rule demanding g = 1.
- **Other fields** (all [E] DXIL): +128 alpha x (.x + .y w); +160 .x alpha-curve scale, .y size-curve scale, .z subdivision factor, .w seed offset; +176 age
  `sat(((n .y + .x) .z)(1 + 0.2 r) + .w)`; +192 near fade (if NOT .x < w: alpha x (w - .y) .z); +208 int4 not read; +224 int4[4] corner colours /128; +288
  uv base/scale; +304/+336 size curve, +368/+400 alpha curve (8 keys at age x 255); +432 uv table [32]; +944 20 x {pos.xyz, drift.xyz, seed}; +1584 int not read.
- **What clips / expands**: the segment (aged points vid+1 -> vid) is rejected when both ends are outside one plane of `-|w| <= x,y,z <= |w|` of `[p,1] M(+64)`,
  else clipped parametrically (signed `c s > w`, planes +x -x +y -y +z -z), then the ends are projected with M(+0); the diamonds are NOT clipped by the GS (SV w = 1:
  the rasteriser clips x/y at the viewport and z = z_s 2^-24 by depth clip). Puffs are dropped when both end alphas < 1 or both sizes < 1/16.
  So substituting +0 = A'_e (z column x the objects' depth scale), +64 = `clip_from_a(A'_e, 1.25)` (eye frustum + 25% guard, near ~1 unit, far 65,793) and
  rescaling +144 draws world-sized puffs at the eye's projection with the objects' depth (offline: registered < 1e-3 GS px, radius within 1e-3, depth exact).

**Built in this step** (`fx_smoke.h`, tests `fx_smoke_offline` 809 checks, all run and passing in a private build):
- `eye_a_from_delta`: A'_e = A + D_e x S element by element exactly as `world::substitute` (zero delta keeps the raw bits); `a_matches_world` (+0 vs the
  world frame's reference A, `world::record_matches` tolerance); `prepare_eye_records`: both eye copies, refusing (named) unknown roots, other screen constants,
  an unproven lens form, a missing pose, an A that is not the world frame's, a non-finite eye A, or any `record_check` failure.
- `ReplayStore`: per-slot bounded copies, 16 draws x 2 eyes x 1,600 B (16-byte aligned) = 51,200 B; over the cap refused.
- `plan_root_swap` / `replay_commands`: t10 must be one root SRV parameter; eye e binds `upload_va + offset(i, e)`, the game's VA is bound back; sequence
  `[PSO clone] {OM(eye FX RT, eye objects DSV), RootSRV(t10, eye VA), Draw} x 2, RootSRV(t10, game VA), [PSO game], OM(game targets)` (8 / 10 commands),
  checked with a fake recorder (eye draws see their own copy, the game's root argument / PSO / targets are back afterwards).
- **fxmem:2** (`xr_terrain_stage` option, no new key): capture as fxmem:1 plus a CPU DRY RUN in the producer (`step5FxSmokeDryRun`): every stored record is
  prepared for both eyes from the frame's world camera (`s5.wobj`: D_e, S, reference A; the objects' depth scale) into the `ReplayStore`, and its trail points
  are counted per eye. Lines: `fx-smoke-replay-plan;frame=;draws=;prepared=;refused=;points=;eye0-visible=;eye1-visible=;eye-only=;lens-gain=;depth-scale=;
  world=;last-why=;mode=dry-run(cpu-only,no-gpu-replay)` with each admitted `fx-smoke-frame` line, then `fx-smoke-replay-totals`. Proxy:
  `fx-memory;requested=2;active=capture-log+replay-dry-run`. Nothing is recorded on the GPU; the window effects image keeps the smoke (no `class-smoke-s2`).
  Fixture `step5-effects-smoke-capture` (d): fxmem:2 = the same commands, the plan + totals lines.

**NOT built** (needs live iteration; GPU resource lifetime is reviewed): the per-slot UPLOAD block for the copies and the two per-eye FX images (RGBA16F at the
eye-target size, created with the slot, retired by the slot fence), recording `replay_commands` after the game's smoke draw under the effects predicate (the
replay is speculative like the window effects), the `class-smoke-s2` ignore of the smoke in the window effects image while the eye replay is active, the
StereoFrame fields and the consumer composite (after terrain/objects, before the cockpit, premultiplied coverage, decouple re-projection as the objects).
Open design facts for that step: the smoke PSO's DSV format / sample count must equal the eye objects DSV (else refuse); the eye depth holds only what was
drawn BEFORE the smoke draw in the list (a cockpit member recorded later does not occlude it: compose the FX image under the cockpit layer or accept);
the eye targets must be in RENDER_TARGET / DEPTH_WRITE at the smoke draw (between members).

**Live check list for fxmem:2** (`-TerrainRender '...;fxmem:2'`, effects keys + `gpu_copy_step5_world_objects`; one missile; head turned 60-90 deg right and
then looking back while the trail is behind):
1. xr-stereo.log `event=fx-memory;requested=2;active=capture-log+replay-dry-run`.
2. `fx-smoke-frame` lines as in run Q, now `lens=0.0019,0.0019,1.0000,1.0000(proven)` (rx = ry to 4 digits).
3. `fx-smoke-replay-plan`: `prepared == draws`, `refused=0`, `world=ok`, `last-why=none` (else the named reason: `smoke-world-no-object-record` = world
   objects key off/refused, `smoke-screen-consts`, `smoke-lens-field-unproven`, `smoke-a-differs-from-world`, ...); `lens-gain` ~ 0.0019; `depth-scale` =
   1 or 0.0009766 with the depth fix.
4. With the head turned away from the window: `eye-only > 0` and growing while the trail leaves the window (the eyes see trail points the game window does
   not); looking straight ahead: `eye-only` ~ 0 and `eye0-visible ~ eye1-visible ~ pts-in-window`.
5. `fx-smoke-replay-totals;refused=0`; the game's smoke and the window effects layer unchanged; no FPS change beyond noise (CPU only, <= 16 x 2 copies).
Only then build the GPU replay above, then check in the headset: the trail stays in the world when the head turns, not doubled inside the window,
hidden behind replayed aircraft, puff size equal to the in-window smoke at the window edge.

### 8.7 GPU replay per eye (fxmem:4) and the in-window depth order (headset test 19, D-362)

Status: built and offline-tested on this branch; NOT live-run, no headset claim. Option `fxmem:4` inside `xr_terrain_stage` (fxmem:2 stays the CPU dry run,
so one session can check 2 then 4; `fxmem:3` is the draw census of section 9 and is NOT implied by 4: 4 sets the capture, dry-run and GPU flags, never effects_census_flag). Files: `src/acz-vr/fx_smoke_gpu.h` (policy + CPU mirror of the composite), `gpu_copy_step5_hooks.inl`
(`step5CreateSmokeGpu`, `step5FxSmokeGpuPlan_locked`, `step5RecordSmoke`, `step5FxSmokeGpuClose`), `stereo_frame_source.h` (`smoke[2]`, `smoke_valid`,
`smoke_format`, `smoke_draws`, `smoke_refused`), `xr_stereo_output.cpp/.h` (`BlitJob::smoke`, `use_smoke`, `kModeBitSmoke`), `xr_stereo_d3d12.cpp` (t13,
barriers), `xr_stereo_blit.hlsl` (`smoke_over`).

**Why vehicles rendered through the missile smoke (in the window)** [E: code]: the effects replay draws the smoke into the WINDOW effects image against the
shared HUD depth-stencil, which is cleared to depth 0 every frame and holds no object (`step5EffectsConsider_locked`: `call.dsv=sl.hudDsv`), so the effects
image has no occlusion by objects at all; the consumer then composites it as part of the world layer UNDER the eye image (`ps_main`:
`o + (1 - o.a) * world_layer(uv)`, `world_layer_terrain_t` -> `effects_over`), and every replayed object pixel has coverage 1 (`a = depth > 0`). So an
object always covers the smoke whatever its distance: a vehicle behind a trail is drawn over it. The depth scale was NOT the cause (the smoke's `z_s 2^-24`
and the objects' depth share the A depth model; `substitute` applies the objects' depth scale to A's z column; offline: identical eye depth per view depth,
`test_gpu_depth_order_and_composite`); the eye depth holding only what was drawn before the smoke is a limit of the fix below, not the cause.

**The fix (fxmem:4)**: each captured + prepared smoke draw (8.3-8.6) is recorded per eye into a per-slot, per-eye SMOKE image (effects format, eye size) with
the eye's objects depth-stencil bound (depth TEST only: the smoke PSO writes no depth), under the same predicate word the effects layer opened for that draw,
and is NOT replayed into the window effects image. The consumer composites the eye smoke image OVER the objects / terrain / world at the eye pixel (the
objects' decouple uv and edge fade) and before the cockpit. Smoke behind a replayed object fails the GREATER test and is absent; smoke in front covers it;
smoke outside the game window is drawn from the eye's own camera (the off-window trail of run Q). Recorded per smoke draw (12 commands + 2 clears once per
frame): [effects call clears] [ClearRTV smoke eye 0/1] SetPredication(word) SetPipelineState(dual clone) {OMSet(smoke e, eye DSV e), RootSRV(t10, eye copy e),
DrawInstanced(the game's args)} x 2, RootSRV(t10, game VA), SetPipelineState(game), SetPredication(null), OMSet(game targets); the game's own draw follows
under exactly its own state.

**Gate (fail closed, `fx_smoke_gpu.h gate`; a refusal = that draw takes the window effects replay as with fxmem:0..2, counted `window-fallback`)**: resources
ready; the effects layer recorded the draw; effects_dual (coverage_v1 clone) on; the draw was prepared this frame (the fxmem:2 dry run: world frame, roots,
lens form, A = world A); t10 one root SRV; eye depth present; **the smoke PSO's DSV format equals the eye objects DSV format** (the open point of 8.6:
`smoke-dsv-format-differs`), single sample, one RT of the smoke image format; depth GREATER / GREATER_EQUAL, no depth write; stencil off or inert (ALWAYS +
keep / no write); one viewport = the full eye image. Live D-361 facts: eye DSV format 20 (D32_FLOAT_S8X24_UINT), HUD/effects DS format 20, the smoke candidate
`d2464ed2:A:vs9eff1f92:greater:b1:dw0:vtx` (depth GREATER, blended, no depth write, non-indexed).

**Lifetime (held-slot rules; invariant text for review)**:
1. Created at most ONCE per process (`smokeTried`), only when fxmem:4 asks for it and the effects layer is ready (effects_dual on, eye depth present, effects
   size = eye size, `budget` within the caps); kept for the process lifetime like every ring resource; never released or recreated. Two moments: at step-5
   ring creation when the option is already set (owner context, the producer state not yet published), or LATE: live run R2 showed the ring is created
   (284885703) before the proxy sets the flag at VR start (284885937), so the worker tick (`step5Tick`) makes the one attempt on its first tick after the
   flag arrives. The late path holds the step-1 mutex for the whole attempt (no Draw seam, arm, close or publication can run), builds every object into
   locals, stores the slot pointers under `meta` (the consumer's acquisition reads the slots) and sets `smokeReady` LAST; a frame can only use them when it
   is ARMED after that (`f.eff.sm` is latched at the effects arm), so no recording ever references a half-built set. One-off cost: the game thread waits
   for the creation of 6 textures + 1 buffer (~178 MB) once. The line says `created=ring` or `created=late(worker,option-after-ring)`. Clearing and setting
   the option again never creates a second set.
2. Per slot: 2 colour targets (RENDER_TARGET at rest) + 2 CPU-only RTV heaps; one UPLOAD buffer with a 64 KiB span per slot (persistently mapped, 51,200 B used).
3. A slot's smoke images and UPLOAD span are written only by a recording INTO that slot, i.e. while the slot is PRODUCING: picked by `step5PickSlot_locked`
   (not HELD, not owned by another live recording, its last submitted use completed on the private fence). The CPU writes the eye copies into the span at the
   smoke draw (game thread, step-1 mutex) before the list is closed; the GPU reads them when the recording executes.
4. The consumer reads the images only between a successful published -> held exchange and `release`, after its queue waited on the slot's fence value; it
   transitions them RENDER_TARGET -> PIXEL_SHADER_RESOURCE -> RENDER_TARGET inside its own recording (the effects image contract).
5. A re-execution of the owning recording rewrites the same images from the same span (same bytes) and is quarantined like every other slot write (DECOUPLE
   review 3, J-180 owner table). `smoke_valid` is published only for a frame that cleared both images and has >= 1 replay that executes (predicate words read
   at the Close: eq0 word != 0, ne0 word == 0, off all).

**Memory**: 2560x1440 RGBA16F: 2 x 3 x (29.5 MB + 64 KiB) + 192 KiB = 177.6 MB (caps 32 MiB per image, 200 MiB total), only when fxmem:4 is set (at the ring or later).
Run Q peaked at 93.4% commit: run `mem-guard` before the live run.

**Limits**: terrain does not occlude smoke (no terrain in the eye depth; as the effects layer); the eye depth holds only what was replayed BEFORE the smoke draw
in the list (objects drawn later, and world draws lost before the frame opened, do not occlude it); the replayed cockpit band keeps its pixel over smoke (a
smoke draw recorded before a later cockpit member is not occluded by that member in the eye depth); with the model cockpit, smoke behind the GAME's cockpit
depth is absent (the objects' limit too); other effects (explosions, tracers, 4caf6e2d sprites) stay in the window effects image with the old order.

**Tests** (offline): `fx_smoke_offline` (+ budget, gate refusals, predicate commit counting, the 12-command sequence, the depth order: every point puff hidden
by a nearer object and covering a farther one with the depth fix, the composite mirror incl. cockpit band / model cockpit / sky / fade / tonemap, line
limits); `gpu_copy_step1_step5-effects-smoke-gpu_offline` (resources + ring line; an unprepared smoke draw falls back to the identical window replay with the
named refusal and lines; the recorder's exact command stream on the fake list; fxmem:2 alone runs nothing of it); `..._step5-effects-smoke-gpu-late_offline` (the R2 order: ring first, option later -> one late creation on the next tick,
`created=late(...)`, the next armed frame latches the GPU path, never a second creation); `terrain_offline` (fxmem:4 parses). NOT
testable offline: a prepared record reaching the recorder (no world frame in the effects fixture), the shader beyond an fxc compile, the real GPU.

**Live check list for fxmem:4** (`-TerrainRender '...;fxmem:4'`; keys `gpu_copy_step5_effects` + `_effects_dual` + `_hud` +
`_continuous` + `gpu_copy_step5_world_objects` (+ the depth fix as in the day config); one missile at a ground vehicle or a wingman; head turned 60-90 deg right
while the trail leaves the window, then looking back; mem-guard first):
1. xr-stereo.log `event=fx-memory;requested=4;active=capture+replay-dry-run+replay-per-eye-gpu(no-census)` (and NO `fx-census` lines).
2. step5-producer.log `fx-smoke-ring;outcome=ready;reason=none;slots=3;format=10;size=2560x1440;...;created=ring|late(worker,option-after-ring)`
   (expected live: `late`, shortly after the fx-memory line; no line at all = the flag never reached the producer; else `reason=` `needs-effects-dual`, `eye-depth-missing`, `effects-size-differs-from-eye`, `smoke-over-*`, `*-create-failed`).
3. The fxmem:2 lines as in 8.6 (`fx-smoke-frame` refused=0, `fx-smoke-replay-plan` prepared == draws, world=ok).
4. `fx-smoke-gpu;...;replays=N;committed=M;refused=0;window-fallback=0;cleared=1;valid=1;last-why=none` with N == the plan's `prepared` and M >= 1 while a trail
   is between members; any `last-why` names the gate (`smoke-dsv-format-differs` = the open point failed: report both formats; `smoke-viewport-not-image`;
   `smoke-pso-stencil`; `smoke-not-prepared` = the dry run refused, see its `last-why`); `fx-smoke-gpu-totals;...;refused=0;window-fallback=0`.
5. xr-stereo.log `summary-effects;...;smoke-eye-valid=V;smoke-eye-composited=C` with C == V > 0 (C < V: the consumer refused the images).
6. Headset: the trail stays in the world when the head turns (off-window smoke visible), it is NOT doubled inside the window, a vehicle BEHIND the smoke is
   veiled by it, a vehicle / wingman IN FRONT of the smoke covers it, the cockpit frame covers the smoke, puff size at the window edge equals the in-window
   size, no FPS change beyond noise (<= 16 x 2 extra GS draws per frame).
7. Regression: fxmem:2 / fxmem:4 / fxmem:0 runs unchanged (no `fx-smoke-ring` / `fx-smoke-gpu` lines, `smoke-eye-valid=0`).

## 9. Explosions and gun tracers (D): identification from the archive, fxmem:3 census, sprite head-pose replay core

Headset test 19 (D-362): no explosions at all in VR, no gun tracers (the old synthetic `tac:1` tracers looked terrible: not used). Goal: capture the game's
own draws and replay them from the head pose, as for the smoke (section 8).

### 9.1 What the archived logs already show [E unless tagged]

Source: the effects candidate table `continuous-hud-effects-ps-candidates` (one line per 5 s in step5-producer.log: every draw that reached the class gate,
`ps:A|-:vs:depth:b:dw:idx|vtx:seen:rep`) of every archived run under D:\acz-evidence-archive, plus the `pso|...|hash=` creation records of
diagnostics.evidence.log. Deltas per 5 s were extracted with a small stdlib script (not in git).

| PS (full hash, size) | VS | State (table) | Where it appears | rep | Reading |
| --- | --- | --- | --- | --- | --- |
| `542a11d7386a6c000682563efb79f16d` (2,948 B) | `534f29ca` ps2_v | depth GREATER, blend on, no depth write, indexed | ONLY in sessions where missiles hit: headset tests 9, 10, 11, 13, 15, 17, 19 and null runs step41/step50, in bursts of 4..180 draws per 5 s that start while missile smoke (`d2464ed2`) is present (test 19: +100 at 502 s, +98 at 507 s, +101 at 542 s, +70 at 562 s; test 13: 78/175/7/179/168 at 60-85 s). ABSENT in every run where missiles missed or only the gun fired: step83 X, P, Q (2 missiles), Z1 (1 missile, nothing hit), E10, the step69 gun runs | **always 0** (`class-ps-not-allowlisted`) | **explosion candidate** [I, strong]: never replayed into the effects image, so no explosion at all in VR. Test 19 also has exactly 1 draw/frame for ~120 s (321-441 s): a long-lived fire / burning wreck [G] |
| `7a3c93859743ef5123d130f4e1cd4a1c` (3,072 B) | `534f29ca` | depth ALWAYS, blend on, non-indexed | headset tests 9-17, never together with smoke; 8..478 draws per 5 s | 0 (class-state-not-effect) | a screen-space overlay [I], not an explosion (no depth test); sun lens flare / screen flash [G] |
| `4caf6e2d` | `534f29ca` | GREATER (sprites) / GREATER_EQUAL (clouds) | always | replayed (clouds ignored with gamecloud:0) | the GREATER count jumps at kills (test 19 at 502 s: 2,880 seen / 1,986 replayed per 5 s vs ~600 quiet): explosion debris/spark sprites [I]; the stored-frame tracer-like streak (section 3) belongs to this family: **gun tracer candidate** [I] |

So the explosion fireball draw is identified by correlation (not yet by image). It is a ps2_v pre-projected sprite like the clouds/specks, so its world
position is recoverable (section 3). Gun tracers have no pipeline of their own (D-318: no new draw while firing; tracers visible as tiny streaks in the game
window): they must be inside the GREATER 4caf6e2d sprite draw, which is already replayed into the window image (so at most visible inside the rectangle).
NOT known: the blend tuple of 542a11d7 (with `gpu_copy_step5_effects_dual` the effects path refuses anything but 5,6,1/18,19,1 as `dual-blend-unsupported`:
an additive fireball would stay refused even when allowlisted), its topology / counts / root layout, whether it is also drawn before the first cockpit
member (the table only sees the open phase), whether the 4caf6e2d GREATER count rises while the gun fires, and which draws make the flash/shockwave.

### 9.2 fxmem:3: the census (built; log only)

`fxmem:3` inside `xr_terrain_stage` (no new config key) = the smoke capture of fxmem:1 PLUS `effects_census_flag`: every non-member, non-ACM draw the effects
seam sees while the frame's effects layer is armed (after the first cockpit member) is counted per pipeline SIGNATURE (`fx_census.h`: PS/VS prefixes, RT0
blend tuple + write mask, depth func/write, stencil, topology, indexed, vertex stride, root-layout shape n/c/v/t + hash) with its class-gate verdict
(`candidate` or the ignore/refuse reason). Classes: smoke, cloud, sprite(tracer-cand) (= 4caf6e2d GREATER), explosion-cand (542a11d7), overlay-cand
(7a3c9385), hud-sprite, other. Lines (step5-producer.log, < 426 bytes for realistic values, offline-tested):
- `fx-census-onset;frame=;class=;gap=new:0|<frames>;draws=;cnt-sum=;sig=...` when a signature draws for the first time or again after > 30 frames without
  it (exact frame id: lines up with the gun/missile key times and the PNG bursts);
- once per second `fx-census-window;frame=;ms=;frames=;draws=;sigs=;active=;lines<=;overflow=;onsets=;onsets-dropped=;mode=identify-log-only(no-replay)`
  and up to 12 `fx-census;frame=;class=;sig=ps/vs;blend=on:s,d,o/sa,da,oa:m;depth=f<func>:w;sten=;topo=;idx=;vbs=;roots=known:n:c:v:t:h;frames=;draws=;
  per-frame-max=;cnt=min-max(sum:);inst-max=;gate=;total=frames/draws;first-frame=` lines (most draws first).
Bounded: 48 signatures (more counted as overflow), 16 onsets per worker pass, ~12 KB; no game memory read; flag off = one pointer store per candidate.
Wiring: `gpu_copy_step5_fxcensus.inl` (new) + one-line hooks in gpu_copy_step5_hooks.inl. Proxy line `xr-stereo;event=fx-memory;requested=3;
active=capture-log+draw-census`. Tests: `fx_census_offline` (classes, signature key, a 150-frame timeline with tracer/explosion/smoke onsets and windows,
caps, line lengths; 133 checks, private build) and `terrain_offline` (fxmem:3 accepted, fxmem:4 refused at that time; since the merge with 8.7 fxmem:4 = the GPU smoke replay is accepted; 912 checks) PASS; the 19 step5-effects/png/tac fixture scenarios of gpu_copy_step1_offline still pass with the hooks change (private cl build). Not covered: a producer fixture case (gpu_copy_step1_offline does not exercise
the census); draws before the first cockpit member (the phase observer covers that phase).

### 9.3 Replay design for the ps2_v effects (fireball, debris, tracers): CPU-expanded re-projection (core built)

The smoke route (substitute the camera in a world-space record) does not exist for ps2_v sprites: their vertices are already projected. Route
**CPU-expanded** (`fx_sprite_replay.h`, offline-tested): for each captured draw, copy the referenced vertex range (64-byte ps2_v vertices, from the game's
mapped UPLOAD VB as section 4 step 1 describes), unproject every vertex with the frame's A (z form or q form), project it with the eye's A'_e = A + D_e x S
(the same eye matrix as the world objects and the smoke: it maps the eye frustum onto the same GS window), write z' = depth_scale x (2^24/d' - 255.0078)
and q' = q d/d' (q form: z' = 0, q' = 1/d'), collapse every quad with a vertex behind the eye, refuse a draw with a depth-less (2D) vertex. The replay
re-issues the game's draw with its PSO (or the dual clone), root arguments, IB and textures but a per-eye VB copy, into the per-eye FX image of section 8.3
(tested against that eye's objects depth); while active, the class is not replayed into the window effects image. Offline: eye = game camera reproduces
the vertices (< 2e-3 GS px); a head 25 deg left / 6 deg up with a wider HMD lens lands every vertex on the eye projection of its world corner (< 0.05 GS px,
depth to float32); head turned 180 deg / a near-plane cut collapse exactly the affected quads. Limits: only what the game drew in ITS window exists (an
explosion seen once can be kept by the section-4 particle memory); quads stay cards facing the game camera. Not built: the bounded VB read (e.g. 8 draws x
256 vertices = 128 KB per frame), the per-eye VB upload + IA swap commands, the consumer composite (shared with smoke step 3).

### 9.4 LIVE PROTOCOL (driver; null driver is enough, one run of ~5 min)

Config: the day install + `;fxmem:3` appended to the terrain value (effects keys incl. `gpu_copy_step5_effects_dual`; world objects on). Before launch write
`542a11d7`, `4caf6e2d` and `7a3c9385` (one per line) into `%LOCALAPPDATA%\acz-vr\dump-shaders.txt` (D-352 mechanism; PSOs are created after launch, so the
file must exist first; delete it afterwards); offline `dxc -dumpbin` gives the debug names (an `Eff...` name = identification by name). Optional second run:
`%LOCALAPPDATA%\acz-vr\effects-ps.txt` containing `542a11d7` (allowlist add, no build) to see whether the fireball then reaches the window effects image or is
refused (`continuous-hud-effects;...;last-refusal=dual-blend-unsupported` = not the dual tuple: needs its own clone class).
Actions (run83.ps1 timing, T0 = mission start; keep the S-tap pattern to stay airborne): (1) quiet reference T0+10..25, no keys; (2) GUN: hold J 2 s at
T0+30, +40, +50 with `png-burst.now` 1.5 s after each key-down (D-318: tracers appear 1.3-3.6 s after the key) and game-window shots at the same times;
(3) EXPLOSION: when a TGT box is near the screen centre (run Q's T35 shot had FRND + 2 TGT), fire L every 3 s for 30 s (ammo 50); a kill shows as the HUD
message and as `fx-census-onset;class=explosion-cand`; repeat on the next target; PNG bursts 0.5 s and 2 s after a kill if the driver watches the log, else
every 5 s during the volley; (4) optional: let the aircraft crash (own explosion) at the end.
Read: xr-stereo.log `event=fx-memory;requested=3`; step5-producer.log `fx-census-onset` (frame ids vs key times: a class that starts only after J = tracer
pipeline; only after a kill = explosion pipeline), the `fx-census` lines of those windows (blend tuple, topology, counts, root shape, gate),
`continuous-hud-effects-ps-candidates`, `continuous-hud-effects;...;last-refusal=`, the fx-smoke lines (unchanged), effects-on-black PNGs vs game shots.
Decide: the blend tuple of 542a11d7 (dual or not); whether the GREATER 4caf6e2d `cnt` max / per-frame-max rises only while J is held (tracer quads inside the
sprite draw); any `other` signature with onsets only at kills (flash / shockwave / debris). Then: built-in allowlist entry + the CPU-expanded replay of 9.3
for the identified classes.

## 10. Explosions in the effects image and the sprite head-pose replay (E): fxmem:5

Live facts (D-370, run X2; shaders D:\acz-evidence-archive\step97\X2-shaders): PS 542a11d7 = `extra7_p`, VS 534f29ca = `ps2AtestCA_v` (the generic ps2_v
sprite VS, also used by `ps2AtestCA_p` = 4caf6e2d); census `class=explosion-cand;blend=on:5,5,1/18,19,1;depth=f5:w0;sten=1;topo=5;idx=1` ~2 s after the
missile; `seen455:rep0`, `last-refusal=dual-blend-unsupported`. 7a3c9385 = `extra34_p`, never drawn.

### 10.1 extra7_p and its blend [E: DXIL, 542a11d7...dis.txt]

`t = Sample(t0,s0,uv) x COLOR0`, `a = t.a`, discard `a < 1/255`; `SV_Target0 = (t.rgb a, 1 + a)`, `SV_Target1 = (0,0,0, a/(1+a))`. Blend RT0 = RGB SRC_ALPHA /
SRC_ALPHA / ADD, alpha SRC1_ALPHA / INV_SRC1_ALPHA / ADD (D3D12_BLEND 5 = SRC_ALPHA; 18/19 = SRC1_ALPHA / INV_SRC1_ALPHA, the ALPHA of output 1, not its
colour). So RGB' = (1+a)(a t.rgb + D): an additive glow a(1+a)t plus a (1+a) brightening of what is behind; A' = a + A/(1+a) (game alpha, unused by us).
The standard dual tuple 5,6,1/18,19,1 is over-blending; this one is light. In the coverage_v1 effects image (premultiplied RGB + coverage A, composited
`e + (1-A) world`) the glow is exact and the (1+a) on effects already in the image is kept; the only loss is `a x (1-A) x world` (the brightening of the WORLD
behind the fireball: up to ~0.2 on a 0.35 background at a = 1, offline model). Clone: RGB factors unchanged, alpha ZERO / ONE (A' = A: light adds no
coverage). Code: `gpu_copy_step5.h` `effects_explosion_blend_tuple`, `effects_clone_alpha_factors` (dual 2/6, explosion 1/2); d3d12_hook.cpp creates the
clone for either tuple under `gpu_copy_step5_effects_dual` at PSO creation (the explosion PSO appears at the first impact, maybe before the option arrives;
one extra PSO, unused unless fxmem:5).

### 10.2 fxmem:5 (built; default off; xr_terrain_stage option, no new key)

`fxmem:5` = fxmem:4 (smoke capture + dry run + GPU per-eye smoke) PLUS `effects_explosion_flag`:
- **window effects image**: the class gate admits PS 542a11d7 (only with the dual path, else `class-explosion-needs-dual`), then the usual rules (hud_decide,
  no depth/stencil write, dual_ready = its clone exists): the explosion is replayed into the window effects image, i.e. it renders at least inside the
  game-window rectangle (as the smoke did before fxmem:4). fxmem 0..4: identical class decisions (`class-ps-not-allowlisted`).
- **sprite capture + CPU dry run** (`gpu_copy_step5_fxsprite.inl`, `fx_sprite_replay.h`): class 0 = PS 542a11d7, class 1 = indexed GREATER 4caf6e2d (tracer
  candidates). The draw's R16 indices and referenced 64-byte vertices are read from the game's mapped UPLOAD memory (bounds before any read: indexed, stride
  64, <= 1536 indices, <= 512 vertices; guarded reads), then counted: vertices inside / outside the game window, z/q depth forms, quads, the per-eye
  re-projection from the frame's world camera (A of the first DrawACM_v record + D_e x S, as the smoke) with eye-visible / eye-only / behind / collapsed,
  and the quad SHAPES (10.4). Lines (1/s per class; a tracer-candidate frame whose quads start to contain streaks after >= 30 frames without is always
  logged): `fx-sprite-plan;frame=;class=explosion|sprite(tracer-cand);draws=;read=;prep=;refused=;vtx=;quads=;in-win=;out-win=;z=;q=;no-depth=;
  eye-vis=e0,e1;eye-only=;behind=;collapsed=;near=;streaks=N(rgb:r,g,b;near:;len:);last-why=;skipped=` + `fx-sprite-totals`. Proxy line
  `fx-memory;requested=5;active=4+explosion-window-effects+explosion-sprite-plan`. Nothing is recorded on the GPU for the sprites yet.
- The fx_memory.cpp maths are duplicated header-only in fx_sprite_replay.h (`game_map_from_a`, `sp_project`, `sp_unproject`, `sp_vertex_depth`; the DLL does not
  link fx_memory.cpp); `fx_census_offline` checks they equal fx_memory's.
- Census (fxmem:3): the window lines now list effect classes before HUD sprites (run S3: 14 hud-sprite signatures pushed the tracer candidates out).

Are the pre-projected explosion vertices inside the game frustum only? Not logged before; `out-win` answers it. The game submits whole quads (the window
clip happens in the rasteriser), so quads straddling the edge have vertices outside; whether the CPU culls sprites wholly outside the view is the live
question, as it was for the smoke (`out-win`, and `eye-only > 0` with the head turned away in the headset).

### 10.3 Head-pose replay of the sprites (design; CPU core built and tested)

Per captured draw and eye: the re-projected vertex copy (`reproject` + `collapse_quads`, what the dry run computes into `fxSprEye`) goes to the slot's UPLOAD
span, and the draw is recorded like the fxmem:4 smoke replay (fx_smoke_gpu.h): same predicate word as the draw's effects epoch, the clone PSO, RT = that
eye's smoke image (the per-eye FX images already exist and are composited over the eye objects as coverage_v1), DSV = the eye objects depth; instead of the
t10 swap an IASetVertexBuffers swap (eye VB view = the copy, stride 64) and the game's VB view bound back before its own draw; the class then leaves the
window effects image (no double). Needs: a larger UPLOAD span per slot (smoke uses 51,200 B of 64 KiB; one explosion draw = 2 eyes x 512 x 64 B = 64 KiB:
~256 KiB per slot for 4 draws), the VB-view restore (the setter already tracks `vb_va/vb_bytes/vb_stride`), the fxmem:4 lifetime rules for the bigger span.
Not built: needs the live facts of 10.5 steps 2-3 first, and a lifetime review. (Built as fxmem:6 after run L3: section 11; its own per-eye images, not the
smoke's, because of the composite, 11.3.)

### 10.4 Tracers: a discriminating feature

Run S3 census (step83\runS3-logs): the 4caf6e2d GREATER family has TWO signatures: indexed (idx=1, every frame, 1-3 draws, 9-24 indices = 2-5 quads) and
NON-indexed single 4-vertex quads (idx=0) with an onset every 43-56 frames (~0.8 s) for the whole run, with or without the gun: a blinking light (nav /
strobe light [G]), not tracers; the onsets S3 tied to the gun belong to it. The census has no per-quad facts, so it cannot separate tracers from specks.
Feature now logged (class 1 of fxmem:5): the quad shape in WORLD units (unprojected corners): aspect = long / short edge; a STREAK has aspect >= 3 (the
stored-frame tracer segments: 152 units long, a few wide, along the view direction); per frame: streak count, mean COLOR0 of the streaks (D-318: pale
yellow), nearest streak depth, longest streak. Expected tracer signature: streaks > 0 only while J is held (+~1.3 s), near depth of a few hundred units,
yellowish rgb. If ambiguous, add next: frame-to-frame streak motion along the aircraft axis (~80 units per 60 Hz frame at ~1000 m/s and 5 units/m).

### 10.5 Live check list (null driver; day config + `;fxmem:5`, effects_dual on, world objects on; ~5 min)

1. xr-stereo.log `event=fx-memory;requested=5;active=4+explosion-window-effects+explosion-sprite-plan`; the fxmem:4 smoke lines as in run S2.
2. Missile kill (L volley at a centred TGT): `continuous-hud-effects-ps-candidates` shows `542a11d7:...:rep>0` and no `dual-blend-unsupported` for it; an
   effects-on-black PNG / XR dump ~2 s after the impact shows the fireball inside the window rectangle (slightly dimmer over terrain than the game: 10.1).
3. `fx-sprite-plan;class=explosion`: `read == draws`, `refused=0` (else `last-why`), the z/q forms, `out-win` (> 0 = quads beyond the window), `eye-vis`.
4. Gun J holds at T0+30/40/50 after a quiet reference: `fx-sprite-plan;class=sprite(tracer-cand)`: `streaks>0` only during/after the holds, `near`, `rgb`;
   PNG bursts 1.5 s after each J.
5. Hook cost lines: no change beyond noise; `fx-sprite-totals` refused counts.
Then: the GPU sprite replay of 10.3 for the explosion (and for the tracer streaks if step 4 confirms them).

## 11. GPU per-eye replay of the sprite effects (F): fxmem:6

Status: built and offline-tested on this branch; NOT live-run, no headset claim. Option `fxmem:6` inside `xr_terrain_stage` (no new key; default off;
0..5 keep their meanings) = fxmem:5 PLUS `effects_sprite_gpu_flag` (effects_stage_flags.h; proxy line `fx-memory;requested=6;active=5+sprite-replay-per-eye-gpu`).

Live trigger (batch L run L3, step83\runL3-logs / step104l, D-382 context) [E]: an explosion logged `fx-sprite-plan;class=explosion;draws=1;refused=0;in-win=0;
out-win=8;eye-only=8;near=6779` with the candidate `542a11d7:-:vs534f29ca:greater:b1:dw0:idx:seen58:rep58`. The game DOES submit explosion sprites whose
pre-projected vertices lie outside its window; fxmem:5 replays them into the WINDOW effects image (rep58), where they fall outside the image (nothing in
VR), and the CPU plan proves every one of those 8 vertices visible to an eye. So the head-pose replay outside the window is feasible: this section builds it.

Files: `src/acz-vr/fx_sprite_gpu.h` (new: memory, gate, command sequence, eye VB views, CPU mirror of the composite, lines), `fx_sprite_replay.h`
(`plan_draw` keeps both eye copies: `keep0/keep1`), `gpu_copy_step5_fxsprite.inl` (`step5FxSpriteGpuPlan_locked`, `step5RecordSprite`,
`step5FxSpriteGpuClose`, the fx-sprite-gpu lines), `gpu_copy_step5_hooks.inl` (`step5CreateSpriteGpu`, `step5EmitSpriteRing`, slot / frame / state fields,
the Draw-seam glue, publication), `stereo_frame_source.h` (`sprite[2]`, `sprite_valid`, `sprite_format`, `sprite_draws`, `sprite_refused`),
`xr_stereo_output.cpp/.h` (`BlitJob::sprite`, `use_sprite`, `kModeBitSprite` = bit 19, `sprite-eye-valid/-composited`), `xr_stereo_d3d12.cpp` (t14, kSlots 15,
2 more barriers), `xr_stereo_blit.hlsl` (`sprite_over`, `eye_fx_over`).

### 11.1 What is replayed

Per captured sprite draw (class 0 = PS 542a11d7 extra7_p; class 1 = the indexed GREATER 4caf6e2d ps2AtestCA draws) that the effects layer ADMITTED this frame
(`eRecord`: allowlist/explosion class gate, dual clone present, game predication provably off) and whose referenced vertices the fxmem:5 plan PREPARED for
both eyes (8.3 world camera: A of the frame's first DrawACM_v record + D_e x S; no world frame / 2D vertex / read refusal = not prepared):
1. `step5FxSpriteCapture` (step-1 mutex) reads the R16 indices and the referenced 64-byte vertices from the game's mapped UPLOAD memory (unchanged bounds:
   <= 1536 indices, <= 512 vertices, stride 64), and `plan_draw` re-projects every vertex per eye (unproject with the frame's A, project with A'_e, z' with
   the objects' depth scale, q' = q d/d'; quads with a vertex behind the eye / nearer than 1 unit collapsed to a point) into `fxSprKeep[e]`.
2. `step5FxSpriteGpuPlan_locked` (same mutex hold, right after) passes the gate (11.2) and copies the two eye copies into THIS slot's sprite UPLOAD span at
   `offset(r, e) = (2r + e) x 32 KiB` (r = the frame's replay index, < 8).
3. `step5RecordSprite` (after the mutex is released, inline before the game's own draw), native vtable calls only:
   `[effects clears] [ClearRTV(sprite eye 0), ClearRTV(sprite eye 1): first replay of the frame] SetPredication(the draw's effects word)
   SetPipelineState(clone) {OMSet(sprite e, eye objects DSV e), IASetVertexBuffers(0, 1, {span + offset(r,e), n x 64, 64}),
   DrawIndexedInstanced(count, 1, start, -lo, startInstance)} x 2, IASetVertexBuffers(0, 1, the game's slot-0 view), SetPipelineState(game),
   SetPredication(null), OMSet(the game's targets)` (12 commands + 2 clears once). The game's index buffer, root arguments, textures, viewport and scissor
   stay bound: index i reads copy vertex i - lo (BaseVertexLocation -lo). The game's own draw then runs under exactly its own state.
4. The draw is NOT replayed into the window effects image (no double inside the window). A refusal at 2 = the window replay exactly as fxmem:5.

The clone: extra7_p's coverage clone keeps the colour blend (RGB SRC_ALPHA/SRC_ALPHA/ADD, light) and has alpha ZERO/ONE (10.1); 4caf6e2d uses the standard
dual clone (coverage). Depth: GREATER/GREATER_EQUAL test against the eye's own objects depth, no depth write: an explosion behind a replayed aircraft is
discarded, one in front of it is drawn.

### 11.2 Gate (fail closed; `fx_sprite_gpu.h gate`, every refusal named and counted as `window-fallback`)

`sprite-gpu-not-ready` (images / span missing), `sprite-effects-not-recorded`, `sprite-needs-effects-dual`, `sprite-not-prepared` (the plan refused or no world
frame: its own `fx-sprite-plan;last-why` names why), `sprite-vertex-count`, `sprite-gpu-draw-cap` (> 8 per frame), `sprite-not-indexed-r16`, `sprite-vb-stride`,
`sprite-instanced` (instances != 1), `sprite-vb-view` (the game's slot-0 view cannot be restored: no VA / size > 4 GiB), `sprite-eye-depth-missing`,
`sprite-dsv-format-differs` (the PSO's DSV format != the eye objects DSV, live both 20), `sprite-pso-multisample`, `sprite-rt-format-differs`,
`sprite-pso-depth-test`, `sprite-pso-depth-write`, `sprite-pso-stencil` (a real stencil test), `sprite-viewport-unknown`, `sprite-viewport-not-image`,
`sprite-scissor-not-image` (the game's scissor 0 must cover the eye image: the eye maps its frustum onto the whole GS window), `sprite-eye-view`.

### 11.3 Why a second per-eye image (not the smoke images)

The smoke images are composited by un-premultiplying by the coverage (`smoke_over`, and the window `effects_over` mode 4: colour = P / A, 0 when A = 0).
extra7_p's clone adds LIGHT with A unchanged (10.1), so over empty space a fireball is P > 0, A = 0 and the smoke composite would drop it entirely
(offline: `test_sprite_gpu` (5), "glow kept by the sprite composite, dropped by the smoke composite"). The same loss applies to the fxmem:5 WINDOW effects
image: an explosion over a part of the window with no other effect coverage is not visible there either [I, from the composite code; not yet checked on
an image]. The sprite image is therefore composited as premultiplied light + coverage: `s = tonemap(P x exposure)` (no division), `out = s + (1 - A) x under`
(exact for pure light A = 0 and for opaque A = 1; for 4caf6e2d sprites with partial coverage and Reinhard it is slightly brighter than un-premultiplying:
accepted). It runs AFTER the smoke (`eye_fx_over = sprite_over(smoke_over(...))`), with the smoke's cockpit-band rule and the objects' decouple fade.

### 11.4 Lifetime invariants (held-slot rules; text for review)

1. Created at most ONCE per process (`spriteTried`), only when fxmem:6 asks for it and the effects layer is ready (effects_dual, eye depth, effects size = eye
   size, `budget` within the caps): at step-5 ring creation when the option is already set (owner context, producer state not yet published), else LATE by
   the worker tick (`step5Tick`, step-1 mutex held for the whole attempt: no Draw seam, arm, close or publication runs meanwhile), after the smoke's own
   attempt. Every object is built into locals, the slot pointers are stored under `meta`, `spriteReady` is set LAST; a frame uses them only when ARMED
   after that (`f.eff.sp` latched at the effects arm). Never released or recreated (clearing and setting the option again creates nothing). Line
   `fx-sprite-ring;...;created=ring|late(worker,option-after-ring)`.
2. Per slot: 2 colour targets (effects format, eye size, RENDER_TARGET at rest) + 2 CPU-only RTV heaps; one UPLOAD buffer with a 512 KiB span per slot
   (persistently mapped; 8 draws x 2 eyes x 32 KiB).
3. A slot's sprite images and span are written only by a recording INTO that slot, i.e. while the slot is PRODUCING (`step5PickSlot_locked`: not HELD, not
   owned by another live recording, its last submitted use completed on the private fence). The CPU writes the eye copies into the span at the sprite draw
   (game thread, step-1 mutex) before the list is closed; the GPU reads them when the recording executes. Each replay of a frame owns its own span byte range
   `[cursor, cursor + used)` (packed since 13.1; the cursor only grows within the frame: no overwrite within a recording).
4. `fxSprKeep` (the CPU eye copies) is valid only between `step5FxSpriteCapture` and the GPU plan of the SAME draw under ONE step-1 mutex hold
   (`fxSprLastPrepared` is cleared right after the plan and at the start of every capture).
5. The consumer reads the images only between a successful published -> held exchange and `release`, after its queue waited on the slot's fence value; it
   transitions them RENDER_TARGET -> PIXEL_SHADER_RESOURCE -> RENDER_TARGET inside its own recording (the effects / smoke image contract).
6. A re-execution of the owning recording rewrites the same images from the same span bytes and is quarantined like every other slot write (DECOUPLE
   review 3, J-180 owner table). `sprite_valid` is published only for a frame that cleared both images and has >= 1 replay that executes (predicate words
   read at the Close: `FrameState::close`, the smoke's rule).
7. The game's state after the replay: slot-0 VB view = the setter-tracked view (`vb_va / vb_bytes / vb_stride`), PSO = the bound game PSO, predication off
   (the effects gate proved the game's own predication off), OM = the bind-time descriptor ring; other VB slots, IB, root arguments, viewport and scissor are
   never touched.

### 11.5 Memory, limits, tests

**Memory**: 2560x1440 RGBA16F: 2 x 3 x (29.5 MB + 64 KiB) + 1.5 MiB = 179 MB of GPU memory (caps 32 MiB per image, 200 MiB total), only with fxmem:6, in
addition to the smoke's 178 MB (fxmem:6 implies the smoke replay). CPU: 64 KB more in the producer state. Run `mem-guard` before the live run.

**Limits**: as the smoke (terrain does not occlude; the eye depth holds only what was replayed before the draw; the replayed cockpit band keeps its pixel);
only what the game submitted exists (an explosion entirely outside the game camera's view is never drawn by the game; the CPU does not cull whole quads
outside its window, as L3 shows, but how far beyond the window it keeps them is unmeasured); quads stay cards facing the GAME camera; the explosion's
brightening of the world behind it is lost (10.1); the vertex bytes are copied at the game's Draw call (a game that wrote its vertices after recording the
draw would be replayed stale [G: no evidence either way; the plan's z/q forms were sane live]); at most 8 sprite draws per frame per eye.

**Tests (offline, private build D:\b-fxexp2, run and passing)**: `fx_census_offline` (581 checks; `test_sprite_gpu`: keep copies = each eye's projection of the
world corners within 1e-4 GS px with the game's colour/uv bytes, a refused draw not prepared, budget / span / base vertex, every gate refusal, the command
order, the composite incl. the smoke-composite loss, the depth order through the copy, line lengths); `gpu_copy_step1_offline` scenario
`step5-effects-sprite-gpu` (resources + ring line at ring creation, a GREATER 4caf6e2d draw read from the fake upload memory but not prepared -> the window
replay unchanged + `sprite-not-prepared` lines, the recorder's exact 12-op stream on the fake list incl. IASetVertexBuffers / DrawIndexedInstanced and the
restores, fxmem:5 runs nothing of it) and all 209 scenarios of that binary; `xr_composite_warp_harness` (REAL backend + shader on WARP: `case_sprite`, eye 0
= baseline + light outside the cockpit band, eye 1 byte-identical, 2 more barriers; 11375 checks); `terrain_offline` (fxmem:6 parses, 7 refused);
`flight_stage_adapter_offline` (the switch table for 6); `xr_stereo_output_offline`; the DLL target builds. NOT testable offline: a PREPARED draw reaching the
recorder in the producer fixture (no world frame there), the real GPU, the game.

### 11.6 Live check list (null driver; day config with `;fxmem:6`, effects_dual on, world objects on; mem-guard first; ~5 min)

Explosions happen only when something is destroyed or hit (autopilot volley at a centred TGT, or the aircraft's own crash at the end).
1. xr-stereo.log `event=fx-memory;requested=6;active=5+sprite-replay-per-eye-gpu`.
2. step5-producer.log `fx-sprite-ring;outcome=ready;reason=none;slots=3;format=10;size=2560x1440;...;created=ring|late(worker,option-after-ring)` (expected
   live: late, right after the `fx-smoke-ring` line; else `reason=` names it). The fxmem:4 smoke lines as in run S2.
3. At a kill: `fx-sprite-plan;class=explosion;draws=N;read=N;prep=N;refused=0` (as L3, with `out-win > 0` / `eye-only > 0` when the head is turned).
4. `fx-sprite-gpu;frame=;replays=R(explosion:X,sprite:Y);committed=C;refused=0;window-fallback=0;cleared=1;valid=1;last-why=none` with X == the plan's
   `prep` of the explosion class in that frame and C >= 1; any `last-why` names the gate (e.g. `sprite-scissor-not-image`, `sprite-dsv-format-differs`:
   report it). `fx-sprite-gpu-totals;...;refused=0;window-fallback=0`. Class 1 (the 4caf6e2d GREATER sprites) appears every frame as `sprite:Y`.
5. `continuous-hud-effects-ps-candidates`: `542a11d7:...:rep` keeps counting (the effects layer admitted it; the replay went per eye).
6. xr-stereo.log `summary-effects;...;sprite-eye-valid=V;sprite-eye-composited=C` with C == V > 0 (C < V: the consumer refused the images).
7. Mirror shot (`png-screen.now` / XR dump) within ~1 s of the kill message, head turned ~30-60 deg toward where the explosion was out of the window: the
   fireball visible OUTSIDE the game-window rectangle, not doubled inside it; looking straight ahead it sits where the game draws it.
8. Regression: fxmem:5 / 4 / 0 runs show no `fx-sprite-ring` / `fx-sprite-gpu` lines and `sprite-eye-valid=0`; no FPS change beyond noise.

## 12. The gun tracer (G): per-draw log `fxsig:` and the `tracer` class of fxmem:6

### 12.1 Evidence (live batch G, step106; notes `research/experiments/step83-verify-run1/notes.md`, archive `D:\acz-evidence-archive\step106`) [E]
The gun tracers are the draw `psd34ee153/vs534f29ca`: VS = ps2AtestCA_v (the ps2_v sprite family), ADDITIVE blend `on:5,2,1` (RGB SRC_ALPHA / ONE / ADD; alpha
`1,2,1` = ZERO / ONE / ADD, write mask 15), scene depth GREATER tested WITHOUT writing it (`f5:w0`), `sten=1`, topology 5 (triangle strip). It appears 0.03-0.13 s
after J is pressed in all 6 bursts: first ONE non-indexed 4-vertex quad, while J is held an INDEXED strip of 9 / 14 / 19 / 24 indices (2-5 quads of 4 vertices
separated by 0xFFFF restarts), and a single quad again ~0.3-0.5 s after J up. The 9-index variant is also drawn by a long-running non-gun draw (500-600 draws/s).
The earlier candidate `ps4caf6e2d` does NOT follow J. The census filed d34ee153 under `hud-sprite` (it shares the HUD pixel shader) and prints at most 12 of
~25-28 signatures per window, so it never showed positions: hence 12.2.

### 12.2 `fxsig:<ps8>` (log only; TASK 1)
Sub-option of `xr_terrain_stage` (no new config key; `;fxsig:d34ee153` adds 14 characters to the option string, which the config buffer limits to 255): exactly 8 hex
digits, non-zero, case-insensitive; any other value is a parse error (`terrain_stage.cpp`). Active with every `fxmem >= 1` (fxmem:3 census, fxmem:5, fxmem:6: the
effects seam runs for all of them; the census itself stays fxmem:3 only); ignored at fxmem:0. Carried by `effects_stage_flags.h` (`stage_effects(fx, drop, sig)` ->
`effects_sig_filter()`), cleared with the other stage switches.
For every draw of that pixel-shader prefix the effects seam sees, ONE line, never subject to the census cap (12 / window), in `step5-producer.log`:
`gpu-copy-step1;event=fx-sig-draw;f=<published frame>;ord=<draw ordinal>;n=<index among the matching draws of this frame>;sig=psXXXXXXXX/vsXXXXXXXX;gate=<class-gate
verdict, pass = reached the replay rules>;idx=<count>|cnt=<count>;in=<instances>;st=<start>;bs=<base>;vb=<GPU VA>(res:<resource key>,s<stride>);ib=<index stride>;
bl=<on|off>:<blend 6 factors>:m<mask>;d=f<func>:w<depth write>;sten=;topo=;trc=<tracer PSO>;i=<first 4 indices>;v0=x,y,z,q;...v3=...;roots=<bound root parameters>`.
The line always fits the 448-byte step-1 text (426-byte body + the 21-byte prefix; the feature log's cap is 480): realistic values use ~380 bytes; the tail
(`roots=`) is what a pathological value would cut. Logging window: 3 s after the signature first appears and again after every onset following more than 30 frames
(> 30 quiet frames) without it (`SigLog::admit`, tested); a line queue of 40 absorbs bursts, a full queue drops and counts (`dropped=N` on the next line).
Vertices: the first up-to-4 vertices' RAW position float4 (ps2_v `x_s, y_s, z_s, q`: pre-projected GS coordinates, NOT world positions) from the game's pinned,
CPU-mapped UPLOAD memory through `step3MappedSpan_locked` + `guardedCpuRead` (16 bytes per vertex, any stride >= 16; indexed draws read the first 4 indices first;
bounded, fail closed). Failures replace the vertices by `vtx-why=<reason>` (`vb-unbound`, `vb-stride`, `ib-stride`, `ib-unbound`, `ib-outside-view`,
`vertex-outside-view`, `vertex-read-fault`, `map-unmap-coverage-incomplete` ...); nothing else changes (the draw is forwarded and replayed exactly as without the option).
Root slots: `roots=` lists the bound root signature's first 6 parameters as `<param>:<C|V|S|U|T><register>=<value>` (C root constants: first two words or `-` if never
written; V/S/U root views: the GPU VA; T descriptor table: its handle value). The tracer VS takes PRE-PROJECTED vertices, so no world/view matrix slot is expected;
the line shows what is bound if there is one.

### 12.3 The `tracer` class (TASK 2; fxmem:6 only, default behaviour unchanged)
* Class gate (`gpu_copy_step5.h effects_decide`): the HUD pixel shader `d34ee153` stays `class-hud-sprite` (ignored) for EVERY draw except the one that has ALL of:
  fxmem:6 (`effects_sprite_gpu_flag`), the coverage key (`dual`), VS `534f29ca`, and a PSO whose blend is EXACTLY RGB 5,2,1 / alpha 1,2,1 / mask 15
  (`PsoOutput::tracer_ready`, set at PSO creation together with the clone). Then the usual class rules apply (blended, GREATER|GREATER_EQUAL, no depth write, no
  stencil write, no UAV root, predication provably off): refusals keep their names. A PSO with the tracer tuple is NOT a clone tuple for any other pixel shader
  (`dual-blend-unsupported` as before), and without fxmem:6 nothing changes.
* Clone PSO (`effects_clone_alpha_factors`): the game's alpha factors are already ZERO / ONE (A' = A: light adds no coverage, the explosion's rule), so the private
  clone keeps the game's factors; the dual-source trick does not apply (no SRC1). RGB: SRC_ALPHA x S + D (additive light; premultiplied-light sprite image, 11.3).
* Sprite class 2 (`gpu_copy_step5_fxsprite.inl`): the draw is captured and planned per eye like class 0/1 (`fx-sprite-plan;class=tracer`, its onset lines are always
  admitted after >= 30 quiet frames), counted as `sprite:` in `fx-sprite-gpu`. Topology: the replay re-issues the game's draw under the game's IA state (the
  topology is not touched), so strips need no new draw path; the 9-24 index strips already are 4-vertex quads separated by 0xFFFF restarts, which `collapse_quads`
  (quads with a vertex behind the eye collapse to a point) and the quad statistics handle. The first bullet is a NON-INDEXED 4-vertex quad: it is read from
  `[start, start+count)` of the slot-0 view (count 4..512, multiple of 4, one instance: `sprite-nonindexed-shape`), a restart-separated quad list is synthesised for
  the plan, and the replay uses `DrawInstanced(count, 1, 0, startInstance)` from the eye copy (`GateIn::allow_nonindexed`; indexed draws still need R16).
* Per-eye re-projection: unchanged (fx_sprite_replay.h: unproject the pre-projected vertex with the frame's camera, project with the eye's A'_e, depth from z or q
  form; a draw whose vertices have neither (`z = 0` and `q` outside (0,1)) is refused `sprite-no-depth`: the first live log will tell which form the tracer uses).
* Window fallback: every gate refusal falls back to the window effects replay of the draw (additive into the effects image, correct there), counted
  `window-fallback`. The non-gun draw with the same signature (9-index variant, 500-600 draws/s) is admitted too (same state); the per-frame cap of 8 sprite draws
  protects the frame, extra ones fall back (`sprite-gpu-draw-cap`).

### 12.4 Minimum width (>= 1 eye pixel)
The tracer is thin and far: re-projected to the eye image (2560 x 1440 for the 512 x 448 GS window = 5 x 3.2 eye pixels per GS pixel) a quad ~0.2 GS px wide
covers no pixel centre and vanishes. For class 2 `plan_draw` runs `widen_thin_quads` after the collapse (`fx_sprite_replay.h`): each 4-vertex strip quad whose long
side is >= 1 eye px and whose END (a-b or c-d pair across the short side) is narrower than 1 eye px grows that end to 1 px about its centre line, along the
perpendicular of the quad's long direction; length, depth (z), q, colour and uv are untouched; specks (no side >= 1 px) and collapsed quads are left alone;
idempotent (0.5% slack). The vertex copies carry the widened quads (the plan line shows `wide=N` quads, both eyes). Maths limits: a speck remains a sub-pixel dot
(not drawn); a pair distance is 1 px along the perpendicular, i.e. up to a few % above 1 for a sheared quad; the additive colour is the game's (the extra width is a
visibility floor, not a size change of the tracer's glow). Tested: `fx_census_offline` `test_tracer_class` (150 x 0.04 unit streak 400 units ahead: 0.2 px
re-projected, 1.0 px widened).

### 12.5 Tests
`fx_census_offline_tests` (726 checks: SigLog onsets / 3 s window / uncapped, formatter lengths with extreme values, queue drop accounting, tracer tuple + clone, the
class gate for every refusal, widening cases, plan_draw + widen, the non-indexed gate), `flight_stage_adapter_offline_tests` (parser, shared switch table, clear),
the `gpu_copy_step1_offline_tests` effects scenarios (default behaviour unchanged), and `d3d12_hook.cpp` compiled with the new producer code. NOT run: any live run.

### 12.6 Live check list (null or headset driver; day config + `;fxmem:6;fxsig:d34ee153`, effects_dual on, world objects on; mem-guard first)
1. xr-stereo.log `fx-memory;requested=6`; `fx-sprite-ring;outcome=ready`.
2. A gun burst in OPEN SKY (no other effects): `fx-sig-draw` lines from 0.03-0.13 s after J down: `n=0` the quad (`cnt=4`, `topo=5`), then `idx=9/14/19/24`; read
   `v0..v3` (are `z` / `q` in the z form or the q form? the first quad's x, y: where on the GS window?) and `roots=`; `gate=pass` (else the class-gate verdict names
   what stopped it: `class-hud-sprite` = the tracer PSO tuple / VS / key is not what 12.1 says).
3. `fx-sprite-plan;class=tracer;...;prep=N;refused=0;...;wide=W` per burst frame; `fx-sprite-gpu;...;replays=R(explosion:0,sprite:S)` with S >= the tracer draws, `refused`,
   `window-fallback`, `last-why=none` (any `sprite-*` names the gate: report it).
4. EYE IMAGE (the point): mirror capture at 0.25 s after J down (`png-screen.now`): is the tracer visible in the eye image (headset mirror), in open sky, looking
   straight ahead? Then with the head turned ~30-60 deg (or the debug camera) while the burst runs: the tracers must stay where the bullets are (outside the game
   window rectangle too), not doubled inside it.
5. Regression: fxmem:5 / 4 / 3 / 0 runs show no `class=tracer` plan lines and no `sprite:` from d34ee153; `fxsig:` alone (fxmem:3) logs the same `fx-sig-draw` lines
   with `gate=class-hud-sprite`.

## 13. Headset test 2026-10-28 items 5/6 (H): why objects showed through smoke, why explosions looked like dust; packed sprite span

Sources: the user's step105 session logs still in `%LOCALAPPDATA%\acz-vr\logs` (19:21-19:38, day install step105 + `fxmem:4`; NOT archived yet: copy them to
the evidence archive before the next run overwrites them), batch G/H archives (`D:\acz-evidence-archive\step106`, `step107`), and the original-game reference
`docs/headset-tests/2026-10-28-explosion-targets-original-reference.webp`. Tags as above: [E] evidence, [I] inference.

### 13.1 Item 6: objects render through smoke
* [E: code] With the terrain renderer on (day config), the WINDOW effects image is composited inside `world_layer_terrain_t` (window rays, `effects_over`),
  i.e. UNDER every replayed object (`ps_main`: `o + (1 - o.a) * world_layer`), and it was drawn against the HUD depth (cleared, no objects): any effect left in
  the window image is covered by every object at any distance (8.7). `xr_world_window_occlude` is forced off with the terrain (`!terrain_on`), so the
  objects stay drawn over it inside the window too.
* [E: user log] What was in that image at step105: `4caf6e2d:A:vs534f29ca:greater:b1:dw0:idx:seen182672:rep131979` (the GREATER ps2AtestCA sprites: debris and
  smoke puffs at kills, burning-wreck smoke [I]) and 835 missile-smoke draws that fell back from the eye replay (`fx-smoke-gpu-totals;frames=6549;
  valid-frames=6334;replays=31157;committed=31157;refused=835;window-fallback=835;last-why=smoke-not-prepared`; the plan says
  `world=smoke-world-no-object-record`, in runs of >= 1 s, e.g. frames 8212..8272 and 30412..30472: frames with no accepted DrawACM_v record before the smoke).
  In those frames the trail is missing outside the window and lies under the objects inside it.
* [E: code + user log] NOT the cause: the eye smoke's depth model (live `depth-scale=0.0009766` = the objects' depth fix, `a-vs-obj=0`; same A'_e, and the
  same per-eye DSV / RT / record index as the objects: `step5FxSmokeGpuPlan_locked` binds `sl.dsv[e]` with `smokeRt[e]` and eye copy e); the consumer
  composites the eye smoke OVER the objects (`eye_fx_over`), `smoke-eye-composited == smoke-eye-valid` (6353). Draw order inside the eye path can only make
  smoke cover a NEARER object replayed after it (the eye depth holds the objects replayed before the smoke), never the reverse. One residual hole [I]: the
  game's replayed cockpit stays in the eye depth (>= 2^-10, nearer than any smoke) while the model cockpit hides it, so smoke is cut inside the game
  cockpit's footprint where the model is transparent (mostly under the opaque model frame; not seen on an image).
* Already moved by fxmem:6 (step108, not live): the indexed GREATER 4caf6e2d draws (class 1), the explosion (class 0) and the tracers (class 2) go to the
  per-eye sprite image (eye depth tested, composited over the objects) instead of the window image.
* [E: batch G census + code] The remaining risk, fixed on this branch: the per-frame sprite cap was 8 fixed 2 x 32 KiB pairs. The tracer class also admits
  the persistent non-gun 9-index d34ee153 strip (500-600 draws/s = 8-10 per 60 Hz frame in batch G periods), plus 1-3 class-1 draws per frame (batch H). An
  explosion drawn after them got `sprite-gpu-draw-cap` and fell back to the window image: invisible (light with A = 0, 11.3) and under the objects again.
  Now the span is PACKED (`fx_sprite_gpu.h`): each replay takes 2 x align_up(n x 64, 256) bytes at the frame cursor `f.eff.spBytes`, up to 32 draws per
  frame; the sprite/tracer classes stop at 30 draws and 384 KiB, the last 2 draws / 128 KiB are reserved for the explosion class (two 512-vertex draws).
  New refusal `sprite-gpu-span-full`. Same 512 KiB span per slot, same memory. Lifetime rule 11.4.3 now reads: each replay of a frame owns its own byte
  range `[cursor, cursor + used)`; the cursor only grows within the frame (reset with `f.eff` at the effects arm).

### 13.2 Item 5: dust instead of fireballs
* [E: user log] At step105 the fireball pipeline never drew in VR: `542a11d7:-:vs534f29ca:greater:b1:dw0:idx:seen1564:rep0` (`class-ps-not-allowlisted` below
  fxmem:5). The only kill visuals were the coverage-blended 4caf6e2d debris/smoke sprites, in the window image under the objects: a grey-brown cloud.
* [E: reference image] The game's explosion: a SMALL (about 1% of the screen width at ~4.7 km) bright, nearly WHITE core with an orange rim and a THIN white
  smoke column rising from the hit point. White-hot = saturated additive light, not a textured cloud.
* [E: DXIL 10.1] extra7_p adds light: RGB' = (1+a)(a t + D). fxmem:6 replays it per eye with the game's colour factors (premultiplied light, 11.3), so the
  core should return. Remaining differences [I, from the maths, not seen on an image]: (1) the (1+a) x D brightening of what is behind is dropped (alpha
  ZERO/ONE), so the core is less blown out than the game's: at a = 1 over a 0.35 background the game gives 2t + 0.70 (saturates to white), ours 2t + 0.35;
  (2) the effects images hold the game's DISPLAY values (D-311: replayed values = srgb_encode(game window)) and are composited as linear over decoded
  objects / terrain, so core and smoke come out paler and less saturated (orange towards tan) than the game; (3) size: the quads are the game's own cards, so
  the size matches when the vertices do (no evidence of a size error). Not changed here (needs a live image first): (1) can be made exact with a SPRITE-ONLY
  explosion clone, alpha `SrcBlendAlpha = SRC1_ALPHA, DestBlendAlpha = SRC_ALPHA, BlendOpAlpha = REV_SUBTRACT` (A' = A(1+a) - a: transmittance x (1+a)) and a
  sprite composite that accepts A < 0; it must not replace the shared clone, because the window mode-4 composite treats A < 0 as 0 and would lose smoke
  under a fireball. (2) is a domain decision for every effects image (decode in `smoke_over` / `sprite_over` when `objdom:display` is on).

### 13.3 Tests (private build D:\b-fxd, Release, this branch)
`fx_census_offline` 833 checks PASS (packed span: copy sizes, offsets, eye views incl. misaligned / overflowing cursors, a frame of 30 tracer strips + 2
full-size explosions without overlap, the draw and byte reserves, no underflow); `gpu_copy_step1_offline` scenarios `step5-effects-sprite-gpu` (ring line
`draws:32`, recorder at a packed cursor), `step5-effects-smoke-gpu`, `step5-effects-smoke-gpu-late`, `step5-effects-dual`, `step5-effects` PASS. NOT run: the
full ctest, the DLL build, any live run.

### 13.4 Live check list (fxmem:6 day config, effects_dual on, world objects on, terrain on; mem-guard first)
1. Ring line `fx-sprite-ring;...;caps=...,draws:32,vtx:512;...`.
2. While the gun fires and in the persistent-strip periods: `fx-sprite-gpu;replays=R(explosion:X,sprite:Y)` with Y > 8 possible; an explosion frame never
   shows `sprite-gpu-draw-cap` / `sprite-gpu-span-full` for the explosion (those refusals only for sprite/tracer draws, counted in `window-fallback`).
3. At a kill: `explosion:X` with X == the plan's `prep` for class explosion, `refused=0`; mirror / XR dump within 1 s: a small bright core at the hit point,
   not doubled in the window; compare with the reference image (how pale / how much less blown out: 13.2 items 1-2).
4. A vehicle or aircraft BEHIND debris smoke or a trail is veiled by it outside AND inside the window rectangle; one in front covers it.
5. Missile smoke: count `fx-smoke-replay-plan;...;world=smoke-world-no-object-record` periods; during them the trail lies under objects in the window and is
   absent outside it (13.1): note when they happen (menus, radio cut-ins, few objects).
6. Headset: any smoke hole shaped like the game's cockpit (canopy glass area) = the residual of 13.1.

## 14. The per-eye explosion's own pipeline state and the display-space sprite composite (I): 13.2 items (1) and (2)

Status: built and offline-tested on this branch; NOT live-run, no headset claim. No new key and no new sub-option: active with `fxmem:6` only (the
per-eye sprite replay), default behaviour of fxmem:0..5 and of the WINDOW effects image unchanged. Reference: headset report 2026-10-28 items 5, 13, 16
(`2026-10-28-explosion-targets-original-reference.webp`, `...-aircraft-shading-explosion-original-reference.webp`: a small white-hot core with an orange rim).

### 14.1 (1) The background brightening: a dedicated "light" PSO for the per-eye explosion only
* The game: RGB' = (1+a)(a t + D) (10.1). The shared clone (alpha ZERO/ONE, A' = A) keeps the glow a(1+a)t and the (1+a) on what is already in the image,
  but drops (1+a) x (world behind): 2t + 0.35 instead of 2t + 0.70 at a = 1 over a 0.35 background.
* `d3d12_hook.cpp` creates, next to the shared clone and only for the exact explosion tuple 5,5,1/18,19,1 (`gpu_copy_step5.h
  effects_explosion_light_factors`), a SECOND private PSO `effectsExplosionLight`: RGB factors unchanged, alpha SrcBlendAlpha = SRC1_ALPHA (o1.a = a/(1+a)),
  DestBlendAlpha = SRC_ALPHA (o0.a = 1+a), BlendOpAlpha = REV_SUBTRACT: A' = A(1+a) - a, i.e. the transmittance T = 1 - A becomes T(1+a). Same shaders, root
  signature, formats, depth/stencil; same lifetime as the clone (released with the game PSO's record). Creation failure leaves it null (no refusal: the
  replay keeps the shared clone).
* Used ONLY by `step5RecordSprite` (`Step5SpriteCall::pso`, set by `step5FxSpriteGpuPlan_locked` for class 0 when `fx_sprite_gpu.h explosion_pso_why` passes:
  the light PSO exists AND the eye sprite image is a float format (RGBA16F / RGBA32F: A < 0 must be storable)). Otherwise the shared clone is bound, named on
  the frame line `fx-sprite-gpu;...;xpso=light:L,shared:<why>` (`light-pso-missing`, `image-not-float`; `none` = every explosion of the frame used the light
  PSO or there was none). The sprite/tracer classes, the window effects replay (fxmem:5 and every fxmem:6 refusal = `window-fallback`) and the smoke keep the
  shared clone: the window mode-4 composite treats A < 0 as 0 and would lose smoke under a fireball (13.2).
* All gate refusals, the memory (no new resource but one PSO per explosion PSO), the span reserve and the 12-command sequence are unchanged.

### 14.2 (2) The colour domain: the sprite layer is composited in DISPLAY space
* The effects images hold the game's display values (D-311); the eye pipeline composites linear values (terrain/world decoded with t_srgb_to_linear) into an
  sRGB RTV. Adding display light to linear values and encoding again turned the white-orange core tan/pale.
* `xr_stereo_blit.hlsl sprite_over` (CPU mirror `fx_sprite_gpu.h sprite_over`) now composites as the game blends: `out = decode(min(1, s + T x encode(under)))`,
  `s = tonemap(P x exposure) x fade`, `T = 1 - A x fade`, A clamped to [kMinCoverage = -3, 1] (T <= 4: two overlapping full-alpha explosions); out.a =
  saturate(A + T under.a). An empty texel (P = 0, A = 0) returns `under` bit for bit. Only the sprite image (t14, mode bit 19 = fxmem:6) changes; the smoke
  image (t13) and the window effects image keep their linear composite (a separate domain decision).
* Result (offline model, `fx_census_offline test_explosion_light`, assumed orange texel t = (1, 0.55, 0.15)): our per-eye output equals the game's display value
  within 6e-8 for every a and backgrounds 0 / 0.35 / 0.7, also for two overlapping explosions and for an explosion over a coverage smoke puff in the same
  image; the previous path was off by up to 0.33. At a = 1 over 0.35 the core is (1.00, 1.00, 1.00) = white; at a = 0.3 the rim is (0.85, 0.67, 0.51) orange
  (previous path: (0.73, 0.60, 0.44), tan).

### 14.3 Limits / unverified
* The explosion texture's real colour is not decoded; the white core needs (1+a)(a t + D) >= 1 in every channel, which holds over bright sky / haze
  backgrounds and for saturated texels; over a dark background the core is orange-white as in the game.
* `under` is assumed opaque (eye composite pixels are); a premultiplied partial-alpha `under` is encoded as is (small error).
* The (1+a) brightening applies to what was composited UNDER the sprite image (objects, terrain, the eye smoke); in the game it also applies to effects drawn
  in the window image before the explosion only in draw order, which the per-eye split does not keep (the eye smoke is always under the sprite image).
* The display-space composite also applies to the class-1 / tracer sprites in the same image (correct for the game's blends, but their look changes too:
  coverage sprites over bright terrain come out slightly darker than before, the game's look).
* Tonemap 1 (Reinhard) compresses P before the display add (as before); the day config uses tonemap 0.

### 14.4 Tests (private build D:\b-exb, Release, this branch)
`fx_census_offline` 1370 checks PASS (new `test_explosion_light`: light factors only for the explosion tuple, PSO choice + refusals, exact game display value,
overlap, puff underneath, white core / orange rim vs the previous path, coverage floor, fade; the sprite composite cases of `test_sprite_gpu` updated to the
display domain incl. the bit-exact empty texel; line lengths with `xpso=`); `gpu_copy_step1_offline` all 209 scenarios PASS (`step5-effects-sprite-gpu`: the
frame line with `xpso=light:0,shared:none`, the recorder binding `Step5SpriteCall::pso` for both eye draws and restoring the game PSO); compiles
`d3d12_hook.cpp` with the light PSO creation; `xr_composite_warp_harness` 11380 checks PASS (REAL shader on WARP, `case_sprite` with A = 0 and A = -0.5:
decode(L + T encode(baseline)), eye 1 byte-identical). NOT run: full build / ctest, the DLL, the game, the headset; the light PSO was never created by a real
driver (the fake device only).

### 14.5 Live check list (fxmem:6 day config, effects_dual on, world objects on, terrain on; mem-guard first)
1. No PSO creation failure at the first impact: `fx-sprite-gpu;...;xpso=light:L,shared:none` with L == `explosion:X` in kill frames. `shared:light-pso-missing`
   = the driver refused the alpha SRC1_ALPHA/SRC_ALPHA/REV_SUBTRACT PSO (report it); `shared:image-not-float` = the effects format is not RGBA16F.
2. Debug layer / driver: no device removal or validation error at the first explosion (the light PSO uses dual-source factors in the alpha blend).
3. Mirror / XR dump within ~1 s of a kill, looking at the hit point (inside and outside the window rectangle): a small WHITE core with an ORANGE rim
   over sky and haze, matching `2026-10-28-explosion-targets-original-reference.webp`; not a tan or grey puff; the sky/terrain right behind the fireball
   visibly brightened (not a dark halo or a black square = negative A mishandled).
4. Overlapping explosions (multi-kill / wreck fires): saturate to white, no black or negative-colour pixels.
5. Debris/smoke sprites of class 1 (same image): compare with fxmem:6 step108 images: darker/more contrasted is expected (display domain), a colour shift
   (green/purple) is not.
6. Regression: fxmem:5 / 4 / 0 images unchanged (no sprite image); the window effects image still shows the fireball inside the window under fxmem:5.

## 15. Headset test 2026-10-29 items 5 and 7 (J): the non-indexed GREATER 4caf6e2d quads never left the window image

Sources: the user's session `D:\acz-evidence-archive\headset-2026-10-29-session` (day install step123, `fxmem:6`, terrain + env clouds, gamecloud default 0),
flights m02a (one) and m03k "The Round Table" (three). The user confirmed the window-only clouds in BOTH missions. Screenshot
`docs/headset-tests/2026-10-29-user-clouds-in-rectangle.webp`. Tags as above.

### 15.1 Which layer draws the window-only clouds (item 5)
* NOT the game's cloud cards: `game-clouds;replay=off` on every flight and `continuous-hud-effects-gamecloud;...;ignored=N` grows by ~1 per frame [E]: the
  GREATER_EQUAL q-form cards are dropped. NOT our cloud sheet (`te_clouds`: a world-anchored layer on every ray, no window clip) [E: code]. NOT the game's
  scene image (`xr_terrain_blend` is not in the installed config, so `t_bits` bit 1 is clear) [E: runtime.cfg]. NOT the per-eye sprites: every
  `fx-sprite-plan` line has `q=0` and they are composited in the eye image anyway.
* The layer: the WINDOW effects image (t3, `effects_over` inside `world_layer_terrain_t`, window rays only = the rectangle). What still reaches it: the
  GREATER `4caf6e2d` draws that are NOT class 1. Class 1 required an INDEXED draw, but the game also draws this family as NON-indexed 4-vertex strip quads
  [E]: `effects-replay;...;ps8=4caf6e2d;depth=greater;...;indexed=0;count=4` five times per frame from the first m02a frame (plus one indexed 9-index
  strip), the candidate table alternates `:idx:` / `:vtx:`. Counts [E]: m02a up to 100,274,890 ms: 113,737 seen - 34,642 cloud cards = 79,095 GREATER draws,
  39,049 class-1 reads; m03k window (1007.. to 1010.. x 10^5 ms): +89,517 GREATER, +~51,900 class-1 reads. About 40-50 % of the GREATER family on BOTH stages
  went to the window image only. They pass the depth test of a real world point (z form), so they are world sprites; that the rectangle cloud patch is
  among them is [I, strong] (no vertex dump of them yet: the new plan suffix below gives their world size).
### 15.2 Why vehicles render through explosions (item 7)
* The fireball (542a11d7) is fine [E]: every explosion plan line `refused=0`, `fx-sprite-totals;class=explosion;...;refused=0`, `xpso=light:N`: per eye, eye-depth
  tested, composited OVER the objects (`eye_fx_over`). It is pure light (RGB' = (1+a)(a t + D)), so even in the game it brightens, never hides, what is behind.
* What hides a vehicle in the game is the coverage smoke/debris of the kill (4caf6e2d GREATER, blend 5,6,1). The non-indexed ones were in the window image,
  which the terrain path composites UNDER every replayed object (`o + (1 - o.a) * world_layer`, 13.1): vehicles drawn through the explosion. The earlier
  "smoke is over the objects" (13.1) is true for the eye images (t13/t14) only; the window image was never depth-correct.
### 15.3 Fix (fxmem:6 only; fxmem:0..5 byte-identical)
* `gpu_copy_step5.h effects_sprite_class` (pure; `step5FxSpriteClass` calls it): with the per-eye GPU replay on, a non-indexed GREATER 4caf6e2d draw is class 1;
  `effects_sprite_nonindexed_class` (classes 1 and 2) lets the capture take it like the tracer's first-bullet quad (count 4..512, multiple of 4, one instance;
  quad list synthesised for the plan only) and the GPU gate (`allow_nonindexed`) replay it with `DrawInstanced` from the eye copies. It then leaves the
  window image (no double), is re-projected per eye, depth-tested against the eye objects (a nearer vehicle occludes it) and composited over the objects
  (it covers a farther vehicle), in every view direction. GREATER_EQUAL cards, smoke and explosion classes unchanged.
* Plan line: `;ni=N;qm=L` appended only when a non-indexed draw was captured (N draws, L = longest world quad edge in units: cloud puffs should be
  thousands, debris tens). Indexed-only lines are unchanged.
* Residual: draws refused by the plan/gate (`window-fallback`, e.g. no object record in the frame, `sprite-gpu-draw-cap` under heavy tracer load) still
  go to the window image (rectangle-only, under objects). The cap now has to absorb ~5 more draws per frame: watch `sprite-gpu-draw-cap`.
### 15.4 Tests (private build D:\b-fx3, Release)
`fx_census_offline` 1393 checks PASS (class table both modes, suffix byte-identity, line cap 424/426); `gpu_copy_step1_offline` `step5-effects-sprite-gpu`
(a non-indexed GREATER 4caf6e2d `DrawInstanced` is captured as class 1, counted non-indexed, falls back named without a world record) and all 70
`step5-effects*/world*/hud*` scenarios PASS; `xr_composite_warp_harness` 11428 checks PASS (new: opaque A = 1 sprite texel replaces the pixel under it).
NOT run: full build/ctest, DLL, game, headset.
### 15.5 Live check list (day config, fxmem:6; mem-guard first; m02a AND m03k)
1. `fx-sprite-plan;class=sprite(tracer-cand);...;ni=N;qm=L` lines appear from the first frames; `fx-sprite-totals` class-1 `read` now tracks the GREATER
   4caf6e2d count minus the cloud cards (gap of 15.1 closed).
2. `fx-sprite-gpu`: `sprite:` per frame up by ~5; `window-fallback` rate not higher than before; no `sprite-gpu-draw-cap` in quiet flight.
3. Look 60-90 deg off the nose with clouds in view (m02a SE, m03k): cloud puffs outside the rectangle at stable world positions; no straight-edged patch at
   the window border. Pause menu: no bright patch inside the darkened HUD quad beyond what the world shows.
4. Kill a ground vehicle with the gun/missile, look at it: the smoke/debris hides the wreck behind it; a vehicle in FRONT of the smoke covers it.
5. Performance: summary-timing unchanged within noise (a few small extra draws per eye).

## 16. step124 null run C1 (m03k): the cloud that ends in a straight vertical edge at the window border (2026-10-29, offline)

Sources: `D:\acz-evidence-archive\step124\runC1` (day config, fxmem:6): `sheetC1cut.png` (left-eye crops m0134-m0139, wall 22:35:12-17), `gameshots\g0135-223516.668-F.png`
(game window, same second), `logs\step5-producer.log` (log ms 106805156-106813406 = the same seconds). Tags: [E] established, [I] inference.

### 16.1 Admission gap: none provable [E]
* `continuous-hud-effects-ps-candidates` at 106813406: `4caf6e2d ... greater ... idx seen53152 rep41083`; `continuous-hud-effects-gamecloud ignored=12069`. seen - rep = 12069 = ignored, exactly. So EVERY
  GREATER 4caf6e2d draw seen was replayed; the only ones not replayed are the one GREATER_EQUAL cloud card per frame (deliberately ignored, `game-clouds;replay=off`).
* Plan lines for the seconds (class=sprite): `draws=3..4;read=prep;refused=0;last-why=none`, `eye-vis` = all vertices; over all 203 class=sprite plan lines `last-why=none`.
  `fx-sprite-gpu`: `committed==replays`, `window-fallback=0` per frame (totals: 18 fallbacks in 42994 replays, reason `sprite-not-prepared`).
* The non-indexed fix is live: class=sprite lines carry `ni=1;qm=~7600` (one non-indexed quad per frame, longest world edge ~7700 units = cloud-puff sized), plus
  `streaks=4..7;len~5000` (very elongated/large quads). The `ni=4..7;qm=0` lines are class=tracer (d34ee153, refused `sprite-no-depth`; unrelated).
* Vertices: over the class=sprite lines 4837 of 5664 vertices (85 %) are inside the game window, 827 outside; `eye-only` = the out-window ones.
### 16.2 Which draw class is the cut cloud (ranked)
1. [I, strong] a replayed GREATER (class 1) sprite whose vertices the GAME ALREADY submitted cut at the window: ps2_v quads are pre-projected GS coordinates and 85 % of their vertices lie in the window,
   i.e. the game culls/clips its sprites to about the window before submission; a puff straddling the border arrives truncated and the per-eye re-projection places the truncated quad faithfully: a straight
   edge exactly at the window border. In the game window (g0135) the same haze runs on to the window edge, consistent with this. The gap would be game-side input data, not our admission.
2. [I] the GE cloud card (1 per frame) is ignored (not in the window effects image, not per eye) and our `te_clouds` sheet has no window clip [E: code], so neither makes a window-aligned edge.
3. [I, weak] the game scene image: not composited (`xr_terrain_blend` absent).
Evidence quality: admission refuted [E]; the game-side clip mechanism is [I] (logs hold counts, not vertex coordinates).
### 16.3 No code change
No refused/unadmitted draw is provable from source or logs, so no speculative admit (an offline test would pin nothing).
### 16.4 Null check to confirm (day config, m03k, same flight)
Run with `fxsig:4caf6e2d` (per-draw vertex dump) and look at the large quad (`ni=1;qm~7600`) and the streaks while a cut cloud is in view: if its vertices are clamped to GS x 1792/2304
(2048 +- 256) or y 1824/2272, the game clips at the window and only a game-side widen (see lens-static-search.md "Guard band") can fix it. If the vertices extend beyond the window but the eye still shows
the edge, the cut is in our replay (scissor `sprite-scissor-not-image`, GPU quad) and needs a new look.

## 17. Headset test 2026-10-30 (m03k, step133): clouds still in the forward rectangle (K, offline, 2026-10-30)

Sources: the user's session `D:\acz-evidence-archive\headset-2026-10-30-session\live-1144` (day install step133, `fxmem:6`, terrain + env clouds,
gamecloud default 0), `step5-producer.log` (ms 154172234-154682546), `xr-stereo.log`; cloud-deck flight `D:\acz-evidence-archive\clouds\runT2` and
step124 `runD5` (both with `fxsig:4caf6e2d` per-draw vertex dumps). Tags as above.

### 17.1 What the counters really say [E]
* `fx-sprite-gpu-totals;frames=29657;valid-frames=16929;replays=104439;committed=86470;refused=16657;window-fallback=16657;last-why=sprite-not-prepared`:
  `window-fallback` counts DRAWS, not frames (16,657 of ~121,000 sprite-class draws, 14 %), and `last-why` is only the LAST refusal of the run.
* The fallbacks come in bursts (+600..1,880 per second, 17-30 per frame) in the second flight only (ms 154583..154677); quiet seconds add 0. In those
  seconds the per-frame lines read `replays=30..32(explosion:0..2,sprite:30);refused=11..33;last-why=sprite-gpu-draw-cap` (13 of the 15 lines with a
  refusal; 2 say `sprite-not-prepared`) and the class-1 plan lines have `draws=86..121`: the per-eye path stopped at kMaxDraws - reserve = 30.
* The same seconds: `fx-sprite-totals;class=sprite(tracer-cand)` refused +27 per frame (20,672 in all); plan lines `refused=27;last-why=sprite-nonindexed-shape;
  ni=27..31`: NON-indexed class-1 draws whose shape is not a 4-vertex strip-quad list were refused BEFORE the read (never prepared); those the effects layer
  accepted reach the GPU gate as `sprite-not-prepared` and go to the window image.
* The effects layer itself refused 43,679 draws (`refused=43679(state:43679)`, `last-refusal=dual-blend-unsupported`), all GREATER 4caf6e2d
  (`4caf6e2d ... seen164139:rep91021`, minus 29,439 ignored cloud cards = 43,679), in the same bursts (~1,900/s). They are drawn NOWHERE in the eye (no
  window, no per-eye): missing, not rectangle-only. Their blend tuple was not logged (now it is: 17.3).
* Smoke (d2464ed2) is separate: `fx-smoke-gpu-totals;refused=2125;last-why=smoke-not-prepared` (16-entry store), missile trails, not clouds.

### 17.2 Are the clouds these sprites? Yes, and the game clips them at its window [E + I]
* Our cloud sheet (te_clouds) is world-anchored on every ray; the GE cloud cards are ignored (`gamecloud;replay=off;ignored=29439`); no scene image
  (`xr_terrain_blend` absent). What remains cloud-like are the GREATER 4caf6e2d sprites: non-indexed quads with `qm` 3,800-7,500 world units (puff sized)
  in the dense m03k frames [E].
* Game-side clip [E, new]: in the cloud-deck flight `clouds\runT2` (`fxsig:4caf6e2d`) EVERY dumped GREATER 4caf6e2d draw (696 non-indexed quads, 331
  indexed 9/14) has a vertex exactly on the GS window border (x 1792 / 2304, y 1824 / 2272) and NONE has a vertex outside the window (2,721 of 4,108
  vertices sit on the border; e.g. `v0=1792,2205.6 v1=1852.8,2205.6 v2=1792,2272 v3=1852.8,2272`). The game submits these cloud sprites already clipped
  to its own window and culls those outside it. In `step124\runD5` (no deck) 280 of 378 quads touch the border; some draws do reach outside.
* Consequence [I, strong]: even with every sprite replayed per eye, the cloud SPRITES can only cover what the game's camera saw (the forward rectangle,
  re-projected), with a straight edge where the game clipped. This is the dominant cause of "clouds only in the rectangle" and it cannot be fixed by
  admission: it needs a game-side guard band (lens-static-search.md "Guard band") or world-space persistence of seen puffs (section 4 particle memory; a
  new GPU design, not started). The fixes below remove OUR fallbacks (window image: rectangle only, under the objects, no stereo) in dense cloud frames.

### 17.3 Fix (fxmem:6 only; fxmem:0..5 byte-identical)
1. `fx_sprite_gpu.h kMaxDraws` 32 -> 96 = `effects_draw_cap` (static_assert in `gpu_copy_step5_fxsprite.inl`): every draw the effects layer can accept in a
   frame can be replayed per eye; the explosion reserve (2 draws, 128 KiB) and the packed 512 KiB span are unchanged (`sprite-gpu-span-full` still named).
2. `gpu_copy_step5.h effects_sprite_nonindexed_shape` (class 1 only): a non-indexed TRIANGLESTRIP of any count 3..512 (a multiple of 4 keeps the quad list,
   else ONE run) or a TRIANGLELIST with a count that is a multiple of 3 (one run) is captured; the GPU replay draws the eye copy under the game's own
   topology. `fx_sprite_replay.h collapse_quads`: a non-quad run with any unusable vertex (behind the eye) now collapses WHOLE (zero area) instead of
   keeping that vertex. Tracer (class 2) rule unchanged (strip, multiple of 4). [I] the refused m03k shape itself is not in the logs; this admits the
   geometrically safe shapes and logs the rest:
3. Diagnostics: `fx-sprite-totals` suffix `;nir=N(c:C,t:T)` (non-indexed draws of the logged frame refused by shape/topology, the last one's count and
   D3D topology; the plan line is at the text cap and stays unchanged); status line
   `continuous-hud-effects-dual-refused;count=N;last=psXXXXXXXX/vsXXXXXXXX;blend=s,d,o/sa,da,oa;dual-ready=;tracer-ready=;depth=;idx|vtx=count;topo=`
   (absent while nothing was refused that way). Verdicts unchanged.
The explosion-vs-vehicle ordering (15.2: per-eye explosion, eye objects depth test, light PSO, display composite) is untouched.

### 17.4 Tests (private build D:\b-cld, Release; "before" = the same tests on the unfixed sources)
`fx_census_offline`: (K) dense frame (94 accepted class-1 quads all per eye, explosion reserve kept), whole-run collapse, shape table, `nir` suffix within
the line cap. `gpu_copy_step1_offline`: `step5-effects-sprite-gpu` (a non-indexed 6-vertex GREATER 4caf6e2d strip is captured and read, not
`sprite-nonindexed-shape`), `step5-effects-dual-refused` (the dual-refused line names blend 5,6,1/18,19,1).
* Before (unfixed sources, same tests): `(K) dense frame: 94 accepted class-1 quads -> 30 per eye, 64 window fallbacks (first sprite-gpu-draw-cap)` FAIL;
  whole-run collapse FAIL (the bad vertex kept); `non-indexed 6-vertex strip: read 0 why sprite-nonindexed-shape` FAIL; `no dual-refused line` FAIL.
* After: `fx_census_offline` PASS (1616 checks; `94 per eye, 0 window fallbacks`); `gpu_copy_step1_offline` all 210 scenarios PASS (the 6-vertex strip:
  `read 1 why sprite-world-no-object-record`, the fixture has no world record). Ring line expectation `draws:32` -> `draws:96`.
* The private build uses a configure-only OpenXR stub (D:\b-cld-oxr): the cached SDK under `build\_deps\openxr-src` was absent on 2026-10-30.
NOT run: full build/ctest, WARP harness (no shader change), DLL, game, headset.

### 17.5 Live check (null driver, day config + `;fxsig:4caf6e2d`, m03k through the cloud deck; mem-guard first)
1. `fx-sprite-gpu` lines: `replays` above 32 in dense frames, `last-why` no longer `sprite-gpu-draw-cap`; `fx-sprite-gpu-totals` `window-fallback` growth
   in the dense seconds near 0 (was 600-1,880/s); remaining refusals only `sprite-gpu-span-full` or world/camera reasons.
2. `fx-sprite-plan;class=sprite(tracer-cand)`: no `refused=27` per dense frame; if `fx-sprite-totals;class=sprite(tracer-cand)` carries `;nir=`, its
   `c:`/`t:` names the shape still refused.
3. `continuous-hud-effects-dual-refused`: present iff `continuous-hud-effects` shows `refused=` > 0; its blend tuple decides whether a new clone is needed.
4. `fx-sig-draw` (fxsig:4caf6e2d): vertices on x 1792/2304 or y 1824/2272 = the game clip (17.2): the eye shows the cloud cut there whatever we replay.
5. Explosion over a vehicle / a vehicle in front of smoke as in 15.5 item 4 (ordering unchanged).

### 17.6 Step158/159 null runs: "refused=27" is NOT the cache; the cache still gets its own budget (N, offline, 2026-10-31)
Evidence: `D:\acz-evidence-archive\step158\regression\runAGC1` and `step159\check\runAGC1` (`step5-producer.log`, user cfg `vrn:1` = fxcloud:120, fxtrail:8).
* Which lines: step159 has three `fx-sprite-plan;class=sprite(tracer-cand)` lines with `refused=27` (frames 19944 / 20004 / 20064: `draws=128/140/138;read=101/113/111;prep=101/113/111;
  refused=27;last-why=sprite-nonindexed-topology;ni=29..31`); step158 has the same at frame 9109. The other refused>0 plan lines are `sprite-world-a-form` /
  `sprite-world-no-object-record` (transitions, `prep=0`), as in earlier runs.
* Cause [E]: the 27 refusals are the game's own ONE-vertex POINT-LIST draws (`fx-sprite-totals;class=sprite(tracer-cand)...;nir=27(c:1,t:1)`: count 1, D3D topology 1) of
  the GREATER 4caf6e2d class: `effects_sprite_nonindexed_shape` refuses topology 1 by name (strips and 3-lists are admitted since 17.3). It is the same 27 per dense frame that
  17.1 / 18.3 recorded in the USER's m03k session BEFORE the puff memory existed (`refused=27;last-why=sprite-nonindexed-shape`, then `-topology`). Steps up to 155 never
  reached a dense frame (max 60 sprite draws, `refused=0`), step158/159 fly long enough to reach the cloud deck (max 154 draws), so the check began to fire; nothing regressed.
* Not the cap, not the cache [E]: the same frame 19944 has `prep=101` of `read=101` (all 101 strip / list draws prepared), `fx-sprite-gpu;replays=68...refused=0;last-why=none`
  (no `sprite-gpu-draw-cap`, no `sprite-gpu-span-full`), and `fx-puff-mem;...;cemit=4` (the cache re-emitted 4 cards that frame, `live=s0,c17`, far under 160). The per-eye cap
  96 is the effects layer's own `effects_draw_cap`; the cache does not take a draw index (`note_replay` counts only the host).
* What the point-list draws are is open [I]: one vertex under a TRIANGLE pipeline draws nothing without a geometry-shader expansion, so they are probably the GS sprite points of
  the game's glow family; they are not clones of strip puffs and the replay cannot draw them. A dedicated item if a missing glow is reported.
* The check: `17.5.2` in `runBatchAG.ps1` now FAILS only on `refused=27` lines whose `last-why` is not `sprite-nonindexed-topology` (cap, span, anything new) and WARNS with the
  `nir=` text when they are the point lists.

The real hazard that the review found, fixed regardless (fxmem:6 only; fxmem:0..5 byte-identical):
* The cache's eye copies (up to `kMaxEmit` 160 cards x 6 vertices = 960 vertices per eye, 120 KiB for both eyes) were packed into the SAME 384 KiB game span, at the cursor of the
  first host draw of the frame, so a full cache left the game's own sprites 261 KiB and a later dense frame of larger puffs hit `sprite-gpu-span-full`: the game's sprite went
  to the window image (rectangle only, under the objects) because of the cache's cards.
* Now: `fx_sprite_gpu.h kTrailBytes` = 128 KiB per slot behind the game's `kSlotUpload` (`kSlotStride` = 640 KiB; the sprite UPLOAD buffer is `kSlotStride x slots`, +384 KiB for 3
  slots, the ring budget 178 MB stays far under its cap). `trail_take()` hands the cache its eye copies from that region only; `Step5Eff::trBytes` counts them and `spBytes` (the
  game's cursor, `gate()` span limit, explosion reserve) never sees them. A selection that does not fit the region (a second host of the frame after a full one) is dropped by name
  `trail-span-full` (priority below the game: the game's draw is recorded first and unchanged). Command counts: the cache's commands are only added to the effects diagnostic count,
  never checked against `effects_command_cap` (draw cap and predicate slots are the host's own).
* Test (`fx_census_offline`, 1,894 checks PASS, private build D:\b-dense, /W4): a dense frame with a FULL cache (960 vertices per eye at the first host) for 94 x 10 (step159's shape),
  94 x 4, 40 x 64 and 20 x 128 game vertices: 0 game refusals, every game copy below `kSlotUpload`, the cache copies inside the trail region, a second full selection refused as
  `trail-span-full`. The shared-cursor accounting of the old code refuses 7 (40 x 64) and 4 (20 x 128) game draws in the same frames (asserted), 0 for the two light frames: step159's
  own frames never starved, which agrees with the logs. `gpu_copy_step1_offline_tests.cpp` compiles (`/Zs`; width expectation `kSlotStride x slots`); NOT run: full build / ctest, game, headset.

## 18. Headset test 2026-10-30b (step134): aircraft / missile smoke trails cut at the forward rectangle (L, offline, 2026-10-30)

User: "clouds still clipping inside forward rectangle, same with aircraft smoke trails". Clouds: the world puff field `wcloud:1` (world-clouds.md) is not
in step134; nothing new here. Trails: decided from the archive (the step134 afternoon session is not archived; `%LOCALAPPDATA%\acz-vr` was not read).
Sources: the user's morning session `D:\acz-evidence-archive\headset-2026-10-30-session\live-1144` (step133, m03k, fxmem:6, same day config as step134
minus ui3d), null runs `step133\batchAG\runAGB2` (kills, missiles) and `runAGC1` (`fxsig:4caf6e2d`). Tags as above.

### 18.1 Which draw the trails are, and the route: (b), refused by OUR capture cap [E]
* The only trail shader is `d2464ed2` (EffDrawMissileSmoke, section 8): the user session's candidate table lists four pixel shaders (`4caf6e2d`, `d34ee153`,
  `d2464ed2`, `542a11d7`) and nothing else reaches the class gate. In m03k `d2464ed2` runs as 7 draws per frame for minutes (aircraft trails, not only
  missiles); `seen79675:rep79675`: every one admitted.
* Not (a): the smoke points are WORLD points in the record, and the game does not cull them to its window (`fx-smoke-frame` in the user session:
  e.g. `points=103;pts-in-window=50;pts-outside-window=16;pts-behind=37`; runAGB2/AGC1 `eye-only` up to 35 per frame). The per-eye GPU replay
  (fxmem:4, implied by 6) draws them in every direction: AGB2 `fx-smoke-gpu-totals;replays=7972;committed=7972;refused=0;window-fallback=0`.
* Not (c): every smoke draw is captured or counted.
* (b): `fx-smoke-totals;draws=80269;stored=78144;unread=0` and `fx-smoke-gpu-totals;refused=2125;window-fallback=2125;last-why=smoke-not-prepared`:
  draws - stored = 2,125 = the fallbacks, exactly, and `unread=0`, so EVERY fallback is a draw over `FrameCapture::kCap` = 16
  (`fx-smoke-frame;...;last-why=smoke-capture-cap`; frame lines with `draws=16` and per-frame GPU lines `replays=16;refused=1..11`). The draw over the cap
  is never prepared, the GPU gate refuses it (`smoke-not-prepared`) and it goes to the WINDOW effects image: the rectangle only, under the objects.
  In the dense m03k seconds up to 27 smoke draws per frame: up to 11 trails at a time were drawn only inside the rectangle and stopped at its border.
* Ribbons in `4caf6e2d` are not trails: runAGC1's z-varying `4caf6e2d` quads (350 dumped, all 117..1,000 units away, about 150 units long) are the gun
  tracer streaks of section 1. 47 lie wholly outside the window and none is clipped at the border; they are class 1 and already replayed per eye.

### 18.2 Fix (fxmem:4/6 only; fxmem:0..3 and 5 byte-identical)
* `fx_smoke.h FrameCapture::kCap` 16 -> 64 (2.4 x the live peak of 27). `ReplayStore` (64 x 2 eyes x 1,600 B = 204,800 B, one heap instance),
  `gpu::FrameState`, `kSlotUpload` (64 KiB -> 256 KiB per slot, 768 KiB for 3 slots) and `plan_root_swap` follow from it. The ring stays ~178 MB
  (2560x1440 RGBA16F: 177.9 MB, cap 200 MiB). CPU: at most 64 x 2 guarded 1,588-byte copies plus 2 eye records per draw. GPU: 2 GS draws per smoke draw,
  as before. A frame with more than 64 still falls back by name (`smoke-capture-cap`).
* Read-only diagnostics: `fx-smoke-totals` ends with `;over-cap=N;peak-draws=P`. N counts the draws over the cap in the run, and each one is a window-only
  trail. P is the run's peak smoke draws in one frame.
* No new option. The trail persistence of route (a) (world-space segments kept and aged after the game drops them) is not needed: the record already
  holds the trail outside the window. The game's own trail length, up to 20 points per record, is what is drawn.

### 18.3 Tests (private build D:\b-smk, Release, -j2, mem-guard 62.6 %)
* `fx_smoke_offline` 1,627 checks PASS. New checks:
  * the live peak of 27 draws in one frame is all stored, with no overflow;
  * the cap counts kCap + 4 -> 4 over;
  * `plan_root_swap` index 63 fits the 256 KiB span, index 64 is refused as `smoke-replay-cap`, and the old 64 KiB span is refused as
    `smoke-upload-span`;
  * `ReplayStore::kBytes == 204800`.
* `gpu_copy_step1_offline` all 210 scenarios PASS, including the smoke-capture, smoke-gpu and smoke-gpu-late scenarios. The new expectations are
  `store-cap=64`, `store-bytes=204800`, `upload:262144` and `;over-cap=0;peak-draws=1`.
* NOT run: the WARP harness (no shader or consumer change), the DLL, full ctest, the game, the headset.

### 18.4 Null-run check (day config, fxmem:6; m03k dogfight with several aircraft and missiles; mem-guard first)
1. `fx-smoke-ring;outcome=ready;...;per-slot=2x(smoke-rt)+upload:262144;...` (else `reason=` names it; `smoke-over-total-cap` would mean the budget changed).
2. `fx-smoke-totals`: `store-cap=64`, `over-cap=0`, and `peak-draws` above 16 in the dense seconds (it proves this run hit the old cap) and below 64.
   `stored == draws - unread`.
3. `fx-smoke-gpu-totals`: `window-fallback=0` (or only non-cap reasons named by `last-why`); per-frame `fx-smoke-gpu` lines with `replays` above 16 where
   `fx-smoke-frame` shows `draws` above 16.
4. Mirror or `png-screen.now`, head turned 60-120 deg off the nose while several trails cross the window border: each trail continues outside the
   rectangle with no straight cut at the border, and appears once (no in-window copy).
5. `summary-timing` within noise with vs without the dense trails.

## 19. Headset test 5 2026-10-30c (step137) item 5: smoke on damaged enemy aircraft cut at the forward rectangle (M, offline, 2026-10-30)

User: "smoke trails on damaged enemy aircraft also clipped to the forward rectangle" (docs/headset-tests/2026-10-30c-user-report.md item 5). Sources: the
session `D:\acz-evidence-archive\headset-2026-10-30-session\live-1936-step137` (step137, user cfg with `fxmem:6`, the 64-entry smoke store of section 18),
`step5-producer.log`; the per-draw dumps `fx-sig-draw` (`fxsig:4caf6e2d`) of the null runs `step137\batchAG\runAGC1`, `step136\batchAG2\runAGC1`,
`step133\batchAG\runAGC1`, `step124\runD5`, `clouds\runT2`. Tags as above. No log line names "damaged-aircraft smoke": the attribution is by
elimination over the draw classes that can draw smoke at all.

### 19.1 The three routes, decided [E unless tagged]
* (a) "never captured / another PSO": the session's candidate table has exactly four pixel shaders (`4caf6e2d:A:...seen305582:rep197655`,
  `d34ee153`, `d2464ed2:A:vs9eff1f92:...seen154389:rep154389`, `542a11d7`). Nothing else reaches the effects class gate. Not (a).
* The trail ribbons `d2464ed2` (sections 8 and 18) are NOT the cut smoke: `fx-smoke-totals;draws=154131;stored=154131;unread=0;store-cap=64;over-cap=0;
  peak-draws=23` (the section 18 fix is live and never hit); `fx-smoke-gpu-totals;replays=153292;committed=153292;window-fallback=839;
  last-why=smoke-not-prepared`, and those 839 are 2 frame lines with `world=smoke-world-no-object-re` (no world record: transitions), not a cap. Over the
  324 `fx-smoke-frame` lines the record points are 9,399 in the window, 8,552 outside, 17,808 behind the game camera, and 322 of 324 `fx-smoke-gpu`
  lines say `window-fallback=0`: the ribbons are world data and are drawn per eye in every direction.
* (c) "captured but refused": the class-1 sprite path refuses little: `fx-sprite-gpu-totals;replays=243848;committed=225714;window-fallback=4457`
  (1.8 %, `sprite-not-prepared`), and the plan lines name the cause, `sprite-nonindexed-topology` with `nir=..(c:1,t:1)`: one-vertex POINT lists, not
  smoke cards. The effects layer refused 64,360 draws as `dual-blend-unsupported`, all `ps4caf6e2d` with the ADDITIVE tuple `blend=5,2,1/18,19,1`
  (point lists, 12-index lists, 4-vertex strips): those are drawn NOWHERE in the eye (section 17.1), so they cannot appear "inside the rectangle only".
  [I] they are the additive glow / fire family (missing everywhere; a separate item that needs a clone for 5,2,1/18,19,1). Not the cut.
* (b) "captured but pre-clipped by the game": what remains is the dual-blend (5,6,1/18,19,1) camera-facing 4caf6e2d PUFF quads (one 4-vertex strip per
  draw, `gate=pass;cnt=4`, every vertex at one depth). Over every per-draw dump in the archive (constant-depth `gate=pass` quads of `fx-sig-draw`):

  | run | puff quads | a vertex exactly on x 1792/2304 or y 1824/2272 | outside the window WITHOUT a clipped vertex |
  |---|---|---|---|
  | step137 batchAG runAGC1 | 508 | 496 | 0 |
  | step136 batchAG2 runAGC1 | 518 | 463 | 0 |
  | step133 batchAG runAGC1 | 888 | 834 | 0 |
  | step124 runD5 | 698 | 400 | 0 |
  | clouds runT2 (cloud deck) | 1,027 | 1,027 | 0 |

  The quads section 17.2 saw reaching "outside" are all border-clipped slivers (e.g. AGC1 f=9402: `1792,1824 / 1886.9,1824 / 1792,1771.6 /
  1886.9,1771.6`), and the wcdiag census shows the uv clipped with them (`uv:0.42-0.84/0.24-0.54,b1`). The game clips every puff card to its window and
  culls the rest. The per-eye replay re-projects exactly what was submitted, so a puff trail behind a damaged aircraft ends in a straight edge at the
  rectangle whatever the head does. The z-varying 4caf6e2d quads (tracer streaks) are NOT clipped (they reach 120+ GS px outside) and are not puffs.
* Verdict: (b) for the puffs [E: the clip; I: that the damaged-aircraft smoke is these puffs, by elimination of the other draw classes above].
  Admission cannot fix it: the trail must be synthesised from what the game showed earlier.

### 19.2 Fix: the world-space puff memory `fxtrail:<s>` (fxmem:6 only; default OFF; everything else byte-identical)
`src/acz-vr/fx_puff_trail.h` (CPU core, header only), wired in `gpu_copy_step5_fxsprite.inl` / `gpu_copy_step5_hooks.inl`:
1. Observe (every class-1 draw the sprite plan reads, with the frame's world camera A): a WHOLE constant-depth strip quad (no vertex on / beyond the border),
   aspect < 3 (no streak), world edge in [1, `fxtmax`] (default 1500: the cloud puffs, 3,800-7,500 units, stay out) is unprojected to its world centre and
   half extents and stored with its 4 game vertices as a template (colour, alpha, uv, second colour, z / q form) and its BINDING KEY (the draw's first
   descriptor-table root argument, live `2:T0=15678a0012a280` / `...a2a0`); a puff whose centre lies within half its size of a stored one refreshes it.
   128 puffs; a full memory evicts the puff seen longest ago.
2. Select (at the FIRST class-1 per-eye replay of a binding key in the frame): every stored puff of that key the game did not draw whole in this or the
   previous frame. Their eye copies are BILLBOARDS facing the eye (the eye map's orthonormalised right / up), corners projected with A'_e as `reproject`
   does (z' = depth_scale x (Z0/d' + b), q' = q d/d'), alpha x (1 - age / life); a puff with a corner behind the eye is skipped. Two triangles per puff.
3. GPU: the copies go to the slot's sprite UPLOAD span right after the replay's own copies (the sprite / tracer share; `trail-span-full` otherwise), and
   `step5RecordSprite` adds per eye, after the eye draw and under the same clone PSO, OM (eye sprite image + eye objects depth test), predicate and root
   arguments: `IASetVertexBuffers(0, trail copy) IASetPrimitiveTopology(TRIANGLELIST) DrawInstanced(6n, 1, 0, startInstance)
   IASetPrimitiveTopology(the game's, setter-tracked)`; the game's slot-0 view is restored by the existing `vb_game` step. Refused (`trail-topology`)
   unless the game's topology is a triangle strip / list.
4. No double: a CLIPPED game quad whose world centre lies on a puff re-emitted this frame is collapsed in both eye copies (`plan_draw` new `extra_drop`).
5. Cull / age: a puff the game stops drawing for 3 frames while its whole card lies inside the game window in front of its camera is dropped (it
   dissipated); a puff older than the life (`fxtrail` seconds since last seen whole) expires.
6. A window fallback / plan refusal of the draw un-selects the puffs (`trail-window-fallback`; a later replay of the key may take them).

Options (inside `xr_terrain_stage`): `fxtrail:<s>` 0 (off, default) or 1..30 seconds of life; `fxtmax:<units>` 50..100000 (default 1500). Active only with
`fxmem:6` (`stage_effects`); line `xr-stereo;event=fx-puff-trail-option;life-s=;max-edge=;active=0|1;reason=` (`needs-fxmem-6` when inactive).
Log (step5-producer.log): since 19.5 ONE `fx-puff-trail` line per second while `fxtrail` is set, whatever happened (format in 19.5). Memory: CPU ~100 KB in the producer state (128 puffs + 2 x 24 KiB eye copies); no new GPU resource (the existing 512 KiB sprite span, at most
2 x 24 KiB of it per binding key per frame).
Limits [I]: a stored puff is frozen at its last whole observation (no drift / growth after it); a puff born outside the window is never seen; a puff
returning into full view is drawn twice for at most one frame; the texture identity is the binding key (a key reused for another texture page would draw
the wrong page: watch for it in the live check); the attribution of item 5 to these puffs is by elimination.

### 19.3 Tests (offline; written, NOT yet built or run: build on "build go")
`fx_puff_trail_offline` (new, registered in src/acz-vr/CMakeLists.txt): quad analysis (whole puff, clipped, streak, cloud-sized, the live AGC1 border
quad), the camera basis, store / one-frame hand-over / select once per key / eye billboard (positions = the eye projection of the world corners, the
side view keeps its width, z form with the depth scale, uv, alpha fade 0.75 after 2 of 8 s, triangle order), behind the eye, output cap, collapse of
the game's clipped part only for a re-emitted puff of the same key, cull after 3 frames in view, expiry, refresh, eviction, whole-run draws ignored,
`plan_draw` extra_drop, the line length, options (good / bad values), `stage_effects` (fxmem:6 only), apply / clear, the option line. NOT testable
offline: a PREPARED draw reaching the recorder in the producer fixture (no world frame there, as 11.5), the real GPU, the game, the headset.

### 19.4 Live check (null driver; day config + `;fxtrail:8` (optionally `;fxsig:4caf6e2d`), m03k dogfight with kills; mem-guard first)
1. xr-stereo.log `event=fx-puff-trail-option;life-s=8.0;max-edge=1500;active=1`.
2. step5-producer.log `fx-puff-trail` lines while enemies smoke: `stored` / `refreshed` > 0, `emitted` > 0 once the head or the aircraft turns away,
   `refused` small with `last-why` none or `trail-span-full` (report any `trail-binding-key-unknown` / `trail-topology`: a wiring gap). `stored` = 0
   during smoking enemies = the smoke is not these puffs (then the attribution in 19.1 is wrong; re-run with `fxsig:4caf6e2d` and look at the kill).
3. `fx-sprite-gpu-totals`: `window-fallback` growth unchanged vs a run without `fxtrail` (the trail never sends a draw to the window image).
4. Mirror / `png-screen.now` with the head turned 60-120 deg toward a smoking enemy that left the window: its puff trail continues outside the rectangle,
   fades over ~8 s, no straight cut at the border, no doubled puffs in the window; a wrong texture page on the re-emitted puffs = the key is not a texture
   identity (report it).
5. `summary-timing` within noise with vs without `fxtrail`.

### 19.5 step139 null runs: zero `fx-puff-trail` lines (M2, 2026-10-30)
`step139\batchAG\runAGB1` (gun) / `runAGB2` (kills), master 980bcc7a, day config + `;fxtrail:8`: xr-stereo.log has
`fx-puff-trail-option;life-s=8.0;max-edge=1500;active=1`, the sprite path is live (`fx-sprite-ring;outcome=ready`, `fx-sprite-gpu-totals;replays=71268` /
`89895`, class-1 plan lines `prep=2..4`), and step5-producer.log has NO `fx-puff-trail` line [E].
Cause (source): the line was a "something happened" line. `step5FxSpriteAccount` queued it only when the frame stored, emitted, collapsed, culled, expired
or refused something; a run in which no whole constant-depth quad within `fxtmax` reached the memory (and every draw had a binding key) queued nothing, so
a quiet memory and a dead path printed the same thing: nothing. Second, smaller gap: the worker called `step5FxSpriteEmit` only while a sprite / GPU line
was pending, so a queued trail (or wcdiag) line waited for an unrelated line. Which of "nothing whole within 1500" / "path closed" applied in step139 is
NOT decidable from these logs [G]; the new line decides it on the next run.
Fix (no behaviour change of the memory itself):
* `step5FxTrailTick` (worker, every effects status tick): one line per second while `effects_puff_trail_life_s() > 0`, unconditionally
  (`fx_puff_trail.h line_due`); the window is every frame `step5FxSpriteAccount` ended since the previous line (`frames=0` = no armed fxmem:6 frame
  reached the memory, `open=gG,rR` = the sprite GPU switch / its resources).
* The observe census on the line: `draws` (class-1 draws observed), `nokey` (no binding key: also `refused`, `trail-binding-key-unknown`), `runs`
  (whole-run draws), `quads`, `nc` (not one depth), `whole` (constant-depth quads not clipped), `big` / `streak` / `tiny` (whole quads refused by
  `fxtmax` / shape / min edge), `emax` (largest world edge of a whole card). A class-1 draw without the frame's world camera is now refused by name
  (`trail-no-world-camera`).
* `step5FxSpriteEmit` also runs while a wcdiag line is pending.
Line: `fx-puff-trail;frame=;frames=;open=g1,r1;live=;draws=;nokey=;runs=;quads=;nc=;whole=;big=;streak=;tiny=;emax=;obs=;stored=;refreshed=;clipped=;
coll=;sel=;emitted=;behind=;culled=;exp=;evict=;refused=;run=stored:,emitted:,refused:;last-why=` (<= 462 bytes with the prefix, offline-tested).
Reading the next run: `frames=0` = the trail never opened (report `open=`); `draws=0` with frames > 0 = no class-1 draw reached capture with a world
camera (`last-why`); `whole` > 0 with `big` = `whole` and `emax` > 1500 = the smoke cards are larger than `fxtmax` (raise it, e.g. `;fxtmax:6000`, only
after checking `emax` in kill seconds against the cloud puffs' 3,800-7,500); `whole=0` with `clipped` > 0 = every card the game drew was cut at its
border, nothing whole to remember; `stored` > 0 and `emitted` = 0 = nothing left the window yet.
Tests: `fx_puff_trail_offline` (+ the census of one frame over big / streak / puff / clipped / non-constant / no-key / whole-run draws, the line with
every field at its cap < 480 bytes, the zero-window line, `line_due`: 10 lines over 10 s of a run that never stores a puff, `Stats::add` of emax).

### 19.6 step141 null runs: every whole card is cloud-sized; classify by aircraft proximity, not size (M3, 2026-10-30)
Sources: `step141\batchAG\runAGB1` (Gun) / `runAGB2` (Kill, look-at-contacts), master b6ca668c, day config + `;fxtrail:8`; for positions, the wcdiag
census (`fx-cloud-puff`, cards with an edge >= 1000) of `step136\batchAG2\runAGC1/AGC2` and `step137\batchAG\runAGC1/AGC2`.
What the logs show [E]:
* AGB2: 217 of 344 lines have `whole` > 0, and every one of them has `big == whole`, with `emax` from 4,000 to 11,000. Across 10-s windows, whole cards
  appear at every altitude the aircraft flew (autopilot y 19,600 down to 4,000) and in almost every window from T=30 s to the end. The player fired
  nothing (`missile=0 gun=0`). The logs carry no kill or damage timestamps (killcap.log is the harness), so no window can be tied to a damaged
  aircraft [G].
* Census positions: step136 AGC1 has 159 whole samples. 102 lie in the band (y 8,000-13,500) and 57 below it at y 5,300-7,900, all with edges of
  3,900-10,400. Their uv boxes are the same atlas region in and out of the band (u ~0.13-0.90, v ~0.15-0.89). step137 AGC1 has 105 of 107 whole
  samples in the band. No whole card under ~3,800 appears in any census.
* No per-draw dump (`fxsig`) and no vertex colour exist for step141; nothing in the archive separates smoke cards from cloud cards per card [G].
Consequences:
* SIZE cannot separate them: every whole constant-depth card in the archive is cloud-sized.
* The binding key / uv region cannot either: all the cards use one atlas region.
* The BAND cannot either: cloud-sized whole cards at 5,300-7,900 sit below the measured band (a lower layer).
* The one discriminator the producer has is WHERE the aircraft are: smoke is emitted at an aircraft, a cloud is not.
Fix (fxmem:6 + fxtrail only):
* `fxtcls:near` (the new default; `fxtcls:size` keeps the M1 rule). A whole, constant-depth, non-streak card (edge <= 20,000) is stored as a NEW puff
  only when its centre lies within max(`fxtnear`, 0.75 x its edge) of a world object drawn in the same frame. `fxtnear` defaults to 2,500 (range
  100..50,000). The object positions are the DrawACM record translations, the same field TACTICAL uses (record +76..78); `step5FxTrailObject` keeps up
  to 64 per frame, starting again at the frame's opener record. A stored puff is refreshed wherever it drifts. A cloud far from every aircraft is never
  stored; a cloud an aircraft flies through can be, and expires after the life.
* Line fields: `cls`, `objs` (object positions available, summed over the draws), `near` / `far` (whole cards within / beyond the near distance; `far` =
  puff-shaped cards not stored), `emin` (the smallest whole edge; `emax` hid small cards), `nocam`. Counters are capped at 99999, so the line stays
  under 448 bytes with the prefix (the step-1 text buffer; the M2 cap of 480 was wrong).
No world camera: AGB2's 94 `trail-no-world-camera` refusals all fall in ONE 1-s window (line 189580500, frames 12028-12087). There the sprite plan says
`last-why=sprite-world-no-object-record` with `prep=0` for every class: no DrawACM record had been drawn before the class-1 draws, so the frame has no A
at all. AGB1 has 4. AGB1's `frames=0` lines are the last 7 of 162 (after the flight), when no effects-armed frame ran. Using the last good camera was
rejected: unprojecting this frame's vertices with another frame's A misplaces the cards by depth x camera rotation (1 degree at the cards' 15,000-38,000
units of depth = 260-660 units), and re-emission needs the eye maps, which come from the same record. These draws are now counted as `nocam`
(not a refusal); the memory coasts (nothing observed, nothing culled, stored puffs keep their age).
Reading the next run (kill run, `;fxtrail:8`; default `fxtcls:near`): `objs` > 0 proves the positions arrive. `near` > 0 with `stored` > 0 in the
seconds of a kill or a smoking enemy, then `emitted` > 0 when the head or the aircraft turns away = smoke is these cards. `near` = 0 throughout while
enemies smoke = the smoke is not a whole card at all (always clipped, or another class); then the next step is an `fxsig:4caf6e2d` dump around a
damaged enemy (the kill time logged by the driver).
Tests (`fx_puff_trail_offline`, not built): the near rule (a 5,000-unit card 1,000 units from an object is stored; the same card 21,000 units from every
object is `far`, not stored; the stored puff is refreshed after drifting with no object present; `fxtcls:size` refuses it as `big`), `nocam` is not a
refusal, `emin` accumulation, the line at every cap < 448 bytes, `fxtcls` / `fxtnear` parse (good and bad values), `stage_effects` / apply / clear, and
the option line `cls=near;near=2500`.

## 20. Headset test 6 2026-10-30d (step143) items 5-6: the game's own clouds and smoke, world-anchored (M4, offline, 2026-10-30)

User (docs/headset-tests/2026-10-30d-user-report.md): item 5, the procedural world clouds (`wcloud:1`, world-clouds.md) "look terrible" next to the game's
soft puffs (user-shots 11.webp vs 12.webp): they stay off. Item 6, the damaged-aircraft smoke trails are "inconsistent, stop and start; must be rendered in
world". Goal: clouds AND smoke from the GAME'S OWN puff cards, kept in world space all around the viewer. Sources: the user session
`D:\acz-evidence-archive\headset-2026-10-30-session\live-2210-step143` (step143, cfg `...;fxmem:6;...;wcloud:1;fxtrail:8`, m03k), its
`step5-producer.log` (863 `fx-puff-trail` lines) and `xr-stereo.log` (`fx-puff-trail-option;life-s=8.0;max-edge=1500;cls=near;near=2500;active=1`). Tags as above.

### 20.1 What the fx-puff-trail lines say [E]
Sums over the 863 lines (846 with armed frames, 50,670 frames):
* `stored=27,935` new puffs, `refreshed=43,045`, `culled=27,868` (99.8 % of what was stored), `exp=67`, `evict=0`. `live` averages 2.3 and never exceeds
  16 (capacity 128). The memory never accumulates: almost every stored puff is culled by the in-view drop rule within 3 frames.
* `sel=809` selections and `emitted=1,620` eye builds (2 per selection) in the whole session: 34 of 846 seconds. Re-emitted smoke existed for well
  under 2 % of the flight.
* `whole=3,837,720` of which `streak=3,739,533` (non-card shapes) and `far=27,203` whole cards stored as nothing (cloud-sized: `emin`/`emax` 4,800-11,000 in
  those seconds). `big=0`.
* `nocam=4,155` draws without a world camera (one burst), `refused=1` (`trail-window-fallback`; `last-why` is sticky, which is why 790 lines show it).
* The one window where the trail worked (ms 191914750-191923796, 9 s): cloud-sized cards near objects (`near=37..82` per second), refreshed (`obs=120`,
  `stored=0`), `live=2`, `sel=15..83` and `emitted=30..166` while the cards left the window. In the next second the class-1 draws stopped (`frames=5`, then
  `nocam=3482`) and the emission stopped with them.
* A 213-second block (ms 192413000-192627000, in flight on m03k, `summary-flight-stage;in-flight=1`) repeats the same counts every second: 72 class-1 draws
  per frame, small cards (`emin=11`, `emax=278`), `stored=120;culled=120`, `sel=0`. A frozen scene (most likely the pause menu) whose cards are re-stored
  every frame instead of refreshed. Why they do not match frame to frame is not decidable from the logs [G: the world camera of the frozen frame changes,
  or the cards animate].
* 37 lines have fewer class-1 draws than frames; in 6 of them `live` > 0 (e.g. ms 191982109-191984109, 192291484-192293484): frames with no class-1 draw at
  all, so nothing could be re-emitted in them.

### 20.2 Does the game camera follow the head? No [E: source]
* `look:1` is the WORLD-4 fitted game LOOK (ground gain, haze, sky stops, moon: `terrain_stage.h` "WORLD-4 option look:1", parsed at `terrain_stage.cpp`
  `k == "look"`). It does not touch the camera.
* The head-driven game camera was parked (D-291: "the game's look input must not rotate the VR view"); `look_stick_option.h` `lookstick:block` (the default)
  even reports the right stick centred while the world is composed from the game camera; the EE look-write path (`ee_look_write.h`,
  `ee_camera_look_write_trial` / `ee_camera_look_drive`) is off by default and not in the user cfg. `view_mode.h` classifies cockpit / HUD-only / chase from the
  E6 camera; in cockpit the camera sits at the type's eye offset on the aircraft.
* Consequence: the game draws puffs only inside its forward rectangle along the aircraft nose, wherever the head looks. A persistent memory therefore fills
  with what the NOSE has swept: during a dogfight the nose sweeps most of the sky within a minute, so a long-lived cloud cache surrounds the viewer; a sky
  region the nose never pointed at stays empty (no fake fill: only cards the game drew). Clouds are static, so a cloud card is valid wherever it was seen.

### 20.3 Why the stored smoke "stops and starts" (ranked) [E: source + 20.1]
1. **Re-emission only inside a game draw of the SAME key** (M1 `select(key, ...)` at "the first class-1 replay of that key"). When the smoking aircraft
   leaves the forward rectangle the game stops drawing that key; its stored puffs had no draw to ride on and vanished until any card of the key was drawn
   again. 20.1: emission in 34 seconds; the 9-s working window ended exactly when the class-1 draws stopped. This is the "stop and start".
2. **Refresh radius 0.5 x the edge, nearest wins, no one-to-one assignment.** Consecutive puffs of a trail overlap (spacing well under half a 4,000-11,000 unit
   card), so every card of the trail refreshed the SAME stored puff, which hopped between them each frame (`live=2` for a whole trail).
3. **The 3-frame in-view drop + the "not drawn whole in this or the previous frame" selection.** A card that did not match (moved, or a spark) is a new puff;
   the old one, still wholly in view, is culled after 3 frames but SELECTED on 2 of them: ghosts that flicker for 2 frames (99.8 % of stored puffs were
   culled; the 213-s block churned 2 puffs per frame).
4. Not the cause: the life (`exp=67`), eviction at 128 (`evict=0`), no-world-camera coasting (`nocam` in one burst; the memory keeps age, nothing is lost).
   A frame without ANY class-1 draw (20.1 last item) still draws nothing (residual, now counted: `nohost`).

### 20.4 Design (M4, `fx_puff_trail.h`; fxmem:6 only; default OFF; everything else byte-identical)
* **Two kinds in one memory, separate budgets.** SMOKE (`fxtrail:<s>`, 128 cards, life from the last whole sighting, linear fade) and CLOUD (`fxcloud:<s>`,
  new, 384 cards, life 10..600 s, default 120). A new whole card is smoke when born at an aircraft (`fxtcls:near`: within max(`fxtnear`, 0.75 x edge) of a
  DrawACM object of the frame; objects within 300 units of the game camera = the player's own aircraft are ignored) or, `fxtcls:size`, when its edge is
  <= `fxtmax`; otherwise it is a cloud when its edge is 1,000..30,000. The class is fixed at birth.
* **Stable dedupe keyed on world position.** A whole card refreshes the nearest stored card of its key within 0.25 x the larger edge that was NOT yet
  refreshed this frame (one card, one stored card, per frame); a card within 0.02 x the edge of a card already refreshed this frame is the same card drawn
  twice (`dup`, nothing stored). Neighbouring trail puffs stay separate and do not hop.
* **No ghosts, no blink at the hand-over.** A stored card is re-emitted only when (a) it was seen whole on >= 2 frames (a spark that never holds still is never
  drawn), (b) the game camera does NOT show it whole this frame (its billboard is not wholly inside the game window: the game draws that one itself; a card
  that dissipated in view is never re-emitted, the 3-frame cull removes it), (c) it is not emitted yet this frame.
* **Hosts: any class-1 replay of the same pipeline.** Every class-1 draw replayed per eye is a host (not only the first draw of the card's key). A host takes
  its own key's cards and, with `fxtxkey:1` (default), the cards of OTHER keys when they were stored under the same game PSO and the same descriptor heaps
  and their key was bound by a game draw within 120 frames (2 s) [superseded, review 2026-10-31 F1: only a key a game draw bound EARLIER IN THE SAME FRAME under the same heaps; a key bound only in an earlier frame may name a rewritten or freed descriptor slot]. The eye copy is grouped by key; the GPU replay draws one group per key and, for a foreign
  group, re-binds the host's first descriptor-table root parameter to that key (`SetGraphicsRootDescriptorTable`, native), then binds the host's own key
  back before the game's draw. The stored smoke trail is therefore drawn on every frame that has ANY puff/cloud draw, not only frames that draw its key.
* **Visible first, bounded.** With the eye map, a host takes only cards in front of the eye overlapping its window (`offeye` otherwise); over 160 per host
  the nearest win. Order: the host's key first, then far to near (alpha blending of the dual-blend 5,6,1 pipeline).
* **Depth-correct per-eye redraw.** Unchanged maths: each card is a billboard facing the EYE, corners projected with the eye A'_e (z form with the objects'
  depth scale, q form 1/d'), drawn under the clone PSO into the eye sprite image with the eye objects' depth test (sections 11-15): aircraft in front of a
  cloud hide it, a cloud in front of an aircraft covers it.
* **Fades at the edges of the cache.** Smoke: alpha x (1 - age / life). Cloud: full until the last 20 % of its life, then linear to 0; a smooth distance
  fade from 0.75 x `fxcfar` to `fxcfar` (the cache radius, default 40,000 units); a near fade below 0.75 x its edge (0 under 0.25 x: no card in the face
  when the camera is inside a cloud the game itself no longer draws). Clouds farther than 1.25 x `fxcfar` from the game camera are dropped (`gone`).
* **The game's own puffs hidden where our copy replaces them.** As M1: a CLIPPED game quad whose world centre lies on a card of the same key re-emitted this
  frame is collapsed in both eye copies (now for clouds and for cards hosted under another key). A card wholly inside the game window is the game's (ours is
  not emitted), so the game's in-window clouds and smoke are untouched and continuous with ours at the border.
* Cost: CPU ~215 KB (512 cards) + 2 x 60 KiB eye copies in the producer state (heap), an 8 KiB selection scratch inside the memory (not on the hook thread's
  stack); per host draw O(512) tests and one sort of <= 160. GPU: <= 2 x 60 KiB of the existing 512 KiB sprite span per host (`trail-span-full` otherwise),
  one draw per key group per eye plus the table binds. No new resource.
Limits [I]: a stored card is frozen at its last whole sighting (smoke growth / drift after it is not followed); a frame without any class-1 host draw draws
none of the stored cards (`nohost`; the residual of 20.3 item 4); the texture identity is the binding key: a key re-bound by xkey is a descriptor-heap slot the
game bound within 2 s under the same heaps, so a slot the game rewrote with another texture in that time would draw that texture (`fxtxkey:0` limits every
card to its own key's draws, the M1 behaviour); the sky behind the viewer holds only what the nose swept (20.2); whether the game's cloud cards are static
in the world is [I] (the census of 19.6 shows the same positions across seconds; the live check below measures it).

### 20.5 Options, log lines, null-run cfg
Inside `xr_terrain_stage` (fxmem:6 only; the parse rejects bad values):
* `fxcloud:<s>` 0 (off, default) or 10..600: the cloud class and its life. `fxcfar:<u>` 5,000..200,000 (default 40,000): the cache radius.
* `fxtxkey:0|1` (default 1): cross-key hosts.
* Unchanged: `fxtrail:<s>` (0 or 1..30), `fxtmax:<u>`, `fxtcls:near|size`, `fxtnear:<u>`. `fxcloud` works without `fxtrail` (clouds only) and vice versa.
* xr-stereo.log option line (now also for `fxcloud` alone): `event=fx-puff-trail-option;life-s=;max-edge=;cls=;near=;active=0|1;reason=;cloud-s=;cfar=;
  xkey=;log=step5-producer.log(fx-puff-trail,fx-puff-mem);doc=...#20`.
* step5-producer.log, once per second while either option is set: the `fx-puff-trail` line (format unchanged; `live` = smoke + cloud, `far` = whole cards
  stored as nothing) followed by `fx-puff-mem;frame=;smoke=<s>;cloud=<s>;xk=;live=sS,cC;cst=;cref=;dup=;hosts=;nohost=;xkey=;nomatch=;offeye=;gone=;cexp=;
  cemit=;run=cst:,cemit:,xkey:,nohost:,dup:` (< 448 bytes with the prefix, tested).
Null-run cfg: the day cfg (cfg-U, `wcloud` absent) with `;fxtrail:8;fxcloud:120` appended to `xr_terrain_stage`, i.e.
`xr_terrain_stage=auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;...;fxtrail:8;fxcloud:120`;
Batch AG: `pwsh -NoProfile -File research\experiments\step83-verify-run1\runBatchAG.ps1 -Only boxes,clouds -Root <archive>\stepNNN\batchAG
-AllAppend ';fxtrail:8;fxcloud:120' -ExpectDxgi <new> -ExpectD3d12 <new>` (AGB2 = kills, smoking enemies; AGC1 = cruise through the m03k decks with look taps).
For the user cfg: replace `wcloud:1;fxtrail:8` (wcloud is already removed, DAAE81D7) by `fxtrail:8;fxcloud:120`.

### 20.6 Tests (offline; written, NOT built or run: build on "build go")
* `fx_puff_cloud_offline` (new, registered in src/acz-vr/CMakeLists.txt): options; the cloud class (far card -> cloud, near card -> smoke, a speck -> nothing,
  both classes in one memory, cloud refresh); the self exclusion of the near rule; separate capacities (385 clouds evict one cloud, the smoke stays); two
  trail puffs 1,000 apart with 5,000-unit cards stay two and keep their positions when seen in reverse order (M1 merged them), `dup`; the live-2210 spark churn
  (60 frames, 60 stored, >= 55 culled, 0 selected) and seen-once (0 selected); a puff dissipating in view is never re-emitted (M1: 2 ghost frames); cross-key
  hosts (the stored cloud of key K drawn on 100 consecutive frames while only key K2 is drawn, `xkey` 100, `nohost` 0), refused for another PSO, other heaps,
  `fxtxkey:0`, a dead key (F1: not bound in the same frame, or under other heaps); the own key without a PSO identity; `nohost` on a frame without hosts; `offeye`; the selection order (host key first,
  far to near) and the groups (K2 0..5, K 6..17); the emission cap (200 wanted, the nearest 160, the farthest excluded); the fades (smoke age, cloud age tail,
  cache edge 0.5 at 35,000 of 40,000, near) and build_eye applying them (beyond the edge: nothing drawn; inside the band: alpha x the smooth fade); the cache
  drop (`gone`) and the cloud expiry; the collapse of the game's clipped part for a cloud hosted under another key; the `fx-puff-mem` line at every cap; the
  `fxcloud`/`fxcfar`/`fxtxkey` parse (good and bad), `stage_effects` (fxmem:6 only) / apply / clear, the option line.
* `fx_puff_trail_offline` (updated to M4): the hand-over now needs two whole sightings and a game camera that no longer shows the card; select takes a host and
  the game map; four hosts in frame 4; `unselect(frame, ...)`; `kMaxEmitVerts x 64 = 61,440`. Every other expectation unchanged.
NOT run: the build, ctest, the WARP harness (no shader change), the DLL, the game, the headset.

### 20.7 Live check (null driver; cfg of 20.5; m03k dogfight with kills and the cloud decks; mem-guard first)
1. xr-stereo.log `fx-puff-trail-option;...;active=1;...;cloud-s=120;cfar=40000;xkey=1`.
2. `fx-puff-mem`: `live=sS,cC` with C growing as the aircraft turns through the decks (towards hundreds, capacity 384); `cref` >> `cst` while cruising
   (clouds are refreshed, not re-stored: the cards are world-static; `cst` ~ `cref` would mean they are not, then stop and report); `dup` small;
   `hosts` > 0 every second in flight; `nohost` small (frames with nothing to host on); `xkey` > 0 when smoke / clouds of one key are drawn under another;
   `nomatch` large with `xkey` 0 = the keys come from different PSOs / heaps (report the line).
3. `fx-puff-trail`: `culled` well below `stored` (M1: 99.8 %), `sel`/`emitted` > 0 in most flight seconds once the decks are behind the nose, `refused`
   without `trail-span-full` / `trail-group-cap`.
4. Mirror / `png-screen.now` with the head turned 90-180 deg from the nose after a turn through the decks: game-art cloud puffs around and behind the viewer,
   at their world places as the head moves, fading toward the cache edge; no straight cut at the forward rectangle; no doubled puffs at its border; a
   smoking enemy that left the rectangle keeps a continuous trail behind it (no gaps second to second) that fades over ~8 s.
5. A wrong texture page on re-emitted cards = a re-bound key was not that texture (then run `fxtxkey:0` and report).
6. `summary-timing` within noise with vs without `fxcloud`.

### 20.8 step146 null runs (`D:\acz-evidence-archive\step146\batchAG-fx`, `;fxtrail:8;fxcloud:120;mk360cen:1`): reading and M4b (2026-10-30)
Sums over the seconds with hosts (fx-puff-mem) and the fx-puff-trail lines [E]:

| run | secs | cst | cref | dup | gone | cexp | max live c | sel | cemit (per eye) | coll | culled | xkey | nohost | nomatch | offeye |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AGB1 gun | 333 | 43 | 25,456 | 0 | 32 | 0 | 16 | 29,679 | 58,911 | 22,849 | 1 | 8,730 | 48 | 132,484 | 296,002 |
| AGB2 kill | 348 | 102 | 38,416 | 0 | 58 | 0 | 34 | 65,572 | 129,456 | 40,874 | 12 | 16,924 | 3 | 310,638 | 798,971 |
| AGC1 cruise | 152 | 30 | 6,622 | 0 | 6 | 0 | 21 | 17,903 | 35,460 | 12,941 | 6 | 4,445 | 2 | 54,783 | 106,090 |
| AGC2 held cam | 89 | 6 | 787 | 0 | 0 | 0 | 5 | 5,533 | 11,048 | 3,719 | 1 | 1,172 | 0 | 3,716 | 3,153 |

* The cache behaves: each cloud card is refreshed hundreds of times (cref / cst 130-590), no duplicates, almost no in-view culls (1-12 against M1's 99.8 %),
  no expiry; `gone` (dropped beyond 1.25 x fxcfar from the game camera) is 74 % / 57 % of the clouds in the long dogfight runs. `gone` > 0 is itself
  evidence that the cards stay where they were seen: a field carried along with the camera would never get 50,000 units from it. cref >> cst alone does NOT
  prove it (a card moving < 0.25 x its edge per frame is refreshed too, its stored centre following it): M4b adds `cdrift` (below).
* The game draws FEW whole cloud cards: 6-102 distinct clouds per flight, at most 34 alive. The sky is sparse because the game's own deck is sparse along the
  autopilot's path, not because of the cache.
* Emission is real: cemit = 2 x sel - a few (every selected card is built into both eye copies); `refused` 1-12 per run (the `trail-window-fallback`
  last-why is sticky). `coll` 22,849 (AGB1): the game's border-clipped copy was replaced by ours that often. xkey = 15-30 % of the selections:
  cross-key hosting is used.
* nomatch / offeye are counted PER HOST CALL, not per card: a frame has ~3-5 host draws and a wanted card a host cannot take (other PSO / heaps, key not
  alive) or that lies outside the eye is counted at each of them. With the null driver the eye looks along the game camera, so most wanted cards (by
  definition outside the game window) are also outside the eye: offeye is expected to dominate in null runs and to shrink when a head turns.
* The eye dumps f448 (AGC1) / f447 (AGC2) predate the first stored cloud: AGC1's dump at ms 197381812, `live=s0,c0` until the first emission at 197417250;
  AGC2's at 197802046, first emission at 197837406. They cannot show cards. `png-screen.now` cannot help: it dumps the virtual screen, only while it is shown
  (menus). xr_png_once fires once per process after 240 fresh frames.
Changes (M4b, offline, not built):
* `fx-puff-mem` gains `cdrift` (window) and `,cdrift:` (run): the largest distance of a refreshed cloud card from its BIRTH centre. World-static clouds stay
  at unprojection noise (< a few hundred units); a camera-attached field grows by thousands. Read it in the next run before trusting the cache.
* `xkey` no longer counts a cross-key selection that `unselect` handed back (it counted selections, so it could exceed the emitted cards).
* FLIGHT-DUMP: `%LOCALAPPDATA%\acz-vr\png-eye.now` re-arms xr_png_once once per file after its capture finished (needs `xr_png_once=true`; line
  `xr-stereo;event=png-eye-armed;outcome=armed|refused;...;rearms=N`); `research/experiments/step83-verify-run1/cloudshots.ps1 -RunDir <run dir>` writes
  it from autopilot T+60 s every 20 s, 4 shots, and copies the new eye-0 pngs to `<run>\cloudshots`. Use it on the -Png runs (AGC1 / AGC2).
Tests: `fx_puff_cloud_offline` (static vs camera-following cloud over 30 frames: both cref 29, cdrift < 1 vs 5,800; the line fields; xkey after unselect),
`xr_stereo_output_offline` (png-eye.now: one re-arm = two captures / six files / distinct frame ids; a stuck file: no re-arm, one refused line; not polled
without png_once).

### 20.9 Cut cards with no whole copy: fade toward the border (`fxtclip`, M4c, 2026-10-31)
Evidence (coordinator, POV run `D:\acz-evidence-archive\pov1\runPOV2b`, `acz-vr-xr-eye0-f5207-final-up.png`): a game cloud card ends in a hard vertical
edge at x = 232 of 1200, the game-window border of a 90-degree eye (~234). M4 hides a CUT card only when a whole stored copy of it is re-emitted in the same
frame; in that second `clipped=161`, `collapsed=32`: about three cut cards in four keep their straight edge (cards first seen already cut, cards seen whole
only once, cards whose copy faded or lies off the eye).
Options considered:
* hide every cut card (`kClipHide`): no edge, but the visible part of the card inside the window vanishes too. A card crossing the border pops out of the
  forward view and pops back when the game draws it whole again; a cloud bank across the border becomes a hole along the border.
* fade the cut card toward the cut (`kClipFade`, chosen as the default): the vertices that lie ON the border (the game's clamped ones) get alpha 0 in our CPU
  copy, which the eye copies are built from. PS 4caf6e2d (ps2AtestCA_p) multiplies the texture by COLOR0 (alpha = t.a x COLOR0.a) and discards only below
  1/255 [E: DXIL, step97 X2-shaders 4caf6e2d...dis.txt], so the rasteriser interpolates a linear ramp from the card's alpha at its inside edge to 0 at the
  cut. No straight line remains; the part deep inside the window keeps most of its cover; a thin sliver at the border is almost invisible; a card moving
  across the border brightens continuously instead of popping. Cost: one flag array (512 B in the producer state) and a loop over the draw's vertices, no
  GPU change. A corner-cut card (3 border vertices) fades over most of its area; a card larger than the window (4 border vertices) disappears: the same
  as hide, and acceptable inside a cloud. The game's own vertex buffer is never written.
* A whole stored copy re-emitted this frame still REPLACES the cut card (collapse), as in M4: the full card is better than either.
Applies to cut cards of cloud size (edge >= 1,000; for a card cut at a side the uncut side keeps the full edge) while `fxcloud` is on, and to smoke-class cut
cards (near an aircraft, or the size rule) while `fxtrail` is on. Option inside `xr_terrain_stage`: `fxtclip:off|fade|hide` (or 0|1|2; default fade; fxmem:6
with fxtrail / fxcloud). Lines: `fx-puff-mem;...;clip=fade;efade=N;ehide=N;run=...,efade:N,ehide:N`; option line `...;xkey=1;clip=fade;...`.
Tests (compiled and run standalone with MSVC 18, /std:c++17; the DLL itself not built): `fx_puff_trail_offline` 880 checks, `fx_puff_cloud_offline` 4,973
(fade marks exactly the two border vertices of a left-cut card and nothing else; hide collapses all four; off leaves it; a bad mode is fade; smoke-only:
faded near an aircraft, untouched far from every aircraft; a sub-cloud-size cut card untouched; a re-emitted whole copy still collapses instead; plan_draw's
eye copies carry alpha 0 at the border and the card alpha inside; parse, stage switches, line fields), `xr_stereo_output_offline` 280,057 (includes the
M4b png-eye.now cases). `/Zs` syntax check of d3d12_hook.cpp, xr_flight_stage.cpp, xr_stereo_output.cpp, xr_stereo_proxy.cpp: clean.
Live check: in the POV / AGC runs, `efade` should carry most of `clipped - coll`; eye dumps (cloudshots.ps1) at the border: no vertical cut, cards thinning
out toward the border line; if the fade reads as a visible "gutter" along the border, try `fxtclip:hide` and compare.

## 21. The one-vertex point lists `nir=27(c:1,t:1)`: what they are, where they go, how to replay them per eye (P, offline, 2026-10-31)

The question from 17.6 and 19.1: what are the ~27 one-vertex POINT-list draws (D3D topology 1, count 1) that the class-1 sprite path refuses as
`sprite-nonindexed-topology` in dense frames? Is there a geometry shader that expands them, and how can they reach the eye in world space?
Sources: `D:\acz-evidence-archive\step159\check\runAGC1` (`step5-producer.log`, frames 19944 / 20004 / 20064; `gameshots`, `logs\png`),
`step158\regression\runAGC1` (frame 9109), `step160\check\runAGC1`, the headset session `headset-2026-10-30-session\live-2210-step143`, and the
PSO stage census in every `diagnostics.evidence.log` of the archive. The DXIL is in `D:\acz-extract\fx-out\dxil` (`ps2_v.txt`, `ps2AtestCA_p.txt`,
`ps2Line_g.txt`). Tags as above.

### 21.1 The pipeline: ps2_v + ps2AtestCA_p, NO geometry shader, additive blend [E]
* Class 1 needs PS `4caf6e2d` (ps2AtestCA_p) with depth GREATER (`effects_sprite_class`), so the points are `4caf6e2d` draws. The headset session's
  status line names one of them: `continuous-hud-effects-dual-refused;...;last=ps4caf6e2d/vs534f29ca;blend=5,2,1/18,19,1;dual-ready=0;tracer-ready=0;
  depth=greater;vtx=1;topo=1` (6 lines). So the pipeline is VS `534f29ca` (ps2_v) + PS `4caf6e2d`, blend SRC_ALPHA / ONE / ADD for colour (ADDITIVE),
  SRC1_ALPHA / INV_SRC1_ALPHA for alpha (the dual-source alpha factors of the 4caf6e2d family), depth GREATER without a write.
* No GS. Every `4caf6e2d` PSO in the archive's census (2,953 creations) is a VS + PS pair: the record before it is always `a=0` VS `534f29ca`
  (container `sha256=6ace264b...`, 3,092 bytes) and the record after it is always the summary (`a=1|b=1`). No `a=4` (GS) record ever follows one. The
  game's GS family is small and none of it takes points:

  | GS hash | name / stage partner | input -> output |
  |---|---|---|
  | `a3b4aadb...f43b` (5,096 B) | `ps2Line_g`, only with VS ps2_v + PS `d34ee153` | lineadj -> triangles (PS2 lines widened) |
  | `bcb04cf9...6d00` (8,308 B) | `EffDrawMissileSmoke_g` (section 8) | point -> strip, world records |
  | `34d012cc...60a0` (7,512 B) | `DrawMesh_g` (terrain) | - |
  | `06084b62...86b8` (8,964 B) | DrawCPT GS (E10 / E11) | - |
  | `3af0f792...ac17` (5,924 B) | with VS `74368faa`, no colour PS (depth-only pass) | - |

* `ps2_v` (DXIL) has no point size and could not have one (D3D12 has no point-size semantic). It forwards COLOR0/1 and TEXCOORD0 and maps the
  pre-projected GS position: `clip.xy = 2 (pos.xy - C0.zw) / C0.xy / pos.w`, `clip.z = pos.z 2^-24 / pos.w`, `clip.w = 1 / pos.w` (C0 = 512, -448 and
  the window centre: the GS window 1792..2304 x 1824..2272 is NDC -1..1).
* So each draw rasterises ONE pixel at the game's scene target (2560 x 1440): D3D12 point rasterisation without a GS. There is no size or uv logic to
  recover. The PS2 original drew a POINT primitive as one GS pixel (5 x 3.2 host pixels at this target); the port draws it as one host pixel, about
  1/16 of the PS2 area.

### 21.2 When they occur and what they look like [E + I]
* Only around the player's own crash in the null runs. step159: the three frames 19944 / 20004 / 20064 (tick 220563656.., 06:11:29.6). The game
  shot `gameshots\g0140-061129.613.png` is the crash camera: "Aircraft destroyed", "MISSION FAILED", a dark explosion cloud with bright orange sparks
  around it. step158 frame 9109 (05:38:42) is the same: `g0051-053842.289.png`, "MISSION FAILED", the burning wreck with orange embers. step160 has none.
  steps 155 and earlier never reached such a frame (17.6).
* In the headset session (step143) they come in long steady stretches. The plan line refused exactly 27 in 232 of its sampled frames. Frames
  34057..45577 (about 190 s) have byte-identical plan lines (`draws=99;...;in-win=574;out-win=4;near=373;ni=28;qm=7374`): a held frame [I: pause or the
  mission-failed screen; the user shots do not cover it]. The run total is `refused=384825;nir=27(c:1,t:1)`, about 27 x 14,250 frames.
* Look [I]: the visible sparks in g0140 are 3x3..4x4-pixel blobs (an isolated-bright-pixel census of the shot: the warm components around the explosion,
  x 470..900 / y 378..520 of 2405 x 1353, are 5..18 px each; the one- and two-pixel hits sit mostly on the banner text). That is bigger than a one-pixel point, so the visible sparks are the 4-vertex additive quads of the same family. The 27 points are
  single-pixel glints among them, at most 27 pixels of a 3.7-Mpixel image. They are not clouds, not smoke, and not light sources of the world.

### 21.3 How they reach the VR eye today: nowhere [E]
* The class-1 capture refuses them before any read (`sprite-nonindexed-topology`), so there is never a per-eye copy.
* Headset session: the effects layer also refuses them. The additive tuple 5,2,1/18,19,1 has no coverage clone, so the verdict is
  `dual-blend-unsupported`, which means drawn nowhere in the eye: no window image and no per-eye replay (17.1). In the held stretch the dual-refused
  count grows about 51 per frame (425,294 -> 655,661 in 75 s), which is the 27 points plus the additive quads and lists of the same family
  (`vtx=4;topo=5`, `idx=12;topo=4`, `idx=9;topo=5`; 741,906 in the run).
* step159: the effects layer did not record them either. If it had, the GPU gate would have refused them as `sprite-not-prepared` and sent them to the
  window image, but `fx-sprite-gpu;frame=19944;...;refused=0;window-fallback=0` and the run's `window-fallback` stayed 9,207. The dual-refused count
  stayed at 42 for the whole run, so the effects layer either ignored them or refused them in the tail. Neither reason is logged per draw. The same
  frames show `committed=0;valid=0` (the crash camera has no later cockpit member, so the whole speculative effects layer stayed predicated off). Nothing
  of those frames' effects reached the eye at all.
* The flat game window is not composited into the eye for effects. So the answer to "window copy, missing, or the game's own per-eye pass" is MISSING,
  together with the rest of the additive 4caf6e2d family (19.1).

### 21.4 Design: replay the points per eye under the game's own POINT topology (no CPU quad expansion)
There is no GS whose size / uv logic could be emulated, so "expand on the CPU like the GS does" has nothing to copy. The game's own result is one pixel.
The faithful per-eye replay is therefore the same pixel at the re-projected world position. This needs no GPU design.

Stage A, the prerequisite: an additive coverage clone for 5,2,1/18,19,1. Without it the effects layer refuses the points (and the whole additive family)
before the sprite path is reached, and stage B alone changes nothing in the headset session.
* `effects_clone_alpha_factors`: a third tuple `effects_additive_blend_tuple` (5,2,1/18,19,1, full mask) -> alpha ZERO / ONE (D3D12_BLEND 1 / 2). This is
  the explosion and tracer rule: additive light adds no coverage (A' = A); RGB stays the game's SRC_ALPHA / ONE.
* The clone is made at PSO creation like the others (`d3d12_hook.cpp createGraphicsPSO`, `*desc` copied, so it keeps the game's
  `PrimitiveTopologyType`: POINT for the point PSOs, TRIANGLE for the quads and lists). Set a new `PsoOutput::additive_ready` beside `dual_ready`.
* Default stays byte-identical: `effects_decide` refuses an `additive_ready` PSO as `dual-blend-unsupported` unless the new option is on (the same
  pattern as `tracer_ready && !tracer`). Option `fxadd:1` (fxmem:6 only, default off) in the option parser next to `fxtrail`.
* With `fxadd:1` the additive quads and lists go through the existing class-1 path unchanged (strip quads, one-run lists, eye copies, clone, eye depth
  test, display composite). The window path gets them too wherever the per-eye gate refuses. This is the item 19.1 named ("needs a clone for
  5,2,1/18,19,1"), and it is the visible part (the sparks, glows and fire).

Stage B, the points (`fxadd:1` implies it, or a separate `fxpt:1` if the coordinator wants them split):
1. `effects_sprite_nonindexed_shape`, class 1 only: topology 1 (POINTLIST) with 1 <= count <= kMaxVertices and one instance is captured as ONE run
   (`quads=false`, no restarts). Topology 1 without the option keeps `sprite-nonindexed-topology`.
2. `reproject` already maps every vertex (z or q form) to the eye. The one thing that differs from strips and lists is the bad vertex (behind the eye):
   `collapse_quads` gives a run zero area, but a point has no area and would still draw one pixel at the GAME's position. New `cull_points(out, n, bad)`:
   a bad point gets pos.xy = (0, 0) GS units, which ps2_v maps to NDC (-8, +9.1) [I: C0.zw = the window centre 2048, 2048]. That is outside every viewport, so D3D12 clips the point. Colour,
   uv and q are untouched. The whole-run collapse must not run for a point run (it would only move the other points onto a bad one).
3. GPU: nothing new. The per-eye replay already draws the eye copy with `DrawInstanced(count, 1, ...)` under the game's bound topology (the sprite
   commands never set the IA topology, except the trail's restore). The bound clone is the game's PSO copy with POINT topology type, so the eye draw is
   the game's draw: one eye pixel, additive, eye-objects depth test. The eye copy is one 64 B vertex per eye, aligned to `kCopyAlign` (512 B of the
   384 KiB span per point draw, 13.5 KiB for 27). The draw cap of 96 is the real limit: 27 points plus the additive quads of a crash frame can approach it, and `sprite-gpu-draw-cap`
   names that. A later refinement could merge the frame's points into one draw if it bites.
4. The trail cache (19.2 / 20.4) already refuses a host that is not topology 4 or 5 (`trail-topology`), so a point draw never hosts puffs. `wcDrop`,
   `puff_samples` and `observe` must skip a point run (count < 3: no card). Guard them with `topology != 1` rather than relying on their shape checks.
5. Diagnostics: the plan line's `ni=` already counts them; `fx-sprite-totals` gains `;pts=N(cull:C)` (points prepared / culled) after `nir=`, within
   the line cap.

Stage C, NOT recommended unless the live look demands it: a minimum point size. Real CPU expansion (one point -> one 4-vertex strip quad of k eye pixels,
uv constant, colour copied) needs a TRIANGLE-type clone of the POINT PSO (another PSO per point PSO) and an IA topology override with a restore after the
eye draws. It would also change the game's look (the port draws one pixel). One eye pixel subtends about 0.035 deg (90 deg over 2,560) against the game
camera's 0.024 deg per pixel, so a replayed point is already about 1.5 times the game's angular size. Expand only if the headset shows the points
shimmering away and the user asks for PS2-sized sparks (k = 5 x 3.2 eye px = one GS pixel).

Tests (offline, when built): `fx_census_offline`: (P1) a topology-1 count-1 class-1 draw is captured with `fxpt` on and refused by name with it off;
(P2) its eye copy lands on the eye projection of its world point (< 1e-3 GS px, both depth forms) with the game's colour and uv bytes; (P3) a point
behind the eye is culled to (0, 0) and the other points of a multi-point run are unchanged (no whole-run collapse); (P4) 27 points + 60 quads in one
frame: all per eye, span and draw cap accounting. `gpu_copy_step1_offline`: the additive tuple gets `additive_ready`, is refused as
`dual-blend-unsupported` with `fxadd` off (byte-identical line) and accepted with it on; `effects_clone_alpha_factors(5,2,1,18,19,1)` = 1 / 2.

Live check (null driver, day config + `;fxadd:1`, a run that ends in a crash (autopilot into terrain) or a mission-failed screen; mem-guard first):
`continuous-hud-effects-dual-refused` absent (or no `blend=5,2,1/18,19,1`); `fx-sprite-plan;class=sprite(tracer-cand)` with `refused=0` in the crash
frames and `pts=27`; `fx-sprite-gpu` replays up by the family's count, `sprite-gpu-draw-cap` absent; an eye dump of the crash camera (png-screen.now)
shows the orange sparks around the explosion in both eyes, outside the forward rectangle when the head is turned.

### 21.5 Decision
The first pass (design only) left stage A to the coordinator: it changes how the whole additive 4caf6e2d family (glows, sparks, fire, ~50 draws per frame
in the headset session) reaches the eye. Approved 2026-10-31 by the orchestrator under the user's overnight pre-approval: stages A and B as designed above,
behind `fxadd:1` (default off). Stage C not built.

### 21.6 Implementation (P, `fxadd:1`, fxmem:6 only; default byte-identical)
* Option: `xr_terrain_stage ...;fxadd:0|1` (`terrain_stage.cpp`, `Conventions::fx_add`). `effects_stage_flags.h stage_effects(..., additive)` sets
  `effects_additive_flag()` only with fxmem:6; `clear_stage_effects` resets it. Option line in xr-stereo.log:
  `xr-stereo;event=fx-additive-option;requested=1;active=1|0;reason=...` (active=0: `needs-fxmem-6`).
* Stage A: `gpu_copy_step5.h effects_additive_blend_tuple` (5,2,1/18,19,1, mask 15) and `effects_clone_alpha_factors` -> ZERO / ONE. The clone is created at
  PSO creation whenever `gpu_copy_step5_effects_dual` is on (like the explosion clone: the PSO exists before the stage option arrives). That is the one
  default-path difference: one extra private PSO per additive PSO, never bound. The record gets `PsoOutput::additive_ready` (in the padding after `known`:
  `Shape` stays within its 448-byte static_assert) and keeps `dual_ready` FALSE. `effects_decide` admits it only when `EffectsClassIn::additive`
  (fxadd) is set. Without the flag the verdict, the refusal counters and the `-dual-refused` line (`dual-ready=0`) are exactly as before.
* Stage B: `effects_sprite_nonindexed_shape(..., points, &is_points)` takes a class-1 POINTLIST as one run only with the flag.
  `fx_sprite_replay.h cull_points` moves a behind-eye point to GS (0, 0). `plan_draw(..., points)` uses it instead of the collapse and turns off puff
  marking and widening. The capture skips the trail memory and the puff census for a point run. The GPU replay is unchanged: the clone is a copy of the
  game's POINT PSO, and the game's topology stays bound.
* Counters to read (step5-producer.log):
  * `gpu-copy-step1-status;event=continuous-hud-effects-additive;on=1;tuple=5,2,1/18,19,1;admitted=A;points=P;per-eye=E;per-eye-points=EP;frames=F;shots=S`
    (only while fxadd is on). The fields:
    * A = additive draws the effects layer accepted (window or per eye);
    * P = the point lists among them;
    * E / EP = replayed per eye;
    * F = frames with any per-eye additive draw;
    * S = eye dumps requested.
  * `fx-sprite-totals;class=sprite(tracer-cand);...;pts=N(cull:C)`: the pending frame's point vertices prepared per eye, and those culled behind an eye
    (both eyes counted).
  * `gpu-copy-step1;event=fx-additive-shot;frame=..;additive=..;points=..;shot=1|2`: the ADDITIVE SHOT. One eye-dump request on the first per-eye
    additive frame, and one more >= 20 s later on a frame with points. The consumer takes it only with `xr_png_once=true`, logging
    `xr-stereo;event=png-eye-armed;...;trigger=fx-additive(first-additive-per-eye-frame)`; the dump is the next `acz-vr-xr-eye0-f<frame>-*.png`.
  * What must disappear: `continuous-hud-effects-dual-refused` with `blend=5,2,1/18,19,1`, and `nir=..(c:1,t:1)` / `refused=27;last-why=sprite-nonindexed-topology`.
* Null-run cfg: the day cfg + `;fxadd:1` in `xr_terrain_stage` (fxmem:6 is already in the day preset `vrn:1`), plus `xr_png_once=true` (swap for
  `xr_edge_feather=0.06`, as the batch does) for the eye dump. Batch: `pwsh -NoProfile -File research\experiments\step83-verify-run1\runBatchAG.ps1
  -Only boxes,clouds -FxAdd -ExpectDxgi <hash> -ExpectD3d12 <hash>`. `-FxAdd` appends `;fxadd:1` to every run and gives AGB2 (Kill: explosions)
  png_once. AGC1 (cruise; step158/159 ended in the crash camera) already has png_once. The new `Analyze-FxAdd` block (P.1-P.10) runs for every run whose
  xr-stereo.log has the option line. With fxadd on, check 17.5.2 turns a topology refusal into FAIL.
* Tests (standalone MSVC 18, /std:c++17 /W4, D:\b-pts; the DLL not built):
  * `fx_census_offline` 2,068 checks PASS. New p_points cases:
    * tuple and clone factors, and the exact additive composite;
    * the class gate, byte-identical verdict with fxadd off, admitted with it on;
    * the shape table, flag off = the old refusal;
    * `cull_points`;
    * point plan: eye registration 2.1e-4 GS px; a head 100 deg left culls 8 of 10 eye points; 180 deg culls all; the old whole-run path would collapse them;
    * line formats within the cap;
    * the shot rule;
    * a dense crash frame of 27 points + 67 quads: 94 per eye, 0 fallbacks;
    * the stage switch.
  * `gpu_copy_step1_offline` all 212 scenarios PASS, with the new `step5-effects-additive-off` and `step5-effects-additive-on`:
    * off: refused 2, drawn nowhere, `blend=5,2,1/18,19,1;dual-ready=0;tracer-ready=0;`, no `-additive` line;
    * on: 2 draws in the coverage layer under the clone, `admitted=2`.
  * `fx_puff_cloud_offline` 5,533 (fxadd parse and switch), `xr_stereo_output_offline` 280,515, `hud_late_offline` 312.
  * `/Zs` of `d3d12_hook.cpp`, `xr_flight_stage.cpp`, `xr_stereo_proxy.cpp`: clean.
  * runBatchAG.ps1: parses. A re-analysis of step159 runAGC1 is unchanged (17.5.2 WARN, no FXADD block). A synthetic fxadd run gives P.1-P.8 PASS.
* NOT run: full build / ctest, the WARP harness, the game, the headset. The look of the additive family per eye is unverified until a null run's png.

### 21.7 step162 null run: hard-edged cloud rectangles with fxadd (P2, 2026-10-31)
Evidence: `D:\acz-evidence-archive\step162\fxadd` (master 955ef82b, `-FxAdd`). The counters passed (AGB2 `admitted=21932;per-eye=4314;frames=349`). The
png `runAGB2\upright\acz-vr-xr-eye0-f11356-final.png` shows three bright, hard-edged rectangles on the horizon (x 1480-2400, y 600-680), each with a cloud
streak inside. In `-worldfx.png` the region is clean. It is a pov:3 cinematic dump (`png-eye-armed;trigger=pov3-cinematic(a-only)`), not the
fx-additive shot. The game shot `g0136-090012.078-F.png` shows soft wisps there.

Cause [E: logs + DXIL + shader source; I: that these rectangles are those cards]:
* In that cinematic the class-1 draws cannot be re-projected per eye. The logs show `fx-sprite-plan;frame=11356;...;last-why=sprite-world-a-form` and
  `fx-sprite-gpu;frame=11341;...;window-fallback=10;last-why=sprite-not-prepared`, so they go to the WINDOW effects image.
* That image is coverage_v1, and its consumer composite (`xr_stereo_blit.hlsl effects_over`, mode 4) un-premultiplies: `ec = sat(P / A) x A`.
* The PS (DXIL `ps2AtestCA_p`) writes `o0 = (lerp(fog, tex x COLOR0, f), a)` and `o1 = (1, 1, 1, alpha-test bit)`. Under 5,2,1 the additive card adds
  `a x rgb` to P and leaves A unchanged.
* Over a faint dual cloud card (coverage ~0.05 across its quad), `P / A` saturates everywhere. The composite then gives one flat lift of about A over the
  dual card's whole quad: the soft shape is lost and a rectangle remains. Where A = 0 the light is dropped instead.
* So the window image cannot hold additive light at all. The per-eye sprite images can: `sprite_over` composites P + (1 - A) under without dividing.

Fix (fxadd only; default unchanged):
* The additive tuple's SHARED clone, the one the window replay binds, now draws NOTHING: RGB ZERO / ONE and alpha ZERO / ONE
  (`effects_additive_window_src/dst`). A window fallback of an additive draw is therefore invisible, as before fxadd.
* The per-eye replay binds a second private PSO, the additive light PSO (`effects_additive_light_factors`: RGB the game's 5,2,1, alpha ZERO / ONE). It
  lives in the per-eye-only slot `effectsExplosionLight`.
* An additive draw whose light PSO failed to create is refused per eye by name (`sprite-additive-pso-missing`) and falls back to the no-op window draw.

Test (`fx_census_offline`, 3,166 checks PASS): a 33 x 33 textured additive card (alpha 0.9 at the centre down to 0.02 at the corners, fog lerp as the DXIL)
over a 0.05-coverage dual card, against the game's own blend:
* per eye: equal to the game's display value at every texel (worst 1.2e-7); centre lift 0.537 against corner lift 0.0168, so the falloff is kept;
* window with the new clone: the image is unchanged, exactly 0;
* the step162 path reproduced: one flat lift of 0.0150 at both centre and corner, the hard rectangle; the centre light is clipped.

`gpu_copy_step1_offline` 212 of 212 PASS; `/Zs` of `d3d12_hook.cpp` clean. Live check: re-run `-FxAdd`. In cinematic dumps the horizon must show no
rectangles; P.3 per-eye counts are as before; explosions and sparks per eye are additive and soft.

## 22. Headset test 7 2026-10-31 (step160): clouds pop in and out, smoke plumes inconsistent (M5, offline, 2026-10-31)

User: "clouds popping in and out"; "smoke plumes from aircraft are not rendering properly or consistently in world; render them in world". Evidence:
`D:\acz-evidence-archive\headset-test7-20261031` (`step5-producer.log` = the second m03k flight, 232 `fx-puff-trail` / `fx-puff-mem` seconds;
`step5-producer.prev.log` = the first flight, 239 seconds; user cfg `vrn:1` = `fxtrail:8;fxcloud:120`, option line `xkey=1;clip=fade`). Sums [E]:

| flight | cst | cref | max live c | smoke stored / culled | emitted (run) | cemit (run) | xkey | nomatch | nohost frames | cexp | evict | trail-span-full |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| prev (m03k) | 53 | 6,574 | 22 | 0 / 0 | 37,580 | 37,580 | 2 | 168,293 | 3,668 | 0 | 0 | 0 |
| last (m03k) | 39 | 2,319 | 21 | 1,099 / ~1,090 | 87,112 | 87,112 | 3 | 137,742 | 84 | 21 | 0 | 0 |

### 22.1 Why clouds pop (ranked) [E]
1. **The F1 key rule hides a cached cloud on every frame in which the game binds no card of its key.** Since the review fix (D-409, kKeyAliveFrames 0)
   a card is drawable only under a host of its OWN key (`xkey` 2 / 3 in the two flights: cross-key hosting is effectively gone; nomatch 137-168 k). The
   game binds a key only while a card of it is inside its forward window, so the clouds beside / behind the head appear when the nose happens to see a
   cloud of the same texture key and vanish when it does not: 137 of 139 / 196 of 214 in-flight seconds with stored clouds had wanted cards no host could
   take; in 23 / 14 seconds NO stored cloud was drawn at all (e.g. prev ms 231208296: live c15, one host draw per frame, sel 0, nomatch 840 = 14 per frame).
2. **Clouds the game still drew CUT at its window border expired.** A card's life ran from its last WHOLE sighting only. Last flight ms 236986-237107:
   `whole=0` for two minutes with 6 cut cloud draws per frame (efade 4-5 / frame, coll 0-2) while `live` fell c21 -> c0 (`cexp` 21) 96-120 s after the
   last whole sightings: the cached clouds faded (age tail) and vanished while the game was still drawing them.
3. Not the cause: eviction (`evict=0`; at most 22 of 384 cloud slots), the caps, the upload region (`trail-span-full` never; refused 1-3 per flight,
   `trail-window-fallback`), the cache edge (`gone` 1-5), the in-view cull (culled clouds 1-26). The 60 `nohost` seconds of the first flight are the
   HUD-only view, where the sprite replay has no world camera (`sprite-world-a-form`, `nocam` 57 / s): nothing is replayed per eye there (separate item).

### 22.2 Why the smoke plumes are inconsistent [E + I]
* **No smoke card was ever re-emitted.** Run `emitted` == run `cemit` in both flights: every re-emitted card was a cloud. In the combat seconds of the last
  flight (ms 236960-236984) `near` = 4,700-12,300 whole quads per second near aircraft (`emin` 11, `emax` 467), 95-483 smoke cards stored per second,
  `refreshed` ~0, `culled` ~ `stored`: no smoke card matched its own previous sighting, so none reached kMinSeen = 2 and none was drawn outside the game
  window; the plume ended at the window border (faded there by `fxtclip`). The refresh radius was 0.25 x the edge = 3-117 units for these cards [I: a puff
  that drifts / grows, or a one-frame camera / vertex mismatch at ~270 units per frame (250 m/s x 65 u/m / 60), moves farther than that between frames;
  the M5 counters `sw` / `sc` below tell which].
* The d2464ed2 trail replay (section 18) is healthy: `fx-smoke-gpu-totals;replays=4724;committed=4724;refused=0;window-fallback=0`, peak 13 draws (cap 64).
* Inside the window the game's own puffs are replayed per eye (`fx-sprite-plan` class 1: read 99 / prep 99 of 126 draws; the 27 refused are the fxadd points).

### 22.3 Fix (M5, `fx_puff_trail.h` + `fx_sprite_gpu.h`; fxmem:6 with fxtrail / fxcloud; no new option, no cfg change)
* **Cut sightings renew the life.** A cut game quad that `match_clipped` assigns to a stored card (same key, centre within the card) sets its `seen_ms`:
  a card the game keeps drawing at its border never ages out; position and template stay those of the last whole sighting. Counter `renew`.
* **Smoke refresh radius.** A stored SMOKE card is refreshed by the nearest whole card of its key within max(0.25 x edge, 1.0 x edge, 1.25 x the game
  camera's step of this frame (<= 1,000 units; 0 across a cut or a skipped frame)). One card still refreshes one puff per frame and the nearest unrefreshed
  card wins, so neighbouring puffs of a trail keep their own cards; a spark moving more than an edge per frame under a still camera is still never
  re-emitted (the live-2210 churn test is unchanged). Clouds keep 0.25 x edge. Counters `sw` (one-edge rule) / `sc` (camera slack).
* **Trail region 128 -> 256 KiB per slot** (`kTrailBytes`; +384 KiB upload for 3 slots): with smoke re-emitted again (up to 128 cards) next to the clouds,
  one frame's distinct cards can exceed the ~170 the old region held; two full host selections now fit, a third is still dropped by name.
* **Telemetry per card-frame** on `fx-puff-mem`: `want` (wanted card-frames), `unb` (of them undrawable: no game draw bound the key in the frame), `renew`,
  `kch` (a whole card that matched no card of its key but lies on a card of ANOTHER key: the game re-bound the texture), `sw`, `sc`; run `,unb:`. The line
  is 430 bytes at every cap (< 448).

### 22.4 What M5 does NOT fix, and the design that would [I]
[Implemented as M6 `fxatlas:1`: section 23.] Cause 1 of 22.1 stays: a card whose key the game does not bind in a frame cannot be drawn without reading a descriptor slot that may have been rewritten or
freed (F1). `unb / want` measures it. The fix is a texture identity of our own: when a host draw binds key K (valid by construction in that draw), render
the card's uv rectangle of K into a tile of a private atlas (our RT, the clone root signature, one extra draw at the first sighting and on re-binds), and
draw the stored cards from that atlas through our own SRV heap (SetDescriptorHeaps, then the game's heaps, root signature and tables restored). That is new
GPU / lifetime work (opus design + Codex / Astra review), not part of M5. Smoke has the same limit: a smoking aircraft that left the window keeps its trail
only while some card of the trail's key is still drawn in the frame.

### 22.5 Tests (private build D:\b-puff5, Release, -j1)
* `fx_puff_cloud_offline` 5,714 checks PASS. New: a cloud cut at the border for 150 s keeps living and is drawn at full alpha (before: expired at 120 s,
  nothing selected); it expires 120 s after the last cut sighting; a 100-unit smoke puff drifting 40 units / frame is one card refreshed 3x and re-emitted
  after the nose turns (before: 4 cards, none re-emitted); two puffs half an edge apart keep their cards; a world-static puff whose unprojection jitters by
  one 270-unit camera step is one card (before: a new card every frame); a camera cut adds no slack; want / unbound / kchurn; the line. Failing-before
  verified: the behavioural checks compiled against the unchanged header fail (7 checks: renew, drift, jitter).
* `fx_census_offline` 3,174 PASS (two full host selections fit the trail region, the third is refused); `fx_puff_trail_offline` 880 PASS;
  `gpu_copy_step1_offline` 212/212 PASS (includes d3d12_hook.cpp and the producer). NOT run: the DLL build, full ctest, WARP harness, game, headset.

### 22.6 Live check (null or headset; user cfg vrn:1; m03k)
1. `fx-puff-mem`: `renew` > 0 while `whole=0` and cut cloud draws continue; no `cexp` while the game still draws the clouds.
2. `fx-puff-trail` in combat: smoke `refreshed` > 0 (`obs` > `stored`), `culled` well below `stored`, run `emitted` > run `cemit` (smoke re-emitted);
   `sw` vs `sc` says which radius matched (`sc` dominant = the camera / vertex mismatch).
3. `unb / want` = the share of wanted cached cards hidden by F1: the measure for the atlas work of 22.4. `kch` > 0 = the game re-binds textures under new keys.
4. Eye dumps / headset: a smoking enemy that leaves the window keeps its plume outside (while its key is drawn); clouds at the border stay.

## 23. Removing the texture gate: our own puff-card atlas (M6, `fxatlas:1`, offline, 2026-10-31)

Goal (22.4): a cached cloud / smoke card (FX-PUFF-MEM) is drawn per eye in any frame that has a class-1 host draw of the same game PSO, not only in a frame
where a game draw binds that card's texture handle. Test 7 had 23 / 14 seconds with no cached cloud drawn and 137-168 k wanted-but-unhostable card-frames
(`nomatch`; M5 names them `unb`). Code: `src/acz-vr/fx_puff_atlas.h` (policy + maths, SDK-free), `fx_puff_atlas_d3d12.h` (the D3D12 descriptions),
`fx_puff_atlas.hlsl` (the capture pass), `fx_puff_trail.h` (selection / eye copies), `gpu_copy_step5_fxsprite.inl` + `gpu_copy_step5_hooks.inl` (recording,
resources, telemetry), `d3d12_hook.cpp` (the capture PSO). Default OFF; `fxatlas:0` (and every cfg without the option) runs the M5 code path unchanged.

### 23.1 Why a render-pass copy and not CopyTextureRegion [E: DXIL step97 X2-shaders 4caf6e2d / 534f29ca; fx-sig-draw roots]
* The binding key is a GPU descriptor handle in the game's shader-visible heap. The proxy does not know which ID3D12Resource it describes (no reliable
  descriptor -> resource registry at Draw; the J-123..J-128 t0 work), nor that resource's state. A CopyTextureRegion would need both plus barriers on a GAME
  resource. A draw needs neither: it reads the texture exactly as the game's own draw does a moment later, through the game's bound table.
* The game's 4caf6e2d root signature: `0:C0[4]` (b0), `1:C1[4]` (b1), `2:T0` (the key table); ps2AtestCA_p reads t0 (space 0), b1 and sampler s0 from NO
  table (a static sampler). VS 534f29ca passes TEXCOORD = vertex uv.xy unchanged (normalized uv); the PS samples it, multiplies by COLOR0, discards alpha
  < 1/255, applies the COLOR1 fog and writes the dual-source alpha test.

### 23.2 Design
* **Capture** (at a host draw that binds key K, recorded inline right before the replay of that draw, unpredicated): for each stored card that this draw
  stored / refreshed / renewed (`observe`, key K) without a ready tile of (K, its uv rectangle), take a tile and record ONE TRIANGLELIST draw of all such
  quads: barrier atlas PIXEL_SHADER_RESOURCE -> RENDER_TARGET, the capture PSO (the GAME's root signature + `vs_copy` / `ps_copy` = `t0.Sample(s0, uv)`:
  the game's own table and static sampler, nothing of ours read), OM = atlas RTV, viewport / scissor = the atlas, the quads' VB (our capture UPLOAD block),
  draw, barrier back, then the game's viewport / scissor / topology. The replay that follows binds its PSO / OM / slot-0 VB and restores the game's. The
  root signature is the bound one, so the game's root arguments stay as they are.
* **Tile** = the card's uv rectangle resampled into `kContent` = 128 texels plus a 2-texel gutter on each side (the source uv continued past the rectangle,
  so the game's own sampler decides the border texels: clamp / wrap as in the game). 16 x 8 tiles of 132 texels = 2112 x 1056 RGBA16F (17.8 MB; linear
  storage, so UNORM / sRGB / float sources keep their values). Tiles are shared by all cards of the same key and rectangle.
* **Draw from the atlas**: `select` takes a wanted card under a host when (own key) or (xkey: same PSO + heaps, key bound earlier this frame) or, new,
  (fxatlas, the host is atlas-capable, same game PSO, the card has a READY tile of its current key and rectangle). Atlas cards form their own group
  (`atlas::kGroupKey`, sorted last), their eye-copy uv remapped into the tile (`map_uv`). Per eye the group is drawn under the host's clone PSO (the game's
  VS / PS / blend: same COLOR0, fog, alpha test, dual-source coverage) with `SetDescriptorHeaps(our heap)` + the key table pointed at our SRV (8 identical
  SRVs of the atlas), then `SetDescriptorHeaps(the game's bound heaps)` + the key table set back to the host's own key.
* **Host check** (`host_why`): the bound root signature must have exactly ONE descriptor table, the key's, SRV-only, <= 8 descriptors, its value tracked;
  1-2 heaps bound. "Descriptor table state is undefined ... after descriptor heaps are changed" (Microsoft Learn, Setting and Populating Descriptor Heaps):
  with one table we set it back ourselves; nothing else of the graphics binding depends on the heap. Otherwise refused by name.
* **The fxtclip border fade** (20.9) is untouched: cut game cards still fade toward the border; a whole stored copy drawn from the atlas collapses the cut
  game part exactly as an own-key copy did (`emit_frame`).

### 23.3 Lifetime, residency, fences (fail closed)
* Resources: atlas texture (created in PIXEL_SHADER_RESOURCE), a 1-entry RTV heap, a shader-visible 8-entry SRV heap, a 3 x 64 KiB UPLOAD block for the
  capture quads. Created ONCE per process by the worker tick (step-1 mutex) when `fxatlas` is on and the sprite ring is ready, on the ring's device,
  `atlasReady` set last; never released (a recording that uses them may be in flight; teardown vetoed as for the ring). Failure: `fx-puff-atlas-ring;
  outcome=refused;reason=...`, no capture and no atlas draw, ever.
* State: the atlas is PIXEL_SHADER_RESOURCE between our uses in EVERY list (each capture transitions to RENDER_TARGET and back): list order does not matter.
* No game resource is held or transitioned: the capture reads K only inside the draw that binds it; the heaps bound back are the ones the game has bound in
  that list at that point (the game keeps them alive while the list can run).
* Readiness: a tile becomes drawable only when the recording that captured it was submitted and its native ExecuteCommandLists + Signal returned
  (`slot_submitted` in step5AfterExecute). A recording Reset unsubmitted never makes its tiles ready; the next pick of its slot marks them dead and the next
  sighting recaptures (`dead`). A recording started before that point saw the tile pending and does not read it.
* Reuse: a tile goes to another (key, rectangle) only when no card references it AND every recording that captured or read it has been followed by a new pick
  of its slot (`step5PickSlot_locked` picks a slot only after its last GPU use completed and no live owned recording - one that could be re-executed - writes
  it). A quarantined slot is never picked again, so its tiles are never reused (`stuck` on the line). A ready tile is refreshed in place (same key and
  rectangle) at most every 120 frames while its cards are sighted (a key whose texture the game rewrote shows the new content within ~2 s, like the own-key
  path does at once).
* Budget: 128 tiles; a full atlas refuses the capture (`full`) and the card keeps the M5 key path; 8 captures per draw, 32 per frame (`ccap`).
* Device: the copy PSO records its device; a host whose copy PSO is not on the ring's device does not capture (`atlas-device`). A lost device returns
  UINT64_MAX from the fence and disables the producer (no recording touches the atlas again).
* Compute: the heap switch also leaves compute descriptor tables undefined. The game is not seen dispatching compute between its sprite draws [I], and the
  game's heaps are rebound before its next draw; a compute dispatch that relied on a table set before our draw would read undefined state (residual risk).
* Cost: per frame typically one atlas-group host, 2 heap switches per eye (a GPU flush on some hardware: measure `summary-timing`); one capture draw per draw
  that sighted uncaptured cards (<= 8 x 132^2 px).

### 23.4 Telemetry
* `fx-puff-mem`: `unb` now counts wanted card-frames with neither a bound key nor a READY tile (should drop to ~0); new Stats `adrawn` / `amiss`.
* `fx-puff-atlas` (1/s, after fx-puff-mem, while fxatlas is on): `res=ready|<why>`, `tiles=used/ready/stuck`, `cap` (first captures), `ref` (refreshes),
  `shr` (shared), `full`, `rec` (recycled), `rdy` (readied by submission), `dead`, `ccap`, `nor` (untileable rectangles), `drawn` (cards drawn from the
  atlas), `amiss` (a ready tile but no atlas-capable host of that PSO), `unb`, `want`, `hostno` / `hwhy` (hosts that could not draw the atlas group),
  `capno` / `cwhy` (draws whose cards could not be captured), `run=cap:,full:,drawn:,unb:,want:`. < 448 bytes (tested: 391 at every cap).
* `fx-puff-atlas-ring` once at creation; the xr-stereo option line gains `atlas=0|1`.

### 23.5 Option
`fxatlas:0|1` in `xr_terrain_stage` (default 0; fxmem:6 with fxtrail / fxcloud). NOT added to `vrn:1`: none of it has run on the game's GPU yet.
Recommended after the null run of 23.7 passes: append `;fxatlas:1` to the user's xr_terrain_stage (10 characters), then add it to the vrn preset.

### 23.6 Tests (private build D:\b-atlas, Release)
* `fx_puff_atlas_offline` (new, 366 checks): tile geometry, `rect_of` / `map_uv` / `copy_quad` and their inverse; a CPU round trip (capture at texel
  centres, then the game's bilinear read of the atlas): 1:1 rectangle max |d| 2.7e-5; a 2x magnified smooth texture 7.6e-4 inside, 4.9e-3 at a clamped edge,
  5.4e-2 across a non-periodic wrap seam (bilinear of a bilinear upsample differs by the slope change / 8: exact only at 1:1); the tile table (pending ->
  ready only on submission, shared tiles, in-place refresh, dead recapture after a discarded recording, the capture cap, reuse only after every using slot
  was picked again, full, recycled, a never-re-picked slot stuck); `host_why` / `table_span`; the memory: the test-7 scenario (a cloud of key K seen twice,
  then 100 frames whose only host binds K2): fxatlas:0 (= M5) selects nothing, unb 100, nomatch 100 (the popping: the failing-before behaviour);
  fxatlas:1 draws it every frame from tile 0 with the eye-copy uv on the tile's texels 2..130, unb 0; another PSO / a non-atlas host: nomatch + amiss, never
  unb; own key preferred; unselect hands back; expiry releases the tile; a mixed frame (own key, xkey, atlas group last); 10 cards / cap 8; the line; the option.
* `fx_puff_atlas_warp` (new, WARP + D3D12 debug layer): the production `copy_pso_desc` with the embedded `fx_puff_atlas.hlsl` bytecode, the production
  atlas / SRV-heap descriptions and the step5RecordSprite command order, against a stand-in of the 4caf6e2d root signature (b0, b1, t0 table, static s0) and
  PS. Recording 1 binds the card's key: reference card + capture (+ a decoy into the neighbouring tile); recording 2 binds only the decoy key: the card from
  the atlas under our heap, then the game's heap + key back and the host's own draw. Result: atlas card vs game card max 2/255 (1:1 noise), 2/255 (2x soft
  puff), 1/255 (4x sheet cell); no decoy bleed; the host draw after the atlas group identical to the plain host draw; debug layer 0 errors, its negative
  control (a wrong StateBefore) reported.
* Existing `fx_puff_cloud_offline` 5,714 and `fx_puff_trail_offline` 880 unchanged (fxatlas off).
* NOT run: the game, SteamVR, the headset. The heap switch and the capture have run on WARP only, never on the user's GPU / driver.

### 23.7 Null-run checks (cfg: the user's vrn:1 value + `;fxatlas:1`; m03k with turns through the cloud decks; mem-guard first; restore the cfg after)
1. `step5-producer.log`: one `fx-puff-atlas-ring;outcome=ready;...;size=2112x1056;format=10`; xr-stereo.log option line `...;atlas=1;...`.
2. `fx-puff-atlas` every second in flight: `res=ready`; `tiles` used growing then steady (tens, not 128); `cap` > 0 early then mostly `ref` / `shr`;
   `full=0`; `hostno` small with `hwhy=none` (a steady `atlas-other-table` / `atlas-key-table-*` = the live root signature differs from 23.1: stop, report);
   `capno` 0 or `cwhy=atlas-frame-cap` only (`atlas-no-copy-pso` = the copy PSO was not created: report); `dead` ~0; `stuck` 0.
3. `fx-puff-mem`: `unb` ~0 in seconds where want > 0; `run=...,unb:` far below `want`.
4. `drawn` > 0 in the seconds after the nose turned away from clouds; the fx-puff-trail `sel` / `emitted` no longer drop to 0 while clouds are cached.
5. Eye dumps (cloudshots.ps1 / png-eye.now) with the head turned away from the nose: game-art cloud cards outside the window with the SAME texture and alpha
   as the in-window ones (a garbage / black / wrong-texture card = a bad capture or remap: run `fxatlas:0` and report); no doubled cards at the window
   border; the fxtclip border fade still smooth.
6. `summary-timing` with vs without `fxatlas:1` within noise (heap switches); no device-removed / TDR; no `continuous-final`.
7. Headset (only the user can prove it): clouds beside / behind the head stay while turning; smoke plumes of an aircraft that left the window persist.

### 23.8 step164 null runs: the atlas never drew; M6b fixes (offline, 2026-10-31)
Evidence `D:\acz-evidence-archive\step164\vrn-atlas3\runAGC1|runAGC2` (cfg vrn:1;fxatlas:1), baseline `...\vrn-clouds3` [E]:
* Ring ready (2112x1056). AGC2: `drawn=0`, `hostno` ~120 / s with `hwhy=atlas-other-table` on every line, run cap 38, drawn 0; `amiss=60`, `want=300`,
  `unb=0` per second. AGC1: `tiles=128/128/0` for the rest of the flight, run cap 1573, drawn 0, `want:99999` (the print cap).
* **Cause 1, the layout.** The live 4caf6e2d root signature is not 23.1's: it is root signature 310 (stored-draw-manifest-1.md 3.2, parsed from the
  ps2_v RTS0): `0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p` = constants b0 (VS), b1 (PS), the SRV table t0 (the key) and a SAMPLER table s0, no static sampler.
  The fx-sig-draw `roots=` text (72 bytes) had been cut after parameter 2, so 23.1 read "static s0". M6 allowed one table only: every host refused.
* **Cause 2, churn.** Every stored card took a tile at its first sighting and a tile was taken back only when its cards went away: with more distinct
  (key, rectangle) cards than 128 the atlas stayed full; sparks and one-frame puffs took tiles too.
* **Cause 3, telemetry.** M6 moved a miss with a ready tile from `unb` to `amiss`: a run whose atlas never drew showed `unb` 0. The run counters printed
  at 99,999.
Fixes (M6b):
* `host_why` admits SAMPLER tables next to the key table (and still refuses any other CBV / SRV / UAV table: `atlas-other-view-table`); they need two bound
  heaps (`atlas-sampler-heap`). The step-1 section classifies the host's two bound heaps (GetDesc, outside the hook-state mutex) and keeps the game's
  SAMPLER heap bound next to ours: `record_group` (fx_puff_atlas_d3d12.h, shared by the proxy and the WARP harness) records
  `SetDescriptorHeaps(ours, game sampler)`, every sampler table set again to its game value, the key table = our SRV, the draw, `SetDescriptorHeaps(the
  game's heaps)`, every table set again (key = the host's own key). 3 + 2 x tables commands. The capture needed no change (it runs under the game's own
  heaps and tables; vs_copy / ps_copy declare t0 / s0, which root signature 310 provides through its tables).
* The layout is logged: `lay=` on the 1/s `fx-puff-atlas` line (the last refused host) and once `fx-puff-atlas-host;why=;heaps=;sampler-heap=;layout=`.
* Churn: a card takes a tile only once it was seen whole kMinSeen (2) times (it cannot be drawn before); a full atlas evicts the tile least recently
  sighted or drawn, if >= kEvictAge (30) frames ago and retired (its cards drop to the key path; `ev=` on the line).
* `unb` counts every wanted card-frame that was not drawn and whose key no game draw bound, tile or not; `amiss` is the part of it that had a ready tile.
  The run totals print up to 9,999,999.
* 17.5.3 FAIL (AGC1: 30 `continuous-hud-effects-dual-refused` lines, `refused=0`) is not the atlas: the refusals are the additive 4caf6e2d tuple
  `5,2,1/18,19,1` (fxadd off: `dual-blend-unsupported`, decided in step5EffectsConsider before any atlas code), and the status line's `refused` sums only
  the committed epochs of published frames (a refusal after the frame's last member is `tailRefused`). AGC1 of vrn-atlas3 flew twice as long (18,560 vs
  ~9,200 frames) with `tail-only` frames 3,259 (baseline 350): all 31,595 additive refusals fell in frame tails. The baseline had 60, all committed. The
  check should compare against refused + tail refusals; AGC2 of both batches: 0 / 0.
Tests: `fx_puff_atlas_offline` 1,836 checks (failing-before: root signature 310 -> M6 `atlas-other-table`, now accepted; with one heap
`atlas-sampler-heap`; another view table refused; the layout text; `record_group`'s exact command sequence for 310 and for a static-s0 signature; a miss
with a ready tile is `unb` 1 + `amiss` 1 (M6: unb 0); a first sighting takes no tile; 128 cards fill the atlas, 16 new cards evict the 16 tiles not
sighted for 35 frames (M6: all 16 refused for the rest of the flight), a recently used tile is never evicted; the line at every cap 429 bytes).
`fx_puff_atlas_warp`: both signatures (static s0 and 310 with a game SAMPLER heap whose table points at slot 1 = LINEAR / CLAMP, slot 0 a decoy POINT /
WRAP): atlas vs game 2 / 2 / 1 / 255 in both, no bleed, the host draw after the group identical (the decoy texture is now noise, so a wrong sampler after
the restore would show), debug layer 0 errors (negative control reported).
Null-run checks: 23.7 plus `fx-puff-atlas-host` (once: `why=ok;heaps=2;sampler-heap=1;layout=0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p`; anything else: report
the line), `hwhy=none`, `drawn` > 0, `unb` well below `want` (and `amiss` small), `ev` small and `full` 0 or rare.

### 22.3 HUD-only / chase: `sprite-world-a-form` was the per-view depth model, not the camera layout (offline, 2026-10-31)

[E] **Cause.** `game_map_from_a` (and `fxmem::screen_map_from_a`) demanded Z0 = a[14] - b a[15] within 1e-3 of 2^24 (the cockpit's model, b = -255.01). The
game's absolute world camera in the chase and the HUD-only view has another depth model (same screen form, rotation / lens columns as usual):

| view | b = A col2 / col3 | Z0 | Z0 / abs(b) | source (verbatim A) |
|---|---|---|---|---|
| cockpit | -255.01 | 16,777,471 | 65,791 | `pov1\runPOV3\logs\ee-camera.log` 204002287.126 (all=17) |
| chase | -1,474.51 | 96,697,267 | 65,579 | same log, 204070489.123 (all=17), 204067404.741 |
| HUD-only | -1,341.44 | 87,976,709 | 65,584 | same log, 204084889.040 (all=19), also 204092553.889 |

The depth column check passed (camera_setup opens the frame: `no-cockpit-frames` grew, the frame was valid), then only the Z0 test refused: every class-1 / tracer
draw of the frame went `sprite-world-a-form` (headset test 7 `step5-producer.prev.log` 231214312 .. 231225906, ~60 s; runPOV3 204084250 .. 204104296, the same
signature with frames flowing), `nocam` 52-254 per 60 frames, `hosts=0;nohost=60` in `fx-puff-mem`: nothing replayed per eye, no puff host, the cached cards not drawn.
[I] It is not a layout difference: rows 0..2 col2 = b x col3 holds in HUD-only (the two records' b differ, not their form).

**Fix.** The map carries its own b / Z0 (`ScreenMap` already did; the code still wrote the cockpit constants):
* `fxmem::depth_model_ok(b, z0)`: b < 0, Z0 > 0, Z0 / abs(b) in 64,000 .. 67,000 (every logged world A: 65,579 .. 65,791) replaces the 2^24 test; a matrix
  outside it is still refused.
* The eye map shares the game map's b / Z0 (`eye_map_from_a(a, eye, &game)`: the eye A' = A + D x S keeps the z column b x w and the Z0 translation).
* The eye vertex z of a z-form vertex is `Z0 / d + b` of THAT model (`game_z_for_depth(map, d)`), in `reproject` and in the puff `build_eye`; the old constant gave
  an eye depth about 5x off in HUD-only (a wrong depth test against the eye world).

Test (`fx_census_offline`, 3,256 checks PASS; new `test_hudonly_depth_model`): the four logged A (cockpit x2, HUD-only x2) are accepted by both maps, with b / Z0 as
logged; a z-form vertex round-trips to its depth with the map's model, and the cockpit constants give a depth off by more than 10% for the HUD-only A (the old
behaviour); an eye on the game camera reproduces the game z; a Z0 outside the PS2 form is refused. Failing-before: the old Z0 rule returns false for both HUD-only A.
`fx_memory` 734, `fx_puff_trail` 880, `fx_puff_cloud` 5,714, `fx_puff_atlas` 366, `world_clouds` 652,515, `fx_smoke` 1,627 PASS; `acz-vr` (dxgi.dll) compiles.

[U] Not fixed here: the SMOKE dry run / replay (`fx_smoke.h depth_column_ok`, `clip_from_a`, `eye_depth`) still assumes the cockpit's b / 2^24, so in HUD-only the smoke
record is refused `smoke-a-depth-column` as before. [U] That the HUD-only sprite VERTICES are z-form `b + Z0 / w` of their own view (indirect: the plan lines show `z=`
counts as in the cockpit, `q=0`). Null check: `runPOV1.ps1 -Tag POV3 -Append ';pov:3;povc:1;povrig:level' ... -AllAppend ';vrn:1'` (other-povs.md 11.6): in the
`request=hud-only` window `fx-sprite-plan` must show `prep>0` and no `last-why=sprite-world-a-form`, `fx-puff-trail` `nocam=0`, `fx-puff-mem` `hosts>0`; then look at
the eye dump for clouds / puffs in HUD-only.

**Smoke path (follow-up, offline).** `fx_smoke.h depth_column_ok` solved nothing (fixed b = -255.0078), so the HUD-only / chase smoke record was refused `smoke-a-depth-column` / `smoke-subst-depth-column`. It now solves b from A, requires `fxmem::depth_model_ok(b, Z0)` and can return b / Z0. `clip_from_a` needs no change (z_c / w = 2 z_s 2^-24 - 1 holds for any b, Z0: h.z = Z0 + b h.w). `gpu::eye_depth(w, scale, b, z0)` is the per-view depth (the 2-arg form stays the cockpit). Test `fx_smoke_offline` `test_hudonly_depth_model` (1,639 checks PASS): the logged HUD-only A (b -1341.44, Z0 8.80e7) is refused by the old fixed-b rule and accepted now (record_check and substitute), the cockpit A still is, a wrong Z0 / non-b-x-w column is refused, the clip z / w equals 2 x eye_depth - 1 with the view model and differs from the cockpit-constant depth. [U] smoke look in HUD-only per eye: null run only.

## 24. fxadd:1 dense-frame caps (Q, offline, 2026-11-01; step174 null runs `D:\acz-evidence-archive\step174\fxadd` vs `...\regression`)

**What overran [E].** RESULTS.txt P.7 in AGC1 (49 lines `effects-cap`, 2 `sprite-gpu-draw-cap`) and AGB2 (12 and 3): the effects layer's per-frame cap
`effects_draw_cap` (96; `first-refusal=effects-cap`, `effects-frame ... draws=96;refused=1(...cap:1)`, `continuous-hud-effects ... refused=3631(...cap:3631)`) and the
sprite path's `kMaxDraws` (96, class limit 94 after the 2-draw explosion reserve: every `fx-sprite-gpu` line with `last-why=sprite-gpu-draw-cap` reads `replays=94 ... refused=2`).
The span (`kSlotUpload`, `sprite-gpu-span-full`) never fired; the upload region of the puff cache (`trail-span-full`) never fired.

**Peak demand [E, sampled 1 frame/s].** `fx-sprite-plan` class=sprite prepared 131 (AGB2 f9673: 951 vertices), 129, 115 (AGC1 f7961); class=tracer 55-58 drawn (867 vertices);
explosion 1-2. The cap hides the true demand (frames stop at 96), so the sampled peak is a lower bound; a coincident worst frame is 131 + 58 + 2 = 191 candidates.
Bytes at that peak: ~259 KB of the span (test: 191 draws, 7 / 14 vertices per draw), i.e. 2 x 512 B per sprite-class draw.

**Change [I].** `effects_draw_cap` 96 -> 256 and `kMaxDraws` 96 -> 256 (the static_assert keeps them equal): 1.34 x the 191-candidate peak. `kSlotUpload` 512 KiB -> 1 MiB per slot
(sprite / tracer limit 896 KiB, explosion reserve 128 KiB unchanged): at 256 draws the old 384 KiB limit was exactly full with 10-vertex draws. Memory: +512 KiB UPLOAD per slot
(+1.5 MiB for 3 slots; the sprite upload buffer is now 3 x 1.25 MiB = 3.75 MiB with the 256 KiB trail region); effects and sprite images unchanged; fx total stays under
`kTotalCap` 200 MB (about 181 MB at 2560x1440). Recorded commands: 12 per sprite draw, about 3,100 per frame at the cap; no command-list size limit is involved
(`effects_command_cap` is only a derived test constant). The epoch cap (32) is untouched: it never fired.
Tests: `fx_census_offline` `test_step174_peak_frame` (191-draw frame with the measured vertex counts, 254 x 24-vertex worst case, 30 % margin checks) and
`gpu_copy_step1_step5-effects-refused` (f) (191 candidates all replayed, refused 0). Both fail on the old caps (`peak frame: draws 96 refused 95 (want 191/0)`).

**Frame time, fxadd vs the step174 regression runs without it [E, `summary-timing`, 5 s windows].** Median of the per-window values (fxadd / regression):
AGB1 frame-cpu-ms-max 10 / 12, gpu-start 1.05 / 1.31 ms; AGC1 11 / 12 and 0.56 / 1.32; AGC2 8 / 11 and 1.52 / 1.52; gpu-blit-ms 0.9-1.0 in both everywhere; xr-period 11.11 ms
in all windows (no missed frames); hooks-us-per-frame median 4.3-7.1 ms in both (its p95/max of 1.3 s are the E6 struct-scan stalls, present in both sets). One outlier:
AGB2 (the kill run, 12 explosions with the extra draws) fxadd frame-cpu-ms-max median 22 / 12, p95 78 / 24, end-late median +8.2 / -1.9 ms over ~35 consecutive windows
(max 137 vs 181 ms). Cause not established (two different flights; the over-cap frames; scan stalls); one run only.

## 25. Headset test 8 2026-11-01 (step178): cached clouds roll with the aircraft, smoke sprites rotate with the HMD (M7, offline, 2026-11-01)

User (docs/headset-tests/2026-11-01-test8-user-report.md): "Clouds are rendering now outside of forward rectangle but when i roll the aircraft they roll
too"; "MIssle smoke trail sprites seem to also rotate with my hmd". Session `D:\acz-evidence-archive\headset-test8-20261101`: option line
`fx-puff-trail-option;...;active=1;...;cloud-s=120;cfar=40000;xkey=1;clip=fade;atlas=1`, 5,868 `fx-puff-mem` lines, run totals `cemit:99999` (saturated)
`cst:512`: the cards the user saw outside the window are the fx_puff_trail re-emissions (M4..M6), not the in-window replay.

### 25.1 Cause [E: source]
* `fx_puff_trail.h` `build_eye` built every stored card with `corners(x, R, U)` where (R, U) = `basis(eye)`, the EYE map's camera axes. The eye pose is the
  cockpit pose x the HMD pose, so a re-emitted card took the aircraft's roll AND the head's roll, and its plane was parallel to the eye image plane (a head
  yaw / pitch turned it too). The same eye basis was used by `select`'s eye-visibility test. Clouds and smoke share this path (`fxcloud`, `fxtrail`;
  `fxatlas` only changes the texture source, not the corners).
* Not the cause: the in-window per-eye replay (`fx_sprite_replay.h reproject`) re-projects the game's own four world corners: those cards keep the GAME
  camera's orientation (the aircraft's roll in the cockpit, never the head's). `fx_memory.h billboard` (the older fxmem particle memory) already keeps world +Y.
* The stored data is fine: the card's world centre, half extents (game screen x / y) and corner signs; only the axes used at re-emission were wrong.

### 25.2 Fix (M7, `fx_puff_trail.h` `card_axes` / `seam_weight`; every fxtrail / fxcloud card; no new cfg key)
* **World up, facing the eye point.** The card's normal is the ray eye -> card centre (not the eye's forward: head yaw / pitch / roll do not turn it), its up
  is the world +Y (stage frame, +Y up) projected perpendicular to that ray, right = up x ray with the eye map's handedness. Neither the aircraft's roll nor
  the HMD's roll enters. A spherical billboard with world up, not a cylindrical one (a cylinder goes edge-on for clouds seen from above / below).
* **The pole.** Within ~3 degrees of straight up / down (|up perpendicular| < `kPoleSin` 0.05) the eye's up is blended in continuously (weight 0 at the
  cone's edge), so a card overhead never spins on a degenerate axis. Inside that cone the card's roll follows the eye; passing directly under a cloud the
  card turns through up to 90 degrees within the cone (inherent to any world-up billboard).
* **The seam (`kUpSeam`, the default).** The game's own in-window cards are screen-aligned in the GAME camera (rolled with the aircraft in the cockpit). A
  re-emitted card whose centre projects inside the game window or within `kSeamBand` = 0.5 window half extents beyond its border takes the game camera's
  roll, weighted 1 at the border -> 0 at the band's edge (smoothstep, rotation about the ray). At the hand-over (the card becomes wholly visible and the game
  draws it; or our whole copy replaces a cut game card) the two orientations agree (to the small tilt between "faces the game camera" and "faces the eye
  point"): no jump in roll. The head's roll never enters the seam blend (the game camera does not follow the head, D-291). Cost: one game-map projection and
  a few cross products per card per eye; no GPU change, no new resource.
* `Options::up` (`kUpEye` = M4..M6, `kUpWorld`, `kUpSeam`) is not a cfg key (the terrain option buffer is 255 chars); the DLL always runs `kUpSeam`. If the
  seam band reads wrong in the headset (cards near the window visibly rolling with the aircraft), `kSeamBand` is the one constant to change (0 = pure world
  up everywhere; the roll then snaps at the border by the aircraft's roll angle).
* `build_eye` gains a trailing `const fxmem::ScreenMap* game` (nullptr = pure world up); the producer passes the frame's game map
  (`gpu_copy_step5_fxsprite.inl`). `select` uses the same axes for its eye-visibility test.

### 25.3 Interaction with the forward window seam [I]
* In the cockpit the game camera rolls with the aircraft, so the game's in-window cards (and their per-eye replay) still roll with the aircraft: the game's
  own art, unchanged by M7. With the aircraft banked, a card moving outward across the border turns smoothly from the game's roll to world up over the seam
  band (half a window half-width, roughly 15-20 degrees of view); outside it, no roll at all.
* Cut cards at the border (`fxtclip:fade`) and the collapse rule are unchanged; the whole copy that replaces a cut card is emitted with seam weight ~1,
  i.e. the game's orientation, so the replacement does not visibly rotate.
* In HUD-only / chase views where the game camera does not roll, the seam blend is a no-op (game up ~ world up).

### 25.4 Tests (private build D:\b-fxup, MSVC 18 Release, /std:c++17, standalone as in 20.9)
`fx_puff_cloud_offline` 8,721 checks (new `orientation_tests`): (1) a stored cloud and a stored smoke card, on-axis and off-axis, re-emitted with the aircraft
turned 90 degrees (outside the game window) for aircraft roll {0, 30, 90, 170, -45} x HMD roll {0, 20, -60} x head looking at / beside the card (120 cases):
the emitted card's world up (unprojected from the eye vertices) equals world +Y perpendicular to the ray (dot > 0.99999), its right edge is level, its
normal is the ray; the emitted axes are identical between aircraft roll 0 / HMD 0 and aircraft 90 / HMD 30; (2) `kUpEye` reproduces the bug (up . Y =
cos(roll)), `kUpWorld` does not; (3) the seam, game camera rolled 30 degrees, card centres from 0.2 inside to 0.8 beyond the window border, HMD roll
{0, 25, -70}: weight 1 at / inside the border, 0 beyond `kSeamBand`, continuous; the card's roll about the ray = weight x the game's roll, independent of the
HMD roll; orthonormal, eye handedness kept; `kUpWorld` ignores the game; weight 0 behind the game camera; end to end through select / build_eye (a cloud
0.1 beyond the border: emitted roll = w x game roll) and without the game map (pure world up); (4) the pole, 10 degrees -> straight overhead in
0.25-degree steps: finite, perpendicular to the ray, no flip. `fx_puff_trail_offline` 880 and `fx_puff_atlas_offline` 1,868 unchanged and passing. `/Zs` of
d3d12_hook.cpp and xr_flight_stage.cpp: clean. NOT run: the full build, ctest, the DLL, the game, the headset.

### 25.5 Headset check list (user cfg, m03k; fxcloud:120 fxtrail:8 fxatlas:1 as installed)
1. Level cruise, then a slow 90-degree roll: clouds OUTSIDE the forward rectangle (look left / right / behind) stay upright in the world while the cockpit
   and horizon rotate around them. Report any that rotate with the cockpit.
2. Roll the head (ear to shoulder) while looking at a cached cloud and at a missile / damaged-aircraft smoke trail outside the rectangle: the puffs must not
   rotate with the head; they stay fixed in the world like the horizon.
3. Look up at a cloud directly overhead: no fast spinning except within a few degrees of straight up.
4. The seam: banked 30-60 degrees, turn the head so the edge of the forward rectangle is mid-view and watch cards crossing it: a card just outside the edge
   carries the game's tilt and straightens up as it gets farther out; no sudden roll snap at the edge itself. A snap = report the bank angle; a gradual
   "unrolling" band that bothers = candidate to shrink `kSeamBand`.
5. Inside the rectangle the game's own cards still bank with the aircraft (game art; unchanged by M7).
6. Logs: `fx-puff-mem` fields unchanged (no new field); `cemit` still > 0 in flight; no new `refused` reason.

## 26. Headset test 8 2026-11-01 (step164/step178 evidence): aircraft smoke plumes outside the forward rectangle (M7, offline, 2026-11-01)

User: "Aircraft smoke plumes dont render outside forward rectange" (docs/headset-tests/2026-11-01-test8-user-report.md item 5; the step164 radius fix was not enough).
Evidence: `D:\acz-evidence-archive\headset-test8-20261101\step5-producer.log` (user cfg `vrn:1` + `fxmem:6`; `fx-memory;requested=6`, `fx-puff-trail-option;life-s=8.0;...;atlas=1`).

### 25.1 Which path draws what [E]
| Thing | Path | Log |
| --- | --- | --- |
| missile / trail ribbons (PS `d2464ed2`, GS from world-space records) | `fx-smoke-replay` + `fx-smoke-gpu` (fxmem:4+, GPU replay per eye) | `fx-smoke-gpu;mode=gpu-replay(...)`, `replays=` |
| aircraft smoke PUFFS (camera-facing `4caf6e2d` dual-blend cards, small edges: `emin` 11-56) | `fx-puff-mem` smoke class (fxtrail:8, fxmem:6) | `fx-puff-trail`, `fx-puff-mem` |

* **"Dry run" is a label, not the state.** `fx-smoke-replay-plan;...;mode=dry-run(cpu-only,no-gpu-replay)` is the CPU plan line of the ribbons; its text is a constant
  (`fx_smoke.h` `format_dry_run`). The same session logs `fx-smoke-gpu;...;mode=gpu-replay(per-eye,...);replays=7;committed=7;refused=0;window-fallback=0` and
  `fx-smoke-ring;outcome=ready;...;created=late`. The GPU replay is on (it comes from `fxmem:4..6` in the user cfg; `vrn:1` correctly carries no `fxmem`, the cfg does).
  Nothing is missing from `terrain_preset.h`.
* **`eye0-visible=0` is not a defect.** It counts ribbon points whose eye projection lies inside the eye window; it is 0 whenever the head looks away from the
  trail (the same session shows 15, 22, 70 ... in other seconds). Both eyes agree except at the frame edge.
* **`nohost:3651` / `dup:1903` are not smoke rejections.** `nohost` = frames that wanted a cached card and had no class-1 host draw (menus, the HUD-only view);
  `dup` = the same card drawn twice in a frame. The final `live=s0` is the end of the session (no aircraft near), not a failure.
* The aircraft puffs ARE emitted in some stretches (ms 353642-353648 and 353712-353722: smoke-emitted card-frames 786 and 400, from `run emitted - run cemit`).

### 25.2 Why a plume outside the window cannot be remembered [E + I]
The game submits a puff only when it lies (at least partly) inside its window: it clips and culls on the CPU (section 19.1). A puff born while its emitter is outside
the window is never seen, so the memory (section 19: "kept from the last whole sighting") has nothing to keep. In the combat stretch (ms 353741-353752, live `s14..s21`):
`stored` 60 / s (one new puff per frame at the aircraft), `culled` 60 / s (a puff lives about 14 frames), `obs` 3 per frame; `emitted` equals `cemit` (clouds only):
every wanted card was a cloud, no smoke card was ever outside the window. The plume is inside the window whenever the nose follows the target; the moment the
aircraft leaves it, new puffs stop arriving and the 14 live ones are gone within a quarter of a second. That is what the user sees. [I: no per-card log separates
smoke `want`; the argument is `cemit == emitted` plus `live=s>0` plus `unb 0`.]

### 25.3 Fix (M7 plume streams, `fx_puff_trail.h`; fxmem:6 + fxtrail only; no option, no cfg change)
* A STREAM (4 emitters) follows the newest whole smoke card that was stored NEW nearest to a world object of the frame (the aircraft): world position per real
  birth, velocity per frame (smoothed), the card copied as the template (the game's own colour, uv, size, binding key, host PSO / heaps).
* Each frame without a real birth the stream is extrapolated (`pos + vel x frames`). While that predicted card is NOT wholly inside the game window (it is cut by
  the border, outside it or behind the game camera) and the stream had >= 6 real births (`kStreamMinBirths`: a spark is not a plume), ONE synthetic smoke card is
  spawned at the predicted point (`Puff::synth`, `seen_n = kMinSeen`, life = the fxtrail life). It then rides the existing select / build_eye / atlas path unchanged.
  The synthetic card takes the atlas tile of a live real card of the same key and uv rectangle (`Atlas::join`) so it can be drawn where the key is not bound.
* A stream ends when its predicted card lies wholly inside the window and the game draws nothing for `kStreamQuiet` = 8 frames (smoke stopped / aircraft gone) and
  `kStreamMaxFrames` = 240 frames (4 s) after the last real birth. A synthetic card that drifts into the window is culled like any card the game does not draw.
* Card basis untouched: `corners()` / `basis()` are not edited, so the card orientation (world-up, roll fix of the other item) is exactly what the card code gives.
  Only `Atlas::join` was added to `fx_puff_atlas.h`.
* Telemetry (always on while fxtrail is on): `fx-puff-mem` now ends its per-frame group with `syn=` (synthetic cards spawned this second) and `strm=` (emitters followed);
  they replace `sw=` / `sc=`, which read 0 in the whole test 8 session. The line stays 433 bytes at every cap.

### 25.4 Tests (private build D:\b-plume, Release, -j1)
`fx_puff_cloud_offline` 6,514 checks PASS (new block "M7 plume continuation"): an aircraft leaving the window emits no synthetic card while the game draws its puffs;
after it leaves, one synthetic card per frame, selected under a host of the key, built for an eye turned 45 degrees (x up to 36,000 units, following the extrapolated
emitter, not the last real puff at x = 8,800); an emitter that stops inside the window spawns nothing and the stream ends; fewer than 6 births spawn nothing; a stream
expires after 240 frames; fxtrail off = no stream. `fx_puff_trail_offline` 880, `fx_puff_atlas_offline` 1,868, `fx_census_offline` 3,834 PASS;
`gpu_copy_step1_offline_tests` builds (producer + hooks). NOT run: the DLL build, the full ctest, the game, the headset.

### 25.5 Headset check list (only the headset can prove the visual)
1. Shoot an enemy until it smokes, then turn the head 90 degrees away from it (or let it fly past outside the forward window): its plume keeps growing behind it
   for a few seconds instead of ending at the window border, drawn in the game's own puff art.
2. `fx-puff-mem`: `syn` > 0 and `strm` >= 1 in those seconds; `run emitted - run cemit` (smoke emitted) well above 0 in the combat stretch.
3. The plume stops when the aircraft is destroyed or stops smoking (`strm` falls to 0 within ~8 frames after the aircraft is back inside the window, or after 4 s outside).
4. No plume appears behind an aircraft that never smoked (a stream needs 6 puffs born at an aircraft). If one does: grab `fx-puff-trail` `near=` / `stored=` for that second.
5. Missile trails and clouds unchanged (they use the other paths); puff cards stay world-up (the card-roll item is a separate edit of the card basis).
6. If `syn` stays 0 while a smoking enemy leaves the window: the puffs are not born near a DrawACM object (check `near` / `far` on `fx-puff-trail`), or the aircraft is
   not in the frame's object list; then the next step is an `fxsig:4caf6e2d` dump around that aircraft.

## 27. Headset test 9 2026-11-02 (step182): plume streams were created but their cards could not be drawn (M8, offline, 2026-11-02)

User: "aircraft damage Smoke trails dont render outside the forward rectangle" (same as test 8).
Evidence: `D:\acz-evidence-archive\headset-test9-20261102\step5-producer.log` (the `syn=` / `strm=` fields are in `fx-puff-mem` there, not in `xr-stereo.log`; 574 lines).

### 27.1 Which gate fails [E]
* Emitter detection, aircraft match and the spawn gates PASS: `strm=1` in 67 seconds, `strm=2` in 6; `syn=60` per second (one card per frame) in every combat stretch
  (e.g. frame 5650: `syn=47`, 5710: `60`, 16498: `60,strm=2`). `live=s128` = the smoke cap (`kCap`) is saturated by synthetic cards.
* Draw gate FAILS: in the same seconds `want` is 7,000-8,900 card-frames and `unb` 6,500-8,200 (`unb/want` ~ 0.9), `nomatch` 20,000-99,999, and the atlas line shows
  `drawn=99` (frame 5710: `want=5532;unb=4615;amiss=87;rdy=17;tiles=128/16/0;cap=17`). `amiss` (cards that had a ready tile no host took) is 87 of 4,615: ~98% of the
  unbound cards had NO tile, so neither the game's own key (key K is bound only by the game's draws of other puffs, none while the aircraft is outside) nor the atlas
  could draw them. Only ~17 real cards ever have a tile.
* Cause (`fx_puff_trail.h`): M7 `spawn_synth` took an atlas tile by `join` from a live REAL card of the same key and uv rectangle, and at the cap it evicted the oldest
  card of any kind, i.e. the real cards, which are the only donors. A plume outside the window runs ~2 s of synthetic cards to the cap; then the donors are evicted and
  every later card has `tile = -1`: undrawable whenever the game binds another key (always, once the aircraft has left). Missile trails use the GPU ribbon replay
  (`fx-smoke-gpu`, no key / texture gate), which is why they appear outside the window and the damage plume does not.

### 27.2 Fix (M8)
* A stream now holds its own reference on the atlas tile of its template (`Stream::tile`, `stream_tile`): taken from any live real card of the key and rectangle at each
  real birth (retried until one exists), re-validated every call (evicted / recycled tile -> re-taken), released when the stream ends (`stream_reset`).
  Synthetic cards join the stream's tile, so they stay drawable after the real donors are gone.
* At the smoke cap a synthetic spawn evicts the oldest SYNTHETIC card first (real cards only when there is none): the real smoke the game draws is no longer pushed out.
* No new log field: the existing `unb` / `want` / `drawn` / `amiss` must now show `unb` far below `want` during a plume and `drawn` in the hundreds-thousands per second.

### 27.3 Tests (private build D:\b-smoke9, Release)
`fx_puff_cloud_offline` 11,081 PASS: new block "M8" (atlas on, game binds only another key, 250 frames, cap reached): every live synthetic card has a tile and
>= 10 synthetic cards per frame are selected from the atlas 140 frames after the real donors are gone; the block FAILS on the M7 code (`tileless > 0`).
`fx_puff_trail_offline` 880, `fx_puff_atlas_offline` 1,868 PASS. NOT run: DLL build, game, headset.

### 27.4 Headset check list
1. Shoot an enemy until it smokes, look away: the plume continues behind it for up to 4 s. 2. `fx-puff-mem` `unb` << `want` and `fx-puff-atlas` `drawn` high in those
seconds. 3. If still absent with `drawn` high: remaining gates are depth / occlusion against eye objects, alpha (`fade`) and the eye-window rectangle (`offeye`); only
the headset shows those, take a screenshot plus the log second.
