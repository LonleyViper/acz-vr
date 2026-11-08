#pragma once
// ui3d_census.h -- UI3D stage-2 prerequisite (docs/world-render/ui-3d.md section 4 item 1): the READ-ONLY menu draw census.
// Trigger (no config key; the 32-key cap is full): the file %LOCALAPPDATA%\acz-vr\ui3d-census.arm, polled once per present-statistics window (5 s,
// ee_camera_probe_win.cpp pacing_tick) on the Present thread. Absent file: the draw hook pays one relaxed atomic load.
// While armed, ONE frame per second is sampled (the draws recorded between two presents), at most kMaxSamples frames per arm; then the census
// latches off (stays off until the file is removed and created again). For every ps2_v menu draw (VS 534f29ca / PS d34ee153) the first kMaxDraws
// of the frame are logged, one line each, plus per-frame totals and the top kOtherTop OTHER shader keys (movie / ACM / SelMap frames stay visible).
// Output: lines `event=ui3d-census;kind=frame|draw|other;...` in ee-camera.log (+ the diagnostics log), at most kMaxLines per arm, the last one
// `event=ui3d-census;latched;...`. Nothing is written to a command list, to game memory or to the GPU; every unreadable field prints `?`.
// The draw side keeps fixed storage only (no allocation); the Present thread formats and logs outside the lock. Header-only: d3d12_hook.cpp is
// compiled into many offline test executables. The D3D12 glue is ui3d_census_hooks.inl (hook-state mutex held).
// ACM mode (UI3D stage 3b, ui-3d.md 8.4): %LOCALAPPDATA%\acz-vr\ui3d-census-acm.arm (instead of, or with, the other arm files; arms by itself) also logs
// every DrawACM_v draw (VS b569109e, any PS) of the sampled frame, the first kMaxAcm of them: `kind=acm` (facts), `kind=acm-b` (the record's B matrix in
// full) and, for the first ACM draw and for the first draw of every further distinct camera (A bit-different from every A printed before; at most
// kMaxAcmCameras per frame; POV-RENDER census, docs/world-render/other-povs.md), `kind=acm-a` (A in full). The ordinal `all=` is the frame draw ordinal
// shared with the ps2_v `kind=draw` lines.
// The t10 record is read with step3MappedSpan_locked + two agreeing guardedCpuRead copies only (ui3d_census_hooks.inl); unreadable = `?(reason)`.
// TAIL mode (UI3D stage 3, ui-3d.md 7.4): %LOCALAPPDATA%\acz-vr\ui3d-census-tail.arm (instead of, or with, ui3d-census.arm) logs the LAST kMaxDraws
// menu draws of each sampled frame (a ring, printed oldest first; the frame line gains `;mode=tail`). In flight the HUD's ~62 menu-key draws come
// first and the pause menu last (hud-menu.md 2), so only the tail shows the pause menu's draws. Same limits, same latch, same sink.
// GEO lines (2026-10-30, DEBRIEF-3D: docs/world-render/debrief-3d-map.md section 7): every `kind=draw` line ends with `;ia=<IA topology>` (the
// D3D_PRIMITIVE_TOPOLOGY bound at the draw, 0 = unobserved), and each of the first kMaxGeo INDEXED menu draws of a sampled frame is followed by one
// `kind=geo` line: the index range read (strip-cut markers counted, min/max index), x/y/z/q ranges of every referenced vertex (bounded), the
// GS -> pixel bbox and the first kGeoSamples vertices in index order. Read through step3MappedSpan_locked + guardedCpuRead only; any other case
// prints `geo=?(reason)`. No new arm file: armed with the census.
#include "briefing_observe.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace acz::u3c {

inline constexpr uint32_t kMenuVs8 = acz::brobs::kPs2Vs8;   // ps2_v 534f29ca (the identity table of briefing_observe.h)
inline constexpr uint32_t kMenuPs8 = 0xd34ee153u;           // ps2 HUD/menu sprite pixel shader (fx_census.h kPsHudSprite)
inline constexpr unsigned kMaxDraws = 48;                   // menu draws logged per sampled frame
inline constexpr uint32_t kAcmVs8 = acz::brobs::kAcmVs8;   // DrawACM_v b569109e (hangar shell, aircraft, world objects)
inline constexpr unsigned kMaxAcm = 32;                     // ACM draws logged per sampled frame (more are only counted)
inline constexpr unsigned kMaxAcmCameras = 4;               // distinct A matrices printed per sampled frame (`kind=acm-a`; POV census, other-povs.md)
inline constexpr unsigned kRecBytes = 416;                  // the SrtData record (A +0, B +256, depth scale +360, screen constants +400)
inline constexpr unsigned kMaxVerts = 8;                    // vertices read per draw (4 are printed)
inline constexpr unsigned kPrintVerts = 4;
inline constexpr unsigned kMaxSamples = 60;                 // sampled frames per arm
inline constexpr unsigned kMaxLines = 6000;                 // log lines per arm, including the final latched line
inline constexpr unsigned kOtherTop = 8;
inline constexpr uint64_t kPeriodUs = 1000000;              // one sampled frame per second
inline constexpr unsigned kMaxGeo = 16;                     // indexed menu draws per sampled frame that get a GEO read (the first ones)
inline constexpr unsigned kGeoMaxIdx = 4096;                // indices read per draw (debrief census: up to 1,559)
inline constexpr unsigned kGeoMaxVerts = 2048;              // referenced vertices read per draw (debrief census: up to 1,248)
inline constexpr unsigned kGeoSamples = 3;                  // vertices printed in index order
inline constexpr size_t kLineLimit = 460;                  // diagnostics records hold < 576 B; the same bound as brief3d's lines (< 459)

