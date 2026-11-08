# Static search for the world lens / culling constants (route 2 of D-291)

Status: offline, read-only research on the user's installed files. No game launch,
no network, no installs, nothing written into the game directory. Claims are
tagged **[E]** evidence-backed (measured here or in a cited repo document) or **[I]**
inferred. Scanner: `docs/world-render/lens_static_scan.py` (stdlib + numpy, run
`python -I`); it reads `acz.exe` only.

## 0. Result in one paragraph

The lens cannot be located by a static search of the installed files. **[E]** The
code section of `acz.exe` is encrypted on disk (entropy 7.999 over every 256 KiB
block; entry point inside a `.bind` section, the Steam DRM wrapper layout), so no
immediate constants and no code cross-references are visible. **[E]** Every
float or double near the lens constants that IS visible (`.rdata`, `.data`,
`_RDATA`, `.rsrc`) is a member of an unrelated lookup table. **[E]** The archives
(`a.arc`..`e.arc`) are also not plaintext on disk and the project's archive tool
(`aczarc.py`) is no longer on disk, so no archive content was inspected; only
the previously generated file lists were used for names and sizes. What the
search did produce is useful guidance for the runtime scan: the game is a native
x64 build with a PS2-memory-shaped game state (section 4), the 16:9 relation of the
measured pair suggests ONE vertical scalar plus an aspect (section 5), and a few
strings and file names point at where the lens or camera parameters might be
configured (section 3).

## 1. Files inspected (SHA-256)

| File | SHA-256 | How read |
|---|---|---|
| `...\ACZero\acz.exe` (4,037,304 B) | `8aa8ffc9a185c58d45b34bc1116d8e1880ea785cc6fa3fa7d6a421fffeb9c739` | whole file, sections parsed by hand |
| `a.arc` | `82c05ed08ca584b551776f916ba06e9bfd21c01e0cfede8afe6ffbd276745f80` | headers + 4 x 1 MB entropy samples + one 112-byte entry; not decoded |
| `b.arc` | `4c4a97f5783cc925002d642306d3b25de2fa87e4e10a4f5a82e0cc96ec00d7ff` | not opened beyond hashing |
| `c.arc` | `a84efdbba33046105134111197d1aa7babecb16dc3f15f158f7689dd25299bce` | not opened beyond hashing |
| `d.arc` | `c28e41acdc6ba6699481169e168381234b920191f0189384fca751c04538d0e2` | not opened beyond hashing |
| `e.arc` | `63d8f45fc98cc5fbeca71de8e98727f8662a1e7d6aeb291b305d5503bc6de3d5` | first 2 KiB + one stored PNG entry |
| `D:\acz-extract\lists\a.txt` | `5890fbb84956b3104ee4f07b5c919cb603deb23510b4a242b8f334ababd3b5e1` | names/offsets/sizes (4,816 files) |
| `...\lists\b.txt` | `c7a910625c2015f39ccbaf0e9beb12fd770c7afab9a9dc019cf29ed08cd45ef6` | list |
| `...\lists\c.txt` | `638d9abef328bab6c2fefdc0c854290f5923f3df21d27835c3c228259763ea9e` | list |
| `...\lists\d.txt` | `8f578f2ae1a9186816675ec76bc8d373d6d40ff160ed350b22f56da5289c22f8` | list |
| `...\lists\e.txt` | `ec47b8661f2d98d874ccf189f42fe093bbfe33c3fb9e2344048e4034a11bf09b` | list |

PE layout **[E]**: image base 0x140000000, DYNAMIC_BASE + HIGH_ENTROPY_VA set
(addresses move per launch), entry RVA 0x385D310 (inside `.bind`). Sections
(RVA / virtual size / raw size): `.text` 0x1000 / 2.6 MB / 2.6 MB (encrypted);
`.rdata` 0x27B000 / 581 KB; `.data` 0x309000 / **55.4 MB virtual, only 112 KB raw**
(a large zero-initialised static area); `.pdata`; `_RDATA` 0x37F9000 / 88 KB;
`.xbld` (GDK build marker); `.rsrc`; `.reloc`; `.bind` 234 KB (entropy 7.96).
The embedded build path string is a Gaming.Desktop.x64_Release path (GDK build).

