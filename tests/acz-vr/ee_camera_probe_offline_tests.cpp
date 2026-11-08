// J-w1-ee-camera offline fixtures: fake 64 MiB EE blocks (sparse), a fake game
// frame with switchable models, the seam order of the live hooks, and the real
// SEH-guarded read paths on private pages. No GPU, no game, no XR.
#include "ee_camera_probe.h"
#include "ee_look_write.h"
#include <functional>
#include "ee_camera_diff.h"
#include "ee_camera_logfile.h"
#include <algorithm>
#include <array>
#include <random>
#include "ee_camera_guarded.h"
#include "runtime_config.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>

using namespace acz::ee_camera;
namespace step5=acz::gpu_copy::step5;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)

// ------------------------------------------------------------------ fakes
struct FakeMemory final:Memory {
    struct Block {uint64_t base;std::unordered_map<uint32_t,uint32_t> w;uint64_t size=block_bytes;uint8_t type=rt_private;};
    std::vector<Block> blocks;bool failReads=false,failWrites=false;
    struct W {uint64_t a;float v;};std::vector<W> writes;
    Block* find(uint64_t a,uint32_t n) {for(auto& b:blocks)if(a>=b.base&&a+n<=b.base+b.size)return &b;return nullptr;}
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        if(failReads)return false;Block* b=find(a,n);if(!b)return false;
        if(n>256){std::memset(out,0,n);const uint32_t lo=uint32_t(a-b->base);
            for(auto& e:b->w)if(e.first>=lo&&e.first+4<=lo+n)std::memcpy(static_cast<char*>(out)+(e.first-lo),&e.second,4);return true;}
        for(uint32_t i=0;i<n;i+=4){auto it=b->w.find(uint32_t(a-b->base)+i);const uint32_t v=it==b->w.end()?0:it->second;std::memcpy(static_cast<char*>(out)+i,&v,4);}
        return true;
    }
    bool write_float(uint64_t a,float v) noexcept override {
        if(failWrites)return false;Block* b=find(a,4);if(!b)return false;b->w[uint32_t(a-b->base)]=bits(v);writes.push_back({a,v});return true;
    }
    void set(uint64_t base,uint32_t off,float v){auto* b=find(base+off,4);if(b)b->w[uint32_t(base+off-b->base)]=bits(v);}
    void setw(uint64_t a,uint32_t v){auto* b=find(a,4);if(b)b->w[uint32_t(a-b->base)]=v;}
    float get(uint64_t base,uint32_t off){auto* b=find(base+off,4);if(!b)return 0.f;auto it=b->w.find(uint32_t(base+off-b->base));return it==b->w.end()?0.f:from_bits(it->second);}
};
struct FakeScanner final:Scanner {
    std::vector<Region> regions;
    void scan(CandidateCollector& out) noexcept override {for(auto& r:regions)out.add(r);}
    void block(uint64_t base){Region r;r.base=base;r.size=block_bytes;r.allocation_base=base;r.committed=r.is_private=r.read_write=true;regions.push_back(r);}
};
struct FakeSink final:Sink {
    std::vector<std::string> lines;unsigned important=0;
    void line(bool imp,const char* t) noexcept override {lines.emplace_back(t);important+=imp;if(std::getenv("EE_CAMERA_VERBOSE"))std::printf("%s%s\n",imp?"! ":"  ",t);}
    const std::string* find(const char* s) const {for(auto& l:lines)if(l.find(s)!=std::string::npos)return &l;return nullptr;}
    unsigned count(const char* s) const {unsigned n=0;for(auto& l:lines)n+=l.find(s)!=std::string::npos;return n;}
};

