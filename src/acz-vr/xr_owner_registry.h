#pragma once
// xr_owner_registry.h -- R-5 (docs/review/overnight-runtime-review.md): publication of the XR attempt owners for the producer's try-only
// pose reads, with SAFE release of finished failed attempts. Header-only, SDK-free (offline-tested in xr_owner_offline_tests).
//
// The old design (D-232/D-324) kept every published Owner alive forever because readers (try_read_pose, request_stop; any thread, any
// time) load a raw pointer. This registry keeps that guarantee and adds a two-phase quiescence protocol (userspace-RCU style) so a
// FINISHED owner can be dropped:
//   reader:   e = epoch_ & 1; cnt_[e]++; p = current_; use p; cnt_[e]--          (all seq_cst; never waits, never locks)
//   release:  current_: owner -> nullptr if it is still current; then twice: flip epoch_, wait until the counter of the previous parity
//             reads 0; then drop the registry's reference.
// A reader that can still hold the old pointer loaded current_ before the swap, so it incremented a counter before the swap; the two
// flips cover both parities, and new readers go to the other counter, so the wait ends promptly even under continuous reads. A reader
// that increments after a counter was observed 0 loads current_ after the swap (seq_cst total order) and cannot see the old pointer.
// If a counter does not drain inside the bound, the owner simply stays retained (the old behaviour; logged). Releasing never destroys
// anything itself: the last shared_ptr (the monitor's local, the owner/watchdog threads' copies) frees the Owner, whose destructor and
// its consumer's / backend's destructors make no runtime or D3D12 call (~Owner, ~OpenXr, ~Blit are "no foreign calls"; ~PropsGpu joins
// its worker). Only one thread (the monitor) releases; release() serialises itself anyway.
#include "xr_owner.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace acz::xr_owner {

class OwnerRegistry {
public:
    enum class Release : uint8_t { released, not_finished, not_found, readers_busy };
    static const char* release_name(Release r) noexcept {
        switch (r) {
        case Release::released: return "released";
        case Release::not_finished: return "kept(not-finished)";
        case Release::not_found: return "kept(not-published)";
        case Release::readers_busy: return "kept(pose-readers-busy)";
        }
        return "?";
    }

    // Publication replaces the pointer readers see; the registry keeps a reference. false = no memory to keep it (not published).
    bool publish(const std::shared_ptr<Owner>& o) noexcept {
        if (!o) return false;
        try {
            std::lock_guard<std::mutex> l(m_);
            kept_.push_back(o);
        } catch (...) { return false; }
        current_.store(o.get(), std::memory_order_seq_cst);
        published_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Try-only, never waits (any thread).
    bool try_read_pose(PoseSnapshot& out) const noexcept {
        const unsigned e = enter();
        const Owner* o = current_.load(std::memory_order_seq_cst);
        const bool ok = o && o->try_read_pose(out);
        leave(e);
        return ok;
    }
    // The orderly stop hook: request_stop() on the current owner under the same reader protocol.
    void request_stop_current() noexcept {
        const unsigned e = enter();
        if (Owner* o = current_.load(std::memory_order_seq_cst)) o->request_stop();
        leave(e);
    }
    // Drops the registry's reference to a FINISHED owner once no reader can hold its raw pointer. Never blocks a reader.
    Release release(const std::shared_ptr<Owner>& o, int64_t max_wait_ms) noexcept {
        if (!o) return Release::not_found;
        if (!o->finished()) return Release::not_finished;
        std::unique_lock<std::mutex> serial(release_m_, std::defer_lock);
        try { serial.lock(); } catch (...) { return Release::readers_busy; }
        if (!contains(o.get())) return Release::not_found;
        Owner* expected = o.get();
        current_.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms < 0 ? 0 : max_wait_ms);
        for (int phase = 0; phase < 2; ++phase) {
            const unsigned old = epoch_.fetch_add(1, std::memory_order_seq_cst) & 1u;
            for (unsigned spin = 0; cnt_[old].load(std::memory_order_seq_cst) != 0; ++spin) {
                if (std::chrono::steady_clock::now() >= until) { busy_.fetch_add(1, std::memory_order_relaxed); return Release::readers_busy; }
                if (spin < 64) std::this_thread::yield(); else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        std::shared_ptr<Owner> drop;   // destroyed outside the lock (by the caller's last reference, normally)
        {
            std::lock_guard<std::mutex> l(m_);
            for (size_t i = 0; i < kept_.size(); ++i)
                if (kept_[i].get() == o.get()) { drop = std::move(kept_[i]); kept_.erase(kept_.begin() + std::ptrdiff_t(i)); break; }
        }
        released_.fetch_add(1, std::memory_order_relaxed);
        return Release::released;
    }
    size_t retained() const noexcept { std::lock_guard<std::mutex> l(m_); return kept_.size(); }
    uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
    uint64_t released() const noexcept { return released_.load(std::memory_order_relaxed); }
    uint64_t busy() const noexcept { return busy_.load(std::memory_order_relaxed); }
    const Owner* current() const noexcept { return current_.load(std::memory_order_seq_cst); }

private:
    unsigned enter() const noexcept {
        const unsigned e = epoch_.load(std::memory_order_seq_cst) & 1u;
        cnt_[e].fetch_add(1, std::memory_order_seq_cst);
        return e;
    }
    void leave(unsigned e) const noexcept { cnt_[e].fetch_sub(1, std::memory_order_seq_cst); }
    bool contains(const Owner* p) const noexcept {
        std::lock_guard<std::mutex> l(m_);
        for (const auto& k : kept_) if (k.get() == p) return true;
        return false;
    }
    std::atomic<Owner*> current_{nullptr};
    mutable std::atomic<unsigned> epoch_{0};
    mutable std::atomic<uint32_t> cnt_[2]{};
    mutable std::mutex m_;
    std::mutex release_m_;
    std::vector<std::shared_ptr<Owner>> kept_;
    std::atomic<uint64_t> published_{0}, released_{0}, busy_{0};
};

} // namespace acz::xr_owner
