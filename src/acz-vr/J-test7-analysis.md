# J-test7-analysis: test 7 (real Bigscreen Beyond, step30-dll, 21 keys) read-only analysis

Read-only: no source edited, nothing built or run. Tags: **[E]** established from a log line or code line cited here,
**[I]** inference from several [E] facts, **[G]** guess / unverified.
Logs: `research/experiments/L1-logs/20261024-224030/` (`xr-stereo.log`, `step5-producer.log`; the sibling
`20261024-223517/` is identical, `diff -rq` empty). XR time base: `t-ms = ts - 17980340`; the session runs about 245 s;
`ts` is the first column of each line.

## 1. Black flicker / world cut-out when pulling up

### 1.1 What the counters show
* [E] No consumer-side math or admission refusal at any time: `world-lens-invalid=0`, `anchor-rebases=0`, `zero-no-pose=0`,
  `should-render-false=0`, `cockpit-fov-invalid=0`, `summary-world-composite refused=0`, `end-failed=0`, `lens-valid=N/N`,
  hmd `fallback=0`, `clamped=0`, `quarantined:0`, `gpu-busy=0`, `skipped=0`, `deferred=0`. Pose was invalid only in the
  first 10 s (132 frames; `pose-valid=8608/8740` never changes afterwards). NaN / pose / lens / fence / quarantine causes
  are ruled out by counters.
* [E] **World-snapshot refusals, producer side**: final `world=valid:8686,refused:54,last-refusal:entry-state-unknown`.
  Bursts (deltas of the 5 s `continuous-quality` samples): +11 at t=110-115 s (one log line at ts 18093281,
  `world-snapshot;outcome=refused;reason=entry-state-unknown;world-valid=0`), +17 at t=160-165 s, +26 at t=165-170 s.
  Everything else is 0. The single reason is the last line of `step5WorldWhy` (`gpu_copy_step5_hooks.inl:817`): no tracked
  transition to RENDER_TARGET of the scene RT earlier in the SAME list generation (or the guard at
  `gpu_copy_step3_hooks.inl:168-171` cleared `world_rt_transition`: generation, watch epoch or serial mismatch).
* [E] **The consumer then draws no world for that frame**: `xr_stereo_output.cpp:805` `use_world = f.world_valid && ...`; the
  projection layer is OPAQUE (`proj-flags=none(opaque)`), so the background is black. XR `summary-admission` `depth` vs `world`
  differ in exactly those windows: t-ms 115271 (130 vs 125), 165461 (103 vs 93), 170475 (97 vs 78): 34 consumed frames with
  cockpit and no world; 20 of the 54 refused producer frames were never selected.
  Reused XR frames repeat the last composed frame, so one world-less producer frame stays black until the next new frame
  (new rate only 18-25/s in that part of the run) [I]: 10-26 consecutive refusals = 0.5-1 s of black world, which fits
  "black flickering / cut out".
* [I] The 110-115 s and 160-170 s windows are consistent with the user pulling up (marker frames drop 104 -> **0** at 110-115 s,
  i.e. no targets in view; 85-97 at 165-175 s). Pitch / look direction is NOT logged anywhere, so the link
  "sky -> entry-state-unknown" is unproven [G]. Plausible mechanism [G]: with a sky-only view the scene RT is first written by a
  different command list / pass structure (its RT transition lies in an earlier list or after a list split), so the cockpit
  list sees a carried (unproven) entry state, which J-155 deliberately refuses.

### 1.2 Other windows where the XR output goes empty (not pitch related)
* [E] t=30-35 s: `stale-zero=225 stale-age=225`, `submitted=137/362`, `reuse-age-max-ms=500`, producer ids 1761 -> 1846
  (85 vs about 290 in a normal 5 s); `first-marker-frame` at ts 18014640 (t=34.3 s). A mission-start load hitch: nothing was
  published for >500 ms and the consumer, by policy (`reuse-max-age-ms=500`), submits ZERO layers (SteamVR shows its void).
