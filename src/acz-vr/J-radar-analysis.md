# J-radar-analysis: radar contacts missing from the captured HUD, and a minimal late-tail replay

Read-only analysis (nothing built, run or edited; no jobboard). Tags: **[E]** established from a cited
log/image/code line, **[I]** inference from several [E], **[G]** guess. Evidence:
`research/experiments/step37-live-run1/logs/step5-producer.log` (phase observer: 9351 `event=draw`/`draw-state`
pairs, 149 `frame-phases`, 110 `tail-sig`/`tail-sig-state`, `png-burst-phases`, `continuous-hud`), the step37 PNGs
(`logs/png/*-b1/b3-hud-over-world.png`), the step36 game-window shot
`step36-live-run1/shots/acz-20261026-093422.png`, step32/step34 `frame-phases` (tm columns only; those runs have
no `tail-sig` lines, step36 has no phase-observer lines at all), and code `gpu_copy_step5.h hud_decide`,
`gpu_copy_step5_hooks.inl` (`step5HudDraw`, `hudRegion*`, `hudReplayTarget_locked`).
Correction to the earlier docs: the observer's phase name "after-tonemap" is misleading, see 1.3.

## 0. Method note: the draw dumps are the strongest evidence
`tail-sig` lines only give per-signature min/max index counts. The full per-draw dumps (`event=draw` +
`draw-state`, written for frames 1,2,3,19,... and every frame with a changed phase) give the true order. Frames
used: 1 (mission start, no contacts), 1763, 2533, 2958 (contacts, different counts), 19 and 288 (late quads).

## 1. Draw signatures

### 1.1 The radar group (all inside the observer's hud-region, scene FP16 RT, 2560x1440, VS 534f29ca / PS d34ee153)
Order at the END of the hud-region, identical shape in every frame [E, frames 1, 1763, 2533, 2958]
(ordinals from frame 2533; PSO ids are pointers, only the sequence matters):

| step | draw | state | role |
| --- | --- | --- | --- |
| a | rings/cross/cone: 4x idx4 + 5x idx35 (33310) | stencil ALWAYS/KEEP, blend 5/2 | rings, cross, cone lines [I] (as captured) |
| b | radar background `vtx 4` (6ec60) | ALWAYS/KEEP, blend 5/6, alpha ops ZERO/ONE (alpha unchanged) | translucent black background [I] |
| c | `vtx 4` + `idx 6` (65790/32bf0) | stencil ALWAYS, **pass op REPLACE (3)**, blend 5/2, alpha ops **ONE/ZERO** (alpha = source alpha) | stencil mask quads, they also WRITE ALPHA [E state; role I] |
| d | `vtx 3` (vs 127819e3 / ps 7e94d4fc), vp 2405x1352, **om=0 / rt unknown**, depth LESS **write**, stencil EQUAL pass ZERO | the tone-map/copy shader family (a3050) used mid-radar | [E state] |
| e | `vtx 4` (63ae0) stencil **NOT_EQUAL**, blend 5/2 | | [E state] |
| f | step c again (REPLACE quads), step d again | | re-establishes the mask [I] |
| g | **contact draws, stencil EQUAL pass KEEP, blend 5/6, depth ALWAYS no write**: one `idx N` (PSO 6a410/28220), three `idx 5` (PSO e68c0/d5370), one `vtx 4` (PSO 6a410) | | **the radar contacts** [I, strong] |
| h | `idx 7` (33310), ALWAYS | | closing instrument line |

