#pragma once
// world_dedupe.h -- WORLD-4: do not draw a building twice. With the props pass on (WORLD-3, xr_terrain_stage props:1) the game's own DrawACM_v replay
// still draws the stage's static buildings near the player: a replayed record's model->world translation (record +256 B matrix, floats 76..78) is the
// .pdl placement of the building it draws (evidence: test-16 record (12120, 512, 132150) = m02a .pdl #83 (12120.09, 512.18, 132150.48), 0.49 units;
// docs/world-render/world4-impl.md). Option propdedupe:1 (inside xr_terrain_stage; default 0) publishes the loaded prop placements here, and the producer
// skips (refuses as out of scope, "world-dedupe-prop") a world record whose translation matches one within `tol` units: the building is then drawn
// once, by the props pass, registered with the terrain. Aircraft, ground units and anything not at a placement are untouched.
// Header-only, SDK-free; the slot is process-wide (one DLL), written once per terrain load, never freed while readers may run (the published set is
// intentionally leaked: a few kB per load).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace acz::props {

struct Dedupe {
    std::vector<float> xyz;   // placements sorted by x (3 floats each)
    float tol = 2.0f;
};
inline std::atomic<const Dedupe*>& dedupe_slot() noexcept { static std::atomic<const Dedupe*> s{nullptr}; return s; }
inline void publish_dedupe(const std::vector<float>& xyz_unsorted, float tol) {
    auto* d = new Dedupe;
    const size_t n = xyz_unsorted.size() / 3;
    std::vector<size_t> ord(n);
    for (size_t i = 0; i < n; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return xyz_unsorted[a * 3] < xyz_unsorted[b * 3] || (xyz_unsorted[a * 3] == xyz_unsorted[b * 3] && a < b); });
    d->xyz.reserve(n * 3);
    for (size_t i : ord) for (int k = 0; k < 3; ++k) d->xyz.push_back(xyz_unsorted[i * 3 + size_t(k)]);
    d->tol = tol;
    dedupe_slot().store(d, std::memory_order_release);   // the previous set (if any) stays allocated: a reader may still hold it
}
inline void clear_dedupe() noexcept { dedupe_slot().store(nullptr, std::memory_order_release); }
// true when (x, y, z) is within d.tol of a placement on every axis.
inline bool dedupe_match(const Dedupe& d, double x, double y, double z) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const size_t n = d.xyz.size() / 3;
    size_t lo = 0, hi = n;   // first placement with px >= x - tol
    while (lo < hi) { const size_t m = (lo + hi) / 2; if (double(d.xyz[m * 3]) < x - d.tol) lo = m + 1; else hi = m; }
    for (size_t i = lo; i < n && double(d.xyz[i * 3]) <= x + d.tol; ++i)
        if (std::fabs(double(d.xyz[i * 3 + 1]) - y) <= d.tol && std::fabs(double(d.xyz[i * 3 + 2]) - z) <= d.tol) return true;
    return false;
}
// The producer's check for one DrawACM_v record (raw = the 416-byte record; +256 B = model->world, row-major with the translation in floats 76..78).
// nullptr = keep; "world-dedupe-prop" = drawn by the props pass instead.
inline const char* dedupe_record_why(const uint8_t* raw) noexcept {
    const Dedupe* d = dedupe_slot().load(std::memory_order_acquire);
    if (!d || !raw) return nullptr;
    float t[3];
    std::memcpy(t, raw + 4 * 76, sizeof t);   // same read as gpu_copy_step5_world.h rf(raw, 64 + 12 + k)
    return dedupe_match(*d, t[0], t[1], t[2]) ? "world-dedupe-prop" : nullptr;
}

} // namespace acz::props