// GEO facts of one indexed menu draw (filled by the glue through the pure helpers below). Plain data.
struct Geo {
    const char* why = "not-read";               // null = read; "not-read" = not attempted (no line)
    uint32_t ibs = 0, cut = UINT32_MAX;         // index stride; the PSO IBStripCutValue enum (0 disabled, 1 0xFFFF, 2 0xFFFFFFFF; UINT32_MAX unknown)
    int32_t base = 0;
    uint32_t ni = 0, nread = 0, cuts = 0;       // indices of the draw / read; markers equal to the stride's maximum (0xFFFF / 0xFFFFFFFF)
    uint32_t imin = 0, imax = 0;                // over the non-marker indices read
    uint32_t nv = 0; bool vtrunc = false;       // vertices read (imin..imax, at most kGeoMaxVerts)
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0, q0 = 0, q1 = 0;   // ranges over the vertices read
    uint32_t sidx[kGeoSamples] = {}; uint32_t nsidx = 0;   // the first non-marker indices in index order
    float sv[kGeoSamples][4] = {}; uint32_t nsv = 0;        // their vertices (x, y, z, q), when inside the vertices read
};
// Pass 1 (pure): scan `n` indices of `stride` bytes (little endian). why = null when at least one vertex index was found.
inline void geo_scan_indices(const uint8_t* ib, uint32_t n, uint32_t stride, Geo& g) noexcept {
    g.nread = n; g.cuts = 0; g.nsidx = 0; g.imin = UINT32_MAX; g.imax = 0; g.nv = 0; g.nsv = 0;
    if (!ib || !n || (stride != 2 && stride != 4)) { g.imin = 0; g.nread = 0; g.why = "u3c-geo-no-indices"; return; }
    const uint32_t marker = stride == 2 ? 0xffffu : 0xffffffffu;
    uint32_t vertices = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        for (uint32_t j = 0; j < stride; ++j) v |= uint32_t(ib[size_t(i) * stride + j]) << (8 * j);
        if (v == marker) { ++g.cuts; continue; }
        ++vertices;
        if (v < g.imin) g.imin = v;
        if (v > g.imax) g.imax = v;
        if (g.nsidx < kGeoSamples) g.sidx[g.nsidx++] = v;
    }
    if (!vertices) { g.imin = g.imax = 0; g.why = "u3c-geo-only-markers"; return; }
    g.why = nullptr;
}
// Pass 2 (pure): one vertex (x, y, z, q) of the read range; `vertex` = its index relative to imin. A non-finite value sets why (the ranges stop).
inline void geo_add_vertex(Geo& g, uint32_t vertex, const float f[4]) noexcept {
    if (g.why) return;
    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3])) { g.why = "u3c-geo-vertex-nonfinite"; return; }
    if (g.nv == 0) { g.x0 = g.x1 = f[0]; g.y0 = g.y1 = f[1]; g.z0 = g.z1 = f[2]; g.q0 = g.q1 = f[3]; }
    else {
        g.x0 = std::min(g.x0, f[0]); g.x1 = std::max(g.x1, f[0]); g.y0 = std::min(g.y0, f[1]); g.y1 = std::max(g.y1, f[1]);
        g.z0 = std::min(g.z0, f[2]); g.z1 = std::max(g.z1, f[2]); g.q0 = std::min(g.q0, f[3]); g.q1 = std::max(g.q1, f[3]);
    }
    ++g.nv;
    for (uint32_t k = 0; k < g.nsidx; ++k)
        if (g.sidx[k] - g.imin == vertex) { std::memcpy(g.sv[k], f, 16); if (k + 1 > g.nsv) g.nsv = k + 1; }
}

// One logged draw. Plain data, no pointers except static reason strings.
struct DrawRec {
    uint32_t all = 0, ord = 0;                  // draw ordinal in the frame (all keys) / menu-draw ordinal (1-based)
    uint32_t count = 0, instances = 0, start = 0;
    bool indexed = false;
    // render target 0 as the game bound it (step-3 OM track + descriptor registry); unknown = false
    bool rt_known = false; uint64_t rt_res = 0; uint32_t rt_vfmt = 0, rt_rfmt = 0, rt_w = 0, rt_h = 0;
    bool pso_known = false; uint32_t pso_rtv0 = 0, pso_dsv = 0, dsv_view_fmt = 0; bool dsv_view_known = false;
    bool vp_known = false; float vp[4] = {}; uint32_t nvp = 0;          // x, y, w, h of viewport 0
    bool sc_known = false; int32_t sc[4] = {}; uint32_t nsc = 0;        // l, t, r, b of scissor 0
    bool blend_enable = false; uint32_t blend[6] = {};                  // src, dst, op, src-a, dst-a, op-a
    bool depth_enable = false, depth_write = false, stencil = false; uint32_t depth_func = 0;
    // vertex buffer view 0
    bool vb_bound = false; uint64_t vb_va = 0, vb_size = 0; uint32_t vb_stride = 0;
    const char* vb_why = nullptr;                // null = the view lies in a pinned mapped UPLOAD span (step3MappedSpan_locked)
    uint32_t nv = 0; const char* vtx_why = nullptr; float vtx[kMaxVerts][4] = {};   // x, y, z, q
    // cb0 (b0 of the vertex stage): where it came from and its four dwords
    const char* cb_kind = nullptr;               // "rc" root constants, "cbv" root CBV, null = unresolved
    uint32_t cb_root = 0; uint64_t cb_va = 0; const char* cb_why = nullptr; uint32_t cb_mask = 0; float cb[4] = {};
    uint32_t ia = 0;                             // IA primitive topology bound at the draw (D3D_PRIMITIVE_TOPOLOGY; 0 = unobserved)
    Geo geo;                                     // indexed draws only (the first kMaxGeo of the frame)
};

