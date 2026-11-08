// Lens/FOV widen plan, phase 0: offline fixtures for the READ-ONLY lens scanner (ee_lens_scan.h) against synthetic memory
// images with planted candidates, noise, unstable values and caps. Nothing here touches a process or the game.
#include "ee_lens_scan.h"
#include "runtime_config.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace acz::ee_camera;
using acz::diagnostics::Config;
using acz::diagnostics::ConfigIssue;
using acz::diagnostics::parse_config;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)

// ------------------------------------------------------------------ fakes
// A memory image made of segments [base, base+size) whose first words are given; everything beyond is zero.
struct Seg {
    uint64_t base=0,size=0;std::vector<uint32_t> w;
    bool committed=true,data=true,exec=false;uint32_t rstate=0,rtype=0,rprot=0;uint64_t alloc=0;   // rstate/rtype/rprot: raw VirtualQuery values (0 = use the flags)
             // page attributes the fake VirtualQuery reports (alloc 0 = base)
    std::vector<uint64_t> splits;uint64_t badFrom=~0ull;                  // extra region boundaries; addresses >= badFrom are not readable data
};
struct Mem final:LensMem {
    std::vector<Seg> segs;std::vector<std::pair<uint64_t,uint64_t>> unreadable;   // [a, b) ranges whose reads fail
    unsigned reads=0,writeCalls=0;
    Seg& add(uint64_t base,uint64_t size,size_t words) {segs.push_back(Seg{base,size,std::vector<uint32_t>(words,0)});return segs.back();}
    void put(uint64_t addr,float v) {put_bits(addr,bits(v));}
    void put_bits(uint64_t addr,uint32_t b) {
        for(Seg& s:segs)if(addr>=s.base&&addr+4<=s.base+s.size) {
            const size_t i=size_t((addr-s.base)/4);if(i>=s.w.size())s.w.resize(i+1,0);s.w[i]=b;return;
        }
        std::abort();
    }
    uint32_t get_bits(uint64_t addr) const {
        for(const Seg& s:segs)if(addr>=s.base&&addr+4<=s.base+s.size){const size_t i=size_t((addr-s.base)/4);return i<s.w.size()?s.w[i]:0;}
        return 0;
    }
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        ++reads;
        for(auto& r:unreadable)if(a<r.second&&a+n>r.first)return false;
        for(const Seg& s:segs) {
            if(a<s.base||a+n>s.base+s.size)continue;
            uint8_t* o=static_cast<uint8_t*>(out);std::memset(o,0,n);
            const uint64_t off=a-s.base,have=uint64_t(s.w.size())*4;
            if(off<have){const uint64_t c=std::min<uint64_t>(n,have-off);std::memcpy(o,reinterpret_cast<const char*>(s.w.data())+off,size_t(c));}
            return true;
        }
        return false;
    }
    bool query(uint64_t a,LensRegion& o) noexcept override {
        ++queries;
        for(const Seg& s:segs) {
            if(a<s.base||a>=s.base+s.size)continue;
            uint64_t lo=s.base,hi=s.base+s.size;
            auto sp=s.splits;if(s.badFrom!=~0ull)sp.push_back(s.badFrom);
            for(uint64_t b:sp){if(b<=a)lo=std::max(lo,b);else hi=std::min(hi,b);}
            o.base=lo;o.size=hi-lo;o.alloc=s.alloc?s.alloc:s.base;o.committed=s.committed;o.exec=s.exec;
            o.data=s.committed&&s.data&&!s.exec&&a<s.badFrom;
            if(s.rstate){o.state=s.rstate;o.type=s.rtype;o.protect=s.rprot;o.committed=s.rstate==lw_mem_commit;const LensPageClass c=lens_classify(s.rstate,s.rtype,s.rprot);o.exec=c==lpc_executable;o.data=c==lpc_data&&a<s.badFrom;}
            return true;
        }
        return false;
    }
    bool write_float(uint64_t,float) noexcept override {++writeCalls;return false;}   // must never be called
    unsigned queries=0;
    uint64_t checksum() const {uint64_t h=1469598103934665603ull;for(const Seg& s:segs)for(uint32_t w:s.w){h^=w;h*=1099511628211ull;}return h;}
};
struct Sink2 final:Sink {
    std::vector<std::string> lines;
    void line(bool,const char* t) noexcept override {lines.emplace_back(t);if(std::getenv("EE_LENS_VERBOSE"))std::printf("  %s\n",t);}
    unsigned count(const char* s) const {unsigned n=0;for(auto& l:lines)n+=l.find(s)!=std::string::npos;return n;}
    const std::string* find(const char* s) const {for(auto& l:lines)if(l.find(s)!=std::string::npos)return &l;return nullptr;}
    size_t longest() const {size_t m=0;for(auto& l:lines)m=std::max(m,l.size());return m;}
};
struct Out final:LensOut {
    std::vector<std::string> files;unsigned calls=0,failFirst=0;bool failAll=false;
    bool write_file(const char* t,size_t n) noexcept override {++calls;if(failAll||calls<=failFirst)return false;files.emplace_back(t,n);return true;}
};

