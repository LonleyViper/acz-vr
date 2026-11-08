// VSCREEN capture at the game's Present (see vscreen_capture.h, docs/world-render/vscreen.md section 3). DLL only.
// Threads: on_present() runs ONLY on the game's Present thread (the existing Present vtable hook, outer call). The XR owner thread uses
// capture_acquire()/capture_release() (the Ring's mutex; the writer side is try-only) and reads the published image pointers/extents.
// Every D3D12 call goes through acz::nativeDispatch (the proxy's own hooks never see these objects or this submission), objects are created
// inside a proxy creation scope, and nothing is ever destroyed while a reader may hold it (re-creation needs Ring::retire_all: no slot held,
// every capture completed). Default OFF: capture_configure(vscreen:1) arms it; it records only while the XR consumer requests it.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "vscreen_capture.h"
#include "d3d12_hook.h"
#include "xr_vscreen_vs.h"   // g_xr_vscreen_vs (fxc, xr_vscreen.hlsl vs_screen)
#include "xr_vscreen_ps.h"   // g_xr_vscreen_ps (fxc, xr_vscreen.hlsl ps_screen)

#include <atomic>
#include <cstdio>

namespace acz::vscreen {
namespace {
enum : int {
    kRelease = 2,
    kQueueGetDevice = 7, kQueueExecute = 10, kQueueSignal = 14,
    kDevCreateAllocator = 9, kDevCreateGraphicsPso = 10, kDevCreateList = 12, kDevCreateHeap = 14, kDevIncrement = 15, kDevCreateRoot = 16,
    kDevCreateSrv = 18, kDevCreateRtv = 20, kDevCreateCommittedResource = 27, kDevCreateFence = 36,
    kListClose = 9, kListReset = 10, kListDraw = 12, kListCopyResource = 17, kListTopology = 20, kListViewports = 21, kListScissors = 22,
    kListBarrier = 26, kListHeaps = 28, kListRoot = 30, kListTable = 32, kListTargets = 46,
    kAllocReset = 8, kFenceCompleted = 8,
};
constexpr unsigned kMaxFailures = 8;
template <class Fn> Fn native(void* o, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(o, slot)); }
template <class T> void rel(T*& p) noexcept {
    if (!p) return;
    if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(p, kRelease)) fn(reinterpret_cast<IUnknown*>(p));
    p = nullptr;
}
// Back-buffer format -> (staging resource format, SRV format). Unknown formats are refused (no capture).
bool formats_for(DXGI_FORMAT bb, DXGI_FORMAT& staging, DXGI_FORMAT& view) noexcept {
    switch (bb) {
    case DXGI_FORMAT_R10G10B10A2_UNORM: staging = view = bb; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: staging = view = bb; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: staging = DXGI_FORMAT_R8G8B8A8_TYPELESS; view = DXGI_FORMAT_R8G8B8A8_UNORM; return true;   // raw bytes
    case DXGI_FORMAT_B8G8R8A8_UNORM: staging = view = bb; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: staging = DXGI_FORMAT_B8G8R8A8_TYPELESS; view = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: staging = view = bb; return true;
    default: return false;
    }
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) noexcept {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    return x;
}

struct Capture {
    // shared (atomics / Ring)
    std::atomic<bool> configured{false};
    std::atomic<bool> disabled{false};
    std::atomic<bool> lost{false};      // a capture fence reported device removal (Astra finding 2): nothing is reused, acquired or re-created any more
    std::atomic<uint64_t> want_until{0};
    std::atomic<uint32_t> want_w{2400}, want_h{1350};
    std::atomic<ID3D12Fence*> fence_pub{nullptr};
    std::atomic<ID3D12Resource*> out_pub[kRingSlots]{};
    std::atomic<uint32_t> out_w{0}, out_h{0};
    std::atomic<uint64_t> presents{0}, wanted{0}, recorded{0}, failures{0}, recreated{0};
    std::atomic<uint32_t> src_w{0}, src_h{0}, src_format{0};
    std::atomic<const char*> last_failure{"none"};
    Ring ring;
    // Present thread only
    void* swapchain = nullptr;
    ID3D12CommandQueue* q = nullptr;   // the swap chain's queue (referenced for the process)
    ID3D12Device* dev = nullptr;       // its device (referenced for the process)
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* srv = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    UINT rtv_step = 0;
    ID3D12CommandAllocator* alloc[kRingSlots] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* staging = nullptr;
    ID3D12Resource* out[kRingSlots] = {};
    uint32_t sw = 0, sh = 0, ow = 0, oh = 0;
    DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN, bbfmt = DXGI_FORMAT_UNKNOWN;
    bool pipeline = false, pipeline_dead = false;
};
Capture g_cap;

