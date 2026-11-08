#pragma once
// VIEW-MODE (ROADMAP R5 alternative POVs, user wish D-362; docs/world-render/pov.md). Sub-option of the existing key xr_terrain_stage (no new
// config key; default OFF = nothing below runs):
//   pov:1   classify the game's flight view (cockpit / HUD-only / chase / other) every XR frame from the E6 camera geometry and the age of the
//           last flight composite, log it (`pov-mode`, `summary-pov`), and keep the VSEARLY flight stage alive while the game shows a flight
//           view that has no cockpit (HUD-only, chase): the 8 s "no fresh composite" end rule (flight_cycle.h) then counts from the last
//           qualifying view sample, not from the last cockpit composite.
// Evidence (live batch W1, step96; pov.md section 2): the game's view key V cycles cockpit -> chase -> HUD-only -> cockpit. The producer opens a
// frame only at the first DrawCPT (cockpit) draw, so HUD-only and chase publish NO producer frame at all (summary-producer-cost frames=0,
// summary-hud hud-frames=0, the screen quad takes over after vsenter, flight-stage-ended 8 s later). The E6 camera of HUD-only is IDENTICAL to the
// cockpit eye (cam - air = the type's eye offset, j35j 0,3.575,-36.3); chase puts the camera 76..84 units BEHIND and 17 units ABOVE the aircraft
// (the E6 locator rejects it: eye-distance > 60, and loses its lock after 1 s). So: fresh composite = cockpit; eye geometry without composites
// = HUD-only; chase geometry without composites = chase; anything else (mission-end orbit, menus, zero block) = other.
// SDK-free, header-only (options, geometry classifier, mode machine, the probe -> consumer mailbox); tests/acz-vr/view_mode_offline_tests.cpp.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace acz::viewmode {

