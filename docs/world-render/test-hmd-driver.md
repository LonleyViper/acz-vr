# acz_testhmd: a scriptable-pose SteamVR test HMD (2026-11-02)

Status: **built and unit-tested offline only.** Not registered with SteamVR, never loaded by vrserver, never
used in a live run. Everything under "Untested" below is open until the first live null-style run.

## 1. Why

Unattended "null HMD" flights use SteamVR's stock null driver (`SteamVR\drivers\null`, switched on per run by
`research/experiments/step83-verify-run1/null-vr.ps1`, called from `runBatchAG.ps1` line ~664). Its head pose is
fixed, so the only way to "look around" was the game's own look keys (numpad 4/6/8 in `autopilot.ps1`). Those
turn the GAME camera and the forward window with it, so they never test what the player sees outside the
forward window when the HEAD turns (cockpit sides, world-render periphery, HUD/marker layers, boxes, clouds).
`acz_testhmd` is a drop-in replacement for the null driver whose head orientation (and optionally position) is
scripted from a text file, so a run can turn the VR point of view while the game camera stays put.

## 2. What was built

| Item | Where |
| --- | --- |
| Driver source (C++17, one TU + header-only pose core) | `tools/testhmd/src/driver.cpp`, `tools/testhmd/src/pose_core.h` |
| Unit test (parser + slew maths, 50 checks) | `tools/testhmd/test/pose_core_test.cpp` -> `testhmd_test.exe`, ctest `testhmd_pose_core` |
| CMake (MSVC x64, Ninja; no openvr_api.lib, static CRT) | `tools/testhmd/CMakeLists.txt` |
| Driver resources (manifest, default.vrsettings, input profile) | `tools/testhmd/driver/acz_testhmd/` |
| Helper | `tools/testhmd/testhmd.ps1` |
| Third-party note (OpenVR SDK v2.5.1, BSD-3) | `tools/testhmd/THIRD_PARTY.txt` |
| Build dir | `E:\acz-testhmd\build` |
| Ready (unregistered) driver folder | `E:\acz-testhmd\driver\acz_testhmd` (driver.vrdrivermanifest, bin\win64\driver_acz_testhmd.dll, resources\...) |

Build:

```
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S tools/testhmd -B E:\acz-testhmd\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build E:\acz-testhmd\build -j2
ctest --test-dir E:\acz-testhmd\build --output-on-failure
```

The DLL exports only `HmdDriverFactory` and imports only KERNEL32 (checked with dumpbin). The SDK header is
read from `OPENVR_SDK_DIR` (default `E:/acz-deps/openvr-2.5.1`), not vendored.

## 3. Display: same as the null driver

Structure follows the SDK sample `samples/drivers/drivers/simplehmd` (an `IVRDisplayComponent`, i.e. an
extended/desktop-window HMD like the null driver, not a direct-mode or virtual-display driver):

* `IsDisplayOnDesktop()=true`, `IsDisplayRealDisplay()=false`, `Prop_DisplayDebugMode_Bool=true`: the compositor
  opens its own desktop window (the null driver's "Headset Window" that `mirror-shot.ps1` captures; the title
  under this driver is UNVERIFIED, see 8).
* Render target 1512x1680 per eye, 90 Hz, `secondsFromVsyncToPhotons` 0.01111111 (null default.vrsettings).
* Window: driver default 2160x1200 at 0,0 (null default.vrsettings); `testhmd.ps1 enable` writes 1280x720 at
  0,0, the same values `null-vr.ps1` writes for `driver_null`, so mirror captures stay comparable.
* Projection raw tan -1/+1 on both axes (90x90 deg per eye), the same FOV our null logs report
  (`eye0-fov=-45.00,45.00,45.00,-45.00`, docs/world-render/cockpit-render-impl.md). Identity distortion.
* IPD: SteamVR's `steamvr.ipd` setting, as the sample does.

