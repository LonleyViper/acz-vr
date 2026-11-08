#pragma once
// BRIEF3D consumer core (docs/world-render/brief3d.md section 6). Header-only and SDK-free so the offline tests drive the real decision code with a
// fake GPU, fake producer source, fake swapchain runtime and fake Gate (tests/acz-vr/brief3d_table_offline_tests.cpp). D3D12: xr_brief3d_d3d12.cpp.
//
// What it does: while the virtual screen is shown (the vscreen hook planned its quad), the newest READY briefing-table slot of the producer
// (brief3d_live.h) whose fence value completed is blitted into the two EYE swapchains (targets 0/1: unused outside flight) and submitted as ONE
// projection layer (premultiplied SOURCE_ALPHA, LOCAL) ON TOP of the screen quad, with the eye poses/FOV the slot was rendered with.
// Ownership rules (the ScreenCore rules of xr_vscreen_core.h, Astra review 6455891169 / follow-up 6458181039):
//  * both eye swapchain acquisitions are settled inside one frame() call (wait succeeded -> blit -> release); a wait that never succeeds or a
//    failed release POISONS the hook (sticky): the decorator reports poisoned(), Output fails the session, retire() retains;
//  * the producer slot is HELD from acquisition until the blit's private fence value completed (never on loss); an Execute without a Signal keeps it
//    held for ever (quarantined) and poisons;
//  * the table layer is submitted only while BOTH eye swapchains' last released images are known to be this core's blit of the slot whose poses
//    are submitted (content_ok): cleared by a new acquisition, by Output writing an eye target (target_written: flight), by poison;
//  * the GPU work runs only inside g.call(Op::d3d_execute) (deadline admission); a refused call never invokes the callback.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "brief3d_live.h"
#include "xr_screen_dump.h"
#include "xr_vscreen.h"

namespace acz::xr_stereo {

// The producer's slot images (DLL: brief3d_gpu_win.cpp), R16G16B16A16_FLOAT, resting in COMMON.
// opaque = a UI3D slot (SlotMeta::opaque): the blit writes rgb with alpha 1 (no un-premultiply) and the layer is submitted opaque.
// pause = a UI3D stage-3a pause slot (SlotMeta::pause): the blit BLENDS it over the destination (ONE / INV_SRC_ALPHA, xr_brief3d.hlsl mode 2) with
// the alpha floor `floor` (the hud-menu menub). Only PauseCore sets it.
struct TableImages { void* eye[2] = {nullptr, nullptr}; uint32_t width = 0, height = 0, format = 0; bool opaque = false; bool pause = false; float floor = 0.0f; };
struct TableSource {
    virtual ~TableSource() = default;
    // The newest READY slot newer than `after` whose producer fence value completed (CPU-observed) -> HELD.
    virtual bool acquire(uint64_t after, acz::brief3d::live::Held& out, TableImages& img) = 0;
    virtual void release(const acz::brief3d::live::Held&) = 0;
    virtual bool dead() = 0;                 // the producer is poisoned / lost: nothing more will come
    virtual std::string describe() = 0;      // producer tail of the table line [+ '\n' + the producer's own line]
    // The clock SlotMeta::published_ms is on (the producer's Present clock: GetTickCount64 in the DLL), given the Gate's owner clock `gate_now`.
    // The owner clock has another epoch, so the age of a publication is only comparable on this one. Default (fakes): the Gate's.
    virtual int64_t clock_ms(int64_t gate_now) { return gate_now; }
};
// The command allocators (and, for the latch, upload spans) a TableGpu provides: TableCore / PauseCore alternate 0 / 1, LatchCore cycles all
// (review 2026-10-31 F3: with 2 the latch found its allocator busy whenever the queue ran more than ~2 XR frames behind at 90 Hz).
inline constexpr unsigned kTableAllocs = 3;
// The blit (DLL: D3D12 on the XR binding queue). Owner thread only.
struct TableGpu {
    virtual ~TableGpu() = default;
    virtual bool setup(const SwapchainImages& eye0, const SwapchainImages& eye1, const char*& why) = 0;   // once; false = never usable
    virtual acz::vscreen::Completion completed() = 0;
    struct Submit { bool executed = false, signalled = false; uint64_t value = 0; bool readback = false; };
    // Records and executes the two eye blits (slot image e -> swapchain image dst[e], un-premultiply, sRGB decode, premultiply) with allocator
    // `alloc` (0/1) and signals the private fence. SCREEN-DUMP: while readback_want(true), the list also copies dst0 (after the blit) into a
    // private readback buffer (Submit::readback = recorded into the executed list).
    virtual Submit blit(unsigned alloc, void* dst0, void* dst1, const TableImages& src, Extent e0, Extent e1) = 0;
    // brief3d.md 11 (b3l:1): draws the latched scene for both eyes with the plan's records (the game's PSO / root signature, the scene's geometry
    // copied into this allocator's upload span) into private eye images, then blits them into dst0 / dst1 like blit(). Default: not supported.
    // `alloc` < kTableAllocs.
    virtual Submit render(unsigned alloc, void* dst0, void* dst1, const acz::brief3d::latch::FramePlan& f, Extent e0, Extent e1) {
        (void)alloc; (void)dst0; (void)dst1; (void)f; (void)e0; (void)e1; return Submit{};
    }
    virtual void readback_want(bool) {}
    virtual bool readback_take(std::vector<uint8_t>&, uint32_t&, uint32_t&) { return false; }   // completed readback, RGBA8 premultiplied
    // After a given-up readback's fence value was PROVEN complete (never on timeout/loss alone): drop it so a fresh dump can be recorded.
    virtual void readback_discard() {}
    virtual void destroy() = 0;   // every private object setup() created, also after a PARTIAL setup; only after every submission completed
};

// W14-1 F1 (2026-10-30): a slot's age counts from its PUBLICATION (SlotMeta::published_ms, on the producer's clock `clk` = TableSource::clock_ms),
// not from its acquisition: set, not in the future by more than the slack, at most `stale_ms` old. The PauseCore rule (W13-1 F2), shared.
inline bool slot_published_fresh(int64_t published, int64_t clk, int64_t stale_ms, int64_t future_slack_ms = 50) noexcept {
    return published > 0 && published <= clk + future_slack_ms && clk - published <= stale_ms;
}
class TableCore {
public:
    static constexpr int64_t kStaleMs = 500;   // no new producer slot for this long (the map screen was left): the table layer is dropped
    TableCore(std::shared_ptr<TableGpu> gpu, std::shared_ptr<TableSource> src) : gpu_(std::move(gpu)), src_(std::move(src)) {}

