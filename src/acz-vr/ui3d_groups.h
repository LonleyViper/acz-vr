#pragma once
// UI3D 2026-10-31: stages 2 and 3 of the 3D reinterpretation of the ACZ menus ("glass and amber", docs/world-render/ui-3d.md 13.4 and 14).
// SDK-free, header-only, pure: the panel groups of a frame, the placement surface (pane) of a draw, the shape a draw is replayed as (flat quad,
// rim, slab, raised text) and the stage-3 colour shading of the replayed copies. The producer (brief3d_live.h Producer::ui3d_draw) uses it only
// when an option of ui3d.h `Options::reinterpret()` is on; otherwise the stage-1 path runs unchanged.
//
// Rules (ui-3d.md 13.4): (R1) every element of one panel lies on one surface; (R2) depth comes from STRUCTURE (panel groups, nesting, the
// selection), never from a per-draw size class; (R3) from the anchor every replayed point lies on the ray anchor -> its flat-screen point, so the
// composite is exactly the flat image on every screen; (R4) any unknown structure falls back to the flat base surface (the text plane, = u3pp:1).
//
// Stage 2 (u3gp:1). Panel rectangles are the PREVIOUS UI3D frame's frame quads (ui3d.h frame_quad: one medium 4-vertex quad at the panel
// priority z > 1; the census [E: runK10 hangar overlay 3,371 quads, step124 runD1 menus] shows every panel / frame / tile is one such quad with
// the outline in its texture). No look-ahead exists at a draw (the replay is recorded before the game's draw and a panel's quad precedes its
// text), so the previous frame's rectangles are the reference (the SelectionRows precedent: a screen change costs one frame on stale groups,
// still on the right rays). A draw joins the SMALLEST rectangle that contains its box centre; its pane is that group's plane:
//   * a top-level rectangle (contained in no other) gets the tangent plane of a vertical cylinder about the anchor (radius = the text distance)
//     at its centre's yaw x u3gk: the panels wrap gently around the viewer while every panel stays flat (text sharp);
//   * a nested rectangle takes its top-level ancestor's orientation, u3gn x its nesting depth (<= kMaxNest) in front of it;
//   * a draw in no rectangle stays on the base surface (the stage-1 text plane / cylinder).
// The selection bar is never a group rectangle (it moves with the cursor).
//
// Stage 3 (each its own option): u3bv shades the rim / slab walls that face down or right on the flat image (COLOR0.rgb x (1 - u3bv), the
// game's own shader lit by its vertex colour: ATTRIBUTE1 = COLOR0, float4 at bytes 16..31 [E: ps2_v dxc disassembly, ui-3d.md section 1]);
// u3tx raises the text in front of its pane; u3sr raises the selection slab IN FRONT of its pane (back face on the pane) and the text inside the
// frame's selection box rides on its front face; u3pb draws a rim's back face (u3lt behind the pane) with COLOR0.a x u3pb.
#include "ui3d.h"