void fail(const char* why, bool fatal) noexcept {
    g_cap.last_failure.store(why, std::memory_order_relaxed);
    const uint64_t n = g_cap.failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fatal || n >= kMaxFailures) g_cap.disabled.store(true, std::memory_order_release);
}
// Astra finding 2: UINT64_MAX (device removed) is LOSS, never a completed value. Loss disables the capture for the process.
Completion completion(ID3D12Fence* f) noexcept {
    auto fn = f ? native<UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*)>(f, kFenceCompleted) : nullptr;
    Completion r = completion_from_raw(fn ? fn(f) : 0);
    if (r.lost) { g_cap.lost.store(true, std::memory_order_release); fail("fence-lost(device-removed)", true); }
    return r;
}
QueueAssoc g_assoc;   // Astra finding 3: filled by the DXGI factory wrappers (note_swapchain_queue)
// The queue's CURRENT device (one reference taken and dropped; the queue keeps its device alive). null = lookup failed.
void* queue_device(void* qv) noexcept {
    auto* q = static_cast<ID3D12CommandQueue*>(qv);
    auto getDevice = q ? native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, REFIID, void**)>(q, kQueueGetDevice) : nullptr;
    ID3D12Device* d = nullptr;
    if (!getDevice || FAILED(getDevice(q, IID_PPV_ARGS(&d))) || !d) return nullptr;
    void* r = d;
    rel(d);
    return r;
}

// The swap chain's queue (from the creation association, re-validated on use), the fence, the list, the per-slot allocators and the
// pass pipeline (once).
bool ensure_base(void* swapchain) noexcept {
    Capture& c = g_cap;
    const char* why = "none";
    void* qv = resolve_queue(g_assoc, swapchain, queue_device, why);
    if (!qv) { fail(why, false); return false; }   // not fatal by itself (kMaxFailures applies): never a substitute queue
    if (c.q) {
        // One queue for the process (fence values are ordered on ONE queue); a swap chain of another queue disables the capture.
        if (qv != static_cast<void*>(c.q)) { fail("swapchain-queue-changed", true); return false; }
        c.swapchain = swapchain;
        return c.pipeline;
    }
    c.q = static_cast<ID3D12CommandQueue*>(qv);   // the association holds the reference (process lifetime)
    if (c.q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { fail("swapchain-queue-not-direct", true); return false; }
    auto getDevice = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, REFIID, void**)>(c.q, kQueueGetDevice);
    if (!getDevice || FAILED(getDevice(c.q, IID_PPV_ARGS(&c.dev))) || !c.dev) { c.dev = nullptr; fail("queue-device", true); return false; }
    c.swapchain = swapchain;
    acz::ProxyCreationScope scope(false);
    auto createFence = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**)>(c.dev, kDevCreateFence);
    if (!createFence || FAILED(createFence(c.dev, 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence))) || !c.fence) { c.fence = nullptr; fail("fence", true); return false; }
    auto createAlloc = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**)>(c.dev, kDevCreateAllocator);
    for (auto& a : c.alloc)
        if (!createAlloc || FAILED(createAlloc(c.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) || !a) { a = nullptr; fail("allocator", true); return false; }
    auto createList = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**)>(c.dev, kDevCreateList);
    if (!createList || FAILED(createList(c.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc[0], nullptr, IID_PPV_ARGS(&c.list))) || !c.list) { c.list = nullptr; fail("list", true); return false; }
    auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(c.list, kListClose);
    if (!close || FAILED(close(c.list))) { fail("list-close", true); return false; }
    // Pipeline: [0] table t0 (the staging copy), static linear-clamp s0; PSO -> R8G8B8A8_UNORM, opaque, no depth.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1; param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &param; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (error) error->Release();
    if (FAILED(hr) || !blob) { if (blob) blob->Release(); fail("root-serialize", true); return false; }
    auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(c.dev, kDevCreateRoot);
    hr = createRoot ? createRoot(c.dev, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&c.root)) : E_FAIL;
    blob->Release();
    if (FAILED(hr) || !c.root) { c.root = nullptr; fail("root", true); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = c.root;
    pd.VS = {g_xr_vscreen_vs, sizeof(g_xr_vscreen_vs)};
    pd.PS = {g_xr_vscreen_ps, sizeof(g_xr_vscreen_ps)};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(c.dev, kDevCreateGraphicsPso);
    if (!createPso || FAILED(createPso(c.dev, &pd, IID_PPV_ARGS(&c.pso))) || !c.pso) { c.pso = nullptr; fail("pso", true); return false; }
    auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(c.dev, kDevCreateHeap);
    auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(c.dev, kDevIncrement);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!createHeap || !increment || FAILED(createHeap(c.dev, &hd, IID_PPV_ARGS(&c.srv))) || !c.srv) { c.srv = nullptr; fail("srv-heap", true); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = kRingSlots;
    if (FAILED(createHeap(c.dev, &rd, IID_PPV_ARGS(&c.rtv))) || !c.rtv) { c.rtv = nullptr; fail("rtv-heap", true); return false; }
    c.rtv_step = increment(c.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!c.rtv_step) { fail("rtv-step", true); return false; }
    c.fence_pub.store(c.fence, std::memory_order_release);
    c.pipeline = true;
    return true;
}

