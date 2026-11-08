# BRIEFING-3D: the mission briefing's 3D map as true 3D in VR (roadmap item D-362 (b))

Offline research (2026-10-27 night). Nothing here ran in the game or a headset. Tags: **[E]** evidence (file, log line or
disassembly cited), **[I]** inference, **[U]** unknown (the live protocol in section 5 decides it).

## 0. Summary

* The briefing map is the mission **overview** screen: a dark terrain grid with yellow lines, 3D unit icons (aircraft wedges,
  flags, ground-unit blocks), red target rings, 2D text panels (MISSION / DATA NO. / ENEMIES / COORDINATES) and a 2D relief
  minimap; footer "Left stick: Change POV / Menu button: Proceed to mission prep." [E: `research/experiments/step50-ps2-probe-run1/briefing-roselein.png`].
* The game ships a dedicated shader pair for it: **`DrawSelMap_v` / `DrawSelMap_p`** (and `DrawModelIcon_v` for lit icon models).
  Disassembly (section 2) shows `DrawSelMap_v` is **GPU-transformed 3D**: the vertex buffer holds model-space `float4 (x, y, z,
  colour index)`, a 4x4 matrix from `t10` SrtData does the whole projection, then the PS2 screen mapping. Unlike the in-flight
  terrain/sky (`ps2_v`, pre-projected by the emulated VU), the briefing map can be **re-projected per eye** by substituting that
  matrix, exactly as the cockpit (`DrawCPT_v`) and world objects (`DrawACM_v`) already are.
* The current observers cannot see it: every step-5 observer looks only at lists with cockpit draws, and the XR session starts
  only when the mission starts (`docs/world-render/xr-start.md` section 1). A read-only draw histogram armed by a file was added
  (`briefing_observe.h`, section 4) so one producer-only live run can measure the briefing's draws.
* Recommended design: **replay the game's own SelMap/ModelIcon draws into a tabletop volume per eye** (section 6, option A),
  after the virtual-screen work makes the XR session run outside missions; fallback option B builds the table from our baked
  stage heightfield (`.zvrs`) plus the `.bfn` unit list.

## 1. How the screen is reached (from the save on this machine)

[E: D-89, D-90 and the L2 route notes (DECISIONS.md ~line 1537), `research/experiments/step83-verify-run1/route77.ps1`, D-354 run M]

Main menu -> CAMPAIGN -> Continue (the save resumes mission 02; no mission select exists in this save, D-354) -> the "document"
cinematic (Esc skips) -> the "map presentation" (Esc skips) -> the #02 ANNEX title card (auto-advances) -> **MISSION ROSELEIN
data / overview screen with the 3D map** (the camera moves by itself; Enter is ignored while it moves; Left stick changes the POV;
the tour lasts ~4.5 min, HANDOFF-2026-10-26) -> Esc -> the "Mission 02 / Annex" menu (START MISSION, HANGAR, **BRIEFING**, ...).
route77 skips all of this with four `back` taps at +40..+52 s.

[U] whether the "map presentation" before the title card is a pre-rendered movie (`movie/work/briefing/bNN.webm`, 17 files in
b.arc, drawn with `movie_yuv_v/p`) or the real-time map with narration; [U] whether the menu's BRIEFING item re-enters the
real-time map. The live run answers both (section 5, steps 4 and 8).

## 2. The game's data and shaders for the briefing map

### 2.1 Shaders (a.arc `shader/*.cso`; extracted to a scratch folder, disassembled with `dxc -dumpbin`; not committed)

| shader | inputs | resources | what it does [E: disassembly] |
| --- | --- | --- | --- |
| `DrawSelMap_v` (3068 B, shader hash `384ade02...`) | `ATTRIBUTE0 float4` | `t10 StructuredBuffer<SrtData>` (64 B = one float4x4), `t11 StructuredBuffer<ColorMap>` (float4 per entry), `cb10` (8 B = float2) | `p = x*row0 + y*row1 + z*row2 + row3` (row vectors), divide by `p.w`, `xy = (p.xy/p.w - 2048) * 2 / cb10.xy`, `z = p.z/p.w * 2^-24`, `w = 1`; colour = `ColorMap[int(in.w)]` scaled by 1/255 (rgb) and 1/128 (alpha). |
| `DrawSelMap_p` (1904 B) | `COLOR` | none | outputs the interpolated colour (PS hash `9ebdf113...`, identical DXIL to `DrawModelIcon_p` and logged with the same hash). |
| `DrawModelIcon_v` (4948 B, hash `d5a41c2f...`) | `ATTRIBUTE0 float3` position, `ATTRIBUTE1 float3` normal (integer x 1/4096) | `t10 SrtData` 272 B (four float4x4 + one float4), `cb10` 8 B | model matrix x view-projection product in the shader, same PS2 screen mapping, a lit colour from the normal. |

