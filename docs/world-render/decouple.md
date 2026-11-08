# DECOUPLE: the composite on every XR frame at that frame's own head pose (decouple:1)

User goal (D-340): smooth VR; frame rate matters most; invasive changes accepted. D-342's correction: the consumer composed terrain/sky/cockpit/effects
only when a NEW game frame arrived; on the other headset frames it re-submitted the last images with their render pose (`Output::pace`), so SteamVR's
reprojection corrected head ROTATION only and the cockpit and terrain juddered under 6DoF head motion at the game rate (60, or 12-16 in the decayed
state). This track composes on every XR frame.
Offline work only. Nobody launched the game, SteamVR or a headset. A driver agent runs the live plan in section 7.
Tags: **[E]** = established here (code reading, offline tests, WARP, the GPU bench on the RTX 4090). **[I]** = inference, not verified in the game.
Code: `src/acz-vr/xr_stereo_output.cpp` (`Output::compose`, `no_new_frame`, `dc_*`, the pure `reproject_*` / `camera_delta_rotation`),
`xr_stereo_output.h` (`Reproject`, `BlitJob::reproject`, `DecoupleState`), `xr_stereo_blit.hlsl` (`dc_source_uv`, `dc_rotate`, cbuffer b2),
`xr_stereo_d3d12.cpp` (root parameter 3 = b2, `root_reset`), `frame_pacing.h` (options). Tests: `xr_stereo_decouple_offline`
(= `xr_stereo_output_offline_tests --decouple`, also in the full run), `frame_pacing_offline`, WARP harness case `[d] DECOUPLE`, `xr_gpu_bench --decouple`.

## 1. Option

