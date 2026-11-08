# J-107: deferred same-thread metadata retirement proof

Offline worker authoring only; no commit or native build. Changes are confined
to d3d12_hook.cpp, diagnostics_offline_tests.cpp and this report. Capture, GPU
copy/replay and XR remain disabled. No live action or runtime parity claim.

## Evidence

Read J-106-ambiguity-findings.md first, relevant J-105 notes, the claimed job,
STATUS.md, DECISIONS.md (including D-126), coordination instructions and relevant
source/tests. Only registered evidence l2j-live-20261021 was opened, at
tools/evidence/l2j-live-20261021-diagnostics.evidence.log. Get-FileHash verified
SHA-256 40620d7decd4c0e2f9abbd8ae9e8242597161f0551d63244f8761c9d29a46da3,
matching D-126. The supplied final suffix is ar=001;ak=4;mc=00000a00;mh=01000900.
It identifies the writer/kind, not the retired object's identity or completeness.

## Why the constructor cannot decide

releaseMetadata snapshots opaque keys before native Release, then constructs
the retirement event only after returned-zero. Its constructor initially knows
only rootKey; touch adds the remaining keys afterwards. The outer PSO may still
be inside native creation, with no returned object or canonical/ancestor keys.
Neither side's final disjointness is available to the old constructor rule.
The decision therefore moves to the existing locked PSO creation snapshot,
after every creation QI output has been touched and before diagnostics,
distinct-event eligibility and publication. No already-set ambiguity is cleared.

## Proof and conservative refusals

Accepted root creation and lifetime-validated PSO creation retain fixed arrays
of their complete creation keys. PSOs include every retained QI output and both
explicit root dependency keys, rather than reconstructing a smaller alias set
at Release. Retirement unions these keys with the existing pre-call keys.
The actual releasing address must occur in the saved certificate. Unregistered
objects sharing a hooked table, unsupported/malformed identities, binding/list
owners, native-call metadata activity and overflow cannot certify retirement.
The snapshot never dereferences a destroyed COM object.

Only a newly nested release inside an active same-thread pso_create defers the
parent's constructor mark. The child retains its own old reentry mark. All other
constructor relationships still mark both immediately. The parent has a
separate monotonically increasing audit count and a proven-count gate: distinct()
refuses an unresolved count, even if a caller omits resolution. od bit 5 includes
that unresolved state. No successful resolution can clear another writer.

At resolution, the parent must be known, bounded and serialised with intact
history/clock. Every audited child must be retained and completed with a known,
complete, nonoverflowing retirement certificate, disjoint from ALL parent keys
(including the PSO, explicit root and all ancestors/dependencies). Child keys
must also be disjoint from retained metadata and binding object/dependency/heap
keys across the parent's interval. Unknown retained peers refuse the exception.
A child's active/completed key, binding or capacity ambiguity invalidates its
certificate. Missing history, unfinished children, saturation and clock/serial
failure refuse. The original distinct() active/completed/binding checks still
run. Nested root/PSO creation and other events, binding reentry, self overflow
and other-thread conflicts retain their existing marks/checks.

The append-only suffix ;nr=XX counts all observed nested releases for this PSO,
including refused children, saturating at ff (which refuses proof). It is
independent of ar/ak and never decremented. Existing prefix and fields stay in
order; their mark-time maxima still describe actual marks, which now occur at
resolution for deferred failures. Maximum detail grows from 91 to 97 characters;
the two detail arrays grow from 96 to 104 bytes. Existing summary/context/log
bounds still fit. No new log reason, quota, disk partition or Draw scan is added.
All new history scans use fixed storage during creation/retirement only.

## Fixtures and checks

Extended the existing diagnostics_offline target; no CMake changes. Authored
fake-COM checks cover unknown outer output until child completion, disjoint
complete retirement, PSO/root key conflicts, unknown/incomplete certificates,
child overflow, other-thread conflict marks, binding reentry, root/PSO/other
nesting, unresolved decisions, active children, lost history, saturation and
a retained conflicting peer that ended before the child began. Existing
other-thread active conflict and allocation/no-I/O Draw fixtures remain.

An actual hooked final Release inside the fake native PSO callback contrasts
a validated independent root with an unregistered peer sharing the same table.
The positive control expects nr=01/ar=000 and advances to missing t10 root
parameter observation; the negative control expects nr=01/ar=001 and the
unchanged explicit-lifetime refusal. Existing attempt() checks freeze refusal
and requirements_proven=false. Encoding tests check full 97-character detail
and maximum refusal capacity. These are authored assertions, not executed results.

git diff --check passed on the final changes. A read-only PowerShell comparison
against HEAD confirmed the complete observeDraw and two Draw wrapper block
unchanged (2024 characters). Capture-enablement source is outside the diff.
The standalone command
node scripts/verify-offline.mjs failed before native compilation: configured
Python313/python.exe spawnSync returned EPERM. No native build or CTest ran;
diagnostics_activation_offline and xinput_proxy_offline were excluded from worker
runs. multi status failed because this checkout lacks .mcp.json; scoped board
claim/heartbeat succeeded. Existing untracked mcp.launch.json was preserved.

## Done when / handoff

Authoring is ready for executor pinning and mandatory other-family exact-pin
review. Merge requires the native Ninja build and all 43+ registered CTests to
pass, plus the offline integration gate. The known executor C1083 d3d12.h
include-environment fault does not waive validation. No synthetic assertion
proves the actual live retirements have complete certificates or are independent.
Untracked retirements will deliberately keep refusing. Actual caller identity,
runtime eligibility, safe GPU copy/replay, stereo and XR remain unresolved.

Exact executor commands, separately from the executor checkout in an offline
cmd.exe session with existing Ninja cache and disconnected dependencies:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -C Release --output-on-failure --no-tests=error
node scripts/verify-offline.mjs
```

Reviewer gate: node scripts/verify-offline.mjs --review.
Next discriminating check: executor builds and runs diagnostics_offline first,
then the full CTest command above. After successful validation/review, only a
separately authorized live campaign could compare nr, ar and od on a cockpit
PSO. nr alone never proves harmlessness and this report grants no live action.
