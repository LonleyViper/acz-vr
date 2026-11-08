# J-143: ordered GPU copy of one selected DrawCPT draw

Design only, offline, for D-173 / master `ecbf6e0`. No implementation, native
experiment or enablement is delivered here. Capture and replay remain disabled.
D-151/D-152 record the staged L3 GPU authority; this worker's current instruction
permits only this document. A later executor must satisfy the gates below before
using that authority. Gate 4 is NOT authorised.

Evidence key: all paths below are repository-relative. Findings are under
`src/acz-vr/`; logs are the hydrated, read-only registered copies under
`tools/evidence/`. Only these four registered evidence IDs were read:

| ID / file basename | SHA-256 | Relevant log lines |
| --- | --- | --- |
| `l3a-live-20261021-diagnostics.evidence.log` | `68d946cc674a8138501cf08d13face703bf515484ae23b587acb1ddd881f6d4f` | 785–790: source spans, maps, submission |
| `l3d-live-20261022-diagnostics.evidence.log` | `e6162af137154c8c5e0905820e1703e85a179da7cdfcf85ceac8ac200ffc88fc` | 917–925, 1071: unowned snapshot refused |
| `l3e-live-20261022-diagnostics.evidence.log` | `f6b90fdcae89a48df3b66f479a12180ed4335fd691d957553dd199de29178e3f` | 972–980: owned CPU samples, completion unknown |
| `l3f-live-20261022-diagnostics.evidence.log` | `e4097516ad2dea01837c6efc1500c33fd03438d9e556c71e8900682e655c39bc` | 1072–1082: corrected sampled-diagnostic path |

Hashes were computed locally and match D-154/D-168/D-169/D-172. D-173's approval
covers sampled diagnostics only. Earlier J-131/J-132 acquisition claims are
superseded by J-139, then J-141/J-142 (D-164, D-168–D-173).
L3f line 1082 records covered cb10/t0/s0 and t10/IA metadata, index-verified
sampled bytes and `cn=none`, with draw use and GPU completion explicitly unproven.

**ASSUMPTION convention:** every proposed mechanism, limit, D3D12 API-contract
premise and future outcome below is an ASSUMPTION to validate, not a measured
property of this game. Native observations are individually cited. No network
documentation or additional raw evidence was consulted; API premises require
independent review against the executor's available SDK before implementation.

## 1. Coherence problem and the precise ordering claim

L3a reports committed UPLOAD buffers with one outstanding Map, no observed Unmap,
and repeated source resources across submissions (D-154; L3a 785–789). Application
retention of the returned pointer is unknown. Direct CPU stores bypass Map/Unmap
hooks. Resource ownership and complete hook coverage prevent neither those
stores nor game ring-buffer reuse (J-142-review-fix-findings.md; D-170/D-172).
The game can change bytes after both equal CPU samples and before GPU consumption;
equal samples can also hide intervening writes or mixed-time reads. The L3f hashes
`11bf9348396aef57`, `1730612e9cabdcf0`, `967d8f291117f083` describe samples only
(L3f 1076–1079); they are not expected constants for future scenes.

Proposal (ASSUMPTION): use `ID3D12GraphicsCommandList::CopyBufferRegion` on
the game's own open DIRECT list: recorded immediately before the Draw, in the same recording/submission.
The copy then belongs to that recording generation and its actual submission;
it samples on the GPU timeline rather than at hook CPU time. Under conforming
application fence discipline and no source writes between GPU reads, the
copy and draw can observe the same version. This is conditional: it does not
redirect the draw to the destination, freeze the source, establish CPU writer
exclusion, or prove equal contents for the two reads. Even adjacent commands
are not an execution-serialization guarantee: independent reads can overlap.
They are not an atomic source snapshot with the draw. A CPU rewrite before execution,
during the copy, or between copy and draw remains possible. UPLOAD coherence
does not replace the application's obligation to wait before reusing memory.

