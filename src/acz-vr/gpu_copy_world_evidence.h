#pragma once
// J-world-evidence (Astra reply 5, docs/interception/astra-reply-5.md; default OFF, no key of its own: it runs under the existing
// gpu_copy_step5_world_observe / gpu_copy_step5_world_objects keys). Bounded, POD, allocation-free evidence about the DrawACM_v draws:
//  (1) RT slot 1: per ACM PSO the saved output facts (rt_count, all rtv[] formats, slot-1 blend/write mask) and the ORIGINAL draw's
//      OMSetRenderTargets metadata with slot 0 / slot 1 provenance, emitted at the first occurrence and at every change of
//      (PSO identity, OM binding epoch) (per-PSO and total caps; the rest is counted);
//  (2) the 416-byte liveSrt snapshot the draw's acceptance decision used, copied into a fixed slot (never re-read), at most 3 per
//      window and at most `window_cap` windows per process, with the decision / cb10 facts, formatted (hex + FNV-1a 64) by the worker;
//      plus a bounded per-reason decision summary so refused classes stay visible.
// The caller serialises State with a leaf mutex (never held across another lock, formatting, feature_log or native calls).
#include "gpu_copy_step5_world.h"
#include "root_constant_witness.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::gpu_copy::wev {
constexpr unsigned line_bytes=480;                  // feature_log::line_cap
constexpr unsigned rec_per_window=3,window_cap=8;   // Astra: <=3 records per window, a per-process cap on windows
constexpr unsigned rt_queue=24,rt_psos=16,rt_changes_per_pso=6,rt_record_cap=48;
constexpr unsigned reason_cap=24,ordinal_table=8;
constexpr uint64_t window_gap_ms=2000,slot_age_ms=750;
constexpr unsigned text_cap=56;

// ------------------------------------------------------------- RT slot facts (Draw seam, hook-state mutex held while collecting)
enum SlotKind : uint8_t {sk_unknown=0,sk_unbound=1,sk_null_descriptor=2,sk_resource=3,sk_range_unresolved=4,sk_om_never_set=5};
struct SlotFacts {
    uint8_t kind=sk_unknown;const char* detail="";   // detail: static string (why unknown / unbound)
    uint64_t handle=0,serial=0,bindSerial=0,resource=0,width=0;
    uint32_t height=0,resFormat=0,viewFormat=0,viewDim=0,mip=0,slice=0;bool viewDesc=false,current=false;
};
struct RtFacts {
    uint64_t list=0,generation=0,pso=0,psoId=0,key=0;uint32_t ordinal=0;
    char vs[33]{},ps[33]{};
    bool psoKnown=false;uint32_t rtCount=0,rtv[8]{};bool blend0=false,independent=false,blend1=false;uint32_t mask0=0,mask1=0;
    bool omSet=false,omSingle=false,omProof=false;uint32_t omCount=0;uint64_t handles[8]{},bindEpoch=0,curEpoch=0;
    SlotFacts slot0,slot1;
};
inline uint64_t fnv64(const uint8_t* p,size_t n,uint64_t h=1469598103934665603ull) noexcept {
    for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;
}
inline void mix(uint64_t& h,uint64_t v) noexcept {h=fnv64(reinterpret_cast<const uint8_t*>(&v),sizeof v,h);}
// The OM binding epoch key: everything the original OMSetRenderTargets bind carries that matters here (count, form, handles, bind-time
// serials, copy epoch) plus whether the slot 0/1 views are still the ones present at the bind (`current`).
inline uint64_t om_key(const RtFacts& f) noexcept {
    uint64_t h=1469598103934665603ull;
    mix(h,f.psoKnown);mix(h,f.rtCount);for(unsigned i=0;i<8;++i)mix(h,f.rtv[i]); // the saved PSO output facts (a PSO identity can be re-created)
    mix(h,f.omSet);mix(h,f.omCount);mix(h,f.omSingle);mix(h,f.omProof);mix(h,f.bindEpoch);
    const unsigned n=f.omCount<8?f.omCount:8;
    for(unsigned i=0;i<n;++i)mix(h,f.handles[i]);
    mix(h,f.slot0.kind);mix(h,f.slot0.bindSerial);mix(h,f.slot0.serial);mix(h,f.slot0.current);
    mix(h,f.slot1.kind);mix(h,f.slot1.bindSerial);mix(h,f.slot1.serial);mix(h,f.slot1.current);
    return h;
}
inline const char* slot_kind_name(unsigned k) noexcept {
    static constexpr const char* n[]={"UNKNOWN","UNBOUND","NULL-DESCRIPTOR","RESOURCE","range-unresolved","UNKNOWN"};return k<6?n[k]:"?";
}
// One slot as a token: `RESOURCE(res=..,view-serial=..,...)`; the detail says WHY for the non-resource kinds.
inline int fmt_slot(char* out,size_t cap,const SlotFacts& s) noexcept {
    using U=unsigned long long;
    if(s.kind==sk_resource)
        return std::snprintf(out,cap,"RESOURCE(res=%llx,view-serial=%llu,bind-serial=%llu,still-current=%u,res-fmt=%u,%llux%u,view-desc=%s,view-fmt=%u,view-dim=%u,mip=%u,first-slice=%u)",
            U(s.resource),U(s.serial),U(s.bindSerial),unsigned(s.current),s.resFormat,U(s.width),s.height,s.viewDesc?"yes":"none-recorded",s.viewFormat,s.viewDim,s.mip,s.slice);
    if(s.kind==sk_null_descriptor)
        return std::snprintf(out,cap,"NULL-DESCRIPTOR(handle=%llx,view-serial=%llu,bind-serial=%llu,still-current=%u)",U(s.handle),U(s.serial),U(s.bindSerial),unsigned(s.current));
    if(s.handle)return std::snprintf(out,cap,"%s(%s,handle=%llx)",slot_kind_name(s.kind),s.detail,U(s.handle));
    return std::snprintf(out,cap,"%s(%s)",slot_kind_name(s.kind),s.detail);
}

