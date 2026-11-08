# SEATED RECENTER: line the cockpit eye point up with the pilot's head without taking the headset off

Status: implemented, offline-tested (`recenter_offline_tests`, `xr_owner_offline_tests` software_recenter), `acz-vr` target compiles. NOT run live.
Tags: **[E]** read from code, **[I]** inference, **[U]** unverified (needs the headset).

## 1. How the head pose is referenced today [E]
* The live session (`xr_owner_openxr.cpp`) creates ONE `XR_REFERENCE_SPACE_TYPE_LOCAL` space (plus VIEW); no STAGE space is created. Eye poses are located in LOCAL
  and published raw (head and IPD included) in `PoseSnapshot`, tagged with `space_generation`. (`openxr_session.cpp` with its own `recenter()` is the older
  stand-alone session class, not the live path.)
* The seated neutral lives in the producer (`gpu_copy_step5.h` `Neutral`): the FIRST valid pose of a LOCAL generation fixes `p0` = eye midpoint and
  `yaw0` = heading of the left eye's forward (`atan2(-fx,-fz)`); every eye is then `Ry(-yaw0)(p - p0)` and `Ry(-yaw0) R`. Pitch/roll and gravity alignment are kept
  (heading only). The neutral is reset whenever the generation changes (`setup_poses_generation`); the frame carries it (`neutral_valid/pos/yaw`) and the
  consumer re-anchors the world quads/HUD/cockpit on it (`Output::anchor`); frames rendered under another generation are released unused.
* So before this change the neutral was captured ONCE per session at the first valid pose (with `vsearly:1` that is in the menus) and never again except on a
  runtime reference-space change. A user who sat differently when the flight started had the cockpit eye offset by that difference for the whole session.
* **SteamVR "reset seated position"**: the owner handles `XrEventDataReferenceSpaceChangePending` for LOCAL (`xr_owner.cpp`, honouring `changeTime`): a new
  generation, so the producer re-captures the neutral from the first pose after the reset and the consumer re-anchors. So it should already work [E code];
  whether SteamVR sends that event for its reset is [U]. The recenter below does not depend on it. A reset the runtime performs is logged as
  `reason=runtime-space-change`.

