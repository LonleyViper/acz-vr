# J-world-objects v2: design (analysis only, nothing implemented or run)

Evidence: `research/experiments/step29-live-run1/logs-final/step5-producer.log`, D-268, `gpu_copy_step5_hooks.inl`
(line numbers below), `gpu_copy_step5_world.h`, `docs/interception/stored-draw-manifest-1.md`/`-2.md`.

## 1. What the live run shows (and does not)
- The periodic line is `event=continuous-world` (22 lines; the first one is `continuous-status-world`). Last one: `seen=253928 before-frame=253928`, 52 identified PSOs,
  `refused` all 0, `frames=0 members=0`, with 6288 cockpit frames published. So ~40.4 DrawACM_v draws per published frame (capture 11906 has 34), and
  NOT ONE was observed while a frame was active (a draw with `f.active` and another list would count as `seen` but not `before-frame`, and none did).
- `camera=not-recovered` is an artefact: `s5.wobj.camera` is only set inside `step5WobjWhy` (l.1064-1069), which is never reached because
  `step5WobjMember` returns at l.1085 (`!f.active||f.closed||f.stopped`). It is also reset per frame (`step5FrameSetup`, l.633), so the status line only
  reflects the last frame. It says nothing about whether camera recovery from C would work. v1's `camera_setup` (world.h l.77) is unit-tested only.
- The counters cannot tell whether the ACM draws are in the cockpit list (before its first CPT draw) or in other lists (`seen` is counted before the
  `step1Route(key)!=f.list` test at l.1086).

## 2. How the frame start is decided today
- Only the non-world branch of `step5Draw` (l.1122-1179) starts a frame: a DrawCPT-identity PSO (`drawcpt_identity_matched`), in a qualified list
  (`feed->insertion_candidate`, timeline tokens), `step5MemberWhy` ok (`step5StateWhy`: size/format/depth-clear/t10/stencil + `step5ClassVerdict`), live SrtData
  readable and `camera_frame` ok (by-class), a free slot (`step5PickSlot_locked`). By-class (`gpu_copy_step5_members_by_class`) removes the ordinal range.
- The frame is then bound to ONE list generation (`f.list`, `f.generation`). `step5FrameSetup` (l.631) recovers camera/lens/poses once, `step5RecordMember(first)`
  (l.835-864) records, in this order: world snapshot (`step5RecordWorld`, l.816: 4 barriers + CopyTextureRegion of the game scene RT into `sl.world`),
  ClearRTV x2, ClearDSV x2, then per member {OMSet eye, SRV t10, Draw} x2 eyes + restore. Frame ends at that list's Execute (`step5BeforeExecute` l.1327 /
  `step5AfterExecute` l.1378); publication needs the Close and the private-fence Signal.
- The world snapshot is therefore "everything the GPU has executed into the scene RT before the cockpit list's first member", i.e. sky/terrain AND every ACM
  object whose list executes earlier on the queue (at least; same-list ACM draws before the first CPT draw too).

## 3. Why ACM draws precede it (best explanation, honest confidence)
- By design: world draws never start a frame (l.1085, l.1109). Live evidence for the ordering is strong that the cockpit list begins with CPT draws:
  D-263 notes cockpit members at list ordinals 1..22 (pre-cockpit pairs) then 23..29; `class-family-admit ordinal=2 frame=1` (the first frame started at
  ordinal <= 2). `draw_ordinal` = `l->draws` = all draws recorded in that list generation (step3 hooks l.190). So at most ~1 ACM draw can sit in front of
  the first CPT draw of the cockpit list; the ~40 ACM draws per frame are therefore (very probably) in OTHER lists recorded before the cockpit list
  (the capture's pass 0: 6 ACM at global ordinals 3-8; the pass-6 ACM 35-51 precede the CPT 52-68 in the capture, but that capture's list layout is not the live one).
- Unknown (needs Step 0 below): which lists the ACM draws are in, whether those lists execute before the cockpit list on the SAME queue, whether the
  ACM draws target the same RT as the cockpit (`step5StateWhy` would refuse a different size/format/depth clear; pass-0 ACM RTV 347/DSV 348 2560x1440 vs
  cockpit rtv=10/dsv=20 FP16/D32S8: identity of the resource is not recorded), and the per-frame draw order of ps2_v terrain/sky relative to ACM
  (manifest 1: pass 0 has ps2_v draws 0-1, DrawMesh 2, ACM 3-8, then unknown; "ps2_v much of pass 6" interleaves with ACM/CPT).

## 4. Lifetime / memory: what the cockpit path actually holds (this constrains the design)
- It does NOT record draws for later replay. The replay is INLINE: `step5Draw` runs inside `hookedDrawIndexedInstanced` (d3d12_hook.cpp l.2060-2077)
  BEFORE the game's own native draw, and appends {OMSet(eye), SRV t10, same native Draw, x2, restore} to the SAME list under the same bound PSO / root
  signature / descriptor tables / VB / IB / viewport / scissor. VB/IB/descriptors are never captured; their lifetime is the game's own draw (they must be valid
  for the game's draw immediately after).
