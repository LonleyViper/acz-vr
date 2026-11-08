#pragma once
// J-w1-ee-camera: the per-frame RECOVERED CAMERA BASIS from the live SrtData of
// the producer's first replayed cockpit member (step 5, gpu_copy_step5_hooks.inl
// step5FrameSetup). Same maths as the producer's pose path (gpu_copy_step5.h):
// VP = inverse(M256) * M64 (row-vector), decomposed into eye, right/up/forward
// and the symmetric lens. The Draw path only copies the 128 matrix bytes into a
// single-writer seqlock (32 relaxed atomic words, no allocation, no maths); the
// reader (the ee-camera probe's Present tick) decomposes.
//
// Header-only: no link dependency for the fixtures that include d3d12_hook.cpp.
#include "gpu_copy_step5.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::ee_camera {

struct Basis {
    uint64_t seq=0;                 // publication number (1 = first frame)
    double eye[3]{},right[3]{},up[3]{},fwd[3]{};
    double tanX=0,tanY=0;
    // Aircraft attitude (rev 4): the cockpit member's local->world M256 row 2 (its
    // local forward axis in world), normalised; air_ok=false when degenerate.
    double air[3]{};bool air_ok=false;
};

// Decompose from the two float32 row-major matrices (M64 = local->clip,
// M256 = local->world, affine). false when degenerate / non-finite.
inline bool decompose_basis(const float* m64f,const float* m256f,Basis& out) noexcept {
    double m64[16],m256[16],a[16],inv[16],vp[16];
    for(int i=0;i<16;++i){m64[i]=double(m64f[i]);m256[i]=double(m256f[i]);
        if(!std::isfinite(m64[i])||!std::isfinite(m256[i]))return false;}
    if(m256[3]!=0||m256[7]!=0||m256[11]!=0||m256[15]!=1)return false;
    std::memcpy(a,m256,sizeof a);
    if(!gpu_copy::step5::inv44(a,inv))return false;
    gpu_copy::step5::mul44(inv,m64,vp);
    gpu_copy::step5::Camera c{};
    if(!gpu_copy::step5::decompose(vp,c))return false;
    for(int k=0;k<3;++k){out.eye[k]=c.eye[k];out.right[k]=c.right[k];out.up[k]=c.up[k];out.fwd[k]=c.fwd[k];}
    out.tanX=c.tanX;out.tanY=c.tanY;
    {const double n=std::sqrt(m256[8]*m256[8]+m256[9]*m256[9]+m256[10]*m256[10]);
     out.air_ok=n>1e-9&&std::isfinite(n);if(out.air_ok)for(int k=0;k<3;++k)out.air[k]=m256[8+k]/n;}
    return true;
}

class BasisMailbox {
public:
    // Draw path (single writer: step5FrameSetup runs under the step-1 mutex).
    // `srt` is the member's live 416-byte SrtData; reads +64..+128 and +256..+320.
    void publish(const uint8_t* srt) noexcept {
        if(!srt)return;
        const uint32_t s=seq_.load(std::memory_order_relaxed);
        seq_.store(s+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for(int i=0;i<16;++i){uint32_t w;std::memcpy(&w,srt+64+4*i,4);words_[i].store(w,std::memory_order_relaxed);}
        for(int i=0;i<16;++i){uint32_t w;std::memcpy(&w,srt+256+4*i,4);words_[16+i].store(w,std::memory_order_relaxed);}
        count_.store(count_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        seq_.store(s+2,std::memory_order_release);
    }
    // Any thread. false when nothing was published, a publication was in
    // progress (try-only, no retry loop) or the matrices are degenerate.
    bool try_read(Basis& out) const noexcept {
        const uint32_t s1=seq_.load(std::memory_order_acquire);
        if(s1==0||(s1&1u))return false;
        float m64[16],m256[16];uint64_t n;
        for(int i=0;i<16;++i){const uint32_t w=words_[i].load(std::memory_order_relaxed);std::memcpy(&m64[i],&w,4);}
        for(int i=0;i<16;++i){const uint32_t w=words_[16+i].load(std::memory_order_relaxed);std::memcpy(&m256[i],&w,4);}
        n=count_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(seq_.load(std::memory_order_relaxed)!=s1)return false;
        Basis b{};b.seq=n;
        if(!decompose_basis(m64,m256,b))return false;
        out=b;return true;
    }
    uint64_t published() const noexcept {return count_.load(std::memory_order_relaxed);}
private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[32]{};
    std::atomic<uint64_t> count_{0};
};

// Euler angles of a basis (radians). Convention ASSUMPTION (documented in
// J-w1-ee-camera-findings.md): y is up; yaw = atan2(fwd.x, fwd.z),
// pitch = asin(fwd.y), roll = atan2(right.y, up.y). The probe compares
// CHANGES relative to a reference, magnitude-first, so a sign or axis-order
// difference shows as a reported sign, not as a false NONE.
inline double basis_yaw(const Basis& b) noexcept {return std::atan2(b.fwd[0],b.fwd[2]);}
inline double basis_pitch(const Basis& b) noexcept {const double y=b.fwd[1];return std::asin(y>1?1:(y<-1?-1:y));}
inline double basis_roll(const Basis& b) noexcept {return std::atan2(b.right[1],b.up[1]);}
inline double basis_air_yaw(const Basis& b) noexcept {return b.air_ok?std::atan2(b.air[0],b.air[2]):0.0;}

} // namespace acz::ee_camera
