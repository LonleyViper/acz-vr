# First flights on the turnable test HMD (step185, 2026-11-02): findings

Evidence: `D:\acz-evidence-archive\step185\boxes\runAGB1|runAGB2`, `...\clouds\runAGC1|runAGC2` (xr-stereo.log, autopilot-1.log,
cloudtrig.log, logs\png eye0 dumps, mirror\*.png), `%LOCALAPPDATA%\acz-vr\testhmd.log` (target/pose wall times), upright previews in
`research/experiments/step185-shots`. Offline analysis only: nothing here was run live. Tags: [E] evidence, [I] inference.

## 0. Reading the images (important for every later test-HMD analysis)

* The eye dumps are eye 0 (left eye) only. The `-hudlayer` dumps are upright; the eye dumps and the Headset Window are upside down
  because the runtime's LOCAL head is rolled 180 deg (section 3), not because of the PNG writer.
* After the usual 180 deg rotation of a whole mirror frame, the **upright RIGHT half is eye 0** [E: the canopy rail heights and cloud
  positions of eye0 dumps f6695 / f7895 match the right half of mirror m0062 / m0079, not the left half].
* `cloudtrig` writes its own `png-eye.now` at the START of each look (13:40:02.275, 0.11 s after the 13:40:02.161 target), so in the
  clouds runs the FIRST dump of each look pair is mid-slew (f5418, f6624, f7826, ...); the autopilot HEADSHOT is the second (f5494, f6695,
  f7895, ...).

## 1. Problem 1: the strip with the red line in one eye

* [E] Not in our eye images: eye0 dump f7895 (13:40:43.6, head at the look-left target) has no strip while the mirror's eye-0 half
  (m0079..m0081, 13:40:43.2-45.2) shows it; AGB2 eye0 f13010 vs m0163 likewise.
* [E] Not one of our quad layers: during runAGB1 13:25:15.7-20.7 `summary-hud hud-layers=0` and `summary-markers marker-layers=0`
  (mission-end cinematic, only the projection layer was submitted), yet m0141..m0144 (13:25:16.9-19.9) show the strip and the red line.
  The HUD quad is 10 m ahead (`first-hud-layer hud-pos=0.003,-0.015,-10.000`), the marker quad 500 m: neither can project 11-15 deg from
  the view centre when the head is turned 90 deg.
* [E] It is not the forward-window feather either: `edge-feather=0.000`, and the window is composited inside the projection image.
* [E] Geometry: one eye only, 11-15 deg from the view centre toward the BACK of the turned head, tilting with the head pitch like a
  vertical world plane (vertical in m0042, diagonal in m0163/m0242), translucent dark band with a red edge, steady while the head holds.
  That is a world-fixed vertical plane passing within a few cm of the head, added by the SteamVR compositor.
