param(
  [ValidateSet('Cruise','Kill','Gun','Sun','Chase')][string]$Mode='Cruise',   # batch M: Gun = GUN-STRIKE (shallow pass on a stationary ground group, J burst), Sun = SUN-LOOK (flare capture)
  [string]$Dry,                     # replay an archived log dir (ee-camera.log [+ xr-stereo.log]); prints commands, sends nothing
  [string]$Events,                  # -Dry only: run83-events.txt of that run, to print the keys the open-loop driver really pressed
  [switch]$Sim,                     # closed-loop simulation on the plant model fitted from the archived runs (no game, no keys)
  [string]$SimFrom,                 # -Sim: 'logdir@T' take the initial aircraft state from that log at T seconds of flight (else the mission start)
  [string]$LogDir=(Join-Path $env:LOCALAPPDATA 'acz-vr\logs'),
  [double]$Total=600,               # live/sim: seconds of flight to run
  [double]$Period=2.0,              # decision period (s); one roll tap and one pitch tap at most per period
  [double]$TargetAlt=5000,          # altitude (aircraft y, game units) to hold; ground of m02a is ~480 (flat map)
  [double]$AltFloor=2500,           # below this: emergency climb
  [double]$AltCeil=12000,           # above this: nose down
  [string]$TargetColors='green,yellow,red,unknown', # Kill: identity colours treated as targets (blue = friendly, never); 'unknown' (white, not yet identified) only for GROUND contacts outside -HomeRadius (batch I: ids 39-44, the nearest ground groups, stayed white until ~10 km and were ignored)
  [double]$PriWeight=6000,          # Kill: target score = distance + index-in-TargetColors * PriWeight (the NEAREST group wins; a kill within ~250 s needs the nearest one)
  [double]$GroundMaxV=450,          # Kill: contact speed below this = ground (batch I: the green TGT vehicle moves 135-220 u/s and was classed 'air' by the old 120 limit; aircraft are 640-1500)
  [double]$HomeX=-24500,            # Kill: mission start (airfield); unknown ground contacts within HomeRadius of it are our own base, never targets
  [double]$HomeZ=132000,
  [double]$HomeRadius=22000,
  [double]$MissileRangeGround=9000, # Kill: ground targets: missile window rng < this ...
  [double]$MissileMinGround=2400,   # ... and > this (6-9 km is the target window; two missiles fit between 9 and 3.2 km)
  [double]$StrafeAlt=2000,          # Kill: ground targets: below 9 km and aligned, descend gently to this altitude so the gun range (1500-2200 u slant) is reachable; 0 = stay level at GroundAlt
  [double]$GroundGunRange=2600,     # Kill ground strike: start the burst inside this slant range with the nose on the target (UNVERIFIED; batch J never got there)
  [int]$TargetId=0,                 # Kill: force one mk360 contact id
  [double]$MissileRange=12000,      # Kill: fire only inside this range (UNVERIFIED; batch L: 12 km)
  [double]$GroundElevDeg=40,        # Kill: elevation tolerance for ground targets (homing missiles): a level 4 km pass sees the target 21 deg below at 9 km and 33 deg at 5.5 km
  [double]$GroundAlt=2800,          # Kill: ground targets are attacked from LEVEL flight at this altitude (batch H: descending to 2900 u capped the bank and overflew the target); no dive below it
  [double]$GunRange=1500,           # Kill: hold J (gun) when closer than this and aligned within GunDeg
  [double]$GunDeg=4,
  [string]$FirstWp='16000,80000;11000,57000;14000,18500;16200,9100', # Kill: ground-site priors for mission 02 ('x,z;x,z'; batch H/I contact positions: white group 40-45 at 16k,80-90k, yellow+green TGT group 11k,55-60k, green group 14k,18k, 16k,9k), flown in order until a target is tracked; '' = off
  [double]$AlignDeg=10,              # batch L: was 5 (never opened live)              # Kill: bearing error (deg) to fire
  [double]$FireEvery=3.0,           # Kill: seconds between missiles
  [int]$FireSalvo=0,                # OPT-IN launch-signal search (aircraft-auto.md 13): fire this many missile taps, -FireEvery s apart, from flight second -FireAt, REGARDLESS of lock (an unlocked missile flies straight); replaces the lock-gated missile decisions. 0 = off
  [double]$FireAt=60.0,             # -FireSalvo: flight second (T) of the first tap
  [int]$FireSp=0,                   # OPT-IN (SP-STORES, aircraft-auto.md 15): after the MSSL salvo fire this many SPECIAL-weapon taps (the HUD SP row: SAAM ---8), -FireSpEvery s apart from flight second -FireSpAt, regardless of lock. 0 = off
  [double]$FireSpEvery=7.0,         # -FireSp: seconds between SP taps (the HUD reader needs ~0.5 s per change; keep it off 15 s multiples like -FireEvery)
  [double]$FireSpAt=-1,             # -FireSp: flight second of the first SP tap; -1 = the last MSSL salvo tap + 10 s (or T=60 without -FireSalvo)
  [string]$SpSwitchKey='',          # -FireSp: the in-flight key that selects the special weapon ('E' or '0x45'); ONE tap, -SpSwitchLead s before the first SP tap. UNKNOWN for this game (no doc or evidence names it): empty = no switch tap, the SP taps then use the missile key L and fire whichever weapon is selected
  [double]$SpSwitchLead=1.5,        # -FireSp -SpSwitchKey: seconds between the switch tap and the first SP tap
  [double]$FireRollMax=0.10,        # -FireSalvo: tap only while |roll| (rad) is below this (wings level) ...
  [double]$FireWaitMax=4.0,         # ... but never wait longer than this past the nominal time (the tap is then logged WAITED)
  # --- batch K strike routine (offline; batch J: 6 missiles at 7.8-2.5 km all MISS = never locked: the level 4 km pass kept the ground target 11-19 deg BELOW the nose, under the HUD window, so no lock) ---
  [double]$LockDeg=9,               # batch M default (was 3.5; live L1b/L1c needed -LockDeg 9 -LockSec 0.5 -DiveFrom 9000 to fire at all). Kill: the target must sit inside the reticle (bearing AND elevation error < this) ...
  [double]$LockSec=0.5,             # batch M default (was 1.6) ... for this long before L is pressed (the game locks after ~1-2 s in the reticle; a missile without lock flies straight and shows MISS)
  [double]$LockRange=9000,          # Kill: lock/fire window, slant range (upper limit; unverified, J fired from 7.8 km)
  [double]$DiveFrom=9000,           # batch M default (was 13000: the long dive piled up stale-state pitch overshoot, elvErr 10-20 deg). Kill ground strike: below this horizontal range and aligned, point the NOSE at the target (pitch = line of sight) so it is in the reticle
  [double]$AimLift=250,             # Kill ground strike: aim this far above the ground point (keeps the pull-up clear of the terrain)
  [double]$PullY=1000,              # Kill ground strike: hard floor: below this altitude pull up (ground ~500 u)
  [double]$PullLag=2.6,             # Kill ground strike: pull-up when height above the target < 150 + V * nose-down pitch * this (s): the plant needs ~2.6 s (tap lag + ~0.1 rad/s) to level off, i.e. it sinks ~V*pitch*1.3
  [double]$PullHold=0.7,            # Kill ground strike: S hold (s) per pull-up tap (0.33*(h-0.11) rad each; holds > 0.45 s are UNVERIFIED live)
  [double]$GunBurst=2.0,            # Kill ground strike: J held this long
  [double]$ReAttackAlt=3200,        # Kill: after an overflight climb only to this altitude, then turn in again
  [double]$BreakStraight=10,        # Kill ground strike: after the pass fly wings-level this long (climbing) before the turn back: the 180 deg turn at the safe bank has a ~15 km radius, so the turn-in needs the extra runway for a 4+ s lock
  [int]$Salvo=3,                    # Kill: missiles per pass (reset at the next break)
  [double]$PreferStatic=3000,       # Kill: stationary ground targets score this much closer than movers (a lead on a mover is the hard case)
  [string]$DecisionLog,             # file for the decision log (live default: autopilot-<time>.log next to this script)
  [switch]$NoFire,                  # Kill: steer only
  [string]$SimTarget,               # -Sim -Mode Kill: synthetic static ground target 'x,y,z' (green contact, reported as an mk360 snapshot every 5 s)
  [string]$SimStart,                # -Sim: start state 'x,y,z,heading' (regression cases), e.g. an offset approach
  [switch]$SimL4,                   # -Sim -Mode Kill: replay the batch-L4 contact pattern: the target's mk360 id is REUSED by a wingman (r~190, v~1100) on every other snapshot
  [double]$SimTargetV=0.0,         # -Sim: the synthetic target moves at this speed (u/s) along -SimTargetHdg
  [double]$SimTargetHdg=0.0,
  [string]$SimTargetRgb='0.29,1,0.22', # -Sim: the target's identity colour as r,g,b ('1,1,1' = unidentified white)
  [double]$SimRange=0.0,            # -Sim: the target only appears in the mk360 snapshots within this range (the game listed ground targets from ~28 km); 0 = always
  [switch]$SimHomeJunk,             # -Sim: static white ground contacts around the airfield (batch I: ids 1-8 at 2-5 km on the ground) that must NOT be attacked
  [double]$SimLockDeg=3.5,          # -Sim: the SIM's lock model (independent of the controller): target inside this cone (bearing AND elevation of the nose) ...
  [double]$SimLockSec=1.5,          # -Sim: ... continuously for this long = a missile fired now is locked (else it flies straight and misses, as in batch J)
  [double]$SimFlipRange=0.0,        # -Sim: batch K2 'KA2' case: inside this horizontal range the target stops being listed and a YELLOW ground contact with the SAME id appears -SimFlipDeg off the nose, 6 km ahead
  [double]$SimFlipDeg=45.0,
  [double]$LowBankAlt=1500,         # batch K2 (KA2 crashed after a target switch with a 0.54 rad bank at 1100 u): below this EFFECTIVE altitude (y + 2 s of sink at the current pitch) the bank is limited to -LowBankRoll and the nose goes up
  [double]$LowBankRoll=0.15,
  [double]$DiveBankRoll=0.35,       # ... but 0.35 (0.20 under floor+500, as before) during the deliberate, aligned strike dive (a 180 u/s mover cannot be followed at 0.15)
  [double]$PullSafeAlt=1400,        # batch K2 (KA3 stayed latched in PULLUP and flew away): the pull-up latch releases when the nose is up, the altitude is above this and >= 600 u above the target, or after -PullMaxSec
  [double]$PullMaxSec=6,
  [double]$SwitchBias=6000,         # the current target keeps priority: another track must beat it by this much (distance + colour priority score) to take over
  # --- batch M ---
  [switch]$NoPredict,               # switch OFF the pending-key compensation of the stale attitude read (live: the newest ee-camera line is 0.4-1.6 s old; the S/W taps of the last 1-2 s are not in it yet, which wound the dive up to pitch -0.40)
  [double]$GunAlt=1900,             # Gun: level approach altitude (1500-2500)
  [double]$GunDiveFrom=10000,        # Gun: below this horizontal range and aligned the NOSE is put on the target (line of sight, no aim lift)
  [double]$GunMaxDive=0.22,         # Gun: steepest dive (rad)
  [double]$GunFireRange=2600,       # Gun: hold J inside this slant range with the nose on the target
  [double]$GunStrikeBurst=2.5,      # Gun: J held this long (s)
  [double]$GunPullY=850,            # Gun: hard pull-up floor, judged on the height at the NEXT decision. Sim (3 scenarios, with -GunPullBase 100, lag 1.8): min y 745-792 and a burst in every case; the aggressive profile -GunPullY 800 -GunPullLag 1.2 puts the nose on target 0.8 s but bottoms at y 554 (54 u above the ground): only for a measured live test
  [double]$SunHdg=2.269,            # Sun: autopilot heading (rad, left turn = +) of the stage sun. m02a: mapset +0x5C heading -130 deg (-2.269) -> world horizontal (sin h, -cos h) = (-0.766,0.643) = terrain_stage.h kSun (-0.42,0.80,0.43); HdgTo gives +2.269 (the sign flips)
  [double]$SunElev=0.8727,          # Sun: sun elevation (rad) = mapset +0x58 (50 deg for m02a)
  [string]$SunMapset,               # Sun: optional path of data/mapset/map/mapset<stage>.bin: reads +0x58 (elevation) and +0x5C (heading, sign flipped) for another stage
  [double]$SunHold=20,              # Sun: seconds to hold the sun heading (aligned) before returning to the cruise
  [double]$SunPitch=0.25,           # Sun: pitch while looking (rad); 0 = level but the sun (50 deg) is then far above the nose; the climb is ~V*sin(pitch) = 270 u/s at 0.25
  [double]$SunAltMax=10500,         # Sun: above this altitude the look is flown level
  [double]$SunStartAlt=3000,        # Sun: approach altitude (leaves room for the climb)
  [ValidateSet('auto','m02a','m03k')][string]$Stage='auto', # area constants: m02a (the original tuning) or m03k (Mission 03, airborne start ~105k,20000,106k); auto = m03k when the first valid position is within 30 km of that start, else m02a
  [string]$SunRun,                  # Sun: 'x,z' run-in point (default: SunHold*V + 40 km up-sun of the centre, so the aircraft arrives from the right side and has room to turn onto the sun heading)
  [double]$SunArrive=6000,          # Sun: run-in point reached inside this distance
  [double]$SunAlignTol=0.12,        # Sun: heading error (rad) that counts as aligned
  [double]$SunGiveUp=150,           # Sun: stop trying after this many seconds in the look phase
  [double]$FreezeMax=45,            # frozen aircraft state: wait this long (cutscene) before treating it as mission over
  [switch]$NoArmFx,                 # batch M live: do not touch fxdiag.arm at T0 (an external helper arms it when the sun look starts; the 400-line cap is spent in ~16 s)
  [double]$BlindMax=240,            # live: longest dead-reckoned flight (the aircraft struct is zero but the hook still runs) before the script stops
  [double]$HookDeadSec=30,          # live: no fresh ee-camera line AND no fresh mk360 snapshot for this long = the game/hook is gone: stop
  [double]$SimStateAge=1.5,         # -Sim: the attitude the controller reads is this many seconds old (live: 0.4-1.6 s)
  [double]$SimBlindAt=0,            # -Sim: from this flight second on the aircraft struct reads zero (live L1a: T62); 0 = never
  [double]$LookSec=110,             # batch W: seconds of hands-off look-up before the run ends
  [switch]$SunLookUp,               # batch W: hold numpad 8 (look-up, camera view only) while the sun phase 1 (heading hold) runs; re-asserted every 4 s, released at phase 2
  [double]$LookEvery=0,             # item 9 (batch T2): every this many seconds hold numpad 4 / numpad 6 (look-left / look-right) alternately for -LookHold s, from -LookStart s; 0 = off
  [double]$LookHold=3,
  [double]$LookStart=30,
  [double]$DipAlt=0,                # OPT-IN (2026-10-31, cloud runs): Cruise/ALT branch only: from flight second -DipFrom for -DipFor s hold this altitude instead of -TargetAlt, to fly through the cloud decks. 0 = off. The ALT branch stops commanding inside a 700 u dead band and the climb/descent is <= 150 u/s, so the aircraft settles at DipAlt+700 at best: keep DipAlt >= 4500 (terrain, the -AltFloor emergency climb) and <= 5800 so it ends below the y~6500 aircraft-struct blind limit.
  [double]$DipFrom=0,
  [double]$DipFor=0,
  [switch]$LookAtContacts,          # batch X: when a mk360 contact lies LookMinDeg..LookMaxDeg off the nose within LookRange, hold look-left/right (numpad 4/6) toward it for -LookHold s (then cool down -LookCool s); logged as 'LOOKC'
  [double]$LookMinDeg=45,
  [double]$LookMaxDeg=115,
  [double]$LookRange=22000,
  [double]$LookCool=4,
  [switch]$HeadLook,                # NULL-HEAD (2026-11-02, docs/world-render/null-head-look.md): wherever -LookEvery / -LookAtContacts / -SunLookUp would press a numpad look key, write
                                    # %LOCALAPPDATA%\acz-vr\head.now instead ('yaw pitch hold'; yaw + = LEFT): the synthetic HMD head turns, the GAME camera stays forward. Needs nh:1 in xr_terrain_stage.
                                    # No hands-off (the attitude read is not corrupted). Logged as 'HEADLOOK'/'HEADLOOKC' (cloudtrig's look gate matches them). The old keys stay the default.
  [double]$HeadYaw=90,              # -HeadLook: |yaw| of a look-left (+) / look-right (-) for -LookEvery
  [double]$HeadLookPitch=0,         # -HeadLook -LookEvery: pitch of each left/right look (+ = up); 53 checks canopy glass above the sill (testhmd-first-flights.md 6.3)
  [double]$HeadUpPitch=60,          # -HeadLook: pitch of the -SunLookUp look-up
  [double]$HeadSlew=90,             # -HeadLook: the DLL's slew speed (nhs:, default 90 deg/s): used only to time the eye shot and the logged look window
  [switch]$HeadPose,                # -HeadLook via the TEST HMD driver (docs/world-render/test-hmd-driver.md): write testhmd-pose.txt 'yaw pitch roll slew' and send it home after the hold (no nh:1 needed)
[switch]$NoHeadShots,             # -HeadLook: do NOT write png-eye.now once the head has arrived (default: one eye dump per head turn; needs xr_png_once)
  [switch]$SunZoom,                 # batch Y: SUN ZOOM CLIMB: aligned on the sun heading in level flight at ~ZoomBase, then nose up to ZoomPitch until y > ZoomTop (or ZoomMaxSec), nose down back to ZoomBase+1200, repeat ZoomMax times; no look key; logs 'ZOOM FIRE' at each start (killcap fast-mode trigger)
  [double]$ZoomPitch=0.8,
  [double]$ZoomBase=2200,
  [double]$ZoomTop=4800,
  [int]$ZoomMax=4,
  [double]$ZoomMaxSec=9,
  [double]$OffsetDeg=0,             # batch X: fly the approach to a ground target with it this many degrees off the nose (+ = target to the right); 0 = off
  [double]$OffsetUntil=3500,        # batch X: ... down to this range (then the normal strike)
  [double]$ChaseTrail=3000,         # Chase (batch Z4): once inside 8 km the aim point trails the air target by this far along its track (ends up 2-4 km behind/at its side, not an overshoot)
  [double]$ChaseMin=800,           # Chase: 'hold' counts while the range is inside ChaseMin..ChaseMax and the target within ChaseDeg of the nose
  [double]$ChaseMax=8000,
  [double]$ChaseDeg=30,
  [double]$SimTargetTurn=0.0,       # -Sim: the synthetic target turns at this rate (rad/s, + = left like the aircraft heading)
  [double]$SimNoise=0.0,            # -Sim: random roll disturbance (rad/s) to emulate wind/turbulence
  [double]$HoldLock=0,
  [switch]$NoLockShots,
  [string]$SwitchKey='I',            # LOCK-HOLD: the game's target-switch key (PCSX2 Pad1 Triangle = Keyboard/I; ACZ change target [I: AC-series default, not verified live]). runLOCK2: the box ahead was not the game's selected target (no label, the target wedge pointed elsewhere): no lock
  [switch]$NoSwitch,                # LOCK-HOLD: never tap the target-switch key
  [double]$HoldBank=0.45,           # LOCK-HOLD on an air target: bank limit (rad) above AltFloor+3000; > 0.45 = tighter turns (sim: 0.6-0.75 did NOT improve the hold, so the default leaves the bank unchanged)
  [double]$HoldAspect=15000,
  [double]$HoldDeg=20)              # LOCK-HOLD: once held 1 s inside -LockDeg, the hold continues while the target stays within this cone (deg) [I: ACZ keeps a lock inside its seeker area]        # LOCK-HOLD: target score penalty (units) x cos(aspect) for a track flying TOWARD us (head-on passes give 3-7 s in the reticle at ~1150-2300 u/s closing; runLOCK1/2)            # LOCK-HOLD: do not write png-eye.now / png-screen.now at 3 s of hold and at LOCK-HELD              # LOCK-HOLD (markers-360.md 18.3; Kill/Chase): keep the nose ON the nearest target (no lead, -PriWeight 0 unless given) and fire NOTHING until it was held in the reticle this many seconds without a break (the game's lock diamond for the mk360cen:1 census); then normal. 0 = off
