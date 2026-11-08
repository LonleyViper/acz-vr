// Option 3 stage 2a/2b offline fixtures: fake memory and a fake game with switchable look models.
// Nothing here touches a process, the game, the GPU or XR. The Experiment-level integration test
// (real probe classification -> look_view -> LookWriter) is in ee_camera_probe_offline_tests.cpp.
#include "ee_look_write.h"
#include "runtime_config.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace acz::ee_camera;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)

// ------------------------------------------------------------------ fakes
struct FakeMem final:Memory {
    uint64_t base=0x7000000000ull,size=block_bytes;
    std::unordered_map<uint64_t,uint32_t> w;
    bool failReads=false,failWrites=false;
    struct W {uint64_t a;float v;};std::vector<W> writes;
    bool inside(uint64_t a,uint32_t n) const {return a>=base&&a+n<=base+size;}
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        if(failReads||!inside(a,n))return false;
        for(uint32_t i=0;i<n;i+=4){auto it=w.find(a+i);const uint32_t v=it==w.end()?0:it->second;std::memcpy(static_cast<char*>(out)+i,&v,4);}
        return true;
    }
    bool write_float(uint64_t a,float v) noexcept override {
        if(failWrites||!inside(a,4)||(a&3))return false;
        w[a]=bits(v);writes.push_back({a,v});return true;
    }
    void set(uint64_t a,float v){w[a]=bits(v);}
    float get(uint64_t a) const {auto it=w.find(a);return it==w.end()?0.f:from_bits(it->second);}
};
struct FakeSink final:Sink {
    std::vector<std::string> lines;
    void line(bool,const char* t) noexcept override {lines.emplace_back(t);if(std::getenv("EE_LOOK_VERBOSE"))std::printf("  %s\n",t);}
    const std::string* find(const char* s) const {for(auto& l:lines)if(l.find(s)!=std::string::npos)return &l;return nullptr;}
    unsigned count(const char* s) const {unsigned n=0;for(auto& l:lines)n+=l.find(s)!=std::string::npos;return n;}
};

enum Model {m_persist,m_overwrite,m_filter};
struct Game {
    FakeMem& m;uint64_t eff;Model model=m_persist;double hdg=-0.7854,ax=-29925,ay=4000,az=137590;bool moving=true;float pad=0;double srtGain=-0.44;
    float lastSrt=0;
    Game(FakeMem& mm,uint64_t e):m(mm),eff(e) {
        for(unsigned f:{look_pitch,look_yaw,look_roll,cam_pitch,cam_roll,look_yaw_copy})m.set(eff+field_offset[f],0);
        step();
    }
    uint64_t a(unsigned f) const {return eff+field_offset[f];}
    void step() {
        if(moving){ax+=1.5;az-=1.2;}
        float L=m.get(a(look_yaw));
        if(model==m_overwrite)L=pad;
        else if(model==m_filter)L=L+(pad-L)*0.3f;
        m.set(a(look_yaw),L);
        m.set(a(heading),float(hdg));m.set(a(air_x),float(ax));m.set(a(air_y),float(ay));m.set(a(air_z),float(az));
        m.set(a(cam_yaw),float(wrap(hdg+L)));m.set(a(look_yaw_copy),L);m.set(a(cam_pitch),m.get(a(look_pitch)));
        lastSrt=float(wrap(hdg+L)*srtGain);   // a deliberately different (and negative) gain on the recovered camera
    }
};
struct Harness {
    FakeMem mem;Game game{mem,0x7000000000ull};FakeSink sink;LookWriter w;
    double now=0;uint64_t frame=1;bool stop=false,cockpit=true,lockedPresent=true,have=true;unsigned samples=50,ev=10,hit=10;
    double quietMs=1e9,structAge=0;   // J-look-locator inputs: no look event ever, structure confirmed this tick
    explicit Harness(LookWriter::Options o):w(o) {}
    uint64_t yawAddr() const {return game.a(look_yaw);}
    LookIn input() const {
        LookIn in;in.now_ms=now;in.frame=frame;in.tid=4242;in.stop_file=stop;
        in.v.have=have;in.v.eff=game.eff;in.v.lo=mem.base;in.v.hi=mem.base+mem.size;in.v.samples=samples;in.v.look_ev=ev;in.v.look_hit=hit;
        in.v.locked_present=lockedPresent;in.v.locked_lo=mem.base;in.v.locked_hi=mem.base+mem.size;in.v.cockpit=cockpit;
        in.v.quiet_ms=quietMs;in.v.struct_age_ms=structAge;
        in.v.basis_ok=true;in.v.basis_seq=frame;in.v.srt_yaw=game.lastSrt;in.v.srt_air_yaw=float(game.hdg);
        return in;
    }
    void tick(){w.tick(mem,sink,input());game.step();now+=1000.0/60;++frame;}
    void run(unsigned n){for(unsigned i=0;i<n&&w.phase()!=LookWriter::p_done&&w.phase()!=LookWriter::p_aborted;++i)tick();}
    bool onlyYawWrites() const {for(auto& x:mem.writes)if(x.a!=yawAddr())return false;return true;}
    unsigned writesOf(float v) const {unsigned n=0;for(auto& x:mem.writes)n+=bits(x.v)==bits(v);return n;}
};
static LookWriter::Options enabled(){LookWriter::Options o;o.enabled=true;return o;}

