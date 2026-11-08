# J-103: lifetime refusal discriminator

Offline authoring only. No commit, native build, live action, capture enablement,
GPU copy/replay or XR/headset operation was performed.

## Evidence

Read J-102-lifetime-findings.md and only the two cited hydrated log evidence IDs:

- `l2e-live-20261020`: `tools/evidence/l2e-live-20261020-diagnostics.evidence.log`,
  SHA-256 `612b074a814124061a03d3342a025089c29353c3571107631b8537b30dc0798a`.
- `l2f-live-20261020`: `tools/evidence/l2f-live-20261020-diagnostics.evidence.log`,
  SHA-256 `423d135beaf7c1c20ee77804feca6e6256090bcead9d854950b50ab2b9d3258d`.

Both hashes were computed from the copies. Their explicit-lifetime records have
`b=64`, the identical split/pr=0/canonical-release=unproven reason and the
same cb10/draw/lp fields. The records do not identify the failing conjunct.
Sequence numbers are not attempt counts. No original evidence was modified.

## Fields and interpretation

Each pair is six hexadecimal digits of `known/value`. A zero known bit means
unobserved or not reached, not a failed check. A known bit with zero value means
false. In particular pending bits mean restoration IS pending when set.

- `pd`: PSO creation snapshot, appended to its existing numeric summary and
  to explicit-lifetime refusals after all existing text/fields.
- `rd`: accepted explicit dependency's root creation snapshot at PSO creation.
  All zeros means no accepted root creation record was found then.
- `bd`: currently bound root's accepted creation snapshot at the first refused
  draw. Its bit 16 also compares that root's publication with this PSO's event.
  This distinguishes a root observed later from an already accepted dependency.
- `ld`: root creation snapshot appended to each existing root metadata record.
  Its existing reason, order, blocking, overlap and other fields remain intact.

| Bit | Observation |
| --- | --- |
| 0 | canonical QI stable |
| 1 | canonical-to-base round trip |
| 2, 3 | Object QI and canonical identity from base / canonical |
| 4, 5 | DeviceChild QI and canonical identity from base / canonical |
| 6, 7 | Pageable QI and canonical identity from base / canonical |
| 8, 9 | base / canonical Release covered |
| 10, 11 | base / canonical Release has a unique native dispatch owner |
| 12, 13 | base / canonical Release protection restoration pending |
| 14 | canonical address differs from base at creation |
| 15 | explicit dependency identity accepted at PSO observation |
| 16 | dependency root creation began before PSO creation began |
| 17 | creation metadata event has distinct, known ordering |
| 18 | supported creation IID/output shape |
| 19, 20 | base / canonical lifetime installer succeeded |
| 21 | PSO per-object routing has no conflicting owner |

Root records report the interface checks the existing root contract actually
performs (stable QI, root round trip and supported output). Root ancestor bits
2-7 stay unknown: the root path does not query those interfaces. The existing
self-canonical PSO fast path likewise leaves its skipped stability/ancestor
checks unknown. No extra QI or Release was added to manufacture those facts.
Earlier failed QIs keep later checks unknown. Release-table observations are
read-only and distinguish unique dispatch ownership from actual patched coverage.

Ordering uses nonserialized event publication keys; no pointers, pointer hashes,
lifetime tokens or event IDs are output. Root-before-PSO is a begin-order
observation, not a claim of nonoverlap; bit 17 and the root's existing order
fields carry that separate fact. `rd` and `bd` are snapshots, not lifetime
refreshes. The logger retains the FIRST example per static outcome/cb10 key,
as before, even if later attempts have different masks.

The PSO summary is emitted after its observation so early failures also carry
flags; shader-stage records keep their old payloads and precede that summary.
The number of PSO summaries, their a/b/c values and the existing quotas remain
unchanged. Refusals preserve the full existing prefix and cb10/draw/lp suffix
before appending 51 characters. No new outcome keys or logger partitions exist.
All added draw work is fixed stack formatting, scalar reads and existing-map
lookups under the capture mutex. It does not query COM, allocate, patch or do I/O.
The capture checks and the original lifetime expression remain unchanged.

## Fixture and worker checks

The existing `diagnostics_offline_tests.cpp` fixture now:

- Faults each split QI condition independently by source and IID; later QIs
  stay unknown, and clearing the fault restores the successful mask.
- Separately perturbs each Release table's pending restoration, ambiguous native
  dispatch owner and patched slot; checks the exact corresponding mask changes.
- Checks base installer failure, canonical installer failure, root-before-PSO
  versus root-after-PSO, missing dependency, distinct-event refusal, conflicting
  object routing, unsupported output, stable-QI and round-trip failure through
  actual fake creation wrappers, plus each bit's independent encoding.
- Checks PSO/root summary suffixes and preserved refusal fields; existing
  allocation-failure/no-I/O Draw assertions also cover the new formatting.

`git diff --check` passed. The standalone required command
`node scripts/verify-offline.mjs` failed before compilation because spawning
`%USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe` returned
`EPERM`. No native fixture execution is claimed. No worker run of
`diagnostics_activation_offline` or `xinput_proxy_offline` was attempted.
`pwsh -NoProfile -File tools/jobboard.ps1 multi status` failed because this
isolated checkout has no `.mcp.json`; scoped MCP claim/heartbeat succeeded.
No CMake, runtime, control-file or account-setting changes were made.

## Done when / handoff

Worker authoring is ready for independent source review. Done for merge only
when the executor pins the changes, the exact other-family review approves that
pin, and a native Ninja build plus the full CTest suite pass at that pin.
No commit was created by the worker. Any unrelated `mcp.launch.json` is outside
the authored file set and must be preserved separately.

Exact executor commands, separately in an offline cmd.exe session from the
executor checkout, using the existing Ninja cache and disconnected dependencies:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -C Release --output-on-failure --no-tests=error
node scripts/verify-offline.mjs
```

All registered native tests must pass; worker exclusions do not waive executor
tests. The previously reported C1083 `d3d12.h` integration-shell problem is not
evidence of a successful native build and does not waive the vcvars64 gate.

Unresolved runtime facts: the actual split PSO's failed condition, its accepted
explicit root association at creation, live interface shape and table ownership,
and any runtime capture/stereo/VR behavior. Synthetic tests establish none of
these. Next command is the executor build above after vcvars64. Only a separately
authorized live campaign could then inspect one refused cockpit example's pd,
rd and bd masks alongside its root ld/order record and discriminate the cause.
