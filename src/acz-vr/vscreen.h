#pragma once
// VSCREEN (docs/world-render/vscreen.md; D-362 roadmap item a): the WHOLE game on a virtual screen in VR.
// Sub-options of the existing key xr_terrain_stage (no new config key; default OFF = nothing below runs):
//   vscreen:1          enable: whenever no in-flight composite is young enough (menus, hangar, briefing, results, cutscenes, loading, a pause
//                      that stops the game's world draws), the game's final presented image is shown on a world-locked screen quad.
//   vsd:<m>            screen distance in metres (default 2.5, 0.8..20)
//   vsw:<m>            screen width in metres (default 3.2, 0.5..30); the height follows the captured image's aspect
//   vsy:<m>            vertical offset of the screen centre from the eye height at placement (default 0, -3..3)
//   vscurve:<0|1>      0 = flat quad (default). 1 = cylinder (radius = vsd) REQUESTED: needs XR_KHR_composition_layer_cylinder, which the
//                      instance does not enable yet; the consumer logs `curve=requested-flat(cylinder-extension-not-enabled)` and draws flat.
//   vsenter:<ms>       flight composite age after which the screen takes over (default 500 = the existing reuse limit, 150..5000)
//   vsfollow:<0|1>     1 = re-place the screen in front of the user after looking away > 50 deg for 1.5 s (default 0: placed once per entry)
//   vspov:<world|flat> world (default) = the screen only when no flight composite exists; flat = the screen ALWAYS (the game's own camera,
//                      cockpit/HUD-only/chase view, flat in VR: a comfort/alternative-POV mode). Stereo HUD-only and stereo chase: see vscreen.md 6.
//   vsearly:<0|1>      1 = start XR in the menus (at the game's presents) and detect the stage per flight: flight_cycle.h, xr_flight_stage.h,
//                      docs/world-render/vsearly.md (the proxy's `vsearly-config` line says whether it is effective).
//   vsstart:<first|steady|lazy>  when vsearly:1 starts XR (xr_start_gate.h, docs/world-render/launch-mode.md 3): first (default) = the
//                      game's first Present after Launch Game; steady = the previous gate (>= 120 presents and >= 3 s); lazy = as vsearly:0.
// Removed from the option value before the other parsers see it (like lookstick:/e6loc:); a value without them is unchanged byte for byte.
// SDK-free, header-only: options, the mode decision, the screen placement, the capture ring state machine. The D3D12 parts live in
// vscreen_capture_win.cpp (capture at the game's Present) and xr_vscreen_d3d12.cpp (copy into the XR swapchain), both DLL-only.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include "xr_start_gate.h"

namespace acz::vscreen {

enum class Pov : uint8_t { world, flat };
inline const char* pov_name(Pov p) noexcept { return p == Pov::flat ? "flat" : "world"; }

struct Options {
    bool on = false;
    float distance_m = 2.5f;
    float width_m = 3.2f;
    float y_m = 0.0f;
    bool curve = false;
    int64_t enter_ms = 500;
    bool follow = false;
    Pov pov = Pov::world;
    bool early = false;          // VSEARLY (read by the proxy monitor; vsearly.md)
    acz::xr_gate::StartMode start = acz::xr_gate::StartMode::first;   // vsstart: (launch-mode.md 3)
    bool start_given = false;    // a vsstart: token was present
    bool any_token = false;      // at least one vs* token was present
};

namespace detail {
inline bool num(const std::string& v, double lo, double hi, double& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end || !std::isfinite(d) || d < lo || d > hi) return false;
    out = d;
    return true;
}
inline bool flag(const std::string& v, bool& out) {
    if (v == "1") { out = true; return true; }
    if (v == "0") { out = false; return true; }
    return false;
}
} // namespace detail

