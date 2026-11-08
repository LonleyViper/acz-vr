# J-146: XR stage design — frame ownership and image-use evidence

Design only, offline, against the J-145 baseline `f4db588`. This document starts
no session, changes no executable code and approves no live experiment.
`DECISIONS.md` D-178 records the user's words, "headset gate authorised";
D-179 queues this design and still requires reviewed, concrete experiment cards
with the user present. Earlier gate-4 prohibitions in J-007/J-143 describe their
historical scope; D-178 does not establish any VR result or waive their technical
prerequisites. The staged outcome remains `docs/proposals/native-6dof-proof.md`.

Evidence convention: paths below are repository-relative. Source citations
establish implementation, not native execution. Historical native observations
are attributed to the cited decision/proposal, not reproduced here. No raw
registered evidence log, original binary, external reference or network source
was opened. **Every proposal, API-contract premise, numerical budget, expected
outcome and likelihood/impact estimate below is an ASSUMPTION for review unless
explicitly identified as a repository fact.** API premises need checking against
the available OpenXR/D3D12 SDK before implementation; this is not a substitute
for that check. In particular, deadlines do not imply cancellable runtime calls.

## 1. Inventory: implementation and evidence limits

| Repository path and symbols | What exists / what the tests inspect | What is not established |
| --- | --- | --- |
| `src/acz-vr/openxr_session.h`: `XRDispatch`, `GraphicsDeviceInfo`, `EyeView`, `FrameState`, `OpenXRSession`; `src/acz-vr/openxr_session.cpp`: `init`, `createSwapchains` | Injectable loader dispatch; HMD system, D3D12 graphics requirements and LUID/feature-level/DIRECT queue/device checks; LOCAL space; exactly two views; enumerated formats restricted to `DXGI_FORMAT_R16G16B16A16_FLOAT`; separate recommended dimensions and image arrays per eye. `tests/acz-vr/openxr_session_offline_tests.cpp::initialization` supplies fake handles/graphics facts, fails initialization stages 1–10, tests six graphics mismatches, unsupported format/view count/null inputs and fake cleanup counts (J-010). | No real adapter compatibility, runtime format support, allocation, residency, headset availability or destruction safety. A game scene-target format is not an XR format witness. |
| Same files: `pollEvents`, `handleSessionStateChange`, `beginFrame`, `endFrame` | READY/STOPPING and loss handling; wait/begin/locate/end bookkeeping; invalid views produce no layer; stored frame timing/views resist caller mutation. The test's `sessionEvents`, `frames`, `cleanupFailure` and `ownership` cover fake failures, one end attempt, two-eye completeness and zero layers. | No real event timing, actual presentation, runtime recovery or bounded call duration. `pollEvents` drains in an uncapped loop; `beginFrame` calls `xrWaitFrame` synchronously. |
| Same files: `acquireSwapchainImage`, `releaseSwapchainImage`, `markEyeRendered`; `SwapchainInfo` | Tracks acquired/waited/rendered/released plus index. `ownership` rejects duplicate/out-of-range acquisition, no-wait release, bad image index, timeout/error and incomplete eyes; fake release failures retain ownership. `frames` tests a two-eye fake projection layer. | **`markEyeRendered` accepts a caller assertion, with no fence, command or pixel evidence.** Both acquire and end-frame cleanup use `XR_INFINITE_DURATION` for image waits. Fake wait ignores the timeout argument. No D3D12 work is recorded, no completion is authenticated. Existing cleanup is not approved for pending native GPU work. |
| Same files: `recenter`, `xrPoseToPose`, `xrFovToFov` | Converts named XR fields; recenter uses eye-position midpoint and left-eye orientation as an approximation. Test `recentering` exercises fake midpoint translation, invalid flags and vertical-heading preservation. | No tracked head-center reference, reference-space-change generation handling, human scale calibration or compliance of this approximation with the full pose contract. Valid bits alone are not a measured tracking-quality guarantee. |
| `src/acz-vr/stereo_renderer.{h,cpp}`: `StereoRenderer::setAircraftPose`, `setDrawCPTCalibration`, `computeEyeMatrices`, `substituteDraw`, `substituteAllDraws` | Row chain `T_X_E * T_R_X * T_A_S * T_W_A`, then `C_W_N * inverse(T_W_E) * P`; per-eye asymmetric FOV; finite intermediate checks and atomic publication on exceptions. `tests/acz-vr/stereo_math_offline_tests.cpp`: `composition`, `asymmetricProjection`, `recenter`, `validity`, `finiteMatrices` exercise noncommuting transforms, frustum/depth edges, tilt/IPD, invalid inputs, overflow and infinite far plane (J-010). | No native aircraft/seat/calibration provider, actual stereo draw, upload or GPU target. Invalid-eye flags can leave just one eye valid; the future packet owner must require both. Identity calibration and synthetic near/far values are not measurements. `substituteAllDraws` returns two matrices per draw, not full SrtData. |
| `src/acz-vr/cockpit_math.h`: `poseToWorld`, `projectionFromFov`, `computeRecenter`, `extractHeadingYaw`, `substituteEye`, `ps2ViewportMatrix` | CPU math used by the stereo tests. `docs/proposals/pose-contract.md` (J-003/J-004/J-006) defines the column contract, row adaptation, named FOV mapping and synthetic fixture expectations; cites `tests/pose/test_pose_contract.py`, `tests/pose/test_native_mapping.py`. | Default PS2 viewport/depth algebra in a helper is not a measured native clip/screen adapter. The contract explicitly leaves physical scale, neutral camera and native depth open. The Python files were not independently run in this job. |
| `src/acz-vr/stereo_payload.{h,cpp}`: `stereo_payload::prepare`, `output_size`, `Batch` | Complete immutable CPU payloads, explicit caller eye VPs and clip-to-screen mapping, replacing only +0/+64 and retaining provenance (J-022). `tests/acz-vr/stereo_payload_offline_tests.cpp`: `success_tests`, `failure_tests`, `accounting_tests` check synthetic 416/512/2048-byte strides, duplicate instance order, unchanged tails/source, separate eyes, allocation faults, overflow and 16 MiB output accounting. | No native mapping K, GPU upload, immutable draw-time game bytes, replay or XR scheduling. `gpu_replay_complete` stays false in the fixture. |
| `src/acz-vr/dllmain.cpp`: `gpu_transactions_enabled`, `deviceReady`, `execute`, `Runtime`, `DllMain`; `src/acz-vr/d3d12_hook.cpp`: `hookedExecuteCommandLists`, `captureDeviceAndQueue` | Constant `gpu_transactions_enabled=false` keeps XR callbacks inert. Dormant execute code is an event pump, explicitly not a frame owner. Hook forwards original Execute arguments once; its callback occurs before native Execute. No Present-frame owner is supplied here. `tests/acz-vr/dll_lifecycle_offline_tests.cpp::main/noFrames` use fake session/setters to check inert concurrent/reentrant callbacks, registration failures, no frame/math actions and no detach cleanup (J-021, D-176/D-177). | No production arming, display-frame identity or reachable populated XR shutdown. Merely flipping the constant would put synchronous initialization/event work on callback threads and is rejected by this design. `d3d12_hook.h::endUnrenderedFrame` changes a caller copy, whereas `OpenXRSession::endFrame` ignores that argument: it is not an independent force-zero-layer mechanism for an already rendered stored frame. |
| `src/acz-vr/gpu_copy_ledger.{h,cpp}`: `gpu_copy::Session`, `Key`, `Admission`, `Backend`, `Snapshot`, `diagnostic_session` | J-144/J-145 one-attempt ledger, one record, 1 MiB private allocation charge; orthogonal closure/completion/consumption/retirement facts; private timeline and drain veto. `created` retains the first late result even after quarantine. `tests/acz-vr/gpu_copy_ledger_offline_tests.cpp` covers admission, creation receipts, completion, refusals and races; `gpu_copy_drain_offline_tests.cpp` exercises actual drain/forwarding with fake COM; fixture injection lives in `gpu_copy_ledger_fixture.h` (D-176/D-177/D-179; `src/acz-vr/J-144-ledger-findings.md`, including J-145 addendum). | No native arming, Signal/Map adapter, from-Reset closure, insertion restriction coverage or native completion. Fifty polls is a call cap, not a deadline. Tests cannot authenticate their injected fence/queue/history facts. One 1 MiB record is not a ready-made two-eye allocation budget. |

