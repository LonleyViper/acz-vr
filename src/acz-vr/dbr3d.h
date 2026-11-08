#pragma once
// dbr3d.h -- DEBRIEF-3D (docs/world-render/debrief-3d-map.md sections 7 and 8). SDK-free, header-only CPU core. Nothing here touches D3D12 or
// OpenXR. Fixture success says nothing about the live game or a headset.
//
// STAGE 2 (2026-10-30, section 8; the LIVE route, glue in brief3d_live.h Producer::dbr3d_draw + ui3d_replay_hooks.inl): the screenshot of the
// debrief (live-1936-step137 png-screen, debrief-upright-small.png) shows that the "map window" of the census is the MISSION RESULT panel (90 % x
// 64 % of the 4:3 picture, the same rectangle), drawn OVER a perspective wireframe grid with the flight paths (pre-projected by the game's own
// debrief camera). So the debrief frame is split in game order: every draw BEFORE the frame's first non-indexed GS-mapped draw (the indexed /
// pixel-space map draws: grid, paths, labels) is the MAP SCENE and is re-rendered per eye on a world-locked WRAPAROUND (a cylinder of radius dbrr
// about the head at the vscreen placement, spread dbrk times the flat screen's angles); from that draw on (the panel quad, its text batches, the
// narration line) the OVERLAY goes on the UI3D floating-panel layers (u3s-scaled vscreen, menu layer distances). Both use the ui3d surface code
// (surface_point / rewrite_at / eye_scissor_at); indexed draws are replayed with the game's index buffer and a copy of the referenced vertex range.
// The section below ("stage 1") is the earlier table core (the window as a horizontal table); it is kept with its tests but is NOT used live.
//
// STAGE 1 (section 7): the post-mission debriefing map window laid on a TABLE in front of the user, its 2D content re-rendered per eye on the
// table plane (sprite markers lifted above it): options, the online draw-role assigner, the window latch, the table geometry, the per-vertex
// re-projection of a ps2_v draw onto the table, the eye guard and the upload budget.
//
// Facts this core relies on [E: live-1508 census, D:\acz-evidence-archive\headset-2026-10-30-session\live-1508\ee-camera.log, 60 frames
// 167,655-167,716 s, the screen the user left after finishing m03k; debrief-3d-map.md 7.1]:
//  * every draw of the screen is VS 534f29ca (ps2_v) / PS d34ee153: no SelMap, ModelIcon, ACM, movie or other key (other=0 in 60/60 frames);
//    the vertices are pre-projected GS screen coordinates (x, y, z, q), so there is no world/model matrix to substitute: the 3D presentation
//    has to be built from the 2D image (this file), not replayed through a camera (brief3d's route).
//  * a MAP WINDOW quad in every frame (60/60): non-indexed, 4 vertices, GS x 1817..2278, y 1880..2168, z 16,777,216, q 1, blend
//    SRC_ALPHA/INV_SRC_ALPHA, depth ALWAYS without write, cb0 (682, -448, 2048, 2048) = 90 % x 64 % of the 4:3 picture; the narration line
//    below it (glyph quads at GS y 2180..2204, z 1) and the pixel-space draws (cb0 2560, -1440, 1280, 720) are not part of the map.
//  * the map content (indexed, GS space, additive SRC_ALPHA/ONE, depth ALWAYS) fits triangle strips with one cut marker per strip [I, from
//    index and vertex counts only: n = 4 k + (k - 1) for every batch]: sprite batches (11..312 quads) and 3..11 long strips (75..121 vertices).
//    Their vertex values (inside the window? q = 1?) and the IA topology are [U]: the census GEO lines (ui3d_census.h) record them next run.
// Every rule below that depends on a [U] fails closed: a vertex outside the latched window keeps its draw on the flat screen, a varying or
// non-unit q (an EE-projected perspective view, not a flat map) refuses the frame, an unknown shader key refuses the frame.
//
// Sub-options of xr_terrain_stage (no new config key; the 32-key cap is full). Default OFF. Removed before the other parsers.
//   dbr3d:<0|1>   1 = the debrief in 3D (stage 2: map scene on the wraparound, results panel on the UI3D layers). Needs vscreen:1 + vsearly:1 +
//                 xr_hud_layer + ui3d:1 (it rides on the UI3D frames, ring and consumer).
//   dbrr:<m>      stage 2: wraparound radius (horizontal distance from the head at the vscreen placement), default 4.0 (1.5..20)
//   dbrk:<x>      stage 2: wraparound angular spread (x the flat screen's angles about its centre), default 1.4 (0.5..2.5)
//   dbrw:<m>      stage 1 (not live): table width (the window's long side), default 1.0 (0.2..4)
//   dbrd:<m>      stage 1 (not live): table centre distance in front of the head at placement, default 0.75 (0.2..5)
//   dbry:<m>      stage 1 (not live): table top height relative to the head at placement, default -0.45 (-2.5..1)
//   dbrl:<m>      stage 1 (not live): height of MARKER draws (small sprite batches) above the table top, default 0.03 (0..0.3)
#include "brief3d.h"
#include "ui3d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace acz::dbr3d {