// One logged DrawACM_v draw: the common draw/state facts (DrawRec fields used: all, count, instances, start, indexed, rt*, pso*, blend*, depth*), plus the
// base vertex, the index stride, cb10 and the t10 record as the flight path decodes it (gpu_copy_step5_world.h).
struct AcmRec {
    DrawRec c;                                  // c.ord = the number of menu (ps2_v) draws before this one in the frame
    int32_t base = 0; uint32_t ib_stride = 0;   // BaseVertexLocation; index buffer stride (0 = unbound / unknown)
    const char* cb_why = "not-read";            // cb10 (root constants, vertex then pixel stage): null = words in cb[] under cb_mask
    uint32_t cb_root = 0, cb_mask = 0; uint32_t cb[4] = {};
    uint64_t t10_va = 0; const char* t10_why = "not-read";   // null = the record was read (two agreeing copies)
    float a[16] = {}, b[16] = {}, k360 = 0.0f, k400[4] = {};   // A = +0, B = +256, +360, +400
};

struct Frame {
    uint32_t serial = 0;
    uint32_t acm_n = 0;                          // ACM draws seen (acm mode)
    bool acm_mode = false;
    AcmRec acm[kMaxAcm];
    bool acm_pending[kMaxAcm] = {};
    uint32_t all = 0, menu = 0, menu_indexed = 0, menu_verts = 0;
    DrawRec rec[kMaxDraws];
    bool pending[kMaxDraws] = {};                // admitted for this serial but its record is not committed yet (never a prior sample's data)
    uint32_t slot_ord[kMaxDraws] = {};           // the menu ordinal that owns each record slot (tail mode reuses slots: a late commit of an
                                                 // overwritten ordinal is dropped)
    bool tail = false;                           // TAIL mode: rec[] is a ring of the last kMaxDraws menu draws
    acz::brobs::Window others;                   // every key except the menu key
};

// ---- derived values (pure)
struct Derived {
    bool ranges = false; float zmin = 0, zmax = 0, qmin = 0, qmax = 0;   // over the vertices read
    bool bbox = false; double x0 = 0, y0 = 0, x1 = 0, y1 = 0;            // screen pixels (needs cb0, viewport 0 and q != 0)
};
inline Derived derive(const DrawRec& d) noexcept {
    Derived o;
    if (!d.nv) return o;
    o.ranges = true; o.zmin = o.qmin = 1e30f; o.zmax = o.qmax = -1e30f;
    for (uint32_t i = 0; i < d.nv; ++i) {
        o.zmin = std::min(o.zmin, d.vtx[i][2]); o.zmax = std::max(o.zmax, d.vtx[i][2]);
        o.qmin = std::min(o.qmin, d.vtx[i][3]); o.qmax = std::max(o.qmax, d.vtx[i][3]);
    }
    if (d.cb_mask != 15u || !d.vp_known || d.cb[0] == 0.0f || d.cb[1] == 0.0f) return o;
    // ps2_v: ndc = ((x-ox)*2/sx, (y-oy)*2/sy) / q   (docs/world-render/ui-3d.md section 1); D3D viewport transform, y up in ndc
    bool first = true;
    for (uint32_t i = 0; i < d.nv; ++i) {
        const double q = d.vtx[i][3];
        if (!(q != 0.0) || !std::isfinite(q)) return o;
        const double nx = (double(d.vtx[i][0]) - double(d.cb[2])) * 2.0 / double(d.cb[0]) / q;
        const double ny = (double(d.vtx[i][1]) - double(d.cb[3])) * 2.0 / double(d.cb[1]) / q;
        const double px = double(d.vp[0]) + (nx * 0.5 + 0.5) * double(d.vp[2]);
        const double py = double(d.vp[1]) + (0.5 - ny * 0.5) * double(d.vp[3]);
        if (!std::isfinite(px) || !std::isfinite(py)) return o;
        if (first) { o.x0 = o.x1 = px; o.y0 = o.y1 = py; first = false; }
        else { o.x0 = std::min(o.x0, px); o.x1 = std::max(o.x1, px); o.y0 = std::min(o.y0, py); o.y1 = std::max(o.y1, py); }
    }
    o.bbox = !first;
    return o;
}

