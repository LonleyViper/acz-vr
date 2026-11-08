# Cockpit display concept: radar and stores

Status: offline visual prototype. It is not a game modification, an asset
import, a physical-panel decode, or a claim that the chosen pixels identify
the original model's triangle surfaces.

## What is visible already

The recorded cockpit frame has a large central, blank, multi-sided display
opening and a lower central console. The HUD capture has separable regions:
the tactical/radar scope is lower-left and the gun, weapon/stores and damage
region is lower-right. The central aiming/HUD symbology, speed and altitude are
not moved to a console in this concept.

The derived [concept image](../../research/acz-arc-mpl/cockpit-display-concept.png)
maps those two source regions to those two visible surfaces. It is a placement
target for calibration, not texture baking. Its profile is
[`capture-4132-stock-cockpit`](../../research/acz-arc-mpl/cockpit-profiles/capture-4132-stock-cockpit.json),
which is deliberately capture-specific rather than labelled as an F-16C.

| Source HUD region | Proposed cockpit location | Why |
| --- | --- | --- |
| radar/scope, lower-left | lower-centre tactical console | tactical scan information belongs below the forward hood/combiner |
| gun, weapon/stores and damage, lower-right | two small lower side consoles | secondary, glance-down information; deliberately smaller than radar |
| reticle, attitude/flight ladder | canopy HUD | must remain view/camera aligned rather than panel-mounted |
| speed, altitude | initially canopy HUD | avoids incorrectly assigning them to an unverified physical panel |

## Measured prototype coordinates

All values below are pixels in the specific `2560x1440` recorded image, not
game-model coordinates or runtime addresses.

| Item | Rectangle / polygon |
| --- | --- |
| radar crop | `(82, 868, 620, 560)` |
| stores crop | `(2038, 1003, 520, 437)` |
| radar console surface | `(985,1080) (1575,1080) (1615,1350) (945,1350)`; centroid `x=1280` |
| left status surface | `(500,1190) (820,1190) (830,1370) (490,1370)` |
| right weapons/status surface | `(1700,1190) (1990,1190) (2030,1370) (1690,1370)` |

The matching content-free calibration reference is
[`cockpit-console-calibration-overlay.png`](../../research/acz-arc-mpl/cockpit-console-calibration-overlay.png).
It identifies the usable image-space target regions as follows:

| Console target | Image-space bounds | Placement rationale |
| --- | --- | --- |
| radar | `x=945..1615`, `y=1080..1350` | centroid locked to `x=1280`, below the forward hood; largest lower-console display |
| status | `x=490..830`, `y=1190..1370` | compact port-side console/radio region |
| weapons and damage | `x=1690..2030`, `y=1190..1370` | compact starboard-side console region |

These are the only screen-space targets currently identified for this capture.
They must be separately calibrated for each eye and aircraft before a runtime
profile may use them.

## Down-look and alpha limitations

This capture is forward-neutral. Existing look evidence reaches substantial
upward pitch but only about two degrees downward, so it does not expose enough
of the lower console to calibrate a genuine glance-down view. The screen-space
polygons therefore remain **forward-view placeholders**; a down-look capture
for each eye is required before treating them as physical display locations.

The recorded `hud-on-black` and `hud-alpha-raw` diagnostic PNGs both store
fully opaque PNG alpha in a sampled audit. The latter is an RGB visualisation
of alpha, not an alpha-bearing texture. Consequently the concept renderer's
black colour-key is preview-only. Runtime composition must use the original
FP16 `StereoFrame::hud` resource and an explicitly tested coverage policy;
the existing consumer raises alpha to at least the maximum shaded RGB channel.
Stored alpha alone is insufficient for this sample. See the runtime contract's
alpha audit; neither diagnostic PNG is an exact FP16 reconstruction.

## Implementation boundary

The current XR path can isolate a premultiplied HUD texture. Its ordinary
HUD/world-quad modes are **not** mesh-mounted instruments: they are separate
composition layers. The implementation that follows needs to render two
cropped HUD subrectangles onto cockpit-frame surfaces, per eye, after the
cockpit replay and before final composition.

The preferred radar source is the existing opt-in radar-aware HUD replay
(`gpu_copy_step5_hud_radar`): it accounts for the stencil-masked contact tail
that the earlier generic HUD replay omitted. Stores/status uses the regular HUD
capture. This selects dynamic game-produced images, not a copied cockpit or HUD
asset.

It must:

1. Use the selected source UV subrectangles only when the HUD source texture
   is valid for that frame; otherwise draw neither panel.
2. Transform each panel using the same cockpit/eye frame that produced the
   replayed cockpit image, not LOCAL/head-locked coordinates.
3. Depth-test/occlude against a valid replayed cockpit depth image; absence of
   valid depth is a transparent refusal, not a floating screen.
4. Keep the original canopy-HUD layer selectable until panel placement is calibrated.
5. Provide a calibration mode that renders panel outlines and records their
   image-space corners; do not hard-code the prototype pixels as 3D geometry.

Profiles are described in
[`PROFILE-SCHEMA.md`](../../research/acz-arc-mpl/cockpit-profiles/PROFILE-SCHEMA.md).
They require `flight_reticle`, `attitude_ladder` and `targeting_cues` to remain
on the canopy HUD, and permit only radar/stores-status crops on console surfaces.
The renderer derives `cockpit-canopy-hud-only.png` by clearing the console crops
from the canopy texture before composition, so console information is never
duplicated over the flight HUD.

## Next discriminating check

Decode enough `itesu` topology and material state to associate the central
visible blank display with a real model surface and UV range. Until then the
implementation should use a fail-closed, calibration-only virtual panel rather
than claiming a mesh-texture replacement.

The immediate file-data result is deliberately narrower: `f16ck` part 8 is a
263,216-byte GIM texture and part 9 a 17,456-byte GIM; `itesu` parts 8 and 9
are both 263,216-byte GIM textures. They are potential panel materials but are
not decoded or assigned to a display surface here.
