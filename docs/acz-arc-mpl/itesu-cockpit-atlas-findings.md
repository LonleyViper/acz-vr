# `itesu` and F-16C cockpit-atlas finding

Status: offline texture evidence only. This identifies cockpit *texture atlases*;
it does not establish a mesh triangle, UV assignment, camera position, runtime
draw identity or a usable VR cockpit surface.

## Reproducible decode

`research/acz-arc-mpl/gim_inspect.py` reads an already-extracted GIM or an MPL
part directly. It recognises the bounded layout seen in the four candidate
parts:

| Field | Value |
| --- | --- |
| magic | `GIM\\0` |
| image dimensions | little-endian `u16` at `0x1c` and `0x1e` |
| indexed pixels | one 8-bit raster at `0x20` |
| trailing data | 16-byte header, then 256 four-byte BGRA palette entries |

The parser refuses a different size/layout. It emits a direct-palette image and
a PS2 CSM1-style palette-order candidate; it deliberately does not claim a GS
texture-memory swizzle, orientation, alpha blend interpretation or UV mapping.

The read-only commands used against the extracted copies were:

```powershell
python -I research/acz-arc-mpl/gim_inspect.py E:\ACZ-ARC-Scratch\extracted-a\a\data\plmodel\itesu.mpl --mpl-part 9 --out E:\ACZ-ARC-Scratch\gim-inspect-2026-10-26\itesu-part9
python -I research/acz-arc-mpl/gim_inspect.py E:\ACZ-ARC-Scratch\extracted-a\a\data\plmodel\f16ck.mpl --mpl-part 9 --out E:\ACZ-ARC-Scratch\gim-inspect-2026-10-26\f16ck-part9
```

The generated previews are held outside the repository because they are derived
game texture data.

## What the previews establish

The CSM1-style candidate for `itesu.mpl` part 9 is a coherent 512×512 cockpit
instrument atlas: it visibly includes a forward instrument panel, a green
radar-like display, green multifunction-display regions, lower-console panels,
standalone gauges and pilot/cockpit details. This establishes that `itesu`
contains a cockpit-oriented texture asset. It does **not** establish that part
2's dense ACM geometry is the matching cockpit interior.

The 128×128 part-9 candidate in `f16ck.mpl` is also a small cockpit/instrument
atlas. `f16ck` part 8 instead visibly contains the aircraft's exterior UV atlas
(fuselage, tail and aircraft markings) with a small pilot/cockpit island. That
separates the exterior and cockpit-material leads, but still does not provide a
triangle-to-UV association.

| Model / part | Block SHA-256 | Dimensions | Visual classification |
| --- | --- | --- | --- |
| `itesu` / 8 | `1e9bea86554ba1acb70fcde56b7fc8b9b9ee701335de4eba5296be9978b15076` | 512×512 | low-contrast material candidate; no panel assignment |
| `itesu` / 9 | `db68018cb2232ce7ec567da294426470c1d010afbf1964d8954f5391215243c3` | 512×512 | cockpit/instrument atlas |
| `f16ck` / 8 | `af059694ba5b30656e4f6709eb11c74aa5c47e09925a3c63fec2b25cf607cc79` | 512×512 | exterior aircraft atlas plus small cockpit/pilot island |
| `f16ck` / 9 | `f07f64cb2bb906009f16bdd20bc549338f9244d2b9bab81ad66ae8f69de01acc` | 128×128 | cockpit/instrument atlas |

The `itesu` part-9 source is at MPL offset `0x183eb0`, length `0x40430`; its
container SHA-256 is
`614f4e683d34348df80a159fd1156aa21827248145efcef14f549804c17f9a9a`.

## Aircraft-reference lead in the extracted datapack

The extracted `data/datapack_jp.bin` (406,116 bytes, SHA-256
`3da2923be5890bea70bdb3cb1045bf4f86c62bf2c0cd56b725863883a2a3b671`)
contains repeated runs of 17 path records, each 36 bytes, that group an
aircraft's `.pmb`, a model path, `.pvb`, sounds, animation, HUD and other assets.
One such run at file offset `0x4b3c4` begins:

