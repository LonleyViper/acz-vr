# E6-ANCHOR-FREE: start the terrain/cockpit camera feed from the E6 content, without look events

Status: implemented behind the existing key `xr_terrain_stage` (option `e6loc:`; no new config key), offline tests only. NOT run live
(no game/SteamVR launch in this task): whether it locks in the real game, and how fast, is unverified until a driver run reads the
`e6-locate` lines (section 6).

Code: `src/acz-vr/ee_e6_locator.h/.cpp` (SDK-free core), glue in `src/acz-vr/ee_camera_probe_win.cpp` (`terrain_feed_mode`,
`e6_background`, `e6_terrain_tick`, `e6_frame_tick`), `CamMailbox::invalidate` in `src/acz-vr/terrain_camera.h`, option extraction in
`src/acz-vr/xr_stereo_proxy.cpp`. Tests: `tests/acz-vr/e6_locator_offline_tests.cpp` (ctest `e6_locator_offline`).

Labels: **[E]** = read from an archived log/file, **[I]** = inference, **[T]** = shown by an offline test only.

## 1. What the existing feed actually does (correction of the task premise)

The request said the feed starts only after the probe's `tracks-look` anchor (>= 2 game look events). That is true for the PS2 probe and the
lens scan (`ee_ps2_probe`, `ee_lens_scan`: `LookView.have` needs a `tracks-look` track; D-295), **not for the terrain feed** [E]:

* `terrain_tick` / `terrain_frame_tick` read the E6 words at the probe's **observation base** `Shared::obs_base`
  (`Experiment::observe`): the validated base, else the best-scoring relative-scan candidate (`bestEff_`, set by `set_tracks` from a WIDE
  relative pass, which runs while the producer publishes the cockpit basis), else `blocks_[0]` = the first allocation-exact 64 MiB block.
  None of these needs a look event.
* headset test 16 (real headset, no look drive) and every terrain run step53..step67: `summary-terrain` shows `no-camera=0` in every window
  [E]. The feed DID run without look events.

The real defect is different [E]: until the first wide pass lands (`observe-follow`), `obs_base` is `blocks_[0]`, and in every archived run
that block is **all zero** at the E6 offsets (step67 discovery hexdump at +FDD400/+FDD440/+FDD850: zeros). The terrain was fed
campos = (0,0,0), rot = 0 (finite, so not counted as no-camera). In headset test 16 the first follow was a score-4 junk structure
(`243af19c898`, air = 46.4,-1.25,1.25) for another 7.7 s. Time from the first XR frame to the correct block [E]:

| run | first XR frame | correct `observe-follow` | wrong-feed window |
|---|---|---|---|
| step53 | 193060171 | 193061321 | 1.15 s (zero block) |
| step54 / run1 | 193976562 / 193703312 | 193978321 / 193703901 | 1.76 s / 0.59 s |
| step56 | 196760109 | 196761793 | 1.68 s |
| step58 | 200693875 | 200695040 | 1.17 s |
| step62 A / B | 202305640 / 202552937 | 202305963 / 202554342 | 0.32 s / 1.41 s |
| step65 launch 2 | 206132109 | 206132985 | 0.88 s |
| step67 | 209192546 | 209192259 | none (followed 0.29 s before) |
| headset test 16 | 197164500 | 197173523 | **9.0 s** (1.4 s zero block, then 7.7 s junk) |

The relative scan also depends on the cockpit basis and its cadence (2 s for the first 60 s of cockpit time, then 10 s), and a junk structure
can win a pass. The new locator removes both dependencies.

## 2. The block across runs [E]

From the `region-largest`, `candidates`, `observe-follow` and `timeline` lines of every archived `ee-camera.log` (step47, step50, step52,
step53, step54 x2, step56 x2, step58 x2, step62 x3, step65 x2, step67, headset tests 15/16):

* The E6 structure is always at **offset 0 of a single VirtualQuery region of exactly 64.00 MiB, MEM_PRIVATE, protect 4 (PAGE_READWRITE)**,
  ranked 2nd by size (after a 66.12 MiB image). Its base is sometimes not 64 KiB aligned (`1f824bdf000`, `22f2732a000`, `1cb24912000`,
  `1f420b3f000`): it is a region inside a larger allocation, which is why the probe's allocation-exact `CandidateCollector` never lists it.
* The probe's `candidates` line always lists exactly ONE allocation-exact 64 MiB block (total=1), a different address, all-zero at the
  E6 offsets.
* Effective base shift is +0 in every run (`located;shift=+0`, best0 = region base).
* Values seen at the real block over 3,000+ timeline samples: |cam - air| 35.69..36.56; angle between cam - air and the aircraft forward
  (heading, rot pitch - look pitch) <= 5.7 deg without look and <= 12.8 deg with look turns/rolls; |wrap(rot yaw - heading - look yaw)|
  <= 0.372 (look ramps); |heading| <= 0.94; |rot| <= 3.136; |air x|,|air z| <= 144,003; air y 502..4,433; pitch -29.8..+15.9 deg,
  roll -169.7..+174.6 deg, look yaw up to 2.48 rad.
