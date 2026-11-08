#pragma once
// stage_scan.h -- the bounded, prioritised scan core for xr_terrain_stage=auto (R-3 of docs/review/overnight-runtime-review.md).
// SDK-free: the Windows glue (stage_detect_win.cpp) enumerates the regions and supplies a PageSource (VirtualQuery'd regions,
// QueryWorkingSetEx residency, SEH-guarded copies, Sleep); tests/acz-vr/stage_fingerprint_offline_tests.cpp drives it with fakes.
//
// Cost bounds (docs/world-render/stages-and-detection.md section 3.5):
//  - regions are scanned most-likely first (order_regions: the 64 MiB private RW EE-RAM-like allocation, then private RW 1..256 MiB
//    allocations largest first, then the rest);
//  - the scan stops at the first DECISIVE lead (best >= decisive_hits, best - second >= decisive_margin, best >= 2 * second);
//  - once any candidate leads (best >= min_hits and best > second) only post_candidate_bytes more are read; max_bytes is a hard cap;
//  - reads are <= chunk bytes and the scan yields (yield_ms) after every chunk-sized amount of bytes read;
//  - with skip_nonresident, only pages already in the working set are read: the scan never pages memory in (no hard or soft fault,
//    no demand-zero page materialised). Non-resident bytes are counted, never read.
#include "stage_fingerprint.h"
#include <cstdint>
#include <vector>

namespace acz::stagefp {

inline constexpr uint32_t kPage = 4096;
inline constexpr uint32_t kMaxChunk = 4u << 20;   // 4 MiB = 1024 pages (the residency flags live on the stack)

// Region priority classes (lower scans first).
enum RegionClass : uint8_t {
    kEeLike = 0,       // MEM_PRIVATE read-write, allocation exactly 64 MiB: the E6 locator's EE-RAM block shape (e6-anchor-free.md) [I]
    kPrivRwMid = 1,    // MEM_PRIVATE read-write, allocation 1..256 MiB (heap segments, file buffers); largest allocation first
    kPrivRwOther = 2,  // MEM_PRIVATE read-write, allocation < 1 MiB or > 256 MiB
    kPrivOther = 3,    // MEM_PRIVATE readable but not plain read-write
    kMapped = 4,       // MEM_MAPPED (sections / file views)
    kClassCount = 5
};
const char* class_name(uint8_t c) noexcept;
uint8_t classify(bool is_private, bool read_write, uint64_t alloc_span) noexcept;

struct Region {
    uint64_t base = 0, size = 0;           // committed, readable, page-aligned
    uint64_t alloc_base = 0, alloc_span = 0;
    uint8_t cls = kPrivRwOther;
};
// Stable: class first; inside kPrivRwMid larger allocations first (then address); elsewhere address order.
void order_regions(std::vector<Region>& r);

class PageSource {
public:
    virtual ~PageSource() = default;
    // Fill flags[i] = 1 when page addr + i*kPage is resident in the working set. Returns false when the query failed (the chunk is
    // then skipped: unknown residency is treated as not resident).
    virtual bool resident(uint64_t addr, uint32_t pages, uint8_t* flags) noexcept = 0;
    virtual bool read(uint64_t addr, void* out, size_t n) noexcept = 0;   // false = the copy faulted (region vanished etc.)
    virtual uint64_t now_ms() noexcept = 0;
    virtual void yield(uint32_t ms) noexcept = 0;
};

struct ScanLimits {
    uint64_t max_bytes = 1ull << 30;            // hard cap of bytes READ per attempt (1 GiB; the caller raises it to 2 GiB on the last attempt)
    uint64_t post_candidate_bytes = 384ull << 20;   // after the first candidate lead, read at most this much more
    uint32_t max_ms = 20000;                    // wall-clock cap including yields
    uint32_t decisive_hits = 16, decisive_margin = 8;
    uint32_t chunk = kMaxChunk;                 // clamped to [kPage, kMaxChunk], rounded down to whole pages
    uint32_t yield_ms = 1;                      // Sleep after every `chunk` bytes read (0 = never)
    bool skip_nonresident = true;
    bool (*stop)() noexcept = nullptr;          // orderly stop (game exit): checked before every chunk
};

struct HitRegion { uint32_t rank = 0; uint8_t cls = 0; uint64_t alloc_span = 0, size = 0; uint32_t hits = 0; };

struct ScanStats {
    uint32_t regions = 0;                       // regions visited (at least partly)
    uint32_t regions_eligible = 0;              // regions in the ordered list
    uint32_t regions_skipped_gpu = 0, regions_skipped_image = 0, regions_skipped_own = 0;   // filled by the Windows enumerator
    uint32_t read_failures = 0, ws_query_failures = 0;
    uint64_t bytes = 0;                         // bytes read and fed
    uint64_t bytes_nonresident = 0;             // bytes skipped because they were not resident (never touched)
    uint64_t bytes_eligible = 0;                // sum of the ordered regions' sizes
    uint64_t candidate_at = 0, decisive_at = 0; // bytes read when the first candidate / the decisive lead appeared (0 = never)
    uint64_t cap_bytes = 0;                     // the byte cap in force at the end
    uint32_t ms = 0, yields = 0;
    bool truncated = false;                     // a cap (bytes/time) or a stop ended the scan before the list was done
    const char* stop = "complete";              // complete | decisive | byte-cap | post-candidate-cap | time-cap | stop | no-buffer | no-memory
    HitRegion hit[4]; uint32_t hit_n = 0;       // the regions that contributed most new signatures (best first)
};

// Scans the regions in the given order with `buf` (>= lim.chunk bytes). The Matcher accumulates; call decide() afterwards.
// `st` is NOT reset (the enumerator's skip counters survive); start from ScanStats{}.
void scan_regions(Matcher& m, const std::vector<Region>& ordered, PageSource& src, uint8_t* buf, const ScanLimits& lim,
                  ScanStats& st) noexcept;

} // namespace acz::stagefp
