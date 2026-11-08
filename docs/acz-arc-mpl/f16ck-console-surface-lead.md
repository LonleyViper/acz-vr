# F-16C console geometry lead

Status: offline source-lane and provisional-preview evidence; no runtime profile
or playable cockpit has been established.

In the hash-pinned `f16ck.mpl` part 2, two adjacent four-vertex batches form
approximately mirrored planar candidate regions in the lower instrument area:

| ACM batch offset | Source X | Source Y | Source Z | Part-9 candidate UV rectangle |
| --- | --- | --- | --- | --- |
| `0x191e0` | `-1.033425..-0.510175` | `2.147403..2.645993` | `-35.709671..-35.576073` | `u=.246094..285156`, `v=.933594..972656` |
| `0x19284` | `0.510168..1.033418` | `2.147407..2.645998` | `-35.709652..-35.576054` | `u=.250526..280724`, `v=.935119..971133` |

These are position/UV source tuples from five-UNPACK batches, not verified
triangles or runtime draw calls. Their appearance in the provisional viewer
is consistent with lower-panel geometry, but the part-9 texture binding,
viewing direction, exact function and overlapping components are unresolved.
The candidate IDs deliberately describe source-X sign, not aircraft left/right.

The second UV rectangle lies inside the first. **If** both batches use the
same part-9 texture, changing those atlas pixels would affect both regions.
That rules out assuming that a single in-place atlas edit can display distinct
radar and weapons information. The candidate per-aircraft route instead needs
separately authenticated screen draws or independently transformed panel
surfaces, with per-eye depth and HUD safety gates.

An explicitly opted-in **offline mock** now demonstrates that approach:
`export_itesu_preview.py --model f16ck --hud-preview <recorded-png>
--experimental-f16-screen-mock` places different recorded samples on batch
`0x191e0` and batch `0x19284` without changing their shared source atlas. The
radar fills its candidate panel; the weapons/status sample is inset. In the
down-looking preview, additional original geometry/text overlaps the display,
which makes draw order, depth, and static-instrument masking unresolved work.
The assignment is arbitrary for the mock and is not a claim about actual
aircraft sides or instrument functions. The preview is single-view CPU canvas,
uses an opaque 8-bit diagnostic, and is neither a GPU port nor live data.

The bounds and source hashes are recorded in
`research/acz-arc-mpl/cockpit-profiles/f16ck-display-surface-candidates.json`.
Run the shared hash-checking inspector with `--candidates` pointing there. The
local viewer can click a projected triangle to report its source batch offset,
UV and source-position bounds. Picking uses painter order and nearest-pixel
atlas alpha, so it is an exploratory aid rather than a runtime draw match.

The still-unassigned rectangles are useful calibration targets, not an F-16C
radar/stores profile. Static texture text and other overlapping geometry must be
classified before assigning the real display functions.

## Nearby geometry and occlusion probe

`find_coplanar_batches.py` checks all source vertices of each other batch
against a candidate screen's plane and rectangle. At a plane tolerance of
0.004 source units and 2% edge margin, it found one other full-screen batch
on each candidate: `0x8ed4` and `0x8f78`. At a deliberately wider tolerance
of 0.012 units, the counts rise to six and eight, respectively. The positive-X
candidate includes seven small strips (`0x31c0`, `0x3264`, `0x3308`, `0x33ac`,
`0x3598`, `0x363c`, `0x36e0`) around 0.0082–0.0086 source units from the
plane; several use the same 128×128-atlas colour/text band. The negative-X
candidate has five additional nearby batches with different UVs.

The opt-in mock viewer can hide these 14 geometrically nearby batches to
compare the result. That removes some visible overlapping markings in the
static preview, but **proximity does not establish their purpose, final draw
order, or safe removability**. A runtime implementation must never suppress
these batches by these offsets without authenticating the actual draw,
material and required cockpit details. Partially intersecting geometry is not
covered by the survey.

## Comparison with an existing D3D12 capture

The original E10 `frame11906.zip` matched the recorded SHA-256. Two existing
12,582,912-byte blob copies were separately hashed and searched without
modification. For each candidate panel, all four unscaled little-endian
float32 XYZ byte sequences were searched throughout each blob. No exact
sequence was found for either `f16ck` or `itesu` (eight corner sequences per
model per blob). Blob `000335` is the saved resource-329 upload buffer noted
in `stored-draw-manifest-2.md`; blob `000293` was also scanned, without
assigning it a draw identity here.

This negative result rules out only literal contiguous XYZ bytes in these two
saved blobs. It does **not** exclude model-space transforms, precision
conversion, other capture resources, culling, different aircraft or other
frames. It cannot justify enabling the runtime profile. The bounded,
hash-pinned search is `search_capture_panel_vertices.py`; full output remains
in the local scratch directory.

The saved XML can also be surveyed without loading its pickle files or
replaying the capture. `survey_capture_cpt_vertices.py` tracks command-list
reset, selected pipeline and slot-0 vertex views for the four pinned E10
DrawCPT PSOs, then checks each bounded view in resource 329. For this capture
it finds 30 draw records with valid 64-byte-stride views and 18 distinct view
byte hashes. The views include chunks `2983` through `3096`, whose bounds
are in the local report `e10-cpt-vertex-views-2026-10-27.json`. These are
selected observed views, not all cockpit draws, and each view can include
vertices that a particular indexed draw never references. The saved upload is
an end-of-recording snapshot; content at individual draw times is unproven.
The mismatch with the candidate's exact source bytes does not by itself
identify a transform or aircraft.

```powershell
python -I research/acz-arc-mpl/find_coplanar_batches.py E:\ACZ-ARC-Scratch\extracted-a\a\data\plmodel\f16ck.mpl --candidates research\acz-arc-mpl\cockpit-profiles\f16ck-display-surface-candidates.json --distance 0.012 --out E:\ACZ-ARC-Scratch\f16ck-panel-nearby-0012-2026-10-27.json
```
# Indexed capture snapshot follow-up

The offline E10 survey now also tracks the command list's index-buffer binding,
start index and base vertex for the same four selected DrawCPT PSOs. The pinned
index upload snapshot `000336` has SHA-256
`4c8e4f5698edc575682378f0dbf8985d1753ee4e274e65743b456792e92021a1`.
All 30 selected indexed draw records resolve to in-range positions in the
saved buffer snapshots; their actual index-referenced position bounds are in
`E:\ACZ-ARC-Scratch\e10-cpt-indexed-survey-2026-10-27.json`.
The survey observed 1,050 occurrences of the 16-bit `0xffff` value across
those records and excludes them from bounds. The saved pipeline definitions
for all four PSOs specify `D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF`; the
tracked command-list topology at each of the 30 draws is triangle strip. The
updated result is `E:\ACZ-ARC-Scratch\e10-cpt-indexed-strips-2026-10-27.json`.
The saved index snapshots contain 653 four-index strip occurrences across
the 30 records (including repeated draws), so a four-index strip alone is not
enough to identify a console display. The survey now fails closed if the
expected topology or strip-cut mode is absent.
The snapshot can have been overwritten after a draw, and the survey
does not establish aircraft identity, actual rasterized geometry, or a mapping
to either candidate console surface. Reproduce with
`research/acz-arc-mpl/survey_capture_cpt_indexed.py --help`.

