# HUD-MENU: smaller HUD on the gunsight glass, pause menu in front (D-362 headset test 19)

Code: `src/acz-vr/hud_menu.h` (pure), `Output::hud_menu_frame / hud_quad_params` (xr_stereo_output.cpp), one constant line in
`xr_stereo_d3d12.cpp`, one line in `xr_stereo_blit.hlsl` (mode 1), cockpit options `menu: menud: menub:` (cockpit_model.cpp),
`tools/live/vr-test-start.ps1` default `-HudScale 0.8` with `-CockpitModel`. Tests: `xr_stereo_cockpit_model_offline` (`hud_menu_tests`).
Tags: [E] evidence in logs/images, [V] offline test, [I] inference, [G] guess / not verified live.

## 1. HUD size (user: "HUD too big")

**Facts.** The day install draws the HUD quad at 10 m with `xr_hud_scale=1`: 63.20 x 38.17 deg, exactly the game lens (test19
`xr-stereo.log` `first-hud-layer`) [E]. At scale 1 the HUD registers angularly with the model's gunsight glass and the world
(cockpit-render-impl.md sections 11, 14). The j35j reflector glass spans 12.6 L / 12.8 R / 8.5 U / 9.3 D deg from the pvb eye
(step62 `cockpit-hud-glass` line): its angular centre is (+0.1, -0.4) deg, i.e. on the lens axis within 0.4 deg [E].

**Options evaluated.**
* Moving the quad (`xr_hud_quad_distance_m`, `hudglass:1`) never changes the angular size: `hud_world_quad` keeps every corner's
  direction at any distance. Distance only sets vergence/parallax (10 m = collimated, no head-turn parallax; test-17 lesson). Not a size control.
* A fixed-size quad moved farther would shrink the angle but also drift off the glass with every lean and is the old test-5 problem.
* **Chosen:** scale below 1 about the **glass centre** (`acz::hudmenu::scale_lens_about`): the HUD quad becomes `world_quad` of a
  lens whose edge tangents are pulled toward the glass centre, `edge' = pivot + s (edge - pivot)`, at the configured distance.
  The pivot ray is fixed [V], the size scales by `s` [V], `s = 1` is bit-identical to before [V]. Without a model glass the pivot
  is the lens centre (the old `scale_quad` about the quad centre, equal within 1e-4 [V]).

**Trade-off (unavoidable with any s != 1).** Only the pivot stays registered. A world-referenced HUD point (pitch ladder rung,
horizon line, flight-path marker away from the centre) at angle `a` from the pivot is drawn at `atan(s tan a)`: at s = 0.8 the
error is 1.0 / 2.0 / 3.8 deg at 5 / 10 / 20 deg and 5.4 deg at the HUD's horizontal edge (31.6 deg) [V, printed by the test].
The ladder therefore reads "compressed" by 20 %. The FRND/target boxes are NOT affected: they are on the separate markers layer
(world-quad pose, never scaled). The gun cross/boresight (lens centre) moves by (1 - s) x 0.4 deg = 0.08 deg on j35j: negligible.
Size at 0.8: 52.4 x 31.0 deg. The consumer logs `event=cockpit-hud-registration;hud-scale=0.800;pivot=glass-centre;...;error-at-10deg=1.96deg`.

**Default.** `vr-test-start.ps1 -CockpitModel` now passes `xr_hud_scale=0.8` (was 1.0); `-HudScale 1.0` restores exact registration,
0.7 / 0.9 are the next steps. The xr_hud_scale key range is unchanged (0.25..2).

## 2. Pause menu behind the HUD

**Where the menu is drawn [E].** In the game window the PAUSE MENU is a yellow-framed, dark translucent panel with yellow text drawn
over the HUD, which shows dimmed through it (`step14-live-run11-full/game-window-shots/acz-20261024-125357.png`). In test 19 the
producer's HUD region has long stretches of bit-constant draw counts (95, 98, 100, 93 draws per frame for minutes = frozen scene)
against 54-65 in flight; `hud-end` at 95 s: `draws-seen=106; post-excursion-draws=48` (flight: 54 and 4), all `r` (replayed),
`refused=0`, `end=scene-barrier` in 100 % of frames. So the menu draws are captured INTO the HUD image, after the HUD and the radar
group, in game order. Markers per frame do not change in those stretches (the menu is not in the markers layer).

**Why it looks "behind" [I, strong; G until a pause PNG].** The HUD layer alpha is `max(src.a, max(rgb))` because the HUD target's
alpha is not a coverage mask: it is 0 everywhere except the radar stencil box (`L1-logs/20261024-165114/png/*-s1-hud-alpha-raw.png`) [E].
The menu's dark backdrop (rgb near 0) therefore gets alpha near 0 = transparent: in VR the bright cockpit model, world and the HUD
(only dimmed by the panel inside the HUD image) show through and around the menu text at full brightness, so the HUD and cockpit
compete with the menu instead of being covered by it. Layer ORDER is not the cause: the HUD quad is already the last (topmost)
layer (`order=projection-then-markers-then-hud`) and the menu is inside it.

