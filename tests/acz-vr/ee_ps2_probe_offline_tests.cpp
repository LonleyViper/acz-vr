// PS2 address probe: offline fixtures for the READ-ONLY probe (ee_ps2_probe.h) against FAKE 64 MiB memory images with a planted mission byte, game id,
// camera-struct signature, E6 fields, drifting values, unreadable boundaries and exhausted caps. Nothing here touches a process or the game.
#include "ee_ps2_probe.h"
#include "runtime_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static uint32_t fbits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
struct Seg {
    uint64_t base=0,size=0;std::vector<uint32_t> w;
    bool committed=true,data=true,exec=false;uint64_t alloc=0;
    std::vector<uint64_t> splits;uint64_t badFrom=~0ull;                  // extra region boundaries; addresses >= badFrom are not readable data
};
struct Mem final:LensMem {
    std::vector<Seg> segs;std::vector<std::pair<uint64_t,uint64_t>> unreadable;   // [a, b) ranges whose reads fail
    unsigned reads=0,writeCalls=0,queries=0;
    Seg& add(uint64_t base,uint64_t size) {segs.push_back(Seg{base,size,std::vector<uint32_t>()});return segs.back();}
    void put(uint64_t addr,float v) {put_bits(addr,fbits(v));}
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
    void put_byte(uint64_t addr,uint8_t v) {
        const uint64_t a=addr&~3ull;const unsigned sh=unsigned(addr&3)*8;
        put_bits(a,(get_bits(a)&~(0xFFu<<sh))|(uint32_t(v)<<sh));
    }
    void put_str(uint64_t addr,const char* t) {for(size_t i=0;t[i];++i)put_byte(addr+i,uint8_t(t[i]));}
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
            return true;
        }
        return false;
    }
    bool write_float(uint64_t,float) noexcept override {++writeCalls;return false;}   // must never be called
    uint64_t checksum() const {uint64_t h=1469598103934665603ull;for(const Seg& s:segs)for(uint32_t w:s.w){h^=w;h*=1099511628211ull;}return h;}
};
struct Sink2 final:Sink {
    std::vector<std::string> lines;
    void line(bool,const char* t) noexcept override {lines.emplace_back(t);if(std::getenv("EE_PS2_VERBOSE"))std::printf("  %s\n",t);}
    unsigned count(const char* s) const {unsigned n=0;for(auto& l:lines)n+=l.find(s)!=std::string::npos;return n;}
    const std::string* find(const char* s) const {for(auto& l:lines)if(l.find(s)!=std::string::npos)return &l;return nullptr;}
    const std::string* find2(const char* a,const char* b) const {for(auto& l:lines)if(l.find(a)!=std::string::npos&&l.find(b)!=std::string::npos)return &l;return nullptr;}
};
struct Out final:LensOut {
    std::vector<std::string> files;unsigned calls=0;bool failAll=false;
    bool write_file(const char* t,size_t n) noexcept override {++calls;if(failAll)return false;files.emplace_back(t,n);return true;}
};

constexpr uint64_t B0=0x7000000000ull;
constexpr uint64_t CAM_A=0x900000,CAM_B=0xA00000,E6=0xFDD000;
Ps2Prober::Options opts(uint32_t chunk=pp_chunk_default) {
    Ps2Prober::Options o;o.enabled=true;o.chunk_bytes=chunk;o.settle_ticks=3;return o;
}
struct Run {
    Mem mem;Sink2 sink;Ps2Prober pr;Out out;
    double now=1000;uint64_t frame=1;bool have=true,stop=false;uint64_t lo=B0;
    double tStart=-1;
    std::function<void(Run&)> hook;
    explicit Run(Ps2Prober::Options o=opts()):pr(o) {mem.add(B0,block_bytes);}
    LensIn in() const {LensIn i;i.now_ms=now;i.frame=frame;i.stop_file=stop;i.v.have=have;i.v.lo=lo;i.v.hi=lo+block_bytes;i.v.cockpit=true;return i;}
    void tick(){pr.tick(in());++frame;now+=1000.0/60;}
    void work(){
        if(hook)hook(*this);
        Ps2WorkIn w;w.now_ms=now;w.stop_file=stop;pr.work(mem,sink,w);
        if(tStart<0&&pr.phase()==Ps2Prober::p_sampling)tStart=now;
        now+=5;
    }
    void ticks(unsigned n){for(unsigned i=0;i<n;++i)tick();}
    void all(unsigned maxCalls=400000){
        ticks(5);
        for(unsigned g=0;g<maxCalls&&!pr.terminal();++g){work();if(g%12==0)tick();}
    }
    bool drain(){return pr.drain(sink,out);}
};
#define RUN(n,...) std::unique_ptr<Run> n##_p(new Run(__VA_ARGS__)); Run& n=*n##_p