```text
mis/mpl/j35j.pmb
plmodel/itesu.mpl
mis/mpl/j35j.pvb
sound/bank_jet_j35j.p3d
sound/bank_jet_j35j.pbd
sound/jet/J35J.bin
planim/j35j.bin
```

The same run later includes `lodaircraft/j35ju.bin`, `hud/pltex/j35j.gim`
and `hud/pldata/j35j.atd`. Six other `j35j` runs instead name `j35j` or
`pubj35j` models in the same position. Seven corresponding F-16C runs name
F-16 variants or `pubf16ze`; none names `itesu`. This is a **static j35j
association lead**, stronger than the earlier unknown-aircraft description,
but it does not establish that `itesu` is a production J35J cockpit or that
this particular run is selected at runtime. No gameplay/camera identity is
inferred and the cockpit must not be labelled F-16C from this data.

The reproducible bounded scanner is
`research/acz-arc-mpl/inspect_datapack_model_refs.py`. It validates the record
pattern and reports rejected false matches. Example:

```powershell
python -I research/acz-arc-mpl/inspect_datapack_model_refs.py E:\ACZ-ARC-Scratch\extracted-a\a\data\datapack_jp.bin --aircraft j35j --aircraft f16c --aircraft j39c --out E:\ACZ-ARC-Scratch\datapack-model-ref-survey-2026-10-27.json
```

## Direct UV-to-surface candidates

The five-UNPACK batches already preserve adjacent position, normal, colour and
UV source lanes. Two four-vertex batches in `itesu` ACM part 2 select the two
prominent green forward-display rectangles in the part-9 atlas. The source
lanes are real file observations, not assembled triangles:

| Candidate | Batch offset | Atlas UV / 512-pixel rectangle | Position-source bounds |
| --- | --- | --- | --- |
| port forward display | `0x210` | `u=.171569..281244`, `v=.301067..410742`; `(87.84,154.15,56.15,56.15)` | `x=-.997511..-.514495`, `y=2.363941..2.832801`, `z=-35.052044..-34.935966` |
| starboard forward display | `0x16c` | `u=.515002..625250`, `v=.301702..408719`; `(263.68,154.47,56.45,54.79)` | `x=.514495..997511`, `y=2.363941..2.832801`, `z=-35.052044..-34.935966` |

The port rectangle contains the atlas's tactical/radar-like green raster, so
`radar` is an evidence-grounded proposed domain. The distinct starboard green
display is a credible `stores_status` target, but its exact labels have not
been decoded. The machine-readable, hash-pinned record is
`research/acz-arc-mpl/cockpit-profiles/itesu-display-surface-candidates.json`.
It is deliberately **not** a runtime profile: the VIF/VU primitive execution,
texture binding, aircraft selection and stereo calibration remain unproven.

The source-lane geometry has now been re-read directly from the hash-pinned
MPL and texture using `research/acz-arc-mpl/inspect_display_surfaces.py`.
Both four-corner candidates are planar and essentially square (width and height
each about 0.4830 source units). Their centres are `(-0.7560, 2.5984,
-34.9940)` and `(0.7560, 2.5984, -34.9940)`: a matched pair around source
`x=0`. The units, axis orientation relative to the player and runtime transform
remain unknown. The script reports all paired position/UV corners and refuses
the wrong MPL, ACM, texture or UV bounds.

At ACM-part-relative offsets `0x200` and `0x2a4`, immediately after the two
candidate batches, the words are `0x11000000` (VIF FLUSH), `0x17000000`
(MSCNT), `0x01000404` (STCYCL CL=4/WL=4), and `0x01000105` (STCYCL
CL=5/WL=1), followed by the next batch. This establishes VU continuation at
both observed boundaries; the microprogram and the primitive it emits are
still unknown. These are file offsets, not runtime addresses.