// Removes every vscreen token from `value` (the first item, the stage id, is never an option). false + why on a bad value.
inline bool extract_options(const std::string& value, std::string& rest, Options& o, std::string& why) {
    o = Options{};
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
                double d = 0;
                ours = true;
                if (k == "vscreen") { if (!detail::flag(v, o.on)) { why = "bad value for vscreen (0|1)"; return false; } }
                else if (k == "vsd") { if (!detail::num(v, 0.8, 20.0, d)) { why = "bad value for vsd (0.8..20 m)"; return false; } o.distance_m = float(d); }
                else if (k == "vsw") { if (!detail::num(v, 0.5, 30.0, d)) { why = "bad value for vsw (0.5..30 m)"; return false; } o.width_m = float(d); }
                else if (k == "vsy") { if (!detail::num(v, -3.0, 3.0, d)) { why = "bad value for vsy (-3..3 m)"; return false; } o.y_m = float(d); }
                else if (k == "vscurve") { if (!detail::flag(v, o.curve)) { why = "bad value for vscurve (0|1)"; return false; } }
                else if (k == "vsenter") { if (!detail::num(v, 150.0, 5000.0, d)) { why = "bad value for vsenter (150..5000 ms)"; return false; } o.enter_ms = int64_t(d); }
                else if (k == "vsfollow") { if (!detail::flag(v, o.follow)) { why = "bad value for vsfollow (0|1)"; return false; } }
                else if (k == "vsearly") { if (!detail::flag(v, o.early)) { why = "bad value for vsearly (0|1)"; return false; } }
                else if (k == "vsstart") {
                    if (!acz::xr_gate::parse_start_mode(v.c_str(), o.start)) { why = "bad value for vsstart (first|steady|lazy)"; return false; }
                    o.start_given = true;
                }
                else if (k == "vspov") {
                    if (v == "world") o.pov = Pov::world;
                    else if (v == "flat") o.pov = Pov::flat;
                    else { why = "bad value for vspov (world|flat)"; return false; }
                } else ours = false;
                if (ours) o.any_token = true;
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}

inline std::string describe(const Options& o) {
    char t[360];
    std::snprintf(t, sizeof t, "vscreen=%u;vsd=%.2f;vsw=%.2f;vsy=%.2f;vscurve=%u;curve=%s;vsenter=%lld;vsfollow=%u;vspov=%s;vsearly=%u%s%s",
        o.on ? 1u : 0u, double(o.distance_m), double(o.width_m), double(o.y_m), o.curve ? 1u : 0u,
        o.curve ? "requested-flat(cylinder-extension-not-enabled)" : "flat", static_cast<long long>(o.enter_ms), o.follow ? 1u : 0u,
        pov_name(o.pov), o.early ? 1u : 0u, o.early ? ";early=see-vsearly-config(vsearly.md);vsstart=" : "",
        o.early ? acz::xr_gate::start_mode_name(o.start) : "");
    return t;
}