// The staging copy (back-buffer size/format) and the slot images (the requested screen size). Re-created only through Ring::retire_all.
bool ensure_images(const D3D12_RESOURCE_DESC& bb) noexcept {
    Capture& c = g_cap;
    const uint32_t ww = std::max<uint32_t>(64, std::min<uint32_t>(c.want_w.load(std::memory_order_relaxed), 4096));
    const uint32_t wh = std::max<uint32_t>(64, std::min<uint32_t>(c.want_h.load(std::memory_order_relaxed), 4096));
    DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN, vfmt = DXGI_FORMAT_UNKNOWN;
    if (bb.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || bb.SampleDesc.Count != 1 || bb.DepthOrArraySize != 1 || !formats_for(bb.Format, sfmt, vfmt)) {
        fail("back-buffer-shape-or-format", true);
        return false;
    }
    if (c.staging && c.sw == uint32_t(bb.Width) && c.sh == bb.Height && c.bbfmt == bb.Format && c.ow == ww && c.oh == wh) return true;
    if (c.staging || c.out[0]) {
        if (!c.ring.retire_all(completion(c.fence))) return false;   // a slot is held or a capture is in flight: try again later
        for (unsigned i = 0; i < kRingSlots; ++i) c.out_pub[i].store(nullptr, std::memory_order_release);
        rel(c.staging);
        for (auto& o : c.out) rel(o);
        c.recreated.fetch_add(1, std::memory_order_relaxed);
    }
    acz::ProxyCreationScope scope(false);
    auto create = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)>(c.dev, kDevCreateCommittedResource);
    if (!create) { fail("create-committed", true); return false; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sd{};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; sd.Width = bb.Width; sd.Height = bb.Height; sd.DepthOrArraySize = 1; sd.MipLevels = 1;
    sd.Format = sfmt; sd.SampleDesc.Count = 1; sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(create(c.dev, &hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&c.staging))) || !c.staging) {
        c.staging = nullptr; fail("staging", false); return false;
    }
    D3D12_RESOURCE_DESC od = sd;
    od.Width = ww; od.Height = wh; od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (auto& o : c.out)
        if (FAILED(create(c.dev, &hp, D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&o))) || !o) {
            o = nullptr; fail("slot-image", false); return false;
        }
    auto createSrv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(c.dev, kDevCreateSrv);
    auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(c.dev, kDevCreateRtv);
    if (!createSrv || !createRtv) { fail("view-natives", true); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = vfmt; v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 1;
    createSrv(c.dev, c.staging, &v, c.srv->GetCPUDescriptorHandleForHeapStart());
    D3D12_CPU_DESCRIPTOR_HANDLE h = c.rtv->GetCPUDescriptorHandleForHeapStart();
    for (unsigned i = 0; i < kRingSlots; ++i) {
        D3D12_RENDER_TARGET_VIEW_DESC r{}; r.Format = DXGI_FORMAT_R8G8B8A8_UNORM; r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE hi = h; hi.ptr += SIZE_T(i) * c.rtv_step;
        createRtv(c.dev, c.out[i], &r, hi);
    }
    c.sw = uint32_t(bb.Width); c.sh = bb.Height; c.bbfmt = bb.Format; c.sfmt = sfmt; c.ow = ww; c.oh = wh;
    c.src_w.store(c.sw, std::memory_order_relaxed); c.src_h.store(c.sh, std::memory_order_relaxed); c.src_format.store(uint32_t(bb.Format), std::memory_order_relaxed);
    c.out_w.store(ww, std::memory_order_release); c.out_h.store(wh, std::memory_order_release);
    for (unsigned i = 0; i < kRingSlots; ++i) c.out_pub[i].store(c.out[i], std::memory_order_release);
    return true;
}

