# Cockpit console display runtime contract

Status: offline implementation contract. It describes a fail-closed route for
placing dynamic information in a replayed cockpit. It is not a claim that the
required per-aircraft surface, texture, depth or runtime model identifier has
been established.

## Desired presentation

The canopy HUD contains flight- and aim-critical information: reticle,
attitude ladder, targeting cues, speed, altitude and heading. Radar and
weapons/stores status are different: they are presented on the aircraft's
instrument consoles. No console crop is also drawn in the canopy layer.

This is selected by an **aircraft display profile**, not by a generic HUD
layout. A profile has one unambiguous aircraft/model selector and evidence
record, plus named display targets (for example `radar_mfd` and
`stores_console`). A model with no validated profile produces the existing
canopy HUD only; it never inherits another aircraft's panels.

`capture-4132-stock-cockpit.json` is a test placement profile, not an F-16C
profile. The `f16ck` and `itesu` GIM findings are potential material leads,
not surface assignments.

## Available image sources

`StereoFrame::hud` is the private, premultiplied game HUD target. The existing
`gpu_copy_step5_hud_radar` producer option extends that target with the
stencil-masked radar-contact replay. It remains evidence-gated: its offline
tests prove the replay rules, not that a live frame has correct contacts.

Despite the target's intended premultiplied convention, its **stored alpha is
not sufficient by itself** for source-over compositing. The existing consumer
`shade_hud` clamps the linear RGB and uses `max(stored alpha, max(shaded RGB))`
for effective coverage. In the recorded run-11 s2 diagnostic sample, sampling
every fourth pixel found stored alpha zero in all 2,134 sampled coloured pixels
in the weapons crop, and in 164 of 1,220 sampled coloured radar-crop pixels.
The diagnostic RGB has been clamped, sRGB encoded and quantized, and the alpha
is quantized independently; this is evidence for that captured frame only, not
the full FP16 data or proof of a live radar-contact capture. Reproduce with
`pwsh -NoProfile -File research/acz-arc-mpl/audit_hud_alpha.ps1`.

Therefore the first console-composition experiment should match the existing
consumer's shaded-RGB/coverage policy in linear space before blending, and
validate it against known pixels and the existing HUD-over-world diagnostic.
Using stored alpha alone with ordinary source-over would produce different
results, particularly for the weapons text. The actual display's backing and
opacity are still an explicit material decision; do not blindly black-key RGB.

The coverage rule was independently checked against the matching captured
`hud-over-world` PNG. Reconstructing each fourth coloured pixel from the
quantized HUD-colour, alpha and world PNGs, the average absolute RGB-channel
error on a 0–255 scale was **0.040 vs 2.432** for radar and **0.311 vs 20.398**
for weapons, comparing `max(alpha, RGB)` with stored alpha alone. The effective
rule was closer at 164 radar and 2,113 weapons samples; stored alpha was closer
at 0 radar and 21 weapons samples. Quantization prevents bit-exact recovery and
ties are possible. This corroborates the current diagnostic's policy for this
frame, not a real headset result. Reproduce with
`pwsh -NoProfile -File research/acz-arc-mpl/compare_hud_alpha_composite.ps1`.

When `hud_markers` is separately valid, target boxes and labels remain canopy
information. The console pass must sample only `hud`; the marker target is
composited only into the canopy pass. Target/friendly markers can appear at
arbitrary screen coordinates, including within a proposed radar or stores crop.
Therefore static protected rectangles alone cannot prove target-cue preservation:
require a complete, separately captured marker layer or equivalent per-draw
semantic separation before masking any instrument crop. A marker-less valid
frame requires an explicit producer verdict that no markers were present; it
cannot be distinguished safely from missing capture by a null texture alone.
Likewise a complete radar-contact capture must be established for the frame;
turning on the experimental radar key is not by itself proof. If either expected source is invalid in a
frame, the affected panel is transparent for that frame. A previous panel is
never reused against a newer cockpit frame. The canopy crops are masked only
when every required panel has been accepted and successfully rendered for both
eyes. If any required panel fails, the complete original canopy HUD is kept so
radar or weapons information does not disappear.

## Integration seam