- The only data held: the 416 B SrtData, copied at the seam (l.843-845) into the slot's private UPLOAD area (`uploadOffset + 2*idx*blob_stride`, two
  substituted records per member) and referenced by GPU VA in the recorded SRV call; plus the private slot targets. Slot reuse is gated by the private fence
  (`lastUse`, J-3b9) so the GPU is finished before the upload area / eye targets are rewritten.
- Consequence: "record the draw before frame start and replay it later in the frame" is not implementable cheaply. A later replay (even in the same
  list) would have to re-establish PSO, root signature, root params 0-3 (descriptor tables into game descriptor heaps that the game recycles), VB/IB views,
  topology, viewport, stencil ref: none is tracked as replayable state. Different list = additionally no inherited state at all. So v2 must keep the
  replay INLINE at the ACM draw and instead make the eye targets/slot/pose exist EARLIER.

## 5. Camera recovery for ACM (what v1 does, what v2 must change)
- v1: `world::camera_setup` (world.h l.77-97): VP = C (+64), `decompose` -> absolute eye/axes/tan, S = inverse(C) x A, A|C kept as reference; requires
  axes dot >= 1-1e-5 and lens 1e-4 against the COCKPIT camera (`s5.pf.cam`), so it needs the cockpit frame already set up. Later records: `record_matches`
  (A|C equal 1e-6; bit-identical in evidence). Poses: `setup_poses(f.pose, s5.neutral, w.pf, ...)` with the cockpit's shared neutral; D_e = VP_e - VP_ref;
  `substitute` writes C+D_e and A + D_e x S. Cockpit eye is camera-relative (~0), ACM eye absolute (~(-14360,3492,123218)): two different frames of reference, same
  orientation and lens (3x3+w equal to 3.2e-6).
- Never exercised live. Risk list: (a) C and A are shared by all ACM draws of the frame, so one decomposition per frame is right, but it is untested that the
  live C equals the capture's C pattern; (b) float32 absolute coordinates ~1e5 (unit test: ~1 px error at 1280); (c) depth: ACM +360 flag is 1.0 vs 16.0,
  z column differs (176 vs 11): occlusion between ACM objects and cockpit parts in the shared eye DS is unproven; (d) the `world-pose-unavailable` refusal when the
  pose provider is not ready (continuous-first shows `pose=pose-unavailable` for frame 1).
- v2 change: a world-led frame cannot compare against the cockpit camera at the start. Do the camera_setup with `cockpit=nullptr` (already supported), and
  do the equality check when the first CPT member arrives (cockpit camera_frame + setup_poses with the SAME `f.pose`, `f.spaceGeneration`).

## 6. Double rendering (objects in the flat snapshot and in the per-eye replay)
Options, ranked for a first live test:
1. ACCEPT overlap (test only): leave the snapshot at the cockpit start. With zero head offset the two coincide (a free registration check: objects must overlay
   their ghost exactly); with head offset the replay shows parallax against the ghost. Needs no new risk.
2. Snapshot before the first ACM draw: ghost-free for ACM, but then the quad lacks everything drawn after that point that is not replayed (ps2_v terrain
   drawn after ACM, DrawMesh, effects, any pass between). Only valid if measured order shows all non-ACM scene content precedes the first ACM draw: NOT shown by the
   manifests (pass 0: ps2_v 0-1, DrawMesh 2 then ACM; pass 6 interleaves). Do not use without the Step 0 order trace.
3. Suppress the game's own native ACM draw when the replay was recorded (hook returns a skip flag from `step5Draw`, fn(...) not forwarded at
   d3d12_hook.cpp l.2057/2075). Removes ghost AND mono duplicate, but changes the game's scene RT and depth (later depth-tested passes, post effects), and the
   flat monitor view. Highest value, highest risk; separate default-OFF key, only after 1 works.
4. Mask/erase in the snapshot: not possible (cannot un-draw); depth-based masking would need the scene depth at snapshot time and a consumer shader: out of scope.

## 7. Minimal safe v2 (recommended), conditional on Step 0
Assumption from section 3: ACM draws are in lists recorded BEFORE the cockpit list, same queue, executed before it. Then the replay must be recorded in those
lists, so the frame has to span lists ("carrier lists"), and the cockpit list adopts it.

State additions (Step5State, step-1 mutex): `pend` frame = {slot, pose snapshot (f.pose, predicted, spaceGeneration), wobj::Frame (camera from C), idx/upload counter,
carrier list set (route, generation, commands, closed, submitted), clearsDone, openedAt}.
1. First ACM draw with no active/pending frame, only if gated: list route == `s5.lastList` (the list that hosted the last published cockpit frame) OR
   the first N=3 learning frames; world key on; continuous; ring ready; list qualified (copy-observation witness + timeline tokens, l.1138-1142);
   state ok (`step5StateWhy`); other-target ACM draws (size/format/depth-clear/DSV) are refused alone and NEVER open a frame (reuse `other_target`/`out_of_class` semantics);
   free slot (try-only); `camera_setup(raw, nullptr, ...)` ok; pose read; world camera loosely within 0.05 rad of the last published cockpit axes (cheap guard, since objects cannot
   be un-recorded). Opens the pending frame: picks the slot, records the 4 eye clears (ClearRTV x2, ClearDSV x2) as the first commands in this carrier list.
