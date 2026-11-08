# NULL-HEAD: synthetic head turns for null-HMD test flights (TEST ONLY)

Status 2026-11-02: implemented and offline-tested (`null_head_offline`); the dxgi.dll compiles. **Not yet run live.** Nothing here is evidence
of headset behaviour.

## 1. Why

The user, watching our null-HMD flights on the mirror: the way we "look around" moved the forward rectangle too. The batch drivers pressed the
GAME's look keys (numpad 4/6/8), which rotate the game camera and with it the forward window. That is not the real use case: in the headset the
game camera stays forward and the player's HEAD turns, so everything outside the forward window (360 boxes, cloud/smoke cache, world render,
briefing table, cockpit) must come from our own rendering. SteamVR's null driver has a fixed HMD pose, so no null run ever turned the head.

## 2. Option and trigger

Inside the existing key `xr_terrain_stage` (no new config key; removed before the terrain parser, like `rc:`). Default OFF:

| token | meaning |
|---|---|
| `nh:1` | enable (the head stays forward until a head.now command: nothing changes without one) |
| `nhs:<deg/s>` | slew speed, default 90, range 10..720 |

A bad value is logged (`event=null-head-config;ok=0`) and the feature stays off; the world is never refused for it.

Run-time trigger `%LOCALAPPDATA%\acz-vr\head.now` (ASCII, polled by the XR attempt thread at most every 250 ms, deleted when read):

    <yaw_deg> <pitch_deg> [<hold_s> [<roll_deg>]]

- yaw + = LEFT (OpenXR +Y; the same sign as `recenter.h` / `look_drive_keys.h` head yaw), pitch + = UP, roll + = left ear down.
- |yaw| <= 180 (180 = behind), |pitch| <= 89, |roll| <= 180, hold 0..600 s. Separators: spaces, tabs, commas.
- The head slews at `nhs:` to the target (largest axis sets the time, all axes arrive together), holds `hold_s` counted from ARRIVAL
  (0 = until the next command), then slews back to `0 0`. `0 0` returns at once. The latest command wins.

## 3. Where the pose is applied

`xr_owner::PoseFilter` (new seam in `xr_owner.h`, owner thread only): in `Owner::frames()` right after `locate_views` / `locate_head` and BEFORE
the pose snapshot is published and BEFORE the consumer's `frame()`. The eyes and the VIEW-space head are rotated about the head point
(`R = Ry(yaw) Rx(pitch) Rz(roll)`, left-multiplied in LOCAL; IPD and FOV kept). Therefore one consistent turned head reaches:

- the producer (`try_read_pose`: eye matrices, culling, world objects, 360 boxes, cloud/smoke cache, lens flare, terrain),
- the consumer (`FrameInfo::views`: projection views, world/HUD/marker quads, brief3d / UI3D / vscreen placement, head-locked fallbacks),
- the submitted layer poses (the projection views are the views the frame was rendered with), and
- the recenter poller (it reads the same snapshot).

The compositor reprojects with the submitted pose. With the null HMD the real pose is fixed, so the mirror shows the turned view.
The runtime's own samples are never modified; identity angles leave the poses byte-identical.

**Neutral rule.** The producer captures its seated neutral (`gpu_copy_step5.h` `Neutral`) from the first pose of each LOCAL generation it sees.
The rotation is therefore forced to identity for 1 s after every session start and every LOCAL generation change (recenter, runtime space
change). Issue head commands only once the flight's world is showing (the drivers do).

## 4. Still unrotated (by design or not covered)

- Layers submitted in the runtime's VIEW space (`hud_mode=1` head-locked HUD fallback) follow the REAL (fixed) head: they stay centred, which
  is what a turned head sees for a head-locked layer.
- The game camera, the game's own forward image and the E6/ee-camera reads are untouched (that is the point).
- `xr_look_drive_keys` (parked, D-291) would turn a synthetic head into game look keys exactly like a real one: keep it off for these tests.
- Anything that reads poses outside the owner snapshot (none known at the time of writing).

## 5. Logs

- `event=null-head-config;ok=..;null-head=1;slew-dps=..` once at start.
- `event=null-head;stage=command|refused;...;trigger=head.now(deleted)` per head.now file.
- `event=null-head;stage=settle|slew-start|reached|returning|home;cur=..;target=..;phase=..` per change (owner thread, capped 400).
- `event=summary-null-head;cur=..;target=..;phase=..;settling=..;commands=..;reached=..;frames=..;turned-frames=..;rotated-frames=..;gen=..`
  every 5 s while enabled, also with zero counters.

## 6. Drivers

- `autopilot.ps1 -HeadLook` (with `-LookEvery`, `-LookAtContacts`, `-SunLookUp`): writes head.now instead of pressing numpad 4/6/8.
  Look-left = `+HeadYaw` (90), look-right = `-HeadYaw`; look-at-contacts = yaw `-bearing` (mk360 bearing + = right), pitch = contact elevation
  minus the nose pitch (clamped -45..60); look-up = pitch `+HeadUpPitch` (60) for `-LookSec`. No hands-off (the attitude read is not
  corrupted). Logged as `HEADLOOK` / `HEADLOOKC ... hold <window>` (the window = slew out + hold + slew back, so cloudtrig's look gate still
  matches). One `png-eye.now` per head turn 0.4 s after arrival (`HEADSHOT`; `-NoHeadShots` to skip; needs `xr_png_once`). The head is sent
  home (`0 0`) when the autopilot ends. The old keys remain the default of autopilot.ps1.
- `runBatchAG.ps1`: boxes (AGB1/AGB2) and clouds (AGC1/AGC3) now default to `-HeadLook` with `;nh:1` and `xr_png_once` (-Png); `-KeyLook`
  restores the numpad keys. Analyze-Common reports `NULL-HEAD head turns` (commands, refused, reached, HEADSHOT dumps).

## 7. Live check list (not done yet)

1. `event=null-head-config;ok=1;null-head=1` and a `summary-null-head` line every 5 s.
2. Each HEADLOOK line is followed by `stage=command` and `stage=reached` within slew + 1 s; `refused` = 0.
3. HEADSHOT eye dumps (`upright\*-final.png`): the forward window is OFF-centre or out of view; the turned view shows our own world, boxes,
   clouds and cockpit, not the game camera. The mirror at the HEADLOOK times shows the same.
4. After `home` the forward window is centred again and the producer's neutral did not move (no recenter line).
