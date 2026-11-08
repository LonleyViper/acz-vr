#pragma once
// VSCREEN consumer core (xr_vscreen.h; docs/world-render/vscreen.md section 2 and 7). Header-only and SDK-free so the offline tests drive the
// real decision code with a fake GPU, fake capture source, fake swapchain runtime and fake Gate (tests/acz-vr/vscreen_offline_tests.cpp,
// xr_stereo_output_offline_tests.cpp). The D3D12 parts are behind ScreenGpu (xr_vscreen_d3d12.cpp).
//
// Ownership rules (Astra review 6455891169):
//  * Finding 1: the core acquires the shared HUD swapchain only inside one frame() call and settles it there (wait succeeded -> release).
//    A wait that never succeeds or a failed release POISONS the hook (sticky); Output checks poisoned() right after the call and fails for
//    the session, so neither path acquires, writes or submits that swapchain again; retire() then refuses (the attempt is retained).
//  * Finding 2: completion is a Completion (value or LOSS). Loss never releases a held capture slot, never resets an allocator, never counts
//    as retirement; an Execute without a Signal keeps the slot held for ever (quarantined, explicit flag) and poisons the hook.
//  * Finding 4: the screen layer is submitted only while the swapchain's last released image is KNOWN to be a screen copy: set by a released
//    successful copy, cleared by any release without a completed copy record, by Output writing the swapchain (target_written), by poison.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "vscreen.h"
#include "vscreen_capture.h"
#include "xr_screen_dump.h"
#include "xr_vscreen.h"

namespace acz::xr_stereo {

// The GPU side of one copy (DLL: D3D12 on the XR binding queue). Owner thread only.
struct ScreenGpu {
    virtual ~ScreenGpu() = default;
    virtual bool setup(const SwapchainImages&, const char*& why) = 0;   // once; false = never usable
    virtual acz::vscreen::Completion completed() = 0;                    // the private fence (loss-aware)
    struct Submit { bool executed = false, signalled = false; uint64_t value = 0; bool readback = false; };
    // Records and executes the copy of `src` into swapchain image `dst` with allocator `alloc` (0/1) and signals the private fence.
    // SCREEN-DUMP: while readback_want(true), the copy also copies `dst` (after the write) into a private readback buffer in the same list
    // (Submit::readback = recorded into the executed list).
    virtual Submit copy(unsigned alloc, void* dst, const acz::vscreen::CaptureImage& src, Extent ext) = 0;
    virtual void readback_want(bool) {}
    // After the readback's fence value completed: RGBA8 rows top-down (as stored in the swapchain image), w x h.
    virtual bool readback_take(std::vector<uint8_t>&, uint32_t&, uint32_t&) { return false; }
    // After a given-up readback's fence value was PROVEN complete (never on timeout/loss alone): drop it so a fresh dump can be recorded.
    virtual void readback_discard() {}
    virtual void destroy() = 0;   // every private object setup() created, also after a PARTIAL setup; only after every submission completed
};
// The capture (DLL: vscreen_capture.h functions).
struct ScreenSource {
    virtual ~ScreenSource() = default;
    virtual void request() = 0;
    virtual void set_extent(uint32_t w, uint32_t h) = 0;
    virtual bool acquire(uint64_t after, acz::vscreen::CaptureImage&) = 0;
    virtual void release(const acz::vscreen::CaptureImage&) = 0;
    virtual std::string describe() = 0;
};

class ScreenCore final : public VScreenHook {
public:
    ScreenCore(const acz::vscreen::Options& o, std::shared_ptr<ScreenGpu> gpu, std::shared_ptr<ScreenSource> src)
        : o_(o), mm_(o), gpu_(std::move(gpu)), src_(std::move(src)) {}
    ~ScreenCore() override = default;   // no foreign call