Inside `xr_terrain_stage` (no new config key; 31 of 32 keys stay): `decouple:1` (default off), `dcage:<20..500>` (ms a held game frame is composed
again, default 250), `dcms:<5..150>` (terrain-camera dead-reckoning clamp, default 70), `dcfade:<0..0.2>` (edge fade width of the re-projected objects
image in source uv, default 0.02). Removed before the terrain parser sees the value, like `interp:`. Intended live value (72 characters, under the
100-character limit): `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;interp:1;decouple:1'`.
It needs the terrain composite (`xr_world_in_projection` + a loaded stage); without it it is inert (tested). With the option off nothing new runs:
`no_new_frame()` is exactly `pace()`, every `fresh` guard is true, the b2 constants are zero and the shader takes exactly the previous path
(Release ctest 315/315 unchanged before any new test was added; the WARP harness' existing byte-identity and CPU-reference checks unchanged). [E]
Exception (review 3, Astra 6443240380 open risk 1): the source-validity check of a frame's FIRST composite (section 2) runs with the option off too;
it only changes the output when the producer cannot prove the images are the published bytes. [E code; not built or run by the review-3 author]

## 2. What was resolved in the consumer (code reading) [E]

* **Slot ownership and the immutable-image contract (corrected, review 2).** The producer (`Step5Source`, `gpu_copy_step5_hooks.inl`) marks a slot
  HELD in `try_acquire_latest`; `step5PickSlot_locked` only reuses FREE slots or PUBLISHED slots superseded by a newer frame, so a HELD slot is never
  picked for a NEW frame. **HELD alone does NOT make the images immutable**: the game may Execute the already injected recording that wrote the slot
  again (J-180 P1, `step5BeforeExecute`), which renders into the same images; the producer then quarantines the slot (a held consumer cannot be
  revoked). Earlier text here ("HELD slots are never written") was wrong (Astra 6442582596 #1). The contract a re-composite relies on is
  `FrameSource::images_unchanged(frame)`: true only while the slot is still HELD for that frame id and no Execute went unexamined since the frame's
  SUBMISSION (review 3: `Step5Slot::pubUntracked` is the `Step5State::untrackedExec` value taken when the frame was submitted; acquisition never
  resets it, and a PUBLISHED slot with an unexamined Execute after its submission is never acquired). Unexamined = producer disabled, oversize
  batch, null list array, or ANY Execute forwarded while the phase is not continuous (`step5ExecuteUnexamined`, d3d12_hook.cpp; fail closed at the
  Execute boundary, independent of any diagnostic callback). The producer records a re-execution (quarantine, seq_cst) and an unexamined Execute
  BEFORE the native Execute. Review 3: the quarantine store may bypass `meta` (try_lock failed), so `try_acquire_latest` acquires by compare-exchange
  PUBLISHED -> HELD (a quarantine stored after the candidate selection wins: nothing is acquired), `release` by compare-exchange from HELD, and a
  deferred publication never overwrites a quarantine. The FIRST composite of a frame (`transact`/`compose(fresh)`) uses the same proof: checked
  before any GPU work (`fresh_rewritten_pre`: released unused, `no_new_frame()`) and after its GPU completion (`fresh_rewritten_post`: layer not
  submitted, `have_last_` false, never retained). This proves only which bytes were read; resource/queue ordering of a concurrent GPU write remains a
  separate requirement. `no_new_frame()` checks it before a re-composite (else `dc_release("rewritten")`, no composite) AND again after
  that composite's GPU completion was observed: a write submitted after the first check and ordered before our reads would have been recorded before
  the second check, so "unchanged" then proves the composite read the published bytes (no check-then-write race). A composite that fails the second
  check is not submitted (`rewritten-dropped`), nothing re-submits it (`have_last_` false) and the hold is released (`rewritten`). A producer that does
  not implement the check gets no re-composition (default false). Several slots may be held at once (independent per-slot state). With decouple the slot
  of the newest composited frame stays HELD (owned by `dc_`, no longer by the per-frame ledger) after its completion, and is released exactly once
  (`dc_release`): when a newer frame is admitted (`superseded`, before that frame's GPU work), when it is older than `dcage` (`stall`), on a LOCAL
  space-generation change (`space`: P0's generation is saved for EVERY held frame, posed or not; an unposed frame's P0 is the located views of its first
  composite), on a lock/stage epoch change of the camera it was admitted with (`camera`, CAM-EPOCH admission only), when its images are no longer
  provably the published ones (`rewritten`), or at `retire` (after the existing completion proof). Every composite still observes its own GPU
  completion before the XR frame ends, so a held slot never has consumer GPU work in flight when it is released.
* **The producer never starves**: with one slot held by the consumer the producer has the two others (one newest PUBLISHED, one PRODUCING; the older
  published one is superseded and reusable). The held slot is released at the admission of the newer frame, before that frame's composite, so the
  consumer never holds two slots across a GPU submission. Fake-producer run: 0 skipped over 90 XR frames at 60/90. [E offline; I live]
* **Re-composite = a new swapchain image per XR frame without a new game frame.** `no_new_frame()` sets up the ledger with the held frame
  (`l_.holding = false`) and runs the same `compose()` as a new frame with `fresh = false`: only the two eye images are acquired, waited, recorded,
  executed, completion-observed and released. The producer fence needs no wait (its value completed before the first composite: CPU-observed or the queue
  Wait our completed blit sat behind).
* **Poses.** The images were rendered at P0 = the producer's pose (`StereoFrame::pose`, the previous `composite_views` rule incl. the cockpit/lens FOV).
  Every composite (the first one too) uses P1 = THIS XR frame's located views (`FrameInfo::views`, located at its predicted display time) with P0's FOV
  (so the swapchain pixel mapping and the lens are unchanged); the layer is submitted with P1. "Applied exactly once" holds: the model cockpit, terrain,
  props and world plane are drawn from P1 and the same P1 is the layer pose; the objects image is moved from P0 to P1 inside the shader.
* **HUD and marker quads** stay separate LOCAL quad layers (their own world-locked poses, unchanged): on a re-composite nothing is acquired or drawn for
  them; their swapchains keep the last released image (the same game frame) and the plan re-submits the quads (OpenXR uses the most recently released
  image). Their content changes at the game rate as before.
* **Untracked XR frames**: no composite (the frame stays held); `pace()` as before (zero layers). **reuse fallback**: after `dcage` the slot is released
  and `pace()` re-submits the last composite (with the P1 of that composite) until `reuse_max_age_ms` (500 ms), exactly the old behaviour.

## 3. The re-projection (shader; CPU mirrors `reproject_uv`, `reproject_dir`) [E]

Per eye, root constants b2 (15 dwords; the root signature is now 1 + 28 + 20 + 15 = 64 DWORDs, the D3D12 maximum):
* **Objects image (eye source + its depth)**: ROTATION-ONLY. The P1 pixel's anchor-frame ray `d1 = base1 + u ux1 + v vy1` is mapped to the P0 image by
  `p = H d1, uv0 = p.xy / p.z` with `H = M0^-1 Qsrc`, `M0 = [ux0 | vy0 | base0]`. Head translation between P0 and P1 is ignored for these pixels (choice:
  the objects are hundreds of metres to kilometres away; a 10 cm head shift moves a wingman 50 m away by 0.11 deg; the stored depth is not used).
  Sampling is clamped; outside the source image the coverage is 0 (terrain/sky shows, nothing is stretched) and within `dcfade` of the edge it ramps
  (smoothstep). The coverage, the object-band test (model cockpit drop, terrain occlusion, objdom) all use the re-projected sample.
* **Camera delta** (the aircraft rotates between the camera the objects were drawn with and the terrain camera of this composite): `Q = Cobj^T Cterrain`
  with `C = [right | up | -fwd]`. `Qsrc = Q` when the replayed cockpit band is dropped (model cockpit; the replayed cockpit is camera-fixed and must not
  turn), else the identity. The world-window / effects rays use `q d1` (quaternion of Q): the game's screen-space images live in the cockpit frame of
  their own camera.
* **No-op**: P1 == P0 and Q == I (bit-exact) set no flags: the shader then samples the pixel's own uv (`suv = i.uv`, `a *= 1` never runs) = the previous
  path. Forcing the source flag with the identity mapping changed 0 bytes on WARP.
* The cockpit model, terrain, props, clouds, water, sky and canopy glass are rendered fresh from P1 (they never were images).

Terrain camera per composite: the frame's game camera (or E6) feeds a per-game-frame dead reckoning (`pacing::MotionExtrap`, the extrap:1 model, clamp
`dcms`, rotation clamp 6 deg, no extrapolation through reversals/stops/cuts) once per new game frame; every composite of that frame (the first one and the
re-composites) asks it for the camera at this XR frame's display time (horizon = XR predicted display time - the frame's predicted display time). When it
refuses, the first composite keeps the previous camera (interp:1 delta / extrap:1 / frame camera) and re-composites keep the first composite's camera.

## 4. Evidence and inference

| statement | tag |
|---|---|
| A held producer slot is never picked for a new frame; several can be held | [E] code reading (`step5PickSlot_locked`, `Step5Source`) |
| A held slot CAN be rewritten by a re-execution of its owning recording; a re-composite is used only when `images_unchanged` holds before and after its GPU work | [E] code reading + `gpu_copy_step1_offline` `step5-owner-held`, `xr_stereo_decouple_offline` (G), (H); not built or run by the review-2 author |
| Review 3: acquisition never erases a concurrent quarantine or an unexamined Execute after publication; the first composite is checked before and after its GPU work; an Execute outside the continuous phase fails held/published frames closed | [E] code reading + `gpu_copy_step1_offline` `step5-acquire-quarantine-race`, `step5-untracked-before-acquire`, `step5-unexamined-phase`, `xr_stereo_decouple_offline` (K), (L); not built or run by the review-3 author |
| Option off: the consumer path and every image unchanged | [E] ctest 315/315 before the new test; WARP existing checks; no flags = previous shader path |
| Every XR frame submits a layer with its OWN predicted pose; composites = XR frames; fresh = new game frames | [E] fake runtime 90 Hz over a 60 Hz producer (`xr_stereo_decouple_offline` A) |
| The producer ring never starves; every acquired slot released exactly once | [E] fake producer (A, D) |
| Terrain does not stall between game frames: a re-composite moves the terrain eye by 2/3 of a game frame's motion at 60/90 | [E] (B), worst 0.05 u tolerance |
| The re-projected object lands on its P0 direction seen from P1 | [E] WARP: (0.6578,0.5222) vs hand projection (0.6577,0.5208); 0.033 uv off without |
| The camera delta moves objects and the window/effects consistently | [E] WARP vs CPU mirror, 0 bad pixels |
| Edge fade, nothing stretched | [E] WARP: 82 world-only, 10 fading, 228 object columns on the centre row at a 20 deg head turn; 0 bad pixels |
| Terrain + model cockpit at P1 are drawn fresh (byte-identical to the plain P1 composite) | [E] WARP |
| Re-projection GPU cost: +0.001-0.002 ms per frame | [E] RTX 4090 bench (section 6) |
| The per-XR-frame CPU/GPU wait fits the 90 Hz frame | [I] (completion was 1.8-2.1 ms null / ~4 ms headset per composite, now 90x/s instead of 60x/s) |
| It looks smoother in the headset | NOT shown. Needs the headset. |

## 5. Limits and risks

* **Objects and effects still update at the game rate.** They are re-projected (head rotation + camera rotation) but their positions are those of the
  game frame (interp:1: at its t_display). Between game frames the terrain moves on (display-time camera) while objects keep their position: a translation
  slip of v x (t_XR - t_display) (one 11 ms XR period at 60/90: ~12 units = ~1 m at cruise for ground objects, invisible kilometres away; a wingman
  flying alongside is camera-relative and correct). Without interp:1 the slip adds the select age (objects at frame time): use decouple:1 WITH interp:1.
* **Positional parallax of the objects image is ignored** (rotation-only; documented choice). For the replayed cockpit WITHOUT the model cockpit the head
  translation between P0 and P1 (2-15 mm over 11-33 ms) is uncorrected (0.2-1.7 deg at 0.5 m), no worse than today's SteamVR reprojection, which also
  ignores it; with the model cockpit the cockpit is drawn fresh at P1.
* **Edges**: after a fast head turn the objects image does not cover the new view edge (fade band, then terrain/sky only) until the next game frame.
* **Cost / timing [I]**: each XR frame now records, executes and CPU-waits one composite on the game's DIRECT queue (the old new-frame path did this 60x/s
  at 60/90, now 90x/s). The composite's own GPU time is 0.46-0.49 ms with zpre (0.71-0.81 ms without); the wait also covers the game's queued GPU work
  ahead of it (completion-ms). A slow game queue now delays every XR frame, not only those with a new game frame. Watch end-frame-ms-max,
  completion-ms-mean/max and xr-period-ms-mean (section 7).
