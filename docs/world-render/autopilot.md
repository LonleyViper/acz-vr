# autopilot (closed-loop flight/kill driver for the null-driver live tests)

`research/experiments/step83-verify-run1/autopilot.ps1` replaces the open-loop "right tap every 30 s + S tap every 15 s" steering.
It reads the aircraft state from the logs the DLL already writes, sends only W/A/S/D/L keys (same SendInput path and foreground check as
`run83.ps1`), and logs every decision. No game memory is read or written. Written offline from archived logs; **not yet run live**.

## Usage

```powershell
# live (game in the mission, XR hooked): fly a racetrack inside the mission area at ~5000 u, 600 s
pwsh -NoProfile -File research/experiments/step83-verify-run1/autopilot.ps1 -Mode Cruise -Total 600
# live kill: steer to the nearest target contact, fire L every 3 s when aligned and in range (needs mk360:1 for contacts)
pwsh -NoProfile -File research/experiments/step83-verify-run1/autopilot.ps1 -Mode Kill -Total 400 [-NoFire] [-TargetColors red,yellow,green] [-TargetId 49] [-MissileRange 9000]
# offline: replay an archived run (no keys): the commands it would send, next to the keys the open-loop driver pressed
pwsh -NoProfile -File .../autopilot.ps1 -Dry D:\acz-evidence-archive\step83\runE10-logs -Events D:\acz-evidence-archive\step83\runE10-archive\run83-events.txt
# offline closed loop on the fitted plant model (optionally from the state of an archived run at T s)
pwsh -NoProfile -File .../autopilot.ps1 -Sim -Total 600 [-SimNoise 0.03] [-SimFrom D:\acz-evidence-archive\step83\runE10-logs@137] [-Mode Kill -SimTarget 60000,3000,60000]
```
Start it after V / the flight started (it waits up to 120 s for a valid flight state and sends nothing before). The decision log goes to
`autopilot-<time>.log` next to the script (`-DecisionLog` to change). Stops (exit 3, keys released) when the acz process is gone, the aircraft
state is frozen for 3 reads (mission failed / replay camera) or the position is back at the (46,-1,1) menu value; run it from the run script in
parallel with the sampler (the sampler's commit stop rule still applies: this script does not look at memory).

## What is logged (found in `D:\acz-evidence-archive`, steps 83-104)

| source | line | rate | fields |
| --- | --- | --- | --- |
| `ee-camera.log` | `ee-camera;obs;f=;k=e/b;fields=lp,ly,lr,hdg,ax,ay,az,cx,cy,cz,rp,ry,rr;A=base,size,flag:<13 floats>` | ~6/s (bursts 60/s for a few s; then ~1/s) | `hdg` rad (v = (-sin h, -cos h) in x,z; left turn = h up), `ax,ay,az` aircraft position (y up), `rp` pitch (+ nose up), `rr` roll (+ left bank). Zero until the flight starts; frozen at the end |
| `xr-stereo.log` | `event=mk360-contacts;contacts=id:win/out[+box]:r:b:e:v:id/none r,g,b:g` (needs `mk360:1`) | every 5 s, <= 8 contacts | range (3D, u), bearing deg (+ = right of the nose), elevation deg (world, not relative to pitch), speed u/s, identity colour (blue FRND, white = unknown), glyph count |
| `xr-stereo.log` | `camlag` lines (game camera eye/forward/up) | 60/s but only in short windows | not used (not continuous) |

Latency: the line prefix is `GetTickCount64` ms (equals `[Environment]::TickCount64`; the implied boot time is identical for runs on Oct 27 and 28), so
the script measures the age of the newest line itself and skips a decision when it is older than 3 s; the decision uses the state extrapolated by its age
(speed ~1100 u/s). The offline logs cannot give the file-flush delay: the live log (`age=` on every decision line) will.
The `tac`/`continuous-tac` lines are only 5-s averages (no per-contact data) and the HUD needs OCR: neither is used.

## Control response (fitted on 38 archived runs, ee-camera vs the `run83-events.txt` key times)

* D/A (right/left) hold `h` s: roll changes -/+ `0.95*h` rad (0.3 s: mean 0.28 +- 0.08, n=32 isolated left taps), onset ~50 ms, settled in ~1.5 s. The roll is not
  self-levelling (it stays), so a turn needs an explicit opposite tap.
* Turn rate = `0.15 * tan(roll)` rad/s (median ratio over 12700 two-second windows, 0.14-0.15 at every roll size).
* S (`down`, nose up) hold `h`: pitch `+0.33*(h-0.11)` rad (0.14 s: +0.010, 0.20: +0.031, 0.24: +0.038, 0.25: +0.044, 0.27: +0.050; dead zone ~0.11 s);
  pitch then stays. **W (nose down) has no archived sample**; assumed to mirror S.
* Speed ~1100 u/s (median of all runs, 1085-1121); climb rate = speed * tan(pitch) (50 u/s at 0.045 rad, checked). A banked turn sinks the nose
  (`dp/dt ~ -0.05*roll^2 - 0.015*pitch`), which is why the open-loop runs lost altitude (runA/B/Q2: y 4000 -> 486-1100 in 125-150 s of 0.3 s right taps without enough S).
* 0.14 s S taps barely move the pitch (+0.01 rad) and the S-tap pattern of run83 (0.14-0.25 s every 15 s) climbed to y 12000-22000 in several runs: that
  was the other drift (E10, S2, Z1, Z3: y 14500-22000).

## Mission area (derived from 38 archived flights)

* Flown without failing: x -90.5k..138k, z -22.7k..178k (S2 reached z 178k in a 240 s run; whether it was warned is not recorded).
* E10: AWACS "about to exit combat airspace" at T175 (z ~171k), MISSION FAILED at ~T184 (x 79k, z 180k); S1b OFF COURSE at T205 (x 69k, z 186k); R1b ended
  T225 at z 181k. So the +z edge is ~172k (warning) / ~180k (fail); no -x/-z/+x edge was reached (x 138k, z -23k fine).
* m02a ground height 0..480 u (heightfield bytes 0..6 x 80; "nearly flat", docs/world-render/terrain-data-inventory.md); runA ended at y 486 = the ground. The
  controller therefore uses an altitude band (target 5000, floor 2500, ceiling 12000) instead of a terrain lookup.
* Autopilot box: BOX x -85k..130k, z -15k..160k; INNER x -65k..110k, z 5k..125k (outside INNER: steer to the centre (25k,70k) with up to 0.6 rad bank);
  Cruise flies the racetrack (40k,110k) (90k,60k) (50k,8k) (-20k,30k) (-50k,100k) (10k,115k).

## Controller (period 2 s, one roll tap and one pitch tap at most)

Heading error -> turn-rate demand `clamp(0.25*he, +-0.05)` (+-0.10 outside INNER) -> bank `atan(w/0.15)` (<= 0.35, 0.60 outside INNER) -> tap
`clamp(|bank error|/0.95, 0.12, 0.40)` s when |error| > 0.05 rad. Pitch: altitude error -> climb rate (0.15*err, +-150 u/s) -> pitch demand; tap
`0.11 + |err|/0.33` s (0.14-0.30) when |err| > 0.012 rad; floor/ceiling override. Kill: contact (bearing, elevation, range) -> world position using the
aircraft state nearest the snapshot; velocity from consecutive snapshots (needs ids stable, 2-15 s apart); lead point at range/1100 s; heading and climb
angle to it; fire `L` 0.3 s every `-FireEvery` s when range < `-MissileRange`, bearing and elevation error < `-AlignDeg`. Targets are the identity colours
of `-TargetColors` (default red, yellow, green; blue = FRND never), nearest first within the first colour that exists, and only inside INNER.

## Offline results (this session; no live run)

* `-Sim` cruise, 600 s from the mission start: stays inside the area (max z 131k), y 4000-4440, 0 crashes; with 0.03 rad/s roll noise the same.
* `-Sim` from archived E10 states: T137 (first GUARD in the replay, z 136k heading +z): max z 149k (the open loop reached 180k and failed); T150 (z 150k): max z 161k;
  T170 (z 171k, already outbound): max z 177k, i.e. too late (the guard cannot save a run that is already at the warning line).
* `-Sim -Mode Kill` with a static red target: closest approach 410 u and 73 u, first fire at range ~8.1-8.8 km with bearing error < 1.5 deg.
* `-Dry` on E10 / runA / runB-step85 / R1b / S2: runA and runB (descent 4000 -> 486/707 u) trigger FLOOR at y < 2500 (T82 / T93) and the replay already commands nose-up taps
  from T10 (target 5000 > y); E10, S2 and R1b (y 12-15k, outbound to z 175-185k): CEIL (nose down) at y > 12k and GUARD -> right/left turns toward the centre from z ~136k
  (E10 T137, S2 T145, R1b T89). The replay is open loop (its commands do not change the logged flight), so it shows direction and timing only.
* Kill `-Dry` on S1/R1b parses the contacts and world positions (demo with `-TargetColors blue`); no archived contact is red.

## Risks / unverified

1. Not run live: key timing, foreground handling and the actual log-flush latency are untested. First live run: `-Mode Cruise -Total 120`, look at the `age=` field.
2. W (nose down) response and roll taps shorter than 0.2 s have no archived data (assumed linear / mirrored).
3. Which identity colour is a "TGT" is not established (no red contact in any archived `mk360-contacts` line; yellow and green occur, blue = wingmen). Use `-TargetColors`/`-TargetId`.
   The missile range is unknown (default 9000 u is a guess); the lock state is not logged, so firing relies on geometry only (earlier runs: 26 missiles fired without lock all missed).
4. Contacts arrive every 5 s and only with `mk360:1`; a fast crossing target is tracked by dead reckoning from two snapshots. The 8-contact cap can drop the target.
5. Flights that start at the edge of the area (z > ~160k heading outward) cannot be saved.

## Kill-mode fixes after batch L4 (offline only, not run live)

* L4 found the kill mode never fired in 7 attempts: mk360 contact ids are REUSED (id 35 alternated between the ground target, v0, and a wingman, `35:out:r190:...:v1127`), so the id-keyed track flipped between ground and air. Tracks are now associated by POSITION CONTINUITY (nearest unmatched track to the dead-reckoned position; gate 2500 u ground, 1500 + 1300 u/s * dt air); the class (ground = v < 120 at track start) is fixed per track and ground tracks get no velocity. The log prints `TGT#<last id>u<track uid>/<class>/<colour>`.
* The pitch-based bank cap no longer applies during an attack turn (bearing error > 10 deg, y > floor+800): bank up to 0.45 rad; the pitch demand is held >= -0.03 rad until the error is < 10 deg (no dive while turning).
* Overflight: target came within 3500 u and is now > 92 deg off the nose -> BREAK (max 90 s): turn back at 0.45 rad (0.20 below floor+800), climb to the band (`BREAK-climb`), re-attack when error < 20 deg, y > band-1000 and range > 5 km.
* `-Sim -Mode Kill -SimTarget 60000,480,60000 [-SimL4]` feeds mk360 snapshots every 5 s built from the plant state; `-SimL4` makes a wingman reuse id 35 on every other snapshot. Result: one track survives the reuse, first fire decision T=168 s (rng 11.6 km, bearing error 0.1 deg), BREAK at T~180 s after the overflight, re-attack cycles stay in the box, min y 3184. `-Dry` on L4 now prints the final tracks.

## Kill-mode fix after batch H (offline only, not run live)

* Batch H1 overflew a ground target: the attack descended to 2900 u, where the bank cap (y < floor+500 or nose down) applied and the bearing error stayed above AlignDeg. Ground tracks (v < 120) are now attacked from LEVEL flight: pitch holds `-GroundAlt` (default 4000 u, `GRD-level`, no dive below it); the attack-bank rule starts at 5 deg of heading error (was 10 = the fire threshold); `-GroundElevDeg` default 25 (level 4 km pass fires from 12 km to ~8 km). No `-AlignDeg` override is needed.
* `-Sim -SimStart x,y,z,heading` sets an offset start. Regression: `research/experiments/step83-verify-run1/test-autopilot-kill.ps1` (L4 id reuse, static ground, offset overflight case; each must fire and keep y >= 3300/3500).
* `killcap.ps1` start detection counts only a `steps.txt` line stamped at/after the helper's launch (a pre-seeded `autopilot start` line from an earlier run ended H2's capture at once).

