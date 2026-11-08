// Offline only: injected XR and graphics facts, no device/runtime/DLL loading.
#include "openxr_session.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cstring>
using namespace acz;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
namespace {
template<class T> T handle(uintptr_t v) { return reinterpret_cast<T>(v); }
struct Fake {
    int liveInstances=0, liveSessions=0, liveSpaces=0, liveSwapchains=0;
    int creates=0, waitFrames=0, beginFrames=0, endFrames=0, beginSessions=0, endSessions=0;
    int acquires=0, waits=0, releases=0, layers=-1;
    bool owned[2]{}, ready[2]{};
    int event=0; // 1 READY, 2 STOPPING
    int initFail=0; // instance/system/proc/requirements/session/space/views/formats/sc/images
    XrResult beginSessionResult=XR_SUCCESS, endSessionResult=XR_SUCCESS, pollResult=XR_SUCCESS;
    XrResult waitFrameResult=XR_SUCCESS, beginFrameResult=XR_SUCCESS, locateResult=XR_SUCCESS;
    XrResult acquireResult=XR_SUCCESS, waitResult=XR_SUCCESS, releaseResult=XR_SUCCESS, endResult=XR_SUCCESS;
    uint32_t viewCount=2, index=0;
    XrViewStateFlags flags=XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    bool shouldRender=true, supportedFormat=true, badPose=false, vertical=false, badFov=false;
    bool queried=true;
    GraphicsDeviceInfo graphics{{17,23}, D3D_FEATURE_LEVEL_12_0, D3D12_COMMAND_LIST_TYPE_DIRECT, true};
    XrTime submittedTime=0;
    int failWaits=0, failReleases=0;
} *f;
XrResult fail(int point) { return f->initFail==point ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS; }
int eye(XrSwapchain s) { return static_cast<int>(reinterpret_cast<uintptr_t>(s)-10); }
XRAPI_ATTR XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo*, XrInstance* out) {
    if (XR_FAILED(fail(1))) return fail(1);
    *out=handle<XrInstance>(1); ++f->liveInstances; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL getSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* out) {
    *out=1; return fail(2);
}
XRAPI_ATTR XrResult XRAPI_CALL requirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* out) {
    out->adapterLuid={17,23}; out->minFeatureLevel=D3D_FEATURE_LEVEL_12_0; return fail(4);
}
XRAPI_ATTR XrResult XRAPI_CALL proc(XrInstance, const char* name, PFN_xrVoidFunction* out) {
    CHECK(std::strcmp(name,"xrGetD3D12GraphicsRequirementsKHR")==0);
    *out=reinterpret_cast<PFN_xrVoidFunction>(requirements); return fail(3);
}
bool graphics(ID3D12Device*, ID3D12CommandQueue*, D3D_FEATURE_LEVEL minimum, GraphicsDeviceInfo& out) {
    CHECK(minimum==D3D_FEATURE_LEVEL_12_0); out=f->graphics; return f->queried;
}
XRAPI_ATTR XrResult XRAPI_CALL createSession(XrInstance, const XrSessionCreateInfo*, XrSession* out) {
    if (XR_FAILED(fail(5))) return fail(5);
    *out=handle<XrSession>(2); ++f->liveSessions; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL createSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* out) {
    if (XR_FAILED(fail(6))) return fail(6);
    *out=handle<XrSpace>(3); ++f->liveSpaces; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL views(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap,
                                    uint32_t* count, XrViewConfigurationView* out) {
    *count=f->viewCount;
    if (cap) for (uint32_t i=0; i<std::min(cap,*count); ++i) {
        out[i].recommendedImageRectWidth=100+i;
        out[i].recommendedImageRectHeight=80+i;
    }
    return fail(7);
}
XRAPI_ATTR XrResult XRAPI_CALL formats(XrSession, uint32_t cap, uint32_t* count, int64_t* out) {
    *count=1;
    if (cap) out[0]=f->supportedFormat ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    return fail(8);
}
XRAPI_ATTR XrResult XRAPI_CALL createSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    CHECK(info->format==DXGI_FORMAT_R16G16B16A16_FLOAT);
    ++f->creates;
    if (f->initFail==9 && f->creates==2) return XR_ERROR_RUNTIME_FAILURE;
    *out=handle<XrSwapchain>(9+f->creates); ++f->liveSwapchains; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL images(XrSwapchain, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* out) {
    *count=1;
    if (cap) reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)[0].texture=handle<ID3D12Resource*>(5);
    return fail(10);
}
XRAPI_ATTR XrResult XRAPI_CALL destroySwapchain(XrSwapchain s) {
    --f->liveSwapchains; f->owned[eye(s)]=f->ready[eye(s)]=false; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL destroySpace(XrSpace) { --f->liveSpaces; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL destroySession(XrSession) { --f->liveSessions; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL destroyInstance(XrInstance) { --f->liveInstances; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL poll(XrInstance, XrEventDataBuffer* out) {
    if (XR_FAILED(f->pollResult)) return f->pollResult;
    if (!f->event) return XR_EVENT_UNAVAILABLE;
    auto* change=reinterpret_cast<XrEventDataSessionStateChanged*>(out);
    *change={XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    change->session=handle<XrSession>(2);
    change->state=f->event==1 ? XR_SESSION_STATE_READY : XR_SESSION_STATE_STOPPING;
    f->event=0; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL beginSession(XrSession, const XrSessionBeginInfo*) {
    ++f->beginSessions; return f->beginSessionResult;
}
XRAPI_ATTR XrResult XRAPI_CALL endSession(XrSession) { ++f->endSessions; return f->endSessionResult; }
XRAPI_ATTR XrResult XRAPI_CALL waitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    ++f->waitFrames; out->predictedDisplayTime=1234; out->predictedDisplayPeriod=10;
    out->shouldRender=f->shouldRender; return f->waitFrameResult;
}
XRAPI_ATTR XrResult XRAPI_CALL beginFrame(XrSession, const XrFrameBeginInfo*) {
    ++f->beginFrames; return f->beginFrameResult;
}
XRAPI_ATTR XrResult XRAPI_CALL locate(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t, uint32_t* count, XrView* out) {
    CHECK(info->displayTime==1234);
    *count=f->viewCount; state->viewStateFlags=f->flags;
    for (int i=0; i<2; ++i) {
        out[i].pose.position={1.f+(i ? .032f : -.032f),2.f,3.f};
        out[i].pose.orientation={0,0,0,1};
        if (f->vertical) out[i].pose.orientation={.70710677f,0,0,.70710677f};
        if (f->badPose) out[i].pose.position.x=std::numeric_limits<float>::quiet_NaN();
        out[i].fov={-.7f,.8f,.9f,-.6f};
        if (f->badFov) out[i].fov.angleLeft=out[i].fov.angleRight;
    }
    return f->locateResult;
}
XRAPI_ATTR XrResult XRAPI_CALL acquire(XrSwapchain s, const XrSwapchainImageAcquireInfo*, uint32_t* out) {
    ++f->acquires;
    if (XR_FAILED(f->acquireResult)) return f->acquireResult;
    CHECK(!f->owned[eye(s)]);
    f->owned[eye(s)]=true; *out=f->index; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL waitImage(XrSwapchain s, const XrSwapchainImageWaitInfo*) {
    ++f->waits; CHECK(f->owned[eye(s)]);
    if (f->failWaits>0) { --f->failWaits; return f->waitResult; }
    f->ready[eye(s)]=true; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL release(XrSwapchain s, const XrSwapchainImageReleaseInfo*) {
    ++f->releases; CHECK(f->owned[eye(s)] && f->ready[eye(s)]);
    if (f->failReleases>0) { --f->failReleases; return f->releaseResult; }
    f->owned[eye(s)]=f->ready[eye(s)]=false; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL endFrame(XrSession, const XrFrameEndInfo* info) {
    ++f->endFrames; f->layers=info->layerCount; f->submittedTime=info->displayTime;
    if (info->layerCount) {
        CHECK(!f->owned[0] && !f->owned[1]);
        auto* layer=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        CHECK(layer->viewCount==2);
        CHECK(layer->views[0].pose.position.x<layer->views[1].pose.position.x);
        CHECK(layer->views[0].subImage.imageRect.extent.width==100);
        CHECK(layer->views[1].subImage.imageRect.extent.width==101);
    } else CHECK(info->layers==nullptr);
    return f->endResult;
}
XRDispatch dispatch() {
    XRDispatch d;
    d.xrCreateInstance=createInstance; d.xrGetSystem=getSystem; d.xrGetInstanceProcAddr=proc;
    d.xrCreateSession=createSession; d.xrCreateReferenceSpace=createSpace;
    d.xrEnumerateViewConfigurationViews=views; d.xrEnumerateSwapchainFormats=formats;
    d.xrCreateSwapchain=createSwapchain; d.xrEnumerateSwapchainImages=images;
    d.xrDestroySwapchain=destroySwapchain; d.xrDestroySpace=destroySpace;
    d.xrDestroySession=destroySession; d.xrDestroyInstance=destroyInstance;
    d.xrPollEvent=poll; d.xrBeginSession=beginSession; d.xrEndSession=endSession;
    d.xrWaitFrame=waitFrame; d.xrBeginFrame=beginFrame; d.xrLocateViews=locate;
    d.xrAcquireSwapchainImage=acquire; d.xrWaitSwapchainImage=waitImage;
    d.xrReleaseSwapchainImage=release; d.xrEndFrame=endFrame; d.queryGraphics=graphics;
    return d;
}
bool init(OpenXRSession& s) { return s.init(handle<ID3D12Device*>(6),handle<ID3D12CommandQueue*>(7)); }
void start(OpenXRSession& s) {
    CHECK(init(s)); CHECK(s.swapchainFormat()==DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(s.eyeWidth(0)==100 && s.eyeWidth(1)==101);
    CHECK(s.eyeHeight(0)==80 && s.eyeHeight(1)==81);
    f->event=1; CHECK(s.pollEvents()); CHECK(s.isSessionRunning());
}
void clean(const Fake& x) {
    CHECK(!x.liveInstances && !x.liveSessions && !x.liveSpaces && !x.liveSwapchains);
}
void render(OpenXRSession& s, int e, bool complete=true) {
    ID3D12Resource* resource=nullptr; uint32_t index=42;
    CHECK(s.acquireSwapchainImage(e,&resource,&index)); CHECK(resource && index==0);
    if (complete) CHECK(s.markEyeRendered(e));
    CHECK(s.releaseSwapchainImage(e));
}
void initialization() {
    for (int point=1; point<=10; ++point) {
        Fake x; f=&x; x.initFail=point;
        OpenXRSession s(dispatch()); CHECK(!init(s)); CHECK(XR_FAILED(s.lastResult()));
        clean(x); s.shutdown(); clean(x);
    }
    for (int mismatch=0; mismatch<6; ++mismatch) {
        Fake x; f=&x;
        if (mismatch==0) ++x.graphics.adapterLuid.LowPart;
        if (mismatch==1) ++x.graphics.adapterLuid.HighPart;
        if (mismatch==2) x.graphics.maxFeatureLevel=D3D_FEATURE_LEVEL_11_0;
        if (mismatch==3) x.graphics.queueType=D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (mismatch==4) x.graphics.queueUsesDevice=false;
        if (mismatch==5) x.queried=false;
        OpenXRSession s(dispatch()); CHECK(!init(s));
        CHECK(s.lastResult()==XR_ERROR_GRAPHICS_DEVICE_INVALID); clean(x);
    }
    { Fake x; f=&x; x.supportedFormat=false; OpenXRSession s(dispatch());
      CHECK(!init(s)); CHECK(s.lastResult()==XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED); clean(x); }
    { Fake x; f=&x; x.viewCount=3; OpenXRSession s(dispatch()); CHECK(!init(s)); clean(x); }
    { Fake x; f=&x; OpenXRSession s(dispatch()); CHECK(!s.init(nullptr,nullptr)); clean(x); }
}
void sessionEvents() {
    Fake x; f=&x; OpenXRSession s(dispatch()); CHECK(init(s));
    x.event=1; x.beginSessionResult=XR_ERROR_RUNTIME_FAILURE;
    CHECK(!s.pollEvents()); CHECK(!s.isSessionRunning());
    x.event=1; x.beginSessionResult=XR_SUCCESS;
    CHECK(s.pollEvents()); CHECK(s.isSessionRunning());
    FrameState frame; CHECK(s.beginFrame(frame));
    x.event=2; CHECK(s.pollEvents()); CHECK(!s.isSessionRunning());
    CHECK(x.endFrames==1 && x.layers==0 && x.endSessions==1);
    x.pollResult=XR_ERROR_RUNTIME_FAILURE; CHECK(!s.pollEvents());
    s.shutdown(); clean(x);
    { Fake stopped; f=&stopped; OpenXRSession other(dispatch()); start(other);
      CHECK(other.beginFrame(frame)); stopped.endResult=XR_ERROR_RUNTIME_FAILURE;
      stopped.event=2; CHECK(!other.pollEvents());
      CHECK(stopped.endFrames==1 && stopped.endSessions==1 && !other.isSessionRunning());
      CHECK(other.lastResult()==XR_ERROR_RUNTIME_FAILURE); other.shutdown(); clean(stopped); }
    { Fake stopped; f=&stopped; OpenXRSession other(dispatch()); start(other);
      stopped.endSessionResult=XR_ERROR_RUNTIME_FAILURE; stopped.event=2;
      CHECK(!other.pollEvents()); CHECK(other.lastResult()==XR_ERROR_RUNTIME_FAILURE);
      other.shutdown(); clean(stopped); }
}
void frames() {
    for (int scenario=0; scenario<11; ++scenario) {
        Fake x; f=&x; OpenXRSession s(dispatch()); start(s); FrameState frame;
        if (scenario==0) x.waitFrameResult=XR_ERROR_RUNTIME_FAILURE;
        if (scenario==1) x.beginFrameResult=XR_ERROR_RUNTIME_FAILURE;
        if (scenario==2) x.locateResult=XR_ERROR_RUNTIME_FAILURE;
        if (scenario==3) x.viewCount=1;
        if (scenario==4) x.viewCount=3;
        if (scenario==5) x.flags=XR_VIEW_STATE_POSITION_VALID_BIT;
        if (scenario==6) x.flags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (scenario==7) x.shouldRender=false;
        if (scenario==8) x.badPose=true;
        if (scenario==9) x.badFov=true;
        bool success=s.beginFrame(frame);
        if (scenario<=4 || scenario==8 || scenario==9) CHECK(!success);
        else {
            CHECK(success);
            if (scenario==10) { render(s,0); render(s,1); }
            else CHECK(!frame.shouldRender && !frame.eyes[0].valid && !frame.eyes[1].valid);
            frame.predictedDisplayTime=999; CHECK(s.endFrame(frame)); CHECK(x.submittedTime==1234);
        }
        if (scenario==0) CHECK(x.beginFrames==0 && x.endFrames==0);
        else if (scenario==1) CHECK(x.endFrames==0);
        else CHECK(x.endFrames==1);
        CHECK(x.layers==(scenario==10 ? 1 : (scenario<2 ? -1 : 0)));
        CHECK(!s.endFrame(frame)); s.shutdown(); clean(x);
    }
}
void cleanupFailure() {
    Fake x; f=&x; OpenXRSession s(dispatch()); start(s); FrameState frame;
    x.locateResult=XR_ERROR_RUNTIME_FAILURE; x.endResult=XR_ERROR_SESSION_LOST;
    CHECK(!s.beginFrame(frame)); CHECK(x.endFrames==1);
    CHECK(s.lastResult()==XR_ERROR_SESSION_LOST); s.shutdown(); clean(x);
}
void ownership() {
    for (int scenario=0; scenario<9; ++scenario) {
        Fake x; f=&x; OpenXRSession s(dispatch()); start(s); FrameState frame;
        CHECK(s.beginFrame(frame));
        ID3D12Resource* resource=handle<ID3D12Resource*>(123); uint32_t index=123;
        if (scenario==0) { render(s,0,false); render(s,1,false); }
        if (scenario==1) { render(s,0); } // incomplete right eye
        if (scenario==2) {
            x.acquireResult=XR_ERROR_RUNTIME_FAILURE;
            CHECK(!s.acquireSwapchainImage(0,&resource,&index));
            CHECK(!resource && index==0 && x.waits==0 && x.releases==0);
        }
        if (scenario==3 || scenario==4) {
            x.failWaits=scenario==3 ? 1 : 2;
            x.waitResult=scenario==3 ? XR_TIMEOUT_EXPIRED : XR_ERROR_RUNTIME_FAILURE;
            CHECK(!s.acquireSwapchainImage(0,&resource,&index));
            CHECK(!resource && !s.releaseSwapchainImage(0) && x.releases==0);
        }
        if (scenario==5 || scenario==6) {
            CHECK(s.acquireSwapchainImage(0,&resource,&index));
            CHECK(s.markEyeRendered(0));
            x.failReleases=scenario==5 ? 1 : 2; x.releaseResult=XR_ERROR_RUNTIME_FAILURE;
            CHECK(!s.releaseSwapchainImage(0)); CHECK(x.owned[0]);
        }
        if (scenario==7) {
            x.index=9; CHECK(!s.acquireSwapchainImage(0,&resource,&index)); CHECK(!resource);
        }
        if (scenario==8) {
            CHECK(!s.acquireSwapchainImage(-1,&resource,&index));
            CHECK(!s.acquireSwapchainImage(2,&resource,&index));
            CHECK(!s.acquireSwapchainImage(0,nullptr,&index));
            CHECK(!s.markEyeRendered(0)); CHECK(!s.releaseSwapchainImage(0));
            CHECK(s.acquireSwapchainImage(0,&resource,&index));
            CHECK(!s.acquireSwapchainImage(0,&resource,&index)); CHECK(x.acquires==1);
        }
        bool ended=s.endFrame(frame); CHECK(ended==(scenario<2 || scenario==8));
        CHECK(x.endFrames==1 && x.layers==0);
        if (scenario==4 || scenario==6) {
            CHECK(x.owned[0]); CHECK(!s.beginFrame(frame)); // no reuse of poisoned ownership
        } else CHECK(!x.owned[0]);
        s.shutdown(); clean(x);
    }
    { Fake x; f=&x; OpenXRSession s(dispatch()); start(s); FrameState frame;
      CHECK(s.beginFrame(frame)); render(s,0); render(s,1);
      x.endResult=XR_ERROR_RUNTIME_FAILURE; CHECK(!s.endFrame(frame));
      CHECK(s.lastResult()==XR_ERROR_RUNTIME_FAILURE); CHECK(x.endFrames==1);
      CHECK(!s.endFrame(frame)); CHECK(x.endFrames==1); s.shutdown(); clean(x); }
}
void recentering() {
    Fake x; f=&x; OpenXRSession s(dispatch()); start(s); FrameState frame;
    s.recenter(); CHECK(s.getRecenterTransform()==identity());
    CHECK(s.beginFrame(frame)); s.recenter();
    CHECK(std::abs(s.getRecenterTransform()[12]+1)<1e-6);
    CHECK(std::abs(s.getRecenterTransform()[13]+2)<1e-6);
    const auto previous=s.getRecenterTransform(); CHECK(s.endFrame(frame));
    x.flags=0; CHECK(s.beginFrame(frame)); s.recenter();
    CHECK(s.getRecenterTransform()==previous); CHECK(s.endFrame(frame));
    x.flags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    x.vertical=true; CHECK(s.beginFrame(frame)); s.recenter();
    CHECK(s.getRecenterTransform()==previous); CHECK(s.endFrame(frame));
    s.shutdown(); clean(x);
}
}
int main() {
    try { initialization(); sessionEvents(); frames(); cleanupFailure(); ownership(); recentering();
          std::cout<<"XR offline initialization/events/frames/ownership/recenter regressions passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