What scales with contacts [E]: the first `idx N` draw of step g. Frame 1 (mission start): N=24 and NO `idx5`/`vtx4`
contact draws; frame 1763: N=44; 2958: N=29; 2533: N=84; `tail-sig` key `a886e21f4414e8db` (the EQUAL / ALWAYS-blend
indexed draws): `count-min-max` 5/24 (frames <=2057), 5/44 (frame 1763), **5/94** (frame 2621, 2901), 5/34, 5/39 (3181, 3461),
`max-draws-per-frame` 4 (= idx N + 3x idx5), period average 3.4 to 4.0 per frame. `hud` index sum per frame moves 1104..1259 in
`frame-phases` (hud=65..74 draws, later 60..61 at frame 2958+). The colour split of the game radar (blue arrows,
yellow/green/red squares) cannot be tied to the 3 idx5 draws or to the `vtx4`: [G] (colours come from the vertex colours / constants, not the PSO).
Counts of the `idx5` and `vtx 4` draws are constant (3 and 1), so they are not per-contact [E].
The EQUAL pass-KEEP signature (`stencil=1,front:equal,back:equal,front-pass:1`, blend src5/dst6, depth ALWAYS w0) is the process-independent
key; the only other EQUAL draws are the two step-d triangles (vs 127819e3, om=0, pass ZERO) and `bc79704672aa941f`
(blend 5/2 additive, idx 4, first frame 2313, 1 per frame, EQUAL: an additive blip/flash, [G]).

### 1.2 Phase
All of step a..h are phase **hud-region** to the observer [E]. The observer's region ends at the first PROVEN RT change and does not
treat the unresolved-RT step-d triangle as one (J-phase-observe-findings). The PRODUCER's region ended earlier than the observer's (2.1).

### 1.3 The "after-tonemap" quads are not the radar and are not after the tone map
Frames 19 and 288 [E, ord numbers from the dumps]: after the hud-region come `16x16` offscreen (405d0) and `1x1` (aa7d0, vs a16573be)
passes (the exposure / bloom downsample), then `scene 10 2560x1440` draws, **all the same PSO** (22030 / 116c0; pso-key
`a1eb9e4c311bfc66`; VS 534f29ca PS d34ee153; blend 5/6; depth ALWAYS w0; stencil ALWAYS/KEEP, so hud_decide-clean), 38..103 unindexed `vtx 4`
quads + 3 indexed (14, 9, 9), and THEN the final `r10-output 24 2405x1352` pass (ord 228 in frame 19). `frame-phases tm` counts:
3 (quiet: 16x16 + 1x1 + output) vs 44/86/65/109/59 (extra quads = 38, 80, 59, 103, 53; `vertex-sum` = 9 + 4*extra [E]). They
step in plateaus of 100-1000+ frames and repeat the same values in step32/34/37 (59, 65, 86, 109), which looks scripted, not
contact-dependent [I]. **All three step37 burst frames (845, 1442, 2373) and all four PNG sets sit in tm=3 plateaus [E]**, so the
captured images contain none of them. Content [G]: radio subtitles / message text or flashing HUD sprites. Not tied to the radar.

### 1.4 Correct the earlier hypothesis
J-hud-split-analysis section 2 H-A said the blips are the stencil-EQUAL `03510`/`ad6c0` draws "after `a3050`". Confirmed for the identity (EQUAL, varying
counts, 3x I5), but the order is different: they are inside the hud-region, right after the second a3050-shaped triangle, not after the tone map.

## 2. Why the replay does not capture them

### 2.1 Evidence
1. [E] The HUD counters never refuse: `continuous-hud` last line `frames=valid:3518,invalid:1;draws=202757;avg-draws=57.6;refused=0(...);end=rt-change:3519,scene-barrier:0,close:0;last-refusal=none`.
2. [E] The observer shows 6..13 draws per frame in the hud-region with stencil EQUAL / NOT_EQUAL on the scene RT (frame 1: 180 and 183; frame 1763: 198, 201-205). `hud_decide` would
   return `stencil-test-not-always` for every one of them (gpu_copy_step5.h:349) and increment `refused`. Since `refused=0`, **they never reached `hud_decide`**. This rules out the stencil-EQUAL refusal as the actual cause today.
3. [E] Replayed draws per frame: 58 (`png-burst ... hud-draws=58`; avg-draws 57.6) against 65-74 observer hud-region draws (frame 1: 65, 845: 68, 1763: 71, 2533: 74). The shortfall equals the length of the
   tail after step c: 7 (frame 1: ords 178-184), 10 (845), 12-13 (1763). In frame 1, 120..177 is 58 draws, so the replay ends at the first REPLACE quad pair / just before step d [I, +-1 draw].
