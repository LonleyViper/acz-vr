#pragma once
// SEATED RECENTER (docs/world-render/recenter.md): a user-triggered (key) and optional automatic (first flight sample) recenter of the seated
// neutral, without taking the headset off. SDK-free, OS-free, header-only: the glue (xr_stereo_proxy.cpp) feeds it the key state, the latest pose
// snapshot and a clock and acts on its decisions (Owner::request_recenter + a log line).
//
// Options inside the existing key xr_terrain_stage (no new config key); removed from the value before the other parsers (like lookstick:):
//   rc:<0|1>        default 1  recenter on (0 = never act, the key is not polled)
//   rckey:<key>     default home  home end insert delete pageup pagedown pause scrolllock f1..f24 (f12 = Steam's screenshot key), or 0xNN / decimal virtual key.
//                   Refused (never a recenter key): Esc, Enter, Space, arrows, digits, letters, the numpad (the look keys 4/6/8/2) and mouse buttons.
//   rcauto:<cm>     default 0 (off); 1..200: at the first flight sample, recenter once when the head is farther than <cm> from the neutral eye point
//   rcyaw:<deg>     default 30 (5..180): the auto trigger also fires when the head heading differs from the neutral heading by more than this
//   rcfocus:<0|1>   default 1: the key counts only while the game window is the foreground window
// A bad value is logged and the whole recenter option falls back to the defaults (the world is never refused for it).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace acz::recenter {

constexpr unsigned kVkHome = 0x24;
constexpr int64_t kMinIntervalMs = 1000;     // two recenters are never closer than this (key repeat, bounce, an unfocused hold)
constexpr int64_t kPendingTimeoutMs = 2000;  // a request not seen as a new generation within this is dropped (session paused/dead)
constexpr int64_t kFlightGapMs = 500;        // flight composites unchanged for this long = not in flight

struct Options {
    bool on = true;
    unsigned vk = kVkHome;
    double auto_cm = 0.0;        // 0 = off
    double auto_yaw_deg = 30.0;
    bool focus = true;
    bool any_token = false;
};

// A key that must never become the recenter key: the game's own keys (Esc, look NumPad 4/6/8/2 and the whole numpad, V, S and the other
// letters, digits, arrows, Enter, Space, Tab) and the mouse buttons.
inline bool key_forbidden(unsigned vk) noexcept {
    if (vk < 8 || vk > 254) return true;
    if (vk < 0x10) return true;                                 // mouse buttons, Back, Tab, Clear, Enter
    if (vk == 0x1B || vk == 0x20) return true;                  // Esc, Space
    if (vk >= 0x25 && vk <= 0x28) return true;                  // arrows
    if (vk >= 0x30 && vk <= 0x39) return true;                  // digits
    if (vk >= 0x41 && vk <= 0x5A) return true;                  // letters
    if (vk >= 0x60 && vk <= 0x6F) return true;                  // numpad digits and operators
    if (vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0x5B || vk == 0x5C) return true;   // modifiers
    return false;
}

