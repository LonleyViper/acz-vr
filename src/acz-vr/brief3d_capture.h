#pragma once
// brief3d_capture.h -- BRIEF3D phase-2 capture (docs/world-render/brief3d.md sections 6.6 and 7, live check B3-2/B3-5): a READ-ONLY logging pass over
// the briefing map's own draws (DrawSelMap_v 384ade02 grid, DrawModelIcon_v d5a41c2f icons; identified by the VS creation hash as briefing_observe.h).
// Trigger (no config key): the file %LOCALAPPDATA%\acz-vr\brief3d-capture.arm, polled once per present-statistics window (5 s) on the Present thread.
// While armed, at most ONE frame per second is sampled: the draws recorded between two presents [I: the frame boundary is the present, not the
// list]. A sampled frame that holds at least one SelMap draw gives ONE `brief3d-capture;...` line in ee-camera.log (< kLineLimit - kLinePrefix
// bytes for any values, offline-tested); a sampled frame without one gives nothing and the next frame is sampled instead.
// Per sampled frame: the FIRST SelMap draw's facts (t10 record 64 B through the shared safe reader, cb10, PSO depth/blend/RT/DSV/samples/topology,
// IA topology, index format and count, VB view and whether it lies in a pinned mapped UPLOAD span) and up to kIconCap ModelIcon records (272 B each,
// the four matrices compared with the grid's matrix of the same frame).
// Nothing is written to the GPU, to a command list or to game memory. Reads: step3MappedSpan_locked (pinned, mapped, inside the resource) and two
// SEH-guarded copies that must agree (read_twice below, the fx_smoke capture rule); every failure is a named reason in the line (fail closed).
// Not armed: the draw path pays one relaxed atomic load. Header-only (d3d12_hook.cpp is compiled into many offline test executables).
#include "brief3d.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace acz::b3cap {

using acz::brief3d::Kind;
inline constexpr unsigned kRecordCap = acz::brief3d::kModelIconSrtBytes;   // 272: the largest record read
inline constexpr unsigned kIconCap = 128;                                  // icon records read per sampled frame (live: 63); more = counted only
inline constexpr uint64_t kPeriodUs = 1000000;                             // one line per second at most
inline constexpr size_t kLineLimit = acz::brief3d::kLineLimit, kLinePrefix = acz::brief3d::kLinePrefix;

// The draw's kind by its VS creation hash only (as briefing_observe); brief3d::classify (PS, PSO topology, indexed) is logged separately (`cls`).
inline Kind kind_of(uint32_t vs8) noexcept {
    return vs8 == acz::brief3d::kSelMapVs8 ? Kind::selmap : vs8 == acz::brief3d::kModelIconVs8 ? Kind::modelicon : Kind::none;
}

// The shared safe read: `span(address, va, bytes, p)` = step3MappedSpan_locked (nullptr = p points at `bytes` CPU-visible bytes of a pinned, mapped
// UPLOAD buffer, inside the resource; else its reason), `read(p, dst, n)` = guardedCpuRead (SEH-guarded copy). Two reads that must agree.
// nullptr = `out` holds the record; else the first failing step's name. `scratch` >= bytes.
template <class Span, class Read>
inline const char* read_twice(Span&& span, Read&& read, uint64_t address, uint64_t va, unsigned bytes, uint8_t* out, uint8_t* scratch) noexcept {
    if (!out || !scratch || !bytes || bytes > kRecordCap) return "b3cap-record-size";
    if (!va) return "b3cap-t10-va-unknown";
    if (!address) return "b3cap-t10-address-unresolved";
    const uint8_t* p = nullptr;
    if (const char* why = span(address, va, uint64_t(bytes), p)) return why;
    if (!p) return "b3cap-span-null";
    if (!read(p, out, size_t(bytes)) || !read(p, scratch, size_t(bytes))) return "b3cap-read-fault";
    if (std::memcmp(out, scratch, bytes) != 0) return "b3cap-record-torn";   // being written while read
    return nullptr;
}
// cb10 from the tracked root constants: the first two 32-bit words, both written since the root was bound, finite.
inline const char* b10_pair(const uint32_t* words, const bool* written, size_t n, float out[2]) noexcept {
    out[0] = out[1] = 0.0f;
    if (!words || !written || n < 2) return "b3cap-b10-short";
    if (!written[0] || !written[1]) return "b3cap-b10-unwritten";
    std::memcpy(&out[0], &words[0], 4); std::memcpy(&out[1], &words[1], 4);
    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) return "b3cap-b10-nonfinite";
    return nullptr;
}

