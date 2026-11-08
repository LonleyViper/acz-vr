#pragma once
// ui3d_pause.h -- UI3D stage 3a (docs/world-render/ui-3d.md section 7): the IN-FLIGHT PAUSE MENU as per-eye depth layers in front of the
// cockpit. SDK-free, header-only (the offline tests drive it): the consumer -> producer trigger, its freshness gate, the pause placement and the
// window rule. The producer frame kind lives in brief3d_live.h (Producer::ui3d_draw with Run::pause), the consumer blend in xr_brief3d_core.h
// (PauseCore), the GPU blend in xr_brief3d_d3d12.cpp / xr_brief3d.hlsl (mode 2). Default OFF (u3p:1 inside xr_terrain_stage, ui3d.h).
//
// Facts it relies on (ui-3d.md 7.1):
//  * [E] the flight HUD region's scene target IS the menu UI target: step124/runD1 step5-producer.log `event=hud-ring;...;format=10;ds-format=20;
//    size=2560x1440;scene=25613d6ffc0` and ee-camera.log `event=ui3d-census;kind=draw;...;rt=0x25613d6ffc0/v0/r10/2560x1440` (2430 lines).
//  * [E] the pause menu draws are the LAST draws of the HUD region on that target, after the radar group: step124/runA1 step5-producer.log
//    hud-end frame 3013 (pause, xr-stereo.log `hud-menu;state=enter` at 103311796): draws-seen=120, post-excursion-draws=65 vs 62 / 7 in flight;
//    the stencil-func string shows the flight HUD's 62 draws, then a 12-draw group with 6 stencil-EQUAL draws, then 46 ALWAYS draws.
//  * [I] they are ps2_v quads (VS 534f29ca, PS d34ee153, the HUD/menu key) with the flight cb0 (512, -448, 2048, 2048) (fx_sprite_replay.h:6);
//    their geometry is NOT logged yet: ui3d-census-tail.arm (ui3d_census.h) logs the last 48 menu-key draws of a paused frame on the next run.
//  * [I] the game dims the scene with a large translucent panel first (the game-window shot step14-live-run11-full acz-20261024-125357.png:
//    a dark translucent panel over the HUD): the window opens at the first ps2_v draw on the UI target with bbox area >= kOpenArea.
// Fail closed: no fresh trigger, no large opener, a refused draw inside the window, a non-ps2_v draw followed by more on-target menu draws, no
// placement -> no pause slot is published, and the consumer keeps today's hud-menu image (HUD quad at menud with the alpha floor).
#include "ui3d.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace acz::ui3d::pause {

// [E] the HUD/scene target spans the game lens: step124/runA1 step5-producer.log continuous-quality `lens-tan=0.61517,0.61517,0.34604,0.34604`.
inline constexpr double kLensTanX = 0.61517, kLensTanY = 0.34604;
inline constexpr double kScreenDistanceM = 2.0;    // the reference plane (hud-menu menud default); the layers themselves sit at u3n..u3f
inline constexpr int64_t kSignalStaleMs = 250;     // a consumer heartbeat older than this = no pause (the XR owner stopped or left menu mode)
inline constexpr int64_t kPlacementGapMs = 2000;   // no pause frame opened for this long: the next pause re-latches the placement
inline constexpr double kOpenArea = kLargeArea;    // the window opener: the first large ps2_v draw on the UI target (the dimming panel) [I]
inline constexpr double kMinHorizontal = 0.2;      // the head's forward must have this much horizontal length to give a yaw

// ------------------------------------------------------------------------------------------------ consumer -> producer trigger (lock-free)
// Output::hud_menu_frame publishes the HUD-MENU state once per FRESH game frame (XR owner thread). The producer reads it at every Present.
struct Signal { std::atomic<bool> on{false}; std::atomic<uint32_t> beat{0}; };
inline Signal& signal() noexcept { static Signal s; return s; }
inline void publish(bool on) noexcept {
    Signal& s = signal();
    s.on.store(on, std::memory_order_relaxed);
    s.beat.fetch_add(1, std::memory_order_release);
}