    // Owner thread, an XR frame in which the screen layer is planned. `space_generation` = FrameInfo::space_generation (raw; the slots carry
    // the ENCODED value, encode_space_generation). true = plan.layer.table set with the slot's views.
    bool frame(Gate& g, SwapchainRuntime& xr, const SwapchainImages* eye0, const SwapchainImages* eye1, Extent e0, Extent e1,
               uint32_t space_generation, xr_owner::LayerPlan& plan) {
        now_ = g.now_ms(); touched_ = false;
        poll_release();
        if (poisoned_ || dead_) return false;
        if (src_->dead()) { dead_ = true; why_ = "producer-dead"; return false; }
        const uint32_t gen = encode_space_generation(space_generation);
        // Astra 6460640585 P2-4: content blitted in another LOCAL generation is never submitted again (the held source, if any, is still
        // released only when its blit fence completed: poll_release above).
        if (content_ok_ && meta_.space_generation != gen) { content_ok_ = false; ++st_.generation_dropped; }
        if (!eye0 || !eye1 || !eye0->count || !eye1->count) { ++st_.no_chains; return false; }
        const int64_t clk = src_->clock_ms(now_);   // W14-1 F1: the clock of SlotMeta::published_ms
        if (!holding_) {
            acz::brief3d::live::Held h; TableImages img;
            if (src_->acquire(last_id_, h, img)) {
                last_id_ = h.a.id;
                if (!h.meta.space_generation || h.meta.space_generation != gen) {   // rendered for another (or an unknown) LOCAL generation:
                    src_->release(h); ++st_.generation_refused;                     // refused BEFORE any eye acquisition
                } else if (!slot_published_fresh(h.meta.published_ms, clk, kStaleMs)) {   // W14-1 F1: an old READY slot never regains freshness
                    src_->release(h); ++st_.stale_published;                                // by being acquired late: refused before any eye acquisition
                } else {
                    last_new_ms_ = now_;
                    blit(g, xr, *eye0, *eye1, e0, e1, h, img);
                    if (poisoned_ || dead_) return false;
                }
            }
        }
        if (!content_ok_) { ++st_.no_image; return false; }
        if (now_ - last_new_ms_ > kStaleMs) { ++st_.stale; return false; }
        if (!slot_published_fresh(meta_.published_ms, clk, kStaleMs)) { ++st_.stale; return false; }   // W14-1 F1: retained content expires on its publication time
        plan.layer.table = true;
        plan.layer.table_opaque = meta_.opaque;   // UI3D (ui-3d.md 4 item 4): the slot's own flag
        plan.layer.table_front = layering_ != 0;     // UI3D 2026-10-30b (u3q): quad layers draw above projections: no screen quad over the 3D (compose_layers)
        for (unsigned e = 0; e < 2; ++e) {
            const acz::brief3d::EyeView& v = meta_.eyes[e];
            auto& o = plan.layer.views[e];
            for (int k = 0; k < 3; ++k) o.pose.position[k] = float(v.position[k]);
            for (int k = 0; k < 4; ++k) o.pose.orientation[k] = float(v.orientation[k]);
            o.fov.left = float(v.fov[0]); o.fov.right = float(v.fov[1]); o.fov.up = float(v.fov[2]); o.fov.down = float(v.fov[3]);
        }
        ++st_.layers;
        return true;
    }
    // UI3D 2026-10-30b (u3q): 1 (default) = the layer list never lets the screen quad cover the 3D (ProjectionLayer::table_front); 0 = the old list.
    void set_layering(unsigned v) noexcept { layering_ = v; }
    // Owner thread, a frame without the screen (flight): only the held slot is released when its blit completed.
    void idle(Gate& g) { now_ = g.now_ms(); poll_release(); }
    // SCREEN-DUMP (xr_screen_dump.h): the next blit also reads back eye 0 (the image the table layer submits for the left eye).
    const char* dump_arm(int64_t now_ms) {
        if (poisoned_ || dead_) return "table-dead";
        if (!content_ok_ || now_ - last_new_ms_ > kStaleMs) return "table-not-live";
        if (dump_.busy()) return "busy";
        dump_.arm(now_ms);
        return nullptr;
    }
    // The eye-0 part: the premultiplied layer composited over black (opaque), written as stored (like xr_png_once eye dumps; xr_screen_dump.h).
    bool dump_take(Gate& g, DumpImage& out) {
        if (dump_.draining) {   // a given-up (timed-out/lost) readback: reclaimed only once its fence value is proven complete
            if (dump_.reclaimable(gpu_->completed())) {
                bool done = false;
                (void)g.call(Op::d3d_sample, [&] { gpu_->readback_discard(); done = true; return Rc::ok; });
                if (done) dump_.drained();
            }
            return false;
        }
        if (!dump_.busy()) return false;
        const acz::vscreen::Completion c = dump_.state == DumpTrack::pending ? gpu_->completed() : acz::vscreen::Completion{};
        const bool r = dump_.poll(g.now_ms(), c, "eye0-table", false, out, [&](std::vector<uint8_t>& v, uint32_t& w, uint32_t& h) {
            bool ok = false;
            (void)g.call(Op::d3d_sample, [&] { ok = gpu_->readback_take(v, w, h); return Rc::ok; });
            return ok;
        });
        if (r && out.ok) premultiplied_over_black(out.rgba);
        return r;
    }
    bool dump_busy() const noexcept { return dump_.busy(); }
    // Output acquired an eye target for the flight composite: the eye swapchains no longer hold the table.
    void target_written(unsigned t) noexcept { if (t < 2) { if (content_ok_) ++st_.invalidated; content_ok_ = false; } }
    // (F3) this frame() acquired (or tried to acquire) the eye swapchains: what they last released is no longer another core's image.
    bool touched() const noexcept { return touched_; }