constexpr uint64_t B0=0x7000000000ull;
LensScanner::Options opts(unsigned frames=40) {
    LensScanner::Options o;o.enabled=true;o.chunk_bytes=4096;o.frames=frames;o.settle_ticks=3;o.chunks_per_call=4;return o;
}
struct Run {
    Mem mem;Sink2 sink;LensScanner sc;Out out;
    double now=1000;uint64_t frame=1;bool cockpit=true,have=true,stop=false;uint64_t alo=B0,ahi=B0+(1u<<20),aeff=B0+0x100;
    std::vector<uint64_t> blocks;
    explicit Run(LensScanner::Options o=opts()):sc(o) {mem.add(B0,1u<<20,0);}
    LensIn in() const {LensIn i;i.now_ms=now;i.frame=frame;i.stop_file=stop;i.v.cockpit=cockpit;i.v.have=have;i.v.lo=alo;i.v.hi=ahi;i.v.eff=aeff;return i;}
    void tick(){sc.tick(mem,sink,in());now+=1000.0/60;++frame;}
    void work(){
        LensWorkIn w;w.now_ms=now;w.stop_file=stop;w.have_blocks=true;w.blocks=blocks.data();w.nblocks=unsigned(blocks.size());
        sc.work(mem,sink,w);now+=5;
    }
    void settle(){for(unsigned g=0;g<100000&&sc.phase()==LensScanner::p_waiting;++g)tick();}
    void scan(){for(unsigned g=0;g<2000000&&sc.phase()==LensScanner::p_scanning;++g)work();}
    void track(const std::function<void(unsigned)>& per=nullptr,unsigned maxFrames=100000){
        for(unsigned f=0;f<maxFrames&&sc.phase()==LensScanner::p_tracking;++f){if(per)per(f);tick();}
    }
    void all(const std::function<void(unsigned)>& per=nullptr){settle();scan();track(per);}
    bool drain(){return sc.drain(sink,out);}
    const LensCand* at(uint64_t ax) const {for(unsigned i=0;i<sc.candidates();++i)if(sc.candidate(i).ax==ax)return &sc.candidate(i);return nullptr;}
};
#define RUN0(n) std::unique_ptr<Run> n##_p(new Run()); Run& n=*n##_p
#define RUN(n,a) std::unique_ptr<Run> n##_p(new Run(a)); Run& n=*n##_p
static std::string last_line(const std::string& f,size_t& start) {
    size_t e=f.size();if(e&&f[e-1]=='\n')--e;
    size_t s=f.rfind('\n',e?e-1:0);start=(s==std::string::npos)?0:s+1;
    return f.substr(start,e-start);
}
static std::string hex_of(const std::string& s){char h[65];sha256_hex(s.data(),s.size(),h);return h;}

