#pragma once
// briefing_observe.h -- BRIEFING-3D (docs/world-render/briefing-3d-map.md) read-only diagnostic: a per-window histogram of EVERY draw the hooked
// command lists record, keyed by the PSO's creation shader hashes (first 8 hex digits of the VS/PS hashes), primitive topology type, indexed and
// geometry-shader flags. Purpose: find the mission-briefing 3D map's draws (DrawSelMap_v terrain grid, DrawModelIcon_v unit icons) on screens where
// no cockpit exists (the step-5 observers only look at lists with DrawCPT draws, so they are blind there).
// Trigger (no config key; the 32-key cap is full): the file %LOCALAPPDATA%\acz-vr\briefing-observe.arm. Its presence is polled once per present-
// statistics window (5 s, ee_camera_probe_win.cpp pacing_tick) on the Present thread; absent file = the draw hook pays one relaxed atomic load.
// Output: two lines per armed window in ee-camera.log (and the diagnostics log): `briefing-observe;event=window;...` and `...;event=top;...`.
// Nothing is copied, read from game memory or changed; no GPU work. Header-only: d3d12_hook.cpp is compiled into many offline test executables.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace acz::brobs {

inline constexpr unsigned kEntries = 32;  // distinct (vs8,ps8,topology,indexed,gs) keys kept per window; more = counted in `overflow`
inline constexpr unsigned kTop = 8;       // entries printed in the `top` line (by draws, descending)
// VS shader HASHes as logged by the proxy's PSO records, named by matching the container sizes of the game's own shader/*.cso files (a.arc listing)
// and, for 384ade02/d5a41c2f, by `dxc -dumpbin` of the extracted .cso (debug name + `shader hash:` line) [E: docs/world-render/briefing-3d-map.md].
inline constexpr uint32_t kSelMapVs8 = 0x384ade02u;     // DrawSelMap_v: float4 (x,y,z,colour index), t10 SrtData = one 4x4 matrix, t11 colour table
inline constexpr uint32_t kModelIconVs8 = 0xd5a41c2fu;  // DrawModelIcon_v: float3 position + float3 normal, t10 SrtData 272 bytes (4 matrices + vector)
inline constexpr uint32_t kAcmVs8 = 0xb569109eu;        // DrawACM_v (world objects)
inline constexpr uint32_t kCptVs8 = 0xf4cfd928u;        // DrawCPT_v (cockpit)
// Named by unique container size in the test-19 PSO records (briefing-3d-map.md 2.1) [E]; only used to label `top`/`rest` items.
inline constexpr uint32_t kPs2Vs8 = 0x534f29cau;        // ps2_v / ps2AW_v (identical DXIL): the emulated-GS 2D/pre-projected draws (panels, text)
inline constexpr uint32_t kMovieVs8 = 0x15c8c0bdu;      // movie_yuv_v (pre-rendered movies)
inline constexpr uint32_t kExtra2Vs8 = 0xa16573beu;     // extra2_v
inline constexpr unsigned kRestLinesMax = 3;            // DEBRIEF-3D: at most this many `rest` lines per armed window (keys ranked after kTop)

struct Entry {
    uint32_t vs8 = 0, ps8 = 0;
    uint8_t topo = 0;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE as created: 1 point, 2 line, 3 triangle, 4 patch, 0 unknown
    bool indexed = false, gs = false;
    uint64_t draws = 0, units = 0, instances = 0;  // units = vertex or index counts summed
    uint32_t maxUnits = 0;
};
struct Window {
    Entry e[kEntries]{};
    unsigned n = 0;
    uint64_t draws = 0, overflow = 0, noPso = 0;
    uint32_t presents = 0;
};

// Adds one draw (caller holds the window's lock). vs8 == 0 && ps8 == 0 means the PSO was not known to the proxy (counted as noPso, not keyed).
inline void note_draw(Window& w, uint32_t vs8, uint32_t ps8, uint8_t topo, bool indexed, bool gs, uint32_t count, uint32_t instances) noexcept {
    ++w.draws;
    if (!vs8 && !ps8) { ++w.noPso; return; }
    for (unsigned i = 0; i < w.n; ++i) {
        Entry& x = w.e[i];
        if (x.vs8 == vs8 && x.ps8 == ps8 && x.topo == topo && x.indexed == indexed && x.gs == gs) {
            ++x.draws; x.units += count; x.instances += instances; if (count > x.maxUnits) x.maxUnits = count;
            return;
        }
    }
    if (w.n >= kEntries) { ++w.overflow; return; }
    Entry& x = w.e[w.n++];
    x.vs8 = vs8; x.ps8 = ps8; x.topo = topo; x.indexed = indexed; x.gs = gs;
    x.draws = 1; x.units = count; x.instances = instances; x.maxUnits = count;
}