    bool retire(Gate& g) {
        if (poisoned_ || quarantined_) { pending_ = line(true); return false; }
        if (value_) {
            const int64_t until = g.now_ms() + 1000;
            acz::vscreen::Completion c = gpu_->completed();
            while (!c.lost && !c.reached(value_) && g.now_ms() < until && !g.lost()) { g.sleep_ms(5); c = gpu_->completed(); }
            if (!c.reached(value_)) { why_ = c.lost ? "retire-fence-lost" : "retire-timeout"; pending_ = line(true); return false; }
        }
        if (holding_) { src_->release(held_); holding_ = false; }
        if (setup_done_ && !destroyed_) gpu_->destroy();
        setup_done_ = true; setup_ok_ = false; dead_ = true; content_ok_ = false; destroyed_ = true;
        pending_ = line(true);
        return true;
    }
    std::string summary() {
        if (!pending_.empty()) { std::string t; t.swap(pending_); return t; }
        if (now_ - last_summary_ms_ < 5000) return {};
        last_summary_ms_ = now_;
        return line(false);
    }
    struct Stats { uint64_t blits = 0, blit_failed = 0, layers = 0, no_image = 0, stale = 0, no_chains = 0, alloc_busy = 0, invalidated = 0, lost = 0,
                            generation_refused = 0, generation_dropped = 0, stale_published = 0; };
    const Stats& stats() const noexcept { return st_; }
    bool poisoned() const noexcept { return poisoned_; }
    bool holding() const noexcept { return holding_; }
    bool content_ok() const noexcept { return content_ok_; }
    bool quarantined() const noexcept { return quarantined_; }
    bool dead() const noexcept { return dead_; }
    const char* why() const noexcept { return why_; }

private:
    void poison(const char* why) noexcept { poisoned_ = true; dead_ = true; content_ok_ = false; why_ = why; }
    void poll_release() noexcept {
        if (!holding_ || quarantined_) return;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; return; }   // held for ever: loss is never completion
        if (c.reached(held_value_)) { src_->release(held_); holding_ = false; }
    }
    // Acquire + wait one eye target. 0 = acquired and waited; 1 = nothing acquired (acquire failed); 2 = acquired, never waited (poison).
    int take(Gate& g, SwapchainRuntime& xr, unsigned t, const SwapchainImages& imgs, uint32_t& index) {
        Rc rc = g.call(Op::acquire_image, [&] { return xr.acquire(t, index); });
        if (!succeeded(rc)) return 1;
        if (index >= imgs.count) return 2;
        bool timed_out = false;
        rc = g.call(Op::wait_image, [&] { return xr.wait(t, 0, timed_out); });
        if (succeeded(rc) && timed_out) rc = g.call(Op::wait_image, [&] { return xr.wait(t, 100000000, timed_out); });   // 100 ms
        return succeeded(rc) && !timed_out ? 0 : 2;
    }
    void blit(Gate& g, SwapchainRuntime& xr, const SwapchainImages& i0, const SwapchainImages& i1, Extent e0, Extent e1,
              const acz::brief3d::live::Held& h, const TableImages& img) {
        if (!setup_done_) {
            setup_done_ = true;
            const char* why = "setup";
            setup_ok_ = gpu_->setup(i0, i1, why);
            if (!setup_ok_) why_ = why;
        }
        if (!setup_ok_) { dead_ = true; src_->release(h); ++st_.blit_failed; return; }
        if (!img.eye[0] || !img.eye[1]) { src_->release(h); ++st_.blit_failed; why_ = "slot-images-missing"; return; }
        const unsigned k = next_alloc_;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; src_->release(h); return; }
        if (!c.reached(alloc_value_[k])) { ++st_.alloc_busy; src_->release(h); return; }
        uint32_t x0 = 0, x1 = 0;
        touched_ = true;
        const int r0 = take(g, xr, 0, i0, x0);
        if (r0 == 1) { src_->release(h); dead_ = true; why_ = "acquire-eye0"; ++st_.blit_failed; return; }   // nothing acquired
        content_ok_ = false;   // from here the eye swapchains' next released images are unknown until the blit is proven recorded
        if (r0 == 2) { src_->release(h); poison("wait-eye0"); return; }
        const int r1 = take(g, xr, 1, i1, x1);
        if (r1 != 0) {
            src_->release(h);
            if (r1 == 2) { poison("wait-eye1"); return; }
            const Rc rr = g.call(Op::release_image, [&] { return xr.release(0); });   // eye 0 released unwritten (content unknown)
            if (!succeeded(rr)) { poison("release-eye0"); return; }
            dead_ = true; why_ = "acquire-eye1"; ++st_.blit_failed; return;
        }
        TableGpu::Submit s;
        const bool want = dump_.want();
        gpu_->readback_want(want);
        (void)g.call(Op::d3d_execute, [&] { s = gpu_->blit(k, i0.images[x0], i1.images[x1], img, e0, e1); return s.signalled ? Rc::ok : Rc::error; });
        if (want && s.executed && s.signalled) { if (s.readback) dump_.submitted(true, s.value, now_); else dump_.refuse("readback-setup"); }
        const Rc rr1 = g.call(Op::release_image, [&] { return xr.release(1); });
        const Rc rr0 = g.call(Op::release_image, [&] { return xr.release(0); });
        if (s.executed && !s.signalled) { held_ = h; holding_ = true; quarantined_ = true; poison("signal"); return; }
        if (!s.executed) {
            src_->release(h); ++st_.blit_failed; why_ = "record";
            if (!succeeded(rr1) || !succeeded(rr0)) poison("release-image");
            return;
        }
        held_ = h; holding_ = true; held_value_ = s.value; value_ = s.value; alloc_value_[k] = s.value; next_alloc_ = k ^ 1u;
        ++st_.blits;
        if (!succeeded(rr1) || !succeeded(rr0)) { poison("release-image"); return; }
        meta_ = h.meta; content_ok_ = true;
    }
public:
    // The table line (without the producer tail). flags: content, holding, dead, poisoned, quarantined (bits 0..4). Counters clamped to 7 digits,
    // t-ms to 9, why to 24 bytes: with the producer tail (format_xr_tail) the line stays < kLineLimit for any value (offline-tested).
    static int format_line(char* buf, size_t n, bool final_, int64_t now, const Stats& s, unsigned flags, const char* why, uint64_t frame) noexcept {
        using acz::brief3d::live::clamp_digits;
        auto c = [](uint64_t v) { return clamp_digits(v, 7); };
        return std::snprintf(buf, n, "xr-stereo;event=%s;t-ms=%llu;blits=%llu;blit-failed=%llu;layers=%llu;no-image=%llu;stale=%llu;no-chains=%llu;"
            "alloc-busy=%llu;invalidated=%llu;lost=%llu;gen-refused=%llu;gen-dropped=%llu;content=%u;holding=%u;dead=%u;poisoned=%u;quarantined=%u;"
            "why=%.24s;frame=%llu;",
            final_ ? "brief3d-table-result" : "summary-brief3d-table", clamp_digits(uint64_t(now > 0 ? now : 0), 9), c(s.blits), c(s.blit_failed),
            c(s.layers), c(s.no_image), c(s.stale), c(s.no_chains), c(s.alloc_busy), c(s.invalidated), c(s.lost), c(s.generation_refused),
            c(s.generation_dropped), flags & 1u, (flags >> 1) & 1u, (flags >> 2) & 1u, (flags >> 3) & 1u, (flags >> 4) & 1u, why ? why : "none", c(frame));
    }
private:
    // Two lines joined by '\n' (Output emits each separately): the table line + the producer tail, then the producer's own line.
    std::string line(bool final_) {
        char t[acz::brief3d::kLineLimit];
        format_line(t, sizeof t, final_, now_, st_, (content_ok_ ? 1u : 0u) | (holding_ ? 2u : 0u) | (dead_ ? 4u : 0u) | (poisoned_ ? 8u : 0u) |
                    (quarantined_ ? 16u : 0u), why_, meta_.frame);
        return std::string(t) + src_->describe();
    }

    std::shared_ptr<TableGpu> gpu_;
    std::shared_ptr<TableSource> src_;
    acz::brief3d::live::Held held_;
    acz::brief3d::live::SlotMeta meta_;
    uint64_t alloc_value_[2] = {0, 0};
    unsigned next_alloc_ = 0;
    uint64_t value_ = 0, held_value_ = 0, last_id_ = 0;
    bool holding_ = false, content_ok_ = false, setup_done_ = false, setup_ok_ = false, dead_ = false, poisoned_ = false, quarantined_ = false,
         destroyed_ = false, touched_ = false;
    const char* why_ = "none";
    int64_t now_ = 0, last_new_ms_ = 0, last_summary_ms_ = 0;
    unsigned layering_ = 1;   // u3q
    std::string pending_;
    Stats st_;
    DumpTrack dump_;   // SCREEN-DUMP
};