static std::string hex_of(const std::string& s){char h[65];sha256_hex(s.data(),s.size(),h);return h;}
// the last line of the file is sha256=<hex of everything before it>
static bool file_hash_ok(const std::string& f,std::string* sha=nullptr) {
    if(f.empty()||f.back()!='\n')return false;
    const size_t e=f.size()-1,s=f.rfind('\n',e-1);
    const size_t start=(s==std::string::npos)?0:s+1;
    const std::string last=f.substr(start,e-start);
    if(last.compare(0,7,"sha256=")!=0)return false;
    if(sha)*sha=last.substr(7);
    return last.substr(7)==hex_of(f.substr(0,start));
}
static double num_after(const std::string& line,const char* key) {
    const size_t p=line.find(key);return p==std::string::npos?-1e30:std::strtod(line.c_str()+p+std::strlen(key),nullptr);
}
// the standard planted image: mission 3, gates, draw distance, a game id, two camera structs, E6 fields.
static void plant(Run& r,bool second=true) {
    Mem& m=r.mem;
    m.put_byte(B0+pp_off_mission,3);m.put_byte(B0+pp_off_gate,13);m.put_byte(B0+pp_off_phase,15);m.put_bits(B0+pp_off_draw,0x477FFF00u);
    m.put_str(B0+0x3A1003,"SLUS_213.46");                      // unaligned on purpose
    for(uint64_t S:{CAM_A,CAM_B}) {
        if(S==CAM_B&&!second)break;
        m.put(B0+S,512.0f);m.put(B0+S+4,448.0f);m.put(B0+S+pp_c_dist,1.0f);m.put(B0+S+pp_c_dist+4,1.0f);
        m.put(B0+S+pp_c_bg,512.0f);m.put(B0+S+pp_c_bg+4,448.0f);
        for(unsigned k=0;k<9;++k)m.put(B0+S+pp_c_pos+4*k,100.0f*float(k+1)+(S==CAM_B?7.0f:0.0f));
        const float rot[3]={0.1f,1.5f,0.25f};
        for(unsigned k=0;k<3;++k)m.put(B0+S+pp_c_rot+4*k,S==CAM_A?rot[k]:rot[k]+0.5f);
    }
    for(unsigned k=0;k<3;++k){m.put(B0+pp_e6_pos+4*k,10.0f*float(k+1)+0.0f);m.put(B0+pp_e6_rot+4*k,k==0?0.1f:(k==1?1.5f:0.25f));}
    m.put(B0+pp_e6_yaw,0.7f);
    // A's third position triple equals the E6 position
    for(unsigned k=0;k<3;++k)m.put(B0+CAM_A+pp_c_pos+24+4*k,10.0f*float(k+1));
}

