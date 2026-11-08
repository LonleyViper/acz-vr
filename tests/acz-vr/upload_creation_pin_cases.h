#pragma once
// Included by retirement_snapshot_offline_tests, using only logical COM refs.
namespace upload_fixture {
using namespace acz;
using Pins=diagnostics::UploadCreationPins<ID3D12Resource>;
static std::function<void()> addReentry;
static ULONG STDMETHODCALLTYPE addWithReentry(IUnknown* object) {
    if(addReentry){auto callback=std::move(addReentry);addReentry={};callback();}
    return metadata_fixture::t0Add(object);
}
struct Context {
    std::unique_ptr<Pins> saved=std::make_unique<Pins>(state().uploadPins);
    std::unique_ptr<diagnostics::L3Observation> l3=std::make_unique<diagnostics::L3Observation>(state().l3);
    decltype(hook_fixture::table()) table=hook_fixture::table();
    D3D12_RESOURCE_DESC desc{};D3D12_HEAP_PROPERTIES heap{};
    Context() {
        state().uploadPins=*std::make_unique<Pins>();state().l3.enabled=true;
        table[1]=reinterpret_cast<void*>(&addWithReentry);
        table[2]=reinterpret_cast<void*>(&metadata_fixture::t0Release);
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4096;
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;metadata_fixture::t0Throw=false;
    }
    void create(FakeCOM& object) {
        void* out=&object;
        l3Created(S_OK,&out,__uuidof(ID3D12Resource),&desc,1,&heap);
        pinUploadCreation(S_OK,&out,__uuidof(ID3D12Resource),&desc,&heap);
    }
    ~Context() {
        for(size_t i=0;i<Pins::capacity;++i)if(auto* p=state().uploadPins.take(i))p->Release();
        state().uploadPins=*saved;state().l3=*l3;metadata_fixture::t0Throw=false;addReentry={};
    }
};
__declspec(noinline) static void ownedAfterGameRelease() {
    Context c;FakeCOM object{c.table.data()};unsigned reentered=0;
    addReentry=[&] {
        ++reentered;CHECK(!mutex_depth&&object.refs==1);
        const auto& pending=state().uploadPins.entries[0];
        CHECK(pending.reserved&&!pending.finished&&!pending.held);
        // A simultaneous acquisition sees the reservation, never retries COM.
        CHECK(state().uploadPins.reserve(pending.address,pending.creation)==Pins::capacity);
    };
    c.create(object);CHECK(object.refs==2&&reentered==1);
    auto& pins=state().uploadPins;const auto address=reinterpret_cast<uintptr_t>(&object);
    const auto creation=state().l3.resources.get(address)->serial;
    const unsigned char bytes[]={1,2,3,4};
    pins.bind(address,creation,7);pins.map(address,creation,7,3,bytes);
    auto* resource=reinterpret_cast<ID3D12Resource*>(&object);
    CHECK(resource->Release()==1); // game has no remaining reference
    const auto adds=metadata_fixture::t0AddCalls,releases=metadata_fixture::t0ReleaseCalls;
    CHECK(!pins.lookup(address,creation,7,3,bytes));
    CHECK(std::string_view(pins.lookup(address,creation+1,7,3,bytes))=="upload-pin-generation-mismatch");
    CHECK(std::string_view(pins.lookup(address,creation,8,3,bytes))=="upload-pin-generation-mismatch");
    CHECK(std::string_view(pins.lookup(address,creation,7,4,bytes))=="upload-pin-map-mismatch");
    CHECK(std::string_view(pins.lookup(address,creation,7,3,bytes+1))=="upload-pin-map-mismatch");
    CHECK(std::string_view(pins.lookup(123,creation,7,3,bytes))=="upload-pin-not-created");
    CHECK(metadata_fixture::t0AddCalls==adds&&metadata_fixture::t0ReleaseCalls==releases);
    auto* owned=pins.take(0);CHECK(owned==resource);CHECK(owned->Release()==0);
    CHECK(!pins.take(0)); // a second drain owns nothing
}
__declspec(noinline) static void capAndFault() {
    Context c;auto objects=std::make_unique<std::array<FakeCOM,Pins::capacity+1>>();
    for(auto& o:*objects)o.table=c.table.data();
    const auto adds=metadata_fixture::t0AddCalls,releases=metadata_fixture::t0ReleaseCalls;
    c.desc.Width=Pins::per_buffer_cap; // inclusive bound is accepted
    for(size_t i=0;i<Pins::capacity;++i)c.create((*objects)[i]);
    c.create((*objects)[Pins::capacity]);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-table-full");
    CHECK((*objects)[Pins::capacity].refs==1&&metadata_fixture::t0AddCalls==adds+Pins::capacity);
    for(size_t i=0;i<Pins::capacity;++i) {
        auto* owned=state().uploadPins.take(i);CHECK(owned);owned->Release();CHECK(!state().uploadPins.take(i));
    }
    CHECK(metadata_fixture::t0ReleaseCalls==releases+Pins::capacity);
    for(const auto& o:*objects)CHECK(o.refs==1);
    state().uploadPins=*std::make_unique<Pins>();
    metadata_fixture::t0Throw=true;c.create((*objects)[0]);metadata_fixture::t0Throw=false;
    CHECK((*objects)[0].refs==1&&state().uploadPins.entries[0].reserved&&!state().uploadPins.entries[0].held);
    const auto address=reinterpret_cast<uintptr_t>(&(*objects)[0]);
    const auto creation=state().l3.resources.get(address)->serial;
    state().uploadPins.bind(address,creation,9);
    CHECK(std::string_view(state().uploadPins.lookup(address,creation,9,1,&c))=="upload-pin-addref-fault");
    const auto afterFault=metadata_fixture::t0AddCalls;
    void* out=&(*objects)[0];pinUploadCreation(S_OK,&out,__uuidof(ID3D12Resource),&c.desc,&c.heap);
    CHECK(metadata_fixture::t0AddCalls==afterFault); // no retry, including after failure
    for(size_t i=1;i<Pins::capacity;++i)c.create((*objects)[i]);
    c.create((*objects)[Pins::capacity]);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-table-full");
    CHECK((*objects)[Pins::capacity].refs==1);
    for(size_t i=0;i<Pins::capacity;++i)if(auto* owned=state().uploadPins.take(i))owned->Release();
    CHECK(metadata_fixture::t0AddCalls-adds==2*Pins::capacity); // one threw before reference increment
    CHECK(metadata_fixture::t0ReleaseCalls-releases==2*Pins::capacity-1);
    for(const auto& o:*objects)CHECK(o.refs==1);
}
__declspec(noinline) static void refusalsAndOff(TestPaths& paths) {
    Context c;FakeCOM object{c.table.data()};
    const auto adds=metadata_fixture::t0AddCalls,releases=metadata_fixture::t0ReleaseCalls;
    c.heap.Type=D3D12_HEAP_TYPE_DEFAULT;c.create(object);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-not-upload");
    c.heap.Type=D3D12_HEAP_TYPE_UPLOAD;c.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;c.create(object);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-not-buffer");
    c.desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;c.desc.Width=Pins::per_buffer_cap+1;c.create(object);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-oversize");
    c.desc.Width=4096;void* out=&object;
    CHECK(state().uploadPins.reserve(reinterpret_cast<uintptr_t>(&object),UINT32_MAX)==Pins::capacity);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-generation-unavailable");
    pinUploadCreation(S_OK,&out,__uuidof(ID3D12Resource),&c.desc,nullptr);
    CHECK(std::string_view(state().uploadPins.last_reason)=="upload-pin-heap-unavailable");
    pinUploadCreation(E_FAIL,reinterpret_cast<void**>(uintptr_t(1)),__uuidof(ID3D12Resource),&c.desc,&c.heap);
    pinUploadCreation(S_OK,nullptr,__uuidof(ID3D12Resource),&c.desc,&c.heap);
    out=nullptr;pinUploadCreation(S_OK,&out,__uuidof(ID3D12Resource),&c.desc,&c.heap);
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();auto* saved=diagnostics::sink();set_sink(log.get());
    c.create(object);set_sink(saved);
    CHECK(object.refs==1&&metadata_fixture::t0AddCalls==adds&&metadata_fixture::t0ReleaseCalls==releases);
    for(const auto& e:state().uploadPins.entries)CHECK(!e.reserved&&!e.held);
}
// Drive the production creation wrappers, with temporary QI references counted
// separately from permanent AddRef acquisition. Every native callback is unlocked.
static HRESULT wrapperResult=S_OK;
static FakeCOM* wrapperOutput=nullptr;
static bool wrapperThrow=false;
static unsigned wrapperCalls=0;
static void** wrapperSeenOut=nullptr;
static UINT STDMETHODCALLTYPE wrapperNodes(ID3D12Device*) {CHECK(!mutex_depth);return 1;}
static UINT64 STDMETHODCALLTYPE wrapperVA(ID3D12Resource*) {CHECK(!mutex_depth);return 0x900000;}
static HRESULT STDMETHODCALLTYPE wrapperQuery(IUnknown* self,REFIID,void** out) {
    CHECK(!mutex_depth);++reinterpret_cast<FakeCOM*>(self)->refs;*out=self;return S_OK;
}
static HRESULT wrapperReturn(void** out) {
    CHECK(!mutex_depth);++wrapperCalls;wrapperSeenOut=out;
    if(wrapperThrow)throw std::runtime_error("fixture native creation exception");
    if(out)*out=wrapperOutput;return wrapperResult;
}
static HRESULT STDMETHODCALLTYPE wrapperCommitted(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void** out) {return wrapperReturn(out);}
static HRESULT STDMETHODCALLTYPE wrapperCompatibility(ID3D12CompatibilityDevice*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,const D3D11_RESOURCE_FLAGS*,
    D3D12_COMPATIBILITY_SHARED_FLAGS,ID3D12LifetimeTracker*,ID3D12SwapChainAssistant*,REFIID,void** out) {return wrapperReturn(out);}
__declspec(noinline) static void actualWrappers() {
    Context c;auto& s=state();auto protectSaved=g_protect;g_protect=hook_fixture::protect;g_pinResult=true;
    auto dt=hook_fixture::table(),ct=hook_fixture::table();
    dt[7]=reinterpret_cast<void*>(&wrapperNodes);
    FakeCOM device{dt.data()},compatibility{ct.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);
    auto* compat=reinterpret_cast<ID3D12CompatibilityDevice*>(&compatibility);
    const auto deviceHooks=s.devices.find(dt.data());const bool hadDevice=deviceHooks!=s.devices.end();
    auto savedDevice=std::make_unique<Hooks>();if(hadDevice)*savedDevice=deviceHooks->second;
    s.devices[dt.data()].originals[27]=reinterpret_cast<void*>(&wrapperCommitted);
    s.compatibility[ct.data()].originals[3]=reinterpret_cast<void*>(&wrapperCompatibility);
    s.compatibilityOwners[reinterpret_cast<IUnknown*>(compat)]={d,0};
    // Independent observer per attempt: rejected compatibility aliases are sticky.
    for(unsigned seam=0;seam<2;++seam)for(unsigned form=0;form<21;++form) {
        s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
        auto rt=c.table;rt[0]=reinterpret_cast<void*>(&wrapperQuery);rt[11]=reinterpret_cast<void*>(&wrapperVA);
        FakeCOM resource{rt.data()};wrapperOutput=&resource;wrapperResult=S_OK;wrapperThrow=false;
        auto desc=c.desc;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto heap=c.heap;D3D11_RESOURCE_FLAGS f11{};
        D3D12_HEAP_FLAGS flags=D3D12_HEAP_FLAG_NONE;
        auto cf=D3D12_COMPATIBILITY_SHARED_FLAG_NONE;const D3D11_RESOURCE_FLAGS* flags11=nullptr;
        ID3D12LifetimeTracker* lifetime=nullptr;ID3D12SwapChainAssistant* swapchain=nullptr;
        IID iid=__uuidof(ID3D12Resource);
        // 0 positive, 1..10 rejected form, 11..17 result/output/exception cases.
        if(form==1)flags=D3D12_HEAP_FLAG_SHARED;
        if(form==2)desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if(form==3)heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(form==4)desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        if(form==5)desc.Width=Pins::per_buffer_cap+1;
        if(form==6)iid=__uuidof(IUnknown);
        if(form==7){if(seam)cf=static_cast<D3D12_COMPATIBILITY_SHARED_FLAGS>(0x80);else desc.Height=2;}
        if(form==8){if(seam)flags11=&f11;else desc.MipLevels=2;}
        if(form==9){if(seam)lifetime=reinterpret_cast<ID3D12LifetimeTracker*>(uintptr_t(1));else heap.VisibleNodeMask=3;}
        if(form==10){if(seam)swapchain=reinterpret_cast<ID3D12SwapChainAssistant*>(uintptr_t(1));else desc.SampleDesc.Count=2;}
        if(form==11)wrapperResult=E_FAIL;
        if(form==12)wrapperResult=S_FALSE;
        if(form==13)wrapperOutput=nullptr;
        if(form==15)wrapperThrow=true;
        if(form==16)desc.Width=0;
        if(form==17)desc.Format=DXGI_FORMAT_R32_UINT;
        if(form==18)s.uploadPins.retained_bytes=Pins::retained_byte_cap;
        if(form==19)addReentry=[&] {
            CHECK(!mutex_depth);
            const auto address=reinterpret_cast<uintptr_t>(&resource);
            s.nativeBindings.remove_resource(s.nativeBindings.resource_token(address));
        };
        if(form==20)s.nativeBindings.mark_untracked(reinterpret_cast<uintptr_t>(&resource));
        void* returned=nullptr;void** output=form==14?nullptr:&returned;
        const auto adds=metadata_fixture::t0AddCalls,calls=wrapperCalls;
        bool threw=false;HRESULT hr=E_UNEXPECTED;
        try {
            hr=seam?createCompatibilityResource(compat,&heap,flags,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                flags11,cf,lifetime,swapchain,iid,output):
                createBindingResource(d,&heap,flags,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,iid,output);
        }catch(const std::runtime_error&){threw=true;}
        CHECK(wrapperCalls==calls+1&&wrapperSeenOut==output&&threw==(form==15));
        if(!threw){CHECK(hr==wrapperResult);if(output)CHECK(returned==wrapperOutput);}
        if(form==0) {
            CHECK(resource.refs==2&&metadata_fixture::t0AddCalls==adds+1);
            CHECK(s.uploadPins.retained_bytes==desc.Width&&s.uploadPins.entries[0].held);
        } else if(form==19) {
            CHECK(resource.refs==1&&metadata_fixture::t0AddCalls==adds+1);
            CHECK(s.uploadPins.entries[0].reserved&&!s.uploadPins.entries[0].held);
            CHECK(std::string_view(s.uploadPins.last_reason)=="upload-pin-publication-changed");
        } else {
            CHECK(resource.refs==1&&metadata_fixture::t0AddCalls==adds);
            CHECK(s.uploadPins.retained_bytes==(form==18?Pins::retained_byte_cap:0));
            if(form==18)CHECK(std::string_view(s.uploadPins.last_reason)=="upload-pin-byte-budget");
            for(const auto& entry:s.uploadPins.entries)CHECK(!entry.reserved&&!entry.held);
        }
        for(size_t i=0;i<Pins::capacity;++i)if(auto* owned=s.uploadPins.take(i))owned->Release();
        s.uploadPins=*std::make_unique<Pins>();s.objects.erase(rt.data());
    }
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    if(hadDevice)s.devices[dt.data()]=*savedDevice;else s.devices.erase(dt.data());
    s.compatibility.erase(ct.data());s.compatibilityOwners.erase(reinterpret_cast<IUnknown*>(compat));
    g_protect=protectSaved;wrapperOutput=nullptr;wrapperThrow=false;
}
static void aggregateBudget() {
    auto pins=std::make_unique<Pins>();
    CHECK(pins->reserve(1,1,1024)==0&&pins->retained_bytes==1024);
    CHECK(pins->reserve(2,2,2048)==1&&pins->retained_bytes==3072);
    // Preset remaining capacity to exercise subtraction/overflow at the exact
    // aggregate limit without making the fixture allocate any real buffers.
    pins->retained_bytes=Pins::retained_byte_cap-4096;
    CHECK(pins->reserve(3,3,4096)==2&&pins->retained_bytes==Pins::retained_byte_cap);
    CHECK(pins->reserve(4,4,1)==Pins::capacity);
    CHECK(std::string_view(pins->last_reason)=="upload-pin-byte-budget"&&!pins->entries[3].reserved);
}
static void cases(TestPaths& paths) {ownedAfterGameRelease();capAndFault();refusalsAndOff(paths);actualWrappers();aggregateBudget();}
}
