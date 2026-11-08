#pragma once
// POV-RENDER stage 1 (docs/world-render/other-povs.md): the camera rig, routing, comfort vignette and the one-shot POV census for the game's
// non-cockpit views (chase, HUD-only, in-engine cinematics such as the mission-end orbit and the takeoff intro). Sub-options of the existing key
// xr_terrain_stage (no new config key; default OFF = nothing below runs). They need pov:1 (view_mode.h: the classifier and the raw E6 view mailbox).
//   povc:<0|1>                    one-shot POV census: pov-census lines (the raw E6 sample + the derived geometry + the rig) at every view change and
//                                 1 Hz for the first 6 s of each visit, and ONE read-only ACM census request per newly seen view (the ui3d ACM census
//                                 of ui3d_census.h: the frame's DrawACM_v A matrices = the game's own view/projection for that view)
//   povrig:<off|game|level|anchor> the rig the stereo world WOULD be rendered from in chase/cinematic views. Stage 1 is a DRY RUN: the rig runs every XR
//                                 frame and is logged (summary-pov-rig, and in the census lines); nothing is rendered differently. The render needs a
//                                 producer frame for views without a DrawCPT draw (other-povs.md section 5, stage 2).
//   povv:<0..1>                   comfort vignette strength (default 0.60; 0 = no vignette)
//   povtc:<50..3000>              yaw spring time constant of the level rig in ms (default 400)
// Rigs (E6 Euler terms, the same campos/pitch/yaw/roll the terrain camera_frame() consumes; the head pose is added on top by eye_world()):
//   game   = the game's camera exactly (E6 cam position + rot): what the flat picture shows, including the game's own spring and any roll.
//   level  = the game camera POSITION, yaw through a critically damped spring towards the game camera yaw, pitch and roll 0 (horizon level; the
//            aircraft pitches/banks in front of the user, who looks around with the head).
//   anchor = a point that follows the game camera slowly (2 s pull + the camera's low-passed velocity: AnchorCore, stage 4), re-latched with a
//            blink when the game cuts or the camera gets > 80 m away; yaw through a slow spring (1 s), pitch/roll 0. Always used for cinematic
//            views unless povrig:game.
// Stages 2-4 (pov:2 / pov:3, other-povs.md sections 10-11) are at the end of this file: the producer request per view kind (chase, HUD-only,
// cinematic), the opener checks, the producer rigs, the consumer's flat-fallback gate and the drawn vignette.
// SDK-free, header-only; tests/acz-vr/pov_render_offline_tests.cpp (ctest pov_render_offline). Fixture success says nothing about the live game.
#include "view_mode.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace acz::povr {