// ------------------------------------------------------------------ tests
static void test_happy() {
    RUN(r,opts());plant(r);
    const uint64_t sum=r.mem.checksum();
    r.all();
    CHECK(r.pr.phase()==Ps2Prober::p_finished);
    CHECK(r.pr.base()==B0);
    CHECK(r.pr.cam_hits()==2&&r.pr.cam_total()==2&&r.pr.cam_hit(0).off==CAM_A&&r.pr.cam_hit(1).off==CAM_B);
    CHECK(r.pr.gid_hits()==1&&r.pr.gid_hit(0).off==0x3A1003&&std::string(r.pr.gid_hit(0).text)=="SLUS_213.46");
    CHECK(r.pr.aob_hits()==0&&r.pr.bg_pair_hits()==2);
    CHECK(r.pr.fixed_done()==pp_fixed_samples&&r.pr.cam_done()==pp_cam_samples&&r.pr.stage_done()==pp_stage_samples);
    CHECK(r.pr.scanned_bytes()==(pp_cam_hi-pp_cam_lo)+(pp_id_hi-pp_id_lo));
    CHECK(r.pr.scan_complete()&&r.pr.unreadable_chunks()==0&&r.pr.excluded_chunks()==0&&r.pr.value_unreadable()==0);
    // line counts: fixed = 20 samples x (4 fixed points + 1 game id), cam = 10 x 2 hits, e6 = 10, stage = 60
    CHECK(r.sink.count("ps2-probe;event=fixed;")==pp_fixed_samples*5);
    CHECK(r.sink.count("ps2-probe;event=cam;")==pp_cam_samples*2);
    CHECK(r.sink.count("ps2-probe;event=e6;")==pp_cam_samples);
    CHECK(r.sink.count("ps2-probe;event=stage;")==pp_stage_samples);
    CHECK(r.sink.count("event=hit;kind=camera-struct")==2&&r.sink.count("event=hit;kind=game-id;")==1);
    // values
    const std::string* st=r.sink.find("event=stage;s=7;");
    CHECK(st&&st->find("mission=3;")!=std::string::npos&&st->find("gate3ACEA0=13")!=std::string::npos&&st->find("phase76587C=15")!=std::string::npos);
    CHECK(st&&st->find("cam-b30=100,200,300")!=std::string::npos&&st->find("e6-campos=10,20,30")!=std::string::npos);
    const std::string* fx=r.sink.find2("event=fixed;s=0;","name=draw765320;");
    CHECK(fx&&fx->find("u32=0x477FFF00")!=std::string::npos&&fx->find("f32=65535")!=std::string::npos);
    const std::string* gi=r.sink.find("name=game-id;");
    CHECK(gi&&gi->find("text=SLUS_213.46")!=std::string::npos&&gi->find("ctx=")!=std::string::npos);
    if(gi){   // 32 bytes of context = 64 hex digits in 8 groups; the id starts at byte 16 ('S' = 53)
        const size_t p=gi->find("ctx=");const std::string c=gi->substr(p+4);
        CHECK(c.size()==71&&c.substr(36,11)=="534c5553 5f");
    }
    const std::string* cm=r.sink.find2("event=cam;s=2;","hit=0;");
    CHECK(cm&&cm->find("sig=512,448")!=std::string::npos&&cm->find("dist198=1,1")!=std::string::npos&&cm->find("rot=0.1,1.5,0.25")!=std::string::npos);
    CHECK(cm&&cm->find("rot-eq-e6=1")!=std::string::npos&&cm->find("pos-eq-e6=1")!=std::string::npos);
    const std::string* cm1=r.sink.find2("event=cam;s=2;","hit=1;");
    CHECK(cm1&&cm1->find("rot-eq-e6=0")!=std::string::npos&&cm1->find("pos-eq-e6=0")!=std::string::npos);
    CHECK(r.pr.rot_matches_e6(0)==pp_cam_samples&&r.pr.rot_matches_e6(1)==0);
    // report
    CHECK(r.drain()&&r.out.files.size()==1&&r.pr.written());
    std::string sha;CHECK(file_hash_ok(r.out.files[0],&sha)&&sha==r.pr.file_sha256());
    CHECK(r.sink.count("ps2-probe;event=file;")==1&&r.sink.find("event=file;")->find(sha)!=std::string::npos);
    CHECK(r.pr.coverage_known());
    const std::string* cov=r.sink.find("event=coverage;");
    CHECK(cov&&cov->find("known=1")!=std::string::npos&&cov->find("complete-for-these-ranges")!=std::string::npos);
    const std::string* vd=r.sink.find("event=verdict;");
    CHECK(vd&&vd->find("mission-values=3;")!=std::string::npos&&vd->find("game-id-found=1")!=std::string::npos&&vd->find("camera-struct-found=1")!=std::string::npos);
    CHECK(vd&&vd->find("hit0-rot-eq-e6=10/10")!=std::string::npos&&vd->find("hit1-rot-eq-e6=0/10")!=std::string::npos&&vd->find("values only")!=std::string::npos);
    CHECK(!r.drain());                                              // idempotent: nothing more to report
    CHECK(r.out.files[0].find("event=stage;s=59;")!=std::string::npos&&r.out.files[0].size()<pp_file_cap);
    // read only
    CHECK(r.mem.writeCalls==0&&r.mem.checksum()==sum);
}