// ------------------------------------------------------------------ tests
static void test_sha() {
    CHECK(hex_of("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex_of("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex_of(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for(size_t n:{54u,55u,56u,57u,63u,64u,65u,119u,120u,128u,200u}) {          // incremental == one shot across block edges
        std::string s;for(size_t i=0;i<n;++i)s.push_back(char('a'+i%26));
        Sha256 a;for(size_t i=0;i<n;i+=7)a.update(s.data()+i,std::min<size_t>(7,n-i));
        uint8_t d[32];a.final(d);char h[65];static const char* hx="0123456789abcdef";for(int i=0;i<32;++i){h[2*i]=hx[d[i]>>4];h[2*i+1]=hx[d[i]&15];}h[64]=0;
        CHECK(hex_of(s)==h);
    }
}

static void test_pairs() {
    RUN0(r);
    const uint64_t base=B0;
    auto A=[&](unsigned w){return base+uint64_t(w)*4;};
    // 1 tan adjacent, with distinctive neighbours
    r.mem.put(A(100),0.6152f);r.mem.put(A(101),0.3460f);
    for(int j=-3;j<=3;++j){if(j&&j!=1)r.mem.put(A(100+j),10.f+float(j));}
    r.mem.put(A(104),99.5f);
    // 2 scale, REVERSED order (y first), 8 bytes apart
    r.mem.put(A(300),2.8902f);r.mem.put(A(302),1.6255f);
    // 3 angle-rad exactly 64 B apart: found. 4 angle-rad 68 B apart: not found
    r.mem.put(A(500),1.1030f);r.mem.put(A(516),0.6663f);
    r.mem.put(A(700),1.1030f);r.mem.put(A(717),0.6663f);
    // 5 tan16, 6 tan64, 7 angle-deg
    r.mem.put(A(900),9.8432f);r.mem.put(A(901),5.5360f);
    r.mem.put(A(1100),39.3728f);r.mem.put(A(1103),22.144f);
    r.mem.put(A(1300),63.2f);r.mem.put(A(1301),38.1f);
    // near misses: x outside 1e-3 (0.6165), y inside (0.3465 < 1e-3? 0.3465-0.3460=5e-4 -> inside)
    r.mem.put(A(1500),0.6165f);r.mem.put(A(1501),0.3460f);
    r.mem.put(A(1700),0.6161f);r.mem.put(A(1701),0.3465f);                 // both inside tolerance: found
    // non-finite and single values
    r.mem.put_bits(A(1900),0x7fc00000);r.mem.put(A(1901),0.3460f);          // NaN + y only
    r.mem.put(A(2000),0.6152f);                                              // x only
    r.mem.put_bits(A(2100),0x7f800000);r.mem.put_bits(A(2101),0xff800000);
    r.settle();r.scan();
    CHECK(r.sc.phase()==LensScanner::p_tracking);
    CHECK(r.sc.scan_complete());
    CHECK(r.sc.found(lk_tan)==2&&r.sc.found(lk_scale)==1&&r.sc.found(lk_angle_rad)==1&&r.sc.found(lk_tan16)==1&&r.sc.found(lk_tan64)==1&&r.sc.found(lk_angle_deg)==1);
    CHECK(r.sc.candidates()==7);
    const LensCand* c=r.at(A(100));
    CHECK(c&&c->kind==lk_tan&&c->ay==A(101)&&c->bx0==bits(0.6152f)&&c->by0==bits(0.3460f));
    if(c) {   // neighbours: 3 before / 3 after the x (index 0..2 before, 3..5 after); the word after x is y itself
        CHECK(from_bits(c->nbx[0])==10.f-3.f&&from_bits(c->nbx[2])==10.f-1.f&&from_bits(c->nbx[3])==0.3460f&&from_bits(c->nbx[4])==12.f&&from_bits(c->nbx[5])==from_bits(r.mem.get_bits(A(103))));
        CHECK(from_bits(c->nby[0])==from_bits(r.mem.get_bits(A(98)))&&from_bits(c->nby[2])==0.6152f);
    }
    c=r.at(A(302));   // reversed: the candidate is keyed by the x address (the 1.6255 at 302), y precedes it
    CHECK(c&&c->kind==lk_scale&&c->ay==A(300)&&int64_t(c->ay)-int64_t(c->ax)==-8);
    c=r.at(A(500));CHECK(c&&c->kind==lk_angle_rad&&c->ay==A(516));
    CHECK(!r.at(A(700)));                                                    // 68 bytes apart
    CHECK(r.at(A(900))&&r.at(A(900))->kind==lk_tan16&&r.at(A(1100))&&r.at(A(1100))->kind==lk_tan64&&r.at(A(1300))&&r.at(A(1300))->kind==lk_angle_deg);
    CHECK(!r.at(A(1500)));CHECK(r.at(A(1700))&&r.at(A(1700))->kind==lk_tan);
    CHECK(!r.at(A(1900))&&!r.at(A(2000))&&!r.at(A(2100)));
    CHECK(r.mem.writeCalls==0);
    r.track();
    CHECK(r.sc.phase()==LensScanner::p_finished&&std::strcmp(r.sc.reason(),"tracked")==0);
}

static void test_chunk_edges() {
    auto o=opts();o.chunk_bytes=256;
    RUN(r,o);
    auto A=[&](unsigned w){return B0+uint64_t(w)*4;};
    // chunk = 64 words. x at the last word of chunk 0, y first word of chunk 1; x first word of chunk 2 with y at the last word of chunk 1
    r.mem.put(A(63),0.6152f);r.mem.put(A(64),0.3460f);
    r.mem.put(A(127),0.3460f);r.mem.put(A(128),0.6152f);
    // region start and end (anchor = [B0, B0+4096))
    r.mem.put(A(0),1.6255f);r.mem.put(A(1),2.8902f);
    r.mem.put(A(1022),1.1030f);r.mem.put(A(1023),0.6663f);
    r.ahi=B0+4096;
    r.settle();r.scan();
    CHECK(r.sc.candidates()==4);
    CHECK(r.at(A(63))&&r.at(A(128))&&r.at(A(0))&&r.at(A(1022)));
    CHECK(r.sc.scanned_bytes()==4096);
}

static void test_noise() {
    {   // pure random bit patterns: nothing in the prefilter range pairs up -> zero hits, an honest "abort the experiment" report
        RUN0(r);
        uint32_t rng=12345;
        auto next=[&]{rng=rng*1664525u+1013904223u;return rng;};
        Seg& s=r.mem.segs[0];s.w.assign((1u<<20)/4,0);
        for(size_t i=0;i<s.w.size();++i)s.w[i]=next();
        r.settle();r.scan();
        CHECK(r.sc.candidates()==0);
        CHECK(r.sc.phase()==LensScanner::p_finished&&std::strcmp(r.sc.reason(),"zero-hits")==0);
        CHECK(r.drain());
        CHECK(r.sink.find("result=zero-hits")!=nullptr);
        CHECK(r.mem.writeCalls==0);
    }
    {   // sparse floats in the prefilter range [0.3, 64): the narrow kinds do not fire, the stored candidates stay bounded
        RUN0(r);
        uint32_t rng=999;
        auto next=[&]{rng=rng*1664525u+1013904223u;return rng;};
        Seg& s=r.mem.segs[0];s.w.assign((1u<<20)/4,0);
        for(size_t i=0;i<s.w.size();i+=8)s.w[i]=bits(0.3f+float(next()%6370)/100.f);
        r.settle();r.scan();
        CHECK(r.sc.found(lk_tan)==0&&r.sc.found(lk_scale)==0&&r.sc.found(lk_angle_rad)==0);
        CHECK(r.sc.candidates()<=lens_cand_cap);
        CHECK(r.sc.scan_complete()&&r.mem.writeCalls==0);
    }
}

static void plant_tan(Mem& m,uint64_t a){m.put(a,0.6152f);m.put(a+4,0.3460f);}
static void test_tracking_classes() {
    RUN(r,opts(60));
    const uint64_t st=B0+0x1000,jit=B0+0x2000,uns=B0+0x3000,gone=B0+0x4000,flick=B0+0x5000;
    for(uint64_t a:{st,jit,uns,gone,flick})plant_tan(r.mem,a);
    r.mem.segs[0].size=1u<<20;
    r.settle();r.scan();
    CHECK(r.sc.candidates()==5);
    unsigned reinit=0;
    r.track([&](unsigned f){
        ++reinit;
        // jitter: +-1 ulp each frame (always inside tolerance)
        r.mem.put_bits(jit,bits(0.6152f)+(f&1));
        // unstable: leaves tolerance on 10 of 60 frames
        r.mem.put(uns,(f%6==0)?0.8f:0.6152f);
        // gone: becomes unreadable from the first tracked frame on
        if(f==0)r.mem.unreadable.push_back({gone,gone+8});
        // flicker: one read-failing frame only affects unread, the rest static
        if(f==5)r.mem.unreadable.push_back({flick,flick+4});
        if(f==6)r.mem.unreadable.pop_back();
        // note: pop_back above removes the LAST range, which at that time is the flicker range (gone stays)
    });
    CHECK(r.sc.phase()==LensScanner::p_finished&&r.sc.frames_tracked()==60);
    CHECK(reinit==60);
    const LensCand* a=r.at(st);const LensCand* b=r.at(jit);const LensCand* c=r.at(uns);const LensCand* d=r.at(gone);const LensCand* e=r.at(flick);
    CHECK(a&&a->cls==0);   // not classified until compose()
    std::vector<char> buf(lens_file_cap);
    r.sc.compose(buf.data(),buf.size(),nullptr);
    CHECK(a->cls==lc_static&&a->reads==60&&a->changes==0&&a->outside==0);
    CHECK(b->cls==lc_stable&&b->reads==60&&b->changes==59&&b->outside==0);
    CHECK(c->cls==lc_unstable&&c->outside==10&&c->changes>0&&c->maxx==0.8f);
    CHECK(d->cls==lc_unreadable&&d->reads==0&&d->unread==60);
    CHECK(e->cls==lc_static&&e->unread==1&&e->reads==59);
    CHECK(r.mem.writeCalls==0);
}

static void test_cockpit_gate_and_dup_frames() {
    RUN(r,opts(30));
    plant_tan(r.mem,B0+0x100);
    r.settle();r.scan();
    CHECK(r.sc.phase()==LensScanner::p_tracking);
    for(int i=0;i<40;++i){r.cockpit=false;r.tick();}                          // 40 frames without a cockpit: nothing counted
    CHECK(r.sc.frames_tracked()==0&&r.sc.phase()==LensScanner::p_tracking);
    r.cockpit=true;
    for(int i=0;i<10;++i){r.sc.tick(r.mem,r.sink,r.in());r.sc.tick(r.mem,r.sink,r.in());++r.frame;r.now+=16;}   // same frame twice
    CHECK(r.sc.frames_tracked()==10);
    r.track();
    CHECK(r.sc.frames_tracked()==30&&r.sc.phase()==LensScanner::p_finished);
    CHECK(r.at(B0+0x100)->reads==30);
    CHECK(r.drain());
    CHECK(r.sink.count("frames-not-cockpit=40")==1);
}

static void test_verdicts() {
    {   RUN0(r);plant_tan(r.mem,B0+0x100);r.all();r.drain();CHECK(r.sink.find("result=one-numerically-stable-tan-pair")!=nullptr);}
    {   RUN0(r);plant_tan(r.mem,B0+0x100);plant_tan(r.mem,B0+0x900);r.mem.put(B0+0x908,7.f);r.all();r.drain();CHECK(r.sink.find("result=several-tan-pairs")!=nullptr);}
    {   RUN0(r);plant_tan(r.mem,B0+0x100);r.all([&](unsigned f){r.mem.put(B0+0x100,(f&1)?0.9f:0.6152f);});r.drain();CHECK(r.sink.find("result=tan-pairs-but-none-numerically-stable")!=nullptr);}
    {   RUN0(r);r.mem.put(B0+0x100,1.6255f);r.mem.put(B0+0x104,2.8902f);r.all();r.drain();CHECK(r.sink.find("result=no-tan-pair-see-other-kinds")!=nullptr);}
    {   RUN0(r);r.all();r.drain();CHECK(r.sink.find("result=zero-hits")!=nullptr);CHECK(r.sc.phase()==LensScanner::p_finished);}
}

static void test_caps() {
    {   // byte cap: 300 KiB of a 1 MiB region
        auto o=opts();o.byte_cap=300*1024;RUN(r,o);
        plant_tan(r.mem,B0+100*1024);plant_tan(r.mem,B0+500*1024);
        r.settle();r.scan();
        CHECK(r.sc.scanned_bytes()==300*1024&&!r.sc.scan_complete()&&r.sc.candidates()==1&&r.at(B0+100*1024));
        r.track();r.drain();
        CHECK(r.sink.count("scan-stop=byte-cap")==1&&r.sink.count("tracking-complete=1")==1&&r.sink.count("scan-complete=0")==1);
    }
    {   // time cap
        auto o=opts();o.scan_cap_ms=50;o.chunks_per_call=1;RUN(r,o);
        plant_tan(r.mem,B0+8);plant_tan(r.mem,B0+900*1024);
        r.settle();r.scan();
        CHECK(!r.sc.scan_complete()&&r.sc.scanned_bytes()<1u<<20&&r.sc.candidates()==1);
        CHECK(r.sink.count("event=scan-done;stop=time-cap")==1);
    }
    {   // per-kind cap (12) and the drop counter
        RUN0(r);for(unsigned i=0;i<20;++i)plant_tan(r.mem,B0+0x1000+i*0x200);
        r.settle();r.scan();
        CHECK(r.sc.candidates()==lens_kind_cap&&r.sc.found(lk_tan)==20&&r.sc.dropped()==8);
    }
    {   // total cap (48) over five kinds x 12
        RUN0(r);
        for(unsigned i=0;i<14;++i) {
            const uint64_t a=B0+0x1000+i*0x200;
            if(i<13){r.mem.put(a,0.6152f);r.mem.put(a+4,0.3460f);}
        }
        for(unsigned k=1;k<lk_kinds;++k)for(unsigned i=0;i<14;++i){const uint64_t a=B0+0x20000+k*0x4000+i*0x200;r.mem.put(a,lens_kind[k].x);r.mem.put(a+4,lens_kind[k].y);}
        r.settle();r.scan();
        CHECK(r.sc.candidates()==lens_cand_cap);
        unsigned per[lk_kinds]{};for(unsigned i=0;i<r.sc.candidates();++i)++per[r.sc.candidate(i).kind];
        for(unsigned k=0;k<lk_kinds;++k)CHECK(per[k]<=lens_kind_cap);
        r.track();CHECK(r.drain());
        CHECK(r.out.files.size()==1&&r.out.files[0].size()<lens_file_cap&&r.sink.longest()<440);
    }
    {   // region cap and overlap: eight offered 64 MiB blocks (one overlapping the anchor) -> at most 4 regions
        auto o=opts();o.chunk_bytes=lens_chunk_default;RUN(r,o);
        r.mem.segs.clear();
        for(unsigned i=0;i<8;++i)r.mem.add(B0+uint64_t(i)*block_bytes,block_bytes,0);
        r.alo=B0+uint64_t(2)*block_bytes+0x1000;r.ahi=r.alo+0x20000;r.aeff=r.alo;
        for(unsigned i=0;i<8;++i)r.blocks.push_back(B0+uint64_t(i)*block_bytes);
        r.mem.put(r.alo+0x40,0.6152f);r.mem.put(r.alo+0x44,0.3460f);
        r.settle();
        CHECK(r.sc.needs_blocks());
        r.work();
        CHECK(!r.sc.needs_blocks());
        r.scan();
        CHECK(r.sc.scanned_bytes()<=lens_byte_cap);
        CHECK(r.sink.find("event=targets;regions=4;")!=nullptr);
        const std::string* t=r.sink.find("event=targets");
        CHECK(t&&t->find("(anchor)")!=std::string::npos);
        CHECK(r.at(r.alo+0x40)!=nullptr);
    }
    {   // no regions at all: no anchor, no blocks -> aborted
        RUN0(r);r.have=false;const double t0=r.now;r.settle();
        CHECK(r.sc.phase()==LensScanner::p_scanning&&r.now-t0>=lens_anchor_grace_ms);   // waited the anchor grace first
        CHECK(r.sink.find("anchor=none")!=nullptr);
        r.work();
        CHECK(r.sc.phase()==LensScanner::p_aborted&&std::strcmp(r.sc.reason(),"no-regions")==0);
        CHECK(r.drain()&&r.out.files.size()==1);
    }
}

static void test_waiting_gate() {
    {   // cockpit must stay up for settle_ticks; a drop resets the count
        auto o=opts();o.settle_ticks=10;RUN(r,o);
        for(int i=0;i<5;++i)r.tick();
        r.cockpit=false;r.tick();r.cockpit=true;
        for(int i=0;i<9;++i)r.tick();
        CHECK(r.sc.phase()==LensScanner::p_waiting);
        r.tick();CHECK(r.sc.phase()==LensScanner::p_scanning);
        CHECK(r.mem.reads==0);                                                  // the Present tick never reads while waiting/scanning
    }
    {   // never a cockpit: wait cap -> aborted, a (zero candidate) report is still produced
        auto o=opts();o.wait_cap_ms=5000;RUN(r,o);r.cockpit=false;
        for(int i=0;i<2000&&r.sc.phase()==LensScanner::p_waiting;++i)r.tick();
        CHECK(r.sc.phase()==LensScanner::p_aborted&&std::strcmp(r.sc.reason(),"never-cockpit")==0);
        CHECK(r.mem.reads==0&&r.sink.count("event=wait;reason=waiting-for-cockpit")>=1);
        CHECK(r.drain()&&r.out.files.size()==1&&r.out.files[0].find("candidates=0")!=std::string::npos);
    }
    {   // anchor: scanning starts at once with the anchor; without it only after the grace period
        RUN0(r);r.settle();CHECK(r.sc.phase()==LensScanner::p_scanning);
        CHECK(r.sink.find("anchor=tracks-look")!=nullptr);
        RUN0(q);q.have=false;q.settle();CHECK(q.sc.phase()==LensScanner::p_scanning&&q.sink.count("waiting-tracks-look-anchor")>=1);
    }
}

static void test_stop_and_finish() {
    {   // stop file while waiting: no read, ever
        RUN0(r);r.stop=true;r.tick();
        CHECK(r.sc.phase()==LensScanner::p_stopped&&std::strcmp(r.sc.reason(),"stop-file")==0&&r.mem.reads==0);
        r.stop=false;r.tick();r.work();CHECK(r.mem.reads==0&&r.sc.phase()==LensScanner::p_stopped);
        CHECK(r.drain()&&r.out.files.size()==1&&r.out.files[0].find("reason=stop-file")!=std::string::npos);
    }
    {   // stop file while scanning (seen by the worker): scanning ends, no further reads
        auto o=opts();o.chunks_per_call=1;RUN(r,o);plant_tan(r.mem,B0+16);
        r.settle();r.work();r.work();r.work();
        const unsigned before=r.mem.reads;
        r.stop=true;r.work();r.work();
        CHECK(r.sc.phase()==LensScanner::p_stopped&&r.mem.reads==before);
        CHECK(r.sc.scanned_bytes()==3*4096&&r.sc.candidates()==1);
        CHECK(r.drain()&&r.out.files[0].find("tracking-complete=0")!=std::string::npos);
    }
    {   // stop file while tracking (seen by the Present tick)
        RUN(r,opts(100));plant_tan(r.mem,B0+16);r.settle();r.scan();
        for(int i=0;i<10;++i)r.tick();
        const unsigned before=r.mem.reads;
        r.stop=true;r.tick();
        CHECK(r.sc.phase()==LensScanner::p_stopped&&r.sc.frames_tracked()==10);
        r.tick();CHECK(r.mem.reads==before);
        CHECK(r.drain()&&r.out.files[0].find("tracking-complete=0")!=std::string::npos&&r.out.files[0].find("partial=1")!=std::string::npos);
    }
    {   // exit path: finish() during tracking / after the end
        RUN(r,opts(100));plant_tan(r.mem,B0+16);r.settle();r.scan();
        for(int i=0;i<5;++i)r.tick();
        r.sc.finish("process-exit");
        CHECK(r.sc.phase()==LensScanner::p_stopped&&std::strcmp(r.sc.reason(),"process-exit")==0);
        r.sc.finish("again");CHECK(std::strcmp(r.sc.reason(),"process-exit")==0);   // idempotent, first reason wins
        CHECK(r.drain());
        RUN(q,opts(5));plant_tan(q.mem,B0+16);q.all();q.sc.finish("late");CHECK(q.sc.phase()==LensScanner::p_finished);   // a finished scan stays finished
    }
}

static void test_read_only() {
    RUN0(r);
    uint32_t rng=7;
    Seg& s=r.mem.segs[0];s.w.assign(1u<<14,0);
    for(auto& w:s.w){rng=rng*1664525u+1013904223u;w=rng;}
    plant_tan(r.mem,B0+0x400);r.mem.put(B0+0x800,1.6255f);r.mem.put(B0+0x804,2.8902f);
    const uint64_t h0=r.mem.checksum();
    r.all([&](unsigned){});
    CHECK(r.sc.candidates()>=2);
    CHECK(r.mem.checksum()==h0);
    CHECK(r.mem.writeCalls==0);
    CHECK(r.sc.phase()==LensScanner::p_finished);
}

static void test_file_and_hash() {
    auto run_once=[&](float tanx){
        RUN0(r);r.mem.put(B0+0x100,tanx);r.mem.put(B0+0x104,0.3460f);r.mem.put(B0+0x300,1.6255f);r.mem.put(B0+0x30C,2.8902f);
        r.all([&](unsigned f){r.mem.put_bits(B0+0x300,bits(1.6255f)+(f&1));});
        CHECK(r.drain());
        return std::make_pair(r.out.files.empty()?std::string():r.out.files[0],r.sink.lines);
    };
    auto a=run_once(0.6152f);auto b=run_once(0.6152f);auto c=run_once(0.6153f);
    CHECK(!a.first.empty()&&a.first==b.first);                                   // deterministic
    CHECK(a.first!=c.first);
    size_t st=0;const std::string last=last_line(a.first,st);
    CHECK(last.rfind("sha256=",0)==0&&last.size()==7+64);
    CHECK(last.substr(7)==hex_of(a.first.substr(0,st)));                           // the hash covers every byte before its own line
    std::string flipped=a.first.substr(0,st);flipped[10]^=1;CHECK(last.substr(7)!=hex_of(flipped));
    CHECK(a.first.rfind("lens-scan;event=file-header;format=acz-lens-scan-candidates-v2",0)==0);
    CHECK(a.first.find("writes-game-memory=0")!=std::string::npos);
    bool logged=false;for(auto& l:a.second)if(l.find("event=file;")!=std::string::npos&&l.find("sha256="+last.substr(7))!=std::string::npos)logged=true;
    CHECK(logged);
    {   // report once; second drain does nothing
        RUN0(r);plant_tan(r.mem,B0+0x100);r.all();
        CHECK(r.drain()&&!r.drain()&&r.out.files.size()==1&&r.sc.written());
        CHECK(std::string(r.sc.file_sha256())==last_line(r.out.files[0],st).substr(7));
    }
    {   // write failure: retried, succeeds on the 2nd attempt
        RUN0(r);plant_tan(r.mem,B0+0x100);r.out.failFirst=1;r.all();
        CHECK(!r.drain()&&r.sc.write_failures()==1&&!r.sc.written());
        CHECK(r.drain()&&r.sc.written()&&r.out.files.size()==1);
    }
    {   // permanent failure: three attempts, then it stops trying (the log lines remain the record)
        RUN0(r);plant_tan(r.mem,B0+0x100);r.out.failAll=true;r.all();
        for(int i=0;i<6;++i)r.drain();
        CHECK(r.out.calls==3&&r.sc.write_failures()==3&&!r.sc.written()&&r.sink.count("event=file-write-failed")==3);
    }
    {   // nothing to report until a terminal state
        RUN0(r);plant_tan(r.mem,B0+0x100);r.settle();CHECK(!r.drain()&&r.out.calls==0);
        RUN(q,LensScanner::Options{});CHECK(!q.sc.enabled()&&q.sc.phase()==LensScanner::p_off);
        q.tick();q.work();CHECK(q.mem.reads==0&&!q.drain());
    }
    {   // every log line fits the 448-byte sink buffers used by the glue
        RUN0(r);for(unsigned i=0;i<10;++i)plant_tan(r.mem,B0+0x1000+i*0x100);r.all();r.drain();
        CHECK(r.sink.longest()<440);
    }
}


static void test_astra_review() {
    {   // (1) unreadable boundary: a region split at a chunk edge makes the straddling windows unreadable; coverage is UNKNOWN, never "absent"
        RUN0(r);r.mem.segs[0].splits.push_back(B0+8192);
        plant_tan(r.mem,B0+5000);plant_tan(r.mem,B0+20000);
        r.settle();r.scan();
        CHECK(r.at(B0+20000)!=nullptr&&r.at(B0+5000)==nullptr);          // the pair inside the unreadable window is NOT reported (no partial reads)
        r.track();r.drain();
        CHECK(r.sink.find("event=coverage;known=0")!=nullptr&&r.sink.find("unreadable-chunks")!=nullptr);
        CHECK(r.sink.find("coverage=UNKNOWN")!=nullptr);
    }
    {   // zero hits with unreadable coverage: the statement is bounded, and never claims "VU1/matrix only"
        RUN0(r);r.mem.segs[0].badFrom=B0+8192;
        r.settle();r.scan();r.drain();
        CHECK(r.sc.candidates()==0);
        const std::string* z=r.sink.find("event=zero-hits");
        CHECK(z&&z->find("not found within these representations/ranges/sampling limits")!=std::string::npos&&z->find("coverage=UNKNOWN")!=std::string::npos);
        for(auto& l:r.sink.lines)CHECK(l.find("VU1")==std::string::npos&&l.find("abort")==std::string::npos);
        RUN0(q);q.settle();q.scan();q.drain();                            // complete coverage: still bounded wording
        const std::string* z2=q.sink.find("event=zero-hits");
        CHECK(z2&&z2->find("coverage=complete-for-these-ranges")!=std::string::npos&&z2->find("not found within these representations/ranges/sampling limits")!=std::string::npos);
    }
    {   // executable / non-committed / non-data pages are never read at all
        RUN0(r);r.mem.segs[0].exec=true;plant_tan(r.mem,B0+0x100);
        r.settle();r.scan();
        CHECK(r.sc.candidates()==0&&r.mem.reads==0&&r.mem.queries>0);
        r.drain();CHECK(r.sink.find("excluded-nondata-pages")!=nullptr);
        RUN0(q);q.mem.segs[0].committed=false;plant_tan(q.mem,B0+0x100);q.settle();q.scan();CHECK(q.sc.candidates()==0&&q.mem.reads==0);
        RUN0(g);g.mem.segs[0].data=false;plant_tan(g.mem,B0+0x100);g.settle();g.scan();CHECK(g.sc.candidates()==0&&g.mem.reads==0);
    }
    {   // stale / reallocated block: the identity is revalidated before every re-read; a change = unreadable, no further reads
        RUN(r,opts(60));plant_tan(r.mem,B0+0x100);r.settle();r.scan();
        for(int i=0;i<10;++i)r.tick();
        r.mem.segs[0].alloc=0x1234000;                                    // block freed and reallocated elsewhere
        const unsigned before=r.mem.reads;
        r.track();
        CHECK(r.mem.reads==before);
        CHECK(r.at(B0+0x100)->identChanged==50&&r.at(B0+0x100)->reads==10);
        r.drain();
        CHECK(r.sink.find("class=unreadable-or-identity-changed")!=nullptr);
        RUN(q,opts(30));plant_tan(q.mem,B0+0x100);q.settle();q.scan();
        for(int i=0;i<5;++i)q.tick();
        q.mem.segs[0].committed=false;q.track();                          // decommitted
        CHECK(q.at(B0+0x100)->identChanged==25);
        RUN(z,opts(30));plant_tan(z.mem,B0+0x100);z.settle();z.scan();
        for(int i=0;i<5;++i)z.tick();
        z.mem.segs[0].size=1u<<19;z.track();                              // region shrank
        CHECK(z.at(B0+0x100)->identChanged==25);
    }
    {   // duplicates: identical pairs with identical neighbours are copies of one thing, not two candidates
        RUN0(r);plant_tan(r.mem,B0+0x100);plant_tan(r.mem,B0+0x2100);r.all();r.drain();
        CHECK(r.sc.found(lk_tan)==2);
        CHECK(r.sink.find("result=one-tan-pair-with-copies")!=nullptr);
        CHECK(r.sink.count("dup-group=0;copies=2")==2);
    }
    {   // same-value rewrite is invisible to reads: classed static, and the output never implies Phase 1 admission
        RUN(r,opts(40));plant_tan(r.mem,B0+0x100);r.settle();r.scan();
        r.track([&](unsigned){r.mem.put(B0+0x100,0.6152f);r.mem.put(B0+0x104,0.3460f);});     // rewritten with the same value every frame
        r.drain();
        CHECK(r.at(B0+0x100)->cls==lc_static);
        const std::string* v=r.sink.find("event=verdict");
        CHECK(v&&v->find("candidate only; target/consumer unidentified")!=std::string::npos&&v->find("phase1-admission=not-implied")!=std::string::npos);
        CHECK(r.sink.find("same-value-rewrite=undetectable-by-reads")!=nullptr&&r.sink.find("needed-before-any-write=live-vs-copy,write-target,consumer-identity,rewrite-behaviour")!=nullptr);
        CHECK(r.out.files[0].find("statement=\"candidate only; target/consumer unidentified\"")!=std::string::npos);
        for(auto& l:r.sink.lines)CHECK(l.find("plan-phase1-gate")==std::string::npos&&l.find("exactly-one-stable")==std::string::npos);
    }
    {   // second sample point: a value that differs between the Present point and the seam-B point is NOT a constant
        RUN(r,opts(30));plant_tan(r.mem,B0+0x100);r.settle();r.scan();
        for(unsigned f=0;f<30&&r.sc.phase()==LensScanner::p_tracking;++f) {
            r.mem.put_bits(B0+0x100,bits(0.6152f));r.tick();
            r.mem.put_bits(B0+0x100,bits(0.6152f)+1);r.sc.sample_b(r.mem);r.sc.sample_b(r.mem);   // second call in the same frame is ignored
        }
        const LensCand* c=r.at(B0+0x100);
        r.drain();
        CHECK(c->bReads>=28&&c->bReads<=30&&c->crossDiffs>=28&&c->cls==lc_point_varies);
        CHECK(r.sink.find("result=tan-pairs-but-none-numerically-stable")!=nullptr);
        RUN(q,opts(10));plant_tan(q.mem,B0+0x100);q.sc.sample_b(q.mem);CHECK(q.mem.reads==0);    // ignored outside tracking
    }
    {   // log cap: candidate lines beyond the cap are dropped from the LOG (counted), never from the file; essential lines survive
        auto o=opts();o.max_log_lines=14;RUN(r,o);
        for(unsigned i=0;i<12;++i)plant_tan(r.mem,B0+0x1000+i*0x200+(i%3)*4);
        r.all();r.drain();
        CHECK(r.sink.count("event=candidate;")<12);
        CHECK(r.sink.find("event=verdict")!=nullptr&&r.sink.find("event=file;")!=nullptr);
        const std::string* v=r.sink.find("event=verdict");
        CHECK(v&&v->find("log-lines-dropped=0")==std::string::npos);
        unsigned inFile=0;for(size_t q=0;(q=r.out.files[0].find("event=candidate;",q))!=std::string::npos;++q)++inFile;
        CHECK(inFile==r.sc.candidates());
    }
}

// Step 47 run 1 regression: the whole 64 MiB EE block was rejected (256 of 256 chunks "unreadable" AND "excluded"). Attributes used here are
// the observed ones (MEM_PRIVATE, committed, allocation base == region base 0x18808800000, size 0x4000000; the probe accepts it through
// rw_protect(), so the low protect byte is PAGE_READWRITE [or PAGE_EXECUTE_READWRITE]); the census dropping it points at a WC/NOCACHE modifier.
// The exact protect word was not logged by that run, so every plausible value is covered below.
static void run_block(uint32_t state,uint32_t type,uint32_t prot,bool expectRead,const char* expectReason) {
    constexpr uint64_t base=0x18808800000ull,size=0x4000000ull;
    LensScanner::Options o=opts();o.chunk_bytes=1u<<18;
    Run r(o);r.mem.segs.clear();
    Seg& s=r.mem.add(base,size,0x1000);s.alloc=base;s.rstate=state;s.rtype=type;s.rprot=prot;
    r.alo=base;r.ahi=base+size;r.aeff=base+0x100;r.have=false;r.blocks.push_back(base);
    plant_tan(r.mem,base+0x2000);
    r.settle();r.scan();
    if(expectRead) {
        CHECK(r.sc.candidates()>=1);
        CHECK(r.sink.count("first-reject")==0);
        CHECK(r.sink.find("unreadable-chunks=0")!=nullptr);
    } else {
        CHECK(r.sc.candidates()==0);
        const std::string* f=r.sink.find("event=first-reject");
        CHECK(f!=nullptr);
        if(f){
            CHECK(f->find(expectReason)!=std::string::npos);
            CHECK(f->find("window=18808800000+")!=std::string::npos);
            CHECK(f->find("query-base=18808800000;query-size=4000000;query-alloc=18808800000")!=std::string::npos);
            char want[48];std::snprintf(want,sizeof want,"state=%x;type=%x;protect=%x",state,type,prot);
            CHECK(f->find(want)!=std::string::npos);
        }
        CHECK(r.sink.find("unreadable-chunks=256")!=nullptr);
        CHECK(r.mem.writeCalls==0);
    }
}
static void test_step47_block_attributes() {
    // pure classifier
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_readwrite)==lpc_data);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_writecombine)==lpc_data);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_nocache)==lpc_data);
    CHECK(lens_classify(lw_mem_commit,lw_mem_mapped,lw_pg_readwrite|lw_pg_writecombine)==lpc_wc_nocache_not_private);
    CHECK(lens_classify(lw_mem_commit,lw_mem_mapped,lw_pg_readonly|lw_pg_nocache)==lpc_wc_nocache_not_private);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_guard)==lpc_guard);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_noaccess)==lpc_no_access);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_exec_rw)==lpc_executable);
    CHECK(lens_classify(lw_mem_commit,lw_mem_private,lw_pg_exec_read|lw_pg_writecombine)==lpc_executable);
    CHECK(lens_classify(lw_mem_commit,lw_mem_image,lw_pg_readonly)==lpc_image);
    CHECK(lens_classify(lw_mem_reserve,lw_mem_private,lw_pg_readwrite)==lpc_not_committed);
    CHECK(lens_classify(lw_mem_free,0,lw_pg_noaccess)==lpc_not_committed);
    // the block under every plausible attribute word
    run_block(lw_mem_commit,lw_mem_private,lw_pg_readwrite,true,"");
    run_block(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_writecombine,true,"");     // the suspected real value (was rejected before the fix)
    run_block(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_nocache,true,"");
    run_block(lw_mem_commit,lw_mem_mapped,lw_pg_readwrite|lw_pg_writecombine,false,"write-combined-or-uncached-non-private");
    run_block(lw_mem_commit,lw_mem_private,lw_pg_exec_rw,false,"reason=executable");        // still excluded, and the rerun now says so
    run_block(lw_mem_commit,lw_mem_private,lw_pg_readwrite|lw_pg_guard,false,"reason=guard");
}