// ================================================================================================== constants
inline constexpr uint32_t kPs2Vs8 = 0x534f29cau, kMenuPs8 = 0xd34ee153u;   // the only key of the debrief screen [E]
inline constexpr uint32_t kBlendSrcAlpha = 5, kBlendInvSrcAlpha = 6;          // D3D12_BLEND
inline constexpr float kWindowZMin = 1.6e7f;          // the window quad's z = 16,777,216 [E]; glyphs z 1, map draws unknown
inline constexpr double kWindowMinW = 0.6, kWindowMinH = 0.4;   // window size as fractions of the 4:3 picture (live: 0.90 x 0.64) [E]
inline constexpr double kPixelSpaceSx = 1024.0;       // |cb0.sx| at or above this = the pixel-space mapping (2560) [E]; GS mapping 682 / 512
inline constexpr double kInsideTolGs = 1.0;           // a vertex this far outside the latched window (GS units) still counts as inside
inline constexpr double kLatchTolGs = 2.0;            // a window quad this far from the latched one = a new screen (re-place the table)
inline constexpr double kMarkerMaxFrac = 0.12;        // a draw whose bbox is at most this fraction of the window in BOTH axes = a MARKER [I]
inline constexpr double kQRefM = 1.0;                 // q' = q kQRefM / w_eye (perspective-correct attributes on the table; any constant)
inline constexpr double kNearM = 0.05;
inline constexpr double kGuardMarginM = 0.10;         // headroom above the marker height for the eye guard
inline constexpr uint32_t kVertexStride = 64;         // ps2_v vertex: 4 x float4 [E]
inline constexpr uint32_t kMaxVerticesPerDraw = 4096; // live max 1,248 [E]
inline constexpr uint64_t kUploadPerSlot = 2ull << 20;   // two eyes x 16,384 vertices x 64 B (live frame ~3,600 table vertices [E])

// ================================================================================================== options
struct Options {
    bool on = false;
    float width_m = 1.0f, dist_m = 0.75f, y_m = -0.45f, lift_m = 0.03f;
    float radius_m = 4.0f, spread = 1.4f;   // stage 2 wraparound
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
// Removes every dbr3d token from `value` (the first item, the stage id, is never an option); a value without them comes back byte for byte.
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
                if (k == "dbr3d") { if (v == "1") o.on = true; else if (v == "0") o.on = false; else { why = "bad value for dbr3d (0|1)"; return false; } }
                else if (k == "dbrw") { if (!detail::num(v, 0.2, 4.0, d)) { why = "bad value for dbrw (0.2..4 m)"; return false; } o.width_m = float(d); }
                else if (k == "dbrd") { if (!detail::num(v, 0.2, 5.0, d)) { why = "bad value for dbrd (0.2..5 m)"; return false; } o.dist_m = float(d); }
                else if (k == "dbry") { if (!detail::num(v, -2.5, 1.0, d)) { why = "bad value for dbry (-2.5..1 m)"; return false; } o.y_m = float(d); }
                else if (k == "dbrl") { if (!detail::num(v, 0.0, 0.3, d)) { why = "bad value for dbrl (0..0.3 m)"; return false; } o.lift_m = float(d); }
                else if (k == "dbrr") { if (!detail::num(v, 1.5, 20.0, d)) { why = "bad value for dbrr (1.5..20 m)"; return false; } o.radius_m = float(d); }
                else if (k == "dbrk") { if (!detail::num(v, 0.5, 2.5, d)) { why = "bad value for dbrk (0.5..2.5)"; return false; } o.spread = float(d); }
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
// nullptr = could run in this configuration; else the missing prerequisite (the vscreen / XR side).
inline const char* prerequisites(const Options& o, bool vscreen_on, bool vsearly_on, bool hud_layer) noexcept {
    if (!o.on) return "off";
    if (!vscreen_on) return "needs-vscreen:1";
    if (!vsearly_on) return "needs-vsearly:1";
    if (!hud_layer) return "needs-xr_hud_layer";
    return nullptr;
}
// Stage 2 (live): the XR prerequisites, then UI3D configured (ui3d:1 with its own prerequisites) and the present hook (ee_camera_probe).
inline const char* live_prerequisites(const Options& o, bool vscreen_on, bool vsearly_on, bool hud_layer, bool ui3d_configured, bool present_hook) noexcept {
    if (const char* why = prerequisites(o, vscreen_on, vsearly_on, hud_layer)) return why;
    if (!ui3d_configured) return "needs-ui3d:1";
    if (!present_hook) return "needs-ee_camera_probe(present-hook)";
    return nullptr;
}
inline std::string describe(const Options& o) {
    char t[160];
    std::snprintf(t, sizeof t, "dbr3d=%u;dbrw=%.2f;dbrd=%.2f;dbry=%.2f;dbrl=%.3f;dbrr=%.2f;dbrk=%.2f", o.on ? 1u : 0u, double(o.width_m), double(o.dist_m),
                  double(o.y_m), double(o.lift_m), double(o.radius_m), double(o.spread));
    return t;
}

// ================================================================================================== one draw as the assigner sees it
// v: the draw's vertices (x, y, z, q; the non-indexed draw's own, or every vertex an indexed draw references); nv = 0 = unread (fails closed).
struct DrawIn {
    uint32_t vs8 = 0, ps8 = 0;
    bool indexed = false;
    acz::ui3d::Cb0 cb;
    bool blend = false; uint32_t src = 0, dst = 0;
    const float (*v)[4] = nullptr; uint32_t nv = 0;
};
inline bool pixel_space(const acz::ui3d::Cb0& c) noexcept { return std::fabs(double(c.sx)) >= kPixelSpaceSx; }
struct GsRect { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool valid = false; };
// x/y bbox of the vertices; false = none or a non-finite / non-positive q vertex.
inline bool bbox(const DrawIn& d, GsRect& r) noexcept {
    if (!d.v || !d.nv) return false;
    for (uint32_t i = 0; i < d.nv; ++i) {
        const float* f = d.v[i];
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3]) || !(f[3] > 0.0f)) return false;
        if (!i) { r.x0 = r.x1 = f[0]; r.y0 = r.y1 = f[1]; }
        else { r.x0 = std::min(r.x0, double(f[0])); r.x1 = std::max(r.x1, double(f[0])); r.y0 = std::min(r.y0, double(f[1])); r.y1 = std::max(r.y1, double(f[1])); }
    }
    r.valid = true;
    return true;
}
// The MAP WINDOW quad (signature of the live census, every rule [E]): non-indexed 4 vertices on two x and two y values, GS mapping, alpha blend
// SRC_ALPHA / INV_SRC_ALPHA, z >= kWindowZMin, q = 1, at least kWindowMinW x kWindowMinH of the 4:3 picture.
inline bool is_window_quad(const DrawIn& d, GsRect& out) noexcept {
    if (d.vs8 != kPs2Vs8 || d.ps8 != kMenuPs8 || d.indexed || d.nv != 4 || !d.v) return false;
    if (!acz::ui3d::cb0_ok(d.cb) || pixel_space(d.cb)) return false;
    if (!d.blend || d.src != kBlendSrcAlpha || d.dst != kBlendInvSrcAlpha) return false;
    GsRect r;
    if (!bbox(d, r)) return false;
    for (uint32_t i = 0; i < 4; ++i) {
        const float* f = d.v[i];
        if (!(f[2] >= kWindowZMin) || f[3] != 1.0f) return false;
        if ((f[0] != float(r.x0) && f[0] != float(r.x1)) || (f[1] != float(r.y0) && f[1] != float(r.y1))) return false;   // axis aligned
    }
    const double pic_w = 0.75 * std::fabs(double(d.cb.sx)), pic_h = std::fabs(double(d.cb.sy));   // the 4:3 picture in GS units (512 x 448)
    if (!((r.x1 - r.x0) >= kWindowMinW * pic_w) || !((r.y1 - r.y0) >= kWindowMinH * pic_h)) return false;
    out = r;
    return true;
}

