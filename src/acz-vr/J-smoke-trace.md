# J-smoke-trace: which draws are the missile smoke, and why the effects target does not hold it (run L, step38-live-run1; offline log/PNG analysis)

Tags: [E] established from logs/PNGs/code, [I] inference, [G] guess. Nothing was run. Sources: step5-producer.log (phase-observe full dumps of frames 4332 quiet, 4373/4374 missile launch,
4592/4664 smoke grown; png-burst-phases b1/b2/b3), png/diff-b3.png, b1/b3-effects-nonzero.png, shots/b3-missile-window.png, gpu_copy_step5.h effects_decide, hooks.inl step5EffectsConsider_locked.

## 1. Which draws are the smoke
* [E] Quiet btw scene draws (frames 4332, 1621): 3 = the 4-vertex alpha-only pass (534f29ca/d34ee153, mask 8) + two indexed sprites 534f29ca/4caf6e2d (89-104 and 14-19 idx).
* [E] Missile launch frame 4373: btw gains ONE draw, ord 116: vs 9eff1f92 / ps d2464ed2, NON-indexed, count=2, new-pso=1, depth GREATER no write, blend enabled 5/6 with alpha factors 18/19
  (dual source), mask f, stencil always. Frame 4374: same draw, count 2 (PSO known). Frames 4592/4664 (smoke grown): SEVEN such draws (counts 16,16,6,16,16,16,10 / 16,6,16,16,16,16,10),
  one PSO per segment group. All 58 occurrences of ps d2464ed2 in the log are phase=between-members, rt-class=scene; none in pre, hud-region or after-tonemap. [E]
  Growth 2 -> 70+ vertices over time matches a trail that lengthens. => the smoke trail = ps d2464ed2 draws, between members. [I, strong: no other scene draw changes between quiet and launch].
* [E] b3 (frame 4401, observer): btw 44 vs 41, btw on-scene 6 vs 3 (+3), btw vertex-sum 139 vs 108 (+31), index-sum 1144 vs 816, committed-draws 5 vs 2 (+3). The +3 non-alpha-only scene draws
  = d2464ed2 segments (vertex +31 fits 2..16-vertex draws; one extra indexed sprite explains indexed 15 vs 14). [I]
* [E] Also new at launch, but in `pre` (inside the world snapshot, depth-writing, blend ONE/ZERO-like): +3 draws vs/ps b569109e/c1b58903 (183/44/44 idx) = missile body / flame mesh; b3 pre 61 vs 56.
  They are in world.png by construction; hud 65 vs 68 and members 26 vs 29 are a 3-draw cockpit-membership change, not smoke. [I]
* [E] tm (after-tonemap) grows 3 -> 36 (b3) / 109 (b2): 33 / 106 scene-RT draws AFTER the first render-target change (+ tone-map). scene-end is taken at that change, so the tm draws are NOT in scene-end.png.
  They are the late tail (534f29ca/d34ee153 text/subtitle quads, tail-sig). Not the smoke (smoke is visible in scene-end). [E]

## 2. Phase, classifier, predicate
* [E] Smoke = between-members: after the first member (ord 62..66), BEFORE the last member (ord 131..133; ords 110-117 vs last 137 in frame 4592). So it is NOT after the last cockpit member and the
  "discarded by construction (tail predicate 0)" hypothesis is REFUTED for the smoke. It is also not in the HUD region (hud draws are all 534f29ca/d34ee153, depth ALWAYS).
* [E] The classifier does not ignore or refuse it: no-colour-write no (mask f), blend on, depth GREATER (real test), no depth write, stencil keep, no constant factor (alpha factors 18/19 only; 14/15/20/21 would refuse).
  Live counters: refused=0 in every status line, const-blend=0; `draws` rises from avg 2.00 (until frame 4331) to 2.38/2.49 = extra committed draws; `ignored` = 1/frame (the alpha-only pass) plus a few extra
  per smoke frame (ignored 5061 vs commits 4945 at the end). So the smoke draws are COMMITTED (candidates replayed, epoch followed by a recorded member, predicate written 1). The "ignored grows" is just the alpha-only pass. [E]
* [E] Yet b3 effects-nonzero has no pixels where diff-b3 (world vs scene-end) shows the trail (display x 870-1040, y 640-800): only HUD graphics, AWACS text and the wedge/bar.
  => the replays were recorded but produced no visible pixels. Cause NOT established.
