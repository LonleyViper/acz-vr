# Defensive offline VIF/GIF inspection (J-183)

This tool inspects an **already-extracted block** from a caller-selected VIF1
start offset. It does not discover ACM headers, unpack archives, traverse DMA
chains, execute VU code, or access the game. All offsets are input byte offsets,
never runtime addresses. Synthetic tests establish internal framing and export
behaviour; they do not establish ACM layout, engine semantics or runtime parity.

## Files and evidence boundary

- [decoder.py](../../research/acz-arc-mpl/decoder.py): Python standard-library
  decoder, annotations and loss-aware exporter.
- [test_decoder.py](../../research/acz-arc-mpl/test_decoder.py): deterministic
  synthetic binary fixture builders and 28 test methods. No real game inputs.
- [acmscan.py](../../research/acz-arc-mpl/acmscan.py): existing heuristic scanner,
  inspected but neither executed nor modified. Its V3-32 position selection,
  hard-coded start and triangle-strip faces are **not evidence** for this tool.
- `render.py` and `camera-hud-plan.md` were inspected for scoped context. No
  source handoff exists in the two campaign directories. The plan cites a
  handoff elsewhere; that file was not opened because J-183 restricts handoff
  discovery to the campaign directories. The inherited untracked decoder draft
  was completed here; it is implementation input, not protocol authority.

No extracted-fixture provenance, external protocol implementation or historical
triage evidence was used. No downloads, installs, extraction, runtime operations
or original binary changes were performed. Unvalidated framing corner cases
listed below stop explicitly rather than adopting guessed semantics.

## Usage

Requires an existing Python 3 interpreter; no dependencies. The caller must
already know the stream start and provide a block whose byte coordinates also
express the DIRECT qword alignment. This job used synthetic bytes only.

```powershell
python -I -B research/acz-arc-mpl/decoder.py <already-extracted-block> --start 0x150 --out <existing-empty-directory>
python -I -B research/acz-arc-mpl/test_decoder.py
```

The CLI creates `model.obj`, `model.mtl` and `model.json` exclusively in the
existing empty output directory. It refuses a populated directory. Exit 0
means supported framing finished, 2 means argument failure or a preserved
incomplete decode (inspect stderr and the sidecar). Input limits are 16 MiB
and 65,536 shared inspection records: VIF commands, UNPACK source vectors,
GIF tags and GIF register observations. These are inspection bounds, not
claims about format limits. The complete JSON sidecar contains raw input
bytes in coverage records, including rejected and opaque regions.

## Framing contract

`decode_vif(data, start=0)` returns schema `acz-vif-gif-1`, SHA-256, input size,
stream offset, command records, final tracked state, issues and byte coverage.
Start must be a multiple of four in range; the preceding bytes are an opaque
prefix. An input ending at start is valid and contains no commands.

Each coverage record has `offset`, exclusive `end`, `kind`, `raw_hex` and an
optional reason. Top-level coverage partitions **every input byte exactly
once**. GIF coverage separately partitions its enclosing DIRECT payload in
the same absolute block coordinates; do not concatenate nested coverage with
top-level coverage. Original bytes can be reconstructed by concatenating
top-level `raw_hex` in order. Code words and payloads are also retained on
command records for convenient inspection.

`complete` means that inspection reached the end using supported framing and
within the record budget. It does **not** mean every register is understood,
the stream is valid for hardware, or an ACM mesh was recovered. `framed` has
the same narrow meaning for one VIF command. Known-length opaque microcode,
images and GS writes can occur in a complete result. Rejected commands and
unknown-length tails set `complete=false` and have an offset/reason. After
an unknown-length command, parsing stops without scanning for a plausible
next opcode. No heuristic resynchronization is performed.

### VIF

The decoder recognizes NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK,
FLUSHE, FLUSH, FLUSHA, MSCAL, MSCALF, MSCNT, STMASK, STROW, STCOL, MPG, DIRECT,
DIRECTHL and all UNPACK command encodings. IRQ, immediate and NUM fields are
preserved. Recognizing a command does not simulate its execution or validate
all reserved bits.

