#pragma once
// brief3d.h -- BRIEF3D (roadmap R4, D-362 (b)): the mission briefing's 3D map as a world-locked TABLETOP in VR, per-eye re-projection of the
// game's OWN DrawSelMap_v / DrawModelIcon_v draws (docs/world-render/brief3d.md; research: briefing-3d-map.md). SDK-free, header only.
//
// Facts this core is built on [E]:
//  * Root signatures (RTS0 parts of the live shader dumps D:\acz-evidence-archive\step92\runR4-logs\shaders, decoded offline; flags 0x33d =
//    IA input layout + DENY hull/domain/geometry/pixel/amplification/mesh root access):
//      DrawSelMap_v  384ade02 (and its PS 9ebdf113): p0 = root SRV t10 (VERTEX), p1 = root SRV t11 (VERTEX), p2 = root constants b10 x2 (VERTEX)
//      DrawModelIcon_v d5a41c2f:                     p0 = root SRV t10 (VERTEX), p1 = root constants b10 x2 (VERTEX)
//    => t10 (the SrtData with the matrix) is ONE root SRV parameter: its VA is element 0, and a per-eye replay only rebinds that parameter, exactly as
//    the fxmem:4 smoke replay rebinds the smoke GS's t10 (fx_smoke.h plan_root_swap / replay_commands). The vertex/index buffers, t11 (colour table),
//    b10 (screen divisor) and the PSO stay the game's: nothing of them needs to be readable by the CPU for the replay.
//  * Live run R4 (D-365, step92 ee-camera.log briefing-observe lines): per 5 s window (300 presents) 300 SelMap draws = ONE per frame, 65,024 indices
//    each (= 2 x 32,512 = 2 x 2 x 128 x 127: the indexed LINE LIST of a 128 x 128 vertex grid), units constant across POVs (no culling);
//    ModelIcon 18,900 draws = 63 per frame, <= 65 indices (triangle-type topology; an odd count means strips or a non-multiple index range).
//  * DrawSelMap_v (dxc -dumpbin, briefing-3d-map.md 2.1): p = [x y z 1] M (row vectors, M = t10 element 0, 64 B), q = p / p.w,
//    SV_Position = ((q.x - 2048) * 2 / cb10.x, (q.y - 2048) * 2 / cb10.y, q.z * 2^-24, 1). The shader divides by w ITSELF: the rasteriser never sees a
//    homogeneous w, so a vertex BEHIND the eye is mirrored, not clipped (=> eye_clear_of_volume below refuses an eye inside / too close to the table).
//    Screen-space linear interpolation of near/w is exact, so the depth of the per-eye lines is correct.
//
// This header: the options (sub-options of xr_terrain_stage), the draw identity + root layout gate, the PS2-form eye projection, the table placement,
// the model-space fit (grid bounding box + the game camera's centre for the up axis and the handedness), the SelMap / ModelIcon record substitution,
// the root-swap plan, the per-draw command sequence, the per-slot upload layout and memory budget, the per-frame state and the log lines. The D3D12
// recording, the menu-time producer ring and the consumer layer are NOT here (brief3d.md section 6: live wiring plan).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace acz::brief3d {

// ================================================================================================== identity
inline constexpr uint32_t kSelMapVs8 = 0x384ade02u, kModelIconVs8 = 0xd5a41c2fu, kMapPs8 = 0x9ebdf113u;   // = acz::brobs names
inline constexpr unsigned kSelMapSrtBytes = 64;      // one float4x4
inline constexpr unsigned kModelIconSrtBytes = 272;  // four float4x4 + one float4
inline constexpr double kPs2Centre = 2048.0;
inline constexpr double kDepthUnit = 1.0 / 16777216.0;   // SV_Position.z = q.z * 2^-24
inline constexpr unsigned kTopoLine = 2, kTopoTriangle = 3;   // D3D12_PRIMITIVE_TOPOLOGY_TYPE of the PSO

enum class Kind : uint8_t { none, selmap, modelicon };
inline const char* kind_name(Kind k) noexcept { return k == Kind::selmap ? "selmap" : k == Kind::modelicon ? "modelicon" : "none"; }
// The draw's PSO creation hashes (first 8 hex digits, as PipelineRecord::brVs8/brPs8), its PSO topology type and whether it is indexed. Anything else
// is none (never replayed). Both live keys were indexed (R4: `t2i`, `t3i`).
inline Kind classify(uint32_t vs8, uint32_t ps8, unsigned topo_type, bool indexed) noexcept {
    if (!indexed) return Kind::none;
    if (vs8 == kSelMapVs8 && ps8 == kMapPs8 && topo_type == kTopoLine) return Kind::selmap;
    // D-384 (user finding: no aerial icons after the zoom click): the log of that session shows every classified icon replayed (refused did not move)
    // while the game's classified icon draws fell 63 -> ~20 per frame, so the missing icons are draws this function dropped silently. The icon
    // VERTEX shader is the replay-relevant identity (t10 + b10 root layout, checked per draw by root_layout_check, DENY_PIXEL root access); a
    // PSO that pairs it with another pixel shader (a per-class variant) is replayed like the 9ebdf113 one and counted (`icons-other-ps`).
    if (vs8 == kModelIconVs8 && ps8 != 0 && topo_type == kTopoTriangle) return Kind::modelicon;
    return Kind::none;
}
inline unsigned srt_bytes(Kind k) noexcept { return k == Kind::selmap ? kSelMapSrtBytes : k == Kind::modelicon ? kModelIconSrtBytes : 0; }

// ---- root layout (acz::metadata numbering: kind 0 table, 1 constants, 2 CBV, 3 SRV, 4 UAV; visibility 0 all, 1 vertex)
struct RootParam { uint32_t kind = 0, visibility = 0, reg = 0, space = 0, constants = 0; };
inline constexpr uint32_t kParamTable = 0, kParamConstants = 1, kParamSrv = 3, kVisAll = 0, kVisVertex = 1;
inline constexpr uint32_t kDenyVs = 2;   // D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS
struct RootPlan { uint32_t t10 = UINT32_MAX, t11 = UINT32_MAX, b10 = UINT32_MAX; };
// nullptr = the bound root signature is the one the dumps show for `k` (t10 root SRV, b10 two root constants, SelMap also t11 root SRV, all visible to
// the vertex stage, no other parameter naming t10/b10), plan filled. A different layout (a table, a root CBV, a missing parameter) is refused.
inline const char* root_layout_check(Kind k, const RootParam* p, unsigned n, uint32_t flags, RootPlan& out) noexcept {
    out = RootPlan{};
    if (k == Kind::none) return "brief3d-kind";
    if (!p || n == 0 || n > 64) return "brief3d-root-params";
    if (flags & kDenyVs) return "brief3d-root-vs-denied";
    for (unsigned i = 0; i < n; ++i) {
        const RootParam& q = p[i];
        if (!(q.visibility == kVisAll || q.visibility == kVisVertex) || q.space != 0) continue;
        if (q.kind == kParamTable) return "brief3d-root-table";   // the game's map roots have none; a table could also name t10/b10
        if (q.kind == kParamSrv && q.reg == 10) { if (out.t10 != UINT32_MAX) return "brief3d-root-t10-ambiguous"; out.t10 = i; }
        else if (q.kind == kParamSrv && q.reg == 11) { if (out.t11 != UINT32_MAX) return "brief3d-root-t11-ambiguous"; out.t11 = i; }
        else if (q.kind == kParamConstants && q.reg == 10) {
            if (out.b10 != UINT32_MAX) return "brief3d-root-b10-ambiguous";
            if (q.constants < 2) return "brief3d-root-b10-short";
            out.b10 = i;
        } else if (q.reg == 10) return "brief3d-root-other-binding";   // a root CBV b10 / UAV u10 etc.
    }
    if (out.t10 == UINT32_MAX) return "brief3d-root-t10-absent";
    if (out.b10 == UINT32_MAX) return "brief3d-root-b10-absent";
    if (k == Kind::selmap && out.t11 == UINT32_MAX) return "brief3d-root-t11-absent";
    return nullptr;
}

