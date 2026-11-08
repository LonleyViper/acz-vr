# RADAR-BG: the game's translucent radar background in the XR HUD layer

Known issue (D-356/D-357, radar-markers-game-original.md 1 and 6.4, J-radar-analysis 3.5): the XR HUD layer shows the game's radar
as an OPAQUE black square; the game window shows a dark translucent square over the world. Offline work only: nothing here was run in
the game or a headset. Tags: **[E]** evidence, **[I]** inference, **[G]** guess.

## 1. What the game draws (unchanged facts)

Radar group, end of the HUD region (J-radar-analysis 1.1, step37 per-draw dumps) [E]:

* b: background quad, blend colour SRC_ALPHA/INV_SRC_ALPHA, alpha ops ZERO/ONE (target alpha untouched), stencil ALWAYS/KEEP;
* c: mask quads, stencil pass REPLACE, colour SRC_ALPHA/ONE (additive black = nothing), alpha ONE/ZERO (alpha = source alpha);
* contacts later: stencil EQUAL, colour SRC_ALPHA/INV_SRC_ALPHA. The radar-resume path (`gpu_copy_step5_hud_radar`, RADAR-SCENE-RESUME)
  replays them into the same HUD target; it does not touch alpha.

In our zero-cleared FP16 HUD target the square therefore holds rgb 0 (b: black x a + 0) and alpha exactly 1 (c), with or without the
radar key. The consumer's HUD alpha is `max(src.a, max(rgb))` (+ the menu floor), so the square is opaque black.

## 2. Measurements (archived step83 logs, stdlib Python, nothing new captured)

* `hud-alpha-raw` [E]: run P s1/s2, run Q s2, run X2 s2, step69 run1 s2, headset-test15 s2: the ONLY non-zero alpha is the rectangle
  x 157..555, y 934..1345 (399x412 of the 2560x1440 target), every non-zero value 255. 0..465 texels inside are 0 (not touched).
  So "alpha > 0" means "inside the radar square" in every archived flight frame.
