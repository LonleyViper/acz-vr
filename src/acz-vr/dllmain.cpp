// Minimal loader entry. All work begins lazily from a factory export.
#include "d3d12_hook.h"
#ifndef ACZ_VR_DLL_LIFECYCLE_OFFLINE_TEST
#include "openxr_session.h"
#include "stereo_renderer.h"
#include "xr_stereo_proxy.h"
#include "launch_mode_win.h"
#endif
#include <windows.h>
#include <atomic>
#include <mutex>

namespace {
// L3 is policy-only. No runtime enable path exists until native provenance,
// copy, synchronization and replay have independent proof and review.
constexpr bool gpu_transactions_enabled = false;
struct Runtime {
    acz::OpenXRSession session;
    acz::StereoRenderer renderer;
    acz::StereoConfig config;
    enum class Phase { idle, disabled, active };
    Phase phase = Phase::idle;
    acz::diagnostics::Mutex mutex;
};
// Do not invoke XR/COM destructors from CRT process detach. This scaffold is
// process-lifetime; installed shared-vtable hooks pin the proxy until process exit.
// Explicit unload/shutdown is unsupported until hooks can be safely quiesced.
struct Lifecycle {
    std::once_flag allocation, registration;
    Runtime* value = nullptr;
    std::atomic<bool> registered{false};
};
Lifecycle& lifecycle() { static Lifecycle value; return value; }
struct ReentryGuard {
    bool& flag;
    explicit ReentryGuard(bool& value) : flag(value) { flag = true; }
    ~ReentryGuard() { flag = false; }
};
Runtime* runtime(Lifecycle& state) {
    static thread_local bool allocating = false;
    if (allocating) return nullptr;
    ReentryGuard guard(allocating);
    std::call_once(state.allocation, [&] {
        // Catch inside call_once: allocation/construction failure is terminal,
        // rather than the standard exception-triggered retry on the next call.
        try { state.value = new Runtime; } catch (...) {}
    });
    return state.value;
}
void deviceReady(Lifecycle& state, ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        if (!state.registered.load() || !device || !queue) return;
        if (!gpu_transactions_enabled) return;
        auto* r = runtime(state);
        if (!r) return;
        // Device-ready is a one-shot notification: wait through a competing
        // callback's short CPU state transition rather than lose initialization.
        // No native/COM work runs under this mutex; init/poll reentry observes
        // disabled after acquiring it, so waiting here cannot deadlock reentry.
        std::unique_lock lock(r->mutex);
        if (r->phase != Runtime::Phase::idle) return;
        // init(false), init exceptions, and calibration exceptions are terminal.
        // Publish disabled before releasing the lock: other callbacks must not
        // touch the session during initialization. init can call shutdown/COM,
        // so its native work and internal cleanup run outside this mutex.
        // Retain exceptional partial state for process lifetime, without retry.
        r->phase = Runtime::Phase::disabled;
        lock.unlock();
        try {
            if (!r->session.init(device, queue)) return;
            r->renderer.setDrawCPTCalibration(acz::identity());
            r->renderer.setAircraftPose(acz::identity(), acz::identity());
            lock.lock();
            r->phase = Runtime::Phase::active;
        } catch (...) {}
    } catch (...) {} // Also contains allocation/call_once/mutex failures.
}
void execute(Lifecycle& state) noexcept {
    try {
        if (!state.registered.load()) return;
        if (!gpu_transactions_enabled) return;
        auto* r = runtime(state);
        if (!r) return;
        std::unique_lock lock(r->mutex, std::try_to_lock);
        if (!lock || r->phase != Runtime::Phase::active) return;
        // Suppress competing/reentrant callbacks while the poll owns session
        // access. Native event handling (including EndSession) runs unlocked.
        // Only normal completion restores active; a throwing poll is terminal.
        r->phase = Runtime::Phase::disabled;
        lock.unlock();
        try {
            (void)acz::pollReadySession(r->session, true);
            lock.lock();
            r->phase = Runtime::Phase::active;
        } catch (...) {}
    } catch (...) {}
    // Poll READY before testing running. This is only an event pump: submissions
    // are not display frames; no complete bindings or stereo replay exist yet.
    // Deliberately do not begin/acquire/submit XR frames here. The future display
    // frame owner must end begun, unrendered frames with zero layers (see
    // endUnrenderedFrame) and submit eyes only after confirmed rendering/GPU sync.
}
void onDeviceReady(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
#ifndef ACZ_VR_DLL_LIFECYCLE_OFFLINE_TEST
    // J-xr-stereo-output (default OFF: one sink lookup). Independent of the
    // dormant gpu_transactions_enabled scaffold below; makes no XR call here.
    acz::xr_stereo::on_device_ready(device, queue);
#endif
    try { deviceReady(lifecycle(), device, queue); } catch (...) {}
}
void onExecuteCommandLists(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept {
    try { execute(lifecycle()); } catch (...) {}
}
void registerCallbacks(Lifecycle& state) noexcept {
    static thread_local bool registering = false;
    if (registering) return;
    ReentryGuard guard(registering);
    try {
        std::call_once(state.registration, [&] {
            try {
                acz::setDeviceReadyCallback(onDeviceReady);
                acz::setExecuteCallback(onExecuteCommandLists);
                state.registered.store(true);
            } catch (...) {
                // A setter may already have published one callback. Leave it
                // installed but inert, including synchronous setter reentry.
                // No rollback setters or unsafe partial registration retries.
            }
        });
    } catch (...) {}
}
}
namespace acz {
void initializeProxyCallbacks() {
    // Optional instrumentation failure cannot replace the native factory result.
    try { registerCallbacks(lifecycle()); } catch (...) {}
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    // Loader lock is never an owner context: no session mutex, wait or foreign
    // COM cleanup here, for either explicit detach or process termination.
    // Populated hooks pin the module. Orderly cleanup requires an explicit
    // outside-loader-lock owner; uncertain owners remain process-lifetime.
#ifndef ACZ_VR_DLL_LIFECYCLE_OFFLINE_TEST
    // LAUNCH-MODE (docs/world-render/launch-mode.md): one CreateThread for the launcher's "VR Mode" combo (acz.exe only); no other work.
    if (reason == DLL_PROCESS_ATTACH) acz::launch_mode::start_launcher_watch(instance);
#else
    (void)instance; (void)reason;
#endif
    return TRUE;
}
