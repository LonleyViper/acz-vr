#pragma once
// ee_counter_search.h -- STORES-RT launch-signal SEARCH, wide version (option stwatch:1 of xr_cockpit_model; docs/world-render/aircraft-auto.md section 13).
// READ-ONLY, SDK-free, no lock and no allocation on the render path (everything here runs on the probe's worker thread).
//
// Why: the 32 KiB window of ee_store_watch.h found no word that followed the HUD "MSSL" count (null run AGB2: 50 -> 46 after four taps), so the counter is outside
// that window or is not a 32-bit int. This searches the WHOLE 64 MiB E6 block (which holds the emulated PS2 EE RAM; E6 structure = block + kWinA) for the
// count at every aligned u8 / u16 / u32 / float position, in two phases:
//   Phase 1 (seed): positions whose value equals `target` (stwatch-start:50, the HUD count at the first lock). The first scan after a lock seeds; further scans
//                   seed again (every kSeedPeriodMs) only while nothing has moved yet and kSeedMs has not run out (a counter that is filled after the lock).
//   Phase 2 (step): every kPeriodMs a candidate is re-read; it survives only while it is unchanged or counts DOWN by 1..kMaxDrop since the last scan (50 -> 49 -> 48
//                   ...). Any other change (a rise, a bigger fall, a non-integer float) kills it. Candidates that stay quiet are KEPT for the whole flight (step168: a
//                   4 s grace prune after an unrelated early mover - five u8 at stride 0x98 that fell 50 -> 49 117 s before the first missile - dropped the real
//                   counter); the memory is bounded by kMaxCand. Seeding continues for kSeedMs after the lock whether or not something moved.
// Track list: up to kTrackMax u8 offsets (explicit `sttrack:<hex>,<hex>..` or the first movers) are read every scan and logged as
//   store-search;event=track;scan=..;t-ms=..;list=<off>=<value>,...   on every change and at least every kSummaryMs (kMaxTrackLines lines), even after the candidate died.
// Survivors that moved are logged after every scan that changed one (at most kMaxLines lines, kListMax offsets per line, widths of one offset grouped):
//   store-search;event=survivors;scan=..;t-ms=..;target=50;alive=..;moved=..;groups=..;list=<off-in-block hex>(ee=<hex>|rel=<signed from the E6 structure>|w=124f|v=46|d=4);...
// `ee=` is the block offset read as an EE address (valid only if the EE RAM sits at the block start; 32 MiB), `w=` the widths that survive at that offset
// (1 = u8, 2 = u16, 4 = u32, f = float), `v` the current value, `d` the total decrease. A counter 50 -> 46 after four launches shows v=46;d=4.
// store-search;event=summary every kSummaryMs says how many candidates/scans/unreadable chunks there are, so a quiet search is told from a dead one.
//
// Cost: one 1 MiB chunk buffer + at most kMaxCand * 8 bytes of candidates (16 MiB, twice during a scan); a scan reads the chunks that hold candidates (all chunks while seeding).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "ee_e6_locator.h"
#include "hud_msl.h"   // review 2026-11-01 F6: the MSL-HUD reader owns the launch mailbox unless mslhud:0