// Camera -> live SrtData (M0 unused, M64 = VP, M256 = identity).
static void build_srt(uint8_t* srt,const double eye[3],double yaw,double pitch,double roll,double airYaw=0,bool rotate=false) {
    const double fw[3]={std::cos(pitch)*std::sin(yaw),std::sin(pitch),std::cos(pitch)*std::cos(yaw)};
    const double r0[3]={std::cos(yaw),0,-std::sin(yaw)};
    const double u0[3]={fw[1]*r0[2]-fw[2]*r0[1],fw[2]*r0[0]-fw[0]*r0[2],fw[0]*r0[1]-fw[1]*r0[0]};
    double r[3],u[3];for(int k=0;k<3;++k){r[k]=std::cos(roll)*r0[k]+std::sin(roll)*u0[k];u[k]=-std::sin(roll)*r0[k]+std::cos(roll)*u0[k];}
    step5::Camera c{};c.n3=1;c.A=1.0;c.C=-0.1;
    double vp[16];step5::build_vp(eye,r,u,fw,-0.6152,0.6152,0.3460,-0.3460,c,vp);
    // M256 = cockpit local->world: identity, or (rotate) a rotation about y by the aircraft yaw
    // (local +z -> (sin, 0, cos)); M64 = M256 * VP so that inverse(M256) * M64 == VP.
    double m256d[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    if(rotate){const double c=std::cos(airYaw),sn=std::sin(airYaw);
        const double rm[16]={c,0,-sn,0, 0,1,0,0, sn,0,c,0, 0,0,0,1};std::memcpy(m256d,rm,sizeof rm);}
    double m64d[16];step5::mul44(m256d,vp,m64d);
    float m64[16],m256[16];for(int i=0;i<16;++i){m64[i]=float(m64d[i]);m256[i]=float(m256d[i]);}
    std::memset(srt,0,416);std::memcpy(srt,m64,64);std::memcpy(srt+64,m64,64);std::memcpy(srt+256,m256,64);
}

// The fake game: one EE frame per Present interval, run between seam A and
// seam B (the synchronous order assumed in the findings file).
struct FakeGame {
    FakeMemory& m;uint64_t base;
    bool recomputeLook=false;      // (a) look fields = pad at EE frame start (overwrites a write before use)
    bool resetAfterCompose=false;  // compose from the field, THEN look = pad (write consumed, readback reverted)
    bool imageIgnoresField=false;  // composed fields follow, the image uses a pad-only camera (render copy / other consumer)
    bool composeInternal=false;    // composition uses an internal copy: field never consumed (ii)
    bool flicker=false;            // recompute only on even EE frames
    bool recomposePosition=true;   // camera position = aircraft + eye offset every frame
    bool frozen=false,rotateCockpit=false;double turn=0;
    bool eyeRel16=false;           // the recovered eye is 16 x (camera - aircraft) as in the live game (rel_eye holds, score 5)
    bool lagCompose=false;         // J-look-locator: the camera yaw is composed from the PREVIOUS frame's look (a ramp lags one frame)
    float prevYaw=0;
    double eyeUp=3.6,eyeAhead=36,yawBreak=0;bool degrees=false; // layout/units variants
    uint64_t block=0;
    double offX=0,offZ=0;uint32_t lastX=0,lastZ=0;
    float pad[3]{};double hdg=-0.7854,ax=-29925,ay=4000,az=137590;unsigned n=0;uint8_t srt[416]{};
    FakeGame(FakeMemory& mm,uint64_t b,int64_t shift=0):m(mm),base(uint64_t(int64_t(b)+shift)),block(b) {m.blocks.push_back({b,{}});}
    static bool angular(unsigned f){return f==look_pitch||f==look_yaw||f==look_roll||f==heading||f==cam_pitch||f==cam_yaw||f==cam_roll||f==look_yaw_copy;}
    void put(unsigned f,double v){m.set(base,field_offset[f],float(degrees&&angular(f)?v*180/pi:v));}
    float at(unsigned f){const float v=m.get(base,field_offset[f]);return degrees&&angular(f)?float(v*pi/180):v;}
    void init(){for(unsigned i=0;i<3;++i)put(look_pitch+i,0);frame(true);}
    void frame(bool first=false) {
        if(frozen&&!first)return;
        // Persistent model: the camera keeps its (double) offset from the aircraft;
        // a foreign write to the field is adopted as the new offset.
        if(!first&&bits(at(cam_x))!=lastX)offX=double(at(cam_x))-ax;
        if(!first&&bits(at(cam_z))!=lastZ)offZ=double(at(cam_z))-az;
        ++n;hdg=wrap(hdg+turn);ax+=1.5;az-=1.2;
        if(!recomposePosition&&!first){put(cam_x,ax+offX);put(cam_z,az+offZ);lastX=bits(at(cam_x));lastZ=bits(at(cam_z));}
        float look[3]={at(look_pitch),at(look_yaw),at(look_roll)};
        if(recomputeLook&&(!flicker||n%2==0)){for(int i=0;i<3;++i){put(look_pitch+i,pad[i]);look[i]=pad[i];}}
        const float* used=composeInternal?pad:look;
        put(air_x,ax);put(air_y,ay);put(air_z,az);put(heading,hdg);
        const float yawUsed=lagCompose&&!first?prevYaw:used[1];prevYaw=used[1];
        put(cam_pitch,used[0]);put(cam_yaw,wrap(hdg+yawUsed+yawBreak));put(cam_roll,used[2]);put(look_yaw_copy,used[1]);
        if(recomposePosition||first){put(cam_x,ax+eyeAhead*std::sin(hdg));put(cam_y,ay+eyeUp);put(cam_z,az+eyeAhead*std::cos(hdg));
            offX=eyeAhead*std::sin(hdg);offZ=eyeAhead*std::cos(hdg);lastX=bits(at(cam_x));lastZ=bits(at(cam_z));}
        else put(cam_y,ay+eyeUp);
        const float rc[9]={at(cam_x),at(cam_y),at(cam_z),at(cam_pitch),at(cam_yaw),at(cam_roll),0,0,1};
        for(unsigned i=0;i<9;++i)m.set(base,render_offset+4*i,rc[i]);
        if(resetAfterCompose)for(int i=0;i<3;++i)put(look_pitch+i,pad[i]);
        double eye[3],y,p,r;
        if(imageIgnoresField){eye[0]=ax+eyeAhead*std::sin(hdg);eye[1]=ay+eyeUp;eye[2]=az+eyeAhead*std::cos(hdg);y=wrap(hdg+pad[1]);p=pad[0];r=pad[2];}
        else {eye[0]=at(cam_x);eye[1]=at(cam_y);eye[2]=at(cam_z);y=at(cam_yaw);p=at(cam_pitch);r=at(cam_roll);}
        if(eyeRel16){eye[0]=16*(at(cam_x)-ax);eye[1]=16*(at(cam_y)-ay);eye[2]=16*(at(cam_z)-az);}
        build_srt(srt,eye,y,p,r,hdg,rotateCockpit);
    }
};

// Synchronous stand-in for the worker-thread scan service.
struct FakeRequester final:ScanRequester {
    Memory& mem;Sink& log;std::vector<float> scratch=std::vector<float>(scan_scratch_floats);ScanResult r{};bool ready=false;unsigned requests=0;
    FakeRequester(Memory& m,Sink& s):mem(m),log(s) {}
    // block 0 = a wide pass over every fake region (sorted), as in production.
    std::vector<RegionInfo> regions;WideCursor cur{};uint64_t budget=~0ull;
    bool request(uint64_t block,const Basis* basis,bool dump) noexcept override {
        if(ready)return false;++requests;(void)block;
        auto& fm=static_cast<FakeMemory&>(mem);
        regions.clear();for(auto& b:fm.blocks){RegionInfo g;g.base=b.base;g.size=b.size;g.alloc=b.base;g.type=b.type;g.protect=4;g.writable=true;regions.push_back(g);}
        std::sort(regions.begin(),regions.end(),[](const RegionInfo& x,const RegionInfo& y){return x.base<y.base;});
        if(dump)for(auto& g:regions)if(g.size==block_bytes)dump_e6(mem,g.base,log,"scan");
        r=ScanResult{};wide_pass(mem,regions.data(),unsigned(regions.size()),basis,scratch.data(),budget,cur,r,log);log_scan(r,log);ready=true;return true;
    }
    const ScanResult* poll() noexcept override {if(!ready)return nullptr;ready=false;return &r;}
};
struct FakeWorld final:WorldPathObserver {bool observe(uint64_t,WorldPathResponse&) noexcept override {return false;}};
static FakeWorld g_world;
static Options trialOpts(Options o=Options{}) {o.write_trials=true;o.world=&g_world;return o;}
static Options observeOpts() {return Options{};}
struct Rig {
    FakeMemory mem;FakeScanner scan;FakeSink sink;FakeRequester req{mem,sink};Shared sh;BasisMailbox mbox;std::unique_ptr<Experiment> exp;
    std::vector<std::unique_ptr<FakeGame>> games;std::function<void()> afterTick;double now=0;bool basisOn=true,eeAfterBOdd=false;unsigned ticks=0;
    ScanRequester* reqp=nullptr;   // J-look-locator replay: a scripted requester instead of the real wide pass
    explicit Rig(Options o=trialOpts()) {exp=std::make_unique<Experiment>(o);sh.position_allowed.store(o.position);}
    FakeGame& add(uint64_t block,bool listed=true,int64_t shift=0){games.push_back(std::make_unique<FakeGame>(mem,block,shift));if(listed)scan.block(block);
        games.back()->init();return *games.back();}
    void interval() {
        Basis b;const bool hb=mbox.try_read(b);
        exp->tick(mem,sh,scan,reqp?*reqp:static_cast<ScanRequester&>(req),sink,now,hb?&b:nullptr);
        if(afterTick)afterTick();
        const uint64_t us=uint64_t(now*1000);
        seam_observe(sh,seam_A,mem,us,1,mbox.published());seam_apply(sh,seam_A,mem);
        const bool late=eeAfterBOdd&&(ticks%2);
        if(!late)for(auto& g:games)g->frame();
        if(basisOn&&!games.empty())mbox.publish(games[0]->srt);
        seam_observe(sh,seam_B,mem,us+5000,2,mbox.published());seam_apply(sh,seam_B,mem);
        if(late)for(auto& g:games)g->frame();
        seam_observe(sh,seam_C,mem,us+9000,2,mbox.published());seam_apply(sh,seam_C,mem);
        now+=1000.0/60;++ticks;
    }
    void runFor(unsigned n){for(unsigned i=0;i<n;++i)interval();}
    bool finished() const {auto p=exp->phase();return p==Experiment::done||p==Experiment::aborted||p==Experiment::refused;}
    void run(unsigned maxTicks){for(unsigned i=0;i<maxTicks&&!finished();++i)interval();}
    const TrialResult* result(uint8_t fam,uint32_t seam,unsigned nth=0) const {
        for(unsigned i=0;i<exp->trial_count();++i){const auto& r=exp->result(i);if(r.done&&r.family==fam&&r.seam==seam&&nth--==0)return &r;}
        return nullptr;
    }
    // Every write: listed offsets of the one validated block; angle writes within the bound of the pad value 0.
    bool bounded(uint64_t base,bool position) const {
        for(auto& w:mem.writes) {
            if(w.a<base||w.a>=base+block_bytes)return false;
            const uint32_t off=uint32_t(w.a-base);
            const bool ang=off==field_offset[look_pitch]||off==field_offset[look_yaw]||off==field_offset[look_roll];
            const bool pos=off==field_offset[cam_x]||off==field_offset[cam_z];
            if(!(ang||(position&&pos)))return false;
            if(ang&&std::fabs(w.v)>angle_bound+1e-6)return false;
        }
        return true;
    }
};
static uint8_t verdict(const TrialResult* r){return r?r->rec.verdict:255;}

// ------------------------------------------------------------------ unit tests
static void test_collector() {
    CandidateCollector c;Region r;
    auto reg=[&](uint64_t base,uint64_t size,uint64_t alloc,bool commit=true,bool priv=true,bool rw=true){r.base=base;r.size=size;r.allocation_base=alloc;r.committed=commit;r.is_private=priv;r.read_write=rw;c.add(r);};
    reg(0x10000000,block_bytes,0x10000000);                               // one region, exact
    reg(0x20000000,block_bytes/2,0x20000000);reg(0x22000000,block_bytes/2,0x20000000); // split, same allocation
    reg(0x30000000,block_bytes/2,0x30000000);reg(0x32000000,block_bytes/2,0x30000000,false); // half reserved
    reg(0x40000000,block_bytes/2,0x40000000);                              // 32 MiB
    reg(0x50000000,block_bytes,0x50000000,true,false);                     // mapped/image, not private
    reg(0x60000000,block_bytes,0x60000000,true,true,false);                // read-only
    reg(0x70000000,block_bytes,0x70000000);reg(0x74000000,0x1000,0x70000000); // 64 MiB + 4 KiB
    c.finish();
    CHECK(c.total()==2);CHECK(c.base(0)==0x10000000);CHECK(c.base(1)==0x20000000);
    CandidateCollector many;for(unsigned i=0;i<10;++i){r.base=r.allocation_base=0x100000000ull+i*0x10000000ull;r.size=block_bytes;r.committed=r.is_private=r.read_write=true;many.add(r);}
    many.finish();CHECK(many.total()==10);CHECK(many.count()==CandidateCollector::cap);
}
static void test_validate() {
    FakeMemory m;FakeGame g(m,0x7000000000ull);g.init();g.frame();Snapshot s;
    CHECK(read_snapshot(m,g.base,s));CHECK(validate(s)==nullptr);
    Snapshot b=s;b.v[cam_yaw]+=0.01f;CHECK(std::string(validate(b))=="camera-yaw-relation");
    CHECK(validate(b,0.02)==nullptr);
    b=s;b.v[cam_y]=b.v[air_y]+5.f;CHECK(validate(b)==nullptr); // D-241 relaxed band [1, 8]
    b=s;b.v[cam_y]=b.v[air_y]+9.f;CHECK(std::string(validate(b))=="eye-height");CHECK(validate(b,0,1.5)==nullptr);
    b=s;b.v[cam_x]=b.v[air_x]+70.f;b.v[cam_z]=b.v[air_z];CHECK(std::string(validate(b))=="eye-horizontal");
    b=s;b.v[air_x]=0;b.v[air_z]=0;CHECK(std::string(validate(b))=="aircraft-at-origin");
    b=s;b.v[look_yaw]=std::nanf("");CHECK(std::string(validate(b))=="nonfinite");
    b=s;b.v[cam_pitch]=7.f;CHECK(std::string(validate(b))=="rotation-range");
    b=s;b.v[air_y]=-5000.f;b.v[cam_y]=-4996.4f;CHECK(std::string(validate(b))=="altitude-range");
    // Yaw relation across the wrap: heading +3.1, look +0.1 -> camera -3.083
    b=s;b.v[heading]=3.1f;b.v[look_yaw]=0.1f;b.v[cam_yaw]=float(wrap(3.2));CHECK(validate(b)==nullptr);
    Snapshot z{};CHECK(validate(z)!=nullptr); // an all-zero block fails (eye height 0)
    CHECK(!read_snapshot(m,0x1000,s)); // unmapped: read fails, no crash
}
static void test_locator() {
    // 0 candidates: never locks, no writes, refused after the 20-minute cap.
    {Rig rig;rig.run(60*60*21);CHECK(rig.exp->phase()==Experiment::refused);CHECK(rig.mem.writes.empty());
     CHECK(rig.sink.find("not-located-within-20-min"));CHECK(rig.sh.base.load()==0);}
    // 2 candidates, one valid: locks on the valid one.
    {Rig rig;rig.add(0x7100000000ull);FakeMemory::Block junk{0x7200000000ull,{}};rig.mem.blocks.push_back(junk);rig.scan.block(0x7200000000ull);
     for(unsigned i=0;i<60*8&&rig.exp->phase()==Experiment::searching;++i)rig.interval();
     CHECK(rig.exp->phase()==Experiment::stabilising);CHECK(rig.sh.base.load()==0x7100000000ull);CHECK(rig.sink.find("event=located;base=7100000000"));
     CHECK(rig.mem.writes.empty());}
    // 2 valid candidates: refused, never writes.
    {Rig rig;rig.add(0x7100000000ull);rig.add(0x7300000000ull);rig.run(2000);
     CHECK(rig.exp->phase()==Experiment::refused);CHECK(rig.sink.find("ambiguous-several-candidates-validate"));CHECK(rig.mem.writes.empty());CHECK(rig.sh.base.load()==0);}
    // 3 candidates, one valid, one unreadable (listed but unmapped): locks.
    {Rig rig;FakeMemory::Block junk{0x7200000000ull,{}};rig.mem.blocks.push_back(junk);rig.scan.block(0x7200000000ull);
     rig.scan.block(0x7400000000ull);rig.add(0x7100000000ull);
     for(unsigned i=0;i<60*8&&rig.exp->phase()==Experiment::searching;++i)rig.interval();
     CHECK(rig.exp->phase()==Experiment::stabilising);CHECK(rig.sh.base.load()==0x7100000000ull);CHECK(rig.sink.find("total=3"));}
    // 3 candidates, all valid: refused.
    {Rig rig;rig.add(0x7100000000ull);rig.add(0x7300000000ull);rig.add(0x7500000000ull);rig.run(60*10);
     CHECK(rig.exp->phase()==Experiment::refused);CHECK(rig.mem.writes.empty());}
    // Validation needs 10 CONSECUTIVE frames: a broken relation resets the streak.
    {FakeMemory m;FakeGame g(m,0x7100000000ull);g.init();Locator L;L.add(g.base,g.base);
     for(int i=0;i<9;++i){g.frame();L.step(m);}
     g.put(cam_yaw,1.0);L.step(m);CHECK(L.state()==Locator::searching);CHECK(L.streak(0)==0);
     for(int i=0;i<9;++i){g.frame();L.step(m);}CHECK(L.state()==Locator::searching);
     g.frame();L.step(m);CHECK(L.state()==Locator::locked);
     // a static copy (aircraft never moves) never locks
     FakeMemory m2;FakeGame s2(m2,0x7100000000ull);s2.init();Locator L2;L2.add(s2.base,s2.base);
     for(int i=0;i<30;++i)L2.step(m2);CHECK(L2.state()==Locator::searching);CHECK(std::string(L2.why(0))=="static-not-tracking");}
}
static void test_seam_bounds() {
    FakeMemory m;FakeGame g(m,0x7100000000ull);g.init();Shared sh;
    Command c;c.seam=seam_B;c.mode=mode_absolute;c.n=1;c.field[0]=look_yaw;c.value[0]=1.0f;c.lo[0]=-0.35f;c.hi[0]=0.35f;
    sh.cmd.publish(c);
    CHECK(!seam_apply(sh,seam_B,m)); // base 0: never before validation
    sh.base.store(g.base);sh.block_lo.store(g.base);sh.block_hi.store(g.base+block_bytes);
    CHECK(!seam_apply(sh,seam_B,m)); // writes not enabled (production: never)
    sh.writes_enabled.store(true);
    CHECK(!seam_apply(sh,seam_A,m)); // wrong seam
    CHECK(seam_apply(sh,seam_B,m));CHECK(g.at(look_yaw)==0.35f); // clamped to the hard bound
    CHECK(!seam_apply(sh,seam_B,m)); // once per interval
    sh.frame.fetch_add(1);
    // delta accumulates and clamps
    c.mode=mode_delta;c.step[0]=0.1745f;sh.cmd.publish(c);g.put(look_yaw,0);
    for(int i=0;i<5;++i){CHECK(seam_apply(sh,seam_B,m));sh.frame.fetch_add(1);}
    CHECK(std::fabs(g.at(look_yaw)-0.35f)<1e-6f);
    // a non-listed field is refused (heading), a position field needs the key
    c.mode=mode_absolute;c.field[0]=heading;sh.cmd.publish(c);const size_t before=m.writes.size();
    CHECK(!seam_apply(sh,seam_B,m));sh.frame.fetch_add(1);
    c.field[0]=cam_x;c.anchor[0]=air_x;c.value[0]=1;c.lo[0]=-5;c.hi[0]=5;sh.cmd.publish(c);
    CHECK(!seam_apply(sh,seam_B,m));sh.position_allowed.store(true);CHECK(seam_apply(sh,seam_B,m));
    CHECK(std::fabs(double(g.at(cam_x))-(double(g.at(air_x))+1.0))<0.01); // anchored to the aircraft now
    c.anchor[0]=heading;sh.cmd.publish(c);sh.frame.fetch_add(1);CHECK(!seam_apply(sh,seam_B,m)); // anchor not allowed
    CHECK(m.writes.size()==before+1);
    // disarmed: no write; fault flags
    sh.cmd.disarm();sh.frame.fetch_add(1);CHECK(!seam_apply(sh,seam_B,m));
    c.field[0]=look_yaw;c.anchor[0]=no_anchor;c.lo[0]=-1;c.hi[0]=1;c.value[0]=0.1f;sh.cmd.publish(c);m.failWrites=true;
    CHECK(!seam_apply(sh,seam_B,m));CHECK(sh.fault.load()==2);
    sh.fault.store(0);m.failWrites=false;m.failReads=true;sh.frame.fetch_add(1);CHECK(!seam_apply(sh,seam_B,m));CHECK(sh.fault.load()==1);
    CHECK(sh.inflight.load()==0);
}
static void test_classify() {
    float cmd[20],resp[20];int lag[20];
    for(int i=0;i<20;++i){cmd[i]=0.1745f;lag[i]=i;resp[i]=i<2?0.f:0.17f;}
    Stats s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_effect);CHECK(s.lag==2);CHECK(std::fabs(s.ratio-0.974)<0.01);
    for(int i=0;i<20;++i)resp[i]=-0.175f;s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_effect);CHECK(s.ratio<0); // sign convention reported
    for(int i=0;i<20;++i)resp[i]=0.001f;s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_none);CHECK(s.lag==-1);
    for(int i=0;i<20;++i)resp[i]=0.10f;s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_partial); // 57%: consumed but not 1:1
    for(int i=0;i<20;++i)resp[i]=i%2?0.1745f:0.f;s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_flicker);
    for(int i=0;i<20;++i)resp[i]=std::nanf("");s=classify(cmd,resp,lag,20);CHECK(s.verdict==v_no_data);
    CHECK(std::string(hypothesis(v_none,v_none,v_none,0.0,1.0))=="i-overwritten-before-use");
    CHECK(std::string(hypothesis(v_none,v_none,v_none,1.0,0.0))=="ii-composed-before-boundary-or-not-an-input");
    CHECK(std::string(hypothesis(v_none,v_effect,v_effect,1.0,0.0))=="iii-composed-and-copied-but-not-rendered");
    CHECK(std::string(hypothesis(v_effect,v_effect,v_effect,0,1))=="consumed");
}
static void test_guarded() {
    // Real SEH paths on private pages: no access, read-only, freed.
    void* p=VirtualAlloc(nullptr,0x10000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(p!=nullptr);
    float v=1.5f,out=0;
    CHECK(range_ok(uint64_t(uintptr_t(p)),0x10000));CHECK(guarded_store(static_cast<volatile float*>(p),v));
    CHECK(guarded_copy(&out,p,4)&&out==1.5f);
    DWORD old=0;VirtualProtect(p,0x1000,PAGE_NOACCESS,&old);
    CHECK(!range_ok(uint64_t(uintptr_t(p)),4));CHECK(!guarded_copy(&out,p,4));
    CHECK(!guarded_store(static_cast<volatile float*>(p),2.f));
    VirtualProtect(p,0x1000,PAGE_READONLY,&old);
    CHECK(!range_ok(uint64_t(uintptr_t(p)),4));CHECK(guarded_copy(&out,p,4));CHECK(!guarded_store(static_cast<volatile float*>(p),2.f));
    CHECK(!range_ok(uint64_t(uintptr_t(p)),0x2000)); // spans a read-only and a read-write page
    VirtualFree(p,0,MEM_RELEASE);
    CHECK(!range_ok(uint64_t(uintptr_t(p)),4));CHECK(!guarded_copy(&out,p,4));
    CHECK(!guarded_copy(&out,reinterpret_cast<const void*>(uintptr_t(16)),4)); // null page
}
static void test_basis() {
    uint8_t srt[416];const double eye[3]={10,20,30};build_srt(srt,eye,0.5,0.1,0.2);
    BasisMailbox mb;Basis b;CHECK(!mb.try_read(b));mb.publish(srt);CHECK(mb.try_read(b));CHECK(b.seq==1);
    CHECK(std::fabs(basis_yaw(b)-0.5)<1e-4);CHECK(std::fabs(basis_pitch(b)-0.1)<1e-4);CHECK(std::fabs(basis_roll(b)-0.2)<1e-3);
    CHECK(std::fabs(b.eye[0]-10)<1e-2&&std::fabs(b.eye[2]-30)<1e-2);CHECK(std::fabs(b.tanX-0.6152)<1e-4);
    std::memset(srt+256,0,64);mb.publish(srt);CHECK(!mb.try_read(b)); // degenerate M256 -> no basis
}
static void test_config() {
    using acz::diagnostics::parse_config;
    auto r=parse_config("ee_camera_probe=true\nee_camera_probe_position=1\nee_camera_probe_points=CB\n");
    CHECK(r.issue==acz::diagnostics::ConfigIssue::none);CHECK(r.config.ee_camera_probe());CHECK(r.config.ee_camera_probe_position());
    CHECK(r.config.ee_camera_probe_points()==6u);
    r=parse_config("log_enabled=true\n");CHECK(!r.config.ee_camera_probe());CHECK(!r.config.ee_camera_probe_position());CHECK(r.config.ee_camera_probe_points()==7u);
    CHECK(!r.config.ee_camera_write_trials());
    CHECK(parse_config("ee_camera_write_trials=true\n").config.ee_camera_write_trials());
    for(const char* bad:{"ee_camera_write_trials=maybe\n","ee_camera_probe_points=AA\n","ee_camera_probe_points=D\n","ee_camera_probe_points=ABCA\n","ee_camera_probe=yes\n"})
        CHECK(parse_config(bad).issue==acz::diagnostics::ConfigIssue::out_of_range);
}

// ------------------------------------------------------------------ closed loop
static void common_run_checks(Rig& rig,uint64_t base,bool position) {
    CHECK(rig.bounded(base,position));
    // the run (baseline start .. final) stays within 90 s
    CHECK(rig.sink.find("ee-camera;result="));CHECK(rig.sh.base.load()==0);CHECK(rig.sh.inflight.load()==0);
    // one attempt: further frames never write
    const size_t w=rig.mem.writes.size();for(int i=0;i<300;++i)rig.interval();CHECK(rig.mem.writes.size()==w);
    CHECK(rig.sink.important<=28);
    for(auto& l:rig.sink.lines)CHECK(l.size()<448);
}
static void test_persistent_input() {
    // The look field is a persistent input composed at the EE frame (between A and B): every seam wins.
    Rig rig;auto& g=rig.add(0x7100000000ull);g.turn=0.002;
    rig.run(60*120);
    CHECK(rig.exp->phase()==Experiment::done);
    CHECK(rig.exp->yaw_comp()==-1); // turning aircraft, world-space camera: heading-compensated
    const auto* a=rig.result(fam_yaw_abs,seam_A);CHECK(verdict(a)==v_effect);CHECK(a&&a->rec.lag==0);CHECK(a&&a->comp.verdict==v_effect);
    CHECK(a&&a->persisted>0.9);CHECK(a&&std::string(a->restore)=="restored");CHECK(a&&std::string(a->hyp)=="consumed");
    const auto* b=rig.result(fam_yaw_abs,seam_B);CHECK(verdict(b)==v_effect);CHECK(b&&b->rec.lag==1);
    const auto* c=rig.result(fam_yaw_abs,seam_C);CHECK(verdict(c)==v_effect);CHECK(c&&c->rec.lag==1);
    const auto* d=rig.result(fam_yaw_delta,seam_A);CHECK(verdict(d)==v_effect);CHECK(d&&std::fabs(d->peak_cmd-angle_bound)<1e-3); // accumulated to the clamp
    CHECK(verdict(rig.result(fam_pitch,seam_A,0))==v_effect);CHECK(verdict(rig.result(fam_pitch,seam_A,1))==v_effect);
    CHECK(verdict(rig.result(fam_roll,seam_A))==v_effect);
    CHECK(rig.sink.find("result=LOOK-YAW-INPUT-DRIVES-CAMERA"));
    CHECK(g.at(look_yaw)==0.f&&g.at(look_pitch)==0.f&&g.at(look_roll)==0.f); // restored
    CHECK(rig.sink.find("ee-phase=AB:"));
    common_run_checks(rig,g.base,false);
}
static void test_recompute_each_frame() {
    // (a) the game recomputes the look field from the pad every frame: writes lose.
    Rig rig;auto& g=rig.add(0x7100000000ull);g.recomputeLook=true;
    rig.run(60*120);
    CHECK(rig.exp->phase()==Experiment::done);
    for(uint32_t s:{seam_A,seam_B,seam_C}) {
        const auto* r=rig.result(fam_yaw_abs,s);CHECK(verdict(r)==v_none);CHECK(r&&r->reverted>0.9);
        CHECK(r&&std::string(r->hyp)=="i-overwritten-before-use");
        // A: the EE frame already replaced the value before the restore; B/C: the
        // last write is still there at the boundary, so it is restored.
        CHECK(r&&std::string(r->restore)==(s==seam_A?"game-owned":"restored"));
        const auto* d=rig.result(fam_yaw_delta,s);CHECK(d&&std::fabs(d->peak_cmd-ten_degrees)<1e-4); // never accumulates
    }
    CHECK(rig.sink.find("result=NO-EFFECT"));
    common_run_checks(rig,g.base,false);
}
static void test_compose_then_reset() {
    // The EE composes from the field and only then rewrites it from the pad: the
    // write is consumed although the readback says reverted.
    Rig rig;auto& g=rig.add(0x7100000000ull);g.resetAfterCompose=true;
    rig.run(60*120);
    const auto* a=rig.result(fam_yaw_abs,seam_A);CHECK(verdict(a)==v_effect);CHECK(a&&a->reverted>0.9);CHECK(a&&a->rec.lag==0);
    const auto* b=rig.result(fam_yaw_abs,seam_B);CHECK(verdict(b)==v_effect);CHECK(b&&b->rec.lag==1);
    common_run_checks(rig,g.base,false);
}
static void test_render_consumer_and_not_input() {
    {   // (iii) composed fields follow, the image does not.
        Rig rig;auto& g=rig.add(0x7100000000ull);g.imageIgnoresField=true;rig.run(60*120);
        const auto* a=rig.result(fam_yaw_abs,seam_A);CHECK(verdict(a)==v_none);CHECK(a&&a->comp.verdict==v_effect);
        CHECK(a&&a->rend.verdict==v_effect);CHECK(a&&std::string(a->hyp)=="iii-composed-and-copied-but-not-rendered");
        common_run_checks(rig,g.base,false);
    }
    {   // (ii) our value persists but is never composed.
        Rig rig;auto& g=rig.add(0x7100000000ull);g.composeInternal=true;rig.run(60*120);
        const auto* a=rig.result(fam_yaw_abs,seam_A);CHECK(verdict(a)==v_none);CHECK(a&&a->comp.verdict==v_none);CHECK(a&&a->persisted>0.9);
        CHECK(a&&std::string(a->hyp)=="ii-composed-before-boundary-or-not-an-input");
        CHECK(rig.exp->phase()==Experiment::done); // the widened yaw relation keeps validating
        common_run_checks(rig,g.base,false);
    }
}
static void test_flicker() {
    Rig rig;auto& g=rig.add(0x7100000000ull);g.recomputeLook=true;g.flicker=true;rig.run(60*120);
    CHECK(verdict(rig.result(fam_yaw_abs,seam_A))==v_flicker);
    common_run_checks(rig,g.base,false);
}
static void test_position() {
    {   // default model: the game recomposes camera position each frame -> NONE (E7)
        Options o;o.position=true;o.points=1;Rig rig(trialOpts(o));auto& g=rig.add(0x7100000000ull);rig.run(60*120);
        CHECK(rig.exp->phase()==Experiment::done);
        CHECK(verdict(rig.result(fam_position,seam_A,0))==v_none);CHECK(verdict(rig.result(fam_position,seam_A,1))==v_none);
        CHECK(rig.sink.find("position=A:NONE/NONE"));
        common_run_checks(rig,g.base,true);
    }
    {   // persistent position consumed by the image: EFFECT, anchored to the moving aircraft
        Options o;o.position=true;o.points=1;Rig rig(trialOpts(o));auto& g=rig.add(0x7100000000ull);g.recomposePosition=false;rig.run(60*120);
        const auto* p1=rig.result(fam_position,seam_A,0);const auto* p5=rig.result(fam_position,seam_A,1);
        CHECK(verdict(p1)==v_effect);CHECK(verdict(p5)==v_effect);CHECK(p5&&std::fabs(p5->rec.mean_cmd-5.0)<0.05);
        CHECK(p5&&std::string(p5->restore)=="restored");
        // after restore the camera is back at aircraft + eye offset, not at a stale point
        const double dx=double(g.at(cam_x))-double(g.at(air_x)),dz=double(g.at(cam_z))-double(g.at(air_z));
        CHECK(std::fabs(std::hypot(dx,dz)-36.0)<3.0);
        if(std::fabs(std::hypot(dx,dz)-36.0)>=3.0){std::printf("dx=%g dz=%g\n",dx,dz);for(auto& l:rig.sink.lines)if(l.find("restore")!=std::string::npos||l.find("trial-begin")!=std::string::npos)std::printf("%s\n",l.c_str());}
        common_run_checks(rig,g.base,true);
    }
    {   // position key off: no position trial, and the final line says off
        Rig rig;auto& g=rig.add(0x7100000000ull);rig.run(60*120);CHECK(rig.sink.find("position=off"));
        for(auto& w:rig.mem.writes){const uint32_t off=uint32_t(w.a-g.base);CHECK(off!=field_offset[cam_x]&&off!=field_offset[cam_z]);}
    }
}
static void test_aborts() {
    auto midTrial=[](Rig& rig,unsigned trial){while(!rig.finished()){rig.interval();
        // the write phase of `trial`: at least 30 writes in
        if(rig.exp->phase()==Experiment::running&&rig.mem.writes.size()>30&&rig.sink.count("event=trial-begin")>trial)break;}};
    {   // pause: aircraft frozen -> abort + restore
        Rig rig;auto& g=rig.add(0x7100000000ull);midTrial(rig,1);g.frozen=true;rig.run(600);
        CHECK(rig.exp->phase()==Experiment::aborted);CHECK(std::string(rig.exp->reason())=="paused");
        CHECK(g.at(look_yaw)==0.f);CHECK(rig.sink.find("result=ABORTED:paused"));CHECK(rig.sink.find("status=restored"));
        common_run_checks(rig,g.base,false);
    }
    {   // validation failure (eye height jumps) -> abort
        Rig rig;auto& g=rig.add(0x7100000000ull);midTrial(rig,1);g.ay+=50;g.frame();g.put(cam_y,g.ay+9);
        Basis b;rig.exp->tick(rig.mem,rig.sh,rig.scan,rig.req,rig.sink,rig.now,rig.mbox.try_read(b)?&b:nullptr);
        CHECK(rig.exp->phase()==Experiment::aborted);CHECK(std::string(rig.exp->reason())=="eye-height");
        CHECK(g.at(look_yaw)==0.f);
    }
    {   // first fault on a seam write -> abort
        Rig rig;auto& g=rig.add(0x7100000000ull);midTrial(rig,1);rig.mem.failWrites=true;rig.run(10);
        CHECK(rig.exp->phase()==Experiment::aborted);CHECK(std::string(rig.exp->reason())=="seam-write-fault");
        CHECK(rig.sink.find("status=write-fault"));(void)g;
    }
    {   // cockpit lost: no recovered basis -> abort
        Rig rig;auto& g=rig.add(0x7100000000ull);midTrial(rig,1);rig.basisOn=false;rig.run(60);
        CHECK(rig.exp->phase()==Experiment::aborted);CHECK(std::string(rig.exp->reason())=="cockpit-lost");CHECK(g.at(look_yaw)==0.f);
    }
    {   // no cockpit before the run: never starts, never writes
        Rig rig;rig.basisOn=false;auto& g=rig.add(0x7100000000ull);rig.run(60*30);
        CHECK(rig.exp->phase()==Experiment::searching);CHECK(rig.mem.writes.empty());CHECK(rig.req.requests==0); // scans only in the cockpit
        CHECK(rig.sink.find("scans=0"));CHECK(rig.exp->observed_frames()==0);(void)g;
    }
    {   // stable validation must last 5 s: an interruption restarts the clock
        Rig rig;auto& g=rig.add(0x7100000000ull);
        for(int i=0;i<60*4;++i)rig.interval();
        g.put(cam_yaw,1.0);Basis b;rig.exp->tick(rig.mem,rig.sh,rig.scan,rig.req,rig.sink,rig.now,rig.mbox.try_read(b)?&b:nullptr);
        for(int i=0;i<60*4;++i)rig.interval();
        CHECK(rig.exp->phase()==Experiment::stabilising);CHECK(rig.mem.writes.empty());
        for(int i=0;i<60*2;++i)rig.interval();CHECK(rig.exp->phase()==Experiment::running);
    }
}
static void test_run_cap() {
    Options o;o.position=true;Rig rig(trialOpts(o));auto& g=rig.add(0x7100000000ull);
    auto probe=std::make_unique<Experiment>(o);CHECK(probe->build_trials()==18);
    double total=0;for(unsigned i=0;i<probe->trial_count();++i)total+=probe->trial(i).write_ms+probe->trial(i).settle_ms;
    CHECK(total<=run_cap_ms);
    // every write is <= +-0.35 rad and <= 5 units from the anchored trial-start value
    rig.run(60*120);CHECK(rig.exp->phase()==Experiment::done);(void)g;
}

static bool has_line(const FakeSink& s,const char* a,const char* b) {
    for(auto& l:s.lines)if(l.find(a)!=std::string::npos&&l.find(b)!=std::string::npos)return true;return false;
}
static bool has(const Rig& rig,const char* a,const char* b) {
    for(auto& l:rig.sink.lines)if(l.find(a)!=std::string::npos&&l.find(b)!=std::string::npos)return true;return false;
}
static void test_observe_only_default() {
    // Default: locator + scan + read-only observation; never a write.
    Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.turn=0.001;
    rig.runFor(60*40);
    CHECK(rig.exp->phase()==Experiment::observing);
    CHECK(rig.mem.writes.empty());CHECK(rig.sh.writes.load()==0);CHECK(!rig.sh.writes_enabled.load());
    CHECK(rig.sink.find("mode=observe-only;writes=disabled(ee_camera_write_trials=false)"));
    CHECK(rig.sink.find("event=observe-base;base=7100000000;block=7100000000;kind=e6-unvalidated"));
    CHECK(rig.sink.find("kind=validated"));
    CHECK(rig.sink.count("ee-camera;obs;f=")>=300);CHECK(rig.sink.count("ee-camera;obs2;f=")>=300);
    CHECK(has(rig,"ee-camera;obs;f=",";A=")&&has(rig,"ee-camera;obs;f=",";B="));
    CHECK(rig.sink.find("eye16-vs-cam-air="));CHECK(rig.sink.find(";rcB="));
    CHECK(has(rig,"event=activity;","FDD42C-FDD42C:"));   // aircraft x/y/z change every frame
    CHECK(has(rig,"event=activity;","FDD448-FDD448:"));   // heading (turning)
    CHECK(has(rig,"event=observe-summary","ordering=AB:share=1.000"));CHECK(has(rig,"event=observe-summary","demonstrated=1"));
    CHECK(has(rig,"event=observe-summary","between-seams-changed=AB:"));
    CHECK(rig.sink.find("event=hexdump;tag=discovery;base=7100000000;off=+FDD400;hex="));
    CHECK(rig.sink.find("event=hexdump;tag=scan"));
    CHECK(rig.exp->observed_frames()>2000);
    for(auto& l:rig.sink.lines)CHECK(l.size()<448);
}
static void test_write_gates() {
    {   // key on, no independent world-path observer (production): refused, no writes
        Options o;o.write_trials=true;Rig rig(o);rig.add(0x7100000000ull);rig.runFor(60*20);
        CHECK(rig.exp->phase()==Experiment::observing);CHECK(rig.mem.writes.empty());
        CHECK(rig.sink.find("write-trials-refused;reason=no-world-path-observer"));
    }
    {   // ordering not demonstrated (EE frame lands between A-B or B-C): refused, no writes
        Rig rig;rig.eeAfterBOdd=true;rig.add(0x7100000000ull);rig.runFor(60*25);
        CHECK(rig.exp->phase()==Experiment::observing);CHECK(rig.mem.writes.empty());
        CHECK(rig.sink.find("write-trials-refused;reason=ordering-not-demonstrated"));
        CHECK(!ordering(rig.sh).demonstrated);
    }
    {   // ordering demonstrated + observer: trials start, gated by writes_enabled
        Rig rig;rig.add(0x7100000000ull);rig.runFor(60*8);
        CHECK(rig.exp->phase()==Experiment::running);CHECK(rig.sh.writes_enabled.load());CHECK(rig.sink.find("ordering=AB:1.000"));
    }
}
static void test_shifted_layout() {
    for(int64_t shift:{int64_t(-0x123450),int64_t(0x200004)}) {
        Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull,true,shift);rig.runFor(60*10);
        CHECK(rig.exp->phase()==Experiment::observing);CHECK(rig.sh.base.load()==g.base);
        char want[64];std::snprintf(want,sizeof want,"shift=%+lld;",static_cast<long long>(shift));CHECK(rig.sink.find(want));
        CHECK(rig.sink.find("event=scan-full;count=1;"));CHECK(rig.sink.find("kind=validated"));CHECK(rig.mem.writes.empty());
        // the fixed E6 offsets are wrong here: the discovery hexdump shows zeros
        CHECK(rig.sink.find("tag=discovery;base=7100000000;off=+FDD400;hex=0000000000000000"));
    }
}
static void test_eye_offset_variants() {
    {   // different eye offset inside the relaxed bands: validates (the old 3..4 rule would not)
        Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.eyeUp=1.5;g.eyeAhead=50;rig.runFor(60*10);
        CHECK(rig.exp->phase()==Experiment::observing);CHECK(rig.sh.base.load()==g.base);
    }
    {   // eye 9 units up: never validates; reported as a near miss failing vert
        Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.eyeUp=9;rig.runFor(60*10);
        CHECK(rig.exp->phase()==Experiment::searching);CHECK(rig.sh.base.load()==0);CHECK(rig.mem.writes.empty());
        CHECK(has(rig,"event=scan-near;","+FDD410:fail=vert"));CHECK(has(rig,"event=scan-best;rank=0;look-yaw-at=7100000000+FDD410","fail=vert"));
        CHECK(has(rig,"event=scan;","full=0;near="));(void)g;
    }
}
static void test_degrees_flagged() {
    Rig rig;auto& g=rig.add(0x7100000000ull);g.degrees=true;rig.runFor(60*12);
    CHECK(rig.exp->phase()==Experiment::searching);CHECK(rig.mem.writes.empty());CHECK(rig.sh.base.load()==0);
    CHECK(has(rig,"event=scan-degrees;flag=angles-in-degrees-never-written","+FDD410"));
    CHECK(has(rig,"event=scan;","degrees=1;"));(void)g;
}
static void test_near_miss_and_counts() {
    Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.yawBreak=0.5;rig.runFor(60*6);
    CHECK(rig.exp->phase()==Experiment::searching);CHECK(rig.mem.writes.empty());
    CHECK(has(rig,"event=scan-near;","+FDD410:fail=yaw"));CHECK(has(rig,"event=scan-best;rank=0;look-yaw-at=7100000000+FDD410","fail=yaw"));
    const std::string* l=rig.sink.find("event=scan;");CHECK(l&&l->find("full=0;")!=std::string::npos);
    CHECK(l&&l->find("alone=sane:")!=std::string::npos);
    // search keeps re-running: every 2 s in the cockpit
    CHECK(rig.req.requests>=3);(void)g;
}
static void test_eye_proportional_route() {
    Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.frozen=true;
    // plant the recovered eye / 16 somewhere else in the block (DrawCPT units = 16 x PS2 units hypothesis)
    const double e[3]={g.at(cam_x),g.at(cam_y),g.at(cam_z)};
    for(int k=0;k<3;++k)rig.mem.set(g.block,0x200000+4*k,float(e[k]/16));
    rig.runFor(3);
    CHECK(has(rig,"event=scan-eye-proportional;","+FDD860:r=1"));CHECK(has(rig,"event=scan-eye-proportional;","+200000:r=0.0625"));
    CHECK(has(rig,"event=scan;","x16:1"));
}

