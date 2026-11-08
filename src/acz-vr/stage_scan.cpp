// stage_scan.cpp -- see stage_scan.h. SDK-free, offline-tested (stage_fingerprint_offline_tests).
#include "stage_scan.h"
#include <algorithm>

namespace acz::stagefp {

const char* class_name(uint8_t c) noexcept {
    switch (c) {
    case kEeLike: return "ee64";
    case kPrivRwMid: return "prw";
    case kPrivRwOther: return "prw-x";
    case kPrivOther: return "priv";
    case kMapped: return "map";
    default: return "?";
    }
}

uint8_t classify(bool is_private, bool read_write, uint64_t alloc_span) noexcept {
    if (!is_private) return kMapped;
    if (!read_write) return kPrivOther;
    if (alloc_span == (64ull << 20)) return kEeLike;
    if (alloc_span >= (1ull << 20) && alloc_span <= (256ull << 20)) return kPrivRwMid;
    return kPrivRwOther;
}

void order_regions(std::vector<Region>& r) {
    std::stable_sort(r.begin(), r.end(), [](const Region& a, const Region& b) {
        if (a.cls != b.cls) return a.cls < b.cls;
        if (a.cls == kPrivRwMid && a.alloc_span != b.alloc_span) return a.alloc_span > b.alloc_span;
        return a.base < b.base;
    });
}

namespace {
void note_hits(ScanStats& st, uint32_t rank, const Region& r, uint32_t hits) noexcept {
    if (!hits) return;
    HitRegion h; h.rank = rank; h.cls = r.cls; h.alloc_span = r.alloc_span; h.size = r.size; h.hits = hits;
    uint32_t n = st.hit_n < 4 ? st.hit_n : 4;
    uint32_t pos = n;
    while (pos > 0 && st.hit[pos - 1].hits < hits) --pos;
    if (pos >= 4) return;
    for (uint32_t i = (n < 4 ? n : 3); i > pos; --i) st.hit[i] = st.hit[i - 1];
    st.hit[pos] = h;
    if (st.hit_n < 4) ++st.hit_n;
}
} // namespace

void scan_regions(Matcher& m, const std::vector<Region>& ordered, PageSource& src, uint8_t* buf, const ScanLimits& lim,
                  ScanStats& st) noexcept {
    const uint64_t t0 = src.now_ms();
    st.regions_eligible = uint32_t(ordered.size());
    for (const auto& r : ordered) st.bytes_eligible += r.size;
    uint32_t chunk = lim.chunk < kPage ? kPage : (lim.chunk > kMaxChunk ? kMaxChunk : lim.chunk);
    chunk -= chunk % kPage;
    uint64_t cap = lim.max_bytes;
    st.cap_bytes = cap;
    if (!buf) { st.stop = "no-buffer"; st.truncated = true; ++st.read_failures; return; }
    const uint32_t min_hits = m.table().min_hits;
    bool candidate = false, done = false;
    uint32_t seen_before_check = m.seen_count();
    uint64_t since_yield = 0;   // bytes read since the last yield (many small regions must not each cost a Sleep)
    uint8_t flags[kMaxChunk / kPage];

    // Read [a, a+n) (resident, already checked) and feed it as part of the current stream.
    auto read_feed = [&](uint64_t a, uint64_t n) noexcept {
        if (!src.read(a, buf, size_t(n))) { ++st.read_failures; m.end_stream(); m.begin_stream(); return; }
        m.feed(buf, size_t(n));
        st.bytes += n;
    };

    for (uint32_t rank = 0; rank < ordered.size() && !done; ++rank) {
        const Region& R = ordered[rank];
        if (!R.size) continue;
        const uint32_t at_region_start = m.seen_count();
        ++st.regions;
        m.begin_stream();
        const uint64_t end = R.base + R.size;
        for (uint64_t a = R.base; a < end && !done;) {
            if (lim.stop && lim.stop()) { st.stop = "stop"; st.truncated = true; done = true; break; }
            const uint64_t n = (end - a) < chunk ? (end - a) : chunk;
            const uint64_t bytes_before = st.bytes;
            if (lim.skip_nonresident) {
                const uint32_t pages = uint32_t((n + kPage - 1) / kPage);
                if (!src.resident(a, pages, flags)) {
                    ++st.ws_query_failures; st.bytes_nonresident += n; m.end_stream(); m.begin_stream();
                } else {
                    // contiguous runs of resident pages are read; a gap breaks the stream (windows never span unread bytes)
                    uint32_t p = 0;
                    while (p < pages) {
                        const uint32_t q0 = p;
                        const bool res = flags[p] != 0;
                        while (p < pages && (flags[p] != 0) == res) ++p;
                        const uint64_t ra = a + uint64_t(q0) * kPage;
                        uint64_t rn = uint64_t(p - q0) * kPage;
                        if (ra + rn > a + n) rn = a + n - ra;
                        if (res) read_feed(ra, rn);
                        else { st.bytes_nonresident += rn; m.end_stream(); m.begin_stream(); }
                    }
                }
            } else {
                read_feed(a, n);
            }
            a += n;
            // decisive / candidate bookkeeping only when a new signature appeared
            if (m.seen_count() != seen_before_check) {
                seen_before_check = m.seen_count();
                uint32_t best = 0, second = 0; int group = -1;
                m.lead(best, second, group);
                if (!candidate && best >= min_hits && best > second) {
                    candidate = true;
                    st.candidate_at = st.bytes;
                    const uint64_t c2 = st.bytes + lim.post_candidate_bytes;
                    if (c2 < cap) cap = c2;
                    st.cap_bytes = cap;
                }
                if (best >= min_hits && best >= lim.decisive_hits && best >= second + lim.decisive_margin && best >= 2 * second) {
                    st.decisive_at = st.bytes; st.stop = "decisive"; done = true; break;
                }
            }
            if (st.bytes >= cap) { st.stop = candidate && cap < lim.max_bytes ? "post-candidate-cap" : "byte-cap"; st.truncated = true; done = true; break; }
            if (src.now_ms() - t0 >= lim.max_ms) { st.stop = "time-cap"; st.truncated = true; done = true; break; }
            since_yield += st.bytes - bytes_before;
            if (lim.yield_ms && since_yield >= chunk) { src.yield(lim.yield_ms); ++st.yields; since_yield = 0; }
        }
        m.end_stream();
        note_hits(st, rank, R, m.seen_count() - at_region_start);
    }
    st.ms = uint32_t(src.now_ms() - t0);
}

} // namespace acz::stagefp
