# Terrain renderer (D-291 route 2): baker, loader, per-eye ray-marched heightfield pass

WORLD-1 (roll sign, colour/haze/sky, one camera, object occlusion) changed sections 3-4: see `world1-impl.md`.

Status: code written, built (Release, D:\b-tr), offline tested incl. the real shader on WARP. **Never run in the game or a headset.** Everything about
game fidelity (stage id, yaw/handedness, unit scale, colours, water, objects) is INFERENCE from docs/world-render/terrain-calibration.md and stays
`provisional` until the live calibration below is done. Tags: **[V]** verified by a test in this repo, **[I]** inferred / unverified.

## 1. What it is
Default OFF. With `xr_terrain_render=true` + `xr_terrain_stage=<id>[;opt:val...]` (plus `xr_world_in_projection`, the E6 camera feed or `cam:`),
the base WORLD layer of each eye's composite (the layer under the cockpit/HUD/effects) is rendered from the baked stage instead of the game's
world snapshot. Cockpit, replayed world objects, HUD, markers and effects composite on top exactly as today. A/B option `blend:1` draws the game's
world window over the terrain (terrain-only is the default). Any failure (bad option, missing/corrupt .zvrs, no camera, GPU resource failure)
falls back to the game's own world image for that frame/session and logs it (`xr-stereo;event=terrain-disabled`, counters `no-camera`, `backend-failed`).

Design choice (justified): a **full-screen ray march of the 513x513 heightfield in the existing eye pixel shader** (HLSL `terrain_trace`, mirrored
by the CPU reference `acz::terrain::trace`). No new PSO, vertex buffers, depth buffers, LOD rings or clipping: the per-eye view ray is the SAME ray the
world-plane composite already builds (`WorldComposite`), so the tracked-eye pose is applied exactly once, the near plane/horizon/low-altitude cases need
no special handling, and the key-off path is untouched. Cost: up to 320 steps + 8 bisection steps per pixel that is not under an opaque cockpit pixel
(early out when `o.a >= 1`); adaptive step `clamp(0.4*(y-h), 128+0.004 t, 8192)`, march stops at the haze end. [I] GPU cost on a real headset unmeasured.

## 2. Files
| file | role |
|---|---|
| `research/world-render-data/stage_baker.py` | offline, numpy+stdlib, no network: a.arc (via `arcread.py`) or an extracted tree -> `%LOCALAPPDATA%\acz-vr\stage\<id>.zvrs` |
| `src/acz-vr/terrain_stage.h/.cpp` | SDK-free: .zvrs loader/validator + serializer, option/convention parser, camera frame, CPU reference renderer, SHA-256 |
| `src/acz-vr/terrain_camera.h` | seqlock mailbox of the E6 camera (campos, rot pitch/yaw/roll, heading) |
| `src/acz-vr/ee_camera_probe_win.cpp` `terrain_tick` | per-Present read-only copy of the E6 fields into the mailbox (only with the key; guarded reads inside the validated 64 MiB block) |
| `src/acz-vr/xr_stereo_output.{h,cpp}` | `TerrainRuntime`, `BlitJob::terrain*`, per-eye camera, counters/log lines |
| `src/acz-vr/xr_stereo_d3d12.cpp`, `xr_stereo_blit.hlsl` | resources (heights R8_UINT, tile words R16_UINT, RGBA8 tile array with 7 mips), one-time upload, root constants `b1`, the shader pass |
| `src/acz-vr/xr_stereo_proxy.cpp` | parse + load + log, builds `Settings::terrain` |
| `tests/acz-vr/terrain_offline_tests.cpp`, `xr_stereo_output_offline_tests.cpp --terrain`, `xr_composite_warp_harness.cpp case_terrain` | tests |
| `tools/live/vr-test-start.ps1 -TerrainRender '<stage>[;opt:val]'` | writes the keys (adds `ee_camera_probe=true` unless `cam:` is used) |

