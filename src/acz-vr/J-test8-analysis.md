# J-test8-analysis: test 8 (real Bigscreen Beyond, step34-dll, 23 keys, no world hold/extend) read-only analysis

Read-only: nothing built or run, no tracked source edited. Tags: **[E]** established from a log line / code line / image cited here,
**[I]** inference from several [E] facts, **[G]** guess. Logs: `research/experiments/headset-test8/` (`xr-stereo.log` 594 lines, `step5-producer.log`
1188 lines, `diagnostics.log`). Time base used below: producer `ts - 97428171` ms (about 311 s of log). Both logs END without any `*-result` line
(`grep result xr-stereo.log` = 1 hit, the config line): the process was ended without a clean shutdown, so every consumer counter that is only
emitted at shutdown (`effects-result`, `world-composite-result`, `hud-result`) is MISSING [E].

User feedback (verbatim, test 8): tracers/smoke/explosions NO; world behind cockpit "no on front, but the hud renders on the world again";
flicker "yes but not as badly"; black box still there but "buildings and planes now render outside of it"; markers spot on; stretch gone;
"cockpit and aircraft still only renders with head turns"; no radar blips. Test 7 (D-271): "World - on front of cockpit now. Doesnt feel crazy far."

## 1. "World in front of the cockpit"

### 1.1 What the compositor does with alpha [E, code]
* `xr_stereo_blit.hlsl:ps_main` (mode 2/3): `a = depth_texel > 0 ? 1 : 0` (nearest texel of the eye depth plane, `use_depth` on);
  `o = feathered(float4(c*a, a), uv)`; `out = o + (1 - o.a) * world_layer(uv)`. The cockpit COLOUR is multiplied by `a`: wherever the
  eye depth is 0 the cockpit colour is DISCARDED and the world shows; wherever depth > 0 the cockpit fully replaces the world. Alpha is
  binary 0/1 (no partial alpha except the 6% `xr_edge_feather` ramp at the eye-image border, ~3% of each side). There is no additive path
  between cockpit and world; the world can never add on top of an a=1 pixel. So at the shader level "world over cockpit" is impossible for
  pixels with depth > 0; it can only happen for pixels whose depth is 0 (cockpit colour exists but no depth was written) or whose depth was
  written by something that is not cockpit.
* The depth plane is cleared to 0 (PS2 GREATER) per slot. Producer `swapchains ... alpha=depth-coverage-premultiplied` [E, xr log line 10].
* Reference image [E]: `step14-live-run11-full/png/...s2-eye0.png` (colour only, no depth): green canopy arcs, two struts, HUD combiner frame,
  AoA gauge, glare-shield console. The large dark region (canopy interior behind the struts, and the brown combiner glass) is dark grey colour; the
  run 11 NOTES give `eye0 alpha-gt0 37.6%`, i.e. only 37.6% of pixels have depth: canopy frame, struts, gauge, console are covered, the
  dark interior and the combiner glass are not (those draws are non-member or no-depth draws). So the cockpit opaque solids have alpha 1 [I from
  37.6% and the image shape] and the dark "window" area correctly shows the world.

### 1.2 What is NEW in test 8 versus test 7 [E]
* `gpu_copy_step5_world_objects` is now EFFECTIVE: producer `continuous-world`: `frames=12252 members=363260 avg-per-frame=29.65`, `early opened=12253
  adopted=12253` (test 7: `frames=0 members=0`, every draw "before-frame"). The producer now replays up to 29.6 DrawACM world draws per frame INTO THE
  COCKPIT EYE IMAGE at the HMD fov, per eye, with REAL depth writes (`DrawACM_v depth GREATER_EQUAL w1`, step32 novel key `cd184480`). That is why the
  user sees buildings/planes outside the 63x38 window [E: user]; those pixels have alpha 1 (depth written) so they are drawn over black.
