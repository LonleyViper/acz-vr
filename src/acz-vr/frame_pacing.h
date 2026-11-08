#pragma once
// frame_pacing.h -- UNCAP / EXTRAP (xr_terrain_stage options uncap:1 and extrap:1, both default OFF; docs/world-render/uncap.md).
// Header only, SDK-free (no Windows/DXGI/OpenXR types), so the option parser, the Present override decision, the Present statistics, the
// simulation-rate guard and the motion extrapolator are tested offline (tests/acz-vr/frame_pacing_offline_tests.cpp) and used unchanged by
//  * ee_camera_probe_win.cpp (the existing Present/Present1 vtable hook: override + statistics + guard; no new hook, no game code touched),
//  * xr_stereo_proxy.cpp (parses the options, arms uncap while an XR session is running),
//  * xr_stereo_output.cpp (extrapolates the terrain camera and the TACTICAL markers to the XR frame's display time).
//
// Evidence (uncap.md section 1): the game's simulation advances a FIXED step per rendered frame (E6 aircraft displacement per frame is
// independent of the frame's wall duration, 6,717 frames, 23 runs) and its 60.000 fps pace comes from its own QPC/Sleep limiter
// (Sleep + timeBeginPeriod + QueryPerformanceCounter imports; 16.6668 ms long-run period on a 119.9 Hz desktop). Removing the swap-chain
// sync interval can therefore NOT make the game faster than its limiter, and anything that did would speed the game up (fixed step).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace acz::pacing {