// ================================================================================================== brief3d.md 11 (b3l:1): the latched table
// The source of the latched scene (DLL: the producer, brief3d_live.h latched_scene(); tests: a fake or the real producer).
struct LatchSource {
    virtual ~LatchSource() = default;
    virtual std::shared_ptr<const acz::brief3d::latch::Scene> scene() = 0;
    virtual bool dead() = 0;
    virtual int64_t clock_ms(int64_t gate_now) { return gate_now; }   // the clock of Scene::published_ms (DLL: GetTickCount64)
};
inline acz::brief3d::EyeView eye_view(const xr_owner::EyeSample& s) noexcept {
    acz::brief3d::EyeView v;
    for (int k = 0; k < 3; ++k) v.position[k] = s.pose.position[k];
    for (int k = 0; k < 4; ++k) v.orientation[k] = s.pose.orientation[k];
    v.fov[0] = s.fov.left; v.fov[1] = s.fov.right; v.fov[2] = s.fov.up; v.fov[3] = s.fov.down;
    return v;
}
// Headset test 8 (docs/world-render/brief3d-worldlock.md): the depth along the eye's view axis of the table centre (LOCAL `c`), the plane depth the
// table quad of that eye is submitted at (ProjectionLayer::table_quad_distance_m). The quad is the table's picture drawn for exactly this eye pose;
// its plane at the table's own depth gives zero parallax error for the table under head-pose prediction error, the 1.5 m default gave 0.67 rad/m of
// it. A centre behind / beside the eye (axial depth < 0.3 m) falls back to the straight distance. Always > 0 for finite input.
inline float table_axial_depth(const xr_owner::EyeSample& v, const double c[3]) noexcept {
    const double x = v.pose.orientation[0], y = v.pose.orientation[1], z = v.pose.orientation[2], w = v.pose.orientation[3];
    const double fx = -2.0 * (x * z + w * y), fy = -2.0 * (y * z - w * x), fz = -(1.0 - 2.0 * (x * x + y * y));   // R * (0, 0, -1)
    const double dx = c[0] - v.pose.position[0], dy = c[1] - v.pose.position[1], dz = c[2] - v.pose.position[2];
    const double axial = dx * fx + dy * fy + dz * fz, straight = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double d = axial >= 0.3 ? axial : straight;
    return std::isfinite(d) && d > 0.0 ? float(d) : 0.0f;
}
// Every XR frame in which the screen is planned and a fresh scene exists: plan_frame() with THIS frame's located views (FrameInfo::views),
// TableGpu::render() draws the scene for both eyes and blits it into the eye swapchains, and the table layer is submitted with exactly those
// views. The table's pixels are therefore always rendered for the pose the compositor displays them with: no stale image, no translation error
// left for a rotation-only reprojection, independent of the game's map draw rate. Ownership (the TableCore rules): both eye acquisitions are
// settled inside frame(); a wait that never succeeds or a failed release poisons; an Execute without a Signal quarantines (the scene stays
// referenced for ever) and poisons; each allocator's scene (the game's PSO / root signature references) is kept until that allocator's fence
// value completed.
// Review 2026-10-31 F3: the latch renders on the game's DIRECT queue behind the game's own work; at 90 Hz the allocator of kTableAllocs frames
// back is often still busy. Such a frame used to submit NO table layer (the table blinked). Now it RESUBMITS the layer: the eye swapchains still
// hold this core's last released render (nothing acquired them since: no slot blit, no flight composite, no failed acquisition), so the layer is
// submitted again with exactly the views that render was drawn with (the compositor reprojects it like any late frame). alloc-busy keeps counting
// the busy frames; resubmits counts the layers submitted that way.
// b3p:1 (headset test 9): the plane as the submitted quad, an EyeView as the layer's view, and the layer fields of a world-fixed table quad.
inline Quad plane_quad(const acz::brief3d::latch::TablePlane& p) noexcept {
    Quad q;
    for (int k = 0; k < 3; ++k) q.pose.position[k] = float(p.centre[k]);
    for (int k = 0; k < 4; ++k) q.pose.orientation[k] = float(p.orientation[k]);
    q.width = float(p.width); q.height = float(p.height);
    return q;
}
inline xr_owner::EyeSample plane_eye_sample(const acz::brief3d::EyeView& v) noexcept {
    xr_owner::EyeSample s;
    for (int k = 0; k < 3; ++k) s.pose.position[k] = float(v.position[k]);
    for (int k = 0; k < 4; ++k) s.pose.orientation[k] = float(v.orientation[k]);
    s.fov.left = float(v.fov[0]); s.fov.right = float(v.fov[1]); s.fov.up = float(v.fov[2]); s.fov.down = float(v.fov[3]);
    return s;
}
inline void set_fixed(xr_owner::ProjectionLayer& l, bool fixed, const Quad& q) noexcept {
    l.table_quad_fixed = fixed;
    if (fixed) { l.table_quad_pose = q.pose; l.table_quad_width = q.width; l.table_quad_height = q.height; }
}
class LatchCore {
public:
    static constexpr int64_t kStaleMs = 500;   // no new scene for this long (the map screen was left): no table
    static constexpr unsigned kAllocs = kTableAllocs;
    LatchCore(std::shared_ptr<TableGpu> gpu, std::shared_ptr<LatchSource> src) : gpu_(std::move(gpu)), src_(std::move(src)) {}