* Consequence [I, strong]: these objects are drawn by the same eye RT/depth as the cockpit, BEFORE the cockpit members (phase `before-first-member`). A
  cockpit pixel survives only if the member's depth is GREATER than the object's depth stored there. Object depth is `z_s = b*w` from the world record (b about
  -255.008, `J-world-objects-findings.md`: "the VS takes depth and w from A only", "depth func/write may differ") whereas cockpit depth comes from the cockpit
  matrices. Nobody has proven the two share one depth scale. If an object's depth exceeds the cockpit's at a pixel the cockpit is overwritten by world
  geometry, i.e. the world is IN FRONT of the cockpit, exactly where the user also reports buildings/planes. [G] This is the single new code path since test 7,
  so it is the leading explanation for "world in front, again" IF "in front" means "over the cockpit". Not established: nobody has looked at a depth image.
* Dropped world draws [E]: `refused=...cap:30999,target:48125` of 363,260 recorded + 79,124 refused; cap = 48 draws per frame, so about 2.5 draws/frame are
  dropped (8% of recorded) every frame: objects pop in/out with the frame mix. Also 142 frames `world-pose-unavailable` at the start. This is a flicker source (small).

### 1.3 Other candidate readings of "in front"
1. "On front" = "in front of me, out through the canopy" (test 7 wording; D-271 took it as correct). Then the answer to "behind cockpit?" is simply "it IS
   behind/beyond, as designed" and the complaint is only "the HUD renders on the world again". [G] Ask the user: does any cockpit part (canopy rail, console,
   gauge) get covered by the world/terrain?
