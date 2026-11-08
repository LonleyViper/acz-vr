// Lens/FOV Phase 0: READ-ONLY lens scanner. See ee_lens_scan.h and docs/review/lens-scan-impl.md.
// This file never calls Memory::write_float.
#include "ee_lens_scan.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace acz::ee_camera {
namespace {
template<size_t N,class... A> void appendf(char (&buf)[N],size_t& len,const char* fmt,A... a) noexcept {
    if(len>=N-1)return;
    const int w=std::snprintf(buf+len,N-len,fmt,a...);
    if(w>0)len=len+size_t(w)<N?len+size_t(w):N-1;
}
inline unsigned long long ull(uint64_t v) noexcept {return static_cast<unsigned long long>(v);}
inline bool near_to(float v,float target,float tol) noexcept {return std::isfinite(v)&&std::fabs(v-target)<=tol;}
inline bool match_x(unsigned k,float v) noexcept {return near_to(v,lens_kind[k].x,lens_kind[k].tx);}
inline bool match_y(unsigned k,float v) noexcept {return near_to(v,lens_kind[k].y,lens_kind[k].ty);}
inline uint32_t rotr(uint32_t x,unsigned n) noexcept {return (x>>n)|(x<<(32-n));}
constexpr uint32_t K256[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
}

// ------------------------------------------------------------------ SHA-256
Sha256::Sha256() noexcept {
    static constexpr uint32_t init[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::memcpy(h_,init,sizeof h_);std::memset(buf_,0,sizeof buf_);
}
void Sha256::block(const uint8_t* b) noexcept {
    uint32_t w[64];
    for(int i=0;i<16;++i)w[i]=(uint32_t(b[4*i])<<24)|(uint32_t(b[4*i+1])<<16)|(uint32_t(b[4*i+2])<<8)|uint32_t(b[4*i+3]);
    for(int i=16;i<64;++i) {
        const uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3),s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=h_[0],bb=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
    for(int i=0;i<64;++i) {
        const uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25),ch=(e&f)^(~e&g),t1=h+S1+ch+K256[i]+w[i];
        const uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22),mj=(a&bb)^(a&c)^(bb&c),t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=bb;bb=a;a=t1+t2;
    }
    h_[0]+=a;h_[1]+=bb;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=h;
}
void Sha256::update(const void* p,size_t n) noexcept {
    const uint8_t* s=static_cast<const uint8_t*>(p);bytes_+=n;
    while(n) {
        const size_t take=64-fill_<n?64-fill_:n;
        std::memcpy(buf_+fill_,s,take);fill_+=take;s+=take;n-=take;
        if(fill_==64){block(buf_);fill_=0;}
    }
}
void Sha256::final(uint8_t out[32]) noexcept {
    const uint64_t bits_=bytes_*8;
    const uint8_t pad=0x80;update(&pad,1);
    const uint8_t zero=0;while(fill_!=56)update(&zero,1);
    uint8_t len[8];for(int i=0;i<8;++i)len[i]=uint8_t(bits_>>(56-8*i));
    update(len,8);
    for(int i=0;i<8;++i){out[4*i]=uint8_t(h_[i]>>24);out[4*i+1]=uint8_t(h_[i]>>16);out[4*i+2]=uint8_t(h_[i]>>8);out[4*i+3]=uint8_t(h_[i]);}
}
void sha256_hex(const void* p,size_t n,char out[65]) noexcept {
    Sha256 s;s.update(p,n);uint8_t d[32];s.final(d);
    static constexpr char hex[]="0123456789abcdef";
    for(int i=0;i<32;++i){out[2*i]=hex[d[i]>>4];out[2*i+1]=hex[d[i]&15];}
    out[64]=0;
}

// ------------------------------------------------------------------ scanner
LensScanner::LensScanner(Options o) noexcept:o_(o) {
    if(o_.chunk_bytes<256)o_.chunk_bytes=256;
    if(o_.chunk_bytes>lens_chunk_default)o_.chunk_bytes=lens_chunk_default;
    o_.chunk_bytes&=~63u;                                   // multiple of the overlap and of 4
    if(o_.byte_cap>lens_byte_cap)o_.byte_cap=lens_byte_cap;
    if(o_.frames<1)o_.frames=1;
    if(o_.chunks_per_call<1)o_.chunks_per_call=1;
    trackStart_=-1;
    phase_.store(o_.enabled?p_waiting:p_off,std::memory_order_relaxed);
    reason_.store(o_.enabled?"waiting":"not-started",std::memory_order_relaxed);
}