All tests above are **synthetic numerical, fake-runtime or fake-COM tests and
demonstrate no engine behaviour or runtime parity**. They cannot establish a
headset saw an image. This job inspected the cited C++ tests; it did not execute
them independently. `src/acz-vr/CMakeLists.txt` registers the corresponding
offline targets; registration is not an execution result.

Native status is narrower than source capability: D-172/D-173 in
`DECISIONS.md` report metadata and sampled diagnostic bytes with
`draw-use-unproven;gpu-completion-unproven`. J-143's
`docs/proposals/l3-gpu-copy-design.md` reports the L3 evidence references and
conditional copy proposal; this job has not reread its raw logs. D-176/D-179
report offline ledger merges, not native GPU transactions. D-178 explicitly
records no demonstrated stereo or OpenXR submission. J-007's
`docs/proposals/cockpit-openxr-layer.md` supplies historical cockpit/world split
and unresolved camera/depth facts; its original hook-status descriptions predate
later metadata work. `docs/proposals/l3-job-plan.md` explicitly deferred stereo,
frame ownership and OpenXR integration (Astra stage 6).

## 2. Frame and simulation ownership

### Proposed owners and boundary

ASSUMPTION: the game exclusively owns simulation, its DXGI swapchain and every
original Present/Execute. Forward each original call once, with its original
arguments/result; do not issue an extra game Present, change sync intervals,
rerun simulation, Reset a game list or make the game wait for XR. A future
observed Present may be a diagnostic publication boundary only after chain,
mode and generation have been authenticated. **Present is not proven to be a
simulation tick**, and Execute is not a display frame (`d3d12_hook.cpp`,
`dllmain.cpp`; `src/acz-vr/J-130-l3-observation-findings.md`).