// ================================================================================================== the window latch (one table per screen)
struct Latch {
    GsRect rect; acz::ui3d::Cb0 cb;
    uint32_t screens = 0;       // window rectangles latched so far (a new one = a new screen: the table is placed again)
    bool place_pending = false; // set by a new rectangle; the glue places the table with the next pose and clears it
};
// true = this quad latched a NEW rectangle (or the first one)
inline bool latch_window(Latch& l, const GsRect& r, const acz::ui3d::Cb0& cb) noexcept {
    if (l.rect.valid && std::fabs(r.x0 - l.rect.x0) <= kLatchTolGs && std::fabs(r.x1 - l.rect.x1) <= kLatchTolGs &&
        std::fabs(r.y0 - l.rect.y0) <= kLatchTolGs && std::fabs(r.y1 - l.rect.y1) <= kLatchTolGs && cb.sx == l.cb.sx && cb.sy == l.cb.sy &&
        cb.ox == l.cb.ox && cb.oy == l.cb.oy)
        return false;
    l.rect = r; l.rect.valid = true; l.cb = cb; ++l.screens; l.place_pending = true;
    return true;
}

// ================================================================================================== the online role assigner (per game frame)
enum class Role : uint8_t { flat, window, surface, marker, refused };
inline const char* role_name(Role r) noexcept {
    return r == Role::flat ? "flat" : r == Role::window ? "window" : r == Role::surface ? "surface" : r == Role::marker ? "marker" : "refused";
}
// flat    = not on the table (stays on the flat screen only: pixel-space draws, the narration text, decorations straddling the window border,
//           anything before the first window quad was ever latched)
// window  = the window quad itself: the table top (h = 0)
// surface = GS content wholly inside the latched window: on the table top (h = 0)
// marker  = surface content with a small bbox: lifted by dbrl
// refused = the frame is not a debrief frame / cannot be laid flat (fail closed; why())
struct Assigner {
    Latch latch;
    const char* why_ = nullptr;
    uint32_t on_table = 0, flat = 0;
    void begin_frame() noexcept { why_ = nullptr; on_table = flat = 0; }
    const char* why() const noexcept { return why_; }
    Role refuse(const char* w) noexcept { why_ = w; return Role::refused; }
    Role assign(const DrawIn& d) noexcept {
        if (why_) return Role::refused;   // the frame is already refused
        if (d.vs8 != kPs2Vs8) return refuse("dbr3d-frame-has-other-shader");
        if (d.ps8 != kMenuPs8) return refuse("dbr3d-frame-has-other-ps");
        if (!acz::ui3d::cb0_ok(d.cb)) return refuse("dbr3d-cb0");
        if (pixel_space(d.cb)) { ++flat; return Role::flat; }
        GsRect w;
        if (is_window_quad(d, w)) { latch_window(latch, w, d.cb); ++on_table; return Role::window; }
        if (!latch.rect.valid) { ++flat; return Role::flat; }
        if (d.cb.sx != latch.cb.sx || d.cb.sy != latch.cb.sy || d.cb.ox != latch.cb.ox || d.cb.oy != latch.cb.oy) { ++flat; return Role::flat; }
        GsRect r;
        if (!d.nv || !d.v) return refuse("dbr3d-vertices-unread");
        if (!bbox(d, r)) return refuse("dbr3d-vertex-invalid");
        const GsRect& L = latch.rect;
        const bool inside = r.x0 >= L.x0 - kInsideTolGs && r.x1 <= L.x1 + kInsideTolGs && r.y0 >= L.y0 - kInsideTolGs && r.y1 <= L.y1 + kInsideTolGs;
        if (!inside) { ++flat; return Role::flat; }
        // a flat map image only: one q for every vertex of the draw, and q = 1 (an EE-projected perspective view would be laid flat wrongly)
        for (uint32_t i = 0; i < d.nv; ++i) if (d.v[i][3] != 1.0f) return refuse("dbr3d-perspective-content");
        ++on_table;
        const double fw = (r.x1 - r.x0) / std::max(1e-9, L.x1 - L.x0), fh = (r.y1 - r.y0) / std::max(1e-9, L.y1 - L.y0);
        return fw <= kMarkerMaxFrac && fh <= kMarkerMaxFrac ? Role::marker : Role::surface;
    }
};

