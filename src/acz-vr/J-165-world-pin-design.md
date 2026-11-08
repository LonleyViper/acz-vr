# J-165: one world-texture pin and recording retirement

## Verdict and limits

**A source-feasible ownership protocol exists, but it is not implemented or
proven on the real game's call sequence.** Implement the two stages below on
J-164 (`c5b8669`). Keep its fail-closed predicate until both stages are complete.
Do not merge J-164 alone as restoration of world snapshots. My recommendation
is to hold it for the combined implementation, consistent with the job's
reported decisions 39/40 option B. This is a recommendation, not a new user
decision or authorization to merge.

The supported route waits for a live selected-resource argument and a fresh RTV
publication tied to the acquired reference. If those events never occur after
selection, world copying remains refused. In particular, the reported accepted
world snapshots at `743a9cd` and `fbbc733` do not prove repeated RTV creation.
An observed exact-ID3D12Resource device-creation output for the current
incarnation is also mandatory: a render target created before hooks were
installed remains refused, even with later live RTV/barrier arguments.
Preserving acceptance for every historical call sequence is not established.
Do not resolve that uncertainty by AddRef'ing the pointer saved by step5Arm.

This design prevents source-object destruction under conforming COM/D3D12
caller lifetime and synchronization contracts. It does not prove texture-content
coherence, backing-heap residency/lifetime for placed resources, correct state
transitions, stereo correctness, or runtime parity. Existing world eligibility,
state, heap and recording restrictions remain mandatory; an ownership pin must
not relax them.

## Source and evidence inspected

Line references below are the worker baseline `ed5e357` unless explicitly marked
`c5b8669`. Sources are under `src/acz-vr/`. Read-only inspection covered:

- `d3d12_hook.cpp`; relevant `gpu_copy_step1/3/5_hooks.inl` sections;
  `gpu_copy_witness.h/.cpp`; J-143 GPU-copy design; J-148 findings, whose actual
  filename is `J-182-witness-findings.md` (heading identifies J-148/D-182).
- J-113/J-115/J-134/J-141 ownership findings and the relevant live acquisition,
  pin-query and release/drain source cited below.
- J-160 findings at `8cdfb52`, J-164 findings/source at `c5b8669`, and J-164's
  independent Claude review via the local board. Neither branch was altered.
- STATUS.md, DECISIONS.md and the coordination guide. The claim's summaries of
  issue 29 comment 6369523201, decisions 39/40 and the two live runs are reported
  context. The full external comment, separate decision records and original
  live-run artifacts were not independently fetched or verified. No additional
  registered evidence ID was opened; no network access was used.

J-164's review correctly identifies the current blockers: historical selection,
the copy baked into a game recording, and no reachable production global
shutdown caller. Its synthetic held-pin certificate supplies no production
retirement evidence. This design adds the missing state and observation feeds.

## 1. Live acquisition and the selection join

| Existing seam | Live resource pointer? | Proposed use and limitation |
| --- | --- | --- |
| Successful exact-ID3D12Resource creation output, `d3d12_hook.cpp:2560-2575`; committed/placed/reserved adapters around 3755/3775/3801 and compatibility creation at 4074 | Yes, before returning the caller's output | Mandatory provenance source: the `l3Created` `record_creation` path records a distinct device-created flag and creation serial for this incarnation. Acquire only if a selected request already exists. Initial creation usually precedes selection; do not pin all candidate RTs. Unsupported creation interfaces remain unsupported. |
| CreateRenderTargetView, device slot 20, `unsupportedView` at 3922-3933 and `step3NoteView` at 3888-3907 | Yes, its actual `resource` argument throughout the intercepted call | Preferred later acquisition: confirms an already observed device-created incarnation; cannot establish that provenance itself. Selected request matches, slot empty, supported non-depth view; acquire outside locks while the caller still owns the live argument. Publish the descriptor only after pin publication succeeds. |
| OMSetRenderTargets, `step3SetRenderTargets` at 5027-5047 | No resource interface argument; only handles | Match descriptor publication and bind serials. Never AddRef the resource recovered from this map. A private descriptor copy does not retain its resource. |
| Legacy ResourceBarrier, `l3Barriers` at 4978-4990; transition matching at 5081-5100 | Yes, `Transition.pResource` in the game's actual array | Additional later acquisition opportunity confirming an already observed device-created incarnation at the selected address; cannot establish that provenance itself. Add a pre-observation, outside-lock acquisition leaf before the existing locked barrier loop. Do not acquire from a saved barrier or from null/aliasing/UAV guesses. |
| step5Arm at `gpu_copy_step5_hooks.inl:775-803`, source selection at 686-708/749-756 | No; saved shape and scalar address | Publish a desired-selection request only. No COM dispatch. |