4. [E] `end=rt-change` in 100% of frames and the producer ends on the OMSet hook (`hudReplayTarget_locked`) or on a draw whose RT 0 is resolved and not the scene (`step5HudDraw` line 1342). The step-d triangle
   (a3050 shader, vp 2405x1352) is the first draw of the radar group whose render target is not the scene (the observer calls it `rt-class=unknown`; an offscreen/R10 target that the producer can PROVE). **[I, strong]: the replay's
   region latch fires at step d**, `hudRegionEnded()` then returns for everything after it, and the latch is only cleared by a LATER cockpit member (`hudRegionReopen`, line 1308), never by a return to the scene target.
5. [E] Captured image (`b1-hud-over-world.png`, radar crop): rings, white cross and cone lines, the hollow triangle at upper left, a fully opaque black square, no contact. Consistent with replaying steps a..c only. The game's own radar
   (step36 shot) has the translucent background, ~12 contacts (blue/green arrows, yellow and green squares), edge chevrons (red, yellow) and the hollow triangle at the same place.
6. [E] Opaque black: step c draws write alpha = source alpha (alpha ops ONE/ZERO, `draw-state` blend `src-a:2,dst-a:1`), colour is additive black (adds nothing). Step b (the real translucent black) keeps alpha
   (ZERO/ONE) and in a zero-cleared target therefore adds nothing visible. The HUD alpha over the radar square thus comes from the mask quads alone [I]; the consumer composites black with alpha ~1.

### 2.2 What it is not
* Not the cap (`cap-frames=0`; hud_draw_cap 160 vs ~58). Not the effects layer (the between-members epochs). Not a depth/alpha-rule loss (the draws are never replayed).
* The a3050-shaped triangle and the NOT_EQUAL quad would be refused by hud_decide anyway (om_count!=1/unresolved RT; stencil), so even a region that continued would need rule changes (section 3).
* Open: why `refused` stays 0 for the 58-vs-65 gap is a code-path inference, not observed (U1).

## 3. Minimal late-tail replay ("radar group replay")