Entropy **[E]**: `.text` 8.00 (all blocks), `.bind` 7.96, `.rdata` 5.36, `.data`
2.15, `_RDATA` 5.60. `.text` also contains 10,085 `E8` bytes against 0 `48 83 EC`
sequences, so it is not readable x64. The `.bind` section name plus an entry
point in it matches the Steam DRM wrapper layout **[I]** (the wrapper itself was
not identified or opened). Decrypting `.text` would mean circumventing that
wrapper: **not done and not recommended** (section 6).

## 2. Constants searched and hits

Target representations (tan pair from the E3/E9 measurements, composed from
0.6152 x 0.3460; E9 gives 0.61517 x 0.34604, ratio 1.77779 = 16:9 **[E]**):

| Quantity | Value | float32 bits | float64 bits |
|---|---|---|---|
| tan half-FOV x (4-digit / E9) | 0.6152 / 0.61517 | 0x3F1D7DBF / 0x3F1D7BC8 | 0x3FE3AFB7E90FF972 / 0x3FE3AF78FEEF5EC8 |
| tan half-FOV y | 0.3460 / 0.34604 | 0x3EB126E9 / 0x3EB12C28 | 0x3FD624DD2F1A9FBE / 0x3FD62584F4C6E6DA |
| 1/tan x, 1/tan y (computed) | 1.62549, 2.89017 | 0x3FD00FFB, 0x4038F89A | 0x3FFA01FF558E25F4, 0x40071F133CABA737 |
| 1/tan x, y (as measured) | 1.6255, 2.8902 | 0x3FD01062, 0x4038F909 | 0x3FFA020C49BA5E35, 0x40071F212D773190 |
| x16 (tan x, tan y) | 9.8432, 5.536 | 0x411D7DBF, 0x40B126E9 | 0x4023AFB7E90FF972, 0x401624DD2F1A9FBE |
| x64 (tan x, tan y) | 39.3728, 22.144 | 0x421D7DBF, 0x41B126E9 | 0x4043AFB7E90FF972, 0x403624DD2F1A9FBE |
| full angle rad (x, y) | 1.10304, 0.66621 | 0x3F8D307C, 0x3F2A8CFD | 0x3FF1A60F81A51623, 0x3FE5519FA54F2D9D |
| half angle rad (x, y) | 0.55152, 0.33311 | 0x3F0D307C, 0x3EAA8CFD | 0x3FE1A60F81A51623, 0x3FD5519FA54F2D9D |
| full angle deg (x, y) | 63.1997, 38.1712 | 0x427CCC73, 0x4218AF5A | 0x404F998E6B3AF677, 0x404315EB32900023 |
| half angle deg (x, y) | 31.5998, 19.0856 | 0x41FCCC73, 0x4198AF5A | 0x403F998E6B3AF677, 0x403315EB32900023 |
| screen focal px (258.048/tan x, 225.792/tan y) | 419.454, 652.578 | 0x43D1BA17, 0x442324FF | 0x407A3742E9B219F8, 0x4084649FD0A5BBEF |
| 256/tan x, 224/tan y (unscaled viewport) | 416.125, 647.399 | 0x43D00FFB, 0x4421D987 | - |
| 4:3 tan x from tan y | 0.46133 | 0x3EEC33E2 | - |
| aspect 16/9, 9/16 | 1.77778, 0.5625 | 0x3FE38E39, 0x3F100000 | - |

A 4-digit measurement fixes a float32 only to about +-700 ULP (tan x +-5e-5),
so bit patterns above are centres of ranges, not exact keys. The scan therefore
used relative tolerances of 1e-4 (tan, reciprocals, x16/x64) to 2e-3 (degrees).
Also searched: fixed point (x4096, x65536) and PS2 angle units (0x10000 and 0x1000
per turn: full x 11505 / 719, full y 6949 / 434, half x 5753 / 360, half y 3474 /
217; degrees x256: 16179, 9772) as int16/int32 by arithmetic only; they were
not found as aligned int tables in the visible sections (no distinctive hit
worth listing; those ranges are dense with unrelated ints and cannot be
discriminated without a code reference). PS2 VU/EE floats are IEEE-754
single precision apart from NaN/Inf/denormal handling, so the float32 patterns
are the right form **[I]**.

