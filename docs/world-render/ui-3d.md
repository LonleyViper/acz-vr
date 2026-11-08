# UI-3D: menus and UI rendered in 3D in the headset (user goal 2026-10-29)

Offline design 2026-10-29. Nothing here ran in the game, SteamVR or a headset. Tags: **[E]** evidence (file:line, log line, disassembly),
**[I]** inference, **[U]** unknown (a live census decides it). Stage 1 (the pure core) is implemented: `src/acz-vr/ui3d.h`, ctest `ui3d_offline`.
The first live census (2026-10-29, section 6) answered the open questions and changed the thresholds; stage 2 (producer + consumer glue,
section 4) is implemented behind `ui3d:1` and NOT run live yet. Stage 3a (the in-flight pause menu on the same layers, `u3p:1`) and the
stage-3b census/design for aircraft select, hangar and results are in section 7 (2026-10-30, offline only). Section 8 is the stage-3b evidence
from null run K9 (the hangar screens are a full DrawACM_v 3D scene) plus the K10 ACM record facts (8.5); section 9 is stage 3b (the hangar as a
3D room, `u3a:1|2`, implemented 2026-10-30, offline tests only, NOT run live).
User rule: genuine render routes only (no stick-view or depth-from-image tricks): every stage below re-renders the GAME'S OWN draws.

## 0. Summary

* Outside flight the headset today shows ONE flat opaque quad with the game's final image (vscreen.md 2: `ProjectionLayer::screen_only`,
  `compose_layers` xr_stereo_output.cpp:591-596), plus the BRIEF3D table on the briefing overview (projection layer over the quad, brief3d.md 3.6).
  The in-flight pause menu is part of the HUD replay image, pulled to 2 m with an alpha floor (hud-menu.md 2).
* What the menus draw (section 1): the title / main / campaign / mission menus and save/load are **100 % pre-projected 2D** (one shader key,
  ps2_v quads). GPU-transformed 3D exists only on the briefing overview (SelMap/ModelIcon, already BRIEF3D) and on the screens after START
  MISSION (DrawACM_v, ~145 draws/frame: the aircraft-select model [I]). Cinematics/map presentation are one `movie_yuv` draw per frame.
* Chosen stage 1: **UI3D layered menus** = per-eye replay of the game's own ps2_v menu quads with a rewritten 4-vertex copy that puts each draw
  on one of 3 world-locked depth layers (backdrop far, panels, text near). From the anchor eye the composite equals the flat game image exactly;
  stereo and head motion give the layers real depth. The maths is exact for the ps2_v shader (perspective-correct texturing on tilted or curved
  layers, section 3.3). Pure core + tests done; the D3D12 producer/consumer glue (stage 2) is designed here, not implemented.

## 1. What the game draws on menu screens (evidence)

Source of truth: the `briefing-observe` histogram of run R4 (D-365; `D:\acz-evidence-archive\step92\runR4-logs\ee-camera.log`, one
`event=window` + `event=top` line per 5 s; keys = VS8/PS8/topology/indexed/GS; draws per 5 s window of ~300 presents; units = vertices or indices).

| screen (R4 route, shots `step92\runR4-shots`) | keys | draws per present | 3D content | grade |
|---|---|---|---|---|
| main menu / campaign (`01-mainmenu.png`, windows 285550-285570) | **1**: `534f29ca/d34ee153/t3n-`, max 4 units | ~347 (228.9 in the first window) | none | [E] |
| document cinematic / map presentation (285575-285611, 285741-285766) | ps2 quads + `15c8c0bd/f9027c72/t3n-` 1 per present (movie_yuv) | 23-104 | the movie is a decoded video, not geometry | [E] |
| briefing overview + BRIEFING (285631-285716, 285771-285786) | + `384ade02` SelMap `t2i` 1/frame, `d5a41c2f` ModelIcon 63/frame, ps2 `t2ig` GS lines 17/frame, ps2 `t3i` 1/frame | ~372 | SelMap grid + icons (BRIEF3D replays them); stalks/names are ps2 2D (brief3d.md 9.2) | [E] |
| mission 02 menu, save/load (`11-mission-menu.png`, `17-after-start.png`; 285796-285826) | **1**: ps2 `t3n-`, max 4 | 68-146 | none | [E] |
| after START MISSION confirms (285831-285856, 25 s, no shot) | ps2 `t3n` 4-vertex + ps2 `t3i` (9-18 idx) + `b569109e/c1b58903/t3i-` DrawACM_v, 130-149 draws/present, max 2958 idx | 157-485 | an ACM model drawn with the world-object shader: the aircraft-select model [I strong: R4 confirms aircraft/weapons/wingman there, D-89 route] | [E] counts, [I] screen |
| mission load (285861) | 13 keys incl. `a0d44e53`, `74368faa` GS, `38cd1d17`, `127819e3` | 48 | loading scene | [E] |
| hangar (V1 walk, D-89) | not histogrammed | - | [U] (likely the same ACM model path as aircraft select [I]) | [U] |
| debrief | not histogrammed; no SelMap/ModelIcon (debrief-3d-map.md 2.1) | - | [U] | [E] negative only |
| pause in flight | menu draws inside the HUD capture after the radar group: 93-106 HUD draws vs 49-65 flying (hud-menu.md 2) | - | none | [E] |

Shader facts:
* ps2_v (`534f29ca`, debug name `ps2AtestCA_v`) [E: `dxc -dumpbin`, `D:\acz-evidence-archive\step92\R3-shaders\534f29ca...dis.txt`]: inputs 4 x float4
  (64 B/vertex): ATTRIBUTE0 = x, y, z, q; ATTRIBUTE1 = COLOR0; ATTRIBUTE2.xy = TEXCOORD (w -> COLOR1.w); ATTRIBUTE3.xyz = COLOR1. cb0 (16 B) =
  (sx, sy, ox, oy): `SV_Position = ((x-ox)*2/sx, (y-oy)*2/sy, z*2^-24, 1) / q` (dis.txt lines 122-141). COLOR0/1 and TEXCOORD are `linear` =
  perspective-correct against w = 1/q. In flight cb0 = (512, -448, 2048, 2048) (fx_sprite_replay.h:6); the menu value is [U] (census).
* The 3D classes and their names: SelMap `384ade02`, ModelIcon `d5a41c2f`, DrawACM_v `b569109e`, DrawCPT_v `f4cfd928`, movie_yuv `15c8c0bd`
  (briefing-3d-map.md 2.1; `briefing_observe.h:32`).

What already distinguishes them in code:
* `PipelineRecord::brVs8/brPs8` (VS/PS creation-hash prefixes, kept for every PSO; d3d12_hook.cpp:2273 `observeDraw`) - the menu split needs
  nothing more than VS8 to tell 2D (ps2_v) from 3D.
* `brief3d::classify` (brief3d_replay_hooks.inl / brief3d_live.h) for SelMap/ModelIcon; the step-5 world classifier (gpu_copy_step5_world.h)
  for DrawACM_v, but only inside a producer frame that started at DrawCPT (in flight; `step5Draw` returns without `g_step5`, briefing-3d-map.md 3).
* Within ps2_v, the existing splits are flight-only: `hud_decide` / HUD radar windows (gpu_copy_step5.h:367-500, scene target + excursions),
  the marker classifier (box/label sprites), the fx census (by PS: 4caf6e2d sprites, d34ee153 HUD sprite). **No identity separates menu
  backdrop, panels and text**: one PSO key draws all of them [E: keys=1]. The layer split must come from per-draw geometry and draw order.

## 2. Candidates

### (a) Layered UI: the game's 2D draws split into depth layers  — CHOSEN (stage 1/2)
* Exists: the draw seam right before the game's draw (`brief3dDraw`, d3d12_hook.cpp:2352, for indexed draws); VS8 per PSO; the step-3 RS/OM track
  (viewport/scissor/OM restore; active in the menus: the BRIEF3D table renders on null runs N4/M3, notes.md:253/260 [E]); the BRIEF3D producer ring
  (3 slots, eye images, fence, Reset witness; brief3d_live.h) and consumer (`TableCore`, xr_brief3d_core.h) that submits a projection layer over
  the screen quad; the ps2_v vertex rewrite pattern (fx_sprite_replay.h, the effects "VB copy" route).
* Route: per ps2_v draw on the UI target, decide its layer online (section 3.2), write the 4 vertices twice (one copy per eye, section 3.3) into
  the slot's upload span, and record inline before the game's draw: per eye `OMSet(eye RT[, eye DSV]), RSSetViewports/Scissor(eye image),
  IASetVertexBuffers(0, copy view, stride 64), DrawInstanced(n, 1, 0, startInstance)`, then restore VB view 0, viewport, scissor, OM. PSO, root
  signature, cb0, textures and blend stay the game's: the replay IS the game's draw, moved.
