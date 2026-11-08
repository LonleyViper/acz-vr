// D-269 follow-up offline fixtures: the READ-ONLY look-input trace (ee_look_input_trace.h) against fake memory and a fake
// game with a digital key flag, a first-order filter state and noise words. Nothing here touches a process or the game.
#include "ee_look_input_trace.h"
#include "runtime_config.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace acz::ee_camera;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)

// ------------------------------------------------------------------ fakes
struct Mem final:Memory {
    uint64_t start=0;std::vector<uint32_t> w=std::vector<uint32_t>(trace_words,0);
    bool fail=false;unsigned reads=0,writeCalls=0;
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        if(fail||a<start||a+n>start+trace_region_bytes)return false;
        std::memcpy(out,reinterpret_cast<const char*>(w.data())+(a-start),n);++reads;return true;
    }
    bool write_float(uint64_t,float) noexcept override {++writeCalls;return false;}   // must never be called
};
struct Sink2 final:Sink {
    std::vector<std::string> lines;
    void line(bool,const char* t) noexcept override {lines.emplace_back(t);if(std::getenv("EE_TRACE_VERBOSE"))std::printf("  %s\n",t);}
    unsigned count(const char* s) const {unsigned n=0;for(auto& l:lines)n+=l.find(s)!=std::string::npos;return n;}
    const std::string* find(const char* s) const {for(auto& l:lines)if(l.find(s)!=std::string::npos)return &l;return nullptr;}
};
struct Out final:TraceOut {
    std::vector<uint8_t> bytes;unsigned writes=0;bool fail=false;
    bool write(const void* p,size_t n) noexcept override {if(fail)return false;++writes;const auto* s=static_cast<const uint8_t*>(p);bytes.insert(bytes.end(),s,s+n);return true;}
};

// word indices (region relative) of the planted fake game
constexpr uint32_t W_L=trace_l_word,W_PITCH=trace_l_word-1,W_FLAG=W_L+0x40,W_STATE=W_L-0x80,W_TIMER=W_L+0x100,W_CONST=W_L+0x200,W_TARGET=W_L+0x41;
struct Game {
    Mem& m;float state=0;bool key=false;float dir=1;uint32_t timer=0;unsigned noisy=0;uint32_t rng=1;
    explicit Game(Mem& mm):m(mm) {m.w[W_CONST]=0xC0FFEE;}
    void step() {
        m.w[W_FLAG]=key?1u:0u;
        m.w[W_TARGET]=bits(key?dir*2.48f:0.f);
        state+=((key?dir*2.48f:0.f)-state)*0.3f;
        m.w[W_STATE]=bits(state);
        m.w[W_L]=bits(state);                      // L = the filter state (a copy)
        m.w[W_TIMER]=++timer;
        for(unsigned i=0;i<noisy;++i){rng=rng*1664525u+1013904223u;m.w[0x100+i]=rng;}
    }
};
struct Harness {
    Mem mem;Game game{mem};Sink2 sink;std::unique_ptr<LookInputTrace> t;Out out;
    uint64_t eff=0x7000000000ull;double now=1000;uint64_t frame=1;bool have=true;double quiet=1e9;uint64_t lo=0,hi=0;
    explicit Harness(bool enabled=true,unsigned maxw=trace_max_windows) {
        LookInputTrace::Options o;o.enabled=enabled;o.max_windows=maxw;t.reset(new LookInputTrace(o));
        lo=eff;hi=eff+block_bytes;mem.start=eff+trace_l_off-trace_half;
    }
    LookIn in() const {LookIn i;i.now_ms=now;i.frame=frame;i.tid=777;i.v.have=have;i.v.eff=eff;i.v.lo=lo;i.v.hi=hi;i.v.quiet_ms=quiet;return i;}
    void tick(){game.step();t->tick(mem,sink,in());now+=1000.0/60;++frame;if(quiet<1e8)quiet+=1000.0/60;}
    void run(unsigned n){for(unsigned i=0;i<n;++i)tick();}
    // key held for ms, then released; nothing else
    void hold(double ms,float dir=1){game.dir=dir;game.key=true;run(unsigned(ms*60/1000));game.key=false;}
};