Visible-section hits at tolerance 1e-4 (float32/float64, all alignments counted;
only aligned ones are plausible data):

| Hit | Section, file offset (VA) | Neighbouring data | Verdict |
|---|---|---|---|
| 0.6152316 (f32) x4 | `_RDATA` 0x34DDB4, 0x34E254, 0x350054, 0x350BB4 (0x14380B1B4 ...) | bytes `D1 7F 1D 3F` between `-0.2117 ... 0.0, 1.0, -0.227, 0.634`: a repeating (c, 0, +-1, s) table | NOT lens (table value) |
| 0.6152316 (f64) x2 | `_RDATA` 0x3488D0, 0x349B50 | same `_RDATA` math-table area | NOT lens |
| 1.62536 / 1.62544 (f32) | `.rdata` 0x282A24, 0x2D48BC | strictly increasing sequence 1.6238, 1.6248, 1.6254, 1.6258 ... with junk interleaved: a reciprocal / interpolation table | NOT lens |
| 1.62540 (f64) | `.rdata` 0x2835D8 | descending run 1.6463 .. 1.6101 step ~0.005: table | NOT lens |
| 1.7777778 (f32) | `.rdata` 0x2D5F00 | descending run 1.8286 1.8156 1.8028 1.7902 1.7778 1.7655 ... (a 1/x style table) | NOT lens (but see 16:9 note below) |
| 1.7777778 (f64) | `.rdata` 0x283500 | run 1.8028 1.7965 1.7902 1.7840 1.7778 ... | NOT lens |
| 19.032 (f32) x2 | `.rdata` 0x285DEC, 0x289ADC | sorted list 15.77 17.03 17.66 17.91 19.03 19.91 ... | NOT lens (vs 19.0856 half-FOV y: 3e-3 off, ordered list) |
| 1.10345 (f32) | `.rdata` 0x2D6060 | descending run 1.1228 1.1179 1.1130 1.1082 1.1034 ... | NOT lens (table; 1.1034 vs 1.1030 not equal) |
| 0.333 (f32) | `.rdata` 0x2D1504 | sorted decimal list 0.3, 0.31416, 0.31831, 0.32, 0.333, 0.33333, 0.34907 ... | NOT lens (round decimal, sorted constants list) |
| 26.0 (f32) | `.rdata` 0x2D19AC | small-integer constant | NOT lens |
| any exact match to tan y 0.3460, 1/tan y 2.8902, x16/x64 pairs, 258.048, 225.792 | none found | - | UNKNOWN (absent in visible sections) |
| `.text` immediate-looking hits (e.g. the angle pair 1.10294 / 0.66626 at 0x139EF9 / 0x139F6C) | `.text` | random ciphertext | NOT evidence: `.text` is encrypted; these are chance matches in random bytes |

No pair (tan x with tan y, or reciprocals) occurs within 256 bytes in any visible
section; the only pair "hit" (the angle pair above) is in ciphertext.

**Cross-references: not possible [E].** RIP-relative loads of these `.rdata`
addresses live in the encrypted `.text`; capstone 5.0.7 exists in the user
Python but disassembling ciphertext is meaningless (checked: the 0x139EE0 region
disassembles to nonsense). The hits above are table members anyway.

## 3. Other static pointers worth knowing (all in visible data)

- **`#cam_lens##` and `#dtl_x#dtl_y#dtl_z`** at file offset 0x2AAD38 / VA 0x1402AB938,
  next to `data/../stream/pack/RADIOJP.PAC` and `FRND`. Looks like a debug or
  text-template tag set from the original game ("cam_lens" and detail x/y/z)
  **[I]**. It suggests the original game has a variable literally named camera
  lens, but a string does not locate it, and it may belong to a cutscene or
  script system rather than the flight camera.