bool LensScanner::terminate(Phase to,const char* why) noexcept {
    uint8_t cur=phase_.load(std::memory_order_acquire);
    for(;;) {
        if(cur<p_waiting||cur>=p_finished)return false;
        if(phase_.compare_exchange_weak(cur,static_cast<uint8_t>(to),std::memory_order_acq_rel,std::memory_order_acquire))break;
    }
    reason_.store(why,std::memory_order_release);
    pending_.store(true,std::memory_order_release);
    return true;
}
void LensScanner::finish(const char* why) noexcept {terminate(p_stopped,why);}

void LensScanner::say(Sink& log,bool important,const char* text) noexcept {
    if(logCount_>=o_.max_log_lines&&!important){++logDropped_;return;}
    ++logCount_;log.line(important,text);
}

void LensScanner::wait_log(Sink& log,double now,const char* why) noexcept {
    if(why==lastWhy_&&now-lastWait_<lens_wait_log_ms)return;
    lastWait_=now;lastWhy_=why;
    char t[224];std::snprintf(t,sizeof t,"lens-scan;event=wait;reason=%s;key=ee_lens_scan",why);
    say(log,false,t);
}

// One sample of one candidate at point A (Present tick) or B (seam). Revalidates the page region identity first: a region
// that vanished, changed base/size/allocation, lost its committed/readable-data state or no longer contains both floats is
// "unreadable" (a stale or reallocated block), never read.
bool LensScanner::sample_one(LensMem& mem,LensCand& c,bool pointB) noexcept {
    LensRegion q;
    const bool idok=mem.query(c.ax,q)&&q.committed&&q.data&&!q.exec&&q.base==c.idBase&&q.size==c.idSize&&q.alloc==c.idAlloc&&
                    c.ax>=q.base&&c.ay>=q.base&&c.ax-q.base+4<=q.size&&c.ay-q.base+4<=q.size;
    if(!idok){++c.identChanged;++c.unread;return false;}
    uint32_t x=0,y=0;
    if(!mem.read(c.ax,&x,4)||!mem.read(c.ay,&y,4)){++c.unread;return false;}
    if(pointB) {
        ++c.bReads;
        if(c.haveB&&(x!=c.bx||y!=c.by))++c.bChanges;
        if(c.haveLast&&(x!=c.lastx||y!=c.lasty))++c.crossDiffs;     // B differs from the last A sample
        c.bx=x;c.by=y;c.haveB=true;
        return true;
    }
    const float fx=from_bits(x),fy=from_bits(y);
    if(c.haveLast&&(x!=c.lastx||y!=c.lasty))++c.changes;
    if(c.haveB&&(x!=c.bx||y!=c.by))++c.crossDiffs;                   // A differs from the last B sample
    if(!(match_x(c.kind,fx)&&match_y(c.kind,fy)))++c.outside;
    if(!c.reads||!(fx>=c.minx))c.minx=fx;
    if(!c.reads||!(fx<=c.maxx))c.maxx=fx;
    if(!c.reads||!(fy>=c.miny))c.miny=fy;
    if(!c.reads||!(fy<=c.maxy))c.maxy=fy;
    c.lastx=x;c.lasty=y;c.haveLast=true;++c.reads;
    return true;
}

void LensScanner::sample_b(LensMem& mem) noexcept {
    if(!o_.enabled||phase()!=p_tracking||bFrame_==lastFrame_||lastFrame_==~0ull)return;
    if(trackBusy_.test_and_set(std::memory_order_acquire))return;
    bFrame_=lastFrame_;
    const unsigned n=n_.load(std::memory_order_acquire);
    for(unsigned i=0;i<n&&i<lens_b_budget;++i)sample_one(mem,c_[i],true);
    trackBusy_.clear(std::memory_order_release);
}