### .zvrs v1 (little endian; the Python baker and the C++ serializer agree byte for byte [V], checked against the baked m02a/m16n files)
Header 256 bytes: `"ZVRS"`, u32 version=1, char[8] stage id, u32 flags (bit0 sea level guessed, bit1 sea level present), u32 hdim=513, tgrid=256, ntiles (<=1024),
tilepx=64, nobj, f32 cell=1024, hstep=80, half=262144, sea_level, u32 offsets of the 5 sections + file size, SHA-256 of bytes [256,end) at 192, SHA-256 of the
concatenated source files at 224 (informational). Sections (4-byte aligned): heights u8 513x513 (row=Z, col=X, y = byte*80); tile words u16 256x256 (low 10 bits
tile index remapped to the used set, upper bits original flags [I: unused]); tiles u8 ntiles*64*64 (mip 0 only); CLUT RGBA 256; objects (x,y,z,model) 16 B each.
The loader refuses: short/oversized files, bad magic/version/header/size/alignment, SHA mismatch, tile word >= ntiles, non-finite scalars [V, 24 error cases].
Baked now (not committed, game data): `D:\acz-extract\stage\m02a.zvrs` (4,116,372 B, 908 tiles, 105 objects, payload sha256 efda0049...a2d1) and `m16n.zvrs`
(4,450,564 B, 990 tiles, sea plane at 0 u flagged as a guess, sha256 f24be529...8307). Copy them to `%LOCALAPPDATA%\acz-vr\stage\` (or re-bake there: `python -I
research/world-render-data/stage_baker.py m02a`; `--arc <a.arc>`, `--src <extracted map dir>`, `--out <dir>`, `--sea-level auto|none|<units>`).

## 3. Config keys (budget: the 24-key headset set + ee_camera_probe + these 2 = 27 of 32 keys, < 2048 B [V])
* `xr_terrain_render=true` (bool, default off). Latched only with `xr_stereo_output` + `xr_world_in_projection` and a non-empty stage string.
* `xr_terrain_stage=<id>[;opt:val;...]` string value (config lines cannot contain a second `=`, so options use `:`; the parser also accepts `=` for tests). id = `[A-Za-z0-9_]{1,8}`.
Options (all validated, unknown/bad => the whole terrain is disabled and logged, never half applied):

| option | default | meaning |
|---|---|---|
| `yaw:<deg>` | 0 | added to the heading (after the sign) |
| `sgn:-1` | 1 | flip the sign of the logged yaw |
| `mirror:1` | 0 | negate the camera right axis (handedness flip) |
| `psgn:-1` | 1 | pitch sign |
| `rsgn:1` / `rsgn:-1` | -1 | roll sign (WORLD-1: -1 = positive E6 roll is a LEFT bank, measured on headset test 16; +1 was the test-16 bug) |
| `alt:<u>` | 0 | added to the camera y |
| `fixy:<u>` | off | replace the camera y (the logged y=4000 may be a constant, calibration doc) |
| `upm:<n>` | producer `gpu_copy_step5_units_per_metre` (WORLD-1) | head offset metres -> camera frame units; without the option the terrain uses the replayed objects' scale |
| `csrc:game` / `csrc:e6` | game | WORLD-1: terrain orientation from the frame's game camera (+ DrawACM_v eye), cross-checked against E6; `e6` = E6 Euler only |
| `e6:frame` / `e6:latest` | frame | WORLD-1: the E6 sample read at the frame's first member vs the newest Present-tick sample |
| `gmax:<deg>` | 15 | WORLD-1: a game camera further than this from E6 is refused for that frame (E6 used, counted) |
| `occl:0/1` | 1 | WORLD-1: terrain hides replayed objects behind it (needs `xr_world_objects_depth_fix`) |
| `fogl:<u>` | 55000 | WORLD-1 haze length L: f = 1 - exp(-t/L) toward the horizon colour (130,148,141) display, fitted on the step56 game shot (`fog0` was removed: no start distance) |
| `far:<u>` (`fog1:` legacy alias) | 160000 | march end; the haze reaches 1 smoothly over the last 20% (no flat band) |
| `sea:<u>` / `sea:off` | baked | override / disable the flat sea plane (water tiles are not understood: flat tinted plane, flagged unknown) |
| `blend:1` | 0 | A/B: the game's world window over the terrain (calibration tool) |
| `cam:x,y,z,deg` | off | fixed camera (desktop / no-E6 test); heading in degrees |

### 3a. `xr_terrain_stage` preset `vrn:1` and the value limit (D-400)

The option string outgrew the old 255-character buffer (the day value reached 241 characters), so there are two independent fixes.

1. **Preset token `vrn:1` ("VR native").** `runtime_config.cpp` expands it ONCE, when it stores the value (`terrain_preset.h`), into
   `ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;pov:3;povrig:level`, in place of the token.
   Every consumer (vscreen/vsearly, e6loc, look-stick, props, terrain, ui3d, brief3d, dbr3d, pov, pacing, recenter extractors in
   `xr_stereo_proxy.cpp`; the flight-stage template; the gpu_copy_step5 hooks) reads `xr_terrain_stage()` only, so all see the long form;
   no consumer reads the raw cfg text. Explicit tokens override the preset's by key (`vrn:1;pov:0` keeps `pov:0`, drops the preset's `pov:3`; `pov` and
   `povrig` are different keys). `vrn:0` expands to nothing. Any other `vrn` value, or two `vrn` tokens, rejects the whole config
   (`config xr terrain stage preset unknown`). The result is logged once at terrain start: `xr-stereo;event=preset;vrn=1;expanded=...`.
   Short day value: `auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;vrn:1`
   (further census/test options can be appended after it).
2. **Limit raised.** The value may now be up to 1023 characters (`xr_terrain_stage_capacity` 1024, `max_config_line` 1056, `max_config_bytes` 4096, was 255/288/2048),
   measured AFTER preset expansion. This was safe because the buffer is only copied (sizeof-bounded) into the diagnostics sink and read into `std::string`
   by the extractors; the reader's stack array is 4 KB. The 32-key limit is unchanged.

## 4. Named conventions, and where to change them
Defaults = docs/world-render/terrain-calibration.md (PROVISIONAL): right-handed, +X east, -Z north, Y up; heading h = E6 `rot[1]` (kE6YawField=1; the logged first 'yaw'
field is garbage), forward = (-sin h cos p, sin p, -cos h cos p), right = forward x up, positive yaw turns toward -X; camera position = E6 `campos` (already aircraft + 36 u
ahead + 3.6 u up, it rotates with the heading); `y` live unless `fixy`. All of them are fields of `acz::terrain::Conventions` (`terrain_stage.h`) set by the options above;
the maths is `camera_frame()` / `eye_world()` in `terrain_stage.cpp`. Shader constants (haze/sun/sky/sea colours, march steps, lod scale, ambient/diffuse) are named `kT*`
in `xr_stereo_blit.hlsl` mirrored 1:1 by `k*` in `terrain_stage.h` (edit both; the WARP test compares them). WORLD-1: the terrain is shaded in the game's DISPLAY domain (CLUT/255 x light, haze, sky table) and converted ONCE to linear at the end of the trace, because the
sRGB RTV encodes again (before WORLD-1 the display values were encoded twice: too bright and blue-white; see world1-impl.md).

Per-eye position: `eye_world = campos + upm * (right*o.x + up*o.y + fwd*(-o.z))` with `o` = the eye origin in the anchor frame (metres, includes the seated neutral, the
head pose and the IPD; `WorldComposite::origin`). View ray: `normalize(right*d.x + up*d.y + fwd*(-d.z))` with `d` = the anchor-frame ray of the pixel (the existing
`WorldComposite` maths, tested). So the HMD orientation turns the ray exactly like the cockpit/world plane, and the E6 rotation maps the cockpit frame into stage space.

## 5. What is verified (offline) and what is not
[V] .zvrs round trip, byte identity with the Python baker, 24 loader error cases, option parser (17 bad strings), camera frame (heading -45 deg => forward (+0.707,0,-0.707),
orthonormal for 50 random attitudes, mirror/yaw/sign/offset), bilinear heights, +X=column/+Z=row, sea plane, adaptive ray march vs a 4-unit brute force on 576 rays
(576/576 agree, worst |dt| 3.9 u), determinism (CPU and GPU bit-identical on repeat), the real shader on WARP vs the CPU trace on 320x180 per eye (max error 0.005, 0 bad pixels, 5
cases: down/turn/sea/blend/refused stage), key off = byte-identical eye images, refused/dead generation = plain world layer, consumer: per-eye cameras, no-camera fallback, non-finite
camera refused, inert without `xr_world_in_projection`, counters, config latch/budget, full ctest (all pass).
[I] stage m02a for mission 02; yaw sign / handedness / 90 deg ambiguity; unit scale; the E6 `campos` being the right eye reference; E6 offsets stable per launch via the probe's
`obs_base`; tile flags; colours/gamma; water; sky/haze colours and distance; the terrain being the whole world (no objects: .pdl markers, trees and buildings are NOT drawn);
GPU cost; (corrected by E6-ANCHOR-FREE, docs/world-render/e6-anchor-free.md: the feed never needed look events - it read the probe's observation base, which was the all-zero
block until the first relative scan; the default `e6loc:auto` now locks the E6 block by its content); the world snapshot being present for a frame (terrain is drawn only in
composite frames, i.e. when `world_composite` is on); OpenXR runtime behaviour. `-TerrainRender` has not been run.

## 6. Live calibration recipe (headset, user present; this is the only step that can verify registration)
1. `python -I research/world-render-data/stage_baker.py m02a` (or copy the baked file) into `%LOCALAPPDATA%\acz-vr\stage`.
2. `pwsh -NoProfile -File tools/live/vr-test-start.ps1 -DllDir <dll set> -TerrainRender 'm02a;blend:1'` then launch ACZ, go to mission 02, no look input is needed (E6-ANCHOR-FREE: watch `ee-camera.log` `e6-locate;event=locked`).
   Watch `xr-stereo;event=terrain-loaded`, `event=terrain` and `summary-terrain` (`terrain-frames` rising, `no-camera=0`).
3. With `blend:1` the game's window is drawn over the terrain: compare landmarks at the window edge (river bends, field patchwork, the horizon). Then `blend:0` and look around.
4. Adjust by editing `runtime.cfg` `xr_terrain_stage` and relaunching: `yaw:90/180/-90` for the compass rotation, `mirror:1` / `sgn:-1` for handedness, `alt:` / `fixy:` for height,
   `fog1:` for the haze, `upm:` for the head-offset scale. Record the final string and a screenshot pair in the evidence card; promote it to the defaults in `Conventions`.
5. No headset needed for a smoke test of the render path: `-TerrainRender 'm02a;cam:25137,4000,82532,-45'` with the null driver renders from a fixed camera (no E6 needed).