inline bool parse_key(const std::string& s, unsigned& vk) {
    static const struct { const char* n; unsigned v; } names[] = {
        {"home", 0x24}, {"end", 0x23}, {"insert", 0x2D}, {"ins", 0x2D}, {"delete", 0x2E}, {"del", 0x2E},
        {"pageup", 0x21}, {"pgup", 0x21}, {"pagedown", 0x22}, {"pgdn", 0x22}, {"pause", 0x13}, {"scrolllock", 0x91}, {"scroll", 0x91}};
    std::string l; for (char c : s) l += char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    unsigned v = 0; bool found = false;
    for (const auto& e : names) if (l == e.n) { v = e.v; found = true; break; }
    if (!found && l.size() >= 2 && l[0] == 'f') {
        unsigned n = 0; bool digits = true;
        for (size_t i = 1; i < l.size(); ++i) { if (l[i] < '0' || l[i] > '9') { digits = false; break; } n = n * 10 + unsigned(l[i] - '0'); if (n > 99) { digits = false; break; } }
        if (digits && n >= 1 && n <= 24) { v = 0x70 + n - 1; found = true; }
    }
    if (!found && !l.empty()) {
        char* end = nullptr;
        const bool hex = l.size() > 2 && l[0] == '0' && l[1] == 'x';
        const unsigned long u = std::strtoul(l.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
        if (end && *end == 0 && u <= 254) { v = unsigned(u); found = true; }
    }
    if (!found || key_forbidden(v)) return false;
    vk = v; return true;
}

inline bool parse_number(const std::string& s, double lo, double hi, double& out) {
    if (s.empty()) return false;
    char* end = nullptr; const double d = std::strtod(s.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(d) || d < lo || d > hi) return false;
    out = d; return true;
}

// Removes the recenter tokens from the ';'-separated value (the first item is the stage id, never an option). On a bad value returns false, sets
// `why`, and `opt` is the defaults (any_token still true); `rest` never contains our tokens.
inline bool extract_options(const std::string& value, std::string& rest, Options& opt, std::string& why) {
    Options o; rest.clear(); bool ok = true;
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
                auto bad = [&](const char* m) { if (ok) why = m; ok = false; };
                if (k == "rc") { ours = true; if (v == "1") o.on = true; else if (v == "0") o.on = false; else bad("bad value for rc (0|1)"); }
                else if (k == "rckey") { ours = true; if (!parse_key(v, o.vk)) bad("bad value for rckey (home|end|insert|delete|pageup|pagedown|pause|scrolllock|f1..f24|0xNN; never a game key)"); }
                else if (k == "rcauto") { ours = true; if (v == "0") o.auto_cm = 0; else if (!parse_number(v, 1, 200, o.auto_cm)) bad("bad value for rcauto (0 or 1..200 cm)"); }
                else if (k == "rcyaw") { ours = true; if (!parse_number(v, 5, 180, o.auto_yaw_deg)) bad("bad value for rcyaw (5..180 deg)"); }
                else if (k == "rcfocus") { ours = true; if (v == "1") o.focus = true; else if (v == "0") o.focus = false; else bad("bad value for rcfocus (0|1)"); }
                if (ours) o.any_token = true;
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    if (!ok) { Options d; d.any_token = true; opt = d; return false; }
    opt = o; return true;
}

inline std::string describe(const Options& o) {
    char b[200];
    std::snprintf(b, sizeof b, "recenter=%u;key=0x%02X;auto-cm=%.0f;auto-yaw-deg=%.0f;focus=%u", o.on ? 1u : 0u, o.vk, o.auto_cm, o.auto_yaw_deg, o.focus ? 1u : 0u);
    return b;
}

// ---- pose math (the producer's seated-recentering convention, gpu_copy_step5.h setup_poses / Neutral) ----------------------------------------

struct Head {
    bool ok = false;
    double p[3]{};     // head centre: the midpoint of the two eye positions, LOCAL metres
    double yaw = 0.0;  // heading about +Y: atan2(-fx, -fz) of the LEFT eye's forward R(0,0,-1); 0 when it points (nearly) straight up/down
};

// Same validity as the producer (finite, |p| <= 1000 m, quaternion norm within (0.5, 1.5)); same centre and heading as Neutral.
inline Head head_of(const float pos[2][3], const float ori[2][4]) noexcept {
    Head h;
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 3; ++k) if (!std::isfinite(pos[e][k]) || std::fabs(pos[e][k]) > 1000.0f) return h;
        const double n = std::sqrt(double(ori[e][0]) * ori[e][0] + double(ori[e][1]) * ori[e][1] + double(ori[e][2]) * ori[e][2] + double(ori[e][3]) * ori[e][3]);
        if (!std::isfinite(n) || !(n > 0.5) || !(n < 1.5)) return h;
    }
    for (int k = 0; k < 3; ++k) h.p[k] = 0.5 * (double(pos[0][k]) + double(pos[1][k]));
    double x = ori[0][0], y = ori[0][1], z = ori[0][2], w = ori[0][3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w); x /= n; y /= n; z /= n; w /= n;
    const double fx = -2 * (x * z + y * w), fz = -(1 - 2 * (x * x + y * y));   // forward = R (0,0,-1) = -(R[2], R[8])
    h.yaw = std::hypot(fx, fz) > 1e-6 ? std::atan2(-fx, -fz) : 0.0;
    h.ok = true; return h;
}

