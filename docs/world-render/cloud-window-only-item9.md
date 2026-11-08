# Item 9 (clouds only in the forward rectangle): offline evidence pass, 2026-10-29

[E] No archived frame shows discrete clouds in the game window or in our eye mirror (step114 runM1a/L1a, k00a clear-sky flights: haze only), so
"game vs eye at the same moment" cannot be judged from the archive. A capture with clouds in view is needed.

[E] Log classes (step116 runP2 step5-producer.log; user flight headset-2026-10-28 prev log):
- 4caf6e2d GREATER (indexed): class `sprite`, replayed PER EYE under fxmem:6 (rep 65452 of 94016 seen in the user flight; plan lines refused=0, no-depth=0,
  out-win vertices present in about half the frames, i.e. the eye copies do project outside the window). The fx_sprite_gpu gate passes both GREATER and
  GREATER_EQUAL; window-fallback only 30/27049 (user flight) and 475/20835 (runP2, tracer no-depth refusals) frames.
- 4caf6e2d GREATER_EQUAL (cloud cards): deliberately ignored (gamecloud:0, ignored=28564 / 21326): neither in the window effects image nor per eye. Our own
  cloud sheet replaces them.
- d2464ed2 (missile smoke, vertex draws): smoke class, per eye under fxmem:4+. d34ee153: HUD, never admitted.

[I] With the day cfg no allowlisted draw should reach the window-only image except refused-frame fallbacks (0.1-2%). The rectangle clouds in the report
likely predate fxmem:6 (step105), or are the game's own scene image in the cockpit/window composite (not an effects class); not an unadmitted class.

Not changed in code: no evidence of an unadmitted cloud class, so no speculative admit.

## Live capture plan (one flight, clouds in view, current day install)
1. Mission/altitude with visible cloud decks (not k00a); day-install cfg (fxmem:6, gamecloud default 0), png-burst, `fxsig:4caf6e2d`.
2. Look 60-90 degrees off the window; capture the eye PNGs (`*-final`, `*-world`, effects-on-black) plus a game window shot in the same second.
3. Pass: cloud-like sprite masses present outside the rectangle in the eye image. Fail with `fx-sprite-gpu` committed == replays: the source is not an
   effects draw; log `phase-observe` scene-RT draws (ps8 4caf6e2d GREATER_EQUAL) and run once with `gamecloud:1` (diagnostic only) to see whether the
   rectangle clouds are the replayed cards.
4. Confirm xr-stereo `game-clouds;replay=off` and step5 `effects-gamecloud;ignored` growing.

## Correction (2026-10-29 evening, headset items 5/7; effects-outside-window.md section 15)
The statement "no allowlisted draw should reach the window-only image except refused-frame fallbacks" was wrong: class 1 took only INDEXED GREATER 4caf6e2d
draws; the NON-indexed 4-vertex quads of the same family (5 per frame in m02a, ~40-50 % of the family in m02a and m03k) stayed in the window effects image.
Fixed with fxmem:6 (they are class 1 now); live check in section 15.5.