## Batch J analysis: why Kill never produced a kill (batches H, I; 9 flights) and the changes

Causes (from decision logs + the mk360-contacts streams of step107 H1/H2 and step111 I1-I3, ground contacts mapped to world positions):
1. The nearest ground groups (ids 39-44 at ~16k,80-93k; 60 km from the start, listed from ~28 km) are WHITE (unidentified, rgb 1,1,1, v 0) until ~10 km and `-TargetColors green,yellow` ignored them; the autopilot flew 120 km to the prior (14k,21k).
2. The mission's green TGT contact (id 45) is a vehicle moving 135-220 u/s: the 120 u/s ground limit made it `air` (etol 10, elevation-pursuit pitch, bogus lead); every approach ended in `BREAK-climb` after an overflight at ~4 km altitude (elvErr 17-35 deg).
3. Tracks expired after 12/25 s, so after the firstwp leg or a break the group was forgotten and the racetrack WP loop wandered (I2 never saw a target; I3 arrived at the group unidentified, then picked a YELLOW group 35 km behind and U-turned); colour priority beat distance (a far green over a near yellow).
4. H2/I3 fired only at T373/T419 at far air/ground contacts; `-TargetId 39` (I2) is meaningless because ids are reused.
5. The gun (1.5 km slant) is unreachable from level 4 km flight (vertical distance alone 3.5 km).