void LensScanner::tick(LensMem& mem,Sink& log,const LensIn& in) noexcept {
    if(!o_.enabled)return;
    Phase p=phase();
    if(p==p_off||p>=p_finished)return;
    if(in.stop_file) {
        if(terminate(p_stopped,"stop-file")){
            char t[160];std::snprintf(t,sizeof t,"lens-scan;event=stopped;reason=stop-file;phase=%u",unsigned(p));say(log,true,t);
        }
        return;
    }
    if(p==p_waiting) {
        if(waitStart_<0)waitStart_=in.now_ms;
        if(!in.v.cockpit) {
            cockpitTicks_=0;cockpitSince_=-1;
            if(in.now_ms-waitStart_>=o_.wait_cap_ms){
                if(terminate(p_aborted,"never-cockpit")){say(log,true,"lens-scan;event=aborted;reason=never-cockpit");}
            } else wait_log(log,in.now_ms,"waiting-for-cockpit");
            return;
        }
        if(in.frame==lastFrame_)return;
        lastFrame_=in.frame;
        if(cockpitSince_<0)cockpitSince_=in.now_ms;
        if(++cockpitTicks_<o_.settle_ticks)return;
        if(!in.v.have&&in.now_ms-cockpitSince_<o_.anchor_grace_ms){wait_log(log,in.now_ms,"waiting-tracks-look-anchor");return;}
        haveAnchor_=in.v.have&&in.v.hi>in.v.lo;
        anchorLo_=haveAnchor_?in.v.lo:0;anchorHi_=haveAnchor_?in.v.hi:0;anchorEff_=haveAnchor_?in.v.eff:0;
        scanArmedMs_=in.now_ms;
        char t[320];
        std::snprintf(t,sizeof t,"lens-scan;event=scan-start;cockpit-ticks=%u;anchor=%s;anchor-eff=%llx;anchor-lo=%llx;anchor-hi=%llx;"
            "caps=bytes:%llu,regions:%u,scan-ms:%.0f,frames:%u,candidates:%u,log-lines:%u;read-only=1;pages=committed-readable-data-only",
            cockpitTicks_,haveAnchor_?"tracks-look":"none",ull(anchorEff_),ull(anchorLo_),ull(anchorHi_),ull(o_.byte_cap),lens_region_cap,
            o_.scan_cap_ms,o_.frames,lens_cand_cap,o_.max_log_lines);
        say(log,true,t);
        set_phase(p_scanning);
        return;
    }
    if(p!=p_tracking)return;
    // ---- tracking: one re-read of every candidate per distinct Present frame while the cockpit is live
    if(in.frame==lastFrame_)return;
    if(trackBusy_.test_and_set(std::memory_order_acquire))return;      // seam B is sampling: skip this tick, not counted
    lastFrame_=in.frame;
    if(trackStart_<0){
        trackStart_=in.now_ms;
        char t[160];std::snprintf(t,sizeof t,"lens-scan;event=tracking-start;candidates=%u;frames=%u",n_.load(std::memory_order_acquire),o_.frames);say(log,true,t);
    }
    if(!in.v.cockpit) {
        ++framesOff_;
        trackBusy_.clear(std::memory_order_release);
        if(in.now_ms-trackStart_>=o_.track_cap_ms)terminate(p_finished,"track-time-cap");
        return;
    }
    const unsigned n=n_.load(std::memory_order_acquire);
    for(unsigned i=0;i<n;++i)sample_one(mem,c_[i],false);
    trackBusy_.clear(std::memory_order_release);
    ++framesTracked_;
    if(framesTracked_>=o_.frames){trackDone_=true;terminate(p_finished,"tracked");return;}
    if(in.now_ms-trackStart_>=o_.track_cap_ms)terminate(p_finished,"track-time-cap");
}

bool LensScanner::build_targets(Sink& log,const LensWorkIn& in) noexcept {
    if(!in.have_blocks)return false;
    targets_=true;nreg_=0;blocksOffered_=in.nblocks;
    auto overlaps=[&](uint64_t lo,uint64_t bytes){
        for(unsigned i=0;i<nreg_;++i)if(lo<reg_[i].lo+reg_[i].bytes&&reg_[i].lo<lo+bytes)return true;
        return false;
    };
    if(haveAnchor_) {
        uint64_t lo=(anchorLo_+3)&~uint64_t(3);
        uint64_t bytes=anchorHi_>lo?anchorHi_-lo:0;
        if(bytes>lens_region_max)bytes=lens_region_max;
        bytes&=~uint64_t(3);
        if(bytes>=lens_pair_window&&lo+bytes>lo){reg_[nreg_].lo=lo;reg_[nreg_].bytes=bytes;reg_[nreg_].anchor=true;++nreg_;}
    }
    for(unsigned i=0;i<in.nblocks;++i) {
        const uint64_t lo=in.blocks[i];
        if(!lo||(lo&3)||lo+lens_region_max<lo||overlaps(lo,lens_region_max))continue;
        if(nreg_>=lens_region_cap){++blocksSkipped_;continue;}                 // region cap: coverage UNKNOWN
        reg_[nreg_].lo=lo;reg_[nreg_].bytes=lens_region_max;reg_[nreg_].anchor=false;++nreg_;
    }
    char t[448];size_t n=0;
    appendf(t,n,"lens-scan;event=targets;regions=%u;blocks-offered=%u;blocks-skipped-by-cap=%u;regions-list=",nreg_,in.nblocks,blocksSkipped_);
    for(unsigned i=0;i<nreg_;++i)appendf(t,n,"%s%llx+%llx%s",i?",":"",ull(reg_[i].lo),ull(reg_[i].bytes),reg_[i].anchor?"(anchor)":"");
    say(log,true,t);
    if(!nreg_){terminate(p_aborted,"no-regions");return false;}
    return true;
}