Settings section `driver_acz_testhmd` (defaults in `resources/settings/default.vrsettings`): `enable` (false by
default, like `driver_null`), `loadPriority` -999, `serialNumber`, `modelNumber`, `windowX/Y/Width/Height`,
`renderWidth/Height`, `secondsFromVsyncToPhotons`, `displayFrequency`, plus `defaultSlewDegPerSec` (120),
`positionSlewMetresPerSec` (1.0), `applyHeadModel` (false: pure rotation about the eye centre),
`angularVelocityFrame` ("world" or "local", see 8), `poseFile` (empty = the default path below) and `localRollFix` (default true since the
first live flights: the game's LOCAL space is the driver space rolled 180 deg, which mirrored every yaw and pitch; the driver now reports the
pose so the pose file is the look in LOCAL. Details: docs/world-render/testhmd-first-flights.md section 3).

## 4. Pose file

Path: `%LOCALAPPDATA%\acz-vr\testhmd-pose.txt` (setting `poseFile` overrides). The driver's pose thread
(4 ms ticks, i.e. ~250 pose updates/s to vrserver) re-reads it every 50 ms with full sharing.

First non-empty, non-comment line (`#` starts a comment; separators blanks, tabs or commas):

```
yaw pitch roll                 # 3 numbers
yaw pitch roll slew            # 4 numbers
yaw pitch roll x y z           # 6 numbers
yaw pitch roll x y z slew      # 7 numbers
```

* Degrees; position in metres; slew in deg/s (default 120; `0` snaps instantly).
* **Sign convention (SteamVR tracking space: +X right, +Y up, -Z forward):** `+yaw` = head turns LEFT (about +Y),
  `+pitch` = looks UP (about +X), `+roll` = tilts LEFT (about +Z). q = Ry(yaw)*Rx(pitch)*Rz(roll). So "look left
  90" is `90 0 0`, "look right 90" is `-90 0 0`. The unit test checks each sign.
* Absent file = identity (until the first valid line). Missing, garbled, wrong field count, NaN/inf, angle
  > 3600 or |position| > 10 m = keep the last target (one log line per new rejection reason).
* Without x y z the last target position is kept. Default position is 0,0,0 (the null driver's own default
  head position is not known; LOCAL space is what our XR session uses, see 8).
* Slew: the orientation turns along the shortest arc at the constant rate; a 90 deg turn at 120 deg/s takes
  0.75 s. During the turn the pose reports the angular velocity (|w| = slew rate), zero at rest; position moves
  at `positionSlewMetresPerSec` with matching linear velocity. Always `poseIsValid`, `deviceIsConnected`,
  `TrackingResult_Running_OK`.
* Write it atomically (temp file + rename): `testhmd.ps1 pose` does this.

Log: `%LOCALAPPDATA%\acz-vr\testhmd.log`, one line per target change (`target yaw=... pitch=... slew=...`), the
current pose every 5 s (`pose yaw=... remaining=... deg w=...`), init/activate/rejection lines; rotated to
`testhmd.log.1` above 1 MiB (so at most ~2 MiB). Shared access, so samplers never block it.

## 5. Helper `tools/testhmd/testhmd.ps1`

```
pwsh -NoProfile -File tools/testhmd/testhmd.ps1 install            # copy to E:\acz-testhmd\installed\acz_testhmd, PRINT the vrpathreg change
pwsh -NoProfile -File tools/testhmd/testhmd.ps1 install -Apply     # ... and register it (vrpathreg adddriver)
pwsh -NoProfile -File tools/testhmd/testhmd.ps1 enable  -E <rundir> [-NoDashboard] [-Apply]
pwsh -NoProfile -File tools/testhmd/testhmd.ps1 disable -E <rundir> [-Apply]
pwsh -NoProfile -Command "& 'tools/testhmd/testhmd.ps1' pose -Yaw -90 -Slew 120"
pwsh -NoProfile -File tools/testhmd/testhmd.ps1 status
```