// ------------------------------------------------------------------ tests
static void test_default_off_and_options() {
    Harness h(LookWriter::Options{});
    CHECK(h.w.phase()==LookWriter::p_off);
    for(int i=0;i<2000;++i)h.tick();
    CHECK(h.mem.writes.empty());CHECK(h.sink.lines.empty());CHECK(!h.w.attempted());CHECK(!h.w.succeeded());
    // Bounds on the configured value / frames.
    for(float y:{5.f,-3.f,1.0f,std::nanf(""),0.f}) {
        LookWriter::Options o=enabled();o.yaw=y;Harness g(o);g.run(2000);
        for(auto& x:g.mem.writes)CHECK(std::fabs(x.v)<=look_write_clamp);
    }
    {LookWriter::Options o=enabled();o.yaw=5.f;Harness g(o);g.run(2000);CHECK(g.writesOf(1.0f)==look_trial_frames);}
    {LookWriter::Options o=enabled();o.yaw=-3.f;Harness g(o);g.run(2000);CHECK(g.writesOf(-1.0f)==look_trial_frames);}
    {LookWriter::Options o=enabled();o.yaw=std::nanf("");Harness g(o);g.run(2000);CHECK(g.writesOf(look_write_default)==look_trial_frames);}
    {LookWriter::Options o=enabled();o.frames=100000;Harness g(o);g.run(5000);CHECK(g.w.result().writes==look_trial_cap);CHECK(g.w.phase()==LookWriter::p_done);}
    // Constants the live card relies on.
    CHECK(look_trial_frames==120&&look_write_default==0.5f&&look_write_clamp==1.0f&&look_settle_ms==3000&&look_deadline_ms==10000);
}
static void test_refusals() {
    struct Case {const char* why;void (*set)(Harness&);};
    const Case cases[]={
        {"no-tracks-look-candidate",[](Harness& h){h.have=false;}},
        {"candidate-evidence-weak",[](Harness& h){h.samples=10;}},
        {"candidate-evidence-weak",[](Harness& h){h.ev=3;h.hit=3;}},
        {"candidate-evidence-weak",[](Harness& h){h.hit=5;}},
        {"no-cockpit",[](Harness& h){h.cockpit=false;}},
        {"kill-switch-file",[](Harness& h){h.stop=true;}},
        {"look-not-neutral",[](Harness& h){h.mem.set(h.game.a(look_yaw),0.5f);h.game.model=m_persist;}},
    };
    for(const Case& c:cases) {
        Harness h(enabled());c.set(h);
        for(int i=0;i<1500;++i)h.tick();   // 25 s
        CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(!h.w.attempted());
        CHECK(h.sink.find(c.why)!=nullptr);
    }
    // Unreadable block: waits, never writes.
    {Harness h(enabled());h.mem.failReads=true;for(int i=0;i<600;++i)h.tick();CHECK(h.mem.writes.empty());CHECK(h.sink.find("reason=read-failure"));}
    // Region too small to contain the used fields.
    {Harness h(enabled());
     LookWriter w(enabled());FakeSink s;LookIn in=h.input();in.v.hi=in.v.lo+0x1000;in.v.locked_hi=in.v.hi;
     for(int i=0;i<10;++i){in.frame=i+1;w.tick(h.mem,s,in);}CHECK(h.mem.writes.empty());CHECK(s.find("fields-outside-region"));}
    // A held look key delays the start: settle resets until the field is neutral again, then the trial runs.
    {Harness h(enabled());h.game.model=m_filter;h.game.pad=2.4f;h.run(400);CHECK(h.mem.writes.empty());
     h.game.pad=0;h.run(3000);CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.result().writes==look_trial_frames);}
}
static void test_trial_persisting_field() {
    Harness h(enabled());h.run(3000);
    const LookResult& r=h.w.result();
    CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.succeeded());CHECK(r.attempted&&!r.aborted&&r.restored&&r.responded);
    CHECK(r.writes==look_trial_frames);CHECK(h.onlyYawWrites());
    CHECK(h.writesOf(0.5f)==look_trial_frames);CHECK(h.mem.writes.size()==look_trial_frames+1u);  // 120 trial writes + ONE restore
    CHECK(bits(h.mem.writes.back().v)==bits(r.orig));CHECK(h.mem.get(h.yawAddr())==0.f||h.game.model!=m_persist);
    CHECK(h.mem.writes.front().v==0.5f);
    // 3 s settle before the first write (60 ticks/s -> 180 ticks, +- one).
    CHECK(h.sink.find("event=look-write-settle;")&&h.sink.find("event=look-write-begin;"));
    CHECK(r.overwritten==0);CHECK(r.persisted>0.99);
    CHECK(r.lag_ticks==1);CHECK(r.follow>0.99);CHECK(std::fabs(r.comp_gain-1.0)<0.01);
    CHECK(r.srt_valid);CHECK(std::fabs(std::fabs(r.srt_gain)-0.44)<0.02);
    double upr=0;const char* src="";CHECK(h.w.calibration(upr,&src));CHECK(std::fabs(upr-1.0/0.44)<0.1);CHECK(std::string(src)=="recovered-camera");
    // Logging: every write has a line with thread id, address, old, new and read-backs.
    CHECK(h.sink.count("event=look-write;n=")==look_trial_frames);
    const std::string* l=h.sink.find("event=look-write;n=1;");
    CHECK(l&&l->find("tid=4242")!=std::string::npos&&l->find("addr=7000fdd410")!=std::string::npos&&l->find(";old=0;")!=std::string::npos&&
          l->find(";new=0.5;")!=std::string::npos&&l->find(";rb=0.5;")!=std::string::npos&&l->find("rb-cam-yaw=")!=std::string::npos&&l->find("srt-yaw=")!=std::string::npos);
    CHECK(h.sink.find("event=look-write;n=120;"));CHECK(!h.sink.find("event=look-write;n=121;"));
    CHECK(h.sink.count("event=look-post;")==look_post_frames);
    CHECK(h.sink.find("event=look-restore;ok=1;why=trial-complete"));
    CHECK(h.sink.find("event=look-write-result;result=ok;reason=field-responded"));
    // One trial per process: terminal state, nothing more is ever written; exit paths are no-ops.
    const size_t n=h.mem.writes.size();for(int i=0;i<2000;++i)h.tick();h.w.abort_and_restore(h.mem,h.sink,"process-exit");
    CHECK(h.mem.writes.size()==n);CHECK(h.w.phase()==LookWriter::p_done);
}
static void test_trial_game_overwrites() {
    {Harness h(enabled());h.game.model=m_overwrite;h.run(3000);   // field recomputed from the pad every frame
     const LookResult& r=h.w.result();
     CHECK(h.w.phase()==LookWriter::p_done);CHECK(!h.w.succeeded());CHECK(r.restored);CHECK(!r.responded);
     CHECK(r.writes==look_trial_frames);CHECK(r.overwritten>=look_trial_frames-2);CHECK(r.persisted<0.05);CHECK(h.onlyYawWrites());
     CHECK(h.sink.find("result=no-response"));CHECK(h.mem.get(h.yawAddr())==0.f);
     double u;CHECK(!h.w.calibration(u,nullptr));}
    {Harness h(enabled());h.game.model=m_filter;h.run(3000);      // smoothing filter: pulls the field towards the pad between our writes
     CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.result().writes==look_trial_frames);CHECK(h.w.result().restored);CHECK(h.onlyYawWrites());
     CHECK(h.w.result().overwritten>0);CHECK(h.mem.get(h.yawAddr())!=0.5f);}
}
// Run until the trial writes, then apply `fault` and finish; returns the harness state.
template<class F> static void abort_case(const char* reason,F fault,Harness& h,unsigned afterWrites=10) {
    for(unsigned i=0;i<2000&&h.w.result().writes<afterWrites;++i)h.tick();
    CHECK(h.w.phase()==LookWriter::p_writing);CHECK(h.w.dirty());
    fault(h);
    for(unsigned i=0;i<60&&h.w.phase()==LookWriter::p_writing;++i)h.tick();
    CHECK(h.w.phase()==LookWriter::p_aborted);CHECK(h.w.result().aborted);
    CHECK(std::string(h.w.result().reason)==reason);
    CHECK(h.sink.find((std::string("result=aborted;reason=")+reason).c_str()));
    CHECK(!h.w.succeeded());
}
static void test_aborts_restore() {
    const float orig=0.f;
    auto restored=[&](Harness& h){CHECK(h.w.result().restored);CHECK(!h.w.dirty());CHECK(bits(h.mem.get(h.yawAddr()))==bits(orig));
        CHECK(h.onlyYawWrites());CHECK(h.w.result().writes<look_trial_frames);CHECK(h.sink.find("event=look-restore;ok=1"));};
    {Harness h(enabled());abort_case("kill-switch-file",[](Harness& x){x.stop=true;},h);restored(h);}
    {Harness h(enabled());abort_case("identity-changed",[](Harness& x){x.lockedPresent=false;},h);restored(h);}
    {Harness h(enabled());abort_case("cockpit-lost",[](Harness& x){x.cockpit=false;},h);restored(h);}
    {Harness h(enabled());abort_case("deadline",[](Harness& x){x.now+=11000;},h);restored(h);}
    {Harness h(enabled());abort_case("paused",[](Harness& x){x.game.moving=false;},h,10);   // aircraft stops: 30 identical ticks
     restored(h);}
    {Harness h(enabled());abort_case("heading-changed",[](Harness& x){x.game.hdg+=1.0;},h);restored(h);}
    {Harness h(enabled());abort_case("readback-out-of-bounds",[](Harness& x){x.mem.set(x.yawAddr(),2.0f);},h);restored(h);}      // game / user pushes the field
    {Harness h(enabled());abort_case("readback-out-of-bounds",[](Harness& x){x.mem.set(x.yawAddr(),-1.3f);},h);restored(h);}
    {Harness h(enabled());abort_case("nonfinite-field",[](Harness& x){x.mem.set(x.yawAddr(),std::nanf(""));},h);restored(h);}
    // Region moves under us (identity), also reported when the lo/hi differ.
    {Harness h(enabled());for(int i=0;i<500&&h.w.result().writes<5;++i)h.tick();
     LookIn in=h.input();in.v.locked_lo+=4096;h.w.tick(h.mem,h.sink,in);CHECK(h.w.phase()==LookWriter::p_aborted);
     CHECK(std::string(h.w.result().reason)=="identity-changed");restored(h);}
    // Read fault mid-trial: restore cannot verify -> reported, stays owed, and the EXIT path retries once it works.
    {Harness h(enabled());
     abort_case("read-failure",[](Harness& x){x.mem.failReads=true;},h);   // the read fails -> abort -> restore unverifiable
     CHECK(!h.w.result().restored);CHECK(h.w.dirty());CHECK(h.sink.find("event=look-restore;ok=0"));
     h.mem.failReads=false;h.w.abort_and_restore(h.mem,h.sink,"process-exit");
     CHECK(h.w.result().restored);CHECK(!h.w.dirty());CHECK(bits(h.mem.get(h.yawAddr()))==bits(orig));}
    // Write fault: abort, restore cannot store; later exit path restores.
    {Harness h(enabled());abort_case("write-failure",[](Harness& x){x.mem.failWrites=true;},h,10);
     // (the write failure itself surfaces as write-failure; restore then fails too)
     CHECK(h.w.dirty());h.mem.failWrites=false;h.w.abort_and_restore(h.mem,h.sink,"dll-detach");CHECK(!h.w.dirty());CHECK(bits(h.mem.get(h.yawAddr()))==bits(orig));}
    // Shutdown / DLL detach mid-trial restores at once and is idempotent.
    {Harness h(enabled());for(int i=0;i<500&&h.w.result().writes<20;++i)h.tick();
     CHECK(h.w.dirty());h.w.abort_and_restore(h.mem,h.sink,"process-exit");restored(h);CHECK(std::string(h.w.result().reason)=="process-exit");
     const size_t n=h.mem.writes.size();h.w.abort_and_restore(h.mem,h.sink,"process-exit");h.tick();h.tick();CHECK(h.mem.writes.size()==n);}
    // Shutdown during the settle: nothing was written, nothing to restore.
    {Harness h(enabled());h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);h.w.abort_and_restore(h.mem,h.sink,"process-exit");
     CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_aborted);CHECK(!h.w.attempted());}
    // Aborts during the settle (identity, kill switch, motion) never write.
    {Harness h(enabled());h.run(60);h.lockedPresent=false;h.run(5);CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_waiting);
     h.lockedPresent=true;h.run(3000);CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.succeeded());}
    {Harness h(enabled());h.run(100);h.stop=true;h.run(10);CHECK(h.mem.writes.empty());h.stop=false;h.run(3000);CHECK(h.w.succeeded());}
    // A restore whose read-back disagrees (game rewrites immediately) is NOT claimed verified.
    {Harness h(enabled());for(int i=0;i<500&&h.w.result().writes<119;++i)h.tick();
     struct Stubborn final:Memory {FakeMem& m;explicit Stubborn(FakeMem& mm):m(mm){}
        bool read(uint64_t a,void* o,uint32_t n) noexcept override {return m.read(a,o,n);}
        bool write_float(uint64_t a,float v) noexcept override {return m.write_float(a,v==0.f?0.01f:v);}  // "game" rewrites the restore value
     } st(h.mem);
     LookIn in=h.input();
     // 120th write succeeds (v=0.5), the restore is then falsified: not verified -> aborted with restored=0, dirty.
     h.w.tick(st,h.sink,in);
     CHECK(h.w.phase()==LookWriter::p_aborted);CHECK(!h.w.result().restored);CHECK(h.w.dirty());
     h.w.abort_and_restore(h.mem,h.sink,"process-exit");CHECK(h.w.result().restored);CHECK(!h.w.dirty());}
}
static void test_bounds_never_other_words() {
    // Across every scenario above only the yaw address is ever written (checked via onlyYawWrites);
    // here additionally the address is in the region and word-aligned, with a deliberately hostile fake.
    Harness h(enabled());h.run(3000);
    for(auto& x:h.mem.writes){CHECK(x.a>=h.mem.base&&x.a<h.mem.base+h.mem.size);CHECK((x.a&3)==0);CHECK(std::fabs(x.v)<=look_write_clamp);}
    CHECK(h.mem.writes.size()>0);
}