static void test_region_table() {
    RegionInfo r[30];RegionCensus c;c.seen=40;c.kept=30;c.tiny=5;c.own=2;c.gpu=3;
    for(unsigned i=0;i<30;++i){r[i].base=0x10000000ull*(i+1);r[i].size=(i+1)*0x100000ull;r[i].type=uint8_t(1+i%3);r[i].protect=i%2?4:2;r[i].writable=i%2;c.bytes+=r[i].size;}
    FakeSink s;log_region_table(r,30,c,s,true);
    CHECK(s.find("event=region-table;cols=type:count/MiB/writable-count/writable-MiB;seen=40;kept=30;small=5;own=2;gpu=3"));
    CHECK(s.count("event=region-largest;")==4);const std::string* l=s.find("event=region-largest;rank=0;");
    CHECK(l&&l->find("1e0000000:30.00:image")!=std::string::npos); // the largest first
    CHECK(find_region(r,30,0x10000000ull*3+5)==&r[2]);CHECK(find_region(r,30,0x10000000ull*3+0x400000)==nullptr);
}
static void test_wide_pass_budget_and_mapped_region() {
    {   // budget: 3 x 64 MiB regions, 100 MiB per pass -> resumes, completes on the third pass
        FakeMemory m;for(uint64_t b:{0x7100000000ull,0x7200000000ull,0x7300000000ull})m.blocks.push_back({b,{}});
        std::vector<RegionInfo> rs;for(auto& b:m.blocks){RegionInfo g;g.base=b.base;g.size=b.size;rs.push_back(g);}
        std::vector<float> scratch(scan_scratch_floats);WideCursor cur;FakeSink s;
        for(int pass=0;pass<3;++pass){ScanResult r{};wide_pass(m,rs.data(),3,nullptr,scratch.data(),100ull<<20,cur,r,s);
            CHECK(r.regions==1);CHECK(r.complete==(pass==2));}
        CHECK(cur.passes==1);
    }
    {   // the camera structure in a 2 MiB MAPPED region at an unknown offset (no 64 MiB block at all)
        Rig rig(observeOpts());const uint64_t reg=0x6600000000ull;
        auto& g=rig.add(reg,false,int64_t(0x1000)-int64_t(field_offset[look_yaw]));
        rig.mem.blocks.back().size=2u<<20;rig.mem.blocks.back().type=rt_mapped;
        rig.runFor(60*10);
        CHECK(rig.exp->phase()==Experiment::observing);CHECK(rig.sh.base.load()==g.base);
        CHECK(rig.sh.block_lo.load()==reg&&rig.sh.block_hi.load()==reg+(2u<<20));
        CHECK(has(rig,"event=region-scan;base=6600000000;MiB=2.00;type=mapped","full=1"));
        CHECK(has(rig,"event=located;","render=outside-block"));CHECK(rig.mem.writes.empty());
    }
}
// ------------------------------------------------------------------ differential (fake process memory)
struct DiffWorld final:BasisSource,Waiter {
    FakeMemory mem;uint64_t r1=0x5100000000ull,r2=0x5200000000ull,r3=0x5300000000ull,field=0x5100002340ull;
    uint8_t enc=enc_rad;double t=0,heading=0.3,look=0,pitch=0;uint64_t seq=0;std::mt19937 rng{7};
    std::vector<float> decoy=std::vector<float>(1000);
    DiffWorld(uint8_t e):enc(e) {
        for(uint64_t b:{r1,r2,r3})mem.blocks.push_back({b,{},1u<<20,rt_mapped});
        for(unsigned i=0;i<decoy.size();++i)decoy[i]=float(i);
        update();
    }
    static uint32_t encode(uint8_t e,double a) {
        switch(e){case enc_rad:return bits(float(a));case enc_deg:return bits(float(a*180/pi));
                  case enc_fx4096:return uint32_t(int32_t(std::lround(a/(2*pi)*4096)));default:return uint32_t(int32_t(std::lround(a/(2*pi)*65536)));}
    }
    void schedule() {
        const double c=std::fmod(t,3000.0);const bool odd=int(t/3000.0)%2;
        auto ramp=[&](double a0,double a1,double v){return c<a0?0.0:c<a1?(c-a0)/(a1-a0)*v:v;};
        double v=0;
        if(c<1900)v=ramp(500,1200,1.0);else v=1.0-ramp(1900,2600,1.0);
        if(odd){pitch=0.8*v;look=0;}else{look=2.48*v;pitch=0;}
    }
    void update() {
        schedule();
        mem.setw(field,encode(enc,look));mem.setw(field+4,encode(enc,pitch));
        mem.setw(field+8,bits(float(2*look)));                         // a mirror with the wrong ratio
        std::normal_distribution<float> nd(0.f,0.05f);
        for(unsigned i=0;i<decoy.size();++i){decoy[i]+=nd(rng);mem.setw(r2+0x100+4*i,bits(decoy[i]));}
        mem.setw(r2+0x8000,uint32_t(int32_t(t)));                       // a ms timer
    }
    void wait_ms(unsigned ms) noexcept override {t+=ms;update();}
    bool latest(Basis& b) noexcept override {
        b=Basis{};b.seq=++seq;const double y=heading+look;
        b.fwd[0]=std::cos(pitch)*std::sin(y);b.fwd[1]=std::sin(pitch);b.fwd[2]=std::cos(pitch)*std::cos(y);
        b.right[0]=std::cos(y);b.right[2]=-std::sin(y);b.up[1]=1;return true;
    }
};
static void test_diff_encodings() {
    for(uint8_t e:{uint8_t(enc_rad),uint8_t(enc_deg),uint8_t(enc_fx4096)}) {
        DiffWorld w(e);DiffScanner d;DiffDriver drv;
        std::vector<RegionInfo> rs;for(auto& b:w.mem.blocks){RegionInfo g;g.base=b.base;g.size=b.size;g.type=b.type;g.writable=true;rs.push_back(g);}
        std::vector<uint32_t> A(16384),B(16384);
        for(int i=0;i<300;++i){uint64_t a=0;uint32_t n=0;CHECK(drv.next(rs.data(),3,d,65536,a,n));CHECK(diff_window(w.mem,w,w,a,n,A.data(),B.data(),d));
            if(std::getenv("EE_DIFF_DEBUG")&&i<30)std::printf("win %d a=%llx t=%.0f look=%.3f pitch=%.3f moving=%llu turn=%.3f\n",i,(unsigned long long)a,w.t,w.look,w.pitch,(unsigned long long)d.moving,d.maxTurn);}
        DiffCandidate top[10];const unsigned k=d.top(top,10);
        CHECK(k>=2);
        CHECK(top[0].addr==w.field||top[0].addr==w.field+4);
        bool yaw=false,pit=false;
        for(unsigned i=0;i<k;++i) {
            if(top[i].addr==w.field&&top[i].axis==0&&top[i].enc==e&&top[i].sign==1)yaw=true;
            if(top[i].addr==w.field+4&&top[i].axis==1&&top[i].enc==e&&top[i].sign==1)pit=true;
            CHECK(!(top[i].addr>=w.r3&&top[i].addr<w.r3+(1u<<20)));         // zeros region
            if(i<2)CHECK(top[i].addr!=w.field+8);                            // wrong-ratio mirror never on top
            if(top[i].addr>=w.r2&&top[i].addr<w.r2+(1u<<20))CHECK(top[i].score()<top[0].score()); // decoys rank below
        }
        CHECK(yaw);CHECK(pit);CHECK(d.moving>0&&d.steady>0);
        if(!pit){FakeSink s2;log_diff(d,rs.data(),3,s2);for(auto& l:s2.lines)std::printf("%s\n",l.c_str());}
        FakeSink s;log_diff(d,rs.data(),3,s);
        CHECK(s.find("event=diff-summary;tag=periodic;windows=300;"));
        char want[96];std::snprintf(want,sizeof want,"enc=%s;axis=",encoding_name(e));CHECK(has_line(s,"event=diff-top;rank=0;",want));
        CHECK(has_line(s,"event=diff-top;rank=0;","type=mapped"));
    }
    // encoded_delta guards
    double dd;CHECK(!encoded_delta(enc_fx4096,bits(1.5f),bits(2.5f),dd)); // float bit patterns are not small integers
    CHECK(!encoded_delta(enc_rad,bits(5000.f),bits(5001.f),dd));          // not an angle
    CHECK(encoded_delta(enc_rad,bits(3.1f),bits(-3.1f),dd)&&std::fabs(dd-(2*pi-6.2))<1e-5); // wraps
}

