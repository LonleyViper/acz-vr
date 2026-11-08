// Standalone MSVC /std:c++17 /EHsc: includes the production callback boundaries.
// Fake session/renderer/setters only; no real loader, COM, GPU or XR dispatch.
// Executor: register this source as a standalone CTest later (CMake out of scope).
#include "../../src/acz-vr/d3d12_hook.h"
#include "../../src/acz-vr/cockpit_math.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static bool failAllocation = false;
static int failedAllocations = 0;
void* operator new(std::size_t size) {
    if (failAllocation) { failAllocation=false; ++failedAllocations; throw std::bad_alloc(); }
    if (void* value=std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace fake {
int constructed=0, initCalls=0, polls=0, calibrations=0, poses=0, cleanups=0;
int waits=0, begins=0, acquires=0, releases=0, submits=0, math=0;
int deviceSets=0, executeSets=0, registrationFailure=0;
bool throwConstructor=false, throwInit=false, initResult=true;
bool throwPoll=false, pollResult=true, running=true, throwCalibration=false, throwPose=false;
bool reenterSetters=false;
std::function<void()> constructorReentry, initReentry, pollReentry;
acz::DeviceReadyCallback deviceCallback;
acz::ExecuteCallback executeCallback;
}
static unsigned ownershipShutdownCalls=0;
namespace acz {
bool shutdownDiagnosticOwnership(bool terminating) noexcept {CHECK(!terminating);++ownershipShutdownCalls;return true;}
// Only operations actually called by dllmain are faked. No lifecycle or eye
// math implementation is copied into this fixture.
struct OpenXRSession {
    OpenXRSession() {
        ++fake::constructed;
        if (fake::constructorReentry) fake::constructorReentry();
        if (fake::throwConstructor) throw std::runtime_error("constructor");
    }
    ~OpenXRSession() { ++fake::cleanups; }
    bool init(ID3D12Device*, ID3D12CommandQueue*) {
        ++fake::initCalls;
        if (fake::initReentry) fake::initReentry();
        if (fake::throwInit) throw std::bad_alloc();
        return fake::initResult;
    }
    bool pollEvents() {
        ++fake::polls;
        if (fake::pollReentry) fake::pollReentry();
        if (fake::throwPoll) throw std::runtime_error("poll");
        return fake::pollResult;
    }
    bool isSessionRunning() const { return fake::running; }
    // Tripwires if a frame owner is ever accidentally added to this callback.
    bool beginFrame(...) { ++fake::waits; ++fake::begins; return true; }
    bool acquireSwapchainImage(...) { ++fake::acquires; return true; }
    bool releaseSwapchainImage(...) { ++fake::releases; return true; }
    bool endFrame(...) { ++fake::submits; return true; }
};
struct StereoConfig {};
struct StereoRenderer {
    void setDrawCPTCalibration(const Mat4&) {
        ++fake::calibrations;
        if (fake::throwCalibration) throw std::runtime_error("calibration");
    }
    void setAircraftPose(const Mat4&, const Mat4&) {
        ++fake::poses;
        if (fake::throwPose) throw std::runtime_error("pose");
    }
    void computeEyeMatrices(...) { ++fake::math; throw std::runtime_error("unexpected frame math"); }
};
void setDeviceReadyCallback(DeviceReadyCallback cb) {
    ++fake::deviceSets;
    if (fake::registrationFailure==1) throw std::bad_alloc();
    fake::deviceCallback=std::move(cb);
    if (fake::reenterSetters) {
        initializeProxyCallbacks();
        fake::deviceCallback(reinterpret_cast<ID3D12Device*>(1), reinterpret_cast<ID3D12CommandQueue*>(2));
    }
    if (fake::registrationFailure==2) throw std::runtime_error("device published then failed");
}
void setExecuteCallback(ExecuteCallback cb) {
    ++fake::executeSets;
    if (fake::registrationFailure==3) throw std::bad_alloc();
    fake::executeCallback=std::move(cb);
    if (fake::reenterSetters) {
        initializeProxyCallbacks();
        fake::executeCallback(nullptr,0,nullptr);
    }
    if (fake::registrationFailure==4) throw std::runtime_error("execute published then failed");
}
}
#define ACZ_VR_DLL_LIFECYCLE_OFFLINE_TEST
#include "../../src/acz-vr/dllmain.cpp"
static_assert(!gpu_transactions_enabled, "production lifecycle must be inert by default");

static ID3D12Device* device=reinterpret_cast<ID3D12Device*>(1);
static ID3D12CommandQueue* queue=reinterpret_cast<ID3D12CommandQueue*>(2);
static void ready() { if (fake::deviceCallback) fake::deviceCallback(device,queue); }
static void pump() { if (fake::executeCallback) fake::executeCallback(queue,0,nullptr); }
static void reset() {
    // Test-only reset, after all fixture threads join and published callbacks are
    // cleared. Production has no reset/unload and retains partial native state.
    fake::deviceCallback={}; fake::executeCallback={};
    auto* state=&lifecycle();
    delete state->value; state->~Lifecycle(); new (state) Lifecycle;
    fake::constructorReentry={}; fake::initReentry={}; fake::pollReentry={};
    fake::constructed=fake::initCalls=fake::polls=fake::calibrations=fake::poses=fake::cleanups=0;
    fake::deviceSets=fake::executeSets=fake::registrationFailure=0;
    fake::throwConstructor=fake::throwInit=fake::throwPoll=fake::throwCalibration=fake::throwPose=false;
    fake::initResult=fake::pollResult=fake::running=true;
    fake::reenterSetters=false;
}
static void noFrames() {
    CHECK(fake::waits==0 && fake::begins==0 && fake::acquires==0 &&
          fake::releases==0 && fake::submits==0 && fake::math==0);
}
int main() {
    CHECK(DllMain(nullptr,DLL_PROCESS_ATTACH,nullptr)==TRUE);
    CHECK(fake::deviceSets==0 && fake::constructed==0);
    CHECK(DllMain(nullptr,DLL_PROCESS_DETACH,reinterpret_cast<void*>(1))==TRUE);
    CHECK(ownershipShutdownCalls==0);
    // Exceptions before and after setter publication: no retries, rollback or
    // active partial callbacks, even when invoked again after fault removal.
    for (int failure=1; failure<=4; ++failure) {
        reset(); fake::registrationFailure=failure; fake::reenterSetters=true;
        acz::initializeProxyCallbacks();
        CHECK(!lifecycle().registered.load());
        CHECK(fake::deviceSets==1 && fake::executeSets==(failure>=3 ? 1 : 0));
        fake::registrationFailure=0;
        acz::initializeProxyCallbacks(); ready(); pump();
        CHECK(fake::deviceSets==1 && fake::executeSets==(failure>=3 ? 1 : 0));
        CHECK(fake::constructed==0 && fake::initCalls==0 && fake::polls==0);
        noFrames();
    }
    reset(); fake::reenterSetters=true; acz::initializeProxyCallbacks();
    CHECK(lifecycle().registered.load() && fake::constructed==0);
    acz::initializeProxyCallbacks(); CHECK(fake::deviceSets==1 && fake::executeSets==1);
    fake::deviceCallback(nullptr,queue); fake::deviceCallback(device,nullptr);
    // These are the production callback boundaries. The default guard must
    // prevent construction, initialization, poll, GPU action and allocation.
    fake::throwConstructor=fake::throwInit=fake::throwPoll=true;
    const int priorFailures=failedAllocations;
    failAllocation=true;
    for (int i=0;i<128;++i) { ready(); pump(); }
    CHECK(failAllocation && failedAllocations==priorFailures);
    failAllocation=false;
    CHECK(lifecycle().value==nullptr);
    CHECK(fake::constructed==0 && fake::initCalls==0 && fake::polls==0 &&
          fake::calibrations==0 && fake::poses==0 && fake::cleanups==0);
    noFrames();
    // Reentry at setter publication is covered above; concurrent callbacks
    // must also remain inert without a runtime allocation or session action.
    std::thread other([] { for (int i=0;i<128;++i) { ready(); pump(); } });
    for (int i=0;i<128;++i) { ready(); pump(); }
    other.join();
    CHECK(lifecycle().value==nullptr && fake::constructed==0 && fake::polls==0);
    noFrames();
    CHECK(DllMain(nullptr,DLL_PROCESS_DETACH,nullptr)==TRUE);
    CHECK(ownershipShutdownCalls==0);
    CHECK(lifecycle().registered.load() && fake::cleanups==0);
    ready();pump();CHECK(lifecycle().value==nullptr && fake::constructed==0);
    reset();
    std::puts("DLL lifecycle offline fixtures passed (fake callbacks only; no runtime VR claim)");
}