// ================================================================================================== the table (LOCAL space)
// The latched window as a rectangle of the shown image (u, v in -1..1, v up) and the physical table it becomes: width = dbrw along the
// window's x, depth from the window's aspect ON THE SHOWN IMAGE (image aspect = rt_w / rt_h), table top at the placement's centre height.
struct Table {
    bool valid = false;
    acz::brief3d::Placement pl;
    acz::brief3d::Mat4 to_local;
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;   // the window on the shown image
    double width = 0, depth = 0;
    acz::ui3d::Cb0 cb; acz::ui3d::SourceRect src;
};
inline const char* make_table(const Options& o, const Latch& l, const acz::ui3d::SourceRect& src, double image_aspect, const double head_pos[3],
                              const double head_q[4], uint32_t space_generation, Table& t) noexcept {
    t = Table{};
    if (!l.rect.valid) return "dbr3d-no-window";
    if (!acz::ui3d::rect_ok(src)) return "dbr3d-viewport";
    if (!(image_aspect > 0.1) || !std::isfinite(image_aspect)) return "dbr3d-image-aspect";
    double a, b, c, e;
    acz::ui3d::gs_to_screen(l.rect.x0, l.rect.y0, l.cb, src, a, b);
    acz::ui3d::gs_to_screen(l.rect.x1, l.rect.y1, l.cb, src, c, e);
    t.u0 = std::min(a, c); t.u1 = std::max(a, c); t.v0 = std::min(b, e); t.v1 = std::max(b, e);
    if (!(t.u1 - t.u0 > 1e-4) || !(t.v1 - t.v0 > 1e-4)) return "dbr3d-window-degenerate";
    t.width = double(o.width_m);
    t.depth = t.width * (t.v1 - t.v0) / ((t.u1 - t.u0) * image_aspect);
    acz::brief3d::Options bo; bo.width_m = o.width_m; bo.dist_m = o.dist_m; bo.y_m = o.y_m;
    t.pl = acz::brief3d::place(bo, head_pos, head_q, space_generation);
    if (!t.pl.valid) return "dbr3d-placement-invalid";
    t.to_local = acz::brief3d::table_to_local(t.pl);
    t.cb = l.cb; t.src = src;
    t.valid = std::isfinite(t.depth) && t.depth > 1e-3;
    return t.valid ? nullptr : "dbr3d-window-degenerate";
}
// Shown-image point (u, v) of the window at height h above the table top -> LOCAL. The window's top edge (v1) is the table's far edge.
inline void table_point(const Table& t, double u, double v, double h, double out[3]) noexcept {
    const double s = (u - t.u0) / (t.u1 - t.u0), r = (v - t.v0) / (t.v1 - t.v0);
    const double p[3] = {(s - 0.5) * t.width, h, -(r - 0.5) * t.depth};
    double q[4]; acz::brief3d::xform(t.to_local, p, q);
    out[0] = q[0]; out[1] = q[1]; out[2] = q[2];
}
inline double role_height(const Options& o, Role r) noexcept { return r == Role::marker ? double(o.lift_m) : 0.0; }
inline acz::ui3d::EyeView eye_view(const acz::brief3d::EyeView& e) noexcept {
    acz::ui3d::EyeView v;
    for (int i = 0; i < 3; ++i) v.p[i] = e.position[i];
    for (int i = 0; i < 4; ++i) { v.q[i] = e.orientation[i]; v.fov[i] = e.fov[i]; }
    return v;
}
// Every corner of the table volume (the top, and the marker height + margin) at least kNearM in front of the eye.
inline bool eye_clear(const Table& t, const Options& o, const acz::ui3d::EyeView& eye) noexcept {
    if (!t.valid) return false;
    const double hs[2] = {0.0, double(o.lift_m) + kGuardMarginM};
    for (int i = 0; i < 8; ++i) {
        double P[3]; table_point(t, (i & 1) ? t.u1 : t.u0, (i & 2) ? t.v1 : t.v0, hs[(i >> 2) & 1], P);
        double x, y, w;
        if (!acz::ui3d::project(eye, P, x, y, w)) return false;
    }
    return true;
}

