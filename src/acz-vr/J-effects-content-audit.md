# J-effects-content: what the replayed effects draws put into the effects target (audit + producer PNG capture)

Basis: D-273/D-274, J-effects-layer-findings, J-test8-fixes-findings, J-warp-composite-harness-findings, run I (step34 producer-only), phase-observe data of
step32-live-run1. Tags: [E] established by code/fixtures/live logs, [I] inference, [G] unverified live. Nothing here ran against the game.

## 1. Tooling (default OFF, same key `gpu_copy_step5_png_once`)
With `gpu_copy_step5_png_once` AND `gpu_copy_step5_effects` (both need continuous + hud) the one-shot readback gets a 4th footprint, the captured slot's effects target
(readback 4 x 29.5 MB at 2560x1440 instead of 3 x). Effects off or not ready: byte-identical to before (3 images, 2 samples, no new line).
* Base samples (2, as before): the effects image is included when the frame cleared the effects target (a frame with no candidate would hold stale content: then the sample has 3 images
  and 5 files). Outputs `acz-vr-png-<pid>-<t>-s<N>-<name>.png`: the five old ones plus `effects` (effects over the world quad: the consumer's mode-3 maths, alpha = max(a, max rgb)),
  `effects-on-black` (premultiplied rgb), `effects-alpha-raw` (stored alpha, linear grey), `effects-nonzero` (white = any channel non-zero, so faint content is visible).
* Combat trigger: after the base samples, up to 3 more sets (`...-b1-`, `-b2-`, `-b3-`; effects outputs only, 4 files) at the FIRST frame whose COMMITTED (between-members)
  effects draws >= threshold, >= 30 published frames apart. Threshold: constant 6 or the file `%LOCALAPPDATA%\acz-vr\png-effects.threshold` (decimal 1..96, read once at ring creation;
  invalid -> 6, logged `source=file-invalid`). Run I: quiet 3.0 committed draws/frame, gun +1, missile up to 10; with the alpha-only fix below quiet is about 2.
* Log lines (step5-producer.log): `png-effects-config` (threshold, source), `png-stats ... image=effects`, `png-effects;sample=;kind=base|burst;committed-draws=;threshold=;texels=;
  nonzero=N(frac);alpha-gt0=;rgb-gt0=;alpha-max=;alpha-mean=;rgb-max=;nonfinite=;bbox=x0,y0,x1,y1`, `png-once;outcome=read;...;kind=;effects=draws:,nonzero:,alpha-max:,alpha-mean:,bbox:`,
  `continuous-png` status gains `effects-bursts=n/3;threshold=;below-threshold-frames=;mode=`, `outcome=done;samples=2;effects-bursts=n`. The effects status line gains `hud-ds-writers=`.
* Burst waiting costs one lock + a few compares per list Close for the rest of the process (diagnostic runs only).

## 2. Audit: ways committed draws could yield no pixels
1. Predicate semantics [E code, E fake GPU, I live]. NOT_EQUAL_ZERO on a 64-bit word, written 1 by the CPU at the NEXT recorded cockpit member, before Close/Submit; the buffer is
   UPLOAD/GENERIC_READ (includes the PREDICATION state), persistently mapped, offset = (slot*32 + epoch)*8 (8-aligned), vtable index 55 = SetPredication [verified against the
   ID3D12GraphicsCommandList order]. Indexed by SLOT, not frame: safe because a slot is only reused after its fence completed (J-3b9 rule) and the slot's words are zeroed at frame arm.
   The run before the FIRST member is not a candidate phase at all (ignored: it is inside the world snapshot). The run after the LAST member (tail) stays 0 by design.
   A run followed by a recorded member always commits (`commit()` returns its index when it had draws). Refused (non-recorded) members do not commit: those draws then merge into
   the next epoch (harmless). Live counters agree (valid 7616/7616 = one committed epoch per frame). No bug found. [G] the real GPU evaluating a persistently mapped WC upload
   predicate has never been observed; a debug-layer run would show it.
2. Clear order [E]. effects RT cleared once at the first candidate (never predicated), the shared DS cleared by whichever consumer comes first; nothing else clears `effRtv`
   (grep: only `Step5EffCall.rtv`). The HUD reset-on-later-member clears the HUD target, not the effects target. No second clear.
3. Viewport/scissor [E]: not touched; the replay runs under the game's own state; target size/format equal the scene RT (checked by hud_decide). OM is restored from the bind-time ring.
4. Depth [E data]: the shared DS is cleared to 0.0; GREATER/GREATER_EQUAL pass for any fragment depth > 0 (>= 0). Phase data (step32, between-members, scene RT): 7 state rows,
   ALL `w0` (no depth write), stencil always. So the feared hazard (HUD replay of a depth-writing between-members draw altering the shared DS that the effects draws then test
   against) does not occur in the recorded data; `hud-ds-writers` now counts it live. [G] depth exactly 0 fragments fail GREATER (not seen).
5. Blend [E data]. Logged tuple is `blend=<enable>,src:,dst:,op:,src-a:,dst-a:,op-a:` (D3D12_BLEND: 1 ZERO, 2 ONE, 3 SRC_COLOR, 4 INV_SRC_COLOR, 5 SRC_ALPHA, 6 INV_SRC_ALPHA, 14 BLEND_FACTOR,
   15 INV_BLEND_FACTOR, 16..19 SRC1_COLOR/INV/SRC1_ALPHA/INV, 20/21 ALPHA_FACTOR/INV; op 1 ADD, 3 REV_SUBTRACT). So D-273's "1,5,6" = enabled, SRC_ALPHA/INV_SRC_ALPHA, and the live scene
   PSOs are: (a) blend 5/6 with ALPHA factors 18/19 (SRC1_ALPHA/INV_SRC1_ALPHA, i.e. dual-source: the PS writes SV_Target1), depth GREATER_EQUAL or GREATER, two PS families
   (534f29ca/4caf6e2d sprites; 9eff1f92/d2464ed2 non-indexed); (b) additive-like 5/2 (alpha 1/2) GREATER; (c) the alpha-only pass (below). Onto a (0,0,0,0) target: rgb = src.rgb*a
   (premultiplied, correct); alpha = a0*a1 (dual-source) or other: it is NOT coverage in general. The consumer rule alpha = max(a, max rgb) keeps coloured content visible but
   under-occludes dim smoke. Additive (5/2) composited as over darkens the world slightly instead of adding. No constant-factor draws (const-blend=0 live). Not a "no pixels" cause.
6. FP16 target [E]: the PSOs' RTV format equals the scene format (rt-format-mismatch would have refused); FP16 holds premultiplied values without clamp.
7. **Likely bug, FIXED: the alpha-only pass** [E data]. Every between-members phase starts with a 4-vertex non-indexed full-screen draw, depth ALWAYS (enabled, no write), blend OFF,
   RenderTargetWriteMask 0x8 (alpha only), PS d34ee153. `effects_decide` only ignored draws with blend off AND depth disabled, so it was a candidate and was replayed (committed
   every frame: run I `ignored=0` always) writing the PS alpha across the WHOLE effects target. With the consumer composite `e + (1 - e.a) * world` a non-zero alpha there dims/blanks the
   world window and tells nothing about real effects (a pure alpha write can never show colour in the game's picture either). It is also at least one of the 3.0 quiet "effects" draws/frame.
   Fix: `effects_decide` ignores draws whose write mask has no R/G/B bit ("no-colour-write"), and "depth-tested" now means a real GREATER/GREATER_EQUAL test (an opaque ALWAYS/NEVER
   draw is an always-pass draw: ignored "not-effect-like"; blended ALWAYS stays a candidate). Tests: rules table, `step5-effects-alpha-only` (not replayed, not predicated, counted
   `ignored`, no clear), existing scenarios unchanged. Whether this explains the empty headset is unknown (if the PS wrote alpha 0 it was harmless): the PNGs decide.
8. Remaining unknown: the quiet-frame committed draws that remain (blend 5/6, GREATER_EQUAL, 89 indices, sprite PS 4caf6e2d) are plausibly cockpit-glass/lens decals, i.e. valid
   content the user may take for "nothing". The `effects-nonzero` image and the bbox say where they land. Dual-source alpha (item 5) is the other content risk.
9. Consumer side is proven on WARP (xr_composite_warp); with world hold/extend the effects image is used only for FRESH world composites [E J-effects-layer-findings].

## 3. Producer-only combat run (no XR)
Config (runtime.cfg): `gpu_copy_step5_continuous=true`, `gpu_copy_step5_members_by_class=true`, `gpu_copy_step5_hud=true`, `gpu_copy_step5_effects=true`, `gpu_copy_step5_png_once=true`
(+ the usual selection/logging keys; optionally `gpu_copy_step5_phase_observe=true`). Optional file `%LOCALAPPDATA%\acz-vr\png-effects.threshold` containing e.g. `5`.
Memory: readback 118 MB + effects 88.7 MB over the HUD run; check commit first. Fire guns/missiles after the two base samples (a few seconds in the cockpit).
Read: `step5-producer.log` lines `png-effects-config`, `png-effects;sample=1..5`, status `continuous-png ... effects-bursts=n/3`; PNGs in `%LOCALAPPDATA%\acz-vr\logs\png` (the existing output dir) named
`...-s1/s2-*` and `...-b1..b3-effects*.png`. Interpretation: `effects-nonzero` empty + committed-draws >= 6 = the replay produced nothing (predicate/depth/PS); non-empty with
`effects-alpha-raw` bright over the whole window = alpha contamination; content only at tracer/smoke positions = producer fine, look at the consumer.

## 4. DECISIONS entry text (for the coordinator)
D-275 - Effects target content audit and producer PNG capture (J-effects-content). `gpu_copy_step5_png_once` now also reads back the effects target (4th image, outputs effects /
effects-on-black / effects-alpha-raw / effects-nonzero, coverage log line) and, after the two base samples, takes up to 3 burst sets when committed effects draws per frame reach a
threshold (6, or %LOCALAPPDATA%\acz-vr\png-effects.threshold). Audit: predicate/clear/viewport/depth logic read as correct; phase data show no depth writers between members. Found and fixed
a probable content bug: the full-screen alpha-only pass (mask 0x8, ALWAYS, blend off) was replayed into the effects target every frame; `effects_decide` now ignores no-colour-write draws
and treats only GREATER/GREATER_EQUAL as depth-tested. Logged blend tuple is enable,src,dst,op,src-a,dst-a,op-a; live effect PSOs use dual-source alpha factors (18/19). Tests: 260/260 Release ctest.
Not verified live; the next producer-only combat run reads the PNGs.

## 5. Burst capture protocol (v2; default OFF, same key `gpu_copy_step5_png_once`)
Why: run I/step36 live evidence: during gun/missile bursts the game window shows a tracer and a long missile smoke trail, but the effects-target PNGs of the b1..b3 sets
contain only HUD elements (AWACS message, FRND boxes, a green wedge). So the extra between-members draws are HUD overlay, not tracers/smoke. Open question: are tracers/smoke
already inside the WORLD snapshot (drawn before the first cockpit member, phase `pre`) or drawn elsewhere (after the last member / other list / other target)? v2 captures the evidence.
* Every burst set (<= 3 per process, shared by both triggers; labels `-b1-`..`-b3-`) now writes 7 files: `world`, `eye0`, `hud-over-world` (of the SAME frame, same readback as s1/s2; no extra
  memory: the 4-footprint readback already held all images) plus the 4 effects outputs. ~15 MB each stored, ~105 MB per set, ~315 MB for three. Base samples are unchanged (5 or 9 files).
* Trigger 1 (unchanged): first frame with committed effects draws >= threshold (`png-effects.threshold`, default 6, >= 30 published frames apart). Write `96` into the threshold file
  to make this trigger practically unreachable, so the three sets are spent on file requests only.
* Trigger 2 (new): the FILE `%LOCALAPPDATA%\acz-vr\png-burst.now`. The worker polls its stamp (creation + last-write time) every 200 ms. The first poll only records the stamp: a file left
  over from an earlier session is NOT a request (`png-burst-config ... file-present-at-first-poll=1` says so). Afterwards creating or REWRITING the file (the content may be anything;
  the last-write time must change) is one request; deletion is not. A request takes the NEXT valid cockpit frame (world/HUD valid, not pose-broken), without the 30-frame gap
  and without the threshold; it is honoured only once the two base samples are read (else it waits: `status=pending-until-the-base-samples-are-read`) and only while burst sets remain
  (`status=ignored-all-burst-sets-taken`). A set recorded into a list that was then Reset is not lost (the request becomes pending again). Needs `png_once` AND `effects` (fxCapable).
  Typical latency: file write -> <= 200 ms poll -> next frame (tens of ms) -> worker readback/PNG within a few seconds. `request-to-capture-ms` logs the measured latency.
* Log lines (step5-producer.log), per set: `png-burst;sample=;set=n/3;reason=file|threshold;file-requests=;request-to-capture-ms=;published-frame=;slot=;members=;hud-draws=;
  committed-draws=;effects-image=0|1;world-nonblack=N(frac);world-max=;world-mean-rgb=;eye0-nonblack=;hud-nonblack=;effects-nonzero=;files=7/7` (non-black = any of r,g,b > 0 over ALL texels);
  `png-stats` for all four images of a burst set (before only effects); `png-burst-request;request=;tick=;stage=;mode=;bursts-read=;status=` (worker, per file change);
  `png-burst-config` (once, at the first poll). With `gpu_copy_step5_phase_observe=true` also `png-burst-phases;sample=;observer-frame=;draws-per-phase=pre:,mem:,btw:,hud:,end:,tm:;indexed=;on-scene-rt=;
  new-pso=;first-member-ord=;last-member-ord=;region-end-ord=;tonemap-ord=;total=;sig=` and `png-burst-phase-shapes` (index/vertex sums and the draw-shape sum per phase) for the SAME frame
  (copied from the observer at the list's Close; without the observer key: `observer=none(...)`). Phase meaning: `pre` = non-member draws before the first cockpit member = INSIDE the world
  snapshot; `btw` = between the first member and the HUD region (includes the effects/HUD-overlay draws); `hud` = HUD region; `end`/`tm` = after the region / after tonemap (tail).
  Status line gains `file-requests=;file-bursts=;file-ignored=`.
* How to read the answer: (1) on a gun/missile frame compare `world.png` (and `hud-over-world.png`) with the game window screenshot: tracer/smoke present in `world.png` => already in the world
  snapshot (drawn before the first member); (2) `world-nonblack`/`png-stats image=world` between a quiet reference set and a firing set, and `png-burst-phases` draws `pre` quiet vs firing
  (extra `pre` draws while firing = the tracer/smoke draws are before the first member); extra `end`/`tm` draws = drawn after the last member (a tail the layers never replay);
  (3) tracer absent from `world.png` and `pre` unchanged => drawn in another list/target: look at `phase-observe` windows (armed `phase-observe.arm`) for draws on other targets.
* Offline tests: `step5-png-burst-file` (file trigger x3 without gap/threshold, 7 files per set, request/phase lines, 4th request ignored, done), `step5-png-burst-file-stale` (stale file ignored,
  threshold + file mix), updated `step5-png-effects` (7 files per threshold set, `png-burst` lines). Key OFF byte-identical (png-off, phase-observe digest tests unchanged). 262/262 Release ctest.

### Burst protocol v3: `scene-end.png` (the scene target at the START of the HUD region; file-triggered sets only)
Why: run K (step37-live-run1): during a missile launch neither the effects target nor `world.png` (snapshot at the first cockpit member) shows the smoke trail the game window shows,
although the burst frame has +16 `pre` draws. Question: is the smoke in the game's SCENE render target once ALL scene draws (cockpit and between-members effects) are done?
* What: when a `png-burst.now` request is pending and the machinery is armed (burst mode), the OM hook (after the native call) or the barrier hook (BEFORE the native call) that ENDS the
  frame's HUD region (`rt-change` = first render-target change away from the scene target, `scene-barrier` = the scene target leaves RENDER_TARGET) records, inside the game's
  command list, Barrier(scene RT->COPY_SOURCE), CopyTextureRegion(scene RT -> readback footprint 4), Barrier(back). No private image, no per-slot memory: the readback buffer of an
  effects-capable png_once process grows from 4 to 5 footprints (118 -> 147 MB at 2560x1440; allocated at creation only with `gpu_copy_step5_png_once` + effects, unchanged otherwise).
  Cost: three commands per candidate frame while a request is pending (at most 3 per frame if the region reopens), nothing otherwise (one relaxed atomic load per OM/barrier call).
* Which sets: FILE-triggered burst sets only (threshold-triggered sets cannot know at the region end that they will be taken, and a speculative 29 MB copy every frame is not free).
  A request that arrives after the frame's HUD region ended waits for the next frame (at most 2 declined frames, `declined=` in the log); if no hooked call ever ends the region
  (`draw-on-other-target` ends, or none), the set is served WITHOUT the image (`scene-end=0`, 7 files).
* Files: a file-triggered set now writes up to 8 files: `world`, `scene-end`, `eye0`, `hud-over-world` + the 4 effects outputs (`...-bN-scene-end.png` = sRGB(shade(scene), clamp, exposure 1),
  alpha 255). The cockpit IS visible in it (it is drawn before the region start); the goal is effects pixels.
* Log: `png-scene-end;sample=;region-end=rt-change|scene-barrier;copies=;declined=;scene-end-nonblack=N(frac);scene-end-max=;scene-end-mean-rgb=;world-nonblack=N(frac);
  changed-vs-world=N(frac);changed-bbox=x0,y0,x1,y1;eps=0.004` (changed = any shaded channel differs by more than 0.004 between scene-end and world, over ALL texels, cockpit included)
  and `scene-end=0|1` in the `png-burst` line (`files=8/8`); `png-stats image=scene-end` too.
* How to read it (compare with the game-window screenshot taken at the same moment):
  - smoke/tracer/explosion visible in `scene-end.png` but NOT in `world.png` => it is drawn between the first cockpit member and the HUD region start. The effects-layer classifier
    / replay must capture it (it is not in the world snapshot, and the effects target does not hold it): look at the `btw` phase draws and at why the classifier ignores them.
  - visible in neither => drawn after the HUD region start (a later pass on the scene target, e.g. after a render-target change that came back, which the region logic treats as the
    end) or in ANOTHER target / list (post-process, a separate effects buffer composited at the tone map): see `png-burst-phases` (`end`, `tm` draws) and the phase-observe windows.
  - visible in `world.png` already => drawn before the first member (nothing to fix on the producer side; the consumer must show the world snapshot there).
  - a quiet reference set tells which changed pixels are just the cockpit: use the offline helper below.
* Offline helper: `python -I tools/analysis/png_diff.py world.png scene-end.png --eye0 eye0.png --out diff.png [--threshold 8] [--cell 16] [--json]` (self-test:
  `python -I tools/analysis/png_diff.py --self-test`). It writes a difference image (red = changed outside the cockpit mask, blue = inside, grey = unchanged) and prints the bounding
  boxes of the changed regions OUTSIDE the mask, biggest first. Mask = pixels with eye0 alpha > 0; the producer's eye0.png carries alpha 255 everywhere, so the helper falls back to
  eye0 pixels with a non-zero RGB and says so (`mask:` line); the cockpit pixels of scene-end are normally also the eye0 non-black pixels only approximately (different view), so
  treat boxes touching the cockpit outline with suspicion and read the diff image.
* Offline tests: `step5-png-burst-file` (scene-end ops order/state, file, pixels, log), `step5-png-scene-end-barrier` (copy before the game's own barrier), `step5-png-scene-end-declined`
  (request after the region end waits one frame), `step5-png-scene-end-absent` (served without the image after 2 declined frames), updated `step5-png-effects*` (threshold sets unchanged:
  7 files, no scene-end). Key OFF byte-identical (png-off, phase-observe digest tests unchanged). 272/272 Release ctest.
* Next null run: same executor steps as below, nothing new to configure (`png_once` + `phase_observe` + the file trigger; `png-effects.threshold`=96 keeps all three sets for file requests).
  Rewrite `png-burst.now` while the smoke is visible (set 3/3, missile) and also during a quiet frame (set 1/3) and a gun burst (set 2/3); collect `...-bN-scene-end.png` + the `png-scene-end` line.

### Executor steps for the next null run (producer-only, no XR; the game is never launched by this tool)
1. runtime.cfg: the section-3 keys (`gpu_copy_step5_continuous`, `members_by_class`, `hud`, `effects`, `png_once` = true) plus `gpu_copy_step5_phase_observe=true`.
2. Before launch: write `96` into `%LOCALAPPDATA%\acz-vr\png-effects.threshold`; DELETE `%LOCALAPPDATA%\acz-vr\png-burst.now` if present. Check system commit first (readback 118 MB + effects 88.7 MB).
3. Start the game (library Play + 'Play ACE COMBAT ZERO' dialog; never -applaunch), reach the cockpit, wait for `outcome=read;sample=2` in `step5-producer.log` (2 base samples).
4. Quiet reference: `Set-Content $env:LOCALAPPDATA\acz-vr\png-burst.now (Get-Date)` (any content; rewrite it for each request, at least 100 ms apart). Wait for `event=png-burst;...set=1/3;reason=file`.
5. Fire guns: press and HOLD the gun key, THEN rewrite the file while the tracer is visible (set 2/3). Take a game-window screenshot at the same moment.
6. Fire a missile: press the missile key, rewrite the file ~0.5-1 s later while the smoke trail is visible (set 3/3); screenshot.
7. Collect from `%LOCALAPPDATA%\acz-vr\logs`: `step5-producer.log` (`png-burst*`, `png-stats`, `png-effects`), `png\acz-vr-png-<pid>-<t>-b1..b3-{world,eye0,hud-over-world,effects*}.png`, the screenshots. Compare per 'How to read'.