// ---- formatting (pure; truncation-safe; every unknown value is `?`)
namespace detail {
struct Out {
    char* p; size_t cap; size_t n = 0;
    Out(char* b, size_t c) : p(b), cap(c) { if (cap) p[0] = 0; }
    void put(const char* fmt, ...) noexcept;
};
inline void Out::put(const char* fmt, ...) noexcept {
    if (!cap || n + 1 >= cap) return;
    va_list a; va_start(a, fmt);
    const int r = std::vsnprintf(p + n, cap - n, fmt, a);
    va_end(a);
    if (r < 0) { p[n] = 0; return; }
    n = size_t(r) >= cap - n ? cap - 1 : n + size_t(r);
}
}  // namespace detail

inline int format_draw(const DrawRec& d, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    detail::Out o(out, cap);
    const Derived v = derive(d);
    o.put("event=ui3d-census;kind=draw;s=%u;ord=%u;all=%u;idx=%u;n=%u;inst=%u;st=%u;", sample, d.ord, d.all, d.indexed ? 1u : 0u, d.count, d.instances, d.start);
    if (d.rt_known) o.put("rt=0x%llx/v%u/r%u/%ux%u;", static_cast<unsigned long long>(d.rt_res), d.rt_vfmt, d.rt_rfmt, d.rt_w, d.rt_h);
    else o.put("rt=?;");
    if (d.pso_known) o.put("pso-rtv0=%u;", d.pso_rtv0); else o.put("pso-rtv0=?;");
    if (d.pso_known && d.dsv_view_known) o.put("dsv=%u/view%u;", d.pso_dsv, d.dsv_view_fmt);
    else if (d.pso_known) o.put("dsv=%u/view?;", d.pso_dsv);
    else o.put("dsv=?;");
    if (d.vp_known) o.put("vp=%.5g,%.5g,%.5g,%.5g/%u;", double(d.vp[0]), double(d.vp[1]), double(d.vp[2]), double(d.vp[3]), d.nvp); else o.put("vp=?;");
    if (d.sc_known) o.put("sc=%d,%d,%d,%d/%u;", d.sc[0], d.sc[1], d.sc[2], d.sc[3], d.nsc); else o.put("sc=?;");
    if (d.pso_known) {
        o.put("bl=%u:%u,%u,%u,%u,%u,%u;", d.blend_enable ? 1u : 0u, d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5]);
        o.put("dp=%u/%u/%u/stc%u;", d.depth_enable ? 1u : 0u, d.depth_func, d.depth_write ? 1u : 0u, d.stencil ? 1u : 0u);
    } else o.put("bl=?;dp=?;");
    if (d.vb_bound) o.put("vb=0x%llx,%llu,%u,%s;", static_cast<unsigned long long>(d.vb_va), static_cast<unsigned long long>(d.vb_size), d.vb_stride,
                          d.vb_why ? d.vb_why : "mapped-upload");
    else o.put("vb=?;");
    if (d.nv) {
        o.put("v=");
        const unsigned np = d.nv < kPrintVerts ? d.nv : kPrintVerts;
        for (unsigned i = 0; i < np; ++i)
            o.put("%s%.5g,%.5g,%.5g,%.5g", i ? "|" : "", double(d.vtx[i][0]), double(d.vtx[i][1]), double(d.vtx[i][2]), double(d.vtx[i][3]));
        o.put("/nv%u;", d.nv);
    } else o.put("v=?(%s);", d.vtx_why ? d.vtx_why : "none");
    if (d.cb_kind) {
        if (d.cb_kind[0] == 'r') o.put("cb0=rc%u:", d.cb_root); else o.put("cb0=cbv%u@0x%llx:", d.cb_root, static_cast<unsigned long long>(d.cb_va));
        for (unsigned i = 0; i < 4; ++i) {
            if (d.cb_mask & (1u << i)) o.put("%s%.5g", i ? "," : "", double(d.cb[i])); else o.put("%s?", i ? "," : "");
        }
        o.put(";");
    } else o.put("cb0=?(%s);", d.cb_why ? d.cb_why : "none");
    if (v.bbox) o.put("bbox=%.5g,%.5g,%.5g,%.5g;", v.x0, v.y0, v.x1, v.y1); else o.put("bbox=?;");
    if (v.ranges) o.put("z=%.5g,%.5g;q=%.5g,%.5g", double(v.zmin), double(v.zmax), double(v.qmin), double(v.qmax)); else o.put("z=?;q=?");
    o.put(";ia=%u", d.ia);
    return int(o.n);
}
// GS (x, y) -> render-target pixel through the draw's cb0 (q = 1) and viewport 0. false = unknown cb0 / viewport.
inline bool gs_pixel(const DrawRec& d, double x, double y, double& px, double& py) noexcept {
    if (d.cb_mask != 15u || !d.vp_known || d.cb[0] == 0.0f || d.cb[1] == 0.0f) return false;
    const double nx = (x - double(d.cb[2])) * 2.0 / double(d.cb[0]), ny = (y - double(d.cb[3])) * 2.0 / double(d.cb[1]);
    px = double(d.vp[0]) + (nx * 0.5 + 0.5) * double(d.vp[2]); py = double(d.vp[1]) + (0.5 - ny * 0.5) * double(d.vp[3]);
    return std::isfinite(px) && std::isfinite(py);
}
// `kind=geo` (indexed draws with a GEO attempt): ia / cut / ibs / base, ni=<indices read>/<draw indices>, cuts, imin / imax, nv (+`t` = truncated),
// GS ranges, the pixel bbox (q = 1 assumed: meaningful only when q=1,1) and the first kGeoSamples vertices in index order (`i<index>:x,y,z,q`).
inline int format_geo(const DrawRec& d, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    detail::Out o(out, cap);
    const Geo& g = d.geo;
    o.put("event=ui3d-census;kind=geo;s=%u;ord=%u;all=%u;ia=%u;", sample, d.ord, d.all, d.ia);
    if (g.cut == UINT32_MAX) o.put("cut=?;"); else o.put("cut=%u;", g.cut);
    o.put("ibs=%u;base=%d;", g.ibs, g.base);
    if (g.why && g.nread == 0) { o.put("geo=?(%s)", g.why); return int(o.n); }
    o.put("ni=%u/%u;cuts=%u;imin=%u;imax=%u;", g.nread, g.ni, g.cuts, g.imin, g.imax);
    if (g.why) { o.put("nv=%u;geo=?(%s)", g.nv, g.why); return int(o.n); }
    o.put("nv=%u%s;x=%.6g,%.6g;y=%.6g,%.6g;z=%.6g,%.6g;q=%.6g,%.6g;", g.nv, g.vtrunc ? "t" : "", double(g.x0), double(g.x1), double(g.y0), double(g.y1),
          double(g.z0), double(g.z1), double(g.q0), double(g.q1));
    double a = 0, b = 0, c = 0, e = 0;
    if (gs_pixel(d, g.x0, g.y0, a, b) && gs_pixel(d, g.x1, g.y1, c, e))
        o.put("px=%.5g,%.5g,%.5g,%.5g;", std::min(a, c), std::min(b, e), std::max(a, c), std::max(b, e));
    else o.put("px=?;");
    o.put("v=");
    for (uint32_t k = 0; k < g.nsv; ++k)
        o.put("%si%u:%.6g,%.6g,%.6g,%.6g", k ? "|" : "", g.sidx[k], double(g.sv[k][0]), double(g.sv[k][1]), double(g.sv[k][2]), double(g.sv[k][3]));
    if (!g.nsv) o.put("?");
    return int(o.n);
}
inline bool geo_line(const DrawRec& d) noexcept { return d.indexed && !(d.geo.why && std::strcmp(d.geo.why, "not-read") == 0); }

