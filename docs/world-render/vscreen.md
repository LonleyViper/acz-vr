# VSCREEN: the whole game on a virtual screen in VR (D-362 roadmap item a)

Status: implemented behind `vscreen:1` (a sub-option of `xr_terrain_stage`, no new config key, default OFF), offline-tested only.
No live or headset run has exercised any of it. Code: `src/acz-vr/vscreen.h` (pure), `vscreen_capture.h` / `vscreen_capture_win.cpp`
(capture at Present), `xr_vscreen.h` / `xr_vscreen_d3d12.cpp` (XR consumer hook), `xr_vscreen.hlsl`; small hooks in `xr_stereo_output.cpp`
(`pace`, `transact`, `retire`, `compose_layers`), `xr_owner_openxr.cpp` (screen-only quad), `xr_stereo_proxy.cpp` (option, one hook per
attempt), `ee_camera_probe_win.cpp` (one call before the native Present). Test: `tests/acz-vr/vscreen_offline_tests.cpp`.

## 1. What the XR session does outside flight today (log evidence)

| situation | evidence | what the headset gets |
|---|---|---|
| from launch to the first flight frame | every log: `armed`, `waiting-for-source;published=0;try-acquire=not-tried` every 5 s; `source-ready` only when the producer publishes its first StereoFrame (test 19: 14.9 s with the skip-intro save; step75: 113-128 s; tests 9-16: 9.6-32.5 s) | no OpenXR instance at all: SteamVR shows no app, the game is only on the monitor |
| after a mission ends (MISSION FAILED menu, results) | `D:\acz-evidence-archive\step83\runE10-logs\xr-stereo.log`: at T+185 s `stale-zero=232`, then every summary `frames=600;submitted=0;new=0;stale-zero=600;stale-age=600` until the game closed (82 of 119 summaries); producer `published` stops growing | the session stays alive (FOCUSED, 90 Hz frames) but every xrEndFrame has ZERO layers (`pace()`: re-submit only for 500 ms, then zero layers): a black/empty headset |
| a game stall (loading, a scene change) | test 19 `xr-stereo.log`: attempt 1 ended at 674 s `first-cause=progress-deadline` with `frame-cpu-ms-max=1968`, `end-late-ms-max=1936`; attempt 2 the same 6.9 s later; attempt 3 hooked 60 s later (`yes-persistent(slow-60s)`); the producer kept publishing 60/s meanwhile and ee-camera `present-stats` showed `max-gap-ms=465.9/885.9` | the session is torn down for 60+ s (the consumer's frame waited on the game's GPU queue/producer for ~2 s, past the 250 ms progress deadline) |
| the producer's view of menus | `step5-producer.log`: `continuous;published=N` grows only with world members; the Present hook still sees 60 presents/s (`present-stats`) | the game's final image exists every frame, only nobody copies it |

The game's swap chain (`pacing;event=swapchain-desc`): 2560x1440, format 24 (R10G10B10A2_UNORM), 2 buffers, FLIP_DISCARD, windowed,
sync interval 1. XR swapchain format 29 (R8G8B8A8_UNORM_SRGB); 28 is not offered by SteamVR (`formats;offered-28=0`).

## 2. Design: the screen layer (consumer)

* Trigger: `Output::pace()` (an XR frame without a new producer frame) asks the hook first. `ModeMachine` (vscreen.h): flight age =
  time since the last FRESH flight composite (`last_new_ms_`; a refused frame or a space change does not end a flight); older than
  `vsenter` (default 500 ms = the old re-submit limit, so nothing changes inside it) or no flight since the session started -> screen.
  The capture is requested 350 ms before the takeover so the first screen frame normally has an image. Exit: the first fresh flight
  composite (`transact` composes it as before; the screen is simply not asked). `vspov:flat` makes the screen permanent (`transact`
  does not compose producer frames at all).