void LensScanner::add_candidate(unsigned kind,unsigned region,uint64_t addrBase,const float* sc,unsigned nw,unsigned wx,unsigned wy,const LensRegion& q) noexcept {
    ++found_[kind];
    const unsigned n=n_.load(std::memory_order_relaxed);
    if(n>=lens_cand_cap||perKind_[kind]>=lens_kind_cap){++dropped_;return;}
    LensCand& c=c_[n];c=LensCand{};
    c.kind=uint8_t(kind);c.region=region;c.ax=addrBase+uint64_t(wx)*4;c.ay=addrBase+uint64_t(wy)*4;
    c.idBase=q.base;c.idSize=q.size;c.idAlloc=q.alloc;
    std::memcpy(&c.bx0,sc+wx,4);std::memcpy(&c.by0,sc+wy,4);
    for(int j=-int(lens_neigh),k=0;j<=int(lens_neigh);++j) {
        if(!j)continue;
        const long ix=long(wx)+j,iy=long(wy)+j;
        if(ix>=0&&ix<long(nw))std::memcpy(&c.nbx[k],sc+ix,4);
        if(iy>=0&&iy<long(nw))std::memcpy(&c.nby[k],sc+iy,4);
        ++k;
    }
    // Representation + surrounding structure evidence (all heuristics, none is proof):
    //  finite/zero words among +-8 of x, a 4x4 matrix diagonal shape (y exactly 5 words after x with 4 zeros between), and the number
    //  of words within +-16 that are the reciprocal of x (a tan/scale partner).
    const float fx=sc[wx];unsigned fin=0,zer=0,rec=0;
    for(int j=int(wx)-8;j<=int(wx)+8;++j) {
        if(j<0||j>=int(nw)||j==int(wx)||j==int(wy))continue;
        uint32_t u;std::memcpy(&u,sc+j,4);
        if(std::isfinite(sc[j]))++fin;
        if(!(u&0x7FFFFFFFu))++zer;
    }
    for(int j=int(wx)-16;j<=int(wx)+16;++j)
        if(j>=0&&j<int(nw)&&j!=int(wx)&&j!=int(wy)&&std::isfinite(sc[j])&&fx!=0.f&&std::fabs(double(sc[j])*double(fx)-1.0)<=3e-3)++rec;
    c.finiteN=uint8_t(fin);c.zeroN=uint8_t(zer);c.recip=uint8_t(rec>255?255:rec);
    if(wy==wx+5&&wy<nw) {
        bool z=true;for(unsigned j=wx+1;j<wy;++j){uint32_t u;std::memcpy(&u,sc+j,4);z=z&&!(u&0x7FFFFFFFu);}
        c.diag4x4=z?1:0;
    }
    c.minx=c.maxx=from_bits(c.bx0);c.miny=c.maxy=from_bits(c.by0);
    ++perKind_[kind];
    n_.store(n+1,std::memory_order_release);
}

void LensScanner::note_reject(uint64_t a,uint64_t n,bool queried,const LensRegion& q,const char* reason) noexcept {
    if(queried&&q.state) {
        const LensPageClass c=lens_classify(q.state,q.type,q.protect);
        if(c!=lpc_data)++rejByClass_[c];else ++rejOther_;
    } else ++rejOther_;
    if(firstRej_.have)return;
    firstRej_.have=true;firstRej_.addr=a;firstRej_.len=n;firstRej_.reason=reason;
    if(queried){firstRej_.qbase=q.base;firstRej_.qsize=q.size;firstRej_.qalloc=q.alloc;firstRej_.state=q.state;firstRej_.type=q.type;firstRej_.protect=q.protect;}
}