ASSUMPTION: one explicit outside-loader-lock XR owner, on a dedicated worker,
serializes session/event/frame/image calls. Its loop is:
poll a bounded event batch; `xrWaitFrame`; `xrBeginFrame`; locate views at that
wait's predicted time; choose one immutable diagnostic packet; render at most
one pair; `xrEndFrame` once. A successful begin, including shouldRender=false,
owns one end attempt. Never retry an ambiguously consumed end. Do not call any
XR function from the game's render/present/Execute hook, device-ready callback,
DllMain, or under a hook/ledger mutex. Move initialization off those callbacks;
do not reuse the dormant `deviceReady`/`execute` path unchanged.

ASSUMPTION: no one-to-one coupling of Present and XR frames. A capacity-one
mailbox carries only owned immutable CPU packets and lifetime/generation keys;
publication uses a try-only operation and drops on contention/full. No raw game
pointer passes to the XR worker. The game never joins that worker. XR consumes
only a complete pair from one packet, never independently samples the two eyes.
If no eligible packet exists, submit zero layers; do not wait for the game.
Initially packets contain only synthetic diagnostic geometry, with no game
resource dependency. Future game replay needs a separately accepted L3 source,
full private replay and a real simulation snapshot ID k. A queue serial or
Present count cannot fabricate k (`pose-contract.md`; `native-6dof-proof.md`).

ASSUMPTION: both eyes share k, space/recenter generation and predicted-time
sample; advance the game once through its existing loop, never once per eye.
No game tick suppression or engine replay is proposed here. Require both valid
eyes and identical packet IDs, and preserve raw X-space eye poses/FOV in layer
submission, while rendering uses the contract's world mapping. Head/IPD enters
once. Reference-space changes invalidate the packet/recenter until a new valid
sample is explicitly accepted. The current session does not implement this full
generation contract (`pose-contract.md`; inventory above).

### Numeric bounds and refusal to block

All limits in this table are proposed ASSUMPTIONS; none was measured here.

| Operation | Proposed bound and enforcement | On refusal/expiry |
| --- | --- | --- |
| Game-side XR handoff | 0 ms wait allowance: no XR call, fence wait, join, condition-variable wait, sleep, spin, blocking mailbox lock, Map or hash in a game callback. One try-only publication; fixed small preallocated metadata. | Drop diagnostic publication; forward game call. This bounds the added protocol, not arbitrary OS scheduling time or existing hooks. |
| Initialization | One attempt per process/session card; 5 s total startup watchdog, no repeated runtime discovery. | Disable XR for this session. An in-flight foreign call cannot be cancelled by this timer; retain owners. |
| Event polling | At most 16 events per worker iteration and 1 ms cooperative processing budget between calls. | Overflow/budget exhaustion stops admission; no endless event-drain loop. A single stuck foreign call remains a separate hazard. |
| `xrWaitFrame` | **No timeout field exists in the call used by `beginFrame`.** Worker only; watchdog at 250 ms without return/progress; overall card hard stop as below. | Stop further admissions, latch XR disabled. Never claim the watchdog interrupts the call. Never run on game render/present thread unless a later review proves a bound; this design does not do so. |
| `xrWaitSwapchainImage` | Explicit `timeout=0` first; at most one further wait per image with 1,000,000 ns (1 ms), worker only; never `XR_INFINITE_DURATION`, including cleanup. | `XR_TIMEOUT_EXPIRED` is not readiness. Do not render/release the unready acquisition. Retain its ownership; stop acquiring more images and enter bounded abort disposition. No repeated wait loop. |
| Own XR GPU fence | Nonblocking GetCompletedValue samples at most 1/ms for at most 20 ms per frame, on the owner; no game-queue Wait or CPU fence wait. | No rendered witness, layer or early release. Stop the card; bounded cleanup can continue observing the same pending token, at most 10 polls/s for 2 s, then quarantine. |
| Cooperative stop/teardown | Stop token checked before/after each call; 2 s owner cleanup observation budget; no game-thread join. Maximum one outstanding XR frame/two acquisitions. | No unsafe destroy, thread termination, unload or freeing possible GPU references on timeout. Preserve owner/module; record incomplete teardown and forbid in-proxy progression. |

ASSUMPTION / hard limitation: `xrWaitFrame` and other in-process foreign calls
cannot be made hard-time-bounded by a watchdog thread. Do not destroy a session
from a competing thread to escape a stuck call. A standalone harness can be
terminated as its own process by its supervisor after the 2 s grace; record that
as failed clean teardown, never success. The game is not that harness. In-proxy
steps require reviewed ownership/stop handling, standalone measurements and an
explicit card accepting the remaining hang/crash risk; if hard isolation or a
guaranteed finite in-process session is required, **refuse those steps** and
keep the probe out of process. Killing a game worker thread is not a fallback.