* Layer: ONE opaque `XrCompositionLayerQuad` in LOCAL (`ProjectionLayer::screen_only`; `compose_layers` returns only `screen_quad` on
  target kHud; no projection layer, no eye swapchain referenced). Default 3.2 m wide at 2.5 m (about 65 deg), height from the image
  aspect. World-locked: placed at the head yaw (pitch/roll ignored, level) and `vsy` height when the screen mode is ENTERED, again after
  a LOCAL space change (SteamVR recentre = the user's recentre button) and, with `vsfollow:1`, after looking > 50 deg away for 1.5 s.
* Image path: the existing HUD swapchain (2400x1350, format 29; it exists with `xr_hud_layer`, which the day install has; without it the
  Output drops the hook). The HUD layer is never submitted in screen mode, so the swapchain is free. Per NEW captured image: acquire +
  wait (RENDER_TARGET) -> barrier to COPY_DEST -> CopyTextureRegion from the capture's R8G8B8A8_UNORM image (same typeless family as the
  _SRGB swapchain: the raw display bytes are copied, the compositor decodes them as sRGB exactly as the monitor shows them) -> barrier
  back -> execute on the XR binding queue -> Signal(private fence) -> release. Without a new image the layer is re-submitted WITHOUT any
  acquire or GPU work (the swapchain's last released image): a stalled game queue is never waited on by the screen path.
* Curvature: `vscurve:1` is parsed and the cylinder parameters are computed (`Placement::cyl_*`: radius = vsd, angle = width/radius), but
  `XR_KHR_composition_layer_cylinder` is not enabled at xrCreateInstance yet, so the layer stays flat and the config line says
  `curve=requested-flat(cylinder-extension-not-enabled)`. Next step: request the extension when the runtime lists it and submit
  `XrCompositionLayerCylinderKHR` for `screen_only` (additive in `end_frame_projection`).

## 3. Capture of the game's final image (Present)

`acz::vscreen::on_present(swapchain)` is called by the existing Present/Present1 vtable hook (`ee_camera_probe_win.cpp`, armed with
`ee_camera_probe`, i.e. the terrain camera feed of the day install) for the outer, non-TEST call on the game's swap chain, BEFORE the
native Present. It returns at once unless `vscreen:1` was parsed AND the XR consumer requested images within the last second.
Then, on the swap chain's own queue (the queue the game passed when it created the swap chain, recorded by the DXGI factory wrappers; section 7.1 item 3), one private list: back buffer PRESENT ->
COPY_SOURCE, CopyResource into a private staging texture (back-buffer size/format; sRGB formats copied into their TYPELESS family),
back buffer -> PRESENT, one full-screen draw (xr_vscreen.hlsl, bilinear, raw values) into the slot's R8G8B8A8_UNORM image at the XR
swapchain extent, staging and slot image back to COMMON, ORIGINAL ExecuteCommandLists, Signal(private fence). Cost estimate (not
measured): one 14.7 MB copy + one 2400x1350 draw, well under 0.1 ms on the RTX 4090, only while the screen is shown or about to be.
Memory: staging 14.7 MB + 3 x 13 MB slot images, created on first use, kept for the process.

## 4. Starting XR before the first flight (menus): implemented as VSEARLY (vsearly.md), the history below

Today the monitor thread starts OpenXR only at the producer's first published StereoFrame (`start=lazy-first-published-frame`). With the
skip-intro save that is ~15 s after launch; a full intro/menu path is ~2 min. Starting on the first CAPTURED screen frame would be simple
in the monitor loop, but three things are decided ONCE at `source-ready` and would then be decided in the main menu:
1. `xr_terrain_stage=auto` stage detection (`detect_auto_stage`: 3 bounded scans, then the terrain for the whole process) - in the menu
   it would find no stage or a wrong one (D-362 already reports a wrong map at takeoff);
2. the cockpit auto/assumed type and the source extent (`settings.assumed_src_w/h`, eye swapchain sizes) from the producer probe;
3. the terrain camera epochs/admission state.
Plan (needs the stage-detection owner): (a) make the terrain runtime replaceable per flight (detect the stage at each flight start, i.e.
when the producer publishes again after a screen period; bump the existing stage epoch, swap the `TerrainRuntime` shared_ptr between XR
frames); (b) then let the monitor fire on `capture published >= 1` when `vsearly:1`, with the extent from the swap chain; (c) the
capture must run before XR exists (request it from the monitor while waiting). Until (a) lands `vsearly:1` is accepted and ignored
(logged `early=ignored(...)`). The per-flight re-detection also fixes "the second mission keeps the first mission's terrain".

Now (docs/world-render/vsearly.md): (a) is done as a per-flight stage slot (flight_cycle.h; the stage is replaced/cleared between XR frames,
detection on a worker thread at each flight start); (b) the monitor starts on >= 120 game presents with an assumed 2560x1440 source;
(c) is not needed: the consumer requests the capture on its first screen frame (the first 2-3 frames have zero layers). The cockpit auto
type is detected lazily by the consumer already. The `vscreen-option` line now says `early=see-vsearly-config(vsearly.md)`.

## 5. Transitions and the pause menu (interface with the HUD/menu-in-front agent)

* flight -> screen: no fresh composite for `vsenter` (mission end, results, briefing, hangar, cutscenes that stop the world draws,
  loading). Until then the old re-submit (<= 500 ms) shows the last flight image. The screen is placed where the user looks at entry.
* screen -> flight: the first fresh composite. No hysteresis on purpose (a stray composite in a menu has not been seen in any log);
  `summary-vscreen;transitions=` counts them, a live run decides whether a minimum flight run is needed.
* Pause menu DURING flight: it is drawn by the game into the frame the producer replays, so the producer keeps publishing and vscreen
  does NOT engage; showing the pause menu in front of the HUD stays with the HUD-size/menu-in-front agent. Interface for them:
  if the game's pause stops the world members (the producer stops publishing), vscreen takes over after `vsenter` automatically and
  shows the whole paused screen (menu included) flat in front of the user; if it does not, their HUD/menu layer is the path. No
  shared state is needed; `ProjectionLayer::screen_only` is exclusive (no HUD/marker layer is ever submitted with it), so their layer
  ordering in flight is untouched. If they later want an explicit "show the screen now" request, the place is `ModeMachine::step`
  (one more input, e.g. a pause flag) - not implemented to avoid two owners of the same decision.

## 6. Alternative POVs

| POV | status | why |
|---|---|---|
| the game's own view flat on the virtual screen (whatever camera the game uses: cockpit, HUD-only, external/chase) | implemented: `vspov:flat` | it is just the captured final image; mono, no head tracking inside the image |
| stereo HUD-only (no cockpit model, world + HUD in 6DoF) | not implemented; W1 answered the questions below: see pov.md (HUD-only = the cockpit E6 eye, the producer publishes NO frame: no DrawCPT; `pov:1` classifies it and keeps the flight stage alive) | needs a signal that the game is in its HUD-only view. The model cockpit is drawn by AIRCRAFT-AUTO whenever a type is detected; the game's HUD-only view keeps the E6 eye at the cockpit position (inference), so the E6 offset cannot tell it. Candidates: the producer's cockpit members (`members=7` in the cockpit view) dropping to 0 in HUD-only view, if the producer then still publishes. Live check 6 below decides it; then it is a cockpit option (suppress the model while members==0), not vscreen |
| stereo chase / external camera | not implemented; W1: the E6 camera is the chase rig (76-84 units behind, 17 above; the locator rejects it), no producer frame; design + `pov:1` classifier in pov.md | the terrain uses the E6 camera, which should follow the game's external camera (it is the game's render camera); the producer replays world members from the game's camera with the per-eye offset, so the world itself may already be correct in stereo. What breaks: the model cockpit (drawn at the head) and the in-cockpit HUD anchor. `aircraft::eye_in_aircraft` (E6 eye inside the aircraft box) is the natural switch: outside -> no model cockpit, HUD layer head-locked. Needs live check 6/7 (does the producer publish in the external view; does `cockpit-auto` report `off-centre`) before any code |