// Everything the hook copies about one map draw (under the hook-state mutex). Icons fill only kind/count/ia/t10_why/rec.
struct DrawFacts {
    Kind kind = Kind::none;
    bool classified = false;          // brief3d::classify(vs8, ps8, PSO topology type, indexed) == kind
    uint32_t count = 0, instances = 0;
    const char* t10_why = "b3cap-not-read";   // nullptr = rec holds the record (srt_bytes(kind) bytes); else the first failing step (root, VA, span, read)
    uint8_t rec[kRecordCap]{};
    const char* b10_why = "b3cap-not-read"; float b10[2]{};
    bool pso_known = false, depth_enable = false, depth_write = false, stencil = false, blend_enable = false;
    uint32_t depth_func = 0, blend[6]{}, rt_count = 0, rtv0 = 0, dsv = 0, samples = 0, topo_type = 0;
    uint32_t ia_topology = 0, index_stride = 0;
    uint64_t vb_va = 0, vb_bytes = 0; uint32_t vb_stride = 0;
    const char* vb_why = "b3cap-not-read";    // nullptr = the VB view lies inside a pinned, mapped UPLOAD span (no vertex byte is read)
};

// Equality of one float4x4 with the reference (the brief3d::icon_camera_index rule: 1e-5 relative to the reference's largest magnitude).
inline bool matrix_equal(const uint8_t* m64, const uint8_t* ref64) noexcept {
    float a[16], r[16]; std::memcpy(a, m64, 64); std::memcpy(r, ref64, 64);
    double mag = 0; for (float v : r) { if (!std::isfinite(v)) return false; mag = std::fmax(mag, std::fabs(double(v))); }
    if (!(mag > 0)) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(a[i]) || std::fabs(double(a[i]) - double(r[i])) > 1e-5 * mag) return false;
    return true;
}

// One sampled frame.
struct Frame {
    uint64_t serial = 0;
    uint32_t sel_draws = 0, icon_draws = 0, icon_reserved = 0, icon_read = 0, icon_unread = 0, late = 0;
    bool sel_taken = false, have_sel = false;
    DrawFacts sel;
    uint32_t icon_ia = 0, icon_max_count = 0;
    const char* icon_why = nullptr;   // first icon refusal
    uint8_t icon_m[kIconCap][256]{};  // the four matrices of each icon record read
    uint32_t icon_cnt[kIconCap]{};    // the draw's index count of each icon record read (the icon model shape: aircraft wedge, flag, block ...)
};
struct Summary {   // the icon comparison of one frame
    uint32_t eq[4]{}, one = 0, none = 0, multi = 0;
    const char* why = nullptr;   // nullptr = compared; else why not (no grid matrix) or the first icon refusal
};
inline Summary compare_icons(const Frame& f) noexcept {
    Summary s;
    if (!f.have_sel || f.sel.t10_why) { s.why = "b3cap-no-grid-matrix"; return s; }
    for (uint32_t i = 0; i < f.icon_read; ++i) {
        unsigned hits = 0;
        for (unsigned k = 0; k < 4; ++k) if (matrix_equal(f.icon_m[i] + 64 * k, f.sel.rec)) { ++s.eq[k]; ++hits; }
        hits == 1 ? ++s.one : hits == 0 ? ++s.none : ++s.multi;
    }
    s.why = f.icon_why;
    return s;
}