After a proxy fence completes, hash the immutable readback and compare its bytes
and per-range hash with the associated optional CPU diagnostic sample. Mismatch
detects a temporal discrepancy; it does not identify its writer or timing. A
post-fence source sample, if separately admitted by J-142, can expose later reuse
but is also only a sample. Equal hashes do not exclude ABA writes, collisions,
mixed reads, or a rewrite between copy and draw. Retain exact bytes and SHA-256
alongside compatibility FNV hashes; byte-compare when both payloads are present.
This detects some races rather than preventing them. A completion fence proves
completion of the copy, not that the draw consumed its bytes. Label results
`gpu-copy-completed;draw-use-unproven;writer-exclusion-unproven`. Do not issue a
safe-copy certificate or satisfy a replay source-version predicate from equality
(D-173; inert requirements in gpu_transaction_contract.h).

## 2. States, heap selection, bounds and destination lifetime

Observed spans are t10 element zero 416 bytes, VB 512 bytes and IB 18 bytes;
t10/VB share a 12 MiB resource and IB is in a 4 MiB resource (D-154; L3f 1072–1078).
L3f source offsets are `0x1499e0`, `0x149b80`, `0x3510`. These are observations,
not constants: resolve current tokens, VA containment, offsets and lengths anew.
VB stride `40` in the log is hexadecimal (64 bytes); 512 / 64 = eight vertices
(L3f 1079). Copy the accepted whole VB view, not a span inferred from old indices.
For indexed draws validate start/count/index width and cut/topology separately;
a nonindexed draw has no IB and is outside the initial game-buffer experiment
(J-131/J-132 findings; D-172).

ASSUMPTION, API states: UPLOAD resources remain `D3D12_RESOURCE_STATE_GENERIC_READ`,
which includes COPY_SOURCE, and cannot transition away from that state. Do not
barrier, Unmap or change a game source. L3 observation's `state=unknown` is not a
state witness; use validated committed UPLOAD heap/type facts for this invariant
(J-130-l3-observation-findings.md; D-154). Reject DEFAULT, CUSTOM, placed/reserved,
alias/heap ambiguity, protection/cross-device issues or missing creation evidence.

Choose a proxy-owned READBACK buffer created in COPY_DEST. It stays COPY_DEST;
there is no transition to a CPU-readable resource state and no source restoration
barrier. Map subresource zero only after completion and destination-write closure
(section 4), using the exact CPU read range; Unmap with an empty written range.
Alternative, deferred: DEFAULT destination initially COPY_DEST, barrier that
destination alone to COPY_SOURCE, copy into READBACK/COPY_DEST, then signal after
both copies. This adds allocation, barriers and commands with no benefit to the
first tiny readback experiment; neither alternative proves source coherence.

Proposed hard-capped destination ring (ASSUMPTION): three process-lifetime slots,
each one 4096-byte READBACK buffer, at most one armed/in-flight experiment. Full
three-role layout would place t10 at destination 0, VB at 512, IB at 1024: 946
payload bytes, maximum used endpoint 1042. Validate nonzero lengths, subtraction-
based source/destination bounds and nonoverlap before any command. Reject larger
or changed spans; never truncate. Three slots cap logical destination bytes at
12288. Charge actual allocation sizes separately, proposed aggregate allocation
cap 1 MiB, checked before creation; driver overhead/residency are not bounded by
logical Width. Known-pattern source: one 4096-byte UPLOAD allocation under the
same aggregate cap. No per-Draw allocation or large automatic storage.

Initial staged campaign consumes at most two slots (one per step); the third is
reserved, not permission for an automatic retry. Slots are never recycled in this
first design, even after success. Hold resources from reservation through every
possible execution and all readback work. An unsubmitted recording is not safe
to release merely because a timeout expired. It may still be submitted later.
Quarantine charged allocations on ambiguous submission, failure or timeout; no
eviction, reallocation loop or attempt-budget reset. Safe release additionally
requires recording invalidation and fence completion as specified next.