## 7. GPU lifetime (for review)

* Capture (game Present thread): objects created inside a proxy creation scope, every call through `nativeDispatch` (the proxy's hooks
  never see them; the submission bypasses the hooked ExecuteCommandLists like the XR consumer's). A slot is re-recorded only when it is
  FREE (never HELD) and its previous capture value is <= the fence's completed value (its allocator is then idle); the newest READY slot
  is never overwritten. A failed Signal after Execute leaves the slot WRITING for ever and disables the capture. Re-creation (back-buffer
  size/format or XR extent change) only through `Ring::retire_all`, which atomically requires no HELD/WRITING slot and every capture
  completed. Objects are never destroyed otherwise (process lifetime). A swap chain on another queue disables the capture (fence order).
* Reader (XR owner thread): acquires only READY slots whose capture completed (CPU-observed; no queue wait), keeps exactly one HELD until
  its own copy's private fence value completed, then releases it. Its allocators (2) are reset only after their last value completed.
  Swapchain image: acquire -> wait (100 ms retry) -> copy -> release; a failed wait/release marks the hook dead and `retire()` returns
  false so `Output::retire` retains the swapchains (never destroyed under an outstanding acquisition). `retire()` waits <= 1 s for its
  last copy, then releases its private objects; the destructor makes no foreign call.
* Cross-queue: the capture image rests in COMMON; the XR copy uses implicit promotion to COPY_SOURCE (valid for textures from COMMON);
  both queues are the game's (the binding queue is the producer's device queue; the capture uses the swap chain's queue, normally the same).

### 7.1 Astra review 6455891169 (CHANGES REQUESTED, 4 P2): fixes

1. Shared HUD swapchain: every decision moved into the header-only `ScreenCore` (`xr_vscreen_core.h`). Its acquisition is settled inside the
   same call (wait succeeded -> copy -> release). A wait that never succeeds or a failed release POISONS the hook (sticky);
   `Output::vscreen_pace` checks `poisoned()` right after the call and fails the Output for the session, so neither the screen nor the flight
   HUD acquires, writes or submits target 3 again; `retire()` then refuses and the attempt is retained.
2. Fence loss: both fences are read as `acz::vscreen::Completion` (`completion_from_raw`: UINT64_MAX = loss, never a value). Ring
   `begin_write/acquire_latest/retire_all` take a Completion and refuse on loss; the capture disables itself on loss; the consumer never
   releases a held slot, never resets an allocator and never retires on loss. Execute-success/Signal-failure is an explicit quarantine flag
   (slot held for ever, hook poisoned), not a UINT64_MAX sentinel.
3. Queue: `IDXGISwapChain::GetDevice` is no longer used. The DXGI factory wrappers (`d3d12_hook.cpp`, all four Create* paths) record the
   DIRECT `ID3D12CommandQueue` passed as pDevice of a successful creation with its device (`QueueAssoc`, one queue reference kept for the
   process); the capture uses only that queue, re-checks its device identity on use, and never substitutes another queue.
4. Screen content: `content_ok` is set only by a released, successfully recorded+signalled copy; it is cleared when a copy is acquired, by a
   record failure (the released image keeps unknown content), by Output's own acquisition of target 3 (`target_written`, the flight HUD) and
   by poison. Without it the screen layer is not submitted (the old zero-layer path).

Tests: `vscreen_offline` (ring loss incl. Signal failure then loss, queue association incl. "swap chain cannot supply a queue", ScreenCore
with fake GPU/runtime: happy path with exact image indices, wait timeout, release failure, record failure, flight HUD then stalled capture,
loss, Signal failure then loss) and `xr_stereo_output_offline_tests --vscreen` (the real Output + ScreenCore on one fake HUD swapchain:
timeout -> flight and release failure -> flight: exactly one acquisition of target 3, no write before a successful wait, swapchains
retained; plus screen -> flight without a stale screen).

### 7.2 Astra follow-up 6458181039 (four fixes approved; one P2, one P3): fixes

* P2: the copy (allocator reset, record, Execute, Signal) runs again ONLY inside `g.call(Op::d3d_execute, ...)`, so the owner's deadline
  admission and call record apply. A refused call never invokes the callback; the result stays "not executed" and is handled like a record
  failure (capture slot returned, image released if the release is admitted, screen content unknown). If the release is refused too, the
  image stays acquired and the hook is poisoned (Output fails, the attempt is retained). Fixtures: execute refused after a successful wait
  (no GPU call, image released, clean balanced retire) and execute + release refused (acquisition retained, poisoned, retire refuses).
* P3: `retire()` destroys the backend whenever setup was ATTEMPTED (`destroy()` releases every object a partial setup created; no submission
  can exist then) and only once. Fixtures: setup failing after the fence, after the first allocator and at the initial Close, each with
  balanced create/release counts. The D3D12 backend keeps every created object in its members on a failed setup, so `destroy()` sees them.

## 8. Tests, unverified points, live check list

Offline (`vscreen_offline`): option parsing/removal (the day-install value unchanged byte for byte), mode decision incl. the capture lead
and `vspov:flat`, placement (yaw only, pitch ignored, 16:9, recentre generation), follow timer, the ring (held/latest never rewritten,
completion before acquisition, abort, retire_all) and a two-thread writer/reader stress, the screen-only layer list (one opaque LOCAL quad
on kHud; defaults unchanged). NOT tested offline: the D3D12 code (no fake device for it), the OpenXR quad submission, `Output` with a hook.

Unverified (inference): back buffer state PRESENT at Present; SteamVR accepting a screen-only frame (no projection layer); the cost;
CopyTextureRegion R8G8B8A8_UNORM -> the runtime's swapchain resource (TYPELESS or _SRGB, same family); colours matching the monitor.

Live check list (null driver first, then headset; config: day install + `;vscreen:1` appended to `xr_terrain_stage`):
1. `xr-stereo.log`: `vscreen-option;ok=1;vscreen=1;...;detail=needs-xr_hud_layer(present)` once; no `terrain-disabled;reason=bad-stage-option`.
2. In flight: `summary-vscreen;mode=flight;layers=0`; `cap-wanted` stays 0 or tiny; game fps unchanged (ee-camera `present-stats`).
3. Fly out of the area / fail the mission (runE10 route): within 1 s `summary-vscreen;mode=screen;entries=1;copies>0;layers` ~= XR
   frames; `summary;submitted>0` again instead of `stale-zero`; `cap-recorded` ~60/s, `cap-failures=0`, `cap-src=2560x1440/fmt24`,
   `cap-dst=2400x1350`.
4. Headset: the MISSION FAILED / results / briefing / hangar screens are readable on a level screen ~2.5 m ahead, colours as on the
   monitor (no washed-out or dark image), the screen stays put when turning the head; SteamVR recentre places it in front again.
5. Start the next mission: `mode=flight` at the first composite, the cockpit world returns, `transitions` +1, no `vscreen-result;dead=1`.
6. In flight switch the game's view (cockpit -> HUD-only -> external): record `continuous;published`, `members`, `cockpit-auto` lines;
   is the producer still publishing in HUD-only/external views? (decides section 6).
7. `vspov:flat`: the game's view (all three cameras) on the screen, 60 fps, no flicker; `vsfollow:1` re-places after looking away 1.5 s.
8. Stall: a loading screen with vscreen on: no new `start-attempt;...progress-deadline` caused by the screen path (`copies` stop, layers
   continue during the stall).
9. Exit the game while the screen is shown: `vscreen-result;...;dead=0` then `retire;outcome=clean`.
10. XR image of the screen (section 9): create `%LOCALAPPDATA%\acz-vr\png-screen.now` while the screen is shown; `png-screen-armed;outcome=armed`
   within ~1 s, the file gone, `png-screen;part=screen;outcome=queued;size=2400x1350`, then the writer's `png-file;outcome=written;file=..` and the PNG in `<logs>\png` (read it rotated 180 degrees).

## 9. SCREEN-DUMP: an XR image in screen mode (implemented 2026-10-28; offline only, not run)

The flight dump (`xr_png_once`) needs fresh game composites, which screen mode never has, so the menus / briefing table had no XR image.
`xr_screen_dump.h` (SDK-free decisions), `xr_dump_readback_d3d12.h` (the readback buffer), `ScreenCore` / `TableCore` / `Brief3dHook`
(`dump_arm` / `dump_take` / `dump_busy`), `Output::screen_dump`, the trigger in `xr_stereo_proxy.cpp`. No config key; the PNG sink exists
whether or not `xr_png_once` is set (its writer queue is created at the first image).

* Trigger: `%LOCALAPPDATA%\acz-vr\png-screen.now`. `Output::vscreen_pace` checks it (GetFileAttributesW) at most once per second and only
  while the screen layer is shown and no dump is busy; absent = that one check per second, nothing else. Present = deleted, then
  `dump_arm`: one trigger = one attempt (`png-screen-armed;outcome=armed|refused;why=..;table=armed|table-not-live|no-table`).
* Armed: the NEXT screen copy records, in its own list after the copy, `HUD image COPY_DEST -> COPY_SOURCE -> readback buffer -> RENDER_TARGET`
  (the image released and submitted as the screen quad, 2400x1350); with BRIEF3D and a live table, the next table blit likewise copies its
  eye-0 swapchain image (`RENDER_TARGET -> COPY_SOURCE -> RENDER_TARGET`). No extra submission, no CPU wait: each part is mapped only after
  its copy's own private fence value completed (polled per pace frame through `Op::d3d_sample`). Given up (named) after 2 s without a new
  copy (`no-new-copy`), 2 s pending (`readback-timeout`), on fence loss (`fence-lost`, never mapped) or when the buffer could not be created
  (`readback-setup`). The readback buffer (~13 MB for the screen) lives from the armed copy to the take; a given-up (timeout / lost) readback is
  DRAINING: its fence value is kept, `dump_busy()` stays true, the buffer is released (`readback_discard`) only after that fence is proven
  complete (a lost fence never is: kept until `destroy()`), and only then can a fresh trigger arm (review w2-2 finding 2).
* Output: `<logs>\png\acz-vr-xr-screen-t<armed ms>-screen.png` and `...-eye0-table.png`, one line each
  `png-screen;part=..;outcome=queued|failed;why=..;size=WxH;file=..` (`queued` = the sink accepted the image; the asynchronous writer then reports
  the FINAL result once per file, `png-file;outcome=written|failed;file=..;size=WxH`, via the process-lifetime log, disk I/O stays async; review
  w2-2 finding 3). A trigger file that cannot be deleted does not arm (`png-screen-armed;outcome=refused;why=trigger-not-consumed`, logged once per
  stuck file; finding 1). sRGB bytes as stored in the swapchain. Read every XR dump
  with `xr_dump_upright.py` (rotate 180 degrees): the screen part (an upright quad image) is therefore written rotated 180 degrees; the table
  part (an eye image) is written AS STORED, like the xr_png_once eye dumps, because an eye image rendered at the located pose is itself rotated
  180 degrees in the live runs (D-298; Q1a: rotating it too put the table upside down, far edge at 0.32 H instead of the placement's 0.68 H:
  b3y -0.45 m, far edge 1.25 m ahead, tan 0.36 at FOV +-45). The table part is the premultiplied left-eye projection layer composited over
  black (a separate layer over the quad; not re-projected onto the screen image).
* Tests: `vscreen_offline` (core_dump, core_dump_pending_timeout), `brief3d_table_offline` (test_dump), `xr_stereo_output_offline_tests
  --vscreen` (vscreen_output_dump: poll rate, file deleted once, one PNG; no trigger = nothing armed). Fixture success says nothing about the
  D3D12 readback on the device or what the headset shows.