* [I] Most likely the SteamVR chaperone (collision bounds) near the driver's fixed head position (the test HMD stands at the raw tracking
  origin; the stored universe in `Steam\config\chaperone_info.vrchap` puts walls 0.05-2 m from it depending on the frame convention).
  Not proven: the colour (red here) and the one-eye visibility are not explained. The user's "thin cyan lines" in headset test 9 fit the
  same cause (SteamVR's default bounds colour is cyan; the seated origin is ~0.9 m from the play-area wall), also unproven.
* Fix (diagnostic, harness): `null-vr.ps1 -Driver acz_testhmd` now writes `collisionBounds` (alpha 0, fade distance 0, no perimeter /
  centre marker / play space) into the run's vrsettings; the byte-identical restore removes it. If the strip is gone on the next flight the
  chaperone is confirmed; if not, it is another compositor overlay (look next at the Headset Window's debug drawing). Nothing in the DLL
  changed for this item. For the user: check whether the cyan lines disappear with SteamVR's chaperone hidden (ask first: it is their setting).

## 2. Problem 2: eye dumps without any cockpit

* [E] runAGB1 f9255 was the automatic `pov3-cinematic(a-only)` dump and f11625 the HEADSHOT at T=134.5; both were taken after the mission
  ended: autopilot `T=104.9 aircraft state frozen: cutscene or mission over`, xr-stereo `pov-mode;from=cockpit;to=hud-only;...;
  composite-age-ms=563;cockpit-model=gated` at 419343078 (the game stopped drawing its cockpit, DrawCPT), `summary-cockpit model-frames=0`,
  `summary-pov-stage3 request=cinematic`. The cockpit model is gated by design in a no-cockpit game view (view_mode.h
  `draw_cockpit_model`). Not a head-turn, cull or depth problem: the other turned-head dumps all have the cockpit.
* Fix (harness): `autopilot.ps1` no longer starts head looks (`-LookEvery`, `-LookAtContacts`) while the aircraft state is frozen.

## 3. Problem 3: the turned head looks the wrong way (pitch AND yaw mirrored)

* [E] Pitch: f13010 (target yaw -90.6 pitch +53.3) shows the side console with the canopy sill near the TOP edge (sill ~+24 deg above the
  view centre); the same for f17863/f20152 (a wing store ~10 deg above centre at pitch +30). That is a view ~50 deg DOWN.
* [E] Yaw: the cloudtrig mid-slew dumps turn the wrong way: f5418 (0.11 s into a LEFT turn) has the instrument panel shifted LEFT
  (= the view turned right ~20 deg), f6624 (start of a RIGHT turn) has it shifted right; the wing stores point toward image-left in the
  "look left" shots (forward on the left = looking right).
* [E] At rest the eye images are upside down (section 0).
* [I] One cause fits all three: the game's OpenXR LOCAL space is the driver space turned 180 deg about the forward axis,
  head_local = Rz(180) * head_driver. A frame-side 180 deg roll negates the yaw and the pitch of every turn and keeps the roll; a head-side
  roll would only turn the image. Our pipeline renders the pose it is given consistently (`world_composite_params` and the cockpit
  `eye_camera` use the same quaternion columns; `+pitch` = up in both, checked); the mirror shows no reprojection borders at rest, so the
  submitted pose equals the display pose. The real headset does not have this roll (the user would see an upside-down, counter-rotating
  cockpit). Where SteamVR introduces the roll for these desktop debug HMDs is not identified.
* Conventions: `testhmd` pose file and autopilot `-HeadPose` both use +yaw = left, +pitch = up (pose_core.h, unit-tested) and autopilot
  `pitch = contact elevation - nose pitch` (+ = up): correct in themselves; the sign flip happened after the driver.
* Fix (driver): new setting `driver_acz_testhmd.localRollFix` (default **true**, logged as `localrollfix=1` in the init line). The driver
  reports Rz(180) q Rz(180)^-1, i.e. the quaternion with x and y negated (and position / velocities / angular velocity as (-x, -y, z)), so
  head_local = q_cmd * Rz(180): the commanded look direction exactly, the rest-pose image roll unchanged (all "rotate 180" analysis steps stay
  valid). `pose_core.h local_roll_fix`, tests `test_local_roll_fix` (the unfixed chain mirrors the forward vector, the fixed one gives the
  commanded forward and keeps the image roll, vectors map back, +pitch looks up): `testhmd_pose_core` 75/75. Set it false to get the old
  behaviour. Needs the rebuilt driver installed into `E:\acz-testhmd\installed` (not done here).
* `nh:1` (in-mod null head, xr_owner PoseFilter) left-multiplies the turn in LOCAL, so it is NOT affected by this roll.

## 4. Problem 4: contacts and 360 boxes during the head looks

* [E] runAGB2 looks (T=165.3, 246.0, 284.2) all aimed at a GREEN (friendly) contact on the right: at T=165 it is
  `7:out:r13708:b90.6:e42.5:...:id0.29,1.00,0.22:g0:c2` = identified, with a cached stamp cell, so a box existed at b=+90.6 - but the head
  actually turned LEFT and down (section 3), so no head shot could contain it. `summary-mk360-nbox-backend used` rose 2569 -> 2815 in that
  window: stamps were drawn when their contacts were in view.
* [E] runAGB1's only look (T=133.2, during the mission-end freeze) aimed at `42:out:r10717:b81.5:e6.8:...:none` = never identified
  (never in the game's window), so mk360:3 has no picture of it; with provisional boxes off (step183, `prov-friend=0;prov-enemy=0;
  prov-unknown=0`) nothing is drawn for such a contact. The game never boxed it either (out of its window), so provisional-off removed no box
  the game showed: `in-window-identified-without-box=0` in all four runs.
* [E] Few contacts to look at: runAGB2 had 6 tracks, 2 identified, 4 unidentified ones behind (b 150-171); runAGB1 8 tracks, 0 identified.
* Fix (harness): `-LookAtContacts` now prefers a contact with an identity (the game boxed it at least once, so a box can be shown) and only
  then the nearest unidentified one.

## 5. Also wrong in the turned-head images: whole-eye blue/green box washes

* [E] runAGC1 f5494 (eye 0, 13:40:03.5) and mirror m0040 (13:40:03.1, both eyes) are covered by a translucent lavender/blue sheet with a thick
  straight band at the top; runAGB2 mirror m0169 (13:32:49, the head slewing home) has two broad green vertical bands. The world layer of
  f5494 (`-worldfx`) is clean, so it is drawn later in the eye pass. The colours are the friendly box colours of the contacts present
  (`id0.21,0.23,1.00` blue in AGC1, `id0.29,1.00,0.22` green in AGB2).
* Cause [I, strong]: NBOX stamps (mk360:3) whose anchor is far outside the eye's view. `eye_clip_` only refuses points BEHIND the eye and
  `stamp_frame` takes the stamp size from the local projection Jacobian, which grows as 1/cos^2 of the off-axis angle: a box 80-89 deg off
  the view axis (reached during fast head turns, also in DECOUPLE re-composites mid-slew) becomes a quad several NDC wide whose magnified
  outline washes the eye.
* Fix (DLL, `marker360.h build_stamps`): an anchor with |NDC x| or |NDC y| > `kStampAnchorNdcMax` = 1.25 draws nothing (a box just past
  the edge still draws its visible part; inside the margin the magnification is at most ~2.5x for a 90 deg field). Counted (eye 0) as
  `summary-mk360-hs6 ...;off-view=N`. Test `test_nbox_off_view` (on-axis, 40 and 46 deg drawn and bounded; 55, 85 and -89 deg not drawn
  and counted; behind the eye refused earlier and not counted).
* Haze, clouds and seams in the turned views: the sky/haze gradient and the terrain look continuous in f6695, f7895, f13010 and f20152; no
  seam is visible at the former window edges. The cloud cache puffs could not be judged (no puff in view in the head shots).

## 6. What the next test-HMD flight must check

1. testhmd.log init line `localrollfix=1`; run.cfg / vrsettings carry `collisionBounds` alpha 0 during the run and are restored after.
2. Look left (`90 0 0`): the head shot shows the LEFT side (stores pointing toward image right = forward on the right); the cloudtrig
   mid-slew dump has the panel shifted RIGHT. Look right: the mirror image of that.
3. A `+53` pitch look shows canopy glass and sky above the sill, not the side console.
4. The red-edged strip is gone in the mirror at 90 deg head turns (chaperone confirmed); if not, record its eye/position again.
5. HEADLOOKC aims at identified contacts; the head shot shows that contact's box; no look starts while `aircraft state frozen`.
6. No blue/green whole-eye wash in the mirror during slews; `summary-mk360-hs6 off-view` grows during turns.
7. Only the headset can confirm the user's cyan lines (section 1).

## 7. Verification flights (step186 build, 2026-11-03/04; evidence `D:\acz-evidence-archive\step186c|186d|186e`)

* [E] `localrollfix=1`: a 90 deg right look shows the cockpit side with the windscreen frame on the left; a 90 deg left look shows the mirror
  image; horizons level (186d f5531 / f6734). A +53 deg look (186e f5546) shows sky and sun glare with only a sliver of cockpit at the
  corner, not the side console: the pitch sign is right.
* [E] `summary-mk360-hs6 off-view` grows (120 in the 186c boxes flight); no whole-eye wash seen in the dumps opened.
* [E] The red-edged one-eye strip remains with `collisionBounds` hidden (186c AGB2 m0172, AGC1 m0044): it is NOT the chaperone. Next
  suspects: another SteamVR overlay or the compositor. Only the headset can confirm the user's cyan lines.
* [E] Bug fixed (ff7caed1): the step185 `-LookEvery` edit put a `#` comment mid-line, commenting out `$lk`, `lookN++` and `nextLook`;
  the look repeated every tick with no direction (186c clouds). 186d alternates left/right and goes home; cloud-trigger dumps 6 of 6.
* Open: looks at identified contacts (186c AGB2 aimed at an unidentified one), the `ee-camera` stale-eye FAIL row and the
  continuous-hud-effects dual-refused FAIL in the clouds analysis, the strip.

### 7.1 The strip experiments (2026-11-04)

* Gunstock Buddy (a SteamVR overlay app that autostarts every run) closed for the run (step186f): the red-edged strip is still there
  (m0083, now in the other eye). Not that overlay. fpsVR is the corner overlay in the mirror (both eyes); not the strip either.
* Head raised to y=1.6 m after the flight started (step186g, pose file `0 0 0 0 1.6 0 90`, the autopilot's 4-number lines keep the
  position): NO strip in the 90 deg look-left mirror frames (m0043). The cockpit is then left behind below the head, which is the
  correct 6DoF result for a head moved 1.6 m up from the captured neutral, not a bug.
* So the strip depends on the head being at the tracking origin / floor: a world-fixed plane near that point, i.e. the SteamVR play-area
  / chaperone boundary near the origin. The `collisionBounds` alpha 0 setting did not hide it, so SteamVR ignores that key (or draws the
  boundary another way). For the user: a seated origin close to a play-area wall can show the same lines (cyan in SteamVR's default
  colour); re-centre or reshape the play area. Not proven for the real headset.

### 7.2 Contact looks (2026-11-05, step186i; evidence `D:\acz-evidence-archive\step186i\boxes\runAGB1`)

* The default window (45-115 deg off the nose, 14 km) found no contact in the full boxes flights (step186h: LOOKC 0). Widened with the new
  `runBatchAG -BoxesApAppend '-LookMinDeg 30 -LookMaxDeg 180 -LookRange 30000'` (appended switches replace the same-named defaults): 32 head
  looks at contacts, mostly BEHIND the aircraft (b 155-180 deg), no frozen-state looks.
* [E] f7641 (head turned ~156 deg left, +13 deg up at a green contact): two green NBOX stamps are drawn on the sky behind the aircraft, so
  identified contacts out of the forward window do get boxes in the turned view; no whole-eye wash. The cockpit rear (seat back, headrest)
  is a plain dark block model: its look is a cockpit-model item, not a regression.
* The `cam=2,2,2` and 17.5.3 analysis FAILs are the known stale checks (same in step183-185, documented in the morning handoffs).

### 7.3 Briefing table with a moving head (2026-11-06, step186j; evidence `D:\acz-evidence-archive\step186j\brief\runAGM1`)

* New `runBatchAG -BriefMap -TestHmd` steps the test HMD through yaw +-25, pitch 15, lean +-0.3 m and up 0.3 m (runK3ag `-HeadSweep`, one pose per
  png-screen trigger). Overview leg reached; the briefing-menu leg was not (`briefing=False`, a known route weakness, unrelated to the head).
* [E] `summary-brief3d-latch plane=3617 plane-off=0` over 3617 renders, no render-failed/poisoned/dead: the world-fixed table quad (b3p:1) stayed
  in use for the whole sweep.
* [E] The eye0-table dumps (producer image, 10 of 10 content) shift sideways between head poses (bbox x 0..2251) as expected for a table that
  stays put while the head moves.
* Not proven: the null mirror is black in the menus (playbook section 7), so no compositor frames exist to compare the table's on-screen
  position per pose. Whether the table still "swims" can only be confirmed in the headset (or with a CopyFromScreen capture).

### 7.4 Briefing table, compositor frames (2026-11-06, step186k; evidence `D:\acz-evidence-archive\step186k\brief\runAGM1\mirror`)

* killcap waited for 'autopilot start' and so never captured in a briefing run; AGM1 now passes `-StartPattern BRIEFMAP.hold.start` (32 mirror frames).
* [E] `research/experiments/step186-shots/brief-headsweep-mirror.png` (rotated upright; neutral, yaw +25, yaw -25, pitch +15, lean x +0.3/-0.3): the
  table and the map panel move across the view opposite to the head turn/lean and change elevation with pitch, i.e. they stay fixed in the
  world. The yaw -25 frame caught the pose slew (a panel edge doubled); the pitch frame also shows a small unrelated panel at the top (the
  mission-menu layer; not examined). Residual swim in the real headset (tracking latency, low fps) is still only judgeable there.
