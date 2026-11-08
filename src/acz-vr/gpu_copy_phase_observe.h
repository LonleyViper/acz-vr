#pragma once
// J-phase-observe (gpu_copy_step5_phase_observe, default OFF): a read-only, bounded observer that tags EVERY draw
// (DrawInstanced / DrawIndexedInstanced) of a cockpit command list (a list recording with >= 1 DrawCPT-identity draw)
// with a frame phase and logs per-phase draw tables only when the frame changed (src/acz-vr/J-phase-observe-findings.md).
// Purpose: find where MISSILES, EXPLOSIONS, MACHINE-GUN TRACERS and RADAR BLIPS are drawn (J-test7-analysis.md section 3).
//  * The Draw seam copies one fixed-size POD record per draw into the recording's buffer (NO GPU work, no command recorded,
//    no state of the step-3/step-5 machinery written, nothing allocated, no I/O).
//  * The list's Close finalises the buffer: it assigns the phases, computes the per-phase summary and the frame signature,
//    decides whether the frame is logged (first frames, a signature never seen, a draw PSO never seen, a change versus the
//    previous frame within a budget, a 5-s sample) and queues POD records. The worker formats and writes them.
//  * Phases (decided at Close, when the first and the LAST DrawCPT draw of the recording are known):
//      before-first-member : draws before the first DrawCPT draw (the world: sky, terrain, DrawACM objects ...)
//      member              : the DrawCPT draws themselves
//      between-members     : non-DrawCPT draws between the first and the last DrawCPT draw
//      hud-region          : draws after the LAST member until the first PROVEN render-target change away from the scene
//                            target (the target of the first member), exactly the HUD replay's region rule (J-hud-capture);
//                            a draw under an unresolvable render target does not end it
//      after-region-end    : draws after the region ended, before the tone-map
//      after-tonemap       : from the first draw after the region end that renders into an output-class target (R10G10B10A2,
//                            the tone-map / copy pass) or with a viewport different from the scene's, to the Close (the late
//                            tail, which includes the stencil-EQUAL draws and may return to the scene target)
//  * The caller serialises State with a leaf mutex (never held across another lock, formatting, feature_log or native calls).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::gpu_copy::phobs {
constexpr unsigned list_cap=4,draw_cap=1024,queue_records=640,phase_count=6;
// J-test8-fixes (radar blips): every 5 s one `tail-sig` line per signature of the after-tonemap / stencil-EQUAL draws seen in that period (read-only).
constexpr unsigned tail_cap=24,tail_lines_per_period=16;constexpr uint64_t tail_period_ms=5000;
// Armed capture windows (phase-observe.arm / phase-observe.mark in %LOCALAPPDATA%/acz-vr; see J-phase-observe-findings.md "Armed windows").
constexpr unsigned window_cap=4,window_ref_slots=2048,window_novel_cap=128,window_novel_per_frame=24,mark_cap=64,armed_log_cap=8;
constexpr uint64_t window_ms=20000,window_learn_ms=1500,window_byte_cap=2ull<<20;
constexpr unsigned sampled_full=3,dump_budget=6,dump_total_cap=200,dump_draw_cap=200,sig_cap=64,pso_cap=256;
constexpr unsigned line_bytes=480; // feature_log::line_cap
constexpr uint32_t dxgi_r10g10b10a2=24;
enum Phase : uint8_t {ph_before=0,ph_member=1,ph_between=2,ph_hud=3,ph_after_region=4,ph_after_tonemap=5};
inline const char* phase_name(unsigned p) noexcept {
    static constexpr const char* n[]={"before-first-member","member","between-members","hud-region","after-region-end","after-tonemap"};
    return p<phase_count?n[p]:"?";
}
inline const char* phase_short(unsigned p) noexcept {
    static constexpr const char* n[]={"pre","mem","btw","hud","end","tm"};return p<phase_count?n[p]:"?";
}
enum Kind : uint8_t {kind_cpt=0,kind_acm=1,kind_other=2};
inline const char* kind_name(unsigned k) noexcept {static constexpr const char* n[]={"DrawCPT_v","DrawACM_v","other"};return k<3?n[k]:"?";}
enum RtClass : uint8_t {rt_unknown=0,rt_scene=1,rt_scene_size=2,rt_r10=3,rt_offscreen=4};
inline const char* rt_class_name(unsigned c) noexcept {
    static constexpr const char* n[]={"unknown","scene","scene-size","r10-output","offscreen"};return c<5?n[c]:"?";
}
inline const char* func_name(uint32_t f) noexcept {
    static constexpr const char* n[]={"?","never","less","equal","less-equal","greater","not-equal","greater-equal","always"};return f<9?n[f]:"?";
}
// One observed draw (everything the Draw seam learned under the hook-state lock). The finalisation fills phase/rtClass/flags.
struct Draw {
    uint64_t pso=0,id=0,rtv=0,rt=0,dsv=0,ds=0,rtWidth=0;
    uint32_t rtHeight=0,rtFormat=0,dsFormat=0,vs8=0,ps8=0,count=0,instances=0,ordinal=0;
    float vpW=0,vpH=0;
    uint32_t stencilRead=0,stencilWrite=0,rcMask=0,rc[4]{};
    uint32_t blendSrc=0,blendDst=0,blendOp=0,blendSrcA=0,blendDstA=0,blendOpA=0,depthFunc=0,frontFunc=0,backFunc=0,frontPass=0,backPass=0,writeMask=0;
    uint8_t kind=kind_other,phase=ph_before,rtClass=rt_unknown,omCount=0;
    bool indexed=false,psoKnown=false,rtKnown=false,dsKnown=false,vpKnown=false,blend=false,independentBlend=false,depthEnable=false,depthWrite=false,
        stencil=false,rcAvailable=false,psoNew=false,vpOffScene=false;
};
// J-effects-smoke-fix: psSmoke / psSprite / psHud = draws of the pixel shader prefixes d2464ed2 (missile smoke), 4caf6e2d (effects sprites), d34ee153 (HUD sprite).
struct PhaseSum {uint32_t draws=0,indexed=0,onScene=0,unknownRt=0,offVp=0,newPso=0;uint64_t idx=0,vtx=0,inst=0,shape=0;uint32_t psSmoke=0,psSprite=0,psHud=0;};
struct Summary {
    PhaseSum p[phase_count]{};uint64_t sig=0;uint32_t total=0,stored=0,overflow=0,members=0,firstMember=0,lastMember=0,regionEndOrd=0,tonemapOrd=0;
    uint64_t sceneRt=0,sceneW=0;uint32_t sceneH=0,sceneFormat=0;bool sceneKnown=false;
};
enum Reason : uint8_t {reason_none=0,reason_first=1,reason_new_signature=2,reason_new_pso=3,reason_change=4,reason_sample=5,reason_window=6};
inline const char* reason_name(unsigned r) noexcept {
    static constexpr const char* n[]={"none","first","new-signature","new-pso","change","sample","window"};return r<7?n[r]:"?";
}
enum RecordType : uint8_t {rec_summary=0,rec_draw=1,rec_armed=2,rec_mark=3,rec_window_end=4,rec_novel=5,rec_tail=6};
struct Record {
    RecordType type=rec_summary;uint64_t frame=0,list=0,generation=0,prevSig=0;
    Summary sum{};Draw draw{};uint8_t reason=reason_none,changed=0;bool newSig=false;uint32_t omitted=0,dumpDraws=0;
    // Armed windows (zero for ordinary records). A window summary / draw carries `winIndex` (1-based), the close tick `t`, the ms since the arm and since the
    // last mark (-1: none) and the number of draws absent from the reference set (`novel`, per phase in novelPhase).
    uint8_t winIndex=0;uint64_t t=0,utc=0,key=0;int64_t sinceArm=-1,sinceMark=-1;uint32_t novel=0,novelPhase[phase_count]{};
    // rec_armed / rec_mark / rec_window_end / rec_novel: free-form counters (see the formatter)
    bool accepted=false;const char* why="";uint32_t a0=0,a1=0,a2=0,a3=0,a4=0;uint64_t b0=0,b1=0,b2=0,b3=0;
};
inline uint64_t mix64(uint64_t x) noexcept {
    x+=0x9e3779b97f4a7c15ull;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);
}
// The PSO's stable identity: its creation shader hashes and creation state, NOT the object pointer or the creation sequence number (the game re-creates
// identical PSOs every frame: live step31 had 98 distinct PSO pointers/ids for one 1x1 after-tonemap draw that never changed). Without creation
// hashes (the observer keeps them; fixtures may not) it falls back to the id / pointer.
inline uint64_t pso_key(const Draw& d) noexcept {
    if(!d.vs8&&!d.ps8)return d.id?d.id:d.pso;
    uint64_t h=mix64((uint64_t(d.vs8)<<32)|d.ps8);
    h=mix64(h^(uint64_t(d.kind)|(uint64_t(d.psoKnown)<<2)|(uint64_t(d.blend)<<3)|(uint64_t(d.independentBlend)<<4)|(uint64_t(d.depthEnable)<<5)|
        (uint64_t(d.depthWrite)<<6)|(uint64_t(d.stencil)<<7)|(uint64_t(d.depthFunc&15)<<8)|(uint64_t(d.frontFunc&15)<<12)|(uint64_t(d.backFunc&15)<<16)|
        (uint64_t(d.frontPass&15)<<20)|(uint64_t(d.backPass&15)<<24)|(uint64_t(d.writeMask&15)<<28)|(uint64_t(d.stencilRead&0xff)<<32)|(uint64_t(d.stencilWrite&0xff)<<40)));
    h=mix64(h^(uint64_t(d.blendSrc&63)|(uint64_t(d.blendDst&63)<<6)|(uint64_t(d.blendOp&63)<<12)|(uint64_t(d.blendSrcA&63)<<18)|(uint64_t(d.blendDstA&63)<<24)|
        (uint64_t(d.blendOpA&63)<<30)));
    return h?h:1;
}
// A draw as the reference set sees it: the PSO's stable identity and the phase it is drawn in.
inline uint64_t draw_key(uint64_t psoKey,unsigned phase) noexcept {const uint64_t h=mix64(psoKey^(uint64_t(phase+1)<<58));return h?h:1;}
// The draw's shape: the part of the draw that makes a DIFFERENT draw (not its counts): identity, state, target class.
inline uint64_t shape_of(const Draw& d) noexcept {
    uint64_t h=mix64(pso_key(d));
    h=mix64(h^(uint64_t(d.kind)|(uint64_t(d.indexed)<<4)|(uint64_t(d.blend)<<5)|(uint64_t(d.depthEnable)<<6)|(uint64_t(d.depthWrite)<<7)|
        (uint64_t(d.stencil)<<8)|(uint64_t(d.rtClass)<<9)|(uint64_t(d.depthFunc)<<12)|(uint64_t(d.frontFunc)<<16)|(uint64_t(d.vpOffScene)<<20)));
    return h;
}
struct Buffer {
    bool used=false,lastLost=false;uint64_t list=0,generation=0,touch=0;uint32_t count=0,overflow=0,cpt=0,cptStored=0,first=0,last=0;
    std::array<Draw,draw_cap> d{};
    void clear(uint64_t l,uint64_t g,uint64_t t) noexcept {used=true;lastLost=false;list=l;generation=g;touch=t;count=overflow=cpt=cptStored=first=last=0;}
};
struct SigEntry {uint64_t sig=0,frames=0,firstFrame=0;uint32_t draws[phase_count]{};};
// A tail candidate: an after-tonemap draw, or any draw whose stencil test (front or back) is EQUAL (func 3). Signature = PSO identity + state + target class
// (not the counts), so a radar-blip draw whose index count follows the game state is ONE entry with a moving count.
inline bool tail_candidate(const Draw& d) noexcept {return d.phase==ph_after_tonemap||(d.stencil&&(d.frontFunc==3||d.backFunc==3));}
struct TailSig {uint64_t key=0,frames=0,draws=0,firstFrame=0;uint32_t maxPerFrame=0,cur=0,minCount=0,maxCount=0;uint64_t curFrame=0;Draw draw{};};
struct State {
    std::array<Buffer,list_cap> tab{};uint64_t touch=0;
    // Lifetime totals (always counted, logged or not).
    uint64_t draws=0,lists=0,cockpitLists=0,noCockpit=0,discarded=0,evicted=0,overflowFrames=0,dumps=0,dumpReason[6]{},newSigs=0,sigFull=0,newPsos=0,psoFull=0,
        suppressed=0,lost=0,records=0,unknownRtDraws=0,tailSceneDraws=0,noLastRegion=0;
    bool due=false;unsigned budget=dump_budget;
    bool havePrev=false;Summary prev{};Summary last{};uint64_t lastFrame=0;
    std::array<SigEntry,sig_cap> sigs{};unsigned sigCount=0;
    std::array<TailSig,tail_cap> tails{};unsigned tailCount=0;uint64_t tailLogMs=0,tailOverflow=0,tailEmitted=0;
    std::array<uint64_t,pso_cap> psos{};unsigned psoCount=0;
    std::array<Record,queue_records> queue{};uint64_t head=0,tail=0;
    // ---- Armed capture windows. The last cockpit frame's draw keys (the quiet reference at the moment of arming), the reference set of the open window
    // (hash set of draw keys, 0 = empty slot) and the per-window table of novel draw keys. All fixed size; State is process-lifetime storage.
    std::array<uint64_t,draw_cap> lastPso{};std::array<uint8_t,draw_cap> lastPhase{};unsigned lastKeyCount=0;
    struct Novel {uint64_t key=0,firstFrame=0,lastFrame=0,frames=0;uint32_t maxPerFrame=0,cur=0;uint64_t curFrame=0;Draw draw{};};
    struct Window {
        bool active=false,exhausted=false,markSeen=false;unsigned index=0;
        uint64_t startMs=0,endMs=0,learnUntil=0,markMs=0,bytes=0,frames=0,framesLogged=0,firstFrame=0,lastFrame=0,novelDraws=0,novelKeyOverflow=0,refCount=0,novelDrawsLogged=0,
            novelDrawsOmitted=0;
        unsigned novelCount=0;
    } win{};
    std::array<uint64_t,window_ref_slots> ref{};
    std::array<Novel,window_novel_cap> novel{};
    uint64_t dumpBase=0,windowsStarted=0,armRefused=0,armLogged=0,marks=0,marksLogged=0,staleIgnored=0,windowFrames=0,windowBytesTotal=0,windowsEnded=0;
    bool pollInit=false;uint64_t seenArm=0,seenMark=0;
    struct Totals {
        uint64_t draws=0,lists=0,cockpitLists=0,noCockpit=0,discarded=0,evicted=0,overflowFrames=0,dumps=0,dumpReason[6]{},newSigs=0,sigFull=0,newPsos=0,psoFull=0,
            suppressed=0,lost=0,records=0,unknownRtDraws=0,tailSceneDraws=0,distinctSigs=0,distinctPsos=0;Summary last{};uint64_t lastFrame=0;
        uint64_t windowsStarted=0,windowsEnded=0,armRefused=0,marks=0,staleIgnored=0,windowFrames=0,windowBytesTotal=0,windowBytes=0;bool windowActive=false;unsigned windowIndex=0;
    };
    Totals totals() const noexcept {
        Totals t;t.draws=draws;t.lists=lists;t.cockpitLists=cockpitLists;t.noCockpit=noCockpit;t.discarded=discarded;t.evicted=evicted;
        t.overflowFrames=overflowFrames;t.dumps=dumps;for(unsigned i=0;i<6;++i)t.dumpReason[i]=dumpReason[i];t.newSigs=newSigs;t.sigFull=sigFull;
        t.newPsos=newPsos;t.psoFull=psoFull;t.suppressed=suppressed;t.lost=lost;t.records=records;t.unknownRtDraws=unknownRtDraws;t.tailSceneDraws=tailSceneDraws;
        t.distinctSigs=sigCount;t.distinctPsos=psoCount;t.last=last;t.lastFrame=lastFrame;
        t.windowsStarted=windowsStarted;t.windowsEnded=windowsEnded;t.armRefused=armRefused;t.marks=marks;t.staleIgnored=staleIgnored;t.windowFrames=windowFrames;
        t.windowBytesTotal=windowBytesTotal;t.windowBytes=win.bytes;t.windowActive=win.active;t.windowIndex=win.index;return t;
    }
    // J-test8-fixes: accumulate one tail candidate (called at Close, per cockpit frame) and emit the period's table every tail_period_ms.
    void tail_add(const Draw& d,uint64_t frame) noexcept {
        uint64_t k=mix64(pso_key(d)^(uint64_t(d.rtClass)<<56)^(uint64_t(d.indexed)<<60)^(uint64_t(d.phase)<<52));
        if(!k)k=1;
        TailSig* t=nullptr;
        for(unsigned i=0;i<tailCount;++i)if(tails[i].key==k){t=&tails[i];break;}
        if(!t){if(tailCount>=tail_cap){++tailOverflow;return;}t=&tails[tailCount++];t->key=k;t->draw=d;t->firstFrame=frame;t->minCount=t->maxCount=d.count;}
        if(t->curFrame!=frame){t->curFrame=frame;t->cur=0;++t->frames;}
        ++t->cur;if(t->cur>t->maxPerFrame)t->maxPerFrame=t->cur;++t->draws;
        if(d.count<t->minCount)t->minCount=d.count;if(d.count>t->maxCount)t->maxCount=d.count;
    }
    void tail_flush(uint64_t nowMs,uint64_t frame) noexcept {
        if(!nowMs)return;
        if(!tailLogMs){tailLogMs=nowMs;return;}
        if(nowMs-tailLogMs<tail_period_ms)return;
        tailLogMs=nowMs;unsigned lines=0;
        for(unsigned i=0;i<tailCount;++i) {
            TailSig& t=tails[i];if(!t.draws)continue;
            if(lines<tail_lines_per_period) {
                Record r;r.type=rec_tail;r.frame=frame;r.key=t.key;r.draw=t.draw;r.a0=uint32_t(t.frames);r.a1=t.maxPerFrame;r.a2=t.minCount;r.a3=t.maxCount;r.b0=t.draws;
                r.b1=t.firstFrame;r.b2=tailOverflow;push(r);++lines;++tailEmitted;
            }
            t.frames=t.draws=0;t.maxPerFrame=0;t.cur=0;t.minCount=t.maxCount=t.draw.count; // per-period counters restart; the signature stays
        }
    }
    // ---- queue
    void push(const Record& r) noexcept {
        if(head-tail==queue_records){++lost;return;}
        queue[head++%queue_records]=r;++records;
    }
    bool pop(Record& r) noexcept {if(tail==head)return false;r=queue[tail++%queue_records];return true;}
    // ---- Draw seam
    Buffer* find(uint64_t list) noexcept {
        for(auto& b:tab)if(b.used&&b.list==list)return &b;
        return nullptr;
    }
    // Records one draw of the list recording `list` (one recording per list route: a Reset discards it, the Close finalises it).
    void draw(uint64_t list,uint64_t gen,const Draw& in) noexcept {
        ++draws;
        Buffer* b=find(list);
        if(!b) {
            for(auto& t:tab)if(!t.used){b=&t;break;}
            if(!b){b=&tab[0];for(auto& t:tab)if(t.touch<b->touch)b=&t;++evicted;}
            b->clear(list,gen,++touch);
        } else b->touch=++touch;
        if(b->count>=draw_cap){++b->overflow;if(in.kind==kind_cpt){++b->cpt;b->lastLost=true;}return;}
        Draw& d=b->d[b->count];d=in;d.ordinal=b->count+1;
        if(in.kind==kind_cpt){if(!b->cptStored)b->first=b->count;++b->cpt;++b->cptStored;b->last=b->count;b->lastLost=false;}
        ++b->count;
    }
    // A Reset list: its unfinalised recording is discarded (counted).
    void discard(uint64_t list) noexcept {
        for(auto& t:tab)if(t.used&&t.list==list){t.used=false;++discarded;}
    }
    // ---- Close seam: classify, summarise, decide, queue.
    static bool is_output(const Draw& d,const Summary& s) noexcept {
        if(d.rtKnown&&d.rtFormat==dxgi_r10g10b10a2)return true;
        (void)s;return d.vpOffScene;
    }
    // Phases, rt classes and the summary of a buffer holding >= 1 DrawCPT draw. Public for the offline fixtures.
    static void classify(Buffer& b,Summary& s) noexcept {
        s=Summary{};
        const unsigned n=b.count;s.total=n+b.overflow;s.stored=n;s.overflow=b.overflow;s.members=b.cpt;
        const Draw& fm=b.d[b.first];
        s.sceneKnown=fm.rtKnown;s.sceneRt=fm.rt;s.sceneW=fm.rtWidth;s.sceneH=fm.rtHeight;s.sceneFormat=fm.rtFormat;
        s.firstMember=b.d[b.first].ordinal;s.lastMember=b.d[b.last].ordinal;
        bool regionEnded=false,tonemap=false;
        for(unsigned i=0;i<n;++i) {
            Draw& d=b.d[i];
            // render target class against the scene target (the first member's)
            if(!d.rtKnown)d.rtClass=rt_unknown;
            else if(s.sceneKnown&&d.rt==s.sceneRt)d.rtClass=rt_scene;
            else if(s.sceneW&&s.sceneH&&d.rtWidth==s.sceneW&&d.rtHeight==s.sceneH)d.rtClass=rt_scene_size;
            else if(d.rtFormat==dxgi_r10g10b10a2)d.rtClass=rt_r10;
            else d.rtClass=rt_offscreen;
            d.vpOffScene=d.vpKnown&&fm.vpKnown&&(d.vpW!=fm.vpW||d.vpH!=fm.vpH);
            if(i<b.first)d.phase=ph_before;
            else if(d.kind==kind_cpt)d.phase=ph_member;
            else if(i<b.last)d.phase=ph_between;
            else {
                if(!regionEnded&&s.sceneKnown&&d.rtKnown&&d.rt!=s.sceneRt){regionEnded=true;s.regionEndOrd=d.ordinal;}
                if(!regionEnded)d.phase=ph_hud;
                else {
                    if(!tonemap&&is_output(d,s)){tonemap=true;s.tonemapOrd=d.ordinal;}
                    d.phase=tonemap?ph_after_tonemap:ph_after_region;
                }
            }
            PhaseSum& p=s.p[d.phase];
            ++p.draws;if(d.indexed){++p.indexed;p.idx+=uint64_t(d.count)*d.instances;}else p.vtx+=uint64_t(d.count)*d.instances;
            p.inst+=d.instances;if(d.rtClass==rt_scene)++p.onScene;if(d.rtClass==rt_unknown)++p.unknownRt;if(d.vpOffScene)++p.offVp;
            p.shape+=shape_of(d);
            p.psSmoke+=d.ps8==0xd2464ed2u;p.psSprite+=d.ps8==0x4caf6e2du;p.psHud+=d.ps8==0xd34ee153u;
        }
        uint64_t h=0xcbf29ce484222325ull;
        for(unsigned i=0;i<phase_count;++i){h=mix64(h^s.p[i].draws);h=mix64(h^s.p[i].shape);}
        s.sig=h;
    }
    bool known_pso(uint64_t key,bool& isNew) noexcept {
        isNew=false;if(!key)return false;
        for(unsigned i=0;i<psoCount;++i)if(psos[i]==key)return true;
        if(psoCount>=pso_cap){++psoFull;return false;}
        psos[psoCount++]=key;isNew=true;return false;
    }
    // ---- Armed capture windows (control-file protocol; the worker polls the files, the game thread's Close feeds the window).
    // Reference set: open-addressing hash set of draw keys (0 = empty); full tables refuse inserts (counted by the caller via the return value).
    bool ref_has(uint64_t key) const noexcept {
        unsigned i=unsigned(key%window_ref_slots);
        for(unsigned n=0;n<window_ref_slots;++n,i=(i+1)%window_ref_slots){if(ref[i]==key)return true;if(!ref[i])return false;}
        return false;
    }
    bool ref_add(uint64_t key) noexcept {
        unsigned i=unsigned(key%window_ref_slots);
        for(unsigned n=0;n<window_ref_slots;++n,i=(i+1)%window_ref_slots){
            if(ref[i]==key)return true;
            if(!ref[i]){if(win.refCount>=window_ref_slots*3/4)return false;ref[i]=key;++win.refCount;return true;}
        }
        return false;
    }
    // Estimated formatted bytes of one queued record: its two lines at the feature-log line cap (an upper bound, so the real file bytes are <= the charge).
    static constexpr uint64_t record_bytes=2ull*line_bytes;
    static constexpr uint64_t window_end_reserve=(window_novel_cap+2ull)*record_bytes;
    bool charge() noexcept {
        if(win.bytes+record_bytes+window_end_reserve>window_byte_cap){win.exhausted=true;return false;}
        win.bytes+=record_bytes;return true;
    }
    void push_control(Record& r) noexcept {push(r);}
    // The tables are state of "what is new": an armed window starts them afresh, seeded with the quiet frame that was the last cockpit frame at arming.
    void begin_window(uint64_t nowMs,uint64_t utcMs,uint32_t& oldPsos,uint32_t& oldSigs,uint32_t& seededKeys) noexcept {
        oldPsos=psoCount;oldSigs=sigCount;
        win=Window{};ref.fill(0);novel.fill(Novel{});
        win.active=true;win.index=unsigned(++windowsStarted);win.startMs=nowMs;win.endMs=nowMs+window_ms;win.learnUntil=nowMs+window_learn_ms;(void)utcMs;
        psoCount=0;sigCount=0;budget=dump_budget;dumpBase=dumps;
        for(unsigned i=0;i<lastKeyCount;++i) {
            bool isNew=false;(void)known_pso(lastPso[i],isNew);
            ref_add(draw_key(lastPso[i],lastPhase[i]));
        }
        seededKeys=uint32_t(win.refCount);
        if(havePrev&&sigCount<sig_cap){auto& e=sigs[sigCount++];e.sig=prev.sig;e.frames=1;e.firstFrame=lastFrame;for(unsigned i=0;i<phase_count;++i)e.draws[i]=prev.p[i].draws;}
    }
    // The arm file appeared. Always queues one `phase-observe-armed` record (accepted or not; refusals are logged at most armed_log_cap times).
    void arm(uint64_t nowMs,uint64_t utcMs) noexcept {
        Record r;r.type=rec_armed;r.t=nowMs;r.utc=utcMs;r.frame=cockpitLists;
        const bool saturated=sigCount>=sig_cap||psoCount>=pso_cap||dumps-dumpBase>=dump_total_cap;
        r.a4=saturated?1u:0u;r.b2=sigCount;r.b3=psoCount;
        if(win.active) {r.accepted=false;r.why="window-active";}
        else if(windowsStarted>=window_cap) {r.accepted=false;r.why="window-cap";}
        else {
            uint32_t oldPsos=0,oldSigs=0,seeded=0;begin_window(nowMs,utcMs,oldPsos,oldSigs,seeded);
            r.accepted=true;r.why="ok";r.a0=win.index;r.a1=oldPsos;r.a2=oldSigs;r.a3=seeded;r.b0=win.endMs;r.b1=win.learnUntil;
            push(r);++armLogged;return;
        }
        ++armRefused;r.a0=uint32_t(windowsStarted);
        if(armLogged<armed_log_cap){++armLogged;push(r);}
    }
    // The mark file appeared (the driver touches it right before firing).
    void mark(uint64_t nowMs,uint64_t utcMs) noexcept {
        ++marks;
        if(win.active){win.markSeen=true;win.markMs=nowMs;}
        if(marksLogged>=mark_cap)return;
        ++marksLogged;
        Record r;r.type=rec_mark;r.t=nowMs;r.utc=utcMs;r.frame=cockpitLists;r.winIndex=uint8_t(win.active?win.index:0);r.sinceArm=win.active?int64_t(nowMs-win.startMs):-1;
        r.a0=uint32_t(marks);push(r);
    }
    void end_window(uint64_t nowMs,const char* why) noexcept {
        if(!win.active)return;
        // one record per novel key (bounded by window_novel_cap), then the window summary
        for(unsigned i=0;i<win.novelCount;++i) {
            const auto& n=novel[i];Record r;r.type=rec_novel;r.winIndex=uint8_t(win.index);r.key=n.key;r.draw=n.draw;r.frame=n.firstFrame;r.list=n.lastFrame;r.generation=n.frames;
            r.omitted=n.maxPerFrame;push(r);
        }
        Record r;r.type=rec_window_end;r.winIndex=uint8_t(win.index);r.t=nowMs;r.why=why;r.frame=win.frames;r.list=win.framesLogged;r.generation=win.bytes;
        r.a0=uint32_t(win.novelDraws);r.a1=win.novelCount;r.a2=uint32_t(win.novelKeyOverflow);r.a3=uint32_t(win.novelDrawsLogged);r.a4=uint32_t(win.novelDrawsOmitted);
        r.b0=win.firstFrame;r.b1=win.lastFrame;r.b2=win.markSeen?win.markMs:0;r.b3=win.startMs;r.accepted=win.markSeen;r.sinceArm=int64_t(nowMs-win.startMs);
        push(r);
        windowBytesTotal+=win.bytes;++windowsEnded;win.active=false;
    }
    // Worker poll: `armStamp` / `markStamp` identify the current state of the two control files (0: absent; otherwise a value that changes when the file is
    // created or touched, e.g. its last-write time). The first poll only records the stamps (a file left over from an earlier session is not an order).
    void poll(uint64_t nowMs,uint64_t utcMs,uint64_t armStamp,uint64_t markStamp) noexcept {
        if(win.active&&nowMs>=win.endMs)end_window(nowMs,"time");
        if(!pollInit){pollInit=true;seenArm=armStamp;seenMark=markStamp;staleIgnored=(armStamp?1u:0u)+(markStamp?1u:0u);return;}
        if(armStamp!=seenArm){seenArm=armStamp;if(armStamp)arm(nowMs,utcMs);}
        if(markStamp!=seenMark){seenMark=markStamp;if(markStamp)mark(nowMs,utcMs);}
    }
    // One cockpit frame of an open window: logs the summary (every frame) and the draws absent from the reference set (first sighting of each key).
    // Returns true when the frame was handled by the window (the ordinary dump logic is skipped for it).
    bool window_frame(Buffer& b,const Summary& s,uint64_t frame,uint64_t nowMs,uint64_t listId,uint8_t changed,bool newSig) noexcept {
        if(!win.active)return false;
        if(!charge()){end_window(nowMs,"bytes");return false;}
        const bool learning=nowMs<win.learnUntil;
        Record rec;rec.type=rec_summary;rec.frame=frame;rec.list=listId;rec.generation=b.generation;rec.sum=s;rec.reason=reason_window;rec.winIndex=uint8_t(win.index);rec.t=nowMs;
        rec.sinceArm=int64_t(nowMs-win.startMs);rec.sinceMark=win.markSeen?int64_t(nowMs-win.markMs):-1;
        rec.prevSig=havePrev?prev.sig:0;rec.changed=changed;rec.newSig=newSig;rec.dumpDraws=0;
        ++win.frames;++windowFrames;++win.framesLogged;if(!win.firstFrame)win.firstFrame=frame;win.lastFrame=frame;
        unsigned perFrameLogged=0;
        for(unsigned i=0;i<b.count;++i) {
            Draw& d=b.d[i];const uint64_t key=draw_key(pso_key(d),d.phase);
            if(learning){ref_add(key);continue;}
            if(ref_has(key))continue;
            ++rec.novel;++rec.novelPhase[d.phase];++win.novelDraws;
            Novel* n=nullptr;
            for(unsigned k=0;k<win.novelCount;++k)if(novel[k].key==key){n=&novel[k];break;}
            bool first=false;
            if(!n) {
                if(win.novelCount<window_novel_cap){n=&novel[win.novelCount++];n->key=key;n->firstFrame=frame;n->draw=d;first=true;}
                else ++win.novelKeyOverflow;
            }
            if(n) {
                if(n->curFrame!=frame){n->curFrame=frame;n->cur=0;++n->frames;}
                ++n->cur;if(n->cur>n->maxPerFrame)n->maxPerFrame=n->cur;n->lastFrame=frame;
            }
            if(first) {
                if(perFrameLogged>=window_novel_per_frame||!charge()){++win.novelDrawsOmitted;continue;}
                Record x;x.type=rec_draw;x.frame=frame;x.list=listId;x.generation=b.generation;x.draw=d;x.winIndex=uint8_t(win.index);x.key=key;x.t=nowMs;
                x.sinceArm=rec.sinceArm;x.sinceMark=rec.sinceMark;
                pending[perFrameLogged++]=x;++win.novelDrawsLogged;
            }
        }
        push(rec);
        for(unsigned i=0;i<perFrameLogged;++i)push(pending[i]);
        if(win.exhausted)end_window(nowMs,"bytes");
        return true;
    }
    std::array<Record,window_novel_per_frame> pending{};
    // Returns true when the buffer was a cockpit recording. `frame` counts cockpit recordings.
    // `nowMs`: the tick (GetTickCount64) of the close; only the armed windows use it (0 in fixtures that never arm one).
    bool close(uint64_t list,uint64_t nowMs=0) noexcept {
        Buffer* b=find(list);if(!b)return false;
        ++lists;
        if(!b->cptStored||b->count==0){++noCockpit;b->used=false;return false;}
        if(b->lastLost)++noLastRegion;
        ++cockpitLists;const uint64_t frame=cockpitLists;
        if(win.active&&nowMs>=win.endMs)end_window(nowMs,"time");
        Summary s;classify(*b,s);
        if(s.overflow)++overflowFrames;
        unsigned newPsoDraws=0;
        for(unsigned i=0;i<b->count;++i) {
            Draw& d=b->d[i];bool isNew=false;const uint64_t pk=pso_key(d);(void)known_pso(pk,isNew);
            d.psoNew=isNew;if(isNew){++newPsoDraws;++newPsos;++s.p[d.phase].newPso;}
            if(d.rtClass==rt_unknown)++unknownRtDraws;
            if(d.phase==ph_after_tonemap&&d.rtClass==rt_scene)++tailSceneDraws;
            if(tail_candidate(d))tail_add(d,frame);
        }
        if(!win.active)tail_flush(nowMs,frame); // an armed window logs every frame itself
        // change versus the previous frame: phases whose draw count or shape set differ
        uint8_t changed=0;
        for(unsigned i=0;i<phase_count;++i)
            if(!havePrev||prev.p[i].draws!=s.p[i].draws||prev.p[i].shape!=s.p[i].shape)changed=uint8_t(changed|(1u<<i));
        bool newSig=false;
        {   bool found=false;
            for(unsigned i=0;i<sigCount;++i)if(sigs[i].sig==s.sig){found=true;++sigs[i].frames;break;}
            if(!found) {
                if(sigCount<sig_cap){auto& e=sigs[sigCount++];e.sig=s.sig;e.frames=1;e.firstFrame=frame;for(unsigned i=0;i<phase_count;++i)e.draws[i]=s.p[i].draws;newSig=true;++newSigs;}
                else ++sigFull;
            }
        }
        // This frame is the "quiet reference" a later arm seeds its window from; an open window handles the frame itself (every frame is logged).
        lastKeyCount=b->count<draw_cap?b->count:draw_cap;
        for(unsigned i=0;i<lastKeyCount;++i){lastPso[i]=pso_key(b->d[i]);lastPhase[i]=b->d[i].phase;}
        if(win.active&&window_frame(*b,s,frame,nowMs,list,changed,newSig)) {
            prev=s;havePrev=true;last=s;lastFrame=frame;b->used=false;return true;
        }
        unsigned reason=reason_none;
        if(frame<=sampled_full)reason=reason_first;
        else if(newSig)reason=reason_new_signature;
        else if(newPsoDraws)reason=reason_new_pso;
        else if(changed&&havePrev)reason=reason_change;
        if(reason==reason_change) {
            if(budget)--budget;else {++suppressed;reason=reason_none;}
        }
        if(reason==reason_none&&due)reason=reason_sample;
        if(reason!=reason_none&&reason!=reason_first)due=false; // a logged frame counts as the 5-s sample
        if(reason!=reason_none&&dumps-dumpBase>=dump_total_cap){++suppressed;reason=reason_none;}
        const Summary prevSum=prev;const bool hadPrev=havePrev;
        prev=s;havePrev=true;last=s;lastFrame=frame;
        if(reason!=reason_none) {
            ++dumps;++dumpReason[reason];
            // which draws are written: the changed phases (and draws with a never-seen PSO); the first dump and samples write every
            // non-member phase (the first one also the members)
            uint8_t mask=changed;
            if(reason==reason_first||reason==reason_sample)mask=uint8_t(0x3f&~(frame==1?0u:(1u<<ph_member)));
            if(!hadPrev)mask=0x3f;
            unsigned want=0;for(unsigned i=0;i<b->count;++i)if((mask&(1u<<b->d[i].phase))||b->d[i].psoNew)++want;
            const unsigned written=want<dump_draw_cap?want:dump_draw_cap;
            Record r;r.type=rec_summary;r.frame=frame;r.list=list;r.generation=b->generation;r.prevSig=hadPrev?prevSum.sig:0;r.sum=s;r.reason=uint8_t(reason);
            r.changed=changed;r.newSig=newSig;r.omitted=want-written;r.dumpDraws=written;push(r);
            unsigned out=0;
            for(unsigned i=0;i<b->count&&out<written;++i) {
                const Draw& d=b->d[i];if(!((mask&(1u<<d.phase))||d.psoNew))continue;
                Record x;x.type=rec_draw;x.frame=frame;x.list=list;x.generation=b->generation;x.draw=d;push(x);++out;
            }
        }
        b->used=false;
        return true;
    }
};

