#pragma once
// store_tracker.h -- STORES-RT (docs/world-render/aircraft-auto.md section 13): which baked weapon stations of the model aircraft are still loaded.
// SDK-free, header only, no allocation after configure().
//
// The baked weapon parts (model_baker.add_stores, station = the game's weapon-station locator group id) are one AnimPiece per station in the
// animated range (cockpit_model.h, AnimRole::store); apply_store_mask collapses the stations whose bit is set. THIS header decides which bits:
//  * Tracker: launch events -> hidden mask in a fixed firing order. A launch empties the next shown station for a reload time (default 3 s, an
//    estimate), then it shows again while missiles remain; set_remaining(n) caps the shown stations at n; rearm() (respawn, refill) shows all.
//  * Signal: the thread-safe mailbox between whoever DETECTS a launch (nobody yet: no reliable game signal is known, see the doc: the producers
//    are written but have no source) and the render consumer. note_launch / note_rearm / set_remaining may be called from any thread; sync()
//    (consumer thread only) applies them to a Tracker and reports whether the mask changed.
// Firing order (INFERENCE, made up: the game's order is not decoded): stations alternate right wing (+x) / left wing (-x), starting right, outboard
// first on each side (|x| descending, then z ascending = the more forward station first). With one wing only that wing's stations fire in turn.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <vector>

#include "cockpit_model.h"

