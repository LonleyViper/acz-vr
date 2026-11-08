#pragma once
// tactical_producer.h -- TACTICAL (tac:1) pure helpers used on the game's render thread by the step-5 producer (gpu_copy_step5_hooks.inl) and by the
// offline tests. Header only, SDK-free, no allocation, bounded loops. docs/world-render/tactical.md.
#include "tactical_frame.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::tac {

// ---- world draws
// Mesh signature of one draw: what stays the same for one model part from frame to frame (counts and layout, not addresses) [INFERENCE].
inline uint32_t mesh_signature(bool indexed, uint32_t count, uint32_t vb_stride, uint32_t topology) noexcept {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xffu; h *= 16777619u; } };
    mix(indexed ? 1u : 0u); mix(count); mix(vb_stride); mix(topology);
    return h ? h : 1u;
}
// The record's model translation (+256 B: floats 76..78) and the rows of its 3x3 (floats 64..74). raw = the 416-byte record.
inline void object_from_record(const uint8_t* raw, ObjDraw& o) noexcept {
    for (int k = 0; k < 3; ++k) std::memcpy(&o.t[k], raw + 4 * (76 + k), 4);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) std::memcpy(&o.axes[r * 3 + c], raw + 4 * (64 + r * 4 + c), 4);
}
// A (record +0, row vectors: h = (p, 1) x A) applied to a world point; GS screen = h.xy / h.w. false = behind the camera / not finite.
inline bool project_ps2(const float A[16], const float p[3], float& xs, float& ys, float& w) noexcept {
    double h[4];
    for (int j = 0; j < 4; ++j) h[j] = double(p[0]) * A[j] + double(p[1]) * A[4 + j] + double(p[2]) * A[8 + j] + A[12 + j];
    w = float(h[3]);
    if (!(h[3] > 1e-6) || !std::isfinite(h[0]) || !std::isfinite(h[1])) return false;
    xs = float(h[0] / h[3]); ys = float(h[1] / h[3]);
    return std::isfinite(xs) && std::isfinite(ys);
}

