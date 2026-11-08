# Eye resolution (headset test 8: "the SteamVR cinema screen is waaay better resolution")

Status (2026-11-01): the pixel path was traced from the source code and the test-8 logs. Option `eyeres:<pct>` is implemented and covered by
offline tests. It is OFF by default and has **not** been run live, in the null driver or in the headset. Nothing here shows the image in the
headset is sharper. Only the check list in section 6 can show that.

Step 2 (same day, branch `worktree-agent-a52b11741c2503ffb`): with `eyeres` > 0 the game world window is now sampled with a footprint filter (section 3b, loss 2).
The per-eye producer targets at the eye size (loss 3) were judged too risky for one step without a live run; they are staged precisely in
section 5.1. Not run live, in the null driver or in the headset.

Live evidence for the base option (step180 null run, `eyeres:100`): `eye-res-config` ok, the eye swapchains at the runtime's recommended size, the
eye dump correct (`D:\acz-evidence-archive\step180\eyeres`).

Step 3 (branch `worktree-agent-a9a40891a084a732f`): section 5.1 (a)-(g) is implemented as section 3c: the per-eye producer targets follow the
eye swapchain size, with the sub-option `eyeobj:0` as the fallback. Offline tests only. **Not run live, in the null driver or in the headset.**

Evidence: `D:\acz-evidence-archive\headset-test8-20261101\` (`xr-stereo.log`, `step5-producer.log`), the user's `runtime.cfg` (read only), and
`docs/world-render/gpu-cost.md`.
Tags: [E] measured or logged, [R] read from the code, [I] inference.

## 1. The pixel path of the world in each eye (flight, test-8 config)

The game draws its frame at **2560x1440** (`source=2560x1440`). The eye image in the headset is put together from five sources. All of them are
sampled into the eye swapchain in one composite pass (`xr_stereo_d3d12.cpp`, `ps_main` mode 2-4, one pass per eye).

| # | source | how it reaches the eye | resolution at the source | notes |
|---|---|---|---|---|
| A | **Native world:** terrain ray march, sky, clouds, water, light, props/trees raster | drawn directly at eye-swapchain resolution (props: own RGBA8 + RGBA32F targets at the swapchain size, `fr.w = map.dst.w`) | = the eye swapchain [R] | most of the visible world with `vrn:1` |
| B | **Model cockpit** (`xr_cockpit_model=auto`) | drawn directly into the eye swapchain (depth sized to the larger eye) | = the eye swapchain [R] | |
| C | **Replayed per-eye producer images:** world objects, replayed cockpit parts, smoke (t13), sprites (t14), effects | the game's draws are **replayed per eye** (not a copy of the backbuffer), with the eye matrices substituted and the HMD FOV (`eyes=pose-full-substitution-hmd-fov`), into producer targets of the **game's target size** 2560x1440 R16G16B16A16F (`gpu-copy-step1;event=ring;per-slot=2x(RT ... 2560x1440 ...)`) | 2560x1440 over the HMD FOV: **1099 x 626 px/tan** [E, calculated from the logged FOV] | sampled full-rect (bilinear; depth nearest) |
| D | **Game world window:** the game's own mono scene image, copied once per frame (`world:1`, 88 MB) | a plane 500 m ahead covering the game lens FOV 63x38 deg (`lens-tan=0.61517,0.34604`), sampled per eye pixel | **2081 px/tan** both axes [E] | one mip level, single bilinear tap |
| E | HUD / markers / menus | HUD and marker **quad layers** (own 2400x1350 swapchains, 10 m, scale 0.6), vscreen screen on the HUD swapchain, UI3D/briefing tables (1440x1440 producer eye images, `kEyeW/kEyeH`) | HUD quad ~3250 px/tan; screen ~2240 px/tan; UI3D 1440 over the eye FOV | quads are resampled by SteamVR, the same way as the cinema screen |

Then the **eye swapchain** goes to SteamVR's compositor, which resamples it to the panel with the lens distortion.

### The eye swapchain (the bottleneck)

- Policy `source-aspect-width-min(src,2400,max)` (`swapchain_extent`): the source aspect, with the width capped at 2400. That gives **2400x1350
  (16:9)** for both eyes [E].
- SteamVR recommends **2688x2688** per eye (`recommended=2688x2688-unused`) [E]. This is SteamVR's own render size for the Beyond at the
  user's resolution setting, the size the cinema screen is rendered at.
- The projection layer is submitted with the headset's own, nearly square FOV (`cockpit-fov=hmd`, eye0 -54.95/+42.12/+43.88/-53.21 deg = tan
  span 2.330 x 2.299). Mapping is `full-rect`, so 1350 rows cover the whole vertical FOV.
- Logged density (`event=layer-fov`): **eye0-px-per-tan=1030.1x587.3**, anisotropy 1.754 [E]. Recommended: 2688/2.330 = **1154 px/tan**
  horizontally and 1169 vertically [E, calculated].
- At the centre that is 18.0 x 10.2 px/deg in our eye, against about 20 px/deg recommended.

## 2. Resolution losses, largest first

1. **Eye swapchain vertical density: 587 vs 1169 px/tan (x0.50).** The horizontal is 1030 vs 1154 (x0.89). That is 3.24 Mpx against 7.23 Mpx
   per eye (45 %). Sources A and B (the terrain, the sky, the trees and the model cockpit) can never be sharper than this. C and D are minified
   through it as well. This is the main reason the cinema screen looks "waaay better": SteamVR renders that screen at the full recommended
   size from a 2560x1440 image spread over about 55 deg, about 2x the vertical detail per degree of our world.
2. **Game world window D is minified without mips:** 2081 px/tan down to 587 vertically (3.5x) and 1030 horizontally (2.0x), with one
   bilinear tap. The result is detail thrown away plus shimmer and aliasing on edges and text in the window [I, by the sampling theorem; not
   measured]. With `eyeres:100` this becomes 1.8x on both axes.
3. **Producer targets C at the game's 2560x1440 over the HMD FOV:** 1099 x 626 px/tan, 0.54x of the recommended vertical density. This
   affects world objects (aircraft and targets), smoke, sprites and any replayed cockpit band. It does not change with the swapchain. It needs
   the producer redesign in section 5.
4. **UI3D / briefing tables:** 1440x1440 producer images over the eye FOV (about 620 px/tan). These are upscaled now (Catmull-Rom +
   sharpen, `ui3d_resample.h`). A square eye removes the 1.67x/0.94x anisotropic resample.
5. **Terrain texture LOD** is distance-only (`kTLodScale = 1/16000`, not tied to the pixel footprint) [R]. A bigger eye sharpens edges,
   geometry, trees and the cockpit. Terrain texel detail at range does not change until the LOD is biased (section 5, step 3).

Not a loss: the HUD quad (about 3250 px/tan) and the menu screen (about 2240 px/tan) are above the panel's density, like the cinema screen.

## 3. Implemented now: `eyeres:<pct>` (inside `xr_terrain_stage`)

The user's cfg already has 32 keys (`max_config_keys=32`), so this is a sub-option of the existing key, like `pov:`/`vsearly:`. The stage
value becomes about 170 of its 1023 characters.

- `eyeres:0`, or no token (the default): the old policy. Every swapchain and every line stay byte-identical (tested).
- `eyeres:100`: both **eye** swapchains use the runtime's recommended size, 2688x2688 on the Beyond.
- `eyeres:N` (50..200): the recommended size times N/100 per axis. The result is clamped to the runtime max (aspect kept) and to 4096x4096 =
  16.8 Mpx per eye (`eyeres::kPixelBudget`, aspect kept). Use 100 first. SteamVR's own resolution slider also moves the recommended size.
- Quads (world quad, HUD, markers, the vscreen) keep the source-aspect policy.
- **Prerequisite:** `gpu_copy_step5_hmd_fov=true` (the user has it). Without it the layer FOV is the 16:9 game lens, a square eye would waste
  rows, and the proxy logs `prerequisites=needs-gpu_copy_step5_hmd_fov;effective-pct=0`.

Why this is safe:
- Every consumer of the eye extent already takes it as a parameter: the composite viewport, the props targets, the cockpit depth, the
  brief3d/ui3d/pause blits (`eye_ext`) and the PNG dumps (side up to 8192).
- No shader constant assumes 2400x1350. `GetDimensions` is used where a size is needed.
- Full-rect mapping with the same FOV is geometrically exact at any aspect: the layer describes the FOV and the image fills it.
- A refused size (zero, out of range) falls back to the old extent per eye (`clamp=refused(old-policy)`).

Logs:
- `xr-stereo;event=eye-res-config;ok;eyeres=N;prerequisites;effective-pct` (proxy, once).
- In the swapchains line, `policy=recommended-x100%(eyes),source-aspect(quads);recommended=2688x2688-used`.
- New `xr-stereo;event=eye-res;pct;used;eye0;eye1;clamp;old-policy;mpix-per-eye;budget-mpix;est-mib=...`.
- The existing `layer-fov` line then reports the new px/tan.

Files:
- `src/acz-vr/eye_res.h` (new, header-only)
- `xr_stereo_output.{h,cpp}` (`Settings::eye_res_pct`, `Output::setup`)
- `xr_stereo_proxy.cpp` (token extraction)
- `tests/acz-vr/eye_res_offline_tests.cpp` (ctest `eye_res_offline`, 50 checks)
- `xr_stereo_output_offline_tests --eye-res` (ctest `xr_stereo_eye_res_offline`, 121 checks)

Checks run:
- The private build in `D:\b-res`: both test targets, plus the `xr_stereo_proxy.cpp` object under MSVC with windows.h. The full DLL was not
  built.
- Both new tests pass, every existing `xr_stereo_output_offline_tests` mode passes, and the full run passes (281,336 checks).

## 3b. Implemented: world-window footprint filter (loss 2; only with `eyeres` > 0)

What changed:
- With `eyeres` > 0 the composite eye targets get `mode_bits` bit 21 (`xr_stereo::kModeBitWorldFilter`; free on the eye targets: 19 sprite,
  20 blink, 24..31 vignette; never set on the HUD target, where 19..23 are the radar background). `eyeres:0` / no token: the bit is never set
  and the shader takes exactly the old single `SampleLevel` (the first tap is `wuv + 0`).
- Shader `world_rgb()` (`xr_stereo_blit.hlsl`) for the window texel of `world_layer_window`: the default window path and the
  `xr_terrain_blend` path. The `world_layer_ext` / `world_layer_cache` window rays keep one tap (see the WARP note).
- Per eye pixel: two extra ray/plane hits, one eye pixel right and one down (`px` = `ddx/ddy` of the eye uv, taken once at the top of
  `ps_main_body`, uniform control flow, passed down as a parameter) give the footprint Jacobian (ju, jv) in world uv. If either axis is
  longer than 1.25 world texels: 4 bilinear taps at `wuv +- (ju + jv)/4` and `wuv +- (ju - jv)/4`, equal weights (a 2x2 box over the
  footprint). Otherwise the old single tap. CPU mirror: `acz::eyeres::filter_select / filter_offset` (`eye_res.h`).
- Test-8 geometry at the centre: eyeres:100 -> 1.80 x 1.78 texels per eye pixel -> 4 taps. eyeres:200 on a 4096 eye -> 1.18 x 1.17 -> the
  old single tap (no blur once the eye out-resolves the window).
- Logs: the `event=eye-res` line ends with `;world-filter=footprint-grid(max-2x2,bit21)`; new `event=summary-world-filter;t-ms;frames;
  total-frames;bit=21;grid=...;eye0=WxH;no-headset-claim` once per summary period while eyeres is on (zero frames = no world composite).

Why a 4-tap filter and not a mip chain:
- A mip chain needs a mipped copy of the producer-owned world image (+1/3 memory, about +29 MiB per slot at 2560x1440 FP16), a pass per
  level (11 levels) every frame, and a new state/lifetime contract on that image. The 4-tap filter needs no resource, no pass and no state
  change: **0 MiB**.
- GPU cost [I, not measured]: the window covers about 16 % of an eye (lens tan 1.230 x 0.692 against the eye's 2.330 x 2.299). At eyeres:100
  that is about 2.3 Mpx for both eyes, each with 3 extra bilinear taps from a 7 MiB texture that stays in L2 plus about 30 ALU ops: an
  estimated **< 0.1 ms** on the 4090, and zero where the window is not drawn. A mip chain would cost about the same per frame.
- 2x2 is enough at eyeres:100: in a 1-D shimmer model (white-noise texture, eye pixel moved in 1/16 px steps) r = 1.8 goes from 0.132 (1 tap)
  to 0.079 (2 taps per axis); at r = 3.54 (old eye size, vertical) from 0.174 to 0.100 (3 taps would reach 0.069; not built, see below).

**WARP / fxc note (important for any later shader change):** a per-axis 1..4 grid passed fxc but broke unrelated modes in the WARP
harness: with `[loop]` loops the mode-0 cockpit alpha became the raw depth (127 failures); unrolled 4x4 with `[branch]` broke the sprite and
debug-view cases (35 failures, then the device was removed); filtering the extend/cache window rays as well (more inlined copies) broke
mode 0 again. The master shader passes, and so does this straight-line 2x2 version in `world_layer_window` only (full harness: 11512
checks, 0 failed). It was not found whether fxc or WARP is at fault; the shader is about 17.6k instruction slots and every inlined copy of
new code costs. Run the full `xr_composite_warp` harness after every change to `xr_stereo_blit.hlsl`, not only the new case.

**Scope, honestly:** with the user's installed cfg the terrain (`vrn:1`) is drawn in flight and `xr_terrain_blend` is absent, so the game's
world window is **not composited in flight** where the stage terrain is drawn (effects-outside-window.md section 15). There the filter does
nothing. It acts where `world_layer_window` shows the window: stages or frames without our terrain (unknown stage, terrain refused) and
`xr_terrain_blend:1`. The effects image (missiles, tracers) is still sampled in the window rays with one tap; filtering it the same way
(`effects_over` takes the same `wuv`) is the obvious next step, but thin tracers would get dimmer and the WARP note applies.

Tests:
- `eye_res_offline` (64 checks): threshold, footprint length, selection by the larger axis, NaN, the 2x2 offsets equal the shader's o1/o2,
  a 1x1 grid is the old tap, the test-8 ratios (old, eyeres:100, eyeres:200) and the shimmer model above.
- `xr_stereo_eye_res_offline` (`--eye-res`, 321 checks): the request is on every world-composite recording with eyeres and on none without;
  the summary line; bit 21 overlaps no other eye bit; the exact eye-res line. Full `xr_stereo_output_offline_tests`: 281336 checks pass.
- `xr_composite_warp` (real backend + shader on WARP), new case `[w]`: a 1-texel checkerboard world equals the CPU 2x2 mirror (max error
  0.003 over 23944 window pixels) and the aliasing (mean |pixel - 0.5|) drops from 0.125 to 0.003; the gradient world equals the mirror
  (0.004); a magnified eye (4x) is byte-identical with the bit on and off; bit off equals the old CPU mirror. Whole harness: 0 failed.
- fxc compile of ps_main / ps_cache_update / ps_cache_fill / vs_main with /WX passes. The full DLL was not built.

## 3c. Implemented: per-eye producer targets at the eye size (loss 3; `eyeres` > 0 with `eyeobj` on)

Option: `eyeobj:<0|1>` inside `xr_terrain_stage`, next to `eyeres` (the cfg is at the 32-key limit). Default 1 while `eyeres` > 0. Without
`eyeres` it does nothing (the config line then says `eyeobj=0(inactive:eyeres-off)` or nothing). **Fallback:** `eyeres:100;eyeobj:0` keeps the
eye swapchains at 2688x2688 and the producer at the game size (exactly the step-2 behaviour). `eyeres:0` (or no token) leaves everything as it was
before eyeres. The user's value plus `;eyeres:100;eyeobj:0` is about 170 characters (tested < 255).

What changed (`src/acz-vr/eye_ring.h`, new, header-only; `gpu_copy_step5_hooks.inl`, `gpu_copy_step5_fxsprite.inl`, `xr_stereo_output.cpp`,
`xr_stereo_proxy.cpp`, `stereo_frame_source.h`, `fx_smoke_gpu.h`, `fx_sprite_gpu.h`, `gpu_copy_step3_hooks.inl`):
- **Channel.** At cfg time the proxy publishes the request (eyeres on and the eyeobj flag). After the `eye-res` line, `Output::setup` publishes the
  eye extent (the larger of the two eyes on each axis). The producer reads both **once**, when it creates its ring. With `vsearly:1` the XR
  swapchains exist in the menus, so the extent is known before the first flight frame. If the ring is created first, the ring stays at the game size
  for the whole process, and the log says `why=no-eye-extent`.
- **Decision** (`eyering::choose`): `off` (nothing asked, no line), `eyeobj-off`, `no-eye-extent`, `eye-size-refused` (> 4096),
  `eye-over-image-cap` (> 128 MiB per image), `eye-equals-game` (nothing to rewrite), or `eye`.
- **Sizes in the ring.** `ringW/ringH` are now the per-eye target size. The new `gameW/gameH` are the game's viewport extents. Every check that
  meant the GAME size now compares with `gameW/gameH`: `step5StateWhy` (`viewport-size-changed`), the re-selection check, the smoke/sprite gates
  (the game viewport must cover the game image) and `StereoFrame::width/height`. The per-eye images get the new
  `StereoFrame::eye_width/eye_height` (0 without the eye ring). The world copy, HUD, markers and the mono effects image stay at the game size (they
  are game-screen images).
- **Targets at the eye size:** the object ring (RT + DS, 2 eyes x 3 slots), and the per-eye smoke (t13, fxmem:4) and sprite (t14, fxmem:6) images
  (they are depth-tested against the ring DSV). The smoke/sprite budgets take eye caps on the eye ring: 128 MiB per image, 800 MiB per kind. The
  game-size caps (32 MiB per image, 200 MiB per kind) would refuse a 2688x2688 FP16 image (55 MiB).
- **Viewport/scissor.** For every replayed member (cockpit members and world objects, `step5RecordMember`) and every per-eye smoke/sprite replay
  (`step5RecordSmoke`, `step5RecordSprite`), the game's viewport is scaled per axis by eye/game. The scissor is scaled the same way, rounded outward
  and clamped. Both are set right before the two eye draws, and the game's own viewport and scissor are set back right after them, before the
  game's own draw (the native vtable calls 21/22, as the fxatlas capture already does). This needs exactly one viewport and one scissor, tracked
  (`eyering::rs_restorable`). Otherwise the draw is refused alone (`eye-ring-viewport-count` / `eye-ring-scissor-count` / `eye-ring-rs-unknown`;
  a smoke/sprite draw then takes the window effects replay). Each member adds 4 commands, and the command cap is raised by 4 x the member cap, so
  the member capacity is unchanged.
- **Constants:** none rewritten, by design. The replayed shaders go GS units -> NDC through the record's own +400 constants (512, -448, 2048,
  2048; `docs/interception/acm-shader-dataflow.md` section 1). The eye matrices carry the HMD FOV, and the PS reads no `SV_Position`
  (dataflow section 2). The target size therefore enters only through the viewport. The one producer term in eye pixels, the tracer's minimum width
  (`fx_sprite_replay.h` Widen, 512 / ringW), already follows the eye image. Tested: the same GS point lands on the same relative eye position at any
  target size.
- **Consumer:** `job.eyes/depth/smoke/sprite` use the eye size. `job.world/hud/markers/effects`, the mapping, the source-aspect quad policy, the
  radar aspect and the tactical stamps keep the game frame size. The shader samples every per-eye image by uv and reads depth with
  `GetDimensions` (no shader change). The backend's `check()` compares each resource with its own descriptor size.
- **png_once (producer FP16 dump):** refused on the eye ring (`eye-ring-sizes-differ(use-eyeobj:0)`: one footprint size). The consumer
  `xr_png_once` / `png-eye.now` eye dumps are not affected.

Memory at the user's 2688x2688 (FP16 RT 8 B/px + D32_S8X24 DS 8 B/px; `eye_res_offline` checks every number):

| item | game size 2560x1440 | eye ring 2688x2688 | extra |
|---|---|---|---|
| object ring, 2 eyes x 3 slots x (RT + DS) | 337 MiB | 661 MiB | +324 MiB |
| per-eye smoke images (fxmem:4), 6 | 168 MiB | 330 MiB | +162 MiB |
| per-eye sprite images (fxmem:6), 6 | 168 MiB | 330 MiB | +162 MiB |
| **total producer** | | | **+648 MiB** (DEFAULT heap) |

Together with the eyeres:100 swapchain/props/depth growth (section 4, about +260 MiB), that is about +0.9 GB of GPU memory against eyeres:0. The
effects/HUD/marker caps (`effects_budget`, `hud_marker_total_cap` 128 MiB) are unchanged: those images stay at the game size. Smoke/sprite charged
at 2688: 348.0 MB / 351.1 MB (inside the 800 MiB eye cap; 4096x4096 also fits).

GPU time [I, not measured]: the replayed objects, smoke and sprites are a small part of the frame. Their pixel cost grows with the pixel count:
x1.96 over 2560x1440 (7.23 against 3.69 Mpx per eye). The extra RS calls cost no GPU time worth noting.

Tests (offline only; fixture success says nothing about the headset picture):
- `eye_res_offline` (4875 checks): eyeobj parsing (default, 0/1, bad values, the stage id, prefix keys, the 255 budget), the trailing token, the
  channel and every `choose` outcome, the viewport math (the user's case, identity, a sub-rect viewport with NDC landing on the same relative eye
  position, odd sizes), the scissor (outward rounding on a grid of 1,000+ rects, under one pixel, clamps, negative origin, empty), `rs_restorable`,
  no constant term, memory and the smoke/sprite caps.
- `gpu_copy_step1_offline_tests` (real hook wrappers, fake D3D12 with a recording of every native RS call): `step5-eye-ring` (eye RT/DS 320x320 for
  a 256x144 game, per member eye vp+sc right before the two eye draws, the game's set back right after them, the refused member records nothing,
  the list ends in the game's state, commands 4 + 12 per member, the published frame says game 256x144 / eye 320x320, the eye-ring and
  summary lines); `step5-eye-ring-eyeobj-off` and `step5-eye-ring-no-extent` (game-size ring, no producer RS call, eye_width 0, the old command
  count, the named reason); `step5-eye-ring-fx` (smoke/sprite created at 320x320 under the eye caps, the effects/HUD/world images at the game
  size, the smoke and sprite recorders bracket the eye draws and restore the game's state; rs off: no RS call); `step5-continuous` now also checks
  that eyeres off adds no RS call, no state and no line. Every existing scenario passes unchanged.
- `xr_stereo_eye_res_offline` (486 checks): Output publishes the extent only when eyeres is used (not when off or refused); frames with eye_width
  give eyes/depth the eye size and world/HUD the game size; one `eye-ring-consumer` line; without eye_width nothing changes.
- The full `xr_stereo_output_offline` run, `xr_composite_warp` (no shader change; run anyway: passed), the vscreen/tactical/brief3d targets that
  include the consumer (built), and the DLL objects `d3d12_hook.cpp`, `xr_stereo_output.cpp` and `xr_stereo_proxy.cpp` compiled under MSVC in
  `D:\b-res3`. The full DLL was not built.

Not proven offline: the fake rasteriser ignores the viewport, so only the RS call values and their order are checked, not where an object lands in
the image. The first null run with a `cam:` pose and the eye dumps is the real test (section 6, items 8-10).

## 4. Cost of `eyeres:100` (estimates, not measured live)

| item | old (2400x1350) | eyeres:100 (2688x2688) | source |
|---|---|---|---|
| pixels per eye | 3.24 Mpx | 7.23 Mpx (x2.23) | |
| eye swapchains, 2 x 3 RGBA8 | 74 MiB | 165 MiB | `eye_bytes` (runtime copies extra) |
| props targets, RGBA8 + RGBA32F, 2 eyes | 123 MiB | 275 MiB | `xr_props_d3d12.cpp` |
| model-cockpit depth D32 | 12 MiB | 27 MiB | |
| **total GPU memory** | | **about +260 MiB** | the 4090 has 24 GB |
| composite (terrain march + env + props merge), both eyes | about 0.6-0.7 ms | about 1.4-1.5 ms | gpu-cost.md scaled by pixels [I] |
| props raster | about 0.09 ms | about 0.2 ms | [I] |
| **frame GPU time** | | **about +0.9 ms** of the 11.1 ms at 90 Hz | [I] |

- **Commit charge:** these are DEFAULT-heap GPU resources. On WDDM they normally do not count against system commit, but this project has
  seen GPU slots matter (`ui3d_resample.h`). Measure commit % in the null run before the headset run.
- **Large values:** `eyeres:150` (4032x4032, 5x the old pixels) is about +850 MiB and +2.8 ms over the old policy. 200 hits the 4096x4096
  budget, about the same again. Do not use them before 100 has been measured.
- **Other paths:** the pacing and completion windows are unchanged. The png_once readback grows to 2688x2688 (28 MiB per image).

## 5. Larger redesign (not implemented; ordered by gain per effort)

1. **Producer eye targets at the eye size (loss 3) - implemented in step 3 (section 3c), offline only.** The staging below is kept as the
   design record. Differences from it: (b) png_once refuses on the eye ring as a whole; (c) the summary line is `summary-eye-ring` in
   step5-producer.log; (d) smoke/sprite already used the ring DSV (not the HUD DS), so they only needed their own size.
   Why not in this step: every replayed draw runs under the game's own viewport/scissor, the publish path checks that the ring, the world
   copy and the game RT have one size, and effects/smoke/sprites share the game-size HUD depth-stencil. Each is a lifetime/state change in
   `gpu_copy_step5_hooks.inl` that only a null run can check, and a wrong viewport shows up as a shifted or cropped object layer.
   Exact steps (one commit each, each behind `eyeres` > 0 and byte-identical when off):
   - (a) **Eye size to the producer.** `xr_stereo_proxy.cpp` knows `eye_res_pct` at cfg time; the recommended size is only known after
     `xrEnumerateViewConfigurationViews`. Publish `{w, h}` once from `Output::setup` (after the `eye-res` line) through an atomic the producer
     reads in `step5CreateRing` (`gpu_copy_step5_hooks.inl`, around line 4066, the `s5->ringW/ringH` assignment near line 4532). If the size
     is not known yet when the ring is created (vsearly creates swapchains in the menus, the ring is created at the first flight frame), use
     it; if it is not known, keep the game size and log `eye-ring=game-size(no-eye-extent)`. The ring is fixed for the process.
   - (b) **Separate sizes.** Split `ringW/ringH` (eye targets) from the game viewport (`Targets.width/height`): `step5StateWhy` (line ~1379,
     `viewport-size-changed`) must compare the live viewport with the GAME size, not the ring. `step5CreatePng` (line ~4130,
     `image-sizes-differ`) needs its own eye footprint or must refuse the eye part only. `step5WorldSourceWhy` stays at the game size.
   - (c) **Viewport/scissor substitution.** In `step5RecordMember` (line ~1561) and the HUD/effects/smoke/sprite recorders, after `OMSet`:
     `RSSetViewports` with the game viewport scaled by (ringW / gameW, ringH / gameH) and `RSSetScissorRects` scaled the same way; after the
     replayed draw, restore the game's viewport and scissor (from the tracked RS state, `t.vp0` / scissor, like `omRtv` is restored). Count
     substitutions and restores in an audit line (`summary-eye-ring;draws;vp-sub;vp-restore;scissor-sub`) that is unconditional while on.
     NDC is unchanged (the eye matrices already carry the HMD FOV), so no constant changes are needed **unless** a game shader reads
     `SV_Position` or a screen-size constant: prove with `wcdiag:2` (magenta) first that objects land on the same NDC.
   - (d) **Targets that follow the eye:** the object ring (RT + DS, 2 eyes x 3 slots) and the per-eye smoke/sprite targets. The effects image
     (one, mono) and the HUD/markers stay at the game size (they are sampled in game-screen uv). Effects/smoke/sprites currently share the
     HUD DS: the per-eye ones need their own eye-size DS.
   - (e) **Consumers:** `xr_stereo_output.cpp` builds `job.eyes/depth/smoke/sprite` with `f.width/f.height`; the shader samples them by uv
     (`dc_source_uv`) and reads depth with `GetDimensions` + `Load` (already size-agnostic). Only the `{.., f.width, f.height}` descriptors
     and the `mapping` line change.
   - (f) **Memory:** today the ring is 2560x1440 x 16 B (FP16 RT + D32 DS) x 2 eyes x 3 slots = 338 MiB (+ the 88 MiB world copy). At
     2688x2688 it is 661 MiB (+323 MiB). The per-eye smoke and sprite targets (FP16, 2 eyes x 3 slots each) are 331 MiB per kind at
     2688x2688 against 169 MiB today (+162 MiB per kind). Total about **+650 MiB** of DEFAULT-heap memory. Check `hud_marker_total_cap` (128 MiB) and `effects_budget`, which are sized
     for the game frame, and commit % in the null run.
   - (g) Offline: a fixture for the viewport scaling (game 2560x1440 viewport -> 2688x2688 target, odd sizes, a sub-rect viewport, a
     scissor smaller than the viewport) and the restore order. Then a null run with png_once: object edges at 2688 vs 2560 dumps of a
     `cam:` pose.
   Opus-level GPU/lifetime work; Astra review of the pinned commit before install.
2. **Filtered sampling of the game world window (loss 2): done in step 2, section 3b.**
3. **Resolution-aware terrain LOD (loss 5).**
   - Pass a `lod_bias = log2(old_px_per_tan / new_px_per_tan)` (about -1 for eyeres:100 vertically) through the b1 constants and use
     `kTLodScale * 2^bias`.
   - Shader-only, but it changes the look of the terrain. Gate it under its own token and compare dumps.
4. **UI3D / briefing producer images** (`kEyeW/kEyeH` 1440): size them from the eye extent. They are 192 MiB today at 3 slots x 2 eyes, so
   check memory first.
5. **Game render resolution (no code):** the producer (C) and the world window (D) follow the game's own target size. A higher in-game
   resolution, if ACZ PC offers one above the monitor (DSR or a custom resolution), scales C and D. Untested. The ring memory scales with it.

## 6. Headset / null check list

Null run first (no approval needed, playbook 8). Add `;eyeres:100` to `xr_terrain_stage` in the batch's cfg copy. runBatchAG restores the
user's cfg.
1. `xr-stereo.log`:
   - `event=eye-res-config;ok=1;eyeres=100;prerequisites=ok;effective-pct=100`.
   - `event=swapchains;...eye0=2688x2688...policy=recommended-x100%(eyes)...recommended=2688x2688-used`.
   - `event=eye-res;used=1;clamp=none,none`.
   - `event=layer-fov` with `eye0-px-per-tan` about 1154x1169 and `ratio-x-over-y` about 0.99 (was 1.754).
2. Empty logs mean the cfg was refused: check the value length first (playbook 8).
3. Commit % during the flight against the previous null batch (the stop rule is still 95 % twice).
4. `summary-timing`: `gpu-start-ms`, `gpu-blit-ms`, `end-late-ms-max`, `xr-period-ms-max` against the previous batch. No new `end-failed`.
5. png_once / png-eye.now: the eye dump is 2688x2688. Rotate it 180 degrees. Compare the horizon, tree edges, the cockpit gauges and the
   game-window edges with a 2400x1350 dump of the same pose (`cam:` debug pose). Then look at the null mirror.
6. Menus (vsearly): the vscreen and the briefing table still show, and the table text is not worse.
7. World filter (section 3b): `event=eye-res` ends with `world-filter=footprint-grid(max-2x2,bit21)`; `event=summary-world-filter` appears
   every period with `frames` > 0 whenever the world composite runs. To SEE the filter in a null run, the window must be shown: run one batch
   with `-AllAppend ';vrn:1'` removed (or a stage without terrain) and compare the png_once eye dump at `eyeres:100` against `eyeres:0` on the
   runway / hangar edges and the in-game text in the window: edges smoother, no blur of large areas. Same `summary-timing` check as item 4.

8. Eye ring (section 3c), `eyeres:100` with `eyeobj` default on, `vsearly:1` (the batch base has it):
   - `xr-stereo.log`: `event=eye-res-config;ok=1;eyeres=100;prerequisites=ok;effective-pct=100;detail=ok;eyeobj=1`.
   - `xr-stereo.log`: `event=eye-ring-consumer;source=2560x1440;per-eye=2688x2688;eye0=2688x2688;` once, at the first flight frame.
   - `step5-producer.log`: `gpu-copy-step1;event=eye-ring;mode=eye;why=eye;game=2560x1440;per-eye=2688x2688;eye-extent=2688x2688;eyeobj=1;ring-mib=661(game-size-337)`.
     `mode=game-size;why=no-eye-extent` means the ring came before the swapchains: check vsearly. Then the run is the step-2 picture.
   - `step5-producer.log`: the `event=ring` line has `per-slot=2x(RT ... 2688x2688 + DS ...)`.
   - `step5-producer.log`, every status period: `event=summary-eye-ring;mode=eye;...;member-draws=N;fx-replays=M;vp-sub=;scissor-sub=;vp-restore=;rs-refused=R`.
     Expect vp-sub = scissor-sub = vp-restore = N + M, rising in flight, and R = 0. R > 0: grep `last-member-refusal=eye-ring-` in the
     continuous-quality line (a game draw with 2 viewports or no scissor).
   - `fx-smoke-ring` / `fx-sprite-ring`: `outcome=ready` with `size=2688x2688`. A refusal `*-over-*-cap` means the eye caps were not taken.
   - Unchanged: `continuous-quality` `world=valid` count, `published`/`skipped`, no new `fail`/`fault` lines, `fx-smoke-gpu`/`fx-sprite-gpu`
     replays > 0 in a smoky flight, as in the last batch without eyeobj.
9. Pictures (the real proof; the fake rasteriser ignores the viewport): one `cam:` pose, three runs: `eyeres:0`, `eyeres:100;eyeobj:0`,
   `eyeres:100`. In the consumer eye dumps (`xr_png_once` / `png-eye.now`, rotate 180; the producer `gpu_copy_step5_png_once` refuses on the eye
   ring), aircraft, targets, missiles and the replayed cockpit band must sit at the SAME place in
   all three. They must not be shifted, scaled into a corner (a viewport left at the game size draws into the top-left 2560x1440 of the
   2688x2688 image) or cropped (a scissor left at the game size cuts the right/bottom). Edges should be sharper in the third run. Smoke puffs and
   explosion sprites (fxmem:4/6) must sit on their trails, not offset from the objects. Then the null mirror.
10. Commit % against the eyeobj:0 run: about +0.65 GB of GPU memory more (DEFAULT heap). Stop rule unchanged.

Headset (user):
0. If objects, smoke or explosions are misplaced, cut off or missing with the eye ring, use the fallback `eyeres:100;eyeobj:0` (no reinstall)
   and send `step5-producer.log` + `xr-stereo.log`.
1. In flight, compare the horizon, terrain, trees, cockpit gauge text and HUD with test 8. Is it closer to the cinema screen?
1b. Eye ring: are far aircraft, targets, missiles and smoke trails sharper (less jagged edges, thin tracers stay visible), with no shift against
   the terrain while you turn your head? Compare with the same flight at `eyeres:100;eyeobj:0`.
2. Turn the head slowly while watching the game-window edges and far aircraft: is there less shimmer? (The filter only acts where the
   game's window is shown: with the terrain on and no `xr_terrain_blend`, the in-flight world is our terrain, not the window.)
3. Smoothness: fpsVR frame time and reprojection, and any stutter, against test 8.
4. The briefing table, menus and debrief are all still there.
5. If it does not hook, send `%LOCALAPPDATA%\acz-vr\logs\xr-stereo.log`.
6. Rollback: `eyeres:100;eyeobj:0` (keeps the sharper eye swapchains, producer back to the game size), or remove `;eyeres:100` / set `eyeres:0`
   (everything as before eyeres). No reinstall is needed.
