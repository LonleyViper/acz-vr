#pragma once
// fx_memory.h -- EFFECTS-OUTSIDE (B) particle memory core (docs/world-render/effects-outside-window.md). SDK-free, header + fx_memory.cpp.
//
// The game draws its sprite effects (PS 4caf6e2d = ps2AtestCA_p through the generic VS 534f29ca = ps2_v) as PRE-PROJECTED PS2 GS-screen quads, only
// for what is inside its 63x38 deg window. This module turns such quads back into WORLD positions and keeps them after they leave the window:
//  * unprojection (evidence: stored frame 11906, docs section 3): with the frame's world -> PS2-screen matrix A (DrawACM_v record +0, row vectors,
//    h = [P,1] A) a ps2_v vertex (x_s, y_s, z_s, w_in) of a world point P has x_s = h.x/h.w, y_s = h.y/h.w, and its view depth d = h.w is
//      d = Z0 / (z_s - b)    when z_s > 0      (A's z column = Z0 e3 + b A3: Z0 = 2^24, b = -255.0078; the depth the ACM objects test against), or
//      d = 1 / w_in          when z_s == 0 and 0 < w_in < 1 (perspective q; the cloud cards are written at z = 0 = the far plane).
//    P then solves P.A0 = x_s d, P.A1 = y_s d, P.A3 = d (3 linear equations).
//  * memory: sprites seen this frame are matched to remembered particles (nearest centre within a size-relative radius); a remembered particle that
//    is NOT seen although it lies inside the game's view died in the game and is dropped; one outside the game's view is kept and fades out over
//    `decay_s` after it was last seen (the game never tells us more: particles that never entered the window cannot be known).
//  * output: world-anchored billboards (centre, half extents, colour, uv rect) for any eye pose.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace acz::fxmem {

inline constexpr double kZ0 = 16777216.0;        // 2^24 (measured 16777470 from the stored A: float32 rounding of the 1.6e8 translation row)
inline constexpr double kDepthB = -255.0078;     // A col2 / col3 on rows 0..2 (stored frame: -255.00778 +- 1e-8)
inline constexpr double kGsCentre = 2048.0, kGsScaleX = 258.048, kGsScaleY = 225.792;
inline constexpr double kGsHalfW = 256.0, kGsHalfH = 224.0;   // the game window in GS units: x 2048 +- 256, y 2048 +- 224 (ps2_v constants 512, -448)

// The frame's world -> PS2-screen map, built from the 16 floats of A (row-major, rows x, y, z, w(translation)).
struct ScreenMap {
    double a[16] = {};
    double eye[3] = {};         // the camera position (h.x = h.y = h.w = 0)
    double fwd[3] = {};         // unit view direction (the w column)
    double z0 = kZ0, b = kDepthB;
    bool ok = false;
};
inline double game_z_for_depth(const ScreenMap& m, double d) noexcept { return m.z0 / d + m.b; }
// false + why when A is not a world -> screen map of the expected form (singular, depth column not Z0 e3 + b A3 within 1e-4 relative).
bool screen_map_from_a(const float a16[16], ScreenMap& out, const char*& why) noexcept;
bool screen_map_from_a(const double a16[16], ScreenMap& out, const char*& why) noexcept;
// h = [P,1] A ; returns false when the point is behind the camera (h.w <= 0).
bool project(const ScreenMap& m, const double p[3], double& xs, double& ys, double& depth) noexcept;
// Inverse of project for a known view depth.
bool unproject(const ScreenMap& m, double xs, double ys, double depth, double p[3]) noexcept;
enum DepthSource : uint8_t { depth_none = 0, depth_z = 1, depth_q = 2 };
double vertex_depth(const ScreenMap& m, float z, float w, DepthSource& src) noexcept;   // 0 = unknown
// The game z a point at view depth d gets (what the PS2-style projector writes; used by tests and the synthetic producer).
inline double game_z_for_depth(double d) noexcept { return kZ0 / d + kDepthB; }
// (HUD-only / chase, headset test 7) The depth model is PER VIEW: the cockpit camera has b = -255.01, Z0 = 2^24, but the chase / HUD-only world camera has
// b = -1,341 .. -1,475 and Z0 = 8.8e7 .. 9.7e7 (logged A of runPOV3: z0 / |b| stays 65,580 .. 65,790, the cockpit's 65,791). The map's own b / z0 are the truth.
// A depth model b, Z0 of the PS2 screen form: b < 0, Z0 > 0 and Z0 / |b| ~ 65,536 .. 65,800 (every logged world A: 65,579 .. 65,791). Anything else is not a world map.
inline bool depth_model_ok(double b, double z0) noexcept {
    if (!(b < 0.0) || !(z0 > 0.0)) return false;
    const double r = z0 / -b;
    return r >= 64000.0 && r <= 67000.0;
}
// true when (xs, ys) is inside the game window (GS units) shrunk by `margin` GS units.
inline bool in_window(double xs, double ys, double margin = 0.0) noexcept {
    return xs >= kGsCentre - kGsHalfW + margin && xs <= kGsCentre + kGsHalfW - margin && ys >= kGsCentre - kGsHalfH + margin && ys <= kGsCentre + kGsHalfH - margin;
}

