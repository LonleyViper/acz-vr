# ACZ cockpit-camera and HUD analysis plan (J-185)

Status: offline planning only. No fixture bytes were read, so this document
contains no byte observations. No runtime claim is made. File offsets, where
cited later, are file offsets and never runtime addresses.

## Done when

This plan is done when each of the five leads has required inputs, a bounded
read-only comparison, discriminating observations, limits and a stop
condition (below), and the missing-input blocker is recorded. Executing the
plan is a later job and needs the fixtures in B1.

## Evidence

### Direct inputs actually present (hashes computed this run)

| Path | SHA-256 | Role |
|---|---|---|
| `docs/handoffs/2026-10-26-acz-arc-mpl-cockpit-geometry.md` | not hashed (text handoff) | source of every claim below |
| `research/acz-arc-mpl/acmscan.py` (1,605 B) | `2a93c090cc0e376fe9f8b1ab8c8d9664cd1a58e465ff4d12fa90197d29d211aa` | reads `0x68` V3-32 batches only; faces unchecked |
| `research/acz-arc-mpl/render.py` (748 B) | `0c25be61e9f42eb4c52eb8646731c80373db8a2acf4b4a4dd88cb32df83ade21` | plotting only, needs `.npy` outputs not in repo |
| `research/acz-arc-mpl/cockpit_compare.png` (100,258 B) | `0ff23d63d87d841a642813d552012edb7a89ef32ae9fd3ab22679b27bed87bb5` | derived render, not a fixture; not interpreted here |

### B1. Missing-input blocker (precise)

The handoff records only the game archive locations and sizes
(`D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\a.arc` 479,214,608 B,
etc.) and says extraction went to an unrecorded scratch directory. It records
no path, size or hash for any extracted `.mpl`, `.acm`, `.ahm`, `.fhm` or
`.atd` file. It points to `DECISIONS.md` line 545 for `.arc` hashes (state E1);
that file was not in the permitted read set and archive hashes are not
fixture hashes anyway. The repo contains no extracted fixtures. Historical
evidence (aczero-triage, vr-feasibility) is context only and was not used as
fixture provenance.

