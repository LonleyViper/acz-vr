# Takeoff segment: the airbase stage k00a (TAKEOFF)

Tags: **[E]** evidence, **[I]** inference, **[U]** unknown. Offline work only (no live run, no game data in git).

## Problem (headset test 19, mission 02)

"Takeoff from the airbase at the start doesn't load the correct map." Logs: `D:\acz-evidence-archive\test19\logs-live`.

## What the logs show [E]

- Stage detection ran once, at XR start, while the aircraft was parked on the runway:
  `276609765 terrain-auto;result=match;stage=m02a;best=24;second=4;top=m02a:24,k00a:4` (decisive stop after 11 MB of the 64 MiB
  EE block), `terrain-auto-selected;stage=m02a`. It never changed (detection is once per process, section 3.3 of stages-and-detection.md).
  m02a IS the mission stage (Operation Roselein, Arlon): the detector was right about the mission.
- E6 lock (`ee-camera.log` 276614754): aircraft at **(17110.4, 1846.88, 21346.6)**, heading 0, stationary; the game camera eye
  (camlag `ge`) 17110.4, 1851.9, 21311.9. The roll starts at ~276617 (present-stats 737 then 1144 units/s).
- At ~276629 the aircraft position cuts to **(-33573, 4000, 141234)** (`obs2` C record), frozen ~2.5 s, then flies on in the m02a mission
  area (camlag from 276672: (-21633, 2361, 116517) ...). The takeoff segment and the mission area are two different maps.
- Heights of the installed bakes (`%LOCALAPPDATA%\acz-vr\stage\*.zvrs`, bilinear) at the runway:

  | stage | ground at (17110, 21312) | strip x=17110, z 12000..28000 |
  |---|---|---|
  | k00a | **1840** (byte 23) | **1840 flat**; across x 13000..19000 at z=15000 flat 1840 (a 6 x 20 km plateau) |
  | m02a | 480 | 0..480 |
  | all 37 others | none within 5 of 1840 except m15a (1846 at one point, not flat) | - |

  The aircraft origin sits 6.9 units above k00a's plateau (gear) and 1367 above m02a's ground. k00a's preview
  (`D:\acz-extract\stage-all\preview\k00a.png`) is a snowy repeating filler with one central feature, which is this plateau.
  In the mission area the reverse holds: at (-21633, 116517) the aircraft at 2361 is under k00a's ground (4745) and above m02a's (0).

**Root cause:** the takeoff segment of mission 02 is played on a separate airbase map, stage **k00a** (until now listed as "no title card,
tutorial/demo [I]"), and the game cuts to the m02a mission area after the takeoff. The renderer loads exactly one stage per process (the
detected mission stage), so during the takeoff it drew m02a's landscape ~1360 units under the runway. Not a detector, fingerprint, tile or
origin error, and no bake was missing: k00a was already baked and installed (39/39 stages).

Why the null-driver runs never saw it [I]: they start the mission in flight (mission start at ~(54053, 8816, -49174) in m02a), past the takeoff.

## Fix

- Option `takeoff:<id>|off` inside `xr_terrain_stage` (default **k00a**; no new config key). The proxy loads the takeoff stage next to the
  mission stage (terrain, atlas, env, props/trees with the same options; generation = mission + 1), skipped when off, when it equals the
  mission stage, or when its file is missing (`terrain-takeoff-off;reason=...`). Logged: `terrain-takeoff-loaded;takeoff=k00a;mission=m02a;...`.
- `acz::terrain::TakeoffSelector` (terrain_stage.h, SDK-free) is fed the frame camera eye of every fresh composite:
  - ENTER the base only while armed (session start, or a cut that lands within 30000 units of the first takeoff spot = a mission restart)
    after 30 consecutive samples that are parked (horizontal move <= 4 units/sample), at cockpit height over the base ground
    (-40..+200) and >= 300 above the mission ground.
  - LEAVE on a cut (one-sample move > 20000) or when the eye is > 60 under the base ground; leaving disarms until a cut.
  - The consumer draws the base runtime's stage/atlas/env/props while active (`drawn_terrain()`), everything else (conventions, camera,
    tactical, cockpit stage light) stays on the mission runtime. The D3D12 backend re-uploads on the generation change; props restart
    their async init (terrain without props for that time). Each switch logs
    `terrain-takeoff;drawn=k00a|m02a;switch=to-takeoff-base|to-mission;reason=parked-on-base-ground|cut|under-base-ground;eye=..;base-ground=..;mission-ground=..`.
- Tests (`terrain_offline_tests`, `test_takeoff`): option parsing; synthetic k00a/m02a-shaped stages along the test-19 path (29 parked
  samples = no switch, the 30th enters; roll and climb stay; the cut leaves; frozen mission start and slow low flight over the base hills
  never re-enter; a restart back on the runway re-enters; under-ground leaves and disarms); in-flight start, mid-roll start, identical
  stages, 250 over the runway and unusable input never enter; and the real installed k00a/m02a bakes with the logged test-19 points.

## Look: brown takeoff terrain (TAKEOFF-LOOK)