ASSUMPTION: absent headset, SteamVR not already running, unsupported device or
format, runtime-unavailable error, session/instance loss, exception or device
loss => latch XR disabled for the rest of this game session. No runtime launch,
automatic reconnect, retry storm, identity-pose fallback, device recreation or
silent format substitution. A new attempt needs a new explicit card/session.
Do not call `xrCreateInstance` merely to try to start SteamVR: user starts it
first and the card checks readiness without changing configuration. Lack of a
readiness witness refuses the attempt. XR disablement leaves game output and
original calls unchanged. This is a design goal for recoverable failures, **not
a guarantee against a native runtime crash/device removal in the game process**.
Current dormant callbacks do not implement this failure latch completely
(`dllmain.cpp::execute` ignores a false poll result), another enablement blocker.

## 3. XR image-use witness and GPU-copy ledger interplay

ASSUMPTION: add a separate XR owner ledger, default OFF, with session/device/
queue/space/swapchain lifetime epochs; frame serial and predicted display time;
eye, acquired index and acquisition serial; native resource identity; format,
dimensions, array slice/rect; command-list/allocator recording epochs; render
packet ID and pose/FOV/recenter IDs; fence identity/value and all call results.
Do not use an address alone as identity. Keep timestamped transitions and thread
IDs in a bounded log, with loss-of-record counters that invalidate proof.

Proposed required sequence, independently for both eyes:

1. Record successful wait/begin frame and valid located two-eye sample, or end
   with zero layers if shouldRender=false/invalid sample. No image acquisition
   is needed for a zero-layer frame.
2. `xrAcquireSwapchainImage` succeeds, returning in-range index j. The witness
   associates j with this swapchain's enumerated `ID3D12Resource`, not the game
   backbuffer or the other eye. Exactly one outstanding acquisition per eye.
3. `xrWaitSwapchainImage` returns **XR_SUCCESS** within the explicit wait policy.
   Neither positive timeout status nor acquisition alone permits access.
4. Use only proxy-owned allocator/list/RTV/upload dependencies to record an
   actual clear/draw to that exact image/rect; retain all owners. Authenticate
   the graphics binding device/queue, native image description and resource-state
   contract first. Proposed D3D12 premise: transition the acquired image from
   the runtime-required boundary state to RENDER_TARGET and back to the required
   release state (proposed COMMON); **review the exact D3D12 OpenXR binding rule
   before use**, never infer state from an old log or call the game backbuffer
   PRESENT state applicable. Unknown state or alias refuses rendering.
5. Submit that closed private recording once on the authenticated XR binding
   DIRECT queue; Signal a private, exclusively GPU-signalled fence after it.
   Keep exact submit/result/queue epoch joins. No CPU Signal, reused fence
   value, cross-device assumption or unnamed cross-queue dependency. Signal
   success and Execute's void return are not GPU completion.
6. Only a valid non-regressing sample on the same device/queue/fence epoch with
   completed value >= issued value establishes the conservative GPU-complete
   witness. Reject UINT64_MAX/device-removal before that comparison. Reject
   missing/duplicate/reentrant submissions and ambiguous Signal. Never Reset
   or reuse a private allocator/list until its GPU work is complete and future
   execution of that recording is excluded by the owner.
7. Bind the completion witness to the image and its recorded render operation;
   only then may a future replacement for `markEyeRendered` accept it.
   `xrReleaseSwapchainImage` must succeed after readiness and GPU completion.
   No further image access after release. A failed release does not discharge
   ownership. Native-call success plus fence progress shows ordered operation
   completion, not that the headset displayed the intended pixels.
8. `xrEndFrame` may name a two-view projection layer only when both images have
   complete matching witnesses and successful releases in this frame. Use the
   exact rendered raw X-space poses/FOV, space generation and predicted time;
   never submit transformed world poses. Otherwise use zero layers only when
   legal for the remaining frame/ownership state. Make one end attempt, log
   its result, and do not retry an ambiguous end.

ASSUMPTION: if an image remains acquired-but-unready or GPU-pending at abort,
never fake readiness, release, completion or clean shutdown to meet a deadline.
Latch disabled, preserve that frame/image record and dependent owners; perform
only a reviewed legal zero-layer/end/teardown path after any in-flight call
returns. Whether an end with retained acquisitions is valid must be checked
against the local SDK before implementing abort. Until then this case refuses
live enablement. A 2 s cleanup timeout preserves uncertainty; it does not prove
resources are free. Do no foreign cleanup at DllMain/process termination
(`dllmain.cpp`; D-176/D-177).

### Relationship to J-143/J-144/J-145

Repository fact: `gpu_copy::Session` can be armed only through the fixture
friend today. Its Key covers device/queue/fence/list/generation; its orthogonal
flags, owner leases, one-attempt charge and global-drain veto model conservative
readback lifetime, not XR images (`gpu_copy_ledger.{h,cpp}`;
`J-144-ledger-findings.md`; D-176/D-179). J-145's `created=true` means ownership
receipt even for a quarantined late completion; it does not authorize work.