static void test_config() {
    CHECK(!Config{}.ee_lens_scan());
    CHECK(!parse_config("").config.ee_lens_scan());
    CHECK(parse_config("ee_lens_scan=true\n").config.ee_lens_scan());
    CHECK(parse_config("ee_lens_scan=1\n").config.ee_lens_scan());
    CHECK(!parse_config("ee_lens_scan=0\n").config.ee_lens_scan());
    CHECK(parse_config("ee_lens_scan=true\n").issue==ConfigIssue::none);
    for(const char* bad:{"ee_lens_scan=maybe\n","ee_lens_scan=\n"})CHECK(parse_config(bad).issue!=ConfigIssue::none);
    const std::string base=
        "log_enabled=true\ndiagnostic_ownership_pins=true\ngpu_copy_witness_observation=true\ngpu_copy_early_device_hook=true\n"
        "gpu_copy_step3_experiment=true\ngpu_copy_step3_whole_set=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_members_by_class=true\n";
    {   // the latch: needs the opt-in gate, logging, ee_camera_probe and the key itself
        auto r=parse_config((base+"ee_camera_probe=true\nee_lens_scan=true\n").c_str());
        CHECK(r.issue==ConfigIssue::none&&r.config.ee_lens_scan());
        CHECK(r.config.lens_scan_latch(true)&&!r.config.lens_scan_latch(false));
        CHECK(!parse_config((base+"ee_lens_scan=true\n").c_str()).config.lens_scan_latch(true));            // no probe
        CHECK(!parse_config((base+"ee_camera_probe=true\n").c_str()).config.lens_scan_latch(true));         // no key
        CHECK(!parse_config("ee_camera_probe=true\nee_lens_scan=true\nlog_enabled=false\n").config.lens_scan_latch(true));   // no logging
        // refused with ANY key that can write game memory
        for(const char* w:{"ee_camera_write_trials=true\n","ee_camera_look_write_trial=true\n","ee_camera_look_drive=true\n"})
            CHECK(!parse_config((base+"ee_camera_probe=true\nee_lens_scan=true\n"+w).c_str()).config.lens_scan_latch(true));
        // a writer key set to false does not refuse
        CHECK(parse_config((base+"ee_camera_probe=true\nee_lens_scan=true\nee_camera_look_write_trial=false\n").c_str()).config.lens_scan_latch(true));
    }
    {   // budgets: the null-driver scan set and a full headset-like set + the two keys stay inside 32 keys / 2048 bytes
        const std::string scan=base+"ee_camera_probe=true\nee_lens_scan=true\n";
        CHECK(scan.size()<2048&&parse_config(scan.c_str()).issue==ConfigIssue::none);
        const std::string headset=base+"xr_stereo_output=true\ngpu_copy_step5_hmd_fov=true\ngpu_copy_step5_hud=true\nxr_hud_layer=true\nxr_hud_quad_distance_m=0.5\n"
            "xr_hud_scale=0.5\nxr_world_quad_distance_m=500\ngpu_copy_step5_units_per_metre=65\nxr_world_in_projection=true\ngpu_copy_step5_world_objects=true\n"
            "gpu_copy_step5_hud_markers=true\nxr_hud_markers_layer=true\ngpu_copy_step5_effects=true\nxr_effects_layer=true\nxr_world_hold_last=true\nxr_world_extend=true\n"
            "ee_camera_probe=true\nee_lens_scan=true\n";
        size_t keys=0;for(char ch:headset)keys+=ch=='\n';
        CHECK(keys<=32&&headset.size()<2048&&parse_config(headset.c_str()).issue==ConfigIssue::none);
    }
}

int main() {
    test_sha();
    test_pairs();
    test_chunk_edges();
    test_noise();
    test_tracking_classes();
    test_cockpit_gate_and_dup_frames();
    test_verdicts();
    test_caps();
    test_waiting_gate();
    test_stop_and_finish();
    test_read_only();
    test_file_and_hash();
    test_astra_review();
    test_config();
    test_step47_block_attributes();
    std::printf("ee_lens_scan_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