// ================================================================================================== one draw's vertex copy for one eye
struct RewriteResult { const char* why = nullptr; bool culled = false; };
// src: `n` vertices (64 B each, as the game's VB holds them: the non-indexed draw's range, or the whole referenced range of an indexed draw;
// the index buffer is reused unchanged). dst: the same layout for the eye replay. Bytes 8..11 (z) and 16..63 are copied unchanged; x, y, q
// are rewritten so the game's own shader puts each vertex where the table point projects: x' = ox + ndc_x sx / 2 (ps2_v's mapping inverted;
// NDC does not depend on q), and q' = q kQRefM / w_eye (clip w = 1 / q', so attributes interpolate perspective-correctly across the table).
// why = the frame must be refused; culled = a vertex is behind / too near this eye: the glue records NO draw for this eye.
inline RewriteResult rewrite_vertices(const uint8_t* src, uint32_t n, const Table& t, const Options& o, Role role, const acz::ui3d::EyeView& eye,
                                      uint8_t* dst) noexcept {
    RewriteResult res;
    if (!src || !dst || n == 0) { res.why = "dbr3d-no-vertices"; return res; }
    if (n > kMaxVerticesPerDraw) { res.why = "dbr3d-draw-too-many-vertices"; return res; }
    if (!t.valid) { res.why = "dbr3d-table-invalid"; return res; }
    if (role != Role::window && role != Role::surface && role != Role::marker) { res.why = "dbr3d-role-not-on-table"; return res; }
    const double h = role_height(o, role);
    std::memcpy(dst, src, size_t(n) * kVertexStride);
    for (uint32_t i = 0; i < n; ++i) {
        float f[4]; std::memcpy(f, src + size_t(i) * kVertexStride, 16);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3])) { res.why = "dbr3d-vertex-nonfinite"; return res; }
        if (!(f[3] > 0.0f)) { res.why = "dbr3d-vertex-q"; return res; }
        double u, v; acz::ui3d::gs_to_screen(f[0], f[1], t.cb, t.src, u, v);
        double P[3]; table_point(t, u, v, h, P);
        double nx, ny, w;
        if (!acz::ui3d::project(eye, P, nx, ny, w)) { res.culled = true; continue; }
        const float x2 = float(double(t.cb.ox) + nx * double(t.cb.sx) * 0.5), y2 = float(double(t.cb.oy) + ny * double(t.cb.sy) * 0.5);
        const float q2 = float(double(f[3]) * kQRefM / w);
        if (!std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(q2) || !(q2 > 0.0f)) { res.culled = true; continue; }
        std::memcpy(dst + size_t(i) * kVertexStride + 0, &x2, 4);
        std::memcpy(dst + size_t(i) * kVertexStride + 4, &y2, 4);
        std::memcpy(dst + size_t(i) * kVertexStride + 12, &q2, 4);
    }
    return res;
}
// Upload bytes of one frame (both eyes) and the per-slot cap check.
inline uint64_t upload_bytes(uint64_t table_vertices) noexcept { return table_vertices * 2u * kVertexStride; }
inline bool upload_fits(uint64_t table_vertices) noexcept { return upload_bytes(table_vertices) <= kUploadPerSlot; }

// ================================================================================================== statistics / summary line
struct Stats {
    uint64_t frames = 0, tabled = 0, refused = 0, flat_only = 0, draws_on_table = 0, draws_flat = 0, screens = 0;
    const char* last_why = nullptr;
};
// After a frame: on_table == 0 and no refusal = a frame with no window (flat only).
inline void count_frame(Stats& s, const Assigner& a) noexcept {
    ++s.frames;
    if (a.why()) { ++s.refused; s.last_why = a.why(); }
    else if (a.on_table) ++s.tabled;
    else ++s.flat_only;
    s.draws_on_table += a.on_table; s.draws_flat += a.flat; s.screens = a.latch.screens;
}
inline std::string summary_line(const Stats& s) {
    char t[256];
    std::snprintf(t, sizeof t, "summary-dbr3d;frames=%llu;tabled=%llu;refused=%llu;flat-only=%llu;table-draws=%llu;flat-draws=%llu;screens=%llu;last-why=%s",
                  static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.tabled), static_cast<unsigned long long>(s.refused),
                  static_cast<unsigned long long>(s.flat_only), static_cast<unsigned long long>(s.draws_on_table),
                  static_cast<unsigned long long>(s.draws_flat), static_cast<unsigned long long>(s.screens), s.last_why ? s.last_why : "none");
    return t;
}

// ==================================================================================================================================================
// STAGE 2: the live route (debrief-3d-map.md 8). Used by brief3d_live.h Producer::dbr3d_draw.
// ==================================================================================================================================================
inline constexpr int64_t kArmMs = 1000;                  // the debrief window quad seen within this long = ps2_v draws take the debrief route
inline constexpr uint32_t kMaxIndicesPerDraw = 8192;     // live max 1,559 indices [E census]; more = the frame is refused
inline constexpr uint32_t kStripCutDisabled = 0, kStripCut16 = 1, kStripCut32 = 2;   // D3D12_INDEX_BUFFER_STRIP_CUT_VALUE

// The debrief window quad from one draw's 64-byte vertices (the arming signature: is_window_quad on the census values). n must be 4.
inline bool window_from_vertices(uint32_t vs8, uint32_t ps8, bool indexed, const acz::ui3d::Cb0& cb, bool blend, uint32_t src, uint32_t dst,
                                 const uint8_t* vtx, uint32_t n, GsRect& out) noexcept {
    if (!vtx || n != 4) return false;
    float v[4][4];
    for (uint32_t i = 0; i < 4; ++i) std::memcpy(v[i], vtx + size_t(i) * kVertexStride, 16);
    const float (&cv)[4][4] = v;
    DrawIn d; d.vs8 = vs8; d.ps8 = ps8; d.indexed = indexed; d.cb = cb; d.blend = blend; d.src = src; d.dst = dst; d.v = cv; d.nv = 4;
    return is_window_quad(d, out);
}