bool LensScanner::scan_step(LensMem& mem) noexcept {
    const Region& R=reg_[ri_];
    const uint64_t len=R.bytes,pos=pos_;
    uint64_t primEnd=pos+o_.chunk_bytes;
    if(primEnd>len)primEnd=len;
    if(o_.byte_cap>scanned_&&primEnd-pos>o_.byte_cap-scanned_)primEnd=pos+(o_.byte_cap-scanned_);
    const uint64_t start=pos>=lens_pair_window?pos-lens_pair_window:0;
    uint64_t end=primEnd+lens_pair_window;if(end>len)end=len;
    ++chunks_;
    // Only committed readable DATA pages; the whole window must lie inside ONE queried region (checked arithmetic, no partial reads).
    LensRegion q;const uint64_t a=R.lo+start,n=end-start;
    bool ok=mem.query(a,q);
    if(!ok)note_reject(a,n,false,q,"query-failed");
    if(ok&&(!q.committed||!q.data||q.exec)){ok=false;++excludedChunks_;note_reject(a,n,true,q,q.state?lens_pageclass_name(lens_classify(q.state,q.type,q.protect)):"not-committed-or-not-data");}
    if(ok){ok=a>=q.base&&a-q.base<=q.size&&n<=q.size-(a-q.base);if(!ok)note_reject(a,n,true,q,"window-not-inside-one-region");}
    if(ok){ok=mem.read(a,scratch_,uint32_t(n));if(!ok)note_reject(a,n,true,q,"read-failed");}
    if(!ok){++unreadable_;unreadableBytes_+=primEnd-pos;}
    else {
        const unsigned nw=unsigned(n/4),w0=unsigned((pos-start)/4),w1=unsigned((primEnd-start)/4);
        constexpr int win=int(lens_pair_window/4);
        for(unsigned w=w0;w<w1;++w) {
            const float f=scratch_[w];
            if(!(f>=lens_prefilter_lo&&f<=lens_prefilter_hi))continue;
            for(unsigned k=0;k<lk_kinds;++k) {
                if(match_y(k,f))++singles_[k][1];
                if(!match_x(k,f))continue;
                ++singles_[k][0];
                for(int j=int(w)-win;j<=int(w)+win;++j) {
                    if(j<0||j>=int(nw)||j==int(w))continue;
                    if(match_y(k,scratch_[j]))add_candidate(k,ri_,a,scratch_,nw,w,unsigned(j),q);
                }
            }
        }
    }
    scanned_+=primEnd-pos;
    pos_=primEnd;
    return pos_<len&&primEnd>pos;
}

void LensScanner::work(LensMem& mem,Sink& log,const LensWorkIn& in) noexcept {
    if(!o_.enabled||phase()!=p_scanning)return;
    if(in.stop_file) {
        if(terminate(p_stopped,"stop-file")){say(log,true,"lens-scan;event=stopped;reason=stop-file;phase=scanning");}
        return;
    }
    if(scanStart_<0)scanStart_=in.now_ms;
    if(!targets_&&!build_targets(log,in))return;
    auto end_scan=[&](const char* why){
        scanComplete_=std::strcmp(why,"complete")==0;scanStop_=why;scanMs_=in.now_ms-scanStart_;
        char t[400];std::snprintf(t,sizeof t,"lens-scan;event=scan-done;stop=%s;complete=%u;regions=%u;bytes=%llu;chunks=%u;unreadable-chunks=%u;unreadable-bytes=%llu;excluded-chunks=%u;ms=%.0f;candidates=%u;"
            "found=tan:%u,scale:%u,tan16:%u,tan64:%u,angle-rad:%u,angle-deg:%u;dropped=%u",why,unsigned(scanComplete_),nreg_,ull(scanned_),chunks_,unreadable_,ull(unreadableBytes_),excludedChunks_,scanMs_,
            n_.load(std::memory_order_relaxed),found_[0],found_[1],found_[2],found_[3],found_[4],found_[5],dropped_);
        say(log,true,t);
        if(firstRej_.have) {
            const Reject& r=firstRej_;
            char u[640];std::snprintf(u,sizeof u,"lens-scan;event=first-reject;reason=%s;window=%llx+%llx;query-base=%llx;query-size=%llx;query-alloc=%llx;state=%x;type=%x;protect=%x;"
                "class-counts=%s:%u,%s:%u,%s:%u,%s:%u,%s:%u,%s:%u,%s:%u,other:%u",r.reason,ull(r.addr),ull(r.len),ull(r.qbase),ull(r.qsize),ull(r.qalloc),r.state,r.type,r.protect,
                lens_pageclass_name(lpc_not_committed),rejByClass_[lpc_not_committed],lens_pageclass_name(lpc_no_access),rejByClass_[lpc_no_access],lens_pageclass_name(lpc_guard),rejByClass_[lpc_guard],
                lens_pageclass_name(lpc_executable),rejByClass_[lpc_executable],lens_pageclass_name(lpc_image),rejByClass_[lpc_image],
                lens_pageclass_name(lpc_wc_nocache_not_private),rejByClass_[lpc_wc_nocache_not_private],lens_pageclass_name(lpc_other_protect),rejByClass_[lpc_other_protect],rejOther_);
            say(log,true,u);
        }
        if(n_.load(std::memory_order_relaxed)==0){terminate(p_finished,"zero-hits");return;}
        uint8_t exp=p_scanning;
        phase_.compare_exchange_strong(exp,static_cast<uint8_t>(p_tracking),std::memory_order_acq_rel,std::memory_order_acquire);
    };
    for(unsigned k=0;k<o_.chunks_per_call;++k) {
        if(phase()!=p_scanning)return;                                   // ended from the Present thread (stop file)
        if(scanned_>=o_.byte_cap){end_scan("byte-cap");return;}
        if(in.now_ms-scanStart_>=o_.scan_cap_ms){end_scan("time-cap");return;}
        if(!scan_step(mem)) {
            ++ri_;pos_=0;
            if(ri_>=nreg_){end_scan(scanned_>=o_.byte_cap?"byte-cap":"complete");return;}
        }
    }
}