Required to unblock, for each file used: absolute local path, size, SHA-256,
and the archive+entry it came from (with the archive's recorded hash). Files
needed:

- `data/plmodel/itesu.mpl` (handoff: 1,852,128 B) and `f16ck.mpl`, plus at
  least two other aircraft `.mpl` (suggest `a10a`, `f01a`, named in the
  handoff for `.atd`).
- `data/hud/pldata/a10a.atd`, `f01a.atd` and ≥2 more; `data/hud/huddataN.fhm`
  (handoff: 373,168 B, which N is unrecorded).

Until then everything below is conditional.

### Handoff claims (secondhand, unverified by this job)

Treat as hypotheses H0, with the source being the handoff only:
`.mpl` = u32 part count (10) + offsets; parts 0-3,5-7 ACM, 4 AHM, 8-9 GIM;
ACM header `ACM\0`, version 5, 64-byte records at offset 80, `80 + A*64 ==
offset B`; `.atd` is 4 B per aircraft; `.fhm` is a container; itesu part 2 has
23,910 vertices; itesu parts 0/1 are about 179x19x19 and 94-unit cubes.

## Plan per question

Common rules: read-only, one file at a time, Python with `-I`, standard
library `struct` only (no archive tooling, no extraction, no decoder work).
Every observation is logged as `path / sha256 / offset / length`. Each step
first re-verifies the file hash against B1; mismatch is a stop.

### Q1. ACM 64-byte records across aircraft

- Inputs: the ACM parts of ≥3 aircraft `.mpl` (B1). Part offsets from the
  u32 table at file start; ACM block bounds are part table entries.
- Comparison: for each ACM part, read the header (the first 80 bytes), check
  `ACM\0`, version, and `80 + A*64 == offset B`; dump the A records of 64 B
  as 16 u32 and as 16 f32. Column-wise, mark columns constant across all
  aircraft and parts, columns varying per part but not per aircraft, and
  columns varying per aircraft.
- Discriminating observations: a column group of 3 finite floats that is
  zero/constant in all but part 2 (cockpit-ish) or that varies per aircraft
  and lies within the part's bounding region. Integer columns that look like
  offsets/counts into the VIF stream (checkable against block length).
- Limits: a position-like float group is a hypothesis only; no unit, frame or
  eye-point identity follows from a column pattern.
- Stop: after the column table is written for all selected parts, or at the
  first header that violates the H0 layout (report, do not guess).

### Q2. AHM part 4 across aircraft

- Inputs: part 4 of the same `.mpl` files.
- Comparison: confirm `AHM` tag; hexdump header; byte-wise and u32-wise diff
  between aircraft; list contiguous float-plausible runs (finite, |x|<5000)
  and report which differ per aircraft.
- Discriminating: small float groups differing per aircraft and not
  equal to anything in the ACM records (Q1), versus a mostly identical block
  (then it is likely shared structure/skeleton, not a per-aircraft camera).
- Limits: the handoff's search window (x≈0, y≈2-4, z≈-30 on F-16C) is a
  prior to test, not evidence, and must not be used to select a match
  without an independent constraint; report all candidate runs.
- Stop: diff finished for ≥3 aircraft; or part 4 has no float-plausible run
  (negative result is recorded as such).

### Q3. itesu parts 0 and 1

- Inputs: `itesu.mpl` and one normal aircraft `.mpl` for contrast.
- Comparison: tag and header of parts 0 and 1; sizes; whether they parse as
  ACM; with a read-only vertex-batch scan (only `0x68` batches, as
  `acmscan.py` does, run against a copy of the logic, not modified) compute
  bounding boxes, and compare with the same parts of the normal aircraft.
- Discriminating: whether parts 0/1 resemble the body/part-1 roles of normal
  aircraft (same record structure, similar relative sizes) or differ in tag.
- Limits: the bounding-box dimensions in the handoff are not confirmed; bbox
  identifies shape extents, not identity (cockpit/canopy/other). Naming it a
  cockpit interior needs independent evidence (e.g. textures), not done here.
- Stop: bboxes and header comparison recorded for parts 0/1; no OBJ/face work.

### Q4. FHM classification

- Inputs: one `huddataN.fhm` (handoff: 373,168 B for one).
- Comparison: first 64 bytes, magic, whether early u32s form a monotone
  offset table ending at ≤ file size, then the tag bytes at each offset
  (`ACM`/`AHM`/`GIM`/other). Entry count and sizes.
- Discriminating: a valid offset table with recognizable sub-tags classifies
  it as a container of those types; otherwise report "unclassified".
- Limits: parsing a table does not give HUD semantics or layout. No
  sub-entry decoding.
- Stop: top-level table classified or shown not to be a table.

### Q5. ATD classification

- Inputs: ≥4 `.atd` files (handoff: 4 bytes each; `a10a`=`30000000`, `f01a`
  =`13000f0e` hex unverified).
- Comparison: dump each file's 4 bytes as u8×4, u16×2, u32; tabulate across
  aircraft; check whether any field correlates with something in Q1/Q2 only if
  that comparison is stated before looking.
- Discriminating: whether bytes act as bit flags (few distinct bits), small
  indexes (values < part/entry counts) or opaque IDs.
- Limits: with 4 bytes per aircraft and few samples the meaning stays a
  hypothesis; a correlation among a handful of aircraft is weak. No claim
  about HUD rendering.
- Stop: table written; classification limited to "flags / index / opaque".

## Unresolved engine facts

All of these stay open regardless of file analysis:

- Whether any value found is the cockpit camera/eye point, and its units,
  frame and handedness; how the runtime computes the view.
- Any mapping between file data and runtime buffers, CPT/ACM draws or
  addresses (file offsets are not runtime addresses).
- What the engine does with ATD/FHM at runtime; HUD layout, depth and scaling.
- Whether `data/cam/*.bin` is related to in-flight camera (handoff: scripted
  cameras, unverified).
- Whether `d.arc` or mod overrides change these files at runtime.

## Next authorized experiment

Not run by this job. Needs a separate job whose inputs are the B1 files with
path/size/SHA-256/source-entry provenance (supplied by whoever holds the
extracted scratch directory, or an authorized extraction job). Then run
Q1-Q5 in that order, read-only, writing results to a docs-only report. It
must not wait on, or reuse, J-183 decoder tooling and leaves f16ck part 2 /
itesu part 2 decoding to J-184. If the fixtures are not supplied, stop at B1.

## Checks

`node scripts/verify-offline.mjs` was run twice as a standalone command. Both
runs exited 1 with `Native DLL check failed ... cmake.exe --build ... ETIMEDOUT`:
the verifier's own native-build step hit its internal spawn timeout while
compiling the (unrelated) C++ test targets. The second run was incremental
and had compiled most targets before timing out. Neither run reached a pass
or a failure of any check, so the verifier result is **inconclusive, not a
pass**. The timeout is not attributable to this document (the only change is
this markdown file). No fixture-based test could run, since no fixtures
exist. Nothing was installed.
