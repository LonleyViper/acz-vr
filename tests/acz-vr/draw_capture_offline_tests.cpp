// Pure CPU data + fake COM/vtables. No DLL, game, GPU or XR runtime loaded.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <thread>
#define CHECK(x) do { ++checks; if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
static int checks=0;
using namespace acz::capture;
using namespace acz::metadata;
struct Fixture {
    State state;
    Command command{true,5,2,3,-7,19};
    Inputs in;
    std::vector<uint8_t> srt,vertices,indices;
    Fixture(uint32_t stride=416):srt(13+5*stride),vertices(80),indices(65) {
        for(size_t i=0;i<srt.size();++i) srt[i]=uint8_t(i*17+3);
        std::fill(vertices.begin(),vertices.end(),uint8_t(0xab));std::fill(indices.begin(),indices.end(),uint8_t(0xcd));
        // This fixture's declared per-vertex layout has four 16-byte vertices.
        // With StartIndex=3/BaseVertex=-7, these five indices fetch [0,3].
        // Instance offset 19 does not affect this per-vertex-only IA layout.
        for(size_t i=0;i<5;++i) {indices[9+2*(3+i)]=uint8_t(7+i%4);indices[9+2*(3+i)+1]=0;}
        RootLayout layout;
        RootParameter table;table.kind=ParameterKind::table;table.visibility=Visibility::geometry;
        table.ranges.push_back({BindingKind::srv,1,10,0,0,2});
        RootParameter constants;constants.kind=ParameterKind::constants;constants.constant_count=2;constants.shader_register=1;
        layout.parameters={table,constants};
        state.tracking_complete=true;state.pipeline={true,true,{{BindingKind::srv,10,0,ShaderStage::geometry},{BindingKind::cbv,1,0,ShaderStage::vertex}},{1}};
        state.pipeline.identity=new_lifetime_identity();
        state.pipeline.explicit_root_identity=new_lifetime_identity();
        state.set_root(9,RootBindingMetadata::from_adapter(layout),state.pipeline.explicit_root_identity);state.set_heaps({4});state.set_value(0,ParameterKind::table,100);
        uint32_t words[]={7,8};state.set_constants(1,0,words,2);
        state.vertices[1]={2,7,64,16};state.index=BufferView{3,9,32,2};
        Descriptor descriptor{BindingKind::srv,{1,13,5ull*stride,stride},1,3,true};
        in.generation=state.generation;in.revision=state.revision;in.list_identity=state.list_identity;
        in.tables={{4,100,{Descriptor{},Descriptor{},descriptor},state.generation,state.revision,state.list_identity}};in.indexing_proven=true;in.srt_indices={0,2};
        in.vertex_fetch_proven=true;in.vertex_access={{1,0,64}};refresh();
    }
    void refresh() {in.resources={{1,state.generation,srt.size(),srt.data(),srt.size(),true,true,state.revision,state.list_identity},
                                 {2,state.generation,vertices.size(),vertices.data(),vertices.size(),true,true,state.revision,state.list_identity},
                                 {3,state.generation,indices.size(),indices.data(),indices.size(),true,true,state.revision,state.list_identity}};}
};
void pureTests() {
    // Equal recording counters do not authorize another list's provider data.
    Fixture listA,listB;
    CHECK(listA.state.generation==listB.state.generation && listA.state.revision==listB.state.revision);
    CHECK(listA.state.list_identity && listB.state.list_identity && listA.state.list_identity!=listB.state.list_identity);
    CHECK(!freeze(listB.state,listB.command,listA.in));
    auto wrongResource=listB.in;wrongResource.resources=listA.in.resources;
    CHECK(!freeze(listB.state,listB.command,wrongResource));
    auto wrongTable=listB.in;wrongTable.tables=listA.in.tables;
    CHECK(!freeze(listB.state,listB.command,wrongTable));
    CHECK(freeze(listA.state,listA.command,listA.in) && freeze(listB.state,listB.command,listB.in));
    State copied=listA.state;CHECK(copied.list_identity==listA.state.list_identity);
    copied.reset();CHECK(copied.list_identity==listA.state.list_identity && copied.generation==listA.state.generation+1);
    CHECK(!freeze(copied,listA.command,listA.in));
    // Reconstruct at identical storage, with identical recording counters.
    // Old provider provenance must reject before consulting freed source bytes.
    std::optional<Fixture> reused;reused.emplace();
    const auto oldInputs=reused->in;const auto oldAddress=&reused->state;
    const auto oldIdentity=reused->state.list_identity;
    reused.reset();reused.emplace();
    CHECK(&reused->state==oldAddress && reused->state.list_identity!=oldIdentity);
    CHECK(reused->state.generation==oldInputs.generation && reused->state.revision==oldInputs.revision);
    CHECK(!freeze(reused->state,reused->command,oldInputs));
    CHECK(freeze(reused->state,reused->command,reused->in));
    for(auto stride:{416u,512u,2048u}) {
        Fixture f(stride);auto result=freeze(f.state,f.command,f.in);CHECK(result);
        auto owned=result.snapshot;
        CHECK(owned->state.list_identity==f.state.list_identity);
        CHECK(owned->state.compatible_root());
        const auto rootIdentity=owned->state.root_identity, pipelineIdentity=owned->state.pipeline.identity;
        CHECK(owned->cpu_data_complete && !owned->gpu_replay_complete);
        CHECK(owned->command.indexed && owned->command.count==5 && owned->command.instances==2);
        CHECK(owned->command.start==3 && owned->command.base_vertex==-7 && owned->command.start_instance==19);
        CHECK(owned->state.index->offset==9 && owned->state.vertices.at(1).offset==7);
        CHECK(owned->resources.size()==3 && owned->srt_elements.size()==2);
        CHECK(owned->bindings.size()==1 && owned->bindings[0].descriptor.view.offset==13 && owned->bindings[0].descriptor.first_element==1);
        CHECK(owned->srt_elements[0]==std::vector<uint8_t>(f.srt.begin()+13+stride,f.srt.begin()+13+2*stride));
        CHECK(owned->srt_elements[1]==std::vector<uint8_t>(f.srt.begin()+13+3*stride,f.srt.begin()+13+4*stride));
        acz::srt::Matrix a{},b{};a.fill(1.25f);b.fill(-2.5f);
        const auto left=eye_payload(*owned,0,a,b);a.fill(4.75f);const auto right=eye_payload(*owned,0,a,b);
        CHECK(left && right && left.bytes!=right.bytes);
        for(size_t i=128;i<stride;++i) {CHECK(left.bytes[i]==owned->srt_elements[0][i]);CHECK(right.bytes[i]==left.bytes[i]);}
        const auto original=owned->srt_elements[0];
        f.srt.assign(f.srt.size(),0);f.srt.clear();f.srt.shrink_to_fit();f.vertices.clear();f.indices.clear();f.state.reset();
        CHECK(owned->srt_elements[0]==original && owned->resources.at(2)[0]==0xab);
        CHECK(owned->state.compatible_root() && owned->state.root_identity==rootIdentity && owned->state.pipeline.identity==pipelineIdentity);
        CHECK(eye_payload(*owned,0,acz::srt::Matrix{},acz::srt::Matrix{}));
        CHECK(!eye_payload(*owned,2,a,b));
    }
    {
        Fixture f;CHECK(freeze(f.state,f.command,f.in));
        acz::binding::BufferBindingRegistry registry;
        auto heap=registry.add_heap({77,0x7000,0x9000,1,32,1,1});CHECK(heap);
        acz::binding::Sampler sampler;sampler.words[0]=1;
        CHECK(registry.write_sampler(heap.token,0x7000,sampler));
        auto metadata=registry.resolve_gpu(heap.token,0x9000);CHECK(metadata);
        const auto buffer=f.in.tables[0].descriptors[2];
        f.in.tables[0].descriptors[2]=metadata.snapshot->descriptor;
        CHECK(!freeze(f.state,f.command,f.in)); // even with an otherwise valid synthetic proof
        f.in.tables[0].descriptors[2]=buffer;
        f.state.pipeline.resources.push_back({BindingKind::sampler,0,0,ShaderStage::pixel});
        CHECK(!freeze(f.state,f.command,f.in));
        acz::capture::State native;CHECK(!native.pipeline.requirements_proven);
    }
    auto reject=[](auto mutate) {Fixture f;mutate(f);auto r=freeze(f.state,f.command,f.in);CHECK(!r && !r.rejection.empty());};
    reject([](auto& f){f.state.tracking_complete=false;});
    reject([](auto& f){f.state.list_identity=0;});
    reject([](auto& f){f.in.list_identity=0;});
    reject([](auto& f){f.in.resources[0].list_identity=0;});
    reject([](auto& f){f.in.tables[0].list_identity=0;});
    reject([](auto& f){f.state.pipeline.requirements_proven=false;});
    reject([](auto& f){f.state.pipeline.identity=0;});
    reject([](auto& f){f.state.pipeline.explicit_root_identity=0;});
    reject([](auto& f){f.state.root_identity=0;});
    reject([](auto& f){f.state.root_identity=new_lifetime_identity();});
    reject([](auto& f){f.state.set_root(f.state.root_id,f.state.root,new_lifetime_identity());});
    reject([](auto& f){f.state.pipeline.drawcpt=false;});
    reject([](auto& f){f.state.reset();});
    reject([](auto& f){++f.in.revision;});
    reject([](auto& f){f.state.root.reset();});
    reject([](auto& f){f.state.root_id=0;});
    reject([](auto& f){f.in.resources[0].generation=0;});
    reject([](auto& f){f.in.resources[0].revision=0;});
    reject([](auto& f){f.in.resources[0].cpu_readable=false;});
    reject([](auto& f){f.in.resources[0].synchronized=false;});
    reject([](auto& f){f.in.resources[0].bytes=nullptr;});
    reject([](auto& f){--f.in.resources[0].byte_length;});
    reject([](auto& f){f.in.resources.erase(f.in.resources.begin()+1);});
    reject([](auto& f){f.in.resources.push_back(f.in.resources[0]);});
    reject([](auto& f){f.in.resources[0].byte_length=f.in.resources[0].resource_size=max_copy_bytes+1;});
    reject([](auto& f){f.in.resources[0].bytes=reinterpret_cast<const void*>(UINTPTR_MAX-1);});
    reject([](auto& f){f.in.tables[0].heap=999;});
    reject([](auto& f){f.in.tables[0].base=999;});
    reject([](auto& f){f.in.tables[0].revision=0;});
    reject([](auto& f){f.in.tables[0].generation=0;});
    reject([](auto& f){f.in.tables.push_back(f.in.tables[0]);});
    reject([](auto& f){f.in.tables[0].descriptors.resize(2);});
    reject([](auto& f){f.in.tables[0].descriptors[2].kind=BindingKind::uav;});
    reject([](auto& f){f.in.tables[0].descriptors[2].view.stride=384;});
    reject([](auto& f){f.in.tables[0].descriptors[2].view.stride=433;});
    reject([](auto& f){f.in.tables[0].descriptors[2].structured=false;});
    reject([](auto& f){f.in.tables[0].descriptors[2].buffer=false;});
    reject([](auto& f){f.in.tables[0].descriptors[2].first_element=UINT64_MAX;});
    reject([](auto& f){f.in.tables[0].descriptors[2].elements=UINT64_MAX/2;});
    reject([](auto& f){f.in.tables[0].descriptors[2].view.offset=UINT64_MAX;});
    reject([](auto& f){f.in.indexing_proven=false;});
    reject([](auto& f){f.in.srt_indices.pop_back();});
    reject([](auto& f){f.in.srt_indices[1]=3;});
    reject([](auto& f){f.state.roots.erase(0);});
    reject([](auto& f){f.state.roots[0].kind=ParameterKind::srv;});
    reject([](auto& f){f.state.roots[0].value=0;});
    reject([](auto& f){f.state.roots[1].written[1]=false;});
    reject([](auto& f){f.state.roots[1].constants.clear();});
    reject([](auto& f){f.state.vertices.clear();});
    reject([](auto& f){f.in.vertex_fetch_proven=false;});
    reject([](auto& f){f.in.vertex_access[0].length=65;});
    reject([](auto& f){f.in.vertex_access[0].offset=UINT64_MAX;});
    reject([](auto& f){f.in.vertex_access.clear();});
    reject([](auto& f){f.state.index.reset();});
    reject([](auto& f){f.command.count=100;});
    reject([](auto& f){f.state.index->stride=3;});
    reject([](auto& f){f.command.instances=4097;});
    reject([](auto& f){f.state.pipeline.resources.clear();});
    reject([](auto& f){f.state.pipeline.resources.push_back({BindingKind::srv,11,0,ShaderStage::vertex});});
    Fixture f;auto before=f.state.roots;f.state.set_heaps({4});CHECK(f.state.roots.size()==before.size());
    f.state.set_heaps({5});CHECK(!f.state.roots.count(0) && f.state.roots.count(1));
    f.state.set_value(0,ParameterKind::table,100);f.state.set_root(10,f.state.root);CHECK(f.state.roots.empty());
    f.state.set_constants(80,0,nullptr,1);CHECK(!f.state.tracking_complete);
    Fixture n;n.command.indexed=false;n.command.base_vertex=0;n.command.start=1;n.command.count=2;n.state.index.reset();
    auto nonindexed=freeze(n.state,n.command,n.in);CHECK(nonindexed && !nonindexed.snapshot->command.indexed);
    // Proven root-address adapter, with independent descriptor metadata/stride.
    RootLayout root;RootParameter p;p.kind=ParameterKind::srv;p.shader_register=10;p.visibility=Visibility::geometry;root.parameters={p};
    n.state.set_root(20,RootBindingMetadata::from_adapter(root),n.state.pipeline.explicit_root_identity);n.state.set_value(0,ParameterKind::srv,0x500);
    n.state.pipeline.resources.resize(1);
    n.in.revision=n.state.revision;n.refresh();n.in.root_descriptors[0x500]=n.in.tables[0].descriptors[2];
    CHECK(freeze(n.state,n.command,n.in));n.in.root_descriptors.clear();CHECK(!freeze(n.state,n.command,n.in));
}
struct Fake {void** table;ULONG refs=1;Fake* owner=nullptr;Fake* rootInterface=nullptr;Fake* unknownInterface=nullptr;};
static UINT drawArgs[5]{},constantArg[4]{};
static int draws=0;static HRESULT resetHr=S_OK;
static IUnknown* queryAlias=nullptr;
static bool queryFailure=false,queryCanonicalFailure=false;
static IUnknown* queryRootAlias=nullptr;
static bool auditMetadataCOM=false;
static std::function<void()> finalReleaseAction;
static std::function<void()> nonFinalReleaseAction;
static std::function<void()> rootQueryAction;
void checkCOMUnlocked() {
    if(!auditMetadataCOM) return;
    bool unlocked=false;
    std::thread probe([&] {if(acz::state().mutex.try_lock()) {unlocked=true;acz::state().mutex.unlock();}});
    probe.join();CHECK(unlocked);
}
ULONG STDMETHODCALLTYPE add(IUnknown* p) {checkCOMUnlocked();auto* f=reinterpret_cast<Fake*>(p);return ++(f->owner?f->owner:f)->refs;}
ULONG STDMETHODCALLTYPE release(IUnknown* p) {
    if(nonFinalReleaseAction) {auto action=std::move(nonFinalReleaseAction);nonFinalReleaseAction={};action();}
    checkCOMUnlocked();auto* f=reinterpret_cast<Fake*>(p);auto refs=--(f->owner?f->owner:f)->refs;
    if(!refs && finalReleaseAction) {auto action=std::move(finalReleaseAction);finalReleaseAction={};action();}
    return refs;
}
HRESULT STDMETHODCALLTYPE query(IUnknown* p,REFIID iid,void** out) {
    if(iid==__uuidof(ID3D12Device4)){*out=nullptr;return E_NOINTERFACE;}
    for(unsigned i=1;i<std::size(acz::listIids);++i)if(iid==acz::listIids[i]){*out=nullptr;return E_NOINTERFACE;}

    checkCOMUnlocked();
    if(rootQueryAction) {auto action=std::move(rootQueryAction);rootQueryAction={};action();}
    if(queryFailure || (queryCanonicalFailure && iid==__uuidof(IUnknown))) {*out=nullptr;return E_NOINTERFACE;}
    // Both interfaces of the split object return the same canonical identity.
    auto* f=reinterpret_cast<Fake*>(p);
    if(f->unknownInterface) {
        auto* value=iid==__uuidof(IUnknown)?f->unknownInterface:f->rootInterface;
        add(reinterpret_cast<IUnknown*>(value));*out=value;return S_OK;
    }
    auto value=(iid==__uuidof(IUnknown) && queryAlias)?queryAlias:
        (iid==__uuidof(ID3D12RootSignature) && queryRootAlias)?queryRootAlias:p;
    ++reinterpret_cast<Fake*>(value)->refs;*out=value;return S_OK;
}
static ID3D12CommandAllocator* forwardedAllocator=nullptr;
static ID3D12PipelineState* forwardedPSO=nullptr;
static ID3D12RootSignature* forwardedRoot=nullptr;
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator* a,ID3D12PipelineState* p) {
    forwardedAllocator=a;forwardedPSO=p;return resetHr;
}
void STDMETHODCALLTYPE pso(ID3D12GraphicsCommandList*,ID3D12PipelineState* p) {forwardedPSO=p;}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,UINT d) {++draws;drawArgs[0]=a;drawArgs[1]=b;drawArgs[2]=c;drawArgs[3]=d;}
void STDMETHODCALLTYPE indexed(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,INT d,UINT e) {++draws;drawArgs[0]=a;drawArgs[1]=b;drawArgs[2]=c;drawArgs[3]=UINT(d);drawArgs[4]=e;}
void STDMETHODCALLTYPE rootSet(ID3D12GraphicsCommandList*,ID3D12RootSignature* r) {forwardedRoot=r;}
void STDMETHODCALLTYPE heapSet(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*) {}
void STDMETHODCALLTYPE tableSet(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) {}
// Observable side effect keeps this same-signature competing setter distinct
// from the empty original under Release /OPT:ICF identical COMDAT folding.
static volatile unsigned foreignTableCalls=0;
void STDMETHODCALLTYPE foreignTableSet(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) {++foreignTableCalls;}
void STDMETHODCALLTYPE addressSet(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS) {}
void STDMETHODCALLTYPE constantSet(ID3D12GraphicsCommandList*,UINT i,UINT v,UINT o) {constantArg[0]=i;constantArg[1]=v;constantArg[2]=o;}
void STDMETHODCALLTYPE constantsSet(ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT) {}
void STDMETHODCALLTYPE indexSet(ID3D12GraphicsCommandList*,const D3D12_INDEX_BUFFER_VIEW*) {}
void STDMETHODCALLTYPE vertexSet(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*) {}
void STDMETHODCALLTYPE bundleSet(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*) {}
static uint64_t indirectOffsets[2]{};
void STDMETHODCALLTYPE indirectSet(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT count,ID3D12Resource*,UINT64 offset,ID3D12Resource*,UINT64 counter) {
    CHECK(count==3);indirectOffsets[0]=offset;indirectOffsets[1]=counter;
}
static unsigned protectCalls=0,failProtectCall=0;
static DWORD protectionSeen=0;
BOOL WINAPI protect(LPVOID,SIZE_T,DWORD requested,DWORD* old) {
    protectionSeen=requested;*old=PAGE_READONLY;
    return ++protectCalls!=failProtectCall;
}
HRESULT STDMETHODCALLTYPE noListDevice(ID3D12GraphicsCommandList*,REFIID,void** out){*out=nullptr;return E_NOINTERFACE;}
void STDMETHODCALLTYPE topologySet(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY) {}
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE directListType(ID3D12CommandList*) {return D3D12_COMMAND_LIST_TYPE_DIRECT;}
std::array<void*,60> listTable() {
    std::array<void*,60> t{};
    t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&add);t[2]=reinterpret_cast<void*>(&release);
    t[8]=reinterpret_cast<void*>(&directListType);t[7]=reinterpret_cast<void*>(&noListDevice);t[20]=reinterpret_cast<void*>(&topologySet);
    t[10]=reinterpret_cast<void*>(&reset);t[11]=t[25]=reinterpret_cast<void*>(&pso);
    t[12]=reinterpret_cast<void*>(&draw);t[13]=reinterpret_cast<void*>(&indexed);
    t[27]=reinterpret_cast<void*>(&bundleSet);t[28]=reinterpret_cast<void*>(&heapSet);t[30]=reinterpret_cast<void*>(&rootSet);
    t[32]=reinterpret_cast<void*>(&tableSet);t[34]=reinterpret_cast<void*>(&constantSet);t[36]=reinterpret_cast<void*>(&constantsSet);
    t[38]=t[40]=t[42]=reinterpret_cast<void*>(&addressSet);
    t[43]=reinterpret_cast<void*>(&indexSet);t[44]=reinterpret_cast<void*>(&vertexSet);t[59]=reinterpret_cast<void*>(&indirectSet);return t;
}
void installedCaptureTests() {
    acz::g_protect=protect;
    auto ta=listTable(),tb=listTable();
    Fake fa{ta.data()},fs{ta.data()},fb{tb.data()};
    auto a=reinterpret_cast<ID3D12GraphicsCommandList*>(&fa);
    auto shared=reinterpret_cast<ID3D12GraphicsCommandList*>(&fs);
    auto b=reinterpret_cast<ID3D12GraphicsCommandList*>(&fb);
    auto start=protectCalls;
    CHECK(acz::hookCaptureCommandList(a));
    const auto first=protectCalls-start;start=protectCalls;
    CHECK(acz::hookCaptureCommandList(shared));
    const auto sharedCalls=protectCalls-start;start=protectCalls;
    CHECK(acz::hookCaptureCommandList(b));
    const auto distinct=protectCalls-start;
    CHECK(first==38 && sharedCalls==0 && distinct==38);
    std::printf("Capture protection calls: first=%u shared=%u distinct=%u\n",first,sharedCalls,distinct);
    auto& hooks=acz::state().lists.at(ta.data());
    const auto originals=hooks.originals;
    const auto identity=acz::state().states.at(a).bindings.list_identity;
    CHECK(identity && acz::state().states.at(shared).bindings.list_identity!=identity);
    start=protectCalls;
    CHECK(acz::hookCommandList(a) && acz::hookCaptureCommandList(a) &&
          acz::hookCaptureCommandList(shared) && acz::hookCaptureCommandList(b));
    CHECK(protectCalls==start && hooks.originals==originals);
    CHECK(acz::state().states.at(a).bindings.list_identity==identity);
    acz::g_pinResult=false;
    CHECK(!acz::hookCaptureCommandList(a) && protectCalls==start);
    acz::g_pinResult=true;

    // A capture setter restoration failure cannot be hidden by installed draws.
    ta[28]=originals[28];failProtectCall=protectCalls+2;
    CHECK(!acz::hookCaptureCommandList(a) && protectCalls==start+2);
    CHECK(hooks.pendingProtection[28]==PAGE_READONLY);
    CHECK(!acz::state().states.at(a).capture_hooks && !acz::state().states.at(shared).capture_hooks);
    const auto& refused=acz::state().states.at(a);
    const auto& propagated=acz::state().states.at(shared);
    CHECK(!refused.bindings.list_identity&&!propagated.bindings.list_identity);
    CHECK(std::string_view(acz::listPointFailure_locked(refused,refused.bindings))==
        "rejected:list certification setter patch failed");
    CHECK(std::string_view(acz::listPointFailure_locked(propagated,propagated.bindings))==
        acz::diagnostics::shared_table_invalidation);
    CHECK(!freeze(refused.bindings,{},{} )&&!freeze(propagated.bindings,{},{}));
    CHECK(acz::hookCommandList(shared) && protectCalls==start+2);
    failProtectCall=protectCalls+1;
    CHECK(!acz::hookCaptureCommandList(a) && protectionSeen==PAGE_READONLY);
    start=protectCalls;failProtectCall=0;
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start+1);
    CHECK(!hooks.pendingProtection[28] && hooks.originals==originals);
    CHECK(acz::hookCaptureCommandList(shared) && protectCalls==start+1);

    // Partial setter installation retries only the missing slot.
    ta[40]=originals[40];start=protectCalls;failProtectCall=protectCalls+1;
    CHECK(!acz::hookCaptureCommandList(a) && ta[40]==originals[40]);
    failProtectCall=0;
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start+3);
    start=protectCalls;
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start);
    hooks.originals[30]=nullptr;
    CHECK(!acz::hookCaptureCommandList(a) && protectCalls==start);
    hooks.originals[30]=originals[30];
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start);
    // A foreign setter must be refused, preserving the true original dispatch.
    CHECK(reinterpret_cast<void*>(&foreignTableSet)!=originals[32]);
    foreignTableCalls=0;
    ta[32]=reinterpret_cast<void*>(&foreignTableSet);
    CHECK(!acz::hookCaptureCommandList(a) && protectCalls==start+2);
    CHECK(ta[32]==reinterpret_cast<void*>(&foreignTableSet) && hooks.originals==originals);
    a->SetGraphicsRootDescriptorTable(0,{123});
    CHECK(foreignTableCalls==1); // The competing setter remains callable.
    ta[32]=originals[32];
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start+4);
    CHECK(acz::hookCaptureCommandList(shared));

    // Restored lifetime prerequisites are checked even with all setters present.
    ta[2]=originals[2];start=protectCalls;
    CHECK(acz::hookCaptureCommandList(a) && protectCalls==start+2);
    CHECK(hooks.originals==originals);
    // Retained root cleanup remains outside the mutex on the installed path.
    auto tr=listTable();Fake root{tr.data()};
    auditMetadataCOM=true;
    a->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(&root));
    CHECK(acz::hookCaptureCommandList(a) && root.refs==2);
    CHECK(a->Reset(nullptr,nullptr)==S_OK && root.refs==1);
    a->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(&root));
    CHECK(a->Release()==0 && root.refs==1);
    CHECK(shared->Release()==0 && b->Release()==0);
    auditMetadataCOM=false;
    // Stack fixture tables must not leave dispatch records for reused addresses.
    acz::state().lists.erase(ta.data());acz::state().lists.erase(tb.data());
}
void hookTests() {
    acz::g_protect=protect;auto table=listTable();Fake fa{table.data()},fb{table.data()};
    auto a=reinterpret_cast<ID3D12GraphicsCommandList*>(&fa),b=reinterpret_cast<ID3D12GraphicsCommandList*>(&fb);
    CHECK(acz::hookCaptureCommandList(a) && acz::hookCaptureCommandList(b));
    auto objectTable=listTable();Fake root1{objectTable.data()},root2{objectTable.data()},heap1{objectTable.data()},heap2{objectTable.data()};
    auto r1=reinterpret_cast<ID3D12RootSignature*>(&root1),r2=reinterpret_cast<ID3D12RootSignature*>(&root2);
    RootLayout layout;RootParameter srv;srv.kind=ParameterKind::srv;srv.shader_register=10;layout.parameters={srv};
    acz::state().roots.emplace(r1,RootBindingMetadata::from_adapter(layout));
    acz::state().roots.emplace(r2,RootBindingMetadata::from_adapter(layout));
    a->SetGraphicsRootSignature(r1);b->SetGraphicsRootSignature(r2);
    ID3D12DescriptorHeap* heaps[]={reinterpret_cast<ID3D12DescriptorHeap*>(&heap1)};a->SetDescriptorHeaps(1,heaps);
    a->SetGraphicsRootDescriptorTable(0,{123});a->SetGraphicsRootConstantBufferView(1,111);
    a->SetGraphicsRootShaderResourceView(2,222);a->SetGraphicsRootUnorderedAccessView(3,333);
    a->SetGraphicsRoot32BitConstant(4,77,1);CHECK(constantArg[0]==4 && constantArg[1]==77 && constantArg[2]==1);
    uint32_t words[]={88,99};a->SetGraphicsRoot32BitConstants(4,2,words,2);words[0]=0;
    D3D12_INDEX_BUFFER_VIEW ib{0x2007,40,DXGI_FORMAT_R32_UINT};a->IASetIndexBuffer(&ib);
    D3D12_VERTEX_BUFFER_VIEW vb{0x1009,80,16};a->IASetVertexBuffers(2,1,&vb);
    std::shared_ptr<const State> observed;Command command;
    acz::setBindingCallback([&](std::shared_ptr<const State> s,Command c){observed=std::move(s);command=c;});
    a->DrawIndexedInstanced(5,2,3,-7,19);
    CHECK(draws==1 && drawArgs[0]==5 && drawArgs[1]==2 && drawArgs[2]==3 && INT(drawArgs[3])==-7 && drawArgs[4]==19);
    CHECK(command.indexed && command.base_vertex==-7 && command.start_instance==19);
    CHECK(observed->root_id==reinterpret_cast<uintptr_t>(r1) && observed->roots.at(4).constants[2]==88);
    CHECK(observed->roots.at(2).value==222 && observed->roots.at(3).kind==ParameterKind::uav);
    CHECK(observed->index->resource==0x2007 && observed->vertices.at(2).resource==0x1009);
    const auto saved=observed;auto generation=saved->generation;auto identity=saved->list_identity;
    CHECK(!freeze(*saved,command,Inputs{})); // Shader ID/state alone cannot be a cockpit snapshot.
    resetHr=E_FAIL;CHECK(a->Reset(nullptr,nullptr)==E_FAIL);a->DrawInstanced(6,1,9,12);CHECK(observed->generation==generation && observed->roots.size()==saved->roots.size());
    resetHr=S_OK;CHECK(a->Reset(nullptr,nullptr)==S_OK);a->DrawInstanced(6,1,9,12);
    CHECK(observed->generation==generation+1 && observed->list_identity==identity && observed->roots.empty() && !observed->root);
    CHECK(!command.indexed && command.start==9 && command.start_instance==12 && drawArgs[2]==9 && drawArgs[3]==12);
    CHECK(saved->roots.at(4).constants[2]==88 && saved->index->resource==0x2007);
    b->DrawInstanced(1,1,0,0);CHECK(observed->root_id==reinterpret_cast<uintptr_t>(r2));
    CHECK(observed->list_identity!=identity);
    a->SetGraphicsRootSignature(r1);a->SetGraphicsRootDescriptorTable(0,{456});
    heaps[0]=reinterpret_cast<ID3D12DescriptorHeap*>(&heap2);a->SetDescriptorHeaps(1,heaps);a->DrawInstanced(1,1,0,0);
    CHECK(!observed->roots.count(0));
    a->ExecuteBundle(b);a->DrawInstanced(1,1,0,0);CHECK(!observed->tracking_complete);
    CHECK(acz::hookCaptureCommandList(a));a->DrawInstanced(1,1,0,0);CHECK(!observed->tracking_complete);
    CHECK(a->Reset(nullptr,nullptr)==S_OK);a->DrawInstanced(1,1,0,0);CHECK(observed->tracking_complete);
    a->ExecuteIndirect(nullptr,3,nullptr,13,nullptr,29);a->DrawInstanced(1,1,0,0);
    CHECK(!observed->tracking_complete && indirectOffsets[0]==13 && indirectOffsets[1]==29);
    CHECK(a->Reset(nullptr,nullptr)==S_OK);
    a->SetGraphicsRootShaderResourceView(70,123);a->DrawInstanced(1,1,0,0);CHECK(!observed->tracking_complete);
    a->ClearState(nullptr);a->DrawInstanced(1,1,0,0);CHECK(observed->tracking_complete && observed->roots.empty());
    CHECK(a->Release()==0 && b->Release()==0);
    CHECK(root1.refs==1 && root2.refs==1 && heap1.refs==1 && heap2.refs==1);
    fa.refs=1;CHECK(acz::hookCaptureCommandList(a));a->DrawInstanced(1,1,0,0);CHECK(observed->roots.empty() && !observed->root);
    CHECK(observed->list_identity && observed->list_identity!=identity && saved->list_identity==identity);
    CHECK(a->Release()==0);acz::setBindingCallback({});acz::state().roots.clear();
}
static Fake* createdObject=nullptr;
static HRESULT createHr=S_OK;
static IID expectedIID=__uuidof(IUnknown);
static UINT expectedNode=7;
static ID3D12Device* expectedRootDevice=nullptr;
static const void* expectedBytes=nullptr;
static SIZE_T expectedLength=0;
static bool transferRootReference=false;
static std::function<void()> rootCreationAction;
HRESULT STDMETHODCALLTYPE createRootFake(ID3D12Device* self,UINT node,const void* bytes,SIZE_T length,REFIID iid,void** out) {
    CHECK(self==expectedRootDevice && node==expectedNode && bytes==expectedBytes && length==expectedLength && iid==expectedIID);
    if(rootCreationAction) {auto action=std::move(rootCreationAction);rootCreationAction={};action();}
    if(SUCCEEDED(createHr)) {if(transferRootReference) add(reinterpret_cast<IUnknown*>(createdObject));*out=createdObject;}return createHr;
}
static const D3D12_GRAPHICS_PIPELINE_STATE_DESC* expectedDesc=nullptr;
static ID3D12PipelineState* expectedInitial=nullptr;
HRESULT STDMETHODCALLTYPE createPSOFake(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
    CHECK(desc==expectedDesc && iid==expectedIID);if(SUCCEEDED(createHr)) *out=createdObject;return createHr;
}
HRESULT STDMETHODCALLTYPE createListFake(ID3D12Device*,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* alloc,
                                        ID3D12PipelineState* initial,REFIID iid,void** out) {
    CHECK(node==expectedNode && type==D3D12_COMMAND_LIST_TYPE_DIRECT && !alloc && initial==expectedInitial && iid==expectedIID);
    if(SUCCEEDED(createHr)) *out=createdObject;return createHr;
}
void put(std::vector<uint8_t>& v,size_t at,uint32_t n) {for(int i=0;i<4;++i)v[at+i]=uint8_t(n>>(i*8));}
void tag(std::vector<uint8_t>& v,size_t at,const char* text) {std::copy_n(text,4,v.begin()+at);}
std::vector<uint8_t> shaderFixture() {
    // Structural metadata only, never executable or authenticated shader bytes.
    std::vector<uint8_t> v(104);tag(v,0,"DXBC");put(v,20,1);put(v,24,104);put(v,28,2);put(v,32,40);put(v,36,76);
    tag(v,40,"DXIL");put(v,44,28);put(v,48,(2u<<16)|0x60);put(v,52,7);tag(v,56,"DXIL");put(v,60,0x100);
    put(v,64,16);put(v,68,4);put(v,72,0xdec04342);tag(v,76,"HASH");put(v,80,20);
    std::copy(drawcpt_geometry_identifier.begin(),drawcpt_geometry_identifier.end(),v.begin()+88);return v;
}
// SDK CPU exports only. A synthetic shader-style DXBC container carries the
// SDK's actual RTS0 payload alongside DXIL/HASH parts, as in the game VS shape.
// DXIL bytes are structural padding, never executable shader evidence.
void smallSerializedRootTests() {
    using F=acz::metadata::SerializedRootFailure;
    for(auto flags:{D3D12_ROOT_SIGNATURE_FLAG_NONE,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
        static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x30d),
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED}) {
        D3D12_ROOT_SIGNATURE_DESC desc{0,nullptr,0,nullptr,flags};
        ID3DBlob* blob=nullptr;
        CHECK(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr))&&blob);
        const auto* b=static_cast<const uint8_t*>(blob->GetBufferPointer());const auto n=blob->GetBufferSize();
        auto word=[](const uint8_t* v,size_t at){return uint32_t(v[at])|(uint32_t(v[at+1])<<8)|(uint32_t(v[at+2])<<16)|(uint32_t(v[at+3])<<24);};
        CHECK(n==68&&word(b,28)==1);const auto offset=word(b,32);
        CHECK(offset+8<=n&&word(b,offset)==0x30535452u&&word(b,offset+4)==24);
        // Fixed 184 bytes: 44 directory + 80 DXIL + 28 HASH + 32 RTS0.
        std::vector<uint8_t> container(184);tag(container,0,"DXBC");put(container,20,1);
        put(container,24,184);put(container,28,3);put(container,32,44);put(container,36,124);put(container,40,152);
        tag(container,44,"DXIL");put(container,48,72);put(container,52,(1u<<16)|0x60);
        put(container,56,18);tag(container,60,"DXIL");put(container,64,0x100);put(container,68,16);put(container,72,48);
        tag(container,124,"HASH");put(container,128,20);
        std::copy_n(b+offset,32,container.begin()+152);
        const bool accepted=flags!=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        for(bool wrapped:{false,true}) {
            const void* bytes=wrapped?static_cast<const void*>(container.data()):b;
            const auto length=wrapped?container.size():n;
            ID3D12VersionedRootSignatureDeserializer* sdk=nullptr;
            CHECK(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(bytes,length,IID_PPV_ARGS(&sdk)))&&sdk);
            const auto* parsed=sdk->GetUnconvertedRootSignatureDesc();
            CHECK(parsed&&parsed->Version==D3D_ROOT_SIGNATURE_VERSION_1_0&&parsed->Desc_1_0.NumParameters==0&&parsed->Desc_1_0.NumStaticSamplers==0);
            auto decoded=acz::metadata::RootBindingMetadata::from_serialized(bytes,length);
            CHECK(decoded.valid()==accepted&&decoded.failure==(accepted?F::none:F::unsupported_layout));
            if(accepted) {
                CHECK(decoded.metadata->layout().parameters.empty()&&decoded.metadata->layout().flags==uint32_t(flags));
                CHECK(!decoded.metadata->resolve(acz::metadata::BindingKind::srv,0,0,acz::metadata::ShaderStage::vertex).valid);
            } else CHECK(decoded.detail=="unsupported root signature flags");
            std::printf("empty root flags=0x%x bytes=%zu sdk=accepted adapter=%u\n",unsigned(flags),length,unsigned(decoded.failure));
            sdk->Release();
        }
        // Size alone does not select a gate: the same 184-byte class can reach
        // header_bounds or sdk_rejected, rather than unsupported_layout.
        put(container,24,185);
        CHECK(acz::metadata::RootBindingMetadata::from_serialized(container.data(),184).failure==F::header_bounds);
        container.assign(184,0);
        CHECK(acz::metadata::RootBindingMetadata::from_serialized(container.data(),184).failure==F::sdk_rejected);
        blob->Release();
    }
}
void creationTests() {
    auditMetadataCOM=true;
    std::array<void*,45> deviceTable{};deviceTable[0]=reinterpret_cast<void*>(&query);deviceTable[1]=reinterpret_cast<void*>(&add);deviceTable[2]=reinterpret_cast<void*>(&release);deviceTable[10]=reinterpret_cast<void*>(&createPSOFake);
    deviceTable[12]=reinterpret_cast<void*>(&createListFake);deviceTable[16]=reinterpret_cast<void*>(&createRootFake);
    Fake device{deviceTable.data()};auto d=reinterpret_cast<ID3D12Device*>(&device);expectedRootDevice=d;CHECK(acz::hookCaptureDevice(d));
    auto rootTable=listTable();Fake root{rootTable.data()};createdObject=&root;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,10,0,2};
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_GEOMETRY;parameter.DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC desc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* bytes=nullptr;CHECK(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr)));
    expectedBytes=bytes->GetBufferPointer();expectedLength=bytes->GetBufferSize();void* out=nullptr;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK && out==&root);
    auto r=reinterpret_cast<ID3D12RootSignature*>(&root);
    CHECK(acz::state().roots.at(r).resolve_srt_data().table_offset==2);
    const auto rootIdentity=acz::state().rootIdentities.at(r);CHECK(rootIdentity);
    auto equivalentTable=listTable();Fake equivalent{equivalentTable.data()};createdObject=&equivalent;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    auto er=reinterpret_cast<ID3D12RootSignature*>(&equivalent);
    CHECK(acz::state().roots.at(er).resolve_srt_data().table_offset==2);
    CHECK(acz::state().rootIdentities.at(er)!=rootIdentity);
    auto aliasTable=listTable();Fake alias{aliasTable.data()},aliasIdentity{aliasTable.data()};
    createdObject=&alias;queryAlias=reinterpret_cast<IUnknown*>(&aliasIdentity);
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK && out==&alias);
    auto ar=reinterpret_cast<ID3D12RootSignature*>(&alias);
    CHECK(!acz::state().rootIdentities.count(ar) && !acz::state().roots.count(ar));
    queryAlias=nullptr;CHECK(alias.refs==1 && aliasIdentity.refs==1);
    std::vector<uint8_t> rootBytes(static_cast<const uint8_t*>(expectedBytes),static_cast<const uint8_t*>(expectedBytes)+expectedLength);
    bytes->Release();expectedBytes=nullptr;expectedLength=0;
    auto psoTable=listTable();Fake pipeline{psoTable.data()};createdObject=&pipeline;
    auto shader=shaderFixture();D3D12_GRAPHICS_PIPELINE_STATE_DESC pdesc{};pdesc.GS={shader.data(),shader.size()};pdesc.pRootSignature=r;
    pdesc.Flags=D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG;expectedDesc=&pdesc;
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && out==&pipeline);
    auto p=reinterpret_cast<ID3D12PipelineState*>(&pipeline);
    CHECK(acz::state().pipelines.at(p).shader.matches_drawcpt_geometry() && root.refs==2);
    // Pre-seed a stale entry at the next created PSO's exact address. Its owned
    // root has hooked Release, so replacement must dispose it outside the lock.
    int displacedReleases=0;
    auto& stale=acz::state().pipelines.at(p);stale.shader={};
    r->AddRef();
    stale.root=std::shared_ptr<ID3D12RootSignature>(r,[&](ID3D12RootSignature* value) {
        bool unlocked=false;
        std::thread probe([&] {if(acz::state().mutex.try_lock()) {unlocked=true;acz::state().mutex.unlock();}});
        probe.join();CHECK(unlocked); // Fail promptly before a re-entrant Release.
        CHECK(acz::vtable(value)[2]==reinterpret_cast<void*>(&acz::releaseMetadata));
        ++displacedReleases;value->Release();
    });
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && out==&pipeline);
    CHECK(displacedReleases==1 && root.refs==2 && pipeline.refs==1);
    CHECK(acz::state().pipelines.at(p).shader.matches_drawcpt_geometry());
    queryFailure=true;
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && out==&pipeline);
    CHECK(!acz::state().pipelines.count(p) && root.refs==1 && pipeline.refs==1);
    queryFailure=false;
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && root.refs==2);
    shader.clear();shader.shrink_to_fit();CHECK(acz::state().pipelines.at(p).shader.matches_drawcpt_geometry());
    // Decoded metadata owns its lifetime; the descriptor must not retain freed input.
    pdesc.GS={nullptr,0};
    auto table=listTable();Fake list{table.data()};auto l=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    createdObject=&list;expectedInitial=p;
    CHECK(d->CreateCommandList(7,D3D12_COMMAND_LIST_TYPE_DIRECT,nullptr,p,expectedIID,&out)==S_OK && out==&list);
    l->SetGraphicsRootSignature(r);
    std::shared_ptr<const State> observed;
    acz::setBindingCallback([&](std::shared_ptr<const State> s,Command){observed=std::move(s);});
    l->DrawInstanced(1,1,0,0);CHECK(observed->pipeline.drawcpt && !observed->pipeline.requirements_proven && observed->root);
    CHECK(observed->compatible_root() && observed->root_identity==rootIdentity && forwardedRoot==r);
    const auto compatibleSnapshot=observed;
    const auto pipelineIdentity=observed->pipeline.identity;
    // A second successful creation transfers a NEW caller reference to the
    // already-live canonical root retained by both the PSO and command list.
    createdObject=&root;transferRootReference=true;
    expectedBytes=rootBytes.data();expectedLength=rootBytes.size();
    const auto retainedRefs=root.refs;createHr=S_FALSE;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_FALSE && out==&root);
    CHECK(root.refs==retainedRefs+1);CHECK(r->Release()==retainedRefs);createHr=S_OK;
    CHECK(acz::state().rootIdentities.at(r)==rootIdentity);
    l->DrawInstanced(1,1,0,0); // Existing bound list stays compatible without rebinding.
    CHECK(observed->compatible_root() && observed->pipeline.explicit_root_identity==rootIdentity);
    CHECK(compatibleSnapshot->root_identity==rootIdentity);
    l->SetGraphicsRootSignature(r);l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root());
    transferRootReference=false;
    l->SetGraphicsRootSignature(er);l->DrawInstanced(1,1,0,0);
    CHECK(!observed->compatible_root() && forwardedRoot==er);
    l->SetGraphicsRootSignature(ar);l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
    l->SetGraphicsRootSignature(nullptr);l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
    l->SetGraphicsRootSignature(r);
    l->SetPipelineState(nullptr);l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root() && !forwardedPSO);
    l->SetPipelineState(p);l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root() && forwardedPSO==p);
    auto allocator=reinterpret_cast<ID3D12CommandAllocator*>(uintptr_t(0x1234));
    const auto generation=observed->generation;
    resetHr=E_FAIL;CHECK(l->Reset(allocator,nullptr)==E_FAIL && forwardedAllocator==allocator && !forwardedPSO);
    l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root() && observed->generation==generation);
    resetHr=S_OK;CHECK(l->Reset(allocator,p)==S_OK && forwardedPSO==p);
    l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root() && observed->generation==generation+1);
    l->SetGraphicsRootSignature(r);l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root());
    l->ClearState(p);l->DrawInstanced(1,1,0,0);
    CHECK(!observed->compatible_root() && forwardedPSO==p);
    l->SetGraphicsRootSignature(r);l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root());
    // Success is necessary only: native all-stage/copy/index proofs stay absent.
    Inputs nativeInputs;nativeInputs.list_identity=observed->list_identity;
    nativeInputs.generation=observed->generation;nativeInputs.revision=observed->revision;
    CHECK(!freeze(*observed,Command{false,1,1},nativeInputs));
    CHECK(!observed->pipeline.requirements_proven);
    // A PSO with an embedded/null root remains unsupported even with the same GS.
    auto embeddedTable=listTable();Fake embedded{embeddedTable.data()};createdObject=&embedded;
    const auto embeddedShader=shaderFixture();
    pdesc.GS={embeddedShader.data(),embeddedShader.size()};pdesc.pRootSignature=nullptr;
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK);
    auto ep=reinterpret_cast<ID3D12PipelineState*>(&embedded);
    l->SetPipelineState(ep);l->DrawInstanced(1,1,0,0);
    CHECK(observed->pipeline.drawcpt && !observed->compatible_root() && !observed->pipeline.explicit_root_identity);
    l->SetPipelineState(p);CHECK(ep->Release()==0);pdesc.pRootSignature=r;
    // Unsupported SM6.1 creation still returns the ORIGINAL success result.
    auto otherTable=listTable();Fake newer{otherTable.data()};createdObject=&newer;
    auto unsupportedShader=shaderFixture();put(unsupportedShader,48,(2u<<16)|0x61);
    pdesc.GS={unsupportedShader.data(),unsupportedShader.size()};
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && out==&newer);
    auto np=reinterpret_cast<ID3D12PipelineState*>(&newer);CHECK(!acz::state().pipelines.at(np).shader.valid);
    l->SetPipelineState(np);l->DrawInstanced(1,1,0,0);CHECK(!observed->pipeline.drawcpt);
    // Failure/conflict fixtures use newly transferred references, never steal
    // the PSO/list owners. Historical snapshots retain the originally proven ID.
    createdObject=&root;transferRootReference=true;
    expectedBytes=rootBytes.data();expectedLength=rootBytes.size();
    l->SetPipelineState(p);l->SetGraphicsRootSignature(r);
    auto repeatRoot=[&] {
        const auto refs=root.refs;void* result=nullptr;
        CHECK(d->CreateRootSignature(expectedNode,expectedBytes,expectedLength,expectedIID,&result)==S_OK && result==&root);
        CHECK(root.refs==refs+1);CHECK(r->Release()==refs);
    };
    auto trackedRoot=[&] {
        expectedNode=7;expectedIID=__uuidof(IUnknown);
        expectedBytes=rootBytes.data();expectedLength=rootBytes.size();repeatRoot();
        CHECK(acz::state().rootIdentities.count(r));return acz::state().rootIdentities.at(r);
    };
    createHr=E_FAIL;out=reinterpret_cast<void*>(0x123);const auto failureRefs=root.refs;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==E_FAIL && out==reinterpret_cast<void*>(0x123));
    CHECK(root.refs==failureRefs && acz::state().rootIdentities.at(r)==rootIdentity);createHr=S_OK;
    l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root());
    // The input buffer differs from the first one; matching OWNED bytes suffice.
    CHECK(acz::state().rootCreations.at(r).bytes==rootBytes);
    Fake anotherDevice{deviceTable.data()};auto otherDevice=reinterpret_cast<ID3D12Device*>(&anotherDevice);
    expectedRootDevice=otherDevice;const auto contextRefs=root.refs;
    CHECK(otherDevice->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK && out==&root);
    CHECK(root.refs==contextRefs+1 && acz::state().rootIdentities.at(r)!=rootIdentity);CHECK(r->Release()==contextRefs);
    expectedRootDevice=d;trackedRoot();
    expectedNode=8;repeatRoot();CHECK(acz::state().rootIdentities.at(r)!=rootIdentity);
    l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
    auto token=trackedRoot();expectedIID=__uuidof(ID3D12RootSignature);repeatRoot();
    CHECK(acz::state().rootIdentities.at(r)!=token);
    token=trackedRoot();
    // An identical address/token lacking owned creation history cannot preserve.
    acz::state().rootCreations.erase(r);repeatRoot();
    CHECK(acz::state().rootIdentities.at(r)!=token);
    trackedRoot();acz::g_rootMetadataAllocation=false;repeatRoot();acz::g_rootMetadataAllocation=true;
    CHECK(!acz::state().rootIdentities.count(r) && !acz::state().rootCreations.count(r));
    for(unsigned stage=1;stage<=2;++stage) {
        trackedRoot();acz::g_rootPublishFailure=stage;repeatRoot();acz::g_rootPublishFailure=0;
        CHECK(!acz::state().rootIdentities.count(r) && !acz::state().roots.count(r) && !acz::state().rootCreations.count(r));
    }
    trackedRoot();expectedIID=__uuidof(ID3D12Device);repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();queryCanonicalFailure=true;repeatRoot();queryCanonicalFailure=false;
    CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();queryRootAlias=reinterpret_cast<IUnknown*>(er);repeatRoot();queryRootAlias=nullptr;
    CHECK(!acz::state().rootIdentities.count(r) && !acz::state().rootIdentities.count(er));
    trackedRoot();queryFailure=true;repeatRoot();queryFailure=false;
    CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();queryAlias=reinterpret_cast<IUnknown*>(&aliasIdentity);repeatRoot();queryAlias=nullptr;
    CHECK(!acz::state().rootIdentities.count(r) && aliasIdentity.refs==1);
    trackedRoot();expectedLength=max_copy_bytes+1;repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();expectedBytes=nullptr;expectedLength=0;repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();uint8_t badRoot[]={1,2,3,4};expectedBytes=badRoot;expectedLength=sizeof(badRoot);repeatRoot();
    CHECK(!acz::state().rootIdentities.count(r));
    // Valid serialized bytes with a different range offset conflict, even on
    // this same still-live canonical object. No semantic equivalence is inferred.
    trackedRoot();range.OffsetInDescriptorsFromTableStart=5;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr)));
    token=acz::state().rootIdentities.at(r);expectedBytes=bytes->GetBufferPointer();expectedLength=bytes->GetBufferSize();
    repeatRoot();CHECK(acz::state().rootIdentities.at(r)!=token);bytes->Release();range.OffsetInDescriptorsFromTableStart=2;
    // Fresh publication during known, distinct observation events. Native
    // creation output owns a reference before QI reentry; no driver/GPU involved.
    auto freshTableA=listTable(),freshTableB=listTable();
    Fake freshA{freshTableA.data()},freshB{freshTableB.data()};
    auto freshAR=reinterpret_cast<ID3D12RootSignature*>(&freshA);
    auto freshBR=reinterpret_cast<ID3D12RootSignature*>(&freshB);
    auto freshCreate=[&](Fake& value) {
        createdObject=&value;void* result=nullptr;const auto refs=value.refs;
        CHECK(d->CreateRootSignature(expectedNode,expectedBytes,expectedLength,expectedIID,&result)==S_OK && result==&value);
        CHECK(value.refs==refs+1);CHECK(reinterpret_cast<IUnknown*>(&value)->Release()==refs);
    };
    trackedRoot();
    acz::g_rootReady=[&] {
        std::thread overlap([&] {freshCreate(freshB);});overlap.join();
        CHECK(acz::state().rootIdentities.count(freshBR));
    };
    freshCreate(freshA);
    CHECK(acz::state().rootIdentities.count(freshAR));
    const auto freshAToken=acz::state().rootIdentities.at(freshAR);
    const auto freshBToken=acz::state().rootIdentities.at(freshBR);
    CHECK(freshAToken && freshBToken && freshAToken!=freshBToken && freshAToken!=rootIdentity && freshBToken!=rootIdentity);
    // A concurrently accepted fresh identity must work through the actual PSO
    // and list path, without supplying any of the missing native copy proofs.
    auto concurrentPSOTable=listTable();Fake concurrentPSO{concurrentPSOTable.data()};
    auto cp=reinterpret_cast<ID3D12PipelineState*>(&concurrentPSO);
    auto concurrentShader=shaderFixture();pdesc.GS={concurrentShader.data(),concurrentShader.size()};
    createdObject=&concurrentPSO;pdesc.pRootSignature=freshAR;
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK && out==&concurrentPSO);
    l->SetPipelineState(cp);l->SetGraphicsRootSignature(freshAR);l->DrawInstanced(1,1,0,0);
    CHECK(observed->pipeline.drawcpt && observed->compatible_root() && observed->root_identity==freshAToken);
    CHECK(!observed->pipeline.requirements_proven);
    CHECK(!freeze(*observed,Command{false,1,1},Inputs{}));
    l->SetPipelineState(p);l->SetGraphicsRootSignature(r);CHECK(cp->Release()==0);
    pdesc.pRootSignature=r;
    // Actual patched non-final metadata Release wraps native creation. The new
    // root identity must work through a PSO and list, while freeze stays closed.
    CHECK(freshBR->Release()==0);freshB.refs=2;
    concurrentPSO.refs=1;
    nonFinalReleaseAction=[&] {
        freshCreate(freshB);
        const auto nestedToken=acz::state().rootIdentities.at(freshBR);
        CHECK(nestedToken && nestedToken!=freshBToken);
        createdObject=&concurrentPSO;pdesc.pRootSignature=freshBR;
        CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK&&out==&concurrentPSO);
        l->SetPipelineState(cp);l->SetGraphicsRootSignature(freshBR);l->DrawInstanced(1,1,0,0);
        CHECK(observed->pipeline.drawcpt&&observed->compatible_root()&&observed->root_identity==nestedToken);
        CHECK(!freeze(*observed,Command{false,1,1},Inputs{}));
        l->SetPipelineState(p);l->SetGraphicsRootSignature(r);CHECK(cp->Release()==0);
        pdesc.pRootSignature=r;
    };
    CHECK(freshBR->Release()==1&&acz::state().rootIdentities.count(freshBR));
    auto freshTableC=listTable();Fake freshC{freshTableC.data()};
    auto freshCR=reinterpret_cast<ID3D12RootSignature*>(&freshC);
    auto unrelatedTable=listTable();Fake unrelated{unrelatedTable.data()};
    auto unrelatedObject=reinterpret_cast<IUnknown*>(&unrelated);CHECK(acz::trackMetadataLifetime(unrelatedObject));
    finalReleaseAction=[&] {std::thread worker([&]{freshCreate(freshC);});worker.join();};CHECK(unrelatedObject->Release()==0);
    CHECK(acz::state().rootIdentities.count(freshCR) && acz::state().rootIdentities.at(freshCR)!=freshAToken && acz::state().rootIdentities.at(freshCR)!=freshBToken);
    // A failing older, distinct creator cannot erase the successfully
    // published sibling, including a newly bound list record.
    CHECK(freshAR->Release()==0 && freshBR->Release()==0 && freshCR->Release()==0);
    freshA.refs=freshB.refs=1;
    acz::g_rootReady=[&] {
        std::thread worker([&]{freshCreate(freshB);});worker.join();l->SetGraphicsRootSignature(freshBR);
        CHECK(acz::state().states.at(l).bindings.root_identity==acz::state().rootIdentities.at(freshBR));
        acz::g_rootPublishFailure=2;
    };
    freshCreate(freshA);acz::g_rootPublishFailure=0;
    CHECK(!acz::state().rootIdentities.count(freshAR));
    CHECK(acz::state().rootIdentities.count(freshBR));
    const auto successfulSibling=acz::state().rootIdentities.at(freshBR);
    CHECK(successfulSibling!=freshBToken && acz::state().states.at(l).bindings.root_identity==successfulSibling);
    // Direct bookkeeping seam probes rollback ownership and late retirement
    // ambiguity. These synthetic records are NOT a runtime ordering claim.
    const auto olderPublication=acz::state().rootCreations.at(freshBR).publication-1;
    const auto siblingRevision=acz::state().states.at(l).bindings.revision;
    {acz::RootCreationInvalidation older{freshBR,olderPublication};}
    CHECK(acz::state().rootIdentities.at(freshBR)==successfulSibling &&
          acz::state().states.at(l).bindings.revision==siblingRevision);
    {
        acz::RootCreationEvent publishing;
        {auto& s=acz::state();std::lock_guard lock(s.mutex);
         publishing.identify(reinterpret_cast<IUnknown*>(freshBR));
         s.rootCreations.at(freshBR).publication=publishing.serial;}
        // Returned-zero Release at this address taints publication. Non-final
        // Release no longer opens a metadata observation across native calls.
        CHECK(freshBR->Release()==1);CHECK(freshBR->Release()==0);
        CHECK(publishing.ambiguous);
    }
    CHECK(!acz::state().rootIdentities.count(freshBR) &&
          !acz::state().states.at(l).bindings.root_identity);
    l->SetGraphicsRootSignature(r);
    CHECK(freshAR->Release()==0);freshB.refs=1;
    // Unresolved native output ordering remains unsupported even for roots
    // which eventually turn out distinct. Both must remain tokenless.
    freshA.refs=freshB.refs=1;
    rootCreationAction=[&] {freshCreate(freshB);createdObject=&freshA;};
    freshCreate(freshA);
    CHECK(!acz::state().rootIdentities.count(freshAR) && !acz::state().rootIdentities.count(freshBR));
    CHECK(freshAR->Release()==0 && freshBR->Release()==0);
    // Same-address final Release/reuse cannot publish a guessed new lifetime.
    freshA.refs=1;freshCreate(freshA);
    const auto retiringFreshToken=acz::state().rootIdentities.at(freshAR);
    finalReleaseAction=[&] {
        freshA.refs=1;freshCreate(freshA);
        // Retirement is known only after native return; then it invalidates
        // this publication. No live-runtime ordering is inferred here.
        CHECK(acz::state().rootIdentities.count(freshAR));
    };
    CHECK(freshAR->Release()==0 && !acz::state().rootIdentities.count(freshAR));
    freshCreate(freshA);
    CHECK(acz::state().rootIdentities.at(freshAR)!=retiringFreshToken);
    CHECK(freshAR->Release()==0);
    // Known same-address creations taint both active observations. Unlike the
    // native-output reentry tests below, both outputs are already identified.
    rootQueryAction=[&] {repeatRoot();};createdObject=&root;
    repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    createdObject=&root;
    trackedRoot();
    rootCreationAction=[&] {
        l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
        repeatRoot(); // Reentry is ambiguous even though bytes/address agree.
    };repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    trackedRoot();rootCreationAction=[&] {
        std::thread overlap([&] {repeatRoot();});overlap.join();
    };repeatRoot();CHECK(!acz::state().rootIdentities.count(r));
    // An unrelated final metadata Release has no root footprint conflict;
    // creation inside it is an ordinary independent event.
    trackedRoot();auto retireTable=listTable();Fake retiring{retireTable.data()};
    auto retireObject=reinterpret_cast<IUnknown*>(&retiring);CHECK(acz::trackMetadataLifetime(retireObject));
    finalReleaseAction=[&] {repeatRoot();};CHECK(retireObject->Release()==0);
    CHECK(acz::state().rootIdentities.count(r));
    token=trackedRoot();CHECK(token && token!=rootIdentity);
    l->SetGraphicsRootSignature(r);l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
    CHECK(compatibleSnapshot->compatible_root() && compatibleSnapshot->root_identity==rootIdentity);
    transferRootReference=false;l->SetPipelineState(np);
    CHECK(p->Release()==0 && !acz::state().pipelines.count(p));
    const auto retiringListToken=acz::state().states.at(l).bindings.list_identity;
    finalReleaseAction=[&] {
        list.refs=1;auto savedObject=createdObject;auto savedInitial=expectedInitial;auto savedIID=expectedIID;
        createdObject=&list;expectedInitial=nullptr;expectedIID=__uuidof(ID3D12GraphicsCommandList);
        void* replacement=nullptr;
        CHECK(d->CreateCommandList(expectedNode,D3D12_COMMAND_LIST_TYPE_DIRECT,nullptr,nullptr,expectedIID,&replacement)==S_OK&&replacement==l);
        CHECK(acz::state().states.at(l).bindings.list_identity!=retiringListToken);
        createdObject=savedObject;expectedInitial=savedInitial;expectedIID=savedIID;
    };
    CHECK(l->Release()==0&&acz::state().states.count(l)); // outer retirement preserves the replacement
    CHECK(l->Release()==0 && np->Release()==0 && root.refs==1);
    CHECK(er->Release()==0 && !acz::state().rootIdentities.count(er));
    CHECK(r->Release()==0 && !acz::state().roots.count(r));
    CHECK(!acz::state().rootIdentities.count(r));
    CHECK(compatibleSnapshot->compatible_root() && compatibleSnapshot->root_identity==rootIdentity &&
          compatibleSnapshot->pipeline.identity==pipelineIdentity);
    acz::setBindingCallback({});
    // Exact same bytes at the exact address AFTER final Release get a new ID.
    root.refs=1;createdObject=&root;expectedBytes=rootBytes.data();expectedLength=rootBytes.size();
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK && out==&root);
    CHECK(acz::state().rootIdentities.at(r)!=token && acz::state().rootIdentities.at(r)!=rootIdentity);
    CHECK(r->Release()==0 && !acz::state().rootCreations.count(r));
    // Documented versioned CPU deserializer adapter, including range flags.
    root.refs=1;createdObject=&root;
    D3D12_DESCRIPTOR_RANGE1 range1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,10,0,D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,3};
    D3D12_ROOT_PARAMETER1 param1{};param1.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param1.ShaderVisibility=D3D12_SHADER_VISIBILITY_GEOMETRY;param1.DescriptorTable={1,&range1};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc{};vdesc.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
    vdesc.Desc_1_1={1,&param1,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    CHECK(SUCCEEDED(D3D12SerializeVersionedRootSignature(&vdesc,&bytes,nullptr)));
    expectedBytes=bytes->GetBufferPointer();expectedLength=bytes->GetBufferSize();
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    CHECK(acz::state().roots.at(r).layout().version==RootVersion::v1_1 && acz::state().roots.at(r).resolve_srt_data().table_offset==3);
    CHECK(acz::state().rootIdentities.at(r)!=rootIdentity);
    bytes->Release();CHECK(r->Release()==0);
    // The retiring root token cannot erase a reentrant new same-address token.
    root.refs=1;createdObject=&root;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr)));
    expectedBytes=bytes->GetBufferPointer();expectedLength=bytes->GetBufferSize();
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    bytes->Release();const auto newerRootIdentity=new_lifetime_identity();
    finalReleaseAction=[&] {
        std::lock_guard lock(acz::state().mutex);
        acz::state().rootIdentities.at(r)=newerRootIdentity;
    };
    CHECK(r->Release()==0 && acz::state().rootIdentities.at(r)==newerRootIdentity);
    {std::lock_guard lock(acz::state().mutex);acz::state().roots.erase(r);acz::state().rootIdentities.erase(r);}
    root.refs=1;createdObject=&root;uint8_t invalid[]={1,2,3,4};expectedBytes=invalid;expectedLength=sizeof(invalid);
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK && !acz::state().roots.count(r));
    CHECK(r->Release()==0);expectedBytes=nullptr;expectedLength=0;
    // Address reuse after final Release must decode anew and cannot inherit ID.
    pipeline.refs=1;createdObject=&pipeline;pdesc.pRootSignature=nullptr;pdesc.GS={nullptr,0};
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==S_OK);
    CHECK(!acz::state().pipelines.at(p).shader.valid);
    CHECK(acz::state().pipelines.at(p).identity!=pipelineIdentity && !acz::state().pipelines.at(p).root_identity);
    // A reentrant original Release installs a newer same-address record before
    // cleanup. The captured retiring token must not erase that newer lifetime.
    const auto replacementIdentity=new_lifetime_identity();
    finalReleaseAction=[&] {
        std::lock_guard lock(acz::state().mutex);
        acz::state().pipelines.at(p).identity=replacementIdentity;
    };
    CHECK(p->Release()==0 && acz::state().pipelines.at(p).identity==replacementIdentity);
    {std::lock_guard lock(acz::state().mutex);acz::state().pipelines.erase(p);acz::state().gs.erase(p);}
    createHr=E_FAIL;out=reinterpret_cast<void*>(0x123);
    CHECK(d->CreateGraphicsPipelineState(&pdesc,expectedIID,&out)==E_FAIL && out==reinterpret_cast<void*>(0x123));createHr=S_OK;
    auditMetadataCOM=false;
}

