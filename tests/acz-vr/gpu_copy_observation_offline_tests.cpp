// SYNTHETIC ONLY: real production wrappers, writable fake vtables and fake
// HRESULTs/tokens. No native device, queue, fence, copy, Map, game or XR exists.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include "gpu_copy_ledger_fixture.h"
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace acz::gpu_copy;
static size_t allocations=0;
void* operator new(size_t n) {++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
namespace acz::gpu_copy {
struct WitnessFixtureAccess {
    static bool attach(Witness& w,Session& s,const Key& k){return w.attach(s,k);}
};
struct ObservationFixtureAccess {
    static Witness& witness(ObservationFeed& f){return *f.witness_;}
    static bool attach(ObservationFeed& f,Session& s,const Key& k) {
        if(!WitnessFixtureAccess::attach(*f.witness_,s,k))return false;
        f.recorded_=k;return true;
    }
};
}
static_assert(sizeof(acz::CopyNativeCall)<=32,"Small automatic wrapper guard");
static_assert(sizeof(ObservationFeed)<=256,"Bounded off-Draw feed storage");
static Identity syntheticIdentity(){return {1,2,3,4,6,7,8,true};}
static bool ownershipAvailable=true;
static unsigned providerCalls=0;
static bool syntheticReset(void*,const void*,const void*,Identity& out) noexcept {
    ++providerCalls;out=syntheticIdentity();return ownershipAvailable;
}
static bool syntheticExecute(void*,const void* queue,const Key& recorded,Key& out) noexcept;
static OwnershipProvider syntheticProvider(){return {nullptr,syntheticReset,syntheticExecute};}
struct Rig;
struct Object {void** table=nullptr;Rig* rig=nullptr;};
static unsigned calls[86]{};
static HRESULT resetHr=S_OK,closeHr=S_OK;
static std::function<void()> nativeAction;
static void invoked(unsigned slot) {++calls[slot];if(nativeAction)nativeAction();}
static void STDMETHODCALLTYPE unused() {CHECK(false);}
static HRESULT STDMETHODCALLTYPE query(ID3D12GraphicsCommandList*,REFIID,void**);
static ULONG STDMETHODCALLTYPE addRef(IUnknown*);
static ULONG STDMETHODCALLTYPE release(IUnknown*);
static HRESULT STDMETHODCALLTYPE device(ID3D12GraphicsCommandList*,REFIID,void** out){*out=nullptr;return E_NOINTERFACE;}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE type(ID3D12GraphicsCommandList*){return D3D12_COMMAND_LIST_TYPE_DIRECT;}
static HRESULT STDMETHODCALLTYPE nativeReset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator* a,ID3D12PipelineState* p) {
    CHECK(a==reinterpret_cast<ID3D12CommandAllocator*>(uintptr_t(0x600))&&!p);invoked(10);return resetHr;
}
static HRESULT STDMETHODCALLTYPE nativeClose(ID3D12GraphicsCommandList*){invoked(9);return closeHr;}
static void STDMETHODCALLTYPE nativeClear(ID3D12GraphicsCommandList*,ID3D12PipelineState* p){CHECK(!p);invoked(11);}
static void STDMETHODCALLTYPE nativeDraw(ID3D12GraphicsCommandList*,UINT count,UINT instances,UINT start,UINT first) {
    CHECK(count==3&&instances==1&&start==2&&!first);invoked(12);
}
static void STDMETHODCALLTYPE nativePred(ID3D12GraphicsCommandList*,ID3D12Resource* p,UINT64 offset,D3D12_PREDICATION_OP op) {
    CHECK((!p||p==reinterpret_cast<ID3D12Resource*>(uintptr_t(0x700)))&&offset==37&&op==D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);invoked(55);
}
static void STDMETHODCALLTYPE nativeBeginQuery(ID3D12GraphicsCommandList*,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE t,UINT index) {
    CHECK(!heap&&t==D3D12_QUERY_TYPE_OCCLUSION&&index==17);invoked(52);
}
static void STDMETHODCALLTYPE nativeEndQuery(ID3D12GraphicsCommandList*,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE t,UINT index) {
    CHECK(!heap&&t==D3D12_QUERY_TYPE_OCCLUSION&&index==17);invoked(53);
}
static void STDMETHODCALLTYPE nativeResolve(ID3D12GraphicsCommandList*,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE t,UINT first,UINT count,ID3D12Resource* dest,UINT64 offset) {
    CHECK(!heap&&t==D3D12_QUERY_TYPE_OCCLUSION&&first==3&&count==7&&!dest&&offset==91);invoked(54);
}
static D3D12_RENDER_PASS_FLAGS passFlags=D3D12_RENDER_PASS_FLAG_NONE;
static void STDMETHODCALLTYPE nativePass(ID3D12GraphicsCommandList4*,UINT count,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,D3D12_RENDER_PASS_FLAGS flags) {
    CHECK(count==0&&!rt&&!ds&&flags==passFlags);invoked(68);
}
static void STDMETHODCALLTYPE nativeEndPass(ID3D12GraphicsCommandList4*){invoked(69);}
static void STDMETHODCALLTYPE nativeProtected(ID3D12GraphicsCommandList3*,ID3D12ProtectedResourceSession* p){CHECK(!p);invoked(67);}
static void STDMETHODCALLTYPE nativeBundle(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList* p){CHECK(!p);invoked(27);}
static void STDMETHODCALLTYPE nativeIndirect(ID3D12GraphicsCommandList*,ID3D12CommandSignature* p,UINT count,
    ID3D12Resource* args,UINT64 offset,ID3D12Resource* counter,UINT64 counterOffset) {
    CHECK(!p&&count==5&&!args&&offset==19&&!counter&&counterOffset==23);invoked(59);
}
static UINT executeCount=0;
static ID3D12CommandList* const* executeArray=nullptr;
static unsigned executeCalls=0;
static void STDMETHODCALLTYPE nativeExecute(ID3D12CommandQueue*,UINT count,ID3D12CommandList* const* lists) {
    CHECK(count==executeCount&&lists==executeArray);++executeCalls;if(nativeAction)nativeAction();
}
static BOOL WINAPI protect(LPVOID,SIZE_T,DWORD,DWORD* old){*old=PAGE_READWRITE;return TRUE;}
using Table=std::array<void*,86>;
struct Rig {
    // Heap storage: distinct tables for every alias, no large automatic arrays.
    std::unique_ptr<std::array<Table,11>> tables=std::make_unique<std::array<Table,11>>();
    std::array<Object,11> aliases{};
    std::array<void*,26> queueTable{};
    Object queue{};
    int unavailable=-1;
    ULONG refs=1;
    std::shared_ptr<ObservationFeed> feed;
    explicit Rig(bool on=true,bool synthetic=true,int missing=-1):unavailable(missing) {
        acz::g_copyObservationTest=on;acz::g_copyOwnershipTest=synthetic?syntheticProvider():OwnershipProvider{};
        acz::g_protect=protect;acz::g_pinResult=true;ownershipAvailable=true;
        resetHr=closeHr=S_OK;nativeAction={};std::fill(std::begin(calls),std::end(calls),0);
        for(unsigned i=0;i<11;++i) {
            auto& t=(*tables)[i];t.fill(reinterpret_cast<void*>(&unused));aliases[i]={t.data(),this};
            t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&addRef);t[2]=reinterpret_cast<void*>(&release);
            t[7]=reinterpret_cast<void*>(&device);t[8]=reinterpret_cast<void*>(&type);
            t[9]=reinterpret_cast<void*>(&nativeClose);t[10]=reinterpret_cast<void*>(&nativeReset);t[11]=reinterpret_cast<void*>(&nativeClear);
            t[12]=reinterpret_cast<void*>(&nativeDraw);
            t[27]=reinterpret_cast<void*>(&nativeBundle);t[52]=reinterpret_cast<void*>(&nativeBeginQuery);t[53]=reinterpret_cast<void*>(&nativeEndQuery);
            t[54]=reinterpret_cast<void*>(&nativeResolve);t[55]=reinterpret_cast<void*>(&nativePred);t[59]=reinterpret_cast<void*>(&nativeIndirect);
            t[67]=reinterpret_cast<void*>(&nativeProtected);t[68]=reinterpret_cast<void*>(&nativePass);t[69]=reinterpret_cast<void*>(&nativeEndPass);
        }
        queueTable[10]=reinterpret_cast<void*>(&nativeExecute);queue={queueTable.data(),this};
        CHECK(acz::hookCaptureCommandList(list()));CHECK(acz::hookCommandQueue(q()));
        feed=acz::state().states.at(list()).copyObservation;
    }
    ID3D12GraphicsCommandList* list(unsigned i=0){return reinterpret_cast<ID3D12GraphicsCommandList*>(&aliases[i]);}
    ID3D12CommandQueue* q(){return reinterpret_cast<ID3D12CommandQueue*>(&queue);}
    void reset(unsigned i=0){CHECK(list(i)->Reset(reinterpret_cast<ID3D12CommandAllocator*>(uintptr_t(0x600)),nullptr)==resetHr);}
    bool allowed(){return feed&&ObservationFixtureAccess::witness(*feed).insertion_allowed(syntheticIdentity());}
    void execute(bool duplicate=false,bool otherQueue=false) {
        ID3D12CommandList* lists[]={list(),list(10)};
        executeCount=duplicate?2:1;executeArray=lists;
        if(otherQueue) {
            Object other{queueTable.data(),this};reinterpret_cast<ID3D12CommandQueue*>(&other)->ExecuteCommandLists(executeCount,lists);
        } else q()->ExecuteCommandLists(executeCount,lists);
        executeArray=nullptr;
    }
    ~Rig() {
        nativeAction={};auto& s=acz::state();std::lock_guard lock(s.mutex);
        s.states.erase(list());
        for(auto it=s.listKeys.begin();it!=s.listKeys.end();)if(it->second==list())it=s.listKeys.erase(it);else ++it;
        for(auto& t:*tables)s.lists.erase(t.data());s.queues.erase(queueTable.data());
        // Independent synthetic rigs; no production reservation recycling API.
        for(auto& slot:s.copyFeeds)slot.reset();s.copyFeedReservations=0;s.copyFeedUnavailable=0;
        acz::g_copyObservationTest=false;acz::g_copyOwnershipTest={};
    }
};
static HRESULT STDMETHODCALLTYPE query(ID3D12GraphicsCommandList* self,REFIID iid,void** out) {
    auto* r=reinterpret_cast<Object*>(self)->rig;*out=nullptr;
    if(iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12CommandList)||iid==__uuidof(ID3D12DeviceChild)||iid==__uuidof(ID3D12Object))*out=r->list();
    for(unsigned i=0;i<11;++i)if(iid==acz::listIids[i]&&int(i)!=r->unavailable)*out=r->list(i);
    if(!*out)return E_NOINTERFACE;++r->refs;return S_OK;
}
static ULONG STDMETHODCALLTYPE addRef(IUnknown* self){return ++reinterpret_cast<Object*>(self)->rig->refs;}
static ULONG STDMETHODCALLTYPE release(IUnknown* self){return --reinterpret_cast<Object*>(self)->rig->refs;}
static bool syntheticExecute(void*,const void* queue,const Key& recorded,Key& out) noexcept {
    out=recorded;auto* o=static_cast<const Object*>(queue);
    if(o!=&o->rig->queue)++out.queue; // explicit synthetic wrong-queue token
    return true;
}
struct BackendFake final:Backend {
    unsigned reads=0,drops=0,releases=0;
    FenceSample sample{{1,2,3,4,1},1,true};
    FenceSample poll() noexcept override{return sample;}
    bool read() noexcept override{++reads;return true;}
    void drop_logical_source_lease(uint64_t) noexcept override{++drops;}
    void release_private(uint64_t) noexcept override{++releases;}
};
static void attachSynthetic(Rig& r,Session& session,BackendFake& backend) {
    CHECK(r.allowed());auto admission=fixture_admission();admission.key.generation=1;
    CHECK(FixtureAccess::arm(session,true,backend,admission));CHECK(FixtureAccess::reserve(session,admission));
    CHECK(FixtureAccess::created(session));CHECK(FixtureAccess::record(session));
    CHECK(ObservationFixtureAccess::attach(*r.feed,session,admission.key));
}
static void offAndUnavailable() {
    CHECK(!acz::diagnostics::Config{}.gpu_copy_witness_observation());
    CHECK(acz::diagnostics::parse_config("gpu_copy_witness_observation=true").config.gpu_copy_witness_observation());
    CHECK(!acz::diagnostics::parse_config("gpu_copy_witness_observation=false").config.gpu_copy_witness_observation());
    CHECK(acz::diagnostics::parse_config("gpu_copy_witness_observation=bad").issue==acz::diagnostics::ConfigIssue::out_of_range);
    {Rig r(false);const auto before=providerCalls;r.reset();CHECK(!r.feed&&providerCalls==before);
     for(auto& t:*r.tables)CHECK(t[55]==reinterpret_cast<void*>(&nativePred)&&t[9]==reinterpret_cast<void*>(&nativeClose));
     r.list()->SetPredication(nullptr,37,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);CHECK(calls[55]==1);
     const auto allocated=allocations;r.list()->DrawInstanced(3,1,2,0);
     CHECK(allocations==allocated&&calls[12]==1&&!acz::state().states.at(r.list()).copyObservation);}
    {Rig r(true,false);for(unsigned i=0;i<3;++i)r.reset();
     CHECK(r.feed&&!r.feed->constructed()&&r.feed->ownership_unavailable()==3);
     CHECK(!r.feed->snapshot().coverage&&!r.feed->snapshot().generation);
     const auto allocated=allocations;r.list()->DrawInstanced(3,1,2,0);
     CHECK(allocations==allocated&&calls[12]==1&&!r.feed->constructed());
     r.list()->ClearState(nullptr);CHECK(!r.feed->constructed());}
    {Rig r;ownershipAvailable=false;r.reset();CHECK(!r.feed->constructed());
     ownershipAvailable=true;r.reset();CHECK(r.allowed()&&!r.feed->snapshot().ambiguous);}
    {acz::state().copyFeedReservations=64;Rig r;r.reset();
     CHECK(!r.feed&&acz::state().copyFeedUnavailable==1);}
}
static void aliasesAndRestrictions() {
    for(unsigned version=0;version<11;++version) {
        Rig r;r.reset(version);CHECK(r.allowed()&&r.feed->snapshot().coverage==required_coverage);
        const auto before=calls[55];
        // New restriction observations happen AFTER fake native forwarding.
        nativeAction=[&]{CHECK(r.allowed());};
        r.list(version)->SetPredication(reinterpret_cast<ID3D12Resource*>(uintptr_t(0x700)),37,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
        nativeAction={};CHECK(calls[55]==before+1&&!r.allowed());
        const auto generation=r.feed->snapshot().generation;r.list(version)->ClearState(nullptr);
        CHECK(r.feed->snapshot().generation==generation&&!r.allowed());
        r.list(version)->SetPredication(nullptr,37,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);CHECK(r.allowed());
    }
    for(unsigned version=0;version<11;++version)for(unsigned event=0;event<7;++event) {
        if((event==2&&version<3)||(event>=3&&version<4))continue;
        Rig r;r.reset(version);CHECK(r.allowed());
        auto* l4=reinterpret_cast<ID3D12GraphicsCommandList4*>(r.list(version));
        auto* l3=reinterpret_cast<ID3D12GraphicsCommandList3*>(r.list(version));
        if(event==0) {r.list(version)->BeginQuery(nullptr,D3D12_QUERY_TYPE_OCCLUSION,17);r.list(version)->EndQuery(nullptr,D3D12_QUERY_TYPE_OCCLUSION,17);CHECK(calls[52]==1&&calls[53]==1);}
        if(event==1) {r.list(version)->ResolveQueryData(nullptr,D3D12_QUERY_TYPE_OCCLUSION,3,7,nullptr,91);CHECK(calls[54]==1);}
        if(event==2) {l3->SetProtectedResourceSession(nullptr);CHECK(calls[67]==1);}
        if(event>=3&&event<=5) {passFlags=event==3?D3D12_RENDER_PASS_FLAG_NONE:event==4?D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS:D3D12_RENDER_PASS_FLAG_RESUMING_PASS;
            l4->BeginRenderPass(0,nullptr,nullptr,passFlags);l4->EndRenderPass();CHECK(calls[68]==1&&calls[69]==1);}
        if(event==6) {l4->EndRenderPass();CHECK(calls[69]==1);}
        CHECK(!r.allowed());
    }
    for(unsigned event=0;event<2;++event) {
        Rig r;r.reset();nativeAction=[&]{CHECK(!r.allowed());}; // pre-forward exclusion
        if(event==0)r.list()->ExecuteBundle(nullptr);
        else r.list()->ExecuteIndirect(nullptr,5,nullptr,19,nullptr,23);
        nativeAction={};CHECK(calls[event?59:27]==1&&!r.allowed());
    }
    {Rig r(true,true,7);r.reset();CHECK(!(r.feed->snapshot().coverage&coverage_bit(Coverage::alias7))&&!r.allowed());
     r.unavailable=-1;CHECK(acz::hookCaptureCommandList(r.list()));CHECK(!r.allowed());
     r.reset();CHECK(r.allowed());}
    {Rig r;(*r.tables)[8][55]=reinterpret_cast<void*>(&unused);
     CHECK(!acz::hookCaptureCommandList(r.list()));r.reset();CHECK(!r.allowed());}
    {Rig r;resetHr=E_FAIL;r.reset();CHECK(r.feed->snapshot().ambiguous&&!r.feed->snapshot().generation);}
    {Rig r;resetHr=S_FALSE;r.reset();CHECK(r.allowed()&&r.feed->snapshot().generation==1);
     closeHr=S_FALSE;CHECK(r.list()->Close()==S_FALSE&&!r.feed->snapshot().ambiguous&&!r.feed->snapshot().open);}
    {Rig r;r.reset();closeHr=E_FAIL;CHECK(r.list()->Close()==E_FAIL);CHECK(r.feed->snapshot().ambiguous);}
    {Rig r;bool nested=false;nativeAction=[&]{if(!nested){nested=true;r.list()->ClearState(nullptr);}};
     r.reset();nativeAction={};CHECK(r.feed->snapshot().ambiguous&&!r.allowed());}
    {Rig r;reinterpret_cast<ID3D12GraphicsCommandList3*>(r.list(3))->SetProtectedResourceSession(nullptr);
     CHECK(!r.feed->constructed());r.reset();CHECK(r.feed->snapshot().ambiguous&&!r.allowed());
     CHECK(!(r.feed->snapshot().coverage&coverage_bit(Coverage::protected_session)));}
    {Rig r;passFlags=D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS;
     reinterpret_cast<ID3D12GraphicsCommandList4*>(r.list(4))->BeginRenderPass(0,nullptr,nullptr,passFlags);
     CHECK(!r.feed->constructed());r.reset();CHECK(r.feed->snapshot().ambiguous&&!r.allowed());}
}
static void submissionCases() {
    for(unsigned mode=0;mode<10;++mode) {
        Session session;BackendFake backend;Rig r;r.reset();attachSynthetic(r,session,backend);
        if(mode==0) {r.list()->ClearState(nullptr);CHECK(r.feed->snapshot().generation==1&&!session.snapshot().recording_closed);}
        if(mode!=4)CHECK(r.list()->Close()==S_OK);
        const auto before=executeCalls;
        nativeAction=[&] {
            CHECK(executeCalls==before+1);
            CHECK(!session.snapshot().gpu_complete&&!backend.reads&&!backend.drops&&!backend.releases);
            if(mode==0)CHECK(session.snapshot().state==State::submitted&&r.feed->snapshot().pending);
        };
        r.execute(mode==1,mode==2);nativeAction={};CHECK(executeCalls==before+1);
        if(mode==5){r.execute();CHECK(executeCalls==before+2);}
        if(mode==6)r.reset(); // outstanding join cannot supply closure
        if(mode==7) {CHECK(r.feed->signal_result(r.feed->ticket(),true));r.reset();r.execute();}
        if(mode==8) {
            std::array<ID3D12CommandList*,65> tooMany{};tooMany.fill(r.list());
            executeCount=65;executeArray=tooMany.data();r.q()->ExecuteCommandLists(executeCount,executeArray);executeArray=nullptr;
        }
        const bool ok=r.feed->signal_result(mode==9?r.feed->ticket()+1:r.feed->ticket(),mode!=3);
        if(mode==0) {
            CHECK(ok&&session.snapshot().state==State::signalled);
            // ClearState cannot close the old recording. Only actual Reset can.
            CHECK(!session.snapshot().recording_closed);r.reset();
            CHECK(session.snapshot().recording_closed&&session.poll()&&session.read()&&session.retire());
            CHECK(backend.reads==1&&backend.drops==1&&backend.releases==1);
        } else {
            CHECK(!ok&&session.snapshot().state==State::quarantined);
            CHECK(session.snapshot().private_held&&session.snapshot().source_lease);
            CHECK(!session.read()&&!session.retire()&&!backend.reads&&!backend.drops&&!backend.releases);
        }
    }
    {Session session;BackendFake backend;Rig r;r.reset();attachSynthetic(r,session,backend);
     CHECK(r.list()->Close()==S_OK);bool entered=false;
     nativeAction=[&]{if(!entered){entered=true;r.execute();}};
     r.reset();nativeAction={};CHECK(session.snapshot().state==State::quarantined);
     CHECK(!session.read()&&!session.retire()&&!backend.reads&&!backend.drops&&!backend.releases);}
}
int main() {
    offAndUnavailable();aliasesAndRestrictions();submissionCases();
    std::puts("synthetic observation-wrapper checks passed; no native GPU evidence");return 0;
}