static void sweep(FakeGame& g,unsigned tick) { // look sweep every 3 s: 0.5 s ramp to 1 rad, 0.7 s hold, back
    const unsigned c=tick%180;g.pad[1]=c<30?0.f:c<60?float((c-30)/30.0):c<102?1.f:c<132?float(1.0-(c-102)/30.0):0.f;
}
static void test_shadow_and_live_blocks() {
    // A frozen shadow copy of the E6 structure + a live one: the live block locks and
    // tracks the look sweeps; the shadow is classified static.
    Rig rig(observeOpts());
    auto& live=rig.add(0x7100000000ull);live.recomputeLook=true;
    auto& shadow=rig.add(0x7300000000ull);shadow.frozen=true;
    for(unsigned i=0;i<60*30;++i){sweep(live,i);rig.interval();}
    CHECK(rig.sh.base.load()==live.base);CHECK(rig.exp->phase()==Experiment::observing);
    CHECK(std::string(rig.exp->track_class_of(live.base))=="tracks-look");
    CHECK(std::string(rig.exp->track_class_of(shadow.base))=="static");
    CHECK(has(rig,"event=tracking-summary","tracks-look=1;tracks-aircraft-only=0;static=1"));
    CHECK(has(rig,"event=timeline;slot=","ev=look"));CHECK(rig.sink.find("event=timeline-candidate;slot=1;"));
    CHECK(has(rig,"event=tracking;","last-change-age-ms="));CHECK(rig.mem.writes.empty());
}
// Option 3 stage 2a on the REAL probe: classification (tracks-look) -> look_view -> LookWriter on the
// fake game, in the live order (probe tick, then the look tick, then the game's EE frame).
static void run_look_trial(bool persist) {
    Rig rig(observeOpts());
    auto& live=rig.add(0x7100000000ull);live.recomputeLook=true;
    LookWriter w([]{LookWriter::Options o;o.enabled=true;return o;}());
    uint64_t frame=0;LookView v0;
    rig.afterTick=[&]{
        LookIn in;in.now_ms=rig.now;in.frame=++frame;in.tid=77;rig.exp->look_view(w.locked_eff(),in.v);
        w.tick(rig.mem,rig.sink,in);
    };
    // Before the class is established nothing may be written.
    for(unsigned i=0;i<60*8;++i){sweep(live,i);rig.interval();}
    CHECK(w.phase()==LookWriter::p_waiting||w.phase()==LookWriter::p_settling);CHECK(rig.mem.writes.empty());
    for(unsigned i=60*8;i<60*30;++i){sweep(live,i);rig.interval();}
    CHECK(std::string(rig.exp->track_class_of(live.base))=="tracks-look");
    rig.exp->look_view(0,v0);
    CHECK(v0.have&&v0.eff==live.base&&v0.lo==live.base&&v0.hi==live.base+block_bytes&&v0.cockpit&&v0.samples>=look_min_samples&&v0.look_ev>=look_min_events);
    // Sweeps (user look input) happened: the writer must not have started while the field was non-neutral.
    const size_t before=rig.mem.writes.size();
    live.pad[1]=0;live.recomputeLook=!persist;         // persist: the game keeps what is written; else it recomputes from the pad
    for(unsigned i=0;i<20;++i)rig.interval();          // neutral again
    for(unsigned i=0;i<60*12&&w.phase()!=LookWriter::p_done&&w.phase()!=LookWriter::p_aborted;++i)rig.interval();
    CHECK(w.phase()==LookWriter::p_done);CHECK(w.result().writes==look_trial_frames);CHECK(w.result().restored);
    CHECK(rig.mem.writes.size()-before==look_trial_frames+1u);
    for(size_t i=before;i<rig.mem.writes.size();++i)CHECK(rig.mem.writes[i].a==live.base+field_offset[look_yaw]);
    CHECK(rig.sink.find("event=look-write-begin;"));CHECK(rig.sink.count("event=look-write;n=")==look_trial_frames);
    CHECK(rig.exp->phase()!=Experiment::running);   // the old write-trial skeleton stays inert
    if(persist) {
        CHECK(w.succeeded());CHECK(w.result().lag_ticks>=1&&w.result().lag_ticks<=2);CHECK(w.result().follow>0.9);
        CHECK(std::fabs(w.result().comp_gain-1.0)<0.05);CHECK(w.result().srt_valid);CHECK(std::fabs(std::fabs(w.result().srt_gain)-1.0)<0.1);
    } else {
        CHECK(!w.succeeded());CHECK(w.result().overwritten>look_trial_frames/2);CHECK(!w.result().responded);
    }
    CHECK(live.at(look_yaw)==0.f);
}
static void test_look_writer_on_probe() {run_look_trial(true);run_look_trial(false);}
static void test_observe_follows_best_candidate() {
    // Run 3: a zero 64 MiB block first, the E6 structure (static) in a second block:
    // the observation must follow the best-scoring candidate, not the zero block.
    Rig rig(observeOpts());
    FakeMemory::Block zero{0x7000000000ull,{}};rig.mem.blocks.push_back(zero);rig.scan.block(0x7000000000ull);
    auto& s=rig.add(0x7300000000ull);s.frozen=true;
    rig.runFor(60*25);
    CHECK(rig.exp->phase()==Experiment::searching);CHECK(rig.sh.base.load()==0);
    CHECK(rig.sink.find("event=observe-base;base=7000000000;block=7000000000;kind=e6-unvalidated"));
    CHECK(rig.sink.find("event=observe-follow;base=7300000000"));
    CHECK(rig.sink.find("event=observe-base;base=7300000000;block=7300000000;kind=best-candidate"));
    CHECK(rig.exp->observe_base()==s.base);
    CHECK(std::string(rig.exp->track_class_of(s.base))=="static");
    CHECK(std::string(rig.exp->track_class_of(0x7000000000ull))=="static");
    CHECK(has(rig,"event=search;","static-not-tracking"));CHECK(rig.mem.writes.empty());
}
static void test_tracks_aircraft_only() {
    Rig rig(observeOpts());auto& g=rig.add(0x7100000000ull);g.turn=0.005;g.rotateCockpit=true;
    rig.runFor(60*25);
    CHECK(std::string(rig.exp->track_class_of(g.base))=="tracks-aircraft-only");
    CHECK(has(rig,"event=timeline;slot=","ev=aircraft"));
}
static void test_log_file_shared_and_bounded() {
    wchar_t dir[MAX_PATH];GetTempPathW(MAX_PATH,dir);
    wchar_t path[MAX_PATH],prev[MAX_PATH];
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%see-camera-test-%lu.log",dir,GetCurrentProcessId());
    _snwprintf_s(prev,MAX_PATH,_TRUNCATE,L"%see-camera-test-%lu.prev.log",dir,GetCurrentProcessId());
    {EeLogFile a;CHECK(a.open(path,prev));a.line("first-run",1);CHECK(a.final_flush());a.close();}
    EeLogFile f(4096);CHECK(f.open(path,prev));
    f.line("ee-camera;event=one",1000);f.line("ee-camera;event=two",1001);
    // an external reader can open (and copy) the file while it is being written
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(h!=INVALID_HANDLE_VALUE);
    CHECK(f.try_line("ee-camera;event=final-report;trigger=test",1002));CHECK(f.final_flush());
    char buf[8192]{};DWORD got=0;
    if(h!=INVALID_HANDLE_VALUE){ReadFile(h,buf,sizeof buf-1,&got,nullptr);CloseHandle(h);}
    const std::string txt(buf,got);
    CHECK(txt.find("1000.000|ee-camera;event=one")!=std::string::npos);CHECK(txt.find("trigger=test")!=std::string::npos);
    CHECK(txt.find("first-run")==std::string::npos); // the previous run moved to .prev
    for(int i=0;i<200;++i)f.line("ee-camera;event=filler;xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",2000+i);
    CHECK(f.capped());CHECK(f.written()<=4096);
    HANDLE p=CreateFileW(prev,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(p!=INVALID_HANDLE_VALUE);if(p!=INVALID_HANDLE_VALUE)CloseHandle(p);
    f.close();DeleteFileW(path);DeleteFileW(prev);
}
static void test_diff_reporting_and_order() {
    // single-hit candidates are reported (run 3 printed none), with min/max and the tag
    DiffScanner d;uint32_t A[4]={bits(0.f),0,0,0},B[4]={bits(0.5f),0,0,0};
    d.analyse(0x5100000000ull,A,B,4,0.5,0);
    RegionInfo g;g.base=0x5100000000ull;g.size=1u<<20;g.type=rt_mapped;g.writable=true;
    FakeSink s;log_diff(d,&g,1,s,"final");
    CHECK(s.find("event=diff-summary;tag=final;windows=1;moving=1"));
    CHECK(has_line(s,"event=diff-top;rank=0;addr=5100000000;region=5100000000+0;type=mapped;enc=rad;axis=yaw;sign=+1;hits=1;misses=0","min=0;max=0.5;tag=final"));
    // window order: 64 MiB and 32 MiB regions first, then regions near them, then the rest
    RegionInfo r[5];
    const uint64_t bases[5]={0x1000000000ull,0x2000000000ull,0x3000000000ull,0x3004000000ull,0x4000000000ull};
    const uint64_t sizes[5]={1u<<20,block_bytes/2,block_bytes,1u<<20,block_bytes};
    for(int i=0;i<5;++i){r[i].base=bases[i];r[i].size=sizes[i];r[i].writable=i!=4;}
    DiffDriver drv;DiffScanner empty;uint64_t a=0;uint32_t n=0;std::vector<uint64_t> seq;
    for(int i=0;i<16;++i){CHECK(drv.next(r,5,empty,8u<<20,a,n));if(seq.empty()||seq.back()!=a)seq.push_back(a);}
    CHECK(seq.size()>=7);
    CHECK(seq[0]==0x2000000000ull);CHECK(seq[4]==0x3000000000ull); // 32 MiB = 4 windows, then the 64 MiB block
    bool sawNear=false,farBeforeNear=false;
    for(uint64_t x:seq){if(x==0x3004000000ull)sawNear=true;if(x==0x1000000000ull&&!sawNear)farBeforeNear=true;}
    CHECK(sawNear);CHECK(!farBeforeNear);
    for(uint64_t x:seq)CHECK(x<0x4000000000ull); // read-only (non-writable) region never diffed
}

static bool await_phase(const std::atomic<unsigned>& phase,unsigned wanted) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(phase.load(std::memory_order_acquire)<wanted) {
        if(std::chrono::steady_clock::now()>=deadline)return false;
        std::this_thread::yield();
    }
    return true;
}

