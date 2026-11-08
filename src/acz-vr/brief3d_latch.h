#pragma once
// brief3d_latch.h -- BRIEF3D section 11 (2026-10-30, headset test 6 item 3: "the table shakes like crazy when I move or turn my head").
// SDK-free, header only (the offline tests drive it: tests/acz-vr/brief3d_latch_offline_tests.cpp).
//
// Root cause (brief3d.md 11, step143 live-2210 logs): the table was RENDERED only inside the game's own map draws (60 per second in step143,
// 7-9 in step137) with the eye pose the producer read at that moment, then shown by the compositor in later XR frames (90 per second:
// `layers` 446 vs `blits` 298 per 5 s, `lag-frames` 1, `lag-max` 2, `age-max-ms` 32). Each XR frame therefore showed an image rendered for an
// OLDER head pose, a different age every frame (the 60/90 cadence), and the runtime's rotation-only reprojection cannot remove the head
// TRANSLATION part of that error: the table swims and judders whenever the head moves. The eye guard (eye_clear_of_volume) additionally
// refused whole frames when any table corner came within 5 cm of the eye plane (leaning in, looking up/sideways), which froze the old image.
//
// Fix (b3l:1, default): the game's map draws are LATCHED (their matrix record, root constants, the game's PSO and root signature references,
// and CPU copies of the exact index range, the referenced vertex range and the t11 colour table read from the game's pinned, mapped UPLOAD
// buffers) once per game frame; nothing is recorded into the game's list any more. The XR consumer re-draws that latched scene EVERY XR frame,
// for each eye, with the eye poses of THAT frame (FrameInfo::views), and submits the projection layer with exactly those poses. The table is
// then world-locked like real geometry, independent of the game's draw rate. The eye guard is replaced by per-primitive culling: only grid
// segments with an end closer than 5 cm to an eye plane (or behind it) and icon copies whose bounding sphere reaches that plane are dropped
// for that eye; nothing refuses a frame.
#include "brief3d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace acz::brief3d::latch {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

inline constexpr uint64_t kBufCap = 4ull << 20;      // one latched buffer (the grid VB is 520,192 B [E D-370])
inline constexpr uint64_t kGeomCap = 4ull << 20;     // the whole scene's geometry (IB + VB + t11 blobs, 256-aligned) in one consumer upload span
inline constexpr uint64_t kSmallRead = 4096;         // read whole every time (icon meshes, t11); above: sampled fingerprint + cache
inline constexpr unsigned kSamples = 16;             // 8-byte words sampled for the fingerprint of a large buffer
inline constexpr int64_t kRefreshMs = 1000;          // a cached large buffer is re-read at least this often (changes between the samples)
inline constexpr int64_t kLargeReadGapMs = 50;       // the large-read window (WC memory reads are slow): at most kLargeReadBudget bytes per window
// Headset test 7 (2026-10-31, step160, D:\acz-evidence-archive\headset-test7-20261031 ee-camera.log brief3d-latch): the old rule allowed ONE large
// read per 50 ms window. A map frame needs two large buffers (grid IB 130,048 B + grid VB 520,192 B): whenever both missed in one frame the second
// was ALWAYS refused (b3l-read-rate) and the whole frame dropped, and the IB (read first) could starve the VB for several windows. Live: 225 of
// 507 map frames dropped (scenes 282, incomplete 225, rate 225), scenes at 20-36/s instead of 60 with gaps up to 125 ms (summary-brief3d-latch
// age-max-ms=125): the "very low FPS" of the briefing table. Now (a) a window admits reads up to kLargeReadBudget bytes (the first read always),
// so a frame's IB + VB pair is read together; (b) over the budget, the newest copy of the SAME buffer (va + size) is served when it is at most
// kStaleServeMs old (an unchanged one up to kRefreshMs + kStaleServeMs), so the frame still publishes, with that buffer's content up to
// kStaleServeMs old (the quality trade: a grid that changes every frame updates at the window rate, the scene - icons, content scale - at the
// game's rate); (c) an older copy is re-read even over the budget (starvation guard, at most once per kStaleServeMs per buffer). A buffer never
// read before is still refused over the budget (b3l-read-rate).
inline constexpr uint64_t kLargeReadBudget = 1ull << 20;   // bytes per kLargeReadGapMs window (the IB + VB pair = 650,240 B)
inline constexpr int64_t kStaleServeMs = 250;              // the oldest copy of a changed buffer served over the budget
inline constexpr unsigned kCacheEntries = 6;              // IB + VB + t11 of the two alternating buffer sets
inline constexpr int64_t kCacheIdleClearMs = 2000;        // (review 2026-10-31 F7) no read for this long (= live kScreenGapMs: the screen was left): the cache is emptied
inline constexpr unsigned kMaxRecs = kSelMapCap + kIconCap;   // 132 records per eye (icon copies count against kIconCap as before)
inline constexpr unsigned kRecStride = 512;
inline constexpr uint64_t kRecArea = align_up(2ull * kMaxRecs * kRecStride, kAlign);
inline constexpr uint64_t kCullCap = 256ull << 10;   // culled index bytes per eye (the grid: 65,024 x 2 B = 130,048)
inline constexpr uint64_t kUploadPerFrame = kGeomCap + kRecArea + 2 * kCullCap;   // one consumer allocator's upload span
inline constexpr double kNearM = 0.05;               // the eye plane margin (= EyeCfg::near_m: the PS2 projection's near plane)
inline constexpr uint32_t kIaLineList = 2;           // D3D_PRIMITIVE_TOPOLOGY_LINELIST

