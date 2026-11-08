#pragma once
// NULL-HEAD (TEST ONLY; docs/world-render/null-head-look.md): a scripted synthetic head rotation for the null-HMD test flights. SteamVR's null driver has
// a FIXED head pose, so pressing the game's look keys (the old test method) turned the GAME camera and the forward window with it. This turns the HEAD
// instead: the owner thread rewrites each begun frame's located views and VIEW-space head (xr_owner::PoseFilter) BEFORE the pose snapshot is published
// and BEFORE the consumer builds its layers, so the producer (eye matrices, culling, 360 boxes, clouds, world), the consumer (projection views, quads,
// brief3d/vscreen placement) and the submitted layer poses all see the same turned head while the game camera stays forward. The compositor reprojects
// with the submitted pose; with the null HMD the real pose is fixed, so the mirror shows the turned view.
//
// SDK-free, OS-free, header-only. The glue (xr_stereo_proxy.cpp) polls %LOCALAPPDATA%\acz-vr\head.now, posts commands to the Mailbox, attaches a
// PoseFilter that runs Controller + apply() and logs the lines formatted here.
//
// Options inside the existing key xr_terrain_stage (no new config key; removed before the terrain parser, like rc:). Default OFF:
//   nh:<0|1>       1 = enable (the head stays forward until a head.now command; nothing changes without one)
//   nhs:<deg/s>    slew speed, default 90 (10..720)
// A bad value is logged and the feature stays OFF (the world is never refused for it).
//
// head.now (ASCII, deleted when read): "<yaw_deg> <pitch_deg> [<hold_s> [<roll_deg>]]" (spaces, tabs or commas). Angles are relative to the seated
// forward: yaw + = LEFT (OpenXR +Y; the same sign as recenter.h / look_drive_keys.h head yaw), pitch + = UP, roll + = left ear down (about +Z).
// |yaw| <= 180, |pitch| <= 89, |roll| <= 180, hold 0..600 s. The head slews to the target, holds it for hold_s (0 = until the next command) counted from
// ARRIVAL, then slews back to 0 0. "0 0" returns at once.
//
// Neutral rule: the producer captures its seated neutral (gpu_copy_step5.h Neutral) from the first pose of each LOCAL generation it sees, so the
// rotation is forced to identity for kSettleMs after every session start and every LOCAL generation change (recenter / runtime space change).
// Issue commands only once the flight's world is showing.
#include "xr_owner.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace acz::nullhead {

constexpr double kDefaultSlewDps = 90.0, kMinSlewDps = 10.0, kMaxSlewDps = 720.0;
constexpr double kMaxYawDeg = 180.0, kMaxPitchDeg = 89.0, kMaxRollDeg = 180.0, kMaxHoldS = 600.0;
constexpr int64_t kSettleMs = 1000;    // identity after a session start / LOCAL generation change (the producer's neutral capture)
constexpr int64_t kSummaryMs = 5000;   // one summary line per this while enabled (also with zero counters)
constexpr int64_t kMaxStepMs = 250;    // a stalled frame never jumps the head by more than this much slew
constexpr int64_t kPollMs = 250;       // the glue polls head.now at most this often

struct Options {
    bool on = false;
    double slew_dps = kDefaultSlewDps;
    bool any_token = false;
};

inline bool parse_num(const std::string& s, double lo, double hi, double& out) {
    if (s.empty()) return false;
    char* end = nullptr; const double d = std::strtod(s.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(d) || d < lo || d > hi) return false;
    out = d; return true;
}

// Removes nh:/nhs: from the ';'-separated value (the first item is the stage id, never an option). On a bad value returns false, sets `why`, and
// `opt` is OFF (any_token still true); `rest` never contains our tokens.
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
                if (k == "nh") { ours = true; if (v == "1") o.on = true; else if (v == "0") o.on = false; else bad("bad value for nh (0|1)"); }
                else if (k == "nhs") { ours = true; if (!parse_num(v, kMinSlewDps, kMaxSlewDps, o.slew_dps)) bad("bad value for nhs (10..720 deg/s)"); }
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
    char b[96];
    std::snprintf(b, sizeof b, "null-head=%u;slew-dps=%.0f", o.on ? 1u : 0u, o.slew_dps);
    return b;
}

// ---- head.now commands ---------------------------------------------------------------------------------------------------------------------

struct Command { double yaw = 0, pitch = 0, roll = 0, hold_s = 0; };