Changes (autopilot.ps1): ground = v < `-GroundMaxV` 450; movers are led; ground tracks are remembered (dropped only when in view range and unseen 12 s); target score = distance + colour index * `-PriWeight` 6000; `unknown` (white) ground contacts are targets unless within `-HomeRadius` 22 km of the airfield (ids 1-8 sit there); priors `-FirstWp 'x,z;x,z;...'` = 16k,80k; 11k,57k; 14k,18.5k; 16.2k,9.1k, flown in order until a target is tracked; ground attack = level 4 km, from 14 km a gentle descent to `-StrafeAlt` 2000 (floor 1600), missile window 9000..3200 every 2 s (`-GroundElevDeg` 40), gun below `-GroundGunRange` 2300 (UNVERIFIED), 4 missiles at a still-listed target make it lose priority. `-Sim` got `-SimTargetV/-SimTargetHdg/-SimTargetRgb/-SimRange/-SimHomeJunk`; test-autopilot-kill.ps1 has 4 mission-start cases (first FIRE T 46-112 s, never at the airfield junk). Not yet flown live: whether L at 9-3 km with no lock kills is unknown.

## Batch K: why batch J's 6 missiles missed, and the lock-based strike routine (offline only, not flown live)
Evidence (step112 J1/J2): all 6 missiles were fired from level 2.4-3.1 km flight at a STATIONARY yellow ground group (mk360 ids 41/42, v 0; the moving contact is the green TGT vehicle, ids 39/40) with `elvErr` 10-19 deg: the target sat 12-30 deg below the nose, under the HUD window (mirror frames: the missile smoke leaves below the reticle, the target is behind the cockpit coaming, no target box, no lock reticle, HUD says MISS ~4 s later). The missiles flew straight = no lock. No mk360 contact ever showed a lock label (`g` stayed 0 for the target; g4 = FRND only), so the lock look and the missile range are still unseen. The old fire rule (`AlignDeg` 10, `GroundElevDeg` 40) was geometry that cannot lock.
Changes (autopilot.ps1): (1) lock model: bearing AND elevation error < `-LockDeg` 3.5 for `-LockSec` 1.6 s (1 s decision samples in the attack run) before each L, window `-LockRange` 9 km .. `-MissileMinGround` 2.4 km, `-Salvo` 3 per pass, log shows `lock=<s>`. (2) Ground strike dive (`GRD-dive`): from `-DiveFrom` 13 km and aligned (level approach at `-GroundAlt` 2800) pitch = line of sight to the target, nose ON it; no lead on ground targets (a 20 s lead put a 180 u/s mover 10 deg off the nose in the sim; the seeker leads); the aim-point lift ramps in only inside 4.5 km. (3) Dynamic pull-up (`PULLUP`): height above target < 250 + V * nose-down pitch * `-PullLag` 2.6 s (or y < `-PullY` 1000), S taps of `-PullHold` 0.7 s (UNVERIFIED live: the pitch fit covers holds <= 0.45 s). (4) After the overflight: `-BreakStraight` 10 s wings level (only with room inside INNER), climb only to `-ReAttackAlt` 3200, turn in; ~90 s per pass in the sim. (5) Stationary ground targets preferred (`-PreferStatic` 3000). (6) Gun J burst `-GunBurst` 2 s inside `-GroundGunRange` 2600 with the nose on the target.
Gun finding: with the plant's pitch authority (~0.1 rad/s) the pull-up needs ~800 u above the target at a 17 deg dive, i.e. more than 3 km slant; a gun pass inside 1.5-2 km is NOT safe without a faster pitch response (measure S/W holds of 0.6-1.0 s live first). The sim never puts rounds on target (gun on target 0.0 s); the kill has to come from locked missiles.
Sim: `-Sim` has its own lock model (`-SimLockDeg` 3.5, `-SimLockSec` 1.5, independent of the controller) and prints locked/blind missiles and gun seconds on target. test-autopilot-kill.ps1: 10 cases PASS (static, L4 id reuse, offset, 4 mission-start cases, 180 u/s mover, slow-locking game with `-LockSec 2.6`, far group) plus 'J replay' (level pass, LockSec 0) = 12 blind, 0 locked, which reproduces batch J. Live check: first locked FIRE with a red lock reticle + range in the game shot (watch the mk360 `g` glyph counts), then the hit text; tune `-LockSec`, `-LockDeg`, `-LockRange`.

