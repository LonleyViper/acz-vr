#include "gpu_copy_witness.h"
#include "gpu_copy_ledger_fixture.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <thread>
using namespace acz::gpu_copy;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
namespace acz::gpu_copy {
struct WitnessFixtureAccess {
    static void enable(Witness& w) {w.enabled_=true;}
    static bool attach(Witness& w,Session& s,const Key& k) {return w.attach(s,k);}
};
}
using F=FixtureAccess;
static_assert(sizeof(Witness)<=512,"Keep fixture/witness storage bounded");
static_assert(sizeof(Session)<=512,"No large ledger fixture stack storage");
static Identity identity() {return {1,2,3,4,6,7,8,true};}
static Admission admission() {auto a=fixture_admission();a.key.generation=1;return a;}
struct Fake final:Backend {
    unsigned maps=0,drops=0,releases=0,insertions=0;
    FenceSample sample{admission().key,1,true};
    FenceSample poll() noexcept override {return sample;}
    bool read() noexcept override {++maps;return true;}
    void drop_logical_source_lease(uint64_t) noexcept override {++drops;}
    void release_private(uint64_t) noexcept override {++releases;}
    void untouched() const {CHECK(!maps && !drops && !releases);}
    void execute_callback(Witness& w,Session& s,const Key& k) {
        // Fake native forwarding seam: registration must already be visible.
        CHECK(s.snapshot().state==State::submitted && w.snapshot().pending);
        CHECK(!w.before_execute(k,1,true));
    }
};
static void install(Witness& w,int omit=-1) {
    WitnessFixtureAccess::enable(w);
    for(unsigned n=0;n<static_cast<unsigned>(Coverage::count);++n)
        if(static_cast<int>(n)!=omit)w.coverage(static_cast<Coverage>(n),true);
}
static void ready(Witness& w) {install(w);w.reset(true,identity());CHECK(w.insertion_allowed(identity()));}
static void refused(Witness& w,Session& s,Fake& b) {
    CHECK(!w.insertion_allowed(identity()));CHECK(b.insertions==0);
    CHECK(!F::record(s));CHECK(!s.snapshot().records);CHECK(!s.read() && !s.retire());b.untouched();
}
static void prepared(Witness& w,Session& s,Fake& b,bool closed=true) {
    CHECK(!F::record(s));CHECK(!b.insertions && !s.snapshot().records);b.untouched();
    const auto a=admission();CHECK(w.insertion_allowed(identity()));
    CHECK(F::arm(s,true,b,a));CHECK(F::reserve(s,a));CHECK(F::created(s));
    CHECK(F::record(s));CHECK(WitnessFixtureAccess::attach(w,s,a.key));
    ++b.insertions; // Fake possible recording only; never a native copy.
    if(closed)w.close(true);
    CHECK(!s.read() && !s.retire());b.untouched();
}
static void retained(Session& s,Fake& b) {
    CHECK(!s.read() && !s.retire());b.untouched();
    CHECK(s.snapshot().records==1 && s.snapshot().charged_bytes==4096);
    CHECK(s.snapshot().private_held && s.snapshot().source_lease);
}
static void quarantined(Session& s,Fake& b) {
    CHECK(s.snapshot().state==State::quarantined);CHECK(!s.poll());retained(s,b);
}
static void coverage_cases() {
    {Witness w;Session s;Fake b;
     for(unsigned n=0;n<static_cast<unsigned>(Coverage::count);++n)w.coverage(static_cast<Coverage>(n),true);
     w.reset(true,identity());w.close(false);w.event(Event::clear_state);w.alias(99);
     CHECK(!w.before_execute(admission().key,1,true));CHECK(!w.after_signal(1,true));
     CHECK(!WitnessFixtureAccess::attach(w,s,admission().key));
     const auto snap=w.snapshot();CHECK(!snap.enabled && !snap.generation && !snap.coverage && !snap.tracked);
     CHECK(!F::arm(s,false,b,admission()));refused(w,s,b);}
    for(unsigned n=0;n<static_cast<unsigned>(Coverage::count);++n) {
        Witness w;Session s;Fake b;install(w,static_cast<int>(n));w.reset(true,identity());
        refused(w,s,b); // Every missing hook bit independently refuses.
        w.coverage(static_cast<Coverage>(n),true);refused(w,s,b); // too late for this epoch
        w.reset(true,identity());CHECK(w.insertion_allowed(identity()));
        w.coverage(static_cast<Coverage>(n),false);refused(w,s,b); // unsupported/lost hook
    }
    {Witness w;Session s;Fake b;install(w,static_cast<int>(Coverage::alias7));w.reset(true,identity());
     w.alias(7);CHECK(w.snapshot().ambiguous);refused(w,s,b);}
    {Witness w;Session s;Fake b;ready(w);w.alias(11);refused(w,s,b);}
    {Witness w;Session s;Fake b;install(w);auto i=identity();i.direct=false;w.reset(true,i);refused(w,s,b);}
    for(unsigned n=0;n<8;++n) {
        Witness w;Session s;Fake b;ready(w);auto i=identity();
        switch(n) {
        case 0:++i.device;break;case 1:++i.queue;break;case 2:++i.fence;break;
        case 3:++i.list;break;case 4:++i.allocator;break;case 5:++i.creation;break;
        case 6:++i.node;break;case 7:i.direct=false;break;
        }
        CHECK(!w.insertion_allowed(i));CHECK(!b.insertions && !s.snapshot().records);b.untouched();
    }
    const Event events[]={Event::bundle,Event::indirect,Event::predication_on,Event::begin_query,
        Event::resolve_query,Event::begin_pass,Event::suspended_pass,Event::resumed_pass,
        Event::cross_list_pass,Event::protected_change};
    for(const auto e:events) {
        Witness w;Session s;Fake b;ready(w);w.event(e);w.event(Event::clear_state);refused(w,s,b);
        if(e==Event::protected_change)CHECK(!(w.snapshot().coverage & coverage_bit(Coverage::protected_session)));
    }
    {Witness w;Session s;Fake b;ready(w);w.event(Event::begin_query);w.event(Event::end_query);refused(w,s,b);}
    {Witness w;Session s;Fake b;ready(w);w.event(Event::begin_pass);w.event(Event::suspended_pass);
     w.reset(true,identity());refused(w,s,b);}
}
static void generation_cases() {
    for(unsigned n=0;n<2;++n) {
        Witness w;Session s;Fake b;ready(w);
        if(n==0)w.reset(false,identity());else w.close(false);
        CHECK(w.snapshot().generation==1);refused(w,s,b);
    }
    {Witness w;Session s;Fake b;ready(w);w.event(Event::predication_on);
     refused(w,s,b);w.event(Event::predication_off);CHECK(w.insertion_allowed(identity()));
     w.event(Event::begin_pass);refused(w,s,b);w.event(Event::end_pass);
     CHECK(w.insertion_allowed(identity()));w.event(Event::clear_state);
     CHECK(w.snapshot().generation==1 && w.insertion_allowed(identity()));}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b,false);w.event(Event::clear_state);
     CHECK(w.snapshot().generation==1);CHECK(!s.snapshot().recording_closed);retained(s,b);
     w.close(true);CHECK(!s.snapshot().recording_closed);
     const auto ticket=w.before_execute(admission().key,1,true);CHECK(ticket==1);
     CHECK(!s.poll());retained(s,b);CHECK(w.after_signal(ticket,true));
     b.sample.value=0;w.reset(true,identity());CHECK(w.snapshot().generation==2);
     CHECK(s.snapshot().recording_closed);CHECK(!s.poll());retained(s,b);
     b.sample.value=1;CHECK(s.poll());CHECK(s.read());CHECK(s.retire());
     CHECK(b.maps==1 && b.drops==1 && b.releases==1);CHECK(s.snapshot().charged_bytes==4096);}
    for(unsigned n=0;n<2;++n) {
        Witness w;Session s;Fake b;ready(w);prepared(w,s,b,false);
        if(n==0)w.reset(false,identity());else w.close(false);
        CHECK(w.snapshot().generation==1 && w.snapshot().ambiguous);quarantined(s,b);
    }
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true));w.reset(true,identity());
     // Reset racing the pending join cannot supply trusted closure.
     CHECK(w.snapshot().generation==2);quarantined(s,b);CHECK(!w.after_signal(1,true));}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     w.reset(true,identity());CHECK(s.snapshot().recording_closed);retained(s,b);
     CHECK(!w.before_execute(admission().key,1,true));quarantined(s,b);}
    // J-3b5 (live run 4): joined, Reset, then LATER generations recorded,
    // excluded and executed before completion. The old transaction is detached.
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true)==1&&w.after_signal(1,true));
     b.sample.value=0;
     for(unsigned f=0;f<4;++f) {
         auto i=identity();i.allocator+=f%2;w.reset(true,i);
         CHECK(w.snapshot().detached&&s.snapshot().recording_closed&&s.snapshot().state!=State::quarantined);
         w.event(Event::begin_query);w.event(Event::end_query);w.close(true);
         CHECK(!w.before_execute(admission().key,1,true)); // later generation: no join, no fault
         CHECK(!s.poll()&&s.snapshot().state!=State::quarantined);
     }
     w.close(true); // a malformed later Close is a list fault only (post-closure)
     const auto snap=w.snapshot();
     CHECK(!snap.fault&&snap.later_executes==4&&snap.post_closure_faults==1&&snap.recorded_generation==1);
     b.sample.value=1;CHECK(s.poll()&&s.read()&&s.retire()&&!s.snapshot().fault);}
    // ...but a later generation executed while open still quarantines.
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true)==1&&w.after_signal(1,true));
     w.reset(true,identity());CHECK(!w.before_execute(admission().key,1,true));quarantined(s,b);
     CHECK(std::string_view(s.snapshot().fault)=="later-execute-unclosed");}
    // Reset BEFORE the join completed never detaches (closure without submission).
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     w.reset(true,identity());w.close(true);CHECK(!w.snapshot().detached);
     CHECK(!w.before_execute(admission().key,1,true));quarantined(s,b);
     CHECK(std::string_view(w.snapshot().fault)=="execute-later-generation");}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     w.coverage(Coverage::reset,false);quarantined(s,b);}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b,false);
     w.event(Event::protected_change);quarantined(s,b);}
}
static void execute_cases() {
    // Missing/failed/ambiguous Close can never be repaired after Execute.
    for(unsigned n=0;n<3;++n) {
        Witness w;Session s;Fake b;ready(w);prepared(w,s,b,false);
        if(n==1)w.close(false);
        if(n==2){w.close(true);w.close(true);} // duplicate Close is ambiguous
        CHECK(!w.before_execute(admission().key,1,true));
        // Application Execute and Signal must still be forwarded exactly once;
        // a successful native Signal cannot authenticate a refused ticket.
        CHECK(!w.after_signal(1,true));
        w.close(true);w.reset(true,identity());b.sample.value=1;
        CHECK(!s.poll());CHECK(!s.snapshot().gpu_complete);
        quarantined(s,b); // no read, source-lease drop or private release
    }

    for(unsigned n=0;n<8;++n) {
        Witness w;Session s;Fake b;ready(w);prepared(w,s,b);auto k=admission().key;
        if(n==0){CHECK(!w.before_execute(k,2,true));}
        if(n==1){++k.queue;CHECK(!w.before_execute(k,1,true));}
        if(n==2){CHECK(!w.before_execute(k,1,false));}
        if(n==3){CHECK(w.before_execute(k,1,true));CHECK(!w.after_signal(1,false));}
        if(n==4){CHECK(w.before_execute(k,1,true));CHECK(!w.after_signal(2,true));}
        if(n==5){CHECK(w.before_execute(k,1,true)); // fake native callback, prior to Signal
            b.execute_callback(w,s,k);CHECK(!w.after_signal(1,true));}
        if(n==6){CHECK(w.before_execute(k,1,true));CHECK(w.after_signal(1,true));
            CHECK(!w.before_execute(k,1,true));}
        if(n==7){CHECK(w.before_execute(k,1,true));CHECK(w.after_signal(1,true));
            w.reset(true,identity());b.sample.value=(std::numeric_limits<uint64_t>::max)();CHECK(!s.poll());}
        quarantined(s,b);
    }
    // Actual overlapping callers race their registrations; neither native
    // callback runs under the witness lock. Any ordering permanently quarantines.
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);std::atomic<bool> go{false};
     uint64_t other=0;std::thread t([&]{while(!go.load())std::this_thread::yield();
         other=w.before_execute(admission().key,1,true);});
     go=true;const auto first=w.before_execute(admission().key,1,true);t.join();
     CHECK((first==1)!=(other==1));CHECK(!w.after_signal(1,true));quarantined(s,b);}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true));std::atomic<bool> go{false};
     std::thread t([&]{while(!go.load())std::this_thread::yield();
         CHECK(!w.before_execute(admission().key,1,true));});
     go=true;(void)w.after_signal(1,true);t.join();quarantined(s,b);}
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true));CHECK(w.after_signal(1,true));
     w.reset(true,identity());++b.sample.timeline.queue;CHECK(!s.poll());quarantined(s,b);}
}
static void allocator_rotation_cases() {
    // Part 2: a rotated (still authenticated) allocator advances the generation;
    // any other identity change on Reset stays permanent ambiguity.
    {Witness w;ready(w);auto i=identity();++i.allocator;w.reset(true,i);
     CHECK(w.snapshot().generation==2 && !w.snapshot().ambiguous && w.insertion_allowed(i));
     CHECK(!w.insertion_allowed(identity())); // the CURRENT allocator token is required
     Identity out{};uint64_t g=0;CHECK(w.candidate(out,g) && g==2 && out.allocator==i.allocator);}
    for(unsigned n=0;n<6;++n) {
        Witness w;ready(w);auto i=identity();
        switch(n){case 0:++i.device;break;case 1:++i.queue;break;case 2:++i.fence;break;
                  case 3:++i.list;break;case 4:++i.creation;break;case 5:++i.node;break;}
        w.reset(true,i);CHECK(w.snapshot().ambiguous);Identity out{};uint64_t g=0;CHECK(!w.candidate(out,g));
    }
    {Witness w;Session s;Fake b;ready(w);prepared(w,s,b);
     CHECK(w.before_execute(admission().key,1,true));CHECK(w.after_signal(1,true));
     auto i=identity();++i.allocator;w.reset(true,i); // closure through a rotated allocator
     CHECK(s.snapshot().recording_closed && s.poll() && s.read() && s.retire());
     Identity out{};uint64_t g=0;CHECK(!w.candidate(out,g)); // one transaction per lifetime
     CHECK(w.snapshot().closed_generation==0 && w.snapshot().generation==2);}
}
int main() {
    coverage_cases();generation_cases();execute_cases();allocator_rotation_cases();
    std::puts("GPU witnesses: fake backend only; no native copy or runtime parity");return 0;
}