// ================================================================================================== options
// Sub-options of xr_terrain_stage (no new config key; the 32-key cap is full). Default OFF.
//   brief3d:<0|1>   1 = the tabletop map on the briefing / overview screen (needs vscreen:1 + vsearly:1 + xr_hud_layer: XR must run in the menus)
//   b3w:<m>         table (map) width in metres, the longer horizontal map axis (default 1.0, 0.2..4)
//   b3d:<m>         horizontal distance of the table centre in front of the head at placement (default 0.75, 0.2..5)
//   b3y:<m>         table top relative to the eye height at placement (default -0.45 = a table for a seated user, -2.5..1)
//   b3v:<x>         vertical exaggeration on top of the game's own (default 1, 0.25..8)
//   b3l:<0|1>       1 (default) = brief3d.md 11: the map draws are LATCHED and the XR consumer re-draws them every XR frame per eye with that
//                   frame's head pose (world-locked, no eye-guard refusal); 0 = the old per-game-frame replay into the producer's eye images
//   b3p:<0|1>       1 (default; headset test 9, docs/world-render/brief3d-worldlock.md) = with b3l:1 and u3q:1 the table quad is a WORLD-FIXED plane
//                   (posed once per placement, latch::TablePlane) and each eye's picture is drawn onto it from that eye's position; 0 = the old
//                   quad per eye posed at that eye's located pose every frame
struct Options {
    bool on = false;
    float width_m = 1.0f, dist_m = 0.75f, y_m = -0.45f, vexag = 1.0f;
    bool latch = true;
    bool fixed_plane = true;
    bool any_token = false;
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
} // namespace detail
// Removes every brief3d token from `value` (the first item, the stage id, is never an option); a value without them comes back byte for byte.
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
                if (k == "brief3d") { if (v == "1") o.on = true; else if (v == "0") o.on = false; else { why = "bad value for brief3d (0|1)"; return false; } }
                else if (k == "b3w") { if (!detail::num(v, 0.2, 4.0, d)) { why = "bad value for b3w (0.2..4 m)"; return false; } o.width_m = float(d); }
                else if (k == "b3d") { if (!detail::num(v, 0.2, 5.0, d)) { why = "bad value for b3d (0.2..5 m)"; return false; } o.dist_m = float(d); }
                else if (k == "b3y") { if (!detail::num(v, -2.5, 1.0, d)) { why = "bad value for b3y (-2.5..1 m)"; return false; } o.y_m = float(d); }
                else if (k == "b3v") { if (!detail::num(v, 0.25, 8.0, d)) { why = "bad value for b3v (0.25..8)"; return false; } o.vexag = float(d); }
                else if (k == "b3l") { if (v == "1") o.latch = true; else if (v == "0") o.latch = false; else { why = "bad value for b3l (0|1)"; return false; } }
                else if (k == "b3p") { if (v == "1") o.fixed_plane = true; else if (v == "0") o.fixed_plane = false; else { why = "bad value for b3p (0|1)"; return false; } }
                else ours = false;
                if (ours) o.any_token = true;
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}
// nullptr = the tabletop can run in this configuration; else the missing prerequisite (logged once, the option then does nothing).
inline const char* prerequisites(const Options& o, bool vscreen_on, bool vsearly_on, bool hud_layer) noexcept {
    if (!o.on) return "off";
    if (!vscreen_on) return "needs-vscreen:1";
    if (!vsearly_on) return "needs-vsearly:1";
    if (!hud_layer) return "needs-xr_hud_layer";
    return nullptr;
}
inline std::string describe(const Options& o) {
    char t[160];
    std::snprintf(t, sizeof t, "brief3d=%u;b3w=%.2f;b3d=%.2f;b3y=%.2f;b3v=%.2f;b3l=%u;b3p=%u", o.on ? 1u : 0u, double(o.width_m), double(o.dist_m), double(o.y_m),
                  double(o.vexag), o.latch ? 1u : 0u, o.fixed_plane ? 1u : 0u);
    return t;
}

// ================================================================================================== matrices (row vectors: h = [x y z 1] M, M[r*4+c])
struct Mat4 { double m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; };
inline Mat4 mul(const Mat4& a, const Mat4& b) noexcept {   // apply a, then b
    Mat4 o;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { double s = 0; for (int k = 0; k < 4; ++k) s += a.m[r * 4 + k] * b.m[k * 4 + c]; o.m[r * 4 + c] = s; }
    return o;
}
inline void xform(const Mat4& m, const double p[3], double h[4]) noexcept {
    for (int c = 0; c < 4; ++c) h[c] = p[0] * m.m[c] + p[1] * m.m[4 + c] + p[2] * m.m[8 + c] + m.m[12 + c];
}
inline bool finite(const Mat4& m) noexcept { for (double v : m.m) if (!std::isfinite(v)) return false; return true; }
inline Mat4 load(const uint8_t* raw) noexcept { Mat4 o; for (int i = 0; i < 16; ++i) { float f; std::memcpy(&f, raw + 4 * i, 4); o.m[i] = f; } return o; }

