// PS2 address probe: READ-ONLY. See ee_ps2_probe.h and docs/world-render/ps2-probe-impl.md.
// This file never calls Memory::write_float.
#include "ee_ps2_probe.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

namespace acz::ee_camera {
namespace {
inline unsigned long long ull(uint64_t v) noexcept {return static_cast<unsigned long long>(v);}
inline uint32_t ld32(const uint8_t* p) noexcept {uint32_t v;std::memcpy(&v,p,4);return v;}
inline float ldf(const uint8_t* p) noexcept {float f;std::memcpy(&f,p,4);return f;}
// A formatted line (max 767 chars; longer text is cut).
struct Line {
    char b[768];size_t n=0;
    Line() noexcept {b[0]=0;}
    void f(const char* fmt,...) noexcept {
        if(n>=sizeof b-1)return;
        va_list ap;va_start(ap,fmt);
        const int w=std::vsnprintf(b+n,sizeof b-n,fmt,ap);
        va_end(ap);
        if(w>0)n=n+size_t(w)<sizeof b?n+size_t(w):sizeof b-1;
    }
};
// "a,b,c" of n floats, or UNREAD.
void floats(Line& l,const uint8_t* p,unsigned n,bool ok) noexcept {
    if(!ok){l.f("UNREAD");return;}
    for(unsigned i=0;i<n;++i)l.f(i?",%.7g":"%.7g",double(ldf(p+4*i)));
}
void hex32(Line& l,const uint8_t* p) noexcept {
    for(unsigned i=0;i<32;++i)l.f("%02x%s",p[i],(i%4==3&&i!=31)?" ":"");
}
bool eq3(const uint8_t* a,const uint8_t* b) noexcept {
    bool nz=false;
    for(unsigned i=0;i<3;++i) {
        const float x=ldf(a+4*i),y=ldf(b+4*i);
        if(!std::isfinite(x)||!std::isfinite(y)||std::fabs(x-y)>1e-3f)return false;
        if(std::fabs(x)>1e-6f)nz=true;
    }
    return nz;     // all-zero triples prove nothing
}
bool is_prefix(const uint8_t* p) noexcept {
    static constexpr const char* ids[]={"SLUS","SLES","SLPM","SLPS","SCUS","SCES","SCPS"};
    for(const char* id:ids)if(p[0]==uint8_t(id[0])&&p[1]==uint8_t(id[1])&&p[2]==uint8_t(id[2])&&p[3]==uint8_t(id[3]))return true;
    return false;
}
// 'SLUS_213.46' / 'SLUS-21346' / 'SLUS_21346': prefix, '_' or '-', exactly five digits with an optional '.' after the third. Returns the length or 0.
unsigned match_id(const uint8_t* p,size_t avail) noexcept {
    if(avail<10||!is_prefix(p)||(p[4]!='_'&&p[4]!='-'))return 0;
    unsigned digits=0,i=5;
    for(;i<avail&&i<11;++i) {
        if(p[i]>='0'&&p[i]<='9'){++digits;continue;}
        if(p[i]=='.'&&digits==3)continue;
        break;
    }
    return digits==5?i:0;
}
constexpr uint8_t aob[16]={0x18,0xB7,0x3D,0x00,0x88,0x44,0x3F,0x00,0x18,0xB7,0x3D,0x00,0x68,0x45,0x3F,0x00};
} // namespace

Ps2Prober::Ps2Prober(Options o) noexcept : o_(o) {
    if(o_.chunk_bytes==0||o_.chunk_bytes>pp_chunk_default)o_.chunk_bytes=pp_chunk_default;
    if(o_.chunks_per_call==0)o_.chunks_per_call=1;
    if(o_.file_cap>pp_file_cap)o_.file_cap=pp_file_cap;
    if(o_.file_cap<2*pp_file_tail)o_.file_cap=2*pp_file_tail;
    if(o_.enabled)phase_.store(p_waiting,std::memory_order_release);
    reason_.store(o_.enabled?"waiting":"off",std::memory_order_release);
}

bool Ps2Prober::terminate(Phase to,const char* why) noexcept {
    uint8_t cur=phase_.load(std::memory_order_acquire);
    while(cur!=p_off&&cur<p_finished) {
        if(phase_.compare_exchange_weak(cur,static_cast<uint8_t>(to),std::memory_order_acq_rel)) {
            reason_.store(why,std::memory_order_release);pending_.store(true,std::memory_order_release);return true;
        }
    }
    return false;
}

void Ps2Prober::say(Sink& log,bool important,const char* text,bool essential) noexcept {
    const size_t n=std::strlen(text);
    const size_t limit=essential?o_.file_cap:o_.file_cap-pp_file_tail;
    if(fileLen_+n+1<=limit){std::memcpy(file_+fileLen_,text,n);fileLen_+=n;file_[fileLen_++]='\n';}
    else truncated_=true;
    if(important||essential||logCount_<o_.max_log_lines){log.line(important||essential,text);++logCount_;}
    else ++logDropped_;
}

void Ps2Prober::tick(const LensIn& in) noexcept {
    if(in.stop_file)stop_.store(true,std::memory_order_release);
    if(phase()!=p_waiting)return;
    if(in.frame==lastFrame_)return;      // one count per distinct Present frame
    lastFrame_=in.frame;
    const uint64_t lo=in.v.have?in.v.lo:0;
    if(!lo){anchorLo_.store(0,std::memory_order_release);anchorTicks_.store(0,std::memory_order_release);return;}
    if(lo!=anchorLo_.load(std::memory_order_acquire)){anchorLo_.store(lo,std::memory_order_release);anchorTicks_.store(1,std::memory_order_release);return;}
    anchorTicks_.store(anchorTicks_.load(std::memory_order_acquire)+1,std::memory_order_release);
}

void Ps2Prober::finish(const char* why) noexcept {finishReq_.store(why?why:"finish",std::memory_order_release);}

// ------------------------------------------------------------------ gated reads
void Ps2Prober::note_reject(uint64_t off,uint64_t n,const char* reason,const LensRegion* q) noexcept {
    if(firstRej_.have)return;
    firstRej_.have=true;firstRej_.off=off;firstRej_.len=n;firstRej_.reason=reason;
    if(q){firstRej_.qbase=q->base;firstRej_.qsize=q->size;firstRej_.qalloc=q->alloc;firstRej_.state=q->state;firstRej_.type=q->type;firstRej_.protect=q->protect;}
}
bool Ps2Prober::rd(LensMem& mem,uint64_t off,void* out,uint32_t n) noexcept {
    if(!n||off>=block_bytes||n>block_bytes-off){note_reject(off,n,"outside-block",nullptr);return false;}
    const uint64_t a=base_+off;
    if(a<base_||a+n<a){note_reject(off,n,"address-overflow",nullptr);return false;}   // checked arithmetic
    LensRegion q{};
    if(!mem.query(a,q)){note_reject(off,n,"query-failed",nullptr);++unreadable_;return false;}
    if(!q.data){note_reject(off,n,"not-data-page",&q);++excluded_;return false;}
    if(q.alloc!=allocId_){note_reject(off,n,"allocation-changed",&q);++identChanged_;return false;}
    if(a<q.base||a-q.base>=q.size||n>q.size-(a-q.base)){note_reject(off,n,"crosses-region",&q);++unreadable_;return false;}
    if(!mem.read(a,out,n)){note_reject(off,n,"read-failed",&q);++unreadable_;return false;}
    return true;
}

// ------------------------------------------------------------------ worker
void Ps2Prober::work(LensMem& mem,Sink& log,const Ps2WorkIn& in) noexcept {
    if(!o_.enabled||phase()==p_off||terminal())return;
    if(workBusy_.test_and_set(std::memory_order_acquire))return;
    work_locked(mem,log,in);
    workBusy_.clear(std::memory_order_release);
}

void Ps2Prober::work_locked(LensMem& mem,Sink& log,const Ps2WorkIn& in) noexcept {
    const double now=in.now_ms;
    if(in.stop_file||stop_.load(std::memory_order_acquire)){terminate(p_stopped,"stop-file");return;}
    if(finishReq_.load(std::memory_order_acquire)){terminate(p_stopped,finishReq_.load());return;}
    if(phase()==p_waiting) {
        if(waitStart_<0)waitStart_=now;
        const uint64_t lo=anchorLo_.load(std::memory_order_acquire);
        const unsigned ticks=anchorTicks_.load(std::memory_order_acquire);
        if(lo&&ticks>=o_.settle_ticks) {
            base_=lo;
            LensRegion q{};
            const bool qok=mem.query(base_,q);
            if(!qok||!q.data) {      // the block start must be readable data; later windows are gated chunk by chunk (a split region only makes coverage UNKNOWN)
                note_reject(0,block_bytes,"anchor-block-not-readable-data",qok?&q:nullptr);
                Line l;l.f("ps2-probe;event=start;base=0x%llX;result=anchor-block-not-readable",ull(base_));say(log,true,l.b);
                terminate(p_aborted,"anchor-block-not-readable");return;
            }
            allocId_=q.alloc;scanStart_=now;set_phase(p_scanning);
            Line l;l.f("ps2-probe;event=start;base=0x%llX;alloc=0x%llX;region-size=0x%llX;read-only=1;writes-game-memory=0;offsets-assumed-ps2-addresses=1;"
                "caps=bytes:%llu,scan-ms:%.0f,cam-hits:%u,gid-hits:%u,aob-hits:%u,fixed-samples:%u,cam-samples:%u,stage-samples:%u,file:%zu,log-lines:%u",
                ull(base_),ull(allocId_),ull(q.size),ull(o_.byte_cap),o_.scan_cap_ms,pp_cam_cap,pp_gid_cap,pp_aob_cap,pp_fixed_samples,pp_cam_samples,pp_stage_samples,o_.file_cap,o_.max_log_lines);
            say(log,true,l.b);
            return;
        }
        if(now-lastWait_>=pp_wait_log_ms) {
            lastWait_=now;
            Line l;l.f("ps2-probe;event=wait;why=%s;anchor-ticks=%u/%u;waited-ms=%.0f",lo?"anchor-settling":"no-tracks-look-anchor",ticks,o_.settle_ticks,now-waitStart_);
            say(log,false,l.b);
        }
        if(now-waitStart_>o_.wait_cap_ms)terminate(p_aborted,"never-anchor");
        return;
    }
    if(phase()==p_scanning) {
        if(anchorLo_.load(std::memory_order_acquire)!=base_&&!anchorMoved_) {
            anchorMoved_=true;                  // the probe re-located the block: keep reading the one pinned at the start (identity-gated), flag it
            Line l;l.f("ps2-probe;event=anchor-moved;pinned=0x%llX;now=0x%llX",ull(base_),ull(anchorLo_.load()));say(log,true,l.b);
        }
        if(!scan_step(mem,log,now))begin_sampling(log,now);
        return;
    }
    if(phase()==p_sampling) {
        if(anchorLo_.load(std::memory_order_acquire)!=base_&&!anchorMoved_) {
            anchorMoved_=true;
            Line l;l.f("ps2-probe;event=anchor-moved;pinned=0x%llX;now=0x%llX",ull(base_),ull(anchorLo_.load()));say(log,true,l.b);
        }
        sample(mem,log,now);
    }
}

// ------------------------------------------------------------------ scanning (two passes: 0 camera signature, 1 game id)
bool Ps2Prober::scan_step(LensMem& mem,Sink& log,double now) noexcept {
    for(unsigned c=0;c<o_.chunks_per_call;++c) {
        if(pass_>=2)return false;
        const bool camPass=pass_==0;
        const uint64_t lo=camPass?pp_cam_lo:pp_id_lo,hi=camPass?pp_cam_hi:pp_id_hi;
        if(!passBegun_) {
            passBegun_=true;pos_=lo;passBytes0_=scanned_;passHits0_=camPass?camTotal_:gidTotal_+aobTotal_;
            Line l;l.f("ps2-probe;event=scan-start;pass=%s;lo=0x%llX;hi=0x%llX;chunk=%u",camPass?"camera-signature-512x448":"game-id",ull(lo),ull(hi),o_.chunk_bytes);
            say(log,true,l.b);
        }
        if(pos_>=hi){end_pass(log,now);++pass_;passBegun_=false;continue;}
        if(now-scanStart_>o_.scan_cap_ms){scanStop_="scan-time-cap";capHit_=true;pass_=2;end_pass(log,now);return false;}
        const uint64_t n=hi-pos_<o_.chunk_bytes?hi-pos_:o_.chunk_bytes;
        if(scanned_+n>o_.byte_cap){scanStop_="byte-cap";capHit_=true;pass_=2;end_pass(log,now);return false;}
        scan_chunk(mem,log,pos_,n,hi,camPass);
        pos_+=n;
    }
    return true;
}
void Ps2Prober::end_pass(Sink& log,double now) noexcept {
    (void)now;
    const bool camPass=pass_==0;
    Line l;l.f("ps2-probe;event=scan-done;pass=%s;stop=%s;bytes=%llu;hits=%u;kept=%u",camPass?"camera-signature-512x448":(pass_==1?"game-id":"aborted"),scanStop_,
        ull(scanned_-passBytes0_),camPass?camTotal_:gidTotal_+aobTotal_,camPass?nCam_:nGid_+nAob_);
    say(log,true,l.b);
}
void Ps2Prober::scan_chunk(LensMem& mem,Sink& log,uint64_t pos,uint64_t n,uint64_t hi,bool camPass) noexcept {
    (void)log;
    uint64_t wlen=n+pp_overlap;if(wlen>block_bytes-pos)wlen=block_bytes-pos;     // the window never leaves the block
    ++chunks_;
    const uint64_t a=base_+pos;
    LensRegion q{};
    if(!mem.query(a,q)){note_reject(pos,wlen,"query-failed",nullptr);++unreadable_;unreadableBytes_+=n;return;}
    if(!q.data){note_reject(pos,wlen,"not-data-page",&q);++excluded_;unreadableBytes_+=n;return;}
    if(q.alloc!=allocId_){note_reject(pos,wlen,"allocation-changed",&q);++identChanged_;unreadableBytes_+=n;return;}
    if(a<q.base||wlen>q.size||a-q.base>q.size-wlen){note_reject(pos,wlen,"crosses-region",&q);++unreadable_;unreadableBytes_+=n;return;}
    if(!mem.read(a,scratch_,uint32_t(wlen))){note_reject(pos,wlen,"read-failed",&q);++unreadable_;unreadableBytes_+=n;return;}
    scanned_+=n;
    if(camPass) {
        for(uint64_t i=0;i<n&&i+8<=wlen&&pos+i+8<=hi;i+=4) {
            if(ld32(scratch_+i)!=pp_sig_w||ld32(scratch_+i+4)!=pp_sig_h)continue;
            // The cheat layout repeats the pair at struct+0x1C0 (background W/H): that hit is the same struct, not a second one.
            if(havePrim_&&pos+i==lastPrim_+pp_c_bg){++bgPair_;continue;}
            havePrim_=true;lastPrim_=pos+i;
            ++camTotal_;
            if(nCam_<pp_cam_cap){cam_[nCam_].off=pos+i;++nCam_;}
        }
        return;
    }
    for(uint64_t i=0;i<n;++i) {
        const uint8_t c=scratch_[i];
        if(c=='S') {
            const unsigned len=match_id(scratch_+i,size_t(wlen-i));
            if(len&&pos+i+len<=hi) {
                ++gidTotal_;
                if(nGid_<pp_gid_cap){Ps2Hit& h=gid_[nGid_++];h.off=pos+i;std::memcpy(h.text,scratch_+i,len);h.text[len]=0;}
                i+=len-1;
            }
        } else if(c==aob[0]&&i+16<=wlen&&std::memcmp(scratch_+i,aob,16)==0&&pos+i+16<=hi) {
            ++aobTotal_;
            if(nAob_<pp_aob_cap){aob_[nAob_].off=pos+i;++nAob_;}
        }
    }
}

// ------------------------------------------------------------------ sampling
void Ps2Prober::begin_sampling(Sink& log,double now) noexcept {
    scanComplete_=pass_>=2&&!capHit_&&unreadable_==0&&excluded_==0&&identChanged_==0&&std::strcmp(scanStop_,"running")==0;
    if(std::strcmp(scanStop_,"running")==0)scanStop_="complete";
    if(firstRej_.have) {
        Line l;l.f("ps2-probe;event=first-reject;reason=%s;off=0x%llX;len=%llu;query-base=0x%llX;query-size=0x%llX;query-alloc=0x%llX;state=0x%X;type=0x%X;protect=0x%X;"
            "unreadable-chunks=%u;excluded-chunks=%u;ident-changed=%u",firstRej_.reason,ull(firstRej_.off),ull(firstRej_.len),ull(firstRej_.qbase),ull(firstRej_.qsize),
            ull(firstRej_.qalloc),firstRej_.state,firstRej_.type,firstRej_.protect,unreadable_,excluded_,identChanged_);
        say(log,true,l.b);
    }
    for(unsigned i=0;i<nCam_;++i){Line l;l.f("ps2-probe;event=hit;kind=camera-struct;idx=%u;off=0x%llX;sig=512,448",i,ull(cam_[i].off));say(log,true,l.b);}
    for(unsigned i=0;i<nGid_;++i){Line l;l.f("ps2-probe;event=hit;kind=game-id;idx=%u;off=0x%llX;text=%s",i,ull(gid_[i].off),gid_[i].text);say(log,true,l.b);}
    for(unsigned i=0;i<nAob_;++i){Line l;l.f("ps2-probe;event=hit;kind=game-id-aob;idx=%u;off=0x%llX",i,ull(aob_[i].off));say(log,true,l.b);}
    Line l;l.f("ps2-probe;event=sampling-start;camera-hits=%u/%u;bg-pair-hits-merged=%u;game-id-hits=%u/%u;aob-hits=%u/%u;scan-stop=%s",nCam_,camTotal_,bgPair_,nGid_,gidTotal_,nAob_,aobTotal_,scanStop_);
    say(log,true,l.b);
    t0_=now;sIdx_=0;set_phase(p_sampling);
}

void Ps2Prober::sample(LensMem& mem,Sink& log,double now) noexcept {
    const double due=t0_+double(sIdx_)*pp_sample_period_ms;
    if(now<due)return;
    if(now-due>pp_sample_period_ms/2)++lateSamples_;
    const unsigned s=sIdx_;
    // E6 fields (the existing camera probe's), same second as the struct sample
    uint8_t e6yaw[4]{},e6air[12]{},e6hdg[4]{},e6pos[12]{},e6rot[12]{};
    const bool oy=rd(mem,pp_e6_yaw,e6yaw,4),oa=rd(mem,pp_e6_air,e6air,12),oh=rd(mem,pp_e6_hdg,e6hdg,4),op=rd(mem,pp_e6_pos,e6pos,12),orr=rd(mem,pp_e6_rot,e6rot,12);
    valUnread_+=unsigned(!oy)+unsigned(!oa)+unsigned(!oh)+unsigned(!op)+unsigned(!orr);
    if(s<pp_cam_samples) {
        Line l;l.f("ps2-probe;event=e6;s=%u;t=%.0f;yaw=",s,now-t0_);floats(l,e6yaw,1,oy);
        l.f(";air=");floats(l,e6air,3,oa);l.f(";heading=");floats(l,e6hdg,1,oh);l.f(";campos=");floats(l,e6pos,3,op);l.f(";rot=");floats(l,e6rot,3,orr);
        say(log,false,l.b);
    }
    // stage identification (every second): mission byte, the two gates, camera position of the first struct hit and E6
    {
        uint8_t m=0,g=0,ph=0;uint8_t cp[24]{};
        const bool om=rd(mem,pp_off_mission,&m,1),og=rd(mem,pp_off_gate,&g,1),oph=rd(mem,pp_off_phase,&ph,1);
        valUnread_+=unsigned(!om)+unsigned(!og)+unsigned(!oph);
        bool ocp=false;
        if(nCam_)ocp=rd(mem,cam_[0].off+pp_c_pos,cp,24);
        if(nCam_)valUnread_+=unsigned(!ocp);
        if(om) {
            if(lastMission_>=0&&lastMission_!=m)++missionChanges_;
            lastMission_=m;
            bool seen=false;for(unsigned i=0;i<nMission_;++i)seen=seen||missionSeen_[i]==m;
            if(!seen){if(nMission_<8)missionSeen_[nMission_++]=m;else missionOverflow_=true;}
        }
        Line l;l.f("ps2-probe;event=stage;s=%u;t=%.0f;mission=",s,now-t0_);
        if(om)l.f("%u",unsigned(m));else l.f("UNREAD");
        l.f(";gate3ACEA0=");if(og)l.f("%u",unsigned(g));else l.f("UNREAD");
        l.f(";phase76587C=");if(oph)l.f("%u",unsigned(ph));else l.f("UNREAD");
        l.f(";cam-b30=");if(nCam_)floats(l,cp,3,ocp);else l.f("no-struct-hit");
        l.f(";cam-b48=");if(nCam_)floats(l,cp+12,3,ocp);else l.f("no-struct-hit");
        l.f(";e6-campos=");floats(l,e6pos,3,op);
        say(log,false,l.b);
        ++stageDone_;
    }
    if(s%2==0&&s/2<pp_fixed_samples){sample_fixed(mem,log,s,now);++fixedDone_;}
    if(s<pp_cam_samples)sample_cam(mem,log,s,now);
    ++sIdx_;
    if(s<pp_cam_samples)++camDone_;
    if(sIdx_>=pp_stage_samples)terminate(p_finished,"complete");
}

void Ps2Prober::sample_fixed(LensMem& mem,Sink& log,unsigned s,double now) noexcept {
    struct P {const char* name;uint64_t off;const char* text;};
    P pts[4+pp_gid_cap+pp_aob_cap];unsigned np=0;
    pts[np++]={"mission",pp_off_mission,nullptr};pts[np++]={"gate3ACEA0",pp_off_gate,nullptr};
    pts[np++]={"phase76587C",pp_off_phase,nullptr};pts[np++]={"draw765320",pp_off_draw,nullptr};
    for(unsigned i=0;i<nGid_;++i)pts[np++]={"game-id",gid_[i].off,gid_[i].text};
    for(unsigned i=0;i<nAob_;++i)pts[np++]={"game-id-aob",aob_[i].off,nullptr};
    for(unsigned i=0;i<np;++i) {
        uint8_t ctx[32]{};
        // 32 bytes of context: 16 before the value and 16 from it on, shifted to stay inside the block at its very end (every point is >= 0x300000)
        uint64_t cs=pts[i].off-16;if(cs>block_bytes-32)cs=block_bytes-32;
        const bool okc=rd(mem,cs,ctx,32);
        uint8_t v[4]{};bool okv=okc;
        if(okc)std::memcpy(v,ctx+(pts[i].off-cs),4);else okv=rd(mem,pts[i].off,v,4);
        if(!okc)++valUnread_;
        Line l;l.f("ps2-probe;event=fixed;s=%u;t=%.0f;name=%s;off=0x%llX;",s,now-t0_,pts[i].name,ull(pts[i].off));
        if(okv)l.f("u8=%u;u32=0x%08X;f32=%.9g;",unsigned(v[0]),ld32(v),double(ldf(v)));else {l.f("u8=UNREAD;u32=UNREAD;f32=UNREAD;");++valUnread_;}
        if(pts[i].text)l.f("text=%s;",pts[i].text);
        l.f("ctx-off=0x%llX;ctx=",ull(cs));if(okc)hex32(l,ctx);else l.f("UNREAD");
        say(log,false,l.b);
    }
}

void Ps2Prober::sample_cam(LensMem& mem,Sink& log,unsigned s,double now) noexcept {
    uint8_t e6pos[12]{},e6rot[12]{};
    const bool op=rd(mem,pp_e6_pos,e6pos,12),orr=rd(mem,pp_e6_rot,e6rot,12);
    for(unsigned i=0;i<nCam_;++i) {
        const uint64_t S=cam_[i].off;
        uint8_t sig[8]{},dist[8]{},bg[8]{},pos[36]{},rot[12]{};
        const bool os=rd(mem,S,sig,8),od=rd(mem,S+pp_c_dist,dist,8),ob=rd(mem,S+pp_c_bg,bg,8),opp=rd(mem,S+pp_c_pos,pos,36),orot=rd(mem,S+pp_c_rot,rot,12);
        valUnread_+=unsigned(!os)+unsigned(!od)+unsigned(!ob)+unsigned(!opp)+unsigned(!orot);
        Line l;l.f("ps2-probe;event=cam;s=%u;t=%.0f;hit=%u;off=0x%llX;sig=",s,now-t0_,i,ull(S));floats(l,sig,2,os);
        l.f(";dist198=");floats(l,dist,2,od);l.f(";bg1C0=");floats(l,bg,2,ob);l.f(";pos=");floats(l,pos,9,opp);l.f(";rot=");floats(l,rot,3,orot);
        // correlation with E6 (same second): rotation equal to +0xFDD870 and any of the three position triples equal to +0xFDD860
        const bool rotEq=orot&&orr&&eq3(rot,e6rot);
        bool posEq=false;if(opp&&op)for(unsigned k=0;k<3;++k)posEq=posEq||eq3(pos+12*k,e6pos);
        if(os&&od&&ob&&opp&&orot)++camReads_[i];
        rotEq_[i]+=rotEq;posEq_[i]+=posEq;
        l.f(";rot-eq-e6=%s;pos-eq-e6=%s",(orot&&orr)?(rotEq?"1":"0"):"?",(opp&&op)?(posEq?"1":"0"):"?");
        say(log,false,l.b);
    }
}

// ------------------------------------------------------------------ report
void Ps2Prober::summary(Sink& log) noexcept {
    // coverage: any cap hit, unreadable/excluded chunk, early end, dropped hits/lines, truncated file, unreadable value or missing anchor
    char why[320];why[0]=0;size_t wl=0;
    auto add=[&](bool c,const char* t){if(c&&wl<sizeof why-40)wl+=size_t(std::snprintf(why+wl,sizeof why-wl,"%s%s",wl?",":"",t));};
    const bool early=phase()!=p_finished;
    const bool hitsDropped=camTotal_>nCam_||gidTotal_>nGid_||aobTotal_>nAob_;
    add(capHit_,scanStop_);add(unreadable_||excluded_||identChanged_,"unreadable-or-excluded-chunks");add(early,"ended-early");add(hitsDropped,"hit-cap");
    add(logDropped_>0,"log-lines-dropped");add(truncated_,"file-truncated");add(valUnread_>0,"unreadable-values");add(anchorMoved_,"anchor-moved");
    add(pass_<2&&!capHit_,"scan-not-finished");
    coverageKnown_=wl==0;
    {Line l;l.f("ps2-probe;event=summary;phase=%s;reason=%s;base=0x%llX;scanned-bytes=%llu;chunks=%u;unreadable-chunks=%u;excluded-chunks=%u;ident-changed=%u;"
        "unreadable-bytes=%llu;camera-hits=%u/%u;game-id-hits=%u/%u;aob-hits=%u/%u;bg-pair-hits=%u;fixed-samples=%u/%u;cam-samples=%u/%u;stage-samples=%u/%u;late-samples=%u;"
        "unreadable-values=%u;log-lines=%u;log-lines-dropped=%u;file-truncated=%u",
        early?(phase()==p_stopped?"stopped":"aborted"):"finished",reason(),ull(base_),ull(scanned_),chunks_,unreadable_,excluded_,identChanged_,ull(unreadableBytes_),
        nCam_,camTotal_,nGid_,gidTotal_,nAob_,aobTotal_,bgPair_,fixedDone_,pp_fixed_samples,camDone_,pp_cam_samples,stageDone_,pp_stage_samples,lateSamples_,valUnread_,logCount_,logDropped_,unsigned(truncated_));
        say(log,true,l.b,true);}
    {Line l;l.f("ps2-probe;event=coverage;known=%u;coverage=%s;reasons=%s",unsigned(coverageKnown_),coverageKnown_?"complete-for-these-ranges":"UNKNOWN",wl?why:"none");say(log,true,l.b,true);}
    {   // numerical facts only
        Line l;l.f("ps2-probe;event=verdict;mission-values=");
        if(!nMission_)l.f("none-read");
        for(unsigned i=0;i<nMission_;++i)l.f(i?",%u":"%u",unsigned(missionSeen_[i]));
        if(missionOverflow_)l.f(",more");
        l.f(";mission-changes=%u;game-id-found=%u;camera-struct-found=%u;",missionChanges_,unsigned(gidTotal_+aobTotal_>0),unsigned(camTotal_>0));
        for(unsigned i=0;i<nCam_;++i)l.f("hit%u-rot-eq-e6=%u/%u,pos-eq-e6=%u/%u;",i,rotEq_[i],camReads_[i],posEq_[i],camReads_[i]);
        l.f("statement=\"values only; whether block offsets equal PS2 addresses and how the struct relates to E6 is decided offline\";coverage=%s",coverageKnown_?"complete-for-these-ranges":"UNKNOWN");
        if(!camTotal_&&!capHit_&&unreadable_==0&&excluded_==0)l.f(";camera-struct=not found within this signature/range/sampling");
        say(log,true,l.b,true);
    }
}

bool Ps2Prober::drain(Sink& log,LensOut& out) noexcept {
    if(!o_.enabled)return false;
    if(drainBusy_.test_and_set(std::memory_order_acquire))return false;
    bool wrote=false;
    if(!terminal()&&finishReq_.load(std::memory_order_acquire)) {
        bool got=false;
        for(unsigned i=0;i<20000&&!(got=!workBusy_.test_and_set(std::memory_order_acquire));++i)std::this_thread::yield();
        if(got){terminate(p_stopped,finishReq_.load());workBusy_.clear(std::memory_order_release);}
    }
    if(terminal()&&pending_.exchange(false,std::memory_order_acq_rel)) {
        bool got=false;
        for(unsigned i=0;i<20000&&!(got=!workBusy_.test_and_set(std::memory_order_acquire));++i)std::this_thread::yield();
        if(!got){pending_.store(true,std::memory_order_release);drainBusy_.clear(std::memory_order_release);return false;}
        summary(log);
        // hash-pinned file: everything before the last line
        sha256_hex(file_,fileLen_,sha_);
        char tail[96];const int tn=std::snprintf(tail,sizeof tail,"sha256=%s\n",sha_);
        if(tn>0&&fileLen_+size_t(tn)<=sizeof file_) {
            std::memcpy(file_+fileLen_,tail,size_t(tn));
            written_=out.write_file(file_,fileLen_+size_t(tn));
            file_[fileLen_]=0;
            if(!written_)++writeFail_;
        } else ++writeFail_;
        Line l;l.f("ps2-probe;event=file;path=logs\\ps2-probe.txt;written=%u;bytes=%zu;sha256=%s;lines=%u",unsigned(written_),fileLen_,sha_,logCount_);
        log.line(true,l.b);
        wrote=written_;
        workBusy_.clear(std::memory_order_release);
    }
    drainBusy_.clear(std::memory_order_release);
    return wrote;
}

} // namespace acz::ee_camera