// ------------------------------------------------------------- record snapshot (Draw seam)
struct SrtMeta {
    uint64_t list=0,generation=0,pso=0,psoId=0,t10Va=0,t10Address=0,t10ResSerial=0,t10Offset=0;uint32_t ordinal=0,t10Param=UINT32_MAX,t10Vis=UINT32_MAX;
    bool t10OffsetKnown=false;char vs[33]{},ps[33]{},path[12]{},read[text_cap]{},decision[text_cap]{};bool accepted=false,hasBytes=false,observed=false;
    step5::world::Cb10 cb10{};
};
struct Srt {
    bool used=false,ready=false;uint64_t born=0,window=0,execSeq=0;unsigned rec=0;SrtMeta m{};uint8_t bytes[416]{};
};
inline void copy_text(char* dst,size_t cap,const char* src) noexcept {
    if(!src)src="none";size_t n=std::strlen(src);if(n>=cap)n=cap-1;std::memcpy(dst,src,n);dst[n]=0;
}

struct State {
    // ---- RT evidence
    struct PsoTrack {uint64_t id=0,lastKey=0;unsigned emitted=0;};
    PsoTrack track[rt_psos]{};RtFacts rtq[rt_queue]{};unsigned rtHead=0,rtTail=0;
    uint64_t rtRecords=0,rtSuppressed=0,rtTableFull=0,rtLost=0,rtSeen=0;
    // True when this (PSO, OM epoch) pair is new and within the caps; the pair is then marked emitted. Cheap (relaxed scan of 16).
    bool rt_wanted(uint64_t psoId,uint64_t key) noexcept {
        ++rtSeen;
        PsoTrack* t=nullptr;
        for(auto& x:track)if(x.id==psoId&&x.id){t=&x;break;}
        if(!t) {
            for(auto& x:track)if(!x.id){t=&x;break;}
            if(!t){++rtTableFull;return false;}
            t->id=psoId;t->lastKey=0;t->emitted=0;
        }
        if(t->emitted&&t->lastKey==key)return false;
        if(t->emitted>=rt_changes_per_pso||rtRecords>=rt_record_cap){++rtSuppressed;t->lastKey=key;return false;}
        t->lastKey=key;++t->emitted;++rtRecords;return true;
    }
    void rt_push(const RtFacts& f) noexcept {
        if(rtHead-rtTail>=rt_queue){++rtLost;return;}
        rtq[rtHead++%rt_queue]=f;
    }
    bool rt_pop(RtFacts& f) noexcept {if(rtTail==rtHead)return false;f=rtq[rtTail++%rt_queue];return true;}