// The located eye (OpenXR LOCAL: metres, +y up, -z forward; quaternion x,y,z,w; FOV angles in radians) = acz::stereo::EyePose's fields.
struct EyeView { double position[3] = {0, 0, 0}; double orientation[4] = {0, 0, 0, 1}; double fov[4] = {-0.8, 0.8, 0.8, -0.8}; };
inline void quat_rotate(const double q[4], const double v[3], double out[3]) noexcept {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty); out[1] = v[1] + w * ty + (z * tx - x * tz); out[2] = v[2] + w * tz + (x * ty - y * tx);
}
// LOCAL -> eye view space (x right, y up, -z forward): the inverse of the eye's rigid pose.
inline Mat4 view_from_eye(const EyeView& e) noexcept {
    const double n = std::sqrt(e.orientation[0] * e.orientation[0] + e.orientation[1] * e.orientation[1] + e.orientation[2] * e.orientation[2] +
                               e.orientation[3] * e.orientation[3]);
    const double qi[4] = {-e.orientation[0] / n, -e.orientation[1] / n, -e.orientation[2] / n, e.orientation[3] / n};   // conjugate = inverse
    Mat4 v;
    for (int r = 0; r < 3; ++r) {   // row r = image of LOCAL axis r
        const double a[3] = {r == 0 ? 1.0 : 0.0, r == 1 ? 1.0 : 0.0, r == 2 ? 1.0 : 0.0};
        double o[3]; quat_rotate(qi, a, o);
        for (int c = 0; c < 3; ++c) v.m[r * 4 + c] = o[c];
        v.m[r * 4 + 3] = 0;
    }
    double t[3]; quat_rotate(qi, e.position, t);
    for (int c = 0; c < 3; ++c) v.m[12 + c] = -t[c];
    v.m[15] = 1;
    return v;
}
// Depth convention of the replayed draw = the game's PSO depth function (D3D12_COMPARISON_FUNC): GREATER / GREATER_EQUAL -> reverse (near = 1),
// LESS / LESS_EQUAL -> standard (near = 0); depth off or ALWAYS -> either (reverse). Eye depth value: reverse d = near / w, standard d = 1 - near / w.
inline constexpr uint32_t kCmpNever = 1, kCmpLess = 2, kCmpEqual = 3, kCmpLessEqual = 4, kCmpGreater = 5, kCmpNotEqual = 6, kCmpGreaterEqual = 7, kCmpAlways = 8;
enum class DepthMode : uint8_t { reverse, standard };
// nullptr = usable (mode + the eye depth clear value set). The game's map PSO depth settings are NOT known yet (live check B3-2 logs them).
inline const char* depth_mode(bool depth_enable, uint32_t func, DepthMode& mode, float& clear) noexcept {
    mode = DepthMode::reverse; clear = 0.0f;
    if (!depth_enable || func == kCmpAlways) return nullptr;
    if (func == kCmpGreater || func == kCmpGreaterEqual) return nullptr;
    if (func == kCmpLess || func == kCmpLessEqual) { mode = DepthMode::standard; clear = 1.0f; return nullptr; }
    return "brief3d-pso-depth-func";
}
// Eye view -> PS2 homogeneous screen for DrawSelMap_v / DrawModelIcon_v with the game's own b10 (cb10 = the screen divisor, kept bound):
//   w = -z_view;  x_ps2 = h.x / w = 2048 + ndc_x cb10.x / 2 with ndc_x = (2 tan_x - (tR + tL)) / (tR - tL), tan_x = x_view / w (same for y, up/down);
//   z_s = h.z / w * 2^-24 = d(w) (depth_mode). FOV angles -> tangents. nullptr = built; else why.
inline const char* ps2_projection(const double fov[4], const float cb10[2], double near_m, DepthMode mode, Mat4& out) noexcept {
    const double tl = std::tan(fov[0]), tr = std::tan(fov[1]), tu = std::tan(fov[2]), td = std::tan(fov[3]);
    if (!std::isfinite(tl + tr + tu + td) || !(tr - tl > 1e-6) || !(tu - td > 1e-6)) return "brief3d-fov";
    if (!std::isfinite(cb10[0]) || !std::isfinite(cb10[1]) || std::fabs(cb10[0]) < 1.0f || std::fabs(cb10[1]) < 1.0f) return "brief3d-cb10";
    if (!(near_m > 0.001 && near_m < 1.0)) return "brief3d-near";
    const double hx = double(cb10[0]) / 2.0, hy = double(cb10[1]) / 2.0;
    Mat4 p; for (double& v : p.m) v = 0;
    // view x, y, z, 1 -> columns x, y, z, w
    p.m[0 * 4 + 0] = hx * 2.0 / (tr - tl);
    p.m[1 * 4 + 1] = hy * 2.0 / (tu - td);
    p.m[2 * 4 + 0] = -(kPs2Centre - hx * (tr + tl) / (tr - tl));   // times w = -z
    p.m[2 * 4 + 1] = -(kPs2Centre - hy * (tu + td) / (tu - td));
    p.m[2 * 4 + 3] = -1.0;
    const double s = 1.0 / kDepthUnit;   // h.z = 2^24 (a w + b): reverse a = 0, b = near; standard a = 1, b = -near
    if (mode == DepthMode::reverse) { p.m[2 * 4 + 2] = 0.0; p.m[3 * 4 + 2] = s * near_m; }
    else { p.m[2 * 4 + 2] = -s; p.m[3 * 4 + 2] = -s * near_m; }
    out = p;
    return nullptr;
}
// CPU mirror of DrawSelMap_v's position path (offline tests): SV_Position (x, y in NDC, z in [0,1] when visible, w = 1). false: w <= 0 (the shader
// would mirror the vertex) or non-finite.
inline bool emulate_selmap(const Mat4& m, const float cb10[2], const double p[3], double sv[3]) noexcept {
    double h[4]; xform(m, p, h);
    if (!(h[3] > 0) || !std::isfinite(h[0] + h[1] + h[2] + h[3])) return false;
    sv[0] = (h[0] / h[3] - kPs2Centre) * 2.0 / double(cb10[0]);
    sv[1] = (h[1] / h[3] - kPs2Centre) * 2.0 / double(cb10[1]);
    sv[2] = h[2] / h[3] * kDepthUnit;
    return true;
}

// ================================================================================================== table placement (world-locked)
// Placed when the briefing table is ENTERED (and after a LOCAL space change = recentre): at the head's yaw (pitch/roll ignored), b3d ahead, b3y
// relative to the eye height. Table frame: origin = centre of the table top, +y up, -z = away from the user at placement.
struct Placement { double centre[3] = {0, 0, 0}; double yaw = 0; bool valid = false; uint32_t space_generation = 0; };
inline double head_yaw(const double q[4]) noexcept {   // yaw of the head's forward (-z) about +y
    const double f0[3] = {0, 0, -1}; double f[3]; quat_rotate(q, f0, f);
    return std::atan2(-f[0], -f[2]);
}
inline Placement place(const Options& o, const double head_pos[3], const double head_q[4], uint32_t space_generation) noexcept {
    Placement p; p.yaw = head_yaw(head_q);
    const double fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
    p.centre[0] = head_pos[0] + fx * o.dist_m; p.centre[1] = head_pos[1] + o.y_m; p.centre[2] = head_pos[2] + fz * o.dist_m;
    p.valid = std::isfinite(p.centre[0] + p.centre[1] + p.centre[2] + p.yaw); p.space_generation = space_generation;
    return p;
}
// table frame -> LOCAL (rotation about +y by yaw, then the centre)
inline Mat4 table_to_local(const Placement& p) noexcept {
    Mat4 t; const double c = std::cos(p.yaw), s = std::sin(p.yaw);
    t.m[0] = c;  t.m[1] = 0; t.m[2] = -s;
    t.m[4] = 0;  t.m[5] = 1; t.m[6] = 0;
    t.m[8] = s;  t.m[9] = 0; t.m[10] = c;
    t.m[12] = p.centre[0]; t.m[13] = p.centre[1]; t.m[14] = p.centre[2];
    return t;
}