inline double wrap_pi(double a) noexcept {
    const double two = 6.283185307179586;
    a = std::fmod(a + 3.141592653589793, two); if (a < 0) a += two;
    return a - 3.141592653589793;
}

// The head relative to a neutral (p0, yaw0), in the cockpit frame: Ry(-yaw0)(p - p0) (metres, +x right, +y up, -z nose) and the heading difference.
struct Offset { double d[3]{}; double dyaw = 0.0; double dist() const noexcept { return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); } };
inline Offset offset_from(const Head& h, const Head& neutral) noexcept {
    Offset o;
    const double cy = std::cos(-neutral.yaw), sv = std::sin(-neutral.yaw);
    const double dx = h.p[0] - neutral.p[0], dy = h.p[1] - neutral.p[1], dz = h.p[2] - neutral.p[2];
    o.d[0] = cy * dx + sv * dz; o.d[1] = dy; o.d[2] = -sv * dx + cy * dz;
    o.dyaw = wrap_pi(h.yaw - neutral.yaw);
    return o;
}

// ---- trigger debounce --------------------------------------------------------------------------------------------------------------------

// Fires once per press (rising edge), only after the key was seen UP at least once (a key already held when polling starts never fires),
// and never closer than kMinIntervalMs to the previous fire. Holding the key never repeats.
class Debounce {
public:
    bool update(bool down, int64_t now_ms) noexcept {
        if (!down) { armed_ = true; return false; }
        if (!armed_) return false;
        armed_ = false;                                    // consumed: a hold or auto-repeat needs a release first
        if (fired_ && now_ms - last_ms_ < kMinIntervalMs) return false;
        fired_ = true; last_ms_ = now_ms; return true;
    }
private:
    bool armed_ = false, fired_ = false; int64_t last_ms_ = 0;
};

// ---- controller ---------------------------------------------------------------------------------------------------------------------------

struct Input {
    bool key_down = false;          // the configured key, already gated by window focus
    const Head* head = nullptr;     // the latest valid pose snapshot (null = none)
    uint32_t generation = 0;        // the snapshot's LOCAL space generation (PoseSnapshot::space_generation)
    uint64_t flight_composites = 0; // Output::flight_composites(): moves while the in-flight world is composed
    int64_t now_ms = 0;
};
enum class Reason : uint8_t { none, key, autoflight, runtime };   // runtime = a generation change nobody here asked for (SteamVR's own reset seated position)
inline const char* reason_name(Reason r) noexcept { return r == Reason::key ? "key" : r == Reason::autoflight ? "auto-flight" : r == Reason::runtime ? "runtime-space-change" : "none"; }

struct Output {
    bool request = false;           // call Owner::request_recenter() now
    Reason request_reason = Reason::none;
    Offset request_offset;          // the head's offset from the neutral at the request (the "before" offset)
    bool applied = false;           // the mirror saw the new generation and captured the new neutral
    Reason applied_reason = Reason::none;
    Offset before;                  // head vs the OLD neutral at the capture sample
    Offset after;                   // head vs the NEW neutral (zero by construction at the capture sample)
    double shift[3]{};              // new p0 - old p0 (LOCAL metres)
    double yaw_from = 0, yaw_to = 0;
    uint32_t generation = 0;
    unsigned count = 0;             // applied recenters of this session (not counting the very first neutral)
};

class Controller {
public:
    explicit Controller(const Options& o) : opt_(o) {}
    const Options& options() const noexcept { return opt_; }
    unsigned applied_count() const noexcept { return applied_; }
    bool have_neutral() const noexcept { return have_; }
    const Head& neutral() const noexcept { return neutral_; }