## 3. Recording on the game's list and preserving its calls

Existing interception seams are `hookedDrawInstanced`,
`hookedDrawIndexedInstanced`, and `hookedExecuteCommandLists` in d3d12_hook.cpp
(1892–1908 in this baseline). Draw hooks observe before calling the saved native
Draw; capture-list discovery checks DIRECT type (same file, hookCaptureCommandList).
These seams are insertion candidates, not evidence that GPU copying exists.

ASSUMPTION, admission requires all of the following before the first copy:

- Exact matched DrawCPT PSO/root, current list identity/lifetime, recording
  generation/revision, selected draw ordinal and argument tuple; accepted current
  metadata and independently held source entries. Never select by raw address or
  a historical list alone (J-142 findings; D-173).
- Fully observed open recording after successful Reset; no failed Close,
  concurrent mutation, bundle, ExecuteIndirect or unsupported interface escape.
  Destination, source owner and queue/device setup have already succeeded.
- No active predication, query interval, render pass or other restriction that
  could predicate the copy, prohibit it, or alter measured query results. Absence
  of observed events is insufficient without complete coverage from Reset.
  New coverage of SetPredication, Begin/EndQuery and versioned render-pass seams
  is required: the inspected baseline does not establish these negatives.
  Never disable/restore game predication or end a game query to make room.
- Same-device source/destination, validated spans/states, stable ownership
  admission and capacity, one reserved command recipe and an empty slot. Any
  unsupported state or lost observation refuses the insertion.

Record only the bounded CopyBufferRegion recipe via the game's list, bypassing
recursive proxy observation with saved valid dispatch. Immediately forward the
original Draw exactly once with its original arguments. Copy commands do not
change graphics root signature, PSO, descriptor heaps/tables, IA views, viewport,
scissor or render targets (ASSUMPTION, API premise); no graphics setters, Close,
Reset, allocator change, extra Draw or replacement submission are permitted.
The destination is private; the game continues drawing from its original source.

Before insertion, any allocation/coverage/admission failure skips proxy work and
preserves the original call. After insertion, CopyBufferRegion returns void and
recording cannot be rolled back: a later error must mark the result unusable,
retain all possibly referenced owners, forward Draw and Execute normally, and
stop further admissions. Exception handling cannot repair a poisoned list or
promise native-call success after device removal. Do not claim atomic fail-open
rollback. Proposed budget: one extra copy in each first experiment, at most three
in the later three-role design. GPU latency, query effects, command-recording
overhead and stack changes are unmeasured; remeasure against D-167's incomplete
9432-byte known-chain screening value before live enablement.

## 4. Submission, nonblocking completion and readback closure

ASSUMPTION, use a proxy-owned device fence and a queue owner acquired during a
valid queue-bearing API interval, not from a stale observed queue address.
Identity tuple: device/queue lifetime epochs, fence identity/epoch/value, list
lifetime/generation, draw ordinal/revision, submission serial and list ordinal.
Current L3 joins are diagnostic only and explicitly say completion unknown
(J-130 findings; L3f 1080); this design needs a new authenticated completion path.

On hooked ExecuteCommandLists, identify the exact copied recording in the actual
argument array, forward the original count/pointers once, then append proxy
`queue->Signal(fence, monotonically increasing value)` on that same queue.
Signal comes after the entire submitted batch, which is a conservative completion
boundary. No queue replacement, source mutation, game fence modification or
`Wait`/CPU fence wait occurs on a game thread. CPU metadata critical sections
must be short, allocation-free and never span native submission or Signal.
Concurrency or reentry that prevents a trustworthy submission-to-signal join
invalidates the result; do not hold the game's queue hostage to serialize it.