2. Each later ACM draw (any list, while pending): `record_matches`, `substitute`, `step5RecordMember` with `first=false` (needs a `clearsDone` flag instead of
   `first`, and the snapshot decoupled from `first`). New carrier lists get the same list qualification. Cap 32 draws and a per-carrier command cap.
3. Cockpit list start while pending: adopt (do not pick a slot; do not re-read pose): `camera_frame` on the first CPT member, setup_poses with the pending `f.pose`,
   require axes/lens equal to the world camera (`world.h` tolerances) and `fov_matches`; take the WORLD SNAPSHOT here (unchanged position = option 1) and skip
   the clears; arm the HUD region on this first CPT member (`step5HudMember_locked(first=true)`, today tied to `first`) and call `ee_camera::notify_first_member` here.
   Mismatch -> mark `poseBroken` (executes, never published, slot retired by fence), counted.
4. Carrier accounting in `step5BeforeExecute`/`AfterExecute`: every Execute of a carrier list gets a private-fence Signal (`out.signal`, `slot.lastUse = value`) so the slot is
   reusable only after the LAST carrier completes; only the cockpit list's Execute publishes. Publication requires: all carriers closed AND executed before the cockpit
   list in queue order AND on `st->queue`, clears' carrier executed. Carrier Reset/discard (`step5NoteReset`) after clears -> frame invalid (slot quarantined/retired, never published);
   after only draws -> `partial`. Pending frame with no cockpit list within e.g. 2 frames or 50 ms -> abandon: no publish, slot retired by its carrier fences.
5. Same-list case (ACM before CPT in the cockpit list) is the degenerate single-list version of the same code (carrier == cockpit list); no extra branch.
6. Status line: add `pending-opened`, `adopted`, `abandoned`, `carriers/frame`, `adopt-camera-mismatch`, `clears-lost`, and fix the misleading `camera=` field to
   report the last attempt and a lifetime count of recoveries.

If Step 0 instead shows the ACM draws in the cockpit list before its first CPT draw (single list), only items 1-3 and 5 are needed and 4 collapses to today's accounting.

## 8. Measure live next (read-only; modelled on `gpu_copy_step5_precockpit_observe`, no commands recorded)
Step 0 observer, default-OFF key, ~3 frames fully then 1 per 5 s, POD ring + worker formatting:
- per ACM and per CPT draw: list route, generation, per-list ordinal, Close/Execute sequence number of the list, queue identity, batch index within
  ExecuteCommandLists; RTV/DSV resource pointers, sizes, formats, `game_rt == worldSource`; viewport, depth func/write/clear; PSO route.
- per frame: ordered list of (list, #ACM, #CPT, #other) in record order and in Execute order; recorded-before-execute gaps; whether the ACM lists are reused objects
  across frames (`s5.lastList` equality) - decides the gate.
- ACM record facts for the first ACM draw of each frame (CPU read): C 3x3+w vs the cockpit VP (axes dot, tan), A/C bit-identity across the frame's draws, +360 flag,
  model translation vs camera eye (distance), count of draws that pass `record_check`.
- ps2_v/DrawMesh/other draw positions relative to the first and last ACM draw (decides Option 2).
Live checks after implementation: world-camera recovery rate (`recovered` per frame), `adopt-camera-mismatch` = 0, objects overlay ghost exactly at neutral head pose (screenshots both
eyes, SteamVR mirror), parallax with head offset, no flicker when `members` varies, cockpit occlusion order (canopy vs distant aircraft), GPU commit (already 77.7% peak; slot
clears add 2 RT + 2 DS clears per frame only on opened frames), producer fps (55 -> ?), slot starvation (`no-slot`, `gpu-busy`), zero `ambiguous`/`failures`.

## 9. Ordered implementation plan and tests
1. Step 0 observer + fixture (key off: identical command stream; sampling; bounded line length). Run live once; decide single-list vs carrier.
2. Fix the status line semantics (camera counters) and add `seen-in-active-other-list` counter (cheap, immediately resolves ambiguity of section 1).
3. Refactor `step5RecordMember` so clears, snapshot, HUD-arm and `notify_first_member` are separate flags (no behaviour change; byte-identical command stream tests for the existing
   fixtures `step5-members-by-class`, `step5-world-objects`).
4. Pending-frame state machine (offline fake GPU): tests = ACM-led single list; ACM in earlier list then cockpit list; ACM list never followed by cockpit (abandon, slot retired);
   carrier Reset before/after clears; carrier executed after the cockpit list (refuse publish); two carriers; camera mismatch at adoption (poseBroken); slot starvation; cap 32; key off
   unchanged; zero offset gives raw bits for A and C; HUD region armed on the first CPT member only.
5. Live null-HMD run (same config as step29 + new keys), then headset only after the overlay/ghost registration screenshot passes.
6. Only then: option 3 (suppress mono ACM draw) behind its own default-OFF key, with before/after screenshots of the flat game window.
