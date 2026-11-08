// Synthetic CPU facts only; no real synchronization or runtime VR claims.
#include "native_copy_provider.h"
#include "gpu_transaction_contract.h"
#include "draw_input_provenance_contract.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <thread>
#include <type_traits>

static int allocation_budget=-1;
void* operator new(std::size_t n) {
    if(allocation_budget==0) throw std::bad_alloc();
    if(allocation_budget>0) --allocation_budget;
    if(auto p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}} while(0)
namespace copy=acz::native_copy;
namespace input=acz::capture_input;
using namespace acz::capture;
using namespace acz::metadata;

struct Fake final : copy::Source {
    std::vector<uint8_t> bytes;
    copy::Stamp stamp{{7,12},1};
    std::function<void()> during_copy, during_inspect;
    unsigned inspections=0, copies=0;
    int count_adjust=0, throw_at=0;
    explicit Fake(size_t n):bytes(n) {
        for(size_t i=0;i<n;++i) bytes[i]=uint8_t(i*17+3);
    }
    copy::Stamp inspect(const copy::Request& r) override {
        CHECK(r.resource.id==1 && r.witness.list_identity);
        ++inspections;
        if(throw_at==1) throw std::bad_alloc();
        if(during_inspect) during_inspect();
        return stamp;
    }
    size_t copy(const copy::Request& r,uint8_t* out,size_t capacity) override {
        CHECK(r.offset==0 && capacity==bytes.size());++copies;
        if(throw_at==2) throw 42;
        std::copy(bytes.begin(),bytes.end(),out);
        if(during_copy) during_copy();
        return size_t(int64_t(capacity)+count_adjust);
    }
};
struct Fixture {
    acz::binding::NativeBindingObserver observer;
    State state;
    Fake source;
    copy::Request request;
    static constexpr uint64_t device=99,heap=4,cpu=0x1000,gpu=0x2000;
    explicit Fixture(size_t n=832):source(n) {
        observer.heap({heap,cpu,gpu,8,32,0,1,0},device);
        observer.resource({1,0x10000,n},device);write();
        RootLayout layout;
        RootParameter table;table.kind=ParameterKind::table;table.visibility=Visibility::geometry;
        table.ranges.push_back({BindingKind::srv,1,10,0,0,2});layout.parameters={table};
        state.tracking_complete=true;
        // Explicit complete SYNTHETIC manifests; acquire does not create them.
        state.pipeline={true,true,{{BindingKind::srv,10,0,ShaderStage::geometry}},{}};
        state.pipeline.identity=new_lifetime_identity();
        state.pipeline.explicit_root_identity=new_lifetime_identity();
        state.set_root(9,RootBindingMetadata::from_adapter(layout),state.pipeline.explicit_root_identity);
        state.set_heaps({heap});state.set_value(0,ParameterKind::table,gpu);
        refresh();
    }
    void write() {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements=2;d.Buffer.StructureByteStride=416;
        observer.srv(1,&d,cpu+64,device);
    }
    void refresh() {
        request={observer.resource_token(1),observer.table(state,0,heap,2,device),0,source.bytes.size(),1,{7,10},device};
        CHECK(request.witness);
    }
    copy::Result acquire() {return copy::acquire(observer,state,source,request);}
};

