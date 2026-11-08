#pragma once
// J-world-objects v2, plan step 1 (gpu_copy_step5_world_observe, default OFF): a read-only, bounded observer of
// the 3D-path world-object draws (DrawACM_v) wherever they are recorded, to decide from live data whether they sit
// in the cockpit command list or in carrier lists recorded/executed before it (src/acz-vr/J-world-objects-v2-design.md,
// sections 3 and 8). Modelled on the J-cockpit-fov F2 pre-cockpit observer:
//  * the Draw seam only updates a fixed-size POD tally per (list route, generation) and copies at most 3 SrtData
//    records per list from the game's CPU-mapped UPLOAD memory (NO GPU readback, no command recorded, no state of
//    the step-3/step-5 machinery written, nothing allocated, no I/O);
//  * the Execute seam turns the tally of each executed list into POD records (execute order, batch index, queue);
//    a cockpit list (one with DrawCPT draws) closes the "window": the lists executed since the previous cockpit
//    list, summarised in one `frame` record;
//  * the worker formats the records into the dedicated step5-producer.log.
// Sampling: the first 3 windows fully, then one window per 5 s status period (`due`); per sampled window at most
// 12 list lines, 8 ACM draw lines, 3 decoded SrtData lines and 1 summary (+ one truncation marker); a 128-record
// queue (overflow counted as lost). Lifetime totals are always counted (status line). The caller serialises State
// with a leaf mutex (never held across another lock, formatting, feature_log or native calls).
#include "gpu_copy_step5.h"
#include "gpu_copy_step5_world.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::gpu_copy::wobs {
constexpr unsigned list_cap=48,detail_cap=8,srt_cap=3,queue_records=128,sampled_full=3;
constexpr unsigned list_lines=12,draw_lines=8,srt_lines=3,summary_lists=6;
constexpr unsigned line_bytes=480; // feature_log::line_cap
enum Kind : uint8_t {kind_acm=0,kind_cpt=1,kind_other=2};
// Root-signature diagnostics (t10-va=0 hunt): per distinct root signature seen with an ACM PSO one layout record, and for the first draws
// of each such signature the raw root-parameter state. Bounded: root_cap signatures, state_per_root + state_total_cap state records.
constexpr unsigned root_cap=16,root_param_cap=16,state_per_root=8,state_total_cap=40,state_param_cap=8;
struct ParamFact {
    uint8_t kind,vis;uint32_t reg,space,count; // count: constants (32-bit values) or table ranges
    uint8_t r0kind;uint32_t r0base,r0count,r0space; // first table range (kind: 0 srv 1 uav 2 cbv 3 sampler)
};
struct RootFact { // what the layout of a root signature is and how the t10 rule judged it
    uint64_t key,identity;bool layoutPresent,layoutValid;char rejection[40];
    uint32_t flags,staticSamplers,paramCount;ParamFact p[root_param_cap];
    uint32_t t10Parameter,t10Offset;bool t10Table;unsigned cockpitMatches,worldMatches,matches;bool worldKeyOn,pending;
};
struct StateParam {uint32_t index;uint8_t kind;uint64_t value;uint32_t words;}; // kind: ParameterKind numeric (0 table 1 constants 2 cbv 3 srv 4 uav)
struct RootState { // the raw root-parameter state of one ACM draw (parameters on which a SetGraphicsRoot* call was observed)
    uint64_t list,generation,rootKey,pso,t10Va,t10Address;uint32_t ordinal,t10Parameter,setCount;bool t10Table,layoutPresent;
    StateParam set[state_param_cap];
};
// Filled by the Draw seam (hook-state mutex held) and queued afterwards; arrays left uninitialised on purpose (trivial to construct).
struct RootDiagOut {bool root=false,state=false;RootFact rf;RootState rs;};