    // ---- all-draw ordinals per (list, generation) recording
    struct Ord {uint64_t list=0,gen=0,n=0,touch=0;};
    Ord ord[ordinal_table]{};uint64_t ordTouch=0;
    uint32_t draw_ordinal(uint64_t list,uint64_t gen) noexcept {
        Ord* pick=nullptr;
        for(auto& o:ord)if(o.n&&o.list==list&&o.gen==gen){pick=&o;break;}
        if(!pick) {
            for(auto& o:ord)if(!o.n){pick=&o;break;}
            if(!pick){pick=&ord[0];for(auto& o:ord)if(o.touch<pick->touch)pick=&o;}
            *pick=Ord{};pick->list=list;pick->gen=gen;
        }
        pick->touch=++ordTouch;return uint32_t(++pick->n);
    }
    // A Reset list starts a new recording: its ordinals are dropped.
    void discard(uint64_t list) noexcept {for(auto& o:ord)if(o.n&&o.list==list)o=Ord{};}

    // ---- record snapshots: fixed slots of the CURRENT window
    Srt slot[rec_per_window]{};
    uint64_t winList=0,winGen=0,winId=0,winOpened=0,lastWindowMs=0;unsigned winRecs=0;bool winOpen=false;
    uint64_t srtStored=0,dropWindowFull=0,dropWindowCap=0,dropSpacing=0,dropBusy=0,srtDone=0;
    enum Verdict {accept_record,drop_full,drop_cap,drop_spacing,drop_busy};
    // Would a record of this recording be stored now? (cheap pre-check before the caller collects the metadata; nothing is counted)
    bool srt_wanted(uint64_t list,uint64_t gen,uint64_t now) const noexcept {
        if(winOpen&&winList==list&&winGen==gen)return winRecs<rec_per_window;
        if(winOpened>=window_cap)return false;
        if(winOpened&&now-lastWindowMs<window_gap_ms)return false;
        for(const auto& s:slot)if(s.used)return false;
        return true;
    }
    // Store one record (counts every drop). `bytes` may be null (read failed / never read).
    Verdict srt_offer(const SrtMeta& m,const uint8_t* bytes,uint64_t now) noexcept {
        const bool same=winOpen&&winList==list_of(m)&&winGen==m.generation;
        if(same) {
            if(winRecs>=rec_per_window){++dropWindowFull;return drop_full;}
        } else {
            if(winOpened>=window_cap){++dropWindowCap;return drop_cap;}
            if(winOpened&&now-lastWindowMs<window_gap_ms){++dropSpacing;return drop_spacing;}
            for(const auto& s:slot)if(s.used){++dropBusy;return drop_busy;}
            winOpen=true;winList=list_of(m);winGen=m.generation;++winOpened;winId=winOpened;winRecs=0;lastWindowMs=now;
        }
        Srt& s=slot[winRecs];s=Srt{};s.used=true;s.born=now;s.window=winId;s.rec=winRecs;s.m=m;
        if(bytes&&m.hasBytes)std::memcpy(s.bytes,bytes,416);else s.m.hasBytes=false;
        ++winRecs;++srtStored;return accept_record;
    }
    static uint64_t list_of(const SrtMeta& m) noexcept {return m.list;}
    // The recording was executed (observer): its pending records can be formatted and the Execute occurrence is known.
    void note_execute(uint64_t list,uint64_t gen,uint64_t execSeq) noexcept {
        for(auto& s:slot)if(s.used&&!s.ready&&s.m.list==list&&s.m.generation==gen){s.execSeq=execSeq;s.ready=true;}
    }
    // Worker: take one record that is ready (executed, or old enough that the draw is certainly over).
    bool srt_pop(Srt& out,uint64_t now) noexcept {
        for(auto& s:slot) {
            if(!s.used)continue;
            if(s.ready||now-s.born>=slot_age_ms) {out=s;s.used=false;++srtDone;
                bool any=false;for(const auto& x:slot)any|=x.used;if(!any)winOpen=false;return true;}
        }
        return false;
    }