* **One slot held longer**: the producer has two slots instead of three while a frame is held. With a GPU backlog (a superseded slot whose replay has not
  completed) the producer skips a frame where it previously might have had a free slot. Watch the producer's `skipped`.
* Counters: with decouple:1 `summary-terrain` / `summary-cockpit` count composites (XR frames); `new` / `new-per-s`, world/effects/HUD counters stay per
  game frame. The `xr_png_once` capture is taken on a new game frame only.
* Not handled: the world cache / extend periphery (not used with the terrain) are not rotated by the camera delta; a re-composite does not re-capture the
  held world (`xr_world_hold_last`).

## 6. GPU cost (RTX 4090, `xr_gpu_bench --quick --decouple --frames 40`, 2400x1350 per eye, m02a, 1.16M props, j35jk cockpit) [E]

| pose | q0 | zpre | dc (re-projection on) | zpre+dc |
|---|---|---|---|---|
| level-2500 | 0.732 | 0.477 | 0.734 | 0.479 |
| low-300-forest | 0.730 | 0.464 | 0.732 | 0.464 |
| nose-down-30 | 0.803 | 0.486 | 0.805 | 0.487 |
| horizon-300 | 0.713 | 0.464 | 0.715 | 0.465 |

ms per composite, both eyes, median of 40 interleaved frames; raw output `D:\acz-extract\gpu-bench\decouple\decouple.csv`. The re-projection adds
0.001-0.002 ms. At 90 Hz every XR frame composes: 90 x 0.48 = 43 ms of GPU per second with zpre (4.3% of each 11.1 ms frame), 90 x 0.73-0.80 = 66-72 ms
without (6.6-7.2%); before, the same work ran ~60 times per second at 60/90. Recommendation: use zpre:1 with decouple:1.