// ------------------------------------------------------------------------------------------------ option (inside the xr_terrain_stage value)
struct Options {
    bool on = false;
    bool stage2 = false;   // pov:2 (POV-RENDER stage 2, pov_render.h / other-povs.md section 10): pov:1 + the chase view in stereo
    bool stage3 = false;   // pov:3 (POV-RENDER stages 3 + 4, other-povs.md section 11): pov:2 + the HUD-only view + in-engine cinematics + the drawn vignette
    bool any_token = false;
};
// Removes every pov:<0|1|2|3> token (':' or '=') from a "<id>;opt:val;..." value (the first item, the stage id, is never an option). A value without
// it is returned byte-identical. A bad value: false + why, the token is STILL removed from `rest` (the caller logs and ignores it; the terrain
// parser never sees an unknown token). pov:2 = everything pov:1 does plus stage 2; pov:3 = pov:2 plus stages 3 and 4.
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
            if (eq != std::string::npos && item.compare(0, eq, "pov") == 0 && eq == 3) {
                ours = true; o.any_token = true;
                const std::string v = item.substr(eq + 1);
                if (v == "1") { o.on = true; o.stage2 = false; o.stage3 = false; }
                else if (v == "2") { o.on = true; o.stage2 = true; o.stage3 = false; }
                else if (v == "3") { o.on = true; o.stage2 = true; o.stage3 = true; }
                else if (v == "0") { o.on = false; o.stage2 = false; o.stage3 = false; }
                else { ok = false; o.on = false; o.stage2 = false; o.stage3 = false; why = "bad value for pov (0|1|2|3)"; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return ok;
}
inline std::string describe(const Options& o) {
    return std::string("pov=") + (o.stage3 ? "3" : o.stage2 ? "2" : o.on ? "1" : "0");
}

// The process-wide switch: set by the proxy from the option, read by the probe's Present tick (publishes the view mailbox only when on).
inline std::atomic<bool>& enabled_flag() noexcept { static std::atomic<bool> f{false}; return f; }
inline bool enabled() noexcept { return enabled_flag().load(std::memory_order_relaxed); }

// ------------------------------------------------------------------------------------------------ geometry (one E6 sample)
// The E6 fields (ee_e6_locator.h Sample: look p/y/r, aircraft position, heading, camera position, camera rotation p/y/r).
struct Geo { float look[3]{}, air[3]{}, heading = 0, cam[3]{}, rot[3]{}; };
template <class S> inline Geo geo_of(const S& s) noexcept {
    Geo g;
    for (int i = 0; i < 3; ++i) { g.look[i] = s.look[i]; g.air[i] = s.air[i]; g.cam[i] = s.cam[i]; g.rot[i] = s.rot[i]; }
    g.heading = s.heading;
    return g;
}
enum class Geom : uint8_t { invalid = 0, eye, chase, other };
inline const char* geom_name(Geom g) noexcept {
    switch (g) { case Geom::eye: return "eye"; case Geom::chase: return "chase"; case Geom::other: return "other"; default: return "invalid"; }
}
// Thresholds (evidence pov.md section 2; units = game units, ~5 per metre, world_scale.h).
inline constexpr float kEyeMin = 15.0f, kEyeMax = 60.0f;          // = ee_e6_locator.h (the 36 eye points are 19.4..53.2 units)
inline constexpr double kEyeCosMin = 0.9063;                       // cos 25 deg: the eye offset points along the aircraft forward
inline constexpr float kChaseBehindMin = 40.0f, kChaseBehindMax = 260.0f;   // W1: 76.0 parked, 79.5 intro, 83.4 at speed (spring stretch)
inline constexpr float kChaseUpMin = 0.0f, kChaseUpMax = 80.0f;    // W1: 16.5..17.0
inline constexpr float kChaseSideFrac = 0.35f, kChaseSideSlack = 10.0f;   // W1: 0.0
inline constexpr double kYawRelMax = 0.6;                          // |wrap(rot yaw - heading - look yaw)| (= the locator's yaw relation)
inline constexpr double kLookCentredMax = 0.2;                     // chase is only recognised with the look centred (look-around in chase: not observed)
inline constexpr double kHeadingMax = 2.0 * 3.14159265358979 + 0.01;   // = ee_e6_locator.h kHeadingMax (the raw E6 fallback is not check_sample'd)
inline constexpr float kWorldLimit = 262144.0f + 32768.0f;         // = ee_e6_locator.h kWorldHalf + kWorldMargin
struct GeomOut {
    Geom g = Geom::invalid;
    float ahead = 0, up = 0, side = 0, dist = 0;   // camera - aircraft in the aircraft frame (heading, pitch = rot pitch - look pitch)
    const char* why = "nonfinite";                 // the classification reason (stable names, logged)
};
namespace detail {
inline double wrap(double a) noexcept {
    const double tp = 6.283185307179586;
    if (!std::isfinite(a)) return 0.0;
    if (a > 3.141592653589793 || a < -3.141592653589793) {   // bounded reduction (a huge finite value never loops)
        a = std::fmod(a, tp);
        if (a > 3.141592653589793) a -= tp;
        else if (a < -3.141592653589793) a += tp;
    }
    return a;
}
} // namespace detail
inline GeomOut classify(const Geo& s) noexcept {
    GeomOut o;
    const float* all[] = {s.look, s.air, &s.heading, s.cam, s.rot};
    const int cnt[] = {3, 3, 1, 3, 3};
    for (int k = 0; k < 5; ++k) for (int i = 0; i < cnt[k]; ++i) if (!std::isfinite(all[k][i])) return o;
    if (std::fabs(s.air[0]) + std::fabs(s.air[2]) <= 1.0f) { o.why = "aircraft-at-origin"; return o; }
    for (int i = 0; i < 3; ++i) if (std::fabs(double(s.look[i])) > 3.15 || std::fabs(double(s.rot[i])) > 3.15) { o.why = "angle-range"; return o; }
    if (std::fabs(s.air[0]) > kWorldLimit || std::fabs(s.air[2]) > kWorldLimit || std::fabs(s.cam[0]) > kWorldLimit || std::fabs(s.cam[2]) > kWorldLimit) {
        o.why = "position-range"; return o;
    }
    if (std::fabs(double(s.heading)) > kHeadingMax) { o.why = "heading-range"; return o; }
    const double d[3] = {double(s.cam[0]) - s.air[0], double(s.cam[1]) - s.air[1], double(s.cam[2]) - s.air[2]};
    const double h = s.heading, p = double(s.rot[0]) - double(s.look[0]);
    // Aircraft frame (the game's right-handed frame, D-297; = ee_e6_locator.cpp eye_angle_deg): forward f, right r, up u = r x f.
    const double f[3] = {-std::sin(h) * std::cos(p), std::sin(p), -std::cos(h) * std::cos(p)};
    const double r[3] = {std::cos(h), 0.0, -std::sin(h)};
    const double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    o.ahead = float(d[0] * f[0] + d[1] * f[1] + d[2] * f[2]);
    o.up = float(d[0] * u[0] + d[1] * u[1] + d[2] * u[2]);
    o.side = float(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
    o.dist = float(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    const bool yaw_ok = std::fabs(detail::wrap(double(s.rot[1]) - h - double(s.look[1]))) <= kYawRelMax;
    if (o.dist >= kEyeMin && o.dist <= kEyeMax && double(o.ahead) >= kEyeCosMin * double(o.dist) && yaw_ok) { o.g = Geom::eye; o.why = "eye-offset"; return o; }
    const bool centred = std::fabs(double(s.look[0])) <= kLookCentredMax && std::fabs(double(s.look[1])) <= kLookCentredMax &&
                         std::fabs(double(s.look[2])) <= kLookCentredMax;
    const float behind = -o.ahead;
    if (centred && yaw_ok && behind >= kChaseBehindMin && behind <= kChaseBehindMax && o.up >= kChaseUpMin && o.up <= kChaseUpMax &&
        std::fabs(o.side) <= kChaseSideFrac * behind + kChaseSideSlack) {
        o.g = Geom::chase; o.why = "behind-above"; return o;
    }
    o.g = Geom::other;
    o.why = !centred ? "look-not-centred" : !yaw_ok ? "yaw-relation" : o.ahead > 0 ? "camera-ahead" : "offset-out-of-band";
    return o;
}

// ------------------------------------------------------------------------------------------------ mode machine (consumer owner thread)
enum class Mode : uint8_t { unknown = 0, cockpit, hud_only, chase, other };
inline const char* mode_name(Mode m) noexcept {
    switch (m) {
        case Mode::cockpit: return "cockpit"; case Mode::hud_only: return "hud-only"; case Mode::chase: return "chase";
        case Mode::other: return "other"; default: return "unknown";
    }
}
// Per-mode gating (pov.md section 3). The consumer composes only producer frames, which exist only with DrawCPT draws, so today these gates
// never change a composed frame; they are the contract for the producer's no-cockpit frames (pov.md section 4, phase 2).
inline bool draw_cockpit_model(Mode m) noexcept { return m != Mode::hud_only && m != Mode::chase; }
inline bool flight_view(Mode m) noexcept { return m == Mode::cockpit || m == Mode::hud_only || m == Mode::chase; }

inline constexpr int64_t kCompositeFreshMs = 250;   // a composite this young = the cockpit view (= DECOUPLE dc-age-ms)
inline constexpr int64_t kConfirmMs = 300;          // a non-cockpit candidate must hold this long (cockpit -> other view: ~550 ms after the key)
inline constexpr int64_t kFrozenKeepMs = 90000;     // keep-alive while the aircraft does not move (parked runway/takeoff loop ~60 s); then the 8 s rule
inline constexpr int64_t kSampleStaleMs = 500;      // no new mailbox publication for this long = no sample (the Present tick runs at 60 Hz)

struct In {
    int64_t now_ms = 0;
    int64_t composite_age_ms = -1;   // age of the last fresh flight composite (-1 = none yet)
    bool sample = false;             // a current E6 sample (the probe's view mailbox)
    Geo s{};
};
struct Out {
    Mode mode = Mode::unknown;
    Mode from = Mode::unknown;
    bool changed = false;
    GeomOut geom{};
    int64_t alive_ms = -1;   // the last time a no-cockpit flight view qualified for the keep-alive (-1 = never)
};
class Machine {
public:
    Out step(const In& in) noexcept {
        Out o;
        o.geom = in.sample ? classify(in.s) : GeomOut{};
        // aircraft motion (bit-exact position change), for the frozen keep-alive cap
        if (in.sample && o.geom.g != Geom::invalid) {
            // The last known position and motion deadline survive missing/invalid samples (an outage is not motion); only a real change advances.
            if (!ever_sample_ || std::memcmp(prev_air_, in.s.air, sizeof prev_air_) != 0) last_motion_ms_ = in.now_ms;
            std::memcpy(prev_air_, in.s.air, sizeof prev_air_); ever_sample_ = true; have_prev_ = true;
        } else have_prev_ = false;
        Mode cand;
        const bool fresh = in.composite_age_ms >= 0 && in.composite_age_ms <= kCompositeFreshMs;
        if (fresh) cand = Mode::cockpit;
        else if (o.geom.g == Geom::eye) cand = Mode::hud_only;
        else if (o.geom.g == Geom::chase) cand = Mode::chase;
        else cand = Mode::other;
        o.from = mode_;
        if (cand == mode_) pending_ = Mode::unknown;
        else if (fresh) { set(cand, in.now_ms); }   // a cockpit composite is proof: no confirmation
        else if (cand != pending_) { pending_ = cand; pending_since_ = in.now_ms; }
        else if (in.now_ms - pending_since_ >= kConfirmMs) set(cand, in.now_ms);
        o.changed = mode_ != o.from;
        if ((mode_ == Mode::hud_only || mode_ == Mode::chase) && have_prev_ && in.now_ms - last_motion_ms_ <= kFrozenKeepMs) {
            alive_ms_ = in.now_ms; ++alive_frames_;
        }
        o.mode = mode_;
        o.alive_ms = alive_ms_;
        ++frames_[unsigned(mode_)];
        ++geoms_[unsigned(o.geom.g)];
        return o;
    }
    Mode mode() const noexcept { return mode_; }
    uint64_t transitions() const noexcept { return transitions_; }
    uint64_t frames(Mode m) const noexcept { return frames_[unsigned(m)]; }
    uint64_t geoms(Geom g) const noexcept { return geoms_[unsigned(g)]; }
    uint64_t alive_frames() const noexcept { return alive_frames_; }
private:
    void set(Mode m, int64_t now) noexcept { mode_ = m; pending_ = Mode::unknown; since_ms_ = now; ++transitions_; }
    Mode mode_ = Mode::unknown, pending_ = Mode::unknown;
    int64_t pending_since_ = 0, since_ms_ = 0, last_motion_ms_ = 0, alive_ms_ = -1;
    bool have_prev_ = false, ever_sample_ = false;
    float prev_air_[3]{};
    uint64_t transitions_ = 0, alive_frames_ = 0;
    uint64_t frames_[5]{}, geoms_[4]{};
};

// ------------------------------------------------------------------------------------------------ probe -> consumer mailbox (seqlock, one writer)
// The probe's Present tick publishes the RAW E6 sample of the locked (else the last locked) block every tick while pov:1 is on, also when the
// locator rejects it (chase): 92 bytes per tick, the same guarded read the locator uses. Read-only game memory.
class ViewMailbox {
public:
    static constexpr unsigned kWords = 13;
    void publish(const Geo& g) noexcept {
        const float v[kWords] = {g.look[0], g.look[1], g.look[2], g.air[0], g.air[1], g.air[2], g.heading, g.cam[0], g.cam[1], g.cam[2],
                                 g.rot[0], g.rot[1], g.rot[2]};
        write(v, 1u);
    }
    void invalidate() noexcept { const float v[kWords]{}; write(v, 0u); }
    // false: nothing published, a write in progress (try-only) or the last publication was an invalidation. n = publication number.
    bool read(Geo& g, uint64_t& n) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1u)) return false;
        uint32_t w[kWords];
        for (unsigned i = 0; i < kWords; ++i) w[i] = w_[i].load(std::memory_order_relaxed);
        const uint32_t valid = valid_.load(std::memory_order_relaxed);
        const uint64_t pn = n_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        n = pn;
        if (!valid) return false;
        float v[kWords];
        std::memcpy(v, w, sizeof v);
        for (int i = 0; i < 3; ++i) { g.look[i] = v[i]; g.air[i] = v[3 + i]; g.cam[i] = v[7 + i]; g.rot[i] = v[10 + i]; }
        g.heading = v[6];
        return true;
    }
    uint64_t published() const noexcept { return n_.load(std::memory_order_relaxed); }
private:
    void write(const float v[kWords], uint32_t valid) noexcept {
        uint32_t w[kWords];
        std::memcpy(w, v, sizeof w);
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < kWords; ++i) w_[i].store(w[i], std::memory_order_relaxed);
        valid_.store(valid, std::memory_order_relaxed);
        n_.store(n_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }
    std::atomic<uint32_t> seq_{0}, valid_{0};
    std::atomic<uint32_t> w_[kWords]{};
    std::atomic<uint64_t> n_{0};
};
inline ViewMailbox& view_mailbox() noexcept { static ViewMailbox m; return m; }

