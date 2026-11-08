// Offline tests for the in-proxy stereo consumer (xr_stereo_output) driven by
// the REAL xr_owner::Owner in continuous mode. Everything foreign is a fake:
// XR runtime + swapchains (extended copy of the xr_owner_offline_tests fake),
// D3D12 blit backend with per-resource state tracking, a FrameSource with a
// ring of kSlots fake slots, and a fake clock. No loader, device, game or HMD.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "xr_stereo_output.h"
#include "hud_late.h"   // HUD-LATE subtitle shot request
#include "lens_flare.h"   // LENS-FLARE telemetry cases
#include "xr_vscreen_core.h"   // VSCREEN + Output sequences (--vscreen)
#include "gpu_copy_step5.h"
#include "gpu_copy_png.h"
#include "xr_stereo_log.h"
#include "eye_ring.h"   // EYE-RING: the producer channel Output::setup publishes into (--eye-res)
#define ACZ_FEATURE_LOG_FIXTURE_ONLY
#include "feature_log_offline_tests.cpp"
#include "runtime_config.h"
#include "xr_probe/format_policy.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace acz;
using namespace acz::xr_owner;
using acz::xr_stereo::BlitMapping;

namespace {
unsigned checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL " << what << '\n'; std::exit(1); }
}

struct FakeClock final : Clock {
    std::atomic<int64_t> t{0};
    int64_t now_ms() override { return t.load(); }
    void sleep_ms(int64_t ms) override { check(ms >= 0 && ms <= Limits{}.poll_ms, "sleep bounded"); t += ms; }
};

// One shared, ordered call log for every fake.
struct Journal {
    std::mutex m;
    std::vector<std::string> log;
    void add(const std::string& s) { std::lock_guard<std::mutex> g(m); log.push_back(s); }
    std::vector<std::string> calls() { std::lock_guard<std::mutex> g(m); return log; }
    unsigned count(const std::string& s) { std::lock_guard<std::mutex> g(m); return unsigned(std::count(log.begin(), log.end(), s)); }
    unsigned prefix(const std::string& p) {
        std::lock_guard<std::mutex> g(m); unsigned n = 0;
        for (auto& s : log) n += s.compare(0, p.size(), p) == 0;
        return n;
    }
    long index(const std::string& s, size_t from = 0) {
        std::lock_guard<std::mutex> g(m);
        for (size_t i = from; i < log.size(); ++i) if (log[i] == s) return long(i);
        return -1;
    }
};

Event state_event(SessionState s) { Event e; e.type = EventType::session_state; e.state = s; return e; }

// ---------------------------------------------------------------- XR fake ---
struct FakeXr final : Runtime, xr_stereo::SwapchainRuntime {
    Journal& j; FakeClock& clock;
    FakeXr(Journal& journal, FakeClock& c) : j(journal), clock(c) {
        sys.hmd_system = true; sys.primary_stereo = true; sys.view_count = 2; sys.blend_count = 1; sys.blend_modes[0] = blend_opaque;
        req = {7, 3, 0xb000}; bind = {true, true, 7, 3, 0xc100, true, true}; spaces = {true, true};
        views.view_count = 2; views.view_state_flags = valid_bits | tracked_bits;
        for (int i = 0; i < 2; ++i) {
            views.eyes[i].pose.position[0] = i ? 0.032f : -0.032f; views.eyes[i].pose.position[1] = 1.2f;
            views.eyes[i].fov = {-0.9f, 0.8f, 0.85f, -0.95f};
        }
        head.location_flags = valid_bits | tracked_bits; head.pose.position[1] = 1.2f;
        cfg[0] = cfg[1] = {2688, 2688, 4096, 4096, 1};
    }
    std::map<std::string, std::deque<Rc>> script;
    std::map<std::string, std::function<void()>> hook;
    std::deque<Event> events;
    SystemFacts sys; GraphicsRequirements req; BindingFacts bind; SpaceFacts spaces; ViewSample views; HeadSample head;
    std::array<xr_stereo::ViewConfig, 2> cfg;
    std::vector<int64_t> offered{2, 28, 29, 87};
    uint32_t image_count = 3;
    uint32_t expect_w = 2560, expect_h = 1440;
    uint32_t expect_eye_w = 0, expect_eye_h = 0;   // EYE-RES: when set, the eye swapchains (0/1) must have this size instead
    bool should_render = true; unsigned waits = 0;
    int64_t period = 11111111;
    bool late_available = false;
    double late_ms = 0;
    bool end_lateness_ms(double& value) const override { value = late_ms; return late_available; }
    std::vector<ProjectionLayer> layers; std::vector<int64_t> layer_times;
    // swapchain state
    struct Chain { bool live = false, acquired = false, waited = false; uint32_t next = 0, index = 0; unsigned destroyed = 0; };
    std::array<Chain, 5> chains;       // 0/1 eyes, 2 world quad, 3 HUD quad, 4 marker quad
    std::deque<bool> wait_timeouts[5]; // scripted per-target timeouts, consumed in order
    unsigned violations = 0;

    Rc enter(const std::string& op) {
        j.add(op);
        Rc rc = Rc::ok;
        auto it = script.find(op);
        if (it != script.end() && !it->second.empty()) { rc = it->second.front(); it->second.pop_front(); }
        auto h = hook.find(op);
        if (h != hook.end() && h->second) h->second();
        return rc;
    }
    Rc create_instance() override { return enter("create_instance"); }
    Rc query_system(SystemFacts& o) override { Rc rc = enter("query_system"); o = sys; return rc; }
    Rc graphics_requirements(GraphicsRequirements& o) override { Rc rc = enter("graphics_requirements"); o = req; return rc; }
    Rc query_binding(BindingFacts& o) override { Rc rc = enter("query_binding"); o = bind; return rc; }
    Rc create_session() override {
        Rc rc = enter("create_session");
        if (succeeded(rc)) { events.push_back(state_event(SessionState::idle)); events.push_back(state_event(SessionState::ready)); }
        return rc;
    }
    Rc query_spaces(SpaceFacts& o) override { Rc rc = enter("query_spaces"); o = spaces; return rc; }
    Rc create_local_space() override { return enter("create_local_space"); }
    Rc create_view_space() override { return enter("create_view_space"); }
    Rc poll_event(Event& e) override {
        Rc rc = Rc::ok; // not journaled: one poll batch per frame would drown the order checks
        auto it = script.find("poll_event");
        if (it != script.end() && !it->second.empty()) { rc = it->second.front(); it->second.pop_front(); }
        if (rc != Rc::ok) return rc;
        if (events.empty()) return Rc::event_unavailable;
        e = events.front(); events.pop_front(); return Rc::ok;
    }
    Rc begin_session() override {
        Rc rc = enter("begin_session");
        if (succeeded(rc)) { events.push_back(state_event(SessionState::synchronized)); events.push_back(state_event(SessionState::focused)); }
        return rc;
    }
    Rc wait_frame(FrameTiming& t) override {
        Rc rc = enter("wait_frame");
        clock.t += 11;
        ++waits;
        t.predicted_time = 1000000000 + int64_t(waits) * period; t.predicted_period = period; t.should_render = should_render;
        return rc;
    }
    Rc begin_frame() override { return enter("begin_frame"); }
    Rc locate_views(int64_t, ViewSample& v) override { Rc rc = enter("locate_views"); v = views; return rc; }
    Rc locate_head(int64_t, HeadSample& h) override { Rc rc = enter("locate_head"); h = head; return rc; }
    Rc end_frame_zero_layers(int64_t, uint32_t) override { return enter("end_frame"); }
    Rc end_frame_projection(int64_t t, uint32_t, const ProjectionLayer& l) override {
        // A layer may only reference swapchains with no outstanding acquisition.
        if (chains[0].acquired || chains[1].acquired || chains[2].acquired || chains[3].acquired || chains[4].acquired || !chains[0].live || !chains[1].live) ++violations;
        if (l.quad && !chains[2].live) ++violations;
        if (l.hud && !chains[3].live) ++violations;
        if (l.markers && !chains[4].live) ++violations;
        Rc rc = enter("end_frame_projection");
        layers.push_back(l); layer_times.push_back(t);
        return rc;
    }
    Rc request_exit() override {
        Rc rc = enter("request_exit");
        if (succeeded(rc)) events.push_back(state_event(SessionState::stopping));
        return rc;
    }
    Rc end_session() override { return enter("end_session"); }
    Rc destroy_view_space() override { return enter("destroy_view_space"); }
    Rc destroy_local_space() override { return enter("destroy_local_space"); }
    Rc destroy_session() override {
        if (chains[0].live || chains[1].live || chains[2].live || chains[3].live || chains[4].live) j.add("destroy_session.with-live-swapchain");
        return enter("destroy_session");
    }
    Rc release_binding() override { return enter("release_binding"); }
    Rc destroy_instance() override { return enter("destroy_instance"); }
    // swapchains
    Rc view_configs(std::array<xr_stereo::ViewConfig, 2>& o) override { Rc rc = enter("xr.view_configs"); o = cfg; return rc; }
    Rc formats(std::array<int64_t, xr_stereo::max_formats>& o, uint32_t& n) override {
        Rc rc = enter("xr.formats"); n = uint32_t(offered.size());
        for (size_t i = 0; i < offered.size() && i < o.size(); ++i) o[i] = offered[i];
        return rc;
    }
    Rc create_swapchain(unsigned eye, int64_t format, uint32_t w, uint32_t h, xr_stereo::SwapchainImages& out) override {
        Rc rc = enter("xr.create " + std::to_string(eye));
        const bool eye_rule = eye < 2 && expect_eye_w; // EYE-RES
        if (format != 29 || w != (eye_rule ? expect_eye_w : expect_w) || h != (eye_rule ? expect_eye_h : expect_h)) ++violations; // source-aspect policy, not the recommended size (EYE-RES: the eyes may follow it)
        if (succeeded(rc)) {
            chains[eye].live = true; out.count = image_count;
            for (uint32_t i = 0; i < image_count && i < out.images.size(); ++i) out.images[i] = reinterpret_cast<void*>(uintptr_t(0x1000 + eye * 0x100 + i));
        }
        return rc;
    }
    Rc acquire(unsigned eye, uint32_t& index) override {
        auto& c = chains[eye];
        if (!c.live || c.acquired) ++violations;
        Rc rc = enter("xr.acquire " + std::to_string(eye));
        if (succeeded(rc)) { c.acquired = true; c.waited = false; c.index = c.next; c.next = (c.next + 1) % image_count; index = c.index; }
        return rc;
    }
    Rc wait(unsigned eye, int64_t timeout, bool& timed_out) override {
        auto& c = chains[eye];
        if (!c.acquired || c.waited) ++violations;
        Rc rc = enter("xr.wait " + std::to_string(eye) + " " + std::to_string(timeout));
        timed_out = false;
        if (!wait_timeouts[eye].empty()) { timed_out = wait_timeouts[eye].front(); wait_timeouts[eye].pop_front(); }
        if (succeeded(rc) && !timed_out) c.waited = true;
        return rc;
    }
    Rc release(unsigned eye) override {
        auto& c = chains[eye];
        if (!c.acquired || !c.waited) ++violations; // release only after a successful wait
        Rc rc = enter("xr.release " + std::to_string(eye));
        if (succeeded(rc)) { c.acquired = false; c.waited = false; }
        return rc;
    }
    Rc destroy_swapchain(unsigned eye) override {
        Rc rc = enter("xr.destroy " + std::to_string(eye));
        if (succeeded(rc)) { chains[eye].live = false; ++chains[eye].destroyed; }
        return rc;
    }
};

// ---------------------------------------------------------- D3D12 fake ---
enum class St { rt, psr, dw, other };
struct FakeBlit final : xr_stereo::BlitBackend {
    Journal& j;
    explicit FakeBlit(Journal& journal) : j(journal) {}
    std::map<void*, St>* states = nullptr; // the producer's eye resources
    bool setup_ok = true, record_ok = true, restore = true, close_ok = true, wait_ok = true, signal_ok = true, sample_ok = true, destroy_ok = true;
    bool execute_ok = true, execute_dispatch = true;
    bool producer_complete = false;      // CPU view of the producer fence
    unsigned complete_after = 1;         // samples after a signal before completion is visible
    bool never_complete = false;
    uint64_t signalled = 0, completed = 0; unsigned samples_since = 0;
    bool executing = false;              // GPU work in flight (submitted, not complete)
    unsigned destroys = 0;
    std::function<void()> signal_hook; // simulate time spent after GPU dispatch
    std::vector<std::array<BlitMapping, xr_stereo::kTargets>> maps;
    std::vector<xr_stereo::BlitJob> jobs;
    bool depth_unreadable = false;
    bool hud_unreadable = false;       // backend refuses the HUD source (cleared, not drawn)
    bool markers_unreadable = false;   // J-hud-markers: backend refuses the marker source (cleared, not drawn)
    bool effects_unreadable = false;   // J-effects-layer: backend refuses the effects source (mode 2, effects_used=false)
    bool terrain_unreadable = false;   // J-terrain-render: backend could not create/upload the terrain resources
    bool cockpit_unreadable = false;   // COCKPIT-2: backend could not create/upload the model resources
    bool capture_ok = true, held_ready = false; unsigned captures = 0; // J-world-hold: the backend's held world image
    bool png_ok = true, png_pending = false; unsigned png_takes = 0;   // J-test8-fixes: xr_png_once
    bool png_take(xr_stereo::PngCapture& out) override {
        check(!executing, "png readback only after completion");
        if (!png_pending) return false;
        png_pending = false; ++png_takes;
        out.w = 2; out.h = 1;
        out.final_rgba.assign(8, 1); out.alpha_rgba.assign(8, 2); out.worldfx_rgba.assign(8, 3);
        if (png_layers) { out.lw = 1; out.lh = 1; out.hud_rgba.assign(4, 4); if (png_layers > 1) out.markers_rgba.assign(4, 5); }   // LAYER-DUMP
        return true;
    }
    unsigned png_layers = 0;   // LAYER-DUMP: 0 none, 1 HUD layer only, 2 HUD + marker layer
    uint32_t bound_w[5]{}, bound_h[5]{};
    xr_stereo::GpuTiming gpu_timing{};
    xr_stereo::GpuTiming timing() override {
        check(!executing, "timestamp readback only after completion");
        return gpu_timing;
    }
    bool setup() override { j.add("d3d.setup"); return setup_ok; }
    bool bind(unsigned eye, const xr_stereo::SwapchainImages& im, uint32_t w, uint32_t h, int64_t) override {
        bound_w[eye]=w; bound_h[eye]=h;
        j.add("d3d.bind " + std::to_string(eye)); return im.count > 0;
    }
    xr_stereo::RecordFacts record(const xr_stereo::BlitJob& job) override {
        j.add("d3d.record " + std::to_string(job.index[0]) + "," + std::to_string(job.index[1]) +
            (job.use_world ? (job.world_composite ? std::string(job.world_held ? ",wh" : ",w") : ",q" + std::to_string(job.index[2])) : std::string()) +
            (job.use_hud ? ",h" + std::to_string(job.index[3]) : std::string()) +
            (job.use_markers ? ",m" + std::to_string(job.index[4]) : std::string()) +
            (job.use_effects ? std::string(",e") : std::string()));
        maps.push_back(job.map); jobs.push_back(job);
        xr_stereo::RecordFacts f;
        if (!record_ok) return f;
        // Recording changes no GPU state: the sequence is applied at execute.
        pending.clear();
        const bool depth = job.use_depth && !depth_unreadable;
        const bool hud = job.use_hud && !hud_unreadable;
        f.depth_used = depth;
        f.hud_used = hud;
        const bool markers = job.use_markers && !markers_unreadable;
        f.markers_used = markers;
        const bool effects = job.use_effects && !effects_unreadable && job.use_world && job.world_composite && !job.world_held;
        f.effects_used = effects;
        f.terrain_used = job.terrain && !terrain_unreadable;   // J-terrain-render
        f.cockpit_used = job.cockpit && job.cockpit_mesh && !cockpit_unreadable;   // COCKPIT-2
        auto out = [&](void* r, St before) {
            check((*states)[r] == before, "source in its published state at record");
            pending.push_back({r, St::psr}); ++f.barriers_out;
        };
        for (auto& e : job.eyes) out(e.resource, St::rt);
        if (depth) for (auto& d : job.depth) out(d.resource, St::dw);
        if (job.use_world && !job.world_held) out(job.world.resource, St::rt);
        if (hud) out(job.hud.resource, St::rt);
        if (markers) out(job.markers.resource, St::rt);
        if (effects) out(job.effects.resource, St::rt);
        f.draws = 2;
        for (unsigned t = 0; t < 2; ++t) f.clears += xr_stereo::needs_clear(job.map[t], bound_w[t], bound_h[t]);
        if (job.use_world && !job.world_composite) { ++f.draws; f.clears += xr_stereo::needs_clear(job.map[2], bound_w[2], bound_h[2]); }
        if (hud) { ++f.draws; f.clears += xr_stereo::needs_clear(job.map[3], bound_w[3], bound_h[3]); }
        else if (job.use_hud) ++f.clears; // refused HUD: image cleared transparent, not drawn
        if (markers) { ++f.draws; f.clears += xr_stereo::needs_clear(job.map[4], bound_w[4], bound_h[4]); }
        else if (job.use_markers) ++f.clears; // refused markers: image cleared transparent, not drawn
        if (restore) {
            for (auto& e : job.eyes) { pending.push_back({e.resource, St::rt}); ++f.barriers_back; }
            if (depth) for (auto& d : job.depth) { pending.push_back({d.resource, St::dw}); ++f.barriers_back; }
            if (job.use_world && !job.world_held) { pending.push_back({job.world.resource, St::rt}); ++f.barriers_back; }
            if (hud) { pending.push_back({job.hud.resource, St::rt}); ++f.barriers_back; }
            if (markers) { pending.push_back({job.markers.resource, St::rt}); ++f.barriers_back; }
            if (effects) { pending.push_back({job.effects.resource, St::rt}); ++f.barriers_back; }
        }
        if (job.world_capture && job.use_world && !job.world_held && capture_ok) { f.world_captured = true; held_ready = true; ++captures; }
        else if (job.world_capture) held_ready = false;
        f.held_used = job.world_held && held_ready;
        if (job.png_capture && png_ok) { f.png_recorded = true; png_pending = true; }
        f.ok = true; f.sources_restored = restore; f.swapchain_untouched = true;
        return f;
    }
    std::vector<std::pair<void*, St>> pending;
    bool close() override { j.add("d3d.close"); return close_ok; }
    bool fence_completed(void*, uint64_t) override { j.add(producer_complete ? "d3d.presync complete" : "d3d.presync pending"); return producer_complete; }
    bool gpu_wait(void*, uint64_t) override { j.add("d3d.gpu_wait"); return wait_ok; }
    bool execute(bool& dispatch) override {
        dispatch = execute_dispatch; j.add("d3d.execute");
        if (dispatch) { executing = true; for (auto& t : pending) (*states)[t.first] = t.second; }
        return execute_ok;
    }
    bool signal(uint64_t v) override {
        j.add("d3d.signal " + std::to_string(v));
        if (signal_hook) signal_hook();
        if (signal_ok) { signalled = v; samples_since = 0; }
        return signal_ok;
    }
    bool sample(uint64_t& done) override {
        ++samples_since;
        if (!never_complete && samples_since >= complete_after && signalled > completed) { completed = signalled; executing = false; }
        done = completed;
        j.add("d3d.sample " + std::to_string(done));
        return sample_ok;
    }
    bool destroy() override {
        if (executing) j.add("d3d.destroy.while-executing");
        j.add("d3d.destroy"); ++destroys; return destroy_ok;
    }
};

// ------------------------------------------------------- producer fake ---
struct FakeSource final : stereo::FrameSource {
    Journal& j; FakeBlit* blit = nullptr;
    std::map<void*, St> states;
    enum class S { free, published, held, quarantined };
    uint64_t reexecutions = 0;   // DECOUPLE review 2: re-executions of an owning recording (reexecute())
    struct Slot { S s = S::free; stereo::StereoFrame f; };
    std::array<Slot, stereo::kSlots> slots{};
    uint64_t next_id = 1, published = 0, skipped = 0, releases = 0, bad_releases = 0;
    uint64_t reported_ahead = 0; // publication racing the diagnostic snapshot
    bool pose_valid = true; uint32_t format = 10, width = 2560, height = 1440; bool partial = false;
    bool with_depth = false, with_world = false, lens_valid = false; uint32_t depth_format = 20;
    uint64_t world_gap_from = 0, world_gap_to = 0; // frame ids in [from, to] publish WITHOUT a world (a refusal burst)
    float lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    uint64_t epoch = 1, signal_lag = 0; int other_fence = 0; bool swap_fence = false;
    uint32_t space_gen = 1; // encoded rendered LOCAL generation (PoseSnapshot generation + 1)
    int fence_token = 0;
    // J-hud-consumer producer fields (hud_valid frames only when with_hud; hud_every > 1
    // publishes the HUD on every hud_every-th frame id only).
    bool with_hud = false; uint32_t hud_format = 10; unsigned hud_every = 1;
    enum class HudBad { none, null_resource, alias_eye, alias_world } hud_bad = HudBad::none;
    // J-hud-markers producer fields (hud_markers_valid frames only when with_markers; markers_every > 1 publishes the
    // markers on every markers_every-th frame id only). The marker image is res(slot, 6); it shares hud_format.
    bool with_markers = false; unsigned markers_every = 1;
    enum class MarkerBad { none, null_resource, alias_eye, alias_hud, alias_world } marker_bad = MarkerBad::none;
    // J-effects-layer producer fields (effects_valid frames only when with_effects; effects_every > 1 publishes the effects image on
    // every effects_every-th frame id only). The effects image is res(slot, 7); it shares hud_format (the producer sets it with the HUD).
    bool with_effects = false; unsigned effects_every = 1;
    uint32_t effects_repr = 0; // J-effects-dual: StereoFrame::effects_repr of every effects frame (0 legacy, 1 coverage_v1, other = unknown)
    enum class EffectsBad { none, null_resource, alias_eye, alias_world, alias_hud, bad_format } effects_bad = EffectsBad::none;
    // J-cockpit-fov-consumer producer fields: cockpit_fov sets cockpit_fov_valid with cfov; when
    // cockpit_bad_every > 1 every cockpit_bad_every-th frame id carries cfov_bad instead.
    bool cockpit_fov = false; unsigned cockpit_bad_every = 0;
    float cfov[2][4] = {{-0.95f, 0.75f, 0.90f, -0.88f}, {-0.80f, 0.92f, 0.91f, -0.86f}};
    float cfov_bad[2][4] = {{-0.95f, 0.75f, 0.90f, -0.88f}, {std::nanf(""), 0.92f, 0.91f, -0.86f}};
    // J-quad-yaw producer fields: with_neutral sets neutral_valid (posed frames only) with npos/nyaw.
    bool with_neutral = false; float npos[3] = {0.4f, 1.3f, -0.2f}; float nyaw = 0.0f;
    // xr_look_drive_keys producer fields: with_look sets look_valid with lyaw/lpitch (the look the game rendered).
    bool with_look = false; float lyaw = 0.0f, lpitch = 0.0f;
    // Timing-anchor fixtures: an optional hook applied to every published frame last (lets a case set the render pose, look and diagnostics).
    std::function<void(stereo::StereoFrame&)> mutate;
    std::function<void(unsigned slot, uint64_t frame_id)> on_acquire;   // review 3: called after a successful acquisition (slot already HELD)
    explicit FakeSource(Journal& journal) : j(journal) {
        for (unsigned i = 0; i < stereo::kSlots; ++i) {
            for (unsigned e = 0; e < 2; ++e) { states[res(i, e)] = St::rt; states[res(i, 2 + e)] = St::dw; }
            states[res(i, 4)] = St::rt; // world snapshot
            states[res(i, 5)] = St::rt; // HUD image
            states[res(i, 6)] = St::rt; // marker image
            states[res(i, 7)] = St::rt; // effects image
        }
    }
    static void* res(unsigned slot, unsigned eye) { return reinterpret_cast<void*>(uintptr_t(0x9000 + slot * 0x10 + eye)); }
    bool publish() {
        // Producer: reuse a free slot, else the OLDEST published one; never a held one.
        int pick = -1;
        for (unsigned i = 0; i < stereo::kSlots; ++i) if (slots[i].s == S::free) { pick = int(i); break; }
        if (pick < 0) {
            uint64_t oldest = UINT64_MAX;
            for (unsigned i = 0; i < stereo::kSlots; ++i)
                if (slots[i].s == S::published && slots[i].f.frame_id < oldest) { oldest = slots[i].f.frame_id; pick = int(i); }
        }
        if (pick < 0) { ++skipped; return false; }
        auto& sl = slots[pick];
        check(states[res(pick, 0)] == St::rt && states[res(pick, 1)] == St::rt, "producer reuses only RENDER_TARGET images");
        sl.s = S::published;
        auto& f = sl.f; f = {};
        f.frame_id = next_id++; f.slot = unsigned(pick); f.eye[0] = static_cast<ID3D12Resource*>(res(pick, 0));
        f.eye[1] = static_cast<ID3D12Resource*>(res(pick, 1)); f.dxgi_format = format; f.width = width; f.height = height;
        f.fence = reinterpret_cast<ID3D12Fence*>(swap_fence ? &other_fence : &fence_token); f.fence_value = f.frame_id * 10; f.pose_valid = pose_valid;
        f.fence_epoch = epoch; f.last_signalled = f.fence_value - signal_lag; // producer bookkeeping (issue 29)
        f.space_generation = pose_valid ? space_gen : 0;
        for (int e = 0; e < 2; ++e) {
            f.pose[e] = {{e ? 0.031f : -0.031f, 1.5f, 0.1f * float(f.frame_id)}, {0, 0, 0, 1}, {-0.8f, 0.7f, 0.75f, -0.85f}};
        }
        f.predicted_display_time = 5; f.members = 7; f.partial = partial;
        if (with_depth) { f.depth[0] = static_cast<ID3D12Resource*>(res(pick, 2)); f.depth[1] = static_cast<ID3D12Resource*>(res(pick, 3)); f.depth_format = depth_format; }
        if (with_world && !(world_gap_from && f.frame_id >= world_gap_from && f.frame_id <= world_gap_to)) { f.world = static_cast<ID3D12Resource*>(res(pick, 4)); f.world_valid = true; }
        f.lens_valid = lens_valid; for (int k = 0; k < 4; ++k) f.lens_tan[k] = lens[k];
        if (with_hud && (f.frame_id - 1) % hud_every == 0) {
            f.hud_valid = true; f.hud_format = hud_format; f.hud_draws = 58; f.hud_refused = 1;
            f.hud = hud_bad == HudBad::null_resource ? nullptr : hud_bad == HudBad::alias_eye ? f.eye[0] :
                hud_bad == HudBad::alias_world ? static_cast<ID3D12Resource*>(res(pick, 4)) : static_cast<ID3D12Resource*>(res(pick, 5));
        }
        if (with_markers && (f.frame_id - 1) % markers_every == 0) {
            f.hud_markers_valid = true; f.hud_format = hud_format; f.hud_marker_draws = 3; f.hud_marker_refused = 0;
            f.hud_markers = marker_bad == MarkerBad::null_resource ? nullptr : marker_bad == MarkerBad::alias_eye ? f.eye[0] :
                marker_bad == MarkerBad::alias_hud ? static_cast<ID3D12Resource*>(res(pick, 5)) :
                marker_bad == MarkerBad::alias_world ? static_cast<ID3D12Resource*>(res(pick, 4)) : static_cast<ID3D12Resource*>(res(pick, 6));
            if (marker_bad == MarkerBad::alias_hud && !with_hud) f.hud = static_cast<ID3D12Resource*>(res(pick, 5));
        }
        if (with_effects && (f.frame_id - 1) % effects_every == 0) {
            f.effects_valid = true; f.hud_format = effects_bad == EffectsBad::bad_format ? 0u : hud_format; f.effects_draws = 4; f.effects_refused = 1; f.effects_tail = 2; f.effects_repr = effects_repr;
            f.effects = effects_bad == EffectsBad::null_resource ? nullptr : effects_bad == EffectsBad::alias_eye ? f.eye[0] :
                effects_bad == EffectsBad::alias_world ? static_cast<ID3D12Resource*>(res(pick, 4)) :
                effects_bad == EffectsBad::alias_hud ? static_cast<ID3D12Resource*>(res(pick, 5)) : static_cast<ID3D12Resource*>(res(pick, 7));
            if (effects_bad == EffectsBad::alias_hud && !with_hud) f.hud = static_cast<ID3D12Resource*>(res(pick, 5));
        }
        if (cockpit_fov) {
            const bool bad = cockpit_bad_every && f.frame_id % cockpit_bad_every == 0;
            f.cockpit_fov_valid = true;
            std::memcpy(f.cockpit_fov, bad ? cfov_bad : cfov, sizeof f.cockpit_fov);
        } else std::memcpy(f.cockpit_fov, cfov, sizeof f.cockpit_fov); // values present, flag OFF: must be ignored
        if (with_neutral && pose_valid) { f.neutral_valid = true; std::memcpy(f.neutral_pos, npos, sizeof npos); f.neutral_yaw = nyaw; }
        if (with_look) { f.look_valid = true; f.look_yaw = lyaw; f.look_pitch = lpitch; }
        if (mutate) mutate(f);
        ++published;
        return true;
    }
    bool try_acquire_latest(uint64_t after, stereo::StereoFrame& out) override {
        int best = -1; uint64_t id = after;
        for (unsigned i = 0; i < stereo::kSlots; ++i)
            if (slots[i].s == S::published && slots[i].f.frame_id > id) { id = slots[i].f.frame_id; best = int(i); }
        if (best < 0) { j.add("source.none"); return false; }
        slots[best].s = S::held; out = slots[best].f;
        j.add("source.acquire " + std::to_string(out.frame_id));
        if (on_acquire) on_acquire(unsigned(best), out.frame_id);   // review 3 (K): an event between the acquisition and the consumer's first use
        return true;
    }
    // The producer re-executes the recording that owns `slot` (gpu_copy_step5 step5BeforeExecute, J-180 P1): the images are about to be rendered
    // again, so a published/held slot is QUARANTINED before the native Execute (a consumer that acquired it cannot be revoked; its release then
    // finds the quarantine and the slot is never reused). A free slot is only re-timestamped.
    void reexecute(unsigned slot) {
        auto& sl = slots[slot]; ++reexecutions;
        if (sl.s == S::published || sl.s == S::held) sl.s = S::quarantined;
        j.add("source.reexecute " + std::to_string(sl.f.frame_id));
    }
    bool images_unchanged(const stereo::StereoFrame& f) const override {
        return f.slot < stereo::kSlots && slots[f.slot].s == S::held && slots[f.slot].f.frame_id == f.frame_id;
    }
    void release(const stereo::StereoFrame& f) override {
        auto& sl = slots[f.slot];
        const bool quarantined = sl.s == S::quarantined && sl.f.frame_id == f.frame_id;   // re-executed while held: stays quarantined (legal)
        if ((sl.s != S::held && !quarantined) || sl.f.frame_id != f.frame_id) ++bad_releases;
        if (states[res(f.slot, 0)] != St::rt || states[res(f.slot, 1)] != St::rt) ++bad_releases; // RENDER_TARGET at release
        if (states[res(f.slot, 2)] != St::dw || states[res(f.slot, 3)] != St::dw || states[res(f.slot, 4)] != St::rt) ++bad_releases; // depth DEPTH_WRITE, world RT
        if (states[res(f.slot, 5)] != St::rt) ++bad_releases; // HUD RENDER_TARGET at release
        if (states[res(f.slot, 6)] != St::rt) ++bad_releases; // marker image RENDER_TARGET at release
        if (blit && blit->executing) ++bad_releases;            // never while consumer GPU work is in flight
        sl.s = quarantined ? S::quarantined : S::free; ++releases;
        j.add("source.release " + std::to_string(f.frame_id));
    }
    uint64_t frames_published() const override { return published + reported_ahead; }
    uint64_t frames_skipped() const override { return skipped; }
};

// ------------------------------------------------------------------ rig ---
struct Rig {
    Journal j;
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<FakeXr> xr = std::make_shared<FakeXr>(j, *clock);
    std::shared_ptr<FakeBlit> blit = std::make_shared<FakeBlit>(j);
    FakeSource src{j};
    std::vector<std::string> lines; unsigned lifecycle = 0;
    std::function<void(bool, const char*)> tee; // J-xr-log: optional second sink (the proxy router)
    std::shared_ptr<xr_stereo::Output> out;
    std::shared_ptr<Owner> owner;
    unsigned stop_after = 0;           // request_stop when this many waits happened
    bool publish_each_frame = true;    // producer publishes before each wait
    unsigned publish_until = UINT32_MAX;
    unsigned publish_from = 0;         // first wait index that publishes
    xr_stereo::Settings settings{};
    Limits limits{};
    Rig() { settings.cap_width = 2560; blit->states = &src.states; src.blit = blit.get(); }
    void build(bool with_consumer = true, bool enabled = true) {
        Config c; c.enabled = enabled; c.continuous = true;
        owner = std::make_shared<Owner>(xr, clock, c, limits);
        out = std::make_shared<xr_stereo::Output>([this] { return static_cast<stereo::FrameSource*>(&src); }, xr, blit,
            [this](bool life, const char* line) { lines.emplace_back(line); lifecycle += life; if (tee) tee(life, line); }, settings);
        if (with_consumer) check(owner->set_consumer(out), "consumer attaches before run");
        xr_stereo::register_producer_fence(&src.fence_token, 1); // the producer's explicit registration
        xr->hook["wait_frame"] = [this] {
            if (publish_each_frame && xr->waits >= publish_from && xr->waits < publish_until) src.publish();
            if (stop_after && xr->waits + 1 >= stop_after) owner->request_stop();
        };
    }
    Report run() { return owner->run(); }
    bool has_line(const std::string& p) const {
        for (auto& l : lines) if (l.find(p) != std::string::npos) return true;
        return false;
    }
};

std::vector<std::string> frame_slice(const std::vector<std::string>& all, unsigned which) {
    // The calls of the which-th (0-based) begun frame: from its wait_frame to its end.
    unsigned seen = 0; std::vector<std::string> out; bool in = false;
    for (auto& s : all) {
        if (s == "wait_frame") { if (in) break; if (seen++ == which) in = true; }
        if (in) { out.push_back(s); if (s == "end_frame" || s == "end_frame_projection") break; }
    }
    return out;
}

// ------------------------------------------------------------------ tests ---
void mapping_math() {
    auto m = xr_stereo::letterbox(2560, 1440, 2688, 2688);
    check(m.ok && m.dst.x == 0 && m.dst.y == 588 && m.dst.w == 2688 && m.dst.h == 1512, "2560x1440 -> 2688x2688 letterbox rect");
    check(std::fabs(m.scale - 1.05) < 1e-12, "scale 1.05");
    double u = 0, v = 0;
    check(!xr_stereo::source_uv(m, 1344, 587, u, v), "row above the rect is black");
    check(!xr_stereo::source_uv(m, 1344, 2100, u, v), "row below the rect is black");
    check(xr_stereo::source_uv(m, 0, 588, u, v) && std::fabs(u - 0.5 / 2688) < 1e-12 && std::fabs(v - 0.5 / 1512) < 1e-12, "first rect pixel uv");
    check(xr_stereo::source_uv(m, 2687, 2099, u, v) && std::fabs(u - 2687.5 / 2688) < 1e-12 && std::fabs(v - 1511.5 / 1512) < 1e-12, "last rect pixel uv");
    check(xr_stereo::source_uv(m, 1343, 1343, u, v) && std::fabs(u - 1343.5 / 2688) < 1e-12 && std::fabs(v - 755.5 / 1512) < 1e-12, "centre uv");
    // the centre of the square maps to the centre of the source (to half a pixel)
    check(std::fabs(u * 2560 - 1280) < 1 && std::fabs(v * 1440 - 720) < 1, "centre -> source centre");
    // source pixel footprint: one destination pixel = 1/1.05 source pixel
    double u2 = 0, v2 = 0; xr_stereo::source_uv(m, 1344, 1344, u2, v2);
    check(std::fabs((u2 - u) * 2560 - 1 / 1.05) < 1e-9 && std::fabs((v2 - v) * 1440 - 1 / 1.05) < 1e-9, "uv step = 1/scale source px");
    auto p = xr_stereo::letterbox(1440, 2560, 2688, 2688);
    check(p.ok && p.dst.x == 588 && p.dst.y == 0 && p.dst.w == 1512 && p.dst.h == 2688, "portrait pillarbox");
    auto e = xr_stereo::letterbox(2000, 1000, 1000, 500);
    check(e.ok && e.dst.x == 0 && e.dst.y == 0 && e.dst.w == 1000 && e.dst.h == 500 && e.scale == 0.5, "same aspect fills exactly");
    auto r = xr_stereo::letterbox(1920, 1080, 1000, 1000);
    check(r.ok && r.dst.w == 1000 && r.dst.h == 563 && r.dst.y == 218, "half-up rounding 562.5 -> 563, floor centring");
    auto wide = xr_stereo::letterbox(2560, 1440, 2016, 2240);
    check(wide.ok && wide.dst.w == 2016 && wide.dst.h == 1134 && wide.dst.y == 553, "non-square view");
    check(!xr_stereo::letterbox(0, 1440, 2688, 2688).ok && !xr_stereo::letterbox(2560, 1440, 2688, 0).ok, "zero refused");
    check(!xr_stereo::source_uv(BlitMapping{}, 0, 0, u, v), "invalid mapping samples nothing");
    // every destination pixel inside the rect samples inside [0,1]
    for (uint32_t y = 588; y < 2100; y += 37) for (uint32_t x = 0; x < 2688; x += 41) {
        check(xr_stereo::source_uv(m, x, y, u, v) && u > 0 && u < 1 && v > 0 && v < 1, "uv inside (0,1)");
    }
}

void format_policy_same_as_probe() {
    const std::vector<std::vector<int64_t>> sets{{}, {29}, {28}, {28, 29}, {29, 28}, {87, 2}, {87, 28, 10}, {2, 29, 91}, {91, 87}};
    for (auto& s : sets) {
        auto probe = acz::probe::select_later_colour_format(s);
        auto mine = xr_stereo::select_format(s.data(), uint32_t(s.size()));
        check((probe ? *probe : 0) == mine, "format selection identical to xr_probe/format_policy.h");
    }
    check(xr_stereo::source_format_ok(10) && xr_stereo::source_format_ok(2) && !xr_stereo::source_format_ok(28) &&
        !xr_stereo::source_format_ok(0) && !xr_stereo::source_format_ok(87), "source formats");
}

void config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("log_enabled=true\n");
    check(d.issue == ConfigIssue::none && !d.config.xr_stereo_output() && !d.config.gpu_copy_step5_continuous(), "keys default OFF");
    auto on = parse_config("log_enabled=true\nxr_stereo_output=true\ngpu_copy_step5_continuous=1\n");
    check(on.issue == ConfigIssue::none && on.config.xr_stereo_output() && on.config.gpu_copy_step5_continuous(), "keys parse ON");
    auto off = parse_config("xr_stereo_output=0\ngpu_copy_step5_continuous=false\n");
    check(off.issue == ConfigIssue::none && !off.config.xr_stereo_output() && !off.config.gpu_copy_step5_continuous(), "keys parse OFF");
    check(parse_config("xr_stereo_output=yes\n").issue == ConfigIssue::out_of_range, "xr_stereo_output malformed rejected");
    check(parse_config("gpu_copy_step5_continuous=2\n").issue == ConfigIssue::out_of_range, "step5 malformed rejected");
    check(parse_config("xr_submission=true\n").issue == ConfigIssue::capture_enable_rejected, "legacy xr_submission still rejected");
    check(!parse_config("xr_stereo_output=true\nxr_stereo_output=true\n").config.xr_stereo_output(), "duplicate refuses");
}

void registry() {
    PoseSnapshot s;
    check(!xr_stereo::try_read_pose(s), "no owner: no pose");
    check(xr_stereo::registered_frame_source() == nullptr, "no source registered");
    Journal j; FakeSource a(j), b(j);
    xr_stereo::register_frame_source(&a);
    xr_stereo::register_frame_source(&b);
    check(xr_stereo::registered_frame_source() == &a, "first registration wins");
    xr_stereo::request_stop(); // no owner: harmless
}

void disabled_owner_makes_no_calls() {
    Rig g; g.build(true, false);
    auto r = g.run();
    check(g.j.calls().empty() && g.src.published == 0 && r.first_cause == Cause::disabled, "OFF: zero runtime/consumer calls");
    check(!g.out->ledger().holding && g.lines.empty(), "OFF: nothing logged or held");
}

void success_sequence() {
    Rig g; g.stop_after = 40; g.build();
    auto r = g.run();
    const auto calls = g.j.calls();
    check(r.complete && r.success && r.clean && r.consumer_attached && r.consumer_retired, "success report");
    check(r.stop == StopReason::requested && r.first_cause == Cause::none, "orderly requested stop");
    const unsigned n = r.begun;
    check(n >= 38 && r.ended == n && r.waited == n && r.projection_layers == n, "every begun frame carried one layer");
    check(g.out->counters().submitted == n && g.out->counters().reused == 0 && !g.out->failed(), "consumer counters");
    check(g.src.releases == g.out->counters().acquired_frames && g.src.bad_releases == 0, "every slot released once, after completion, in RT");
    check(g.xr->violations == 0, "no swapchain rule violation");
    // Exact per-frame order (frames 0, 1 and the last).
    for (unsigned f : {0u, 1u, n - 1}) {
        auto s = frame_slice(calls, f);
        const std::string id = std::to_string(f + 1);
        const std::vector<std::string> expected{"wait_frame", "begin_frame", "locate_views", "locate_head",
            "source.acquire " + id, "d3d.presync pending", "xr.acquire 0", "xr.wait 0 0", "xr.acquire 1", "xr.wait 1 0",
            "d3d.record " + std::to_string(f % 3) + "," + std::to_string(f % 3), "d3d.close", "d3d.gpu_wait",
            "d3d.execute", "d3d.signal " + id, "d3d.sample " + id, "xr.release 0", "xr.release 1", "source.release " + id,
            "end_frame_projection"};
        check(s == expected, "exact call order of frame " + std::to_string(f));
    }
    // Startup order: consumer setup after the VIEW space, before BeginSession.
    long view = g.j.index("create_view_space"), cfg = g.j.index("xr.view_configs"), fmt = g.j.index("xr.formats"),
         setup = g.j.index("d3d.setup"), c0 = g.j.index("xr.create 0"), b0 = g.j.index("d3d.bind 0"),
         c1 = g.j.index("xr.create 1"), b1 = g.j.index("d3d.bind 1"), begin = g.j.index("begin_session");
    check(view < cfg && cfg < fmt && fmt < setup && setup < c0 && c0 < b0 && b0 < c1 && c1 < b1 && b1 < begin, "setup order");
    // Teardown: swapchains and private objects before the exit handshake and the session.
    long d0 = g.j.index("xr.destroy 0"), d1 = g.j.index("xr.destroy 1"), dd = g.j.index("d3d.destroy"),
         exit = g.j.index("request_exit"), end = g.j.index("end_session"), ds = g.j.index("destroy_session");
    check(d0 > 0 && d0 < d1 && d1 < dd && dd < exit && exit < end && end < ds, "retire before exit handshake and session destruction");
    check(g.j.count("destroy_session.with-live-swapchain") == 0 && g.j.count("d3d.destroy.while-executing") == 0, "never destroyed under use");
    // Layer content: the producer's poses (pose_valid), raw, and this frame's predicted time.
    check(g.xr->layers.size() == n, "layers recorded");
    const auto& l = g.xr->layers[4];
    check(std::fabs(l.views[0].pose.position[2] - 0.5f) < 1e-6 && l.views[0].pose.position[0] == -0.031f &&
        l.views[1].pose.position[0] == 0.031f && l.views[0].fov.left == -0.8f && l.views[1].fov.down == -0.85f, "frame poses/FOV submitted");
    check(g.xr->layer_times[4] == 1000000000 + 5 * g.xr->period, "layer uses this frame's predicted time");
    check(g.out->counters().pose_from_frame == n && g.out->counters().pose_from_located == 0, "pose source counted");
    // Blit mapping used for both eyes.
    check(!g.blit->maps.empty() && g.blit->maps[0][0].dst.x == 0 && g.blit->maps[0][0].dst.y == 0 && g.blit->maps[0][1].dst.w == 2560 &&
        g.blit->maps[0][1].dst.h == 1440 && g.blit->maps[0][0].scale == 1.0, "full-rect 1:1 mapping passed to record");
    check(!g.blit->jobs.empty() && !g.blit->jobs[0].use_depth && !g.blit->jobs[0].use_world, "no depth/world in this frame: opaque, no quad");
    check(!g.xr->layers[0].source_alpha && !g.xr->layers[0].quad, "opaque projection without a quad");
    for (auto& kv : g.src.states) check(kv.second != St::psr && kv.second != St::other, "all producer images back in their published state");
    check(g.has_line("xr-stereo;event=formats;count=4;offered-28=1;offered-29=1;selected=29"), "format line");
    check(g.has_line("xr-stereo;event=swapchains;format=29;eye0=2560x1440,images=3;eye1=2560x1440,images=3;quad=2560x1440,images=3"), "swapchain line");
    check(g.has_line("xr-stereo;event=mapping;source=2560x1440;fmt=10;eye0=0,0,2560x1440;eye1=0,0,2560x1440;quad=0,0,2560x1440;scale=1.000000;aspect-match=1;lens=none-pose-fov"), "mapping line");
    check(g.j.count("xr.create 2") == 1 && g.j.count("xr.destroy 2") == 1 && g.j.count("d3d.bind 2") == 1, "world quad swapchain created and retired");
    check(g.has_line("xr-stereo;event=first-frame;frame=1;frame_id=1;slot=0;src=2560x1440"), "first-frame line");
    check(g.has_line("xr-stereo;event=first-layer"), "first-layer line");
    check(g.has_line("xr-stereo;event=summary;"), "periodic summary");
    check(g.has_line("xr-stereo;result=PASS;reason=layers-submitted"), "final result line");
    check(g.has_line("xr-stereo;event=retire;outcome=clean"), "retire line");
}

void cpu_presynced_skips_queue_wait() {
    Rig g; g.stop_after = 6; g.blit->producer_complete = true; g.build();
    auto r = g.run();
    check(r.success && g.j.count("d3d.gpu_wait") == 0 && g.out->counters().cpu_presynced == r.begun, "CPU-observed producer fence: no queue Wait");
}

void release_only_after_completion() {
    Rig g; g.stop_after = 8; g.blit->complete_after = 4; g.build();
    auto r = g.run();
    check(r.success && g.src.bad_releases == 0 && g.xr->violations == 0, "delayed completion still succeeds");
    auto s = frame_slice(g.j.calls(), 2);
    long sig = -1, done = -1, rel = -1, srel = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == "d3d.signal 3") sig = long(i);
        if (s[i] == "d3d.sample 3" && done < 0) done = long(i);
        if (s[i] == "xr.release 0") rel = long(i);
        if (s[i] == "source.release 3") srel = long(i);
    }
    check(sig >= 0 && done > sig && rel > done && srel > rel, "release after the completing sample");
    unsigned pending = 0; for (auto& c : s) pending += c == "d3d.sample 2";
    check(pending == 3, "three incomplete samples observed before completion");
}

void should_render_false() {
    Rig g; g.stop_after = 10; g.xr->should_render = false; g.build();
    auto r = g.run();
    check(r.success && r.projection_layers == 0 && g.j.count("end_frame") == r.begun, "shouldRender=false: zero layers every frame");
    check(g.j.prefix("source.") == 0 && g.j.prefix("xr.acquire") == 0 && g.out->counters().should_render_false == r.begun, "no acquisition");
    check(g.has_line("result=FAIL;reason=no-layer-submitted"), "no layer is not a PASS");
}

void producer_stops_publishing() {
    // Revision 3 pacing: after the first StereoFrame never zero layers while the
    // last submitted layers are younger than 1 s; zero ("stale-zero") only after.
    Rig g; g.stop_after = 200; g.publish_until = 4; g.build(); // frames 1..4 new, then none (11 ms per frame)
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && c.new_frames == 4 && c.zero_no_frame == 0, "four new frames, no blank before the first");
    check(c.reused == 45 && c.stale_zero > 0 && c.stale_age == c.stale_zero, "reuse while age <= 500 ms (45 x 11 ms), then stale zero by age");
    check(g.has_line("reuse-age-max-ms=495"), "reuse age maximum logged");
    check(c.new_frames + c.reused + c.stale_zero == r.begun, "every frame accounted new/reused/stale-zero");
    check(r.projection_layers == c.new_frames + c.reused && g.j.count("end_frame") == c.stale_zero, "layers until stale, zero after");
    auto s5 = frame_slice(g.j.calls(), 4);
    check(std::find(s5.begin(), s5.end(), "xr.acquire 0") == s5.end() && s5.back() == "end_frame_projection", "reuse submits without acquire");
    check(std::find(s5.begin(), s5.end(), "source.none") != s5.end(), "reuse only when nothing newer was published");
    check(g.xr->layers[40].views[0].pose.position[2] == g.xr->layers[3].views[0].pose.position[2], "reused poses unchanged");
    check(g.xr->violations == 0, "reuse never references an acquired image");
}
void pacing_gaps() {
    // 11 ms per XR frame. New frames at frame 0, frame 30 (a ~330 ms gap: all
    // reused) and frame 150 (a ~1.3 s gap: reuse up to 500 ms, then stale zero);
    // a newer frame switches back at once. (Clock jumps inside one call would
    // trip the 250 ms progress rule, so gaps are made of frames.)
    Rig g; g.stop_after = 160; g.publish_each_frame = false; g.build();
    g.xr->hook["wait_frame"] = [&g] {
        const unsigned w = g.xr->waits;
        if (w == 0 || w == 30 || w == 150) g.src.publish();
        if (w + 1 >= 160) g.owner->request_stop();
    };
    auto r = g.run();
    const auto& c = g.out->counters();
    auto last = [&](unsigned f) { auto s = frame_slice(g.j.calls(), f); return s.empty() ? std::string() : s.back(); };
    bool gap1 = true; for (unsigned f = 0; f <= 30; ++f) gap1 = gap1 && last(f) == "end_frame_projection";
    check(gap1, "330 ms gap: every frame re-submits the last layers");
    check(last(30 + 45) == "end_frame_projection" && last(30 + 46) == "end_frame", "reuse until 500 ms old, then zero");
    check(last(149) == "end_frame" && last(150) == "end_frame_projection", "a newer frame switches back immediately");
    check(c.new_frames == 3 && c.stale_zero == 150 - 76 && r.success, "new/stale counts");
    check(c.new_frames + c.reused + c.stale_zero == r.begun, "every frame accounted");
    check(g.has_line(";new=") && g.has_line("stale-zero=") && g.has_line("hud=not-included"), "summary counters");
}
void lens_depth_world() {
    Rig g; g.stop_after = 6; g.src.lens_valid = true; g.src.with_depth = true; g.src.with_world = true; g.build();
    auto r = g.run();
    check(r.success && g.src.bad_releases == 0 && g.xr->violations == 0, "lens/depth/world frames succeed, states restored");
    const auto& l = g.xr->layers[1];
    const float al = 0.5515210659147595f, au = 0.3331069101666005f;
    check(std::fabs(l.views[0].fov.left + al) < 1e-6f && std::fabs(l.views[0].fov.right - al) < 1e-6f &&
        std::fabs(l.views[1].fov.up - au) < 1e-6f && std::fabs(l.views[1].fov.down + au) < 1e-6f, "layer FOV = atan(lens tan)");
    check(l.views[0].pose.position[0] == -0.031f && l.views[1].pose.position[0] == 0.031f, "frame poses kept with the lens FOV");
    check(l.source_alpha, "premultiplied coverage alpha flag");
    check(l.quad && std::fabs(l.quad_width - 49.216f) < 1e-3f && std::fabs(l.quad_height - 27.68f) < 1e-3f, "quad subtends the lens FOV at 40 m");
    check(std::fabs(l.quad_pose.position[2] - (0.1f - 40.0f)) < 1e-4f && l.quad_pose.position[1] == 1.5f && std::fabs(l.quad_pose.position[0]) < 1e-6f &&
        l.quad_pose.orientation[3] == 1.0f, "quad world-locked at the first frame's neutral eye midpoint, 40 m ahead, identity");
    check(g.xr->layers[4].quad_pose.position[2] == l.quad_pose.position[2], "quad stays world-locked (neutral captured once)");
    auto s = frame_slice(g.j.calls(), 0);
    const std::vector<std::string> expected{"wait_frame", "begin_frame", "locate_views", "locate_head", "source.acquire 1",
        "d3d.presync pending", "xr.acquire 0", "xr.wait 0 0", "xr.acquire 1", "xr.wait 1 0", "xr.acquire 2", "xr.wait 2 0", "d3d.record 0,0,q0",
        "d3d.close", "d3d.gpu_wait", "d3d.execute", "d3d.signal 1", "d3d.sample 1", "xr.release 0", "xr.release 1", "xr.release 2",
        "source.release 1", "end_frame_projection"};
    check(s == expected, "exact order with the world quad target");
    const auto& job = g.blit->jobs[0];
    check(job.use_depth && job.use_world && job.depth[0].format == 20 && job.tonemap == xr_stereo::Tonemap::clamp && job.exposure == 1.0f, "job");
    check(g.has_line("order=quad-then-projection") && g.has_line("proj-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)"), "first-layer order/flags line");
    check(g.has_line("lens=frame;fov-deg=-31.600,31.600,19.086,-19.086;quad-distance-m=40.0;tonemap=clamp"), "mapping line logs the lens FOV");
    check(g.has_line("depth=coverage;depth-fmt=20;world=quad;lens=frame"), "first-frame line");
}
void depth_fallbacks() {
    { Rig g; g.stop_after = 4; g.src.with_depth = true; g.src.depth_format = 87; g.build(); // unsupported depth format
      auto r = g.run();
      check(r.success && !g.blit->jobs[0].use_depth && !g.xr->layers[0].source_alpha && g.out->counters().depth_frames == 0, "unsupported depth: opaque"); }
    { Rig g; g.stop_after = 4; g.src.with_depth = true; g.blit->depth_unreadable = true; g.build(); // backend could not bind it
      auto r = g.run();
      check(r.success && g.blit->jobs[0].use_depth && !g.xr->layers[0].source_alpha && g.has_line("depth=unreadable-opaque"), "unreadable depth: opaque"); }
    { Rig g; g.stop_after = 4; g.src.with_world = false; g.src.with_depth = true; g.build();
      auto r = g.run();
      check(r.success && g.xr->layers[0].source_alpha && !g.xr->layers[0].quad && g.j.prefix("xr.acquire 2") == 0, "world invalid: no quad"); }
    { Rig g; g.stop_after = 4; g.settings.world_quad = false; g.src.with_world = true; g.build();
      auto r = g.run();
      check(r.success && g.j.count("xr.create 2") == 0 && !g.xr->layers[0].quad, "world quad disabled by setting: no swapchain"); }
}
void geometry_and_shading_math() {
    const float tan[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    check(xr_stereo::lens_ok(tan), "lens ok");
    const float bad[4] = {0.6f, -0.1f, 0.3f, 0.3f}, nan[4] = {0.6f, std::nanf(""), 0.3f, 0.3f};
    check(!xr_stereo::lens_ok(bad) && !xr_stereo::lens_ok(nan) && !xr_stereo::lens_ok(nullptr), "lens refusals");
    auto f = xr_stereo::fov_from_lens(tan);
    check(std::fabs(f.left + 0.551521066f) < 1e-6f && std::fabs(f.right - 0.551521066f) < 1e-6f &&
        std::fabs(f.up - 0.333106910f) < 1e-6f && std::fabs(f.down + 0.333106910f) < 1e-6f, "atan(0.6152)=0.55152, atan(0.3460)=0.33311");
    const float asym[4] = {0.5f, 0.7f, 0.4f, 0.2f}, n[3] = {1, 2, 3};
    auto q = xr_stereo::world_quad(n, asym, 10.0f);
    check(std::fabs(q.width - 12.0f) < 1e-5f && std::fabs(q.height - 6.0f) < 1e-5f && std::fabs(q.pose.position[0] - 2.0f) < 1e-5f &&
        std::fabs(q.pose.position[1] - 3.0f) < 1e-5f && q.pose.position[2] == -7.0f, "asymmetric quad: offset centre, exact edges");
    // edges subtend exactly the lens: left edge x = cx - w/2 = n.x - D*tan_l
    check(std::fabs((q.pose.position[0] - q.width / 2) - (1.0f - 5.0f)) < 1e-5f && std::fabs((q.pose.position[1] + q.height / 2) - (2.0f + 4.0f)) < 1e-5f, "edges");
    auto e = xr_stereo::swapchain_extent(2560, 1440, 4096, 4096, 2560);
    check(e.w == 2560 && e.h == 1440, "2560x1440 kept");
    e = xr_stereo::swapchain_extent(2560, 1440, 4096, 4096, 1920);
    check(e.w == 1920 && e.h == 1080, "cap 1920 -> 1920x1080");
    e = xr_stereo::swapchain_extent(2560, 1440, 2048, 1000, 2560);
    check(e.w == 1778 && e.h == 1000, "max height limits, aspect kept");
    check(xr_stereo::swapchain_extent(0, 1440, 4096, 4096, 2560).w == 0, "zero refused");
    auto m = xr_stereo::full_rect(2560, 1440, 1920, 1080);
    double u = 0, v = 0;
    check(m.ok && m.dst.w == 1920 && m.dst.h == 1080 && m.dst.x == 0 && xr_stereo::source_uv(m, 959, 539, u, v) &&
        std::fabs(u - 959.5 / 1920) < 1e-12 && std::fabs(v - 539.5 / 1080) < 1e-12, "full rect uv");
    check(xr_stereo::depth_srv_format(20) == 21 && xr_stereo::depth_srv_format(40) == 41 && xr_stereo::depth_srv_format(45) == 46 &&
        xr_stereo::depth_srv_format(55) == 56 && xr_stereo::depth_srv_format(10) == 0, "depth SRV formats");
    check(xr_stereo::depth_srv_format(19) == 21 && xr_stereo::depth_srv_format(39) == 41 && xr_stereo::depth_srv_format(44) == 46 &&
        xr_stereo::depth_srv_format(53) == 56, "typeless depth RESOURCE formats map to the same SRV plane (D-240)");
    using xr_stereo::Tonemap;
    const float c[3] = {0.5f, 2.0f, -1.0f};
    auto s = xr_stereo::shade(c, true, 0.25f, Tonemap::clamp, 1.0f);
    check(s.r == 0.5f && s.g == 1.0f && s.b == 0.0f && s.a == 1.0f, "covered: clamp, opaque");
    s = xr_stereo::shade(c, true, 0.0f, Tonemap::clamp, 1.0f);
    check(s.r == 0 && s.g == 0 && s.b == 0 && s.a == 0, "uncovered (depth 0, canopy glass): transparent premultiplied");
    s = xr_stereo::shade(c, false, 0.0f, Tonemap::clamp, 1.0f);
    check(s.a == 1.0f && s.r == 0.5f, "no depth: opaque fallback");
    s = xr_stereo::shade(c, false, 0.0f, Tonemap::reinhard, 2.0f);
    check(std::fabs(s.r - 0.5f) < 1e-6f && std::fabs(s.g - 0.8f) < 1e-6f && s.b == 0.0f, "reinhard with exposure 2");
    const float n2[3] = {std::nanf(""), 0.2f, 0.2f};
    s = xr_stereo::shade(n2, true, 1.0f, Tonemap::clamp, 1.0f);
    check(s.r == 0.0f && std::fabs(s.g - 0.2f) < 1e-6f, "NaN -> 0");
}
void located_pose_fallback() {
    Rig g; g.stop_after = 5; g.src.pose_valid = false; g.build();
    auto r = g.run();
    check(r.success && g.out->counters().pose_from_located == r.begun, "unposed frames use the located views");
    check(g.xr->layers[0].views[0].pose.position[0] == -0.032f && g.xr->layers[0].views[1].fov.left == -0.9f, "located poses raw");
}

void session_loss_at(const std::string& op, bool hard) {
    Rig g; g.stop_after = 30; g.build();
    // inject on the 3rd occurrence
    g.xr->script[op] = {Rc::ok, Rc::ok, hard ? Rc::session_lost : Rc::loss_pending};
    auto r = g.run();
    const std::string what = "loss at " + op + (hard ? " (lost)" : " (pending)");
    check(r.loss_observed && !r.success, what + ": sticky loss");
    check(r.stop == StopReason::failure, what + ": failure stop");
    check(g.j.count("request_exit") == 0 && g.j.count("end_session") == 0, what + ": no exit handshake");
    check(g.j.count("destroy_session") == 1, what + ": session destroyed (children with it)");
    check(g.src.bad_releases == 0 && g.blit->executing == false, what + ": no slot/GPU violation");
    // nothing new after the loss
    long at = -1; auto calls = g.j.calls(); unsigned occ = 0;
    for (size_t i = 0; i < calls.size(); ++i) if (calls[i] == op && ++occ == 3) { at = long(i); break; }
    check(at > 0, what + ": injected");
    for (size_t i = size_t(at) + 1; i < calls.size(); ++i) check(calls[i] != "wait_frame" && calls[i] != "xr.acquire 0", what + ": no later frame");
    for (auto& kv : g.src.states) check(kv.second != St::psr, what + ": states restored");
}

void acquire_timeout_outstanding(bool retire_wait_ok) {
    Rig g; g.stop_after = 30; g.build();
    // frame 3, eye 1: both waits time out
    g.xr->hook["xr.acquire 1"] = [&g] {
        if (g.xr->waits == 3) { g.xr->wait_timeouts[1] = {true, true}; }
    };
    // retire's extra wait
    g.xr->hook["destroy_view_space"] = nullptr;
    auto r = g.run();
    (void)retire_wait_ok;
    check(g.out->failed() && g.out->failure().find("timeout; acquisition outstanding") != std::string::npos, "wait timeout disables output");
    check(r.first_cause == Cause::consumer && !r.success, "owner failure cause consumer");
    check(g.j.count("xr.wait 1 0") >= 3 && g.j.count("xr.wait 1 20000000") >= 2, "bounded retry then retire wait");
    check(g.j.count("xr.release 0") == 3, "eye 0 (waited) released at failure; never retried");
    check(g.src.releases == 3 && g.src.bad_releases == 0, "slot released (no GPU work referenced it)");
    check(g.j.prefix("d3d.execute") == 2, "no submission for the failed frame");
}

void acquire_timeout_retained() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.acquire 1"] = [&g] { if (g.xr->waits == 3) g.xr->wait_timeouts[1] = {true, true, true}; };
    auto r = g.run();
    check(g.out->failed() && !r.success && !r.clean && !r.consumer_retired, "retire refuses with an unwaited acquisition");
    check(g.j.count("xr.destroy 1") == 0 && g.j.count("destroy_session") == 0 && g.j.count("destroy_instance") == 0,
        "swapchain, session and instance retained");
    check(g.j.count("end_session") == 1, "exit handshake still performed");
    check(r.session_held && r.instance_held && r.binding_held, "handles reported held");
    check(g.has_line("xr-stereo;event=retire;outcome=retained;reason=unwaited acquisition outstanding"), "retained line");
}

void acquire_timeout_recovered_at_retire() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.acquire 1"] = [&g] { if (g.xr->waits == 3) g.xr->wait_timeouts[1] = {true, true, false}; };
    auto r = g.run();
    check(g.out->failed() && r.clean && r.consumer_retired && g.j.count("xr.destroy 1") == 1, "late wait at retire allows clean retirement");
    check(g.xr->violations == 0, "release only after the late wait");
}

void restoration_not_proven() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.wait 1 0"] = [&g] { if (g.xr->waits == 2) g.blit->restore = false; };
    auto r = g.run();
    check(g.out->failed() && g.out->failure().find("restoration not proven") != std::string::npos, "unproven restore disables");
    check(g.j.prefix("d3d.execute") == 1 && g.j.prefix("d3d.close") == 1, "never closed or submitted");
    check(r.clean && r.consumer_retired && g.src.bad_releases == 0 && g.src.releases == 2, "images and slot released; clean");
}

void completion_timeout_quarantine(bool late) {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.wait 1 0"] = [&g] { if (g.xr->waits == 2) g.blit->never_complete = true; };
    if (late) g.xr->hook["end_frame"] = [&g] { g.blit->never_complete = false; };
    auto r = g.run();
    check(g.out->failed() && g.out->failure() == "GPU completion pending; quarantine", "completion window expiry quarantines");
    check(g.has_line("xr-stereo;event=completion-pending;frame=2;frame_id=2;sync=queue-wait;first-submission=0;window-ms=200;"),
        "pending line: steady 200 ms window after a completed first submission");
    check(g.has_line(";private-fence=2;private-completed=1;producer-fence-value=20;producer-last-signalled=20;producer-complete=0;"),
        "pending line: private and producer fence facts");
    if (late) {
        check(g.has_line("xr-stereo;event=late-completion;private-fence=2;after-signal-ms-max="), "late completion measured at retire");
        check(r.consumer_retired && r.clean && g.src.bad_releases == 0 && g.src.releases == 2, "late completion discharges quarantine");
        long done = g.j.index("d3d.sample 2"), rel = g.j.index("xr.release 0", size_t(g.j.index("d3d.signal 2")));
        check(done > 0 && rel > done, "release after late completion");
    } else {
        check(!r.consumer_retired && !r.clean && g.src.releases == 1, "slot HELD while GPU work may reference it");
        check(g.j.count("xr.release 0") == 1 && g.j.count("xr.destroy 0") == 0 && g.j.count("d3d.destroy") == 0 && g.j.count("destroy_session") == 0,
            "images acquired, swapchains/session/private objects retained");
    }
}

// step14 run 2 (live, 1280x720, ~13 published/s, producer fence lag 2): the FIRST
// frame's producer fence was still pending (queue-wait) and the blit, queued on the
// game's queue behind its backlog, did not complete inside the old fixed 100 ms
// window -> 'GPU completion pending; quarantine' at frame 1, submitted=0.
void first_frame_backlog_completes() {
    Rig g; g.stop_after = 12; g.build();
    g.blit->complete_after = 400; // ~400 ms (1 ms sample spacing) for the first submission only
    g.xr->hook["xr.release 0"] = [&g] { g.blit->complete_after = 1; };
    auto r = g.run();
    check(!g.out->failed() && r.success && r.clean, "first-frame backlog (400 ms) completes inside the first-use window");
    check(g.j.count("d3d.gpu_wait") == r.begun && g.out->counters().cpu_presynced == 0, "producer fence pending: queue-wait path on every frame");
    check(g.has_line("xr-stereo;event=first-frame;frame=1;frame_id=1;") && g.has_line("sync=queue-wait;private-fence=1;") &&
        g.has_line(";completion-ms=399"), "first frame logged with its real completion time");
    check(g.out->counters().submitted == r.begun && !g.has_line("event=completion-pending"), "every frame submitted; nothing pending");
    check(g.src.bad_releases == 0 && g.xr->violations == 0, "release still only after completion");
}
void steady_backlog_inside_window() {
    // ~150 ms of game-queue backlog on EVERY frame: inside the 200 ms steady window.
    Rig g; g.stop_after = 10; g.build();
    g.blit->complete_after = 150;
    auto r = g.run();
    check(!g.out->failed() && r.success && r.clean && !r.policy_failure, "150 ms per-frame completion: no quarantine, no progress failure");
    check(g.out->counters().submitted == r.begun && g.src.bad_releases == 0, "all frames submitted, slots released after completion");
}
void first_window_capped_by_startup_reserve() {
    // Boundary = 5000 - 4990 = 10 ms; first wait alone takes 11 ms.
    // The old 200 ms floor consumed the reserve. Refuse before any GPU work.
    Rig g; g.stop_after = 12; g.settings.first_end_reserve_ms = 4990; g.build();
    auto r = g.run();
    check(g.out->failure() == "insufficient first-frame completion budget before submission" && r.begun == 1,
        "reserve leaves insufficient completion budget: explicit refusal");
    check(g.j.prefix("xr.acquire") == 0 && g.j.count("d3d.gpu_wait") == 0 && g.j.count("d3d.execute") == 0 &&
        g.j.prefix("d3d.signal") == 0, "no images or GPU work admitted over the reserve");
    check(r.clean && r.consumer_retired && g.src.releases == 1 && g.src.bad_releases == 0, "unused slot safely released");
    check(!xr_stereo::retry_allowed(1, r), "consumer failure: no retry (unchanged)");
    // PERF: the consumer reports the budget refusal; the cold-start policy then allows the ONE retry (step65 launch 1)
    check(g.out->startup_budget_refused() && r.first_cause == Cause::consumer && xr_stereo::retry_allowed(1, r, g.out->startup_budget_refused()),
        "first-frame budget refusal (nothing submitted): one cold-start retry");
    check(!xr_stereo::retry_allowed(2, r, true), "never a second retry for the budget refusal");
}
void first_frame_setup_budget(int64_t setup_ms, int64_t startup_ms, bool admitted) {
    Rig g; g.stop_after = 12; g.limits.startup_ms = startup_ms; g.build();
    // Advance during first-frame setup, using the REAL OwnerGate and owner limit.
    g.xr->hook["locate_head"] = [&g, setup_ms] { g.clock->t = setup_ms; };
    g.blit->never_complete = true;
    const auto r = g.run();
    const int64_t boundary = startup_ms - g.settings.first_end_reserve_ms;
    if (!admitted) {
        check(g.out->failure() == "insufficient first-frame completion budget before submission", "late setup explicitly refused");
        check(g.j.prefix("xr.acquire") == 0 && g.j.count("d3d.execute") == 0 && g.j.count("d3d.gpu_wait") == 0 &&
            g.j.prefix("d3d.signal") == 0, "late setup refused before GPU submission");
        check(r.clean && r.consumer_retired && g.src.releases == 1 && g.src.bad_releases == 0, "late setup cleans up unused frame");
    } else {
        const int64_t window = std::min<int64_t>(1000, boundary - setup_ms);
        check(g.out->failure() == "GPU completion pending; quarantine", "admitted setup retains pending GPU work");
        check(g.out->ledger().signal_ms == setup_ms, "setup clock reached Signal unchanged");
        check(g.has_line("first-submission=1;window-ms=" + std::to_string(window) + ";waited-ms=" + std::to_string(window) + ";"),
            "polling window uses actual owner limit minus reserve");
        check(g.has_line(";owner-t-ms=" + std::to_string(setup_ms + window)), "polling stops at calculated deadline");
        check(setup_ms + window <= boundary, "deadline never exceeds reserve boundary");
        check(!r.clean && !r.consumer_retired && g.out->ledger().holding && g.j.count("xr.release 0") == 0 &&
            g.j.count("d3d.destroy") == 0 && g.j.count("destroy_session") == 0, "pending images, slot and parents quarantined");
    }
    check(!r.policy_failure && r.first_cause == Cause::consumer, "owner hard deadline policy unchanged");
}
void first_frame_budget_consumed_before_submission() {
    Rig g; g.stop_after = 12; g.build();
    g.xr->hook["locate_head"] = [&g] { g.clock->t = 3500; };
    g.xr->hook["xr.wait 1 0"] = [&g] { g.clock->t = 3900; };
    const auto r = g.run();
    check(g.out->failure() == "insufficient first-frame completion budget before submission", "budget rechecked after recording");
    check(g.j.count("d3d.close") == 1 && g.j.count("d3d.gpu_wait") == 0 && g.j.count("d3d.execute") == 0,
        "setup passed initial admission but no GPU work submitted with insufficient remaining budget");
    check(r.clean && r.consumer_retired && g.src.bad_releases == 0 && g.src.releases == 1 && g.xr->violations == 0,
        "waited images and unused slot safely settled");
}
void first_frame_budget_consumed_after_submission(int64_t signal_ms) {
    Rig g; g.stop_after = 12; g.build();
    g.xr->hook["locate_head"] = [&g] { g.clock->t = 3500; };
    g.blit->signal_hook = [&g, signal_ms] { g.clock->t = signal_ms; };
    g.blit->never_complete = true;
    const auto r = g.run();
    check(g.out->failure() == "GPU completion pending; quarantine", "submission consumes budget: quarantine preserved");
    check(g.has_line("first-submission=1;window-ms=" + std::to_string(4000 - signal_ms) +
        ";waited-ms=" + std::to_string(std::max<int64_t>(0, 4000 - signal_ms)) + ";"), "no steady floor after Signal");
    if (signal_ms >= 4000) {
        const auto calls = frame_slice(g.j.calls(), 0);
        check(std::none_of(calls.begin(), calls.end(), [](const std::string& s) { return s.find("d3d.sample ") == 0; }),
            "no frame completion poll beyond reserve boundary (cleanup may observe later)");
    }
    check(!r.clean && !r.consumer_retired && g.out->ledger().submitted && g.out->ledger().holding &&
        g.j.count("xr.release 0") == 0 && g.j.count("xr.destroy 0") == 0 && g.j.count("d3d.destroy") == 0 &&
        g.j.count("destroy_session") == 0, "GPU work may be pending: no release or destruction");
}
void first_window_still_bounded() {
    // A first submission slower than the first-use window still quarantines (bounded).
    Rig g; g.stop_after = 12; g.build();
    g.blit->never_complete = true;
    auto r = g.run();
    check(g.out->failed() && g.out->failure() == "GPU completion pending; quarantine", "never-completing first frame quarantines");
    check(g.has_line("first-submission=1;window-ms=1000;waited-ms=1000;"), "first-use window is 1000 ms");
    check(!r.consumer_retired && !r.clean && g.j.count("d3d.destroy") == 0 && g.j.count("destroy_session") == 0,
        "pending GPU work: images, slot, swapchains, private objects and session retained");
    check(g.out->ledger().holding && g.out->ledger().submitted && !g.out->ledger().complete, "slot held while GPU work may reference it");
}

void execute_ambiguous() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.wait 1 0"] = [&g] { if (g.xr->waits == 2) { g.blit->execute_ok = false; } };
    auto r = g.run();
    check(g.out->failed() && g.out->ledger().ambiguous && !r.consumer_retired, "ambiguous Execute retains everything");
    check(g.j.count("d3d.signal 2") == 0 && g.j.count("destroy_session") == 0, "no signal; session retained");
}

void execute_refused_cleanly() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->hook["xr.wait 1 0"] = [&g] { if (g.xr->waits == 2) { g.blit->execute_ok = false; g.blit->execute_dispatch = false; } };
    auto r = g.run();
    check(g.out->failed() && !g.out->ledger().ambiguous && r.consumer_retired && r.clean, "refused (no dispatch) Execute is clean");
}

void invalid_frame() {
    Rig g; g.stop_after = 30; g.src.format = 28; g.build();
    auto r = g.run();
    check(g.out->failed() && g.out->failure() == "source format not FP16/FP32 RGBA" && g.src.releases == 1, "invalid frame released, disabled");
    check(g.j.prefix("xr.acquire") == 0 && r.clean, "nothing acquired; clean");
}

void setup_refusals() {
    { Rig g; g.xr->offered = {87, 2}; g.stop_after = 5; g.build();
      auto r = g.run();
      check(r.first_cause == Cause::consumer && !r.session_begun && g.j.count("begin_session") == 0, "no RGBA8: startup refused");
      check(g.j.count("d3d.setup") == 0 && r.clean, "nothing created; clean"); }
    { Rig g; g.stop_after = 5; g.build(); g.xr->script["xr.create 1"] = {Rc::error};
      auto r = g.run();
      check(r.first_cause == Cause::consumer && g.j.count("xr.destroy 0") == 1 && g.j.count("xr.destroy 1") == 0 && g.j.count("d3d.destroy") == 1 && r.clean,
          "partial setup retired"); }
    { Rig g; g.stop_after = 5; g.xr->cfg[1].recommended_w = 9000; g.build();
      auto r = g.run();
      check(r.first_cause == Cause::consumer && g.j.prefix("xr.create") == 0, "oversized view refused"); }
    { Rig g; g.stop_after = 5; g.blit->setup_ok = false; g.build();
      auto r = g.run();
      check(r.first_cause == Cause::consumer && g.j.prefix("xr.create") == 0 && g.j.count("d3d.destroy") == 1 && r.clean, "blit setup failure"); }
    { Rig g; g.build(); auto o2 = std::make_shared<xr_stereo::Output>(nullptr, nullptr, nullptr, nullptr);
      check(!g.owner->set_consumer(o2), "second consumer refused"); }
}

void end_frame_failure_not_retried() {
    Rig g; g.stop_after = 30; g.build();
    g.xr->script["end_frame_projection"] = {Rc::ok, Rc::ok, Rc::error};
    auto r = g.run();
    check(r.first_cause == Cause::end_frame_failed && g.j.count("end_frame_projection") == 3 && g.j.count("end_frame") == 0, "failed layer end never retried");
    check(r.consumer_retired && r.clean, "clean retirement after end failure");
}

void slot_contention() {
    // The producer outpaces the consumer: three publishes per frame; the newest wins, older ones are skipped.
    Rig g; g.stop_after = 10; g.build();
    g.xr->hook["wait_frame"] = [&g] { g.src.publish(); g.src.publish(); g.src.publish(); if (g.xr->waits + 1 >= 10) g.owner->request_stop(); };
    auto r = g.run();
    check(r.success && g.out->counters().frame_id_gaps > 0 && g.src.bad_releases == 0, "latest frame consumed; gaps counted");
    check(g.src.skipped == 0, "producer never blocked by the consumer (one slot held at most)");
}

void watchdog_retracts_pose() {
    Rig g; g.stop_after = 0; g.build();
    g.xr->hook["wait_frame"] = [&g] { g.src.publish(); if (g.xr->waits == 3) g.clock->t += 300; }; // a 300 ms foreign call
    auto r = g.run();
    check(r.policy_failure && r.first_cause == Cause::progress_deadline, "250 ms progress rule in continuous mode");
    PoseSnapshot s;
    check(!g.owner->try_read_pose(s), "pose retracted");
    check(r.consumer_retired && r.clean, "consumer retired after a policy failure");
}

// J-xr-stereo-output run-1 regression: the cold first xrEndFrame took 1481 ms.
void first_frame_is_startup_budget() {
    Rig g; g.stop_after = 12; g.publish_from = 1; g.build();
    unsigned ends = 0;
    g.xr->hook["end_frame"] = [&] { if (++ends == 1) g.clock->t += 1481; };
    auto r = g.run();
    check(r.success && !r.policy_failure && r.first_cause == Cause::none, "1481 ms first end inside the startup budget");
    check(r.first_end_ms == 1481 && r.first_wait_ms == 11 && r.first_begin_ms == 0 && r.first_frame_done_ms > 1481, "first-frame durations recorded");
    check(g.has_line("xr-stereo;event=first-xr-frame;wait-ms=11;begin-ms=0;end-ms=1481;end-rc=ok;layer=0"), "first-xr-frame line (zero layers before the first StereoFrame)");
    check(r.projection_layers == r.begun - 1, "layers from the second frame on (first frame: no StereoFrame consumed yet)");
}
void second_frame_keeps_progress_rule() {
    Rig g; g.stop_after = 30; g.publish_from = 1; g.build();
    unsigned ends = 0;
    g.xr->hook["end_frame_projection"] = [&] { if (++ends == 1) g.clock->t += 300; }; // frame 2
    g.xr->hook["end_frame"] = [&] { g.clock->t += 200; };                          // frame 1: under the startup budget
    auto r = g.run();
    check(r.policy_failure && r.first_cause == Cause::progress_deadline && r.begun == 2, "250 ms progress rule from frame 2");
    check(r.consumer_retired && r.clean, "clean retirement");
    check(xr_stereo::retry_allowed(1, r), "progress-deadline cold start: one retry allowed");
    check(!xr_stereo::retry_allowed(2, r), "never a second retry");
}
void first_frame_over_startup() {
    Rig g; g.stop_after = 30; g.publish_from = 1; g.build();
    g.xr->hook["end_frame"] = [&] { g.clock->t += 6000; };
    auto r = g.run();
    check(r.policy_failure && r.first_cause == Cause::startup_deadline && r.begun == 1 && r.ended == 1, "first frame past 5 s: startup deadline");
    check(r.clean && xr_stereo::retry_allowed(1, r), "cold-start timeout retried once");
}
void retry_policy() {
    Report r; r.complete = true; r.clean = true; r.first_cause = Cause::ready_timeout;
    check(xr_stereo::retry_allowed(1, r), "READY timeout retried");
    r.loss_observed = true; check(!xr_stereo::retry_allowed(1, r), "never after a loss");
    r.loss_observed = false; r.clean = false; check(!xr_stereo::retry_allowed(1, r), "never after an unclean retirement");
    r.clean = true; r.session_held = true; check(!xr_stereo::retry_allowed(1, r), "never with a retained session");
    r.session_held = false; r.first_cause = Cause::consumer; check(!xr_stereo::retry_allowed(1, r), "not for a consumer failure");
    // PERF: a consumer failure is retried ONLY when it is the first-frame startup-budget refusal, with every other condition unchanged
    check(!xr_stereo::retry_allowed(1, r, false), "consumer failure without the budget flag: no retry");
    check(xr_stereo::retry_allowed(1, r, true), "consumer budget refusal: retried once");
    r.loss_observed = true; check(!xr_stereo::retry_allowed(1, r, true), "budget refusal: never after a loss");
    r.loss_observed = false; r.clean = false; check(!xr_stereo::retry_allowed(1, r, true), "budget refusal: never after an unclean retirement");
    r.clean = true; r.binding_held = true; check(!xr_stereo::retry_allowed(1, r, true), "budget refusal: never with a held binding");
    r.binding_held = false; r.first_cause = Cause::startup_call; check(!xr_stereo::retry_allowed(1, r, true), "the flag never widens other causes");
    r.first_cause = Cause::consumer;
    r.first_cause = Cause::startup_call; check(!xr_stereo::retry_allowed(1, r), "not for a startup call failure");
    r.first_cause = Cause::startup_deadline; r.success = true; check(!xr_stereo::retry_allowed(1, r), "not after success");
    Journal j; FakeSource s(j);
    check(!xr_stereo::start_ready(nullptr) && !xr_stereo::start_ready(&s), "no source / nothing published: not started");
    s.publish(); check(xr_stereo::start_ready(&s), "first published StereoFrame starts the session");
}
std::atomic<int> g_depth{0}, g_window{0}, g_enters{0}, g_window_enters{0}, g_bad{0};
void bracket(bool enter, bool window) noexcept {
    if (enter) { ++g_enters; if (window) { ++g_window_enters; ++g_window; } if (++g_depth != 1) ++g_bad; }
    else { if (--g_depth != 0) ++g_bad; if (window) --g_window; }
}
void call_bracket_covers_every_call() {
    xr_owner::set_call_bracket(&bracket);
    Rig g; g.stop_after = 6; g.build();
    auto r = g.run();
    xr_owner::set_call_bracket(nullptr);
    check(r.success && g_bad == 0 && g_depth == 0 && g_window == 0, "bracket balanced, never nested");
    check(unsigned(g_enters.load()) == r.runtime_calls, "every runtime/consumer call bracketed");
    check(g_window_enters > 0 && g_window_enters < g_enters, "blocking calls (wait/poll/locate/sample) open no window");
}


// Issue 29 (2): never a GPU Wait on the game's queue for an unauthenticated value.
void fence_authentication() {
    { Rig g; g.stop_after = 8; g.src.signal_lag = 1; g.build(); // value beyond the producer's last signalled value
      g.xr->hook["wait_frame"] = [&g] { g.src.publish(); if (g.xr->waits == 3) g.blit->producer_complete = true;
          if (g.xr->waits + 1 >= 8) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && g.j.count("d3d.gpu_wait") == 0, "unauthenticated value: never a queue Wait");
      check(c.deferred == 3 && c.new_frames == r.begun - 3 && c.cpu_presynced == r.begun - 3 && g.src.bad_releases == 0,
          "deferred (released unused) until CPU-observed completion, then used");
      check(g.j.prefix("xr.acquire") == 2 * (r.begun - 3) && g.src.releases == c.acquired_frames, "no image acquired for a deferred frame");
      check(c.zero_no_frame == 3, "nothing to reuse before the first frame"); }
    { Rig g; g.stop_after = 6; g.src.epoch = 0; g.build(); // producer gave no epoch
      auto r = g.run();
      check(r.success && g.j.count("d3d.gpu_wait") == 0 && g.out->counters().deferred > 0 && g.out->counters().new_frames == 0, "no epoch: deferred"); }
    { Rig g; g.stop_after = 10; g.build(); // fence pointer changes after registration
      g.xr->hook["wait_frame"] = [&g] { if (g.xr->waits == 3) g.src.swap_fence = true; g.src.publish(); if (g.xr->waits + 1 >= 10) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && g.j.count("d3d.gpu_wait") == 3 && c.deferred >= 6 && c.new_frames == 3, "other fence than the registered one: deferred"); }
    { Rig g; g.stop_after = 6; g.blit->producer_complete = true; g.src.epoch = 0; g.src.signal_lag = 5; g.build();
      auto r = g.run();
      check(r.success && g.out->counters().deferred == 0 && g.out->counters().cpu_presynced == r.begun, "CPU-observed completion needs no authentication"); }
}
// Issue 29 (3): reuse only while tracked and in the same LOCAL space generation.
void reuse_validity() {
    { Rig g; g.stop_after = 12; g.publish_until = 2; g.build();
      g.xr->hook["begin_frame"] = [&g] { if (g.xr->waits == 5) g.xr->views.view_state_flags = valid_bits; }; // tracking lost from frame 5
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.reused == 2 && c.stale_tracking == r.begun - 4 && c.stale_age == 0, "tracking lost: no reuse"); }
    { Rig g; g.stop_after = 12; g.publish_until = 2; g.build();
      g.xr->hook["wait_frame"] = [&g] {
          if (g.xr->waits < 2) g.src.publish();
          if (g.xr->waits == 4) { Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local;
              e.change_time = 1000000000 + 6 * g.xr->period; g.xr->events.push_back(e); }
          if (g.xr->waits + 1 >= 12) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.reused == 3 && c.stale_space == 1 && c.stale_zero == r.begun - 5, "LOCAL space change: zero from the change, never reused again"); }
}
// Issue 29 (4): counts are submissions, never delivery/presentation claims.
void labels() {
    Rig g; g.stop_after = 6; g.build();
    g.run();
    for (auto& l : g.lines) {
        std::string low = l; for (auto& ch : low) ch = char(std::tolower((unsigned char)ch));
        check(low.find("present") == std::string::npos && low.find("deliver") == std::string::npos && low.find("display") == std::string::npos,
            "no delivery/presentation/display claim: " + l);
    }
    check(g.has_line("counts=submitted-means-accepted-by-xrEndFrame") && g.has_line("event=summary;") && g.has_line("no-headset-claim"), "submission semantics stated");
}


// Issue 29 verdict 2: explicit producer fence registration is the only trust anchor.
void fence_registration() {
    { Rig g; g.stop_after = 6; g.build(); xr_stereo::register_producer_fence(nullptr, 0); // nothing registered
      auto r = g.run();
      check(r.success && g.j.count("d3d.gpu_wait") == 0 && g.out->counters().deferred == r.begun && g.out->counters().new_frames == 0,
          "unregistered: no GPU wait; the first frame never establishes trust"); }
    { Rig g; g.stop_after = 6; g.build(); xr_stereo::register_producer_fence(&g.src.fence_token, 2); // epoch replaced
      auto r = g.run();
      check(r.success && g.j.count("d3d.gpu_wait") == 0 && g.out->counters().deferred == r.begun, "epoch mismatch: deferred"); }
    { Rig g; g.stop_after = 6; g.build(); xr_stereo::register_producer_fence(nullptr, 0); g.blit->producer_complete = true;
      auto r = g.run();
      check(r.success && g.out->counters().cpu_presynced == r.begun && g.out->counters().deferred == 0, "unregistered: CPU-observed completion still works"); }
    check(!xr_stereo::producer_fence_registered(nullptr, 1) && !xr_stereo::producer_fence_registered(&checks, 0), "null fence / zero epoch never trusted");
    int x = 0; xr_stereo::register_producer_fence(&x, 7);
    check(xr_stereo::producer_fence_registered(&x, 7) && !xr_stereo::producer_fence_registered(&x, 8), "registration matches fence AND epoch");
}
// Verdict 2 (5): a delayed pre-recenter render is never relabelled into the new space.
void fresh_space_and_tracking() {
    { Rig g; g.stop_after = 12; g.src.with_world = true; g.src.lens_valid = true; g.build();
      g.xr->hook["wait_frame"] = [&g] {
          const unsigned w = g.xr->waits;
          if (w == 3) { Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local;
              e.change_time = 1000000000 + 5 * g.xr->period; g.xr->events.push_back(e); }
          g.src.space_gen = w >= 7 ? 2 : 1;  // the producer renders under the new generation from frame 8
          g.src.publish();
          if (w + 1 >= 12) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      auto last = [&](unsigned f) { auto s = frame_slice(g.j.calls(), f); return s.empty() ? std::string() : s.back(); };
      check(r.success && c.fresh_stale_space == 3 && c.new_frames == r.begun - 3, "frames rendered under generation N consumed in N+1: released unused");
      check(last(3) == "end_frame_projection" && last(4) == "end_frame" && last(6) == "end_frame" && last(7) == "end_frame_projection",
          "no relabelled old-space layer; zero until a render of the new generation");
      check(c.stale_space >= 1 && g.src.bad_releases == 0 && g.j.count("source.release 5") == 1, "old layers not reused across the change");
      // World anchor rebased: first world frame of generation 1 is frame_id 8 (eye midpoint z = 0.8).
      const auto& before = g.xr->layers[0];
      const auto& after = g.xr->layers.back();
      check(std::fabs(before.quad_pose.position[2] - (0.1f - 40.0f)) < 1e-4f && std::fabs(after.quad_pose.position[2] - (0.8f - 40.0f)) < 1e-4f &&
          c.anchor_rebases == 1, "world-quad anchor re-captured after the space change"); }
    { Rig g; g.stop_after = 10; g.build(); // every frame published; tracking lost from frame 3
      g.xr->hook["begin_frame"] = [&g] { if (g.xr->waits == 3) g.xr->views.view_state_flags = valid_bits; };
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.new_frames == 2 && c.fresh_stale_tracking == r.begun - 2 && r.projection_layers == 2,
          "fresh frames while tracking is invalid: released unused, zero layers");
      check(g.src.releases == c.acquired_frames && g.j.prefix("xr.acquire") == 4, "no image acquired for a refused fresh frame"); }
    { Rig g; g.stop_after = 6; g.src.space_gen = 0; g.build(); // producer has not filled the generation yet
      auto r = g.run();
      check(r.success && g.out->counters().space_unknown == r.begun && g.out->counters().new_frames == r.begun,
          "unknown generation accepted only while no space change has happened (counted)"); }
}
// J-155 F4: feed the actual producer camera/neutral/substitution maths into the
// consumer fixture across a translated AND rotated LOCAL epoch. No XR runtime.
void producer_neutral_space_coupling() {
    Rig g;g.stop_after=12;g.build();
    using namespace gpu_copy::step5;
    Neutral neutral;Scratch scratch;PoseFrame produced;
    Camera camera{};camera.right[0]=camera.up[1]=1;camera.fwd[2]=-1;
    camera.n3=1;camera.tanX=0.6;camera.tanY=0.35;camera.A=0.99;camera.C=-0.2;
    double vp[16];build_vp(camera.eye,camera.right,camera.up,camera.fwd,-0.6,0.6,0.35,-0.35,camera,vp);
    std::array<uint8_t,416> raw{};
    for(unsigned i=0;i<16;++i){float v=float(vp[i]),identity=i%5==0?1.0f:0.0f;
        std::memcpy(raw.data()+4*i,&v,4);std::memcpy(raw.data()+64+4*i,&v,4);std::memcpy(raw.data()+256+4*i,&identity,4);}
    std::array<uint8_t,416> original[2]{},rendered[2]{};bool compared=false;
    g.xr->hook["wait_frame"]=[&] {
        const auto w=g.xr->waits;
        if(w==3){Event e;e.type=EventType::reference_space_change;e.space=SpaceType::local;
            e.change_time=1000000000+5*g.xr->period;g.xr->events.push_back(e);}
        const bool newSpace=w>=7;const uint32_t generation=newSpace?2:1;
        const double yaw=newSpace?1.0471975511965976:0;
        stereo::EyePose eyes[2]{};
        for(unsigned eye=0;eye<2;++eye) {
            const double x=eye?0.031:-0.031;
            eyes[eye].position[0]=float((newSpace?2:0)+std::cos(yaw)*x);
            eyes[eye].position[1]=newSpace?2.7f:1.2f;
            eyes[eye].position[2]=float((newSpace?-3:0)-std::sin(yaw)*x);
            eyes[eye].orientation[1]=float(std::sin(yaw/2));eyes[eye].orientation[3]=float(std::cos(yaw/2));
            eyes[eye].fov[0]=-0.8f;eyes[eye].fov[1]=0.7f;eyes[eye].fov[2]=0.75f;eyes[eye].fov[3]=-0.85f;
            g.xr->views.eyes[eye].pose.position[0]=eyes[eye].position[0];
            g.xr->views.eyes[eye].pose.position[1]=eyes[eye].position[1];
            g.xr->views.eyes[eye].pose.position[2]=eyes[eye].position[2];
            for(unsigned k=0;k<4;++k)g.xr->views.eyes[eye].pose.orientation[k]=eyes[eye].orientation[k];
        }
        Lens lens;check(!camera_frame(raw.data(),produced,scratch,lens),"producer camera decoded");
        check(!setup_poses_generation(eyes,generation,neutral,produced,scratch),"producer generation-aware pose built");
        for(unsigned eye=0;eye<2;++eye) {
            check(!pose_substitute(raw.data(),produced,eye,rendered[eye].data(),scratch),"producer native upload bytes computed");
            if(w==0)original[eye]=rendered[eye];
            if(newSpace)for(unsigned i=0;i<16;++i) {
                float before=0,after=0;std::memcpy(&before,original[eye].data()+64+4*i,4);std::memcpy(&after,rendered[eye].data()+64+4*i,4);
                check(std::fabs(before-after)<1e-5f,"same physical neutral gives same rendered matrix after translated/rotated LOCAL change");
            }
        }
        if(newSpace)compared=true;
        g.src.space_gen=generation;check(g.src.publish(),"producer frame available");
        for(auto& sl:g.src.slots)if(sl.s==FakeSource::S::published&&sl.f.frame_id==g.src.next_id-1) {
            sl.f.pose[0]=eyes[0];sl.f.pose[1]=eyes[1];sl.f.pose_valid=produced.valid;
            sl.f.lens_valid=lens.valid;for(unsigned k=0;k<4;++k)sl.f.lens_tan[k]=lens.tan[k];
        }
        if(w+1>=12)g.owner->request_stop();
    };
    const auto result=g.run();const auto& counts=g.out->counters();
    check(result.success&&compared&&counts.fresh_stale_space==3&&counts.new_frames==result.begun-3,
        "consumer rejects delayed old-space renders then accepts rebased producer output");
    const auto stale=frame_slice(g.j.calls(),4),fresh=frame_slice(g.j.calls(),7);
    check(!stale.empty()&&stale.back()=="end_frame"&&!fresh.empty()&&fresh.back()=="end_frame_projection",
        "actual consumer submission stream switches from zero layers to rebased stereo");
    check(!g.xr->layers.empty()&&g.xr->layers.back().views[0].pose.position[0]>1.9f&&
        std::fabs(g.xr->layers.back().views[0].pose.orientation[1]-0.5f)<1e-5f,
        "accepted native layer carries translated and rotated new-space pose");
}
// Verdict 2 (6): invalid lens values never reach the quad (no destroyed temporary).
void world_lens_invalid() {
    Rig g; g.stop_after = 6; g.src.with_world = true; g.src.lens_valid = false;
    g.src.lens[0] = std::nanf(""); g.src.lens[1] = -1.0f; g.src.lens[2] = 1e30f; g.src.lens[3] = 0.0f;
    g.build();
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && c.world_lens_invalid == r.begun && c.world_frames == 0 && g.j.prefix("xr.acquire 2") == 0, "invalid lens: no world quad, counted");
    for (auto& l : g.xr->layers) check(!l.quad && std::isfinite(l.views[0].fov.left) && l.views[0].fov.left == -0.8f, "cockpit keeps its own (pose) FOV");
    check(g.has_line("world-lens-invalid="), "summary counts world-lens-invalid");
    for (auto& l : g.lines) if (l.find("event=summary;") != std::string::npos)
        check(l.size() >= 16 && l.compare(l.size() - 16, 16, "no-headset-claim") == 0, "summary line not truncated: " + l);
}

void continuous_has_no_caps() {
    Rig g; g.stop_after = 400; g.build();
    auto r = g.run();
    check(r.success && r.begun >= 398 && r.begun > Limits{}.max_frames, "continuous: no 120-frame cap");
    check(r.records_overflow > 0 && r.records_lost == 0, "continuous: record overflow counted, not a failure");
    check(g.clock->t > Limits{}.absolute_ms / 4, "ran long");
    check(g.has_line("xr-stereo;event=milestone;result=PROVISIONAL"), "milestone line after 300 layers");
}

// ------------------------------------------------------- J-xr-log routing ---
std::wstring g_log_dir;
std::string dedicated_log() { // as a reader sees it while the game runs (shared read/write)
    const std::wstring path = g_log_dir + L"\\xr-stereo.log";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out; char buf[4096]; DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}
// The payload of every dedicated line, in order, with the `<ms>|` prefix checked and removed.
std::vector<std::string> dedicated_payloads() {
    std::vector<std::string> out;
    const std::string all = dedicated_log();
    size_t pos = 0;
    while (pos < all.size()) {
        const size_t nl = all.find('\n', pos);
        check(nl != std::string::npos, "dedicated line newline-terminated");
        const std::string line = all.substr(pos, nl - pos);
        const size_t bar = line.find('|');
        check(bar != std::string::npos && bar > 0 && line.find_first_not_of("0123456789") == bar, "dedicated line has only a <ms>| prefix");
        out.push_back(line.substr(bar + 1));
        pos = nl + 1;
    }
    return out;
}
struct SharedLine { bool startup; std::string text; };
std::vector<SharedLine> g_shared;
bool g_logging = false;
void fake_shared(bool startup, const char* text) noexcept { g_shared.push_back({startup, text}); }
bool fake_enabled() noexcept { return g_logging; }

void evidence_route_policy() {
    using namespace xr_stereo::evidence;
    auto same = [](Route r, bool d, Shared s, bool f) { return r.dedicated == d && r.shared == s && r.flush == f; };
    check(same(route(true, 0, true), true, Shared::startup, true) && same(route(true, 47, true), true, Shared::startup, true),
        "lifecycle: dedicated + flush, startup-partition mirror");
    check(same(route(true, 48, true), true, Shared::none, true), "lifecycle over cap: dedicated only");
    check(same(route(false, 0, true), true, Shared::none, false), "periodic: dedicated only, never the shared ordinary ring");
    check(same(route(true, 0, false), false, Shared::startup, false) && same(route(true, 48, false), false, Shared::ordinary, false) &&
          same(route(false, 0, false), false, Shared::ordinary, false), "no dedicated file: legacy shared routing");
}

void evidence_router_lines() {
    namespace fl = acz::feature_log;
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    g_log_dir = std::wstring(tmp) + L"acz-xr-log-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_log_dir.c_str(), nullptr);
    DeleteFileW((g_log_dir + L"\\xr-stereo.log").c_str());
    DeleteFileW((g_log_dir + L"\\xr-stereo.prev.log").c_str());
    fl::set_directory(g_log_dir.c_str());
    // Logging disabled: nothing opened; exactly the legacy shared routing.
    {
        g_logging = false; g_shared.clear();
        xr_stereo::evidence::Router r(&fake_shared, &fake_enabled);
        r.emit(true, "xr-stereo;event=refused;reason=off");
        r.emit(false, "xr-stereo;event=summary;off");
        r.emit(true, nullptr);
        check(r.dedicated_handle() == -2 && dedicated_log().empty(), "disabled: dedicated file not opened");
        check(g_shared.size() == 2 && g_shared[0].startup && g_shared[0].text == "xr-stereo;event=refused;reason=off" &&
              !g_shared[1].startup && g_shared[1].text == "xr-stereo;event=summary;off", "disabled: legacy startup/ordinary lines");
    }
    // Enabled: the dedicated file is the record; lifecycle lines drain on the I/O owner.
    g_logging = true; g_shared.clear();
    xr_stereo::evidence::Router r(&fake_shared, &fake_enabled);
    r.emit(true, "xr-stereo;event=armed;source-pin=test");
    check(r.wait_idle(), "router opened on I/O owner");
    check(r.dedicated_handle() >= 0 && fl::stats(r.dedicated_handle()).open, "enabled: xr-stereo.log opened");
    auto p = dedicated_payloads();
    check(p.size() == 1 && p[0] == "xr-stereo;event=armed;source-pin=test", "lifecycle line drained asynchronously, text unchanged");
    check(g_shared.size() == 1 && g_shared[0].startup && g_shared[0].text == "xr-stereo;event=armed;source-pin=test", "lifecycle mirrored to startup partition");
    r.emit(false, "xr-stereo;event=summary;t-ms=1");
    check(g_shared.size() == 1, "periodic line never reaches the shared log");
    fl::flush();
    check(r.wait_idle(), "router lifecycle batch drained");
    p = dedicated_payloads();
    check(p.size() == 2 && p[1] == "xr-stereo;event=summary;t-ms=1", "periodic line in the dedicated log");
    // Lifecycle cap: 48 mirrored in total, every line in the dedicated file.
    for (int i = 1; i < 60; ++i) r.emit(true, ("xr-stereo;event=cap-probe;i=" + std::to_string(i)).c_str());
    unsigned startup = 0, ordinary = 0;
    for (auto& s : g_shared) (s.startup ? startup : ordinary) += 1;
    check(startup == xr_stereo::evidence::lifecycle_cap && ordinary == 0, "48 lifecycle mirrors, none spill into the ordinary ring");
    check(r.wait_idle(), "all lifecycle lines drained");
    p = dedicated_payloads();
    check(p.size() == 61 && p.back() == "xr-stereo;event=cap-probe;i=59", "every lifecycle line in the dedicated log");
    // A second router (e.g. a later attempt) shares the same file.
    xr_stereo::evidence::Router r2(&fake_shared, &fake_enabled);
    r2.emit(true, "xr-stereo;event=second");
    check(r2.wait_idle(), "second router drained");
    check(r2.dedicated_handle() == r.dedicated_handle() && dedicated_payloads().back() == "xr-stereo;event=second", "same dedicated handle");
}

// Full consumer run through the router: the dedicated file holds exactly the
// consumer's lines, in order, unchanged; periodic summaries stay out of the shared log.
void evidence_consumer_run() {
    namespace fl = acz::feature_log;
    fl::flush();
    const size_t before = dedicated_payloads().size();
    g_logging = true; g_shared.clear();
    xr_stereo::evidence::Router router(&fake_shared, &fake_enabled);
    Rig g; g.stop_after = 400;
    g.tee = [&router](bool life, const char* line) { router.emit(life, line); };
    g.build();
    auto r = g.run();
    check(r.success, "router run succeeds");
    check(router.shutdown(), "final consumer line drained and I/O owner joined");
    auto p = dedicated_payloads();
    check(p.size() == before + g.lines.size(), "every consumer line reached the dedicated log");
    check(std::equal(g.lines.begin(), g.lines.end(), p.begin() + static_cast<std::ptrdiff_t>(before)), "dedicated lines identical and in order");
    unsigned summaries = 0;
    for (auto& l : g.lines) summaries += l.rfind("xr-stereo;event=summary", 0) == 0 ? 1u : 0u;
    check(summaries >= 3 && g.lines.size() > g.lifecycle, "run produced periodic summaries");
    check(g_shared.size() == g.lifecycle && g.lifecycle <= xr_stereo::evidence::lifecycle_cap, "shared log saw only the lifecycle lines");
    for (auto& s : g_shared) check(s.startup && s.text.rfind("xr-stereo;event=summary", 0) != 0, "no summary in the shared log");
    check(!g.lines.empty() && p.back() == g.lines.back() && g.lines.back().rfind("xr-stereo;", 0) == 0, "final line present and last");
}

// These cases run separately under CTest, with fresh process-wide logger state.
void evidence_sink_case(const std::string& mode) {
    namespace ff=feature_log_fixture;
    namespace fl=acz::feature_log;
    ff::setup();ff::Sink sink;
    sink.blocked=mode=="slow";sink.fail=mode=="failing";sink.piece=mode=="partial"?3:MAXDWORD;
    fl::set_sink(&ff::Sink::put,&sink);
    xr_stereo::evidence::Router router(nullptr,+[]() noexcept{return true;});
    router.emit(true,"xr-stereo;event=prime");
    check(WaitForSingleObject(sink.entered,2000)==WAIT_OBJECT_0,"consumer sink entered");
    Rig g;g.stop_after=400;
    g.tee=[&](bool life,const char* line){router.emit(life,line);};g.build();
    if(mode=="slow") {
        HANDLE progressed=CreateEventW(nullptr,TRUE,FALSE,nullptr);bool success=false;
        HANDLE finalPending=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE allowFinal=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        g.tee=[&](bool life,const char* line){
            if(std::strncmp(line,"xr-stereo;result=",17)==0){SetEvent(finalPending);WaitForSingleObject(allowFinal,5000);}
            router.emit(life,line);
        };
        std::thread consumer([&]{success=g.run().success;SetEvent(progressed);});
        const bool pending=WaitForSingleObject(finalPending,2000)==WAIT_OBJECT_0;
        const bool earlyFinished=pending&&g.owner->finished()&&router.final_count()==0;
        const bool rejectsEarly=!router.wait_final_after(0,20);
        SetEvent(allowFinal);
        const bool progress=WaitForSingleObject(progressed,1000)==WAIT_OBJECT_0;
        const bool stillBlocked=sink.waiting.load()&&WaitForSingleObject(sink.release,0)==WAIT_TIMEOUT;
        // A blocked sink cannot be safely forced to finish. The bounded stop must
        // report failure, keep state alive, then permit a later successful join.
        const auto start=GetTickCount64();const bool stopped=router.shutdown(20);
        const auto elapsed=GetTickCount64()-start;
        SetEvent(sink.release);consumer.join();CloseHandle(progressed);CloseHandle(finalPending);CloseHandle(allowFinal);
        check(g.lines.size()+1<=fl::queue_lines,"blocked consumer run fits bounded router queue including prime");
        check(earlyFinished&&rejectsEarly,"owner finished flag alone does not admit the delayed final callback");
        check(router.final_count()==1,"final admission fence advances before disk drain");
        check(g.xr->waits>=399&&g.has_line("event=summary")&&g.has_line("xr-stereo;result="),"consumer executed frames, summaries and final callback");
        check(progress&&stillBlocked&&success,"real consumer frames, summaries and final line progress with sink blocked");
        check(!stopped&&elapsed<500&&router.incomplete(),"shutdown deadline exposes incomplete evidence without freeing live owner");
    } else {
        check(g.run().success,"consumer succeeds despite failing/partial sink");
        check(router.wait_idle(),"consumer batch completed");
        if(mode=="failing") {
            check(router.incomplete()&&fl::stats(router.dedicated_handle()).write_errors>0,"consumer failure observable");
            sink.fail=false;router.emit(true,"xr-stereo;event=recovery");
        }
    }
    check(router.wait_final_after(0),"consumer final callback admission observed");
    check(router.shutdown(2000),"consumer final queue drained and thread joined");
    const auto text=sink.text();
    if(mode!="failing") {
        check(!g.lines.empty()&&text.find(g.lines.back()+"\n")!=std::string::npos,"final consumer line reached sink");
        size_t cursor=0;for(const auto& line:g.lines){const auto at=text.find(line+"\n",cursor);
            check(at!=std::string::npos,"every consumer payload intact and ordered");cursor=at+line.size()+1;}
    }
    if(mode=="partial")check(fl::stats(router.dedicated_handle()).short_writes>0,"consumer partial-write retry exercised");
    if(mode!="slow")check(router.incomplete()&&text.find("event=incomplete-evidence")!=std::string::npos,"consumer incomplete-evidence marker");
    // The slow case already stopped its owner to test bounded shutdown; the
    // failing case may have lost its marker before recovery. Use a fresh owner
    // on the same recovered sink to test reporting over live idle drain periods
    // in ALL three cases, without changing their blocked/failing/partial checks.
    {std::lock_guard<std::mutex> lock(sink.mutex);sink.content.clear();}
    xr_stereo::evidence::Router reporting(nullptr,+[]() noexcept{return true;});
    reporting.mark_incomplete();
    reporting.emit(true,"xr-stereo;event=reporting-probe");
    check(reporting.wait_idle(),"router reporting probe drained");
    const std::string marker="feature-log;event=incomplete-evidence;source=consumer-router";
    const auto marker_count=[&] {
        const auto content=sink.text();size_t count=0,at=0;
        while((at=content.find(marker,at))!=std::string::npos){++count;at+=marker.size();}
        return count;
    };
    Sleep(350); // several idle 100 ms drain periods must not repeat the marker
    check(marker_count()==1,"router incomplete marker appears exactly once across idle drains");
    reporting.mark_incomplete(); // a repeated fault/recovery does not clear the latch
    reporting.emit(true,"xr-stereo;event=reporting-recovery");
    check(reporting.wait_idle(),"router recovery probe drained");
    Sleep(350);
    check(reporting.incomplete()&&marker_count()==1,"recovery and repeated fault do not re-arm router marker");
    check(reporting.shutdown(2000),"reporting probe owner joined");
    check(marker_count()==1,"shutdown does not repeat router marker");
    fl::set_sink(nullptr);
}
} // namespace


// J-167: synthetic diagnostics never establish native GPU/XR parity.
void throughput_diagnostics() {
    using namespace xr_stereo;
    auto t = timestamp_timing(1200,1250,1000,5000,5050,1000,1000);
    check(t.available && std::fabs(t.start_ms-150)<1e-9 && std::fabs(t.blit_ms-50)<1e-9, "GPU calibration relates Execute QPC and blit timestamps");
    t = timestamp_timing(1200,1250,1300,5300,5050,1000,1000);
    check(t.available && std::fabs(t.start_ms-150)<1e-9, "calibration supports GPU start before calibration");
    check(!timestamp_timing(1,2,0,0,0,0,1).available && !timestamp_timing(1,2,0,0,0,1,0).available &&
        !timestamp_timing(2,1,0,0,0,1,1).available && !timestamp_timing(1,2,10,0,100,1,1).available,
        "missing clocks, reversed timestamps and invalid calibration unavailable");
    for (bool present : {false,true}) {
        Rig g; g.stop_after=6; g.settings.summary_ms=20; g.blit->gpu_timing={present,150,2.5};
        g.blit->complete_after=6; g.src.reported_ahead=present ? 3 : 0; g.xr->late_available=present; g.xr->late_ms=7.25;
        g.build(); g.xr->hook["end_frame_projection"]=[&]{g.clock->t+=3;};
        const auto r=g.run();
        check(r.success && !g.out->failed() && !g.src.bad_releases && !g.xr->violations, "timing availability does not change ownership/completion");
        check(g.has_line(present ? "gpu-start-ms=150.000000;gpu-blit-ms=2.500000" : "gpu-start-ms=unavailable;gpu-blit-ms=unavailable"), "GPU timing present or unavailable");
        check(g.has_line(present ? "end-late-ms-max=7.250000" : "end-late-ms-max=unavailable"), "end lateness present or unavailable");
        check(g.has_line("xr-period-ms-mean=11.111111;xr-period-ms-max=11.111111") && g.has_line("frame-cpu-ms-max=8;blocked-ms-max=5"), "period, begin-to-end CPU and completion block");
        check(g.has_line(present ? "select-lag=3;select-age-ms=" : "select-lag=0;select-age-ms=") && g.has_line("new-per-s="), "selection and throughput summary fields");
        check(g.has_line("event=session-state;state=") && g.has_line(";xr-time-ns=0;owner-t-ms="), "fake session transitions timestamped");
        unsigned transitions=0;for(const auto& line:g.lines) transitions += line.find("event=session-state;")!=std::string::npos;
        check(transitions==r.events,"every session-state event logged");
        for(const auto& line:g.lines) check(line.size()<feature_log::line_cap,"diagnostic lines fit router");
    }
    Rig e; e.build(); Event event=state_event(SessionState::focused); event.event_time=123456789;
    struct EventGate final : Gate {
        int64_t now_ms() override{return 42;} void sleep_ms(int64_t) override{}
        bool lost() const override{return false;} bool stopping() const override{return false;}
        Rc invoke(Op,Rc(*fn)(void*),void* p) override{return fn(p);}
    } gate;
    e.out->session_event(gate,event);
    check(e.has_line(";xr-time-ns=123456789;owner-t-ms=42"),"session preserves runtime/receipt times");
}
void source_sizing_throughput() {
    using namespace xr_stereo;
    check(Settings{}.cap_width==2400 && configured_cap(nullptr)==2400 && configured_cap("")==2400,"default cap");
    check(configured_cap("1920")==1920 && configured_cap("3000")==3000 && configured_cap("0")==2400 &&
        configured_cap("-1")==2400 && configured_cap("8193")==2400 && configured_cap("2400junk")==2400,"configurable cap and invalid fallback");
    for(const auto source:{Extent{1280,720},Extent{1920,1080},Extent{2560,1440},Extent{3840,2160}}) {
        Rig g; g.src.width=source.w;g.src.height=source.h;g.src.lens_valid=true;g.src.with_depth=g.src.with_world=true;
        check(g.src.publish(),"source ready before launch"); const auto actual=published_source_extent(&g.src);
        check(actual.w==source.w && actual.h==source.h && g.src.releases==1 && !g.blit->executing,"metadata sample released without GPU use");
        g.settings.assumed_src_w=actual.w;g.settings.assumed_src_h=actual.h;g.settings.cap_width=Settings{}.cap_width;
        const auto extent=swapchain_extent(actual.w,actual.h,4096,4096,g.settings.cap_width);
        g.xr->expect_w=extent.w;g.xr->expect_h=extent.h;
        check(extent.w==std::min(source.w,2400u) && uint64_t(extent.w)*source.h==uint64_t(extent.h)*source.w,"all source sizes capped with aspect preserved");
        g.stop_after=4;g.build();const auto r=g.run();
        check(r.success && !g.xr->violations && !g.src.bad_releases,"source-sized swapchains retire safely");
        check(g.out->extent(0).w==extent.w && g.out->extent(2).h==extent.h,"eyes and world sized from source");
        const auto m=g.blit->maps.front()[0];
        check(!needs_clear(m,extent.w,extent.h) && std::fabs(m.scale-double(extent.w)/source.w)<1e-12,"full rect clear omission and scale");
        double u=0,v=0;
        check(source_uv(m,extent.w-1,extent.h-1,u,v) && std::fabs(u-(extent.w-.5)/extent.w)<1e-12 &&
            std::fabs(v-(extent.h-.5)/extent.h)<1e-12,"full rect UV covers full lens");
        const auto expected=fov_from_lens(g.src.lens);
        check(!g.xr->layers.empty() && std::fabs(g.xr->layers.front().views[0].fov.right-expected.right)<1e-6 &&
            g.xr->layers.front().quad,"source sizing preserves lens FOV/world quad");
        check(g.has_line(";cap=2400;runtime-limit=4096x4096,4096x4096;source="),"swapchain cap/runtime limits logged");
        check(g.has_line(";draws=3;clears=0;pose="),"full rect fake recording omits redundant clears");
    }
    {
        Rig capped; capped.settings.cap_width=configured_cap("1920");capped.xr->expect_w=1920;capped.xr->expect_h=1080;
        capped.stop_after=4;capped.build();const auto r=capped.run();
        check(r.success && !capped.xr->violations && capped.out->extent(0).w==1920 &&
            std::fabs(capped.blit->maps.front()[0].scale-.75)<1e-12,"configured cap reaches swapchains and scale");
    }
    check(!published_source_extent(nullptr).w,"missing source unavailable");
    Rig empty;check(!published_source_extent(&empty.src).w && !empty.src.releases,"empty source does not fabricate size");
    check(needs_clear(letterbox(1920,1080,1000,1000),1000,1000) && needs_clear(BlitMapping{},1000,1000),"partial/invalid mapping retains clear");
    auto offset=full_rect(1280,720,1280,720);offset.dst.x=1;
    check(needs_clear(offset,1280,720),"offset mapping retains clear");
}

// ------------------------------------------------- J-hud-consumer (HUD quad) ---
bool same_pose(const xr_owner::Pose& a, const xr_owner::Pose& b) {
    return std::memcmp(a.position, b.position, sizeof a.position) == 0 && std::memcmp(a.orientation, b.orientation, sizeof a.orientation) == 0;
}
bool same_layer(const ProjectionLayer& a, const ProjectionLayer& b) {
    for (unsigned e = 0; e < 2; ++e) {
        if (!same_pose(a.views[e].pose, b.views[e].pose)) return false;
        if (std::memcmp(&a.views[e].fov, &b.views[e].fov, sizeof a.views[e].fov) != 0) return false;
    }
    return a.source_alpha == b.source_alpha && a.quad == b.quad && same_pose(a.quad_pose, b.quad_pose) &&
        a.quad_width == b.quad_width && a.quad_height == b.quad_height && a.hud == b.hud && a.hud_view_space == b.hud_view_space &&
        same_pose(a.hud_pose, b.hud_pose) && a.hud_width == b.hud_width && a.hud_height == b.hud_height &&
        a.markers == b.markers && same_pose(a.marker_pose, b.marker_pose) && a.marker_width == b.marker_width && a.marker_height == b.marker_height;
}
std::vector<std::string> per_frame_calls(Rig& g, unsigned frames) {
    std::vector<std::string> out; const auto all = g.j.calls();
    for (unsigned f = 0; f < frames; ++f) { auto s = frame_slice(all, f); out.insert(out.end(), s.begin(), s.end()); out.push_back("|"); }
    return out;
}
void hud_full_rig(Rig& g) { g.src.lens_valid = true; g.src.with_depth = true; g.src.with_world = true; }

// HUD-LATE F6 (hud_late.h): the radio subtitles are post-tone-map display values; the late draw runs under the alpha ONE / ONE / REV_SUBTRACT clone, so a
// touched pixel ends with alpha = a_dst - A (< 0). The consumer must return the game's value under Reinhard + exposure 0.8 and change nothing by default.
void hud_late_display_marker() {
    using xr_stereo::Tonemap;
    namespace s5 = acz::gpu_copy::step5;
    // The game's blend of one late text texel (rgb SRC_ALPHA / INV_SRC_ALPHA) over the HUD texel `under`, with the clone's alpha (a_dst - A).
    auto late = [](const float under[4], const float t[3], float A, float out[4]) {
        const float u[4] = {under[0], under[1], under[2], under[3]};
        for (int i = 0; i < 3; ++i) out[i] = t[i] * A + u[i] * (1.0f - A);
        out[3] = u[3] - A;
    };
    const float empty[4] = {0, 0, 0, 0}, white[3] = {1, 1, 1};
    float px[4];
    late(empty, white, 1.0f, px);
    check(s5::hud_late_is_display(px[3]) && px[3] == -1.0f, "F6: a fully covered late texel is marked (alpha -1)");
    auto s = xr_stereo::shade_hud(px, Tonemap::reinhard, 0.8f);
    check(std::fabs(s.r - 1.0f) <= 1.0f / 255 && std::fabs(s.g - 1.0f) <= 1.0f / 255 && std::fabs(s.b - 1.0f) <= 1.0f / 255 && s.a == 1.0f,
          "F6: white subtitle text under Reinhard + exposure 0.8 composites to the game's white (within 1/255)");
    const float unmarked[4] = {1, 1, 1, 1};
    s = xr_stereo::shade_hud(unmarked, Tonemap::reinhard, 0.8f);
    check(std::fabs(s.r - 0.8f / 1.8f) < 1e-6f, "F6: an unmarked HUD texel is still tone-mapped (0.8/1.8)");
    late(empty, white, 0.5f, px);   // an anti-aliased edge: the game's value over an empty layer is 0.5, premultiplied
    s = xr_stereo::shade_hud(px, Tonemap::reinhard, 0.8f);
    check(std::fabs(s.r - 0.5f) <= 1.0f / 255 && std::fabs(s.a - 0.5f) <= 1.0f / 255, "F6: an edge texel keeps the game's premultiplied value and coverage");
    const float cyan[3] = {0.0f, 0.8f, 1.0f};
    late(empty, cyan, 1.0f, px);    // a coloured speaker name
    s = xr_stereo::shade_hud(px, Tonemap::reinhard, 0.5f);
    check(s.r == 0.0f && std::fabs(s.g - 0.8f) <= 1.0f / 255 && std::fabs(s.b - 1.0f) <= 1.0f / 255, "F6: coloured text keeps the game's channels");
    late(px, white, 1.0f, px);      // a second, overlapping late draw (outline then text): coverage sums, still marked
    check(s5::hud_late_is_display(px[3]) && s5::hud_late_coverage(px[3]) == 1.0f, "F6: overlapping late draws stay marked, coverage saturates");
    // Defaults (clamp, exposure 1): the marked texel shades to the same colour as before, and an unmarked texel is bit for bit the old formula.
    late(empty, white, 1.0f, px);
    s = xr_stereo::shade_hud(px, Tonemap::clamp, 1.0f);
    check(s.r == 1.0f && s.g == 1.0f && s.b == 1.0f, "F6: defaults, marked texel colour == game value");
    const float dim[4] = {0.3f, 0.5f, 0.7f, 0.2f};
    s = xr_stereo::shade_hud(dim, Tonemap::clamp, 1.0f);
    check(s.r == 0.3f && s.g == 0.5f && s.b == 0.7f && s.a == 0.7f, "F6: defaults, unmarked texel unchanged");
    check(!s5::hud_late_is_display(std::nanf("")) && !s5::hud_late_is_display(0.0f) && !s5::hud_late_is_display(0.7f), "F6: NaN, zero and positive alpha are never marked");
}
void hud_shading_math() {
    using xr_stereo::Tonemap;
    const float green[4] = {0.1f, 0.6f, 0.2f, 0.0f}; // alpha channel 0: not a coverage mask
    auto s = xr_stereo::shade_hud(green, Tonemap::clamp, 1.0f);
    check(s.r == 0.1f && s.g == 0.6f && s.b == 0.2f && s.a == 0.6f, "HUD: alpha = max(rgb) when the game alpha is 0; colour kept (premultiplied)");
    const float clear[4] = {0, 0, 0, 0};
    s = xr_stereo::shade_hud(clear, Tonemap::clamp, 1.0f);
    check(s.r == 0 && s.g == 0 && s.b == 0 && s.a == 0, "HUD: untouched texel transparent");
    const float dim[4] = {0.1f, 0.1f, 0.1f, 0.8f};
    s = xr_stereo::shade_hud(dim, Tonemap::clamp, 1.0f);
    check(s.a == 0.8f && s.r == 0.1f, "HUD: game alpha kept when larger");
    const float over[4] = {3.0f, 0.0f, 0.0f, 4.0f};
    s = xr_stereo::shade_hud(over, Tonemap::clamp, 1.0f);
    check(s.r == 1.0f && s.a == 1.0f, "HUD: overbright clamped, alpha saturated");
    const float nan_a[4] = {0.3f, 0.2f, 0.1f, std::nanf("")};
    s = xr_stereo::shade_hud(nan_a, Tonemap::clamp, 1.0f);
    check(std::fabs(s.a - 0.3f) < 1e-7f, "HUD: NaN alpha -> 0 then max(rgb)");
    const float neg[4] = {-1.0f, 0.5f, 0.0f, 0.0f}; // (a negative alpha is the HUD-LATE display marker since F6: see hud_late_display_marker)
    s = xr_stereo::shade_hud(neg, Tonemap::reinhard, 2.0f);
    check(s.r == 0.0f && std::fabs(s.g - 0.5f) < 1e-6f && std::fabs(s.a - 0.5f) < 1e-6f, "HUD: same exposure/Reinhard as the other layers");
    hud_late_display_marker();
    const float rgb[3] = {0.7f, 0.25f, 1.5f};
    const float rgba[4] = {0.7f, 0.25f, 1.5f, 0.0f};
    const auto w = xr_stereo::shade(rgb, false, 0.0f, Tonemap::clamp, 1.3f);
    s = xr_stereo::shade_hud(rgba, Tonemap::clamp, 1.3f);
    check(s.r == w.r && s.g == w.g && s.b == w.b, "HUD colour == world/cockpit tone mapping of the same texel");
    unsigned seed = 7;
    for (unsigned i = 0; i < 2000; ++i) {
        float v[4];
        for (float& x : v) { seed = seed * 1103515245u + 12345u; x = float(seed >> 8) / float(1u << 24) * 4.0f - 1.0f; }
        for (auto tm : {Tonemap::clamp, Tonemap::reinhard}) {
            s = xr_stereo::shade_hud(v, tm, 1.0f);
            check(s.a >= 0 && s.a <= 1 && s.r <= s.a && s.g <= s.a && s.b <= s.a && s.r >= 0, "HUD output is valid premultiplied colour (c <= a, a in [0,1])");
        }
    }
}

// J-hud-colour: the producer's png_once conversion (gpu_copy::png::shade_rgb/shade_hud) is the
// consumer's own shading, bit for bit (clamp and Reinhard, several exposures, NaN/negative/overbright).
void png_shade_mirror() {
    using xr_stereo::Tonemap;
    unsigned seed = 11, same = 0, n = 0;
    for (unsigned i = 0; i < 4000; ++i) {
        float v[4];
        for (float& x : v) { seed = seed * 1103515245u + 12345u; x = float(seed >> 8) / float(1u << 24) * 6.0f - 2.0f; }
        if (i % 97 == 0) v[i % 4] = std::nanf("");
        for (unsigned tm = 0; tm < 2; ++tm) for (float e : {1.0f, 0.5f, 1.7f}) {
            const auto c = xr_stereo::shade_hud(v, tm ? Tonemap::reinhard : Tonemap::clamp, e);
            float p[4]; acz::gpu_copy::png::shade_hud(v, tm, e, p);
            const auto w = xr_stereo::shade(v, false, 0.0f, tm ? Tonemap::reinhard : Tonemap::clamp, e);
            float q[3]; acz::gpu_copy::png::shade_rgb(v, tm, e, q);
            ++n; same += std::memcmp(&c.r, &p[0], 4) == 0 && std::memcmp(&c.g, &p[1], 4) == 0 && std::memcmp(&c.b, &p[2], 4) == 0 &&
                std::memcmp(&c.a, &p[3], 4) == 0 && std::memcmp(&w.r, &q[0], 4) == 0 && std::memcmp(&w.g, &q[1], 4) == 0 && std::memcmp(&w.b, &q[2], 4) == 0;
        }
    }
    check(same == n && n == 24000, "png_once shading == consumer shade/shade_hud bit for bit");
}

void hud_compose_layers() {
    using namespace xr_stereo;
    ProjectionLayer p; // key-off shapes
    auto l = compose_layers(p);
    check(l.count == 1 && l.e[0].kind == LayerKind::projection && !l.e[0].source_alpha, "projection only");
    p.source_alpha = true; p.quad = true;
    l = compose_layers(p);
    check(l.count == 2 && l.e[0].kind == LayerKind::world_quad && l.e[0].target == kQuad && !l.e[0].source_alpha &&
        l.e[1].kind == LayerKind::projection && l.e[1].source_alpha && !l.e[0].unpremultiplied && !l.e[1].unpremultiplied,
        "existing 2-layer list: opaque world quad then premultiplied projection");
    p.hud = true;
    l = compose_layers(p);
    check(l.count == 3 && l.e[0].kind == LayerKind::world_quad && l.e[1].kind == LayerKind::projection && l.e[2].kind == LayerKind::hud_quad,
        "3 layers: world quad -> cockpit projection -> HUD quad");
    check(l.e[2].target == kHud && l.e[2].source_alpha && !l.e[2].unpremultiplied && l.e[2].space == LayerSpace::local,
        "HUD quad: SOURCE_ALPHA, premultiplied (no UNPREMULTIPLIED bit), LOCAL");
    p.quad = false; p.hud_view_space = true;
    l = compose_layers(p);
    check(l.count == 2 && l.e[0].kind == LayerKind::projection && l.e[1].kind == LayerKind::hud_quad && l.e[1].space == LayerSpace::view,
        "no world quad: projection then head-locked HUD in VIEW");
}

void hud_config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("log_enabled=true\n");
    check(d.issue == ConfigIssue::none && !d.config.xr_hud_layer() && d.config.hud_mode() == 0, "xr_hud_layer default OFF, hud_mode world-quad");
    auto on = parse_config("xr_stereo_output=true\nxr_hud_layer=true\nhud_mode=head-locked\n");
    check(on.issue == ConfigIssue::none && on.config.xr_hud_layer() && on.config.hud_mode() == 1, "xr_hud_layer ON, hud_mode head-locked");
    auto wq = parse_config("xr_hud_layer=1\nhud_mode=world-quad\n");
    check(wq.issue == ConfigIssue::none && wq.config.xr_hud_layer() && wq.config.hud_mode() == 0, "hud_mode world-quad");
    check(!parse_config("xr_hud_layer=0\n").config.xr_hud_layer(), "xr_hud_layer=0 OFF");
    check(parse_config("xr_hud_layer=yes\n").issue == ConfigIssue::out_of_range, "xr_hud_layer malformed rejected");
    check(parse_config("hud_mode=headlocked\n").issue == ConfigIssue::out_of_range, "hud_mode unknown value rejected");
    check(!parse_config("xr_hud_layer=true\nxr_hud_layer=true\n").config.xr_hud_layer(), "duplicate refuses");
    check(std::string(xr_stereo::hud_mode_name(xr_stereo::HudMode::world_quad)) == "world-quad" &&
        std::string(xr_stereo::hud_mode_name(xr_stereo::HudMode::head_locked)) == "head-locked", "mode names = config values");
    check(!xr_stereo::Settings{}.hud_layer && xr_stereo::Settings{}.hud_mode == xr_stereo::HudMode::world_quad, "Settings default OFF");
}

void hud_three_layers_world_quad() {
    Rig g; g.stop_after = 8; g.settings.hud_layer = true; hud_full_rig(g); g.src.with_hud = true; g.build();
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && g.src.bad_releases == 0 && g.xr->violations == 0 && !g.out->failed(), "HUD frames succeed, every source restored");
    check(g.j.count("xr.create 3") == 1 && g.j.count("d3d.bind 3") == 1 && g.j.count("xr.destroy 3") == 1, "HUD swapchain created, bound, retired once");
    const long b2 = g.j.index("d3d.bind 2"), c3 = g.j.index("xr.create 3"), b3 = g.j.index("d3d.bind 3"), begin = g.j.index("begin_session");
    check(b2 >= 0 && b2 < c3 && c3 < b3 && b3 < begin, "HUD swapchain after the world quad, before BeginSession");
    check(g.out->extent(3).w == g.out->extent(2).w && g.out->extent(3).h == g.out->extent(2).h && g.blit->bound_w[3] == 2560 &&
        g.blit->bound_h[3] == 1440, "HUD swapchain extent = world quad extent policy");
    const long dd = g.j.index("d3d.destroy"), d3 = g.j.index("xr.destroy 3");
    check(d3 > 0 && d3 < dd && dd < g.j.index("request_exit"), "HUD swapchain retired before private objects and the exit handshake");
    const auto s = frame_slice(g.j.calls(), 0);
    const std::vector<std::string> expected{"wait_frame", "begin_frame", "locate_views", "locate_head", "source.acquire 1",
        "d3d.presync pending", "xr.acquire 0", "xr.wait 0 0", "xr.acquire 1", "xr.wait 1 0", "xr.acquire 2", "xr.wait 2 0",
        "xr.acquire 3", "xr.wait 3 0", "d3d.record 0,0,q0,h0", "d3d.close", "d3d.gpu_wait", "d3d.execute", "d3d.signal 1", "d3d.sample 1",
        "xr.release 0", "xr.release 1", "xr.release 2", "xr.release 3", "source.release 1", "end_frame_projection"};
    check(s == expected, "exact order: HUD acquired/waited, ONE recording/Close/Execute/Signal, released after observed completion");
    check(g.j.prefix("d3d.execute") == r.begun && g.j.prefix("d3d.signal") == r.begun && g.j.prefix("d3d.record") == r.begun,
        "the HUD blit joins the frame's one list and one fence");
    const auto& job = g.blit->jobs[0];
    check(job.use_hud && job.hud.resource == FakeSource::res(0, 5) && job.hud.format == 10 && job.hud.width == 2560 &&
        job.hud.height == 1440 && job.map[3].ok && job.map[3].dst.w == 2560 && job.tonemap == xr_stereo::Tonemap::clamp &&
        job.exposure == 1.0f, "HUD job: source-sized HUD, full-rect map, same tonemap/exposure as the world blit");
    check(g.xr->layers.size() == r.begun, "one layer set per frame");
    for (auto& l : g.xr->layers) {
        check(l.hud && l.quad && !l.hud_view_space, "world quad + HUD on every layer");
        check(same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width && l.hud_height == l.quad_height,
            "HUD pose and size EXACTLY equal the world quad's");
        const auto list = xr_stereo::compose_layers(l);
        check(list.count == 3 && list.e[2].kind == xr_stereo::LayerKind::hud_quad && list.e[2].source_alpha && !list.e[2].unpremultiplied,
            "submitted list: world quad -> projection -> premultiplied HUD quad");
    }
    const auto& l0 = g.xr->layers[0];
    check(std::fabs(l0.hud_width - 49.216f) < 1e-3f && std::fabs(l0.hud_height - 27.68f) < 1e-3f &&
        std::fabs(l0.hud_pose.position[2] - (0.1f - 40.0f)) < 1e-4f && l0.hud_pose.orientation[3] == 1.0f,
        "HUD at the world quad's LOCAL placement: 40 m, 49.2 x 27.7 m");
    check(c.hud_frames == c.new_frames && c.hud_frames == r.begun && c.hud_layers == c.submitted && c.hud_skipped == 0 &&
        c.hud_invalid == 0 && c.hud_lens_invalid == 0, "HUD counters");
    check(g.has_line("xr-stereo;event=hud-swapchain;key=xr_hud_layer;hud=2560x1440,images=3;format=29;policy=same-as-world-quad;hud-mode=world-quad"),
        "hud-swapchain line");
    check(g.has_line("xr-stereo;event=first-hud-frame;frame=1;frame_id=1;slot=0;hud-fmt=10;src=2560x1440;hud-image=0;producer-hud-draws=58;producer-hud-refused=1;hud-mode=world-quad;world=quad;draws=4"),
        "first-hud-frame line");
    check(g.has_line("xr-stereo;event=first-hud-layer;") && g.has_line("layers=3;order=quad-then-projection-then-hud;hud-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);hud-mode=world-quad;hud-space=LOCAL;") &&
        g.has_line("hud-size=49.216x27.680;hud-pose-equals-world-quad=1"), "first-hud-layer line");
    const std::string n = std::to_string(r.begun);
    check(g.has_line("xr-stereo;event=summary-hud;") && g.has_line(";hud-frames=" + n + ";hud-layers=" + n + ";hud-skipped=0;hud-invalid=0;hud-lens-invalid=0;hud-mode=world-quad;hud-swapchain=2560x1440;producer-hud-draws=58;producer-hud-refused=1;no-headset-claim"),
        "summary-hud line");
    check(g.has_line("xr-stereo;event=hud-result;key=xr_hud_layer;hud-mode=world-quad;hud-frames=" + n + ";hud-layers=" + n + ";hud-skipped=0"), "hud-result line");
    check(g.has_line("hud=layer-world-quad") && g.has_line("reuse-max-age-ms=500;hud=layer") && g.has_line("result=PASS;") && g.has_line(";hud=layer;no-headset-claim"),
        "existing lines name the HUD layer when the key is on");
    for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "HUD lines fit the router: " + line);
}

void hud_absent_keeps_existing_path() {
    // A: key OFF (the existing 2-layer path). B: key ON, frames without hud_valid.
    // C: key ON, hud_valid but a refused HUD (null resource). B and C must submit the
    // byte-identical layer structs and the identical per-frame call sequence as A.
    Rig a; a.stop_after = 10; hud_full_rig(a); a.build(); auto ra = a.run();
    Rig b; b.stop_after = 10; hud_full_rig(b); b.settings.hud_layer = true; b.build(); auto rb = b.run();
    Rig cr; cr.stop_after = 10; hud_full_rig(cr); cr.settings.hud_layer = true; cr.src.with_hud = true;
    cr.src.hud_bad = FakeSource::HudBad::null_resource; cr.build(); auto rc = cr.run();
    check(ra.success && rb.success && rc.success && ra.begun == rb.begun && ra.begun == rc.begun, "absent/invalid HUD runs succeed");
    check(per_frame_calls(a, ra.begun) == per_frame_calls(b, rb.begun) && per_frame_calls(a, ra.begun) == per_frame_calls(cr, rc.begun),
        "absent/invalid HUD: per-frame call sequence identical to the key-off path (no HUD acquire/blit)");
    check(a.xr->layers.size() == b.xr->layers.size() && a.xr->layers.size() == cr.xr->layers.size(), "same number of layers");
    for (size_t i = 0; i < a.xr->layers.size(); ++i) {
        check(same_layer(a.xr->layers[i], b.xr->layers[i]) && same_layer(a.xr->layers[i], cr.xr->layers[i]) && !b.xr->layers[i].hud,
            "absent/invalid HUD: submitted layer byte-identical to the 2-layer path");
        check(xr_stereo::compose_layers(b.xr->layers[i]).count == 2, "absent HUD: 2 layers");
    }
    check(b.j.prefix("xr.acquire 3") == 0 && cr.j.prefix("xr.acquire 3") == 0, "no HUD image acquired");
    check(b.out->counters().hud_skipped == b.out->counters().acquired_frames && b.out->counters().hud_invalid == 0 &&
        b.out->counters().hud_frames == 0 && b.out->counters().hud_layers == 0, "absent HUD counted hud-skipped");
    check(cr.out->counters().hud_invalid == rc.begun && cr.out->counters().hud_skipped == rc.begun && cr.out->counters().hud_layers == 0,
        "invalid HUD counted hud-invalid");
    // Lines: once the HUD-only lines are removed and the hud token mapped back, B logs exactly A's lines.
    std::vector<std::string> mapped;
    for (std::string l : b.lines) {
        if (l.find("event=hud-") != std::string::npos || l.find("event=summary-hud") != std::string::npos) continue;
        for (const char* from : {"hud=layer-world-quad", "hud=layer"}) {
            const size_t p = l.find(from);
            if (p != std::string::npos) l.replace(p, std::strlen(from), "hud=not-included");
        }
        mapped.push_back(l);
    }
    check(mapped == a.lines, "key ON without HUD: every existing line unchanged apart from the hud token");
}

void hud_key_off_unchanged() {
    // The key OFF ignores producer HUD fields completely: journal, lines and layers are
    // identical to a run whose frames carry no HUD; no HUD swapchain exists.
    Rig a; a.stop_after = 10; hud_full_rig(a); a.build(); auto ra = a.run();
    Rig d; d.stop_after = 10; hud_full_rig(d); d.src.with_hud = true; d.build(); auto rd = d.run();
    check(ra.success && rd.success, "key-off runs succeed");
    check(a.j.calls() == d.j.calls(), "key OFF: the complete call journal is identical with or without producer HUD fields");
    check(a.lines == d.lines, "key OFF: every evidence line identical");
    check(a.xr->layers.size() == d.xr->layers.size(), "same layer count");
    for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], d.xr->layers[i]) && !d.xr->layers[i].hud, "key OFF: no HUD layer");
    check(d.j.count("xr.create 3") == 0 && d.j.prefix("xr.acquire 3") == 0 && d.j.count("d3d.bind 3") == 0, "key OFF: no HUD swapchain at all");
    check(!d.blit->jobs.empty() && !d.blit->jobs[0].use_hud, "key OFF: no HUD in the recording");
    check(d.has_line("hud=not-included") && !d.has_line("event=hud-") && !d.has_line("summary-hud"), "key OFF: lines say not-included");
    const auto& c = d.out->counters();
    check(c.hud_frames == 0 && c.hud_layers == 0 && c.hud_skipped == 0 && c.hud_invalid == 0, "key OFF: HUD counters never move");
    // Swapchain creation only when ON (world quad disabled too: eyes only).
    Rig e; e.stop_after = 3; e.settings.world_quad = false; e.settings.hud_layer = true; e.build(); auto re = e.run();
    check(re.success && e.j.count("xr.create 2") == 0 && e.j.count("xr.create 3") == 1 && e.j.count("xr.destroy 3") == 1,
        "key ON: HUD swapchain created even with the world quad disabled");
}

void hud_refusals() {
    struct Case { const char* what; std::function<void(Rig&)> set; bool acquires; };
    const std::vector<Case> cases{
        {"float HUD format other than FP16/FP32 refused", [](Rig& g) { g.src.hud_format = 28; }, false},
        {"HUD aliasing an eye refused", [](Rig& g) { g.src.hud_bad = FakeSource::HudBad::alias_eye; }, false},
        {"HUD aliasing the world snapshot refused", [](Rig& g) { g.src.hud_bad = FakeSource::HudBad::alias_world; }, false},
        {"backend cannot read the HUD: cleared, not drawn", [](Rig& g) { g.blit->hud_unreadable = true; }, true},
    };
    for (const auto& k : cases) {
        Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; k.set(g); g.build();
        auto r = g.run();
        const auto& c = g.out->counters();
        check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, std::string(k.what) + ": frame proceeds");
        for (auto& l : g.xr->layers) check(!l.hud && l.quad, std::string(k.what) + ": no HUD layer, world quad kept");
        check(c.hud_invalid == r.begun && c.hud_skipped == r.begun && c.hud_frames == 0 && c.hud_layers == 0, std::string(k.what) + ": counted hud-invalid");
        check(g.j.prefix("xr.acquire 3") == (k.acquires ? r.begun : 0u) && g.j.prefix("xr.release 3") == (k.acquires ? r.begun : 0u),
            std::string(k.what) + ": HUD image acquired only when the backend decides, always released after completion");
    }
    { Rig g; g.stop_after = 6; g.settings.hud_layer = true; g.src.with_hud = true; // world-quad mode needs a valid lens
      g.src.lens[0] = std::nanf(""); g.build();
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.hud_lens_invalid == r.begun && c.hud_skipped == r.begun && c.hud_layers == 0 && g.j.prefix("xr.acquire 3") == 0,
          "world-quad HUD without a valid lens: refused, counted hud-lens-invalid");
      check(g.has_line("hud-lens-invalid=" + std::to_string(r.begun)), "hud-lens-invalid logged"); }
}

void hud_head_locked() {
    Rig g; g.stop_after = 6; g.settings.hud_layer = true; g.settings.hud_mode = xr_stereo::HudMode::head_locked;
    g.src.with_hud = true; g.src.lens[0] = std::nanf(""); // no lens, no world: the fallback still places the HUD
    g.build();
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && c.hud_frames == r.begun && c.hud_layers == r.begun && c.hud_lens_invalid == 0, "head-locked HUD needs no lens");
    for (auto& l : g.xr->layers) {
        check(l.hud && l.hud_view_space && !l.quad, "head-locked HUD in VIEW space");
        check(l.hud_pose.position[0] == 0.0f && l.hud_pose.position[1] == 0.0f && l.hud_pose.position[2] == -2.5f &&
            l.hud_pose.orientation[0] == 0.0f && l.hud_pose.orientation[3] == 1.0f, "2.5 m straight ahead, identity");
        check(l.hud_width == 1.6f && std::fabs(l.hud_height - 0.9f) < 1e-6f, "1.6 m wide, source aspect (16:9)");
        const auto list = xr_stereo::compose_layers(l);
        check(list.count == 2 && list.e[1].kind == xr_stereo::LayerKind::hud_quad && list.e[1].space == xr_stereo::LayerSpace::view &&
            list.e[1].source_alpha && !list.e[1].unpremultiplied, "projection then premultiplied VIEW-space HUD");
    }
    check(g.has_line("hud-mode=head-locked;hud-space=VIEW;hud-pos=0.000,0.000,-2.500;hud-size=1.600x0.900") && g.has_line("hud=layer-head-locked"),
        "head-locked lines");
    // With a world quad present, head-locked does not use the world pose.
    Rig w; w.stop_after = 4; hud_full_rig(w); w.settings.hud_layer = true; w.settings.hud_mode = xr_stereo::HudMode::head_locked;
    w.src.with_hud = true; w.build(); auto rw = w.run();
    check(rw.success && w.xr->layers[0].quad && w.xr->layers[0].hud_view_space && !same_pose(w.xr->layers[0].hud_pose, w.xr->layers[0].quad_pose) &&
        w.has_line("hud-pose-equals-world-quad=0"), "head-locked is a fallback: not aligned with the world quad");
}

void hud_never_stale() {
    // HUD on every other produced frame: a frame without hud_valid never shows an older HUD.
    { Rig g; g.stop_after = 10; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.src.hud_every = 2; g.build();
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && g.xr->layers.size() == r.begun, "alternating HUD frames succeed");
      for (size_t i = 0; i < g.xr->layers.size(); ++i) check(g.xr->layers[i].hud == (i % 2 == 0), "layer carries the HUD of ITS frame only");
      check(g.j.prefix("xr.acquire 3") == (r.begun + 1) / 2 && c.hud_frames == (r.begun + 1) / 2 && c.hud_skipped == r.begun / 2 &&
          c.hud_layers == c.hud_frames, "HUD acquired/blitted/counted only for HUD frames"); }
    // Re-submission repeats the WHOLE last layer set (cockpit, world and HUD of one frame).
    { Rig g; g.stop_after = 20; g.publish_until = 2; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.build();
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.new_frames == 2 && c.reused == r.begun - 2 && c.hud_layers == c.submitted && c.hud_frames == 2,
          "re-submitted layers keep their own frame's HUD (no new acquire)"); }
    { Rig g; g.stop_after = 20; g.publish_until = 2; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.src.hud_every = 2; g.build();
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.new_frames == 2 && c.hud_frames == 1 && c.hud_layers == 1 && !g.xr->layers.back().hud,
          "the newest frame had no HUD: re-submissions carry none (never the previous HUD)"); }
}

void hud_failures_retain() {
    { // HUD image wait times out for good: acquisition outstanding -> retained, nothing destroyed.
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.build();
      g.xr->hook["xr.acquire 3"] = [&g] { if (g.xr->waits == 3) g.xr->wait_timeouts[3] = {true, true, true}; };
      auto r = g.run();
      check(g.out->failed() && g.out->failure().find("timeout; acquisition outstanding") != std::string::npos && !r.success, "HUD wait timeout disables");
      check(!r.clean && !r.consumer_retired && r.session_held && g.j.count("xr.destroy 3") == 0 && g.j.count("xr.destroy 0") == 0 &&
          g.j.count("destroy_session") == 0, "unwaited HUD acquisition: swapchains/session retained");
      check(g.j.count("xr.release 2") == 3 && g.j.count("xr.release 3") == 2 && g.src.releases == 3 && g.src.bad_releases == 0 &&
          g.j.prefix("d3d.execute") == 2, "waited images and the slot released (no GPU work); nothing submitted");
      check(g.has_line("xr-stereo;event=retire;outcome=retained;reason=unwaited acquisition outstanding"), "retained line"); }
    { // ... recovered by retire's bounded wait: clean.
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.build();
      g.xr->hook["xr.acquire 3"] = [&g] { if (g.xr->waits == 3) g.xr->wait_timeouts[3] = {true, true, false}; };
      auto r = g.run();
      check(g.out->failed() && r.clean && r.consumer_retired && g.j.count("xr.destroy 3") == 1 && g.xr->violations == 0,
          "late HUD wait at retire: released after the wait, clean"); }
    { // HUD acquire fails: everything waited is released, no submission.
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.build();
      g.xr->script["xr.acquire 3"] = {Rc::ok, Rc::ok, Rc::error};
      auto r = g.run();
      check(g.out->failed() && g.out->failure() == "xrAcquireSwapchainImage failed" && g.j.prefix("d3d.execute") == 2, "HUD acquire failure disables before submission");
      check(r.clean && r.consumer_retired && g.src.bad_releases == 0 && g.src.releases == 3 && g.xr->violations == 0, "HUD acquire failure: clean"); }
    for (bool late : {false, true}) { // GPU completion pending with the HUD in the recording
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.build();
      g.xr->hook["xr.wait 3 0"] = [&g] { if (g.xr->waits == 2) g.blit->never_complete = true; };
      if (late) g.xr->hook["end_frame"] = [&g] { g.blit->never_complete = false; };
      auto r = g.run();
      check(g.out->failed() && g.out->failure() == "GPU completion pending; quarantine", "HUD frame quarantined on completion expiry");
      check(g.has_line(";targets=4;owner-t-ms="), "completion-pending counts the HUD target");
      if (late) {
          check(r.clean && r.consumer_retired && g.src.bad_releases == 0 && g.src.releases == 2 && g.j.count("xr.release 3") == 2 &&
              g.j.count("xr.destroy 3") == 1, "late completion: HUD image released, then retired");
          const long done = g.j.index("d3d.sample 2"), rel = g.j.index("xr.release 3", size_t(g.j.index("d3d.signal 2")));
          check(done > 0 && rel > done, "HUD image released only after observed completion");
      } else {
          check(!r.clean && !r.consumer_retired && g.src.releases == 1 && g.j.count("xr.release 3") == 1 && g.j.count("xr.destroy 3") == 0 &&
              g.j.count("d3d.destroy") == 0 && g.j.count("destroy_session") == 0, "quarantine: HUD image, slot, swapchains, private objects retained");
      }
    }
}

// ------------------------------------- J-feather (edge feather + quad distance) ---
bool bits_equal(const xr_stereo::Shade& a, const xr_stereo::Shade& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void feather_ramp_math() {
    using namespace xr_stereo;
    check(clamp_feather(0.0f) == 0.0f && clamp_feather(-1.0f) == 0.0f && clamp_feather(std::nanf("")) == 0.0f &&
        clamp_feather(0.04f) == 0.04f && clamp_feather(0.25f) == 0.25f && clamp_feather(0.9f) == kMaxFeather, "feather clamp [0, 0.25], NaN OFF");
    for (float f : {0.0f, -0.5f, std::nanf("")})
        for (float u : {0.0f, 0.001f, 0.25f, 0.5f, 1.0f}) check(edge_ramp(u, u, f) == 1.0f && edge_ramp(u, 0.0f, f) == 1.0f, "feather OFF: ramp exactly 1 everywhere");
    for (float f : {0.01f, 0.04f, 0.1f, 0.25f}) {
        check(edge_ramp(0.5f, 0.5f, f) == 1.0f, "centre: ramp 1");
        check(edge_ramp(0.0f, 0.5f, f) == 0.0f && edge_ramp(0.5f, 1.0f, f) == 0.0f && edge_ramp(1.0f, 0.0f, f) == 0.0f, "border: ramp 0");
        // d = 2*min(u,1-u) = f/2 -> t = 1/2 -> smoothstep 0.5 (half of the feather band).
        check(std::fabs(edge_ramp(f / 4.0f, 0.5f, f) - 0.5f) < 1e-5f && std::fabs(edge_ramp(0.5f, 1.0f - f / 4.0f, f) - 0.5f) < 1e-4f,
            "half of the feather band: ramp 0.5");
        check(edge_ramp(f / 2.0f, 0.5f, f) == 1.0f && edge_ramp(0.3f, 0.7f, f) == 1.0f, "inside the band's inner edge: ramp exactly 1");
        check(std::fabs(edge_ramp(f / 4.0f, f / 4.0f, f) - 0.25f) < 1e-5f, "corner: product of both axes");
        float prev = -1.0f;
        for (unsigned i = 0; i <= 500; ++i) {
            const float u = float(i) / 1000.0f; // border -> centre
            const float r = edge_ramp(u, 0.5f, f);
            check(r >= prev && r >= 0.0f && r <= 1.0f, "ramp monotonic non-decreasing toward the centre, in [0,1]");
            check(std::fabs(r - edge_ramp(1.0f - u, 0.5f, f)) < 1e-3f, "ramp symmetric");
            check(std::fabs(r - edge_ramp(0.5f, u, f)) < 1e-7f, "same ramp on both axes");
            prev = r;
        }
    }
    // Premultiplied consistency: every path stays valid premultiplied colour, and the ramp
    // scales colour and alpha together (cockpit coverage, world opaque, HUD).
    unsigned seed = 11;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return float(seed >> 8) / float(1u << 24); };
    for (unsigned i = 0; i < 3000; ++i) {
        const float rgba[4] = {rnd() * 3 - 0.5f, rnd() * 3 - 0.5f, rnd() * 3 - 0.5f, rnd() * 1.5f - 0.2f};
        const float u = rnd(), v = rnd(), f = rnd() * 0.3f, cover = rnd() > 0.5f ? 1.0f : 0.0f;
        const float k = edge_ramp(u, v, f);
        for (auto tm : {Tonemap::clamp, Tonemap::reinhard}) {
            const Shade base[3] = {shade(rgba, true, cover, tm, 1.0f), shade(rgba, false, 0.0f, tm, 1.0f), shade_hud(rgba, tm, 1.0f)};
            for (const auto& b : base) {
                const Shade o = apply_feather(b, k);
                check(o.a >= 0 && o.a <= b.a && o.r <= o.a + 1e-7f && o.g <= o.a + 1e-7f && o.b <= o.a + 1e-7f && o.r >= 0,
                    "feathered output valid premultiplied (c <= a, a <= unfeathered a)");
                check(o.r == b.r * k && o.a == b.a * k, "ramp multiplies colour and alpha alike");
                check(bits_equal(apply_feather(b, edge_ramp(u, v, 0.0f)), b), "feather OFF: bit-identical shade");
            }
        }
    }
    // CPU image of one blit (source_uv per destination pixel): feather OFF = identical pixels.
    const auto m = full_rect(64, 36, 64, 36);
    unsigned differ = 0, faded = 0;
    for (uint32_t y = 0; y < 36; ++y) for (uint32_t x = 0; x < 64; ++x) {
        double u = 0, v = 0; check(source_uv(m, x, y, u, v), "uv");
        const float rgb[3] = {float(x) / 64.0f, float(y) / 36.0f, 0.5f};
        const Shade plain = shade(rgb, false, 0.0f, Tonemap::clamp, 1.0f);
        differ += !bits_equal(apply_feather(plain, edge_ramp(float(u), float(v), 0.0f)), plain);
        const Shade fe = apply_feather(plain, edge_ramp(float(u), float(v), 0.1f));
        faded += fe.a < 1.0f;
        if (x == 32 && y == 18) check(bits_equal(fe, plain), "image centre untouched with the feather on");
        if (x == 0 || y == 0) check(fe.a < 0.2f, "border pixels nearly transparent with the feather on");
    }
    check(differ == 0 && faded > 0 && faded < 64 * 36 / 2, "OFF: every pixel identical; ON: only a border band fades");
}

void feather_config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("log_enabled=true\n");
    check(d.issue == ConfigIssue::none && d.config.xr_edge_feather() == 0.0f && d.config.xr_world_quad_distance_m() == 40.0f &&
        !d.config.xr_edge_feather_set() && !d.config.xr_world_quad_distance_set(), "J-feather keys default OFF / 40 m / unset");
    auto on = parse_config("xr_stereo_output=true\nxr_edge_feather=0.04\nxr_world_quad_distance_m=100\n");
    check(on.issue == ConfigIssue::none && std::fabs(on.config.xr_edge_feather() - 0.04f) < 1e-7f && on.config.xr_world_quad_distance_m() == 100.0f &&
        on.config.xr_edge_feather_set() && on.config.xr_world_quad_distance_set(), "J-feather keys parsed");
    check(parse_config("xr_edge_feather=0.9\n").config.xr_edge_feather() == 0.25f, "feather clamped to 0.25");
    check(parse_config("xr_edge_feather=0\n").config.xr_edge_feather() == 0.0f && parse_config("xr_edge_feather=0\n").config.xr_edge_feather_set(), "feather 0 = OFF (configured)");
    check(parse_config("xr_world_quad_distance_m=1\n").config.xr_world_quad_distance_m() == 5.0f, "distance clamped to 5 m");
    check(parse_config("xr_world_quad_distance_m=99999\n").config.xr_world_quad_distance_m() == 2000.0f, "distance clamped to 2000 m");
    check(parse_config("xr_world_quad_distance_m=250.5\n").config.xr_world_quad_distance_m() == 250.5f, "fractional distance");
    for (const char* bad : {"xr_edge_feather=abc\n", "xr_edge_feather=-0.1\n", "xr_edge_feather=.5\n", "xr_edge_feather=1.\n", "xr_edge_feather=1e-2\n",
                            "xr_edge_feather=0.0.1\n", "xr_world_quad_distance_m=40m\n", "xr_world_quad_distance_m=1234567890123\n"})
        check(parse_config(bad).issue == ConfigIssue::out_of_range, std::string("non-decimal value rejects the config: ") + bad);
    check(parse_config("xr_edge_feather=0.04\nxr_edge_feather=0.04\n").issue == ConfigIssue::duplicate, "duplicate refuses");
    const xr_stereo::Settings s{};
    check(s.edge_feather == 0.0f && !s.comfort_log && s.quad_distance_m == 40.0f, "Settings default OFF / 40 m");
    check(xr_stereo::clamp_quad_distance(1.0f) == 5.0f && xr_stereo::clamp_quad_distance(5000.0f) == 2000.0f &&
        xr_stereo::clamp_quad_distance(std::nanf("")) == 40.0f && xr_stereo::clamp_quad_distance(40.0f) == 40.0f, "distance clamp");
}

void feather_layers() {
    using namespace xr_stereo;
    { ProjectionLayer p; p.quad = true; p.quad_source_alpha = true; p.hud = true;
      const auto l = compose_layers(p);
      check(l.count == 3 && l.e[0].kind == LayerKind::world_quad && l.e[0].source_alpha && !l.e[0].unpremultiplied,
          "feathered world quad: SOURCE_ALPHA premultiplied (no UNPREMULTIPLIED bit)"); }
    // OFF (default): world quad opaque on every layer, no ramp in the recording, no new tokens.
    Rig a; a.stop_after = 8; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true; a.build(); auto ra = a.run();
    check(ra.success, "default run");
    for (auto& l : a.xr->layers) check(l.quad && !l.quad_source_alpha && !compose_layers(l).e[0].source_alpha, "OFF: world quad opaque");
    for (auto& j : a.blit->jobs) check(j.edge_feather == 0.0f, "OFF: feather constant 0 in every recording");
    check(!a.has_line("edge-feather") && !a.has_line("world-quad-distance-m=") && !a.has_line("quad-flags="), "OFF: no J-feather tokens");
    // Keys configured at their defaults: journal and layers identical; only the first-layer tokens are added.
    Rig b; b.stop_after = 8; hud_full_rig(b); b.settings.hud_layer = true; b.src.with_hud = true; b.settings.comfort_log = true;
    b.settings.edge_feather = 0.0f; b.settings.quad_distance_m = 40.0f; b.build(); auto rb = b.run();
    check(rb.success && a.j.calls() == b.j.calls() && a.xr->layers.size() == b.xr->layers.size(), "configured defaults: identical journal");
    for (size_t i = 0; i < a.xr->layers.size(); ++i)
        check(same_layer(a.xr->layers[i], b.xr->layers[i]) && !b.xr->layers[i].quad_source_alpha, "configured defaults: identical layers");
    check(a.lines.size() == b.lines.size(), "same line count");
    for (size_t i = 0; i < a.lines.size(); ++i) {
        if (a.lines[i].rfind("xr-stereo;event=first-layer;", 0) == 0) {
            // J-cockpit-fov-consumer: updated only to allow its trailing eye-fov/cockpit-fov tokens
            // (after the J-feather tokens); the J-feather tokens themselves are unchanged.
            const size_t k = a.lines[i].find(";cockpit-fov=");
            check(k != std::string::npos && b.lines[i] == a.lines[i].substr(0, k) +
                ";edge-feather=0.000;world-quad-distance-m=40.0;quad-flags=none(opaque)" + a.lines[i].substr(k), "first-layer tokens appended: " + b.lines[i]);
        }
        else check(a.lines[i] == b.lines[i], "configured defaults: line unchanged: " + a.lines[i]);
    }
    // ON: 4 % feather. World quad blends, the cockpit projection blends even without depth.
    for (bool depth : {true, false}) {
        Rig g; g.stop_after = 8; hud_full_rig(g); g.src.with_depth = depth; g.settings.hud_layer = true; g.src.with_hud = true;
        g.settings.edge_feather = 0.04f; g.settings.comfort_log = true; g.build(); auto r = g.run();
        check(r.success && !g.out->failed(), "feather run succeeds");
        for (auto& j : g.blit->jobs) check(j.edge_feather == 0.04f, "feather reaches every recording");
        for (auto& l : g.xr->layers) {
            const auto list = compose_layers(l);
            check(l.quad && l.quad_source_alpha && l.source_alpha && list.count == 3 && list.e[0].source_alpha && list.e[1].source_alpha &&
                list.e[2].source_alpha && !list.e[0].unpremultiplied && !list.e[1].unpremultiplied && !list.e[2].unpremultiplied,
                "ON: world quad, projection and HUD all premultiplied SOURCE_ALPHA");
            check(same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width && l.hud_height == l.quad_height, "ON: HUD pose == world quad");
        }
        check(g.has_line(std::string("proj-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)")) &&
            g.has_line(";rect=full-image;space=LOCAL;edge-feather=0.040;world-quad-distance-m=40.0;quad-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)"),
            "first-layer line names the feather, distance and quad blend");
        check(g.has_line("hud-pose-equals-world-quad=1"), "first-hud-layer: HUD pose equals the world quad");
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "feather lines fit the router: " + line);
    }
    // Clamp inside the consumer (the shader divides by the feather).
    { Rig g; g.stop_after = 3; hud_full_rig(g); g.settings.edge_feather = 0.9f; g.settings.comfort_log = true; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty() && g.blit->jobs[0].edge_feather == kMaxFeather && g.has_line("edge-feather=0.250"), "feather clamped to 0.25 in the consumer"); }
    // No world image: the quad-flags token says so (projection still blends with the ramp).
    { Rig g; g.stop_after = 3; g.src.lens_valid = true; g.settings.edge_feather = 0.04f; g.settings.comfort_log = true; g.build(); auto r = g.run();
      check(r.success && !g.xr->layers[0].quad && !g.xr->layers[0].quad_source_alpha && g.xr->layers[0].source_alpha &&
          g.has_line("quad-flags=no-world-quad-this-frame"), "no world quad: no quad blend flag, projection blends"); }
}

void feather_quad_distance() {
    using namespace xr_stereo;
    const float tan_[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f}, n[3] = {0.0f, 1.5f, 0.1f};
    for (float d : {5.0f, 12.5f, 40.0f, 100.0f, 400.0f, 2000.0f}) {
        const Quad q = world_quad(n, tan_, d);
        // Angular extent from the anchor: edges at atan(tan) for every distance.
        const double left = std::atan((double(q.pose.position[0]) - q.width / 2.0 - n[0]) / d);
        const double right = std::atan((double(q.pose.position[0]) + q.width / 2.0 - n[0]) / d);
        const double up = std::atan((double(q.pose.position[1]) + q.height / 2.0 - n[1]) / d);
        const double down = std::atan((double(q.pose.position[1]) - q.height / 2.0 - n[1]) / d);
        check(std::fabs(left + std::atan(0.6152)) < 1e-5 && std::fabs(right - std::atan(0.6152)) < 1e-5 &&
            std::fabs(up - std::atan(0.346)) < 1e-5 && std::fabs(down + std::atan(0.346)) < 1e-5, "quad subtends the lens FOV at any distance");
        check(std::fabs(double(q.pose.position[2]) - (0.1 - d)) < 1e-6 * d + 1e-5, "quad d metres ahead of the anchor");

        Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true;
        g.settings.quad_distance_m = d; g.settings.comfort_log = true; g.build(); auto r = g.run();
        check(r.success && !g.out->failed() && !g.xr->layers.empty(), "distance run succeeds");
        for (auto& l : g.xr->layers) {
            check(l.quad && l.hud && !l.hud_view_space && same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width &&
                l.hud_height == l.quad_height, "HUD keeps EXACTLY the world quad's pose and size at " + std::to_string(d) + " m");
            check(!l.quad_source_alpha, "distance alone keeps the world quad opaque");
            check(std::fabs(l.quad_width / d - 1.2304f) < 1e-5f && std::fabs(l.quad_height / d - 0.692f) < 1e-5f &&
                std::fabs(l.quad_pose.position[2] - (0.1f - d)) < 1e-6f * d + 1e-4f, "size scales with distance: angular size unchanged");
        }
        char tok[96]; std::snprintf(tok, sizeof tok, "quad-distance-m=%.1f;", double(d));
        check(g.has_line(tok) && g.has_line("hud-pose-equals-world-quad=1"), "mapping line and first-hud-layer at the configured distance");
        std::snprintf(tok, sizeof tok, ";edge-feather=0.000;world-quad-distance-m=%.1f;quad-flags=none(opaque)", double(d));
        check(g.has_line(tok), "first-layer distance token");
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "distance lines fit the router: " + line);
    }
    // Out-of-range Settings are clamped by the consumer.
    for (auto pr : {std::pair<float, float>{1.0f, 5.0f}, {9000.0f, 2000.0f}}) {
        Rig g; g.stop_after = 3; hud_full_rig(g); g.settings.quad_distance_m = pr.first; g.build(); auto r = g.run();
        check(r.success && std::fabs(g.xr->layers[0].quad_pose.position[2] - (0.1f - pr.second)) < 1e-3f, "consumer clamps the quad distance");
    }
}

// ------------------------------------------------ J-cockpit-fov-consumer ---
bool fov_is(const Fov& f, const float v[4]) { return f.left == v[0] && f.right == v[1] && f.up == v[2] && f.down == v[3]; }
std::string layer_fov_tokens(const ProjectionLayer& l) {
    auto d = [](float r) { return double(r) * 57.29577951308232; };
    char t[160];
    std::snprintf(t, sizeof t, ";layer-eye0-fov=%.2f,%.2f,%.2f,%.2f;layer-eye1-fov=%.2f,%.2f,%.2f,%.2f",
        d(l.views[0].fov.left), d(l.views[0].fov.right), d(l.views[0].fov.up), d(l.views[0].fov.down),
        d(l.views[1].fov.left), d(l.views[1].fov.right), d(l.views[1].fov.up), d(l.views[1].fov.down));
    return t;
}
const std::string* line_with(const Rig& g, const std::string& p) {
    for (auto& l : g.lines) if (l.find(p) != std::string::npos) return &l;
    return nullptr;
}

void cockpit_fov_math() {
    using xr_stereo::cockpit_fov_ok;
    const float ok[4] = {-0.95f, 0.75f, 0.90f, -0.88f}, edge[4] = {-1.5f, 1.5f, 1.5f, -1.5f}, canted[4] = {0.1f, 0.9f, 0.5f, 0.2f};
    check(cockpit_fov_ok(ok) && cockpit_fov_ok(edge) && cockpit_fov_ok(canted), "cockpit FOV accepted (asymmetric, 1.5 rad bound, same-sign canted)");
    const float nan[4] = {std::nanf(""), 0.75f, 0.90f, -0.88f}, inf[4] = {-0.9f, INFINITY, 0.9f, -0.9f};
    const float lr[4] = {0.5f, 0.5f, 0.9f, -0.9f}, ud[4] = {-0.9f, 0.9f, -0.2f, 0.1f}, wide[4] = {-1.5001f, 0.9f, 0.9f, -0.9f};
    const float pi2[4] = {-0.9f, 0.9f, 1.5707964f, -0.9f};
    for (const float* bad : {nan, inf, lr, ud, wide, pi2}) check(!cockpit_fov_ok(bad), "cockpit FOV refused");
    check(!cockpit_fov_ok(nullptr), "null refused");
    const stereo::StereoFrame f{};
    check(!f.cockpit_fov_valid && f.cockpit_fov[0][0] == 0.0f && f.cockpit_fov[1][3] == 0.0f, "StereoFrame cockpit FOV default OFF / zero");
}

void cockpit_fov_flag_off_unchanged() {
    // Flag OFF (values present in the frame but cockpit_fov_valid = false): the producer
    // values are ignored completely; the layer uses the lens exactly as before.
    for (bool lens : {true, false}) {
        Rig a; a.stop_after = 10; hud_full_rig(a); a.src.lens_valid = lens; a.settings.hud_layer = true; a.src.with_hud = true; a.build(); auto ra = a.run();
        Rig b; b.stop_after = 10; hud_full_rig(b); b.src.lens_valid = lens; b.settings.hud_layer = true; b.src.with_hud = true;
        std::memset(b.src.cfov, 0, sizeof b.src.cfov); b.build(); auto rb = b.run(); // different ignored values
        check(ra.success && rb.success && a.j.calls() == b.j.calls() && a.lines == b.lines, "flag OFF: journal and lines independent of cockpit_fov values");
        check(a.xr->layers.size() == b.xr->layers.size() && !a.xr->layers.empty(), "flag OFF: layers");
        for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], b.xr->layers[i]), "flag OFF: identical layers");
        const auto lf = xr_stereo::fov_from_lens(a.src.lens);
        for (auto& l : a.xr->layers) {
            if (lens) check(std::memcmp(&l.views[0].fov, &lf, sizeof lf) == 0 && std::memcmp(&l.views[1].fov, &lf, sizeof lf) == 0, "flag OFF: lens FOV");
            else check(l.views[0].fov.left == -0.8f && l.views[1].fov.down == -0.85f, "flag OFF without lens: the pose FOV");
        }
        const auto& c = a.out->counters();
        check(c.cockpit_fov_frames == 0 && c.cockpit_fov_invalid == 0 && c.lens_frames == (lens ? c.new_frames : 0), "flag OFF counters");
        // New tokens only: the headset FOV (fake views -0.9/0.8/0.85/-0.95 rad) and the FOV source.
        const std::string eyes = ";eye0-fov=-51.57,45.84,48.70,-54.43;eye1-fov=-51.57,45.84,48.70,-54.43";
        const std::string src = lens ? "lens" : "pose";
        const std::string* fl = line_with(a, "xr-stereo;event=first-layer;");
        check(fl && fl->size() > 13 + src.size() && fl->compare(fl->size() - 13 - src.size(), std::string::npos, ";cockpit-fov=" + src) == 0 &&
            fl->find(";rect=full-image;space=LOCAL;cockpit-fov=") != std::string::npos, "first-layer: existing tokens unchanged, cockpit FOV source trailing");
        const std::string* ff = line_with(a, "xr-stereo;event=first-layer-fov;");
        check(ff && *ff == "xr-stereo;event=first-layer-fov" + eyes + ";cockpit-fov=" + src + layer_fov_tokens(a.xr->layers[0]),
            "first-layer-fov: headset FOV, source and the submitted per-eye FOV");
        check(lens ? ff->find(";layer-eye0-fov=-31.60,31.60,19.09,-19.09;") != std::string::npos
                   : ff->find(";layer-eye0-fov=-45.84,40.11,42.97,-48.70;") != std::string::npos, "first-layer-fov degrees");
        const std::string* mp = line_with(a, "xr-stereo;event=mapping;");
        check(mp && mp->size() > eyes.size() && mp->compare(mp->size() - eyes.size(), std::string::npos, eyes) == 0, "mapping line ends with the headset FOV");
        const std::string* st = line_with(a, "xr-stereo;event=summary-timing;t-ms=");
        check(st && st->find(";hud=layer" + eyes + ";cockpit-fov=" + src + ";cockpit-fov-frames=0;cockpit-fov-invalid=0") != std::string::npos,
            "summary-timing: trailing headset FOV, source and counters");
        check(a.has_line(";source=2560x1440;recommended-eye1=2688x2688"), "swapchains line: recommended size of eye 1 trailing");
        for (auto& line : a.lines) check(line.size() < feature_log::line_cap, "cockpit-fov lines fit the router: " + line);
    }
}

void cockpit_fov_flag_on() {
    for (float feather : {0.0f, 0.04f}) {
        Rig a; a.stop_after = 12; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true;
        a.settings.edge_feather = feather; a.settings.comfort_log = feather > 0; a.build(); auto ra = a.run();
        Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.src.cockpit_fov = true;
        g.settings.edge_feather = feather; g.settings.comfort_log = feather > 0; g.build(); auto r = g.run();
        check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "flag ON run succeeds");
        check(a.j.calls() == g.j.calls(), "flag ON: same acquire/blit/release journal as the lens path");
        check(g.xr->layers.size() == a.xr->layers.size() && !g.xr->layers.empty(), "flag ON: same layer count");
        for (size_t i = 0; i < g.xr->layers.size(); ++i) {
            const auto& l = g.xr->layers[i];
            const auto& base = a.xr->layers[i];
            check(fov_is(l.views[0].fov, g.src.cfov[0]) && fov_is(l.views[1].fov, g.src.cfov[1]), "flag ON: layer FOV = the frame's per-eye cockpit FOV");
            check(l.views[0].fov.right != l.views[1].fov.right && l.views[0].fov.left != l.views[1].fov.left, "asymmetric per-eye FOV preserved (not averaged)");
            check(same_pose(l.views[0].pose, base.views[0].pose) && same_pose(l.views[1].pose, base.views[1].pose), "flag ON: the rendered (frame) poses");
            check(l.quad && same_pose(l.quad_pose, base.quad_pose) && l.quad_width == base.quad_width && l.quad_height == base.quad_height,
                "world quad still lens-based (identical to the lens path)");
            check(std::fabs(l.quad_width - 49.216f) < 1e-3f && std::fabs(l.quad_height - 27.68f) < 1e-3f, "world quad subtends the lens at 40 m");
            check(l.hud && same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width && l.hud_height == l.quad_height &&
                same_pose(l.hud_pose, base.hud_pose), "HUD pose still equals the world quad pose");
            check(l.source_alpha == base.source_alpha && l.quad_source_alpha == base.quad_source_alpha && l.quad_source_alpha == (feather > 0),
                "blend flags unchanged (feather applies to the layer border as before)");
        }
        for (auto& j : g.blit->jobs) check(j.edge_feather == feather, "feather in every recording");
        const auto& c = g.out->counters();
        check(c.cockpit_fov_frames == c.new_frames && c.cockpit_fov_frames > 0 && c.lens_frames == 0 && c.cockpit_fov_invalid == 0, "flag ON counters");
        const std::string* fl = line_with(g, "xr-stereo;event=first-layer;");
        check(fl && fl->size() > 16 && fl->compare(fl->size() - 16, 16, ";cockpit-fov=hmd") == 0, "first-layer: cockpit-fov=hmd");
        char fov[96]; std::snprintf(fov, sizeof fov, "fov=%.4f,%.4f,%.4f,%.4f;", g.src.cfov[0][0], g.src.cfov[0][1], g.src.cfov[0][2], g.src.cfov[0][3]);
        check(fl->find(fov) != std::string::npos, "first-layer fov token shows the submitted eye-0 cockpit FOV");
        check(g.has_line(";cockpit-fov=hmd;cockpit-fov-frames=") && g.has_line("cockpit-fov-invalid=0"), "summary-timing: hmd");
        if (feather > 0) check(fl->find("edge-feather=0.040;world-quad-distance-m=40.0;quad-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);cockpit-fov=hmd") != std::string::npos,
            "feather tokens followed by the cockpit-fov token");
        check(g.has_line("xr-stereo;event=first-layer-fov;eye0-fov=-51.57,45.84,48.70,-54.43;eye1-fov=-51.57,45.84,48.70,-54.43;cockpit-fov=hmd;"
            "layer-eye0-fov=-54.43,42.97,51.57,-50.42;layer-eye1-fov=-45.84,52.71,52.14,-49.27"), "first-layer-fov: headset vs submitted asymmetric per-eye FOV");
        check(g.has_line("hud-pose-equals-world-quad=1"), "first-hud-layer: HUD pose equals the world quad");
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "flag ON lines fit the router: " + line);
    }
    // Reused (paced) layers keep their own cockpit FOV.
    Rig g; g.stop_after = 30; hud_full_rig(g); g.src.cockpit_fov = true; g.publish_until = 3; g.build(); auto r = g.run();
    check(r.success && g.out->counters().reused > 0, "pacing reuse with the cockpit FOV");
    for (auto& l : g.xr->layers) check(fov_is(l.views[0].fov, g.src.cfov[0]) && fov_is(l.views[1].fov, g.src.cfov[1]), "re-submitted layer keeps the cockpit FOV");
    // Without a frame pose (located views of this frame) the frame's cockpit FOV still wins.
    Rig p; p.stop_after = 6; p.src.pose_valid = false; p.src.cockpit_fov = true; p.build(); auto rp = p.run();
    check(rp.success && !p.xr->layers.empty() && p.out->counters().pose_from_located > 0, "located-pose run");
    for (auto& l : p.xr->layers) check(fov_is(l.views[0].fov, p.src.cfov[0]) && fov_is(l.views[1].fov, p.src.cfov[1]) &&
        l.views[0].pose.position[0] == -0.032f, "located poses with the rendered cockpit FOV");
}

void cockpit_fov_invalid() {
    const float nan = std::nanf("");
    const float bad_sets[][4] = {{nan, 0.92f, 0.91f, -0.86f}, {0.9f, 0.8f, 0.91f, -0.86f}, {-0.9f, 0.8f, -0.9f, 0.86f},
                                 {-1.6f, 0.8f, 0.91f, -0.86f}, {-0.9f, 0.8f, 0.91f, INFINITY}};
    for (const auto& bad : bad_sets) for (unsigned eye : {0u, 1u}) {
        // Every frame flagged with a refused FOV: no layer is ever submitted, nothing acquired or blitted.
        Rig g; g.stop_after = 8; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.src.cockpit_fov = true;
        g.src.cockpit_bad_every = 1; std::memcpy(g.src.cfov_bad, g.src.cfov, sizeof g.src.cfov_bad); std::memcpy(g.src.cfov_bad[eye], bad, sizeof bad);
        g.build(); auto r = g.run();
        const auto& c = g.out->counters();
        check(!g.out->failed() && r.complete && g.src.bad_releases == 0 && g.xr->violations == 0, "refused cockpit FOV: no failure, slots released");
        check(g.xr->layers.empty() && g.j.count("end_frame_projection") == 0 && r.projection_layers == 0, "refused cockpit FOV: no projection layer submitted");
        check(g.j.prefix("xr.acquire") == 0 && g.j.prefix("d3d.record") == 0 && g.j.prefix("d3d.execute") == 0, "refused before any acquire or GPU work");
        check(c.cockpit_fov_invalid == c.acquired_frames && c.acquired_frames > 0 && c.released_frames == c.acquired_frames &&
            c.cockpit_fov_frames == 0 && c.new_frames == 0, "refusals counted once per producer frame");
        check(c.zero_no_frame == c.cockpit_fov_invalid, "accounted as zero-layer frames");
        check(g.has_line("cockpit-fov-invalid=" + std::to_string(c.cockpit_fov_invalid)), "summary-timing counts cockpit-fov-invalid");
    }
    // Mixed: every 3rd frame refused. Valid frames submit the cockpit FOV; a refused frame
    // submits zero layers and is NOT replaced by the previous layer (no mismatched or stale layer).
    Rig g; g.stop_after = 20; hud_full_rig(g); g.src.cockpit_fov = true; g.src.cockpit_bad_every = 3; g.build(); auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "mixed run succeeds");
    for (auto& l : g.xr->layers) check(fov_is(l.views[0].fov, g.src.cfov[0]) && fov_is(l.views[1].fov, g.src.cfov[1]), "only valid cockpit FOVs submitted");
    check(c.cockpit_fov_invalid > 0 && c.cockpit_fov_frames > 0 && c.cockpit_fov_invalid + c.cockpit_fov_frames == c.acquired_frames &&
        c.lens_frames == 0 && c.reused == 0, "mixed counters (no reuse across a refusal)");
    for (unsigned f = 0; f < r.begun; ++f) {
        const auto s = frame_slice(g.j.calls(), f);
        const std::string id = std::to_string(f + 1);
        const bool bad = (f + 1) % 3 == 0;
        check(!s.empty() && s.back() == (bad ? "end_frame" : "end_frame_projection"), "refused frame ends with zero layers, valid frames with the layer");
        if (bad) check(std::count(s.begin(), s.end(), "source.release " + id) == 1 && std::count(s.begin(), s.end(), "xr.acquire 0") == 0,
            "refused frame released unused");
    }
    check(g.xr->layers.size() == c.cockpit_fov_frames, "one layer per valid frame");
}

// ------------------------------------------------------- J-hud-placement ---
// Tangent-space direction (x/z, y/z as seen from `n` looking along -Z) of the four corners of a
// LOCAL quad with identity orientation, plus its centre: [0..3] corners, [4] centre.
struct Dirs { double t[5][2]; };
Dirs quad_dirs(const xr_owner::Pose& p, float w, float h, const double n[3]) {
    Dirs d{};
    const double dz = n[2] - double(p.position[2]);
    const double cx = double(p.position[0]) - n[0], cy = double(p.position[1]) - n[1];
    const double sx[5] = {-1, 1, 1, -1, 0}, sy[5] = {-1, -1, 1, 1, 0};
    for (int k = 0; k < 5; ++k) {
        d.t[k][0] = (cx + sx[k] * w / 2.0) / dz;
        d.t[k][1] = (cy + sy[k] * h / 2.0) / dz;
    }
    return d;
}
// The world quad's anchor, recovered from its own placement (world_quad(): centre = n + D*(cx, cy, -1)).
void anchor_of(const ProjectionLayer& l, const float tan_[4], float D, double n[3]) {
    n[0] = double(l.quad_pose.position[0]) - double(D) * (double(tan_[1]) - tan_[0]) * 0.5;
    n[1] = double(l.quad_pose.position[1]) - double(D) * (double(tan_[2]) - tan_[3]) * 0.5;
    n[2] = double(l.quad_pose.position[2]) + double(D);
}
bool angles_match(const Dirs& a, const Dirs& b, double tol) {
    for (int k = 0; k < 5; ++k) for (int i = 0; i < 2; ++i)
        if (!(std::fabs(std::atan(a.t[k][i]) - std::atan(b.t[k][i])) < tol)) return false;
    return true;
}
const float kAsymLens[4] = {0.50f, 0.70f, 0.40f, 0.30f}; // off-centre lens: the quad centre has a lateral/vertical offset

void hud_placement_math() {
    using namespace xr_stereo;
    // Clamps.
    check(clamp_hud_quad_distance(0.0f) == 0.0f && clamp_hud_quad_distance(-3.0f) == 0.0f && clamp_hud_quad_distance(std::nanf("")) == 0.0f &&
        clamp_hud_quad_distance(0.1f) == 0.3f && clamp_hud_quad_distance(3.0f) == 3.0f && clamp_hud_quad_distance(9000.0f) == 2000.0f, "HUD distance clamp (0 = unset)");
    check(clamp_hud_scale(1.0f) == 1.0f && clamp_hud_scale(0.1f) == 0.25f && clamp_hud_scale(5.0f) == 2.0f && clamp_hud_scale(std::nanf("")) == 1.0f &&
        clamp_hud_scale(0.6f) == 0.6f, "HUD scale clamp");
    check(clamp_hud_head_distance(2.5f) == 2.5f && clamp_hud_head_distance(0.0f) == 0.5f && clamp_hud_head_distance(50.0f) == 20.0f &&
        clamp_hud_head_distance(std::nanf("")) == 2.5f, "head distance clamp");
    check(clamp_hud_head_width(1.6f) == 1.6f && clamp_hud_head_width(0.0f) == 0.3f && clamp_hud_head_width(50.0f) == 10.0f &&
        clamp_hud_head_width(std::nanf("")) == 1.6f, "head width clamp");
    const Settings s{};
    check(s.hud_quad_distance_m == 0.0f && s.hud_scale == 1.0f && s.hud_head_distance_m == 2.5f && s.hud_head_width_m == 1.6f && !s.hud_placement_log,
        "Settings defaults: unset / 1 / 2.5 / 1.6, no placement tokens");
    // Unset distance + scale 1: EXACTLY the world quad (bitwise).
    const float n[3] = {0.013f, 1.5f, 0.1f};
    for (const float* tan_ : {static_cast<const float*>(kAsymLens), static_cast<const float*>(nullptr)}) {
        const float sym[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
        const float* t = tan_ ? tan_ : sym;
        for (float D : {5.0f, 40.0f, 2000.0f}) {
            const Quad w = world_quad(n, t, D), h = hud_world_quad(n, t, D, 0.0f, 1.0f);
            check(same_pose(w.pose, h.pose) && w.width == h.width && w.height == h.height, "unset HUD keys: hud_world_quad == world_quad bitwise");
            const Quad e = hud_world_quad(n, t, D, D, 1.0f);
            check(same_pose(w.pose, e.pose) && w.width == e.width && w.height == e.height, "HUD distance equal to the world distance: identical");
            const double nd[3] = {n[0], n[1], n[2]};
            const Dirs wd = quad_dirs(w.pose, w.width, w.height, nd);
            for (float hd : {0.5f, 1.0f, 2.0f, 3.0f, 10.0f, 39.0f, 100.0f, 2000.0f}) {
                const Quad q = hud_world_quad(n, t, D, hd, 1.0f);
                check(q.pose.orientation[3] == 1.0f && q.pose.orientation[0] == 0.0f && q.pose.orientation[1] == 0.0f && q.pose.orientation[2] == 0.0f,
                    "HUD keeps the world quad's identity orientation");
                check(std::fabs(double(q.pose.position[2]) - (double(n[2]) - hd)) < 1e-6 * hd + 1e-5, "HUD at the configured distance ahead of the anchor");
                check(angles_match(quad_dirs(q.pose, q.width, q.height, nd), wd, 2e-6),
                    "corner and centre directions from the anchor equal the world quad's at " + std::to_string(hd) + " m");
                check(std::fabs(q.width / hd - w.width / D) < 1e-5f && std::fabs(q.height / hd - w.height / D) < 1e-5f, "size scales with the distance");
            }
            // Scale about the centre: the centre direction is kept, every corner's tangent offset from it scales.
            for (float sc : {0.25f, 0.6f, 0.7f, 1.5f, 2.0f}) for (float hd : {0.0f, 3.0f}) {
                const Quad q = hud_world_quad(n, t, D, hd, sc);
                const Quad base = hud_world_quad(n, t, D, hd, 1.0f);
                check(same_pose(q.pose, base.pose) && q.width == base.width * sc && q.height == base.height * sc, "scale: same pose, size * scale");
                const Dirs qd = quad_dirs(q.pose, q.width, q.height, nd);
                bool ok = std::fabs(qd.t[4][0] - wd.t[4][0]) < 1e-6 && std::fabs(qd.t[4][1] - wd.t[4][1]) < 1e-6;
                for (int k = 0; k < 4; ++k) for (int i = 0; i < 2; ++i)
                    ok = ok && std::fabs((qd.t[k][i] - qd.t[4][i]) - sc * (wd.t[k][i] - wd.t[4][i])) < 1e-5;
                check(ok, "scale shrinks/grows the HUD about its centre direction (tangent space)");
            }
        }
    }
    check(same_pose(scale_quad(world_quad(n, kAsymLens, 40.0f), 1.0f).pose, world_quad(n, kAsymLens, 40.0f).pose), "scale 1 is the identity");
    // Head-locked quad: defaults reproduce the J-hud-consumer fallback exactly.
    const Quad d = hud_head_quad(2.5f, 1.6f, 1.0f, 2560, 1440);
    check(d.pose.position[0] == 0.0f && d.pose.position[1] == 0.0f && d.pose.position[2] == -2.5f && d.pose.orientation[3] == 1.0f &&
        d.width == 1.6f && d.height == 1.6f * float(1440) / float(2560), "head-locked defaults: 2.5 m, 1.6 m wide, source aspect");
    const Quad k = hud_head_quad(4.0f, 2.0f, 0.5f, 2560, 1440);
    check(k.pose.position[2] == -4.0f && k.width == 1.0f && std::fabs(k.height - 0.5625f) < 1e-7f, "head-locked keys: distance, width * scale, aspect kept");
    check(hud_head_quad(2.5f, 1.6f, 1.0f, 0, 0).height == 0.0f, "zero source width: no division");
    // Tokens.
    Settings t; t.hud_quad_distance_m = 3.0f; t.hud_scale = 0.7f;
    check(hud_placement_tokens(t) == ";hud-quad-distance-m=3.00;hud-scale=0.700;hud-head-distance-m=2.50;hud-head-width-m=1.60", "placement tokens");
    check(hud_placement_tokens(Settings{}) == ";hud-quad-distance-m=world;hud-scale=1.000;hud-head-distance-m=2.50;hud-head-width-m=1.60", "placement tokens, unset distance");
}

void hud_placement_config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("log_enabled=true\n");
    check(d.issue == ConfigIssue::none && d.config.xr_hud_quad_distance_m() == 0.0f && d.config.xr_hud_scale() == 1.0f &&
        d.config.xr_hud_head_distance_m() == 2.5f && d.config.xr_hud_head_width_m() == 1.6f && !d.config.xr_hud_placement_set() &&
        !d.config.xr_hud_quad_distance_set(), "J-hud-placement keys default unset / 1 / 2.5 / 1.6");
    auto on = parse_config("xr_stereo_output=true\nxr_hud_layer=true\nxr_hud_quad_distance_m=3\nxr_hud_scale=0.7\nxr_hud_head_distance_m=4.5\nxr_hud_head_width_m=2.25\n");
    check(on.issue == ConfigIssue::none && on.config.xr_hud_quad_distance_m() == 3.0f && std::fabs(on.config.xr_hud_scale() - 0.7f) < 1e-7f &&
        on.config.xr_hud_head_distance_m() == 4.5f && on.config.xr_hud_head_width_m() == 2.25f && on.config.xr_hud_placement_set() &&
        on.config.xr_hud_quad_distance_set(), "J-hud-placement keys parsed");
    check(parse_config("xr_hud_scale=1\n").config.xr_hud_placement_set() && !parse_config("xr_hud_scale=1\n").config.xr_hud_quad_distance_set(),
        "any one key marks the placement configured");
    check(parse_config("xr_hud_head_width_m=1.6\n").config.xr_hud_placement_set(), "a head key alone marks the placement configured");
    check(parse_config("xr_hud_quad_distance_m=0\n").config.xr_hud_quad_distance_m() == 0.5f, "HUD distance clamped to 0.5 m");
    check(parse_config("xr_hud_quad_distance_m=99999\n").config.xr_hud_quad_distance_m() == 2000.0f, "HUD distance clamped to 2000 m");
    check(parse_config("xr_hud_scale=0.1\n").config.xr_hud_scale() == 0.25f && parse_config("xr_hud_scale=3\n").config.xr_hud_scale() == 2.0f, "scale clamped");
    check(parse_config("log_enabled=true\n").config.gpu_copy_step5_units_per_metre() == 39.6f && parse_config("gpu_copy_step5_units_per_metre=43.5\n").config.gpu_copy_step5_units_per_metre() == 43.5f &&
          parse_config("gpu_copy_step5_units_per_metre=5\n").config.gpu_copy_step5_units_per_metre() == 20.0f && parse_config("gpu_copy_step5_units_per_metre=500\n").config.gpu_copy_step5_units_per_metre() == 100.0f, "units per metre default/set/clamped");
    check(parse_config("xr_hud_head_distance_m=0.1\n").config.xr_hud_head_distance_m() == 0.5f &&
        parse_config("xr_hud_head_distance_m=30\n").config.xr_hud_head_distance_m() == 20.0f, "head distance clamped");
    check(parse_config("xr_hud_head_width_m=0.1\n").config.xr_hud_head_width_m() == 0.3f &&
        parse_config("xr_hud_head_width_m=11\n").config.xr_hud_head_width_m() == 10.0f, "head width clamped");
    for (const char* bad : {"xr_hud_quad_distance_m=abc\n", "xr_hud_quad_distance_m=-3\n", "xr_hud_quad_distance_m=3m\n", "xr_hud_quad_distance_m=.5\n",
                            "xr_hud_scale=0.6x\n", "xr_hud_scale=-0.6\n", "xr_hud_scale=6e-1\n", "xr_hud_scale=1.\n", "xr_hud_scale=\n",
                            "xr_hud_head_distance_m=two\n", "xr_hud_head_distance_m=1234567890123\n", "xr_hud_head_width_m=1,6\n", "xr_hud_head_width_m=0.0.1\n"})
        check(parse_config(bad).issue != ConfigIssue::none, std::string("invalid HUD placement value rejects the config: ") + bad);
    check(parse_config("xr_hud_scale=0.7\nxr_hud_scale=0.7\n").issue == ConfigIssue::duplicate, "duplicate refuses");
    check(parse_config("xr_hud_quad_distance_m=-3\n").issue == ConfigIssue::out_of_range, "negative distance: out_of_range like the J-feather keys");
}

void hud_placement_defaults_unchanged() {
    // Keys at their defaults with the token flag on: journal and layers identical to the default run;
    // only the first-hud-layer and summary-hud lines gain trailing tokens.
    for (auto mode : {xr_stereo::HudMode::world_quad, xr_stereo::HudMode::head_locked}) {
        Rig a; a.stop_after = 8; hud_full_rig(a); a.settings.hud_layer = true; a.settings.hud_mode = mode; a.src.with_hud = true; a.build(); auto ra = a.run();
        Rig b; b.stop_after = 8; hud_full_rig(b); b.settings.hud_layer = true; b.settings.hud_mode = mode; b.src.with_hud = true;
        b.settings.hud_placement_log = true; b.settings.hud_quad_distance_m = 0.0f; b.settings.hud_scale = 1.0f;
        b.settings.hud_head_distance_m = 2.5f; b.settings.hud_head_width_m = 1.6f; b.build(); auto rb = b.run();
        check(ra.success && rb.success && a.j.calls() == b.j.calls() && a.xr->layers.size() == b.xr->layers.size() && !a.xr->layers.empty(),
            "configured defaults: identical journal");
        for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], b.xr->layers[i]), "configured defaults: identical layers");
        if (mode == xr_stereo::HudMode::world_quad)
            for (auto& l : b.xr->layers) check(same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width && l.hud_height == l.quad_height,
                "configured defaults: HUD pose == world quad");
        check(!a.has_line("hud-quad-distance-m=") && !a.has_line("hud-distance-m=") && !a.has_line("hud-scale="), "unset: no placement tokens");
        check(a.lines.size() == b.lines.size(), "same line count");
        const std::string tok = ";hud-quad-distance-m=world;hud-scale=1.000;hud-head-distance-m=2.50;hud-head-width-m=1.60";
        for (size_t i = 0; i < a.lines.size() && i < b.lines.size(); ++i) {
            const std::string& x = a.lines[i];
            if (x.rfind("xr-stereo;event=summary-hud;", 0) == 0) check(b.lines[i] == x + tok, "summary-hud: tokens appended: " + b.lines[i]);
            else if (x.rfind("xr-stereo;event=first-hud-layer;", 0) == 0) {
                check(b.lines[i].rfind(x + ";hud-distance-m=", 0) == 0 && b.lines[i].size() > tok.size() &&
                    b.lines[i].compare(b.lines[i].size() - tok.size(), tok.size(), tok) == 0, "first-hud-layer: tokens appended: " + b.lines[i]);
                const char* want = mode == xr_stereo::HudMode::world_quad ? ";hud-distance-m=40.000;hud-angular-deg=63.20x38.17" : ";hud-distance-m=2.500;hud-angular-deg=35.49x20.41";
                check(b.lines[i].find(want) != std::string::npos, std::string("first-hud-layer: actual distance and angular size ") + want + " in " + b.lines[i]);
                check(b.lines[i].find(mode == xr_stereo::HudMode::world_quad ? "hud-pose-equals-world-quad=1" : "hud-pose-equals-world-quad=0") != std::string::npos,
                    "configured defaults keep the pose-equality token");
            }
            else check(b.lines[i] == x, "configured defaults: line unchanged: " + x);
        }
        for (auto& line : b.lines) check(line.size() < feature_log::line_cap, "placement lines fit the router: " + line);
    }
}

void hud_placement_world_quad_runs() {
    for (bool asym : {false, true}) for (float D : {40.0f, 100.0f}) for (float hd : {0.5f, 1.0f, 3.0f, 10.0f, 2000.0f}) for (float sc : {1.0f, 0.6f}) {
        Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true;
        if (asym) std::memcpy(g.src.lens, kAsymLens, sizeof g.src.lens);
        g.settings.quad_distance_m = D; g.settings.comfort_log = true;
        g.settings.hud_quad_distance_m = hd; g.settings.hud_scale = sc; g.settings.hud_placement_log = true; g.build(); auto r = g.run();
        const std::string what = std::string(asym ? "asym" : "sym") + " D=" + std::to_string(D) + " hud=" + std::to_string(hd) + " s=" + std::to_string(sc);
        check(r.success && !g.out->failed() && !g.xr->layers.empty() && g.src.bad_releases == 0 && g.xr->violations == 0, "placement run succeeds: " + what);
        const auto& c = g.out->counters();
        check(c.hud_frames == c.new_frames && c.hud_layers == c.submitted && c.hud_skipped == 0, "HUD counters unchanged by placement: " + what);
        for (auto& l : g.xr->layers) {
            check(l.quad && l.hud && !l.hud_view_space, "world quad + LOCAL HUD: " + what);
            const auto list = xr_stereo::compose_layers(l);
            check(list.count == 3 && list.e[2].kind == xr_stereo::LayerKind::hud_quad && list.e[2].space == xr_stereo::LayerSpace::local &&
                list.e[2].source_alpha && !list.e[2].unpremultiplied, "layer list unchanged: " + what);
            check(std::fabs(l.quad_width - D * (g.src.lens[0] + g.src.lens[1])) < 1e-3f * D,
                "the world quad itself is unchanged by the HUD keys: " + what);
            double n[3]; anchor_of(l, g.src.lens, D, n);
            const Dirs w = quad_dirs(l.quad_pose, l.quad_width, l.quad_height, n);
            const Dirs h = quad_dirs(l.hud_pose, l.hud_width, l.hud_height, n);
            check(std::fabs((n[2] - l.hud_pose.position[2]) - hd) < 1e-4 * hd + 2e-5 * D, "HUD at the configured distance: " + what);
            check(l.hud_pose.orientation[3] == 1.0f && l.hud_pose.orientation[0] == 0.0f, "same identity orientation: " + what);
            check(std::fabs(std::atan(h.t[4][0]) - std::atan(w.t[4][0])) < 5e-5 && std::fabs(std::atan(h.t[4][1]) - std::atan(w.t[4][1])) < 5e-5,
                "HUD centre in the world quad's direction: " + what);
            bool ok = true;
            for (int k = 0; k < 4; ++k) for (int i = 0; i < 2; ++i)
                ok = ok && std::fabs((h.t[k][i] - h.t[4][i]) - sc * (w.t[k][i] - w.t[4][i])) < 1e-4;
            check(ok, "corner directions = world quad's (scaled about the centre by the HUD scale): " + what);
            if (sc == 1.0f) check(angles_match(h, w, 5e-5), "scale 1: every corner at the world quad's angle: " + what);
        }
        const bool same = hd == D && sc == 1.0f;
        check(g.has_line(same ? "hud-pose-equals-world-quad=1" : "hud-pose-equals-world-quad=0"), "pose-equality token: " + what);
        char tok[128]; std::snprintf(tok, sizeof tok, ";hud-distance-m=%.3f;", double(hd));
        check(g.has_line(tok), std::string("first-hud-layer actual distance ") + tok + what);
        std::snprintf(tok, sizeof tok, ";hud-quad-distance-m=%.2f;hud-scale=%.3f;hud-head-distance-m=2.50;hud-head-width-m=1.60", double(hd), double(sc));
        check(g.has_line(tok) && g.has_line("xr-stereo;event=summary-hud;"), "placement tokens: " + what);
        const auto& l0 = g.xr->layers[0];
        char sz[64]; std::snprintf(sz, sizeof sz, "hud-size=%.3fx%.3f;", l0.hud_width, l0.hud_height);
        check(g.has_line(sz), "first-hud-layer reports the actual HUD size: " + what);
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "placement lines fit the router: " + line);
    }
    // Typical next-test values: 3 m, scale 0.7 at the 40 m world quad (symmetric live lens).
    Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true;
    g.settings.hud_quad_distance_m = 3.0f; g.settings.hud_scale = 0.7f; g.settings.hud_placement_log = true; g.build(); auto r = g.run();
    const auto& l = g.xr->layers.at(0);
    check(r.success && std::fabs(l.hud_width - 3.0f * 1.2304f * 0.7f) < 1e-4f && std::fabs(l.hud_height - 3.0f * 0.692f * 0.7f) < 1e-4f &&
        std::fabs(l.hud_pose.position[2] - (0.1f - 3.0f)) < 1e-5f && l.hud_pose.position[0] == l.quad_pose.position[0] &&
        l.hud_pose.position[1] == l.quad_pose.position[1], "3 m / 0.7: 2.584 x 1.453 m at 3 m ahead, same centre line (symmetric lens)");
    check(g.has_line(";hud-distance-m=3.000;hud-angular-deg=46.60x27.23;hud-quad-distance-m=3.00;hud-scale=0.700"), "3 m / 0.7 first-hud-layer tokens");
}

void hud_placement_head_locked() {
    Rig g; g.stop_after = 6; g.settings.hud_layer = true; g.settings.hud_mode = xr_stereo::HudMode::head_locked; g.src.with_hud = true;
    g.settings.hud_head_distance_m = 4.0f; g.settings.hud_head_width_m = 2.4f; g.settings.hud_scale = 0.5f;
    g.settings.hud_quad_distance_m = 3.0f; // world-quad key: ignored in head-locked mode
    g.settings.hud_placement_log = true; g.build(); auto r = g.run();
    check(r.success && !g.xr->layers.empty() && g.out->counters().hud_layers == r.begun, "head-locked placement run");
    for (auto& l : g.xr->layers) {
        check(l.hud && l.hud_view_space && l.hud_pose.position[0] == 0.0f && l.hud_pose.position[1] == 0.0f && l.hud_pose.position[2] == -4.0f &&
            l.hud_pose.orientation[3] == 1.0f, "head-locked: VIEW, identity, 4 m ahead");
        check(l.hud_width == 1.2f && std::fabs(l.hud_height - 0.675f) < 1e-6f, "head-locked: 2.4 m * 0.5 wide, source aspect");
    }
    check(g.has_line("hud-mode=head-locked;hud-space=VIEW;hud-pos=0.000,0.000,-4.000;hud-size=1.200x0.675;hud-pose-equals-world-quad=no-world-quad-this-frame;hud-distance-m=4.000;"),
        "head-locked first-hud-layer: actual distance");
    check(g.has_line(";hud-quad-distance-m=3.00;hud-scale=0.500;hud-head-distance-m=4.00;hud-head-width-m=2.40"), "head-locked tokens");
    // Out-of-range Settings clamped by the consumer.
    Rig c; c.stop_after = 3; c.settings.hud_layer = true; c.settings.hud_mode = xr_stereo::HudMode::head_locked; c.src.with_hud = true;
    c.settings.hud_head_distance_m = 100.0f; c.settings.hud_head_width_m = 0.01f; c.settings.hud_scale = 9.0f; c.build(); auto rc = c.run();
    check(rc.success && c.xr->layers.at(0).hud_pose.position[2] == -20.0f && std::fabs(c.xr->layers[0].hud_width - 0.6f) < 1e-6f,
        "consumer clamps head distance 20 m, width 0.3 m, scale 2");
    Rig q; q.stop_after = 3; hud_full_rig(q); q.settings.hud_layer = true; q.src.with_hud = true; q.settings.hud_quad_distance_m = 0.01f;
    q.settings.hud_scale = 0.0f; q.build(); auto rq = q.run();
    const auto& ql = q.xr->layers.at(0);
    double n[3]; anchor_of(ql, q.src.lens, 40.0f, n);
    check(rq.success && std::fabs((n[2] - ql.hud_pose.position[2]) - 0.3) < 1e-4 && std::fabs(ql.hud_width - 0.3f * 1.2304f * 0.25f) < 1e-5f,
        "consumer clamps HUD distance 0.3 m (COCKPIT-3: reaches the 0.395 m reflector glass) and scale 0.25");
}

void hud_placement_safety() {
    // Placement keys never create a HUD: frames without hud_valid / with a refused HUD / without a lens
    // submit exactly the key-off layers; a HUD is never carried to a newer frame.
    auto placed = [](Rig& g) { g.settings.hud_quad_distance_m = 3.0f; g.settings.hud_scale = 0.6f; g.settings.hud_placement_log = true; };
    Rig a; a.stop_after = 10; hud_full_rig(a); a.build(); auto ra = a.run();
    Rig b; b.stop_after = 10; hud_full_rig(b); b.settings.hud_layer = true; placed(b); b.build(); auto rb = b.run();
    Rig cr; cr.stop_after = 10; hud_full_rig(cr); cr.settings.hud_layer = true; placed(cr); cr.src.with_hud = true;
    cr.src.hud_bad = FakeSource::HudBad::null_resource; cr.build(); auto rc = cr.run();
    check(ra.success && rb.success && rc.success && per_frame_calls(a, ra.begun) == per_frame_calls(b, rb.begun) &&
        per_frame_calls(a, ra.begun) == per_frame_calls(cr, rc.begun), "placement keys: absent/invalid HUD keeps the key-off call sequence");
    check(a.xr->layers.size() == b.xr->layers.size() && a.xr->layers.size() == cr.xr->layers.size(), "same layer count");
    for (size_t i = 0; i < a.xr->layers.size(); ++i)
        check(same_layer(a.xr->layers[i], b.xr->layers[i]) && same_layer(a.xr->layers[i], cr.xr->layers[i]) && !b.xr->layers[i].hud && !cr.xr->layers[i].hud,
            "placement keys: no HUD layer without a valid HUD");
    check(b.j.prefix("xr.acquire 3") == 0 && cr.j.prefix("xr.acquire 3") == 0 && cr.out->counters().hud_invalid == rc.begun, "no HUD image; invalid counted");
    { Rig g; g.stop_after = 6; g.settings.hud_layer = true; placed(g); g.src.with_hud = true; g.src.lens[0] = std::nanf(""); g.build(); auto r = g.run();
      check(r.success && g.out->counters().hud_lens_invalid == r.begun && g.out->counters().hud_layers == 0 && g.j.prefix("xr.acquire 3") == 0,
          "placement keys: world-quad HUD without a valid lens still refused"); }
    { Rig g; g.stop_after = 10; hud_full_rig(g); g.settings.hud_layer = true; placed(g); g.src.with_hud = true; g.src.hud_every = 2; g.build(); auto r = g.run();
      check(r.success && g.xr->layers.size() == r.begun, "alternating HUD with placement keys");
      for (size_t i = 0; i < g.xr->layers.size(); ++i) check(g.xr->layers[i].hud == (i % 2 == 0), "layer carries the HUD of ITS frame only"); }
    { Rig g; g.stop_after = 20; g.publish_until = 2; hud_full_rig(g); g.settings.hud_layer = true; placed(g); g.src.with_hud = true; g.src.hud_every = 2; g.build();
      auto r = g.run();
      check(r.success && g.out->counters().hud_layers == 1 && !g.xr->layers.back().hud, "newest frame without HUD: re-submissions carry none"); }
    // Key OFF: placement values in Settings are inert (no HUD swapchain, every line and layer as before).
    Rig k; k.stop_after = 10; hud_full_rig(k); k.src.with_hud = true; k.settings.hud_quad_distance_m = 3.0f; k.settings.hud_scale = 0.6f;
    k.settings.hud_head_distance_m = 4.0f; k.build(); auto rk = k.run();
    check(rk.success && a.j.calls() == k.j.calls() && a.lines == k.lines && k.j.count("xr.create 3") == 0, "xr_hud_layer OFF: placement values inert");
}

// ------------------------------------------------------------- J-quad-yaw ---
// v' = q v q* for a unit quaternion (x, y, z, w).
void quat_apply(const float q[4], const double v[3], double out[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double t[3] = {2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])};
    out[0] = v[0] + w * t[0] + (y * t[2] - z * t[1]);
    out[1] = v[1] + w * t[1] + (z * t[0] - x * t[2]);
    out[2] = v[2] + w * t[2] + (x * t[1] - y * t[0]);
}
// Tangent-space corner/centre directions of a LOCAL quad with ANY orientation, seen from n looking
// along the quad's own -Z axis (the same quantities quad_dirs() measures for identity quads).
Dirs quad_dirs_turned(const xr_owner::Pose& p, float w, float h, const double n[3]) {
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    double X[3], Y[3], Z[3]; quat_apply(p.orientation, ex, X); quat_apply(p.orientation, ey, Y); quat_apply(p.orientation, ez, Z);
    Dirs d{};
    const double sx[5] = {-1, 1, 1, -1, 0}, sy[5] = {-1, -1, 1, 1, 0};
    for (int k = 0; k < 5; ++k) {
        double r[3];
        for (int i = 0; i < 3; ++i) r[i] = double(p.position[i]) + sx[k] * w / 2.0 * X[i] + sy[k] * h / 2.0 * Y[i] - n[i];
        const double fz = -(r[0] * Z[0] + r[1] * Z[1] + r[2] * Z[2]);
        d.t[k][0] = (r[0] * X[0] + r[1] * X[1] + r[2] * X[2]) / fz;
        d.t[k][1] = (r[0] * Y[0] + r[1] * Y[1] + r[2] * Y[2]) / fz;
    }
    return d;
}
const double kPi = 3.14159265358979323846;
const float kYawsDeg[] = {0.0f, 30.0f, -30.0f, 90.0f, -90.0f, 180.0f};

void quad_yaw_math() {
    using namespace xr_stereo;
    const float n[3] = {0.4f, 1.3f, -0.2f};
    const double nd[3] = {n[0], n[1], n[2]};
    const float sym[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    // Sign-convention proof (numeric): the producer's yaw0 = atan2(-fx, -fz) of a left-eye forward
    // f = R(0,0,-1); a rotation by yaw0 about +Y maps (0,0,-1) to (-sin yaw0, 0, -cos yaw0) = f.
    for (float deg_ : kYawsDeg) {
        const double a = deg_ * kPi / 180.0;
        const float q[4] = {0, float(std::sin(a / 2)), 0, float(std::cos(a / 2))};
        const double negz[3] = {0, 0, -1}; double f[3]; quat_apply(q, negz, f);
        const double yaw0 = std::atan2(-f[0], -f[2]);
        check(std::fabs(std::sin(yaw0) - std::sin(a)) < 1e-6 && std::fabs(std::cos(yaw0) - std::cos(a)) < 1e-6, "yaw0 = atan2(-fx,-fz) recovers the head yaw");
        check(std::fabs(f[0] + std::sin(yaw0)) < 1e-6 && std::fabs(f[2] + std::cos(yaw0)) < 1e-6 && std::fabs(f[1]) < 1e-7, "Ry(yaw0)(0,0,-1) = (-sin, 0, -cos)");
    }
    for (bool asym : {false, true}) for (float D : {40.0f, 0.8f, 2000.0f}) for (float deg_ : kYawsDeg) {
        const float* t = asym ? kAsymLens : sym;
        const float yaw = float(deg_ * kPi / 180.0);
        const std::string what = std::string(asym ? "asym" : "sym") + " D=" + std::to_string(D) + " yaw=" + std::to_string(deg_);
        const Quad base = world_quad(n, t, D), q = world_quad(n, t, D, yaw);
        check(q.width == base.width && q.height == base.height, "size unchanged by the yaw: " + what);
        check(q.pose.orientation[0] == 0.0f && q.pose.orientation[2] == 0.0f && std::fabs(q.pose.orientation[1] - std::sin(yaw / 2)) < 1e-7f &&
            std::fabs(q.pose.orientation[3] - std::cos(yaw / 2)) < 1e-7f, "orientation = (0, sin(yaw/2), 0, cos(yaw/2)): " + what);
        // Centre offset, turned back by -yaw, equals the unturned offset.
        const double ox = double(q.pose.position[0]) - nd[0], oz = double(q.pose.position[2]) - nd[2];
        const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
        const double lx = c * ox - s * oz, lz = s * ox + c * oz;
        const double tol = 2e-6 * (1.0 + D);
        check(std::fabs(lx - (double(base.pose.position[0]) - nd[0])) < tol && std::fabs(lz - (double(base.pose.position[2]) - nd[2])) < tol &&
            q.pose.position[1] == base.pose.position[1], "turned offset = Ry(yaw) * unturned offset: " + what);
        // Symmetric lens: the quad centre lies exactly along the cockpit forward from the neutral.
        if (!asym) {
            const double len = std::sqrt(ox * ox + oz * oz);
            check(std::fabs(ox / len + std::sin(double(yaw))) < 1e-5 && std::fabs(oz / len + std::cos(double(yaw))) < 1e-5 &&
                std::fabs(len - D) < tol, "centre direction = cockpit forward (-sin yaw0, 0, -cos yaw0): " + what);
        }
        // The quad faces the neutral: its +Z normal points back to n (its -Z axis is the forward).
        { const double ez[3] = {0, 0, 1}; double Z[3]; quat_apply(q.pose.orientation, ez, Z);
          check(std::fabs(Z[0] - std::sin(double(yaw))) < 1e-6 && std::fabs(Z[2] - std::cos(double(yaw))) < 1e-6 &&
              (nd[0] - q.pose.position[0]) * Z[0] + (nd[2] - q.pose.position[2]) * Z[2] > 0.99 * D, "quad faces the neutral position: " + what); }
        // Every corner subtends the same angles as the unturned quad (the lens FOV).
        check(angles_match(quad_dirs_turned(q.pose, q.width, q.height, nd), quad_dirs(base.pose, base.width, base.height, nd), 2e-6),
            "same angular extent from the neutral: " + what);
        // HUD: unset distance + scale 1 is EXACTLY the turned world quad; a HUD distance keeps the directions.
        const Quad h0 = hud_world_quad(n, t, D, 0.0f, 1.0f, yaw);
        check(same_pose(h0.pose, q.pose) && h0.width == q.width && h0.height == q.height, "HUD (unset distance) == turned world quad: " + what);
        for (float hd : {0.8f, 3.0f}) {
            const Quad h = hud_world_quad(n, t, D, hd, 1.0f, yaw);
            check(angles_match(quad_dirs_turned(h.pose, h.width, h.height, nd), quad_dirs_turned(q.pose, q.width, q.height, nd), 5e-6) &&
                std::memcmp(h.pose.orientation, q.pose.orientation, sizeof q.pose.orientation) == 0,
                "HUD at another distance: same directions, same orientation: " + what);
            const Quad hs = hud_world_quad(n, t, D, hd, 0.6f, yaw);
            check(same_pose(hs.pose, h.pose) && std::fabs(hs.width - 0.6f * h.width) < 1e-6f, "HUD scale about the centre in the turned frame: " + what);
        }
        if (deg_ == 0.0f) check(same_pose(q.pose, base.pose), "yaw 0 reproduces the identity quad exactly: " + what);
    }
    stereo::StereoFrame f; check(!neutral_ok(f), "no neutral by default");
    f.neutral_valid = true; check(neutral_ok(f), "zero neutral accepted");
    f.neutral_yaw = std::nanf(""); check(!neutral_ok(f), "NaN yaw refused");
    f.neutral_yaw = 5.0f; check(!neutral_ok(f), "|yaw| > 4 refused");
    f.neutral_yaw = 3.14159f; f.neutral_pos[1] = std::nanf(""); check(!neutral_ok(f), "NaN position refused");
    f.neutral_pos[1] = 2000.0f; check(!neutral_ok(f), "far position refused");
}

const std::string* find_line(const Rig& g, const std::string& p) {
    for (auto& l : g.lines) if (l.find(p) != std::string::npos) return &l;
    return nullptr;
}

// xr_look_drive_keys end to end: a frame carrying the rendered look turns the WORLD quad and the MARKER quad by it (they still coincide),
// the HUD quad stays on the cockpit glass (yaw0 only); a frame without it (key off) is the previous placement bit for bit.
bool markers_register(const ProjectionLayer& l);
void look_quad_runs() {
    using namespace xr_stereo;
    for (bool with_look : {false, true}) for (float lp : {0.0f, 0.35f}) {
        const float yaw0 = 0.4f, ly = 0.8f;
        Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.hud_layer = true; g.settings.hud_markers_layer = true;
        g.src.with_hud = true; g.src.with_markers = true; g.src.with_neutral = true; g.src.nyaw = yaw0;
        g.src.with_look = with_look; g.src.lyaw = ly; g.src.lpitch = lp;
        g.build(); auto r = g.run();
        const std::string what = std::string(with_look ? "look on" : "look off") + " pitch=" + std::to_string(lp);
        check(r.success && !g.out->failed() && !g.xr->layers.empty() && g.xr->violations == 0, "run succeeds: " + what);
        const Quad wantWorld = with_look ? world_quad(g.src.npos, g.src.lens, 40.0f, yaw0 + ly, lp) : world_quad(g.src.npos, g.src.lens, 40.0f, yaw0);
        const Quad wantHud = world_quad(g.src.npos, g.src.lens, 40.0f, yaw0);
        for (auto& l : g.xr->layers) {
            check(l.quad && l.markers && l.hud && same_pose(l.quad_pose, wantWorld.pose) && l.quad_width == wantWorld.width && l.quad_height == wantWorld.height,
                "world quad along the rendered look: " + what);
            check(markers_register(l), "marker quad coincides with the world quad: " + what);
            check(same_pose(l.hud_pose, wantHud.pose), "HUD quad stays turned by yaw0 only: " + what);
        }
    }
    // an implausible look is ignored (finite and inside the game's range only): the old placement
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.src.with_neutral = true; g.src.nyaw = 0.4f;
      g.src.with_look = true; g.src.lyaw = 5.0f; g.src.lpitch = 0.0f; g.build(); auto r = g.run();
      const Quad want = world_quad(g.src.npos, g.src.lens, 40.0f, 0.4f);
      check(r.success && !g.xr->layers.empty() && same_pose(g.xr->layers[0].quad_pose, want.pose), "|look yaw| > 3 rad ignored"); }
}

void quad_yaw_runs() {
    using namespace xr_stereo;
    for (float deg_ : kYawsDeg) for (float hd : {0.0f, 0.8f}) {
        const float yaw = float(deg_ * kPi / 180.0);
        Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true;
        g.src.with_neutral = true; g.src.nyaw = yaw;
        if (hd > 0.0f) { g.settings.hud_quad_distance_m = hd; g.settings.hud_scale = 1.0f; g.settings.hud_placement_log = true; }
        g.build(); auto r = g.run();
        const std::string what = "yaw=" + std::to_string(deg_) + " hud=" + std::to_string(hd);
        check(r.success && !g.out->failed() && !g.xr->layers.empty() && g.src.bad_releases == 0 && g.xr->violations == 0, "neutral run succeeds: " + what);
        const auto& c = g.out->counters();
        check(c.anchor_neutral == 1 && c.anchor_neutral_upgrades == 0 && c.anchor_rebases == 0, "anchored once from the neutral: " + what);
        const Quad want = world_quad(g.src.npos, g.src.lens, 40.0f, yaw);
        const double nd[3] = {g.src.npos[0], g.src.npos[1], g.src.npos[2]};
        for (auto& l : g.xr->layers) {
            check(l.quad && l.hud && !l.hud_view_space, "world quad + LOCAL HUD: " + what);
            check(same_pose(l.quad_pose, want.pose) && l.quad_width == want.width && l.quad_height == want.height,
                "world quad at the neutral, turned by yaw0: " + what);
            if (hd == 0.0f) check(same_pose(l.hud_pose, l.quad_pose) && l.hud_width == l.quad_width && l.hud_height == l.quad_height,
                "HUD pose == world quad (unset distance): " + what);
            else {
                check(angles_match(quad_dirs_turned(l.hud_pose, l.hud_width, l.hud_height, nd),
                                   quad_dirs_turned(l.quad_pose, l.quad_width, l.quad_height, nd), 5e-6),
                    "HUD at 0.8 m: same direction/angular size as the world quad: " + what);
                const double dx = double(l.hud_pose.position[0]) - nd[0], dz = double(l.hud_pose.position[2]) - nd[2];
                check(std::fabs(std::sqrt(dx * dx + dz * dz) - 0.8) < 1e-5, "HUD 0.8 m from the neutral along the cockpit forward: " + what);
            }
        }
        char tok[96]; std::snprintf(tok, sizeof tok, ";quad-anchor=neutral;neutral-yaw-deg=%.2f;neutral-pos=0.4000,1.3000,-0.2000", double(yaw) * 57.29577951308232);
        const std::string* a1 = find_line(g, "xr-stereo;event=quad-anchor;for=first-layer;");
        const std::string* a2 = find_line(g, "xr-stereo;event=quad-anchor;for=first-hud-layer;");
        check(a1 && a2 && *a1 == std::string("xr-stereo;event=quad-anchor;for=first-layer") + tok &&
            *a2 == std::string("xr-stereo;event=quad-anchor;for=first-hud-layer") + tok, "quad-anchor lines: " + what);
        check(g.has_line(hd == 0.0f ? "hud-pose-equals-world-quad=1" : "hud-pose-equals-world-quad=0"), "pose-equality token: " + what);
        if (hd > 0.0f) check(g.has_line(";hud-distance-m=0.800;hud-angular-deg=63.20x38.17;hud-quad-distance-m=0.80;hud-scale=1.000"),
            "combiner default: 0.8 m, full lens angle measured in the turned frame: " + what);
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "lines fit the router: " + line);
    }
}

void quad_yaw_invalid_unchanged() {
    // neutral_valid=false (or an unusable neutral): every layer and journal entry as before; only the
    // quad-anchor companion lines name the eye-midpoint anchor.
    for (int mode = 0; mode < 3; ++mode) {
        Rig a; a.stop_after = 8; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true;
        if (mode == 2) a.src.pose_valid = false;
        a.build(); auto ra = a.run();
        Rig b; b.stop_after = 8; hud_full_rig(b); b.settings.hud_layer = true; b.src.with_hud = true;
        b.src.nyaw = 0.7f;                                   // values present, flag off: inert
        if (mode == 1) { b.src.with_neutral = true; b.src.nyaw = std::nanf(""); }   // refused neutral
        if (mode == 2) { b.src.with_neutral = true; b.src.pose_valid = false; }     // no pose: no neutral
        b.build(); auto rb = b.run();
        check(ra.success && rb.success && a.j.calls() == b.j.calls() && a.xr->layers.size() == b.xr->layers.size() && a.lines == b.lines,
            "neutral invalid: journal, layers and lines identical (mode " + std::to_string(mode) + ")");
        for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], b.xr->layers[i]), "neutral invalid: identical layers");
        for (auto& l : b.xr->layers) if (l.quad) check(l.quad_pose.orientation[3] == 1.0f && l.quad_pose.orientation[1] == 0.0f, "identity orientation");
        check(b.out->counters().anchor_neutral == 0, "no neutral anchor");
        if (mode != 2) check(b.has_line("xr-stereo;event=quad-anchor;for=first-layer;quad-anchor=eye-midpoint;neutral-yaw-deg=none;neutral-pos=none;anchor-pos="),
            "quad-anchor=eye-midpoint line");
    }
    // Head-locked HUD unchanged by a neutral (VIEW space; the world quad still turns).
    Rig h; h.stop_after = 4; hud_full_rig(h); h.settings.hud_layer = true; h.settings.hud_mode = xr_stereo::HudMode::head_locked; h.src.with_hud = true;
    h.src.with_neutral = true; h.src.nyaw = 0.5f; h.build(); auto rh = h.run();
    check(rh.success && !h.xr->layers.empty(), "head-locked run");
    for (auto& l : h.xr->layers) check(l.hud_view_space && l.hud_pose.position[2] == -2.5f && l.hud_pose.orientation[3] == 1.0f &&
        l.hud_pose.orientation[1] == 0.0f && l.quad_pose.orientation[1] != 0.0f, "head-locked HUD unchanged; world quad turned");
    check(!h.has_line("for=first-hud-layer"), "no LOCAL HUD anchor line in head-locked mode");
}

void quad_yaw_generation_and_upgrade() {
    using namespace xr_stereo;
    // Space generation change: the anchor is re-captured from the NEW generation's neutral.
    { Rig g; g.stop_after = 12; g.src.with_world = true; g.src.lens_valid = true; g.src.with_neutral = true; g.src.nyaw = 0.5f; g.build();
      g.xr->hook["wait_frame"] = [&g] {
          const unsigned w = g.xr->waits;
          if (w == 3) { Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local;
              e.change_time = 1000000000 + 5 * g.xr->period; g.xr->events.push_back(e); }
          g.src.space_gen = w >= 7 ? 2 : 1;
          if (w >= 7) { g.src.nyaw = -1.2f; g.src.npos[0] = 2.0f; g.src.npos[1] = 2.7f; g.src.npos[2] = -3.0f; }
          g.src.publish();
          if (w + 1 >= 12) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      const float n1[3] = {0.4f, 1.3f, -0.2f}, n2[3] = {2.0f, 2.7f, -3.0f};
      check(r.success && c.anchor_rebases == 1 && c.anchor_neutral == 2 && c.anchor_neutral_upgrades == 0, "rebased once, both anchors from the neutral");
      check(same_pose(g.xr->layers.front().quad_pose, world_quad(n1, g.src.lens, 40.0f, 0.5f).pose) &&
          same_pose(g.xr->layers.back().quad_pose, world_quad(n2, g.src.lens, 40.0f, -1.2f).pose), "each generation uses its own neutral"); }
    // Upgrade: an eye-midpoint anchor (no neutral yet) is replaced once by the first neutral of the generation.
    { Rig g; g.stop_after = 8; g.src.with_world = true; g.src.lens_valid = true; g.src.nyaw = 0.3f; g.build();
      g.xr->hook["wait_frame"] = [&g] { g.src.with_neutral = g.xr->waits >= 3; g.src.publish(); if (g.xr->waits + 1 >= 8) g.owner->request_stop(); };
      auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.anchor_neutral_upgrades == 1 && c.anchor_neutral == 1 && c.anchor_rebases == 0, "eye-midpoint anchor upgraded once");
      check(g.xr->layers.front().quad_pose.orientation[1] == 0.0f && same_pose(g.xr->layers.back().quad_pose, world_quad(g.src.npos, g.src.lens, 40.0f, 0.3f).pose),
          "first layers identity, later layers at the turned neutral");
      check(g.has_line("xr-stereo;event=quad-anchor;for=first-layer;quad-anchor=eye-midpoint;") &&
          g.has_line("xr-stereo;event=quad-anchor;for=upgrade;quad-anchor=neutral;neutral-yaw-deg=17.19;neutral-pos=0.4000,1.3000,-0.2000"), "upgrade line"); }
}

// ------------------------------------------------- J-hud-markers (marker quad) ---
// Key xr_hud_markers_layer (default OFF): a 5th swapchain and a marker quad placed EXACTLY like the world quad
// (same anchor, neutral yaw, lens, distance; scale 1), submitted world -> markers -> cockpit projection -> HUD.
void markers_rig(Rig& g) {
    hud_full_rig(g); g.settings.hud_layer = true; g.settings.hud_markers_layer = true;
    g.src.with_hud = true; g.src.with_markers = true;
}
// The registration contract for one submitted layer set: the marker quad IS the world quad (bitwise pose, size).
bool markers_register(const ProjectionLayer& l) {
    return l.markers && l.quad && same_pose(l.marker_pose, l.quad_pose) && l.marker_width == l.quad_width && l.marker_height == l.quad_height;
}

void markers_compose_layers() {
    using namespace xr_stereo;
    for (int q = 0; q < 2; ++q) for (int m = 0; m < 2; ++m) for (int h = 0; h < 2; ++h) {
        ProjectionLayer p; p.source_alpha = true; p.quad = q; p.markers = m; p.hud = h;
        const auto l = compose_layers(p);
        std::vector<LayerKind> want;
        if (q) want.push_back(LayerKind::world_quad);
        if (m) want.push_back(LayerKind::marker_quad);
        want.push_back(LayerKind::projection);
        if (h) want.push_back(LayerKind::hud_quad);
        const std::string what = "q" + std::to_string(q) + " m" + std::to_string(m) + " h" + std::to_string(h);
        check(l.count == want.size(), "layer count: " + what);
        for (unsigned i = 0; i < l.count && i < want.size(); ++i) check(l.e[i].kind == want[i], "layer order world, markers, projection, hud: " + what);
        for (unsigned i = 0; i < l.count; ++i) if (l.e[i].kind == LayerKind::marker_quad)
            check(l.e[i].target == kMarkers && l.e[i].source_alpha && !l.e[i].unpremultiplied && l.e[i].space == LayerSpace::local,
                "marker quad: own swapchain, SOURCE_ALPHA premultiplied (never UNPREMULTIPLIED), LOCAL: " + what);
    }
    // Key off: the lists are exactly the pre-existing ones (markers false).
    ProjectionLayer p; p.quad = true; p.hud = true; p.source_alpha = true;
    const auto l = compose_layers(p);
    check(l.count == 3 && l.e[0].kind == LayerKind::world_quad && l.e[1].kind == LayerKind::projection && l.e[2].kind == LayerKind::hud_quad,
        "markers false: unchanged world -> projection -> hud");
    check(kTargets == 5 && kMarkers == 4 && kHud == 3 && kQuad == 2, "target indices: markers appended after the HUD");
}

void markers_config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("log_enabled=true\n");
    check(d.issue == ConfigIssue::none && !d.config.xr_hud_markers_layer() && !d.config.gpu_copy_step5_hud_markers(), "marker keys default OFF");
    auto on = parse_config("gpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=true\nxr_hud_layer=true\nxr_hud_markers_layer=1\n");
    check(on.issue == ConfigIssue::none && on.config.xr_hud_markers_layer() && on.config.gpu_copy_step5_hud_markers(), "marker keys parse ON");
    check(!parse_config("xr_hud_markers_layer=0\n").config.xr_hud_markers_layer() && !parse_config("gpu_copy_step5_hud_markers=false\n").config.gpu_copy_step5_hud_markers(),
        "marker keys parse OFF");
    check(parse_config("xr_hud_markers_layer=yes\n").issue == ConfigIssue::out_of_range, "xr_hud_markers_layer malformed rejected");
    check(parse_config("gpu_copy_step5_hud_markers=2\n").issue == ConfigIssue::out_of_range, "gpu_copy_step5_hud_markers malformed rejected");
    check(!parse_config("xr_hud_markers_layer=true\nxr_hud_markers_layer=true\n").config.xr_hud_markers_layer(), "duplicate refuses");
    check(!parse_config("gpu_copy_step5_hud_markers=true\ngpu_copy_step5_hud_markers=true\n").config.gpu_copy_step5_hud_markers(), "duplicate refuses (producer key)");
    // The complete headset key set (tools/live/vr-test-start.ps1 + the two marker keys, CRLF as Set-Content writes it)
    // stays inside the 24-key / 2048-byte limits.
    const char* keys[] = {"log_enabled=true", "diagnostic_ownership_pins=true", "gpu_copy_witness_observation=true", "gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true", "gpu_copy_step3_whole_set=true", "gpu_copy_step5_continuous=true", "xr_stereo_output=true", "xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true", "gpu_copy_step5_hmd_fov=true", "gpu_copy_step5_hud=true", "xr_hud_layer=true", "xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5", "xr_world_quad_distance_m=500", "gpu_copy_step5_units_per_metre=87", "gpu_copy_step5_hud_markers=true", "xr_hud_markers_layer=true",
        "xr_world_in_projection=true", "gpu_copy_step5_world_objects=true", "gpu_copy_step5_world_observe=true"};
    std::string cfg; for (auto* k : keys) { cfg += k; cfg += "\r\n"; }
    const auto all = parse_config(cfg);
    check(std::size(keys) == 22 && std::size(keys) <= max_config_keys && cfg.size() <= max_config_bytes && cfg.size() < 850, "full headset key set (+ world_in_projection, world_objects, world_observe, marker keys) within the limits (22 keys, < 850 bytes of 2048)");
    check(all.issue == ConfigIssue::none && all.config.xr_hud_markers_layer() && all.config.gpu_copy_step5_hud_markers() && all.config.xr_hud_layer() &&
        all.config.gpu_copy_step5_hud() && all.config.xr_stereo_output() && all.config.xr_world_quad_distance_m() == 500.0f && all.config.xr_hud_scale() == 0.5f,
        "headset key set parses with the marker keys");
    // The 24-key cap, not the byte cap, is the binding limit: png_once fits as the 20th key.
    check(parse_config(cfg + "gpu_copy_step5_png_once=true\r\n").issue == ConfigIssue::none, "one more key (png_once) still fits");
    check(!xr_stereo::Settings{}.hud_markers_layer, "Settings default OFF");
}

void markers_four_layers_world_quad() {
    Rig g; g.stop_after = 8; markers_rig(g); g.build();
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && g.src.bad_releases == 0 && g.xr->violations == 0 && !g.out->failed(), "marker frames succeed, every source restored");
    check(g.j.count("xr.create 4") == 1 && g.j.count("d3d.bind 4") == 1 && g.j.count("xr.destroy 4") == 1, "marker swapchain created, bound, retired once");
    const long c3 = g.j.index("xr.create 3"), b3 = g.j.index("d3d.bind 3"), c4 = g.j.index("xr.create 4"), b4 = g.j.index("d3d.bind 4"), begin = g.j.index("begin_session");
    check(c3 > 0 && b3 > c3 && c4 > b3 && b4 > c4 && b4 < begin, "marker swapchain created after the HUD swapchain, before BeginSession");
    check(g.out->extent(4).w == g.out->extent(2).w && g.out->extent(4).h == g.out->extent(2).h && g.blit->bound_w[4] == 2560 && g.blit->bound_h[4] == 1440,
        "marker swapchain extent = world quad extent policy");
    const long dd = g.j.index("d3d.destroy"), d4 = g.j.index("xr.destroy 4");
    check(d4 > 0 && d4 < dd && dd < g.j.index("request_exit"), "marker swapchain retired before private objects and the exit handshake");
    const auto s = frame_slice(g.j.calls(), 0);
    const std::vector<std::string> expected{"wait_frame", "begin_frame", "locate_views", "locate_head", "source.acquire 1",
        "d3d.presync pending", "xr.acquire 0", "xr.wait 0 0", "xr.acquire 1", "xr.wait 1 0", "xr.acquire 2", "xr.wait 2 0",
        "xr.acquire 3", "xr.wait 3 0", "xr.acquire 4", "xr.wait 4 0", "d3d.record 0,0,q0,h0,m0", "d3d.close", "d3d.gpu_wait", "d3d.execute",
        "d3d.signal 1", "d3d.sample 1", "xr.release 0", "xr.release 1", "xr.release 2", "xr.release 3", "xr.release 4", "source.release 1", "end_frame_projection"};
    check(s == expected, "exact order: marker image acquired/waited, ONE recording/Close/Execute/Signal, released after observed completion");
    check(g.j.prefix("d3d.execute") == r.begun && g.j.prefix("d3d.signal") == r.begun && g.j.prefix("d3d.record") == r.begun,
        "the marker blit joins the frame's one list and one fence");
    const auto& job = g.blit->jobs[0];
    check(job.use_markers && job.markers.resource == FakeSource::res(0, 6) && job.markers.format == 10 && job.markers.width == 2560 && job.markers.height == 1440 &&
        job.map[4].ok && job.map[4].dst.w == 2560 && job.use_hud && job.hud.resource == FakeSource::res(0, 5) && job.hud.resource != job.markers.resource,
        "marker job: its own source image (not the HUD's), source-sized, full-rect map");
    check(g.xr->layers.size() == r.begun, "one layer set per frame");
    for (auto& l : g.xr->layers) {
        check(markers_register(l) && l.hud && !l.hud_view_space, "marker quad + world quad + HUD on every layer; marker pose/size == world quad");
        const auto list = xr_stereo::compose_layers(l);
        check(list.count == 4 && list.e[0].kind == xr_stereo::LayerKind::world_quad && list.e[1].kind == xr_stereo::LayerKind::marker_quad &&
            list.e[2].kind == xr_stereo::LayerKind::projection && list.e[3].kind == xr_stereo::LayerKind::hud_quad,
            "submitted list: world -> markers -> cockpit projection -> instrument HUD");
    }
    check(c.marker_frames == c.new_frames && c.marker_frames == r.begun && c.marker_layers == c.submitted && c.marker_skipped == 0 &&
        c.marker_invalid == 0 && c.marker_lens_invalid == 0 && c.hud_frames == r.begun, "marker counters");
    check(g.has_line("xr-stereo;event=markers-swapchain;key=xr_hud_markers_layer;markers=2560x1440,images=3;format=29;policy=same-as-world-quad;flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);"
        "order=quad-then-markers-then-projection-then-hud;placement=world-quad-pose-and-size;scale=1;distance-m=40.0"), "markers-swapchain line");
    check(g.has_line("xr-stereo;event=first-marker-frame;frame=1;frame_id=1;slot=0;marker-fmt=10;src=2560x1440;marker-image=0;producer-marker-draws=3;producer-marker-refused=0;world=quad;hud=layer;draws=5"),
        "first-marker-frame line");
    check(g.has_line("xr-stereo;event=first-marker-layer;") && g.has_line("layers=4;order=quad-then-markers-then-projection-then-hud;marker-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);marker-space=LOCAL;") &&
        g.has_line("marker-size=49.216x27.680;marker-distance-m=40.000;marker-angular-deg=63.20x38.17;marker-scale=1;marker-pose-equals-world-quad=1"), "first-marker-layer line");
    check(g.has_line("layers=3;order=quad-then-markers-then-projection;") , "first-layer line names the markers");
    check(g.has_line("layers=4;order=quad-then-markers-then-projection-then-hud;hud-flags="), "first-hud-layer line names the markers");
    const std::string n = std::to_string(r.begun);
    check(g.has_line(";marker-frames=" + n + ";marker-layers=" + n + ";marker-skipped=0;marker-invalid=0;marker-lens-invalid=0;marker-swapchain=2560x1440;producer-marker-draws=3;producer-marker-refused=0;no-headset-claim"),
        "summary-markers line");
    check(g.has_line("xr-stereo;event=markers-result;key=xr_hud_markers_layer;marker-frames=" + n + ";marker-layers=" + n + ";marker-skipped=0") && g.has_line(";hud=layer;no-headset-claim"),
        "markers-result line and result token");
    for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "marker lines fit the router: " + line);
}

// The marker quad registers with the world quad for every lens, distance and anchor (and a mutation must fail).
void markers_pose_matches_world_quad() {
    for (bool asym : {false, true}) for (float D : {5.0f, 40.0f, 500.0f, 2000.0f}) for (bool neutral : {false, true}) {
        Rig g; g.stop_after = 6; markers_rig(g);
        if (asym) std::memcpy(g.src.lens, kAsymLens, sizeof g.src.lens);
        g.settings.quad_distance_m = D; g.settings.comfort_log = true;
        // Keys that move the instrument HUD must not move the markers.
        g.settings.hud_quad_distance_m = 0.8f; g.settings.hud_scale = 0.5f; g.settings.hud_placement_log = true;
        if (neutral) { g.src.with_neutral = true; g.src.nyaw = 0.35f; }
        g.build(); auto r = g.run();
        const std::string what = std::string(asym ? "asym" : "sym") + " D=" + std::to_string(D) + (neutral ? " neutral" : " eye-midpoint");
        check(r.success && !g.out->failed() && !g.xr->layers.empty() && g.src.bad_releases == 0 && g.xr->violations == 0, "marker pose run succeeds: " + what);
        for (auto& l : g.xr->layers) {
            check(markers_register(l), "marker pose and size equal the world quad's bitwise: " + what);
            // Independent of the quad the consumer built: recompute from the producer facts.
            double n[3]; anchor_of(l, g.src.lens, D, n);
            const Dirs w = quad_dirs(l.quad_pose, l.quad_width, l.quad_height, n);
            const Dirs m = quad_dirs(l.marker_pose, l.marker_width, l.marker_height, n);
            if (!neutral) check(angles_match(m, w, 1e-9), "every corner at the world quad's angle: " + what);
            check(std::fabs(l.marker_width - D * (g.src.lens[0] + g.src.lens[1])) < 1e-3f * D &&
                std::fabs(l.marker_height - D * (g.src.lens[2] + g.src.lens[3])) < 1e-3f * D, "marker size subtends the lens at distance D: " + what);
            // Negative control: a marker quad built the HUD way (HUD distance and scale) is NOT registered; the check must see it.
            ProjectionLayer bad = l;
            const xr_stereo::Quad hq = neutral ? xr_stereo::hud_world_quad(g.src.npos, g.src.lens, D, 0.8f, 0.5f, g.src.nyaw)
                                               : xr_stereo::hud_world_quad(g.xr->layers[0].views[0].pose.position, g.src.lens, D, 0.8f, 0.5f);
            bad.marker_pose = hq.pose; bad.marker_width = hq.width; bad.marker_height = hq.height;
            check(!markers_register(bad), "mutation (marker quad at the HUD distance/scale) is detected: " + what);
            check(!same_pose(l.hud_pose, l.marker_pose) && l.hud_width != l.marker_width, "the instrument HUD stays at its own distance/scale: " + what);
        }
        check(g.has_line("marker-pose-equals-world-quad=1") && g.has_line("marker-distance-m=" + [&] { char b[32]; std::snprintf(b, sizeof b, "%.3f", double(D)); return std::string(b); }()),
            "first-marker-layer reports the world distance: " + what);
    }
    // Typical headset-test configuration: world 500 m, HUD 0.8 m scale 0.5.
    Rig g; g.stop_after = 4; markers_rig(g); g.settings.quad_distance_m = 500.0f; g.settings.hud_quad_distance_m = 0.8f; g.settings.hud_scale = 0.5f; g.build(); auto r = g.run();
    const auto& l = g.xr->layers.at(0);
    check(r.success && markers_register(l) && std::fabs(l.marker_pose.position[2] - (0.1f - 500.0f)) < 1e-3f && std::fabs(l.hud_pose.position[2] - (0.1f - 0.8f)) < 1e-4f &&
        l.hud_width < l.marker_width, "500 m markers, 0.8 m x 0.5 instruments");
}

void markers_absent_and_refusals() {
    // Frames without hud_markers_valid: no marker acquire/blit/layer, the rest is the HUD path unchanged; never an older marker image.
    { Rig g; g.stop_after = 10; markers_rig(g); g.src.markers_every = 2; g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && g.xr->layers.size() == r.begun, "alternating marker frames succeed");
      for (size_t i = 0; i < g.xr->layers.size(); ++i) check(g.xr->layers[i].markers == (i % 2 == 0) && g.xr->layers[i].hud, "layer carries the markers of ITS frame only; HUD always");
      check(g.j.prefix("xr.acquire 4") == (r.begun + 1) / 2 && c.marker_frames == (r.begun + 1) / 2 && c.marker_skipped == r.begun / 2 && c.marker_layers == c.marker_frames,
          "markers acquired/blitted/counted only for marker frames"); }
    // Re-submission repeats the WHOLE last layer set; if the newest frame had no markers the re-submissions carry none.
    { Rig g; g.stop_after = 20; g.publish_until = 2; markers_rig(g); g.src.markers_every = 2; g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.new_frames == 2 && c.marker_frames == 1 && c.marker_layers == 1 && !g.xr->layers.back().markers,
          "the newest frame had no markers: re-submissions carry none (never the previous markers)"); }
    { Rig g; g.stop_after = 20; g.publish_until = 2; markers_rig(g); g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.new_frames == 2 && c.reused == r.begun - 2 && c.marker_layers == c.submitted && c.marker_frames == 2,
          "re-submitted layers keep their own frame's markers (no new acquire)"); }
    struct Case { const char* what; std::function<void(Rig&)> set; bool acquires; };
    const std::vector<Case> cases{
        {"marker format other than FP16/FP32 refused", [](Rig& g) { g.src.hud_format = 28; }, false},
        {"markers aliasing an eye refused", [](Rig& g) { g.src.marker_bad = FakeSource::MarkerBad::alias_eye; }, false},
        {"markers aliasing the HUD refused", [](Rig& g) { g.src.marker_bad = FakeSource::MarkerBad::alias_hud; }, false},
        {"markers aliasing the world snapshot refused", [](Rig& g) { g.src.marker_bad = FakeSource::MarkerBad::alias_world; }, false},
        {"null marker resource refused", [](Rig& g) { g.src.marker_bad = FakeSource::MarkerBad::null_resource; }, false},
        {"backend cannot read the markers: cleared, not drawn", [](Rig& g) { g.blit->markers_unreadable = true; }, true},
    };
    for (auto& k : cases) {
        Rig g; g.stop_after = 6; markers_rig(g); k.set(g); g.build(); auto r = g.run();
        const auto& c = g.out->counters();
        check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, std::string(k.what) + ": the frame itself proceeds");
        check(g.j.prefix("xr.acquire 4") == (k.acquires ? r.begun : 0u), std::string(k.what) + ": acquisition as expected");
        for (auto& l : g.xr->layers) check(!l.markers && l.quad, std::string(k.what) + ": no marker layer, world quad kept");
        check(c.marker_invalid == r.begun && c.marker_skipped == r.begun && c.marker_frames == 0 && c.marker_layers == 0, std::string(k.what) + ": counted marker-invalid");
        if (std::string(k.what).find("format") == std::string::npos) check(c.hud_frames == r.begun && c.hud_layers == r.begun, std::string(k.what) + ": HUD unaffected");
    }
    // World-quad-registered markers need a valid lens: refused and counted, the frame still goes out (HUD refused alike).
    { Rig g; g.stop_after = 6; markers_rig(g); g.src.lens_valid = false; g.src.with_world = false; g.src.lens[0] = std::nanf(""); g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.marker_lens_invalid == r.begun && c.marker_skipped == r.begun && c.marker_layers == 0 && g.j.prefix("xr.acquire 4") == 0 &&
          g.has_line("marker-lens-invalid=" + std::to_string(r.begun)), "markers without a valid lens: refused, counted marker-lens-invalid"); }
    // A producer HUD with refused instruments does not cost the markers (independent layers).
    { Rig g; g.stop_after = 4; markers_rig(g); g.src.hud_bad = FakeSource::HudBad::null_resource; g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.hud_invalid == r.begun && c.marker_frames == r.begun && c.marker_layers == c.submitted, "refused instrument HUD: markers still submitted");
      for (auto& l : g.xr->layers) check(l.markers && !l.hud && markers_register(l), "layer: markers without the HUD"); }
    // Markers without a world quad this frame: still a marker quad (computed from the lens), no pose equality token claim.
    { Rig g; g.stop_after = 4; markers_rig(g); g.settings.world_quad = false; g.src.with_world = false; g.build(); auto r = g.run();
      check(r.success && !g.xr->layers.empty() && g.xr->layers[0].markers && !g.xr->layers[0].quad && g.has_line("marker-pose-equals-world-quad=no-world-quad-this-frame"),
          "no world quad: marker quad still submitted, no equality claimed"); }
}

void markers_key_off_unchanged() {
    // Key OFF ignores producer marker fields completely: journal, lines and layers identical to a run without them.
    { Rig a; a.stop_after = 10; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true; a.build(); auto ra = a.run();
      Rig d; d.stop_after = 10; hud_full_rig(d); d.settings.hud_layer = true; d.src.with_hud = true; d.src.with_markers = true; d.build(); auto rd = d.run();
      check(ra.success && rd.success, "key-off runs succeed");
      check(a.j.calls() == d.j.calls(), "key OFF: the complete call journal is identical with or without producer marker fields");
      check(a.lines == d.lines, "key OFF: every evidence line identical");
      check(a.xr->layers.size() == d.xr->layers.size(), "same layer count");
      for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], d.xr->layers[i]) && !d.xr->layers[i].markers, "key OFF: no marker layer");
      check(d.j.count("xr.create 4") == 0 && d.j.prefix("xr.acquire 4") == 0 && d.j.count("d3d.bind 4") == 0 && !d.has_line("marker") && !d.has_line("markers"),
          "key OFF: no marker swapchain, no marker lines");
      check(!d.blit->jobs.empty() && !d.blit->jobs[0].use_markers, "key OFF: no markers in the recording");
      const auto& c = d.out->counters();
      check(c.marker_frames == 0 && c.marker_layers == 0 && c.marker_skipped == 0 && c.marker_invalid == 0 && c.marker_lens_invalid == 0, "key OFF: marker counters never move"); }
    // Key ON but no frame carries markers: every existing line, the call sequence and the layers equal the HUD-only run (marker-only lines removed).
    { Rig a; a.stop_after = 10; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true; a.build(); auto ra = a.run();
      Rig b; b.stop_after = 10; hud_full_rig(b); b.settings.hud_layer = true; b.settings.hud_markers_layer = true; b.src.with_hud = true; b.build(); auto rb = b.run();
      check(ra.success && rb.success && ra.begun == rb.begun, "key ON without producer markers runs");
      check(per_frame_calls(a, ra.begun) == per_frame_calls(b, rb.begun), "no marker acquire/blit when no frame has markers");
      for (size_t i = 0; i < a.xr->layers.size(); ++i) check(same_layer(a.xr->layers[i], b.xr->layers[i]), "layers identical to the HUD-only path");
      std::vector<std::string> mapped;
      for (std::string l : b.lines) {
          if (l.find("event=markers-") != std::string::npos || l.find("event=summary-markers") != std::string::npos) continue;
          const size_t p = l.find(";markers=layer");
          if (p != std::string::npos) l.erase(p, std::strlen(";markers=layer"));
          mapped.push_back(l);
      }
      check(mapped == a.lines, "key ON without markers: every existing line unchanged apart from the marker-only lines/token"); }
}

void markers_requires_hud_world_quad() {
    // The key needs xr_hud_layer in world-quad mode; otherwise the consumer ignores it (no swapchain, no layer, lines as without it).
    { Rig a; a.stop_after = 6; hud_full_rig(a); a.src.with_hud = true; a.src.with_markers = true; a.build(); auto ra = a.run();
      Rig b; b.stop_after = 6; hud_full_rig(b); b.settings.hud_markers_layer = true; b.src.with_hud = true; b.src.with_markers = true; b.build(); auto rb = b.run();
      check(ra.success && rb.success && a.j.calls() == b.j.calls() && a.lines == b.lines, "markers key without the HUD layer: ignored, identical journal and lines");
      check(b.j.count("xr.create 4") == 0, "markers key without the HUD layer: no marker swapchain"); }
    { Rig b; b.stop_after = 6; markers_rig(b); b.settings.hud_mode = xr_stereo::HudMode::head_locked; b.build(); auto rb = b.run();
      check(rb.success && b.j.count("xr.create 4") == 0 && b.out->counters().marker_layers == 0 && !b.has_line("event=markers-"), "head-locked HUD: markers ignored (no world alignment there)");
      for (auto& l : b.xr->layers) check(!l.markers && l.hud && l.hud_view_space, "head-locked layers unchanged"); }
}

void markers_failures() {
    { // The marker swapchain cannot be created: the consumer disables cleanly before the session starts, nothing submitted.
      Rig g; g.stop_after = 4; markers_rig(g); g.build(); g.xr->script["xr.create 4"] = {Rc::error};
      auto r = g.run();
      check(g.out->failed() && g.out->failure() == "xrCreateSwapchain failed" && g.xr->layers.empty() && g.j.prefix("d3d.execute") == 0, "marker swapchain creation refusal: disabled, nothing submitted");
      check(r.clean && g.j.count("xr.destroy 4") == 0 && g.j.count("xr.destroy 3") == 1 && g.j.count("xr.destroy 0") == 1 && g.src.bad_releases == 0,
          "...and retired cleanly (only the swapchains that exist are destroyed)"); }
    { // Marker image wait times out for good: acquisition outstanding -> retained, nothing destroyed.
      Rig g; g.stop_after = 30; markers_rig(g); g.build();
      g.xr->hook["xr.acquire 4"] = [&g] { if (g.xr->waits == 3) g.xr->wait_timeouts[4] = {true, true, true}; };
      auto r = g.run();
      check(g.out->failed() && g.out->failure().find("timeout; acquisition outstanding") != std::string::npos && !r.success, "marker wait timeout disables");
      check(!r.clean && !r.consumer_retired && r.session_held && g.j.count("xr.destroy 4") == 0 && g.j.count("xr.destroy 0") == 0 && g.j.count("destroy_session") == 0,
          "unwaited marker acquisition: swapchains/session retained");
      check(g.src.bad_releases == 0 && g.j.prefix("d3d.execute") == 2, "waited images and the slot released; nothing submitted for the failed frame"); }
    for (bool late : {false, true}) { // GPU completion pending with the markers in the recording
      Rig g; g.stop_after = 30; markers_rig(g); g.build();
      g.xr->hook["xr.wait 4 0"] = [&g] { if (g.xr->waits == 2) g.blit->never_complete = true; };
      if (late) g.xr->hook["end_frame"] = [&g] { g.blit->never_complete = false; };
      auto r = g.run();
      check(g.out->failed() && g.out->failure() == "GPU completion pending; quarantine", "marker frame quarantined on completion expiry");
      check(g.has_line(";targets=5;owner-t-ms="), "completion-pending counts the marker target");
      if (late) check(r.clean && r.consumer_retired && g.src.bad_releases == 0 && g.j.count("xr.release 4") == 2 && g.j.count("xr.destroy 4") == 1, "late completion: marker image released, then retired");
      else check(!r.clean && !r.consumer_retired && g.j.count("xr.release 4") == 1 && g.j.count("xr.destroy 4") == 0 && g.j.count("d3d.destroy") == 0, "quarantine: marker image, swapchains, private objects retained");
    }
}

// xr_world_in_projection ON: no world quad layer exists; the marker quad is an overlay ABOVE the projection, placed exactly
// at the world plane the in-projection world uses (bitwise the pose/size the world quad has with the key off).
void markers_world_in_projection() {
    for (bool asym : {false, true}) for (bool neutral : {false, true}) for (float D : {40.0f, 500.0f}) {
        Rig off; off.stop_after = 6; markers_rig(off); off.settings.quad_distance_m = D; off.settings.comfort_log = true;
        Rig on; on.stop_after = 6; markers_rig(on); on.settings.quad_distance_m = D; on.settings.comfort_log = true; on.settings.world_in_projection = true;
        for (Rig* g : {&off, &on}) {
            if (asym) std::memcpy(g->src.lens, kAsymLens, sizeof g->src.lens);
            if (neutral) { g->src.with_neutral = true; g->src.nyaw = 0.35f; }
            g->settings.hud_quad_distance_m = 0.8f; g->settings.hud_scale = 0.5f;
            g->build();
        }
        auto r0 = off.run(); auto r1 = on.run();
        const std::string what = std::string(asym ? "asym" : "sym") + " D=" + std::to_string(D) + (neutral ? " neutral" : " eye-midpoint");
        check(r0.success && r1.success && !on.out->failed() && on.src.bad_releases == 0 && on.xr->violations == 0 && !on.xr->layers.empty(), "composite+markers run succeeds: " + what);
        check(on.j.count("xr.create 2") == 0 && on.j.count("xr.create 4") == 1 && on.j.prefix("xr.acquire 2") == 0 && on.j.prefix("xr.acquire 4") == on.xr->layers.size(),
            "no world quad swapchain, marker swapchain used: " + what);
        for (size_t i = 0; i < on.xr->layers.size() && i < off.xr->layers.size(); ++i) {
            const auto& a = off.xr->layers[i]; const auto& b = on.xr->layers[i];
            check(b.markers && b.world_composite && !b.quad && b.hud, "composite layer: markers + HUD, no world quad: " + what);
            check(a.markers && a.quad && same_pose(a.marker_pose, b.marker_pose) && a.marker_width == b.marker_width && a.marker_height == b.marker_height,
                "marker quad at the world-quad pose/size bitwise, with and without the in-projection world: " + what);
            const auto list = xr_stereo::compose_layers(b);
            check(list.count == 3 && list.e[0].kind == xr_stereo::LayerKind::projection && list.e[1].kind == xr_stereo::LayerKind::marker_quad &&
                list.e[2].kind == xr_stereo::LayerKind::hud_quad && list.e[1].source_alpha && !list.e[1].unpremultiplied,
                "overlay order: projection -> markers -> HUD (markers above the cockpit, accepted): " + what);
        }
        check(on.has_line("order=projection-then-markers-then-hud;marker-flags=") && on.has_line("marker-pose-equals-world-quad=world-composite-plane") &&
            on.has_line("world-composite=projection;layers=3;order=projection-then-markers-then-hud;"), "composite marker lines: " + what);
        for (auto& line : on.lines) check(line.size() < feature_log::line_cap, "composite marker lines fit the router: " + line);
    }
}


// ------------------------------------------------- J-effects-layer (effects image inside the world-in-projection pass) ---
void effects_math() {
    using namespace xr_stereo;
    // CPU mirror of shader mode 3: e = HUD-mode shade of the effects texel (alpha = max(a, max rgb)), layer = e + (1 - e.a) * world, cockpit over that.
    const Shade world{0.2f, 0.2f, 0.2f, 1.0f};
    const float half[4] = {0.5f, 0.0f, 0.0f, 0.5f};
    auto o = effects_over_world(half, Tonemap::clamp, 1.0f, 1.0f, world);
    check(std::fabs(o.r - 0.6f) < 1e-6f && std::fabs(o.g - 0.1f) < 1e-6f && std::fabs(o.b - 0.1f) < 1e-6f && std::fabs(o.a - 1.0f) < 1e-6f, "effects over world: e + (1-a) world");
    const float none[4] = {0, 0, 0, 0};
    o = effects_over_world(none, Tonemap::clamp, 1.0f, 1.0f, world);
    check(o.r == world.r && o.g == world.g && o.b == world.b && o.a == world.a, "a transparent effects texel leaves the world bit for bit");
    const float zeroA[4] = {0.3f, 0.6f, 0.0f, 0.0f}; // alpha 0 but lit: alpha = max(rgb) (HUD rule), c <= a
    o = effects_over_world(zeroA, Tonemap::clamp, 1.0f, 1.0f, world);
    check(std::fabs(o.g - (0.6f + 0.4f * 0.2f)) < 1e-6f && std::fabs(o.r - (0.3f + 0.4f * 0.2f)) < 1e-6f && std::fabs(o.a - 1.0f) < 1e-6f, "effects alpha rule max(a, max(rgb)): lit texel with alpha 0 still covers");
    const float full[4] = {2.0f, 0.5f, 0.0f, 1.0f};
    o = effects_over_world(full, Tonemap::clamp, 1.0f, 1.0f, world); // clamp tonemap: c = (1, .5, 0), a = 1: the world is hidden
    check(o.r == 1.0f && o.g == 0.5f && o.b == 0.0f && o.a == 1.0f, "opaque effects texel hides the world (clamp tonemap)");
    o = effects_over_world(full, Tonemap::reinhard, 1.0f, 1.0f, world);
    check(std::fabs(o.r - 2.0f / 3.0f) < 1e-6f && std::fabs(o.a - 1.0f) < 1e-6f, "reinhard tonemap acts on the effects like the world");
    o = effects_over_world(half, Tonemap::clamp, 1.0f, 0.0f, world); // ramp 0 (outside the window border): effects fully faded, world is the (already ramped) sample
    check(o.r == world.r && o.a == world.a, "edge ramp 0 removes the effects");
    // Cockpit over (effects over world): an opaque cockpit pixel hides both; a transparent one shows the effects layer; alpha 0.5 mixes.
    const Shade layer = effects_over_world(half, Tonemap::clamp, 1.0f, 1.0f, world);
    const Shade cockpitOpaque{0.1f, 0.2f, 0.3f, 1.0f}, cockpitClear{0, 0, 0, 0}, cockpitHalf{0.05f, 0.05f, 0.05f, 0.5f};
    auto c = composite_over_world(cockpitOpaque, layer);
    check(c.r == 0.1f && c.g == 0.2f && c.b == 0.3f && c.a == 1.0f, "effects are BEHIND an opaque cockpit pixel");
    c = composite_over_world(cockpitClear, layer);
    check(c.r == layer.r && c.g == layer.g && c.b == layer.b, "a transparent cockpit pixel shows the effects layer");
    c = composite_over_world(cockpitHalf, layer);
    check(std::fabs(c.r - (0.05f + 0.5f * layer.r)) < 1e-6f, "half-covered cockpit pixel: cockpit + (1-a) (effects over world)");
    // Layout: no new root constant (mode 3 reuses the existing layout), the world composite constants are unchanged.
    check(kBlitConstants == 24 && kBlitConstantsLegacy == 22 && sizeof(WorldComposite) == 68, "effects add no root constant; WorldComposite layout unchanged");
    // The effects pixel maps through the SAME ray/plane mapping as the world: the world uv of a pixel is world_ray_uv, nothing else.
    xr_owner::EyeSample e{}; e.pose.orientation[3] = 1.0f; e.pose.position[1] = 1.5f; e.fov = {-0.7f, 0.7f, 0.5f, -0.5f};
    const float anchor[3] = {0.0f, 1.5f, 0.0f}, lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    WorldComposite wc{};
    check(world_composite_params(e, anchor, 0.0f, lens, 40.0f, wc), "composite params for the mapping check");
    const WorldHit centre = world_ray_uv(wc, 0.5f, 0.5f);
    check(centre.hit && std::fabs(centre.u - 0.5f) < 1e-3f && std::fabs(centre.v - 0.5f) < 1e-3f, "view-centre ray hits the world window centre (the uv the effects texel is sampled at)");
}
void effects_config_keys() {
    using namespace acz::diagnostics;
    check(!acz::diagnostics::Config{}.gpu_copy_step5_effects() && !acz::diagnostics::Config{}.xr_effects_layer() && !parse_config("").config.gpu_copy_step5_effects(), "effects keys default OFF");
    auto on = parse_config("gpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\nxr_stereo_output=true\nxr_world_in_projection=true\nxr_effects_layer=1\n");
    check(on.issue == ConfigIssue::none && on.config.gpu_copy_step5_effects() && on.config.xr_effects_layer(), "effects keys parse ON");
    check(!parse_config("gpu_copy_step5_effects=0\n").config.gpu_copy_step5_effects() && !parse_config("xr_effects_layer=false\n").config.xr_effects_layer(), "effects keys parse OFF");
    check(parse_config("gpu_copy_step5_effects=yes\n").issue == ConfigIssue::out_of_range && parse_config("xr_effects_layer=2\n").issue == ConfigIssue::out_of_range, "malformed rejected");
    check(!parse_config("xr_effects_layer=true\nxr_effects_layer=true\n").config.xr_effects_layer(), "duplicate refuses");
    check(!xr_stereo::Settings{}.effects_layer, "Settings default OFF");
}
void effects_rig(Rig& g) {
    hud_full_rig(g); g.settings.hud_layer = true; g.settings.world_in_projection = true; g.settings.effects_layer = true; g.src.with_effects = true; g.src.with_hud = true;
    g.settings.quad_distance_m = 500.0f; g.src.with_neutral = true;
}
void test8_diag() {
    using namespace xr_stereo;
    // CPU mirror of the HLSL debug block and the mode packing.
    const float col[3] = {0.2f, 0.4f, 0.6f}, none[3] = {0, 0, 0}, wd[4] = {0.1f, 0.2f, 0.3f, 1.0f}, nw[4] = {0, 0, 0, 0};
    float o[3];
    debug_view_rgb(1, 1.0f, col, wd, o); check(o[0] == 1 && o[1] == 1 && o[2] == 1, "view 1: covered = white");
    debug_view_rgb(1, 0.0f, col, wd, o); check(o[0] == 0 && o[1] == 0 && o[2] == 0, "view 1: uncovered = black");
    debug_view_rgb(2, 0.0f, col, wd, o); check(o[0] == 0.2f && o[1] == 0.4f && o[2] == 0.6f, "view 2: cockpit colour, alpha ignored");
    debug_view_rgb(3, 1.0f, col, wd, o); check(o[0] == 0.1f && o[1] == 0.2f && o[2] == 0.3f, "view 3: the world layer");
    debug_view_rgb(4, 1.0f, col, wd, o); check(o[0] == 1 && o[1] == 1 && o[2] == 0, "view 4: covered + world = red+green");
    debug_view_rgb(4, 0.0f, col, nw, o); check(o[0] == 0 && o[1] == 0 && o[2] == 1, "view 4: colour discarded by the depth rule = blue");
    debug_view_rgb(4, 0.0f, none, nw, o); check(o[0] == 0 && o[1] == 0 && o[2] == 0, "view 4: nothing = black");
    debug_view_rgb(0, 1.0f, col, wd, o); check(o[0] == 0 && o[1] == 0 && o[2] == 0, "view 0/unknown: black (never used)");
    check(pack_mode_bits(3, 0) == 3 && pack_mode_bits(2, 4) == (2u | (4u << 8)) && pack_mode_bits(2, 9) == 2u && pack_mode_bits(0, 0) == 0, "mode bits: low byte = mode, off = unchanged");
    check(!Settings{}.png_once && Settings{}.debug_view == 0 && !Settings{}.png_sink, "diagnostic keys default OFF");
    // Runs: the debug view reaches every job; the png capture is requested ONCE, taken after completion, the sink gets three images; counters in the summary.
    { Rig g; g.stop_after = 12; effects_rig(g); g.settings.debug_view = 3; g.settings.png_once = true; g.settings.png_after_new_frames = 2;
      std::vector<std::string> names; unsigned bytes = 0;
      g.settings.png_sink = [&](const std::string& n, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) { names.push_back(n); bytes += unsigned(rgba.size()); return w == 2 && h == 1; };
      g.settings.summary_ms = 1;
      g.build(); auto r = g.run();
      check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "png run succeeds");
      unsigned caps = 0; for (const auto& j : g.blit->jobs) { caps += j.png_capture; check(j.debug_view == 3, "debug view in every job"); }
      check(caps == 1 && g.blit->png_takes == 1 && names.size() == 3 && bytes == 24, "png capture requested and taken exactly once, three images");
      check(names[0].find("-final.png") != std::string::npos && names[1].find("-alpha.png") != std::string::npos && names[2].find("-worldfx.png") != std::string::npos, "png names");
      check(g.has_line("xr-stereo;event=png-once;outcome=taken;") && g.has_line("files=3/3;") && g.has_line("xr-stereo;event=diagnostics-config;key=xr_debug_view|xr_png_once;debug-view=3;png-once=1;"), "png lines");
      check(g.has_line("xr-stereo;event=summary-effects;") && g.has_line(";producer-valid="), "effects counters in the 5 s summary");
      for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "diagnostic lines fit the router: " + line); }
    // A backend that records no png: three attempts, then given up (no further requests).
    { Rig g; g.stop_after = 14; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      g.settings.png_sink = [](const std::string&, const std::vector<uint8_t>&, uint32_t, uint32_t) { return true; };
      g.build(); g.blit->png_ok = false; auto r = g.run();
      unsigned caps = 0; for (const auto& j : g.blit->jobs) caps += j.png_capture;
      check(r.success && caps == 3 && g.out->failure().empty(), "png: 3 attempts then off, the run is unaffected"); }
    // FLIGHT-DUMP png-eye.now: after the first capture, one trigger file re-arms ONE more capture (consumed once); a stuck file is refused and logged once.
    { Rig g; g.stop_after = 16; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      std::vector<std::string> names;
      g.settings.png_sink = [&](const std::string& n, const std::vector<uint8_t>&, uint32_t, uint32_t) { names.push_back(n); return true; };
      auto file = std::make_shared<bool>(true); auto deletes = std::make_shared<unsigned>(0);
      g.settings.png_eye_trigger = [file, deletes](bool consume) { if (consume) { ++*deletes; const bool had = *file; *file = false; return had; } return *file; };
      g.build(); auto r = g.run();
      unsigned caps = 0; for (const auto& j : g.blit->jobs) caps += j.png_capture;
      check(r.success && caps == 2 && g.blit->png_takes == 2 && names.size() == 6 && *deletes == 1 && !*file, "png-eye.now: one re-arm, two captures, six files");
      check(names[0] != names[3] && g.has_line("xr-stereo;event=png-eye-armed;outcome=armed;why=none;rearms=1;"), "png-eye.now: distinct frame ids, armed line"); }
    { Rig g; g.stop_after = 16; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      g.settings.png_sink = [](const std::string&, const std::vector<uint8_t>&, uint32_t, uint32_t) { return true; };
      auto polls = std::make_shared<unsigned>(0);
      g.settings.png_eye_trigger = [polls](bool consume) { if (consume) return false; ++*polls; return true; };   // exists, delete refused
      g.build(); auto r = g.run();
      unsigned caps = 0, refused = 0; for (const auto& j : g.blit->jobs) caps += j.png_capture;
      for (const auto& l : g.lines) refused += l.find("event=png-eye-armed;outcome=refused;why=trigger-not-consumed") != std::string::npos;
      check(r.success && caps == 1 && *polls >= 1 && refused == 1, "png-eye.now stuck: no re-arm, one refused line"); }
    // LAYER-DUMP: a capture whose backend also read the HUD / marker layer images writes them as -hudlayer / -markerlayer next to the eye images.
    { Rig g; g.stop_after = 8; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      std::vector<std::string> names; std::vector<uint32_t> sizes;
      g.settings.png_sink = [&](const std::string& n, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) { names.push_back(n); sizes.push_back(w * h * 4 == px.size() ? w : 0); return true; };
      g.build(); g.blit->png_layers = 2; auto r = g.run();
      check(r.success && names.size() == 5 && names[3].find("-hudlayer.png") != std::string::npos && names[4].find("-markerlayer.png") != std::string::npos, "layer dump: five files");
      check(sizes[3] == 1 && sizes[4] == 1 && g.has_line(";layers=2(1x1);"), "layer dump: sizes and the png-once line"); }
    { Rig g; g.stop_after = 8; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      std::vector<std::string> names;
      g.settings.png_sink = [&](const std::string& n, const std::vector<uint8_t>&, uint32_t, uint32_t) { names.push_back(n); return true; };
      g.build(); g.blit->png_layers = 1; auto r = g.run();
      check(r.success && names.size() == 4 && g.has_line(";layers=1(1x1);"), "layer dump: HUD layer only");
      Rig g0; g0.stop_after = 8; effects_rig(g0); g0.settings.png_once = true; g0.settings.png_after_new_frames = 1;
      g0.settings.png_sink = [](const std::string&, const std::vector<uint8_t>&, uint32_t, uint32_t) { return true; };
      g0.build(); auto r0 = g0.run();
      check(r0.success && g0.has_line(";layers=0(0x0);"), "layer dump: none read, none written"); }
    // HUD-LATE SUBTITLE SHOT: the producer's in-process request re-arms ONE more capture after the first (taken once, flag cleared); png_once off: never taken.
    { Rig g; g.stop_after = 16; effects_rig(g); g.settings.png_once = true; g.settings.png_after_new_frames = 1;
      std::vector<std::string> names;
      g.settings.png_sink = [&](const std::string& n, const std::vector<uint8_t>&, uint32_t, uint32_t) { names.push_back(n); return true; };
      acz::gpu_copy::step5::hud_late_eye_request().store(true);
      g.build(); auto r = g.run();
      unsigned caps = 0; for (const auto& j : g.blit->jobs) caps += j.png_capture;
      check(r.success && caps == 2 && names.size() == 6 && !acz::gpu_copy::step5::hud_late_eye_request().load(), "subtitle shot: one re-arm, two captures, request cleared");
      check(g.has_line("trigger=hud-late-onset(subtitle);"), "subtitle shot: armed line"); }
    { Rig g; g.stop_after = 8; effects_rig(g);
      acz::gpu_copy::step5::hud_late_eye_request().store(true);
      g.build(); auto r = g.run();
      check(r.success && acz::gpu_copy::step5::hud_late_eye_request().exchange(false), "subtitle shot: not taken without png_once"); }
    // png_once off: the trigger is never polled.
    { Rig g; g.stop_after = 8; effects_rig(g);
      auto polls = std::make_shared<unsigned>(0);
      g.settings.png_eye_trigger = [polls](bool) { ++*polls; return true; };
      g.build(); auto r = g.run();
      check(r.success && *polls == 0, "png-eye.now: not polled without png_once"); }
    // No sink: png_once is forced off. Key off (defaults): no png request, debug view 0, no diagnostics lines.
    { Rig g; g.stop_after = 8; effects_rig(g); g.settings.png_once = true; g.build(); auto r = g.run();
      for (const auto& j : g.blit->jobs) check(!j.png_capture && j.debug_view == 0, "no sink: no png request");
      check(r.success && !g.has_line("event=png-once") && !g.has_line("event=diagnostics-config"), "no sink: no lines"); }
    { Rig g; g.stop_after = 8; effects_rig(g); g.build(); auto r = g.run();
      for (const auto& j : g.blit->jobs) check(!j.png_capture && j.debug_view == 0, "key off: jobs unchanged");
      check(r.success && !g.has_line("event=png-once") && !g.has_line("event=diagnostics-config"), "key off: no diagnostics lines"); }
    // Out of range debug view = off.
    { Rig g; g.stop_after = 4; effects_rig(g); g.settings.debug_view = 7; g.build(); auto r = g.run();
      for (const auto& j : g.blit->jobs) check(j.debug_view == 0, "debug view 7 is off"); (void)r; }
}
void effects_runs() {
    using namespace xr_stereo;
    // ON: every composite frame with a valid effects image records mode 3 (use_effects) with ITS OWN image; no extra swapchain, no extra layer.
    { Rig g; g.stop_after = 8; effects_rig(g); g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0 && !g.xr->layers.empty(), "effects run succeeds, every source restored");
      check(c.effects_frames == c.new_frames && c.effects_frames > 0 && c.effects_skipped == 0 && c.effects_invalid == 0, "effects counters");
      for (const auto& j : g.blit->jobs) {
          check(j.use_effects && j.world_composite && !j.world_held && j.effects.resource != nullptr && j.effects.format == 10 && j.effects.width == 2560 && j.effects.height == 1440, "job carries the effects image");
          check(j.effects.resource != j.eyes[0].resource && j.effects.resource != j.world.resource && j.effects.resource != j.hud.resource, "effects image distinct from eye/world/HUD");
      }
      check(g.j.count("xr.create 4") == 0 && g.j.prefix("xr.acquire 4") == 0, "no extra swapchain for the effects");
      for (auto& l : g.xr->layers) { const auto list = compose_layers(l); check(!l.quad && l.world_composite && l.hud && list.count == 2, "layer list unchanged: projection then HUD"); }
      check(g.has_line("xr-stereo;event=effects-config;key=xr_effects_layer;requires=producer-gpu_copy_step5_effects;") &&
            g.has_line("xr-stereo;event=effects-result;key=xr_effects_layer;effects-frames=" + std::to_string(c.effects_frames) + ";effects-skipped=0;effects-invalid=0;no-headset-claim"), "effects log lines");
      for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "effects lines fit the router: " + line); }
    // Key OFF: a producer that publishes effects changes nothing (identical recordings, no counters, no lines).
    { Rig off; off.stop_after = 8; effects_rig(off); off.settings.effects_layer = false; off.build(); auto r = off.run();
      Rig base; base.stop_after = 8; effects_rig(base); base.settings.effects_layer = false; base.src.with_effects = false; base.build(); auto rb = base.run();
      check(r.success && rb.success && off.blit->jobs.size() == base.blit->jobs.size(), "key-off runs");
      for (const auto& j : off.blit->jobs) check(!j.use_effects && j.effects.resource == nullptr, "key off: no effects in the job");
      const auto& c = off.out->counters();
      check(c.effects_frames == 0 && c.effects_skipped == 0 && c.effects_invalid == 0, "key off: effects counters never move");
      check(!off.has_line("event=effects-") && off.lines.size() == base.lines.size(), "key off: no effects lines, same line count as a producer without effects");
      for (size_t i = 0; i < off.blit->jobs.size(); ++i) check(std::memcmp(&off.blit->jobs[i].world_eye, &base.blit->jobs[i].world_eye, sizeof off.blit->jobs[i].world_eye) == 0, "key off: ray constants identical"); }
    // Without xr_world_in_projection the layer is forced off (the effects are composited inside that pass only).
    { Rig g; g.stop_after = 6; effects_rig(g); g.settings.world_in_projection = false; g.build(); auto r = g.run();
      check(r.success && g.out->counters().effects_frames == 0 && g.out->counters().effects_skipped == 0, "forced off without world-in-projection");
      for (const auto& j : g.blit->jobs) check(!j.use_effects, "no effects without the composite"); }
    // Frames without effects_valid (every 2nd) or without a fresh world are composited without effects; never an older image.
    { Rig g; g.stop_after = 8; effects_rig(g); g.src.effects_every = 2; g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.effects_frames > 0 && c.effects_skipped > 0 && c.effects_frames + c.effects_skipped == c.new_frames && c.effects_invalid == 0, "effects_valid every 2nd frame: skipped counted");
      unsigned with = 0; for (const auto& j : g.blit->jobs) with += j.use_effects; check(with == c.effects_frames, "jobs with effects == effects frames"); }
    { Rig g; g.stop_after = 8; effects_rig(g); g.src.world_gap_from = 1; g.src.world_gap_to = 1000; g.build(); auto r = g.run();
      for (const auto& j : g.blit->jobs) check(!j.use_effects, "no world: no effects");
      check(r.success && g.out->counters().effects_frames == 0, "no world, no effects frames"); }
    // Refusals: counted invalid + skipped, the frame still succeeds without effects.
    using EB = FakeSource::EffectsBad;
    for (EB bad : {EB::null_resource, EB::alias_eye, EB::alias_world, EB::alias_hud, EB::bad_format}) {
        Rig g; g.stop_after = 6; effects_rig(g); g.src.effects_bad = bad; g.build(); auto r = g.run();
        const auto& c = g.out->counters();
        check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "refused effects image: run succeeds");
        check(c.effects_frames == 0 && c.effects_invalid == c.new_frames && c.effects_skipped == c.new_frames, "refused effects image counted invalid/skipped");
        for (const auto& j : g.blit->jobs) check(!j.use_effects, "refused effects image never reaches the recording"); }
    { Rig g; g.stop_after = 6; effects_rig(g); g.build(); g.blit->effects_unreadable = true; auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.effects_frames == 0 && c.effects_invalid == c.new_frames && c.effects_skipped == c.new_frames, "backend refuses the effects source: mode 2, counted"); }
}

// J-effects-dual: representation tag flow (producer StereoFrame::effects_repr -> job.effects_coverage -> shader mode 4) and the coverage maths.
void effects_dual() {
    using namespace xr_stereo;
    check(stereo::effects_repr_legacy == 0 && stereo::effects_repr_coverage_v1 == 1, "representation tags: legacy 0, coverage_v1 1");
    // CPU mirror: literal values of docs/review/patches/effects-dual-source-composite.md, derived by hand (not by the production mirror).
    auto nearf = [](float a, float b) { return std::fabs(a - b) <= 1e-6f; };
    const Shade B{0.25f, 0.5f, 0.75f, 1.0f};
    {   const float row1[4] = {0.125f, 0.0625f, 0, 0.25f};                       // S=(.5,.25,0), a=.25
        auto o = effects_coverage_over_world(row1, Tonemap::clamp, 1.0f, 1.0f, B);
        check(nearf(o.r, 0.3125f) && nearf(o.g, 0.4375f) && nearf(o.b, 0.5625f) && nearf(o.a, 1.0f), "coverage: P + (1-A)*world");
        o = effects_coverage_over_world(row1, Tonemap::reinhard, 2.0f, 1.0f, B);
        check(nearf(o.r, 0.3125f) && nearf(o.g, 11.0f / 24.0f) && nearf(o.b, 0.5625f), "coverage: reinhard exposure 2 (un-premultiply, tonemap, premultiply)"); }
    {   const float f1[4] = {0, 0, 0, 0.25f};                                    // finding 1: black out0, a = 0.25, src1 alpha 0.5
        auto o = effects_coverage_over_world(f1, Tonemap::clamp, 1.0f, 1.0f, B);
        check(nearf(o.r, 0.1875f) && nearf(o.g, 0.375f) && nearf(o.b, 0.5625f), "finding 1: 0.75 of the world is kept");
        const float legacy[4] = {0, 0, 0, 0.125f};                               // what the old heuristic saw (game alpha a*b)
        o = effects_over_world(legacy, Tonemap::clamp, 1.0f, 1.0f, B);
        check(nearf(o.r, 0.21875f) && nearf(o.g, 0.4375f) && nearf(o.b, 0.65625f), "old heuristic keeps 0.875 (the documented error)"); }
    {   const float none[4] = {0, 0, 0, 0}, nan[4] = {1, 1, 1, std::numeric_limits<float>::quiet_NaN()};
        auto o = effects_coverage_over_world(none, Tonemap::reinhard, 2.0f, 1.0f, B);
        check(o.r == B.r && o.g == B.g && o.b == B.b && o.a == B.a, "zero coverage: the world bit for bit, no division");
        o = effects_coverage_over_world(nan, Tonemap::clamp, 1.0f, 1.0f, B);
        check(o.r == B.r && o.g == B.g && o.b == B.b && o.a == B.a, "NaN coverage: the world"); }
    {   const float row1[4] = {0.125f, 0.0625f, 0, 0.25f}; const Shade w{0.125f, 0.25f, 0.375f, 0.5f};
        const auto o = effects_coverage_over_world(row1, Tonemap::clamp, 1.0f, 0.5f, w);
        check(nearf(o.r, 0.171875f) && nearf(o.g, 0.25f) && nearf(o.b, 0.328125f) && nearf(o.a, 0.5625f), "coverage: window ramp 0.5"); }
    // Runs. coverage_v1 frames reach the backend with effects_coverage (mode 4); legacy frames keep mode 3; unknown tags are refused (fail closed).
    { Rig g; g.stop_after = 8; effects_rig(g); g.src.effects_repr = 1; g.build(); auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "coverage_v1 run succeeds");
      check(c.effects_frames == c.new_frames && c.effects_frames > 0 && c.effects_invalid == 0 && c.effects_skipped == 0, "coverage_v1 counters");
      for (const auto& j : g.blit->jobs) check(j.use_effects && j.effects_coverage && j.world_composite && !j.world_held, "coverage_v1 job: mode 4");
      check(g.has_line("effects-repr=legacy|coverage_v1(frame-tagged,mode4:P+(1-A)*world);unknown-repr=invalid,mode2;"), "config line names the representations"); }
    { Rig g; g.stop_after = 8; effects_rig(g); g.build(); auto r = g.run();
      check(r.success && g.out->counters().effects_frames > 0, "legacy run succeeds");
      for (const auto& j : g.blit->jobs) check(j.use_effects && !j.effects_coverage, "legacy tag: mode 3 exactly as before"); }
    for (uint32_t bad : {2u, 7u, 0xffffffffu}) {
        Rig g; g.stop_after = 6; effects_rig(g); g.src.effects_repr = bad; g.build(); auto r = g.run();
        const auto& c = g.out->counters();
        check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "unknown representation: run succeeds");
        check(c.effects_frames == 0 && c.effects_invalid == c.new_frames && c.effects_skipped == c.new_frames, "unknown representation counted invalid/skipped (never composited)");
        for (const auto& j : g.blit->jobs) check(!j.use_effects && !j.effects_coverage, "unknown representation never reaches the recording"); }
    // The mode number sent to the shader: 0..3 as before, 4 = coverage. (pack_mode_bits keeps the low byte.)
    check(pack_mode_bits(4, 0) == 4 && pack_mode_bits(4, 3) == (4u | (3u << 8)), "mode bits carry mode 4");
}

// ------------------------------------------------- J-world-in-projection ---
// Hypothesis under test offline: the world must live INSIDE the projection images (SteamVR draws quad
// layers above projection layers). Nothing here proves the compositor behaviour; it proves the layer list,
// the packed ray constants and the plane math that the shader mirrors operation for operation.
void quat_mul(const double a[4], const double b[4], double out[4]) { // (x, y, z, w) Hamilton product a*b
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
xr_owner::EyeSample eye_at(const float p[3], double yaw, double pitch, const xr_owner::Fov& fov) {
    const double qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, qx[4] = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    double q[4]; quat_mul(qy, qx, q);
    xr_owner::EyeSample e;
    for (int i = 0; i < 3; ++i) e.pose.position[i] = p[i];
    for (int i = 0; i < 4; ++i) e.pose.orientation[i] = float(q[i]);
    e.fov = fov;
    return e;
}
// Independent reference: intersect the eye ray of pixel uv with the PLANE OF THE QUAD LAYER (pose from
// world_quad(), extents width x height) in LOCAL, no anchor-frame maths at all.
struct RefHit { bool hit = false; double u = 0, v = 0; };
RefHit ref_world_uv(const xr_stereo::Quad& q, const xr_owner::EyeSample& e, double u, double v) {
    const double tl = std::tan(double(e.fov.left)), tr = std::tan(double(e.fov.right)), tu = std::tan(double(e.fov.up)), td = std::tan(double(e.fov.down));
    const double de[3] = {tl + u * (tr - tl), tu + v * (td - tu), -1.0};
    double d[3], X[3], Y[3], Z[3];
    quat_apply(e.pose.orientation, de, d);
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    quat_apply(q.pose.orientation, ex, X); quat_apply(q.pose.orientation, ey, Y); quat_apply(q.pose.orientation, ez, Z);
    double cp[3];
    for (int i = 0; i < 3; ++i) cp[i] = double(q.pose.position[i]) - double(e.pose.position[i]);
    const double denom = d[0] * Z[0] + d[1] * Z[1] + d[2] * Z[2];
    RefHit r;
    if (!(denom < -1e-9)) return r;
    const double s = (cp[0] * Z[0] + cp[1] * Z[1] + cp[2] * Z[2]) / denom;
    if (!(s > 0)) return r;
    double h[3];
    for (int i = 0; i < 3; ++i) h[i] = double(e.pose.position[i]) + s * d[i] - double(q.pose.position[i]);
    r.u = (h[0] * X[0] + h[1] * X[1] + h[2] * X[2]) / double(q.width) + 0.5;
    r.v = 0.5 - (h[0] * Y[0] + h[1] * Y[1] + h[2] * Y[2]) / double(q.height);
    r.hit = r.u >= 0 && r.u <= 1 && r.v >= 0 && r.v <= 1;
    return r;
}
// uv of the eye image pixel whose ray has horizontal / vertical tangents (tx, ty) in the eye's own frame.
void uv_for_tan(const xr_owner::Fov& f, double tx, double ty, double& u, double& v) {
    const double tl = std::tan(double(f.left)), tr = std::tan(double(f.right)), tu = std::tan(double(f.up)), td = std::tan(double(f.down));
    u = (tx - tl) / (tr - tl);
    v = (ty - tu) / (td - tu);
}

// xr_look_drive_keys: the world / marker planes are turned by the look the game rendered (StereoFrame::look_*). Key off: look 0 is the old maths.
void world_look_math() {
    using namespace xr_stereo;
    const float sym[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    const float anchor[3] = {0.4f, 1.3f, -0.2f};
    const xr_owner::Fov wide{-0.9f, 0.9f, 0.8f, -0.8f};
    // Key off / zero look: bit-for-bit the previous results.
    { WorldComposite a{}, b{};
      const auto eye = eye_at(anchor, 0.7, 0.1, wide);
      check(world_composite_params(eye, anchor, 0.5f, sym, 40.0f, a) && world_composite_params(eye, anchor, 0.5f, sym, 40.0f, b, 0.0f) &&
            std::memcmp(&a, &b, sizeof a) == 0, "pitch 0: world_composite_params identical to the call without it");
      const Quad q0 = world_quad(anchor, sym, 40.0f, 0.5f), q1 = world_quad(anchor, sym, 40.0f, 0.5f, 0.0f);
      check(std::memcmp(&q0, &q1, sizeof q0) == 0, "pitch 0: world_quad(yaw, pitch) is exactly world_quad(yaw)"); }
    // The plane along the look: the eye turned by (yaw0 + look yaw, look pitch) looks at the plane centre; the composite maths equal an independent
    // intersection with the turned quad layer; the quad's forward direction and size are as documented.
    unsigned compared = 0;
    for (float yaw0 : {0.0f, 0.6f, -2.0f}) for (float ly : {0.0f, 0.5f, -1.1f, 2.3f}) for (float lp : {0.0f, 0.3f, 1.0f}) for (float D : {5.0f, 40.0f}) {
        const float yaw = yaw0 + ly;
        const Quad q = world_quad(anchor, sym, D, yaw, lp);
        check(std::fabs(q.width - D * 2 * 0.6152f) < 1e-3f && std::fabs(q.height - D * 2 * 0.3460f) < 1e-3f, "turned quad keeps its size");
        // orientation sends -Z to Ry(yaw) Rx(pitch) (0,0,-1) = (-sin yaw cos p, sin p, -cos yaw cos p) and the centre lies D along it
        const double c[3] = {0, 0, -1}; double fwd[3]; quat_apply(q.pose.orientation, c, fwd);
        const double want[3] = {-std::sin(yaw) * std::cos(lp), std::sin(lp), -std::cos(yaw) * std::cos(lp)};
        check(std::fabs(fwd[0] - want[0]) < 1e-5 && std::fabs(fwd[1] - want[1]) < 1e-5 && std::fabs(fwd[2] - want[2]) < 1e-5, "turned quad faces the look direction");
        const double dx = q.pose.position[0] - anchor[0], dy = q.pose.position[1] - anchor[1], dz = q.pose.position[2] - anchor[2];
        check(std::fabs(dx - D * want[0]) < 2e-4 && std::fabs(dy - D * want[1]) < 2e-4 && std::fabs(dz - D * want[2]) < 2e-4, "turned quad centre is D along the look direction");
        // an eye looking along the look sees the plane centre
        const auto look_eye = eye_at(anchor, yaw, lp, wide);
        WorldComposite wc{};
        check(world_composite_params(look_eye, anchor, yaw, sym, D, wc, lp), "look params");
        double cu = 0, cv = 0; uv_for_tan(wide, 0.0, 0.0, cu, cv);
        const auto hit = world_ray_uv(wc, float(cu), float(cv));
        check(hit.hit && std::fabs(hit.u - 0.5f) < 2e-4f && std::fabs(hit.v - 0.5f) < 2e-4f, "an eye along the rendered look hits the world plane centre");
        // many rays against the independent quad intersection
        for (double dyaw : {-0.3, 0.0, 0.25}) for (double dp : {-0.2, 0.0, 0.15}) for (float dxp : {-0.032f, 0.032f}) {
            const float p[3] = {anchor[0] + 0.1f + dxp * float(std::cos(yaw)), anchor[1] + 0.02f, anchor[2] - 0.05f - dxp * float(std::sin(yaw))};
            const auto eye = eye_at(p, yaw + dyaw, lp + dp, wide);
            WorldComposite w2{};
            check(world_composite_params(eye, anchor, yaw, sym, D, w2, lp), "params");
            for (int iu = 0; iu <= 8; ++iu) for (int iv = 0; iv <= 8; ++iv) {
                const double u = (iu + 0.5) / 9.0, v = (iv + 0.5) / 9.0;
                const RefHit ref = ref_world_uv(q, eye, u, v);
                const WorldHit got = world_ray_uv(w2, float(u), float(v));
                const bool near_border = std::fabs(ref.u) < 2e-3 || std::fabs(ref.u - 1) < 2e-3 || std::fabs(ref.v) < 2e-3 || std::fabs(ref.v - 1) < 2e-3;
                if (ref.hit && !near_border) { check(got.hit && std::fabs(got.u - ref.u) < 3e-4 && std::fabs(got.v - ref.v) < 3e-4, "look: ray/plane hit uv matches the quad-layer intersection"); ++compared; }
                else if (!ref.hit && !near_border && ref.u > -50 && ref.u < 50 && ref.v > -50 && ref.v < 50) check(!got.hit, "look: a ray that misses the quad layer misses");
            }
        }
    }
    check(compared > 500, "enough look rays compared with the independent intersection");
    // Without the look the same head direction misses (a head turned 1 rad sees nothing of the unturned 63 deg plane): the double rotation the
    // consumer change removes.
    { WorldComposite wc{};
      const auto eye = eye_at(anchor, 1.0, 0.0, wide);
      double cu = 0, cv = 0; uv_for_tan(wide, 0.0, 0.0, cu, cv);
      check(world_composite_params(eye, anchor, 0.0f, sym, 40.0f, wc) && !world_ray_uv(wc, float(cu), float(cv)).hit, "unturned plane: a head 57 deg to the side sees black");
      check(world_composite_params(eye, anchor, 1.0f, sym, 40.0f, wc) && world_ray_uv(wc, float(cu), float(cv)).hit, "plane turned by the look: it sees the world"); }
}

void world_projection_layout_and_compose() {
    using namespace xr_stereo;
    // The packed struct IS the root-constant tail after b0[0..4] (HLSL cbuffer rows 1..5).
    check(offsetof(WorldComposite, origin) == 0 && offsetof(WorldComposite, base) == 12 && offsetof(WorldComposite, dist) == 24 &&
        offsetof(WorldComposite, ux) == 28 && offsetof(WorldComposite, au) == 40 && offsetof(WorldComposite, vy) == 44 &&
        offsetof(WorldComposite, bu) == 56 && offsetof(WorldComposite, av) == 60 && offsetof(WorldComposite, bv) == 64 &&
        sizeof(WorldComposite) == 68 && kBlitConstantsLegacy == 22 && kBlitConstants == 24, "WorldComposite layout = cbuffer rows (origin | base dist | ux au | vy bu | av bv | fade extend)");
    // Layer list with the key on: no quad, projection first (opaque), HUD last.
    { ProjectionLayer p; p.world_composite = true;
      auto l = compose_layers(p);
      check(l.count == 1 && l.e[0].kind == LayerKind::projection && !l.e[0].source_alpha && !l.e[0].unpremultiplied, "world-composite: projection only, opaque");
      p.hud = true;
      l = compose_layers(p);
      check(l.count == 2 && l.e[0].kind == LayerKind::projection && !l.e[0].source_alpha && l.e[1].kind == LayerKind::hud_quad &&
          l.e[1].source_alpha && !l.e[1].unpremultiplied && l.e[1].space == LayerSpace::local, "world-composite + HUD: projection then HUD (premultiplied SOURCE_ALPHA)");
      p.hud_view_space = true;
      check(compose_layers(p).e[1].space == LayerSpace::view, "head-locked HUD still VIEW");
      ProjectionLayer q; q.quad = true; q.hud = true;
      check(compose_layers(q).count == 3 && compose_layers(q).e[0].kind == LayerKind::world_quad, "key off list unchanged: quad first"); }
    // Combine: cockpit + (1 - a) * world, premultiplied.
    { const Shade w{0.2f, 0.4f, 0.6f, 1.0f};
      Shade r = composite_over_world({0.5f, 0.5f, 0.5f, 1.0f}, w);
      check(r.r == 0.5f && r.g == 0.5f && r.b == 0.5f && r.a == 1.0f, "opaque cockpit hides the world");
      r = composite_over_world({0, 0, 0, 0}, w);
      check(r.r == w.r && r.g == w.g && r.b == w.b && r.a == 1.0f, "uncovered cockpit shows the world");
      r = composite_over_world({0.3f, 0.0f, 0.0f, 0.5f}, w);
      check(std::fabs(r.r - 0.4f) < 1e-6f && std::fabs(r.g - 0.2f) < 1e-6f && std::fabs(r.b - 0.3f) < 1e-6f && std::fabs(r.a - 1.0f) < 1e-6f, "half coverage blends premultiplied");
      r = composite_over_world({0, 0, 0, 0}, {0, 0, 0, 0});
      check(r.a == 0.0f, "no world, no cockpit: transparent (black over nothing)"); }
}

void world_projection_math() {
    using namespace xr_stereo;
    const float sym[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    const float anchor[3] = {0.4f, 1.3f, -0.2f};
    const xr_owner::Fov wide{-0.9f, 0.9f, 0.8f, -0.8f};
    const double kd = kPi / 180.0;
    // 1. A ray along the anchor forward hits the quad centre (any yaw, any distance); edges at +-31.6 deg.
    for (float yaw_deg : {0.0f, 30.0f, -90.0f, 180.0f}) for (float D : {5.0f, 40.0f, 500.0f}) {
        const double yaw = yaw_deg * kd;
        const auto eye = eye_at(anchor, yaw, 0.0, wide);
        WorldComposite wc{};
        check(world_composite_params(eye, anchor, float(yaw), sym, D, wc), "params for the anchor-forward eye");
        double cu = 0, cv = 0; uv_for_tan(wide, 0.0, 0.0, cu, cv);
        const auto c = world_ray_uv(wc, float(cu), float(cv));
        check(c.hit && std::fabs(c.u - 0.5f) < 1e-4f && std::fabs(c.v - 0.5f) < 1e-4f, "forward ray hits the quad centre (yaw " + std::to_string(yaw_deg) + " D " + std::to_string(D) + ")");
        // horizontal edges: tan 0.6152 (= 31.6 deg) maps to u = 0 / 1.
        for (int sgn : {-1, 1}) {
            const double edge = 0.6152;
            double u = 0, v = 0;
            uv_for_tan(wide, sgn * (edge * 0.999), 0.0, u, v);
            auto in = world_ray_uv(wc, float(u), float(v));
            check(in.hit && std::fabs(in.u - (sgn < 0 ? 0.0005f : 0.9995f)) < 2e-3f, "just inside the +-31.6 deg horizontal edge hits");
            uv_for_tan(wide, sgn * (edge * 1.002), 0.0, u, v);
            check(!world_ray_uv(wc, float(u), float(v)).hit, "just outside the +-31.6 deg horizontal edge misses");
        }
        for (int sgn : {-1, 1}) {
            double u = 0, v = 0;
            uv_for_tan(wide, 0.0, sgn * 0.346 * 0.999, u, v);
            check(world_ray_uv(wc, float(u), float(v)).hit, "just inside the vertical edge hits");
            uv_for_tan(wide, 0.0, sgn * 0.346 * 1.002, u, v);
            check(!world_ray_uv(wc, float(u), float(v)).hit, "just outside the vertical edge misses");
        }
        check(std::fabs(std::atan(0.6152) / kd - 31.6) < 0.01 && std::fabs(std::atan(0.3460) / kd - 19.1) < 0.02, "lens half angles 31.6 x 19.1 deg (63.2 x 38.2)");
    }
    // 2. yaw0 rotation: an eye with the UNTURNED orientation does not look at a plane turned by 90 deg;
    //    the same eye turned by yaw0 does; the anchor frame is turned, not the eye.
    { WorldComposite wc{};
      const auto straight = eye_at(anchor, 0.0, 0.0, wide);
      check(world_composite_params(straight, anchor, float(kPi / 2), sym, 40.0f, wc), "params (yaw 90, eye looking -Z)");
      double u = 0, v = 0; uv_for_tan(wide, 0.0, 0.0, u, v);
      check(!world_ray_uv(wc, float(u), float(v)).hit, "plane turned by 90 deg: the unturned eye's centre ray is parallel to it (miss)");
      const auto turned = eye_at(anchor, kPi / 2, 0.0, wide);
      check(world_composite_params(turned, anchor, float(kPi / 2), sym, 40.0f, wc) && world_ray_uv(wc, float(u), float(v)).hit, "the eye turned by yaw0 hits it"); }
    // 3. Off-quad rays: looking back, up, down, past the plane.
    { WorldComposite wc{};
      double u = 0, v = 0; uv_for_tan(wide, 0.0, 0.0, u, v);
      check(world_composite_params(eye_at(anchor, kPi, 0.0, wide), anchor, 0.0f, sym, 40.0f, wc) && !world_ray_uv(wc, float(u), float(v)).hit, "looking backwards misses");
      check(world_composite_params(eye_at(anchor, 0.0, 0.6, wide), anchor, 0.0f, sym, 40.0f, wc) && !world_ray_uv(wc, float(u), float(v)).hit, "looking 34 deg up misses (vertical extent 19.1 deg)");
      check(world_composite_params(eye_at(anchor, 0.0, -0.6, wide), anchor, 0.0f, sym, 40.0f, wc) && !world_ray_uv(wc, float(u), float(v)).hit, "looking 34 deg down misses");
      const float beyond[3] = {0.4f, 1.3f, -60.0f};   // 59.8 m ahead of the anchor: past the 40 m plane
      check(world_composite_params(eye_at(beyond, 0.0, 0.0, wide), anchor, 0.0f, sym, 40.0f, wc) && !world_ray_uv(wc, float(u), float(v)).hit, "eye past the plane misses (hit behind)");
    }
    // 4. Per-eye parallax at a near plane: the two eyes hit different texels, by IPD / plane width.
    { WorldComposite l{}, r{};
      const float pl[3] = {anchor[0] - 0.032f, anchor[1], anchor[2]}, pr[3] = {anchor[0] + 0.032f, anchor[1], anchor[2]};
      double u = 0, v = 0; uv_for_tan(wide, 0.0, 0.0, u, v);
      check(world_composite_params(eye_at(pl, 0, 0, wide), anchor, 0, sym, 5.0f, l) && world_composite_params(eye_at(pr, 0, 0, wide), anchor, 0, sym, 5.0f, r), "params L/R");
      const auto hl = world_ray_uv(l, float(u), float(v)), hr = world_ray_uv(r, float(u), float(v));
      const double expect = 0.064 / (5.0 * (2 * 0.6152));
      check(hl.hit && hr.hit && std::fabs(std::fabs(hr.u - hl.u) - expect) < 1e-4, "5 m plane: eyes differ by IPD/plane width (parallax)");
      check(hl.u < hr.u, "each eye straight-ahead ray hits the plane at its own x: left u < right u"); }
    // 5. Against an independent LOCAL plane/quad intersection (quad layer pose), many poses and lenses.
    unsigned compared = 0;
    for (bool asym : {false, true}) for (float yaw_deg : {0.0f, 27.0f, -140.0f}) for (float D : {5.0f, 40.0f, 500.0f}) {
        const float* t = asym ? kAsymLens : sym;
        const double yaw = yaw_deg * kd;
        const Quad q = world_quad(anchor, t, D, float(yaw));
        for (double dyaw : {-0.3, 0.0, 0.25}) for (double pitch : {-0.2, 0.0, 0.15}) for (float dx : {-0.032f, 0.032f}) {
            const float p[3] = {anchor[0] + 0.1f + dx * float(std::cos(yaw)), anchor[1] + 0.02f, anchor[2] - 0.05f + dx * float(-std::sin(yaw))};
            const auto eye = eye_at(p, yaw + dyaw, pitch, wide);
            WorldComposite wc{};
            check(world_composite_params(eye, anchor, float(yaw), t, D, wc), "params");
            for (int iu = 0; iu <= 8; ++iu) for (int iv = 0; iv <= 8; ++iv) {
                const double u = (iu + 0.5) / 9.0, v = (iv + 0.5) / 9.0;
                const RefHit ref = ref_world_uv(q, eye, u, v);
                const WorldHit got = world_ray_uv(wc, float(u), float(v));
                const bool near_border = std::fabs(ref.u) < 2e-3 || std::fabs(ref.u - 1) < 2e-3 || std::fabs(ref.v) < 2e-3 || std::fabs(ref.v - 1) < 2e-3;
                if (ref.hit && !near_border) {
                    check(got.hit && std::fabs(got.u - ref.u) < 2e-4 && std::fabs(got.v - ref.v) < 2e-4, "ray/plane hit uv matches the quad-layer intersection");
                    ++compared;
                } else if (!ref.hit && !near_border && ref.u > -50 && ref.u < 50 && ref.v > -50 && ref.v < 50) {
                    check(!got.hit, "ray that misses the quad layer misses");
                }
            }
        }
    }
    check(compared > 2000, "enough in-quad rays compared: " + std::to_string(compared));
    // 6. Refusals: nothing degenerate becomes a ray.
    { WorldComposite wc{};
      const auto eye = eye_at(anchor, 0, 0, wide);
      check(!world_composite_params(eye, anchor, 0, sym, 0.0f, wc) && !world_composite_params(eye, anchor, 0, sym, std::nanf(""), wc), "distance 0 / NaN refused");
      const float bad[4] = {0.6f, 0.6f, 0.0f, 0.3f};
      check(!world_composite_params(eye, anchor, 0, bad, 40.0f, wc) && !world_composite_params(eye, anchor, 0, nullptr, 40.0f, wc), "bad lens refused");
      check(!world_composite_params(eye, anchor, std::nanf(""), sym, 40.0f, wc), "NaN yaw refused");
      auto e2 = eye; e2.pose.position[0] = std::nanf(""); check(!world_composite_params(e2, anchor, 0, sym, 40.0f, wc), "NaN eye position refused");
      e2 = eye; e2.pose.orientation[3] = 0.0f; e2.pose.orientation[1] = 0.0f; check(!world_composite_params(e2, anchor, 0, sym, 40.0f, wc), "zero quaternion refused");
      e2 = eye; e2.fov = {0.5f, -0.5f, 0.4f, -0.4f}; check(!world_composite_params(e2, anchor, 0, sym, 40.0f, wc), "inverted FOV refused"); }
}

void world_projection_config_keys() {
    using namespace acz::diagnostics;
    auto d = parse_config("xr_stereo_output=true\n");
    check(d.issue == ConfigIssue::none && !d.config.xr_world_in_projection(), "xr_world_in_projection default OFF");
    check(parse_config("xr_world_in_projection=true\n").config.xr_world_in_projection() && parse_config("xr_world_in_projection=1\n").config.xr_world_in_projection(), "ON: true / 1");
    check(!parse_config("xr_world_in_projection=false\n").config.xr_world_in_projection() && !parse_config("xr_world_in_projection=0\n").config.xr_world_in_projection(), "OFF: false / 0");
    check(parse_config("xr_world_in_projection=yes\n").issue == ConfigIssue::out_of_range, "malformed value rejected");
    check(parse_config("xr_world_in_projection=true\nxr_world_in_projection=true\n").issue == ConfigIssue::duplicate, "duplicate rejected");
    // The recommended headset-test line set still fits the 24-key / 2048-byte config with the new key.
    const std::string cfg = "log_enabled=true\ndiagnostic_ownership_pins=true\ngpu_copy_witness_observation=true\ngpu_copy_early_device_hook=true\n"
        "gpu_copy_step3_experiment=true\ngpu_copy_step3_whole_set=true\ngpu_copy_step5_continuous=true\nxr_stereo_output=true\nxr_edge_feather=0.06\n"
        "gpu_copy_step5_members_by_class=true\ngpu_copy_step5_hmd_fov=true\ngpu_copy_step5_hud=true\nxr_hud_layer=true\nxr_hud_quad_distance_m=0.8\n"
        "xr_hud_scale=0.5\nxr_world_quad_distance_m=500\ngpu_copy_step5_units_per_metre=87\nxr_world_in_projection=true\n";
    const auto full = parse_config(cfg);
    check(full.issue == ConfigIssue::none && full.config.xr_world_in_projection() && cfg.size() < max_config_bytes && max_config_keys == 32, "test config line set parses (18 keys, < 2048 bytes)");
}

void world_projection_runs() {
    using namespace xr_stereo;
    // ON, neutral anchor with a yaw, world + depth + HUD: no quad swapchain, no quad layer, opaque projection, HUD last.
    for (float yaw : {0.0f, 0.5f, -2.2f}) {
        Rig g; g.stop_after = 10; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.settings.world_in_projection = true;
        g.settings.quad_distance_m = 500.0f; g.settings.comfort_log = true; g.settings.edge_feather = 0.06f;
        g.src.with_neutral = true; g.src.nyaw = yaw;
        g.build(); auto r = g.run();
        const std::string what = "yaw " + std::to_string(yaw);
        check(r.success && !g.out->failed() && !g.xr->layers.empty() && g.src.bad_releases == 0 && g.xr->violations == 0, "composite run succeeds: " + what);
        check(g.j.count("xr.create 2") == 0 && g.j.prefix("xr.acquire 2") == 0 && g.j.prefix("xr.create 3") == 1 && !g.xr->chains[2].live, "no quad swapchain created/acquired: " + what);
        const auto& c = g.out->counters();
        check(c.world_composite_frames > 0 && c.world_composite_frames == c.world_frames && c.world_composite_refused == 0 && c.new_frames == c.world_composite_frames, "composite counters: " + what);
        for (auto& l : g.xr->layers) {
            const auto list = compose_layers(l);
            check(!l.quad && l.world_composite && !l.source_alpha && !l.quad_source_alpha && l.hud && list.count == 2 &&
                list.e[0].kind == LayerKind::projection && !list.e[0].source_alpha && list.e[1].kind == LayerKind::hud_quad, "layers: projection (opaque) then HUD, no quad: " + what);
        }
        for (auto& j : g.blit->jobs) check(j.world_composite && j.use_world && j.edge_feather == 0.06f && j.use_hud, "recordings carry the composite request: " + what);
        // The ray constants were built from exactly the poses/FOV the submitted layer carries, anchored at the neutral turned by yaw0.
        for (auto& j : g.blit->jobs) {
            bool matched = false;
            for (auto& l : g.xr->layers) {
                WorldComposite a{}, b{};
                if (world_composite_params(l.views[0], g.src.npos, yaw, g.src.lens, 500.0f, a) && world_composite_params(l.views[1], g.src.npos, yaw, g.src.lens, 500.0f, b) &&
                    std::memcmp(&a, &j.world_eye[0], sizeof a) == 0 && std::memcmp(&b, &j.world_eye[1], sizeof b) == 0) { matched = true; break; }
            }
            check(matched, "recorded ray constants equal world_composite_params(layer view, neutral, yaw0, lens, D): " + what);
        }
        check(g.has_line("xr-stereo;event=world-composite;key=xr_world_in_projection;world-composite=projection;quad-layer=none;quad-swapchain=none;projection-flags=none(opaque);order=projection-then-hud;plane-distance-m=500.0") &&
            g.has_line("xr-stereo;event=world-composite;for=first-layer;world-composite=projection;layers=2;order=projection-then-hud;proj-flags=none(opaque);plane-distance-m=500.0;quad-layer=none") &&
            g.has_line("xr-stereo;event=quad-anchor;for=first-layer;quad-anchor=neutral;") &&
            g.has_line("xr-stereo;event=first-frame;") && g.has_line("world=projection") && g.has_line("world-composite=projection;world-composite-frames="), "log lines: " + what);
        check(g.has_line("xr-stereo;event=first-layer;") && g.has_line("layers=1;order=projection;proj-flags=none(opaque);"), "first-layer line: one layer (the HUD is its own event), opaque projection");
        for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "composite lines fit the router: " + line);
    }
    // Eye-midpoint anchor (no producer neutral): yaw 0 at the first frame's eye midpoint, same as the quad.
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty() && g.blit->jobs[0].world_composite && g.out->counters().anchor_neutral == 0, "eye-midpoint run");
      const auto& l = g.xr->layers.front();
      float mid[3]; for (int i = 0; i < 3; ++i) mid[i] = 0.5f * (l.views[0].pose.position[i] + l.views[1].pose.position[i]);
      WorldComposite a{}; check(world_composite_params(l.views[0], mid, 0.0f, g.src.lens, 40.0f, a) && std::memcmp(&a, &g.blit->jobs[0].world_eye[0], sizeof a) == 0, "anchored at the first frame's eye midpoint, yaw 0");
      check(!l.quad && !l.hud && compose_layers(l).count == 1, "projection only without a HUD"); }
    // The HUD instruments quad keeps working beside the composite (same pose as the key-off world-quad HUD).
    { Rig a; a.stop_after = 8; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true; a.src.with_neutral = true; a.src.nyaw = 0.4f; a.build(); auto ra = a.run();
      Rig b; b.stop_after = 8; hud_full_rig(b); b.settings.hud_layer = true; b.src.with_hud = true; b.src.with_neutral = true; b.src.nyaw = 0.4f; b.settings.world_in_projection = true; b.build(); auto rb = b.run();
      check(ra.success && rb.success && a.xr->layers.size() == b.xr->layers.size(), "HUD comparison runs");
      for (size_t i = 0; i < a.xr->layers.size(); ++i) {
          check(b.xr->layers[i].hud && same_pose(a.xr->layers[i].hud_pose, b.xr->layers[i].hud_pose) && a.xr->layers[i].hud_width == b.xr->layers[i].hud_width &&
              a.xr->layers[i].hud_height == b.xr->layers[i].hud_height, "HUD quad pose/size identical to the key-off run");
          check(same_pose(a.xr->layers[i].views[0].pose, b.xr->layers[i].views[0].pose) && std::memcmp(&a.xr->layers[i].views[1].fov, &b.xr->layers[i].views[1].fov, sizeof(Fov)) == 0, "cockpit views identical to the key-off run");
      }
      check(b.out->counters().hud_layers == a.out->counters().hud_layers && b.out->counters().hud_frames == a.out->counters().hud_frames, "HUD counters identical"); }
    // The frame's own cockpit FOV and the located-view fallback are what the rays use.
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.src.cockpit_fov = true; g.build(); auto r = g.run();
      check(r.success && g.out->counters().cockpit_fov_frames > 0 && !g.blit->jobs.empty(), "cockpit_fov run");
      const auto& l = g.xr->layers.front();
      check(l.views[0].fov.left == g.src.cfov[0][0] && l.views[1].fov.right == g.src.cfov[1][1], "layer carries the cockpit FOV");
      WorldComposite a{}; float mid[3]; for (int i = 0; i < 3; ++i) mid[i] = 0.5f * (l.views[0].pose.position[i] + l.views[1].pose.position[i]);
      check(world_composite_params(l.views[1], mid, 0.0f, g.src.lens, 40.0f, a) && std::memcmp(&a, &g.blit->jobs[0].world_eye[1], sizeof a) == 0, "rays use the cockpit FOV"); }
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.src.pose_valid = false; g.build(); auto r = g.run();
      check(r.success && g.out->counters().pose_from_located > 0 && g.out->counters().world_composite_frames > 0 && !g.out->failed(), "located-pose fallback composites with the located views"); }
    // No world image / invalid lens / world_quad=false: nothing is composited, the cockpit follows the key-off rules.
    { Rig g; g.stop_after = 5; g.src.lens_valid = true; g.src.with_depth = true; g.settings.world_in_projection = true; g.build(); auto r = g.run();
      check(r.success && g.out->counters().world_composite_frames == 0 && !g.xr->layers.front().world_composite && g.xr->layers.front().source_alpha && !g.xr->layers.front().quad, "no world image: depth-coverage projection as before, no quad"); }
    { Rig g; g.stop_after = 5; hud_full_rig(g); g.src.lens_valid = false; g.src.lens[0] = std::nanf(""); g.settings.world_in_projection = true; g.build(); auto r = g.run();
      check(r.success && g.out->counters().world_composite_frames == 0 && g.out->counters().world_lens_invalid > 0 && !g.xr->layers.front().world_composite, "invalid lens: world refused, no composite"); }
    { Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.world_quad = false; g.build(); auto r = g.run();
      check(r.success && g.out->counters().world_composite_frames == 0 && g.j.count("xr.create 2") == 0 && !g.xr->layers.front().world_composite && !g.has_line("event=world-composite"), "world_quad=false makes the key inert"); }
    // Re-submitted (paced) layers keep the composite flag and the opaque projection.
    { Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.publish_until = 3; g.build(); auto r = g.run();
      check(r.success && g.out->counters().reused > 0, "reuse path exercised");
      for (auto& l : g.xr->layers) check(l.world_composite && !l.quad && !l.source_alpha, "re-submitted layers keep the composite shape"); }
    // Key OFF (default): quad layer path, counters and lines untouched.
    { Rig a; a.stop_after = 8; hud_full_rig(a); a.settings.hud_layer = true; a.src.with_hud = true; a.build(); auto ra = a.run();
      check(ra.success && a.out->counters().world_composite_frames == 0 && a.out->counters().world_composite_refused == 0, "key off: no composite counters");
      for (auto& l : a.xr->layers) check(l.quad && !l.world_composite, "key off: world quad layer");
      for (auto& j : a.blit->jobs) check(!j.world_composite, "key off: no composite request");
      check(!a.has_line("world-composite") && a.j.count("xr.create 2") == 1, "key off: no composite lines, quad swapchain created"); }
}


// --------------------------------------------- J-world-hold / J-world-extend ---
// Offline only: the hold's fade, the periphery fill maths (the CPU mirror of world_layer_ext), the consumer's hold bookkeeping against the
// fake backend, the config keys, the layer-fov line and the producer diagnostics helpers. Nothing here runs the shader, the D3D12 copy or a headset.
namespace {
// A WorldComposite whose every ray is the direction d (ux = vy = 0), the eye at `origin`, plane distance D, the symmetric game lens.
xr_stereo::WorldComposite dir_composite(double dx, double dy, double dz, double D = 500.0, const double origin[3] = nullptr) {
    xr_stereo::WorldComposite c{};
    for (int i = 0; i < 3; ++i) c.origin[i] = origin ? float(origin[i]) : 0.0f;
    c.base[0] = float(dx); c.base[1] = float(dy); c.base[2] = float(dz);
    c.dist = float(D);
    const double l = 0.6152, r = 0.6152, u = 0.3460, d = 0.3460;
    c.au = float(1.0 / (D * (l + r))); c.bu = float(l / (l + r)); c.av = float(-1.0 / (D * (u + d))); c.bv = float(u / (u + d));
    return c;
}
bool sample_ok(const xr_stereo::WorldExtendSample& s) {
    return std::isfinite(s.u) && std::isfinite(s.v) && std::isfinite(s.k) && std::isfinite(s.ang) && s.u >= 0.0f && s.u <= 1.0f && s.v >= 0.0f && s.v <= 1.0f &&
        s.k >= xr_stereo::kExtendMinLight - 1e-6f && s.k <= 1.0f;
}
}

void world_hold_fade_math() {
    using namespace xr_stereo;
    check(world_hold_fade(0, 750) == 1.0f && world_hold_fade(375, 750) == 1.0f && world_hold_fade(-5, 750) == 1.0f, "fade: full strength for the first half (and a just-captured negative age)");
    check(world_hold_fade(750, 750) == 0.0f && world_hold_fade(751, 750) == 0.0f && world_hold_fade(100000, 750) == 0.0f, "fade: black at and after expiry");
    check(std::fabs(world_hold_fade(562, 750) - 0.501f) < 0.002f, "fade: linear through the middle of the second half");
    float prev = 1.0f; bool mono = true, range = true;
    for (int64_t a = 0; a <= 800; ++a) { const float f = world_hold_fade(a, 750); mono = mono && f <= prev; range = range && f >= 0.0f && f <= 1.0f; prev = f; }
    check(mono && range, "fade: monotone non-increasing in [0,1]");
    check(world_hold_fade(10, 0) == 0.0f && world_hold_fade(10, -1) == 0.0f, "fade: degenerate hold window = black");
    check(kWorldHoldMs == 750 && kBlitConstants == 24 && kBlitConstantsLegacy == 22, "hold window 750 ms; two extra root constants");
}

void world_extend_math() {
    using namespace xr_stereo;
    const float sym[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    const float anchor[3] = {0.4f, 1.3f, -0.2f};
    const xr_owner::Fov hmd{-0.95909f, 0.73519f, 0.76586f, -0.92861f}; // the Beyond's eye0 fov seen in test 7
    // 1. Window identity: every eye pixel whose ray hits the window gets exactly world_ray_uv, k = 1, no ext; every other pixel is ext.
    for (float yaw : {0.0f, 0.4f, -2.2f}) for (double pitch : {0.0, 0.3, -0.5, 0.7}) for (float D : {40.0f, 500.0f}) {
        WorldComposite wc{};
        check(world_composite_params(eye_at(anchor, yaw, pitch, hmd), anchor, yaw, sym, D, wc), "params");
        unsigned inside = 0, outside = 0; bool exact = true, flags = true, valid = true;
        for (int iy = 0; iy < 48; ++iy) for (int ix = 0; ix < 48; ++ix) {
            const float u = (float(ix) + 0.5f) / 48.0f, v = (float(iy) + 0.5f) / 48.0f;
            const auto h = world_ray_uv(wc, u, v); const auto s = world_ray_extend(wc, u, v);
            valid = valid && sample_ok(s);
            if (h.hit) { ++inside; exact = exact && !s.ext && s.u == h.u && s.v == h.v && s.k == 1.0f && s.ang == 0.0f; }
            else { ++outside; flags = flags && s.ext; }
        }
        check(exact && flags && valid, "window rays are exactly world_ray_uv, the rest are periphery (finite, clamped, k in range)");
        if (pitch == 0.0 && yaw == 0.0f && D == 500.0f) check(inside > 100 && outside > 100, "the headset fov sees both the window and the periphery");
    }
    // 2. Centre identity: the anchor-forward ray is the window centre (0.5, 0.5), k = 1.
    { WorldComposite wc{}; check(world_composite_params(eye_at(anchor, 0.0, 0.0, hmd), anchor, 0.0f, sym, 500.0f, wc), "params");
      double cu = 0, cv = 0; uv_for_tan(hmd, 0.0, 0.0, cu, cv);
      const auto s = world_ray_extend(wc, float(cu), float(cv));
      check(!s.ext && std::fabs(s.u - 0.5f) < 1e-4f && std::fabs(s.v - 0.5f) < 1e-4f && s.k == 1.0f, "centre ray = window centre, undarkened"); }
    // 3. Continuity: neighbouring pixels differ by a small step everywhere, including across the window edge and the front/back blend (the
    //    eye turned 86 deg right sees azimuths past 90 deg; 29 deg up keeps the view clear of the pole, where azimuth converges and uv legitimately changes fast).
    for (double yaw : {0.0, 1.5}) for (double pitch : {0.0, 0.5}) {
        WorldComposite wc{};
        check(world_composite_params(eye_at(anchor, yaw, pitch, hmd), anchor, 0.0f, sym, 500.0f, wc), "params (continuity)");
        const int N = 1200; double worst = 0;
        for (int iy = 0; iy < 24; ++iy) {
            const float v = (float(iy) + 0.5f) / 24.0f;
            WorldExtendSample prev = world_ray_extend(wc, 0.5f / N, v);
            for (int ix = 1; ix < N; ++ix) {
                const auto s = world_ray_extend(wc, (float(ix) + 0.5f) / N, v);
                const double st = std::max({double(std::fabs(s.u - prev.u)), double(std::fabs(s.v - prev.v)), double(std::fabs(s.k - prev.k))});
                if (st > worst) { worst = st; if (std::getenv("ACZ_WH_DEBUG")) std::fprintf(stderr, "row v=%g u=%g: prev(%g,%g,%g ext%d ang%g) now(%g,%g,%g ext%d ang%g)\n", v, (float(ix) + 0.5f) / N, prev.u, prev.v, prev.k, prev.ext, prev.ang, s.u, s.v, s.k, s.ext, s.ang); }
                prev = s;
            }
        }
        for (int ix = 0; ix < 24; ++ix) {
            const float u = (float(ix) + 0.5f) / 24.0f;
            WorldExtendSample prev = world_ray_extend(wc, u, 0.5f / N);
            for (int iy = 1; iy < N; ++iy) {
                const auto s = world_ray_extend(wc, u, (float(iy) + 0.5f) / N);
                worst = std::max({worst, double(std::fabs(s.u - prev.u)), double(std::fabs(s.v - prev.v)), double(std::fabs(s.k - prev.k))});
                prev = s;
            }
        }
        check(worst < 0.03, "edge continuity (steps < 0.03 uv per 0.08 deg; a real seam is >= 0.1): largest neighbour step " + std::to_string(worst) + " (yaw " + std::to_string(yaw) + ", pitch " + std::to_string(pitch) + ")");
    }
    // 4. Window edge: the periphery sample just outside an edge is the edge pixel and the light is still ~1.
    { WorldComposite wc{}; check(world_composite_params(eye_at(anchor, 0.0, 0.0, hmd), anchor, 0.0f, sym, 500.0f, wc), "params");
      double u = 0, v = 0; uv_for_tan(hmd, 0.6152 * 1.002, 0.0, u, v);
      const auto s = world_ray_extend(wc, float(u), float(v));
      check(s.ext && s.u > 0.995f && s.v > 0.49f && s.v < 0.51f && s.k > 0.99f, "just outside the right edge: the edge column, light ~1");
      uv_for_tan(hmd, 0.0, 0.346 * 1.002, u, v);
      const auto t = world_ray_extend(wc, float(u), float(v));
      check(t.ext && t.v < 0.005f && t.u > 0.49f && t.u < 0.51f && t.k > 0.99f, "just above the top edge: the top row, light ~1"); }
    // 5. Direction cases (exact axes, behind, corners): finite, clamped, no NaN / Inf, the right edge.
    { struct Dir { double x, y, z; } dirs[] = {{0, 0, -1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0.001, 0, 1}, {-0.001, 0, 1}, {0.3, 0.2, 0.9}, {1, 1, 1}, {-1, 1, -1},
          {-1, -1, 1}, {1e-9, 1, 1e-9}, {0, 1e-9, 1}, {5, 0, -1e-7}, {5, 0, -1e-6}, {5, 0, -2e-6}, {0, 3, -1e-6}, {1e-12, 1e-12, 1e-12}, {0, 1e6, 1}, {1e6, 0, 0}};
      bool all = true; std::string bad;
      for (const auto& d : dirs) {
          const auto wc = dir_composite(d.x, d.y, d.z);
          const auto s = world_ray_extend(wc, 0.3f, 0.7f);
          if (!sample_ok(s)) { all = false; bad += " (" + std::to_string(d.x) + "," + std::to_string(d.y) + "," + std::to_string(d.z) + ")"; }
      }
      check(all, "directions incl. axes, behind, near-zero and huge components: finite, uv in [0,1], k in [min,1]:" + bad);
      // eye past the plane (the plane is behind the eye): angular fallback, still finite
      const double beyond[3] = {0.0, 0.0, -600.0};
      const auto wcb = dir_composite(0.2, 0.1, -1.0, 500.0, beyond);
      check(sample_ok(world_ray_extend(wcb, 0.5f, 0.5f)) && world_ray_extend(wcb, 0.5f, 0.5f).ext, "eye past the plane: periphery, finite");
      // looking straight up / down: the top / bottom row at the middle column, strongly darkened
      auto up = world_ray_extend(dir_composite(0, 1, 0), 0.5f, 0.5f), dn = world_ray_extend(dir_composite(0, -1, 0), 0.5f, 0.5f);
      check(up.ext && up.v == 0.0f && std::fabs(up.u - 0.5f) < 1e-4f && up.k < 0.62f && up.k > 0.5f, "straight up: top row, middle column, darkened");
      check(dn.ext && dn.v == 1.0f && std::fabs(dn.u - 0.5f) < 1e-4f && dn.k < 0.62f && dn.k > 0.5f, "straight down: bottom row, middle column, darkened");
      auto rt = world_ray_extend(dir_composite(1, 0, 0), 0.5f, 0.5f), lf = world_ray_extend(dir_composite(-1, 0, 0), 0.5f, 0.5f);
      check(rt.u == 1.0f && lf.u == 0.0f && std::fabs(rt.v - 0.5f) < 1e-4f && std::fabs(lf.v - 0.5f) < 1e-4f && rt.k < 0.8f && rt.k > 0.5f && std::fabs(rt.k - lf.k) < 1e-6f, "90 deg right / left: the side edge columns at the horizon, darker (k ~0.69: 58 deg from the edge)");
      auto bh = world_ray_extend(dir_composite(0.01, 0, 1), 0.5f, 0.5f), bh2 = world_ray_extend(dir_composite(-0.01, 0, 1), 0.5f, 0.5f);
      check(bh.u == 1.0f && bh2.u == 0.0f && bh.k < 0.40f && bh.k >= kExtendMinLight, "straight behind: the edge column of its side, darkest region, above the floor"); }
    // 6. Symmetry: x mirrored => u mirrored, v and k equal (symmetric lens, origin 0); y mirrored => v mirrored, u and k equal.
    { struct Dir { double x, y, z; } dirs[] = {{0.3, 0.2, 0.9}, {0.7, 0.9, -0.4}, {0.9, 0.1, -0.5}, {0.4, 0.8, 0.3}, {0.1, 0.5, -0.9}, {0.8, -0.6, 0.2}, {0.2, 3.0, -0.1}};
      bool sx = true, sy = true;
      for (const auto& d : dirs) {
          const auto a = world_ray_extend(dir_composite(d.x, d.y, d.z), 0.5f, 0.5f), b = world_ray_extend(dir_composite(-d.x, d.y, d.z), 0.5f, 0.5f);
          sx = sx && std::fabs(a.u - (1.0f - b.u)) < 1e-5f && std::fabs(a.v - b.v) < 1e-5f && std::fabs(a.k - b.k) < 1e-5f;
          const auto c = world_ray_extend(dir_composite(d.x, -d.y, d.z), 0.5f, 0.5f);
          sy = sy && std::fabs(a.v - (1.0f - c.v)) < 1e-5f && std::fabs(a.u - c.u) < 1e-5f && std::fabs(a.k - c.k) < 1e-5f;
      }
      check(sx, "left/right symmetry of the periphery fill"); check(sy, "up/down symmetry of the periphery fill"); }
    // 7. Darkening grows with the angle away from the window (straight up the middle, 19.2 deg .. 89 deg), uv stays on the top row; the blur contracts the tangential axis.
    { float prevk = 2.0f, prevang = -1.0f; bool mono = true, row = true;
      for (double p = 19.2; p <= 89.0; p += 0.5) {
          const double a = p * kPi / 180.0;
          const auto s = world_ray_extend(dir_composite(0, std::sin(a), -std::cos(a)), 0.5f, 0.5f);
          mono = mono && s.k <= prevk && s.ang >= prevang; row = row && s.ext && s.v == 0.0f && std::fabs(s.u - 0.5f) < 1e-4f;
          prevk = s.k; prevang = s.ang;
      }
      check(mono && row, "elevation sweep: k non-increasing, angle increasing, sample stays on the top row at the middle column");
      const double az = 28.0 * kPi / 180.0, el = 60.0 * kPi / 180.0;
      const auto s = world_ray_extend(dir_composite(std::cos(el) * std::sin(az), std::sin(el), -std::cos(el) * std::cos(az)), 0.5f, 0.5f);
      check(s.ext && s.v == 0.0f && s.u > 0.5f && s.u < 0.7f && s.k < 0.9f && s.k > 0.5f, "above the window at 28 deg azimuth: the top row, contracted toward the middle column (blur), darkened"); }
    // 8. Randomised: no NaN / Inf, uv in [0,1], over random poses, lenses and distances.
    { unsigned seed = 12345; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return double(seed >> 8) / double(1u << 24); };
      bool ok = true; unsigned n = 0;
      for (int i = 0; i < 4000; ++i) {
          const double yaw = (rnd() - 0.5) * 2 * kPi, pitch = (rnd() - 0.5) * 3.0;
          const float D = 5.0f + float(rnd()) * 1995.0f;
          const float pos[3] = {float((rnd() - 0.5) * 2), float(1.0 + rnd()), float((rnd() - 0.5) * 4)};
          const xr_owner::Fov f{float(-0.3 - rnd() * 1.1), float(0.3 + rnd() * 1.1), float(0.3 + rnd() * 1.1), float(-0.3 - rnd() * 1.1)};
          WorldComposite wc{};
          if (!world_composite_params(eye_at(pos, yaw, pitch, f), anchor, float((rnd() - 0.5) * 2 * kPi), sym, D, wc)) continue;
          ++n;
          for (int k = 0; k < 8; ++k) ok = ok && sample_ok(world_ray_extend(wc, float(rnd()), float(rnd())));
      }
      check(ok && n > 3500, "random poses: all samples finite, clamped, in range (" + std::to_string(n) + " poses)"); }
}

void world_hold_producer_helpers() {
    namespace s5 = acz::gpu_copy::step5;
    check(s5::entry_subs == 11 && std::string(s5::entry_sub_name(0)) == "no-transition-tracked" && std::string(s5::entry_sub_name(10)) == "capture-incomplete" && std::string(s5::entry_sub_name(11)) == "?", "entry sub names");
    for (unsigned i = 0; i < s5::entry_subs; ++i) check(s5::entry_sub_index(s5::entry_sub_name(i)) == i, "entry sub index round trip");
    check(s5::entry_sub_index(nullptr) == 10 && s5::entry_sub_index("something-new") == 9, "null = capture-incomplete, unknown = unclassified");
    double deg = 99;
    const float ident[4] = {0, 0, 0, 1};
    check(s5::head_pitch_deg(ident, deg) && std::fabs(deg) < 1e-9, "identity: pitch 0");
    for (double p : {-80.0, -30.0, 10.0, 30.0, 89.0}) {
        const double a = p * kPi / 180.0; // OpenXR: positive rotation about +X looks UP
        const float q[4] = {float(std::sin(a / 2)), 0, 0, float(std::cos(a / 2))};
        check(s5::head_pitch_deg(q, deg) && std::fabs(deg - p) < 0.01, "pitch " + std::to_string(p));
        const double yaw = 1.1; // a heading does not change the pitch
        const double qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, qx[4] = {std::sin(a / 2), 0, 0, std::cos(a / 2)};
        double r[4]; quat_mul(qy, qx, r);
        const float qq[4] = {float(r[0]), float(r[1]), float(r[2]), float(r[3])};
        check(s5::head_pitch_deg(qq, deg) && std::fabs(deg - p) < 0.01, "pitch with a heading " + std::to_string(p));
    }
    const float zero[4] = {0, 0, 0, 0}, nan[4] = {std::nanf(""), 0, 0, 1};
    check(!s5::head_pitch_deg(zero, deg) && !s5::head_pitch_deg(nan, deg) && !s5::head_pitch_deg(nullptr, deg), "unsound quaternion refused");
    s5::PitchStat st;
    const double f1[3] = {0.1, 0.9, -0.2}, f2[3] = {-0.3, 0.2, 0.5};
    st.add(true, 12.5, true, f1); st.add(true, -40.0, true, f2); st.add(false, 0, false, nullptr); st.add(true, std::nan(""), true, f1);
    check(st.n == 4 && st.nHead == 2 && st.nCam == 3 && st.headMin == -40.0 && st.headMax == 12.5 && st.fwdMin[0] == -0.3 && st.fwdMax[1] == 0.9 && st.fwdMax[2] == 0.5 && st.fwdMin[2] == -0.2, "PitchStat min/max");
}

void world_hold_config_keys() {
    using namespace acz::diagnostics;
    check(!parse_config("xr_stereo_output=true\n").config.xr_world_hold_last() && !parse_config("xr_stereo_output=true\n").config.xr_world_extend(), "hold / extend default OFF");
    check(parse_config("xr_world_hold_last=true\n").config.xr_world_hold_last() && parse_config("xr_world_hold_last=1\n").config.xr_world_hold_last(), "hold ON: true / 1");
    check(parse_config("xr_world_extend=true\n").config.xr_world_extend() && parse_config("xr_world_extend=1\n").config.xr_world_extend(), "extend ON: true / 1");
    check(!parse_config("xr_world_hold_last=false\n").config.xr_world_hold_last() && !parse_config("xr_world_extend=0\n").config.xr_world_extend(), "OFF: false / 0");
    check(parse_config("xr_world_hold_last=yes\n").issue == ConfigIssue::out_of_range && parse_config("xr_world_extend=2\n").issue == ConfigIssue::out_of_range, "malformed values rejected");
    check(parse_config("xr_world_extend=true\nxr_world_extend=true\n").issue == ConfigIssue::duplicate, "duplicate rejected");
    // J-occlusion-fix: key xr_world_window_occlude (default off; true/1/false/0; anything else out of range; duplicate rejected); 26 of 32 keys fit the byte cap.
    check(!parse_config("xr_stereo_output=true\n").config.xr_world_window_occlude() && parse_config("xr_world_window_occlude=true\n").config.xr_world_window_occlude() &&
          parse_config("xr_world_window_occlude=1\n").config.xr_world_window_occlude() && !parse_config("xr_world_window_occlude=0\n").config.xr_world_window_occlude() &&
          parse_config("xr_world_window_occlude=yes\n").issue == ConfigIssue::out_of_range && parse_config("xr_world_window_occlude=true\nxr_world_window_occlude=true\n").issue == ConfigIssue::duplicate,
          "occlude key parse");
    check(xr_stereo::pack_mode_bits(2, 0) == 2u && (xr_stereo::pack_mode_bits(2, 3) | xr_stereo::kModeBitOcclude) == (2u | (3u << 8) | 65536u) && xr_stereo::kModeBitOcclude == 65536u, "occlude bit 16 does not collide with mode / debug view");
    // The whole headset set (21 keys of test 7 + hold + extend = 23 keys, 24 with the world observer) fits the 24-key / 2048-byte config.
    const char* keys[] = {"log_enabled=true", "diagnostic_ownership_pins=true", "gpu_copy_witness_observation=true", "gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true", "gpu_copy_step3_whole_set=true", "gpu_copy_step5_continuous=true", "xr_stereo_output=true", "xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true", "gpu_copy_step5_hmd_fov=true", "gpu_copy_step5_hud=true", "xr_hud_layer=true", "xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5", "xr_world_quad_distance_m=500", "gpu_copy_step5_units_per_metre=65", "gpu_copy_step5_hud_markers=true", "xr_hud_markers_layer=true",
        "xr_world_in_projection=true", "gpu_copy_step5_world_objects=true", "xr_world_hold_last=true", "xr_world_extend=true", "gpu_copy_step5_world_observe=true"};
    std::string cfg; for (const char* k : keys) cfg += std::string(k) + "\n";
    const auto full = parse_config(cfg);
    check(full.issue == ConfigIssue::none && full.config.xr_world_hold_last() && full.config.xr_world_extend() && full.config.xr_world_in_projection() &&
        std::size(keys) == 24 && std::size(keys) <= max_config_keys && cfg.size() <= max_config_bytes && max_config_keys == 32, "the full headset set with hold + extend + observer parses (24 keys, <= 2048 bytes)");
}

namespace {
// Advance the fake clock by `ms` at every begun frame (on top of the rig's own wait_frame hook).
void clock_per_frame(Rig& g, int64_t ms) {
    auto inner = g.xr->hook["wait_frame"];
    g.xr->hook["wait_frame"] = [&g, inner, ms] { g.clock->t += ms; if (inner) inner(); };
}
}

void world_hold_runs() {
    using namespace xr_stereo;
    // Frames 8..47 carry no world (a refusal burst); frames before and after do. 25 ms per begun frame: the gap is 1 s, the hold is 750 ms.
    auto make = [](bool hold, bool extend, bool projection) {
        auto g = std::make_unique<Rig>();
        g->stop_after = 80; hud_full_rig(*g); g->settings.world_in_projection = projection; g->settings.world_hold_last = hold; g->settings.world_extend = extend;
        g->src.world_gap_from = 8; g->src.world_gap_to = 47; g->src.with_neutral = true; g->settings.quad_distance_m = 500.0f;
        g->build(); clock_per_frame(*g, 25);
        return g;
    };
    { auto gp = make(true, false, true); auto& g = *gp; auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "hold run succeeds");
      check(c.world_captured > 0 && c.world_held > 0 && c.world_hold_expired > 0 && c.world_hold_capture_failed == 0, "hold counters: captured, held, expired");
      check(c.world_held < 40 && c.world_held >= 15, "held for ~750 ms of the 1 s gap, then expired (" + std::to_string(c.world_held) + " frames held)");
      unsigned held_jobs = 0, capture_jobs = 0, no_world_jobs = 0; bool full_first = true, mono = true, fresh = true; float prev = 1.0f; bool in_gap = false;
      for (const auto& j : g.blit->jobs) {
          if (!j.use_world) { ++no_world_jobs; check(!j.world_composite && !j.world_held && !j.world_capture, "past the hold window: a plain no-world recording"); continue; }
          check(j.world_composite, "every other recording composites a world (fresh or held)");
          if (j.world_held) {
              ++held_jobs; in_gap = true;
              check(j.world.resource == nullptr && !j.world_capture, "a held recording has no producer world resource and does not capture");
              mono = mono && j.world_fade <= prev; prev = j.world_fade;
              if (held_jobs == 1) full_first = j.world_fade == 1.0f;
              check(j.world_fade >= 0.0f && j.world_fade <= 1.0f, "fade in range");
          } else {
              check(j.world.resource != nullptr, "a fresh recording carries the producer world");
              fresh = fresh && j.world_fade == 1.0f && j.world_capture;
              capture_jobs += j.world_capture;
              if (in_gap) { prev = 1.0f; in_gap = false; }
          }
      }
      check(held_jobs == c.world_held && capture_jobs == c.world_captured && full_first && mono && fresh, "held jobs start at full strength and fade monotonically; fresh jobs capture at fade 1");
      // The held jobs use the lens of the captured frame and the live frame's own eye poses (composite params from the held lens).
      check(no_world_jobs > 0 && g.blit->jobs.size() == c.world_captured + c.world_held + no_world_jobs && no_world_jobs >= c.world_hold_expired - 0, "frames past the hold window are recorded without a world (" + std::to_string(no_world_jobs) + ")");
      check(g.has_line("xr-stereo;event=world-hold;key=xr_world_hold_last;hold-ms=750;") && g.has_line("xr-stereo;event=summary-world-hold;") &&
          g.has_line("xr-stereo;event=world-hold-result;key=xr_world_hold_last;captured="), "hold log lines");
      check(g.has_line("shader=ps_main-mode2;constants=24;"), "setup line reports the 24-constant layout when a key is on");
      check(g.has_line("xr-stereo;event=layer-fov;") && g.has_line("px-per-tan-ratio-x-over-y="), "layer-fov line present");
      for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "hold lines fit the router: " + line);
      // After the hold expired the frame is submitted without a world (depth-coverage rules, as key-off): composite flag off on those layers.
      bool saw_noworld = false;
      for (auto& l : g.xr->layers) saw_noworld = saw_noworld || !l.world_composite;
      check(saw_noworld, "frames past the hold window are submitted without a world"); }
    // Hold ON but no gap: nothing is held, every frame captures; the output is as key-off apart from the capture request.
    { Rig g; g.stop_after = 20; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.world_hold_last = true; g.build(); auto r = g.run();
      check(r.success && g.out->counters().world_held == 0 && g.out->counters().world_hold_expired == 0 && g.out->counters().world_captured == g.out->counters().world_composite_frames, "no refusals: nothing held, every composite frame captured"); }
    // Hold key without the in-projection world is inert (forced off); the key-off request is bitwise the old one.
    { Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_hold_last = true; g.settings.world_extend = true; g.src.world_gap_from = 4; g.src.world_gap_to = 9; g.build(); auto r = g.run();
      check(r.success, "inert-key run");
      for (auto& j : g.blit->jobs) check(!j.world_capture && !j.world_held && !j.world_extend && j.world_fade == 1.0f && !j.world_composite, "keys inert without xr_world_in_projection");
      check(!g.has_line("event=world-hold") && !g.has_line("event=world-extend") && !g.has_line("event=summary-world-hold"), "no hold / extend lines when inert"); }
    // Key OFF with a gap (the real refusal pattern): behaviour is exactly the existing one -> frames in the gap have no world, nothing held.
    { auto gp = make(false, false, true); auto& g = *gp; auto r = g.run();
      check(r.success && g.out->counters().world_held == 0 && g.out->counters().world_captured == 0 && g.out->counters().world_hold_expired == 0, "hold off: no hold counters in the gap");
      for (auto& j : g.blit->jobs) check(!j.world_capture && !j.world_held && !j.world_extend && j.world_fade == 1.0f, "hold off: recordings carry no hold request");
      check(!g.has_line("event=world-hold") && !g.has_line("event=summary-world-hold") && !g.has_line("event=world-hold-result"), "hold off: no hold lines");
      check(g.has_line("shader=ps_main-mode2;constants=22;"), "key off: the setup line is the legacy one (constants=22)"); }
    // Extend: the request flag rides on the composite recordings only.
    { auto gp = make(false, true, true); auto& g = *gp; auto r = g.run();
      check(r.success && !g.out->failed(), "extend run");
      unsigned n = 0; for (auto& j : g.blit->jobs) { if (!j.use_world) { check(!j.world_extend && !j.world_composite, "extend: a no-world recording carries no extend request"); continue; } check(j.world_composite && j.world_extend && !j.world_capture && !j.world_held && j.world_fade == 1.0f, "extend: composite recordings carry extend only"); ++n; }
      check(n > 0 && g.has_line("xr-stereo;event=world-extend;key=xr_world_extend;"), "extend log line"); }
    // J-occlusion-fix (xr_world_window_occlude): the request rides on the composite recordings that also carry depth; key off / no projection = never.
    { Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.world_window_occlude = true; g.src.with_neutral = true; g.build(); auto r = g.run();
      check(r.success && !g.out->failed(), "occlude run");
      unsigned n = 0, nd = 0; for (auto& j : g.blit->jobs) { check(j.occlude_window == (j.world_composite && j.use_depth), "occlude: flag only on composite recordings with depth"); n += j.occlude_window; nd += j.world_composite && j.use_depth; }
      check(n == nd && g.has_line("xr-stereo;event=window-occlude;key=xr_world_window_occlude;"), "occlude request + log line"); }
    { Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_window_occlude = true; g.build(); g.run();
      for (auto& j : g.blit->jobs) check(!j.occlude_window, "occlude inert without xr_world_in_projection");
      check(!g.has_line("event=window-occlude"), "no occlude line when inert"); }
    { Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.src.with_neutral = true; g.build(); g.run();
      for (auto& j : g.blit->jobs) check(!j.occlude_window, "occlude key off: never requested");
      check(!g.has_line("event=window-occlude"), "key off: no occlude line"); }
    // Hold + a held frame whose composite views cannot be resolved falls back to no world (counted refused), never a sticky failure.
    { Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.world_hold_last = true; g.src.world_gap_from = 6; g.src.world_gap_to = 20;
      g.src.with_neutral = true; g.build(); clock_per_frame(g, 20); auto r = g.run();
      check(r.success && !g.out->failed() && g.out->counters().world_held > 0, "hold: held frames do not fail the output"); }
    // Backend reports the capture failed: the hold is invalid, the following gap frames are expired not held.
    { auto gp = make(true, false, true); auto& g = *gp; g.blit->capture_ok = false; auto r = g.run();
      const auto& c = g.out->counters();
      check(r.success && c.world_captured == 0 && c.world_held == 0 && c.world_hold_capture_failed > 0 && c.world_hold_expired > 0, "capture refused by the backend: nothing is ever held"); }
}

void world_layer_fov_line() {
    // layer-fov: the submitted fov, the swapchain and the pixels-per-tan anisotropy (test 7: 1030 vs 587 px/tan for the headset fov in a 16:9 swapchain).
    Rig g; g.stop_after = 8; hud_full_rig(g); g.src.cockpit_fov = true;
    g.src.cfov[0][0] = -0.95909f; g.src.cfov[0][1] = 0.73519f; g.src.cfov[0][2] = 0.76586f; g.src.cfov[0][3] = -0.92861f;
    g.src.cfov[1][0] = -0.8f; g.src.cfov[1][1] = 0.92f; g.src.cfov[1][2] = 0.91f; g.src.cfov[1][3] = -0.86f;
    g.build(); auto r = g.run();
    check(r.success && g.has_line("xr-stereo;event=layer-fov;swapchain=2560x1440;cockpit-fov=hmd;eye0-fov-deg=-54.95,42.12,43.88,-53.21;"), "layer-fov: swapchain and the submitted fov (degrees)");
    const double sx = std::tan(0.73519) - std::tan(-0.95909), sy = std::tan(0.76586) - std::tan(-0.92861);
    char want[200];
    std::snprintf(want, sizeof want, "eye0-tan-span=%.4fx%.4f;eye0-tan-aspect=%.4f;eye0-px-per-tan=%.1fx%.1f;eye0-px-per-tan-ratio-x-over-y=%.3f", sx, sy, sx / sy, 2560.0 / sx, 1440.0 / sy, (2560.0 / sx) / (1440.0 / sy));
    check(g.has_line(want), std::string("layer-fov: tan span, aspect, px-per-tan and the anisotropy ratio: ") + want);
    check(g.has_line(";eye1-px-per-tan=") && g.has_line(";pixel-aspect=1.7778;no-headset-claim"), "layer-fov: second eye and the pixel aspect");
    for (auto& line : g.lines) check(line.size() < feature_log::line_cap, "layer-fov line fits the router: " + line);
    // game lens fov (no cockpit fov): isotropic density
    Rig h; h.stop_after = 8; hud_full_rig(h); h.build(); auto rh = h.run();
    check(rh.success && h.has_line("eye0-px-per-tan-ratio-x-over-y=1.000;"), "lens fov: isotropic (ratio 1.000)");
}

// ============================================================ timing / anchor fixtures ===
// docs/review/timing-anchor-fixtures.md. Offline only: the real owner + consumer against the fake XR / D3D12 / producer. These cases
// establish what the CONSUMER submits for a published frame whose render pose P0, recorded look L0 and metadata differ from the
// CURRENT located pose P1 (which moves every XR frame); they say nothing about game or headset behaviour and are not timing measurements.
namespace {
const float kP0Fov[2][4] = {{-0.70f, 0.55f, 0.50f, -0.62f}, {-0.55f, 0.70f, 0.50f, -0.62f}};
stereo::EyePose p0_eye(unsigned e, float dx = 0.0f, float dz = 0.0f) {
    stereo::EyePose p{};
    p.position[0] = (e ? 0.07f : -0.07f) + dx; p.position[1] = 1.55f; p.position[2] = 0.30f + dz;
    p.orientation[1] = std::sin(0.15f); p.orientation[3] = std::cos(0.15f); // yaw 0.3 rad about +Y
    for (int i = 0; i < 4; ++i) p.fov[i] = kP0Fov[e][i];
    return p;
}
xr_owner::EyeSample eye_sample(const stereo::EyePose& e) {
    xr_owner::EyeSample s;
    for (int i = 0; i < 3; ++i) s.pose.position[i] = e.position[i];
    for (int i = 0; i < 4; ++i) s.pose.orientation[i] = e.orientation[i];
    s.fov = {e.fov[0], e.fov[1], e.fov[2], e.fov[3]};
    return s;
}
bool pose_is(const xr_owner::Pose& p, const stereo::EyePose& e) {
    return std::memcmp(p.position, e.position, sizeof p.position) == 0 && std::memcmp(p.orientation, e.orientation, sizeof p.orientation) == 0;
}
// The located pose P1 changes at every wait (position, yaw and FOV all differ from P0); recorded when the owner locates it.
struct Located { std::vector<std::array<xr_owner::EyeSample, 2>> seen; };
void install_moving_located(Rig& g, Located& loc) {
    auto inner = g.xr->hook["wait_frame"];
    g.xr->hook["wait_frame"] = [&g, inner] {
        const unsigned w = g.xr->waits;
        const double yaw = 1.1 + 0.07 * w;
        for (unsigned e = 0; e < 2; ++e) {
            auto& v = g.xr->views.eyes[e];
            v.pose.position[0] = (e ? 0.032f : -0.032f) + 0.02f * float(w); v.pose.position[1] = 1.2f + 0.01f * float(w); v.pose.position[2] = -0.05f * float(w);
            v.pose.orientation[0] = 0; v.pose.orientation[1] = float(std::sin(yaw / 2)); v.pose.orientation[2] = 0; v.pose.orientation[3] = float(std::cos(yaw / 2));
            v.fov = {-0.9f, 0.8f, 0.85f, -0.95f};
        }
        if (inner) inner();
    };
    g.xr->hook["locate_views"] = [&g, &loc] { loc.seen.push_back(g.xr->views.eyes); };
}
bool same_eye(const xr_owner::EyeSample& a, const xr_owner::EyeSample& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
std::string tok(const std::string& line, const std::string& key) { // value of ";key=" up to the next ';'
    const std::string k = ";" + key + "=";
    const size_t at = line.find(k);
    if (at == std::string::npos) return "<missing>";
    const size_t b = at + k.size(), e = line.find(';', b);
    return line.substr(b, e == std::string::npos ? std::string::npos : e - b);
}
std::vector<std::string> corr_lines(const Rig& g) {
    std::vector<std::string> out;
    for (auto& l : g.lines) if (l.find("event=select-corr") != std::string::npos) out.push_back(l);
    return out;
}
}

// A. Cockpit timing: the projection layer and the world composite use the pose/FOV/look the frame was RENDERED with (P0, L0), never the
// located pose P1 of the XR frame that happens to submit it. Early-world adoption and cockpit-first frames are separate runs.
void timing_cockpit_keeps_render_pose() {
    using namespace xr_stereo;
    for (int mode = 0; mode < 3; ++mode) { // 0 lens FOV, 1 per-eye cockpit FOV, 2 neither (the pose's own FOV; no world)
        std::vector<ProjectionLayer> first_layers; std::vector<std::array<WorldComposite, 2>> first_jobs;
        for (int start : {1, 2, 3}) { // diag_start: 1 cockpit-first, 2 early-world adopted, 3 early-world unadopted (metadata only for the consumer)
            Rig g; g.stop_after = 14; hud_full_rig(g);
            if (mode == 2) { g.src.lens_valid = false; g.src.with_world = false; }
            g.settings.world_in_projection = mode != 2; g.settings.quad_distance_m = 500.0f;
            g.src.with_neutral = true; g.src.nyaw = 0.4f; g.src.cockpit_fov = mode == 1;
            g.src.mutate = [&g, start](stereo::StereoFrame& f) {
                f.pose[0] = p0_eye(0); f.pose[1] = p0_eye(1);
                f.look_valid = true; f.look_yaw = 0.5f; f.look_pitch = 0.2f; // L0 (nonzero)
                f.diag_valid = true; f.diag_start = uint8_t(start); f.diag_look_src = start == 1 ? 1 : 2;
            };
            Located loc; g.build(); install_moving_located(g, loc);
            auto r = g.run();
            const std::string what = "mode " + std::to_string(mode) + " start " + std::to_string(start);
            const auto& c = g.out->counters();
            check(r.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0 && !g.xr->layers.empty(), "run succeeds: " + what);
            check(c.pose_from_frame == c.new_frames && c.pose_from_located == 0 && c.new_frames > 0, "every fresh layer takes the frame's pose, never the located one: " + what);
            // Fixture sanity: P1 really differs from P0 at the XR frames that submit layers.
            check(!loc.seen.empty(), "located views were sampled: " + what);
            for (auto& l : loc.seen) for (unsigned e = 0; e < 2; ++e)
                check(!pose_is(l[e].pose, p0_eye(e)) && !fov_is(l[e].fov, kP0Fov[e]), "P1 differs from P0 (fixture): " + what);
            const xr_owner::Fov lensf = fov_from_lens(g.src.lens);
            for (auto& l : g.xr->layers) for (unsigned e = 0; e < 2; ++e) {
                check(pose_is(l.views[e].pose, p0_eye(e)), "projection pose is the render pose P0: " + what);
                for (auto& seen : loc.seen) check(!same_pose(l.views[e].pose, seen[e].pose), "projection pose is not any located P1: " + what);
                if (mode == 0) check(std::memcmp(&l.views[e].fov, &lensf, sizeof lensf) == 0, "FOV is the game lens FOV of the render (not the located FOV): " + what);
                if (mode == 1) check(fov_is(l.views[e].fov, g.src.cfov[e]), "FOV is the per-eye cockpit FOV of the render: " + what);
                if (mode == 2) check(fov_is(l.views[e].fov, kP0Fov[e]), "FOV is the pose's own FOV: " + what);
                check(!fov_is(l.views[e].fov, std::array<float, 4>{-0.9f, 0.8f, 0.85f, -0.95f}.data()), "FOV is never the located FOV: " + what);
            }
            if (mode < 2) {
                check(!g.blit->jobs.empty() && g.blit->jobs.size() == c.new_frames, "one composite recording per fresh frame: " + what);
                const float* lens = g.src.lens;
                for (auto& j : g.blit->jobs) {
                    check(j.world_composite && j.use_world && !j.world_held, "fresh world composite: " + what);
                    for (unsigned e = 0; e < 2; ++e) {
                        xr_owner::EyeSample v = eye_sample(p0_eye(e));
                        v.fov = g.xr->layers.front().views[e].fov; // exactly the FOV the layer carries
                        WorldComposite want{}, no_look{}, at_p1{};
                        check(world_composite_params(v, g.src.npos, g.src.nyaw + 0.5f, lens, 500.0f, want, 0.2f), "reference params (P0, L0)");
                        check(std::memcmp(&want, &j.world_eye[e], sizeof want) == 0, "world composite uses P0 and the recorded look L0: " + what);
                        check(world_composite_params(v, g.src.npos, g.src.nyaw, lens, 500.0f, no_look) && std::memcmp(&no_look, &j.world_eye[e], sizeof want) != 0,
                            "...and not look 0: " + what);
                        xr_owner::EyeSample p1 = loc.seen.back()[e]; p1.fov = v.fov;
                        check(world_composite_params(p1, g.src.npos, g.src.nyaw + 0.5f, lens, 500.0f, at_p1) && std::memcmp(&at_p1, &j.world_eye[e], sizeof want) != 0,
                            "...and not the located pose P1: " + what);
                    }
                }
            }
            // Early-world adoption / cockpit-first differ only in producer metadata: placement, poses and recordings are bit-identical.
            std::vector<std::array<WorldComposite, 2>> jobs;
            for (auto& j : g.blit->jobs) jobs.push_back(j.world_eye);
            if (start == 1) { first_layers = g.xr->layers; first_jobs = jobs; }
            else {
                check(first_layers.size() == g.xr->layers.size() && first_jobs.size() == jobs.size(), "same number of layers/recordings as cockpit-first: " + what);
                for (size_t i = 0; i < first_layers.size() && i < g.xr->layers.size(); ++i) check(same_layer(first_layers[i], g.xr->layers[i]), "layer identical to cockpit-first: " + what);
                for (size_t i = 0; i < first_jobs.size() && i < jobs.size(); ++i) check(std::memcmp(&first_jobs[i], &jobs[i], sizeof first_jobs[i]) == 0, "recording identical to cockpit-first: " + what);
            }
        }
    }
}

// A2. Reused layers keep their ORIGINAL metadata while the located pose and the XR display time move on.
void timing_reused_layers_keep_metadata() {
    using namespace xr_stereo;
    Rig g; g.stop_after = 20; hud_full_rig(g); g.settings.hud_layer = true; g.src.with_hud = true; g.settings.quad_distance_m = 40.0f;
    g.src.with_neutral = true; g.src.nyaw = 0.4f; g.publish_until = 4; // frames 1..4 fresh, then the producer goes quiet
    g.src.mutate = [](stereo::StereoFrame& f) { f.pose[0] = p0_eye(0); f.pose[1] = p0_eye(1); f.look_valid = true; f.look_yaw = 0.5f; f.look_pitch = 0.2f; };
    Located loc; g.build(); install_moving_located(g, loc);
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && !g.out->failed() && c.new_frames == 4 && c.reused > 5 && g.xr->layers.size() > 6, "4 fresh layers then re-submissions");
    const size_t fresh = size_t(c.new_frames);
    const ProjectionLayer& last_fresh = g.xr->layers[fresh - 1];
    for (size_t i = fresh; i < g.xr->layers.size(); ++i) {
        const auto& l = g.xr->layers[i];
        check(same_layer(l, last_fresh), "re-submitted layer is the last fresh layer bit for bit (poses, FOV, world/HUD quads)");
        for (unsigned e = 0; e < 2; ++e) check(pose_is(l.views[e].pose, p0_eye(e)) && !same_pose(l.views[e].pose, loc.seen.back()[e].pose), "re-submitted pose is the ORIGINAL render pose, not the current located pose");
        check(g.xr->layer_times[i] > g.xr->layer_times[fresh - 1], "the XR display time advanced while the metadata did not");
    }
    // The world quad of the reused layers is the quad along L0 (recorded with the image), not along anything current.
    const Quad want = world_quad(g.src.npos, g.src.lens, 40.0f, g.src.nyaw + 0.5f, 0.2f);
    check(same_pose(g.xr->layers.back().quad_pose, want.pose), "re-submitted world quad still along the recorded look L0");
}

// A3. A held world keeps ITS OWN recorded look/lens while later frames (without a world) carry a different look; the cockpit stays on P0.
void timing_held_world_keeps_its_look() {
    using namespace xr_stereo;
    Rig g; g.stop_after = 80; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.world_hold_last = true; g.settings.quad_distance_m = 500.0f;
    g.src.world_gap_from = 8; g.src.world_gap_to = 47; g.src.with_neutral = true; g.src.nyaw = 0.4f;
    auto look_yaw = [](uint64_t id) { return 0.10f + 0.02f * float(id); };
    auto look_pitch = [](uint64_t id) { return 0.05f + 0.01f * float(id); };
    g.src.mutate = [&](stereo::StereoFrame& f) { f.pose[0] = p0_eye(0); f.pose[1] = p0_eye(1); f.look_valid = true; f.look_yaw = look_yaw(f.frame_id); f.look_pitch = look_pitch(f.frame_id); };
    Located loc; g.build(); clock_per_frame(g, 25); install_moving_located(g, loc);
    auto r = g.run();
    const auto& c = g.out->counters();
    check(r.success && !g.out->failed() && c.world_held > 5 && c.frame_id_gaps == 0 && g.blit->jobs.size() == c.new_frames, "hold run: every published frame consumed, some held");
    float hold_y = 0, hold_p = 0; unsigned held_seen = 0;
    for (size_t k = 0; k < g.blit->jobs.size(); ++k) {
        const auto& j = g.blit->jobs[k];
        const uint64_t id = k + 1;
        if (!j.use_world) continue;
        if (!j.world_held) { hold_y = look_yaw(id); hold_p = look_pitch(id); }
        else ++held_seen;
        for (unsigned e = 0; e < 2; ++e) {
            xr_owner::EyeSample v = eye_sample(p0_eye(e)); v.fov = fov_from_lens(g.src.lens); // the cockpit pose of THIS frame (P0), lens FOV
            WorldComposite want{}, own{};
            check(world_composite_params(v, g.src.npos, g.src.nyaw + hold_y, g.src.lens, 500.0f, want, hold_p) && std::memcmp(&want, &j.world_eye[e], sizeof want) == 0,
                j.world_held ? "held world uses the look it was captured with" : "fresh world uses its own look");
            if (j.world_held) check(world_composite_params(v, g.src.npos, g.src.nyaw + look_yaw(id), g.src.lens, 500.0f, own, look_pitch(id)) && std::memcmp(&own, &j.world_eye[e], sizeof want) != 0,
                "held world does NOT take the later frame's look");
        }
    }
    check(held_seen == c.world_held, "all held recordings checked");
    for (auto& l : g.xr->layers) for (unsigned e = 0; e < 2; ++e) check(pose_is(l.views[e].pose, p0_eye(e)), "cockpit stays on P0 through the hold");
}

// B. HUD / marker / world anchor contract (the CURRENT baseline, see the doc): look changing independently of the head, then head
// translation, then a recenter. head_locked=false: LOCAL world-quad HUD; true: VIEW-space HUD.
void anchor_contract_case(bool head_locked, float scale, float dist) {
    using namespace xr_stereo;
    const float n1[3] = {0.4f, 1.3f, -0.2f}, n2[3] = {2.0f, 2.7f, -3.0f};
    const float yaw1 = 0.4f, yaw2 = -1.2f;
    const unsigned kFrames = 24, kRecenterEvent = 10, kRecenterFrom = 14;
    Rig g; g.stop_after = kFrames; markers_rig(g);
    g.settings.hud_mode = head_locked ? HudMode::head_locked : HudMode::world_quad; g.settings.hud_scale = scale;
    if (head_locked) g.settings.hud_head_distance_m = dist; else g.settings.hud_quad_distance_m = dist; // dist 0 = unset (the world quad's distance)
    g.src.with_neutral = true; g.build();
    auto look_of_w = [](unsigned w, float& ly, float& lp) { // look script: changes at w=3 (head fixed), again at w=16
        ly = w < 3 ? 0.0f : w < 16 ? 0.6f : -0.9f; lp = w < 3 ? 0.0f : w < 16 ? 0.25f : -0.2f;
    };
    auto head_dx = [](unsigned w) { return w < 6 ? 0.0f : 0.3f; }; // head translation from w=6
    g.src.mutate = [&](stereo::StereoFrame& f) {
        const unsigned w = g.xr->waits;
        f.pose[0] = p0_eye(0, head_dx(w), 0.01f * float(w)); f.pose[1] = p0_eye(1, head_dx(w), 0.01f * float(w)); // z encodes the wait index w
        float ly, lp; look_of_w(w, ly, lp);
        f.look_valid = true; f.look_yaw = ly; f.look_pitch = lp;
        const bool gen2 = w >= kRecenterFrom;
        f.neutral_valid = true; std::memcpy(f.neutral_pos, gen2 ? n2 : n1, sizeof f.neutral_pos); f.neutral_yaw = gen2 ? yaw2 : yaw1;
    };
    g.xr->hook["wait_frame"] = [&g, kRecenterEvent, kRecenterFrom, kFrames] {
        const unsigned w = g.xr->waits;
        if (w == kRecenterEvent) { Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local; e.change_time = 1000000000 + (kRecenterEvent + 2) * g.xr->period; g.xr->events.push_back(e); }
        g.src.space_gen = w >= kRecenterFrom ? 2 : 1;
        g.src.publish();
        if (w + 1 >= kFrames) g.owner->request_stop();
    };
    auto r = g.run();
    const std::string what = std::string(head_locked ? "head-locked" : "world-quad") + " scale " + std::to_string(scale) + " dist " + std::to_string(dist);
    check(r.success && !g.out->failed() && g.xr->violations == 0 && g.src.bad_releases == 0 && g.xr->layers.size() >= kFrames - 4, "anchor run succeeds: " + what);
    check(g.out->counters().anchor_rebases == 1, "exactly one anchor rebase (the recenter): " + what);
    // Every submitted layer is checked against the script step that PRODUCED its images (recovered from the encoded render pose), so a
    // re-submitted layer around the recenter is checked as the frame it repeats.
    auto step_of = [](const ProjectionLayer& l) { return unsigned(std::lround((l.views[0].pose.position[2] - 0.30f) / 0.01f)); };
    const bool markers_on = !head_locked; // the marker layer is forced off in head-locked mode (no world alignment there)
    for (const auto& l : g.xr->layers) {
        const unsigned w = step_of(l);
        float ly, lp; look_of_w(w, ly, lp);
        const bool gen2 = w >= kRecenterFrom;
        const float* n = gen2 ? n2 : n1; const float yaw = gen2 ? yaw2 : yaw1;
        // World and marker quads: along the RECORDED look L, anchored at the generation's neutral, independent of the head.
        const Quad wq = world_quad(n, g.src.lens, 40.0f, yaw + ly, lp);
        check(l.quad && same_pose(l.quad_pose, wq.pose) && l.quad_width == wq.width && l.quad_height == wq.height, "world quad follows the recorded look L: " + what);
        if (markers_on) check(markers_register(l), "marker quad is the world quad (markers sit on the world rays): " + what);
        else check(!l.markers, "no marker layer in head-locked mode: " + what);
        if (head_locked) {
            const Quad hq = hud_head_quad(dist, 1.6f, scale, 2560, 1440);
            check(l.hud && l.hud_view_space && same_pose(l.hud_pose, hq.pose) && l.hud_width == hq.width && l.hud_height == hq.height, "head-locked HUD: fixed in VIEW space, scale/distance as configured: " + what);
            const auto list = compose_layers(l);
            check(list.e[list.count - 1].kind == LayerKind::hud_quad && list.e[list.count - 1].space == LayerSpace::view, "head-locked HUD layer is VIEW space (follows the head): " + what);
        } else {
            const Quad hq = hud_world_quad(n, g.src.lens, 40.0f, dist, scale, yaw);
            check(l.hud && !l.hud_view_space && same_pose(l.hud_pose, hq.pose) && l.hud_width == hq.width && l.hud_height == hq.height,
                "LOCAL HUD: anchored at the neutral turned by yaw0 ONLY (not by L, not by the head): " + what);
            const double nd[3] = {n[0], n[1], n[2]};
            const Quad full = hud_world_quad(n, g.src.lens, 40.0f, dist, 1.0f, yaw);
            const auto dh = quad_dirs_turned(l.hud_pose, l.hud_width, l.hud_height, nd), df = quad_dirs_turned(full.pose, full.width, full.height, nd);
            for (int c = 0; c < 4; ++c) for (int i = 0; i < 2; ++i)
                check(std::fabs(dh.t[c][i] - scale * df.t[c][i]) < 5e-5, "scale shrinks the tangent extents about the anchor ray (angular size x scale): " + what);
            check(std::fabs(dh.t[4][0]) < 5e-5 && std::fabs(dh.t[4][1]) < 5e-5, "centre stays on the anchor ray at any scale: " + what);
            const double want_d = dist > 0.0f ? double(dist) : 40.0;
            const double dx = double(l.hud_pose.position[0]) - nd[0], dz = double(l.hud_pose.position[2]) - nd[2];
            check(std::fabs(std::sqrt(dx * dx + dz * dz) - want_d) < 1e-4 * (1.0 + want_d), "HUD at the configured distance from the anchor: " + what);
        }
        // Cockpit projection: the render pose P0 (translated head) - never recentred or look-turned by the consumer.
        for (unsigned e = 0; e < 2; ++e) check(pose_is(l.views[e].pose, p0_eye(e, head_dx(w), 0.01f * float(w))), "projection pose is the head's render pose: " + what);
    }
    check(g.xr->layers.size() >= 14, "enough layers: " + what);
    // Baseline statements (pairwise) on the layers made from script steps w = 1 (start), 4 (look changed), 7 (head translated) and the last (recentred, look changed again).
    auto at = [&](unsigned w) -> const ProjectionLayer& {
        for (const auto& l : g.xr->layers) if (step_of(l) == w) return l;
        check(false, "a layer made from script step " + std::to_string(w) + " exists: " + what); return g.xr->layers[0];
    };
    const auto &A = at(1), &B = at(4), &C = at(7), &D = g.xr->layers.back();
    check(step_of(D) >= kRecenterFrom, "the last layer is made after the recenter: " + what);
    check(!same_pose(A.quad_pose, B.quad_pose), "world quad MOVES when only the look changes: " + what);
    if (markers_on) check(!same_pose(A.marker_pose, B.marker_pose), "marker quad MOVES when only the look changes: " + what);
    check(same_pose(A.hud_pose, B.hud_pose) && A.hud_width == B.hud_width, "the HUD does NOT move when only the look changes (current baseline): " + what);
    check(same_pose(B.quad_pose, C.quad_pose) && same_pose(B.hud_pose, C.hud_pose) && (!markers_on || same_pose(B.marker_pose, C.marker_pose)), "head translation moves no quad/HUD anchor: " + what);
    check(!same_pose(B.views[0].pose, C.views[0].pose), "...while the cockpit projection does follow the (render) head: " + what);
    check(!same_pose(C.quad_pose, D.quad_pose) && (head_locked ? same_pose(C.hud_pose, D.hud_pose) : !same_pose(C.hud_pose, D.hud_pose)),
        "recenter re-anchors world/marker quads and the LOCAL HUD; the VIEW HUD is unaffected: " + what);
}

void anchor_contract() {
    for (float scale : {0.5f, 1.0f}) {
        for (float dist : {0.0f, 0.5f, 0.8f}) anchor_contract_case(false, scale, dist);
        for (float dist : {1.0f, 2.5f}) anchor_contract_case(true, scale, dist);
    }
}


// ============================================================ POV fixes: F-LEAN anchors and F-HUD (docs/review/pov-fixes-impl.md) ===
// A head that only ROTATES about a neck pivot (true lever f, n = (0, 0.7 f, -f)) is the test-14 situation: the eye midpoint swings by R n - R0 n.
namespace {
constexpr double kPovF = 0.132, kPovYaw0 = 0.4;
const double kPovPivot[3] = {0.4, 1.3, -0.5};
void pov_mid(double psi, double out[3]) {
    const double n[3] = {0.0, 0.7 * kPovF, -kPovF}, c = std::cos(psi), s = std::sin(psi);
    out[0] = kPovPivot[0] + c * n[0] + s * n[2]; out[1] = kPovPivot[1] + n[1]; out[2] = kPovPivot[2] - s * n[0] + c * n[2];
}
double quat_yaw(const float* q) { return 2.0 * std::atan2(double(q[1]), double(q[3])); }
double quat_pitch(const float* q) { return 2.0 * std::atan2(double(q[0]), double(q[3])); }
// A frame whose head yaw is kPovYaw0 + rel(frame id), carrying look (ly, lp) from look_of_id, neutral = the pose at rel 0.
void pov_rig(Rig& g, float lever, float scale, unsigned follow, std::function<double(uint64_t)> rel_of, std::function<std::pair<float, float>(uint64_t)> look_of_id) {
    g.settings.lean = acz::pov::make_lean(lever, scale);
    g.settings.hud_follow = follow;
    g.src.with_neutral = true;
    g.build();
    g.src.mutate = [rel_of, look_of_id](stereo::StereoFrame& f) {
        const double psi = kPovYaw0 + rel_of(f.frame_id);
        double m[3]; pov_mid(psi, m);
        const double c = std::cos(psi), s = std::sin(psi), h = 0.032;
        for (unsigned e = 0; e < 2; ++e) {
            const double sg = e ? 1.0 : -1.0;
            f.pose[e] = p0_eye(e);
            f.pose[e].position[0] = float(m[0] + sg * h * c); f.pose[e].position[1] = float(m[1]); f.pose[e].position[2] = float(m[2] - sg * h * s);
            f.pose[e].orientation[0] = 0; f.pose[e].orientation[1] = float(std::sin(psi / 2)); f.pose[e].orientation[2] = 0; f.pose[e].orientation[3] = float(std::cos(psi / 2));
        }
        double p0[3]; pov_mid(kPovYaw0, p0);
        f.neutral_valid = true; for (int i = 0; i < 3; ++i) f.neutral_pos[i] = float(p0[i]); f.neutral_yaw = float(kPovYaw0);
        const auto lk = look_of_id(f.frame_id);
        f.look_valid = lk.first != 0.0f || lk.second != 0.0f; f.look_yaw = lk.first; f.look_pitch = lk.second;
    };
}
double pov_rel_of_layer(const ProjectionLayer& l) { return quat_yaw(l.views[0].pose.orientation) - kPovYaw0; }
}

void pov_lean_anchors() {
    using namespace xr_stereo;
    for (unsigned follow : {0u, 1u}) {
        Rig g; g.stop_after = 16; markers_rig(g); g.settings.quad_distance_m = 40.0f;
        pov_rig(g, float(kPovF), 1.0f, follow, [](uint64_t id) { return 0.12 * double(id % 12); }, [](uint64_t) { return std::pair<float, float>(0.0f, 0.0f); });
        auto r = g.run();
        const std::string what = "lean follow " + std::to_string(follow);
        check(r.success && !g.out->failed() && g.xr->violations == 0 && g.src.bad_releases == 0 && g.xr->layers.size() >= 8, "lean run succeeds: " + what);
        double p0[3]; pov_mid(kPovYaw0, p0);
        const float p0f[3] = {float(p0[0]), float(p0[1]), float(p0[2])};
        unsigned moved = 0;
        for (const auto& l : g.xr->layers) {
            const double rel = pov_rel_of_layer(l);
            double m[3]; pov_mid(kPovYaw0 + rel, m);
            const Quad base = world_quad(p0f, g.src.lens, 40.0f, float(kPovYaw0));
            // Pure rotation about the true pivot: the virtual eye stays at the neutral, so the real eye is displaced from it by (m - p0) and every
            // cockpit-frame layer must be displaced by exactly that (it stays fixed relative to the virtual eye).
            for (int i = 0; i < 3; ++i) {
                check(std::fabs((double(l.quad_pose.position[i]) - double(base.pose.position[i])) - (m[i] - p0[i])) < 2e-5, "world quad shifted by m - p0: " + what);
                check(std::fabs((double(l.marker_pose.position[i]) - double(base.pose.position[i])) - (m[i] - p0[i])) < 2e-5, "marker quad shifted like the world quad: " + what);
            }
            const Quad hbase = hud_world_quad(p0f, g.src.lens, 40.0f, g.settings.hud_quad_distance_m, g.settings.hud_scale, float(kPovYaw0));
            for (int i = 0; i < 3; ++i) {
                const double want = follow ? (m[i] - p0[i]) : 0.0;
                check(std::fabs((double(l.hud_pose.position[i]) - double(hbase.pose.position[i])) - want) < 2e-5, "HUD shifted only with xr_hud_follow >= 1: " + what);
            }
            if (std::fabs(m[0] - p0[0]) + std::fabs(m[2] - p0[2]) > 0.05) ++moved;
        }
        check(moved >= 4, "the fixture really swings the head (> 5 cm): " + what);
    }
}

void pov_lean_off_identical() {
    using namespace xr_stereo;
    // Keys off (default Settings): the same script gives the baseline anchors. Lean with lever 0 / scale 1 (the key-off values) is inactive.
    check(!acz::pov::Lean{}.active() && !acz::pov::make_lean(0.0, 1.0).active() && acz::pov::make_lean(0.1, 1.0).active() && acz::pov::make_lean(0.0, 0.5).active(), "Lean activity");
    Rig g; g.stop_after = 16; markers_rig(g); g.settings.quad_distance_m = 40.0f;
    pov_rig(g, 0.0f, 1.0f, 0u, [](uint64_t id) { return 0.12 * double(id % 12); }, [](uint64_t) { return std::pair<float, float>(0.0f, 0.0f); });
    auto r = g.run();
    check(r.success && g.xr->layers.size() >= 8, "key-off run succeeds");
    double p0[3]; pov_mid(kPovYaw0, p0);
    const float p0f[3] = {float(p0[0]), float(p0[1]), float(p0[2])};
    const Quad base = world_quad(p0f, g.src.lens, 40.0f, float(kPovYaw0));
    const Quad hbase = hud_world_quad(p0f, g.src.lens, 40.0f, g.settings.hud_quad_distance_m, g.settings.hud_scale, float(kPovYaw0));
    for (const auto& l : g.xr->layers)
        check(same_pose(l.quad_pose, base.pose) && same_pose(l.marker_pose, base.pose) && same_pose(l.hud_pose, hbase.pose), "keys off: quads stay on the neutral anchor");
}

void pov_scale_modes() {
    using namespace xr_stereo;
    // scale 0 (rotation-only, lever 0): the virtual eye follows the real head centre, so the layer shift is the whole (m - p0).
    for (float scale : {0.0f, 0.5f}) {
        Rig g; g.stop_after = 16; markers_rig(g); g.settings.quad_distance_m = 40.0f;
        pov_rig(g, 0.0f, scale, 1u, [](uint64_t id) { return 0.12 * double(id % 12); }, [](uint64_t) { return std::pair<float, float>(0.0f, 0.0f); });
        auto r = g.run();
        check(r.success && g.xr->layers.size() >= 8, "scale run succeeds");
        double p0[3]; pov_mid(kPovYaw0, p0);
        const float p0f[3] = {float(p0[0]), float(p0[1]), float(p0[2])};
        const Quad base = world_quad(p0f, g.src.lens, 40.0f, float(kPovYaw0));
        for (const auto& l : g.xr->layers) {
            double m[3]; pov_mid(kPovYaw0 + pov_rel_of_layer(l), m);
            for (int i = 0; i < 3; ++i)
                check(std::fabs((double(l.quad_pose.position[i]) - double(base.pose.position[i])) - double(1.0f - scale) * (m[i] - p0[i])) < 2e-5, "layer shift = (1 - scale) (m - p0), lever 0");
        }
    }
}

void pov_hud_follow_mode2() {
    using namespace xr_stereo;
    const float kLook = 0.6f;
    Rig g; g.stop_after = 30; markers_rig(g); g.settings.quad_distance_m = 40.0f;
    pov_rig(g, 0.0f, 1.0f, 2u, [](uint64_t) { return 0.0; }, [kLook](uint64_t id) { return id < 4 ? std::pair<float, float>(0.0f, 0.0f) : std::pair<float, float>(kLook, 0.2f); });
    clock_per_frame(g, 25);
    auto r = g.run();
    check(r.success && !g.out->failed() && g.xr->layers.size() >= 20, "mode 2 run succeeds");
    double prev = -1, maxstep = 0; double last = 0;
    bool seen_partial = false;
    for (const auto& l : g.xr->layers) {
        const double hy = quat_yaw(l.hud_pose.orientation) - kPovYaw0;
        if (prev >= 0) maxstep = std::max(maxstep, std::fabs(hy - prev));
        if (hy > 0.01 && hy < 0.5 * kLook - 0.01) seen_partial = true;
        prev = hy; last = hy;
        check(hy >= -1e-4 && hy <= 0.5 * kLook + 1e-4, "HUD turn stays between 0 and alpha * look");
    }
    // 60 deg/s over a 25 ms frame (the fixture's clock) is 0.0262 rad; layers may be sampled every frame or every other frame.
    check(maxstep <= 2.0 * 1.0471975512 * 0.025 + 2e-3, "HUD turn rate limited (60 deg/s)");
    check(seen_partial, "the HUD was seen part-way (rate limit, not a jump)");
    check(std::fabs(last - 0.5 * kLook) < 2e-3, "the HUD converges to alpha * look");
    // The world quad itself is NOT rate limited: it follows the full recorded look.
    const auto& L = g.xr->layers.back();
    check(std::fabs(quat_yaw(L.quad_pose.orientation) - (kPovYaw0 + kLook)) < 2e-3 && std::fabs(quat_pitch(L.quad_pose.orientation) - 0.2) < 2e-3, "world quad still along the recorded look");
    // Mode 1 (no look follow) keeps the HUD on the neutral heading whatever the look does.
    Rig h; h.stop_after = 20; markers_rig(h); h.settings.quad_distance_m = 40.0f;
    pov_rig(h, 0.0f, 1.0f, 1u, [](uint64_t) { return 0.0; }, [kLook](uint64_t id) { return id < 4 ? std::pair<float, float>(0.0f, 0.0f) : std::pair<float, float>(kLook, 0.2f); });
    auto r2 = h.run();
    check(r2.success && h.xr->layers.size() >= 10, "mode 1 run succeeds");
    for (const auto& l : h.xr->layers) check(std::fabs(quat_yaw(l.hud_pose.orientation) - kPovYaw0) < 2e-4 && std::fabs(quat_pitch(l.hud_pose.orientation)) < 2e-4, "mode 1: HUD does not follow the look");
}

void pov_fixes_consumer() {
    pov_lean_anchors(); pov_lean_off_identical(); pov_scale_modes(); pov_hud_follow_mode2();
}

// C. The correlated per-selected-frame record (select_corr_log).
namespace {
void corr_rig(Rig& g, int start, bool log_on) {
    hud_full_rig(g); g.settings.world_in_projection = true; g.settings.quad_distance_m = 500.0f;
    g.settings.select_corr_log = log_on;
    g.settings.mono_us = [&g] { return int64_t(10000000) + g.clock->t.load() * 1000; };
    g.src.with_neutral = true; g.src.nyaw = 0.4f;
    g.src.mutate = [&g, start](stereo::StereoFrame& f) {
        f.pose[0] = p0_eye(0); f.pose[1] = p0_eye(1);
        f.look_valid = true; f.look_yaw = 0.5f; f.look_pitch = 0.2f;
        if (start < 0) return; // a producer that fills no diagnostics
        f.diag_valid = true; f.diag_start = uint8_t(start); f.diag_look_src = start == 1 ? 1 : start >= 2 ? 2 : 0;
        f.diag_look_meas_valid = start == 1; f.diag_look_meas_yaw = 0.52f; f.diag_look_meas_pitch = 0.21f; f.diag_look_est_yaw = 0.5f; f.diag_look_est_pitch = 0.2f;
        f.diag_pose_seq = 1000 + f.frame_id; f.diag_generation = 77;
        const int64_t t = g.clock->t.load();
        f.diag_start_us = 10000000 + (t - 4) * 1000; f.diag_publish_us = 10000000 + t * 1000;
    };
}
}

void timing_select_corr_record() {
    using namespace xr_stereo;
    // OFF (the default): no record; the byte stream is the same whether or not the producer fills diagnostics.
    { Rig a; corr_rig(a, 1, false); a.stop_after = 60; a.build(); auto ra = a.run();
      Rig b; corr_rig(b, -1, false); b.stop_after = 60; b.build(); auto rb = b.run();
      check(ra.success && rb.success && corr_lines(a).empty() && corr_lines(b).empty(), "OFF: no select-corr record");
      check(a.lines == b.lines && a.j.calls() == b.j.calls(), "OFF: log lines and call journal identical with or without producer diagnostics"); }
    // ON: additive only - removing the records leaves the OFF stream unchanged.
    { Rig off; corr_rig(off, 1, false); off.stop_after = 120; off.build(); auto roff = off.run();
      Rig on; corr_rig(on, 1, true); on.stop_after = 120; on.build(); auto ron = on.run();
      check(roff.success && ron.success && !corr_lines(on).empty(), "ON: records emitted");
      std::vector<std::string> rest;
      for (auto& l : on.lines) if (l.find("event=select-corr") == std::string::npos) rest.push_back(l);
      check(rest == off.lines && on.j.calls() == off.j.calls(), "ON: every other line and every call byte-identical to OFF");
      const auto recs = corr_lines(on);
      check(recs.size() >= kSelCorrFirst + 1 && recs.size() <= kSelCorrFirst + 4, "first " + std::to_string(kSelCorrFirst) + " then rate limited (" + std::to_string(recs.size()) + ")");
      for (auto& l : recs) check(l.size() < feature_log::line_cap, "record fits the router: " + l);
      const std::string& r1 = recs.front();
      const double r2d = 57.29577951308232;
      char want[64];
      check(tok(r1, "n") == "1" && tok(r1, "layer") == "fresh" && tok(r1, "world") == "fresh" && tok(r1, "start") == "cockpit" && tok(r1, "members") == "7" &&
          tok(r1, "gen") == "77" && tok(r1, "refusal") == "none" && tok(r1, "look") == "measured" && tok(r1, "partial") == "0", "record: identity/status tokens");
      check(tok(r1, "pose-seq") == std::to_string(1000 + std::stoull(tok(r1, "id"))) && tok(r1, "pose-pred-ns") == "5", "record: producer frame id, pose snapshot identity and predicted time");
      const long long fx = std::stoll(tok(r1, "xr-frame"));
      check(fx >= 1 && tok(r1, "xr-pred-ns") == std::to_string(1000000000LL + fx * 11111111LL) &&
          std::find(on.xr->layer_times.begin(), on.xr->layer_times.end(), std::stoll(tok(r1, "xr-pred-ns"))) != on.xr->layer_times.end(),
          "record: current XR frame and its predicted display time (the time the layer was submitted for)");
      std::snprintf(want, sizeof want, "%.2f,%.2f", 0.5 * r2d, 0.2 * r2d); check(tok(r1, "look-deg") == want, "record: published look");
      std::snprintf(want, sizeof want, "%.2f,%.2f", 0.52 * double(1.0f) * r2d, 0.21 * r2d);
      check(tok(r1, "look-meas-deg").substr(0, 5) == std::string(want).substr(0, 5), "record: measured look");
      std::snprintf(want, sizeof want, "%.2f,%.2f", double(0.5f) * r2d, double(0.2f) * r2d); check(tok(r1, "look-est-deg") == want, "record: estimated look");
      check(tok(r1, "sub-pos") == "-0.070,1.550,0.300" && tok(r1, "sub-yaw-pitch-deg").substr(0, 6) == "17.19," && std::fabs(std::stod(tok(r1, "sub-yaw-pitch-deg").substr(6))) < 0.01, "record: submitted eye-0 pose is the render pose P0: " + r1);
      const xr_owner::Fov lf = fov_from_lens(on.src.lens);
      std::snprintf(want, sizeof want, "%.1f,%.1f,%.1f,%.1f", double(lf.left) * r2d, double(lf.right) * r2d, double(lf.up) * r2d, double(lf.down) * r2d);
      check(tok(r1, "sub-fov-deg") == want, "record: submitted FOV");
      check(tok(r1, "prod-ms") == "4.00" && std::stod(tok(r1, "pub-to-sel-ms")) >= 11.0 - 1e-9 && std::stod(tok(r1, "pub-to-sel-ms")) < 200.0,
          "record: producer start->publication (4 ms) and publication->selection (>= the 11 ms wait) from the monotonic stamps");
      check(std::stod(tok(r1, "age-ms")) >= 0.0, "record: non-negative age");
      check(std::stoll(tok(r1, "sel-us")) >= 10000000, "record: selection stamp present"); }
    // Start kinds, look source, refusal sanitising, no diagnostics -> no record.
    for (int start : {0, 1, 2, 3}) {
        Rig g; corr_rig(g, start, true); g.stop_after = 12;
        g.src.mutate = [inner = g.src.mutate](stereo::StereoFrame& f) { inner(f); f.diag_refusal = "world-early-no-slot;x y=z"; };
        g.build(); auto r = g.run();
        const auto recs = corr_lines(g);
        check(r.success && !recs.empty(), "start-kind run");
        const char* names[] = {"unknown", "cockpit", "early-adopted", "early-unadopted"};
        check(tok(recs.front(), "start") == names[start], std::string("start kind ") + names[start]);
        check(tok(recs.front(), "look") == (start == 1 ? "measured" : start == 0 ? "unknown" : "estimated"), "look source by start kind");
        check((start == 1) == (tok(recs.front(), "look-meas-deg") != "none"), "measured pair only when measured");
        check(tok(recs.front(), "refusal") == "world-early-no-slot?x?y?z" && tok(recs.front(), "no-headset-claim") == "<missing>" && recs.front().find(";no-headset-claim") != std::string::npos,
            "refusal reason sanitised: a hostile string cannot add tokens");
    }
    { Rig g; corr_rig(g, -1, true); g.stop_after = 20; g.build(); auto r = g.run();
      check(r.success && corr_lines(g).empty(), "ON but the producer fills no diagnostics: no record"); }
    // Reused layers: the record repeats the last SELECTED frame's facts with status reused.
    { Rig g; corr_rig(g, 1, true); g.stop_after = 24; g.publish_until = 3; g.build(); auto r = g.run();
      const auto recs = corr_lines(g);
      check(r.success && g.out->counters().reused > 0 && recs.size() > 3, "reuse run");
      unsigned reused = 0;
      for (auto& l : recs) if (tok(l, "layer") == "reused") {
          ++reused; check(tok(l, "id") == "3" && tok(l, "pose-seq") == "1003" && tok(l, "world") == "fresh" && std::stod(tok(l, "age-ms")) > 0.0, "reused record: last selected frame's facts, positive age");
      }
      check(reused > 0, "at least one reused record"); }
    // Held world: the record says so.
    { Rig g; corr_rig(g, 1, true); g.settings.world_hold_last = true; g.src.world_gap_from = 6; g.src.world_gap_to = 47; g.stop_after = 60; g.build(); clock_per_frame(g, 25);
      auto r = g.run();
      unsigned held = 0; for (auto& l : corr_lines(g)) held += tok(l, "world") == "held";
      check(r.success && g.out->counters().world_held > 0 && held > 0, "held-world record"); }
    // Cap: never more than kSelCorrCap records, however long the run.
    { Rig g; corr_rig(g, 1, true); g.stop_after = 2100; g.build(); clock_per_frame(g, 100); auto r = g.run();
      check(r.success && !g.out->failed() && corr_lines(g).size() == kSelCorrCap, "capped at kSelCorrCap records (" + std::to_string(corr_lines(g).size()) + ")"); }
}

// ---------------------------------------------------------------- J-terrain-render (consumer side)
std::shared_ptr<xr_stereo::TerrainRuntime> terrain_rt(bool fixed_cam, bool blend = false) {
    auto rt = std::make_shared<xr_stereo::TerrainRuntime>();
    rt->stage = acz::terrain::make_synthetic_stage(1, false);
    acz::terrain::build_atlas(rt->stage, rt->atlas);
    if (fixed_cam) { rt->conv.fixed_cam = true; rt->conv.cam[0] = 1000; rt->conv.cam[1] = 5000; rt->conv.cam[2] = -2000; rt->conv.cam[3] = -45; }
    rt->conv.blend_window = blend;
    return rt;
}
// WORLD-1 step 3: ONE camera. The frame's game camera (orientation) + DrawACM_v eye (position) drive the terrain when present and consistent with E6; a game
// camera that disagrees with E6 beyond gmax is refused for that frame (E6 used, counted); csrc:e6 forces E6; the frame-synchronous E6 sample beats the newest
// mailbox sample (e6:frame) unless e6:latest; the object occlusion flag needs the producer's depth fix and a float depth format.
void terrain_world1_camera_runs() {
    using acz::terrain::CamFrame;
    const float e6pos[3] = {1000, 5000, -2000};
    const float pitch = 0.1f, yaw = -0.785f, roll = 0.3f;
    auto rt_with_e6 = [&](float p, float y, float r) {
        auto rt = terrain_rt(false);
        rt->camera = [=](acz::terrain::CamSample& c) { for (int i = 0; i < 3; ++i) c.campos[i] = e6pos[i]; c.pitch = p; c.yaw = y; c.roll = r; return true; };
        return rt;
    };
    auto game_axes = [&](const acz::terrain::Conventions& cv, float p, float y, float r, stereo::StereoFrame& f) {
        CamFrame g{}; acz::terrain::camera_frame(e6pos, p, y, r, cv, g);
        for (int i = 0; i < 3; ++i) { f.game_cam_right[i] = g.right[i]; f.game_cam_up[i] = g.up[i]; f.game_cam_fwd[i] = g.fwd[i]; }
        f.game_cam_valid = true;
    };
    const float geye[3] = {1100, 5050, -2100};
    auto expect_eyes = [&](Rig& g, const CamFrame& cf, float upm, const char* what) {
        check(!g.blit->jobs.empty() && g.blit->jobs[0].terrain, std::string(what) + ": terrain job");
        if (g.blit->jobs.empty()) return;
        const auto& jb = g.blit->jobs[0];
        for (unsigned e = 0; e < 2; ++e) {
            float w[3]; acz::terrain::eye_world(cf, jb.world_eye[e].origin, upm, w);
            for (int i = 0; i < 3; ++i)
                check(std::fabs(w[i] - jb.terrain_eye[e].eye[i]) < 1e-2f && std::fabs(jb.terrain_eye[e].fwd[i] - cf.fwd[i]) < 1e-5f && std::fabs(jb.terrain_eye[e].up[i] - cf.up[i]) < 1e-5f,
                      std::string(what) + ": per-eye position and basis");
        }
    };
    // (1) the game camera + DrawACM_v eye of the frame drive the terrain (rolled attitude, E6 consistent)
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); g.settings.terrain = rt;
        const auto cv = rt->conv;
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = geye[i]; };
        g.build(); (void)g.run();
        CamFrame want{}; CamFrame e6f{}; acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, e6f);
        check(acz::terrain::camera_frame_axes(geye, e6f.right, e6f.up, e6f.fwd, cv, want), "game frame");
        expect_eyes(g, want, cv.head_units_per_metre, "game camera");
        const auto& c = g.out->counters();
        check(c.terrain_src_game > 0 && c.terrain_eye_game > 0 && c.terrain_src_e6 == 0 && c.terrain_game_refused == 0 && c.terrain_e6_angle_max_cdeg < 5, "game-camera counters");
        check(g.has_line("event=summary-terrain-camera") && g.has_line("cam-source=game-camera(e6-fallback)"), "camera provenance lines"); }
    // (2) game camera without a DrawACM_v eye: position from E6
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); g.settings.terrain = rt;
        const auto cv = rt->conv;
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw, roll, f); };
        g.build(); (void)g.run();
        CamFrame want{}; acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, want);
        expect_eyes(g, want, cv.head_units_per_metre, "game axes + E6 eye");
        check(g.out->counters().terrain_src_game > 0 && g.out->counters().terrain_eye_e6 > 0, "E6 eye counted"); }
    // (3) a game camera 30 deg away from E6 (beyond gmax 15): refused, E6 used, counted
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); g.settings.terrain = rt;
        const auto cv = rt->conv;
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw + 0.5236f, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = geye[i]; };
        g.build(); (void)g.run();
        CamFrame want{}; acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, want);
        expect_eyes(g, want, cv.head_units_per_metre, "refused game camera");
        const auto& c = g.out->counters();
        check(c.terrain_game_refused > 0 && c.terrain_src_e6 > 0 && c.terrain_src_game == 0 && c.terrain_e6_angle_max_cdeg >= 2900, "refusal counted with the angle"); }
    // (3b) E6-PLAUSIBLE (2026-10-30 m03k): the same 30 deg disagreement, but the E6 eye is nowhere near the game camera's eye (stale E6 block): the E6 sample cannot veto the
    // game camera: it is KEPT (not refused), counted as implausible, mode game-camera-only (objects registered), one enter line; the healthy case (3) is unchanged above.
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); g.settings.terrain = rt;
        const auto cv = rt->conv;
        const float far_eye[3] = {105768.0f, 18021.0f, 92654.0f};
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw + 0.5236f, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = far_eye[i]; };
        g.build(); (void)g.run();
        CamFrame want{}; CamFrame e6f{}; acz::terrain::camera_frame(e6pos, pitch, yaw + 0.5236f, roll, cv, e6f);
        check(acz::terrain::camera_frame_axes(far_eye, e6f.right, e6f.up, e6f.fwd, cv, want), "far game frame");
        expect_eyes(g, want, cv.head_units_per_metre, "implausible E6 cannot veto the game camera");
        const auto& c = g.out->counters();
        check(c.terrain_e6_implausible > 0 && c.terrain_game_refused == 0 && c.terrain_src_game > 0 && c.terrain_src_e6 == 0 && c.terrain_eye_game > 0, "implausible E6 counters");
        check(g.has_line("event=e6-implausible;state=enter") && g.has_line("e6-implausible=") && !g.has_line("state=exit"), "implausible E6 lines"); }
    // (3d) LENS-FLARE telemetry: a stage env with the flare switched on and the sun straight ahead of the terrain camera = `active` frames, a config line and a window summary;
    // the same env with the sun behind the camera = `behind`; no env at all = `no-env` (the flare cannot run); none of it needs an E6 lock or a game camera.
    for (int variant = 0; variant < 3; ++variant) {
        Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(0.6f, yaw, 0.0f); g.settings.terrain = rt;
        const auto cv = rt->conv;
        CamFrame cf{}; acz::terrain::camera_frame(e6pos, 0.6f, yaw, 0.0f, cv, cf);
        if (variant < 2) {
            rt->env.assign(size_t(acz::terrain::kEnvTexW) * acz::terrain::kEnvTexH * 4, 0.0f);
            rt->conv.env_bits = acz::terrain::kEnvSky;
            float* L = &rt->env[size_t(acz::terrain::kPLight) * 4];
            for (int i = 0; i < 3; ++i) L[i] = (variant == 0 || i == 1 ? 1.0f : -1.0f) * cf.fwd[i];   // variant 1: the horizontal part mirrored = the sun behind the camera, still above the horizon
            L[3] = 1.0f;
            acz::lensflare::param_texel(true, 1.0f, &rt->env[size_t(acz::terrain::kPFlare) * 4]);
        }
        g.build(); (void)g.run();
        const auto& c = g.out->counters();
        const uint64_t act = c.lf_why[unsigned(acz::lensflare::Why::active)], beh = c.lf_why[unsigned(acz::lensflare::Why::behind)], noenv = c.lf_why[unsigned(acz::lensflare::Why::no_env)];
        check(c.lf_frames > 0 && c.lf_frames == act + beh + noenv + c.lf_why[unsigned(acz::lensflare::Why::below_horizon)] + c.lf_why[unsigned(acz::lensflare::Why::out_of_view)], "lens flare: every frame classified");
        if (variant == 0) check(act == c.lf_frames, "lens flare: sun ahead = active in every frame");
        if (variant == 1) check(beh == c.lf_frames, "lens flare: sun behind = behind");
        if (variant == 2) check(noenv == c.lf_frames, "lens flare: no env = no-env");
        check(g.has_line("event=lensflare-config") && g.has_line("event=summary-lensflare") && g.has_line(variant == 0 ? "active=" : "no-env="), "lens flare: config and summary lines");
        if (variant == 0) check(g.has_line("switch=1;k=1.00;light=1;light-is-moon=0"), "lens flare: config fields");
    }
    // (3c) the healthy 30 deg refusal (3) never counts as implausible
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); g.settings.terrain = rt;
        const auto cv = rt->conv;
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw + 0.5236f, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = geye[i]; };
        g.build(); (void)g.run();
        check(g.out->counters().terrain_e6_implausible == 0 && g.out->counters().terrain_game_refused > 0 && !g.has_line("event=e6-implausible"), "near eyes: refusal, not implausible"); }
    // (4) csrc:e6: the E6 path even with a game camera in the frame
    {   Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); rt->conv.cam_source_game = false; g.settings.terrain = rt;
        const auto cv = rt->conv;
        g.src.mutate = [&](stereo::StereoFrame& f) { game_axes(cv, pitch, yaw + 0.05f, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = geye[i]; };
        g.build(); (void)g.run();
        CamFrame want{}; acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, want);
        expect_eyes(g, want, cv.head_units_per_metre, "csrc:e6");
        check(g.out->counters().terrain_src_game == 0 && g.out->counters().terrain_src_e6 > 0, "csrc:e6 counters"); }
    // (5) e6:frame: the frame's own E6 sample beats the newest mailbox sample; e6:latest restores the mailbox
    for (int latest = 0; latest < 2; ++latest) {
        Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll); rt->conv.e6_frame = latest == 0; g.settings.terrain = rt;
        const auto cv = rt->conv;
        const float fpos[3] = {7, 4000, 9};
        g.src.mutate = [&](stereo::StereoFrame& f) { f.e6_valid = true; for (int i = 0; i < 3; ++i) f.e6_campos[i] = fpos[i]; f.e6_pitch = -0.2f; f.e6_yaw = 0.4f; f.e6_roll = -1.0f; f.e6_seq = 77; };
        g.build(); (void)g.run();
        CamFrame want{};
        if (latest) acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, want); else acz::terrain::camera_frame(fpos, -0.2f, 0.4f, -1.0f, cv, want);
        expect_eyes(g, want, cv.head_units_per_metre, latest ? "e6:latest" : "e6:frame");
        check(latest ? g.out->counters().terrain_e6_frame == 0 : g.out->counters().terrain_e6_frame > 0, "frame-sample counter");
    }
    // (6) object occlusion flag: needs the producer's depth fix (settings.objects_depth_fix), a float depth format and occl:1
    for (int k = 0; k < 4; ++k) {
        Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = terrain_rt(true); g.settings.terrain = rt;
        g.settings.objects_depth_fix = k != 1; if (k == 2) rt->conv.occlude_objects = false; if (k == 3) g.src.depth_format = 45;   // 45 = D24_UNORM_S8_UINT
        g.build(); (void)g.run();
        const bool want = k == 0;
        check(!g.blit->jobs.empty() && g.blit->jobs[0].terrain && g.blit->jobs[0].terrain_occlude == want, "terrain_occlude only with depth fix + float depth + occl:1 (case " + std::to_string(k) + ")");
    }
    // (7) PROPS-SHADING objdom:display: the job flag needs the option, the producer's depth fix and a float depth format (occl does not matter); default off
    for (int k = 0; k < 5; ++k) {
        Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = terrain_rt(true); g.settings.terrain = rt;
        rt->conv.obj_display = k != 1; g.settings.objects_depth_fix = k != 2; if (k == 3) g.src.depth_format = 45; if (k == 4) rt->conv.occlude_objects = false;
        g.build(); (void)g.run();
        const bool want = k == 0 || k == 4;
        check(!g.blit->jobs.empty() && g.blit->jobs[0].terrain && g.blit->jobs[0].terrain_obj_display == want, "terrain_obj_display only with objdom + depth fix + float depth (case " + std::to_string(k) + ")");
        check(g.has_line("event=terrain-debug;fixed-cam=1") && g.has_line("objdom=display") == (k != 1), "terrain-debug line names the fixed cam and objdom (case " + std::to_string(k) + ")");
    }
    // (8) PROPS-SHADING fixed camera pitch/roll (cam:x,y,z,yaw,pitch,roll): the eyes use camera_frame with those angles, exactly like an E6 sample
    {
        Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = terrain_rt(true);
        rt->conv.cam[4] = -35.0f; rt->conv.cam[5] = 10.0f; g.settings.terrain = rt;
        g.build(); (void)g.run();
        const auto cv = rt->conv; CamFrame want{};
        acz::terrain::camera_frame(cv.cam, cv.cam[4] * 3.14159265358979f / 180.0f, cv.cam[3] * 3.14159265358979f / 180.0f, cv.cam[5] * 3.14159265358979f / 180.0f, cv, want);
        expect_eyes(g, want, cv.head_units_per_metre, "fixed cam pitch/roll");
        check(!g.blit->jobs.empty() && g.blit->jobs[0].terrain_eye[0].fwd[1] < -0.5f, "fixed cam pitched down 35 deg looks down");
    }
    // (9) OBJECT-INTERP interp:1: the terrain camera takes the PRODUCER's delta (StereoFrame::interp: the one the replayed objects were drawn with);
    //     a frame without the plan keeps the frame camera (extrap:1 off); interp off ignores the facts; the config / summary / trace lines are written.
    {
        const float dang = 0.03f;   // a 1.7 deg yaw about +y + a translation
        const float dR[9] = {std::cos(dang), 0, std::sin(dang), 0, 1, 0, -std::sin(dang), 0, std::cos(dang)};
        const float dt[3] = {12.5f, -1.0f, -30.0f};
        auto with_facts = [&](const acz::terrain::Conventions& cv, stereo::StereoFrame& f, bool valid) {
            game_axes(cv, pitch, yaw, roll, f); f.game_eye_valid = true; for (int i = 0; i < 3; ++i) f.game_eye[i] = geye[i];
            f.interp = acz::interp::Facts{};
            if (!valid) return;
            f.interp.valid = true; f.interp.cam_moved = true; f.interp.k = 1.25f; f.interp.horizon_ms = 20.8f; f.interp.why = "ok"; f.interp.objects = 3; f.interp.moved = 2;
            for (int i = 0; i < 3; ++i) f.interp.dt[i] = dt[i];
            for (int i = 0; i < 9; ++i) f.interp.dR[i] = dR[i];
            f.interp.nsample = 1; f.interp.sample[0].id = 7; f.interp.sample[0].kind = 2; f.interp.sample[0].moved = 1;
        };
        for (int mode = 0; mode < 3; ++mode) {   // 0 interp on + facts, 1 interp on, no facts, 2 interp off + facts
            Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = rt_with_e6(pitch, yaw, roll);
            rt->pacing.interp = mode != 2; g.settings.terrain = rt;
            const auto cv = rt->conv;
            g.src.mutate = [&, mode](stereo::StereoFrame& f) { with_facts(cv, f, mode != 1); };
            g.build(); (void)g.run();
            CamFrame e6f{}, want{}; acz::terrain::camera_frame(e6pos, pitch, yaw, roll, cv, e6f);
            check(acz::terrain::camera_frame_axes(geye, e6f.right, e6f.up, e6f.fwd, cv, want), "interp: game frame");
            if (mode == 0) acz::interp::apply_to_camera(dt, dR, want.eye, want.right, want.up, want.fwd);
            expect_eyes(g, want, cv.head_units_per_metre, mode == 0 ? "interp: the producer's delta moves the terrain camera" :
                                                          mode == 1 ? "interp: no plan in the frame: the frame camera" : "interp off: the facts are ignored");
            check(g.has_line("event=interp-config") == (mode != 2), "interp-config line only with interp:1 (mode " + std::to_string(mode) + ")");
            check(g.has_line("event=interp-trace;") == (mode == 0) && (mode != 0 || g.has_line(";o0=7:2:1:")), "interp-trace lines with the object samples (mode " + std::to_string(mode) + ")");
        }
    }
}
// CAM-EPOCH (Astra P2; e6-anchor-free.md section 10): the consumer admits the frame's E6 sample only when fresh, of the current epochs and correlated;
// otherwise the bounded Present-history fallback or the explicit no-camera path. Test mailbox/epochs are injected (rt->present / rt->epochs); the
// consumer clock (settings.mono_us) follows the rig clock so the frames' publication times are "now".
void terrain_cam_epoch_runs() {
    using acz::terrain::CamFrame;
    using acz::terrain::Admit;
    using acz::terrain::CamSource;
    static constexpr int64_t kBaseUs = 1000000000;
    struct Cam { acz::terrain::CamEpochs ep; acz::terrain::CamMailbox mb{&ep}; };
    // frame sample: campos x = 1000 + frame id (distinct per frame), stamped age_ms before the frame's publication, with the given epochs/tag
    struct FrameSpec { bool own = true; int64_t age_ms = 5; bool use_current_epochs = true; uint32_t lock = 0, stage = 0; bool tag_ok = true; bool game = false; };
    auto setup = [&](Rig& g, Cam& cam, std::shared_ptr<xr_stereo::TerrainRuntime>& rt, std::function<FrameSpec(uint64_t)> spec, bool admit = true) {
        hud_full_rig(g); g.settings.world_in_projection = true;
        rt = terrain_rt(false); rt->conv.cam_admit = admit; rt->present = &cam.mb; rt->epochs = &cam.ep; g.settings.terrain = rt;
        g.settings.mono_us = [&g] { return kBaseUs + g.clock->t * 1000; };
        const auto cv = rt->conv;
        g.src.mutate = [&g, &cam, spec, cv](stereo::StereoFrame& f) {
            const FrameSpec s = spec(f.frame_id);
            const int64_t now_us = kBaseUs + g.clock->t * 1000;
            f.diag_generation = 5000 + f.frame_id; f.diag_start_us = now_us - 12000; f.diag_publish_us = now_us;
            f.e6_valid = s.own;
            f.e6_campos[0] = 1000.0f + float(f.frame_id); f.e6_campos[1] = 5000; f.e6_campos[2] = -2000;
            f.e6_pitch = 0.1f; f.e6_yaw = -0.785f; f.e6_roll = 0.3f; f.e6_seq = f.frame_id;
            f.e6_t_ns = (now_us - s.age_ms * 1000) * 1000;
            f.e6_lock_epoch = s.use_current_epochs ? cam.ep.lock() : s.lock; f.e6_stage_epoch = s.use_current_epochs ? cam.ep.stage() : s.stage;
            f.e6_tag = s.tag_ok ? f.diag_generation : f.diag_generation + 1;
            if (s.game) {
                CamFrame gc{}; const float p[3] = {1000.0f + float(f.frame_id), 5000, -2000}; acz::terrain::camera_frame(p, 0.1f, -0.785f, 0.3f, cv, gc);
                for (int i = 0; i < 3; ++i) { f.game_cam_right[i] = gc.right[i]; f.game_cam_up[i] = gc.up[i]; f.game_cam_fwd[i] = gc.fwd[i]; f.game_eye[i] = gc.eye[i] + 3.0f; }
                f.game_cam_valid = true; f.game_eye_valid = true;
            }
        };
    };
    auto terrain_jobs = [](Rig& g) { unsigned n = 0; for (auto& jb : g.blit->jobs) n += jb.terrain; return n; };
    // (1) fresh, current-epoch, correlated own sample: used (eye x follows the frame's own campos), counted as frame, transition start -> frame, summary line
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 8;
        setup(g, cam, rt, [](uint64_t) { return FrameSpec{}; }); g.build(); clock_per_frame(g, 16); (void)g.run();
        const auto& st = g.out->camera_stats();
        check(terrain_jobs(g) == g.blit->jobs.size() && !g.blit->jobs.empty(), "cam-epoch: fresh own sample drives every terrain job");
        check(st.src[unsigned(CamSource::frame)] == g.blit->jobs.size() && g.out->counters().terrain_no_camera == 0, "cam-epoch: admitted as frame samples");
        check(g.has_line("event=camera-admit;from=start;to=e6-frame/frame") && g.has_line("event=summary-camera-admit") && g.has_line("admitted-frame="), "cam-epoch: transition + summary lines");
        check(st.age_max_ms >= 4.9 && st.age_max_ms <= 5.1, "cam-epoch: admitted age recorded (" + std::to_string(st.age_max_ms) + ")"); }
    // (2) stale own sample (400 ms old), no history: no camera (terrain off every frame), frame refusal counted as stale; the legacy path (cam_admit off) draws it
    for (int admit = 1; admit >= 0; --admit) {
        Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 8;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.age_ms = 400; return s; }, admit != 0); g.build(); clock_per_frame(g, 16); (void)g.run();
        const auto& st = g.out->camera_stats();
        if (admit) {
            check(terrain_jobs(g) == 0 && g.out->counters().terrain_no_camera > 0, "cam-epoch: stale own sample -> no camera");
            check(st.frame_refused[unsigned(Admit::stale)] > 0 && st.refused[unsigned(Admit::none)] > 0, "cam-epoch: stale counted, empty history -> none");
            check(g.has_line("to=none") && g.has_line("frame-sample=stale") && g.has_line("action=no-camera(terrain-off-this-frame)"), "cam-epoch: no-camera transition line");
        } else {
            check(terrain_jobs(g) == g.blit->jobs.size() && !g.has_line("event=summary-camera-admit") && !g.has_line("event=camera-admit"), "legacy: the stale sample is drawn, no new lines");
        }
    }
    // (3) stage change mid-run: the producer keeps stamping the OLD stage epoch (its samples predate the change) -> refused from the change on
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 12;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.use_current_epochs = false; return s; }); g.build();
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&g, &cam, inner] { g.clock->t += 16; if (g.xr->waits == 6) cam.ep.bump_stage(); if (inner) inner(); };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        check(st.src[unsigned(CamSource::frame)] > 0 && st.frame_refused[unsigned(Admit::stage_epoch)] > 0, "cam-epoch: admitted before, stage-epoch refused after the change");
        check(terrain_jobs(g) > 0 && terrain_jobs(g) < g.blit->jobs.size() && !g.blit->jobs.back().terrain, "cam-epoch: terrain stops at the stage change");
        check(g.has_line("from=e6-frame/frame;to=none/none") && g.has_line("frame-sample=stage-epoch-mismatch"), "cam-epoch: stage transition logged"); }
    // (4) lock lost then relocked: lock epoch 1 samples, loss (2) refuses them, relock (3) with fresh samples is admitted again
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 16; cam.ep.set_lock(1);
        setup(g, cam, rt, [](uint64_t id) { FrameSpec s; s.use_current_epochs = false; s.lock = id >= 10 ? 3 : 1; return s; }); g.build();
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&g, &cam, inner] { g.clock->t += 16; if (g.xr->waits == 5) cam.ep.set_lock(2); if (g.xr->waits == 8) cam.ep.set_lock(3); if (inner) inner(); };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        check(st.frame_refused[unsigned(Admit::lock_epoch)] > 0 && st.src[unsigned(CamSource::frame)] > 0 && g.blit->jobs.back().terrain, "cam-epoch: loss refuses, relock admits");
        check(st.transitions >= 3, "cam-epoch: frame -> refused -> frame transitions"); }
    // (5) bounded fallback: frames without their own sample use the Present-history publication nearest to the recording start (not the newest)
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 8;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.own = false; return s; }); g.build();
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&g, &cam, inner] {
            g.clock->t += 16;
            const int64_t now_us = kBaseUs + g.clock->t * 1000;
            const float near_p[3] = {777, 5000, -2000}, newest[3] = {999, 5000, -2000};
            cam.mb.publish(near_p, 0.1f, -0.785f, 0.3f, 0.0f, (now_us - 12000) * 1000);   // at the recording start
            cam.mb.publish(newest, 0.1f, -0.785f, 0.3f, 0.0f, now_us * 1000);            // the newest (must NOT be chosen)
            if (inner) inner();
        };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        check(st.src[unsigned(CamSource::fallback)] == g.blit->jobs.size() && terrain_jobs(g) == g.blit->jobs.size(), "cam-epoch: history fallback used");
        CamFrame want{}; const float p777[3] = {777, 5000, -2000}; acz::terrain::camera_frame(p777, 0.1f, -0.785f, 0.3f, rt->conv, want);
        bool near_used = !g.blit->jobs.empty();
        for (auto& jb : g.blit->jobs) { float w[3]; acz::terrain::eye_world(want, jb.world_eye[0].origin, rt->conv.head_units_per_metre, w); near_used = near_used && std::fabs(w[0] - jb.terrain_eye[0].eye[0]) < 1e-2f; }
        check(near_used, "cam-epoch: the fallback is the publication at the recording start, not the newest"); }
    // (6) held world image keeps the camera snapshot of the frame it was captured from (never the newest)
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 30; g.settings.world_hold_last = true; g.src.world_gap_from = 6; g.src.world_gap_to = 20;
        setup(g, cam, rt, [](uint64_t) { return FrameSpec{}; }); g.build(); clock_per_frame(g, 16);
        std::vector<float> held_x;
        auto inner = g.xr->hook["wait_frame"];
        uint64_t seen_held = 0;   // record the snapshot only right after a frame that WAS held (after the gap, new captures legitimately replace it)
        g.xr->hook["wait_frame"] = [&g, &held_x, &seen_held, inner] {
            acz::terrain::CamSample h; const uint64_t wh = g.out->counters().world_held;
            if (wh > seen_held && g.out->held_camera(h)) held_x.push_back(h.campos[0]);
            seen_held = wh; if (inner) inner(); };
        (void)g.run();
        bool all5 = !held_x.empty(); for (float x : held_x) all5 = all5 && x == 1005.0f;   // frame 5 = the last frame with a world before the gap
        check(g.out->counters().world_held > 0 && g.out->camera_stats().held > 0 && all5, "cam-epoch: held image snapshot stays the captured frame's camera"); }
    // (7) E6 refused but the frame carries its own game camera + DrawACM_v eye: terrain from the game camera only, counted, logged
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 6;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.age_ms = 900; s.game = true; return s; }); g.build(); clock_per_frame(g, 16); (void)g.run();
        check(terrain_jobs(g) == g.blit->jobs.size() && g.out->camera_stats().game_only == g.blit->jobs.size() && g.has_line("action=game-camera-only"), "cam-epoch: game camera only"); }
    // (8) lag diagnostic: game camera 3 units from the E6 eye, same orientation -> summary-camera-lag with angle ~0 and position 3
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 10;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.game = true; return s; }); g.build(); clock_per_frame(g, 16); (void)g.run();
        std::string lag; for (auto& l : g.lines) if (l.find("event=summary-camera-lag") != std::string::npos) lag = l;
        const std::string want = "pos-u-max=5.20";   // the rig offsets the game eye by +3 units on every axis: sqrt(27)
        std::string fl; for (auto& l : g.lines) if (l.find("event=camlag;f=") != std::string::npos) { fl = l; break; }
        check(!lag.empty() && lag.find(want) != std::string::npos && lag.find("angle-deg-max=0.0") != std::string::npos && g.out->camera_lag().total_evaluated() > 0,
              "cam-epoch: lag line (" + lag + " want " + want + " f-line " + fl + ")");
        check(lag.size() < feature_log::line_cap, "cam-epoch: lag line fits"); }
    // (9) fixed camera: the admission is inactive; jobs identical with cam_admit on/off, no new lines
    {   Rig a; a.stop_after = 5; hud_full_rig(a); a.settings.world_in_projection = true; auto ra = terrain_rt(true); a.settings.terrain = ra; a.build(); (void)a.run();
        Rig b; b.stop_after = 5; hud_full_rig(b); b.settings.world_in_projection = true; auto rb = terrain_rt(true); rb->conv.cam_admit = true; b.settings.terrain = rb; b.build(); (void)b.run();
        bool same = a.blit->jobs.size() == b.blit->jobs.size() && !a.blit->jobs.empty();
        for (size_t i = 0; same && i < a.blit->jobs.size(); ++i) same = std::memcmp(&a.blit->jobs[i].terrain_eye, &b.blit->jobs[i].terrain_eye, sizeof a.blit->jobs[i].terrain_eye) == 0;
        check(same && !b.has_line("summary-camera-admit") && !b.has_line("event=camera-admit"), "cam-epoch: fixed camera byte-identical, inactive"); }
    // (10) FAIL CLOSED (Astra 6438481134): terrain occluding replayed objects needs the objects' own camera; E6-only frames refuse it, hybrid frames keep it
    for (int game = 0; game < 2; ++game) {
        Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 6; g.settings.objects_depth_fix = true;
        setup(g, cam, rt, [game](uint64_t) { FrameSpec s; s.game = game != 0; return s; }); g.build(); clock_per_frame(g, 16); (void)g.run();
        const auto& st = g.out->camera_stats();
        bool occ = true; for (auto& jb : g.blit->jobs) occ = occ && jb.terrain_occlude;
        bool none = true; for (auto& jb : g.blit->jobs) none = none && !jb.terrain_occlude;
        if (game) check(occ && st.mode[unsigned(acz::terrain::CamMode::correlated_hybrid)] == g.blit->jobs.size() && st.occlude_refused == 0, "cam-epoch: hybrid keeps object occlusion");
        else check(none && st.mode[unsigned(acz::terrain::CamMode::e6_frame)] == g.blit->jobs.size() && st.occlude_refused == g.blit->jobs.size() &&
                   g.has_line("object-registration=refused"), "cam-epoch: e6-only refuses object occlusion");
    }
    // (11) never an old replay orientation with a newer E6 position: game axes WITHOUT a DrawACM_v eye + no own E6 sample (history only) -> E6 alone
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 6;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.own = false; s.game = true; return s; }); g.build();
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&g, &cam, inner] {
            g.clock->t += 16; const int64_t now_us = kBaseUs + g.clock->t * 1000; const float p[3] = {1000, 5000, -2000};
            cam.mb.publish(p, 0.1f, -0.785f, 0.3f, 0.0f, (now_us - 12000) * 1000); if (inner) inner(); };
        auto mut = g.src.mutate;
        g.src.mutate = [mut](stereo::StereoFrame& f) { mut(f); f.game_eye_valid = false; };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        check(st.mode[unsigned(acz::terrain::CamMode::e6_fallback)] == g.blit->jobs.size() && st.mode[unsigned(acz::terrain::CamMode::correlated_hybrid)] == 0 &&
              g.out->counters().terrain_src_game == 0, "cam-epoch: game axes are not paired with a history E6 eye"); }
    // (11b) W15 F2: a Present-history / latest E6 sample (no gmax veto: not a same-frame pair) that is grossly far (> 2000 units) from the frame's own game eye must not
    // reach the cockpit animation state; a near one (control) still does. The game camera stays selected in both (game-only).
    for (int src = 0; src < 2; ++src) for (int farcase = 0; farcase < 2; ++farcase) {
        auto crt = std::make_shared<xr_stereo::CockpitRuntime>(); std::string why; crt->opt.anim = true; crt->id = "synthetic_c4";
        check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model_c4(), crt->opt, crt->mesh, why), "f2: cockpit mesh");
        Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 8;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.own = false; s.game = true; return s; });
        if (src == 1) rt->conv.e6_frame = false;   // e6:latest (the newest Present sample)
        g.settings.cockpit = crt; g.build();
        auto inner = g.xr->hook["wait_frame"];
        float x = 1000.0f;
        g.xr->hook["wait_frame"] = [&g, &cam, &x, inner] {
            g.clock->t += 16; const int64_t now_us = kBaseUs + g.clock->t * 1000; x += 1.0f; const float p[3] = {x, 5000, -2000};
            cam.mb.publish(p, 0.1f, -0.785f, 0.3f, 0.0f, (now_us - 12000) * 1000); if (inner) inner(); };
        auto mut = g.src.mutate;
        g.src.mutate = [mut, farcase](stereo::StereoFrame& f) { mut(f); f.game_eye[0] = 1000.0f + (farcase ? 50000.0f : 0.0f); f.game_eye[1] = 5000; f.game_eye[2] = -2000; };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        const auto want_src = src == 0 ? CamSource::fallback : CamSource::latest;
        check(st.src[unsigned(want_src)] > 0 && st.src[unsigned(CamSource::frame)] == 0 && g.out->counters().terrain_src_game > 0, "f2: non-frame E6 source with the game camera selected");
        if (farcase) check(g.out->counters().cockpit_anim_updates == 0, std::string("f2: far ") + (src ? "latest" : "history") + " E6 sample does not feed the animation (" + std::to_string(g.out->counters().cockpit_anim_updates) + ")");
        else check(g.out->counters().cockpit_anim_updates > 0, std::string("f2: near ") + (src ? "latest" : "history") + " E6 sample still feeds the animation");
    }
    // (12) gmax refusal -> e6-only-game-refused (not counted as hybrid)
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 6;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.game = true; return s; }); g.build(); clock_per_frame(g, 16);
        auto mut = g.src.mutate;
        g.src.mutate = [mut](stereo::StereoFrame& f) { mut(f); for (int i = 0; i < 3; ++i) std::swap(f.game_cam_right[i], f.game_cam_fwd[i]); for (int i = 0; i < 3; ++i) f.game_cam_fwd[i] = -f.game_cam_fwd[i]; };
        (void)g.run();
        const auto& st = g.out->camera_stats();
        check(st.mode[unsigned(acz::terrain::CamMode::e6_game_refused)] == g.blit->jobs.size() && st.mode[unsigned(acz::terrain::CamMode::correlated_hybrid)] == 0,
              "cam-epoch: gmax-refused game camera is e6-only"); }
    // (13) camlag evidence lines: columns, one f line per composed frame (mode, seam-B sample identity), p lines, reuse lines; every line within the cap
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 10;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.game = true; return s; }); g.build();
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&g, &cam, inner] {
            g.clock->t += 16; const int64_t now_us = kBaseUs + g.clock->t * 1000; const float p[3] = {1000, 5000, -2000};
            cam.mb.publish(p, 0.1f, -0.785f, 0.3f, 0.0f, (now_us - 3000) * 1000, (now_us - 3100) * 1000); if (inner) inner(); };
        (void)g.run();
        unsigned fl = 0, pl = 0, cols = 0; bool fits = true, hybrid = true;
        for (auto& l : g.lines) {
            if (l.find("event=camlag-columns") != std::string::npos) ++cols;
            if (l.find("event=camlag;f=") != std::string::npos) { ++fl; hybrid = hybrid && l.find(",correlated-hybrid,frame,ok,ok,") != std::string::npos; }
            if (l.find("event=camlag;p=") != std::string::npos) ++pl;
            if (l.find("camlag") != std::string::npos) fits = fits && l.size() < feature_log::line_cap;
        }
        check(cols == 2 && fl == g.blit->jobs.size() && fl > 0 && hybrid && pl > 0 && fits, "cam-epoch: camlag lines (f=" + std::to_string(fl) + " p=" + std::to_string(pl) + ")"); }
    // (14) review 2 (Astra P2 #4): an ADMITTED own-frame sample keeps the producer's read-start time: the 'f' line carries the exact nonzero
    //      e6_read_us / e6_pub_us of that frame's sample (columns 9 and 10 of v=1)
    {   Rig g; Cam cam; std::shared_ptr<xr_stereo::TerrainRuntime> rt; g.stop_after = 8;
        setup(g, cam, rt, [](uint64_t) { FrameSpec s; s.game = true; return s; }); g.build(); clock_per_frame(g, 16);
        std::map<uint64_t, std::pair<int64_t, int64_t>> want;   // frame id -> (read start ns, publication ns)
        auto mut = g.src.mutate;
        g.src.mutate = [mut, &want](stereo::StereoFrame& f) { mut(f); f.e6_t_read_ns = f.e6_t_ns - 700000; want[f.frame_id] = {f.e6_t_read_ns, f.e6_t_ns}; };
        (void)g.run();
        unsigned checked = 0, bad = 0;
        for (auto& l : g.lines) {
            const size_t p = l.find("event=camlag;f=");
            if (p == std::string::npos) continue;
            std::vector<std::string> col; std::string cur;
            for (char ch : l.substr(p + 15)) { if (ch == ',') { col.push_back(cur); cur.clear(); } else cur += ch; }
            col.push_back(cur);
            if (col.size() < 11 || col[5] != "frame" || col[6] != "ok") { ++bad; continue; }
            const auto it = want.find(std::stoull(col[0]));
            if (it == want.end()) { ++bad; continue; }
            const long long rd = std::stoll(col[9]), pb = std::stoll(col[10]);
            if (rd == 0 || pb == 0 || rd != it->second.first / 1000 || pb != it->second.second / 1000 || pb - rd != 700) ++bad;
            ++checked;
        }
        check(checked > 0 && checked == g.blit->jobs.size() && bad == 0, "cam-epoch: admitted frame sample keeps its exact read-start/publication times on the f line (" +
              std::to_string(checked) + " checked, " + std::to_string(bad) + " bad)"); }
}
void terrain_consumer_runs() {
    terrain_world1_camera_runs();
    terrain_cam_epoch_runs();
    // key off (no TerrainRuntime): no terrain job, no terrain counters, no terrain log line.
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty(), "terrain off run");
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.terrain || jb.terrain_stage; check(!any, "key off: no terrain job fields");
      check(g.out->counters().terrain_frames == 0 && g.out->counters().terrain_no_camera == 0 && !g.has_line("event=terrain"), "key off: no counters or lines"); }
    // on with a fixed camera: every composite job carries both eye cameras derived from the SAME camera frame and the per-eye anchor origin.
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = terrain_rt(true); g.settings.terrain = rt; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty() && g.blit->jobs[0].world_composite, "terrain on run");
      const auto& jb = g.blit->jobs[0];
      check(jb.terrain && !jb.terrain_blend && jb.terrain_stage == &rt->stage && jb.terrain_atlas == &rt->atlas && jb.terrain_gen == rt->generation, "terrain job fields");
      acz::terrain::CamFrame cf{}; const float campos[3] = {1000, 5000, -2000};
      check(acz::terrain::camera_frame(campos, 0, rt->conv.cam[3] * 3.14159265358979f / 180.0f, 0, rt->conv, cf), "camera frame");
      for (unsigned e = 0; e < 2; ++e) {
          float w[3]; acz::terrain::eye_world(cf, jb.world_eye[e].origin, rt->conv.head_units_per_metre, w);
          for (int i = 0; i < 3; ++i) check(std::fabs(w[i] - jb.terrain_eye[e].eye[i]) < 1e-3f && jb.terrain_eye[e].fwd[i] == cf.fwd[i] && jb.terrain_eye[e].right[i] == cf.right[i], "per-eye world position and basis");
      }
      check(jb.terrain_eye[0].eye[0] != jb.terrain_eye[1].eye[0] || jb.terrain_eye[0].eye[2] != jb.terrain_eye[1].eye[2], "the two eyes differ (IPD applied once)");
      check(!jb.occlude_window && !jb.world_cache, "no window occlusion / cache with the terrain");
      check(g.out->counters().terrain_frames > 0 && g.out->counters().terrain_failed == 0 && g.has_line("event=terrain;") && g.has_line("event=summary-terrain"), "counters and lines");
      check(g.has_line("provisional=1") && g.has_line("unverified=live-registration"), "log flags the provisional conventions");
      for (auto& line : g.lines) if (line.find("terrain") != std::string::npos) check(line.size() < feature_log::line_cap, "terrain lines fit the router: " + line); }
    // blend toggle reaches the job
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.terrain = terrain_rt(true, true); g.build(); (void)g.run();
      check(!g.blit->jobs.empty() && g.blit->jobs[0].terrain && g.blit->jobs[0].terrain_blend, "blend flag"); }
    // no camera published: the frame keeps the game's world exactly as with the key off (job identical in the world fields), counted.
    { Rig a; a.stop_after = 6; hud_full_rig(a); a.settings.world_in_projection = true; a.build(); (void)a.run();
      Rig b; b.stop_after = 6; hud_full_rig(b); b.settings.world_in_projection = true; auto rt = terrain_rt(false);
      rt->camera = [](acz::terrain::CamSample&) { return false; }; b.settings.terrain = rt; b.build(); (void)b.run();
      check(!a.blit->jobs.empty() && a.blit->jobs.size() == b.blit->jobs.size(), "same number of jobs");
      for (size_t i = 0; i < a.blit->jobs.size(); ++i) {
          check(!b.blit->jobs[i].terrain, "no camera: no terrain");
          check(std::memcmp(&a.blit->jobs[i].world_eye, &b.blit->jobs[i].world_eye, sizeof a.blit->jobs[i].world_eye) == 0 && a.blit->jobs[i].world_composite == b.blit->jobs[i].world_composite &&
                a.blit->jobs[i].occlude_window == b.blit->jobs[i].occlude_window && a.blit->jobs[i].world_cache == b.blit->jobs[i].world_cache, "no camera: world fields identical to the key-off job");
      }
      check(b.out->counters().terrain_no_camera > 0 && b.out->counters().terrain_frames == 0, "no-camera counted"); }
    // a camera lambda supplying E6-like values (pitch/yaw/roll) is used; non-finite values are refused.
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = terrain_rt(false);
      rt->camera = [](acz::terrain::CamSample& c) { c.campos[0] = 10; c.campos[1] = 20; c.campos[2] = 30; c.pitch = 0; c.yaw = std::nanf(""); c.roll = 0; return true; };
      g.settings.terrain = rt; g.build(); (void)g.run();
      check(!g.blit->jobs.empty() && !g.blit->jobs[0].terrain && g.out->counters().terrain_no_camera > 0, "non-finite camera refused"); }
    // terrain needs the in-projection composite: without the key the setting is dropped and the run is the plain quad path.
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.terrain = terrain_rt(true); g.build(); (void)g.run();
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.terrain; check(!any && !g.has_line("event=terrain;"), "terrain without world_in_projection is inert"); }
    // backend could not build the resources: counted, the run continues
    { Rig g; g.stop_after = 5; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.terrain = terrain_rt(true); g.blit->terrain_unreadable = true; g.build(); auto r = g.run();
      check(r.success && g.out->counters().terrain_failed > 0, "backend failure counted, run continues"); }
}

// COCKPIT-3 (cockpit-render-impl.md section 11): the world-quad HUD (hud_world_quad: seat anchor n, heading yaw0, the game lens tangents) and the model cockpit
// (acz::cockpit::eye_camera: pvb eye + upm * Ry(-yaw0)(eye - n)) are the SAME rigid frame. Projected through the per-eye model camera of any head pose:
//  (a) the HUD corners lie on the game-lens rays from the pvb eye (the model was measured to register with the game camera through that lens),
//  (b) at hudglass:1 (HUD distance = the glass distance) the HUD centre and the model point on the same ray at the glass distance coincide for every
//      head rotation AND translation (the HUD does not move relative to the cockpit),
//  (c) at another distance they coincide under rotation only; a lean shows the parallax of the two distances (bounded, reported).
static void quat_rotate(const float q[4], const float v[3], float o[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
static void quat_from_ypr(float yaw, float pitch, float roll, float q[4]) {   // Ry Rx Rz, (x,y,z,w)
    const float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2), cr = std::cos(roll / 2), sr = std::sin(roll / 2);
    const float a[4] = {0, sy, 0, cy}, b[4] = {sp, 0, 0, cp}, c[4] = {0, 0, sr, cr};
    auto mul = [](const float* x, const float* y, float* o) {
        o[0] = x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1]; o[1] = x[3] * y[1] - x[0] * y[2] + x[1] * y[3] + x[2] * y[0];
        o[2] = x[3] * y[2] + x[0] * y[1] - x[1] * y[0] + x[2] * y[3]; o[3] = x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2]; };
    float t[4]; mul(a, b, t); mul(t, c, q);
}
void cockpit_hud_registration() {
    namespace K = acz::cockpit;
    const float n[3] = {0.1f, 1.2f, -0.4f}, yaw0 = 0.3f, upm = 5.0f;
    const float lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};   // the game's cockpit lens (63.2 x 38.2 deg)
    const float pvb[3] = {0.0f, 3.575f, -36.3f};
    const float fov[4] = {-0.9f, 0.85f, 0.8f, -0.75f};
    const float glass_u = 1.974f;                                    // j35j reflector glass, 0.395 m (cockpit_model --load)
    auto local_to_model = [&](const float p[3], float m[3]) {
        const float rx = p[0] - n[0], ry = p[1] - n[1], rz = p[2] - n[2], c = std::cos(yaw0), s = std::sin(yaw0);
        m[0] = pvb[0] + upm * (c * rx - s * rz); m[1] = pvb[1] + upm * ry; m[2] = pvb[2] + upm * (s * rx + c * rz); };
    auto ndc = [](const K::EyeCam& c, const float p[3], float o[2]) {
        float v[4]; for (int r = 0; r < 4; ++r) v[r] = c.m[4 * r] * p[0] + c.m[4 * r + 1] * p[1] + c.m[4 * r + 2] * p[2] + c.m[4 * r + 3];
        o[0] = v[0] / v[3]; o[1] = v[1] / v[3]; };
    const float poses[][6] = {{0, 0, 0, 0, 0, 0}, {25, 0, 0, 0, 0, 0}, {-15, 10, 0, 0, 0, 0}, {5, -8, 20, 0, 0, 0}, {0, 0, 0, 0.05f, 0, 0}, {10, 5, -10, -0.04f, 0.03f, 0.06f}};
    for (float hud_m : {glass_u / upm, 0.5f}) {
        const xr_stereo::Quad q = xr_stereo::hud_world_quad(n, lens, 40.0f, xr_stereo::clamp_hud_quad_distance(hud_m), 1.0f, yaw0);
        const float d_m = xr_stereo::clamp_hud_quad_distance(hud_m);
        check(std::fabs(d_m - hud_m) < 1e-6f, "HUD distance not clamped (0.395 m >= the 0.3 m minimum)");
        // (a) corners on the lens rays from the pvb eye (model frame, neutral head)
        for (int cx = -1; cx <= 1; cx += 2) for (int cy = -1; cy <= 1; cy += 2) {
            const float off[3] = {cx * q.width / 2, cy * q.height / 2, 0}; float r[3]; quat_rotate(q.pose.orientation, off, r);
            const float pl[3] = {q.pose.position[0] + r[0], q.pose.position[1] + r[1], q.pose.position[2] + r[2]}; float pm[3]; local_to_model(pl, pm);
            const float az = -(pm[2] - pvb[2]);
            check(az > 0 && std::fabs((pm[0] - pvb[0]) / az - cx * lens[0]) < 1e-4f && std::fabs((pm[1] - pvb[1]) / az - cy * lens[2]) < 1e-4f,
                "HUD corner on the game-lens ray from the pvb eye (model frame)");
        }
        float centre_m[3]; local_to_model(q.pose.position, centre_m);
        const float on_glass[3] = {pvb[0], pvb[1], pvb[2] - glass_u};   // the model point on the HUD-centre ray at the glass distance
        float worst_rot = 0, worst_lean = 0;
        for (const auto& P : poses) {
            float qh[4]; quat_from_ypr(P[0] * 0.0174533f + yaw0, P[1] * 0.0174533f, P[2] * 0.0174533f, qh);
            const float pos[3] = {n[0] + P[3], n[1] + P[4], n[2] + P[5]};
            K::EyeCam cam; check(K::eye_camera(qh, pos, fov, n, yaw0, pvb, upm, 0.05f, cam), "eye camera");
            float a[2], b[2]; ndc(cam, centre_m, a); ndc(cam, on_glass, b);
            const float err = std::hypot(a[0] - b[0], a[1] - b[1]);
            if (P[3] == 0 && P[4] == 0 && P[5] == 0) worst_rot = std::max(worst_rot, err); else worst_lean = std::max(worst_lean, err);
        }
        std::printf("  HUD at %.3f m vs glass %.3f m: rotation max %.6f NDC, lean max %.4f NDC\n", d_m, glass_u / upm, worst_rot, worst_lean);
        check(worst_rot < 1e-4f, "head ROTATION: the HUD does not move relative to the model cockpit");
        if (std::fabs(d_m - glass_u / upm) < 1e-4f) check(worst_lean < 1e-4f, "hudglass:1: head TRANSLATION does not move the HUD relative to the glass");
        else check(worst_lean > 1e-3f && worst_lean < 0.1f, "other HUD distance: a lean shows a bounded parallax against the glass");
    }
}
// ---------------------------------------------------------------- COCKPIT-2 (consumer side)
std::shared_ptr<xr_stereo::CockpitRuntime> cockpit_rt(bool keep_replay = false) {
    auto rt = std::make_shared<xr_stereo::CockpitRuntime>();
    std::string why;
    check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model(), rt->opt, rt->mesh, why), "synthetic mesh builds");
    rt->opt.keep_replay = keep_replay; rt->id = "synthetic";
    return rt;
}
// ---------------------------------------------------------------- HUD-MENU (hud_menu.h; docs/world-render/hud-menu.md)
void hud_menu_tests() {
    namespace M = acz::hudmenu;
    const float lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    // (1) scale_lens_about: s = 1 is bit-identical; pivot 0 on a symmetric lens = scale_quad about the quad centre; the pivot ray is fixed.
    { float o[4]; check(M::scale_lens_about(lens, 1.0f, 0.03f, -0.02f, o) && std::memcmp(o, lens, sizeof o) == 0, "scale 1: lens unchanged bit for bit");
      check(M::scale_lens_about(lens, 0.8f, 0.0f, 0.0f, o), "scale 0.8 accepted");
      const float n[3] = {0.1f, 1.2f, -0.4f};
      const xr_stereo::Quad ref = xr_stereo::hud_world_quad(n, lens, 500.0f, 10.0f, 0.8f, 0.3f), piv = xr_stereo::hud_world_quad(n, o, 500.0f, 10.0f, 1.0f, 0.3f);
      check(std::fabs(ref.width - piv.width) < 1e-4f && std::fabs(ref.height - piv.height) < 1e-4f && std::fabs(ref.pose.position[0] - piv.pose.position[0]) < 1e-4f &&
            std::fabs(ref.pose.position[1] - piv.pose.position[1]) < 1e-4f && std::fabs(ref.pose.position[2] - piv.pose.position[2]) < 1e-4f, "pivot 0 = scale about the quad centre");
      const float gl[4] = {std::tan(12.6f / 57.29578f), std::tan(12.8f / 57.29578f), std::tan(8.5f / 57.29578f), std::tan(9.3f / 57.29578f)};   // j35j glass (step62 log)
      float px, py; M::glass_pivot(gl, px, py);
      check(std::fabs(std::atan(px) * 57.29578f - 0.1f) < 0.02f && std::fabs(std::atan(py) * 57.29578f + 0.41f) < 0.02f, "j35j glass centre ~(+0.1, -0.4) deg off the lens axis");
      for (float s : {0.5f, 0.8f, 1.25f}) {
          check(M::scale_lens_about(lens, s, px, py, o), "glass pivot scale");
          // the pivot keeps its tangent: (pivot - left edge) / width is the same fraction before and after
          const float fx0 = (px + lens[0]) / (lens[0] + lens[1]), fx1 = (px + o[0]) / (o[0] + o[1]);
          const float fy0 = (lens[2] - py) / (lens[2] + lens[3]), fy1 = (o[2] - py) / (o[2] + o[3]);
          check(std::fabs(fx0 - fx1) < 1e-5f && std::fabs(fy0 - fy1) < 1e-5f, "the glass-centre texel stays on the glass-centre ray");
          check(std::fabs((o[0] + o[1]) - s * (lens[0] + lens[1])) < 1e-5f && std::fabs((o[2] + o[3]) - s * (lens[2] + lens[3])) < 1e-5f, "size scales by s");
      }
      check(!M::scale_lens_about(lens, 0.0f, 0, 0, o) && !M::scale_lens_about(lens, std::nanf(""), 0, 0, o) && !M::scale_lens_about(lens, 0.8f, INFINITY, 0, o), "bad inputs refused");
      std::printf("  HUD scale 0.8 about the glass: error at 5/10/20/31.6 deg = %.2f/%.2f/%.2f/%.2f deg\n", M::registration_error_deg(0, std::tan(5 / 57.29578), 0.8),
                  M::registration_error_deg(0, std::tan(10 / 57.29578), 0.8), M::registration_error_deg(0, std::tan(20 / 57.29578), 0.8), M::registration_error_deg(0, 0.6152, 0.8)); }
    // (2) detector: enters after kMenuEnterFrames identical cameras, exits at once on motion / invalid camera.
    // Sequences from the live evidence (per-5-s HUD draws, step5-producer continuous-hud): step83 R1 runway parked 28 s at 51-54 draws with a frozen
    // camera (the false trigger), R1 pause 94-102, test19 flight 54-65 / pause 93-106, R1b flight 51-61.
    { const float fw[3] = {0, 0, -1};
      M::Detector d; float p[3] = {0, 100, 0};
      auto fly = [&](unsigned n, uint32_t draws) { for (unsigned i = 0; i < n; ++i) { p[2] -= 0.4f; (void)d.update(true, p, fw, draws + (i % 7)); } };
      // (a) R1 takeoff sequence: frozen camera, 51-54 draws for 28 s -> never enters; the frozen frames are counted as rejected
      bool any = false; for (unsigned i = 0; i < 28 * 60; ++i) any = d.update(true, p, fw, 51u + (i % 4u)) || any;
      check(!any && d.enters() == 0 && d.frozen_rejected() > 1600, "parked on the runway (frozen camera, 51-54 HUD draws): no menu");
      // (b) flight, then a real pause (R1 96 draws): enter at exactly kMenuEnterFrames (0.33 s at 60 Hz), not before
      fly(600, 54);
      unsigned at = 0; for (unsigned i = 1; i <= 60 && !at; ++i) if (d.update(true, p, fw, 96)) at = i;
      check(at == M::kMenuEnterFrames && d.enters() == 1, "real pause enters after 20 frozen menu frames (~0.33 s)");
      // (c) a one-frame camera change inside the pause (R1: exit/re-enter within 0.3 s at 96 draws) does not exit
      p[0] += 0.01f; bool still = d.update(true, p, fw, 96); for (unsigned i = 0; i < 30; ++i) still = d.update(true, p, fw, 96) && still;
      check(still && d.exits() == 0, "short camera change during the pause keeps the menu");
      // (d) a long pause does not raise the baseline: after 3000 menu frames the threshold is still max(80, flight + 25)
      for (unsigned i = 0; i < 3000; ++i) (void)d.update(true, p, fw, 96 + (i % 7));
      check(d.active() && d.threshold() < 92 && d.baseline() < 62, "the pause never feeds the baseline");
      // (e) resume (flight 58 draws, moving): exit within kMenuExitSignatureFrames
      unsigned ex = 0; for (unsigned i = 1; i <= 10 && !ex; ++i) { p[2] -= 0.4f; if (!d.update(true, p, fw, 58)) ex = i; }
      check(ex == M::kMenuExitSignatureFrames && d.exits() == 1, "resume exits within 3 frames");
      // (f) test19 numbers: flight 55-65, pause at 93 -> enters; again after a resume
      fly(600, 58);
      at = 0; for (unsigned i = 1; i <= 60 && !at; ++i) if (d.update(true, p, fw, 93)) at = i;
      check(at == M::kMenuEnterFrames && d.enters() == 2, "test19 pause (93 draws over a 58-64 flight baseline) enters");
      // (g) the menu signature with a MOVING camera never enters (a busy flight HUD); a camera moving inside the menu exits after 10 frames
      M::Detector m; float q[3] = {0, 0, 0};
      for (unsigned i = 0; i < 400; ++i) { q[2] -= 0.5f; check(!m.update(true, q, fw, 120), "a moving camera never enters"); }
      for (unsigned i = 0; i < 25; ++i) (void)m.update(true, q, fw, 120);
      unsigned mv = 0; for (unsigned i = 1; i <= 20 && !mv; ++i) { q[2] -= 0.5f; if (!m.update(true, q, fw, 120)) mv = i; }
      check(m.enters() == 1 && mv == M::kMenuExitMovingFrames, "camera moving inside the menu exits after 10 frames");
      // (h) invalid camera exits at once
      for (unsigned i = 0; i < 25; ++i) (void)m.update(true, q, fw, 120);
      check(m.active() && !m.update(false, q, fw, 120), "invalid camera exits");
      // (i) no baseline yet (game starts paused): the absolute minimum 80 applies
      M::Detector z; check(z.threshold() == M::kMenuMinDraws, "threshold without a baseline = 80"); }
    // (3) backdrop bits / alpha mirror
    { check(M::backdrop_bits(0.0f) == 0 && M::backdrop_bits(std::nanf("")) == 0 && M::backdrop_bits(-1) == 0, "backdrop off = 0 bits");
      check(M::backdrop_bits(0.6f) == (153u << 24) && M::backdrop_bits(5.0f) == (242u << 24), "0.6 -> 153/255; clamped to 0.95");
      check(M::hud_alpha(0.0f, 0.1f, 0.6f, 0.2f, 0) == 0.6f && M::hud_alpha(0.0f, 0, 0, 0, 0) == 0.0f, "bits 0 = the old alpha rule");
      check(std::fabs(M::hud_alpha(0.0f, 0, 0, 0, M::backdrop_bits(0.6f)) - 0.6f) < 1e-6f && M::hud_alpha(0.0f, 0.9f, 0.1f, 0.1f, M::backdrop_bits(0.6f)) == 0.9f,
            "backdrop raises a transparent texel to the floor, brighter texels keep max(rgb)");
      check((xr_stereo::pack_mode_bits(1, 0) | M::backdrop_bits(0.6f)) % 256u == 1u, "mode byte unchanged by the backdrop bits"); }
    // (4) options
    { std::string id, why; acz::cockpit::Options o;
      check(acz::cockpit::parse_options("m", id, o, why) && o.menu && o.menu_distance == 2.0f && o.menu_backdrop == 0.6f, "menu defaults");
      check(acz::cockpit::parse_options("m;menu:0;menud:3.5;menub:0.4", id, o, why) && !o.menu && o.menu_distance == 3.5f && o.menu_backdrop == 0.4f, "menu options");
      check(!acz::cockpit::parse_options("m;menud:0.4", id, o, why) && !acz::cockpit::parse_options("m;menub:0.96", id, o, why) &&
            !acz::cockpit::parse_options("m;menu:2", id, o, why), "menu options refused out of range"); }
    // (4b) RADAR-BG (docs/world-render/radar-background.md): bits, CPU alpha rule, options.
    { check(M::radar_bg_bits(0.0f) == 0 && M::radar_bg_bits(std::nanf("")) == 0 && M::radar_bg_bits(-1) == 0, "radar-bg off = 0 bits");
      check(M::radar_bg_bits(0.5625f) == (18u << 19) && M::radar_bg_bits(5.0f) == (31u << 19) && M::radar_bg_bits(0.001f) == (1u << 19), "0.5625 -> 18/32; clamped 1..31");
      check((M::radar_bg_bits(0.9f) & M::backdrop_bits(0.95f)) == 0 && (M::radar_bg_bits(0.9f) & 0x7FFFFu) == 0, "radar bits 19..23 overlap neither the backdrop nor bits 0..18");
      const uint32_t rb = M::radar_bg_bits(M::kRadarBgDefault);
      check(M::hud_alpha(1.0f, 0, 0, 0, rb) == 0.5625f, "radar square (stored a 1, black): a = 0.5625");
      check(std::fabs(M::hud_alpha(1.0f, 0.2f, 0.6f, 0.2f, rb) - (0.5625f + 0.4375f * 0.6f)) < 1e-6f, "symbol over the square: rb + (1-rb)*max(rgb)");
      check(M::hud_alpha(1.0f, 1.0f, 1.0f, 1.0f, rb) == 1.0f, "white symbol stays opaque");
      int same = 0, n = 0;   // every texel whose stored alpha is <= the cap is bit-identical to the old rule (all texels outside the square: a = 0)
      for (int ia = 0; ia <= 20; ++ia) for (int ic = 0; ic <= 20; ++ic) {
          const float a = ia / 20.0f, cv = ic / 20.0f;
          if (a > 0.5625f + 0.4375f * cv) continue;
          ++n; same += M::hud_alpha(a, cv, 0.5f * cv, 0, rb) == M::hud_alpha(a, cv, 0.5f * cv, 0, 0) ? 1 : 0;
      }
      check(n > 200 && same == n, "radar-bg: texels at or below the cap keep the old alpha exactly");
      check(M::hud_alpha(0.0f, 0, 0, 0, rb | M::backdrop_bits(0.6f)) == M::backdrop_of_bits(M::backdrop_bits(0.6f)) &&
            M::hud_alpha(1.0f, 0, 0, 0, rb | M::backdrop_bits(0.6f)) == M::backdrop_of_bits(M::backdrop_bits(0.6f)), "menu floor still applies with radar-bg (0.6 > 0.5625)");
      check(M::hud_alpha(std::nanf(""), 0.3f, 0, 0, rb) == 0.3f, "NaN stored alpha -> max(rgb)");
      std::string id, why; acz::cockpit::Options o;
      check(acz::cockpit::parse_options("m", id, o, why) && !o.radar_bg && o.radar_bg_alpha == 0.5625f, "radarbg defaults: off, 0.5625");
      check(acz::cockpit::parse_options("m;radarbg:1;radarbga:0.4", id, o, why) && o.radar_bg && o.radar_bg_alpha == 0.4f, "radarbg options");
      check(!acz::cockpit::parse_options("m;radarbg:2", id, o, why) && !acz::cockpit::parse_options("m;radarbga:0.99", id, o, why) &&
            !acz::cockpit::parse_options("m;radarbga:0.01", id, o, why), "radarbg options refused out of range"); }
    // (5) consumer: frozen camera -> menu quad (scale 1, menud) + backdrop on the HUD job; moving again -> the configured HUD; glass pivot with xr_hud_scale 0.8.
    { auto rt = std::make_shared<xr_stereo::CockpitRuntime>(); std::string why;
      check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model(), rt->opt, rt->mesh, why), "synthetic mesh builds");
      rt->id = "synthetic";
      rt->mesh.hud.valid = true; rt->mesh.hud.tan_l = 0.20f; rt->mesh.hud.tan_r = 0.24f; rt->mesh.hud.tan_u = 0.14f; rt->mesh.hud.tan_d = 0.18f;   // pivot (0.02, -0.02)
      // frames 1-29: parked on the runway (frozen camera, 52 HUD draws: must NOT enter); 30-44 flight (58); 45-74 paused (96, frozen); 75-90 flight.
      Rig g; g.stop_after = 90; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.hud_layer = true; g.src.with_hud = true;
      g.settings.cockpit = rt; g.settings.hud_scale = 0.8f; g.settings.hud_quad_distance_m = 10.0f;
      int k = 0;
      g.src.mutate = [&k](stereo::StereoFrame& f) {
          ++k; const bool runway = k < 30, paused = k >= 45 && k < 75;
          f.game_cam_valid = f.game_eye_valid = true;
          f.game_cam_fwd[0] = 0; f.game_cam_fwd[1] = 0; f.game_cam_fwd[2] = -1; f.game_cam_up[1] = 1; f.game_cam_right[0] = 1;
          f.game_eye[0] = 0; f.game_eye[1] = 100; f.game_eye[2] = runway ? 0.0f : paused ? -10.0f : -0.5f * float(k);
          f.hud_draws = paused ? 96u : runway ? 52u : 58u;
      };
      g.build(); auto r = g.run();
      check(r.success && g.xr->layers.size() >= 50 && g.blit->jobs.size() == g.xr->layers.size(), "menu run");
      size_t menu = 0, glass = 0, wrong = 0;
      const float* lt = g.src.lens;
      for (size_t i = 0; i < g.xr->layers.size(); ++i) {
          const auto& l = g.xr->layers[i]; const auto& jb = g.blit->jobs[i];
          if (!l.hud) { ++wrong; continue; }
          const float z = -l.hud_pose.position[2];
          if (jb.hud_backdrop_bits) {
              ++menu;
              if (jb.hud_backdrop_bits != M::backdrop_bits(0.6f) || std::fabs(l.hud_width - 2.0f * (lt[0] + lt[1])) > 0.05f || z < 1.5f || z > 2.6f) ++wrong;
          } else {
              ++glass;
              if (std::fabs(l.hud_width - 0.8f * 10.0f * (lt[0] + lt[1])) > 0.05f || z < 9.0f) ++wrong;
          }
      }
      std::printf("  menu run: %zu menu layers, %zu HUD layers, %zu wrong\n", menu, glass, wrong);
      check(menu >= 5 && menu <= 30 && glass >= 20 && wrong == 0, "menu layers only while frozen (after the streak), HUD 0.8 at 10 m otherwise, backdrop only in menu");
      check(g.has_line("event=hud-menu;state=enter") && g.has_line("event=hud-menu;state=exit") && g.has_line("event=hud-menu-config;menu=1") &&
            g.has_line("event=cockpit-hud-registration;hud-scale=0.800;pivot=glass-centre"), "menu / registration lines");
      for (auto& line : g.lines) if (line.find("hud-menu") != std::string::npos || line.find("cockpit-hud-registration") != std::string::npos)
          check(line.size() < feature_log::line_cap, "hud-menu lines fit the router: " + line);
      check(g.has_line("radar-bg=0;radar-bg-alpha=0.0000"), "radar-bg off by default in the config line"); }
    // (6) RADAR-BG plumbing: radarbg:1 -> every HUD job carries the radar bits (also in menu mode, with the backdrop); off -> none (run (5) above).
    { auto rt = std::make_shared<xr_stereo::CockpitRuntime>(); std::string why;
      check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model(), rt->opt, rt->mesh, why), "synthetic mesh builds (radar-bg)");
      rt->id = "synthetic"; rt->opt.radar_bg = true; rt->opt.radar_bg_alpha = 0.5625f;
      Rig g; g.stop_after = 40; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.hud_layer = true; g.src.with_hud = true; g.settings.cockpit = rt;
      int k = 0;
      g.src.mutate = [&k](stereo::StereoFrame& f) {
          ++k; const bool paused = k >= 5;
          f.game_cam_valid = f.game_eye_valid = true;
          f.game_cam_fwd[0] = 0; f.game_cam_fwd[1] = 0; f.game_cam_fwd[2] = -1; f.game_cam_up[1] = 1; f.game_cam_right[0] = 1;
          f.game_eye[0] = 0; f.game_eye[1] = 100; f.game_eye[2] = paused ? -10.0f : -0.5f * float(k);
          f.hud_draws = paused ? 96u : 58u;
      };
      g.build(); auto r = g.run();
      size_t hud = 0, radar = 0, both = 0;
      for (size_t i = 0; i < g.blit->jobs.size(); ++i) {
          const auto& jb = g.blit->jobs[i]; if (!jb.use_hud) continue;
          ++hud; if ((jb.hud_backdrop_bits & M::kRadarBgMask) == M::radar_bg_bits(0.5625f)) ++radar;
          if (jb.hud_backdrop_bits == (M::radar_bg_bits(0.5625f) | M::backdrop_bits(0.6f))) ++both;
      }
      std::printf("  radar-bg run: %zu HUD jobs, %zu with radar bits, %zu with radar + menu backdrop\n", hud, radar, both);
      check(r.success && hud >= 20 && radar == hud && both >= 5, "radar-bg bits on every HUD job, combined with the menu backdrop in menu mode");
      check(g.has_line("radar-bg=1;radar-bg-alpha=0.5625"), "radar-bg config line"); }
}
void cockpit_consumer_runs() {
    // key off: no cockpit job fields, counters or lines (the job is otherwise identical to the cockpit-on one in its world fields)
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty(), "cockpit off run");
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.cockpit || jb.cockpit_mesh || jb.cockpit_keep_replay; check(!any, "key off: no cockpit job fields");
      check(g.out->counters().cockpit_frames == 0 && g.out->counters().cockpit_no_view == 0 && !g.has_line("event=cockpit-model") && !g.has_line("summary-cockpit"), "key off: no counters or lines"); }
    // on: every composite job carries both eye constants; the model eye of each eye is pvb + upm * (the composite's anchor-frame origin of that eye)
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = cockpit_rt(); g.settings.cockpit = rt; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty() && g.blit->jobs[0].world_composite, "cockpit on run");
      for (const auto& jb : g.blit->jobs) {
          check(jb.cockpit && !jb.cockpit_keep_replay && jb.cockpit_mesh == &rt->mesh && jb.cockpit_gen == rt->generation, "cockpit job fields");
          float E[2][3];
          for (unsigned e = 0; e < 2; ++e) {
              for (int i = 0; i < 3; ++i) E[e][i] = rt->mesh.eye[i] + rt->opt.upm * jb.world_eye[e].origin[i];
              const auto& k = jb.cockpit_eye[e];
              float v[4]; for (int row = 0; row < 4; ++row) v[row] = k.m[4 * row] * E[e][0] + k.m[4 * row + 1] * E[e][1] + k.m[4 * row + 2] * E[e][2] + k.m[4 * row + 3];
              check(std::fabs(v[0]) < 1e-3f && std::fabs(v[1]) < 1e-3f && std::fabs(v[3]) < 1e-3f && std::fabs(v[2] - rt->opt.near_u) < 1e-6f, "the model eye = pvb + upm * the composite origin");
              check(k.gain == rt->opt.gain && k.ambient == rt->opt.ambient && k.cut == rt->opt.cut, "material constants from the options");
          }
          const float d = std::sqrt((E[0][0] - E[1][0]) * (E[0][0] - E[1][0]) + (E[0][1] - E[1][1]) * (E[0][1] - E[1][1]) + (E[0][2] - E[1][2]) * (E[0][2] - E[1][2]));
          check(d > 0.0f && d < 1.0f, "the two eyes differ by the IPD at 5 u/m");
      }
      check(g.out->counters().cockpit_frames > 0 && g.out->counters().cockpit_failed == 0 && g.has_line("event=cockpit-model;model=synthetic") && g.has_line("event=summary-cockpit"), "counters and lines");
      check(g.has_line("replay=cockpit-band-dropped(objects-kept)") && g.has_line("frame=cockpit(seat-anchor,yaw0;no-game-look)"), "config line names the replay rule and the frame");
      for (auto& line : g.lines) if (line.find("cockpit-model") != std::string::npos) check(line.size() < feature_log::line_cap, "cockpit lines fit the router: " + line); }
    // replay:1 reaches the job (A/B)
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit = cockpit_rt(true); g.build(); (void)g.run();
      check(!g.blit->jobs.empty() && g.blit->jobs[0].cockpit && g.blit->jobs[0].cockpit_keep_replay, "keep-replay flag"); }
    // the cockpit does not change any world-composite field of the job
    { Rig a; a.stop_after = 6; hud_full_rig(a); a.settings.world_in_projection = true; a.build(); (void)a.run();
      Rig b; b.stop_after = 6; hud_full_rig(b); b.settings.world_in_projection = true; b.settings.cockpit = cockpit_rt(); b.build(); (void)b.run();
      check(!a.blit->jobs.empty() && a.blit->jobs.size() == b.blit->jobs.size(), "same number of jobs");
      for (size_t i = 0; i < a.blit->jobs.size() && i < b.blit->jobs.size(); ++i)
          check(std::memcmp(&a.blit->jobs[i].world_eye, &b.blit->jobs[i].world_eye, sizeof a.blit->jobs[i].world_eye) == 0 && a.blit->jobs[i].world_composite == b.blit->jobs[i].world_composite &&
                a.blit->jobs[i].use_hud == b.blit->jobs[i].use_hud && a.blit->jobs[i].use_effects == b.blit->jobs[i].use_effects, "world/HUD/effects fields unchanged by the cockpit"); }
    // needs the in-projection composite: without it the setting is dropped (inert)
    { Rig g; g.stop_after = 4; hud_full_rig(g); g.settings.cockpit = cockpit_rt(); g.build(); (void)g.run();
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.cockpit; check(!any && !g.has_line("event=cockpit-model;"), "cockpit without world_in_projection is inert"); }
    // backend failure: counted, logged ONCE, the run continues (the replay stays: the drop bit is only set by a backend that drew the model)
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit = cockpit_rt(); g.blit->cockpit_unreadable = true; g.build(); auto r = g.run();
      size_t n = 0; for (auto& l : g.lines) n += l.find("event=cockpit-model-disabled;reason=gpu") != std::string::npos;
      check(r.success && g.out->counters().cockpit_failed > 1 && g.out->counters().cockpit_frames == 0 && n == 1, "backend failure counted, logged once, run continues"); }
    // COCKPIT-4: the config line; anim:1 without E6 (no terrain) poses the animated range at the neutral commands (= the rest pose) for every job;
    // anim:0 sends no posed range
    { Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; auto rt = cockpit_rt(); g.settings.cockpit = rt; g.build(); (void)g.run();
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.cockpit_anim || jb.cockpit_anim_count;
      check(!any && g.has_line("event=cockpit-model-c4;canopy=none-found") && g.has_line("interior=procedural") && g.has_line("anim=off(rest-pose)"), "c4 line; anim:0 sends no pose"); }
    { auto rt = std::make_shared<xr_stereo::CockpitRuntime>(); std::string why; rt->opt.anim = true; rt->id = "synthetic_c4";
      check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model_c4(), rt->opt, rt->mesh, why) && rt->mesh.anim_surfaces == 1, "c4 synthetic mesh with anim");
      Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit = rt; g.build(); auto r = g.run();
      check(r.success && !g.blit->jobs.empty(), "c4 anim run");
      for (const auto& jb : g.blit->jobs) {
          check(jb.cockpit_anim != nullptr && jb.cockpit_anim_count == rt->mesh.anim_vertices, "anim:1: the posed range rides on every cockpit job");
          float worst = 0;
          for (uint32_t i = 0; jb.cockpit_anim && i < jb.cockpit_anim_count; ++i)
              for (int a = 0; a < 3; ++a) worst = std::max(worst, std::fabs(jb.cockpit_anim[i].pos[a] - rt->mesh.v[rt->mesh.anim_first + i].pos[a]));
          check(worst < 1e-4f, "no E6 rates: neutral commands = the rest pose");
      }
      check(g.has_line("event=cockpit-model-c4;canopy=cockpit-part;canopy-parts=1") && g.has_line("anim=on;anim-surfaces=1") && g.has_line(";anim-updates=0;"),
            "c4 line names the canopy source and the driven surfaces; summary counts no E6 updates");
      for (auto& line : g.lines) if (line.find("cockpit-model-c4") != std::string::npos || line.find("summary-cockpit") != std::string::npos)
          check(line.size() < feature_log::line_cap, "c4 lines fit the router: " + line); }
    // CPU mirror of the replay-drop rule
    check(xr_stereo::model_cockpit_cover(true, 0.5f, 1.0f) == 0.0f && xr_stereo::model_cockpit_cover(true, 0.0005f, 1.0f) == 1.0f &&
          xr_stereo::model_cockpit_cover(true, 0.0f, 0.0f) == 0.0f && xr_stereo::model_cockpit_cover(false, 0.0005f, 1.0f) == 0.0f &&
          xr_stereo::model_cockpit_cover(true, 0.0005f, 0.25f) == 0.25f, "model_cockpit_cover: object band kept, cockpit band and no-depth dropped");
    cockpit_hud_registration();
    hud_menu_tests();
}

// ---------------------------------------------------------------- AIRCRAFT-AUTO (consumer side; docs/world-render/aircraft-auto.md)
// A fake eye source (one sample per call, a new sequence each time, the table eye of `type`) and a fake loader (the synthetic mesh under the
// table's model id, a fresh generation per load).
struct AutoFeed {
    int type = -1;              // -1 = no lock: nothing published
    uint32_t epoch = 1; uint64_t seq = 0; bool centred = true;
    bool odd = false;           // publish an eye offset that is no player aircraft (0, 12, -30)
    std::vector<int> loads;     // types the loader was asked for
    uint64_t gen = 500;
};
std::shared_ptr<xr_stereo::CockpitAuto> auto_settings(std::shared_ptr<AutoFeed> fd, bool async, unsigned window = 4, bool fail_load = false) {
    auto ca = std::make_shared<xr_stereo::CockpitAuto>();
    std::string why;
    check(acz::aircraft::parse_auto("auto", ca->cfg, why), "auto config");
    ca->det.window = window;
    ca->det.drop_confirm = 2;   // the default (25 windows) is covered by aircraft_auto_offline; these rigs check the unload path itself
    ca->async = async;
    ca->grace_ms = 0;   // the rigs below test the plain stale rule; the AIRCRAFT-AUTO-GRACE rigs set it explicitly
    ca->sample = [fd](acz::aircraft::EyeSample& e) {
        if (fd->type < 0) return false;
        static const float kOdd[3] = {0.0f, 12.0f, -30.0f};
        const float* eye = fd->odd ? kOdd : acz::aircraft::kAircraft[fd->type].eye;
        for (int i = 0; i < 3; ++i) e.local[i] = eye[i] + 0.01f * float((fd->seq % 3)) - 0.01f;
        e.centred = fd->centred; e.lock_epoch = fd->epoch; e.seq = ++fd->seq;
        return true;
    };
    ca->load = [fd, fail_load](unsigned type, std::string& w) -> std::shared_ptr<xr_stereo::CockpitRuntime> {
        fd->loads.push_back(int(type));
        if (fail_load) { w = "load-failed;status=io-error;path=X:\\none\\" + std::string(acz::aircraft::kAircraft[type].model_id) + ".zvrm"; return nullptr; }
        auto rt = std::make_shared<xr_stereo::CockpitRuntime>();
        rt->opt = acz::aircraft::options_for(acz::aircraft::AutoConfig{}, type);
        check(acz::cockpit::build_mesh(acz::cockpit::make_synthetic_model(), rt->opt, rt->mesh, w), "synthetic mesh builds");
        rt->id = acz::aircraft::kAircraft[type].model_id; rt->generation = ++fd->gen;
        return rt;
    };
    return ca;
}
// AIRCRAFT-AUTO-FAST: assumed model at XR start (remembered file / default:), confirm / switch, the remembered file, per-sample drain.
std::string fast_tmp_dir() {
    const char* t = std::getenv("TEMP");
    static unsigned n = 0;
    return std::string(t && *t ? t : ".") + "\\acz-vr-auto-fast-" + std::to_string(unsigned(reinterpret_cast<uintptr_t>(&n) & 0xffffu)) + "-" + std::to_string(++n);
}
void cockpit_auto_fast_runs() {
    namespace A = acz::aircraft;
    const int f16 = A::find_type("f16c"), j35 = A::find_type("j35j");
    auto every_job_has_model = [](Rig& g) { bool all = !g.blit->jobs.empty(); for (auto& jb : g.blit->jobs) all = all && jb.cockpit && jb.cockpit_mesh; return all; };
    // 1. remembered aircraft = the flown one: the FIRST job already carries the model (no replayed-cockpit frame), one load, then confirm; no rewrite
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      const std::string dir = fast_tmp_dir(); ca->state_path = dir + "\\last-aircraft.json";
      Rig g; g.stop_after = 16; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); auto r = g.run();
      check(r.success && every_job_has_model(g) && g.blit->jobs.front().cockpit_gen == 501, "assumed model on the very first job and every later one");
      check(fd->loads.size() == 1 && fd->loads[0] == j35, "one load: the assumed model (before the session)");
      check(g.has_line("action=assume;type=j35j;model=j35jk_vrcockpit;source=remembered-file;installed=yes") && g.has_line("until-detected=assumed-model"),
            "assume line");
      check(g.has_line("action=first-frame;") && g.has_line("model-drawn=yes;replayed-cockpit=dropped;assumed=j35j"), "first-frame evidence: model drawn, replay dropped");
      check(g.has_line("action=confirm;type=j35j") && !g.has_line("action=switch") && !g.has_line("action=remember"), "confirmed, no switch, no rewrite");
      check(g.has_line("event=cockpit-model;model=j35jk_vrcockpit"), "config lines of the assumed model at setup");
      std::error_code ec; check(!std::filesystem::exists(ca->state_path, ec), "nothing written when the file already names the type");
      for (auto& l : g.lines) if (l.find("cockpit-auto") != std::string::npos) check(l.size() < feature_log::line_cap, "fast lines fit the router: " + l); }
    // 2. default: (no file) and the flown aircraft matches: same first-frame result; the confirmed type is written (atomic temp + rename)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "config-default";
      const std::string dir = fast_tmp_dir(); ca->state_path = dir + "\\last-aircraft.json";
      Rig g; g.stop_after = 16; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); (void)g.run();
      int got = -1; std::string w;
      check(every_job_has_model(g) && g.has_line("action=confirm;type=j35j") && g.has_line("action=remember;type=j35j"), "default confirmed and remembered");
      check(A::read_remembered(ca->state_path, got, w) && got == j35, "the file now names j35j");
      std::error_code ec; std::filesystem::remove_all(dir, ec); }
    // 3. WRONG remembered aircraft (f16c remembered, j35j flown): the F-16 model is drawn from frame 1, two contradicting windows switch to the
    //    Draken; no job ever lacks a model (no replayed cockpit on the way), the file is corrected
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      auto ca = auto_settings(fd, false); ca->assumed = f16; ca->assumed_source = "remembered-file"; ca->remembered = f16;
      const std::string dir = fast_tmp_dir(); ca->state_path = dir + "\\last-aircraft.json";
      Rig g; g.stop_after = 24; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); auto r = g.run();
      std::vector<uint64_t> gens; for (auto& jb : g.blit->jobs) if (jb.cockpit && (gens.empty() || gens.back() != jb.cockpit_gen)) gens.push_back(jb.cockpit_gen);
      check(r.success && every_job_has_model(g) && gens.size() == 2 && fd->loads.size() == 2 && fd->loads[0] == f16 && fd->loads[1] == j35, "f16c then j35j, never no model");
      check(g.has_line("action=switch;from=f16c;to=j35j;reason=assumed-type-contradicted;windows=2") && g.has_line("action=installed;type=j35j") &&
            g.has_line("replaced=yes") && !g.has_line("action=confirm") && !g.has_line("action=unload"), "switch logged; replaced, not unloaded");
      size_t first_j35 = SIZE_MAX; for (size_t i = 0; i < g.blit->jobs.size(); ++i) if (g.blit->jobs[i].cockpit_gen == gens.back()) { first_j35 = i; break; }
      check(first_j35 >= 7, "the switch needs two full windows (hysteresis kept for switches)");
      int got = -1; std::string w; check(A::read_remembered(ca->state_path, got, w) && got == j35, "remembered file corrected to j35j");
      std::error_code ec; std::filesystem::remove_all(dir, ec); }
    // 4. an assumed model that does not load (file missing): logged, the plain detector (first window chooses) and the replayed cockpit until then
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f16;
      auto ca = auto_settings(fd, false, 4, true); ca->assumed = j35; ca->assumed_source = "config-default";
      Rig g; g.stop_after = 16; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); (void)g.run();
      check(g.has_line("action=assume-failed;type=j35j") && g.has_line("until-detected=replayed-cockpit") && g.has_line("model-drawn=no;replayed-cockpit=shown;assumed=none"),
            "assume failure falls back to detection");
      check(!g.blit->jobs.front().cockpit && fd->loads.size() == 2 && fd->loads[1] == f16, "then the detected type is tried (also missing here)"); }
    // 5. an assumed model and no E6 lock at all: the model stays for the whole run (no-match/no samples never drop an assumption)
    { auto fd = std::make_shared<AutoFeed>();
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "config-default";
      Rig g; g.stop_after = 20; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); (void)g.run();
      check(every_job_has_model(g) && g.has_line("event=summary-cockpit-auto-start") && g.has_line("assumed=j35j;source=config-default"), "assumed model without a lock"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "config-default";
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, fd] { if (g.publish_each_frame) g.src.publish(); fd->odd = true; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(every_job_has_model(g) && g.has_line("verdict=no-match") && !g.has_line("action=unload"), "no-match windows keep the assumed model"); }
    // 5b. AIRCRAFT-AUTO-STALE (headset test 2026-10-29 item 9, 'The Round Table' F-5E): a remembered/default Draken with NO locked E6 sample for stale: ms is
    //     forgotten -> the game's replayed cockpit (jobs without a model), never the wrong model; when the lock then comes with another aircraft (f05e) the
    //     next matching window loads that one. A running lock keeps the model (samples refresh the clock).
    { const int f05 = A::find_type("f05e");
      auto fd = std::make_shared<AutoFeed>(); fd->type = -1;
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->stale_ms = 15000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 40; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, fd, clock, f05] {
          if (g.publish_each_frame) g.src.publish();
          *clock += 2000.0;                                  // 2 s per frame: the 15 s limit passes at frame ~8
          if (g.xr->waits >= 20) fd->type = f05;             // the locator locks on the F-5E
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      auto r = g.run();
      check(r.success && g.blit->jobs.front().cockpit, "stale: the assumed model is drawn first");
      check(g.has_line("action=stale-forget;type=j35j;was=assumed;") && g.has_line("fallback=replayed-cockpit;next=first-matching-window") && g.has_line("action=unload;type=j35j"),
            "stale: forget + unload logged");
      bool gap = false; for (const auto& jb : g.blit->jobs) gap = gap || !jb.cockpit;
      check(gap, "stale: some frames after the limit carry no model (the game's cockpit)");
      check(!fd->loads.empty() && fd->loads.front() == j35 && fd->loads.back() == f05 && g.has_line("action=installed;type=f05e"), "the F-5E is loaded once its lock matches, never the Draken again");
      check(g.blit->jobs.back().cockpit, "the F-5E model is drawn at the end");
      check(g.has_line("stale-forgets=1"), "summary counts the forget");
      for (auto& l : g.lines) if (l.find("cockpit-auto") != std::string::npos) check(l.size() < feature_log::line_cap, "stale lines fit the router: " + l); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;   // lock running: the clock advances but samples keep arriving -> never stale
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->stale_ms = 15000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 5000.0; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(every_job_has_model(g), "stale: samples keep the model (every job has it)");
      check(!g.has_line("action=stale-forget"), "stale: no forget while samples arrive"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = -1;   // stale:0 = legacy (never forget)
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "config-default"; ca->stale_ms = 0; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 60000.0; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(every_job_has_model(g) && !g.has_line("action=stale-forget"), "stale:0 never forgets"); }
    // 5c. AIRCRAFT-AUTO-GRACE (headset test 7, 2026-10-31, F-5E): the remembered AND correct f05e was forgotten at +15 s because the E6 locator needs
    //     20-25 s to lock (xr-stereo.log: first frame 236876375, stale-forget 236891390, window 1 match 236899828): the cockpit, airframe and wings were
    //     gone for 8.4 s. An assumed model without junk samples is now kept for max(stale, grace) ms.
    { const int f05 = A::find_type("f05e");
      auto fd = std::make_shared<AutoFeed>(); fd->type = -1;
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = f05; ca->assumed_source = "remembered-file"; ca->remembered = f05;
      ca->stale_ms = 15000; ca->grace_ms = 60000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 40; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, fd, clock, f05] {
          if (g.publish_each_frame) g.src.publish();
          *clock += 2000.0;                                  // 2 s per frame
          if (g.xr->waits >= 12) fd->type = f05;             // the locator locks after ~24 s of mission: past the 15 s stale limit
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      auto r = g.run();
      check(r.success && every_job_has_model(g), "grace: the remembered f05e is drawn on every frame, also between 15 s and the lock at 24 s");
      check(!g.has_line("action=stale-forget") && !g.has_line("action=unload") && g.has_line("stale-forgets=0"), "grace: no forget, no unload");
      check(fd->loads.size() == 1 && fd->loads.front() == f05, "grace: the model is loaded once (never reloaded)");
      check(g.has_line("action=confirm;type=f05e") || g.has_line("verdict=match"), "grace: the lock then confirms the assumed type"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = -1;   // no lock for the whole run: the grace ends at 60 s (not 15 s)
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->stale_ms = 15000; ca->grace_ms = 60000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 45; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 2000.0; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      size_t first_gap = SIZE_MAX; for (size_t i = 0; i < g.blit->jobs.size(); ++i) if (!g.blit->jobs[i].cockpit) { first_gap = i; break; }
      check(first_gap != SIZE_MAX && first_gap >= 28, "grace: the model survives the 15 s limit (first model-less job at about 60 s = frame 30, not 8)");
      check(g.has_line("action=stale-forget;type=j35j;was=assumed;") && g.has_line("limit-ms=60000;reason=no-e6-samples(locator-not-locked)"), "grace: without any lock the forget comes at the 60 s grace and logs it"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;   // a CONFIRMED model that goes silent keeps the short stale limit (the grace is for unconfirmed models)
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->stale_ms = 15000; ca->grace_ms = 60000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, fd, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 2000.0; if (g.xr->waits >= 6) fd->type = -1; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(g.has_line("action=stale-forget;type=j35j;was=confirmed;") && g.has_line("limit-ms=15000;"), "grace: a confirmed model that goes silent still forgets at the short limit"); }
    // 6. per-sample drain: several E6 samples per composed frame all count (a window of 4 completes on the first frame with 4 queued samples)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f16;
      auto ca = auto_settings(fd, false); ca->sample = nullptr;
      ca->drain = [fd](uint64_t after, A::EyeSample* out, unsigned cap) -> unsigned {
          unsigned k = 0;
          for (; k < 4 && k < cap; ++k) {
              for (int i = 0; i < 3; ++i) out[k].local[i] = A::kAircraft[fd->type].eye[i];
              out[k].centred = true; out[k].lock_epoch = 1; out[k].seq = after + k + 1;
          }
          return k;
      };
      Rig g; g.stop_after = 6; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); (void)g.run();
      check(g.blit->jobs.size() >= 2 && g.blit->jobs.front().cockpit && g.has_line("action=installed;type=f16c") && g.has_line("samples=4"),
            "4 samples per frame: detected and drawn on the first frame"); }
    // 7. the real mailbox ring: every published sample is drained in order, overruns are counted, a later drain sees only new ones
    { static A::EyeMailbox mb; const float e[3] = {0.0f, 3.575f, -36.3f};
      for (int i = 0; i < 10; ++i) mb.publish(e, i % 2 == 0, 3);
      A::EyeSample buf[A::EyeMailbox::kRing]; uint64_t lost = 0;
      unsigned n = mb.drain(0, buf, A::EyeMailbox::kRing, &lost);
      check(n == 10 && buf[0].seq == 1 && buf[9].seq == 10 && buf[0].centred && !buf[1].centred && buf[5].lock_epoch == 3 && lost == 0, "ring drains every sample");
      check(mb.drain(10, buf, A::EyeMailbox::kRing, &lost) == 0, "nothing new");
      for (int i = 0; i < 100; ++i) mb.publish(e, true, 3);
      n = mb.drain(10, buf, A::EyeMailbox::kRing, &lost);
      check(n == A::EyeMailbox::kRing && buf[n - 1].seq == 110 && lost == 100 - A::EyeMailbox::kRing, "overrun: the newest kRing, the rest counted lost");
      A::EyeSample latest; check(mb.try_read(latest) && latest.seq == 110, "the newest-sample read is unchanged"); }
}

// AIRCRAFT-AUTO-JUNK (headset 2026-10-30 live-1508: F-5E flown, remembered Draken drawn all mission). The content-scan locator locked a STATIC junk block
// (air 0,0,60; cam 0,0,0 -> centred eye 0,0,-60, nearest table eye 8.5 away); its samples kept the stale clock alive, every window was no-match, and
// no-match never drops an assumed type -> the Draken stayed. Now only plausible eyes vouch: the assumption is forgotten after stale: ms (the game's own
// cockpit), and a later real lock (another epoch) on the F-5E loads the F-5E.
void cockpit_auto_junk_lock_runs() {
    namespace A = acz::aircraft;
    const int j35 = A::find_type("j35j"), f05 = A::find_type("f05e");
    check(j35 >= 0 && f05 >= 0, "junk: table types");
    // 1. the plausibility rule itself
    for (unsigned i = 0; i < A::kAircraftCount; ++i) check(A::nearest_eye_distance(A::kAircraft[i].eye) == 0.0f && A::plausible_eye(A::kAircraft[i].eye), "junk: every table eye is plausible");
    { const float wob[3] = {A::kAircraft[j35].eye[0] + 0.15f, A::kAircraft[j35].eye[1] - 0.15f, A::kAircraft[j35].eye[2] + 0.15f};
      const float look[3] = {A::kAircraft[j35].eye[0], A::kAircraft[j35].eye[1], A::kAircraft[j35].eye[2] + 1.0f};   // step50 look-back pivot (~0.8 u)
      check(A::plausible_eye(wob) && A::plausible_eye(look), "junk: ground wobble and look pivot stay plausible"); }
    { const float junk[3] = {0.0f, 0.0f, -60.0f}, odd[3] = {0.0f, 12.0f, -30.0f}, zero[3] = {0.0f, 0.0f, 0.0f}, nan3[3] = {std::nanf(""), 0.0f, 0.0f};
      check(A::nearest_eye_distance(junk) > 8.0f && !A::plausible_eye(junk), "junk: the live-1508 eye 0,0,-60 is implausible");
      check(!A::plausible_eye(odd) && !A::plausible_eye(zero) && !A::plausible_eye(nan3), "junk: off-table / origin / non-finite eyes are implausible"); }
    check(A::kPlausibleEyeDist < 0.5f * 8.498f && A::kPlausibleEyeDist > 4.0f * 0.08f, "junk: the bound sits between the match tolerance and the live junk residual");
    auto every_job_has_model = [](Rig& g) { bool all = !g.blit->jobs.empty(); for (auto& jb : g.blit->jobs) all = all && jb.cockpit && jb.cockpit_mesh; return all; };
    // a feed that publishes the junk block's eye (epoch 1) until `real` is set, then the given aircraft's eye on a new lock epoch
    struct Junk { bool on = true; bool centred = true; int real = -1; uint64_t seq = 0; };
    auto junk_sample = [](std::shared_ptr<Junk> jk) {
        return [jk](A::EyeSample& e) {
            if (jk->real >= 0) { for (int i = 0; i < 3; ++i) e.local[i] = A::kAircraft[jk->real].eye[i]; e.centred = true; e.lock_epoch = 2; e.seq = ++jk->seq; return true; }
            if (!jk->on) return false;
            e.local[0] = 0.0f; e.local[1] = 0.0f; e.local[2] = -60.0f; e.centred = jk->centred; e.lock_epoch = 1; e.seq = ++jk->seq; return true;
        };
    };
    // 2. remembered Draken + junk lock from the start: forgotten after 15 s (reason names the junk), the game's cockpit, then the F-5E once it really locks
    { auto fd = std::make_shared<AutoFeed>(); auto jk = std::make_shared<Junk>();
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->sample = junk_sample(jk); ca->stale_ms = 15000; ca->grace_ms = 60000; ca->now_ms = [clock] { return *clock; };   // grace never covers junk samples
      Rig g; g.stop_after = 40; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, jk, clock, f05] {
          if (g.publish_each_frame) g.src.publish();
          *clock += 2000.0;                                  // 2 s per frame: 15 s pass at frame ~8 although junk samples arrive every frame
          if (g.xr->waits >= 22) jk->real = f05;             // the locator re-locks on the real F-5E block
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      auto r = g.run();
      check(r.success && g.blit->jobs.front().cockpit, "junk: the remembered Draken is drawn first");
      check(g.has_line("verdict=no-match") && g.has_line("eye=0.000,0.000,-60.000"), "junk: the junk windows are no-match");
      check(g.has_line("action=stale-forget;type=j35j;was=assumed;") && g.has_line("reason=implausible-e6-eye(locked-block-not-the-aircraft)") && g.has_line("action=unload;type=j35j"),
            "junk: the Draken is forgotten although junk samples kept arriving");
      bool gap = false; for (const auto& jb : g.blit->jobs) gap = gap || !jb.cockpit;
      check(gap, "junk: frames after the limit carry no model (the game's replayed cockpit)");
      check(!fd->loads.empty() && fd->loads.front() == j35 && fd->loads.back() == f05 && g.has_line("action=installed;type=f05e"), "junk: the real F-5E lock loads the F-5E");
      check(g.blit->jobs.back().cockpit, "junk: the F-5E model is drawn at the end");
      check(g.has_line("stale-forgets=1") && !g.has_line(";implausible=0"), "junk: summary counts the forget and the implausible samples");
      for (auto& l : g.lines) if (l.find("cockpit-auto") != std::string::npos) check(l.size() < feature_log::line_cap, "junk lines fit the router: " + l); }
    // 3. a real lock (plausible samples) keeps the model under the same clock; off-centre samples (look turned: not judged) also keep it
    for (int mode = 0; mode < 2; ++mode) {
      auto fd = std::make_shared<AutoFeed>(); auto jk = std::make_shared<Junk>();
      if (mode == 0) jk->real = j35; else jk->centred = false;
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file"; ca->remembered = j35;
      ca->sample = junk_sample(jk); ca->stale_ms = 15000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 5000.0; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(every_job_has_model(g) && !g.has_line("action=stale-forget"), mode == 0 ? "junk: a real lock never goes stale" : "junk: off-centre samples still vouch (unjudged)");
      if (mode == 0) check(g.has_line(";implausible=0"), "junk: real samples are never counted implausible");
    }
    // 4. the legacy no-lock case keeps its reason
    { auto fd = std::make_shared<AutoFeed>(); auto jk = std::make_shared<Junk>(); jk->on = false;
      auto clock = std::make_shared<double>(1000.0);
      auto ca = auto_settings(fd, false); ca->assumed = j35; ca->assumed_source = "remembered-file";
      ca->sample = junk_sample(jk); ca->stale_ms = 15000; ca->now_ms = [clock] { return *clock; };
      Rig g; g.stop_after = 20; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build();
      g.xr->hook["wait_frame"] = [&g, clock] { if (g.publish_each_frame) g.src.publish(); *clock += 2000.0; if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop(); };
      (void)g.run();
      check(g.has_line("action=stale-forget;type=j35j;") && g.has_line("reason=no-e6-samples(locator-not-locked)"), "junk: no samples at all keeps the no-lock reason"); }
}

void cockpit_auto_consumer_runs() {
    namespace A = acz::aircraft;
    const int f16 = A::find_type("f16c"), j35 = A::find_type("j35j"), f15c = A::find_type("f15c"), f15e = A::find_type("f15e");
    auto strip_auto = [](const std::vector<std::string>& v) { std::vector<std::string> o; for (auto& l : v) if (l.find("cockpit-auto") == std::string::npos) o.push_back(l); return o; };
    // key off vs auto with no E6 lock: the jobs carry no cockpit fields and every line except the cockpit-auto ones is byte-identical
    { Rig a; a.stop_after = 12; hud_full_rig(a); a.settings.world_in_projection = true; a.build(); (void)a.run();
      auto fd = std::make_shared<AutoFeed>();
      Rig b; b.stop_after = 12; hud_full_rig(b); b.settings.world_in_projection = true; b.settings.cockpit_auto = auto_settings(fd, false); b.build(); (void)b.run();
      check(!a.has_line("cockpit-auto") && b.has_line("event=cockpit-auto;state=armed") && b.has_line("event=summary-cockpit-auto"), "auto lines only with the key");
      bool any = false; for (auto& jb : b.blit->jobs) any = any || jb.cockpit || jb.cockpit_mesh; check(!any && fd->loads.empty(), "no lock: no model, no load");
      check(a.blit->jobs.size() == b.blit->jobs.size() && strip_auto(a.lines) == strip_auto(b.lines), "no lock: everything else identical to the key-off run");
      check(b.has_line("current=none;drawn=none;loading=none"), "summary names the empty state"); }
    // detection -> inline load -> the model rides on the following jobs; the stick style comes from the table (F-16: side)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f16;
      Rig g; g.stop_after = 16; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false); g.build(); auto r = g.run();
      check(r.success && fd->loads.size() == 1 && fd->loads[0] == f16, "one load of the detected type");
      size_t first = SIZE_MAX; uint64_t gen = 0;
      for (size_t i = 0; i < g.blit->jobs.size(); ++i) if (g.blit->jobs[i].cockpit) { if (first == SIZE_MAX) { first = i; gen = g.blit->jobs[i].cockpit_gen; } }
      check(first != SIZE_MAX && first >= 3 && gen == 501, "no model before the first full window, then the loaded generation");
      for (size_t i = first; i < g.blit->jobs.size(); ++i) check(g.blit->jobs[i].cockpit && g.blit->jobs[i].cockpit_gen == gen, "model on every later job");
      check(g.has_line("event=cockpit-auto;window=1;verdict=match;candidates=f16c:") && g.has_line("chosen=f16c;model=f16ck_vrcockpit") &&
            g.has_line("action=installed;type=f16c;model=f16ck_vrcockpit") && g.has_line("stick=side") && g.has_line("event=cockpit-model;model=f16ck_vrcockpit"),
            "window, install and config lines");
      for (auto& l : g.lines) if (l.find("cockpit-auto") != std::string::npos) check(l.size() < feature_log::line_cap, "auto lines fit the router: " + l); }
    // asynchronous load (the production path): the model appears a few frames later, never inside the frame that asked
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      Rig g; g.stop_after = 400; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, true); g.build();
      g.xr->hook["wait_frame"] = [&g] {   // fake frames take no time: give the worker real time (2 ms per frame, <= 0.8 s) under a parallel ctest
          if (g.publish_each_frame) g.src.publish();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          if (g.has_line("action=installed;type=j35j") || g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      auto r = g.run();
      bool on = false; for (auto& jb : g.blit->jobs) on = on || jb.cockpit;
      check(r.success && on && g.has_line("action=installed;type=j35j") && g.has_line("thread=worker"), "worker-thread load installs"); }
    // F-15 tie: reported, default rule = first in table order (f15c); prefer:f15e picks the E
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f15e;
      Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false); g.build(); (void)g.run();
      check(!fd->loads.empty() && fd->loads[0] == f15c && g.has_line("ties=f15c+f15e+f15s;tie-rule=first-in-table"), "F-15 tie -> f15c"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f15c;
      auto ca = auto_settings(fd, false); std::string why; check(A::parse_auto("auto;prefer:f15e", ca->cfg, why), "prefer parses"); ca->det.prefer = ca->cfg.prefer;
      Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = ca; g.build(); (void)g.run();
      check(!fd->loads.empty() && fd->loads[0] == f15e && g.has_line("tie-rule=prefer") && g.has_line("prefer=f15e"), "prefer:f15e"); }
    // a missing model file: logged once, never retried, no model (the replayed cockpit)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f16;
      Rig g; g.stop_after = 30; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false, 4, true); g.build(); auto r = g.run();
      bool any = false; for (auto& jb : g.blit->jobs) any = any || jb.cockpit;
      size_t n = 0; for (auto& l : g.lines) n += l.find("action=load-failed;type=f16c") != std::string::npos;
      check(r.success && !any && fd->loads.size() == 1 && n == 1 && g.has_line("fallback=replayed-cockpit"), "missing model: one failure line, replay kept"); }
    // the real loader with a folder that does not hold the file: io_error, null
    { A::AutoConfig c; std::string why; check(A::parse_auto("auto", c, why), "auto");
      auto rt = xr_stereo::load_auto_model(c, unsigned(f16), "Z:\\acz-vr-nonexistent", why);
      check(!rt && why.find("status=io-error") != std::string::npos && why.find("f16ck_vrcockpit.zvrm") != std::string::npos, "load_auto_model: missing file refused"); }
    // switching aircraft mid-session (new lock epoch = mission change): the old model stays until the new one is confirmed and loaded, then the job
    // generation changes; a feed that stops matching any type unloads after the confirm windows
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      Rig g; g.stop_after = 70; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false); g.build();
      g.xr->hook["wait_frame"] = [&g, fd, f16] {
          if (g.publish_each_frame) g.src.publish();
          if (g.xr->waits == 20) { fd->type = f16; fd->epoch = 2; }
          if (g.xr->waits == 45) fd->type = -1;   // lock lost: no samples; the F-16 stays (no evidence against it)
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      auto r = g.run();
      std::vector<uint64_t> gens; for (auto& jb : g.blit->jobs) if (jb.cockpit && (gens.empty() || gens.back() != jb.cockpit_gen)) gens.push_back(jb.cockpit_gen);
      check(r.success && fd->loads.size() == 2 && fd->loads[0] == j35 && fd->loads[1] == f16 && gens.size() == 2, "j35j then f16c, two generations");
      check(g.has_line("action=installed;type=f16c") && g.has_line("replaced=yes") && !g.has_line("action=unload"), "replaced, never unloaded on a lost lock");
      check(g.blit->jobs.back().cockpit, "the last confirmed model stays drawn without samples"); }
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      Rig g; g.stop_after = 50; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false); g.build();
      g.xr->hook["wait_frame"] = [&g, fd] {
          if (g.publish_each_frame) g.src.publish();
          if (g.xr->waits == 15) fd->centred = false;   // the player looks around: no windows, the model stays
          if (g.xr->waits == 25) { fd->centred = true; fd->type = A::find_type("zoef"); }
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      (void)g.run();
      check(g.has_line("action=installed;type=zoef") && g.has_line("off-centre="), "look-turned stretch ignored, then the next type");
    }
    // an offset that matches no player type for `confirm` windows (e.g. another camera mode): the model is dropped (the replayed cockpit)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = j35;
      Rig g; g.stop_after = 40; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.cockpit_auto = auto_settings(fd, false); g.build();
      g.xr->hook["wait_frame"] = [&g, fd] {
          if (g.publish_each_frame) g.src.publish();
          if (g.xr->waits == 12) { fd->odd = true; fd->epoch = 2; }   // W5-3 P2 #1: no-match only drops in a later lock epoch (re-lock / mission change)
          if (g.xr->waits + 1 >= g.stop_after) g.owner->request_stop();
      };
      (void)g.run();
      check(g.has_line("action=installed;type=j35j") && g.has_line("verdict=no-match") && g.has_line("action=unload;type=j35j;reason=no-matching-type"), "no-match unloads");
      check(!g.blit->jobs.back().cockpit && g.blit->jobs.front().cockpit == false, "replayed cockpit before the detection and after the unload"); }
    // without the in-projection composite the setting is dropped (inert)
    { auto fd = std::make_shared<AutoFeed>(); fd->type = f16;
      Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.cockpit_auto = auto_settings(fd, false); g.build(); (void)g.run();
      check(!g.has_line("cockpit-auto") && fd->loads.empty(), "auto without world_in_projection is inert"); }
    cockpit_auto_fast_runs();
}

// ---------------------------------------------------------------- DECOUPLE (xr_terrain_stage decouple:1; docs/world-render/decouple.md)
// Pure maths: the P1 -> P0 source mapping (identity when the poses agree, the direction-preserving mapping for a head yaw), the edge fade, the camera-delta
// rotation and the window-ray rotation.
static void decouple_math() {
    using namespace acz::xr_stereo;
    const float lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f}, anchor[3] = {0.0f, 1.2f, 0.0f};
    EyeSample e0{}; e0.pose.position[0] = -0.032f; e0.pose.position[1] = 1.2f; e0.fov = {-0.9f, 0.8f, 0.85f, -0.95f};
    auto yawed = [](EyeSample e, float deg, float dx) {
        const float a = deg * 3.14159265f / 180.0f;
        e.pose.orientation[0] = 0; e.pose.orientation[1] = std::sin(0.5f * a); e.pose.orientation[2] = 0; e.pose.orientation[3] = std::cos(0.5f * a);
        e.pose.position[0] += dx; return e;
    };
    WorldComposite w0{}, w1{};
    check(world_composite_params(e0, anchor, 0.0f, lens, 40.0f, w0, 0.0f), "decouple math: P0 params");
    double I[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) I[i][j] = i == j ? 1.0 : 0.0;
    check(rotation_is_identity(I), "identity recognised");
    Reproject r{};
    // (1) P1 == P0: the mapping is the identity (within float rounding)
    check(reproject_params(w0, I, I, true, false, 0.0f, r) && r.flags == kReprojSource, "identity params");
    float worst = 0;
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 9; ++x) {
        const float u = (x + 0.5f) / 9.0f, v = (y + 0.5f) / 9.0f;
        const ReprojectSample s = reproject_uv(r, w0, u, v);
        worst = std::max(worst, std::max(std::fabs(s.u - u), std::fabs(s.v - v)));
        check(s.in && s.fade == 1.0f, "identity: inside, no fade (fade width 0)");
    }
    check(worst < 2e-6f, "identity mapping exact within float rounding (" + std::to_string(worst) + ")");
    // (2) the head yawed 6 deg left and moved 5 cm: the P1 pixel maps to the P0 pixel of the SAME anchor-frame direction (rotation only)
    const EyeSample e1 = yawed(e0, 6.0f, 0.05f);
    check(world_composite_params(e1, anchor, 0.0f, lens, 40.0f, w1, 0.0f), "P1 params");
    check(reproject_params(w0, I, I, true, false, 0.0f, r), "yaw params");
    auto p0_uv = [&](const float d[3], float& u, float& v) {   // the P0 eye pixel of anchor-frame direction d (e0 has identity orientation)
        const float tl = std::tan(e0.fov.left), tr = std::tan(e0.fov.right), tu = std::tan(e0.fov.up), td = std::tan(e0.fov.down);
        const float s = d[0] / -d[2], t = d[1] / -d[2];
        u = (s - tl) / (tr - tl); v = (t - tu) / (td - tu);
    };
    worst = 0; unsigned outside = 0;
    for (int y = 0; y < 11; ++y) for (int x = 0; x < 11; ++x) {
        const float u = (x + 0.5f) / 11.0f, v = (y + 0.5f) / 11.0f;
        float d[3]; for (int i = 0; i < 3; ++i) d[i] = w1.base[i] + u * w1.ux[i] + v * w1.vy[i];
        float eu, ev; p0_uv(d, eu, ev);
        const ReprojectSample s = reproject_uv(r, w1, u, v);
        const bool in = eu >= 0 && eu <= 1 && ev >= 0 && ev <= 1;
        check(s.in == in, "yaw: inside/outside agrees with the hand projection");
        if (in) worst = std::max(worst, std::max(std::fabs(s.u - eu), std::fabs(s.v - ev))); else { ++outside; check(s.fade == 0.0f, "outside the source: fade 0 (nothing stretched)"); }
    }
    check(worst < 1e-5f && outside > 0, "yaw mapping = the hand projection (" + std::to_string(worst) + "), left edge uncovered (" + std::to_string(outside) + " samples)");
    // (3) edge fade: inside the fade band 0 < fade < 1, in the middle 1
    check(reproject_params(w0, I, I, true, false, 0.05f, r), "fade params");
    { float d[3]; const float u = 0.5f, v = 0.5f; for (int i = 0; i < 3; ++i) d[i] = w0.base[i] + u * w0.ux[i] + v * w0.vy[i];
      const ReprojectSample mid = reproject_uv(r, w0, 0.5f, 0.5f), edge = reproject_uv(r, w0, 0.02f, 0.5f);
      check(mid.fade == 1.0f && edge.fade > 0.0f && edge.fade < 1.0f, "fade: 1 in the middle, ramp inside the band (" + std::to_string(edge.fade) + ")"); (void)d; }
    // (4) camera delta: identical bases = the exact identity; a turned base: C_t d = C_o Q d for any d; the window rotation applies Q
    acz::terrain::CamFrame co{}, ct{};
    const float pos[3] = {0, 0, 0};
    acz::terrain::Conventions cv;
    check(acz::terrain::camera_frame(pos, 0.1f, -0.7f, 0.2f, cv, co) && acz::terrain::camera_frame(pos, 0.12f, -0.66f, 0.25f, cv, ct), "camera frames");
    double Q[3][3];
    camera_delta_rotation(co.right, co.up, co.fwd, co.right, co.up, co.fwd, Q);
    check(rotation_is_identity(Q), "same camera: the exact identity");
    camera_delta_rotation(co.right, co.up, co.fwd, ct.right, ct.up, ct.fwd, Q);
    check(!rotation_is_identity(Q), "turned camera: not the identity");
    double qworst = 0;
    const float dirs[3][3] = {{0.3f, -0.2f, -1.0f}, {1.0f, 0.5f, 0.2f}, {-0.4f, 0.9f, -0.3f}};
    for (const auto& d : dirs) {
        double qd[3]; for (int i = 0; i < 3; ++i) qd[i] = Q[i][0] * d[0] + Q[i][1] * d[1] + Q[i][2] * d[2];
        for (int i = 0; i < 3; ++i) {   // world: C d = right d.x + up d.y + fwd (-d.z)
            const double wt = ct.right[i] * d[0] + ct.up[i] * d[1] - ct.fwd[i] * d[2];
            const double wo = co.right[i] * qd[0] + co.up[i] * qd[1] - co.fwd[i] * qd[2];
            qworst = std::max(qworst, std::fabs(wt - wo));
        }
    }
    check(qworst < 1e-5, "camera delta: C_terrain d = C_objects Q d (" + std::to_string(qworst) + ")");
    check(reproject_params(w0, I, Q, false, true, 0.0f, r) && r.flags == kReprojWindow, "window-only params");
    for (const auto& d : dirs) {
        float o[3]; reproject_dir(r, d, o);
        for (int i = 0; i < 3; ++i) check(std::fabs(o[i] - float(Q[i][0] * d[0] + Q[i][1] * d[1] + Q[i][2] * d[2])) < 1e-5f, "window ray rotated by Q (quaternion)");
    }
    { Reproject off{}; const float d[3] = {0.1f, 0.2f, -1.0f}; float o[3]; reproject_dir(off, d, o); const ReprojectSample s = reproject_uv(off, w0, 0.3f, 0.7f);
      check(o[0] == d[0] && o[1] == d[1] && o[2] == d[2] && s.u == 0.3f && s.v == 0.7f && s.fade == 1.0f && s.in, "flags 0: exactly the pixel's own uv and ray"); }
    // (5) a singular P0 (degenerate rays) is refused (no partial output)
    WorldComposite bad = w0; for (int i = 0; i < 3; ++i) bad.vy[i] = 0.0f;
    check(!reproject_params(bad, I, I, true, false, 0.0f, r) && r.flags == 0, "singular P0 refused");
}
// Consumer runs: a 90 Hz XR runtime over a 60 Hz producer (2 new frames per 3 XR frames), a moving head and a moving, turning game camera.
static void decouple_runs() {
    using acz::terrain::CamFrame;
    const float e6pos[3] = {1000, 5000, -2000};
    auto make_rt = [&](bool on) {
        auto rt = terrain_rt(false);
        rt->camera = [=](acz::terrain::CamSample& c) { for (int i = 0; i < 3; ++i) c.campos[i] = e6pos[i]; c.pitch = 0.05f; c.yaw = -0.7f; c.roll = 0.0f; return true; };
        rt->conv.game_cam_max_deg = 179.0f;   // the game camera turns away from the fixed E6 sample: never refused here
        rt->pacing.decouple = on;
        return rt;
    };
    struct Run {
        Rig g; std::shared_ptr<xr_stereo::TerrainRuntime> rt;
        std::vector<std::array<xr_owner::Pose, 2>> head;   // the located views of each XR frame (by wait index)
        bool move_head = true, move_cam = true;
        unsigned publish_mod = 3, publish_until = UINT32_MAX;   // publish on waits n % 3 != 2 (60 Hz over 90 Hz)
        bool same_pose = false;                               // the producer renders at exactly the XR frame's views (P0 == P1)
        unsigned untracked_from = UINT32_MAX, untracked_to = 0;
        acz::terrain::CamEpochs* admit_ep = nullptr; acz::terrain::CamMailbox* admit_mb = nullptr;   // review 2 (I): CAM-EPOCH admission with these
        unsigned markers_every = 0;   // GAME-MARKERS: > 0 = the day config's marker layer (producer markers on every markers_every-th frame id)
    };
    auto setup = [&](Run& R, bool on, unsigned frames) {
        Rig& g = R.g; g.stop_after = frames; hud_full_rig(g); g.settings.world_in_projection = true; g.settings.hud_layer = true; g.src.with_hud = true;
        if (R.markers_every) { g.settings.hud_markers_layer = true; g.src.with_markers = true; g.src.markers_every = R.markers_every; }
        R.rt = make_rt(on); g.settings.terrain = R.rt;
        if (R.admit_ep) { R.rt->conv.cam_admit = true; R.rt->epochs = R.admit_ep; R.rt->present = R.admit_mb; }
        const auto cv = R.rt->conv;
        g.src.mutate = [&R, cv](stereo::StereoFrame& f) {
            const unsigned n = R.g.xr->waits;
            f.predicted_display_time = uint64_t(1000000000 + int64_t(n + 1) * R.g.xr->period);   // the frame is due at this XR frame's display time
            f.diag_publish_us = int64_t(f.frame_id) * 16667;                                     // 60 Hz game frames
            const float k = R.move_cam ? float(f.frame_id) : 0.0f;
            const float gpos[3] = {1000.0f + 10.0f * k, 5000.0f, -2000.0f - 12.0f * k};
            CamFrame gf{}; acz::terrain::camera_frame(gpos, 0.05f, -0.7f + 0.002f * k, 0.0f, cv, gf);
            for (int i = 0; i < 3; ++i) { f.game_cam_right[i] = gf.right[i]; f.game_cam_up[i] = gf.up[i]; f.game_cam_fwd[i] = gf.fwd[i]; f.game_eye[i] = gpos[i]; }
            f.game_cam_valid = true; f.game_eye_valid = true;
            if (R.same_pose) for (int e = 0; e < 2; ++e) {
                const auto& v = R.g.xr->views.eyes[e];
                for (int i = 0; i < 3; ++i) f.pose[e].position[i] = v.pose.position[i];
                for (int i = 0; i < 4; ++i) f.pose[e].orientation[i] = v.pose.orientation[i];
                f.pose[e].fov[0] = v.fov.left; f.pose[e].fov[1] = v.fov.right; f.pose[e].fov[2] = v.fov.up; f.pose[e].fov[3] = v.fov.down;
            }
        };
        g.build();
        g.xr->hook["wait_frame"] = [&R] {
            Rig& gg = R.g; const unsigned n = gg.xr->waits;
            if (R.move_head) for (int e = 0; e < 2; ++e) {   // 1 mm and 0.23 deg of yaw per XR frame
                auto& p = gg.xr->views.eyes[e].pose; const float a = 0.004f * float(n);
                p.position[0] = (e ? 0.032f : -0.032f) + 0.001f * float(n); p.orientation[1] = std::sin(0.5f * a); p.orientation[3] = std::cos(0.5f * a);
            }
            gg.xr->views.view_state_flags = (n >= R.untracked_from && n < R.untracked_to) ? valid_bits : (valid_bits | tracked_bits);
            if (n % R.publish_mod != R.publish_mod - 1 && n < R.publish_until) gg.src.publish();
            R.head.push_back({gg.xr->views.eyes[0].pose, gg.xr->views.eyes[1].pose});
            if (n + 1 >= gg.stop_after) gg.owner->request_stop();
        };
    };
    auto frame_of = [](const Rig& g, size_t i) { return unsigned((g.xr->layer_times[i] - 1000000000) / g.xr->period) - 1; };   // the layer's wait index
    // (A) decouple:1, 90 XR frames over a 60 Hz producer: EVERY XR frame composes with its own pose; the producer never starves; each slot released once
    {
        Run R; setup(R, true, 90);
        const Report rep = R.g.run();
        Rig& g = R.g; const auto& c = g.out->counters(); const auto& ds = g.out->decouple_stats();
        check(rep.success && !g.out->failed(), "decouple 90/60: run ok (" + g.out->failure() + ")");
        check(g.has_line("event=decouple-config;") && g.has_line("decouple=1;dc-age-ms=250") && g.has_line("event=summary-decouple;"), "decouple config + summary lines");
        size_t own_pose = 0;
        for (size_t i = 0; i < g.xr->layers.size(); ++i) {
            const unsigned n = frame_of(g, i);
            bool same = n < R.head.size();
            for (unsigned e = 0; e < 2 && same; ++e) same = same_pose(g.xr->layers[i].views[e].pose, R.head[n][e]);
            own_pose += same;
        }
        const size_t first = size_t(frame_of(g, 0));
        check(g.xr->layers.size() + first >= 88 && own_pose == g.xr->layers.size(), "every XR frame submits a layer with its OWN predicted pose (" + std::to_string(own_pose) + "/" +
              std::to_string(g.xr->layers.size()) + ")");
        check(ds.composites == g.xr->layers.size() && ds.fresh == c.new_frames && ds.recomposed > 20 && ds.fresh + ds.recomposed == ds.composites,
              "composites = XR frames (fresh " + std::to_string(ds.fresh) + " + re-composed " + std::to_string(ds.recomposed) + ")");
        check(c.reused == 0 && ds.reuse_fallback == 0, "no re-submitted (stale-pose) layer while the game runs");
        check(ds.reproj_source > 0 && ds.reproj_window > 0 && ds.cam_extrap > 10, "objects re-projected, window rays rotated, terrain camera dead-reckoned");
        check(g.src.skipped == 0 && g.src.bad_releases == 0, "the producer ring never starves (skipped " + std::to_string(g.src.skipped) + ") and every release is legal");
        check(g.j.prefix("source.acquire ") == g.j.prefix("source.release ") && ds.released == ds.retained && !g.out->decouple_holding(), "every acquired slot released (held ones once each)");
        for (uint64_t id = 1; id < g.src.next_id; ++id) {
            const unsigned a = g.j.count("source.acquire " + std::to_string(id)), r = g.j.count("source.release " + std::to_string(id));
            check(a == r && a <= 1, "frame " + std::to_string(id) + ": acquired " + std::to_string(a) + " released " + std::to_string(r));
        }
        // the re-composites: the eye targets only (no HUD acquire/draw), the HUD quad re-submitted from the first composite; the same source images as the fresh job
        unsigned rec = 0, fresh = 0, bad = 0;
        for (size_t i = 0; i < g.blit->jobs.size(); ++i) {
            const auto& jb = g.blit->jobs[i];
            if (jb.use_hud) { ++fresh; continue; }
            ++rec;
            if (!(i > 0 && jb.eyes[0].resource == g.blit->jobs[i - 1].eyes[0].resource && jb.terrain && jb.world_composite && (jb.reproject[0].flags & xr_stereo::kReprojSource))) ++bad;
        }
        check(fresh == ds.fresh && rec == ds.recomposed && bad == 0, "re-composite jobs: same held images, no HUD, re-projected (bad " + std::to_string(bad) + ")");
        check(g.j.count("xr.acquire 3") == ds.fresh && c.hud_layers == g.xr->layers.size(), "HUD image acquired per game frame, HUD quad on every XR layer");
        bool hud_all = true; for (auto& l : g.xr->layers) hud_all = hud_all && l.hud;
        check(hud_all, "HUD quad present on every layer");
    }
    // (B) the terrain camera does not stall between game frames: with a still head, each re-composite moves the terrain eye on by 2/3 of a game frame's step
    {
        Run R; R.move_head = false; setup(R, true, 90);
        (void)R.g.run();
        Rig& g = R.g;
        size_t checked = 0; double worst = 0;
        const auto& jobs = g.blit->jobs;
        for (size_t i = 40; i < jobs.size(); ++i) {
            if (jobs[i].use_hud || jobs[i - 1].use_hud == false || jobs[i - 2].use_hud == false) continue;   // a re-composite after two fresh frames (F F R)
            const float* e2 = jobs[i - 2].terrain_eye[0].eye; const float* e1 = jobs[i - 1].terrain_eye[0].eye; const float* e0 = jobs[i].terrain_eye[0].eye;
            for (int k = 0; k < 3; k += 2) worst = std::max(worst, std::fabs(double(e0[k] - e1[k]) - (2.0 / 3.0) * double(e1[k] - e2[k])));
            ++checked;
        }
        check(checked >= 8 && worst < 0.05, "re-composite terrain eye = the last game camera + 2/3 frame of motion (" + std::to_string(checked) + " cases, worst " + std::to_string(worst) + " u)");
        size_t moved = 0; for (size_t i = 41; i < jobs.size(); ++i) moved += std::fabs(jobs[i].terrain_eye[0].eye[0] - jobs[i - 1].terrain_eye[0].eye[0]) > 1.0f;
        check(moved + 1 >= jobs.size() - 41, "the terrain eye moves on EVERY XR frame (" + std::to_string(moved) + "/" + std::to_string(jobs.size() - 41) + ")");
    }
    // (C) P1 == P0 (the producer rendered at the XR frame's exact views), a still camera, a new frame every XR frame: decouple:1 is a no-op on the
    //     composite (no re-projection flags; the same jobs and layers as decouple:0)
    {
        Run on, off;
        for (Run* R : {&on, &off}) { R->move_head = false; R->move_cam = false; R->same_pose = true; R->publish_mod = 1000; setup(*R, R == &on, 30); (void)R->g.run(); }
        check(!on.g.out->failed() && !off.g.out->failed() && on.g.blit->jobs.size() == off.g.blit->jobs.size() && on.g.xr->layers.size() == off.g.xr->layers.size(),
              "P1 == P0: same number of jobs/layers");
        bool same = true, flags = false;
        for (size_t i = 0; i < on.g.blit->jobs.size() && i < off.g.blit->jobs.size(); ++i) {
            const auto& a = on.g.blit->jobs[i]; const auto& b = off.g.blit->jobs[i];
            same = same && std::memcmp(a.world_eye.data(), b.world_eye.data(), sizeof a.world_eye) == 0 && std::memcmp(a.terrain_eye.data(), b.terrain_eye.data(), sizeof a.terrain_eye) == 0 &&
                   a.use_hud == b.use_hud && a.use_depth == b.use_depth && a.terrain == b.terrain;   // (the ring slot differs: decouple holds one slot longer)
            flags = flags || a.reproject[0].flags || a.reproject[1].flags || b.reproject[0].flags || b.reproject[1].flags;
        }
        for (size_t i = 0; i < on.g.xr->layers.size() && i < off.g.xr->layers.size(); ++i) same = same && same_layer(on.g.xr->layers[i], off.g.xr->layers[i]);
        check(same && !flags, "P1 == P0: identical composites (world/terrain eyes, layers) and no re-projection flags: the exact previous shader path");
        check(on.g.out->decouple_stats().reproj_identity > 0 && on.g.out->decouple_stats().recomposed == 0, "P1 == P0: identity counted, nothing to re-compose");
    }
    // (D) a game stall: the held frame is re-composed while younger than dcage, then released exactly once (stall) and the old re-submit path takes over
    {
        Run R; R.publish_until = 6; setup(R, true, 60);
        (void)R.g.run();
        Rig& g = R.g; const auto& ds = g.out->decouple_stats(); const auto& c = g.out->counters();
        const uint64_t last = g.src.next_id - 1;
        unsigned uses = 0; void* last_res = nullptr;
        for (auto& sl : g.src.slots) if (sl.f.frame_id == last) last_res = sl.f.eye[0];
        for (auto& jb : g.blit->jobs) uses += jb.eyes[0].resource == last_res;
        check(ds.released_stall == 1 && g.j.count("source.release " + std::to_string(last)) == 1, "stall: the held frame released exactly once (stall " + std::to_string(ds.released_stall) + ")");
        check(ds.recomposed >= 15 && uses >= 16, "stall: the held frame re-composed over many XR frames (" + std::to_string(ds.recomposed) + ", uses " + std::to_string(uses) + ")");
        check(ds.reuse_fallback > 0 && c.reused > 0 && c.stale_age > 0, "after dcage: the old re-submit path, then nothing after reuse_max_age");
        check(!g.out->decouple_holding() && g.src.bad_releases == 0 && g.j.prefix("source.acquire ") == g.j.prefix("source.release "), "stall: nothing held at the end");
    }
    // (E) frames without tracking: no re-composite (the frame stays held), zero layers as before; tracking back: composites resume
    {
        Run R; R.untracked_from = 40; R.untracked_to = 46; setup(R, true, 70);
        (void)R.g.run();
        const auto& ds = R.g.out->decouple_stats();
        check(!R.g.out->failed() && (ds.untracked > 0 || R.g.out->counters().fresh_stale_tracking > 0) && ds.recomposed > 10 && R.g.out->counters().stale_tracking > 0,
              "untracked XR frames: no composite, then resumed");
    }
    // (F) decouple:1 without the in-projection composite (no terrain composite possible): inert
    {
        Rig g; g.stop_after = 20; hud_full_rig(g); g.settings.terrain = make_rt(true);   // world_in_projection off: the terrain (and decouple) is dropped
        g.build(); (void)g.run();
        bool flags = false; for (auto& jb : g.blit->jobs) flags = flags || jb.reproject[0].flags || jb.reproject[1].flags;
        check(!g.out->failed() && !g.has_line("decouple-config") && g.out->decouple_stats().composites == 0 && !flags && !g.out->decouple_holding() &&
              g.out->counters().new_frames > 0, "decouple without world_in_projection is inert");
    }
    // (F2) GAME-MARKERS (D-355/D-356, the day config: tac off, decouple:1, world in projection, HUD + marker layer): the game's own FRND/TGT boxes
    //      reach the marker quad on EVERY XR frame of a game frame that drew them (fresh composite AND its re-composites), and on NO XR frame of a
    //      game frame that drew none (a killed / off-screen target leaves at once: never an older marker image). One marker acquire per game frame
    //      with markers; the re-composites re-submit the fresh composite's marker quad unchanged (same world-plane pose/size).
    for (unsigned every : {1u, 3u}) {
        Run R; R.markers_every = every; setup(R, true, 90);
        const Report rep = R.g.run();
        Rig& g = R.g; const auto& c = g.out->counters(); const auto& ds = g.out->decouple_stats();
        const std::string what = " (markers every " + std::to_string(every) + ")";
        check(rep.success && !g.out->failed() && g.src.bad_releases == 0 && g.xr->violations == 0, "decouple + markers: run ok" + what);
        check(g.blit->jobs.size() == g.xr->layers.size() && ds.recomposed > 20, "decouple + markers: one composite job per XR layer, re-composites present" + what);
        unsigned fresh = 0, fresh_markers = 0, bad = 0, carried = 0; bool cur = false; size_t fresh_i = 0;
        for (size_t i = 0; i < g.blit->jobs.size() && i < g.xr->layers.size(); ++i) {
            const auto& jb = g.blit->jobs[i]; const auto& l = g.xr->layers[i];
            if (jb.use_hud) { ++fresh; cur = jb.use_markers; fresh_i = i; fresh_markers += cur; }   // a fresh composite (the game frame's own images)
            else if (jb.use_markers) ++bad;                                                          // a re-composite never re-blits the markers
            if (l.markers != cur) ++bad;                                                             // the layer carries exactly ITS game frame's markers
            if (l.markers) {
                ++carried;
                const auto& f0 = g.xr->layers[fresh_i];
                if (!(l.world_composite && !l.quad && same_pose(l.marker_pose, f0.marker_pose) && l.marker_width == f0.marker_width && l.marker_height == f0.marker_height)) ++bad;
                const auto list = xr_stereo::compose_layers(l);
                if (!(list.count == 3 && list.e[1].kind == xr_stereo::LayerKind::marker_quad && list.e[1].source_alpha)) ++bad;
            }
        }
        check(bad == 0, "decouple + markers: every layer carries its own game frame's markers, at the fresh composite's world-plane pose (bad " + std::to_string(bad) + ")" + what);
        check(fresh == ds.fresh && fresh_markers == c.marker_frames && g.j.prefix("xr.acquire 4") == c.marker_frames && c.marker_skipped == ds.fresh - fresh_markers &&
              c.marker_invalid == 0 && c.marker_lens_invalid == 0, "decouple + markers: one marker blit per game frame with boxes, frames without boxes skipped (absent)" + what);
        check(c.marker_layers == carried && carried > c.marker_frames, "decouple + markers: re-composites carry the markers too (layers " + std::to_string(carried) + ")" + what);
        if (every == 1) check(c.marker_frames == ds.fresh && carried == g.xr->layers.size(), "decouple + markers: boxes on every frame -> on every XR layer");
        else check(c.marker_frames > 0 && c.marker_skipped > 0 && carried < g.xr->layers.size(), "decouple + markers: frames without boxes submit no marker quad");
    }
    // ---- review 2 (Astra 6442582596). Common shape: a game stall after frame 6 so the last frame is HELD and re-composed; an event at XR wait kAt.
    static constexpr unsigned kAt = 12;
    auto held_slot = [](Rig& g) { for (unsigned i = 0; i < stereo::kSlots; ++i) if (g.src.slots[i].s == FakeSource::S::held) return int(i); return -1; };
    auto uses_after = [](Rig& g, size_t from, void* res) { unsigned n = 0; for (size_t i = from; i < g.blit->jobs.size(); ++i) n += g.blit->jobs[i].eyes[0].resource == res; return n; };
    auto legal = [](Rig& g) { return g.src.bad_releases == 0 && g.j.prefix("source.acquire ") == g.j.prefix("source.release ") && !g.out->decouple_holding(); };
    // (G) P2 #1: acquire + retain A, the producer re-executes A's owning recording (quarantine = a new write occurrence), the next XR frame must
    //     NOT compose from A again: the held metadata never authenticates the rewritten bytes. A is released once (rewritten), never by stall.
    {
        Run R; R.publish_until = 6; setup(R, true, 30);
        Rig& g = R.g; size_t jobs_at = 0; void* res = nullptr; uint64_t held_id = 0; unsigned uses_before = 0; int held_s = -1;
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&] {
            if (g.xr->waits == kAt) {
                const int s = held_slot(g);
                if (s >= 0) { held_s = s; res = g.src.slots[s].f.eye[0]; held_id = g.src.slots[s].f.frame_id; jobs_at = g.blit->jobs.size(); uses_before = uses_after(g, 0, res);
                              g.src.reexecute(unsigned(s)); }
            }
            inner();
        };
        (void)g.run();
        const auto& ds = g.out->decouple_stats();
        check(res && uses_before >= 2 && held_id, "re-exec: A was held and re-composed before the re-execution (uses " + std::to_string(uses_before) + ")");
        check(uses_after(g, jobs_at, res) == 0, "re-exec: no composite reads A's images after the re-execution (" + std::to_string(uses_after(g, jobs_at, res)) + ")");
        check(ds.released_rewritten == 1 && ds.released_stall == 0 && ds.rewritten_dropped == 0 && g.j.count("source.release " + std::to_string(held_id)) == 1,
              "re-exec: A released exactly once as rewritten (rewritten " + std::to_string(ds.released_rewritten) + ", stall " + std::to_string(ds.released_stall) + ")");
        check(!g.out->failed() && legal(g) && held_s >= 0 && g.src.slots[size_t(held_s)].s == FakeSource::S::quarantined,
              "re-exec: A's slot stays quarantined after the consumer's release; every release legal");
    }
    // (H) P2 #1, the check-then-write race: the re-execution is submitted AFTER the pre-check, while the re-composite's GPU work is in flight. The
    //     post-completion check must refuse that composite: its layer is not submitted, nothing re-submits it, the hold is released (rewritten).
    {
        Run R; R.publish_until = 6; setup(R, true, 30);
        Rig& g = R.g; bool fired = false, in_flight = false; size_t layers_at = 0; void* res = nullptr;
        g.blit->signal_hook = [&] {
            if (fired || g.xr->waits < kAt || g.blit->jobs.empty() || g.blit->jobs.back().use_hud) return;   // a re-composite of the held frame
            const int s = held_slot(g); if (s < 0) return;
            fired = true; res = g.src.slots[s].f.eye[0]; layers_at = g.xr->layers.size();
            in_flight = g.blit->executing && g.blit->jobs.back().eyes[0].resource == res;   // passed the pre-check, reads A on the GPU now
            g.src.reexecute(unsigned(s));
        };
        (void)g.run();
        const auto& ds = g.out->decouple_stats();
        check(fired && in_flight, "race: the re-execution hit an in-flight re-composite of A");
        check(ds.rewritten_dropped == 1 && ds.released_rewritten == 1, "race: the composite that may have read rewritten bytes is dropped (dropped " +
              std::to_string(ds.rewritten_dropped) + ")");
        check(g.xr->layers.size() == layers_at, "race: no layer submitted from that composite or re-submitted afterwards (" + std::to_string(g.xr->layers.size() - layers_at) + ")");
        check(!g.out->failed() && legal(g), "race: released once, legally, no failure");
    }
    // (I) P2 #3: retain, change the lock OR the stage epoch WITHOUT a LOCAL space change, then more XR frames before dcage: the held camera is not used
    //     for any further composite (released as camera, never by stall/space).
    for (int which = 0; which < 2; ++which) {
        acz::terrain::CamEpochs ep; acz::terrain::CamMailbox mb{&ep};
        Run R; R.publish_until = 6; R.admit_ep = &ep; R.admit_mb = &mb; setup(R, true, 30);   // CAM-EPOCH admission (game camera, no E6 sample)
        Rig& g = R.g;
        size_t jobs_at = 0; void* res = nullptr; unsigned uses_before = 0;
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&] {
            if (g.xr->waits == kAt) {
                const int s = held_slot(g);
                if (s >= 0) { res = g.src.slots[s].f.eye[0]; jobs_at = g.blit->jobs.size(); uses_before = uses_after(g, 0, res); }
                if (which == 0) ep.set_lock(ep.lock() + 1); else ep.bump_stage();
            }
            inner();
        };
        (void)g.run();
        const auto& ds = g.out->decouple_stats();
        const std::string w = which == 0 ? "lock" : "stage";
        check(res && uses_before >= 2 && ds.recomposed >= 2, "epoch(" + w + "): held and re-composed before the change");
        check(uses_after(g, jobs_at, res) == 0, "epoch(" + w + "): no re-composite with the invalidated camera (" + std::to_string(uses_after(g, jobs_at, res)) + ")");
        check(ds.released_camera == 1 && ds.released_stall == 0 && ds.released_space == 0 && !g.out->failed() && legal(g),
              "epoch(" + w + "): released once as camera, not stall/space");
    }
    // (J) P2 #5: an UNPOSED held frame (P0 = the located views of its first composite) and a LOCAL recenter during the stall: no re-composite combines
    //     the old-space P0 with a new-space P1; released as space.
    {
        Run R; R.publish_until = 6; setup(R, true, 30);
        Rig& g = R.g; g.src.pose_valid = false;
        size_t jobs_at = 0; void* res = nullptr; unsigned uses_before = 0;
        auto inner = g.xr->hook["wait_frame"];
        g.xr->hook["wait_frame"] = [&] {
            const unsigned w = g.xr->waits;
            if (w == kAt) {
                const int s = held_slot(g);
                if (s >= 0) { res = g.src.slots[s].f.eye[0]; uses_before = uses_after(g, 0, res); }
                Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local; e.change_time = 1000000000 + int64_t(w + 1) * g.xr->period;
                g.xr->events.push_back(e);
            }
            if (w == kAt + 3) jobs_at = g.blit->jobs.size();   // the new generation is in force by then
            inner();
        };
        (void)g.run();
        const auto& ds = g.out->decouple_stats();
        check(res && uses_before >= 2, "unposed: the held frame was re-composed before the recenter (uses " + std::to_string(uses_before) + ")");
        check(jobs_at && uses_after(g, jobs_at, res) == 0, "unposed: no re-composite of the old-space P0 after the recenter (" + std::to_string(uses_after(g, jobs_at, res)) + ")");
        check(ds.released_space == 1 && ds.released_stall == 0 && !g.out->failed() && legal(g), "unposed: released once as space (space " + std::to_string(ds.released_space) +
              ", stall " + std::to_string(ds.released_stall) + ")");
    }
    // ---- review 3 (Astra 6443240380, open risk 1): the FIRST composite of a frame is held to the same source-validity proof, decouple on and off.
    // (K) pre-check: the owning recording of frame 3 is re-executed right after its acquisition (before any consumer GPU work): frame 3 is released
    //     unused, no job ever reads its images, nothing fails.
    for (int on = 0; on < 2; ++on) {
        Run R; R.publish_until = 6; setup(R, on == 1, 30);
        Rig& g = R.g; size_t jobs_at = 0; void* res = nullptr; bool fired = false;
        g.src.on_acquire = [&](unsigned slot, uint64_t id) {
            if (fired || id != 3) return;
            fired = true; res = g.src.slots[slot].f.eye[0]; jobs_at = g.blit->jobs.size();
            g.src.reexecute(slot);
        };
        (void)g.run();
        const auto& c = g.out->counters();
        const std::string w = on ? "decouple:1" : "decouple:0";
        check(fired && res, "first-composite pre (" + w + "): frame 3 acquired and re-executed");
        check(c.fresh_rewritten_pre == 1 && c.fresh_rewritten_post == 0 && uses_after(g, jobs_at, res) == 0,
              "first-composite pre (" + w + "): released unused, never read (pre " + std::to_string(c.fresh_rewritten_pre) + ", uses " + std::to_string(uses_after(g, jobs_at, res)) + ")");
        check(g.j.count("source.release 3") == 1 && !g.out->failed() && g.src.bad_releases == 0 && g.j.prefix("source.acquire ") == g.j.prefix("source.release ") &&
              !g.out->decouple_holding(), "first-composite pre (" + w + "): released once, legally, no failure");
    }
    // (L) post-check, the check-then-write race: the re-execution is submitted while the FIRST composite of a game frame is on the GPU (after the
    //     pre-check). Its layer is never submitted, nothing re-submits the swapchain images it wrote, it is not retained, the slot is released once.
    //     A LATER game frame may still be published and composed (decouple:0 run: frame 4 after the dropped frame 3); its own layer and the pace()
    //     re-submits of THAT layer are legitimate, so "nothing from the dropped frame" = zero layers between the drop and the next first composite.
    for (int on = 0; on < 2; ++on) {
        Run R; R.publish_until = 6; setup(R, on == 1, 30);
        Rig& g = R.g; bool fired = false, in_flight = false, later = false; size_t layers_at = 0, layers_at_later = 0, jobs_at = 0; uint64_t id = 0; void* res = nullptr;
        g.blit->signal_hook = [&] {
            if (g.blit->jobs.empty() || !g.blit->jobs.back().use_hud) return;   // a FIRST composite (HUD drawn)
            if (fired) { if (!later) { later = true; layers_at_later = g.xr->layers.size(); } return; }   // the next first composite (a later frame)
            if (g.xr->waits < 4) return;
            const int s = held_slot(g); if (s < 0 || g.src.slots[s].f.frame_id != g.src.next_id - 1) return;   // of the latest published frame
            fired = true; id = g.src.slots[s].f.frame_id; layers_at = g.xr->layers.size(); jobs_at = g.blit->jobs.size(); res = g.src.slots[s].f.eye[0];
            in_flight = g.blit->executing && g.blit->jobs.back().eyes[0].resource == res;
            g.src.reexecute(unsigned(s));
        };
        (void)g.run();
        const auto& c = g.out->counters();
        const std::string w = on ? "decouple:1" : "decouple:0";
        check(fired && in_flight, "first-composite post (" + w + "): the re-execution hit the in-flight first composite");
        check(c.fresh_rewritten_post == 1 && c.fresh_rewritten_pre == 0, "first-composite post (" + w + "): dropped after completion (post " + std::to_string(c.fresh_rewritten_post) + ")");
        const size_t until = later ? layers_at_later : g.xr->layers.size();   // layers submitted before any later frame's first composite
        check(until == layers_at, "first-composite post (" + w + "): no layer submitted from it or re-submitted afterwards (" + std::to_string(until - layers_at) +
              (later ? ", a later frame was composed" : ", no later frame") + ")");
        check(uses_after(g, jobs_at, res) == 0, "first-composite post (" + w + "): no later composite reads the dropped frame's (quarantined) images");
        check(g.j.count("source.release " + std::to_string(id)) == 1 && !g.out->failed() && g.src.bad_releases == 0 &&
              g.j.prefix("source.acquire ") == g.j.prefix("source.release ") && !g.out->decouple_holding() && g.out->decouple_stats().released_rewritten == 0,
              "first-composite post (" + w + "): never retained, released once, legally, no failure");
    }
}

// R-5 measurement (docs/world-render/xr-start.md section 2): the per-attempt consumer object an early failed attempt (before consumer
// setup) used to keep forever: sizeof(Output) + its constructor's heap, from the private-commit delta of 2000 default-constructed Outputs.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
static void r5_output_retention_probe() {
    auto priv = [] { PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof pm;
        return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm) ? int64_t(pm.PrivateUsage) : int64_t(0); };
    std::vector<std::shared_ptr<xr_stereo::Output>> kept;
    kept.reserve(2000);
    const int64_t before = priv();
    for (int i = 0; i < 2000; ++i) kept.push_back(std::make_shared<xr_stereo::Output>(nullptr, nullptr, nullptr, nullptr));
    const int64_t per = (priv() - before) / 2000;
    std::cout << "R-5 retention probe: sizeof(Output)=" << sizeof(xr_stereo::Output) << " B; private commit per constructed Output ~" << per << " B\n";
    check(per >= 0 && per < (int64_t(4) << 20), "R-5 probe: Output retention measurable and bounded");
}
// ---------------------------------------------------------------- VSCREEN + Output (Astra review 6455891169 finding 1)
// The REAL Output and ScreenCore share the HUD swapchain (target 3) of the fake runtime. Menus first (no producer frame: the screen path
// acquires 3), then fresh flight frames. A wait timeout / release failure in the screen path must stop EVERY later use of target 3.
struct VsGpu final : xr_stereo::ScreenGpu {
    FakeXr* xr = nullptr; unsigned writes = 0, writes_unwaited = 0; uint64_t v = 0;
    bool setup(const xr_stereo::SwapchainImages&, const char*&) override { return true; }
    acz::vscreen::Completion completed() override { acz::vscreen::Completion c; c.value = v; return c; }   // every copy completes at once
    Submit copy(unsigned, void*, const acz::vscreen::CaptureImage&, xr_stereo::Extent) override {
        ++writes; if (!xr->chains[3].waited) ++writes_unwaited;
        Submit s; s.executed = s.signalled = true; s.value = ++v; s.readback = rb_want; rb_recorded += rb_want ? 1u : 0u; return s;
    }
    bool rb_want = false; unsigned rb_recorded = 0;   // SCREEN-DUMP: a 2x2 image
    void readback_want(bool w) override { rb_want = w; }
    bool readback_take(std::vector<uint8_t>& px, uint32_t& w, uint32_t& h) override { w = 2; h = 2; px.assign(16, 9); return true; }
    void destroy() override {}
};
struct VsShots final : xr_stereo::ScreenSource {
    uint64_t id = 0; bool held = false;
    void request() override { ++id; }   // a new capture per request (enough for the order checks)
    void set_extent(uint32_t, uint32_t) override {}
    bool acquire(uint64_t after, acz::vscreen::CaptureImage& o) override {
        if (held || id <= after) return false;
        o = acz::vscreen::CaptureImage{}; o.resource = reinterpret_cast<void*>(uintptr_t(0x7700)); o.width = 2560; o.height = 1440; o.held.slot = 0; o.held.id = id;
        held = true; return true;
    }
    void release(const acz::vscreen::CaptureImage&) override { held = false; }
    std::string describe() override { return "cap=fake"; }
};
void vscreen_output_sequence(bool release_failure) {
    Rig g; g.stop_after = 60; g.settings.hud_layer = true; g.src.with_hud = true; g.publish_from = 20;   // 20 menu frames, then flight
    acz::vscreen::Options o; o.on = true;
    auto gpu = std::make_shared<VsGpu>(); gpu->xr = g.xr.get();
    g.settings.vscreen = std::make_shared<xr_stereo::ScreenCore>(o, gpu, std::make_shared<VsShots>());
    if (release_failure) g.xr->script["xr.release 3"] = {Rc::error};
    else g.xr->wait_timeouts[3] = {true, true};
    g.build();
    auto r = g.run();
    const std::string what = release_failure ? "release failure" : "wait timeout";
    check(g.out->failed() && g.has_line("vscreen left the shared HUD swapchain acquisition unsettled"), what + ": Output fails for the session");
    check(g.j.count("xr.acquire 3") == 1, what + ": exactly one acquisition of the HUD swapchain (the screen's); the flight HUD never acquires it");
    const long a3 = g.j.index("xr.acquire 3");
    check(a3 >= 0 && g.j.index("source.acquire 1") < 0, what + ": no producer frame was ever composed after the poison");
    check(g.xr->chains[3].index == 0, what + ": the screen held image index 0");
    check(gpu->writes_unwaited == 0 && gpu->writes == (release_failure ? 1u : 0u), what + ": no write before a successful wait");
    check(g.xr->violations == 0, what + ": no swapchain rule violated (no layer referenced the unsettled swapchain)");
    check(g.j.count("xr.destroy 3") == 0 && g.has_line("retire;outcome=retained"), what + ": swapchains retained, never destroyed under the acquisition");
    check(!r.clean, what + ": not a clean retirement");
}
// SCREEN-DUMP: the trigger file is polled at most once per second while the screen is shown, deleted on the attempt; the screen part reaches
// png_sink after its readback completed. Without a trigger nothing is ever armed.
void vscreen_output_dump(bool with_file) {
    Rig g; g.stop_after = 300; g.settings.hud_layer = true; g.src.with_hud = true; g.publish_from = 100000;   // menus only, ~3.3 s
    acz::vscreen::Options o; o.on = true;
    auto gpu = std::make_shared<VsGpu>(); gpu->xr = g.xr.get();
    g.settings.vscreen = std::make_shared<xr_stereo::ScreenCore>(o, gpu, std::make_shared<VsShots>());
    auto file = std::make_shared<bool>(with_file); auto polls = std::make_shared<unsigned>(0), deletes = std::make_shared<unsigned>(0);
    g.settings.screen_dump_trigger = [file, polls, deletes](bool consume) { if (consume) { ++*deletes; const bool had = *file; *file = false; return had; } ++*polls; return *file; };
    auto names = std::make_shared<std::vector<std::string>>();
    g.settings.png_sink = [names](const std::string& n, std::vector<uint8_t>& px, uint32_t w, uint32_t h) { names->push_back(n); return px.size() == size_t(w) * h * 4; };
    g.build();
    auto r = g.run();
    const std::string what = with_file ? "screen dump" : "no trigger";
    check(r.success && !g.out->failed() && g.xr->violations == 0, what + ": clean run");
    check(*polls >= 2 && *polls <= 4, what + ": at most one trigger poll per second (" + std::to_string(*polls) + " in ~3.3 s)");
    if (with_file) {
        check(!*file && *deletes == 1 && gpu->rb_recorded == 1, what + ": one trigger = one attempt, the file deleted, one readback recorded");
        check(names->size() == 1 && names->at(0).find("acz-vr-xr-screen-t") == 0 && names->at(0).find("-screen.png") != std::string::npos, what + ": one PNG");
        check(g.has_line("event=png-screen-armed;outcome=armed;why=none;table=no-table") &&
              g.has_line("event=png-screen;part=screen;outcome=queued;why=none;size=2x2;"), what + ": armed + queued lines");
    } else {
        check(*deletes == 0 && gpu->rb_recorded == 0 && names->empty() && !g.has_line("png-screen"), what + ": nothing armed, nothing written");
    }
}
// SD-1: the trigger exists but cannot be consumed (delete refused): never armed, and repeated polls never re-arm; logged once; no PNG.
void vscreen_output_dump_unconsumable() {
    Rig g; g.stop_after = 400; g.settings.hud_layer = true; g.src.with_hud = true; g.publish_from = 100000;   // menus only, ~4.4 s
    acz::vscreen::Options o; o.on = true;
    auto gpu = std::make_shared<VsGpu>(); gpu->xr = g.xr.get();
    g.settings.vscreen = std::make_shared<xr_stereo::ScreenCore>(o, gpu, std::make_shared<VsShots>());
    auto polls = std::make_shared<unsigned>(0), consumes = std::make_shared<unsigned>(0);
    g.settings.screen_dump_trigger = [polls, consumes](bool consume) { if (consume) { ++*consumes; return false; } ++*polls; return true; };   // exists, refuses deletion
    auto names = std::make_shared<std::vector<std::string>>();
    g.settings.png_sink = [names](const std::string& n, std::vector<uint8_t>& px, uint32_t w, uint32_t h) { names->push_back(n); return px.size() == size_t(w) * h * 4; };
    g.build();
    auto r = g.run();
    check(r.success && !g.out->failed() && g.xr->violations == 0, "unconsumable trigger: clean run");
    check(*polls >= 3 && *consumes == *polls, "unconsumable trigger: polled repeatedly, consumption tried at every poll (" + std::to_string(*polls) + ")");
    check(gpu->rb_recorded == 0 && names->empty(), "unconsumable trigger: never armed, nothing written");
    check(!g.has_line("outcome=armed") && g.has_line("event=png-screen-armed;outcome=refused;why=trigger-not-consumed;"), "unconsumable trigger: the failure is logged");
}
void vscreen_output_happy() {
    // The screen in the menus, then flight: the Output's HUD acquisition invalidates the screen content (finding 4) and the screen is
    // never submitted while a flight frame is young.
    Rig g; g.stop_after = 60; g.settings.hud_layer = true; g.src.with_hud = true; g.publish_from = 20;
    acz::vscreen::Options o; o.on = true;
    auto gpu = std::make_shared<VsGpu>(); gpu->xr = g.xr.get();
    auto core = std::make_shared<xr_stereo::ScreenCore>(o, gpu, std::make_shared<VsShots>());
    g.settings.vscreen = core;
    g.build();
    auto r = g.run();
    check(r.success && !g.out->failed() && g.xr->violations == 0 && g.src.bad_releases == 0, "screen then flight: no violation");
    unsigned screen = 0, flight = 0; bool after_flight_screen = false; bool seen_flight = false;
    for (const auto& l : g.xr->layers) {
        if (l.screen_only) { ++screen; if (seen_flight) after_flight_screen = true; }
        else { ++flight; seen_flight = true; }
    }
    check(screen > 0 && flight > 0 && !after_flight_screen, "screen layers in the menus, flight layers after, never a screen while flying");
    check(core->stats().invalidated > 0 && !core->content_ok(), "the flight HUD acquisition invalidated the screen content");
    check(g.j.count("xr.destroy 3") == 1 && g.has_line("retire;outcome=clean"), "clean retire");
}

// EYE-RES (eye_res.h, docs/world-render/eye-resolution.md): eyeres:<pct> sizes the two EYE swapchains from the runtime's recommended view size; the
// quads keep the source-aspect policy; eyeres off keeps every swapchain and the swapchains line byte-for-byte.
std::string eye_res_line(const Rig& g, const std::string& p) {
    for (auto& l : g.lines) if (l.find(p) != std::string::npos) return l;
    return {};
}
void eye_res_runs() {
    std::string off_line;
    {   // off: exactly the previous policy and line (the reference run)
        Rig g; g.stop_after = 6; g.build();
        auto r = g.run();
        check(r.success && g.xr->violations == 0, "eyeres off: success, source-aspect sizes");
        off_line = eye_res_line(g, "xr-stereo;event=swapchains;");
        check(off_line.find(";policy=source-aspect-width-min(src,2560,max);recommended=2688x2688-unused;") != std::string::npos, "eyeres off: old policy tokens");
        check(!g.has_line("event=eye-res"), "eyeres off: no eye-res line");
    }
    {   // 100 %: 2688x2688 eyes, the world quad still 2560x1440
        Rig g; g.stop_after = 6; g.settings.eye_res_pct = 100; g.xr->expect_eye_w = 2688; g.xr->expect_eye_h = 2688; g.build();
        auto r = g.run();
        check(r.success && r.clean && !g.out->failed() && g.xr->violations == 0, "eyeres 100: success, eyes at the recommended size, quad at the source aspect");
        check(g.out->extent(0).w == 2688 && g.out->extent(0).h == 2688 && g.out->extent(1).w == 2688 && g.out->extent(1).h == 2688, "eyeres 100: eye extents");
        check(g.out->extent(xr_stereo::kQuad).w == 2560 && g.out->extent(xr_stereo::kQuad).h == 1440, "eyeres 100: quad unchanged");
        const std::string sl = eye_res_line(g, "xr-stereo;event=swapchains;");
        check(sl.find("eye0=2688x2688,images=3;eye1=2688x2688,images=3;quad=2560x1440,images=3;") != std::string::npos, "eyeres 100: swapchains line sizes");
        check(sl.find(";policy=recommended-x100%(eyes),source-aspect(quads);recommended=2688x2688-used;") != std::string::npos, "eyeres 100: policy tokens");
        check(sl.size() < 480 && sl.size() <= off_line.size() + 16, "eyeres 100: swapchains line under the router cap");
        const std::string el = eye_res_line(g, "xr-stereo;event=eye-res;");
        check(el == "xr-stereo;event=eye-res;pct=100;used=1;eye0=2688x2688;eye1=2688x2688;clamp=none,none;old-policy=2560x1440,2560x1440;"
                    "mpix-per-eye=7.23(old-3.69);budget-mpix=16.78;est-mib=swapchains:165(old-84),props:275(old-140),model-depth:27(old-14);"
                    "quads=source-aspect(unchanged);needs=layer-fov-hmd;world-filter=footprint-grid(max-2x2,bit21)", "eyeres 100: exact eye-res line");
        check(el.size() < 480, "eye-res line under the router cap");
        check(g.has_line("xr-stereo;event=mapping;source=2560x1440;fmt=10;eye0=0,0,2688x2688;eye1=0,0,2688x2688;quad=0,0,2560x1440;scale=1.050000;aspect-match=0;"),
              "eyeres 100: full-rect mapping into the square eyes (same FOV, more pixels)");
        check(!g.blit->maps.empty() && g.blit->maps[0][0].dst.w == 2688 && g.blit->maps[0][0].dst.h == 2688 && g.blit->maps[0][1].dst.w == 2688 &&
              g.blit->maps[0][xr_stereo::kQuad].dst.w == 2560 && g.blit->maps[0][xr_stereo::kQuad].dst.h == 1440, "eyeres 100: record maps");
        check(g.j.count("xr.create 0") == 1 && g.j.count("xr.create 1") == 1 && g.j.count("xr.create 2") == 1, "eyeres 100: same swapchain set");
    }
    {   // 200 % over the runtime max 4096: aspect kept at 4096x4096 (also exactly the pixel budget)
        Rig g; g.stop_after = 4; g.settings.eye_res_pct = 200; g.xr->expect_eye_w = 4096; g.xr->expect_eye_h = 4096; g.build();
        auto r = g.run();
        check(r.success && g.xr->violations == 0 && g.out->extent(0).w == 4096 && g.out->extent(1).h == 4096, "eyeres 200: clamped to the runtime max");
        check(eye_res_line(g, "xr-stereo;event=eye-res;").find(";clamp=max,max;") != std::string::npos, "eyeres 200: clamp reported");
    }
    {   // per-eye recommended sizes are followed per eye
        Rig g; g.stop_after = 4; g.settings.eye_res_pct = 100; g.xr->cfg[0] = g.xr->cfg[1] = {1512, 1680, 4096, 4096, 1};
        g.xr->expect_eye_w = 1512; g.xr->expect_eye_h = 1680; g.build();
        auto r = g.run();
        check(r.success && g.xr->violations == 0 && g.out->extent(0).w == 1512 && g.out->extent(0).h == 1680, "eyeres: a non-square recommended view");
    }
    {   // an out-of-range pct reaching the Output (the proxy refuses it first) is refused per eye: the old policy, used=0
        Rig g; g.stop_after = 4; g.settings.eye_res_pct = 300; g.build();
        auto r = g.run();
        check(r.success && g.xr->violations == 0 && g.out->extent(0).w == 2560 && g.out->extent(0).h == 1440, "eyeres 300: refused, old sizes");
        const std::string el = eye_res_line(g, "xr-stereo;event=eye-res;");
        check(el.find("pct=300;used=0;eye0=2560x1440;") != std::string::npos && el.find("clamp=refused(old-policy),refused(old-policy);") != std::string::npos,
              "eyeres 300: refusal logged");
        check(eye_res_line(g, "xr-stereo;event=swapchains;") == off_line, "eyeres refused: the swapchains line is the old one byte-for-byte");
    }
    // WORLD-WINDOW FILTER (eye_res.h filter_select, mode_bits bit 21): requested on every world-composite recording while eyeres is on, never otherwise.
    {
        Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.src.with_neutral = true; g.settings.eye_res_pct = 100;
        g.xr->expect_eye_w = 2688; g.xr->expect_eye_h = 2688; g.build();
        auto r = g.run();
        check(r.success && !g.out->failed(), "world filter: run");
        unsigned n = 0, nc = 0;
        for (auto& j : g.blit->jobs) { check(j.world_filter == j.world_composite, "world filter: flag exactly on the composite recordings"); n += j.world_filter; nc += j.world_composite; }
        check(n > 0 && n == nc, "world filter: composite recordings carry the request");
        check(g.has_line("xr-stereo;event=summary-world-filter;"), "world filter: summary line while eyeres is on");
    }
    {
        Rig g; g.stop_after = 12; hud_full_rig(g); g.settings.world_in_projection = true; g.src.with_neutral = true; g.build();
        auto r = g.run();
        check(r.success && !g.out->failed(), "world filter off: run");
        unsigned nc = 0;
        for (auto& j : g.blit->jobs) { check(!j.world_filter, "world filter off: never requested without eyeres"); nc += j.world_composite; }
        check(nc > 0 && !g.has_line("event=summary-world-filter"), "world filter off: composite recordings, no filter line");
    }
    // EYE-RING (eye_ring.h, eye-resolution.md 5.1 (a)): Output::setup publishes the eye extent for the producer only with eyeres on (used); frames with
    // eye_width/eye_height give the per-eye sources (eyes, depth) the eye size, the game-screen sources (world, HUD) keep the frame size.
    {
        acz::eyering::reset_for_tests();
        Rig g; g.stop_after = 4; g.build(); auto r = g.run();
        check(r.success && !acz::eyering::read().extent_known, "eye ring: eyeres off publishes no extent");
    }
    {
        acz::eyering::reset_for_tests();
        Rig g; g.stop_after = 4; g.settings.eye_res_pct = 300; g.build(); auto r = g.run();   // refused size: the old policy, nothing for the producer
        check(r.success && !acz::eyering::read().extent_known, "eye ring: a refused eyeres publishes no extent");
    }
    {
        acz::eyering::reset_for_tests();
        Rig g; g.stop_after = 8; g.settings.eye_res_pct = 100; g.xr->expect_eye_w = 2688; g.xr->expect_eye_h = 2688;
        g.src.with_depth = true; g.src.with_world = true; g.src.lens_valid = true; hud_full_rig(g); g.settings.world_in_projection = true;
        g.src.mutate = [](stereo::StereoFrame& f) { f.eye_width = 2688; f.eye_height = 2688; };
        g.build(); auto r = g.run();
        const auto c = acz::eyering::read();
        check(r.success && !g.out->failed() && c.extent_known && c.w == 2688 && c.h == 2688, "eye ring: eyeres 100 publishes the eye extent 2688x2688");
        unsigned n = 0;
        for (auto& jb : g.blit->jobs) {
            check(jb.eyes[0].width == 2688 && jb.eyes[0].height == 2688 && jb.eyes[1].width == 2688 && jb.eyes[1].height == 2688, "eye ring: eye sources at the eye size");
            if (jb.use_depth) check(jb.depth[0].width == 2688 && jb.depth[1].height == 2688, "eye ring: depth at the eye size");
            if (jb.use_world && jb.world.resource) check(jb.world.width == 2560 && jb.world.height == 1440, "eye ring: the world copy stays at the game size");
            if (jb.use_hud) check(jb.hud.width == 2560 && jb.hud.height == 1440, "eye ring: the HUD stays at the game size");
            ++n;
        }
        check(n > 0, "eye ring: recordings");
        check(g.has_line("xr-stereo;event=eye-ring-consumer;source=2560x1440;per-eye=2688x2688;eye0=2688x2688;sampled=uv-full-rect;"), "eye ring: consumer line");
        unsigned lines = 0; for (auto& l : g.lines) lines += l.find("event=eye-ring-consumer") != std::string::npos;
        check(lines == 1, "eye ring: consumer line once");
        acz::eyering::reset_for_tests();
    }
    {   // frames without eye_width (every producer without the eye ring): the eye sources keep the frame size, no consumer line
        Rig g; g.stop_after = 6; g.settings.eye_res_pct = 100; g.xr->expect_eye_w = 2688; g.xr->expect_eye_h = 2688; g.src.with_depth = true; g.build();
        auto r = g.run();
        check(r.success && !g.has_line("eye-ring-consumer"), "eye ring off: no consumer line");
        for (auto& jb : g.blit->jobs) check(jb.eyes[0].width == 2560 && jb.eyes[0].height == 1440, "eye ring off: eye sources at the frame size");
        acz::eyering::reset_for_tests();
    }
    check(xr_stereo::kModeBitWorldFilter == (1u << 21) && (xr_stereo::kModeBitWorldFilter & (xr_stereo::kModeBitSprite | xr_stereo::kModeBitSmoke |
          xr_stereo::kModeBitModelCockpit | xr_stereo::kModeBitOcclude | 0xFF00FFFFu | (1u << 20))) == 0u, "world filter: bit 21 collides with no eye bit");
}

int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--vscreen") {
        vscreen_output_sequence(false); vscreen_output_sequence(true); vscreen_output_happy(); vscreen_output_dump(true); vscreen_output_dump(false); vscreen_output_dump_unconsumable();
        std::cout << "xr_stereo_output_offline_tests --vscreen: " << checks << " checks passed\n"; return 0;
    }
    if(argc==2&&std::string(argv[1])=="--decouple") { decouple_math(); decouple_runs(); std::cout << "xr_stereo_output_offline_tests --decouple: " << checks << " checks passed\n"; return 0; }
    if(argc==2&&std::string(argv[1])=="--cockpit-auto-junk") { cockpit_auto_junk_lock_runs(); std::cout << "xr_stereo_output_offline_tests --cockpit-auto-junk: " << checks << " checks passed\n"; return 0; }
    if(argc==2&&std::string(argv[1])=="--cockpit-auto") { cockpit_auto_consumer_runs(); std::cout << "xr_stereo_output_offline_tests --cockpit-auto: " << checks << " checks passed\n"; return 0; }
    if(argc==2&&std::string(argv[1])=="--cockpit") { cockpit_consumer_runs(); std::cout << "xr_stereo_output_offline_tests --cockpit: " << checks << " checks passed\n"; return 0; }    if(argc==2&&std::string(argv[1])=="--terrain") { terrain_consumer_runs(); std::cout << "xr_stereo_output_offline_tests --terrain: " << checks << " checks passed\n"; return 0; }
    if(argc==2&&std::string(argv[1])=="--world-hold") { // J-world-hold / J-world-extend cases only (own ctest entry)
        world_hold_fade_math(); world_extend_math(); world_hold_producer_helpers(); world_hold_config_keys(); world_hold_runs(); world_layer_fov_line();
        std::cout << "xr_stereo_output_offline_tests --world-hold: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--hud-markers") { // J-hud-markers cases only (own ctest entry)
        markers_compose_layers(); markers_config_keys(); markers_four_layers_world_quad(); markers_pose_matches_world_quad(); markers_absent_and_refusals();
        markers_key_off_unchanged(); markers_requires_hud_world_quad(); markers_failures(); markers_world_in_projection();
        std::cout << "xr_stereo_output_offline_tests --hud-markers: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--effects") { // J-effects-layer cases only (own ctest entry)
        effects_math(); effects_config_keys(); effects_runs(); effects_dual(); test8_diag();
        std::cout << "xr_stereo_output_offline_tests --effects: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--quad-yaw") { // J-quad-yaw cases only (own ctest entry)
        quad_yaw_math(); quad_yaw_runs(); look_quad_runs(); quad_yaw_invalid_unchanged(); quad_yaw_generation_and_upgrade();
        std::cout << "xr_stereo_output_offline_tests --quad-yaw: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--world-projection") { // J-world-in-projection cases only (own ctest entry)
        world_projection_layout_and_compose(); world_projection_math(); world_look_math(); world_projection_config_keys(); world_projection_runs();
        std::cout << "xr_stereo_output_offline_tests --world-projection: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==3&&std::string(argv[1])=="--evidence-sink"){evidence_sink_case(argv[2]);return 0;}
    if(argc==2&&std::string(argv[1])=="--hud-placement") { // J-hud-placement cases only (own ctest entry)
        hud_placement_math(); hud_placement_config_keys(); hud_placement_defaults_unchanged(); hud_placement_world_quad_runs();
        hud_placement_head_locked(); hud_placement_safety();
        std::cout << "xr_stereo_output_offline_tests --hud-placement: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--hud") { // J-hud-consumer cases only (own ctest entry)
        hud_shading_math(); png_shade_mirror(); hud_compose_layers(); hud_config_keys(); hud_three_layers_world_quad(); hud_absent_keeps_existing_path();
        hud_key_off_unchanged(); hud_refusals(); hud_head_locked(); hud_never_stale(); hud_failures_retain();
        std::cout << "xr_stereo_output_offline_tests --hud: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--feather") { // J-feather cases only (own ctest entry)
        feather_ramp_math(); feather_config_keys(); feather_layers(); feather_quad_distance();
        std::cout << "xr_stereo_output_offline_tests --feather: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--pov-fixes") { // POV fixes (F-LEAN anchors, F-HUD) consumer cases only (own ctest entry)
        pov_fixes_consumer();
        std::cout << "xr_stereo_output_offline_tests --pov-fixes: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--timing-anchor") { // timing / anchor / correlated-record fixtures only (own ctest entry)
        timing_cockpit_keeps_render_pose(); timing_reused_layers_keep_metadata(); timing_held_world_keeps_its_look(); anchor_contract(); timing_select_corr_record();
        std::cout << "xr_stereo_output_offline_tests --timing-anchor: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--eye-res") { // EYE-RES cases only (own ctest entry)
        eye_res_runs();
        std::cout << "xr_stereo_output_offline_tests --eye-res: " << checks << " checks passed\n";
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--cockpit-fov") { // J-cockpit-fov-consumer cases only (own ctest entry)
        cockpit_fov_math(); cockpit_fov_flag_off_unchanged(); cockpit_fov_flag_on(); cockpit_fov_invalid();
        std::cout << "xr_stereo_output_offline_tests --cockpit-fov: " << checks << " checks passed\n";
        return 0;
    }
    throughput_diagnostics();
    source_sizing_throughput();
    mapping_math();
    format_policy_same_as_probe();
    config_keys();
    registry();
    disabled_owner_makes_no_calls();
    success_sequence();
    cpu_presynced_skips_queue_wait();
    release_only_after_completion();
    should_render_false();
    producer_stops_publishing();
    pacing_gaps();
    fence_authentication();
    reuse_validity();
    labels();
    fence_registration();
    fresh_space_and_tracking();
    producer_neutral_space_coupling();
    world_lens_invalid();
    lens_depth_world();
    depth_fallbacks();
    geometry_and_shading_math();
    located_pose_fallback();
    for (const char* op : {"xr.acquire 0", "xr.acquire 1", "xr.wait 0 0", "xr.wait 1 0", "xr.release 0", "xr.release 1", "end_frame_projection", "locate_views"}) {
        session_loss_at(op, false);
        session_loss_at(op, true);
    }
    acquire_timeout_outstanding(true);
    acquire_timeout_retained();
    acquire_timeout_recovered_at_retire();
    restoration_not_proven();
    completion_timeout_quarantine(false);
    completion_timeout_quarantine(true);
    first_frame_backlog_completes();
    steady_backlog_inside_window();
    first_window_capped_by_startup_reserve();
    first_frame_setup_budget(3900, 5000, false);
    first_frame_setup_budget(4500, 5000, false);
    first_frame_setup_budget(3500, 5000, true);
    first_frame_setup_budget(2000, 5000, true);
    first_frame_setup_budget(1500, 3000, true);
    first_frame_setup_budget(1900, 3000, false);
    first_frame_budget_consumed_before_submission();
    first_frame_budget_consumed_after_submission(3950);
    first_frame_budget_consumed_after_submission(4100);
    first_window_still_bounded();
    execute_ambiguous();
    execute_refused_cleanly();
    invalid_frame();
    setup_refusals();
    end_frame_failure_not_retried();
    slot_contention();
    watchdog_retracts_pose();
    continuous_has_no_caps();
    first_frame_is_startup_budget();
    second_frame_keeps_progress_rule();
    first_frame_over_startup();
    retry_policy();
    call_bracket_covers_every_call();
    evidence_route_policy();
    evidence_router_lines();
    evidence_consumer_run();
    hud_shading_math();
    png_shade_mirror();
    hud_compose_layers();
    hud_config_keys();
    hud_three_layers_world_quad();
    hud_absent_keeps_existing_path();
    hud_key_off_unchanged();
    hud_refusals();
    hud_head_locked();
    hud_never_stale();
    hud_failures_retain();
    feather_ramp_math();
    feather_config_keys();
    feather_layers();
    feather_quad_distance();
    cockpit_fov_math();
    cockpit_fov_flag_off_unchanged();
    cockpit_fov_flag_on();
    cockpit_fov_invalid();
    hud_placement_math();
    hud_placement_config_keys();
    hud_placement_defaults_unchanged();
    hud_placement_world_quad_runs();
    hud_placement_head_locked();
    hud_placement_safety();
    markers_compose_layers();
    markers_config_keys();
    markers_four_layers_world_quad();
    markers_pose_matches_world_quad();
    markers_absent_and_refusals();
    markers_key_off_unchanged();
    markers_requires_hud_world_quad();
    markers_failures();
    markers_world_in_projection();
    effects_math(); effects_config_keys(); effects_runs(); effects_dual(); test8_diag();
    world_projection_layout_and_compose();
    world_projection_math();
    world_look_math();
    world_projection_config_keys();
    world_projection_runs();
    world_hold_fade_math();
    world_extend_math();
    world_hold_producer_helpers();
    world_hold_config_keys();
    world_hold_runs();
    world_layer_fov_line();
    timing_cockpit_keeps_render_pose();
    timing_reused_layers_keep_metadata();
    timing_held_world_keeps_its_look();
    anchor_contract();
    timing_select_corr_record();
    pov_fixes_consumer();
    terrain_consumer_runs();
    cockpit_consumer_runs();
    cockpit_auto_consumer_runs();
    decouple_math(); decouple_runs();   // DECOUPLE
    r5_output_retention_probe();
    std::cout << "xr_stereo_output_offline_tests: " << checks << " checks passed\n";
    return 0;
}