// ------------------------------------------------------------------------------------------------ CPU reads of the game's upload buffers
// A small cache keyed by CONTENT (size + sampled fingerprint) and (review 2026-10-31 F7) the source VA: the grid VB alternates between two mapped
// UPLOAD buffers with the same content [E D-370]: each keeps its own entry (6 entries = IB + VB + t11 of both). A content hit is never served for
// another buffer (a new map whose sampled words happen to match), and a gap of kCacheIdleClearMs without any read (the screen was left / a new
// briefing) empties the cache. `read(offset, bytes, dst)` copies from the draw's span (the glue: step3MappedSpan_locked + guardedCpuRead).
// old = over-budget misses served from the same buffer's previous copy; forced = over-budget re-reads (starvation guard); miss-fp / miss-age /
// miss-new = why a large get missed (sampled content changed / the copy was older than kRefreshMs / no copy of this buffer: a new va or size).
struct ReadStats { uint64_t full = 0, hits = 0, bytes = 0, rate = 0, torn = 0, faults = 0, idle_clears = 0, old = 0, forced = 0,
                   miss_fp = 0, miss_age = 0, miss_new = 0; };
class ReadCache {
public:
    template <class Read>
    const char* get(uint64_t bytes, int64_t now_ms, Read&& read, Bytes& out, uint64_t va = 0) {
        out.reset();
        if (last_get_ms_ && now_ms - last_get_ms_ > kCacheIdleClearMs) { clear(); ++st_.idle_clears; }   // F7: a new screen never sees old entries
        last_get_ms_ = now_ms ? now_ms : 1;
        if (!bytes) return "b3l-read-empty";
        if (bytes > kBufCap) return "b3l-read-too-large";
        if (bytes <= kSmallRead) {   // small: one whole read every time (the content is what this draw uses now)
            auto v = std::make_shared<std::vector<uint8_t>>(size_t(bytes));
            if (!read(0, bytes, v->data())) { ++st_.faults; return "b3l-read-fault"; }
            st_.bytes += bytes; ++st_.full;
            out = std::move(v);
            return nullptr;
        }
        uint64_t fp[kSamples];
        if (!sample(bytes, read, fp)) { ++st_.faults; return "b3l-read-fault"; }
        Entry* same = nullptr;   // the newest copy of THIS buffer (va + size), whatever its content
        for (Entry& e : e_)
            if (e.data && e.bytes == bytes && e.va == va && (!same || e.read_ms > same->read_ms)) same = &e;
        const bool same_fp = same && std::memcmp(same->fp, fp, sizeof fp) == 0;
        const int64_t age = same ? now_ms - same->read_ms : 0;
        if (same_fp && age < kRefreshMs) { same->used_ms = now_ms; ++st_.hits; out = same->data; return nullptr; }
        if (!same) ++st_.miss_new; else if (!same_fp) ++st_.miss_fp; else ++st_.miss_age;
        if (!win_ms_ || now_ms - win_ms_ >= kLargeReadGapMs) { win_ms_ = now_ms ? now_ms : 1; win_bytes_ = 0; }
        if (win_bytes_ && win_bytes_ + bytes > kLargeReadBudget) {   // over this window's budget
            if (same && age <= (same_fp ? kRefreshMs + kStaleServeMs : kStaleServeMs)) { same->used_ms = now_ms; ++st_.old; out = same->data; return nullptr; }
            if (!same) {   // no copy to serve: wait - but one waiting buffer is read once it has waited kStaleServeMs (a buffer that
                           // fills every window's budget on its own cannot starve a new one for ever)
                const bool mine = pend_ms_ && pend_va_ == va && pend_bytes_ == bytes;
                if (!mine && (!pend_ms_ || now_ms - pend_seen_ms_ > 2 * kLargeReadGapMs)) { pend_va_ = va; pend_bytes_ = bytes; pend_ms_ = now_ms ? now_ms : 1; }
                const bool waiting = pend_va_ == va && pend_bytes_ == bytes;
                if (waiting) pend_seen_ms_ = now_ms;
                if (!waiting || now_ms - pend_ms_ < kStaleServeMs) { ++st_.rate; return "b3l-read-rate"; }
            }
            ++st_.forced;   // starvation guard: this buffer's copy is too old to serve (or it waited too long for its first copy)
        }
        if (pend_ms_ && pend_va_ == va && pend_bytes_ == bytes) pend_ms_ = 0;
        win_bytes_ += bytes;
        auto v = std::make_shared<std::vector<uint8_t>>(size_t(bytes));
        if (!read(0, bytes, v->data())) { ++st_.faults; return "b3l-read-fault"; }
        st_.bytes += bytes; ++st_.full;
        uint64_t after[kSamples], copy[kSamples];
        if (!sample(bytes, read, after)) { ++st_.faults; return "b3l-read-fault"; }
        for (unsigned i = 0; i < kSamples; ++i) std::memcpy(&copy[i], v->data() + offset(bytes, i), 8);
        if (std::memcmp(fp, after, sizeof fp) != 0 || std::memcmp(fp, copy, sizeof fp) != 0) { ++st_.torn; return "b3l-read-torn"; }
        Entry* slot = same;   // a buffer's new copy replaces its old one (one entry per buffer)
        if (!slot) { slot = &e_[0]; for (Entry& e : e_) { if (!e.data) { slot = &e; break; } if (e.used_ms < slot->used_ms) slot = &e; } }
        slot->data = v; slot->bytes = bytes; slot->va = va; std::memcpy(slot->fp, fp, sizeof fp); slot->read_ms = now_ms; slot->used_ms = now_ms;
        out = std::move(v);
        return nullptr;
    }
    const ReadStats& stats() const noexcept { return st_; }
    void clear() noexcept { for (Entry& e : e_) e = Entry{}; pend_ms_ = 0; }
    static uint64_t offset(uint64_t bytes, unsigned i) noexcept { return (bytes - 8) * i / (kSamples - 1) / 4 * 4; }
private:
    template <class Read> static bool sample(uint64_t bytes, Read& read, uint64_t fp[kSamples]) {
        for (unsigned i = 0; i < kSamples; ++i) { fp[i] = 0; if (!read(offset(bytes, i), 8, reinterpret_cast<uint8_t*>(&fp[i]))) return false; }
        return true;
    }
    struct Entry { Bytes data; uint64_t bytes = 0, va = 0; uint64_t fp[kSamples] = {}; int64_t read_ms = 0, used_ms = 0; };
    Entry e_[kCacheEntries];
    int64_t win_ms_ = 0, last_get_ms_ = 0, pend_ms_ = 0, pend_seen_ms_ = 0;
    uint64_t win_bytes_ = 0, pend_va_ = 0, pend_bytes_ = 0;
    ReadStats st_;
};