inline bool parse_command(const char* text, size_t n, Command& c, std::string& why) {
    double v[4] = {0, 0, 0, 0}; int count = 0;
    size_t i = 0;
    while (i < n) {
        const char ch = text[i];
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == '\r' || ch == '\n' || ch == ';') { ++i; continue; }
        if (ch == 0) break;
        size_t j = i;
        while (j < n && text[j] && text[j] != ' ' && text[j] != '\t' && text[j] != ',' && text[j] != '\r' && text[j] != '\n' && text[j] != ';') ++j;
        if (count >= 4) { why = "more than 4 numbers (yaw pitch [hold [roll]])"; return false; }
        const std::string tok(text + i, j - i);
        char* end = nullptr; const double d = std::strtod(tok.c_str(), &end);
        if (!end || *end != 0 || !std::isfinite(d)) { why = "not a number: " + tok.substr(0, 16); return false; }
        v[count++] = d;
        i = j;
    }
    if (count < 2) { why = "need at least yaw and pitch"; return false; }
    if (std::fabs(v[0]) > kMaxYawDeg) { why = "yaw outside -180..180"; return false; }
    if (std::fabs(v[1]) > kMaxPitchDeg) { why = "pitch outside -89..89"; return false; }
    if (v[2] < 0 || v[2] > kMaxHoldS) { why = "hold outside 0..600 s"; return false; }
    if (std::fabs(v[3]) > kMaxRollDeg) { why = "roll outside -180..180"; return false; }
    c.yaw = v[0]; c.pitch = v[1]; c.hold_s = v[2]; c.roll = v[3];
    return true;
}

// Single writer (the glue's monitor thread), single reader (the owner thread). Seqlock over integer atomics: never waits, never allocates; a read that
// overlaps a write is simply retried on the next frame.
class Mailbox {
public:
    void post(const Command& c) noexcept {
        seq_.fetch_add(1, std::memory_order_acq_rel);   // odd: write in progress
        yaw_.store(int32_t(std::lround(c.yaw * 1000)), std::memory_order_relaxed);
        pitch_.store(int32_t(std::lround(c.pitch * 1000)), std::memory_order_relaxed);
        roll_.store(int32_t(std::lround(c.roll * 1000)), std::memory_order_relaxed);
        hold_.store(int32_t(std::lround(c.hold_s * 1000)), std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);   // even: complete
    }
    // true = a complete command newer than `seen` was read (seen is updated).
    bool read(uint32_t& seen, Command& out) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) || s1 == seen) return false;
        Command c;
        c.yaw = yaw_.load(std::memory_order_relaxed) / 1000.0;
        c.pitch = pitch_.load(std::memory_order_relaxed) / 1000.0;
        c.roll = roll_.load(std::memory_order_relaxed) / 1000.0;
        c.hold_s = hold_.load(std::memory_order_relaxed) / 1000.0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        seen = s1; out = c; return true;
    }
    uint32_t sequence() const noexcept { return seq_.load(std::memory_order_acquire); }
private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<int32_t> yaw_{0}, pitch_{0}, roll_{0}, hold_{0};
};

// ---- slew controller -----------------------------------------------------------------------------------------------------------------------

struct Angles { double yaw = 0, pitch = 0, roll = 0; };   // degrees
inline bool is_zero(const Angles& a) noexcept { return a.yaw == 0 && a.pitch == 0 && a.roll == 0; }
enum class Phase : uint8_t { idle, slewing, holding, returning };
inline const char* phase_name(Phase p) noexcept {
    return p == Phase::slewing ? "slewing" : p == Phase::holding ? "holding" : p == Phase::returning ? "returning" : "idle";
}

struct StepOut {
    Angles cur;                // the rotation to apply this frame (identity while settling)
    Angles target;
    Phase phase = Phase::idle;
    bool settling = false;     // inside kSettleMs after a session start / generation change
    bool settle_start = false; const char* settle_reason = "";
    bool command = false; Command cmd;
    bool reached = false;      // arrived at a non-zero target
    bool returning = false;    // the hold expired: slewing back to 0 0
    bool home = false;         // arrived back at 0 0
    bool summary = false;
};

class Controller {
public:
    explicit Controller(double slew_dps, uint32_t seen = 0) noexcept
        : dps_(slew_dps >= kMinSlewDps && slew_dps <= kMaxSlewDps ? slew_dps : kDefaultSlewDps), seen_(seen) {}