// One ps2_v vertex as the game submits it (4 x float4, 64 bytes): position (x_s, y_s, z_s, q), COLOR0, TEXCOORD (uv in xy), COLOR1.
struct Vtx { float pos[4]; float col[4]; float uv[4]; float col1[4]; };
static_assert(sizeof(Vtx) == 64, "ps2_v vertex stride");

enum SpriteClass : uint8_t { class_sprite = 0, class_cloud = 1 };
struct Sprite {
    double centre[3] = {};
    double half_w = 0, half_h = 0;   // world half extents (along the quad's first and second edge)
    double depth = 0;                // view depth when seen
    float colour[4] = {};            // COLOR0 average (the game's vertex colour, PS2 scale: 0.5 = 1.0)
    float uv0[2] = {}, uv1[2] = {};  // uv bounding box
    uint8_t depth_src = depth_none;
    uint8_t cls = class_sprite;
};
struct ExtractStats { uint32_t quads = 0, sprites = 0, no_depth = 0, degenerate = 0, behind = 0, bad_index = 0; };
// Triangle-strip quads: `idx` = 16-bit indices with 0xFFFF restarts (indexed draw), or nullptr for a non-indexed strip of `nv` vertices. Every strip
// of exactly 4 vertices (the game's sprite quad: v0..v3 = two triangles) becomes one Sprite; longer strips are split into consecutive 4-vertex groups
// only when their count is a multiple of 4 (else counted degenerate). Appends to `out`; returns the number appended.
size_t extract_sprites(const ScreenMap& m, const Vtx* v, size_t nv, const uint16_t* idx, size_t ni, uint8_t cls, std::vector<Sprite>& out,
                       ExtractStats& st);

struct Params {
    double match_rel = 0.75;     // a seen sprite matches a remembered particle within match_rel * max(half extents) (+ match_abs)
    double match_abs = 2.0;      // world units
    double decay_s = 4.0;        // fade-out time after the particle was last seen (outside the game view)
    double max_age_s = 30.0;     // hard limit since first seen
    double view_margin = 8.0;    // GS units: a remembered particle this far inside the window edge counts as "inside the game view"
    unsigned cap = 512;          // remembered particles (oldest-last-seen dropped first)
};
struct Particle {
    uint32_t id = 0;
    Sprite s;
    double first_seen = 0, last_seen = 0;
    uint32_t hits = 0;
};
struct UpdateStats { uint32_t seen = 0, matched = 0, born = 0, died_in_view = 0, expired = 0, evicted = 0, kept_outside = 0; };

class Memory {
public:
    explicit Memory(const Params& p = Params{}) : p_(p) {}
    // One game frame: the sprites extracted this frame and the frame's screen map (to know which remembered particles the game could see).
    UpdateStats update(double t, const std::vector<Sprite>& seen, const ScreenMap& view);
    // Opacity factor of a particle at time t (1 while seen, linear fade to 0 over decay_s after the last sighting).
    double fade(const Particle& q, double t) const noexcept;
    const std::vector<Particle>& particles() const noexcept { return list_; }
    void clear() noexcept { list_.clear(); }
    const Params& params() const noexcept { return p_; }
private:
    Params p_;
    std::vector<Particle> list_;
    uint32_t next_id_ = 1;
};

// A world-anchored billboard facing `eye` (world units): corners c[0..3] = TL, TR, BL, BR (world 'up' = +Y kept vertical when possible).
void billboard(const Particle& q, const double eye[3], double c[4][3]) noexcept;

// ------------------------------------------------------------------ CPU reference renderer (tests / the future WARP harness)
// A symmetric-FOV pinhole eye: position, yaw/pitch (radians; yaw positive turns toward -X like the game heading: forward = (-sin y, 0, -cos y)
// for pitch 0), tan of the half FOV, square image of `size` pixels. Pixel (0,0) = top left.
struct Eye { double pos[3] = {}; double yaw = 0, pitch = 0; double tan_half = 1.0; int size = 256; };
bool eye_project(const Eye& e, const double p[3], double& px, double& py) noexcept;
// Rasterise every live particle (premultiplied over, colour = particle colour x fade) into rgba (size*size*4 floats).
void render_reference(const Memory& mem, double t, const Eye& e, std::vector<float>& rgba);

} // namespace acz::fxmem