// ================================================================================================== model-space fit
// The grid's model-space bounding box (from the vertex buffer, if readable: brief3d.md 3; else from the phase-2 GPU copy) and the game's SelMap
// matrix of the same frame. The up axis is the box's flattest axis; its sign = the side the game camera is on (the briefing camera looks down on the
// map); the handedness = the on-screen orientation of the two horizontal axes in the game's own image (seen from above), so the table is never
// mirrored. Scale: the longer horizontal extent = b3w metres; the lowest grid point sits on the table top.
struct Box { double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}; uint32_t vertices = 0; bool valid = false; };
// Vertex bytes of DrawSelMap_v (ATTRIBUTE0 float4 x, y, z, colour index) at `stride`; vertices [first, first + count). Refused when any position is
// non-finite or beyond 1e7 (fail closed: a wrong buffer / format), or when count is 0.
inline const char* box_from_vertices(const uint8_t* bytes, size_t size, uint32_t stride, uint32_t first, uint32_t count, Box& out) noexcept {
    out = Box{};
    if (!bytes || !count) return "brief3d-box-empty";
    if (stride < 12 || stride > 256) return "brief3d-box-stride";
    if ((uint64_t(first) + count) * stride > size + (stride - 12)) return "brief3d-box-span";
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* v = bytes + size_t(first + i) * stride;
        for (int k = 0; k < 3; ++k) {
            float f; std::memcpy(&f, v + 4 * k, 4);
            if (!std::isfinite(f) || std::fabs(f) > 1e7f) return "brief3d-box-nonfinite";
            if (i == 0 || f < out.lo[k]) out.lo[k] = f;
            if (i == 0 || f > out.hi[k]) out.hi[k] = f;
        }
    }
    out.vertices = count; out.valid = true;
    return nullptr;
}
// The game camera's centre in model space: the point where the x, y and w columns of M all vanish (a projective camera's null point).
inline const char* camera_centre(const Mat4& m, double c[3]) noexcept {
    const int cols[3] = {0, 1, 3};
    double a[3][3], b[3];
    for (int i = 0; i < 3; ++i) { for (int k = 0; k < 3; ++k) a[i][k] = m.m[k * 4 + cols[i]]; b[i] = -m.m[12 + cols[i]]; }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    double scale = 0; for (auto& r : a) for (double v : r) scale = std::fmax(scale, std::fabs(v));
    if (!std::isfinite(det) || !(scale > 0) || std::fabs(det) <= 1e-12 * scale * scale * scale) return "brief3d-camera-singular";
    for (int j = 0; j < 3; ++j) {   // Cramer
        double t[3][3]; std::memcpy(t, a, sizeof t);
        for (int i = 0; i < 3; ++i) t[i][j] = b[i];
        const double dj = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                          t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
        c[j] = dj / det;
    }
    return std::isfinite(c[0] + c[1] + c[2]) ? nullptr : "brief3d-camera-nonfinite";
}
struct Fit {
    int up = -1, up_sign = 0, a1 = -1, a2 = -1, a2_sign = 0;
    double origin[3] = {0, 0, 0};   // model point that lands on the table centre (horizontal centre, lowest grid height)
    double metres_per_unit = 0, vexag = 1;
    Mat4 model_to_table;
    bool valid = false;
};
inline const char* fit_model(const Box& box, const Mat4& game, const float cb10[2], const Options& o, Fit& out) noexcept {
    out = Fit{};
    if (!box.valid) return "brief3d-box-invalid";
    double e[3]; for (int k = 0; k < 3; ++k) e[k] = box.hi[k] - box.lo[k];
    int up = 0; for (int k = 1; k < 3; ++k) if (e[k] < e[up]) up = k;
    const int a1 = up == 0 ? 1 : 0, a2 = up == 2 ? 1 : 2;
    const double horiz = std::fmax(e[a1], e[a2]), other = std::fmin(e[a1], e[a2]);
    if (!(other > 0) || !(horiz > 0)) return "brief3d-box-degenerate";
    if (!(e[up] <= 0.6 * other)) return "brief3d-box-not-flat";   // a grid map: its height range is well below its width (live check B3-4)
    double c[3];
    if (const char* why = camera_centre(game, c)) return why;
    double mid[3]; for (int k = 0; k < 3; ++k) mid[k] = 0.5 * (box.lo[k] + box.hi[k]);
    const int up_sign = c[up] > box.hi[up] ? 1 : c[up] < box.lo[up] ? -1 : 0;
    if (!up_sign) return "brief3d-camera-not-above-map";
    // handedness in the game's own image (D3D NDC, y up): project mid, mid + a1, mid + a2 (one tenth of the extent each)
    double s[3][2];
    for (int i = 0; i < 3; ++i) {
        double p[3] = {mid[0], mid[1], mid[2]};
        if (i == 1) p[a1] += 0.1 * e[a1];
        if (i == 2) p[a2] += 0.1 * e[a2];
        double h[4]; xform(game, p, h);
        if (!(h[3] > 0)) return "brief3d-map-behind-game-camera";
        s[i][0] = (h[0] / h[3] - kPs2Centre) * 2.0 / double(cb10[0]);
        s[i][1] = (h[1] / h[3] - kPs2Centre) * 2.0 / double(cb10[1]);
    }
    const double cross = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[1][1] - s[0][1]) * (s[2][0] - s[0][0]);
    if (!std::isfinite(cross) || cross == 0.0) return "brief3d-handedness-degenerate";
    // Our table seen from above (camera on +y, looking down, screen-up = -z): a1 -> +x = NDC (1, 0); a2 -> a2_sign z = NDC (0, -a2_sign): cross = -a2_sign.
    const int a2_sign = cross > 0 ? -1 : 1;
    // An up-sign of -1 means the game camera is on the -up side: "seen from above" then is seen from that side; the same NDC rule holds because the
    // up axis is mapped onto +y with that sign (the viewer of the table is on +y).
    Fit f; f.up = up; f.up_sign = up_sign; f.a1 = a1; f.a2 = a2; f.a2_sign = a2_sign;
    f.metres_per_unit = double(o.width_m) / horiz; f.vexag = double(o.vexag);
    f.origin[a1] = mid[a1]; f.origin[a2] = mid[a2]; f.origin[up] = up_sign > 0 ? box.lo[up] : box.hi[up];
    Mat4 t; for (double& v : t.m) v = 0;
    const double s0 = f.metres_per_unit;
    t.m[a1 * 4 + 0] = s0;
    t.m[up * 4 + 1] = s0 * f.vexag * up_sign;
    t.m[a2 * 4 + 2] = s0 * a2_sign;
    for (int col = 0; col < 3; ++col) { double acc = 0; for (int k = 0; k < 3; ++k) acc += f.origin[k] * t.m[k * 4 + col]; t.m[12 + col] = -acc; }
    t.m[15] = 1;
    f.model_to_table = t; f.valid = finite(t);
    out = f;
    return f.valid ? nullptr : "brief3d-fit-nonfinite";
}
// The table volume in table space: x and z half extents, height (from the fit, for the eye guard and the consumer's base plate).
struct Volume { double half_x = 0, half_z = 0, height = 0; };
inline Volume volume(const Box& box, const Fit& f) noexcept {
    Volume v; if (!f.valid) return v;
    v.half_x = 0.5 * (box.hi[f.a1] - box.lo[f.a1]) * f.metres_per_unit;
    v.half_z = 0.5 * (box.hi[f.a2] - box.lo[f.a2]) * f.metres_per_unit;
    v.height = (box.hi[f.up] - box.lo[f.up]) * f.metres_per_unit * f.vexag;
    return v;
}
// ---- zoom-aware content fit (brief3d.md section 9.1). Live (N2, 2026-10-29): the game's zoom (cross / Space) magnifies a region around the
// player's flight ~15x in the grid's model space: the grid keeps its -32..32 layout, the icons it still draws (only the units inside the region:
// PIXY and CIPHER in Roselein) sit 15x higher (matrix-3 translation y 3.16 -> 47.9 units = 0.05 -> 0.75 m above the table at b3w 1, i.e. ABOVE the
// seated eye: out of view while the user looks down at the table). The latched fit is kept; the table is scaled UNIFORMLY (no distortion) about
// its centre on the table top so that the content top stays `max_height_m` above the table, and the eye-guard volume grows to the content top.
inline constexpr int kIconModelMatrix = 3;   // [I] the census: matrices 1/2 constant for every icon, matrix 3's translation = the unit position
// The icon's height above the fit origin along the fit's up axis, in model units (matrix kIconModelMatrix's translation row). false = unusable
// (no fit, non-finite, or more than kIconHeightCapM above/below the table at the unscaled fit: then it is not a position and is ignored).
inline constexpr double kIconHeightCapM = 8.0;
inline bool icon_height(const uint8_t* icon, const Fit& f, double& h) noexcept {
    h = 0;
    if (!icon || !f.valid || f.up < 0 || f.up > 2 || !(f.metres_per_unit > 0)) return false;
    float v; std::memcpy(&v, icon + 64 * kIconModelMatrix + 4 * (12 + f.up), 4);
    if (!std::isfinite(v)) return false;
    const double d = (double(v) - f.origin[f.up]) * double(f.up_sign);
    if (!(std::fabs(d) * f.metres_per_unit * f.vexag <= kIconHeightCapM)) return false;
    h = d;
    return true;
}
// The highest content point the table may carry: the content top stays >= 0.10 m below the seated eye (b3y is the table top relative to eye height).
inline double content_max_height(const Options& o) noexcept { return std::fmax(0.10, -double(o.y_m) - 0.10); }
// Uniform scale [kMinContentScale..1] that keeps a content top of `top_units` within `max_height_m`; 1 = unchanged (the overview: 0.05 m).
// 0 = the cap would need a scale below kMinContentScale: the caller must refuse ("brief3d-content-too-tall"), not exceed the cap.
inline constexpr double kMinContentScale = 0.1;
inline double content_scale(const Fit& f, double top_units, double max_height_m) noexcept {
    if (!f.valid || !(top_units > 0) || !(max_height_m > 0)) return 1.0;
    const double h = top_units * f.metres_per_unit * f.vexag;
    if (!std::isfinite(h) || !(h > max_height_m)) return 1.0;
    const double k = max_height_m / h;
    if (!(k >= kMinContentScale)) return 0.0;   // the cap cannot be honoured within the minimum table size: never claim a capped fit (W8-1 P1)
    return k;
}
// The fit scaled uniformly by k about the table origin (the table-top centre): every model point p -> k * (p x model_to_table).
inline Fit scaled_fit(const Fit& f, double k) noexcept {
    Fit s = f;
    if (!f.valid || !(k > 0) || k == 1.0) return s;
    s.metres_per_unit = f.metres_per_unit * k;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) s.model_to_table.m[r * 4 + c] = f.model_to_table.m[r * 4 + c] * k;
    s.valid = finite(s.model_to_table);
    return s;
}
// brief3d.md 9.2 (headset 2026-10-29 item 4): the UNIFORM scale shrank the whole table to 0.43 m at the zoom ("it just zooms out") and
// left the two aircraft as ~1 cm arrows. The content fit is now VERTICAL ONLY: the table keeps its width (the zoomed terrain fills it, like the
// game's zoom), heights are compressed by kv so the content top stays at the cap. The icon meshes are compensated (icon_copies) so they are not
// flattened. The eye-guard volume (volume(): height from vexag) follows the compressed heights.
inline Fit squashed_fit(const Fit& f, double kv) noexcept {
    Fit s = f;
    if (!f.valid || !(kv > 0) || kv == 1.0) return s;
    s.vexag = f.vexag * kv;
    for (int r = 0; r < 4; ++r) s.model_to_table.m[r * 4 + 1] = f.model_to_table.m[r * 4 + 1] * kv;   // table y column (translation included)
    s.valid = finite(s.model_to_table);
    return s;
}
// brief3d.md 10 (headset test 5, "the table shakes a lot"): the content top is a MEASURED quantity (the highest icon the game drew in the last frame
// window). The game omits icons from frame to frame (summary-brief3d: 35 map frames in 5 s but 12 icons in them), so a frame without icons read as
// top = 0, the old 0.5x rule dropped the latch to 0 at once and the whole terrain's vertical scale jumped 0.68 <-> 1 with each omission.
// Now: GROWTH is immediate (the 0.35 m cap must hold), but a lower reading must persist for kContentHoldMs before the latch shrinks, and the
// resulting vertical scale may RELAX only at kContentRelaxPerS per second (tightening is immediate: the cap is a safety limit).
// Headset test 8 (2026-11-01, "the icons floated a little higher after a few seconds"; ee-camera.log brief3d-content: scale 0.6155 -> 0.80 -> 0.68
// -> 0.6155 -> 1.0, height-m 0.35 -> 0.185 -> 0.232, changes=249 in 70 s): the slow relax RAISED every icon above the table whenever the measured
// content top fell (icons leaving the frame, a zoom-out), and the next tall icon snapped them down again. The icons are part of the table: within one
// briefing page the scale is now MONOTONE (kContentRelaxPerS = 0: it tightens when the cap needs it and never relaxes); a new page (the map draws
// stopped for kScreenGapMs) starts from 1 again.
inline constexpr int64_t kContentHoldMs = 4000;
inline constexpr double kContentRelaxPerS = 0.0;
struct ContentLatch { double top = 0; int64_t low_since_ms = 0; };
inline double latch_content_top(ContentLatch& l, double observed, int64_t now_ms) noexcept {
    if (observed > l.top) { l.top = observed * 1.1; l.low_since_ms = 0; }
    else if (l.top > 0 && observed < 0.5 * l.top) {
        if (!l.low_since_ms) l.low_since_ms = now_ms ? now_ms : 1;
        else if (now_ms - l.low_since_ms >= kContentHoldMs) { l.top = observed > 0 ? observed * 1.1 : 0.0; l.low_since_ms = 0; }
    } else l.low_since_ms = 0;
    return l.top;
}
inline double relax_content_scale(double cur, double target, int64_t dt_ms) noexcept {
    if (!(target > cur)) return target;
    const double step = kContentRelaxPerS * double(dt_ms > 0 ? dt_ms : 0) / 1000.0;
    return cur + step < target ? cur + step : target;
}
// The grid box extended along up to the content top (the eye guard then covers the icons above the grid; horizontal extents unchanged).
inline Box content_box(const Box& b, const Fit& f, double top_units) noexcept {
    Box c = b;
    if (!b.valid || !f.valid || f.up < 0 || !(top_units > 0)) return c;
    if (f.up_sign > 0) c.hi[f.up] = std::fmax(c.hi[f.up], f.origin[f.up] + top_units);
    else c.lo[f.up] = std::fmin(c.lo[f.up], f.origin[f.up] - top_units);
    return c;
}
// Every corner of the table volume at least `near_m` in front of the eye (w = -z_view >= near). The volume is convex and w is affine, so then every
// grid vertex and icon inside it is in front of the eye and DrawSelMap_v's own divide is safe. Icons above the grid top: `margin_m` of headroom.
inline bool eye_clear_of_volume(const Volume& v, const Mat4& table_to_view, double near_m, double margin_m = 0.15) noexcept {
    for (int i = 0; i < 8; ++i) {
        const double p[3] = {(i & 1) ? v.half_x : -v.half_x, (i & 2) ? v.height + margin_m : 0.0, (i & 4) ? v.half_z : -v.half_z};
        double h[4]; xform(table_to_view, p, h);
        if (!(-h[2] >= near_m)) return false;
    }
    return true;
}