    StepOut step(int64_t now_ms, uint32_t generation, const Mailbox* mb) noexcept {
        StepOut o;
        int64_t dt = 0;
        if (!started_) {
            started_ = true; gen_ = generation; settle_until_ = now_ms + kSettleMs; next_summary_ = now_ms + kSummaryMs;
            o.settle_start = true; o.settle_reason = "session-start";
        } else {
            dt = now_ms - last_ms_;
            if (dt < 0) dt = 0;
            if (dt > kMaxStepMs) dt = kMaxStepMs;
            if (generation != gen_) {
                gen_ = generation; cur_ = Angles{}; settle_until_ = now_ms + kSettleMs; ++gen_changes_;
                o.settle_start = true; o.settle_reason = "local-generation-change";
            }
        }
        last_ms_ = now_ms;
        Command c;
        if (mb && mb->read(seen_, c)) {
            target_ = Angles{c.yaw, c.pitch, c.roll}; hold_ms_ = int64_t(std::llround(c.hold_s * 1000)); hold_until_ = -1;
            phase_ = is_zero(target_) ? Phase::returning : Phase::slewing;
            ++commands_; o.command = true; o.cmd = c;
        }
        ++frames_;
        const bool settling = now_ms < settle_until_;
        if (!settling) {
            const double e = std::fmax(std::fabs(target_.yaw - cur_.yaw), std::fmax(std::fabs(target_.pitch - cur_.pitch), std::fabs(target_.roll - cur_.roll)));
            const double stepd = dps_ * double(dt) / 1000.0;
            if (e <= stepd || e < 1e-9) cur_ = target_;
            else {
                const double k = stepd / e;
                cur_.yaw += (target_.yaw - cur_.yaw) * k; cur_.pitch += (target_.pitch - cur_.pitch) * k; cur_.roll += (target_.roll - cur_.roll) * k;
            }
            const bool at = cur_.yaw == target_.yaw && cur_.pitch == target_.pitch && cur_.roll == target_.roll;
            if (at && phase_ == Phase::slewing) {
                if (is_zero(target_)) { phase_ = Phase::idle; o.home = true; }
                else { phase_ = Phase::holding; o.reached = true; ++reached_; hold_until_ = hold_ms_ > 0 ? now_ms + hold_ms_ : -1; }
            } else if (at && phase_ == Phase::returning) { phase_ = Phase::idle; o.home = true; }
            if (phase_ == Phase::holding && hold_until_ >= 0 && now_ms >= hold_until_) {
                target_ = Angles{}; phase_ = Phase::returning; hold_until_ = -1; o.returning = true;
            }
            if (!is_zero(cur_)) ++turned_frames_;
        }
        o.settling = settling;
        o.cur = settling ? Angles{} : cur_;
        o.target = target_; o.phase = phase_;
        if (now_ms >= next_summary_) { o.summary = true; next_summary_ = now_ms + kSummaryMs; }
        return o;
    }
    double slew_dps() const noexcept { return dps_; }
    unsigned commands() const noexcept { return commands_; }
    unsigned reached() const noexcept { return reached_; }
    unsigned generation_changes() const noexcept { return gen_changes_; }
    uint64_t frames() const noexcept { return frames_; }
    uint64_t turned_frames() const noexcept { return turned_frames_; }
    uint32_t generation() const noexcept { return gen_; }
private:
    double dps_; uint32_t seen_;
    Angles cur_, target_; Phase phase_ = Phase::idle;
    bool started_ = false; uint32_t gen_ = 0; int64_t settle_until_ = 0, last_ms_ = 0, hold_ms_ = 0, hold_until_ = -1, next_summary_ = 0;
    unsigned commands_ = 0, reached_ = 0, gen_changes_ = 0; uint64_t frames_ = 0, turned_frames_ = 0;
};

// ---- pose composition ----------------------------------------------------------------------------------------------------------------------

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