// The [lo, hi] vertex index range of `count` indices of `isz` bytes (2 or 4); the strip cut value (0xFFFF / 0xFFFFFFFF) is skipped when
// `skip_cut`. false = no index left or a bad size.
inline bool index_range(const uint8_t* ib, uint32_t isz, uint32_t count, bool skip_cut, uint32_t& lo, uint32_t& hi) noexcept {
    lo = UINT32_MAX; hi = 0;
    if (!ib || !count || (isz != 2 && isz != 4)) return false;
    const uint32_t cut = isz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = 0;
        if (isz == 2) { uint16_t s; std::memcpy(&s, ib + 2 * size_t(i), 2); v = s; } else std::memcpy(&v, ib + 4 * size_t(i), 4);
        if (skip_cut && v == cut) continue;
        lo = std::min(lo, v); hi = std::max(hi, v); any = true;
    }
    return any;
}
// Largest |position| (float3 at offset 0 of each vertex) of a latched vertex range: the icon mesh's bounding radius in its own model units.
inline double mesh_radius(const uint8_t* vb, size_t bytes, uint32_t stride) noexcept {
    double r = 0;
    if (!vb || stride < 12) return 0;
    for (size_t o = 0; o + 12 <= bytes; o += stride) {
        float p[3]; std::memcpy(p, vb + o, 12);
        const double d = std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]);
        if (std::isfinite(d)) r = std::max(r, d);
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ the t11 ColorMap the grid indexes
// Headset test 7 ("Briefing render doesnt fully render now at all") and null run step164 runAGU1 (eye0-table dump t66335): the latched table
// showed only its FAR 4 of 64 grid rows (a 1 m x 5 cm strip at the far edge; geometry, fit and placement exact: the strip's far edge lands on
// the predicted row). DrawSelMap_v colours a vertex with ColorMap[int(in.w)] (dxc -dumpbin 384ade02: `fptosi in.w` -> bufferLoad on the t11
// ROOT SRV, no bounds check), and the latch copied only the first 4 KiB of t11 (256 entries, the fmcolor.gim 256x1 guess). The live colour
// index runs far past 255 (row-major over the grid [I: 4 rows x 64 = 256 entries visible]); every vertex past entry 255 read the zeroed upload
// memory after our copy: colour 0, alpha 0, invisible. Now the glue reads every entry the latched vertices index (bounded by the resource and
// kBufCap) and layout() refuses a SelMap draw whose t11 copy does not cover its vertices (the frame keeps the previous scene; a persistent
// shortfall reaches the automatic b3l:0 fallback, which draws with the game's own t11).
inline constexpr uint64_t kColourEntryBytes = 16;   // StructuredBuffer<ColorMap> = one float4
inline constexpr uint64_t kColourDefaultEntries = 256;   // stride < 16 (no colour component): the old 4 KiB read
// Entries [0, n) indexed by the vertices of a latched SelMap range: n = max(int(w)) + 1, w = the float at byte 12 of each vertex, truncated toward
// zero like fptosi. Indices the shader cannot read defined (negative, non-finite, >= 2^31) are counted in `bad` and not covered (the game's own
// read is undefined there too). stride < 16 = no colour component: kColourDefaultEntries.
inline uint64_t colour_entries(const uint8_t* vb, size_t bytes, uint32_t stride, uint64_t* bad = nullptr) noexcept {
    if (bad) *bad = 0;
    if (!vb || stride < 16) return kColourDefaultEntries;
    uint64_t n = 0;
    for (size_t o = 0; o + 16 <= bytes; o += stride) {
        float w; std::memcpy(&w, vb + o + 12, 4);
        if (!(w > -1.0f) || !(w < 2147483648.0f)) { if (bad) ++*bad; continue; }   // NaN fails both
        const uint64_t i = uint64_t(int64_t(w));
        if (i + 1 > n) n = i + 1;
    }
    return n;
}
// The strip-cut rule of index_range for a draw's topology / index size / IA strip-cut value (the glue and layout() agree on it).
inline bool skips_cut(uint32_t topo, uint32_t isz, uint32_t cut) noexcept {
    const bool strip = topo == 3 || topo == 5 || topo == 11 || topo == 13;
    return strip && ((isz == 2 && cut == 1) || (isz == 4 && cut == 2));
}
// (review 2026-11-01 F4) Entries [0, n) indexed by the vertices the INDEX LIST references (not every vertex of the latched range [lo, hi]: a vertex no
// index draws, with a stale large w, would raise n and turn a fully covered draw into b3l-t11-short). Vertex = index + base (the rebased base vertex).
// A REFERENCED vertex whose index the shader cannot read defined (w <= -1, NaN, >= 2^31) or that lies outside the latched vb is counted in `bad`:
// layout() refuses such a draw (b3l-t11-index), because fptosi of a negative w reads BEFORE our t11 copy through the unbounded root SRV.
// stride < 16 = no colour component: kColourDefaultEntries.
inline uint64_t colour_entries_indexed(const uint8_t* vb, size_t bytes, uint32_t stride, const uint8_t* ib, uint32_t isz, uint32_t count,
                                       bool skip_cut, int64_t base, uint64_t* bad = nullptr) noexcept {
    if (bad) *bad = 0;
    if (!vb || stride < 16) return kColourDefaultEntries;
    if (!ib || (isz != 2 && isz != 4)) { if (bad) *bad = 1; return 0; }
    const uint32_t cut = isz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    uint64_t n = 0;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t v = 0;
        if (isz == 2) { uint16_t s; std::memcpy(&s, ib + 2 * size_t(k), 2); v = s; } else std::memcpy(&v, ib + 4 * size_t(k), 4);
        if (skip_cut && v == cut) continue;
        const int64_t at = int64_t(v) + base;
        if (at < 0 || uint64_t(at) * stride + 16 > bytes) { if (bad) ++*bad; continue; }
        float w; std::memcpy(&w, vb + size_t(at) * stride + 12, 4);
        if (!(w > -1.0f) || !(w < 2147483648.0f)) { if (bad) ++*bad; continue; }   // NaN fails both
        const uint64_t i = uint64_t(int64_t(w));
        if (i + 1 > n) n = i + 1;
    }
    return n;
}
// The t11 bytes the glue reads for a SelMap draw: every indexed entry (at least one), bounded by what the resource holds from the bound VA
// (`avail`, whole entries). A result below entries x 16 = the resource itself ends early: layout() then refuses the draw.
inline uint64_t t11_read_bytes(uint64_t entries, uint64_t avail) noexcept {
    const uint64_t need = (entries ? entries : 1) * kColourEntryBytes;
    const uint64_t have = avail / kColourEntryBytes * kColourEntryBytes;
    return need < have ? need : have;
}
// colour_entries of the same immutable latched vb blob (a cache hit returns the same object) is not scanned again (glue: hook-state mutex held).
// (review 2026-11-01 F4) keyed on the vb AND ib blobs and the index walk (colour_entries_indexed); `bad` of the cached walk is returned as well.
struct ColourMemo {
    std::weak_ptr<const std::vector<uint8_t>> vb, ib; uint32_t stride = 0, isz = 0, count = 0; bool skip = false; int64_t base = 0;
    uint64_t entries = 0, bad_n = 0;
    uint64_t get(const std::shared_ptr<const std::vector<uint8_t>>& b, const std::shared_ptr<const std::vector<uint8_t>>& i, uint32_t s, uint32_t isz_,
                 uint32_t count_, bool skip_, int64_t base_, uint64_t* bad = nullptr) {
        if (bad) *bad = 0;
        if (!b) return kColourDefaultEntries;
        if (!(s == stride && isz_ == isz && count_ == count && skip_ == skip && base_ == base && vb.lock() == b && ib.lock() == i)) {
            const bool fits = i && uint64_t(count_) * (isz_ ? isz_ : 1) <= i->size();
            entries = colour_entries_indexed(b->data(), b->size(), s, fits ? i->data() : nullptr, isz_, count_, skip_, base_, &bad_n);
            vb = b; ib = i; stride = s; isz = isz_; count = count_; skip = skip_; base = base_;
        }
        if (bad) *bad = bad_n;
        return entries;
    }
};