static void test_drift_and_mission_change() {
    RUN(r,opts());plant(r,false);
    r.hook=[](Run& x){
        if(x.tStart<0)return;
        const double sec=(x.now-x.tStart)/1000.0;
        x.mem.put(B0+CAM_A+pp_c_pos,1000.0f+float(sec));
        x.mem.put_byte(B0+pp_off_mission,sec<30?3:255);
    };
    r.all();
    CHECK(r.pr.phase()==Ps2Prober::p_finished);
    const std::string* c0=r.sink.find("event=cam;s=0;");const std::string* c9=r.sink.find("event=cam;s=9;");
    CHECK(c0&&c9);
    if(c0&&c9){
        const double a=num_after(*c0,";pos="),b=num_after(*c9,";pos=");
        CHECK(a>999&&a<1002&&std::fabs((b-a)-9.0)<0.1);
    }
    const std::string* s10=r.sink.find("event=stage;s=10;");const std::string* s50=r.sink.find("event=stage;s=50;");
    CHECK(s10&&s10->find("mission=3;")!=std::string::npos&&s50&&s50->find("mission=255;")!=std::string::npos);
    CHECK(s10&&num_after(*s10,"cam-b30=")>1009&&s50&&num_after(*s50,"cam-b30=")>1049);
    r.drain();
    const std::string* vd=r.sink.find("event=verdict;");
    CHECK(vd&&vd->find("mission-values=3,255;")!=std::string::npos&&vd->find("mission-changes=1")!=std::string::npos);
    CHECK(r.pr.coverage_known());
}