## 7. Live test plan (null driver; the driver agent)

Preconditions: `tools/mem-guard.ps1 -Report` start gate (commit <= 85%; XR live runs need idle commit <= ~82%, D-338). A DLL set built from this
branch. Launch acz.exe directly (D-279), Mission 02 with the skip-intro protocol; exclude the first 15 s.
1. **D0 (2 min, baseline):** `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;interp:1'`, cockpit `auto;anim:1;default:j35jk_vrcockpit;zpre:1`.
2. **D1 (3 min):** the same + `;decouple:1`. Fly straight 30 s, then gentle turns and rolls; move the null HMD pose if the driver can (otherwise
   only the camera part is exercised: the null HMD head is static, P1 == P0 except prediction).
3. Lines (xr-stereo.log), tabulated by `python -I tools/live/decouple-log-table.py "D0=<dir>" "D1=<dir>"`:
   * once: `event=pacing-config;...;decouple=1`, `event=decouple-config;...`;
   * every 5 s `event=summary-decouple`: **composites-per-s** (expect = the XR rate = 1000 / xr-period-ms-mean, within 5 %), **new-per-s** (= the
     game's frame rate; compare with `summary-selection new-per-s` of D0: must not drop by more than 5 %), `recomposed` (= composites - fresh),
     `reuse-fallback` (expect ~0 while the game runs), `released=(superseded,stall,space,retire)` (stall 0 unless the game stalls > 250 ms),
     `reproj-source` (~ composites), `reproj-window` (> 0 in turns), `cam-extrap` vs `cam-held` (held = refused dead reckoning: turns, cuts),
     `horizon-ms-mean`, `held-age-ms-max` (< dcage), `cam-delta-deg-max`;
   * `event=summary`: `reused` drops to ~0 in D1 (was ~1/3 of the XR frames at 60/90 in D0); `submitted` = frames;
   * `event=summary-timing`: **end-frame-ms-max**, **completion-ms-mean/max** (per composite; expect within +1 ms of D0), **xr-period-ms-mean/max**
     (no missed frames: mean unchanged vs D0), **frame-cpu-ms-max**, **gpu-blit-ms** (GPU time per composite, expect ~D0);
   * step5-producer.log `summary-producer-cost` and `skipped` (no increase vs D0: the producer is not starved).
4. Pass: composites-per-s within 5 % of the XR rate, reuse-fallback <= 1 % of the frames, new-per-s within 5 % of D0, xr-period-ms-mean within 2 %
   of D0, end-frame-ms-max <= D0 + 2 ms, no new failures. Abort and leave decouple off if xr-period-ms-mean rises or new-per-s falls by > 10 %.
5. Optional: an `xr_png_once` frame in a turn (new game frame, composed at P1) and a game-window screenshot: registration as D0.
   Smoothness itself needs the real headset (head translation + rotation while flying).

## 8. Tests and build [E]

* `xr_stereo_decouple_offline` (2914 checks): the P1 -> P0 mapping (identity within 2e-6; a 6 deg yaw + 5 cm equals the hand projection within 1e-5,
  uncovered side detected), edge fade, camera-delta rotation (C_t d = C_o Q d), window-ray quaternion, singular P0 refused; runs (A) 90 Hz XR over a 60 Hz
  producer, moving head, moving + turning game camera, HUD layer, (B) the terrain dead reckoning per re-composite, (C) P1 == P0 = the decouple:0 jobs and
  layers with no flags, (D) a game stall (re-composed while < dcage, released once, then the old re-submit path, then stale), (E) untracked frames,
  (F) inert without world_in_projection. Review 2: (G) re-execution of the held frame's recording between XR frames (no further composite from it,
  released once as `rewritten`, slot stays quarantined), (H) re-execution while a re-composite is in flight (that composite dropped, nothing
  re-submitted), (I) lock / stage epoch change without a LOCAL change (released as `camera`), (J) unposed held frame + LOCAL recenter (released as
  `space`). `summary-decouple` gains `released=(...,camera:N,rewritten:N)` and `rewritten-dropped=N`.
* `frame_pacing_offline`: decouple/dcage/dcms/dcfade parsing, ranges, describe(), the 72-character live value.
* WARP `[d] DECOUPLE`: re-projected object position, camera delta (objects + window), edge fade vs the CPU mirror, the identity no-op, terrain + model
  cockpit at P1 identical to the plain P1 composite and different from P0 (landmark moved right after a left head turn).
* Build `D:\b-dc` (Release, Ninja, -j2): Release ctest 316/316 (315 + `xr_stereo_decouple_offline`); WARP harness 923 checks.