* `hud-on-black` run P s1 inside the square: 145431 of 164388 texels rgb < 16/255 (black background), the rest rings/cross/contacts.
* The game's background alpha [E/I]: `scene-end` (the game's own scene RT, radar group composited) run P b1/b2/b3, linear-light ratio
  inside/outside across the square's four edges (smooth regions only, 247-289 pairs per image): median 0.429 / 0.435 / 0.436
  (q25..q75 0.42..0.48). Background colour is black, so a_bg = 1 - ratio = 0.56..0.57; 0.5625 = PS2 alpha 0x48/0x80 [G for the exact
  value]. The game WINDOW shots (T20, T50; after the game's output pass and window tone curve) give 0.21 in display-decoded values: the
  output pass is not linear, so it is not the blend alpha; our HUD composites over OUR shaded world, so the scene-space value is the
  one to reproduce.

## 3. Why not the PSO clone (J-radar-analysis 3.5 option 2)

It needs creation-desc retention, lazy clones and 2 extra commands per replayed radar draw in the producer, and the result is still a
guess for the alpha of draw b (the game's PS output alpha cannot be read; only its blended result). The consumer cap below reproduces
the measured translucency with no producer change, no new GPU work and no new memory.

## 4. The fix (consumer only; option `radarbg:1`, default OFF)

HUD target only, `mode_bits` bits 19..23 = q = round(rb x 32) (1..31; 0 = off). In `xr_stereo_blit.hlsl` mode 1 and its CPU mirror
`acz::hudmenu::hud_alpha` (`hud_menu.h`):

    m  = max(shaded rgb);  sa = max(src.a, 0)
    if q != 0: sa = min(sa, rb + (1 - rb) * m)      // the background gets rb; a symbol over it gets rb "over" its brightness
    a  = saturate(max(sa, m, menu floor))

* Every texel whose stored alpha is <= the cap is bit-for-bit the old rule: all texels outside the square (stored 0) and the untouched
  texels inside. The premultiplied colour is never changed. The menu-mode floor (`menub`, bits 24..31) is applied after and unchanged.
* Rings/cross are additive in the game (exact alpha rb, rgb added): we give rb + (1 - rb) m, i.e. the world under a line is dimmed a
  little more than in the game (max 0.44 x line brightness). An opaque contact gets rb + (1 - rb) m (0.91 for a 0.8 blue): the world
  leaks through by (1 - rb)(1 - m) where the game shows none [I; same order as every other HUD symbol under the max(rgb) rule].
* Options (cockpit option string, model cockpit runs only: `-CockpitModel '...;radarbg:1'`): `radarbg:0|1` (default 0),
  `radarbga:<0.05..0.95>` (default 0.5625, quantised to 1/32; e.g. `radarbga:0.75` approximates the darker game-window look).
  `xr-stereo.log` `event=hud-menu-config;...;radar-bg=1;radar-bg-alpha=0.5625`.
* Why default OFF: the rule is not geometric, it changes any texel the game wrote alpha > the cap into. Every archived frame has that
  only in the radar square, but no hud-alpha-raw exists for the pause menu or the expanded big map (TAB) [unknown]; if they write
  alpha 1 under dark panels, those become translucent too (the menu floor still applies in menu mode).
* Not in the producer PNGs: `hud-over-world` / `hud-alpha-raw` show the stored target and the OLD rule (the producer does not know the
  cockpit options). Check in the headset / XR mirror.

Files: `hud_menu.h` (`radar_bg_bits`, `hud_alpha`), `xr_stereo_blit.hlsl` (mode 1), `xr_stereo_d3d12.cpp` (HUD mask 0xFFF80000),
`xr_stereo_output.cpp` (job bits, config line), `cockpit_model.h/.cpp` (options).

## 5. Tests (offline, private build D:\b-radarbg, run by this change)

* `xr_composite_warp` case **g2** (real shader, real D3D12 WARP backend): synthetic radar target (square rgb 0/a 1, additive line,
  opaque contact, alpha-0 hole, symbols outside); GPU == CPU `hud_alpha` over the whole target (max_err 0.0029, 0 bad) for bits 0,
  radar-bg and radar-bg + menu floor; 47196 texels outside the square byte-identical to the job without bits; 9975 inside with a new
  alpha, colour unchanged; background (0,0,0,0.5625); crop over a 0.5 world = 0.5 x 0.4375; markers target untouched.
  Result: 11357 checks, 0 failed.
* `xr_stereo_output_offline` (`hud_menu_tests` 4b/6, also under `--cockpit`): bits/quantisation, overlap with the backdrop, the old
  rule below the cap (441-point sweep), NaN, floor + cap, options parse/refuse; a consumer run with `radarbg:1` puts the bits on all 40
  HUD jobs (16 also with the menu backdrop) and the config line. 278515 / 1962 (--cockpit) / 5571 (--hud) checks passed.

## 6. Live check list (not run)

1. Install with `-CockpitModel 'auto;anim:1;default:j35jk_vrcockpit;radarbg:1'` (day config, `-HudRadar` optional). `xr-stereo.log`:
   `hud-menu-config;...;radar-bg=1;radar-bg-alpha=0.5625`.
2. Flight, game window screenshot + headset/mirror at the same moment: the radar square is dark translucent (terrain/sky visible
   through it), rings, cross, cone and contacts at full brightness; nothing else of the HUD changed (compare a run with `radarbg:0`).
3. Open the big map (TAB) and pause: note whether the map/menu panels look more transparent than with `radarbg:0`. If they do and it
   is unwanted, keep the default off or reduce with `radarbga`; if not, the option is a candidate for default ON.
4. If the square looks too light/dark vs the game window, try `radarbga:0.7|0.8`.
5. `summary-timing` unchanged (no new GPU work); `continuous-hud refused=0` (producer untouched).