**Fix: menu mode (consumer only, default ON with a model cockpit, option `menu:0` disables).**
* Trigger (revised after the step83 R1 false trigger: parked on the runway in the takeoff sequence the camera is frozen for 28 s
  with 51-54 HUD draws [E]): 20 consecutive fresh game frames that are BOTH camera-frozen (`StereoFrame::game_eye` + `game_cam_fwd`
  bit-identical) AND carry the menu HUD signature `hud_draws >= max(80, baseline + 25)`, baseline = running mean (1/64) of the
  non-menu frames only. Evidence: pause 93-106 (test19) / 94-102 (R1) draws vs 49-65 in flight and on the runway [E]. Exit when the
  signature is gone for 3 frames (resume drops the menu draws at once), the camera moves for 10 frames, or the camera is invalid;
  a one-frame camera change inside a pause (seen in R1) no longer flickers the menu [V]. Logs carry `threshold`, `baseline` and
  `frozen-rejected` (runway frames refused). A frozen big map is caught only if it also adds >= 25 HUD draws [G].
* While on: the HUD quad is shown at the game's angular size (scale 1, lens-centred, ignoring `xr_hud_scale`) at `menud` metres
  (default 2.0, range 0.5..20) and the HUD blit gets an alpha FLOOR `menub` (default 0.6, range 0..0.95) in `mode_bits` 24..31 of
  the HUD target only: every layer below (cockpit, world, markers) is darkened by 60 % like the game's panel, the menu text and
  frame stay at full brightness on top. Floor 0 = exactly the old alpha [V].
  RADAR-BG (radar-background.md, `radarbg:1`, default off) uses `mode_bits` 19..23 of the HUD target; the floor is applied after it, unchanged.
* Logs: `event=hud-menu-config` (with the cockpit lines), `event=hud-menu;state=enter(frozen-camera)|exit;hud-draws=..` (first 40).
* Limits [G]: the backdrop darkens the WHOLE HUD quad (52-63 deg), not just the panel rectangle; the HUD symbology inside the image is
  shown with the menu (as in the game, dimmed by the game's own panel). A frozen camera from a hitch < 20 frames never triggers.
  Head-locked HUD mode and runs without a model cockpit (no options) keep the old behaviour.
* UI3D stage 3a (`;ui3d:1;u3p:1` in xr_terrain_stage, default off; ui-3d.md 7, not run live): menu mode is also the trigger for the pause
  menu's per-eye depth layers, blended into the eye images over the cockpit dimmed by `menub`; while that blend is submitted the HUD quad is
  not submitted. Any failure keeps the behaviour above.

## 3. Interface for the virtual-screen route (vscreen; another agent)

Not implemented here. Compatibility contract:
* Topmost-layer precedence: vscreen (when it shows the full game image) > menu mode > in-flight HUD. A vscreen implementation should
  either suppress menu mode while it owns the view (set the cockpit option `menu:0`, or gate `hud_menu_frame` on its own state) or
  reuse `menu_on_` as its trigger in flight. Layer order stays back-to-front world/markers/projection/HUD; a vscreen quad, if it is
  a separate layer, goes AFTER the HUD.
* `acz::hudmenu::Detector` is a reusable frozen-scene trigger (pure, per fresh frame).
* `mode_bits` 24..31 are reserved for the HUD-target backdrop; other targets must leave them 0 (the d3d12 pack ORs them only for `kHud`).
* The menu quad is `hud_world_quad(neutral, lens, ., menud, 1, yaw)`: a vscreen placed at its own distance should use the same anchor
  (neutral + yaw0) so switching between the two does not jump.

## 4. Live check list (lead's driver; nothing here was run live)

Install the build with `-CockpitModel 'auto;anim:1;default:j35jk_vrcockpit'` (day config) and no explicit `-HudScale`.
1. `xr-stereo.log`: `hud-config ... hud-scale=0.800`, `cockpit-hud-registration;hud-scale=0.800;pivot=glass-centre`,
   `hud-menu-config;menu=1;menu-distance-m=2.00;menu-backdrop=0.60`; `first-hud-layer hud-angular-deg` ~52.4x31.0.
2. Flight 60 s: no `event=hud-menu;state=enter` (false trigger check) while flying, also during slow turns / straight flight.
3. Press Start (pause) for 10 s, resume, twice: one `enter` within ~0.4 s of each pause and one `exit` on resume; HUD draws in the
   enter line ~95-106. Mirror/png: menu text bright, background darkened, HUD quad nearer (2 m) and full size.
4. Open the big map in flight: note whether it triggers menu mode (frozen scene or not) and whether that is wanted.
5. Producer `continuous-hud` refused stays 0; summary-timing unchanged (no new GPU work).
6. Headset: HUD smaller and still centred on the gunsight glass; ladder compression acceptable? Try `-HudScale 0.7/0.9`, menu `menud:1.5|3`, `menub:0.4|0.8`.
