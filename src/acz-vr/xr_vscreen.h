#pragma once
// VSCREEN consumer hook (docs/world-render/vscreen.md section 2). The XR consumer (Output) calls it from its no-new-frame path (Output::pace):
// when no flight composite is young enough (vsenter) it shows the game's captured final image (vscreen_capture.h) as ONE quad layer in front
// of the user, world-locked in LOCAL and placed at the head yaw when the screen mode is entered (and after a LOCAL space change / vsfollow).
// The image goes into the existing HUD quad swapchain (target kHud: the HUD layer is not submitted outside flight) by a plain GPU copy
// (R8G8B8A8_UNORM capture -> R8G8B8A8(_SRGB) swapchain image) on the XR binding queue; while no new capture exists the layer is re-submitted
// with the swapchain's last released image (no acquire, no GPU work: a stalled game queue is never waited on).
// Settings::vscreen == null (the default and every offline test that does not set it): Output behaves exactly as before.
#include <functional>
#include <memory>
#include <string>

#include "brief3d.h"
#include "vscreen.h"
#include "xr_screen_dump.h"
#include "xr_stereo_output.h"

namespace acz::xr_stereo {

struct VScreenIn {
    int64_t flight_age_ms = -1;   // age of the last flight composite; -1 = none since the session started
    // BRIEF3D (xr_brief3d_core.h; additive): the eye swapchains (targets 0/1) for the briefing table layer; null = not created.
    const SwapchainImages* eye_images[2] = {nullptr, nullptr};
    Extent eye_ext[2] = {};
};
struct VScreenHook {
    virtual ~VScreenHook() = default;
    // Owner thread, Output::pace (an XR frame without a new game frame). true = `plan` holds the screen layer (screen_only) and nothing else.
    virtual bool frame(Gate&, const xr_owner::FrameInfo&, SwapchainRuntime&, unsigned target, const SwapchainImages&, Extent, const VScreenIn&,
                       xr_owner::LayerPlan&) = 0;
    // vspov:flat: the screen replaces the flight composite too (Output::transact does not compose producer frames).
    virtual bool exclusive() const noexcept = 0;
    // Owner thread, Output::retire BEFORE the swapchains are destroyed: true = no GPU work of the hook can still reference a swapchain image
    // (its private objects are released then); false = the caller retains (never destroys under in-flight work).
    virtual bool retire(Gate&) = 0;
    // Summary line(s) (Output::summary cadence), several joined by '\n' (Output emits each); empty = nothing to report.
    virtual std::string summary() = 0;
    // Astra review 6455891169 finding 1: the hook left an acquisition on the shared swapchain unsettled (a wait that never succeeded, a release
    // that failed) or GPU work whose completion can never be proven. Sticky. Output then fails for the session (no path acquires, writes or
    // submits that swapchain again) and its retire() retains everything.
    virtual bool poisoned() const noexcept = 0;
    // Finding 4: Output acquired `target` for its own content (the flight HUD): the swapchain's last released image is no longer the screen.
    // Called for kHud and (BRIEF3D) for the eye targets 0/1; a hook ignores the targets it does not write.
    virtual void target_written(unsigned target) noexcept = 0;
    // SCREEN-DUMP (xr_screen_dump.h): owner thread, while the screen is shown. Arms a one-shot readback of the next screen copy (BRIEF3D: and of
    // the next table blit's eye 0 while the table layer is live). nullptr = armed; else the refusal. Defaults: unsupported, nothing to take.
    virtual const char* dump_arm(int64_t now_ms) { (void)now_ms; return "unsupported"; }
    // Owner thread, every pace frame while a dump is busy: one finished (or given-up) part, taken once. Never waits on the GPU.
    virtual bool dump_take(Gate&, DumpImage&) { return false; }
    virtual bool dump_busy() const noexcept { return false; }
    // The BRIEF3D table part of the last dump_arm: "armed" or its refusal; "no-table" without the BRIEF3D decorator.
    virtual const char* dump_table() const noexcept { return "no-table"; }
    // UI3D stage 3a (u3p:1; docs/world-render/ui-3d.md 7; UNTESTED LIVE). Owner thread, inside Output's FLIGHT composite while HUD-MENU mode is on,
    // AFTER the composite's GPU completion was observed and BEFORE the eye images are released: blend the newest pause-menu slot into the two
    // acquired, waited eye swapchain images dst0/dst1 (RENDER_TARGET) of the chains i0/i1. true = submitted (Output then drops the HUD quad: the
    // menu is in the eye images). `eyes` = the 2 eyes (pose/FOV) the composite is submitted with (null = unknown): the slot is blended only when it was
    // rendered with the same eyes (fix round F1). Default: no pause layer (today's hud-menu behaviour).
    virtual bool pause_frame(Gate&, const SwapchainImages& i0, const SwapchainImages& i1, void* dst0, void* dst1, Extent e0, Extent e1,
                             uint32_t space_generation, float floor, const acz::brief3d::EyeView* eyes) {
        (void)i0; (void)i1; (void)dst0; (void)dst1; (void)e0; (void)e1; (void)space_generation; (void)floor; (void)eyes; return false;
    }
    // Every other flight composite (menu mode off): held pause slots whose blends completed are released.
    virtual void pause_idle(Gate&) {}
};
// DLL only (xr_vscreen_d3d12.cpp): device/queue = the XR binding (the game's device and DIRECT queue). null when `o.on` is false.
std::shared_ptr<VScreenHook> make_vscreen_hook(const acz::vscreen::Options& o, void* device, void* queue);

} // namespace acz::xr_stereo