For the requested layout, use the port candidate for radar and keep the
weapons/status content smaller *within* the starboard candidate. The two
physical source rectangles have the same size; the smaller appearance comes
from the content inset, not from distorting the model surface. This is a design
proposal until texture binding and in-game orientation are confirmed.

```powershell
python -I research/acz-arc-mpl/inspect_display_surfaces.py E:\ACZ-ARC-Scratch\extracted-a\a\data\plmodel\itesu.mpl --out E:\ACZ-ARC-Scratch\itesu-display-source-lanes-2026-10-26.json
```

The two hash-verified candidate quads also pass conservative full-frustum
projection into **both** eyes of an explicitly synthetic stereo rig with a
0.064-source-unit eye separation. The output is reproducible with
`inspect_stereo_panel_fixture.py` and contains both sets of NDC corners plus
the input matrices. These numerical matrices are fixture values, not recovered
eye positions, headset projections, depth occlusion, or a scale in metres.

## Interactive candidate preview

`export_itesu_preview.py` creates an OBJ/MTL, decoded atlas and a self-contained
HTML viewer outside the repository. The viewer starts with a spatially isolated
panel region and has orbit, zoom, down-looking and full-model controls. The
rendered panel is visually recognisable in an offline browser check, including
the two candidate screens. It still makes explicit, unverified assumptions that
each batch is a triangle strip and all part-2 batches use part 9's texture.
The viewer's camera is an inspection camera, not the game's cockpit eye. Some
faces, texture assignments, clipping and alpha may be wrong. Its apparent left
and right are view-relative: the previous `port`/`starboard` candidate names
are provisional until the game camera orientation is established.

An optional `--hud-preview` argument embeds a recorded 2560×1440 `hud-on-black`
PNG and enables a button that replaces the two candidate atlas rectangles with
a static radar sample and a smaller inset weapons/status sample. This is a
placement mockup only: the image is a diagnostic with opaque PNG alpha, and
the viewer has no live HUD source, cockpit depth test or stereo projection.

```powershell
python -I research/acz-arc-mpl/export_itesu_preview.py E:\ACZ-ARC-Scratch\extracted-a\a\data\plmodel\itesu.mpl --out E:\ACZ-ARC-Scratch\itesu-interactive-panel-2026-10-26
```

The runtime port still requires a proven primitive and binding, an aircraft
selector, frame/eye transforms, depth and dynamic HUD routing; this preview does
not add or enable any of those features.

The same exporter now accepts `--model f16ck`, with its own exact MPL, part-2
and part-9 hashes and its own inspection bounds. A first local down-looking
preview exposes a recognisable, substantially lower-resolution F-16 panel
candidate; the same topology/binding/eye limitations apply. The `itesu` HUD
placement is not applied to `f16ck`. The F-16 viewer has a separately defined,
explicit opt-in, per-batch placement mock using unassigned candidate surfaces;
see `f16ck-console-surface-lead.md`. This does not enable a runtime profile.
A viewer button compares palette alpha as stored with a deliberately
opaque atlas diagnostic. Forcing opacity exposes hidden RGB but is not a
claim about game material alpha and can obscure geometry.

## Display-placement implication

The atlas visibly distinguishes a radar-like screen in the upper forward panel
from lower-console controls and additional right-hand display/gauge clusters.
That supports the intended presentation rule: radar targets a forward display;
stores/status targets a lower or side console; neither should cover the canopy
reticle, ladder or targeting cues. It does not yet identify the appropriate
surface for a particular named aircraft, so it cannot promote a capture-only
placement profile into an `itesu` or F-16C runtime profile.

The next discriminating check is to join the ACM draw/VU output to this atlas:
recover each textured draw's texture-state reference and UV lanes, then test
whether its UV range lands on the visible radar or console rectangles. Only
that result can name an actual mesh surface and make the per-eye/depth runtime
contract usable.