static void check_report_generation(const FakeSink& sink,unsigned generation) {
    const unsigned n=1+generation%2;
    const std::string header=";regions="+std::to_string(n)+";wide-passes="+std::to_string(generation);
    CHECK(sink.find(header.c_str()));
    const std::string census=";seen="+std::to_string(100+generation)+";kept="+std::to_string(n)+";";
    CHECK(sink.find(census.c_str()));
    const std::string summary="event=diff-summary;tag=final;windows="+std::to_string(generation)+";";
    CHECK(sink.find(summary.c_str()));
    if(generation) {
        const std::string hits=";hits="+std::to_string(generation)+";misses=0;";
        CHECK(has_line(sink,"event=diff-top;rank=0;addr=5100000000;region=5100000000+0;type=mapped",hits.c_str()));
    } else CHECK(sink.count("event=diff-top")==0);
}

static void test_final_report_interleaving() {
    FinalReportBox box;FakeSink empty;
    CHECK(!box.try_report("before-worker",empty));CHECK(empty.lines.empty());
    std::atomic<unsigned> phase{0},ack{0};std::atomic<bool> worker_ok{true};
    // Only the worker touches these mutable inputs. Each round stops halfway
    // through enumeration, then updates real DiffScanner candidates and publishes.
    std::thread worker([&] {
        RegionInfo regions[2]{};RegionCensus census{};DiffScanner diff;
        regions[0].base=0x5100000000ull;regions[0].size=region_min;
        regions[0].type=rt_mapped;regions[0].writable=true;
        regions[1]=regions[0];regions[1].base+=region_min;
        census.seen=100;census.kept=1;census.bytes=region_min;
        if(!box.publish(regions,1,census,0,diff))worker_ok=false;
        for(unsigned gen=1;gen<=32;++gen) {
            const unsigned n=1+gen%2;
            census.seen=100+gen; // deliberately inconsistent half-enumerated state
            regions[0].type=rt_private;
            phase.store(2*gen-1,std::memory_order_release);
            if(!await_phase(ack,2*gen-1)){worker_ok=false;return;}
            census.kept=n;census.bytes=n*region_min;regions[0].type=rt_mapped;
            const uint32_t A=bits(0.f),B=bits(0.5f);
            diff.analyse(regions[0].base,&A,&B,1,0.5,0);
            if(!box.publish(regions,n,census,gen,diff))worker_ok=false;
            phase.store(2*gen,std::memory_order_release);
            if(!await_phase(ack,2*gen)){worker_ok=false;return;}
        }
    });
    for(unsigned gen=1;gen<=32;++gen) {
        const bool reached=await_phase(phase,2*gen-1);CHECK(reached);
        if(!reached){ack.store(64,std::memory_order_release);break;}
        // The header belongs to the previous snapshot. Allow the worker to
        // finish enumeration, mutate candidates and publish a replacement BEFORE
        // consuming its remaining lines: every line must still be the old one.
        struct InterleavedSink final:Sink {
            FakeSink captured;std::atomic<unsigned>& phase;std::atomic<unsigned>& ack;
            unsigned generation;bool waited=false;
            InterleavedSink(std::atomic<unsigned>& p,std::atomic<unsigned>& a,unsigned g):phase(p),ack(a),generation(g) {}
            void line(bool imp,const char* text) noexcept override {
                captured.line(imp,text);
                if(captured.lines.size()==1) {
                    ack.store(2*generation-1,std::memory_order_release);
                    waited=await_phase(phase,2*generation);
                }
            }
        } during(phase,ack,gen);
        const bool reported=box.try_report("factory-final-release",during);CHECK(reported);
        if(!reported||!during.waited) {
            CHECK(during.waited);ack.store(64,std::memory_order_release);break;
        }
        CHECK(during.waited);
        check_report_generation(during.captured,gen-1);
        FakeSink after;CHECK(box.try_report("process-exit",after));check_report_generation(after,gen);
        ack.store(2*gen,std::memory_order_release);
    }
    worker.join();CHECK(worker_ok.load());
}