// ================================================================================================== per-eye records
struct EyeCfg { double near_m = 0.05; DepthMode depth = DepthMode::reverse; };
// M_eye = model_to_table x table_to_local x view(eye) x ps2_projection(eye fov, cb10). nullptr = built.
inline const char* eye_matrix(const Fit& f, const Placement& pl, const EyeView& eye, const float cb10[2], const EyeCfg& cfg, Mat4& out) noexcept {
    if (!f.valid) return "brief3d-fit-invalid";
    if (!pl.valid) return "brief3d-placement-invalid";
    Mat4 p;
    if (const char* why = ps2_projection(eye.fov, cb10, cfg.near_m, cfg.depth, p)) return why;
    out = mul(mul(mul(f.model_to_table, table_to_local(pl)), view_from_eye(eye)), p);
    return finite(out) ? nullptr : "brief3d-eye-nonfinite";
}
inline bool store(const Mat4& m, uint8_t* out) noexcept {
    for (int i = 0; i < 16; ++i) { const float f = float(m.m[i]); if (!std::isfinite(f)) return false; std::memcpy(out + 4 * i, &f, 4); }
    return true;
}
// SelMap: the copy is the eye matrix (64 B; the whole SrtData). nullptr = written.
inline const char* selmap_record(const Mat4& eye_m, uint8_t out[kSelMapSrtBytes]) noexcept {
    return store(eye_m, out) ? nullptr : "brief3d-selmap-nonfinite";
}
// ModelIcon: which of the four matrices is the map camera? The one equal (1e-5 relative) to THIS frame's SelMap matrix, i.e. the icons are drawn in
// the grid's model space with the same view-projection (inference from "model matrix x view-projection in the shader", briefing-3d-map.md 2.1; live
// check B3-5 logs the match). Exactly one match = its index; none / several = refused (the icons are then not replayed: grid only).
inline const char* icon_camera_index(const uint8_t* icon, const uint8_t* selmap64, int& index) noexcept {
    index = -1;
    if (!icon || !selmap64) return "brief3d-icon-camera-inputs";
    double mag = 0; float ref[16];
    std::memcpy(ref, selmap64, 64);
    for (float v : ref) { if (!std::isfinite(v)) return "brief3d-icon-camera-inputs"; mag = std::fmax(mag, std::fabs(double(v))); }
    if (!(mag > 0)) return "brief3d-icon-camera-inputs";
    int found = 0;
    for (int k = 0; k < 4; ++k) {
        float m[16]; std::memcpy(m, icon + 64 * k, 64);
        bool eq = true;
        for (int i = 0; i < 16 && eq; ++i) eq = std::isfinite(m[i]) && std::fabs(double(m[i]) - double(ref[i])) <= 1e-5 * mag;
        if (eq) { index = k; ++found; }
    }
    if (found == 1) return nullptr;
    index = -1;
    return found ? "brief3d-icon-camera-ambiguous" : "brief3d-icon-camera-unmatched";
}
// The icon copy: the game's 272 bytes with matrix `index` replaced by the eye matrix (the model matrix, the lighting matrices and the vector kept).
inline const char* modelicon_record(const uint8_t* icon, int index, const Mat4& eye_m, uint8_t out[kModelIconSrtBytes]) noexcept {
    if (!icon || index < 0 || index > 3) return "brief3d-icon-index";
    std::memcpy(out, icon, kModelIconSrtBytes);
    return store(eye_m, out + 64 * index) ? nullptr : "brief3d-icon-nonfinite";
}

