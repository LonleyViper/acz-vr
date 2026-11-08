#pragma once
// VSEARLY (docs/world-render/vsearly.md; roadmap R1 prerequisite): the XR session starts in the game's menus and survives between missions, and
// the terrain stage (with its takeoff stage) is detected PER FLIGHT. Behind `vsearly:1` inside xr_terrain_stage (with vscreen:1); default OFF:
// nothing here runs and every existing path is unchanged. SDK-free and header-only: the offline tests (flight_cycle_offline_tests.cpp) drive the
// real decision code with a fake stage type, a fake clock and a synchronous loader.
//
//   * early_refusal / early_start_ready: the monitor's start gate (game presents instead of the producer's first StereoFrame).
//   * FlightCycle: per XR frame, from the age of the last FRESH flight composite: flight start (the first fresh composite while not in flight) and
//     flight end (no fresh composite for end_ms: mission end, results, briefing, hangar; also a long pause or a load longer than end_ms).
//   * FlightStage<T>: the per-flight stage slot. Flight start -> a detection/load request for that flight (run by ONE worker thread, never the
//     owner thread); the result is installed only while the same flight still runs (a late result is discarded). Flight end -> the drawn stage
//     is cleared (the consumer draws no terrain until the next flight's detection installs one). The previous stage is PARKED and reused when the
//     next detection names the same stage (no reload), released after park_ms otherwise.
//   * Lifetime (owner thread only): a stage leaves the draw path only between frames. The D3D12 backend borrows the CPU stage only inside a record
//     EXCEPT the props init worker (xr_props_d3d12.cpp PropsGpu::async_ready keeps a reference while it runs). A replaced stage WITHOUT props is
//     released at once; one WITH props is kept RETIRED until a recorded job of a NEWER stage reports its props settled (ready or failed): the
//     backend starts the next worker only after the previous one left its running state (its ready/failed state is published after its last
//     access to the borrowed runtime) and joins it first (async_ready), so that report shows no worker still uses the retired stage.
//     Generations are unique per load (the loader's job), so the backend's (pointer, generation) caches can never alias a freed stage.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "xr_start_gate.h"

namespace acz::flight {

// ------------------------------------------------------------------------------------------------ early start gate (monitor thread)
// nullptr = vsearly is effective; else the reason it is ignored (logged once). `auto_stage` = xr_terrain_stage names the stage `auto`
// (per-flight detection); a fixed stage id still starts early but keeps that one stage for the process.
inline const char* early_refusal(bool vsearly, bool vscreen, bool hud_layer, bool present_hook) noexcept {
    if (!vsearly) return "off";
    if (!vscreen) return "needs-vscreen:1";
    if (!hud_layer) return "needs-xr_hud_layer";
    if (!present_hook) return "needs-ee_camera_probe(present-hook)";
    return nullptr;
}
// The previous (vsstart:steady) gate: the game window presenting steadily before XR starts. The default is now the first Present
// (vsstart:first; xr_start_gate.h, docs/world-render/launch-mode.md 3); the proxy monitor uses acz::xr_gate::decide().
inline constexpr uint64_t kEarlyMinPresents = acz::xr_gate::kSteadyMinPresents;     // 120, ~2 s at 60 fps
inline constexpr int64_t kEarlyMinArmedMs = acz::xr_gate::kSteadyMinArmedMs;        // 3000
inline bool early_start_ready(uint64_t presents, int64_t since_armed_ms) noexcept {
    return presents >= kEarlyMinPresents && since_armed_ms >= kEarlyMinArmedMs;
}
// Source extent assumed when XR starts before any StereoFrame (eye swapchain sizing only): the game's swap chain (test 19 / step83: 2560x1440).
inline constexpr uint32_t kAssumedSrcW = 2560, kAssumedSrcH = 1440;
// The early session's frame-phase progress limit (xr_owner::Limits::frame_stall_ms; test 19 stalls reached 1968 ms).
inline constexpr int64_t kEarlyFrameStallMs = 8000;

// ------------------------------------------------------------------------------------------------ flight start / end (owner thread)
struct CycleConfig {
    int64_t end_ms = 8000;   // > vsenter and > the load stalls seen (2 s); a pause longer than this ends the flight (the next detection reuses the stage)
};
class FlightCycle {
public:
    enum class Event : uint8_t { none, start, end };
    explicit FlightCycle(CycleConfig c = {}) noexcept : c_(c) {}
    // last_new_ms: the time of the last fresh flight composite (-1 = none since the session started). Called once per XR frame.
    // alive_ms (VIEW-MODE pov:1, view_mode.h; -1 = off): the last time the game showed a flight view WITHOUT a cockpit (HUD-only, chase), which
    // publishes no producer frame: the end rule counts from the later of the two. It never starts a flight (only a fresh composite does).
    Event step(int64_t now_ms, int64_t last_new_ms, int64_t alive_ms = -1) noexcept {
        const bool fresh = last_new_ms >= 0 && last_new_ms != seen_new_;
        if (fresh) seen_new_ = last_new_ms;
        if (!in_flight_ && fresh) { in_flight_ = true; ++flight_; ++starts_; return Event::start; }
        const int64_t last = alive_ms > last_new_ms ? alive_ms : last_new_ms;
        if (in_flight_ && last_new_ms >= 0 && now_ms - last > c_.end_ms) { in_flight_ = false; ++ends_; return Event::end; }
        return Event::none;
    }
    bool in_flight() const noexcept { return in_flight_; }
    uint64_t flight() const noexcept { return flight_; }   // 1-based number of the current/last flight; 0 = none yet
    uint64_t starts() const noexcept { return starts_; }
    uint64_t ends() const noexcept { return ends_; }
    // A new XR session (a new consumer): not in flight, no composite seen; the flight numbering continues (stale loads stay recognisable).
    void reset_session() noexcept { in_flight_ = false; seen_new_ = -1; }
private:
    CycleConfig c_;
    bool in_flight_ = false;
    int64_t seen_new_ = -1;
    uint64_t flight_ = 0, starts_ = 0, ends_ = 0;
};

// ------------------------------------------------------------------------------------------------ per-flight stage slot
template <class T>
struct Loaded {
    std::shared_ptr<const T> rt;           // null = no stage for this flight (no match, tie, load failure)
    std::string id;                        // the stage id (reuse key)
    std::vector<uint64_t> gens;            // every generation the backend may see for it (mission + takeoff)
    bool has_props = false;                // the props init worker may borrow it
    std::string why;                       // the loader's reason text (logged)
    uint64_t session = 0;                  // the session whose backend may have borrowed it (set by FlightStage)
};

struct StageConfig {
    CycleConfig cycle;
    int64_t park_ms = 60000;   // a parked stage not reused within this time is released
};

template <class T>
class FlightStage {
public:
    using Ptr = std::shared_ptr<const T>;
    using Loader = std::function<Loaded<T>(uint64_t flight)>;
    struct Hooks {
        std::function<void(const Loaded<T>&, bool reused, uint64_t flight)> installed;   // owner thread (log, camera epoch, producer flags)
        std::function<void(uint64_t flight, bool had_stage)> ended;                       // owner thread (clear producer flags/dedupe)
        std::function<void(const Loaded<T>&, uint64_t flight)> no_stage;                   // owner thread (detection found nothing)
        // owner thread of the NEW session, first: the previous session may have ended in flight (loss/stall) without `ended`: clear every
        // stage-bound producer switch before the new consumer, which has no terrain, sees a frame (Astra 6458816088 P2).
        std::function<void(uint64_t flight, bool was_in_flight, bool had_stage)> rollover;
    };
    explicit FlightStage(StageConfig c = {}) : c_(c), cycle_(c.cycle) {}
    void set_hooks(Hooks h) { hooks_ = std::move(h); }