$ErrorActionPreference='Stop'
# ---------------------------------------------------------------------------------------------------------------------------------
# step83 closed-loop autopilot. Sources (all written by the DLL while flying, nothing else is read):
#   ee-camera.log  `ee-camera;obs;f=..;k=e|b;fields=lp,ly,lr,hdg,ax,ay,az,cx,cy,cz,rp,ry,rr;A=base,size,flag:<13 floats>`  1-6 lines/s
#       hdg = heading rad (velocity = (-sin h, -cos h) in x,z; LEFT turn = h increasing), ax/ay/az = aircraft position (y up),
#       rp = pitch rad (+ = nose up), rr = roll rad (+ = left bank). Tick (line prefix) = GetTickCount64 ms (same clock as xr-stereo.log).
#   xr-stereo.log  `event=mk360-contacts;contacts=id:win|out[+box]:r<range>:b<bearing deg, + = right of nose>:e<elevation deg, world>:v<speed>:id|none<r,g,b>:g<n>` every 5 s (needs mk360:1)
# Control response (fitted on 38 archived runs, notes: docs/world-render/autopilot.md):
#   D / A hold h s  -> roll change  -/+ 0.95*h rad, settled in ~1.5 s, no self-levelling (roll stays)
#   S / W hold h s  -> pitch change +/- 0.33*(h-0.11) rad (dead zone 0.11 s); W unverified (mirror of S), pitch stays
#   heading rate = 0.15*tan(roll) rad/s ; speed ~1100 u/s ; climb rate = speed*tan(pitch) ; bank sinks the nose (dp/dt ~ -0.05*roll^2)
# ---------------------------------------------------------------------------------------------------------------------------------
$V=1100.0; $KTURN=0.15; $KROLL=0.95; $KPITCH=0.33; $PDEAD=0.11
$BOX=@{x0=-85000;x1=130000;z0=-15000;z1=160000}          # mission area: flown without failing x -90k..138k, z -23k..178k; OFF COURSE at z >= ~180k
$INNER=@{x0=-65000;x1=110000;z0=5000;z1=125000}           # steer back to the centre when outside this
$CENTER=@(25000.0,70000.0)
if($Mode -eq 'Chase'){ $INNER=@{x0=-75000;x1=120000;z0=-8000;z1=140000}; $CENTER=@(25000.0,60000.0) }   # batch Z4: the air contacts roam the south part (z < 5k) too; BOX (z -15k..160k) is unchanged, flown without failing down to z -23k
$chase=($Mode -eq 'Chase'); $kill=($Mode -eq 'Kill' -or $Mode -eq 'Gun' -or $chase); $gunMode=($Mode -eq 'Gun'); $sunMode=($Mode -eq 'Sun')
$SunHdgE=$SunHdg; $SunElevE=$SunElev
if($SunMapset -and (Test-Path $SunMapset)){ $by=[IO.File]::ReadAllBytes($SunMapset); if($by.Length -ge 0x60){ $SunElevE=[double][BitConverter]::ToSingle($by,0x58); $SunHdgE=-[double][BitConverter]::ToSingle($by,0x5C) } }
$SunU=@((-[math]::Sin($SunHdgE)),(-[math]::Cos($SunHdgE)))   # horizontal direction to FLY to look at the sun
$SunLim=@{x0=-57000.0;x1=102000.0;z0=13000.0;z1=117000.0}   # run-in point clamp (= INNER shrunk by 8 km, m02a values; unchanged by Chase)
function SetSunQ{ if($SunRun){ $z=$SunRun.Split(','); $script:SunQ=@([double]$z[0],[double]$z[1]) } else { $lr=$V*$SunHold+40000.0; $script:SunQ=@(($script:CENTER[0]-$SunU[0]*$lr),($script:CENTER[1]-$SunU[1]*$lr)) } }
function ClampSunQ{ $script:SunQ[0]=[math]::Max($script:SunLim.x0,[math]::Min($script:SunLim.x1,$script:SunQ[0])); $script:SunQ[1]=[math]::Max($script:SunLim.z0,[math]::Min($script:SunLim.z1,$script:SunQ[1])) }
SetSunQ; ClampSunQ
$WP=@(@(40000,110000),@(90000,60000),@(50000,8000),@(-20000,30000),@(-50000,100000),@(10000,115000))  # racetrack loop inside INNER (m02a)
$script:StageName='m02a'
# m03k (Mission 03 'The Round Table'): NO bounds are documented. Evidence: the airborne start is ~105k,20000,106k; 3 autopilot flights (step124 runA3/C1/C2) flew x -40k..105k, z 17k..106k, y 20000->7400
# without an area warning; their 'mission over' at T111-159 is a scripted formation cutscene (HUD 'Target spotted.' / 'Commencing attack.'), NOT leaving the area (positions differ: -40k,28k / 13k,17k). The box below is the observed-safe
# envelope plus margin, not a measured limit.
function SetStage([string]$n){
  if($n -eq 'm03k'){ $script:BOX=@{x0=-60000;x1=125000;z0=-5000;z1=135000}; $script:INNER=@{x0=-35000;x1=110000;z0=12000;z1=118000}; $script:CENTER=@(35000.0,65000.0)
    $script:WP=@(@(85000,100000),@(90000,40000),@(30000,20000),@(-20000,50000),@(-15000,100000),@(40000,110000)); $script:SunLim=@{x0=-27000.0;x1=102000.0;z0=20000.0;z1=110000.0}
    $script:StageName='m03k'; SetSunQ; ClampSunQ }
}
function AutoStage($x,$z){ if($Stage -eq 'm03k' -or ($Stage -eq 'auto' -and [math]::Sqrt([math]::Pow($x-105000,2)+[math]::Pow($z-106000,2)) -lt 30000)){ SetStage 'm03k' }; Log ("stage {0} (-Stage {1}) BOX x {2}..{3} z {4}..{5} INNER x {6}..{7} z {8}..{9} centre {10:N0},{11:N0} sun run-in {12:N0},{13:N0}" -f $script:StageName,$Stage,$BOX.x0,$BOX.x1,$BOX.z0,$BOX.z1,$INNER.x0,$INNER.x1,$INNER.z0,$INNER.z1,$CENTER[0],$CENTER[1],$SunQ[0],$SunQ[1]) }
$FWP=@(); foreach($q in ($FirstWp -split ';')){ if($q.Trim()){ $z=$q.Split(','); $FWP+=,@([double]$z[0],[double]$z[1]) } }
function Wrap([double]$a){ while($a -gt [math]::PI){$a-=2*[math]::PI}; while($a -lt -[math]::PI){$a+=2*[math]::PI}; $a }
function Clamp([double]$v,[double]$lo,[double]$hi){ [math]::Max($lo,[math]::Min($hi,$v)) }
function InBox($b,[double]$x,[double]$z){ $x -ge $b.x0 -and $x -le $b.x1 -and $z -ge $b.z0 -and $z -le $b.z1 }
function HdgTo([double]$dx,[double]$dz){ [math]::Atan2(-$dx,-$dz) }   # inverse of v=(-sin h,-cos h)
function ColorClass([double]$r,[double]$g,[double]$b){
  if($r -gt 0.95 -and $g -gt 0.95 -and $b -gt 0.95){ return 'unknown' }
  if($r -gt 0.8 -and $g -lt 0.5 -and $b -lt 0.5){ return 'red' }
  if($r -gt 0.8 -and $g -gt 0.7 -and $b -lt 0.3){ return 'yellow' }
  if($g -gt 0.8 -and $r -lt 0.5 -and $b -lt 0.5){ return 'green' }
  if($b -gt 0.8 -and $r -lt 0.4){ return 'blue' }
  'other' }