* [E] t>=215.5 s to the end of the log: producer `published=8740` frozen, `skipped=0` (no cockpit draw lists at all), XR
  `stale-zero` 378/379/329/344/342/345 per 5 s, `submitted=0`. The game stopped drawing the cockpit (pause / result / menu / exit) [I].
  Last pre-stall `reuse-age-max-ms=319` at t=215 s.
* [E] Cadence: `new-per-s` falls from about 45 (t<30 s) to about 34 (40-55 s) to 18-25 (t=100-215 s); `reused` up to 243/316
  (t=180 s, `reuse-age-max-ms=121`). Producer `lag` 0-1, `completion-ms-mean` 3-5: a slow producer, not GPU back-pressure [I].

### 1.3 The geometric cut-out that is always there (design limit; probably what "cuts out" means when looking up)
* [E] The world composite is a FLAT plane (`xr_stereo_blit.hlsl` `world_layer`), perpendicular to the anchor forward (yaw-only
  anchor, `neutral-yaw-deg=-60.70`, no pitch), 500 m away, half-extent from the game lens: +-31.60 deg horizontally, +-19.09 deg
  vertically (307.6 m x 173.0 m). Outside it the shader returns (0,0,0,0); with cockpit alpha 0 there the pixel is black. HMD eye0
  fov is -54.95..42.12 / -53.21..43.88 deg, so the plane misses **24.8 deg of the upper field, 34 deg of the lower, 11-23 deg per
  side even when looking straight ahead**. Looking more than about 19 deg up (natural when the aircraft pulls up) the view is black
  except for the cockpit. The 6% feather only fades the last 3% of the plane. [E geometry; I that this is the user's "cuts out"].
* [E] No division-by-zero / NaN path: ray condition `d.z < -1e-6`, `s > 0`, `wuv` range test, 17 constants checked finite
  (`world_composite_params`), `|fov| < 1.5 rad`. For this headset an upward ray has `d.z` about -1. Behind-plane hits and NaN give
  transparent, never garbage.
* [G] FP16 sky: the world RT is R16G16B16A16_FLOAT, tonemap=clamp, `max(rgb*exposure,0)` maps NaN to 0 (black) but Inf to white. A sun or
  glare writing NaN in the game RT would give black texels in sky views only. Cheap to test (count non-finite texels in a `png_once`);
  no evidence either way.

### 1.4 Proposed fixes (not implemented), in order
1. Hold-last-good world: when the snapshot is refused keep showing the previous world frame (a dedicated world texture GPU-copied from
   the last valid slot) instead of black; the world is 500 m away so a 50 ms stale world is invisible. Count `world-held-frames`.
   Removes the symptom whatever the cause. Risk: slot / fence lifetime.
2. Root cause of `entry-state-unknown`: on each refusal log which sub-condition failed (`rtTransition` false vs generation / watch epoch /
   serial mismatch), the list key, generation, draw ordinal and whether a Close / split happened since the previous accepted frame;
   run a null combat session that pitches up repeatedly. If it is "carried state", a policy change to J-155 (accept the bound RTV of
   the previous accepted scene RT) needs a DECISIONS entry.
3. Hold, not zero: for the >500 ms stale case hold the last frame (dimmed) instead of submitting no layer.
4. The real cure for the upper / lower / side black is the periphery fill (D-271 item 1): replay the world per eye at the HMD fov.
   Interim (shader constants only): pitch-follow the plane, or add a larger far surface sampling the snapshot edge so looking up does
   not leave the plane.

## 2. "Stretched horizontally": cockpit aspect with `gpu_copy_step5_hmd_fov`

* [E] Render and submit use the SAME tangents. Producer `hmd-fov outcome=applied eye0=-0.95909,0.73519,0.76586,-0.92861` rad
  (= -54.95, 42.12, 43.88, -53.21 deg) equals the XR `eye0-fov`; the consumer submits `f.cockpit_fov[e]` verbatim (`layer_from`,
  611-615; `cockpit-fov=hmd`, `cockpit-fov-frames` = world frames, `cockpit-fov-invalid=0`).
* [E] `build_vp` (`gpu_copy_step5.h:108`) maps the tan edges [tl,tr] x [td,tu] to NDC [-1,1] linearly, including the principal-point
  term `(tr+tl)/(tr-tl)`; the target keeps the game's full 2560x1440 viewport; the swapchain (2400x1350) is 16:9 with
  `scale=0.937500` on both axes and `mapping=full-rect`. A rectilinear image uniform in tan space, submitted with the identical fov,
  is geometrically exact: no aspect error exists in the code path [E, by reading].
* [E] Numbers: eye0 tan span 2.3297 x 2.2989 (ratio 1.013), eye1 2.3240 x 2.3046 (1.008): the HMD view is nearly SQUARE in tan space
  while the image is 16:9. Pixel density is 1030 px/tan horizontally vs 587 vertically (x1.754 anisotropy; lens mode was isotropic at
  1951 px/tan). That is a resolution deficit (vertical 3.3x, horizontal 1.9x lower than lens mode), not a stretch; predicted in
  `J-cockpit-fov-consumer-findings.md` ("Sharpness"). The runtime recommends 2688x2688 per eye (`recommended=` in the swapchains line), unused.
* [I] Likely origins of the impression, none provable offline: (a) rectilinear wide-angle perspective at about 97 deg per axis (the
  periphery looks stretched in any rectilinear render; the user also says the cockpit sides are not drawn); (b) vertical softness read as
  horizontal elongation; (c) HUD / world / markers stay lens-fov quads (63x38), so next to the 97 deg cockpit they look wide [G].
* [G] An anamorphic factor hidden in the game's lens ratio (tanX/tanY = 1.7777, exactly 16:9) would also have affected the lens mode the
  user accepted; unlikely.
* Test (null driver, no headset): with `gpu_copy_step5_hmd_fov` on and off, `png_once` the eye image; take a round cockpit element (gauge
  bezel, HUD ring) and compute `(w_px/1030)/(h_px/587)` for hmd mode, `(w/1951)/(h/1951)` for lens mode; both must be 1.00 +- 2%. If hmd mode
  reads 1.00 there is no stretch bug. Also add a `layer-fov` log line per 5 s with the submitted fov (today `first-layer-fov` prints the lens fov
  only; the hmd fov appears in `summary-timing`).
* Mitigation independent of the answer: size the eye swapchains from the HMD fov (2688x2688 recommended or 2400x~2370) for isotropic density; the
  producer RT (2560x1440) would need the same shape: a bigger change, defer.

## 3. Radar blips, missiles, explosions, tracers: where are they drawn?

* [E] Per-frame order (J-hud-split-analysis, stored-draw-manifest-2): world draws (DrawACM, pass 6 ordinals 35-51, with exhaust / flame-like
  discs 42-46) -> **first cockpit member = world snapshot point** -> 34 cockpit members (DrawCPT, 52-68) -> HUD region (about 58 draws, after the
  LAST member until the FIRST RT change, `end=rt-change` 100%) -> tone-map `a3050` -> late scene-target tail with stencil EQUAL (`03510`, `ad6c0`,
  varying counts). World-objects observer in test 7: `seen=68619...`, `before-frame` = all, `frames=0 members=0`: every DrawACM draw precedes the
  frame opening, so nothing is ever observed as a member.
* [E] Test 7 had no HUD refusals (`producer-hud-refused=0`; `hud-draws` 49-65, peaks 61/65 at t=105, 125, 140, 215 s). Effect sprites inside the HUD
  region would have been replayed onto the HUD quad; the user sees none, so they are not in the region [I]. Weapon use in the run is not logged [G].
* [I] Effects must be in one of: (A) between the first and the last cockpit member (not in the snapshot, not in the HUD region: lost); (B) after the
  HUD region end (tone-map and later; also rejected by `hud_decide` as `stencil-test-not-always` / `depth-test-not-replayable`); (C) before the first
  member with state the snapshot lacks (unlikely, the snapshot is a full RT copy). Radar blips: J-hud-split H-A (the stencil-EQUAL late tail:
  `03510` I14/I19/I99 and `ad6c0` I5 are the only draws whose index count follows game state) remains the leading hypothesis; [E] the radar group
  53-63 is constant per frame.
* Smallest read-only observation (extend `gpu_copy_step5_hud_observe`, default off, no replay change):
  1. phase tag on every observed draw: P0 before first member, P1 between members (previous/next member index), P2 HUD region, P3 after region
     end before the first RT change, P4 after the tone-map / viewport change, up to Close; raise the cap to 256 for P1, P3, P4;
  2. per record: RT class (scene FP16 / R10 output / other), PSO id + VS/PS hash, topology, index / vertex / instance count, blend (src, dst, op,
     alpha ops, blend factor), depth func + write, stencil func + ref, viewport;
  3. `continuous-hud` counters: draws per phase, `late-scene-draws-after-region-end`, distinct PSOs per phase;
  4. emit full record lists only when a phase-count signature moves by more than N from the previous frame (plus the first 3 frames), so a missile
     launch or explosion shows its new draws;
  5. null combat run: fire the gun (tracers), launch a missile at a lock, destroy a target, radar with enemies; `png_once` of the scene RT at the first
     member, at the region end and of the final output (3 images) to place the effect pixels in time.
  Success = a table (tail_seq / phase / PSO / count / blend) before and after each event; the draw whose count appears only after the event is the effect
  and its phase decides the fix (A: snapshot after the last member or a second snapshot; B: extend the region and add blend replay).

## 4. Prioritised work list for the night

| # | Item | Owner | Risk |
| - | ---- | ----- | ---- |
| 1 | Hold-last-good world on refusal plus `world-held-frames` counter | code agent (producer + consumer), then null-driver verify | slot / fence lifetime; low with a dedicated copy texture |
| 2 | Refusal diagnostics: failing sub-condition, list key, generation, ordinal; per-5 s pitch proxy (game camera forward y from `PoseFrame.cam.fwd`) next to the counters | code agent (logging only) | very low |
| 3 | Null-driver run: repeated pitch-ups and a sustained climb with 2's logging; confirms or kills the sky -> entry-state-unknown link | live run (flight input, D-271 authority) | game time, driver pitch input |
| 4 | Phase-tagged observer (section 3) and a combat null run with PNGs | code agent then live run | observer cost on the game thread (rate-limit), memory cap |
| 5 | Plane interim: pitch-follow or larger far surface so looking up is not black (shader constants + CPU mirror test) | code agent (consumer + hlsl) | seam duplication; keep the mirror test in step |
| 6 | Stretch test (section 2): ellipticity in lens vs hmd PNGs, `layer-fov` log | live run (PNG) + code agent | none; decides whether any fix is needed |
| 7 | Periphery fill: open the frame at the first world draw (MemberPhase split exists), replay DrawACM per eye at the HMD fov | code agent (large), review by the other family | highest: state tracking, 1100-command cap, pre-projected `ps2_v` draws are not replayable (manifest-2) |
| 8 | Stale policy: hold-dimmed instead of zero layers for hitches >500 ms | code agent (consumer) | low |
| 9 | Eye swapchain / RT shape from the HMD fov (isotropic density) | code agent | memory, HUD / marker coupling; defer |

Not established by this analysis: any link between head or aircraft pitch and the refusals (not logged); weapon or effect presence in the test-7 frames;
that the user's flicker is the refusal bursts rather than the plane edge or a hitch; any aspect error.
