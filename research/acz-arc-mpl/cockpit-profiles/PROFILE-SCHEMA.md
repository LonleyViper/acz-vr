# Per-aircraft cockpit-display profiles

Each aircraft needs its own profile. A profile maps only console-bound HUD
domains—currently `radar` and `stores_status`—to instrument surfaces. It must
not route `flight_reticle`, `attitude_ladder` or `targeting_cues` away from the
canopy HUD.

The schema is `acz-vr-cockpit-display-profile/v1`:

```json
{
  "id": "aircraft-identifier",
  "aircraft": { "model_id": "archive model id", "status": "evidence" },
  "capture": { "width": 2560, "height": 1440, "source": "recorded cockpit frame" },
  "hud_texture": { "width": 2560, "height": 1440, "source": "recorded HUD texture", "alpha": "premultiplied source capture" },
  "canopy_hud": {
    "protected": ["flight_reticle", "attitude_ladder", "targeting_cues"],
    "protected_regions": [{ "id": "flight_reticle", "rect": ["x", "y", "width", "height"] }],
    "rule": "..."
  },
  "console_displays": [{
    "id": "stable display id",
    "kind": "radar | stores_status",
    "source_crop": ["x", "y", "width", "height"],
    "surface_polygon": [["x", "y"], ["x", "y"], ["x", "y"], ["x", "y"]],
    "alignment": { "centreline_x": "optional x", "centroid_tolerance_px": "optional positive tolerance" },
    "status": "calibrated mesh evidence or image-space prototype"
  }]
}
```

Coordinates are pixels in the declared capture/HUD images. They are never
model-space coordinates, runtime addresses, or a substitute for surface/UV
evidence. The concept renderer refuses a profile whose source resolutions,
protected canopy domains, crops or four-corner surface definitions are invalid.
It also refuses a console crop that overlaps a declared protected source region.

It additionally fails closed when a console display has no ID, two displays
reuse an ID, a crop or protected rectangle leaves its declared image, a surface
corner leaves the cockpit image, or a four-corner surface has zero area. Source
crops may overlap each other deliberately: the stock-cockpit prototype uses one
stores/status source region for two lower-console subpanels. The rejection tests
are in `../test_cockpit_display_profiles.ps1`.

When a display declares `alignment`, its polygon centroid must be within the
declared tolerance of that capture-space centreline. The current radar target
uses this to stay centred beneath the hood.

`capture-4132-stock-cockpit.json` is intentionally **not** named F-16C: the
recorded capture identifies its process and frame, not its aircraft model. A
real F-16C profile requires a captured F-16C cockpit and a decoded/identified
surface or an explicit calibration record.