ASSUMPTION: retain these principles in an XR-specific ledger, rather than
reinterpret readback `consumed` as image release or reuse its 1 MiB cap for
runtime-sized stereo images. A future XR budget must charge private allocations
before creation, enumerate runtime image counts/dimensions and impose reviewed
limits; runtime/driver residency remains unbounded by logical bytes. One owner
admission barrier must veto global cleanup while **either** copy or XR records,
polls, releases or in-flight calls remain. Stop closes experiment admission,
not the original game-hook forwarding path. Late creation results transfer once
into quarantine; duplicates cannot overwrite held owners. No COM under locks.

ASSUMPTION: for later game-data stereo, each accepted L3 copy manifest is joined
by transaction ID, source lifetime/draw revision, recording generation and
submission serial to the immutable render packet. Copy completion proves
neither XR render completion nor image release. Give XR work its own fence and
queue timeline. If the queues differ, require an explicitly reviewed completed
copy dependency before CPU publication, or an authenticated GPU queue Wait on
the **private XR queue**; never stall the game's queue. Never compare values
from different fences. The first experiments do not need game-data copies and
must not arm the GPU-copy ledger at all.

J-143/J-144/J-145 closure rules remain intact: a game-list copy destination needs
successful native Reset to close the old recording, every relevant submission
accounted for, GPU completion and read consumption before retirement. ClearState
does not erase recorded commands; Stop/timeout is neither Reset nor completion.
Dropping a transaction's logical source lease does not drop J-142's permanent
upload reference (`l3-gpu-copy-design.md` sections 4–5; D-175;
`gpu_copy_ledger.cpp::read/retire`). ASSUMPTION: source facts are never promoted
to a draw-use, writer-exclusion or replay certificate just because XR submits.

Explicitly unproven after the present offline work:

- Actual headset availability, runtime/extension/format/blend-mode support,
  device/adapter compatibility and all native call latencies (D-178 is a dated
  configuration observation, not a fresh device test).
- Native image acquire/readiness/state/queue ownership, correct target access,
  GPU completion, release, compositor use and visible output.
- Native Signal/Map backend, from-Reset/restriction coverage, source draw use
  and writer exclusion; ledger fixtures supply these facts (D-175–D-179).
- Frame-to-simulation identity, native calibration, camera/culling, +0/+64/depth
  adaptation, both-eye replay and world/cockpit compatibility
  (`pose-contract.md`; `cockpit-openxr-layer.md`).
- Reachable safe XR teardown, no game-thread blocking, crash containment, stack
  headroom and pacing under load (`J-144-ledger-findings.md`; D-167/D-177).

## 4. Minimal gated LIVE experiment ladder — proposals only

No card is executed or approved here. ASSUMPTION: each step needs its own
reviewed exact build/config pin, offline fault checks, user acceptance of the
concrete card, and exclusive resources appropriate to it. Reserve GPU/runtime/
headset for (a); add game ownership for (b)–(d). Never automatically advance or
retry. Gates 1/3/6 where needed and gate 4 do not waive the staged L3/capture
limits (D-178/D-179). Missing tools/dependencies produce a refusal, not an
install. The current code is not ready to run these cards: implement/review the
bounded owner, failure latch and image-use witness first.

Common proposed controls (ASSUMPTION): the user starts SteamVR manually, confirms
headset readiness and stays present at the monitor. No proxy/harness auto-starts
or restarts it. An executor-owned stop button/console Escape sets the stop token;
the user can say "stop" for the executor to do the same. User removes the HMD
immediately on discomfort and manually stops SteamVR after the session returns;
removing the HMD is not assumed to terminate XR. Executor does not remotely kill
SteamVR or the game. All caps count from activation, including shouldRender=false
and failed frames; wall time wins over frame count. Startup 5 s and cleanup 2 s
are included in the absolute limits below. No armed session waits indefinitely
for the user to put the HMD on.

Common manifest/evidence (ASSUMPTION): card/run ID, build/config/source pins,
runtime identity/version reported by the probe, process/thread IDs, monotonic
call entry/exit times and XrResult/HRESULT, state events, abort reason, caps,
zero dropped witness records, resource/queue/fence epochs and pending-owner
disposition. Game cards additionally require original-call counts/results,
available baseline Present timings, game-health observations and authorized
cleanup/baseline record. No raw game resource read/capture is added to collect
these diagnostics. A missing Present seam is reported, not guessed from Execute.

### (a) Standalone OpenXR-only session probe, no game involvement