    // ---- decision summary (every ACM acceptance decision, accepted or refused)
    struct Reason {char name[text_cap]{};uint64_t n=0,member=0,early=0;};
    Reason reasons[reason_cap]{};unsigned reasonCount=0;uint64_t reasonOverflow=0,decisions=0;
    void note_decision(const char* why,bool early) noexcept {
        if(!why)why="accepted";
        ++decisions;
        for(unsigned i=0;i<reasonCount;++i)
            if(std::strncmp(reasons[i].name,why,text_cap-1)==0){++reasons[i].n;if(early)++reasons[i].early;else ++reasons[i].member;return;}
        if(reasonCount>=reason_cap){++reasonOverflow;return;}
        Reason& r=reasons[reasonCount++];copy_text(r.name,sizeof r.name,why);r.n=1;if(early)r.early=1;else r.member=1;
    }
    struct Totals {
        uint64_t rtSeen=0,rtRecords=0,rtSuppressed=0,rtTableFull=0,rtLost=0,srtStored=0,srtDone=0,dropWindowFull=0,dropWindowCap=0,dropSpacing=0,dropBusy=0,
            windows=0,decisions=0,reasonOverflow=0;unsigned reasons=0;
    };
    Totals totals() const noexcept {
        Totals t;t.rtSeen=rtSeen;t.rtRecords=rtRecords;t.rtSuppressed=rtSuppressed;t.rtTableFull=rtTableFull;t.rtLost=rtLost;t.srtStored=srtStored;t.srtDone=srtDone;
        t.dropWindowFull=dropWindowFull;t.dropWindowCap=dropWindowCap;t.dropSpacing=dropSpacing;t.dropBusy=dropBusy;t.windows=winOpened;t.decisions=decisions;
        t.reasonOverflow=reasonOverflow;t.reasons=reasonCount;return t;
    }
};

