# BRIEF3D: the briefing's 3D map as a tabletop in VR (roadmap R4, D-362 (b))

Status (2026-10-28, after D-370): the LIVE WIRING is implemented behind `brief3d:1` (default off) and offline-tested with fakes; **no live,
null-driver or headset run has exercised the replay or the table layer**. Producer core `brief3d_live.h` (ctest `brief3d_live_offline`),
producer D3D12 glue `brief3d_replay_hooks.inl` (in `d3d12_hook.cpp`), consumer core `xr_brief3d_core.h` (ctest `brief3d_table_offline`),
consumer D3D12 `xr_brief3d_d3d12.cpp` + `xr_brief3d.hlsl`, proxy wiring (`xr_stereo_proxy.cpp`), present close (`ee_camera_probe_win.cpp`);
the matrix core `brief3d.h` (ctest `brief3d_offline`) and the read-only capture (`brief3d-capture.arm`, section 6.6, live PASS X1 = D-370).
Research: `briefing-3d-map.md` (option A), D-365 run R4, D-370 run X1. Tags: [E] evidence, [I] inference, [U] unknown.
**2026-10-30 (section 11):** with `b3l:1` (the default) the map draws are LATCHED and the XR consumer re-draws the table every XR frame per eye with that frame's head pose (`brief3d_latch.h`, `Producer::latch_draw`, `LatchCore`; ctest `brief3d_latch_offline`); the per-game-frame replay of sections 3.5/6 is the fallback (`b3l:0`, or automatic when the geometry is unreadable). Not built or run yet.

## 1. What the map draws are (evidence)

* **Root signatures [E]** (RTS0 parts of the live dumps `D:\acz-evidence-archive\step92\runR4-logs\shaders\*.dxbc`, decoded offline with a
  20-line parser; flags `0x33d` = IA input layout + DENY hull/domain/geometry/pixel/amplification/mesh root access):

  | shader | p0 | p1 | p2 |
  | --- | --- | --- | --- |
  | DrawSelMap_v `384ade02` (PS `9ebdf113` has the same RTS0) | root SRV t10 (vertex) | root SRV t11 (vertex) | root constants b10 x 2 (vertex) |
  | DrawModelIcon_v `d5a41c2f` | root SRV t10 (vertex) | root constants b10 x 2 (vertex) | - |

  The matrix record (t10, `StructuredBuffer<SrtData>`) is one **root SRV**: its GPU VA is element 0, and a per-eye replay only has to rebind
  that one parameter. The pixel stage reads nothing from the root (DENY_PIXEL): the PS just outputs the interpolated colour.
* **Draw counts [E]** (run R4, `briefing-observe;event=top` every 5 s, 300 presents per window on the overview): SelMap `t2i` 300 draws,
  19,507,200 indices, max 65,024 = **one indexed line draw per frame of 65,024 indices = 32,512 segments = 2 x 128 x 127: the edges of a
  128 x 128 vertex grid** [I for the grid size, E for the counts]; ModelIcon `t3i` 18,900 draws = 63 per frame, <= 65 indices (an odd
  count: strips, or ranges of a shared list) [E counts, U topology]; both constant through 20 s idle and all six POV holds (no culling).
  The 2D panels/text are `ps2_v` (`534f29ca/d34ee153`) draws and stay on the virtual screen.
* **Vertex shader maths [E: dxc -dumpbin, briefing-3d-map.md 2.1]**: `p = [x y z 1] M`, `q = p / p.w`,
  `SV_Position = ((q.x - 2048) * 2 / cb10.x, (q.y - 2048) * 2 / cb10.y, q.z * 2^-24, 1)`. The shader divides by w itself, so the rasteriser
  never clips against w: a vertex behind the eye is MIRRORED (handled by the eye guard, section 3.4). `near/w` is linear in screen space, so
  the depth of the replayed lines is exact.

## 2. Buffer readability (what the CPU can and cannot read)

| input | where | readable by the CPU? [E: D-370 run X1, 66 `brief3d-capture` lines] | needed for |
| --- | --- | --- | --- |
| t10 SrtData (64 B / 272 B) | root SRV VA | yes: `t10=ok` in all 66 lines (pinned, mapped UPLOAD; two guarded reads agree) | the fit (game camera centre + handedness), the icon camera check |
| t11 colour table | root SRV VA | not read | nothing (stays bound) |
| b10 (cb10) | root constants | yes: `b10=682,-448` | the eye projection (the game's screen divisor stays bound) |
| grid VB | IA view 0 | yes: two ALTERNATING mapped UPLOAD buffers, 520,192 B, stride 16 (= 32,512 vertices); indices 16-bit, 65,024, LINE LIST | only the bounding box (fit, once per screen, read twice) |
| PSO | creation facts | depth test+write, GREATER_EQUAL (7), stencil ON, blend SRC_ALPHA/INV_SRC_ALPHA/ADD (alpha ONE/ZERO), RTV R16G16B16A16_FLOAT (10), DSV D32_FLOAT_S8X24_UINT (20), 1 sample | the gate; our eye images use exactly these formats |
| icons | 272 B records | `eq=63.0.0.0`: matrix 0 of every icon equals the grid matrix; icon IA topology 5 (strips), <= 65 indices | matrix 0 is replaced per eye |
| camera | t10 over time | rows 1 and 3 constant, rows 0 and 2 rotate: the game camera ORBITS about model y; the model origin projects to a fixed point | confirms the fit's up axis (y) and centre [I] |

Consequence: **the replay never needs the vertex data** (the game's own draw is re-issued with only t10 swapped, like the fxmem:4 smoke
replay); the CPU reads t10 per draw and the VB once per screen for the box.

## 3. Design (option A: replay the game's draws per eye)

### 3.1 Table placement (`place`, `table_to_local`)
World-locked in LOCAL, placed when the briefing table is entered and after a LOCAL space change (recentre): at the head's yaw (pitch/roll
ignored), `b3d` metres ahead (default 0.75), table top `b3y` metres relative to the eye height (default -0.45: a seated table), map width
`b3w` metres across the longer horizontal axis (default 1.0), vertical exaggeration `b3v` on top of the game's (default 1).

### 3.2 Model fit (`box_from_vertices`, `camera_centre`, `fit_model`), latched once per briefing screen
* up axis = the box's flattest axis (refused when its extent is > 0.6 x the shorter horizontal extent: `brief3d-box-not-flat`);
* up sign = the side the game camera is on: the camera centre is the model point where the x, y and w columns of the game matrix vanish;
  refused when it is inside the height range (`brief3d-camera-not-above-map`);
* handedness: the map's two horizontal axes are projected with the game's matrix and cb10 into the game's own NDC; the table maps the second
  axis to +z or -z so that the map seen from above has the SAME orientation as in the game's image (never mirrored), whatever the game's
  model-space conventions (tested for all three up axes, both signs, mirrored and non-mirrored cameras, three yaws);
* scale = `b3w` / longer horizontal extent; the lowest grid point sits on the table top; the horizontal centre on the table centre.
The game camera keeps orbiting (D-365): the table ignores it after the fit, so the map stays still and the user walks around it.

### 3.3 Eye records (`eye_matrix`, `selmap_record`, `icon_camera_index`, `modelicon_record`)
`M_eye = model_to_table x table_to_local x view(eye) x ps2_projection(eye FOV, cb10, near 0.05 m, depth mode)`, row vectors, written as
float32 in place of the SelMap record. `ps2_projection` inverts the shader's screen mapping exactly (offline: every vertex of a synthetic grid
lands within 2e-4 NDC of an independent OpenXR projection after the float32 round trip). Depth: the PSO's depth function decides the
convention (GREATER/GEQUAL: `near/w`, cleared 0; LESS/LEQUAL: `1 - near/w`, cleared 1; EQUAL/NOT_EQUAL/NEVER refused).
ModelIcon: the record holds four matrices; the one equal to THIS frame's SelMap matrix is taken as the map camera and replaced by `M_eye`
(model matrix, lighting matrices and the vector kept). No match or two matches = the icons are not replayed (grid only) [U: run B3-5].

### 3.4 Eye guard (`eye_clear_of_volume`, `open_eyes`)
Every corner of the table volume (+0.15 m headroom for icons) must be >= 0.05 m in front of BOTH eyes; the volume is convex and w is
affine, so then every vertex is in front and the shader's own divide is safe. Otherwise neither eye is replayed this frame
(`brief3d-eye-inside-volume`): putting the head into the table makes it vanish instead of smearing mirrored lines.