// ---- marker sprites (ps2_v vertices: 4 x float4, ATTRIBUTE0 = GS x, y, z, w; ATTRIBUTE1 = colour)
constexpr unsigned kMaxSpriteVerts = 512;     // more vertices in one marker draw: unreadable (counted), never truncated silently
constexpr float kBoxLink = 20.0f;             // GS units: vertices closer than this (both axes) belong to one box (box side ~15 GS units, step62 shot)
constexpr float kLabelLink = 10.0f;           // glyph quads of one label (glyph advance ~5.6 GS units)
constexpr float kLabelReach = 30.0f;          // a label cluster within this gap of a box belongs to it (the label sits above-left of its box)
// x, y = attribute 0 as stored (GS units; NOT divided by q: VS 534f29ca divides the whole position by q, which cancels in the perspective divide,
// markers-360.md section 17.1); q = attribute 0 w as stored.
struct SpriteVert { float x = 0, y = 0, rgba[4] = {}; float q = 1; };
struct Cluster { float x0 = 0, y0 = 0, x1 = 0, y1 = 0, rgb[3] = {}; uint32_t n = 0; };
// Working storage for one marker draw (kept in the producer's heap state, never on the game thread's stack: ~16 KB).
struct SpriteScratch {
    SpriteVert v[kMaxSpriteVerts];
    uint16_t parent[kMaxSpriteVerts];
    bool use[kMaxSpriteVerts];
    int slot[kMaxSpriteVerts];
    Cluster c[kMarkCap];
};
// Single-linkage clustering (union-find over at most kMaxSpriteVerts vertices; O(n^2) worst case = 262k comparisons, bounded). Returns the cluster count
// (<= cap; the rest are dropped and counted in `dropped`). Vertices with non-finite or off-window coordinates (|x-2048| > 1024) are ignored.
inline unsigned cluster_vertices(SpriteScratch& sc, unsigned n, float link, uint32_t& dropped) noexcept {
    dropped = 0;
    if (n > kMaxSpriteVerts) n = kMaxSpriteVerts;
    const SpriteVert* v = sc.v; uint16_t* parent = sc.parent; bool* use = sc.use; int* slot_of = sc.slot; Cluster* out = sc.c;
    const unsigned cap = kMarkCap;
    for (unsigned i = 0; i < n; ++i) {
        parent[i] = uint16_t(i);
        use[i] = std::isfinite(v[i].x) && std::isfinite(v[i].y) && std::fabs(v[i].x - 2048.0f) <= 1024.0f && std::fabs(v[i].y - 2048.0f) <= 1024.0f;
    }
    auto find = [&](unsigned a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    for (unsigned i = 0; i < n; ++i) if (use[i]) for (unsigned j = i + 1; j < n; ++j) if (use[j])
        if (std::fabs(v[i].x - v[j].x) <= link && std::fabs(v[i].y - v[j].y) <= link) {
            const unsigned a = find(i), b = find(j);
            if (a != b) parent[a > b ? a : b] = uint16_t(a < b ? a : b);
        }
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) slot_of[i] = -1;
    for (unsigned i = 0; i < n; ++i) {
        if (!use[i]) continue;
        const unsigned r = find(i);
        if (slot_of[r] == -1) {
            if (count >= cap) { ++dropped; slot_of[r] = -2; continue; }
            slot_of[r] = int(count);
            Cluster& c = out[count++];
            c = Cluster{}; c.x0 = c.x1 = v[i].x; c.y0 = c.y1 = v[i].y;
        }
        if (slot_of[r] < 0) continue;
        Cluster& c = out[slot_of[r]];
        c.x0 = std::fmin(c.x0, v[i].x); c.x1 = std::fmax(c.x1, v[i].x); c.y0 = std::fmin(c.y0, v[i].y); c.y1 = std::fmax(c.y1, v[i].y);
        for (int k = 0; k < 3; ++k) if (std::isfinite(v[i].rgba[k])) c.rgb[k] += v[i].rgba[k];
        ++c.n;
    }
    return count;
}
inline void normalise_rgb(float rgb[3]) noexcept {
    const float m = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2]));
    if (m > 0.0f && std::isfinite(m)) for (int k = 0; k < 3; ++k) rgb[k] = std::fmax(0.0f, rgb[k] / m);
    else rgb[0] = rgb[1] = rgb[2] = 1.0f;
}
// The HUD tail restarts at a cockpit member (gpu_copy_step5_hooks.inl step5TacMember): the markers collected so far are discarded (nmark = 0), and so
// are the facts ABOUT them: clusters dropped and box / label draws unreadable before the restart say nothing about the boxes kept after it. Headset test 9
// (markers-360.md section 22): those facts were kept, so a drop before the restart (the frame's sprites averaged ~1 marker while mark_dropped > 0: the
// drops were never in the final list) marked the frame unsafe on 79 % (test 9) to 100 % (step180 null) of frames, which switched off the in-window miss
// rule (marker360.h update step 5): a contact the game stopped boxing in view (a kill, a retarget) kept its identity and its cached game box outside the
// window (wrecks with boxes). unread_box / unread_label (the status totals) keep the discarded counts.
inline void restart_marker_tail(TacFrame& f, uint64_t& unread_box, uint64_t& unread_label) noexcept {
    unread_box += f.box_unreadable; unread_label += f.label_unreadable;
    f.nmark = 0; f.mark_dropped = 0; f.box_unreadable = 0; f.label_unreadable = 0; f.lock_shapes = 0;
}
// Box clusters -> MarkerSprites appended to f (after the ones already there).
inline void add_boxes(TacFrame& f, const Cluster* c, unsigned n) noexcept {
    for (unsigned i = 0; i < n; ++i) {
        if (f.nmark >= kMarkCap) { ++f.mark_dropped; continue; }
        MarkerSprite& m = f.mark[f.nmark++];
        m = MarkerSprite{};
        m.cx = 0.5f * (c[i].x0 + c[i].x1); m.cy = 0.5f * (c[i].y0 + c[i].y1);
        m.hw = 0.5f * (c[i].x1 - c[i].x0); m.hh = 0.5f * (c[i].y1 - c[i].y0);
        for (int k = 0; k < 3; ++k) m.rgb[k] = c[i].n ? c[i].rgb[k] / float(c[i].n) : 0.0f;
        normalise_rgb(m.rgb);
        m.verts = c[i].n;
    }
}
// Label clusters -> glyph counts on the nearest box of f.mark[first..nmark) (gap between the rectangles <= kLabelReach). 4 vertices per glyph quad.
// NBOX / FRND fix (markers-360.md section 12): the label rectangle (union) and the label's OWN colour are kept on the box. Association stays purely
// geometric: the game's labels need not share the box colour (original-game reference, user report item 14: a RED enemy box with a GREEN "F-4E" type
// label at its upper right and a green range "3138" below), so colour never steers which box a label belongs to.
// box_of (optional, n entries): the f.mark index each cluster joined, -1 = none (label_suppress_ok).
inline void add_labels(TacFrame& f, unsigned first, const Cluster* c, unsigned n, int* box_of = nullptr) noexcept {
    for (unsigned i = 0; i < n; ++i) {
        if (box_of) box_of[i] = -1;
        float lrgb[3] = {c[i].n ? c[i].rgb[0] / float(c[i].n) : 0.0f, c[i].n ? c[i].rgb[1] / float(c[i].n) : 0.0f, c[i].n ? c[i].rgb[2] / float(c[i].n) : 0.0f};
        normalise_rgb(lrgb);
        float best = kLabelReach; int bi = -1;
        for (unsigned k = first; k < f.nmark; ++k) {
            const MarkerSprite& m = f.mark[k];
            if (m.shape != 0) continue;   // LOCK-SHAPE: a diamond never carries a label (box draws only)
            const float gx = std::fmax(0.0f, std::fmax((m.cx - m.hw) - c[i].x1, c[i].x0 - (m.cx + m.hw)));
            const float gy = std::fmax(0.0f, std::fmax((m.cy - m.hh) - c[i].y1, c[i].y0 - (m.cy + m.hh)));
            const float g = std::fmax(gx, gy);
            if (g <= best) { best = g; bi = int(k); }
        }
        const uint32_t q = (c[i].n + 2) / 4;
        if (bi >= 0 && q) {   // a stray 1-vertex cluster (q = 0) is no label
            MarkerSprite& m = f.mark[bi];
            if (!m.glyphs) { m.lx0 = c[i].x0; m.ly0 = c[i].y0; m.lx1 = c[i].x1; m.ly1 = c[i].y1; }
            else { m.lx0 = std::fmin(m.lx0, c[i].x0); m.ly0 = std::fmin(m.ly0, c[i].y0); m.lx1 = std::fmax(m.lx1, c[i].x1); m.ly1 = std::fmax(m.ly1, c[i].y1); }
            m.glyphs = uint8_t(q > 255 ? 255 : q);
            for (int k = 0; k < 3; ++k) m.lrgb[k] = lrgb[k];
            if (box_of) box_of[i] = bi;
        }
    }
}
// LOCK-FX (markers-360.md section 11): which label text the consumer can redraw. A 4-glyph label is "FRND" only in the game's friendly blue (every
// logged g4 label is blue [E step104l/step105 mk360-contacts]); a 4-glyph label in another colour (LOCK / MISS style text) is NOT FRND and must stay the
// game's. A 3-glyph label is TGT. Any other glyph count is text we cannot redraw (ranges, names, lock / miss text).
inline bool label_text_redrawable(unsigned glyphs, const float rgb[3]) noexcept {
    if (glyphs == 3) return true;
    if (glyphs != 4) return false;
    return rgb[2] >= 0.6f && rgb[2] > rgb[0] + 0.3f && rgb[2] > rgb[1] + 0.3f;
}
// Every cluster of a label draw is redrawable text (the draw may be suppressed); an empty draw is not.
inline bool label_draw_redrawable(const Cluster* c, unsigned n) noexcept {
    if (!n) return false;
    for (unsigned i = 0; i < n; ++i) {
        float rgb[3] = {c[i].n ? c[i].rgb[0] / float(c[i].n) : 0.0f, c[i].n ? c[i].rgb[1] / float(c[i].n) : 0.0f, c[i].n ? c[i].rgb[2] / float(c[i].n) : 0.0f};
        normalise_rgb(rgb);
        if (!label_text_redrawable((c[i].n + 2) / 4, rgb)) return false;
    }
    return true;
}
// Codex wave-4 review J-201 (docs/review/review-w4-2-marker-lock-miss.md, P1 x2): a label draw may be hidden (mk360:1 / tacboxes:1) ONLY when hiding is
// paired with a replacement for every one of its pixels: the preceding box draw was hidden (box_hidden), the label draw was READ completely (read_ok,
// no cluster dropped by the cap), it has >= 1 cluster, every cluster is redrawable text (label_text_redrawable) AND joined one of THAT box draw's
// markers (box_of in [first, nmark)), and no marker received two clusters (the consumer draws one label per box). Anything else stays the game's draw.
// NBOX mk360:3 hides no box, so box_hidden is never true there.
inline bool label_suppress_ok(bool box_hidden, bool read_ok, const Cluster* c, unsigned n, uint32_t dropped, const int* box_of, unsigned first,
                              unsigned nmark) noexcept {
    if (!box_hidden || !read_ok || dropped || !n || !label_draw_redrawable(c, n)) return false;
    for (unsigned i = 0; i < n; ++i) {
        if (box_of[i] < int(first) || box_of[i] >= int(nmark)) return false;   // out of reach: no box would carry its replacement
        for (unsigned j = 0; j < i; ++j) if (box_of[j] == box_of[i]) return false;   // two texts on one box: only one replacement
    }
    return true;
}
// Matches f.mark[first..nmark) to the frame's world draws: the nearest projected draw translation within the box (half extents x 1.5 + 4 GS units).
// Returns true when EVERY marker in the range matched (then the draw holding them may be suppressed).
inline bool match_markers(TacFrame& f, unsigned first) noexcept {
    if (!f.a_valid) return false;
    bool all = f.nmark > first;
    for (unsigned k = first; k < f.nmark; ++k) {
        MarkerSprite& m = f.mark[k];
        const float rx = m.hw * 1.5f + 4.0f, ry = m.hh * 1.5f + 4.0f;
        float best = 1e30f; int bi = -1;
        for (unsigned i = 0; i < f.nobj; ++i) {
            float xs, ys, w;
            if (!project_ps2(f.A, f.obj[i].t, xs, ys, w)) continue;
            const float dx = std::fabs(xs - m.cx), dy = std::fabs(ys - m.cy);
            if (dx > rx || dy > ry) continue;
            const float d = dx * dx + dy * dy;
            if (d < best) { best = d; bi = int(i); }
        }
        if (bi >= 0) { m.obj = uint8_t(bi + 1); if (!f.obj[bi].marker) f.obj[bi].marker = uint8_t(k + 1); }
        else all = false;
    }
    return all;
}