ResourceBarrier is an existing argument source overlooked by the earlier
escape-path findings. Its presence is not proof that its pointer is the same
incarnation as an old RTV entry. Likewise a live later RTV argument at a reused
address does not revive an earlier selection certificate.

Use a two-phase selection: `requested` then `certified`. A request contains a
monotonic selection epoch, expected address, RTV handle/write serial, device,
and expected shape. It is only a filter, never ownership evidence. Reserve the
single slot under the hook-state mutex, capture the request revision and native
Release dispatch, unlock, AddRef through the LIVE argument's own interface,
then relock and revalidate the reservation/request/publication revision.
No hook-state or step-1 mutex spans COM. A leaf called from the existing locked
step5NoteBarrier_locked is therefore insufficient; acquisition precedes it.

Every acquisition gets a new nonzero pin serial, including later-than-creation
acquisition. Require a separate device-created flag and creation serial from
the observed `l3Created` `record_creation` path for an exact ID3D12Resource
output of this current incarnation. An RTV/`step3NoteView` lifetime record is
insufficient; never invent creation provenance from it or the buffer-only L3
store. Before any AddRef, refuse swap-chain GetBuffer outputs and all otherwise
unobserved incarnations with `world-pin-incarnation-not-device-created`.
No GetBuffer, QI or swap-chain hooks are added to classify them. Creation/reuse
invalidation must clear old provenance; address/table equality cannot transfer
the device-created flag or serial to a later incarnation.
At acquisition invalidate earlier
view-to-incarnation links, watch serial, carried state and saved positive shapes
for that address. The pin serial is the new ownership epoch. A barrier/creation
acquisition alone waits for a **subsequent live RTV write**; an acquisition inside
the RTV call can stamp that call's own completed write. Stamp descriptor handle,
write serial, pin serial, exact interface address and device in one publication.
Do not restamp old descriptors from pointer equality. Descriptor copies stay
conservatively unproved unless their complete source publication is tracked;
the initial implementation should refuse them for this join.

After that publication, a fresh OM bind and fresh qualified shape must match the
same descriptor write, address, pin serial and selection epoch. Revalidate shape
and supported subresource/format/dimensions rather than relabelling the saved
pre-pin shape. step5Arm may certify this new selection without another AddRef.
The held reference then bridges RTV publication through selection and every use.
This proves the *newly qualified* selection is the acquired object; it makes no
claim that an unpinned historical object survived until acquisition. Old shapes
remain refused even when address and vtable match.

No repeated RTV is guaranteed by this source inspection. The next discriminating
runtime evidence, only in a separately authorized campaign, is the ordering of
selection request, live acquisition, fresh RTV write, OM bind and first qualified
draw, with their serials. If it is absent in the real game, this strict route
does not restore that game's snapshot; an earlier authorized selection source
would then be needed. Do not expand to speculative candidate pins.
The same refusal applies if the render target's creation predates installed
hooks: later RTV/barrier observations cannot supply the missing device-creation
record, even if fresh RTV publication occurs.

## 2. One-slot ownership and faults

Use the J-141 reserve/AddRef/publish pattern (`d3d12_hook.cpp:2608-2649`) and
J-134 cancellation ordering, with one fixed slot instead of a candidate ring.
States are empty, acquiring, held, retiring, releasing, quarantined. Acquiring,
releasing and unknown-completion states all occupy the sole capacity; no second
reference may be acquired while any of them exists. Repeated observations of
the same certified selection perform no AddRef. Monotonic serial overflow
refuses; it never wraps into an old identity.