// Producer side (Present thread, under the producer lock): live = the last published state is ON and the heartbeat changed within
// kSignalStaleMs. A frozen heartbeat (the consumer stopped publishing) turns pause off by itself.
class Gate {
public:
    bool observe(bool on, uint32_t beat, int64_t now_ms) noexcept {
        if (!seen_ || beat != beat_) { beat_ = beat; changed_ms_ = now_ms; seen_ = true; }
        live_ = on && now_ms - changed_ms_ <= kSignalStaleMs && now_ms >= changed_ms_;
        return live_;
    }
    bool live() const noexcept { return live_; }
private:
    bool seen_ = false, live_ = false;
    uint32_t beat_ = 0;
    int64_t changed_ms_ = 0;
};

// ------------------------------------------------------------------------------------------------ placement
// The pause screen: the game lens' angular size (kLensTanX/Y) on a plane kScreenDistanceM ahead of the mid-eye point, turned to the head's yaw
// (level, like vscreen::place). From the anchor (the mid-eye point) every layer covers exactly the lens angles, as the HUD quad at scale 1 does in
// hud-menu mode. An invalid screen (bad pose, head pointing straight up/down) = no pause frame.
inline void rotate(const double q[4], const double v[3], double out[3]) noexcept {   // q = x, y, z, w
    const double t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
inline Screen place(const EyeView& e0, const EyeView& e1) noexcept {
    double anchor[3];
    for (int i = 0; i < 3; ++i) anchor[i] = 0.5 * (e0.p[i] + e1.p[i]);
    const double back[3] = {0, 0, -1};
    double f[3]; rotate(e0.q, back, f);
    const double hl = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (!std::isfinite(hl) || !(hl >= kMinHorizontal)) return Screen{};
    const double yaw = std::atan2(-f[0], -f[2]);   // make_screen's forward is (-sin yaw, 0, -cos yaw)
    const double D = kScreenDistanceM;
    const float centre[3] = {float(anchor[0] - std::sin(yaw) * D), float(anchor[1]), float(anchor[2] - std::cos(yaw) * D)};
    return make_screen(centre, float(yaw), float(2.0 * kLensTanX * D), float(2.0 * kLensTanY * D), anchor);
}

// ------------------------------------------------------------------------------------------------ the window rule (one game frame)
// Before the window opens every draw is the HUD / world / effects of the paused frame: never replayed, never a refusal, no UI3D hold.
// It opens at the first ps2_v draw on a usable target whose bbox (fractions of the shown image) has area >= kOpenArea.
inline bool opens_window(double bbox_w, double bbox_h) noexcept {
    return std::isfinite(bbox_w) && std::isfinite(bbox_h) && bbox_w > 0 && bbox_h > 0 && bbox_w * bbox_h >= kOpenArea;
}

// ------------------------------------------------------------------------------------------------ the consumer blend (xr_brief3d.hlsl mode 2)
// Root constant b0 of the table blit: bits 0..7 mode (0 premultiplied table, 1 opaque UI3D, 2 pause), bits 8..15 the pause alpha floor (0..255).
inline uint32_t blit_mode_bits(bool opaque, bool pause, float floor) noexcept {
    if (pause) {
        float f = std::isfinite(floor) ? floor : 0.0f;
        f = f < 0.0f ? 0.0f : f > 0.95f ? 0.95f : f;
        return 2u | (uint32_t(std::lround(f * 255.0f)) << 8);
    }
    return opaque ? 1u : 0u;
}
// The shader's pause pixel (CPU mirror for the tests): the slot holds the menu draws over transparent black with the game's alpha untouched
// (ZERO/ONE alpha blend [E census bl=...,1,2,1]), so coverage = max(floor, max(rgb)); written with blend ONE / INV_SRC_ALPHA:
// out = menu + dst x (1 - a). Text (bright) covers the cockpit, the dark panel dims it by the floor.
inline double blend_one(double menu, double dst, double a) noexcept { return menu + dst * (1.0 - a); }
inline double coverage(const double rgb[3], double floor) noexcept {
    double m = 0.0;
    for (int i = 0; i < 3; ++i) { const double c = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i]; if (c > m) m = c; }
    return m > floor ? m : floor;
}

} // namespace acz::ui3d::pause