    // ---- owner thread, once per XR frame BEFORE the frame's work. `io` = the consumer's drawn stage; true = it was replaced.
    bool frame(int64_t now_ms, int64_t last_new_ms, Ptr& io, int64_t alive_ms = -1) {
        bool changed = false;
        const auto ev = cycle_.step(now_ms, last_new_ms, alive_ms);
        if (ev == FlightCycle::Event::start) request(cycle_.flight());
        else if (ev == FlightCycle::Event::end) {
            const bool had = bool(current_.rt);
            if (had) { park(std::move(current_), now_ms); current_ = Loaded<T>{}; }
            if (io) { io = nullptr; changed = true; }
            if (hooks_.ended) hooks_.ended(cycle_.flight(), had);
        }
        if (parked_.rt && now_ms - parked_at_ > c_.park_ms) { retire(std::move(parked_)); parked_ = Loaded<T>{}; ++park_expired_; }
        // A finished load (try-lock: the owner never waits on the worker).
        Loaded<T> got; uint64_t got_flight = 0; bool have = false;
        {
            std::unique_lock<std::mutex> l(m_, std::try_to_lock);
            if (l.owns_lock() && result_ready_) { got = std::move(result_); got_flight = result_flight_; result_ready_ = false; have = true; }
        }
        if (have) {
            if (!cycle_.in_flight() || got_flight != cycle_.flight()) ++discarded_;   // late: that flight is over (never drawn: freed here)
            else if (!got.rt) { ++no_stage_; if (parked_.rt) { retire(std::move(parked_)); parked_ = Loaded<T>{}; } if (hooks_.no_stage) hooks_.no_stage(got, got_flight); }
            else {
                bool reused = false;
                if (parked_.rt && parked_.id == got.id) { got = std::move(parked_); parked_ = Loaded<T>{}; reused = true; ++reused_; }
                else if (parked_.rt) { retire(std::move(parked_)); parked_ = Loaded<T>{}; }
                if (current_.rt) retire(std::move(current_));
                current_ = std::move(got);
                // A fresh load belongs to this session's backend. A REUSED stage keeps its session: if an older backend's worker could borrow it,
                // it goes to kept_ when replaced (never freed).
                if (!reused) current_.session = session_;
                io = current_.rt; changed = true; ++installs_;
                if (hooks_.installed) hooks_.installed(current_, reused, got_flight);
            }
        }
        return changed;
    }
    // ---- owner thread of a NEW session (a new consumer and D3D12 backend), before its first frame(). The drawn stage is parked (reusable by id);
    // stages the PREVIOUS backend's props worker may still borrow can no longer be proven idle by the new backend (it never joins the old
    // worker): they are kept for the process (`kept-for-process`), never freed under a possible reader.
    void begin_session(int64_t now_ms) {
        ++session_;
        ++rollovers_;
        const bool was_in_flight = cycle_.in_flight(), had = bool(current_.rt);
        cycle_.reset_session();
        // P3 (Astra 6458816088): the owner clock restarts near zero in a new session: rebase a parked stage's timestamp to this clock
        // (its 60 s park restarts here; it can never be kept for the previous session's elapsed time on top).
        if (parked_.rt) parked_at_ = now_ms;
        if (current_.rt) { park(std::move(current_), now_ms); current_ = Loaded<T>{}; }
        for (auto& e : retired_) kept_.push_back(std::move(e));
        retired_.clear();
        if (hooks_.rollover) hooks_.rollover(cycle_.flight(), was_in_flight, had);
    }
    uint64_t rollovers() const noexcept { return rollovers_; }
    // ---- owner thread, after each recorded job that carried the terrain (gen = the drawn generation).
    void recorded(uint64_t gen, bool props_job, bool props_settled) {
        if (retired_.empty() || !current_.rt || !props_job || !props_settled) return;
        bool mine = false;
        for (uint64_t g : current_.gens) mine = mine || g == gen;
        if (!mine) return;
        released_ += retired_.size();
        retired_.clear();   // this session's backend started (and first joined) this worker only after every older one left its running state
    }
    // ---- worker thread: runs one pending request (true) or nothing (false). The loader runs WITHOUT the lock.
    bool work(const Loader& load) {
        uint64_t f = 0;
        {
            std::lock_guard<std::mutex> l(m_);
            if (want_ == 0 || want_ == done_) return false;
            f = want_;
        }
        Loaded<T> r;
        try { r = load(f); } catch (...) { r = Loaded<T>{}; r.why = "loader-exception"; }
        std::lock_guard<std::mutex> l(m_);
        done_ = f;
        if (want_ != f) return true;   // a newer flight asked meanwhile: this result is stale (freed by the worker; never drawn)
        result_ = std::move(r); result_flight_ = f; result_ready_ = true;
        return true;
    }
    bool pending() { std::lock_guard<std::mutex> l(m_); return want_ != 0 && want_ != done_; }

