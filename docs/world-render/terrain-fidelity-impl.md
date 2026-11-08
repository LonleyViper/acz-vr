# Terrain fidelity: orientation, tile flags, water (D-298, D-299)

Tags: **[E]** measured offline in this pass, **[I]** inference / not verified in the game. Offline only (no game, no headset). Scripts in `research/world-render-data/`.

## 1. Orientation (task 1) -- no renderer bug [E]
Full write-up: `terrain-orientation-resolution.md` section 6 and D-298. Summary: the `xr_png_once` dumps are the eye image rotated 180 degrees (cockpit gauge digits mirrored and on the wrong
side after a vertical-only un-flip; correct after 180 degrees). Read with 180 degrees (`xr_dump_upright.py`) the shipped `mirror:0` render has the river on the LEFT like the game. The
WARP harness case `terrain orientation` (real shader; landmark blocks left/right of the heading derived by hand; headings 0/-45/90/170; head = anchor yaw 0/25/-40 deg) passes, and
`mirror:1` swaps the sides. Default stays `mirror:0` (right-handed). Unexplained: why a dump of our own private render target is rotated (the harness readback is upright).

## 2. Tile flag bits (task 2) [E] -- they are NOT flips/rotations; renderer unchanged
`.mti` word = tile id (low 10 bits) + flag bits. Observed flag values (bits 10.. = value `word>>10`): m02a {0,1,4,5,8,9}, m16n {0,1}, m07a {0,1}. The orientation doc's "bits 1/4/8" are the values
1 (bit 10), 4 (bit 12), 8 (bit 13).

**Seam test** (`tile_flag_scan.py m02a m16n m07a`): metric = mean |RGB| difference between the touching 64-pixel edges of every 4-neighbour tile pair (256x256 grid). For each flag value f, the 8
dihedral variants (identity, flipX, flipY, rot180, transpose, rot90 cw/ccw, anti-transpose) are applied to the f tiles only and scored over f-f, f-0 and 0-f pairs.
Reference levels: natural seam of flag-0 pairs, and a shuffled-neighbour control.

| stage | natural (flag 0 pairs) | shuffled control | flag 1 identity | best non-identity (flag 1) |
|---|---|---|---|---|
| m02a | 9.99 | 34.7 | **11.96** (89,878 pairs) | 23.1 (flipY) |
| m16n | 7.82 | 18.0 | **6.17** (7,375 pairs) | 10.1 (flipX) |
| m07a | 13.62 | 27.2 | **15.55** (209 pairs) | 26.0 (flipY) |

m02a flags 4 and 5: identity 11.0 and 10.4 against 25.5 and 22.9 for the best flip. For every flag value and every stage the identity transform wins by a factor 2 or more and sits at the
natural seam level; every flip/rotation/transpose is as bad as a random neighbour. **Conclusion: no dihedral interpretation is supported; the renderer correctly ignores them as transforms.** (The 16x16 mosaic of
m02a around the river, `D:\acz-extract\out\river_crop.png`, is seamless with identity.) The "kaleidoscopic" impression therefore does not come from missing flips.

**What the flags do mean** (statistics per flag value over the 2x2-cell height block under each tile, `tile_flag_scan` companion stats, m02a/m16n/m07a):
* bit 10 (value 1) = FLAT tile: height range inside the tile is 0.02 (m02a, 41,657 tiles at height byte 6), 0.14 (m16n, 2,133 tiles; 85% of them at height byte 0 = sea level) and 0.10 (m07a, 61 tiles), against
  roughness 5-15 for flag-0 tiles [E; the meaning "flat/level-ground hint" is inference, nothing else was found to depend on it].
* bit 12 (value 4) = WATER/RIVER tile [E]: all 225 tile ids used with bit 12 are used ONLY with bit 12; 651 of the 1,020 flagged map cells are height-byte 0 exactly (value 5 = flat + water), value 4 = bank cells
  (mean height 0.4); they lie on the single connected river polyline of m02a (351 of 370 red "river" map tiles). Not present in m16n/m07a.
* bit 13 (value 8/9): 10 cells in m02a, 3 of them at the river; meaning unknown [U].

## 3. Water (task 3)
**Evidence** [E] (`water.py`/`water2.py` style checks, m02a): the red-brown "river" of the earlier renders is palette index 40 = RGBA (74,39,25), a FLAT key colour: 280,043 of the 280,061 pixels with that
index lie in bit-12 tiles (18 elsewhere); inside those tiles it covers 0-77% of the pixels (mean 30%) as a smooth ribbon, the rest is ordinary land texture (see `river_crop.png`: the ribbon is
~1 tile (2048 u) wide, windy, with textured banks). So the water SHAPE is in the tile pixels (key colour) and the water TILE set is given by flag bit 12; the colour is not in the tile data.
The game's river at ~30 km measures (70,88,82)/255 (a grey-teal, haze-blended; screenshot `acz-20261026-222415.png`, ground (30,35,30), sky (94,123,126)).
Water-related files: `_sea.gim` is a 128x128 4-bit texture whose 16-entry CLUT is dark grey/green with alpha 0x0c..0x2f (a translucent overlay, not a base colour) [E, decoded header/CLUT only]; `.mii`/`.mia` are all zero in m02a;
`.wad/.wbd/.wpd` (4.9/17/27 KB) were not decoded [U]; `_ref.gim` (32x32, blue-grey CLUT) is probably the reflection map [I].

**Implemented** (format `.zvrs` v2, baker + C++ loader + CPU reference + HLSL):
* v2 header: +80 `off_tilewater`, +84 `water_key`, flag bit 2 `kFlagWater`; section `tilewater u8[ntiles]` (1 = the tile is used only with word bit 12). v1 files still load (no water). Refusals (tested): version 0/9,
  key > 255, tile-water byte > 1, misaligned/overlapping/truncated section, unknown flag bits, v1 carrying water. Baker: `stage_baker.py` selftest covers a water case.
* Atlas: in water tiles texels with palette index == key get alpha 0 and the tile's mean land colour (filtering keeps a smooth shoreline, mips average the alpha). Shader and CPU reference blend
  `lerp(river_colour, lit_land, alpha)`; `river_colour = lerp(kRiverDeep (0.09,0.20,0.21), sky, 0.05 + 0.75*(1-|d.y|)^3)` -- the same Fresnel-like model as the sea plane [I: colour/Fresnel are guesses
  matched to the grey-teal far colour, not measured near water]. Stages without bit-12 tiles (m16n, m07a) are unchanged. Baked: `D:\acz-extract\stage\m02a.zvrs` (4,117,280 B, 225 water tiles, key 40, payload sha256 398221c3...fe97a) and
  `m16n.zvrs` (4,451,556 B, v2, no water, sea plane 0 u as before, sha256 ea284904...731db).
* Tests: terrain_offline (314 checks; v2 round trip, v1 compat, 7 refusals, atlas alpha, river vs land colour, no-water unchanged), WARP harness (503 checks, shader vs CPU for the unchanged synthetic cases).

**Unknown / next**: sea (m16n) colour and animation, the `.wad/.wbd/.wpd` water tables, `_sea.gim` use (specular/foam overlay?), river flow/shore foam, the true near-water colour (needs a low-altitude game screenshot over the river), bit 13.