// ------------------------------------------------------------------------------------------------ mode decision (owner thread)
// Input per XR frame that reaches the consumer's no-new-frame path (Output::pace). flight_age_ms < 0 = no flight composite since XR start.
struct ModeIn {
    int64_t now_ms = 0;
    int64_t flight_age_ms = -1;
    bool screen_image = false;   // a captured image was ever copied into the screen swapchain (the layer can be submitted)
};
enum class Mode : uint8_t { flight, screen };
inline const char* mode_name(Mode m) noexcept { return m == Mode::screen ? "screen" : "flight"; }
struct ModeOut {
    Mode mode = Mode::flight;
    bool want_capture = false;   // ask the Present-side capture to run (it stops on its own ~1 s after the last request)
    bool entered = false;        // this frame switched flight -> screen (place the screen)
};
// The capture is requested a little BEFORE the screen takes over so the first screen frame already has an image.
inline constexpr int64_t kWantLeadMs = 350;
class ModeMachine {
public:
    explicit ModeMachine(const Options& o) : o_(o) {}
    // Called ONLY from the no-new-frame path. A fresh flight composite never reaches it: it leaves the screen implicitly (Output::transact
    // composes it); the next no-new-frame call then sees a young age and reports flight again (exit = the first fresh flight frame).
    ModeOut step(const ModeIn& in) noexcept {
        ModeOut r;
        const bool stale = in.flight_age_ms < 0 || in.flight_age_ms > o_.enter_ms;
        const bool want = o_.pov == Pov::flat || in.flight_age_ms < 0 || in.flight_age_ms > std::max<int64_t>(0, o_.enter_ms - kWantLeadMs);
        r.want_capture = want;
        const Mode next = (o_.pov == Pov::flat || stale) ? Mode::screen : Mode::flight;
        r.entered = next == Mode::screen && mode_ != Mode::screen;
        if (next != mode_) { ++transitions_; mode_ = next; since_ms_ = in.now_ms; }
        r.mode = mode_;
        return r;
    }
    // Output::transact composed a fresh flight frame (vspov:world): the mode is flight from now on.
    void flight_frame(int64_t now_ms) noexcept {
        if (o_.pov == Pov::flat) return;
        if (mode_ != Mode::flight) { ++transitions_; mode_ = Mode::flight; since_ms_ = now_ms; }
    }
    Mode mode() const noexcept { return mode_; }
    uint64_t transitions() const noexcept { return transitions_; }
    int64_t since_ms() const noexcept { return since_ms_; }
private:
    Options o_;
    Mode mode_ = Mode::flight;
    uint64_t transitions_ = 0;
    int64_t since_ms_ = 0;
};