* The pitch sign: forward = (-sin h cos p, sin p, -cos h cos p) with p = rot pitch - look pitch gives 5.7 deg on the step56/step58
  nose-down samples, the opposite sign 54-56 deg. With the game look turned, the camera offset stays fixed in the AIRCRAFT frame (rot pitch/
  yaw include the look; using rot instead of heading gives 140 deg errors).
* The three decoy structures the relative scan keeps seeing inside the real block (`...898`, `...850`, `...61c`, at negative shifts) are
  static, with |cam-air| 42-46 and 43-122 deg off the forward axis.
* Eye offsets of all 36 player aircraft (`research/world-render-data/aircraft_cockpits.json`, .pvb): 19.4..53.2 units, 3.0..15.1 deg above
  the forward axis.

## 3. The locator

Candidates (`CandidateBuilder`, fed by a VirtualQuery walk on the probe's worker thread; no memory contents read by the walk): every
committed private read-write region of exactly 64 MiB (`r`), plus every allocation of exactly 64 MiB made only of such regions (`a`, the
probe's old rule), deduplicated, in address order, at most 16 (`dropped=` counts the rest). Asked again every 2 s while searching.

Per sample (two windows, 64 + 28 = **92 bytes**: +0xFDD40C..+0xFDD44B and +0xFDD860..+0xFDD87B; each read VirtualQuery-checked and
SEH-guarded) `check_sample` rejects, in order: `nonfinite`, `aircraft-at-origin` (|x|+|z| <= 1: the zero block), `look-range`,
`rotation-range` (look and rot outside [-pi, pi] + 0.01), `heading-range` (2 pi: wrap never observed [I]), `position-range`
(|x|,|z| of aircraft and camera > 262,144 + 32,768), `altitude-range` (-2,000..200,000), `eye-distance` (|cam - air| outside 15..60),
`eye-direction` (> 25 deg off the aircraft forward), `yaw-relation` (> 0.6 rad). Consecutive samples of the same block (`check_pair`):
`jump` (> 20,000 u/s * dt + 200) and `eye-distance-unstable` (> 1.5 units change; the eye offset is rigid).

Searching: every candidate is sampled at most every 100 ms, round robin, at most 512 bytes (5 samples) per Present tick. A candidate
**locks** when it has 8 consecutive valid, consistent samples of which >= 3 moved the aircraft (bit-exact position change), it is the only
one, and no other candidate is a moving structure still building a streak. Two or more qualifying: `ambiguous`, no lock (a live duplicate
must not be guessed). A plausible but static structure never locks (`static`). Expected lock time ~0.8 s after the first enumeration
[T: 0.7-1.5 s in the fake runs].

Locked: one 92-byte sample per Present tick, every sample re-validated (single + pair checks; a paused game's identical samples stay valid).
An invalid sample is not published; the previous one is kept for up to 250 ms (`hold`), after that the mailbox is invalidated (the consumer
counts `no-camera` and shows the game's own world, exactly the existing no-camera path); after 1,000 ms without a valid sample the lock is
`lost` and the search restarts (e.g. the block freed, reused, or the mission reloaded). A teleport (mission restart) costs one held sample.

The render-thread frame sample (WORLD-1 `e6:frame`, seam B) reads the locked base (an atomic published by the Present tick), 92 bytes,
single-sample checked, into the frame mailbox; no lock state is touched there.

## 4. Modes (option inside the existing `xr_terrain_stage` value; config stays at 31 of 32 keys)

`cam:` is already the fixed test camera (`cam:x,y,z,deg`), so the option is named `e6loc:`:

* `e6loc:auto` (**default**): the content-validated block; while the locator has no lock, the probe's observation base is still used, but
  only for samples that pass `check_sample` (so the zero block and the junk structures no longer feed a camera: no-camera instead).
* `e6loc:content`: the locator only (no probe fallback).
* `e6loc:probe`: the previous feed exactly (`terrain_tick`/`terrain_frame_tick` bodies unchanged; the mailbox is never invalidated).

Terrain key off: unchanged (the mode lookup sits after the existing `xr_terrain_render` test; no locator object is created). A value without
`e6loc` reaches the terrain/props parsers byte-identical [T]. A bad `e6loc` value refuses the terrain like any bad option
(`terrain-disabled;reason=bad-stage-option`). The feed still needs `ee_camera_probe` (its Present hook and worker thread host the locator);
`tools/live/vr-test-start.ps1 -TerrainRender` already adds it.

## 5. Read-only and cost

`acz::e6loc::ReadMem` has no write operation; the glue's implementations are `range_ok` + `guarded_copy` and the probe's tick-side
`ProcessMemory::read`. No game-memory writes, no code patching, no protection changes. Per Present tick: locked 92 bytes (2 x 2
VirtualQuery + 2 copies), searching <= 512 bytes; the region walk runs on the worker thread at most every 500 ms and only while searching.

## 6. Log lines (`%LOCALAPPDATA%\acz-vr\logs\ee-camera.log`, also diagnostics)

`e6-locate;event=armed` (mode, caps) / `candidates` (n, regions seen, dropped, base:kind list; on change) / `search` (every 5 s while
searching: per candidate base:kind:streak/moves:last reason) / `ambiguous` / `locked` (base, kind, samples, moves, air, heading, cam, rot,
eye distance and angle, `look-events-needed=0`) / `lost` (reason, ms without a valid sample) / `summary` (every 10 s: state, locks, losses,
valid, rejected, fallback published/rejected, no-camera ticks, max bytes per tick, reason counts). `xr-stereo.log`:
`terrain-camera-feed;e6loc=<mode>`.

What a driver run should check: `locked` within ~1-2 s of the cockpit with `base=` = the 64.00 MiB region of `region-largest` (and = the
later `observe-follow` base); `summary` `losses=0` in flight; `summary-terrain` no-camera only before the lock; no terrain drawn from the
origin in the first seconds of XR.

## 7. Tests [T] (`e6_locator_offline`, 1,831+ checks)

Option extraction (values without `e6loc` unchanged, all three modes, `=` form, last wins, bad values refused, `mode_in` agrees); single-sample
checks on the REAL step67 E6 bytes (decoded from the hexdump lines) and on step56 nose-down, step50 look-turned and headset-test-16 inverted
samples (all valid), the zero block and the headset-test-16 junk structure (rejected), every rejection reason by name; candidate rules
(region vs allocation, non-RW/mapped/32 MiB/mixed allocations excluded, cap 16 with `dropped`); a fake process with an all-zero
allocation-exact block, a static plausible decoy, a moving decoy with an 80-unit eye, an unreadable 64 MiB block, a 32 MiB region and the
moving E6 block: locks on the E6 block in < 1.5 s, <= 512 bytes per searching tick, exactly 92 per locked tick, explanatory search/summary
lines; static decoy alone never locks and is reported `static`; teleport = one held sample; paused = stays valid; freed block = hold
< 250 ms, then no camera, `lost;reason=unreadable` after 1.0-1.1 s, relock when it returns; garbage over the block = lost; two live copies
= `ambiguous`, never locked; look-turned climbing flight locks; 16 candidates with the real one last locks via the round robin;
`CamMailbox::invalidate`.

Not tested offline: the Windows glue (VirtualQuery walk, threads), the consumer's reaction (existing no-camera path), the real game.

## 8. Open / inferences

* [I] Heading wrap: never seen beyond +-0.94 rad; the check allows 2 pi. If the game keeps heading unwrapped beyond 2 pi in long turns the
  lock would be lost (`heading-range` in the summary) - widen then.
* [I] A mission change probably frees/reuses the block: expected `lost` then a relock; unverified.
* [I] Torn reads (the emulator writing E6 while we copy) were not observed in the archived samples (eye distance range 0.87 over whole runs);
  if they occur, they cost held samples, not a wrong camera.
* The PS2 probe and lens scan still need the tracks-look anchor; the locked base could serve as their anchor too (not done here).

## 9. Camera-freshness audit (Astra P2, issue 29 comment 6438207964; master 3e7bb01 BEFORE the CAM-EPOCH change)

Every consumer of camera data and the rule that admitted a sample at 3e7bb01 (read from the code, not from a run):

| Consumer | Camera source | Admission rule at 3e7bb01 | Stale risk |
|---|---|---|---|
| Terrain eye cameras (`xr_stereo_output.cpp` transact) | `e6:frame` (default): `StereoFrame::e6_*` = the frame-mailbox sample published during this frame's seam-B call (`gpu_copy_step5_hooks.inl`: copied only when `published()` moved during the call); otherwise **the newest Present-tick sample** (`cam_mailbox().try_read`) | per-frame: valid flag + (auto/content) `check_sample` on the producer side; no time, no epoch. Fallback: finite + not invalidated. `e6:latest`: the newest sample, same rule | YES: a frame without its own sample silently took the newest Present sample, of any age. `e6loc:probe` never invalidates: the last sample stays forever after source loss. auto/content: the Present tick invalidates after the 250 ms hold, but only while the Present tick runs (a stopped tick, `g_finished` or a skipped busy tick left the last sample valid indefinitely) |
| Terrain, `csrc:game` | this frame's cockpit camera axes (`game_cam_*`, decomposed from the frame's first cockpit member) + DrawACM_v eye (`game_eye`) | axes orthonormal/right-handed (`camera_frame_axes`); cross-checked against E6 only when E6 was available (`gmax` 15 deg); eye from E6 when the frame has no DrawACM_v eye | the game camera is frame-correlated by construction; its E6-eye fallback and the gmax cross-check inherited the E6 staleness above |
| Props/trees (WORLD-3), object occlusion (`occl:`), stage light/tint/canopy (COCKPIT-3/4) | the terrain eye cameras of the same frame | as terrain | as terrain |
| Cockpit animation (COCKPIT-4 `anim:1`) | the E6 Euler frame of the sample above; time = the frame's publication time | a new `seq`; `anim_update` resets on dt > max_dt | a fallback (newest) sample was paired with the frame's time; a relock within max_dt turned the attitude jump into a rate spike |
| Aircraft auto (AIRCRAFT-AUTO / COCKPIT-FIRST) | eye-offset ring (`aircraft::eye_mailbox`, 64 slots), published only for validated samples of the LOCKED block | every drained sample; lock epoch carried; the detector restarts its window on a new epoch; ring overrun counted as lost | no age limit (by design: the aircraft type does not change with time); a mission change WITHOUT a lock loss kept the window |
| Tactical (`tac:1`) | `tac_cam` = the terrain camera of this frame; contacts from this frame's object records + world-to-screen matrix | `terrain_on` (no terrain camera: `no_camera`, no tactical) | as terrain |
| Effects memory (`fxmem:1`) | missile-smoke record capture + `fx-smoke-frame` log only (`active=capture-log-only`, effects-outside-window.md 8.5); the sprite memory core uses the per-frame world-objects camera | not in the live render path (nothing replayed) | none today |
| HUD quad, HUD markers, effects layer | seat anchor + head pose + the frame's lens; the frame's own HUD/marker/effects images (never an older one) | per-frame validity flags | no E6 use |
| Re-submitted XR layers (`pace`) | the last released images with their own pose/FOV | `reuse_max_age_ms` 500, tracked, same LOCAL space generation | none: the terrain is baked into the image with the camera it was rendered with |
| Held world image (`xr_world_hold_last`) | the last captured game world image with its own lens and look | `kWorldHoldMs` 750 with a fade | the held image keeps its lens/look; the terrain under it is drawn with the current frame's camera |

Evidence vs inference: the table is read from the source at 3e7bb01 [E]. Whether a stale camera actually reached a headset is NOT shown by
any archived log (summary-terrain `no-camera` counts refusals only, never stale admissions) [I].

## 10. CAM-EPOCH: freshness, epochs, fail-closed camera modes (answer to Astra P2, comments 6438207964 and 6438481134)

Status: implemented and tested OFFLINE only (no game, SteamVR or headset run in this task). Code: `src/acz-vr/terrain_camera.h` (sample,
mailbox, history, admission, modes), `src/acz-vr/cam_lag.h` (in-DLL lag meter), consumer `xr_stereo_output.cpp` (`admit_camera`,
`camera_outcome`, `camera_lag_frame`, `camlag_*`), producer `ee_camera_probe_win.cpp` (epochs, read-start times), `gpu_copy_step5_hooks.inl`
(per-frame copy), proxy (`e6loc:probe` keeps the old path; stage epoch at stage install), offline tool `tools/live/camera-lag-analyze.py`.

### 10.1 Locator timing (correction)

The locator keeps the last good sample for **at most 250 ms** (`hold`); after that the glue invalidates the mailbox (no camera). **1 s** without
a valid sample is the lock-LOSS threshold (`lost`, search restarts). It is not the no-camera threshold. That was correct in section 3; the
locator header comment said "drops the lock (no camera) after 1 s" and was rewritten. (The D-317 one-line summary "holds 250 ms, no-camera
after 1 s" reads the same way: the code is the authority, 250 ms then no camera, 1 s lost.) The hold only works while the Present tick runs;
the consumer's age check below covers a stopped tick.

### 10.2 The immutable sample and the epochs

`CamSample` = campos, pitch/yaw/roll, heading **plus** `t_read_ns` (read start), `t_ns` (publication, after the read; steady clock, the
same domain as the producer's `diag_start_us`/`diag_publish_us`), `seq`, `lock_epoch`, `stage_epoch`, `tag` (the recording generation the
frame mailbox was told before the seam-B read; 0 for the Present tick). A frame record carries a COPY (`StereoFrame::e6_*`, `e6_t_ns`,
`e6_t_read_ns`, `e6_lock_epoch`, `e6_stage_epoch`, `e6_tag`); nothing ever rewrites it.
Review 2 (Astra 6442582596 #2): the epochs are those of the READ, not of the publication. The Present tick installs the locked base and its lock
epoch as one seqlock transition (`CamEpochs::set_source`); every reader that is not the lock writer (seam B, the probe feeds) uses
`acz::terrain::read_publish`: capture base + lock + stage before the read, read from that base, refuse the sample when any of them changed during
the read, publish the captured identity (a change after the check leaves the old identity on the sample, which admission refuses). The Present tick
stamps its own lock epoch and the stage epoch captured before its read. Fixtures: `cam_epoch_offline` `test_read_interleavings` / `test_read_threads`.

* Lock epoch = locks + losses of the locator: increments on every lock, loss and relock (`camera-epoch;event=lock|loss|relock` in
  ee-camera.log; also `lock-epoch=` in the `e6-locate` summary).
* Stage epoch: +1 when the proxy installs a terrain stage (`xr-stereo;event=camera-epoch;change=stage;reason=terrain-stage-installed`,
  i.e. every XR start / retry and any future re-detection) and +1 on every locked position discontinuity (a `jump` rejection: mission
  restart/change, a block reused; `camera-epoch;event=stage-change;reason=position-discontinuity(locked-jump)`). The epochs change before the
  tick publishes, so the new sample carries the new epoch.
* The Present mailbox keeps a 32-entry history (publications and invalidation markers, ~0.5 s at 60 Hz) for the bounded fallback.
* AIRCRAFT-AUTO: the eye-offset epoch now also changes with the stage epoch (a teleport without a lock loss restarts the detector window).

### 10.3 Admission (key `xr_terrain_stage`, default on; `camadmit:0` = the previous selection; `e6loc:probe` forces it off)

A sample is admitted only if finite and sane (|campos| <= 600,000, |angles| <= 6.3, heading <= 12.6), timed (t != 0), of the CURRENT lock and
stage epoch, not older than `camage` (default **250 ms**, 20..2000) and not more than 20 ms newer than its reference:

* `e6:frame` (default): the frame's own seam-B sample, reference = the frame's publication time, and its `tag` must equal the frame's
  recording generation (`uncorrelated` otherwise). If it is absent or refused: the **bounded fallback** = the Present-history publication
  nearest to the frame's recording start within `camwin` (default **50 ms**, 0..200, 0 = none), epochs and age checked again; an
  invalidation marker there -> `fallback-invalidated`; the history no longer reaching back -> `fallback-overwritten`; nothing within the
  window -> `fallback-missing`. Never the newest sample by default.
* `e6:latest`: the newest Present sample, reference = now.
* Refused: the frame takes the explicit no-camera path for E6.

### 10.4 Camera modes: fail closed (comment 6438481134 (1))

Every composed frame records the mode it actually used (counted in `summary-camera-admit;modes=`, in every `camera-admit` transition line and in
the `camlag` f-lines):

| mode | orientation | position | counted as correlated |
|---|---|---|---|
| `correlated-hybrid` | the frame's own draw camera (`game_cam_*`) | the frame's DrawACM_v eye, or the E6 eye of the SAME frame (seam-B sample); cross-checked against that E6 sample (`gmax`) | yes |
| `game-camera-only` | the frame's own draw camera | the frame's DrawACM_v eye; no admissible same-frame E6, so no cross-check | no (frame-correlated by construction, uncross-checked) |
| `e6-frame` | E6 Euler of the frame's own sample | E6 | no |
| `e6-only-game-refused` | E6 (the draw camera was > gmax away) | E6 | no |
| `e6-present-history` / `e6-latest` | E6 from the Present tick | E6 | no: explicitly uncorrelated display fallback |
| `none` | - | - | terrain off for that frame |

Rules: (a) the draw camera's orientation is never combined with an E6 position from another frame (history/latest): without its own
DrawACM_v eye and without a same-frame E6 sample, the draw camera is not used at all; (b) the gmax cross-check only compares against the
same-frame sample; (c) **correlation-dependent replacement is refused** when the pair is absent: terrain occluding replayed objects
(`occl:`) runs only when the terrain camera is the objects' own camera of that frame (`correlated-hybrid`, `game-camera-only`, or the fixed
test camera); E6-only frames draw the terrain as a display fallback without object occlusion (`occlusion-refused=` counter,
`object-registration=refused` in the transition line). (d) TACTICAL keeps its own limited contract: contacts come from the frame's own object
records and world-to-screen matrix; its world markers use the terrain camera of the frame (whatever mode), so in E6-only modes they are a
display approximation [I]. (e) Records of another view/pass: the producer already refuses world records that do not match the reference
camera (gpu_copy_step5_world.h camera checks); nothing here averages cameras.

EXTRAP (`extrap:1`, merged from master, default off) dead-reckons the chosen terrain camera to the display time AFTER the mode is decided; the
recorded mode names the source the extrapolation started from, and the camlag lines log the un-extrapolated inputs.

Reused images: `pace()` re-submits the released images with their own pose/FOV; they are never re-composed, so they keep the camera they
were made with (a `camlag;r=` line names the original frame and E6 sequence). The held world image (`xr_world_hold_last`) keeps the camera
snapshot and mode of the frame it was captured from (`held_camera()`; test: never the newest).

### 10.5 Log lines

ee-camera.log: `camera-epoch;event=lock|relock|loss|stage-change;...`; `e6-locate;event=summary;...;lock-epoch=;discontinuities=`.
xr-stereo.log: `camera-epoch;change=stage;reason=terrain-stage-installed;admission=on|off(...)`; `camera-admit;from=<mode/source-or-reason>;
to=...;mode=;frame-sample=<reason>;...;action=;object-registration=` once per outcome change (cap 200, then counted);
`summary-camera-admit` every 5 s (admitted frame/fallback/latest, stale/future refused, epoch mismatch lock/stage, uncorrelated, fallback
missing/overwritten/invalidated, no-sample, bad-sample, frame-sample-refused, game-camera-only, held, anim-epoch-resets, transitions,
admitted-age-max-ms, limits, current epochs, modes, occlusion-refused); `summary-camera-lag` every 5 s (see 10.6). The admission lines only
appear with admission on (legacy and key off: no new lines).

COCKPIT-4 `anim:1`: fed only with the admitted sample, timed by its own publication time, keyed by source + sequence, and the rate state
restarts on a lock/stage epoch change (a relock jump is not a stick deflection).

### 10.6 Lag / correlation evidence (comment 6438481134 (2))

**Not proven**: which emulated frame wrote the bytes read at seam B, and the phase between the Present-tick samples and the replayed draws.
Two instruments, both diagnostic only (no decision reads them):

* In the DLL, `summary-camera-lag` (cam_lag.h): per 5 s window, the relative-rotation angle (mean/max) and eye distance (mean/max) between the
  frame's draw camera and its own seam-B E6 sample, the sample's age, the best per-frame offset (E6 of consumed frames k-2..k+2) and the best
  Present-history offset (-48..+48 ms). A quick look in the live log, nothing more.
* For the OFFLINE analysis: bounded `camlag` bursts (300 composed frames every 60 s, at most 6,000 lines per session, `camlog:0` turns them
  off): `camlag-columns` header lines, one `f=` line per composed frame with the inputs it actually used (frame id, recording generation,
  slot, members, mode, source, refusal reasons, E6 sequence + read-start + publication times, sample and current lock/stage epochs, recording
  start/publication times, draw camera up/fwd/eye, E6 up/fwd/eye, lens), one `p=` line per Present publication (seam-B and Present samples are
  distinguished by `src`/line kind), `r=` for reused images, `s=` per burst (refused, frame-sample-missing, present-gaps, dropped-by-cap,
  counts per mode). Not logged (not carried by the frame record today): the submission occurrence and the object reference-draw identity.
* `tools/live/camera-lag-analyze.py <xr-stereo.log> [--json]`: relative rotation Q = G E^T as angle + signed residuals about the draw camera's
  right/up/fwd axes (no Euler subtraction), against seam-B samples of frames fid-3..fid+3 and Present samples at -64..+64 ms (8 ms steps,
  +-4 ms); per offset pairs/missing/median/p95/max; best lag with `resolved|ambiguous|unresolved`; separately for left/right turns and the
  frames around yaw-rate reversals (a lag flips the signed residual with the turn, a fixed transform does not). `--selftest` (ctest
  `camera_lag_analyze_selftest` when Python is installed): synthetic logs with a known +1 frame / +16 ms lag, no lag, a static attitude
  (ambiguous), no draw camera (unresolved), skipped frame ids (missing pairs).
* A nonzero best lag is a diagnosis of skew, **not** permission to compensate by a constant frame offset. Registration (landmark reprojection
  at two poses, Astra (a)4) is a separate check not done here.

Driver recipe (null run, user absent is fine): any existing terrain run with the step-N DLLs containing this change; afterwards
`python -I tools/live/camera-lag-analyze.py %LOCALAPPDATA%\acz-vr\logs\xr-stereo.log`. For a meaningful first baseline (Astra (a)2): a steady
segment, opposite banked turns with at least one reversal, some pitch; head and game look neutral.

### 10.7 Tests [T]

`cam_epoch_offline` (~35,500 checks): stamping/immutability/invalidation markers; every admission rule; publication followed by source loss
(stale after the limit, no newest substitution, invalidated); stage change (old-stage samples refused in frame, fallback and latest modes);
relock after loss (lock epochs 1 -> 2 -> 3); wrong recording generation (uncorrelated) with the bounded fallback choosing the publication at
the recording start, not the newest; history overwritten; lag meter (+1 frame offset, skipped ids, Present offset); option parsing; a
randomized comparison against an independent reference model (5,000 random sessions, ~75,000 queries, 0 mismatches, every refusal class
exercised); a thread stress (200,000 publications with invalidations/epoch changes, three readers, ~150,000 reads, 0 torn).
`e6_locator_offline`: lock epoch 1 / teleport = 1 discontinuity / loss 2 / relock 3 / loss 4. Consumer `xr_stereo_terrain_offline` (cases
1-13 of `terrain_cam_epoch_runs`): fresh own sample used; stale -> no camera (legacy path still draws it, no new lines); stage change;
loss/relock; history fallback = the publication at the recording start; held image keeps the captured frame's snapshot; game-camera-only;
lag line; fixed camera byte-identical; E6-only frames refuse object occlusion while hybrid frames keep it; draw axes never paired with a
history E6 eye; gmax refusal is e6-only; camlag columns/f/p lines within the line cap. Existing terrain/WORLD-1 cases run unchanged with
admission off (hand-built runtimes) [T].

### 10.8 Answer to Astra (what is evidence and what is inference)

* [T] P2 as stated at pin 9f878c2 (no time, no epoch, indefinite acceptance, silent newest fallback) is fixed in code and in offline tests;
  the 250 ms / 1 s correction is applied (10.1).
* [T] Fail-closed modes (10.4): fallbacks are recorded per frame and never counted as correlated; object occlusion refuses without the pair.
* [I] That seam B reads the E6 values of the emulated frame being drawn: NOT shown. The `tag` proves only that the sample was read during that
  frame's seam-B call (CPU timing), not which emulated frame wrote it (as Astra notes). The `camlag` bursts + offline tool are built to
  quantify it; no live data yet.
* [I] The 250 ms age limit and the 50 ms fallback window are engineering choices (a few frames at 15-60 fps), not measured optima.
* [I] The draw camera's DrawACM_v eye as absolute stage position rests on the test-16 comparison (same x/y as E6 at one moment, world1-impl.md
  section 3); its centre meaning/scale is not independently proven.
* [E] earlier live evidence that the roll fix agrees with the game's horizon (D-306, D-319) is unchanged; nothing here was run live.
* A plausible finite sample and a seqlock do not prove a coherent multi-window game read (the E6 windows are two reads); the locator's
  pair checks catch gross tearing only [I].

## 11. E6-RELOCATED (2026-10-30): the structure is not always at +0xFDD40C; the probe-base fallback published static junk

### 11.1 Evidence [E] (headset session `D:\acz-evidence-archive\headset-2026-10-30-session\live-1144`, step133, m03k, F-5E)

* The locator never locked: `summary` `locks=0;valid=0;rejected=5498+`, every `search` line `c0=1c713260000:r:aircraft-at-origin;c1=1c75ff87000:r:aircraft-at-origin`.
  Both candidate regions read zero/garbage at the fixed offset (+FDD400/+FDD440 discovery hexdumps zero).
* The probe's relative scan DID find the live structure (score 5, eye 3.971/21.9 = F-5E): `scan-best;rank=0;look-yaw-at=1c75ff87000+CAAC70`, i.e. window A at
  **region +0xCAAC6C** (probe base `1c75fc54860` = region - 0x3327A0). Its `timeline` samples move for the whole flight (air y 14,500..24,650): the structure is
  fine at altitude; only its position in the block differs.
* The same session's first launch (`ee-camera.prev.log`, J35): window A at region **+0xCA811C**. Every m03k/airborne-start null run of step124 (36 runs, all `locks=0`; 32 show the score-5 structure at `+CA8120`)
  and the L1 runs of 2026-10-29/30 show the same `+CA8120` score-5 structure and `locks=0` (they flew on the probe fallback). 2026-10-29 m02a/k00a: `+FDD410`
  (= the fixed offset). Across the archive the score-5 structure was never seen twice in one pass (no live duplicate). The playbook note "ee-camera reads
  ZERO above y ~6,500" is [I] a symptom of the same thing (the fixed offset going stale while the structure lives elsewhere), not an altitude limit of the struct.
* The camera then failed in a second, separate way: the e6loc:auto fallback published the probe's observation base whenever ONE sample passed `check_sample`.
  At 154209589 the probe's `observe-follow` switched to `1c683093140` (score 4, a STATIC junk structure: L=2,2,2 air=2,6.6e-10,45.3 cam=2,2,2 rot=2,2,2), which
  passes `check_sample` (eye 43.4, 2.6 deg). Within ~11 ms xr-stereo logged `camera-admit;to=e6-only-game-refused/frame;object-registration=refused`,
  and every later `camlag` line carries E6 eye `2.00,2.00,2.00` with an E6 forward pointing up (0.378,0.909,-0.173): the game camera was refused against the junk
  for the rest of the flight (2,401 lines), hence `cam-held=450/450`, `cam-extrap=0`, boxes `refused-view` and the misplaced lock diamond.
* There is no hud-menu latch: `camera_outcome` recomputes the mode per frame (`cam_mode_registers_objects`); the hud-menu enters (154192875, 154210140) only
  coincide. The mode stayed refused because the fallback source stayed on the junk base. The cockpit `stale-forget` follows from `no-e6-samples(locator-not-locked)`.

### 11.2 Fix

* `StructScanner` (ee_e6_locator.h, SDK-free): while the locator searches, the probe's worker thread reads every candidate block in 1 MiB chunks (4 MiB per worker
  call, VirtualQuery-checked + SEH-guarded copies, read-only) and reports each 4-aligned position whose 92 bytes (window B = window A + 0x454) pass `check_sample`
  (prefilter: 15..60-unit eye, aircraft off the origin), at most 12 per pass. A pass takes ~0.5-2 s for two blocks; a new one at most every 3 s, at once when a
  search starts (loss, mission change); cancelled while locked. Hits join the candidate list as kind `s`; the Locator validates them exactly like the fixed-offset
  ones (8 consistent samples, 3 moves, unique, ambiguity refused), so static junk found by the scan never locks.
* `FallbackGate`: the e6loc:auto probe-base fallback (Present tick and the render-thread frame tick) now needs the same rules as a lock (8 consecutive
  `check_sample` + `check_pair` samples of ONE base with 3 moves); a base change, invalid sample or jump withdraws it at once; once qualified a paused (identical)
  sample stays published. The frame tick uses the probe base only while it equals the gate's qualified base.
* New log lines: `e6-locate;event=struct-scan;pass;blocks;bytes;unreadable-chunks;ms;hits;dropped;list=<base>(block+<window A offset>)` and
  `e6-locate;event=fallback-gate;state=qualified|withdrawn;base`. `candidates` lists `<base>:s`; `locked` shows `kind=s` for a relocated structure.

### 11.3 Tests [T] (`e6_locator_offline`)

REAL m03k samples (3 timeline rows of `1c75fc54860`: valid, consistent, F-5E eye 22.26) and the real junk (passes `check_sample`: documents why the gate exists);
`scan_struct` finds the real sample at region +0xCAAC6C with base `1c75fc54860` (= the probe's score-5 base), exact-fit boundaries, zero memory = none; the
scanner across a 1 MiB chunk border, an unreadable block (64 chunks counted), the hit cap/dropped count, dedup, keep-previous-hits, cancel; the m03k layout
(zero region + region with garbage at the fixed offset, the moving F-5E structure at +0xCAAC6C and the static junk): BEFORE (fixed offsets only = the shipped
flow) no lock in 8 s with `aircraft-at-origin` like the session; AFTER lock on `1c75fc54860;kind=s` in < 2.5 s, junk reported `static`, no ambiguity, searching
ticks <= 512 bytes; relocation while locked = one loss and a relock at the new offset; `FallbackGate`: junk never admitted (`static`, 600 samples), real admitted at
the 8th sample, withdrawn on a base switch / jump / zero sample, kept while paused; frozen at the spawn (structure + junk both found, neither moves) = no lock,
both `static`. The fake m03k block also produced a third, accidental plausible pattern (`1c75ff87434:s`: a window spanning the fixed-offset garbage) - static,
never locked: real memory will show such hits too (`struct-scan` lists them), which is why the scan only proposes and the Locator decides.
Run 2026-10-30 (D:\b-e6\w, Release, stub OpenXR source for configure only - the cached SDK path no longer exists on disk): `e6_locator_offline` 4000 checks
passed, `aircraft_auto_offline` and `runtime_safety_review` passed (3/3); `ee_camera_probe_win.cpp` (glue) compiled. The DLL was not linked; nothing ran live.

### 11.4 What a null live run must check (m03k airborne save)

`e6-locate;event=struct-scan` within ~2 s of the first enumeration with a hit at `block+CA811C` (J35) or `+CAAC6C` (F-5E) (any +0xC.. offset is fine: it moves);
`event=locked;...;kind=s` with the eye of the flown aircraft; `summary` `locks>=1;losses=0` in flight and `fallback-published` small; no `fallback-gate;state=qualified`
for a base whose samples do not move; `camera-admit` back to `correlated-hybrid` (`object-registration=allowed`) and `camlag` E6 eye = the aircraft position, not
2,2,2; `cockpit-auto` loads `f05e`/the flown type (no `stale-forget`); boxes `refused-view` near 0. Also a k00a/m02a run: `struct-scan` hit at `+FDD40C`
(= the `r` candidate, deduplicated), lock unchanged. Worker cost: the `struct-scan` `ms=` per pass and present-stats unchanged.
[I] Unverified live: whether the structure relocates mid-mission (would show as `lost` + rescan + relock), and the scan cost on the user's machine.
