# First full-cockpit target: `itesu`, with F-16C kept separate

Decision: use the already-present `itesu` asset as the first **full cockpit
integration candidate**. Do not label it F-16C, substitute it into the F-16C
model, or redistribute any extracted game asset.

## Why `itesu` first

| Evidence | `f16ck` | `itesu` |
| --- | --- | --- |
| Exterior aircraft material | Clear 512×512 F-16 exterior atlas | not identified |
| Cockpit/instrument atlas | 128×128, small but present | 512×512, detailed forward panel, displays, consoles and gauges |
| Candidate cockpit geometry | not established | dense ACM part 2, 23,910 observed source vertices |
| Display surface UV/position candidates | not yet selected | two four-vertex forward-display batches selected from the cockpit atlas |
| Safe present role | F-16-specific evidence lead | first full-cockpit rendering/calibration target |

The visual cockpit density is the reason to begin with `itesu`; it offers real
console detail rather than placing instruments on the sparse forward capture.
The extracted `datapack_jp.bin` now places `itesu.mpl` in one repeated path run
with `j35j` data, HUD and animation; this is a static association lead, not
proof of runtime aircraft selection or cockpit identity. See
`itesu-cockpit-atlas-findings.md` for the offset and comparison. The
F-16C path remains valid, but needs its own surface/UV association and a
captured F-16C cockpit before any F-16C profile can exist.

## Intended first profile

`itesu` has two evidence-pinned forward display candidates from
`itesu-display-surface-candidates.json`:

| Target | Texture rectangle | Proposed dynamic content |
| --- | --- | --- |
| port forward display | 512-atlas `(87.84,154.15,56.15,56.15)` | radar |
| starboard forward display | 512-atlas `(263.68,154.47,56.45,54.79)` | compact weapons/stores/status |

These are actual atlas-to-source-lane joins, so they are the correct next
places to aim a cockpit-mounted renderer. The forward hood, reticle, attitude
ladder and target boxes are explicitly excluded from both.

## Remaining offline gates

1. Recover the corresponding VU/GIF primitive/binding state: the current
   batches establish source lanes but not final triangle assembly or texture
   binding.
2. Build a local, non-redistributable preview from the user's extracted data
   only after that association is confirmed.
3. Capture `itesu` in a neutral and real down-look pose for both eyes; measure
   each panel's screen/depth calibration.
4. Only then add a default-off runtime profile that composites radar and status
   into those surfaces using premultiplied source-over. It must leave all other
   aircraft on the normal canopy HUD.

This keeps the eventual cockpit data-driven: a later proven F-16C interior
gets its own profile and never inherits the `itesu` dimensions or placements.