// ------------------------------------------------------------- worker: formatting
using U=unsigned long long;
inline void finish(char (&a)[line_bytes],int n) noexcept {
    static constexpr char mark[]=";truncated=1";
    if(n<0||n>=int(sizeof a))std::memcpy(a+line_bytes-sizeof mark,mark,sizeof mark);
}
// Formats one record into up to two lines (b empty when unused).
inline void fmt_since(char* out,size_t cap,int64_t v) noexcept {if(v<0)std::snprintf(out,cap,"none");else std::snprintf(out,cap,"%lld",static_cast<long long>(v));}
inline void format(const Record& r,char (&a)[line_bytes],char (&b)[line_bytes]) noexcept {
    a[0]=0;b[0]=0;
    char since[2][24];
    switch(r.type) {
    case rec_armed:
        if(r.accepted)
            finish(a,std::snprintf(a,sizeof a,"phase-observe;event=phase-observe-armed;accepted=1;window=%u/%u;t=%llu;utc-ms=%llu;length-ms=%llu;learn-ms=%llu;ends-t=%llu;"
                "bytes-cap=%llu;caps=%u-novel-draw-lines-per-frame,%u-novel-keys;cockpit-frames-so-far=%llu;"
                "prior-state=%s;reset=psos:%u,signatures:%u;reference-seeded-draw-keys=%u",r.a0,window_cap,U(r.t),U(r.utc),
                U(window_ms),U(window_learn_ms),U(r.b0),U(window_byte_cap),window_novel_per_frame,window_novel_cap,U(r.frame),
                r.a4?"saturated":"running",r.a1,r.a2,r.a3));
        else
            finish(a,std::snprintf(a,sizeof a,"phase-observe;event=phase-observe-armed;accepted=0;reason=%s;windows-started=%u/%u;t=%llu;utc-ms=%llu;cockpit-frames-so-far=%llu",
                r.why,r.a0,window_cap,U(r.t),U(r.utc),U(r.frame)));
        return;
    case rec_mark:
        fmt_since(since[0],sizeof since[0],r.sinceArm);
        finish(a,std::snprintf(a,sizeof a,"phase-observe;event=phase-observe-mark;mark=%u;t=%llu;utc-ms=%llu;cockpit-frames-so-far=%llu;window=%u;since-arm-ms=%s",r.a0,U(r.t),
            U(r.utc),U(r.frame),unsigned(r.winIndex),since[0]));
        return;
    case rec_window_end:
        // frames = cockpit frames in the window, logged = frames written, bytes = the charge against the 2 MiB cap, mark-t = tick of the mark (0: none)
        finish(a,std::snprintf(a,sizeof a,"phase-observe;event=phase-observe-window-end;window=%u/%u;reason=%s;length-ms=%lld;frames=%llu;logged=%llu;first-frame=%llu;"
            "bytes=%llu;novel-draws=%u;novel-keys=%u;key-overflow=%u;novel-lines=%u;lines-omitted=%u;mark-seen=%u;mark-t=%llu",unsigned(r.winIndex),window_cap,r.why,
            static_cast<long long>(r.sinceArm),U(r.frame),U(r.list),U(r.b0),U(r.generation),r.a0,r.a1,r.a2,r.a3,r.a4,unsigned(r.accepted),U(r.b2)));
        return;
    case rec_tail: {
        const auto& d=r.draw;
        finish(a,std::snprintf(a,sizeof a,"phase-observe;event=tail-sig;key=%016llx;pso-key=%016llx;frame=%llu;first-frame=%llu;phase=%s;shader=%s;vs8=%08x;ps8=%08x;rt-class=%s;rt=%llx;"
            "rt-size=%llux%u;rt-fmt=%u;indexed=%u;period-frames=%u;period-draws=%llu;max-draws-per-frame=%u;count-min-max=%u/%u;tail-overflow=%llu",U(r.key),U(pso_key(d)),U(r.frame),
            U(r.b1),phase_name(d.phase),kind_name(d.kind),d.vs8,d.ps8,rt_class_name(d.rtClass),U(d.rt),U(d.rtWidth),d.rtHeight,d.rtFormat,unsigned(d.indexed),r.a0,U(r.b0),r.a1,
            r.a2,r.a3,U(r.b2)));
        finish(b,std::snprintf(b,sizeof b,"phase-observe;event=tail-sig-state;key=%016llx;blend=%u,src:%u,dst:%u,op:%u,src-a:%u,dst-a:%u,op-a:%u;depth=%u,%s,w%u;"
            "stencil=%u,front:%s,back:%s,read:%02x,write:%02x,ref:untracked;write-mask=%x;vp=%gx%g;om=%u",U(r.key),unsigned(d.blend),d.blendSrc,d.blendDst,d.blendOp,d.blendSrcA,
            d.blendDstA,d.blendOpA,unsigned(d.depthEnable),func_name(d.depthFunc),unsigned(d.depthWrite),unsigned(d.stencil),func_name(d.frontFunc),func_name(d.backFunc),
            d.stencilRead&0xffu,d.stencilWrite&0xffu,d.writeMask,double(d.vpW),double(d.vpH),unsigned(d.omCount)));
        return;
    }
    case rec_novel: {
        const auto& d=r.draw;
        // line 1: which draw and how often; line 2: its (first-sighting) state
        finish(a,std::snprintf(a,sizeof a,"phase-observe;event=phase-observe-novel;window=%u;key=%016llx;pso-key=%016llx;first-frame=%llu;last-frame=%llu;frames-seen=%llu;"
            "max-draws-per-frame=%u;phase=%s;shader=%s;vs8=%08x;ps8=%08x",unsigned(r.winIndex),U(r.key),U(pso_key(d)),U(r.frame),U(r.list),U(r.generation),r.omitted,
            phase_name(d.phase),kind_name(d.kind),d.vs8,d.ps8));
        finish(b,std::snprintf(b,sizeof b,"phase-observe;event=phase-observe-novel-state;window=%u;key=%016llx;rt-class=%s;rt-size=%llux%u;rt-fmt=%u;indexed=%u;count=%u;instances=%u;"
            "blend=%u,src:%u,dst:%u,op:%u;depth=%u,%s,w%u;stencil=%u,front:%s",unsigned(r.winIndex),U(r.key),rt_class_name(d.rtClass),U(d.rtWidth),d.rtHeight,d.rtFormat,
            unsigned(d.indexed),d.count,d.instances,unsigned(d.blend),d.blendSrc,d.blendDst,d.blendOp,unsigned(d.depthEnable),func_name(d.depthFunc),unsigned(d.depthWrite),
            unsigned(d.stencil),func_name(d.frontFunc)));
        return;
    }
    default: break;
    }
    if(r.type==rec_summary) {
        const auto& s=r.sum;
        finish(a,std::snprintf(a,sizeof a,"phase-observe;event=frame;frame=%llu;list=%llx;gen=%llx;reason=%s;draws=%u;stored=%u;overflow=%u;members=%u;"
            "first-member-ord=%u;last-member-ord=%u;region-end-ord=%u;tonemap-ord=%u;scene-rt=%llx(%llux%u,fmt:%u,known:%u);sig=%016llx;prev-sig=%016llx;"
            "new-sig=%u;changed=%02x;dump-draws=%u;dump-omitted=%u",U(r.frame),U(r.list),U(r.generation),reason_name(r.reason),s.total,s.stored,s.overflow,
            s.members,s.firstMember,s.lastMember,s.regionEndOrd,s.tonemapOrd,U(s.sceneRt),U(s.sceneW),s.sceneH,s.sceneFormat,unsigned(s.sceneKnown),U(s.sig),
            U(r.prevSig),unsigned(r.newSig),unsigned(r.changed),r.dumpDraws,r.omitted));
        if(r.winIndex) { // armed window: tick, time since the arm / mark, draws absent from the reference set (per phase pre/mem/btw/hud/end/tm)
            const size_t used=std::strlen(a);fmt_since(since[0],sizeof since[0],r.sinceArm);fmt_since(since[1],sizeof since[1],r.sinceMark);
            finish(a,std::snprintf(a+used,sizeof a-used,";win=%u;t=%llu;since-arm-ms=%s;since-mark-ms=%s;ref-novel=%u(%u/%u/%u/%u/%u/%u)",unsigned(r.winIndex),U(r.t),since[0],
                since[1],r.novel,r.novelPhase[0],r.novelPhase[1],r.novelPhase[2],r.novelPhase[3],r.novelPhase[4],r.novelPhase[5])+int(used));
        }
        size_t at=size_t(std::snprintf(b,sizeof b,"phase-observe;event=frame-phases;frame=%llu;fields=draws/indexed/on-scene-rt/unknown-rt/vp-off-scene/new-pso/index-sum/vertex-sum/instance-sum/shape-hash",U(r.frame)));
        for(unsigned i=0;i<phase_count&&at<sizeof b;++i) {
            const auto& p=s.p[i];
            const int n=std::snprintf(b+at,sizeof b-at,";%s=%u/%u/%u/%u/%u/%u/%llu/%llu/%llu/%08x",phase_short(i),p.draws,p.indexed,p.onScene,p.unknownRt,p.offVp,p.newPso,
                U(p.idx),U(p.vtx),U(p.inst),unsigned(p.shape&0xffffffffu));
            if(n<0||at+size_t(n)>=sizeof b){at=sizeof b;break;}
            at+=size_t(n);
        }
        if(at>=sizeof b)finish(b,int(sizeof b));
        return;
    }
    const auto& d=r.draw;
    finish(a,std::snprintf(a,sizeof a,"phase-observe;event=draw;frame=%llu;ord=%u;phase=%s;rt-class=%s;rt=%llx;rt-size=%llux%u;rt-fmt=%u;rtv=%llx;dsv=%llx;ds=%llx;ds-fmt=%u;"
        "om=%u;vp=%gx%g;vp-off-scene=%u;rt-known=%u;pso=%llx;id=%llu;shader=%s;vs8=%08x;ps8=%08x;new-pso=%u;indexed=%u;count=%u;instances=%u",U(r.frame),d.ordinal,
        phase_name(d.phase),rt_class_name(d.rtClass),U(d.rt),U(d.rtWidth),d.rtHeight,d.rtFormat,U(d.rtv),U(d.dsv),U(d.ds),d.dsFormat,unsigned(d.omCount),
        double(d.vpW),double(d.vpH),unsigned(d.vpOffScene),unsigned(d.rtKnown),U(d.pso),U(d.id),kind_name(d.kind),d.vs8,d.ps8,unsigned(d.psoNew),
        unsigned(d.indexed),d.count,d.instances));
    if(r.winIndex) { // first sighting of a draw absent from the armed window's reference set
        const size_t used=std::strlen(a);fmt_since(since[0],sizeof since[0],r.sinceArm);fmt_since(since[1],sizeof since[1],r.sinceMark);
        finish(a,std::snprintf(a+used,sizeof a-used,";win=%u;novel=1;key=%016llx",unsigned(r.winIndex),U(r.key))+int(used));
    }
    char rc[96]="none";
    if(d.rcAvailable) {
        size_t at=size_t(std::snprintf(rc,sizeof rc,"mask:%x",d.rcMask));
        for(unsigned i=0;i<4&&at<sizeof rc;++i) { // only written words carry a value
            char w[16];if(d.rcMask&(1u<<i))std::snprintf(w,sizeof w,"%08x",d.rc[i]);else std::snprintf(w,sizeof w,"????????");
            const int m=std::snprintf(rc+at,sizeof rc-at,"%s%s",i?",":":",w);if(m<0||at+size_t(m)>=sizeof rc)break;at+=size_t(m);
        }
    }
    finish(b,std::snprintf(b,sizeof b,"phase-observe;event=draw-state;frame=%llu;ord=%u;phase=%s;pso-known=%u;depth=%u,%s,w%u;stencil=%u,front:%s,back:%s,front-pass:%u,"
        "back-pass:%u,read:%02x,write:%02x,ref:untracked;blend=%u,src:%u,dst:%u,op:%u,src-a:%u,dst-a:%u,op-a:%u,independent:%u,mask:%x,factor:untracked;rc=%s",
        U(r.frame),d.ordinal,phase_name(d.phase),unsigned(d.psoKnown),unsigned(d.depthEnable),func_name(d.depthFunc),unsigned(d.depthWrite),unsigned(d.stencil),
        func_name(d.frontFunc),func_name(d.backFunc),d.frontPass,d.backPass,d.stencilRead&0xffu,d.stencilWrite&0xffu,unsigned(d.blend),d.blendSrc,d.blendDst,d.blendOp,
        d.blendSrcA,d.blendDstA,d.blendOpA,unsigned(d.independentBlend),d.writeMask,rc));
    if(r.winIndex) { // the window timing of a novel draw rides on its state line
        const size_t used=std::strlen(b);fmt_since(since[0],sizeof since[0],r.sinceArm);fmt_since(since[1],sizeof since[1],r.sinceMark);
        finish(b,std::snprintf(b+used,sizeof b-used,";pso-key=%016llx;t=%llu;since-arm-ms=%s;since-mark-ms=%s",U(pso_key(d)),U(r.t),since[0],since[1])+int(used));
    }
}
} // namespace acz::gpu_copy::phobs
