# J-104: independent split-PSO diagnostics

Offline source/fake-COM authoring against J-103 (`3bee119`). No commit,
native build, live action or capture/GPU/replay/XR/headset operation performed.

## Evidence and source finding

Read J-102-lifetime-findings.md and J-103-lifetime-diagnostics.md first. Only
registered evidence `l2g-live-20261020` was opened from the hydrated copy
`tools/evidence/l2g-live-20261020-diagnostics.evidence.log`; SHA-256 verified:
`6ae31ee727703a85e4168b4fbd6490b5d71aa5614bd2023d0c45730a961680a7`.
The PSO summary contains `pd=0fff07/0dcf03;rd=1e7f03/1e4f03`.

`PipelineInterfaces::observe` originally made PSO bit 2 false for ANY of:

- Object QI failed (FAILED HRESULT), or succeeded with a null output.
- Object QI succeeded but returned an address other than base or canonical.
- The returned Object's IUnknown QI failed, returned null, or succeeded with
  an identity different from the initially discovered canonical identity.

Thus the log does not establish an actual COM error or identity mismatch.
It also does not exclude a third, otherwise canonical-consistent ancestor.
The two-address check intentionally bounds the supported final-Release seams;
source inspection does not prove it incorrect for the supported shape. No
eligibility correction is proposed or applied. Supporting another address
would require independent identity, lifetime-routing and Release-coverage
proof, beyond this diagnostic job.

Crucially, `createGraphicsPSO` sets `event.footprint.known` to
`interfaces.valid && record.creation_interface_supported`. A failed ancestor
check therefore makes ordering bit 17 false through `unknown_self`, even if
there is no overlap and the root was created earlier. Bit 16 is only a begin
order comparison; it cannot prove nonoverlap. Other ordering failures remain
possible and are now observed independently. No event identifiers are logged.

## Appended fields

The old refusal prefix and all old fields, including pd/rd/bd, retain their
format and order. The old masks retain their short-circuit meanings. PSO
summaries append the same detail after pd/rd; root summaries are unchanged.
New fields snapshot creation observations, never refresh them at Draw.

Six slot indices s:

| s | Requested interface / source |
| --- | --- |
| 0 | Object / base |
| 1 | Object / canonical |
| 2 | DeviceChild / base |
| 3 | DeviceChild / canonical |
| 4 | Pageable / base |
| 5 | Pageable / canonical |

For each slot, query index q=2*s is the requested-interface QI, and q=2*s+1
is IUnknown QI on its successful nonnull output. This second QI also runs
on a third address for diagnostics, while eligibility still rejects it.

| Field | Encoding |
| --- | --- |
| qf | 12 bits: bit q means FAILED HRESULT for query q |
| qm | 6 bits: bit s means successful nonnull IUnknown result differs from canonical |
| qn | 12 bits: bit q means null output for an attempted query, regardless of HRESULT |
| qa | 6 bits: bit s means successful nonnull ancestor output is neither covered address |
| qh | 24 bits: two bits per query q, at offset 2*q: 0 unattempted, 1 success, 2 E_NOINTERFACE, 3 other failure |
| od | four-hex-digit known/value masks for the ordering components below |

`qf` distinguishes QI failure from `qm` identity mismatch and `qa` unsupported
address. Success includes S_FALSE. A failed call with nonnull output still
sets qf and retains/releases that output using the existing fixed reference
slots. An unattempted query has qh=0 and does not set qn. All six ancestors
are evaluated independently for a discovered split canonical address, even
when stability, round trip or an earlier ancestor fails. Self-canonical and
unavailable-canonical cases retain their old query policy (qh stays zero).

All od components are evaluated independently under the existing mutex;
empty relevant sets satisfy their universal conditions. All bits 0-14 are
known when the snapshot runs. A zero known mask / absent detail means the
creation path did not reach this observation.

| od bit | Passing component |
| --- | --- |
| 0 | event serial nonzero |
| 1 | event output address known |
| 2 | complete self footprint known |
| 3 | cohort clock not exhausted |
| 4 | retained history covers this event's begin |
| 5 | event not marked ambiguous |
| 6 | active binding count matches occupied active records |
| 7 | other active metadata footprints all known |
| 8 | other active metadata footprints all disjoint |
| 9 | overlapping completed metadata footprints all known |
| 10 | overlapping completed metadata footprints all disjoint |
| 11 | no overlapping same-thread active binding reentry |
| 12 | relevant binding records all known |
| 13 | no binding object/dependency/heap conflict |
| 14 | no binding creation with unresolved canonical identity |

The conjunction matches the original distinct() predicates; that function,
its blocking/overlap strings and the lifetime expression are unchanged.
The new masks never grant eligibility. In particular od bit 2 distinguishes
the self-footprint failure from independent ordering/overlap failures.

## Bounds and fixtures

Creation retains the existing 15-slot QI reference array; no new allocation
or interception is introduced. Independent ordering scans use existing fixed
pools and existing bounded footprint ranges. The 96-byte detail is formatted
once at creation and copied into the existing record. Draw only appends that
string in its existing snprintf, using a 192-byte stack context. No Draw QI,
allocation, ordering scan, patching, I/O or new logger key/partition is added.
Text capacity, quotas, first-example behavior and capture enablement are unchanged.

`diagnostics_offline_tests.cpp` extends fake COM controls for each source/IID:
E_NOINTERFACE, other failure, success with null, successful identity mismatch,
third-address identity, and failures/nulls of the identity query. Other slots'
HRESULT codes remain populated after a failure. Each of the 15 ordering bits
is perturbed through a synthetic cohort, with binding object/dependency/heap
conflicts tested separately. Existing masks/eligibility controls remain.
Summary/refusal checks require the appended creation snapshot; existing Draw
allocation/no-I/O traps cover its emission. A maximum-width refusal checks the
complete suffix fits the existing 384-byte text capacity.

## Done when / worker result / handoff

Worker source and diff audit completed; `git diff --check` passed. A read-only
Python source audit confirmed the lifetime expression and distinct() body are
byte-identical to HEAD, and maximum-width field/context/refusal text lengths
are 49/100/272 bytes (within 96/192/384-byte buffers). The required
standalone `node scripts/verify-offline.mjs` failed before native compilation:
`spawnSync %USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
No native fixtures were executed; no worker run of diagnostics_activation_offline
or xinput_proxy_offline was attempted. Programme status command failed because
this isolated checkout lacks `.mcp.json`; scoped claim/heartbeat succeeded.
Unrelated `mcp.launch.json` is not part of the authored file set.

Done for merge only after executor pinning, independent exact-pin review and
native Ninja build plus full CTest success. Exact executor commands, separately
in an offline cmd.exe session with the existing disconnected Ninja cache:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -C Release --output-on-failure --no-tests=error
node scripts/verify-offline.mjs
```

The known C1083 d3d12.h executor-shell environment fault does not waive native
validation. Worker test exclusions do not waive any executor tests.

Unresolved runtime facts: actual per-interface HRESULT/address/identity shape,
independent ordering components, and all capture/stereo/VR behavior. Synthetic
fixtures establish no runtime parity. Next command: executor build after
vcvars64 above. Only a separately authorized live campaign may then inspect
one refused example's qf/qm/qn/qa/qh/od fields and select the next investigation.