    bool exclusive() const noexcept override { return o_.pov == acz::vscreen::Pov::flat; }
    bool poisoned() const noexcept override { return poisoned_; }
    void target_written(unsigned t) noexcept override { if (t != kHud) return; content_ok_ = false; ++st_.invalidated; }   // BRIEF3D: eye targets are not ours
    // SCREEN-DUMP (xr_screen_dump.h): the next copy also reads back the HUD swapchain image it wrote (the image the screen layer submits).
    const char* dump_arm(int64_t now_ms) override {
        if (poisoned_ || dead_) return "screen-dead";
        if (!content_ok_) return "no-screen-image";
        if (dump_.busy()) return "busy";
        dump_.arm(now_ms);
        return nullptr;
    }
    bool dump_take(Gate& g, DumpImage& out) override {
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
        return dump_.poll(now_, c, "screen", true, out, [&](std::vector<uint8_t>& v, uint32_t& w, uint32_t& h) {
            bool ok = false;
            (void)g.call(Op::d3d_sample, [&] { ok = gpu_->readback_take(v, w, h); return Rc::ok; });
            return ok;
        });
    }
    bool dump_busy() const noexcept override { return dump_.busy(); }

    bool frame(Gate& g, const xr_owner::FrameInfo& info, SwapchainRuntime& xr, unsigned target, const SwapchainImages& imgs, Extent ext,
               const VScreenIn& in, xr_owner::LayerPlan& plan) override {
        now_ = g.now_ms();
        if (!logged_) {
            logged_ = true; src_->set_extent(ext.w, ext.h);
            pending_ = "xr-stereo;event=vscreen-config;" + acz::vscreen::describe(o_) + ";target=hud-swapchain;extent=" + std::to_string(ext.w) + "x" +
                std::to_string(ext.h) + ";layer=quad(opaque,LOCAL,screen-only)";
        }
        if (poisoned_) return false;
        acz::vscreen::ModeIn mi; mi.now_ms = now_; mi.flight_age_ms = in.flight_age_ms; mi.screen_image = content_ok_;
        const acz::vscreen::ModeOut m = mm_.step(mi);
        if (m.want_capture && !dead_) src_->request();
        poll_release();
        if (m.mode != acz::vscreen::Mode::screen) return false;
        ++st_.screen_frames;
        if (m.entered) ++st_.entries;
        const bool views = info.views_valid && info.views.view_count >= 2;
        float head[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
        if (views) {
            for (int i = 0; i < 3; ++i) head[i] = 0.5f * (info.views.eyes[0].pose.position[i] + info.views.eyes[1].pose.position[i]);
            for (int i = 0; i < 4; ++i) q[i] = info.views.eyes[0].pose.orientation[i];
        }
        const float aspect = ext.h ? float(ext.w) / float(ext.h) : 16.0f / 9.0f;
        bool replace = m.entered || !pl_.valid || pl_.generation != info.space_generation;
        if (!replace && views) replace = follow_.step(o_.follow, acz::vscreen::head_yaw(q), pl_.yaw, now_);
        if (replace && views) {
            const acz::vscreen::Placement p = acz::vscreen::place(o_, head, q, aspect, info.space_generation);
            if (p.valid) { pl_ = p; ++st_.placements; acz::vscreen::publish_placement(p, head, encode_space_generation(info.space_generation)); }   // UI3D
        }
        if (!pl_.valid) { ++st_.no_pose; return false; }
        if (!dead_ && !holding_) {
            acz::vscreen::CaptureImage ci;
            if (src_->acquire(last_id_, ci)) {
                last_id_ = ci.held.id;
                copy(g, xr, target, imgs, ext, ci);   // takes ownership of `ci` (held / released / quarantined)
                if (poisoned_) return false;
            }
        }
        if (!content_ok_) { ++st_.no_image; return false; }   // the swapchain's last released image is not (known to be) the screen
        plan.submit = true;
        plan.layer = xr_owner::ProjectionLayer{};
        plan.layer.screen_only = true;
        for (int i = 0; i < 3; ++i) plan.layer.screen_pose.position[i] = pl_.pose.p[i];
        for (int i = 0; i < 4; ++i) plan.layer.screen_pose.orientation[i] = pl_.pose.q[i];
        plan.layer.screen_width = pl_.width;
        plan.layer.screen_height = pl_.height;
        ++st_.layers;
        return true;
    }

    bool retire(Gate& g) override {
        if (poisoned_ || quarantined_) { pending_ = summary_line(true); return false; }   // the caller retains (never destroyed under use)
        if (value_) {
            const int64_t until = g.now_ms() + 1000;
            acz::vscreen::Completion c = gpu_->completed();
            while (!c.lost && !c.reached(value_) && g.now_ms() < until && !g.lost()) { g.sleep_ms(5); c = gpu_->completed(); }
            if (!c.reached(value_)) { why_ = c.lost ? "retire-fence-lost" : "retire-timeout"; pending_ = summary_line(true); return false; }
        }
        if (holding_) { src_->release(held_); holding_ = false; }
        if (setup_done_ && !destroyed_) gpu_->destroy();   // Astra follow-up P3: also a PARTIAL setup (no submission ever happened then: value_ == 0)
        setup_ok_ = false; setup_done_ = true; dead_ = true; content_ok_ = false; destroyed_ = true;
        pending_ = summary_line(true);
        return true;
    }

    std::string summary() override {
        if (!pending_.empty()) { std::string t; t.swap(pending_); return t; }
        if (now_ - last_summary_ms_ < 5000) return {};
        last_summary_ms_ = now_;
        return summary_line(false);
    }

    struct Stats {
        uint64_t screen_frames = 0, layers = 0, entries = 0, placements = 0, copies = 0, copy_failed = 0, alloc_busy = 0, no_image = 0,
                 no_pose = 0, invalidated = 0, lost = 0;
    };
    const Stats& stats() const noexcept { return st_; }
    bool content_ok() const noexcept { return content_ok_; }
    bool holding() const noexcept { return holding_; }
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
    void copy(Gate& g, SwapchainRuntime& xr, unsigned target, const SwapchainImages& imgs, Extent ext, const acz::vscreen::CaptureImage& ci) {
        if (!setup_done_) {
            setup_done_ = true;
            const char* why = "setup";
            setup_ok_ = gpu_->setup(imgs, why);
            if (!setup_ok_) why_ = why;
        }
        if (!setup_ok_) { dead_ = true; src_->release(ci); ++st_.copy_failed; return; }
        const unsigned k = next_alloc_;
        const acz::vscreen::Completion c = gpu_->completed();
        if (c.lost) { ++st_.lost; dead_ = true; why_ = "fence-lost"; src_->release(ci); return; }   // nothing of ours reads `ci`
        if (!c.reached(alloc_value_[k])) { ++st_.alloc_busy; src_->release(ci); return; }
        uint32_t index = 0;
        Rc rc = g.call(Op::acquire_image, [&] { return xr.acquire(target, index); });
        if (!succeeded(rc)) { src_->release(ci); dead_ = true; why_ = "acquire"; ++st_.copy_failed; return; }   // nothing acquired
        content_ok_ = false;   // from here the swapchain's next released image is unknown until the copy is proven recorded
        if (index >= imgs.count) { src_->release(ci); poison("acquire-index"); return; }
        bool timed_out = false;
        rc = g.call(Op::wait_image, [&] { return xr.wait(target, 0, timed_out); });
        if (succeeded(rc) && timed_out) rc = g.call(Op::wait_image, [&] { return xr.wait(target, 100000000, timed_out); });   // 100 ms
        if (!succeeded(rc) || timed_out) { src_->release(ci); poison("wait-image"); return; }   // never written, never released: sticky
        // Astra follow-up P2: the GPU work runs ONLY through the owner's guarded call (deadline admission + call record). A refused call never
        // invokes the callback: `s` stays "not executed" (no reset/record/execute happened) and is handled exactly like a record failure.
        ScreenGpu::Submit s;
        const bool want = dump_.want();
        gpu_->readback_want(want);
        (void)g.call(Op::d3d_execute, [&] { s = gpu_->copy(k, imgs.images[index], ci, ext); return s.signalled ? Rc::ok : Rc::error; });
        if (want && s.executed && s.signalled) { if (s.readback) dump_.submitted(true, s.value, now_); else dump_.refuse("readback-setup"); }
        const Rc rr = g.call(Op::release_image, [&] { return xr.release(target); });
        if (s.executed && !s.signalled) {   // the copy may run at any time: the slot stays held for ever, nothing is ever proven complete
            held_ = ci; holding_ = true; quarantined_ = true;
            poison("signal");
            return;
        }
        if (!s.executed) {   // nothing read `ci`; the released image keeps its previous, unknown content
            src_->release(ci); ++st_.copy_failed; why_ = "record";
            if (!succeeded(rr)) poison("release-image");
            return;
        }
        held_ = ci; holding_ = true; held_value_ = s.value; value_ = s.value; alloc_value_[k] = s.value; next_alloc_ = k ^ 1u;
        ++st_.copies;
        if (!succeeded(rr)) { poison("release-image"); return; }
        content_ok_ = true;
    }
    std::string summary_line(bool final_) {
        char t[760];
        std::snprintf(t, sizeof t, "xr-stereo;event=%s;t-ms=%lld;mode=%s;transitions=%llu;screen-frames=%llu;layers=%llu;entries=%llu;placements=%llu;copies=%llu;"
            "copy-failed=%llu;alloc-busy=%llu;no-image=%llu;no-pose=%llu;invalidated=%llu;lost=%llu;content=%u;dead=%u;poisoned=%u;quarantined=%u;why=%s;"
            "screen=%.2fx%.2fm@%.2f,%.2f,%.2f;yaw-deg=%.1f;",
            final_ ? "vscreen-result" : "summary-vscreen", (long long)now_, acz::vscreen::mode_name(mm_.mode()), (unsigned long long)mm_.transitions(),
            (unsigned long long)st_.screen_frames, (unsigned long long)st_.layers, (unsigned long long)st_.entries, (unsigned long long)st_.placements,
            (unsigned long long)st_.copies, (unsigned long long)st_.copy_failed, (unsigned long long)st_.alloc_busy, (unsigned long long)st_.no_image,
            (unsigned long long)st_.no_pose, (unsigned long long)st_.invalidated, (unsigned long long)st_.lost, content_ok_ ? 1u : 0u, dead_ ? 1u : 0u,
            poisoned_ ? 1u : 0u, quarantined_ ? 1u : 0u, why_, double(pl_.width), double(pl_.height), double(pl_.pose.p[0]), double(pl_.pose.p[1]),
            double(pl_.pose.p[2]), double(pl_.yaw) * 57.29577951308232);
        return std::string(t) + src_->describe();
    }

    acz::vscreen::Options o_;
    acz::vscreen::ModeMachine mm_;
    acz::vscreen::Follow follow_;
    acz::vscreen::Placement pl_;
    std::shared_ptr<ScreenGpu> gpu_;
    std::shared_ptr<ScreenSource> src_;
    uint64_t alloc_value_[2] = {};
    unsigned next_alloc_ = 0;
    uint64_t value_ = 0, held_value_ = 0, last_id_ = 0;
    acz::vscreen::CaptureImage held_;
    bool holding_ = false, content_ok_ = false, setup_done_ = false, setup_ok_ = false, dead_ = false, poisoned_ = false, quarantined_ = false,
         logged_ = false, destroyed_ = false;
    const char* why_ = "none";
    int64_t now_ = 0, last_summary_ms_ = 0;
    std::string pending_;
    Stats st_;
    DumpTrack dump_;   // SCREEN-DUMP
};

} // namespace acz::xr_stereo
