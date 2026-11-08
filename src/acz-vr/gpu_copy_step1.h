#pragma once
// GPU-copy step 3b part 2: design section 6 "Step 1 - one known-pattern copy,
// no game data". SDK-free pieces shared by the hook adapter
// (gpu_copy_step1_hooks.inl, included by d3d12_hook.cpp) and its offline
// fixture. Default OFF: nothing here runs unless the experiment owner is
// enabled by gpu_copy_step1_experiment plus every diagnostics opt-in.
#include "gpu_copy_ledger.h"
#include <cstddef>
#include <cstdint>

namespace acz::gpu_copy {
// The only production entry that arms the shared diagnostic ledger session.
// Narrow friend of Session; used by the step-1 owner at the Draw seam only.
struct Step1Owner {
    static bool arm(Session& s,Backend& b,const Admission& a) noexcept {return s.arm(true,b,a);}
    static bool reserve(Session& s,const Admission& a) noexcept {return s.reserve(a);}
    static bool created(Session& s,uint64_t private_output,uint64_t source) noexcept {
        return s.created(true,private_output,source);
    }
    static bool possible_recording(Session& s) noexcept {return s.possible_recording();}
    // `why`: static literal recorded as the ledger's first fault (J-3b5).
    static void uncertain(Session& s,const char* why="step1-owner") noexcept {s.uncertain(why);}
};
namespace step1 {
// One copy of exactly this many bytes, UPLOAD offset 0 -> READBACK offset 0.
constexpr uint64_t copy_bytes=4096;
// Conservative logical charge: each committed 4096-byte buffer is assumed to
// occupy one 64 KiB placement (D3D12 default resource alignment). Not measured.
constexpr uint64_t charged_bytes=2*65536;
// Opaque ledger tokens for the two private owners (never addresses).
constexpr uint64_t private_token=0x51e1, source_token=0x51e2;
// Worker cadence and hard bounds (ticks are 100 ms in production).
constexpr unsigned tick_ms=100;
constexpr unsigned wait_ticks=6000;      // <=600 s for one qualified matched draw
constexpr unsigned status_every=600;     // <=10 ordinary-ring status lines while waiting
constexpr unsigned submit_ticks=100;     // <=10 s from insertion to Execute+Signal
constexpr unsigned closure_ticks=100;    // <=10 s from completion to observed Reset
constexpr const char* pattern_name="xorshift32-le;seed=0xace0c0de;words=1024";
// Deterministic generator: 1024 little-endian words of xorshift32 (13,17,5).
inline void pattern(uint8_t* out,size_t n) noexcept {
    uint32_t x=0xace0c0deu;
    for(size_t i=0;i+4<=n;i+=4) {
        x^=x<<13;x^=x>>17;x^=x<<5;
        out[i]=uint8_t(x);out[i+1]=uint8_t(x>>8);out[i+2]=uint8_t(x>>16);out[i+3]=uint8_t(x>>24);
    }
}
inline void hex(const uint8_t* bytes,size_t n,char* out) noexcept {
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}
    out[2*n]=0;
}
// First differing offset, or -1 when equal.
inline long long first_mismatch(const uint8_t* a,const uint8_t* b,size_t n) noexcept {
    for(size_t i=0;i<n;++i)if(a[i]!=b[i])return static_cast<long long>(i);
    return -1;
}
} // namespace step1
} // namespace acz::gpu_copy