Both PSOs are created once at startup (test 19 `diagnostics.evidence.log` PSO records at ticks 42784 (VS 3068 B + PS 1904 B) and
44170 (VS 4948 B + PS 1884 B)); the byte sizes are unique among the 59 game shaders, which names them [E]. Other names by unique
size in the same log (for reading the histogram): `movie_yuv_v` 2384 B = `15c8c0bd`, `DrawACM_v` = `b569109e`, `DrawCPT_v` =
`f4cfd928`, `ps2_v`/`ps2AW_v` 3088 B share `534f29ca` (identical DXIL), `extra2_v` 1704 B = `a16573be`.

Consequence [E for the shader, I for the draws]: the briefing terrain grid and icons reach the GPU as 3D model-space vertices with
the complete camera in one replaceable matrix (SelMap) or a model matrix plus a camera (ModelIcon). The `ColorMap` table indexed by
`in.w` gives the height/class colouring [I].

### 2.2 Archive data (a.arc listing `D:\acz-extract\lists\a.txt`; copies in `D:\acz-extract\mis-src\data`)

| file | content |
| --- | --- |
| `data/selmap/brmodel.bin` (9,376 B) | a table of 8 offsets then 8 small `ACM` models (unit-size triangles, coordinates ~0.37) [E: hexdump]: the icon shapes [I]. |
| `data/selmap/fmap_b01..b18.gim` (66,608 B each) | GIM, 256x256 8-bit with a grey 256-entry CLUT [E]. Pixel values 0..44 on b02; correlation with the m02a heightfield (2x2 downsample) only 0.24 in the identity orientation, ~0 in the other 7 [E: scratch `fmapcmp.py`] -> most likely the 2D relief minimap of the overview (bottom right of the screenshot), not the grid's height source [I]. |
| `data/selmap/fmcolor.gim` (1,056 B) | GIM 256x1 RGBA ramp (dark olive -> yellow) [E]: the likely source of `t11 ColorMap` [I]. |
| `data/selmap/brfont.gim`, `brfont_11.bin` | the panel font. |
| `data/brdata/<stage>.bfn` (31 files) | `BFN` header, stage id (`m02a`), a block of floats (camera/route keyframes? incl. 0.785 = 45 deg), then named units each with a world position in **stage coordinates** + a heading/flags: `Cipher` (-35469, 4000, 144130), `pixy`, `F-15C` x3 (-27136, 3000, 120576)..., `TANK` (16400, 0, 81808), `APC`, `AA GUN` ... [E: hexdump]. Cipher's position matches the E6 start camera (-29899, 4003.6, 137560) to ~8 km, i.e. the same frame as our terrain (+-262144) [I]. |
| `data/brdata/BriefingID.bin` | 19 title-card records -> briefing ids 0..30 (stages-and-detection.md section 2). |
| `data/select/DetailMap.swa`, `data/dmap/dmap_*.gim` | probably the in-flight big map (already working in VR, D-362) [I]. Not the briefing. |

[U] where the grid's heights come from (the loaded stage heightfield, a coarser runtime mesh, or a file not yet identified). The
live VB capture (phase 2 in section 5) decides; option A does not need to know.

## 3. Why the existing capture does not see it

* `step5Draw`/`step5HudDraw` return unless the producer state exists (`g_step5`, created by `step5Arm` when the cockpit set is
  chosen, i.e. in the mission) and the cockpit members are known [E: `gpu_copy_step5_hooks.inl` step5Draw/step5Arm]. The phase,
  world and HUD observers are scoped to lists that contain DrawCPT draws [E: `phase-observe-definitions` text].
* The OpenXR session starts lazily when the producer publishes its first StereoFrame, which in direct launches happens only at mission
  start [E: `xr-start.md` section 1]. So nothing is shown in the headset on the briefing screen today.
* The record of test 16 (world1-impl.md section 4) counted 109,737 DrawACM_v early refusals before the mission: menus/hangar/briefing
  ACM draws exist pre-mission, but they were never classified.

## 4. Logging added (this branch): `briefing-observe` (read-only, no config key)

* Code: `src/acz-vr/briefing_observe.h` (header-only), two call sites: `d3d12_hook.cpp observeDraw` (one relaxed atomic load per
  draw when off; when armed the PSO's creation hashes, topology type, indexed/GS flags and count/instances go into a 32-key window
  histogram under its own mutex, after the hook-state lock is released) and `ee_camera_probe_win.cpp pacing_tick` (the existing 5 s
  present-stats window: polls the arm file, counts presents, emits the lines). `PipelineRecord` now always keeps `brVs8/brPs8`
  (8 bytes per PSO; `hudVs/hudPs` behaviour unchanged).