// Per-frame totals: `event=ui3d-census;kind=frame;s=<sample>;draws=<all>;menu=<ps2 menu-key draws>;logged=<min(menu,48)>;indexed=<menu draws that were indexed>;
// menu-units=<summed vertex/index counts>;other=<draws of other keys>;other-keys=<distinct>;overflow=<keys beyond 32>;no-pso=<unknown PSO draws>`.
inline int format_frame(const Frame& f, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    detail::Out o(out, cap);
    o.put("event=ui3d-census;kind=frame;s=%u;draws=%u;menu=%u;logged=%u;indexed=%u;menu-units=%u;other=%llu;other-keys=%u;overflow=%llu;no-pso=%llu",
          sample, f.all, f.menu, f.menu < kMaxDraws ? f.menu : unsigned(kMaxDraws), f.menu_indexed, f.menu_verts,
          static_cast<unsigned long long>(f.others.draws), f.others.n, static_cast<unsigned long long>(f.others.overflow),
          static_cast<unsigned long long>(f.others.noPso));
    if (f.tail) o.put(";mode=tail");
    if (f.acm_mode) o.put(";acm=%u;acm-logged=%u", f.acm_n, f.acm_n < kMaxAcm ? f.acm_n : unsigned(kMaxAcm));
    return int(o.n);
}
// ---- ACM draws. `kind=acm`: ord = ps2_v draws before it; `all` = the shared frame ordinal; idx/n/st/base = indexed flag, index count, start index, base vertex.
inline bool same_bits(const float* x, const float* y, unsigned n) noexcept { return std::memcmp(x, y, n * sizeof(float)) == 0; }
inline int format_acm(const AcmRec& r, const AcmRec* first, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    detail::Out o(out, cap);
    const DrawRec& d = r.c;
    o.put("event=ui3d-census;kind=acm;s=%u;all=%u;ord=%u;idx=%u;n=%u;st=%u;base=%d;inst=%u;ibs=%u;", sample, d.all, d.ord, d.indexed ? 1u : 0u, d.count, d.start,
          r.base, d.instances, r.ib_stride);
    if (d.rt_known) o.put("rt=0x%llx/v%u/r%u/%ux%u;", static_cast<unsigned long long>(d.rt_res), d.rt_vfmt, d.rt_rfmt, d.rt_w, d.rt_h);
    else o.put("rt=?;");
    if (d.pso_known) o.put("pso-rtv0=%u;", d.pso_rtv0); else o.put("pso-rtv0=?;");
    if (d.pso_known && d.dsv_view_known) o.put("dsv=%u/view%u;", d.pso_dsv, d.dsv_view_fmt);
    else if (d.pso_known) o.put("dsv=%u/view?;", d.pso_dsv);
    else o.put("dsv=?;");
    if (d.pso_known) {
        o.put("bl=%u:%u,%u,%u,%u,%u,%u;", d.blend_enable ? 1u : 0u, d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5]);
        o.put("dp=%u/%u/%u/stc%u;", d.depth_enable ? 1u : 0u, d.depth_func, d.depth_write ? 1u : 0u, d.stencil ? 1u : 0u);
    } else o.put("bl=?;dp=?;");
    if (!r.cb_why) {
        o.put("cb10=rc%u:", r.cb_root);
        for (unsigned i = 0; i < 4; ++i) {
            if (r.cb_mask & (1u << i)) o.put("%s0x%x", i ? "," : "", r.cb[i]); else o.put("%s?", i ? "," : "");
        }
        o.put(";");
    } else o.put("cb10=?(%s);", r.cb_why);
    if (r.t10_va) o.put("t10=0x%llx", static_cast<unsigned long long>(r.t10_va)); else o.put("t10=?");
    if (r.t10_why) {
        o.put("(%s);k360=?;k400=?;A=?", r.t10_why);
    } else {
        o.put("/read;k360=%.7g;k400=%.7g,%.7g,%.7g,%.7g;", double(r.k360), double(r.k400[0]), double(r.k400[1]), double(r.k400[2]), double(r.k400[3]));
        if (!first || first == &r) o.put("A=first");
        else if (first->t10_why) o.put("A=?");
        else o.put("A=%s", same_bits(first->a, r.a, 16) ? "same" : "differs");
    }
    return int(o.n);
}
// `kind=acm-b` (B = +256 in full, every draw) / `kind=acm-a` (A = +0 in full, first draw): row-major as stored, %.9g (float round-trip).
inline int format_acm_mat(const AcmRec& r, bool a_matrix, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    detail::Out o(out, cap);
    o.put("event=ui3d-census;kind=acm-%c;s=%u;all=%u;", a_matrix ? 'a' : 'b', sample, r.c.all);
    if (r.t10_why) { o.put("%c=?(%s)", a_matrix ? 'A' : 'B', r.t10_why); return int(o.n); }
    const float* m = a_matrix ? r.a : r.b;
    o.put("%c=", a_matrix ? 'A' : 'B');
    for (unsigned i = 0; i < 16; ++i) o.put("%s%.9g", i ? "," : "", double(m[i]));
    return int(o.n);
}
// The top kOtherTop OTHER keys by draws: items vs8/ps8/t<topology><i|n><g|->/draws/units/max[/name] (briefing_observe.h append_items). 0 = nothing.
inline int format_other(const Frame& f, unsigned sample, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!f.others.n) return 0;
    unsigned order[acz::brobs::kEntries];
    acz::brobs::rank(f.others, order);
    int n = std::snprintf(out, cap, "event=ui3d-census;kind=other;s=%u;keys=%u;top=", sample, f.others.n);
    if (n < 0) { out[0] = 0; return 0; }
    if (size_t(n) >= cap) return int(cap - 1);
    return acz::brobs::append_items(f.others, order, 0, kOtherTop, out, cap, n, nullptr);
}
inline int format_latched(const char* why, unsigned samples, unsigned lines, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    const int r = std::snprintf(out, cap, "event=ui3d-census;latched;reason=%s;samples=%u;lines=%u;rearm=remove-and-recreate-the-arm-file", why ? why : "?", samples, lines);
    return r < 0 ? 0 : (size_t(r) >= cap ? int(cap - 1) : r);
}