* Two variants were considered and rejected: separate quad layers per depth (needs N extra swapchains and a coverage-correct alpha per layer;
  the game's alpha is not coverage, hud-menu.md 2) and a viewport-only shift (not exact for a world-locked plane once the head turns). The per-eye
  projection layer is exact, uses the existing consumer and costs no extra swapchain.
* Gains: every ps2-only screen (title, main/campaign/options menus, mission menu, save/load, very likely results/debrief panels) gets
  world-locked depth: the backdrop recedes, panels float, text is nearest, with optional tilt (`u3t`) or curvature (`u3c`).
* Failure modes (all fail closed to today's flat vscreen): a frame with any non-ps2 draw on the UI target (3D, movie, unknown shader), an indexed
  ps2 draw, > 64 vertices, > 1024 draws, an unreadable VB or cb0, an unknown viewport/OM, a PSO whose RTV/DSV formats the eye images cannot match,
  instances != 1 -> the frame is aborted (no slot published) and the table layer goes stale -> dropped after 500 ms -> the flat screen. A head
  turned away culls the draw for that eye only (`culled`). A wrong layer rule only changes depth, never the image seen from the anchor.

### (b) Per-eye replay of the GPU-transformed 3D content on menu screens (aircraft select / hangar model) — stage 3
* Exists: DrawACM_v SrtData substitution per eye in flight (gpu_copy_step5_world.h: record +0 = the world->GS matrix A, +256 translation), the
  BRIEF3D ring/consumer, `brief3d::place` (table placement) and the eye guard.
* Needs: an ACM member class outside a DrawCPT frame (today refused early: test 16 counted 109,737 pre-mission DrawACM_v refusals,
  world1-impl.md 4), a turntable fit (model box from the record's translation + the game camera centre, like `brief3d::fit_model`), the eye
  matrix `M_eye = model_to_turntable x table_to_local x view(eye) x ps2 projection` written in place of A, the lighting/reflection terms of the
  ACM record left as the game's [U: whether they depend on the camera], and a census of the screen (is the hangar backdrop also ACM? [U]).
* Gain: the aircraft on a pedestal ~1 m in front of the user, walk-around. Coverage: aircraft select / hangar only. Risk medium-high.
* Failure modes: camera-dependent lighting (wrong shading per eye), partial models (culling by the game camera), several ACM objects (backdrop
  vs model) -> refuse the frame (the flat screen stays). Cost: ~145 draws x 2 eyes, as BRIEF3D's icons.

### (c) The pause menu and in-flight panels as their own layer — stage 4
* Exists: menu mode (`hud_menu.h` Detector, 2 m, alpha floor), the HUD replay with radar windows; the menu draws are the HUD's LATE draws after
  the radar group (hud-menu.md 2: `post-excursion-draws=48` in pause).
* Route: in menu mode route the HUD frame's post-radar draws into a SECOND HUD image (new swapchain, its own quad at `menud`), and keep the HUD
  symbology at the gunsight; with UI3D's classifier the menu panel and text could get two depths. Needs the HUD replay to split one frame
  across two targets (a lifetime change in the producer, gpu_copy_step5_hooks.inl) and one more swapchain. Gain modest (menu mode already
  works, D-362 PASS in S1 notes.md:149); risk medium. Ranked last.

### Ranking
| | coverage | visible 3D gain | risk | reuse | rank |
|---|---|---|---|---|---|
| (a) layered menus | every 2D screen | depth on every menu | medium (new replay kind in a proven seam) | BRIEF3D ring/consumer, step-3 track, VB-copy pattern | **1** |
| (b) aircraft/hangar model | 1-2 screens | high there | medium-high | world-object maths, BRIEF3D | 2 |
| (c) pause layer | in-flight pause | small (exists flat) | medium | HUD replay | 3 |

## 3. Stage 1 (implemented: `ui3d.h`, pure; ctest `ui3d_offline`)

### 3.1 Options (inside `xr_terrain_stage`; no new key; default off; removed before the other parsers)
`ui3d:<0|1>`, `u3n:<0.5..10 m>` nearest (text) layer, default 1.8; `u3f:<0.6..30 m>` backdrop, default 3.5 (`u3n < u3f` enforced); `u3t:<-30..30 deg>`
tilt of every layer, top away, default 0; `u3c:<0|1>` cylinder instead of plane, default 0. The panel layer sits halfway in dioptres. Prerequisites
as BRIEF3D: `vscreen:1`, `vsearly:1`, `xr_hud_layer` (`prerequisites`). The day value plus the longest ui3d set fits the 255-character buffer (tested).

### 3.2 Layer classifier (`LayerAssigner`, online, per game frame, no look-ahead)
Per draw in game order: a non-ps2 draw on the UI target refuses the frame (`ui3d-frame-has-3d|movie|other-shader`); a draw on another target is
skipped (not replayed: pre-passes, the final present pass); indexed / > 64 vertices / > 1024 draws / bad bbox refuse. Class by the draw's
bounding box on the shown image: area >= 0.30 -> 0 (backdrop/overlay), both sides <= 0.05 -> 2 (glyph), else 1 (panel); a small draw before the
first panel belongs to the backdrop (its particles). The layer never decreases within a frame, so a far draw is never painted over a near one
(painter's order consistent with depth). A frame of only text stays on layer 0 (flat, never wrong). Thresholds are [I] until the census.
**Census update (section 6.4):** kSmallDim 0.06 (glyphs are 0.0536 high), small draws are split by the GS z (glyph z 1 vs panel/backdrop z 64:
another z = text, the same z = a decoration of the current layer), zero-area draws never promote.

### 3.3 Geometry and the vertex rewrite (`layer_point`, `project`, `rewrite_vertices`)
* GS -> shown image: `X = (x-ox)*2/sx`, then the game viewport rectangle inside the image (`SourceRect`, e.g. a 4:3 pillarbox {0.125, 0, 0.75, 1}
  [I from the R4 screenshots]) -> screen coordinates u, v on the vscreen quad (`make_screen` from the vscreen `Placement` + the anchor head position).
* Layer point: on the ray anchor -> screen(u, v), at the layer plane `dot(P - anchor, n) = D_k cos(tilt)` or on the cylinder of radius `D_k`
  about the anchor. So from the anchor every layer covers exactly the screen's angles (tested to 1e-9 for 3 yaws x plane/tilt/cylinder).
* Eye: `x' = ox + ndc_x sx/2`, `y' = oy + ndc_y sy/2` (the shader's own mapping inverted, the eye viewport = the whole eye image), z unchanged
  (depth test/write behave as in the game), and `q' = q * w_anchor / w_eye`, where `w_anchor = dot(P - anchor, fwd)` and `w_eye` = the vertex's
  view depth. Derivation: the game's attributes are perspective-correct in the source image w.r.t. q; the source image is the anchor's central
  projection of the layer; the hardware interpolates with weights 1/w_clip = q'; so the weights must be `q_src x w_anchor / w_eye` (for an
  untilted plane w_anchor is constant and drops out). The test rasterises an edge of a quad on a 25-degree tilted layer from a moved, rotated eye
  and matches the true texcoord to 2e-5, while the naive `q' = q` is wrong there.
* Culling: a vertex behind the eye or nearer than 5 cm -> `culled`: the glue records no draw for that eye.

### 3.4 API (for the stage-2 glue)
`extract_options / prerequisites / describe`, `layer_distance`, `Cb0 + cb0_ok`, `SourceRect + rect_ok`, `gs_to_screen`, `make_screen`,
`layer_point`, `EyeView + project`, `rewrite_vertices(src, n, cb0, rect, screen, opts, layer, eye, dst) -> {why, culled}`, `screen_bbox`,
`DrawFacts + LayerAssigner::assign -> layer | kSkip | kRefused`, `upload_bytes`, `kUploadPerSlot` (512 KiB), `Stats/count_frame/summary_line`
(`summary-ui3d;frames;usable;refused;draws;culled;far;mid;near;last-why`, < 256 B).

### 3.5 Tests and result
`tests/acz-vr/ui3d_offline_tests.cpp` (CMake: `ui3d_offline`, next to `brief3d_offline`): options (day value byte-identical, ranges, order rule,
buffer fit), dioptre spacing, GS mapping incl. pillarbox, direction preservation, anchor alignment (all layers = the flat image), stereo
disparity `-ipd/D` per layer, vertex rewrite vs an independent projection, perspective-correct texcoords on a tilted layer, bytes other than
x/y/q unchanged, culling (looking back / 90 degrees aside), every refusal name, the classifier on a main-menu-like frame and on SelMap/ACM/movie/
other/off-target/indexed/caps, budget, bounded summary. Private build `D:\b-ui3d` (cl /std:c++17 /W4, commit 72.6 %):
`ui3d_offline: 5699 checks passed`. The DLL was not built; the full ctest was not run.

## 4. Stage 2 (glue) - IMPLEMENTED 2026-10-29 behind `ui3d:1` (offline tests only; nothing ran in the game or a headset)

Implementation map (items 2-4 below are the design they follow):
* `ui3d.h`: census thresholds (section 6.4), `rect_from_viewport`, `eye_scissor` (the game's scissor carried onto the draw's layer and
  projected per eye: the census backdrop quads overshoot the 4:3 scissor), `DrawFacts::z`.
* `brief3d_live.h`: `UiFacts` / `UiPlan`, `Producer::ui3d_draw / ui3d_commit / ui3d_cancel / ui3d_held / ui3d_summary`. A UI3D frame is a Run
  with `ui = true` in the SAME ring/slot/list/fence lifetime (invariants 1-11 of brief3d.md 8 unchanged: one list per frame, the first plan
  clears, reservations before mutation, publication only after the list's Reset, one queue). Any refusal inside an open UI3D frame REJECTS it:
  closed, kept WRITING until its Reset, then retired WITHOUT publication; an executed rejected recording keeps its fence value on the ring slot
  (`vscreen::Ring::retire_unpublished`) so the slot is rewritten only after that GPU work completed. A non-ps2_v draw anywhere or a BRIEF3D map
  draw holds UI3D off for 1 s (`kUiHoldMs`; the seam skips without a lock while > 250 ms remain). The screen placement comes from ScreenCore
  (`vscreen::publish_placement`, the head position it was placed from = the anchor); a frame opens only when its generation matches the poses.
  Upload: one 512 KiB span per slot after the BRIEF3D spans (`kUploadTotal` 2.1 MiB). Published UI3D slots carry `SlotMeta::opaque`.
* `ui3d_replay_hooks.inl` (`ui3dDraw`, called in hookedDrawInstanced and hookedDrawIndexedInstanced right before the game's draw): facts under
  the hook-state mutex (PSO, step-3 RS/OM track, RT0 identity/size/format from the step-3 view registry, VB view 0 + the draw's vertices read
  twice through `step3MappedSpan_locked`/`guardedCpuRead`, cb0 root constants or root CBV), then per plan: barrier COMMON->RT, [clear], per
  visible eye OMSet(eye RT[, eye DSV]) / viewport / scissor / IASetVertexBuffers(0, eye copy) / DrawInstanced(n, 1, 0, startInstance), restore
  VB 0 / viewport / scissor / OM, barrier RT->COMMON. Off: one relaxed load.
* Consumer: `TableImages::opaque` + `ProjectionLayer::table_opaque` -> `compose_layers` submits the projection without SOURCE_ALPHA; the blit
  shader (`xr_brief3d.hlsl`, root constant b0) writes `srgb_eotf(rgb), 1` for an opaque slot (the menu PSOs blend alpha ZERO/ONE: the eye alpha
  stays 0 [E]). Stale > 500 ms -> no layer -> the flat screen (unchanged TableCore rule).
* Options/logs: `xr_stereo_proxy.cpp` parses ui3d/u3* after brief3d and logs `xr-stereo;event=ui3d-option;ok=1;ui3d=1;u3n=1.80;u3f=3.50;
  u3t=0.0;u3c=0;prerequisites=ok` once (prerequisites also need ee_camera_probe for the present hook); `summary-ui3d;...` (+ `;replays;published;
  rejected;refused-draws;no-pose;held;last-refusal`) every 5 s window in ee-camera.log next to `summary-brief3d`.
* Tests: `ui3d_offline` (census fixtures, eye scissor), `brief3d_live_offline` (UI3D frame publish, reject after replays with retained fence
  value, every refusal rejecting the frame, table/UI3D interplay, upload budget, culling, cancelled clear), `brief3d_table_offline` (opaque
  slot -> opaque layer + opaque blit, per slot).

Design (stage-1 text):

1. **Census first - IMPLEMENTED 2026-10-29 (read-only, file-armed, no config key)**: `src/acz-vr/ui3d_census.h` (pure sampling/format core, ctest
   `ui3d_census_offline`) + `ui3d_census_hooks.inl` (D3D12 glue, called from `observeDraw` next to `b3capDraw_locked`, hook-state mutex held, leaf
   mutex inside) + the Present-thread poll/emit in `ee_camera_probe_win.cpp`. Offline only: the DLL was not built and nothing ran in the game.
   * **Arm**: create `%LOCALAPPDATA%\acz-vr\ui3d-census.arm` (any content; polled once per 5 s present-statistics window), then enter a menu screen.
     Disarm/re-arm: delete the file (wait one window), create it again; a re-arm resets the sample/line counters and the latch. Not armed: the draw hook
     pays one relaxed load, no allocation, no GPU work, nothing written into command lists or game memory.
   * **Sampling**: one frame per second (the draws between two presents), at most 60 sampled frames and 6000 log lines per arm, then it latches off
     with the last line `event=ui3d-census;latched;reason=samples|lines;samples=<n>;lines=<n>;rearm=...`. A frame without any draw is not counted.
   * **Sink**: the same as briefing-observe: `ee-camera.log` (`%LOCALAPPDATA%\acz-vr\logs`) and the diagnostics log. Per sampled frame, in order:
     * `event=ui3d-census;kind=frame;s=<sample>;draws=<all draws>;menu=<ps2_v menu-key draws>;logged=<min(menu,48)>;indexed=<menu draws that were indexed>;menu-units=<summed counts>;other=<draws of other keys>;other-keys=<distinct>;overflow=<keys beyond 32>;no-pso=<draws with an unknown PSO>`
     * up to 48 x `event=ui3d-census;kind=draw;s=;ord=<menu ordinal>;all=<ordinal among all draws>;idx=<0|1>;n=<count>;inst=;st=<start>;rt=0x<res>/v<RTV view fmt>/r<resource fmt>/<w>x<h>;pso-rtv0=<PSO RTV0 fmt>;dsv=<PSO dsv>/view<bound DSV view fmt>;vp=<x,y,w,h>/<count>;sc=<l,t,r,b>/<count>;bl=<enable>:<src,dst,op,src-a,dst-a,op-a>;dp=<enable>/<func>/<write>/stc<stencil>;vb=0x<va>,<size>,<stride>,<mapped-upload|reason>;v=<x,y,z,q>|... /nv<read>;cb0=rc<root>:<d0,d1,d2,d3>|cbv<root>@0x<va>:<..>;bbox=<x0,y0,x1,y1 px>;z=<min,max>;q=<min,max>`
       (formats are D3D12 DXGI_FORMAT numbers; up to 8 vertices are read, 4 printed; `cb0=rc` = root constants, `cbv` = a root CBV read through the mapped span;
       a b0 in a descriptor table logs `cb0=?(u3c-cb0-in-descriptor-table)`)
     * `event=ui3d-census;kind=other;s=;keys=<n>;top=<vs8/ps8/t<topo><i|n><g|->/draws/units/max[/name]>,...` (top 8 other keys by draws; names SelMap/ModelIcon/ACM/CPT/ps2/movie)
   * **Fail closed**: any unreadable field prints `?` (viewport, scissor, RT and DSV view need the step-3 RS/OM track, i.e. `gpu_copy_step3_experiment` on, as the
     BRIEF3D producer does; vertices need a mapped UPLOAD VB via `step3MappedSpan_locked` + `guardedCpuRead`; cb0 needs the tracked root). `bbox` is in pixels of
     viewport 0 from the ps2_v mapping (`ndc = ((x-ox)*2/sx, (y-oy)*2/sy)/q`, section 1) and is `?` unless cb0 (all 4 dwords written) and the viewport are known.
   * Answers (to read from the log): UI target(s) per screen, the viewport (pillarbox?), cb0, q == 1?, VB readability, PSO formats (vs BRIEF3D's eye images
     RTV 10 / DSV 20), whether the bbox thresholds split backdrop/panels/text; the `other` lines show movie / ACM / SelMap frames.
2. **Seam**: `hookedDrawInstanced` (d3d12_hook.cpp:2305): `ui3dDraw(self, count, instances, start, startInstance)` right before
   `fn(...)`, one relaxed load when off (the `brief3dDraw` pattern; ps2 menu draws are non-indexed).
3. **Producer**: generalise `brief3d_live.h Producer` to carry a second plan kind (UI3D VB-copy replay) in the SAME frame/slot/list lifetime
   (invariants 1-11 of brief3d.md 8 unchanged: one list per frame, clear in the first plan, reservations, publish after Reset, one queue).
   Upload span per slot + `kUploadPerSlot`. A refused frame is aborted (nothing published). Present close: the existing `brief3d_present`.
4. **Consumer**: `TableCore` unchanged except a per-slot `opaque` flag -> `ProjectionLayer::table_opaque` -> `compose_layers`
   (xr_stereo_output.cpp:596) submits the projection WITHOUT SOURCE_ALPHA, fully covering the flat quad (the menus' surroundings are black
   today anyway, vsearly N4). Stale > 500 ms -> dropped -> the flat screen (unchanged rule).
5. **Briefing overview** (stage 2b): replay both kinds into the same eye images (panels on layers, map on the table): the classifier then admits
   SelMap/ModelIcon draws as "owned by BRIEF3D" instead of refusing.

GPU/CPU cost (estimate, not measured): per menu frame ~350 draws x 2 eyes x ~6 API calls + restores (~4,200 calls, ~0.3-0.7 ms game-thread CPU),
512 KiB upload writes, pixel work ~2 FP16 1440x1440 fills + text overdraw: negligible on the RTX 4090. Memory: the BRIEF3D slots (192 MiB) are
shared; none extra besides the upload span. No cost in flight (the seam returns on one load when no menu frame is open).

## 5. Live check list (null driver; after stage 2; nothing ran)
Config: the day install + `;ui3d:1` (with `vscreen:1;vsearly:1`). SteamVR first (vsearly). Route: launch -> main menu (20 s) -> CAMPAIGN -> Continue ->
(skip cinematics) -> overview (table) -> back -> mission menu (20 s) -> SAVE/LOAD (cancel) -> START MISSION -> aircraft select (20 s) -> flight.
1. `xr-stereo.log`: `ui3d-option;ok=1;ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;prerequisites=ok` once; no `terrain-disabled`.
2. Main menu: `summary-ui3d` (ee-camera.log) every 5 s with `usable` ~ presents, `refused` 0, `far/mid/near` all > 0 (census expectation:
   far 5 per frame, near the glyphs), `culled` 0 when looking ahead, `published` growing, `held=0`; the table consumer line
   (`summary-brief3d-table`, xr-stereo.log) `blits` growing, `stale` 0; `summary-vscreen;mode=screen` unchanged; present-stats fps 60.
   If `refused` grows with `last-why=ui3d-stencil-not-always` (or `ui3d-viewport-unknown`: the step-3 track is off) the menus stay flat: report it.
3. Mirror/`png-screen.now` (eye0-table PNG): the menu image, backdrop/panels/text aligned when the head is at the placement point; the two eye
   PNGs differ by a larger horizontal shift for text than for the backdrop. Headset: text floats in front of the red backdrop, readable, no seams.
4. Movie screens: `refused` with `last-why=ui3d-frame-has-movie`, the flat screen shows the movie (no flicker between 3D and flat: one slot
   stale -> 500 ms rule).
5. Overview: `ui3d-frame-has-3d` (stage 2) and the BRIEF3D table exactly as before.
6. Aircraft select: `ui3d-frame-has-3d` (ACM), flat screen.
7. Turn the head 90 degrees: `culled` grows, no smearing; recentre: layers re-placed with the screen.
8. Flight: `summary-ui3d` frames stop growing; hooks cost unchanged; exit clean (`retire;outcome=clean`).
9. Census lines (step 4.1) archived before stage 2 is enabled: thresholds re-checked against real bboxes.

## 6. Open questions - answered by the first live census (2026-10-29)

Source: step125 null run D1 (22:48-22:57, m03k route: title -> main/campaign menus -> cinematic -> briefing overview -> mission menu -> flight),
`D:\acz-evidence-archive\step124\runD1\logs\ee-camera.log`, `event=ui3d-census` (2510 lines, 60 sampled frames, latched `reason=samples`
at line 2575). Only the first 48 menu draws of each sampled frame are logged (`logged=48`), so the bbox facts below cover each frame's prefix.
Screens by sample (from the frame/other lines): s=1-2 title (9 draws), s=3-40 main/campaign menu (347 draws every frame), s=41-42
transition (191/199), s=43-47 black/fade (3), s=48-50 cinematic (movie), s=51-57 briefing overview (SelMap/ModelIcon), s=58-60 mission menu (144-149).

1. **cb0 [E]**: root constants, root parameter 0, all 4 dwords written: `cb0=rc0:682,-448,2048,2048` on every main/title/mission-menu draw
   (e.g. line 38: `kind=draw;s=3;ord=1;...;cb0=rc0:682,-448,2048,2048`). sx = 682 = 512 x 4/3: the 512-unit GS screen spans 1920 of the
   2560 pixels, i.e. **the 4:3 pillarbox comes from cb0, not from the viewport**. Other values on the same target: `2560,-1440,1280,720`
   (pixel space: the overview's indexed GS-line draws, line 2121), `2,-2,0,0` (the cinematic's full-image NDC quad, line 2055), `852,-448,...`
   (one fade on s=50). **q = 1** on every logged vertex [E] (`q=1,1`). The UI3D glue reads cb0 per draw (root constants or a root CBV).
2. **UI target [E]**: one intermediate target on every screen: `rt=0x25613d6ffc0/v0/r10/2560x1440` (R16G16B16A16_FLOAT), PSO `pso-rtv0=10`,
   `dsv=20/view19` (D32_FLOAT_S8X24 with an R32G8X24_TYPELESS view), viewport `0,0,2560,1440/1`, scissor `320,0,2240,1440/1` (the 4:3 area),
   depth on with func ALWAYS and no write (`dp=1/8/0/stc1`; fades `1/8/1`), stencil enabled (its func is not logged: [U], the producer requires
   ALWAYS on both faces). The final pass is ONE ps2_v draw per frame into the two alternating swapchain buffers `0x255c6bb29f0` /
   `0x255c6bb20a0` (`v24/r24/2405x1353`, R10G10B10A2, blend off, depth off, cb0 `2560,-1440,1408,976`, line 26). => the BRIEF3D eye images
   (RTV 10 / DSV 20) match the menu PSOs without new formats; the shown image IS the UI target scaled, so SourceRect = viewport / RT = {0,0,1,1} [I].
3. **VB [E]**: `vb=...,256,64,mapped-upload` on every non-indexed menu draw: a CPU-readable mapped UPLOAD span, stride 64, 4 vertices (one quad).
   Indexed ps2 draws exist only on the overview (`idx=1`, 6-16 per frame, n = 4-59, `u3c-vb-not-checked`): they refuse a UI3D frame.
4. **Thresholds [E]** (bboxes as fractions of the 2560 x 1440 image; scratch analysis of all logged draws on the UI target): backdrop quads
   0.7507 x 1.0 (5 per menu frame, z 64); panel strips/tiles 0.023 x 0.36..0.60, 0.047 x 0.071, the title panel 0.29 x 0.14 (z 64);
   **glyphs 0.015 x 0.0536 (main/mission menus) and 0.009 x 0.036 (overview), z = 1** (line 66: `v=1824,1902,1,1|...;z=1,1`); decorations
   0.0235 x 0.0357 and 0.0235 x 0.002 (z 64); 14+ **zero-area** degenerate quads per main-menu frame (`h=0`, z 64). Verdict: the stage-1 rule
   did NOT split them: (a) kSmallDim 0.05 made every main/mission-menu glyph (h 0.0536) a panel; (b) in the main menu the z-64 decoration
   pieces right after the first strips promoted the frame to the text layer, so the panel tiles and the title went near too (the layer never
   decreases). Changed in `ui3d.h` (ctest `ui3d_offline`, census fixtures `test_census_fixtures`): **kSmallDim = 0.06**; the classifier uses the
   draw's **GS z** (the game's own 2D priority: glyphs z 1, everything else z 64 [E]): a small draw whose z differs from the last large/medium
   draw's z is text (near), one with the same z is a decoration of the current layer; zero-area draws keep the current layer. Result on the
   census prefixes: main menu = backdrop far (5), strips/pieces/tiles/title mid, glyphs near; mission menu = backdrop + its 2 thin lines far,
   tile mid, glyphs near [I: only the 48-draw prefixes are logged; the remaining ~300 main-menu draws are unseen].
5. **Other keys [E]** (the `kind=other` lines): main/title/mission menus: none (`other=0`); s=48-50: `15c8c0bd/f9027c72/t3n-/1/4/4/movie` (one
   movie_yuv draw per frame, line 2057) -> `ui3d-frame-has-movie`; s=51-57: `384ade02/.../SelMap` + `d5a41c2f/.../ModelIcon` (10 per frame,
   line 2119) -> held off for the BRIEF3D table. The census route did not reach aircraft select (ACM), the hangar, results or debrief: those
   stay [U] (questions 5/6 of the stage-1 list), and UI3D refuses any non-ps2_v draw there (flat screen).
6. Still [U]: the stencil functions of the menu PSOs (if not ALWAYS everything refuses `ui3d-stencil-not-always`: the null run tells); whether
   the game clears the UI target each frame (UI3D clears its eye images to transparent black; a game that accumulates over the previous frame
   would differ); ClearRenderTargetView / copies into the UI target mid-frame (not observed by the draw seam).

## Note 2026-10-30: first live null evidence - the menu angular size is correct (offline audit, no code change)
Evidence: step124 runH1, `acz-vr-xr-screen-t15957-eye0-table.png` (2400x1350) vs `-screen.png` (flat). The eye-0 table image is the FULL eye
render (swapchain 2400x1350), not a table-only target: the null driver reports eye fov -45/+45/+45/-45 deg (tan span 2.0 x 2.0, so 90x90 deg
mapped onto a 16:9 image, non-square pixels). vscreen is 3.2x1.8 m at 2.5 m = tan span 1.28 x 0.72 (65 x 40 deg). Expected in the eye image:
full screen 1.28/2.0 = 64% of the width, 0.72/2.0 = 36% of the height; the game's 4:3 pillarbox content is 75% of the screen -> 48% of the
width. Measured (display px of 2000x1125): menu 529..1489 = 960 = 48.0% wide, 360..765 = 405 = 36.0% high; the inner panel border sits at 5%
of the content width in both images. So the layers reproduce the vscreen angles exactly (u3n 1.8 / u3f 3.5 m all subtend the same angles from
the anchor; section 3.5 tests). The "24%" reading is not supported; the menu looks small only because the null FOV is 90 deg and the pillarbox
content is 48% of it (about 51 deg wide, 2*atan(0.48)) in a ~100 deg real eye, identical to the flat screen. Making menus larger is a vscreen
sizing choice (`vsw`/`vsd`), not a UI3D scale bug. No fix, no new test.

## 7. Stage 3 (2026-10-30): the in-flight pause menu in 3D (3a, IMPLEMENTED behind `u3p:1`, NOT run live) + census/design for the other screens (3b)

Nothing of this section ran in the game, SteamVR or a headset. Tags as above ([E] evidence, [I] inference, [U] unknown).

### 7.1 Which draws make up the pause menu, and on which target (evidence)
* **The flight HUD target IS the menu UI target [E].** step124/runD1 `step5-producer.log` `event=hud-ring;outcome=ready;...;format=10;ds-format=20;
  size=2560x1440;scene=25613d6ffc0` (the HUD replay's scene target) and the same run's census `rt=0x25613d6ffc0/v0/r10/2560x1440` on 2430 menu
  draw lines (main / mission menus). So there is ONE route: the pause menu is drawn into the same R16G16B16A16_FLOAT 2560x1440 target the UI3D
  stage-2 replay already handles; the BRIEF3D eye images (RTV 10 / DSV 20) match without new formats.
* **Position in the frame [E].** step124/runA1 (hud-menu enter at `103311796`, `hud-draws=109`): `hud-end` frame 3013 `draws-seen=120;
  post-excursion-draws=65;excursions=5` vs every flight frame `62 / 7 / 3` (frame 2713). `hud-end-draws` stencil string: the flight HUD's 62
  draws (radar group `NAEEEE` at 57-62) unchanged, then `AAAAAA` (4 skipped `late`, 2 replayed), `EEEEEE` (6 stencil-EQUAL, replayed through the
  radar rule), then 46 stencil-ALWAYS draws. The menu is the TAIL of the HUD region, after the radar group; the HUD replay copies it into the HUD
  image (hud-menu.md 2), shown flat at menud 2 m with the alpha floor.
* **Shader / geometry [I/U].** The HUD replay keeps no VS/PS or vertex log per draw, and the census (4.1) latched after 60 samples before flight in
  runD1 (`latched;reason=samples`, line 2575): no pause frame was ever sampled. The pause draws are ps2_v (534f29ca / d34ee153, the only HUD/menu
  key) [I: runA1 HUD candidates `d34ee153:-:vs534f29ca:always`] with the flight cb0 (512, -448, 2048, 2048) [E fx_sprite_replay.h:6]. Their sizes,
  z values and the opener are [U] until the TAIL census (7.4) runs.

### 7.2 Route chosen
The UI3D producer (same ring/slot/list/fence lifetime, same vertex rewrite, same glue seam) replays the menu WINDOW of a paused flight frame per
eye onto the three depth layers; the consumer BLENDS that slot into the flight eye images after the cockpit/world composite, so the menu sits over
the dimmed cockpit inside the projection layer (no new swapchain: world-composite mode has no free quad chain, step124 `quad=none,...,images=0`).
* **Trigger** (`ui3d_pause.h` Signal/Gate): Output::hud_menu_frame publishes `menu_on_` (the existing hudmenu::Detector) every fresh game frame;
  the producer latches it at each Present; a heartbeat older than 250 ms turns pause off by itself.
* **Window** (`Producer::ui3d_draw`, `Run::pause`): with the trigger live and no frame open, every draw (ACM/CPT/effects/any shader, HUD ps2
  quads, the radar's EQUAL draws) is `ui3d-pause-prefix`: not replayed, no refusal, no UI3D hold. The frame opens at the first ps2_v draw with
  readable geometry whose bbox area >= 0.30 on a usable target (the game's dimming panel [I]) -> layer 0 (far); then the stage-2 classifier
  (panels mid, z-different small draws = text near) and every stage-2 refusal (stencil not ALWAYS, indexed, unknown facts ...) REJECTS the frame.
  A non-ps2_v draw inside the window closes it (`ui3d-pause-window-closed`); a later menu draw on the target rejects (`ui3d-pause-draw-after-3d`);
  other targets (the final pass) are skipped. The frame kind is the frame's: an open pause frame keeps its rules until its Present.
* **Placement** (`ui3d::pause::place`): the game lens' angular size (tan 0.61517 x 0.34604 [E runA1 lens-tan]) on a plane 2 m ahead of the mid-eye
  point at the head yaw, latched while the pause goes on (re-latched after a 2 s gap or a LOCAL generation change). The layers sit at u3n/u3f
  (default 1.8 / 3.5 m, the stage-2 knobs); from the anchor the composite has exactly the hud-menu angles.
* **Consumer** (`PauseCore`, xr_brief3d_core.h; `D3D12TableGpu` mode 2, xr_brief3d.hlsl): in Output's flight composite, after the GPU completion
  was observed and before the eye images are released, the newest READY pause slot (SlotMeta::pause, same generation, < 500 ms old) is drawn
  full-screen into both acquired eye images with blend ONE / INV_SRC_ALPHA: rgb = the menu (display values over cleared black, decoded once),
  alpha = max(menub floor, max(rgb)) -> text covers, the dark panel dims the cockpit/world by `menub` (default 0.6), as hud-menu does. The shown
  slot stays HELD (re-composites blend it again); a newer one replaces it and the old one is released after its last blend completed; a Signal
  failure poisons (Output fails), a lost fence releases nothing. With a blend submitted the HUD quad is dropped from the plan (it carries the flat
  menu). No pause slot / stale / any refusal = the HUD quad stays: exactly today's hud-menu image (fail closed).
* **Fix round 2026-10-30 (review W13-1; offline tests only, NOT run live)** - the consumer is now fail-closed on three more counts:
  * **Eye match (F1) - choice: MATCH-OR-FALL-BACK, no reprojection.** The slot's pixels are projected with the eyes (pose + FOV) the producer
    rendered with (SlotMeta::eyes); the blend writes them at unchanged UV into the flight eye images, which the compositor shows with the
    composite's eyes (`Output::composite_views`, the layer_from selection, passed as `pause_frame(..., eyes)`). `pause_eye_match` accepts only
    position <= 0.5 mm per axis, orientation <= ~0.01 degree and FOV angle <= 1e-4 rad per edge, per eye; any other case (head moved between
    producer capture and XR composite, DECOUPLE views, a different lens FOV, eyes unknown) = no blend and the HUD quad stays (today's hud-menu image;
    summary field `view-mismatch`). A real reprojection would need the layer depths (translation parallax) and was not attempted; consequence [I]:
    the 3D menu shows only while slot and composite eyes agree, a moving head will mostly fall back to the flat menu - the live check decides if
    that is acceptable.
  * **Publication age (F2).** The < 500 ms rule now uses `SlotMeta::published_ms` on the producer's clock (`TableSource::clock_ms`; DLL:
    GetTickCount64, the clock of `on_present`), also for re-blends of the held slot; an unset or future (> 50 ms) stamp is invalid; a too-old slot
    is released unused (`stale-pub`) and never replaces the current one.
  * **Partial setup (F3).** `PauseCore::retire` destroys a setup that was attempted (also a failed/partial one), once, like TableCore.
  * **Pause PSO isolation (F4).** `D3D12TableGpu` creates the pause blend PSO only when constructed for PauseCore (`u3p:1`); the TableCore
    instance (brief3d table + UI3D stage 2) never creates it, so a pause-PSO failure can only kill the pause path (its own instance). A pause blit
    without that PSO records nothing.
* **Cost** [I]: while paused ~120 draws/frame take the lock (the prefix reads ps2 VB/cb0 for the opener test), ~50 menu draws x 2 eyes replayed,
  one full-screen blend per eye per XR frame. `u3p` absent: no change (the seam bypass needs the live trigger, which needs u3p:1).

**Option string** (inside `xr_terrain_stage`, no new key): the day value + `;ui3d:1;u3p:1` (with `vscreen:1;vsearly:1` already in it, a model
cockpit with menu mode on (`menu:1`, the default) and `xr_hud_layer`). Log: `ui3d-option;ok=1;ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3p=1;
prerequisites=ok`. Without `u3p:1` every stage-2 line is byte-identical.

Code map: `ui3d_pause.h` (pure), `ui3d.h` (`u3p`), `brief3d_live.h` (pause frames, `pause-*` summary fields), `ui3d_replay_hooks.inl` (hold
bypass while the trigger is live: the only producer glue change), `xr_vscreen.h` (`pause_frame` / `pause_idle`), `xr_brief3d_core.h`
(`PauseCore`, `Brief3dHook` forwarding), `xr_brief3d_d3d12.cpp` (blend PSO, mode bits, a second D3D12TableGpu for PauseCore when u3p:1),
`xr_brief3d.hlsl` (mode 2), `xr_stereo_output.cpp` (publish; the blend before `release_images`; HUD quad dropped). D3D12 glue UNTESTED LIVE.

### 7.3 Tests
`ui3d_pause_offline` (tests/acz-vr/ui3d_pause_offline_tests.cpp): option + buffer fit, trigger gate (stale heartbeat), placement (lens angles from
the anchor, yaw, head-down refusal), blend bits / CPU mirror of the shader, producer: u3p:0 unchanged, a paused frame published with
meta.pause / not opaque and far/mid/near, the prefix (ACM, CPT, HUD quads, stencil-EQUAL) never refusing, latched / re-latched placement, the
window closed by a non-ps2 draw then rejected, a refusal inside the window rejecting, a stale trigger falling back to the stage-2 hold; PauseCore:
no slot, blend with floor, re-blend while held, replacement with release after completion, allocator busy, stale, non-pause / other generation,
Signal failure (poison, held for ever), fence loss, setup failure, Brief3dHook forwarding; census tail ring. `ui3d_offline`, `ui3d_census_offline`,
`brief3d_live_offline`, `brief3d_table_offline` unchanged and passing.

### 7.4 Diagnostics for the next null run (file-armed, no config key)
* **Pause menu**: create `%LOCALAPPDATA%\acz-vr\ui3d-census-tail.arm` (arms the census by itself) right before pausing in flight: each sampled
  frame logs its LAST 48 ps2_v menu-key draws (`kind=frame;...;mode=tail`, draw lines oldest first). In a paused frame that is the menu window
  (+ the end of the HUD): opener bbox / z, panel and text sizes, the EQUAL group, cb0, q. Remove it to stop (60 samples max per arm).
* **Hangar / aircraft select / results / debrief**: `ui3d-census.arm` already samples ANY screen (one frame per second, 60 per arm). It latched
  before those screens in runD1, so the driver must RE-ARM per screen: delete the file, wait one 5 s window, create it again on entering the
  screen (a re-arm resets the latch). The `kind=other` line gives the 3D keys per frame (ACM counts = the aircraft model, CPT, SelMap, movie),
  the `kind=draw` lines the 2D overlays (targets, cb0, sizes). Not covered: the ACM SrtData records (matrices), the first step of 3b below.

### 7.5 Design (not implemented): the aircraft-select model as a real 3D model in front of the user (stage 3b) - see section 8: the K9 census shows the model is NOT a separable class (the hangar shell is DrawACM_v too)
* Facts: after START MISSION the frame carries `b569109e/c1b58903/t3i-` DrawACM_v, 130-149 draws per present, max 2958 indices, plus ps2 quads
  (R4, section 1) [E]; flight already substitutes DrawACM_v per eye (gpu_copy_step5_world.h: SrtData +0 = the model->GS matrix A, +256 the
  translation) [E]; outside a DrawCPT frame those draws are refused today (109,737 pre-mission refusals, world1-impl.md 4) [E].
* Route: a THIRD frame kind in the brief3d producer ("model"): it opens at the first DrawACM_v of a frame with no CPT / SelMap / movie draw, while
  the frame's other draws are ps2_v (overlays: replayed by UI3D in the same slot, or left flat). Per ACM draw: read the SrtData record (root SRV,
  read twice, like ModelIcon), split A = MV x P_ps2 with the ps2 projection known from the frame's constants (as BRIEF3D's eye matrices assume),
  latch at the first draw a pedestal transform T (game camera space -> LOCAL: the model's camera-space origin from MV's translation placed 1.0 m
  ahead of the head at its yaw, a uniform scale so the frame's record translations fit 0.6 m), and write per eye A_eye = MV x T x view(eye) x
  P_ps2(eye) (the `open_eyes` / `modelicon_record` machinery of brief3d) into that eye's record copy; replay the game's draw with the root SRV
  swapped (the BRIEF3D icon pattern), depth GREATER_EQUAL into the slot depth, published opaque like a menu slot.
* Unknowns to settle first (census + an ACM record capture: extend `brief3d-capture.arm` to log the first N DrawACM_v records of a sampled
  frame): whether all ACM draws share one camera (one MV per part or per model) [U], whether the record's lighting / reflection terms depend on the
  camera (shading per eye) [U], whether the hangar backdrop is ACM too (several objects: refuse or place both) [U], that screen's ps2 projection
  constants [U]. Fail closed: a record that does not decompose with the latched camera rejects the frame (the flat screen stays).
* Risk medium-high (new kind, matrix decomposition); gain: a walk-around aircraft at ~1 m. Results / debrief: no 3D content is known there [U];
  if the census shows only ps2_v they are already covered by stage 2 (layered menus).

### 7.6 Null check list (stage 3a; a build with `;ui3d:1;u3p:1`; nothing ran)
1. `xr-stereo.log`: `ui3d-option;ok=1;...;u3p=1;prerequisites=ok`; `hud-menu-config;menu=1`.
2. Menus: `summary-ui3d` as in section 5 plus `;pause-live=0;pause-frames=0;...` (no pause outside flight).
3. Flight 60 s unpaused: `pause-live=0`, `pause-frames` unchanged, `summary-ui3d-pause` `blends` 0, fps / hooks cost as before.
4. Pause (Start) 10 s, twice: `hud-menu;state=enter`, then `summary-ui3d` `pause-live=1`, `pause-frames` and `pause-published` growing ~60/s,
   `rejected` 0, far/mid/near > 0, `pause-placements` +1 per pause; `summary-ui3d-pause` (xr-stereo.log) `blends` growing at the XR rate,
   `stale` / `blit-failed` 0, `held` 1-2, `poisoned=0`. If `rejected` grows: note `last-refusal` (e.g. `ui3d-stencil-not-always`: the EQUAL
   group falls inside the window) - the pause then shows today's hud-menu image (the correct fallback) and the tail census decides the fix.
   If `pause-prefix` grows but `pause-frames` stays 0: no large opener (the 0.30 rule is wrong): read the tail census.
5. Mirror / eye PNG while paused: the menu over the dimmed cockpit, no flat HUD quad; text nearer than the panel (eye-pair shift). Resume: the HUD
   quad back within one frame, `pause-live=0` within 250 ms, no `ui3d pause blend` failure line.
6. Tail census (7.4) archived with the run; compare its opener / glyph sizes with 7.2.

## 8. Stage 3b evidence (2026-10-30, step129 null run K9; offline analysis, no code change)

Source: `D:\acz-evidence-archive\step124\runK9` (route main menu -> campaign -> cinematic -> briefing overview -> mission menu -> START MISSION ->
4 confirms -> deploy -> flight; `steps.txt`). `logs\ee-camera.log` `event=ui3d-census` (5814 lines, 14 arms of 5-10 samples, one frame per second),
`summary-ui3d` lines in the same log, the 20 XR dumps `logs\png` (`t<ms>` + 125,345,978 = the log tick: `xr-stereo.log` `png-screen-armed;t-ms=`),
`shots\0*.png`. `gameshots` / `mirror` start at 03:47:40 (flight only). Clock map [I, +-1 s]: log tick ~= 125,376,600 + seconds after 03:45:00
(START MISSION 03:45:40.1 ~= 125,416.7 s; confirms ~125,423.5 / 431.3 / 438.0; deploy ~125,446.9).

### 8.1 Screens per sampled window [E counts / keys; screen names from the dumps and steps.txt]
| arm (first tick, s) | samples | screen | menu draws (ps2_v) | 3D keys (`kind=other` top: VS/PS/topo/draws/units/max) |
|---|---|---|---|---|
| 1 (125348) | s1-2 / s3-5 | title / main menu (`01-mainmenu` 03:45:05.9) | 9 / 347 | none |
| 2-3 (125358, 125373) | 1-10 | main / campaign menu | 347 | none |
| 4 (125388) | s1 / s2-3 / s4-8 / s9-10 | menu / transition / fade / cinematic (`02-cinematic`) | 347 / 182-199 / 3 / 2 | movie `15c8c0bd` x1 |
| 5 (125403) | s1-3 / s4 / s5-10 | briefing overview (`03-overview`) / cut / mission menu (`04`, dumps t62742, t70887) | 275-286 (6-16 idx) / 1 / 144 | SelMap x1 + ModelIcon x10 / none / none |
| 6 (125418) | s1-7 | **aircraft select** (no dump: between t70887 = mission menu and t78979) | 65-430, 2-4 idx | **`b569109e/c1b58903/t3i-` ACM 132 draws, 24,474 idx, max 2958** |
| 6 | s8-10 | **special-weapon select, PLAYER** (dump t78979 = 125,424.96: "SP WEAPON NAME SAAM", missile on the hangar floor) | 227-269 | ACM 139 draws, 26,985 idx |
| 7 (125433) | s1-6 | **special-weapon select, WINGMAN** (dump t87147: "WINGMAN", XMAA) | 201-310 | ACM 145 draws, 29,292 idx |
| 7 | s7-10 | next hangar screen after confirm 3 (no dump; [U] deploy prompt) | 110-111, 4 idx | ACM 145, 29,885 idx |
| 8 (125448) | s1-4 / s5-10 | black after deploy (t103345) / mission load | 3 / 46-90, 17 idx | none / ACM 11 + `38cd1d17`, ps2 PS `4caf6e2d`, `9eff1f92` t1ng, `127819e3`, `a16573be` |
| 9 (125463) | 1-10 | mission intro fly-over (`05-loaded` = terrain) | 42-109 | as arm 8 + `f7ab4176/5af1f802` t3n- 1 draw, 1,902-4,482 vtx |
| 10 (125478) | s1-9 / s10 | pre-flight / flight starts (CPT appears; 03:46:53.9 "flight detected") | 48-102 | ACM 11-15 / + CPT `f4cfd928` 29 |
| 11-14 (125493+) | all | flight | 94-190 | CPT 29, ACM 15, ps2 HUD |

The mission menu's HANGAR entry (D-89) was not visited; in this route the aircraft select follows START MISSION.

### 8.2 What the hangar screens draw (aircraft select, both weapon selects) [E unless tagged]
* **One 3D key only**: DrawACM_v `b569109e` / PS `c1b58903`, triangle list, indexed, 132-145 draws per frame, max 2,958 indices per draw. No CPT,
  SelMap, ModelIcon, movie or other key. Every logged ps2_v draw of these frames is on the UI target `rt=0x1ba6cb7e550/v0/r10/2560x1440` (= the K9
  mission menu's target, ord 1 at 125407), cb0 `682,-448,2048,2048`, depth func ALWAYS without write (`dp=1/8/0`), stencil on.
* **The background IS DrawACM_v**: the hangar shell (walls, floor, carts; visible in t78979 / t87147) and the model share the key; there is no
  aircraft-only class. Draw order of an aircraft-select frame (s=1, `all=` of the logged menu draws): ACM ordinals 1-24, ps2 25-33, ACM 34-59,
  ps2 60-70, ACM 71-152 (82 draws), ps2 153+ (2D overlay). Weapon select (125424): ACM 1-24, ps2 25-44, ACM 45-70, ps2 71-96, ACM 97-178.
  The ACM count moves with the screen/camera (132 -> 139 -> 145) and cannot be split into "hangar" and "model" from the census: which draws
  are the aircraft is [U] (counts change with camera culling; the weapon screens show a missile and no aircraft).
* **Pre-projected 3D sprites between the ACM blocks**: 17 (aircraft select) to 42 (player weapon) 4-vertex ps2_v quads with REAL depth
  (z 3.4e4-2.0e5 in 2^-24 units, vs z 64 / z 1 for the 2D overlay), blend SRC_ALPHA/ONE (`bl=1:5,2,1`, additive), squares of 160-340 px in
  rows of 4 receding z values = the ceiling-light glows [I]; and 1-3 triangle draws (`n=3` non-indexed, `idx=1 n=6/9`) with blend
  SRC_ALPHA/ONE/REV_SUBTRACT (`bl=1:5,2,3`) = darkening, the model's floor shadow [I]. These vertices are already in screen space (the EE
  projected them with the menu camera): they cannot be re-rendered per eye by a camera substitution.
* **2D overlay**: 25-40 ps2_v quads `bl=1:5,6,1` (SRC_ALPHA/INV_SRC_ALPHA), z 64 panels / z 1 glyphs, as the menus (section 6.4).
* **The ACM draws' own facts were not logged** (the census logs only ps2_v draws in full; `kind=other` holds counts): target, PSO formats,
  depth function, cb10 flags and the SrtData records (A = world->GS, B = model->world, +360, +400) are [U]. That they write the UI target is [I]
  (interleaved by ordinal with depth-ALWAYS ps2 draws on it; the dumps show model + hangar in one image).
* **UI3D today**: every hangar frame is refused (`summary-ui3d` frames stay 3530 from 125,418 to 125,448; `held=1`; `last-refusal=
  ui3d-frame-has-3d`; `refused-draws` +6-7 per 5 s = one hold re-arm per second) -> the flat screen, as designed (fail closed). The mission-load /
  intro frames refuse with `ui3d-frame-has-other-shader` (frames 3719 constant from 125,453).

### 8.3 Verdict: GPU-transformed, but stage 3b is NOT implemented
* GPU-transformed: yes [E: the key is DrawACM_v, whose VS forms `h = (local,1) x B x A` on the GPU (gpu_copy_step5_world.h header)]. Per draw the
  flight machinery could substitute A per eye (`camera_setup`, `A'_e = A + D_e x S`) and BRIEF3D could swap the t10 root SRV.
* Not implementable fail-closed with today's evidence, for three reasons:
  1. **No model selection**: the aircraft is not a class; a rule that picks it out of 132-145 ACM draws (by B translation, by screen centre,
     by rotation over time) cannot be checked against any logged fact, and a wrong pick (a hangar wall chunk on the pedestal) is not detectable
     at run time, so it cannot fail closed.
  2. **Records unknown on this screen**: the flight `record_check` requires +400 = (512,-448,2048,2048) and `camera_setup` checks the cockpit
     camera; on the hangar the ps2 cb0 is (682,...) and there is no cockpit. Whether all ACM draws of a frame share one A (flight: bit-identical),
     the +400 values, cb10 dword 0 (fade bit) and +360 are [U].
  3. **Mixed content**: the scene also contains the pre-projected light glows and the shadow (8.2), which only a re-projection through the
     recovered camera could move (the fx_sprite_replay route), not a camera substitution.
* Concrete alternative (next steps, in order):
  1. **ACM record capture on the hangar screens (read-only, file-armed, no config key)**: extend `ui3d-census` with `kind=acm` lines on frames
     that carry ACM and no CPT: per ACM draw the `all=` ordinal, index count/start, target (`rt=`), PSO RTV/DSV formats, depth func/write, stencil
     funcs, cb10 dword 0, and the t10 record read twice through `step3MappedSpan_locked` + `guardedCpuRead` (the `brief3d_capture` reader):
     A equal-to-first (bit), A once per frame, B in full, +360, +400. Sample the aircraft select, both weapon selects and the HANGAR entry of the
     mission menu, two frames ~2 s apart per screen (turntable: does B or A change?).
  2. **Decide the selection from that capture**: the model = the B group (one rigid B, or Bs sharing one rotation) whose rotation changes
     between frames while the others stay (turntable) [hypothesis], or the group nearest the camera's look-at point; if A changes instead (the
     camera orbits), the pedestal uses B x (world -> pedestal latched at the first frame) and ignores the game camera, as BRIEF3D ignores its orbit.
     Then implement `u3a:1` as the brief3d third frame kind of 7.5 with `camera_setup` taking the frame's own +400 and no cockpit check, refusing
     any frame whose groups are ambiguous; the 2D overlay and glows stay on the flat screen (the slot is a projection layer over it, like the table).
  3. **Route B2 (no selection needed)**: the whole hangar as a 3D room at the screen: every ACM draw replayed per eye with the flight maths
     (`A'_e = A + D_e x S`, head offset at a fixed game-units-per-metre scale, camera latched per screen), the glow quads re-projected through the
     recovered camera, the 2D overlay on the UI3D layers. Needs the same capture (A shared, +400) plus the units scale.

### 8.4 ACM census (IMPLEMENTED 2026-10-30, offline tests only; NOT run in the game) - the read-only capture of 8.3 next step 1
* **Arming** (no config key): create `%LOCALAPPDATA%\acz-vr\ui3d-census-acm.arm` (on its own it arms the census; it combines with `ui3d-census.arm` /
  `ui3d-census-tail.arm`; remove and re-create to re-arm). Polled with the 5 s present window like the other arm files. Same sampling: one frame
  per second, at most 60 frames and 6000 lines per arm, then `event=ui3d-census;latched;...`. Not armed: the draw hook pays the same single relaxed
  load. The ps2_v (menu) lines of the frame stay in the output, so the `all=` ordinal ties both kinds together.
* **Per sampled frame** (after the `kind=frame` line, which gains `;acm=<ACM draws seen>;acm-logged=<min(seen,32)>`): the first 32 DrawACM_v draws
  (VS b569109e, any PS; later ones are only counted, they stay in `kind=other`), each as
  * `kind=acm;s;all;ord;idx;n;st;base;inst;ibs;rt;pso-rtv0;dsv;bl;dp;cb10;t10;k360;k400;A`: `all` = the frame draw ordinal shared with the ps2_v
    `kind=draw` lines; `ord` = ps2_v draws before it; `idx/n/st/base` = indexed flag, index count, StartIndexLocation, BaseVertexLocation; `ibs` = index
    stride (0 unknown); `rt=0x<res>/v<view fmt>/r<res fmt>/<w>x<h>`, `pso-rtv0`, `dsv=<pso fmt>/view<fmt>`, `bl=<enable>:<src,dst,op,srcA,dstA,opA>`,
    `dp=<enable>/<func>/<write>/stc<stencil>` (same decoding as the ps2_v lines); `cb10=rc<root>:<w0>,<w1>,<w2>,<w3>` hex root-constant dwords
    (`?` for a dword never written; `cb10=?(reason)` otherwise; dword 0 is the flags word whose bit 8 is the fade branch); `t10=0x<va>/read` then
    `k360` (depth scale), `k400` (the four screen constants, flight value 512,-448,2048,2048), and `A=first|same|differs` (bitwise against the first
    ACM draw's A of the frame; `?` when either record was unreadable). A failed record prints `t10=0x<va>(reason);k360=?;k400=?;A=?`.
  * `kind=acm-b;s;all;B=<16 floats>`: B (+256, model -> world) in full, every logged draw, row-major as stored, %.9g.
  * `kind=acm-a;s;all;A=<16 floats>`: A (+0, world -> PS2 screen) in full, for the first logged ACM draw only.
  * a draw admitted but not yet committed at the present prints `kind=acm;s;ord=<n>;incomplete=admitted-not-committed` and no matrices.
* **Reads**: only the pinned mapped UPLOAD span (`step3MappedSpan_locked`) + two `guardedCpuRead` copies of the 416-byte record that must agree
  (`u3c-t10-record-torn` otherwise); t10 = the root SRV found by `root->resolve(srv,10,0,vertex)` (descriptor table = `?(u3c-t10-in-descriptor-table)`);
  cb10 = the tracked root constants of `resolve(cbv,10,0,vertex)` (pixel stage as fallback). Nothing is recorded into a list, no native call, no GPU
  readback; unknown = `?(reason)`. Storage is fixed (32 records per frame); formatting runs on the Present thread outside the u3c lock.
* **Use**: arm, open the aircraft select / weapon selects / hangar, take two frames per screen (about 2 s apart: does B or A change between frames,
  is A shared, are k400/cb10 the flight values?), disarm. That settles 8.3 reasons 1 and 2. Tests: `ui3d_census_offline` (`test_acm_format`,
  `test_acm_sampling`); nothing here established any live behaviour.

### 8.5 ACM records of the hangar screens (step130 null run K10, 2026-10-30 04:13-04:22; offline analysis)
Source: `D:\acz-evidence-archive\step124\runK10\logs\ee-camera.log` (`ui3d-census;kind=acm|acm-a|acm-b|frame|draw|other`); the hangar frames are
the arms at log ticks 127,149,386-127,158,603 (s1-7 aircraft select, s8-10 PLAYER special weapon; XR dump t79569 = tick 127,156,722) and
127,164,420-127,173,685 (s1-6 WINGMAN special weapon, dump t87677; s7-10 the next hangar screen) = 20 frames x 32 logged ACM draws (of 132-145).
Tick map: `xr-stereo.log` `png-screen-armed;t-ms=3268` at 127,080,421 -> log tick = t + 127,077,153. Index contents unread (`?(u3c-indexed-unread)`).
* **Every hangar ACM draw [E, 640/640]:** RT `0x13e5d1b3750` 2560x1440 fmt 10 (the menu target), PSO RTV 10 / DSV 20 (view 19), blend
  SRC_ALPHA/INV_SRC_ALPHA (`bl=1:5,6,1`), depth GREATER (5) with write, stencil enabled (funcs not logged [U]), indexed, 1 instance,
  `cb10=rc3:0x2,0x0` (flags 2: no fade bit 8), `t10` a readable root SRV, `k360=1`, `k400=682,-448,2048,2048` (the menu cb0; the flight's 580 ACM
  lines carry 512, the 128x128 offscreen passes 128,-128: k400 identifies the hangar screens).
* **B = identity in all 640 logged draws [E]**: the hangar geometry is stored in world units; **A is bit-identical across a frame's logged draws
  [E: 620 `A=same`, 20 `A=first`]**. So A = the shared world -> PS2 screen matrix (view + projection), B the (here trivial) model matrix.
* **A is an exact pinhole camera in every frame [E, decomposition of the 20 `acm-a` lines]:** w column xyz unit length (1.000000), axes
  orthogonal (< 2e-7), right.y = 0 (no roll; world up +Y), right x up = -forward, lens tan 0.8325 x 0.4688 over the full 2560x1440 with the 682
  constants (aspect 1.776 = square pixels; the 4:3 scissor 320..2240 shows tan 0.624 x 0.469 = 64 x 50 degrees), depth column = b w + Z0 with
  b = -255.008 and Z0 = 16,777,470 (residual <= 4e-8; the flight's model: reversed depth, near ~1 unit, far ~65,536 units).
* **The turntable is the CAMERA [E]:** A changes every frame (aircraft select: the eye orbits from (-155,28,-65) to (-189,6,-2) over 6 s, forward
  yaw 35 -> 88 degrees, ~9 deg/s, pitch -12.5 -> +2.6 degrees, 170-189 units from the origin); B never changes. Screen changes are cuts (weapon
  select: the eye jumps to (-34,27,-31), 53 units from a look point near (-4,1,5); wingman: (170,28,-32); next screen: (-210,33,55)).
* **Model separability: NO [E/U].** All 32 logged draws per frame share A and B = identity; the aircraft is not a distinct B group in them, and
  draws 33+ (the ACM block after the second glow group, where the model most likely is [I]) were not logged [U]. A rule that picks the model
  cannot be checked: the preferred "model only" route is not provable; the ROOM route (whole scene, substituted camera) is (section 9).
* **Frame structure [E: 16 of 20 frames show the overlay within the 48 logged ps2_v draws; the other 4 (next screen) show only effects]:** the
  first draw of every hangar frame is ACM (`all=1`); every ACM draw precedes the first overlay draw; the ps2_v draws between the ACM blocks are
  quads with real GS z (4.5e4-2e5; additive `bl=1:5,2,1`, the light glows) or indexed / 3-vertex REV_SUBTRACT draws (`bl=1:5,2,3`, the floor
  shadow); the overlay (`all` >= 153..180) has z 0 / 1 / 64 (`bl=1:5,6,1`, `1:5,2,1`, and once `1:1,14,1` = a full-screen ZERO/BLEND_FACTOR dim at
  z 0). No effect draw after the first overlay draw, no ACM draw after it. ps2_v viewport 0,0,2560,1440, scissor 320,0,2240,1440 (all 960).
* **How k400 maps to the screen [E]:** ndc.x = (h.x/h.w - 2048)/341, ndc.y = (h.y/h.w - 2048)/(-224) over the WHOLE 2560x1440 target
  (pixel x = 1280 + (x_gs - 2048) * 3.754); the game's 4:3 picture is the scissored middle 1920 pixels (GS x 1792..2304), the vscreen shows that
  pillarbox. The 2D overlay uses the same constants (cb0), so overlay and room share one screen mapping.

## 9. Stage 3b (2026-10-30): the hangar screens as a 3D ROOM (IMPLEMENTED behind `u3a:1|2`; offline tests only, NOT run live)

Nothing of this section ran in the game, SteamVR or a headset. Route chosen from 8.5: the aircraft is not provably separable (B identity, A
shared), but the whole scene is one consistent pinhole camera, so the WHOLE hangar is replayed per eye with a substituted camera as a room around
the user, and the 2D overlay goes on the stage-2 layers over it.

### 9.1 Maths (`ui3d_hangar.h`, pure)
* `decompose(A, k400)`: the game camera (eye, right/up/forward, lens, depth model b/Z0) from A alone with the record's own k400; refuses a
  non-unit w column, non-orthogonal axes, a mirrored basis, a rolled camera (right.y != 0), a depth column that is not b w + Z0, odd lenses.
* The ROOM: hangar = E + upm * Rm (local - anchor). Rm maps the head's level frame at the latch (yaw only) onto the game camera's level frame
  (yaw only: the game's pitch is dropped so the floor is level; the user's own head pitch is the HMD's). anchor = the mid-eye point at the latch,
  E = the game camera eye: at the latch the user stands where the game camera is and faces where it faces.
* `eye_matrix`: A'_e in the shader's PS2 form for eye e (HMD position/orientation through the room, the eye's own asymmetric FOV, the record's
  k400 and the frame camera's b/Z0): w' = eye depth in hangar units, ndc = the eye's projection, depth = the game's reversed model. Offline: with
  the eye at the anchor looking with the game camera's pitch through the game lens, A' reproduces A to < 2e-4 NDC for all 8 real K10 cameras
  tested; for a moved head / asymmetric FOV / both eyes it equals `ui3d::project` of the LOCAL point.
* B and every other record byte stay the game's (the substitution is A only, so a non-identity B would still be correct).

### 9.2 Producer (`brief3d_live.h`: `Producer::ui3d_acm` + the hangar branch of `ui3d_draw`; same ring/slot/list/fence lifetime as stage 2)
* **Opening:** only at the FIRST draw reaching the UI3D core after a Present whose whole interval lies after any hold, when it is a DrawACM_v
  draw with a readable record carrying k400 = (682,-448,2048,2048), k360 1, affine B, cb10 dword 0 known without bit 8, indexed, 1 instance,
  triangles, PSO RTV 10 / DSV 20 / 1 sample, depth GREATER(_EQUAL), stencil off or ALWAYS, one viewport/scissor, restorable OM, a known fmt-10
  target, t10 a root SRV (param < 64), a pose, the vscreen placement of the same generation, a decomposable camera, a free slot. Without the
  hangar k400 (flight 512, offscreen 128) or with an unreadable record it is today's rule exactly (`ui3d-frame-has-3d` + hold).
* **Room latch** (`u3a:1`): latched at the first frame after a gap > 2 s / a LOCAL generation change (head anchor + yaw too), re-targeted at a
  camera CUT (eye jump > 8 units or forward change > 10 degrees between hangar frames: K10 drift is ~0.2 units/frame); between cuts the room is
  static and the game camera's orbit is ignored (the BRIEF3D precedent; comfortable, but the turntable motion is lost). `u3a:2`: re-targeted at
  every frame's camera (the orbit preserved: the room turns around the user at ~9 deg/s; vection risk).
* **Per ACM draw:** A must equal the frame's first A (rel 1e-6) and the target the frame's; the two eye copies (A -> A'_e) go 256-aligned into the
  slot's UI3D span (2 x 512 B per draw, <= 256 draws); plan = the BRIEF3D root-SRV swap (`brief3d::replay_commands`).
* **ps2_v draws of a hangar frame:** off-target = skipped; BEFORE the overlay, an indexed draw or a quad with GS z > 256 is a scene effect:
  **dropped** (not replayed; counted `u3a-fx`): the light glows and the floor shadow are pre-projected for the game camera and cannot be moved by a
  camera substitution (8.2). The first ps2_v draw with z <= 256 starts the overlay: from there the stage-2 rules (layers at the vscreen placement,
  painted over the room: depth ALWAYS). The full-screen dim (z 0, ZERO/BLEND_FACTOR) is part of the overlay: it darkens the room inside the
  far layer's rectangle (as the flat image darkens behind the menu) [I].
* **Fail closed:** ANY refusal inside a hangar frame (record/camera/cb10/PSO facts, A differing, another target/list, an ACM draw after the
  overlay, an effect after it, any stage-2 refusal, a non-ps2_v non-ACM draw, a draw after the list's Reset, no ACM replay at the Present)
  REJECTS the frame (never published; the stage-2 retirement rules) and holds UI3D for 1 s: the flat screen. No frame opens mid-way (first-draw rule).
* Published OPAQUE (`SlotMeta::opaque`): the TableCore shows it like a stage-2 menu slot, covering the flat screen (outside the hangar shell:
  the cleared black).
* Summary (`summary-ui3d`, u3a only): `;u3a-frames;u3a-published;u3a-rejected;u3a-acm;u3a-fx;u3a-latches;u3a-cuts;u3a-why`.

### 9.3 Glue (`ui3d_replay_hooks.inl`, UNTESTED LIVE)
`ui3dDraw` (now also given the BaseVertexLocation) routes a DrawACM_v draw to `u3AcmFacts_locked` (PSO/RS/OM facts, cb10 root constants vertex
then pixel stage, t10 root SRV + its 416-byte record read twice through `step3MappedSpan_locked` + `guardedCpuRead`) when `u3a` is configured,
then records barrier / [clears] / {OM eye RT+DSV, eye viewport+scissor (the whole eye image), root SRV t10 = eye copy, DrawIndexedInstanced(game
args)} x 2 / t10 restored / RS+OM restored / barrier, before the game's draw. A non-indexed ACM draw is refused by the core.

### 9.4 Tests
`ui3d_hangar_offline` (tests/acz-vr/ui3d_hangar_offline_tests.cpp): options (+ buffer fit), record checks, decomposition of 8 real K10 matrices
(lens, depth model, eye positions vs the log decomposition, mirrored / non-unit / rolled refusals), cuts (aircraft s7 -> weapon screen, wingman
-> next screen; no cut for the 1 s orbit step), A' = A for the game's own camera (3 head yaws), A' = the eye's LOCAL projection (2 scales, both
eyes, moved head, asymmetric FOV), the level room; producer: a K10-ordered frame (9 ACM, 4 effects dropped, 3 overlay layers) published opaque
with the exact eye matrices and the untouched record bytes, latch / cut / gap / follow mode, rejections (A differs, ACM after overlay, effect
after overlay, fade, stencil, depth func, non-indexed, torn record, DSV, stage-2 stencil, CPT in the frame), no opening (flight k400,
unreadable record, not first draw, mid-hold, no pose, other placement generation, pause window), effects-only frame, draw after reset.

### 9.5 Option string and null check list (a build with `;ui3d:1;u3a:1`; nothing ran)
Option (inside `xr_terrain_stage`, no new key): the day value + `;ui3d:1;u3a:1` (follow mode `;u3a:2`; scale `;u3au:<units/m>`, default 5.0 [I:
the special-weapon missiles measure ~4-5 units/m in the K10 dumps]). Log: `ui3d-option;ok=1;ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3a=1;
u3au=5.0;prerequisites=ok`. Without `u3a` every stage-2 / 3a line is byte-identical.
1. `xr-stereo.log`: the option line above; no `ui3d-hangar-list-natives` poison.
2. Main / mission menus: `summary-ui3d` as section 5 plus `;u3a-frames=0;...;u3a-why=none` (no ACM there).
3. Aircraft select and both special-weapon selects (~10 s each): `u3a-frames` and `u3a-published` growing ~60/s, `u3a-rejected` 0, `u3a-acm`
   +132..145 per frame, `u3a-fx` +10..46 per frame, `u3a-latches` +1 at the first screen, `u3a-cuts` +1 per screen change, `held=0`. If
   `u3a-rejected` grows, note `u3a-why`: `u3a-stencil-not-always` (stencil funcs [U]), `u3a-t10-not-root-srv`, `u3a-cb10-*`, `u3a-a-differs`
   (an unlogged ACM draw 33+ with another camera), `u3a-acm-after-overlay` / `u3a-fx-after-overlay` (structure), `ui3d-no-screen-placement`.
   Every refusal must show the flat screen (correct fallback).
4. XR dumps (`png-screen.now`) on each screen: `-eye0-table.png` shows the hangar around the screen-sized overlay: textures, the aircraft in front
   of the walls (depth), the overlay readable over the room, level floor, no mirrored geometry; the missile/aircraft scale plausible (else u3au).
   Turn the (null) head 90 degrees: geometry the game culled for its camera is missing (expected [I]).
5. Flight (60 s): `u3a-frames` unchanged, fps / hook cost as before (k400 512 = today's hold, one record read per renewal window).
6. Re-arm `ui3d-census-acm.arm` on one hangar screen and archive it (stencil funcs are still not logged; add them if (3) refuses on stencil).
7. Archive xr-stereo.log / ee-camera.log / dumps under `D:\acz-evidence-archive\step1xx\runXX`.

### 9.6 Risks / not proven
(a) Culling: the game culls for its own camera; looking away shows holes / black [I]. (b) Lighting/specular of the ACM PS might depend on
camera terms outside A [U]: shading stays the game camera's. (c) upm 5 is an estimate: a wrong scale feels like a giant/dwarf room (stereo is
weak at 30-40 m anyway). (d) The glows and floor shadow are missing in VR (dropped, screen-space). (e) Stencil funcs of the hangar ACM PSO are
unknown: if not ALWAYS, everything refuses (flat screen). (f) `u3a:2` vection from the 9 deg/s orbit. (g) Near plane ~1 unit (0.2 m at upm 5):
geometry within 20 cm of an eye is clipped. (h) The overlay layers float at 1.8-3.5 m while the room is at its real depth (tens of metres):
the overlay is always on top by construction, but can visually cut through near room geometry. (i) A rejected frame holds 1 s (flat):
transient refusals (no free slot) flicker to flat for 1 s.

### 9.7 First live null run (step131, run L11, 2026-10-30 04:48-04:52, `;ui3d:1;u3a:1`) and the fix (offline tests only)
Evidence: `D:\acz-evidence-archive\step124\runL11\logs`. [E] `ee-camera.log` `summary-ui3d` at 129,280,524: `u3a-frames=11;u3a-published=10;
u3a-why=none` (the room opened and published); at 129,285,541: `u3a-frames=24;u3a-published=23;u3a-rejected=1`, stage-2 `last-why=
u3a-fx-after-overlay`, then for the rest of the hangar visit `held=1;last-refusal=u3a-not-first-draw` (later `ui3d-frame-has-3d`) with
`refused-draws` +5..7 per 5 s and `u3a-frames` frozen at 24. `xr-stereo.log`: the screen dumps t78957 / t87050 / t95156 were WRITTEN; only the
eye dumps were skipped (`png-screen-armed;...;table=table-not-live`): no live slot, a consequence, not a separate defect.
* **Cause 1 (the lock-in) [E + source]:** one rejection held UI3D for 1 s. The game frame straddling the hold's end reached the core with its
  ACM draws after earlier draws of the same frame (the glue's renewal window lets draws through in the hold's last 250 ms), so its first ACM after
  the expiry failed `u3a-not-first-draw` through `fail()`, which RENEWED the hold: exactly one refusal per hold period, for ever.
* **Cause 2 (the trigger) [E/I]:** `u3a-fx-after-overlay`: a ps2_v draw after the overlay started was classed as an effect (GS z > 256 or
  indexed). The K10 census logged only the first 48 ps2_v draws per frame, so later overlay draws were never seen; which kind it was is [U].
* **Fix:** (a) the timing refusals of an opening ACM draw (hold active, not the first draw, the hold covered part of the frame) no longer call
  `fail()`: no hold renewal, no refusal count; they latch a per-frame SKIP (`u3a-skips`) so the frame's ps2_v draws never open a stage-2 menu
  frame or refuse (`u3a-frame-skipped`); the skip ends at the Present. (b) After the overlay started, a NON-indexed real-depth quad is replayed
  by the stage-2 rules as an overlay draw (`u3a-late`; the flat image paints it there too); an INDEXED ps2_v draw after the overlay still
  rejects the frame, now named `u3a-indexed-after-overlay` (vertices unread: fail closed, 1 s flat, then recovery).
* **Regression tests** (`ui3d_hangar_offline`, `test_l11_hold_recovers`, `test_l11_late_quad_publishes`): a 60 fps glue-faithful simulation
  (draws reach the core only while `!ui3d_held(now)`); after one rejection the first frame whose Present lies after the hold's end opens and
  every frame after it publishes, `refused-draws` grows by the rejection only, no stage-2 frame opens; the old rule fails this test.
  Summary now ends `...;u3a-cuts;u3a-skips;u3a-late;u3a-why`.
* **Next null run (same cfg):** aircraft select / weapon selects: `u3a-published` ~60/s for the whole visit; `u3a-skips` a few (screen
  entries, ~1 per rejection); `u3a-late` > 0 if cause 2 was a late quad (then `u3a-rejected` stays ~0); if `u3a-rejected` still grows with
  `u3a-why=u3a-indexed-after-overlay`, each costs 1 s flat and recovers (the overlay contains indexed draws: next step a census of the late
  overlay draws). Eye dumps on the hangar should then be `table=armed` with the room in `-eye0-table.png`.

### 9.7 u3au check (2026-10-30, offline): 5.0 units/metre CONFIRMED, no change
* Model extent [V, model-decode.md 1.3]: the hangar draws use B = identity, i.e. the same model-space units as the flight models: Draken (j35j) 76.7 u long / 15.35 m = 5.00, span 46.8 u / 9.42 m = 4.97 (MiG-21 5.00, F-16C 5.26). The 65 of `gpu_copy_step5_units_per_metre` is the flight terrain scale, not the model scale.
* Camera geometry [E, all 76 K10 `acm-a` records decomposed]: eye height 27 u = 5.4 m on aircraft/weapon screens (a hangar camera on a boom), weapon-select eye 53 u = 10.6 m from its look point (SAAM ~3.7 m ~ 18 u), aircraft-select orbit radius 170-189 u = 34-38 m about the origin, deploy-screen eye 220 u from the origin. Other values (1 u/m: 190 m orbit in a ~30 m shelter; 65 u/m: 0.4 m high camera) are implausible; 5 u/m gives a shelter-sized room. Deploy screen: aircraft ~38 % of the 4:3 picture (tan 0.47) with 76.7 u length gives 100-160 u = 20-32 m, consistent.
* The aircraft looks small in the eye dump because u3au does NOT set apparent size: the latch puts the eye at the game camera (E), so angular size = the game's (a 64 x 50 degree 4:3 picture) shown in a ~100 degree eye FOV, and 0.32 u IPD at 100+ u gives no stereo. u3au only scales head translation / IPD (larger = hyperstereo/miniature, smaller = giant). To make the aircraft feel closer, dolly the anchor along the camera forward (new option) rather than change u3au.
* Anchor: head at latch -> E (the game camera eye), yaw = camera yaw, level; so the user already stands at the game camera.

## 10. Note 2026-10-30 (afternoon): step133 headset feedback - hangar late-draw lock-out, menus that read as the flat screen, summary lines (offline tests only, NOT run live)

User words (real headset session, cfg `;ui3d:1;u3p:1;u3a:1`): "didnt see any 3d menus. Aircraft hanger loading was spotty. The menu and game render sat on the screen in front. Whereas youd want the hanger to load and just the ui to sit on front". Evidence: `D:\acz-evidence-archive\headset-2026-10-30-session\live-1144` (`ee-camera.log` carries 127 `summary-ui3d` windows, t=154,050 .. 154,681 s).

### 10.1 Causes [E = log, S = source]
* **Hangar spotty [E+S]:** the summary shows the room opening and publishing (`u3a-published` 26) and then `u3a-rejected` +5 per 5 s (17 in total, `u3a-skips` 269) with `last-why=u3a-indexed-after-overlay` and `held=1` for ~20 s at the first hangar screen; then, at 154,115, one clean stretch of ~3,000 published frames (50 s at 60/s) until the screen changed (`u3a-cuts` 21 -> 58 in 5 s while scrolling through aircraft). Every rejected frame held UI3D off for 1 s (flat screen), the next frame retried and was rejected again by the same late indexed draw (D-398 noted 7 indexed late overlay draws). So not a permanent latch, but a once-per-second flat flash while the screen drew that indexed draw: the "spotty" load. [S] `Producer::ui3d_draw` refused any INDEXED ps2_v draw after the overlay started (vertices are unread so it cannot be placed on a layer).
* **Menus look like the flat screen [S+I]:** every layer keeps the direction of each point of the flat screen from the anchor eye, so the panel had EXACTLY the vscreen's angular size (3.2 m x 1.8 m at 2.5 m = 65 degrees wide) and the layers sat only 1.8 / 2.7 / 3.5 m away (ratio 1.9: a stereo disparity difference of ~0.017 NDC between text and backdrop at 64 mm IPD). It is a flat picture with a faint relief. In the hangar the same full-size overlay (including the full-screen dim) covered the room behind it: "the menu and game render sat on the screen".
* **Summary lines [E]:** they did NOT stop. `ee-camera.log` has one per 5 s for the whole session. The lines that stop at ~154,292 are in `diagnostics.evidence.log`: that log is capped at 1 MB (`max_evidence_size`; the PSO census fills it in ~4 minutes: 1,009,175 bytes) and every `LogSink::line(true, ...)` mirrors there. Not a product defect, but the mirror used up the cap.

### 10.2 Fixes
1. **Late indexed overlay draw: dropped, not rejected** (`brief3d_live.h`, hangar branch of `ui3d_draw`): an indexed ps2_v draw after the overlay started returns `u3a-late-indexed-dropped` without a plan (the game's own draw still paints the flat image), counted `u3a-dropped`. The frame is not rejected, no hold, the room stays. Rejections that remain (a real structural failure) still cost the 1 s flat screen and recover as in 9.7.
2. **The overlay is a floating panel** (`u3s`, `u3mn`, `u3mf`; menus AND the hangar overlay; the pause menu keeps `u3n` / `u3f` and its own placement): `ui3d::scaled(screen, k)` shrinks the screen rectangle about its centre on the same plane and anchor, so each layer keeps the direction of each point of the SMALLER rectangle (angular size k x vscreen's); the layers sit at 1.1 m (text) / 1.9 m (panel) / 6.0 m (backdrop) by default (ratio 5.5: text-to-backdrop stereo disparity difference ~2.7x the old pair, tested > 2x). In the menus the surroundings are the cleared opaque slot (black void = the dark neutral environment); in the hangar the room is visible around the panel and the full-screen dim now darkens only the panel's rectangle.
3. **Summary lines:** `ee-camera.log` is the complete record (unchanged); the shared diagnostics evidence log now gets only every 6th `summary-ui3d` window (30 s) so it lasts; the other windows go to `ee-camera.log` only (`ee_camera_probe_win.cpp`, pacing tick; compile-checked by the full build only).

### 10.3 New options (inside `xr_terrain_stage`, same extraction as the others; the 255-character buffer was re-tested with the longest set)
| option | default | range | meaning |
|---|---|---|---|
| `u3s:<f>` | 0.7 | 0.2..1.5 | angular size of the menu / hangar-overlay panel relative to the vscreen framing (1.0 = the old full-framing panel) |
| `u3mn:<m>` | 1.1 | 0.5..10 | distance of the nearest (text) layer of menu / hangar-overlay frames |
| `u3mf:<m>` | 6.0 | 0.6..30 | distance of the farthest (backdrop) layer of the same frames; must be > `u3mn` (else the option is refused) |

Old look: `;u3s:1;u3mn:1.8;u3mf:3.5`. The log line `ui3d-option;ok=1;...` now ends `;u3mn=1.10;u3mf=6.00;u3s=0.70`. Summary: `u3a-dropped=<n>` before `u3a-why` (u3a only).

### 10.4 Tests (offline)
`ui3d_offline` (`test_menu_depth_and_scale`: parse, ranges, ordering, describe, `scaled`, tan(half angle) halved, centre kept, disparity gap > 2x, far/near > 5), `ui3d_hangar_offline` (`test_late_indexed_never_rejects`: 60 glue-faithful frames with a late quad AND a late indexed draw publish, 0 rejected, 0 skips, `u3a-dropped=60`; the indexed-after-overlay case now drops and publishes; the L11 hold-recovery test uses a stencil refusal as its rejection; `test_overlay_panel_scale_and_depth`: the producer's overlay vertices equal `rewrite_vertices` on the scaled screen with the menu distances, and differ from the old framing).

### 10.5 Null run check (a build with `;ui3d:1;u3a:1`, the default new options; eye dumps via `png-screen.now`)
1. `ui3d-option` line shows `u3mn=1.10;u3mf=6.00;u3s=0.70`. Menus: `-eye0-table.png` shows a SMALLER panel (about 0.7 of the width of the old dump) in black; the left/right eye dumps differ for text vs backdrop by a clearly larger offset than before (text vs frame border).
2. Hangar screens: `-eye0-table.png` shows the room filling the eye image with the panel floating over it, the room visible all round (not covered by a full-size dim); `u3a-published` ~60/s for the whole visit, `u3a-rejected` 0..few, `u3a-dropped` > 0 (the indexed late draws), `held=0`.
3. If the panel text is too small to read: raise `u3s` (0.85-1.0); if the depth feels too strong or the backdrop too far: lower `u3mf`.
4. `ee-camera.log`: `summary-ui3d` every 5 s for the whole session; the evidence log carries every 6th window.
5. Not changed / still open: a dolly of the room anchor toward the aircraft (the aircraft keeps the game's angular size, 9.7 note) and what the hangar draws once the aircraft-select screen is left (`u3a-why=ui3d-frame-has-3d` after 154,165 in live-1144: other ACM screens with a different k400, not analysed here).


### 10.6 Follow-up 2026-10-30: what `u3a-why=ui3d-frame-has-3d` after t=154165 was (live-1144; offline tests only)
* **Evidence [E]:** `u3a-why` is set only by `ui3d_acm`'s `fail()`; the string `ui3d-frame-has-3d` there comes ONLY from an ACM draw outside an open hangar frame that is not a hangar draw (record +400 != the hangar constants, an unreadable record) or from a stage-2 frame already open when an ACM draw arrives. In the log `u3a-frames` stays 3199 from 154,165 on while stage-2 menu `frames` publish again (+173 at 154,170, 60/s from 154,215 to 154,315), `last-refusal` runs `ui3d-frame-has-other-shader` (154,175) -> `ui3d-frame-has-3d` (154,190) -> thousands of `ui3d-pso-rtv` (154,205+, the K9 mission-load / intro / flight pattern: arms 8-14 of 8.1 draw ACM 11-15 with another k400 plus other shaders). K9 8.1 lists the same sequence after the last hangar screen (black, mission load, flight). So the hangar visit ENDED at ~154,165 (deploy confirmed); no hangar room frame was refused after it. The sticky `u3a-why` was the flight/loading ACM frames' refusal, which is the designed behaviour (those frames are not the room). Not proven from this log alone (the census was not armed): the stage-2-open-before-ACM ordering is not excluded for other screens.
* **What the log does show in the last hangar seconds [E]:** `u3a-cuts` 21 -> 58 (+37) while only 228 hangar frames were seen (154,160-154,165, the deploy sequence): the K10 cut rule (eye jump > 8 units or forward change > 10 degrees against the previous frame) fired on ~16 % of the frames: a continuously moving camera counted as a cut every frame, re-latching (re-targeting) the room each time = the room snapping around (the "spotty" feel).
* **Fixes:** (1) consecutive cuts are a camera in fast motion: the room stays latched (`u3a-moving` counts the held frames) and is re-targeted ONCE at the first quiet frame; an isolated cut still re-targets at once (K10 screen changes). (2) ACM draws with another screen constant (flight, offscreen, mission load) or an unreadable record outside a hangar frame are counted `u3a-foreign` and no longer overwrite `u3a-why`, so the summary names the last REAL hangar failure. Summary tail: `...;u3a-dropped;u3a-moving;u3a-foreign;u3a-why`.
* **Tests:** `test_moving_camera_does_not_relatch_every_frame` (6 alternating-camera frames: cuts 1, moving 4, latches 1 (the old rule: cuts 5); one quiet frame -> cuts 2 and then stable; a later isolated cut re-targets), `test_foreign_acm_does_not_mask_the_hangar_reason` (a real hangar rejection, then a flight-k400 ACM: foreign 1, why unchanged).
* **Null run:** keep the census file-armed (`ui3d-census-acm.arm`) over aircraft select -> weapon selects -> deploy -> load; `u3a-foreign` should start growing only when the load starts; `u3a-moving` > 0 only during the deploy camera sweep; a stage-2 frame refused with ACM after a ps2 preface would show as `u3a-why=ui3d-frame-has-3d` with `u3a-foreign` not growing (then add ACM-after-ps2 ownership).
* **Dolly of the room anchor toward the aircraft:** not done (not cheap: the look point is unknown, the weapon-select eye is only ~10 m from it, so a fixed dolly could pass through the target).


## 11. Note 2026-10-30b: step134 headset report - the flat screen quad hid the 3D (hangar room, briefing table) and the panel was soft (offline tests only, NOT run live)

User words (real headset, cfg `;ui3d:1;u3p:1;u3a:1` + `brief3d:1`, screenshot `docs/headset-tests/2026-10-30b-user-1.webp`): "In briefing screen, the main screen renders in front of the briefing 3d model so i cant see elevated markers"; "Hangar still has screen in front also very blurry needs some edge smoothing or sharpening".

### 11.1 Causes [E = evidence, S = source]
* **Screen in front [E+S] (both complaints, one cause):** `xr-stereo.log` has said since step40 `event=world-composite;...;hypothesis=steamvr-draws-quad-layers-above-projection`: SteamVR draws QUAD layers above PROJECTION layers whatever the submission order (that is why the terrain moved into the projection). `compose_layers` (screen_only path) submitted `[screen quad, projection]` for every menu / hangar / briefing frame, relying on list order, so the flat 2560x1440 game image (pillarbox bars, light glows the room drops, full size HUD text: all visible in the screenshot) was drawn OVER the room / the table. The screenshot's panel is the 16:9 vscreen at its full 65 degree size, the room only shows around it; the 0.7-scaled overlay panel of the eye image is hidden behind it. Briefing: the table sits ~31 degrees below the eye and its raised (air) markers poke up into the 18-degree-high region the screen quad covers: the ground part stays visible, the elevated markers vanish behind the screen = the report. The null `eye0-table.png` dumps (runAGU1 t76034 / t100101: room + small yellow panel, correct) are the eye image BEFORE the compositor, so they never showed it.
* **Soft panel [E+S]:** the producer's eye image is 1440 x 1440 (`kEyeW/kEyeH`) and the eye swapchain is 2400 x 1350 (`xr-stereo.log swapchains;eye0=2400x1350;...policy=source-aspect-width-min`): the table blit UPSCALES x1.67 horizontally (x0.94 vertically) with the hardware bilinear filter (`xr_brief3d.hlsl`), after the game's own draws already minified the 2560-wide UI into the 0.7-scaled panel. Text strokes lose contrast twice. (The flat screen copy, `xr_vscreen.hlsl`, is also a bilinear 2560 -> 2400 resample; unchanged here.)

### 11.2 Fixes
1. **Layering (`u3q:1`, default):** `ProjectionLayer::table_front` (set by `TableCore` from the producer's u3q). `compose_layers` with `screen_only`: an OPAQUE slot (stage-2 menus, hangar room + overlay panel) is submitted ALONE (`[projection]`, no screen quad: it covered the whole view anyway); a premultiplied BRIEF3D table is submitted as `[screen quad, table quad eye 0, table quad eye 1]` (new `LayerKind::table_quad`): quads order among themselves by list order (HUD over marker over world quad has worked since step40), so the table is above the screen. `xr_stereo::view_quad(view, 1.5 m)` turns each projection view (pose + asymmetric fov) into a plane perpendicular to the view axis whose edges keep their directions (seen from the render pose it is the projection image; `xr_owner_openxr.cpp` submits it with `eyeVisibility` LEFT / RIGHT, same swapchain and flags). Stale slot or no table: the screen quad alone, as before. Flight lists are untouched. `u3q:0` restores the old list.
2. **Sampling (`u3r`, `u3k`):** opaque UI3D / pause slots are resampled in the blit with Catmull-Rom (4x4 `Load` taps) plus an unsharp mask `CR + k (CR - bilinear)` (`ui3d_resample.h` mirrors the shader; b0 bits 16..17 filter, 24..31 amount; the premultiplied table keeps bilinear). Default `u3r:2;u3k:0.35`. Justification: the loss is at a 1.67x UPSCALE of an already minified image, so mips / anisotropic filtering do not apply; Catmull-Rom is interpolating (exact at texel centres), C1 and the sharpest 4-tap kernel without strong ringing; the unsharp term restores stroke contrast (measured offline: 10-90 % step edge 1.3 / 1.2 / 1.1 output px for bilinear / CR / CR+unsharp, 1-texel lines keep more peak). Rejected: Lanczos-3 (6x6 taps, ringing on 1-texel frame lines), a bigger eye image (192 MiB of slots at 1440; the commit limit), a high-resolution UI quad layer (loses the 3-layer stereo depth; kept as the next step if this is still soft: replay the 2D overlay into a separate 2560x1440 slot shown as a quad above the room, which SteamVR then samples itself).
3. **Option summary (inside `xr_terrain_stage`, same extraction, 255-character buffer re-tested with the new tokens):**

| option | default | meaning |
|---|---|---|
| `u3q:<0\|1>` | 1 | 1 = the screen quad never covers the 3D (above); 0 = old `[screen quad, projection]` list |
| `u3r:<0\|1\|2>` | 2 | resample of opaque UI3D / pause slots: 0 bilinear (old), 1 Catmull-Rom, 2 Catmull-Rom + unsharp |
| `u3k:<0..1>` | 0.35 | unsharp amount of `u3r:2` |
| `u3s:<f>` | 0.7 | (existing) a larger panel (0.85-1.0) also raises the pixel density of the text |

`describe()` / the `ui3d-option` line show `;u3q=..;u3r=..;u3k=..` only when one differs from the default (the default line is byte-identical to before).

### 11.3 Tests (offline; the compositor behaviour itself is only visible in a headset)
`brief3d_table_offline` (`test_ui3d_opaque`: opaque slot -> `[projection]` alone; table -> `[screen, table_quad 0, table_quad 1]` premultiplied; `u3q:0` -> the old lists; `test_layers`; `test_view_quad`: size / centre / orientation for an asymmetric fov and a turned head), `ui3d_offline` (`test_layering_and_filter_options`), new `ui3d_resample_offline` (weights, bit packing, exact at texel centres, constants preserved, sharper than bilinear at the real 1440 -> 2400 scale, bounded overshoot, the HLSL source carries the same weights). The shader (fxc `/WX`), `xr_owner_openxr.cpp` and `xr_brief3d_d3d12.cpp` compile in a private build; ctest `ui3d_hangar_offline`, `ui3d_pause_offline`, `brief3d_live_offline`, `vscreen_offline`, `xr_stereo_output_offline` still pass.

### 11.4 Null-run / headset check list
1. `xr-stereo.log`: `ui3d-option` line unchanged (defaults); aircraft select: `-eye0-table.png` = room + 0.7 panel, text crisper than runAGU1 t76034 (zoom on `J35J Draken`), no ringing halos around the yellow frame lines (else lower `u3k`, or `u3r:1`).
2. Headset: hangar = room all round with ONLY the panel in front, no pillarboxed game image; briefing: raised markers visible above the table, the 2D briefing panels still readable behind (`u3q:0` brings the old order back for comparison). If the briefing table looks shifted against the screen when moving the head, the quad distance (`kTableQuadDistanceM` 1.5 m) is the knob (not an option yet).
3. Not changed / open: the stage-2 menus' black surroundings (no room), the flat screen copy's own 2560 -> 2400 bilinear, reflections / hangar lighting, panel text still limited by the 1440 eye image.

## 12. Note 2026-10-30c: headset test 5 items 1-2 - the selection box as a 3D slab, the hangar yellow screen, the hangar light glows, the backdrop size (offline tests only, NOT run live)

Report: `docs/headset-tests/2026-10-30c-user-report.md` items 1-2; screenshots `D:\acz-evidence-archive\headset-2026-10-30-session\live-1936-step137\user-shots` (5.webp 3D main menu, 6.webp VR hangar, 7.webp flat hangar). Live cfg [E, xr-stereo.log]: `ui3d-option;ok=1;ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3p=1;u3a=1;u3au=5.0;u3mn=1.10;u3mf=6.00;u3s=0.70`. User clarification of item 1 (verbatim): "offset the lines is fine, but the selection box could be 3d and closer to the text so i can use it in VR".

### 12.1 Causes [E = evidence, S = source]
* **Selection box depth [S + E]:** the classifier (`ui3d.h` `LayerAssigner::assign`) puts a medium draw on layer 1, and `layer_distance` spaces the layers evenly in dioptres between `u3mn` 1.1 m and `u3mf` 6.0 m (`brief3d_live.h` ui3d_draw: `ui_lopts_.near_m = uo_.menu_near_m; ui_lopts_.far_m = uo_.menu_far_m`), so the panel layer is at 1/((1/6 + 1/1.1)/2) = 1.86 m, 0.76 m behind the text (and the layer never decreases, so a box drawn after the first glyph lands on the text plane instead: a mix of both). The selection box is a panel-class draw [E: census step124 runD1 s=10/59: the 0.293 x 0.143 quad at (0.157, 0.127) is drawn right before the glyphs (ord 28 / 19, z 64, SRC_ALPHA/INV_SRC_ALPHA), its centre y 0.1985 on the first text row's centre 0.201; in 38 + 24 sampled frames of runD1 / K10]. With 0.76 m between the box and its text the two separate as soon as the head moves (5.webp, a tilted view: the yellow bar is not under the item it highlights). Live counts [E, ee-camera.log summary-ui3d at 182627603]: far 45,003 / mid 72,318 / near 1,004,821 draws over 4,390 menu frames.
* **Hangar yellow screen [S + E]:** the hangar overlay (u3a) is placed by the stage-2 rules, so its FULL-SCREEN draws (bbox area >= `kLargeArea` 0.30) go to layer 0 at `u3mf` 6 m, scaled by `u3s` 0.7: a translucent screen-sized rectangle floating in the room (6.webp). K10 census [E, step124 runK10 ee-camera.log, hangar arms 127,149,000-127,174,000, overlay draws z <= 256]: 24 x 0.751 x 1.0 (z 64, SRC_ALPHA/INV_SRC_ALPHA: the yellow tint / grid / frame), 19 x 0.751 / 0.821 x 1.0 (z 64, additive SRC_ALPHA/ONE), 1 x 0.938 x 1.0 (z 0, ZERO/BLEND_FACTOR: the dim). The largest non-full-screen overlay draw is 0.375 x 0.286 (area 0.107); the STANDBY-type boxes are 0.188 x 0.071 / 0.199 x 0.214. In the flat game these are the yellow-green wash and grid over the whole picture (7.webp).
* **Hangar lights [S + E]:** the ceiling-light glows are not ACM geometry: they are ps2_v quads the EE already projected with the hangar camera [E, K10: 4 vertices, q 1, ONE GS z per quad (3.4e4..2.9e5), additive `bl=1:5,2,1`, depth ALWAYS without write `dp=1/8/0`, between the ACM blocks, before the overlay]. `ui3d_draw` dropped every one (`brief3d_live.h` hangar branch, `u3a-fx-dropped`; 9.2 / 9.6 (d)). Live [E, step137 summary-ui3d at 182878120]: `u3a-fx=271209` over `u3a-frames=6627` = ~41 dropped effect draws per hangar frame (`u3a-dropped=2439` late indexed draws besides). The lamp fixtures are ACM geometry and are visible in 6.webp; only their glow sprites were missing. Not a post pass and not a class gate: a deliberate drop, because a camera substitution cannot move pre-projected vertices.

### 12.2 Fixes
1. **Selection slab (`u3sel:2`, default):** a frame's glyphs (small draws on the text layer) are merged into text rows (`SelectionRows`); the NEXT frame tests every medium draw against them (no look-ahead exists: the box is drawn before its text, and the replay is recorded before the game's draw). A bar (height <= 0.16 of the image, width >= 1.5 x height) whose centre lies within 0.33 row heights of a row's centre, 0.8..3.2 rows high, overlapping the row by more than half of the narrower of the two, is the selection box. It is replayed as a SLAB: back face, 4 side walls, front face (6 quads, one `DrawInstanced(4, 1, c * 4, ...)` each from one eye vertex copy, `UiPlan::copies`), the front face `u3sg` 4 mm behind the TEXT plane, the back face `u3sd` 12 mm further. Each face is the game's own quad (PSO, texture, colours) with x, y, q rewritten for its depth (`rewrite_at`); a wall of edge (a, b) is [front a, back a, front b, back b] with the attributes of a and b (the edge texels). From the anchor the front face covers the flat image's box exactly; its stereo disparity differs from the text's by 0.9 % of the text-to-panel difference (test), so it stays registered to the item. The other panels, tiles, strips and lines keep their layer depth (byte-identical, tested). `u3sel:1` = the flat front face only, `u3sel:0` = the old placement. Scissor: the game's scissor carried onto the front and back faces (union). Risks [I]: the first frame of a new screen uses the old placement (one frame); a non-selection bar centred exactly on a text row (a header centred on its title) would also become a slab; the PSO cull mode is not logged (with back-face culling some walls would show from one side only; the faces keep the game's vertex order).
2. **Hangar tint (`u3at:0`, default):** in a hangar frame an overlay draw with bbox area >= `kLargeArea` is classified (the classifier state is unchanged) and then NOT replayed (`u3a-tint-dropped`, counted `u3a-tint`). The panels, the STANDBY-type boxes, the selection box and the text are replayed as before. `u3at:1` = the old far-layer rectangle.
3. **Hangar glows (`u3ag:1`, default):** a non-indexed effect quad before the overlay is RE-PROJECTED (`ui3d_hangar.h` `gs_to_world` / `world_to_local` / `rewrite_effect`, producer `hz_glow_locked`): the GS z is the game's depth model `z = b + Z0 / w` (b, Z0 from the frame's decomposed A [E for the ACM draws; that the glows share it: I, they share the target and the depth range]), so each vertex is the hangar-world point `P = eye + w (fwd + ndc_x tanX right + ndc_y tanY up)` (ndc from the draw's cb0 over the whole target = k400 [E]); P goes through the latched room to LOCAL and is projected per eye as the layers are; `q' = q (w_src / upm) / w_eye`; z, colours, texcoords unchanged; scissor = the whole eye image (as the ACM replays). The glow keeps its place in the game's draw order between the ACM blocks (depth ALWAYS: painter's order as in the game). Any unusable fact (PSO, a viewport other than the whole target, a depth outside 0.5..70,000 units, the upload budget) DROPS the glow (`u3a-fx`, reason `u3a-glow-why`) and never rejects the room. The sprites face the GAME camera, not the eye: far off-axis they flatten [I]; the floor shadow (indexed, unread vertices) is still dropped. `u3ag:0` = the old drop.
4. **Backdrop size (item 4, `u3fs:<1..3>`, default 1.0 = unchanged):** layer 0 (the menu backdrop and its scissor) is spread by k about the panel centre (`layer_scale`; panel and text layers unchanged). Options to fill more of the view, cheapest first: (a) `u3fs:1.5` (backdrop only; its texture magnified x1.5, softer); (b) `u3s:0.85..1.0` (the whole panel larger, also raises the text pixel density, 11.2); (c) `vsw` / `vsd` (the vscreen framing; changes the flat screen too). Not done: enhancing the backdrop asset (no texture-replacement route exists). The default stays 1.0 because the anchor-alignment invariant (every layer = the flat image from the anchor) is tested with the defaults.

| option | default | meaning |
|---|---|---|
| `u3sel:<0\|1\|2>` | 2 | selection box: 2 slab behind the text, 1 flat face behind the text, 0 old layer |
| `u3sg:<mm>` | 4 | text plane -> slab front face (0..50) |
| `u3sd:<mm>` | 12 | slab thickness (1..100) |
| `u3fs:<k>` | 1.0 | backdrop layer angular scale (1..3) |
| `u3at:<0\|1>` | 0 | hangar: 1 keeps the full-screen overlay draws (old) |
| `u3ag:<0\|1>` | 1 | hangar: 0 drops the light glows (old) |

`describe()` / the `ui3d-option` line show `;u3sel=..;u3sg=..;u3sd=..;u3fs=..` only when one differs from the default, and `;u3at=..;u3ag=..` only with u3a and a non-default value (the default line is byte-identical to before; the 255-character buffer re-tested). Summaries: `summary-ui3d` gains `;sel=<selection replays>` after `last-refusal`; with u3a the hangar part gains `;u3a-tint=<dropped>;u3a-glow=<re-projected>;u3a-glow-why=<last glow drop reason>` after `u3a-why`.

### 12.3 Tests (offline; nothing ran in the game or a headset)
`ui3d_offline` `test_selection_slab_and_backdrop_scale`: options / describe / buffer; the selection rule on the census rows (the box and the box one row down select; the header panel, strip, tile, backdrop, a bar beside the text and a bar on no row do not); `screen_box`; the slab = back face, walls, front face byte for byte; real depth from a moved eye; the front face from the anchor = the text layer's pixels; disparity within 2 % (0.9 %) of the text-to-panel gap; refusals; culling; `u3fs` spreads layer 0 only and its scissor grows. New `ui3d_select_offline` (producer): a menu frame then the same frame again: frame 2's box is the slab exactly as `rewrite_slab` on the u3s-scaled screen with the menu distances (u3sel 2), the flat front face (1), byte-identical to frame 1 (0); every other draw byte-identical to frame 1; `;sel=1`. Hangar tint: the dim and the 4:3 tint dropped and counted, the box / glyph keep their classes, replays 4 (6 with `u3at:1`). Glows: a 6 x 6 unit light 150 units ahead projected with the real K10 A gives the GS quad; the replayed eye vertex = the eye matrix A'_e of the latched room on the same world point within 0.1 GS units, q' = w_src / w_eye, z / colours / texcoords unchanged, whole-eye scissor, the shadow still dropped; `u3ag:0` and a bad viewport drop it without rejecting the room; `gs_to_world` / `world_to_local` round trips. `ui3d_hangar_offline` keeps its stage-3b structure tests with `u3at:1;u3ag:0` pinned in its fixture. Nothing was built in this session (no build authorised yet): compile errors surface at the next build.

### 12.4 Null run (a build with this change; the live cfg, the new options at their defaults)
cfg (inside `xr_terrain_stage`): the day value with `;ui3d:1;u3p:1;u3a:1` (the defaults give `u3sel:2;u3sg:4;u3sd:12;u3at:0;u3ag:1`); add `;u3fs:1.5` to try the larger backdrop. The `ui3d-option` line must equal step137's (plus `;u3sel=..;u3fs=1.50` only if set).
1. Main / campaign menus: `summary-ui3d` `sel` grows ~1 per menu frame (0 = the rule never matched: then census the late menu draws with `ui3d-census-tail.arm`), `rejected` unchanged. Eye dump (`png-screen.now`): the highlight bar directly under its item; moving the null head sideways shows its rim.
2. Hangar (aircraft select, weapon selects, deploy prompt): `u3a-tint` +4..6 per frame, `u3a-glow` +17..42 per frame, `u3a-fx` +1..3 per frame (the shadow only), `u3a-glow-why=none`, `u3a-rejected` 0. Eye dump: no yellow rectangle; glow spots at the ceiling lamps, staying on the lamps when the head turns.
3. Pause menu (u3p): unchanged except a selection slab when its bar matches a row (`sel` grows while paused).

### 12.5 step139 null run (runAGU1): "hangar text mostly gone" is the game's text-reveal animation, not a regression (no code change)
Evidence: `D:\acz-evidence-archive\step139\batchAG\runAGU1`. [E] `upright\acz-vr-xr-screen-t99313-eye0-table.png` shows "OK", "ST", "De" on yellow cells; the FLAT image dumped at the same instant (`-t99313-screen.png`; `xr-stereo.log` line 463-464: both parts queued at t-ms 99331) shows the same partial text, "OK|", "STR", "Dep" on yellow cells: the deploy prompt typing itself out (the third confirm tap was ~2 s earlier). The other two hangar dumps show all text in the eye image: t83305 (aircraft select: OK | CANCEL | DISPLAY, J35J Draken, statistics, weapons, the description, Y MORE) and t91313 (PLAYER weapon select: every panel and the description). `summary-ui3d` [E]: `u3a-tint=5430` over `u3a-frames=1812` = 3.0 per frame (the K10 full-screen count: 3 per frame), `u3a-rejected=0`, `sel` 5123 in all.
* Hypotheses checked: (a) batched glyph draws spanning >= 30 %: none exist (every non-indexed ps2_v overlay draw is ONE quad: K10 hangar 3,980 x n=4 + 44 x n=3, runAGU1 2,371 x n=4); (b) the slab covering the text: the slab is drawn before the glyphs (painter's order, depth ALWAYS) and never replaces them; (c) the text layer: unchanged.
* Regression test `ui3d_select_offline` `test_k10_hangar_text_all_replayed`: the real overlay of the K10 PLAYER weapon screen (runK10 ticks 127156569-570, s=8, all=154..187, the census vertices) in two hangar frames: only the three full-screen draws are dropped; every panel, box and all 23 glyphs are replayed for both eyes, glyphs on the text layer and never a slab; frame 2's glyphs (selection rule armed) are byte-identical to frame 1's.

## 13. Note 2026-10-30d: headset test 6 items 1-2 - hangar lights blink with the game camera, the hangar panel offset, and a 3D reinterpretation of the menus (offline tests only, NOT run live)

Report: `docs/headset-tests/2026-10-30d-user-report.md` items 1-2 (step143, cfg day + `ui3d:1;u3p:1;u3a:1`); screenshot
`D:\acz-evidence-archive\headset-2026-10-30-session\live-2210-step143\user-shots\10.webp` (hangar in VR); logs in that folder. User: "menus are
better - would love to see how you could reinterpret them in 3D in the same style/theme"; the hangar UI is offset from the background elements
(open to a 3D reinterpretation); the hangar lights turn off intermittently. Nothing below ran in the game, SteamVR or a headset; nothing was built.

### 13.1 Causes [E = evidence, S = source, I = inference]
* **Lights turn off: the game stops emitting the glows; nothing of ours drops them [E + I].** `ee-camera.log` has 22 `summary-ui3d` windows
  with hangar frames (t = 191,737,170 .. 191,842,368 ms; 6,268 hangar frames). For the whole session `u3a-glow-why=none` (no glow was ever
  dropped by a fact: not the depth model `u3a-glow-unproject`, not the viewport, not the upload budget), `u3a-rejected=0`, `u3a-published =
  u3a-frames - 1`, and `u3a-fx` grew by 2-3.5 per frame (the indexed floor shadow only). Yet the re-projected glows per hangar frame, window by
  window, were 17, 36, 40, 24, 7.4, 39, 27, 38, 33, 44, 37, **9.3, 3.5, 5.5, 5.7, 7.2**, 21, 27, 25, 24, 37, 52. The fall from 44 to 3.5 per frame
  (and back) happened inside a stretch with NO cut, NO moving frame, NO latch and NO rejection (`u3a-cuts` 22, `u3a-moving` 55, `u3a-latches` 1
  unchanged from frame 2,714 to 4,818; t = 191,782,226 .. 191,817,291). So the three candidates are excluded: (a) depth-model failures would
  have set `u3a-glow-why`; (b) cuts / the consecutive-cut hold did not change in the low stretch; (c) no frame was dropped or rejected. What
  varies is the number of glow quads the GAME draws: the EE pre-projects a glow only for a lamp its own camera sees [I: the aircraft-select
  camera orbits at ~9 deg/s (8.5), a 40 s turn; the low stretch lasted ~25 s], while the VR room is latched and shows every lamp fixture (ACM
  geometry). Lamps whose glow the orbiting game camera cannot see are dark (10.webp: white fixtures without glow, only two green floor lights
  lit), and they light up again as the orbit comes round. K10 census [E]: glows are drawn in runs between ACM blocks (s=1: 24 ACM, 10 glows,
  26 ACM, 8 glows, 3 shadow, 1 glow, ~82 ACM, overlay), so a remembered glow must not be drawn earlier than its run.
* **Hangar panel offset [S + E].** The overlay uses the stage-2 layers: panels / frames / tiles go to layer 1 (1.86 m with `u3mn` 1.1 /
  `u3mf` 6.0), glyphs to layer 2 (1.1 m), and the layer never decreases within a frame, so frames drawn after the first glyph sit on the text
  plane while those drawn before it stay 0.76 m behind. Every layer keeps the anchor's directions: from the anchor all coincide, from any other
  head position parallax separates them (10.webp, the user looking up and right: the [AIRCRAFT NAME] frame, the [WEAPONS] frame and the
  colour-column squares sit off the text and boxes they belong to). The census [E: runK10 hangar overlay, 3,371 quads] shows the outlines are
  NOT line quads: every panel / frame / tile is ONE textured quad at z 64 with the outline in its texture (3 thin quads in all), glyphs are
  z 1, the dim z 0. So "frame lines" are panel quads, and giving them thickness means extruding the panel quad.

### 13.2 Fixes (offline tests only)
1. **Glow memory (`u3agm:1`, default)** (`ui3d_hangar.h` `GlowMemory`, producer `hz_glow_mem_locked`): every re-projected glow is remembered in
   HANGAR-WORLD space (its 4 source vertices + the camera and cb0 that projected them; matched across frames by its world centre within
   clamp(radius / 2, 1, 8) units and its state key; up to 96). A real glow draw of the game draws, as consecutive copies of ITS draw (its PSO,
   texture, sampler, blend; `UiPlan::copies`, up to 16 quads, within the slot's upload span), first itself (unless the memory already drew it
   in this frame) and then every remembered glow with the same state key that this frame has not drawn and that the game last drew no later
   in its order (the count of ACM draws before it). A real glow already drawn from memory in this frame adds nothing (never doubled). State
   key (glue, `u3Facts_locked`): FNV-1a of the PSO record, the pixel t0 root value + table offset, and s0 when it is a root parameter; t0
   unresolved = 0 = no memory for that draw. Cleared at a latch / gap and at a camera cut (`u3agm:2`: kept across cuts); `u3agm:0` = the old
   path. A single first-seen glow gives exactly the old plan. Remembered glows a frame could not place (no later carrier with their key) are
   counted (`u3a-glow-missed`).
2. **One surface per panel (`u3pp:1`, opt-in)** (`ui3d.h` `placement_layer`): the panel layer is placed ON the text plane, so frame quads,
   fills, tiles, selection and text of every panel share one surface and nothing of a panel can separate with head motion. The backdrop
   (layer 0) keeps its own depth. The classifier, its layer numbers and statistics are unchanged.
3. **Frame rims (`u3lt:<mm>`, opt-in)** (`ui3d.h` `frame_quad`, `rewrite_rim`): a frame quad (4 vertices, medium, z > 1) is replayed as a RIM:
   its 4 edge walls extruded u3lt behind its surface, then the front face on the surface (5 copies: the slab without its back face, so a
   translucent fill is not doubled). From the anchor the front face is exactly the flat image; off-axis the walls show the quad's edge texels
   (the outline at the texture border [I from the flat image]) as a bevelled strip. If the frame's upload span cannot hold 5 copies the quad is
   drawn flat (`rim-flat`), the frame is never refused for it. The selection slab (u3sel) takes precedence.

Both stage-1 options are opt-in so every existing fixture and the default `ui3d-option` line stay byte-identical; the recommended cfg is below.

### 13.3 Options and summary
| option | default | meaning |
|---|---|---|
| `u3agm:<0\|1\|2>` | 1 | hangar glow memory: 1 cleared at latch / cut, 2 kept across cuts, 0 the game's glows only (old) |
| `u3pp:<0\|1>` | 0 | 1 = the panel layer on the text plane (one surface per panel: no offset) |
| `u3lt:<mm>` | 0 | 0..50: frame quads as rims this deep behind their surface (recommended 6) |

Recommended cfg addition (inside `xr_terrain_stage`): `;u3pp:1;u3lt:6` (the step143 value without wcloud + these = 185 characters, < 255).
`ui3d-option` shows `;u3pp=..;u3lt=..` only when set and `;u3agm=..` only with u3a and a non-default value. `summary-ui3d` gains
`;rim=<rims>;rim-flat=<fallbacks>` after `sel` (only with u3lt > 0) and, with u3a, u3ag:1 and u3agm != 0,
`;u3a-glow-mem=<remembered quads drawn>;u3a-glow-lights=<entries>;u3a-glow-missed=<remembered glows a frame could not place>` after `u3a-glow-why`.

### 13.4 Design: the ACZ menus reinterpreted in 3D ("glass and amber"), driven by the game's own draws
Goal: the same look (the amber / yellow outline panels, the grid, the typeface, the colours) as real objects in space, correct on every screen
because every element is still one of the game's own ps2_v quads (its PSO, texture, colours, glyphs) moved and extruded per eye; nothing is
drawn from our own assets. Rules: (R1) every element of one panel lies on one surface (no offset, ever); (R2) depth comes from STRUCTURE (panel
groups, nesting, selection), not from a per-draw size class; (R3) from the anchor the composite is still exactly the flat image, so nothing is
lost or misplaced on any screen; (R4) any unknown structure falls back to the flat panel, never to a missing element.

Elements:
* **Panes**: each panel quad (z 64, medium) is a thin glass slab: the front face on the panel surface, walls behind it (stage 1: the rim;
  stage 3: optionally a back face at reduced alpha for a visible pane thickness).
* **Frame lines as bevelled strips**: the rim walls carry the outline texels; stage 3 shades them by orientation by scaling the wall copies'
  COLOR0 (top / left x1.0, bottom / right x0.6 [U: the colour slot is bytes 16..31 per the ps2_v disassembly (section 1); verify on a dump]),
  a lit bevel with the game's own shader.
* **Text** on the pane's front face (stage 1), optionally embossed 2-4 mm in front of it (stage 3; the `u3sg` precedent: within 1 % of stereo of
  the pane, so it stays registered).
* **Selection**: the existing slab (u3sel:2) moved IN FRONT of its pane (stage 3), so the cursor reads as a lit key standing out of the glass.
* **The grid / tint** (the full-screen yellow wash, dropped in the hangar by u3at:0): stage 4 replays that same draw on a large horizontal
  plane below the panels (a floor grid in the menus' void) and keeps it dropped in the hangar room.
* **Curvature**: panel groups on tangent planes of a vertical cylinder about the anchor (radius = the panel distance), each facing the user:
  a gentle wrap around the viewer without bending any one panel (text stays flat and sharp).

Staged plan:
| stage | what | how (driven by the game's draws) | status |
|---|---|---|---|
| 1 | one surface per panel + frame thickness | `u3pp:1` panel layer on the text plane; `u3lt` rims of the frame quads | IMPLEMENTED (offline tests), opt-in |
| 2 | panel GROUPS on their own planes, gentle curvature | the previous frame's frame quads (medium, z 64) define panel rectangles (the SelectionRows precedent: no look-ahead); each draw of the current frame joins the smallest rectangle containing its bbox centre (else the base group); a group's plane is the cylinder tangent at its centre direction (`u3c` radius), nested groups 5 mm in front of their parent; a screen change costs one frame on the base surface | IMPLEMENTED 2026-10-31 (section 14), opt-in `u3gp:1` |
| 3 | lit bevels, embossed text, raised selection, pane back faces | colour-scaled wall copies; text in front by a few mm; slab in front of the pane; back face at reduced vertex alpha | IMPLEMENTED 2026-10-31 (section 14), opt-in `u3gp:2\|3` |
| 4 | the grid floor / void | the full-screen tint / grid quad replayed onto a floor plane (menus only), its scissor dropped | design |
| 5 | hangar | the overlay panels as stages 1-3 over the room; the glow memory (13.2) keeps the room lit | stage 1 + glows done |

Per stage the checks are those of stage 1: the anchor invariance (from the anchor every replayed pixel equals the flat image), coplanarity per
group (a frame corner and a glyph at the same screen point project to the same eye pixel from a moved head), byte equality of every
non-participating draw, and a K10 / runD1 real-overlay fixture.

### 13.5 Tests (offline; nothing was built in this session)
New `ui3d_panel_offline` (tests/acz-vr/ui3d_panel_offline_tests.cpp, registered in src/acz-vr/CMakeLists.txt): options / describe (the default
line unchanged, u3agm only with u3a) / the 255-character buffer with the step143 cfg; `placement_layer` and `frame_quad` on the K10 census
sizes; coplanarity (with u3pp:1 a panel corner and a glyph at the same screen point land on the same eye pixel from a head 0.3 m off the
anchor; with u3pp:0 they separate by > 1 GS unit; both agree at the anchor); `rewrite_rim` = the slab's walls + front face, the front face =
the coplanar panel, the walls keep the edge attributes and have depth; producer menu frames (old / u3pp / u3pp + u3lt: the layer-1 tile placed on
the text plane, as a rim; glyph and backdrop unchanged; `;rim=1;rim-flat=0`); the real K10 PLAYER-weapon overlay in a hangar frame with
u3pp:1;u3lt:6 (every panel a rim whose front face lies on the text plane, every glyph flat on it); the glow memory: pure (centre / radius,
tolerance, same-frame sprites, capacity, pending order / skip / max, forget + compact, degenerate copies), a lamp the game stops emitting stays
drawn at its room position (eye-matrix oracle < 0.1 GS units) and is never doubled when the game emits it again (`u3a-glow-remembered`),
`u3agm:0` = the old single copies, painter's order (a glow seen after 2 ACM draws is not carried by a glow after 1), state keys (another key is
never carried and is counted missed; key 0 = no memory), a cut clears (u3agm:1) / keeps and re-projects through the re-targeted room
(u3agm:2), a gap clears. Existing fixtures are unchanged (u3pp / u3lt default off; the glow memory needs a state key, which they leave 0).

### 13.6 Null run / headset check list (a build with this change)
cfg: the day value with `;ui3d:1;u3p:1;u3a:1;u3pp:1;u3lt:6`.
1. `ui3d-option` = step143's line + `;u3pp=1;u3lt=6.0`. Main menu: `summary-ui3d` `rim` grows (~5-15 per menu frame), `rim-flat=0`,
   `rejected` unchanged. Eye dump: panels, outlines and text on one plane; with the null head moved sideways the outlines stay on their text and
   panel edges show a thin strip.
2. Hangar (aircraft select over a full orbit, 40 s): `u3a-glow-lights` rises to the number of lamps seen and stays; `u3a-glow-mem` grows while
   the per-frame `u3a-glow` falls (the orbit's low stretch); `u3a-glow-missed` small; `u3a-glow-why=none`, `u3a-rejected=0`. Eye dumps at the
   low point of the orbit: the ceiling lamps glow. Headset: no lamp turns off while the screen stays; the colour-column squares and panel
   outlines stay on their items when the head moves.
3. A remembered glow with the wrong texture or brightness: `u3agm:0` and report it (the blend factor is not part of the key [I]).

### 13.7 Risks / not proven
(a) The game-camera explanation of the glow count is inferred from the counters (no per-frame census of glows against the camera exists); the
memory fixes the symptom either way, but if the game dims glows by angle the remembered version is the last one seen. (b) Remembered glows use
the carrier's blend factor (not tracked by the proxy). (c) A lamp the game hides on purpose (an animated light) stays lit until the next cut.
(d) A remembered glow placed after its own run (no carrier in its run) can be covered only where later geometry is in front of it (the depth
test), which is the correct occlusion. (e) With u3pp:1 the panels lose the old 0.76 m relief against the text (requested); the backdrop keeps
its depth. (f) Rims assume the outline at the texture border; an inset outline gives transparent walls (no harm). (g) Nothing was compiled
in this session: compile errors surface at the next build.

## 14. Note 2026-10-31: stages 2-3 of the 3D reinterpretation ("glass and amber") - IMPLEMENTED behind `u3gp`, offline tests only, NOT run live

Design: 13.4. Code: `src/acz-vr/ui3d_groups.h` (new, pure), `ui3d.h` (options, the `Surf` placement surface), `brief3d_live.h`
(`Producer::ui3d_draw_s23_locked`, the panel groups per frame, `summary-ui3d` fields). Nothing ran in the game, SteamVR or a headset; the
DLL was not built (single test files only, see 14.3).

### 14.1 What it does (every element is still one of the game's own ps2_v quads)
* **The surface refactor [S].** `surface_point` now builds a `Surf` (plane normal + distance + tilt cosine, or the u3c cylinder) and calls
  `surf_point`; `rewrite_surf`, `rewrite_slab_surf` and `eye_scissor_surf` take an explicit surface. The layer surfaces use exactly the old
  arithmetic, so every stage-1 vertex is byte-identical (all existing ui3d / pause / hangar / select / panel / dbr3d / brief3d_live tests pass
  unchanged). The q rewrite `q' = q w_anchor / w_eye` (3.3) holds for ANY plane (the source image is affine in the anchor's view of it), so a
  turned group plane keeps exact perspective texture mapping.
* **Stage 2, panel groups (`u3gp:1`).** The previous UI3D frame's frame quads (`frame_quad`: medium, 4 vertices, z > 1; the selection bar
  excluded) are the panel rectangles (`PanelGroups`, up to 48, duplicates merged, nesting levels and top-level ancestors computed when the set
  becomes the reference). Each non-backdrop draw joins the SMALLEST rectangle containing its box centre; its pane is that group's plane:
  top-level = the tangent plane of a vertical cylinder about the anchor (radius = the text distance) at the panel centre's yaw x `u3gk`;
  nested = the top-level ancestor's orientation, `u3gn` x depth (<= 4) in front. A draw in no rectangle, and every full-screen draw (area >=
  0.30: a late tint / the z-0 dim), stays on the base surface (the u3pp:1 text plane). Backdrop draws (classifier layer 0) keep the stage-1
  path. Every point stays on its ray anchor -> flat-screen point, so from the anchor the composite is the flat image on every screen (R3), and
  an unknown structure is the u3pp:1 panel (R4). A screen change costs one frame on the previous screen's rectangles (still on the right rays).
* **Stage 3** (each knob separately; `u3gp:2|3` set them together):
  * bevels (`u3bv`): the rim / slab walls whose outward edge faces DOWN or RIGHT on the flat image get COLOR0.rgb x (1 - u3bv) (ATTRIBUTE1 =
    COLOR0, float4 bytes 16..31 [E: ps2_v disassembly, section 1]; a non-finite colour is left unchanged and counted `shade-skip`);
  * raised text (`u3tx`): non-frame draws on the text layer stand u3tx in front of their pane;
  * the raised selection (`u3sr`): the selection slab's back face lies ON its pane and its front face u3sr in front; the glyphs whose centre
    lies in this frame's selection box (drawn before them, so known at the glyph) ride on its front face (+ u3tx). u3sr 0 = the u3sel rule
    (behind the text plane);
  * pane back faces (`u3pb`, needs u3lt > 0): a rim also draws its back face u3lt behind the pane with COLOR0.a x u3pb (6 copies:
    back, 4 walls, front, as the selection slab).
* **Upload:** a multi-copy shape keeps 64 KiB of the slot's 512 KiB span free (`kShapeReserve`; the main menu is ~178 KiB flat); beyond that it
  steps down (no back face, then flat, counted `rim-flat`) before anything is refused.

### 14.2 Options (inside `xr_terrain_stage`; all default off; the default `ui3d-option` line is unchanged)
The user's value (2026-10-31 runtime.cfg) is 224 characters of the 255, so the whole look is ONE token:

| option | default | meaning |
|---|---|---|
| `u3gp:<0..3>` | 0 | 0 off; 1 = panel groups (stage 2); 2 = 1 + `u3bv 0.4; u3tx 3; u3sr 8`; 3 = 2 + `u3pb 0.35` |
| `u3gk:<0..1>` | 1 | curvature: the group plane's yaw = u3gk x the panel centre's yaw (0 = all groups face forward) |
| `u3gn:<mm>` | 5 | 0..20: a nested panel in front of its parent |
| `u3bv:<0..0.9>` | 0 (u3gp>=2: 0.4) | darkening of the down / right facing walls |
| `u3tx:<mm>` | 0 (u3gp>=2: 3) | 0..20: text in front of its pane |
| `u3sr:<mm>` | 0 (u3gp>=2: 8) | 0..50: the selection slab raised in front of its pane (0 = the u3sel rule) |
| `u3pb:<0..1>` | 0 (u3gp=3: 0.35) | rim back-face alpha factor (0 = no back face) |

An explicit knob overrides the level's value in either order (`u3gp:3;u3pb:0`). Recommended: append `;u3gp:3` (231 characters). With any of
them set, `ui3d-option` gains `;u3gp=..;u3gk=..;u3gn=..;u3bv=..;u3tx=..;u3sr=..;u3pb=..` (resolved values) and `summary-ui3d` gains, after
`rim-flat`: `;grp=<draws on a group plane>;grp-base=<non-backdrop draws on the base surface>;grp-n=<rectangles in the reference>;grp-over=<rectangles
beyond 48>;bevel=<walls shaded>;pane-back=<back faces>;sel-text=<glyphs on the raised selection>;shade-skip=<non-finite colours>`.

### 14.3 Tests (`ui3d_groups_offline`, tests/acz-vr/ui3d_groups_offline_tests.cpp, registered in src/acz-vr/CMakeLists.txt)
Real census geometry: step124 runD1 s=10 ord 1-48 (main menu head: backdrop, strips, tiles, the selection box, three glyph rows), step147
runAGU1 s=10 tail (tile, glyph rows, the header bar drawn after the text, the z-0 dim) and s=45 (a box drawn AFTER the text it covers; census
mode=tail, so the test frame is preceded by the runD1 head ord 1-7), step124 runK10 PLAYER-weapon hangar overlay (157 nested inside
[STATISTICS]). Checks: options (levels, overrides, ranges, the user's 224-character cfg + `;u3gp:3` <= 255, the default describe line
unchanged); `Surf` byte identity (layer_surf = rewrite_vertices; a yaw-0 group plane = the layer plane, three tilts); groups (K10: 8
rectangles, 157 level 1 under 158, every glyph in [SP WEAPON NAME], smallest wins, merge / cap); pane geometry (an off-centre panel's centre on
the cylinder, half the yaw at u3gk 0.5, nested 5 mm in front with the parent's normal, base fallback); ANCHOR INVARIANCE on all four real
frames for every shape (flat, rim + back face, raised selection slab, raised text) after shading: every copy's x, y, q from the anchor = the
flat image's vertex (walls: their edge's vertices) to 1e-3 GS units; coplanarity from a head 0.3 m off the anchor (a glyph and its panel's
corner < 0.05 GS units; with u3tx 3 mm a bounded 0.05..3 units); shapes and shading (exactly the bottom / right walls are darkened, only the
back face's alpha changes, positions / uv / COLOR1 untouched, NaN skipped); the PRODUCER: with u3gp:1, u3gk 0, u3gn 0 the plans are
byte-identical to stage 1 (`u3pp:1;u3lt:6`) on three real frames x 3 frames, and u3gp:1's first frame (no reference yet) too; the main menu
under u3gp:3 (the selection slab raised with its back face on its pane, rows 1-2 = 15 glyphs on its front face, row 3 u3tx in front, rims with
back faces, backdrop on layer 0); the K10 hangar under u3gp:2 (every overlay draw on its group plane, 157 nested, tint dropped,
`u3a-rejected=0`); the box-after-text menu (glyphs and tile on the box's plane, the dim on the base surface; grp / grp-base counts); the upload
step-down (180 six-copy rims: 149 fit, the rest flat, nothing refused). Result (cl /std:c++17 /W4 standalone, private dir D:\b-u3gp):
`ui3d_groups_offline: 15621 checks passed`; re-run unchanged: ui3d_offline 5999, ui3d_panel_offline 848, ui3d_select_offline 463,
ui3d_pause_offline 505, ui3d_hangar_offline 2901, dbr3d_live_offline 122, dbr3d_offline 214, brief3d_live_offline 2396.

### 14.4 Null run / headset check list (a build with this change; nothing ran)
cfg: the user's value + `;u3gp:3` (i.e. `...;ui3d:1;u3p:1;u3a:1;...;u3pp:1;u3lt:6;...;u3gp:3`).
1. `ui3d-option` = today's line + `;u3gp=3;u3gk=1.00;u3gn=5.0;u3bv=0.40;u3tx=3.0;u3sr=8.0;u3pb=0.35`.
2. Main menu, 10 s: `summary-ui3d` `rejected` unchanged against the step143 cfg; `grp-n` ~5 (tiles, strips, the box of the first frame),
   `grp` grows, `grp-base` > 0 (the first frame, the dims), `sel-text` > 0 (the selected item's glyphs), `pane-back` and `bevel` grow,
   `shade-skip=0`, `rim-flat=0`. Move the cursor: `sel` keeps growing, no rejection.
3. Eye dumps (`png-screen.now` / `xr_png_once`) with the null head at the anchor: each menu's eye image has the flat image's layout (R3);
   with the null head offset sideways (if the null driver allows): the selected item stands out of its panel, panel edges show a darker
   bottom / right bevel, the text sits on its panel.
4. Hangar (aircraft select, weapon selects): `u3a-rejected=0`, `u3a-published` as before; `grp-n` ~8 on the weapon screen; the panels wrap
   gently around the view (the right-hand PLAYER / [WEAPONS] boxes turned towards the viewer).
5. A screen change (main menu -> mission menu): one frame on the old rectangles; no rejection.
6. If anything looks wrong: `u3gp:1` (groups only), then `u3gp:0` (= the step143 look) and report which.

### 14.5 Risks / not proven
(a) COLOR0 is assumed to be the vertex colour the pixel shader multiplies (ATTRIBUTE1 per the disassembly; its scale - 0..1 or /128 - does
not matter for a multiply; if a menu shader ignores it the bevel is invisible, no harm). (b) The ps2_v pixel shader is an alpha-test variant
(`ps2AtestCA`): a back face whose alpha falls under the test threshold disappears (no harm; raise `u3pb` or set 0). (c) The menu PSOs use depth
ALWAYS, so the painter's order decides overlap: the raised selection is drawn before its text (correct), but a later panel drawn over a raised
element is painted in front of it from a moved head (a few mm of parallax). (d) Groups are the previous frame's rectangles: an animated panel
(sliding in) uses last frame's rectangle for one frame. (e) A nested panel's rim walls (u3lt 6 mm) reach 1 mm behind its parent's front
(5 mm nest): edge-on from the anchor, a hairline from steep angles. (f) The census logged 48 draws per frame (head or tail); the middle of the
347-draw main menu was never seen, so its rectangles are inferred from the head and tail. (g) The DLL was not built in this session: a compile
error in the glue would surface at the next full build (the producer header compiles in every test that includes brief3d_live.h).