// What a draw's render target / depth-stencil are, as the existing view tracking knows them.
struct Target {
    uint64_t rtv=0,rt=0,dsv=0,ds=0,rtWidth=0;uint32_t rtHeight=0,rtFormat=0,dsFormat=0,omCount=0;
    float vpW=0,vpH=0;bool rtKnown=false,dsKnown=false,sceneRt=false;
    bool same_targets(const Target& o) const noexcept {return rtv==o.rtv&&rt==o.rt&&dsv==o.dsv&&ds==o.ds;}
};
// One observed draw (everything the Draw seam learned under its locks).
struct Obs {
    Kind kind=kind_other;uint64_t list=0,generation=0,pso=0,id=0,t10Va=0,t10Address=0;uint32_t l3Ordinal=0,vs8=0;
    Target target{};bool frameActive=false,inFrameList=false;uint64_t frameList=0,frameGeneration=0;
};
struct DrawFacts {
    uint32_t ordinal=0,l3Ordinal=0,acmIndex=0;uint64_t pso=0,id=0,t10Va=0;Target target{};bool frameActive=false,inFrameList=false;
    uint64_t frameList=0,frameGeneration=0;
};
struct SrtCopy {const char* status="not-read";std::array<uint8_t,416> bytes{};};
// The counts of one list recording (what a `list` record carries). The "after" counts are sinceCpt / sinceOther:
// the non-ACM draws recorded since the LAST ACM draw.
struct ListSummary {
    uint64_t list=0,generation=0;
    uint32_t draws=0,acm=0,cpt=0,other=0,firstAcm=0,lastAcm=0,firstCpt=0,lastCpt=0;
    uint32_t cptBefore=0,cptBetween=0,sinceCpt=0,otherBefore=0,otherBetween=0,sinceOther=0;
    uint64_t otherPsoBefore=0,otherPsoAfter=0;uint32_t otherVsBefore=0,otherVsAfter=0;
    uint32_t rtChanges=0,dsChanges=0,sceneAcm=0,activeAcm=0,otherListAcm=0,inFrameAcm=0,distinctPso=0;uint64_t psoSeen[4]{};
    Target firstTarget{};
};
// Per (list route, generation) tally of the recording; consumed at the list's Execute.
struct Tally {
    bool used=false;uint64_t touch=0;ListSummary s{};
    unsigned detailCount=0;DrawFacts detail[detail_cap]{};SrtCopy srt[srt_cap]{};
};
enum RecordType : uint8_t {rec_draw=0,rec_srt=1,rec_list=2,rec_frame=3,rec_marker=4,rec_root=5,rec_rootstate=6};
struct ListRef {uint64_t list=0,generation=0;uint32_t acm=0,cpt=0,other=0,batch=0;uint64_t execSeq=0,queue=0;};
struct Record {
    RecordType type=rec_marker;uint64_t window=0,list=0,generation=0,execSeq=0,queue=0;
    DrawFacts draw{};SrtCopy srt{};uint32_t acmIndex=0;                      // draw / srt
    ListSummary sum{};uint32_t batch=0,batchCount=0;bool producerList=false;const char* role="other"; // list
    uint64_t prevCockpitSeq=0;                                              // frame summary
    uint32_t listsSince=0,acmLists=0,carrierLists=0,acmTotal=0,acmInCockpit=0,sameQueue=0,droppedLists=0;
    ListRef lists[summary_lists]{};unsigned listCount=0;
    RootFact rf{};RootState rs{};unsigned rsIndex=0;                         // root / rootstate (diagnostics)
};
struct State {
    Tally tab[list_cap]{};uint64_t touch=0,execSeq=0;
    // Lifetime totals (always counted, sampled or not).
    uint64_t acmDraws=0,cptDraws=0,otherDraws=0,acmLists=0,cockpitLists=0,executed=0,windows=0,windowsWithCarriers=0,carrierLists=0,
        acmActive=0,acmActiveOtherList=0,acmActiveSameList=0,acmNoFrame=0,evicted=0,discarded=0,untracked=0,noList=0,
        sampledWindows=0,records=0,lost=0,truncated=0;
    // The open window (lists executed since the previous cockpit list).
    bool open=false,sampling=false,due=false,cut=false;
    uint64_t win=0,prevCockpitSeq=0;unsigned wList=0,wDraw=0,wSrt=0;
    uint32_t wLists=0,wAcmLists=0,wAcmTotal=0,wDropped=0;ListRef wRefs[summary_lists]{};
    std::array<Record,queue_records> queue{};uint64_t head=0,tail=0;
    // Lifetime totals for the status line (copied under the caller's leaf mutex).
    struct Totals {
        uint64_t acmDraws=0,cptDraws=0,otherDraws=0,acmLists=0,cockpitLists=0,carrierLists=0,executed=0,windows=0,windowsWithCarriers=0,acmActive=0,
            acmActiveSameList=0,acmActiveOtherList=0,acmNoFrame=0,evicted=0,discarded=0,untracked=0,noList=0,sampledWindows=0,records=0,lost=0,truncated=0;
    };
    Totals totals() const noexcept {
        Totals t;t.acmDraws=acmDraws;t.cptDraws=cptDraws;t.otherDraws=otherDraws;t.acmLists=acmLists;t.cockpitLists=cockpitLists;t.carrierLists=carrierLists;
        t.executed=executed;t.windows=windows;t.windowsWithCarriers=windowsWithCarriers;t.acmActive=acmActive;t.acmActiveSameList=acmActiveSameList;
        t.acmActiveOtherList=acmActiveOtherList;t.acmNoFrame=acmNoFrame;t.evicted=evicted;t.discarded=discarded;t.untracked=untracked;t.noList=noList;
        t.sampledWindows=sampledWindows;t.records=records;t.lost=lost;t.truncated=truncated;return t;
    }
    // ---- queue
    void push(const Record& r) noexcept {
        if(head-tail==queue_records){++lost;return;}
        queue[head++%queue_records]=r;++records;
    }
    bool pop(Record& r) noexcept {if(tail==head)return false;r=queue[tail++%queue_records];return true;}
    // Root-signature diagnostics (independent of the sampling windows; bounded by the callers' atomics).
    uint64_t rootRecords=0,rootStateRecords=0;
    void push_root(const RootDiagOut& d) noexcept {
        // These records are not counted in `records` (the window/list record totals stay what they were without the diagnostics).
        if(d.root){Record r;r.type=rec_root;r.rf=d.rf;const auto before=records;push(r);if(records>before)records=before;++rootRecords;}
        if(d.state){Record r;r.type=rec_rootstate;r.rs=d.rs;r.rsIndex=unsigned(rootStateRecords++);const auto before=records;push(r);if(records>before)records=before;}
    }
    // ---- Draw seam
    Tally* find(uint64_t list,uint64_t gen) noexcept {
        for(auto& t:tab)if(t.used&&t.s.list==list&&t.s.generation==gen)return &t;
        return nullptr;
    }
    Tally& open_tally(uint64_t list,uint64_t gen) noexcept {
        if(auto* t=find(list,gen)){t->touch=++touch;return *t;}
        Tally* pick=nullptr;
        for(auto& t:tab)if(!t.used){pick=&t;break;}
        if(!pick){pick=&tab[0];for(auto& t:tab)if(t.touch<pick->touch)pick=&t;++evicted;}
        *pick=Tally{};pick->used=true;pick->s.list=list;pick->s.generation=gen;pick->touch=++touch;return *pick;
    }
    // Record one draw. Returns true when the caller should copy this ACM draw's SrtData (its index is in acmIndex).
    bool draw(const Obs& o,unsigned& acmIndex) noexcept {
        Tally& t=open_tally(o.list,o.generation);auto& s=t.s;const uint32_t pos=++s.draws;acmIndex=0; // pos: position among ALL draws of the recording (l3Ordinal counts only DrawCPT draws)
        if(o.kind==kind_cpt) {
            ++cptDraws;++s.cpt;if(!s.firstCpt)s.firstCpt=pos;s.lastCpt=pos;
            if(!s.acm)++s.cptBefore;else ++s.sinceCpt;
            return false;
        }
        if(o.kind==kind_other) {
            ++otherDraws;++s.other;
            if(!s.acm){++s.otherBefore;s.otherPsoBefore=o.id;s.otherVsBefore=o.vs8;}
            else{++s.sinceOther;if(s.sinceOther==1){s.otherPsoAfter=o.id;s.otherVsAfter=o.vs8;}}
            return false;
        }
        ++acmDraws;
        if(o.frameActive) {
            ++acmActive;++s.activeAcm;
            if(o.inFrameList){++acmActiveSameList;++s.inFrameAcm;}else{++acmActiveOtherList;++s.otherListAcm;}
        } else ++acmNoFrame;
        if(s.acm) {
            s.cptBetween+=s.sinceCpt;s.otherBetween+=s.sinceOther;
            if(o.target.rtv!=s.firstTarget.rtv||o.target.rt!=s.firstTarget.rt)++s.rtChanges;
            if(o.target.dsv!=s.firstTarget.dsv||o.target.ds!=s.firstTarget.ds)++s.dsChanges;
        } else {s.firstTarget=o.target;s.firstAcm=pos;}
        s.sinceCpt=s.sinceOther=0;s.otherPsoAfter=0;s.otherVsAfter=0;s.lastAcm=pos;
        if(o.target.sceneRt)++s.sceneAcm;
        {bool seen=false;const unsigned n=s.distinctPso<4?s.distinctPso:4;for(unsigned i=0;i<n;++i)seen|=s.psoSeen[i]==o.pso;
         if(!seen){if(s.distinctPso<4)s.psoSeen[s.distinctPso]=o.pso;++s.distinctPso;}}
        if(t.detailCount<detail_cap) {
            auto& d=t.detail[t.detailCount++];d.ordinal=pos;d.l3Ordinal=o.l3Ordinal;d.acmIndex=s.acm;d.pso=o.pso;d.id=o.id;d.t10Va=o.t10Va;d.target=o.target;
            d.frameActive=o.frameActive;d.inFrameList=o.inFrameList;d.frameList=o.frameList;d.frameGeneration=o.frameGeneration;
        }
        acmIndex=s.acm;++s.acm;
        return acmIndex<srt_cap;
    }
    void store_srt(uint64_t list,uint64_t gen,unsigned index,const char* status,const uint8_t* bytes) noexcept {
        auto* t=find(list,gen);if(!t||index>=srt_cap)return;
        t->srt[index].status=status;if(bytes)std::memcpy(t->srt[index].bytes.data(),bytes,416);
    }
    // A list was Reset: its unexecuted recordings are discarded (counted).
    void discard(uint64_t list) noexcept {
        for(auto& t:tab)if(t.used&&t.s.list==list){t.used=false;++discarded;}
    }
    // ---- Execute seam: once per ExecuteCommandLists call, then once per list of the batch.
    void begin_call() noexcept {++execSeq;}
    void begin_window() noexcept {
        open=true;cut=false;++windows;win=windows;wList=wDraw=wSrt=0;wLists=wAcmLists=wAcmTotal=wDropped=0;
        sampling=sampledWindows<sampled_full||due;
        if(sampling){++sampledWindows;due=false;}
    }
    // `producer`: this list recording is the producer's active frame.
    void execute(uint64_t list,uint64_t gen,uint64_t queueRoute,unsigned batch,unsigned batchCount,bool producer) noexcept {
        Tally* t=find(list,gen);
        if(!t){++untracked;return;}
        ++executed;
        const auto& s=t->s;const bool acm=s.acm>0,cockpit=s.cpt>0;
        if(!acm&&!cockpit){t->used=false;return;}
        if(!open)begin_window();
        if(acm){++acmLists;++wAcmLists;wAcmTotal+=s.acm;if(!cockpit)++carrierLists;}
        if(cockpit)++cockpitLists;
        if(wLists<summary_lists) {
            auto& r=wRefs[wLists];r.list=list;r.generation=gen;r.acm=s.acm;r.cpt=s.cpt;r.other=s.other;r.batch=batch;r.execSeq=execSeq;r.queue=queueRoute;
        } else ++wDropped;
        ++wLists;
        if(sampling) {
            if(wList<list_lines) {
                ++wList;Record r;r.type=rec_list;r.window=win;r.sum=s;r.list=list;r.generation=gen;r.execSeq=execSeq;r.queue=queueRoute;r.batch=batch;
                r.batchCount=batchCount;r.producerList=producer;r.role=cockpit?"cockpit":"carrier";
                push(r);
            } else if(!cut){cut=true;++truncated;Record m;m.type=rec_marker;m.window=win;push(m);}
            if(acm) for(unsigned i=0;i<t->detailCount&&wDraw<draw_lines;++i,++wDraw) {
                Record r;r.type=rec_draw;r.window=win;r.list=list;r.generation=gen;r.draw=t->detail[i];r.execSeq=execSeq;push(r);
            }
            if(acm) for(unsigned i=0;i<srt_cap&&i<s.acm&&wSrt<srt_lines;++i,++wSrt) {
                Record r;r.type=rec_srt;r.window=win;r.list=list;r.generation=gen;r.acmIndex=i;r.srt=t->srt[i];r.execSeq=execSeq;push(r);
            }
        }
        if(cockpit) { // the cockpit list closes the window
            const unsigned carriers=wAcmLists-(acm?1u:0u);
            if(carriers)++windowsWithCarriers;
            if(sampling) {
                Record r;r.type=rec_frame;r.window=win;r.list=list;r.generation=gen;r.execSeq=execSeq;r.queue=queueRoute;r.producerList=producer;
                r.prevCockpitSeq=prevCockpitSeq;r.listsSince=wLists;r.acmLists=wAcmLists;r.carrierLists=carriers;r.acmTotal=wAcmTotal;r.acmInCockpit=s.acm;
                r.droppedLists=wDropped;r.listCount=wLists<summary_lists?wLists:summary_lists;
                for(unsigned i=0;i<r.listCount;++i){r.lists[i]=wRefs[i];if(wRefs[i].queue==queueRoute)++r.sameQueue;}
                push(r);
            }
            prevCockpitSeq=execSeq;open=false;sampling=false;
        }
        t->used=false;
    }
};

