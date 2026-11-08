#pragma once
// hud_menu.h -- HUD-SIZE + PAUSE-MENU (D-362 headset test 19; docs/world-render/hud-menu.md). Pure, header-only, no D3D/OpenXR.
//
// 1. HUD scale about the gunsight glass (xr_hud_scale < 1 with a model cockpit whose HUD glass was found):
//    the HUD quad is world_quad() of a SCALED LENS whose four edge tangents are pulled toward the glass's angular centre
//    (tangent space, from the pvb eye): edge' = pivot + s * (edge - pivot). The pivot direction keeps its world direction
//    (so the symbology at the glass centre stays on the glass and on the world), every other HUD point moves toward it by
//    (1 - s) of its tangent offset. s == 1 returns the lens bit-identically (registration exactly as before).
// 2. Pause-menu mode: the game draws the pause menu INTO its HUD stream after the HUD (test 19 producer log: pause frames
//    have ~95-106 HUD-region draws vs 54-65 in flight, all replayed; the extra ~45 come after the radar group). Our HUD layer
//    alpha is max(src.a, max(rgb)) because the HUD target's alpha is not a coverage mask (it is 0 except the radar stencil
//    box, L1 s1 hud-alpha-raw.png), so the menu's dark translucent backdrop (rgb ~ 0) becomes TRANSPARENT: the bright
//    cockpit, world and the HUD (only dimmed in the HUD image) stay visible through and around the menu text, which reads as
//    "the menu is behind the HUD". Menu mode, entered while the game's camera is frozen (pause freezes the scene), gives the
//    HUD layer an alpha FLOOR (backdrop) so it darkens everything below it like the game's panel, and places the HUD quad
//    at the game's angular size (scale 1, centred on the lens) at a nearer reading distance. The HUD layer is already the
//    topmost layer (order: world/markers/projection/HUD), so the menu is in front of every other layer.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::hudmenu {

// ------------------------------------------------------------------------------------------- 1. scale about the glass
// lens / out: {left, right, up, down} tangent MAGNITUDES (the StereoFrame::lens_tan / world_quad convention).
// glass: the HUD glass's angular extent from the pvb eye, same convention (acz::cockpit::HudGlass tan_l/r/u/d).
// Pivot = the glass's angular bounding-box centre (tan x = (r - l)/2, tan y = (u - d)/2).
inline void glass_pivot(const float glass[4], float& px, float& py) noexcept {
    px = 0.5f * (glass[1] - glass[0]);
    py = 0.5f * (glass[2] - glass[3]);
}
// Returns false (out untouched) for a non-finite / non-positive scale or a non-finite input; s == 1 copies lens exactly.
inline bool scale_lens_about(const float lens[4], float s, float px, float py, float out[4]) noexcept {
    if (!lens || !out || !std::isfinite(s) || !(s > 0.0f) || !std::isfinite(px) || !std::isfinite(py)) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(lens[i])) return false;
    if (s == 1.0f) { for (int i = 0; i < 4; ++i) out[i] = lens[i]; return true; }
    const float xl = -lens[0], xr = lens[1], yu = lens[2], yd = -lens[3];   // signed edges
    out[0] = -(px + s * (xl - px));
    out[1] = px + s * (xr - px);
    out[2] = py + s * (yu - py);
    out[3] = -(py + s * (yd - py));
    return true;
}
// The glass-centre registration error of a world-referenced HUD point at tangent offset `t` from the pivot (degrees):
// the game draws it at atan(pivot + t), the scaled HUD at atan(pivot + s t). Documentation / test helper.
inline double registration_error_deg(double pivot, double t, double s) noexcept {
    return (std::atan(pivot + t) - std::atan(pivot + s * t)) * 57.29577951308232;
}