`enable`/`disable` mirror `null-vr.ps1` on/off exactly (stop vr* processes, byte backup
`<rundir>\steamvr.vrsettings.pre-run-backup` + `vrsettings-prerun.sha`, refuse if `forcedDriver` is already set,
restore and compare hashes). Without `-Apply` they only print. `pose` needs no `-Apply` (it only writes the pose
file). As of this commit nothing was run with `-Apply`: not registered, vrsettings untouched (hash D5E9E51E).

## 6. Exact SteamVR changes needed to switch a run from driver_null to acz_testhmd

One-time (persistent; needs the user's go-ahead because it is a standing SteamVR configuration change):

```
"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver "E:\acz-testhmd\installed\acz_testhmd"
```

This adds the folder to `external_drivers` in `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`. Harmless for the
user's headset sessions because `driver_acz_testhmd.enable` defaults to false (as `driver_null` does); undo with
`vrpathreg removedriver <same path>` (`testhmd.ps1 uninstall -Apply`).

Per run, in `steamvr.vrsettings` (written by `testhmd.ps1 enable -Apply`, restored byte for byte by `disable`):

| Key | null run (null-vr.ps1) | acz_testhmd run |
| --- | --- | --- |
| `steamvr.forcedDriver` | `"null"` | `"acz_testhmd"` |
| `steamvr.showMirrorView` | false | false |
| `driver_null` block | `{enable:true, 1280x720 window, 1512x1680, 90 Hz, ...}` | not written |
| `driver_acz_testhmd` block | - | `{enable:true, windowX:0, windowY:0, windowWidth:1280, windowHeight:720, renderWidth:1512, renderHeight:1680, displayFrequency:90}` |
| `dashboard.enableDashboard` (-NoDashboard) | false | false |

`steamvr.activateMultipleDrivers` is NOT needed: with `forcedDriver` SteamVR loads only that HMD driver (same
as the null runs, which never set it). Only if SteamVR still picks another driver would we add
`activateMultipleDrivers=false` / `requireHmd=true`; not expected. No driver signature is needed: the `.sig`
files next to `driver_null.dll` are Valve's own; third-party drivers load unsigned.

## 7. Integration plan for runBatchAG.ps1 / autopilot.ps1 (NOT done here: another agent owns those files)

### 7.1 runBatchAG.ps1

1. New parameter `-Hmd null|testhmd` (default `null`, so existing batches are unchanged).
2. Preflight (where it already prints helper presence, ~line 622): when `testhmd`, run `testhmd.ps1 status` and
   refuse unless the installed DLL hash equals the built one and `registered True`.
3. Line ~664: replace `null-vr.ps1 -Mode on -E $E -NoDashboard` with
   `testhmd.ps1 enable -E $E -NoDashboard -Apply`. The backup file name is the same, so the existing restore at
   ~line 703 (`Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset`) and the end-of-batch "vrsettings
   identical" check keep working unchanged; the off path can call `testhmd.ps1 disable -E $E -Apply` or stay as is.
4. Before the launch write the identity pose (`pose -Yaw 0 -Pitch 0 -Roll 0 -Slew 0`); after the run copy
   `%LOCALAPPDATA%\acz-vr\testhmd.log` and the pose file into `$E\logs`, then delete the pose file so the next
   run starts at identity.
5. Analysis: add a check that `testhmd.log` has `testhmd: activated` and one `target` line per scripted look;
   pair each `target` timestamp with the nearest eye dump / mirror frame (the existing `Near-Mirror` helper).
6. Window title: confirm the compositor window title under this driver before relying on `mirror-shot.ps1`
   (`vrcompositor 'Headset Window'`).

### 7.2 autopilot.ps1 look phases -> pose-file writes

Add `-HeadPose` (switch). When set, the three look phases write the pose file instead of tapping numpad keys
(an in-process atomic write, the same temp+rename as `testhmd.ps1 pose`; no pwsh per tap) and log
`HEAD yaw=.. pitch=..` lines:

| Phase (autopilot.ps1) | Today (game camera) | With -HeadPose (head only) |
| --- | --- | --- |
| `-LookEvery N` (line ~630): alternately `lookleft`/`lookright` held `-LookHold` s | numpad 4 / numpad 6 | `90 0 0` (left) / `-90 0 0` (right), back to `0 0 0` after `-LookHold` s. At 120 deg/s the turn takes 0.75 s, so keep `-LookHold` >= 3 (default 3) and capture >= 1 s after the target line |
| `-LookAtContacts` (line ~651): contact bearing `b` (deg, + = right of the nose) | numpad 6 if b > 0 else numpad 4 | yaw = `-b` (SteamVR +yaw is LEFT), pitch = contact elevation `e` (deg; CHECK its sign on the first run), clamp |yaw| <= 135, back to `0 0 0` after `-LookHold` |
| `-SunLookUp` (line ~629): numpad 8 held while aligned on the sun | numpad 8 | pitch = sun elevation relative to the nose, `($SunElevE - p)` converted rad->deg (as logged in `sunRelEl`), yaw = `-sunRelBrg` (sign to confirm) |

Because the head pose does not move the game camera, the "hands-off" gates that exist only because the camera
look corrupts the attitude read (`lookHandsOff` at line ~646, `luHands` at line ~627) are unnecessary in
`-HeadPose` mode: the autopilot can keep flying while the head is turned. Keep them for the key path.

New scenarios this enables (write them into the tests, playbook section 8): head sweep left/right/up while the
nose stays on course (cockpit side panels, periphery seams, boxes/markers off-axis), small position offsets
(`0 0 0 0.05 0 0`: lean right 5 cm) for parallax checks, and a slow continuous yaw (slew 30) for smear/judder.

## 8. Risks and open questions

* **Untested in SteamVR.** The driver has never been loaded by vrserver. Possible first-run failures: the
  compositor refusing the display component, a "Room Setup"/"Headset not tracking" popup (the null route already
  has a Room Setup popup that runK3ag closes), the window title differing from 'Headset Window'.
* **Angular-velocity frame.** OpenVR's header does not state the frame of `vecAngularVelocity`; the driver
  reports the world (tracking) frame by default, `angularVelocityFrame: "local"` switches to the head frame. It
  only matters DURING a slew (prediction ~11-30 ms ahead): at rest it is zero. If turns look wobbly mid-slew,
  try "local" or use slew 0 (snap) for captures.
* **Head model.** `shouldApplyHeadModel` is false (pure rotation about the eye centre). A real headset rotates
  about the neck; set `applyHeadModel: true` to emulate it.
* **Recentre / LOCAL space.** Our session uses `XR_REFERENCE_SPACE_TYPE_LOCAL`. If SteamVR's seated zero pose
  was calibrated with another driver, LOCAL may carry an offset; a yaw-only offset would shift every look by
  the same angle. Check the first run's identity pose against a null run's eye dump.
* **Driver selection.** `forcedDriver` should be enough; if vrserver still loads `null` or the user's headset
  driver, see section 6. `driver_acz_testhmd.enable` must be true in the run's vrsettings (the default is false
  so a registered driver never takes over the user's headset sessions).
* **Registration is persistent** (openvrpaths.vrpath). It survives runs; needs the user's go-ahead.
* **No vsync events.** Like the sample, the driver sends no `VsyncEvent`; the compositor paces its own window.
  If frame pacing differs from the null driver (the null runtime already degrades to ~32 Hz at times), compare
  the `summary-decouple` composites-per-s between a null and a testhmd run.

## 9. Untested until a live run

vrserver loading the DLL; the display window, its title and capture by mirror-shot.ps1; our XR session on this
HMD (FOV/eye fov log lines identical to null); pose updates reaching the OpenXR view pose; the sign convention
end to end (a `90 0 0` pose must show the LEFT side of the cockpit); slew smoothness and the angular-velocity
frame; frame pacing; the log written from inside vrserver (LOCALAPPDATA in that process); SteamVR popups.