// ------------------------------------------------------------- worker: decoding and formatting
// The ACM record's camera facts, from its own bytes (+64 C is the world view-projection; the camera is its
// decomposition in ABSOLUTE world units; +256 B is the model, a pure translation in the evidence).
struct Derived {
    const char* record="srt-not-read";const char* camera="srt-not-read";bool ok=false;
    double eye[3]{},right[3]{},up[3]{},fwd[3]{},tan[2]{},model[3]{};uint32_t ac=0;
};
inline uint32_t fnv32(const uint8_t* p,size_t n) noexcept {uint32_t h=2166136261u;for(size_t i=0;i<n;++i){h^=p[i];h*=16777619u;}return h;}
inline void derive(const SrtCopy& s,Derived& d,step5::Scratch& sc) noexcept {
    d=Derived{};
    if(std::strcmp(s.status,"read")!=0){d.record=s.status;d.camera=s.status;return;}
    const uint8_t* raw=s.bytes.data();
    d.ac=fnv32(raw,128); // A|C: equal across the draws of a frame in the evidence
    d.record=step5::world::record_check(raw);if(!d.record)d.record="ok";
    step5::world::Frame wf;
    if(const char* why=step5::world::camera_setup(raw,nullptr,wf,sc)){d.camera=why;return;}
    const auto& c=wf.pf.cam;d.ok=true;d.camera="recovered";
    for(int k=0;k<3;++k){d.eye[k]=c.eye[k];d.right[k]=c.right[k];d.up[k]=c.up[k];d.fwd[k]=c.fwd[k];d.model[k]=wf.model[k];}
    d.tan[0]=c.tanX;d.tan[1]=c.tanY;
}
using U=unsigned long long;
inline void finish(char (&a)[line_bytes],int n) noexcept {
    static constexpr char mark[]=";truncated=1";
    if(n<0||n>=int(sizeof a))std::memcpy(a+line_bytes-sizeof mark,mark,sizeof mark);
}
inline void fmt_target(char* out,size_t cap,const Target& t) noexcept {
    std::snprintf(out,cap,"rtv=%llx/res:%llx/fmt:%u/%llux%u,dsv=%llx/res:%llx/fmt:%u,om=%u,vp=%gx%g,scene-rt=%u,known=%u%u",U(t.rtv),U(t.rt),t.rtFormat,
        U(t.rtWidth),t.rtHeight,U(t.dsv),U(t.ds),t.dsFormat,t.omCount,double(t.vpW),double(t.vpH),unsigned(t.sceneRt),unsigned(t.rtKnown),unsigned(t.dsKnown));
}
inline const char* param_kind_name(unsigned k) noexcept {static constexpr const char* n[]={"table","constants","cbv","srv","uav"};return k<5?n[k]:"?";}
inline const char* vis_name(unsigned v) noexcept {static constexpr const char* n[]={"all","vs","hs","ds","gs","ps"};return v<6?n[v]:"?";}
inline char range_letter(unsigned k) noexcept {static constexpr char n[]={'t','u','b','s'};return k<4?n[k]:'?';}
// One parameter as a short token: `2=srv:vs:t10s0`, `0=table:ps:1r(t1x1s0)`, `3=constants:all:b10s0x2`.
inline int fmt_param(char* out,size_t cap,unsigned index,const ParamFact& p) noexcept {
    if(p.kind==0)return std::snprintf(out,cap,"%u=table:%s:%ur(%c%ux%us%u)",index,vis_name(p.vis),p.count,range_letter(p.r0kind),p.r0base,p.r0count,p.r0space);
    if(p.kind==1)return std::snprintf(out,cap,"%u=constants:%s:b%us%ux%u",index,vis_name(p.vis),p.reg,p.space,p.count);
    return std::snprintf(out,cap,"%u=%s:%s:%c%us%u",index,param_kind_name(p.kind),vis_name(p.vis),p.kind==3?'t':p.kind==4?'u':'b',p.reg,p.space);
}
// Formats one record into up to two lines (b empty when unused). `d` is the decoded SrtData (rec_srt only).
inline void format(const Record& r,const Derived& d,char (&a)[line_bytes],char (&b)[line_bytes]) noexcept {
    a[0]=0;b[0]=0;
    switch(r.type) {
    case rec_marker:
        std::snprintf(a,sizeof a,"world-observe;event=truncated;win=%llu;cap=%u-list-lines,%u-draw-lines,%u-srt-lines-per-window",U(r.window),list_lines,draw_lines,srt_lines);
        return;
    case rec_draw: {
        const auto& x=r.draw;char t[220];fmt_target(t,sizeof t,x.target);
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=acm-draw;win=%llu;list=%llx;gen=%llx;exec-seq=%llu;ordinal=%u;l3-ordinal=%u;acm-index=%u;pso=%llx;id=%llu;"
            "frame-active=%u;in-frame-list=%u;frame-list=%llx;frame-gen=%llx;t10-va=%llx;%s",U(r.window),U(r.list),U(r.generation),U(r.execSeq),x.ordinal,
            x.l3Ordinal,x.acmIndex,U(x.pso),U(x.id),unsigned(x.frameActive),unsigned(x.inFrameList),U(x.frameList),U(x.frameGeneration),U(x.t10Va),t));
        return;
    }
    case rec_srt:
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=acm-srt;win=%llu;list=%llx;gen=%llx;acm-index=%u;srt=%s;record=%s;camera=%s;eye=%.3f,%.3f,%.3f;"
            "right=%.4f,%.4f,%.4f;up=%.4f,%.4f,%.4f;fwd=%.4f,%.4f,%.4f;lens-tan=%.5f,%.5f;model=%.3f,%.3f,%.3f;ac-fnv=%08x",U(r.window),U(r.list),U(r.generation),
            r.acmIndex,r.srt.status,d.record,d.camera,d.eye[0],d.eye[1],d.eye[2],d.right[0],d.right[1],d.right[2],d.up[0],d.up[1],d.up[2],d.fwd[0],d.fwd[1],
            d.fwd[2],d.tan[0],d.tan[1],d.model[0],d.model[1],d.model[2],d.ac));
        return;
    case rec_root: {
        const auto& f=r.rf;char params[400]{};
        for(unsigned i=0;i<f.paramCount&&i<12;++i) {
            const size_t at=std::strlen(params);if(at+44>sizeof params)break;
            char tok[44];fmt_param(tok,sizeof tok,i,f.p[i]);std::snprintf(params+at,sizeof params-at,"%s%s",at?",":"",tok);
        }
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=acm-root;root=%llx;identity=%llu;layout=%s;valid=%u;rejection=%s;flags=0x%x(ia:%u,deny-vs:%u,hs:%u,ds:%u,gs:%u,ps:%u);"
            "static-samplers=%u;params=%u;t10-param=%d;t10-table=%u;t10-offset=%u;rule-matches=%u(cockpit-rule:%u,world-rule:%u);world-key-on=%u",U(f.key),U(f.identity),
            f.layoutPresent?"found":"MISSING",unsigned(f.layoutValid),f.rejection[0]?f.rejection:"none",f.flags,f.flags&1,(f.flags>>1)&1,(f.flags>>2)&1,(f.flags>>3)&1,
            (f.flags>>4)&1,(f.flags>>5)&1,f.staticSamplers,f.paramCount,f.t10Parameter==UINT32_MAX?-1:int(f.t10Parameter),unsigned(f.t10Table),f.t10Offset,f.matches,
            f.cockpitMatches,f.worldMatches,unsigned(f.worldKeyOn)));
        finish(b,std::snprintf(b,sizeof b,"world-observe;event=acm-root-params;root=%llx;params=%u%s;%s",U(f.key),f.paramCount,f.paramCount>12?"(first 12 shown)":"",params));
        return;
    }
    case rec_rootstate: {
        const auto& x=r.rs;char set[260]{};size_t at=0;
        for(unsigned i=0;i<x.setCount&&i<state_param_cap&&at+36<sizeof set;++i) {
            const auto& q=x.set[i];
            if(q.kind==1)at+=size_t(std::snprintf(set+at,sizeof set-at,"%s%u:constants:%uw",i?",":"",q.index,q.words));
            else at+=size_t(std::snprintf(set+at,sizeof set-at,"%s%u:%s:%llx",i?",":"",q.index,param_kind_name(q.kind),U(q.value)));
        }
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=acm-root-state;n=%u;list=%llx;gen=%llx;ordinal=%u;pso=%llx;root=%llx;layout=%s;t10-param=%d;t10-table=%u;"
            "t10-va=%llx;t10-address=%llx;set-parameters=%u;set=%s",r.rsIndex,U(x.list),U(x.generation),x.ordinal,U(x.pso),U(x.rootKey),x.layoutPresent?"found":"MISSING",
            x.t10Parameter==UINT32_MAX?-1:int(x.t10Parameter),unsigned(x.t10Table),U(x.t10Va),U(x.t10Address),x.setCount,set));
        return;
    }
    case rec_list: {
        const auto& t=r.sum;
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=list;win=%llu;list=%llx;gen=%llx;role=%s;exec-seq=%llu;batch=%u/%u;queue=%llx;producer-list=%u;draws=%u;"
            "acm=%u;cpt=%u;other=%u;first-acm=%u;last-acm=%u;first-cpt=%u;last-cpt=%u;cpt-around-acm=before:%u,between:%u,after:%u;"
            "other-around-acm=before:%u,between:%u,after:%u;other-vs8=before:%08x,after:%08x;other-id=before:%llu,after:%llu",U(r.window),U(r.list),U(r.generation),
            r.role,U(r.execSeq),r.batch,r.batchCount,U(r.queue),unsigned(r.producerList),t.draws,t.acm,t.cpt,t.other,t.firstAcm,t.lastAcm,t.firstCpt,t.lastCpt,
            t.cptBefore,t.cptBetween,t.sinceCpt,t.otherBefore,t.otherBetween,t.sinceOther,t.otherVsBefore,t.otherVsAfter,U(t.otherPsoBefore),U(t.otherPsoAfter)));
        if(t.acm) {
            char f[220];fmt_target(f,sizeof f,t.firstTarget);
            finish(b,std::snprintf(b,sizeof b,"world-observe;event=list-targets;win=%llu;list=%llx;gen=%llx;acm=%u;rt-changes=%u;ds-changes=%u;scene-rt-acm=%u;"
                "acm-with-frame-active=%u;acm-in-frame-list=%u;acm-in-other-list-while-active=%u;distinct-pso=%u;first-pso=%llx;first-target:%s",U(r.window),
                U(r.list),U(r.generation),t.acm,t.rtChanges,t.dsChanges,t.sceneAcm,t.activeAcm,t.inFrameAcm,t.otherListAcm,t.distinctPso,U(t.psoSeen[0]),f));
        }
        return;
    }
    case rec_frame: {
        char lists[260]{};size_t at=0;
        for(unsigned i=0;i<r.listCount&&at+40<sizeof lists;++i) {
            const auto& l=r.lists[i];
            at+=size_t(std::snprintf(lists+at,sizeof lists-at,"%sL%llx.g%llx:a%u/c%u/o%u@%llu.%u",i?",":"",U(l.list),U(l.generation),l.acm,l.cpt,l.other,U(l.execSeq),l.batch));
        }
        finish(a,std::snprintf(a,sizeof a,"world-observe;event=frame;win=%llu;cockpit=%llx.g%llx;exec-seq=%llu;prev-cockpit-exec-seq=%llu;queue=%llx;producer-list=%u;"
            "lists-since-last-cockpit=%u;acm-lists=%u;carrier-lists=%u;acm-total=%u;acm-in-cockpit-list=%u;lists-on-cockpit-queue=%u;per-list=%s%s",U(r.window),
            U(r.list),U(r.generation),U(r.execSeq),U(r.prevCockpitSeq),U(r.queue),unsigned(r.producerList),r.listsSince,r.acmLists,r.carrierLists,r.acmTotal,
            r.acmInCockpit,r.sameQueue,lists,r.droppedLists?";more-lists":""));
        return;
    }
    }
}
} // namespace acz::gpu_copy::wobs