void ownedAndFreeze() {
    copy::Result owned;
    input::Result lease;
    std::vector<uint8_t> expected;
    std::shared_ptr<const Snapshot> frozen;
    std::weak_ptr<const std::vector<uint8_t>> weak;
    {
        Fixture f;expected=f.source.bytes;
        owned=f.acquire();CHECK(owned && owned.rejection.empty());
        CHECK(owned.before.version==1 && owned.after.completed.value==12);
        CHECK(owned.buffer.bytes->data()!=f.source.bytes.data());
        CHECK(*owned.buffer.bytes==expected);
        input::Request r;
        r.list_identity=f.state.list_identity;r.generation=f.state.generation;
        r.revision=f.state.revision;r.device=Fixture::device;
        r.buffers={owned.buffer};r.descriptors={{0,2,f.request.witness}};
        // Independent caller assertions; the provider result alone is insufficient.
        CHECK(!input::assemble(f.observer,f.state,r));
        r.serialized=r.synchronized=true;
        r.indexing_proven=r.vertex_fetch_proven=true;r.srt_indices={1};
        lease=input::assemble(f.observer,f.state,r);CHECK(lease);
        weak=owned.buffer.bytes;r={};owned={};CHECK(!weak.expired());
        std::fill(f.source.bytes.begin(),f.source.bytes.end(),0);++f.source.stamp.version;
        const auto* retained=static_cast<const uint8_t*>(lease.lease->inputs().resources[0].bytes);
        CHECK(retained && std::equal(expected.begin(),expected.end(),retained));
        Command command{false,3,1};
        auto result=input::freeze_owned(f.observer,f.state,command,*lease.lease);CHECK(result);
        frozen=result.snapshot;
        CHECK(frozen->cpu_data_complete && !frozen->gpu_replay_complete);
        CHECK(frozen->resources.at(1)==expected && frozen->srt_elements[0].size()==416);
        f.state.pipeline.requirements_proven=false;
        CHECK(!input::freeze_owned(f.observer,f.state,command,*lease.lease));
    } // Source/observer/state destroyed; lease owns storage, freeze owns another copy.
    CHECK(!weak.expired());
    CHECK(lease.lease->inputs().resources[0].byte_length==expected.size());
    lease={};CHECK(weak.expired());CHECK(frozen->resources.at(1)==expected);
}
template<class F> void reject(F change) {
    Fixture f;change(f);auto result=f.acquire();
    CHECK(!result && !result.buffer.bytes && !result.buffer.witness.snapshot);
    CHECK(!result.rejection.empty());
}
void rejections() {
    reject([](auto& f){f.request.resource.generation++;});
    reject([](auto& f){f.request.witness={};});
    reject([](auto& f){f.request.witness.device++;});
    reject([](auto& f){f.request.device++;});
    reject([](auto& f){f.request.witness.generation=f.state.generation=0;});
    reject([](auto& f){f.request.required.identity=0;});
    reject([](auto& f){f.request.required.value=0;});
    reject([](auto& f){f.source.stamp.completed.identity=8;});
    reject([](auto& f){f.source.stamp.completed.value=9;});
    reject([](auto& f){f.source.stamp.version=0;});
    reject([](auto& f){f.request.stride=0;});
    reject([](auto& f){f.request.elements=0;});
    reject([](auto& f){f.request.elements=UINT64_MAX;f.request.stride=2;});
    reject([](auto& f){f.request.offset=UINT64_MAX;});
    reject([](auto& f){f.request.elements--;});
    reject([](auto& f){f.request.elements++;});
    reject([](auto& f){f.request.offset=1;f.request.elements--;});
    reject([](auto& f){f.source.count_adjust=-1;});
    reject([](auto& f){f.source.count_adjust=1;});
    reject([](auto& f){f.source.throw_at=1;});
    reject([](auto& f){f.source.throw_at=2;});
    reject([](auto& f){f.observer.disable();});
    // These events occur inside the injected source, between currentness checks.
    reject([](auto& f){f.source.during_copy=[&]{++f.state.revision;};});
    reject([](auto& f){f.source.during_copy=[&]{++f.state.generation;};});
    reject([](auto& f){f.source.during_copy=[&]{f.state.list_identity=new_lifetime_identity();};});
    reject([](auto& f){f.source.during_copy=[&]{f.state.tracking_complete=false;};});
    reject([](auto& f){f.source.during_copy=[&]{f.observer.remove_resource(f.request.resource);};});
    reject([](auto& f){f.source.during_copy=[&]{f.observer.remove_resource(f.request.resource);
        f.observer.resource({1,0x10000,f.source.bytes.size()},Fixture::device);f.write();
        CHECK(f.observer.resource_token(1)!=f.request.resource);};});
    reject([](auto& f){f.source.during_copy=[&]{f.observer.remove_heap(f.observer.heap_token(Fixture::heap));};});
    reject([](auto& f){f.source.during_copy=[&]{auto old=f.observer.heap_token(Fixture::heap);
        f.observer.remove_heap(old);f.observer.heap({Fixture::heap,Fixture::cpu,Fixture::gpu,8,32,0,1,0},Fixture::device);
        f.write();CHECK(f.observer.heap_token(Fixture::heap)!=old);};});
    reject([](auto& f){f.source.during_copy=[&]{f.write();};});
    reject([](auto& f){f.source.during_copy=[&]{++f.source.stamp.version;};});
    reject([](auto& f){f.source.during_copy=[&]{f.source.stamp.version=0;};});
    reject([](auto& f){f.source.during_copy=[&]{f.source.stamp.completed.value=9;};});
    reject([](auto& f){f.source.during_copy=[&]{f.source.stamp.completed.value=11;};});
    reject([](auto& f){f.source.during_copy=[&]{f.source.stamp.completed.identity=8;};});
    reject([](auto& f){f.source.during_inspect=[&]{++f.state.revision;};});
    reject([](auto& f){f.source.during_inspect=[&]{if(f.source.inspections==2) f.write();};});
    reject([](auto& f){++f.state.revision;});
    reject([](auto& f){f.observer.remove_resource(f.request.resource);});
    reject([](auto& f){f.observer.remove_heap(f.observer.heap_token(Fixture::heap));});
    Fixture budget(max_copy_bytes+1);auto no=budget.acquire();
    CHECK(!no && !no.buffer.bytes && budget.source.copies==0);
    Fixture exact(max_copy_bytes);CHECK(exact.acquire());
    Fixture equal;equal.source.stamp.completed.value=10;CHECK(equal.acquire());
    equal.request.elements=2;equal.request.stride=416;CHECK(equal.acquire());
    equal.source.during_copy=[&]{++equal.source.stamp.completed.value;};CHECK(equal.acquire());
}
void allocations() {
    Fixture f;unsigned failures=0;bool success=false;
    for(int budget=0;budget<64;++budget) {
        allocation_budget=budget;auto result=f.acquire();allocation_budget=-1;
        if(!result) {CHECK(!result.buffer.bytes && !result.buffer.witness.snapshot);++failures;}
        else {CHECK(*result.buffer.bytes==f.source.bytes);success=true;break;}
    }
    CHECK(success && failures>=2);CHECK(f.acquire());
}
void ownedFakeFencedCopy() {
    namespace dp=acz::draw_provenance;
    auto pattern=[] {
        std::vector<uint8_t> bytes(832);
        for(size_t i=0;i<bytes.size();++i) bytes[i]=uint8_t((i*29+11)&255);
        return bytes;
    };
    auto restoration=[] {
        return std::vector<copy::FakeSubresource>{
            {0,dp::State::Common,dp::State::CopySource,dp::State::Common,true,true,true,true,true,true},
            {1,dp::State::ShaderResource,dp::State::CopySource,dp::State::ShaderResource,true,true,true,true,true,true}
        };
    };
    copy::Result retained;
    const auto expected_bytes=pattern();
    {
        Fixture f;
        auto writer_bytes=expected_bytes;
        copy::OwnedFakeAttempt attempt(expected_bytes.size(),2);
        CHECK(attempt.write(0,writer_bytes.data(),writer_bytes.size()));
        std::fill(writer_bytes.begin(),writer_bytes.end(),0);
        CHECK(attempt.version()==2 && attempt.seal());
        CHECK(!attempt.write(0,expected_bytes.data(),1));
        CHECK(!attempt.retire());
        const copy::FakeIdentity id{Fixture::device,f.state.list_identity,
            f.state.generation,f.state.revision,77,2,3};
        attempt.submit({id,f.request.resource,expected_bytes.size(),{7,10},attempt.version(),true,false});
        attempt.complete({id,{7,10},copy::FakeCompletionStatus::Confirmed,true});
        auto states=restoration();
        auto result=attempt.finish(f.observer,f.state,f.request,id,states.data(),states.size());
        CHECK(result && result.rejection.empty() && attempt.copies()==1);
        CHECK(*result.buffer.bytes==expected_bytes);
        CHECK(result.buffer.bytes->data()!=expected_bytes.data());
        CHECK(!attempt.native_proof_available() &&
              attempt.unknown_mask()==acz::native_evidence::permanent_unknown_mask);
        CHECK(attempt.retire() && attempt.retired() && attempt.retained_bytes()==0);
        CHECK(attempt.retained_submission().identity.queue==0);
        CHECK(*result.buffer.bytes==expected_bytes && !attempt.retire());
        retained=std::move(result);
    }
    CHECK(retained && *retained.buffer.bytes==expected_bytes);
    // Each case gets a fresh source and fake identity. Every uncertain attempt
    // retains its bytes and cannot be retired or retried.
    for(int fault=0;fault<39;++fault) {
        Fixture f;
        const auto source_bytes=pattern();
        copy::OwnedFakeAttempt attempt(source_bytes.size(),2);
        CHECK(attempt.write(0,source_bytes.data(),source_bytes.size()));
        auto id=copy::FakeIdentity{Fixture::device,f.state.list_identity,
            f.state.generation,f.state.revision,77,2,3};
        auto submission=copy::FakeSubmission{id,f.request.resource,source_bytes.size(),
            {7,10},attempt.version(),true,false};
        auto completion=copy::FakeCompletion{id,{7,10},copy::FakeCompletionStatus::Confirmed,true};
        auto states=restoration();
        size_t state_count=states.size();
        if(fault!=0) CHECK(attempt.seal());
        if(fault==1) attempt.unobservable_writer();
        if(fault==2) submission.ambiguous=true;
        if(fault==3) submission.submitted=false;
        if(fault==4) ++submission.source_version;
        if(fault==5) ++submission.identity.queue;
        if(fault==6) ++submission.identity.queue_epoch;
        if(fault==7) ++submission.identity.list;
        if(fault==8) ++submission.identity.generation;
        if(fault==9) ++submission.identity.revision;
        if(fault==10) ++submission.signal.identity;
        if(fault==11) ++submission.signal.value;
        if(fault==37) ++submission.resource.generation;
        if(fault==38) ++submission.bytes;
        if(fault!=12) attempt.submit(submission); // Missing submission.
        if(fault>=5 && fault<=9) completion.identity=submission.identity;
        if(fault==10 || fault==11) completion.completed=submission.signal;
        if(fault==13) completion.status=copy::FakeCompletionStatus::Pending;
        if(fault==14) completion.status=copy::FakeCompletionStatus::Timeout;
        if(fault==15) completion.status=copy::FakeCompletionStatus::DeviceLost;
        if(fault==16) completion.status=copy::FakeCompletionStatus::Ambiguous;
        if(fault==17) completion.independently_confirmed=false;
        if(fault==18) ++completion.identity.queue;
        if(fault==19) ++completion.identity.list;
        if(fault==34) ++completion.completed.identity;
        if(fault==35) --completion.completed.value;
        if(fault==36) ++completion.completed.value;
        if(fault!=20) attempt.complete(completion); // Missing completion.
        if(fault==21) f.write(); // Descriptor changed after fake completion.
        if(fault==22) attempt.unobservable_writer(); // Unobserved mapped write.
        if(fault==23) state_count=1;
        if(fault==24) states[1].index=0;
        if(fault==25) states[0].original=dp::State::Unknown;
        if(fault==26) states[1].restored=dp::State::Common;
        if(fault==27) states[0].copy_transition_observed=false;
        if(fault==28) states[0].restoration_observed=false;
        if(fault==29) states[0].alias_excluded=false;
        if(fault==30) states[0].split_barrier_excluded=false;
        if(fault==31) states[0].implicit_transition_excluded=false;
        if(fault==32) f.observer.remove_resource(f.request.resource);
        if(fault==33) ++f.state.revision;
        auto result=attempt.finish(f.observer,f.state,f.request,id,states.data(),state_count);
        CHECK(!result && !result.buffer.bytes && attempt.copies()==0);
        CHECK(attempt.quarantined() && attempt.retained_bytes()==source_bytes.size());
        if(fault!=0 && fault!=1 && fault!=12)
            CHECK(attempt.retained_submission().identity.queue==submission.identity.queue);
        if(fault>=13 && fault<=19)
            CHECK(attempt.retained_completion().identity.list==completion.identity.list);
        CHECK(!attempt.retire() && !attempt.finish(f.observer,f.state,f.request,id,states.data(),state_count));
        CHECK(!attempt.native_proof_available() &&
              attempt.unknown_mask()==acz::native_evidence::permanent_unknown_mask);
    }
}
namespace tx=acz::gpu_transaction;
static_assert(tx::max_attempts==1 && tx::max_in_flight==1 && tx::max_leases==64 &&
              tx::max_allocations==128 && tx::max_operations==128 &&
              tx::max_source_bytes==16ull*1024*1024);