namespace acz::ui3d::groups {

inline constexpr unsigned kMaxGroups = 48;      // census: the busiest hangar overlay has ~12 frame quads per frame, the main menu ~20 [E: K10 / D1]
inline constexpr unsigned kMaxNest = 4;         // nesting depth cap (panel > box > tile > ...)
inline constexpr double kContainTol = 0.004;    // containment tolerance (fraction of the image; ~10 px of 2560): a tile touching its panel's border
inline constexpr double kSameTol = 0.002;       // two rectangles this close on every edge are the same panel (drawn twice: fill + outline)
// the producer keeps this much of a slot's upload span free of multi-copy shapes (128 flat 4-vertex quads, both eyes), so rims / slabs never
// crowd out the rest of a frame (the main menu: 347 draws ~ 178 KiB flat of the 512 KiB span [E: census runD1])
inline constexpr uint64_t kShapeReserve = 64ull * 1024ull;

struct Group {
    Box b;
    unsigned level = 0;   // number of rectangles containing it (capped at kMaxNest)
    int top = -1;         // the largest rectangle containing it (itself when top-level)
};
inline double area(const Box& b) noexcept { return b.w() * b.h(); }
inline bool contains(const Box& outer, const Box& inner, double tol) noexcept {
    return inner.x0 >= outer.x0 - tol && inner.x1 <= outer.x1 + tol && inner.y0 >= outer.y0 - tol && inner.y1 <= outer.y1 + tol;
}
inline bool same(const Box& a, const Box& b) noexcept {
    return std::fabs(a.x0 - b.x0) <= kSameTol && std::fabs(a.x1 - b.x1) <= kSameTol && std::fabs(a.y0 - b.y0) <= kSameTol && std::fabs(a.y1 - b.y1) <= kSameTol;
}

class PanelGroups {
public:
    void clear() noexcept { n_ = 0; dropped_ = 0; }
    unsigned size() const noexcept { return n_; }
    unsigned dropped() const noexcept { return dropped_; }
    const Group& at(unsigned i) const noexcept { return g_[i < n_ ? i : 0]; }
    // A panel rectangle of the current frame (a frame quad that is not the selection bar). Duplicates merge; beyond kMaxGroups it is counted.
    void add(const Box& b) noexcept {
        if (!(b.w() > 0) || !(b.h() > 0) || !std::isfinite(b.x0 + b.x1 + b.y0 + b.y1)) return;
        for (unsigned i = 0; i < n_; ++i) if (same(g_[i].b, b)) return;
        if (n_ >= kMaxGroups) { ++dropped_; return; }
        g_[n_].b = b; g_[n_].level = 0; g_[n_].top = int(n_); ++n_;
    }
    // Nesting levels and top-level ancestors (once, when the frame's set is complete: the producer calls it when the set becomes the reference).
    void finish() noexcept {
        for (unsigned i = 0; i < n_; ++i) {
            unsigned lv = 0; int top = int(i); double ta = area(g_[i].b);
            for (unsigned j = 0; j < n_; ++j) {
                if (j == i || !(area(g_[j].b) > area(g_[i].b)) || !contains(g_[j].b, g_[i].b, kContainTol)) continue;
                ++lv;
                if (area(g_[j].b) > ta) { ta = area(g_[j].b); top = int(j); }
            }
            g_[i].level = lv < kMaxNest ? lv : kMaxNest; g_[i].top = top;
        }
    }
    // The smallest rectangle containing the point (x right, y down, fractions of the shown image); -1 = none.
    int find(double x, double y) const noexcept {
        int best = -1; double ba = 0;
        for (unsigned i = 0; i < n_; ++i) {
            const Box& b = g_[i].b;
            if (x < b.x0 - kContainTol || x > b.x1 + kContainTol || y < b.y0 - kContainTol || y > b.y1 + kContainTol) continue;
            if (best < 0 || area(b) < ba) { best = int(i); ba = area(b); }
        }
        return best;
    }
private:
    Group g_[kMaxGroups];
    unsigned n_ = 0, dropped_ = 0;
};

// The yaw (radians, positive = towards the screen's right) of the direction anchor -> the flat-screen point of a box centre.
inline double centre_yaw(const Screen& s, const Box& b) noexcept {
    const double u = (b.x0 + b.x1) - 1.0, v = 1.0 - (b.y0 + b.y1);   // 2 x centre - 1 (x), 1 - 2 x centre (y)
    double S[3]; screen_point(s, u, v, S);
    const double dir[3] = {S[0] - s.anchor[0], S[1] - s.anchor[1], S[2] - s.anchor[2]};
    return std::atan2(dot3(dir, s.right), dot3(dir, s.fwd));
}
// A group plane: normal = fwd turned by `yaw` about the vertical (towards right), tilted by u3t as the layer planes; distance D from the anchor
// along that normal. yaw 0 gives exactly the layer plane's arithmetic (byte-identical to layer_surf with u3c:0).
inline Surf plane_surf(const Screen& s, const Options& o, double D, double yaw) noexcept {
    Surf f; f.cyl = false; f.D = D; f.k = 1.0;
    const double tr = double(o.tilt_deg) * 3.14159265358979323846 / 180.0;
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    for (int i = 0; i < 3; ++i) {
        const double h = cy * s.fwd[i] + sy * s.right[i];
        f.n[i] = std::cos(tr) * h - std::sin(tr) * s.up[i];
    }
    f.ct = std::cos(tr);
    return f;
}

// The pane of a draw: its group's surface (or the base surface).
struct Pane {
    Surf s;
    int group = -1;       // -1 = the base surface
    unsigned level = 0;
};
inline double text_distance(const Options& o) noexcept { return layer_distance(o, kLayers - 1); }
inline Pane pane_for(const Screen& s, const Options& o, const PanelGroups* g, const Box& box) noexcept {
    Pane p;
    const double D = text_distance(o);
    p.s = layer_surf(s, o, D, 1.0);   // the base surface: the text layer (u3pp:1)
    if (!g || !o.groups) return p;
    const int i = g->find(0.5 * (box.x0 + box.x1), 0.5 * (box.y0 + box.y1));
    if (i < 0) return p;
    const Group& G = g->at(unsigned(i));
    const Group& T = g->at(unsigned(G.top >= 0 ? G.top : i));
    const double yaw = double(o.curve_k) * centre_yaw(s, T.b);
    p.s = plane_surf(s, o, D - double(G.level) * double(o.nest_mm) * 1e-3, yaw);
    p.group = i; p.level = G.level;
    return p;
}

// What a draw is replayed as. copies: 1 = the front surface only; kRimCopies = 4 walls + front; kSlabCopies = back + 4 walls + front.
enum class Role : unsigned { flat = 0, rim, selection, text };
struct Shape {
    Role role = Role::flat;
    Surf front, back;
    unsigned copies = 1;
};
inline Shape shape_flat(const Pane& p) noexcept { Shape h; h.front = p.s; h.back = p.s; return h; }
// A frame quad: front on the pane, walls u3lt behind it; u3pb > 0 adds the back face (a pane with visible thickness).
inline Shape shape_rim(const Options& o, const Pane& p) noexcept {
    Shape h = shape_flat(p);
    if (!(o.frame_mm > 0.0f)) return h;
    h.role = Role::rim;
    h.back = surf_at(p.s, p.s.D + double(o.frame_mm) * 1e-3);
    h.copies = o.back_alpha > 0.0f ? kSlabCopies : kRimCopies;
    return h;
}
// The selection bar: u3sr > 0 = a slab standing u3sr in front of its pane (back face on the pane); 0 = the u3sel rule (front u3sg behind the
// pane / text plane, back u3sd further). u3sel:1 = the front face only.
inline Shape shape_selection(const Options& o, const Pane& p, uint32_t vertices) noexcept {
    Shape h; h.role = Role::selection;
    if (o.sel_raise_mm > 0.0f) {
        h.front = surf_at(p.s, p.s.D - double(o.sel_raise_mm) * 1e-3);
        h.back = p.s;
    } else {
        h.front = surf_at(p.s, p.s.D + double(o.sel_gap_mm) * 1e-3);
        h.back = surf_at(h.front, h.front.D + double(o.sel_depth_mm) * 1e-3);
    }
    h.copies = o.select == 2 && vertices == 4 ? kSlabCopies : 1u;
    return h;
}
// Text (a non-frame draw on the text layer): u3tx in front of its pane; inside the frame's raised selection box: on the slab's front face, u3tx
// in front of it.
inline Shape shape_text(const Options& o, const Pane& p, bool on_selection) noexcept {
    Shape h = shape_flat(p); h.role = Role::text;
    double D = p.s.D - double(o.text_mm) * 1e-3;
    if (on_selection && o.sel_raise_mm > 0.0f) D -= double(o.sel_raise_mm) * 1e-3;
    h.front = h.back = surf_at(p.s, D);
    return h;
}
// The upload fallback: one step down (6 -> 5 for a rim: no back face; 6 -> 1 for a slab; 5 -> 1). false = already a single copy.
inline bool downgrade(Shape& h) noexcept {
    if (h.copies <= 1) return false;
    if (h.role == Role::rim && h.copies == kSlabCopies) { h.copies = kRimCopies; return true; }
    h.copies = 1; h.back = h.front;
    return true;
}

// One eye's copies of a draw for a shape (dst: copies x n vertices).
inline RewriteResult rewrite_shape(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Shape& h,
                                   const EyeView& eye, uint8_t* dst) noexcept {
    if (h.copies == 1) return rewrite_surf(src, n, cb, r, s, h.front, eye, dst);
    if (h.copies == kSlabCopies) return rewrite_slab_surf(src, n, cb, r, s, h.front, h.back, eye, dst);
    if (h.copies == kRimCopies) {
        uint8_t slab[kSlabCopies * 4 * kVertexStride];
        const RewriteResult res = rewrite_slab_surf(src, n, cb, r, s, h.front, h.back, eye, slab);
        if (!res.why) std::memcpy(dst, slab + 4 * size_t(kVertexStride), kRimCopies * 4 * size_t(kVertexStride));
        return res;
    }
    RewriteResult res; res.why = "ui3d-shape-copies"; return res;
}

// Stage 3 shading of one eye's copies (after rewrite_shape). Walls (rim: copies 0..3, slab: 1..4, edge e = kSlabEdges[e]) whose outward edge
// faces DOWN or RIGHT on the flat image get COLOR0.rgb x (1 - bevel); a slab's back face (copy 0) gets COLOR0.a x back_alpha when back_alpha > 0
// and the shape is a rim (a selection slab's back face is left as the game's colour). A copy whose COLOR0 is not finite is left unchanged
// (counted in `skipped`). From the anchor the walls are edge-on and the back face is hidden behind the front: the flat image is unchanged.
struct ShadeCount { unsigned walls = 0, back = 0, skipped = 0; };
inline bool scale_colour(uint8_t* quad, unsigned first, unsigned count, float f) noexcept {
    for (unsigned v = 0; v < 4; ++v) for (unsigned c = first; c < first + count; ++c) {
        float x; std::memcpy(&x, quad + v * kVertexStride + 16 + 4 * c, 4);
        if (!std::isfinite(x)) return false;
    }
    for (unsigned v = 0; v < 4; ++v) for (unsigned c = first; c < first + count; ++c) {
        float x; std::memcpy(&x, quad + v * kVertexStride + 16 + 4 * c, 4);
        x *= f;
        std::memcpy(quad + v * kVertexStride + 16 + 4 * c, &x, 4);
    }
    return true;
}
// Which of the 4 edges (kSlabEdges) face down or right on the flat image (from the source vertices).
inline unsigned shaded_edges(const uint8_t* src, const Cb0& cb, const SourceRect& r) noexcept {
    double u[4], v[4], cu = 0, cv = 0;
    for (unsigned i = 0; i < 4; ++i) {
        float f[2]; std::memcpy(f, src + i * kVertexStride, 8);
        gs_to_screen(f[0], f[1], cb, r, u[i], v[i]);
        cu += 0.25 * u[i]; cv += 0.25 * v[i];
    }
    unsigned mask = 0;
    for (unsigned e = 0; e < 4; ++e) {
        const unsigned a = kSlabEdges[e][0], b = kSlabEdges[e][1];
        const double du = 0.5 * (u[a] + u[b]) - cu, dv = 0.5 * (v[a] + v[b]) - cv;
        const bool down = dv < 0 && std::fabs(dv) >= std::fabs(du), right = du > 0 && std::fabs(du) > std::fabs(dv);
        if (down || right) mask |= 1u << e;
    }
    return mask;
}
inline ShadeCount shade(uint8_t* dst, const uint8_t* src, const Cb0& cb, const SourceRect& r, const Shape& h, float bevel, float back_alpha) noexcept {
    ShadeCount n;
    if (h.copies != kRimCopies && h.copies != kSlabCopies) return n;
    const unsigned w0 = h.copies == kSlabCopies ? 1u : 0u;
    const size_t q = 4 * size_t(kVertexStride);
    if (bevel > 0.0f) {
        const unsigned mask = shaded_edges(src, cb, r);
        for (unsigned e = 0; e < 4; ++e) if (mask & (1u << e)) {
            if (scale_colour(dst + q * (w0 + e), 0, 3, 1.0f - bevel)) ++n.walls; else ++n.skipped;
        }
    }
    if (h.role == Role::rim && h.copies == kSlabCopies && back_alpha > 0.0f) {
        if (scale_colour(dst, 3, 1, back_alpha)) ++n.back; else ++n.skipped;
    }
    return n;
}

} // namespace acz::ui3d::groups