2. The HUD quad (xr_hud_quad_distance_m=0.8, scale 0.5) and the markers quad (500 m) are separate quad layers submitted AFTER the projection layer
   (`order=quad-then-markers-then-projection-then-hud` in the markers line is the log's name; the HUD is last) so they ALWAYS draw over the cockpit. [E] The HUD radar
   background is an opaque black square (run 11: radar alpha ~1, `hud-alpha-raw.png` solid white 313x320) and the HUD is `max(a, max rgb)` alpha. At 0.8 m the quad
   is NEARER than much of the cockpit (console/glare-shield), so the HUD both paints over the cockpit and has stereo depth in front of it; with the world 500 m
   behind, the HUD "on the world" impression is a layer-order/depth conflict: it is drawn over cockpit frames that are physically closer than 0.8 m? [I] Sizes in
   game units are not logged; unverified.
3. Stereo perception: the world plane at 500 m has near-zero disparity, cockpit about 0.5-1 m: depth ordering is physically right. [I] Not a candidate.
4. Alpha 0 on cockpit pixels that are drawn by NON-member PSOs (members_by_class replays only the 2 selected PSOs, `member-classes ... psos=2`): instrument faces, glass
   and decals drawn by other PSOs have colour only if they are members; non-members are absent from the eye image altogether (alpha 0, no colour), so the world
   shows where such a part should be. [I] That reads as "the world is in front of / replaces that cockpit part" for the console/gauge faces. The effects layer (non-member
   blended draws between members) now captures these as 3.00 constant draws/frame (see section 2) and composites them BEHIND the cockpit, over the world, which keeps
   them looking like "world-side" content.

### 1.4 Most probable cause and fix
* Most probable [I, not proven]: (a) world-object depth overwrites cockpit pixels (1.2), and/or (b) cockpit parts that are non-members/depth-less show the world
  (1.3-4). The ordering question is unanswerable from the logs: no image of depth or final composite exists for test 8; the log has no `png_once` (key absent in the 23 keys).
* Fix 1 (producer, cheap, decisive): draw the world objects into the eye RT WITHOUT depth write (or with depth write to a separate DS) and clear the cockpit depth
  AFTER the world pass: i.e. a clear of the depth plane at the first cockpit member (`ClearDSV` depth 0 before the first member). Then no world pixel can win a depth test
  against the cockpit; the cockpit is always on top; the objects keep alpha via a separate coverage mask. Needs a coverage plane: write alpha from objects via colour alpha or a
  stencil bit, since alpha is currently derived from depth. Cost: one extra clear command per frame.
* Fix 2 (consumer, cheap, diagnostic AND mitigation): add a shader mode that composes world-object pixels BEHIND the cockpit by a second depth plane or a second RT only if (1)
  is confirmed.
* Diagnostic A (consumer, default-off key `xr_png_once`): save, once at frame N (and N+60) the final composited eye 0 image (the exact `out` of ps_main, i.e. what the
  compositor receives) as PNG, plus the eye colour, the eye depth-coverage plane and the world snapshot. Needs a copy of the eye swapchain image to a readback buffer after the
  blit; same machinery as the producer `png_once`. This lets the executor read, on the null HMD, whether the cockpit parts are there.
* Diagnostic B (shader debug mode, key `xr_debug_view=N`): 1 = output `float4(a, a, a, 1)` (depth coverage), 2 = cockpit colour only (a forced 1), 3 = world only, 4 = false
  colour `(a, a*?, world)`: red where cockpit alpha 1, green where world shows, blue where the effects layer writes. Combine with Diagnostic A so a null-HMD run
  can prove the cockpit alpha map without a person.
* A/B test to isolate (one headset run, 2 minutes): same 23 keys but `gpu_copy_step5_world_objects` off (and `xr_look_drive_keys` off). If the world is still over the cockpit, 1.3-4 is
  the cause; if not, 1.2 is.

## 2. Effects layer: did anything composite?

### 2.1 Counters [E]
* Producer `continuous-hud-effects` (first line / last line): `frames=valid:12394` of 12394 (100% frames have a committed effects image), `refused=0(state:0,descriptor:0,format:0,cap:0)`,
  `const-blend=0`, `ignored=0`, `epoch-cap-frames=0`, `commits=12394`, `speculative=763544`, `tail=717745`, `draws=45799`, `avg-draws=3.70`.
  So 763,544 speculative replays happen but only 45,799 (6%) are COMMITTED (between-members epochs); 717,745 (94%) are tail replays that stay predicated off (as designed).
* Per 5 s: committed draws/frame are EXACTLY 3.00 in most windows (the static baseline; e.g. t=115-150, 165-311 s: `perF=3.00`). The static 3.0 includes whatever the game draws
  between members every frame (glass/reflection/decal sprites), not a weapon effect.
* Bursts above 3.0 [E]: t=0-10 s 8.53/frame (loading/intro cockpit), t=10-15 5.94, t=55 3.60, **t=75-80 12.19**, 80-85 4.83, 85-95 2.9-7.0 (at 90-95 `6.99`), 100-105 3.3.
  Not in any window after t=100 s: 100-311 s are 3.00 exactly. HUD draws per frame show combat-like variation only to t~100 s (47.8-57.9) then are constant 55.0 (115-150) and
  52.0 from t=170 s to the end (`continuous-hud avg-draws` deltas): the scene is static from t=170 s (paused / mission end / result screen) [I]. So the only plausible window for gun/missile
  use is 55-100 s (and the first 15 s); the +9 draws/frame during 75-80 s is the signature to expect for a tracer/smoke burst (compare step32: tracer quad 1/frame, smoke strips up
  to 5/frame, `a5e7939a` up to 7/frame; sum about 8-13). [I] weapons were used at 75-95 s and the classifier DID commit them (avg 12.19 in that window).
* `last-refusal` flips between `stencil-test-not-always`, `effects-cap`, `stencil-write`, `depth-write` while `refused=0`: [I] all refusals occur in the TAIL epochs (after the last cockpit member:
  HUD region and after-tonemap draws, 59-63 candidates per frame, hitting the 64-per-frame replay cap `effects-cap`), which are discarded by predicate and not counted as refused
  (`effects_refused` counts only refusals in COMMITTED epochs). So the refusals are harmless HUD/tail draws, not effects. [E counters; I interpretation]
* Consumer [E]: only `effects-config` (order world-then-effects-then-cockpit) was logged. There is NO per-5s effects counter and `effects-result` is only emitted at shutdown (absent).
  Whether mode 3 ran (effects_frames), was skipped (`held`, `!effects_valid`, `effects_invalid`) or fell back to mode 2 is UNKNOWN.
  Cannot tell from this log whether the consumer composited anything. Fix: log `effects-frames/skipped/invalid` in every `summary-world-composite` line (5 s).

### 2.2 Could the classifier drop the real draws? (step32-live-run1 `phase-observe-novel`, window 2 and 3 after the mark)
| key | phase | VS/PS | topology/count | blend | depth | stencil | classifier verdict (J-effects-layer rules) |
|---|---|---|---|---|---|---|---|
| 1ee6c5da | between-members | 534f29ca/d34ee153 | unindexed 4 (win 2, 409 frames, 1/frame); indexed 348 (win 3, 569 frames, up to 5/frame) | src SRC_ALPHA(5), dst ONE(2) = additive | GREATER, write 0 | ALWAYS | PASS (GREATER allowed, no depth/stencil write, no const blend) |
| a5e7939a | between-members | 9eff1f92/d2464ed2 | unindexed 1 (260 frames, up to 7/frame) | 5 / 6 (alpha) | GREATER, w0 | ALWAYS | PASS |
| cd184480 | before-first-member | DrawACM_v | indexed 44 | 7 (DEST_ALPHA) / 2 | GREATER_EQUAL, **w1** | ALWAYS | NOT a candidate (DrawACM identity); depth write; it is a 3D world object (smoke/flare model) handled only by world-objects (before the snapshot => ends up in the snapshot) |
| d7db8d56 | hud-region | 534f29ca | indexed 9 (351 frames, 1/frame) | 5/2 additive | GREATER, w0 | ALWAYS | HUD, not effects: `effects_decide` ignores/refuses draws outside open between-phase; in the HUD it is accepted (GREATER). It is replayed on the HUD quad at 0.8 m, NOT in the world |
| 116df34f | hud-region | 534f29ca | indexed 4 (210 frames) | 5/2 additive | ALWAYS | **EQUAL** | refused `stencil-test-not-always` (HUD sees refused=0 in test 8, so this draw did not occur in test 8 or never reached the HUD classifier) |
| c71e0504 | after-tonemap (R10/scene tail) | 534f29ca | unindexed 4, up to 33/frame (window 2), 106 (window 1) | 5/6 | ALWAYS | ALWAYS | after region end: NOT replayed anywhere (HUD region ended at RT change; effects tail epoch has no later member so it is predicated off) |
* Result [I]: the between-members tracer/smoke draws (`1ee6c5da`, `a5e7939a`) are the best candidates and DO pass the classifier. `depth-write` refuses nothing of them (w0).
  The depth-write refusals (`last-refusal=depth-write`, t=265 s) and stencil-write (t=205, 306) come from the tail, not from tracers.
* BUT: the 64-per-frame replay cap `effects-cap` is reached EVERY frame by tail candidates (tail 59.7 + committed 3.0 > 62 of 64). Candidates are counted in arrival order
  (between-members first, then the 52-55 HUD draws, then the tail), so the between-members draws are inside the cap EXCEPT when a heavy burst (12.19 + 52 > 64) fills it; the
  cap then drops the LAST draws in submit order = the late tail, but ALSO the later effect draws of the frame. In the 75-80 s window (3+9 draws + about 52 HUD) 64 is exceeded,
  so [G] some effect draws are cut. Fix: do not count tail/HUD-region candidates against the effects cap (HUD region has its own cap); only count draws that enter an epoch.
* Another reason for "none visible" [I]: (a) additive draws composited as "over" using `max(a, max rgb)` after a zero-cleared RT with SRC_ALPHA*src: colour = rgb*a, alpha =
  (blend alpha ops not recorded); additive glows come out dim and over-blended onto an opaque world: faint but not invisible; (b) tracers are SMALL (quad of 4 vertices at range) and the
  world window is a 63x38 deg plane magnified to ~97 deg: tiny; (c) the effects image is only sampled for WINDOW rays (outside the window: no effects), so effects at the edge fade with the
  window; (d) the consumer may not have run mode 3 at all (counters missing). [G] None proven. The log cannot distinguish "effects image empty" from "composited but invisible".
* Diagnostics to add: effects frames/skipped/invalid in every 5 s summary; `png_once` of the effects image (producer: key `gpu_copy_step5_png_once` already lists `hud,world,eye0`;
  add `effects`); producer log line per frame when committed draws > baseline+2 with the PSO ids of the extra draws (key + count + blend) and the epoch index. Then a null-HMD combat run shows whether
  the effects RT has pixels during firing.

## 3. Radar blips
* [E] J-hud-split-analysis: the radar group (draw ordinals 53-63) is constant per frame (rings, cross, cone lines, one hollow triangle): "no per-contact draw". The HUD region ends at the first RT change
  (`end=rt-change` 100%); `hud avg-draws=52.5` in test 8, `refused=0`. In test 8 the HUD never refuses, so blips are not refused: they are not in the region [E].
* [I] The only draws whose count follows game state and that sit after the region: `c71e0504` (after-tonemap, 4-vertex unindexed, blend 5/6, depth ALWAYS, 33-106 per frame) and the stencil-EQUAL
  `03510`/`ad6c0` family (varying indexed counts I14/I19/I99/I5, per J-hud-split-analysis). Stencil EQUAL = clipped by a mask written by the radar-disc quads (`dedc0` REPLACE) =
  consistent with radar contacts clipped to the radar circle. [G] `116df34f` (hud-region, EQUAL, additive, indexed 4) may be the lock/cone sweep.
* How to capture: (1) after the HUD region ends, continue replaying "late tail" draws into a second HUD-like target with the private DS keeping the stencil written by the replayed
  `dedc0` quads (the HUD DS already carries it) and allow stencil EQUAL when the stencil mask quads were replayed in the same frame (replay stencil state exactly); the draws are on the
  scene target after `a3050` (the tone-map copy): record them to the HUD image with the blend state of the draw. (2) Before building (1): extend the phase observer with RT identity,
  per-draw VS/PS hash, topology, index count, blend with ALL factors/alpha ops, stencil ref, and log the draws of the after-tonemap phase ONLY when their count changes by more than N (to catch blips
  appearing). Run with enemies visible and radar contacts. (3) Alternative quick test: `png_once` of the scene RT at the end of the frame, after the after-tonemap draws: blips (if drawn on the
  scene RT) would be pixels at the radar position.

## 4. Black flicker when pulling up
* [E] World snapshot refusals (producer, no world hold): total `world=valid:12366,refused:28,last-refusal:entry-state-unknown` in 12394 frames (0.23%, vs test 7: 54 of 8740). Bursts from the
  `continuous-world-refusal` series: 1 at ~t=103 s (`world-snapshot;outcome=refused;reason=entry-state-unknown;world-valid=0;barrier-calls=0` at ts 97531218), +19 at t=155-160 s, +8 at 160-165 s.
* [E] The new sub-condition and pitch diagnostics are BLIND for these refusals: `entry-state-unknown=0;sub=none;bursts=0;details=0/0` and `continuous-world-pitch refused=n=0` while `refused=28`
  (and `accepted=n=142,cam-fwd-accepted y:0.04..0.04`). Cause [E, code]: `step5NoteWorldPitch` is called only from the non-early first-member path (`gpu_copy_step5_hooks.inl:2049`);
  with `gpu_copy_step5_world_objects` the frame is OPENED EARLY (opened=adopted=12253, `step5Adopt`), which never calls it, and the refusal counter (line 2332) counts those frames. The 142
  `accepted` samples are the 142 frames before the early path took over (`pose:142` / `world-pose-unavailable` 142). So NOTHING about pitch correlation is known for test 8. The pitch hypothesis
  from test 7 is unproven again.
* [I] Windows: the 155-165 s bursts coincide with the HUD draw count dropping to 49.7 then settling at a static 52.0 for the rest of the run, and the effect draws going to exactly 3.00: an end-of-mission /
  pause transition, not necessarily a pull-up. The single refusal at 103 s is in the combat window. So the refusals in test 8 are rare (28 frames = <1 s).
* [E] Other flicker sources in test 8: new-frame rate falls from about 50/s to about 14/s (`new` per 5 s: 250 at t=15 s, 100 at 150 s, 68 at 300 s; `reuse-age-max-ms` 120); 142-frame start
  pose loss; world-object cap/pop-in (section 1.2); `world-lens-invalid=0`, `end-failed=0`. The user's "not as badly" fits fewer refusals (28 vs 54) but the dominant black is the plane geometry: the window is
  63x38 deg (`lens-fov-deg=63.20x38.17`) in a 97 deg view; pulling up leaves the plane (black box). The world replay of objects partly fills it.
* With world hold + extend now default ON (`xr_world_hold_last`, `xr_world_extend`): the refusal flicker (frames with no world) is covered by hold (750 ms hold, fade); the plane edge by extend.
  Still needed: (1) call `step5NoteWorldPitch` also from the early-adopt path (record refused frames, entry sub-condition `live.world_entry_sub`, head pitch, cam fwd) so the next run says whether refusals correlate
  with pitch; (2) raise/removal of the world-object cap (`cap:30999`) or prioritise by distance; (3) producer speed: the new-frame rate decline (50 -> 14/s) must be understood (effects predication + world objects
  cost; world objects add about 30 draws/frame at 2 eyes) because slow producer frames show as judder/flicker regardless of the world; (4) check `world-held-frames`/`world-extend` counters after the run.

## 5. "Cockpit and aircraft only render with head turns" and the keyboard look drive
* [E] `xr_look_drive_keys` is not in the 23 keys of test 8 (D-273; it exists as `-LookDrive` in `tools/live/vr-test-start.ps1`, default off). The cockpit parts the game does not draw for its own camera direction
  (frustum culling by the game's look) cannot be replayed (option 3 of D-266/267). The look-drive injects NumPad 4/6/8 key events so the game's own look follows the head.
* Validation steps next [E: J-look-drive-findings]: (1) null-HMD run `-LookSweepNoXr` (no XR; sweep test) then `-LookDrive -LookSweep` on the null HMD (SteamVR static pose): check `look-drive;` lines every 1 s
  (`look-src=measured,measured`, `foreground=yes`, `residual-rms<8 deg`), (2) then a headset test with real head, short and with the kill switch (`look-drive.stop`) ready.
* Risks in a headset test [E/G]: (a) FOCUS: the keys only go to the game if the game window is the foreground window; under SteamVR, SteamVR can steal focus; then `foreground=no` and nothing is sent (no look). The
  user wears the headset and cannot click the desktop: bring the game window to the foreground before putting on the headset. (b) Stuck key if the process is killed while a NumPad key is down (Windows keeps it down
  until a physical key-up; the cap makes a normal hold <= 400 ms; a crash can leave one down): tap the NumPad key. (c) Sawtooth: a binary key at frame cadence gives about 20-25 deg p-p (7 deg RMS) on the rendered look,
  and the visible window edge flutters; the world plane follows the measured look, cockpit parts are exact if the measurement is right. (d) Double rotation: head minus rendered look compensation unverified live
  (M256 rows 0/1 assumed). (e) Throughput: at 40 fps the residual is 10.4 deg RMS vs 7.7 at 55; test 8 had new-frame rates of 14-50/s, so the controller sees low rates and residual grows. (f) The game's
  look pitch up saturates at 1.57 rad; pitch down is about 0 (D-273): looking down will not render the lower cockpit. (g) Input conflicts with the controller/joystick used for flight; NumPad keys must not be bound to anything
  in the game. (h) Nausea: lag between head and rendered look; run with world hold/extend on to hide the black.

## 6. Summary (20 lines)
1. [E] Shader alpha is binary depth coverage; colour is multiplied by it; the world can never overwrite a cockpit pixel with depth. [E] New in test 8: world objects replayed into the cockpit eye image with depth writes.
2. [I] "World in front" most probably = world-object depth overwriting cockpit pixels and/or non-member cockpit parts missing (alpha 0). [G] Alternatively "on front" means "out front" (test 7 wording). Needs the user's one-line clarification.
3. [E] HUD and markers quads are always composited over the projection layer; the HUD is at 0.8 m with an opaque radar square: "HUD on the world" is expected layer behaviour.
4. [E] No image/depth artefact of the compositor input exists for test 8: add `xr_png_once` (final eye image) and `xr_debug_view` (alpha as colour).
5. [E] Effects: committed in 100% of frames, 3.00 draws/frame static baseline, bursts to 12.19/frame at t=75-80 s: weapon-like draws WERE committed (the classifier passes `1ee6c5da` and `a5e7939a`).
6. [E] The consumer effects counters are missing (no clean shutdown); whether mode 3 ran is unknown. [I] the 64-draw `effects-cap` is filled by tail candidates every frame and may cut effect draws during bursts.
7. [I] Effects would be small/dim (additive via `max(a, max rgb)`, window rays only). Not proven.
8. [E] Radar blips are not in the HUD region (refused=0); [I] they are among the after-tonemap stencil-EQUAL / `c71e0504` draws, never replayed.
9. [E] World refusals: 28 of 12394 frames (test 7: 54 of 8740); [E] the new pitch/sub-condition diagnostics are blind under world-objects early start (call site only on the non-early path).
10. [E] New-frame rate drops 50/s -> 14/s over the run; world-object cap drops ~2.5 draws/frame (cap:30999): pop-in.
11. [I] The 160-165 s refusal burst coincides with a transition to a static scene, not necessarily a pull-up.
12. [E] `xr_look_drive_keys` not enabled; it is the only designed route to cockpit parts that render only with head turns; validate on null HMD first; focus is the main risk.

## 7. Prioritised fix list (owner)
| # | Item | Owner | Risk |
|---|---|---|---|
| 1 | `xr_png_once` of the final composited eye image + `xr_debug_view` (alpha as colour) + effects counters in the 5 s summary + shutdown-independent `effects-result` | code agent (consumer) | low |
| 2 | Call `step5NoteWorldPitch` from the early-adopt path (sub-condition, pitch) | code agent (producer, logging only) | very low |
| 3 | A/B headset or null-HMD run: world_objects off vs on; read the PNG/alpha map | live run executor | none |
| 4 | Depth isolation for world objects: clear the eye depth at the first cockpit member (or no depth write for objects) with a coverage alpha for objects | code agent (producer), then review by the other family | medium (alpha derived from depth must be replaced by a coverage plane) |
| 5 | Do not count HUD-region/tail candidates against `effects-cap`; log effect-burst PSO ids; add `effects` to producer png_once | code agent (producer) | low |
| 6 | Radar: phase observer with RT id/blend/stencil for after-tonemap draws, then late-tail replay with stencil EQUAL on the HUD DS | code agent, then null-HMD combat run | medium |
| 7 | World-object cap 48 -> 64 with distance priority (stop pop-in); investigate producer new-frame rate fall (50 -> 14/s) | code agent | medium (command cap 1100) |
| 8 | Look drive: null-HMD sweep validation, then a 2-minute headset test with the game window foreground, kill switch ready | live run executor, then user | focus / stuck key / sawtooth |
| 9 | Ask the user: does "on front" mean the world covers cockpit parts or just out front? Does the HUD sit on the world window instead of near the face? | programme / user | none |

Not established by this analysis: the meaning of "on front"; whether world-object depth conflicts with cockpit depth (no depth image); whether the consumer ran mode 3; whether the burst at 75-95 s was weapon fire;
any pitch correlation of the refusals; what draws the radar blips (only candidate draw families).