namespace acz::stores {

inline constexpr unsigned kMaxStations = 16;   // a mask is 16 bits of the u32 (bit i = Mesh::store_stations[i])

// The firing order of the stations of a mesh: indices into Mesh::store_stations (the bit numbers), first to fire first. Deterministic.
// `sp` = the special-weapon group (SP-STORES: AnimPiece::sp pieces, bits of Mesh::sp_stations) instead of the standard one.
inline std::vector<unsigned> firing_order(const acz::cockpit::Mesh& m, bool sp = false) {
    struct S { unsigned bit; float x, z; };
    std::vector<S> right, left;
    const std::vector<uint32_t>& stations = sp ? m.sp_stations : m.store_stations;
    for (const acz::cockpit::AnimPiece& a : m.anim) {
        if (a.role != acz::cockpit::AnimRole::store || a.sp != sp) continue;
        unsigned bit = 0;
        while (bit < stations.size() && stations[bit] != a.station) ++bit;
        if (bit >= stations.size() || bit >= kMaxStations) continue;
        S s{bit, a.pivot[0], a.pivot[2]};
        (s.x >= 0.0f ? right : left).push_back(s);
    }
    auto cmp = [](const S& p, const S& q) {
        const float ap = std::fabs(p.x), aq = std::fabs(q.x);
        if (ap != aq) return ap > aq;
        if (p.z != q.z) return p.z < q.z;
        return p.bit < q.bit;
    };
    std::sort(right.begin(), right.end(), cmp);
    std::sort(left.begin(), left.end(), cmp);
    std::vector<unsigned> out;
    for (size_t i = 0; i < std::max(right.size(), left.size()); ++i) {
        if (i < right.size()) out.push_back(right[i].bit);
        if (i < left.size()) out.push_back(left[i].bit);
    }
    return out;
}

// Reload model (Ace Combat: the fired pylon missile vanishes and a new one appears on the same pylon after a short reload while missiles remain):
//  * launch(now): the next SHOWN station in the alternating order empties for reload_ms, then shows again (all stations empty: the next one in turn has
//    its reload restarted). The turn pointer keeps advancing, so a salvo walks the pylons in the firing order.
//  * set_remaining(n): the absolute missile count (HUD MSSL). Only `n` stations may be shown: when more are shown than n, the earliest in the firing order
//    are held empty (until a rearm or a larger count). kUnknown (the default) = no cap.
//  * rearm(): every station shown; reloads and the cap are cleared.
// Times are caller milliseconds (any monotonic clock, non-decreasing).
inline constexpr double kDefaultReloadSeconds = 3.0;   // ESTIMATE (aircraft-auto.md section 13): no doc or evidence has the game's real pylon reload time

class Tracker {
public:
    static constexpr unsigned kUnknown = 0xFFFFFFFFu;
    // Stations of `m` (none: every mask is 0). Resets reloads, the cap and the clock; keeps the reload time.
    void configure(const acz::cockpit::Mesh& m, bool sp = false) { order_ = firing_order(m, sp); for (double& u : until_) u = 0.0; cursor_ = 0; remaining_ = kUnknown; now_ = 0.0; }
    void set_reload_ms(double ms) noexcept { reload_ms_ = ms < 0.0 ? 0.0 : ms; }
    double reload_ms() const noexcept { return reload_ms_; }
    unsigned stations() const noexcept { return unsigned(order_.size()); }
    // Stations empty at the tracker's current time (the last launch / advance / hidden_mask(now)).
    unsigned fired() const noexcept { uint32_t v = mask_at_(now_); unsigned n = 0; for (; v; v &= v - 1) ++n; return n; }
    void launch(double now_ms) noexcept {
        now_ = std::max(now_, now_ms); ++launches_;
        const unsigned s = unsigned(order_.size());
        if (!s) return;
        const uint32_t cur = mask_at_(now_);
        unsigned pick = cursor_ % s;
        for (unsigned k = 0; k < s; ++k) {   // the next station in turn that is shown (not reloading, not held by the cap)
            const unsigned c = (cursor_ + k) % s;
            if (!(cur >> order_[c] & 1u)) { pick = c; break; }
        }
        until_[order_[pick]] = now_ + reload_ms_; cursor_ = (pick + 1) % s;
    }
    void rearm() noexcept { for (double& u : until_) u = 0.0; cursor_ = 0; remaining_ = kUnknown; ++rearms_; }
    void set_remaining(unsigned n) noexcept { remaining_ = n; }   // absolute missile count; kUnknown = no cap
    unsigned remaining() const noexcept { return remaining_; }
    void advance(double now_ms) noexcept { now_ = std::max(now_, now_ms); }
    uint32_t hidden_mask(double now_ms) noexcept { advance(now_ms); return mask_at_(now_); }
    uint32_t hidden_mask() const noexcept { return mask_at_(now_); }
    uint64_t launches() const noexcept { return launches_; }
    uint64_t rearms() const noexcept { return rearms_; }
private:
    // Reload-empty stations, then the cap: of the stations still shown only `remaining_` stay (the earliest in the firing order are held empty).
    uint32_t mask_at_(double t) const noexcept {
        uint32_t h = 0;
        for (unsigned bit : order_) if (until_[bit] > t) h |= 1u << bit;
        if (remaining_ != kUnknown) {
            unsigned shown = 0;
            for (unsigned bit : order_) if (!(h >> bit & 1u)) ++shown;
            for (size_t i = 0; i < order_.size() && shown > remaining_; ++i)
                if (!(h >> order_[i] & 1u)) { h |= 1u << order_[i]; --shown; }
        }
        return h;
    }
    std::vector<unsigned> order_;
    double until_[kMaxStations] = {};
    double reload_ms_ = kDefaultReloadSeconds * 1000.0, now_ = 0.0;
    unsigned cursor_ = 0, remaining_ = kUnknown;
    uint64_t launches_ = 0, rearms_ = 0;
};

// Producer -> consumer mailbox (lock-free). Events are counted, not queued: the consumer applies "launches since the last sync" in order.
class Signal {
public:
    static constexpr uint32_t kNoRemaining = 0xFFFFFFFFu;
    void note_launch() noexcept { launches_.fetch_add(1, std::memory_order_acq_rel); }
    void note_rearm() noexcept { rearms_.fetch_add(1, std::memory_order_acq_rel); }
    void set_remaining(uint32_t n) noexcept { remaining_.store(n, std::memory_order_release); remaining_seq_.fetch_add(1, std::memory_order_acq_rel); }
    // Consumer thread only. Applies, in this order: a rearm since the last sync (fired = 0), an absolute remaining count (newer than the last sync), then every
    // launch since the last sync. Returns true when the tracker's mask may have changed.
    bool sync(Tracker& t, double now_ms) noexcept {
        bool changed = false;
        t.advance(now_ms);
        const uint32_t re = rearms_.load(std::memory_order_acquire);
        if (re != seen_rearms_) { seen_rearms_ = re; t.rearm(); changed = true; }
        const uint32_t rs = remaining_seq_.load(std::memory_order_acquire);
        if (rs != seen_remaining_) {
            seen_remaining_ = rs;
            const uint32_t n = remaining_.load(std::memory_order_acquire);
            if (n != kNoRemaining) { t.set_remaining(n); changed = true; }
        }
        const uint32_t la = launches_.load(std::memory_order_acquire);
        while (seen_launches_ != la) { ++seen_launches_; t.launch(now_ms); changed = true; }
        return changed;
    }
    // A new model: everything signalled before it belongs to the previous aircraft.
    void discard() noexcept {
        seen_rearms_ = rearms_.load(std::memory_order_acquire);
        seen_remaining_ = remaining_seq_.load(std::memory_order_acquire);
        seen_launches_ = launches_.load(std::memory_order_acquire);
    }
    uint32_t launches() const noexcept { return launches_.load(std::memory_order_acquire); }
private:
    std::atomic<uint32_t> launches_{0}, rearms_{0}, remaining_{kNoRemaining}, remaining_seq_{0};
    uint32_t seen_launches_ = 0, seen_rearms_ = 0, seen_remaining_ = 0;   // consumer thread only
};

// The process-wide mailbox (the producers have no handle to the XR output). Never freed.
inline Signal& signal() noexcept { static Signal s; return s; }
// SP-STORES: the same mailbox for the special-weapon group (the HUD's SP row count drives it; hud_msl.h Decoder::sp_event). Never freed.
inline Signal& sp_signal() noexcept { static Signal s; return s; }

}  // namespace acz::stores