- **VU instruction mnemonic table** in `.rdata` (0x2AA2C0..0x2AA4D0: `vadd`,
  `vmulx.xy`, `vmaddw.xyzw`, `vdiv`, `vsqrt`, `vclipw.xyz` ...). Together with
  the RTTI class names in `.data` (`PS2Model`, `Unit...`, `Mode...`, ActionScript
  `swg`), this indicates a native port where VU0 macro code is expressed as
  helper calls and is not a runtime VU interpreter **[I]**. `vclipw.xyz` is the VU
  clip-flag test, so the CPU-side frustum test is plausibly a clip-space test
  of the projected vertex against +-w, in which case the cull window follows the
  same projection as the lens **[I]**; if instead plane constants are
  precomputed, they are a second value to change.
- **ActionScript bindings `option_getAspect`, `option_setAspect`, `option_defAspect`**
  (0x2CBBA8..): an in-game aspect option exists **[E]** (string presence). The
  measured 16:9 ratio of tan x to tan y suggests the horizontal tangent is
  derived from a vertical one by the aspect **[I]**. This is the best calibration
  lever for the runtime scan (section 5).
- **RTTI in `.data`**: `WeaponExRcamCtrl` (rear/weapon camera controller), `ObjShiningSky`,
  `PS2Model`, `BaseViewer`, `ModeSceneViewer`. None is a flight camera class name
  (the flight camera lives in code we cannot read) **[E]**.
- **File names in the archives** (from `lists/a.txt`, names only) that could carry
  lens/camera/cull data, UNREAD: `data/cam/*.bin` (six 112-byte files,
  28 floats each: m02a m07a m13a m15a m16c r01a; probably cutscene cameras),
  `data/datapack_jp.bin` (406,116 B), `data/mis/pack/<mission>/*` (430 files,
  e.g. `m15ad.bin`, `m15ald.bin`, `m15amd3.bin`: mission data, names suggest
  per-distance/level-of-detail suffixes 3 and 4), `data/mis/mpl/*ex.bin`,
  `data/map/<mission>/*.mhd .mtd .wpd .wbd .wad .efa .mta .mti` (terrain
  height/tile/water/object data per mission; 29 files each), `data/mapset/map`,
  `data/spheredata/map`, `data/planim`, and 59 compiled shaders under `shader/`
  (`ps2_v.cso`, `DrawACM_v.cso`, `DrawCPT_v.cso`, ...). The shaders matter for
  the 3D object path only; the pre-projected terrain/sky path is `ps2_v`. None
  of this was decoded here **[E for names, nothing for content]**.

## 4. Which hits are the projection lens, and what the lens would also need

Verdict: **no static hit is the lens.** All visible near-matches are table
entries (section 2). The lens is therefore UNKNOWN as to location; the runtime
scan is the only available discriminator. What the repo evidence implies about
the whole class of change:

1. **Where the live state is [E]** (E6, `research/experiments/E6-2026-10-12.md`):
   a 64 MB private block that behaves like emulated/mirrored PS2 main memory holds
   the camera state at fixed block offsets: look yaw +0xFDD410, aircraft
   position +0xFDD42C..34, heading +0xFDD448, camera position +0xFDD860..68,
   composed camera rotation +0xFDD870..78, a render-side copy at +0x312E30. The
   block base moves per launch; the offsets held across two launches. The look
   yaw is an output rewritten every frame (D-269 run D, 119/120 overwritten), so
   by analogy anything derived per frame will be overwritten **[I]**. A lens
   scalar set at mission or camera init could persist; a scalar recomputed from
   an aspect option each frame behaves like the yaw.
2. **Two plausible homes [I]**: (a) a field in the camera/viewport object in or
   near the heap structure around +0xFDD800..+0xFDDA00 (camera position at +0xFDD860
   is a composed output; the lens belongs to an input struct, so look further
   around it), (b) the static (ELF `.data/.bss`-like) area around +0x312E30, which
   is a fixed address in the block (stable across launches, unlike the heap).
   (c) native globals in the module's 55 MB `.data` (RVA 0x309000..0x37DFD5C),
   which E6 included only as "module's writable sections" without a lens search.