// ------------------------------------------------------------------ options (inside xr_terrain_stage; removed before the terrain parser)
struct Options {
    bool uncap = false;          // uncap:1  Present sync interval 0 (+ ALLOW_TEARING when the game's swap chain supports it) while XR runs
    bool extrap = false;         // extrap:1 dead-reckon the terrain camera and TACTICAL markers to the XR frame's predicted display time
    float extrap_max_ms = 33.0f; // extrapms:<5..100> horizon clamp (ms)
    bool extrap_attitude = true; // extrapatt:0 translation only (no attitude extrapolation)
    bool interp = false;         // interp:1 OBJECT-INTERP (object_interp.h, docs/world-render/interp.md): replayed objects + camera to t_display
    float interp_max_frames = 1.5f; // interpx:<0..3> object extrapolation clamp (game frames ahead of the newest state)
    // DECOUPLE (docs/world-render/decouple.md): decouple:1 composes terrain/sky/cockpit/objects/effects on EVERY XR frame with that frame's own head
    // pose (the newest game images re-projected), instead of only when a new game frame arrives. Default off = exactly the previous consumer.
    bool decouple = false;               // decouple:1
    float decouple_max_age_ms = 250.0f;  // dcage:<20..500> a held game frame is re-composed while younger than this; older: the old re-submit path
    float decouple_horizon_ms = 70.0f;   // dcms:<5..150> terrain camera dead-reckoning clamp for the per-XR-frame camera (display-time camera)
    float decouple_fade = 0.02f;         // dcfade:<0..0.2> edge fade width (source uv) of the re-projected objects image
    bool any() const noexcept { return uncap || extrap || interp || decouple; }
};
namespace detail {
inline bool num(const std::string& v, float& f) {
    if (v.empty() || v.size() > 16) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(d)) return false;
    f = float(d);
    return true;
}
} // namespace detail
// Removes uncap/extrap/extrapms/extrapatt/interp/interpx from a ';'-separated option value (the first item, the stage id, is never ours). A value without
// them comes back unchanged. false (with why) only for a bad value of one of OUR keys.
inline bool extract_options(const std::string& value, std::string& rest, Options& opt, std::string& why) {
    opt = Options{};
    rest.clear();
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
                bool ok = true; float f = 0;
                auto flag = [&](bool& b) { if (v == "1") b = true; else if (v == "0") b = false; else return false; return true; };
                if (k == "uncap") { ours = true; ok = flag(opt.uncap); }
                else if (k == "extrap") { ours = true; ok = flag(opt.extrap); }
                else if (k == "extrapatt") { ours = true; ok = flag(opt.extrap_attitude); }
                else if (k == "extrapms") { ours = true; ok = detail::num(v, f) && f >= 5.0f && f <= 100.0f; if (ok) opt.extrap_max_ms = f; }
                else if (k == "interp") { ours = true; ok = flag(opt.interp); }
                else if (k == "interpx") { ours = true; ok = detail::num(v, f) && f >= 0.0f && f <= 3.0f; if (ok) opt.interp_max_frames = f; }
                else if (k == "decouple") { ours = true; ok = flag(opt.decouple); }
                else if (k == "dcage") { ours = true; ok = detail::num(v, f) && f >= 20.0f && f <= 500.0f; if (ok) opt.decouple_max_age_ms = f; }
                else if (k == "dcms") { ours = true; ok = detail::num(v, f) && f >= 5.0f && f <= 150.0f; if (ok) opt.decouple_horizon_ms = f; }
                else if (k == "dcfade") { ours = true; ok = detail::num(v, f) && f >= 0.0f && f <= 0.2f; if (ok) opt.decouple_fade = f; }
                if (ours && !ok) { why = "bad value for " + k; return false; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}
inline std::string describe(const Options& o) {
    char t[300];
    int n = std::snprintf(t, sizeof t, "uncap=%d;extrap=%d;extrap-max-ms=%.0f;extrap-attitude=%d;interp=%d;interp-max-frames=%.2f", o.uncap ? 1 : 0, o.extrap ? 1 : 0,
                  double(o.extrap_max_ms), o.extrap_attitude ? 1 : 0, o.interp ? 1 : 0, double(o.interp_max_frames));
    if (o.decouple && n > 0 && size_t(n) < sizeof t)   // DECOUPLE: appended only when on (the default line is unchanged)
        std::snprintf(t + n, sizeof t - size_t(n), ";decouple=1;dc-age-ms=%.0f;dc-horizon-ms=%.0f;dc-fade=%.3f", double(o.decouple_max_age_ms),
                      double(o.decouple_horizon_ms), double(o.decouple_fade));
    return t;
}

// ------------------------------------------------------------------ process-wide uncap state (proxy writes, Present hook reads)
enum : uint32_t { kUncapRequested = 1u, kXrActive = 2u };
inline std::atomic<uint32_t>& state_slot() noexcept { static std::atomic<uint32_t> s{0}; return s; }
inline void set_uncap_requested(bool on) noexcept { if (on) state_slot().fetch_or(kUncapRequested); else state_slot().fetch_and(~kUncapRequested); }
inline void set_xr_active(bool on) noexcept { if (on) state_slot().fetch_or(kXrActive); else state_slot().fetch_and(~kXrActive); }
// The override applies only while BOTH hold: the option asked for it and an XR session has submitted its first frame (and not ended).
inline bool uncap_active() noexcept { const uint32_t s = state_slot().load(std::memory_order_acquire); return (s & kUncapRequested) && (s & kXrActive); }

// ------------------------------------------------------------------ Present override decision (pure)
constexpr uint32_t kPresentTest = 0x1u;           // DXGI_PRESENT_TEST
constexpr uint32_t kPresentAllowTearing = 0x200u; // DXGI_PRESENT_ALLOW_TEARING
struct PresentDecision { uint32_t sync = 0, flags = 0; bool changed = false, tearing_added = false; };
// active: uncap_active(); tearing_ok: the swap chain was CREATED with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, the factory reports tearing support and
// the swap chain is windowed (ALLOW_TEARING is invalid otherwise; it is never added to a swap chain created without the flag: that would fail).
// A TEST present (no display) is never touched.
inline PresentDecision present_override(bool active, uint32_t sync, uint32_t flags, bool tearing_ok) noexcept {
    PresentDecision d; d.sync = sync; d.flags = flags;
    if (!active || (flags & kPresentTest)) return d;
    d.sync = 0;
    if (tearing_ok && !(flags & kPresentAllowTearing)) { d.flags |= kPresentAllowTearing; d.tearing_added = true; }
    d.changed = d.sync != sync || d.flags != flags;
    return d;
}

// ------------------------------------------------------------------ Present statistics (one thread: the game's Present caller)
struct PresentStats {
    uint64_t window_start_us = 0, last_us = 0;
    uint32_t presents = 0, sync_count[4] = {0, 0, 0, 0};   // sync 0, 1, 2, >= 3 as the GAME passed it
    uint32_t flags_or = 0, overridden = 0, tearing_added = 0;
    uint64_t max_interval_us = 0;
    uint64_t total_presents = 0;
    void note(uint64_t now_us, uint32_t game_sync, uint32_t game_flags, const PresentDecision& d) noexcept {
        if (!window_start_us) window_start_us = now_us;
        if (last_us && now_us > last_us) max_interval_us = std::max<uint64_t>(max_interval_us, now_us - last_us);
        last_us = now_us;
        ++presents; ++total_presents;
        ++sync_count[game_sync >= 3 ? 3 : game_sync];
        flags_or |= game_flags;
        if (d.changed) ++overridden;
        if (d.tearing_added) ++tearing_added;
    }
    bool due(uint64_t now_us, uint64_t window_us = 5000000) const noexcept { return window_start_us && now_us >= window_start_us + window_us; }
    double fps(uint64_t now_us) const noexcept { return now_us > window_start_us ? double(presents) * 1e6 / double(now_us - window_start_us) : 0.0; }
    void reset(uint64_t now_us) noexcept {
        window_start_us = now_us; presents = 0; for (uint32_t& c : sync_count) c = 0; flags_or = 0; overridden = 0; tearing_added = 0; max_interval_us = 0;
    }
};

// ------------------------------------------------------------------ simulation-rate guard
// Fed once per Present with the wall time and the E6 camera position. Per window it reports the frame rate, the camera's movement per frame and
// per wall second, and a live FIXED-STEP test: frames that took much longer (or shorter) than the window median should move proportionally
// more (less) if the game integrates wall delta time; with a fixed step per frame every frame moves the same distance. slope = (relative
// displacement long/short - 1) / (duration long/short - 1): ~0 fixed step, ~1 delta time. With a fixed step the game's speed is fps / 60.
struct SimRateReport {
    double fps = 0, units_per_frame = 0, units_per_s = 0, slope = 0, game_speed = 0;
    uint32_t frames = 0, moving = 0, short_n = 0, long_n = 0;
    const char* model = "unknown";   // fixed-step | delta-time | unclear | unknown (not enough moving frames with varied durations)
    bool warn = false;               // fps above the 60 Hz design rate while the sim looks fixed-step (or unclear): the game runs fast
};
class SimRateGuard {
public:
    static constexpr double kDesignFps = 60.0;
    void feed(uint64_t now_us, const float pos[3], bool pos_valid) noexcept {
        if (!start_us_) start_us_ = now_us;
        ++frames_;
        if (have_prev_ && pos_valid && now_us > prev_us_) {
            const double dt = double(now_us - prev_us_) * 1e-6;
            const double d = std::sqrt(sq(pos[0] - prev_[0]) + sq(pos[1] - prev_[1]) + sq(pos[2] - prev_[2]));
            if (d > 0.5 && d < 2000.0 && dt < 0.25) {
                ++moving_; sum_d_ += d; sum_dt_ += dt;
                if (ema_d_ > 0 && ema_dt_ > 0) {
                    const double rel = d / ema_d_, rdt = dt / ema_dt_;
                    if (rdt < 0.7) { ++sn_; s_rel_ += rel; s_dt_ += rdt; }
                    else if (rdt > 1.4) { ++ln_; l_rel_ += rel; l_dt_ += rdt; }
                }
                ema_d_ = ema_d_ > 0 ? 0.8 * ema_d_ + 0.2 * d : d;
                ema_dt_ = ema_dt_ > 0 ? 0.9 * ema_dt_ + 0.1 * dt : dt;
            }
        }
        if (pos_valid) { prev_[0] = pos[0]; prev_[1] = pos[1]; prev_[2] = pos[2]; prev_us_ = now_us; have_prev_ = true; }
        else have_prev_ = false;
    }
    SimRateReport report(uint64_t now_us) const noexcept {
        SimRateReport r;
        r.frames = frames_; r.moving = moving_; r.short_n = sn_; r.long_n = ln_;
        r.fps = now_us > start_us_ && start_us_ ? double(frames_) * 1e6 / double(now_us - start_us_) : 0.0;
        if (moving_) { r.units_per_frame = sum_d_ / moving_; r.units_per_s = sum_dt_ > 0 ? sum_d_ / sum_dt_ : 0; }
        if (sn_ >= 4 && ln_ >= 4) {
            const double rel_ratio = (l_rel_ / ln_) / std::max(1e-9, s_rel_ / sn_);
            const double dt_ratio = (l_dt_ / ln_) / std::max(1e-9, s_dt_ / sn_);
            r.slope = dt_ratio > 1.05 ? (rel_ratio - 1.0) / (dt_ratio - 1.0) : 0.0;
            r.model = r.slope < 0.3 ? "fixed-step" : r.slope > 0.7 ? "delta-time" : "unclear";
        }
        const bool fixed_or_unknown = std::string(r.model) != "delta-time";
        r.game_speed = fixed_or_unknown ? r.fps / kDesignFps : 1.0;
        r.warn = fixed_or_unknown && r.fps > 63.0;
        return r;
    }
    void reset(uint64_t now_us) noexcept {
        const bool hp = have_prev_; const uint64_t pu = prev_us_; float p[3] = {prev_[0], prev_[1], prev_[2]};
        const double ed = ema_d_, et = ema_dt_;
        *this = SimRateGuard{};
        start_us_ = now_us; have_prev_ = hp; prev_us_ = pu; prev_[0] = p[0]; prev_[1] = p[1]; prev_[2] = p[2]; ema_d_ = ed; ema_dt_ = et;
    }
private:
    static double sq(double x) noexcept { return x * x; }
    uint64_t start_us_ = 0, prev_us_ = 0;
    float prev_[3] = {0, 0, 0};
    bool have_prev_ = false;
    uint32_t frames_ = 0, moving_ = 0, sn_ = 0, ln_ = 0;
    double sum_d_ = 0, sum_dt_ = 0, ema_d_ = 0, ema_dt_ = 0, s_rel_ = 0, s_dt_ = 0, l_rel_ = 0, l_dt_ = 0;
};

// ------------------------------------------------------------------ motion extrapolation (dead reckoning, clamped)
// A camera frame (the terrain CamFrame layout: eye + right/up/fwd with right x up = -fwd).
struct Frame { float eye[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1}; };
struct ExtrapConfig {
    float max_ms = 33.0f;          // horizon clamp
    float max_step = 400.0f;       // units per game frame; a larger jump is a cut/respawn: no extrapolation until two normal frames
    float max_rot_frame_deg = 10.0f; // rotation per game frame above this: a camera cut
    float max_rot_deg = 6.0f;      // extrapolated rotation clamp
    float cos_min = 0.7f;          // translation direction change between the last two frame steps above ~45 deg: not extrapolated (no overshoot)
    float ratio_min = 0.5f, ratio_max = 2.0f;   // step length change outside this: not extrapolated (braking, stop, launch)
    bool attitude = true;
};
struct ExtrapResult { double frames_ahead = 0, horizon_ms = 0; bool translated = false, rotated = false, clamped = false; const char* why = "ok"; };
class MotionExtrap {
public:
    explicit MotionExtrap(const ExtrapConfig& c = ExtrapConfig{}) : c_(c) {}
    void configure(const ExtrapConfig& c) noexcept { c_ = c; }
    // One game frame (frame ids strictly increase; game frames, not XR frames). t_us: the frame's publication time (steady clock).
    void update(uint64_t id, int64_t t_us, const Frame& f) noexcept {
        if (have_ && id <= id_) return;   // repeated/older frame: nothing new
        if (!anchor_set_) { anchor_id_ = id; anchor_us_ = t_us; anchor_set_ = true; }
        else if (t_us - anchor_us_ >= 250000 && id > anchor_id_) {
            const double inst = double(id - anchor_id_) * 1e6 / double(t_us - anchor_us_);
            fps_ = fps_ > 0 ? 0.5 * fps_ + 0.5 * inst : inst;
            anchor_id_ = id; anchor_us_ = t_us;
        }
        if (have_) {
            const uint64_t gap = id - id_;
            if (gap > 6) { reset_motion(); }
            else {
                float d[3]; for (int i = 0; i < 3; ++i) d[i] = (f.eye[i] - cur_.eye[i]) / float(gap);
                const float dl = len(d);
                if (!(dl <= c_.max_step) || !std::isfinite(dl)) { reset_motion(); cut_ = true; }
                else {
                    const bool had = vel_valid_;
                    for (int i = 0; i < 3; ++i) { prev_vel_[i] = vel_[i]; vel_[i] = d[i]; }
                    prev_vel_valid_ = had; vel_valid_ = true; cut_ = false;
                    // rotation per frame: R = M_cur * M_prev^T (columns right/up/fwd), axis-angle / gap
                    float w[3]; float ang = 0;
                    if (rel_rotation(cur_, f, w, ang)) {
                        const float per = ang / float(gap);
                        if (per > c_.max_rot_frame_deg * 0.0174532925f) { rot_valid_ = false; prev_rot_valid_ = false; }
                        else {
                            const bool hadr = rot_valid_;
                            for (int i = 0; i < 3; ++i) { prev_w_[i] = w_[i]; w_[i] = w[i] * per; }
                            prev_rot_valid_ = hadr; rot_valid_ = true;
                        }
                    } else { rot_valid_ = false; prev_rot_valid_ = false; }
                }
            }
        }
        cur_ = f; id_ = id; t_us_ = t_us; have_ = true;
    }
    double fps() const noexcept { return fps_; }
    bool have() const noexcept { return have_; }
    // The latest frame moved forward by horizon_ms (wall). frames_ahead = horizon x the measured game frame rate (fixed step per frame).
    ExtrapResult predict(double horizon_ms, Frame& out) const noexcept {
        ExtrapResult r; out = cur_;
        if (!have_) { r.why = "no-frame"; return r; }
        if (!(fps_ > 1.0)) { r.why = "no-rate-yet"; return r; }
        if (!(horizon_ms > 0.0) || !std::isfinite(horizon_ms)) { r.why = "no-horizon"; return r; }
        double h = horizon_ms;
        if (h > c_.max_ms) { h = c_.max_ms; r.clamped = true; }
        r.horizon_ms = h; r.frames_ahead = h * 1e-3 * fps_;
        const float k = float(r.frames_ahead);
        if (cut_) r.why = "cut";
        else if (!vel_valid_ || !prev_vel_valid_) r.why = "warming-up";
        else if (!stable_translation()) r.why = "direction-change";
        else { for (int i = 0; i < 3; ++i) out.eye[i] = cur_.eye[i] + vel_[i] * k; r.translated = true; }
        if (c_.attitude && rot_valid_ && prev_rot_valid_ && stable_rotation()) {
            float axis[3] = {w_[0], w_[1], w_[2]};
            const float rate = len(axis);
            if (rate > 1e-7f) {
                for (float& a : axis) a /= rate;
                float ang = rate * k;
                const float lim = c_.max_rot_deg * 0.0174532925f;
                if (ang > lim) { ang = lim; r.clamped = true; }
                rotate(out.right, axis, ang); rotate(out.up, axis, ang); rotate(out.fwd, axis, ang);
                orthonormalise(out);
                r.rotated = true;
            }
        }
        return r;
    }
    // Wall-seconds velocity of the camera (units/s; zero when unknown) for consumers that work in seconds.
    bool velocity_per_s(float v[3]) const noexcept {
        if (!vel_valid_ || !(fps_ > 1.0)) { v[0] = v[1] = v[2] = 0; return false; }
        for (int i = 0; i < 3; ++i) v[i] = vel_[i] * float(fps_);
        return true;
    }
private:
    static float len(const float a[3]) noexcept { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
    static float dot(const float a[3], const float b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static void cross(const float a[3], const float b[3], float o[3]) noexcept {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
    }
    static void rotate(float v[3], const float k[3], float ang) noexcept {   // Rodrigues
        const float c = std::cos(ang), s = std::sin(ang);
        float kv[3]; cross(k, v, kv);
        const float kd = dot(k, v);
        for (int i = 0; i < 3; ++i) v[i] = v[i] * c + kv[i] * s + k[i] * kd * (1.0f - c);
    }
    static void normalise(float a[3]) noexcept { const float l = len(a); if (l > 1e-9f) for (int i = 0; i < 3; ++i) a[i] /= l; }
    static void orthonormalise(Frame& f) noexcept {   // keep fwd, re-derive right and up (right x up = -fwd)
        normalise(f.fwd);
        const float rf = dot(f.right, f.fwd);
        for (int i = 0; i < 3; ++i) f.right[i] -= rf * f.fwd[i];
        normalise(f.right);
        cross(f.right, f.fwd, f.up);
        normalise(f.up);
    }
    // Rotation taking frame a to frame b as an axis (unit, world) and angle (rad).
    static bool rel_rotation(const Frame& a, const Frame& b, float axis[3], float& ang) noexcept {
        float R[3][3];   // R = Mb * Ma^T, M columns = right, up, fwd
        const float* ca[3] = {a.right, a.up, a.fwd};
        const float* cb[3] = {b.right, b.up, b.fwd};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { float s = 0; for (int k = 0; k < 3; ++k) s += cb[k][i] * ca[k][j]; R[i][j] = s; }
        const float tr = R[0][0] + R[1][1] + R[2][2];
        const float cs = std::max(-1.0f, std::min(1.0f, (tr - 1.0f) * 0.5f));
        ang = std::acos(cs);
        if (!std::isfinite(ang)) return false;
        float v[3] = {R[2][1] - R[1][2], R[0][2] - R[2][0], R[1][0] - R[0][1]};
        const float l = len(v);
        if (ang < 1e-6f || l < 1e-9f) { axis[0] = 0; axis[1] = 1; axis[2] = 0; ang = 0; return true; }
        if (ang > 3.0f) return false;   // near 180 deg: ambiguous axis (a cut anyway)
        for (int i = 0; i < 3; ++i) axis[i] = v[i] / l;
        return true;
    }
    bool stable_translation() const noexcept {
        const float a = len(vel_), b = len(prev_vel_);
        if (a < 1e-3f) return false;            // stopped: nothing to extrapolate (and never overshoots)
        if (b < 1e-3f) return false;            // just started moving: wait for a second step
        const float cs = dot(vel_, prev_vel_) / (a * b), ratio = a / b;
        return cs >= c_.cos_min && ratio >= c_.ratio_min && ratio <= c_.ratio_max;
    }
    bool stable_rotation() const noexcept {
        float d[3] = {w_[0] - prev_w_[0], w_[1] - prev_w_[1], w_[2] - prev_w_[2]};
        return len(d) <= 0.5f * len(w_) + 2e-4f;   // the rate vector changed by less than half its size (2e-4 rad/frame noise floor)
    }
    void reset_motion() noexcept { vel_valid_ = prev_vel_valid_ = rot_valid_ = prev_rot_valid_ = false; }
    ExtrapConfig c_;
    Frame cur_{};
    uint64_t id_ = 0, anchor_id_ = 0;
    int64_t t_us_ = 0, anchor_us_ = 0;
    bool have_ = false, anchor_set_ = false, cut_ = false;
    double fps_ = 0;
    float vel_[3] = {0, 0, 0}, prev_vel_[3] = {0, 0, 0}, w_[3] = {0, 0, 0}, prev_w_[3] = {0, 0, 0};
    bool vel_valid_ = false, prev_vel_valid_ = false, rot_valid_ = false, prev_rot_valid_ = false;
};
// A world point moving with velocity v (units/s) led by lead_s seconds, the displacement clamped to max_units (no runaway on a bad velocity).
inline void lead_point(float p[3], const float v[3], double lead_s, float max_units) noexcept {
    if (!(lead_s > 0) || !std::isfinite(lead_s)) return;
    float d[3]; for (int i = 0; i < 3; ++i) d[i] = float(double(v[i]) * lead_s);
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(l)) return;
    const float s = l > max_units && l > 0 ? max_units / l : 1.0f;
    for (int i = 0; i < 3; ++i) p[i] += d[i] * s;
}

} // namespace acz::pacing