// ------------------------------------------------------------------ J-look-locator (ee-look-write-run1, run B)
// The analysis (J-look-locator-analysis.md): the block 1f8a72e0000+FDD410 (score 5) was listed by 17 of the 18 scans
// of run B; scan 4 ran across the RIGHT hold, listed three score-4 junk structures only, set_tracks replaced slot 0
// and reset the look counters (look 0/4 afterwards). Fixes under test: (A) sticky identity, (B) per-tick structural
// re-check with a moving-look allowance + double read, (C) writer gate (see ee_look_write_offline_tests.cpp).
struct JunkStructure {uint32_t off;float L,H,air[3],cam[3],rot[3];};
// The constant score-4 structures of run B (ee-camera.log scan-best lines), offsets inside the 64 MiB region.
static const JunkStructure k_junk[3]={
    {0xDAFCA8,1.f,7.0955e-39f,{46.4492f,-1.25391f,1.25391f},{1.00195f,1.f,-0.848694f},{-0.52888f,1.f,0.84721f}},
    {0xF02C60,0.f,1.f,{6.60416e-07f,-1.93531f,-42.544f},{-0.0869153f,1.f,-0.604411f},{0.001291f,1.f,-0.63293f}},
    {0xD27A2C,0.00018135f,-3.5692e-33f,{46.357f,1.18885e-08f,0.000790883f},{2.84169e-35f,2.78451f,6.00827e-16f},{4.5989e-31f,5.1785e-28f,5.2423e-16f}}};