Signal HRESULT success is not GPU completion. A bounded owner-controlled poller
checks GetCompletedValue outside game hooks, e.g. at most 10 polls/s, with a
proposed five-second wall deadline. Accept a value at least the issued value
only for the same fence/device epoch; reject regression, wrap/saturation and
UINT64_MAX/device-removed indication. No event wait, busy spin or game-thread
Map/hash. Failed Signal or exception after submission is ambiguous: preserve
owners, quarantine and disable; never label it unsubmitted or release on timeout.

**Repeated execution is a second destination writer.** Copies are baked into
the recording and run on every Execute, including duplicate list entries. One
completed fence alone does not permit mapping or freeing: the list can execute
again and overwrite the readback. Keep a tombstone for that recording, track all
executions and their fence watermarks, and refuse a result on duplicate/multiple
executions, cross-queue execution, missing seams or concurrent submission gaps.
Before mapping require observed successful Reset that discards the old recording,
no active Execute for it, and completion of every known submission through its
last watermark. Reset is a recording-invalidation witness, not a completion
witness. A complete list-interface/Reset/Execute observation contract and proper
application list synchronization are ASSUMPTIONS; if they cannot be established,
do not read or reuse the destination. Never Reset a game list to obtain closure.
Keep the pending owner's GPU leases across Reset until fence completion.

For step 1, a proxy-owned list may be used for preliminary mechanics, but the
required decisive experiment inserts the known-pattern copy at the game-list
seam and therefore exercises this same closure rule. Once closed and complete,
map, copy into bounded CPU-owned bytes, hash and publish the exact manifest;
unmap, mark consumed, then release eligible private/source leases outside locks.
Completing a transaction releases only its logical source GPU lease; it never
releases J-142's permanent upload reference. Private resources additionally need
their own recording closure, completion and readback-consumption conditions.
No source CPU mapping is required for the GPU read. An optional CPU comparison
must independently satisfy every J-142 coverage/currentness check.

On device loss: close new admissions, invalidate that epoch, suppress Map/hash
and completion claims, retain uncertain GPU leases until an outside-loader-lock
owner establishes safe device teardown. No device recreation or automatic retry.
Timeouts likewise quarantine; process termination performs no foreign COM work.
Polling, hashing and pending completion must join the ownership admission domain.

## 5. Ownership tables, retirement and teardown barriers

Use J-141/J-142's already-owned upload entries with matching creation serial and
native token, validated committed UPLOAD form and retained dispatch. No AddRef,
QI, vptr probe or ownership acquisition through a historical source at Draw.
Their limits remain 32 reservations, 16 MiB/object, 512 MiB aggregate logical
retention, no eviction/retry/replenishment (J-142-review-fix-findings.md; D-173).
The GPU ledger holds logical leases on those owned references until both
completion and recording invalidation; it cannot let a table drain invalidate
them. Holding a resource alive does not freeze its bytes.

Keep SRV candidates separate: t0 ownership transfers from a live view-write
acquisition to a permanent pin; retired heap generations refuse promotion
(J-135-slot-keyed-candidates-findings.md; J-139-retirement-snapshot-findings.md;
D-159/D-164). A buffer-copy result cannot extend descriptor history, candidate
capacity, texture ownership or eligibility. Publish retirement before outgoing
owned Releases, detach under the mutex, and Release outside it; never resurrect
a token from an address match. Uncertain retirement/currentness refuses new work.

J-136's zero-active-to-closed barrier accounts for CPU hook activity, not GPU
completion; its repeated-success return is not a join. Its module is pinned,
there is no reachable populated production shutdown owner, and DllMain is under
loader lock (J-136 findings; D-163/D-164). A new GPU-pending ledger is therefore
a prerequisite before insertion: shutdown must also refuse while recording,
pending GPU references, polling or readback work remain. Do not merely add GPU
objects to the existing unconditional drain. Keep CPU admission open for saved
native forwarding and owned cleanup while experimental admission is closed;
never invoke global hook closure in an active game to stop the experiment.