Do not use `xr_hud_layer` for instruments. It submits a separate OpenXR quad
after the cockpit projection and therefore cannot be occluded by the cockpit
or sit on a console surface.

The viable first route is an image-space, per-eye cockpit composite inside the
existing D3D12 recording path:

1. Select at most one validated profile after the producer has published a
   matching cockpit frame, HUD and cockpit depth.
2. For each eye, copy the profile's `radar` and `stores_status` HUD rectangles
   into a private premultiplied instrument source, then project each onto that
   eye's calibrated four-corner console surface.
3. Depth-test those pixels against the corresponding replayed cockpit depth.
   A missing depth surface, failed depth test, mismatched extent/format, bad
   homography, incomplete profile or incompatible source alpha refuses the
   whole affected target transparently.
4. Composite the resulting per-eye console pixels before the final cockpit
   projection is submitted. Clear exactly the same instrument rectangles from
   the ordinary canopy-HUD texture. Then composite the canopy texture (and,
   if valid, marker texture) by its existing canopy path.

The D3D12 recorder already transitions the two eye targets, HUD, marker and
depth resources in `src/acz-vr/xr_stereo_d3d12.cpp`; the new instrument targets
belong alongside those resources and must return to their documented state.
The producer/consumer handoff already carries the required source validity,
extent/format and shared fence in `src/acz-vr/stereo_frame_source.h`. This
keeps frame ownership and fence handling unchanged.

The first implementation must have a default-off feature key, a private target
only, and counters for `profile-miss`, `profile-refused`, `source-invalid`,
`depth-invalid`, `calibration-invalid` and `panels-composited`. It must not
modify the original HUD resource, cockpit model data, game archives or draw
state.

The offline CPU reference in `research/acz-arc-mpl/panel_routing_reference.py`
exercises this all-or-nothing crop decision, stereo quad projection with
full-frustum refusal, complete marker/radar evidence gates, and linear
premultiplied source-over.
The default reference profile requires both radar and stores/status panels,
rejects overlapping source crops and accepts only explicit boolean validity
flags. A different aircraft profile can declare a different nonempty set of
required supported panel kinds; this remains an offline policy example.
It is a test oracle, not a GPU implementation: the real code must tie the mask
decision to successful completion of both eye renders, including resource
ownership and fence signalling. Its row-vector clip matrices are synthetic test
inputs; no relationship to the `itesu` source frame or runtime camera has been
established. It refuses partially clipped surfaces rather than guessing their
geometry.

## What a deployable per-plane profile still needs

The current v1 profile is deliberately an image-space prototype with one
surface polygon. A deployable profile needs *both* of the following before it
may be selected at runtime:

| Requirement | Why it is required |
| --- | --- |
| Exact aircraft/model selector with archive hashes | prevents a profile being used for the wrong aircraft |
| Named console-surface evidence and material/UV or calibration record | connects the visual console target to a real cockpit surface |
| Independent left- and right-eye calibration, or a proven 3D/UV transform | one mono screenshot cannot establish stereo placement |
| HUD source extent, format and premultiplied-alpha contract | prevents colour/fringe corruption and incorrect crop coordinates |
| Valid cockpit depth rule for each eye | prevents a floating panel through cockpit geometry |
| Capture comparison that demonstrates the crops never retain reticle/ladder/targeting cues | preserves flight HUD readability |

For full 6DoF, a genuine resolved cockpit surface plus UV/material replacement
is the durable destination: its projection moves correctly with each eye and
head translation. The calibrated image-space route is only permitted as an
intermediate path while its per-eye/depth checks all hold. It must disable
rather than pretending to be a physical panel outside its calibrated range.

## F-16C and `itesu` gate

An F-16C profile is blocked on a captured F-16C cockpit plus a resolved
`f16ck` surface relationship. `itesu` is no longer wholly unresolved: two
four-vertex source batches map directly to distinct forward-display atlas
rectangles; see `itesu-display-surface-candidates.json`. They are useful
offline candidate surfaces, but not enough to select or draw a runtime profile.
The next discriminating check is to confirm the matching VIF/VU primitive and
texture binding, then record left/right calibration and depth. Until that is
done, only the capture-specific prototype may be used for calibration work.