enum class Admit : uint8_t { skip, count, selmap_full, icon_read };

// The sampling state (no lock inside: Shared wraps it).
struct Accumulator {
    bool armed = false, sampling = false;
    uint64_t last_emit_us = 0, serial = 1;
    uint32_t lines = 0;
    Frame f;
    void reset_frame() noexcept {   // the icon matrices are not cleared: only [0, icon_read) is ever read
        f.serial = ++serial;
        f.sel_draws = f.icon_draws = f.icon_reserved = f.icon_read = f.icon_unread = f.late = 0;
        f.sel_taken = f.have_sel = false; f.sel = DrawFacts{};
        f.icon_ia = f.icon_max_count = 0; f.icon_why = nullptr;
    }
    void set_armed(bool a) noexcept { if (armed && !a) { sampling = false; reset_frame(); } armed = a; }
    // Draw path: what to do with a draw of kind k (and the frame serial to commit to).
    Admit admit(Kind k, uint64_t& serial_out) noexcept {
        serial_out = f.serial;
        if (!armed || !sampling || k == Kind::none) return Admit::skip;
        if (k == Kind::selmap) {
            if (f.sel_draws < 0xffffffffu) ++f.sel_draws;
            if (!f.sel_taken) { f.sel_taken = true; return Admit::selmap_full; }
            return Admit::count;
        }
        if (f.icon_draws < 0xffffffffu) ++f.icon_draws;
        if (f.icon_reserved < kIconCap) { ++f.icon_reserved; return Admit::icon_read; }
        return Admit::count;
    }
    void commit(uint64_t serial_in, const DrawFacts& d) noexcept {
        if (serial_in != f.serial) { if (f.late < 0xffffffffu) ++f.late; return; }   // the frame closed between admit and commit
        if (d.kind == Kind::selmap) { f.sel = d; f.have_sel = true; return; }
        if (d.kind != Kind::modelicon) return;
        if (f.icon_read + f.icon_unread == 0) f.icon_ia = d.ia_topology;
        if (d.count > f.icon_max_count) f.icon_max_count = d.count;
        if (d.t10_why) { ++f.icon_unread; if (!f.icon_why) f.icon_why = d.t10_why; return; }
        if (f.icon_read < kIconCap) { f.icon_cnt[f.icon_read] = d.count; std::memcpy(f.icon_m[f.icon_read++], d.rec, 256); }
    }
    // Present path. Returns true when `out` should be formatted from `f` now (then call finish_line()).
    bool present(uint64_t now_us) noexcept {
        if (!armed) { sampling = false; return false; }
        if (sampling) {
            if (f.sel_draws) return true;   // a map frame: the line is due
            reset_frame();                  // no map draw: sample the next frame instead
            return false;
        }
        if (!last_emit_us || now_us - last_emit_us >= kPeriodUs || now_us < last_emit_us) { sampling = true; reset_frame(); }
        return false;
    }
    void finish_line(uint64_t now_us) noexcept { last_emit_us = now_us ? now_us : 1; sampling = false; if (lines < 0xffffffffu) ++lines; reset_frame(); }
};