- **Proposal / bounds:** a new standalone harness, never loaded by the game,
  creates its own D3D12 device and DIRECT queue selected using
  `xrGetD3D12GraphicsRequirementsKHR`. User's SteamVR runtime only. Enumerate
  instance extensions, system, primary-stereo views/dimensions, reference spaces,
  blend modes and swapchain formats; refuse incompatible API/extension/device/
  queue/format/mode. No device/queue borrowed from a running game. One attempt,
  15 s absolute cap, 120 begun frames maximum. First at most 60 frames submit
  zero layers; only after that clean phase may the card's preapproved second
  phase use at most 60 dim solid-colour diagnostic frames. No game is running.
- **Exact evidence:** common manifest; required and selected adapter LUID and
  feature level, device/queue association; full enumerations and selection;
  instance/system/session/space create results and READY/STOPPING events;
  wait/begin/end balance and timings; zero layers in phase 1. Phase 2 needs
  both-eye acquire/index/wait/RTV/clear/state/submit/Signal/completion/release/
  layer witnesses from section 3, exact clear colour, and user's separate report
  of visible dim output if observed. Record clean owner retirement, destroyed
  handles and no pending GPU work; an empty layer alone proves no image use.
- **User actions:** start SteamVR, confirm readiness, keep HMD held off-face for
  lifecycle phase; if comfortable and asked within the cap, hold to eyes briefly
  to inspect the dim static field, or choose no visual inspection. No wearing
  is required for lifecycle success. Press Escape/say stop; remove HMD if needed;
  stop SteamVR manually after completion. No locomotion or gameplay.
- **Refuse/abort/stop:** common timeout/error/stop rules, unsupported enumeration,
  invalid views, failed state/fence/ownership join, unexpected brightness,
  runtime/device loss or discomfort. No second phase after any first-phase
  failure. A supervisor may terminate **only this harness process** after 2 s
  unresponsive grace within the 15 s cap; that is failure, not clean teardown.
- **Game protection / claim:** no game process, proxy, resources, files or hooks
  involved. Success proves only this standalone path on this runtime. It cannot
  prove device sharing, game safety or actual pixels from logs alone.

### (b) Proxy session lifecycle only; game rendering untouched

- **Prerequisite / proposal:** accept (a)'s evidence and a reviewed explicit
  outside-loader-lock owner/cleanup path first. One D3D12 binding against a live,
  independently owned game device/queue (never borrowed getter pointers used
  after their admission interval). One attempt; 10 s absolute cap, at most 100
  event batches. Create instance/system/session/space and enumerate compatibility;
  READY/begin-session and requested stop/STOPPING/end-session as legal. **No
  `xrWaitFrame`, frame begin, swapchain acquisition, clear/draw or XR layer** in
  this lifecycle-only card. Do not enable the broad dormant GPU switch.
- **Exact evidence:** common/game manifests, held device/queue ownership and
  epochs, graphics requirements checks, lifecycle/event results and durations,
  zero XR-frame/image/render counters, unchanged original Present/Execute
  arguments/counts/results where observed, no game output substitution, explicit
  stop/drain/teardown and post-run baseline. Lack of STOPPING/cleanup completion
  is a failed result, not permission to force destruction.
- **User actions:** start SteamVR, remain seated at monitor with HMD off-face or
  held; explicitly start this card. Stop via Escape/stop control/saying stop;
  close game normally if instructed after the diagnostic stops, then stop
  SteamVR manually. No HMD wearing or game input automation is needed.
- **Refuse/abort/stop:** no valid device ownership, missing graphics/event/cleanup
  contract, foreign call >250 ms, 5 s startup expiry, cap, any error/loss, output
  change, game stall or crash. No fallback adapter, runtime restart or retry.
  No acquired image exists, simplifying cleanup; a stuck runtime call still
  cannot be safely cancelled. Preserve owners and return game control when
  possible; never terminate a thread or join from a game callback.
- **Game protection / limit:** game calls/output untouched; worker performs XR
  work. In-process runtime faults can crash or stall the game despite this
  separation; refuse if that residual risk is unacceptable on the concrete card.

### (c) Head-pose-only readout; no rendering substitution

- **Prerequisite / proposal:** accept (b) and reviewed wait/abort handling.
  One attempt; 15 s absolute cap, at most 120 begun XR frames, zero layers
  throughout and no image acquisitions. Worker wait/begin/locate/end only;
  both views located at one predicted time. Log a true head-center pose from
  an explicitly reviewed VIEW-space location against LOCAL (or record unavailable
  and refuse head-center/recenter acceptance). Do not silently treat the current
  midpoint/left-eye approximation as contract compliance.
- **Exact evidence:** frame ID/time/period/shouldRender, session and space
  generations, valid/tracked flags, raw position/quaternion/FOV per eye and
  head-center pose/time/flags, reference-space-change events, one user recenter
  generation if requested. Log computed row chain/inverse/P with synthetic
  aircraft/seat explicitly labelled; compare to `pose-contract.md` for signs,
  normalized quaternion handling, named FOV order, one head/IPD application,
  retained tilt and fixed recenter transform. Report numerical residuals with
  proposed 1e-5 arithmetic tolerance, not a human calibration tolerance. User
  movement reports are qualitative, not measured centimetres/degrees. Evidence
  must also show zero image/render counters and unchanged game output.
