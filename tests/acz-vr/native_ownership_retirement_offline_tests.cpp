// Synthetic CPU/injected-backend tests only. No COM adapter, GPU or runtime proof.
#include "native_ownership_retirement.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <utility>
static bool fail_new=false;
void* operator new(std::size_t n) {
    if(fail_new)throw std::bad_alloc();
    if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace nr=acz::native_retirement;
namespace tx=acz::gpu_transaction;
static nr::Dependency dep(uint64_t h){return {h,h*10,h*100};}
struct FakeCom final:nr::ComBackend {
    unsigned refs[16]{},adds=0,releases=0;
    unsigned fail_add=0,fail_release=0;
    std::function<void(uint64_t)> on_add,on_release;
    bool add_ref(uint64_t h) noexcept override {
        ++adds;CHECK(h&&h<=16);
        if(adds==fail_add)return false;
        ++refs[h-1];auto action=std::move(on_add);if(action)action(h);return true;
    }
    bool release(uint64_t h) noexcept override {
        ++releases;CHECK(h&&h<=16&&refs[h-1]);
        const bool ok=releases!=fail_release;if(ok)--refs[h-1];
        auto action=std::move(on_release);if(action)action(h);return ok;
    }
};
struct FakeFence final:nr::FenceBackend {
    uint64_t handle=3,epoch=300;unsigned reads=0;
    nr::FenceObservation data{};
    std::function<void()> on_read;
    nr::FenceObservation read(uint64_t h,uint64_t e) noexcept override {
        ++reads;const auto result=(h==handle&&e==epoch)?data:nr::FenceObservation{};
        auto action=std::move(on_read);if(action)action();return result;
    }
};
struct Scenario {
    FakeCom com;FakeFence fence;nr::QuarantineKeeper keeper;
    tx::Admission admission;tx::Transaction transaction{admission};
    nr::OwnershipAdapter ownership{com,fence,keeper};
    tx::Signal signal{30,300,10};
    explicit Scenario(uint64_t capture=1,bool missing_lease=false) {
        // Selected game list lifetime 7 is distinct from private execution list 20.
        tx::Manifest m;m.intent=tx::Intent::Copy;m.capture={capture,6,7,8,9,10,11};
        m.source_version_before=m.source_version_after=2;m.device_id=3;m.queue_id=4;m.queue_epoch=5;
        m.source_bytes=m.aggregate_bytes=64;
        m.capture_identity_known=m.source_version_equivalent=m.ownership_established=true;
        m.source_state_known=m.restoration_established=m.category_known=m.bounds_known=true;
        const tx::Lease leases[]={{10},{20},{missing_lease?99u:30u}};
        const tx::Allocation allocation{1,tx::Category::SourceCopy,64,0};
        const tx::Operation operation{tx::Category::SourceCopy,0,64,0,0};
        CHECK(transaction.prepare({3,1,1,tx::max_source_bytes,tx::max_allocation_bytes},m,
            leases,3,&allocation,1,&operation,1));
    }
    void acquire(){CHECK(ownership.acquire(dep(1),dep(2),dep(3),transaction.expected_identity()));}
    void submit(){
        CHECK(transaction.record()&&transaction.close()&&transaction.submit(tx::Commit::Submitted,signal));
        CHECK(ownership.handoff(signal));
        fence.data={dep(3),transaction.expected_identity(),0,true,dep(1),dep(2)};
    }
    void completed(){fence.data.completed=10;CHECK(transaction.complete(ownership.poll_completion()));}
};
static void normalAndPending(){
    Scenario s;s.acquire();s.submit();
    CHECK(s.ownership.phase()==nr::Phase::HandedOff);
    CHECK(!s.ownership.retire(s.transaction)&&s.com.releases==0);
    auto c=s.ownership.poll_completion();
    CHECK(c.status==tx::CompletionStatus::Pending&&c.observed.value==0);
    CHECK(!s.transaction.complete(c)&&s.transaction.reason()==tx::Reason::CompletionPending);
    s.fence.data.completed=9;c=s.ownership.poll_completion();
    CHECK(c.status==tx::CompletionStatus::Pending&&!s.transaction.complete(c));
    CHECK(s.transaction.reason()==tx::Reason::CompletionPending); // J-064, not CompletionBelow
    CHECK(!s.ownership.retire(s.transaction)&&s.ownership.held()==3&&s.com.releases==0);
    s.completed();const auto reads=s.fence.reads;
    CHECK(s.ownership.retire(s.transaction)&&s.fence.reads==reads+1);
    CHECK(s.transaction.phase()==tx::Phase::Retired&&s.ownership.phase()==nr::Phase::Retired);
    CHECK(s.ownership.held()==0&&s.com.releases==3&&!s.keeper.count());
    CHECK(!s.ownership.retire(s.transaction)&&!s.ownership.quarantine_all()&&s.com.releases==3);
    CHECK(s.ownership.phase()==nr::Phase::Retired);
}
static void badInputs(){
    for(unsigned field=0;field<9;++field){
        Scenario s;nr::Dependency d[]={dep(1),dep(2),dep(3)};
        auto& target=d[field/3];if(field%3==0)target.handle=0;else if(field%3==1)target.identity=0;else target.epoch=0;
        CHECK(!s.ownership.acquire(d[0],d[1],d[2],s.transaction.expected_identity()));
        CHECK(s.com.adds==0&&s.ownership.phase()==nr::Phase::Failed);
    }
    for(unsigned failure=1;failure<=3;++failure){
        Scenario s;s.com.fail_add=failure;
        CHECK(!s.ownership.acquire(dep(1),dep(2),dep(3),s.transaction.expected_identity()));
        CHECK(s.com.releases==failure-1&&!s.ownership.held()&&!s.keeper.count());
        CHECK(!s.ownership.acquire(dep(1),dep(2),dep(3),s.transaction.expected_identity()));
    }
    {Scenario s;auto origin=s.transaction.expected_identity();origin.capture.list_lifetime=0;
     CHECK(!s.ownership.acquire(dep(1),dep(2),dep(3),origin)&&!s.com.adds);}
    for(unsigned i=0;i<6;++i){
        Scenario s;s.acquire();auto signal=s.signal;
        switch(i){case 0:signal.fence=0;break;case 1:signal.epoch=0;break;case 2:signal.value=0;break;
        case 3:signal.value=UINT64_MAX;break;case 4:signal.fence=3;break;case 5:signal.epoch=301;break;}
        CHECK(!s.ownership.handoff(signal)&&s.ownership.phase()==nr::Phase::Quarantined);
        CHECK(s.keeper.count()==3&&!s.com.releases&&!s.ownership.retire(s.transaction));
    }
}
static void completionCorrelation(){
    for(unsigned field=0;field<23;++field){
        Scenario s;s.acquire();s.submit();s.fence.data.completed=10;
        auto& o=s.fence.data;auto& t=o.transaction;
        switch(field){
        case 0:++o.fence.handle;break;case 1:++o.fence.identity;break;case 2:++o.fence.epoch;break;
        case 3:++t.capture.capture;break;case 4:++t.capture.device_epoch;break;case 5:++t.capture.list_lifetime;break;
        case 6:++t.capture.reset_generation;break;case 7:++t.capture.reset_revision;break;
        case 8:++t.capture.draw_ordinal;break;case 9:++t.capture.submission_serial;break;
        case 10:++t.version;break;case 11:++t.device;break;case 12:++t.queue;break;
        case 13:++t.queue_epoch;break;case 14:++t.fence;break;case 15:++t.fence_epoch;break;case 16:++t.value;break;
        case 17:++o.allocator.handle;break;case 18:++o.allocator.identity;break;case 19:++o.allocator.epoch;break;
        case 20:++o.execution_list.handle;break;case 21:++o.execution_list.identity;break;case 22:++o.execution_list.epoch;break;
        }
        const auto c=s.ownership.poll_completion();
        CHECK(c.status==tx::CompletionStatus::Invalid&&!c.independently_confirmed);
        CHECK(s.ownership.phase()==nr::Phase::Quarantined&&!s.com.releases&&s.keeper.count()==3);
    }
    for(unsigned fault=0;fault<4;++fault){
        Scenario s;s.acquire();s.submit();s.fence.data.completed=10;
        if(fault==0)++s.fence.handle;else if(fault==1)++s.fence.epoch;
        else if(fault==2)s.fence.data.valid=false;else s.fence.data.completed=UINT64_MAX;
        const auto c=s.ownership.poll_completion();
        CHECK(c.status==tx::CompletionStatus::DeviceLost&&!c.independently_confirmed);
        CHECK(!s.transaction.complete(c)&&s.transaction.reason()==tx::Reason::CompletionDeviceLost);
        CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases);
    }
    {Scenario s;s.acquire();s.submit();s.fence.data.completed=7;(void)s.ownership.poll_completion();
     s.fence.data.completed=6;CHECK(s.ownership.poll_completion().status==tx::CompletionStatus::Invalid);
     CHECK(s.ownership.reason()==nr::FailReason::Regressed&&!s.com.releases);}
    {Scenario s;s.acquire();s.submit();s.fence.data.completed=11;(void)s.ownership.poll_completion();
     s.fence.data.completed=10;CHECK(s.ownership.poll_completion().status==tx::CompletionStatus::Invalid);
     CHECK(s.ownership.reason()==nr::FailReason::Regressed&&!s.com.releases);}
    {Scenario s;s.acquire();s.submit();
     // A forged caller assertion can complete the CPU policy, but cannot release
     // ownership while the independent backend still reports pending zero.
     CHECK(s.transaction.complete({s.transaction.expected_identity(),tx::CompletionStatus::Confirmed,true}));
     CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases&&s.ownership.held()==3);
     CHECK(s.ownership.phase()==nr::Phase::HandedOff&&s.ownership.reason()==nr::FailReason::Incomplete);
     s.fence.data.completed=10;CHECK(s.ownership.retire(s.transaction));}
    {Scenario s;s.acquire();s.submit();s.completed();s.fence.data.completed=11;
     CHECK(!s.ownership.retire(s.transaction)&&s.keeper.count()==3&&!s.com.releases);}
    {Scenario s;s.acquire();s.submit();s.fence.data.completed=11;
     CHECK(!s.transaction.complete(s.ownership.poll_completion())&&s.transaction.reason()==tx::Reason::CompletionAbove);
     CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases);CHECK(s.ownership.quarantine_all());}
    {Scenario s;s.acquire();s.submit();s.completed();s.fence.data.completed=UINT64_MAX;
     CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases&&s.keeper.count()==3);}
    {Scenario s,other(999);s.acquire();s.submit();s.completed();other.acquire();other.submit();other.completed();
     CHECK(!s.ownership.retire(other.transaction)&&!s.com.releases&&s.keeper.count()==3);}
    {Scenario s(1,true);s.acquire();s.submit();s.completed();
     CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases&&s.keeper.count()==3);}
}
static void addReentryAndSnapshots(){
    for(unsigned action=0;action<4;++action){
        Scenario s;auto origin=s.transaction.expected_identity();
        s.com.on_add=[&](uint64_t){
            CHECK(s.ownership.phase()==nr::Phase::Acquiring);
            if(action==0)CHECK(!s.ownership.acquire(dep(4),dep(5),dep(6),origin));
            if(action==1)CHECK(!s.ownership.handoff(s.signal));
            if(action==2)CHECK(!s.ownership.retire(s.transaction));
            if(action==3)CHECK(!s.ownership.quarantine_all());
        };
        CHECK(!s.ownership.acquire(dep(1),dep(2),dep(3),origin));
        CHECK(s.com.adds==1&&!s.com.releases&&s.keeper.count()==1&&s.keeper.contains(1));
        CHECK(s.ownership.phase()==nr::Phase::Quarantined);
    }
    {Scenario s;auto a=dep(1),l=dep(2),f=dep(3);auto origin=s.transaction.expected_identity();
     s.com.on_add=[&](uint64_t){a=dep(4);l=dep(5);f=dep(6);origin.capture.capture=999;};
     CHECK(s.ownership.acquire(a,l,f,origin));
     CHECK(s.ownership.allocator().handle==1&&s.ownership.list().handle==2&&s.ownership.fence().handle==3);
     CHECK(!s.com.refs[3]&&!s.com.refs[4]&&!s.com.refs[5]);s.submit();s.completed();CHECK(s.ownership.retire(s.transaction));}
}
static void pollAndReleaseReentry(){
    for(unsigned action=0;action<4;++action){
        Scenario s;s.acquire();s.submit();s.fence.data.completed=10;
        s.fence.on_read=[&]{
            CHECK(s.ownership.phase()==nr::Phase::Polling);
            if(action==0)CHECK(!s.ownership.retire(s.transaction));
            if(action==1)CHECK(!s.ownership.quarantine_all());
            if(action==2)CHECK(s.ownership.poll_completion().status==tx::CompletionStatus::Invalid);
            if(action==3)CHECK(!s.ownership.handoff(s.signal));
        };
        CHECK(!s.ownership.poll_completion().independently_confirmed);
        CHECK(s.ownership.phase()==nr::Phase::Quarantined&&!s.com.releases&&s.keeper.count()==3);
    }
    for(unsigned action=0;action<3;++action){
        Scenario s;s.acquire();s.submit();s.completed();
        s.com.on_release=[&](uint64_t h){
            CHECK(h==3&&s.ownership.phase()==nr::Phase::Releasing&&!s.ownership.held());
            if(action==0)CHECK(!s.ownership.retire(s.transaction));
            if(action==1)CHECK(!s.ownership.quarantine_all());
            if(action==2)CHECK(!s.transaction.fail_after_commit());
        };
        CHECK(!s.ownership.retire(s.transaction));
        CHECK(s.com.releases==1&&s.com.refs[0]==1&&s.com.refs[1]==1&&!s.com.refs[2]);
        CHECK(s.keeper.count()==2&&s.keeper.contains(1)&&s.keeper.contains(2)&&!s.keeper.contains(3));
        CHECK(!s.ownership.retire(s.transaction)&&!s.ownership.quarantine_all()&&s.com.releases==1);
    }
    {Scenario s;s.acquire();s.submit();s.completed();
     s.fence.on_read=[&]{CHECK(!s.transaction.fail_after_commit());};
     CHECK(!s.ownership.retire(s.transaction)&&!s.com.releases&&s.keeper.count()==3);}
    {Scenario s;s.acquire();s.submit();s.completed();s.com.fail_release=1;
     CHECK(!s.ownership.retire(s.transaction)&&s.keeper.count()==3&&s.com.releases==1);}
    {Scenario s;s.com.fail_add=3;s.com.on_release=[&](uint64_t){CHECK(!s.ownership.quarantine_all());};
     CHECK(!s.ownership.acquire(dep(1),dep(2),dep(3),s.transaction.expected_identity()));
     CHECK(s.com.releases==1&&s.keeper.count()==1&&s.keeper.contains(1));}
}
static void capacityAndDestructor(){
    FakeCom com;FakeFence fence;nr::QuarantineKeeper keeper;Scenario origin;
    for(unsigned i=0;i<nr::max_quarantined/3;++i){
        nr::OwnershipAdapter a(com,fence,keeper);
        CHECK(a.acquire(dep(1),dep(2),dep(3),origin.transaction.expected_identity()));
    }
    CHECK(keeper.count()==255&&keeper.available()==1&&!com.releases);
    const auto calls=com.adds;
    {nr::OwnershipAdapter a(com,fence,keeper);
     CHECK(!a.acquire(dep(1),dep(2),dep(3),origin.transaction.expected_identity()));
     CHECK(a.reason()==nr::FailReason::QuarantineCapacity&&com.adds==calls);}
    CHECK(keeper.count()==255&&!com.releases);
    {Scenario s;fail_new=true;s.acquire();s.submit();s.completed();CHECK(s.ownership.retire(s.transaction));fail_new=false;}
}
int main(){
    normalAndPending();badInputs();completionCorrelation();addReentryAndSnapshots();
    pollAndReleaseReentry();capacityAndDestructor();
    std::printf("%u ownership/retirement checks (offline injected backends only)\n",checks);
}