// Whether a HUD-tail box draw is suppressed (not replayed; the consumer draws its markers). lost = clusters of THIS draw not kept (the per-draw cluster
// cap or the frame's kMarkCap): never suppressed then (a box we could not keep would vanish). kProducerHideAll (mk360:1): every draw with >= 1 kept
// cluster; kProducerSuppress (tacboxes:1): only when every kept marker matched a replayed world draw.
inline bool suppress_box_draw(uint32_t flags, bool all_matched, unsigned kept, uint32_t lost) noexcept {
    if (lost || !kept) return false;
    if (flags & kProducerHideAll) return true;
    return (flags & kProducerSuppress) && all_matched;
}

// ---- LOCK-SHAPE (markers-360.md sections 16.4 / 17.4): the lock diamond is a HUD INSTRUMENT draw (not depth-GREATER), so it went to the flat HUD layer,
// scaled by xr_hud_scale about the gunsight glass, while its box is world-anchored. An instrument draw that follows a box draw of the frame is moved into
// the MARKER image when its vertices form a 45-degree square (every vertex on the rhombus |u| + |v| = 1 of its bounding box, all four tips present, no
// vertex near a bounding-box corner) of box size, in the colour of a box of this frame, centred within the satellite reach of that box. The consumer then
// joins it to the box's stamp (section 13 satellites): one picture at the contact's depth, cached and drawn outside the window, roll-correct.
constexpr float kLockHalfMin = 3.0f, kLockHalfMax = 24.0f;       // GS half extents of an accepted diamond
constexpr float kLockReachX = 64.0f, kLockReachY = 48.0f;        // = marker360.h kSatReachGsX/Y
constexpr unsigned kLockMinVerts = 4, kLockMaxVerts = 64;
struct LockShape { bool ok = false; const char* why = "not-read"; float cx = 0, cy = 0, hw = 0, hh = 0, rgb[3] = {}; int box = -1; float dx = 0, dy = 0; };
inline bool lock_colour_(const float a[3], const float b[3]) noexcept {
    return std::fabs(a[0] - b[0]) <= 0.15f && std::fabs(a[1] - b[1]) <= 0.15f && std::fabs(a[2] - b[2]) <= 0.15f;
}
// v[0..n) = the draw's vertices (unique, as step5TacReadSprites reads them); f.mark[0..nmark) = this frame's box clusters so far.
inline LockShape lock_shape_test(const SpriteVert* v, unsigned n, const TacFrame& f) noexcept {
    LockShape r;
    if (n < kLockMinVerts) { r.why = "few-vertices"; return r; }
    if (n > kLockMaxVerts) { r.why = "many-vertices"; return r; }
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, rgb[3] = {};
    for (unsigned i = 0; i < n; ++i) {
        if (!std::isfinite(v[i].x) || !std::isfinite(v[i].y) || std::fabs(v[i].x - 2048.0f) > 1024.0f || std::fabs(v[i].y - 2048.0f) > 1024.0f) { r.why = "off-screen"; return r; }
        x0 = std::fmin(x0, v[i].x); x1 = std::fmax(x1, v[i].x); y0 = std::fmin(y0, v[i].y); y1 = std::fmax(y1, v[i].y);
        for (int k = 0; k < 3; ++k) rgb[k] += std::isfinite(v[i].rgba[k]) ? v[i].rgba[k] : 0.0f;
    }
    r.cx = 0.5f * (x0 + x1); r.cy = 0.5f * (y0 + y1); r.hw = 0.5f * (x1 - x0); r.hh = 0.5f * (y1 - y0);
    if (r.hw < kLockHalfMin || r.hh < kLockHalfMin || r.hw > kLockHalfMax || r.hh > kLockHalfMax) { r.why = "size"; return r; }
    if (r.hw > 2.0f * r.hh || r.hh > 2.0f * r.hw) { r.why = "aspect"; return r; }
    bool tip[4] = {};   // +x, -x, +y, -y
    for (unsigned i = 0; i < n; ++i) {
        const float u = (v[i].x - r.cx) / r.hw, w = (v[i].y - r.cy) / r.hh, s = std::fabs(u) + std::fabs(w);
        if (s > 1.2f) { r.why = "corner"; return r; }          // an axis-aligned box / bracket has its vertices at the corners (s = 2)
        if (s < 0.55f) { r.why = "inside"; return r; }         // a filled / textured shape (glyph, arrow) has vertices off the rhombus
        if (std::fabs(w) <= 0.35f) { if (u >= 0.8f) tip[0] = true; if (u <= -0.8f) tip[1] = true; }
        if (std::fabs(u) <= 0.35f) { if (w >= 0.8f) tip[2] = true; if (w <= -0.8f) tip[3] = true; }
    }
    if (!(tip[0] && tip[1] && tip[2] && tip[3])) { r.why = "tips"; return r; }
    for (int k = 0; k < 3; ++k) r.rgb[k] = rgb[k] / float(n);
    normalise_rgb(r.rgb);
    float best = 1e30f;
    for (unsigned k = 0; k < f.nmark && k < kMarkCap; ++k) {
        const MarkerSprite& m = f.mark[k];
        if (m.shape != 0 || !lock_colour_(m.rgb, r.rgb)) continue;
        const float dx = r.cx - m.cx, dy = r.cy - m.cy;
        if (std::fabs(dx) > kLockReachX || std::fabs(dy) > kLockReachY) continue;
        const float d = dx * dx + dy * dy;
        if (d < best) { best = d; r.box = int(k); r.dx = dx; r.dy = dy; }
    }
    if (r.box < 0) { r.why = "no-box-in-reach"; return r; }
    r.ok = true; r.why = "diamond";
    return r;
}
// LOCK-WHY (headset test 8, markers-360.md section 21.3): the verdicts of lock_shape_test counted per reason, plus the instrument draws after a box draw that
// never reached the test, so a session WITHOUT mk360cen:1 still says why the lock diamond was never moved (moved:0 and diamond-not-moved:0 only say that no
// draw passed). Bucket order = LockWhyStats::why[]; the last bucket = a read failure or any other text.
constexpr unsigned kLockWhyN = 11;
inline unsigned lock_why_index(const char* why) noexcept {
    static const char* const k[kLockWhyN - 1] = {"diamond", "few-vertices", "many-vertices", "off-screen", "size", "aspect", "corner", "inside", "tips", "no-box-in-reach"};
    if (why) for (unsigned i = 0; i < kLockWhyN - 1; ++i) if (std::strcmp(why, k[i]) == 0) return i;
    return kLockWhyN - 1;
}
struct LockWhyStats { uint64_t why[kLockWhyN] = {}; uint64_t gate_refused = 0, gate_nobox = 0, gate_big = 0; };
inline int format_lock_why(char* b, size_t n, const char* prefix, const LockWhyStats& s) noexcept {
    if (!b || n < 2) return 0;
    const int k = std::snprintf(b, n, "%s-tac-lock;tested=diamond:%llu,few:%llu,many:%llu,offscreen:%llu,size:%llu,aspect:%llu,corner:%llu,inside:%llu,tips:%llu,"
        "nobox:%llu,other:%llu;not-tested=refused-draw:%llu,no-marker:%llu,too-many-indices:%llu", prefix ? prefix : "tac",
        (unsigned long long)s.why[0], (unsigned long long)s.why[1], (unsigned long long)s.why[2], (unsigned long long)s.why[3], (unsigned long long)s.why[4],
        (unsigned long long)s.why[5], (unsigned long long)s.why[6], (unsigned long long)s.why[7], (unsigned long long)s.why[8], (unsigned long long)s.why[9],
        (unsigned long long)s.why[10], (unsigned long long)s.gate_refused, (unsigned long long)s.gate_nobox, (unsigned long long)s.gate_big);
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}
// The accepted diamond as one marker of the frame (shape 1). false = kMarkCap reached (counted in mark_dropped).
inline bool add_lock_shape(TacFrame& f, const LockShape& s, unsigned verts) noexcept {
    if (!s.ok) return false;
    if (f.nmark >= kMarkCap) { ++f.mark_dropped; return false; }
    MarkerSprite& m = f.mark[f.nmark++];
    m = MarkerSprite{};
    m.cx = s.cx; m.cy = s.cy; m.hw = s.hw; m.hh = s.hh; for (int k = 0; k < 3; ++k) m.rgb[k] = s.rgb[k];
    m.verts = verts; m.shape = 1;
    ++f.lock_shapes;
    return true;
}
// mk360cen:1 (one-shot): one line per HUD draw after the first box draw of a census frame, so the lock diamond's signature can be named from the log.
struct CensusFacts {
    uint64_t frame = 0, ord = 0; unsigned after_box = 0; const char* cls = "?"; uint32_t vs8 = 0, ps8 = 0; bool indexed = false; uint32_t count = 0;
    unsigned nv = 0; float x0 = 0, y0 = 0, x1 = 0, y1 = 0, rgb[3] = {}; QStat q; const char* shape = "?"; float dx = 0, dy = 0; int box = -1; const char* read_why = nullptr;
    // runLOCK2 follow-up: the draw's primitive topology and its first kCensusVerts vertices (GS x, y), so a small shape can be drawn from the log.
    uint32_t topo = 0; unsigned nvl = 0; float vx[4] = {}, vy[4] = {};
};
inline int format_lock_census(char* b, size_t n, const CensusFacts& c) noexcept {
    if (!b || n < 2) return 0;
    const int k = std::snprintf(b, n, "tac-lock-census;f=%llu;ord=%llu;after-box=%u;cls=%.12s;vs=%08x;ps=%08x;idx=%u;cnt=%u;nv=%u;bbox=%.1f,%.1f,%.1f,%.1f;"
        "rgb=%.2f,%.2f,%.2f;q=%.4g,%.4g(off:%u);shape=%.16s;box=%d(d:%.1f,%.1f);read=%.24s",
        static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ord), c.after_box, c.cls ? c.cls : "?", unsigned(c.vs8), unsigned(c.ps8),
        unsigned(c.indexed), unsigned(c.count), c.nv, double(c.x0), double(c.y0), double(c.x1), double(c.y1), double(c.rgb[0]), double(c.rgb[1]), double(c.rgb[2]),
        double(c.q.lo), double(c.q.hi), unsigned(c.q.off), c.shape ? c.shape : "?", c.box, double(c.dx), double(c.dy), c.read_why ? c.read_why : "ok");
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    int m = k + std::snprintf(b + k, n - size_t(k), ";topo=%u;v=", unsigned(c.topo));
    if (size_t(m) >= n) { b[n - 1] = 0; return int(n - 1); }
    if (!c.nvl) m += std::snprintf(b + m, n - size_t(m), "-");
    for (unsigned i = 0; i < c.nvl && i < 4 && size_t(m) < n; ++i)
        m += std::snprintf(b + m, n - size_t(m), "%s%.0f,%.0f", i ? "/" : "", double(c.vx[i]), double(c.vy[i]));
    if (size_t(m) >= n) { b[n - 1] = 0; return int(n - 1); }
    return m;
}
// mk360ld:1 (markers-360.md section 21.6): a bounded one-shot per-draw dump of the instrument draws that did NOT become a lock diamond, in frames that hold a
// RED box (the game's locked / targeted colour), so the real diamond's vertices can be read from the log when lock_shape_test rejects it. One line per draw:
// reason, indices, topology, shaders, bbox and the unique vertices (<= kLockDumpVerts). Candidates only: bbox 6..kLockDumpBox GS each way (a glyph, the
// screen-wide HUD frames and the radar are not). One line per signature (reason, vertex count, index count, rounded size), kLockDumpLines in all.
constexpr unsigned kLockDumpLines = 60, kLockDumpVerts = 16, kLockDumpSigs = 96, kLockDumpReadsPerFrame = 24;
constexpr float kLockDumpBox = 90.0f;
inline bool lock_dump_red_(const float rgb[3]) noexcept {
    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]) && rgb[0] >= 0.6f && rgb[1] <= 0.4f && rgb[2] <= 0.4f;
}
// This frame's marker list holds a red box (shape 0) anywhere on screen.
inline bool lock_dump_trigger(const TacFrame& f) noexcept {
    for (unsigned k = 0; k < f.nmark && k < kMarkCap; ++k) if (f.mark[k].shape == 0 && lock_dump_red_(f.mark[k].rgb)) return true;
    return false;
}
struct LockDumpState { unsigned lines = 0, nsig = 0; uint32_t sig[kLockDumpSigs] = {}; };
// true = this signature is new (and recorded): the line may be written. Full table or a repeat = false.
inline bool lock_dump_new_sig(LockDumpState& s, const char* why, unsigned nv, uint32_t count, float hw, float hh) noexcept {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t x) { h = (h ^ x) * 16777619u; };
    for (const char* c = why ? why : ""; *c; ++c) mix(uint8_t(*c));
    mix(nv); mix(count); mix(uint32_t(hw * 0.5f)); mix(uint32_t(hh * 0.5f));
    for (unsigned i = 0; i < s.nsig; ++i) if (s.sig[i] == h) return false;
    if (s.nsig >= kLockDumpSigs) return false;
    s.sig[s.nsig++] = h;
    return true;
}
struct LockDumpFacts {
    uint64_t frame = 0, ord = 0; const char* why = "?"; uint32_t vs8 = 0, ps8 = 0, topo = 0, count = 0; bool indexed = false;
    unsigned nv = 0, after_box = 0; float rgb[3] = {}; unsigned nvl = 0; float vx[kLockDumpVerts] = {}, vy[kLockDumpVerts] = {};
};
// bbox (x0,y0,x1,y1) is computed from ALL nv vertices by the caller; vx / vy hold the first nvl.
inline int format_lock_dump(char* b, size_t n, const LockDumpFacts& c, float x0, float y0, float x1, float y1) noexcept {
    if (!b || n < 2) return 0;
    int k = std::snprintf(b, n, "tac-lock-dump;f=%llu;ord=%llu;why=%.16s;after-box=%u;vs=%08x;ps=%08x;idx=%u;cnt=%u;topo=%u;nv=%u;bbox=%.1f,%.1f,%.1f,%.1f;half=%.1f,%.1f;rgb=%.2f,%.2f,%.2f;v=",
        static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ord), c.why ? c.why : "?", c.after_box, unsigned(c.vs8), unsigned(c.ps8),
        unsigned(c.indexed), unsigned(c.count), unsigned(c.topo), c.nv, double(x0), double(y0), double(x1), double(y1), double(0.5f * (x1 - x0)), double(0.5f * (y1 - y0)),
        double(c.rgb[0]), double(c.rgb[1]), double(c.rgb[2]));
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    if (!c.nvl) k += std::snprintf(b + k, n - size_t(k), "-");
    for (unsigned i = 0; i < c.nvl && i < kLockDumpVerts && size_t(k) < n; ++i)
        k += std::snprintf(b + k, n - size_t(k), "%s%.1f,%.1f", i ? "/" : "", double(c.vx[i]), double(c.vy[i]));
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}
// Census budget (mk360cen:1): kCensusFrames frames, at least kCensusGapMs apart, at most kCensusLinesPerFrame lines each.
constexpr unsigned kCensusFrames = 40, kCensusLinesPerFrame = 20;
constexpr uint64_t kCensusGapMs = 1000;
// CENSUS-AIM (markers-360.md section 18.2). The step146 null runs spent all 40 census frames in the first ~40 s of each flight, before the autopilot
// started, on the GREATER PS 4caf6e2d cloud-sprite draws (220 vertices, near-white, q ~1e-5: 160 of 160 "box" lines), which hud_classify calls boxes
// but which yield no marker; the 20-line cap then went to the green HUD instruments far from any box (2964 lines, every one `shape=not-tested`,
// `box=-1`). A census frame now starts only once the frame's tail holds a COLOURED box marker (not grey / white) near the gunsight, where the seeker
// diamond appears during a lock; its first line lists the frame's boxes; and an instrument draw is logged only when its centre lies within twice the
// satellite reach of a box of the frame (boxes, labels and moved diamonds are always logged).
constexpr float kCensusZoneX = 160.0f, kCensusZoneY = 120.0f;    // GS half extents of the gunsight zone around the screen centre (2048, 2048)
constexpr float kCensusNearX = 2.0f * kLockReachX, kCensusNearY = 2.0f * kLockReachY;
inline bool census_coloured_(const float rgb[3]) noexcept {
    const float mx = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2])), mn = std::fmin(rgb[0], std::fmin(rgb[1], rgb[2]));
    return std::isfinite(mx) && std::isfinite(mn) && mx > 0.0f && (mx - mn) >= 0.2f * mx;
}
// The frame's tail holds a coloured box marker (shape 0) in the gunsight zone: the census frame may start.
inline bool census_trigger(const TacFrame& f) noexcept {
    for (unsigned k = 0; k < f.nmark && k < kMarkCap; ++k) {
        const MarkerSprite& m = f.mark[k];
        if (m.shape != 0 || !census_coloured_(m.rgb)) continue;
        if (std::fabs(m.cx - 2048.0f) <= kCensusZoneX && std::fabs(m.cy - 2048.0f) <= kCensusZoneY) return true;
    }
    return false;
}
// An instrument draw centred at (cx, cy) is worth a census line: within twice the satellite reach of a box marker of the frame.
inline bool census_near_box(const TacFrame& f, float cx, float cy) noexcept {
    if (!std::isfinite(cx) || !std::isfinite(cy)) return false;
    for (unsigned k = 0; k < f.nmark && k < kMarkCap; ++k) {
        const MarkerSprite& m = f.mark[k];
        if (m.shape != 0) continue;
        if (std::fabs(cx - m.cx) <= kCensusNearX && std::fabs(cy - m.cy) <= kCensusNearY) return true;
    }
    return false;
}
// The census frame's first line: its box markers (at most 6: centre, half extents, colour).
inline int format_census_boxes(char* b, size_t n, uint64_t frame, const TacFrame& f) noexcept {
    if (!b || n < 2) return 0;
    int k = std::snprintf(b, n, "tac-lock-census-boxes;f=%llu;n=%u", static_cast<unsigned long long>(frame), unsigned(f.nmark));
    for (unsigned i = 0; i < f.nmark && i < kMarkCap && i < 6 && k > 0 && size_t(k) < n; ++i) {
        const MarkerSprite& m = f.mark[i];
        k += std::snprintf(b + k, n - size_t(k), ";m%u=%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,s%u", i, double(m.cx), double(m.cy), double(m.hw), double(m.hh),
                           double(m.rgb[0]), double(m.rgb[1]), double(m.rgb[2]), unsigned(m.shape));
    }
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}

// ---- firing wedge (D-318 step69 key 1ee6c5da5ac2fa31: VS 534f29ca, PS d34ee153, non-indexed 4 vertices, blend on src5/dst2/op1, depth GREATER w0)
struct WedgeFacts {
    uint32_t vs8 = 0, ps8 = 0;      // first 8 hex digits of the creation shader hashes as numbers
    bool indexed = false; uint32_t count = 0;
    bool blend = false; uint32_t src = 0, dst = 0, op = 0;
    bool depth = false, depth_write = false; uint32_t depth_func = 0;
};
constexpr uint32_t kWedgeVs = 0x534f29cau, kWedgePs = 0xd34ee153u;
inline bool is_wedge(const WedgeFacts& w) noexcept {
    return w.vs8 == kWedgeVs && w.ps8 == kWedgePs && !w.indexed && w.count == 4 && w.blend && w.src == 5 && w.dst == 2 && w.op == 1 &&
           w.depth && !w.depth_write && w.depth_func == 5;   // D3D12_COMPARISON_FUNC_GREATER
}

} // namespace acz::tac