// ------------------------------------------------------------------ stage 2b
struct DriveHarness {
    FakeMem mem;Game game{mem,0x7000000000ull};FakeSink sink;LookDrive d;double now=0;uint64_t frame=1;
    bool stop=false,cockpit=true,gate=true,locked=true;double head[2]={0,0};bool headValid=true;double headAge=10;double upr=2.0;bool calibrated=true;
    explicit DriveHarness(bool enable=true):d(LookDrive::Options{enable}) {game.model=m_persist;}
    uint64_t addr(unsigned f) const {return game.a(f);}
    void tick() {
        DriveIn in;LookIn& b=in.base;b.now_ms=now;b.frame=frame;b.tid=1;b.stop_file=stop;
        b.v.have=true;b.v.eff=game.eff;b.v.lo=mem.base;b.v.hi=mem.base+mem.size;b.v.locked_present=locked;b.v.locked_lo=mem.base;b.v.locked_hi=mem.base+mem.size;
        b.v.cockpit=cockpit;in.gate_ok=gate;in.gate_eff=game.eff;in.units_per_rad=upr;in.calibrated=calibrated;
        in.head.valid=headValid;in.head.yaw=head[0];in.head.pitch=head[1];in.head.age_ms=headAge;
        d.tick(mem,sink,in);game.step();now+=1000.0/60;++frame;
    }
    void run(unsigned n){for(unsigned i=0;i<n;++i)tick();}
    bool onlyLookWrites() const {for(auto& x:mem.writes)if(x.a!=addr(look_yaw)&&x.a!=addr(look_pitch))return false;return true;}
};
static void test_drive() {
    {DriveHarness h(false);h.head[0]=0.3;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.d.phase()==LookDrive::p_off);}
    {DriveHarness h;h.gate=false;h.head[0]=0.3;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.d.phase()==LookDrive::p_waiting);CHECK(h.sink.find("stage-2a-not-succeeded"));}
    {DriveHarness h;h.headValid=false;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.sink.find("no-head-source"));}
    {DriveHarness h;h.headAge=1000;h.head[0]=0.3;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.sink.find("no-head-source"));}
    {DriveHarness h;h.mem.set(h.addr(look_yaw),0.5f);h.game.model=m_persist;h.head[0]=0.3;h.run(100);CHECK(h.mem.writes.empty());CHECK(h.sink.find("look-not-neutral"));}
    {   // Follows the head with rate limit, deadzone, clamp; restores on the kill switch.
        DriveHarness h;h.head[0]=0.3;h.head[1]=0.0;h.run(1);
        CHECK(h.d.phase()==LookDrive::p_active);CHECK(h.mem.writes.size()==2);
        CHECK(std::fabs(h.mem.writes[0].v-drive_rate_per_tick)<1e-6);                       // first step limited to the rate
        h.run(200);
        const float want=float((0.3-drive_deadzone_rad)*2.0);
        CHECK(std::fabs(h.mem.get(h.addr(look_yaw))-want)<1e-4);CHECK(h.onlyLookWrites());
        float prev=0;for(size_t i=0;i<h.mem.writes.size();i+=2){CHECK(std::fabs(h.mem.writes[i].v-prev)<=drive_rate_per_tick+1e-6);prev=h.mem.writes[i].v;}
        h.head[0]=5.0;h.run(300);CHECK(h.mem.get(h.addr(look_yaw))==drive_yaw_clamp);                // clamp
        h.head[0]=-5.0;h.run(300);CHECK(h.mem.get(h.addr(look_yaw))==-drive_yaw_clamp);
        h.head[0]=0.004;h.run(300);CHECK(h.mem.get(h.addr(look_yaw))==0.f);                            // inside the deadzone
        h.head[1]=-1.0;h.run(200);CHECK(h.mem.get(h.addr(look_pitch))==drive_pitch_down);
        h.head[1]=2.0;h.run(200);CHECK(h.mem.get(h.addr(look_pitch))==drive_pitch_up);
        for(auto& x:h.mem.writes)CHECK(std::fabs(x.v)<=drive_yaw_clamp+1e-6);
        h.head[0]=1.0;h.run(40);CHECK(h.mem.get(h.addr(look_yaw))!=0.f);
        h.stop=true;h.run(2);
        CHECK(h.d.phase()==LookDrive::p_aborted);CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f&&h.mem.get(h.addr(look_pitch))==0.f);
        CHECK(h.sink.find("event=look-drive-restore;ok=1;why=kill-switch-file"));CHECK(h.onlyLookWrites());
        const size_t n=h.mem.writes.size();h.stop=false;h.run(50);CHECK(h.mem.writes.size()==n);}
    {   // Stale head: relaxes to the original, rate limited.
        DriveHarness h;h.head[0]=0.5;h.run(100);CHECK(h.mem.get(h.addr(look_yaw))>0.5f);h.headAge=1000;h.run(100);CHECK(h.mem.get(h.addr(look_yaw))==0.f);}
    {   // Exit path mid-drive restores both fields; identity/cockpit/gate loss aborts with restore.
        DriveHarness h;h.head[0]=0.4;h.head[1]=0.2;h.run(50);CHECK(h.d.dirty());h.d.abort_and_restore(h.mem,h.sink,"process-exit");
        CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f&&h.mem.get(h.addr(look_pitch))==0.f);CHECK(h.d.phase()==LookDrive::p_aborted);}
    for(int k=0;k<5;++k) {
        DriveHarness h;h.head[0]=0.4;h.run(30);
        if(k==0)h.locked=false;else if(k==1)h.cockpit=false;else if(k==2)h.gate=false;else if(k==3)h.mem.set(h.addr(look_yaw),2.6f);else h.game.moving=false;
        h.run(40);
        CHECK(h.d.phase()==LookDrive::p_aborted);CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f);CHECK(h.onlyLookWrites());
    }
    {DriveHarness h;h.head[0]=0.4;h.run(30);h.mem.failReads=true;h.run(2);CHECK(h.d.phase()==LookDrive::p_aborted);CHECK(h.d.dirty());   // unverifiable restore stays owed
     h.mem.failReads=false;h.d.abort_and_restore(h.mem,h.sink,"process-exit");CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f);}
    {DriveHarness h;h.head[0]=0.4;h.run(30);h.mem.failWrites=true;h.run(2);CHECK(h.d.phase()==LookDrive::p_aborted);CHECK(h.d.dirty());
     h.mem.failWrites=false;h.d.abort_and_restore(h.mem,h.sink,"dll-detach");CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f);}
    {   // Duration cap: normal end, restored.
        LookDrive::Options o;o.enabled=true;o.max_ms=500;DriveHarness h;h.d=LookDrive(o);h.head[0]=0.4;h.run(100);
        CHECK(h.d.phase()==LookDrive::p_done);CHECK(!h.d.dirty());CHECK(h.mem.get(h.addr(look_yaw))==0.f);CHECK(h.sink.find("result=done;reason=duration-cap"));}
}
static void test_math() {
    CHECK(head_to_units(0.0,2.0,-2,2)==0.f);CHECK(head_to_units(0.005,2.0,-2,2)==0.f);CHECK(head_to_units(-0.005,2.0,-2,2)==0.f);
    CHECK(std::fabs(head_to_units(0.5,2.0,-2,2)-float((0.5-drive_deadzone_rad)*2))<1e-5);CHECK(head_to_units(-0.5,2.0,-2,2)<0);
    CHECK(head_to_units(10,2.0,-2,2)==2.f);CHECK(head_to_units(-10,2.0,-2,2)==-2.f);CHECK(head_to_units(std::nan(""),2.0,-2,2)==0.f);CHECK(head_to_units(0.5,0.0,-2,2)==0.f);
    CHECK(rate_limit(0,1,0.1f)==0.1f);CHECK(rate_limit(0,-1,0.1f)==-0.1f);CHECK(rate_limit(0.5f,0.52f,0.1f)==0.52f);CHECK(rate_limit(std::nanf(""),1,0.1f)==0.f);
    // head_yaw_pitch: Ry(theta) turns the head LEFT (positive yaw); Rx(phi) looks UP (positive pitch).
    const double th=0.5236,c=std::cos(th),s=std::sin(th);
    const double Ry[9]={c,0,s, 0,1,0, -s,0,c};double y,p;
    CHECK(head_yaw_pitch(Ry,0.0,y,p));CHECK(std::fabs(y-th)<1e-9&&std::fabs(p)<1e-9);
    CHECK(head_yaw_pitch(Ry,th,y,p));CHECK(std::fabs(y)<1e-9);                      // neutral-relative
    CHECK(head_yaw_pitch(Ry,-3.0,y,p));CHECK(std::fabs(y-wrap(th+3.0))<1e-9);       // wraps
    const double Rx[9]={1,0,0, 0,c,-s, 0,s,c};
    CHECK(head_yaw_pitch(Rx,0.0,y,p));CHECK(std::fabs(p-th)<1e-9&&std::fabs(y)<1e-9);
    const double bad[9]={0,0,0,0,0,0,0,0,0};CHECK(!head_yaw_pitch(bad,0,y,p));
    double nanR[9]={1,0,0,0,1,0,0,0,1};nanR[4]=std::nan("");CHECK(!head_yaw_pitch(nanR,0,y,p));
    // Double-rotation helper: once the game look renders 0.3 rad of a 0.5 rad head turn, the per-eye residual is 0.2 rad.
    CHECK(std::fabs(residual_after_game_look(0.5,0.3)-0.2)<1e-12);CHECK(std::fabs(residual_after_game_look(0.5,0.5))<1e-12);
    CHECK(std::fabs(residual_after_game_look(3.0,-3.0)-wrap(6.0))<1e-12);
    // Mailbox round trip.
    LookHeadMailbox mb;double a,b,t;CHECK(!mb.read(a,b,t));mb.publish(0.1,-0.2,5.0);CHECK(mb.read(a,b,t)&&a==0.1&&b==-0.2&&t==5.0);
}
// ------------------------------------------------------------------ J-look-locator: writer gate (C)
static void test_quiet_gate() {
    CHECK(look_quiet_ms==2000&&look_settle_ms==3000);
    // waiting: a look event in the last 2 s keeps the writer waiting (not settling, not writing)
    {Harness h(enabled());h.quietMs=500;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(h.sink.find("reason=look-event-recent"));
     h.quietMs=2500;h.run(5);CHECK(h.w.phase()==LookWriter::p_settling);}
    // a look event DURING the settle restarts the settle: not an abort, not a reset to waiting, nothing written
    {Harness h(enabled());h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);
     h.quietMs=100;h.run(240);                                   // 4 s of look activity (longer than the 3 s settle)
     CHECK(h.w.phase()==LookWriter::p_settling);CHECK(!h.w.result().aborted);CHECK(h.mem.writes.empty());
     CHECK(h.sink.count("event=look-write-settle-restart;reason=look-event")==1);CHECK(h.sink.count("event=look-write-settle-reset")==0);
     h.quietMs=1e9;h.run(60*2);                                  // quiet again: only 2 s since the last event, the settle needs 3 s
     CHECK(h.w.phase()==LookWriter::p_settling);CHECK(h.mem.writes.empty());
     h.run(60*2);                                                // 4 s of quiet: the trial has begun
     CHECK(h.w.phase()!=LookWriter::p_settling&&h.w.attempted());
     h.run(3000);CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.succeeded());CHECK(h.w.result().writes==look_trial_frames);CHECK(h.onlyYawWrites());
     CHECK(h.mem.writes.size()==look_trial_frames+1u);}
    // once the trial writes, a look-event flag is not an abort reason (the safety limits are unchanged)
    {Harness h(enabled());h.run(60*4);CHECK(h.w.phase()==LookWriter::p_writing);h.quietMs=0;h.run(3000);
     CHECK(h.w.phase()==LookWriter::p_done);CHECK(h.w.result().writes==look_trial_frames);}
    // pitch must be neutral too (waiting and settle)
    {Harness h(enabled());h.mem.set(h.game.a(look_pitch),0.2f);h.run(300);CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_waiting);
     CHECK(h.sink.find("reason=look-pitch-not-neutral"));
     h.mem.set(h.game.a(look_pitch),0.f);h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);
     h.mem.set(h.game.a(look_pitch),0.2f);h.run(5);CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(h.sink.find("event=look-write-settle-reset;reason=look-pitch-not-neutral"));
     CHECK(h.mem.writes.empty());
     h.mem.set(h.game.a(look_pitch),0.f);h.run(3000);CHECK(h.w.succeeded());}
    // yaw non-neutral during the settle: still a reset to waiting (unchanged), never a write
    {Harness h(enabled());h.run(60);h.mem.set(h.game.a(look_yaw),0.3f);h.game.model=m_persist;h.run(3);CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(h.mem.writes.empty());}
    // the structural re-check must be recent (waiting and settle)
    {Harness h(enabled());h.structAge=5000;h.run(300);CHECK(h.mem.writes.empty());CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(h.sink.find("reason=structure-unconfirmed"));
     h.structAge=0;h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);
     h.structAge=5000;h.run(3);CHECK(h.w.phase()==LookWriter::p_waiting);CHECK(h.sink.find("event=look-write-settle-reset;reason=structure-unconfirmed"));
     CHECK(h.mem.writes.empty());h.structAge=0;h.run(3000);CHECK(h.w.succeeded());}
    // the settle never completes in less than look_quiet_ms of quiet, whatever the configured settle length
    {LookWriter::Options o=enabled();o.settle_ms=100;Harness h(o);h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);h.run(60);CHECK(h.w.phase()==LookWriter::p_settling);
     CHECK(h.mem.writes.empty());h.run(3000);CHECK(h.w.phase()==LookWriter::p_done);}
    // defaults refuse: a default LookView is neither quiet nor confirmed
    {LookView v;CHECK(v.quiet_ms==0&&v.struct_age_ms>look_struct_max_age_ms);}
}
static void test_config_keys() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.ee_camera_look_write_trial()&&!Config{}.ee_camera_look_drive());
    CHECK(!parse_config("").config.ee_camera_look_write_trial());
    CHECK(parse_config("ee_camera_look_write_trial=true\n").config.ee_camera_look_write_trial());
    CHECK(!parse_config("ee_camera_look_write_trial=true\n").config.ee_camera_look_drive());
    CHECK(parse_config("ee_camera_look_drive=1\n").config.ee_camera_look_drive());
    CHECK(!parse_config("ee_camera_look_drive=false\n").config.ee_camera_look_drive());
    for(const char* bad:{"ee_camera_look_write_trial=maybe\n","ee_camera_look_drive=yes\n","ee_camera_look_write_trial=\n"})
        CHECK(parse_config(bad).issue!=ConfigIssue::none);
    // The keys are known (no unknown-key invalidation) and fit the 24-key cap together with a full live set.
    CHECK(parse_config("ee_camera_look_write_trial=true\nee_camera_look_drive=true\n").issue==ConfigIssue::none);
    CHECK(max_config_keys==32); // raised from 24 for xr_look_drive_keys / xr_look_drive_test_sweep
}

int main() {
    test_default_off_and_options();test_refusals();test_trial_persisting_field();test_trial_game_overwrites();
    test_aborts_restore();test_bounds_never_other_words();test_quiet_gate();test_drive();test_math();test_config_keys();
    // TODO(stage-2b, double rotation): an end-to-end test of the producer's per-eye delta with head-driven game look
    // (delta = head - rendered game look, see residual_after_game_look) needs the producer to publish the head into
    // g_look_head; it is NOT wired, so there is nothing to test yet (see J-look-write-findings.md, section 2b).
    std::printf("ee_look_write_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