Record the exact address, observed table, mandatory device-created flag and
creation serial for the current incarnation, pin serial,
selection revision, positive AddRef outcome, saved native Release function,
active acquisition/use counts and recording obligations. The lifetime query
follows `pinnedPipeline_locked`/`pinnedRoot_locked` (2403-2415): scalar/indexed
checks of a complete held record, not a fresh COM probe.

Distinct refusals must include `world-pin-capacity-exhausted`,
`world-pin-incarnation-not-device-created` (zero AddRef),
`world-pin-addref-zero`, `world-pin-addref-exception`,
`world-pin-addref-fault`, `world-pin-publication-changed`,
`world-pin-serial-exhausted`, `world-pin-descriptor-link-unproven` and
`world-recording-capacity-exhausted`. Preserve the outer identity-unproven reason
when no certificate exists, with the pin refusal recorded as its diagnostic
cause. Test both levels. Catching C++ exceptions does not catch Windows access
violations: a tiny SEH acquisition leaf, without destructible locals, is required
for the fault classification (compare the separate SEH leaf at 2417-2423).
A hung COM call has no bounded recovery guarantee.

Zero/exception/fault grants no certificate. Unknown AddRef completion quarantines
the slot permanently, with no retry or speculative Release. A known positive
AddRef whose publication was cancelled has never authorized insertion: detach
and perform one saved-native Release outside locks, retaining the slot through
that call. If Release faults/throws, completion is unknown: no retry and no slot
reuse. This exception to balanced cleanup is explicit, not an exactly-once
success claim. Known success follows one-acquisition/one-release accounting.

Hold the reference continuously until retirement. A creation at an occupied
address, changed table or inconsistent incarnation revokes admission and
quarantines; a live pin should make actual destruction/address reuse impossible
under the stated COM premise. Never overwrite an occupied slot. Once safely
released, a later live acquisition at the same address receives a fresh serial;
all old descriptors, shapes and recording keys still fail.

## 3. Recording obligations, submission and GPU completion

The world copy is inserted in the GAME list at the first member
(`gpu_copy_step5_hooks.inl:51-56`). Ring slot completion is not recording closure.
Current `step5NoteReset` (591-601) and `step5BeforeExecute` (604-650) observe
Reset/re-execution, but `lastList/lastGen/lastLive` remember only one published
recording. They are insufficient for source ownership over multiple lists and
older executable generations. The existing Witness is also one transaction per
list; do not claim it already implements this continuous ledger.

Add a fixed, heap-resident table of 128 recording obligations. This bounds
bookkeeping, not GPU resource memory. Reserve before the FIRST possible injected
barrier/copy command; record `(list lifetime token, Reset generation, pin serial,
device/queue/fence epoch)` and mark possible insertion before the void native
call. No capacity means skip world commands. Keep obligations for partial,
failed, unpublished and quarantined frames, independent of ring-slot reuse.
Reuse a table entry only after its old recording is discarded, all joined work
is complete and no native interval/poll owns it; use a new monotonic entry token.

Required transitions:

1. Insertion admission validates ownership and coverage atomically with its
   recording reservation. Hold a short logical CPU use lease across unlocked
   recording; selection/drain cannot detach the pin meanwhile. Retain the
   obligation after any possible command, even if later recording fails.
2. Successful native Close marks that generation executable. Close does not
   discard it. Failed Close, observation overlap or missing identity quarantines.
3. Before forwarding EVERY Execute batch, snapshot every matching obligation's
   actual list lifetime/generation, occurrences and queue. Reserve native-in-flight
   tickets before native submission, with no mutex held during forwarding. The
   lifetime feed runs while any obligation/pin exists, including after producer
   disable, reselection or phase change; current phase-gated calls at
   `d3d12_hook.cpp:2084-2092` must not hide it.
4. For a supported first execution, join the existing post-batch private Signal
   (`step5AfterExecute`, 652-657) to those exact tickets and fence epoch. One batch
   watermark can cover all its obligations. Native Execute is forwarded exactly
   once with original arguments. Signal success is not completion.
5. Any repeated execution, duplicate batch occurrence, foreign queue, oversize
   uninspectable batch, concurrent/reentrant submission, missing seam or failed
   Signal quarantines the pin and affected obligations BEFORE forwarding. Still
   forward the game call. **Never release that pin in this process.** Do not
   clear quarantine after a later Reset/fence. This deliberately bounded policy
   avoids inventing completion evidence for unsupported re-executions and needs
   no additional Signal for them. It also keeps source ownership when publication
   logic disables itself or forgets a re-executed frame.