// ------------------------------------------------------------------------------------------------ placement (OpenXR LOCAL: +Y up, -Z forward)
struct Pose { float p[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };   // xyz, xyzw
struct Placement {
    bool valid = false;
    Pose pose;                  // quad centre; its +Z (the visible face) points back at the viewer
    float width = 0, height = 0;
    float yaw = 0;              // radians about +Y (0 = facing -Z)
    uint32_t generation = 0;    // LOCAL space generation it was placed in
    // cylinder parameters (vscurve:1, not submitted yet): radius = distance, central angle = width / radius, centre = the viewer position
    float cyl_radius = 0, cyl_angle = 0, cyl_aspect = 0;
    float cyl_centre[3] = {0, 0, 0};
};
// UI3D (ui-3d.md 4): the screen placement the XR consumer (ScreenCore) uses, published for the game-thread producer so the UI3D layers keep the
// directions of the flat screen as seen from the head position it was placed from. generation = ENCODED LOCAL space generation (0 = unknown).
struct SharedPlacement { bool valid = false; float centre[3] = {0, 0, 0}; float yaw = 0, width = 0, height = 0; double anchor[3] = {0, 0, 0};
                         uint32_t generation = 0; uint64_t serial = 0; };
namespace detail {
inline std::mutex& placement_mutex() noexcept { static std::mutex m; return m; }
inline SharedPlacement& placement_store() noexcept { static SharedPlacement p; return p; }
}
inline void publish_placement(const Placement& pl, const float head[3], uint32_t encoded_generation) noexcept {
    std::lock_guard<std::mutex> l(detail::placement_mutex());
    SharedPlacement& s = detail::placement_store();
    s.valid = pl.valid;
    for (int i = 0; i < 3; ++i) { s.centre[i] = pl.pose.p[i]; s.anchor[i] = head[i]; }
    s.yaw = pl.yaw; s.width = pl.width; s.height = pl.height; s.generation = encoded_generation; ++s.serial;
}
inline SharedPlacement read_placement() noexcept { std::lock_guard<std::mutex> l(detail::placement_mutex()); return detail::placement_store(); }
inline void clear_placement() noexcept { std::lock_guard<std::mutex> l(detail::placement_mutex()); detail::placement_store() = SharedPlacement{}; }
// Head yaw of a pose: the forward (-Z) direction projected on the horizontal plane. Pitch/roll are ignored (a level screen).
inline float head_yaw(const float q[4]) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // forward = R * (0,0,-1)
    const float fx = -(2.0f * (x * z + w * y));
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    if (!(std::fabs(fx) > 1e-6f || std::fabs(fz) > 1e-6f)) return 0.0f;   // looking straight up/down: keep yaw 0
    return std::atan2(-fx, -fz);   // yaw 0 = forward -Z; positive = turned left (counter-clockwise seen from above)
}
inline float wrap_pi(float a) noexcept {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
// aspect = image width / height (> 0).
inline Placement place(const Options& o, const float head_pos[3], const float head_q[4], float aspect, uint32_t generation) noexcept {
    Placement pl;
    if (!(aspect > 0.1f && aspect < 10.0f)) aspect = 16.0f / 9.0f;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(head_pos[i])) return pl;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(head_q[i])) return pl;
    const float yaw = head_yaw(head_q);
    const float fx = -std::sin(yaw), fz = -std::cos(yaw);   // forward at that yaw
    pl.yaw = yaw;
    pl.pose.p[0] = head_pos[0] + fx * o.distance_m;
    pl.pose.p[1] = head_pos[1] + o.y_m;
    pl.pose.p[2] = head_pos[2] + fz * o.distance_m;
    pl.pose.q[0] = 0.0f; pl.pose.q[1] = std::sin(yaw * 0.5f); pl.pose.q[2] = 0.0f; pl.pose.q[3] = std::cos(yaw * 0.5f);
    pl.width = o.width_m;
    pl.height = o.width_m / aspect;
    pl.generation = generation;
    pl.cyl_radius = o.distance_m;
    pl.cyl_angle = std::min(o.width_m / o.distance_m, 6.2831f);
    pl.cyl_aspect = aspect;
    pl.cyl_centre[0] = head_pos[0]; pl.cyl_centre[1] = head_pos[1] + o.y_m; pl.cyl_centre[2] = head_pos[2];
    pl.valid = true;
    return pl;
}
// vsfollow:1 - re-place after the head stayed > kFollowDeg away from the screen's yaw for kFollowMs.
inline constexpr float kFollowDeg = 50.0f;
inline constexpr int64_t kFollowMs = 1500;
class Follow {
public:
    // true = re-place now.
    bool step(bool enabled, float head_yaw_rad, float screen_yaw_rad, int64_t now_ms) noexcept {
        if (!enabled) { away_since_ = -1; return false; }
        const float off = std::fabs(wrap_pi(head_yaw_rad - screen_yaw_rad)) * 57.2957795f;
        if (off <= kFollowDeg) { away_since_ = -1; return false; }
        if (away_since_ < 0) { away_since_ = now_ms; return false; }
        if (now_ms - away_since_ >= kFollowMs) { away_since_ = -1; return true; }
        return false;
    }
private:
    int64_t away_since_ = -1;
};

