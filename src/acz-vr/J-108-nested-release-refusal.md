# J-108: nested-release refusal reasons

Offline worker authoring against J-107/master `3b45444`, on branch
`codex/job-j-108-dll-implementation`. No commit, native execution, game access,
attachment, deployment, GPU copy/replay, capture enablement, XR or headset action.

## Evidence and finding

Minimal inputs inspected: J-107-nested-release-proof.md,
J-106-ambiguity-findings.md, relevant d3d12_hook.cpp and diagnostics fixtures,
STATUS.md, DECISIONS.md (D-127), coordination instructions and the claimed job.
The only registered runtime evidence opened was the hydrated
`tools/evidence/l2k-live-20261021-diagnostics.evidence.log`, SHA-256 verified with
Get-FileHash as
`d7a8145c2f704f1bd81f57b8180919e72ff7e4298709fb8ff69dc277fa35f463`.
The PSO examples at lines 165 and 172 end with
`od=7fff/7fdf;ac=3f;ar=001;ak=4;mc=00000a00;mh=00000a00;nr=0a`.

The strongest source-backed candidate is an unregistered retired address:
releaseMetadata selects metadata interception from a shared vtable, whereas
releaseComplete starts false and can become true only through an existing
PipelineRecord or RootCreationRecord with a complete saved certificate containing
the releasing address. An object created before observation or through another
creation path can therefore reach returned-zero retirement without a certificate.
Its raw address may still be known and disjoint, which fits the healthy od
footprint/intersection checks and resolution-only reentry mark in this evidence.

This is a hypothesis, not a unique decoding. Unsupported/uncertified creation,
native destruction activity and a child's historical ambiguity can also make
releaseComplete false while the final parent footprint is healthy. The old
suffix cannot choose among these, or prove that all ten children failed. Only
at least one failed; D-127's title saying all ten refuse is not proven by nr.
The new reason counters provide that missing discriminator.

## No untracked-retirement correction

No acceptance expansion is justified. An unregistered raw pointer is not a
complete COM identity or alias enumeration. In particular the PSO output is not
yet available during native creation; a post-return raw-address comparison
cannot establish every alias of that PSO, its explicit root, their ancestors,
or a retained metadata owner. After returned-zero, querying the destroyed
object would be invalid. Querying before Release would add native reentry and
still would not certify all destruction side effects. Absence from the proxy's
maps proves absence of registration, not absence of aliases or metadata effects.

The smallest evidence-backed alternative is this diagnostic-only refusal
breakdown, followed by identifying a missing creation/identity observation if
unregistered retirements are confirmed. Any later correction needs a complete
pre-destruction certificate and proof of disjointness, or an independently
justified narrower registration path. Unknown identity remains refused. No
ambiguity flag is cleared or suppressed by this job.

## Append-only encoding

After the unchanged existing suffix through nr, creation diagnostics append:

    ;nf=0003;nc=01010000000000000000000000;np=00

nf is a sticky OR mask; nc has thirteen two-hex-digit saturating counts in
ascending bit order; np counts children that passed every resolution clause.
Each reason counts a child once even when multiple keys or peers exhibit it.
Counts saturate at ff. Reasons are independent and may overlap (for example,
an unregistered child is also incomplete). These are counts, not per-child IDs
or a correlation table. No addresses, lifetime tokens or thread IDs are logged.

| Bit | Refusal clause |
| --- | --- |
| 0 | Child not retained/completed, unknown footprint, or incomplete retirement certificate |
| 1 | No registered pipeline/root creation record for the releasing address |
| 2 | Unsupported/unavailable PSO creation identity, or releasing address absent from saved identity keys |
| 3 | A retirement key owns a registered heap, resource or command-list binding |
| 4 | Metadata-event serial changed during native Release |
| 5 | Child certificate overflow, or saturated parent nested-child count |
| 6 | Child key intersects any parent key, including PSO/root aliases and dependencies |
| 7 | Child key intersects retained metadata or binding object/dependency/heap keys |
| 8 | Child's own key, binding or capacity ambiguity (constructor-only reentry excepted as before) |
| 9 | Unknown retained metadata/binding peer |
| 10 | Missing retained history at the parent's begin clock |
| 11 | Parent/child serial unavailable or cohort clock exhausted |
| 12 | Parent address/footprint unknown or overflowing |

