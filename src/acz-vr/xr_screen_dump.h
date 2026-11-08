#pragma once
// SCREEN-DUMP (docs/world-render/vscreen.md section 9): one PNG of what the screen layer submits to the headset, on a trigger file
// (%LOCALAPPDATA%\acz-vr\png-screen.now; Output polls it at most once per second, only while the screen is shown, and deletes it on the attempt).
// The flight dump (xr_png_once) needs fresh game composites, which screen mode never has. Header-only and SDK-free: the decisions are tested
// offline (vscreen_offline_tests.cpp, brief3d_table_offline_tests.cpp, xr_stereo_output_offline_tests.cpp) with fakes.
//
// Per part (the screen quad image; BRIEF3D: the table's eye-0 image) a DumpTrack goes idle -> armed -> pending -> taken:
//  * armed: the NEXT copy/blit of the core also copies the swapchain image it just wrote (the exact image released and submitted) into a private
//    readback buffer in the same command list (no extra submission, no wait); armed for at most kArmMs (no new copy: given up);
//  * pending: the readback completes with that copy's own private fence value; the owner only polls it (never waits) for at most kPendingMs;
//    a lost fence or a timeout gives the part up, but the readback is then DRAINING (its fence value kept, the buffer never released while the
//    GPU may still write it): busy() stays true until the fence is proven complete and the core discarded the readback (reclaim), then a fresh
//    arm works again; a lost fence never proves completion (the buffer stays with the GPU object until destroy(), after every submission completed);
//  * taken: mapped once and handed to Output so that xr_dump_upright.py (rotate 180 degrees) gives the upright view for EVERY XR dump:
//    - an EYE image (projection layer: the table's eye 0) is written AS STORED, exactly like xr_png_once's eye dumps: it is rendered at the
//      located eye pose, and in the live runs that raw image is rotated 180 degrees against the view (D-298, J-hud-colour-findings C; Q1a: the
//      table's far edge at 0.32 H where brief3d.h's placement puts it at 0.68 H for b3y=-0.45, b3d=0.75, b3w=1, FOV +-45: tan = 0.45/1.25 = 0.36);
//    - the SCREEN image (quad layer: a copy of the game's upright back buffer, no pose) is rotated 180 degrees here, so the tool un-rotates it.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "vscreen.h"

namespace acz::xr_stereo {

struct DumpImage {
    std::string part;                 // "screen" / "eye0-table"
    std::vector<uint8_t> rgba;        // w*h*4: the screen part rotated 180 degrees, an eye part as stored (both upright after xr_dump_upright.py)
    uint32_t w = 0, h = 0;
    bool ok = false;                  // false: the part was given up (why)
    const char* why = "none";
};

// In place: RGBA8 rotated 180 degrees (= the pixel order reversed).
inline void rotate180(std::vector<uint8_t>& rgba) noexcept {
    const size_t n = rgba.size() / 4;
    for (size_t i = 0, j = n ? n - 1 : 0; i < j; ++i, --j)
        for (unsigned c = 0; c < 4; ++c) std::swap(rgba[i * 4 + c], rgba[j * 4 + c]);
}
// The premultiplied projection layer composited over black: the colour bytes stay, alpha becomes opaque.
inline void premultiplied_over_black(std::vector<uint8_t>& rgba) noexcept { for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255; }

struct DumpTrack {
    static constexpr int64_t kArmMs = 2000, kPendingMs = 2000;
    enum State : uint8_t { idle, armed, pending };
    State state = idle;
    int64_t since = 0;
    uint64_t value = 0;
    const char* why = "none";
    bool draining = false;     // a given-up readback whose GPU write is not yet proven complete (drain_value = its fence value)
    uint64_t drain_value = 0;

    bool busy() const noexcept { return state != idle || draining; }
    // Owner thread: true = the drained readback's fence is proven complete: the core discards it, then calls drained().
    bool reclaimable(const acz::vscreen::Completion& c) const noexcept { return draining && c.reached(drain_value); }
    void drained() noexcept { draining = false; drain_value = 0; }
    bool want() const noexcept { return state == armed; }
    void arm(int64_t now) noexcept { state = armed; since = now; value = 0; why = "none"; }
    // The core's copy/blit outcome while armed: `readback` = the readback was recorded into the executed and signalled list (fence value `v`).
    void submitted(bool readback, uint64_t v, int64_t now) noexcept {
        if (state != armed) return;
        if (readback) { state = pending; value = v; since = now; }
    }
    // A failure that makes the armed part impossible (setup refused, readback buffer refused): given up at the next take().
    void refuse(const char* w) noexcept { if (state == armed) { why = w; state = pending; value = 0; since = -1; } }
    // Owner thread, every frame. true = `out` holds this part's result (taken once). `take(rgba, w, h)` maps the completed readback.
    template <class Take>
    // rot180: the screen part (quad image) true; an eye part (projection layer image) false.
    bool poll(int64_t now, const acz::vscreen::Completion& c, const char* part, bool rot180, DumpImage& out, Take&& take) {
        if (state == idle) return false;
        auto give_up = [&](const char* w) { out = DumpImage{}; out.part = part; out.why = w; state = idle; return true; };
        if (state == armed) return now - since > kArmMs ? give_up("no-new-copy") : false;
        if (since < 0) return give_up(why);                    // refused while armed
        auto drain_up = [&](const char* w) { draining = true; drain_value = value; return give_up(w); };   // recorded readback: the GPU may still write it
        if (c.lost) return drain_up("fence-lost");
        if (!c.reached(value)) return now - since > kPendingMs ? drain_up("readback-timeout") : false;
        out = DumpImage{}; out.part = part;
        uint32_t w = 0, h = 0;
        if (!take(out.rgba, w, h) || !w || !h || out.rgba.size() != size_t(w) * h * 4) { out.rgba.clear(); return give_up("map"); }
        out.w = w; out.h = h; out.ok = true;
        if (rot180) rotate180(out.rgba);
        state = idle;
        return true;
    }
};

} // namespace acz::xr_stereo
