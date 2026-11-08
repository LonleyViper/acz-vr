#pragma once
// Look-drive -> XInput proxy IPC (xr_look_drive_xinput; docs/review/xinput-look-impl.md).
//
// One tiny shared-memory block, written by the acz-vr DLL (look_drive_xinput.h) and polled by the XINPUT1_4.dll proxy (skip_intro.cpp,
// only when skip-intro.ini has look_xinput=1). Header-only and OS-free so both sides and the offline tests share ONE definition.
//
// Safety model:
//  * the block only ever carries two small integers (a right-stick offset in XInput stick units) plus a clock stamp;
//  * stale (older than the staleness window), absent, malformed, future-stamped, inactive or torn data contributes ZERO (the real stick
//    state then passes through unchanged);
//  * the reader hard-clamps the offset to the configured maximum (never above 32767) and the sum with the real stick to +-32767;
//  * a kill switch (the existing look-drive.stop file, checked by the proxy OS code) latches the contribution to zero for the process;
//  * no game memory is written and no code is patched: the only effect is the XINPUT_STATE the game receives from XInputGetState.
//
// Coordinate convention: stick units exactly as XInput (x: right positive, y: up positive).
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace acz::look_ipc {

inline constexpr uint32_t kMagic = 0x58414C4Cu;   // 'LLAX'
inline constexpr uint32_t kVersion = 1;
inline constexpr int kStickMax = 32767;           // hard ceiling of any stick value (offset and sum)
inline constexpr uint32_t kDefaultStaleMs = 150;  // data older than this contributes nothing
inline constexpr uint32_t kMinStaleMs = 20, kMaxStaleMs = 1000;
inline constexpr uint64_t kFutureSkewMs = 50;     // a stamp this far ahead of the reader clock is rejected (clock mismatch / garbage)
inline constexpr wchar_t kMappingName[] = L"Local\\acz-vr-look-xinput-v1";

struct Block {                 // 40 bytes, written by one writer (the acz-vr DLL)
    volatile uint32_t seq;     // even = stable, odd = write in progress
    uint32_t magic;
    uint32_t version;
    int32_t x;                 // right-stick X offset, stick units
    int32_t y;                 // right-stick Y offset, stick units
    uint32_t flags;            // bit 0: active (the writer wants the offset applied)
    uint64_t stamp_ms;         // writer clock (GetTickCount64) at the publish
    uint32_t check;            // integrity word over the payload
    uint32_t pad;
};
static_assert(sizeof(Block) == 40, "ipc block layout");
inline constexpr uint32_t kFlagActive = 1u;

inline int clamp_stick(long long v, int limit = kStickMax) noexcept {
    if (limit > kStickMax) limit = kStickMax;
    if (limit < 0) limit = 0;
    return v > limit ? limit : (v < -limit ? -limit : static_cast<int>(v));
}
inline int to_stick(double duty) noexcept {   // duty in [-1,1] -> stick units; non-finite -> 0
    if (!std::isfinite(duty)) return 0;
    if (duty > 1.0) duty = 1.0;
    if (duty < -1.0) duty = -1.0;
    return clamp_stick(static_cast<long long>(std::lround(duty * kStickMax)));
}
// Compensation of the game's own right-stick deadzone (test 12: the game ignores right-stick magnitudes below ~19% of full scale). A non-zero
// offset v is remapped to dz*32767 + |v|*(1-dz) (sign kept) so that the game's (|s|-dz)/(1-dz) response becomes linear in the request again.
// 0 stays 0; dz <= 0 (or non-finite) returns v unchanged. dz is clamped to [0, 0.9].
inline int deadzone_remap(int v, double dz) noexcept {
    if (v == 0 || !std::isfinite(dz) || dz <= 0.0) return v;
    if (dz > 0.9) dz = 0.9;
    const double mag = std::fabs(static_cast<double>(v));
    const long long o = std::llround(dz * kStickMax + mag * (1.0 - dz));
    return clamp_stick(v < 0 ? -o : o);
}
inline uint32_t make_check(int32_t x, int32_t y, uint32_t flags, uint64_t stamp) noexcept {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ kMagic;
    h = (h ^ static_cast<uint32_t>(x)) * 0x100000001B3ull;
    h = (h ^ static_cast<uint32_t>(y)) * 0x100000001B3ull;
    h = (h ^ flags) * 0x100000001B3ull;
    h = (h ^ stamp) * 0x100000001B3ull;
    return static_cast<uint32_t>(h ^ (h >> 32));
}

// Single writer. x/y are clamped to +-kStickMax here as well (defence in depth); active=false publishes an explicit "apply nothing".
inline void publish(Block* b, int x, int y, uint64_t stamp_ms, bool active) noexcept {
    if (!b) return;
    const int cx = clamp_stick(x), cy = clamp_stick(y);
    const uint32_t flags = active ? kFlagActive : 0u;
    const uint32_t s = b->seq;
    b->seq = s | 1u;                       // odd: readers retry
    std::atomic_thread_fence(std::memory_order_seq_cst);
    b->magic = kMagic; b->version = kVersion;
    b->x = cx; b->y = cy; b->flags = flags; b->stamp_ms = stamp_ms;
    b->check = make_check(cx, cy, flags, stamp_ms);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    b->seq = (s | 1u) + 1u;                // next even value
}

struct Sample { bool ok = false; int x = 0, y = 0; const char* why = "absent"; };

// Reads one consistent snapshot and decides whether it may contribute. Never throws, never blocks.
inline Sample read_block(const Block* b, uint64_t now_ms, uint32_t stale_ms, int max_stick) noexcept {
    Sample out;
    if (!b) return out;
    Block c{};
    bool got = false;
    for (int tries = 0; tries < 4 && !got; ++tries) {
        const uint32_t s1 = b->seq;
        if (s1 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        c.magic = b->magic; c.version = b->version; c.x = b->x; c.y = b->y; c.flags = b->flags; c.stamp_ms = b->stamp_ms; c.check = b->check;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        got = (b->seq == s1);
    }
    if (!got) { out.why = "torn"; return out; }
    if (c.magic != kMagic || c.version != kVersion) { out.why = "bad-header"; return out; }
    if (c.check != make_check(c.x, c.y, c.flags, c.stamp_ms)) { out.why = "bad-check"; return out; }
    if (!(c.flags & kFlagActive)) { out.why = "inactive"; return out; }
    if (stale_ms < kMinStaleMs) stale_ms = kMinStaleMs;
    if (stale_ms > kMaxStaleMs) stale_ms = kMaxStaleMs;
    if (c.stamp_ms > now_ms + kFutureSkewMs) { out.why = "future-stamp"; return out; }
    if (now_ms > c.stamp_ms && now_ms - c.stamp_ms > stale_ms) { out.why = "stale"; return out; }
    out.x = clamp_stick(c.x, max_stick); out.y = clamp_stick(c.y, max_stick);
    out.ok = true; out.why = "fresh";
    return out;
}

// The proxy-side policy: reads the sample, applies the process-wide latched kill switch, adds the offset to the real stick.
class Override {
public:
    struct Result { int16_t rx = 0, ry = 0; bool injected = false; uint32_t packet_bump = 0; const char* why = "off"; };
    // deadzone: fraction of full stick the game ignores (look_xinput_deadzone); 0 = the offset is added unchanged (the former behaviour).
    Override(int max_stick = kStickMax, uint32_t stale_ms = kDefaultStaleMs, double deadzone = 0.0) noexcept
        : max_(clamp_stick(max_stick)), stale_(stale_ms), dz_(deadzone) {}
    // real_rx/real_ry: the physical pad's right stick. blk may be null (mapping absent). kill: the kill-switch file exists right now.
    Result apply(int16_t real_rx, int16_t real_ry, const Block* blk, uint64_t now_ms, bool kill) noexcept {
        Result r;
        r.rx = real_rx; r.ry = real_ry;
        if (kill) killed_ = true;                        // latched for the process, like the look-drive injector
        Sample s;
        if (killed_) { s.why = "kill-switch"; } else s = read_block(blk, now_ms, stale_, max_);
        int ix = 0, iy = 0;
        if (s.ok) { ix = clamp_stick(deadzone_remap(s.x, dz_), max_); iy = clamp_stick(deadzone_remap(s.y, dz_), max_); }
        if (ix != last_x_ || iy != last_y_) { ++bump_; last_x_ = ix; last_y_ = iy; }   // a changed offset must look like a new input packet
        r.packet_bump = bump_;
        r.why = s.why;
        last_why_ = s.why;
        if (s.ok) {
            // Real stick plus offset, hard-clamped (-32768 is mapped to -32767 so the clamp is symmetric).
            const int rx = clamp_stick(static_cast<long long>(real_rx) + ix), ry = clamp_stick(static_cast<long long>(real_ry) + iy);
            r.rx = static_cast<int16_t>(rx); r.ry = static_cast<int16_t>(ry);
            r.injected = true;
            ++injected_;
        } else { ++passed_; }
        return r;
    }
    bool killed() const noexcept { return killed_; }
    const char* last_why() const noexcept { return last_why_; }
    uint64_t injected() const noexcept { return injected_; }
    uint64_t passed() const noexcept { return passed_; }
    uint32_t bump() const noexcept { return bump_; }
private:
    int max_; uint32_t stale_; double dz_;
    bool killed_ = false; int last_x_ = 0, last_y_ = 0; uint32_t bump_ = 0;
    const char* last_why_ = "off"; uint64_t injected_ = 0, passed_ = 0;
};

} // namespace acz::look_ipc