3. **Projection scale relation [E]**: x_screen = 258.048 x_clip + 2048 w and
   y_screen = 225.792 y_clip + 2048 w (256 and 224 times 1.008). Terrain and
   sky are pre-projected CPU-side (ps2_v), so a lens change only matters if it
   enters the CPU projection that produces these vertices; the 3D object path
   (DrawACM/DrawCPT) uses matrices that carry the lens inside the composed
   matrix (VP = inverse(M256) x M64), where a lens scalar is NOT visible
   independently.
4. **Guard band / scissor [E arithmetic, I for game behaviour]**: GS screen
   coordinates are centred on 2048 with a 4096-wide canvas (+-2048). With the
   viewport scale 258.048 / 225.792, |x_clip| up to 7.94 and |y_clip| up to 9.07
   would still fit the coordinate range; at the current lens that is a
   horizontal half-angle of 78.4 degrees and a vertical one of 72.3 degrees. So
   the 4096 limit is NOT what limits what the game submits (the visible window is
   +-1 in clip space). A cull to the +-w clip window is the likely limiter;
   whether the game clips to +-w or to a guard-band multiple (typical PS2
   practice) is UNKNOWN. If it clips to a guard band, some terrain beyond the
   63x38 degree window might already be submitted; the earlier note that "nothing
   exists outside the window" was a conclusion from the replay stream of one
   capture and is not proven for the CPU (Astra review, issue 29 comment, section 3).
5. **Depth / near-far [E for the shader, I for the cause]**: the object shader
   depth is `1/w - 255*2^-24`, so D3D near clipping at z>=0 removes geometry at
   about w > 65,800 units (docs/interception/acm-shader-dataflow.md section 1.1). That is a
   far limit on objects independent of the lens. A wider lens does not change it.
6. **What changing the lens would also require [I]**: (a) every consumer of the
   same value must change together, or the cockpit (GPU DrawCPT path with its own
   composed matrices), HUD marker projection and the project's lens recovery
   (`camera-lens-differs` refusals) will disagree with the world; (b) the
   culling window (clip test or plane constants); (c) terrain emission: map data
   is tiled per mission (`.mhd/.mtd/.wpd/...`), and the world draw count per frame
   depends on a distance or sector window, which a wider lens enlarges only if that
   window is angular; (d) LOD distance thresholds (the `3`/`4` suffixed mission
   files suggest distance-banded data) are unrelated to FOV and would still cap
   what is emitted at range; (e) because a wider lens at an unchanged 512x448
   viewport SHRINKS content, any widening for VR must be paired with the
   viewport scale (258.048 / 225.792) or the compositor must rescale the window;
   changing only the lens makes the central region smaller, not the world larger.

## 5. Recommendation for the runtime scan (the planned EE-memory experiment)

Facts to use: tan y alone (0.3460 +- 1e-4) likely exists as a float, with tan x
derived from it by 16/9; the scale and reciprocals exist if the code stores a focal
form. Be exhaustive about representation, but prioritise as follows.

Regions (in priority order):
1. The 64 MB block (the E6 block), whole block at 4-byte alignment, first the
   window +0xFDD000..+0xFDF000 (camera structures), then +0x310000..+0x316000
   (the static copy at +0x312E30), then the rest. Log each hit as block-relative
   offset (not absolute address) so it can be compared across launches.
2. The module's `.data` (RVA 0x309000..0x37DFD5C), via the module base of
   `acz.exe` for that launch.
3. Other committed private RW regions of 64 KB or more only if the first two are
   empty, with the caps required by the Astra review (region, byte, time,
   candidate and log caps; readable committed data pages only; no guard,
   no-access or executable pages; revalidate the allocation identity; keep
   UNKNOWN if caps truncate).