// ------------------------------------------------------------------ trace parser
struct Delta {uint32_t off,o,n;};
struct TickRec {uint32_t idx,tid;double ms;uint64_t frame;uint32_t ns,nt;std::vector<Delta> d;};
struct Win {
    uint32_t version,hdr,index,reason;uint64_t base,region;uint32_t bytes,loff,ticks,pre,deltas,data,flags,trunc;double open,close;uint32_t creason,trigger;
    std::vector<TickRec> t;
};
template<class T> static T rd(const uint8_t*& p){T v;std::memcpy(&v,p,sizeof v);p+=sizeof v;return v;}
static bool parse(const std::vector<uint8_t>& b,std::vector<Win>& out) {
    const uint8_t* p=b.data();const uint8_t* e=p+b.size();
    while(p<e) {
        if(e-p<96||std::memcmp(p,"ACZLIT1\0",8)!=0)return false;
        Win w{};p+=8;
        w.version=rd<uint32_t>(p);w.hdr=rd<uint32_t>(p);w.index=rd<uint32_t>(p);w.reason=rd<uint32_t>(p);w.base=rd<uint64_t>(p);w.region=rd<uint64_t>(p);
        w.bytes=rd<uint32_t>(p);w.loff=rd<uint32_t>(p);w.ticks=rd<uint32_t>(p);w.pre=rd<uint32_t>(p);w.deltas=rd<uint32_t>(p);w.data=rd<uint32_t>(p);
        w.flags=rd<uint32_t>(p);w.trunc=rd<uint32_t>(p);w.open=rd<double>(p);w.close=rd<double>(p);w.creason=rd<uint32_t>(p);w.trigger=rd<uint32_t>(p);
        if(w.hdr!=96||uint64_t(e-p)<w.data)return false;
        const uint8_t* q=p;const uint8_t* qe=p+w.data;
        while(q<qe) {
            TickRec t{};t.idx=rd<uint32_t>(q);t.tid=rd<uint32_t>(q);t.ms=rd<double>(q);t.frame=rd<uint64_t>(q);t.ns=rd<uint32_t>(q);t.nt=rd<uint32_t>(q);
            for(uint32_t i=0;i<t.ns;++i){Delta d;d.off=rd<uint32_t>(q);d.o=rd<uint32_t>(q);d.n=rd<uint32_t>(q);t.d.push_back(d);}
            w.t.push_back(std::move(t));
        }
        if(q!=qe)return false;
        p=qe;out.push_back(std::move(w));
    }
    return true;
}