static void test_boundaries() {
    {   // signatures / ids straddling a chunk edge (chunk 4096), exactly at the range ends, and just outside
        RUN(r,opts(4096));plant(r,false);
        r.mem.put(B0+pp_cam_lo+4096*5-4,512.0f);r.mem.put(B0+pp_cam_lo+4096*5,448.0f);                // straddles chunk 4/5 -> found
        r.mem.put(B0+pp_cam_hi-8,512.0f);r.mem.put(B0+pp_cam_hi-4,448.0f);                             // last valid start -> found
        r.mem.put(B0+pp_cam_hi-4,512.0f);                                                              // (overwrites the 448 above: now not a pair)
        r.mem.put(B0+pp_cam_lo-4,512.0f);r.mem.put(B0+pp_cam_lo,448.0f);                               // starts below the range -> not found
        r.mem.put_str(B0+0x300000+4096*9-5,"SLES_522.34");                                             // straddles a chunk edge
        r.mem.put_str(B0+pp_id_hi-11,"SLPM_662.96");                                                    // ends exactly at the block end
        r.mem.put_str(B0+0x340000,"SLUS_213.4");                                                        // 4 digits: no
        r.mem.put_str(B0+0x341000,"SLUS_AB.46");                                                        // not digits: no
        r.mem.put_str(B0+0x342000,"SLUS-21346");                                                        // serial form: yes
        r.all();
        bool a=false,b=false;
        for(unsigned i=0;i<r.pr.cam_hits();++i){a=a||r.pr.cam_hit(i).off==pp_cam_lo+4096*5-4;b=b||r.pr.cam_hit(i).off==pp_cam_hi-8;}
        CHECK(a&&!b);                                                                                   // pp_cam_hi-4 was overwritten
        CHECK(r.pr.cam_total()==2);                                                                     // CAM_A + the straddler
        unsigned found=0;for(unsigned i=0;i<r.pr.gid_hits();++i){const std::string t=r.pr.gid_hit(i).text;found+=(t=="SLES_522.34"||t=="SLPM_662.96"||t=="SLUS-21346"||t=="SLUS_213.46");}
        CHECK(found==4&&r.pr.gid_total()==4);
        r.drain();CHECK(r.pr.coverage_known());
    }
    {   // the last valid signature start
        RUN(r,opts(4096));
        r.mem.put(B0+pp_cam_hi-8,512.0f);r.mem.put(B0+pp_cam_hi-4,448.0f);
        r.all();
        CHECK(r.pr.cam_hits()==1&&r.pr.cam_hit(0).off==pp_cam_hi-8);
    }
    {   // unreadable range over a chunk containing a struct: not found, never claimed absent, first-reject line, UNKNOWN
        RUN(r,opts(4096));plant(r,false);
        r.mem.put(B0+0xB00000,512.0f);r.mem.put(B0+0xB00004,448.0f);
        r.mem.unreadable.push_back({B0+0xB00000,B0+0xB00000+100});
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_finished&&r.pr.unreadable_chunks()>=1&&!r.pr.scan_complete());
        bool hasB=false;for(unsigned i=0;i<r.pr.cam_hits();++i)hasB=hasB||r.pr.cam_hit(i).off==0xB00000;
        CHECK(!hasB&&r.pr.cam_total()==1);
        CHECK(r.sink.find("event=first-reject;")&&r.sink.find("event=first-reject;")->find("reason=read-failed")!=std::string::npos);
        r.drain();
        CHECK(!r.pr.coverage_known());
        const std::string* cov=r.sink.find("event=coverage;");
        CHECK(cov&&cov->find("known=0")!=std::string::npos&&cov->find("UNKNOWN")!=std::string::npos&&cov->find("unreadable-or-excluded-chunks")!=std::string::npos);
        const std::string* vd=r.sink.find("event=verdict;");
        CHECK(vd&&vd->find("not found within")==std::string::npos);                                     // no absence claim when coverage is incomplete
    }
    {   // region boundary inside a chunk window (split) and a non-data page range (badFrom): rejected, nothing read there
        RUN(r,opts(4096));plant(r,false);
        r.mem.segs[0].splits.push_back(B0+0x1000800);                                                       // mid-chunk region split
        r.mem.segs[0].badFrom=B0+0x1800000;                                                                 // everything above: not data (guard/no-access like)
        const unsigned readsBefore=r.mem.reads;(void)readsBefore;
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_finished);
        CHECK(r.pr.unreadable_chunks()>=1&&r.pr.excluded_chunks()>=1);
        r.drain();
        CHECK(!r.pr.coverage_known());
        CHECK(r.sink.find("event=first-reject;")&&r.sink.find("event=first-reject;")->find("reason=crosses-region")!=std::string::npos);
    }
    {   // unreadable sampled values: UNREAD in the lines, counted, coverage UNKNOWN
        RUN(r,opts());plant(r,false);
        r.mem.unreadable.push_back({B0+pp_off_mission&~0xFFFull,B0+(pp_off_mission&~0xFFFull)+0x1000});
        r.all();
        const std::string* st=r.sink.find("event=stage;s=3;");
        CHECK(st&&st->find("mission=UNREAD")!=std::string::npos);
        CHECK(r.pr.value_unreadable()>0);
        r.drain();CHECK(!r.pr.coverage_known()&&r.sink.find("event=coverage;")->find("unreadable-values")!=std::string::npos);
        const std::string* vd=r.sink.find("event=verdict;");CHECK(vd&&vd->find("mission-values=none-read")!=std::string::npos);
    }
    {   // allocation changed mid-run (block re-allocated): reads refused, never silently used
        RUN(r,opts());plant(r,false);
        r.hook=[](Run& x){if(x.tStart>0&&(x.now-x.tStart)>20000)x.mem.segs[0].alloc=B0-0x1000000;};
        r.all();
        CHECK(r.pr.value_unreadable()>0);
        const std::string* st=r.sink.find("event=stage;s=40;");
        CHECK(st&&st->find("mission=UNREAD")!=std::string::npos);
        r.drain();CHECK(!r.pr.coverage_known());
    }
}

