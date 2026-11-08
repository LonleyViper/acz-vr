# LAUNCH-MODE and NATIVE-VR START: a VR On/Off choice in the game's launcher, and XR from the first game frame

Status: implemented and offline-tested (2026-11-01). No game, SteamVR or headset run has exercised it. Code: `src/acz-vr/launch_mode.h`
(pure core), `src/acz-vr/launch_mode_win.{h,cpp}` (launcher combo, file, factory gate), `src/acz-vr/xr_start_gate.h` (start gate),
small hooks in `dllmain.cpp` (one CreateThread), `d3d12_hook.cpp` (the gate in `createWrappedFactory` and `notifyEarlyDevice`),
`xr_stereo_proxy.cpp` (monitor start gate), `vscreen.h` (the `vsstart:` token), `flight_cycle.h` (constants now shared). Tests:
`tests/acz-vr/launch_mode_offline_tests.cpp` (ctest `launch_mode_offline`), manual harness `tests/acz-vr/launch_mode_harness.cpp`
(target `launch_mode_harness`, not in ctest). Companion: `tools/acz-vr-mode.ps1`.

User request (2026-11-01): (1) VR from the first frame after Launch Game: logos, intro and menus in the headset; (2) one more dropdown in the
game's own pre-launch window, "VR Mode: On / Off". On = the current setup (runtime.cfg as it is, vrn:1); Off = the plain flat game, no XR
session, the proxies passive.

## 1. Findings (evidence)

* **Our dxgi.dll is loaded while the launcher shows, but does nothing yet.** acz.exe imports `dxgi.dll!CreateDXGIFactory2` statically (and
  `d3d12.dll` by ordinal 101), so the loader maps our proxy at process start. Its DllMain did nothing ("all work begins lazily from a factory
  export"). The first factory call and the D3D12 device come right AFTER the Launch Game click: in the user's 2026-11-01 session the
  launcher wrote `settings\setting.ini` at 09:04:39.77 (it saves on Launch), `diagnostics.evidence.log` was created at 09:04:39 and
  `xr-stereo;event=armed` (device hook) is at tick 317350187 = ~09:04:40.2 (tick-to-clock from the file's last write and last tick). In
  the archived null run clouds/runT1 `armed` (tick 46621734) is 87 ms before the driver logged `clicked Launch Game` (killcap.log maps
  05:52:08.958 = tick 46611656), about 10 s after the process started. The skip-intro XINPUT1_4.dll (a DllMain thread) already finds the
  launcher button 1.5 s after start (`skip-intro.log`: "launcher button 'Launch Game' found").
* **The launcher is a plain Win32 window built by acz.exe**, not D3D and not a dialog resource: the resource section holds only icons,
  version and manifest (types 3/14/16/24); imports include `CreateWindowExA` (main window, `RegisterClassExA`, `DefWindowProcA`,
  `GetMessageA`/`DispatchMessageA`), `CreateWindowExW` with the wide class names "ComboBox" and "Button", GDI (`StretchBlt`,
  `CreatePatternBrush`, `ExtTextOutW`, `FrameRgn`), UxTheme (`DrawThemeBackground`, `DrawThemeText`, `DrawThemeParentBackground`) and
  comctl32 ordinals 410/413/17 (`SetWindowSubclass`, `DefSubclassProc`, `InitCommonControls`): the launcher paints its own background, labels
  and hint line and custom-draws its combos and button. Strings: "Display Mode", "Display Resolution", "Player 1 Input Device", "Launch Game",
  "Start the game." (UTF-16), "SkipLauncher". The same top-level window becomes the game window (runK3ag: it grows to game size after
  Launch). The launcher has no D3D device, so it cannot be shown in the headset.
* **Why XR waited for 120 presents + 3 s.** `flight_cycle.h`: "The game window must be presenting steadily before XR starts (the capture
  needs presents; a session in the loader screen is pointless)". vscreen.md section 4 shows the original plan was "capture published >= 1";
  the 120/3000 values were a heuristic chosen with VSEARLY (D-367/D-368), not a measured limit, and no decision records a failure at an
  earlier start. The measured start-path risks it was guarding are covered elsewhere: cold or busy SteamVR at start (startup budget 5 s with
  the first frame included, `startup-deadline`, D-323/D-324), the idle-headset slow first xrEndFrame (D-188, the 250 ms rule), the cold first
  frame (D-313, `first-frame-budget(cold-setup)`), and load stalls of the shared queue (test 19, 1968 ms; the vsearly 8 s frame-stall limit).
  Each of these has a retry (cold-start retry, persistent transient retry 3/5/10/15 s, vsearly `yes-early(after-start)`), and no OpenXR call
  ever runs on the game's Present thread (owner thread + watchdog), so starting earlier cannot stall the game's present.
* **Measured start timeline today (2026-11-01 user session, real SteamVR running):** Launch click 09:04:39.77 -> `armed` +0.4 s ->
  `source-ready;start=early-game-presents;presents=343` +3.03 s (the 3 s term decided; 120 presents came after ~1 s) ->
  `xrCreateInstance` 2178 ms -> first XR frame 2268 ms after the attempt start -> `outcome=hooked` ~5.8 s after the click. The 3 s gate is
  what `vsstart:first` removes; the 2.2 s xrCreateInstance stays (see 4, follow-up).

## 2. VR Mode: On / Off in the launcher

* **Where:** one row below "Display Resolution" in the launcher's left column, beside the Launch Game button, label "VR Mode" painted in the
  gap above it like the launcher's own labels; values On / Off. The layout is computed from the launcher's own combo rectangles
  (`launch_mode.h place()`: same width/height/pitch as the left column; refused if it would overlap another child or leave the window).
* **How:** DllMain (acz.exe only) creates ONE thread. It polls `EnumWindows` every 50 ms for a visible top-level window of this process with
  a "Button" child titled "Launch Game" and >= 2 "ComboBox" children (WM_GETTEXT via `SendMessageTimeout` 200 ms). It then installs a
  `WH_GETMESSAGE` hook on the launcher's UI thread and posts WM_NULL: the injection runs once INSIDE the launcher's own GetMessage (no window
  created from a foreign thread, no AttachThreadInput), and the hook is removed at once. The launcher is subclassed with comctl32
  `SetWindowSubclass` (the mechanism acz.exe itself uses): our combo's notifications are handled and never forwarded; WM_PAINT draws the
  label after the launcher's own paint, only inside the repainted area; the Launch Game click (BN_CLICKED, BM_CLICK, mouse up, Enter/Space)
  is noted; when the launch button is hidden/destroyed or the window changes size, our combo is destroyed and every subclass removed. Our
  closed combo field is painted like the launcher's (white field, thin grey frame, small chevron, the launcher's combo font); the open list
  is the standard list. Nothing ever waits on another thread; the only work on the UI thread is the selection write (a few hundred bytes).