// ------------------------------------------------------------------------------------------------ index range of an indexed draw
// The referenced vertex range [imin, imax] of `n` indices (stride 2 or 4) with the PSO's strip-cut value (cut markers are skipped, never fetched).
// The replay copies exactly that range and draws the game's index buffer with BaseVertexLocation = -imin (vertex i of the draw = copy[i - imin]).
struct IndexScan { const char* why = nullptr; uint32_t imin = 0, imax = 0, cuts = 0, used = 0; uint32_t span() const noexcept { return imax - imin + 1; } };
inline IndexScan scan_indices(const uint8_t* ib, uint32_t n, uint32_t stride, uint32_t cut) noexcept {
    IndexScan s;
    if (stride != 2 && stride != 4) { s.why = "dbr3d-ib-stride"; return s; }
    if (cut != kStripCutDisabled && cut != kStripCut16 && cut != kStripCut32) { s.why = "dbr3d-strip-cut-unknown"; return s; }
    if ((cut == kStripCut16 && stride != 2) || (cut == kStripCut32 && stride != 4)) { s.why = "dbr3d-strip-cut-mismatch"; return s; }
    if (!ib || n == 0) { s.why = "dbr3d-no-indices"; return s; }
    if (n > kMaxIndicesPerDraw) { s.why = "dbr3d-too-many-indices"; return s; }
    const uint32_t marker = cut == kStripCut16 ? 0xFFFFu : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = 0;
        if (stride == 2) { uint16_t h; std::memcpy(&h, ib + size_t(i) * 2, 2); x = h; } else std::memcpy(&x, ib + size_t(i) * 4, 4);
        if (cut != kStripCutDisabled && x == marker) { ++s.cuts; continue; }
        if (!s.used) { s.imin = s.imax = x; } else { s.imin = std::min(s.imin, x); s.imax = std::max(s.imax, x); }
        ++s.used;
    }
    if (!s.used) { s.why = "dbr3d-only-cuts"; return s; }
    if (uint64_t(s.imax) - s.imin + 1 > kMaxVerticesPerDraw) { s.why = "dbr3d-index-span"; return s; }
    return s;
}

// ------------------------------------------------------------------------------------------------ the frame split (game order, no look-ahead)
// MAP (scene): indexed or pixel-space draws before the first non-indexed GS-mapped draw. PANEL (overlay): that draw and every later draw, on the
// UI3D layers (0 far / 1 panels / 2 text) by size; a batch of more than 4 vertices (the result-table text: 312 quads in one indexed draw [E]) is
// classed by its largest 4-vertex quad. The layer never decreases within a frame (painter's order consistent with depth, as ui3d LayerAssigner).
enum class Route : uint8_t { map, panel };
struct RouteIn {
    bool indexed = false, pixel_space = false, window = false;
    uint32_t nv = 0;
    double bbox_w = 0, bbox_h = 0;   // ui3d::screen_bbox (fractions of the shown image)
    double quad_max = 0;             // quad_max_dim (fractions; batches only)
};
class Router {
public:
    void begin_frame() noexcept { overlay_ = false; layer_ = 0; map_ = panel_ = 0; }
    Route route(const RouteIn& d, unsigned& layer) noexcept {
        layer = 0;
        if (!overlay_ && (d.indexed || d.pixel_space)) { ++map_; return Route::map; }
        overlay_ = true;
        unsigned cls;
        const bool batch = d.nv > 4;
        if (d.window) cls = 0;
        else if (!(d.bbox_w > 0.0) || !(d.bbox_h > 0.0)) cls = layer_;   // degenerate: draws nothing, never promotes
        else if (!batch && d.bbox_w * d.bbox_h >= acz::ui3d::kLargeArea) cls = 0;
        else if (!batch ? (d.bbox_w <= acz::ui3d::kSmallDim && d.bbox_h <= acz::ui3d::kSmallDim) : (d.quad_max > 0.0 && d.quad_max <= acz::ui3d::kSmallDim)) cls = 2;
        else cls = 1;
        layer_ = std::max(layer_, cls);
        ++panel_;
        layer = layer_;
        return Route::panel;
    }
    bool overlay() const noexcept { return overlay_; }
    unsigned map_draws() const noexcept { return map_; }
    unsigned panel_draws() const noexcept { return panel_; }
private:
    bool overlay_ = false;
    unsigned layer_ = 0, map_ = 0, panel_ = 0;
};
// The largest side (fraction of the shown image) over the 4-vertex groups of a batch (strips of 4 [I census]); n not a multiple of 4 = the whole bbox.
inline double quad_max_dim(const uint8_t* src, uint32_t n, const acz::ui3d::Cb0& cb, const acz::ui3d::SourceRect& r) noexcept {
    double w = 0, h = 0;
    if (!src || n == 0) return 0.0;
    if (n % 4) return acz::ui3d::screen_bbox(src, n, cb, r, w, h) ? std::max(w, h) : 0.0;
    double m = 0.0;
    for (uint32_t i = 0; i < n; i += 4) {
        if (!acz::ui3d::screen_bbox(src + size_t(i) * kVertexStride, 4, cb, r, w, h)) return 0.0;
        m = std::max(m, std::max(w, h));
    }
    return m;
}

// ------------------------------------------------------------------------------------------------ the wraparound (map scene) surface
// The vscreen placement (unscaled) with the cylinder rule: the point of a source pixel lies on the ray anchor -> screen(u k, v k) at horizontal
// distance dbrr from the anchor (ui3d::surface_point with curve). World-locked: the vscreen placement is (it moves only on a re-placement).
inline acz::ui3d::Options wrap_options(const acz::ui3d::Options& base) noexcept { acz::ui3d::Options o = base; o.curve = true; return o; }
// The panel's layer options: the menu layer distances (u3mn / u3mf, as the stage-2 menus), with the far layer pulled in front of the wraparound
// (at most kPanelFarFrac of dbrr) so the panel never lies behind the map it covers (stereo depth agrees with the painter's order).
inline constexpr double kPanelFarFrac = 0.75;
inline acz::ui3d::Options panel_options(const acz::ui3d::Options& base, const Options& o) noexcept {
    acz::ui3d::Options p = base;
    p.near_m = base.menu_near_m;
    const double far_m = std::min(double(base.menu_far_m), kPanelFarFrac * double(o.radius_m));
    p.far_m = float(std::max(far_m, double(p.near_m) + 0.05));
    return p;
}