void divergentCanonicalTests() {
    acz::g_protect=protect;
    auditMetadataCOM=true;
    auto rt=listTable(),ut=listTable(),pt=listTable(),lt=listTable();
    Fake root{rt.data()},unknown{ut.data()},pipeline{pt.data()},list{lt.data()};
    root.rootInterface=unknown.rootInterface=&root;
    root.unknownInterface=unknown.unknownInterface=&unknown;unknown.owner=&root;
    auto r=reinterpret_cast<ID3D12RootSignature*>(&root);
    auto key=reinterpret_cast<ID3D12RootSignature*>(&unknown);
    std::array<void*,45> dt{};dt[0]=reinterpret_cast<void*>(&query);dt[1]=reinterpret_cast<void*>(&add);dt[2]=reinterpret_cast<void*>(&release);dt[16]=reinterpret_cast<void*>(&createRootFake);
    dt[10]=reinterpret_cast<void*>(&createPSOFake);dt[12]=reinterpret_cast<void*>(&createListFake);
    Fake device{dt.data()};auto d=reinterpret_cast<ID3D12Device*>(&device);
    expectedRootDevice=d;CHECK(acz::hookCaptureDevice(d));
    D3D12_ROOT_SIGNATURE_DESC desc{};ID3DBlob* bytes=nullptr;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr)));
    expectedBytes=bytes->GetBufferPointer();expectedLength=bytes->GetBufferSize();
    expectedIID=__uuidof(ID3D12RootSignature);expectedNode=7;createdObject=&root;
    transferRootReference=false;void* out=nullptr;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK&&out==r);
    // Expected-red on pre-J051 production: stable canonical != root interface.
    CHECK(acz::state().rootIdentities.count(key));
    auto token=acz::state().rootIdentities.at(key);CHECK(token&&root.refs==1);
    transferRootReference=true;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    CHECK(acz::state().rootIdentities.at(key)==token&&r->Release()==1);
    transferRootReference=false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};auto shader=shaderFixture();
    pd.GS={shader.data(),shader.size()};pd.pRootSignature=r;expectedDesc=&pd;createdObject=&pipeline;
    // The previous call created a root; PSO creation needs its own supported IID.
    // J-098 deliberately refuses explicit-root eligibility for unrelated IIDs.
    expectedIID=__uuidof(ID3D12PipelineState);
    CHECK(d->CreateGraphicsPipelineState(&pd,expectedIID,&out)==S_OK);
    expectedIID=__uuidof(ID3D12RootSignature);
    auto l=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);CHECK(acz::hookCaptureCommandList(l));
    l->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(&pipeline));l->SetGraphicsRootSignature(r);
    std::shared_ptr<const State> observed;
    acz::setBindingCallback([&](std::shared_ptr<const State> s,Command){observed=std::move(s);});
    l->DrawInstanced(1,1,0,0);CHECK(observed->compatible_root()&&observed->root_identity==token);
    CHECK(!observed->pipeline.requirements_proven);
    Inputs inputs;CHECK(!freeze(*observed,Command{false,1,1},inputs));
    // A different object claiming the same canonical IUnknown cannot inherit
    // the live root token, even when its claimed root round-trip agrees.
    auto collisionTable=listTable();Fake collision{collisionTable.data()};
    collision.rootInterface=&collision;collision.unknownInterface=&unknown;
    unknown.rootInterface=&collision;createdObject=&collision;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK&&out==&collision);
    CHECK(!acz::state().rootIdentities.count(key));
    CHECK(!acz::state().states.at(l).bindings.root_identity);
    CHECK(observed->compatible_root()&&observed->root_identity==token); // historical copy
    l->DrawInstanced(1,1,0,0);CHECK(!observed->compatible_root());
    CHECK(reinterpret_cast<IUnknown*>(&collision)->Release()==0);
    unknown.rootInterface=&root;createdObject=&root;transferRootReference=true;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    CHECK(acz::state().rootIdentities.at(key)!=token);CHECK(r->Release()==3);
    transferRootReference=false;
    CHECK(l->Release()==0);
    CHECK(reinterpret_cast<ID3D12PipelineState*>(&pipeline)->Release()==0);
    CHECK(root.refs==1);
    CHECK(reinterpret_cast<IUnknown*>(&unknown)->Release()==0);
    CHECK(!acz::state().rootIdentities.count(key));
    root.refs=1;createdObject=&root;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    CHECK(acz::state().rootIdentities.at(key)!=token);
    CHECK(r->Release()==0&&!acz::state().rootIdentities.count(key));
    // IUnknown creation may return the canonical interface itself. It resolves
    // to the same root key; the IID change creates a fresh provenance token.
    root.refs=1;createdObject=&unknown;expectedIID=__uuidof(IUnknown);
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK&&out==&unknown);
    auto unknownToken=acz::state().rootIdentities.at(key);CHECK(unknownToken!=token&&root.refs==1);
    transferRootReference=true;
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK&&out==&unknown);
    CHECK(acz::state().rootIdentities.at(key)==unknownToken&&root.refs==2);
    CHECK(reinterpret_cast<IUnknown*>(&unknown)->Release()==1);
    CHECK(r->Release()==0&&!acz::state().rootIdentities.count(key));transferRootReference=false;
    // A canonical-interface retirement must conflict with a root-interface
    // creation at the same object, including native final-Release reentry.
    root.refs=1;createdObject=&root;expectedIID=__uuidof(ID3D12RootSignature);
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    auto retiringToken=acz::state().rootIdentities.at(key);
    finalReleaseAction=[&] {
        root.refs=1;
        CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
        CHECK(acz::state().rootIdentities.count(key)); // Invalidated after returned-zero.
    };
    CHECK(reinterpret_cast<IUnknown*>(&unknown)->Release()==0&&!acz::state().rootIdentities.count(key));
    CHECK(d->CreateRootSignature(7,expectedBytes,expectedLength,expectedIID,&out)==S_OK);
    CHECK(acz::state().rootIdentities.at(key)!=retiringToken);
    CHECK(r->Release()==0&&!acz::state().rootKeys.count(reinterpret_cast<IUnknown*>(r)));
    acz::setBindingCallback({});bytes->Release();auditMetadataCOM=false;
}


