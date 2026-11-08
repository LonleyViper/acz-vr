# J-119: witness companion sink fixture repair

Determination: the J-118 fixture is wrong, not production emission. The assertion
originally at diagnostics_offline_tests.cpp:3724 searches MemoryIO::evidence
(diagnostics.evidence.log). observeRequirements_locked emits the bundle witness
through diagnostics::emit -> Log::push -> ordinary ring -> Log::flush -> write,
which appends to diagnostics.log (MemoryIO::files[0]). The reserved
metadata_outcome path separately persists its first example in the evidence
sink. A shared requirements_metadata event kind does not select that sink.
The companion is not lost to first-example dedupe; that policy applies to the
separate metadata outcome record. The fixture already enables the switch,
advances the requirements clock, issues the exact indexed draw and flushes.

The repair preserves the exact draw/rc=8 payload assertion and reads files[0].
Added checks establish that the bundle counter and sampled-attempt count each
advance exactly once, I/O succeeds and capture/proof remain disabled. No older
(pre-J-118) assertion changed. No production code changed: eligibility, refusal
prefix/fields, default-OFF switch, bounded logging and Draw behavior are intact.
In particular the refusal remains:

`rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported`

## Evidence and verification

Inspected the J-118 diff (96ae56b..0761052) in the allowed source/test trees,
J-118 findings, and relevant hook, witness, logger, MemoryIO and proof code.
No additional raw evidence IDs were opened. The existing native fixture uses
the hydrated e10-drawcpt-pso input; it was not executed by this worker.

- Standalone `node scripts/verify-offline.mjs`: FAILED before native validation,
  spawnSync of the configured Python313/python.exe returned EPERM.
- `git diff --check`: PASS.
- `pwsh -NoProfile -File tools/jobboard.ps1 multi status`: unavailable in this
  isolated checkout because .mcp.json is absent. Scoped board claim succeeded;
  no configuration changed.
- Native compilation/tests not run: this job assigns native execution to the
  executor. Neither diagnostics_activation_offline nor xinput_proxy_offline was
  run by the worker. No hydrated/stale executable is claimed as validation.

## Done when and executor handoff

Worker changes are uncommitted and limited to this handoff and
tests/acz-vr/diagnostics_offline_tests.cpp. Executor owns commit/native checks;
Claude owns independent review of the exact pin. Done when that review approves,
the repaired exact assertion and all 43 native CTests pass before merge, and
the gate outcome is recorded. C1083 d3d12.h is an environment fault, not a pass.

Next discriminating check is the freshly built diagnostics_offline test. Exact
executor commands after vcvars64, from repository root unless stated otherwise:

```bat
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -R ^diagnostics_offline$ -C Release --output-on-failure
ctest --test-dir build-ninja-integration -C Release --output-on-failure
node scripts/verify-offline.mjs
```

Inside build-ninja-integration the equivalent full-suite command is
`ctest -C Release --output-on-failure` (required 43/43). The focused executable
requires its executable-directory working directory. From build-ninja-integration,
the direct alternative to focused CTest is:

```bat
diagnostics_offline_tests.exe ../tools/evidence/e10-drawcpt-pso.json
```

Unresolved runtime facts remain native observed word coverage/meaning, unhooked
activity, GPU ordering/content/safe copy, stereo and VR output. Synthetic tests
cannot establish runtime parity. No runtime action is authorized by this repair.
