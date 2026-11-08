#pragma once
// J-cockpit-fov F2 (gpu_copy_step5_precockpit_observe, default OFF): a CPU-only, bounded,
// read-only log of the DrawCPT draws that PRECEDE the selected cockpit members in a list
// generation (ordinals 1 .. members[0]-1). Modelled on the J-171 HUD tail observer:
//  * the Draw seam copies a fixed-size POD record (no allocation, no formatting, no I/O, no
//    native call, nothing recorded into the game's list); the 416 SrtData bytes come from the
//    same CPU-mapped UPLOAD read the inline live-bytes path uses (step3MappedSpan_locked +
//    guardedCpuRead): NO GPU readback is added;
//  * the worker derives the camera-relative facts and formats two lines per record into the
//    dedicated step5-producer.log.
// Sampling: the first 3 generations with pre-cockpit draws, then one generation per 5 s status
// period; at most 32 records per sampled generation (+ one truncation marker), a 128-record
// queue (overflow counted as lost). Caller serializes State (a leaf mutex).
#include "gpu_copy_step5.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::gpu_copy::precockpit {
constexpr unsigned sampled_full=3,draws_per_frame=32,queue_records=128;
constexpr unsigned line_bytes=480; // feature_log::line_cap; two lines per record
struct Record {
    bool marker=false;               // a truncation marker (draws beyond the per-generation cap)
    uint64_t frame=0,list=0,generation=0,ordinal=0,first_member=0,pso=0,id=0,root=0,t10_va=0,vb_bytes=0;
    uint32_t topology=0,ib=0,vb=0,t10=0,cut=0,rtv=0,dsv=0,samples=0,depth_func=0;
    bool depth_enable=false,depth_write=false,stencil=false,blend=false,pso_known=false;
    bool indexed=false;uint32_t count=0,instances=0,start=0,start_instance=0;int32_t base=0;
    const char* state="ok";          // the per-member state verdict (decide + step-5 ring rules), static text
    const char* klass="?";           // selected / family / out-of-class-* (static text)
    int class_index=-1;              // index of the equal selected class, -1 none
    const char* srt="not-read";      // "read" or why the CPU-mapped SrtData could not be read
    std::array<uint8_t,416> bytes{};
};
struct State {
    bool active=false,sampling=false,cut=false,due=false;
    uint64_t list=0,generation=0,frames=0,sampled=0,records=0,lost=0,truncated=0;
    unsigned frameDraws=0;
    std::array<Record,queue_records> queue{};uint64_t head=0,tail=0;
    // Worker-only scratch for derive().
    step5::PoseFrame pf{};step5::Scratch sc{};
    // Draw seam: true when this draw of (list, generation) is recorded (the caller then pushes).
    // A new generation decides sampling once; beyond the cap one marker is queued.
    bool admit(uint64_t l,uint64_t g) noexcept {
        if(!(active&&l==list&&g==generation)) {
            active=true;list=l;generation=g;++frames;cut=false;frameDraws=0;
            sampling=sampled<sampled_full||due;
            if(sampling){++sampled;due=false;}
        }
        if(!sampling)return false;
        if(frameDraws>=draws_per_frame) {
            if(!cut){cut=true;++truncated;Record m;m.marker=true;m.frame=sampled;m.list=l;m.generation=g;push(m);}
            return false;
        }
        ++frameDraws;return true;
    }
    void push(const Record& r) noexcept {
        if(head-tail==queue_records){++lost;return;}
        queue[head++%queue_records]=r;++records;
    }
    bool pop(Record& r) noexcept {if(tail==head)return false;r=queue[tail++%queue_records];return true;}
};
// Worker: facts derived from the record's own SrtData (the draw's OWN camera VP = inverse(M256) *
// M64, its local origin's world position M256 row 3 and clip position M64 row 3).
struct Derived {
    const char* camera="srt-not-read";bool ok=false;
    double eye[3]{},axes[3][3]{},tan[2]{},t[3]{},rel[3]{},relCam[3]{},dist=0,scale=0,clip[4]{};
    const char* verdict="unknown";
};
inline void derive(const Record& r,State& s,Derived& d) noexcept {
    d=Derived{};
    if(std::strcmp(r.srt,"read")!=0){d.camera=r.srt;return;}
    step5::Lens lens;
    if(const char* why=step5::camera_frame(r.bytes.data(),s.pf,s.sc,lens)){d.camera=why;return;}
    const auto& c=s.pf.cam;d.ok=true;d.camera="recovered";
    for(int k=0;k<3;++k){d.eye[k]=c.eye[k];d.axes[0][k]=c.right[k];d.axes[1][k]=c.up[k];d.axes[2][k]=c.fwd[k];}
    d.tan[0]=c.tanX;d.tan[1]=c.tanY;
    const double* m256=s.sc.m256;const double* m64=s.sc.m64;
    for(int k=0;k<3;++k){d.t[k]=m256[12+k];d.rel[k]=d.t[k]-d.eye[k];}
    for(int a=0;a<3;++a)d.relCam[a]=step5::dot3(d.rel,d.axes[a]);
    d.dist=std::sqrt(step5::dot3(d.rel,d.rel));
    d.scale=std::sqrt(m256[0]*m256[0]+m256[1]*m256[1]+m256[2]*m256[2]);
    for(int k=0;k<4;++k)d.clip[k]=m64[12+k];
    const double w=d.clip[3];
    d.verdict=!(w>0)?"origin-behind-camera":(std::fabs(d.clip[0])<=w&&std::fabs(d.clip[1])<=w)?"origin-in-frustum":"origin-outside-frustum";
}
inline void format(const Record& r,const Derived& d,char (&a)[line_bytes],char (&b)[line_bytes]) noexcept {
    using U=unsigned long long;
    auto mark=[](char* p){static constexpr char s[]=";truncated=1";std::memcpy(p+line_bytes-sizeof s,s,sizeof s);};
    if(r.marker) {
        std::snprintf(a,sizeof a,"precockpit;event=truncated;frame=%llu;list=%llx;gen=%llx;cap=%u-draws-per-frame",U(r.frame),U(r.list),U(r.generation),draws_per_frame);
        b[0]=0;return;
    }
    const int n=std::snprintf(a,sizeof a,"precockpit;event=draw;frame=%llu;list=%llx;gen=%llx;ordinal=%llu;first-member=%llu;pso=%llx;id=%llu;root=%llx;"
        "state=%s;class=%s;class-index=%d;topo=%u;ib=%u;vb=%u;t10=%u;cut=%x;rtv=%u;dsv=%u;samples=%u;depth=%u/%u/%u;stencil=%u;blend=%u;pso-known=%u;"
        "draw=%s%u,%u,%u,%d,%u;vb-view-vertices=%llu;t10-va=%llx",U(r.frame),U(r.list),U(r.generation),U(r.ordinal),U(r.first_member),U(r.pso),U(r.id),
        U(r.root),r.state?r.state:"ok",r.klass?r.klass:"?",r.class_index,r.topology,r.ib,r.vb,r.t10,r.cut,r.rtv,r.dsv,r.samples,unsigned(r.depth_enable),
        unsigned(r.depth_write),r.depth_func,unsigned(r.stencil),unsigned(r.blend),unsigned(r.pso_known),r.indexed?"i":"n",r.count,r.instances,r.start,
        int(r.base),r.start_instance,U(r.vb&&r.vb_bytes?r.vb_bytes/r.vb:0),U(r.t10_va));
    const int m=std::snprintf(b,sizeof b,"precockpit-srt;frame=%llu;gen=%llx;ordinal=%llu;srt=%s;camera=%s;eye=%.3f,%.3f,%.3f;fwd=%.4f,%.4f,%.4f;lens-tan=%.5f,%.5f;"
        "m256-t=%.3f,%.3f,%.3f;rel=%.3f,%.3f,%.3f;rel-cam=right:%.3f,up:%.3f,fwd:%.3f;dist=%.3f;m256-scale=%.4g;origin-clip=%.4g,%.4g,%.4g,%.4g;verdict=%s;"
        "tris=not-read",U(r.frame),U(r.generation),U(r.ordinal),r.srt,d.camera,d.eye[0],d.eye[1],d.eye[2],d.axes[2][0],d.axes[2][1],
        d.axes[2][2],d.tan[0],d.tan[1],d.t[0],d.t[1],d.t[2],d.rel[0],d.rel[1],d.rel[2],d.relCam[0],d.relCam[1],d.relCam[2],d.dist,d.scale,d.clip[0],d.clip[1],
        d.clip[2],d.clip[3],d.ok?d.verdict:"unknown");
    if(n<0||n>=int(sizeof a))mark(a);
    if(m<0||m>=int(sizeof b))mark(b);
}
} // namespace acz::gpu_copy::precockpit
