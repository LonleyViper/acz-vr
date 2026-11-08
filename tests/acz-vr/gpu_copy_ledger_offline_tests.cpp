#include "gpu_copy_ledger_fixture.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
using namespace acz::gpu_copy;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using F=FixtureAccess;
struct Fake final:Backend {
    Session* owner=nullptr;
    FenceSample sample{fixture_admission().key,1,true};
    unsigned polls=0,maps=0,logical_drops=0,private_releases=0;
    unsigned permanent_upload_refs=1;
    void (*on_poll)(Fake&)=nullptr;
    void (*on_read)(Fake&)=nullptr;
    void (*on_release)(Fake&)=nullptr;
    FenceSample poll() noexcept override {++polls;if(on_poll)on_poll(*this);return sample;}
    bool read() noexcept override {++maps;if(on_read)on_read(*this);return true;}
    void drop_logical_source_lease(uint64_t token) noexcept override {
        CHECK(token==22);++logical_drops;if(on_release)on_release(*this);
    }
    void release_private(uint64_t token) noexcept override {
        CHECK(token==11);++private_releases;if(on_release)on_release(*this);
    }
    void untouched() {CHECK(maps==0 && logical_drops==0 && private_releases==0 && permanent_upload_refs==1);}
};
static void prepared(Session& s,Fake& b) {
    b.owner=&s;const auto a=fixture_admission();
    CHECK(!F::record(s));CHECK(s.snapshot().records==0);b.untouched();
    CHECK(F::arm(s,true,b,a));CHECK(F::reserve(s,a));CHECK(F::created(s));
    CHECK(F::record(s));CHECK(s.snapshot().state==State::recorded_ambiguous);
    CHECK(!s.read()&&!s.retire());b.untouched();
}
static void submitted(Session& s,Fake& b) {
    prepared(s,b);CHECK(F::execute(s,fixture_admission().key)==1);
    CHECK(s.snapshot().state==State::submitted);CHECK(F::signal(s));
    CHECK(s.snapshot().state==State::signalled);
}
static void quarantine(Session& s,Fake& b) {
    CHECK(s.snapshot().state==State::quarantined);
    CHECK(!s.poll()&&!s.read()&&!s.retire()&&!s.begin_global_drain());
    CHECK(!F::reserve(s,fixture_admission()));
    CHECK(s.snapshot().records==1 && s.snapshot().charged_bytes==4096);b.untouched();
}
static void admission_cases() {
    {Session s;Fake b;const auto a=fixture_admission();
     CHECK(!F::arm(s,false,b,a));CHECK(!F::reserve(s,a));CHECK(!F::created(s));
     CHECK(!F::record(s));CHECK(!s.poll()&&!s.read()&&!s.retire());
     CHECK(s.snapshot().records==0 && s.snapshot().charged_bytes==0);
     CHECK(!s.snapshot().private_held&&!s.snapshot().source_lease);b.untouched();}
    for(unsigned i=0;i<9;++i) {
        Session s;Fake b;auto a=fixture_admission();
        switch(i) {
        case 0:a.history_complete=false;break;
        case 1:a.retired_generation=true;break;
        case 2:a.private_fence=false;break;
        case 3:a.authenticated_queue=false;break;
        case 4:a.exclusive_signaller=false;break;
        case 5:a.allocation_bytes=Session::byte_cap+1;break;
        case 6:a.allocation_bytes=0;break;
        case 7:a.key.generation=0;break;
        case 8:a.key.device=0;break;
        }
        CHECK(!F::arm(s,true,b,a));CHECK(!F::reserve(s,a));
        CHECK(!F::record(s));CHECK(!s.snapshot().records);b.untouched();
    }
    {Session s;Fake b;auto a=fixture_admission();a.allocation_bytes=Session::byte_cap;
     CHECK(F::arm(s,true,b,a));auto stale=a;stale.key.generation++;
     CHECK(!F::reserve(s,stale));CHECK(!s.snapshot().records);
     CHECK(F::reserve(s,a));CHECK(s.snapshot().state==State::reserved);
     CHECK(!F::reserve(s,a));CHECK(s.snapshot().charged_bytes==Session::byte_cap);
     CHECK(F::created(s,false,11,22));CHECK(s.snapshot().private_held);
     CHECK(!s.begin_global_drain());b.untouched();}
    {Session s;Fake b;const auto a=fixture_admission();CHECK(F::arm(s,true,b,a));
     CHECK(F::reserve(s,a));s.stop();CHECK(F::created(s,true,11,22));quarantine(s,b);}
}
static void creation_receipt_cases() {
    // Rejection before reservation leaves supplied ownership with the caller.
    {Session s;Fake b;const auto a=fixture_admission();
     CHECK(!F::created(s,false,33,44));CHECK(F::owns(s,0,0));
     CHECK(F::arm(s,true,b,a));CHECK(!F::created(s,true,11,22));
     CHECK(F::owns(s,0,0));CHECK(s.snapshot().charged_bytes==0);
     CHECK(F::reserve(s,a));CHECK(F::created(s));CHECK(F::owns(s,11,22));
     CHECK(!F::created(s,true,33,44));CHECK(F::owns(s,11,22));
     CHECK(F::record(s));CHECK(!F::created(s,false,55,66));
     CHECK(F::owns(s,11,22));b.untouched();}
    // Every first receipt is accepted, including failed, partial and empty
    // outputs. Success with missing owners still quarantines; none may retry.
    for(unsigned i=0;i<8;++i) {
        Session s;Fake b;const auto a=fixture_admission();
        const bool ok=(i&4)!=0;
        const uint64_t output=(i&1)?11:0,source=(i&2)?22:0;
        CHECK(F::arm(s,true,b,a));CHECK(F::reserve(s,a));
        CHECK(F::created(s,ok,output,source));CHECK(F::owns(s,output,source));
        CHECK(s.snapshot().private_held==(output!=0));
        CHECK(s.snapshot().source_lease==(source!=0));
        CHECK(!F::created(s,true,33,44));CHECK(F::owns(s,output,source));
        if(ok && output && source) {CHECK(F::record(s));b.untouched();}
        else {CHECK(!F::record(s));quarantine(s,b);}
    }
    // An uncertainty event may win while creation is in flight. Late success
    // and ambiguous failure must both hand off owners into the tombstone once.
    for(unsigned i=0;i<2;++i) {
        const bool ok=i!=0;
        Session s;Fake b;const auto a=fixture_admission();
        CHECK(F::arm(s,true,b,a));CHECK(F::reserve(s,a));F::uncertain(s);
        quarantine(s,b);CHECK(F::owns(s,0,0));
        CHECK(F::created(s,ok,11,22));CHECK(F::owns(s,11,22));
        CHECK(s.snapshot().private_held && s.snapshot().source_lease);
        CHECK(!F::record(s));CHECK(!F::execute(s,a.key));CHECK(!F::close(s,a.key));
        quarantine(s,b);
        CHECK(!F::created(s,!ok,33,44));CHECK(F::owns(s,11,22));
        CHECK(s.snapshot().private_held && s.snapshot().source_lease);
        CHECK(!F::arm(s,true,b,a));CHECK(!F::record(s));
        quarantine(s,b);CHECK(!s.begin_global_drain());CHECK(b.polls==0);
    }
    // A completion cannot reopen even a successfully retired/drained session.
    {Session s;Fake b;submitted(s,b);CHECK(F::close(s,fixture_admission().key));
     CHECK(s.poll());CHECK(s.read());CHECK(s.retire());CHECK(s.begin_global_drain());
     CHECK(!F::created(s,true,33,44));CHECK(F::owns(s,0,0));
     CHECK(!s.snapshot().private_held && !s.snapshot().source_lease);
     CHECK(s.snapshot().retired && s.snapshot().charged_bytes==4096);
     CHECK(b.logical_drops==1 && b.private_releases==1 && b.maps==1);}
}
static void completion_cases() {
    Session s;Fake b;submitted(s,b);
    b.sample.value=0;CHECK(!s.poll());CHECK(!s.read());b.untouched();
    b.sample.value=1;CHECK(s.poll());CHECK(s.snapshot().state==State::completed);
    CHECK(!s.read()&&!s.retire());b.untouched(); // completed but no Reset closure
    CHECK(!s.begin_global_drain()); // closes experiment only
    CHECK(F::close(s,fixture_admission().key));CHECK(s.snapshot().state==State::closed);
    CHECK(s.read());CHECK(b.maps==1);CHECK(!s.read());
    b.on_release=[](Fake& f){CHECK(f.owner->snapshot().owner_busy);
        CHECK(!f.owner->begin_global_drain());CHECK(!f.owner->retire());f.owner->stop();};
    CHECK(s.retire());CHECK(!s.retire());CHECK(b.logical_drops==1&&b.private_releases==1);
    CHECK(b.permanent_upload_refs==1);CHECK(s.begin_global_drain());
    CHECK(!F::reserve(s,fixture_admission()));CHECK(s.snapshot().charged_bytes==4096);
}
static void refusal_cases() {
    {Session s;Fake b;prepared(s,b);CHECK(F::execute(s,fixture_admission().key));
     CHECK(!F::signal(s,1,false));quarantine(s,b);}
    {Session s;Fake b;prepared(s,b);CHECK(F::execute(s,fixture_admission().key));
     CHECK(!F::signal(s,(std::numeric_limits<uint64_t>::max)(),true));quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);b.sample.value=(std::numeric_limits<uint64_t>::max)();
     CHECK(!s.poll());quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);b.sample.valid=false;CHECK(!s.poll());quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);b.sample.timeline.fence++;CHECK(!s.poll());quarantine(s,b);}
    {Session s;Fake b;prepared(s,b);CHECK(!F::execute(s,fixture_admission().key,2));quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);CHECK(!F::execute(s,fixture_admission().key));quarantine(s,b);}
    {Session s;Fake b;prepared(s,b);auto k=fixture_admission().key;k.queue++;
     CHECK(!F::execute(s,k));quarantine(s,b);}
    {Session s;Fake b;prepared(s,b);CHECK(F::execute(s,fixture_admission().key));
     // Synchronous native Execute callback occurs before its Signal returns.
     CHECK(!F::execute(s,fixture_admission().key));CHECK(!F::signal(s));quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);F::uncertain(s); // timeout without Reset
     quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);b.sample.value=0;
     for(unsigned i=0;i<50;++i)CHECK(!s.poll());
     CHECK(b.polls==50);CHECK(!s.poll());quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);s.stop();CHECK(s.poll());
     CHECK(!s.read()&&!s.retire()&&!s.begin_global_drain());b.untouched();}
    {Session s;Fake b;submitted(s,b);b.on_poll=[](Fake& f){F::uncertain(*f.owner);};
     CHECK(!s.poll());quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);CHECK(s.poll());CHECK(F::close(s,fixture_admission().key));
     CHECK(s.read());b.on_release=[](Fake& f){F::uncertain(*f.owner);};
     CHECK(!s.retire());CHECK(b.logical_drops==1&&b.private_releases==0);
     CHECK(s.snapshot().private_held&&!s.begin_global_drain());
     CHECK(b.permanent_upload_refs==1);}
}
static void races() {
    // Both outcomes of the atomic drain/reservation ordering are legal, but
    // there can be no insertion/reference acquisition after drain wins.
    for(unsigned i=0;i<32;++i) {
        Session s;Fake b;const auto a=fixture_admission();CHECK(F::arm(s,true,b,a));
        std::atomic<bool> go{false};bool reserved=false;
        std::thread t([&]{while(!go.load())std::this_thread::yield();reserved=F::reserve(s,a);});
        go=true;const bool drained=s.begin_global_drain();t.join();
        CHECK(drained==!reserved);CHECK(!F::record(s));
        CHECK(s.snapshot().records==(reserved?1u:0u));b.untouched();
    }
    {Session s;Fake b;prepared(s,b);CHECK(F::execute(s,fixture_admission().key));
     std::thread other([&]{CHECK(!F::execute(s,fixture_admission().key));});other.join();
     CHECK(!F::signal(s));quarantine(s,b);}
    {Session s;Fake b;submitted(s,b);
     b.on_poll=[](Fake& f){CHECK(f.owner->snapshot().owner_busy);
         std::thread shutdown([&]{f.owner->stop();CHECK(!f.owner->begin_global_drain());
             CHECK(!f.owner->poll()&&!f.owner->read()&&!f.owner->retire());});shutdown.join();};
     CHECK(s.poll());CHECK(!s.read());b.untouched();}
    {Session s;Fake b;submitted(s,b);CHECK(F::close(s,fixture_admission().key));CHECK(s.poll());
     b.on_read=[](Fake& f){CHECK(f.owner->snapshot().owner_busy);
         std::thread shutdown([&]{f.owner->stop();CHECK(!f.owner->begin_global_drain());
             CHECK(!f.owner->read()&&!f.owner->retire());});shutdown.join();};
     CHECK(s.read());CHECK(b.private_releases==0&&b.logical_drops==0);
     CHECK(s.retire());CHECK(b.permanent_upload_refs==1);}
}
int main() {
    admission_cases();creation_receipt_cases();completion_cases();refusal_cases();races();
    std::puts("GPU copy ledger: synthetic owner/COM/fence refusal fixtures passed; no runtime parity");
}