    bool frame(Gate& g, SwapchainRuntime& xr, const SwapchainImages* eye0, const SwapchainImages* eye1, Extent e0, Extent e1,
               const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
        now_ = g.now_ms(); touched_ = false;
        poll();
        if (poisoned_ || dead_) return false;
        if (src_->dead()) { dead_ = true; why_ = "producer-dead"; return false; }
        std::shared_ptr<const acz::brief3d::latch::Scene> sc = src_->scene();
        if (!sc) { ++st_.no_scene; return false; }
        const int64_t clk = src_->clock_ms(now_);
        if (!slot_published_fresh(sc->published_ms, clk, kStaleMs)) { ++st_.stale; return false; }
        if (!sc->space_generation || sc->space_generation != encode_space_generation(info.space_generation)) { ++st_.generation_refused; return false; }
        if (!info.views_valid) { ++st_.no_views; return false; }
        if (!eye0 || !eye1 || !eye0->count || !eye1->count) { ++st_.no_chains; return false; }
        acz::brief3d::EyeView eyes[2] = {eye_view(info.views.eyes[0]), eye_view(info.views.eyes[1])};
        // headset test 9 (b3p:1, quads only): both eyes drawn onto the scene's world-fixed plane from their positions; an eye at / behind the
        // plane or a grazing view keeps this frame on the per-eye quads (plane-off)
        bool fixed = false;
        if (layering_ != 0 && sc->plane.valid) {
            acz::brief3d::EyeView pv[2];
            const char* pwhy = acz::brief3d::latch::plane_view(sc->plane, eyes[0].position, pv[0]);
            if (!pwhy) pwhy = acz::brief3d::latch::plane_view(sc->plane, eyes[1].position, pv[1]);
            if (!pwhy) { eyes[0] = pv[0]; eyes[1] = pv[1]; fixed = true; } else { ++st_.plane_off; plane_why_ = pwhy; }
        }
        if (const char* why = acz::brief3d::latch::plan_frame(sc, eyes, fp_)) { ++st_.plan_failed; plan_why_ = why; return false; }
        if (!setup_done_) {
            setup_done_ = true;
            const char* why = "setup";
            setup_ok_ = gpu_->setup(*eye0, *eye1, why);
            if (!setup_ok_) why_ = why;
        }
        if (!setup_ok_) { dead_ = true; return false; }
        const unsigned k = next_alloc_;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; return false; }
        if (!c.reached(alloc_value_[k])) { ++st_.alloc_busy; return resubmit(info.space_generation, clk, plan); }
        uint32_t x0 = 0, x1 = 0;
        touched_ = true;   // from here the eye swapchains' next released images are this core's (or unknown)
        shown_ok_ = false;
        const int r0 = take(g, xr, 0, *eye0, x0);
        if (r0 == 1) { dead_ = true; why_ = "acquire-eye0"; ++st_.render_failed; return false; }
        if (r0 == 2) { poison("wait-eye0"); return false; }
        const int r1 = take(g, xr, 1, *eye1, x1);
        if (r1 != 0) {
            if (r1 == 2) { poison("wait-eye1"); return false; }
            const Rc rr = g.call(Op::release_image, [&] { return xr.release(0); });   // eye 0 released unwritten (content unknown)
            if (!succeeded(rr)) { poison("release-eye0"); return false; }
            dead_ = true; why_ = "acquire-eye1"; ++st_.render_failed; return false;
        }
        TableGpu::Submit s;
        const bool want = dump_.want();
        gpu_->readback_want(want);
        (void)g.call(Op::d3d_execute, [&] { s = gpu_->render(k, eye0->images[x0], eye1->images[x1], fp_, e0, e1); return s.signalled ? Rc::ok : Rc::error; });
        if (want && s.executed && s.signalled) { if (s.readback) dump_.submitted(true, s.value, now_); else dump_.refuse("readback-setup"); }
        const Rc rr1 = g.call(Op::release_image, [&] { return xr.release(1); });
        const Rc rr0 = g.call(Op::release_image, [&] { return xr.release(0); });
        if (s.executed && !s.signalled) { stuck_ = sc; quarantined_ = true; poison("signal"); return false; }   // completion never provable
        if (!s.executed) {
            ++st_.render_failed; why_ = "record";
            if (!succeeded(rr1) || !succeeded(rr0)) poison("release-image");
            return false;
        }
        inflight_[k] = sc; alloc_value_[k] = s.value; value_ = s.value; next_alloc_ = (k + 1u) % kAllocs;
        ++st_.renders; st_.clipped += fp_.clipped; st_.culled_copies += fp_.culled_copies; st_.skipped += fp_.skipped;
        st_.age_ms = clk > sc->published_ms ? uint64_t(clk - sc->published_ms) : 0;
        if (st_.age_ms > st_.age_max_ms) st_.age_max_ms = st_.age_ms;
        if (sc->serial != last_serial_) { last_serial_ = sc->serial; ++st_.scenes; }
        if (!succeeded(rr1) || !succeeded(rr0)) { poison("release-image"); return false; }
        plan.layer.table = true;
        plan.layer.table_opaque = false;
        plan.layer.table_front = layering_ != 0;
        for (unsigned e = 0; e < 2; ++e) {
            plan.layer.views[e] = fixed ? plane_eye_sample(eyes[e]) : info.views.eyes[e];   // exactly the views the scene was drawn with: THIS frame's
            shown_dist_[e] = plan.layer.table_quad_distance_m[e] = table_axial_depth(info.views.eyes[e], sc->placement.centre);
        }
        shown_fixed_ = fixed;
        if (fixed) { shown_quad_ = plane_quad(sc->plane); ++st_.plane_frames; }
        set_fixed(plan.layer, fixed, shown_quad_);
        ++st_.layers;
        live_ms_ = now_;
        // (F3) what the eye swapchains now hold: this render, drawn with these views, of a scene published at shown_pub_ in generation shown_gen_
        shown_ok_ = true; shown_gen_ = sc->space_generation; shown_pub_ = sc->published_ms;
        for (unsigned e = 0; e < 2; ++e) shown_views_[e] = plan.layer.views[e];
        return true;
    }
    void set_layering(unsigned v) noexcept { layering_ = v; }
    void idle(Gate& g) { now_ = g.now_ms(); touched_ = false; poll(); }
    bool touched() const noexcept { return touched_; }
    // (F3) another writer released into the eye swapchains (the slot path's blit, Output's flight composite): the last render is no longer shown
    // by them, so it is never resubmitted.
    void target_written(unsigned t) noexcept { if (t < 2) shown_ok_ = false; }
    bool live() const noexcept { return live_ms_ && now_ - live_ms_ <= kStaleMs && !poisoned_ && !dead_; }
    // SCREEN-DUMP (as TableCore): the next render also reads back eye 0.
    const char* dump_arm(int64_t now_ms) {
        if (poisoned_ || dead_) return "table-dead";
        if (!live()) return "table-not-live";
        if (dump_.busy()) return "busy";
        dump_.arm(now_ms);
        return nullptr;
    }
    bool dump_take(Gate& g, DumpImage& out) {
        if (dump_.draining) {
            if (dump_.reclaimable(gpu_->completed())) {
                bool done = false;
                (void)g.call(Op::d3d_sample, [&] { gpu_->readback_discard(); done = true; return Rc::ok; });
                if (done) dump_.drained();
            }
            return false;
        }
        if (!dump_.busy()) return false;
        const acz::vscreen::Completion c = dump_.state == DumpTrack::pending ? gpu_->completed() : acz::vscreen::Completion{};
        const bool r = dump_.poll(g.now_ms(), c, "eye0-table", false, out, [&](std::vector<uint8_t>& v, uint32_t& w, uint32_t& h) {
            bool ok = false;
            (void)g.call(Op::d3d_sample, [&] { ok = gpu_->readback_take(v, w, h); return Rc::ok; });
            return ok;
        });
        if (r && out.ok) premultiplied_over_black(out.rgba);
        return r;
    }
    bool dump_busy() const noexcept { return dump_.busy(); }
    bool retire(Gate& g) {
        if (poisoned_ || quarantined_) { pending_ = line(true); return false; }
        if (value_) {
            const int64_t until = g.now_ms() + 1000;
            acz::vscreen::Completion c = gpu_->completed();
            while (!c.lost && !c.reached(value_) && g.now_ms() < until && !g.lost()) { g.sleep_ms(5); c = gpu_->completed(); }
            if (!c.reached(value_)) { why_ = c.lost ? "retire-fence-lost" : "retire-timeout"; pending_ = line(true); return false; }
        }
        for (auto& f : inflight_) f.reset();
        if (setup_done_ && !destroyed_) gpu_->destroy();
        setup_done_ = true; setup_ok_ = false; dead_ = true; destroyed_ = true; shown_ok_ = false;
        pending_ = line(true);
        return true;
    }
    std::string summary() {
        if (!pending_.empty()) { std::string t; t.swap(pending_); return t; }
        if (now_ - last_summary_ms_ < 5000) return {};
        last_summary_ms_ = now_;
        return line(false);
    }
    struct Stats { uint64_t renders = 0, render_failed = 0, layers = 0, scenes = 0, no_scene = 0, stale = 0, generation_refused = 0, no_views = 0,
                   no_chains = 0, plan_failed = 0, alloc_busy = 0, lost = 0, clipped = 0, culled_copies = 0, skipped = 0, age_ms = 0, age_max_ms = 0,
                   resubmits = 0, plane_frames = 0, plane_off = 0; };
    const Stats& stats() const noexcept { return st_; }
    const acz::brief3d::latch::FramePlan& last_plan() const noexcept { return fp_; }
    bool poisoned() const noexcept { return poisoned_; }
    bool dead() const noexcept { return dead_; }
    bool quarantined() const noexcept { return quarantined_; }
    unsigned inflight() const noexcept { unsigned n = 0; for (const auto& f : inflight_) n += f ? 1u : 0u; return n; }
    const char* why() const noexcept { return why_; }
    static int format_line(char* buf, size_t n, bool final_, int64_t now, const Stats& s, bool dead, bool poisoned, const char* why, const char* plan_why) noexcept {
        using acz::brief3d::live::clamp_digits;
        auto c = [](uint64_t v) { return clamp_digits(v, 7); };
        return std::snprintf(buf, n, "xr-stereo;event=%s;t-ms=%llu;renders=%llu;render-failed=%llu;layers=%llu;scenes=%llu;no-scene=%llu;stale=%llu;"
            "gen-refused=%llu;no-views=%llu;no-chains=%llu;plan-failed=%llu;alloc-busy=%llu;resubmits=%llu;lost=%llu;clipped=%llu;culled-copies=%llu;skipped=%llu;"
            "age-ms=%llu;age-max-ms=%llu;plane=%llu;plane-off=%llu;dead=%u;poisoned=%u;why=%.24s;plan-why=%.24s",
            final_ ? "brief3d-latch-result" : "summary-brief3d-latch", clamp_digits(uint64_t(now > 0 ? now : 0), 9), c(s.renders), c(s.render_failed),
            c(s.layers), c(s.scenes), c(s.no_scene), c(s.stale), c(s.generation_refused), c(s.no_views), c(s.no_chains), c(s.plan_failed), c(s.alloc_busy),
            c(s.resubmits), c(s.lost), clamp_digits(s.clipped, 9), c(s.culled_copies), c(s.skipped), c(s.age_ms), c(s.age_max_ms), c(s.plane_frames), c(s.plane_off),
            dead ? 1u : 0u, poisoned ? 1u : 0u,
            why ? why : "none", plan_why ? plan_why : "none");
    }
