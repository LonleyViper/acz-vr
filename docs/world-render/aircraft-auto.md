# AIRCRAFT-AUTO: `xr_cockpit_model=auto` picks the model cockpit from the E6 eye offset

Status: implemented and tested offline only (no game, SteamVR or headset run in this task). Whether the detection works in the real game is
unverified until a driver run reads the `cockpit-auto` lines (section 8).

Code: `src/acz-vr/aircraft_auto.h/.cpp` (SDK-free: type table, E6 sample -> aircraft-frame eye, mailbox, detector, config value),
publishing in `src/acz-vr/ee_camera_probe_win.cpp` (`aircraft_eye_publish`, locked E6 samples only), consumer in
`src/acz-vr/xr_stereo_output.cpp` (`cockpit_auto_tick`, `cockpit_auto_install`, `load_auto_model`), proxy wiring in
`src/acz-vr/xr_stereo_proxy.cpp`. Bakes: `research/world-render-data/bake_all_aircraft.py` -> `D:\acz-extract\models-all`, table
`docs/world-render/aircraft-table.md`. Tests: `tests/acz-vr/aircraft_auto_offline_tests.cpp` (ctest `aircraft_auto_offline`),
`xr_stereo_output_offline_tests --cockpit-auto` (ctest `xr_stereo_cockpit_auto_offline`).

Labels: **[E]** = evidence (measured on real data or game files), **[T]** = shown by an offline test only, **[I]** = inference.

## 1. What it does

`xr_cockpit_model=auto[;opt:val...]` (the existing key; no new config key). Instead of a fixed `<id>_vrcockpit`, the XR consumer:

1. receives, for every validated sample of the **locked** E6 block (anchor-free locator, `docs/world-render/e6-anchor-free.md`), the camera
   offset in the aircraft frame and whether the game's look is at rest;
2. collects windows of 12 look-centred samples (24 before AIRCRAFT-AUTO-FAST, section 1a), takes the per-axis median, and matches it against the 36 `.pvb` eye points;
3. chooses a type (hysteresis: a change needs two agreeing windows), loads `<type><skin>_vrcockpit.zvrm` from `%LOCALAPPDATA%\acz-vr\models`
   on a worker thread, and draws it from the frame the load completes;