## Batch K2 fixes (step113 KA2 crash, KA3 flyaway, K1 without frames; offline only, not flown live)
* KA2: the target left the list at 7 km, a yellow contact 43-50 deg off took over and the hard turn (bank 0.54) at 1100 u sank into the ground. Now: below `-LowBankAlt` 1500 u (real altitude, or altitude in 2 s at the current nose-down pitch when not in the deliberate strike dive) the bank is capped at `-LowBankRoll` 0.15 (`-DiveBankRoll` 0.35 in the aligned strike dive; wings-level target still 0.20 under floor+500 as before), roll taps run every 1 s while over the cap, and a non-dive pitch target below 0.10 becomes `LOWCLIMB`. Tracks are by position continuity (uid; the id is only logged), the followed track keeps priority (`-SwitchBias` 6000 on the score), and an established ground site (seen >= 3x) survives a 25 s listing gap instead of 12 s.
* KA3: the PULLUP latch now releases when the nose is up (p > -0.02), the aircraft is above `-PullSafeAlt` 1400 and >= 600 u above the target, or after `-PullMaxSec` 6 s once above PullY+200.
* Autopilot T0 touches `%LOCALAPPDATA%cz-vrxdiag.arm` (`ArmFxDiag`): starts the fx-air-draw / fx-flare-draw diagnostics in flight (see aircraft-shading-reflection.md section 6).
* killcap.ps1 always writes a `launched` line, logs while waiting, starts anyway when the autopilot log appears or acz runs `-StartTimeout` 240 s, exits `never started: ...` after `-WaitMax` 900 s without acz, and logs capture errors (K1/H2 had no frames and no explanation).
* Sim: `-SimFlipRange/-SimFlipDeg` (the id flip), summary prints `max bank below 1500 u`. test-autopilot-kill.ps1: 14 cases (4 new K2 cases: flip at 1100 u / 1400 u / 70 deg / normal strike, and a PullMaxSec 4 run; every case asserts bank below 1500 u <= 0.30). The sim did NOT reproduce the old KA2 crash (the old controller also survives it in the plant model), so the new cases guard the limits, not a demonstrated before/after. The 180 u/s mover case was a knife edge (pre-K2 code also misses at 160-200 u/s; 180 hit by luck) and is now 120 u/s.

