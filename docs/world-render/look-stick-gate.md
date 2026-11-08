# LOOK-STICK GATE: the game's right-stick look must not turn the VR world (D-355 problem 3)

Status: implemented and covered by offline fixtures only (`look_stick_gate_offline`). Not built by the author (the lead builds), not run live.
Tags: **[E]** read from code / archived records, **[I]** inference, **[U]** unverified (needs the game).

## 1. Symptom
Headset test 18 (D-355, step86 day install): touching the right stick / look control on the controller rotated the WHOLE VR view (world, plane)
"pretty crazy". The stick-view/HMD look mapping is parked (D-291), so nothing of ours drives the look on purpose; the user's own stick did.

## 2. Root cause [E]
* What the right stick does in the game: cockpit look-around. E3 (research/experiments/E3-2026-10-12.md): holding the look control turns the
  DrawCPT cockpit camera ~62 deg in yaw about a nearly fixed eye; E4 measured a full look-left of ~145 deg; pausing snaps it back. Test 12 (D-288)
  measured the right stick driving that look (deadzone ~19 %, first-order lag tau ~0.1 s). The XInput look-drive (D-287) worked through it.
* Our world camera carries that look. `Output::frame` (xr_stereo_output.cpp, WORLD-1/CAM-EPOCH) draws terrain, props, clouds, sky and the stage
  lighting from **the frame's game camera** (`game_cam_right/up/fwd`, the DrawCPT camera = aircraft attitude x look) in mode `correlated-hybrid`
  / `game-camera-only`, else from the E6 Euler `rot`, which also includes the look (e6-anchor-free.md section 2: "rot pitch/yaw include the look").
* The model cockpit is drawn in the seat frame, `Ry(-yaw0) * head`, "never the lean or the game look" (COCKPIT-2 comment). So a look turn rotates
  the world around a fixed, head-tracked cockpit by up to ~145 deg, on top of the head pose: the world turns with the stick.
* The game's own images (replayed objects, HUD markers, effects) are drawn by the game along the look as well, so they turn with it too.

## 3. Options considered
* (b) Compensate in our camera (remove the look from the game camera): **rejected as the primary fix**. The E6 look fields are not an exact
  factor of the camera: |wrap(rot yaw - heading - look yaw)| reaches 0.372 rad (21 deg) during look ramps (e6-anchor-free.md section 2), the
  composition order with roll is not established, and the game's replayed objects/markers/effects images would still be drawn along the look,
  so de-looking the terrain alone would mis-register them against the world by the look angle.
* (a) Filter the input: the user's XINPUT1_4.dll (src/skip-intro) is already in the game's XInput path (D-270; MORNING-2026-10-27: Player 1 =
  controller) and already edits the right stick for the parked look drive. Centring the right stick for the game while we fly in VR keeps the
  game camera at the aircraft's own attitude, so terrain, cockpit and the game's images stay consistent. **Chosen.** No game memory, no code patch.

## 4. The fix
* acz-vr (`xr_stereo_output.cpp`): `flight_composites()` counts composites whose terrain was drawn from the game/E6 camera (the fixed test camera
  `cam:` does not count). Menus, the hangar and frames without an admitted camera do not count.
* acz-vr glue (`xr_stereo_proxy.cpp`, the XR attempt loop, every ~100 ms): `LookStickHeartbeat` publishes `block` with a fresh stamp into the named
  mapping `Local\acz-vr-xr-view-v2-<pid>` (`src/skip-intro/xr_view_ipc.h`, 32 bytes, seqlock + check word + writer process id) while the count moves in a hooked session;
  when it stops it publishes an explicit `pass`; at session end `pass`. Mapping created lazily on the first in-flight tick.
* skip-intro proxy (`skip_intro.cpp`, `gate_apply`): on XInputGetState / XInputGetStateEx for user 0 with a connected pad, a fresh `block`
  heartbeat sets `sThumbRX = sThumbRY = 0`; `dwPacketNumber` is bumped when the gate opens/closes with the stick deflected. Runs before the
  (parked) look-drive injection. Buttons, triggers, the left stick and other users are never touched. Absent / zeroed / bad / torn / inactive /
  stale (> 500 ms) / future-stamped / foreign-owner heartbeat = the real stick passes (fail open: flat play and a crashed acz-vr keep the look).
