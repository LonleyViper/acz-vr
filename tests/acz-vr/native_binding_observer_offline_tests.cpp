// Compile this file twice: the ABI-only object uses installed SDK C vtables;
// the executable uses fake objects and the actual production interception.
// No real device/queue/GPU/XR/game or generated DLL is ever loaded.
#ifdef ACZ_BINDING_ABI_CHECK
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d12.h>
#include <d3d12compatibility.h>
#include <cstddef>
#include <type_traits>
#define SLOT(T,M,N) static_assert(offsetof(T,M)==sizeof(void*)*(N), "SDK absolute slot")
// Windows SDK 10.0.26100.0 um/d3d12.h: base C vtable 6593,
// methods 6651-6987. Variants inherit the identical base prefix.
#define LIST_SLOTS(T) \
 SLOT(T,Release,2);SLOT(T,GetDevice,7);SLOT(T,GetType,8);SLOT(T,Reset,10);SLOT(T,ClearState,11); \
 SLOT(T,DrawInstanced,12);SLOT(T,DrawIndexedInstanced,13); \
 SLOT(T,IASetPrimitiveTopology,20);SLOT(T,SetPipelineState,25); \
 SLOT(T,ExecuteBundle,27);SLOT(T,SetDescriptorHeaps,28); \
 SLOT(T,SetGraphicsRootSignature,30);SLOT(T,SetGraphicsRootDescriptorTable,32); \
 SLOT(T,SetGraphicsRoot32BitConstant,34);SLOT(T,SetGraphicsRoot32BitConstants,36); \
 SLOT(T,SetGraphicsRootConstantBufferView,38);SLOT(T,SetGraphicsRootShaderResourceView,40); \
 SLOT(T,SetGraphicsRootUnorderedAccessView,42);SLOT(T,IASetIndexBuffer,43); \
 SLOT(T,IASetVertexBuffers,44);SLOT(T,ExecuteIndirect,59)
LIST_SLOTS(ID3D12GraphicsCommandListVtbl);
LIST_SLOTS(ID3D12GraphicsCommandList1Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList2Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList3Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList4Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList5Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList6Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList7Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList8Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList9Vtbl);
LIST_SLOTS(ID3D12GraphicsCommandList10Vtbl);
SLOT(ID3D12GraphicsCommandList4Vtbl,SetPipelineState1,75); // d3d12.h:21818
SLOT(ID3D12GraphicsCommandList10Vtbl,SetProgram,84); // d3d12.h:35143
#undef LIST_SLOTS
// SDK 10.0.26100.0 um/d3d12.h:12840-12846.
SLOT(ID3D12Device4Vtbl,CreateCommandList1,51);
static_assert(std::is_same_v<decltype(ID3D12Device4Vtbl::CreateCommandList1),
    HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**)>);
SLOT(ID3D12DeviceVtbl,CreateDescriptorHeap,14);
SLOT(ID3D12DeviceVtbl,QueryInterface,0);
SLOT(ID3D12DeviceVtbl,Release,2);
SLOT(ID3D12DeviceVtbl,GetPrivateData,3);
SLOT(ID3D12DeviceVtbl,SetPrivateData,4);
SLOT(ID3D12DeviceVtbl,SetPrivateDataInterface,5);
SLOT(ID3D12CompatibilityDeviceVtbl,QueryInterface,0);
SLOT(ID3D12CompatibilityDeviceVtbl,Release,2);
SLOT(ID3D12CompatibilityDeviceVtbl,CreateSharedResource,3);
SLOT(ID3D12CompatibilityDeviceVtbl,CreateSharedHeap,4);
SLOT(ID3D12CompatibilityDeviceVtbl,ReflectSharedProperties,5);
static_assert(std::is_same_v<decltype(ID3D12CompatibilityDeviceVtbl::CreateSharedResource),
    HRESULT(STDMETHODCALLTYPE*)(ID3D12CompatibilityDevice*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,const D3D11_RESOURCE_FLAGS*,
    D3D12_COMPATIBILITY_SHARED_FLAGS,ID3D12LifetimeTracker*,ID3D12SwapChainAssistant*,REFIID,void**)>);
static_assert(std::is_same_v<decltype(ID3D12CompatibilityDeviceVtbl::CreateSharedHeap),
    HRESULT(STDMETHODCALLTYPE*)(ID3D12CompatibilityDevice*,const D3D12_HEAP_DESC*,D3D12_COMPATIBILITY_SHARED_FLAGS,REFIID,void**)>);
static_assert(std::is_same_v<decltype(ID3D12CompatibilityDeviceVtbl::ReflectSharedProperties),
    HRESULT(STDMETHODCALLTYPE*)(ID3D12CompatibilityDevice*,ID3D12Object*,D3D12_REFLECT_SHARED_PROPERTY,void*,UINT)>);
SLOT(ID3D12DeviceVtbl,GetDescriptorHandleIncrementSize,15);
SLOT(ID3D12DeviceVtbl,CreateConstantBufferView,17);
SLOT(ID3D12DeviceVtbl,CreateShaderResourceView,18);
SLOT(ID3D12DeviceVtbl,CreateUnorderedAccessView,19);
SLOT(ID3D12DeviceVtbl,CreateRenderTargetView,20);
SLOT(ID3D12DeviceVtbl,CreateDepthStencilView,21);
SLOT(ID3D12DeviceVtbl,CreateSampler,22);
SLOT(ID3D12DeviceVtbl,CopyDescriptors,23);
SLOT(ID3D12DeviceVtbl,CopyDescriptorsSimple,24);
SLOT(ID3D12DeviceVtbl,CreateCommittedResource,27);
SLOT(ID3D12DeviceVtbl,CreatePlacedResource,29);
SLOT(ID3D12DeviceVtbl,CreateReservedResource,30);
// J-125 declaration-only gap inventory. These slots are NEVER patched here.
SLOT(ID3D12Device4Vtbl,CreateCommittedResource1,53);
SLOT(ID3D12Device4Vtbl,CreateReservedResource1,55);
SLOT(ID3D12Device8Vtbl,CreateCommittedResource2,69);
SLOT(ID3D12Device8Vtbl,CreatePlacedResource1,70);
SLOT(ID3D12Device10Vtbl,CreateCommittedResource3,76);
SLOT(ID3D12Device10Vtbl,CreatePlacedResource2,77);
SLOT(ID3D12Device10Vtbl,CreateReservedResource2,78);
SLOT(ID3D12DeviceVtbl,OpenSharedHandle,32);
SLOT(ID3D12DeviceVtbl,OpenSharedHandleByName,33);
SLOT(ID3D12DescriptorHeapVtbl,Release,2);
SLOT(ID3D12DescriptorHeapVtbl,GetCPUDescriptorHandleForHeapStart,9);
SLOT(ID3D12DescriptorHeapVtbl,GetGPUDescriptorHandleForHeapStart,10);
SLOT(ID3D12ResourceVtbl,Release,2);
SLOT(ID3D12ResourceVtbl,QueryInterface,0);
SLOT(ID3D12DescriptorHeapVtbl,QueryInterface,0);
SLOT(ID3D12ResourceVtbl,GetGPUVirtualAddress,11);
#define SIGNATURE(M,...) static_assert(std::is_same_v<decltype(ID3D12DeviceVtbl::M),__VA_ARGS__>, "SDK forwarding signature")
SIGNATURE(QueryInterface,HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,REFIID,void**));
SIGNATURE(Release,ULONG(STDMETHODCALLTYPE*)(ID3D12Device*));
SIGNATURE(CreateDescriptorHeap,HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**));
SIGNATURE(CreateConstantBufferView,void(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CreateShaderResourceView,void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CreateUnorderedAccessView,void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CreateRenderTargetView,void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CreateDepthStencilView,void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CreateSampler,void(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_SAMPLER_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE));
SIGNATURE(CopyDescriptors,void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE));
SIGNATURE(CopyDescriptorsSimple,void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE));
SIGNATURE(CreateCommittedResource,HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**));
SIGNATURE(CreatePlacedResource,HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**));
SIGNATURE(CreateReservedResource,HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**));
SLOT(ID3D12Device4Vtbl,CreateCommittedResource1,53);
SLOT(ID3D12Device4Vtbl,CreateReservedResource1,55);
SLOT(ID3D12Device8Vtbl,CreateCommittedResource2,69);
SLOT(ID3D12Device8Vtbl,CreatePlacedResource1,70);
SLOT(ID3D12Device10Vtbl,CreateCommittedResource3,76);
SLOT(ID3D12Device10Vtbl,CreatePlacedResource2,77);
SLOT(ID3D12Device10Vtbl,CreateReservedResource2,78);
void binding_sdk_abi_checked() {}
#else
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <limits>
#include <cstring>
#include <condition_variable>
// Same failure mechanism as the registry fixture, thread-local so worker
// coordination and unrelated allocations never consume the injected budget.
static thread_local int allocation_budget=-1;
static thread_local void (*allocation_probe)(void*)=nullptr;
static thread_local void* allocation_probe_context=nullptr;
void* operator new(std::size_t bytes) {
    if(auto probe=allocation_probe) {
        allocation_probe=nullptr;
        probe(allocation_probe_context);
    }
    if(allocation_budget==0)throw std::bad_alloc();
    if(allocation_budget>0)--allocation_budget;
    if(auto* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void binding_sdk_abi_checked();
static std::atomic<unsigned> checks{0};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}} while(0)
#include "descriptor_table_diagnostics_cases.h"
#include "texture_description_cases.h"
static void observerDescriptorDiagnostics() {
    using namespace acz;using D=diagnostics::DescriptorTableDiagnostics;
    binding::NativeBindingObserver off;
    off.diagnostic_tracking(false);off.heap({1,1000,2000,4,32,0,1,0},7);
    off.diagnostic_creation(0,3);off.srv(99,nullptr,1000,7);
    CHECK(!off.diagnostic());
    binding::NativeBindingObserver observer;observer.diagnostic_tracking(true);
    observer.heap({1,1000,2000,4,32,0,1,0},7);
    observer.heap({2,3000,0,4,32,0,0,0},7);
    auto loc=observer.diagnostic()->bound(1,2000,7);
    CHECK(observer.diagnostic_snapshot(loc,0).flags&D::unwritten);
    D3D12_SHADER_RESOURCE_VIEW_DESC texture{};
    texture.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.Texture2D.MipLevels=1;
    observer.srv(99,&texture,3000,7);
    D3D12_CPU_DESCRIPTOR_HANDLE source{3000},dest{1000};
    observer.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7);
    auto snap=observer.diagnostic_snapshot(loc,0);
    CHECK((snap.flags&(D::copied|D::unsupported_srv|D::unknown_resource))==
        (D::copied|D::unsupported_srv|D::unknown_resource));
    CHECK(snap.type==D::srv&&snap.dimension==4&&snap.format==28);
    CHECK(snap.writer==D::copy_ranges&&snap.origin==D::create_srv&&snap.desc==2);
    CHECK(snap.ordinal==2&&snap.writes==1&&snap.source_heap==2&&snap.source_class==0&&snap.source_slot==0&&snap.source_flags==0);
    observer.srv(99,nullptr,3000,7);
    observer.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7,true);
    snap=observer.diagnostic_snapshot(loc,0);
    CHECK(snap.writer==D::copy_simple&&snap.origin==D::create_srv&&snap.desc==1);
    CHECK(snap.dimension==0&&snap.format==0&&snap.ordinal==4&&snap.writes==2);
    CHECK(snap.source_heap==2&&snap.source_class==0&&!snap.source_flags);
    observer.srv(99,&texture,1000,7);snap=observer.diagnostic_snapshot(loc,0);
    CHECK(snap.writer==D::create_srv&&snap.origin==D::create_srv&&snap.desc==2&&!snap.source_heap);
    CHECK(snap.source_class==UINT32_MAX&&snap.dimension==4&&snap.format==28&&snap.writes==3);
    // Explicit but unknown enums are distinguishable from NULL and an unseen copy source.
    texture.ViewDimension=static_cast<D3D12_SRV_DIMENSION>(UINT32_MAX);
    texture.Format=static_cast<DXGI_FORMAT>(UINT32_MAX);
    observer.srv(99,&texture,1032,7);
    auto unknown=observer.diagnostic_snapshot(loc,1);
    CHECK(unknown.writer==D::create_srv&&unknown.desc==2&&unknown.dimension==UINT32_MAX&&unknown.format==UINT32_MAX);
    source.ptr=3064;dest.ptr=1064;
    observer.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7);
    unknown=observer.diagnostic_snapshot(loc,2);
    CHECK(unknown.writer==D::copy_ranges&&unknown.origin==D::writer_unknown&&!unknown.desc);
    CHECK((unknown.flags&(D::unwritten|D::invalidated))==(D::unwritten|D::invalidated));
    CHECK(unknown.source_slot==2&&unknown.source_heap==2&&!unknown.source_flags);
    observer.diagnostic_creation(0,3);observer.diagnostic_creation(1,3);
    observer.diagnostic_creation(2,0);observer.diagnostic_creation(3,1);
    CHECK(observer.diagnostic()->creations[3]==1&&observer.diagnostic()->textures[0]==1&&observer.diagnostic()->missing_desc==1);
    capture::State state;state.list_identity=8;state.generation=1;state.tracking_complete=true;
    state.set_heaps({1});state.set_value(0,metadata::ParameterKind::table,2000);
    CHECK(!observer.table(state,0,1,0,7)); // diagnostic texture did not enter registry
    observer.unsupported_write(1000,7,D::uav);
    snap=observer.diagnostic_snapshot(loc,0);
    CHECK((snap.flags&(D::overwritten|D::wrong_type))==(D::overwritten|D::wrong_type)&&snap.type==D::uav);
    CHECK(snap.writer==D::create_uav&&snap.origin==D::create_uav&&!snap.desc);
    observer.cbv(nullptr,1000,7);CHECK(observer.diagnostic_snapshot(loc,0).writer==D::create_cbv);
    observer.sampler(nullptr,1000,7);CHECK(observer.diagnostic_snapshot(loc,0).writer==D::create_sampler);
    observer.unsupported_write(1000,7);CHECK(observer.diagnostic_snapshot(loc,0).writer==D::create_other);
    observer.remove_heap(observer.heap_token(1));CHECK(observer.diagnostic_snapshot(loc,0).flags&D::stale_heap);
    observer.heap({1,1000,2000,4,32,0,1,0},7);CHECK(observer.diagnostic_snapshot(loc,0).flags&D::stale_heap);
    const auto* history=observer.diagnostic();const auto savedSlots=history->slots.size();
    const auto savedOrdinal=history->write_ordinal;const auto savedCreations=history->creations;
    observer.diagnostic_tracking(false);observer.unsupported_write(1032,7,D::uav);
    observer.diagnostic_creation(0,3);observer.srv(99,nullptr,1000,7);
    CHECK(history->write_ordinal==savedOrdinal&&history->creations==savedCreations);
    CHECK(!observer.diagnostic()&&history->slots.size()==savedSlots);
    observer.diagnostic_tracking(true);CHECK(observer.diagnostic_snapshot(loc,0).flags&D::incomplete);
    observer.disable();CHECK(observer.diagnostic_snapshot(loc,0).flags&D::incomplete);
    CHECK(!diagnostics::Config{}.ownership_pins()&&!diagnostics::Config{}.capture_enabled());
}
struct Fake {void** table;ULONG refs=1;uint64_t cpu=0,gpu=0;};
// Reusable deterministic barrier, with a timeout only to bound deadlock failures.
struct Rendezvous {
    std::mutex mutex;std::condition_variable condition;unsigned arrived=0,phase=0;
    void meet(unsigned participants) {
        std::unique_lock lock(mutex);auto previous=phase;
        if(++arrived==participants) {arrived=0;++phase;condition.notify_all();return;}
        CHECK(condition.wait_for(lock,std::chrono::seconds(5),[&]{return phase!=previous;}));
    }
};
static thread_local Rendezvous* concurrentGate=nullptr;
static thread_local Fake* concurrentObject=nullptr;
static thread_local bool concurrentRelease=false;
static std::atomic<unsigned> concurrentForwards{0};
static void unlockedNativeProbe() {
    // All four overlap callbacks park between the same two rendezvous. The
    // old single try_lock could lose to ANOTHER PROBE's brief ownership (or
    // fail spuriously), even though no callback held the production mutex.
    // A blocking acquisition lets the probes serialize naturally. The caller
    // stays inside its native callback until acquisition is acknowledged, so
    // a mutex retained across forwarding still fails at the bounded wait.
    // The second rendezvous prevents peers returning to production critical
    // sections before every callback has completed this handshake. Elsewhere,
    // the fixture has no competing worker or parks it at waitCondition first.
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool finished=false,available=false;
    std::thread probe([&]{
        {std::lock_guard lock(acz::state().mutex);}
        {std::lock_guard lock(completionMutex);finished=true;}
        completionCondition.notify_one();
    });
    {
        std::unique_lock lock(completionMutex);
        available=completionCondition.wait_for(lock,std::chrono::seconds(5),[&]{return finished;});
    }
    CHECK(available);
    probe.join();
}
static Fake* nextObject=nullptr;
static HRESULT result=S_FALSE;
static bool queryFails=false,probeRelease=false,reenterWrite=false;
static bool throwAddress=false,aliasQuery=false,waitWrite=false,waitCreate=false;
static std::mutex waitMutex;
static std::condition_variable waitCondition;
static bool writeEntered=false,writeContinue=false;
static unsigned heapCalls[2]{},resourceCalls[2]{},writes=0,copies=0;
static ID3D12Device* lastDevice=nullptr;
static GUID lastIID{};
static const void *lastDesc=nullptr,*lastProps=nullptr,*lastClear=nullptr;
static void** lastOutput=nullptr;
static UINT lastFlags=0,lastInitial=0,lastCount=0,lastSourceCount=0;
static UINT64 lastOffset=0;
static ID3D12Heap* lastHeap=nullptr;
static ID3D12Resource *lastResource=nullptr,*lastCounter=nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE lastDest{},lastSource{};
static const D3D12_CPU_DESCRIPTOR_HANDLE *lastDests=nullptr,*lastSources=nullptr;
static const UINT *lastDestSizes=nullptr,*lastSourceSizes=nullptr;
static D3D12_DESCRIPTOR_HEAP_TYPE lastType{};
static acz::capture::State probeState;
static uint64_t probeHeap=0;
static bool nestedPointRejected=false;
static std::function<void(ID3D12Device*,D3D12_CPU_DESCRIPTOR_HANDLE)> nestedAction;
static std::function<void()> releaseAction,creationAction;