A saved certificate that is merely uncertified does not by itself assert bit 2:
it sets bit 0 unless a specific identity-shape/address check also fails. Bit 1
describes map registration, not an inference about when or where creation occurred.
Registered list final Releases retain their existing separate path and do not
become metadata children; bit 3 identifies ownership detected in a metadata
child's saved keys. Native activity provenance is sampled before the retirement
event itself changes the serial.

Common parent/history/clock failures are counted for each audited child. An
unfinished or overwritten child gets bit 0 and any known common failures;
unavailable child-specific reasons are never invented. At count saturation,
retained evidence may still contribute flags, but counts cannot recover the
number beyond ff. Resolution is a single snapshot, so re-entry into the resolver
cannot double-count or turn an earlier failed decision into success.

All eligibility conjunctions remain required. Existing distinct/order checks,
prefix, field order and quotas remain. All scans and provenance writes use
bounded storage at creation/retirement. Draw only formats the stored detail into
its existing context; its context array grows to 256 bytes without adding a
scan. Detail grows from 97 to 141 characters (160-byte arrays), the PSO summary
uses 224 bytes, and full worst-case lifetime refusal still fits the existing
384-byte diagnostic text and 512-byte log record. Disk quotas stay unchanged.

## Fixtures and validation

Extended the existing diagnostics_offline target, with no CMake edits:

- Each reason bit/count, the passed count and one-time snapshot behavior;
  unfinished/missing certificates, overflow, parent conflicts, retained metadata,
  binding object/dependency/heap conflicts, unknown peers, lost history,
  unavailable serials, exhausted clock, parent incompleteness and saturation.
- Child key/binding/capacity ambiguity provenance; conflict and unknown peers
  completed before the child began, retained across the parent's interval.
- Actual hooked final Release during fake PSO creation: a certified independent
  root passes and reaches missing-t10 requirements, while unregistered,
  malformed-certificate, binding-alias and native-activity cases retain the
  explicit-lifetime refusal with exact nf/nc/np assertions. Existing assertions
  still require requirements_proven=false and preserve Draw allocation/I/O traps.
- Maximum suffix fields and full worst-case refusal capacity, preserving all
  previous prefix/field assertions with the updated append-only length.

These are authored assertions, not executed native results. The standalone
`node scripts/verify-offline.mjs` failed before native compilation:
`spawnSync %USERPROFILE%/AppData/Local/Programs/Python/Python313/python.exe EPERM`.
No native build or CTest ran; worker exclusions diagnostics_activation_offline
and xinput_proxy_offline were not run. `git diff --check` passed. A read-only
comparison against HEAD confirmed distinct(), orderConditions(), observeDraw()
and both hooked Draw wrappers unchanged. (The first comparison used incorrect
wrapper names and stopped after checking the first three functions; the corrected
five-function comparison passed.) Independent text-bound calculation gave
141 detail, 192 context and 364 complete-refusal characters, all fitting their
arrays with NUL. Programme status
via `pwsh -NoProfile -File tools/jobboard.ps1 multi status` failed because this
checkout has no .mcp.json; scoped board claim/heartbeat succeeded. No control
files, stale hydrated build cache or unrelated mcp.launch.json were changed.

## Done when / handoff

Current state: authoring ready for executor pinning and independent Claude
review. Ownership: worker reports through complete_job and stops; no resources
for runtime work were acquired. User input: none. Decision boundary: no
untracked-retirement exception is proposed for approval.

Done for merge only after the executor's native build, full CTest suite and
offline integration gate pass, followed by exact-pin independent review. The
known C1083 d3d12.h environment fault does not waive validation. Exact commands,
separately in an offline cmd.exe executor session using the existing Ninja cache
and disconnected dependencies:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --build build-ninja-integration
ctest --test-dir build-ninja-integration -C Release --output-on-failure --no-tests=error
node scripts/verify-offline.mjs
```

Cheapest first acceptance check after building:
`ctest --test-dir build-ninja-integration -C Release -R "^diagnostics_offline$" --output-on-failure`.
Reviewer gate: `node scripts/verify-offline.mjs --review`.

Next command: executor `cmake --build build-ninja-integration` after vcvars64;
then the focused check and full suite above. Remaining runtime facts: actual
retired identities, their certificate failures, eligibility and all capture,
GPU replay, stereo and VR behavior remain unproven. Only a separately authorized
live campaign could collect nf/nc/np: bit 1 with bit 0 would support the missing
registration hypothesis; other bits identify the next proof gap. This report
grants no live action. Recorded by Codex, 2026-10-21.