### 3.5 GPU replay per map draw (`gate`, `plan_root_swap`, `replay_commands`)
Recorded inline right BEFORE the game's map draw, native vtable calls (the proxy's trackers never see them):
`[first of the frame: ClearRTV x2, ClearDSV x2]`, per eye `OMSet(eye RT, eye DSV), RSSetViewports/ScissorRects(eye image),
SetGraphicsRootShaderResourceView(t10, eye copy VA), Draw*(the game's arguments)`, then `t10 = game VA`, viewports, scissors, OM targets
restored; the game's draw follows unchanged. The PSO, IA buffers, topology, t11, b10 stay the game's: **no clone PSO** - the eye images are
created with the PSO's own RTV and DSV formats (gate: one RT, RTV 10 / DSV 20, 1 sample, depth func not EQUAL/NOT_EQUAL/NEVER, stencil
only when both faces test ALWAYS (D-370: stencil is on; the eye stencil is ours, the game's stencil ref is untracked), viewport and scissor
known so they can be restored, OM restorable). The eye images are taken COMMON -> RENDER_TARGET before and back after each replay pair
(implicit decay/promotion rules: the consumer reads them from COMMON). Per slot UPLOAD block 192 KiB: 4 SelMap copies x 2 eyes x 256 B + 128
icon copies x 2 eyes x 512 B. Memory (`budget`): per slot 2 eye images 1440 x 1440 R16G16B16A16_FLOAT + 2 depth images D32_FLOAT_S8X24
(producer-only, per slot) + the upload block, x 3 slots = 191.8 MiB (cap 200 MiB, 32 MiB per image).

### 3.6 Composition (consumer)
In the menus the vscreen hook submits the screen quad only (`ProjectionLayer::screen_only`). `Brief3dHook` wraps it: in every frame in which
the screen is planned, `TableCore` acquires the newest READY table slot whose producer fence completed, blits both eye images into the EYE
swapchains (targets 0/1, unused outside flight) and sets `ProjectionLayer::table`: `compose_layers` then submits [screen quad, projection
(premultiplied SOURCE_ALPHA, LOCAL)] with the eye poses/FOV the slot was rendered with (the compositor re-projects to the display pose).
Blit (`xr_brief3d.hlsl`): un-premultiply, sRGB-decode the game's display values, premultiply; sRGB RTV. No new slot for 500 ms (the map
screen was left) = the table layer is dropped, the screen stays. No base plate yet (an option for later).

## 4. Options (sub-options of `xr_terrain_stage`, no new config key, default off)

`brief3d:<0|1>`, `b3w:<0.2..4 m>`, `b3d:<0.2..5 m>`, `b3y:<-2.5..1 m>`, `b3v:<0.25..8>`. Removed before the other parsers; a value without
them is unchanged byte for byte. Prerequisites (`prerequisites` + the proxy): `vscreen:1`, `vsearly:1`, `xr_hud_layer`, `ee_camera_probe`
(the Present hook closes the map frame); otherwise `brief3d-option;...;live=off` and nothing changes. The terrain option now holds 255
characters (D-368), so the day value + `;brief3d:1` fits.

## 5. Offline tests (`tests/acz-vr/brief3d_offline_tests.cpp`, ctest `brief3d_offline`)