Symptom [E, step83 R1b]: the upright XR dump `runR1b-logs\png\acz-vr-xr-eye0-f390-final.png` (k00a drawn, `terrain-takeoff;drawn=k00a`) shows brown hills
(47,42,33) under an olive sky (86,87,77); the game-window shot of the same parked moment (`runR1b-archive\shots\...-T10.png`) shows snowy grey hills
(106,106,111), neutral tarmac (55,53,53) and a blue sky (97,109,133).

Cause, two parts [E unless marked]:

1. **Environment**: the takeoff stage was drawn with k00a's OWN baked env. Its sky is brown in all 12 sph stops (R > B; row 0 horizon (106,89,64), row-2
   horizon = look:1 fog F (65,62,56)), warm light (58,50,33), ambient (130,111,87), sun 8 deg high. The game's airbase sky is blue (B > R by 36). m02a's sky
   has B > R in all stops. So the game does not show k00a's baked env in mission 02's takeoff [I: the airbase segment keeps the mission's env, or k00a's
   mapset/sph is a default the mission overrides].
2. **Look gain**: look:1 multiplies the ground by kLookGain (0.64, 0.61, 0.50), fitted on m02a's green fields. k00a's texels are grey/white (usage-weighted
   mean (115,110,100), 22 % near-white; snow CLUT entries (229,223,206)): the 22 % blue cut makes snow khaki (R - B +43/255 vs +12 neutral). A quantile fit of
   the T10 airbase terrain (5 luminance quantiles, game = texel x g x (1 - f) + sky x f) gives a NEUTRAL g = 0.54 (f = 0.22), rms 7.0/255; kLookGain on the
   same model rms 12.7/255. Not causes: atlas/CLUT decode (the same path as m02a, alpha 128 CLUT read as RGBA like m02a), the sea rule (k00a has no sea
   words on the plateau), sRGB handling (shared).

Fix (default; m02a unchanged):

- `toenv:mission|own` inside `xr_terrain_stage` (default **mission**): the takeoff runtime adopts the mission stage's env (params, cloud/wave/sprite maps)
  before its env texture is built (`adopt_mission_env`, both the monitor load and the VSEARLY per-flight load). `terrain-takeoff-loaded;...;env=yes(mission)`.
  `toenv:own` restores k00a's own sky.
- `look_gain_for(stage)`: kLookGainNeutral (0.54, 0.54, 0.54) +-0.06 on k00a, kLookGain on every other stage (m02a's env texture bit-identical).
- Tests (`terrain_offline_tests`, `test_takeoff_look`): option; gains; adoption (params + maps, not with `toenv:own`, not without a mission env, not onto
  itself); the takeoff env texture equals the mission's except the 3 gain channels; real bakes: k00a sky R > B in 12/12 stops, m02a <= 2; m02a gain =
  kLookGain; k00a snow R - B old > 20/255, new < half.

Not verified: no live run. The neutral gain is a quantile fit to ONE shot (not registered pixels); the game's own takeoff haze colour is not known exactly
(mission F (85,102,98) is used, the shot's sky is bluer). Other stages used as takeoff bases (if any) keep kLookGain.

## Not verified [U]

- Live: nothing here has run in the game. Whether the cut is always one sample (a fade with E6 zeros would leave via under-ground,
  equally correct), whether other missions with a takeoff also use k00a (the rule only engages when the aircraft is parked on k00a's
  ground), and the cost of the switch (one terrain upload ~ the start-up upload, plus the props worker) are unmeasured.
- k00a's env/look: see section 'Look' (mission env + neutral gain since TAKEOFF-LOOK; compared with one R1b game shot only).
- Props dedupe (WORLD-4) still lists only the mission stage's placements; the airbase segment's game buildings are replayed as before.
- The cockpit stage light/canopy colour keeps the mission stage's env during the takeoff.

## Live check list (mission 02, from the hangar to the mission area)

1. xr-stereo.log at start: `terrain-auto-selected;stage=m02a` and `terrain-takeoff-loaded;takeoff=k00a;mission=m02a;...;props=loaded`.
2. Parked on the runway: within ~0.5 s of the first frames `terrain-takeoff;drawn=k00a;switch=to-takeoff-base;reason=parked-on-base-ground;
   eye=17110.4,1851.9,21311.9;base-ground=1840.0;mission-ground=480.0`.
3. In the headset on the runway: the ground directly under/around the aircraft is at gear height (no drop of ~1400 units), snowy airbase
   terrain with a flat strip ahead along the heading (north, -Z), its hangars/buildings (k00a props) where the game window shows them;
   during the roll the terrain moves past with no step or seam when it lifts off.
4. At the takeoff cut (~11 s after the roll starts in test 19): `terrain-takeoff;drawn=m02a;switch=to-mission;reason=cut` and the m02a
   landscape (river, green fields) from then on; at most a brief terrain-without-props moment, no stall.
5. No further `terrain-takeoff` line during the mission (unless the mission is restarted from the runway: then step 2 again).
6. If the runway is still wrong: report the `terrain-takeoff*` lines; `takeoff:off` restores the previous behaviour.