static void test_caps() {
    {   // byte cap
        Ps2Prober::Options o=opts(4096);o.byte_cap=8u<<20;
        RUN(r,o);plant(r,false);
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_finished&&r.pr.scanned_bytes()<=(8u<<20));
        CHECK(r.pr.gid_hits()==0);                                                                       // the game-id pass never ran: no absence claim
        r.drain();
        const std::string* cov=r.sink.find("event=coverage;");
        CHECK(!r.pr.coverage_known()&&cov&&cov->find("byte-cap")!=std::string::npos&&cov->find("UNKNOWN")!=std::string::npos);
        CHECK(r.sink.find("event=verdict;")->find("not found within")==std::string::npos);
        CHECK(r.pr.stage_done()==pp_stage_samples);                                                      // sampling still ran on what was found
    }
    {   // time cap
        Ps2Prober::Options o=opts(4096);o.scan_cap_ms=20;o.chunks_per_call=1;
        RUN(r,o);plant(r,false);
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_finished);
        r.drain();
        CHECK(!r.pr.coverage_known()&&r.sink.find("event=coverage;")->find("scan-time-cap")!=std::string::npos);
    }
    {   // hit caps: 6 structs (cap 4), 8 ids (cap 6), 3 aob (cap 2)
        RUN(r,opts());plant(r,false);
        for(unsigned i=1;i<6;++i){const uint64_t S=0xA00000+0x20000*i;r.mem.put(B0+S,512.0f);r.mem.put(B0+S+4,448.0f);}
        for(unsigned i=0;i<8;++i){char t[16];std::snprintf(t,sizeof t,"SLUS_213.%02u",40+i);r.mem.put_str(B0+0x310000+0x1000*i,t);}
        for(unsigned i=0;i<3;++i)for(unsigned k=0;k<16;++k)r.mem.put_byte(B0+0x350000+0x1000*i+k,uint8_t(k==0?0x18:k==1?0xB7:k==2?0x3D:k==3?0:k==4?0x88:k==5?0x44:k==6?0x3F:k==7?0:k==8?0x18:k==9?0xB7:k==10?0x3D:k==11?0:k==12?0x68:k==13?0x45:k==14?0x3F:0));
        r.all();
        CHECK(r.pr.cam_hits()==pp_cam_cap&&r.pr.cam_total()==6);
        CHECK(r.pr.gid_hits()==pp_gid_cap&&r.pr.gid_total()==9);
        CHECK(r.pr.aob_hits()==pp_aob_cap&&r.pr.aob_total()==3);
        r.drain();
        const std::string* cov=r.sink.find("event=coverage;");
        CHECK(!r.pr.coverage_known()&&cov&&cov->find("hit-cap")!=std::string::npos);
        CHECK(r.sink.count("event=fixed;s=0;")==4+pp_gid_cap+pp_aob_cap);
    }
    {   // log-line cap: non-essential lines dropped from the LOG (counted), kept in the file; essential lines still pass
        Ps2Prober::Options o=opts();o.max_log_lines=50;
        RUN(r,o);plant(r);
        r.all();
        r.drain();
        CHECK(r.pr.log_dropped()>0&&!r.pr.coverage_known());
        CHECK(r.sink.find("event=coverage;")&&r.sink.find("event=coverage;")->find("log-lines-dropped")!=std::string::npos);
        CHECK(r.sink.find("event=file;")&&r.sink.find("event=verdict;"));
        CHECK(r.out.files.size()==1&&r.out.files[0].find("event=stage;s=59;")!=std::string::npos&&file_hash_ok(r.out.files[0]));
    }
    {   // file cap: truncated, flagged, still hash-pinned, summary lines still present in the tail reserve
        Ps2Prober::Options o=opts();o.file_cap=8*1024;
        RUN(r,o);plant(r);
        r.all();
        r.drain();
        CHECK(r.out.files.size()==1&&r.out.files[0].size()<=8*1024+64&&file_hash_ok(r.out.files[0]));
        CHECK(r.out.files[0].find("event=summary;")!=std::string::npos&&r.out.files[0].find("file-truncated=1")!=std::string::npos);
        CHECK(!r.pr.coverage_known()&&r.sink.find("event=coverage;")->find("file-truncated")!=std::string::npos);
        CHECK(r.out.files[0].find("event=stage;s=59;")==std::string::npos);                              // the body really was cut
    }
    {   // a failing file write is counted, not hidden
        RUN(r,opts());plant(r,false);r.out.failAll=true;
        r.all();CHECK(!r.drain()&&!r.pr.written()&&r.pr.write_failures()==1);
    }
}