$ci=[Globalization.CultureInfo]::InvariantCulture
function D([string]$s){ [double]::Parse($s,$ci) }
# ---- parsers --------------------------------------------------------------------------------------------------------------------
function ParseObs([string]$l){
  $i=$l.IndexOf('|ee-camera;obs;'); if($i -lt 0){ return $null }
  $j=$l.IndexOf(';A=',$i); if($j -lt 0){ return $null }
  $k=$l.IndexOf(':',$j); if($k -lt 0){ return $null }
  $e=$l.IndexOf(';',$k); if($e -lt 0){ $e=$l.Length }
  $v=$l.Substring($k+1,$e-$k-1).Split(','); if($v.Count -ne 13){ return $null }
  try { $f=$v | % { D $_ } } catch { return $null }
  [pscustomobject]@{tick=[double]::Parse($l.Substring(0,$i),$ci);h=$f[3];x=$f[4];y=$f[5];z=$f[6];p=$f[10];r=$f[12];lp=$f[0];ly=$f[1]}
}
function ValidFlight($s){ $s -and ([math]::Abs($s.x) -gt 1000 -or [math]::Abs($s.z) -gt 1000) -and $s.y -gt 1 }
$cre=[regex]'(\d+):(win|out)\S*:r(\d+):b([-\d.]+):e([-\d.]+):v(\d+):(id|none)([\d.]+),([\d.]+),([\d.]+):g(\d+)'
function ParseContacts([string]$l){
  $i=$l.IndexOf('|xr-stereo;event=mk360-contacts'); if($i -lt 0){ return $null }
  $tick=[double]::Parse($l.Substring(0,$i),$ci); $out=@()
  foreach($m in $cre.Matches($l)){ $g=$m.Groups
    $out+=[pscustomobject]@{id=[int]$g[1].Value;win=$g[2].Value;r=D $g[3].Value;b=D $g[4].Value;e=D $g[5].Value;v=D $g[6].Value;ident=($g[7].Value -eq 'id');col=(ColorClass (D $g[8].Value) (D $g[9].Value) (D $g[10].Value));gl=[int]$g[11].Value} }
  [pscustomobject]@{tick=$tick;c=$out}
}
function TailLines([string]$path,[int]$bytes){
  $fs=[IO.File]::Open($path,'Open','Read','ReadWrite,Delete')
  try { $n=[math]::Min($bytes,$fs.Length); [void]$fs.Seek(-$n,'End'); $sr=New-Object IO.StreamReader($fs,[Text.Encoding]::ASCII); $t=$sr.ReadToEnd() } finally { $fs.Dispose() }
  $t.Split("`n")
}
# ---- controller -----------------------------------------------------------------------------------------------------------------
$script:M=@{cur=0;pull=$false;pullT=0.0;lastRoll=-99.0;lastPitch=-99.0;lastMissile=-99.0;wp=-1;chaseHold=0.0;tracks=(New-Object System.Collections.Generic.List[object]);uid=0;fwi=0;shots=@{};near=@{};brk=$null;firstFire=$null;contTick=0.0;hist=New-Object System.Collections.Generic.List[object];prevY=$null;prevT=$null;gamma=0.0;stat=@{left=0;right=0;up=0;down=0;missile=0;guard=0;gun=0;breaks=0};lastGun=-99.0;lockT=0.0;lockAt=-99.0;salvo=0;per=$Period;lockMax=0.0;sunPh=0;sunT1=$null;sunLast=$null;sunAlSec=0.0;sunDone=$null;taps=(New-Object System.Collections.Generic.List[object]);blind=$false;blindStart=0.0;blindT=0.0;drT0=0.0;lastGood=$null;lastGoodT=0.0;fixes=0;fixTick=0.0}
if($HoldLock -gt 0){ $script:M.lockHeld=$false; $script:M.stat.lockheld=0; $script:M.stat.lockshots=0; $script:M.shot3=$false; $script:M.stat.tswitch=0; $script:M.sw=@{}; if(-not $PSBoundParameters.ContainsKey('PriWeight')){ $PriWeight=0.0 } }   # LOCK-HOLD: the NEAREST target (colour priority off unless -PriWeight is given)
$script:DR=$null
# LOCK-HOLD eye shots (live only): %LOCALAPPDATA%\acz-vr\png-eye.now (+ png-screen.now) re-arm the DLL's xr_png_once dump (cfg xr_png_once=true, FLIGHT-DUMP) when the hold
# reaches 3 s (the seeker diamond moving onto the box) and at LOCK-HELD (the locked diamond); -NoLockShots turns them off. The tap summary counts them (lockshots=N).
function LockShot([string]$why){ if(-not $script:Live -or $NoLockShots){ return }; try { $d=Join-Path $env:LOCALAPPDATA 'acz-vr'; Set-Content -Path (Join-Path $d 'png-eye.now') -Value ("LOCK-HOLD {0}" -f $why) -Encoding ascii; Set-Content -Path (Join-Path $d 'png-screen.now') -Value ("LOCK-HOLD {0}" -f $why) -Encoding ascii; $script:M.stat.lockshots++; Log ("LOCK-SHOT {0}: png-eye.now + png-screen.now written" -f $why) } catch { Log ("LOCK-SHOT {0}: not written: {1}" -f $why,$_) } }
# NULL-HEAD head look (live only): head.now = 'yaw pitch hold' (the DLL slews at -HeadSlew deg/s, holds `hold` s from ARRIVAL, then slews back to 0 0). The logged
# hold is the whole look window (slew out + hold + slew back) so cloudtrig's 'LOOKC? (lookleft|lookright) hold N' gate covers it. An eye shot (png-eye.now) is
# scheduled for 0.4 s after the head arrives; HeadShotDue writes it from the 20 ms loop.
$script:headShotAt=-1.0; $script:headShotWhy=''; $script:headUsed=$false; $script:poseHomeAt=-1.0
function HeadCmd([double]$yaw,[double]$pitch,[double]$hold,[string]$tag,[string]$dir,[double]$T,[string]$extra=''){
  if(-not $script:Live){ return $false }
  $yaw=[math]::Max(-180.0,[math]::Min(180.0,$yaw)); $pitch=[math]::Max(-89.0,[math]::Min(89.0,$pitch))
  $slew=[math]::Max([math]::Abs($yaw),[math]::Abs($pitch))/[math]::Max(10.0,$HeadSlew); $win=$hold+2*$slew
  try { $d=Join-Path $env:LOCALAPPDATA 'acz-vr'; $txt=[string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0:0.0} {1:0.0} {2:0.0}',$yaw,$pitch,$hold)
    if($HeadPose){ $txt=[string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0:0.0} {1:0.0} 0 {2:0.0}',$yaw,$pitch,$HeadSlew); Set-PoseFile $txt; $script:poseHomeAt=$(if($hold -gt 0){ $T+$slew+$hold }else{ -1.0 }) }
    else { Set-Content -Path (Join-Path $d 'head.now') -Value $txt -Encoding ascii }; $script:headUsed=$true
    Log ("T={0:N1} {1} {2} hold {3:0.0} yaw={4:0.0} pitch={5:0.0} head-hold={6:0.0} slew={7:0.0}s head.now='{8}'{9}" -f $T,$tag,$dir,$win,$yaw,$pitch,$hold,$slew,$txt,$extra)
    if(-not $NoHeadShots -and $hold -gt 0.5){ $script:headShotAt=$T+$slew+0.4; $script:headShotWhy=("{0} {1} yaw={2:0} pitch={3:0}" -f $tag,$dir,$yaw,$pitch) }
    return $true } catch { Log ("T={0:N1} {1} head.now not written: {2}" -f $T,$tag,$_); return $false } }
function Set-PoseFile([string]$txt){ $p=Join-Path (Join-Path $env:LOCALAPPDATA 'acz-vr') 'testhmd-pose.txt'; Set-Content -Path ($p+'.tmp') -Value $txt -Encoding ascii; Move-Item -LiteralPath ($p+'.tmp') -Destination $p -Force }
function HeadShotDue([double]$T){
  if($HeadPose -and $script:poseHomeAt -ge 0 -and $T -ge $script:poseHomeAt){ $script:poseHomeAt=-1.0; try { Set-PoseFile ('0 0 0 {0:0.0}' -f $HeadSlew); Log ('T={0:N1} HEADLOOK home (test HMD pose file)' -f $T) } catch {} }
  if($script:headShotAt -lt 0 -or $T -lt $script:headShotAt){ return }
  $script:headShotAt=-1.0
  try { Set-Content -Path (Join-Path (Join-Path $env:LOCALAPPDATA 'acz-vr') 'png-eye.now') -Value ("HEADLOOK {0}" -f $script:headShotWhy) -Encoding ascii; Log ("T={0:N1} HEADSHOT png-eye.now written ({1})" -f $T,$script:headShotWhy) } catch { Log ("T={0:N1} HEADSHOT not written: {1}" -f $T,$_) } }
function HeadHome([double]$T){ if($script:headUsed -and $script:Live -and $HeadPose){ try { Set-PoseFile '0 0 0 0'; Log ('T={0:N1} HEADLOOK home (test HMD pose file)' -f $T) } catch {}; return }; if($script:headUsed -and $script:Live){ try { Set-Content -Path (Join-Path (Join-Path $env:LOCALAPPDATA 'acz-vr') 'head.now') -Value '0 0' -Encoding ascii; Log ("T={0:N1} HEADLOOK home: head.now='0 0'" -f $T) } catch {} } }
# Tracks are associated by POSITION CONTINUITY, never by the mk360 id: the game reuses contact ids (batch L4: id 35 was the ground target, then a wingman at
# r~190 v~1100, alternating), so an id-keyed track flipped class and velocity. A contact joins the nearest unmatched track whose (dead-reckoned) position it
# is within a gate of; else it starts a new track. The class (ground / air) is fixed when the track starts.
function TrackClass([double]$v){ if($v -lt $GroundMaxV){ 'ground' } else { 'air' } }
function UpdateContacts($ct,$st){
  if(-not $ct -or $ct.tick -le $script:M.contTick){ return }
  $script:M.contTick=$ct.tick
  $h=$script:M.hist; if($h.Count -eq 0){ return }
  $s=$h[$h.Count-1]; foreach($q in $h){ if([math]::Abs($q.tick-$ct.tick) -lt [math]::Abs($s.tick-$ct.tick)){ $s=$q } }   # state nearest the snapshot
  $tr=$script:M.tracks
  for($i=$tr.Count-1;$i -ge 0;$i--){ $q=$tr[$i]; $age=$ct.tick-$q.tick
    if($q.cls -eq 'air'){ if($age -gt 25000){ $tr.RemoveAt($i) } }
    else { # ground targets are remembered (batch I: the 12/25 s expiry made the autopilot forget the group it had just flown past); a track that should be in view but is gone was destroyed
      $dq=[math]::Sqrt([math]::Pow($q.pos[0]-$s.x,2)+[math]::Pow($q.pos[2]-$s.z,2)); if($age -gt $(if($q.n -ge 3){ 25000 }else{ 12000 }) -and $dq -lt 9000){ $tr.RemoveAt($i) } } }   # batch K2: an established site (seen >= 3x) survives a listing gap of 25 s (KA2: the target left the list at 7 km and a far contact took over)
  $cand=@(); $pairs=@()
  foreach($c in $ct.c){
    if($chase -and $c.r -lt 1200){ continue }   # batch Z4: contact 35 = an unidentified v~1110 object 200-450 u from our own aircraft (Z4a/Z4b chased it): never a target
    $hd=$s.h-[math]::PI/180*$c.b; $el=[math]::PI/180*$c.e; $cz=[math]::Cos($el)
    $P=@(($s.x-[math]::Sin($hd)*$c.r*$cz),($s.y+[math]::Sin($el)*$c.r),($s.z-[math]::Cos($hd)*$c.r*$cz))
    $ci=$cand.Count; $cand+=[pscustomobject]@{c=$c;P=$P;done=$false}
    for($ti=0;$ti -lt $tr.Count;$ti++){ $t=$tr[$ti]; $dt=($ct.tick-$t.tick)/1000
      $q=@(($t.pos[0]+$t.vel[0]*$dt),($t.pos[1]+$t.vel[1]*$dt),($t.pos[2]+$t.vel[2]*$dt))
      $d=[math]::Sqrt([math]::Pow($P[0]-$q[0],2)+[math]::Pow($P[1]-$q[1],2)+[math]::Pow($P[2]-$q[2],2))
      $gate= if($t.cls -eq 'ground'){ 2500.0+$(if($t.v -ge 60){ 300.0*[math]::Min($dt,20.0) }else{ 0.0 })+$(if($script:M.blind){ [math]::Min(6000.0,60.0*$script:M.blindT) }else{ 0.0 }) }else{ 1500.0+1300.0*$dt }
      if($d -le $gate){ $pairs+=[pscustomobject]@{ci=$ci;ti=$ti;d=$d} } } }
  $used=@{}
  foreach($pr in ($pairs | Sort-Object d)){
    if($cand[$pr.ci].done -or $used.ContainsKey($pr.ti)){ continue }
    $cand[$pr.ci].done=$true; $used[$pr.ti]=$true
    $t=$tr[$pr.ti]; $c=$cand[$pr.ci].c; $P=$cand[$pr.ci].P; $dt=($ct.tick-$t.tick)/1000; $vel=@(0.0,0.0,0.0)
    if($script:M.blind -and $t.cls -eq 'ground' -and $t.n -ge 3 -and $c.v -lt 60){ $t.tick=$ct.tick; $t.fix=$ct.tick; $t.rr=$c.r; $t.bb=$c.b; $t.ee=$c.e; continue }   # batch M: while blind the dead-reckoned P is wrong; an established static site keeps its world position and its (range, bearing, elevation) is used to FIX the dead reckoning (DrFix)
    if(($t.cls -eq 'air' -or $c.v -ge 60) -and $dt -gt 1 -and $dt -lt 20){   # ground movers (v 60..GroundMaxV, batch I: the TGT vehicle) get a velocity too; stationary ground contacts never
      $n=@(0.0,0.0,0.0); for($k=0;$k -lt 3;$k++){ $n[$k]=($P[$k]-$t.pos[$k])/$dt*0.6+$t.vel[$k]*0.4 }
      $sp=[math]::Sqrt($n[0]*$n[0]+$n[2]*$n[2]); if($sp -gt $(if($t.cls -eq 'ground'){ $GroundMaxV }else{ 1500.0 }) -or $sp -lt 60){ $n=@(0.0,0.0,0.0) }; $vel=$n }
    elseif($t.cls -eq 'air' -or $c.v -ge 60){ $vel=$t.vel }
    $t.tick=$ct.tick; $t.pos=$P; $t.vel=$vel; if($c.col -ne 'unknown' -or $t.col -eq 'unknown'){ $t.col=$c.col }; $t.ident=$c.ident; $t.v=$c.v; $t.gl=$c.gl; $t.win=$c.win; $t.cid=$c.id; $t.n++ }
  foreach($cd in $cand){ if($cd.done){ continue }; if($script:M.blind){ continue }; $c=$cd.c; $script:M.uid++
    $tr.Add([pscustomobject]@{uid=$script:M.uid;cid=$c.id;cls=(TrackClass $c.v);tick=$ct.tick;pos=$cd.P;vel=@(0.0,0.0,0.0);col=$c.col;ident=$c.ident;v=$c.v;gl=$c.gl;win=$c.win;n=1;fix=0.0;rr=0.0;bb=0.0;ee=0.0}) }
}
function PickTarget($st,[double]$tickNow){
  $order=$TargetColors.Split(',') ; $best=$null;$bestScore=1e18
  foreach($t in $script:M.tracks){ $id=$t.uid; $ground=($t.cls -eq 'ground')
    $age=($tickNow-$t.tick)/1000
    if($gunMode -and (-not $ground -or $t.v -ge 60)){ continue }
    if($chase -and $ground){ continue }   # Chase: AIR tracks only (speed > GroundMaxV when the track started); blue (friendly) is not in -TargetColors   # Gun: stationary ground groups only
    if(-not $ground -and $age -gt 12){ continue }   # aircraft are dead-reckoned for 12 s only; ground sites are remembered (UpdateContacts drops them when they should be visible but are gone)
    if($TargetId -gt 0 -and $t.cid -ne $TargetId){ continue }
    $pri=[array]::IndexOf($order,$t.col); if($TargetId -le 0 -and $pri -lt 0){ continue }; if($pri -lt 0){ $pri=0 }
    $ag=[math]::Min($age,30.0); $px=$t.pos[0]+$t.vel[0]*$ag; $pz=$t.pos[2]+$t.vel[2]*$ag
    if($t.col -eq 'unknown'){ if(-not $ground -and -not $chase){ continue }; if($ground -and [math]::Sqrt([math]::Pow($px-$HomeX,2)+[math]::Pow($pz-$HomeZ,2)) -lt $HomeRadius){ continue } }   # white air contacts = wingmen/unidentified; white ground contacts near the airfield = our base
    if(-not (InBox $BOX $px $pz)){ continue }   # live K2: red contacts were never inside INNER (step104k)
    $d=[math]::Sqrt([math]::Pow($px-$st.x,2)+[math]::Pow($pz-$st.z,2)); if($chase -and $t.col -eq 'unknown' -and $d -lt 3500){ continue }   # unidentified air contact this close = a wingman
    $sc=$pri*$PriWeight+$d; if($ground -and $t.v -lt 60 -and $t.vel[0] -eq 0 -and $t.vel[2] -eq 0){ $sc-=$PreferStatic }
    if($HoldLock -gt 0 -and -not $script:M.lockHeld -and -not $ground){ $tvv=[math]::Sqrt($t.vel[0]*$t.vel[0]+$t.vel[2]*$t.vel[2]); if($tvv -gt 100 -and $d -gt 1){ $ca=($t.vel[0]*($st.x-$px)+$t.vel[2]*($st.z-$pz))/($tvv*$d); if($ca -gt 0){ $sc+=$HoldAspect*$ca } } }   # LOCK-HOLD: prefer a target we can come up behind
    if($id -eq $script:M.cur){ $sc-=$SwitchBias }   # sticky: a track already followed keeps priority unless another is clearly better
    if($script:M.shots.ContainsKey($id) -and $script:M.shots[$id] -ge 4){ $sc+=40000 }   # four missiles and it is still listed: try another one first
    if($sc -lt $bestScore){ $bestScore=$sc; $best=@{id=$id;t=$t;px=$px;py=$t.pos[1]+$t.vel[1]*$ag;pz=$pz;d=$d;pri=$pri} } }
  if($best){ $script:M.cur=$best.id } else { $script:M.cur=0 }
  $best
}
function Decide($st,[double]$T,[double]$tickNow){
  # st: state (extrapolated to now). returns @{cmds=@(@{key;hold});note}
  $cmds=@(); $note=@()
  $h=$st.h; $p=$st.p; $r=$st.r
  # batch M: the attitude read is 0.4-1.6 s old and the roll/pitch response has a ~0.45 s lag: add the effect of the S/W/A/D taps whose release is not in the sample yet
  # (live L1a: pitch -0.40 against a -0.20 demand, the taps of the last 1.5 s were invisible and the dive wound up)
  $ageS=[math]::Max(0.0,($tickNow-$st.tick)/1000); $Ts=$T-$ageS
  for($i=$script:M.taps.Count-1;$i -ge 0;$i--){ if($script:M.taps[$i].rel -lt $T-6){ $script:M.taps.RemoveAt($i) } }
  if(-not $NoPredict){ foreach($tp in $script:M.taps){ $u= if($tp.rel -gt $Ts){ 1.0 }else{ [math]::Exp(-($Ts-$tp.rel)/0.45) }; if($u -lt 0.03){ continue }
      if($tp.key -eq 'down'){ $p+=$KPITCH*($tp.hold-$PDEAD)*$u } elseif($tp.key -eq 'up'){ $p-=$KPITCH*($tp.hold-$PDEAD)*$u } elseif($tp.key -eq 'left'){ $r+=$KROLL*$tp.hold*$u } elseif($tp.key -eq 'right'){ $r-=$KROLL*$tp.hold*$u } } }
  # --- guidance: desired heading hd, desired climb angle gd
  $hd=$h; $why='hold'; $gd=0.0
  $inInner=InBox $INNER $st.x $st.z
  $tgt=$null
  if($kill){ $tgt=PickTarget $st $tickNow }
  # LOCK-HOLD on an AIR target (runLOCK1: Kill pure pursuit met the green enemies head-on at ~2000 u/s closing, then BREAK/PULLUP loops kept them 120-180 deg behind, and the CEIL
  # pitch (y > AltCeil) ignored their elevation): fly the Chase trail (end up behind it), no overflight break, pitch on its elevation even above AltCeil, 1 s decisions; the reticle window stays -LockRange
  $holdAir=($HoldLock -gt 0 -and -not $script:M.lockHeld -and $tgt -and $tgt.t.cls -ne 'ground')
  # LOCK-HOLD: while the hold is pending the reticle window starts V*HoldLock + MissileMinGround + 4 km out (a ground pass at ~1100 u/s crosses the 9 km window in ~6 s: sim lock 3.1 s; +1 km: 7.3 s), and so does the nose-on dive
  $holdR= if($HoldLock -gt 0 -and -not $script:M.lockHeld -and -not $holdAir){ [math]::Max($LockRange,$V*$HoldLock+$MissileMinGround+4000.0) }else{ $LockRange }; $diveR= if($HoldLock -gt 0 -and -not $script:M.lockHeld){ [math]::Max($DiveFrom,$holdR) }else{ $DiveFrom }
  if($sunMode){   # SUN-LOOK phases: 0 run-in to the up-sun point, 1 hold the sun heading until SunHold aligned seconds, 2 done (cruise)
    $dq=[math]::Sqrt([math]::Pow($SunQ[0]-$st.x,2)+[math]::Pow($SunQ[1]-$st.z,2))
    if($script:M.sunPh -eq 0 -and $dq -lt $SunArrive){ $script:M.sunPh=1; $script:M.sunT1=$T; $script:M.sunLast=$T }
    if($script:M.sunPh -eq 1){ $dts=[math]::Min(3.0,$T-$script:M.sunLast); $script:M.sunLast=$T
      if([math]::Abs((Wrap ($SunHdgE-$h))) -lt $SunAlignTol -and [math]::Abs($r) -lt 0.25 -and $inInner){ $script:M.sunAlSec+=$dts }
      if($SunZoom -and $script:zoomN -ge $ZoomMax){ $script:M.sunPh=2; $script:M.sunDone=$T }
      if($script:M.sunAlSec -ge $SunHold -or ($T-$script:M.sunT1) -gt $SunGiveUp){ $script:M.sunPh=2; $script:M.sunDone=$T } } }
  if(-not $inInner){ $hd=HdgTo ($CENTER[0]-$st.x) ($CENTER[1]-$st.z); $why='GUARD->centre'; $script:M.stat.guard++ }
  elseif($tgt){
    $tau=[math]::Min(20.0,$tgt.d/$V); if($tgt.t.cls -eq 'ground' -or ($HoldLock -gt 0 -and -not $script:M.lockHeld)){ $tau=0.0 }   # LOCK-HOLD: pure pursuit (the nose on the target itself) until the lock was held; batch K: NO lead on ground targets: the lock needs the target ITSELF in the reticle (the seeker does the leading; a 20 s lead put a 180 u/s mover 10 deg off the nose in the sim); the position is still extrapolated with the track velocity
    $lx=$tgt.px+$tgt.t.vel[0]*$tau; $lz=$tgt.pz+$tgt.t.vel[2]*$tau; $ly=$tgt.py+$tgt.t.vel[1]*$tau
    if($chase -or $holdAir){ $tv=[math]::Sqrt($tgt.t.vel[0]*$tgt.t.vel[0]+$tgt.t.vel[2]*$tgt.t.vel[2]); if($tv -gt 100){ $tw=$ChaseTrail*(Clamp ((8000.0-$tgt.d)/4000.0) 0.0 1.0); $lx-=$tgt.t.vel[0]/$tv*$tw; $lz-=$tgt.t.vel[2]/$tv*$tw } }   # Chase: trail point behind the target once inside 8 km
    $hd=HdgTo ($lx-$st.x) ($lz-$st.z); $dh=[math]::Sqrt([math]::Pow($lx-$st.x,2)+[math]::Pow($lz-$st.z,2))
    if($OffsetDeg -ne 0 -and ($tgt.t.cls -eq 'ground' -or $chase) -and $tgt.d -gt $OffsetUntil){ $hd=Wrap ($hd+$OffsetDeg*[math]::PI/180.0) }   # batch X: constant-bearing approach, the ground group stays OffsetDeg off the nose (+ = target on the RIGHT) until OffsetUntil
    $gd=[math]::Atan2($ly-$st.y,[math]::Max(500.0,$dh)); $why="TGT#$($tgt.t.cid)u$($tgt.id)/$($tgt.t.cls)/$($tgt.t.col) d=$([int]$tgt.d)"
  } elseif($kill -and $script:M.fwi -lt $FWP.Count){
    $fw=$FWP[$script:M.fwi]; $hd=HdgTo ($fw[0]-$st.x) ($fw[1]-$st.z); $why="PRIOR$($script:M.fwi)"
    if([math]::Sqrt([math]::Pow($fw[0]-$st.x,2)+[math]::Pow($fw[1]-$st.z,2)) -lt 6000){ $script:M.fwi++ }
  } elseif($sunMode -and $script:M.sunPh -eq 0){
    $hd=HdgTo ($SunQ[0]-$st.x) ($SunQ[1]-$st.z); $why='SUN-runin'
  } elseif($sunMode -and $script:M.sunPh -eq 1){
    $hd=Wrap ($h+[math]::Max(-0.6,[math]::Min(0.6,(Wrap ($SunHdgE-$h))))); $why='SUN-look'   # batch W: bounded heading step (a 3 rad error made the W1 aircraft loop and crash)
  } else {
    if($script:M.wp -lt 0){ # first waypoint: the one best ahead (smallest heading change, > 25 km away)
      $bi=0;$bd=9;for($i=0;$i -lt $WP.Count;$i++){ $d=[math]::Sqrt([math]::Pow($WP[$i][0]-$st.x,2)+[math]::Pow($WP[$i][1]-$st.z,2)); if($d -lt 25000){continue}
        $a=[math]::Abs((Wrap ((HdgTo ($WP[$i][0]-$st.x) ($WP[$i][1]-$st.z))-$h))); if($a -lt $bd){$bd=$a;$bi=$i} }; $script:M.wp=$bi }
    $w=$WP[$script:M.wp]; $dw=[math]::Sqrt([math]::Pow($w[0]-$st.x,2)+[math]::Pow($w[1]-$st.z,2))
    if($dw -lt 14000){ $script:M.wp=($script:M.wp+1)%$WP.Count; $w=$WP[$script:M.wp] }
    $hd=HdgTo ($w[0]-$st.x) ($w[1]-$st.z); $why="WP$($script:M.wp)"
  }
  # --- roll: heading error -> turn-rate demand -> bank demand
  $he=Wrap ($hd-$h)
  # ground attack run: inside 14 km of a ground target (not breaking away) the altitude floor drops to 1600 so the strafe descent (StrafeAlt) is not turned into a FLOOR climb
  $AF=$AltFloor; $runin=($tgt -and $inInner -and $tgt.t.cls -eq 'ground' -and $tgt.d -lt $(if($gunMode){ 60000 }else{ 14000 }) -and ($StrafeAlt -gt 0 -or $gunMode))   # Gun: the low approach altitude (GunAlt < AltFloor) is held from the first sighting, not only inside 14 km
  if(-not $tgt){ $script:M.pull=$false }
  $strike=($kill -and $tgt -and $inInner -and $tgt.t.cls -eq 'ground' -and ($StrafeAlt -gt 0 -or $gunMode))
  if($runin){ $AF=[math]::Min($AltFloor,$PullY-300) }
  # B3: after an overflight (target close once, now behind) break away: turn back at the max safe bank and climb to the band, then re-attack
  $breaking=$false
  if($kill){
    if($tgt){ $u=$tgt.id; if(-not $script:M.near.ContainsKey($u) -or $tgt.d -lt $script:M.near[$u]){ $script:M.near[$u]=$tgt.d }
      if(-not $holdAir -and $null -eq $script:M.brk -and $script:M.near[$u] -lt 3500 -and [math]::Abs($he) -gt 1.6 -and $tgt.d -lt 20000){ $script:M.brk=$T; $script:M.stat.breaks++; $script:M.salvo=0 } }
    if($null -ne $script:M.brk){
      if(-not $tgt -or ($T-$script:M.brk) -gt 90 -or ([math]::Abs($he) -lt 0.35 -and $st.y -gt $(if($strike){$ReAttackAlt}else{$TargetAlt})-$(if($strike){600}else{1000}) -and $tgt.d -gt $(if($strike){$DiveFrom-1500}else{5000}))){ $script:M.brk=$null; $script:M.near=@{} } else { $breaking=$true } } }
  if($strike -and $breaking){ $AF=[math]::Min($AltFloor,$PullY-300) }
  if($null -ne $script:M.brk -and $script:M.pull){ $script:M.pull=$false }   # the overflight is over: the pull-up latch ends with the break
  # batch K2: low-altitude guard and pull-up release. yEff = altitude in 2 s at the current nose-down pitch.
  if($script:M.pull){ $hab= if($tgt){ $st.y-$tgt.py }else{ $st.y-500.0 }
    if(($st.y -gt $PullSafeAlt -and $hab -gt 600 -and $p -gt -0.02) -or (($T-$script:M.pullT) -gt $PullMaxSec -and $st.y -gt $PullY+200 -and $p -gt -0.05)){ $script:M.pull=$false } }
  $diving=($strike -and -not $breaking -and -not $script:M.pull -and $tgt.d -lt $(if($gunMode){ $GunDiveFrom }else{ $diveR }) -and [math]::Abs($he) -lt 0.35)
  $ahead= if($gunMode){ $V*[math]::Sin([math]::Min(0.0,$p))*$script:M.per }else{ 0.0 }   # Gun: judge the pull-up on the height at the NEXT decision (1 s = 1.1 km of flight)
  $yEff=$st.y+$V*[math]::Sin([math]::Min(0.0,$p))*2.0; $low=($st.y -lt $LowBankAlt) -or ($yEff -lt $LowBankAlt -and -not $diving)   # the deliberate strike dive is judged on the real altitude only (the 2 s sink look-ahead would cap its bank from 2100 u)
  # B2: an attack turn (bearing error > 10 deg, clear of the floor) is not held to the shallow pitch-based bank cap
  $attack=($tgt -and $inInner -and -not $breaking -and [math]::Abs($he) -gt 0.0873 -and $st.y -gt $AF+800)   # batch H: from 5 deg (was 10 = the fire threshold, so the last 10 deg were flown at the capped bank)
  $bigBank=(($attack -or $breaking) -and ($st.y -gt $AF+800 -or ($breaking -and $strike -and $p -gt -0.02 -and $st.y -gt $AF+150)))
  $wl= if($inInner){0.05}else{0.10}; $rl= if($inInner){0.35}else{0.45}
  if($tgt -and $inInner -and [math]::Abs((Wrap ($hd-$h))) -gt 0.5){ $wl=0.10; $rl=0.45 }   # batch L: tighter turn back onto a target     # steeper bank allowed only when outside the inner box (0.60 sank the nose live: step104k K2 crash)
  if($bigBank){ $wl=0.10; $rl=0.45 }
  elseif($st.y -lt $AF+500 -or ($p -lt -0.06 -and -not $runin)){ $rl=[math]::Min($rl,0.20) }   # batch L: was floor+1500 (made target turns ~100 s long)   # low or nose down: shallow bank so the climb can win (live fix)
  if($holdAir -and $HoldBank -gt 0.45 -and $st.y -gt $AF+3000 -and $p -gt -0.15 -and [math]::Abs($he) -gt 0.15){ $wl=[math]::Max($wl,$KTURN*[math]::Tan($HoldBank)); $rl=[math]::Max($rl,$HoldBank) }   # LOCK-HOLD: the tighter turn back onto an air target (high only)
  if($low){ $rl=[math]::Min($rl,$(if($diving){ $DiveBankRoll }else{ $LowBankRoll })) }   # batch K2: no hard bank below LowBankAlt (also when a target switch asks for a big turn)
  if($st.y -lt $AF){ $rl=0.0 }                                         # FLOOR: wings level first
  $wt=Clamp (0.25*$he) (-$wl) $wl
  $rt=[math]::Atan($wt/$KTURN); $rt=Clamp $rt (-$rl) $rl
  if($breaking -and $strike -and $null -ne $script:M.brk -and ($T-$script:M.brk) -lt $BreakStraight){ $qx=$st.x-[math]::Sin($h)*$V*$BreakStraight; $qz=$st.z-[math]::Cos($h)*$V*$BreakStraight; if($qx -gt $INNER.x0+8000 -and $qx -lt $INNER.x1-8000 -and $qz -gt $INNER.z0+8000 -and $qz -lt $INNER.z1-8000){ $rt=0.0 } }   # only with room (the later 15 km radius turn needs it)
  $er=$rt-$r
  if([math]::Abs($er) -gt 0.05 -and ($T-$script:M.lastRoll) -ge $(if($diving -or ($low -and [math]::Abs($r) -gt $LowBankRoll)){1.0}else{1.8})){
    $hold=Clamp ([math]::Abs($er)/$KROLL) 0.12 0.40
    $key= if($er -gt 0){'left'}else{'right'}
    $cmds+=@{key=$key;hold=[math]::Round($hold,2)}; $script:M.lastRoll=$T; $script:M.stat[$key]++; [void]$script:M.taps.Add(@{key=$key;hold=[math]::Round($hold,2);rel=$T+[math]::Round($hold,2)})
  }
  # --- pitch: altitude band (or target elevation), emergency floor/ceiling
  $pm=''
  if($st.y -lt $AF){ $pt=0.15; $pm='FLOOR' }
  elseif($holdAir -and [math]::Abs($he) -lt 0.6 -and $st.y -gt $AF+1500){ $pt=Clamp $gd -0.25 0.20; $pm='HOLD-elev' }   # LOCK-HOLD: the target's elevation wins over the ceiling
  elseif($st.y -gt $AltCeil){ $pt=-0.05; $pm='CEIL' }
  elseif($strike -and ($script:M.pull -or ($diving -and (($st.y+$ahead) -lt $(if($gunMode){ $GunPullY }else{ $PullY }) -or ($st.y+$ahead-$tgt.py) -lt ($(if($gunMode){ $GunPullBase }else{ 250 })+$V*[math]::Max(0.0,-$p)*$(if($gunMode){ $GunPullLag }else{ $PullLag })) -or $tgt.d -lt 500)))){ if(-not $script:M.pull){ $script:M.pullT=$T }; $script:M.pull=$true; $pt=0.12; $pm='PULLUP' }   # latch until the break starts
  elseif($diving){ $lift=$(if($gunMode){ 0.0 }else{ $AimLift })*(Clamp ((4500.0-$tgt.d)/2500.0) 0.0 1.0); $gl=[math]::Atan2($ly+$lift-$st.y,[math]::Max(300.0,$dh));   # the aim-point lift ramps in only inside 4.5 km (after the missiles: it would pull the target out of the reticle while locking)
    $pt=Clamp $gl (-$(if($gunMode){ $GunMaxDive }else{ 0.50 })) 0.05; $pm='GRD-dive' }   # nose ON the target (line of sight), not level: the target must sit in the reticle to lock
  elseif($breaking){ $ea=$(if($strike){$ReAttackAlt}else{$TargetAlt})-$st.y; $vs=Clamp (0.15*$ea) -150 150; $pt=$vs/$V; $pm='BREAK-climb'; if($p -lt -0.03){ $pt=0.12; $pm='PULLUP' } }   # batch M: the overflight break must not stop a pull-up that is still in progress (Gun sim: break at pitch -0.29, 0.3 s taps, ground)
  elseif($tgt -and $inInner -and $tgt.t.cls -eq 'ground'){ $ga=$GroundAlt; if($runin -and $tgt.d -gt 1500 -and [math]::Abs($he) -lt 0.25){ $ga=$StrafeAlt }; if($gunMode){ $ga=$GunAlt }; $ea=$ga-$st.y; if([math]::Abs($ea) -lt $(if($gunMode){ 150 }else{ 500 })){$ea=0}; $vs=Clamp (0.15*$ea) -150 150; $pt=$vs/$V; $pm='GRD-level' }   # batch H: level pass at ~4 km (full bank stays available, fire from level flight)
  elseif($tgt -and $inInner -and [math]::Abs($he) -lt 0.5){ $pt=Clamp $gd -0.20 0.16; if($st.y -lt 3500){ $pt=[math]::Max($pt,0.0) }; if([math]::Abs($he) -gt 0.1745){ $pt=[math]::Max($pt,-0.03) }; $pm='TGT-elev' }   # no dive while still turning onto the target   # batch L: nose-down to -0.20 rad above 3500 u
  elseif($sunMode -and $SunZoom -and $script:M.sunPh -eq 1){
    $zdt=[math]::Min(3.0,$T-$script:zLast); $script:zLast=$T
    $alOk=([math]::Abs((Wrap ($SunHdgE-$h))) -lt $SunAlignTol -and [math]::Abs($r) -lt 0.2)
    if($script:zm -eq 'level'){ $ea=$ZoomBase-$st.y; if([math]::Abs($ea) -lt 300){ $ea=0 }; $vs=Clamp (0.15*$ea) -150 150; $pt=$vs/$V; $pm='ZOOM-level'
      if($alOk){ $script:zAl+=$zdt }else{ $script:zAl=0.0 }
      if($script:zAl -ge 1.0 -and -not $script:zArmed -and $NoArmFx){ $script:zArmed=$true; ArmFxDiag }   # batch Y: arm the flare diagnostic ~2 s before the first zoom (the 400-line run cap is spent in ~16 s)
      if($script:zAl -ge 3.0 -and $script:zoomN -lt $ZoomMax -and $st.y -lt ($ZoomBase+700) -and [math]::Abs($p) -lt 0.08){ $script:zm='up'; $script:zT=$T; $script:zAl=0.0; Log ('T={0:N1} ZOOM FIRE up #{1} y={2:N0} p={3:N2} r={4:N2} sunRelBrg={5:N1}' -f $T,($script:zoomN+1),$st.y,$p,$r,((Wrap ($SunHdgE-$h))*180/[math]::PI)) } }
    elseif($script:zm -eq 'up'){ $pt=$ZoomPitch; $pm='ZOOM-up'
      if($st.y -gt $ZoomTop -or ($T-$script:zT) -gt $ZoomMaxSec){ $script:zm='down'; $script:zT=$T; Log ('T={0:N1} ZOOM top y={1:N0} p={2:N2}: nose down' -f $T,$st.y,$p) } }
    else { $pt=-0.20; $pm='ZOOM-down'
      if(($st.y -lt ($ZoomBase+1200) -and $p -gt -0.30) -or ($T-$script:zT) -gt 14){ $script:zm='level'; $script:zoomN++; Log ('T={0:N1} ZOOM recovered #{1} y={2:N0} p={3:N2}' -f $T,$script:zoomN,$st.y,$p) } } }
  elseif($sunMode -and $script:M.sunPh -eq 1 -and $SunPitch -ge 0){ $pt= if($st.y -gt $SunAltMax){ 0.0 }else{ $SunPitch }; $pm='SUN-pitch' }
  else { $ta=$TargetAlt; if($DipAlt -gt 0 -and $T -ge $DipFrom -and $T -lt ($DipFrom+$DipFor)){ $ta=$DipAlt }; if($sunMode -and $script:M.sunPh -eq 0){ $ta=$SunStartAlt }; $ea=$ta-$st.y; if([math]::Abs($ea) -lt 700){$ea=0}; $vs=Clamp (0.15*$ea) -150 150; $pt=$vs/$V; $pm=$(if($DipAlt -gt 0 -and $ta -eq $DipAlt){ 'ALT-DIP' }else{ 'ALT' }) }
  if($low -and $pm -ne 'PULLUP' -and $pm -ne 'GRD-dive' -and $pm -ne 'FLOOR' -and $st.y -lt $LowBankAlt -and $pt -lt 0.10){ $pt=0.10; $pm='LOWCLIMB' }   # batch K2: low and not in the deliberate strike dive: climb
  $ep=$pt-$p
  if([math]::Abs($ep) -gt 0.012 -and ($T-$script:M.lastPitch) -ge $(if($pm -eq 'GRD-dive' -or $pm -eq 'PULLUP' -or $pm -like 'ZOOM-*'){0.9}else{1.8})){
    $hmax= if($pm -like 'ZOOM-*'){1.3}elseif($pm -eq 'PULLUP'){$PullHold}elseif($pm -eq 'FLOOR' -or $pm -eq 'GRD-dive'){0.50}else{0.30}; $hold=Clamp ($PDEAD+[math]::Abs($ep)/$KPITCH) 0.14 $hmax
    $key= if($ep -gt 0){'down'}else{'up'}   # S ('down') = nose up, W ('up') = nose down
    $cmds+=@{key=$key;hold=[math]::Round($hold,2)}; $script:M.lastPitch=$T; $script:M.stat[$key]++; [void]$script:M.taps.Add(@{key=$key;hold=[math]::Round($hold,2);rel=$T+[math]::Round($hold,2)})
  }
  # --- Kill: fire
  $fire=''
  if($kill -and $tgt -and (InBox $BOX $st.x $st.z)){
    $dx=$tgt.px-$st.x;$dz=$tgt.pz-$st.z;$dy=$tgt.py-$st.y; $rng=[math]::Sqrt($dx*$dx+$dy*$dy+$dz*$dz)
    $be=[math]::Abs((Wrap ((HdgTo $dx $dz)-$h)))*180/[math]::PI; $ee=[math]::Abs([math]::Atan2($dy,[math]::Sqrt($dx*$dx+$dz*$dz))-$p)*180/[math]::PI
    $fire="rng=$([int]$rng) brgErr=$([math]::Round($be,1)) elvErr=$([math]::Round($ee,1))"
    $isG=($tgt.t.cls -eq 'ground')
    # LOCK model (batch J: the target must be held in the reticle ~1-2 s; unlocked missiles fly straight and MISS). Measured at the decision rate (1 s in the attack run).
    # LOCK-HOLD maintain cone [I]: the game acquires inside the reticle, then keeps the lock while the target stays in its wider seeker area; once the hold has
    # run 1 s, staying inside -HoldDeg (bearing and elevation) keeps it going (runLOCK2: 3 s at 0.7-3.9 deg before the head-on pass)
    $cone= if($HoldLock -gt 0 -and -not $script:M.lockHeld -and $script:M.lockT -ge 1.0){ [math]::Max($LockDeg,$HoldDeg) }else{ $LockDeg }
    $inRet=($be -lt $cone -and $ee -lt $cone -and $rng -lt $holdR)
    if($inRet){ if($script:M.lockAt -ge 0 -and ($T-$script:M.lockAt) -lt 3.5){ $script:M.lockT+=($T-$script:M.lockAt) }; $script:M.lockAt=$T } else { $script:M.lockT=0.0; $script:M.lockAt=-99.0 }
    $fire+=" lock=$([math]::Round($script:M.lockT,1))"
    $holding=($HoldLock -gt 0 -and -not $script:M.lockHeld)   # LOCK-HOLD: no gun, no missile until the lock was held HoldLock s in one stretch
    # LOCK-HOLD target switch: the game locks only ITS selected target (label + range readout; the green wedge points to it when off-screen). Once the chosen track sits in
    # the reticle for 1 s and is not red (= being locked), tap the switch key once; again after 4 s if still not red; at most 3 taps per track.
    if($holding -and -not $NoSwitch -and $inRet -and $script:M.lockT -ge 1.0 -and $tgt.t.col -ne 'red'){ if(-not $script:M.sw.ContainsKey($tgt.id)){ $script:M.sw[$tgt.id]=@(0,-99.0) }; $sw=$script:M.sw[$tgt.id]
      if($sw[0] -lt 3 -and ($T-$sw[1]) -ge 4.0){ $cmds+=@{key='tswitch';hold=0.12}; $script:M.sw[$tgt.id]=@(($sw[0]+1),$T); $script:M.stat.tswitch++; $fire+=" TSWITCH#$($sw[0]+1)" } }
    if($holding){ $fire+=" HOLDLOCK=$([math]::Round($script:M.lockT,1))/$HoldLock"; if(-not $script:M.shot3 -and $script:M.lockT -ge [math]::Min(3.0,$HoldLock)){ $script:M.shot3=$true; LockShot ("lock {0:N1}s tick={1}" -f $script:M.lockT,[uint64]$tickNow) }; if($script:M.lockT -ge $HoldLock){ LockShot ("LOCK-HELD tick={0}" -f [uint64]$tickNow); $script:M.lockHeld=$true; $script:M.stat.lockheld++; $fire+=" LOCK-HELD tick=$([uint64]$tickNow)"; $holding=$false } }
    if($chase -and $rng -ge $ChaseMin -and $rng -le $ChaseMax -and $be -lt $ChaseDeg){ $script:M.chaseHold+=$script:M.per; $fire+=" HOLD=$([math]::Round($script:M.chaseHold,0))s" }
    $gr= if($isG){ $(if($gunMode){ $GunFireRange+0.5*$V*$script:M.per }else{ $GroundGunRange }) }else{ $GunRange }; $gee= if($isG){ 8.0 }else{ $GunDeg }; $gbe= if($isG){ 5.0 }else{ $GunDeg }
    $gh= if($isG){ $(if($gunMode){ $GunStrikeBurst }else{ $GunBurst }) }else{ 1.5 }
    if(-not $holding -and $rng -lt $gr -and $be -lt $gbe -and $ee -lt $gee -and ($T-$script:M.lastGun) -ge ($gh+0.5)){ if(-not $NoFire){ $cmds+=@{key='gun';hold=$gh}; $script:M.stat.gun++ }; $script:M.lastGun=$T; $fire+=' GUN' }
    $mr= if($isG){ [math]::Min($MissileRangeGround,$LockRange) }else{ [math]::Min($MissileRange,$LockRange) }; $mmin= if($isG){ $MissileMinGround }else{ 0.0 }
    if(-not $gunMode -and $FireSalvo -le 0 -and $FireSp -le 0 -and -not $holding -and $rng -lt $mr -and $rng -gt $mmin -and $script:M.lockT -ge $LockSec -and $script:M.salvo -lt $Salvo -and ($T-$script:M.lastMissile) -ge $(if($isG){ [math]::Min($FireEvery,1.8) }else{ $FireEvery })){
      if(-not $NoFire){ $cmds+=@{key='missile';hold=0.30}; $script:M.stat.missile++ }; $script:M.lastMissile=$T; $script:M.salvo++; $fire+=' FIRE'; $script:M.shots[$tgt.id]=1+$(if($script:M.shots.ContainsKey($tgt.id)){ $script:M.shots[$tgt.id] }else{ 0 }); if($null -eq $script:M.firstFire){ $script:M.firstFire=$T } }
  } else { $script:M.lockT=0.0; $script:M.lockAt=-99.0 }
  if($sunMode){ $fire=("sunRelBrg={0:N0} sunRelEl={1:N0} ph={2} al={3:N1}s" -f ((Wrap ($SunHdgE-$h))*180/[math]::PI),(($SunElevE-$p)*180/[math]::PI),$script:M.sunPh,$script:M.sunAlSec) }
  $script:M.per= if($kill -and $tgt -and (($tgt.t.cls -eq 'ground' -and $tgt.d -lt 14000 -and -not $breaking) -or ($holdAir -and $tgt.d -lt 20000))){ 1.0 }else{ $Period }
  $txt=('T={0,6:N1} pos={1:N0},{2:N0},{3:N0} h={4:N2} p={5:N3} r={6:N3} | {7} hd={8:N2} he={9:N2} rt={10:N2} | {11} pt={12:N3} | {13} | {14}' -f $T,$st.x,$st.y,$st.z,$h,$p,$r,$why,$hd,$he,$rt,$pm,$pt,$fire,(($cmds | % { "$($_.key)/$($_.hold)" }) -join ' '))
  @{cmds=$cmds;note=$txt}
}
function PushHist($s){ $h=$script:M.hist; $h.Add($s); if($h.Count -gt 40){ $h.RemoveAt(0) } }
# ---- batch M: flight liveness. Batch L1a/K1/KA3: after the overflight (T~62) the ee-camera aircraft struct reads zero (pos 46,-1,1; the camera fields too) while the game keeps
# flying (HUD ALT 3500) and the hook keeps writing ee-camera lines (f= advances) and mk360 snapshots. That is NOT the end of the mission: the script keeps flying on a
# dead-reckoned state (the fitted plant fed with its own key taps) and FIXES it with the range/bearing/elevation of remembered STATIONARY ground sites from the mk360 stream.
function DrAdvance([double]$T){ $pl=$script:DR; if(-not $pl){ return }; $tt=$T-$script:M.drT0; while($pl.t+0.05 -le $tt){ PlantStep $pl 0.05 } }
function DrState([double]$nowTick){ $pl=$script:DR; [pscustomobject]@{h=$pl.h;p=$pl.p;r=$pl.r;x=$pl.x;y=$pl.y;z=$pl.z;tick=$nowTick} }
$script:EvQ=New-Object System.Collections.Generic.List[string]
function LogE([string]$m){ $script:EvQ.Add($m) }   # Estimate() returns the state: its messages are queued and printed by FlushEv after the call
function FlushEv { foreach($m in $script:EvQ){ Log $m }; $script:EvQ.Clear() }
function Estimate($s,[double]$T,[double]$nowTick){
  if($s){
    if($script:DR){ $d=DrState $nowTick; LogE ("T={0:N1} flight state back after {1:N1} s blind: dead-reckoned {2:N0},{3:N0},{4:N0} h={5:N2} vs read {6:N0},{7:N0},{8:N0} h={9:N2}; {10} contact fixes" -f $T,($T-$script:M.blindStart),$d.x,$d.y,$d.z,$d.h,$s.x,$s.y,$s.z,$s.h,$script:M.fixes); $script:DR=$null
# LOCK-HOLD eye shots (live only): %LOCALAPPDATA%\acz-vr\png-eye.now (+ png-screen.now) re-arm the DLL's xr_png_once dump (cfg xr_png_once=true, FLIGHT-DUMP) when the hold
# reaches 3 s (the seeker diamond moving onto the box) and at LOCK-HELD (the locked diamond); -NoLockShots turns them off. The tap summary counts them (lockshots=N).
function LockShot([string]$why){ if(-not $script:Live -or $NoLockShots){ return }; try { $d=Join-Path $env:LOCALAPPDATA 'acz-vr'; Set-Content -Path (Join-Path $d 'png-eye.now') -Value ("LOCK-HOLD {0}" -f $why) -Encoding ascii; Set-Content -Path (Join-Path $d 'png-screen.now') -Value ("LOCK-HOLD {0}" -f $why) -Encoding ascii; $script:M.stat.lockshots++; Log ("LOCK-SHOT {0}: png-eye.now + png-screen.now written" -f $why) } catch { Log ("LOCK-SHOT {0}: not written: {1}" -f $why,$_) } }; $script:M.blind=$false; $script:M.blindT=0.0 }
    $script:M.lastGood=$s; $script:M.lastGoodT=$T; $script:M.lastGoodTs=$T-[math]::Max(0.0,($nowTick-$s.tick)/1000); return $s }
  if(-not $script:M.lastGood){ return $null }
  if(-not $script:DR){ $g=$script:M.lastGood; $a=[math]::Max(0.0,$T-$script:M.lastGoodT); $c=[math]::Cos($g.p)
    $seed=[pscustomobject]@{h=(Wrap ($g.h+$KTURN*[math]::Tan($g.r)*[math]::Max(0.0,$T-$script:M.lastGoodTs)));p=$g.p;r=$g.r;x=($g.x-$V*[math]::Sin($g.h)*$c*$a);y=($g.y+$V*[math]::Sin($g.p)*$a);z=($g.z-$V*[math]::Cos($g.h)*$c*$a)}
    $script:DR=NewPlant $seed; $script:DR.dr=$true; $script:M.drT0=$T; $script:M.blind=$true; $script:M.blindStart=$T; $script:M.fixes=0
    # the taps whose release is not in the last good sample must be in the dead reckoning too (roll does not self-level: a missed 0.3 s roll tap is a 0.28 rad bank error = a 30 km position error in 100 s)
    # (taps released before the sample are only partly in it: first-order lag 0.45 s; taps released after the sample but before now are complete; taps still held become pending keys)
    foreach($tp in $script:M.taps){ $u=1.0
      if($tp.rel -le $script:M.lastGoodTs){ $u=[math]::Exp(-($script:M.lastGoodTs-$tp.rel)/0.45); if($u -lt 0.03){ continue } }
      elseif($tp.rel -gt $T){ [void]$script:DR.pend.Add(@{t=($tp.rel-$T);key=$tp.key;hold=$tp.hold}); continue }
      $dpp=0.0; $drr=0.0
      if($tp.key -eq 'down'){ $dpp=$KPITCH*($tp.hold-$PDEAD)*$u } elseif($tp.key -eq 'up'){ $dpp=-$KPITCH*($tp.hold-$PDEAD)*$u } elseif($tp.key -eq 'left'){ $drr=$KROLL*$tp.hold*$u } elseif($tp.key -eq 'right'){ $drr=-$KROLL*$tp.hold*$u }
      $script:DR.p+=$dpp; $script:DR.pg+=$dpp; $script:DR.r+=$drr; $script:DR.rg+=$drr }
    LogE ("T={0:N1} BLIND: aircraft struct lost, hook alive: dead reckoning from {1:N0},{2:N0},{3:N0} h={4:N2} p={5:N3} r={6:N3}" -f $T,$seed.x,$seed.y,$seed.z,$seed.h,$seed.p,$seed.r) }
  DrAdvance $T; $script:M.blindT=$T-$script:M.blindStart
  DrState $nowTick }
function DrFix($ct){
  if(-not $script:DR -or -not $ct -or $ct.tick -le $script:M.fixTick){ return }
  $script:M.fixTick=$ct.tick; $pl=$script:DR; $cs=@($script:M.tracks | ? { $_.fix -eq $ct.tick })
  if($cs.Count -eq 0){ return }
  $hs=$pl.h; $mode='pos'
  if($cs.Count -ge 2){ $bi=-1; $bj=-1; $bs=2500.0
    for($i=0;$i -lt $cs.Count;$i++){ for($j=$i+1;$j -lt $cs.Count;$j++){ $sep=[math]::Sqrt([math]::Pow($cs[$i].pos[0]-$cs[$j].pos[0],2)+[math]::Pow($cs[$i].pos[2]-$cs[$j].pos[2],2)); if($sep -gt $bs){ $bs=$sep; $bi=$i; $bj=$j } } }
    if($bi -ge 0){ $ti=$cs[$bi]; $tj=$cs[$bj]; $di=$ti.rr*[math]::Cos($ti.ee*[math]::PI/180); $dj=$tj.rr*[math]::Cos($tj.ee*[math]::PI/180); $bri=$ti.bb*[math]::PI/180; $brj=$tj.bb*[math]::PI/180
      $wx=$di*[math]::Sin($bri)-$dj*[math]::Sin($brj); $wz=-$di*[math]::Cos($bri)+$dj*[math]::Cos($brj)   # P_i-P_j in the nose frame (heading 0)
      if([math]::Sqrt($wx*$wx+$wz*$wz) -gt 1500){ $hc=Wrap ((HdgTo ($ti.pos[0]-$tj.pos[0]) ($ti.pos[2]-$tj.pos[2]))-(HdgTo $wx $wz)); if([math]::Abs((Wrap ($hc-$pl.h))) -lt 0.6){ $hs=$hc; $mode='pos+hdg' } } } }
  $sx=0.0;$sy=0.0;$sz=0.0
  foreach($t in $cs){ $el=[math]::PI/180*$t.ee; $d=$t.rr*[math]::Cos($el); $hd=$hs-[math]::PI/180*$t.bb
    $sx+=$t.pos[0]+[math]::Sin($hd)*$d; $sy+=$t.pos[1]-[math]::Sin($el)*$t.rr; $sz+=$t.pos[2]+[math]::Cos($hd)*$d }
  $sx/=$cs.Count; $sy/=$cs.Count; $sz/=$cs.Count
  $jump=[math]::Sqrt([math]::Pow($sx-$pl.x,2)+[math]::Pow($sz-$pl.z,2))
  if($jump -gt 15000){ Log ("      DR fix rejected: jump {0:N0} u from {1} site(s)" -f $jump,$cs.Count); return }
  $g= if($mode -eq 'pos+hdg'){ 0.7 }else{ 0.5 }
  $pl.x+=$g*($sx-$pl.x); $pl.y+=$g*($sy-$pl.y); $pl.z+=$g*($sz-$pl.z); if($mode -eq 'pos+hdg'){ $pl.h=Wrap ($pl.h+0.5*(Wrap ($hs-$pl.h))) }
  $script:M.fixes++; Log ("      DR fix {0} from {1} static site(s): moved {2:N0} u" -f $mode,$cs.Count,$jump) }
# ---- plant model (for -Sim) -------------------------------------------------------------------------------------------------------
function NewPlant($s){ @{t=0.0;h=$s.h;p=$s.p;r=$s.r;x=$s.x;y=$s.y;z=$s.z;rg=$s.r;pg=$s.p;pend=New-Object System.Collections.Generic.List[object];rnd=New-Object System.Random(7);dr=$false} }
$script:S=@{lock=0.0;hitLock=0;blind=0;gunSec=0.0;gunUntil=-1.0;bursts=0;rng=1e9;be=999.0;ee=999.0}
function PlantKey($pl,[string]$key,[double]$hold,[double]$t){ # effect applies when the key is released, lag 1.5 s ramp modelled as first order tau 0.45 s
  $k=$key
  if($k -eq 'missile'){ if($script:S.lock -ge $SimLockSec -and $script:S.rng -lt 9000){ $script:S.hitLock++ } else { $script:S.blind++ }; return }
  if($k -eq 'gun'){ $script:S.gunUntil=$t+$hold; $script:S.bursts++; return }
  if($k -eq 'tswitch'){ return }   # LOCK-HOLD target switch: no plant effect
  $pl.pend.Add(@{t=$t+$hold;key=$k;hold=$hold}) }
function PlantStep($pl,[double]$dt){
  $pl.t+=$dt
  for($i=$pl.pend.Count-1;$i -ge 0;$i--){ $e=$pl.pend[$i]; if($pl.t -ge $e.t){ $hh=$e.hold
    switch($e.key){ 'left'{$pl.rg+=$KROLL*$hh} 'right'{$pl.rg-=$KROLL*$hh} 'down'{ if($hh -gt $PDEAD){$pl.pg+=$KPITCH*($hh-$PDEAD)} } 'up'{ if($hh -gt $PDEAD){$pl.pg-=$KPITCH*($hh-$PDEAD)} } }
    $pl.pend.RemoveAt($i) } }
  $a=1-[math]::Exp(-$dt/0.45); $dr=($pl.rg-$pl.r)*$a; $pl.r+=$dr; $dp=($pl.pg-$pl.p)*$a; $pl.p+=$dp
  $pl.pg+=(-0.05*$pl.r*$pl.r-0.015*$pl.p)*$dt; $pl.p+=(-0.05*$pl.r*$pl.r-0.015*$pl.p)*$dt
  if($SimNoise -gt 0 -and -not $pl.dr){ $n=($pl.rnd.NextDouble()-0.5)*2*$SimNoise*$dt; $pl.r+=$n; $pl.rg+=$n }
  $pl.h=Wrap ($pl.h+$KTURN*[math]::Tan($pl.r)*$dt)
  $c=[math]::Cos($pl.p); $pl.x+=-$V*[math]::Sin($pl.h)*$c*$dt; $pl.z+=-$V*[math]::Cos($pl.h)*$c*$dt; $pl.y+=$V*[math]::Sin($pl.p)*$dt
}
# -Sim: one mk360 snapshot computed from the plant state: the scripted ground target (green) and, with -SimL4, a wingman that REUSES the target's id 35 on every odd snapshot
function SimContact([int]$id,$pl,[double[]]$P,[double]$v,[double[]]$rgb){
  $dx=$P[0]-$pl.x;$dy=$P[1]-$pl.y;$dz=$P[2]-$pl.z; $hz=[math]::Sqrt($dx*$dx+$dz*$dz)
  [pscustomobject]@{id=$id;win='win';r=[math]::Sqrt($hz*$hz+$dy*$dy);b=(Wrap ($pl.h-(HdgTo $dx $dz)))*180/[math]::PI;e=[math]::Atan2($dy,$hz)*180/[math]::PI;v=$v;ident=$true;col=(ColorClass $rgb[0] $rgb[1] $rgb[2]);gl=0} }
function SimTgtPos($tgtP,[double]$tk){ if($SimTargetV -le 0){ return $tgtP }; if([math]::Abs($SimTargetTurn) -gt 1e-6){ $w=$SimTargetTurn; $k=$SimTargetV/$w; return @(($tgtP[0]+$k*([math]::Cos($SimTargetHdg+$w*$tk)-[math]::Cos($SimTargetHdg))),$tgtP[1],($tgtP[2]-$k*([math]::Sin($SimTargetHdg+$w*$tk)-[math]::Sin($SimTargetHdg)))) }; @(($tgtP[0]-$SimTargetV*[math]::Sin($SimTargetHdg)*$tk),$tgtP[1],($tgtP[2]-$SimTargetV*[math]::Cos($SimTargetHdg)*$tk)) }
$script:SimFlipP=$null
function SimSnapshot($pl,$tgtP0,[int]$k,[double]$tk){
  $c=@(); $tgtP=SimTgtPos $tgtP0 $tk
  if($SimFlipRange -gt 0){   # KA2: inside the range the target is no longer listed; the same id now belongs to a yellow contact 6 km ahead at -SimFlipDeg off the nose
    if(-not $script:SimFlipP -and [math]::Sqrt([math]::Pow($pl.x-$tgtP[0],2)+[math]::Pow($pl.z-$tgtP[2],2)) -lt $SimFlipRange){ $fh=$pl.h+$SimFlipDeg*[math]::PI/180; $script:SimFlipP=@(($pl.x-[math]::Sin($fh)*6000),500.0,($pl.z-[math]::Cos($fh)*6000)) }
    if($script:SimFlipP){ $c+=SimContact 49 $pl $script:SimFlipP 0 @(1.0,0.85,0.0); $c+=SimContact 36 $pl @(($pl.x+20000),($pl.y+3000),($pl.z+15000)) 1000 @(0.21,0.23,1.0); return [pscustomobject]@{tick=$tk*1000;c=$c} } }
  if($SimHomeJunk){ for($j=0;$j -lt 6;$j++){ $c+=SimContact (1+$j) $pl @(($HomeX+2500*$j-5000),500,($HomeZ-3000-800*$j)) 0 @(1.0,1.0,1.0) } }
  $inRange=(-not ($SimRange -gt 0)) -or ([math]::Sqrt([math]::Pow($pl.x-$tgtP[0],2)+[math]::Pow($pl.z-$tgtP[2],2)) -lt $SimRange)
  if(-not $inRange){ $c+=SimContact 36 $pl @(($pl.x+20000),($pl.y+3000),($pl.z+15000)) 1000 @(0.21,0.23,1.0); return [pscustomobject]@{tick=$tk*1000;c=$c} }
  $rgb=$SimTargetRgb.Split(',') | % { [double]$_ }
  if($SimL4 -and ($k % 2) -eq 1){
    $f=@((-[math]::Sin($pl.h)),(-[math]::Cos($pl.h))); $w=@(($pl.x+120*[math]::Cos($pl.h)-150*$f[0]),($pl.y-20),($pl.z-120*[math]::Sin($pl.h)-150*$f[1]))
    $c+=SimContact 35 $pl $w 1127 @(0.21,0.23,1.0)
  } else { $c+=SimContact $(if($SimL4){35}else{49}) $pl $tgtP $SimTargetV $rgb }
  $c+=SimContact 36 $pl @(($pl.x+20000),($pl.y+3000),($pl.z+15000)) 1000 @(0.21,0.23,1.0)   # a distant wingman on its own id
  [pscustomobject]@{tick=$tk*1000;c=$c} }
function Log([string]$s){ Write-Output $s; if($script:dlog){ Add-Content -Path $script:dlog -Value $s } }
$script:dlog=$DecisionLog
# =================================================================================================================================
if($Sim){
  $start=[pscustomobject]@{h=-0.785398;p=0.0;r=0.0;x=-23500.0;y=4000.0;z=131300.0}
  if($SimStart){ $q=$SimStart.Split(',') | % { [double]$_ }; $start=[pscustomobject]@{h=$q[3];p=0.0;r=0.0;x=$q[0];y=$q[1];z=$q[2]} }
  if($SimFrom){ $d,$at=$SimFrom.Split('@'); $rows=Get-Content (Join-Path $d 'ee-camera.log') | % { ParseObs $_ } | ? { $_ }
    $f=$rows | ? { ValidFlight $_ } | select -First 1; $cand=$rows | ? { (ValidFlight $_) -and (($_.tick-$f.tick)/1000) -ge [double]$at } | select -First 1
    $start=$cand; Log ("sim start from {0} T={1}: pos {2:N0},{3:N0},{4:N0} h={5:N2} p={6:N3} r={7:N3}" -f $d,$at,$cand.x,$cand.y,$cand.z,$cand.h,$cand.p,$cand.r) }
  if($Stage -eq 'm03k' -and -not $SimStart -and -not $SimFrom){ $start=[pscustomobject]@{h=0.79;p=0.0;r=0.0;x=105000.0;y=20000.0;z=106000.0} }   # the real Mission 03 start (step124 logs)
  AutoStage $start.x $start.z
  $script:Ring=New-Object System.Collections.Generic.List[object]; $pl=NewPlant $start; $tgtP=$null; $nextHist=0.0; $nextSnap=0.0; $snapK=0; $minD=1e12; if($SimTarget){ $tgtP=$SimTarget.Split(',') | % { [double]$_ } }; $tk=0.0; $next=0.0; $dt=0.05; $minY=1e9; $outT=$null; $maxZ=-1e9; $maxLowBank=0.0; $maxEe=0.0; $sunMaxY=0.0
  while($tk -lt $Total){
    PlantStep $pl $dt; $tk+=$dt
    $script:Ring.Add(@($tk,$pl.h,$pl.p,$pl.r,$pl.x,$pl.y,$pl.z)); if($script:Ring.Count -gt 160){ $script:Ring.RemoveAt(0) }
    if($tk -ge $nextHist){ $nextHist+=0.5
      if($script:DR){ DrAdvance $tk; PushHist (DrState ($tk*1000)) } else { PushHist ([pscustomobject]@{h=$pl.h;p=$pl.p;r=$pl.r;x=$pl.x;y=$pl.y;z=$pl.z;tick=$tk*1000}) } }
    if($tgtP -and $tk -ge $nextSnap){ $nextSnap+=5.0; $sn=SimSnapshot $pl $tgtP $snapK $tk; UpdateContacts $sn $null; if($script:DR){ DrAdvance $tk; DrFix $sn }; $snapK++ }
    if($tk -ge $next){ $next=$tk+$script:M.per
      $meas=$null
      if(-not ($SimBlindAt -gt 0 -and $tk -ge $SimBlindAt)){   # the attitude the controller reads is SimStateAge old, its position extrapolated like the live loop does
        $tg=$tk-$SimStateAge; $rg=$script:Ring[0]; foreach($q in $script:Ring){ if($q[0] -le $tg){ $rg=$q } else { break } }; $ag=[math]::Max(0.0,$tk-$rg[0]); $cp=[math]::Cos($rg[2])
        $meas=[pscustomobject]@{h=$rg[1];p=$rg[2];r=$rg[3];x=($rg[4]-$V*[math]::Sin($rg[1])*$cp*$ag);y=($rg[5]+$V*[math]::Sin($rg[2])*$ag);z=($rg[6]-$V*[math]::Cos($rg[1])*$cp*$ag);tick=$rg[0]*1000} }
      $st=Estimate $meas $tk ($tk*1000); FlushEv
      $res=Decide $st $tk ($tk*1000); Log ($res.note + $(if($script:DR){ " | DRerr {0:N0} u dh {1:N2} dp {2:N3} dr {3:N3}" -f [math]::Sqrt([math]::Pow($script:DR.x-$pl.x,2)+[math]::Pow($script:DR.z-$pl.z,2)),(Wrap ($script:DR.h-$pl.h)),($script:DR.p-$pl.p),($script:DR.r-$pl.r) }else{ '' }))
      foreach($c in $res.cmds){ PlantKey $pl $c.key $c.hold $tk; if($script:DR -and $c.key -in 'left','right','up','down'){ [void]$script:DR.pend.Add(@{t=$script:DR.t+$c.hold;key=$c.key;hold=$c.hold}) } } }
    if($tgtP){ $tp=SimTgtPos $tgtP $tk; $sx=$tp[0]-$pl.x;$sy=$tp[1]-$pl.y;$sz=$tp[2]-$pl.z;$sh=[math]::Sqrt($sx*$sx+$sz*$sz)
      $script:S.rng=[math]::Sqrt($sh*$sh+$sy*$sy); $script:S.be=[math]::Abs((Wrap ((HdgTo $sx $sz)-$pl.h)))*180/[math]::PI; $script:S.ee=[math]::Abs([math]::Atan2($sy,$sh)-$pl.p)*180/[math]::PI
      if($script:S.rng -lt 8000 -and $script:S.rng -gt 5000 -and $script:S.be -lt 20 -and $script:S.ee -gt $maxEe){ $maxEe=$script:S.ee }   # elevation error of the real nose vs the real target on the dive leg
      if($script:S.be -lt $SimLockDeg -and $script:S.ee -lt $SimLockDeg -and $script:S.rng -lt 9000){ $script:S.lock+=$dt } else { $script:S.lock=0.0 }
      if($tk -lt $script:S.gunUntil -and $script:S.rng -lt 1500 -and $script:S.be -lt 6 -and $script:S.ee -lt 8){ $script:S.gunSec+=$dt }
      $dd=[math]::Sqrt([math]::Pow($pl.x-$tp[0],2)+[math]::Pow($pl.y-$tp[1],2)+[math]::Pow($pl.z-$tp[2],2)); if($dd -lt $minD){$minD=$dd} }
    $minY=[math]::Min($minY,$pl.y); $maxZ=[math]::Max($maxZ,$pl.z); if($pl.y -lt $LowBankAlt){ $maxLowBank=[math]::Max($maxLowBank,[math]::Abs($pl.r)) }
    if(-not $outT -and -not (InBox $BOX $pl.x $pl.z)){ $outT=$tk }
    if($pl.y -lt 500){ Log ("SIM: CRASH at T={0:N1} (y<500)" -f $tk); break } }
  Log ("SIM done T={0:N0}s: min y {1:N0}, final y {2:N0}, max z {3:N0}, left the mission box: {4}, closest approach to target {6:N0}, taps {5}, first fire decision {7}, breaks {8}, locked missiles {9}, blind missiles {10}, gun bursts {11} on target {12:N1} s, max bank below {13:N0} u {14:N2}, blind fixes {15}, dead-reckoning error {16:N0} u, sun phase {17} aligned {18:N1} s, dive elvErr max {19:N1}, chase hold {20:N1} s" -f $tk,$minY,$pl.y,$maxZ,$(if($outT){"YES at T=$([int]$outT)"}else{'no'}),(($script:M.stat.GetEnumerator() | % { "$($_.Key)=$($_.Value)" }) -join ' '),$minD,$(if($null -ne $script:M.firstFire){"T=$([int]$script:M.firstFire)"}else{'NONE'}),$script:M.stat.breaks,$script:S.hitLock,$script:S.blind,$script:S.bursts,$script:S.gunSec,$LowBankAlt,$maxLowBank,$script:M.fixes,$(if($script:DR){ [math]::Sqrt([math]::Pow($script:DR.x-$pl.x,2)+[math]::Pow($script:DR.z-$pl.z,2)) }else{ 0 }),$script:M.sunPh,$script:M.sunAlSec,$maxEe,$script:M.chaseHold)
  exit 0
}
if($Dry){
  $rows=Get-Content (Join-Path $Dry 'ee-camera.log') | % { ParseObs $_ } | ? { $_ }
  $cts=@(); $xs=Join-Path $Dry 'xr-stereo.log'; if(Test-Path $xs){ $cts=Select-String -Path $xs -Pattern 'event=mk360-contacts' | % { ParseContacts $_.Line } | ? { $_ } }
  $act=@(); if($Events -and (Test-Path $Events)){ $act=Get-Content $Events | ? { $_ -match 'DOWN at T0\+' } }
  $first=$rows | ? { ValidFlight $_ } | select -First 1; if(-not $first){ Write-Output 'no valid flight rows in the log'; exit 2 }
  AutoStage $first.x $first.z
  $t0=$first.tick; $next=0.0; $ci2=0; $frozen=0; $prev=$null; $nCmd=0; $endT=$null
  foreach($s in $rows){
    if($s.tick -lt $t0){ continue }; $T=($s.tick-$t0)/1000
    if(-not (ValidFlight $s)){ continue }
    if($prev -and ([math]::Abs($s.x-$prev.x)+[math]::Abs($s.y-$prev.y)+[math]::Abs($s.z-$prev.z)) -lt 1){ $frozen++ } else { $frozen=0 }; $prev=$s
    if($frozen -ge 3){ $endT=$T; Log ("T={0:N1} state frozen for 3 rows (mission over / replay camera): the autopilot would stop here" -f $T); break }
    PushHist $s
    while($ci2 -lt $cts.Count -and $cts[$ci2].tick -le $s.tick){ UpdateContacts $cts[$ci2] $s; $ci2++ }
    if($T -ge $next){ $next=$T+$script:M.per; $res=Decide $s $T $s.tick; Log $res.note; $nCmd+=$res.cmds.Count } }
  foreach($tk2 in $script:M.tracks){ Log ("TRACK u{0} lastid {1} class {2} colour {3} seen {4}x last pos {5:N0},{6:N0},{7:N0}" -f $tk2.uid,$tk2.cid,$tk2.cls,$tk2.col,$tk2.n,$tk2.pos[0],$tk2.pos[1],$tk2.pos[2]) }
  Log ("DRY done: {0} commands over {1:N0} s of flight; mk360 snapshots {3}, tracks {4}; taps {2}" -f $nCmd,$T,(($script:M.stat.GetEnumerator() | % { "$($_.Key)=$($_.Value)" }) -join ' '),$cts.Count,$script:M.tracks.Count)
  if($act.Count){ Log "--- keys the open-loop driver really pressed (run83 events):"; $act | % { Log $_ } }
  exit 0
}
# ============================ LIVE ===========================================================================================
if(-not ('APK' -as [type])){ Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class APK{
[StructLayout(LayoutKind.Sequential)]public struct KI{public ushort vk,sc;public uint fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Sequential)]public struct MI{public int dx,dy;public uint md,fl,tm;public IntPtr ex;}
[StructLayout(LayoutKind.Explicit)]public struct U{[FieldOffset(0)]public MI mi;[FieldOffset(0)]public KI ki;}
[StructLayout(LayoutKind.Sequential)]public struct IN{public uint type;public U u;}
[DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")]public static extern uint SendInput(uint n,IN[] i,int s);
[DllImport("user32.dll")]public static extern uint MapVirtualKey(uint c,uint t);
public static uint Key(ushort vk,bool up){IN i=new IN();i.type=1;i.u.ki.vk=vk;i.u.ki.sc=(ushort)MapVirtualKey(vk,0);i.u.ki.fl=up?2u:0u;return SendInput(1,new IN[]{i},Marshal.SizeOf(typeof(IN)));}
}
'@ }
$codes=@{up=0x57;down=0x53;left=0x41;right=0x44;missile=0x4C;gun=0x4A;lookleft=0x64;lookright=0x66;lookup=0x68;tswitch=[int][char]$SwitchKey.ToUpper()[0]}   # tswitch: LOCK-HOLD only (-SwitchKey, -NoSwitch)   # same keys as run83.ps1 / acz-window-driver-combat.ps1: nothing else is ever sent
if(-not $script:dlog){ $script:dlog=Join-Path $PSScriptRoot ("autopilot-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$eeLog=Join-Path $LogDir 'ee-camera.log'; $xrLog=Join-Path $LogDir 'xr-stereo.log'
$p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1
if(-not $p){ Log 'no acz window: abort'; exit 2 }
$hw=$p.MainWindowHandle
function Fg { [void][APK]::SetForegroundWindow($hw); Start-Sleep -Milliseconds 60; if([APK]::GetForegroundWindow() -ne $hw){ throw 'not foreground' } }
# batch K2: the fx-air-draw / fx-flare-draw diagnostics (fxsig:ac0f1a60 / f1a4e000) stay idle until %LOCALAPPDATA%cz-vr\fxdiag.arm is created or touched after their first poll
# (fx_diag_flare_air.h start gate; before it the 400-line run cap was used up on the runway). The autopilot starts after takeoff, so T0 = airborne.
function ArmFxDiag { try { $f=Join-Path $env:LOCALAPPDATA 'acz-vr\fxdiag.arm'; Set-Content -Path $f -Value ("armed by autopilot T0 {0}" -f (Get-Date -f HH:mm:ss.fff)) -Encoding ascii; Log ("fxdiag.arm touched: {0}" -f $f) } catch { Log ("fxdiag.arm not written: {0}" -f $_) } }
function ReadState { try { $ls=TailLines $eeLog 65536; for($i=$ls.Count-1;$i -ge 0;$i--){ if($ls[$i].Contains('|ee-camera;obs;')){ $s=ParseObs $ls[$i]; if($s){ return $s } } } } catch {}; $null }
function ReadContacts { try { $ls=TailLines $xrLog 400000; for($i=$ls.Count-1;$i -ge 0;$i--){ if($ls[$i].Contains('event=mk360-contacts')){ return (ParseContacts $ls[$i]) } } } catch {}; $null }
$script:Live=$true   # LOCK-HOLD eye shots are written only by the live controller
Log ("autopilot live: mode {0} period {1}s alt {2} log {3}" -f $Mode,$Period,$TargetAlt,$script:dlog)
$active=New-Object System.Collections.Generic.List[object]
$script:spN=0; $script:spSwitched=$false; $script:spAt=$(if($FireSpAt -ge 0){ $FireSpAt }elseif($FireSalvo -gt 0){ $FireAt+($FireSalvo-1)*$FireEvery+10.0 }else{ 60.0 })
if($FireSp -gt 0){
  if($SpSwitchKey){ $spv=$(if($SpSwitchKey -match '^0[xX][0-9a-fA-F]+$'){ [Convert]::ToInt32($SpSwitchKey.Substring(2),16) }else{ [int][char]$SpSwitchKey.ToUpper()[0] }); $codes['spswitch']=$spv }
  Log ("SPSALVO armed: {0} special-weapon taps from T={1:N1}, every {2:N1} s, regardless of lock; switch key {3}" -f $FireSp,$script:spAt,$FireSpEvery,$(if($SpSwitchKey){ "$SpSwitchKey tapped $SpSwitchLead s before the first tap" }else{ 'UNKNOWN (none tapped): the taps fire the missile key L = the currently selected weapon' }))
  if($FireSalvo -gt 0 -and $script:spAt -lt $FireAt+($FireSalvo-1)*$FireEvery+3.0){ Log 'SPSALVO WARNING: the first SP tap is within 3 s of the last MSSL tap: the HUD reader cannot tell the two counts apart in time' } }
$script:fsN=0; $script:fsRoll=$null; $script:fsHdg=$null; if($FireSalvo -gt 0){ Log ("SALVO armed: {0} missile taps from T={1:N0}, every {2:N1} s, regardless of lock (|roll| <= {3} or after {4} s)" -f $FireSalvo,$FireAt,$FireEvery,$FireRollMax,$FireWaitMax) }
$script:zArmed=$false; $script:zm='level'; $script:zoomN=0; $script:zT=0.0; $script:zAl=0.0; $script:zLast=0.0; $script:nextLook=$LookStart; $script:lookN=0; $script:lookHandsOff=-1.0; $script:luT=-99.0; $script:luHands=$false; $script:luStart=0.0; $script:luDown=$false
function ReleaseAll { foreach($a in $active){ [void][APK]::Key([uint16]$codes[$a.key],$true) }; $active.Clear() }
# wait for a valid flight state (up to 120 s); no keys before
$sw=[Diagnostics.Stopwatch]::StartNew(); $st0=$null
while($sw.Elapsed.TotalSeconds -lt 120){ $st0=ReadState; if((ValidFlight $st0) -and (([Environment]::TickCount64)-$st0.tick) -lt 3000){ break }; if(-not (Get-Process acz -ea 0)){ Log 'acz gone'; exit 3 }; Start-Sleep -Milliseconds 500 }
if(-not (ValidFlight $st0)){ Log 'no valid flight state within 120 s: abort (no keys sent)'; exit 2 }
Log 'flight state valid: T0'; AutoStage $st0.x $st0.z; if(-not $NoArmFx){ ArmFxDiag }; $sw.Restart(); $next=0.0; $frozen=0; $prev=$null; $lastTick=0.0; $lastGame=0.0; $exit=0; $deadSince=$null; $lastEstLog=-99.0
try {
 while($sw.Elapsed.TotalSeconds -lt $Total){
  $T=$sw.Elapsed.TotalSeconds
  for($i=$active.Count-1;$i -ge 0;$i--){ if($T -ge $active[$i].up){ [void][APK]::Key([uint16]$codes[$active[$i].key],$true); $active.RemoveAt($i) } }
  if($SunLookUp -and $sunMode -and -not $script:luHands -and $script:M.sunPh -eq 1 -and $script:M.sunAlSec -ge 4){ $script:luHands=$true; $script:luStart=$T
    if($HeadLook){ Log ('T={0:N1} HEAD LOOK-UP: aligned, synthetic head pitch +{1} for {2} s, flight keys continue (the game camera stays forward)' -f $T,$HeadUpPitch,$LookSec); [void](HeadCmd 0 $HeadUpPitch $LookSec 'HEADLOOK' 'lookup' $T) }
    else { Log ('T={0:N1} HANDS-OFF: aligned, look-up held, no flight keys (the camera look corrupts the attitude read)' -f $T) } }
  if($script:luHands -and ($T-$script:luStart) -gt $LookSec){ Log 'look-up window done'; break }
  if($SunLookUp -and -not $HeadLook){ if($script:luHands){ if(($T-$script:luT) -ge 4){ $script:luT=$T; try { Fg; [void][APK]::Key([uint16]$codes['lookup'],$false); if(-not $script:luDown){ Log ("T={0:N1} LOOKUP down" -f $T) }; $script:luDown=$true } catch {} } } elseif($script:luDown){ [void][APK]::Key([uint16]$codes['lookup'],$true); $script:luDown=$false; Log ("T={0:N1} LOOKUP up" -f $T) } }
  if($LookEvery -gt 0 -and $T -ge $script:nextLook -and $null -eq $script:frozenSince){   # testhmd-first-flights.md: no looks while the state is frozen (cutscene / mission over: no cockpit to see)
    $lk=$(if(($script:lookN % 2) -eq 0){ 'lookleft' }else{ 'lookright' }); $script:lookN++; $script:nextLook=$T+$LookEvery
    if($HeadLook){ [void](HeadCmd $(if($lk -eq 'lookleft'){ $HeadYaw }else{ -$HeadYaw }) $HeadLookPitch $LookHold 'HEADLOOK' $lk $T) }
    else { try { Fg; [void][APK]::Key([uint16]$codes[$lk],$false); $active.Add([pscustomobject]@{key=$lk;up=$T+$LookHold}); Log ("T={0:N1} LOOK {1} hold {2}" -f $T,$lk,$LookHold) } catch { Log ("T={0:N1} look skipped: {1}" -f $T,$_) } } }
  HeadShotDue $T
  if($FireSalvo -gt 0 -and $script:fsN -lt $FireSalvo){ $due=$FireAt+$script:fsN*$FireEvery   # SALVO: nominal time of the next tap; fires in this 20 ms loop (not on the 2 s decision grid) while wings level
    if($T -ge $due -and -not (@($active | ? { $_.key -eq 'missile' }).Count)){ $rollOk=($null -ne $script:fsRoll -and [math]::Abs($script:fsRoll) -le $FireRollMax); $waited=($T-$due) -ge $FireWaitMax
      if($rollOk -or $waited){ try { Fg; $tkTap=[Environment]::TickCount64; [void][APK]::Key([uint16]$codes['missile'],$false); $active.Add([pscustomobject]@{key='missile';up=$T+0.30}); $script:fsN++; $script:M.stat.missile++
          Log ('T={0:N1} SALVO tap {1}/{2} missile tick={3} nominal T={4:N1} roll={5} hdg={6}{7}' -f $T,$script:fsN,$FireSalvo,$tkTap,$due,$(if($null -ne $script:fsRoll){ '{0:N2}' -f $script:fsRoll }else{ 'n/a' }),$(if($null -ne $script:fsHdg){ '{0:N2}' -f $script:fsHdg }else{ 'n/a' }),$(if(-not $rollOk){ ' WAITED (not level)' }else{ '' })) } catch { Log ('T={0:N1} SALVO tap skipped: {1}' -f $T,$_); $script:fsN++ } } } }
  if($FireSp -gt 0 -and $script:spN -lt $FireSp -and ($FireSalvo -le 0 -or $script:fsN -ge $FireSalvo)){   # SP-STORES: the special-weapon salvo runs after the MSSL one (same 20 ms loop, same wings-level gate)
    if($SpSwitchKey -and -not $script:spSwitched -and $T -ge ($script:spAt-$SpSwitchLead) -and -not (@($active | ? { $_.key -eq 'missile' -or $_.key -eq 'spswitch' }).Count)){
      try { Fg; $tkSw=[Environment]::TickCount64; [void][APK]::Key([uint16]$codes['spswitch'],$false); $active.Add([pscustomobject]@{key='spswitch';up=$T+0.20}); $script:spSwitched=$true
        Log ('T={0:N1} SPSWITCH tap key={1} tick={2}' -f $T,$SpSwitchKey,$tkSw) } catch { Log ('T={0:N1} SPSWITCH tap skipped: {1}' -f $T,$_) } }
    $due=$script:spAt+$script:spN*$FireSpEvery
    if($T -ge $due -and ($script:spSwitched -or -not $SpSwitchKey) -and -not (@($active | ? { $_.key -eq 'missile' -or $_.key -eq 'spswitch' }).Count)){
      $rollOk=($null -ne $script:fsRoll -and [math]::Abs($script:fsRoll) -le $FireRollMax); $waited=($T-$due) -ge $FireWaitMax
      if($rollOk -or $waited){ try { Fg; $tkTap=[Environment]::TickCount64; [void][APK]::Key([uint16]$codes['missile'],$false); $active.Add([pscustomobject]@{key='missile';up=$T+0.30}); $script:spN++; $script:M.stat.missile++
          Log ('T={0:N1} SPSALVO tap {1}/{2} missile tick={3} nominal T={4:N1} roll={5}{6}' -f $T,$script:spN,$FireSp,$tkTap,$due,$(if($null -ne $script:fsRoll){ '{0:N2}' -f $script:fsRoll }else{ 'n/a' }),$(if(-not $rollOk){ ' WAITED (not level)' }else{ '' })) } catch { Log ('T={0:N1} SPSALVO tap skipped: {1}' -f $T,$_) } } } }
  if($T -ge $next){ $next=$T+$script:M.per
    if($LookAtContacts -and $T -lt $script:lookHandsOff){ Log ("T={0:N1} look hold: hands-off (attitude reads the look)" -f $T); Start-Sleep -Milliseconds 20; continue }
    if(-not (Get-Process acz -ea 0)){ Log ("T={0:N1} acz gone" -f $T); $exit=3; break }
    $s=ReadState; $nowTick=[double][Environment]::TickCount64
    $fresh=($s -and ($nowTick-$s.tick) -le 3000)
    $ct=$null; if($kill){ $ct=ReadContacts }
    if($LookAtContacts -and $ct -and $T -ge $script:nextLook -and ($nowTick-$ct.tick) -lt 4000 -and $null -eq $script:frozenSince){   # not while frozen (step185 runAGB1: a head shot of the mission-end cinematic)
      # testhmd-first-flights.md: prefer a contact with an identity (ident: the game boxed it once, so mk360:3 has a box to show); else the nearest any
      $bc=$null; foreach($pass in 0,1){ if($bc){ break }; foreach($c in $ct.c){ if($pass -eq 0 -and -not $c.ident){ continue }; $ab=[math]::Abs($c.b); if($ab -ge $LookMinDeg -and $ab -le $LookMaxDeg -and $c.r -le $LookRange -and ((-not $bc) -or $c.r -lt $bc.r)){ $bc=$c } } }
      if($bc -and $HeadLook){ $lk=$(if($bc.b -gt 0){ 'lookright' }else{ 'lookleft' })   # NULL-HEAD: yaw toward the bearing (b + = right of the nose; head yaw + = left), pitch toward the elevation (minus the nose pitch when known)
        $pn=$(if($fresh){ $bc.e-($s.p*57.29578) }else{ $bc.e }); $pn=[math]::Max(-45.0,[math]::Min(60.0,$pn)); $sl=[math]::Max([math]::Abs($bc.b),[math]::Abs($pn))/[math]::Max(10.0,$HeadSlew)
        $script:nextLook=$T+$LookHold+2*$sl+$LookCool
        [void](HeadCmd (-$bc.b) $pn $LookHold 'HEADLOOKC' $lk $T (" contact id={0} col={1} r={2:N0} b={3:N1} e={4:N1} v={5:N0} win={6}" -f $bc.id,$bc.col,$bc.r,$bc.b,$bc.e,$bc.v,$bc.win)) }
      elseif($bc){ $lk=$(if($bc.b -gt 0){ 'lookright' }else{ 'lookleft' }); $script:nextLook=$T+$LookHold+$LookCool; $script:lookHandsOff=$T+$LookHold+1.5
        try { Fg; [void][APK]::Key([uint16]$codes[$lk],$false); $active.Add([pscustomobject]@{key=$lk;up=$T+$LookHold}); Log ("T={0:N1} LOOKC {1} hold {2} contact id={3} col={4} r={5:N0} b={6:N1} e={7:N1} v={8:N0} win={9}" -f $T,$lk,$LookHold,$bc.id,$bc.col,$bc.r,$bc.b,$bc.e,$bc.v,$bc.win) } catch { Log ("T={0:N1} lookc skipped: {1}" -f $T,$_) } } }
    if($fresh -and (ValidFlight $s)){
      if($prev -and $s.tick -ne $prev.tick){ if(([math]::Abs($s.x-$prev.x)+[math]::Abs($s.y-$prev.y)+[math]::Abs($s.z-$prev.z)) -lt 1){ $frozen++ } else { $frozen=0 } }; $prev=$s
      if($frozen -ge 3){ if($null -eq $script:frozenSince){ $script:frozenSince=$T; Log ("T={0:N1} aircraft state frozen: cutscene or mission over: hands-off, waiting up to {1} s" -f $T,$FreezeMax) }
        if(($T-$script:frozenSince) -ge $FreezeMax){ Log ("T={0:N1} aircraft state frozen for {1:N0} s: mission over / replay: stop" -f $T,($T-$script:frozenSince)); $exit=3; break }
        Start-Sleep -Milliseconds 20; continue }   # m03k: a scripted cutscene ("Target spotted") freezes the state for a while; no input meanwhile
      if($null -ne $script:frozenSince){ Log ("T={0:N1} state moving again after {1:N0} s frozen: resume" -f $T,($T-$script:frozenSince)); $script:frozenSince=$null }
      $age=($nowTick-$s.tick)/1000; $c=[math]::Cos($s.p)    # extrapolate the position to now
      $se=[pscustomobject]@{h=$s.h;p=$s.p;r=$s.r;x=$s.x-$V*[math]::Sin($s.h)*$c*$age;y=$s.y+$V*[math]::Sin($s.p)*$age;z=$s.z-$V*[math]::Cos($s.h)*$c*$age;tick=$s.tick}
      $est=Estimate $se $T $nowTick; FlushEv; PushHist $s; $deadSince=$null
    } else {
      # batch M: the struct reads zero / is stale. Liveness comes from the hook (ee-camera lines still arriving = frame counter advancing, or an mk360 snapshot < 12 s old), not from the struct.
      $hookAlive=($fresh -or ($ct -and ($nowTick-$ct.tick) -lt 12000))
      if($hookAlive){ $deadSince=$null } else { if($null -eq $deadSince){ $deadSince=$T }; if(($T-$deadSince) -gt $HookDeadSec){ Log ("T={0:N1} no ee-camera line and no mk360 snapshot for {1:N0} s: game/hook gone: stop" -f $T,($T-$deadSince)); $exit=3; break } }
      if($script:M.blind -and $script:M.blindT -gt $BlindMax){ Log ("T={0:N1} blind for {1:N0} s (> BlindMax): stop" -f $T,$script:M.blindT); $exit=3; break }
      $est=Estimate $null $T $nowTick; FlushEv
      if(-not $est){ Log ("T={0:N1} no flight state yet and no history: no command" -f $T); continue }
      PushHist $est; $age=0.0
      if($T -ge $lastEstLog+10){ $lastEstLog=$T; Log ("T={0:N1} (blind {1:N0} s, hook alive={2}, struct pos {3})" -f $T,$script:M.blindT,$hookAlive,$(if($s){ "{0:N0},{1:N0},{2:N0}" -f $s.x,$s.y,$s.z }else{ 'n/a' })) }
    }
    if($kill){ UpdateContacts $ct $est; if($script:DR){ DrAdvance $T; DrFix $ct } }
    if($est){ $script:fsRoll=$est.r; $script:fsHdg=$est.h }
    $res=Decide $est $T $nowTick; Log ("{0} age={1:N2}{2}" -f $res.note,$age,$(if($script:DR){ ' BLIND' }else{ '' }))
    if($script:luHands -and -not $HeadLook){ Start-Sleep -Milliseconds 20; continue }
    foreach($cmd in $res.cmds){ $busy=$false; foreach($a in $active){ if($a.key -eq $cmd.key){$busy=$true} }; if($busy){ continue }
      try { Fg; [void][APK]::Key([uint16]$codes[$cmd.key],$false); $active.Add([pscustomobject]@{key=$cmd.key;up=$T+$cmd.hold}); if($script:DR -and $cmd.key -in 'left','right','up','down'){ [void]$script:DR.pend.Add(@{t=$script:DR.t+$cmd.hold;key=$cmd.key;hold=$cmd.hold}) } } catch { Log ("T={0:N1} key {1} skipped: {2}" -f $T,$cmd.key,$_) } }
  }
  Start-Sleep -Milliseconds 20
 }
} finally { if($script:luDown){ [void][APK]::Key([uint16]$codes['lookup'],$true) }; HeadHome $sw.Elapsed.TotalSeconds; ReleaseAll; Log ("autopilot done at T={0:N1}; taps {1}" -f $sw.Elapsed.TotalSeconds,(($script:M.stat.GetEnumerator() | % { "$($_.Key)=$($_.Value)" }) -join ' ')) }
exit $exit