// ------------------------------------------------------------------------------------------------ options (inside the xr_terrain_stage value)
enum class RigMode : uint8_t { off = 0, game, level, anchor };
inline const char* rig_name(RigMode m) noexcept {
    switch (m) { case RigMode::game: return "game"; case RigMode::level: return "level"; case RigMode::anchor: return "anchor"; default: return "off"; }
}
inline constexpr float kVignetteDefault = 0.6f;
inline constexpr int kYawTcDefaultMs = 400, kYawTcMinMs = 50, kYawTcMaxMs = 3000;
struct Options {
    bool census = false;
    RigMode rig = RigMode::off;
    float vignette = kVignetteDefault;
    int yaw_tc_ms = kYawTcDefaultMs;
    bool any_token = false;
    bool active() const noexcept { return census || rig != RigMode::off; }
};
namespace detail {
inline bool parse_float(const std::string& v, float& out) noexcept {
    if (v.empty() || v.size() > 16) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(d)) return false;
    out = float(d);
    return true;
}
inline bool parse_int(const std::string& v, int& out) noexcept {
    if (v.empty() || v.size() > 6) return false;
    int r = 0;
    for (char c : v) { if (c < '0' || c > '9') return false; r = r * 10 + (c - '0'); }
    out = r;
    return true;
}
} // namespace detail
// Removes every povc / povrig / povv / povtc token (':' or '=') from a "<id>;opt:val;..." value (the first item, the stage id, is never an option).
// A value without them is returned byte-identical. A bad value: false + why; the token is STILL removed (the caller logs and ignores ALL of these
// options: a half-applied set is never used). Last value wins.
inline bool extract_options(const std::string& value, std::string& rest, Options& o, std::string& why) {
    o = Options{};
    rest.clear();
    bool ok = true;
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        bool ours = false;
        if (!first) {
            const size_t eq = item.find_first_of(":=");
            if (eq != std::string::npos) {
                const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
                if (k == "povc") {
                    ours = true;
                    if (v == "1") o.census = true; else if (v == "0") o.census = false; else { ok = false; why = "bad value for povc (0|1)"; }
                } else if (k == "povrig") {
                    ours = true;
                    if (v == "off") o.rig = RigMode::off; else if (v == "game") o.rig = RigMode::game;
                    else if (v == "level") o.rig = RigMode::level; else if (v == "anchor") o.rig = RigMode::anchor;
                    else { ok = false; why = "bad value for povrig (off|game|level|anchor)"; }
                } else if (k == "povv") {
                    ours = true; float f = 0;
                    if (detail::parse_float(v, f) && f >= 0.0f && f <= 1.0f) o.vignette = f; else { ok = false; why = "bad value for povv (0..1)"; }
                } else if (k == "povtc") {
                    ours = true; int t = 0;
                    if (detail::parse_int(v, t) && t >= kYawTcMinMs && t <= kYawTcMaxMs) o.yaw_tc_ms = t; else { ok = false; why = "bad value for povtc (50..3000 ms)"; }
                }
                if (ours) o.any_token = true;
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    if (!ok) { const bool any = o.any_token; o = Options{}; o.any_token = any; }
    return ok;
}
inline std::string describe(const Options& o) {
    char b[120];
    std::snprintf(b, sizeof b, "povc=%u;povrig=%s;povv=%.2f;povtc=%d", o.census ? 1u : 0u, rig_name(o.rig), double(o.vignette), o.yaw_tc_ms);
    return b;
}
// nullptr = usable. The rig and the census read the pov:1 tracker (the raw E6 view mailbox only exists with pov:1).
inline const char* prerequisites(const Options& o, bool pov_on) noexcept {
    if (!o.active()) return nullptr;
    return pov_on ? nullptr : "needs-pov:1";
}

// ------------------------------------------------------------------------------------------------ routing (what each view renders)
// cockpit   = the existing producer-frame path (unchanged).
// hud_only  = the stereo world from the E6 eye camera (identical to the cockpit eye), no cockpit model: needs a no-cockpit producer frame (stage 2).
// chase     = the stereo world from the rig around the game's chase camera: needs a no-cockpit producer frame (stage 2).
// cinematic = an in-engine camera that is neither eye nor chase (mission-end orbit, takeoff intro framing, scripted cameras) with a valid, MOVING
//             E6 block within 400 m of the aircraft: the anchor rig (stage 3).
// flat      = the game's own picture on the virtual screen (vscreen): menus, pre-rendered FMV, the zero block, a frozen E6 block, anything unknown.
enum class Route : uint8_t { none = 0, cockpit, hud_only, chase, cinematic, flat };
inline const char* route_name(Route r) noexcept {
    switch (r) {
        case Route::cockpit: return "cockpit"; case Route::hud_only: return "hud-only"; case Route::chase: return "chase";
        case Route::cinematic: return "cinematic"; case Route::flat: return "flat"; default: return "none";
    }
}
inline constexpr float kCinematicMaxDist = 2000.0f;   // units (~400 m at 5 u/m): the W1 mission-end orbit is 111 units from the aircraft
inline constexpr int64_t kFrozenMs = 1000;            // E6 camera and aircraft bit-identical this long = not a live in-engine camera (FMV, pause, menus)
struct RouteOut { Route r = Route::none; const char* why = "none"; };
class Router {
public:
    RouteOut step(int64_t now_ms, viewmode::Mode m, const viewmode::GeomOut& g, bool sample, const viewmode::Geo& s) noexcept {
        // motion of the E6 camera or the aircraft (bit-exact), for the cinematic route only (a stale block must never steer a camera)
        if (sample && g.g != viewmode::Geom::invalid) {
            if (!have_ || std::memcmp(cam_, s.cam, sizeof cam_) != 0 || std::memcmp(air_, s.air, sizeof air_) != 0) last_move_ms_ = now_ms;
            std::memcpy(cam_, s.cam, sizeof cam_); std::memcpy(air_, s.air, sizeof air_); have_ = true;
        }
        RouteOut o;
        switch (m) {
            case viewmode::Mode::cockpit: o = {Route::cockpit, "producer-frame"}; break;
            case viewmode::Mode::hud_only: o = sample && g.g == viewmode::Geom::eye ? RouteOut{Route::hud_only, "e6-eye-camera"} : RouteOut{Route::flat, "no-eye-sample"}; break;
            case viewmode::Mode::chase: o = sample && g.g == viewmode::Geom::chase ? RouteOut{Route::chase, "e6-chase-camera"} : RouteOut{Route::flat, "no-chase-sample"}; break;
            case viewmode::Mode::other:
                if (!sample) o = {Route::flat, "no-sample"};
                else if (g.g != viewmode::Geom::other) o = {Route::flat, g.g == viewmode::Geom::invalid ? g.why : "not-other-geometry"};
                else if (!(g.dist <= kCinematicMaxDist)) o = {Route::flat, "camera-far-from-aircraft"};
                else if (now_ms - last_move_ms_ > kFrozenMs) o = {Route::flat, "frozen-camera(fmv-pause-or-menu)"};
                else o = {Route::cinematic, "e6-moving-camera-near-aircraft"};
                break;
            default: o = {Route::flat, "unknown-view"}; break;
        }
        return o;
    }
private:
    bool have_ = false;
    float cam_[3]{}, air_[3]{};
    int64_t last_move_ms_ = 0;
};

// ------------------------------------------------------------------------------------------------ the rig (E6 Euler terms)
inline constexpr double kPi = 3.141592653589793;
inline double wrap_pi(double a) noexcept { return viewmode::detail::wrap(a); }
inline constexpr int64_t kMaxStepMs = 250;            // a longer gap restarts the rig (no history across a hitch / a view change)
inline constexpr double kCutSpeed = 20.0;             // units per ms (= the E6 locator's 20,000 u/s jump rule) ...
inline constexpr double kCutSlack = 200.0;            // ... plus this slack: a larger camera move in one step is a camera CUT
inline constexpr double kCutYaw = 45.0 * kPi / 180.0; // a camera yaw change above this in one step is a cut too
inline constexpr int64_t kBlinkMs = 150;              // full vignette after a cut / re-latch / entry (hides the teleport)
inline constexpr double kAnchorTcMs = 2000.0;         // anchor position follow
inline constexpr double kAnchorYawTcMs = 1000.0;      // anchor yaw spring
inline constexpr double kAnchorRadius = 400.0;        // units (80 m at 5 u/m): a farther game camera re-latches the anchor
inline constexpr double kVigRateLo = 30.0, kVigRateHi = 120.0;   // deg/s of rig rotation: vignette starts / reaches full strength
inline constexpr double kVigReleaseMs = 250.0;        // vignette release time constant (attack is immediate)
struct RigOut {
    bool valid = false;
    RigMode mode = RigMode::off;      // the rig actually used (cinematic: anchor unless game)
    float campos[3]{};
    float pitch = 0, yaw = 0, roll = 0;
    bool cut = false, entered = false, relatched = false;
    bool blink = false;               // inside the 150 ms blink of an entry / cut / re-latch (stage 3+: the eye image is drawn black)
    float rate_dps = 0;               // rig rotation rate this step
    float vignette = 0;               // 0..1 (scaled by povv), 1 during a blink
    const char* why = "off";
};
// POV-RENDER stage 4 (2026-10-31): the anchor POSITION follower shared by the consumer rig (vignette, census) and the producer anchor (the eyes).
// Evidence (runPOV1 xr-stereo pov-census n=97..100, the m03 mission-end camera): the camera rides 111 units from the aircraft (99.7 ahead, 49.3 up,
// looking back at it) and TRAVELS WITH IT at ~1,100 u/s. A pure world point following with tc 2 s lags v x tc = 2,200 units in steady state, so the
// stage-1 anchor re-latched (blinked) every ~0.4 s (rig-cam 206 / 378 / 156 units behind in consecutive census lines). The follower therefore
// carries the camera's LOW-PASSED VELOCITY (feed-forward: tc 1 s, 250 ms during the first second after a latch so a moving entry does not run away;
// per-step velocity samples clamped to 4,000 u/s, so a jump below the cut rule only adds <= 4,000 u/s x dt) and pulls toward the camera with tc 2 s:
// a steadily travelling camera is followed with zero steady-state lag (a smooth translation, no rotation), its swings/orbits/jitter are smoothed,
// and only a camera that gets > 400 units (80 m) from the anchor (or a cut) re-latches with a blink.
inline constexpr double kAnchorVelTcMs = 1000.0, kAnchorVelTcEntryMs = 250.0, kAnchorEntryMs = 1000.0;
inline constexpr double kAnchorMaxSpeed = 4000.0;     // u/s (800 m/s): a faster per-step move is not taken as velocity
struct AnchorCore {
    double pos[3]{}, vel[3]{};
    int64_t latched_us = 0;
    // entry or cut: at the camera, no velocity
    void reset(const double cam[3], int64_t now_us) noexcept { for (int i = 0; i < 3; ++i) { pos[i] = cam[i]; vel[i] = 0; } latched_us = now_us; }
    // one step (dt_s > 0): true = re-latched (the camera got > kAnchorRadius from the anchor; the velocity is KEPT: a sustained motion continues)
    bool follow(const double cam[3], const double prev_cam[3], double dt_s, int64_t now_us) noexcept {
        if (!(dt_s > 0)) return false;
        double vs[3], n = 0;
        for (int i = 0; i < 3; ++i) { vs[i] = (cam[i] - prev_cam[i]) / dt_s; n += vs[i] * vs[i]; }
        n = std::sqrt(n);
        if (n > kAnchorMaxSpeed) for (int i = 0; i < 3; ++i) vs[i] *= kAnchorMaxSpeed / n;
        const double tcv = double(now_us - latched_us) < kAnchorEntryMs * 1000.0 ? kAnchorVelTcEntryMs : kAnchorVelTcMs;
        const double av = 1.0 - std::exp(-dt_s * 1000.0 / tcv), ap = 1.0 - std::exp(-dt_s * 1000.0 / kAnchorTcMs);
        double e2 = 0;
        for (int i = 0; i < 3; ++i) {
            vel[i] += av * (vs[i] - vel[i]);
            pos[i] += vel[i] * dt_s;
            pos[i] += ap * (cam[i] - pos[i]);
            const double e = cam[i] - pos[i]; e2 += e * e;
        }
        if (std::sqrt(e2) > kAnchorRadius) { for (int i = 0; i < 3; ++i) pos[i] = cam[i]; latched_us = now_us; return true; }
        return false;
    }
};
// Critically damped spring toward target, exact for any dt (x = angle, v = rate; w = 1/tc). Angles wrapped: the short way round.
inline void spring_angle(double& x, double& v, double target, double dt_s, double tc_s) noexcept {
    if (!(tc_s > 0) || !(dt_s > 0)) return;
    const double w = 1.0 / tc_s;
    const double e0 = -wrap_pi(target - x);           // x - target, wrapped
    const double k = std::exp(-w * dt_s);
    const double c = v + w * e0;
    const double e1 = (e0 + c * dt_s) * k;
    const double v1 = (v - w * c * dt_s) * k;
    x = wrap_pi(target + e1);
    v = v1;
}
inline double smooth01(double e0, double e1, double x) noexcept {
    double t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}
class Rig {
public:
    RigOut step(int64_t now_ms, Route r, const viewmode::Geo& s, bool sample, const Options& o) noexcept {
        RigOut out;
        out.mode = effective(r, o.rig);
        if (o.rig == RigMode::off) { reset(); out.why = "off"; return out; }
        if (r != Route::chase && r != Route::cinematic) { reset(); out.why = "route-not-rigged"; return out; }
        if (!sample || !finite(s)) { reset(); out.why = "no-sample"; return out; }
        const int64_t dt_ms = have_ ? now_ms - last_ms_ : -1;
        bool enter = !have_ || dt_ms < 0 || dt_ms > kMaxStepMs || out.mode != mode_;
        bool cut = false;
        if (!enter) {
            const double dx = double(s.cam[0]) - last_cam_[0], dy = double(s.cam[1]) - last_cam_[1], dz = double(s.cam[2]) - last_cam_[2];
            const double move = std::sqrt(dx * dx + dy * dy + dz * dz);
            cut = move > kCutSpeed * double(dt_ms) + kCutSlack || std::fabs(wrap_pi(double(s.rot[1]) - last_rot_yaw_)) > kCutYaw;
        }
        const double dt_s = enter || cut ? 0.0 : double(dt_ms) / 1000.0;
        const double cam[3] = {s.cam[0], s.cam[1], s.cam[2]};
        if (enter || cut) {
            yaw_ = s.rot[1]; yaw_v_ = 0;
            anchor_.reset(cam, now_ms * 1000);
            blink_until_ = now_ms + kBlinkMs;
            vig_ = 1.0;
            ++(enter ? enters_ : cuts_);
            out.entered = enter; out.cut = cut;
        }
        const double prev_p = have_out_ && !enter ? out_p_ : 0, prev_y = have_out_ && !enter ? out_y_ : 0, prev_r = have_out_ && !enter ? out_r_ : 0;
        double p = 0, y = 0, rl = 0, pos[3] = {s.cam[0], s.cam[1], s.cam[2]};
        switch (out.mode) {
            case RigMode::game: p = s.rot[0]; y = s.rot[1]; rl = s.rot[2]; break;
            case RigMode::level:
                spring_angle(yaw_, yaw_v_, s.rot[1], dt_s, double(o.yaw_tc_ms) / 1000.0);
                y = yaw_;
                break;
            case RigMode::anchor: {
                if (anchor_.follow(cam, last_cam_, dt_s, now_ms * 1000)) {   // dt_s 0 (entry / cut): no step
                    yaw_ = s.rot[1]; yaw_v_ = 0; blink_until_ = now_ms + kBlinkMs; vig_ = 1.0; ++relatches_; out.relatched = true;
                } else spring_angle(yaw_, yaw_v_, s.rot[1], dt_s, kAnchorYawTcMs / 1000.0);
                for (int i = 0; i < 3; ++i) pos[i] = anchor_.pos[i];
                y = yaw_;
                break;
            }
            default: break;
        }
        out.valid = true; out.why = enter ? "entered" : cut ? "cut" : out.relatched ? "relatched" : "follow";
        for (int i = 0; i < 3; ++i) out.campos[i] = float(pos[i]);
        out.pitch = float(p); out.yaw = float(y); out.roll = float(rl);
        // rotation rate of the rig (the comfort measure) and the vignette
        double rate = 0;
        if (have_out_ && !enter && !out.cut && !out.relatched && dt_ms > 0) {
            const double a = wrap_pi(p - prev_p), b = wrap_pi(y - prev_y), c = wrap_pi(rl - prev_r);
            rate = std::sqrt(a * a + b * b + c * c) * 180.0 / kPi / (double(dt_ms) / 1000.0);
        }
        out.rate_dps = float(rate);
        const double target = double(o.vignette) * smooth01(kVigRateLo, kVigRateHi, rate);
        out.blink = now_ms < blink_until_;
        if (out.blink) vig_ = 1.0;
        else if (target >= vig_) vig_ = target;
        else vig_ = target + (vig_ - target) * (dt_ms > 0 ? std::exp(-double(dt_ms) / kVigReleaseMs) : 1.0);
        out.vignette = float(o.vignette <= 0.0f && now_ms >= blink_until_ ? 0.0 : vig_);
        // state
        have_ = true; last_ms_ = now_ms; mode_ = out.mode;
        for (int i = 0; i < 3; ++i) last_cam_[i] = s.cam[i];
        last_rot_yaw_ = s.rot[1];
        have_out_ = true; out_p_ = p; out_y_ = y; out_r_ = rl;
        return out;
    }
    uint64_t enters() const noexcept { return enters_; }
    uint64_t cuts() const noexcept { return cuts_; }
    uint64_t relatches() const noexcept { return relatches_; }
    static RigMode effective(Route r, RigMode m) noexcept {
        if (m == RigMode::off) return RigMode::off;
        if (r == Route::cinematic) return m == RigMode::game ? RigMode::game : RigMode::anchor;
        return m;
    }
private:
    static bool finite(const viewmode::Geo& s) noexcept {
        for (int i = 0; i < 3; ++i) if (!std::isfinite(s.cam[i]) || !std::isfinite(s.rot[i])) return false;
        return true;
    }
    void reset() noexcept { have_ = false; have_out_ = false; mode_ = RigMode::off; vig_ = 0; }
    bool have_ = false, have_out_ = false;
    RigMode mode_ = RigMode::off;
    int64_t last_ms_ = 0, blink_until_ = 0;
    double last_cam_[3]{}, last_rot_yaw_ = 0;
    AnchorCore anchor_{};
    double yaw_ = 0, yaw_v_ = 0, vig_ = 0;
    double out_p_ = 0, out_y_ = 0, out_r_ = 0;
    uint64_t enters_ = 0, cuts_ = 0, relatches_ = 0;
};

// ------------------------------------------------------------------------------------------------ ACM census request (consumer -> probe poll)
// The census asks the existing read-only ui3d ACM census (ui3d_census.h, normally armed by ui3d-census-acm.arm) to run once per newly seen view:
// the consumer increments the generation; the probe's 5 s present window (ee_camera_probe_win.cpp) sees the change, re-arms the census in ACM mode
// (a re-arm resets its 60-sample latch) and keeps it armed for kAcmWindows windows. The arm files keep working exactly as before.
inline std::atomic<uint32_t>& acm_request() noexcept { static std::atomic<uint32_t> g{0}; return g; }
inline constexpr unsigned kAcmWindows = 14;   // ~70 s: the census latches itself after 60 sampled frames (one per second)
struct AcmPoll {
    uint32_t last = 0;
    unsigned left = 0;
    uint64_t arms = 0;
    // Once per present window. true = keep the ACM census armed this window; rearm = disarm first (a new request: fresh latch).
    bool poll(uint32_t gen, bool& rearm) noexcept {
        rearm = false;
        if (gen != last) { last = gen; left = kAcmWindows; rearm = true; ++arms; }
        if (left) { --left; return true; }
        return false;
    }
};

// ------------------------------------------------------------------------------------------------ census + summary lines
inline constexpr unsigned kLine = 560;            // < 576 B diagnostics records
inline constexpr unsigned kCensusCap = 400;       // census lines per session
inline constexpr unsigned kVisitLines = 6;        // lines per visit at 1 Hz (the first one at the change)
inline constexpr int64_t kVisitPeriodMs = 1000, kVisitSlowMs = 10000;   // then one line per 10 s while the visit lasts
inline constexpr unsigned kAcmRequestsMax = 4;    // at most one ACM census per view kind (cockpit as the reference, hud-only, chase, other)
// Camera forward from the E6 rot (classify's convention) against the camera -> aircraft direction: the angle (deg) and that direction's pitch (deg).
inline bool fwd_to_air(const viewmode::Geo& s, double& ang_deg, double& pitch_deg) noexcept {
    const double d[3] = {double(s.air[0]) - s.cam[0], double(s.air[1]) - s.cam[1], double(s.air[2]) - s.cam[2]};
    const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(n > 1e-3) || !std::isfinite(n)) return false;
    const double p = s.rot[0], y = s.rot[1];
    const double f[3] = {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
    const double c = (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) / n;
    ang_deg = std::acos(c > 1 ? 1 : (c < -1 ? -1 : c)) * 180.0 / kPi;
    pitch_deg = std::asin(d[1] / n) * 180.0 / kPi;
    return true;
}
class Driver {
public:
    struct Step {
        RouteOut route{};
        RigOut rig{};
        bool acm_request = false;
        unsigned lines = 0;
        char text[2][kLine] = {};
    };
    Step step(int64_t now_ms, const viewmode::Tracker::Step& pv, const Options& o) noexcept {
        Step st;
        st.route = router_.step(now_ms, pv.mode, pv.geom, pv.sample, pv.s);
        st.rig = rig_.step(now_ms, st.route.r, pv.s, pv.sample, o);
        ++route_frames_[unsigned(st.route.r)];
        if (st.rig.valid) { ++rig_valid_; if (st.rig.rate_dps > rate_max_) rate_max_ = st.rig.rate_dps; if (st.rig.vignette > vig_max_) vig_max_ = st.rig.vignette;
                            if (st.rig.vignette > 0.05f) ++vig_frames_; }
        if (o.census) census(now_ms, pv, st, o);
        if (o.rig != RigMode::off && (last_summary_ms_ < 0 || now_ms - last_summary_ms_ >= 10000) && st.lines < 2) {
            last_summary_ms_ = now_ms;
            using ull = unsigned long long;
            std::snprintf(st.text[st.lines++], kLine, "xr-stereo;event=summary-pov-rig;rig=%s;route=%s;route-frames=cockpit:%llu,hud-only:%llu,chase:%llu,cinematic:%llu,"
                "flat:%llu;rig-valid=%llu;enters=%llu;cuts=%llu;relatches=%llu;rate-max-dps=%.1f;vig-max=%.2f;vig-frames=%llu;povv=%.2f;povtc=%d;"
                "render=unchanged(stage-1-dry-run)", rig_name(o.rig), route_name(st.route.r), ull(route_frames_[1]), ull(route_frames_[2]),
                ull(route_frames_[3]), ull(route_frames_[4]), ull(route_frames_[5]), ull(rig_valid_), ull(rig_.enters()), ull(rig_.cuts()),
                ull(rig_.relatches()), double(rate_max_), double(vig_max_), ull(vig_frames_), double(o.vignette), o.yaw_tc_ms);
        }
        return st;
    }
    unsigned census_lines() const noexcept { return census_lines_; }
    unsigned acm_requests() const noexcept { return acm_requests_; }
private:
    void census(int64_t now_ms, const viewmode::Tracker::Step& pv, Step& st, const Options&) noexcept {
        const bool change = pv.mode != visit_mode_ || !visit_on_;
        if (change) {
            visit_on_ = true; visit_mode_ = pv.mode; visit_start_ms_ = now_ms; visit_lines_ = 0; last_line_ms_ = -1;
            const unsigned k = unsigned(pv.mode);
            if (pv.mode != viewmode::Mode::unknown && k < 5 && !(seen_mask_ & (1u << k)) && acm_requests_ < kAcmRequestsMax) {
                seen_mask_ |= 1u << k; ++acm_requests_; st.acm_request = true;
            }
        }
        const int64_t period = visit_lines_ < kVisitLines ? kVisitPeriodMs : kVisitSlowMs;
        const bool due = change || last_line_ms_ < 0 || now_ms - last_line_ms_ >= period;
        if (!due || census_lines_ >= kCensusCap || st.lines >= 2) return;
        last_line_ms_ = now_ms; ++visit_lines_; ++census_lines_;
        const viewmode::Geo& s = pv.s;
        double ang = -1, tp = 0;
        const bool fa = pv.sample && fwd_to_air(s, ang, tp);
        const RigOut& r = st.rig;
        std::snprintf(st.text[st.lines++], kLine, "xr-stereo;event=pov-census;n=%u;mode=%s;visit-ms=%lld;route=%s;why=%s;sample=%u;geom=%s;look=%.4f,%.4f,%.4f;"
            "air=%.1f,%.1f,%.1f;hdg=%.5f;cam=%.1f,%.1f,%.1f;rot=%.5f,%.5f,%.5f;ahead=%.1f;up=%.1f;side=%.1f;dist=%.1f;fwd-to-air-deg=%.2f;"
            "air-pitch-deg=%.2f;rig=%s:%s;rig-cam=%.1f,%.1f,%.1f;rig-rot=%.4f,%.4f,%.4f;vig=%.2f;acm=%s",
            census_lines_, viewmode::mode_name(pv.mode), static_cast<long long>(now_ms - visit_start_ms_), route_name(st.route.r), st.route.why,
            pv.sample ? 1u : 0u, viewmode::geom_name(pv.geom.g), double(s.look[0]), double(s.look[1]), double(s.look[2]),
            double(s.air[0]), double(s.air[1]), double(s.air[2]), double(s.heading), double(s.cam[0]), double(s.cam[1]), double(s.cam[2]),
            double(s.rot[0]), double(s.rot[1]), double(s.rot[2]), double(pv.geom.ahead), double(pv.geom.up), double(pv.geom.side), double(pv.geom.dist),
            fa ? ang : -1.0, fa ? tp : 0.0, rig_name(r.mode), r.valid ? r.why : "invalid", double(r.campos[0]), double(r.campos[1]), double(r.campos[2]),
            double(r.pitch), double(r.yaw), double(r.roll), double(r.vignette),
            st.acm_request ? "requested" : "no");
    }
    Router router_;
    Rig rig_;
    uint64_t route_frames_[6]{}, rig_valid_ = 0, vig_frames_ = 0;
    float rate_max_ = 0, vig_max_ = 0;
    int64_t last_summary_ms_ = -1;
    bool visit_on_ = false;
    viewmode::Mode visit_mode_ = viewmode::Mode::unknown;
    int64_t visit_start_ms_ = 0, last_line_ms_ = -1;
    unsigned visit_lines_ = 0, census_lines_ = 0, acm_requests_ = 0, seen_mask_ = 0;
};

// ================================================================================================ STAGE 2 (pov:2; docs/world-render/other-povs.md section 10)
// The chase view in stereo from the game's own camera. The consumer asks (every XR frame, from its route) and the step-5 producer answers:
//   * while the route is `chase` the producer PUBLISHES the frame a scene DrawACM_v draw opened (until now abandoned because no DrawCPT draw
//     adopted it), tagged no-cockpit; its camera is the A matrix of the opener (the game's render camera), cross-checked against the raw E6 chase
//     sample; lens = the A lens; the world snapshot + HUD region are armed at the opener;
//   * the eyes ride on the level rig (yaw spring towards the camera heading, pitch/roll 0) or on the game camera (povrig:game): the producer passes
//     the rig as the setup_poses `lookR` rotation and publishes the rig axes as the frame's game-camera axes, so terrain (consumer) and objects
//     (producer) use one camera;
//   * the player's aircraft (A = T(p) x A_ref: the same rotation rows, its own translation row; POV1 census draw all=34, w = 84 = the chase
//     distance) is admitted by world::relative_offset / substitute_relative;
//   * a DrawCPT draw inside a no-cockpit frame (the user pressed V back to the cockpit) poisons that frame and holds no-cockpit opens for 1 s, so
//     ordinary cockpit frames resume; the classifier counts only cockpit composites (StereoFrame::no_cockpit), so published chase frames never flip
//     the mode back to cockpit.
inline constexpr uint32_t kReqChase = 1u, kReqLevel = 2u;   // kReqChase = "a no-cockpit frame is requested" (any kind); kReqLevel = the comfort rig
// POV-RENDER stage 3/4 (pov:3): the KIND of no-cockpit frame, bits 2..3 of the request (0 = chase, so every pov:2 request is unchanged).
//   chase     = stage 2 (level rig or povrig:game);
//   hud       = stage 3: the HUD-only view = the cockpit eye without the cockpit: the eyes ride on the game camera exactly (head 6DoF about the eye, as in
//               the cockpit; no rig, no vignette);
//   cinematic = stage 4: an in-engine camera (mission-end camera, scripted/event cameras): the anchor rig (kReqLevel) or povrig:game.
enum class NcKind : uint8_t { chase = 0, hud = 1, cinematic = 2 };
inline const char* nc_kind_name(NcKind k) noexcept { return k == NcKind::hud ? "hud-only" : k == NcKind::cinematic ? "cinematic" : "chase"; }
inline constexpr uint32_t kReqKindShift = 2u, kReqKindMask = 3u << kReqKindShift;
// consumer -> producer (relaxed: a stale value costs at most one frame of the previous behaviour)
inline std::atomic<uint32_t>& producer_request() noexcept { static std::atomic<uint32_t> g{0}; return g; }
inline uint32_t encode_request_kind(NcKind k, bool rig, int tc_ms) noexcept {
    const int tc = tc_ms < kYawTcMinMs ? kYawTcMinMs : (tc_ms > kYawTcMaxMs ? kYawTcMaxMs : tc_ms);
    return kReqChase | (rig ? kReqLevel : 0u) | (uint32_t(k) << kReqKindShift) | (uint32_t(tc) << 8);
}
inline uint32_t encode_request(bool chase, bool level, int tc_ms) noexcept {
    return chase ? encode_request_kind(NcKind::chase, level, tc_ms) : 0u;
}
inline bool req_chase(uint32_t r) noexcept { return (r & kReqChase) != 0; }   // any no-cockpit request (the stage-2 name)
inline bool req_level(uint32_t r) noexcept { return (r & kReqLevel) != 0; }
inline NcKind req_kind(uint32_t r) noexcept { const uint32_t k = (r & kReqKindMask) >> kReqKindShift; return k == 1u ? NcKind::hud : k == 2u ? NcKind::cinematic : NcKind::chase; }
inline int req_tc_ms(uint32_t r) noexcept { const int t = int(r >> 8); return t >= kYawTcMinMs && t <= kYawTcMaxMs ? t : kYawTcDefaultMs; }
// The request for this XR frame: stage 2 on and the route chase. povrig:game keeps the game camera; anything else (off, level, anchor) = level.
inline uint32_t request_for(bool stage2, Route r, const Options& o) noexcept {
    return stage2 && r == Route::chase ? encode_request(true, o.rig != RigMode::game, o.yaw_tc_ms) : 0u;
}
// pov:3: stage 2 + HUD-only (stage 3) + cinematic (stage 4). The cinematic rig is the anchor unless povrig:game.
inline uint32_t request_for_stage3(Route r, const Options& o) noexcept {
    switch (r) {
        case Route::chase: return encode_request_kind(NcKind::chase, o.rig != RigMode::game, o.yaw_tc_ms);
        case Route::hud_only: return encode_request_kind(NcKind::hud, false, o.yaw_tc_ms);
        case Route::cinematic: return encode_request_kind(NcKind::cinematic, o.rig != RigMode::game, o.yaw_tc_ms);
        default: return 0u;
    }
}
// The opener's A camera must be the camera the E6 block shows for the requested view (POV1 chase: |A eye - E6 cam| ~ one frame of motion). A first
// draw with a camera-relative A (its eye ~85 units from the model origin) is refused: the frame is then opened by a later absolute draw or not at all.
// runPOV2b (the post-crash REPLAY, 202,072,750..202,087,812): E6 still showed a chase geometry while the game rendered the replay cameras: 46,628
// openers refused here (`pov-camera-not-the-e6-chase-camera`), 31 frames opened while a replay angle happened to sit at the E6 camera. The check
// is what keeps a wrong camera out; a replay stays flat (section 11).
inline constexpr double kOpenerMaxDist = 400.0;
inline const char* opener_check(const double eye[3], const viewmode::Geo* e6, NcKind k = NcKind::chase) noexcept {
    if (!e6) return "pov-no-e6-sample";
    const viewmode::GeomOut g = viewmode::classify(*e6);
    switch (k) {
        case NcKind::chase: if (g.g != viewmode::Geom::chase) return "pov-e6-not-chase"; break;
        case NcKind::hud: if (g.g != viewmode::Geom::eye) return "pov-e6-not-eye"; break;
        case NcKind::cinematic:
            if (g.g != viewmode::Geom::other) return "pov-e6-not-other";
            if (!(g.dist <= kCinematicMaxDist)) return "pov-e6-camera-far-from-aircraft";
            break;
    }
    double d2 = 0;
    for (int i = 0; i < 3; ++i) { const double d = eye[i] - double(e6->cam[i]); d2 += d * d; }
    if (!(std::sqrt(d2) <= kOpenerMaxDist))
        return k == NcKind::chase ? "pov-camera-not-the-e6-chase-camera" : k == NcKind::hud ? "pov-camera-not-the-e6-eye" : "pov-camera-not-the-e6-cinematic-camera";
    return nullptr;
}
// Camera axes in the step-5 convention (gpu_copy_step5.h decompose: right, screen up, view forward; right x up = -forward; world +Y up).
inline bool heading_of(const double fwd[3], double& yaw) noexcept {
    const double h = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (!(h > 1e-6) || !std::isfinite(h)) return false;
    yaw = std::atan2(-fwd[0], -fwd[2]);   // forward = (-sin yaw, 0, -cos yaw), as terrain camera_frame
    return true;
}
inline void level_axes(double yaw, double r[3], double u[3], double f[3]) noexcept {
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    f[0] = -sy; f[1] = 0; f[2] = -cy;
    r[0] = cy;  r[1] = 0; r[2] = -sy;
    u[0] = 0;   u[1] = 1; u[2] = 0;
}
// setup_poses' lookR (row-major; its unlook computes v_camera[k] = sum_i lookR[i*3+k] v_rig[i]): lookR[i*3+k] = dot(G_i, C_k) with G = rig
// (right, up, -fwd) and C = camera (right, up, -fwd): the head pose is applied in the RIG frame while the eye matrices are built around the camera.
inline void rig_look(const double cr[3], const double cu[3], const double cf[3], const double rr[3], const double ru[3], const double rf[3], double out[9]) noexcept {
    const double* C[3] = {cr, cu, cf};
    const double* G[3] = {rr, ru, rf};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) {
            const double sg = i == 2 ? -1.0 : 1.0, sc = k == 2 ? -1.0 : 1.0;
            out[i * 3 + k] = sg * sc * (G[i][0] * C[k][0] + G[i][1] * C[k][1] + G[i][2] * C[k][2]);
        }
}
// The producer's level rig (game thread, step-1 mutex): yaw through the critically damped spring towards the camera heading, once per opened
// no-cockpit frame. A gap > 250 ms or a heading step > 45 degrees (a camera cut) restarts it at the heading.
class ProducerRig {
public:
    bool step(const double fwd[3], int64_t now_us, int tc_ms, double& yaw_out) noexcept {
        double h = 0;
        if (!heading_of(fwd, h)) return false;
        const int64_t dt = have_ ? now_us - last_us_ : -1;
        if (!have_ || dt < 0 || dt > kMaxStepMs * 1000 || std::fabs(wrap_pi(h - last_h_)) > kCutYaw) { yaw_ = h; v_ = 0; ++enters_; }
        else spring_angle(yaw_, v_, h, double(dt) / 1e6, double(tc_ms) / 1000.0);
        have_ = true; last_us_ = now_us; last_h_ = h;
        yaw_out = yaw_;
        return true;
    }
    uint64_t enters() const noexcept { return enters_; }
private:
    bool have_ = false;
    int64_t last_us_ = 0;
    double yaw_ = 0, v_ = 0, last_h_ = 0;
    uint64_t enters_ = 0;
};
// The no-cockpit open decision for one early world draw (pure): nullptr = open a no-cockpit frame; else why not.
inline constexpr int64_t kCockpitHoldUs = 1000000;   // a DrawCPT draw in a no-cockpit frame: 1 s of ordinary cockpit frames first
inline const char* no_cockpit_why(uint32_t request, int64_t now_us, int64_t cockpit_hold_until_us) noexcept {
    if (!req_chase(request)) return "pov-not-requested";
    if (now_us < cockpit_hold_until_us) return "pov-cockpit-seen-hold";
    return nullptr;
}
// The classifier's cockpit evidence (consumer): only a fresh frame WITH cockpit members is a cockpit composite.
inline bool cockpit_composite(bool fresh, bool no_cockpit) noexcept { return fresh && !no_cockpit; }

// ================================================================================================ STAGES 3 + 4 (pov:3; other-povs.md section 11)
// The producer's anchor rig for cinematic frames (game thread, step-1 mutex; once per opened cinematic frame): the eyes stand at the anchor
// (AnchorCore: follows the A camera with its low-passed velocity + a 2 s pull, re-latched beyond 400 units) and look along a LEVEL yaw that
// springs (1 s) toward the camera heading. Entry (first frame, a gap > 250 ms) and camera cuts (a move > 20 u/ms x dt + 200 or a heading step
// > 45 degrees) latch at the camera. `event` = entered | cut | relatched | follow.
class ProducerAnchor {
public:
    bool step(const double eye[3], const double fwd[3], int64_t now_us, double pos[3], double& yaw_out, const char*& event) noexcept {
        double h = 0;
        event = "none";
        if (!heading_of(fwd, h)) return false;
        const int64_t dt = have_ ? now_us - last_us_ : -1;
        bool enter = !have_ || dt < 0 || dt > kMaxStepMs * 1000, cut = false;
        if (!enter) {
            double m2 = 0;
            for (int i = 0; i < 3; ++i) { const double d = eye[i] - last_eye_[i]; m2 += d * d; }
            cut = std::sqrt(m2) > kCutSpeed * double(dt) / 1000.0 + kCutSlack || std::fabs(wrap_pi(h - last_h_)) > kCutYaw;
        }
        if (enter || cut) {
            core_.reset(eye, now_us); yaw_ = h; v_ = 0;
            ++(enter ? enters_ : cuts_); event = enter ? "entered" : "cut";
        } else if (core_.follow(eye, last_eye_, double(dt) / 1e6, now_us)) {
            yaw_ = h; v_ = 0; ++relatches_; event = "relatched";
        } else { spring_angle(yaw_, v_, h, double(dt) / 1e6, kAnchorYawTcMs / 1000.0); event = "follow"; }
        have_ = true; last_us_ = now_us; last_h_ = h;
        for (int i = 0; i < 3; ++i) { last_eye_[i] = eye[i]; pos[i] = core_.pos[i]; }
        yaw_out = yaw_;
        return true;
    }
    void reset() noexcept { have_ = false; }
    uint64_t enters() const noexcept { return enters_; }
    uint64_t cuts() const noexcept { return cuts_; }
    uint64_t relatches() const noexcept { return relatches_; }
private:
    AnchorCore core_{};
    bool have_ = false;
    int64_t last_us_ = 0;
    double last_eye_[3]{}, last_h_ = 0, yaw_ = 0, v_ = 0;
    uint64_t enters_ = 0, cuts_ = 0, relatches_ = 0;
};

// Consumer gate (pov:3, every XR frame): a no-cockpit view whose frames do not come (the opener check refuses the game's camera: a replay, a camera
// the E6 block does not show) falls back to flat for 1 s instead of flickering between the vscreen and sporadic stereo frames. The request is on
// while the route asks for it; after kGateFirstMs without any no-cockpit frame, or a gap > kGateGapMs once they came, it is withdrawn for
// kGateHoldMs (the vscreen covers it; vsenter = 500 ms), then tried again. A route change re-arms it at once.
//
// A-ONLY escalation (2026-10-31, runPOV3 / step151). In-engine cutscenes and the post-crash REPLAY are cameras the E6 block does NOT show: E6 kept
// the frozen eye during the in-mission event cutscene ("INDIGO ... Target spotted", hud-only 295.6..307.8 s: 17,544 openers refused
// `pov-camera-not-the-e6-eye`) and a chase geometry during the replay (458.8..576.1 s: 140k refused `pov-camera-not-the-e6-chase-camera`, 21 frames
// opened when a replay angle sat near the chase camera); the gate then cycled 107 times (flat 1 s, retry 0.6 s, ...). When a fallback happens while
// the producer counted >= kEscalateMismatch "A is not the E6 camera" refusals during that attempt, the view is a game camera E6 cannot follow: the
// gate ESCALATES the visit to an A-ONLY cinematic request (kind cinematic + kReqAOnly; the anchor rig unless povrig:game) and retries at once. The
// producer then renders around the game's own render camera A (the absolute scene camera of the frame, D-291: a real render camera, never a stick or
// HMD mapping) with the opener check `opener_check_a_only`. The escalation lasts until the route changes (a cockpit frame, another view, flat).
inline constexpr int64_t kGateFirstMs = 600, kGateGapMs = 300, kGateHoldMs = 1000;
inline constexpr uint32_t kReqAOnly = 1u << 4;
inline constexpr int64_t kEscalateWindowMs = 3000;
inline constexpr uint64_t kEscalateMismatch = 30;     // refusals in one attempt (one game frame refuses ~3-10 openers; 600 ms ~ 36 frames)
inline bool req_a_only(uint32_t r) noexcept { return (r & kReqAOnly) != 0; }
// producer -> consumer (relaxed counters)
inline std::atomic<uint64_t>& producer_e6_mismatch() noexcept { static std::atomic<uint64_t> g{0}; return g; }    // openers refused: A is not the E6 camera
inline std::atomic<uint64_t>& producer_anchor_events() noexcept { static std::atomic<uint64_t> g{0}; return g; }  // anchor enters + cuts + re-latches
inline bool is_e6_mismatch(const char* why) noexcept { return why && std::strncmp(why, "pov-camera-not-the-e6", 21) == 0; }
inline uint32_t a_only_request(uint32_t want, const Options& o) noexcept {
    return encode_request_kind(NcKind::cinematic, o.rig != RigMode::game, req_tc_ms(want)) | kReqAOnly;
}
// The A-only opener: the frame's first scene-target camera is accepted alone, but never a camera-relative draw (A = T(p) x A_ref decomposes to an
// "eye" = camera - model ~ tens..hundreds of units from the WORLD origin: POV1 all=34 -> (55, 19.6, 62.8)) and never a camera far from the aircraft
// (the E6 aircraft position). The E6 sample must be valid (a zero block = menus / FMV: refused).
// runPOV3b (step153): the 60,000-unit bound refused EVERY opener of the INDIGO event cutscene (300.7..313.0 s, 20,953 refusals
// `pov-a-only-camera-far-from-aircraft`): the cutscene films the enemy flight far from the player. The relative-draw rule (|eye| >= 3,000) is what
// keeps a wrong camera out; the aircraft bound is only a sanity limit now (500,000 units = 100 km, beyond any mission map).
inline constexpr double kAOnlyMinEyeNorm = 3000.0, kAOnlyMaxFromAircraft = 500000.0;
inline const char* opener_check_a_only(const double eye[3], const viewmode::Geo* e6) noexcept {
    if (!e6) return "pov-no-e6-sample";
    if (viewmode::classify(*e6).g == viewmode::Geom::invalid) return "pov-a-only-e6-invalid";
    double n2 = 0, d2 = 0;
    for (int i = 0; i < 3; ++i) { n2 += eye[i] * eye[i]; const double d = eye[i] - double(e6->air[i]); d2 += d * d; }
    if (!(std::sqrt(n2) >= kAOnlyMinEyeNorm)) return "pov-a-only-camera-relative";
    if (!(std::sqrt(d2) <= kAOnlyMaxFromAircraft)) return "pov-a-only-camera-far-from-aircraft";
    return nullptr;
}
class NcGate {
public:
    // want: the route's request (0 = none); last_nc_ms: consumer time of the newest no-cockpit frame composed (-1 = none); mismatch: the producer's
    // producer_e6_mismatch() count now. Returns the request to send (escalated: an A-only cinematic request).
    uint32_t step(int64_t now_ms, uint32_t want, int64_t last_nc_ms, uint64_t mismatch = 0, const Options& o = Options{}) noexcept {
        const uint32_t kind = want & (kReqChase | kReqKindMask);
        if (!want) { on_ = false; held_kind_ = 0; escalated_ = false; return 0u; }
        if (kind != held_kind_) { held_kind_ = kind; on_ = false; hold_until_ = 0; escalated_ = false; }   // another view: no hold, no escalation carried over
        if (now_ms < hold_until_) return 0u;
        if (!on_) { on_ = true; since_ms_ = now_ms; mismatch0_ = mismatch; }
        const bool got = last_nc_ms >= since_ms_;
        if ((!got && now_ms - since_ms_ > kGateFirstMs) || (got && now_ms - last_nc_ms > kGateGapMs)) {
            on_ = false; ++fallbacks_; last_fallback_got_ = got;
            // a camera E6 does not show: retry at once around A. Only a SHORT attempt (no frame, or a few sporadic ones within kEscalateWindowMs:
            // the replay) escalates; a long-running view that hiccups (a loading stall) falls back as before.
            if (!escalated_ && mismatch - mismatch0_ >= kEscalateMismatch && now_ms - since_ms_ <= kEscalateWindowMs) {
                escalated_ = true; ++escalations_; on_ = true; since_ms_ = now_ms; mismatch0_ = mismatch;
                return a_only_request(want, o);
            }
            hold_until_ = now_ms + kGateHoldMs;
            return 0u;
        }
        return escalated_ ? a_only_request(want, o) : want;
    }
    uint64_t fallbacks() const noexcept { return fallbacks_; }
    uint64_t escalations() const noexcept { return escalations_; }
    bool escalated() const noexcept { return escalated_; }
    bool last_fallback_after_frames() const noexcept { return last_fallback_got_; }
private:
    bool on_ = false, last_fallback_got_ = false, escalated_ = false;
    uint32_t held_kind_ = 0;
    int64_t since_ms_ = 0, hold_until_ = 0;
    uint64_t fallbacks_ = 0, escalations_ = 0, mismatch0_ = 0;
};
// The blink of an A-only visit: the consumer rig follows E6 (which is not the camera there), so the blink comes from the PRODUCER anchor's events
// (enter / cut / re-latch): kBlinkMs after each change of producer_anchor_events().
class EventBlink {
public:
    bool step(int64_t now_ms, uint64_t events) noexcept {
        if (!have_) { have_ = true; last_ = events; }
        if (events != last_) { last_ = events; until_ = now_ms + kBlinkMs; }
        return now_ms < until_;
    }
private:
    bool have_ = false;
    uint64_t last_ = 0;
    int64_t until_ = 0;
};
// In-DLL eye dump of a cinematic (2026-10-31: runPOV3's two cinematic windows lasted 0.6 s and 1.5 s; the runner's file trigger came too late and
// png-eye.now then dumped the next fresh frame = a chase frame 15 s later). The consumer re-arms xr_png_once itself on the kCineDumpFrame-th
// no-cockpit frame of each cinematic / A-only request visit, at most kCineDumpMax per session (needs xr_png_once=true; nothing else changes).
// runPOV3b: the dump f24400 was armed on the 4th frame of an A-only visit, inside the producer anchor's entry blink -> a fully black image. Only
// frames WITHOUT the blink count (and the capture itself is skipped on blink frames by the consumer), so a dump never shows a blink.
inline constexpr unsigned kCineDumpFrame = 4, kCineDumpMax = 4;
class CineDump {
public:
    // req: the request sent this XR frame; fresh_nc: a fresh no-cockpit frame is being composed. true = arm one eye dump now.
    bool step(uint32_t req, bool fresh_nc, bool blink = false) noexcept {
        const bool cine = req_chase(req) && req_kind(req) == NcKind::cinematic;
        const uint32_t key = cine ? (req & (kReqKindMask | kReqAOnly | kReqChase)) : 0u;
        if (key != key_) { key_ = key; frames_ = 0; }
        if (!cine || !fresh_nc || blink) return false;
        if (++frames_ != kCineDumpFrame || dumps_ >= kCineDumpMax) return false;
        ++dumps_;
        return true;
    }
    unsigned dumps() const noexcept { return dumps_; }
private:
    uint32_t key_ = 0;
    unsigned frames_ = 0, dumps_ = 0;
};

// The comfort vignette, DRAWN (stage 2 only computed it). Packed into the eye targets' mode_bits of the composite shader (xr_stereo_blit.hlsl
// ps_main; bits 20..31 are unused on the eye targets: bits 19..23 / 24..31 carry the radar background / alpha floor on the HUD target only, bit 19
// = sprites on the eyes): bits 24..31 = strength q (s = q / 255), bit 20 = blink. CPU mirror of the shader:
//   blink: the eye image is black (rgb x 0, alpha kept: a 150 ms "blink" over an entry / cut / re-latch teleport);
//   else rr = |2 uv - 1| (0 at the eye-image centre, 1 at the edge middles, 1.414 at the corners), inner = 1.45 - s, gain = 1 - smoothstep(inner,
//   inner + 0.35, rr): s = 0 changes nothing (inner 1.45 lies beyond the corners); s = 0.6 (povv default at full rate) darkens from 85 % of the half
//   width to black at 120 %; s = 1 keeps a clear disc of radius 0.45. Premultiplied output: rgb x gain, alpha unchanged.
// Head-locked (eye-image space), applied to the composite pass output (terrain, world, objects, effects, props); the tactical markers / NBOX stamps
// drawn after the composite are not darkened (section 11 limits).
inline constexpr uint32_t kVigBlinkBit = 1u << 20, kVigShift = 24u;
inline constexpr double kVigInnerOff = 1.45, kVigWidth = 0.35;
inline uint32_t vignette_mode_bits(float strength, bool blink) noexcept {
    if (blink) return kVigBlinkBit;
    const float s = std::isfinite(strength) ? (strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength) : 0.0f;
    const uint32_t q = uint32_t(s * 255.0f + 0.5f);
    return q << kVigShift;
}
inline float vignette_gain(uint32_t bits, float u, float v) noexcept {
    if (bits & kVigBlinkBit) return 0.0f;
    const uint32_t q = (bits >> kVigShift) & 255u;
    if (!q) return 1.0f;
    const double s = double(q) / 255.0;
    const double x = 2.0 * u - 1.0, y = 2.0 * v - 1.0, rr = std::sqrt(x * x + y * y);
    const double inner = kVigInnerOff - s;
    return float(1.0 - smooth01(inner, inner + kVigWidth, rr));
}
// Which XR frames get it: pov:3, a rigged route (chase / cinematic: the HUD-only view is the cockpit eye, unvignetted like the cockpit), a valid
// rig, and the composed frame is a no-cockpit frame (a cockpit frame still shown during the switch is never darkened).
inline uint32_t vignette_for(bool stage3, Route r, const RigOut& rig, bool frame_no_cockpit) noexcept {
    if (!stage3 || !frame_no_cockpit || !rig.valid || (r != Route::chase && r != Route::cinematic)) return 0u;
    return vignette_mode_bits(rig.vignette, rig.blink);
}

} // namespace acz::povr