- STMASK consumes four bytes; STROW/STCOL consume sixteen. Their values and
  raw STMOD mode are tracked with per-command state snapshots. OFFSET/BASE,
  ITOP and execution/flush commands retain their fields without pretending to
  execute VIF/VU effects. STMOD=3 is explicitly marked unresolved.
- MPG consumes `8 * (NUM or 256)` bytes as opaque microcode.
- Supported UNPACK forms are S/V2/V3/V4 with 32/16/8-bit lanes, plus V4-5
  (RGB5A1 source bits in sixteen bits). Reserved 5-bit forms stop. NUM=0 is
  represented as 256. For nonzero STCYCL fields, skip mode consumes NUM source
  vectors; fill mode consumes `floor(NUM/WL)*CL + min(NUM%WL, CL)` vectors.
  Payload end rounds up to four bytes, with padding retained separately.
- Zero STCYCL fields are kept raw. A subsequent UNPACK is rejected with an
  unknown tail: zero-field cycle semantics are intentionally unsupported
  pending independent protocol validation. No zero-to-256 substitution is
  silently made for these fields.
- 8/16-bit lanes use the UNPACK unsigned flag for signedness. 32-bit lanes
  remain raw unsigned bit patterns, with `lane_encoding` stating that fact.
  Float interpretation is an explicit caller declaration, never a magnitude
  test. V4-5 extracts three five-bit fields and one single-bit field without
  normalizing them.
- Source vectors are not destination VU memory: masking, STMOD, row/column
  substitution, cycle destination placement, TOPS and VU execution remain
  unresolved. No positions, normals or triangles are inferred from UNPACK.
- DIRECT/DIRECTHL consume `IMM*16` bytes for nonzero IMM and recursively frame
  GIF data. This implementation requires the payload immediately following
  the code to be qword-aligned in supplied block coordinates. Misalignment
  stops with an unknown tail; it never guesses transport padding. Zero IMM
  also stops as an unsupported count encoding pending protocol validation.
- A malformed nested GIF packet does not erase the known enclosing DIRECT
  boundary: VIF framing may resume after that transfer, with overall
  `complete=false`. Truncated VIF payload/padding consumes the remaining
  bytes as rejected and stops. All raw bytes survive these outcomes.
- If the shared inspection budget cannot accommodate an UNPACK's vectors,
  its known-length payload remains raw with status `framed_opaque`, an issue
  is emitted and the next known VIF boundary can still be inspected. Budget
  exhaustion at a command/tag boundary preserves the rest as an unknown tail.

### GIF and topology evidence

`decode_gif(data, base=0)` can also inspect a standalone GIF stream. GIFtag
NLOOP, EOP, PRE, PRIM, FLG and register descriptors survive. NREG=0 means sixteen
descriptors. PACKED consumes sixteen bytes per register; REGLIST consumes
eight and preserves its qword-end padding; IMAGE/IMAGE2 consume NLOOP qwords
as opaque data. EOP is recorded, not treated as the end of a DIRECT transfer:
multiple packets/tags are allowed. A truncated tag/payload stops within that
bounded transfer, preserving all remaining bytes with the rejection reason.

Decoded observations include PRIM, RGBA bytes, Q/ST bit patterns, UV fixed
10.4 texel coordinates and XYZ2/XYZF2/XYZ3/XYZF3 GS coordinates. PACKED
component layout differs from ordinary 64-bit REGLIST and A+D GS writes;
those paths are tested separately. PACKED XYZ ADC suppression and non-kicking
XYZ3/XYZF3 are recorded. Unknown/reserved registers and unimplemented GS
effects retain raw bytes plus `unresolved`; REGLIST A+D is explicitly invalid.

PACKED PRE and explicit PRIM writes supply only the primitive-type field:
point, line, line strip, triangle, triangle strip, triangle fan, sprite or
reserved. `prim_type_field` is a register observation with
`topology_evidence`, **not assembled topology**. State carries across tags
inside one transfer; state before each separate DIRECT is unknown. PRMODE,
GS contexts, vertex queues, clipping and texture state are not evaluated.
XYZ is GS screen-space XY/integer depth, not model space. ST/Q and UV are
not normalized model UVs. No faces are exported even when PRIM says triangle.