* Process binding (D-358, Astra review finding 3): `Local\` is the whole Windows session, so a fixed name let one game process's heartbeat
  centre another process's stick and allowed two writers. Both DLLs run inside the same game process, so both derive the name from
  `GetCurrentProcessId()`; the block carries the writer's process id (v2 layout, in the former pad word, covered by the check word) and the
  reader requires its own id (`foreign-owner` otherwise). Adoption (NARROWED after Astra's D-359 review): if the per-process mapping already exists, `can_adopt`
  accepts any same-process sample for which `read_block().block` is false (unwritten, fresh INACTIVE, stale, torn, bad checksum or an excessive future
  timestamp) and refuses only a stable, fresh, ACTIVE sample or another process's block (then `mapping=exists-not-adopted`, fail open). The
  check/create sequence is NOT an atomic exclusive-ownership claim and this is NOT instance-generation protection: it relies on the supported wiring
  arming once per process, pinning the module and running one heartbeat on its attempt thread. If duplicate instances or a DLL reload are ever supported,
  an exclusive ownership protocol is required first. A reused process id cannot inherit a live heartbeat: a dead writer's stamp is stale.
  Not a security boundary: another process that deliberately writes this process's name and id is not defended against.

## 5. Switches
* acz-vr: option inside the existing key `xr_terrain_stage` (no new key; 31 of 32 stay): `lookstick:block` (DEFAULT) | `lookstick:game` (restore:
  nothing is published). Removed before the other parsers (`look:1`, the WORLD-4 shading option, is a different option and untouched). A bad value
  refuses the terrain (`terrain-disabled;reason=bad-stage-option`). Log: `xr-stereo;event=look-stick-config;lookstick=...` once,
  `xr-stereo;event=look-stick-gate;state=block|pass;mapping=...` per transition (cap 40).
* skip-intro.ini: `xr_block_look_stick=1` (default; 0 = never gate), `xr_block_look_stick_stale_ms=500` (100..2000). skip-intro.log:
  `look-stick-gate: ON ...`, `acz-vr heartbeat block opened`, transitions `in-flight` / `inactive` / `stale` / `absent` / `foreign-owner` (cap 200).
* `tools/live/vr-test-start.ps1 -TerrainRender '...;lookstick:game'` validates the value.

## 6. Risks [I]
* If the user's control layout uses the right stick for something other than look in flight (some presets put throttle/rudder there), that is
  centred too in VR flight. The D-355 report and test 12 say the right stick is look in this setup. Restore with `lookstick:game` or ini `=0`.
* A right-stick use in the in-flight pause menu or the expanded map (if any) is centred while the composite runs. Button actions (view switch,
  map toggle, R3) are not affected (only the axes are edited) [E code].
* Keyboard look (NumPad 4/6/8, the keyboard route) is NOT filtered: with Player 1 = keyboard the look keys still turn the world.
* The DLL must be rebuilt from `src/skip-intro` (own CMake project, not in the main build) and copied over the game folder's XINPUT1_4.dll; with
  the OLD proxy installed the heartbeat has no reader and nothing changes.
* The look decays to centre with the game's own filter (tau ~0.1 s) once the stick is centred; a look held at the moment the gate engages
  returns to centre within ~0.3 s.

## 7. Live check (headset or null driver with a controller)
1. Install the rebuilt XINPUT1_4.dll (keep the old one), the acz-vr DLLs; day config unchanged (default `lookstick:block`).
2. skip-intro.log: `look-stick-gate: ON stale=500ms`; after entering flight `acz-vr heartbeat block opened` and `look-stick-gate: in-flight`.
   xr-stereo.log: `look-stick-config;lookstick=block`, `look-stick-gate;state=block;mapping=ok`.
3. In flight push the right stick fully left/right/up: the VR world must not turn (`deflected polls centred` rising in skip-intro.log).
4. Buttons still work: view switch (cockpit/HUD/chase), map toggle, weapons, R3/L3 actions, throttle/yaw on triggers/shoulders.
5. Menus / hangar / aircraft select (no flight composite): the right stick still works (`look-stick-gate: inactive` or `absent`).
6. Pause in flight, unpause; mission end -> results screen: the gate opens (`inactive`) when the composites stop.
7. Restore checks: `lookstick:game` -> no heartbeat (`state=` lines absent), the stick turns the look again; ini `xr_block_look_stick=0` likewise.
8. D-358: the old (v1-name) proxy and a new acz-vr, or the reverse, never connect: install BOTH rebuilt DLLs together. xr-stereo.log
   `mapping=ok` (not `exists-not-adopted`). Optional: a second game instance (flat, no acz-vr) keeps its right stick while the first flies in VR.

## 8. Tests [E offline, not run by the author]
`look_stick_gate_offline` (tests/acz-vr/look_stick_gate_offline_tests.cpp): option extraction (day value byte-identical, block/game/0/1, `=`
form, last wins, bad values refused, the stage id is never an option, `look:1` untouched); heartbeat block (fresh, window edge, stale, window
clamps, future stamp, inactive, tampered payload, torn, bad version); StickGate (absent passes, fresh centres both axes at full deflection,
packet bump only on a gate change with the stick deflected, disabled passes, stale passes); Heartbeat (first tick, menus, flight, stop,
lookstick:game); an end-to-end writer/reader timeline (menus pass, flight blocks, composite stop passes at once, writer death passes after 500 ms);
two owners in one session (D-358: per-process names, A active never gates B without a publisher or with lookstick:game, one owner's stop or
pause never changes the other's signal, foreign-owner block passes, unbound reader passes, `can_adopt` generation rules, a stable fresh active sample is refused; NOT an exclusive-ownership protocol).