Representations (priority order): float32 tan y in [0.34595, 0.34615] (also
explicitly 0.34604); float32 1/tan y [2.8895, 2.8905]; float32 tan x
[0.6151, 0.6153] and 1/tan x [1.6254, 1.6256]; float32 full angle y in rad
0.6662 and degrees 38.17, half 0.3331 rad / 19.09 deg; same for x (1.1030 rad,
63.20 deg, 0.5515 rad, 31.60 deg); the x16/x64 scaled pairs; the focal-pixel forms
(419.45 / 652.58 and 416.12 / 647.40); 16-bit and 32-bit int fixed (x4096:
2520 / 1417, 6658 / 11838) and PS2 angle units (0x10000/turn: 11505 / 6949 full,
5753 / 3474 half); double forms of the top three. Treat a lone value as weak;
require context: (i) a nearby 16/9 (0x3FE38E39) or 9/16 (0x3F100000) value, (ii)
near-pair distance <= 64 bytes, (iii) not inside a monotone run (table).

Calibration lever (needs the user, not code): the game has an in-game aspect
option (strings `option_getAspect/setAspect`). Take two scans with the aspect at
16:9 and at 4:3 in the same mission and flight state: the true horizontal
tangent changes (0.6152 vs about 0.4613 if the vertical is kept) while tan y
stays; the candidate that does exactly that is the lens. If horizontal stays the
same and vertical changes, the lens is vertical-driven the other way. Likewise,
if any in-game zoom exists (e.g. a sniper-style zoom or the cockpit/chase view
switch), a differential scan between two views finds the field. These are the
only differential tests available without a write; they need the user to change
an option, so they need a separate go-ahead.

What to log in the lens-scanner rerun (per candidate, capped):
- block-relative offset and absolute address, region base/size/protection/type
  (image vs private), representation and exact decoded value, ULP distance to target;
- 64 neighbour bytes before/after as hex and as floats, and whether a 16/9 or 9/16
  value, the other tangent, or the reciprocal exists within +-64 B;
- monotone-run test (is the value inside a table of >= 4 sorted neighbours?);
- per-frame re-read for >= 300 frames at Present ticks: min/max/changed count,
  and whether it changed during look-around, afterburner/speed, view switch, mission
  load, pause, aspect option (a stable value over identical frames cannot
  distinguish an immutable input from a same-value rewrite; do not auto-promote);
- cross-launch: same block-relative offset hits in a second launch;
- coverage: bytes scanned per region, regions skipped and why, caps hit, scan time
  per tick, truncation flag; and a zero-hit statement worded as "not found
  within these representations, ranges and caps".
- the E6 camera fields (+0xFDD410, +0xFDD860, +0xFDD870) in the same record, so the
  scan is tied to the same live block and frame.

## 6. Unknowns and risks

UNKNOWN: location and representation of the lens; whether it is one scalar plus aspect;
whether the culling shares it; terrain emission and LOD limits; contents of every
archive file (no readable copy; tool absent); the pre-projection code (encrypted).

Archive access: `aczarc.py` (ModDB addon, handoff 2026-10-26-acz-arc-mpl-cockpit-geometry.md)
is not on disk any more (only its readme survived in an old scratchpad), and the
files are not plaintext (entropy 8.0; a stored PNG begins `89 54 D8 BD`, not
`89 50 4E 47`). No attempt was made to reimplement the container decryption. The
archive-inventory agent owns that route; this note depends on it for items 3 and
the `data/cam/*.bin` check.

Risks: (1) the code is wrapped by a DRM-style stub; we do not unpack, patch or
bypass it, and the only intended action is a bounded, reversible runtime write
under the existing D-267 limits; any anti-tamper or integrity check on writes to
memory is not characterised (Astra's review: none is established, none may be
bypassed). (2) A lens write may break the project's own lens recovery and the
cockpit/HUD agreement (`camera-lens-differs`). (3) Widening the lens at a fixed
viewport shrinks the image rather than adding world (section 4, item 6e). (4) A per-frame
rewritten field is invisible to writes (D-269). (5) The hits here come from a
single `acz.exe` build (hash above); a different build changes offsets.