void sentinel() {CHECK(false);}
BOOL WINAPI protectOK(LPVOID,SIZE_T,DWORD,DWORD* old) {*old=PAGE_READONLY;return TRUE;}
static unsigned protectCount=0,protectFail=0;
static void* compatibilityOutput=nullptr;
static IUnknown* canonicalAliasOutput=nullptr;
static Fake* canonicalAliasTarget=nullptr;
BOOL WINAPI protectFailure(LPVOID,SIZE_T,DWORD,DWORD* old) {*old=PAGE_READONLY;return ++protectCount!=protectFail;}
HRESULT STDMETHODCALLTYPE query(IUnknown* self,REFIID iid,void** out) {
    if(iid==__uuidof(ID3D12Device4)){*out=nullptr;return E_NOINTERFACE;}
    for(unsigned i=1;i<std::size(acz::listIids);++i)if(iid==acz::listIids[i]){*out=nullptr;return E_NOINTERFACE;}

    if(queryFails) {*out=nullptr;return E_NOINTERFACE;}
    if(self==canonicalAliasOutput && iid==__uuidof(IUnknown)) {
        *out=canonicalAliasTarget;++canonicalAliasTarget->refs;return S_OK;
    }
    auto f=reinterpret_cast<Fake*>(self);++f->refs;
    *out=iid==__uuidof(ID3D12CompatibilityDevice) && compatibilityOutput?compatibilityOutput:
        aliasQuery?static_cast<void*>(nextObject):static_cast<void*>(self);return S_OK;
}
ULONG STDMETHODCALLTYPE add(IUnknown* self) {return ++reinterpret_cast<Fake*>(self)->refs;}
ULONG STDMETHODCALLTYPE release(IUnknown* self) {
    if(concurrentRelease) {concurrentGate->meet(4);unlockedNativeProbe();concurrentGate->meet(4);++concurrentForwards;}
    if(probeRelease && !concurrentGate) {
        unlockedNativeProbe(); // Production temporary QI ownership releases outside locks.
        acz::currentNativeBinding({},probeState); // Actual reentry, no deadlock.
    }
    if(releaseAction) {auto action=std::move(releaseAction);releaseAction={};unlockedNativeProbe();action();}
    return --reinterpret_cast<Fake*>(self)->refs;
}
UINT STDMETHODCALLTYPE nodeCount(ID3D12Device*) {return 1;}
UINT STDMETHODCALLTYPE increment(ID3D12Device*,D3D12_DESCRIPTOR_HEAP_TYPE type) {CHECK(type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);return 32;}
UINT64 STDMETHODCALLTYPE gpuAddress(ID3D12Resource* self) {if(throwAddress) throw std::bad_alloc();return reinterpret_cast<Fake*>(self)->gpu;}
// MSVC struct-return virtual ABI: use actual member dispatch entries, not free
// functions cast to the hidden return-buffer convention.
struct HandleDispatch {
    virtual D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE CPU() {return {reinterpret_cast<Fake*>(this)->cpu};}
    virtual D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GPU() {return {reinterpret_cast<Fake*>(this)->gpu};}
};
void seenCreation(ID3D12Device* self,const void* desc,REFIID iid,void** out) {
    lastDevice=self;lastDesc=desc;lastIID=iid;lastOutput=out;
    if(SUCCEEDED(result) && out) *out=nextObject;
}
template<int N> HRESULT STDMETHODCALLTYPE heapCreate(ID3D12Device* self,const D3D12_DESCRIPTOR_HEAP_DESC* desc,REFIID iid,void** out) {
    if(concurrentGate) {
        CHECK(desc && desc->Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && iid==__uuidof(ID3D12DescriptorHeap) && out);
        concurrentGate->meet(4);unlockedNativeProbe();concurrentGate->meet(4);
        *out=concurrentObject;++concurrentForwards;return S_FALSE;
    }
    ++heapCalls[N];seenCreation(self,desc,iid,out);return result;
}
template<int N> HRESULT STDMETHODCALLTYPE resourceCreate(ID3D12Device* self,const D3D12_HEAP_PROPERTIES* props,D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    if(concurrentGate) {
        CHECK(props && flags==D3D12_HEAP_FLAG_NONE && desc && desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER);
        CHECK(initial==D3D12_RESOURCE_STATE_COMMON && !clear && iid==__uuidof(ID3D12Resource) && out);
        concurrentGate->meet(4);unlockedNativeProbe();concurrentGate->meet(4);*out=concurrentObject;++concurrentForwards;return S_FALSE;
    }
    ++resourceCalls[N];lastProps=props;lastFlags=flags;lastInitial=initial;lastClear=clear;
    seenCreation(self,desc,iid,out);
    if(creationAction) {auto action=std::move(creationAction);creationAction={};unlockedNativeProbe();action();}
    if(waitCreate) {
        std::unique_lock lock(waitMutex);writeEntered=true;waitCondition.notify_all();
        CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeContinue;}));
        // Model a newly returned lifetime at the retiring object's address.
        reinterpret_cast<Fake*>(*out)->refs=1;
    }
    return result;
}
HRESULT STDMETHODCALLTYPE placed(ID3D12Device* self,ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    lastHeap=heap;lastOffset=offset;lastInitial=initial;lastClear=clear;seenCreation(self,desc,iid,out);
    if(waitCreate) {
        unlockedNativeProbe();
        std::unique_lock lock(waitMutex);writeEntered=true;waitCondition.notify_all();
        CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeContinue;}));
    }
    return result;
}
HRESULT STDMETHODCALLTYPE reserved(ID3D12Device* self,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    lastInitial=initial;lastClear=clear;seenCreation(self,desc,iid,out);
    if(waitCreate) {
        unlockedNativeProbe();
        std::unique_lock lock(waitMutex);writeEntered=true;waitCondition.notify_all();
        CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeContinue;}));
    }
    return result;
}
HRESULT STDMETHODCALLTYPE createListFake(ID3D12Device* self,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* pso,REFIID iid,void** out) {
    CHECK(!node && type==D3D12_COMMAND_LIST_TYPE_DIRECT && !allocator && !pso);
    seenCreation(self,nullptr,iid,out);return result;
}
void STDMETHODCALLTYPE srv(ID3D12Device* self,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if(concurrentGate) {
        CHECK(resource==reinterpret_cast<ID3D12Resource*>(concurrentObject) && desc && desc->Buffer.NumElements==1);
        concurrentGate->meet(4);unlockedNativeProbe();concurrentGate->meet(4);++concurrentForwards;return;
    }
    ++writes;lastDevice=self;lastResource=resource;lastDesc=desc;lastDest=dest;
    nestedPointRejected=!acz::resolveNativeTableBinding(probeState,0,probeHeap,0);
    if(waitWrite) {
        std::unique_lock lock(waitMutex);writeEntered=true;waitCondition.notify_all();
        waitCondition.wait(lock,[]{return writeContinue;});
    }
    if(nestedAction) {auto action=std::move(nestedAction);nestedAction={};unlockedNativeProbe();action(self,dest);}
    if(reenterWrite) {reenterWrite=false;self->CreateShaderResourceView(resource,desc,dest);}
}
void STDMETHODCALLTYPE cbv(ID3D12Device* self,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    ++writes;lastDevice=self;lastDesc=desc;lastDest=dest;
}
void STDMETHODCALLTYPE uav(ID3D12Device* self,ID3D12Resource* resource,ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    ++writes;lastDevice=self;lastResource=resource;lastCounter=counter;lastDesc=desc;lastDest=dest;
}
template<class T> void STDMETHODCALLTYPE view(ID3D12Device* self,ID3D12Resource* resource,const T* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    ++writes;lastDevice=self;lastResource=resource;lastDesc=desc;lastDest=dest;
}
void STDMETHODCALLTYPE sampler(ID3D12Device* self,const D3D12_SAMPLER_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    ++writes;lastDevice=self;lastDesc=desc;lastDest=dest;
}
void STDMETHODCALLTYPE copy(ID3D12Device* self,UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* d,const UINT* ds,
    UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* s,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    ++copies;lastDevice=self;lastCount=dc;lastDests=d;lastDestSizes=ds;lastSourceCount=sc;lastSources=s;lastSourceSizes=ss;lastType=type;
}
void STDMETHODCALLTYPE simple(ID3D12Device* self,UINT n,D3D12_CPU_DESCRIPTOR_HANDLE d,D3D12_CPU_DESCRIPTOR_HANDLE s,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    ++copies;lastDevice=self;lastCount=n;lastDest=d;lastSource=s;lastType=type;
}
std::array<void*,44> deviceTable(bool b) {
    std::array<void*,44> t{};t.fill(reinterpret_cast<void*>(&sentinel));
    t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&add);t[2]=reinterpret_cast<void*>(&release);
    t[12]=reinterpret_cast<void*>(&createListFake);
    t[7]=reinterpret_cast<void*>(&nodeCount);t[15]=reinterpret_cast<void*>(&increment);
    t[14]=b?reinterpret_cast<void*>(&heapCreate<1>):reinterpret_cast<void*>(&heapCreate<0>);
    t[27]=b?reinterpret_cast<void*>(&resourceCreate<1>):reinterpret_cast<void*>(&resourceCreate<0>);
    t[17]=reinterpret_cast<void*>(&cbv);t[18]=reinterpret_cast<void*>(&srv);t[19]=reinterpret_cast<void*>(&uav);
    t[20]=reinterpret_cast<void*>(&view<D3D12_RENDER_TARGET_VIEW_DESC>);t[21]=reinterpret_cast<void*>(&view<D3D12_DEPTH_STENCIL_VIEW_DESC>);
    t[22]=reinterpret_cast<void*>(&sampler);t[23]=reinterpret_cast<void*>(&copy);t[24]=reinterpret_cast<void*>(&simple);
    t[29]=reinterpret_cast<void*>(&placed);t[30]=reinterpret_cast<void*>(&reserved);return t;
}
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE directListType(ID3D12CommandList*) {return D3D12_COMMAND_LIST_TYPE_DIRECT;}
HRESULT STDMETHODCALLTYPE noListDevice(ID3D12GraphicsCommandList*,REFIID,void** out){*out=nullptr;return E_NOINTERFACE;}
static unsigned resetCalls=0;
HRESULT STDMETHODCALLTYPE resetList(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*) {++resetCalls;return S_OK;}
void STDMETHODCALLTYPE clearList(ID3D12GraphicsCommandList*,ID3D12PipelineState*) {}
void STDMETHODCALLTYPE setListHeaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*) {}
void STDMETHODCALLTYPE tableList(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) {}
void STDMETHODCALLTYPE addressList(ID3D12GraphicsCommandList*,UINT,UINT64) {}
void STDMETHODCALLTYPE rootList(ID3D12GraphicsCommandList*,ID3D12RootSignature*) {}
void STDMETHODCALLTYPE constantsList(ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT) {}
uint64_t id(void* p) {return reinterpret_cast<uintptr_t>(p);}
struct CompatFake {void** table;Fake* owner;};
static const D3D11_RESOURCE_FLAGS* seenFlags11=nullptr;
static ID3D12LifetimeTracker* seenLifetime=nullptr;
static ID3D12SwapChainAssistant* seenSwapchain=nullptr;
static D3D12_COMPATIBILITY_SHARED_FLAGS seenCompatFlags{};
static bool throwCompat=false;
HRESULT STDMETHODCALLTYPE compatQuery(ID3D12CompatibilityDevice* self,REFIID iid,void** out) {
    unlockedNativeProbe();auto owner=reinterpret_cast<CompatFake*>(self)->owner;
    if(queryFails) {*out=nullptr;return E_NOINTERFACE;}
    ++owner->refs;*out=iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12Device)?static_cast<void*>(owner):self;return S_OK;
}
ULONG STDMETHODCALLTYPE compatAdd(ID3D12CompatibilityDevice* self) {return ++reinterpret_cast<CompatFake*>(self)->owner->refs;}
ULONG STDMETHODCALLTYPE compatRelease(ID3D12CompatibilityDevice* self) {unlockedNativeProbe();return --reinterpret_cast<CompatFake*>(self)->owner->refs;}
HRESULT STDMETHODCALLTYPE compatResource(ID3D12CompatibilityDevice* self,const D3D12_HEAP_PROPERTIES* props,D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,const D3D11_RESOURCE_FLAGS* flags11,
    D3D12_COMPATIBILITY_SHARED_FLAGS cf,ID3D12LifetimeTracker* lifetime,ID3D12SwapChainAssistant* swapchain,REFIID iid,void** out) {
    if(throwCompat)throw std::runtime_error("native compatibility exception");
    if(!concurrentGate){seenFlags11=flags11;seenCompatFlags=cf;seenLifetime=lifetime;seenSwapchain=swapchain;}
    return resourceCreate<0>(reinterpret_cast<ID3D12Device*>(reinterpret_cast<CompatFake*>(self)->owner),props,flags,desc,initial,clear,iid,out);
}
HRESULT STDMETHODCALLTYPE compatHeap(ID3D12CompatibilityDevice*,const D3D12_HEAP_DESC* desc,D3D12_COMPATIBILITY_SHARED_FLAGS cf,REFIID iid,void** out) {
    unlockedNativeProbe();lastDesc=desc;seenCompatFlags=cf;lastIID=iid;lastOutput=out;
    if(out && SUCCEEDED(result))*out=nextObject;return result;
}
HRESULT STDMETHODCALLTYPE compatReflect(ID3D12CompatibilityDevice*,ID3D12Object* object,D3D12_REFLECT_SHARED_PROPERTY kind,void* data,UINT bytes) {
    unlockedNativeProbe();CHECK(object==reinterpret_cast<ID3D12Object*>(0x1234));
    CHECK(kind==D3D12_REFLECT_SHARED_PROPERTY_D3D11_RESOURCE_FLAGS && bytes==sizeof(UINT));
    *static_cast<UINT*>(data)=71;return S_FALSE;
}
HRESULT STDMETHODCALLTYPE privateGet(ID3D12Device*,REFGUID,UINT* bytes,void*) {*bytes=73;return S_FALSE;}
HRESULT STDMETHODCALLTYPE privateSet(ID3D12Device*,REFGUID,UINT bytes,const void*) {CHECK(bytes==74);return S_FALSE;}
HRESULT STDMETHODCALLTYPE privateInterface(ID3D12Device*,REFGUID,const IUnknown*) {return S_FALSE;}
void compatibilityTests(ID3D12Device* dev,Fake& device) {
    auto& s=acz::state();auto reset=[&]{s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) acz::binding::NativeBindingObserver;};
    auto& observer=s.nativeBindings;
    static std::array<void*,6> ct{},ct2{},failedTable{};
    ct={reinterpret_cast<void*>(&compatQuery),reinterpret_cast<void*>(&compatAdd),reinterpret_cast<void*>(&compatRelease),
        reinterpret_cast<void*>(&compatResource),reinterpret_cast<void*>(&compatHeap),reinterpret_cast<void*>(&compatReflect)};
    ct2=ct;failedTable=ct;
    static CompatFake c{},peer{},c2{},failed{};
    c={ct.data(),&device};peer={ct.data(),&device};c2={ct2.data(),&device};failed={failedTable.data(),&device};
    auto compat=reinterpret_cast<ID3D12CompatibilityDevice*>(&c);
    auto acquire=[&](CompatFake& object) {
        compatibilityOutput=&object;void* out=nullptr;auto before=device.refs;
        CHECK(dev->QueryInterface(__uuidof(ID3D12CompatibilityDevice),&out)==S_OK);
        CHECK(out==&object && device.refs==before+1);compatibilityOutput=nullptr;
        return reinterpret_cast<ID3D12CompatibilityDevice*>(out);
    };
    auto before=device.refs;
    compat=acquire(c);CHECK(observer.enabled());
    CHECK(ct[0]==reinterpret_cast<void*>(&acz::queryCompatibilityDevice) && ct[2]==reinterpret_cast<void*>(&acz::releaseCompatibilityDevice));
    CHECK(ct[3]==reinterpret_cast<void*>(&acz::createCompatibilityResource) && ct[4]==reinterpret_cast<void*>(&acz::createCompatibilityHeap));
    CHECK(ct[5]==reinterpret_cast<void*>(&compatReflect));
    CHECK(s.compatibility.at(ct.data()).originals[3]==reinterpret_cast<void*>(&compatResource));
    acz::g_protect=protectFailure;protectCount=0;
    auto cp=acquire(peer);CHECK(observer.enabled() && !protectCount);CHECK(cp->Release()==before+1);
    auto second=acquire(c2);CHECK(observer.enabled() && protectCount==8);CHECK(second->Release()==before+1);
    protectCount=0;protectFail=4;
    auto partial=acquire(failed);CHECK(!observer.enabled());
    CHECK(s.compatibility.at(failedTable.data()).pendingProtection[2]==PAGE_READONLY);
    CHECK(partial->Release()==before+1);reset();protectFail=0;
    partial=acquire(failed);CHECK(observer.enabled());CHECK(partial->Release()==before+1);
    CHECK(!s.compatibility.at(failedTable.data()).pendingProtection[2]);
    acz::g_protect=protectOK;
    acz::g_pinResult=false;second=acquire(c2);CHECK(!observer.enabled());CHECK(second->Release()==before+1);
    acz::g_pinResult=true;reset();second=acquire(c2);CHECK(observer.enabled());CHECK(second->Release()==before+1);
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=8192;
    rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
    static std::array<void*,15> rt{};rt.fill(reinterpret_cast<void*>(&sentinel));
    rt[0]=reinterpret_cast<void*>(&query);rt[1]=reinterpret_cast<void*>(&add);rt[2]=reinterpret_cast<void*>(&release);rt[11]=reinterpret_cast<void*>(&gpuAddress);
    static std::array<Fake,24> buffers{};
    for(size_t i=0;i<buffers.size();++i)buffers[i]={rt.data(),1,0,0x700000+0x10000*i};
    auto create=[&](Fake* buffer) {
        const auto* desc=&rd;
        nextObject=buffer;void* out=nullptr;
        CHECK(compat->CreateSharedResource(&props,D3D12_HEAP_FLAG_NONE,desc,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
            D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
        CHECK(out==buffer);return static_cast<ID3D12Resource*>(out);
    };
    auto resource=create(&buffers[0]);CHECK(observer.enabled() && buffers[0].refs==1);
    CHECK(lastProps==&props && lastDesc==&rd && lastFlags==0 && lastInitial==D3D12_RESOURCE_STATE_COMMON && !lastClear);
    CHECK(!seenFlags11 && !seenLifetime && !seenSwapchain && seenCompatFlags==0 && lastIID==__uuidof(ID3D12Resource));
    acz::capture::State point;point.list_identity=123;point.tracking_complete=true;
    point.roots[0].kind=acz::metadata::ParameterKind::cbv;point.roots[0].value=buffers[0].gpu;
    auto resolution=observer.root(point,0,256,0,0,id(dev));CHECK(resolution && observer.current(resolution,point,id(dev)));
    auto token=observer.resource_token(id(resource));CHECK(resource->Release()==0 && !observer.current(resolution,point,id(dev)));
    buffers[0].refs=1;resource=create(&buffers[0]);CHECK(observer.resource_token(id(resource))!=token);
    observer.remove_resource(token);CHECK(observer.resource_token(id(resource)).id); // Stale retirement cannot erase new token.
    resource=create(&buffers[0]);CHECK(observer.enabled() && !observer.resource_token(id(resource)).id); // Shared alias tombstoned.
    resource=create(&buffers[0]);CHECK(!observer.resource_token(id(resource)).id);
    for(unsigned form=0;form<9;++form) {
        auto desc=rd;auto hp=props;
        if(form==0)desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        if(form==1)hp.VisibleNodeMask=3;
        if(form==2)desc.Flags=static_cast<D3D12_RESOURCE_FLAGS>(0x8000);
        nextObject=&buffers[form+1];void* out=nullptr;D3D11_RESOURCE_FLAGS f11{};D3D12_CLEAR_VALUE clear{};
        auto lifetime=reinterpret_cast<ID3D12LifetimeTracker*>(uintptr_t(form==6?0x1234:0));
        auto swapchain=reinterpret_cast<ID3D12SwapChainAssistant*>(uintptr_t(form==7?0x5678:0));
        CHECK(compat->CreateSharedResource(&hp,form==3?D3D12_HEAP_FLAG_SHARED:D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COPY_DEST,&clear,form==5?&f11:nullptr,
            form==4?static_cast<D3D12_COMPATIBILITY_SHARED_FLAGS>(0x80):D3D12_COMPATIBILITY_SHARED_FLAG_NONE,
            lifetime,swapchain,form==8?__uuidof(IUnknown):__uuidof(ID3D12Resource),&out)==S_FALSE);
        CHECK(out==nextObject && lastProps==&hp && lastDesc==&desc && lastClear==&clear && lastInitial==D3D12_RESOURCE_STATE_COPY_DEST);
        CHECK(seenLifetime==lifetime && seenSwapchain==swapchain && seenFlags11==(form==5?&f11:nullptr));
        CHECK(observer.enabled() && !observer.resource_token(id(out)).id);
        create(&buffers[form+1]);CHECK(!observer.resource_token(id(out)).id); // Reuse remains unprovable.
    }
    void* out=&buffers[12];result=E_FAIL;
    CHECK(compat->CreateSharedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
        D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==E_FAIL);
    CHECK(out==&buffers[12] && observer.enabled());result=S_FALSE;
    create(&buffers[12]);CHECK(!observer.resource_token(id(&buffers[12])).id);
    create(nullptr);CHECK(observer.enabled());
    CHECK(compat->CreateSharedResource(nullptr,D3D12_HEAP_FLAG_NONE,nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
        D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),nullptr)==S_FALSE && observer.enabled());
    D3D12_HEAP_DESC heap{};nextObject=&buffers[13];out=nullptr;
    CHECK(compat->CreateSharedHeap(&heap,D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX,__uuidof(ID3D12Heap),&out)==S_FALSE);
    CHECK(out==nextObject && lastDesc==&heap && lastOutput==&out && lastIID==__uuidof(ID3D12Heap) && seenCompatFlags==D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX);
    CHECK(observer.enabled() && !observer.heap_token(id(out)).id);create(&buffers[13]);CHECK(!observer.resource_token(id(out)).id);
    UINT data=0;auto clock=acz::bindingCohort().clock;
    CHECK(compat->ReflectSharedProperties(reinterpret_cast<ID3D12Object*>(0x1234),D3D12_REFLECT_SHARED_PROPERTY_D3D11_RESOURCE_FLAGS,&data,sizeof data)==S_FALSE);
    CHECK(data==71 && acz::bindingCohort().clock==clock);
    create(&buffers[19]);CHECK(observer.resource_token(id(&buffers[19])).id);
    canonicalAliasOutput=reinterpret_cast<IUnknown*>(&buffers[20]);canonicalAliasTarget=&buffers[19];
    rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;create(&buffers[20]);rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    CHECK(observer.enabled() && observer.untracked(id(&buffers[20])) && observer.untracked(id(&buffers[19])));
    CHECK(!observer.resource_token(id(&buffers[19])).id && buffers[19].refs==1);
    canonicalAliasOutput=nullptr;canonicalAliasTarget=nullptr;
    // Nested creations preserve the existing committed-create rejection rule.
    creationAction=[&]{create(&buffers[17]);};create(&buffers[16]);
    CHECK(!observer.enabled());
    CHECK(std::strcmp(observer.disable_reason(),"overlap:committed_create/committed_create:same")==0);
    CHECK(reinterpret_cast<ID3D12Resource*>(&buffers[16])->Release()==0);
    CHECK(reinterpret_cast<ID3D12Resource*>(&buffers[17])->Release()==0);reset();
    for(unsigned iteration=0;iteration<200;++iteration) {
        Rendezvous gate,checkpoint;std::array<Fake,4> objects;std::array<std::thread,4> threads;
        for(unsigned n=0;n<4;++n) {
            objects[n]={rt.data(),1,0,0x900000+0x10000*n};
            threads[n]=std::thread([&,n]{
                concurrentGate=&gate;concurrentObject=&objects[n];void* returned=nullptr;
                CHECK(compat->CreateSharedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
                    D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&returned)==S_FALSE);
                CHECK(returned==&objects[n] && objects[n].refs==1);
                checkpoint.meet(5);checkpoint.meet(5);
                concurrentRelease=true;CHECK(static_cast<ID3D12Resource*>(returned)->Release()==0);
                concurrentRelease=false;concurrentGate=nullptr;
            });
        }
        checkpoint.meet(5);
        {std::lock_guard lock(s.mutex);CHECK(observer.enabled());
         for(auto& object:objects)CHECK(observer.resource_token(id(&object)).id);}
        checkpoint.meet(5);for(auto& thread:threads)thread.join();
        CHECK(observer.enabled());for(auto& object:objects)CHECK(!observer.resource_token(id(&object)).id);
    }
    // Metadata allocation failure is a global observation gap, never a pass.
    allocation_budget=0;create(&buffers[18]);allocation_budget=-1;
    CHECK(!observer.enabled());reset();
    throwAddress=true;create(&buffers[14]);throwAddress=false;CHECK(observer.enabled() && !observer.resource_token(id(&buffers[14])).id);
    throwCompat=true;bool threw=false;
    try {create(&buffers[15]);}catch(const std::runtime_error&){threw=true;}throwCompat=false;CHECK(threw && !observer.enabled());reset();
    queryFails=true;out=nullptr;CHECK(compat->QueryInterface(__uuidof(ID3D12CompatibilityDevice),&out)==E_NOINTERFACE && !out && observer.enabled());queryFails=false;
    for(auto iid:{__uuidof(ID3D11On12Device),__uuidof(ID3D11On12Device1),__uuidof(ID3D11On12Device2),__uuidof(ID3D12DeviceChild)}) {
        out=nullptr;auto refs=device.refs;CHECK(compat->QueryInterface(iid,&out)==S_OK && out==compat && device.refs==refs+1);
        CHECK(!observer.enabled());CHECK(static_cast<IUnknown*>(out)->Release()==refs);reset();
    }
    // Same table/device address rejection must preserve SDK slots 3/4/5 dispatch.
    device.table[3]=reinterpret_cast<void*>(&privateGet);device.table[4]=reinterpret_cast<void*>(&privateSet);device.table[5]=reinterpret_cast<void*>(&privateInterface);
    auto untouched=std::array<void*,3>{device.table[3],device.table[4],device.table[5]};out=nullptr;
    CHECK(dev->QueryInterface(__uuidof(ID3D12CompatibilityDevice),&out)==S_OK && out==dev && !observer.enabled());
    CHECK(device.table[3]==untouched[0] && device.table[4]==untouched[1] && device.table[5]==untouched[2]);
    UINT bytes=0;CHECK(dev->GetPrivateData(__uuidof(IUnknown),&bytes,nullptr)==S_FALSE && bytes==73);
    CHECK(dev->SetPrivateData(__uuidof(IUnknown),74,nullptr)==S_FALSE && dev->SetPrivateDataInterface(__uuidof(IUnknown),nullptr)==S_FALSE);
    CHECK(dev->Release()==before+1);reset();
    CHECK(compat->Release()==before);CHECK(observer.enabled());
    compat=acquire(c);CHECK(dev->Release()==before);
    CHECK(compat->Release()==0 && !observer.enabled());
    CHECK(!s.compatibilityOwners.count(reinterpret_cast<IUnknown*>(compat)));
    device.refs=before;reset(); // Explicitly new fake device lifetime, no runtime claim.
    nextObject=nullptr;probeRelease=false;resourceCalls[0]=resourceCalls[1]=0;
    std::printf("Compatibility fake COM: distinct/shared tables, partial repair, buffer provenance, per-call tombstones, forwarding and alias QI checked\n");
}
// J-compat-race fixtures: a hooked compatibility device for the scenario modes.
static std::array<void*,6> modeCompatTable{};
static CompatFake modeCompat{};
static ID3D12CompatibilityDevice* acquireModeCompat(ID3D12Device* dev,Fake& device) {
    modeCompatTable={reinterpret_cast<void*>(&compatQuery),reinterpret_cast<void*>(&compatAdd),reinterpret_cast<void*>(&compatRelease),
        reinterpret_cast<void*>(&compatResource),reinterpret_cast<void*>(&compatHeap),reinterpret_cast<void*>(&compatReflect)};
    modeCompat={modeCompatTable.data(),&device};
    compatibilityOutput=&modeCompat;void* out=nullptr;
    CHECK(dev->QueryInterface(__uuidof(ID3D12CompatibilityDevice),&out)==S_OK && out==&modeCompat);
    compatibilityOutput=nullptr;
    CHECK(modeCompatTable[3]==reinterpret_cast<void*>(&acz::createCompatibilityResource));
    CHECK(modeCompatTable[4]==reinterpret_cast<void*>(&acz::createCompatibilityHeap));
    CHECK(acz::state().nativeBindings.enabled());
    return reinterpret_cast<ID3D12CompatibilityDevice*>(out);
}
// Number of captured creation lines naming this object/outcome/thread and containing
// `extra` (a trailing '$' requires `extra` to END the line, e.g. an empty reason).
static unsigned creationLines(const char* object,const char* outcome,unsigned long thread,const char* extra="") {
    char head[160];std::snprintf(head,sizeof head,"binding-creation;object=%s;outcome=%s;thread=%lu;",object,outcome,thread);
    std::string want=extra;const bool suffix=!want.empty()&&want.back()=='$';if(suffix)want.pop_back();
    const auto n=(std::min)(acz::g_testCreationLines.count.load(),acz::TestCreationLines::capacity);unsigned found=0;
    for(unsigned i=0;i<n;++i) {
        const std::string_view t=acz::g_testCreationLines.text[i];
        if(t.substr(0,std::strlen(head))!=head)continue;
        if(suffix?(t.size()>=want.size()&&t.substr(t.size()-want.size())==want):t.find(want)!=std::string_view::npos)++found;
    }
    return found;
}
// An SRV write to a CPU handle in no registered heap: an UNKNOWN binding footprint, held
// inside the native call (fake srv waitWrite) until release() -- the game-thread side of
// the headset-test-2 race.
struct HeldUnknownWrite {
    std::thread worker;unsigned long thread=0;
    HeldUnknownWrite(ID3D12Device* dev,ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* sd) {
        {std::lock_guard lock(waitMutex);writeEntered=false;writeContinue=false;}waitWrite=true;
        std::atomic<unsigned long> tid{0};
        worker=std::thread([&tid,dev,resource,sd]{tid=GetCurrentThreadId();dev->CreateShaderResourceView(resource,sd,{0xdead0000});});
        std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));
        thread=tid.load();
    }
    void release() {
        {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();
        if(worker.joinable())worker.join();waitWrite=false;
    }
    ~HeldUnknownWrite(){release();}
};
static D3D12_RESOURCE_DESC swapchainImageDesc() { // the 24 'unsupported form' images of headset test 2
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=2400;d.Height=1350;d.DepthOrArraySize=1;d.MipLevels=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;return d;
}
int main(int argc,char** argv) {
    textureDescriptionCases();descriptorTableDiagnosticCases();observerDescriptorDiagnostics();
    binding_sdk_abi_checked();acz::g_protect=protectOK;
    auto da=deviceTable(false),db=deviceTable(true);
    Fake a{da.data()},peer{da.data()},b{db.data()};
    auto dev=reinterpret_cast<ID3D12Device*>(&a),shared=reinterpret_cast<ID3D12Device*>(&peer),distinct=reinterpret_cast<ID3D12Device*>(&b);
    CHECK(acz::hookNativeBindingDevice(dev));CHECK(acz::hookNativeBindingDevice(shared));CHECK(acz::hookNativeBindingDevice(distinct));
    const IID companions[]={__uuidof(ID3D12InfoQueue),__uuidof(ID3D12InfoQueue1),
        __uuidof(ID3D12DebugDevice),__uuidof(ID3D12DebugDevice1),__uuidof(ID3D12DebugDevice2),
        __uuidof(ID3D12DeviceRemovedExtendedData),__uuidof(ID3D12DeviceRemovedExtendedData1),
        __uuidof(ID3D12DeviceRemovedExtendedDataSettings),__uuidof(ID3D12DeviceRemovedExtendedDataSettings1),
        __uuidof(ID3D12DeviceConfiguration),__uuidof(ID3D12Tools),__uuidof(ID3D12Tools1)};
    for(const auto& iid:companions) {
        const auto refs=a.refs;void* queried=nullptr;
        CHECK(dev->QueryInterface(iid,&queried)==S_OK&&queried==dev&&a.refs==refs+1);
        CHECK(acz::state().nativeBindings.enabled());CHECK(dev->Release()==refs);
    }
    CHECK(da[15]==reinterpret_cast<void*>(&increment) && da[28]==reinterpret_cast<void*>(&sentinel));
    if(argc==1)compatibilityTests(dev,a);
    std::array<void*,15> ht{},rt{},rt2{};ht.fill(reinterpret_cast<void*>(&sentinel));rt=ht;rt2=ht;
    HandleDispatch hd;auto handles=*reinterpret_cast<void***>(&hd);
    ht[0]=reinterpret_cast<void*>(&query);ht[1]=reinterpret_cast<void*>(&add);ht[2]=reinterpret_cast<void*>(&release);
    ht[9]=handles[0];ht[10]=handles[1];
    rt[0]=reinterpret_cast<void*>(&query);rt[1]=reinterpret_cast<void*>(&add);rt[2]=reinterpret_cast<void*>(&release);rt[11]=reinterpret_cast<void*>(&gpuAddress);
    rt2=rt;
    Fake h{ht.data(),1,0x1000,0x8000},src{ht.data(),1,0x2000,0},r{rt.data(),1,0,0x10000},r2{rt2.data(),1,0,0x20000};
    auto heap=reinterpret_cast<ID3D12DescriptorHeap*>(&h),sourceHeap=reinterpret_cast<ID3D12DescriptorHeap*>(&src);
    auto resource=reinterpret_cast<ID3D12Resource*>(&r),otherResource=reinterpret_cast<ID3D12Resource*>(&r2);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,128,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    void* out=nullptr;nextObject=&h;probeRelease=true;
    CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
    CHECK(out==&h && h.refs==1 && lastDevice==dev && lastDesc==&heapDesc && lastOutput==&out && lastIID==__uuidof(ID3D12DescriptorHeap));
    heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;nextObject=&src;
    CHECK(shared->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);CHECK(src.refs==1 && heapCalls[0]==2 && lastIID==__uuidof(ID3D12DescriptorHeap));
    CHECK(sourceHeap->Release()==0);src.refs=1;CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=8192;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;
    rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
    D3D12_CLEAR_VALUE clear{};nextObject=&r;
    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,&clear,__uuidof(ID3D12Resource),&out)==S_FALSE);
    CHECK(r.refs==1 && out==&r && lastProps==&props && lastFlags==0 && lastDesc==&rd && lastInitial==D3D12_RESOURCE_STATE_COPY_DEST && lastClear==&clear);
    nextObject=&r2;CHECK(distinct->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
    CHECK(r2.refs==1 && resourceCalls[0]==1 && resourceCalls[1]==1 && lastIID==__uuidof(ID3D12Resource));
    CHECK(otherResource->Release()==0);r2.refs=1;
    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
    result=E_INVALIDARG;out=reinterpret_cast<void*>(0x1234);
    CHECK(dev->CreateCommittedResource(nullptr,D3D12_HEAP_FLAG_SHARED,nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==E_INVALIDARG);
    CHECK(out==reinterpret_cast<void*>(0x1234) && lastProps==nullptr && lastFlags==D3D12_HEAP_FLAG_SHARED && lastDesc==nullptr);
    result=S_FALSE;CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(IUnknown),nullptr)==S_FALSE);
    CHECK(lastOutput==nullptr); // Validation-only creation produces no metadata.
    if(argc>1) {
        CHECK(acz::state().nativeBindings.enabled());
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Buffer={0,1,416,D3D12_BUFFER_SRV_FLAG_NONE};
        const std::string mode=argv[1];
        if(mode=="proxy-scope") {
            // J-xr-stereo-output: creations inside the proxy-internal creation scope
            // (the XR owner thread's runtime calls) are forwarded natively and never
            // tracked or disabling; a registry-rejected heap (NodeMask=1) from ANOTHER
            // thread is tolerated only while an XR call window is open; outside both,
            // the existing fail-closed rules hold.
            auto& observer=acz::state().nativeBindings;
            Fake h3{ht.data(),1,0x3000,0x9000},h4{ht.data(),1,0x4000,0xa000},h5{ht.data(),1,0x5000,0xb000};
            D3D12_DESCRIPTOR_HEAP_DESC rejected{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,16,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,1};
            const auto heapsBefore=heapCalls[0];
            {acz::ProxyCreationScope scope;
             CHECK(acz::proxyCreationScopeActive() && acz::xrCallWindowActive());
             nextObject=&h3;CHECK(dev->CreateDescriptorHeap(&rejected,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
             CHECK(out==&h3 && h3.refs==1 && lastDesc==&rejected && heapCalls[0]==heapsBefore+1); // native forwarding exact
             CHECK(observer.enabled() && observer.foreign_heaps()==1 && !observer.heap_token(id(&h3)).id);
             nextObject=&r2;CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
             CHECK(lastFlags==D3D12_HEAP_FLAG_SHARED && out==&r2 && observer.enabled());
             void* queried=nullptr;
             CHECK(dev->QueryInterface(__uuidof(ID3D12DeviceChild),&queried)==S_OK && queried==dev);--a.refs;
             CHECK(observer.enabled());
             auto n=writes;dev->CreateShaderResourceView(resource,&sd,{h3.cpu});CHECK(writes==n+1 && observer.enabled());
             CHECK(acz::state().bindingEvents==0);}
            CHECK(!acz::proxyCreationScopeActive() && !acz::xrCallWindowActive());
            // Another thread is inside a window call (e.g. xrEndFrame) while this
            // thread (a runtime worker) creates a rejected heap and a shared buffer.
            std::atomic<int> phase{0};
            std::thread owner([&]{acz::ProxyCreationScope scope;phase=1;while(phase.load()!=2)std::this_thread::yield();});
            while(phase.load()!=1)std::this_thread::yield();
            CHECK(!acz::proxyCreationScopeActive() && acz::xrCallWindowActive());
            nextObject=&h4;CHECK(dev->CreateDescriptorHeap(&rejected,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
            CHECK(out==&h4 && observer.enabled() && observer.tolerated_rejections()==1 && !observer.heap_token(id(&h4)).id);
            CHECK(std::strstr(observer.last_rejection(),"node mask")!=nullptr);
            // Astra issue 29: a concurrent GAME allocation meets the same condition, so the
            // tolerated object is ORIGIN-UNKNOWN (tombstoned), never proxy-owned: a later
            // valid registration at that address still refuses to track it.
            CHECK(observer.untracked(id(&h4)) && acz::xrWindowTolerated()==1);
            D3D12_DESCRIPTOR_HEAP_DESC valid{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,16,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            nextObject=&h4;CHECK(dev->CreateDescriptorHeap(&valid,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
            CHECK(observer.enabled() && !observer.heap_token(id(&h4)).id);
            nextObject=&r2;CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            CHECK(observer.enabled() && observer.untracked(id(&r2)) && !observer.resource_token(id(&r2)).id && acz::xrWindowTolerated()==2);
            // A non-scope window call (xrWaitFrame-like) opens no window.
            phase=2;owner.join();
            {acz::ProxyCreationScope blocking(false);CHECK(acz::proxyCreationScopeActive() && !acz::xrCallWindowActive());}
            // Outside every scope/window: the game's own rejected heap still disables.
            nextObject=&h5;CHECK(dev->CreateDescriptorHeap(&rejected,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
            CHECK(!observer.enabled() && std::strcmp(observer.disable_reason(),"heap registration rejected")==0);
            std::printf("PASS %u checks: proxy creation scope ignores, XR window tolerates, game rejection disables\n",checks.load());return 0;
        } else if(mode=="proxy-window-expired") {
            // A bracketed XR call that never returns: the window tolerance expires after
            // kXrWindowCapMs and is never extended; the rejection then disables as before.
            auto& observer=acz::state().nativeBindings;
            Fake h6{ht.data(),1,0x6000,0xc000};
            D3D12_DESCRIPTOR_HEAP_DESC rejected{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,16,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,1};
            std::atomic<int> phase{0};
            std::thread stuck([&]{acz::ProxyCreationScope scope;phase=1;while(phase.load()!=2)std::this_thread::yield();});
            while(phase.load()!=1)std::this_thread::yield();
            CHECK(acz::xrCallWindowActive());
            std::this_thread::sleep_for(std::chrono::milliseconds(acz::kXrWindowCapMs+150));
            CHECK(acz::xrCallWindowOpen() && !acz::xrCallWindowActive());
            nextObject=&h6;CHECK(dev->CreateDescriptorHeap(&rejected,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
            CHECK(out==&h6 && !observer.enabled() && std::strcmp(observer.disable_reason(),"heap registration rejected")==0);
            CHECK(acz::xrWindowTolerated()==0 && acz::xrWindowExpired()==1);
            phase=2;stuck.join();
            std::printf("PASS %u checks: stuck XR call does not extend the window tolerance\n",checks.load());return 0;
        } else if(mode=="proxy-scope-shared") {
            // Outside the scope and window a game-created SHARED buffer keeps the fail-closed disable.
            CHECK(!acz::xrCallWindowActive());
            nextObject=&r2;CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            CHECK(!acz::state().nativeBindings.enabled());
            CHECK(std::strcmp(acz::state().nativeBindings.disable_reason(),"resource creation layout unsupported")==0);
            std::printf("PASS %u checks: game shared buffer outside the XR window still disables\n",checks.load());return 0;
        } else if(mode=="compat-proxy-scope") {
            // J-compat-race (a)(c)(d): SteamVR's runtime creates swapchain images through
            // ID3D12CompatibilityDevice::CreateSharedResource/CreateSharedHeap inside the XR
            // owner's proxy scope while a game thread is inside an unknown-footprint
            // descriptor write. Expected-red on master 4907fae (overlap:descriptor_write/
            // committed_create:cross).
            auto& observer=acz::state().nativeBindings;
            auto compat=acquireModeCompat(dev,a);
            const auto me=static_cast<unsigned long>(GetCurrentThreadId());
            // Runtime-owned outputs: ANY COM call on them (QI/AddRef/Release) hits sentinel().
            static std::array<void*,15> noCom{};noCom.fill(reinterpret_cast<void*>(&sentinel));
            static std::array<Fake,26> images{};for(auto& image:images)image={noCom.data(),1,0,0};
            auto tex=swapchainImageDesc();D3D11_RESOURCE_FLAGS f11{};f11.BindFlags=0x20;
            D3D12_HEAP_PROPERTIES texProps{};texProps.Type=D3D12_HEAP_TYPE_DEFAULT;texProps.CreationNodeMask=texProps.VisibleNodeMask=1;
            D3D12_HEAP_DESC heapDesc2{};heapDesc2.SizeInBytes=1<<20;heapDesc2.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
            {
                HeldUnknownWrite game(dev,resource,&sd);
                CHECK(acz::state().bindingEvents==1 && game.thread!=me);
                uint64_t clock=0;{std::lock_guard lock(acz::state().mutex);clock=acz::bindingCohort().clock;}
                {acz::ProxyCreationScope scope;
                 CHECK(acz::proxyCreationScopeActive() && acz::xrCallWindowActive());
                 nextObject=&images[0];out=nullptr;
                 CHECK(compat->CreateSharedResource(&texProps,D3D12_HEAP_FLAG_SHARED,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,&f11,
                     D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
                 CHECK(out==&images[0] && lastDesc==&tex && lastProps==&texProps && seenFlags11==&f11 && images[0].refs==1); // forwarded once, exact
                 const auto calls=resourceCalls[0];
                 nextObject=&images[1];out=nullptr;
                 CHECK(compat->CreateSharedHeap(&heapDesc2,D3D12_COMPATIBILITY_SHARED_FLAG_NONE,__uuidof(ID3D12Heap),&out)==S_FALSE);
                 CHECK(out==&images[1] && lastDesc==&heapDesc2 && resourceCalls[0]==calls);
                 CHECK(acz::state().bindingEvents==1 && observer.enabled()); // no cohort event was opened
                 {std::lock_guard lock(acz::state().mutex);CHECK(acz::bindingCohort().clock==clock);}
                 // (c) non-final Release and a D3D11On12 QI of the compatibility device inside the
                 // scope: native forwarding only, no release event, no owner change, no disable.
                 const auto refs=a.refs;void* q=nullptr;
                 CHECK(compat->QueryInterface(__uuidof(ID3D11On12Device),&q)==S_OK && q==compat && a.refs==refs+1);
                 CHECK(compat->Release()==refs && a.refs==refs);
                 CHECK(observer.enabled() && acz::state().compatibilityOwners.count(reinterpret_cast<IUnknown*>(compat)));
                 {std::lock_guard lock(acz::state().mutex);CHECK(acz::bindingCohort().clock==clock);}
                }
                game.release();
            }
            CHECK(observer.enabled() && acz::state().bindingEvents==0);
            // Never tracked, never tombstoned (a reused address stays trackable for the game).
            CHECK(!observer.resource_token(id(&images[0])).id && !observer.untracked(id(&images[0])) && !observer.untracked(id(&images[1])));
            CHECK(creationLines("compat-resource","ignored-proxy-scope",me,"xr-scope=1;xr-window=1;reason=$")==1);
            CHECK(creationLines("compat-heap","ignored-proxy-scope",me,"xr-scope=1;xr-window=1;")==1);
            // (d) a descriptor-write storm on a game thread concurrent with 24 in-scope creations.
            std::atomic<bool> stop{false};std::atomic<unsigned> stormWrites{0},during{0};std::atomic<bool> creating{false};
            std::thread storm([&]{
                while(!stop.load()) {
                    dev->CreateShaderResourceView(resource,&sd,{0xdead0000+uint64_t(stormWrites.load()%64)*32});
                    ++stormWrites;if(creating.load())++during;
                }
            });
            while(stormWrites.load()<16)std::this_thread::yield();
            {acz::ProxyCreationScope scope;creating=true;
             for(unsigned i=0;i<24;++i) {
                 nextObject=&images[2+i];out=nullptr;
                 CHECK(compat->CreateSharedResource(&texProps,D3D12_HEAP_FLAG_SHARED,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,&f11,
                     D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE && out==&images[2+i]);
                 const auto mark=stormWrites.load();while(stormWrites.load()<mark+2)std::this_thread::yield(); // interleave
                 CHECK(observer.enabled());
             }
             creating=false;}
            stop=true;storm.join();
            CHECK(observer.enabled() && during.load()>=24 && acz::state().bindingEvents==0);
            CHECK(creationLines("compat-resource","ignored-proxy-scope",me,"xr-scope=1;")==25);
            std::printf("PASS %u checks: compatibility creations inside the proxy scope never disable (%u concurrent game writes)\n",
                checks.load(),during.load());return 0;
        } else if(mode=="compat-game-overlap") {
            // J-compat-race (b): the SAME overlap outside the proxy scope and outside any XR
            // window keeps the fail-closed disable (a game compat creation's unknown footprint).
            auto& observer=acz::state().nativeBindings;
            auto compat=acquireModeCompat(dev,a);
            const auto me=static_cast<unsigned long>(GetCurrentThreadId());
            static std::array<Fake,2> images{};for(auto& image:images)image={rt.data(),1,0,0};
            auto tex=swapchainImageDesc();D3D11_RESOURCE_FLAGS f11{};
            D3D12_HEAP_PROPERTIES texProps{};texProps.Type=D3D12_HEAP_TYPE_DEFAULT;texProps.CreationNodeMask=texProps.VisibleNodeMask=1;
            CHECK(!acz::proxyCreationScopeActive() && !acz::xrCallWindowOpen());
            {
                HeldUnknownWrite game(dev,resource,&sd);
                nextObject=&images[0];out=nullptr;
                CHECK(compat->CreateSharedResource(&texProps,D3D12_HEAP_FLAG_SHARED,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,&f11,
                    D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE && out==&images[0]);
                CHECK(observer.untracked(id(&images[0]))); // unchanged: unsupported form tombstoned
                game.release();
            }
            CHECK(!observer.enabled());
            CHECK(std::strcmp(observer.disable_reason(),"overlap:descriptor_write/committed_create:cross")==0);
            CHECK(creationLines("compat-resource","observed",me,"xr-scope=0;xr-window=0;reason=arguments-unsupported")==1);
            CHECK(acz::xrWindowTolerated()==0);
            std::printf("PASS %u checks: game compatibility creation overlapping an unknown write still disables\n",checks.load());return 0;
        } else if(mode=="compat-xr-window") {
            // J-compat-race (e): the runtime creates the images on ITS OWN thread while the XR
            // owner is inside a window call. Arguments-unsupported forms (tombstoned on every
            // path) skip only the cohort event; a trackable buffer form keeps it and fails closed.
            auto& observer=acz::state().nativeBindings;
            auto compat=acquireModeCompat(dev,a);
            const auto me=static_cast<unsigned long>(GetCurrentThreadId());
            static std::array<Fake,4> images{};for(size_t i=0;i<images.size();++i)images[i]={rt.data(),1,0,0x800000+0x10000*i};
            auto tex=swapchainImageDesc();D3D11_RESOURCE_FLAGS f11{};
            D3D12_HEAP_PROPERTIES texProps{};texProps.Type=D3D12_HEAP_TYPE_DEFAULT;texProps.CreationNodeMask=texProps.VisibleNodeMask=1;
            D3D12_HEAP_DESC heapDesc2{};heapDesc2.SizeInBytes=1<<20;heapDesc2.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
            std::atomic<int> phase{0};
            std::thread owner([&]{acz::ProxyCreationScope scope;phase=1;while(phase.load()!=2)std::this_thread::yield();});
            while(phase.load()!=1)std::this_thread::yield();
            CHECK(!acz::proxyCreationScopeActive() && acz::xrCallWindowActive());
            {
                HeldUnknownWrite game(dev,resource,&sd);
                nextObject=&images[0];out=nullptr;
                CHECK(compat->CreateSharedResource(&texProps,D3D12_HEAP_FLAG_SHARED,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,&f11,
                    D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE && out==&images[0]);
                nextObject=&images[1];out=nullptr;
                CHECK(compat->CreateSharedHeap(&heapDesc2,D3D12_COMPATIBILITY_SHARED_FLAG_NONE,__uuidof(ID3D12Heap),&out)==S_FALSE && out==&images[1]);
                CHECK(acz::state().bindingEvents==1);
                game.release();
            }
            CHECK(observer.enabled() && acz::xrWindowTolerated()==2);
            // Counted exception, NOT ownership: both stay origin-unknown (tombstoned).
            CHECK(observer.untracked(id(&images[0])) && observer.untracked(id(&images[1])) && !observer.resource_token(id(&images[0])).id);
            CHECK(creationLines("compat-resource","untracked-origin-unknown-xr-window",me,"xr-scope=0;xr-window=1;reason=arguments-unsupported")==1);
            CHECK(creationLines("compat-heap","untracked-origin-unknown-xr-window",me,"xr-scope=0;xr-window=1;")==1);
            // A trackable BUFFER form inside the same window is not exempt: fail closed.
            D3D12_RESOURCE_DESC buffer=rd;
            {
                HeldUnknownWrite game(dev,resource,&sd);
                nextObject=&images[2];out=nullptr;
                CHECK(compat->CreateSharedResource(&props,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
                    D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE && out==&images[2]);
                game.release();
            }
            CHECK(!observer.enabled() && std::strcmp(observer.disable_reason(),"overlap:descriptor_write/committed_create:cross")==0);
            CHECK(creationLines("compat-resource","observed",me,"xr-scope=0;")==1 && acz::xrWindowTolerated()==2);
            phase=2;owner.join();
            std::printf("PASS %u checks: XR-window compatibility creations from another thread are counted origin-unknown\n",checks.load());return 0;
        } else if(mode=="compat-xr-window-expired") {
            // J-compat-race (e): a stuck window call never extends the exemption; afterwards the
            // runtime-thread overlap fails closed exactly as before.
            auto& observer=acz::state().nativeBindings;
            auto compat=acquireModeCompat(dev,a);
            static std::array<Fake,1> images{};images[0]={rt.data(),1,0,0};
            auto tex=swapchainImageDesc();D3D11_RESOURCE_FLAGS f11{};
            D3D12_HEAP_PROPERTIES texProps{};texProps.Type=D3D12_HEAP_TYPE_DEFAULT;texProps.CreationNodeMask=texProps.VisibleNodeMask=1;
            std::atomic<int> phase{0};
            std::thread stuck([&]{acz::ProxyCreationScope scope;phase=1;while(phase.load()!=2)std::this_thread::yield();});
            while(phase.load()!=1)std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(acz::kXrWindowCapMs+150));
            CHECK(acz::xrCallWindowOpen() && !acz::xrCallWindowActive());
            {
                HeldUnknownWrite game(dev,resource,&sd);
                nextObject=&images[0];out=nullptr;
                CHECK(compat->CreateSharedResource(&texProps,D3D12_HEAP_FLAG_SHARED,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,&f11,
                    D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
                game.release();
            }
            CHECK(!observer.enabled() && std::strcmp(observer.disable_reason(),"overlap:descriptor_write/committed_create:cross")==0);
            CHECK(acz::xrWindowTolerated()==0);
            phase=2;stuck.join();
            std::printf("PASS %u checks: expired XR window keeps the compatibility overlap fail-closed\n",checks.load());return 0;
        } else if(mode=="reentry") {
            // Expected-red on the old unconditional same-thread disable gate.
            acz::capture::State point;point.tracking_complete=true;point.heaps={id(heap)};
            point.set_value(0,acz::metadata::ParameterKind::table,h.gpu);
            auto resolve=[&](unsigned slot) {std::lock_guard lock(acz::state().mutex);return acz::state().nativeBindings.table(point,0,id(heap),slot,id(dev));};
            for(unsigned iteration=0;iteration<200;++iteration) {
                nestedAction=[&](auto* device,auto dest) {device->CreateShaderResourceView(resource,&sd,{dest.ptr+32});};
                auto before=writes;dev->CreateShaderResourceView(resource,&sd,{h.cpu});
                CHECK(writes==before+2 && acz::state().nativeBindings.enabled());
                auto outer=resolve(0),inner=resolve(1);CHECK(outer&&inner);
                CHECK(acz::state().nativeBindings.current(outer,point,id(dev)) && acz::state().nativeBindings.current(inner,point,id(dev)));
                CHECK(h.refs==1 && r.refs==1 && src.refs==1);
            }
            // Nested copy uses a distinct destination and a separately declared
            // source read footprint; both publications must survive.
            dev->CreateShaderResourceView(resource,&sd,{src.cpu});
            nestedAction=[&](auto* device,auto dest) {device->CopyDescriptorsSimple(1,{dest.ptr+64},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);};
            auto copied=copies;dev->CreateShaderResourceView(resource,&sd,{h.cpu});
            CHECK(copies==copied+1 && acz::state().nativeBindings.enabled());CHECK(resolve(0)&&resolve(2));
            // Non-final nested Release preserves the two distinct lifetimes.
            ++r.refs;++r2.refs;
            releaseAction=[&]{CHECK(otherResource->Release()==1);};
            CHECK(resource->Release()==1 && r.refs==1 && r2.refs==1);
            CHECK(acz::state().nativeBindings.enabled() && resolve(0) && resolve(2));
            CHECK(!point.pipeline.requirements_proven);
            acz::capture::Inputs inputs;CHECK(!acz::capture::freeze(point,{},inputs));
            // Every canonical pair and relation is exercised through the real
            // guard. Unknown footprints must still fail closed. These synthetic
            // event kinds test diagnostics, not real D3D12 scheduling.
            using K=acz::binding::ObservationKind;
            auto& observer=acz::state().nativeBindings;
            auto resetObserver=[&]{observer.~NativeBindingObserver();new (&observer) acz::binding::NativeBindingObserver;};
            unsigned distinct=0;
            for(unsigned a=0;a<unsigned(K::count);++a)for(unsigned b=a;b<unsigned(K::count);++b)for(bool same:{false,true}) {
                resetObserver();
                {
                    acz::BindingEvent outer(static_cast<K>(a));
                    if(same) {acz::BindingEvent inner(static_cast<K>(b));}
                    else {std::thread worker([&]{acz::BindingEvent inner(static_cast<K>(b));});worker.join();}
                }
                CHECK(!observer.enabled());
                CHECK(std::strcmp(observer.disable_reason(),acz::binding::overlap_reason(static_cast<K>(a),static_cast<K>(b),same))==0);
                CHECK(acz::binding::overlap_reason(static_cast<K>(a),static_cast<K>(b),same)==acz::binding::overlap_reason(static_cast<K>(b),static_cast<K>(a),same));
                ++distinct;
            }
            CHECK(distinct==acz::binding::overlap_reason_count);
            resetObserver();
            // Discriminating J-055 fixture: the ONLY production `device`
            // BindingEvent is ID3D12Device::Release (slot 2). Swap-chain and
            // queue creation have no hooked entry here. Model the actual
            // parent, not an unobserved DXGI call. Release may execute arbitrary
            // nested work; its object key does not describe that work's effects.
            // A non-final device Release publishes no retirement. Overlay-like
            // nested creations are ordinary events, not runtime-internal data.
            observer.heap({id(heap),h.cpu,h.gpu,8,32,0,1,0},id(dev));
            ++a.refs;
            auto beforeDeviceRelease=resourceCalls[0];
            releaseAction=[&]{
                nextObject=&r2;out=nullptr;
                CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
                CHECK(out==&r2 && r2.refs==1 && lastDevice==dev);
                CHECK(lastProps==&props && lastDesc==&rd && lastOutput==&out);
                CHECK(lastIID==__uuidof(ID3D12Resource));
            };
            CHECK(dev->Release()==1 && a.refs==1);
            CHECK(resourceCalls[0]==beforeDeviceRelease+1 && observer.enabled());
            CHECK(observer.resource_token(id(otherResource)).id);
            dev->CreateShaderResourceView(otherResource,&sd,{h.cpu});
            CHECK(resolve(0) && observer.current(resolve(0),point,id(dev)));
            // The same native nesting under FINAL Release still fails closed
            // after the returned-zero retirement is known. No dead COM access.
            releaseAction=[&]{
                nextObject=&r2;
                CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            };
            CHECK(dev->Release()==0 && a.refs==0 && !observer.enabled());
            CHECK(std::strcmp(observer.disable_reason(),"binding device final release")==0);
            CHECK(!observer.table(point,0,id(heap),0,id(dev)));
            CHECK(!observer.current({},point,id(dev)));
            CHECK(!acz::capture::freeze(point,{},inputs));
            a.refs=1; // New fake lifetime for the remaining isolated scenarios.
            resetObserver();
            // A known destination is insufficient for malformed copy nesting:
            // its registry policy invalidates globally, including the outer slot.
            observer.heap({id(heap),h.cpu,h.gpu,8,32,0,1,0},id(dev));
            nestedAction=[&](auto* device,auto dest) {
                D3D12_CPU_DESCRIPTOR_HANDLE d{dest.ptr+64};
                device->CopyDescriptors(1,&d,nullptr,0,nullptr,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            };
            auto beforeMalformed=copies;dev->CreateShaderResourceView(resource,&sd,{h.cpu});
            CHECK(copies==beforeMalformed+1 && !observer.enabled());
            CHECK(std::strcmp(observer.disable_reason(),"overlap:descriptor_write/copy:same")==0);
            resetObserver();
            // A native creation callback has no output footprint yet. A nested
            // write cannot be proven independent from its future address range.
            observer.heap({id(heap),h.cpu,h.gpu,8,32,0,1,0},id(dev));
            creationAction=[&]{dev->CreateShaderResourceView(resource,&sd,{h.cpu});};
            nextObject=&r2;out=nullptr;
            CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            CHECK(out==&r2 && r2.refs==1);
            CHECK(!observer.enabled() && std::strcmp(observer.disable_reason(),"overlap:descriptor_write/committed_create:same")==0);
            resetObserver();
            // A fully known disjoint chain still fails at the documented bound.
            // Use distinct destinations in a
            // heap large enough for the ninth guard, independently of setup.
            observer.heap({0xf000,0xf0000,0,16,32,0,0,0},id(dev));
            std::function<void(unsigned)> deep=[&](unsigned depth) {
                D3D12_CPU_DESCRIPTOR_HANDLE d{0xf0000+uint64_t(depth)*32};
                acz::BindingEvent event(0,true,0,dev,1,&d);
                if(depth<acz::BindingEvent::maxNesting)deep(depth+1);
            };
            deep(0);CHECK(!observer.enabled());
            CHECK(std::strcmp(observer.disable_reason(),"overlap:descriptor_write/descriptor_write:same")==0);
            // Restore the registered fixture state, without resetting production
            // history. All guards have retired before this test-only reset.
            resetObserver();
            observer.heap({id(heap),h.cpu,h.gpu,8,32,0,1,0},id(dev));
            observer.resource({id(resource),r.gpu,8192},id(dev));
            reenterWrite=true;auto n=writes;dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(writes==n+2);
            CHECK(std::strcmp(observer.disable_reason(),"overlap:descriptor_write/descriptor_write:same")==0);
        } else if(mode=="overlap") {
            acz::capture::State point;point.tracking_complete=true;point.heaps={id(heap)};
            point.set_value(0,acz::metadata::ParameterKind::table,h.gpu);
            auto resolve=[&](unsigned slot) {
                std::lock_guard lock(acz::state().mutex);
                return acz::state().nativeBindings.table(point,0,id(heap),slot,id(dev));
            };
            // Retain the original expected-red disjoint native-write regression.
            waitWrite=true;
            std::thread worker([&]{dev->CreateShaderResourceView(resource,&sd,{h.cpu});});
            {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
            dev->CreateConstantBufferView(nullptr,{h.cpu+32});
            {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();worker.join();waitWrite=false;
            CHECK(acz::state().nativeBindings.enabled());CHECK(resolve(0));
            const std::string focused=argc>2?argv[2]:"";
            if(focused.empty() || focused=="textures") {
                // Unsupported texture creation still has a provably distinct
                // output address. Keep it in flight across an independent write
                // and creation without publishing the texture as a buffer.
                for(unsigned iteration=0;iteration<200;++iteration) for(bool reservedCall:{false,true}) {
                    Fake texture{rt.data()},buffer{rt.data(),1,0,0x40000};
                    auto textureDesc=rd;textureDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    writeEntered=writeContinue=false;waitCreate=true;nextObject=&texture;
                    void* returned=nullptr;HRESULT hr=E_FAIL;
                    std::thread creator([&]{
                        hr=reservedCall?dev->CreateReservedResource(&textureDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                            __uuidof(ID3D12Resource),&returned):
                            dev->CreatePlacedResource(reinterpret_cast<ID3D12Heap*>(0x1234),17,&textureDesc,
                                D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned);
                    });
                    {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
                    CHECK(lastDevice==dev && lastDesc==&textureDesc && lastOutput==&returned &&
                        lastIID==__uuidof(ID3D12Resource) && lastInitial==D3D12_RESOURCE_STATE_COMMON && !lastClear);
                    if(!reservedCall) CHECK(lastHeap==reinterpret_cast<ID3D12Heap*>(0x1234) && lastOffset==17);
                    dev->CreateShaderResourceView(resource,&sd,{h.cpu});
                    // A second unknown creation is resolved before the first
                    // native call returns, retaining its known history.
                    waitCreate=false;nextObject=&buffer;void* created=nullptr;
                    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,
                        nullptr,__uuidof(ID3D12Resource),&created)==S_FALSE);
                    CHECK(created==&buffer && buffer.refs==1);
                    {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();creator.join();
                    CHECK(hr==S_FALSE && returned==&texture && texture.refs==1);
                    CHECK(acz::state().nativeBindings.enabled());auto current=resolve(0);CHECK(current);
                    {std::lock_guard lock(acz::state().mutex);
                     CHECK(acz::state().nativeBindings.current(current,point,id(dev)));
                     CHECK(acz::state().nativeBindings.resource_token(id(&buffer)).id);
                     CHECK(!acz::state().nativeBindings.resource_token(id(&texture)).id);}
                    // Reverse completion order: identify the texture while a
                    // known native write remains in flight.
                    Fake secondTexture{rt.data()};
                    writeEntered=writeContinue=false;waitWrite=true;nextObject=&secondTexture;
                    std::thread writer([&]{dev->CreateShaderResourceView(resource,&sd,{h.cpu});});
                    {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
                    void* second=nullptr;
                    auto secondHr=reservedCall?dev->CreateReservedResource(&textureDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                        __uuidof(ID3D12Resource),&second):
                        dev->CreatePlacedResource(reinterpret_cast<ID3D12Heap*>(0x1234),17,&textureDesc,
                            D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&second);
                    CHECK(secondHr==S_FALSE && second==&secondTexture && secondTexture.refs==1);
                    {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();writer.join();waitWrite=false;
                    CHECK(acz::state().nativeBindings.enabled());current=resolve(0);CHECK(current);
                    {std::lock_guard lock(acz::state().mutex);CHECK(acz::state().nativeBindings.current(current,point,id(dev)));}
                    CHECK(reinterpret_cast<ID3D12Resource*>(&buffer)->Release()==0);
                    CHECK(reinterpret_cast<ID3D12Resource*>(&texture)->Release()==0);
                    CHECK(reinterpret_cast<ID3D12Resource*>(&secondTexture)->Release()==0);
                    CHECK(acz::state().nativeBindings.enabled());
                }
            }
            if(focused.empty() || focused=="untracked") {
                // Shared patched vtables also see untracked objects. Their
                // releases must not turn a lone unknown write into ambiguity.
                Fake untracked{rt.data(),2};
                writeEntered=writeContinue=false;waitWrite=true;
                std::thread writer([&]{dev->CreateShaderResourceView(resource,&sd,{h.cpu+1});});
                {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
                CHECK(reinterpret_cast<ID3D12Resource*>(&untracked)->Release()==1);
                CHECK(reinterpret_cast<ID3D12Resource*>(&untracked)->Release()==0);
                {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();writer.join();waitWrite=false;
                CHECK(acz::state().nativeBindings.enabled());
                dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(resolve(0));
            }
            if(!focused.empty()) {std::printf("PASS %u checks: focused %s regression\n",checks.load(),focused.c_str());return 0;}
            // 200 in-process repetitions exercise real hooked fake-COM creates,
            // disjoint writes and releases on four distinct addresses. Scheduling
            // coverage is probabilistic; no runtime or GPU behaviour is inferred.
            for(unsigned iteration=0;iteration<200;++iteration) {
                Rendezvous native,checkpoint;
                std::array<Fake,4> objects,heaps;
                std::array<std::thread,4> threads;
                std::array<acz::binding::NativeResolution,4> snapshots;
                for(unsigned n=0;n<4;++n) {
                    objects[n]={rt.data(),1,0,0x50000+uint64_t(n)*0x10000};
                    heaps[n]={ht.data(),1,0x100000+uint64_t(n)*0x1000,0x200000+uint64_t(n)*0x1000};
                    point.heaps.push_back(id(&heaps[n]));point.set_value(3+n,acz::metadata::ParameterKind::table,heaps[n].gpu);
                    threads[n]=std::thread([&,n] {
                        concurrentGate=&native;concurrentObject=&heaps[n];
                        D3D12_DESCRIPTOR_HEAP_DESC localHeap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,8,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
                        void* returnedHeap=nullptr;
                        CHECK(dev->CreateDescriptorHeap(&localHeap,__uuidof(ID3D12DescriptorHeap),&returnedHeap)==S_FALSE);
                        CHECK(returnedHeap==&heaps[n] && heaps[n].refs==1);
                        concurrentObject=&objects[n];void* returned=nullptr;
                        CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,
                            nullptr,__uuidof(ID3D12Resource),&returned)==S_FALSE);
                        CHECK(returned==&objects[n] && objects[n].refs==1);
                        auto local=sd;
                        dev->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(returned),&local,{n<2?h.cpu+n*32:heaps[n].cpu});
                        checkpoint.meet(5);checkpoint.meet(5);
                        concurrentRelease=true;
                        CHECK(reinterpret_cast<ID3D12Resource*>(returned)->Release()==0);
                        CHECK(reinterpret_cast<ID3D12DescriptorHeap*>(returnedHeap)->Release()==0);
                        concurrentRelease=false;concurrentGate=nullptr;
                    });
                }
                checkpoint.meet(5);
                CHECK(acz::state().nativeBindings.enabled());
                for(unsigned n=0;n<4;++n) {
                    {
                        std::lock_guard lock(acz::state().mutex);
                        CHECK(acz::state().nativeBindings.heap_token(id(&heaps[n])).id);
                        snapshots[n]=acz::state().nativeBindings.table(point,n<2?0:3+n,n<2?id(heap):id(&heaps[n]),n<2?n:0,id(dev));
                    }
                    CHECK(snapshots[n]);
                    CHECK(snapshots[n].snapshot->resource.id==id(&objects[n]));
                    std::lock_guard lock(acz::state().mutex);
                    CHECK(acz::state().nativeBindings.current(snapshots[n],point,id(dev)));
                }
                checkpoint.meet(5);
                for(auto& t:threads)t.join();
                CHECK(acz::state().nativeBindings.enabled());
                for(unsigned n=0;n<4;++n) {
                    CHECK(objects[n].refs==0 && heaps[n].refs==0);
                    std::lock_guard lock(acz::state().mutex);
                    CHECK(!acz::state().nativeBindings.current(snapshots[n],point,id(dev)));
                    CHECK(!acz::state().nativeBindings.heap_token(id(&heaps[n])).id);
                    CHECK(snapshots[n].snapshot->descriptor.view.stride==416);
                }
                point.heaps.resize(1);
            }
            CHECK(concurrentForwards==200*4*5);
            {
                D3D12_CPU_DESCRIPTOR_HANDLE destination{h.cpu};
                acz::BindingEvent longCall(0,true,0,dev,1,&destination);
                std::thread independent([&]{for(unsigned n=0;n<300;++n)dev->CreateShaderResourceView(resource,&sd,{h.cpu+6*32});});
                independent.join();CHECK(acz::state().nativeBindings.enabled());
            }
            CHECK(resolve(6)); // completed known history is reclaimed, not exhausted

            auto runConflict=[&](unsigned scenario) {
                for(unsigned n=0;n<8;++n)dev->CreateShaderResourceView(resource,&sd,{h.cpu+n*32});
                auto neighbor=resolve(7);CHECK(neighbor);
                writeEntered=writeContinue=false;waitWrite=true;
                // Source heap writes exercise copy-read versus in-flight write.
                auto target=scenario==2?src.cpu:h.cpu;
                std::thread writer([&]{dev->CreateShaderResourceView(resource,&sd,{target});});
                {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
                if(scenario==0)dev->CreateConstantBufferView(nullptr,{h.cpu});
                if(scenario==1)dev->CopyDescriptorsSimple(2,{h.cpu},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if(scenario==2)dev->CopyDescriptorsSimple(1,{h.cpu+32},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();writer.join();waitWrite=false;
                CHECK(acz::state().nativeBindings.enabled());
                CHECK(!resolve(scenario==2?1:0));
                std::lock_guard lock(acz::state().mutex);
                CHECK(acz::state().nativeBindings.current(neighbor,point,id(dev)));
            };
            runConflict(0);runConflict(1);runConflict(2);
            // Native creation is still blocked with an UNKNOWN output while
            // final Release retires the same address. When native creation then
            // returns a new fake lifetime there, the retained retirement footprint
            // must taint its publication without disabling unrelated metadata.
            writeEntered=writeContinue=false;waitCreate=true;nextObject=&r;
            void* returned=nullptr;HRESULT created=E_FAIL;
            std::thread creator([&]{created=dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,
                D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned);});
            {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
            CHECK(resource->Release()==0);
            {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();creator.join();waitCreate=false;
            CHECK(created==S_FALSE && returned==&r && r.refs==1);
            {std::lock_guard lock(acz::state().mutex);CHECK(!acz::state().nativeBindings.resource_token(id(resource)).id);}
            CHECK(acz::state().nativeBindings.enabled());
            // An unprovable native destination overlaps a known event. Since its
            // footprint never becomes known, global invalidation is required.
            writeEntered=writeContinue=false;waitWrite=true;
            std::thread unknown([&]{dev->CreateShaderResourceView(resource,&sd,{h.cpu+1});});
            {std::unique_lock lock(waitMutex);CHECK(waitCondition.wait_for(lock,std::chrono::seconds(5),[]{return writeEntered;}));}
            dev->CreateConstantBufferView(nullptr,{h.cpu+32});
            {std::lock_guard lock(waitMutex);writeContinue=true;}waitCondition.notify_all();unknown.join();waitWrite=false;
            CHECK(!acz::state().nativeBindings.enabled());
            CHECK(std::strcmp(acz::state().nativeBindings.disable_reason(),"overlap:descriptor_write/descriptor_write:cross")==0);
            std::printf("PASS %u checks: 200 concurrent fake-COM cohorts and true ambiguity; probabilistic scheduling only\n",checks.load());return 0;
        } else if(mode=="placed") {
            CHECK(dev->CreatePlacedResource(reinterpret_cast<ID3D12Heap*>(0x1234),17,&rd,D3D12_RESOURCE_STATE_COPY_DEST,&clear,__uuidof(IUnknown),&out)==S_FALSE);
            CHECK(lastOffset==17 && lastClear==&clear && lastIID==__uuidof(IUnknown));
        } else if(mode=="reserved") {
            CHECK(dev->CreateReservedResource(&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,&clear,__uuidof(IUnknown),&out)==S_FALSE);
            CHECK(lastInitial==D3D12_RESOURCE_STATE_COPY_SOURCE && lastClear==&clear && lastIID==__uuidof(IUnknown));
        } else if(mode=="unknown-iid") {
            CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(IUnknown),&out)==S_FALSE);
            CHECK(out==&r2 && lastIID==__uuidof(IUnknown));
        } else if(mode=="device-extension") {
            void* queried=nullptr;CHECK(dev->QueryInterface(__uuidof(ID3D12Device1),&queried)==S_OK);
            CHECK(queried==dev && acz::state().nativeBindings.enabled());--a.refs;
            // Standard newer device IIDs now map through canonical identity;
            // retain this isolated fail-closed mode using an unknown IID too.
            CHECK(dev->QueryInterface(__uuidof(ID3D12DeviceChild),&queried)==S_OK);
            CHECK(queried==dev);--a.refs;
        } else if(mode=="device-release") {
            CHECK(dev->Release()==0);dev=shared; // Forward through the live shared-table peer.
        } else if(mode=="heap-iid") {
            CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(IUnknown),&out)==S_FALSE);
            CHECK(lastIID==__uuidof(IUnknown) && out==&r2);
        } else if(mode=="shared-flags") {
            CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            CHECK(lastFlags==D3D12_HEAP_FLAG_SHARED && out==&r2);
        } else if(mode=="alias" || mode=="extension") {
            aliasQuery=mode=="alias";void* queried=nullptr;
            CHECK(resource->QueryInterface(mode=="alias"?__uuidof(ID3D12Resource):__uuidof(ID3D12Resource1),&queried)==S_OK);
            CHECK(queried==(aliasQuery?static_cast<void*>(&r2):static_cast<void*>(&r)));
            aliasQuery=false;--r.refs; // Balance the deliberately nonconforming fake QI.
        } else if(mode=="exception") {
            if(argc>2 && std::string(argv[2])=="footprint") {
                // More than two ranges exercises the overflow allocation BEFORE
                // the lock/native call. Failure must neither consume a pool slot
                // nor alter any of the forwarded arguments/refcounts.
                D3D12_CPU_DESCRIPTOR_HANDLE dst[]={{h.cpu},{h.cpu+32}},source[]={{src.cpu}};
                UINT ds[]={1,1},ss[]={2};auto before=copies;
                allocation_budget=0;
                dev->CopyDescriptors(2,dst,ds,1,source,ss,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                allocation_budget=-1;
                CHECK(copies==before+1 && lastDests==dst && lastSources==source && lastDestSizes==ds && lastSourceSizes==ss);
                CHECK(lastCount==2 && lastSourceCount==1 && h.refs==1 && src.refs==1 && r.refs==1);
                CHECK(!acz::state().nativeBindings.enabled() && acz::state().bindingEvents==0);
                CHECK(acz::bindingCohort().active==0);
                for(const auto& entry:acz::bindingCohort().records)CHECK(!entry.used && !entry.active);
                std::printf("PASS footprint allocation failure preserves forwarding and retires its event\n");return 0;
            }
            throwAddress=true;
            CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
            CHECK(out==&r2 && r2.refs==1);throwAddress=false;
        } else if(mode=="patch" || mode=="restore") {
            auto dt=deviceTable(false);Fake failed{dt.data()};auto fd=reinterpret_cast<ID3D12Device*>(&failed);
            acz::g_protect=protectFailure;protectFail=mode=="patch"?11:12;
            CHECK(!acz::hookNativeBindingDevice(fd));CHECK(!acz::state().nativeBindings.enabled());
            if(mode=="restore") {
                nextObject=&h;CHECK(fd->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
                CHECK(lastDevice==fd); // Installed slot forwards after failed restoration.
            }
            protectFail=0;CHECK(acz::hookNativeBindingDevice(fd));CHECK(!acz::state().nativeBindings.enabled());
        } else {CHECK(false);}
        CHECK(!acz::state().nativeBindings.enabled());
        auto n=writes;dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(writes==n+1 && lastResource==resource);
        std::printf("PASS %u checks: isolated %s fails closed with native forwarding preserved\n",checks.load(),argv[1]);return 0;
    }

    // A list found after recording cannot resolve metadata until observed reset.
    std::array<void*,60> lt{};lt.fill(reinterpret_cast<void*>(&sentinel));lt[0]=reinterpret_cast<void*>(&query);lt[1]=reinterpret_cast<void*>(&add);lt[2]=reinterpret_cast<void*>(&release);
    lt[8]=reinterpret_cast<void*>(&directListType);lt[7]=reinterpret_cast<void*>(&noListDevice);lt[10]=reinterpret_cast<void*>(&resetList);lt[11]=lt[25]=reinterpret_cast<void*>(&clearList);
    lt[28]=reinterpret_cast<void*>(&setListHeaps);lt[32]=reinterpret_cast<void*>(&tableList);
    lt[30]=reinterpret_cast<void*>(&rootList);lt[36]=reinterpret_cast<void*>(&constantsList);
    lt[38]=lt[40]=lt[42]=reinterpret_cast<void*>(&addressList);
    Fake listObject{lt.data()};auto list=reinterpret_cast<ID3D12GraphicsCommandList*>(&listObject);
    CHECK(acz::hookCaptureCommandList(list));
    auto stateCopy=[&]{std::lock_guard lock(acz::state().mutex);return acz::state().states.at(list).bindings;};
    auto initial=stateCopy();CHECK(!acz::resolveNativeTableBinding(initial,0,id(heap),0));
    CHECK(list->Reset(nullptr,nullptr)==S_OK);
    CHECK(!acz::resolveNativeTableBinding(stateCopy(),0,id(heap),0)); // No observed device identity.
    CHECK(list->Release()==0);listObject.refs=1;nextObject=&listObject;
    const auto resetsBeforeCreation=resetCalls;
    CHECK(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,nullptr,nullptr,__uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE);
    CHECK(stateCopy().list_identity!=initial.list_identity && listObject.refs==1);
    ID3D12DescriptorHeap* hs[]={heap};list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});
    auto point=stateCopy();probeState=point;probeHeap=id(heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_UNKNOWN;sd.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Buffer={2,3,416,D3D12_BUFFER_SRV_FLAG_NONE};
    for(const auto& iid:companions) {
        const auto refs=a.refs;void* queried=nullptr;
        CHECK(dev->QueryInterface(iid,&queried)==S_OK&&queried==dev&&a.refs==refs+1);
        CHECK(dev->Release()==refs&&acz::state().nativeBindings.enabled());
        // Fresh binding events after EACH companion still publish metadata.
        dev->CreateShaderResourceView(resource,&sd,{src.cpu});
        dev->CopyDescriptorsSimple(1,{h.cpu},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto resolved=acz::resolveNativeTableBinding(point,0,id(heap),0);
        CHECK(resolved&&acz::currentNativeBinding(resolved,point));
    }
    dev->CreateShaderResourceView(resource,&sd,{src.cpu});CHECK(lastResource==resource && lastDesc==&sd && lastDest.ptr==src.cpu && nestedPointRejected);
    dev->CopyDescriptorsSimple(1,{h.cpu},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(lastCount==1 && lastDest.ptr==h.cpu && lastSource.ptr==src.cpu && lastType==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto value=acz::resolveNativeTableBinding(point,0,id(heap),0);CHECK(value && acz::currentNativeBinding(value,point));
    CHECK(value.snapshot->descriptor.first_element==2 && value.snapshot->descriptor.elements==3 && value.snapshot->descriptor.view.stride==416);
    CHECK(value.snapshot->descriptor.view.resource==id(resource) && value.snapshot->descriptor.view.offset==0 && value.snapshot->resource_bytes==8192);
    CHECK(value.list_identity==point.list_identity && value.generation==point.generation && value.revision==point.revision);
    CHECK(!point.pipeline.requirements_proven);acz::capture::Inputs inputs;CHECK(!acz::capture::freeze(point,{},inputs));
    sd.Buffer.FirstElement=0;dev->CreateShaderResourceView(otherResource,&sd,{src.cpu});
    CHECK(value.snapshot->descriptor.first_element==2 && acz::currentNativeBinding(value,point));
    CHECK(sourceHeap->Release()==0);CHECK(acz::currentNativeBinding(value,point)); // Copy owns source metadata.
    src.refs=1;nextObject=&src;CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
    dev->CreateShaderResourceView(resource,&sd,{src.cpu});
    D3D12_CPU_DESCRIPTOR_HANDLE dests[]={{h.cpu+32},{h.cpu+64}},sources[]={{src.cpu},{src.cpu+32}};
    D3D12_CONSTANT_BUFFER_VIEW_DESC cd{r.gpu+256,512};dev->CreateConstantBufferView(&cd,{src.cpu+32});
    CHECK(lastDesc==&cd && lastDest.ptr==src.cpu+32);
    dev->CopyDescriptors(2,dests,nullptr,2,sources,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(lastDests==dests && lastSources==sources && !lastDestSizes && !lastSourceSizes && lastCount==2 && lastSourceCount==2);
    auto cb=acz::resolveNativeTableBinding(point,0,id(heap),2);CHECK(cb && cb.snapshot->descriptor.view.offset==256 && cb.snapshot->descriptor.view.size==512);
    UINT counts[]={2};D3D12_CPU_DESCRIPTOR_HANDLE ds[]={{h.cpu+96}},ss[]={{src.cpu}};
    dev->CopyDescriptors(1,ds,counts,1,ss,counts,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(lastDestSizes==counts && lastSourceSizes==counts && lastDests==ds && lastSources==ss);
    CHECK(acz::resolveNativeTableBinding(point,0,id(heap),4));
    CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),8));
    list->SetGraphicsRootConstantBufferView(1,r.gpu+256);list->SetGraphicsRootShaderResourceView(2,r.gpu+416);point=stateCopy();
    auto rootCb=acz::resolveNativeRootBinding(point,1,512,0,0),rootSrv=acz::resolveNativeRootBinding(point,2,0,2,416);
    CHECK(rootCb && rootCb.snapshot->descriptor.view.offset==256);CHECK(rootSrv && rootSrv.snapshot->descriptor.view.offset==416);
    CHECK(!acz::resolveNativeRootBinding(point,2,0,0,0));CHECK(!acz::resolveNativeRootBinding(point,1,257,0,0));
    // Exact exporter candidate: the list, descriptors, and resources above came
    // through fake COM. This fixture supplies an accepted decoded root/PSO pair
    // through the existing private metadata maps, then uses actual list setters.
    std::array<void*,3> metadataTable{reinterpret_cast<void*>(&query),reinterpret_cast<void*>(&add),reinterpret_cast<void*>(&release)};
    Fake rootObject{metadataTable.data()},pipelineObject{metadataTable.data()};
    auto nativeRoot=reinterpret_cast<ID3D12RootSignature*>(&rootObject);
    auto nativePso=reinterpret_cast<ID3D12PipelineState*>(&pipelineObject);
    acz::metadata::RootLayout layout;layout.parameters.resize(4);
    layout.parameters[0].ranges={{acz::metadata::BindingKind::srv,128,10,0,0,0}};
    layout.parameters[1].kind=acz::metadata::ParameterKind::cbv;
    layout.parameters[1].shader_register=1;
    layout.parameters[2].kind=acz::metadata::ParameterKind::srv;
    layout.parameters[2].shader_register=200;
    layout.parameters[3].kind=acz::metadata::ParameterKind::constants;
    layout.parameters[3].shader_register=2;layout.parameters[3].constant_count=59;
    auto decoded=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(decoded.valid());
    constexpr uint64_t rootLifetime=701,psoLifetime=702;
    {
        std::lock_guard lock(acz::state().mutex);
        auto& s=acz::state();s.roots.emplace(nativeRoot,decoded);
        s.rootIdentities.emplace(nativeRoot,rootLifetime);
        acz::PipelineRecord record;record.identity=psoLifetime;record.root_identity=rootLifetime;
        s.pipelines.emplace(nativePso,std::move(record));
    }
    list->SetGraphicsRootSignature(nativeRoot);list->SetPipelineState(nativePso);
    list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});
    list->SetGraphicsRootConstantBufferView(1,r.gpu+256);
    list->SetGraphicsRootShaderResourceView(2,r.gpu+416);
    const uint32_t exportWords[]={0x11223344,0x55667788};
    list->SetGraphicsRoot32BitConstants(3,2,exportWords,1);
    point=stateCopy();
    using namespace acz::native_evidence;
    ExpectedPoint expected{point.list_identity,point.generation,point.revision,id(dev),rootLifetime,psoLifetime};
    Selection tablePick;tablePick.category=Category::TableBuffer;tablePick.root=rootLifetime;tablePick.parameter=0;
    tablePick.count=1;tablePick.resource=value.snapshot->resource;
    tablePick.heap=value.snapshot->heap;tablePick.slot=value.snapshot->slot;
    tablePick.revision=value.snapshot->revision;
    Selection cbPick;cbPick.category=Category::RootCbv;cbPick.root=rootLifetime;cbPick.parameter=1;
    cbPick.resource=rootCb.snapshot->resource;cbPick.address=r.gpu+256;cbPick.bytes=512;
    Selection srvPick; srvPick.category=Category::RootSrv;srvPick.root=rootLifetime;srvPick.parameter=2;
    srvPick.resource=rootSrv.snapshot->resource;srvPick.address=r.gpu+416;
    srvPick.elements=2;srvPick.stride=416;
    Selection wordsPick;wordsPick.category=Category::RootConstants;wordsPick.root=rootLifetime;wordsPick.parameter=3;
    wordsPick.offset=1;wordsPick.count=2;
    Selection requested[]={tablePick,cbPick,srvPick,wordsPick};
    auto observed=export_metadata(expected,requested,4);
    CHECK(observed.outcome==Outcome::MetadataObserved && observed.entry_count==4 && observed.resource_count==1);
    CHECK(observed.unknown_mask==permanent_unknown_mask &&
          observed.coverage==Coverage::CreationOrResetBoundaryObserved);
    // This list was registered through CreateCommandList and has not Reset
    // since that creation. The coverage label names either valid boundary.
    {std::lock_guard lock(acz::state().mutex);
     const auto& live=acz::state().states.at(list);
     CHECK(live.creation_observed && live.registration_path==0 && live.reset_observed);}
    CHECK(resetCalls==resetsBeforeCreation);
    CHECK(observed.entries[0].source_heap==value.snapshot->source_heap &&
          observed.entries[0].source_slot==value.snapshot->source_slot &&
          observed.entries[0].source_revision==value.snapshot->source_revision);
    CHECK(observed.entries[0].descriptor_origin==Provenance::ObservedMetadata &&
          observed.entries[0].destination_origin==Provenance::ObservedMetadata &&
          observed.entries[0].source_origin==Provenance::ObservedMetadata);
    CHECK(observed.entries[1].layout_origin==Provenance::ProviderDeclared &&
          observed.entries[1].descriptor_origin==Provenance::ProviderDeclared &&
          observed.entries[1].containment_origin==Provenance::ObservedMetadata);
    CHECK(observed.entries[3].word_count==2 && observed.entries[3].words[0]==exportWords[0] &&
          observed.entries[3].words[1]==exportWords[1] &&
          observed.entries[3].words_origin==Provenance::ObservedMetadata &&
          observed.entries[3].descriptor_origin==Provenance::Unknown);
    CHECK(!point.pipeline.requirements_proven && !point.pipeline.drawcpt);
    acz::capture::Inputs unproved;CHECK(!acz::capture::freeze(point,{},unproved));
    auto refusal=[&](ExpectedPoint at,const Selection* picks,size_t n,Limits bounds={}) {
        auto r=export_metadata(at,picks,n,bounds);
        CHECK(r.outcome==Outcome::Refused && !r.entry_count && !r.resource_count &&
              r.recording_origin==Provenance::Unknown && r.unknown_mask==permanent_unknown_mask);
        return r.reason;
    };
    auto wrong=expected;++wrong.revision;CHECK(std::strcmp(refusal(wrong,requested,4),"PointMismatch")==0);
    wrong=expected;++wrong.list;CHECK(std::strcmp(refusal(wrong,requested,4),"StaleOrMissingPoint")==0);
    wrong=expected;++wrong.generation;refusal(wrong,requested,4);
    wrong=expected;++wrong.device;refusal(wrong,requested,4);
    wrong=expected;++wrong.pso;CHECK(std::strcmp(refusal(wrong,requested,4),"RootPsoMismatch")==0);
    auto bad=requested[0];++bad.resource.generation;refusal(expected,&bad,1);
    bad=requested[0];++bad.root;CHECK(std::strcmp(refusal(expected,&bad,1),"RootStampMismatch")==0);
    bad=requested[0];++bad.heap.generation;refusal(expected,&bad,1);
    bad=requested[0];++bad.revision;refusal(expected,&bad,1);
    bad=requested[1];++bad.bytes;refusal(expected,&bad,1);
    bad=requested[2];bad.elements=UINT32_MAX;bad.stride=UINT32_MAX;refusal(expected,&bad,1);
    bad=requested[3];bad.offset=0;refusal(expected,&bad,1); // Word zero was not written.
    bad=requested[3];bad.count=65;refusal(expected,&bad,1);
    bad=requested[0];bad.category=Category::IA;
    CHECK(std::strcmp(refusal(expected,&bad,1),"UnsupportedCategory")==0);
    bad.category=Category::Texture;refusal(expected,&bad,1);
    bad.category=Category::Sampler;refusal(expected,&bad,1);
    bad.category=Category::OpaqueWritable;refusal(expected,&bad,1);
    bad.category=static_cast<Category>(255);CHECK(std::strcmp(refusal(expected,&bad,1),"UnsupportedCategory")==0);
    Selection duplicate[]={tablePick,tablePick};
    CHECK(std::strcmp(refusal(expected,duplicate,2),"DuplicateSelection")==0);
    std::array<Selection,129> selectionCeiling{};selectionCeiling.fill(tablePick);
    CHECK(std::strcmp(refusal(expected,selectionCeiling.data(),129),"SelectionBounds")==0);
    CHECK(std::strcmp(refusal(expected,selectionCeiling.data(),128),"DuplicateSelection")==0);
    refusal(expected,requested,4,{0,128,64});refusal(expected,requested,4,{64,3,64});
    refusal(expected,requested,4,{64,128,1});
    allocation_budget=0;auto fault=export_metadata(expected,requested,4);allocation_budget=-1;
    CHECK(fault.outcome==Outcome::Refused && !fault.entry_count && !fault.resource_count &&
          std::strcmp(fault.reason,"AllocationOrObservationFailure")==0);
    CHECK(acz::state().nativeBindings.enabled() && h.refs>=1 && r.refs==1);
    CHECK(observed.entries[0].descriptor.first_element==2); // Owned historical copy.
    // The same live table can reference 64 distinct observed resources. The
    // 65th must discard the entire attempted export, while repeated uses of
    // one resource above contribute only one resource record.
    std::array<Selection,65> resourceSelections{};
    std::array<acz::binding::Token,65> syntheticTokens{};
    {
        std::lock_guard lock(acz::state().mutex);
        auto& native=acz::state().nativeBindings;
        auto extra=sd;extra.Buffer.FirstElement=0;extra.Buffer.NumElements=1;
        for(size_t i=0;i<resourceSelections.size();++i) {
            const uint64_t synthetic=0x400000+i*0x1000;
            native.resource({synthetic,0x500000+i*0x10000,8192},id(dev));
            syntheticTokens[i]=native.resource_token(synthetic);
            CHECK(syntheticTokens[i].id && syntheticTokens[i].generation);
            native.srv(synthetic,&extra,h.cpu+32*(8+i),id(dev));
        }
    }
    for(size_t i=0;i<resourceSelections.size();++i) {
        auto historical=acz::resolveNativeTableBinding(point,0,id(heap),static_cast<uint32_t>(8+i));
        CHECK(historical);
        auto& pick=resourceSelections[i];pick.category=Category::TableBuffer;pick.root=rootLifetime;
        pick.parameter=0;pick.offset=static_cast<uint32_t>(8+i);pick.count=1;
        pick.resource=syntheticTokens[i];pick.heap=historical.snapshot->heap;
        pick.slot=historical.snapshot->slot;pick.revision=historical.snapshot->revision;
    }
    auto atCap=export_metadata(expected,resourceSelections.data(),64);
    CHECK(atCap.outcome==Outcome::MetadataObserved && atCap.entry_count==64 && atCap.resource_count==64);
    CHECK(std::strcmp(refusal(expected,resourceSelections.data(),65),"ResourceBounds")==0);
    auto exactCeilingsAndDeepFault=[&] {
    // Complete the table with repeated uses of one of the 64 resources, then
    // add eight distinct written constant ranges to reach 128 valid picks.
    {
        std::lock_guard lock(acz::state().mutex);
        auto extra=sd;extra.Buffer.FirstElement=0;extra.Buffer.NumElements=1;
        for(size_t slot=72;slot<128;++slot)
            acz::state().nativeBindings.srv(0x400000,&extra,h.cpu+32*slot,id(dev));
    }
    // A separate constants-only root uses the entire 64-DWORD signature
    // budget, then the table root is restored for the 128-selection case.
    Fake constantsRootObject{metadataTable.data()},constantsPsoObject{metadataTable.data()};
    auto constantsRoot=reinterpret_cast<ID3D12RootSignature*>(&constantsRootObject);
    auto constantsPso=reinterpret_cast<ID3D12PipelineState*>(&constantsPsoObject);
    acz::metadata::RootLayout constantsLayout;constantsLayout.parameters.resize(1);
    constantsLayout.parameters[0].kind=acz::metadata::ParameterKind::constants;
    constantsLayout.parameters[0].shader_register=300;
    constantsLayout.parameters[0].constant_count=64;
    auto constantsDecoded=acz::metadata::RootBindingMetadata::from_adapter(constantsLayout);
    CHECK(constantsDecoded.valid());
    constexpr uint64_t constantsRootLifetime=703,constantsPsoLifetime=704;
    {std::lock_guard lock(acz::state().mutex);
     auto& s=acz::state();s.roots.emplace(constantsRoot,constantsDecoded);
     s.rootIdentities.emplace(constantsRoot,constantsRootLifetime);
     acz::PipelineRecord record;record.identity=constantsPsoLifetime;
     record.root_identity=constantsRootLifetime;s.pipelines.emplace(constantsPso,std::move(record));}
    list->SetGraphicsRootSignature(constantsRoot);list->SetPipelineState(constantsPso);
    std::array<uint32_t,64> fullWords{};
    for(uint32_t i=0;i<fullWords.size();++i)fullWords[i]=0x33000000u+i;
    list->SetGraphicsRoot32BitConstants(0,64,fullWords.data(),0);
    point=stateCopy();
    ExpectedPoint constantsExpected{point.list_identity,point.generation,point.revision,id(dev),
                                    constantsRootLifetime,constantsPsoLifetime};
    Selection fullWordsPick;fullWordsPick.category=Category::RootConstants;
    fullWordsPick.root=constantsRootLifetime;fullWordsPick.parameter=0;fullWordsPick.count=64;
    auto fullWordExport=export_metadata(constantsExpected,&fullWordsPick,1);
    CHECK(fullWordExport.outcome==Outcome::MetadataObserved && fullWordExport.entry_count==1 &&
          fullWordExport.entries[0].word_count==64 &&
          fullWordExport.entries[0].words==fullWords);
    list->SetGraphicsRootSignature(nativeRoot);list->SetPipelineState(nativePso);
    list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});
    list->SetGraphicsRootConstantBufferView(1,r.gpu+256);
    list->SetGraphicsRootShaderResourceView(2,r.gpu+416);
    list->SetGraphicsRoot32BitConstants(3,59,fullWords.data(),0);
    {std::lock_guard lock(acz::state().mutex);
     auto& s=acz::state();s.roots.erase(constantsRoot);
     s.rootIdentities.erase(constantsRoot);s.pipelines.erase(constantsPso);}
    point=stateCopy();expected.revision=point.revision;
    std::array<Selection,128> fullSelections{};
    std::copy_n(resourceSelections.begin(),64,fullSelections.begin());
    for(size_t slot=72;slot<128;++slot) {
        auto resolved=acz::resolveNativeTableBinding(point,0,id(heap),static_cast<uint32_t>(slot));
        CHECK(resolved);
        auto pick=resourceSelections[0];pick.offset=static_cast<uint32_t>(slot);
        pick.resource=resolved.snapshot->resource;pick.heap=resolved.snapshot->heap;
        pick.slot=resolved.snapshot->slot;pick.revision=resolved.snapshot->revision;
        fullSelections[slot-8]=pick;
    }
    for(uint32_t i=0;i<8;++i) {
        auto pick=wordsPick;pick.offset=i*7;pick.count=7;
        fullSelections[120+i]=pick;
    }
    auto allSelections=export_metadata(expected,fullSelections.data(),128);
    CHECK(allSelections.outcome==Outcome::MetadataObserved && allSelections.entry_count==128 &&
          allSelections.resource_count==64 && allSelections.unknown_mask==permanent_unknown_mask);
    CHECK(std::strcmp(refusal(expected,fullSelections.data(),128,{64,127,64}),"SelectionBounds")==0);
    // Owned request allocation succeeds, first table entry is copied, then
    // fresh root-CBV resolution allocates and faults. Nothing partial escapes.
    auto beforeToken=acz::state().nativeBindings.resource_token(id(resource));
    const auto beforeFaultPoint=stateCopy();
    allocation_budget=1;auto deepFault=export_metadata(expected,requested,2);allocation_budget=-1;
    CHECK(deepFault.outcome==Outcome::Refused && !deepFault.entry_count && !deepFault.resource_count &&
          std::strcmp(deepFault.reason,"AllocationOrObservationFailure")==0);
    CHECK(acz::state().nativeBindings.enabled() &&
          acz::state().nativeBindings.resource_token(id(resource))==beforeToken);
    CHECK(stateCopy().generation==beforeFaultPoint.generation &&
          stateCopy().revision==beforeFaultPoint.revision);
    CHECK(export_metadata(expected,requested,2).outcome==Outcome::MetadataObserved);
    };
    exactCeilingsAndDeepFault();
    {
        std::lock_guard lock(acz::state().mutex);
        for(const auto token:syntheticTokens)acz::state().nativeBindings.remove_resource(token);
    }
    {
        auto& s=acz::state();s.bindingEvents.store(1);
        CHECK(std::strcmp(refusal(expected,requested,4),"BusyNativeObservation")==0);
        s.bindingEvents.store(0);
        ++s.rootCreates;refusal(expected,requested,4);--s.rootCreates;
        ++s.metadataReleases;refusal(expected,requested,4);--s.metadataReleases;
    }
    // Reenter during the outer request allocation. Both same-thread attempts
    // refuse, then a later ordinary request succeeds.
    auto nestedAttempt=[&] {
        std::thread worker([&] {
        struct Nested {ExpectedPoint point;const Selection* picks;Export result;};
        Nested nested{expected,requested,{}};
        allocation_probe_context=&nested;
        allocation_probe=+[](void* context) {
            auto& call=*static_cast<Nested*>(context);
            call.result=export_metadata(call.point,call.picks,4);
        };
        auto outer=export_metadata(expected,requested,4);
        allocation_probe_context=nullptr;
        CHECK(outer.outcome==Outcome::Refused && nested.result.outcome==Outcome::Refused &&
              std::strcmp(outer.reason,"BusyReentry")==0 &&
              std::strcmp(nested.result.reason,"BusyReentry")==0 &&
              !outer.entry_count && !nested.result.entry_count);
        CHECK(export_metadata(expected,requested,4).outcome==Outcome::MetadataObserved);
        });
        worker.join();
    };
    nestedAttempt();
    // Independent threads enter before either reaches the capture mutex.
    // Their requests serialize and both complete without poisoning each other.
    auto concurrentAttempts=[&] {
        Rendezvous gate;Export results[2];
        auto run=[&](unsigned index) {
            allocation_probe_context=&gate;
            allocation_probe=+[](void* context){static_cast<Rendezvous*>(context)->meet(2);};
            results[index]=export_metadata(expected,requested,4);
            allocation_probe_context=nullptr;
        };
        std::thread a(run,0),b(run,1);a.join();b.join();
        CHECK(results[0].outcome==Outcome::MetadataObserved && results[1].outcome==Outcome::MetadataObserved &&
              results[0].entry_count==4 && results[1].entry_count==4);
        CHECK(export_metadata(expected,requested,4).outcome==Outcome::MetadataObserved);
    };
    concurrentAttempts();
    dev->CreateUnorderedAccessView(resource,otherResource,nullptr,{h.cpu});
    CHECK(std::strcmp(refusal(expected,requested,4),"StaleOrUnsupportedBinding")==0);
    dev->CreateShaderResourceView(nullptr,&sd,{h.cpu});
    refusal(expected,requested,4);
    dev->CreateShaderResourceView(resource,&sd,{h.cpu});
    CHECK(std::strcmp(refusal(expected,&tablePick,1),"DestinationStampMismatch")==0);
    // Release/recreation at the same address changes the lifetime stamp.
    // Previously exported scalars stay immutable, while a fresh request with
    // the old independently retained stamp refuses even after a new write.
    CHECK(resource->Release()==0);CHECK(std::strcmp(refusal(expected,&tablePick,1),"StaleOrUnsupportedBinding")==0);
    r.refs=1;nextObject=&r;
    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,
          nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
    dev->CreateShaderResourceView(resource,&sd,{h.cpu});
    dev->CreateShaderResourceView(resource,&sd,{h.cpu+32});
    CHECK(std::strcmp(refusal(expected,&tablePick,1),"ResourceStampMismatch")==0);
    CHECK(observed.entries[0].descriptor.first_element==2 &&
          observed.entries[0].resource==tablePick.resource &&
          observed.entries[0].revision==tablePick.revision);
    auto cross=point;cross.list_identity++;CHECK(!acz::currentNativeBinding(rootSrv,cross));
    CHECK(!acz::currentNativeBinding(value,point)); // Previous list revision.
    auto forged=point;forged.roots[2].value=r2.gpu;auto real=acz::resolveNativeRootBinding(forged,2,0,2,416);
    CHECK(real && real.snapshot->descriptor.view.resource==id(resource));
    auto beforeReset=point;CHECK(list->Reset(nullptr,nullptr)==S_OK);CHECK(!acz::currentNativeBinding(rootSrv,beforeReset));
    CHECK(resetCalls==resetsBeforeCreation+1);
    list->SetGraphicsRootSignature(nativeRoot);list->SetPipelineState(nativePso);
    list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});point=stateCopy();
    auto resetLive=acz::resolveNativeTableBinding(point,0,id(heap),1);CHECK(resetLive);
    Selection resetPick=tablePick;resetPick.offset=1;resetPick.resource=resetLive.snapshot->resource;
    resetPick.heap=resetLive.snapshot->heap;resetPick.slot=resetLive.snapshot->slot;
    resetPick.revision=resetLive.snapshot->revision;
    ExpectedPoint resetExpected{point.list_identity,point.generation,point.revision,id(dev),rootLifetime,psoLifetime};
    auto resetObserved=export_metadata(resetExpected,&resetPick,1);
    CHECK(resetObserved.outcome==Outcome::MetadataObserved && resetObserved.entry_count==1 &&
          resetObserved.coverage==Coverage::CreationOrResetBoundaryObserved &&
          resetObserved.recording.generation!=observed.recording.generation);
    auto live=acz::resolveNativeTableBinding(point,0,id(heap),1);CHECK(live);
    CHECK(resource->Release()==0);CHECK(!acz::currentNativeBinding(live,point));
    auto oldToken=live.snapshot->resource;r.refs=1;nextObject=&r;
    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
    CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),1));
    CHECK(acz::state().nativeBindings.resource_token(id(resource))!=oldToken);
    dev->CreateShaderResourceView(resource,&sd,{h.cpu+32});live=acz::resolveNativeTableBinding(point,0,id(heap),1);CHECK(live);
    dev->CreateUnorderedAccessView(resource,otherResource,nullptr,{h.cpu+32});
    CHECK(lastResource==resource && lastCounter==otherResource && lastDesc==nullptr && !acz::currentNativeBinding(live,point));
    for(auto badStride:{0u,3u,2052u}) {sd.Buffer.StructureByteStride=badStride;dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));}
    sd.Buffer.StructureByteStride=416;sd.Buffer.FirstElement=std::numeric_limits<UINT64>::max();
    dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));sd.Buffer.FirstElement=0;
    dev->CreateShaderResourceView(resource,&sd,{h.cpu+1});CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));
    dev->CreateShaderResourceView(nullptr,&sd,{h.cpu});CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));
    dev->CreateShaderResourceView(resource,nullptr,{h.cpu});CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));
    Fake undiscovered{rt.data(),1,0,0x30000};dev->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(&undiscovered),&sd,{h.cpu});
    CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0)); // Shared Release table is not creation evidence.
    D3D12_RENDER_TARGET_VIEW_DESC rv{};D3D12_DEPTH_STENCIL_VIEW_DESC dv{};D3D12_SAMPLER_DESC sv{};
    dev->CreateRenderTargetView(resource,&rv,{h.cpu});CHECK(lastDesc==&rv && lastResource==resource);
    dev->CreateDepthStencilView(resource,&dv,{h.cpu});CHECK(lastDesc==&dv && lastResource==resource);
    dev->CreateSampler(&sv,{h.cpu});CHECK(lastDesc==&sv);
    {
        Fake cpuSampler{ht.data(),1,0x6000,0},gpuSampler{ht.data(),1,0x7000,0xb000};
        auto cpuObject=reinterpret_cast<ID3D12DescriptorHeap*>(&cpuSampler);
        auto gpuObject=reinterpret_cast<ID3D12DescriptorHeap*>(&gpuSampler);
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeap{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,8,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        nextObject=&cpuSampler;CHECK(dev->CreateDescriptorHeap(&samplerHeap,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
        CHECK(out==&cpuSampler && cpuSampler.refs==1);
        samplerHeap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;nextObject=&gpuSampler;
        CHECK(dev->CreateDescriptorHeap(&samplerHeap,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
        ID3D12DescriptorHeap* samplerHeaps[]={gpuObject};list->SetDescriptorHeaps(1,samplerHeaps);
        list->SetGraphicsRootDescriptorTable(0,{gpuSampler.gpu});auto samplerPoint=stateCopy();
        D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_BORDER,0.5f,8,D3D12_COMPARISON_FUNC_LESS,
            {0.125f,0.25f,0.5f,1.f},0.f,12.f};
        dev->CreateSampler(&desc,{cpuSampler.cpu});CHECK(lastDesc==&desc && lastDest.ptr==cpuSampler.cpu);
        dev->CopyDescriptorsSimple(1,{gpuSampler.cpu},{cpuSampler.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CHECK(lastType==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && lastCount==1);
        auto observed=acz::resolveNativeTableBinding(samplerPoint,0,id(gpuObject),0);CHECK(observed);
        CHECK(observed.snapshot->descriptor.kind==acz::metadata::BindingKind::sampler);
        CHECK(!observed.snapshot->descriptor.buffer && !observed.snapshot->resource.id);
        CHECK(observed.snapshot->sampler.words[0]==uint32_t(desc.Filter));
        CHECK(observed.snapshot->sampler.words[4]==0x3f000000 && observed.snapshot->sampler.words[12]==0x41400000);
        const std::array<uint32_t,13> expected{uint32_t(desc.Filter),uint32_t(desc.AddressU),uint32_t(desc.AddressV),
            uint32_t(desc.AddressW),0x3f000000,8,uint32_t(desc.ComparisonFunc),0x3e000000,0x3e800000,0x3f000000,0x3f800000,0,0x41400000};
        CHECK(observed.snapshot->sampler.words==expected);

        CHECK(acz::currentNativeBinding(observed,samplerPoint));
        CHECK(!samplerPoint.pipeline.requirements_proven && !acz::capture::freeze(samplerPoint,{},inputs));
        D3D12_CPU_DESCRIPTOR_HANDLE from[]={{cpuSampler.cpu}},to[]={{gpuSampler.cpu+32}};UINT sizes[]={1};
        dev->CopyDescriptors(1,to,sizes,1,from,sizes,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CHECK(lastDests==to && lastSources==from && lastDestSizes==sizes && lastSourceSizes==sizes);
        auto second=acz::resolveNativeTableBinding(samplerPoint,0,id(gpuObject),1);CHECK(second);
        dev->CreateSampler(nullptr,{gpuSampler.cpu});
        CHECK(lastDesc==nullptr && !acz::currentNativeBinding(observed,samplerPoint));
        CHECK(acz::currentNativeBinding(second,samplerPoint));
        CHECK(observed.snapshot->sampler.words[4]==0x3f000000); // immutable
        CHECK(cpuObject->Release()==0 && acz::currentNativeBinding(second,samplerPoint));
        cpuSampler.refs=1;samplerHeap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;nextObject=&cpuSampler;
        CHECK(dev->CreateDescriptorHeap(&samplerHeap,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
        dev->CopyDescriptorsSimple(1,{gpuSampler.cpu},{cpuSampler.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CHECK(!acz::resolveNativeTableBinding(samplerPoint,0,id(gpuObject),0)); // reused source has no slots
        CHECK(acz::currentNativeBinding(second,samplerPoint));
        list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});point=stateCopy();
        CHECK(gpuObject->Release()==0 && cpuObject->Release()==0);
        CHECK(!acz::currentNativeBinding(second,samplerPoint));
        CHECK(acz::state().nativeBindings.enabled());
    }

    dev->CreateShaderResourceView(resource,&sd,{h.cpu});live=acz::resolveNativeTableBinding(point,0,id(heap),0);CHECK(live);
    // Seed a distinct valid destination and neighbor before the rejected copy.
    sd.Buffer.FirstElement=1;dev->CreateShaderResourceView(resource,&sd,{h.cpu+32});
    auto destination=acz::resolveNativeTableBinding(point,0,id(heap),1);CHECK(destination);
    sd.Buffer.FirstElement=2;dev->CreateShaderResourceView(resource,&sd,{h.cpu+64});
    auto neighbor=acz::resolveNativeTableBinding(point,0,id(heap),2);CHECK(neighbor);
    CHECK(acz::currentNativeBinding(destination,point) && acz::currentNativeBinding(neighbor,point));
    dev->CopyDescriptorsSimple(1,{h.cpu+32},{h.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(lastCount==1 && lastDest.ptr==h.cpu+32 && lastSource.ptr==h.cpu && lastType==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(!acz::currentNativeBinding(destination,point)); // Visible source rejected; known destination erased.
    CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),1));
    CHECK(acz::currentNativeBinding(live,point) && acz::currentNativeBinding(neighbor,point));
    auto retainedSource=acz::resolveNativeTableBinding(point,0,id(heap),0);
    auto retainedNeighbor=acz::resolveNativeTableBinding(point,0,id(heap),2);
    CHECK(retainedSource && retainedSource.snapshot==live.snapshot);
    CHECK(retainedNeighbor && retainedNeighbor.snapshot==neighbor.snapshot);
    CHECK(live.snapshot->descriptor.first_element==0 && destination.snapshot->descriptor.first_element==1);
    CHECK(destination.snapshot->descriptor.view.stride==416 && neighbor.snapshot->descriptor.first_element==2);
    CHECK(destination.snapshot->revision!=live.snapshot->revision && neighbor.snapshot->revision!=destination.snapshot->revision);
    // Malformed shape clears provenance across heaps while forwarding the
    // caller's exact arguments. The fake performs no real descriptor operation.
    Fake remote{ht.data(),1,0x4000,0xa000};
    auto remoteHeap=reinterpret_cast<ID3D12DescriptorHeap*>(&remote);
    heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;nextObject=&remote;
    CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
    CHECK(out==&remote && remote.refs==1);
    ID3D12DescriptorHeap* both[]={heap,remoteHeap};
    list->SetDescriptorHeaps(2,both);list->SetGraphicsRootDescriptorTable(0,{h.gpu});
    list->SetGraphicsRootDescriptorTable(3,{remote.gpu});point=stateCopy();
    const auto heapRefs=h.refs,sourceRefs=src.refs,remoteRefs=remote.refs,resourceRefs=r.refs;
    for(unsigned malformed=0;malformed<4;++malformed) {
        sd.Buffer.FirstElement=0;dev->CreateShaderResourceView(resource,&sd,{h.cpu});
        sd.Buffer.FirstElement=1;dev->CreateShaderResourceView(resource,&sd,{h.cpu+32});
        sd.Buffer.FirstElement=2;dev->CreateShaderResourceView(resource,&sd,{remote.cpu});
        dev->CreateShaderResourceView(resource,&sd,{src.cpu});
        auto old=acz::resolveNativeTableBinding(point,0,id(heap),0);
        auto beside=acz::resolveNativeTableBinding(point,0,id(heap),1);
        auto distant=acz::resolveNativeTableBinding(point,3,id(remoteHeap),0);
        CHECK(old && beside && distant);
        UINT destSizes[]={2},sourceSizes[]={1};
        D3D12_CPU_DESCRIPTOR_HANDLE badDest[]={{h.cpu}},badSource[]={{src.cpu}};
        auto forwarded=copies;
        if(malformed==3) {
            dev->CopyDescriptorsSimple(1,{h.cpu},{h.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CHECK(lastCount==1 && lastDest.ptr==h.cpu && lastSource.ptr==h.cpu);
        } else {
            if(malformed==2) {sourceSizes[0]=2;badSource[0].ptr=h.cpu+32;} // interval overlap
            dev->CopyDescriptors(1,badDest,destSizes,malformed==1?0:1,badSource,sourceSizes,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CHECK(lastCount==1 && lastSourceCount==(malformed==1?0u:1u));
            CHECK(lastDests==badDest && lastSources==badSource);
            CHECK(lastDestSizes==destSizes && lastSourceSizes==sourceSizes);
            CHECK(badDest[0].ptr==h.cpu && destSizes[0]==2);
            CHECK(badSource[0].ptr==(malformed==2?h.cpu+32:src.cpu) && sourceSizes[0]==(malformed==2?2u:1u));
        }
        CHECK(copies==forwarded+1 && lastType==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CHECK(acz::state().nativeBindings.enabled());
        CHECK(!acz::currentNativeBinding(old,point) && !acz::currentNativeBinding(beside,point));
        CHECK(!acz::currentNativeBinding(distant,point));
        CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),0));
        CHECK(!acz::resolveNativeTableBinding(point,0,id(heap),1));
        CHECK(!acz::resolveNativeTableBinding(point,3,id(remoteHeap),0));
        CHECK(old.snapshot->descriptor.first_element==0 && beside.snapshot->descriptor.first_element==1);
        CHECK(distant.snapshot->descriptor.first_element==2 && distant.snapshot->descriptor.view.stride==416);
        CHECK(old.snapshot->revision!=beside.snapshot->revision);
        CHECK(!point.pipeline.requirements_proven && !acz::capture::freeze(point,{},inputs));
        CHECK(h.refs==heapRefs && src.refs==sourceRefs && remote.refs==remoteRefs && r.refs==resourceRefs);
    }
    list->SetDescriptorHeaps(1,hs);list->SetGraphicsRootDescriptorTable(0,{h.gpu});
    point=stateCopy();CHECK(remoteHeap->Release()==0);
    sd.Buffer.FirstElement=0;
    dev->CopyDescriptorsSimple(1,{h.cpu},{src.cpu},D3D12_DESCRIPTOR_HEAP_TYPE_RTV);CHECK(lastType==D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CopyDescriptors(65537,nullptr,nullptr,0,nullptr,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);CHECK(lastCount==65537 && !lastDests);
    // Heap removal/reuse leaves owned values readable, but not current.
    dev->CreateShaderResourceView(resource,&sd,{h.cpu});live=acz::resolveNativeTableBinding(point,0,id(heap),0);CHECK(live);
    CHECK(list->Release()==0);CHECK(h.refs==1 && !acz::currentNativeBinding(live,point));
    CHECK(heap->Release()==0);h.refs=1;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;nextObject=&h;
    CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&out)==S_FALSE);
    CHECK(live.snapshot->descriptor.view.stride==416);
    // A returned-zero tracked Release invalidates publication at its address
    // during the native window, even if creation has already ended and replaced
    // the pre-call token. Non-final Release must preserve the new lifetime.
    // Exercise same-thread nesting and a joined cross-thread publication, with
    // native lock probes and exact forwarding checks (repeats are not proof).
    for(unsigned repeat=0;repeat<200;++repeat)
    for(bool isHeap:{false,true})for(bool final:{false,true})for(bool cross:{false,true}) {
        Fake replacement{isHeap?ht.data():rt.data(),final?1u:2u,0x90000,0xa0000};
        auto object=reinterpret_cast<IUnknown*>(&replacement);
        auto token=[&] {
            std::lock_guard lock(acz::state().mutex);
            auto& b=acz::state().nativeBindings;
            return isHeap?b.heap_token(id(object)):b.resource_token(id(object));
        };
        auto create=[&] {
            nextObject=&replacement;void* returned=nullptr;
            if(isHeap) {
                CHECK(dev->CreateDescriptorHeap(&heapDesc,__uuidof(ID3D12DescriptorHeap),&returned)==S_FALSE);
                CHECK(lastDesc==&heapDesc && lastIID==__uuidof(ID3D12DescriptorHeap));
            } else {
                CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,
                    D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned)==S_FALSE);
                CHECK(lastDesc==&rd && lastProps==&props && lastFlags==D3D12_HEAP_FLAG_NONE &&
                    lastInitial==D3D12_RESOURCE_STATE_COMMON && !lastClear && lastIID==__uuidof(ID3D12Resource));
            }
            CHECK(returned==&replacement && lastDevice==dev && lastOutput==&returned);
        };
        create();const auto before=token();CHECK(before.id);
        acz::binding::Token during;
        releaseAction=[&] {
            auto publish=[&]{create();during=token();CHECK(during.id && during!=before);};
            if(cross){std::thread creator(publish);creator.join();}else publish();
        };
        CHECK(object->Release()==(final?0u:1u) && replacement.refs==(final?0u:1u));
        CHECK(acz::state().nativeBindings.enabled());
        if(final)CHECK(!token().id);
        else {CHECK(token()==during);CHECK(object->Release()==0);CHECK(!token().id);}
    }
    // Pure observer fixtures additionally isolate lifetime and interval rejection
    // independently from permanently disabled native state.
    acz::binding::NativeBindingObserver pure;
    pure.heap({1,0x1000,0x8000,8,32,0,1,0});pure.resource({2,0x10000,8192});
    auto oldHeap=pure.heap_token(1),oldResource=pure.resource_token(2);
    acz::capture::State p;p.tracking_complete=true;p.heaps={1};p.set_value(0,acz::metadata::ParameterKind::table,0x8000);
    pure.srv(2,&sd,0x1000);auto pv=pure.table(p,0,1,0);CHECK(pv && pure.current(pv,p));
    pure.remove_resource(oldResource);pure.resource({2,0x10000,8192});CHECK(!pure.current(pv,p));
    pure.remove_resource(oldResource);pure.srv(2,&sd,0x1000);CHECK(pure.table(p,0,1,0));
    pure.remove_heap(oldHeap);pure.heap({1,0x1000,0x8000,8,32,0,1,0});CHECK(!pure.current(pv,p));
    pure.remove_heap(oldHeap);pure.srv(2,&sd,0x1000);CHECK(pure.table(p,0,1,0));
    pure.heap({3,0x1000,0x8000,8,32,0,1,0});CHECK(!pure.table(p,0,1,0));
    pure.remove_heap(pure.heap_token(3));CHECK(pure.table(p,0,1,0));
    pure.resource({4,0x10080,8192});p.set_value(1,acz::metadata::ParameterKind::cbv,0x10100);CHECK(!pure.root(p,1,512,0,0));
    acz::binding::NativeBindingObserver deviceOwned;
    deviceOwned.heap({1,0x1000,0x8000,8,32,0,1,0},11);deviceOwned.resource({2,0x10000,8192},22);
    deviceOwned.srv(2,&sd,0x1000,11);CHECK(!deviceOwned.table(p,0,1,0,11)); // Resource from another device.
    deviceOwned.remove_resource(deviceOwned.resource_token(2));deviceOwned.resource({2,0x10000,8192},11);
    deviceOwned.srv(2,&sd,0x1000,11);auto owned=deviceOwned.table(p,0,1,0,11);CHECK(owned);
    CHECK(!deviceOwned.table(p,0,1,0,22) && !deviceOwned.current(owned,p,22));
    acz::binding::NativeBindingObserver overflowHeap,overflowResource;
    overflowHeap.heap({1,std::numeric_limits<uint64_t>::max()-16,0,2,32,0,0,0});CHECK(!overflowHeap.enabled());
    overflowResource.resource({2,std::numeric_limits<uint64_t>::max()-8,16});CHECK(!overflowResource.enabled());
    CHECK(std::strcmp(overflowHeap.disable_reason(),"heap registration rejected")==0);
    CHECK(std::strcmp(overflowResource.disable_reason(),"resource registration rejected")==0);
    // Missed native creation/QI disables metadata without altering HRESULT/out.
    queryFails=true;nextObject=&r2;
    CHECK(dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&out)==S_FALSE);
    CHECK(out==&r2 && !acz::state().nativeBindings.enabled());queryFails=false;
    auto written=writes;dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(writes==written+1);
    reenterWrite=true;dev->CreateShaderResourceView(resource,&sd,{h.cpu});CHECK(writes==written+3 && !acz::state().nativeBindings.enabled());
    auto fakeHeap=reinterpret_cast<ID3D12Heap*>(0x9988);nextObject=&r2;
    CHECK(dev->CreatePlacedResource(fakeHeap,123,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,&clear,__uuidof(IUnknown),&out)==S_FALSE);
    CHECK(lastHeap==fakeHeap && lastOffset==123 && lastDesc==&rd && lastClear==&clear && lastInitial==D3D12_RESOURCE_STATE_COPY_SOURCE && lastIID==__uuidof(IUnknown));
    CHECK(dev->CreateReservedResource(&rd,D3D12_RESOURCE_STATE_COPY_DEST,&clear,__uuidof(IUnknown),&out)==S_FALSE);
    CHECK(lastDesc==&rd && lastInitial==D3D12_RESOURCE_STATE_COPY_DEST && lastClear==&clear);
    auto dt=deviceTable(false);Fake failed{dt.data()};auto failedDev=reinterpret_cast<ID3D12Device*>(&failed);
    acz::g_protect=protectFailure;protectFail=11;CHECK(!acz::hookNativeBindingDevice(failedDev));
    CHECK(!acz::state().nativeBindings.enabled());protectFail=0;CHECK(acz::hookNativeBindingDevice(failedDev));
    CHECK(!acz::state().nativeBindings.enabled());
    // Competing patches are not overwritten.
    dt[14]=reinterpret_cast<void*>(&heapCreate<1>);CHECK(!acz::hookNativeBindingDevice(failedDev));CHECK(dt[14]==reinterpret_cast<void*>(&heapCreate<1>));
    CHECK(resource->Release()==0 && otherResource->Release()==0 && heap->Release()==0 && sourceHeap->Release()==0);
    std::printf("PASS %u native-binding fake SDK/COM and CPU metadata checks; no runtime parity\n",checks.load());
}
#endif