static void plant_junk(FakeMemory& mem,uint64_t lo,const JunkStructure& j) {
    auto put=[&](unsigned f,float v){mem.set(lo,j.off+field_offset[f]-field_offset[look_yaw],v);};
    for(unsigned f=0;f<field_count;++f)put(f,0.f);
    put(look_yaw,j.L);put(heading,j.H);put(air_x,j.air[0]);put(air_y,j.air[1]);put(air_z,j.air[2]);
    put(cam_x,j.cam[0]);put(cam_y,j.cam[1]);put(cam_z,j.cam[2]);put(cam_pitch,j.rot[0]);put(cam_yaw,j.rot[1]);put(cam_roll,j.rot[2]);
}
static constexpr uint64_t k_realLo=0x1f8a72e0000ull;
static constexpr uint64_t k_junkEff0=k_realLo-0x22D768;      // eff of the DAFCA8 junk: lo + 0xDAFCA8 - 0xFDD410
// Replays the scan results of run B: ranks 0..2 as logged (FDD410 s5, DAFCA8 s4, F02C60 s4) except scan `missScan`
// (the miss: DAFCA8, F02C60, D27A2C, all s4). `queue` overrides the next requests. Locator candidates stay empty.
struct ScriptedRequester final:ScanRequester {
    unsigned requests=0,missScan=4,slowScan=0,slowTicks=0,wait=0;bool ready=false;ScanResult r{};   // slowScan: that request's result arrives slowTicks polls late (run B: 3.7 s scans)
    std::vector<std::array<ScanHit,3>> queue;size_t qi=0;
    static ScanHit hit(uint64_t lo,uint32_t off,int score) {ScanHit h;h.offset=lo+off;h.lo=lo;h.hi=lo+block_bytes;h.score=score;return h;}
    bool request(uint64_t,const Basis*,bool) noexcept override {
        if(ready)return false;
        ++requests;r=ScanResult{};r.regions=373;r.complete=true;
        if(qi<queue.size()){for(int k=0;k<3;++k)r.best[k]=queue[qi][k];++qi;}
        else if(requests==missScan){r.best[0]=hit(k_realLo,0xDAFCA8,4);r.best[1]=hit(k_realLo,0xF02C60,4);r.best[2]=hit(k_realLo,0xD27A2C,4);}
        else {r.best[0]=hit(k_realLo,0xFDD410,5);r.best[1]=hit(k_realLo,0xDAFCA8,4);r.best[2]=hit(k_realLo,0xF02C60,4);}
        ready=true;wait=requests==slowScan?slowTicks:0;return true;
    }
    const ScanResult* poll() noexcept override {if(!ready)return nullptr;if(wait){--wait;return nullptr;}ready=false;return &r;}
};
// One look hold: linear ramp up over 0.4 s, hold, linear ramp back over 0.4 s (the game's smoothing is exponential;
// linear is the harder case for the strict yaw relation).
static float hold_value(double t,double t0,double hold,float peak) {
    if(t<t0||t>t0+hold+0.8)return 0.f;
    if(t<t0+0.4)return float(peak*(t-t0)/0.4);
    if(t<t0+0.4+hold)return peak;
    return float(peak*(1.0-(t-t0-0.4-hold)/0.4));
}
// yaw right (5.0 s, across scan 4 which starts at 6.0 s), yaw left, pitch up, pitch down; 1.0 s holds, gaps shorter than
// the 2 s quiet gate; the sweep ends at 14.6 s
static constexpr double k_sweepEnd=14.6;
static void run_b_input(FakeGame& g,double t) {
    g.pad[1]=hold_value(t,5.0,1.0,2.4f)+hold_value(t,7.6,1.0,-2.4f);
    g.pad[0]=hold_value(t,10.2,1.0,0.8f)+hold_value(t,12.8,1.0,-0.5f);
}
static bool track_counts(const FakeSink& s,unsigned slot,unsigned& passed,unsigned& total) {
    char key[48];std::snprintf(key,sizeof key,"event=tracking;slot=%u;",slot);
    const std::string* last=nullptr;for(auto& l:s.lines)if(l.find(key)!=std::string::npos)last=&l;
    if(!last)return false;
    const size_t p=last->find("struct-ok=");
    return p!=std::string::npos&&std::sscanf(last->c_str()+p,"struct-ok=%u/%u",&passed,&total)==2;
}

static void test_look_locator_relations() {
    // The three residuals of the analysis (log timeline rows of slot 0): strict 2e-3 fails, the moving-look allowance passes.
    struct Case {float L,H,C;} cases[]={{2.45737f,-0.785398f,1.67591f},{-2.37035f,-0.785398f,3.11045f},{-0.255175f,-0.785398f,-1.0023f}};
    for(const Case& c:cases) {
        float v[field_count]{};
        v[look_yaw]=c.L;v[heading]=c.H;v[cam_yaw]=c.C;v[air_x]=-14084.4f;v[air_y]=4000.f;v[air_z]=121746.f;
        v[cam_x]=-14058.7f;v[cam_y]=4003.57f;v[cam_z]=121720.f;
        RelFacts f;
        const uint32_t strict=relations(v,nullptr,0,0,&f);
        CHECK(!(strict&rel_yaw));CHECK(std::fabs(f.yaw_res)>2e-3&&std::fabs(f.yaw_res)<0.05);
        const uint32_t loose=relations(v,nullptr,0.05,0,nullptr);
        CHECK((loose&rel_yaw)&&(loose&rel_vert)&&(loose&rel_horiz)&&(loose&rel_sane));
        CHECK(!(relations(v,nullptr,1e-4,0,nullptr)&rel_yaw));   // a tiny allowance does not pass them
    }
    // junk constants (score 4, eye-err 585): never rel_eye against the real block's recovered eye
    Basis b{};b.seq=1;b.eye[0]=26.0f*16;b.eye[1]=3.575f*16;b.eye[2]=-26.0f*16;
    for(const JunkStructure& j:k_junk) {
        float v[field_count]{};
        v[look_yaw]=j.L;v[heading]=j.H;v[air_x]=j.air[0];v[air_y]=j.air[1];v[air_z]=j.air[2];v[cam_x]=j.cam[0];v[cam_y]=j.cam[1];v[cam_z]=j.cam[2];
        v[cam_pitch]=j.rot[0];v[cam_yaw]=j.rot[1];v[cam_roll]=j.rot[2];
        const uint32_t m=relations(v,&b,0,0,nullptr);
        CHECK((m&rel_sane)&&(m&rel_yaw)&&(m&rel_vert)&&(m&rel_horiz)&&!(m&rel_eye));
    }
}