// ------------------------------------------------------------- worker: formatting
using U=unsigned long long;
inline void finish(char (&a)[line_bytes],int n) noexcept {
    static constexpr char mark[]=";truncated=1";
    if(n<0||n>=int(sizeof a))std::memcpy(a+line_bytes-sizeof mark,mark,sizeof mark);
}
inline const char* build_id() noexcept {return __DATE__ " " __TIME__;}
// Three lines per RT record: the PSO's saved output facts (a), the original draw's OM binding (b) and its slot 0 / slot 1 provenance (c).
inline void format_rt(const RtFacts& f,unsigned n,char (&a)[line_bytes],char (&b)[line_bytes],char (&c)[line_bytes]) noexcept {
    char rtv[96]{};size_t at=0;
    for(unsigned i=0;i<8&&at+8<sizeof rtv;++i)at+=size_t(std::snprintf(rtv+at,sizeof rtv-at,"%s%u",i?",":"",f.rtv[i]));
    const bool slot1Active=f.psoKnown&&f.rtCount>=2&&f.rtv[1]!=0;
    char s1[64];
    if(slot1Active)std::snprintf(s1,sizeof s1,"slot1-blend=%u;slot1-write-mask=0x%x",unsigned(f.blend1),f.mask1);
    else std::snprintf(s1,sizeof s1,"slot1-blend=not-active;slot1-write-mask=not-active");
    finish(a,std::snprintf(a,sizeof a,"world-evidence;event=acm-rt;n=%u;build=%s;label=pso-creation-facts;list=%llx;gen=%llx;ordinal=%u;pso=%llx;pso-id=%llu;vs=%s;ps=%s;"
        "pso-known=%u;pso-rt-count=%u;pso-rtv=%s;pso-blend0=%u;pso-write-mask0=0x%x;independent-blend=%u;slot1-active=%u;%s",n,build_id(),U(f.list),U(f.generation),
        f.ordinal,U(f.pso),U(f.psoId),f.vs[0]?f.vs:"none",f.ps[0]?f.ps:"none",unsigned(f.psoKnown),f.rtCount,rtv,unsigned(f.blend0),f.mask0,unsigned(f.independent),
        unsigned(slot1Active),s1));
    char h[160]{};at=0;
    const unsigned shown=f.omCount<8?f.omCount:8;
    for(unsigned i=0;i<shown&&at+20<sizeof h;++i)at+=size_t(std::snprintf(h+at,sizeof h-at,"%s%llx",i?",":"",U(f.handles[i])));
    char t0[200],t1[200];fmt_slot(t0,sizeof t0,f.slot0);fmt_slot(t1,sizeof t1,f.slot1);
    finish(b,std::snprintf(b,sizeof b,"world-evidence;event=acm-rt-om;n=%u;label=original-game-binding;om-set=%u;om-count=%u;form=%s;handles=%s;bind-copy-epoch=%llu;"
        "current-copy-epoch=%llu;om-proof=%u;om-key=%016llx;injected-eye=%s",n,unsigned(f.omSet),f.omCount,
        !f.omSet?"none":f.omSingle?"contiguous-range(only-the-base-handle-is-known,slot1-unresolved)":"handle-array",h,U(f.bindEpoch),U(f.curEpoch),unsigned(f.omProof),
        U(f.key),f.psoKnown&&f.rtCount==1?"OMSetRenderTargets(count=1,private-eye-target,slot1=none)":"none(draw-refused:rt-count!=1,nothing-replayed)"));
    finish(c,std::snprintf(c,sizeof c,"world-evidence;event=acm-rt-slots;n=%u;label=original-game-binding;slot0=%s;slot1=%s",n,t0,t1));
}
inline const char* reason_text(unsigned r) noexcept {return r<11?acz::diagnostics::RootConstantWitness::reason_name(r):"?";}
// Record snapshot -> up to 6 lines: 2 metadata lines + 4 hex lines (128 bytes each). Returns the line count.
constexpr unsigned srt_lines=6;
inline unsigned format_srt(const Srt& s,char (&lines)[srt_lines][line_bytes]) noexcept {
    const auto& m=s.m;
    for(auto& l:lines)l[0]=0;
    const uint64_t hash=m.hasBytes?fnv64(s.bytes,416):0;
    char exec[24];if(s.execSeq)std::snprintf(exec,sizeof exec,"%llu",U(s.execSeq));else std::snprintf(exec,sizeof exec,"unknown");
    char off[40];if(m.t10OffsetKnown)std::snprintf(off,sizeof off,"%llx",U(m.t10Offset));else std::snprintf(off,sizeof off,"unknown");
    finish(lines[0],std::snprintf(lines[0],sizeof lines[0],"world-evidence;event=acm-record;win=%llu;rec=%u/%u;build=%s;path=%s;list=%llx;recording-gen=%llx;all-draw-ordinal=%u;"
        "exec-seq=%s;pso=%llx;pso-id=%llu;vs=%s;ps=%s;t10-param=%d;t10-vis=%d;t10-va=%llx;t10-resource=%llx;t10-resource-serial=%llu;t10-offset=%s;read=%s;"
        "bytes=%s;fnv1a64=%016llx",U(s.window),s.rec+1,rec_per_window,build_id(),m.path,U(m.list),U(m.generation),m.ordinal,exec,U(m.pso),U(m.psoId),
        m.vs[0]?m.vs:"none",m.ps[0]?m.ps:"none",m.t10Param==UINT32_MAX?-1:int(m.t10Param),m.t10Vis==UINT32_MAX?-1:int(m.t10Vis),U(m.t10Va),U(m.t10Address),
        U(m.t10ResSerial),off,m.read,m.hasBytes?"416(copy-the-decision-used,not-reread)":"none",U(hash)));
    const auto& c=m.cb10;
    char words[96]{};size_t at=0;
    for(unsigned i=0;i<c.size&&i<4&&at+12<sizeof words;++i) {
        if(c.mask&(1u<<i))at+=size_t(std::snprintf(words+at,sizeof words-at,"%s%08x",i?",":"",c.words[i]));
        else at+=size_t(std::snprintf(words+at,sizeof words-at,"%s????????",i?",":""));
    }
    char cb[160];
    if(!c.resolved)std::snprintf(cb,sizeof cb,"cb10=not-resolved(no-cb10-check-ran-for-this-draw)");
    else if(c.known)std::snprintf(cb,sizeof cb,"cb10=known;cb10-dword0=%08x;flags-bit8-fade=%u",c.value,(c.value>>3)&1u);
    else std::snprintf(cb,sizeof cb,"cb10=UNKNOWN;cb10-dword0=unknown;witness-reason=%s;note=%s",reason_text(c.reason),c.note&&c.note[0]?c.note:"none");
    finish(lines[1],std::snprintf(lines[1],sizeof lines[1],"world-evidence;event=acm-record-decision;win=%llu;rec=%u;label=original-game-record;substituted=%s;decision=%s;"
        "reason=%s;%s;cb10-param=%d;cb10-stages=%u;cb10-mask=%x;cb10-words=%s;cb10-root=%llx;cb10-token=%llu;cb10-revision=%llu",U(s.window),s.rec+1,
        m.observed?"none(observer-only)":m.accepted?"eye0+eye1-formed-from-this-record(not-captured)":"none(draw-refused)",
        m.observed?"not-evaluated":m.accepted?"accepted":"refused",m.accepted||m.observed?"none":m.decision,cb,
        c.parameter==UINT32_MAX?-1:int(c.parameter),c.stages,c.mask,words[0]?words:"none",U(c.root),U(c.token),U(c.revision)));
    unsigned n=2;
    if(m.hasBytes)for(unsigned part=0;part<4;++part) {
        const unsigned from=part*128,to=from+128<416?from+128:416;
        char hex[2*128+1];size_t k=0;
        for(unsigned i=from;i<to;++i){std::snprintf(hex+k,sizeof hex-k,"%02x",s.bytes[i]);k+=2;}
        hex[k]=0;
        finish(lines[n],std::snprintf(lines[n],sizeof lines[n],"world-evidence;event=acm-record-hex;win=%llu;rec=%u;part=%u/4;bytes=%u-%u;hex=%s",U(s.window),s.rec+1,part+1,from,to-1,hex));
        ++n;
    }
    return n;
}
// Decision summary lines (<= 6 reasons per line). Returns the number of lines written to `lines` (max 4).
constexpr unsigned summary_lines=4;
inline unsigned format_summary(const State& st,char (&lines)[summary_lines][line_bytes]) noexcept {
    unsigned n=0;
    for(unsigned from=0;from<st.reasonCount&&n<summary_lines;from+=6) {
        char body[400]{};size_t at=0;
        for(unsigned i=from;i<from+6&&i<st.reasonCount;++i) {
            const auto& r=st.reasons[i];
            at+=size_t(std::snprintf(body+at,sizeof body-at,"%s%s:%llu(member:%llu,early:%llu)",i>from?",":"",r.name,U(r.n),U(r.member),U(r.early)));
            if(at>=sizeof body)break;
        }
        finish(lines[n],std::snprintf(lines[n],sizeof lines[n],"world-evidence;event=decision-summary;part=%u;decisions=%llu;distinct-reasons=%u;overflow=%llu;%s",
            n+1,U(st.decisions),st.reasonCount,U(st.reasonOverflow),body));
        ++n;
    }
    return n;
}
}  // namespace acz::gpu_copy::wev