private:
    void poison(const char* why) noexcept { poisoned_ = true; dead_ = true; shown_ok_ = false; why_ = why; }
    void poll() noexcept {
        if (!inflight()) return;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; return; }   // loss is never completion: the scenes stay referenced
        for (unsigned k = 0; k < kAllocs; ++k) if (inflight_[k] && c.reached(alloc_value_[k])) inflight_[k].reset();
    }
    // (F3) the busy frame: the previous render's layer again, if the eye swapchains still hold it and it is still valid for this frame (the same
    // LOCAL generation, its scene's publication still fresh). Nothing is acquired, recorded or released.
    bool resubmit(uint32_t space_generation, int64_t clk, xr_owner::LayerPlan& plan) noexcept {
        if (!shown_ok_ || !shown_gen_ || shown_gen_ != encode_space_generation(space_generation)) return false;
        if (!slot_published_fresh(shown_pub_, clk, kStaleMs)) return false;
        plan.layer.table = true;
        plan.layer.table_opaque = false;
        plan.layer.table_front = layering_ != 0;
        for (unsigned e = 0; e < 2; ++e) { plan.layer.views[e] = shown_views_[e]; plan.layer.table_quad_distance_m[e] = shown_dist_[e]; }   // the views (and plane depth) of that render
        set_fixed(plan.layer, shown_fixed_, shown_quad_);   // and its world-fixed quad (b3p:1)
        ++st_.layers; ++st_.resubmits;
        live_ms_ = now_;
        return true;
    }
    int take(Gate& g, SwapchainRuntime& xr, unsigned t, const SwapchainImages& imgs, uint32_t& index) {
        Rc rc = g.call(Op::acquire_image, [&] { return xr.acquire(t, index); });
        if (!succeeded(rc)) return 1;
        if (index >= imgs.count) return 2;
        bool timed_out = false;
        rc = g.call(Op::wait_image, [&] { return xr.wait(t, 0, timed_out); });
        if (succeeded(rc) && timed_out) rc = g.call(Op::wait_image, [&] { return xr.wait(t, 100000000, timed_out); });   // 100 ms
        return succeeded(rc) && !timed_out ? 0 : 2;
    }
    std::string line(bool final_) {
        char t[acz::brief3d::kLineLimit];
        format_line(t, sizeof t, final_, now_, st_, dead_, poisoned_, why_, plan_why_ ? plan_why_ : plane_why_);
        return t;
    }
    std::shared_ptr<TableGpu> gpu_;
    std::shared_ptr<LatchSource> src_;
    acz::brief3d::latch::FramePlan fp_;
    std::shared_ptr<const acz::brief3d::latch::Scene> inflight_[kAllocs], stuck_;
    uint64_t alloc_value_[kAllocs] = {}, value_ = 0, last_serial_ = 0;
    // (F3) the last render the eye swapchains hold (resubmit)
    bool shown_ok_ = false; uint32_t shown_gen_ = 0; int64_t shown_pub_ = 0;
    xr_owner::EyeSample shown_views_[2] = {};
    float shown_dist_[2] = {0.0f, 0.0f};
    bool shown_fixed_ = false; Quad shown_quad_;   // b3p:1: the world-fixed quad that render was drawn for
    unsigned next_alloc_ = 0, layering_ = 1;
    bool setup_done_ = false, setup_ok_ = false, dead_ = false, poisoned_ = false, quarantined_ = false, destroyed_ = false, touched_ = false;
    const char* why_ = "none"; const char* plan_why_ = nullptr;
    const char* plane_why_ = nullptr;   // b3p:1: the last reason a frame fell back to the per-eye quads (reported as plan-why when no plan failed)
    int64_t now_ = 0, last_summary_ms_ = 0, live_ms_ = 0;
    std::string pending_;
    Stats st_;
    DumpTrack dump_;
};
// UI3D stage 3a (u3p:1; docs/world-render/ui-3d.md 7; the decisions are tested offline, the D3D12 blend is UNTESTED LIVE). The newest READY
// PAUSE slot of the producer (SlotMeta::pause) is blended INTO the flight eye images Output already acquired and composited (cockpit + world),
// so the menu layers sit over the dimmed cockpit in the same projection layer (no extra swapchain; the flight's own release submits them).
// Ownership (the TableCore rules, adapted):
//  * this core never acquires, waits or releases a swapchain image (Output owns them); it only records into its own list on the XR binding
//    queue between Output's completion and Output's release (submission order on that queue makes the blend part of the released image);
//  * the CURRENT pause slot stays HELD while it is shown (re-composites at the XR rate blend it again: a released slot could be rewritten by the
//    producer meanwhile); a newer pause slot replaces it, and the replaced one is released only after its last blend's fence value completed;
//    a lost fence releases nothing (held for ever); an Execute without a Signal quarantines and poisons (Output then fails the session);
//  * a non-pause slot (a menu UI3D or briefing slot: not expected in flight) is released at once unused; another LOCAL generation is refused;
//  * no new pause slot for kStaleMs = no pause layer (Output keeps the HUD quad: today's hud-menu image).
// UI3D stage 3a fix round (review W13-1 F1): the pause slot's pixels were projected with the producer's eyes (pose + FOV); they are only valid in a
// flight eye image that the compositor shows with the SAME eyes. Fail closed: blend only when slot and composite eyes agree within a tight tolerance
// (position 0.5 mm, orientation 0.01 degree, FOV angle 1e-4 rad per edge); otherwise Output keeps the HUD quad (today's hud-menu image).
inline bool pause_eye_match(const acz::brief3d::EyeView& a, const acz::brief3d::EyeView& b) noexcept {
    for (int k = 0; k < 3; ++k) if (!(std::fabs(a.position[k] - b.position[k]) <= 5e-4)) return false;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int k = 0; k < 4; ++k) { dot += a.orientation[k] * b.orientation[k]; na += a.orientation[k] * a.orientation[k]; nb += b.orientation[k] * b.orientation[k]; }
    if (!(na > 0.5 && nb > 0.5 && na < 2.0 && nb < 2.0)) return false;
    const double c = std::fabs(dot) / std::sqrt(na * nb);
    if (!(1.0 - c <= 4e-9)) return false;
    for (int k = 0; k < 4; ++k) if (!(std::fabs(a.fov[k] - b.fov[k]) <= 1e-4)) return false;
    return true;
}