// ------------------------------------------------------------------------------------------- 2. pause-menu mode
// Cockpit option values (xr_cockpit_model / auto option string): menu:0|1, menud:<metres>, menub:<alpha>.
inline constexpr float kMenuDistanceDefault = 2.0f, kMenuDistanceMin = 0.5f, kMenuDistanceMax = 20.0f;
inline constexpr float kMenuBackdropDefault = 0.6f, kMenuBackdropMin = 0.0f, kMenuBackdropMax = 0.95f;
inline constexpr unsigned kMenuEnterFrames = 20;   // consecutive fresh game frames with a bit-identical camera AND the menu HUD signature (~1/3 s)
// HUD signature (step83 R1 false trigger: the aircraft parked on the runway in the takeoff sequence freezes the camera for 28 s with 51-54 HUD
// draws). Pause frames carry the menu draws in the HUD region: test19 93-106 and R1 94-102 HUD draws per frame vs 49-65 in flight / on the runway
// (step5-producer continuous-hud per 5 s; test19 hud-end: 48 draws after the radar group vs 4). Menu = hud_draws >= max(kMenuMinDraws,
// baseline + kMenuExtraDraws), baseline = running mean (1/64) of the frames that are NOT menu-like (so a pause never raises its own threshold).
inline constexpr uint32_t kMenuMinDraws = 80, kMenuExtraDraws = 25;
inline constexpr unsigned kMenuExitSignatureFrames = 3;   // menu signature gone this many frames -> exit (resume drops the menu draws at once)
inline constexpr unsigned kMenuExitMovingFrames = 10;     // camera moving this many frames -> exit even with the signature (never seen; guard)

// Pause-menu detector. Fed once per FRESH game frame (never a DECOUPLE re-composite) with the frame's game camera
// (StereoFrame::game_eye + game_cam_fwd when both valid) and its HUD-region draw count (StereoFrame::hud_draws).
// Enter: kMenuEnterFrames consecutive frames that are BOTH camera-frozen (bit-identical) and menu-signature. Exit: the signature is absent
// for kMenuExitSignatureFrames frames, the camera moves for kMenuExitMovingFrames frames, or the camera is invalid. A short camera
// change inside a pause (R1: exit/re-enter within 0.3 s at 96 draws) no longer flickers the menu.
class Detector {
public:
    bool update(bool cam_valid, const float eye[3], const float fwd[3], uint32_t hud_draws) noexcept {
        last_draws_ = hud_draws;
        if (!cam_valid || !eye || !fwd) { reset_streak(); return active_; }
        float cur[6];
        for (int i = 0; i < 3; ++i) { cur[i] = eye[i]; cur[3 + i] = fwd[i]; }
        bool finite = true;
        for (float v : cur) finite = finite && std::isfinite(v);
        if (!finite) { reset_streak(); return active_; }
        const bool frozen = have_ && std::memcmp(cur, last_, sizeof cur) == 0;
        std::memcpy(last_, cur, sizeof cur); have_ = true;
        const bool sig = signature(hud_draws);
        if (!sig) {   // baseline only from non-menu frames
            baseline_ = base_init_ ? baseline_ + (double(hud_draws) - baseline_) / 64.0 : double(hud_draws);
            base_init_ = true;
        }
        if (frozen && sig) { if (streak_ < 0xFFFFFFFFu) ++streak_; } else streak_ = 0;
        no_sig_ = sig ? 0u : no_sig_ + 1u;
        moving_ = frozen ? 0u : moving_ + 1u;
        if (active_) {
            if (no_sig_ >= kMenuExitSignatureFrames || moving_ >= kMenuExitMovingFrames) { active_ = false; ++exits_; streak_ = 0; }
        } else if (streak_ >= enter_) { active_ = true; ++enters_; }
        if (!active_ && frozen && !sig) ++frozen_rejected_;
        return active_;
    }
    // The draw threshold for this frame (before the frame's own baseline update).
    uint32_t threshold() const noexcept {
        const double b = base_init_ ? baseline_ + double(kMenuExtraDraws) : 0.0;
        return b > double(kMenuMinDraws) ? uint32_t(b + 0.5) : kMenuMinDraws;
    }
    bool signature(uint32_t hud_draws) const noexcept { return hud_draws >= threshold(); }
    bool active() const noexcept { return active_; }
    uint64_t enters() const noexcept { return enters_; }
    uint64_t exits() const noexcept { return exits_; }
    uint64_t frozen_rejected() const noexcept { return frozen_rejected_; }   // frozen-camera frames refused for lack of the HUD signature (runway)
    unsigned streak() const noexcept { return streak_; }
    double baseline() const noexcept { return baseline_; }
    void set_enter_frames(unsigned n) noexcept { enter_ = n < 2 ? 2 : n; }
private:
    void reset_streak() noexcept { have_ = false; streak_ = 0; if (active_) { active_ = false; ++exits_; } }
    float last_[6] = {};
    bool have_ = false, active_ = false, base_init_ = false;
    unsigned streak_ = 0, enter_ = kMenuEnterFrames, no_sig_ = 0, moving_ = 0;
    uint32_t last_draws_ = 0;
    double baseline_ = 0.0;
    uint64_t enters_ = 0, exits_ = 0, frozen_rejected_ = 0;
};