    Output step(const Input& in) noexcept {
        Output out;
        if (!opt_.on) return out;
        // 1. mirror the producer's neutral: it is captured from the first valid pose of every LOCAL generation.
        if (in.head && in.head->ok && (!have_ || in.generation != gen_)) {
            const bool had = have_;
            const Head old = neutral_;
            const Offset before = had ? offset_from(*in.head, old) : Offset{};
            neutral_ = *in.head; gen_ = in.generation; have_ = true;
            if (had) {
                ++applied_;
                out.applied = true;
                out.applied_reason = pending_ != Reason::none ? pending_ : Reason::runtime;
                out.before = pending_ != Reason::none ? pending_offset_ : before;
                out.after = offset_from(*in.head, neutral_);
                for (int k = 0; k < 3; ++k) out.shift[k] = neutral_.p[k] - old.p[k];
                out.yaw_from = old.yaw; out.yaw_to = neutral_.yaw; out.generation = in.generation; out.count = applied_;
                pending_ = Reason::none;
            }
        }
        if (pending_ != Reason::none && in.now_ms - pending_ms_ >= kPendingTimeoutMs) pending_ = Reason::none;
        // 2. flight edge (flight composites moved within kFlightGapMs).
        if (in.flight_composites != last_fc_) { last_fc_ = in.flight_composites; last_fc_move_ms_ = in.now_ms; moved_ = true; }
        const bool in_flight = moved_ && in.now_ms - last_fc_move_ms_ <= kFlightGapMs;
        if (in_flight && !was_flight_) auto_armed_ = true;
        was_flight_ = in_flight;
        // 3. triggers (only with a pose and a neutral to measure against; one request in flight at a time).
        const bool key = deb_.update(in.key_down, in.now_ms);
        if (!(in.head && in.head->ok && have_ && pending_ == Reason::none)) return out;
        const Offset off = offset_from(*in.head, neutral_);
        Reason want = Reason::none;
        if (key) want = Reason::key;
        else if (auto_armed_ && in_flight) {
            auto_armed_ = false;   // evaluated once per flight start
            if (opt_.auto_cm > 0 && (off.dist() * 100.0 > opt_.auto_cm || std::fabs(off.dyaw) * 57.29577951308232 > opt_.auto_yaw_deg)) want = Reason::autoflight;
        }
        if (want != Reason::none) {
            out.request = true; out.request_reason = want; out.request_offset = off;
            pending_ = want; pending_offset_ = off; pending_ms_ = in.now_ms;
        }
        return out;
    }
private:
    Options opt_;
    Debounce deb_;
    Head neutral_; uint32_t gen_ = 0; bool have_ = false; unsigned applied_ = 0;
    Reason pending_ = Reason::none; Offset pending_offset_; int64_t pending_ms_ = 0;
    uint64_t last_fc_ = 0; int64_t last_fc_move_ms_ = 0; bool moved_ = false, was_flight_ = false, auto_armed_ = false;
};

inline std::string format_request(const Output& o, uint32_t generation) {
    char b[360];
    std::snprintf(b, sizeof b, "xr-stereo;event=recenter;stage=request;reason=%s;gen=%u;before-offset-cm=%.1f,%.1f,%.1f;before-yaw-deg=%.1f;mode=instant(new-LOCAL-generation)",
        reason_name(o.request_reason), generation, o.request_offset.d[0] * 100, o.request_offset.d[1] * 100, o.request_offset.d[2] * 100, o.request_offset.dyaw * 57.29577951308232);
    return b;
}
inline std::string format_applied(const Output& o) {
    char b[480];
    std::snprintf(b, sizeof b, "xr-stereo;event=recenter;stage=applied;reason=%s;gen=%u;n=%u;before-offset-cm=%.1f,%.1f,%.1f;before-yaw-deg=%.1f;"
        "after-offset-cm=%.1f,%.1f,%.1f;after-yaw-deg=%.1f;neutral-shift-cm=%.1f,%.1f,%.1f;neutral-yaw-deg=%.1f->%.1f",
        reason_name(o.applied_reason), o.generation, o.count,
        o.before.d[0] * 100, o.before.d[1] * 100, o.before.d[2] * 100, o.before.dyaw * 57.29577951308232,
        o.after.d[0] * 100, o.after.d[1] * 100, o.after.d[2] * 100, o.after.dyaw * 57.29577951308232,
        o.shift[0] * 100, o.shift[1] * 100, o.shift[2] * 100, o.yaw_from * 57.29577951308232, o.yaw_to * 57.29577951308232);
    return b;
}

} // namespace acz::recenter