// Emits one sampled frame through sink(const char*): the frame line, up to kMaxDraws draw lines, the other-keys line, within `budget` lines.
// Returns the lines written. A frame line alone needs 1; draws are dropped (never the frame line) when the budget is short.
template <class Sink>
inline unsigned emit_frame(const Frame& f, unsigned sample, unsigned budget, Sink&& sink) noexcept {
    char b[kLineLimit + 1];
    unsigned n = 0;
    if (budget < 1) return 0;
    format_frame(f, sample, b, sizeof b); sink(static_cast<const char*>(b)); ++n;
    const unsigned logged = f.menu < kMaxDraws ? f.menu : unsigned(kMaxDraws);
    const bool haveOther = f.others.n != 0;
    const unsigned first = f.tail && f.menu > kMaxDraws ? f.menu % kMaxDraws : 0u;   // tail ring: the oldest kept record first
    for (unsigned j = 0; j < logged && n + (haveOther ? 1u : 0u) < budget; ++j) {
        const unsigned i = (first + j) % kMaxDraws;
        if (f.pending[i]) std::snprintf(b, sizeof b, "event=ui3d-census;kind=draw;s=%u;ord=%u;incomplete=admitted-not-committed", sample,
                                        f.slot_ord[i] ? f.slot_ord[i] : i + 1);
        else format_draw(f.rec[i], sample, b, sizeof b);
        sink(static_cast<const char*>(b)); ++n;
        if (!f.pending[i] && geo_line(f.rec[i]) && n + (haveOther ? 1u : 0u) < budget) {
            format_geo(f.rec[i], sample, b, sizeof b); sink(static_cast<const char*>(b)); ++n;
        }
    }
    if (f.acm_mode) {
        const unsigned na = f.acm_n < kMaxAcm ? f.acm_n : unsigned(kMaxAcm);
        unsigned cams[kMaxAcmCameras]; unsigned ncams = 0;   // POV census: the draws whose A was printed (one per distinct camera of the frame)
        for (unsigned j = 0; j < na; ++j) {
            // the first draw always prints A; a later READABLE draw prints A too when it differs from every A printed so far (offscreen passes and the
            // scene camera are different cameras: other-povs.md section 2), at most kMaxAcmCameras per frame. A frame with one camera: unchanged.
            bool newCam = j == 0;
            if (!newCam && !f.acm_pending[j] && !f.acm[j].t10_why && ncams < kMaxAcmCameras) {
                newCam = true;
                for (unsigned c = 0; c < ncams && newCam; ++c)
                    if (!f.acm[cams[c]].t10_why && same_bits(f.acm[cams[c]].a, f.acm[j].a, 16)) newCam = false;
            }
            const unsigned need = (newCam ? 3u : 2u);
            if (n + (haveOther ? 1u : 0u) + need > budget) break;   // whole draws only
            if (f.acm_pending[j]) {
                std::snprintf(b, sizeof b, "event=ui3d-census;kind=acm;s=%u;ord=%u;incomplete=admitted-not-committed", sample, j + 1);
                sink(static_cast<const char*>(b)); ++n; continue;
            }
            format_acm(f.acm[j], &f.acm[0], sample, b, sizeof b); sink(static_cast<const char*>(b)); ++n;
            if (newCam) { format_acm_mat(f.acm[j], true, sample, b, sizeof b); sink(static_cast<const char*>(b)); ++n; if (ncams < kMaxAcmCameras) cams[ncams++] = j; }
            format_acm_mat(f.acm[j], false, sample, b, sizeof b); sink(static_cast<const char*>(b)); ++n;
        }
    }
    if (haveOther && n < budget && format_other(f, sample, b, sizeof b) > 0) { sink(static_cast<const char*>(b)); ++n; }
    return n;
}