static void test_waiting_and_stop() {
    {   // no anchor: waits, logs, never reads; aborts after the wait cap with a report
        Ps2Prober::Options o=opts();o.wait_cap_ms=3000;
        RUN(r,o);r.have=false;
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_aborted&&std::string(r.pr.reason())=="never-anchor"&&r.mem.reads==0);
        r.drain();CHECK(r.out.files.size()==1&&file_hash_ok(r.out.files[0])&&!r.pr.coverage_known());
    }
    {   // anchor must be stable: a changed lo restarts the count
        Ps2Prober::Options o=opts();o.settle_ticks=10;
        RUN(r,o);plant(r,false);
        for(unsigned i=0;i<9;++i)r.tick();
        for(unsigned i=0;i<5;++i)r.work();
        CHECK(r.pr.phase()==Ps2Prober::p_waiting);
        r.lo=B0+0x1000;r.tick();r.work();                                                                // another lo: count restarts, still waiting
        CHECK(r.pr.phase()==Ps2Prober::p_waiting);
        r.lo=B0;for(unsigned i=0;i<12;++i)r.tick();
        r.work();CHECK(r.pr.phase()==Ps2Prober::p_scanning);
        // duplicate frame numbers count once
        RUN(q,o);for(unsigned i=0;i<30;++i){q.pr.tick(q.in());}                                          // the same frame 30 times
        q.work();CHECK(q.pr.phase()==Ps2Prober::p_waiting);
    }
    {   // the anchor block is not readable data: aborted, nothing scanned, a report
        RUN(r,opts());r.mem.segs[0].committed=false;
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_aborted&&std::string(r.pr.reason())=="anchor-block-not-readable"&&r.pr.scanned_bytes()==0);
        r.drain();CHECK(r.out.files.size()==1&&file_hash_ok(r.out.files[0]));
    }
    {   // stop file during sampling: partial report, UNKNOWN
        RUN(r,opts());plant(r,false);
        r.hook=[](Run& x){if(x.tStart>0&&(x.now-x.tStart)>15000)x.stop=true;};
        r.all();
        CHECK(r.pr.phase()==Ps2Prober::p_stopped&&r.pr.stage_done()>5&&r.pr.stage_done()<pp_stage_samples);
        r.drain();CHECK(r.out.files.size()==1&&file_hash_ok(r.out.files[0])&&!r.pr.coverage_known());
        CHECK(r.sink.find("event=coverage;")->find("ended-early")!=std::string::npos);
    }
    {   // stop via the Present tick flag
        RUN(r,opts());plant(r,false);
        r.ticks(5);r.work();r.stop=true;r.tick();r.stop=false;r.work();
        CHECK(r.pr.phase()==Ps2Prober::p_stopped);
    }
    {   // exit path: finish() + drain() with no further work() call still writes the partial report
        RUN(r,opts());plant(r,false);
        r.ticks(5);for(unsigned i=0;i<3;++i)r.work();
        r.pr.finish("process-exit");
        CHECK(r.drain()&&r.out.files.size()==1&&std::string(r.pr.reason())=="process-exit"&&file_hash_ok(r.out.files[0]));
        CHECK(!r.drain());
    }
    {   // disabled: inert
        Ps2Prober::Options o=opts();o.enabled=false;
        RUN(r,o);r.ticks(5);for(unsigned i=0;i<10;++i)r.work();
        CHECK(r.pr.phase()==Ps2Prober::p_off&&r.mem.reads==0&&r.mem.queries==0&&r.sink.lines.empty()&&!r.drain());
    }
    {   // a second run with the same inputs gives the same file hash (deterministic report)
        RUN(a,opts());plant(a);a.all();a.drain();
        RUN(b,opts());plant(b);b.all();b.drain();
        CHECK(a.out.files.size()==1&&b.out.files.size()==1&&std::string(a.pr.file_sha256())==b.pr.file_sha256());
    }
}

static void test_read_only_everywhere() {
    RUN(r,opts(4096));plant(r);
    const uint64_t sum=r.mem.checksum();
    r.hook=[](Run& x){if(x.tStart>0&&(x.now-x.tStart)>10000)x.mem.unreadable.assign({{B0+0x900000,B0+0x901000}});};
    r.all();r.drain();
    CHECK(r.mem.writeCalls==0&&r.mem.checksum()==sum&&r.mem.reads>0);
}