// ------------------------------------------------------------------------------------------------ the world-fixed table plane (b3p:1)
// Headset tests 5-9 (docs/world-render/brief3d-worldlock.md "Headset test 9"): since u3q:1 (2026-10-30b) the table is submitted as one quad
// PER EYE posed at that eye's located pose every frame, and every test since reported the table shaking / moving with the head, through the
// latch (per-XR-frame render), the table-depth plane and 90 Hz renders with no busy frame. The picture of such a quad depends on the head
// ORIENTATION, so whatever SteamVR does to a quad between our submission and the display (its own pose for the quad, a texture of another
// frame) moves the whole table by the head rotation of that interval. Here the quad is posed ONCE per placement (a plane through the table,
// facing the seated head) and each eye's picture is the central projection of the scene onto that plane from the eye's POSITION (an off-axis
// view whose image rectangle is the quad): head rotation does not change the picture at all, and a texture of another frame is wrong only by
// the parallax of that frame's head translation for content off the plane.
inline constexpr double kPlaneMaxHalfM = 1.2;    // the plane rectangle's half extent cap (about 54 degrees from the seated head)
inline constexpr double kPlaneMinEyeM = 0.15;    // an eye closer to the plane (or behind it) uses the per-eye quad for that frame
inline constexpr double kPlaneMaxTan = 6.0;      // an edge further than ~80 degrees off the plane normal (grazing view): per-eye quad
struct TablePlane {
    double centre[3] = {0, 0, 0};                // LOCAL: the quad centre
    double orientation[4] = {0, 0, 0, 1};        // LOCAL: x = right (the table's x), y = up in the plane, z = the normal toward the seated head
    double axis[3][3] = {};                      // the same three axes as LOCAL vectors
    double width = 0, height = 0;                // metres
    bool valid = false;
};
inline void quat_mul(const double a[4], const double b[4], double o[4]) noexcept {   // apply b, then a
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// The plane for a placement made `eye_up_m` below / `eye_back_m` behind the seated head (b3y negated, b3d): through the table centre at half the
// grid relief, normal toward that head, the rectangle = the table volume (top at `top_m`, the content cap) projected from that head, plus a margin,
// each side capped at kPlaneMaxHalfM. Content that a moved head sees outside the rectangle is cut at its edge.
inline TablePlane table_plane(const Placement& pl, const Volume& v, double top_m, double eye_up_m, double eye_back_m) noexcept {
    TablePlane p;
    if (!pl.valid || !(v.half_x > 0) || !(v.half_z > 0) || !(eye_back_m > 0.05)) return p;
    const double lift = 0.5 * std::fmax(0.0, v.height), top = std::fmax(v.height, top_m);
    const double a = eye_up_m - lift, b = eye_back_m, len = std::sqrt(a * a + b * b);
    const double sa = a / len, ca = b / len;   // table frame: n = (0, sa, ca), u = (0, ca, -sa), r = (1, 0, 0); head = (0, eye_up, eye_back)
    double amin = 1e9, amax = -1e9, bmin = 1e9, bmax = -1e9;
    for (int c = 0; c < 8; ++c) {
        const double x = (c & 1) ? v.half_x : -v.half_x, y = (c & 2) ? top : 0.0, z = (c & 4) ? v.half_z : -v.half_z;
        const double d[3] = {x, y - eye_up_m, z - eye_back_m};             // head -> corner
        const double nd = d[1] * sa + d[2] * ca;                           // < 0: the corner is on the plane's side of the head
        if (!(nd < -1e-3)) { amin = -kPlaneMaxHalfM; amax = kPlaneMaxHalfM; continue; }   // beside / behind the head: the widest rectangle
        const double t = -len / nd;                                        // n . (P0 - head) = -len
        const double X[3] = {t * d[0], eye_up_m + t * d[1] - lift, eye_back_m + t * d[2]};   // relative to P0 = (0, lift, 0)
        const double pa = X[0], pb = X[1] * ca - X[2] * sa;
        amin = std::fmin(amin, pa); amax = std::fmax(amax, pa); bmin = std::fmin(bmin, pb); bmax = std::fmax(bmax, pb);
    }
    const double ma = 0.1 * (amax - amin) + 0.03, mb = 0.1 * (bmax - bmin) + 0.03;
    amin = std::fmax(amin - ma, -kPlaneMaxHalfM); amax = std::fmin(amax + ma, kPlaneMaxHalfM);
    bmin = std::fmax(bmin - mb, -kPlaneMaxHalfM); bmax = std::fmin(bmax + mb, kPlaneMaxHalfM);
    if (!(amax - amin > 0.05) || !(bmax - bmin > 0.05)) return p;
    const double ac = 0.5 * (amin + amax), bc = 0.5 * (bmin + bmax);
    const double ct[3] = {ac, lift + bc * ca, -bc * sa};                   // P0 + ac r + bc u (table frame)
    double h[4]; xform(table_to_local(pl), ct, h);
    for (int k = 0; k < 3; ++k) p.centre[k] = h[k];
    const double qy[4] = {0, std::sin(pl.yaw / 2), 0, std::cos(pl.yaw / 2)}, al = std::atan2(a, b);
    const double qx[4] = {std::sin(-al / 2), 0, 0, std::cos(-al / 2)};     // table frame: x rotation by -alpha takes +z to n
    quat_mul(qy, qx, p.orientation);
    for (int k = 0; k < 3; ++k) { const double e[3] = {k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0}; quat_rotate(p.orientation, e, p.axis[k]); }
    p.width = amax - amin; p.height = bmax - bmin;
    p.valid = std::isfinite(p.centre[0] + p.centre[1] + p.centre[2] + p.orientation[0] + p.orientation[1] + p.orientation[2] + p.orientation[3]);
    return p;
}
// The view that draws the scene onto the plane from an eye at `pos`: the plane's orientation, the eye's position, and the FOV whose image
// rectangle at the plane is exactly the quad (tangent = (edge - eye) / eye distance, all in the plane's axes). nullptr = `out` set; else why
// (the eye too close to / behind the plane, or a grazing view): the caller uses the per-eye quad for that frame.
inline const char* plane_view(const TablePlane& p, const double pos[3], EyeView& out) noexcept {
    if (!p.valid) return "b3p-no-plane";
    const double d[3] = {pos[0] - p.centre[0], pos[1] - p.centre[1], pos[2] - p.centre[2]};
    double c[3];
    for (int k = 0; k < 3; ++k) c[k] = d[0] * p.axis[k][0] + d[1] * p.axis[k][1] + d[2] * p.axis[k][2];
    if (!(c[2] >= kPlaneMinEyeM)) return "b3p-eye-at-plane";
    const double t[4] = {(-0.5 * p.width - c[0]) / c[2], (0.5 * p.width - c[0]) / c[2], (0.5 * p.height - c[1]) / c[2], (-0.5 * p.height - c[1]) / c[2]};
    for (double v : t) if (!(std::fabs(v) <= kPlaneMaxTan)) return "b3p-grazing";
    for (int k = 0; k < 3; ++k) out.position[k] = pos[k];
    for (int k = 0; k < 4; ++k) out.orientation[k] = p.orientation[k];
    for (int k = 0; k < 4; ++k) out.fov[k] = std::atan(t[k]);
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ the latched scene
// What the glue reads for one map draw (hook-state mutex held). pso/root keep the game's own objects alive (the proxy's retained references)
// for as long as any scene or in-flight consumer frame uses them.
struct Geom {
    Bytes ib, vb, t11;
    uint32_t index_bytes = 0, index_count = 0, stride = 0;
    int32_t base_vertex = 0;               // rebased: the latched vb starts at the draw's lowest referenced vertex
    uint32_t ia_topology = 0, strip_cut = 0;
    uint32_t t11_param = UINT32_MAX;
    uint32_t extra_vb = 0;                 // other bound IA slots (not latched; counted)
    std::shared_ptr<void> pso, root;       // ID3D12PipelineState / ID3D12RootSignature (type-erased; the deleter releases)
};
struct Draw {
    Kind kind = Kind::none;
    uint8_t rec[kModelIconSrtBytes] = {};
    float b10[2] = {0, 0};
    uint32_t t10_param = UINT32_MAX, b10_param = UINT32_MAX;
    Geom g;
    unsigned copies = 1;                   // icons: the icon + its stalk dots (brief3d 9.2); SelMap 1
    float m3[kMaxIconCopies][16] = {};     // icons: matrix kIconModelMatrix per copy
    bool air = false;
    double radius = 0;                     // icons: mesh radius (model units of the icon mesh)
    Box vbox;                              // SelMap: the box of its latched vertices (grid model space)
    uint64_t colours = 0;                  // SelMap: ColorMap entries its REFERENCED vertices index (colour_entries_indexed; layout())
    uint64_t colour_bad = 0;               // SelMap: referenced vertices with an unreadable colour index (review 2026-11-01 F4: refused)
    uint64_t ib_off = 0, vb_off = 0, t11_off = 0;   // layout(): offsets inside the scene's geometry span
};
struct Scene {
    uint64_t serial = 0;
    int64_t published_ms = 0;              // the producer's Present clock (GetTickCount64)
    uint32_t space_generation = 0;         // ENCODED LOCAL generation of the placement
    Fit fit;                               // the content-squashed fit of this game frame
    Fit base_fit;                          // the latched (unsquashed) fit
    Placement placement;
    TablePlane plane;                      // b3p:1: the world-fixed quad of this placement (invalid = per-eye quads)
    DepthMode depth = DepthMode::reverse;
    float clear_depth = 0.0f;
    float b10[2] = {0, 0};
    double content_k = 1.0;
    std::vector<Draw> draws;
    uint64_t geom_bytes = 0;
    unsigned records = 0, selmap = 0, icons = 0;
};
// Assigns the geometry offsets (256-aligned; a blob shared by several draws is laid out once), the record count and the SelMap vertex boxes
// (taken from `prev` when the vb blob is the same object). nullptr = fits the caps.
inline const char* layout(Scene& s, const Scene* prev = nullptr) {
    std::vector<std::pair<const void*, uint64_t>> seen;
    uint64_t at = 0; unsigned recs = 0;
    auto place = [&](const Bytes& b, uint64_t& off) -> bool {
        if (!b || b->empty()) { off = 0; return true; }
        for (const auto& p : seen) if (p.first == b.get()) { off = p.second; return true; }
        at = align_up(at, 256);
        if (b->size() > kGeomCap || at > kGeomCap - b->size()) return false;
        off = at; at += b->size(); seen.emplace_back(b.get(), off);
        return true;
    };
    for (Draw& d : s.draws) {
        if (!place(d.g.ib, d.ib_off) || !place(d.g.vb, d.vb_off) || !place(d.g.t11, d.t11_off)) return "b3l-geometry-over-cap";
        recs += d.kind == Kind::selmap ? 1u : d.copies;
        if (d.kind == Kind::selmap) {
            bool have = false;
            if (prev) for (const Draw& p : prev->draws)
                if (p.kind == Kind::selmap && p.g.vb == d.g.vb && p.g.stride == d.g.stride && p.vbox.valid && p.g.ib == d.g.ib &&
                    p.g.index_bytes == d.g.index_bytes && p.g.index_count == d.g.index_count && p.g.base_vertex == d.g.base_vertex &&
                    p.g.ia_topology == d.g.ia_topology && p.g.strip_cut == d.g.strip_cut)
                    { d.vbox = p.vbox; d.colours = p.colours; d.colour_bad = p.colour_bad; have = true; break; }
            if (!have && d.g.vb && d.g.stride) {
                (void)box_from_vertices(d.g.vb->data(), d.g.vb->size(), d.g.stride, 0, uint32_t(d.g.vb->size() / d.g.stride), d.vbox);
                // review 2026-11-01 F4: only the vertices the index list references (an IB shorter than its count is refused as bad)
                const bool ibOk = d.g.ib && uint64_t(d.g.index_count) * (d.g.index_bytes ? d.g.index_bytes : 1) <= d.g.ib->size();
                d.colours = colour_entries_indexed(d.g.vb->data(), d.g.vb->size(), d.g.stride, ibOk ? d.g.ib->data() : nullptr, d.g.index_bytes,
                                                   d.g.index_count, skips_cut(d.g.ia_topology, d.g.index_bytes, d.g.strip_cut), d.g.base_vertex, &d.colour_bad);
            }
            if (!d.vbox.valid) return "b3l-grid-box";
            // review 2026-11-01 F4: a drawn vertex whose colour index is negative / NaN / huge reads outside our t11 copy (the root SRV has no bounds)
            if (d.colour_bad) return "b3l-t11-index";
            // every ColorMap entry the vertices index must be inside our t11 copy: the root SRV has no bounds, a read past it is invisible (alpha 0)
            if (!d.g.t11 || d.g.t11->size() / kColourEntryBytes < d.colours) return "b3l-t11-short";
        }
    }
    if (recs > kMaxRecs) return "b3l-records-over-cap";
    s.geom_bytes = at; s.records = recs;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ the consumer's per-XR-frame plan
// Per eye: the eye matrix of THIS frame's view, every record (SelMap: the eye matrix; ModelIcon: the game's record with matrix 0 = the eye
// matrix and matrix 3 = the copy's model matrix), and per draw what is drawn: mode 0 = the whole latched index range, 1 = the culled index
// list at cull_off (cull_count indices), 2 = nothing; mask = the icon copies drawn (bit c).
enum : uint8_t { kModeFull = 0, kModeCulled = 1, kModeSkip = 2 };
struct EyePlan {
    Mat4 m;
    std::vector<uint8_t> rec;                         // records x kRecStride
    std::vector<uint8_t> mode;
    std::vector<uint16_t> mask;
    std::vector<uint64_t> cull_off;
    std::vector<uint32_t> cull_count;
    std::vector<uint8_t> cull;                        // culled indices (the draw's index format), concatenated
    std::vector<double> w;                            // scratch: per-vertex w
};
struct FramePlan {
    std::shared_ptr<const Scene> scene;
    EyeView eyes[2];
    EyePlan eye[2];
    uint64_t clipped = 0, culled_copies = 0, skipped = 0;
};
inline double w_of(const Mat4& m, double x, double y, double z) noexcept { return x * m.m[3] + y * m.m[7] + z * m.m[11] + m.m[15]; }
// SelMap: decide full / culled / skipped for one eye. Line lists are culled per segment; any other topology is drawn whole only when every
// referenced vertex is in front of the eye plane margin.
inline void plan_grid(const Draw& d, const Mat4& m, double near_m, EyePlan& ep, size_t i, FramePlan& fp) {
    ep.mode[i] = kModeFull;
    bool clear = d.vbox.valid;
    for (int c = 0; c < 8 && clear; ++c) {
        const double x = (c & 1) ? d.vbox.hi[0] : d.vbox.lo[0], y = (c & 2) ? d.vbox.hi[1] : d.vbox.lo[1], z = (c & 4) ? d.vbox.hi[2] : d.vbox.lo[2];
        clear = w_of(m, x, y, z) >= near_m;   // w is affine: every vertex inside the box is in front too
    }
    if (clear) return;
    const Geom& g = d.g;
    if (!g.vb || !g.ib || !g.stride || (g.index_bytes != 2 && g.index_bytes != 4)) { ep.mode[i] = kModeSkip; ++fp.skipped; return; }
    const size_t nv = g.vb->size() / g.stride;
    ep.w.resize(nv);
    for (size_t v = 0; v < nv; ++v) {
        float p[3]; std::memcpy(p, g.vb->data() + v * g.stride, 12);
        ep.w[v] = w_of(m, p[0], p[1], p[2]);
    }
    auto idx = [&](uint32_t k) -> int64_t {
        uint32_t v = 0;
        if (g.index_bytes == 2) { uint16_t s; std::memcpy(&s, g.ib->data() + 2 * size_t(k), 2); v = s; } else std::memcpy(&v, g.ib->data() + 4 * size_t(k), 4);
        return int64_t(v) + g.base_vertex;
    };
    auto ok = [&](int64_t v) { return v >= 0 && size_t(v) < nv && std::isfinite(ep.w[size_t(v)]) && ep.w[size_t(v)] >= near_m; };
    if (g.ia_topology != kIaLineList) {   // not expected for the grid: whole or nothing
        for (uint32_t k = 0; k < g.index_count; ++k) {
            const int64_t v = idx(k);
            const bool cut = (g.index_bytes == 2 && v - g.base_vertex == 0xFFFF) || (g.index_bytes == 4 && v - g.base_vertex == 0xFFFFFFFFll);
            if (!cut && !ok(v)) { ep.mode[i] = kModeSkip; ++fp.skipped; return; }
        }
        return;
    }
    const uint64_t start = ep.cull.size();
    uint32_t kept = 0;
    for (uint32_t k = 0; k + 1 < g.index_count; k += 2) {
        if (!ok(idx(k)) || !ok(idx(k + 1))) { ++fp.clipped; continue; }
        if (ep.cull.size() + 2ull * g.index_bytes > kCullCap) { ep.cull.resize(size_t(start)); ep.mode[i] = kModeSkip; ++fp.skipped; return; }
        const uint8_t* src = g.ib->data() + size_t(k) * g.index_bytes;
        ep.cull.insert(ep.cull.end(), src, src + 2 * g.index_bytes);
        kept += 2;
    }
    if (kept == (g.index_count & ~1u)) { ep.cull.resize(size_t(start)); return; }   // nothing clipped after all: the whole range
    if (!kept) { ep.cull.resize(size_t(start)); ep.mode[i] = kModeSkip; ++fp.skipped; return; }
    ep.mode[i] = kModeCulled; ep.cull_off[i] = start; ep.cull_count[i] = kept;
}
// nullptr = planned. Every record is rebuilt from `eyes` (THIS frame's located views): nothing of a previous frame's pose survives.
inline const char* plan_frame(const std::shared_ptr<const Scene>& sp, const EyeView eyes[2], FramePlan& out, double near_m = kNearM) {
    out.scene = sp; out.clipped = out.culled_copies = out.skipped = 0;
    if (!sp) return "b3l-no-scene";
    const Scene& s = *sp;
    if (s.draws.empty() || !s.records || s.records > kMaxRecs) return "b3l-scene-empty";
    EyeCfg cfg; cfg.near_m = near_m; cfg.depth = s.depth;
    const double mpu = s.base_fit.metres_per_unit * std::max(1.0, s.base_fit.vexag);
    for (unsigned e = 0; e < 2; ++e) {
        out.eyes[e] = eyes[e];
        EyePlan& ep = out.eye[e];
        if (const char* why = eye_matrix(s.fit, s.placement, eyes[e], s.b10, cfg, ep.m)) return why;
        const size_t n = s.draws.size();
        ep.rec.assign(size_t(s.records) * kRecStride, 0);
        ep.mode.assign(n, kModeFull); ep.mask.assign(n, 0); ep.cull_off.assign(n, 0); ep.cull_count.assign(n, 0); ep.cull.clear();
        unsigned r = 0;
        for (size_t i = 0; i < n; ++i) {
            const Draw& d = s.draws[i];
            if (d.kind == Kind::selmap) {
                if (const char* why = selmap_record(ep.m, ep.rec.data() + size_t(r) * kRecStride)) return why;
                plan_grid(d, ep.m, near_m, ep, i, out);
                if (ep.mode[i] != kModeSkip) ep.mask[i] = 1;
                r += 1;
                continue;
            }
            for (unsigned c = 0; c < d.copies && c < kMaxIconCopies; ++c) {
                uint8_t* rec = ep.rec.data() + size_t(r + c) * kRecStride;
                if (const char* why = modelicon_record(d.rec, 0, ep.m, rec)) return why;
                std::memcpy(rec + 64 * kIconModelMatrix, d.m3[c], 64);
                // the copy's bounding sphere: centre = matrix 3's translation (grid model space), radius = mesh radius x the matrix's longest row
                const float* m3 = d.m3[c];
                double row = 0;
                for (int k = 0; k < 3; ++k) row = std::max(row, std::sqrt(double(m3[k * 4]) * m3[k * 4] + double(m3[k * 4 + 1]) * m3[k * 4 + 1] + double(m3[k * 4 + 2]) * m3[k * 4 + 2]));
                const double rad_m = d.radius * row * mpu;
                const double w = w_of(ep.m, m3[12], m3[13], m3[14]);
                if (std::isfinite(w) && std::isfinite(rad_m) && w - rad_m >= near_m) ep.mask[i] |= uint16_t(1u << c);
                else ++out.culled_copies;
            }
            if (!ep.mask[i]) ep.mode[i] = kModeSkip;
            r += d.copies;
        }
        if (r != s.records) return "b3l-records-mismatch";
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ test/diagnostic helpers
// OpenXR view (pose + FOV) -> LOCAL point for an NDC position and depth produced by DrawSelMap_v's own maths (emulate_selmap) with the reverse
// depth convention (d = near / w). Independent of the PS2 projection code (the offline test's world-lock check).
inline bool unproject_reverse(const EyeView& e, double near_m, const double sv[3], double local[3]) noexcept {
    if (!(sv[2] > 0) || !std::isfinite(sv[0] + sv[1] + sv[2])) return false;
    const double w = near_m / sv[2];
    const double tl = std::tan(e.fov[0]), tr = std::tan(e.fov[1]), tu = std::tan(e.fov[2]), td = std::tan(e.fov[3]);
    const double tx = (sv[0] * (tr - tl) + (tr + tl)) / 2.0, ty = (sv[1] * (tu - td) + (tu + td)) / 2.0;
    const double v[3] = {tx * w, ty * w, -w};
    double r[3]; quat_rotate(e.orientation, v, r);
    for (int k = 0; k < 3; ++k) local[k] = r[k] + e.position[k];
    return true;
}

// ------------------------------------------------------------------------------------------------ log line (ee-camera.log, every 5 s window)
struct Stats { uint64_t draws = 0, scenes = 0, incomplete = 0, refused = 0, geom_refused = 0, extra_vb = 0; };
inline int format_line(char* buf, size_t n, const Stats& s, const ReadStats& r, uint64_t serial, uint64_t geom_bytes, unsigned recs, const char* why,
                       bool fallback = false, const char* fallback_why = nullptr) noexcept {
    auto c = [](uint64_t v) { return static_cast<unsigned long long>(v > 9999999ull ? 9999999ull : v); };
    return std::snprintf(buf, n, "brief3d-latch;scenes=%llu;incomplete=%llu;draws=%llu;refused=%llu;geom-refused=%llu;extra-vb=%llu;serial=%llu;"
        "geom-kb=%llu;records=%u;reads=%llu;hits=%llu;read-kb=%llu;rate=%llu;torn=%llu;faults=%llu;why=%.32s;fallback=%u;fallback-why=%.32s;"
        "old=%llu;forced=%llu;miss=%llu/%llu/%llu",
        c(s.scenes), c(s.incomplete), c(s.draws), c(s.refused), c(s.geom_refused), c(s.extra_vb), c(serial), c(geom_bytes / 1024), recs,
        c(r.full), c(r.hits), c(r.bytes / 1024), c(r.rate), c(r.torn), c(r.faults), why ? why : "none", fallback ? 1u : 0u, fallback_why ? fallback_why : "none",
        c(r.old), c(r.forced), c(r.miss_fp), c(r.miss_age), c(r.miss_new));
}

} // namespace acz::brief3d::latch