// ------------------------------------------------------------------------------------------------ capture ring (game Present thread <-> XR owner thread)
// Slots of captured images. The WRITER (Present thread) never blocks: it uses try_lock and skips the capture when the lock is busy. A slot is
// re-recorded only when it is FREE (never HELD by the reader) and the writer's previous GPU use of it has completed on the capture fence
// (value <= completed). The READER acquires only a READY slot whose capture completed (CPU-observed; it never makes a queue wait), keeps it HELD
// while its own GPU copy may read it, and releases it after that copy's completion was observed.
inline constexpr unsigned kRingSlots = 3;
// A fence observation (Astra review 6455891169 finding 2): GetCompletedValue() returns UINT64_MAX on device removal. That is LOSS, never a
// timeline value: every decision takes a Completion, and a lost one refuses reuse, acquisition, release and re-creation.
struct Completion {
    bool lost = false;
    uint64_t value = 0;
    bool reached(uint64_t v) const noexcept { return !lost && value >= v; }
};
inline Completion completion_from_raw(uint64_t raw) noexcept { Completion c; if (raw == UINT64_MAX) c.lost = true; else c.value = raw; return c; }
enum SlotState : uint8_t { slot_free = 0, slot_writing = 1, slot_ready = 2, slot_held = 3 };
struct RingSlot { SlotState state = slot_free; uint64_t id = 0; uint64_t value = 0; };
struct Acquired { int slot = -1; uint64_t id = 0; uint64_t value = 0; };
class Ring {
public:
    // Writer: a slot to record into, or -1 (lock busy, or every slot held / not yet completed). `completed` = the capture fence's completed value.
    int begin_write(const Completion& c) noexcept {
        std::unique_lock<std::mutex> l(m_, std::try_to_lock);
        if (!l.owns_lock()) { ++busy_; return -1; }
        if (c.lost) { ++lost_refused_; return -1; }
        const uint64_t completed = c.value;
        int best = -1;
        for (unsigned i = 0; i < kRingSlots; ++i) {
            const RingSlot& s = s_[i];
            const bool reusable = s.state == slot_free || (s.state == slot_ready && s.id != latest_);
            if (!reusable || s.value > completed) continue;
            if (best < 0 || s_[size_t(best)].id > s.id) best = int(i);   // the oldest
        }
        if (best < 0) { ++no_slot_; return -1; }
        s_[size_t(best)].state = slot_writing;
        return best;
    }
    // Writer: the recording of `slot` was submitted and the capture fence will reach `value` when it is done.
    void publish(int slot, uint64_t value) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (slot < 0 || unsigned(slot) >= kRingSlots || s_[size_t(slot)].state != slot_writing) return;
        RingSlot& s = s_[size_t(slot)];
        s.state = slot_ready; s.id = ++next_id_; s.value = value; latest_ = s.id; ++published_;
    }
    // Writer: nothing was submitted for `slot` (its previous `value` stays: the earlier use may still be in flight).
    void abort(int slot) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (slot < 0 || unsigned(slot) >= kRingSlots || s_[size_t(slot)].state != slot_writing) return;
        s_[size_t(slot)].state = slot_free; s_[size_t(slot)].id = 0; ++aborted_;
    }
    // Writer: the recording of `slot` was EXECUTED (fence value `value`) but must never be shown (UI3D: the frame was refused after some replays
    // were recorded). The slot becomes free but is not handed out for writing again before `value` completed (begin_write's value check).
    void retire_unpublished(int slot, uint64_t value) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (slot < 0 || unsigned(slot) >= kRingSlots || s_[size_t(slot)].state != slot_writing) return;
        RingSlot& s = s_[size_t(slot)];
        s.state = slot_free; s.id = 0; if (value > s.value) s.value = value; ++aborted_;
    }
    // Reader: the newest READY slot newer than `after` whose capture completed (value <= completed) -> HELD.
    bool acquire_latest(uint64_t after, const Completion& c, Acquired& out) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (c.lost) { ++lost_refused_; return false; }
        const uint64_t completed = c.value;
        int best = -1;
        for (unsigned i = 0; i < kRingSlots; ++i) {
            const RingSlot& s = s_[i];
            if (s.state != slot_ready || s.id <= after || s.value > completed) continue;
            if (best < 0 || s.id > s_[size_t(best)].id) best = int(i);
        }
        if (best < 0) return false;
        RingSlot& s = s_[size_t(best)];
        s.state = slot_held;
        out.slot = best; out.id = s.id; out.value = s.value;
        ++acquired_;
        return true;
    }
    // Reader: its GPU use of the slot completed.
    void release(const Acquired& a) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (a.slot < 0 || unsigned(a.slot) >= kRingSlots) return;
        RingSlot& s = s_[size_t(a.slot)];
        if (s.state == slot_held && s.id == a.id) { s.state = slot_free; ++released_; }
    }
    // Writer, before re-creating the slot resources (size/format change): ATOMICALLY checks that no slot is HELD or WRITING and that every
    // capture completed, and then forgets every READY image (none can be acquired afterwards). false = not now (nothing changed).
    bool retire_all(const Completion& c) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (c.lost) { ++lost_refused_; return false; }
        for (const auto& s : s_) if (s.state == slot_held || s.state == slot_writing || s.value > c.value) return false;
        for (auto& s : s_) { s.state = slot_free; s.id = 0; }
        latest_ = 0;
        return true;
    }
    struct Stats { uint64_t published, acquired, released, aborted, busy, no_slot, lost_refused; };
    Stats stats() noexcept { std::lock_guard<std::mutex> l(m_); return {published_, acquired_, released_, aborted_, busy_, no_slot_, lost_refused_}; }
    RingSlot slot(unsigned i) noexcept { std::lock_guard<std::mutex> l(m_); return i < kRingSlots ? s_[i] : RingSlot{}; }