static_assert(std::is_nothrow_destructible_v<tx::Transaction>);
struct PolicyFixture {
    tx::Limits limits{64,128,128,tx::max_source_bytes,tx::max_allocation_bytes};
    tx::Manifest m{};
    tx::Lease leases[65]{};
    tx::Allocation allocations[129]{};
    tx::Operation ops[129]{};
    PolicyFixture() {
        m.intent=tx::Intent::Copy;m.capture={1,6,7,8,9,10,11};
        m.source_version_before=m.source_version_after=2;
        m.device_id=3;m.queue_id=4;m.queue_epoch=5;m.source_bytes=m.aggregate_bytes=64;
        m.capture_identity_known=m.source_version_equivalent=m.ownership_established=true;
        m.source_state_known=m.restoration_established=m.category_known=m.bounds_known=true;
        for(size_t i=0;i<65;++i) leases[i].identity=i+1;
        for(size_t i=0;i<129;++i) {
            allocations[i]={i+1,tx::Category::SourceCopy,64,0};
            ops[i]={tx::Category::SourceCopy,0,64,0,0};
        }
    }
    bool prepare(tx::Transaction& t,size_t nl=1,size_t no=1,
                 tx::Fault fault=tx::Fault::None,size_t na=1) const {
        return t.prepare(limits,m,leases,nl,allocations,na,ops,no,fault);
    }
};
static tx::Completion confirmed(const tx::Transaction& t) {
    return {t.expected_identity(),tx::CompletionStatus::Confirmed,true};
}
static void recorded(tx::Transaction& t) {
    CHECK(t.record());CHECK(!t.record());CHECK(t.close());CHECK(!t.close());
}
void gpuTransactionContract() {
    PolicyFixture f;
    auto reject=[&](const PolicyFixture& p,tx::Reason reason,size_t nl=1,size_t no=1) {
        tx::Admission d;tx::Transaction t(d);
        CHECK(!p.prepare(t,nl,no));CHECK(t.phase()==tx::Phase::Refused && t.reason()==reason);
        CHECK(t.held_leases()==0 && d.in_flight==0 && !p.prepare(t));
    };
    auto p=f;p.m.capture_identity_known=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.source_version_equivalent=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.ownership_established=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.source_state_known=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.restoration_established=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.category_known=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.bounds_known=false;reject(p,tx::Reason::MissingFact);
    p=f;p.m.capture.capture=0;reject(p,tx::Reason::MissingFact);
    for(int field=0;field<6;++field) {
        p=f;
        auto& c=p.m.capture;
        switch(field) {
        case 0:c.device_epoch=0;break;case 1:c.list_lifetime=0;break;
        case 2:c.reset_generation=0;break;case 3:c.reset_revision=0;break;
        case 4:c.draw_ordinal=0;break;case 5:c.submission_serial=0;break;
        }
        reject(p,tx::Reason::MissingFact);
    }
    p=f;p.m.queue_epoch=0;reject(p,tx::Reason::MissingFact);
    p=f;p.m.intent=tx::Intent::Unknown;reject(p,tx::Reason::MissingFact);
    p=f;++p.m.source_version_after;reject(p,tx::Reason::IdentityMismatch);
    p=f;++p.m.source_bytes;reject(p,tx::Reason::InvalidAggregate);
    p=f;++p.m.aggregate_bytes;reject(p,tx::Reason::InvalidAggregate);
    p=f;p.limits.leases=0;reject(p,tx::Reason::Capacity);
    p=f;p.limits.leases=65;reject(p,tx::Reason::Capacity);
    p=f;p.limits.allocations=129;reject(p,tx::Reason::Capacity);
    p=f;p.limits.operations=129;reject(p,tx::Reason::Capacity);
    p=f;p.limits.source_bytes=tx::max_source_bytes+1;reject(p,tx::Reason::Capacity);
    p=f;p.limits.allocation_bytes=tx::max_allocation_bytes+1;reject(p,tx::Reason::Capacity);
    p=f;p.limits.allocation_bytes=32;reject(p,tx::Reason::Capacity);
    p=f;p.limits={1,1,1,64,64};
    {tx::Admission d;tx::Transaction t(d);CHECK(p.prepare(t));t.teardown();}
    p.limits.source_bytes=63;reject(p,tx::Reason::InvalidAggregate);
    p=f;p.limits={1,1,1,64,63};reject(p,tx::Reason::Capacity);
    p=f;p.limits.leases=1;reject(p,tx::Reason::Capacity,2);
    p=f;p.limits.operations=1;reject(p,tx::Reason::Capacity,1,2);
    p=f;p.limits.allocations=1;
    {tx::Admission d;tx::Transaction t(d);CHECK(p.prepare(t));t.teardown();}
    reject(f,tx::Reason::Capacity,65);reject(f,tx::Reason::Capacity,1,129);
    {tx::Admission d;tx::Transaction t(d);
     CHECK(!f.prepare(t,1,1,tx::Fault::None,129));
     CHECK(t.reason()==tx::Reason::Capacity);}
    p=f;p.leases[1]=p.leases[0];reject(p,tx::Reason::IdentityMismatch,2);
    p=f;p.ops[0].category=tx::Category::Unknown;reject(p,tx::Reason::InvalidCategory);
    p=f;p.ops[0].category=tx::Category::ReplayInput;reject(p,tx::Reason::InvalidCategory);
    p=f;p.allocations[0].identity=0;reject(p,tx::Reason::MissingFact);
    p=f;p.allocations[1]=p.allocations[0];
    {tx::Admission d;tx::Transaction t(d);CHECK(!p.prepare(t,1,1,tx::Fault::None,2));
     CHECK(t.reason()==tx::Reason::IdentityMismatch);}
    p=f;p.allocations[0].role=tx::Category::Unknown;reject(p,tx::Reason::InvalidCategory);
    p=f;p.allocations[0].extent=0;reject(p,tx::Reason::InvalidBounds);
    p=f;p.allocations[0].lease_index=1;reject(p,tx::Reason::InvalidBounds);
    p=f;p.allocations[0].lease_index=1;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(!p.prepare(t,2));CHECK(t.reason()==tx::Reason::InvalidBounds);}
    p=f;p.ops[0].bytes=0;reject(p,tx::Reason::InvalidBounds);
    p=f;p.ops[0].lease_index=1;reject(p,tx::Reason::InvalidBounds);
    p=f;p.ops[0].allocation_index=1;reject(p,tx::Reason::InvalidBounds);
    p=f;p.ops[0].offset=UINT64_MAX;reject(p,tx::Reason::InvalidBounds);
    p=f;p.ops[0].offset=1;p.ops[0].bytes=UINT64_MAX;
    reject(p,tx::Reason::InvalidBounds);
    p=f;p.allocations[0].extent=tx::max_source_bytes;
    p.m.source_bytes=p.m.aggregate_bytes=tx::max_source_bytes;
    {tx::Admission d;tx::Transaction t(d);CHECK(p.prepare(t));t.teardown();}
    ++p.allocations[0].extent;++p.m.source_bytes;++p.m.aggregate_bytes;
    reject(p,tx::Reason::InvalidAggregate);
    p=f;p.allocations[0].extent=UINT64_MAX;
    p.m.source_bytes=p.m.aggregate_bytes=UINT64_MAX;reject(p,tx::Reason::InvalidAggregate);
    p=f;p.allocations[1].role=tx::Category::DescriptorCopy;
    p.ops[1]={tx::Category::DescriptorCopy,0,64,0,1};p.m.aggregate_bytes=128;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(p.prepare(t,1,2,tx::Fault::None,2));t.teardown();}
    p.allocations[1].extent=tx::max_allocation_bytes-64;
    p.m.aggregate_bytes=tx::max_allocation_bytes;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(p.prepare(t,1,2,tx::Fault::None,2));t.teardown();}
    ++p.allocations[1].extent;++p.m.aggregate_bytes;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(!p.prepare(t,1,2,tx::Fault::None,2));
     CHECK(t.reason()==tx::Reason::InvalidAggregate);}
    p.allocations[1].extent=UINT64_MAX;p.m.aggregate_bytes=UINT64_MAX;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(!p.prepare(t,1,2,tx::Fault::None,2));
     CHECK(t.reason()==tx::Reason::InvalidAggregate);}
    // Two allocations for one copy: the unused descriptor allocation is held
    // and charged independently of the operation count.
    p=f;p.allocations[1].role=tx::Category::DescriptorCopy;
    p.allocations[1].extent=128;p.m.aggregate_bytes=192;
    {tx::Admission d;tx::Transaction t(d);
     CHECK(p.prepare(t,1,1,tx::Fault::None,2));
     CHECK(t.held_allocations()==2 && t.operation_count()==1);
     CHECK(t.held_allocation(0).identity==1 &&
           t.held_allocation(0).role==tx::Category::SourceCopy &&
           t.held_allocation(0).extent==64 &&
           t.held_allocation(1).identity==2 &&
           t.held_allocation(1).role==tx::Category::DescriptorCopy &&
           t.held_allocation(1).extent==128);
     t.teardown();CHECK(t.held_allocations()==0);}
    // Reusing one allocation for two operations does not double count bytes.
    p=f;p.ops[1]={tx::Category::SourceCopy,32,32,0,0};
    {tx::Admission d;tx::Transaction t(d);
     CHECK(p.prepare(t,1,2));CHECK(t.held_allocations()==1 && t.operation_count()==2);
     t.teardown();}
    p=f;p.m.source_bytes=p.m.aggregate_bytes=tx::max_source_bytes;
    for(size_t i=0;i<128;++i){p.allocations[i].extent=tx::max_source_bytes/128;
        p.allocations[i].lease_index=i%64;p.ops[i]={tx::Category::SourceCopy,0,64,i%64,i};}
    {tx::Admission d;tx::Transaction t(d);
     CHECK(p.prepare(t,64,128,tx::Fault::None,128));
     t.teardown();for(size_t i=0;i<64;++i) CHECK(t.ledger(i).disposed==1);}
    for(auto fault:{tx::Fault::Prepare,tx::Fault::Record,tx::Fault::Close}) {
        tx::Admission d;tx::Transaction t(d);
        if(fault==tx::Fault::Prepare) CHECK(!f.prepare(t,1,1,fault));
        else {CHECK(f.prepare(t));if(fault==tx::Fault::Record) CHECK(!t.record(fault));
              else {CHECK(t.record());CHECK(!t.close(fault));}}
        CHECK(t.phase()==tx::Phase::Refused && t.ledger(0).disposed==1 && d.in_flight==0);
        t.teardown();CHECK(t.ledger(0).disposed==1 && t.ledger(0).retired==0);
    }
    {tx::Admission d;tx::Transaction t(d),second(d);CHECK(f.prepare(t));
     CHECK(d.in_flight==1 && d.attempts==1 && !f.prepare(second));
     CHECK(second.reason()==tx::Reason::Admission);recorded(t);
     CHECK(!t.submit(tx::Commit::DeniedBeforeCommit,{9,7,10}));
     CHECK(t.ledger(0).disposed==1 && d.in_flight==0 && !f.prepare(second));}
    {tx::Admission d;std::atomic<int> admitted{0};
     std::thread workers[8];
     for(auto& worker:workers) worker=std::thread([&] {
         tx::Transaction t(d);
         if(f.prepare(t)){++admitted;t.teardown();}
     });
     for(auto& worker:workers) worker.join();
     CHECK(admitted==1 && d.attempts==1 && d.in_flight==0);}
    {tx::Admission d;
     {tx::Transaction t(d);CHECK(f.prepare(t));}
     CHECK(d.in_flight==0 && d.attempts==1);} // destructor pre-commit disposal
    {tx::Admission d;
     {tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
      CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));}
     CHECK(d.in_flight==1 && d.attempts==1);} // destructor cannot retire
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(!t.submit(tx::Commit::AmbiguousAfterCommit,{9,7,10}));
     for(int i=0;i<3;++i){CHECK(!t.complete(confirmed(t)));t.teardown();CHECK(!t.retire());}
     CHECK(t.phase()==tx::Phase::Quarantined && t.held_leases()==1 && d.in_flight==1);
     CHECK(t.ledger(0).disposed==0 && t.ledger(0).retired==0);}
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(!t.submit(tx::Commit::Submitted,{0,7,10}));
     CHECK(t.phase()==tx::Phase::Quarantined && t.held_leases()==1);}
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(!t.submit(tx::Commit::Submitted,{9,7,UINT64_MAX}));
     CHECK(t.phase()==tx::Phase::Quarantined && t.held_leases()==1 &&
           t.held_allocations()==1 && d.in_flight==1);}
    {auto input=f;tx::Admission d;tx::Transaction t(d);
     CHECK(input.prepare(t));const auto frozen=t.expected_identity();
     input.allocations[0]={99,tx::Category::DescriptorCopy,999,0};
     input.m.capture={999,999,999,999,999,999,999};
     input.m.queue_id=999;input.m.source_version_before=999;
     CHECK(t.held_allocation(0).identity==1 &&
           t.held_allocation(0).role==tx::Category::SourceCopy &&
           t.held_allocation(0).extent==64);
     const auto captured=t.expected_identity().capture;
     CHECK(captured.capture==frozen.capture.capture &&
           captured.device_epoch==frozen.capture.device_epoch &&
           captured.list_lifetime==frozen.capture.list_lifetime &&
           captured.reset_generation==frozen.capture.reset_generation &&
           captured.reset_revision==frozen.capture.reset_revision &&
           captured.draw_ordinal==frozen.capture.draw_ordinal &&
           captured.submission_serial==frozen.capture.submission_serial &&
           t.expected_identity().queue==frozen.queue &&
           t.expected_identity().version==frozen.version);
     recorded(t);tx::Signal signal{9,7,10};
     CHECK(t.submit(tx::Commit::Submitted,signal));const auto submitted=t.expected_identity();
     signal.fence=999;signal.epoch=999;signal.value=999;
     CHECK(t.expected_identity().fence==submitted.fence &&
           t.expected_identity().fence_epoch==submitted.fence_epoch &&
           t.expected_identity().value==submitted.value);
     t.teardown();CHECK(t.phase()==tx::Phase::Quarantined);}
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
     CHECK(!t.fail_after_commit() && t.phase()==tx::Phase::Quarantined);
     for(int i=0;i<3;++i) {
         t.teardown();CHECK(!t.complete(confirmed(t)) && !t.retire() && !t.fail_after_commit());
     }
     CHECK(t.ledger(0).disposed==0 && t.ledger(0).retired==0 && d.in_flight==1);}
    for(int field=0;field<15;++field) {
        tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
        auto prepared=t.expected_identity();
        CHECK(prepared.capture.capture==f.m.capture.capture &&
              prepared.capture.device_epoch==f.m.capture.device_epoch &&
              prepared.version==f.m.source_version_before);
        CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
        CHECK(t.expected_identity().capture.capture==prepared.capture.capture &&
              t.expected_identity().queue_epoch==prepared.queue_epoch);
        auto result=confirmed(t);
        switch(field) {
        case 0:++result.observed.capture.capture;break;
        case 1:++result.observed.capture.device_epoch;break;
        case 2:++result.observed.capture.list_lifetime;break;
        case 3:++result.observed.capture.reset_generation;break;
        case 4:++result.observed.capture.reset_revision;break;
        case 5:++result.observed.capture.draw_ordinal;break;
        case 6:++result.observed.capture.submission_serial;break;
        case 7:++result.observed.version;break;case 8:++result.observed.device;break;
        case 9:++result.observed.queue;break;
        case 10:--result.observed.queue_epoch;break;
        case 11:++result.observed.fence;break;
        case 12:--result.observed.fence_epoch;break;
        case 13:--result.observed.value;break;
        case 14:++result.observed.value;break;
        }
        const auto expected_reason=field==10 || field==12 ? tx::Reason::CompletionEpoch :
            field==13 ? tx::Reason::CompletionBelow :
            field==14 ? tx::Reason::CompletionAbove : tx::Reason::CompletionIdentity;
        CHECK(!t.complete(result) && t.reason()==expected_reason);
        CHECK(t.held_leases()==1 && t.held_allocations()==1 && t.ledger(0).disposed==0);
        if(field!=13) {
            CHECK(t.phase()==tx::Phase::Quarantined && !t.complete(confirmed(t)));
            CHECK(!t.retire() && t.ledger(0).retired==0 && d.in_flight==1);
        } else {
            CHECK(t.complete(confirmed(t)));
            CHECK(t.outcome()==tx::Outcome::SyntheticCopyCompleted);
            CHECK(!t.complete(confirmed(t)) && t.retire() && !t.retire());
            CHECK(t.ledger(0).retired==1 && d.in_flight==0);
        }
    }
    // An above-value observation is irreversible even if a later poll reports
    // the expected value or an older value from a recycled fence.
    for(auto later:{std::uint64_t{10},std::uint64_t{9}}) {
        tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
        CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
        auto seen=confirmed(t);seen.observed.value=11;
        CHECK(!t.complete(seen) && t.reason()==tx::Reason::CompletionAbove);
        CHECK(t.completion_anomaly() && t.last_observed_value()==11);
        seen.observed.value=later;
        CHECK(!t.complete(seen) && !t.retire());
        t.teardown();CHECK(t.held_leases()==1 && t.held_allocations()==1 &&
                           t.ledger(0).retired==0 && d.in_flight==1);
    }
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
     auto seen=confirmed(t);seen.observed.value=9;
     CHECK(!t.complete(seen) && t.reason()==tx::Reason::CompletionBelow);
     CHECK(t.last_observed_value()==9 && !t.completion_anomaly());
     seen.observed.value=8;
     CHECK(!t.complete(seen) && t.reason()==tx::Reason::CompletionRegressed);
     CHECK(t.phase()==tx::Phase::Quarantined && t.completion_anomaly() &&
           t.last_observed_value()==9 && !t.retire());}
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
     auto seen=confirmed(t);seen.observed.value=UINT64_MAX;
     CHECK(!t.complete(seen) && t.reason()==tx::Reason::CompletionInvalid);
     CHECK(t.phase()==tx::Phase::Quarantined && t.held_leases()==1 &&
           t.held_allocations()==1 && !t.retire());}
    for(auto status:{tx::CompletionStatus::Timeout,tx::CompletionStatus::DeviceLost,
                     tx::CompletionStatus::Invalid}) {
        tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
        CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
        auto result=confirmed(t);result.status=status;
        CHECK(!t.complete(result) && t.phase()==tx::Phase::Quarantined);
        CHECK(t.reason()==(status==tx::CompletionStatus::Timeout ? tx::Reason::CompletionTimeout :
            status==tx::CompletionStatus::DeviceLost ? tx::Reason::CompletionDeviceLost :
            tx::Reason::CompletionInvalid));
        t.teardown();CHECK(t.held_leases()==1 && t.ledger(0).disposed==0 && d.in_flight==1);
    }
    {tx::Admission d;tx::Transaction t(d);CHECK(f.prepare(t));recorded(t);
     CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));
     auto result=confirmed(t);result.independently_confirmed=false;
     CHECK(!t.complete(result) && t.reason()==tx::Reason::CompletionUnconfirmed);
     result.status=tx::CompletionStatus::Pending;
     CHECK(!t.complete(result) && t.reason()==tx::Reason::CompletionPending);
     CHECK(t.complete(confirmed(t)) && t.retire());t.teardown();
     CHECK(t.phase()==tx::Phase::Retired);}
    p=f;p.m.intent=tx::Intent::Replay;p.ops[0].category=tx::Category::ReplayInput;
    p.allocations[0].role=tx::Category::ReplayInput;p.m.source_bytes=0;
    {tx::Admission d;tx::Transaction t(d);CHECK(p.prepare(t));recorded(t);
     CHECK(t.submit(tx::Commit::Submitted,{9,7,10}));CHECK(t.complete(confirmed(t)));
     CHECK(t.outcome()==tx::Outcome::SyntheticReplayCompleted);t.teardown();
     CHECK(t.phase()==tx::Phase::Quarantined && t.outcome()==tx::Outcome::None);
     CHECK(t.ledger(0).disposed==0 && t.ledger(0).retired==0);}
    tx::Admission d;tx::Transaction inert(d);allocation_budget=0;
    CHECK(f.prepare(inert));recorded(inert);
    CHECK(inert.submit(tx::Commit::Submitted,{9,7,10}));
    CHECK(inert.complete(confirmed(inert)) && inert.retire());
    CHECK(tx::diagnostic(inert.reason())[0]=='n');allocation_budget=-1;
    for(auto reason:{tx::Reason::None,tx::Reason::MissingFact,tx::Reason::Capacity,
                    tx::Reason::CompletionIdentity,tx::Reason::CompletionEpoch,
                    tx::Reason::CompletionTimeout,tx::Reason::TeardownAfterCommit})
        CHECK(std::strlen(tx::diagnostic(reason))<32);
    State proofState;Snapshot proofSnapshot;
    CHECK(!proofState.pipeline.requirements_proven &&
          !proofSnapshot.cpu_data_complete && !proofSnapshot.gpu_replay_complete);
}
namespace prov=acz::draw_provenance;
struct ProvenanceFixture {
    prov::Limits limits{64,128,128,64,128};
    prov::Profile expected{};
    prov::Snapshot observed{};
    ProvenanceFixture(bool indexed=false) {
        auto& id=expected.identity;
        id.capture=1;id.device=2;id.device_epoch=3;id.list_lifetime=4;
        id.reset_generation=5;id.reset_revision=6;id.draw_ordinal=7;
        id.submission_serial=8;id.queue=9;id.queue_epoch=10;
        id.pso=11;id.pso_lifetime=12;id.root=13;id.root_lifetime=14;
        id.input_layout=15;id.profile_revision=16;id.all_stage_profile=17;
        id.draw={true,indexed,9,1,0,0,0};observed.identity=id;
        expected.origin=observed.origin=prov::Origin::FixtureObservation;
        observed.copy.origin=prov::Origin::FixtureObservation;
        observed.copy.device=id.device;observed.copy.device_epoch=id.device_epoch;
        observed.copy.queue=id.queue;observed.copy.queue_epoch=id.queue_epoch;
        observed.copy.after_submission=9;observed.copy.copy_serial=10;
        observed.copy.ordered=true;
        expected.copy_after_submission=9;expected.copy_serial=10;
        auto bit=[](prov::Stage s){return 1u<<static_cast<unsigned>(s);};
        expected.used_stages=bit(prov::Stage::VS)|bit(prov::Stage::GS)|
            bit(prov::Stage::PS)|bit(prov::Stage::IA);
        expected.absent_stages=bit(prov::Stage::HS)|bit(prov::Stage::DS)|bit(prov::Stage::CS);
        observed.used_stages=expected.used_stages;observed.absent_stages=expected.absent_stages;
        observed.resource_count=indexed?4:3;
        for(std::size_t i=0;i<observed.resource_count;++i) {
            auto& r=observed.resources[i];r.origin=prov::Origin::FixtureObservation;
            r.id=100+i;r.lifetime=200+i;r.generation=300+i;
            r.device=id.device;r.device_epoch=id.device_epoch;
            r.bytes=i==0?416:i==1?576:i==2?1024:18;
            r.copy_bytes=r.bytes;
            r.draw_version=r.copy_version=400+i;
            r.draw_queue=r.copy_queue=id.queue;
            r.draw_queue_epoch=r.copy_queue_epoch=id.queue_epoch;
            r.draw_submission=id.submission_serial;r.copy_after_submission=9;
            r.history_serial=500+i;r.complete_history=true;r.writer_excluded=true;
            r.ordered=r.current_at_draw=r.current_at_copy=true;
        }
        observed.state_count=observed.resource_count;
        for(std::size_t i=0;i<observed.state_count;++i) {
            auto& s=observed.states[i];s.origin=prov::Origin::FixtureObservation;
            s.resource=static_cast<std::uint32_t>(i);
            s.original=s.restored=prov::State::Common;
            s.copy_read=prov::State::CopySource;
            s.covered=s.state_known=s.restoration_known=true;
        }
        auto add=[&](prov::Stage stage,prov::Kind kind,std::uint32_t reg,
                     std::uint32_t root,std::uint32_t resource) -> prov::Input& {
            auto& x=observed.inputs[observed.input_count++];
            x.origin=prov::Origin::FixtureObservation;
            x.key={stage,kind,reg,0,root,stage==prov::Stage::IA?reg:0};
            x.root_id=id.root;x.root_lifetime=id.root_lifetime;x.resource=resource;
            if(kind==prov::Kind::StructuredBuffer || kind==prov::Kind::Texture) {
                x.descriptor=prov::DescriptorKind::Dynamic;
                x.heap=20;x.heap_lifetime=21;x.heap_revision=22;
                x.heap_class=prov::HeapClass::CbvSrvUav;x.heap_capacity=64;
                x.table_base=0;x.table_extent=64;
                x.descriptor_slot=observed.input_count;
                x.descriptor_revision=23;x.descriptor_source=24;
            }
            return x;
        };
        for(auto stage:{prov::Stage::VS,prov::Stage::GS}) {
            auto& x=add(stage,prov::Kind::StructuredBuffer,10,0,0);
            x.bytes=416;x.stride=416;
        }
        for(auto stage:{prov::Stage::VS,prov::Stage::PS}) {
            auto& x=add(stage,prov::Kind::RootConstants,10,1,UINT32_MAX);
            x.word_count=2;x.words[0]=0x1234;x.words[1]=0x5678;x.bytes=8;
        }
        {auto& x=add(prov::Stage::PS,prov::Kind::Texture,0,2,2);
         x.texture_lifetime=202;x.texture_version=402;x.dimension=2;
         x.format=28;x.first_subresource=0;x.subresources=1;
         x.texture_view=31;x.texture_content=32;x.texture_layout=33;}
        {auto& x=add(prov::Stage::PS,prov::Kind::Sampler,0,3,UINT32_MAX);
         x.descriptor=prov::DescriptorKind::Static;
         x.sampler_identity=50;x.sampler_lifetime=51;x.sampler_revision=52;
         x.sampler_source=53;x.sampler_immutable=true;
         x.sampler_word_count=4;x.sampler_words[0]=1;x.sampler_words[1]=2;
         x.sampler_words[2]=3;x.sampler_words[3]=4;}
        {auto& x=add(prov::Stage::IA,prov::Kind::Vertex,0,4,1);
         x.bytes=576;x.stride=64;x.vertex_count=9;x.instance_count=1;}
        if(indexed) {
            auto& x=add(prov::Stage::IA,prov::Kind::Index,0,5,3);
            x.bytes=18;x.stride=2;x.index_format=prov::IndexFormat::UInt16;
            x.index_count=9;x.min_index=0;x.max_index=8;x.vertex_count=9;
            x.index_values_known=x.index_access_complete=true;
        }
        // Selected profile is retained independently of the observation inventory.
        expected.count=indexed?8:7;
        expected.required[0].key={prov::Stage::VS,prov::Kind::StructuredBuffer,10,0,0,0};
        expected.required[1].key={prov::Stage::GS,prov::Kind::StructuredBuffer,10,0,0,0};
        for(int i=0;i<2;++i) {
            auto& r=expected.required[i];r.resource_id=100;r.resource_lifetime=200;
            r.resource_generation=300;r.resource_version=400;r.bytes=416;r.stride=416;
            r.resource_window_bytes=416;
            r.descriptor=prov::DescriptorKind::Dynamic;r.heap=20;r.heap_lifetime=21;
            r.heap_revision=22;r.descriptor_slot=i+1;r.descriptor_revision=23;
            r.heap_class=prov::HeapClass::CbvSrvUav;r.heap_capacity=64;
            r.table_base=0;r.table_extent=64;
            r.descriptor_source=24;
        }
        expected.required[2].key={prov::Stage::VS,prov::Kind::RootConstants,10,0,1,0};
        expected.required[3].key={prov::Stage::PS,prov::Kind::RootConstants,10,0,1,0};
        for(int i=2;i<4;++i) {
            auto& r=expected.required[i];r.bytes=8;r.root_words=2;
            r.words[0]=0x1234;r.words[1]=0x5678;
        }
        {auto& r=expected.required[4];r.key={prov::Stage::PS,prov::Kind::Texture,0,0,2,0};
         r.resource_id=102;r.resource_lifetime=202;r.resource_generation=302;
         r.resource_version=402;r.resource_window_bytes=1024;r.subresources=1;
         r.texture_lifetime=202;r.texture_version=402;
         r.texture_dimension=2;r.texture_format=28;
         r.texture_view=31;r.texture_content=32;r.texture_layout=33;
         r.descriptor=prov::DescriptorKind::Dynamic;r.heap=20;r.heap_lifetime=21;
         r.heap_revision=22;r.descriptor_slot=5;r.descriptor_revision=23;
         r.heap_class=prov::HeapClass::CbvSrvUav;r.heap_capacity=64;
         r.table_base=0;r.table_extent=64;
         r.descriptor_source=24;}
        {auto& r=expected.required[5];r.key={prov::Stage::PS,prov::Kind::Sampler,0,0,3,0};
         r.descriptor=prov::DescriptorKind::Static;
         r.sampler_identity=50;r.sampler_lifetime=51;
         r.sampler_revision=52;r.sampler_source=53;
         r.sampler_words=4;r.sampler_values[0]=1;r.sampler_values[1]=2;
         r.sampler_values[2]=3;r.sampler_values[3]=4;}
        {auto& r=expected.required[6];r.key={prov::Stage::IA,prov::Kind::Vertex,0,0,4,0};
         r.resource_id=101;r.resource_lifetime=201;r.resource_generation=301;
         r.resource_version=401;r.bytes=576;r.stride=64;r.resource_window_bytes=576;
         r.vertex_count=9;r.instance_count=1;}
        if(indexed) {
            auto& r=expected.required[7];r.key={prov::Stage::IA,prov::Kind::Index,0,0,5,0};
            r.resource_id=103;r.resource_lifetime=203;r.resource_generation=303;
            r.resource_version=403;r.bytes=18;r.stride=2;r.resource_window_bytes=18;
            r.index_format=prov::IndexFormat::UInt16;r.index_count=9;
            r.min_index=0;r.max_index=8;r.vertex_count=9;
        }
    }
};
static prov::Reason provenance_reason(const ProvenanceFixture& f) {
    prov::Contract c;CHECK(c.begin(f.limits,f.expected,f.observed));
    const bool ok=c.finish();
    CHECK((ok && c.phase()==prov::Phase::Checked &&
           c.result()==prov::Result::SyntheticConsistent) ||
          (!ok && c.phase()==prov::Phase::Refused && c.result()==prov::Result::None));
    return c.reason();
}
void drawInputProvenanceContract() {
    static ProvenanceFixture base;
    CHECK(provenance_reason(base)==prov::Reason::None);
    static ProvenanceFixture indexed(true);
    CHECK(provenance_reason(indexed)==prov::Reason::None);
    static ProvenanceFixture p;p=base;
    p.expected.identity.capture=0;CHECK(provenance_reason(p)==prov::Reason::MissingIdentity);
    p=base;p.observed.identity.device_epoch++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.list_lifetime++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.reset_generation++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.reset_revision++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.draw_ordinal++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.submission_serial++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.queue++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.pso_lifetime++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.root_lifetime++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.input_layout++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.profile_revision++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.all_stage_profile++;CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.identity.draw.count++;CHECK(provenance_reason(p)==prov::Reason::DrawMismatch);
    std::uint64_t prov::Identity::* const identity_fields[]={
        &prov::Identity::capture,&prov::Identity::device,&prov::Identity::device_epoch,
        &prov::Identity::list_lifetime,&prov::Identity::reset_generation,
        &prov::Identity::reset_revision,&prov::Identity::draw_ordinal,
        &prov::Identity::submission_serial,&prov::Identity::queue,
        &prov::Identity::queue_epoch,&prov::Identity::pso,
        &prov::Identity::pso_lifetime,&prov::Identity::root,
        &prov::Identity::root_lifetime,&prov::Identity::input_layout,
        &prov::Identity::profile_revision,&prov::Identity::all_stage_profile};
    for(auto field:identity_fields) {
        p=base;p.observed.identity.*field=0;
        CHECK(provenance_reason(p)==prov::Reason::MissingIdentity);
        p=base;++(p.observed.identity.*field);
        CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    }
    p=base;p.observed.identity.draw.count=0;p.expected.identity.draw.count=0;
    CHECK(provenance_reason(p)==prov::Reason::None); // valid zero, explicit presence
    p=base;p.observed.identity.draw.present=false;CHECK(provenance_reason(p)==prov::Reason::MissingIdentity);
    p=base;p.observed.absent_stages=0;CHECK(provenance_reason(p)==prov::Reason::MissingStage);
    p=base;p.expected.required[0].key.reg=11;CHECK(provenance_reason(p)==prov::Reason::RequiredInput);
    p=base;p.observed.input_count=6;CHECK(provenance_reason(p)==prov::Reason::RequiredInput);
    p=base;p.observed.inputs[2].word_count=1;CHECK(provenance_reason(p)==prov::Reason::RootWords);
    p=base;p.observed.inputs[2].words[0]++;CHECK(provenance_reason(p)==prov::Reason::RootWords);
    p=base;p.observed.inputs[3].words[0]++;
    p.expected.required[3].words[0]++;
    CHECK(provenance_reason(p)==prov::Reason::RootWords);
    p=base;p.observed.inputs[4].texture_content=0;CHECK(provenance_reason(p)==prov::Reason::Texture);
    p=base;p.observed.inputs[5].sampler_immutable=false;CHECK(provenance_reason(p)==prov::Reason::Sampler);
    p=base;p.observed.inputs[5].sampler_words[0]++;CHECK(provenance_reason(p)==prov::Reason::Sampler);
    p=base;p.observed.inputs[5].descriptor=prov::DescriptorKind::Dynamic;
    p.observed.inputs[5].heap=25;p.observed.inputs[5].heap_lifetime=26;
    p.observed.inputs[5].heap_revision=27;p.observed.inputs[5].heap_class=prov::HeapClass::Sampler;
    p.observed.inputs[5].heap_capacity=64;p.observed.inputs[5].table_base=0;
    p.observed.inputs[5].table_extent=64;p.observed.inputs[5].descriptor_slot=6;
    p.observed.inputs[5].descriptor_revision=28;p.observed.inputs[5].descriptor_source=29;
    p.expected.required[5].descriptor=prov::DescriptorKind::Dynamic;
    p.expected.required[5].heap=25;p.expected.required[5].heap_lifetime=26;
    p.expected.required[5].heap_revision=27;p.expected.required[5].heap_class=prov::HeapClass::Sampler;
    p.expected.required[5].heap_capacity=64;p.expected.required[5].table_base=0;
    p.expected.required[5].table_extent=64;p.expected.required[5].descriptor_slot=6;
    p.expected.required[5].descriptor_revision=28;p.expected.required[5].descriptor_source=29;
    CHECK(provenance_reason(p)==prov::Reason::None);
    p=base;p.observed.inputs[5].descriptor=prov::DescriptorKind::Dynamic;
    p.observed.inputs[5].heap=20;p.observed.inputs[5].heap_lifetime=21;
    p.observed.inputs[5].heap_revision=99;p.observed.inputs[5].heap_class=prov::HeapClass::Sampler;
    p.observed.inputs[5].heap_capacity=64;p.observed.inputs[5].table_base=0;
    p.observed.inputs[5].table_extent=64;p.observed.inputs[5].descriptor_slot=6;
    p.observed.inputs[5].descriptor_revision=23;p.observed.inputs[5].descriptor_source=24;
    p.expected.required[5].descriptor=prov::DescriptorKind::Dynamic;
    p.expected.required[5].heap=20;p.expected.required[5].heap_lifetime=21;
    p.expected.required[5].heap_revision=99;p.expected.required[5].heap_class=prov::HeapClass::Sampler;
    p.expected.required[5].heap_capacity=64;p.expected.required[5].table_base=0;
    p.expected.required[5].table_extent=64;p.expected.required[5].descriptor_slot=6;
    p.expected.required[5].descriptor_revision=23;p.expected.required[5].descriptor_source=24;
    CHECK(provenance_reason(p)==prov::Reason::Descriptor);
    p=base;p.observed.inputs[6].vertex_count=1;CHECK(provenance_reason(p)==prov::Reason::Vertex);
    p=indexed;p.observed.inputs[7].index_values_known=false;CHECK(provenance_reason(p)==prov::Reason::Index);
    p=indexed;p.expected.count=7;p.observed.input_count=7;
    CHECK(provenance_reason(p)==prov::Reason::Index);
    p=indexed;p.observed.inputs[7].min_index=65528;
    p.observed.inputs[7].max_index=65536;
    p.expected.required[7].min_index=65528;
    p.expected.required[7].max_index=65536;
    p.observed.identity.draw.base_vertex=-65528;
    p.expected.identity.draw.base_vertex=-65528;
    CHECK(provenance_reason(p)==prov::Reason::Index);
    p=indexed;p.expected.count=9;p.observed.input_count=9;
    p.expected.required[8]=p.expected.required[6];
    p.expected.required[8].key.reg=1;p.expected.required[8].key.ia_slot=1;
    p.observed.inputs[8]=p.observed.inputs[6];
    p.observed.inputs[8].key.reg=1;p.observed.inputs[8].key.ia_slot=1;
    CHECK(provenance_reason(p)==prov::Reason::None);
    p.observed.inputs[8].first_vertex=1;
    CHECK(provenance_reason(p)==prov::Reason::Index);
    p=base;p.observed.inputs[0].root_lifetime++;CHECK(provenance_reason(p)==prov::Reason::RootLifetime);
    p=base;p.observed.inputs[0].descriptor_revision=0;CHECK(provenance_reason(p)==prov::Reason::Descriptor);
    p=base;p.observed.resources[0].copy_queue++;CHECK(provenance_reason(p)==prov::Reason::Queue);
    p=base;p.observed.copy.queue++;CHECK(provenance_reason(p)==prov::Reason::Queue);
    p=base;p.observed.copy.after_submission=p.expected.copy_after_submission=7;
    CHECK(provenance_reason(p)==prov::Reason::Ordering);
    p=base;p.observed.copy.copy_serial++;
    CHECK(provenance_reason(p)==prov::Reason::IdentityMismatch);
    p=base;p.observed.resources[0].ordered=false;CHECK(provenance_reason(p)==prov::Reason::Ordering);
    p=base;p.observed.resources[0].draw_version++;CHECK(provenance_reason(p)==prov::Reason::ResourceVersion);
    p=base;p.observed.resources[0].copy_bytes++;CHECK(provenance_reason(p)==prov::Reason::ResourceRange);
    p=base;p.observed.resources[0].offset=100;
    p.observed.resources[0].copy_offset=100;
    p.expected.required[0].resource_window_offset=100;
    p.expected.required[1].resource_window_offset=100;
    CHECK(provenance_reason(p)==prov::Reason::None);
    p.observed.resources[0].offset=101;p.observed.resources[0].copy_offset=101;
    CHECK(provenance_reason(p)==prov::Reason::ResourceRange);
    p=base;p.observed.resources[0].version_wrapped=true;CHECK(provenance_reason(p)==prov::Reason::ResourceVersion);
    p=base;p.observed.resources[0].complete_history=false;CHECK(provenance_reason(p)==prov::Reason::History);
    p=base;p.observed.resources[0].writer_excluded=false;CHECK(provenance_reason(p)==prov::Reason::Writer);
    p=base;p.observed.resources[0].released=true;CHECK(provenance_reason(p)==prov::Reason::ResourceIdentity);
    p=base;p.observed.resources[0].lifetime++;CHECK(provenance_reason(p)==prov::Reason::ResourceIdentity);
    p=base;p.observed.resources[0].generation++;CHECK(provenance_reason(p)==prov::Reason::ResourceIdentity);
    p=base;p.observed.resources[0].offset=UINT64_MAX;
    p.observed.resources[0].bytes=2;p.observed.resources[0].copy_bytes=2;
    CHECK(provenance_reason(p)==prov::Reason::ResourceRange);
    p=base;p.observed.resources[0].aliased=true;CHECK(provenance_reason(p)==prov::Reason::Alias);
    p=base;p.observed.resources[0].descriptor_overwritten=true;CHECK(provenance_reason(p)==prov::Reason::Descriptor);
    p=base;p.observed.states[0].covered=false;CHECK(provenance_reason(p)==prov::Reason::State);
    p=base;p.observed.state_count=2;CHECK(provenance_reason(p)==prov::Reason::State);
    p=base;p.observed.states[0].restored=prov::State::CopySource;
    CHECK(provenance_reason(p)==prov::Reason::Restoration);
    p=base;p.observed.states[0].conflicting=true;CHECK(provenance_reason(p)==prov::Reason::State);
    p=base;p.observed.states[0].original=static_cast<prov::State>(UINT32_MAX);
    p.observed.states[0].restored=p.observed.states[0].original;
    CHECK(provenance_reason(p)==prov::Reason::State);
    p=base;p.observed.inputs[0].origin=prov::Origin::NativeCertified;
    CHECK(provenance_reason(p)==prov::Reason::Origin);
    p=base;p.expected.origin=prov::Origin::Invalid;
    CHECK(provenance_reason(p)==prov::Reason::Origin);
    p=base;p.observed.resources[0].origin=prov::Origin::NativeCertified;
    CHECK(provenance_reason(p)==prov::Reason::Origin);
    p=base;p.observed.copy_proof=true;CHECK(provenance_reason(p)==prov::Reason::ProofForgery);
    p=base;p.observed.inputs[7]=p.observed.inputs[0];p.observed.input_count=8;
    CHECK(provenance_reason(p)==prov::Reason::DuplicateInput);
    p.observed.inputs[7].alias_of=0;CHECK(provenance_reason(p)==prov::Reason::None);
    p.observed.inputs[7].descriptor_source++;
    CHECK(provenance_reason(p)==prov::Reason::DuplicateInput);
    p.observed.inputs[7]=p.observed.inputs[0];p.observed.inputs[7].alias_of=0;
    p.observed.inputs[7].origin=prov::Origin::NativeCertified;
    CHECK(provenance_reason(p)==prov::Reason::DuplicateInput);
    for(std::size_t i=0;i<base.expected.count;++i) {
        p=base;p.observed.inputs[i].key.reg+=1000;
        CHECK(provenance_reason(p)==prov::Reason::RequiredInput);
    }
    p=indexed;p.observed.inputs[7].key.reg=99;
    CHECK(provenance_reason(p)==prov::Reason::RequiredInput);
    p=base;p.observed.inputs[0].key.kind=prov::Kind::RootDescriptor;
    p.observed.inputs[0].descriptor=prov::DescriptorKind::None;
    p.observed.inputs[0].heap=p.observed.inputs[0].heap_lifetime=0;
    p.observed.inputs[0].heap_revision=p.observed.inputs[0].descriptor_slot=0;
    p.observed.inputs[0].descriptor_revision=p.observed.inputs[0].descriptor_source=0;
    p.expected.required[0].key.kind=prov::Kind::RootDescriptor;
    p.expected.required[0].descriptor=prov::DescriptorKind::None;
    p.expected.required[0].heap=p.expected.required[0].heap_lifetime=0;
    p.expected.required[0].heap_revision=p.expected.required[0].descriptor_slot=0;
    p.expected.required[0].descriptor_revision=p.expected.required[0].descriptor_source=0;
    CHECK(provenance_reason(p)==prov::Reason::None);
    p=base;p.observed.inputs[0].key.kind=static_cast<prov::Kind>(255);
    p.expected.required[0].key.kind=static_cast<prov::Kind>(255);
    CHECK(provenance_reason(p)==prov::Reason::UnsupportedCategory);
    p=base;p.observed.inputs[0].offset=UINT64_MAX;
    p.expected.required[0].offset=UINT64_MAX;
    CHECK(provenance_reason(p)==prov::Reason::ResourceRange);
    p=indexed;p.observed.inputs[7].index_count=UINT64_MAX;
    CHECK(provenance_reason(p)==prov::Reason::Index);
    p=indexed;p.observed.identity.draw.base_vertex=-1;
    p.expected.identity.draw.base_vertex=-1;
    CHECK(provenance_reason(p)==prov::Reason::Index);
    p=base;
    for(int i=2;i<4;++i) {
        p.observed.inputs[i].word_count=64;p.observed.inputs[i].bytes=256;
        p.expected.required[i].root_words=64;p.expected.required[i].bytes=256;
    }
    CHECK(provenance_reason(p)==prov::Reason::None);
    p.observed.inputs[2].word_count=65;
    CHECK(provenance_reason(p)==prov::Reason::RootWords);
    p=base;p.observed.inputs[5].sampler_word_count=128;
    p.expected.required[5].sampler_words=128;
    CHECK(provenance_reason(p)==prov::Reason::None);
    p.observed.inputs[5].sampler_word_count=129;
    CHECK(provenance_reason(p)==prov::Reason::Sampler);
    p=base;p.observed.input_count=p.expected.count=128;
    for(std::size_t i=7;i<128;++i) {
        p.observed.inputs[i]=base.observed.inputs[0];
        p.expected.required[i]=base.expected.required[0];
        p.observed.inputs[i].key.reg=static_cast<std::uint32_t>(100+i);
        p.expected.required[i].key.reg=static_cast<std::uint32_t>(100+i);
    }
    CHECK(provenance_reason(p)==prov::Reason::None);
    p=base;p.observed.resource_count=64;p.observed.state_count=128;
    for(std::size_t i=3;i<64;++i) {
        p.observed.resources[i]=base.observed.resources[0];
        p.observed.resources[i].id=100+i;
        p.observed.states[i]=base.observed.states[0];
        p.observed.states[i].resource=static_cast<std::uint32_t>(i);
    }
    for(std::size_t i=64;i<128;++i) {
        p.observed.states[i]=base.observed.states[0];
        p.observed.states[i].resource=static_cast<std::uint32_t>(i-64);
        p.observed.states[i].subresource=1;
    }
    CHECK(provenance_reason(p)==prov::Reason::None);
    p=base;p.limits.entries=6;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity && !c.begin(base.limits,base.expected,base.observed));}
    p=base;p.limits.entries=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.limits.root_words=65;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.limits.sampler_words=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.limits.resources=65;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.limits.states=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.observed.input_count=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.expected.count=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.observed.resource_count=65;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    p=base;p.observed.state_count=129;{prov::Contract c;CHECK(!c.begin(p.limits,p.expected,p.observed));
        CHECK(c.reason()==prov::Reason::Capacity);}
    {prov::Contract c;CHECK(!c.begin(base.limits,base.expected,base.observed,prov::Fault::Begin));
     CHECK(c.reason()==prov::Reason::Fault && !c.finish());}
    {prov::Contract c;CHECK(c.begin(base.limits,base.expected,base.observed));
     c.missed_event();CHECK(!c.finish() && c.reason()==prov::Reason::History);}
    {prov::Contract c;CHECK(c.begin(base.limits,base.expected,base.observed));
     CHECK(!c.begin(base.limits,base.expected,base.observed));
     CHECK(!c.finish() && c.phase()==prov::Phase::Refused &&
           c.reason()==prov::Reason::Attempt);}
    {prov::Contract c;CHECK(c.begin(base.limits,base.expected,base.observed));
     c.overlap();CHECK(!c.finish() && c.reason()==prov::Reason::Ordering);}
    {prov::Contract c;CHECK(c.begin(base.limits,base.expected,base.observed));
     c.mutation();CHECK(!c.finish() && c.reason()==prov::Reason::ResourceVersion);}
    {prov::Contract c;CHECK(c.begin(base.limits,base.expected,base.observed));
     CHECK(!c.finish(prov::Fault::Finish) && c.reason()==prov::Reason::Fault);}
    {p=base;prov::Contract c;
     CHECK(c.begin(p.limits,p.expected,p.observed));
     p.observed.resources[0].draw_version=999;
     p.expected.required[0].key.reg=99;
     CHECK(c.finish() && c.expected().required[0].key.reg==10 &&
           c.observed().resources[0].draw_version==400);
     CHECK(!c.native_proof_available() && !c.observed().copy_proof &&
           !c.observed().replay_proof && !c.observed().requirements_proven &&
           !c.observed().cpu_data_complete && !c.observed().gpu_replay_complete);
     CHECK(!c.begin(base.limits,base.expected,base.observed) &&
           c.phase()==prov::Phase::Checked);}
    {prov::Contract c;allocation_budget=0;
     const bool started=c.begin(base.limits,base.expected,base.observed);
     const bool finished=c.finish();allocation_budget=-1;
     CHECK(started && finished && c.result()==prov::Result::SyntheticConsistent);}
    CHECK(std::strcmp(prov::diagnostic(prov::Reason::RequiredInput),"required input")==0);
    static_assert(std::is_trivially_copyable_v<prov::Profile> &&
                  std::is_trivially_copyable_v<prov::Snapshot>);
}
int main() {
    ownedAndFreeze();rejections();allocations();ownedFakeFencedCopy();gpuTransactionContract();
    drawInputProvenanceContract();
    std::printf("%u native copy provider checks passed (synthetic only)\n",checks);
}