inline const char* vs_name(uint32_t vs8) noexcept {
    switch (vs8) {
        case kSelMapVs8: return "SelMap";
        case kModelIconVs8: return "ModelIcon";
        case kAcmVs8: return "ACM";
        case kCptVs8: return "CPT";
        case kPs2Vs8: return "ps2";
        case kMovieVs8: return "movie";
        case kExtra2Vs8: return "extra2";
        default: return "";
    }
}
inline uint64_t draws_of(const Window& w, uint32_t vs8) noexcept {
    uint64_t d = 0;
    for (unsigned i = 0; i < w.n; ++i) if (w.e[i].vs8 == vs8) d += w.e[i].draws;
    return d;
}
inline uint64_t units_of(const Window& w, uint32_t vs8) noexcept {
    uint64_t u = 0;
    for (unsigned i = 0; i < w.n; ++i) if (w.e[i].vs8 == vs8) u += w.e[i].units;
    return u;
}

// Line 1: the window summary. `briefing` = 1 when the window holds DrawSelMap_v draws (the briefing/overview map; [I] until a live window shows it).
inline int format_window(const Window& w, double ms, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    const double frames = w.presents ? double(w.presents) : 0.0;
    const int r = std::snprintf(out, cap,
        "briefing-observe;event=window;ms=%.0f;presents=%u;draws=%llu;draws-per-present=%.1f;keys=%u;overflow=%llu;no-pso=%llu;briefing=%u;"
        "selmap=%llu(units=%llu);modelicon=%llu(units=%llu);acm=%llu;cpt=%llu",
        ms, w.presents, static_cast<unsigned long long>(w.draws), frames > 0 ? double(w.draws) / frames : 0.0, w.n,
        static_cast<unsigned long long>(w.overflow), static_cast<unsigned long long>(w.noPso), draws_of(w, kSelMapVs8) ? 1u : 0u,
        static_cast<unsigned long long>(draws_of(w, kSelMapVs8)), static_cast<unsigned long long>(units_of(w, kSelMapVs8)),
        static_cast<unsigned long long>(draws_of(w, kModelIconVs8)), static_cast<unsigned long long>(units_of(w, kModelIconVs8)),
        static_cast<unsigned long long>(draws_of(w, kAcmVs8)), static_cast<unsigned long long>(draws_of(w, kCptVs8)));
    return r < 0 ? 0 : (size_t(r) >= cap ? int(cap - 1) : r);
}
// Keys ranked by draws (descending; ties by insertion order). Returns the number of ranked keys (= w.n).
inline unsigned rank(const Window& w, unsigned (&order)[kEntries]) noexcept {
    for (unsigned i = 0; i < w.n; ++i) order[i] = i;
    std::sort(order, order + w.n, [&](unsigned a, unsigned b) { return w.e[a].draws != w.e[b].draws ? w.e[a].draws > w.e[b].draws : a < b; });
    return w.n;
}
// Appends ranked keys [first, w.n) as vs8/ps8/t<topology><i|n><g|->/draws/units/max[/name] items after the prefix already in out[0..n), stopping
// before cap at an item boundary (and after `limit` items). Returns the new length; *next = the first rank NOT printed.
inline int append_items(const Window& w, const unsigned (&order)[kEntries], unsigned first, unsigned limit, char* out, size_t cap, int n,
                        unsigned* next) noexcept {
    unsigned k = first;
    for (; k < w.n && k - first < limit; ++k) {
        const Entry& x = w.e[order[k]];
        char item[96];
        const char* nm = vs_name(x.vs8);
        const int m = std::snprintf(item, sizeof item, "%s%08x/%08x/t%u%c%c/%llu/%llu/%u%s%s", k > first ? "," : "", x.vs8, x.ps8, unsigned(x.topo),
                                    x.indexed ? 'i' : 'n', x.gs ? 'g' : '-', static_cast<unsigned long long>(x.draws),
                                    static_cast<unsigned long long>(x.units), x.maxUnits, nm[0] ? "/" : "", nm);
        if (m <= 0 || size_t(n) + size_t(m) >= cap) break;
        std::snprintf(out + n, cap - size_t(n), "%s", item);
        n += m;
    }
    if (next) *next = k;
    return n;
}
// Line 2: the kTop keys by draws: vs8/ps8/t<topology><i|n><g|->/draws/units/max[/name]. Truncation-safe (stops before cap).
inline int format_top(const Window& w, char* out, size_t cap) noexcept {
    if (!out || !cap) return 0;
    unsigned order[kEntries];
    rank(w, order);
    int n = std::snprintf(out, cap, "briefing-observe;event=top;fields=vs8/ps8/t<topology:1pt,2line,3tri><i|n:indexed><g|-:gs>/draws/units/max-units;top=");
    if (n < 0) { out[0] = 0; return 0; }
    if (size_t(n) >= cap) return int(cap - 1);
    return append_items(w, order, 0, kTop, out, cap, n, nullptr);
}
// DEBRIEF-3D (docs/world-render/debrief-3d-map.md): the keys ranked AFTER the top line, so a screen dominated by ps2_v 2D draws still names every
// other shader/topology it uses. `from` = the first rank (0-based) to print; returns the line length (0 = nothing to print: from >= w.n or no
// buffer) and *next = the first rank not printed (the caller prints another line from there, at most kRestLinesMax lines per window).
// Format: `briefing-observe;event=rest;from=<1-based rank>;keys=<w.n>;overflow=<w.overflow>;items=<item>,...` (same items as `top`).
inline int format_rest(const Window& w, unsigned from, char* out, size_t cap, unsigned* next) noexcept {
    if (next) *next = w.n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (from >= w.n) return 0;
    unsigned order[kEntries];
    rank(w, order);
    int n = std::snprintf(out, cap, "briefing-observe;event=rest;from=%u;keys=%u;overflow=%llu;items=", from + 1, w.n,
                          static_cast<unsigned long long>(w.overflow));
    if (n < 0) { out[0] = 0; return 0; }
    if (size_t(n) >= cap) { if (next) *next = w.n; return int(cap - 1); }  // prefix alone does not fit: stop (no progress possible)
    unsigned k = from;
    n = append_items(w, order, from, kEntries, out, cap, n, &k);
    if (k == from) { if (next) *next = w.n; return n; }  // not even one item fits: stop instead of repeating the same line
    if (next) *next = k;
    return n;
}