// ------------------------------------------------------------------------------------------------ consumer glue (owner thread, every XR frame)
// Reads the mailbox, runs the machine, returns the keep-alive time and at most one log line per call (a mode change first, else the 10 s summary).
class Tracker {
public:
    struct Step {
        int64_t alive_ms = -1;
        Mode mode = Mode::unknown;
        bool line = false, important = false;
        char text[400] = {};
        // POV-RENDER (pov_render.h; read by its route/rig/census only): this step's sample and its geometry (a copy; nothing above depends on it).
        bool sample = false;
        Geo s{};
        GeomOut geom{};
    };
    Step step(int64_t now_ms, int64_t composite_age_ms, const ViewMailbox& mb) noexcept {
        In in; in.now_ms = now_ms; in.composite_age_ms = composite_age_ms;
        uint64_t n = 0;
        const bool got = mb.read(in.s, n);
        if (n != last_n_) { last_n_ = n; last_n_ms_ = now_ms; }
        in.sample = got && now_ms - last_n_ms_ <= kSampleStaleMs;
        if (!in.sample) ++no_sample_;
        const Out o = m_.step(in);
        Step st; st.alive_ms = o.alive_ms; st.mode = o.mode;
        st.sample = in.sample; st.s = in.s; st.geom = o.geom;
        if (o.changed) {
            st.line = st.important = true;
            std::snprintf(st.text, sizeof st.text, "xr-stereo;event=pov-mode;from=%s;to=%s;geom=%s;why=%s;ahead=%.1f;up=%.1f;side=%.1f;dist=%.1f;"
                "composite-age-ms=%lld;keep-alive=%s;cockpit-model=%s", mode_name(o.from), mode_name(o.mode), geom_name(o.geom.g), o.geom.why,
                double(o.geom.ahead), double(o.geom.up), double(o.geom.side), double(o.geom.dist), static_cast<long long>(composite_age_ms),
                (o.mode == Mode::hud_only || o.mode == Mode::chase) ? "on(flight-stage-end-counts-from-last-view-sample)" : "off",
                draw_cockpit_model(o.mode) ? "drawn-with-composites" : "gated");
        } else if (last_summary_ms_ < 0 || now_ms - last_summary_ms_ >= 10000) {
            last_summary_ms_ = now_ms;
            st.line = true;
            using ull = unsigned long long;
            std::snprintf(st.text, sizeof st.text, "xr-stereo;event=summary-pov;mode=%s;frames=cockpit:%llu,hud-only:%llu,chase:%llu,other:%llu,unknown:%llu;"
                "geom=eye:%llu,chase:%llu,other:%llu,invalid:%llu;transitions=%llu;keep-alive-frames=%llu;alive-ms=%lld;no-sample=%llu;published=%llu",
                mode_name(o.mode), ull(m_.frames(Mode::cockpit)), ull(m_.frames(Mode::hud_only)), ull(m_.frames(Mode::chase)), ull(m_.frames(Mode::other)),
                ull(m_.frames(Mode::unknown)), ull(m_.geoms(Geom::eye)), ull(m_.geoms(Geom::chase)), ull(m_.geoms(Geom::other)), ull(m_.geoms(Geom::invalid)),
                ull(m_.transitions()), ull(m_.alive_frames()), static_cast<long long>(o.alive_ms), ull(no_sample_), ull(mb.published()));
        }
        return st;
    }
    const Machine& machine() const noexcept { return m_; }
private:
    Machine m_;
    uint64_t last_n_ = 0, no_sample_ = 0;
    int64_t last_n_ms_ = 0, last_summary_ms_ = -1;
};

} // namespace acz::viewmode