namespace list_generation_fixture {
struct Interface {void** table;ULONG refs=1;Interface* owner=nullptr;};
static std::array<Interface,15>* active=nullptr;
static unsigned forwards=0;static bool queryThrows=false;
static HRESULT resetResult=S_OK;
static ULONG STDMETHODCALLTYPE add(IUnknown* p){CHECK(!acz::diagnostics::mutex_depth);auto* f=reinterpret_cast<Interface*>(p);return ++f->owner->refs;}
static ULONG STDMETHODCALLTYPE release(IUnknown* p){CHECK(!acz::diagnostics::mutex_depth);auto* f=reinterpret_cast<Interface*>(p);return --f->owner->refs;}
static HRESULT STDMETHODCALLTYPE query(IUnknown*,REFIID iid,void** out) {
    if(iid==__uuidof(ID3D12Device4)){*out=nullptr;return E_NOINTERFACE;}
    CHECK(!acz::diagnostics::mutex_depth);*out=nullptr;
    if(queryThrows&&iid==__uuidof(ID3D12GraphicsCommandList3))throw std::bad_alloc();
    unsigned n=11;
    if(iid!=__uuidof(IUnknown)) {
        for(n=0;n<11;++n)if(iid==acz::listIids[n])break;
        if(n==11) {
            for(unsigned i=0;i<3;++i)if(iid==acz::listBaseIids[i]){n=i+12;break;}
            if(n==11)return E_NOINTERFACE;
        }
    }
    auto* value=&(*active)[n];list_generation_fixture::add(reinterpret_cast<IUnknown*>(value));*out=value;return S_OK;
}
static HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*) {
    CHECK(!acz::diagnostics::mutex_depth);++forwards;return resetResult;
}
static void STDMETHODCALLTYPE topology(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY t) {
    CHECK(!acz::diagnostics::mutex_depth&&t==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);++forwards;
}
static void STDMETHODCALLTYPE pipeline1(ID3D12GraphicsCommandList4*,ID3D12StateObject*) {CHECK(!acz::diagnostics::mutex_depth);++forwards;}
static void STDMETHODCALLTYPE program(ID3D12GraphicsCommandList10*,const D3D12_SET_PROGRAM_DESC*) {CHECK(!acz::diagnostics::mutex_depth);++forwards;}
static void tests() {
    using namespace acz;
    g_protect=protect;g_pinResult=true;
    auto& s=state();const auto before=s.states.size();
    std::array<std::array<void*,86>,15> tables{};
    std::array<Interface,15> objects{};active=&objects;
    auto baseTable=listTable();
    for(unsigned n=0;n<15;++n) {
        std::copy(baseTable.begin(),baseTable.end(),tables[n].begin());
        tables[n][0]=reinterpret_cast<void*>(&query);tables[n][1]=reinterpret_cast<void*>(&add);tables[n][2]=reinterpret_cast<void*>(&release);
        tables[n][10]=reinterpret_cast<void*>(&reset);tables[n][20]=reinterpret_cast<void*>(&topology);
        tables[n][75]=reinterpret_cast<void*>(&pipeline1);tables[n][84]=reinterpret_cast<void*>(&program);
        objects[n].table=tables[n].data();objects[n].owner=&objects[0];
    }
    auto* base=reinterpret_cast<ID3D12GraphicsCommandList*>(&objects[0]);
    // Raw state seen through a shared table before discovery is discarded,
    // never merged with the certified recording or its lifetime token.
    auto rawAlias=reinterpret_cast<ID3D12GraphicsCommandList*>(&objects[1]);
    s.states[rawAlias].bindings.set_value(63,metadata::ParameterKind::srv,999);
    const auto rawToken=s.states.at(rawAlias).bindings.list_identity;
    CHECK(hookCaptureCommandList(base));CHECK(s.states.size()==before+1&&objects[0].refs==1);
    CHECK(s.states.at(base).bindings.list_identity!=rawToken&&!s.states.at(base).bindings.roots.count(63));
    auto& list=s.states.at(base);
    // Expected-red on the old installer: a fresh late list was marked complete.
    CHECK(!list.bindings.tracking_complete&&!list.reset_observed);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:created before hook/creation unobserved; no observed Reset");
    base->ClearState(nullptr);CHECK(!list.bindings.tracking_complete&&!list.reset_observed);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:created before hook/creation unobserved; no observed Reset");
    resetResult=E_FAIL;const auto generation=list.bindings.generation;
    CHECK(base->Reset(nullptr,nullptr)==E_FAIL&&list.bindings.generation==generation&&!list.reset_observed);
    resetResult=S_OK;CHECK(base->Reset(nullptr,nullptr)==S_OK&&list.bindings.tracking_complete&&list.reset_observed);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:list device identity unobserved");
    list.native_binding_device=123;CHECK(!listPointFailure_locked(list,list.bindings));
    const auto token=list.bindings.list_identity;
    // Every SDK interface variant, including different tables/this pointers,
    // updates the ONE certified state; originals see each original self.
    for(unsigned n=0;n<11;++n) {
        auto* variant=reinterpret_cast<ID3D12GraphicsCommandList*>(&objects[n]);
        CHECK(listKey_locked(variant)==base&&s.lists.count(tables[n].data()));
        variant->SetGraphicsRootShaderResourceView(n,100+n);
        CHECK(list.bindings.roots.at(n).value==100+n&&s.states.size()==before+1);
    }
    auto* one=reinterpret_cast<ID3D12GraphicsCommandList1*>(&objects[1]);
    one->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);CHECK(list.bindings.topology==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto copy=list.bindings;
    ++copy.generation;CHECK(std::string_view(listPointFailure_locked(list,copy))=="rejected:list generation mismatch");
    copy=list.bindings;++copy.revision;CHECK(std::string_view(listPointFailure_locked(list,copy))=="rejected:list revision mismatch");
    copy=list.bindings;++copy.list_identity;CHECK(std::string_view(listPointFailure_locked(list,copy))=="rejected:cross-list identity mismatch");
    copy.list_identity=0;CHECK(std::string_view(listPointFailure_locked(list,copy))=="rejected:no list identity");
    auto saved=list.bindings;
    auto* ten=reinterpret_cast<ID3D12GraphicsCommandList10*>(&objects[10]);
    ten->ExecuteBundle(base);CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:bundle use");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);ten->ExecuteIndirect(nullptr,3,nullptr,13,nullptr,29);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:ExecuteIndirect use");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);ten->SetGraphicsRootShaderResourceView(64,0);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:root parameter out of range or invalid constants");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);ten->IASetVertexBuffers(33,0,nullptr);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:IA vertex buffer slot out of range");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);ten->SetPipelineState1(nullptr);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:unsupported SetPipelineState1");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);ten->SetProgram(nullptr);
    CHECK(std::string_view(listPointFailure_locked(list,list.bindings))=="rejected:unsupported SetProgram");
    CHECK(ten->Reset(nullptr,nullptr)==S_OK);
    // An unpatched/competing variant fails the checked installer and poisons
    // the whole identity; the original dispatch is retained for recovery.
    auto native=tables[9][40];tables[9][40]=reinterpret_cast<void*>(&foreignTableSet);
    CHECK(!hookCaptureCommandList(base)&&!list.capture_hooks&&!list.bindings.tracking_complete);
    tables[9][40]=s.lists.at(tables[9].data()).originals[40];CHECK(hookCaptureCommandList(base));
    CHECK(list.bindings.list_identity!=token&&!list.reset_observed);
    CHECK(base->Reset(nullptr,nullptr)==S_OK&&list.bindings.tracking_complete);(void)native;
    queryThrows=true;bool threw=false;
    try {hookCaptureCommandList(base);}catch(const std::bad_alloc&){threw=true;}
    CHECK(threw&&!list.capture_hooks&&!list.bindings.tracking_complete&&objects[0].refs==1);
    queryThrows=false;CHECK(hookCaptureCommandList(base));CHECK(base->Reset(nullptr,nullptr)==S_OK);
    // Canonical IUnknown Release retires all aliases. Same address, fresh token.
    const auto retired=list.bindings.list_identity;
    CHECK(reinterpret_cast<IUnknown*>(&objects[11])->Release()==0&&!s.states.count(base));
    for(auto& object:objects)CHECK(!s.listKeys.count(&object));
    objects[0].refs=1;CHECK(hookCommandList(base));CHECK(base->Reset(nullptr,nullptr)==S_OK);
    CHECK(hookCaptureCommandList(base));CHECK(s.states.at(base).bindings.list_identity!=retired);
    // J-062: the observed Reset now certifies the previously unknown object.
    CHECK(s.states.at(base).reset_observed&&s.states.at(base).bindings.tracking_complete);
    CHECK(base->Reset(nullptr,nullptr)==S_OK&&s.states.at(base).bindings.tracking_complete);
    CHECK(reinterpret_cast<IUnknown*>(&objects[14])->Release()==0&&s.states.size()==before);
    for(auto& table:tables)s.lists.erase(table.data());active=nullptr;
}
}
namespace registration_fixture {
static Fake *baseObject=nullptr,*extendedObject=nullptr,*outputObject=nullptr;
static HRESULT result=S_FALSE;
static UINT expectedMask=7;
static unsigned forwards=0;
static bool throwDeviceQuery=false;
static HRESULT STDMETHODCALLTYPE deviceQuery(IUnknown*,REFIID iid,void** out) {
    CHECK(!acz::diagnostics::mutex_depth);
    Fake* value=iid==__uuidof(ID3D12Device4)?extendedObject:
        (iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12Device))?baseObject:nullptr;
    *out=value;if(!value)return E_NOINTERFACE;++value->refs;
    if(throwDeviceQuery){throwDeviceQuery=false;throw std::bad_alloc();}return S_OK;
}
static HRESULT STDMETHODCALLTYPE create1(ID3D12Device4* self,UINT node,D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** out) {
    CHECK(!acz::diagnostics::mutex_depth&&self==reinterpret_cast<ID3D12Device4*>(extendedObject));
    CHECK(node==expectedMask&&flags==D3D12_COMMAND_LIST_FLAG_NONE&&iid==__uuidof(ID3D12GraphicsCommandList));
    CHECK(type==D3D12_COMMAND_LIST_TYPE_DIRECT||type==D3D12_COMMAND_LIST_TYPE_BUNDLE);
    ++forwards;if(out)*out=outputObject;return result;
}
static HRESULT STDMETHODCALLTYPE listDevice(ID3D12GraphicsCommandList*,REFIID iid,void** out) {
    return deviceQuery(nullptr,iid,out);
}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE bundleType(ID3D12CommandList*) {return D3D12_COMMAND_LIST_TYPE_BUNDLE;}
static HRESULT STDMETHODCALLTYPE concurrentReset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*) {return S_OK;}
static void tests() {
    using namespace acz;
    g_protect=protect;g_pinResult=true;setBindingCallback({});setDrawCallback({});
    for(bool distinct:{false,true}) {
        std::array<void*,57> dt{},et{};
        dt[0]=et[0]=reinterpret_cast<void*>(&deviceQuery);
        dt[1]=et[1]=reinterpret_cast<void*>(&add);dt[2]=et[2]=reinterpret_cast<void*>(&release);
        dt[10]=dt[12]=dt[16]=reinterpret_cast<void*>(&draw); // Never called here.
        dt[51]=et[51]=reinterpret_cast<void*>(&create1);
        Fake base{dt.data()},extension{distinct?et.data():dt.data()};
        baseObject=&base;extendedObject=distinct?&extension:&base;
        auto* device=reinterpret_cast<ID3D12Device*>(&base);
        auto* device4=reinterpret_cast<ID3D12Device4*>(extendedObject);
        const auto original=extendedObject->table[51];
        g_pinResult=false;CHECK(!hookCaptureDevice(device)&&extendedObject->table[51]==original);g_pinResult=true;
        const auto extensionBefore=et;
        CHECK(hookCaptureDevice(device));
        CHECK(extendedObject->table[51]==original&&et==extensionBefore);
        auto protections=protectCalls;CHECK(hookCaptureDevice(device)&&protectCalls==protections);
        CHECK(base.refs==1&&extension.refs==1);
        auto lt=listTable();lt[7]=reinterpret_cast<void*>(&listDevice);
        Fake object{lt.data()};auto* list=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
        outputObject=&object;void* out=nullptr;
        result=S_FALSE;
        // Seed this table using a different list. A closed-created unknown list
        // is deliberately unregistered until its first observed Reset (D-86).
        Fake seed{lt.data()};auto* seeded=reinterpret_cast<ID3D12GraphicsCommandList*>(&seed);
        CHECK(hookCaptureCommandList(seeded));CHECK(seeded->Release()==0);
        result=E_FAIL;CHECK(device4->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,
            __uuidof(ID3D12GraphicsCommandList),&out)==E_FAIL&&out==&object&&!state().states.count(list));
        result=S_FALSE;outputObject=nullptr;
        CHECK(device4->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,
            __uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&!out&&!state().states.count(list));
        outputObject=&object;
        CHECK(device4->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_BUNDLE,D3D12_COMMAND_LIST_FLAG_NONE,
            __uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&!state().states.count(list));
        CHECK(device4->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,
            __uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&out==list&&object.refs==1);
        CHECK(!state().states.count(list));
        resetHr=E_FAIL;CHECK(list->Reset(nullptr,nullptr)==E_FAIL&&!state().states.count(list));
        list->ClearState(nullptr);auto& unknown=state().states.at(list);
        CHECK(!unknown.interfaces_observed&&!unknown.bindings.tracking_complete);
        CHECK(std::string_view(listPointFailure_locked(unknown,unknown.bindings))=="rejected:created before hook/creation unobserved; no observed Reset");
        resetHr=S_OK;CHECK(list->Reset(nullptr,nullptr)==S_OK);
        auto& registeredClosed=state().states.at(list);const auto token=registeredClosed.bindings.list_identity;
        CHECK(token&&!registeredClosed.creation_observed&&registeredClosed.reset_observed&&registeredClosed.bindings.tracking_complete);
        CHECK(registeredClosed.registration_path==2&&!listPointFailure_locked(registeredClosed,registeredClosed.bindings));
        CHECK(list->Release()==0&&!state().states.count(list));object.refs=1;
        CHECK(device4->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,
            __uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&!state().states.count(list));
        CHECK(list->Reset(nullptr,nullptr)==S_OK&&state().states.at(list).bindings.list_identity!=token);
        CHECK(list->Release()==0);
        object.refs=1;lt[8]=reinterpret_cast<void*>(&bundleType);
        // Unknown early Bundle refusals are log-only: shared hooks cannot
        // establish this object's alias routing. No retained gap/state.
        CHECK(list->Reset(nullptr,nullptr)==S_OK&&!state().states.count(list));
        CHECK(list->Release()==0&&!state().states.count(list));
        lt[8]=reinterpret_cast<void*>(&directListType);
        // An unknown object on the already-patched table gets no identity from
        // draws, failed Reset or ClearState eligibility. Its successful Reset
        // certifies all aliases and discards any earlier setter state.
        object.refs=1;list->DrawInstanced(1,1,0,0);CHECK(!state().states.count(list));
        resetHr=E_FAIL;CHECK(list->Reset(nullptr,nullptr)==E_FAIL&&!state().states.count(list));resetHr=S_OK;
        list->SetGraphicsRootShaderResourceView(3,123);CHECK(!state().states.at(list).interfaces_observed);
        CHECK(list->Reset(nullptr,nullptr)==S_OK);auto& registered=state().states.at(list);
        CHECK(registered.interfaces_observed&&registered.capture_hooks&&registered.reset_observed&&!registered.creation_observed);
        CHECK(registered.bindings.tracking_complete&&registered.bindings.roots.empty()&&registered.bindings.list_identity!=token);
        CHECK(!listPointFailure_locked(registered,registered.bindings));const auto resetToken=registered.bindings.list_identity;
        CHECK(list->Release()==0&&!state().states.count(list));object.refs=1;
        CHECK(list->Reset(nullptr,nullptr)==S_OK&&state().states.at(list).bindings.list_identity!=resetToken);
        CHECK(list->Release()==0);
        state().lists.erase(lt.data());state().devices.erase(dt.data());state().devices.erase(et.data());
    }
    // 200 pairs of concurrent unknown-list registrations through a shared,
    // prepatched table. This is scheduling evidence, not a runtime guarantee.
    auto lt=listTable();lt[10]=reinterpret_cast<void*>(&concurrentReset);Fake seed{lt.data()};auto* seeded=reinterpret_cast<ID3D12GraphicsCommandList*>(&seed);
    CHECK(hookCaptureCommandList(seeded));CHECK(seeded->Release()==0);
    for(unsigned iteration=0;iteration<200;++iteration) {
        Fake a{lt.data()},b{lt.data()};auto* la=reinterpret_cast<ID3D12GraphicsCommandList*>(&a);auto* lb=reinterpret_cast<ID3D12GraphicsCommandList*>(&b);
        HRESULT ra=E_FAIL,rb=E_FAIL;
        std::thread first([&]{ra=la->Reset(nullptr,nullptr);});
        std::thread second([&]{rb=lb->Reset(nullptr,nullptr);});first.join();second.join();CHECK(ra==S_OK&&rb==S_OK);
        const auto& sa=state().states.at(la);const auto& sb=state().states.at(lb);
        CHECK(sa.bindings.list_identity!=sb.bindings.list_identity&&sa.bindings.generation==sb.bindings.generation&&sa.bindings.revision==sb.bindings.revision);
        la->SetGraphicsRootShaderResourceView(3,123);CHECK(!sb.bindings.roots.count(3));
        CHECK(la->Release()==0&&lb->Release()==0);
    }
    state().lists.erase(lt.data());baseObject=extendedObject=outputObject=nullptr;draws=0;
}
}
int main() {registration_fixture::tests();smallSerializedRootTests();pureTests();list_generation_fixture::tests();installedCaptureTests();hookTests();creationTests();divergentCanonicalTests();std::printf("%d immutable capture offline checks passed (synthetic only)\n",checks);}