## 2. Design: a recenter is a new LOCAL generation (instant)
`Owner::request_recenter()` (any thread, one atomic increment) makes the owner thread bump `space_gen_` at its next sample (`generation_at`), exactly like a
runtime change without a `changeTime`: the XR space and the raw poses are untouched; the producer sees `n.generation != generation`, drops the neutral and
re-captures `p0`/`yaw0` from the next pose (head position AND heading, so the eye sits at the cockpit eye point facing the nose; pitch/roll stay the head's own);
the consumer re-anchors and releases pre-recenter frames. All consumers already honour a generation change, so nothing else was touched. The neutral then
persists for the session (until the next recenter or runtime change). A burst of requests between two samples is one recenter; a pending runtime change keeps
its `changeTime` and follows the recenter generation. The jump is instant (a hitch of 1-2 frames while pre-recenter frames are released [I]); no smoothing
is offered: the cockpit is rigid to the seat and a gradual slide of the whole cabin is a comfort problem of its own.

## 3. Triggers
* **Key** (default Home, `rckey:`): `GetAsyncKeyState` polled every 20 ms on the XR attempt thread (`RecenterPoller`, `xr_stereo_proxy.cpp`; the former
  `Sleep(100)` is now five 20 ms slices, the 100 ms work is unchanged). Nothing is hooked or sent to the game. Debounce (`recenter.h` `Debounce`): fires on a
  press edge, only after the key was seen up, never repeats on hold, never closer than 1 s to the previous one. Counted only while the game window is the
  foreground window (`rcfocus:1`). Home was chosen because the game/driver scripts use Esc, V, S and NumPad 4/6/8/2 (look) and F12 is Steam's screenshot key;
  the option refuses Esc, Enter, Space, arrows, digits, letters, the numpad, modifiers and mouse buttons. (NumLock off turns NumPad 7 into Home; 7 is not a game key.)
* **Controller chord: NOT implemented.** Any chord reaches the game through the XINPUT1_4.dll proxy unchanged (Back+Start would pause it); doing it safely
  needs the proxy to mask the chord's buttons while it is held (a skip-intro DLL change plus IPC), which is not cheap or safe enough for this round.
* **Auto at the first flight sample** (`rcauto:<cm>`, default OFF): on the rising edge of "flight composites are moving", once per flight start, recenter when the
  head is farther than `<cm>` from the neutral eye point (3-D, cockpit frame) or its heading differs by more than `rcyaw:<deg>` (default 30). The controller
  mirrors the producer's neutral from the same pose snapshots (same formula, tested equal to `setup_poses`) so it can measure the offset [I: the mirror may differ from
  the producer's neutral by the few cm the head moved between the two captures; it only drives the threshold and the log].

## 4. Options (inside `xr_terrain_stage`, no new config key; tokens are removed before the other parsers; defaults need no token)
`rc:0|1` (1) | `rckey:home|end|insert|delete|pageup|pagedown|pause|scrolllock|f1..f24|0xNN` (home) | `rcauto:<1..200 cm>` (0 = off) | `rcyaw:<5..180>` (30) | `rcfocus:0|1` (1).
A bad value is logged (`recenter-config;ok=0;detail=...`) and the defaults apply; the world is never refused for it. Without the terrain option at all the defaults
(on, Home) still apply. Mind the 255-character option buffer.

## 5. Log lines (xr-stereo.log)
* `event=recenter-config;...` (only when a token is present), `event=recenter-armed;recenter=1;key=0x24;auto-cm=0;...;controller-chord=not-implemented` once per attempt.
* `event=recenter;stage=request;reason=key|auto-flight;gen=G;before-offset-cm=x,y,z;before-yaw-deg=D;mode=instant(new-LOCAL-generation)`
* `event=recenter;stage=applied;reason=key|auto-flight|runtime-space-change;gen=G+1;n=N;before-offset-cm=..;before-yaw-deg=..;after-offset-cm=0.0,0.0,0.0;after-yaw-deg=0.0;neutral-shift-cm=x,y,z;neutral-yaw-deg=old->new`
  (offsets in the cockpit frame: +x right, +y up, -z nose; `after` is zero by construction at the capture sample). Capped at 60 lines per session.

## 6. Live check list (headset, [U])
1. Sit slumped/leaned, start a flight; the cockpit and HUD are offset. Sit upright facing the nose, tap Home: one `stage=request` and one `stage=applied` line,
   `before-offset-cm` is the lean, the view snaps so the eye is at the cockpit eye point and the cockpit/HUD are centred in front of you.
2. Turn the head 40 degrees and tap Home: the cockpit forward becomes the new look direction (yaw only: pitch/roll untouched).
3. Hold Home: exactly one recenter. Tap twice within a second: one. Press Home with another window focused (`rcfocus:1`): nothing.
4. Menus (vsearly): Home recenters the menu anchors the same way. SteamVR reset seated position: expect `reason=runtime-space-change` if the runtime emits the event.
5. `rcauto:25` with the head far off at the flight start: one `reason=auto-flight` request at the first flight sample; with the head near the eye point: none.
6. Any anomaly in the first 1-2 frames after a recenter (flash/black frame) is the released pre-recenter frames [I]; report it.

## 7. Tests [E offline]
`recenter_offline_tests` (215 checks): option parsing (day value byte-identical, last wins, first item never an option, every bad value refused, forbidden keys), pose
math (heading equals yaw for any pitch/roll, head centre/heading identical to `step5::Neutral`, invalid poses refused, offset zero after a recenter, offsets in the
cockpit frame for several yaws, yaw-only), `Debounce` (hold, edge, bounce, interval), `Controller` (request/applied with before/after/shift, no second request while
pending, pending timeout, min interval, no pose = no action, `rc:0`, runtime space change, auto off by default, auto far/near/yaw, once per flight start, waits for a pose).
`xr_owner_offline_tests` `software_recenter`: generation 0 to 2 monotonic, burst coalesced, ordering with a pending runtime change.