* Arm: create `%LOCALAPPDATA%\acz-vr\briefing-observe.arm` (any content). Disarm: delete it. A window is logged only if armed over its
  whole span (so the first window after arming is silent).
* Lines in `ee-camera.log` (and the diagnostics log), every 5 s while armed:
  `briefing-observe;event=window;ms=;presents=;draws=;draws-per-present=;keys=;overflow=;no-pso=;briefing=0|1;selmap=N(units=U);modelicon=N(units=U);acm=;cpt=`
  and `briefing-observe;event=top;...;top=vs8/ps8/t<topology><i|n><g|->/draws/units/max-units[/name],...` (8 keys by draws).
  `briefing=1` means DrawSelMap_v draws occurred in the window.
* Test: `tests/acz-vr/briefing_observe_offline_tests.cpp` (ctest `briefing_observe_offline`): keying, overflow, names, both lines
  (bounded, truncation-safe), arm/disarm by roll, 4 threads x 5000 draws + presents. [E] Private build (D:\b-brobs-a95a, -j2):
  `briefing_observe_offline: 56 checks passed`; the edited `d3d12_hook.cpp` and `ee_camera_probe_win.cpp` objects compile. The DLL was
  not linked and the full ctest was not run (the executor's post-merge gate).
* Cost when armed (estimate): one extra `pipelines.find` per draw under the existing lock + one short mutex; meant for menu screens,
  not for flight performance runs.

## 5. LIVE PROTOCOL for the lead's driver (producer-only, no SteamVR needed)

Preconditions: a DLL set built from master with this branch (`.claude/build43.bat` pattern, bump the step); memory gate per
HANDOFF-2026-10-27 (`tools/mem-guard.ps1 -Report`: start < 85 %, stop rule 95 % x2 / 97 %); no compilers running; D-279 launch
rules (acz.exe direct, launcher Windowed 2560x1440 + Player 1 Keyboard, never `-applaunch`); the day install restored at the end.

1. Install the DLL set with the day config (`tools/live/vr-test-start.ps1` as in step83 notes; SteamVR NOT started: producer-only).
   Create `%LOCALAPPDATA%\acz-vr\briefing-observe.arm` (empty) BEFORE launch. Optional: `%LOCALAPPDATA%\acz-vr\dump-shaders.txt`
   containing `9ebdf113` (dumps the live SelMap/ModelIcon/ModelGsh VS+PS bytes once, to confirm they equal the archive .cso).
2. Launch, skip-intro reaches the main menu (~9 s). Screenshot. Wait 12 s (two windows: main-menu baseline).
3. `accelerate` (Campaign), `accelerate` (Continue): screenshot each.
4. Document cinematic: screenshot, wait 12 s, `back`. "Map presentation": screenshot, wait 12 s (record: is `movie_yuv` 15c8c0bd in
   `top`? `briefing=`?), `back`.
5. Title card / overview: when the screenshot shows "Left stick: Change POV", note the time; stay 20 s idle (4 windows, the camera
   moves by itself), screenshots at +0, +10, +20 s.
6. POV test (find the keyboard binding of the left stick): hold each of `left`, `right`, `up`, `down` for 2 s (acz-window-driver
   `-Action hold`, `captureDuringHold`), 4 s apart, screenshot during each hold; then hold `look-left`/`look-right` 2 s each in case
   the POV is on the look keys. Note which key moves the map camera.
7. Stay 10 s more (window after the POV change). Then `back` -> the Mission 02 menu: screenshot, 12 s (menu baseline).
8. Move to BRIEFING (screenshot after every `down`), `confirm`: screenshot; if the 3D map returns, 12 s; `back` to the menu.
9. START MISSION + aircraft/weapon/wingman/deploy confirms + takeoff skip + `V` (route77 from +56 s); 15 s in the cockpit
   (flight baseline: `selmap=0` expected, `cpt>0`). Close with CloseMainWindow.
10. Delete `briefing-observe.arm` (and `dump-shaders.txt`), re-run the day install, copy the logs + shots to
    `D:\acz-evidence-archive\briefing-run1` and record notes (`research/experiments/briefing-run1/notes.md`).

Report per screen (from `grep briefing-observe ee-camera.log`): `briefing`, `selmap` draws and units per window, topology of the SelMap
key(s) (2 = line list -> the yellow grid is lines; 3 = filled triangles), `modelicon` count, `draws-per-present`, the top keys, and
whether SelMap units change between POVs (constant units = the game draws the whole grid, no CPU culling -> any viewing angle works
in VR; changing units = culling, option A needs care). Fixture/null success proves nothing about the headset image.

Phase 2 (needs new code after run 1, not built here): a one-shot armed snapshot of one SelMap draw (VB bytes via the step-2
read-only GPU copy, `t10` 64 B, `cb10` 8 B, `t11` table) and one ModelIcon draw (`t10` 272 B) at two POVs: gives the model-space
extent of the grid, the colour table, and which ModelIcon matrix is the camera (the one that changes with the POV).

## 6. Design: rendering the map in 3D in VR

Common to both: the map becomes a **tabletop volume** in the play space (default 1.0 m across at 0.85 m height in front of the
user, room-scale option 3-4 m on the floor), the user walks/leans around it with the head pose; the 2D panels and footer stay on the
virtual screen (roadmap R1) behind the table; the game's own POV control stays the game's (it only changes the virtual-screen view).

### Option A: replay the game's own briefing draws into the table (recommended; chosen after run R4, implemented as BRIEF3D: `brief3d.md`)

Per eye, re-record the frame's DrawSelMap_v and DrawModelIcon_v draws into the eye targets (private FP16 colour + depth, as the
world-object replay does) with substituted SrtData:
* SelMap: `M_eye = T_model->table * View_eye * Proj_eye * N2PS2`, where `N2PS2` maps NDC to the shader's PS2 screen space
  (`x_ps2 = 2048 + ndc.x * cb10.x / 2`, same for y; `z` scaled so `z * 2^-24` is the eye depth in [0,1]). `T_model->table` is fixed
  from the grid's model-space bounding box (phase-2 capture) so the stage fits the table; vertical exaggeration is the game's own.