4. draws the ASSUMED model (remembered last aircraft or `default:`, section 1a) from the first frame when one exists; otherwise draws **no model** (the game's replayed cockpit, exactly the key-off path) until a type is chosen, while a model file is missing or broken,
   and after the offset stopped matching any type for two windows.

Options: every `cockpit_model` option applies to whichever model is chosen (`upm: gain: amb: cut: near: ext: glass: hudglass: floor: dom:
light: replay: canopy: tint: refl: dirt: interior: anim: stick: zpre: cscale:`; `cscale:<0.8..1.5>` scales whichever model is chosen about its pvb eye,
cockpit-render-impl.md section 15); the per-model ones (`hide: show: eye:`) are refused with `auto`; `stick:`
given explicitly overrides the table's per-type stick style (side-stick for f16c, f16x, f35c, fa22, rm01, su37, su47, yf23 [I: real aircraft]);
`prefer:<type>[,<type>]` (auto only) chooses inside an identical-eye group (section 4). Requires the anchor-free locator: `xr_terrain_render` on
and `xr_terrain_stage` with `e6loc:auto` (default) or `e6loc:content`; otherwise the proxy logs `cockpit-model-disabled;reason=auto-needs-e6-locator`.
`hudglass:1` has no effect with `auto` (the proxy places the HUD quad before any model is known) [limitation].

## 1a. AIRCRAFT-AUTO-FAST: the model cockpit from the first XR frame

Problem (headset test 17, `D:\acz-evidence-archive\headset-test17\xr-stereo.log`) [E]: XR launch 225277421, `cockpit-auto;state=armed` 2.2 s
later, but the E6 locator locked only at 225286487 (ee-camera.log: the candidate was `aircraft-at-origin`, then `static` while the mission
loaded); the first 24-sample window matched j35j 0.39 s after the lock (225286875) and the model was installed at 225286906 = **9.5 s after the
launch**. Until then the game's forward-only replayed cockpit was shown ("the full cockpit didn't render until I turned my head"). The lock
delay dominates; the window adds ~0.4 s.

Fix (no new key; options inside `xr_cockpit_model=auto[;...]`):

1. **Remembered last aircraft**: `%LOCALAPPDATA%\acz-vr\last-aircraft.json` = `{"version":1,"type":"j35j","model":"j35jk_vrcockpit"}`, written
   by the consumer when the detector confirms a type that differs from the file (first match, confirmation of an assumption, or a switch);
   `-1`/no-match is never written. Write = temp file `last-aircraft.json.tmp` + rename (MSVC `std::filesystem::rename` =
   `MoveFileExW(REPLACE_EXISTING)`: the old or the new file, never a torn mix), on a detached thread (never inside a frame), serialized by a
   mutex. Read validation (`parse_remembered`): <= 512 bytes, a JSON object, `"type"` and `"model"` as plain strings (each key once), characters
   `[A-Za-z0-9_]` only (`/ \ : . %` are refused as `path-characters-refused`, which also rejects any JSON escape), `type` a table entry and
   `model` exactly that entry's table stem. A file can therefore only ever select one of the 36 table files under the models folder [T].
2. **`default:<model_stem|type>`** (e.g. `auto;anim:1;default:j35jk_vrcockpit`): the assumption when no valid file exists (missing, corrupt,
   refused). The value must name a table entry; a path, an extension, an unknown stem or > 24 characters refuse the whole value (`bad-option`,
   the replayed cockpit, as for any bad option) [T].
3. **Assumed model before the session**: the proxy resolves file > `default:` > none (`event=cockpit-auto-remembered;file=;result=;reason=;
   default=;assumed=;source=`); the `Output` constructor loads that model on the XR monitor thread (the same thread that loads a fixed
   `xr_cockpit_model=<id>`, before `xrCreateSession`; ~11 ms for j35jk, D-322), so the **first composed frame already draws the model** and drops
   the replayed cockpit band (`action=assume;...;installed=yes;drawn-from=first-frame`). A worker thread would leave frame 1 without it. A failed
   assumed load logs `action=assume-failed` and falls back to the plain detector (first window chooses; the replayed cockpit until then).
4. **Confirm or switch** (`Detector::assume`): a window matching the assumption logs `action=confirm` (no switch counted); `confirm` (2)
   consecutive agreeing windows of ANOTHER type switch (`action=switch;from=;to=;reason=assumed-type-contradicted`), then the usual load/install
   (`replaced=yes`: the old model stays drawn until the new one is installed, so no frame shows the replayed cockpit). `no-match` and `unstable`
   windows, a lost lock and a new lock epoch never drop an assumed model: only a confirmed different aircraft replaces it. A confirmed type's
   later no-match behaves as before (two windows unload).
5. **Faster detection**: windows of **12** look-centred samples (was 24; tolerances unchanged), and the consumer feeds **every** E6 sample: the
   probe's mailbox now also writes a 64-slot ring (one seqlock per slot) that the consumer drains per tick, so a slow composed-frame rate no
   longer drops samples (overruns counted as `lost-samples`). Test 17 fed ~55 samples/s, i.e. the window costs ~0.2 s instead of ~0.4 s; the
   locator's lock time (8 samples / 3 moves, ~9 s in test 17 because the aircraft is static during loading) is unchanged and is exactly what the
   assumed model hides.
6. **Evidence lines**: `action=first-frame;frame=;model=;model-drawn=yes|no;replayed-cockpit=dropped|shown;assumed=`, and per summary
   `event=summary-cockpit-auto-start;assumed=;source=;confirms=;lost-samples=;remember-writes=;remember-ok=;remember-failed=;remembered=`.
   The replay-drop rule is unchanged: bit 17 is set only on recordings where the backend drew the model (a GPU refusal shows the replay).

Window 12 vs 24, false-match risk [T, `aircraft_auto_offline_tests` `test_window_risk`: the nearest distinct pairs e18g/m29a 0.1616,
f02a/f16c 0.224, su27/su37 0.300 in both directions, 4000 windows each, gaussian noise per axis on the true eye]:

| noise u/axis | window 24: wrong / no-match | window 12: wrong / no-match |
|---|---|---|
| 0.02 | 0 / 0 of 24,000 | 0 / 0 |
| 0.03 | 0 / 0 | 0 / 0 |
| 0.05 | 0 / 0 | 0 / 4 |
| 0.08 | 0 / 16 | **6** / 883 |

A wrong type needs the median to move more than gap - 0.08 (0.0816 for e18g/m29a) towards the neighbour; the median's sigma is ~1.25 s/sqrt(n)
per axis, i.e. 1.41x larger with 12 samples. The live per-window spread in test 17 was 0.027 (max |sample - median|, so s ~0.01) and the
logged residuals 0.000-0.039 [E]: at that noise window 12 has no false match in the simulation. Only at 3-8x the live noise (0.08 u/axis) does
window 12 produce rare wrong first choices (6 of 24,000 on the nearest pairs); a wrong first choice is replaced after two windows (~0.4 s).
The 0.08 tolerance and the 75 % inlier rule are unchanged; the `test_all_types` confidence bound was relaxed from 0.5 to 0.25 (lowest seen 0.49).

Install value for the morning runs: `vr-test-start.ps1 ... -CockpitModel 'auto;anim:1;default:j35jk_vrcockpit'` (mission 02 = Draken). The
script prints the remembered file and warns when the default model is not installed.

Unverified live [I]: that the first XR frame shows the model in the headset (offline: the first job carries it and `first-frame` says
`model-drawn=yes`); the assumed load time on the user's machine; the remembered file across missions with different aircraft.

## 2. Evidence: the eye offset in the aircraft frame

* The E6 structure holds aircraft position (`+0xFDD42C`), heading, look angles and camera position/rotation (`+0xFDD860`, `+0xFDD870`)
  (D-237..D-241, e6-anchor-free.md section 2) [E].
* The Draken's `.pvb` eye point (0, 3.575, -36.3) equals the live camera - aircraft offset (D-295/D-303) [E].
* Rotation convention, fitted on real E6 samples (all permutations and signs of Rx/Ry/Rz tried): **world = Ry(rot yaw) Rx(rot pitch)
  Rz(rot roll) * local**. Residual of the recovered local offset against the Draken pvb eye [E, values as logged]:

  | sample | attitude | residual |
  |---|---|---|
  | step67 level flight (hexdump floats) | heading -45 deg | < 0.02 |
  | headset test 16 | pitch -6.8, roll -169.7 deg (inverted) | 0.018 |
  | step56 nose-down | pitch -29.6 deg | 0.060 (logged values rounded to 0.01) |

  The opposite roll sign puts the inverted sample > 1 unit off; the roll convention matters only for the lateral eye offsets (e06b, su32) and
  is pinned by the inverted sample.
* With the game's look turned, the rotation includes the look and the camera pivots: step50 (look yaw 2.475, looking back) has
  |cam - air| = 35.69 vs 36.48 at rest [E]. The documented live band of the Draken (35.7..36.6 u, <= 12.8 deg off the forward axis,
  e6-anchor-free.md) is this look effect. Therefore **only look-centred samples are used**: |look pitch/yaw/roll| <= 0.005 rad and
  |wrap(rot yaw - heading)| <= 0.005 rad. The offline look-only Draken stream (199/200 samples inside that live band, all passing the locator
  checks) produces no decision window [T].

## 3. Separability of the 36 types

`research/world-render-data/aircraft_cockpits.json` (36 types of `plmodel/playerlist.txt`, `.pvb` eye points) [E]: 34 distinct eye points; the
only identical ones are f15c/f15e/f15s. Smallest distances between distinct eye points: e18g/m29a **0.1616**, f02a/f16c 0.224, su27/su37 0.300,
m29a/typn 0.506 (eye lengths alone would not separate them: su27/su32/su37 differ by 0.03-0.075 in length; the 3D vector does).

Decision rule: median within **0.08** units of a table eye (< 0.1616 / 2, so at most one distinct eye can match: no guessing between near
neighbours). A median 0.08 from both e18g and m29a is `no-match` [T]; 0.03 towards one side picks that side [T]. Noise budget: float rounding of
cam/air at |x| ~ 150,000 is <= 0.008 per axis; the median of 24 samples removes outliers (a window needs >= 75% of its samples within 0.3 of the
median, else `unstable`) [T: 0.03 u noise + 5% 0.25 u outliers + 10% look samples -> all 36 correct; 0.08 u noise -> all correct]. The largest
real residual seen (0.06, rounded log values) fits inside the tolerance; whether live residuals stay below 0.08 at high speed or under camera
shake is **unverified** [I].

## 4. Ties: F-15C / F-15E / F-15S_MTD

The three share one `.pvb` eye point, so the E6 offset cannot tell them apart [E]. The baked files show [E, `aircraft-table.md`,
`bake_all_aircraft.py` hashes]:

* f15e and f15s: **identical cockpit part** (positions within 3.8e-6 units = node-transform float noise, identical UVs, colours, normals, part
  records and cockpit atlas); for these two the tie does not matter inside the cockpit.
* f15c: a **different** cockpit part (3249 vs 3087 triangles, 79 vs 93 nodes, different atlas; only 45-66% of triangle centroids coincide within
  0.01-0.1 units).
* All three exteriors differ (e.g. conformal tanks, S/MTD canards; the vrcockpit shows part of the exterior from inside).

Rule (documented, logged as `tie-rule=`): a `prefer:` type inside the tie group wins (first in table order among the preferred); otherwise the
first in table order = **f15c**. So an F-15E/S player gets the F-15C cockpit unless `prefer:f15e` (or `f15s`) is given [I: an acceptable
default; the window logs `ties=f15c+f15e+f15s`]. A real tie-breaker would be the read-only memory fingerprint scan already proven for stages
(D-310/D-313: 32-byte pieces of mpl part 2/part 8 of the resident aircraft file) - not implemented here.

## 5. Detector and hysteresis (`acz::aircraft::Detector`)

* Input: every sample of the locked block (`EyeMailbox`: seqlock + 64-slot ring drained per tick since AIRCRAFT-AUTO-FAST; the consumer feeds each sequence once). Off-centre samples are
  counted and ignored.
* Non-overlapping windows of 12 centred samples (~0.2 s at 60 Hz; 24 before AIRCRAFT-AUTO-FAST). Verdicts: `match` (type after the tie rule), `no-match`, `unstable`,
  `ambiguous` (cannot occur while the tolerance is below half the smallest separation; kept as a guard).
* First choice: one matching window. With an assumed type (section 1a): a matching window confirms it; a change needs `confirm` windows; no-match never drops it. A change (another type, or `no-match` -> no model): two consecutive agreeing windows; `unstable`/`ambiguous`
  windows break a pending streak and change nothing.
* A new lock epoch (the locator lost and re-acquired the block: mission change/reload, e6-anchor-free.md section 3) discards the partial window;
  the previous choice stays drawn until two windows of the new data agree (no flicker, no stale mix). A lost lock alone never unloads [T].

## 6. Loading and fallback (consumer)

* `cockpit_auto_tick` runs once per composed frame before the cockpit block: finished load -> install; new sample -> detector; wanted type !=
  drawn -> start one load (worker thread via `std::async`; never inside a frame; one at a time: a changed wish is served after the running load).
* Install: `s_.cockpit` = the new runtime, a fresh generation (>= 2^20, unique per load; the D3D12 backend rebuilds its GPU copy on a generation
  change exactly like a resize), animation state reset, the usual `cockpit-model` / `cockpit-model-c4` lines.
* A model file missing/corrupt/unbuildable: `action=load-failed;reason=load-failed;status=io-error;path=...;fallback=replayed-cockpit`,
  never retried for that type this session; a wrong model still drawn is dropped.
* Key off or a fixed id: nothing of this exists (the Settings field is null; the probe publishes nothing: one cached flag test). Offline: an
  `auto` run without E6 lock produces exactly the key-off jobs and log lines apart from the `cockpit-auto` lines [T].

## 7. Bakes (`docs/world-render/aircraft-table.md`)

`bake_all_aircraft.py` bakes `<id>_cockpit`, `<id>_exterior`, `<id>_vrcockpit` for all 36 types one at a time from the extracted a.arc
(`E:\ACZ-ARC-Scratch\extracted-a\a\data`), byte-identical to the `model_baker.py` CLI (j35jk/m21bk/f16ck equal `models-c4`) [E]. Result: **36/36
baked, no baker failure** (no different part layouts or missing textures surfaced: every mpl has the 10-part layout, every vrcockpit 2 textures,
every cockpit atlas 128x128 with 35-95% opaque texels, the pvb eye inside every cockpit part's x/z footprint) [E]. All 36 load and build in the
DLL loader with the table eye (`ACZ_MODELS_ALL=D:\acz-extract\models-all aircraft_auto_offline_tests`) [E]; HUD glass found in all; canopy glass
from the exterior canopy in 35 (zoef: from its cockpit part); animated surfaces driven 0-8 per type (zoef 0/51: the ADF-01's animated nodes are
not standard control-surface groups). Visual correctness of the 33 aircraft never rendered before is **unverified** (no previews made here) [I].

Install: `pwsh -NoProfile -File tools/live/install-assets.ps1 -ModelDir models-all` copies the 36 `*_vrcockpit.zvrm` (`-AllBakes` also the
research bakes) and prints how many of the 36 `auto` models are installed.

## 8. Logs and live recipe (driver; null driver first)

`xr-stereo.log`: `event=cockpit-auto-config` (value, models folder), `event=cockpit-auto;state=armed` (window, tolerances, confirm, prefer),
per changed verdict `event=cockpit-auto;window=N;verdict=match;candidates=<best>:<res>,<second>:<res>;chosen=;model=;confidence=;ties=;tie-rule=;
samples=;spread=;eye=x,y,z;current=;switched=`, `action=load|installed|load-failed|discarded|unload`, and every summary
`event=summary-cockpit-auto;samples=;centred=;off-centre=;lock-epochs=;windows=;switches=;current=;drawn=;loading=;loads=;load-failures=;unloads=;
last-verdict=;last-best=`.

1. `pwsh -NoProfile -File tools/live/install-assets.ps1 -ModelDir models-all` (and the stage assets as before).
2. `vr-test-start.ps1 ... -TerrainRender 'auto;env:1;look:1;props:1;trees:0' -DepthFix -CockpitModel 'auto;anim:1;default:j35jk_vrcockpit' -PngOnce` (mission 02 =
   Draken).
3. Expect after the `e6-locate;event=locked` line (~3.5-4 s after skipping the mission opening, D-319): `cockpit-auto;window=1;verdict=match;
   candidates=j35j:<= 0.08` and `action=installed;type=j35j;model=j35jk_vrcockpit` within about a second; `summary-cockpit-auto` with
   `centred` rising in level flight and `switches=1`. Record the live `last-best` residuals (the tolerance check) and try one other aircraft
   in a later mission.
4. AIRCRAFT-AUTO-FAST: expect `cockpit-auto-remembered;...;assumed=j35jk_vrcockpit`, `action=assume;...;installed=yes` right after `state=armed`,
   `action=first-frame;...;model-drawn=yes;replayed-cockpit=dropped`, then (after the lock) `action=confirm;type=j35j` and, on the first run,
   `action=remember` (afterwards `%LOCALAPPDATA%\acz-vr\last-aircraft.json` names j35j). In the headset: the full model cockpit (sides, rear,
   interior) from the first frame, no 10 s of the forward-only replayed cockpit.

## 9. Unverified / inferences

* Live residuals (speed, camera shake, gun fire) vs the 0.08 tolerance; whether the look values are exactly 0 at rest in every aircraft [I].
* Whether a mission change re-locks the E6 block (expected `lost` then `locked`, new epoch) [I, e6-anchor-free.md].
* F-15E/S players get the F-15C cockpit by default (the eye cannot tell; `prefer:` fixes it per user) [E for the tie, I for the default].
* The 33 newly baked aircraft have never been looked at in the headset; the procedural interior, canopy glass and hinge fitting were tuned on
  three aircraft (COCKPIT-4) [I].
* `hudglass:1` is ignored with `auto`.

## 10. Proposed decision entry (for the coordinator)

D-3xx - AIRCRAFT-AUTO: `xr_cockpit_model=auto[;opt]` (no new key) detects the player aircraft from the locked E6 block: camera-aircraft offset
rotated back by Ry*Rx*Rz (fitted on real samples, residual <= 0.06), look-centred samples only (the look pivots the eye ~1 u), windows of 24,
median within 0.08 of one of the 34 distinct pvb eyes (half the smallest separation 0.1616), confirm-2 hysteresis, lock-epoch reset; F-15C/E/S
tie -> `prefer:` else f15c (E and S share the cockpit part; C differs); lazy worker-thread load, missing model = replayed cockpit. All 36 player
aircraft baked (D:\acz-extract\models-all, no failures, all load in the DLL), `install-assets -ModelDir models-all`. Release ctest 303/303
(D:\b-aa). Nothing live.

## Headset test 2026-10-28 item 11 / 4b: model unloaded on the airfield (drop_confirm)
[E] xr-stereo.log (step105, user session): on k00a (t=8896937..8937140) the detector saw no-match windows (j35j residual 0.08..0.153, eye y 3.43..3.56: ground wobble)
and `action=unload;reason=no-matching-type;fallback=replayed-cockpit` fired 4 times (8904531, 8906531, 8911656, 8920734), each followed by a reload on the next match: the
cockpit/airframe model was absent for ~15 of the 25 s, and the game's replayed cockpit (forward window only) showed instead = "plane stops rendering" + model/replay flicker.
[E] Samples with the look turned are not `centred`, so no window completes while the user looks back: an unloaded model can not be reloaded until the head faces forward.
No other counter moved with the view direction (cockpit no-view/backend-failed 0, world-composite refused 0, admission refusals 0).
[I] Fix: `DetectOptions::drop_confirm` = 25 consecutive no-match windows (a match resets); a different type still switches after `confirm` = 2. Tests: aircraft_auto_offline, xr_stereo_output_offline --cockpit-auto.
Unverified live. Capture plan if it persists: `cam:x,y,z,yaw` pose behind the aircraft at k00a with `cockpit-auto` events + `summary-cockpit` (model-frames == frames) while yawing the head 180 deg for 60 s.

## 11. Headset test 2026-10-29 item 9: wrong model after a mission change (AIRCRAFT-AUTO-STALE)
[E] User: 'The Round Table', F-5E chosen: VR drew the Draken with an F-5 cockpit. Findings from `D:\acz-evidence-archive\headset-2026-10-29-session`:
* The F-5E is NOT an unknown type: `f05e` is table entry 7 (eye 0, 3.97, -21.9; len 22.257, inside the locator's 15..60 band) and `f05ek_vrcockpit.zvrm` is baked
  (`D:\acz-extract\models-all`) and installed in `%LOCALAPPDATA%\acz-vr\models`. No profile/fingerprint was missing; no `f05e` window appears in any log.
* Cause: `ee-camera.log` shows the E6 locator `lost` (eye-distance) at 100542868 and then only `search` lines (candidate `static`, then `aircraft-at-origin`)
  for the remaining ~560 s: **no lock, no E6 sample, no detector window**. Every earlier lock had `eye-dist=36.476` (Draken). The detector never ran, so the
  remembered/earlier Draken (`last-aircraft.json` = j35j, `current=j35j` in every summary through the end) stayed drawn: "a lost lock alone never unloads" (section 5).
  Why the block stayed `static`/at-origin in that mission is [I] unresolved (not analysed here; a later lock would have matched f05e).
* Rule chosen (prefer showing the game's own cockpit over a wrong model): an assumed (remembered / `default:`) or earlier-confirmed type is trusted only while the
  locked block keeps delivering samples. `stale:<ms>` (default 15000, 0 = never, max 600000) with no sample = `action=stale-forget;...;fallback=replayed-cockpit`
  (`Detector::forget`, model unloaded); the next matching window chooses the real type with one window. Samples of a running lock (matching or no-match) refresh the clock,
  so airfield wobble / looking back never trigger it. Cost: after a long no-lock gap (menu/debrief, > 15 s) the next mission shows the game's cockpit until the lock
  (the old 9 s case) instead of a possibly wrong model; use `stale:0` to restore the old behaviour. Summary: `stale-forgets=` in `summary-cockpit-auto-start`.
* Adding an aircraft: types come from `playerlist.txt` -> `research/world-render-data/aircraft_cockpits.json` + `kAircraft` (aircraft_auto.cpp: type, model stem, pvb eye,
  stick style), bake with `research/world-render-data/bake_all_aircraft.py`, install with `tools/live/install-assets.ps1 -ModelDir models-all`. A type with no table entry
  is not matched (`no-match`) and now shows the game's cockpit after the stale limit.
Tests: `aircraft_auto_offline` (stale: parse, `forget`), `xr_stereo_cockpit_auto_offline` (forget -> replayed cockpit -> f05e loaded; running lock keeps; stale:0).

## 12. Headset test 2026-10-30 (step134): F-5E chosen on m03k, "stayed in draken" (E6-JUNK-LOCK)
[E] How the type is identified: only by the E6 eye offset (camera - aircraft in the aircraft frame, section 2) matched against the 36 table eyes (`Detector`); there is no aircraft-id
field and no model-table lookup by mission. The remembered file (`last-aircraft.json` = j35j) and `default:j35jk_vrcockpit` only ASSUME a model until a window matches.
The F-5E is table type `f05e`, `f05ek_vrcockpit.zvrm` is baked in `D:\acz-extract\models-all`, and `Detector` re-evaluates every window (a different type switches after 2 windows), so
the type logic is not the fault: it never received the F-5E's eye.
[E] Cause (`D:\acz-evidence-archive\step133\batchAG\run*\logs\ee-camera.log` + `xr-stereo.log`; the user's live-1508 logs were not archived yet): the step134 locator scan locked JUNK,
not the aircraft. `locked;...;kind=s` lines: `air=0.00,0.00,60.00;heading=0;cam=0.00,0.00,0.00;rot=0;eye-dist=60.000` (B1, B2, C1, U1, F1: 1-3 locks per run) and
`air=-0.20,22.39,25.11;cam=0,0,0;eye-dist=33.64` (moves=63, B2/U1/F1): world-origin test structures that pass `check_sample` (the aircraft is "off the origin", eye 60 = the
upper bound) and move a little. Their samples reach the detector as `window=1 verdict=no-match eye=-0.000,-0.000,-60.000`, 976..1746 windows, `last-best=mrgn:8.498`, `switches=0`,
`current=j35j` = the assumed remembered Draken (an assumed type is never dropped by no-match). The real structure (+0xCA811C / +0xCAAC6C) never became a candidate: every `struct-scan`
reports `hits=12;dropped=43..2491`; the 12-slot cap kept the FIRST 12 hits in address order (junk at block+0x2BA49C..0x33AAA4, ahead of +0xCA...).
[T] Fix (`ee_e6_locator`): (1) `check_sample` rejects `camera-at-origin` (|cam x|+|cam z| <= 1; mirror of aircraft-at-origin; real cameras are within 60 units of the aircraft, never at
the origin); (2) the scan keeps the BEST 12 hits per pass, not the first 12: `hit_score` 0..4 (heading non-zero, rot yaw non-zero, rot pitch within 1.6, look near rest; real = 4, zero/denormal/2.0-filled
junk <= 2), a full list replaces its lowest score; hits are still reported in address order; `dropped` = hits - kept. Tests (`e6_locator_offline`, `test_junk_lock`): 300 quiet junk hits before the
F-5E structure (BEFORE with the HEAD source and `-DOLD_API`: real structure not in the list, no lock; AFTER: kept, locks `kind=s` eye 22.26 in < 4 s), the two logged junk samples rejected,
`hit_score` / `insert_best`. Not changed: assumed models still never drop on no-match within a lock epoch (W5-3 P2 #1).
[I] Unverified live: whether the user's session had the same junk (its log is the confirmation); junk with non-zero cam and angles that scores like the real structure is still only refused by the
Locator's moves/ambiguity rules. Check on the next null m03k run: `locked;kind=s` with the flown eye (no `cam=0.00,0.00,0.00`), `cockpit-auto` windows with `verdict=match`.

## 12. Headset test 7 (2026-10-31, step160, F-5E): the aircraft and cockpit vanished for 8.4 s; the missiles are missing on the racks (AIRCRAFT-AUTO-GRACE, STORES)
Evidence: `D:\acz-evidence-archive\headset-test7-20261031\xr-stereo.log` + `ee-camera.log`.
* **Vanish [V].** `summary-cockpit-auto` shows `samples=0` from the start (the locator had not locked), `current=f05e;drawn=f05e` to t=75 s, then `current=none;drawn=none;unloads=1`
  at t=80 s and 85 s, `current=f05e` again at t=90 s (`loads=2`). Events: `first-frame` 236876375 (frame 5548), `action=stale-forget;type=f05e;was=assumed;silent-ms=15017;limit-ms=15000;
  reason=no-e6-samples(locator-not-locked)` 236891390, `action=unload` same ms, window 1 `verdict=match;candidates=f05e:0.012` 236899828, `installed` 236899859: **8.44 s with no model**.
  `summary-cockpit` has no lines while unloaded and `model-frames` is ~450 per 5 s before and after, so nothing else dropped. The game's own replayed cockpit (forward view only, band dropped
  with the model) is all that remains: the user sees the airframe, wings and every cockpit part beyond the game's own cockpit disappear.
  Why: the E6 locator needs ~23 s of flight to lock (`ee-camera.log`: candidates 236873955, struct-scan passes every ~5 s with `static` candidates, fallback-gate qualified 236880676,
  `e6-locate;event=locked` 236899658, 8 samples/7 moves), longer than the 15 s `stale:` limit, and the clock starts at the first composed frame. The remembered type was RIGHT.
  The previous run (`xr-stereo.prev.log`, same build) shows the same stale-forget at +15.0 s (was=confirmed, back after 0.4 s).
* **Fix.** `grace:<ms>` (default 60000, 0..600000; `AutoConfig::grace_ms`, `CockpitAuto::grace_ms`): a model that is only ASSUMED (`Detector::assumed()`, no window confirmed it)
  and whose block has delivered no implausible sample is trusted for max(stale, grace). Confirmed models and junk-lock cases keep the short `stale:` limit (D-402 junk rule unchanged).
  `limit-ms=` in the stale-forget line now shows the limit applied. Cost: a wrong remembered aircraft (the user switched type) stays up to 60 s until the first window instead of 15 s.
  Tests: `xr_stereo_output_offline` (5c: 24 s lock keeps every job's model; no lock forgets at 60 s with `limit-ms=60000`; confirmed-then-silent forgets at 15 s; junk still 15 s) and
  `aircraft_auto_offline` (grace parse). The 5c test fails against the 15 s code.
* **Missiles [V for cause, I for placement].** The racks are in the airframe (mpl part 0: pylon plate nodes n66/n71/n78, groups 53/54/55, weapon-station locators with group ids 65/66 tips,
  67/68, 71..74, 75/76 under the wings). The weapons are NOT in any `.mpl`: they are separate per-aircraft models in `data/mis/mpl/<type>ex.bin` (a container of 3 weapon containers
  [16-byte header, high-detail ACM, low-detail ACM, hull ACM]; f05eex.bin: 3.0 m AAM with seeker ring and fins 161 tris, plus two larger stores) textured with the shared 128x64
  atlas `data/lodweapon/weapon.gim`. `bake_vr_cockpit` carries mpl parts 2 and 0 only, so the VR model had racks and never a missile (the part-7 tank is not baked either).
  Fix (offline bake, no runtime change): `model_baker.add_stores` + `bake_all_aircraft.py STORES` add weapon 0 of the pack at the locators of the drawn pylons (`f05e`: groups 67, 68, 75, 76),
  placed WITHOUT a turn (the weapon models are authored nose at -z, fins/nozzle at +z, like the aircraft; the first bake turned them 180 degrees about y on a wrong comment and every MSSL and SAAM missile pointed backwards: headset test 8, fixed in the fix8 bake), as exterior parts (`MF_EXTERIOR`, hidden by `ext:0`, source part 7, third texture slot).
  f05e becomes 146 parts / 5,925 baked triangles (676 weapon triangles; loader `tris=6471 drawn=95` vs 5795/87), 3 textures (GPU limit 4). Preview (textured, from below):
  four Sidewinder-shaped missiles hang under the pylons with the seeker ring forward. Bake: `python -I bake_all_aircraft.py <a/data incl. lodweapon/ and mis/mpl/f05eex.bin> <out> --only f05e`
  (`--no-stores` reproduces the old file byte for byte; that equals the installed f05ek_vrcockpit.zvrm). Tests: `aircraft_auto_offline` with `ACZ_MODELS_ALL=<dir>` (f05e: 8 drawn weapon
  parts under the wings, both sides; fails on the old model) and `test_model_baker.py` (Stores).
  [I] Not decoded: which weapon the game mounts per station for the selected loadout (the three pylon plate variants 53/54/55 suggest three loadouts; the model shows the full rack with the AAM
  everywhere), that the missile leaves the rack when fired (the rack stays loaded; needs the ammo/launch signal), the other aircraft (add a `STORES` entry per type), and the unbaked belly tank.

## 13. STORES-RT (2026-10-31): fired weapons, the other aircraft's stores, the F-5E belly tank
Follow-up of section 12 gaps 1-3. Branch work only: nothing installed, no game or SteamVR run. [V] = checked offline here, [I] = inference, [U] = only a live run can confirm.
* **Gap 1, the launch signal: no reliable source exists in the evidence we hold [V].** Searched: the E6 structure the camera feed reads (`ee_e6_locator.h`: look/aircraft/heading/camera/rotation words only, no ammo or weapon field decoded
  anywhere in `docs/` or `src/`), the headset-test-7 `ee-camera.log` (the `diff-top` lines track angle-like words only; every `hexdump` is the all-zero decoy block, none is the locked structure), the firing wedge (D-318: the same draw
  shows for the gun AND for a missile, so it cannot tell them apart; the debounced `fire-events` of `tactical.h` has the same limit) and the missile smoke records (`fx_smoke.h`: every missile's trail, the enemy's included, and only
  with `fxmem:1`). No log has a missile launch next to a memory window, so the HUD MSL counter's address is unknown. Nothing is guessed: with no signal the baked missiles stay on the racks (as before).
* **What is built (the machinery a signal plugs into) [V offline].**
  * `cockpit_model` (`stores:0|1`, default 1): the baked weapon parts (`<model>/store<station>/n<node>`, source part 7) become ONE `AnimPiece` per weapon station (`AnimRole::store`, `Mesh::store_stations`, pivot = the
    station's vertex centroid) in the existing animated range, so the one per-job upload of that range already carries them. `apply_store_mask(mesh, hidden_bits, posed)` collapses every set station to its pivot (zero-area
    triangles, nothing rasterised, range size unchanged); `pose_stores_only` does the same for `anim:0`. A weapon part is never taken for a control surface whatever its node group (the aileron groups 5/6 exist on weapons).
    Blended or hidden weapon parts stay static. Models without weapon parts are unchanged (no pieces).
  * `store_tracker.h` (RELOAD model, step173): `Tracker` (`launch(now_ms)` empties the next SHOWN station in a made-up firing order [alternate right/left wing, outboard first, I] for `reload_ms`
    and it then shows again; `set_remaining(n)` caps the shown stations at n [the earliest in the firing order are held]; `rearm()` shows all). Like the game: the pylon missile vanishes on firing and a new one
    appears on the same pylon after a short reload while missiles remain. Option `strel:<seconds>` (0..60, default 3.0): **the 3.0 s default is an ESTIMATE**, no doc or evidence has the game's real pylon
    reload time (measure it from a gameshot series: the frame the pylon missile reappears after a tap). The MSL-HUD reader also sends the absolute count (`set_remaining`) whenever the reading changes.
    The old tracker kept a fired station hidden for good (step172: `hidden-mask=0xf` after 4 taps) and `Signal`, the lock-free process-wide mailbox
    (`acz::stores::signal().note_launch() / note_rearm() / set_remaining(n)` from any thread). The XR output (`xr_stereo_output.cpp`, cockpit job) syncs the tracker per cockpit generation, poses the range with the mask
    (sticky once any station was hidden, so the range returns to rest on rearm) and logs `xr-stereo;event=summary-stores` every summary window **unconditionally while the model has stations**:
    `stations=N;hidden-mask=0x..;fired=..;launch-signals=..;source=none(no-launch-signal-known)|signalled|off(stores:0)`. Rearm/respawn: `note_rearm()` (or `set_remaining(total)`); a new model discards old signals.
  * Nothing calls `note_launch` yet: the mask stays 0 and the image is bit-identical to the section 12 bake.
* **What a null (or headset) run must log to find the signal [U].** `xr_cockpit_model=auto;...;stwatch:1` (parsed by `parse_options`; a bad value refuses the model like any option) turns on `ee_store_watch.h`:
  while the E6 locator is locked, every 250 ms it reads 32 KiB of the SAME block around the E6 structure (read-only, `ReadMem` has no write) and logs
  `store-watch;event=decrement;off=<signed bytes from the structure base>;addr=..;old=N;new=N-1;changes=..;t-ms=..` for every 32-bit word that counts down by one inside 1..99, and `event=rise` for such a word that later jumps up
  (rearm); words that change more than 24 times (timers, positions) are muted; at most 200 lines; `store-watch;event=summary` every 10 s (reads, unreadable, rebases, muted words, decrements) so a quiet watch is told from a dead one.
  Add `tac:1` to the terrain option: `xr-stereo;event=store-watch;kind=fire-wedge;state=on|off;t-s=..` marks every firing-wedge edge (gun or missile). Fly a mission that FIRES MISSILES (the null kill batch AGB2 does:
  `runBatchAG` FIRE decisions; the plain batches never fire) with a screenshot or note of the HUD MSL count and the launch times. The counter is the word whose `old->new` steps equal the missiles fired, one step per launch (a gun burst must
  not move it); its `off=` (relative to the E6 structure, which moves between missions: relocated structures are followed through `locked_base`) is the signal: read it in the probe tick (`ee_camera_probe_win.cpp`, next to the watch call) and call
  `acz::stores::signal().set_remaining(<missiles left>)`. If no word decrements, the counter is outside the 32 KiB window or not a plain int: widen `kSpanBefore/kSpanAfter`, or fall back to the fx-smoke record of the player's own missile (a new
  trail whose first point is within ~30 u of the camera). The wiring decision belongs after that run.
* **Wide counter search (2026-10-31, after null run AGB2: HUD MSSL 50 -> 46, four taps, no 32 KiB-window word followed) [V offline, U live].** `ee_counter_search.h`, on with `stwatch:1`, runs on the probe WORKER thread (`e6_background`;
  the Present tick only publishes the locked block base through an atomic): it scans the WHOLE 64 MiB E6 block (the block holds the emulated EE RAM; `ee=` in the log reads the block offset as an EE address, valid only if the EE RAM starts at the
  block start) once per second (1 MiB chunk reads through `E6ReadMem`: VirtualQuery + SEH, read-only, no lock). Phase 1 seeds every aligned u8/u16/u32/float position equal to `stwatch-start:<n>` (default 50, the HUD count at the first lock;
  re-seeded every 5 s for 30 s while nothing has moved). Phase 2 keeps a candidate only while it is unchanged or falls by 1..2 per scan (50 -> 49 -> ... -> 46); anything else kills it, and 4 s after the first decrease the never-moved ones
  are dropped. Log after each scan that moved something (max 40 lines, 8 offsets each): `store-search;event=survivors;scan;t-ms;target;alive;moved;groups;list=<block off hex>(ee=..|rel=<from the E6 structure>|w=124f|v=46|d=4);...`,
  and `store-search;event=summary` every 10 s. Memory: <= 2M candidates x 8 bytes. `runBatchAG -Analyze <run> -Kind stores` now prints the survivors and PASSes the ones whose decrease equals the taps (`Analyze-StoreSearch`).
  **Wiring (`stctr:<hex block offset>,<1|2|4>`, e.g. `stctr:1A2B3C,4`; unset = unwired, as before):** the Present tick reads the counter, `LaunchDetector` (stable >= 300 ms and 2 reads = hysteresis; first stable value = baseline; fall 1..8 =
  that many `note_launch()`, a rise = `note_rearm()`, a bigger fall = rebaseline) feeds `acz::stores::signal()`; `store-counter;event=launch|rearm` lines (max 100). `stores:0` disables it. The address is not baked in as a default because it
  is not known yet: after the live run, confirm the survivor against the gameshots, repeat in a second run to prove the offset is stable (block-relative or E6-relative), then set the default in `Cfg`/the runtime cfg.
  Run: `-Only boxes -FireSalvo 4 -FireEvery 7 -FireAt 70 -CockpitAppend ';stwatch:1;...' -AllAppend ';vrn:1'`, then `-Analyze <runAGB2> -Kind stores`.
* **Gap 2, the other aircraft [V offline, I for the loadout].** `bake_all_aircraft.py STORES` now lists 24 types (j35j and f05e checked by preview, f05e hand-set; the rest from `--propose-stores`: `model_baker.find_missile_index` takes the first pack
  entry that is a 40..300 triangle, >= 4.5:1, <= 4.5 u wide weapon (an empty first entry, e06b, is skipped); `station_candidates` places it at every weapon locator 67..76 exactly as `add_stores` does and counts pylon-plate vertices inside its box;
  `choose_stations` keeps mirrored wing PAIRS whose box is not inside a rack (> 30 plate vertices) and does not overlap an already chosen station, at most 4; centre-line and wing-tip locators are never proposed).
  The Draken (the null batches' aircraft) gets stations 71..74: two parallel missiles per wing beside the wing tank (8 drawn parts, 440 triangles, 3 textures). 12 types get no stores (internal weapons, no missile-like entry, or
  every locator inside a rack mesh): e06b e18g f02a f16c f20a f35c j39c m21b mrgn su32 tnd4 x02a; their baked files are byte-identical to the installed ones. [I] The weapon is the aircraft's own first slim pack entry: on some types it looks like a
  bomb or a big ASM (mr2d, m31b), and which loadout the game mounts per mission is not decoded. Previews (textured, from below): `D:\acz-extract\models-stores-test\previews\pv_<type>.png`, sheets `sheet[A-D].png`.
  Baked files (CRC32 of the whole file; 24 changed of 36, the other 12 equal the installed ones): a10ak B0DF7695; f01ak 8085D81C; f04ek C65A4DF6; f05ek 55DE11B1; f14dk EA202B45; f15ck 41050DB4; f15ek 269F86C6; f15sk AC98BE3F;
  f16xk 6D5C6DF1; f17ak 0C45D71D; f18ck 8145378B; fa22k 9EF5B54F; j35jk 6DB00DD3; m29ak 9A4BBB10; m31bk 9B95A217; mr2dk 4ED4D562; rm01k BFA854EB; su27k FFEF0D8C; su37k 3F0EC03E; su47k 0A64E25D; typnk 41D47941;
  x29ak 442F4803; yf23k F4C06252; zoefk AD0688B1. Reproduce: `python -I bake_all_aircraft.py <a/data incl. lodweapon/, mis/mpl/*ex.bin, plmodel/> <out>` (`--no-stores` = the pre-STORES files). Not installed anywhere.
* **Gap 3, the F-5E belly tank [V offline, I for 'the game mounts it'].** mpl part 7 (13,296 bytes: one node, 216 triangles, 5.9 x 4.2 x 26.7 u) is baked once at the centre-line locator (group 79, (0,-4.6,-8.6)) as
  `<model>/tank79/n0`, an exterior part with the airframe skin texture (no extra texture: f05e stays at 3), source part 7 (`TANKS` in `bake_all_aircraft.py`, `model_baker.add_tank`). Orientation: the authored frame already has the nose at -z
  (with the weapons' 180-degree turn the tail fins sit at the front in the preview); y -6.3..-2.0, z -21.9..4.8 in the baked model. It is named `tank`, not `store`, so the weapon mask never hides it. f05e is now 147 parts / 6,141
  triangles. The j35j part 7 (two belly pods, group 79, 356 triangles) previews right too but is NOT baked (not asked; whether the game shows it by default is unknown).
  `cockpit_model` ignores `source_part == 7` parts in the cockpit-clip skin fit (`kStoresSourcePart`): a belly tank under the pilot is not airframe skin.
* **Tests (the new ones use APIs that did not exist at 16e39b6f, so they cannot pass there).** `cockpit_model_offline` (`test_stores`: station names, options, one piece per station, pivots, mask 0 = rest bit for bit, collapsed triangles
  have zero area, surviving ones keep theirs, wrong-size/out-of-range masks do nothing, `pose_stores_only`, `anim:1` with the group-5 fin not an aileron, blended/hidden parts excluded, no-weapon model unchanged; the clip fit ignores a source-7 part
  inside the cockpit slice but counts the same quad as source 0; `test_store_tracker`: firing order, launches, empty rack, rearm, absolute count, mailbox ordering/discard; `test_store_watch`: counter 4->3->2 and the refill, a timer muted,
  never-counter jumps ignored, unreadable window skipped without losing the baseline, rebase, unconditional summary, the 200-line budget, no read when not started or not locked), `aircraft_auto_offline` with `ACZ_MODELS_ALL=<baked dir>`
  (36/36 load + build; f05e 4 stations + 8 weapon parts + the tank + 3 textures; j35j 4 stations; e06b/f16c none; every mesh's store pieces equal its stations) and `test_model_baker.py` (missile entry choice, pair/overlap/rack rules,
  plate counting, tank placement/orientation/naming/errors).
* **[U] Only a live run confirms:** that the racks/tank/missiles look right in the headset and in the game's own proportions, the real per-mission loadout, the launch signal (nothing hides a missile until it is wired), the firing order,
  that the per-frame range upload (anim range, `copyBuf_`) is cheap with the mask active on the real GPU (the same path the animated surfaces use), and the `stwatch` lines on a real firing flight.

## 14. MSL-HUD (2026-11-01): the missile count read from the game's own HUD text
Branch work only: nothing installed, no game or SteamVR run. [V] = checked offline here, [I] = inference, [U] = only a live run can confirm.
* **Why [V].** Three EE-memory searches failed: step167 (32 KiB window, nothing), step168 (u8 table at block+0x67D886, stride 0x98, 50 -> 49 before any launch),
  step170 (`D:\acz-evidence-archive\step170\kill\runAGB2`: that table went 46, 45, then 237; no survivor followed the 4 taps). The HUD itself shows MSSL 50 -> 46 for a
  4-tap salvo (`step167\salvo\runAGB2\gameshots` g0070 vs g0097). The HUD text IS the count.
* **What the existing decoders read [V].** Nothing decodes glyph texture coordinates anywhere: `tactical_producer.h add_labels` and `marker360.h label_of` only COUNT the
  glyph quads of a marker label (4 = FRND in blue, 3 = TGT) from their GS positions and colour; the HUD instrument text (SCORE/TIME/ALT/GUN/MSSL/...) is never read on the
  CPU. No log has ever recorded a HUD glyph's uv, so the font atlas layout is unknown.
* **The block [E g0070, GS = 1728 + 640 px/2405, 1824 + 448 (py-30)/1323].** Four rows at the lower right: `GUN ----` / `MSSL --50` / `<special> ---8` / `DMG --0%`, rows
  ~21 GS apart (MSSL y ~2150), each value field 4 glyphs, dash padded, right edges aligned (x ~2345); the selected weapon's field is boxed and drawn with larger glyphs.
* **Built (`hud_msl.h`, producer `gpu_copy_step5_hooks.inl` step5Msl*) [V offline].** Every 6th produced frame the HUD region's instrument draws (not marker boxes/labels,
  not the late text) are READ like the tac sprites (mapped UPLOAD memory + guardedCpuRead; nothing replayed differently; <= 64 draws / 6144 vertices per sampled frame):
  ps2_v position +0, COLOR0 +16, TEXCOORD +32; triangles join vertices into components; a component of 4..6 vertices, 1.5..48 GS, textured = one glyph (GS box + uv box).
  The decoder takes the glyphs in the region x > 2112, y > 2072, groups rows, keeps rows whose last 4 glyphs are evenly spaced (the value field) with a label to their left
  (gap <= 4 advances, then <= 1.6 between label glyphs, so pitch-ladder numbers in the same row are not label), and needs EXACTLY 4 fields with aligned right edges and a
  regular row pitch. Row 2 is MSSL; checked: its label is 4 glyphs with the middle two equal (M S S L), GUN and DMG labels are 3 glyphs. The special weapon's label
  (SAAM, QAAM, XLAA, UGBL, LAGM, FAEB / RCL, GPB, TLS, EML, SOD) is never read and never drives anything; only the MSSL row drives the rack mask.
  **Digits [E step171, section 14.1].** A fixed table of the game's HUD value font: one atlas row (v0 = 3168/4096), quads 112/4096 wide on a 128/4096 pitch,
  digit d at u0 = (1552 + 128 d)/4096, '-' at 1264/4096, '%' at 496/4096 (`hud_msl.h kFont*`, `digit_of`; +-8/4096 row and +-0.15 pitch tolerance). Fallback for
  another font: the DMG ones digit while damage is 0 (8 equal blocks in a row) gives the zero and the row, same pitch. Anything else: `why=digit-not-in-table`, nothing
  signalled. The anchor is fixed with the first baseline.
  **Signal.** Stable >= 300 ms and 2 reads (the FIRST baseline >= 1000 ms and 3 reads); a fall of 1..8 = that many `acz::stores::signal().note_launch()`; a rise =
  `note_rearm()`; a bigger fall = rebaseline that still counts ONE launch (one stable change of the count = one launch at 10 Hz sampling). Latency ~0.7 s.
* **Options.** `xr_cockpit_model=...;mslhud:0|1|2`: default 1 = ON since the step172 live pass (section 14.2; it was opt-in until then); `mslhud:0` = the off switch;
  `stores:0` turns it off whatever mslhud says; no cockpit model option at all = off; `mslhud:2` = on and adds
  the glyph dumps: `msl-hud-dump;dump=n;frame;t-ms;scope=all|region;glyphs;...;why;value` then `msl-hud-glyph;dump=n;first=i;g=x0,y0,x1,y1,u0,v0,u1,v1(x4096),rgb,q,draw|...`
  (the first 3 dumps list every glyph of the frame = the HUD font in use; then the region only; one dump per 2.5 s, at most 48).
* **Logs (step5-producer.log, prefix `gpu-copy-step1;event=`).** `summary-msl-hud` every 5 s, unconditional while enabled: `frames;glyphs-last;cand;y-rows;fields;aligned;
  blocks;readings;value=<stable>|none;last;sp;dmg;anchor;dash;zero;dmg-zero;labels=a/b/c/d;launches;rearms;rebases;tuple-changes;why=<refusal>:<n>,...`.
  `msl-hud;event=launch|rearm|rebase;old;new;launches;t-ms` (max 100; t-ms = GetTickCount64, the autopilot tap clock). `xr-stereo.log summary-stores` gains
  `msl-hud=on|off|on+dump|not-started;msl-hud-value;msl-hud-launches`.
* **Tests [V].** `hud_msl_offline` (synthetic frames laid out from g0070 with an ASCII-grid atlas: quad extraction from indexed strips with cuts / indexed lists /
  non-indexed lists, rejects; the block with distractors incl. a ladder number in the MSSL row; every special-weapon label; 1..3-digit counts; the larger selected glyphs;
  refusals: all dashes, 3 rows, a 3-glyph MSSL label; the 4-tap salvo 50 -> 46 with launch events 0.3..1 s after each tap, a garbage frame and a missing HUD ignored;
  rearm, a 2-step fall, a >8 rebaseline; a non-ASCII digit row refused by the ASCII anchor then read through the DMG anchor; option parse; dump -> log -> replay round trip).
  `cockpit_model_offline`: default 1 (on), `mslhud:0|1|2` accepted, 3 / empty / 12 / on refused; `strel:` 0..60 accepted, -1 / 61 / empty / x refused.
* **Replay tool.** `hud_msl_offline_tests --replay <run>\logs\step5-producer.log` prints per dump the block rows (label and value glyph keys `uuuuvvvv` and cells) and the
  reading; for the full-frame dumps every glyph by row with uv, key and cell: with the known HUD text (SCORE, TIME, SPEED, ALT, GUN, MSSL, DMG, COVER and the digits)
  this gives the real font table if the ASCII assumption fails.
* **Next null run [U].** `pwsh -Command "& 'research\experiments\step83-verify-run1\runBatchAG.ps1' -Only boxes -FireSalvo 4 -FireEvery 7 -FireAt 70 -CockpitAppend ';mslhud:2' -AllAppend ';vrn:1'"`,
  then `-Analyze <runAGB2> -Kind stores`: the MSL-HUD block prints the value sequence (expected 50 -> 49 -> 48 -> 47 -> 46), each `msl-hud;event=launch` with its delay
  after the tap, and PASSes when launches = taps, every tap matched within 2.5 s and the last value = first - taps. If it never decodes, replay the dumps.
  Not proven: the glyph layout (draw class, topology, uv convention, the atlas order), that the HUD draws are instrument draws in the HUD region in every view mode,
  and the cost on the real GPU frame (expected small: reads only, 10 Hz).

### 14.1 Step171 null salvo run (2026-11-01, `;mslhud:2`): wrong digits, fixed from the dumps [E offline, U live]
* Evidence: `D:\acz-evidence-archive\step171\kill\runAGB2` (48 glyph dumps in `logs\step5-producer.log`, all before the salvo; gameshots). Taps at ticks 292945484,
  292952484, 292959468, 292966484. Gameshots g0066-g0100 (crop of the MSSL box): 50 until 02:17:50.8, 49 from 02:17:51.8, 48 from 02:17:58.9, 47 from 02:18:06.0,
  46 from 02:18:13.2: **all four taps launched**, one HUD step each (the 48 flashes red at 02:18:04-05: a warning colour, same glyphs).
* Logged by the first build: start value 60, `rebase 60->49` (+0.47 s after tap 2), `launch 49->48` (+0.50 s after tap 3), `launch 48->47` (+0.47 s after tap 4), end 47,
  launches=2, `tuple-changes=4` (the raw MSSL glyph tuple did change 4 times). Timing right, digits wrong.
* Replay of dump 1 (`hud_msl_offline_tests --replay`): block found (4 aligned fields, labels 3/4/4/3, label glyphs on atlas row v0 ~ 0, value glyphs on row 0.7734);
  MSSL `--50` = dash u0 1264, '5' 2192, '0' 1552; SAAM `---8`: '8' 2576; DMG `--0%`: '0' 1552, '%' 496 (uv x 4096). 0/5/8 sit on one 128-unit pitch from 1552; the quad
  is 112 wide. The ASCII-cell guess (cell = u / quad width) therefore read 5 -> 6, 6 -> 7, 7 -> 8, 8 -> 9 and refused 9: the true 50, 49, 48, 47, 46 became 60,
  refused, 49, 48, 47 (48 -> "49", 47 -> "48", 46 -> "47"), exactly the logged rebase 60->49, 49->48, 48->47. So the table is confirmed for 0, 5, 8 by the dump
  and for 4, 6, 7, 8 by the live events (only 1, 2, 3, 9 rest on the uniform pitch).
* Fixes: the fixed font table above (all 48 dumps now read value 50, SAAM 8, DMG 0, anchor table); a fall > 8 counts one launch (the misread 60 swallowed tap 2);
  the first baseline needs 1 s / 3 reads. Regression `hud_msl_offline` `test_step171`: dump 1 embedded verbatim must decode 50/8/0; its MSSL glyphs re-textured with the
  table for 49..46 decode those values; the salvo on the step171 tick clock gives 4 launches 0.3..1 s after the taps, end 46; a stable 60 then 49 = 1 launch + rebase.
* [U] The next `;mslhud:2` salvo run must show `value=50 -> 46` with 4 launch events (dumps are now every 5 s plus one at every msl-hud event, so the
  48-dump budget covers the salvo: in step171 it ran out 7 s before tap 1).


### 14.2 Step172 null salvo run: PASS, default ON, reload model (2026-11-01) [live null, E offline]
* Evidence: `D:\acz-evidence-archive\step172\kill\runAGB2` (`;mslhud:2`, 4 taps 7 s apart): the reader gave `value 50 -> 49 -> 48 -> 47 -> 46`, one `msl-hud;event=launch` per tap,
  0.42-0.50 s after each tap. This is the live pass section 14 asked for, so **`mslhud` is now default ON** (`mslhud:0` = off, `stores:0` = off, no cockpit model option = off).
* Problem found in the same run: the old tracker hid each fired station for good (`hidden-mask=0xf` after 4 taps, hidden until a rearm). Ace Combat shows the pylon missile vanish and a new one
  reappear after a short reload while missiles remain.
* Fix (`store_tracker.h`): `Tracker::launch(now_ms)` empties the next shown station (alternating order kept) for `strel` seconds (default 3.0, **an estimate**: nothing in the docs or evidence
  has the real value), then shows it again; `set_remaining(n)` (now sent by the HUD reader on every change of the reading) keeps only `n` stations shown; `rearm()` shows all.
  `Signal::sync(tracker, now_ms)` stamps launches with the consumer's clock.
* Tests `cockpit_model_offline test_store_tracker` (they cannot compile against the old API: `launch()` had no time): the step172 salvo (4 shots 7 s apart, each station back before the next
  shot, order 67/68/75/76/67); rapid fire 1 s apart (3 stations empty at once, each back 3 s after its shot); remaining 2 of 4 = 2 stations hidden, 1 -> 3 hidden, 0 -> all, rearm -> all; `strel` 0 and 10 s;
  the mailbox with times. `hud_msl_offline test_option`: no token = on, `mslhud:0` off.
* [U] Next live: confirm in gameshots that a pylon missile reappears about `strel` seconds after the tap (adjust `strel`) and that only `remaining` pylons show at 2 or fewer missiles.


## 15. SP-STORES (2026-11-01): the special weapon on its own pylons, its own tracker
Branch work only: nothing installed, no game or SteamVR run. [E] = evidence read here, [V] = checked offline here, [I] = inference, [U] = only a live run can confirm.
* **Which ex.bin weapon is which special weapon [E for the Draken order, I for the rest].** `data/mis/mpl/<type>ex.bin` holds THREE weapon models per aircraft (pack headers are the same
  four floats for every entry, `weapon_datas.bin` is 20 rows of 5 flags, `amparam.txt` is the supply timer: no weapon names anywhere). The hangar's PLAYER weapon screen of the Draken
  (`step124\runK10\logs\png\acz-vr-xr-screen-t79569-screen.png`, rotated 180 degrees) lists `MISSILE 50 / SAAM 8 / UGB -- / RCL --` and three icons (SAAM selected, a bomb, a cylinder).
  The Draken's entries 0/1/2 render (previews `models-stores-test2\previews\pv_j35j_slot[012].png`) as an olive missile with suspension lugs (node 2), a black bomb with a yellow ring
  and a 1.4 m rocket: SAAM, UGB, RCL = the hangar list order. [I] So **entry i = special weapon i of the hangar list**; the standard MISSILE has no ex.bin entry
  ([I] `data/lodweapon/wz?1_ms.bin`, four models, not extracted here). SAAM is the first special of the Draken AND the F-5E (the hangar default; the HUD example `SAAM ---8`), so
  **SAAM = entry 0**. F-5E entries: 0 = slim 15.0 u missile (185 tris, SAAM), 1 = the same black bomb (152 tris, UGB), 2 = a fat 18.8 u missile with a red nose ring (142 tris, third special,
  label unknown). Note: the STORES bake of section 12/13 called entry 0 "the standard missile": it is the SAAM model.
* **Which stations [E structure, I for the slot meaning].** The weapon-station locators (groups 65..76) are CHILD nodes of three slot nodes, groups 53/54/55 (a pylon-plate mesh or an
  empty container): f05e 53 -> 67,68; 54 -> 71,72,73,74; 55 -> 75,76 (j35j identical; f15c 53 -> 67..70, 54 -> 71,72, 55 -> 75,76). Three slots = three weapons: [I] slot i carries entry i.
  Checks that agree: the F-5E bomb entry 1 gets the four-station slot 54; on the F-15C slot 0 (four fuselage stations) takes the 18.3 u Sparrow-like entry 0 and slot 2 (two wing pylons)
  the 15.0 u Sidewinder-like entry 2. `model_baker.slot_stations(ex, slot)` reads this from the mpl. So **F-5E SAAM = entry 0 on stations 67, 68** (the outboard pylons).
* **Baked [V].** `bake_all_aircraft.py` `SP_STORES = {'f05e': slot 0 SAAM}`: part names `<model>/sp<station>/n<node>` (source part 7, exterior, same weapon atlas as the standard group: the
  texture count stays 3). The standard group now skips the stations the SP group takes (`dedupe_std`: one locator, one weapon), so f05e is `sp67, sp68` + `store75, store76` = the same four
  slim missiles as before the change (the preview is unchanged), 147 parts, 6,141 triangles; `--sp-slot 0|1|2|none` bakes another slot (`none` = byte-identical to the STORES bake,
  CRC 55DE11B1). Only the F-5E has an entry; the other 35 baked models are byte-identical to `models-stores-test` (compared file by file). Output (CRC32 of the whole file):
  `D:\acz-extract\models-stores-test2\f05ek_vrcockpit.zvrm` 1,621,905 bytes **AB179781** (default); variants for comparison under `...\variants\`: `f05e-sp1` (UGB bombs on 71..74)
  1,663,885 **CC536C74**, `f05e-sp2` (entry 2 on 75/76) 1,612,945 **5B983030**, `f05e-spnone` 1,621,917 **55DE11B1**, `j35j-sp0/1/2` 0B72FF1A / 5FBBAF85 / FE6D0B09 (the evidence renders).
  Previews: `...\previews\pv_f05e_default_sp67-68_SAAM.png` (default), `pv_f05e_alt_slot1_UGB.png`, `pv_f05e_alt_slot2.png`, `pv_f05e_no_sp.png`, `pv_j35j_slot[012].png`.
  Reproduce: `python -I bake_all_aircraft.py D:\b-stores\data <out> [--sp-slot N]`.
* **Runtime [V offline].** `cockpit_model`: option `spstores:0|1` (default 1); `sp<station>` parts become `AnimPiece{role store, sp = true}` in the animated range with their own
  `Mesh::sp_stations` bit space; `apply_sp_mask` / `pose_stores_only(mesh, hidden, sp_hidden, out)`; `spstores:0` or `stores:0` leaves them ordinary static parts. `store_tracker.h`:
  `firing_order(mesh, sp)`, `Tracker::configure(mesh, sp)` (the same `strel` reload), a second mailbox `acz::stores::sp_signal()`. `hud_msl.h`: `StableCount` (the MSSL hysteresis on the
  SP field: stable >= 300 ms / 2 reads, first baseline 1 s / 3 reads, fall 1..8 = launches, a bigger fall one launch + rebase, a rise = rearm, an undecodable field is ignored),
  `Decoder::sp_event()`; the producer (`step5SpOn`, `parse_sp`: on with the reader, off with `spstores:0`) sends `sp_signal().note_launch/note_rearm/set_remaining`. The XR output
  runs a second tracker per cockpit generation and poses both masks on the same range. Logs, unconditional while on: `summary-msl-hud-sp` every 5 s in step5-producer.log
  (`sp-last`, `sp-value`, `sp-launches`, `sp-rearms`, `sp-rebases`), `msl-hud;event=sp-launch|sp-rearm|sp-rebase;old;new;launches;t-ms` (max 100), and `xr-stereo;event=summary-sp-stores`
  (`stations, hidden-mask, fired, launch-signals, tracker-launches/rearms, mask-changes, source, sp-hud, sp-hud-value, sp-hud-launches`; absent when the model has no sp stations).
* **Tests.** `test_model_baker.py` (`SpStores`: slot locators, `sp` names + shared atlas, default table/override/dedupe; the file fails at the old `model_baker.py` with an ImportError),
  `hud_msl_offline` (`test_sp_salvo`: SP 8 -> 7 -> 6 with MSSL unchanged, blank/garbage/HUD-gone frames, a 16 -> 5 rebase, rearm, the log lines, `parse_sp`),
  `cockpit_model_offline` (`test_sp_stores`: names, option, stations, masks independent, `spstores:0`/`stores:0` static, anim:1, two trackers and mailboxes),
  `aircraft_auto_offline` with `ACZ_MODELS_ALL=models-stores-test2` (f05e: 2 std + 2 sp stations, 4 + 4 weapon parts, 3 textures; it FAILS against `models-stores-test`).
  The new C++ tests use APIs that did not exist before (`sp_station_of_name`, `apply_sp_mask`, `StableCount`, `sp_signal`), so they cannot compile against the old source.
* **Null salvo option [V offline against synthetic logs, U live].** `runBatchAG.ps1 -Only boxes -FireSalvo 4 -FireEvery 7 -FireAt 70 -FireSp N [-FireSpEvery 7] [-FireSpAt s] [-SpSwitchKey K]
  -CockpitAppend ';mslhud:2' -AllAppend ';vrn:1'` (`autopilot.ps1 -FireSp`): after the MSSL salvo (default first SP tap = last MSSL tap + 10 s, at least 3 s) it taps the
  special-weapon select key once (`SPSWITCH tap`), then N missile-key taps (`SPSALVO tap n/N missile tick=..`). **The in-game key that selects the special weapon is UNKNOWN**: nothing
  in the repo names it (`L` = "fire missile/special weapon", tools/live/acz-window-driver-combat.ps1; the Controls screen is the place to read it). With no `-SpSwitchKey` the taps fire whatever is
  selected. `-Analyze <run> -Kind stores` prints the SP block (`Analyze-SpHud`): PASS when the sp launches equal the SP taps within 2.5 s and the SP value fell by that many; FAIL
  `sp taps fired the special weapon` when the MSSL count moved instead (the wrong weapon). Checked with fake run directories (PASS, wrong-weapon FAIL, `-WhatIf` plan, argument guards).
* **[U] Only a live run can confirm:** that the SAAM count (HUD row 3) decodes in flight on the F-5E and falls by one per SP shot (never fired in step172-175: the null taps used the missile
  key with MISSILE selected); the weapon-select key; that entry 0 / slot 0 is really what the game mounts for SAAM on the F-5E (the Draken order is evidence, the F-5E mapping is inference);
  the real reload time of an SP pylon (`strel` 3.0 s is the same estimate); what the game shows at the SP stations when another special is selected (this bake always shows SAAM);
  how the standard MISSILE looks on the airframe; the other aircraft (add an `SP_STORES` entry each: slot 0 = the first special of the hangar list, but the label per aircraft is unknown).
* **Proposed decision entry:** "D-4xx SP-STORES: ex.bin entry i = special weapon i of the hangar list, mounted on the stations under slot node 53+i; SAAM (first special) = entry 0 = f05e stations
  67/68; `sp<station>` parts, second tracker driven by the HUD SP row, `spstores:0` off; the former 'standard missile' bake was the SAAM model."

## 16. STD-MS (2026-11-01): the standard missile is data/lodweapon/wz<L>1_ms.bin
Branch work only: nothing installed, no game or SteamVR run. [E] = evidence, [V] = checked offline here, [I] = inference, [U] = live only.
* **[E/V] The model.** `a.arc` holds `data/lodweapon/weapon.gim` plus four files `wza1_ms.bin`, `wzf1_ms.bin`, `wzj1_ms.bin`, `wzr1_ms.bin` (extracted read-only with `arcread.py extract` to `D:\b-stmsl\data\lodweapon`;
  `weapon.gim` is byte-identical to the one already used). Each has the same container as one ex.bin weapon: `[16-byte header, high-detail ACM, low-detail ACM, hull ACM]`. High-detail ACMs: wza1 106 tris 2.2 x 14.3 u,
  wzf1 114 tris 2.7 x 13.7, wzj1 114 tris 1.9 x 15.8, wzr1 138 tris 1.4 x 10.4 (one node). Previews (`D:\b-stmsl\prev\pv_*.png`, textured with weapon.gim) are plain finned AAMs, none like the SAAM (entry 0: yellow/brown rings, olive nose).
  wzf1 has two blue bands; the file exists, so UVs are the same atlas [V].
* **[I] Which aircraft gets which.** The four letters equal the first letters of four type codes (a10a, f05e, j35j, rm01): the bake picks `wz<first letter>1_ms.bin` (`bake_all_aircraft.std_ms_name`). Types whose first letter has no file
  (e m s t x y z, mr, ty: m29a, m31b, mr2d, su27/37/47, typn, x29a, yf23, zoef) take `wzf1` as a GUESS; the game's real per-aircraft choice is not decoded (no name table found). `--std-ms none` reproduces the old ex.bin entry bake.
* **[V] Baked.** `model_baker.ms_weapon_model(path)`; `add_stores` takes `stores['ms']` (else the pack entry, still used by the `sp` group). The `store<station>` group is the ms mesh; the `sp<station>` group is unchanged
  (ex.bin entry = slot, F-5E SAAM on 67/68). F-5E: `sp67, sp68` (SAAM, 169 baked tris each) + `store75, store76` (wzf1, 114 each), 147 parts, 3 textures. Station sets are unchanged from the STORES bake: re-running
  `station_candidates` with the ms meshes gives the same pairs except a10a (plate vertices 23 -> 38 with the fatter wza1, above the 30 rack limit; kept) and a few types where the rule would now ALSO propose another pair (not applied).
  Output `D:\acz-extract\models-stores-test3` (24 vrcockpit files differ from `models-stores-test2`, all other 84 files byte-identical), previews in `...\previews`.
* **Tests.** `test_model_baker.py` `StdMs` (the standard group's mesh is the ms file, not ex.bin entry 0; ms needs no pack; letter mapping/fallback); fails at the old source with an ImportError.
* **[U]** Whether the game really mounts wzf1 on the F-5E (a letter coincidence is the only evidence), the fallback types, and how the missile looks on the airframe in the headset.
* **CRC32 (whole file) of the 24 changed vrcockpit files:** a10ak 92EA5D94; f01ak 28C7F4B5; f04ek C1459981; f05ek 81E9C2E5; f14dk 52354A4B; f15ck A0388DF8; f15ek 68071B43; f15sk 80DDD683; f16xk 8088527B; f17ak 624018CC; f18ck 4AFDF211; fa22k B1242F6E; j35jk FC88FB57; m29ak 6ABF1351; m31bk 4CCB6084; mr2dk DE1A75FA; rm01k 764A3127; su27k 0411CB34; su37k 4BB4908A; su47k 90BEFEBB; typnk BBEC3AD8; x29ak E5068836; yf23k 037C3139; zoefk 0716E18B. Reproduce: `python -I bake_all_aircraft.py D:\b-stmsl\data <out>` (`--std-ms none` = the test2 files).