void LensScanner::classify() noexcept {
    const unsigned n=n_.load(std::memory_order_acquire);
    for(unsigned i=0;i<n;++i) {
        LensCand& c=c_[i];
        if(c.identChanged>0||(c.reads==0&&framesTracked_))c.cls=lc_unreadable;
        else if(c.reads==0)c.cls=lc_untracked;
        else if(c.outside>0)c.cls=lc_unstable;
        else if(c.crossDiffs>0)c.cls=lc_point_varies;
        else if(c.changes>0)c.cls=lc_stable;
        else c.cls=lc_static;
    }
}

size_t LensScanner::compose(char* buf,size_t cap,Sink* log) noexcept {
    classify();
    size_t len=0;
    const bool doLog=log&&!reportLogged_;
    auto emit=[&](const char* line,bool essential){
        const size_t n=std::strlen(line);
        if(len+n+1<=cap){std::memcpy(buf+len,line,n);buf[len+n]='\n';len+=n+1;}
        else truncated_=true;
        if(doLog){
            if(logCount_<o_.max_log_lines||essential){++logCount_;log->line(false,line);}
            else ++logDropped_;
        }
    };
    char t[448];size_t n=0;
    const unsigned nc=n_.load(std::memory_order_acquire);
    const bool complete=trackDone_&&framesTracked_>=o_.frames;
    unsigned total=0;for(unsigned k=0;k<lk_kinds;++k)total+=found_[k];
    // Coverage: ANY cap, unreadable/excluded chunk, skipped block or early end makes the coverage UNKNOWN (never "absent").
    char cov[256];size_t cn=0;cov[0]=0;
    auto why=[&](const char* s){appendf(cov,cn,"%s%s",cn?",":"",s);};
    if(!nreg_)why("no-regions");
    if(!scanComplete_)why(scanStop_[0]&&std::strcmp(scanStop_,"running")?scanStop_:"scan-ended-early");
    if(unreadable_)why("unreadable-chunks");
    if(excludedChunks_)why("excluded-nondata-pages");
    if(blocksSkipped_)why("region-cap");
    if(dropped_)why("candidate-cap");
    if(!haveAnchor_)why("no-anchor");
    const bool covKnown=cov[0]==0;
    emit("lens-scan;event=file-header;format=acz-lens-scan-candidates-v2;plan=lens-fov-widen-plan-phase0;writes-game-memory=0;pages=committed-readable-data-only;statement=\"candidate only; target/consumer unidentified\"",true);
    n=0;appendf(t,n,"lens-scan;event=summary;reason=%s;phase=%u;tracking-complete=%u;scan-complete=%u;scan-stop=%s;scan-ms=%.0f;scanned-bytes=%llu;chunks=%u;"
        "unreadable-chunks=%u;unreadable-bytes=%llu;excluded-chunks=%u;frames-tracked=%u;frames-requested=%u;frames-not-cockpit=%u;candidates=%u;dropped=%u",
        reason(),unsigned(phase()),unsigned(complete),unsigned(scanComplete_),scanStop_,scanMs_,ull(scanned_),chunks_,unreadable_,ull(unreadableBytes_),excludedChunks_,
        framesTracked_,o_.frames,framesOff_,nc,dropped_);
    emit(t,true);
    n=0;appendf(t,n,"lens-scan;event=coverage;known=%u;unknown-because=%s;sampling=every-4-byte-aligned-float32-pair-within-64B;regions=tracks-look-block+private-rw-64MiB-blocks-only",
        unsigned(covKnown),covKnown?"-":cov);
    emit(t,true);
    n=0;appendf(t,n,"lens-scan;event=anchor;have=%u;eff=%llx;lo=%llx;hi=%llx",unsigned(haveAnchor_),ull(anchorEff_),ull(anchorLo_),ull(anchorHi_));
    emit(t,false);
    for(unsigned i=0;i<nreg_;++i) {
        n=0;appendf(t,n,"lens-scan;event=region;index=%u;lo=%llx;bytes=%llx;anchor=%u",i,ull(reg_[i].lo),ull(reg_[i].bytes),unsigned(reg_[i].anchor));
        emit(t,false);
    }
    for(unsigned k=0;k<lk_kinds;++k) {
        n=0;appendf(t,n,"lens-scan;event=kind;kind=%s;repr=float32;x=%.5g;y=%.5g;tol-x=%.3g;tol-y=%.3g;x-singles=%u;y-singles=%u;pairs=%u",lens_kind[k].name,
            double(lens_kind[k].x),double(lens_kind[k].y),double(lens_kind[k].tx),double(lens_kind[k].ty),singles_[k][0],singles_[k][1],found_[k]);
        emit(t,false);
    }
    // Duplicate groups: identical kind, values, dy and neighbours = copies/mirrors of one thing (stale copies are indistinguishable by value).
    int grp[lens_cand_cap];unsigned grpSize[lens_cand_cap]{};
    for(unsigned i=0;i<nc;++i) {
        grp[i]=int(i);
        for(unsigned j=0;j<i;++j) {
            const LensCand& a=c_[i];const LensCand& b=c_[j];
            if(a.kind==b.kind&&a.bx0==b.bx0&&a.by0==b.by0&&a.ay-a.ax==b.ay-b.ax&&!std::memcmp(a.nbx,b.nbx,sizeof a.nbx)&&!std::memcmp(a.nby,b.nby,sizeof a.nby)){grp[i]=grp[j];break;}
        }
        ++grpSize[grp[i]];
    }
    unsigned tanGroups=0,tanNumStable=0;
    for(unsigned i=0;i<nc;++i) {
        const LensCand& c=c_[i];
        const bool numStable=c.cls==lc_static||c.cls==lc_stable;
        if(c.kind==lk_tan){ if(numStable)++tanNumStable; if(grp[i]==int(i))++tanGroups; }
        const uint64_t lo=c.region<nreg_?reg_[c.region].lo:0;
        const long long dy=static_cast<long long>(c.ay)-static_cast<long long>(c.ax);
        n=0;appendf(t,n,"lens-scan;event=candidate;id=%u;kind=%s;repr=float32;x-at=%llx;y-at=%llx;dy=%+lld;region=%u;off=+0x%llX;eff-rel=%+lld;vx=%.6g;vy=%.6g;class=%s;reads=%u;unread=%u;ident-changed=%u;changes=%u;outside=%u;x-range=%.6g..%.6g;y-range=%.6g..%.6g",
            i,lens_kind[c.kind].name,ull(c.ax),ull(c.ay),dy,c.region,ull(c.ax-lo),
            haveAnchor_&&anchorEff_?static_cast<long long>(c.ax)-static_cast<long long>(anchorEff_)-static_cast<long long>(field_offset[look_yaw]):0ll,
            double(from_bits(c.bx0)),double(from_bits(c.by0)),lens_class_name(c.cls),c.reads,c.unread,c.identChanged,c.changes,c.outside,
            double(c.minx),double(c.maxx),double(c.miny),double(c.maxy));
        emit(t,false);
        n=0;appendf(t,n,"lens-scan;event=candidate-evidence;id=%u;repr=float32;b-reads=%u;b-changes=%u;cross-point-diffs=%u;struct=finite:%u,zero:%u,diag4x4:%u,recip:%u;dup-group=%d;copies=%u;"
            "same-value-rewrite=undetectable-by-reads",i,c.bReads,c.bChanges,c.crossDiffs,c.finiteN,c.zeroN,c.diag4x4,c.recip,grp[i],grpSize[grp[i]]);
        emit(t,false);
        n=0;appendf(t,n,"lens-scan;event=neighbours;id=%u;before-x3-after-x3=",i);
        for(unsigned j=0;j<2*lens_neigh;++j)appendf(t,n,"%s%.6g",j?",":"",double(from_bits(c.nbx[j])));
        appendf(t,n,";y-before-x3-after-x3=");
        for(unsigned j=0;j<2*lens_neigh;++j)appendf(t,n,"%s%.6g",j?",":"",double(from_bits(c.nby[j])));
        emit(t,false);
    }
    // The verdict is a numerical SUMMARY only. Equal samples cannot separate an immutable input, a stale copy and an output rewritten
    // to the same value, and nothing here identifies the write target or the consumer.
    const char* result;
    if(total==0)result="zero-hits";
    else if(found_[lk_tan]==0)result="no-tan-pair-see-other-kinds";
    else if(tanNumStable==0)result="tan-pairs-but-none-numerically-stable";
    else if(tanGroups==1&&found_[lk_tan]==1)result="one-numerically-stable-tan-pair";
    else if(tanGroups==1)result="one-tan-pair-with-copies";
    else result="several-tan-pairs";
    n=0;appendf(t,n,"lens-scan;event=verdict;advisory=1;result=%s;statement=\"candidate only; target/consumer unidentified\";phase1-admission=not-implied-by-numerical-stability;"
        "tan-pairs=%u;tan-groups=%u;tan-numerically-stable=%u;total-pairs=%u;coverage=%s;partial=%u;log-lines-dropped=%u;file-truncated=%u",
        result,found_[lk_tan],tanGroups,tanNumStable,total,covKnown?"complete-for-these-ranges":"UNKNOWN",unsigned(!complete),logDropped_,unsigned(truncated_));
    emit(t,true);
    if(total==0) {
        n=0;appendf(t,n,"lens-scan;event=zero-hits;statement=\"not found within these representations/ranges/sampling limits\";coverage=%s",covKnown?"complete-for-these-ranges":"UNKNOWN");
        emit(t,true);
    }
    n=0;appendf(t,n,"lens-scan;event=missing-evidence;needed-before-any-write=live-vs-copy,write-target,consumer-identity,rewrite-behaviour;note=identical-samples-cannot-separate-immutable-input/stale-copy/output-rewritten-to-same-value");
    emit(t,true);
    if(doLog)reportLogged_=true;
    return len;
}