Implement later an owner-controlled, outside-loader-lock quiescent cleanup that
retires only closed/completed records. Busy shutdown returns without releasing
anything and without waiting; owner retries are bounded. At process termination
perform no COM dispatch. Preserve module pinning and quarantined owners on every
uncertain path. Source inspection and synthetic cleanup fixtures cannot establish
reachable production cleanup (D-164); this unresolved owner blocks shipping.

## 6. Incremental experiments and exact refusal gates

These are proposed later executor cards under D-151/D-152, not actions by this
worker. Independently review the design, implement offline, obtain exact-pin
other-family review, run the offline/native gate, and remeasure stack first.
Validate list restrictions, closure, identity joins, ambiguous-commit retention
and GPU-aware teardown with appropriate offline fault fixtures. Synthetic
fixtures do not prove GPU behaviour. An unresolved API premise or coverage gap
refuses enablement. Use exclusive campaign game/GPU resources, diagnostic-only
default-OFF controls and an explicit outside-loader-lock owner. No automatic
advance, retry, capture, replay, RenderDoc attachment or headset work.

**Step 1 — one known-pattern copy, no game data.** Prepare a proxy-owned UPLOAD
buffer with deterministic bytes, flush/publish CPU writes according to the
reviewed SDK contract, then cease all writes and retain it. Insert one 4096-byte
copy into a private READBACK destination at the qualified matched game-list
Draw seam. Do not copy any game resource. One attempt, one in-flight transaction;
game Draw arguments/bindings and original Execute remain unchanged.

Required live evidence: DLL/config/source/build pins; pattern generator/expected
SHA-256; owned source/destination identities, sizes/states; list generation and
restriction coverage; matched draw and insertion order; successful Close and
Reset observations; exact Execute queue/ordinal/serial join; Signal HRESULT and
fence tuple; pending-to-completed progression; recording closure; post-fence Map
range, byte-for-byte equality and hash; safe release/quarantine disposition;
bounded duration/counters and normal campaign cleanup/baseline record. This
proves only mechanics and owned-pattern transfer on the observed path.

Refuse/stop on any gate miss, unknown active restriction, missing closure, extra
execution, Signal/Map failure, mismatch, device loss, timeout, increased stack
screening failure, crash or unacceptable game disturbance. Do not switch to a
different queue, standalone list or repeated run and call it the same successful
experiment. A standalone owned-list success alone does not validate insertion.

**Step 2 — one selected game-buffer copy, read-only.** Only after step 1 evidence
is independently accepted, arm a fresh one-attempt card. Choose the selected
draw's IB first: exactly nine 16-bit indices / 18 bytes in the cited observations
(L3f 1078–1079; D-172). Resolve its current bounds and held entry; refuse a changed
shape. Record one GPU read into a separate destination immediately before the
original indexed draw; never write the IB, alter its mapping, or redirect the draw.
No t10/VB copy yet; the later three-role recipe requires a further reviewed card.

Required live evidence: all step-1 identity/state/order/completion/closure fields,
plus current IB creation/token/offset/span, Draw arguments and cut/topology,
J-142 ownership eligibility, exact completed readback bytes and both hashes,
optional same-key CPU sample/hash with its timestamps and limitations, and index
decoding/range analysis against current accepted VB metadata. Log equality or
mismatch explicitly, without making either a coherence certificate. Historical
L3f hashes are comparison context, not a required outcome. Required verdict:
completed ordered IB diagnostic, draw-use and writer exclusion still unproven.

Refuse/stop for step-1 failures, missing creation ownership, stale metadata,
incomplete seams, ring-budget exhaustion, unsafe optional CPU-read coverage,
unexpected bytes/index range, evidence mismatch or observed rewrite. Missing
optional CPU sample must be reported; if comparison is the card's acceptance
criterion, it refuses that acceptance rather than silently omitting comparison.
Never relax index checking or enable capture on equality. D-151's RenderDoc
comparison is a separate later bounded card; it is deliberately not performed
by either step here. Capture/replay stay disabled throughout both experiments.