// The per-eye vertex copy of a draw of up to kMaxVerticesPerDraw vertices on an explicit surface: ui3d::rewrite_at in chunks of its 64-vertex cap
// (every vertex is rewritten independently: x, y, q; z and attributes unchanged). why = unusable (fail closed); culled = some vertex is behind /
// too near this eye. (review 2026-10-31 F2) `cull` (optional, n bytes) names those vertices: the producer then draws only the primitives without
// one (cull_primitives) instead of dropping the whole draw for this eye.
inline acz::ui3d::RewriteResult rewrite_surface(const uint8_t* src, uint32_t n, const acz::ui3d::Cb0& cb, const acz::ui3d::SourceRect& r,
                                                const acz::ui3d::Screen& s, const acz::ui3d::Options& o, double D, double k,
                                                const acz::ui3d::EyeView& eye, uint8_t* dst, uint8_t* cull = nullptr) noexcept {
    acz::ui3d::RewriteResult res;
    if (!src || !dst || n == 0) { res.why = "dbr3d-no-vertices"; return res; }
    if (n > kMaxVerticesPerDraw) { res.why = "dbr3d-draw-too-many-vertices"; return res; }
    for (uint32_t off = 0; off < n; off += acz::ui3d::kMaxVerticesPerDraw) {
        const uint32_t m = std::min<uint32_t>(acz::ui3d::kMaxVerticesPerDraw, n - off);
        const acz::ui3d::RewriteResult c = acz::ui3d::rewrite_at(src + size_t(off) * kVertexStride, m, cb, r, s, o, D, k, eye, dst + size_t(off) * kVertexStride,
                                                                 cull ? cull + off : nullptr);
        if (c.why) return c;
        res.culled = res.culled || c.culled;
    }
    return res;
}

// ------------------------------------------------------------------------------------------------ per-primitive culling of one eye (review 2026-10-31 F2)
// A wraparound map draw spans the whole map: when the head turns, one end of it lies behind an eye (w < kNearM) while the rest is in view. Dropping
// the whole draw for that eye (the stage-2 rule) made the map vanish from that eye. Instead the eye draws an index list of its own that keeps every
// primitive whose vertices are all in front (as BRIEF3D-LATCH plan_grid), drawn as the LIST topology of the game's IA topology from our upload span.
// D3D_PRIMITIVE_TOPOLOGY values of the game's IASetPrimitiveTopology (bindings.topology); 0 = unknown.
inline constexpr uint32_t kTopoPointList = 1, kTopoLineList = 2, kTopoLineStrip = 3, kTopoTriList = 4, kTopoTriStrip = 5;
inline constexpr uint32_t kMaxListIndices = 3 * kMaxIndicesPerDraw;   // a strip of n indices gives 3 (n - 2) list indices (n >= kMaxVerticesPerDraw too)
static_assert(kMaxListIndices >= 3 * kMaxVerticesPerDraw, "a non-indexed strip of the per-draw vertex cap must fit");
static_assert(kMaxVerticesPerDraw <= 0xFFFFu, "the eye lists are R16 indices into the eye copy");
// The list topology our draw uses for the game's topology `t` (0 = not supported: the eye keeps the whole-draw cull).
inline uint32_t list_topology(uint32_t t) noexcept {
    return t == kTopoPointList ? kTopoPointList : (t == kTopoLineList || t == kTopoLineStrip) ? kTopoLineList
         : (t == kTopoTriList || t == kTopoTriStrip) ? kTopoTriList : 0u;
}
struct EyeList { const char* why = nullptr; uint32_t count = 0, kept = 0, dropped = 0; };
// The primitives of one draw that use no culled vertex, as R16 indices into the eye copy (vertex 0 of the copy = draw vertex imin, or the draw's
// first vertex when not indexed). indexed: `ib` = the game's n indices (stride 2 / 4) with the PSO's strip-cut value (a cut restarts assembly, the
// partial primitive is discarded); not indexed: the n vertices 0..n-1. cull[v] (nv bytes): vertex v of the copy is culled for this eye. Strip
// triangles keep the strip's winding (odd triangles (b, a, c)). why = malformed (an index outside the copy, an unknown topology, out over cap).
inline EyeList cull_primitives(bool indexed, const uint8_t* ib, uint32_t stride, uint32_t cut, uint32_t n, uint32_t imin, uint32_t nv, uint32_t topology,
                               const uint8_t* cull, uint16_t* out, uint32_t cap) noexcept {
    EyeList L;
    const uint32_t lt = list_topology(topology);
    if (!lt) { L.why = "dbr3d-topology-unknown"; return L; }
    if (!cull || !out || nv == 0 || nv > kMaxVerticesPerDraw) { L.why = "dbr3d-list-args"; return L; }
    if (indexed && (!ib || (stride != 2 && stride != 4))) { L.why = "dbr3d-ib-stride"; return L; }
    const bool cutOn = indexed && cut != kStripCutDisabled;
    const uint32_t marker = cut == kStripCut16 ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t b[2] = {0, 0}; unsigned have = 0; uint32_t k = 0;   // assembly state: pending vertices, triangles since the last restart
    auto emit = [&](const uint32_t* v, unsigned m) -> bool {
        for (unsigned j = 0; j < m; ++j) if (cull[v[j]]) { ++L.dropped; return true; }
        if (L.count + m > cap) { L.why = "dbr3d-list-cap"; return false; }
        for (unsigned j = 0; j < m; ++j) out[L.count++] = uint16_t(v[j]);
        ++L.kept; return true;
    };
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t v = i;
        if (indexed) {
            uint32_t x = 0;
            if (stride == 2) { uint16_t h; std::memcpy(&h, ib + size_t(i) * 2, 2); x = h; } else std::memcpy(&x, ib + size_t(i) * 4, 4);
            if (cutOn && x == marker) { have = 0; k = 0; continue; }
            if (x < imin || x - imin >= nv) { L.why = "dbr3d-index-outside-copy"; return L; }
            v = x - imin;
        } else if (v >= nv) { L.why = "dbr3d-index-outside-copy"; return L; }
        bool ok = true;
        switch (topology) {
        case kTopoPointList: ok = emit(&v, 1); break;
        case kTopoLineList: if (have == 0) { b[0] = v; have = 1; } else { const uint32_t s[2] = {b[0], v}; ok = emit(s, 2); have = 0; } break;
        case kTopoLineStrip: if (have == 0) { b[0] = v; have = 1; } else { const uint32_t s[2] = {b[0], v}; ok = emit(s, 2); b[0] = v; } break;
        case kTopoTriList:
            if (have < 2) b[have++] = v;
            else { const uint32_t s[3] = {b[0], b[1], v}; ok = emit(s, 3); have = 0; }
            break;
        default:   // kTopoTriStrip
            if (have < 2) { b[have++] = v; break; }
            { const uint32_t s[3] = {(k & 1u) ? b[1] : b[0], (k & 1u) ? b[0] : b[1], v}; ok = emit(s, 3); ++k; b[0] = b[1]; b[1] = v; }
            break;
        }
        if (!ok) return L;
    }
    return L;
}