bool LensScanner::drain(Sink& log,LensOut& out) noexcept {
    if(!pending_.load(std::memory_order_acquire))return false;
    if(drainBusy_.test_and_set(std::memory_order_acquire))return false;
    bool wrote=false;
    if(pending_.load(std::memory_order_acquire)) {
        size_t len=compose(file_,lens_file_cap-96,&log);
        char hex[65];sha256_hex(file_,len,hex);
        std::memcpy(sha_,hex,65);
        char tail[96];const int tn=std::snprintf(tail,sizeof tail,"sha256=%s\n",hex);
        std::memcpy(file_+len,tail,size_t(tn));
        const size_t total=len+size_t(tn);
        char t[448];
        if(out.write_file(file_,total)) {
            written_=true;wrote=true;pending_.store(false,std::memory_order_release);
            std::snprintf(t,sizeof t,"lens-scan;event=file;name=lens-scan-candidates.txt;bytes=%llu;sha256=%s;sha256-covers=all-bytes-before-the-sha256-line;candidates=%u;reason=%s;truncated=%u",
                ull(total),hex,n_.load(std::memory_order_acquire),reason(),unsigned(truncated_));
            log.line(true,t);
        } else {
            ++writeFail_;
            std::snprintf(t,sizeof t,"lens-scan;event=file-write-failed;attempt=%u;sha256=%s;bytes=%llu",writeFail_,hex,ull(total));
            log.line(true,t);
            if(writeFail_>=3)pending_.store(false,std::memory_order_release);   // give up (the lines above are the record)
        }
    }
    drainBusy_.clear(std::memory_order_release);
    return wrote;
}

} // namespace acz::ee_camera