- **User actions:** start SteamVR, sit still, wear or hold HMD to eyes briefly;
  make small comfortable left/right look and lean movements, optionally request
  one recenter; no flight control or forceful motion. Stop at any point through
  the common control, remove HMD and stop SteamVR manually after cleanup.
- **Refuse/abort/stop:** invalid/missing positional or orientation tracking,
  nonfinite math, space-generation mismatch/change, unqualified head-center,
  wrong eye/time association, any wait watchdog/cap/error, discomfort or game
  disturbance. No stale pose/identity fallback or repeated recenter attempts.
- **Game protection / limit:** readout consumes only XR poses and CPU math;
  no camera writes, SrtData changes, game resource reads, GPU-copy arming or
  replay. Physical native scale/seat mapping is still unproven even on success.

### (d) First diagnostic stereo output, synthetic scene only

- **Prerequisite / proposal:** accept (a)–(c) and a reviewed native image-use
  witness/private GPU owner. One attempt; 12 s absolute cap, at most 60 begun
  frames; same worker/bounds. Render a dim stationary synthetic target with
  left/right identifiers and near/far markers into separate XR images using
  each real view pose/FOV and the same immutable synthetic scene ID. This
  demonstrates the stereo output mechanism before any game-content replay.
  No game-buffer capture or desktop-image duplication as a stereo claim.
- **Exact evidence:** full section-3 two-eye witness, private allocation budget,
  state/RTV/image/eye association, synthetic geometry/content hash, both matrix
  sets and raw submitted poses/FOV, common scene/sample ID, completion fence
  values/results, balanced release/end/retirement, CPU call latencies and
  available GPU timings. User reports eye labels, obvious reversal, stability
  and discomfort separately from mechanical evidence. No screenshot/capture
  attachment is enabled to obtain this report. No claim of pixel correctness
  solely from a completed fence.
- **User actions:** start SteamVR, remain seated, initially inspect off-face;
  wear/hold briefly only if comfortable, identify left/right markers and make
  small head movements. Stop immediately for discomfort/incorrect eye display;
  use common stop control, remove HMD, then manually stop SteamVR after cleanup.
- **Refuse/abort/stop:** either eye missing, same image misassociated, inconsistent
  sample/space/time, invalid state/format, incomplete fence/release, timing
  watchdog, unexpected full-bright/unstable output, eye reversal, device/runtime
  loss, discomfort or game disturbance. Never submit one successful eye alone,
  replay a stale pair or continue to diagnose visually after a stop.
- **Game protection / limit:** game targets, commands, Present and simulation
  unchanged; only private XR rendering. GPU contention remains possible on the
  shared device. Success is synthetic XR stereo only. A later cockpit-content
  experiment requires accepted L3 ordered-copy/private-replay evidence, native
  +0/+64/depth mapping, simulation identity, calibrated pose and culling, plus a
  new reviewed card. These are not authorized by success at (d).

## 5. Risk table

Every likelihood/impact rating here is an **ASSUMPTION**, not measured frequency
or severity on this machine. Detections and mitigations are proposed too.

| Risk / evidence basis | Likelihood / impact (ASSUMPTION) | Mitigation | Detection / stop |
| --- | --- | --- | --- |
| GPU resource contention with game device; no native XR timing (D-178) | Medium / high | Start standalone; lifecycle/pose before rendering; one private pair in flight; charge reviewed allocation cap, no recycling on uncertainty. | Allocation/device errors, fence deadline, game health/available Present baseline regression; stop on any >250 ms observed stall or user-reported disturbance. |
| Double-present or multiple frame owners (`dllmain.cpp` explicitly lacks a frame owner) | Medium / high | Game alone owns original Present; one XR loop, separate serials, no Execute-as-frame. | Duplicate begin/end, altered original-call count/arguments, unmatched frame serial => disable. |
| Frame pacing and blocking (`openxr_session.cpp` has infinite image waits) | High until repaired / high | Worker-only calls, finite image waits, mailbox drops, watchdog; refuse current implementation. | Call entry without exit, >250 ms progress watchdog, measured timing/counter gaps => stop admission. |
| Comfort, eye reversal, inconsistent cockpit/world; J-007 risk analysis | Medium / high | Seated dim synthetic output, short caps, both-eye/sample checks; no game world substitution. | User discomfort, eye labels wrong, unstable field => immediate stop/removal; no comfort claim from logs. |
| Device lost / TDR, shared-device consequences (unmeasured) | Low / critical | No game queue waits/state edits; conservative fence checks, no recovery/recreation attempt. | Device removal HRESULT or UINT64_MAX => invalidate epoch, quarantine; game survival cannot be guaranteed. |
| Runtime crash/hang in game process; no isolation proof | Unknown / critical | Standalone first; explicit residual-risk card; no thread kill or unsafe unload; refuse if isolation required. | Missing worker progress, runtime error/loss, process exit; distinguish clean disable from process failure. |
| Acquired image released early / wrong target (`markEyeRendered` trusts caller) | Medium / high | Authenticated per-image record, bounded successful wait, exact RTV/command/fence joins before release. | Missing/mismatched epoch/index/state or failed completion/release => no layer, preserve owner. |
| Cleanup use-after-free / loader lock (D-176/D-177) | Medium / critical | Outside-loader-lock owner; combined copy/XR drain veto; late results retained; no timeout-based free. | Pending frames/owners/calls, reentrant cleanup, failed drain => retain, report incomplete teardown. |
| Source-version conflation (D-173; J-143) | High if unchecked / high | Keep sampled bytes, ordered-copy result, replay eligibility and XR completion distinct. | Missing immutable packet/manifest/dependency => refuse game-content rendering. |
| Pose-space/recenter generation or head applied twice (`pose-contract.md`) | Medium / high | One predicted sample; raw X poses submitted; explicit space generation/head-center; both-eye atomic packet. | Invalid flags, mismatch, wrong residual/sign/IPD checks => zero layers/stop card. |
| Stack/retained memory/log overhead (D-167 incomplete screening; J-144 caps) | Unknown / high | Small fixed game-thread handoff, bounded logging, no per-draw XR work, remeasure changed hook paths before live use. | Screening failure, lost witness records, exhausted allocation/counter budget => refuse/stop, never replenish automatically. |