// ================================================================================================== GPU replay plan (per slot)
// The per-slot UPLOAD block (written only while the slot is WRITING by the recording that owns it): SelMap copies first, then the icon copies; each
// copy at a 256-byte stride (a root SRV VA needs 4-byte alignment; 256 keeps every copy on its own cache line pair and matches CBV rules if a later
// layout needs them).
inline constexpr unsigned kSelMapCap = 4;        // live: 1 per frame
inline constexpr unsigned kIconCap = 128;        // live: 63 per frame
inline constexpr unsigned kCopyStrideSel = 256, kCopyStrideIcon = 512;
inline constexpr uint64_t kAlign = 65536;
inline constexpr uint64_t align_up(uint64_t v, uint64_t a) noexcept { return (v + a - 1) / a * a; }
inline constexpr uint64_t kSlotUploadBytes = align_up(uint64_t(kSelMapCap) * 2 * kCopyStrideSel + uint64_t(kIconCap) * 2 * kCopyStrideIcon, kAlign);
static_assert(kSlotUploadBytes == 196608, "three 64 KiB pages per slot");
inline constexpr uint64_t copy_offset(Kind k, unsigned i, unsigned eye) noexcept {
    return k == Kind::selmap ? (uint64_t(i) * 2 + eye) * kCopyStrideSel
                             : uint64_t(kSelMapCap) * 2 * kCopyStrideSel + (uint64_t(i) * 2 + eye) * kCopyStrideIcon;
}
// Memory: per slot 2 eye colour images in the PSO's RTV format + 2 eye depth images (per slot: frames never share depth, Astra 6460640585 P2-3)
// + the UPLOAD block; caps as fxmem:4.
inline constexpr uint64_t kImageCap = 32ull << 20, kTotalCap = 200ull << 20;
inline const char* budget(unsigned rt_bpp, unsigned ds_bpp, uint32_t w, uint32_t h, unsigned slots, uint64_t& charged) noexcept {
    charged = 0;
    if (!rt_bpp) return "brief3d-format-unsupported";
    if (!w || !h || w > 8192 || h > 8192 || !slots || slots > 4) return "brief3d-size-unsupported";
    const uint64_t rt = uint64_t(w) * h * rt_bpp, ds = uint64_t(w) * h * ds_bpp;
    if (rt > kImageCap || ds > kImageCap) return "brief3d-over-image-cap";
    const uint64_t per = 2 * (align_up(rt, kAlign) + kAlign) + (ds_bpp ? 2 * (align_up(ds, kAlign) + kAlign) : 0) + kSlotUploadBytes;
    const uint64_t total = per * slots;
    if (total > kTotalCap) return "brief3d-over-total-cap";
    charged = total;
    return nullptr;
}