class PauseCore {
public:
    static constexpr int64_t kStaleMs = 500;
    static constexpr int64_t kFutureSlackMs = 50;   // a publication stamped later than the clock by more than this is invalid
    static constexpr unsigned kRetiring = 4;
    PauseCore(std::shared_ptr<TableGpu> gpu, std::shared_ptr<TableSource> src) : gpu_(std::move(gpu)), src_(std::move(src)) {}

    // `eyes` = the two eyes (pose + FOV, the layer's convention) the flight composite is submitted with; null = unknown -> no blend (fail closed).
    bool frame(Gate& g, const SwapchainImages& i0, const SwapchainImages& i1, void* dst0, void* dst1, Extent e0, Extent e1, uint32_t space_generation,
               float floor, const acz::brief3d::EyeView* eyes) {
        now_ = g.now_ms();
        const int64_t clk = src_->clock_ms(now_);   // the clock of SlotMeta::published_ms
        poll();
        if (poisoned_ || dead_) return false;
        if (src_->dead()) { dead_ = true; why_ = "producer-dead"; return false; }
        const uint32_t gen = encode_space_generation(space_generation);
        if (has_cur_ && cur_.meta.space_generation != gen) { ++st_.generation_dropped; retire_cur(); }
        acz::brief3d::live::Held h; TableImages img;
        if (free_retiring() && src_->acquire(last_id_, h, img)) {
            last_id_ = h.a.id;
            if (!h.meta.pause) { src_->release(h); ++st_.not_pause; }
            else if (!h.meta.space_generation || h.meta.space_generation != gen) { src_->release(h); ++st_.generation_refused; }
            else if (!published_fresh(h.meta.published_ms, clk)) { src_->release(h); ++st_.stale_published; }   // F2: age counts from the publication
            else { retire_cur(); cur_ = h; cur_img_ = img; cur_value_ = 0; has_cur_ = true; ++st_.slots; }
        }
        if (!has_cur_) { ++st_.no_slot; return false; }
        if (!published_fresh(cur_.meta.published_ms, clk)) { ++st_.stale; return false; }   // re-blends expire on the publication time too
        if (!eyes) { ++st_.view_mismatch; return false; }
        for (unsigned e = 0; e < 2; ++e) if (!pause_eye_match(cur_.meta.eyes[e], eyes[e])) { ++st_.view_mismatch; return false; }   // F1
        if (!dst0 || !dst1 || !e0.w || !e0.h || !e1.w || !e1.h) { ++st_.no_target; return false; }
        if (!setup_done_) {
            setup_done_ = true;
            const char* why = "setup";
            setup_ok_ = gpu_->setup(i0, i1, why);
            if (!setup_ok_) { why_ = why; dead_ = true; return false; }
        }
        if (!setup_ok_) return false;
        if (!cur_img_.eye[0] || !cur_img_.eye[1]) { ++st_.blit_failed; why_ = "slot-images-missing"; return false; }
        const unsigned k = next_alloc_;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; return false; }
        if (!c.reached(alloc_value_[k])) { ++st_.alloc_busy; return false; }
        TableImages src = cur_img_;
        src.pause = true; src.opaque = false; src.floor = floor;
        TableGpu::Submit s;
        gpu_->readback_want(false);
        (void)g.call(Op::d3d_execute, [&] { s = gpu_->blit(k, dst0, dst1, src, e0, e1); return s.signalled ? Rc::ok : Rc::error; });
        if (s.executed && !s.signalled) { quarantined_ = true; poison("signal"); return false; }   // cur_ stays held for ever
        if (!s.executed) { ++st_.blit_failed; why_ = "record"; return false; }
        cur_value_ = s.value; value_ = s.value; alloc_value_[k] = s.value; next_alloc_ = k ^ 1u;
        ++st_.blends;
        return true;
    }
    // A flight composite without menu mode: the shown slot is retired (released once its blends completed).
    void idle(Gate& g) { now_ = g.now_ms(); if (has_cur_ && !poisoned_) retire_cur(); poll(); }
    bool retire(Gate& g) {
        if (poisoned_ || quarantined_) { pending_ = line(true); return false; }
        if (value_) {
            const int64_t until = g.now_ms() + 1000;
            acz::vscreen::Completion c = gpu_->completed();
            while (!c.lost && !c.reached(value_) && g.now_ms() < until && !g.lost()) { g.sleep_ms(5); c = gpu_->completed(); }
            if (!c.reached(value_)) { why_ = c.lost ? "retire-fence-lost" : "retire-timeout"; pending_ = line(true); return false; }
        }
        if (has_cur_) { src_->release(cur_); has_cur_ = false; }
        for (unsigned i = 0; i < n_old_; ++i) src_->release(old_[i].h);
        n_old_ = 0;
        if (setup_done_ && !destroyed_) gpu_->destroy();   // F3: also after a PARTIAL setup (TableGpu::destroy contract), as TableCore
        destroyed_ = true; dead_ = true;
        pending_ = line(true);
        return true;
    }
    std::string summary() {
        if (!pending_.empty()) { std::string t; t.swap(pending_); return t; }
        if (now_ - last_summary_ms_ < 5000) return {};
        last_summary_ms_ = now_;
        return line(false);
    }
    struct Stats { uint64_t blends = 0, slots = 0, blit_failed = 0, no_slot = 0, stale = 0, not_pause = 0, generation_refused = 0, generation_dropped = 0,
                   alloc_busy = 0, no_target = 0, lost = 0, released = 0, stale_published = 0, view_mismatch = 0; };
    const Stats& stats() const noexcept { return st_; }
    bool poisoned() const noexcept { return poisoned_; }
    bool dead() const noexcept { return dead_; }
    bool holding() const noexcept { return has_cur_; }
    unsigned retiring() const noexcept { return n_old_; }
    const char* why() const noexcept { return why_; }
    static int format_line(char* buf, size_t n, bool final_, int64_t now, const Stats& s, unsigned held, bool dead, bool poisoned, const char* why) noexcept {
        using acz::brief3d::live::clamp_digits;
        auto c = [](uint64_t v) { return clamp_digits(v, 7); };
        return std::snprintf(buf, n, "xr-stereo;event=%s;t-ms=%llu;blends=%llu;slots=%llu;blit-failed=%llu;no-slot=%llu;stale=%llu;not-pause=%llu;"
            "gen-refused=%llu;gen-dropped=%llu;alloc-busy=%llu;no-target=%llu;lost=%llu;released=%llu;stale-pub=%llu;view-mismatch=%llu;held=%u;dead=%u;poisoned=%u;why=%.24s",
            final_ ? "ui3d-pause-result" : "summary-ui3d-pause", clamp_digits(uint64_t(now > 0 ? now : 0), 9), c(s.blends), c(s.slots), c(s.blit_failed),
            c(s.no_slot), c(s.stale), c(s.not_pause), c(s.generation_refused), c(s.generation_dropped), c(s.alloc_busy), c(s.no_target), c(s.lost),
            c(s.released), c(s.stale_published), c(s.view_mismatch), held, dead ? 1u : 0u, poisoned ? 1u : 0u, why ? why : "none");
    }