## 6. Non-goals, unknowns and review handoff

No capture enablement beyond the separately staged L3 comparison; no RenderDoc
or debugger attachment, install, driver change, runtime setting/account change,
network call, tracker post, deployment, original-game-binary change or game-file
change is part of this job. Future proxy staging/removal is executor-owned and
must be explicitly described in its authorized card; this document performs
none. No world-camera hook, native stereo replay, depth reprojection, HUD/world
composition, simulation patch, performance/comfort guarantee or runtime parity
claim is delivered. All source/tests/CMake/docs/evidence stay unchanged.

Unknowns requiring discriminating checks, in order (ASSUMPTION for proposals):

1. API correctness of bounded abort, release-state transitions, blend-mode and
   session-stop paths: offline SDK review/fault fixtures before any live card.
2. Whether SteamVR/device supports the enumerated graphics path and returns
   calls/teardown promptly: standalone (a), including failure disposition.
3. Whether an explicit owned in-proxy lifecycle can stop safely without waiting
   on game threads: reviewed owner design, then (b); no guaranteed crash isolation.
4. Head-center/reference-space generation and contract fidelity: offline adapter
   checks, then (c); native calibration is a separate unresolved measurement.
5. Real image target use, GPU ordering, release and visible diagnostic stereo:
   reviewed native witness, then (d). User observation supplements call logs.
6. Game Present/simulation correspondence, native camera/units/screen/depth,
   source coherence, complete L3 native restrictions/closure and private replay:
   further authorized work; none follows from this ladder's synthetic output.
7. Real latency, frame pacing, GPU residency, stack headroom, mission transitions
   and comfort under load: unmeasured; no shipping readiness from short probes.

Handoff: J-146 owns only this file. Source/test inventory and proposal audit
performed offline. The required standalone `node scripts/verify-offline.mjs`
exited 1 at Python syntax subprocess creation:
`spawnSync %USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
No native compilation/CTest ran; the full gate remains an executor requirement.
The status wrapper
failed because this isolated checkout has no `.mcp.json`; no configuration was
changed. Pre-existing untracked `mcp.launch.json` is unrelated and untouched.
Executor owns pinning and integration; independent Claude review and Astra's
design questions remain. Next executor command: `node scripts/verify-offline.mjs`
in its permitted offline environment. No live action or commit occurred.

### Questions for Astra to review first

1. Is the worker-owned XR loop with unchanged game Present/Execute and no claimed
   simulation-frame identity the right boundary for this stage?
2. Are the 0/1 ms image waits, 250 ms progress watchdog and 2 s retained-owner
   abort disposition correct, including the explicit inability to cancel
   `xrWaitFrame`; should in-proxy probes remain refused until process isolation?
3. What exact D3D12 boundary state, queue binding and legal abort sequence must
   the local OpenXR SDK confirm for an acquired-but-unready or GPU-pending image?
4. Does the proposed per-image command/fence/release witness justify its narrow
   completion claim, and what additional evidence is needed for compositor use?
5. Is a separate XR ledger with a joint cleanup veto sufficient alongside the
   J-145 late-result ownership contract, without changing copy retirement rules?
6. Must head-center VIEW-space sampling/reference-space generation handling
   replace the existing midpoint/left-eye recenter approximation before (c)?
7. Are the standalone, lifecycle-only, pose-only and synthetic-stereo cards
   sufficiently separated and short, with adequate user stop/protection rules?
8. Which unresolved L3/native-camera/simulation/clip-depth prerequisites must
   block the first later cockpit-content stereo card, even after (d) succeeds?