* ModelIcon: keep the model matrix (unit placement/animation), replace the camera matrices the same way.
* Depth test against a cleared eye depth; composite as its own projection layer content (no cockpit/world in this mode).
* Gains: the game's exact look (grid, colours, animated unit movements, target rings if they are icon draws), always in sync with the
  narration; no per-mission data work. Reuses: the DrawACM_v world-object SrtData substitution path, private targets, XR submit.
* Costs/risks: needs the XR session outside missions (R1); a new member class for two shader layouts (64 B and 272 B SrtData); the
  red rings/flags may be `ps2_v` 2D draws (pre-projected: they would stay on the virtual screen) [U]; CPU culling by the game's camera
  [U, run 1 step 6]; the game's view matrix may also drive LOD [U]. Effort: ~2 agent-days after R1 + run 1 + phase 2; risk medium.

### Option B: build the table from our stage data (fallback)

Render the stage's baked heightfield (`.zvrs`, 513x513, already baked for all 39 stages) as a tabletop mesh with a hologram shader
(height-ramp colour like `fmcolor`, yellow grid lines every 8 cells, fresnel rim, transparent dark base) and markers from the `.bfn`
unit list (positions in stage coordinates; icon shapes from `brmodel.bin` ACMs via the existing ACM decoder; colour by class:
player/allies green, ground targets red rings).
* Gains: fully offline-testable (WARP images), no dependency on capturing the game's draws, works for every mission whose stage we
  detect, can be shown in the hangar/menus too.
* Costs/risks: static (no animated tour, no live state), not pixel-identical to the game, needs the mission id at the briefing (stage
  auto-detection runs at XR start; the `.bfn` choice needs the mission: from the resident stage fingerprint [U at briefing time]);
  `.bfn` layout only partly decoded. Effort: ~2-3 agent-days; risk low-medium. Still needs R1 to show anything outside missions.

### Comparison

| | A replay | B rebuild |
| --- | --- | --- |
| fidelity | the game's own map and animation | close, static |
| live dependency | run 1 + phase-2 capture | stage/mission id only |
| offline testability | WARP with captured bytes | full |
| new code | member class + SrtData maths for 2 layouts | mesh + shader + .bfn reader |
| shared prerequisite | R1 (XR session + virtual screen outside missions) | R1 |

Decision rule after run 1: choose A if SelMap draws are 3D triangles/lines with constant units across POVs and the icons are
ModelIcon draws; otherwise B (and keep A's matrix work for later).

## 7. Open questions [U]

1. Map presentation: movie or real-time? 2. Grid topology (lines vs triangles) and vertex count. 3. Culling across POV changes.
4. Which overlay elements (rings, flags, route lines) are 3D (SelMap/ModelIcon/ACM) vs 2D `ps2_v`. 5. The height source of the grid.
6. Whether the menu's BRIEFING item re-enters the map. 7. The keyboard binding of "Left stick: Change POV". 8. The `.bfn` float block
(camera keyframes for the narrated tour?).