    const FlightCycle& cycle() const noexcept { return cycle_; }
    const Loaded<T>& current() const noexcept { return current_; }
    bool parked() const noexcept { return bool(parked_.rt); }
    size_t retired() const noexcept { return retired_.size(); }
    size_t kept() const noexcept { return kept_.size(); }
    std::string summary() const {
        char t[320];
        std::snprintf(t, sizeof t, "flights=%llu;in-flight=%u;ends=%llu;installs=%llu;reused=%llu;no-stage=%llu;discarded=%llu;stage=%s;parked=%s;"
            "retired=%zu;released=%llu;released-at-once=%llu;park-expired=%llu;kept-for-process=%zu;session=%llu",
            (unsigned long long)cycle_.flight(), cycle_.in_flight() ? 1u : 0u, (unsigned long long)cycle_.ends(), (unsigned long long)installs_,
            (unsigned long long)reused_, (unsigned long long)no_stage_, (unsigned long long)discarded_, current_.rt ? current_.id.c_str() : "none",
            parked_.rt ? parked_.id.c_str() : "none", retired_.size(), (unsigned long long)released_, (unsigned long long)released_now_,
            (unsigned long long)park_expired_, kept_.size(), (unsigned long long)session_);
        return t;
    }

private:
    void request(uint64_t flight) { std::lock_guard<std::mutex> l(m_); want_ = flight; result_ready_ = false; result_ = Loaded<T>{}; }
    void park(Loaded<T>&& e, int64_t now) {
        if (parked_.rt) retire(std::move(parked_));
        parked_ = std::move(e); parked_at_ = now;
    }
    void retire(Loaded<T>&& e) {
        if (!e.rt) return;
        if (!e.has_props) { ++released_now_; return; }   // nothing outside a record borrows it: freed here, between frames
        if (e.session != session_) { kept_.push_back(std::move(e)); return; }   // an older backend's worker may borrow it: never freed
        retired_.push_back(std::move(e));
    }
    StageConfig c_;
    FlightCycle cycle_;
    Hooks hooks_;
    Loaded<T> current_, parked_;
    int64_t parked_at_ = 0;
    std::vector<Loaded<T>> retired_, kept_;
    uint64_t session_ = 0, rollovers_ = 0;
    uint64_t installs_ = 0, reused_ = 0, no_stage_ = 0, discarded_ = 0, released_ = 0, released_now_ = 0, park_expired_ = 0;
    // shared with the worker
    std::mutex m_;
    uint64_t want_ = 0, done_ = 0, result_flight_ = 0;
    bool result_ready_ = false;
    Loaded<T> result_;
};

} // namespace acz::flight
