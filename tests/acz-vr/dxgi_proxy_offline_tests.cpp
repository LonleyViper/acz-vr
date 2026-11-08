// Standalone source including the production proxy; no system DXGI loaded.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
struct Factory {void** table;ULONG refs=1;bool factory6=true;};
static Factory* active=nullptr;
static GUID requested{};
static const GUID* iidAddressSeen=nullptr;
static void** factoryOutSeen=nullptr;
static UINT flagsSeen=0;
static int exportSeen=-1,queries=0;
static HRESULT createResult=S_OK;
static int throwQueryAt=0;
static bool publishBeforeQueryThrow=false,queryFailureOutput=false;
static bool throwExport=false,publishBeforeExportThrow=false,exportBadAlloc=false;
static bool nativeFailureOutput=false,nativeNullOutput=false,probeCleanup=false;
static int nativeCalls=0,loadCalls=0,initializeCalls=0;
static bool failLoad=false,reenterLoad=false,failInitialize=false;
static const GUID unknownIID={0xabcde,1,2,{3,4,5,6,7,8,9,10}};
HRESULT STDMETHODCALLTYPE query(IUnknown* obj,REFIID iid,void** out) {
    if(!out) return E_POINTER;
    *out=nullptr;++queries;
    auto* factory=reinterpret_cast<Factory*>(obj);
    if(queries==throwQueryAt) {
        if(publishBeforeQueryThrow) {*out=obj;++factory->refs;}
        throw std::runtime_error("fake QI");
    }
    if(queryFailureOutput && iid==__uuidof(IDXGIFactory6)) {
        *out=obj;++factory->refs;return E_NOINTERFACE;
    }
    if((iid==__uuidof(IDXGIFactory6) && !factory->factory6) ||
        (!acz::isWrappedIID(iid) && iid!=unknownIID)) return E_NOINTERFACE;
    *out=obj;++factory->refs;return S_OK;
}
ULONG STDMETHODCALLTYPE add(IUnknown* obj) {return ++reinterpret_cast<Factory*>(obj)->refs;}
ULONG STDMETHODCALLTYPE release(IUnknown* obj) {
    if(probeCleanup) {
        bool unlocked=false;
        std::thread helper([&] {
            auto& mutex=acz::state().mutex;
            unlocked=mutex.try_lock();if(unlocked) mutex.unlock();
        });helper.join();CHECK(unlocked);
    }
    return --reinterpret_cast<Factory*>(obj)->refs;
}
UINT STDMETHODCALLTYPE creationFlags(IDXGIFactory3*) {return flagsSeen;}
HRESULT create(unsigned version,UINT flags,REFIID iid,void** out) {
    exportSeen=int(version);flagsSeen=flags;requested=iid;
    iidAddressSeen=&iid;factoryOutSeen=out;
    ++nativeCalls;
    if(throwExport) {
        if(publishBeforeExportThrow) {*out=active;++active->refs;}
        if(exportBadAlloc) throw std::bad_alloc();
        throw std::runtime_error("fake export");
    }
    if(FAILED(createResult)) {
        if(nativeFailureOutput) {*out=active;++active->refs;}
        return createResult;
    }
    if(nativeNullOutput) return createResult;
    HRESULT hr=query(reinterpret_cast<IUnknown*>(active),iid,out);
    return FAILED(hr)?hr:createResult;
}
HRESULT WINAPI create0(REFIID iid,void** out){return create(0,0,iid,out);}
HRESULT WINAPI create1(REFIID iid,void** out){return create(1,0,iid,out);}
HRESULT WINAPI create2(UINT flags,REFIID iid,void** out){return create(2,flags,iid,out);}
HMODULE fakeLoad() {
    ++loadCalls;
    if(reenterLoad) {
        void* out=reinterpret_cast<void*>(1);
        CHECK(ProxyCreateDXGIFactory(__uuidof(IDXGIFactory),&out)==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        CHECK(!out);
    }
    if(failLoad) throw std::bad_alloc();
    return reinterpret_cast<HMODULE>(1);
}
void fakeInitialize() {
    ++initializeCalls;
    if(failInitialize) throw std::runtime_error("fake optional registration");
}
bool failAllocation() {throw std::bad_alloc();}
bool nullAllocation() {return false;}
static bool throwAfterWrapperQuery=false;
void failWrapperQuery(bool published) {
    if(published==throwAfterWrapperQuery) throw std::runtime_error("fake wrapper QI boundary");
}
// Test-only per-scenario once_flags, not a production retry/reset mechanism.
struct ScopedLoader {
    std::once_flag once;
    std::once_flag* previous=acz::g_loadOnceTest;
    HMODULE module=acz::g_realDXGI;
    ScopedLoader() {acz::g_loadOnceTest=&once;acz::g_realDXGI=nullptr;}
    ~ScopedLoader() {acz::g_loadOnceTest=previous;acz::g_realDXGI=module;}
};
HRESULT callExport(unsigned version,REFIID iid,void** out) {
    const int before=nativeCalls;
    HRESULT hr=version==0?ProxyCreateDXGIFactory(iid,out):
        version==1?ProxyCreateDXGIFactory1(iid,out):ProxyCreateDXGIFactory2(0x89,iid,out);
    if(nativeCalls!=before) {
        CHECK(exportSeen==int(version) && flagsSeen==(version==2?0x89:0));
        CHECK(iidAddressSeen==&iid && factoryOutSeen==out && requested==iid);
    }
    return hr;
}
void exceptionTests(Factory& real) {
    // No system loader, device, GPU, or XR entry point exists in these seams.
    {
        ScopedLoader loader;
        const int before=loadCalls, calls=nativeCalls;
        failLoad=true;
        for(unsigned version=0;version<3;++version) {
            void* out=reinterpret_cast<void*>(1);
            CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
            CHECK(!out && real.refs==1);
        }
        failLoad=false; // Failure remains terminal after the cause is removed.
        void* out=nullptr;
        CHECK(callExport(0,__uuidof(IDXGIFactory2),&out)==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        CHECK(loadCalls==before+1 && nativeCalls==calls);
    }
    ScopedLoader loader;
    const int before=loadCalls;
    reenterLoad=true;
    probeCleanup=true;
    for(unsigned version=0;version<3;++version) {
        void* out=nullptr;
        failInitialize=true;
        CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==S_OK && out!=&real);
        CHECK(exportSeen==int(version) && requested==__uuidof(IDXGIFactory2));
        CHECK(flagsSeen==(version==2?0x89:0));
        CHECK(static_cast<IUnknown*>(out)->Release()==0 && real.refs==1);
        failInitialize=false;
        reenterLoad=false;
        // Every optional failure preserves a nontrivial success HRESULT/output.
        createResult=S_FALSE;
        for(int failure=0;failure<9;++failure) {
            throwQueryAt=0;publishBeforeQueryThrow=false;queryFailureOutput=false;
            acz::g_allocateFactoryTest=nullptr;acz::g_wrapperQueryTest=nullptr;
            if(failure<4) {
                throwQueryAt=queries+(failure<2?2:3); // Native QI, Factory6 QI, supported-IID QI.
                publishBeforeQueryThrow=(failure%2)!=0;
            } else if(failure==4) acz::g_allocateFactoryTest=failAllocation;
            else if(failure==5) acz::g_allocateFactoryTest=nullAllocation;
            else if(failure<8) {
                throwAfterWrapperQuery=failure==7;acz::g_wrapperQueryTest=failWrapperQuery;
            } else queryFailureOutput=true;
            out=reinterpret_cast<void*>(1);
            CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==S_FALSE);
            CHECK(out==&real && real.refs==2);
            CHECK(static_cast<IUnknown*>(out)->Release()==1 && real.refs==1);
        }
        throwQueryAt=0;queryFailureOutput=false;publishBeforeQueryThrow=false;
        acz::g_allocateFactoryTest=nullptr;acz::g_wrapperQueryTest=nullptr;
        // A throwing foreign export has no native HRESULT. Valid published
        // caller-owned outputs must be released by the boundary, then cleared.
        for(int failure=0;failure<4;++failure) {
            throwExport=true;publishBeforeExportThrow=(failure%2)!=0;exportBadAlloc=failure>=2;
            out=reinterpret_cast<void*>(1);
            CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==(exportBadAlloc?E_OUTOFMEMORY:E_FAIL));
            CHECK(!out && real.refs==1);
        }
        throwExport=false;publishBeforeExportThrow=false;exportBadAlloc=false;
        // Preserve returned native failure and output verbatim, even when the
        // synthetic native failure transfers a reference to its caller.
        createResult=DXGI_ERROR_UNSUPPORTED;nativeFailureOutput=true;out=nullptr;
        CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==DXGI_ERROR_UNSUPPORTED);
        CHECK(out==&real && real.refs==2);CHECK(static_cast<IUnknown*>(out)->Release()==1);
        nativeFailureOutput=false;nativeNullOutput=true;createResult=S_FALSE;out=nullptr;
        CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==S_FALSE && !out && real.refs==1);
        nativeNullOutput=false;createResult=S_OK;
        // Success on a later call proves optional failures did not poison native dispatch.
        CHECK(callExport(version,__uuidof(IDXGIFactory2),&out)==S_OK && out!=&real);
        auto* wrapper=static_cast<IUnknown*>(out);
        void* supported=reinterpret_cast<void*>(1);
        throwQueryAt=queries+1;publishBeforeQueryThrow=true;
        CHECK(wrapper->QueryInterface(__uuidof(IDXGIFactory2),&supported)==E_FAIL && !supported);
        throwQueryAt=0;publishBeforeQueryThrow=false;
        CHECK(wrapper->Release()==0 && real.refs==1);
    }
    CHECK(loadCalls==before+1 && initializeCalls>=3);
    probeCleanup=false;
}
static IUnknown* deviceSeen=nullptr;
static DXGI_SWAP_CHAIN_DESC* descSeen=nullptr;
static const DXGI_SWAP_CHAIN_DESC1* desc1Seen=nullptr;
static const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenSeen=nullptr;
static HWND hwndSeen=nullptr;
static IDXGIOutput* outputSeen=nullptr;
static void* swapOutSeen=nullptr;
static HRESULT swapResult=E_FAIL; // Failure prevents device capture in this fake.
HRESULT STDMETHODCALLTYPE swap(IDXGIFactory*,IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out){
    deviceSeen=device;descSeen=desc;swapOutSeen=out;return swapResult;
}
HRESULT STDMETHODCALLTYPE hwndSwap(IDXGIFactory2*,IUnknown* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,IDXGISwapChain1** out) {
    deviceSeen=device;hwndSeen=hwnd;desc1Seen=desc;fullscreenSeen=fullscreen;outputSeen=output;swapOutSeen=out;return swapResult;
}
int main() {
    std::array<void*,30> table{};
    table[0]=reinterpret_cast<void*>(&query);table[1]=reinterpret_cast<void*>(&add);table[2]=reinterpret_cast<void*>(&release);
    table[10]=reinterpret_cast<void*>(&swap);table[15]=reinterpret_cast<void*>(&hwndSwap);
    table[25]=reinterpret_cast<void*>(&creationFlags);
    Factory real{table.data()};active=&real;
    acz::g_factoryTest=create0;acz::g_factory1Test=create1;acz::g_factory2Test=create2;
    acz::g_loadTest=fakeLoad;acz::g_initializeTest=fakeInitialize;
    CHECK(ProxyCreateDXGIFactory2(0,__uuidof(IDXGIFactory2),nullptr)==E_POINTER);
    void* result=nullptr;
    CHECK(ProxyCreateDXGIFactory2(0x73,__uuidof(IDXGIFactory2),&result)==S_OK);
    CHECK(exportSeen==2 && flagsSeen==0x73 && requested==__uuidof(IDXGIFactory2));
    CHECK(result!=&real && real.refs==2);
    auto* wrapper=static_cast<IDXGIFactory2*>(result);
    IUnknown* identity=nullptr;IDXGIFactory6* six=nullptr;IDXGIObject* object=nullptr;
    CHECK(wrapper->QueryInterface(IID_PPV_ARGS(&identity))==S_OK);
    CHECK(wrapper->QueryInterface(IID_PPV_ARGS(&six))==S_OK);
    CHECK(wrapper->QueryInterface(IID_PPV_ARGS(&object))==S_OK);
    IUnknown* fromSix=nullptr;CHECK(six->QueryInterface(IID_PPV_ARGS(&fromSix))==S_OK);
    CHECK(identity==fromSix && identity==static_cast<IUnknown*>(object));
    CHECK(six->GetCreationFlags()==0x73);
    void* unknown=reinterpret_cast<void*>(1);
    CHECK(wrapper->QueryInterface(unknownIID,&unknown)==E_NOINTERFACE && !unknown);
    CHECK(wrapper->QueryInterface(__uuidof(IUnknown),nullptr)==E_POINTER);
    // Every supported factory version keeps the same controlling IUnknown.
    const GUID versions[]={__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),__uuidof(IDXGIFactory2),
      __uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),__uuidof(IDXGIFactory6)};
    for(const auto& iid:versions) {
        IUnknown* version=nullptr;CHECK(wrapper->QueryInterface(iid,reinterpret_cast<void**>(&version))==S_OK);
        IUnknown* id=nullptr;CHECK(version->QueryInterface(IID_PPV_ARGS(&id))==S_OK);
        CHECK(id==identity);id->Release();version->Release();
    }
    DXGI_SWAP_CHAIN_DESC desc{};IDXGISwapChain* chain=nullptr;
    auto* device=reinterpret_cast<IUnknown*>(0x1234);
    CHECK(wrapper->CreateSwapChain(device,&desc,&chain)==E_FAIL);
    CHECK(deviceSeen==device && descSeen==&desc && swapOutSeen==&chain);
    DXGI_SWAP_CHAIN_DESC1 desc1{};DXGI_SWAP_CHAIN_FULLSCREEN_DESC full{};
    IDXGISwapChain1* chain1=nullptr;auto hwnd=reinterpret_cast<HWND>(0x2345);
    auto* output=reinterpret_cast<IDXGIOutput*>(0x3456);
    CHECK(wrapper->CreateSwapChainForHwnd(device,hwnd,&desc1,&full,output,&chain1)==E_FAIL);
    CHECK(deviceSeen==device && hwndSeen==hwnd && desc1Seen==&desc1 && fullscreenSeen==&full &&
        outputSeen==output && swapOutSeen==&chain1);
    identity->Release();six->Release();object->Release();fromSix->Release();
    CHECK(wrapper->Release()==0 && real.refs==1);
    for(int version=0;version<2;++version) {
        result=nullptr;
        HRESULT hr=version?ProxyCreateDXGIFactory1(__uuidof(IDXGIFactory1),&result):
            ProxyCreateDXGIFactory(__uuidof(IDXGIFactory),&result);
        CHECK(hr==S_OK && exportSeen==version);
        CHECK(requested==(version?__uuidof(IDXGIFactory1):__uuidof(IDXGIFactory)));
        CHECK(static_cast<IUnknown*>(result)->Release()==0 && real.refs==1);
    }
    // A real factory without Factory6 stays correctly typed and unwrapped.
    real.factory6=false;result=nullptr;
    CHECK(ProxyCreateDXGIFactory1(__uuidof(IDXGIFactory1),&result)==S_OK && result==&real);
    CHECK(static_cast<IUnknown*>(result)->Release()==1);
    result=reinterpret_cast<void*>(1);
    CHECK(ProxyCreateDXGIFactory2(9,__uuidof(IDXGIFactory6),&result)==E_NOINTERFACE && !result);
    real.factory6=true;
    // Unimplemented/extension IID is forwarded exactly, without wrong-type wrapping.
    CHECK(ProxyCreateDXGIFactory2(11,unknownIID,&result)==S_OK && result==&real && requested==unknownIID);
    CHECK(static_cast<IUnknown*>(result)->Release()==1);
    acz::g_pinResult=false;result=nullptr;
    CHECK(ProxyCreateDXGIFactory2(11,__uuidof(IDXGIFactory2),&result)==S_OK && result==&real);
    CHECK(static_cast<IUnknown*>(result)->Release()==1);
    acz::g_pinResult=true;
    createResult=E_FAIL;result=reinterpret_cast<void*>(1);
    CHECK(ProxyCreateDXGIFactory2(12,__uuidof(IDXGIFactory2),&result)==E_FAIL && !result && flagsSeen==12);
    createResult=S_FALSE;result=nullptr;
    CHECK(ProxyCreateDXGIFactory2(13,__uuidof(IDXGIFactory2),&result)==S_FALSE && result);
    CHECK(static_cast<IUnknown*>(result)->Release()==0 && real.refs==1);
    acz::g_factory2Test=nullptr;result=reinterpret_cast<void*>(1);
    CHECK(ProxyCreateDXGIFactory2(0,__uuidof(IDXGIFactory2),&result)==HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) && !result);
    acz::g_factory2Test=create2;
    createResult=S_OK;
    exceptionTests(real);
    acz::g_loadTest=nullptr;acz::g_initializeTest=nullptr;
    std::puts("DXGI fake exports/IID/flags/identity/refcount/arguments regressions passed.");
}