## 7. Non-goals, unknowns and risks

No stereo, eye projection, frame owner, Present definition, OpenXR, headset,
texture/sampler copy, replay submission or capture enablement. Gate 4 is NOT
authorised (D-151/D-173). Submissions/list generations are not frames
(J-130 findings). This design does not establish game fence discipline, identical
draw-consumed bytes, safe replay, or complete production stack headroom
(D-167/D-170/D-173). Those unknowns survive a successful known-pattern experiment.

| Risk / evidence | Proposed control (ASSUMPTION) | Remaining refusal or limitation |
| --- | --- | --- |
| Mapped CPU writes / ring reuse (D-170/D-172) | GPU-time copy; completed immutable readback and hash comparison | Detects discrepancies only; equality cannot prove draw use |
| Recorded-list re-execution (J-130 link model) | Generation tombstone, all-submission fences, successful Reset closure before Map | Missing coverage or extra execution quarantines; no destination reuse |
| Predication/query/render-pass restrictions (unmeasured) | Full from-Reset seam coverage; skip insertion when unknown/active | Coverage is an implementation prerequisite, not present proof |
| Partial void recording / failed Signal | Prepare first; retain after any possible recording/submission | No rollback guarantee; stop and preserve original calls |
| Dead historical resource (D-164/D-168) | Creation-time held entries, token/serial match; no Draw AddRef | Missing owner refuses; ownership gives no byte immutability |
| Lost queue/list identity or concurrent Execute | Same-queue fence tuple and complete execution ledger | No inferred join; ambiguous result remains unusable |
| Timeout/device removal | Nonblocking bounded poll; epoch invalidation and quarantine | No deadline-based release of possible GPU references |
| J-136 CPU-only teardown (D-163/D-164) | Separate experiment admission and GPU-pending barrier; outside-loader-lock owner | Reachable production shutdown remains unproved; no shipping claim |
| Cost, stack, retained memory (D-167; J-142 findings) | Fixed charged slots, one attempt per card, fresh stack measurement | Native overhead/headroom unmeasured; limits are not residency bounds |
| Hash equality misread as safety (D-173) | Publish raw bytes and explicit diagnostic labels | No source-version/capture/replay certificate |

Worker handoff: only this new document was authored; no source/test changes or
commit. Local evidence hashes matched the four decision pins above. Source and
document audit confirmed the seven sections, experimental gates and risk table;
`git diff --check` passed for tracked changes (this new file is untracked).
The required standalone `node scripts/verify-offline.mjs` exited 1 at Python
syntax subprocess creation: `spawnSync %USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
No native build/tests ran and no runtime parity is claimed. Programme status
wrapper failed on absent worktree `.mcp.json`; scoped jobboard claim succeeded.
Pre-existing untracked `mcp.launch.json` is unrelated and untouched. Executor owns
pinning/integration; independent Claude review and Astra's design questions
remain. Next executor check is the standalone offline gate in its permitted
environment; no infrastructure repair or live action is requested by this file.

### Questions for Astra

1. Is the conditional GPU-ordering claim precise enough, including the CPU
   rewrite between copy and Draw and the inability of equal hashes to exclude it?
2. Are READBACK/COPY_DEST, UPLOAD/GENERIC_READ and insertion restrictions correct
   for the supported SDK interfaces; what additional seams must refuse admission?
3. Does successful Reset closure plus all-submission fences suffice to exclude
   readback overwrites, given the proposed full alias/Execute observation contract?
4. Is quarantine and GPU-aware admission sufficient to keep J-136 from releasing
   referenced objects, and which outside-loader-lock owner is required first?
5. Are the known-pattern-first and IB-only evidence/refusal gates adequate under
   D-151/D-152 while capture, replay and gate 4 remain disabled?
