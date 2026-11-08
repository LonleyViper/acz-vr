# XR-START: how the in-proxy XR session starts, how it is logged, how it retries

Track XR-START (overnight mandate D-302; evidence D-323). Code: `src/acz-vr/xr_stereo_proxy.cpp`
(monitor thread), `xr_owner.cpp/.h` (owner: every OpenXR call, deadlines, watchdog),
`xr_owner_openxr.cpp` (real OpenXR backend), `xr_start.cpp/.h` (pure attempt diagnosis and
retry policy), `xr_stereo_output.cpp` (consumer; `start_ready`, `retry_allowed`). Tests:
`tests/acz-vr/xr_owner_offline_tests.cpp` (fake runtime and clock; `xr_start_*` cases).

No game or SteamVR run has exercised the new lines or the persistent retry yet. Everything
below about real-headset behaviour is labelled as evidence (seen in a log) or inference.

## 1. The start sequence

1. `armed` (device hook on the game's D3D12 device): the monitor thread starts. Nothing touches
   OpenXR yet.
2. Lazy start. The monitor polls every 100 ms until the producer has published a StereoFrame
   with a non-zero extent. In direct launches that happens only when the mission starts.
   Evidence: step75 (D-323) 113 s and 128 s after launch; headset tests 9-16 between 9.6 s and
   32.5 s. Until then no OpenXR instance exists, so SteamVR shows no app and the log holds only
   `armed`, `identity` and `waiting-for-source` lines.
3. `source-ready`, then the one-time settings and asset lines (terrain, props, cockpit, HUD).
4. One attempt = one `xr_owner::Owner` on its own owner thread plus a watchdog thread:
   xrCreateInstance, xrGetSystem (form factor HEAD_MOUNTED_DISPLAY) and its enumerations,
   the D3D12 graphics requirements, xrCreateSession, reference spaces, consumer setup
   (swapchain formats, swapchains, D3D12 blit setup), wait for session state READY,
   xrBeginSession, then the frame loop.
5. Deadlines per attempt (unchanged by XR-START):
   - startup 5000 ms from attempt start, covering steps 4 up to and including the whole first
     frame (xrWaitFrame/xrBeginFrame/locate/consumer/xrEndFrame);
   - progress 250 ms per call from frame 2 on (owner self-check and watchdog);
   - cleanup 2000 ms.
   The watchdog only detects a deadline and requests a stop. It cannot cancel a blocked OpenXR
   call.
6. Clean retirement: on any end the owner retires everything in order (consumer images and
   swapchains, request exit, xrEndSession, spaces, session, the device/queue references,
   instance). `clean=1` means every handle was retired inside its deadline. If any step fails,
   the handle and its parents are kept for the life of the process (never destroyed under an
   in-flight call), and that attempt may never be followed by another one.

## 2. Retry policy

- Attempt 1 failing with a cold-start timeout (startup deadline, READY timeout, progress
  deadline, or a first frame refused for the startup budget with nothing submitted), clean and
  with no loss: one immediate retry. This rule is unchanged.
- Persistent retry (new, default ON). When an attempt fails for a transient reason and retired
  cleanly, the monitor tries again after 3 s, 5 s, 10 s, then every 15 s, for as long as the
  game process runs. At most 6 attempt starts in any 60 s window; when the cap would be
  exceeded the wait is extended (`yes-persistent(rate-capped)`).
- Bounded schedule (R-5, docs/review/overnight-runtime-review.md). Measured from the first
  attempt's start: after 10 minutes of failed attempts the wait is at least 60 s
  (`yes-persistent(slow-60s)`), after 60 minutes at least 5 minutes
  (`yes-persistent(slow-5min)`). It never stops for good while the game runs. Each step is
  logged once (`retry-slowdown`). Offline: 3 hours without a headset = 116 attempts (42 in the
  first 10 min, 50 in 10-60 min, 24 in 60-180 min) instead of about 720 at every 15 s, so
  xrCreateInstance (which can start SteamVR's server) runs 12 times per hour after the first hour.
- Transient means one of these:
  - xrGetSystem / xrCreateSession / xrBeginSession or another start call (not xrCreateInstance)
    returned XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35, HMD not available: standby, not connected,
    SteamVR still starting), XR_ERROR_RUNTIME_FAILURE (-2), XR_ERROR_SESSION_NOT_READY (-28),
    XR_ERROR_INSTANCE_LOST (-13) or XR_ERROR_SESSION_LOST (-17);
  - the startup deadline, for example xrCreateInstance blocking while SteamVR starts;
  - READY timeout: the session never left IDLE within the startup budget;
  - a progress-deadline failure within the first 450 begun frames (about 5 s), the idle or
    off-face headset first-frame pattern (D-188: first xrEndFrame >250 ms with an idle headset);
  - the first frame refused for the startup budget (cold PSO compile, D-313);
  - a session or instance loss before the first frame completed.
- Permanent (no persistent retry, as before): xrCreateInstance failed (no runtime installed,
  `XR_RUNTIME_JSON` override refused, no XR_KHR_D3D12_enable, any refusal by the runtime), an
  incompatible system or device (LUID / feature level / no LOCAL or VIEW space), another
  XrResult, consumer refusal, unexpected EXITING, events lost or flooded, an exception, or any
  attempt that did not retire cleanly. An attempt that ended after its first frame completed
  (game exit, loss mid-game, watchdog mid-game) is not retried either.
- Escape: set the environment variable `ACZ_XR_RETRY=0` (or `off`, `false`, `no`) for the game
  process to get the previous two-attempt policy exactly. No config key was added (the config
  has 31 of 32 keys). The `armed` line states the active policy.
- Memory (R-5). The producer reads poses through a raw Owner pointer (any thread, any time), so
  owners used to be kept for the process lifetime. They are now published through
  `xr_owner_registry.h`: readers enter a two-counter epoch (no lock, no wait), and the monitor
  releases a FINISHED attempt whose diagnosis is `failed-transient` (that implies `clean=1`,
  complete, no instance/session/binding held) after clearing the published pointer and seeing
  both reader counters drain (bound 500 ms; if they do not drain the owner stays retained as
  before, `release=kept(pose-readers-busy)`). The registry only drops its reference; the
  Owner, its Output, OpenXr backend and blit object are freed with the last reference, and
  none of their destructors calls OpenXR or D3D12 (`~Owner`, `~OpenXr`, `~Blit` make no foreign
  call; `~PropsGpu` joins its worker). Permanent failures, unclean retirements and attempts that
  ended after their start (`ended-after-start`) are never released (the original "never
  destroyed under an in-flight call" rule); an early-frame progress failure counts as transient
  and, having retired cleanly, is released.
  Each attempt logs `owner-retention;attempt=N;release=released|kept(...);published=;released=;
  retained=;busy=`. The call records (4096 x 32 bytes) are still freed after diagnosis.
  Measured offline (before the change): an early failed attempt retained about 0.6 KB of Owner
  (counting allocator, `xr_owner_offline`) plus about 5.3 KB per Output (`sizeof(Output)` 4656 B;
  private-commit probe in `xr_stereo_output_offline`), plus the unmeasured OpenXr/blit CPU
  objects: roughly 6 KB+ per attempt, about 1.5 MB/hour at the old every-15-s rate (the review's
  10 MB/hour was an upper estimate). After: 0 retained owners for transient failures (100 of 100
  released in the measurement, 300 of 300 under 4 hammering pose-reader threads).

Inference, not tested live: SteamVR answers xrGetSystem with FORM_FACTOR_UNAVAILABLE while the
HMD is unavailable. The offline tests prove the policy against a fake runtime only.

- VSEARLY (`vsearly:1`, docs/world-render/vsearly.md; off = everything above unchanged): the start gate also fires on the game's
  first Present (`vsstart:first`, default since 2026-11-01; `vsstart:steady` = >= 120 presents and 3 s; launch-mode.md 3)
  (`start=early-game-presents`), the frame phase uses `Limits::frame_stall_ms` 8000 instead of the 250 ms rule, and a hooked
  attempt that ended with a progress deadline or a loss, retired cleanly, is retried (`yes-early(after-start)`, schedule restarted).

## 3. Log lines (xr-stereo.log; each line is prefixed with `<ms>|`)

| line | meaning |
|---|---|
| `event=armed;...;attempts=1+1-cold-start-retry;persistent-retry=on(transient-only,3s,5s,10s,then-15s;cap-6/min;after-10min-60s;after-60min-5min;ACZ_XR_RETRY=0-disables)` | hook armed (the `after-...` tokens appear from R-5 on). `persistent-retry=off(ACZ_XR_RETRY)` when the escape is set. A log without the `persistent-retry` token comes from an older DLL |
| `event=waiting-for-source;src=...;published=N;...;try-acquire=not-tried\|none\|ok;held-slot=S;held-frame-id=F;held-released=0\|1;probes=P;probe-none=Q;probe-zero-extent=Z` | every 5 s while the lazy start has not fired. `published=0` with `try-acquire=not-tried` means the producer has not published a frame (the mission has not started, or the producer keys are off). `try-acquire=none` with `published>0` means a publication exists but `try_acquire_latest(0)` found no eligible slot (all HELD or PRODUCING). `ok` means the probe HELD slot S (frame F) and released it at once |
| `event=source-ready;...;probe-slot=S;probe-frame-id=F` | the lazy start fires |
| `event=launch;attempt=N;...` | attempt N is starting (owner thread created next) |
| `event=xr-call;n=N;op=<op>;rc=<rc>;xr-result=<NAME>(<num>);t-ms=T;ms=D;phase=startup\|await-ready\|first-frame\|cleanup[;detail=...]` | one line per start-path call of attempt N: owner time T (ms since the attempt started) and duration D. `xr-result=none` = no OpenXR result (a D3D12 op, a refusal by the proxy, or a call refused by a deadline: `rc=refused`). xrPollEvent appears only when it fails. `detail` names the proxy-side reason when xrCreateInstance/xrGetSystem failed without a usable XrResult |
| `event=xr-runtime;n=N;runtime=<name>;runtime-version=a.b.c;create-instance-ms=D` | xrGetInstanceProperties right after xrCreateInstance succeeded |
| `event=xr-system;n=N;form-factor=1(HEAD_MOUNTED_DISPLAY);result=<XrResult>;system-id=...;system-name=...;vendor-id=...;max-layers=...;orientation-tracking=;position-tracking=;get-system-ms=D` | xrGetSystem outcome; on failure `result` is the error and `system-name=unavailable` |
| `event=formats;...`, `event=swapchains;...` | consumer setup (unchanged) |
| `event=session-state;state=K;xr-time-ns=...;owner-t-ms=T;same-session=1;name=IDLE\|READY\|...` | every session state change with its owner time (the `name` token is new) |
| `event=first-xr-frame;wait-ms;begin-ms;end-ms;end-rc` | the first frame's xrWaitFrame/xrBeginFrame/xrEndFrame (unchanged) |
| `event=start-attempt;n=N;outcome=hooked;reason=first-frame-done;...;first-frame-done-ms=T;since-source-ready-ms=S` | attempt N hooked: the first frame completed |
| `event=attempt-ended;attempt=N;...;retry=...` | unchanged summary of the owner report |
| `event=compat-refusal;n=N;stage=system\|binding\|spaces[;req-luid=0xHHHHHHHH:0xLLLLLLLL;req-min-fl=0x..;bind-device=0\|1;bind-queue=0\|1;bind-luid=0xHHHHHHHH:0xLLLLLLLL;bind-max-fl=0x..;bind-queue-direct=0\|1;bind-queue-on-device=0\|1;failed=<terms>\|none]` | emitted right before the `start-attempt` line when the first cause is `compatibility-refusal` (added after step83, whose refusal did not say which field failed). `stage` is the gate: `system` (HMD form factor / PRIMARY_STEREO / 2 views / OPAQUE blend), `binding` (the game's D3D12 device/queue against xrGetD3D12GraphicsRequirementsKHR) or `spaces` (LOCAL/VIEW). Only `binding` carries the fields: `req-*` from the runtime (adapter LUID as HighPart:LowPart, minimum feature level), `bind-*` from the game device (device/queue present, `GetAdapterLuid`, highest supported level of 12_1/12_0/11_1/11_0, queue type DIRECT, `queue->GetDevice` == device). `failed` lists the failing terms of `compatible()`: `device-absent`, `queue-absent`, `min-fl-zero`, `luid-low`, `luid-high`, `feature-level`, `queue-not-direct`, `queue-off-device`. A `req-luid` of all zeros next to a non-zero `bind-luid` means the runtime reported no adapter (see section 4, item 9). A DLL without this line logs only `reason=compatibility-refusal` |
| `event=start-attempt;n=N;outcome=<class>;reason=<reason>;first-failing-call=<op>;xr-result=<NAME>(<num>);call-rc=;call-t-ms=;call-ms=;first-cause=;clean=;loss=;begun=;first-frame-done-ms=;retry=<decision>;next-attempt-in-ms=W` | the structured end of attempt N, described below |
| `event=owner-retention;attempt=N;release=released\|kept(not-transient)\|kept(pose-readers-busy)\|kept(not-finished)\|kept(not-published);published=P;released=R;retained=K;busy=B` | R-5: whether attempt N's owner was released, and the running counts (K = owners still kept) |
| `event=retry-slowdown;tier=1\|2;min-interval-ms=60000\|300000;elapsed-ms=E;attempts=N;never-stops=1` | R-5: the persistent retry slowed down (once per step) |

`outcome`: `failed-transient`, `failed-permanent`, or `ended-after-start` (the attempt had hooked).
`reason` examples: `hmd-unavailable(form-factor-unavailable)`, `runtime-failure`,
`startup-deadline(runtime-slow-to-start)` (xrCreateInstance crossed the 5 s budget),
`startup-deadline`, `ready-timeout(session-never-READY)`, `progress-deadline(early-frame)`,
`first-frame-budget(cold-setup)`, `session-or-instance-lost-during-start`,
`runtime-unavailable(no-openxr-runtime)`, `instance-refused`, `runtime-error`,
`consumer-refused`, `unclean-retirement(handles-retained)`, `operator-stop(game-exit)`,
`orderly-stop`, or the owner cause name. `first-failing-call` is the first call with a
failing Rc. For a startup deadline it is the call that crossed the deadline, and for a
progress deadline the first call after frame 1 that took 250 ms or more. `none` when no call
failed (READY timeout).
`retry`: `yes-cold-start` (the legacy immediate retry), `yes-persistent`,
`yes-persistent(rate-capped)`, `yes-persistent(slow-60s)`, `yes-persistent(slow-5min)`, `no(permanent)`, `no(after-start)`,
`no(persistent-retry-off:ACZ_XR_RETRY=0)`, or `no(stop)`. `next-attempt-in-ms` is the wait
(-1 when there is no next attempt).

## 4. Reading a "VR did not hook" log

1. Only `armed` and `identity` (and maybe `waiting-for-source` with `published=0`): the lazy
   start never fired, so OpenXR was never called. The game closed or the mission was not
   reached before the first published frame (evidence: headset-test10/165917, 2 lines). Check
   that the producer keys are on and that the mission was reached.
2. `waiting-for-source` with `published>0` and `try-acquire=none` repeating: frames are
   published but none can be acquired. This is a producer or slot problem, not OpenXR.
3. `source-ready` and `launch`, then `xr-call;op=create_instance;rc=error`: the runtime refused
   the instance. `xr-result` and `detail` say why (for example
   XR_ERROR_RUNTIME_UNAVAILABLE(-51): no active OpenXR runtime). This is permanent and is not
   retried.
4. `xr-call;op=query_system;...;xr-result=XR_ERROR_FORM_FACTOR_UNAVAILABLE(-35)` then
   `start-attempt;...;outcome=failed-transient;...;retry=yes-persistent`: the HMD was not
   available. Waking or wearing the headset should let a later attempt hook, which shows as
   `start-attempt;n=K;outcome=hooked`.
5. `start-attempt;...;reason=ready-timeout(session-never-READY)`: the session stayed IDLE.
   The `session-state` lines show the last state reached.
6. `reason=startup-deadline...` or `progress-deadline(early-frame)`: compare the `xr-call`
   durations, and `first-xr-frame end-ms` (step75: 128-138 ms with an idle headset, budget
   5000 ms). `first-failing-call` names the slow call.
7. `retry=no(permanent)` or `unclean-retirement`: no further attempt in this game process. A
   game restart is needed. The reason says what to fix.
8. A `start-attempt;outcome=hooked` line means the session ran. If the user still saw nothing,
   look at the frame summaries (`end-failed`, `submitted`), not at the start path.
9. `xr-call;op=query_binding;rc=ok` then `start-attempt;...;reason=compatibility-refusal;...;retry=no(permanent)`:
   read the `compat-refusal` line just before it; `failed=` names the terms. step83
   (2026-10-27 13:34-13:52, three launches, null driver) refused here on every launch, while
   step79 (06:12, same null driver, same SteamVR 2.18.4) hooked. The DLL then did not log the
   fields. Steam's `vrserver.txt` showed a difference: in every working run vrstartup had
   started SteamVR first and vrcompositor was running before acz.exe connected. In step83
   acz.exe's xrCreateInstance cold-started vrserver itself, and no vrcompositor process was
   started. The likely, unconfirmed, cause is a runtime without a compositor/adapter reporting a
   requirement LUID that does not match the game's adapter. Start SteamVR (and let
   vrcompositor come up) before launching the game, then check `failed=` on the next refusal.

Before trusting any of this, check the `identity` line's `dll-sha256` against the installed
set. A stale proxy DLL (step74) logs the old line format, without `xr-call` and
`start-attempt` lines.