6. Only an observed successful native Reset of the exact list lifetime discards
   the old recording. Keep its GPU ticket until same-epoch completed value covers
   its watermark. Reset before first submission permits retirement only with
   zero outstanding native submission reservations and complete observation.
   Reset racing a pending Execute/Signal join quarantines. Failed Reset does not
   close anything; ClearState is never Reset.
7. Poll the already-owned fence off hook locks. Check device removal/UINT64_MAX,
   regression, epoch and overflow before comparison. A completed first execution
   with no Reset remains an executable obligation indefinitely. A Reset with a
   pending fence remains a GPU obligation. Timeout is not retirement. Fence
   rollover cannot transfer an old obligation to the new epoch: quarantine and
   retain if its completion source is lost.

For a normal record, retirement requires `discarded && native_inflight==0 &&
(never_submitted || authenticated_same_epoch_completion) && !ambiguous`.
The pin releases only when ALL its obligations satisfy retirement, no CPU
use/acquisition is active, and admission for that pin is closed. No slot status,
published-frame count, consumer release or elapsed deadline substitutes for this
conjunction. Existing buffer-ledger ordering is the model: quarantine retains
owners; publish retirement/detach before foreign Release.

Witness reference points: `gpu_copy_witness.cpp:46-69` successful Reset and old
tombstone closure; 137-170 pre-Execute ticket/post-Signal join; header coverage
bits and from-Reset rules; J-148 and J-143's closure-before-release contract.
Reuse these invariants, not the single-shot ledger object for every frame.

Observable today: native Reset/Close results, actual Execute arrays, installed
list alias routes, private Signal result/epoch and nonblocking fence samples.
New work: full per-recording source obligations, pre-forward lifetime tickets,
coverage attestation for every admitted Reset/Close/Execute route, fresh
descriptor-to-pin linkage, and a drain admission barrier. Coverage must be
positively established from existing authenticated list/queue registration;
unknown aliases/queues refuse insertion, not infer coverage from no events.
No new QI/alias-discovery calls are permitted. Unproven route coverage keeps
the certificate false. Resource final Release is not a replacement for Reset
closure: use quarantine if list retirement prevents proving discard.

## 4. Selection, teardown and shutdown

Changing selection A to B first revokes A's certificate/watch/carried facts and
closes A's insertion admission. Keep A in the sole slot while its recording/CPU
obligations drain. B is a pending request with **zero references**. Its live RTV
or creation/barrier opportunities while A occupies capacity refuse; after A is
safely released, B must obtain another live argument and fresh descriptor join.
There is no simultaneous old-pin deferred queue plus new pin. Returning to A
does not erase retirement or revive an old certificate.

Use one deferred-release record, the same slot. Existing step-1 owner ticks may
poll completion and detach an eligible retiring reference. Under mutex publish
certificate revocation and `releasing`, clear owned pointer exposure, then call
the saved native Release outside every mutex. Reentry sees an occupied slot;
publish empty only after known completion. Route through the existing owned
release convention at `d3d12_hook.cpp:1398-1413`, retaining the exact native
dispatch captured at acquisition rather than relying on a patched current slot.
No new general COM dispatch framework is needed.

`shutdownDiagnosticOwnership` (2232-2287) has no established production caller;
its global drain and `step1Quiescent` are not world-recording retirement proof.
The continuous ring is deliberately process-lifetime (step5:71-72).
Do not add the world pin to its unconditional detach arrays without the new
guard, or run foreign COM from DllMain.

Add a world-specific orderly-drain attempt to the existing owner-thread exit
path (`step1Thread`, step1:1149-1151), before it returns, and to normal retiring
ticks. It closes world admission and releases only if the above conjunction
already holds; it must not wait for the game to Reset a list. Also make the
global shutdown helper consult the same world guard before draining device/
queue dependencies. This supplies a real world-specific caller without claiming
the pre-existing global ownership shutdown is now reachable. If the owner exits
with pending/executable/ambiguous work, retain the reference and dependency
owners for process lifetime. Hooks continue conservative lifetime observation;
no background retry thread or loader-lock drain is added.