// ---- the line (< kLineLimit - kLinePrefix bytes for any values: every number is clamped, every reason is width-limited)
inline unsigned clampu(uint64_t v, unsigned hi) noexcept { return v > hi ? hi : unsigned(v); }
inline int format_matrix(char* out, size_t cap, const uint8_t* rec64) noexcept {
    size_t n = 0;
    for (int i = 0; i < 16; ++i) {
        float v; std::memcpy(&v, rec64 + 4 * i, 4);
        const int m = std::snprintf(out + n, cap - n, "%s%.4g", i ? "," : "", double(v));
        if (m < 0 || n + size_t(m) >= cap) { out[n] = 0; return int(n); }
        n += size_t(m);
    }
    return int(n);
}
inline int format_line(char* out, size_t cap, const Frame& f) noexcept {
    if (!out || !cap) return 0;
    const DrawFacts& d = f.sel;
    const bool have = f.have_sel;
    char m[200] = "-";
    if (have && !d.t10_why) format_matrix(m, sizeof m, d.rec);
    char b10[32] = "-";
    if (have) {
        if (d.b10_why) std::snprintf(b10, sizeof b10, "%.24s", d.b10_why);
        else std::snprintf(b10, sizeof b10, "%.6g,%.6g", double(d.b10[0]), double(d.b10[1]));
    }
    const Summary s = compare_icons(f);
    const int r = std::snprintf(out, cap,
        "brief3d-capture;sel=%u;cls=%u;ix=%u*%u;t10=%.32s;m=%s;b10=%s;pso=%u%u%u,%u,%u:%u.%u.%u:%u.%u.%u;rt=%u:%u,%u,%u;tt=%u;ia=%u;"
        "vb=%llx+%llu/%u:%.24s;ic=%u/%u/%u;iia=%u;imax=%u;eq=%u.%u.%u.%u;cam=%u/%u/%u;iw=%.24s",
        clampu(f.sel_draws, 99), have && d.classified ? 1u : 0u, have ? clampu(d.index_stride, 9) : 0u, have ? clampu(d.count, 999999) : 0u,
        !have ? "b3cap-not-captured" : d.t10_why ? d.t10_why : "ok", m, b10,
        have && d.depth_enable ? 1u : 0u, have && d.depth_write ? 1u : 0u, have && d.stencil ? 1u : 0u, have ? clampu(d.depth_func, 9) : 0u,
        have && d.blend_enable ? 1u : 0u, clampu(d.blend[0], 99), clampu(d.blend[1], 99), clampu(d.blend[2], 9), clampu(d.blend[3], 99),
        clampu(d.blend[4], 99), clampu(d.blend[5], 9),
        clampu(d.rt_count, 9), clampu(d.rtv0, 999), clampu(d.dsv, 999), clampu(d.samples, 99), clampu(d.topo_type, 9), clampu(d.ia_topology, 99),
        static_cast<unsigned long long>(d.vb_va & 0xffffffffffffull), static_cast<unsigned long long>(d.vb_bytes > 99999999ull ? 99999999ull : d.vb_bytes),
        clampu(d.vb_stride, 999), !have ? "-" : d.vb_why ? d.vb_why : "mapped-upload",
        clampu(f.icon_draws, 999), clampu(f.icon_read, 999), clampu(f.icon_unread, 999), clampu(f.icon_ia, 99), clampu(f.icon_max_count, 99999),
        clampu(s.eq[0], 999), clampu(s.eq[1], 999), clampu(s.eq[2], 999), clampu(s.eq[3], 999), clampu(s.one, 999), clampu(s.none, 999),
        clampu(s.multi, 999), s.why ? s.why : "none");
    return r < 0 ? 0 : (size_t(r) >= cap ? int(cap - 1) : r);
}