// ------------------------------------------------------------------------------------------------ unreferenced vertices of an indexed draw (F4)
// The copy of an indexed draw is the whole range [imin, imax]; a vertex inside it that no index references (a gap: sparse indices into a shared
// buffer) may hold anything (stale, zero: q = 0). Every vertex of the copy is validated, measured (bbox, quad size) and rewritten, so a gap used to
// refuse the WHOLE frame (ui3d-vertex-q). The glue replaces each gap with the nearest referenced vertex before it (vertex 0 of the copy = imin is
// always referenced): never fetched by the GPU, inside the draw's own bounding box. Returns the number of gaps filled (0 when none or bad input).
inline uint32_t fill_unreferenced(uint8_t* vtx, uint32_t nv, const uint8_t* ib, uint32_t n, uint32_t stride, uint32_t cut, uint32_t imin) noexcept {
    if (!vtx || !ib || nv == 0 || nv > kMaxVerticesPerDraw || (stride != 2 && stride != 4)) return 0;
    bool ref[kMaxVerticesPerDraw] = {};
    const bool cutOn = cut != kStripCutDisabled;
    const uint32_t marker = cut == kStripCut16 ? 0xFFFFu : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = 0;
        if (stride == 2) { uint16_t h; std::memcpy(&h, ib + size_t(i) * 2, 2); x = h; } else std::memcpy(&x, ib + size_t(i) * 4, 4);
        if (cutOn && x == marker) continue;
        if (x >= imin && x - imin < nv) ref[x - imin] = true;
    }
    if (!ref[0]) return 0;   // imin not referenced: not the scan's range, leave the copy as read
    uint32_t filled = 0, last = 0;
    for (uint32_t v = 0; v < nv; ++v) {
        if (ref[v]) { last = v; continue; }
        std::memcpy(vtx + size_t(v) * kVertexStride, vtx + size_t(last) * kVertexStride, kVertexStride);
        ++filled;
    }
    return filled;
}

// ------------------------------------------------------------------------------------------------ the producer's debrief statistics (bounded line)
// clipped (F2): per-eye draws drawn with their own culled index list; gaps (F4): unreferenced vertices filled in indexed copies.
struct LiveStats {
    uint64_t arms = 0, frames = 0, usable = 0, published = 0, rejected = 0, map_draws = 0, panel_draws = 0, indexed = 0, vertices = 0, culled = 0,
             refused_draws = 0, clipped = 0, gaps = 0;
    const char* last_why = nullptr;
};
inline int format_live(char* buf, size_t n, const LiveStats& s, bool armed, uint64_t screens) noexcept {
    auto c = [](uint64_t v) { return static_cast<unsigned long long>(v > 999999999ull ? 999999999ull : v); };
    return std::snprintf(buf, n,
        "summary-dbr3d;armed=%u;arms=%llu;screens=%llu;frames=%llu;usable=%llu;published=%llu;rejected=%llu;map-draws=%llu;panel-draws=%llu;indexed=%llu;"
        "vertices=%llu;culled=%llu;clipped=%llu;gaps=%llu;refused-draws=%llu;last-why=%.32s",
        armed ? 1u : 0u, c(s.arms), c(screens), c(s.frames), c(s.usable), c(s.published), c(s.rejected), c(s.map_draws), c(s.panel_draws), c(s.indexed),
        c(s.vertices), c(s.culled), c(s.clipped), c(s.gaps), c(s.refused_draws), s.last_why ? s.last_why : "none");
}

} // namespace acz::dbr3d