### 3.1 Principle
Replay is inline in the game's own command list, so **the stencil reference, blend factor, root arguments, viewport and scissor are the game's, whatever the proxy tracks**: only OMSetRenderTargets is replaced
(hud private RTV + DSV) and restored (the existing 3-command pattern). The untracked stencil ref therefore does not need tracking; what must be proven is the stencil CONTENT of the private DS. Stencil content
comes from REPLACE quads which are plain ALWAYS draws that the replay already runs (the private DS is cleared to stencil 0 once per frame and receives the same REPLACE writes with the game's ref). So the EQUAL contact draws would
mask exactly like the game's if (a) all stencil writers between the last REPLACE and the contact draw are replayed or irrelevant and (b) the ref is the game's [inferred from D3D12 semantics: ref persists in the list across OMSetRenderTargets: G until checked].

### 3.2 Smallest change (default off, new key `gpu_copy_step5_hud_radar`, requires `gpu_copy_step5_hud`)
1. **Region becomes segmented, not latched.** Keep `ended` for barrier/close/member reasons; make a draw under a proven other RT an *excursion* (`away=true`), not the end. Draws while `away` are not replayed. The first draw that is again on the scene RT (resolved, same
   resource) ends the excursion and replay continues (flag `late=true`; count excursions per frame, cap 8). Barrier (`scene-barrier`) and a later member keep their current meaning. This alone also reaches step-1.3 quads (they are after 16x16/1x1 excursions), which is
   desirable only if they are HUD (U4); gate them by a counter first (section 4).
2. **`hud_decide` stays fail-closed with two relaxations only for `late` / radar-group draws**:
   * stencil EQUAL (front and back) with KEEP pass (and NOT_EQUAL only if proven, see below) is accepted when the frame's replayed stencil writers since region start include >=1 REPLACE draw whose pass op is REPLACE and whose write mask covers the draw's read mask, and **no stencil writer between that REPLACE and this draw was skipped** (a skipped writer = counter `radar-stencil-gap`, refuse `stencil-writer-skipped`);
   * the depth write of the a3050 triangle is never replayed (it is a skipped excursion draw, see 3). Draws with depth write remain refused.
3. **What is skipped, not refused** (so a skipped excursion draw does not invalidate the frame): any draw while `away`, i.e. both a3050-shaped triangles (om=0/other RT). Their stencil effect (ZERO where the mask equals the ref) is overwritten by the second REPLACE quad pair before the contact draws, in all four dumped frames [E]; their depth write is irrelevant to the ALWAYS contact draws [E].
4. **NOT_EQUAL quad (step e)**: leave REFUSED for the radar-group relaxation (stay `stencil-test-not-always`), because the game applies it after a ZERO pass whose ref we cannot see; but a refusal must not kill the frame: introduce a per-class verdict `skip-with-count` for exactly the step-e signature (ALWAYS-blend 5/2, vtx 4, NOT_EQUAL, om=1, viewport = scene) so `hud_valid` stays and `radar-notequal-skipped` is counted. [G: this quad is the radar bezel/dim overlay; if the capture then shows a missing element, fall back to refusing the whole radar group.]
5. Draw cap: current `hud_draw_cap=160`, `hud_command_cap=600` are too small if the late quads join (58 + 12 + up to 103); raise to 256 / 960 (+~29 KB of command memory is irrelevant; the budget is the 3 commands per draw), and keep the cap a per-frame refusal.

### 3.3 Alternative considered: replay the whole radar group together
Replay steps a..h as one unit and refuse the unit if any member is unprovable. Same mechanics as 3.2 but the validity is group-level (`radar_valid`): easier to prove, but a skipped step-d/e would invalidate the whole group every frame (they are always present), so it would never publish. Rejected unless 3.2's skips are
shown wrong. Without the mask at all (blips drawn with the stencil test removed): impossible without a PSO clone (the PSO's stencil state is baked); a clone is an option for step 5 below, but note blips are drawn by the game inside the mask so the unmasked draw would only differ
for contacts clipped at the square edge, a small visual loss compared to the cost.

### 3.4 Render target, depth-stencil, memory
No new target: same private FP16 `hudRt` and `hudDs` (the stencil written by REPLACE quads already lives there; clear = once per frame, unchanged). Only more draws on the same target. Memory delta ~0 (command list recording + 3 commands/draw, +<=60 KB at the new caps);
the existing HUD commit (+188 MB measured in run 8) is unchanged. One consumer-visible risk: the HUD image gets the late quads (1.3) too if step 1 is enabled for all excursions; add a `late-class` filter (replay only EQUAL-stencil group + draws sharing the HUD PSO family, count the rest) for the first live run.

### 3.5 HUD alpha for the opaque black radar background
Cause [I]: REPLACE quads write alpha (ONE/ZERO). The fixes, in increasing cost:
1. Consumer-only: none can recover the translucency (the colour is black, alpha 1; the game's real alpha lives only in step b which does not write alpha).
2. **PSO clone for the REPLACE quads (step c) with `RenderTargetWriteMask = R|G|B` (alpha masked)**: stencil and colour behaviour identical, no alpha. Then the black background comes from step b with ZERO/ONE alpha, i.e. colour 0 and alpha 0: invisible, not translucent. So also give step b a clone with alpha ops `ONE/INV_SRC_ALPHA` (premultiplied-friendly: alpha = srcA + dstA*(1-srcA), colour needs premultiplication the PS does not do: colour (0) is black so it is correct for black). That makes the radar translucent black in the HUD layer, equal to the game. Needs: store the creation desc of exactly the HUD shader family PSOs (VS 534f29ca + PS d34ee153), create clones lazily at first use, replace the PSO for the replayed draw (SetPipelineState before the replay Draw, restore afterwards: 2 extra commands), refuse on clone failure. This is a separate stage (stage 5) and is also the general fix for J-hud-colour's alpha issue.
3. Until then: accept the opaque black square (known issue), the blips draw on top of it so they are visible.

## 4. Ordered implementation plan with tests

Stage 0 (diagnostics, no behaviour change; settles U1 and U2 first):
* In `continuous-hud` and a new `hud-end;frame=...` line (first 3 frames and every 5 s): the replayed draw count, the ordinal, PSO route, VS/PS hash prefix and viewport of the region-ending event, its `why`, the RT resource/size/format that triggered it and whether the draw/OM was the cause; plus, read-only, run `hud_decide` on every post-end scene-RT draw of the list and count verdicts by `why` (`late-would-refuse`, `late-would-accept`, by category) with the ordinal of the first would-refuse. Add stencil-ref capture by wrapping `OMSetStencilRef` (state + a counter of changes), logged per draw in the observer's draw-state (replace `ref:untracked`).
* Offline: fixture with a synthetic frame (scene draws, an `OMSet(offscreen)`, a draw, `OMSet(scene)`, EQUAL draw): exact counters; key off byte-identical.
Stage 1: tail segmentation (excursion/return) behind the new key, no relaxation yet. Tests: `step5-hud-region-excursion` (return to scene resumes, barrier ends, later member reopens, excursion cap, region without return unchanged), key off identical command stream.
Stage 2: `hud_decide` EQUAL relaxation with the stencil-writer proof + skip classes; fixtures: writer skipped -> refuse; REPLACE then EQUAL accepted; NOT_EQUAL class skipped with counter; depth write still refused; command-stream golden (OMSet/Draw/restore only, no extra commands); mutation tests (accept EQUAL without writer must fail).
Stage 3: caps 256/960, counters in `continuous-hud` (`late-draws`, `late-refused`, `radar-stencil-gap`, `radar-notequal-skipped`, excursions).
Stage 4: live check, null HMD, one enemy-rich mission: `png_once` (hud-on-black) must show contact glyphs inside the radar square and nothing outside; `refused=0`, `late-draws` ~12/frame in the dumped frames; compare idx N with the game window screenshot contact count.
Stage 5: PSO clone alpha fix (3.5); separate review (it adds creation-desc retention).

## 5. Unknowns and the smallest live observation

* U1 [unknown]: exactly which event ends the producer's region at step d (OMSet hook vs the Draw-seam `draw-on-other-target`) and why `refused` stays 0 although EQUAL draws exist before the observer's region end. Cheapest observation: the Stage-0 `hud-end` line (one run, any cockpit frames; no enemies needed).
* U2 [guess]: that D3D12 keeps the stencil reference across the replay's OMSetRenderTargets (spec says ref is separate list state; proxy never sets it). Observation: Stage-0 stencil-ref wrapper (does the ref at the contact draw equal the ref at the last REPLACE quad? is it non-zero?) in the same run.
* U3 [inferred]: that the idx-N draw tracks contacts. Observation: one enemy-rich frame dump (or `phase-observe.mark` right before an enemy wave) against the game screenshot taken at the same moment; compare N/frame with contact count (expect 3 per arrow if triangle list, 6 per square).
* U4 [guess]: what the after-bloom quads (1.3) are. Observation: a `png-burst` during a tm>3 plateau (e.g. start of the mission, frames 19-177 in step37, or at 288-560): eye the HUD-over-world image; or log the draw ordinals' `vertex positions`. Do not enable replay of them until known.
* U5 [guess]: what the a3050-shaped triangle (om=0 in the observer) really renders to (rt/DSV) and whether the radar background in the game is composed from it (a copy into the radar). If so the game radar is partly an RT-to-RT effect that a stencil-only replay cannot reproduce. Observation: Stage-0 log of the triangle's RT resource, size and format, and its t0 SRV resource, once per process.
* U6 [guess]: the colour of the NOT_EQUAL quad and whether skipping it is visible. Same run: `png_once` with and without it (counter `radar-notequal-skipped`).