Recommendation: bounded deferred release while the owner exists, then a
process-exit leak for unresolved ownership. One retained game texture can retain
substantial native memory; this bounds references, not residency. It preserves
safety at the cost of later selection availability. It is preferable to releasing
against an unknown GPU/recording lifetime. Proven orderly teardown releases the
known reference once; repeated calls dispatch zero further Releases. Process
termination dispatches zero Releases. Guaranteed leak-free arbitrary shutdown
is not offered by these seams.

## 5. Certificate and J-164 integration

Replace `step5CanonicalLifetimeProven_locked` at `c5b8669` step3:72-77 with a
side-effect-free query of the production record. Require held/complete positive
acquisition for the exact address, table and current pin serial; certified fresh
descriptor/selection linkage; non-revoked watch serial; valid recording coverage;
and no acquisition/release/native-observation ambiguity. For insertion, also
reserve the exact recording obligation and CPU use lease before commands.
Make the queried serial explicit in the internal interface or compare the full
current watch/selection tuple under the same mutex; an address-only positive
cache is forbidden. Completion need not already hold while the pin is held:
the certificate establishes that retirement cannot release it prematurely.

Keep J-164's pure lifetime/coverage queries and explicit observation invalidation
(step3:87-142 at `c5b8669`). Publish/revoke certificates only at acquisition,
selection, descriptor, coverage, fault and drain transitions. Preserve its
`step5WorldWhy` current-lifetime revalidation even for saved shapes, adding the
exact serial comparison. Missing pin yields
`world-texture-final-release-identity-unproven`; structural uncovered reasons
remain distinct. Never turn a test callback or plain ID3D12Resource IID into the
production proof.

Move the positive plain/held-alias cases onto the real production publisher and
predicate, with fake native COM/fence dispatch only. Remove their dependency on
`g_step5CanonicalLifetimeTest`. Keep no-pin aliases as negative cases and J-164's
query purity assertion. Held shared-count releases are 2 then 1 after one proxy
AddRef; 1 then 0 belongs to the no-pin case. Static distinct alias object/table
storage remains. An unmapped/unpatched alias cannot destroy the continuously
held base interface under the stated COM contract; no alias mapping is inferred.

## 6. Added game-visible calls

This document adds none. Proposed implementation effects are:

- Exactly one AddRef per successful pin cycle (or one attempted AddRef per
  faulted cycle), only through a selected LIVE creation output, RTV argument or
  transition-barrier argument. No AddRef at step5Arm, OM lookup, Draw or query.
- Exactly one saved-native Release for each known acquired reference after safe
  retirement, including cancelled publication before any insertion. Selection,
  teardown and orderly shutdown share that single ownership transfer; they do
  not each Release. Unknown completion/process exit can intentionally leave it
  retained, as specified above. Refcounts and destruction timing are game-visible.
- Pinning a DXGI swap-chain back buffer would make the game's ResizeBuffers /
  ResizeBuffers1 fail with `DXGI_ERROR_INVALID_CALL` while the extra reference
  remains, potentially for process lifetime under quarantine. Mandatory observed
  device-creation provenance excludes GetBuffer/unobserved incarnations before
  AddRef and avoids this failure; RTV/barrier observations alone never qualify.
  For eligible device-created render targets recreated on mode change, deferred
  destruction and VRAM retention remain game-visible costs until safe release,
  or process exit for a quarantined pin.
- Zero new QueryInterface calls; zero broad QI hooks. J-115's extra QI technique
  is not adopted. Existing unrelated QI calls in Execute discovery are unchanged.
- Zero extra queue Signal calls: reuse the existing per-supported-frame Signal;
  unsupported repeats quarantine permanently. Existing fence polling continues
  off the game thread. No new resource GetDesc/GetHeapProperties calls: reuse
  existing RTV observations. These and current copy/barrier calls remain visible
  costs; the design adds no other native game/device/queue calls.

## 7. Named fixture plan

The names below identify required cases; the implementation may group protocol
cases in one CTest executable. Assertions include exact refusal cause, ownership
counts and zero unexpected native calls, not just a Boolean certificate.