inline void quat_mul(const double a[4], const double b[4], double o[4]) noexcept {   // xyzw, o = a*b (b applied first)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// R = Ry(yaw) Rx(pitch) Rz(roll) in LOCAL: turn (about world up), then look up/down about the turned head's right axis, then tilt.
inline void quat_from_angles(const Angles& a, double q[4]) noexcept {
    const double y = a.yaw * kDegToRad * 0.5, p = a.pitch * kDegToRad * 0.5, r = a.roll * kDegToRad * 0.5;
    const double qy[4] = {0, std::sin(y), 0, std::cos(y)}, qx[4] = {std::sin(p), 0, 0, std::cos(p)}, qz[4] = {0, 0, std::sin(r), std::cos(r)};
    double t[4]; quat_mul(qy, qx, t); quat_mul(t, qz, q);
}
inline void rotate(const double q[4], const double v[3], double o[3]) noexcept {   // o = q v q*
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}
inline bool finite_pose(const xr_owner::Pose& p) noexcept {
    for (float f : p.position) if (!std::isfinite(f)) return false;
    for (float f : p.orientation) if (!std::isfinite(f)) return false;
    return true;
}
inline void turn_pose(const double q[4], const double pivot[3], xr_owner::Pose& p) noexcept {
    const double d[3] = {p.position[0] - pivot[0], p.position[1] - pivot[1], p.position[2] - pivot[2]};
    double r[3]; rotate(q, d, r);
    for (int k = 0; k < 3; ++k) p.position[k] = float(pivot[k] + r[k]);
    const double o[4] = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
    double n[4]; quat_mul(q, o, n);
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
    if (len > 0) for (int k = 0; k < 4; ++k) p.orientation[k] = float(n[k] / len);
}

// Turns the located eyes (and the VIEW-space head) about the head point by `a` (left-multiplied in LOCAL). The pivot is the VIEW-space head
// position when it is usable, else the eye midpoint. Identity angles change nothing (byte-identical). Returns true when something was rotated.
inline bool apply(const Angles& a, bool views_ok, xr_owner::ViewSample& v, bool head_ok, xr_owner::HeadSample& h) noexcept {
    if (is_zero(a)) return false;
    const bool head_use = head_ok && finite_pose(h.pose);
    const bool eyes_use = views_ok && v.view_count == 2 && finite_pose(v.eyes[0].pose) && finite_pose(v.eyes[1].pose);
    if (!head_use && !eyes_use) return false;
    double pivot[3];
    for (int k = 0; k < 3; ++k) pivot[k] = head_use ? double(h.pose.position[k]) : 0.5 * (double(v.eyes[0].pose.position[k]) + double(v.eyes[1].pose.position[k]));
    double q[4]; quat_from_angles(a, q);
    if (eyes_use) for (int e = 0; e < 2; ++e) turn_pose(q, pivot, v.eyes[e].pose);
    if (head_use) turn_pose(q, pivot, h.pose);
    return true;
}

// ---- log lines ------------------------------------------------------------------------------------------------------------------------------

inline std::string format_config(const Options& o, bool ok, const std::string& why) {
    char b[320];
    std::snprintf(b, sizeof b, "xr-stereo;event=null-head-config;ok=%u;%s;detail=%s;trigger=%%LOCALAPPDATA%%\\acz-vr\\head.now(yaw pitch [hold_s [roll]]);"
        "sign=yaw+left,pitch+up;settle-ms=%lld;test-only=1;no-headset-claim", ok ? 1u : 0u, describe(o).c_str(), ok ? "ok" : why.c_str(),
        static_cast<long long>(kSettleMs));
    return b;
}
inline std::string format_command(const Command& c, bool accepted, const std::string& why, const char* file_state) {
    char b[320];
    if (accepted)
        std::snprintf(b, sizeof b, "xr-stereo;event=null-head;stage=command;yaw=%.1f;pitch=%.1f;roll=%.1f;hold-s=%.1f;trigger=head.now(%s)",
            c.yaw, c.pitch, c.roll, c.hold_s, file_state);
    else
        std::snprintf(b, sizeof b, "xr-stereo;event=null-head;stage=refused;detail=%s;trigger=head.now(%s)", why.c_str(), file_state);
    return b;
}
// One line per state change seen by the owner thread (command taken, reached, returning, home, settle start); empty when there is none.
inline std::string format_change(const StepOut& o, int64_t now_ms) {
    const char* stage = o.settle_start ? "settle" : o.reached ? "reached" : o.returning ? "returning" : o.home ? "home" : o.command ? "slew-start" : nullptr;
    if (!stage) return std::string();
    char b[320];
    std::snprintf(b, sizeof b, "xr-stereo;event=null-head;stage=%s;reason=%s;cur=%.1f,%.1f,%.1f;target=%.1f,%.1f,%.1f;phase=%s;t-ms=%lld",
        stage, o.settle_start ? o.settle_reason : "-", o.cur.yaw, o.cur.pitch, o.cur.roll, o.target.yaw, o.target.pitch, o.target.roll,
        phase_name(o.phase), static_cast<long long>(now_ms));
    return b;
}
inline std::string format_summary(const StepOut& o, const Controller& c, uint64_t rotated_frames) {
    char b[360];
    std::snprintf(b, sizeof b, "xr-stereo;event=summary-null-head;cur=%.1f,%.1f,%.1f;target=%.1f,%.1f,%.1f;phase=%s;settling=%u;commands=%u;reached=%u;"
        "frames=%llu;turned-frames=%llu;rotated-frames=%llu;gen=%u;gen-changes=%u;slew-dps=%.0f",
        o.cur.yaw, o.cur.pitch, o.cur.roll, o.target.yaw, o.target.pitch, o.target.roll, phase_name(o.phase), o.settling ? 1u : 0u, c.commands(), c.reached(),
        static_cast<unsigned long long>(c.frames()), static_cast<unsigned long long>(c.turned_frames()), static_cast<unsigned long long>(rotated_frames),
        c.generation(), c.generation_changes(), c.slew_dps());
    return b;
}

} // namespace acz::nullhead