// ---- aerial icons on the table (brief3d.md 9.2). Evidence: in the zoom the game draws PIXY/CIPHER as two DrawModelIcon_v arrows (both
// replayed, nothing refused); their vertical STALKS and names are 2D ps2_v draws (534f29ca/d34ee153: GS lines 17 -> 7 per frame = 5 + one per
// aircraft [I], text 1 -> 2) on the virtual screen, which carry no 3D position and cannot be replayed. So the table draws its own stalk: the
// icon's own draw re-issued as small dots from the table top up to the aircraft (mesh-agnostic: the icon VB is never read).
inline constexpr unsigned kMaxIconCopies = 9;          // the icon + up to kStalkDots dots, one plan (one recording reservation)
inline constexpr unsigned kStalkDots = 8;
inline constexpr unsigned kStalkCopyBudget = kIconCap / 2;   // dots only while the frame's icon copies stay <= 64 (63 overview icons fit after)
inline constexpr double kAirMinTableM = 0.08;          // an icon this high above the table top is AIR: stalk, min size, clamps (overview: 0.05 m)
inline constexpr double kAirMinSizeM = 0.04;           // air icon mesh unit (longest model-matrix row) at least this many metres on the table
inline constexpr double kAirMaxGrow = 4.0;
inline constexpr double kStalkDotScale = 0.25;         // dot = the icon mesh at a quarter of its (grown) size
struct IconCopies { unsigned copies = 1; bool air = false; float m3[kMaxIconCopies][16] = {}; };
// Matrix kIconModelMatrix for each copy of one icon draw. `f` = the latched (unsquashed) fit, `kv` = the vertical content scale, `box` = the
// fit's grid box (air icons are kept above it horizontally), `max_h_m` = the content cap above the table top (air icons never higher: never
// above the head), `room` = icon copies the slot may still take for this draw. Ground icons at kv 1 come back byte-identical (copies 1).
inline void icon_copies(const uint8_t* icon, const Fit& f, double kv, const Box& box, double max_h_m, unsigned room, IconCopies& out) noexcept {
    out = IconCopies{};
    float m[16]; std::memcpy(m, icon + 64 * kIconModelMatrix, 64);
    std::memcpy(out.m3[0], m, 64);
    bool fin = true; for (float v : m) fin = fin && std::isfinite(v);
    if (!fin || !f.valid || f.up < 0 || f.up > 2 || f.a1 < 0 || f.a2 < 0 || !(f.metres_per_unit > 0) || !(kv > 0)) return;
    const int up = f.up;
    double L[16]; for (int i = 0; i < 16; ++i) L[i] = m[i];
    if (kv != 1.0) for (int r = 0; r < 3; ++r) L[r * 4 + up] /= kv;   // undo the table's vertical compression on the mesh (translation untouched)
    const double vs = f.metres_per_unit * f.vexag * kv;               // table metres per model unit along up
    double d = (double(m[12 + up]) - f.origin[up]) * double(f.up_sign);
    out.air = d * vs > kAirMinTableM && std::isfinite(d * vs);
    if (out.air) {
        if (d * vs > max_h_m && max_h_m > 0) { d = max_h_m / vs; L[12 + up] = f.origin[up] + double(f.up_sign) * d; }
        for (int a : {f.a1, f.a2}) if (box.valid) L[12 + a] = std::fmin(std::fmax(L[12 + a], box.lo[a]), box.hi[a]);
        double len = 0;
        for (int r = 0; r < 3; ++r) len = std::fmax(len, std::sqrt(double(m[r * 4]) * m[r * 4] + double(m[r * 4 + 1]) * m[r * 4 + 1] + double(m[r * 4 + 2]) * m[r * 4 + 2]));
        const double size_m = len * f.metres_per_unit;
        const double g = size_m > 0 ? std::fmin(std::fmax(kAirMinSizeM / size_m, 1.0), kAirMaxGrow) : 1.0;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) L[r * 4 + c] *= g;
    }
    for (int i = 0; i < 16; ++i) out.m3[0][i] = float(L[i]);
    if (!out.air || room < 1 + kStalkDots) return;
    for (unsigned k = 1; k <= kStalkDots; ++k) {
        double D[16]; std::memcpy(D, L, sizeof D);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) D[r * 4 + c] *= kStalkDotScale;
        D[12 + up] = f.origin[up] + double(f.up_sign) * d * double(k) / double(kStalkDots + 1);
        for (int i = 0; i < 16; ++i) out.m3[k][i] = float(D[i]);
    }
    out.copies = 1 + kStalkDots;
}

// Per-draw gate: facts the producer holds at the map draw (nullptr = replay per eye; else the named refusal: the game's draw runs unchanged and
// nothing of ours is recorded for it).
struct GateIn {
    Kind kind = Kind::none;
    bool ready = false;              // the slot's eye images + UPLOAD span exist and the slot is WRITING for this list
    bool frame_ok = false;           // this frame has a fit, a placement, both eye matrices and the eye guard passed (FrameState::eyes_ok)
    bool root_ok = false;            // root_layout_check passed for the bound root signature
    bool t10_va = false;             // the game's t10 VA is known (restore value)
    bool b10_written = false;        // cb10's two words were written since the root was bound (the matrices use them)
    bool predication_off = false;    // the list has no predication set (the menus' lists: live check B3-2)
    uint32_t pso_rtv0 = 0, pso_rt_count = 0, pso_dsv = 0, pso_samples = 0, image_format = 0, image_ds_format = 0;
    bool depth_enable = false, depth_write = false, stencil = false; uint32_t depth_func = 0;
    // D-370: the map PSO has stencil ON. The eye DSV's stencil is ours (cleared to 0), the game's OMSetStencilRef is untracked: the replay is
    // proven independent of the stencil content only when both faces test ALWAYS (writes then only change our private stencil plane).
    bool stencil_always = false;
    bool vp_known = false, scissor_known = false; unsigned viewports = 0, scissors = 0;
    bool icon_camera = true;         // ModelIcon: icon_camera_index matched (SelMap: true)
};
inline const char* gate(const GateIn& g) noexcept {
    if (g.kind == Kind::none) return "brief3d-not-a-map-draw";
    if (!g.ready) return "brief3d-not-ready";
    if (!g.frame_ok) return "brief3d-frame-not-ready";
    if (!g.root_ok) return "brief3d-root-layout";
    if (!g.t10_va) return "brief3d-t10-va";
    if (!g.b10_written) return "brief3d-b10-unwritten";
    if (!g.predication_off) return "brief3d-predicated";
    if (g.pso_samples != 1) return "brief3d-pso-multisample";
    if (g.pso_rt_count != 1 || g.pso_rtv0 != g.image_format) return "brief3d-rt-format-differs";
    if (g.pso_dsv != g.image_ds_format) return "brief3d-dsv-format-differs";
    if (g.stencil && !g.stencil_always) return "brief3d-pso-stencil-test";
    if (g.depth_enable) { DepthMode m; float c; if (depth_mode(true, g.depth_func, m, c)) return "brief3d-pso-depth-func"; }
    if (!g.vp_known || g.viewports != 1 || !g.scissor_known || g.scissors != 1) return "brief3d-viewport-unknown";   // restored after the replay
    if (g.kind == Kind::modelicon && !g.icon_camera) return "brief3d-icon-camera";
    return nullptr;
}