| Case | Required result |
| --- | --- |
| `world-pin-live-rtv` / `world-pin-live-creation` / `world-pin-live-barrier` | Require current device-creation provenance; acquire only inside the fake live-call interval; creation/barrier wait for fresh RTV; bind/fresh shape joins exact pin serial. |
| `world-pin-unobserved-incarnation` | Selected address with a live RTV/barrier argument but no current device-creation record (including GetBuffer-like, pre-hook and reused-address cases) performs zero AddRef and refuses with `world-pin-incarnation-not-device-created`; an RTV lifetime record or stale creation serial does not qualify. |
| `world-pin-no-fresh-view` / `world-pin-historical-om` | No stale-descriptor promotion or historical AddRef; copied-command count remains zero. |
| `world-pin-addref-zero` / `world-pin-addref-exception` / `world-pin-addref-fault` | Distinct cause, no proof, no speculative Release/retry; unknown completion consumes capacity. |
| `world-pin-exhaustion` / `world-recording-exhaustion` | Second reference or 129th live obligation refused before native acquisition/insertion. |
| `world-pin-address-reuse` | Same address/table with fresh serial rejects every old shape/view/record; inconsistent reuse while held quarantines. |
| `world-pin-selection-change` | B remains unpinned until A discarded AND complete; A releases once; B needs new live acquisition. |
| `world-pin-cancel-publication` / `world-pin-release-reentry` | Known cancelled acquisition balances once; reentry cannot acquire during Release; faulted Release never retries. |
| `world-pin-recording-pending` / `world-pin-gpu-pending` | Fence-before-Reset and Reset-before-fence each retain; both completed conditions permit release exactly once. |
| `world-pin-two-lists` / `world-pin-old-generation` | Older closed recording is retained after another frame/list publishes; forgetting lastLive cannot permit release. |
| `world-pin-reexecute` / `world-pin-duplicate` / `world-pin-cross-queue` | Native Execute still forwarded once; permanent pin quarantine; later Reset/completion does not release; zero added Signals. |
| `world-pin-reset-join-race` / `world-pin-signal-failure` / `world-pin-epoch-loss` | Native-in-flight/unknown completion prevents release, including after producer disable. |
| `world-pin-shutdown-once` / `world-pin-shutdown-busy` / `world-pin-process-exit` | Proven world drain releases once; executable/pending work retains; termination performs no COM dispatch. |
| Existing `step5-world-plain-proven` | Held production record authorizes/copies without test certificate override; revoke and old-serial variants refuse. |
| Existing `step5-world-alias-unpatched`, `step5-world-alias-patched-unmapped`, and their `-no-pin` variants | Production held-pin path survives shared-count releases 2/1; no-pin 1/0 refuses and invalidates watch; both alias tables distinct/static. |
| `world-pin-query-purity` / `world-pin-coverage-gap` | Repeated queries preserve state; explicit coverage loss revokes and prevents new insertion. |

Also rerun all existing step5-world cases after adapting their setup to real pin
publication. Do not replace their entry-state/reselection/Release assertions
with a blanket ownership-positive callback. Synthetic tests establish protocol
logic only; they do not establish the game's event sequence or GPU behavior.

## 8. At most two implementation jobs, disjoint writes

Both stages build on J-164. Execute sequentially; stage B depends on reviewed,
integrated A. Neither may claim restoration on the real game without separately
authorized evidence. Stage A alone must not enable any world-copy path.

**A — bounded ownership/recording protocol and scalar fixtures.**
`scope_paths`: `src/acz-vr/world_pin_protocol.h`,
`tests/acz-vr/world_pin_protocol_offline_tests.cpp`,
`src/acz-vr/CMakeLists.txt`, `src/acz-vr/J-world-pin-protocol-findings.md`.
Use the actual assigned job id for the findings filename. Define the single-slot
state machine, monotonic keys, 128 obligations, detached release tickets and
coverage/admission API in the header. Native dispatch is supplied by the adapter;
the model makes no COM calls. Register its new standalone CTest in CMake.
Acceptance: all protocol/fault/race/table cases above that need no installed
hook run through this API; demonstrate that no sequence retires a pin while any
executable, GPU-pending, native-in-flight or quarantined obligation exists.
Include `world-pin-unobserved-incarnation`: missing current device-created
flag/serial refuses with `world-pin-incarnation-not-device-created` and grants
zero AddRef dispatch authorization, including stale provenance after reuse.
Native executor build/new CTest plus offline gate; author reports unavailable
checks accurately. Include a small independent event-order matrix, not tests
that merely echo each setter.

