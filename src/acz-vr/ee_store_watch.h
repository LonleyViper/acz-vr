#pragma once
// ee_store_watch.h -- STORES-RT launch-signal SEARCH (option stwatch:1 of xr_cockpit_model; docs/world-render/aircraft-auto.md section 13). READ-ONLY, SDK-free.
//
// Nothing offline says where the game keeps the player's remaining missile count (the HUD MSL counter) or a launch flag: the E6 structure the camera
// feed reads is a camera/attitude block (ee_e6_locator.h), and no log of a headset session contains a missile launch next to a memory window. This
// watch produces that evidence on the next session that fires: while the E6 locator is locked it reads a window of the SAME 64 MiB block around the
// E6 structure (kSpanBefore .. kSpanAfter bytes from the structure base) at kPeriodMs, and logs every 32-bit word that
//   * counts DOWN BY ONE inside 1..kMaxCounter (an ammo / stores counter after a launch): `store-watch;event=decrement;...`
//   * jumps UP after having counted down (a rearm / respawn refill):                       `store-watch;event=rise;...`
// A word that changes more than kMuteChanges times (timers, positions, noise) is muted for the rest of the run; a word logs at most kMaxLines lines in
// total. `store-watch;event=summary` every kSummaryMs says how many words are tracked / muted / logged (so a dead watch is told from a quiet one).
// Correlate the lines with the times the pilot fired (the firing wedge `fire-events` in summary-tactical with tac:1, the HUD MSL counter in a screenshot,
// the user's note): a word whose `old->new` steps match the number of missiles fired is the counter; its offset from the structure base is then the
// signal (feed it to acz::stores::signal().set_remaining or note_launch).
//
// Cost: kWindowBytes (32 KiB) read per tick at 4 Hz, one nothrow allocation at start(); no allocation afterwards; a failed read skips the tick.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "ee_e6_locator.h"

namespace acz::e6store {

inline constexpr uint32_t kSpanBefore = 0x4000, kSpanAfter = 0x4000;             // bytes around the E6 structure base (window A start)
inline constexpr uint32_t kWindowBytes = kSpanBefore + kSpanAfter;
inline constexpr uint32_t kWords = kWindowBytes / 4;
inline constexpr uint32_t kMaxCounter = 99;                                      // a missile / stores counter
inline constexpr unsigned kMuteChanges = 24, kMaxLines = 200;
inline constexpr double kPeriodMs = 250.0, kSummaryMs = 10000.0;

class AmmoWatch {
public:
    // Allocates the snapshot (nothrow); false = no memory (the watch stays off).
    bool start() noexcept {
        if (cur_) return true;
        cur_.reset(new (std::nothrow) uint32_t[kWords]);
        prev_.reset(new (std::nothrow) uint32_t[kWords]);
        changes_.reset(new (std::nothrow) uint8_t[kWords]);
        flags_.reset(new (std::nothrow) uint8_t[kWords]);
        if (!cur_ || !prev_ || !changes_ || !flags_) { cur_.reset(); prev_.reset(); changes_.reset(); flags_.reset(); return false; }
        std::memset(changes_.get(), 0, kWords); std::memset(flags_.get(), 0, kWords);
        return true;
    }
    bool active() const noexcept { return bool(cur_); }
    // `structure_base` = Locator::locked_base() (the E6 window A base); call once per Present tick while locked, 0 while not locked (the baseline is dropped).
    // A new base restarts the baseline (a relock may move the block). Returns the number of lines emitted.
    unsigned tick(acz::e6loc::ReadMem& mem, uint64_t structure_base, double now_ms, acz::e6loc::Sink& sink) noexcept {
        if (!cur_) return 0;
        if (!structure_base) { have_ = false; base_ = 0; return 0; }
        if (structure_base != base_) { base_ = structure_base; have_ = false; ++rebases_; }
        if (haveT_ && now_ms - lastT_ < kPeriodMs) return 0;
        lastT_ = now_ms; haveT_ = true;
        if (!summaryT_) summaryT_ = now_ms;
        const uint64_t from = structure_base - kSpanBefore;   // structure bases are >= 0x10000 (a heap block), so no wrap
        unsigned lines = 0;
        if (!mem.read(from, cur_.get(), kWindowBytes)) { ++unreadable_; }
        else {
            ++reads_;
            if (have_) {
                for (uint32_t i = 0; i < kWords; ++i) {
                    const uint32_t a = prev_[i], b = cur_[i];
                    if (a == b || (flags_[i] & kMuted)) continue;
                    if (++changes_[i] > kMuteChanges) { flags_[i] |= kMuted; ++muted_; continue; }
                    const bool dec = a >= 1 && a <= kMaxCounter + 1 && b == a - 1;
                    const bool rise = (flags_[i] & kCounted) && b > a + 1 && b <= kMaxCounter + 1;
                    if (dec) flags_[i] |= kCounted;
                    if ((dec || rise) && logged_ < kMaxLines) {
                        char t[256];
                        std::snprintf(t, sizeof t, "store-watch;event=%s;off=%+lld;addr=%llx;old=%u;new=%u;changes=%u;t-ms=%.0f",
                            dec ? "decrement" : "rise", static_cast<long long>(int64_t(i) * 4 - int64_t(kSpanBefore)), static_cast<unsigned long long>(from + uint64_t(i) * 4),
                            a, b, unsigned(changes_[i]), now_ms);
                        sink.line(true, t); ++logged_; ++lines;
                        if (dec) ++decrements_; else ++rises_;
                    }
                }
            }
            std::memcpy(prev_.get(), cur_.get(), kWindowBytes);
            have_ = true;
        }
        if (now_ms - summaryT_ >= kSummaryMs) {
            summaryT_ = now_ms;
            char t[256];
            std::snprintf(t, sizeof t, "store-watch;event=summary;base=%llx;window=%u;reads=%llu;unreadable=%llu;rebases=%llu;muted-words=%u;decrements=%u;rises=%u;lines=%u/%u",
                static_cast<unsigned long long>(base_), kWindowBytes, static_cast<unsigned long long>(reads_), static_cast<unsigned long long>(unreadable_),
                static_cast<unsigned long long>(rebases_), muted_, decrements_, rises_, logged_, kMaxLines);
            sink.line(true, t); ++lines;
        }
        return lines;
    }
    unsigned logged() const noexcept { return logged_; }
    unsigned muted() const noexcept { return muted_; }
    unsigned decrements() const noexcept { return decrements_; }
    unsigned rises() const noexcept { return rises_; }
    uint64_t reads() const noexcept { return reads_; }
private:
    enum : uint8_t { kMuted = 1, kCounted = 2 };
    std::unique_ptr<uint32_t[]> cur_, prev_;
    std::unique_ptr<uint8_t[]> changes_, flags_;
    bool have_ = false, haveT_ = false;
    uint64_t base_ = 0, reads_ = 0, unreadable_ = 0, rebases_ = 0;
    double lastT_ = 0, summaryT_ = 0;
    unsigned muted_ = 0, logged_ = 0, decrements_ = 0, rises_ = 0;
};

}  // namespace acz::e6store