// Process-wide state (one instance per module through the inline function's static).
struct Shared {
    std::mutex m;  // guards w and start_us only; never held across another lock, formatting or I/O by the draw side
    Window w{};
    uint64_t start_us = 0;
    std::atomic<bool> armed{false};
};
inline Shared& shared() noexcept { static Shared s; return s; }
inline bool armed() noexcept { return shared().armed.load(std::memory_order_relaxed); }

// Draw path (any recording thread), only after armed() returned true.
inline void draw(uint32_t vs8, uint32_t ps8, uint8_t topo, bool indexed, bool gs, uint32_t count, uint32_t instances) noexcept {
    auto& s = shared();
    std::lock_guard<std::mutex> lock(s.m);
    note_draw(s.w, vs8, ps8, topo, indexed, gs, count, instances);
}
// Present path: counts a present while armed.
inline void present() noexcept {
    auto& s = shared();
    if (!s.armed.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(s.m);
    ++s.w.presents;
}
// Present-statistics window end: `arm` = the trigger file is present now. Takes the finished window out (returns false when nothing is to be
// logged: not armed before and now). The caller formats and logs outside any lock.
inline bool roll(bool arm, uint64_t now_us, Window& out, double& ms) noexcept {
    auto& s = shared();
    const bool was = s.armed.exchange(arm, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(s.m);
    const bool emit = was;  // a window is reported only when it was armed over its whole span
    if (emit) { out = s.w; ms = s.start_us && now_us > s.start_us ? double(now_us - s.start_us) / 1000.0 : 0.0; }
    s.w = Window{};
    s.start_us = now_us;
    return emit;
}

#ifdef _WIN32
// %LOCALAPPDATA%\acz-vr\briefing-observe.arm exists (one GetFileAttributesW; Present thread, once per 5 s window).
inline bool arm_file_present() noexcept {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return false;
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\acz-vr\\briefing-observe.arm", local) < 0) return false;
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

}  // namespace acz::brobs
