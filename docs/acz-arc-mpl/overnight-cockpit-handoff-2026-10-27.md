# Offline cockpit work: 27 October handoff

## Outcome

The F-16-related `f16ck.mpl` cockpit can be inspected in a local, approximate
viewer with provisional radar and stores/status overlays on separate lower
console candidates. The instrument mapping is a mock, not a native game port.
The `itesu` cockpit-like geometry has a separate viewer and separate candidate
surfaces; a static datapack group links `itesu` to J35J-labelled assets, so
do not treat it as an established F-16 model. Model binding, runtime aircraft
identity, camera placement and live radar/weapon source routing remain open.

Review the local F-16 mock at
`E:\ACZ-ARC-Scratch\f16ck-nearby-geometry-study-2026-10-27\inspect-cockpit.html`.
The renderer uses provisional strip assembly, texture binding and painter
sorting. Optional geometry hiding is a diagnostic and must not be copied into
a runtime suppression rule. `f16ck-console-surface-lead.md` describes candidate
planes and overlapping geometry; `itesu-cockpit-atlas-findings.md` describes
the separate model and identity evidence. These are extracted third-party game
assets in scratch, not redistributable project assets.

## Alpha and safety policy

Existing capture evidence contains colored HUD pixels with zero stored alpha.
The offline tests model the current consumer's maximum-color coverage policy;
the measured composites are diagnostics rather than proof of live behavior.
`cockpit-display-runtime-contract.md` and `panel_routing_reference.py` describe
the fail-closed policy: leave the entire original canopy HUD intact unless all
required panels, marker/radar separation, frame/depth and both eye outputs are
validated and completed. No GPU implementation is supplied by this reference.

## Captured geometry evidence

The read-only E10 snapshot survey tracks 30 indexed draws from four selected
cockpit PSOs. Their bound topology and pipeline strip-cut settings are recorded;
the saved index buffers contain many four-index strips, which are insufficient
to identify a display. Saved upload data may have changed after the draw. Exact
source-corner searches in two pinned capture buffers found no matches, which
does not rule out transformed or differently packed geometry. No aircraft or
source-model identity has been established in that capture.

## Validation and next discriminating work

On 27 October, 57 offline Python tests, the viewer picking test, and
`node scripts/verify-offline.mjs --review` passed. These checks do not prove VR.
All work was offline; no game launch, process attachment, injection or headset
operation was performed. Source and generated files remain uncommitted; do not
overwrite unrelated work, including `docs/acz-arc-mpl/campaign.json`.

1. Establish a trustworthy aircraft/model-to-runtime-draw correspondence and
   the relevant camera transform from permitted evidence before selecting a
   surface. The existing snapshot is not a draw-time buffer guarantee.
2. Establish independently separable, complete radar, stores and marker data;
   rectangle crops and static sample content do not prove this.
3. In a separately authorized runtime campaign, implement and validate the
   renderer, frame ownership, depth and both headset eye projections. Keep the
   original HUD available on any failed requirement.
