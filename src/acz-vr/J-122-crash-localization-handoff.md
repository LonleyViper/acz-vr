# J-122: fixture stack-pressure repair candidate

Branch: `codex/job-j-122-dll-implementation`. No worker commit. Native acceptance
and numeric compiler-frame measurements remain pending with the executor.

## Evidence and classification

Executor feedback for narrowing pin b149b4f establishes **stack overflow**
(0xC00000FD): the SAME binary passes 606798 checks with a 64 MiB stack, and
overflows with the default 1 MiB. Last checkpoint: `identity helpers end;
identity draw fixtures begin`, 581891 checks. This worker did not measure
the exact faulting instruction or compiler frame sizes.

Source inventory: metadata_fixture::tests has five automatic Logs (log,
lateLog, disabled, observedLog, witnessLog), and calls identityDiagnostics
with that frame active. identityDiagnostics has four automatic Logs in its
body and three in its createRoot/createPSO/attempt lambdas. Each Log owns a
64-Record ring plus a **36864-byte** batch. In the reported /Od /RTC1 build,
disjoint scopes do not get frame reuse. J-121 enlarged every MetadataResult
by two full Inputs and added checker locals/return temporaries to this tree.

Classification: **fixture storage defect candidate**, based on the native
stack-limit A/B and this inventory. The exact overflowing callee and numeric
frames require the executor listing; do not report them as already measured.

Production audit: the J-121 path has two Inputs per MetadataResult, two Required
locals in covered_constants, one extra Snapshot word, and two uint32 stage facts
per PipelineRecord/Record. No whole provenance Contract or Log is constructed
on that Draw path. State/Resolver/Input array/ListState are passed by reference;
production Log activation is heap-backed. This rules out a new hundreds-of-KiB
J-121 object in those paths; total game-thread headroom remains unmeasured.

## Changes

- All 12 affected fixture Logs now use unique_ptr-owned storage and references.
  Lifetimes, sinks, destruction scopes and every old CHECK remain intact.
  Storage is allocated before the existing failNew probes are armed.
- All fixed J-122 stderr checkpoints remain.
- The entire metadata/identity tree runs synchronously on a thread with an
  explicit **1 MiB reservation**, checked with VirtualQuery and
  GetCurrentThreadStackLimits. Enlarging the executable stack cannot hide this
  regression. Join is bounded to 60 seconds and nonzero completion fails.
- Compile-time object budgets (bytes): Input/Required <=1280 each,
  MetadataResult <=2688, Witness <=1536, Snapshot <=192, PipelineRecord <=2048,
  ListState <=4096. These are bounds, NOT measured compiler-frame sizes.
- The regression prints actual native sizeof values for those types and Log.
  Exact sizeof numbers remain pending executor execution.

Production source/eligibility/log fields/bounds/defaults are unchanged.
Only this handoff and diagnostics_offline_tests.cpp changed.

## Checks actually run

Source/diff audit retains all older CHECKs and finds no production changes.
Executed Python structural check: all 1362 prior CHECK expressions preserved
in order, all prior checkpoints preserved, exactly 12 targeted heap Logs and
no remaining automatic Log declarations in the two affected functions.
`git diff --check` passes. Standalone `node scripts/verify-offline.mjs` fails
before native checks with `spawnSync
%USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
Programme status command fails because this isolated checkout lacks .mcp.json;
scoped board claim/heartbeat succeed. No native build or CTest was run here,
including the excluded diagnostics_activation_offline/xinput_proxy_offline.

Minimal read set: J-122 feedback; acfa9e7..bd5db3d source/test diff; affected
fixture, Log header, production types/checker/hook. No evidence contents read.

## Executor handoff

Run separately from the repository root in the existing disconnected native
environment (no installs/network):

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -R "^diagnostics_offline$" -C Release --output-on-failure --no-tests=error
ctest --test-dir build-ninja-integration -C Release --output-on-failure --no-tests=error
node scripts/verify-offline.mjs
```

Expected acceptance: **43/43**, not an observed worker result. Collect both
stack-regression checkpoints and the sizeof/reservation line. Successful CTest
runs hide stderr; use the same focused CTest with `-V` to collect that line.

Measure frames on both pre-repair narrowing pin and candidate in executor-owned
scratch checkouts with identical flags. If CL is initially empty, assembly
listings can be requested without control-file edits using separate commands:

```bat
set "CL=/FAs"
cmake --build build-ninja-integration --clean-first
set "CL="
```

Preserve pre-existing CL settings if present. Read generated .asm prologues:
`sub rsp` for small frames or the size passed to `__chkstk` for large frames.
Report numeric frames for metadata_fixture::tests, identityDiagnostics, its
three logging lambdas, observeRequirements_locked, check_metadata,
covered_constants, both hooked Draw wrappers, createGraphicsPSO and its
observation lambda. No native listing measurements exist from this worker.

Done when: exact candidate reviewed, native frame/sizeof report confirms the
classification, explicit 1 MiB regression and all **43/43** pass without a
larger-stack relink, and offline gate passes before merge. If it still
overflows, preserve the last checkpoint and inspect remaining helper frames.

Unresolved runtime facts: camera/capture access, texture/sampler provenance,
synchronization, replay, stereo and headset output. Capture stays disabled;
diagnostic_ownership_pins defaults OFF. Offline fixtures prove no runtime parity.