## Explicit source-lane annotations and export loss

Without declarations the OBJ contains comments and an MTL reference only.
`--declarations <json-file>` optionally accepts an array such as:

```json
[
  {"offset": 0, "role": "position", "storage": "float32", "evidence": "synthetic fixture defines V3 float32 source lanes"}
]
```

Each declaration must identify a complete supported UNPACK at the exact
command offset, specify `integer` or `float32`, and give a nonempty evidence
label. Roles are position/normal (three components), uv (two), colour (three
or four). Duplicate offsets, wrong widths, incomplete commands, nonfinite
floats and malformed declarations are rejected. These are **unverified
caller assertions about source lanes**, recorded as such in the sidecar;
the tool does not verify the cited evidence. They are not assertions that
VU memory or engine attributes have been reconstructed.

Position, normal and UV declarations write separate OBJ objects with `v`,
`vn` and `vt` records. There are no inferred attribute associations, index
buffers, faces, UV normalization, colour normalization or material bindings.
Colours remain in JSON. `model.mtl` has an unresolved placeholder material.
JSON preserves the full decode, raw unknown/rejected data, all declarations,
colours, evidence labels and an explicit export-loss list. Raw command and
GS state semantics omitted by OBJ therefore remain recoverable. Output is
deterministic for identical input, start and ordered declarations; float
text uses nine significant digits and JSON disallows nonfinite values.

## Verification and review handoff

Final focused command: `python -I -B research/acz-arc-mpl/test_decoder.py`.
28 test methods pass. Fixture coverage includes command vocabulary, state
snapshots, all supported UNPACK widths/masks, signed and raw lanes, NUM=0,
fill/skip source lengths, alignment/padding, explicit unsupported zero
encodings, unknown commands, GIF modes/register layouts/PRIM/ADC, malformed
lengths, shared record limits, declarations, stable export and CLI exit codes
and refusal to overwrite. The deterministic sweep adds 82 prefix truncations,
240 single-byte mutations and 128 seeded random inputs through both VIF and
GIF parsers. Every sweep checks exact contiguous raw-byte coverage and JSON
serializability. These are synthetic consistency checks, not independent
protocol conformance tests.

`node scripts/verify-offline.mjs` exited 1 before native verification:
`Native DLL gate needs the cached OpenXR SDK; set ACZ_VR_OPENXR_SOURCE. No fetch attempted.`
This is a missing local verifier prerequisite, not a gate pass. Nothing was
installed and no SDK path was guessed. The required programme-state command
`pwsh -NoProfile -File tools/jobboard.ps1 multi status` also exited 1 because
this worker checkout lacks `.mcp.json`; the jobboard MCP claim and heartbeat
succeeded during the original implementation. The review repair removes the
accidentally included `mcp.launch.json` from this branch; no replacement control
configuration is added.

Ownership: J-183, branch `codex/job-j-183-acz-arc-mpl`; worker does not commit.
Only the decoder, its synthetic tests and this document are deliverables.
The accidentally included `mcp.launch.json` is removed by this review repair.
Mandatory Claude-family
review remains. Minimal read set: these three deliverables. Next command:
`python -I -B research/acz-arc-mpl/test_decoder.py`. Review the conservative
framing limits, exact raw coverage and absence of inferred model topology.
Independent protocol validation, real extracted fixtures with provenance and
engine/VU interpretation remain future work; none is needed to run these
synthetic checks and none is claimed by this job.

Review repair verification (2026-10-26):

- `python -I -B research/acz-arc-mpl/test_decoder.py`: exit 0, all 28 tests
  passed (0.235 s); decoder and tests unchanged by this repair.
- `node scripts/verify-offline.mjs`: exit 1, the same missing cached OpenXR SDK
  prerequisite quoted above; no fetch attempted.
- `pwsh -NoProfile -File tools/jobboard.ps1 multi status`: exit 1, missing
  `.mcp.json`; no replacement configuration created.
- `git diff --check`: exit 0.

The dispatcher must commit the tracked `mcp.launch.json` deletion together with
this documentation correction so the reviewed branch contains only the three
approved deliverables. The worker has not committed or rewritten branch history.