namespace acz::e6search {

inline constexpr uint32_t kChunkBytes = 1u << 20;
inline constexpr uint32_t kMaxCand = 2u << 20;
inline constexpr uint32_t kMaxDrop = 2;                 // a sample may span two quick launches
inline constexpr unsigned kMaxLines = 40, kListMax = 8;
inline constexpr double kPeriodMs = 1000.0, kSeedPeriodMs = 5000.0, kSeedMs = 30000.0, kSummaryMs = 10000.0;
inline constexpr unsigned kTrackMax = 8, kMaxTrackLines = 600;
inline constexpr uint32_t kDefaultTarget = 50;
inline constexpr uint64_t kEeRamBytes = 32ull << 20;

enum Kind : uint8_t { kU8 = 0, kU16 = 1, kU32 = 2, kF32 = 3 };
inline constexpr char kKindChar[4] = {'1', '2', '4', 'f'};
inline constexpr uint32_t kKindBytes[4] = {1, 2, 4, 4};

struct Cand {                 // 8 bytes
    uint32_t off;             // byte offset inside the block
    uint16_t cur;             // current value (always 0..target while alive)
    uint8_t kind;
    uint8_t steps;            // total decrease (saturating)
};

// value at p as a small integer; false when it cannot be one (u32 > 0xFFFF, a float that is not an exact integer in 0..0xFFFF)
inline bool decode(uint8_t kind, const uint8_t* p, uint32_t& v) noexcept {
    switch (kind) {
    case kU8: v = p[0]; return true;
    case kU16: { uint16_t x; std::memcpy(&x, p, 2); v = x; return true; }
    case kU32: { uint32_t x; std::memcpy(&x, p, 4); if (x > 0xFFFFu) return false; v = x; return true; }
    default: { float f; std::memcpy(&f, p, 4); if (!(f >= 0.0f && f <= 65535.0f)) return false; const uint32_t i = uint32_t(f); if (float(i) != f) return false; v = i; return true; }
    }
}

class CounterSearch {
public:
    // `block_bytes` is the scanned length (64 MiB in the game; tests use small images), `chunk` the read size.
    bool start(uint32_t target = kDefaultTarget, uint64_t block_bytes = e6loc::kBlockBytes, uint32_t chunk = kChunkBytes) noexcept {
        if (active_) return true;
        if (target < 1 || target > 0xFFFFu || !block_bytes || !chunk) return false;
        buf_.reset(new (std::nothrow) uint8_t[size_t(chunk) + 8]);
        if (!buf_) return false;
        target_ = target; block_bytes_ = block_bytes; chunk_ = chunk; active_ = true;
        return true;
    }
    bool active() const noexcept { return active_; }
    // One call per worker iteration. `block_base` = the locked E6 block base (0 = not locked: nothing is read; the state is kept for a relock of the same block),
    // `struct_base` = block_base + kWinA. A different block restarts the search. Returns the number of lines emitted.
    unsigned tick(e6loc::ReadMem& mem, uint64_t block_base, uint64_t struct_base, double now_ms, e6loc::Sink& sink) noexcept {
        if (!active_ || !block_base) return 0;
        if (block_base != base_) restart(block_base);
        if (haveT_ && now_ms - lastT_ < kPeriodMs) return 0;
        const bool seed_now = (!seeded_ || (now_ms - seedStartT_ < kSeedMs && now_ms - lastSeedT_ >= kSeedPeriodMs));
        lastT_ = now_ms; haveT_ = true;
        if (!summaryT_) summaryT_ = now_ms;
        if (!seeded_) seedStartT_ = now_ms;
        unsigned lines = 0;
        try { lines += scan(mem, struct_base, now_ms, seed_now, sink); } catch (...) { ++errors_; }   // a bad_alloc must never escape
        if (now_ms - summaryT_ >= kSummaryMs) {
            summaryT_ = now_ms;
            char t[360];
            std::snprintf(t, sizeof t, "store-search;event=summary;base=%llx;target=%u;scans=%llu;seeds=%u;alive=%zu;moved=%u;truncated=%u;unreadable-chunks=%llu;errors=%llu;last-scan-chunks=%u;lines=%u/%u",
                (unsigned long long)base_, target_, (unsigned long long)scans_, seeds_, cands_.size(), moved_count_, truncated_ ? 1u : 0u, (unsigned long long)unreadable_, (unsigned long long)errors_,
                lastChunks_, logged_, kMaxLines);
            sink.line(true, t); ++lines;
        }
        return lines;
    }
    // Explicit track list (block offsets, u8 reads; at most kTrackMax); kept across restarts. Without it the first movers are tracked.
    void set_track(const uint32_t* offs, unsigned n) {
        track_.clear(); trackLast_.clear(); explicitTrack_ = n > 0;
        for (unsigned i = 0; i < n && i < kTrackMax; ++i) { track_.push_back(offs[i]); trackLast_.push_back(-2); }
    }
    size_t tracked() const noexcept { return track_.size(); }
    size_t alive() const noexcept { return cands_.size(); }
    unsigned moved() const noexcept { return moved_count_; }
    uint64_t scans() const noexcept { return scans_; }
    unsigned logged() const noexcept { return logged_; }
    bool truncated() const noexcept { return truncated_; }
    const std::vector<Cand>& candidates() const noexcept { return cands_; }
private:
    void restart(uint64_t b) noexcept {
        base_ = b; cands_.clear(); seeded_ = false; moved_any_ = false; haveT_ = false; firstMoveT_ = -1; moved_count_ = 0; truncated_ = false;
        if (!explicitTrack_) { track_.clear(); trackLast_.clear(); }
    }
    unsigned scan(e6loc::ReadMem& mem, uint64_t struct_base, double now_ms, bool seed_now, e6loc::Sink& sink) {
        ++scans_;
        std::vector<Cand> out; out.reserve(cands_.size());
        size_t ci = 0; unsigned changed = 0, chunks = 0;
        std::vector<Cand> chunk_out, fresh;
        for (uint64_t cb = 0; cb < block_bytes_; cb += chunk_) {
            const uint32_t n = uint32_t(std::min<uint64_t>(chunk_, block_bytes_ - cb));
            size_t cj = ci;
            while (cj < cands_.size() && cands_[cj].off < cb + n) ++cj;     // existing candidates [ci, cj) fall into this chunk
            if (cj == ci && !seed_now) continue;
            if (!mem.read(base_ + cb, buf_.get(), n)) {
                ++unreadable_;
                for (size_t k = ci; k < cj; ++k) out.push_back(cands_[k]);    // unreadable: keep, nothing learned
                ci = cj; continue;
            }
            ++chunks;
            chunk_out.clear();
            for (size_t k = ci; k < cj; ++k) {
                Cand c = cands_[k];
                if (c.off - cb + kKindBytes[c.kind] > n) { chunk_out.push_back(c); continue; }   // straddles the end of a short final chunk: keep
                uint32_t v = 0;
                if (!decode(c.kind, buf_.get() + (c.off - cb), v)) continue;
                if (v == c.cur) { chunk_out.push_back(c); continue; }   // never pruned for being quiet: the counter only moves at the first launch (step168: an unrelated early mover must not drop it)
                if (v < c.cur && c.cur - v <= kMaxDrop) {
                    const unsigned s = c.steps + (c.cur - v); c.steps = uint8_t(s > 255 ? 255 : s); c.cur = uint16_t(v);
                    ++changed; chunk_out.push_back(c); continue;
                }
                // any other change kills the candidate
            }
            if (seed_now) {
                fresh.clear(); seed_chunk(buf_.get(), n, uint32_t(cb), fresh);
                for (const Cand& f : fresh) {
                    bool dup = false;
                    for (size_t k = ci; k < cj && !dup; ++k) dup = cands_[k].off == f.off && cands_[k].kind == f.kind;   // a position that was ever a candidate is not reseeded
                    if (dup) continue;
                    if (out.size() + chunk_out.size() >= kMaxCand) { truncated_ = true; continue; }
                    chunk_out.push_back(f);
                }
                std::sort(chunk_out.begin(), chunk_out.end(), [](const Cand& a, const Cand& b) { return a.off != b.off ? a.off < b.off : a.kind < b.kind; });
            }
            out.insert(out.end(), chunk_out.begin(), chunk_out.end());
            ci = cj;
        }
        lastChunks_ = chunks;
        if (seed_now) { seeded_ = true; lastSeedT_ = now_ms; ++seeds_; }
        cands_.swap(out);
        unsigned m = 0; for (const Cand& c : cands_) if (c.steps) ++m;
        moved_count_ = m;
        if (changed && !moved_any_) { moved_any_ = true; firstMoveT_ = now_ms; }
        unsigned lines = changed ? report(struct_base, now_ms, sink) : 0;
        return lines + track(mem, now_ms, sink);
    }
    unsigned track(e6loc::ReadMem& mem, double now_ms, e6loc::Sink& sink) {
        if (track_.empty() || trackLogged_ >= kMaxTrackLines) return 0;
        int vals[kTrackMax]; bool changed = false;
        for (size_t i = 0; i < track_.size(); ++i) {
            uint8_t b = 0; vals[i] = (track_[i] < block_bytes_ && mem.read(base_ + track_[i], &b, 1)) ? int(b) : -1;
            if (trackLast_[i] != vals[i]) changed = true;
        }
        if (!changed && now_ms - lastTrackT_ < kSummaryMs) return 0;
        lastTrackT_ = now_ms;
        std::string t; char h[96];
        std::snprintf(h, sizeof h, "store-search;event=track;scan=%llu;t-ms=%.0f;list=", (unsigned long long)scans_, now_ms); t = h;
        for (size_t i = 0; i < track_.size(); ++i) {
            char e[32]; std::snprintf(e, sizeof e, "%s%X=%d", i ? "," : "", track_[i], vals[i]); t += e; trackLast_[i] = vals[i];
        }
        sink.line(true, t.c_str()); ++trackLogged_;
        return 1;
    }
    void seed_chunk(const uint8_t* p, uint32_t n, uint32_t cb, std::vector<Cand>& out) const {
        const bool byte_ok = target_ <= 0xFF;
        float tf = float(target_); uint32_t fbits; std::memcpy(&fbits, &tf, 4);
        auto add = [&](uint32_t off, Kind k) { out.push_back(Cand{cb + off, uint16_t(target_), uint8_t(k), 0}); };
        if (byte_ok) for (uint32_t i = 0; i < n; ++i) if (p[i] == target_) add(i, kU8);
        for (uint32_t i = 0; i + 2 <= n; i += 2) { uint16_t x; std::memcpy(&x, p + i, 2); if (x == target_) add(i, kU16); }
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            uint32_t x; std::memcpy(&x, p + i, 4);
            if (x == target_) add(i, kU32);
            else if (x == fbits) add(i, kF32);
        }
    }
    unsigned report(uint64_t struct_base, double now_ms, e6loc::Sink& sink) {
        if (logged_ >= kMaxLines) return 0;
        // groups of one offset (widths merged), best (largest decrease) first
        struct G { uint32_t off; unsigned steps, cur; char w[6]; };
        std::vector<G> gs;
        for (const Cand& c : cands_) {
            if (!c.steps) continue;
            if (gs.empty() || gs.back().off != c.off) { G g{c.off, c.steps, c.cur, {0, 0, 0, 0, 0, 0}}; gs.push_back(g); }
            G& g = gs.back(); const size_t l = std::strlen(g.w); if (l < 4) { g.w[l] = kKindChar[c.kind]; g.w[l + 1] = 0; }
            if (c.steps > g.steps) { g.steps = c.steps; g.cur = c.cur; }
        }
        std::stable_sort(gs.begin(), gs.end(), [](const G& a, const G& b) { return a.steps > b.steps; });
        if (track_.empty())   // auto track: the first movers that have a u8 (the step168 table was u8 at stride 0x98)
            for (const G& g : gs) { if (track_.size() >= kTrackMax) break; if (std::strchr(g.w, '1')) { track_.push_back(g.off); trackLast_.push_back(-2); } }
        std::string t; char h[220];
        std::snprintf(h, sizeof h, "store-search;event=survivors;scan=%llu;t-ms=%.0f;target=%u;alive=%zu;moved=%u;groups=%zu;list=", (unsigned long long)scans_, now_ms, target_, cands_.size(), moved_count_, gs.size());
        t = h;
        for (size_t i = 0; i < gs.size() && i < kListMax; ++i) {
            char e[140]; const long long rel = (long long)gs[i].off - (long long)(struct_base - base_);
            if (gs[i].off < kEeRamBytes) std::snprintf(e, sizeof e, "%s%X(ee=%08X|rel=%+lld|w=%s|v=%u|d=%u)", i ? ";" : "", gs[i].off, gs[i].off, rel, gs[i].w, gs[i].cur, gs[i].steps);
            else std::snprintf(e, sizeof e, "%s%X(ee=none|rel=%+lld|w=%s|v=%u|d=%u)", i ? ";" : "", gs[i].off, rel, gs[i].w, gs[i].cur, gs[i].steps);
            t += e;
        }
        sink.line(true, t.c_str()); ++logged_;
        return 1;
    }
    bool active_ = false, haveT_ = false, seeded_ = false, moved_any_ = false, truncated_ = false, explicitTrack_ = false;
    std::vector<uint32_t> track_; std::vector<int> trackLast_; unsigned trackLogged_ = 0; double lastTrackT_ = 0;
    std::unique_ptr<uint8_t[]> buf_;
    std::vector<Cand> cands_;
    uint32_t target_ = 0, chunk_ = 0, lastChunks_ = 0, seeds_ = 0, moved_count_ = 0;
    uint64_t block_bytes_ = 0, base_ = 0, scans_ = 0, unreadable_ = 0, errors_ = 0;
    double lastT_ = 0, summaryT_ = 0, seedStartT_ = 0, lastSeedT_ = 0, firstMoveT_ = -1;
    unsigned logged_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- launch detector (the wiring)
// Turns successive reads of the found counter into launches, with hysteresis: a value must be seen unchanged for >= kStableMs and kStableReads reads before it is
// believed (a transient glitch changes nothing). The first stable value is the baseline. A stable fall of 1..kMaxLaunches = that many launches; a stable rise =
// a rearm (refill/respawn); either way the new value is the baseline. A fall bigger than kMaxLaunches is a reload/noise: rebaselined without launches.
inline constexpr double kStableMs = 300.0;
inline constexpr unsigned kStableReads = 2, kMaxLaunches = 8;
struct LaunchEvent { unsigned launches = 0; bool rearm = false; };
class LaunchDetector {
public:
    void reset() noexcept { haveBase_ = false; havePend_ = false; }
    LaunchEvent update(bool valid, uint32_t v, double now_ms) noexcept {
        LaunchEvent ev;
        if (!valid) return ev;                                   // an unreadable tick keeps the pending state
        if (!havePend_ || v != pend_) { pend_ = v; pendT_ = now_ms; pendN_ = 1; havePend_ = true; return ev; }
        ++pendN_;
        if (pendN_ < kStableReads || now_ms - pendT_ < kStableMs) return ev;
        if (!haveBase_) { base_ = v; haveBase_ = true; return ev; }
        if (v < base_) { const uint32_t d = base_ - v; if (d <= kMaxLaunches) ev.launches = d; base_ = v; }
        else if (v > base_) { ev.rearm = true; base_ = v; }
        return ev;
    }
    bool has_baseline() const noexcept { return haveBase_; }
    uint32_t baseline() const noexcept { return base_; }
private:
    bool haveBase_ = false, havePend_ = false;
    uint32_t base_ = 0, pend_ = 0, pendN_ = 0;
    double pendT_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- option parsing (the probe reads the raw option string)
// ;stwatch-start:<n>   the HUD count at the first lock (1..9999, default 50)
// ;stctr:<hex block offset>,<1|2|4>   the found counter: block offset and width; unset = unwired (the stores stay on the racks)
// ;stores:0 disables the wiring.
// ;sttrack:<hex>,<hex>,...   up to 8 block offsets whose u8 value is logged every scan on change (event=track), e.g. the step168 table 67D886,67D91E,67D9B6,67DA4E,67DAE6
// review 2026-11-01 F6: the stores launch mailbox (acz::stores::signal) has ONE launch source. MSL-HUD (hud_msl.h, on by default) owns it; the stctr
// counter drives it only with mslhud:0 (both on counted every missile twice). msl_hud = the MSL-HUD reader is configured on (hudmsl::parse_mode > 0).
struct Cfg {
    uint32_t target = kDefaultTarget; uint32_t ctr_off = 0; uint32_t ctr_w = 4; bool wire = true; bool bad = false; uint32_t track[kTrackMax] = {}; unsigned ntrack = 0;
    bool msl_hud = false;
    bool drives() const noexcept { return wire && !bad && ctr_off && !msl_hud; }        // the stctr counter feeds note_launch / note_rearm
    bool yields() const noexcept { return wire && !bad && ctr_off && msl_hud; }         // stctr configured but MSL-HUD is the source (logged once)
};
inline Cfg parse_cfg(const char* opt) noexcept {
    Cfg c;
    if (!opt) return c;
    auto find = [&](const char* key) -> const char* {
        const std::string k1 = std::string(";") + key + ":", k2 = std::string(";") + key + "=";
        const char* p = std::strstr(opt, k1.c_str());
        if (!p) p = std::strstr(opt, k2.c_str());
        return p ? p + k1.size() : nullptr;
    };
    if (const char* p = find("stwatch-start")) { char* e = nullptr; const unsigned long v = std::strtoul(p, &e, 10); if (e == p || v < 1 || v > 9999) c.bad = true; else c.target = uint32_t(v); }
    if (const char* p = find("stctr")) {
        char* e = nullptr; const unsigned long v = std::strtoul(p, &e, 16);
        if (e == p || *e != ',' || v == 0 || v >= e6loc::kBlockBytes) c.bad = true;
        else { const unsigned long w = std::strtoul(e + 1, &e, 10); if (w != 1 && w != 2 && w != 4) c.bad = true; else { c.ctr_off = uint32_t(v); c.ctr_w = uint32_t(w); } }
    }
    if (const char* p = find("sttrack")) {
        for (;;) {
            char* e = nullptr; const unsigned long v = std::strtoul(p, &e, 16);
            if (e == p || v >= e6loc::kBlockBytes || c.ntrack >= kTrackMax) { c.bad = true; break; }
            c.track[c.ntrack++] = uint32_t(v);
            if (*e == ',') { p = e + 1; continue; }
            if (*e != 0 && *e != ';') c.bad = true;
            break;
        }
    }
    if (const char* p = find("stores")) if (p[0] == '0') c.wire = false;
    c.msl_hud = acz::hudmsl::parse_mode(opt) > 0;
    return c;
}

}  // namespace acz::e6search
