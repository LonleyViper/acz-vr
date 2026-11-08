#pragma once
// J-171: CPU-only, bounded observer. Caller serializes State. No SDK, ownership,
// allocation, command recording or file I/O. Formatting/draining is worker-only.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::gpu_copy::hud {
constexpr unsigned frame_records=64,run_records=4096,queue_records=256,rt_addresses=8;
constexpr unsigned line_bytes=480; // feature_log::line_cap; two lines per record
enum class Event {tail,rt_change,scene_barrier,r10,close,member_resume,truncated};
inline const char* name(Event e) noexcept {
    switch(e) {
    case Event::tail:return "tail";case Event::rt_change:return "rt_change";
    case Event::scene_barrier:return "scene_barrier";case Event::r10:return "r10";
    case Event::close:return "close";case Event::member_resume:return "member_resume";
    default:return "truncated";
    }
}
struct Record {
    Event event=Event::tail;
    uint64_t frame=0,list=0,generation=0,seq=0,member=0,scene=0,rt=0,pipeline=0,id=0,resume_member=0;
    uint32_t format=0,count=0,instances=0,before=0,after=0,barrier_flags=0,subresource=0;
    bool rt_known=false,pso_known=false,viewport_known=false,matched=false,indexed=false,truncated=false;
    uint32_t depth_func=0,depth_write=0,stencil=0,blend=0,mask=0;
    uint32_t stencil_read=0,stencil_write=0,front_func=0,back_func=0,front_pass=0,independent_blend=0;
    float viewport[6]{};
    char vs[33]{},ps[33]{};
};
struct State {
    bool active=false,sampled=false,resumed=false,tailSeen=false,changed=false,barrierSeen=false,cut=false;
    uint64_t frames=0,list=0,generation=0,scene=0,seq=0,member=0;
    unsigned frameCount=0,runCount=0,addressCount=0;
    std::array<uint64_t,rt_addresses> addresses{};
    std::array<Record,queue_records> queue{};
    uint64_t head=0,tail=0,lost=0;
    static bool sample(uint64_t f) noexcept {return f<=3||f%600==0;}
    bool same(uint64_t l,uint64_t g) const noexcept {return active&&l==list&&g==generation;}
    void begin(uint64_t l,uint64_t g,uint64_t source) noexcept {
        active=true;sampled=sample(++frames);list=l;generation=g;scene=source;
        seq=member=0;frameCount=addressCount=0;resumed=tailSeen=changed=barrierSeen=cut=false;
    }
    void emit(Record r) noexcept {
        if(!sampled||cut||runCount>=run_records)return;
        // Reserve the last record for an explicit evidence-loss marker.
        if(frameCount==frame_records-1||runCount==run_records-1) {r.event=Event::truncated;r.truncated=true;cut=true;}
        r.frame=frames;r.list=list;r.generation=generation;r.seq=seq+(r.event==Event::member_resume?1u:0u);r.member=member;r.scene=scene;
        ++frameCount;++runCount;
        if(head-tail==queue_records) {++lost;queue[(head-1)%queue_records].truncated=true;return;}
        queue[head++%queue_records]=r;
    }
    void matched_member(uint64_t ordinal) noexcept {
        if(!active)return;
        if(tailSeen&&!resumed) {Record r;r.event=Event::member_resume;r.resume_member=ordinal;emit(r);resumed=true;}
        member=ordinal;
    }
    void target(Record r) noexcept {
        if(!same(r.list,r.generation)||!member)return;
        if(r.rt_known&&scene&&r.rt!=scene&&!changed) {changed=true;r.event=Event::rt_change;emit(r);}
        if(r.rt_known&&r.format==24) {
            for(unsigned i=0;i<addressCount;++i)if(addresses[i]==r.rt)return;
            if(addressCount==rt_addresses) {r.event=Event::truncated;r.truncated=true;emit(r);return;}
            addresses[addressCount++]=r.rt;r.event=Event::r10;emit(r);
        }
    }
    void draw(Record r) noexcept {
        if(!same(r.list,r.generation))return;
        ++seq;target(r);
        // Missing PSO metadata is unknown, never evidence of an unmatched draw.
        if(member&&!r.matched) {tailSeen=true;r.event=Event::tail;if(!r.pso_known)r.truncated=true;emit(r);}
    }
    void barrier(Record r) noexcept {
        if(!same(r.list,r.generation)||!member||barrierSeen||!scene||r.rt!=scene||!(r.before&4)||(r.after&4))return;
        barrierSeen=true;r.event=Event::scene_barrier;emit(r);
    }
    void close(uint64_t l,uint64_t g,bool ok) noexcept {
        if(!same(l,g))return;
        Record r;r.event=Event::close;r.after=unsigned(ok);emit(r);active=false;
    }
    bool pop(Record& r) noexcept {if(tail==head)return false;r=queue[tail++%queue_records];return true;}
};
// Every event uses two correlated lines so no normal record exceeds the shared
// 479-character payload limit. Zero addresses/formats/hashes mean unknown.
inline void format(const Record& r,char (&a)[line_bytes],char (&b)[line_bytes]) noexcept {
    using U=unsigned long long;
    const int n=std::snprintf(a,sizeof a,"hud;back_buffer=unknown;event=%s;frame=%llu;list=%llx;generation=%llu;tail_seq=%llu;after_member=%llu;provisional=%u;pipeline=%llx;id=%llu;vs=%s;ps=%s;rt_class=%s;rt=%llx;rt_format=%u;fmt_r10g10b10a2=%s;truncated=%u",
        name(r.event),U(r.frame),U(r.list),U(r.generation),U(r.seq),U(r.member),unsigned(r.event==Event::tail),U(r.pipeline),U(r.id),
        r.vs[0]?r.vs:"0/unknown",r.ps[0]?r.ps:"0/unknown",r.rt_known&&r.scene?(r.rt==r.scene?"scene":"other"):"unknown",U(r.rt),r.format,
        r.format?(r.format==24?"1":"0"):"unknown",unsigned(r.truncated));
    const int m=std::snprintf(b,sizeof b,"hud-state;back_buffer=unknown;event=%s;frame=%llu;list=%llx;generation=%llu;tail_seq=%llu;after_member=%llu;pso_known=%u;depth_func=%u;depth_write=%u;stencil=%u,%u,%u,%u,%u,%u;blend=%u,%u;write_mask=%u;viewport_known=%u;viewport=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g;indexed=%u;count=%u;instances=%u;before=%u;after=%u;resume_member=%llu;barrier_flags=%u;subresource=%u;truncated=%u",
        name(r.event),U(r.frame),U(r.list),U(r.generation),U(r.seq),U(r.member),unsigned(r.pso_known),r.depth_func,r.depth_write,r.stencil,r.stencil_read,r.stencil_write,r.front_func,r.back_func,r.front_pass,r.blend,r.independent_blend,r.mask,
        unsigned(r.viewport_known),double(r.viewport[0]),double(r.viewport[1]),double(r.viewport[2]),double(r.viewport[3]),double(r.viewport[4]),double(r.viewport[5]),
        unsigned(r.indexed),r.count,r.instances,r.before,r.after,U(r.resume_member),r.barrier_flags,r.subresource,unsigned(r.truncated));
    auto mark=[](char* p){static constexpr char s[]=";truncated=1";std::memcpy(p+line_bytes-sizeof s,s,sizeof s);};
    if(n<0||n>=int(sizeof a))mark(a);
    if(m<0||m>=int(sizeof b))mark(b);
}
} // namespace acz::gpu_copy::hud