// ---- sampling state (no lock inside: Shared wraps it)
enum class Admit : uint8_t { skip, count, take, take_acm };   // take_acm: an ACM draw to record (acm mode); slot indexes Frame::acm
struct Accumulator {
    bool armed = false, sampling = false, latched = false, tail = false, acm = false;
    uint64_t last_us = 0;
    uint32_t serial = 1, samples = 0, lines = 0;
    const char* latch_why = nullptr;
    Frame f;
    void reset_frame() noexcept {
        f.serial = ++serial; f.all = f.menu = f.menu_indexed = f.menu_verts = 0;
        f.others = acz::brobs::Window{};
        for (unsigned i = 0; i < kMaxDraws; ++i) { f.rec[i] = DrawRec{}; f.pending[i] = false; f.slot_ord[i] = 0; }   // no record outlives its sample
        f.tail = tail;
        f.acm_mode = acm; f.acm_n = 0;
        for (unsigned i = 0; i < kMaxAcm; ++i) { f.acm[i] = AcmRec{}; f.acm_pending[i] = false; }
    }
    void set_armed(bool a, bool tail_mode = false, bool acm_mode = false) noexcept {
        if (a && !armed) { samples = 0; lines = 0; latched = false; latch_why = nullptr; last_us = 0; }
        if (a && armed && (tail_mode != tail || acm_mode != acm)) { sampling = false; }   // a mode change takes effect at the next sampled frame
        tail = a && tail_mode; acm = a && acm_mode;
        if (!a) { sampling = false; reset_frame(); }
        armed = a;
    }
    // Draw path. `slot` (valid for take) = index into f.rec; `all` = draw ordinal in the frame; `ord` = menu ordinal (1-based).
    Admit admit(bool menu, uint32_t vs8, uint32_t ps8, uint8_t topo, bool indexed, bool gs, uint32_t count, uint32_t instances,
                uint32_t& serial_out, uint32_t& slot, uint32_t& all, uint32_t& ord) noexcept {
        serial_out = f.serial; slot = 0; all = 0; ord = 0;
        if (!armed || !sampling || latched) return Admit::skip;
        all = ++f.all;
        if (!menu) {
            acz::brobs::note_draw(f.others, vs8, ps8, topo, indexed, gs, count, instances);   // ACM stays in the OTHER-keys line too
            if (acm && vs8 == kAcmVs8) {
                const uint32_t n = ++f.acm_n;
                if (n <= kMaxAcm) { slot = n - 1; ord = f.menu; f.acm_pending[slot] = true; return Admit::take_acm; }
            }
            return Admit::count;
        }
        ord = ++f.menu;
        if (indexed) ++f.menu_indexed;
        f.menu_verts += count;
        if (ord > kMaxDraws && !f.tail) return Admit::count;
        slot = (ord - 1) % kMaxDraws;   // tail mode: the ring overwrites the oldest record
        f.pending[slot] = true; f.slot_ord[slot] = ord;
        return Admit::take;
    }
    void commit(uint32_t serial_in, uint32_t slot, const DrawRec& d) noexcept {
        if (serial_in != f.serial || slot >= kMaxDraws) return;   // the frame closed between admit and commit
        if (f.tail && f.slot_ord[slot] != d.ord) return;           // tail ring: a later menu draw owns the slot now (head mode: one slot per ordinal)
        f.rec[slot] = d; f.pending[slot] = false;
    }
    void commit_acm(uint32_t serial_in, uint32_t slot, const AcmRec& r) noexcept {
        if (serial_in != f.serial || slot >= kMaxAcm || !f.acm_mode) return;
        f.acm[slot] = r; f.acm_pending[slot] = false;
    }
    // Present path. true = f holds a finished frame to emit now (call take()).
    bool present(uint64_t now_us) noexcept {
        if (!armed || latched) { sampling = false; return false; }
        if (sampling) {
            if (f.all) return true;
            reset_frame();                 // no draw at all: sample the next frame instead
            return false;
        }
        if (!last_us || now_us - last_us >= kPeriodUs || now_us < last_us) { sampling = true; reset_frame(); }
        return false;
    }
    // Copies the finished frame out and closes the sampling window (draws stop committing: the serial advances).
    unsigned take(Frame& out) noexcept {
        out = f; sampling = false; reset_frame();
        return ++samples;
    }
    // Lines left for the next frame (one is reserved for the latched line).
    unsigned budget() const noexcept { return lines + 1 >= kMaxLines ? 0u : kMaxLines - 1 - lines; }
    // After the frame was written. Returns the latch reason when the census just latched off (else null).
    const char* finish(uint64_t now_us, unsigned emitted) noexcept {
        last_us = now_us ? now_us : 1; lines += emitted;
        if (samples >= kMaxSamples) latch_why = "samples";
        else if (budget() < 2) latch_why = "lines";
        if (latch_why) { latched = true; sampling = false; return latch_why; }
        return nullptr;
    }
};