Options (incl. the day value unchanged), identity + the dumped root layouts and their refusals, the PS2 projection vs an independent OpenXR
projection (both depth modes, behind-eye detection), placement, the fit on a synthetic 128 x 128 grid (3 up axes x 2 signs x mirror x 3
camera yaws: orientation preserved, width, table top, centre), end-to-end through float32 records, the eye guard (head inside, leaning in),
icon camera matching and record substitution, root swap / command sequence / upload layout, budget, gate refusals, bounded log lines.
Live wiring (`brief3d_live_offline`, `brief3d_table_offline`): producer frames (open on the first accepted map draw after a present, close
at the present, publish when every tracked list executed, abort when all were discarded), list reset before/after execution, re-execution
poison, failed Signal quarantine, second-queue quarantine, ring back-pressure and fence loss, fit retry once per second and VB read failures,
screen-leave re-fit and recentre re-placement, the D-370 gate facts (each refusal named), the icon matrix-0 check, two recording threads +
two executing threads, the icon cap; the consumer's exact swapchain sequence (acquire/wait eye 0, acquire/wait eye 1, blit, release 1, 0),
hold until the blit fence completed, re-submit without acquisition, stale drop after 500 ms, screen hidden (flight), target_written (eye vs
HUD), wait timeout (poison), eye-1 acquire failure (eye 0 released unwritten), Signal failure (quarantine), refused GPU call, setup failure
(partial destroy), release failure, fence loss, producer dead, and the layer list (screen + table; flight unchanged).
NOT tested: the D3D12 code (no fake device for it), the OpenXR submission, the real map data.
[E] Private build D:\b-brief3d (-j2, one target at a time, commit 74%): `brief3d_offline: 594077`, `brief3d_live_offline: 791`,
`brief3d_table_offline: 112`, `vscreen_offline: 210` checks passed, `d3d12_hook_offline_tests` passed (the glue compiled into the fake-COM
hook tests, brief3d off); the edited DLL objects compile (d3d12_hook, ee_camera_probe_win, xr_stereo_proxy, xr_stereo_output,
xr_owner_openxr, xr_brief3d_d3d12 + the fxc shader). The DLL was not linked; the full ctest was not run (the executor's post-merge gate).

## 6. Live wiring (implemented 2026-10-28; not run)

1. **Producer core** (`brief3d_live.h`, one `Producer` per module): options, fit/placement latch (re-latched after 2 s without map draws =
   a new screen, and on a LOCAL space-generation change), a 3-slot `acz::vscreen::Ring`, per-slot runs (the frame's one list, recording /
   submission reservations, executions, signalled value, Reset witness) and metadata (eye poses/FOV, encoded space generation, frame), the
   fence value counter, stats and the `summary-brief3d` / `brief3d-fit` lines (section 8 invariants).
2. **Draw seam** (`brief3d_replay_hooks.inl` `brief3dDraw`, called in `hookedDrawIndexedInstanced` right before the game's draw): facts under
   the hook-state mutex (`b3Facts_locked`: classify by VS/PS hash + PSO topology, root layout, t10 read twice through the shared mapped-span
   reader, b10, PSO facts, the step-3 RS/OM track with the private OM restore handles, the grid VB box when the core asks); poses from the
   step-5 PoseProvider (`try_read_ex`, try-only) when a frame must be opened; the core's decision; our upload span written; the replay
   recorded with native calls (section 3.5).
3. **GPU objects** (`b3CreateGpu`, game thread, once, at the first map draw; `budget` checked first): RTV/DSV heaps, 6 eye colour images
   (COMMON), 6 depth images (per slot, DEPTH_WRITE), the mapped upload buffer, the fence. Any failure poisons with a name; partial objects
   are kept for the process and never used.
4. **Execute / reset / present**: `hookedExecuteCommandLists`: a batch containing pending frame lists takes the submit mutex, reserves the
   submissions, runs the native Execute, Signals the producer fence once on that queue (`brief3dBeforeExecute/AfterExecute`);
   `hookedReset`: `brief3dReset(self, hr)` (a successful Reset = the retirement witness that publishes or aborts the slot); the Present
   hook (`ee_camera_probe_win.cpp`): `brief3d_present` closes the frame to new draws; the 5 s present-stats window logs the lines.
5. **Consumer** (`xr_brief3d_core.h` TableCore + `Brief3dHook`, `xr_brief3d_d3d12.cpp`): created by the proxy for each XR attempt as
   `make_brief3d_hook(make_vscreen_hook(...))` (unchanged when brief3d is not configured); `Output::vscreen_pace` passes the eye swapchains
   (`VScreenIn::eye_images/eye_ext`); Output reports eye-target writes (`target_written(0/1)`; ScreenCore now ignores non-HUD targets).
6. Phase-2 capture before the replay: **implemented (2026-10-28), run live in X1 (D-370, PASS)**. `src/acz-vr/brief3d_capture.h` (logic, SDK-free),
   `brief3d_capture_hooks.inl` (draw glue, included by `d3d12_hook.cpp`; one hunk in `observeDraw`), the arm poll + line in
   `ee_camera_probe_win.cpp pacing_tick`; ctest `brief3d_capture_offline`. Trigger: the file `%LOCALAPPDATA%\acz-vr\brief3d-capture.arm`
   (polled every 5 s present-stats window; no config key). READ ONLY: no command is recorded, nothing is written to the GPU or game memory.
   While armed, ONE frame per second is sampled (the draws between two presents [I]); a sampled frame with a SelMap draw gives one line
   `brief3d-capture;...` in `ee-camera.log` (worst case 405 B, offline-tested; the diagnostics log copy is cut at 448), a frame without one
   samples the next frame instead. Draws are identified by the VS creation hash (as briefing-observe). Reads: the bound root must pass
   `root_layout_check`; t10 VA from the tracked root SRV, resource from the setter-time address registry, then `step3MappedSpan_locked`
   (pinned, mapped UPLOAD, span inside the resource) + two `guardedCpuRead`s that must agree (the fxmem:1 smoke rule); every failure is named.
   The first SelMap draw: 64 B record, cb10, PSO and IA facts, VB view (span check only, no vertex byte read); up to 128 icon records (272 B).
   NOT logged (still [U]): predication state, the VB/IB heap type when not a mapped UPLOAD, the grid's bounding box.
   Not armed: one relaxed atomic load per draw and per present. Armed but not sampling: the same load plus a 5 s file poll.

## 7. Live check list (lead's driver; null driver first, then headset; nothing below has run)

Route (D-89/D-90, route77, X1 walk with Down/Enter, briefing-3d-map.md 1): launch (skip-intro) -> main menu -> CAMPAIGN -> Continue ->
document cinematic (`back`) -> map presentation (`back`) -> #02 ANNEX title card (auto) -> **MISSION ROSELEIN overview with the 3D map**
(the camera orbits by itself) -> `back` -> Mission 02 menu -> **BRIEFING** (also draws the map) -> `back` -> START MISSION ... (route77 from
+56 s). Config: the day install (step96+, terrain option incl. `vscreen:1;vsearly:1`) + `;brief3d:1` appended to `xr_terrain_stage`
(fits: 255 chars). SteamVR first (vsearly needs the XR session in the menus). Logs: `xr-stereo.log` (option, table summaries),
`ee-camera.log` (`brief3d-fit`, `summary-brief3d` every 5 s).

* L1 option: `xr-stereo;event=brief3d-option;ok=1;brief3d=1;b3w=1.00;b3d=0.75;b3y=-0.45;b3v=1.00;prerequisites=ok;
  live=producer-configured(table-on-the-screen-layer);eye=1440x1440;rtv=10;dsv=20;slots=3` once; `vsearly-config;effective=1`; no
  `terrain-disabled`. Main menu: `summary-brief3d;frames=0;...;poisoned=0` (no map draws: nothing happens); game fps unchanged.
* L2 overview: within ~1 s one `brief3d-fit;outcome=ok;reason=none;vertices=32512;box=...;up=1+;a1=0;a2=2<sign>;m-per-unit=...` (record the
  box: y should be the flat axis, x/z symmetric about 0 [I from D-370's orbit]); then every 5 s `summary-brief3d` with frames ~300,
  published ~300, replays ~19,200 (64 per frame), refused ~0, `other-list=0` (else the map spans several lists: record it), `executes`=
  `signals`, reexecutes/discarded/cancelled/invalidated/quarantined 0, `poisoned=0;why=none`. Refusal names if not:
  `brief3d-viewport-unknown` / `brief3d-om-unknown` (the step-3 RS/OM track is not active in the menus: needs the gpu_copy step-3 hooks),
  `brief3d-pso-stencil-test` (the stencil test is not ALWAYS: record front/back func), `brief3d-no-pose` (no XR pose yet), fit reasons
  (`brief3d-box-not-flat`, `brief3d-camera-not-above-map`, `b3-vb-*`), `brief3d-icon-camera-*`.
* L3 consumer (`xr-stereo.log` every 5 s, TWO lines since batch Z: the combined line was cut at 489 chars in Z1): (a)
  `summary-brief3d-table;t-ms=..;blits>0;blit-failed=0;layers~XR frames;no-image` small;`stale=0` while on the map;
  `poisoned=0;quarantined=0;why=none;frame=<slot frame>;` followed by the producer's fit tail
  `fits=..;fit-refused=..;placements=..;no-pose=..;no-slot=..;screens=..;fit-why=none`; (b) right after it
  `summary-brief3d-producer;frames;published;aborted;replays;selmap;icons;icons-unverified;refused;other-list=0;executes=signals;
  reexecutes=0;signals;discarded=0;cancelled=0;invalidated=0;quarantined=0;acquired;lag-frames;lag-max;age-ms;age-max-ms;poisoned=0;why=none`.
  Lag (at the consumer's last acquire): `lag-frames` = producer frames opened after the acquired one (expect 0-1, `lag-max` <= 2),
  `age-ms` = the slot's age on the producer's Present clock (GetTickCount64 at the last Present; expect < 50, record `age-max-ms`).
  Both lines are < 480 B for any value (counters clamp at 9999999, replays/icons at 999999999; offline-tested worst case 478/460 B).
  `ee-camera.log` keeps its single `summary-brief3d` line (unchanged). `summary-vscreen;mode=screen` unchanged (`layers`, `copies` as
  without brief3d).
* L3b XR image of the screen + table (SCREEN-DUMP, vscreen.md section 9; no config key): with the table on the screen, create
  `%LOCALAPPDATA%\acz-vr\png-screen.now` (any content). Within ~1 s: `xr-stereo;event=png-screen-armed;outcome=armed;why=none;table=armed`
  (the file is deleted), then `png-screen;part=screen;outcome=written;size=2400x1350;file=acz-vr-xr-screen-t<ms>-screen.png` and
  `png-screen;part=eye0-table;outcome=written;size=<eye w>x<eye h>;file=...-eye0-table.png` in `<logs>\png`. Read BOTH with
  `xr_dump_upright.py`: the screen PNG is written rotated 180 degrees, the table PNG as stored like the xr_png_once eye dumps (an eye image
  rendered at the located pose is itself rotated 180 degrees in the live runs; Q1a: written rotated it came out upside down, far edge at
  0.32 H instead of the 0.68 H the placement gives). Upright: the table in the LOWER half, wide near edge at the bottom. The table PNG is
  the premultiplied left-eye projection layer over black. `table=table-not-live`
  = the screen part alone (no table shown); `outcome=refused;why=no-screen-image` = the screen layer had no image yet (create the file again).
* L4 headset: a table ~0.75 m ahead, ~0.45 m below eye level, ~1 m wide, the yellow grid and the unit icons in 3D, NOT mirrored against the
  monitor image (compare one landmark), the game's orbit does NOT rotate the table; walk/lean around it; colours like the monitor; the
  screen quad behind stays readable; the table does not swim with head motion (pose = the slot's predicted pose). Head into the table ->
  the table disappears for those frames (refused `brief3d-eye-inside-volume`), no smeared lines. SteamVR recentre -> the table is placed in
  front again at the next frame. Tunables: `b3w:1.5`, `b3d:1.0`, `b3y:-0.6`, `b3v:3` change size/position/height exaggeration.
* L5 BRIEFING and back: leaving the overview: table gone within 0.5 s (`stale` grows), the screen stays; entering BRIEFING: a new
  `brief3d-fit` line (screen gap > 2 s) and the table again.
* L6 START MISSION: no map draws (frames stop growing), flight composites as before (`summary-vscreen;mode=flight`), no `brief3d` refusals
  counted in flight, the first flight frame's eye images show the cockpit (no table remnants), hooks cost unchanged in flight.
* L7 cost on the overview: hooks ms with/without `brief3d:1` (63 icon + 1 grid replays x 2 eyes per frame; upload writes 64 x 2 x <= 272 B);
  record `present-stats` fps; memory: + ~192 MiB GPU at the first map draw (commit gate as usual).
* L8 exit from the menu with the table shown: `brief3d-table-result;...;poisoned=0` + its `summary-brief3d-producer` line, then
  `retire;outcome=clean` (where retire lines exist).

Earlier checks (kept for reference):
* B3-1 (now): `xr-stereo.log` `brief3d-option;ok=1;brief3d=1;b3w=1.00;...;prerequisites=ok` once; no
  `terrain-disabled;reason=bad-stage-option`; `vsearly-config;effective=1`.
* B3-2 (phase-2 capture, section 6.6; implemented, never run): create `%LOCALAPPDATA%\acz-vr\brief3d-capture.arm` before or during the run
  (armed within 5 s; delete it to stop). On the overview and on BRIEFING, `ee-camera.log` gets about one line per second:
  `brief3d-capture;sel=<SelMap draws>;cls=<1 = brief3d::classify agrees>;ix=<index bytes>*<index count>;t10=<ok|reason>;m=<16 floats, row
  major>;b10=<x,y|reason>;pso=<depth-en><depth-write><stencil>,<depth func>,<blend-en>:<src.dst.op>:<srcA.dstA.opA>;rt=<count>:<RTV0 fmt>,
  <DSV fmt>,<samples>;tt=<PSO topology type>;ia=<IA topology>;vb=<va>+<bytes>/<stride>:<mapped-upload|reason>;ic=<icon draws>/<read>/
  <unread>;iia=<first icon IA topology>;imax=<max icon index count>;eq=<icons whose matrix 0.1.2.3 equals m>;cam=<one/none/several
  matches>;iw=<first icon refusal|b3cap-no-grid-matrix|none>`. Expect: `sel=1;cls=1;ix=2*65024` (or `4*`), `t10=ok` (if not: the reason
  names the step: `b3cap-root-*`/`brief3d-root-*` layout, `b3cap-t10-unbound`, `b3cap-t10-address-unresolved`, a `step3MappedSpan_locked`
  reason such as `not-upload-buffer`/`not-mapped`/`binding-observer-disabled`, `b3cap-read-fault`, `b3cap-record-torn`), m stable while the
  camera is still and changing while it orbits, b10 = the game's screen divisor, `pso=` depth func 5/7 or 2/4 (anything else refuses the
  replay: section 3.3), `rt=1:<fmt>,<fmt>,1`, `tt=2;ia=2`, the VB view about 256 KiB stride 16; `ic=63/63/0`. Record: the VB verdict (mapped
  UPLOAD or not: section 2), `iia` (4 list / 5 strip) and whether `cam=63/0/0` with one `eq` column at 63 (B3-5 answered in the same line).
  Still needs another check: predication off, viewport/scissor, the grid box (expect ~128 x 128 vertices, a flat axis; record which axis and
  sign).
* B3-3 (fit): `brief3d-fit;outcome=ok;vertices=16384;up=<axis><sign>` stable across the whole tour and both screens (overview, BRIEFING).
* B3-4 (flatness): the box height / width ratio (the 0.6 refusal threshold).
* B3-5 (icons): `icon_camera_index` matches one matrix in every ModelIcon record: the B3-2 line's `eq=` (per-matrix histogram) and `cam=`
  (one/none/several) answer it with the same 1e-5 relative rule; if `cam=0/63/0`, record the four matrices of one draw at two POVs to find
  the camera (not logged by the capture).
* B3-6 (replay, headset): `brief3d-frame;selmap=1;icons=63;replays=64;refused=0;eyes=1` per frame; the table 0.75 m ahead, below eye
  level, map not mirrored against the monitor image, lines stable while the game camera orbits, walking around it works; head into the
  table -> it disappears (`brief3d-eye-inside-volume`), no smeared lines; the panels stay readable on the screen behind.
* B3-7 (leave): START MISSION -> no map draws -> table gone within 2 s, flight composites as before (`mode=flight`), no `brief3d` refusals
  in flight; game exit clean (`retire;outcome=clean`).
* Hooks cost on the overview < 1 ms with brief3d on (63 icon replays x 2 eyes per frame).

## 8. GPU lifetime invariants (for Astra) and risks

Invariants (producer = `brief3d_live.h` + `brief3d_replay_hooks.inl`; consumer = `xr_brief3d_core.h` + `xr_brief3d_d3d12.cpp`):
1. Objects: every producer object is created once (game thread, proxy creation scope, native calls) and never released or recreated
   (process lifetime); the consumer's objects are created at its first blit and destroyed only in `retire()` after its own fence reached the
   last value (also after a partial setup); the destructor makes no foreign call.
2. One writer list per slot: a frame is bound to ONE game command list (its first accepted map draw); map draws in any other list while it
   is open are refused (`brief3d-frame-other-list`), and a list that still holds an unretired frame cannot start another
   (`brief3d-list-holds-previous-frame`). A slot's eye images, depth images (PER SLOT) and upload span are written only by that list's
   recording; the CPU writes the upload span only under a recording reservation of a WRITING slot (`Ring::begin_write` never returns a HELD
   slot or one whose last published value has not completed).
3. Clear before replays: the frame's first granted plan carries the clear of both eye colour and depth images; every later replay of the
   frame is recorded after it in the same list, so command-list order puts the clear first in every execution. A cancelled clear-bearing
   plan invalidates the frame: executed -> the slot is quarantined, never executed -> aborted.
4. Reservations before mutation: `draw()` takes a recording reservation before returning permission (ended by `commit`/`cancel`);
   `before_execute()` takes a submission reservation per pending frame list before the native Execute (ended by `after_execute` with the
   Signal outcome). No settlement while either is outstanding.
5. Publication only after retirement: a slot is published only after a SUCCESSFUL Reset of its list (the recording can never execute
   again) with no reservation outstanding, using the highest value signalled for ANY execution of that recording (re-executions before the
   Reset raise it). Until then the slot stays WRITING: never acquired, never reused. Reset before any execution = discarded (aborted, no GPU
   work referenced it). A list that is never reset keeps its slot WRITING (fail closed: fewer slots, eventually `brief3d-no-free-slot`).
6. One queue, every execution: the queue check runs for first AND repeated executions before any Signal is requested; another queue
   quarantines the slot for ever (no Signal on the shared fence, never published or reused) and poisons. All our Signals are serialized by
   the submit mutex. A failed Signal quarantines and poisons. The consumer acquires only READY slots whose value completed (loss = UINT64_MAX
   is never completion), and `acquire` refuses under the same lock once the producer is poisoned.
7. Reader exclusivity: the consumer holds one slot from acquisition until its own blit fence value completed; while HELD the slot is never
   chosen for writing. The eye images rest in COMMON between replays (COMMON -> RENDER_TARGET -> COMMON around each replay pair inside the
   game's list), so the consumer's queue reads them by implicit promotion to PIXEL_SHADER_RESOURCE.
8. LOCAL generation: a slot carries the encoded space generation of its poses; the consumer refuses a slot of another (or unknown)
   generation before any eye acquisition, and stops submitting already-blitted content when the generation changes (a held source is still
   released only after its blit fence completed).
9. Swapchains: both eye acquisitions are settled inside one `frame()` (wait succeeded -> blit -> release); a wait that never succeeds or a
   failed release poisons (Output fails the session; retire retains). The table is submitted only while both eye swapchains' last released
   images are this core's completed blit (`content_ok`; cleared by Output's eye writes via `target_written` and by a generation change).
10. Game resources: the replay reads only what the game's own draw reads (VB/IB/t11/PSO/root signature) INSIDE the game's own list, so the
   game's lifetime guarantees for that list cover our draws; nothing of the game's is referenced by any list of ours.
11. Fail closed: any unknown fact refuses the draw (the game's draw is untouched; the frame then has fewer replays or none and is aborted);
   poisoned = the producer records nothing more for the process; the screen quad is unaffected.

Astra review 6460640585 (5 P2) is addressed by invariants 2-6 and 8 and the split-phase fixtures in `brief3d_live_offline` / `brief3d_table_offline`.
Risks / not proven: (a) [U] the game's map draws may span more than one command list per frame: the extra lists' draws are refused
(`other-list` counter; grid or icons missing) - live check L2 tells; (b) publication waits for the list's Reset: latency = the game's reset
cadence [U] (typically 1-3 frames; a list never reset = no table); (c) the step-3 RS/OM track may be inactive in the menus (then everything
refuses `brief3d-viewport-unknown`: no harm, no table); (d) the eye images use the game's float format and the PSO's alpha
blend: overlapping lines accumulate alpha slightly differently than a single layer (cosmetic); (e) the producer's eye poses are the latest
located views at record time (one frame older than display): the compositor re-projects; (f) GetCPUDescriptorHandleForHeapStart is called
directly on our own heaps (the vscreen pattern); (g) Output must never re-submit the eye swapchains after the table wrote them: its own
re-submit window (500 ms after the last fresh composite) ends before vscreen shows the screen [I].

## 9. Zoom: aerial icons missing after the user zooms (headset test 2026-10-28, user-report item 1)

**Evidence [E]** (`D:\acz-evidence-archive\headset-2026-10-28\xr-stereo.log` / `ee-camera.log`, `summary-brief3d` deltas per 5 s window):
* Game icon draws per game frame (icons delta / SelMap delta): 63 through t=75 s, then 21.5 (t=80), 16 (t=85), 17.5 (t=90), 21.6 (t=95), 4.4 (t=100).
  The count is taken before every gate (`++st_.icons` precedes the refusals), so the GAME stopped submitting about 40 of 63 icon draws at the zoom.
* After the zoom nothing is refused: `refused` is flat at 47680 -> 47922 and `icons-unverified` flat, `icons-other-ps=0` (batch I). So the icon-camera
  gate (`brief3d-icon-camera-*`), the PS check and the depth/PSO gates did NOT reject the aerial icons; they were never drawn.
* A separate defect is visible BEFORE the zoom: t=40..80 s, `last-why=brief3d-eye-inside-volume`, `refused` ~ `icons-unverified` (a refused frame
  opens no slot, so every icon of it counts as unverified): the head was inside the table volume (b3d=0.75, b3y=-0.45, b3w=1.0), whole frames
  (grid + icons) were refused and the table showed the last published frame (`stale` counts up). Not the zoom cause; lean-in refuses the frame.
* No input is logged anywhere, so the exact zoom moment is only known from the draw-count step (t~76-80 s on the log clock).

**Most likely cause [I]:** the briefing game code culls or switches icon classes by the zoomed view (frustum/range or a mode that hides air units);
the draw counts keep changing with the camera afterwards (21 -> 16 -> 22 -> 4), which fits view culling better than a fixed class switch. The replay
can only show what the game submits, so no gate change helps. If the census (below) shows the missing groups are the same index counts as the
pre-zoom groups at other model heights (y), it is game culling by position; if a whole index-count group disappears it is a class switch.
Fix options after the census: (a) find and defeat the game's zoom cull in EE memory (needs the census plus a memory search); (b) option B of
`briefing-3d-map.md` (draw the units from the `.bfn` list ourselves).

**New diagnostic (offline-tested, `brief3d_capture_offline`):** with `brief3d-capture.arm` armed, every sampled frame now also logs
`brief3d-icons;draws=N;read=N;groups=G;more=M;nonfinite=K;c<indexcount>:n<icons>:y1=<lo>..<hi>:y2=..:y3=..` (up to 5 groups by index count = icon
model; y = translation-row y of matrices 1..3; matrix 0 is the camera). Compare the line before and after the zoom.

**Live capture procedure (producer-only is enough; the table need not be worn):**
1. `brief3d-capture.arm` created in `%LOCALAPPDATA%\acz-vr` before launch; `briefing-observe.arm` also (top keys per 5 s window).
2. Route as section 7 / `runF3.ps1` up to the ROSELEIN overview ("Left stick: Change POV" visible). Idle 15 s: expect `brief3d-capture ... ic=63/63/0 ... cam=63/0/0`
   and one `brief3d-icons` line per second with the 63 icons in a few groups. Note the groups (pre-zoom baseline).
3. The zoom control is NOT documented anywhere in the repo (the earlier driver found none; the footer lists only Left stick = POV and Menu = proceed).
   Probe one input at a time, 3 s hold or 1 tap, 6 s idle after each, screenshot after each, and watch `ic=` (icon draws per sampled frame) for a step:
   keyboard driver actions in this order: `look-up` and `look-down` (numpad 8/2 = right stick Y: the usual PS2 zoom), `look-left`/`look-right`,
   `view` (Tab), `accelerate` (Space = cross-class button), `confirm` (Enter); then, with the XInput bridge/pad route (Player 1 = controller; names in
   `xinput_bridge_protocol.h`): `right-thumb` (R3 click), `right-shoulder`, `left-shoulder`, right stick up/down, `x`, `y`, `b` (the user said "clicking").
   Record which input changes `ic=` (63 -> ~21) and the screenshot shows the zoom; that is the zoom binding.
4. After the zoom hold 15 s idle and keep the `brief3d-icons` lines: the groups with a reduced `n` or missing `c<count>` are the vanished icons;
   also read `summary-brief3d` (`icons`, `refused` must stay flat: expect refused delta 0 when the head is out of the table).
   Press the same input again (or the opposite) to un-zoom: icons must return to 63 (proves reversible game-side culling).
5. Expected counters: pre-zoom `icons` delta = 63 x SelMap delta (+300 per 5 s per 300 frames -> 18900); post-zoom well below; `last-why` must NOT
   be `brief3d-icon-camera-*`. If it is, the icon camera differs after the zoom: take `eq=`/`cam=` from the capture line.
6. Keep the head out of the volume (or `b3d` >= 1.2) so `brief3d-eye-inside-volume` does not mask the counts; then delete both arm files.

### 9.1 Reconciled (2026-10-29, offline from the N2 logs): the zoom is a magnified own-flight view; the table put its icons above the eye

**Evidence [E]** (`D:\acz-evidence-archive\step115\runN2\logs\ee-camera.log`, `brief3d-capture` + `brief3d-icons`, one line per second):
* Overview (every line from t=37882 s to the tap at 38009.5): `ic=63/63/0`, 4 groups: `c15:n12:y3=1.582..3.164` (12 raised units = aircraft),
  `c24:n34`, `c64:n6`, `c65:n11` (51 units at y3 0..0.063 = ground). Grid fit `box=-32,0,-32..32,0,32` (a 64-unit grid), `m-per-unit=0.01562`.
* After the `accelerate` tap (38010.3 s onward, 7 lines, steady): `ic=2/2/0;imax=15;eq=2.0.0.0;cam=2/0/0`, ONE group `c15:n2:y3=47.53..47.91`.
  `briefing-observe` window: `modelicon=703` over 301 presents (2.3 per frame). The grid matrix keeps its rotation rows (orbit, same scale);
  only its translation row changes (`w` 66.24 -> 97.59). No refusal (`refused=0`), the icon camera matched (`cam=2`).
* The game's own 2D image (`N2-zoom-accelerate.png`, right half) shows exactly two icons, PIXY and CIPHER (the player's flight; the names are
  `ps2_v` text on the virtual screen), over a close-up of a mountain. **So the ORIGINAL zoom view shows only the units inside the zoomed region
  (here 2), not all units**: the game draws all it intends to draw; there is nothing for us to un-cull.
* The two remaining icons sit 15.1x higher than the same aircraft in the overview (47.9 / 3.164; CIPHER is at 4000 m in `m02a.bfn`:
  3.164 units = 7.9e-4 per metre before, 0.0120 after): the zoom magnifies the region ~15x in the grid's -32..32 model space [I].
* User session (`headset-2026-10-28`, step105): icons/SelMap per 5 s window before the zoom 63, then 54.5, 15.5, 7.0, 23.3, 13.5, 7.2, 4.3
  (recomputed from `summary-brief3d` deltas): a mix of 63-icon (overview) and 2-icon (zoomed) frames, i.e. the user toggled the zoom several
  times [I]; the steady zoomed value is 2, as in N2.

**Reconciliation [I]:** both reports describe the same state from two sides. On the flat screen the game shows the two AIR icons (PIXY/CIPHER)
over the magnified terrain. On the table the latched fit maps the magnified 64-unit grid onto the same 1 m table (the terrain now fills it:
"only ground"), and the two aircraft land 47.9 x 0.0156 = **0.75 m above the table top, i.e. ~0.3 m ABOVE the seated eye** (table top
b3y = -0.45): outside the view of a user looking down at the table ("the aerial icons don't load"). The user's "only ground" is the terrain,
not ground icons (the report's paraphrase added "icons"). The game's draws and our replay were correct; the table's vertical placement was not.

**Which route [decision input]:** (A) defeating a cull: there is none to defeat (the 2D zoom view shows the same 2 units), and no game code is
patched. (B) drawing all units from an EE list / the `.bfn`: would show units that the original zoom view does NOT show, and they would lie
outside the magnified grid (off the table). Not needed for fidelity. The EE icon list was therefore not searched; if wanted later, the static
`.bfn` list (briefing-3d-map.md section 2) is the offline source, not EE memory. Implemented instead: the **zoom-aware content fit**.

**Zoom-aware content fit (implemented, offline-tested, not run live; the UNIFORM scale below is replaced by the vertical-only fit of 9.2):** `brief3d.h` `icon_height` / `content_max_height` / `content_scale` /
`scaled_fit` / `content_box`, wired in `brief3d_live.h Producer::draw`:
* every icon draw with a readable record (refused or not) reports its height above the fit origin from matrix 3's translation (`kIconModelMatrix`
  = 3 [I, census: matrices 1/2 constant for every icon]; positions > 8 m or non-finite are ignored);
* when a frame OPENS, the highest icon seen since the last opened frame is latched with hysteresis (grow at once to top x 1.1; shrink only when
  below half), and the table is scaled UNIFORMLY (no distortion) about the table-top centre so the content top is at most
  `content_max_height = max(0.10, -b3y - 0.10)` = 0.35 m above the table (0.10 m below the eye); floor 0.1. The overview's 3.16 units =
  0.05 m is never scaled (byte-identical records, tested). Zoomed Roselein: latched 52.7 units -> scale 0.425, the table 0.43 m wide, PIXY/
  CIPHER 0.35 m above it;
* the eye guard volume grows to the content top (`content_box`), so leaning into the raised aircraft refuses the frame as before;
* committed only when the frame really opens (a refused open keeps accumulating); reset with the fit when the screen is left (2 s gap);
* one `brief3d-content;top-units=;scale=;height-m=;width-m=;max-m=;changes=` line in `ee-camera.log` per change (present-stats window).
Lifetime: pure matrix maths inside `draw()` under the producer mutex; no new reservation, list, slot, Signal or Reset path (invariants 1-11
unchanged). One frame of latency at a zoom toggle (that frame uses the previous scale).
Not covered [U]: the zoomed grid's own relief (the VB box is read once per screen, y 0..0 in the overview; mountains taller than the flight
would exceed the guard volume): the live check below reads it.

**New read-only census line:** with `brief3d-capture.arm`, every sampled frame also logs
`brief3d-icons-pos;read=;groups=;more=;c<count>:n<n>:x=lo..hi:z=lo..hi:s=lo..hi` (matrix 3 translation x/z and element 0 = icon scale;
worst case 376 B, offline-tested).

**Live check (producer-only, null driver is enough; then headset):**
1. Arm `brief3d-capture.arm`, route to the Roselein overview (section 7 / `runN2route.ps1`), 15 s idle: `brief3d-content` absent (scale 1),
   `brief3d-icons-pos` c15 x/z inside -32..32, s = the overview icon scale.
2. Tap `accelerate` (Space / cross): within 1 s one `brief3d-content;top-units=52.7;scale=0.425;height-m=0.35;width-m=0.425;...` line;
   `brief3d-icons-pos` c15 n2: x/z inside -32..32 (PIXY/CIPHER near the region centre), s ~15x the overview's if icons are magnified too;
   `summary-brief3d` `refused` flat, `last-why` not `brief3d-eye-inside-volume` with the head at rest.
3. Tap again (un-zoom): a second `brief3d-content` line with `scale=1`, `ic=63`.
4. Headset: in the zoom the table shrinks to ~0.43 m and both aircraft icons with their stalks are visible ~0.35 m above it while looking at
   the table; the overview table is unchanged. Screenshot both states (mirror capture) for the user.
5. If mountains poke through the guard (`eye-inside-volume` while not leaning) or rise above the aircraft: add a VB box re-read on a content
   change (the fit's existing `wants_box` path) and take the content top as max(icons, grid relief).

### 9.2 Headset 2026-10-29 item 4 ("now aerials don't render at all, it just zooms out"): vertical-only content fit, air icons with stalks

**Evidence [E]** (`D:\acz-evidence-archive\headset-2026-10-29-session`, step123; no capture armed in that session):
* `xr-stereo.log` `summary-brief3d-producer` 5 s deltas: before the zoom ~63 icons per SelMap frame, then steady **2 icons and 3 replays per frame**
  (1 grid + 2 icons) with `refused` and `icons-unverified` flat at 0; `ee-camera.log summary-brief3d`: `icons-other-ps=0`, `last-why=none`.
  So both aircraft icons WERE classified (VS d5a41c2f + PS 9ebdf113), passed every gate and were replayed into both eye images. Nothing was
  refused, no other PS or root layout is involved, no `brief3d-content-too-tall`.
* `ee-camera.log brief3d-content`: `top-units=52.7;scale=0.425;height-m=0.35;width-m=0.425` (changes 10 within the first 5 s = the game's zoom
  animation growing the latch step by step), un-zoom `scale=1` at +70 s, a second zoom `52.83/0.424`, un-zoom; second briefing `39.66/0.5648`.
  The 9.1 fit did exactly what it was designed to: it SHRANK THE WHOLE TABLE to 0.43 m (the user's "it just zooms out").
* Icon size (census `brief3d-icons-pos`, runs P1/S1): the zoomed c15 icons keep matrix-3 element 0 = 1.41 (scale 2 rotated 45 deg; overview
  1.73 / 2): the icon MESH is not magnified with the zoom. At the uniform 0.425 the arrows were 2 x 0.0156 x 0.425 = 1.3 cm, 0.32 m above a
  0.43 m table, ~20 deg above the gaze on the table: practically invisible.
* "UI in front" [I, strong]: the game's zoom image (`step115/runN2/zoom-compare.png`) shows two small arrowheads, each on a long vertical STALK
  down to the terrain, plus the names. `briefing-observe` (N2): ps2_v `534f29ca/d34ee153` GS line draws (`t2ig`) fall from 5117 to 2149 per
  301 presents = **17 -> 7.1 per frame = 5 + one per aircraft (12 -> 2)**, `t3i` text 1 -> 2 per frame. The stalks and names are 2D ps2_v
  draws on the virtual screen (already-projected PS2 screen coordinates: no 3D position, cannot be replayed into the table). The arrowheads
  are the DrawModelIcon_v draws (replayed). So the user is right that the visible "aerial" part of the game's zoom view is UI-pass lines; the
  table had only the tiny arrowheads.

**Fix (implemented, offline-tested, not run live):**
1. `squashed_fit` replaces `scaled_fit` in `Producer::draw`: the content scale is VERTICAL only. The table keeps `b3w` width (the magnified
   terrain fills it, like the game's zoom); heights x kv so the content top stays at `content_max_height` (0.35 m at b3y -0.45: 0.10 m below
   the eye). The guard volume follows (`volume()` height uses vexag x kv). `brief3d-content` now prints `mode=vertical` and `width-m` = b3w.
2. `icon_copies` rewrites matrix 3 (the icon model matrix, [I] from the census) per replay copy: (a) every icon: the up column divided by kv, so
   no mesh is flattened by the squash; (b) AIR icons (> 0.08 m above the table top; the overview aircraft at 0.05 m are not): grown so the
   mesh unit is >= 4 cm (x1..x4), height clamped to the cap (never above the head, even between latches), x/z clamped onto the grid box;
   (c) a STALK: kStalkDots = 8 extra copies of the same draw, the mesh at 1/4 size, evenly spaced from the table top to the aircraft
   (mesh-agnostic: the icon VB is never read). Dots only while the frame's icon copies stay <= 64 (the overview's 63 ground/air icons always
   fit in the 128-copy span). Ground icons at kv 1 are byte-identical (copies 1): the overview records are unchanged (tested).
3. Replay plan: one icon plan may carry `copies` (1..9) records per eye; `plan_root_swap` checks the whole copy span (cap and slot span);
   `replay_commands` emits per eye OM/VP/SC once then {RootSRV(copy c), Draw(game args)} per copy (kMaxCmds 50); the glue writes the copies
   into the slot's upload span before recording. Lifetime: UNCHANGED - one plan = one recording reservation (commit/cancel as before), the
   same list, the same slot span (writes only inside the checked span, under the WRITING reservation), the first plan still carries the clear,
   no new list, Signal, Reset or GPU object (invariants 1-11 hold as written in section 8). Counters `air=`/`dots=` in `summary-brief3d`.
Tests: `brief3d_offline` (squash geometry, guard height, icon_copies: shape preserved x g, cap/box clamps, dots strictly between table and
aircraft, room limit, ground byte identity, non-finite passthrough, copy-span refusals) and `brief3d_live_offline` (`test_zoom_air_icons_stalks`:
overview one copy each / no counters, zoom plans 1+8 copies at consecutive indices inside the slot span, matrix 0 = eye matrix and matrices 1/2
+ vector = the game's in every copy, command sequence). Private build D:\b-br4: brief3d_offline 611213, brief3d_live_offline 828,
brief3d_table_offline 195, brief3d_capture_offline 255 checks passed, d3d12_hook_offline_tests passed (the glue compiles in it).
NOT covered [U]: the icon mesh's own extent/origin (dots assume the mesh is centred near its model origin; if the arrow's tip is at the origin
the dots sit slightly off the line - cosmetic), the zoomed grid's relief (still not read; the kv cap is computed from the aircraft as in 9.1).

**Live check (null driver first, then headset; producer-only steps need no headset wear):**
1. Overview: no `brief3d-content` line, `summary-brief3d` `air=0;dots=0` growing never, icons 63/frame, replays 64/frame (unchanged).
2. Tap `accelerate` (zoom): `brief3d-content;...;scale=0.4xx;mode=vertical;height-m=0.35;width-m=1;...`; per 5 s window `air` += ~600 (2 x 300)
   and `dots` += ~4800, replays ~ 1 + 2 x 9 = 19 per frame, `refused` flat (head out of the volume), `poisoned=0`, `invalidated=0`.
3. Headset: the table stays ~1 m wide (no zoom-out), the magnified terrain flattened to <= 0.35 m, two ~4-5 cm arrows ~0.35 m above it with a
   dotted vertical line down to the table under each, both below eye level and visible while looking at the table; un-zoom: dots gone, table
   as before. Mirror-capture both states for the user (L3b `png-screen.now` gives the eye0-table PNG).
4. If the dots look wrong (offset from the arrow, or the arrow points away from them): arm `brief3d-capture.arm` and record one
   `brief3d-icons-pos` line in the zoom; the follow-up is a read-only icon VB extent census (not implemented: the icon VB's heap type is [U]).

## 10. The table shakes (headset test 5, 2026-10-30, user-report item 3): held content scale, stable anchor (2026-10-30)

**Evidence [E]** (`D:\acz-evidence-archive\headset-2026-10-30-session\live-1936-step137`, `ee-camera.log` / `xr-stereo.log`; offline reading, nothing run):
* Movement is NOT the anchor. Over the whole briefing (t=182627..182790 s) `summary-brief3d-table` shows `fits=1;placements=1;screens=0`, `gen-refused=0;invalidated=0`,
  `summary-vscreen` `placements=1`: the placement is latched once from the head at entry (`place()`, brief3d_live.h `draw`, per LOCAL space generation) and
  never moves; it is not tied to the game camera or the E6/aircraft camera (E6 sat at `searching`, `aircraft-at-origin`), and no recentre happened.
  Same in live-1144 and live-1508 (`placements=1`).
* Rendering IS unstable, in the measured content scale. `brief3d-content;top-units=32.92;scale=0.6804` at the fit, 5 s later `top-units=0;scale=1`
  (`changes` 4 -> 5): the whole terrain's vertical scale jumped 0.68 -> 1. The game omits icons from most map frames (182627 -> 182632: map frames
  205 -> 240 = 35 in 5 s, icons 2012 -> 2024 = 12, so most frames saw no icon). A frame without icons read `top = 0`; the old rule (`top < 0.5 x latched`
  -> 0 at once, brief3d_live.h `draw`, content latch) dropped the scale to 1 on that frame and the next icon frame snapped it back, so terrain heights and
  every icon pulse. (live-1508: `changes` 14 -> 20 in 50 s.)
* Contributing, not fixed here [U]: the slot is only as fresh as the game's map draws (7-9 per second) and `brief3d-eye-inside-volume` refuses frames when the
  head leans over the table (`refused=25322` in live-1508), so the compositor re-projects an old slot; rotation-only reprojection cannot correct head
  TRANSLATION, so a moving head sees swim at 0.75 m. No head-pose log exists: run B3-6 and compare.

**Fix.** `brief3d.h` `ContentLatch` / `latch_content_top`: growth is immediate (the 0.35 m cap holds), a lower reading must persist `kContentHoldMs` = 4 s
(a reading back above half the latch resets the hold) before the latch shrinks; `relax_content_scale`: the vertical scale tightens at once but relaxes at most
`kContentRelaxPerS` = 0.2 per second (SUPERSEDED 2026-11-01, headset test 8: now 0, the scale is monotone within a page; see brief3d-worldlock.md). Fit and placement survive a gap in map draws up to `kRelatchGapMs` = 30 s (a page flip); only a longer absence or a
LOCAL space change re-latches them (`screens` counts those). The content latch still resets after 2 s without map draws.

**Tests** (`brief3d_live_offline`): `test_table_stable_under_jitter` feeds 400 map frames with icons on every third one and asserts byte-identical eye
matrices, zero scale changes, one fit, one placement, and that a 3 s page flip keeps them; it drives the latch and the slew directly. `test_zoom_content_scale`
and `test_fit_and_screens` were updated to the held semantics (un-zoom after 4 s, re-latch after more than 30 s). Not built or run yet (awaiting "build go").

## 11. The table shakes with head motion (headset test 6, 2026-10-30d user-report item 3): the table is re-drawn every XR frame (2026-10-30)

**Evidence [E]** (`D:\acz-evidence-archive\headset-2026-10-30-session\live-2210-step143`, offline reading; the user was in the briefing at
t=191689.9..191701 s of the log clock; section 10's latch was in this build and did not cure it):
* `xr-stereo.log summary-brief3d-producer` 5 s deltas in the briefing: `frames` +300 / +221, `published` +300 / +242, `acquired` +298 / +242,
  `lag-frames=1;lag-max=2`, `age-ms=16;age-max-ms=32`, `refused` +0. So in step143 the game drew the map 60 times per second (not 7-9 as in
  step137, section 10) and the producer rendered one table image per game frame.
* `summary-brief3d-table` over the same windows: `blits` 2209 -> 2507 -> 2749 (+298, +242) but `layers` 3418 -> 3864 -> 4270 (+446, +406), and
  `summary` `frames=446..451` per 5 s: the XR loop runs at 90 Hz. A third of the XR frames re-submitted an image already shown, and every image
  was rendered with the eye pose the producer read at the game's map draw (`try_read_ex`, the latest located views), then displayed 1-2 XR frames
  later (`lag-frames` 1, `lag-max` 2, `age-max-ms` 32).
* `ee-camera.log summary-brief3d` in the same briefing: `refused=0;last-why=none`, `fits=1;placements=1;screens=0`, one `brief3d-content` change
  (`scale=0.5889`, the zoom): the fit, the placement and the content scale were stable. In earlier sessions `brief3d-eye-inside-volume` was the
  last refusal (`live-1144` 120 mentions, `live-1508` 322 with `refused=25322` (section 10), `live-1936-step137` 252).

**Root cause [I, strong].** The table's pixels were never rendered for the pose the headset displayed them at. Each slot carried the pose of an
older XR frame, with an age that changed frame by frame (60 game frames into 90 XR frames: 0, 1 or 2 frames old, plus the producer's own lag), and
the runtime's reprojection of a projection layer corrects ROTATION only: the head TRANSLATION during that age stays in the image. At 0.75 m a 1 cm
head move over 30 ms already shifts the table by ~0.8 degrees, alternating with the cadence: the user's "shakes like crazy when I move or turn my
head". At 7-9 map draws per second (step137) the error was 4-10x larger. The eye guard made it worse: whenever any table corner came within 5 cm
of an eye plane (leaning in, looking up or far sideways) the WHOLE frame was refused and the last image stayed on screen until the guard passed
again (a freeze, then a jump). The offline test reproduces the mechanism: a record rendered for pose A and shown at pose B (4 cm sideways, same
orientation) misplaces every vertex by the full 4 cm (`test_world_locked_across_head_motion`, last check).

**Fix (b3l:1, the default with brief3d:1): latch the game's map draws, re-draw them per eye every XR frame with the current pose.**
1. **Producer (game thread, `brief3d_live.h Producer::latch_draw`, glue `brief3d_replay_hooks.inl b3LatchGeom_locked`)**: at each accepted map
   draw nothing is recorded into the game's list any more. The draw is LATCHED: its t10 record and b10 constants (already read), the root
   parameters, references to the game's own PSO and root signature (the proxy's retained `shared_ptr`s: their deleters release), and CPU copies
   of exactly what the draw uses, read from the game's pinned, mapped UPLOAD buffers through the existing safe reader
   (`step3MappedSpan_locked` + `guardedCpuRead`, as t10 and the grid box): the draw's index range, the vertex range it references (slot 0,
   rebased: base vertex = -lowest index), and for the grid the t11 colour table (every entry the latched vertices index, section 12; was the first 4 KiB).
   Large buffers (the 520 KB grid VB, the 130 KB grid IB) go through a content cache (`latch::ReadCache`: size + 16 sampled 8-byte words; a
   hit reuses the blob, a miss re-reads at most once per 50 ms, every entry at least once per second; a copy whose samples changed during the
   read is refused as torn); small buffers (icons, t11) are read whole every time. The fit, the placement (per briefing and LOCAL generation),
   the content latch and the icon copies (air size, stalk dots) are the old rules, computed once per game frame. The frame's draws form the
   pending scene; the Present publishes it only when NO map draw of that frame was refused (else the previous scene stays: never a scene with
   missing icons). Replaced scenes and unlatched references are released after the producer lock (lock order with the hooked Release).
2. **Consumer (XR owner thread, `xr_brief3d_core.h LatchCore`, D3D12 `xr_brief3d_d3d12.cpp D3D12TableGpu::render`)**: in every XR frame in which
   the screen is planned and a scene newer than 500 ms of the current LOCAL generation exists, `latch::plan_frame` builds both eyes' records from
   THIS frame's located views (`FrameInfo::views`), `render` copies the scene's blobs into the allocator's upload span (only when the scene changed),
   the records and culled index lists after them, clears two private eye images (R16G16B16A16_FLOAT + D32_FLOAT_S8X24, the game PSO's formats),
   draws every latched draw per eye with the GAME's root signature and PSO (IA = our copies, t11 = our copy, b10 = the game's constants, t10 =
   the eye record per drawn copy), and blits the images into the eye swapchains (the old blit shader). The table layer is submitted with exactly
   those views. Ownership is TableCore's (both eye acquisitions settled in the call, waits/releases poison, an Execute without a Signal
   quarantines); each allocator keeps its scene (and so the game's PSO/root references) until its own fence value completed. The UI3D menu
   slots keep the old TableCore path (`Brief3dHook`: the latched table first; the slot path only when it did not draw).
3. **No eye-guard refusal**: `latch::plan_frame` culls per primitive per eye. Grid: the latched vertex box against the eye plane (fast path:
   all 8 corners at w >= 0.05 m -> the whole range); otherwise every segment with an end at w < 0.05 m is dropped from that eye's index list
   (a vertex behind the eye would be mirrored by DrawSelMap_v's own divide; 0 < w < 0.05 is depth-clipped exactly anyway). Icons: each copy's
   bounding sphere (matrix 3 translation, mesh radius x the matrix's longest row) must clear the plane. Leaning in, looking up or sideways
   only removes the primitives at or behind the eye, for that eye and that frame.
4. **Automatic fallback**: if the geometry is unreadable from the start (no scene ever published within 120 consecutive map frames that
   failed a read, e.g. the IB is not a mapped UPLOAD buffer in the live game [U]) the producer switches to the old replay path at a Present
   (`brief3d-latch;...;fallback=1;fallback-why=<reason>`), so the briefing never loses its table. `b3l:0` selects the old path explicitly.

Cost: no work in the game's lists (the replay's barriers/clears/draws are gone); per game frame the CPU reads ~130 KB of icon data plus 16
samples per large buffer (a cache hit) [I: WC read speed not measured]; per XR frame two eye passes of the grid (32,512 segments) and ~63 icons on
the XR binding queue plus the blit. Memory: + 2 x 16.6 MiB colour + 2 x 16.6 MiB depth + 2 x 4.7 MiB upload = ~76 MiB GPU, created at the first
render (the producer's slot images stay: UI3D uses them).

**Logs**: `ee-camera.log` every 5 s `brief3d-latch;scenes;incomplete;draws;refused;geom-refused;extra-vb;serial;geom-kb;records;reads;hits;read-kb;
rate;torn;faults;why;fallback;fallback-why;old;forced;miss=fp/age/new` (< 480 B; the last three added after headset test 7: copies served over
the read budget, forced re-reads, and why large gets missed - see `brief3d_latch.h` kLargeReadBudget / kStaleServeMs); `xr-stereo.log` every 5 s `summary-brief3d-latch;t-ms;renders;render-failed;layers;scenes;
no-scene;stale;gen-refused;no-views;no-chains;plan-failed;alloc-busy;lost;clipped;culled-copies;skipped;age-ms;age-max-ms;dead;poisoned;why;
plan-why` (< 480 B, offline-tested), `brief3d-latch-result` at retire. `summary-brief3d-table` stays (UI3D slots); in the briefing its
`blits` no longer grow.

**Tests** (`brief3d_latch_offline`, new): `test_world_locked_across_head_motion` - the head walks (+-0.25 m, +-0.1 m, 0.2 m) and turns (yaw
+-40 deg, pitch -10..-50 deg, roll) EVERY XR frame for 96 frames at 90 Hz while the game publishes a new map frame (its own camera orbiting) only
every 8th; every sampled grid vertex is decoded from the exact float32 record each eye's render used (`emulate_selmap`) and un-projected with an
independent OpenXR view model (`unproject_reverse`): all land on their latched LOCAL points (max error < 0.5 mm) in every frame and both eyes; one
render and one layer per XR frame; the layer carries exactly that frame's views; fit and placement unchanged by the game's orbit; plus the
stale-pose contrast above. `test_lean_in_no_refusal`: a head 10 cm above the table looking down (the old `open_eyes` returns
`brief3d-eye-inside-volume` for the same pose) is drawn, the culled list equals a brute-force list of the segments with both ends at w >= 0.05 m,
the kept vertices stay at their LOCAL points, and leaning back out restores the whole range. `test_incomplete_frames_and_icons` (a refused
draw keeps the previous scene; icon records = the game's with matrix 0 = the eye matrix and matrix 3 = the copy; a head at an icon culls that
copy only), `test_lifetime_stale_generation` (scene references held until the allocator's fence completed; stale > 500 ms and other-generation
scenes never drawn; re-placement after a recentre; retire), `test_signal_failure`, `test_fallback`, `test_read_cache` (hit, rate, refresh, torn,
small, strip-cut index range), line limits. `brief3d_offline` gains the `b3l` option checks. NOT built or run yet (awaiting "build go");
NOT tested: the D3D12 render (no fake device), the live buffers' heap types (IB, icon VB/IB, t11 [U]), the game PSO on our list.

**Live check (null driver first, then headset; cfg unchanged: `brief3d:1` already in `xr_terrain_stage`, b3l defaults to 1; `;b3l:0` = rollback):**
1. `xr-stereo;event=brief3d-option;...;b3l=1;prerequisites=ok`. Briefing: `brief3d-latch` with `scenes` growing ~ the map frame rate,
   `incomplete` small (the once-per-second refresh of the two large grid buffers can defer a frame: `why=b3l-read-rate`, the previous scene
   stays), `fallback=0`, `torn`/`faults` 0, `hits` >> `reads` after the first second. If `fallback=1`: record `fallback-why` (the heap
   type of the failing buffer) - the old path then runs.
2. `summary-brief3d-latch`: `renders` = `layers` ~ the XR frame count of the window (~450 per 5 s at 90 Hz), `render-failed=0`, `alloc-busy`
   small, `age-max-ms` ~ the game frame time, `plan-why=none`; `summary-brief3d-table` `blits` flat in the briefing.
3. Headset: walk, lean and turn the head - the table stays still like a real object (no swim, no judder); leaning into it removes only what is
   at/behind the eye, no freeze/jump; the zoom (cross) behaves as section 9.2. `png-screen.now` gives the eye0-table PNG of the latched render.
4. Cost: present-stats fps with/without (`b3l:0`), hooks ms in the briefing, commit gate for the +76 MiB.

## 12. "Briefing render doesnt fully render now at all" (headset test 7, 2026-10-31): the latched t11 colour map was cut at 256 entries

**Evidence [E]** (offline; `D:\acz-evidence-archive\step164\batch\runAGU1`, the only dump that caught the latched map: `upright\acz-vr-xr-screen-t66335-eye0-table.png`,
2400 x 1350, null runtime: both eyes at the LOCAL origin, identity, FOV +-45 deg on both axes (`first-layer-fov`), so 1200 px per tangent across, 675 down):
* The table is a 1 m x ~5.5 cm strip: rows 917..929, 963 px wide at the top, 1007 at the bottom, 65 vertical lines and 4 horizontal ones (raw dump rows 420..432).
* The fit and placement are exact: `brief3d-fit;box=-32,0,-32..32,0,32;up=1+;a1=0;a2=2+;m-per-unit=0.01562` (a flat grid, model z -32 = the far edge), table top
  0.45 m below the eye, 0.25..1.25 m ahead. The FAR edge projects to row 675 + 675 x 0.45/1.25 = 918, 1200 x 1/1.25 = 960 px wide: the strip's top edge.
  The strip's bottom (row 929) is z = 1.19 m: only the far 4 of 64 grid rows were drawn; a full table fills rows 918..1350 (its near edge is below the image).
* Not culling, not the scene: `summary-brief3d-latch` `clipped=0;culled-copies=0;skipped=0;plan-why=none`, `renders` = `layers`; `brief3d-latch`
  `geom-kb=639` = IB 127 KiB + VB 508 KiB + t11 **4 KiB**.
* DrawSelMap_v (dxc -dumpbin `384ade02`): colour = `ColorMap[fptosi(in.w)]` through the t11 ROOT SRV (no bounds check).

**Root cause [I, strong].** The latch (section 11, new in test 7) copied only the first 4 KiB of t11 (256 entries: the `fmcolor.gim` 256 x 1 guess) and the
grid's colour index runs far past 255, row-major from the far edge (4 rows x 64 = 256 entries = exactly the rows that showed). Every vertex past entry 255
read the zeroed upload memory after our copy: colour 0, alpha 0, invisible. The b3l:0 replay used the game's own t11, which is why the map rendered before
test 7. The live VB's w values were never dumped: the 64-wide row-major layout is the layout that reproduces the dump, not a measurement.

**Fix.** `brief3d_latch.h`: `colour_entries` (max `int(w)` + 1 over the latched vertices, fptosi truncation; negative / non-finite / >= 2^31 counted, not
covered), `t11_read_bytes` (every indexed entry, bounded by the resource), `ColourMemo` (no rescan of the same cached VB blob); the glue
(`b3LatchGeom_locked`) reads that many bytes (a t11 above 4 KiB goes through the content cache like the IB/VB); `layout()` refuses a SelMap draw whose
t11 copy does not cover its vertices (`b3l-t11-short`: the previous scene stays, and a persistent shortfall reaches the automatic b3l:0 fallback, which
draws with the game's own t11).

**Tests** (`brief3d_latch_offline`): `test_null_pose_table_extent` (passes before and after: from the null pose the far edge lands on dump row 918 and
960 px, the near edge at NDC -1.8: the projection, fit and placement are not the cause); `test_t11_covers_colour_indices` (reproduces the dump: with the old
4 KiB copy only 8 vertex rows have a colour, projecting to rows 918..929; FAILED on master `76f11374` at `!s || scene_colours_covered(*s)` because the
producer published that scene; now refused with `why=b3l-t11-short`, and with the full copy published, covered, no segment culled).

**Next null run (runBatchAG -BriefMap) should show:** the briefing eye0-table dump with a trapezoid whose top edge is at row ~918 (960 px wide) and which
fills the image down to the bottom edge (wider than the image there); `brief3d-latch` `geom-kb` above 639 by the t11 size (64 KiB for a 64 x 64 map:
~699), `why=none`, `fallback=0`, `incomplete` small. `why=b3l-t11-short` repeating = the game's t11 resource ends before the indexed entries (after 120
frames `fallback=1;fallback-why=b3l-t11-short`, the replay path then draws the full map). A strip again with `geom-kb=639` = this cause is wrong.