private:
    struct Old { acz::brief3d::live::Held h; uint64_t value = 0; };
    void poison(const char* why) noexcept { poisoned_ = true; dead_ = true; why_ = why; }
    // The shown slot leaves the screen: released now if never blended, else once its last blend completed.
    void retire_cur() noexcept {
        if (!has_cur_) return;
        has_cur_ = false;
        if (!cur_value_) { src_->release(cur_); ++st_.released; return; }
        if (n_old_ < kRetiring) { old_[n_old_].h = cur_; old_[n_old_].value = cur_value_; ++n_old_; return; }
        why_ = "retiring-full"; poison("retiring-full");   // unreachable with 3 ring slots; never drop a held slot silently
    }
    bool free_retiring() const noexcept { return n_old_ < kRetiring; }
    // F2: published_ms (on the producer's clock `clk`) must be set, not in the future, and at most kStaleMs old.
    static bool published_fresh(int64_t published, int64_t clk) noexcept { return published > 0 && published <= clk + kFutureSlackMs && clk - published <= kStaleMs; }
    void poll() noexcept {
        if (!n_old_) return;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; return; }   // held for ever: loss is never completion
        unsigned w = 0;
        for (unsigned i = 0; i < n_old_; ++i) {
            if (c.reached(old_[i].value)) { src_->release(old_[i].h); ++st_.released; }
            else old_[w++] = old_[i];
        }
        n_old_ = w;
    }
    std::string line(bool final_) {
        char t[acz::brief3d::kLineLimit];
        format_line(t, sizeof t, final_, now_, st_, (has_cur_ ? 1u : 0u) + n_old_, dead_, poisoned_, why_);
        return t;
    }

    std::shared_ptr<TableGpu> gpu_;
    std::shared_ptr<TableSource> src_;
    acz::brief3d::live::Held cur_;
    TableImages cur_img_;
    Old old_[kRetiring];
    unsigned n_old_ = 0;
    uint64_t cur_value_ = 0, value_ = 0, last_id_ = 0, alloc_value_[2] = {0, 0};
    unsigned next_alloc_ = 0;
    bool has_cur_ = false, setup_done_ = false, setup_ok_ = false, dead_ = false, poisoned_ = false, quarantined_ = false, destroyed_ = false;
    const char* why_ = "none";
    int64_t now_ = 0, last_summary_ms_ = 0;
    std::string pending_;
    Stats st_;
};

// The vscreen hook with the table on top: Settings::vscreen = Brief3dHook(inner = the ScreenCore hook). Every VScreenHook duty is the inner's;
// the table is added only to a frame in which the inner planned the screen. UI3D stage 3a: `pause` (null unless u3p:1) blends the pause menu
// into the flight eye images (Output calls pause_frame / pause_idle from its flight composite).
class Brief3dHook final : public VScreenHook {
public:
    // `latch` (brief3d.md 11, b3l:1): the briefing table re-drawn every XR frame; null = the old slot path only.
    Brief3dHook(std::shared_ptr<VScreenHook> inner, std::shared_ptr<TableCore> table, std::shared_ptr<PauseCore> pause = nullptr,
                std::shared_ptr<LatchCore> latch = nullptr)
        : inner_(std::move(inner)), table_(std::move(table)), pause_(std::move(pause)), latch_(std::move(latch)) {}
    bool frame(Gate& g, const xr_owner::FrameInfo& info, SwapchainRuntime& xr, unsigned target, const SwapchainImages& imgs, Extent ext,
               const VScreenIn& in, xr_owner::LayerPlan& plan) override {
        const bool shown = inner_->frame(g, info, xr, target, imgs, ext, in, plan);
        if (!shown || inner_->poisoned()) { table_->idle(g); if (latch_) latch_->idle(g); return shown; }
        // brief3d.md 11: a fresh latched scene is drawn for THIS frame's views; the slot path (UI3D menu slots) runs only when it was not. The
        // latch core's eye acquisitions overwrite whatever the slot path last released into the eye swapchains: its content is then gone.
        bool drawn = false;
        if (latch_) {
            drawn = latch_->frame(g, xr, in.eye_images[0], in.eye_images[1], in.eye_ext[0], in.eye_ext[1], info, plan);
            if (latch_->touched()) { table_->target_written(0); table_->target_written(1); }
        }
        if (drawn) table_->idle(g);
        else {
            table_->frame(g, xr, in.eye_images[0], in.eye_images[1], in.eye_ext[0], in.eye_ext[1], info.space_generation, plan);
            if (latch_ && table_->touched()) { latch_->target_written(0); latch_->target_written(1); }   // (F3) the slot path wrote the eye chains
        }
        return true;   // the screen is shown either way; a poisoned table is reported through poisoned() (Output fails the session)
    }
    bool exclusive() const noexcept override { return inner_->exclusive(); }
    bool retire(Gate& g) override {
        const bool p = pause_ ? pause_->retire(g) : true;
        const bool l = latch_ ? latch_->retire(g) : true;
        const bool t = table_->retire(g); const bool s = inner_->retire(g); return p && l && t && s;
    }
    std::string summary() override {
        std::string s = inner_->summary();
        if (s.empty()) s = table_->summary();
        if (latch_) if (std::string l = latch_->summary(); !l.empty()) s = s.empty() ? l : s + "\n" + l;
        if (pause_) if (std::string p = pause_->summary(); !p.empty()) s = s.empty() ? p : s + "\n" + p;
        return s;
    }
    bool poisoned() const noexcept override {
        return inner_->poisoned() || table_->poisoned() || (pause_ && pause_->poisoned()) || (latch_ && latch_->poisoned());
    }
    bool pause_frame(Gate& g, const SwapchainImages& i0, const SwapchainImages& i1, void* dst0, void* dst1, Extent e0, Extent e1,
                     uint32_t space_generation, float floor, const acz::brief3d::EyeView* eyes) override {
        return pause_ && pause_->frame(g, i0, i1, dst0, dst1, e0, e1, space_generation, floor, eyes);
    }
    void pause_idle(Gate& g) override { if (pause_) pause_->idle(g); }
    void target_written(unsigned t) noexcept override { inner_->target_written(t); table_->target_written(t); if (latch_) latch_->target_written(t); }
    // SCREEN-DUMP: the screen part is required; the table part is added while the table layer is live (its refusal is not a failure).
    const char* dump_arm(int64_t now_ms) override {
        if (const char* why = inner_->dump_arm(now_ms)) return why;
        table_why_ = latch_ && latch_->live() ? latch_->dump_arm(now_ms) : table_->dump_arm(now_ms);   // brief3d.md 11: the latched table when live
        return nullptr;
    }
    bool dump_take(Gate& g, DumpImage& out) override {
        return inner_->dump_take(g, out) || (latch_ && latch_->dump_take(g, out)) || table_->dump_take(g, out);
    }
    bool dump_busy() const noexcept override { return inner_->dump_busy() || table_->dump_busy() || (latch_ && latch_->dump_busy()); }
    const char* dump_table() const noexcept override { return table_why_ ? table_why_ : "armed"; }
private:
    std::shared_ptr<VScreenHook> inner_;
    std::shared_ptr<TableCore> table_;
    std::shared_ptr<PauseCore> pause_;
    std::shared_ptr<LatchCore> latch_;
    const char* table_why_ = nullptr;
};

// DLL only (xr_brief3d_d3d12.cpp): wraps `inner` when the producer is configured on; else returns `inner` unchanged.
std::shared_ptr<VScreenHook> make_brief3d_hook(std::shared_ptr<VScreenHook> inner, void* device, void* queue);

} // namespace acz::xr_stereo