// Replay of run B's scan lists against the fake real block (lag-composed camera so the strict yaw relation fails
// in every ramp) and the sweep. Expected after the fix: slot 0 keeps 1f8a72e0000, look counters never reset,
// class tracks-look with every look event hit (run B after the miss: look 0/4 on slot 0).
static void run_replay(bool withWriter) {
    Rig rig(observeOpts());
    ScriptedRequester script;script.slowScan=4;script.slowTicks=210;rig.reqp=&script;   // scan 4 (the miss) takes 3.5 s, as in run B
    auto& live=rig.add(k_realLo,false);live.recomputeLook=true;live.lagCompose=true;live.eyeRel16=true;
    for(const JunkStructure& j:k_junk)plant_junk(rig.mem,k_realLo,j);
    const uint64_t zeroBlock=0x1f873a00000ull;rig.mem.blocks.push_back({zeroBlock,{}});rig.scan.block(zeroBlock);   // the E6 start block of run B (all zero)
    LookWriter w([&]{LookWriter::Options o;o.enabled=withWriter;return o;}());
    uint64_t frame=0;size_t writesInSweep=0;
    if(withWriter)rig.afterTick=[&]{LookIn in;in.now_ms=rig.now;in.frame=++frame;in.tid=77;rig.exp->look_view(w.locked_eff(),in.v);w.tick(rig.mem,rig.sink,in);};
    const double sweepEnd=k_sweepEnd;
    for(unsigned i=0;i<60*36;++i) {
        const double t=rig.now/1000.0;
        run_b_input(live,t);
        if(t>sweepEnd+3)live.recomputeLook=false;       // afterwards the game keeps what is written (as in run_look_trial)
        rig.interval();
        if(t<=sweepEnd+2)writesInSweep=rig.mem.writes.size();
    }
    CHECK(script.requests>=10);
    // (A) the miss happened and was survived: sticky, no eviction, no follow-switch, slot 0 never re-assigned
    CHECK(rig.sink.find("event=track-sticky;slot=0;base=1f8a72e0000;class="));
    CHECK(rig.sink.count("event=track-evict")==0);
    CHECK(rig.sink.count("event=observe-follow;")==1);CHECK(rig.sink.find("event=observe-follow;base=1f8a72e0000;"));
    CHECK(rig.sink.find("event=observe-follow-kept;base=1f8a72e0000;score=5;ignored="));
    CHECK(rig.sink.count("event=timeline-candidate;slot=0;")==1);      // run B: slot 0 re-assigned to base=1f8a702a61c
    CHECK(rig.sink.find("event=timeline-candidate;slot=0;base=1f8a72e0000;"));
    LookView v;rig.exp->look_view(0,v);
    CHECK(std::string(rig.exp->track_class_of(k_realLo))=="tracks-look");
    CHECK(v.have&&v.eff==k_realLo);
    CHECK(v.look_ev>=12);CHECK(v.look_hit==v.look_ev);                 // the counters survive: n/n (run B after the miss: 0/4)
    CHECK(v.struct_age_ms<1500);
    // (B) the structural re-check passed on (almost) every 250 ms tick, during the ramps too
    unsigned a=0,b=0;CHECK(track_counts(rig.sink,0,a,b));CHECK(b>=100);CHECK(a*100>=b*99);
    CHECK(std::string(rig.exp->track_class_of(k_junkEff0))!="tracks-look");
    if(withWriter) {
        // (C) nothing written during the sweep or the 2 s after it; the trial then runs on the SAME identity.
        CHECK(writesInSweep==0);
        CHECK(rig.sink.count("reason=identity-changed")==0);
        CHECK(rig.sink.count("event=look-write-settle;")==1);
        CHECK(w.attempted());CHECK(w.phase()==LookWriter::p_done);CHECK(w.result().writes==look_trial_frames);CHECK(w.result().restored);
        for(size_t i=0;i<rig.mem.writes.size();++i)CHECK(rig.mem.writes[i].a==k_realLo+field_offset[look_yaw]);
        CHECK(rig.mem.writes.size()==look_trial_frames+1u);
    } else CHECK(rig.mem.writes.empty());
}
static void test_look_locator_replay_run_b() {run_replay(false);run_replay(true);}

// The same with real wide scans over a lag-composed block with the junk planted. (The Locator locks this block after
// the first scan, which ends the scanning: scan misses are covered by the replay; this covers B and the look_view
// fields against the real classification.)
static void test_look_locator_natural_scans() {
    Rig rig(observeOpts());
    auto& live=rig.add(k_realLo,false);live.recomputeLook=true;live.lagCompose=true;live.eyeRel16=true;
    for(const JunkStructure& j:k_junk)plant_junk(rig.mem,k_realLo,j);
    LookView mid;bool midSeen=false,quietSeen=false,strictFailed=false;
    for(unsigned i=0;i<60*34;++i) {
        const double t=rig.now/1000.0;
        live.pad[1]=hold_value(t,3.9,1.4,2.4f)+hold_value(t,9.9,1.4,-2.4f);live.pad[0]=hold_value(t,15.9,1.4,0.8f)+hold_value(t,21.9,1.4,-0.5f);
        rig.interval();
        if(!strictFailed&&t>4.1) {    // inside the first ramp: the strict 2e-3 yaw relation fails, i.e. a scan read here would miss the block
            Snapshot sn;CHECK(read_snapshot(rig.mem,k_realLo,sn));
            const uint32_t strict=relations(sn.v,nullptr,0,0,nullptr);CHECK(!(strict&rel_yaw));CHECK(relations(sn.v,nullptr,0.5,0,nullptr)&rel_yaw);
            strictFailed=true;
        }
        if(!midSeen&&t>4.5&&t<4.7){rig.exp->look_view(0,mid);midSeen=true;}
        if(!quietSeen&&t>28.5){LookView q;rig.exp->look_view(0,q);quietSeen=true;CHECK(q.quiet_ms>=2000);CHECK(q.struct_age_ms<1500);}
    }
    CHECK(midSeen&&mid.quiet_ms<1000);
    CHECK(rig.sink.count("event=track-evict")==0);
    CHECK(rig.sink.count("event=timeline-candidate;slot=0;")==1);CHECK(rig.sink.find("event=timeline-candidate;slot=0;base=1f8a72e0000;"));
    CHECK(std::string(rig.exp->track_class_of(k_realLo))=="tracks-look");
    LookView v;rig.exp->look_view(0,v);CHECK(v.have&&v.eff==k_realLo&&v.look_ev>=12&&v.look_hit==v.look_ev);
    unsigned a=0,b=0;CHECK(track_counts(rig.sink,0,a,b));CHECK(b>=100);CHECK(a*100>=b*99);
    CHECK(rig.mem.writes.empty());
}

// Eviction still happens when the structure is really gone: >= track_missed_scans scans without it AND no passing re-check.
static void test_look_locator_eviction_when_gone() {
    Rig rig(observeOpts());
    ScriptedRequester script;script.missScan=0;rig.reqp=&script;
    auto& live=rig.add(k_realLo,false);live.recomputeLook=true;live.eyeRel16=true;
    for(const JunkStructure& j:k_junk)plant_junk(rig.mem,k_realLo,j);
    for(unsigned i=0;i<60*26;++i){run_b_input(live,rig.now/1000.0);rig.interval();}
    CHECK(std::string(rig.exp->track_class_of(k_realLo))=="tracks-look");
    CHECK(rig.sink.count("event=track-evict")==0);
    // The structure vanishes (region words zeroed) and no scan lists it any more. First three new score-5 entries fill the
    // free slots, then one new entry per scan: the two score-4 junk slots go first, the vanished block only after
    // track_missed_scans misses with a stale re-check.
    live.frozen=true;                                    // the fake game stops rewriting the structure
    FakeMemory::Block* blk=rig.mem.find(k_realLo,4);CHECK(blk!=nullptr);
    if(blk)for(auto& e:blk->w)if(e.first>=0xFDD000&&e.first<0xFDE000)e.second=0;
    auto entry=[](unsigned i){return ScriptedRequester::hit(0x1f8b00000000ull+uint64_t(i)*0x10000000ull,0xFDD410,5);};
    script.queue.push_back({entry(1),entry(2),entry(3)});
    for(unsigned i=4;i<=8;++i)script.queue.push_back({entry(i),ScanHit{},ScanHit{}});
    script.qi=0;
    for(unsigned i=0;i<60*20&&script.qi<script.queue.size();++i)rig.interval();
    rig.runFor(60*2);
    const std::string* ev=rig.sink.find("event=track-evict;slot=0;base=1f8a72e0000;");
    CHECK(ev!=nullptr);
    if(ev) {unsigned missed=0;const size_t p=ev->find("missed-scans=");CHECK(p!=std::string::npos);
        if(p!=std::string::npos&&std::sscanf(ev->c_str()+p,"missed-scans=%u",&missed)==1)CHECK(missed>=track_missed_scans);
        double age=0;const size_t q=ev->find("struct-age-ms=");
        if(q!=std::string::npos&&std::sscanf(ev->c_str()+q,"struct-age-ms=%lf",&age)==1)CHECK(age>=track_struct_stale_ms);}
    CHECK(rig.sink.count("event=track-evict;slot=0;")==1);
}

// A lower-scored scan entry never displaces a higher-scored held one, even when the held structure is not tracks-look.
static void test_look_locator_no_downgrade() {
    Rig rig(observeOpts());
    ScriptedRequester script;script.missScan=2;rig.reqp=&script;
    auto& live=rig.add(k_realLo,false);live.frozen=true;live.eyeRel16=true;      // static structure: never tracks-look, but scored 5 by the scan
    for(const JunkStructure& j:k_junk)plant_junk(rig.mem,k_realLo,j);
    rig.runFor(60*8);
    CHECK(rig.sink.find("event=observe-follow;base=1f8a72e0000;"));
    CHECK(rig.sink.find("event=observe-follow-kept;base=1f8a72e0000;score=5;ignored="));
    CHECK(rig.sink.count("event=observe-follow;")==1);
    CHECK(rig.sink.count("event=track-evict")==0);
    CHECK(std::string(rig.exp->track_class_of(k_realLo))!="tracks-look");
    LookView v;rig.exp->look_view(0,v);CHECK(!v.have);
}

// review 2026-11-01 F7: the cached option verdicts of ee_camera_probe_win.cpp (not linked here: Windows hook glue) are read by the worker and the Present
// tick, so a plain `static int` cache is a data race. Source check (failing before: four `static int s_on=-1;` sites): every such cache is std::atomic.
void test_probe_win_cached_flags_atomic() {
#ifdef ACZ_VR_SRC_DIR
    FILE* f=nullptr;
    const std::string path=std::string(ACZ_VR_SRC_DIR)+"/ee_camera_probe_win.cpp";
    if(fopen_s(&f,path.c_str(),"rb")!=0||!f){CHECK(!"ee_camera_probe_win.cpp readable");return;}
    std::string text;char buf[4096];size_t n;
    while((n=std::fread(buf,1,sizeof buf,f))>0)text.append(buf,n);
    std::fclose(f);
    CHECK(text.size()>10000);
    CHECK(text.find("static int s_on")==std::string::npos);
    size_t atomics=0;for(size_t at=text.find("static std::atomic<int> s_on{-1};");at!=std::string::npos;at=text.find("static std::atomic<int> s_on{-1};",at+1))++atomics;
    CHECK(atomics==4);
#endif
}

int main() {
    test_probe_win_cached_flags_atomic();
    test_collector();test_validate();test_locator();test_seam_bounds();test_classify();test_guarded();test_basis();test_config();
    test_persistent_input();test_recompute_each_frame();test_compose_then_reset();test_render_consumer_and_not_input();
    test_flicker();test_position();test_aborts();test_run_cap();
    test_observe_only_default();test_write_gates();test_shifted_layout();test_eye_offset_variants();test_degrees_flagged();
    test_near_miss_and_counts();test_eye_proportional_route();
    test_region_table();test_wide_pass_budget_and_mapped_region();test_diff_encodings();
    test_shadow_and_live_blocks();test_look_writer_on_probe();test_observe_follows_best_candidate();test_tracks_aircraft_only();test_log_file_shared_and_bounded();
    test_diff_reporting_and_order();
    test_final_report_interleaving();
    test_look_locator_relations();test_look_locator_replay_run_b();test_look_locator_natural_scans();
    test_look_locator_eviction_when_gone();test_look_locator_no_downgrade();
    std::printf("ee_camera_probe_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