* **Persistence:** `%LOCALAPPDATA%\acz-vr\launch-mode.txt` (`vr=on` / `vr=off` plus comment lines), written on each selection change
  (temp file + MoveFileEx). runtime.cfg is never touched. Missing/unreadable/unrecognised file = On. The combo starts at the file's value.
  Accepted by the reader: `vr=on|off` (also `vr:`, `vr_mode=`, `vrmode=`), a bare on/off/1/0/true/false/yes/no; first non-comment line;
  BOM/CRLF fine.
* **The decision (latch)** happens at the first `CreateDXGIFactory*` or early-device call (`launch_mode.h decide()`): the launcher's
  selection, else the file, else On. On latches at once (a factory made during the launcher is wrapped exactly as before). Off latches only
  once the launcher is gone; a factory call while the launcher still shows Off is passed through unwrapped and decided again at the next
  call (`provisional-pass-through`), so switching back to On before Launch still works. After the latch, a selection change only rewrites
  the file (`applies=next-launch`).
* **Off:** `createWrappedFactory` returns the system export's result untouched: no diagnostics activation (no runtime.cfg read, no
  diagnostics/xr-stereo/ee-camera logs), no callback registration, no factory wrapper, so no swap-chain/device/Present hook, no producer,
  no XR monitor, no OpenXR call. `AczVrNotifyDeviceCreated` (the d3d12.dll proxy's notification) returns 0. The skip-intro XINPUT1_4.dll is
  not VR-specific: its intro skip/speed-up stays; its look-stick gate stays passive (no heartbeat).
* **Fallbacks:** if the combo cannot be added (`launch-mode.log` says why: layout refused, comctl32 subclass missing, hook failed, UI thread
  not pumping in 3 s), or with `SkipLauncher=True`, the file still decides: `pwsh -NoProfile -File tools/acz-vr-mode.ps1 -Mode off|on`
  (no argument = show). `ACZ_VR_LAUNCHER_OPTION=0` turns the combo off (file only). Tools and probes that load the DLL outside acz.exe are
  never gated (always On, no thread).
* **Log:** `%LOCALAPPDATA%\acz-vr\logs\launch-mode.log`, rewritten each game start (works in Off mode too):
  `event=process;file=...`, `event=launcher-found;waited-ms=`, `event=inject;outcome=ok;combo=x,y,wxh;...` or `outcome=refused;why=...`,
  `event=selection;vr=off;file-write=ok;applies=this-launch`, `event=launch-pressed;via=...`, `event=latched;vr=on|off;source=launcher|file|
  default;...;effect=...`, `event=ui-removed;why=...`.

## 3. XR from the first game frame (`vsstart:`)

* New sub-option of `xr_terrain_stage`, used only when `vsearly:1` is effective (no new config key; the file is at 32 keys):
  `vsstart:first` (DEFAULT) = start XR at the first counted game Present after Launch Game (the monitor's next 100 ms poll);
  `vsstart:steady` = the previous gate exactly (>= 120 presents AND >= 3000 ms since `armed`);
  `vsstart:lazy` = no early start, identical to `vsearly:0` (XR at the producer's first StereoFrame, i.e. in flight).
  `ACZ_XR_START=first|steady|lazy` (environment) overrides the token for one launch without editing runtime.cfg. The user's installed value
  (`...;vscreen:1;vsearly:1;...;vrn:1`) gets `first` with no change.
* What the headset shows: SteamVR's own scene during the launcher (no game device exists); after Launch Game, as soon as the first XR frame
  completes (~2.6 s with SteamVR already running: 0.4 s to the device + first Present, then xrCreateInstance ~2.2 s), the flat game image on
  the vscreen quad (logos, intro videos as far as the skip-intro leaves them, title, menus); the first 2-3 XR frames have zero layers until
  the first capture is copied (vsearly.md 2). The full stereo path takes over at the first flight composite exactly as before.
* Safety kept: the startup budget (5 s incl. the first frame), cold-start retry, persistent transient retry and the vsearly early retry
  (`yes-early(after-start)`), the 8 s frame-stall limit after the first frame, the owner thread and watchdog, the eye swapchains sized
  for the assumed 2560x1440 source. No new retry rule was needed: a first attempt that fails during the game's own start-up is retried after
  3 s, when the old gate would have fired anyway.
* Lines: `vsearly-config;effective=1;reason=ok;start=first-game-present(presents>=1);start-mode=first;start-mode-source=default|option|
  env(ACZ_XR_START)|...` (steady: `start=presents>=120-and-armed>=3000ms`; lazy: `effective=0;reason=vsstart:lazy(as-vsearly:0)`),
  `source-ready;...;start=early-game-presents;presents=N;...;start-mode=first`, and `vscreen-option;...;vsearly=1;early=...;vsstart=first`.
  The `armed` line still says `start=lazy-first-published-frame` (it is written before the options are parsed; unchanged for log readers).

## 4. Live risks (inference until a run)

1. **SteamVR still starting at the first Present.** Launching SteamVR and the game together now meets XR ~3 s earlier. A slow
   xrCreateInstance is `startup-deadline` (transient, retried); but if acz's xrCreateInstance cold-starts vrserver itself, step83 saw a
   permanent `compatibility-refusal` (xr-start.md 4.9). Mitigation: start SteamVR first (playbook 2), or `vsstart:steady`.
2. **Game start-up and our XR start-up overlap.** The first XR frame (swapchains, blit pipelines) now runs while the game loads its first
   assets and plays the intro. Expected cost is a few ms of GPU on the shared queue; a cold first frame beyond 5 s is retried
   (`first-frame-budget(cold-setup)`). The XR creation window (kXrWindowCapMs 250 ms) may now overlap the game's own heap creations: a
   rejected registration in that window is tombstoned instead of disabling the observer (counted by `xrWindowTolerated`, the
   `binding-creation;...;rejected-origin-unknown-xr-window` lines); watch for those lines and producer refusals in the first flight.
3. **Intro video presents.** The capture copies the back buffer at each Present; intro videos are presented by the game into the same swap
   chain, so they appear on the screen quad. Not seen yet: whether the skip-intro speed-up (speed=8 until 15 s) and the Enter pulses leave
   much to see.
4. **Launcher variants.** The combo is placed from the launcher's own combo rectangles; another resolution/DPI/language moves it with them.
   A launcher change that destroys and recreates its children without resizing the window would leave our combo until the window grows;
   a launcher that never notifies BN_CLICKED is still covered by the button subclass and the resize/destroy paths.
5. **Steam overlay / other dxgi callers before Launch.** No factory call before the click was seen; if one happens with Off selected it
   is passed through (provisional); with On it latches On as before.
6. **The hook thread.** If the launcher's UI thread did not pump within 3 s the combo is not added (logged) and the file decides.
7. **Off really flat.** Expected identical to no proxies. SteamVR may still be running (the user's choice); the game will not appear
   as a VR app.

## 5. Live check list

Null driver (runK3ag/runBatchAG, the day cfg; no cfg change needed):
1. `launch-mode.log`: `launcher-found` within ~1.5 s, `inject;outcome=ok;combo=...;pitch=60` (2560x1440 launcher), then `launch-pressed;
   via=bn-clicked|button-up`, `latched;vr=on;source=launcher`, `ui-removed;why=launch-button-hidden|launcher-resized`. A launcher screenshot
   (shot before the click) shows "VR Mode / On" under Display Resolution, left of Launch Game, not overlapping "Start the game.".
2. `xr-stereo.log`: `vsearly-config;...;start=first-game-present(presents>=1);start-mode=first`, `source-ready;...;presents=` small (1-10),
   `waited-ms` under ~1000; `start-attempt;n=1;outcome=hooked`; `summary-vscreen;mode=screen;copies>0`; the null mirror shows the
   title/intro on the screen quad. Record click -> hooked (expect ~2.5-3 s, was ~5.8 s).
3. A normal mission afterwards: flight-stage detect/install, terrain and cockpit as before (no producer refusal in the first flight).
4. Off run: select Off, Launch: `latched;vr=off`; the logs folder gets NO new diagnostics/xr-stereo lines (only launch-mode.log);
   SteamVR shows no acz app; the game plays flat. Next launch shows Off pre-selected; set On again (or `tools/acz-vr-mode.ps1 -Mode on`).
5. `vsstart:steady` (or `ACZ_XR_START=steady`) run: `start=presents>=120-and-armed>=3000ms`, as before.

User headset: SteamVR first, then the game. In the launcher the headset shows SteamVR home (expected). Pick VR Mode On, Launch Game: about
2-3 s later the game's intro/title appears on the screen in front of you, menus follow, missions switch to full VR as now. With Off the game
plays on the monitor only. If the combo is missing, send `%LOCALAPPDATA%\acz-vr\logs\launch-mode.log`.

## 6. Offline verification

* `launch_mode_offline` (76 checks): the file format (BOM, CRLF, comments, synonyms, unknown -> On, round trip), the latch decision
  (selection > file > default; Off provisional while the launcher shows; On latches), the layout on the measured launcher rectangles and its
  refusals, the start gate (first fires at the first Present, steady = the old gate at 3 s, lazy never; the producer frame always starts),
  the token/env resolution and the vscreen `vsstart:` token (removed before the other parsers; describe text). Failing before: the headers
  did not exist and `vsstart:` was not a vscreen token.
* `flight_cycle_offline`, `vscreen_offline`, `dxgi_proxy_offline`, `dll_lifecycle_offline`, `diagnostics_activation_offline` pass; the
  dxgi.dll (acz-vr) target builds without warnings.
* Manual harness (fake launcher named acz.exe, LOCALAPPDATA redirected to a scratch folder, off-screen window): default -> combo injected
  at the computed rect, factory wrapped, combo removed at launch, no file written; select Off -> file `vr=off`, factory NOT wrapped, no
  diagnostics logs created; Off then a factory call while the launcher shows -> `provisional-pass-through`, latched Off after the click;
  `ACZ_VR_LAUNCHER_OPTION=0` + file off -> no combo, latched Off from the file; file off + select On -> wrapped, file rewritten `vr=on`.
  The harness is not acz.exe's launcher (standard controls, no custom paint): the real look and the real click path need the live check.

## 7. Follow-up (not done)

* xrCreateInstance takes ~2.2 s even with SteamVR running and is now most of the delay. Creating the instance during the launcher (when
  VR Mode is On) would hide it, but needs an instance that outlives the attempt owner (today every attempt owns and retires its instance),
  and SteamVR would list acz as an app while the launcher is up. Not attempted.

## 8. Proposed DECISIONS entry

D-4xx - LAUNCH-MODE + NATIVE-VR START (docs/world-render/launch-mode.md). User request 2026-11-01: VR from the first frame after Launch Game,
and a VR Mode On/Off choice in the game's launcher. Findings: our dxgi.dll is a static import (loaded at process start) but the first
factory call and the device come ~0.4 s after the Launch click; the launcher is acz.exe's own Win32 window (ComboBox/Button children,
GDI/UxTheme custom paint, comctl32 SetWindowSubclass); the 120-present/3 s vsearly gate was a heuristic (start in the user's session at
+3.0 s, hooked +5.8 s, xrCreateInstance 2.2 s). Change: (1) a "VR Mode: On/Off" combo injected on the launcher's UI thread (WH_GETMESSAGE
hook, SetWindowSubclass), stored in %LOCALAPPDATA%\acz-vr\launch-mode.txt (default On; runtime.cfg untouched); Off = the dxgi proxy forwards
the factory exports without activation/wrapping/hooks and the early-device notification returns 0; fallback tools/acz-vr-mode.ps1,
ACZ_VR_LAUNCHER_OPTION=0; log launch-mode.log. (2) `vsstart:first|steady|lazy` inside xr_terrain_stage (default first with vsearly:1;
ACZ_XR_START overrides): XR starts at the first game Present; steady = the old gate; lazy = vsearly:0. Offline: launch_mode_offline 76/76,
affected suites pass, harness scenarios A-G pass. Not verified live: the real launcher look and click path, the start at the first
Present with SteamVR (null and real), Off mode in the real game.