// ---- process-wide state (one instance per module through the inline function's static)
struct Shared {
    std::mutex m;     // a leaf lock: the draw path takes it inside the hook-state mutex; the present path alone; nothing is acquired under it
    Accumulator a;
    Frame out;        // present thread only (copied under the lock, formatted after it)
    std::atomic<bool> sampling{false}, armed{false};
};
inline Shared& shared() noexcept { static Shared s; return s; }
inline bool sampling() noexcept { return shared().sampling.load(std::memory_order_relaxed); }   // the draw hook's one relaxed load
inline Admit admit(bool menu, uint32_t vs8, uint32_t ps8, uint8_t topo, bool indexed, bool gs, uint32_t count, uint32_t instances,
                   uint32_t& serial, uint32_t& slot, uint32_t& all, uint32_t& ord) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    return s.a.admit(menu, vs8, ps8, topo, indexed, gs, count, instances, serial, slot, all, ord);
}
inline void commit(uint32_t serial, uint32_t slot, const DrawRec& d) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    s.a.commit(serial, slot, d);
}
inline void commit_acm(uint32_t serial, uint32_t slot, const AcmRec& r) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    s.a.commit_acm(serial, slot, r);
}
// Present-statistics window (5 s): the trigger file's presence now.
inline void set_armed(bool a, bool tail = false, bool acm = false) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    s.a.set_armed(a, tail, acm);
    s.armed.store(s.a.armed, std::memory_order_relaxed);
    s.sampling.store(s.a.sampling, std::memory_order_relaxed);
}
// Every present (Present thread): sink(const char* line) logs one line. Not armed: one relaxed load.
template <class Sink>
inline void present(uint64_t now_us, Sink&& sink) noexcept {
    auto& s = shared();
    if (!s.armed.load(std::memory_order_relaxed)) return;
    unsigned sample = 0, budget = 0;
    {
        std::lock_guard<std::mutex> lock(s.m);
        const bool due = s.a.present(now_us);
        if (due) { sample = s.a.take(s.out); budget = s.a.budget(); }
        s.sampling.store(s.a.sampling, std::memory_order_relaxed);
        if (!due) return;
    }
    const unsigned n = emit_frame(s.out, sample, budget, sink);
    const char* why = nullptr; unsigned samples = 0, lines = 0;
    {
        std::lock_guard<std::mutex> lock(s.m);
        why = s.a.finish(now_us, n);
        samples = s.a.samples; lines = s.a.lines + 1;
        s.sampling.store(false, std::memory_order_relaxed);
    }
    if (why) { char b[kLineLimit + 1]; format_latched(why, samples, lines, b, sizeof b); sink(static_cast<const char*>(b)); }
}

#ifdef _WIN32
// %LOCALAPPDATA%\acz-vr\ui3d-census.arm exists (one GetFileAttributesW; Present thread, once per 5 s window).
inline bool arm_file_present() noexcept {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\acz-vr\\ui3d-census.arm", local) < 0) return false;
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// %LOCALAPPDATA%\acz-vr\ui3d-census-tail.arm exists: TAIL mode (also arms the census by itself).
inline bool tail_file_present() noexcept {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\acz-vr\\ui3d-census-tail.arm", local) < 0) return false;
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// %LOCALAPPDATA%\acz-vr\ui3d-census-acm.arm exists: ACM mode (also arms the census by itself).
inline bool acm_file_present() noexcept {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\acz-vr\\ui3d-census-acm.arm", local) < 0) return false;
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

}  // namespace acz::u3c