private:
    std::mutex m_;
    RingSlot s_[kRingSlots];
    uint64_t next_id_ = 0, latest_ = 0;
    uint64_t published_ = 0, acquired_ = 0, released_ = 0, aborted_ = 0, busy_ = 0, no_slot_ = 0, lost_refused_ = 0;
};

// Swap chain -> command queue association (Astra review finding 3). IDXGISwapChain::GetDevice is NOT a queue lookup: the DXGI factory wrappers
// (d3d12_hook.cpp) record the D3D12 queue the GAME passed as pDevice at a SUCCESSFUL creation (only when it really is a DIRECT queue) together
// with that queue's device. The capture uses only a recorded queue, and only after re-checking (on use) that the queue's device is still the
// recorded one; no association = no capture (never a substitute queue of the same device). Pointers are opaque here (SDK-free).
class QueueAssoc {
public:
    static constexpr unsigned kCap = 8;
    // A re-created swap chain at a recycled address replaces the old entry; when full, the oldest entry is dropped.
    void note(const void* swapchain, void* queue, void* device) noexcept {
        if (!swapchain || !queue || !device) return;
        std::lock_guard<std::mutex> l(m_);
        for (auto& e : e_) if (e.sc == swapchain) { e.queue = queue; e.device = device; e.seq = ++seq_; return; }
        Entry* slot = &e_[0];
        for (auto& e : e_) { if (!e.sc) { slot = &e; break; } if (e.seq < slot->seq) slot = &e; }
        *slot = Entry{swapchain, queue, device, ++seq_};
    }
    bool find(const void* swapchain, void*& queue, void*& device) noexcept {
        std::lock_guard<std::mutex> l(m_);
        for (const auto& e : e_) if (e.sc && e.sc == swapchain) { queue = e.queue; device = e.device; return true; }
        return false;
    }
private:
    struct Entry { const void* sc = nullptr; void* queue = nullptr; void* device = nullptr; uint64_t seq = 0; };
    std::mutex m_;
    Entry e_[kCap];
    uint64_t seq_ = 0;
};
// The capture's use-time validation: the recorded queue is used only when its CURRENT device (queue->GetDevice) is the recorded device.
// `queue_device` performs that lookup (null = failed). Returns the queue, or nullptr with `why`.
template <class QueueDeviceFn>
void* resolve_queue(QueueAssoc& a, const void* swapchain, QueueDeviceFn&& queue_device, const char*& why) noexcept {
    void* q = nullptr; void* d = nullptr;
    if (!a.find(swapchain, q, d)) { why = "no-swapchain-queue-association"; return nullptr; }
    void* now = queue_device(q);
    if (!now) { why = "queue-device-lookup-failed"; return nullptr; }
    if (now != d) { why = "queue-device-mismatch"; return nullptr; }
    return q;
}

// The capture runs only while requested: the reader stamps `want_until` (now + kWantHoldMs) on every frame that wants it.
inline constexpr int64_t kWantHoldMs = 1000;
inline bool capture_wanted(int64_t want_until_ms, int64_t now_ms) noexcept { return want_until_ms > 0 && now_ms <= want_until_ms; }

} // namespace acz::vscreen
