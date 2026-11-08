#pragma once
// XR flight-view heartbeat -> XInput proxy (LOOK-STICK GATE, D-355 problem 3; docs/world-render/look-stick-gate.md).
//
// Why: the terrain/props/cockpit-lit world is drawn from the GAME camera of each frame (the DrawCPT camera axes, else the E6 Euler `rot`), and that
// camera INCLUDES the game's cockpit look-around (E3: the right-stick/look control rotates the cockpit camera about the eye, up to ~145 deg; E6:
// rot pitch/yaw include the look). The model cockpit is drawn in the seat frame without the look. So moving the right stick in VR turns the whole
// world (and the game's replayed objects/HUD/effects images, which are drawn along the look) around a fixed cockpit. The look-drive that used the
// stick on purpose is parked (D-291). Fix: while acz-vr composes the in-flight world from the game camera it publishes a heartbeat here, and the
// user's XINPUT1_4.dll proxy (src/skip-intro) reports the right stick as centred to the game. Buttons, triggers and the left stick are untouched.
//
// Header-only and OS-free so the writer (acz-vr, xr_stereo_proxy.cpp), the reader (skip_intro.cpp) and the offline tests share ONE definition.
// Fail safe: an absent mapping, a zeroed/garbage/torn block, an inactive flag, a stale stamp (> stale window), a future stamp or a block owned by
// another process = NO gate (the real stick passes through). So the gate can only act while acz-vr is alive AND composing in-flight frames AND asks.
//
// Process binding (D-358, Astra review of D-355/356 finding 3): both DLLs run inside the SAME game process, so the mapping name carries that
// process id (`Local\acz-vr-xr-view-v2-<pid>`; `Local` alone is the whole Windows session) and the block records the writer's process id, which
// the reader requires to equal its own. One game process's heartbeat can never centre another process's stick, and each process has at most one
// writer. Instance generation: the writer never takes over a mapping that already holds a fresh active block or another process's block
// (can_adopt); a previous instance of the writer in this process (DLL reload) is adopted only once its block is inactive or stale.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace acz::xr_view_ipc {

inline constexpr uint32_t kMagic = 0x56525841u;   // 'AXRV'
inline constexpr uint32_t kVersion = 2;   // 2: owner_pid in the former pad word, per-process name
inline constexpr uint32_t kDefaultStaleMs = 500;  // the writer publishes every ~100 ms; older data = no gate
inline constexpr uint32_t kMinStaleMs = 100, kMaxStaleMs = 2000;
inline constexpr uint64_t kFutureSkewMs = 50;
inline constexpr wchar_t kMappingPrefix[] = L"Local\\acz-vr-xr-view-v2-";
inline constexpr size_t kMappingNameMax = 64;

// The per-process mapping name (OS-free; the caller passes GetCurrentProcessId()). Returns false if `cap` is too small.
inline bool mapping_name(wchar_t* out, size_t cap, uint32_t pid) noexcept {
    if (!out || cap == 0) return false;
    size_t n = 0;
    for (const wchar_t* p = kMappingPrefix; *p; ++p) { if (n + 1 >= cap) return false; out[n++] = *p; }
    wchar_t digits[10]; size_t d = 0;
    do { digits[d++] = wchar_t(L'0' + pid % 10u); pid /= 10u; } while (pid && d < 10);
    while (d) { if (n + 1 >= cap) return false; out[n++] = digits[--d]; }
    out[n] = 0;
    return true;
}

struct Block {                 // 32 bytes, one writer (the acz-vr DLL of the owning process)
    volatile uint32_t seq;     // even = stable, odd = write in progress
    uint32_t magic;
    uint32_t version;
    uint32_t flags;            // bit 0: block the right stick (in-flight composite from the game camera, lookstick:block)
    uint64_t stamp_ms;         // writer clock (GetTickCount64)
    uint32_t check;
    uint32_t owner_pid;        // the writer's process id; the reader requires its own
};
static_assert(sizeof(Block) == 32, "xr view ipc block layout");
inline constexpr uint32_t kFlagBlockLookStick = 1u;

inline uint32_t make_check(uint32_t flags, uint64_t stamp, uint32_t owner_pid) noexcept {
    uint64_t h = 0xC2B2AE3D27D4EB4Full ^ kMagic;
    h = (h ^ flags) * 0x100000001B3ull;
    h = (h ^ stamp) * 0x100000001B3ull;
    h = (h ^ owner_pid) * 0x100000001B3ull;
    return static_cast<uint32_t>(h ^ (h >> 32));
}

inline void publish(Block* b, bool block_look_stick, uint64_t stamp_ms, uint32_t owner_pid) noexcept {
    if (!b) return;
    const uint32_t flags = block_look_stick ? kFlagBlockLookStick : 0u;
    const uint32_t s = b->seq;
    b->seq = s | 1u;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    b->magic = kMagic; b->version = kVersion; b->flags = flags; b->stamp_ms = stamp_ms; b->owner_pid = owner_pid;
    b->check = make_check(flags, stamp_ms, owner_pid);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    b->seq = (s | 1u) + 1u;
}