**B — production observation, live acquisition and world-copy integration.**
`scope_paths`: `src/acz-vr/d3d12_hook.cpp`,
`src/acz-vr/gpu_copy_step1_hooks.inl`,
`src/acz-vr/gpu_copy_step3_hooks.inl`,
`src/acz-vr/gpu_copy_step5_hooks.inl`,
`tests/acz-vr/gpu_copy_step1_offline_tests.cpp`,
`src/acz-vr/J-world-pin-integration-findings.md` (actual job id).
Consume A's API unchanged; integrate pre-lock live acquisition, serial-stamped
views/fresh selection, phase-independent lifetime feeds, pre-command reservation,
existing Signal joins and world-specific owner-thread drain. Add no CMake edits:
extend the already registered plain/alias/world cases with named subcases and
invoke additional integrated scenarios through those existing dispatcher entries.
No witness-library rewrite, XR code/window changes or runtime tooling edits.
Acceptance: installed fake hook calls prove live-interval AddRef, zero historical
dispatch/QI/extra Signal, production-predicate plain acceptance, all four alias
variants, refusal diagnostics, multiple-list/old-generation retirement, disable/
reselect races and the actual owner-exit call to the guarded drain. Independently
exercise `world-pin-unobserved-incarnation` through live RTV and barrier hooks:
no current exact-ID3D12Resource device-creation record means zero native AddRef
and `world-pin-incarnation-not-device-created`, including pre-hook targets,
GetBuffer-like pointers and stale records at reused addresses. Independently
audit complete admitted Reset/Close/Execute alias coverage; unavailable coverage
must refuse. Executor builds and runs A's CTest, all step5-world cases and full
offline gate. If the native adapters cannot establish a required fact, keep the
predicate false and report the exact seam; do not silently loosen A's API.

## Verification of this design-only delivery

- `node scripts/verify-offline.mjs`: RUN once as the exact standalone command;
  FAILED exit 1 in Python syntax subprocess startup:
  `spawnSync %USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
  The remaining gate, including any native stages, did NOT run.
- Native build: NOT RUN; the initial delivery's `Get-Command cmake,ctest,cl`
  found none on PATH. Tool discovery was not repeated for this amendment.
- CTest/C++ fixtures and the proposed fixture plan: NOT RUN. No implementation
  or test source was changed by this job.
- Programme status: attempted with the required standalone `pwsh -NoProfile
  -File tools/jobboard.ps1 multi status`; FAILED because worktree `.mcp.json`
  is absent. Local MCP claim/heartbeat succeeded. No controller was started.
- Source/design audit (initial delivery): live arguments versus historical handles, saved-native
  cleanup, all-recording closure and fence ordering, J-164 pure query/alias
  constraints and disjoint future write scopes inspected. This is source
  reasoning, not an executed native/runtime check.
- Amendment source audit: reread `l3Created` at 2560-2575 and
  `step3NoteView`/`unsupportedView` at 3888-3933; searched local C++/header/inl
  sources for GetBuffer, ResizeBuffers and record_creation. No swap-chain
  GetBuffer/ResizeBuffers interception was found. No new evidence inputs opened.
- Amendment verification: the standalone offline gate was rerun once with the
  same Python EPERM failure above; programme status was retried with the same
  missing `.mcp.json` result. Native build, CTest and proposed fixtures were
  NOT RUN. `git diff --check`: PASS. `git diff` inspected; only this document
  changed, adding the requested device-creation restriction, resize/retention
  effects and fixture/stage acceptance. `git status --short` shows this document
  modified and pre-existing untouched `mcp.launch.json` untracked.

Only this document is amended. The pre-existing untracked `mcp.launch.json`
is unrelated and untouched. No commit, install, network call, external message,
deployment, game launch/attachment/injection, headset operation or original
binary change was performed. Independent Claude review and executor validation
remain next; production runtime acceptance is unproven.