// ---- the icon census (live finding 2026-10-28: the game's own icon draws fall from 63 to ~17-21 per frame when the briefing map is zoomed; no
// gate refuses them, so WHICH icons the game stops drawing decides whether it is game-side culling or a mode switch). One line per sampled frame:
// the icons read are grouped by their index count (= the icon model); per group: how many, and the min..max of the translation row's y (element 13)
// of matrices 1, 2 and 3 (matrix 0 is the camera). Compare a pre-zoom and a post-zoom line: the groups that vanish are the missing icons.
inline constexpr unsigned kCensusGroups = 5;
struct CensusGroup { uint32_t count = 0, n = 0; float lo[3]{}, hi[3]{}; };
struct Census { CensusGroup g[kCensusGroups]; unsigned groups = 0, more = 0, read = 0, nonfinite = 0; };
inline Census census_of(const Frame& f) noexcept {
    Census c; c.read = f.icon_read;
    for (uint32_t i = 0; i < f.icon_read; ++i) {
        unsigned gi = 0;
        while (gi < c.groups && c.g[gi].count != f.icon_cnt[i]) ++gi;
        if (gi == c.groups) {
            if (c.groups == kCensusGroups) { ++c.more; continue; }
            ++c.groups; c.g[gi].count = f.icon_cnt[i];
            for (int k = 0; k < 3; ++k) { c.g[gi].lo[k] = INFINITY; c.g[gi].hi[k] = -INFINITY; }
        }
        CensusGroup& g = c.g[gi]; ++g.n;
        for (int k = 0; k < 3; ++k) {
            float y; std::memcpy(&y, f.icon_m[i] + 64 * (k + 1) + 4 * 13, 4);
            if (!std::isfinite(y)) { ++c.nonfinite; continue; }
            if (y < g.lo[k]) g.lo[k] = y;
            if (y > g.hi[k]) g.hi[k] = y;
        }
    }
    return c;
}
inline int format_census(char* out, size_t cap, const Frame& f) noexcept {
    if (!out || !cap) return 0;
    const Census c = census_of(f);
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n + 1 >= cap) return;
        const int m = std::snprintf(out + n, cap - n, fmt, a...);
        if (m > 0) n = (n + size_t(m) >= cap) ? cap - 1 : n + size_t(m);
    };
    put("brief3d-icons;draws=%u;read=%u;groups=%u;more=%u;nonfinite=%u", clampu(f.icon_draws, 999), clampu(c.read, 999), c.groups, clampu(c.more, 999), clampu(c.nonfinite, 999));
    for (unsigned i = 0; i < c.groups; ++i) {
        const CensusGroup& g = c.g[i];
        put(";c%u:n%u", clampu(g.count, 99999), clampu(g.n, 999));
        for (int k = 0; k < 3; ++k) {
            if (g.lo[k] > g.hi[k]) put(":y%d=-", k + 1);
            else put(":y%d=%.4g..%.4g", k + 1, double(g.lo[k]), double(g.hi[k]));
        }
    }
    out[n < cap ? n : cap - 1] = 0;
    return int(n);
}

// The position census (brief3d.md 9.1, verifies the zoom magnification): per index-count group the min..max of matrix 3's translation x and z
// (elements 12 and 14; y is on the brief3d-icons line; the unit position in the grid's model space [I]) and of its element 0 (the icon's scale).
// Worst case 449 B < kLineLimit - kLinePrefix (5 groups, every value 9 characters at %.3g; offline-tested). One line per sampled frame, next to
// the brief3d-icons line. Expect overview: x/z inside -32..32; zoomed: the remaining c15 group (PIXY, CIPHER) with x/z inside -32..32 near the
// region centre and s ~15x the overview's if the icons are magnified with the map (else unchanged).
inline int format_census_pos(char* out, size_t cap, const Frame& f) noexcept {
    if (!out || !cap) return 0;
    struct G { uint32_t count = 0, n = 0; float lo[3]{}, hi[3]{}; };
    G g[kCensusGroups]; unsigned groups = 0, more = 0;
    for (uint32_t i = 0; i < f.icon_read; ++i) {
        unsigned gi = 0;
        while (gi < groups && g[gi].count != f.icon_cnt[i]) ++gi;
        if (gi == groups) {
            if (groups == kCensusGroups) { ++more; continue; }
            ++groups; g[gi].count = f.icon_cnt[i];
            for (int k = 0; k < 3; ++k) { g[gi].lo[k] = INFINITY; g[gi].hi[k] = -INFINITY; }
        }
        ++g[gi].n;
        static const int el[3] = {12, 14, 0};
        for (int k = 0; k < 3; ++k) {
            float v; std::memcpy(&v, f.icon_m[i] + 64 * 3 + 4 * el[k], 4);
            if (!std::isfinite(v)) continue;
            if (v < g[gi].lo[k]) g[gi].lo[k] = v;
            if (v > g[gi].hi[k]) g[gi].hi[k] = v;
        }
    }
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n + 1 >= cap) return;
        const int m = std::snprintf(out + n, cap - n, fmt, a...);
        if (m > 0) n = (n + size_t(m) >= cap) ? cap - 1 : n + size_t(m);
    };
    put("brief3d-icons-pos;read=%u;groups=%u;more=%u", clampu(f.icon_read, 999), groups, clampu(more, 999));
    static const char nm[3] = {'x', 'z', 's'};
    for (unsigned i = 0; i < groups; ++i) {
        put(";c%u:n%u", clampu(g[i].count, 99999), clampu(g[i].n, 999));
        for (int k = 0; k < 3; ++k) {
            if (g[i].lo[k] > g[i].hi[k]) put(":%c=-", nm[k]);
            else put(":%c=%.3g..%.3g", nm[k], double(g[i].lo[k]), double(g[i].hi[k]));
        }
    }
    out[n < cap ? n : cap - 1] = 0;
    return int(n);
}

