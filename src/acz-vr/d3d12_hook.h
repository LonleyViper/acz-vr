#pragma once
// d3d12_hook.h — DXGI proxy and D3D12 command-list vtable hooking.
// The DLL is named dxgi.dll and placed in the game directory; the game
// loads it instead of the system one. We forward the three factory exports and hook the
// D3D12 device and command list to intercept DrawCPT draws.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <functional>
#include <mutex>
#include "draw_capture.h"
#include "native_binding_observer.h"
#include "diagnostics_log.h"

namespace acz {

// D-81 diagnostics-only metadata check, synchronous on DrawCPT-identity-matched
// draws under HookState::mutex. Public resolvers lock; internal *_locked
// variants run under that single acquisition. Currentness is checked at the
// draw recording point, not deferred. The common skipped path allocates nothing;
// at most one attempt per second / 64 per process may allocate metadata. No
// byte reads/copies/freezes, native calls or I/O occur inside that mutex.
// This does not set requirements_proven/CPU/GPU readiness or enable capture.
inline constexpr uint64_t requirements_interval_us=1000000;
inline constexpr unsigned requirements_attempt_limit=64;

// Callback when a D3D12 device is first seen (from swap-chain creation).
using DeviceReadyCallback = std::function<void(ID3D12Device*, ID3D12CommandQueue*)>;

// Callback for each draw call on a hooked command list.
struct DrawCallInfo {
    ID3D12GraphicsCommandList* cmdList;
    ID3D12PipelineState*       pso;
    bool                       hasGeometryShader;
    UINT                       vertexCountOrIndexCount;
    UINT                       instanceCount;
    bool                       indexed;
    UINT                       startVertexOrIndex = 0;
    INT                        baseVertex = 0;
    UINT                       startInstance = 0;
};
using DrawCallback = std::function<void(const DrawCallInfo&)>;

// Submission observation only: ExecuteCommandLists is not a display frame.
using ExecuteCallback = std::function<void(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>;

void setDeviceReadyCallback(DeviceReadyCallback cb);
void setDrawCallback(DrawCallback cb);
void setExecuteCallback(ExecuteCallback cb);

// Owned binding observation made before forwarding a draw. Native observations
// are incomplete CPU snapshots: no resource copy/descriptor/indexing seam has
// been established. Consumers must call capture::freeze with proven Inputs.
using BindingCallback = std::function<void(std::shared_ptr<const capture::State>, capture::Command)>;
void setBindingCallback(BindingCallback cb);
// Extended documented base-device/list ABI interception. Stream PSOs, bundles,
// textures, resource contents and GPU completion remain unsupported.
// Bundles/indirect calls invalidate tracking until successful Reset/ClearState.
// Native BufferView.resource values are observed GPU VA tokens, not resolved
// resource identities/offsets. No native complete CPU-copy provider is installed.
// Observation retains bound COM objects and PSO roots; Release's visible count
// includes these references. State copies carry the list's lifetime token.
// PSO/root compatibility supports only explicit, decoded roots with the same
// observed canonical COM object lifetime; distinct equivalent layouts and
// embedded/null roots remain unsupported. Repeated IUnknown/root-signature creation
// preserves a live token only with verified canonical IUnknown identity, exact
// owned serialized bytes/device/node/IID, and isolated observed event ordering.
// First provenance may publish a fresh token during overlapping observations
// only when all active outputs/retirements have known distinct addresses, the
// canonical root is reference-pinned, and this event owns publication. Unknown
// outputs or same-address overlap fail closed; overlap never certifies continuity.
// Root maps use canonical IUnknown keys; observed base/canonical interface
// aliases resolve PSO/list bindings and Release to that key. Both Release
// vtables are hooked; cleanup compares the token captured before forwarding.
// Conflicting aliases, failed decoding/QI/allocation and retirement/reentry fail
// closed; same-address creation after final Release has a fresh lifetime. This
// subset does not assert that real devices return deduplicated root objects.
// SDK 10.0.26100 d3d12.idl defines
// GRAPHICS_PIPELINE_STATE_DESC.pRootSignature and SetGraphicsRootSignature;
// ID3D12RootSignature has no layout mutator. d3d12sdklayers.h message 201
// diagnoses draw root-signature mismatch. These contracts justify the identical
// immutable-object subset, not a general structural-equivalence algorithm.
// Compatibility alone supplies no resource/indexing/copy or replay proof.
// Root diagnostics use one stable reason per failed gate:
// native_failure, null_out, null_object, unsupported_iid, root_qi_failed,
// root_qi_null, root_alias, canonical_qi_failed, canonical_mismatch, null_blob,
// empty_blob, oversized_blob, serialized_failure, lifetime_hook,
// continuity_overlap, ambiguous_order, unresolved_order, stale_invalidation,
// stale_publication, identity_exhausted, allocation_failure,
// observation_exception. Each rejection text starts with "root rejected: ".
// Record a=accepted, b=SerializedRootFailure (literal enum, zero otherwise),
// c=last native/QI/SDK HRESULT as uint32 bits. Bounded text includes the input
// IID/node/byte count and ordering context sampled at the publication gate:
// order bits 0=initial isolation, 1=current isolation, 2=ambiguity,
// 3=prior provenance, 4=independent active AND retained completed footprints.
// creates/releases count active MetadataEvents (releases includes PSO creation
// observations); bindings counts BindingEvents. Fresh roots require fully known
// disjoint addresses, canonical keys and PSO/root dependencies on the binding
// cohort's begin/end clock. Completed metadata uses a fixed 64-entry history;
// lost history/unidentified metadata retirements fail unresolved_order; unknown
// creation outputs, conflicts and reentry fail ambiguous_order. Repeated
// live-object continuity still requires isolation.
// block is bounded static text identifying the blocking condition (including
// unknown output, history loss or dependency/address conflict), never an address.
// A binding creation with only its native output address identified remains
// blocked until canonical independence is provable; no new COM query is inferred.
// ids=B/R/U are per-record opaque equivalence-class tags for the returned,
// QI(root), and QI(IUnknown) pointers: 0 absent, 1 equal to returned, 2 equal
// to a distinct root, 3 otherwise. eq bits 0=B==R, 1=B==U, 2=R==U (nonnull).
// No pointer bits are emitted; tags describe comparisons, not global identity.
// Zero context means the gate was not reached. These are observation facts,
// never runtime capture/readiness proof. Decoding uses from_serialized's
// bounded CPU adapter; b classifies decoder rejection and detail copies at most
// 63 characters of its header/adapter detail (empty for literal SDK failures).
bool hookCaptureDevice(ID3D12Device* device);
bool hookCaptureCommandList(ID3D12GraphicsCommandList* list,
    diagnostics::ListSeam seam=diagnostics::ListSeam::creation);

// Base ID3D12Device only: future CreateDescriptorHeap (CBV/SRV/UAV or sampler, node0),
// CreateCommittedResource (base IID, unshared single-node buffers), structured
// BUFFER/UNKNOWN/NONE/default-component-mapping SRV and aligned buffer CBV
// writes, unsupported base descriptor-write invalidation, descriptor copies,
// and observed base heap/resource Release. Placed/reserved buffers disable
// metadata rather than infer aliasing. No complete interception claim:
// pre-discovery objects/lists, OpenSharedHandle, Device1+ creation extensions,
// resource interface aliases and missed/reentrant events are
// unsupported. Independent concurrent binding events commute; conflicting known
// footprints invalidate their destinations/objects, unresolved footprints or
// reentry disable metadata. Resolutions stay unavailable throughout an in-flight
// cohort, including creation outputs not yet identified. Sampler snapshots carry
// owned SDK fields only; no resource or capture/freeze readiness.
// Lists require observed creation OR their next observed successful Reset
// after complete hook installation, plus GetDevice ownership on an observed
// device. Late lists stay ineligible before that Reset; ClearState cannot
// establish their initial recording boundary. Base and List1..10 aliases are
// certified by QI(IUnknown), share one lifetime/state, and each exposed table
// has checked per-vtable dispatch. Separate CommandList/DeviceChild/Object
// ancestor tables and canonical IUnknown have Release coverage. List4 SetPipelineState1 and List10 SetProgram
// invalidate unsupported pipeline state. Queries/cleanup occur without locks.
// Device destruction and later
// unsupported device/resource QI disable metadata. Earlier extension/history
// gaps cannot be recovered or certified. No complete-history assertion exists.
// Hooks never map/read resources, record/submit/wait GPU work or enable capture.
// Resolutions are owned metadata stamped with live list identity/generation/
// revision. Root sizes/layout are explicit provider inputs, never E2 inference.
// This installs metadata hooks in addition to the existing capture foundation.
// False includes partial installation; installed slots keep forwarding.
// Canonical-equal ID3D12CompatibilityDevice QI additionally hooks distinct
// compatibility tables (QI/Release/shared resource/shared heap), with the same
// committed-buffer registry/cohort/lifetime rules. Same-address/device-table or
// other interface-table collisions reject before patching. Read-only Reflect
// stays native. Only base-IID, single-node, unshared row-major buffers with no
// resource/compatibility flags, D3D11 flags, lifetime tracker or owning swapchain
// are supported. Shared memory heaps and other argument/identity/output gaps
// produce permanent opaque-address tombstones plus bounded device diagnostics;
// they do not disable unrelated metadata. Null outputs have no address to mark.
// Patch/allocation/native-exception gaps and unknown/On12 QI still fail globally
// closed. All 34 device rejection reasons and 182 overlap reasons are retained;
// compatibility adds nine per-call diagnostic texts and one accepted text.
bool hookNativeBindingDevice(ID3D12Device* device);
binding::NativeResolution resolveNativeTableBinding(const capture::State&, uint32_t parameter,
                                                    capture::Id heap, uint32_t descriptor_offset);
binding::NativeResolution resolveNativeRootBinding(const capture::State&, uint32_t parameter,
                                                   uint32_t bytes, uint32_t elements, uint32_t stride);
binding::Status currentNativeBinding(const binding::NativeResolution&, const capture::State&);

// Load the real system dxgi.dll and resolve its exports.
bool loadRealDXGI();

// Get the real D3D12 device from a hooked swap chain (after creation).
ID3D12Device* getHookedDevice();
ID3D12CommandQueue* getHookedQueue();
// Saved native dispatch for a vtable slot of a COM object: the retained
// original when this proxy patched that slot of the object's table, else the
// live table entry (then not a proxy hook). Proxy-owned work (the XR stereo
// consumer) calls through this so no proxy hook observes its own objects.
// Takes the hook-state mutex briefly; never call it while holding that mutex.
void* nativeDispatch(void* object, int slot) noexcept;
// J-xr-stereo-output: optional stop hook for the orderly drain path (set by
// dllmain's registration; null = none). Nonblocking.
using StopHook = void (*)() noexcept;
void setOrderlyStopHook(StopHook) noexcept;
// J-xr-stereo-output: PROXY-INTERNAL CREATION SCOPE. The XR owner thread
// enters it around every runtime/consumer call (xr_owner call bracket). Inside
// the scope on the calling thread, the device creation hooks forward natively
// but never track, register or disable on the created descriptor heaps or
// resources (they belong to the XR runtime / the consumer, not the game).
// `window` calls additionally open a process-wide XR call window: while one is
// in flight, a heap/shared-buffer registration REJECTION on any other thread
// (e.g. a runtime worker thread) is counted instead of disabling the
// observer. Outside the window the existing fail-closed rules are unchanged.
void enterProxyCreationScope(bool window) noexcept;
void leaveProxyCreationScope(bool window) noexcept;
bool proxyCreationScopeActive() noexcept; // calling thread
// Astra issue 29 P2: a window is a bounded, counted EXCEPTION, never provenance.
// Active only while a window call is in flight AND at most kXrWindowCapMs since
// the window opened (a stuck call never extends it). A rejection tolerated in
// the window leaves the object ORIGIN-UNKNOWN: it is tombstoned (untracked), so
// every dependent registration/resolution through it refuses; it is never
// treated as proxy-owned. Same-thread scope handling above is separate.
constexpr uint64_t kXrWindowCapMs = 250;
bool xrCallWindowOpen() noexcept;         // a window call is in flight (uncapped)
bool xrCallWindowActive() noexcept;       // open AND inside the cap
uint64_t xrWindowTolerated() noexcept;    // counted origin-unknown exceptions
uint64_t xrWindowExpired() noexcept;      // rejections refused because the cap expired
struct ProxyCreationScope {
    bool window;
    explicit ProxyCreationScope(bool w = true) noexcept : window(w) { enterProxyCreationScope(window); }
    ~ProxyCreationScope() { leaveProxyCreationScope(window); }
    ProxyCreationScope(const ProxyCreationScope&) = delete;
    ProxyCreationScope& operator=(const ProxyCreationScope&) = delete;
};

// False includes partial installation/protection failure; installed slots still forward.
// Hooks pin the DLL; dynamic unload is unsupported.
// Hook a D3D12 command queue's vtable (ExecuteCommandLists).
bool hookCommandQueue(ID3D12CommandQueue* queue);

// Hook a D3D12 graphics command list's vtable (draw calls, SetPipelineState).
bool hookCommandList(ID3D12GraphicsCommandList* list);

// Borrowed PSO metadata; remove before PSO destruction/reuse.
void markPSOHasGS(ID3D12PipelineState* pso, bool hasGS);
void forgetPSO(ID3D12PipelineState* pso);
// Factory-export initialization, outside loader lock.
void initializeProxyCallbacks();
// Diagnostics only. Termination skips COM. false = busy; shutdown did not start.
// Never waits; retry from an owner-controlled quiescent context, outside DllMain.
bool shutdownDiagnosticOwnership(bool processTerminating) noexcept;

template<class Session> bool pollReadySession(Session& session, bool initialized) {
    return initialized && session.pollEvents() && session.isSessionRunning();
}
template<class Session, class Frame> bool endUnrenderedFrame(Session& session, Frame frame) {
    frame.shouldRender = false;
    return session.endFrame(frame);
}
} // namespace acz