// One capture into `slot` (Ring::begin_write done). false = nothing executed (the caller aborts the slot).
bool record_and_submit(int slot, ID3D12Resource* bb, uint64_t& value) noexcept {
    Capture& c = g_cap;
    ID3D12GraphicsCommandList* l = c.list;
    auto allocReset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*)>(c.alloc[slot], kAllocReset);
    auto reset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(l, kListReset);
    auto barrier = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(l, kListBarrier);
    auto copy = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*)>(l, kListCopyResource);
    auto heaps = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(l, kListHeaps);
    auto rootSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(l, kListRoot);
    auto table = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(l, kListTable);
    auto topology = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(l, kListTopology);
    auto viewports = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(l, kListViewports);
    auto scissors = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(l, kListScissors);
    auto targets = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>(l, kListTargets);
    auto draw = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)>(l, kListDraw);
    auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(l, kListClose);
    auto execute = native<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(c.q, kQueueExecute);
    auto signal = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(c.q, kQueueSignal);
    if (!allocReset || !reset || !barrier || !copy || !heaps || !rootSet || !table || !topology || !viewports || !scissors || !targets || !draw ||
        !close || !execute || !signal) { fail("list-natives", true); return false; }
    if (FAILED(allocReset(c.alloc[slot]))) { fail("allocator-reset", false); return false; }
    if (FAILED(reset(l, c.alloc[slot], c.pso))) { fail("list-reset", false); return false; }
    ID3D12Resource* o = c.out[slot];
    {   // the back buffer is in PRESENT (== COMMON) at Present; it is returned to PRESENT before the list ends
        const D3D12_RESOURCE_BARRIER b[2] = {transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                             transition(c.staging, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        barrier(l, 2, b);
    }
    copy(l, c.staging, bb);
    {
        const D3D12_RESOURCE_BARRIER b[3] = {transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
                                             transition(c.staging, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                                             transition(o, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        barrier(l, 3, b);
    }
    ID3D12DescriptorHeap* hs[1] = {c.srv};
    heaps(l, 1, hs);
    rootSet(l, c.root);
    table(l, 0, c.srv->GetGPUDescriptorHandleForHeapStart());
    topology(l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(c.ow), float(c.oh), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, LONG(c.ow), LONG(c.oh)};
    viewports(l, 1, &vp);
    scissors(l, 1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = c.rtv->GetCPUDescriptorHandleForHeapStart(); rh.ptr += SIZE_T(slot) * c.rtv_step;
    targets(l, 1, &rh, FALSE, nullptr);
    draw(l, 3, 1, 0, 0);
    {   // at rest: staging and the slot image in COMMON (the XR consumer copies the slot image on its own queue by implicit promotion)
        const D3D12_RESOURCE_BARRIER b[2] = {transition(c.staging, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                             transition(o, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON)};
        barrier(l, 2, b);
    }
    if (FAILED(close(l))) { fail("list-close", false); return false; }
    ID3D12CommandList* lists[1] = {l};
    execute(c.q, 1, lists);
    // From here the list may run: a failed Signal leaves the slot WRITING for ever (never reused, never read) and disables the capture.
    const uint64_t v = c.value + 1;
    if (FAILED(signal(c.q, c.fence, v))) { fail("signal", true); value = 0; return true; }
    c.value = v;
    value = v;
    return true;
}
} // namespace

void capture_configure(const Options& o) noexcept { g_cap.configured.store(o.on, std::memory_order_release); }
void note_swapchain_queue(const void* swapchain, void* queue, void* device) noexcept { g_assoc.note(swapchain, queue, device); }
void capture_request() noexcept { g_cap.want_until.store(GetTickCount64() + uint64_t(kWantHoldMs), std::memory_order_relaxed); }
void capture_set_extent(uint32_t w, uint32_t h) noexcept {
    if (w && h) { g_cap.want_w.store(w, std::memory_order_relaxed); g_cap.want_h.store(h, std::memory_order_relaxed); }
}

void on_present(void* swapchain) noexcept {
    Capture& c = g_cap;
    if (!swapchain || !c.configured.load(std::memory_order_acquire) || c.disabled.load(std::memory_order_acquire)) return;
    c.presents.fetch_add(1, std::memory_order_relaxed);
    const uint64_t until = c.want_until.load(std::memory_order_relaxed);
    if (!capture_wanted(int64_t(until), int64_t(GetTickCount64()))) return;
    c.wanted.fetch_add(1, std::memory_order_relaxed);
    try {
        if (!ensure_base(swapchain)) return;
        IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swapchain);
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) { fail("no-swapchain3", true); return; }
        const UINT index = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        ID3D12Resource* bb = nullptr;
        if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&bb))) || !bb) { fail("get-buffer", false); return; }
        const D3D12_RESOURCE_DESC d = bb->GetDesc();
        if (!ensure_images(d)) { rel(bb); return; }
        const int slot = c.ring.begin_write(completion(c.fence));
        if (slot < 0) { rel(bb); return; }
        uint64_t value = 0;
        if (!record_and_submit(slot, bb, value)) { c.ring.abort(slot); rel(bb); return; }
        rel(bb);   // the swap chain keeps the buffer; the GPU work is ordered before this frame's Present on the same queue
        if (value) { c.ring.publish(slot, value); c.recorded.fetch_add(1, std::memory_order_relaxed); }
    } catch (...) { fail("exception", true); }
}