static void test_config() {
    CHECK(!Config{}.ee_ps2_probe());
    CHECK(!parse_config("").config.ee_ps2_probe());
    CHECK(parse_config("ee_ps2_probe=true\n").config.ee_ps2_probe());
    CHECK(parse_config("ee_ps2_probe=1\n").config.ee_ps2_probe());
    CHECK(!parse_config("ee_ps2_probe=0\n").config.ee_ps2_probe());
    CHECK(parse_config("ee_ps2_probe=true\n").issue==ConfigIssue::none);
    for(const char* bad:{"ee_ps2_probe=maybe\n","ee_ps2_probe=\n"})CHECK(parse_config(bad).issue!=ConfigIssue::none);
    const std::string base=
        "log_enabled=true\ndiagnostic_ownership_pins=true\ngpu_copy_witness_observation=true\ngpu_copy_early_device_hook=true\n"
        "gpu_copy_step3_experiment=true\ngpu_copy_step3_whole_set=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_members_by_class=true\n";
    {   // the latch: opt-in gate, logging, ee_camera_probe and the key itself; independent of ee_lens_scan
        auto r=parse_config((base+"ee_camera_probe=true\nee_ps2_probe=true\n").c_str());
        CHECK(r.issue==ConfigIssue::none&&r.config.ee_ps2_probe());
        CHECK(r.config.ps2_probe_latch(true)&&!r.config.ps2_probe_latch(false)&&!r.config.lens_scan_latch(true));
        CHECK(!parse_config((base+"ee_ps2_probe=true\n").c_str()).config.ps2_probe_latch(true));
        CHECK(!parse_config((base+"ee_camera_probe=true\n").c_str()).config.ps2_probe_latch(true));
        CHECK(!parse_config("ee_camera_probe=true\nee_ps2_probe=true\nlog_enabled=false\n").config.ps2_probe_latch(true));
        for(const char* w:{"ee_camera_write_trials=true\n","ee_camera_look_write_trial=true\n","ee_camera_look_drive=true\n"})
            CHECK(!parse_config((base+"ee_camera_probe=true\nee_ps2_probe=true\n"+w).c_str()).config.ps2_probe_latch(true));
        CHECK(parse_config((base+"ee_camera_probe=true\nee_ps2_probe=true\nee_camera_look_write_trial=false\n").c_str()).config.ps2_probe_latch(true));
    }
    {   // budgets: the probe-only set (10 keys) and the full headset set + probe + this key (26 of 32 keys) stay inside 32 keys / 2048 bytes
        const std::string probe=base+"ee_camera_probe=true\nee_ps2_probe=true\n";
        size_t pk=0;for(char ch:probe)pk+=ch=='\n';
        CHECK(pk==10&&probe.size()<2048&&parse_config(probe.c_str()).issue==ConfigIssue::none);
        const std::string headset=base+"xr_stereo_output=true\ngpu_copy_step5_hmd_fov=true\ngpu_copy_step5_hud=true\nxr_hud_layer=true\nxr_hud_quad_distance_m=0.5\n"
            "xr_hud_scale=0.5\nxr_world_quad_distance_m=500\ngpu_copy_step5_units_per_metre=65\nxr_world_in_projection=true\ngpu_copy_step5_world_objects=true\n"
            "gpu_copy_step5_hud_markers=true\nxr_hud_markers_layer=true\ngpu_copy_step5_effects=true\nxr_effects_layer=true\nxr_world_hold_last=true\nxr_world_extend=true\n"
            "ee_camera_probe=true\nee_ps2_probe=true\n";
        size_t keys=0;for(char ch:headset)keys+=ch=='\n';
        CHECK(keys==26&&keys<=32&&headset.size()<2048&&parse_config(headset.c_str()).issue==ConfigIssue::none);
    }
}

int main() {
    test_happy();
    test_drift_and_mission_change();
    test_boundaries();
    test_caps();
    test_waiting_and_stop();
    test_read_only_everywhere();
    test_config();
    std::printf("ee_ps2_probe_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