* [E, important side finding] The effects target holds the HUD TAIL in every set: b1 (quiet) effects-nonzero shows SCORE/TIME/radar/ladder/FRND boxes; effects-nonzero texels 144466/191553/180579 vs hud-nonblack
  137882/142804/134208; alpha-gt0=0 (HUD blend has src-alpha ZERO, so alpha stays 0); b3 additionally holds the AWACS subtitle which is a tm-phase draw (absent from diff-b3). With only 2-5 committed draws this content can only
  be the tail draws (hud-region and after-tonemap, predicate 0 by design). => live, the predicated-off tail draws are EXECUTED (predication not honoured on this GPU/driver for the persistent UPLOAD buffer, or the
  epoch word is non-zero). [I, strong; contradicts audit item 1 "[G] never observed"]. Consequence: "predicate off" cannot be what hides the smoke (it would then have been executed too), and effects
  already double-render the HUD (the separation question in 3 is currently open in the wrong direction).

## 3. Candidate causes of "committed, no pixels" for ps d2464ed2 (rank)
1. [G] Shader samples something the replay changes: the smoke PS (dual-source, alpha 18/19) may read the scene depth/colour (soft particle / emulated framebuffer read); our private DS (0) is irrelevant for SRV reads,
   but a bound SRV may be the scene RT itself in a state/transition the replay does not match (reads black / zero alpha).
2. [G] Depth: fragments depth <= 0 (far, PS2 depth convention "larger is nearer") fail GREATER against the cleared-0 DS; the sprites in the effects image are 2D (depth ALWAYS in HUD, GREATER_EQUAL sprites near).
3. [G] Dual-source output: rgb = s.rgb*s.a, alpha = a0*a1; a PS that writes the real contribution only through SRC1 (second output) gives 0 rgb on a zero target. The consumer would still need alpha.
4. [G] The draw sequence relies on state set by the game's own preceding non-draw commands not tracked (e.g. a root constant/SRV changed between the draw and our recorded replay): not a refusal category.

## 4. Design to capture the smoke (once the cause is removed)
* The smoke is already in the "between-members" epochs the design commits, so NO new post-last-member region is needed. The tail region must stay out: given the finding above, make the tail draws not run:
  (a) do not record speculative replays at all after the LAST member is known: impossible inline (last member unknown), so instead (b) record every candidate under predicate, but ALSO gate the replay on a
  CPU-side rule that works without predication: candidate whitelist by shader class for the effects target: only PS/VS pairs seen between members and never in the hud-region/tm phases (here
  9eff1f92/d2464ed2 and the 4caf6e2d sprites with depth GREATER/GREATER_EQUAL); EXCLUDE depth ALWAYS draws with vs 534f29ca/ps d34ee153 (the HUD sprite class: 134..197 draws, all depth ALWAYS, src-a ZERO).
  Separation criteria that exist in the data: depth func GREATER/GREATER_EQUAL (effects) vs ALWAYS (HUD); ps d2464ed2 / 4caf6e2d vs d34ee153; non-indexed vs indexed text quads; the HUD region starts at the first
  draw after the last member (known only at close), the first stencil EQUAL/NOT_EQUAL draw (ords 189-196: front:equal/not-equal) marks the subtitle/marker block.
* To keep the predicate for tail separation, also fix/verify it (see 5): replace the persistent-upload predicate by a copy (CopyBufferRegion from the upload heap into a DEFAULT predicate buffer with a
  barrier to PREDICATION state at the member commit) or by ExecuteIndirect-free alternative: replay the epoch only AFTER the list is closed (second small command list executed before the game's list is
  impossible: draws need the game's state), so the practical option is the whitelist above plus predicate.
* If the smoke needs scene depth/colour, replay it into a copy of the scene (scene-end style) instead of an empty target.

## 5. Smallest read-only log for the next run
1. At the effects replay (per committed draw, first 8 per frame, only when a request file is pending): epoch index, predicate offset, the byte value read back from the mapped predicate memory at Close,
   vs8/ps8, depth func, blend tuple, and the draw's index/vertex count, ord, and phase of the observer ("btw"/"hud"/"tm"). Decides: which draws execute (tail epochs value 0 or 1?) and whether d2464ed2 is among the replays.
2. At the burst readback add `png-effects-tails`: effects-nonzero texels inside the bbox of the diff-b3 changed region (changed-bbox of the smoke) and the max alpha/rgb there (cheap CPU loop over the already read effects image),
   plus the same for the HUD bbox, to separate "tail executed" from "smoke black".
3. Optional (read-only offline): png_diff.py world vs scene-end with a cockpit mask restricted to the trail bbox, printed with the draw ord range of btw d2464ed2 draws (observer sums per phase and shader class:
   add per-phase counts of draws with ps d2464ed2 / 4caf6e2d / d34ee153 to `png-burst-phases`).