bool capture_acquire(uint64_t after, CaptureImage& out) noexcept {
    Capture& c = g_cap;
    ID3D12Fence* f = c.fence_pub.load(std::memory_order_acquire);
    if (!f) return false;
    Acquired a;
    if (c.lost.load(std::memory_order_acquire) || !c.ring.acquire_latest(after, completion(f), a)) return false;
    out = CaptureImage{};
    out.resource = c.out_pub[size_t(a.slot)].load(std::memory_order_acquire);
    out.width = c.out_w.load(std::memory_order_acquire); out.height = c.out_h.load(std::memory_order_acquire);
    out.format = uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM);
    out.held = a;
    if (!out.resource) { c.ring.release(a); return false; }
    return true;
}
void capture_release(const CaptureImage& img) noexcept { g_cap.ring.release(img.held); }

CaptureStats capture_stats() noexcept {
    Capture& c = g_cap;
    CaptureStats s;
    const Ring::Stats r = c.ring.stats();
    s.presents = c.presents.load(); s.wanted = c.wanted.load(); s.recorded = c.recorded.load(); s.failures = c.failures.load(); s.recreated = c.recreated.load();
    s.skipped_busy = r.busy; s.skipped_no_slot = r.no_slot;
    s.src_w = c.src_w.load(); s.src_h = c.src_h.load(); s.src_format = c.src_format.load(); s.dst_w = c.out_w.load(); s.dst_h = c.out_h.load();
    s.disabled = c.disabled.load(); s.last_failure = c.last_failure.load();
    return s;
}
std::string capture_describe() noexcept {
    const CaptureStats s = capture_stats();
    char t[360];
    std::snprintf(t, sizeof t, "cap-presents=%llu;cap-wanted=%llu;cap-recorded=%llu;cap-busy=%llu;cap-no-slot=%llu;cap-failures=%llu;cap-recreated=%llu;"
        "cap-src=%ux%u/fmt%u;cap-dst=%ux%u;cap-disabled=%u;cap-last-failure=%s",
        (unsigned long long)s.presents, (unsigned long long)s.wanted, (unsigned long long)s.recorded, (unsigned long long)s.skipped_busy,
        (unsigned long long)s.skipped_no_slot, (unsigned long long)s.failures, (unsigned long long)s.recreated, s.src_w, s.src_h, s.src_format,
        s.dst_w, s.dst_h, s.disabled ? 1u : 0u, s.last_failure ? s.last_failure : "none");
    return t;
}

} // namespace acz::vscreen