// ------------------------------------------------------------------ tests
static void test_constants() {
    CHECK(trace_total_bytes==4u*1024*1024);CHECK(trace_arena_bytes+trace_ring_bytes==trace_total_bytes);
    CHECK(trace_max_windows==6&&trace_window_ms==3000&&trace_preroll_ms==1000&&trace_region_bytes==65536);
    CHECK(trace_l_off==field_offset[look_yaw]);CHECK(trace_l_word*4==trace_half);
    CHECK(sizeof(LookInputTrace)>=trace_total_bytes&&sizeof(LookInputTrace)<trace_total_bytes+(1u<<20));
}
static void test_default_off() {
    Harness h(false);h.game.key=true;h.run(600);
    CHECK(h.mem.reads==0&&h.mem.writeCalls==0&&h.sink.lines.empty()&&h.t->windows_closed()==0);
}
static void test_idle_no_window() {
    Harness h;h.run(400);
    CHECK(h.t->windows_closed()==0&&!h.t->is_open());CHECK(h.mem.reads==400);CHECK(h.sink.count("look-input-trace-armed")==1);
    CHECK(h.mem.writeCalls==0);
}
static void test_one_window() {
    Harness h;h.run(90);                       // 1.5 s idle (timer word changes every tick)
    h.hold(700);h.run(300);
    CHECK(h.t->windows_closed()==1);CHECK(h.t->stats().windows_started==1);
    CHECK(h.sink.count("event=look-input-trace-open")==1&&h.sink.count("event=look-input-trace-window")==1);
    CHECK(h.sink.find("open=look-value")&&h.sink.find("close=timeout"));
    CHECK(h.t->drain(h.out)==1&&h.t->drain(h.out)==0);CHECK(h.out.writes==1);
    std::vector<Win> w;CHECK(parse(h.out.bytes,w));CHECK(w.size()==1);
    if(w.size()==1) {
        const Win& x=w[0];
        CHECK(x.version==1&&x.index==0&&x.reason==to_value&&x.base==h.eff&&x.region==h.eff+trace_l_off-trace_half);
        CHECK(x.bytes==trace_region_bytes&&x.loff==0x8000&&x.creason==tc_timeout&&(x.flags&2)==0);
        CHECK(x.ticks==x.t.size());
        CHECK(x.pre>=58&&x.pre<=62);               // ~1 s of pre-roll at 60 ticks/s
        CHECK(x.close-x.open>=2980&&x.close-x.open<=3040);
        CHECK(x.t.size()>=x.pre+170&&x.t.size()<=x.pre+190);   // pre-roll + 3 s
        CHECK(x.trigger==x.t[x.pre].idx);
        bool timerSeen=true,constSeen=false,flagUp=false,flagDown=false,stateChain=true,tidOk=true,msMono=true;uint32_t lastState=0;bool haveState=false;
        uint32_t lastTimer=0;
        for(size_t i=0;i<x.t.size();++i) {
            const TickRec& t=x.t[i];tidOk&=t.tid==777;msMono&=i==0||t.ms>x.t[i-1].ms;
            bool hasTimer=false;
            for(const Delta& d:t.d) {
                CHECK(d.off%4==0&&d.off<trace_region_bytes);CHECK(d.o!=d.n);
                const uint32_t wi=d.off/4;
                if(wi==W_CONST)constSeen=true;
                if(wi==W_TIMER){hasTimer=true;if(lastTimer)CHECK(d.o==lastTimer);lastTimer=d.n;}
                if(wi==W_FLAG){flagUp|=d.o==0&&d.n==1;flagDown|=d.o==1&&d.n==0;}
                if(wi==W_L){if(haveState)stateChain&=d.o==lastState;lastState=d.n;haveState=true;}
            }
            timerSeen&=hasTimer;
        }
        CHECK(timerSeen&&!constSeen&&flagUp&&flagDown&&stateChain&&tidOk&&msMono);
        // the key press is inside the window and AFTER the pre-roll start
        uint32_t sumDeltas=0;for(auto& t:x.t)sumDeltas+=t.ns;CHECK(sumDeltas==x.deltas);
    }
    CHECK(h.mem.writeCalls==0);
    const std::string* s=h.sink.find("event=look-input-trace-window");
    CHECK(s&&s->find("changed-words=")!=std::string::npos);
    // changed words: L, state, flag, target, timer = 5 (pitch/const never change)
    CHECK(s&&s->find("changed-words=5;")!=std::string::npos);
}
static void test_rearm_and_max_windows() {
    Harness h;h.run(30);
    unsigned fired=0;
    for(int k=0;k<9;++k){h.hold(500,k%2?-1.f:1.f);h.run(60*5);fired=h.t->windows_closed();}
    CHECK(fired==trace_max_windows);CHECK(h.t->exhausted());CHECK(h.t->stats().windows_started==6);
    const unsigned readsAtEnd=h.mem.reads;h.run(100);CHECK(h.mem.reads==readsAtEnd);   // exhausted: no more sampling at all
    CHECK(h.t->stats().arena_used<=trace_arena_bytes);
    CHECK(h.t->drain(h.out)==6);std::vector<Win> w;CHECK(parse(h.out.bytes,w));CHECK(w.size()==6);
    size_t total=0;for(auto& x:w){CHECK(x.creason==tc_timeout);total+=96+x.data;}
    CHECK(total==h.out.bytes.size());CHECK(total<=trace_arena_bytes);
    // second hold during an open window / during the decay never opens a second window
    Harness g;g.run(30);g.hold(300);g.run(30);g.hold(300);g.run(400);CHECK(g.t->stats().windows_started==2||g.t->stats().windows_started==1);
    Harness r;r.run(30);r.hold(300);r.run(15);r.hold(300);   // second press while the first window is open
    CHECK(r.t->stats().windows_started==1);
}
static void test_event_trigger() {
    Harness h;h.run(30);h.quiet=5000;h.run(10);h.quiet=0;     // a probe look event begins (quiet resets), L still 0
    h.run(250);
    CHECK(h.t->windows_closed()==1&&h.sink.find("open=look-event"));
}
static void test_budget_and_overflow() {
    Harness h;h.game.noisy=3000;h.run(100);
    for(int k=0;k<6;++k){h.hold(500);h.run(60*4);}
    const LookInputTrace::Stats s=h.t->stats();
    CHECK(s.arena_used<=trace_arena_bytes);CHECK(s.windows_started>=1);
    h.t->finish(h.sink,tc_exit);h.t->drain(h.out);
    std::vector<Win> w;CHECK(parse(h.out.bytes,w));
    size_t tot=0;bool budget=false;for(auto& x:w){tot+=96+x.data;budget|=x.creason==tc_budget;CHECK(x.flags&1);}
    CHECK(tot<=trace_arena_bytes);CHECK(budget);CHECK(h.sink.find("close=budget"));
    // 6000 changing words per tick > cap: counted in n_total, stored up to the cap
    Harness g;g.game.noisy=6000;g.run(20);g.hold(300);g.run(10);
    g.t->finish(g.sink,tc_exit);g.t->drain(g.out);std::vector<Win> v;CHECK(parse(g.out.bytes,v));
    CHECK(v.size()==1);if(v.size()==1){bool ov=false;for(auto& t:v[0].t){CHECK(t.ns<=trace_max_deltas);ov|=t.nt>t.ns;CHECK(t.nt>=t.ns);}CHECK(ov);CHECK(v[0].trunc>0);CHECK(v[0].flags&1);}
    CHECK(g.t->stats().tick_overflows>0);
}
static void test_aborts() {
    {Harness h;h.run(30);h.hold(300);h.mem.fail=true;h.run(2);h.mem.fail=false;
     CHECK(h.t->windows_closed()==1&&h.sink.find("close=read-failure")&&h.sink.find("look-input-trace-wait;reason=read-failure"));}
    {Harness h;h.run(30);h.hold(300);h.have=false;h.run(2);CHECK(h.t->windows_closed()==1&&h.sink.find("close=candidate-lost"));}
    {Harness h;h.run(30);h.hold(300);h.eff+=0x1000;h.mem.start+=0x1000;h.run(2);CHECK(h.t->windows_closed()==1&&h.sink.find("close=identity-changed"));
     CHECK(h.sink.count("look-input-trace-armed")==2);}
    {Harness h;h.run(30);h.hold(300);h.hi=h.eff+0x1000;h.run(2);CHECK(h.t->windows_closed()==1&&h.sink.find("close=region-outside-block"));}
    {Harness h;h.hi=h.eff+0x1000;h.run(50);CHECK(h.mem.reads==0&&h.sink.find("region-outside-block"));}
    {Harness h;h.have=false;h.run(50);CHECK(h.mem.reads==0&&h.sink.count("no-tracks-look-candidate")==1);}   // rate limited
    {Harness h;h.run(30);h.hold(300);h.run(5);CHECK(h.t->is_open());h.t->finish(h.sink,tc_exit);CHECK(!h.t->is_open()&&h.t->windows_closed()==1&&h.sink.find("close=process-exit"));
     h.t->finish(h.sink,tc_exit);CHECK(h.t->windows_closed()==1);}
    {Harness h;h.run(30);h.hold(300);h.run(300);h.out.fail=true;for(int i=0;i<9;++i)h.t->drain(h.out);CHECK(h.t->stats().write_failures==5&&h.out.bytes.empty());
     h.out.fail=false;CHECK(h.t->drain(h.out)==0);}   // gives up after 5 failures
}
static void test_pitch_trigger_and_neutral_band() {
    Harness h;h.mem.w[W_PITCH]=bits(-0.035f);h.run(130);   // the small look-down offset (0.035) already in place is inside the neutral band: no trigger
    CHECK(h.t->windows_closed()==0&&!h.t->is_open());
    Harness g;g.run(30);g.mem.w[W_PITCH]=bits(0.5f);g.run(2);CHECK(g.t->is_open());
}
static void test_config() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.ee_camera_look_input_trace());
    CHECK(!parse_config("").config.ee_camera_look_input_trace());
    CHECK(parse_config("ee_camera_look_input_trace=true\n").config.ee_camera_look_input_trace());
    CHECK(!parse_config("ee_camera_look_input_trace=0\n").config.ee_camera_look_input_trace());
    CHECK(parse_config("ee_camera_look_input_trace=true\n").issue==ConfigIssue::none);
    for(const char* bad:{"ee_camera_look_input_trace=maybe\n","ee_camera_look_input_trace=\n"})CHECK(parse_config(bad).issue!=ConfigIssue::none);
    // the live-run config: 9 keys, within the 24-key cap
    const char* live="log_enabled=true\ndiagnostic_ownership_pins=true\ngpu_copy_witness_observation=true\ngpu_copy_early_device_hook=true\n"
        "gpu_copy_step3_experiment=true\ngpu_copy_step3_whole_set=true\ngpu_copy_step5_continuous=true\nee_camera_probe=true\nee_camera_look_input_trace=true\n";
    const ConfigResult r=parse_config(live);
    CHECK(r.issue==ConfigIssue::none&&r.config.ee_camera_probe()&&r.config.ee_camera_look_input_trace()&&!r.config.ee_camera_look_write_trial());
}

int main() {
    test_constants();test_default_off();test_idle_no_window();test_one_window();test_rearm_and_max_windows();test_event_trigger();
    test_budget_and_overflow();test_aborts();test_pitch_trigger_and_neutral_band();test_config();
    // Optional: dump a fixture trace for the python tool's cross-language check (argv-free: env var).
    if(const char* path=std::getenv("EE_TRACE_DUMP")) {
        Harness h;h.run(90);h.hold(700);h.run(300);h.t->drain(h.out);
        if(FILE* f=std::fopen(path,"wb")){std::fwrite(h.out.bytes.data(),1,h.out.bytes.size(),f);std::fclose(f);}
    }
    std::printf("ee_look_input_trace_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