// The recorded sequence right BEFORE the game's own map draw (native vtable calls; the proxy's trackers never see them; copies > 1: see above):
//   [first replay of the frame: ClearRTV(eye 0), ClearRTV(eye 1), ClearDSV(eye 0), ClearDSV(eye 1)]
//   { OMSet(eye e RT, eye e DSV), RSSetViewports(eye image), RSSetScissorRects(eye image), RootSRV(t10, eye copy e), Draw(game args) } x 2,
//   RootSRV(t10, game VA), RSSetViewports(game), RSSetScissorRects(game), OMSet(game targets).
// The PSO, IA buffers, topology, t11, b10 and predication (proven off) are the game's and never changed. Then the game's draw, forwarded unchanged.
enum class CmdKind : uint8_t { clear_rt, clear_ds, om_eye, vp_eye, sc_eye, root_srv, draw, vp_restore, sc_restore, om_restore };
struct Cmd { CmdKind kind = CmdKind::draw; uint8_t eye = 0; uint32_t parameter = 0; uint64_t va = 0; };
// brief3d 9.2: an icon plan may draw `copies` (1..kMaxIconCopies) consecutive upload copies per eye (the icon + its stalk dots): per eye OM/RS once,
// then { RootSRV(copy c), Draw } per copy. Copy c of eye e is at eye_va[e] + c * step (copy_offset(k, index + c, e)).
inline constexpr unsigned kMaxCmds = 4 + 2 * (3 + 2 * kMaxIconCopies) + 4;
struct RootSwap { uint32_t parameter = UINT32_MAX; uint64_t eye_va[2] = {0, 0}; uint64_t restore_va = 0; unsigned copies = 1; uint64_t step = 0; };
inline const char* plan_root_swap(Kind k, uint32_t parameter, uint64_t game_va, uint64_t upload_va, uint64_t upload_bytes, unsigned index,
                                  RootSwap& out, unsigned copies = 1) noexcept {
    out = RootSwap{};
    if (k == Kind::none) return "brief3d-kind";
    if (parameter == UINT32_MAX || parameter >= 64) return "brief3d-t10-parameter";
    if (!game_va) return "brief3d-t10-game-va";
    if (!upload_va || upload_va % 256 != 0) return "brief3d-upload-va";
    if (copies < 1 || copies > (k == Kind::selmap ? 1u : kMaxIconCopies)) return "brief3d-copies";
    if (index >= (k == Kind::selmap ? kSelMapCap : kIconCap) || copies > (k == Kind::selmap ? kSelMapCap : kIconCap) - index) return "brief3d-replay-cap";
    const uint64_t end = copy_offset(k, index + copies - 1, 1) + (k == Kind::selmap ? kCopyStrideSel : kCopyStrideIcon);
    if (upload_bytes < end || upload_va > UINT64_MAX - end) return "brief3d-upload-span";
    out.parameter = parameter; out.restore_va = game_va; out.copies = copies;
    out.step = 2ull * (k == Kind::selmap ? kCopyStrideSel : kCopyStrideIcon);
    for (unsigned e = 0; e < 2; ++e) out.eye_va[e] = upload_va + copy_offset(k, index, e);
    return nullptr;
}
inline unsigned replay_commands(const RootSwap& r, bool clear_first, bool depth, Cmd out[kMaxCmds]) noexcept {
    if (r.parameter == UINT32_MAX) return 0;
    unsigned n = 0;
    if (clear_first) for (uint8_t e = 0; e < 2; ++e) { out[n++] = Cmd{CmdKind::clear_rt, e, 0, 0}; if (depth) out[n++] = Cmd{CmdKind::clear_ds, e, 0, 0}; }
    for (uint8_t e = 0; e < 2; ++e) {
        out[n++] = Cmd{CmdKind::om_eye, e, 0, 0};
        out[n++] = Cmd{CmdKind::vp_eye, e, 0, 0};
        out[n++] = Cmd{CmdKind::sc_eye, e, 0, 0};
        for (unsigned c = 0; c < r.copies && c < kMaxIconCopies; ++c) {
            out[n++] = Cmd{CmdKind::root_srv, e, r.parameter, r.eye_va[e] + uint64_t(c) * r.step};
            out[n++] = Cmd{CmdKind::draw, e, 0, 0};
        }
    }
    out[n++] = Cmd{CmdKind::root_srv, 0, r.parameter, r.restore_va};
    out[n++] = Cmd{CmdKind::vp_restore, 0, 0, 0};
    out[n++] = Cmd{CmdKind::sc_restore, 0, 0, 0};
    out[n++] = Cmd{CmdKind::om_restore, 0, 0, 0};
    return n;
}

// ================================================================================================== per-frame state (producer, one per recorded slot)
// One briefing frame = the game command list that holds the frame's SelMap draw. The FIRST SelMap draw of a list opens the frame (needs the eye
// poses, the latched fit and placement); later draws of the same list reuse its eye matrices. The fit is latched once per briefing screen (the grid
// is static, the game camera orbits: the table must not move with it).
struct FrameState {
    bool open = false, eyes_ok = false, cleared = false;
    Mat4 eye[2];
    unsigned selmap = 0, icons = 0, replays = 0, refused = 0;
    const char* last_why = nullptr;
    void begin() noexcept { *this = FrameState{}; open = true; }
    void refuse(const char* why) noexcept { ++refused; last_why = why ? why : "brief3d-refused"; }
};
// Opens the frame's eye matrices. nullptr = eyes_ok. The eye guard refuses BOTH eyes when either is inside / too close to the table volume (one eye
// alone would be worse than none: the consumer then shows no table this frame).
inline const char* open_eyes(FrameState& fs, const Fit& f, const Box& box, const Placement& pl, const EyeView eyes[2], const float cb10[2],
                             const EyeCfg& cfg) noexcept {
    fs.eyes_ok = false;
    const Volume v = volume(box, f);
    for (unsigned e = 0; e < 2; ++e) {
        if (const char* why = eye_matrix(f, pl, eyes[e], cb10, cfg, fs.eye[e])) return why;
        if (!eye_clear_of_volume(v, mul(table_to_local(pl), view_from_eye(eyes[e])), cfg.near_m)) return "brief3d-eye-inside-volume";
    }
    fs.eyes_ok = true;
    return nullptr;
}

// ================================================================================================== log lines (< 459 bytes for any values)
inline constexpr size_t kLineLimit = 480, kLinePrefix = 21;
inline int format_fit(char* buf, size_t n, const Box& b, const Fit& f, const char* why) noexcept {
    auto cl = [](double v) { return v > 1e9 ? 1e9 : v < -1e9 ? -1e9 : v; };
    return std::snprintf(buf, n, "brief3d-fit;outcome=%s;reason=%.32s;vertices=%u;box=%.6g,%.6g,%.6g..%.6g,%.6g,%.6g;up=%d%c;a1=%d;a2=%d%c;m-per-unit=%.4g;vexag=%.3g",
        f.valid ? "ok" : "refused", why ? why : "none", b.vertices, cl(b.lo[0]), cl(b.lo[1]), cl(b.lo[2]), cl(b.hi[0]), cl(b.hi[1]), cl(b.hi[2]),
        f.up, f.up_sign > 0 ? '+' : f.up_sign < 0 ? '-' : '?', f.a1, f.a2, f.a2_sign > 0 ? '+' : f.a2_sign < 0 ? '-' : '?', cl(f.metres_per_unit), f.vexag);
}
// The zoom-aware content fit changed (brief3d.md 9.1): latched content top (model units above the fit origin), the uniform table scale, the
// resulting content height and table width in metres, the cap, and how many changes so far.
inline int format_content(char* buf, size_t n, double top_units, double scale, const Fit& f, double max_m, double width_m,
                          unsigned long long changes) noexcept {
    auto cl = [](double v) { return v > 1e9 ? 1e9 : v < -1e9 ? -1e9 : v; };
    // brief3d 9.2: the scale is VERTICAL only (squashed_fit): the table keeps its width.
    return std::snprintf(buf, n, "brief3d-content;top-units=%.4g;scale=%.4g;mode=vertical;height-m=%.3g;width-m=%.3g;max-m=%.3g;changes=%llu",
        cl(top_units), cl(scale), cl(top_units * f.metres_per_unit * f.vexag * scale), cl(width_m), cl(max_m),
        changes > 9999999ull ? 9999999ull : changes);
}
inline int format_frame(char* buf, size_t n, uint64_t frame, const FrameState& s, unsigned skipped) noexcept {
    auto cu = [](unsigned v) { return v > 99999u ? 99999u : v; };
    return std::snprintf(buf, n, "brief3d-frame;frame=%llu;selmap=%u;icons=%u;replays=%u;refused=%u;eyes=%d;cleared=%d;last-why=%.32s;skipped=%u;"
        "mode=gpu-replay(per-eye,t10-root-srv-swap,game-pso-ia-t11-b10)",
        static_cast<unsigned long long>(frame), cu(s.selmap), cu(s.icons), cu(s.replays), cu(s.refused), s.eyes_ok ? 1 : 0, s.cleared ? 1 : 0,
        s.last_why ? s.last_why : "none", cu(skipped));
}

} // namespace acz::brief3d