// Backdrop alpha floor -> HUD shader constant. mode_bits bits 24..31 (HUD target only; 0 = off, the shader's
// alpha is then exactly the old max(src.a, max(rgb))). Quantised to 1/255; NaN / <= 0 -> 0, clamped to kMenuBackdropMax.
inline uint32_t backdrop_bits(float a) noexcept {
    if (!(a > 0.0f)) return 0u;
    if (a > kMenuBackdropMax) a = kMenuBackdropMax;
    return (uint32_t(std::lround(double(a) * 255.0)) & 255u) << 24;
}
inline float backdrop_of_bits(uint32_t mode_bits) noexcept { return float((mode_bits >> 24) & 255u) / 255.0f; }
// ------------------------------------------------------------------------------------------- 3. radar background (RADAR-BG)
// docs/world-render/radar-background.md. The game draws the radar background as black with blend SRC_ALPHA/INV_SRC_ALPHA and alpha ops ZERO/ONE
// (the target alpha is untouched), then the stencil mask quads (REPLACE) write alpha = source alpha = 1 over the whole radar square
// (J-radar-analysis 1.1 b/c, 2.1.6). In the zero-cleared HUD target the square is therefore rgb 0, alpha exactly 1 (every archived
// hud-alpha-raw: a 399x412 rectangle of 255, nothing else non-zero), i.e. an OPAQUE black square, while in the game the background
// multiplies the scene by (1 - a_bg): measured from the scene-end images at the square's edges, scene inside / outside = 0.43..0.44
// (step83 run P b1..b3) -> a_bg ~ 0.56 (PS2 alpha 0x48/0x80 = 0.5625). Reconstruction, only where the stored alpha exceeds it:
//     a' = min(src.a, rb + (1 - rb) * max(rgb))      (rb = the background alpha; max(rgb) = the symbol drawn over it, "over" composition)
// The black background (rgb 0) gets rb; a lit symbol inside the square gets rb + (1 - rb) * its brightness (an opaque contact -> ~opaque);
// every texel whose stored alpha is already <= that value (every texel outside the square: alpha 0) is bit-for-bit the old rule.
// mode_bits bits 19..23 (HUD target only): q = round(rb * 32), 1..31 (0 = off: exactly the old alpha). 0.5625 = 18/32 exactly.
inline constexpr float kRadarBgDefault = 0.5625f, kRadarBgMin = 0.03125f, kRadarBgMax = 0.96875f;
inline constexpr uint32_t kRadarBgShift = 19u, kRadarBgMask = 31u << kRadarBgShift;
inline uint32_t radar_bg_bits(float a) noexcept {
    if (!(a > 0.0f)) return 0u;
    if (a > kRadarBgMax) a = kRadarBgMax;
    long q = std::lround(double(a) * 32.0);
    if (q < 1) q = 1;
    return (uint32_t(q) & 31u) << kRadarBgShift;
}
inline float radar_bg_of_bits(uint32_t mode_bits) noexcept { return float((mode_bits >> kRadarBgShift) & 31u) / 32.0f; }

// CPU mirror of the shader's HUD alpha (xr_stereo_blit.hlsl mode 1): m = max(rgb); sa = max(src.a, 0), capped to rb + (1 - rb) * m when the
// RADAR-BG bits are set; a = saturate(max(sa, m, backdrop)). rgb is the shaded (exposure/tonemap) colour; the output stays premultiplied
// (rgb <= a) because neither the floor nor the cap ever lowers a below m.
inline float hud_alpha(float src_a, float r, float g, float b, uint32_t mode_bits) noexcept {
    float a = src_a > 0.0f ? src_a : 0.0f;   // HLSL max(a, 0): NaN -> 0
    const float m = std::fmax(r, std::fmax(g, b));
    if ((mode_bits & kRadarBgMask) != 0u) {
        const float rb = radar_bg_of_bits(mode_bits);
        a = std::fmin(a, rb + (1.0f - rb) * m);
    }
    a = std::fmax(a, m);
    a = std::fmax(a, backdrop_of_bits(mode_bits));
    return a < 1.0f ? a : 1.0f;
}

}  // namespace acz::hudmenu