## Batch M (step114 findings; offline only, not flown live)

Findings from step114 batch L (L1a/L1b/L1c/L2a/L3) and what changed in `autopilot.ps1` (all verified only in `-Sim`, `test-autopilot-kill.ps1`):

1. **False "mission over" (liveness).** At T~62 the ee-camera aircraft struct (and the camera fields in the same struct) reads zero (pos 46,-1,1) while the game keeps flying (L1a game shot T63: ALT 3500, normal HUD); the hook keeps writing `ee-camera;obs` lines (`f=` advances every ~0.33 s) and mk360 snapshots every 5 s until the end of the run. This also explains K1/KA3. The live loop no longer stops on a zero struct. Liveness = acz process alive AND (a fresh ee-camera line OR an mk360 snapshot < 12 s old); stop only after `-HookDeadSec` 30 s without either, after `-BlindMax` 240 s blind, or on a frozen (non-zero) struct. While the struct is zero the script flies on a **dead-reckoned state**: the fitted plant is seeded from the last valid state and fed with the script's own key taps (`DR` in the log, `BLIND` suffix on every decision line). Each mk360 snapshot FIXES it: remembered stationary ground sites (seen >= 3 times before the loss) keep their world position, their range/bearing/elevation then give the aircraft position (one site) and heading (two sites >= 2.5 km apart) (`DrFix`, log `DR fix pos|pos+hdg`; jumps > 15 km or > 0.6 rad are rejected; new tracks are not started while blind). When the struct comes back the log prints dead-reckoned vs read state. Sim: `-SimBlindAt <T>`; dead-reckoning error stays 0.3-2 km with roll noise.
2. **Stale-state dive.** The newest ee-camera line is 0.4-1.6 s old (L1a: `age=1.5`) and the roll/pitch response lags ~0.45 s, so the S/W taps of the last 1-2 s were invisible and the dive wound up (pitch -0.40 against a -0.20 demand, elvErr 10-22 deg, PULLUP at 750-1300 u). `Decide` now adds the effect of every tap whose release is not in the sample yet (`-NoPredict` switches it off). `-Sim` reads the attitude `-SimStateAge` 1.5 s late (default); with `-NoPredict` the same static-ground case dives to the ground (min y 489, crash), with the compensation min y 949 and the dive elevation error on the 5-8 km leg is 2-3 deg (Gun) / <= 10 deg including the aim lift (Kill).
3. **Lock gate defaults.** `-LockDeg 9 -LockSec 0.5 -DiveFrom 9000` are now the defaults (L1b needed them to fire at all; the 13 km dive started the wind-up earlier). The sim's own lock model is unchanged and strict (3.5 deg for 1.5 s), so in the sim the first missile of each pass is typically blind (locked >= 1 is asserted, blind counts are printed). The real game's lock rule is still unknown: L1b/L1c fired 4 missiles at lock gate 0.5 s with elevation errors of ~5 deg and all MISSED, so the gate may be too loose; if the live HUD shows no lock box before the missile leaves, raise `-LockSec` (1.0-1.6) first.
4. **GUN-STRIKE (`-Mode Gun`).** For a stationary ground group (movers and aircraft are never targets; mk360 ground classes as before): level approach at `-GunAlt` 1900 (held from the first sighting, deadband 150 u), from `-GunDiveFrom` 10 km the NOSE is put on the target (line of sight, no aim lift, max dive `-GunMaxDive` 0.22), J held `-GunStrikeBurst` 2.5 s once inside `-GunFireRange` 2600 (+ half a decision period) with bearing < 5 and elevation < 8 deg; no missiles; then the usual dynamic pull-up and break. Geometry limit found in the sim (same as batch K): the pull-up needs about `250/theta + V*lag` of slant (theta = dive angle), i.e. 2.4-3.4 km at theta 0.2, so a burst from 1.2 km is only possible by pulling later. Defaults are the safe profile (`-GunPullY` 850 judged on the height at the next decision, `-GunPullBase` 100, `-GunPullLag` 1.8: min y 745-792 in 3 scenarios incl. roll noise, a burst in each, burst starts at ~2.6 km, nose on target 0.0-0.1 s by the sim's 1.5 km gun model). The aggressive profile `-GunPullY 800 -GunPullLag 1.2` gets the nose on target for 0.8 s but bottoms at y 554 (54 u above the ground; 617 in another case): only for a measured live test. The break no longer cancels a pull-up in progress (it forces PULLUP while the pitch is < -0.03). Whether the burst kills anything (gun range and damage) is unknown; the sim never scores gun hits.
5. **SUN-LOOK (`-Mode Sun`).** Steers heading and pitch to the stage sun for the flare capture. Sun direction: the mapset block of the stage (`data/mapset/map/mapset<stage>.bin` +0x58 elevation, +0x5C heading, world2-impl.md 1.1; m02a 0.8727 rad = 50 deg and -2.269 rad) equals the renderer's `kSun` (-0.42, 0.80, 0.43) within ~6 deg (`terrain_stage.h`), i.e. world horizontal direction (sin(h), -cos(h)) = (-0.77, 0.64). In the autopilot's heading convention (v = (-sin h, -cos h), left turn = +) the sun is at **+2.269 rad** (the sign flips); the test checks it against the header. Phases: run in to the up-sun point (`-SunRun`, default 62 km up-sun of the centre, 3000 u), then hold the sun heading wings level with pitch `-SunPitch` 0.25 (sun then 36 deg above the nose; 0 = level, sun 50 deg above) for `-SunHold` 20 s of aligned flight (aligned = within 0.12 rad), level above `-SunAltMax` 10500; then cruise. Other stages: `-SunMapset <file>`. The log shows `sunRelBrg/sunRelEl/ph/al` on every line; the flare capture itself (fx-flare-draw with `fxsig:f1a4e000`, arm file touched at T0) is untested. Sim: run-in ~110 s, look 20 s, y 3000 -> 9700, inside the box.
6. **Mirror frames.** gunburst.ps1's PrintWindow capture of the Headset Window gave black frames (L2a); it is now off (`-WithMirror` re-enables) and killcap.ps1 (CopyFromScreen, upright, view with `mirror-sheet.ps1 -NoRotate`) goes to fast mode on every `J DOWN` line of `gunburst.log` as well as on FIRE/GUN decisions. runL.ps1: `-AP '-Mode Gun -Total 300'`, `-AP '-Mode Sun -Total 260'` work unchanged (gunburst `-GunTimes` stays for manual bursts).

Regression: `test-autopilot-kill.ps1` (now 14 old + 11 batch M cases + the kSun check; the 220 u/s mover case is marked KNOWN-FAIL: HEAD never fired at it either).