struct Sample { bool block = false; const char* why = "absent"; };

// One consistent snapshot; block=true only for a fresh, valid, active block written by process `owner_pid` (the reader's own).
inline Sample read_block(const Block* b, uint64_t now_ms, uint32_t stale_ms, uint32_t owner_pid) noexcept {
    Sample out;
    if (!b) return out;
    uint32_t magic = 0, version = 0, flags = 0, check = 0, owner = 0; uint64_t stamp = 0;
    bool got = false;
    for (int tries = 0; tries < 4 && !got; ++tries) {
        const uint32_t s1 = b->seq;
        if (s1 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        magic = b->magic; version = b->version; flags = b->flags; stamp = b->stamp_ms; check = b->check; owner = b->owner_pid;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        got = (b->seq == s1);
    }
    if (!got) { out.why = "torn"; return out; }
    if (magic != kMagic || version != kVersion) { out.why = "bad-header"; return out; }
    if (check != make_check(flags, stamp, owner)) { out.why = "bad-check"; return out; }
    if (!owner_pid || owner != owner_pid) { out.why = "foreign-owner"; return out; }
    if (!(flags & kFlagBlockLookStick)) { out.why = "inactive"; return out; }
    if (stale_ms < kMinStaleMs) stale_ms = kMinStaleMs;
    if (stale_ms > kMaxStaleMs) stale_ms = kMaxStaleMs;
    if (stamp > now_ms + kFutureSkewMs) { out.why = "future-stamp"; return out; }
    if (now_ms > stamp && now_ms - stamp > stale_ms) { out.why = "stale"; return out; }
    out.block = true; out.why = "in-flight";
    return out;
}

// Proxy-side policy. enabled = skip-intro.ini xr_block_look_stick (default 1). Returns the right stick the game receives.
class StickGate {
public:
    struct Result { int16_t rx = 0, ry = 0; bool blocked = false; uint32_t packet_bump = 0; const char* why = "off"; };
    // owner_pid: the reader's own process id (GetCurrentProcessId()); only that process's writer can gate this stick.
    StickGate(uint32_t stale_ms, uint32_t owner_pid) noexcept : stale_(stale_ms), owner_(owner_pid) {}
    Result apply(int16_t real_rx, int16_t real_ry, const Block* blk, uint64_t now_ms, bool enabled) noexcept {
        Result r;
        r.rx = real_rx; r.ry = real_ry;
        Sample s;
        if (!enabled) s.why = "disabled"; else s = read_block(blk, now_ms, stale_, owner_);
        // The output changes without a new pad packet when the gate opens/closes with the stick deflected: bump the packet number then.
        const bool deflected = real_rx != 0 || real_ry != 0;
        if (s.block != last_block_ && deflected) ++bump_;
        last_block_ = s.block;
        r.packet_bump = bump_;
        r.why = s.why;
        if (s.block) {
            r.rx = 0; r.ry = 0; r.blocked = true;
            if (deflected) ++blocked_;
        }
        return r;
    }
    uint64_t blocked() const noexcept { return blocked_; }   // polls whose real right stick was deflected and reported centred
    uint32_t bump() const noexcept { return bump_; }
private:
    uint32_t stale_, owner_;
    bool last_block_ = false; uint32_t bump_ = 0; uint64_t blocked_ = 0;
};

// Writer-side instance-generation rule for a mapping that ALREADY existed when the writer created it (ERROR_ALREADY_EXISTS). A fresh mapping
// (zero-filled) is always ours. An existing one is adopted only if it carries no live heartbeat (not a fresh active block for this owner) and is
// either unwritten or written by this same process (a previous, now-silent instance of the writer). A block written by another process id is
// never adopted (the writer then stays off: fail open, the real stick passes).
inline bool can_adopt(const Block* b, uint64_t now_ms, uint32_t stale_ms, uint32_t owner_pid) noexcept {
    if (!b || !owner_pid) return false;
    const uint32_t owner = b->owner_pid;
    if (b->magic != kMagic && owner == 0) return true;            // never written
    if (owner != owner_pid) return false;                         // another process's block
    return !read_block(b, now_ms, stale_ms, owner_pid).block;     // our process, but no live writer
}

// Writer-side decision, called on a ~100 ms tick: block while in-flight composites (terrain drawn from the game/E6 camera) keep arriving.
// composites = a monotonically increasing count of such composites; want = the lookstick option (block) and an XR session that hooked.
class Heartbeat {
public:
    bool tick(uint64_t composites, bool want) noexcept {
        const bool moved = have_ && composites != last_;
        last_ = composites; have_ = true;
        active_ = want && moved;
        return active_;
    }
    bool active() const noexcept { return active_; }
private:
    uint64_t last_ = 0; bool have_ = false, active_ = false;
};

} // namespace acz::xr_view_ipc
