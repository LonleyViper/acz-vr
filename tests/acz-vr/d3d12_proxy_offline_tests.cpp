// SYNTHETIC ONLY (J-3b4). The d3d12.dll proxy's D3D12CreateDevice decision
// logic with a fake real create function and fake notification exports. No
// D3D12 runtime, device, loader call or game is involved.
#include "d3d12_proxy_logic.h"
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace acz::d3d12proxy;

namespace {
int fakeDevice=0;          // address only; never dereferenced
int fakeAdapter=0;
struct RealCall {unsigned calls=0;IUnknown* adapter=nullptr;D3D_FEATURE_LEVEL level{};const IID* iid=nullptr;void** out=nullptr;};
RealCall real;
HRESULT realHr=S_OK;bool realPublishes=true;
HRESULT WINAPI fakeReal(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out) {
    ++real.calls;real.adapter=adapter;real.level=level;real.iid=&iid;real.out=out;
    if(out&&realPublishes)*out=&fakeDevice;
    return realHr;
}
struct NotifyCall {unsigned calls=0,resolves=0;IUnknown* device=nullptr;const GUID* iid=nullptr;unsigned result=0;bool throws=false;};
NotifyCall notify;
unsigned WINAPI fakeNotify(IUnknown* device,const GUID* iid) {
    ++notify.calls;notify.device=device;notify.iid=iid;
    // The device reference belongs to the caller of D3D12CreateDevice at this
    // point; the wrapper must neither AddRef nor Release (fake has no table).
    if(notify.throws)throw std::runtime_error("notify");
    return notify.result;
}
NotifyFn presentResolve() noexcept {++notify.resolves;return &fakeNotify;}
NotifyFn missingResolve() noexcept {++notify.resolves;return nullptr;}
void reset() {real={};notify={};realHr=S_OK;realPublishes=true;}
IUnknown* adapter(){return reinterpret_cast<IUnknown*>(&fakeAdapter);}

void off_unchanged() {
    // dxgi export present but disabled (returns 0): result and output unchanged.
    reset();notify.result=0;void* out=nullptr;
    const auto hr=create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&out);
    CHECK(hr==S_OK&&out==&fakeDevice&&real.calls==1&&notify.calls==1);
    CHECK(real.adapter==adapter()&&real.level==D3D_FEATURE_LEVEL_12_0&&*real.iid==__uuidof(ID3D12Device)&&real.out==&out);
}
void success_notify() {
    reset();notify.result=1;void* out=nullptr;
    const IID& iid=__uuidof(ID3D12Device5);
    const auto hr=create_device(&fakeReal,&presentResolve,nullptr,D3D_FEATURE_LEVEL_11_0,iid,&out);
    CHECK(hr==S_OK&&out==&fakeDevice&&real.calls==1&&real.adapter==nullptr&&real.iid==&iid);
    CHECK(notify.calls==1&&notify.resolves==1&&notify.device==reinterpret_cast<IUnknown*>(&fakeDevice));
    CHECK(notify.iid==&iid&&*notify.iid==__uuidof(ID3D12Device5)); // caller's IID, untouched
}
void failure_passthrough() {
    for(HRESULT failure:{E_INVALIDARG,DXGI_ERROR_UNSUPPORTED,E_OUTOFMEMORY}) {
        // Even a failure that (incorrectly) published an output is never notified.
        for(bool publishes:{false,true}) {
            reset();realHr=failure;realPublishes=publishes;void* out=nullptr;
            const auto hr=create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_1,__uuidof(ID3D12Device),&out);
            CHECK(hr==failure&&real.calls==1&&notify.calls==0&&notify.resolves==0);
            CHECK(out==(publishes?static_cast<void*>(&fakeDevice):nullptr));
        }
    }
    // Support query: null output, S_FALSE, nothing published, nothing notified.
    reset();realHr=S_FALSE;
    CHECK(create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),nullptr)==S_FALSE);
    CHECK(real.calls==1&&real.out==nullptr&&notify.calls==0&&notify.resolves==0);
}
void missing_export() {
    reset();void* out=nullptr;
    CHECK(create_device(&fakeReal,&missingResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&out)==S_OK);
    CHECK(out==&fakeDevice&&real.calls==1&&notify.resolves==1&&notify.calls==0);
    reset();out=nullptr;
    CHECK(create_device(&fakeReal,nullptr,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&out)==S_OK);
    CHECK(out==&fakeDevice&&real.calls==1&&notify.calls==0);
    // Real function unavailable: no call, no notify, a loader-style failure.
    reset();out=nullptr;
    CHECK(create_device(nullptr,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&out)==
        HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    CHECK(out==nullptr&&real.calls==0&&notify.calls==0&&notify.resolves==0);
}
void notify_throws() {
    reset();notify.throws=true;void* out=nullptr;
    CHECK(create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&out)==S_OK);
    CHECK(out==&fakeDevice&&real.calls==1&&notify.calls==1);
}
void double_creation() {
    // Each call forwards exactly once and notifies once; idempotence lives in the dxgi export.
    reset();notify.result=1;void* a=nullptr;void* b=nullptr;
    CHECK(create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&a)==S_OK);
    notify.result=3;
    CHECK(create_device(&fakeReal,&presentResolve,adapter(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),&b)==S_OK);
    CHECK(a==&fakeDevice&&b==&fakeDevice&&real.calls==2&&notify.calls==2&&notify.resolves==2);
}
}
int main() {
    off_unchanged();success_notify();failure_passthrough();missing_export();notify_throws();double_creation();
    std::printf("d3d12_proxy wrapper: synthetic fakes only; no D3D12 runtime evidence\n");
    return 0;
}