// ---- process-wide state (one instance per module through the inline function's static)
struct Shared {
    std::mutex m;   // a leaf lock: the draw path takes it inside the hook-state mutex, the present path alone; nothing else is acquired under it
    Accumulator a;
    std::atomic<bool> sampling{false};   // mirror of a.sampling for the draw path's single relaxed load
    std::atomic<bool> armed{false};      // mirror of a.armed for the present path's lock-free early return
};
inline Shared& shared() noexcept { static Shared s; return s; }
inline bool sampling() noexcept { return shared().sampling.load(std::memory_order_relaxed); }
inline Admit admit(Kind k, uint64_t& serial) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    return s.a.admit(k, serial);
}
inline void commit(uint64_t serial, const DrawFacts& d) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    s.a.commit(serial, d);
}
// Present-statistics window (5 s): the trigger file's presence now.
inline void set_armed(bool a) noexcept {
    auto& s = shared(); std::lock_guard<std::mutex> lock(s.m);
    s.a.set_armed(a);
    s.armed.store(s.a.armed, std::memory_order_relaxed);
    s.sampling.store(s.a.sampling, std::memory_order_relaxed);
}
// Every present: > 0 = a line of that length is in `out` (the caller logs it outside any lock).
// out2 (optional): the icon census line (format_census) when the frame read at least one icon; *n2 = its length (0 = none).
// out3 (optional): the position census line (format_census_pos) under the same condition; *n3 = its length (0 = none).
inline int present(uint64_t now_us, char* out, size_t cap, char* out2 = nullptr, size_t cap2 = 0, int* n2 = nullptr,
                   char* out3 = nullptr, size_t cap3 = 0, int* n3 = nullptr) noexcept {
    auto& s = shared();
    if (n2) *n2 = 0;
    if (n3) *n3 = 0;
    if (!s.armed.load(std::memory_order_relaxed)) return 0;   // never armed: one relaxed load, no lock
    std::lock_guard<std::mutex> lock(s.m);
    int n = 0;
    if (s.a.present(now_us)) {
        n = format_line(out, cap, s.a.f);
        if (n2 && out2 && s.a.f.icon_read) *n2 = format_census(out2, cap2, s.a.f);
        if (n3 && out3 && s.a.f.icon_read) *n3 = format_census_pos(out3, cap3, s.a.f);
        s.a.finish_line(now_us);
    }
    s.sampling.store(s.a.sampling, std::memory_order_relaxed);
    return n;
}

#ifdef _WIN32
// %LOCALAPPDATA%\acz-vr\brief3d-capture.arm exists (one GetFileAttributesW; Present thread, once per 5 s window).
inline bool arm_file_present() noexcept {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\acz-vr\\brief3d-capture.arm", local) < 0) return false;
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

}  // namespace acz::b3cap
