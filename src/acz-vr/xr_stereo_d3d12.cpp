// J-xr-stereo-output real D3D12 blit: FP16 producer eye image -> sRGB XR
// swapchain image, letterboxed, on a PROXY-OWNED allocator/list submitted with
// the ORIGINAL ExecuteCommandLists of the game's DIRECT queue. Owner thread
// only (every call arrives through the owner's Gate). No game list, no game
// resource other than the producer's published eye images (read-only SRV +
// RENDER_TARGET <-> PIXEL_SHADER_RESOURCE transitions, restored in the same
// recording) is ever touched.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "xr_stereo_d3d12.h"
#include "d3d12_hook.h"
#include "xr_stereo_blit_vs.h"
#include "xr_stereo_blit_ps.h"
#include "xr_stereo_cache_ps.h" // J-world-cache: ps_cache_update (g_xr_cache_ps)
#include "xr_stereo_fill_ps.h"  // J-world-cache: ps_cache_fill (g_xr_fill_ps)
#include "xr_props_d3d12.h"   // WORLD-3 props/trees pass (t11/t12 of the eyes)
#include "effects_stage_flags.h"   // W15 w15-3 F2: withdraw_world_cloud_drop (puff suppression ends when the GPU terrain is dead)
#include "cockpit_model_vs.h"   // COCKPIT-2: vs_model (g_cockpit_vs)
#include "cockpit_model_ps.h"   // COCKPIT-2: ps_model (g_cockpit_ps)
#include "cockpit_model_glass_ps.h"   // COCKPIT-4: ps_glass (g_cockpit_glass_ps), dual-source canopy transmission
#include "xr_tactical_vs.h"   // TACTICAL: vs_tac (g_xr_tac_vs)
#include "xr_tactical_ps.h"   // TACTICAL: ps_tac (g_xr_tac_ps)
#include "xr_stamp_vs.h"      // NBOX (mk360:3): vs_stamp (g_xr_stamp_vs)
#include "xr_stamp_ps.h"      // NBOX (mk360:3): ps_stamp (g_xr_stamp_ps)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace acz::xr_stereo {
namespace {
// Absolute vtable slots (Windows SDK d3d12.h, C++ ABI; the C vtbl lists two
// declarations for struct-returning methods, these indices do not).
enum : int {
    kRelease = 2,
    kDevCreateAllocator = 9, kDevCreateGraphicsPso = 10, kDevCreateList = 12, kDevCreateHeap = 14,
    kDevIncrement = 15, kDevCreateRoot = 16, kDevCreateSrv = 18, kDevCreateRtv = 20, kDevCreateDsv = 21, kDevCreateFence = 36,
    kDevCreateCommittedResource = 27, kDevRemoved = 37, kDevCreateQueryHeap = 39,
    kListClose = 9, kListReset = 10, kListDraw = 12, kListTopology = 20, kListViewports = 21, kListScissors = 22,
    kListCopyBuffer = 15, kListCopyTexture = 16, kListCopyResource = 17, kListClearDepth = 47, kListSetPso = 25, kListBarrier = 26, kListHeaps = 28, kListRoot = 30, kListTable = 32, kListConstants = 36, kListTargets = 46, kListClear = 48,
    kListEndQuery = 53, kListResolveQueryData = 54,
    kQueueGetDevice = 7, kQueueExecute = 10, kQueueSignal = 14, kQueueWait = 15,
    kQueueGetTimestampFrequency = 16, kQueueGetClockCalibration = 17,
    kResourceMap = 8, kResourceUnmap = 9, kResourceGpuVa = 11,
    kListRootSrv = 40,   // TACTICAL: SetGraphicsRootShaderResourceView
    kAllocReset = 8, kFenceCompleted = 8,
};
// SRV table per target group: t0 colour, t1 depth, t2 world, t3 effects, t4 cache colour, t5 cache stamp, t6 cache fill (J-world-cache). One extra group
// (index kTargets) feeds the cache write/fill passes (world at t2, everything else null).
constexpr unsigned kSlots = 15; // + t7 terrain heights, t8 tile words, t9 tile atlas (J-terrain-render; null views when the terrain is off), t10 WORLD-2 environment
                                // + t11 props albedo, t12 props normal+distance (WORLD-3; null views when the props pass did not run)
                                // + t13 the eye's smoke image (EFFECTS-OUTSIDE (C) fxmem:4; null view unless job.use_smoke and readable)
                                // + t14 the eye's sprite image (EFFECTS-OUTSIDE (F) fxmem:6; null view unless job.use_sprite and readable)
constexpr unsigned kUpdateGroup = kTargets;
constexpr unsigned kTerrainConstants = 20; // b1: eye(3) cell | right(3) hstep | up(3) half | fwd(3) sea | haze_l far hmax bits
// COCKPIT-2: the model pass's own table (t0 vertex stream, t1..t4 textures, SKIN-REFL t5 stage reflection map) lives after the blit groups in the same
// shader-visible heap.
constexpr unsigned kModelSlots = 2 + acz::cockpit::kGpuTextures;
constexpr unsigned kModelReflSlot = 1 + acz::cockpit::kGpuTextures;
constexpr unsigned kModelBase = kSlots * (kTargets + 1);
// NBOX (mk360:3): the stamp atlas SRV (t1 of the stamp pass) after the model table in the same heap.
constexpr unsigned kStampSrv = kModelBase + kModelSlots;
uint32_t depth_typeless_format(uint32_t f) noexcept {
    switch (f) { case 20: return 19; case 40: return 39; case 45: return 44; case 55: return 53; default: return 0; }
}
template <class Fn> Fn native(void* object, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(object, slot)); }
void release(IUnknown*& p) noexcept {
    if (!p) return;
    if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(p, kRelease)) fn(p);
    p = nullptr;
}
template <class T> void release(T*& p) noexcept { IUnknown* u = p; release(u); p = nullptr; }

class Blit final : public BlitBackend {
public:
    Blit(ID3D12Device* d, ID3D12CommandQueue* q) : dev_(d), q_(q) {}
    ~Blit() override = default; // deliberately no foreign cleanup (destroy() does it, after completion)

    bool setup() override {
        if (setup_ || !dev_ || !q_) return false;
        setup_ = true; // partial objects are released by destroy()
        // Authenticate the queue against the device and DIRECT type.
        ID3D12Device* owner = nullptr;
        auto getDevice = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, REFIID, void**)>(q_, kQueueGetDevice);
        if (!getDevice || FAILED(getDevice(q_, IID_PPV_ARGS(&owner))) || !owner) return false;
        const bool same = owner == dev_;
        release(owner);
        if (!same || q_->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        auto createFence = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**)>(dev_, kDevCreateFence);
        if (!createFence || FAILED(createFence(dev_, 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || !fence_) return false;
        // Root signature: [0] table t0 (colour) + t1 (depth plane or null) + t2 (world image or null; modes 2/3 only) + t3 (effects image or null; mode 3 only),
        // [1] 22 root constants b0 {use_depth, tonemap, exposure, mode, feather, WorldComposite[17]}, static
        // linear-clamp s0; all pixel visibility.
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = kSlots; range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0; params[1].Constants.RegisterSpace = 0; params[1].Constants.Num32BitValues = kBlitConstantsAll;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // J-terrain-render: b1, 20 constants (zero = off)
        params[2].Constants.ShaderRegister = 1; params[2].Constants.RegisterSpace = 0; params[2].Constants.Num32BitValues = kTerrainConstants;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // DECOUPLE: b2, 15 constants (zero = off). Root signature size 1 + 28 + 20 + 15 = 64 DWORDs (the D3D12 limit).
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister = 2; params[3].Constants.RegisterSpace = 0; params[3].Constants.Num32BitValues = kReprojectConstants;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        static_assert(1 + kBlitConstantsAll + kTerrainConstants + kReprojectConstants <= 64, "root signature over 64 DWORDs");
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 4; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error) error->Release(); // CPU-only blob from d3d12.dll's serializer
        if (FAILED(hr) || !blob) { if (blob) blob->Release(); return false; }
        auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
        hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)) : E_FAIL;
        blob->Release();
        if (FAILED(hr) || !root_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_;
        pd.VS = {g_xr_stereo_vs, sizeof(g_xr_stereo_vs)};
        pd.PS = {g_xr_stereo_ps, sizeof(g_xr_stereo_ps)};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // no blending: opaque
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // encodes linear output once
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&pso_))) || !pso_) return false;
        {   // J-world-cache (optional): the cache write pass (2 render targets) and the fill pass. A failure only disables the cache, never the blit.
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cd = pd, fd = pd;
            cd.PS = {g_xr_cache_ps, sizeof(g_xr_cache_ps)};
            cd.NumRenderTargets = 2; cd.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT; cd.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
            cd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            fd.PS = {g_xr_fill_ps, sizeof(g_xr_fill_ps)};
            fd.NumRenderTargets = 1; fd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            cache_ok_ = SUCCEEDED(createPso(dev_, &cd, IID_PPV_ARGS(&pso_cache_))) && pso_cache_ &&
                SUCCEEDED(createPso(dev_, &fd, IID_PPV_ARGS(&pso_fill_))) && pso_fill_;
        }
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(dev_, kDevIncrement);
        if (!createHeap || !increment) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kStampSrv + 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&srv_))) || !srv_) return false;
        srv_step_ = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_step_ = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_cpu_ = srv_->GetCPUDescriptorHandleForHeapStart();
        srv_gpu_ = srv_->GetGPUDescriptorHandleForHeapStart();
        if (!srv_step_ || !rtv_step_ || !srv_cpu_.ptr || !srv_gpu_.ptr) return false;
        auto createAlloc = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**)>(dev_, kDevCreateAllocator);
        if (!createAlloc || FAILED(createAlloc(dev_, D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_))) || !alloc_) return false;
        auto createList = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
            ID3D12PipelineState*, REFIID, void**)>(dev_, kDevCreateList);
        if (!createList || FAILED(createList(dev_, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_, pso_, IID_PPV_ARGS(&list_))) || !list_) return false;
        // Resolve the list/queue/allocator natives once (vtables are per class;
        // a later patch keeps the original we resolve now).
        close_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(list_, kListClose);
        reset_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(list_, kListReset);
        draw_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)>(list_, kListDraw);
        topology_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(list_, kListTopology);
        viewports_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(list_, kListViewports);
        scissors_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(list_, kListScissors);
        barrier_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(list_, kListBarrier);
        heaps_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(list_, kListHeaps);
        rootSet_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(list_, kListRoot);
        table_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(list_, kListTable);
        constants_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(list_, kListConstants);
        targets_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
            const D3D12_CPU_DESCRIPTOR_HANDLE*)>(list_, kListTargets);
        clear_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)>(list_, kListClear);
        allocReset_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*)>(alloc_, kAllocReset);
        execute_ = native<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(q_, kQueueExecute);
        signal_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(q_, kQueueSignal);
        wait_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(q_, kQueueWait);
        createSrv_ = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
            D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateSrv);
        removed_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*)>(dev_, kDevRemoved);
        if (!close_ || !reset_ || !draw_ || !topology_ || !viewports_ || !scissors_ || !barrier_ || !heaps_ || !rootSet_ ||
            !table_ || !constants_ || !targets_ || !clear_ || !allocReset_ || !execute_ || !signal_ || !wait_ || !createSrv_ || !removed_) return false;
        // Created OPEN: close the initial empty recording.
        if (FAILED(close_(list_))) return false;
        // Optional diagnostics: failure must never change setup/frame admission.
        auto createQuery = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateQueryHeap);
        auto createBuffer = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)>(dev_, kDevCreateCommittedResource);
        // J-world-hold (optional): CopyResource for the held world image; its absence only disables the capture.
        copy_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*)>(list_, kListCopyResource);
        createCommitted_ = createBuffer;
        setPso_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list_, kListSetPso); // J-world-cache (optional)
        if (!setPso_ || !createBuffer) cache_ok_ = false;
        copyTex_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
            const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>(list_, kListCopyTexture); // J-test8-fixes png_once (optional)
        // COCKPIT-2 (optional: their absence only disables the model cockpit)
        clearDepth_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>(list_, kListClearDepth);
        copyBuf_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64)>(list_, kListCopyBuffer);
        endQuery_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT)>(list_, kListEndQuery);
        resolve_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64)>(list_, kListResolveQueryData);
        auto getFrequency = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT64*)>(q_, kQueueGetTimestampFrequency);
        calibrate_ = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT64*, UINT64*)>(q_, kQueueGetClockCalibration);
        D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2 + kPassMarks;   // GPU-COST: + the pass marks (written only with BlitJob::pass_timing)
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 16 + 8 * kPassMarks;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        LARGE_INTEGER frequency{};
        timestamp_ready_ = createQuery && createBuffer && endQuery_ && resolve_ && getFrequency && calibrate_ &&
            QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
            SUCCEEDED(getFrequency(q_, &gpu_hz_)) && gpu_hz_ &&
            SUCCEEDED(createQuery(dev_, &qd, IID_PPV_ARGS(&queries_))) && queries_ &&
            SUCCEEDED(createBuffer(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))) && readback_;
        cpu_hz_ = uint64_t(frequency.QuadPart);
        ready_ = true;
        return true;
    }

    bool bind(unsigned eye, const SwapchainImages& im, uint32_t w, uint32_t h, int64_t format) override {
        if (!ready_ || eye >= kTargets || rtv_[eye] || !im.count || im.count > max_images) return false;
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
            D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        if (!createHeap || !createRtv) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = im.count;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&rtv_[eye]))) || !rtv_[eye]) return false;
        rtv_cpu_[eye] = rtv_[eye]->GetCPUDescriptorHandleForHeapStart();
        if (!rtv_cpu_[eye].ptr) return false;
        for (uint32_t i = 0; i < im.count; ++i) {
            auto* res = static_cast<ID3D12Resource*>(im.images[i]);
            if (!res) return false;
            const D3D12_RESOURCE_DESC rd = res->GetDesc();
            ID3D12Device* owner = nullptr;
            const bool associated = SUCCEEDED(res->GetDevice(IID_PPV_ARGS(&owner))) && owner == dev_;
            release(owner);
            // xr_probe a2 binding rule: exact XR image, this device, 2D, 1 mip/slice/sample,
            // render-target capable, typeless or sRGB storage (never a typed UNORM cast).
            const bool storage = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && (format == rgba8_unorm || format == rgba8_srgb));
            if (!associated || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width != w || rd.Height != h ||
                rd.DepthOrArraySize != 1 || rd.MipLevels != 1 || rd.SampleDesc.Count != 1 ||
                !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || !storage) return false;
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            createRtv(dev_, res, &rv, {rtv_cpu_[eye].ptr + SIZE_T(i) * rtv_step_});
            images_[eye][i] = res;
        }
        count_[eye] = im.count; w_[eye] = w; h_[eye] = h;
        return true;
    }

    RecordFacts record(const BlitJob& job) override {
        RecordFacts f;
        if (!ready_ || !rtv_[0] || !rtv_[1] || open_) return f;
        if (job.use_world && !job.world_composite && !rtv_[kQuad]) return f; // composite: no quad target
        if (job.use_hud && !rtv_[kHud]) return f;
        if (job.use_markers && !rtv_[kMarkers]) return f;
        if (job.use_effects && !(job.use_world && job.world_composite && !job.world_held)) return f; // J-effects-layer: only inside the fresh world composite
        auto check = [&](const SourceEye& e, bool depth) {
            if (!e.resource) return false;
            const D3D12_RESOURCE_DESC rd = static_cast<ID3D12Resource*>(e.resource)->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width != e.width || rd.Height != e.height ||
                rd.SampleDesc.Count != 1) return false;
            if (!depth) return rd.Format == DXGI_FORMAT(e.format);
            // A depth SRV needs a shader-readable resource: the DSV format itself or
            // its typeless family, without DENY_SHADER_RESOURCE.
            return depth_srv_format(e.format) != 0 && !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                (rd.Format == DXGI_FORMAT(e.format) || rd.Format == DXGI_FORMAT(depth_typeless_format(e.format)));
        };
        // Targets written, in order: eyes, world quad (optional), HUD quad (optional, J-hud-consumer).
        unsigned list[kTargets] = {0, 1, 0, 0, 0}, targets = 2;
        if (job.use_world && !job.world_composite) list[targets++] = kQuad;
        if (job.use_hud) list[targets++] = kHud;
        if (job.use_markers) list[targets++] = kMarkers; // J-hud-markers (HUD shader mode, own swapchain)
        for (unsigned k = 0; k < targets; ++k) {
            const unsigned t = list[k];
            if (job.index[t] >= count_[t] || !job.map[t].ok) return f;
            if (job.map[t].dst.x + job.map[t].dst.w > w_[t] || job.map[t].dst.y + job.map[t].dst.h > h_[t]) return f;
        }
        bool use_depth = job.use_depth;
        for (unsigned e = 0; e < 2; ++e) {
            if (!check(job.eyes[e], false)) return f;
            if (use_depth && !check(job.depth[e], true)) use_depth = false; // unreadable depth: opaque fallback
        }
        f.depth_used = use_depth;
        if (job.use_world && !job.world_held && !check(job.world, false)) return f; // held: the backend's own image, no producer resource
        // An unreadable HUD (format/size/dimension mismatch) never fails the frame: its
        // image is cleared to transparent (0,0,0,0), it is not transitioned or sampled,
        // and hud_used=false makes the consumer submit no HUD layer for this frame.
        // J-world-hold: a capture needs a plain 2D, 1-mip, 1-slice, single-sample world image and the optional natives; else the
        // frame is drawn as usual and simply not captured (held image invalid).
        bool capture = false;
        if (job.world_capture && job.use_world && !job.world_held && job.world_composite) {
            const D3D12_RESOURCE_DESC wd = static_cast<ID3D12Resource*>(job.world.resource)->GetDesc();
            capture = copy_ && createCommitted_ && wd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && wd.DepthOrArraySize == 1 &&
                wd.MipLevels == 1 && wd.SampleDesc.Count == 1 && ensure_held(wd);
            if (!capture) held_ready_ = false;
        }
        const bool use_hud = job.use_hud && check(job.hud, false);
        f.hud_used = use_hud;
        const bool use_markers = job.use_markers && check(job.markers, false); // same rule: unreadable -> cleared, no draw, markers_used=false
        f.markers_used = use_markers;
        const bool use_effects = job.use_effects && check(job.effects, false); // J-effects-layer: unreadable -> mode 2 (no effects), effects_used=false
        f.effects_used = use_effects;
        // EFFECTS-OUTSIDE (C) fxmem:4: both eye smoke images readable, inside the fresh world composite with the depth plane bound (else no smoke bit, no t13).
        const bool use_smoke = job.use_smoke && use_depth && job.use_world && job.world_composite && !job.world_held && check(job.smoke[0], false) && check(job.smoke[1], false);
        f.smoke_used = use_smoke;
        // EFFECTS-OUTSIDE (F) fxmem:6: the same rule for both eye sprite images (else no sprite bit, no t14).
        const bool use_sprite = job.use_sprite && use_depth && job.use_world && job.world_composite && !job.world_held && check(job.sprite[0], false) && check(job.sprite[1], false);
        f.sprite_used = use_sprite;
        // J-world-cache: decided before the list opens (resource creation only). cache_read = the eyes sample the persistent cache outside the window;
        // cache_write = the cache is first updated from the job's fresh world. Any refusal (no natives/PSO, bad size or parameters, creation failure)
        // leaves cache_read false: the shader flag stays 0 and the periphery is the extend fill (or transparent), exactly the key-off behaviour.
        bool cache_read = false, cache_write = false;
        if (job.world_cache && job.use_world && job.world_composite && cache_ok_) {
            bool finite = std::isfinite(job.cache_look_yaw) && std::isfinite(job.cache_look_pitch) && std::isfinite(job.cache_now_s) && job.cache_now_s > 0.0f;
            for (float v : job.cache_lens) finite = finite && std::isfinite(v) && v > 0.0f;
            cache_read = finite && ensure_cache(job.cache_w, job.cache_h);
            cache_write = cache_read && job.world_cache_write && !job.world_held;
        }
        f.cache_read = cache_read;
        f.cache_bytes = cache_color_ ? uint64_t(cache_w_) * cache_h_ * 8u : 0u;
        // J-terrain-render: GPU resources (and the one-time upload staging) are created before the list opens; the upload is recorded below.
        bool terrain_use = false;
        if (job.terrain && job.use_world && job.world_composite && job.terrain_stage && job.terrain_atlas) {
            terrain_use = ensure_terrain(job);
            // W15 w15-3 F2: the replacement cloud field cannot be drawn for this generation: the producer must stop collapsing the game's own puffs
            if (!terrain_use && terrain_dead_ && terrain_dead_gen_ == job.terrain_gen) acz::gpu_copy::step5::withdraw_world_cloud_drop();
        }
        f.terrain_used = terrain_use;
        // WORLD-3: props/trees (option inside xr_terrain_stage; only with the terrain). Selection + resources + instance upload before the list opens.
        bool props_use = false;
        props_async_ready_ = false;   // PERF: only this job's answer is reported
        const bool props_asked = terrain_use && job.props && job.props->opt.any();
        if (props_asked) props_use = prepare_props(job);
        f.props_used = props_use;
        if (props_asked && job.props_async && props_) {   // PERF: the async init state as this job saw it
            f.props_ready = props_async_ready_;                       // async_ready() returned true for this job's runtime
            f.props_failed = !props_async_ready_ && props_->async_failed();
            f.props_pending = !props_async_ready_ && !f.props_failed;  // still running (or just started by this job)
            if (f.props_ready || f.props_failed) f.props_init_ms = props_->async_init_ms();
        }
        // COCKPIT-2: model resources/PSOs (and the one-time upload staging) are created before the list opens; any failure = the replayed cockpit for this recording.
        const bool cockpit_use = job.cockpit && job.cockpit_mesh && ensure_cockpit(job);
        f.cockpit_used = cockpit_use;
        const bool zpre_use = cockpit_use && job.cockpit_prepass && zpre_pipeline();   // GPU-COST zpre (a PSO failure = the COCKPIT-2 order)
        if (FAILED(allocReset_(alloc_))) return f; // previous submission completion was CPU-observed
        // TACTICAL: the overlay pipeline + upload buffer (lazy; a failure only means no overlay); the vertices are copied now (the previous
        // recording that read the buffer has completed: CPU-observed above).
        const bool tac_use = job.tac && !job.tac->empty() && ensure_tac() && upload_tac(*job.tac);
        f.tac_used = tac_use;
        // NBOX (mk360:3): the atlas (marker image format, plan size) and the stamp pipeline. A fresh plan's copies need this frame's readable marker image of
        // the plan's size; when they cannot be recorded NOTHING of the stamps runs (no clears: the flat quad keeps the game's boxes; no draws: the live
        // stamps' cells would show older pixels) and stamp_copied=false makes the consumer forget those cells.
        bool stamp_atlas = false, stamp_copy = false, stamp_pso = false;
        if (job.stamps && job.stamps->atlas.ok) {
            const auto& sp = *job.stamps;
            const bool fresh_copies = sp.fresh && !sp.copy.empty();
            const bool src_ok = use_markers && job.markers.width == sp.atlas.img_w && job.markers.height == sp.atlas.img_h;
            const uint32_t fmt = use_markers ? job.markers.format : st_fmt_;
            stamp_atlas = fmt != 0 && copyTex_ && ensure_stamp_atlas(fmt, sp.atlas.w, sp.atlas.h);
            const bool overlay_ok = tac_use || !job.tac || job.tac->empty();   // eye quads exist but the overlay upload failed: no clears either
            stamp_pso = stamp_atlas && overlay_ok && ensure_stamp_pso();
            stamp_copy = stamp_pso && fresh_copies && src_ok;
            if (fresh_copies && !stamp_copy) stamp_atlas = stamp_pso = false;
        }
        const bool stamp_draw = stamp_pso && tac_use;
        f.stamp_used = stamp_draw; f.stamp_copied = stamp_copy;
        if (terrain_upload_recorded_) { release(terrain_upload_); terrain_upload_recorded_ = false; } // the recording that copied from it has completed
        if (ck_upload_recorded_) { release(ck_upload_); ck_upload_recorded_ = false; }                   // COCKPIT-2: same rule
        if (FAILED(reset_(list_, alloc_, pso_))) return f;
        open_ = true;
        calibrated_ = false;
        timing_cache_ = {}; timing_read_ = false; timing_value_ = 0;
        if (timestamp_ready_) endQuery_(list_, queries_, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        pass_rec_ = job.pass_timing && timestamp_ready_; pass_done_ = false;   // GPU-COST pass marks (diagnostic; off = no extra query)
        pass_mark(kPassStart);
        // Fresh SRVs (previous GPU use completed): per target [colour, depth-or-null, world-or-null] (3 slots).
        auto srv = [&](unsigned slot, void* res, uint32_t fmt, bool depth) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT(depth ? depth_srv_format(fmt) : fmt);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1; sd.Texture2D.PlaneSlice = 0; // depth plane
            createSrv_(dev_, static_cast<ID3D12Resource*>(res), &sd, {srv_cpu_.ptr + SIZE_T(slot) * srv_step_});
        };
        auto null_depth = [&](unsigned slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
            createSrv_(dev_, nullptr, &sd, {srv_cpu_.ptr + SIZE_T(slot) * srv_step_}); // null descriptor
        };
        for (unsigned e = 0; e < 2; ++e) {
            srv(kSlots * e, job.eyes[e].resource, job.eyes[e].format, false);
            if (use_depth) srv(kSlots * e + 1, job.depth[e].resource, job.depth[e].format, true); else null_depth(kSlots * e + 1);
            if (job.use_world && job.world_composite && !job.world_held) srv(kSlots * e + 2, job.world.resource, job.world.format, false); // mode 2 samples t2
            else if (job.use_world && job.world_composite && job.world_held && held_ready_) srv(kSlots * e + 2, held_, held_format_, false); // J-world-hold
            else null_depth(kSlots * e + 2); // (a held request without a valid held image samples a null SRV = black)
            if (use_effects) srv(kSlots * e + 3, job.effects.resource, job.effects.format, false); else null_depth(kSlots * e + 3); // J-effects-layer: t3
            if (cache_read) { // J-world-cache: t4 colour, t5 stamp, t6 fill (resting state PIXEL_SHADER_RESOURCE)
                srv(kSlots * e + 4, cache_color_, uint32_t(DXGI_FORMAT_R11G11B10_FLOAT), false);
                srv(kSlots * e + 5, cache_stamp_, uint32_t(DXGI_FORMAT_R32_FLOAT), false);
                srv(kSlots * e + 6, cache_fill_, uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT), false);
            } else { null_depth(kSlots * e + 4); null_depth(kSlots * e + 5); null_depth(kSlots * e + 6); }
            terrain_srvs(kSlots * e + 7, terrain_use); // J-terrain-render: t7 heights, t8 words, t9 atlas (null views when unused)
            if (props_use) { srv(kSlots * e + 11, props_->color(e), uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM), false); srv(kSlots * e + 12, props_->dist(e), uint32_t(DXGI_FORMAT_R32G32B32A32_FLOAT), false); }
            else { null_depth(kSlots * e + 11); null_depth(kSlots * e + 12); }   // WORLD-3 (t11 albedo, t12 normal + distance)
            if (use_smoke) srv(kSlots * e + 13, job.smoke[e].resource, job.smoke[e].format, false); else null_depth(kSlots * e + 13);   // fxmem:4: t13 = THIS eye's smoke
            if (use_sprite) srv(kSlots * e + 14, job.sprite[e].resource, job.sprite[e].format, false); else null_depth(kSlots * e + 14); // fxmem:6: t14 = THIS eye's sprites
        }
        if (job.use_world && !job.world_composite) { srv(kSlots * kQuad, job.world.resource, job.world.format, false); null_depth(kSlots * kQuad + 1); }
        else null_depth(kSlots * kQuad + 1);
        null_depth(kSlots * kQuad + 3); null_depth(kSlots * kHud + 3); null_depth(kSlots * kMarkers + 3); // t3 is read by mode 3 of the eyes only
        null_depth(kSlots * kQuad + 2); null_depth(kSlots * kHud + 1); null_depth(kSlots * kHud + 2);
        if (use_hud) srv(kSlots * kHud, job.hud.resource, job.hud.format, false);
        null_depth(kSlots * kMarkers + 1); null_depth(kSlots * kMarkers + 2); // J-hud-markers: colour only (HUD shader mode)
        if (use_markers) srv(kSlots * kMarkers, job.markers.resource, job.markers.format, false);
        for (unsigned t : {unsigned(kQuad), unsigned(kHud), unsigned(kMarkers)}) for (unsigned i = 4; i < kSlots; ++i) null_depth(kSlots * t + i); // no cache outside the eyes
        if (stamp_pso) srv(kStampSrv, st_atlas_, st_fmt_, false); // NBOX: the stamp atlas (t1 of the stamp pass; read only by draw_stamps)
        if (cache_write) { // J-world-cache write/fill passes: the fresh world at t2, everything else null
            for (unsigned i = 0; i < kSlots; ++i) if (i != 2) null_depth(kSlots * kUpdateGroup + i);
            srv(kSlots * kUpdateGroup + 2, job.world.resource, job.world.format, false);
        }
        if (props_use) {   // WORLD-3: the props pass runs first (own root signature/PSO/heap); the composite state is set again right below
            PropsFacts pf{};
            props_->record(list_, props_frame_, pf);
            f.props_uploaded = pf.uploaded; f.props_instances = pf.instances; f.props_batches = pf.batches; f.props_draws = pf.draws;
            setPso_(list_, pso_);
        }
        pass_mark(kPassProps);
        root_reset();   // J-terrain-render: root parameter 2 (b1) is always set; zero = terrain off (no terrain code runs in the shader); DECOUPLE b2 likewise
        ID3D12DescriptorHeap* heaps[] = {srv_};
        heaps_(list_, 1, heaps);
        if (terrain_use && terrain_upload_pending_) record_terrain_upload(f);
        if (cockpit_use && ck_upload_pending_) record_cockpit_upload(f);   // COCKPIT-2
        if (cockpit_use && job.cockpit_anim && job.cockpit_anim_count) record_cockpit_anim(job, f);   // COCKPIT-4: this frame's posed vertices
        if (cockpit_use && job.cockpit_refl) record_cockpit_refl(job, f);   // SKIN-REFL: the stage map when its key changed
        topology_(list_, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Contract: colour/world in RENDER_TARGET, depth in DEPTH_WRITE at publish.
        D3D12_RESOURCE_BARRIER out[12]{}, back[12]{};   // eyes 2 + depth 2 + world + hud + markers + effects + smoke 2 (fxmem:4) + sprite 2 (fxmem:6)
        unsigned n = 0;
        auto transition = [&](void* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            out[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            out[n].Transition.pResource = static_cast<ID3D12Resource*>(res);
            out[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            out[n].Transition.StateBefore = before;
            out[n].Transition.StateAfter = after;
            back[n] = out[n];
            std::swap(back[n].Transition.StateBefore, back[n].Transition.StateAfter);
            ++n;
        };
        for (unsigned e = 0; e < 2; ++e) transition(job.eyes[e].resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (use_depth) for (unsigned e = 0; e < 2; ++e) transition(job.depth[e].resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        // J-world-hold: a captured world is also a copy source in the same read state (PIXEL_SHADER_RESOURCE | COPY_SOURCE); the held image is
        // not a producer resource (never in out[]/back[]) and stays in PIXEL_SHADER_RESOURCE between recordings.
        if (job.use_world && !job.world_held)
            transition(job.world.resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                capture ? D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE) : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (use_hud) transition(job.hud.resource, D3D12_RESOURCE_STATE_RENDER_TARGET); // contract: RENDER_TARGET at publish/release
        // NBOX: the marker image is also the stamps' copy source (PIXEL_SHADER_RESOURCE | COPY_SOURCE, like the J-world-hold capture of the world)
        if (use_markers) transition(job.markers.resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
            stamp_copy ? D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE) : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (use_effects) transition(job.effects.resource, D3D12_RESOURCE_STATE_RENDER_TARGET); // J-effects-layer: back to RENDER_TARGET in the same recording
        if (use_smoke) for (unsigned e = 0; e < 2; ++e) transition(job.smoke[e].resource, D3D12_RESOURCE_STATE_RENDER_TARGET); // fxmem:4: same contract
        if (use_sprite) for (unsigned e = 0; e < 2; ++e) transition(job.sprite[e].resource, D3D12_RESOURCE_STATE_RENDER_TARGET); // fxmem:6: same contract
        barrier_(list_, n, out); f.barriers_out = n;
        if (stamp_copy) record_stamp_copies(job, f);   // NBOX: this frame's stamps into the atlas, before any eye samples it
        if (cache_read && (cache_write || cache_needs_clear_ || job.world_cache_clear)) record_cache_update(job, cache_write, f);
            auto set_constants = [&](unsigned t, uint32_t dbg) {
            float exposure = job.exposure;
                // b0.w = mode: 0 = cockpit/world (unchanged), 1 = HUD (premultiplied source,
                // alpha = saturate(max(a, max(rgb))) after the same exposure/tonemap).
                // b0[4] = feather (J-feather): 0 = OFF (shader returns its output untouched);
                // > 0 ramps every target's premultiplied output toward its border.
                const float feather = clamp_feather(job.edge_feather);
                // b0[3] = mode: 2 = eye composite over the world plane (J-world-in-projection, eyes only); b0[5..21] = its
                // WorldComposite (zero otherwise: the shader never reads them in modes 0/1, the parameter is still fully set).
                const bool composite_eye = t < 2 && job.use_world && job.world_composite;
                uint32_t constants[kBlitConstantsAll] = {t < 2 && use_depth ? 1u : 0u, uint32_t(job.tonemap), 0u,
                    pack_mode_bits(composite_eye ? (use_effects ? (job.effects_coverage ? 4u : 3u) : 2u) : (t == kHud || t == kMarkers ? 1u : 0u), t < 2 ? dbg : 0u) |
                        (composite_eye && use_depth && job.occlude_window ? kModeBitOcclude : 0u) |
                        (t < 2 && cockpit_use && !job.cockpit_keep_replay ? kModeBitModelCockpit : 0u) |
                        (t == kHud ? (job.hud_backdrop_bits & 0xFFF80000u) : 0u) |   // COCKPIT-2: drop the replayed cockpit band; HUD-MENU: backdrop floor; RADAR-BG 19..23
                        (composite_eye && use_smoke ? kModeBitSmoke : 0u) |          // EFFECTS-OUTSIDE (C) fxmem:4: the eye smoke at t13 over the objects
                        (composite_eye && use_sprite ? kModeBitSprite : 0u) |        // EFFECTS-OUTSIDE (F) fxmem:6: the eye sprites at t14 after the smoke
                        (composite_eye && job.world_filter ? kModeBitWorldFilter : 0u) | // WORLD-WINDOW FILTER (eyeres > 0): footprint grid on the window texel
                        (t < 2 && dbg == 0u ? job.pov_vignette_bits : 0u), 0u};      // POV-RENDER pov:3: vignette / blink (bits 20, 24..31; eyes, final image only)
                std::memcpy(&constants[2], &exposure, sizeof exposure);
                std::memcpy(&constants[4], &feather, sizeof feather);
                if (composite_eye) {
                    std::memcpy(&constants[5], &job.world_eye[t], sizeof(WorldComposite));
                    // J-world-hold / J-world-extend: b0[22] = fade (1 = off), b0[23] = extend flag (0 = off). Other targets keep the shader's off values.
                    const float fade = std::isfinite(job.world_fade) ? (job.world_fade < 0.0f ? 0.0f : job.world_fade > 1.0f ? 1.0f : job.world_fade) : 0.0f;
                    const float extend = job.world_extend ? 1.0f : 0.0f;
                    std::memcpy(&constants[22], &fade, sizeof fade);
                    std::memcpy(&constants[23], &extend, sizeof extend);
                    if (cache_read) { // J-world-cache: b0[24..27] = flag, rendered look yaw/pitch, stamp clock (s). Key off / no cache: stays 0.
                        const float flag = 1.0f;
                        std::memcpy(&constants[24], &flag, sizeof flag);
                        std::memcpy(&constants[25], &job.cache_look_yaw, sizeof(float));
                        std::memcpy(&constants[26], &job.cache_look_pitch, sizeof(float));
                        std::memcpy(&constants[27], &job.cache_now_s, sizeof(float));
                    }
                } else {
                    const float one = 1.0f;
                    std::memcpy(&constants[22], &one, sizeof one);
                }
                constants_(list_, 1, kBlitConstantsAll, constants, 0);
                {   // DECOUPLE b2: this eye's re-projection (composite eyes only; zero = off for every other target and every key-off job)
                    uint32_t dc[kReprojectConstants] = {};
                    if (composite_eye && job.reproject[t].flags) std::memcpy(dc, &job.reproject[t], sizeof dc);
                    constants_(list_, 3, kReprojectConstants, dc, 0);
                }
                if (terrain_use) {   // J-terrain-render b1: this eye's camera (stage space) + stage scalars; zero for every other target
                    uint32_t tc[kTerrainConstants] = {};
                    if (composite_eye) {
                        const TerrainEye& te = job.terrain_eye[t];
                        const acz::terrain::StageData& sd = *job.terrain_stage;
                        float v[kTerrainConstants - 1];
                        for (int i = 0; i < 3; ++i) { v[i] = te.eye[i]; v[4 + i] = te.right[i]; v[8 + i] = te.up[i]; v[12 + i] = te.fwd[i]; }
                        v[3] = sd.cell; v[7] = sd.hstep; v[11] = sd.half; v[15] = sd.sea_present() ? sd.sea_level : 0.0f;
                        v[16] = job.terrain_haze_l; v[17] = job.terrain_far; v[18] = float(sd.hmax_byte) * sd.hstep;
                        std::memcpy(tc, v, sizeof v);
                        tc[19] = 1u | (job.terrain_blend ? 2u : 0u) | (sd.sea_present() ? 4u : 0u) | (job.terrain_occlude && job.use_depth ? 8u : 0u) |   // bit3: WORLD-1 object occlusion
                                 (terrain_e_ ? (job.terrain_env_bits & acz::terrain::kEnvAll) << acz::terrain::kEnvBitShift : 0u) |                       // bits 4..7: WORLD-2 environment
                                 (props_use ? 256u : 0u) |                                                                                                   // bit 8: WORLD-3 props (TP_PROPS)
                                 ((job.terrain_quality > 2u ? 2u : job.terrain_quality) << 9) |                                                           // bits 9..10: GPU-COST q
                                 (job.terrain_obj_display && job.use_depth ? 2048u : 0u) |                                                                 // bit 11: PROPS-SHADING objdom:display
                                 (props_use && job.props->opt.shade_game ? 4096u : 0u) |                                                                  // bit 12: PROPS-SHADING propshade:game
                                 (sd.sea_present() && sd.sea_masked ? 8192u : 0u) |                                                                       // bit 13: D-344 C1 sea tile mask
                                 (terrain_detail_ ? 16384u : 0u);                                                                                         // bit 14: DTEX detail slices
                    }
                    constants_(list_, 2, kTerrainConstants, tc, 0);
                }
            };
        for (unsigned k = 0; k < targets; ++k) {
            const unsigned t = list[k];
            // A successful xrWaitSwapchainImage establishes RENDER_TARGET for the
            // image; it is written as a render target and returned unchanged.
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_cpu_[t].ptr + SIZE_T(job.index[t]) * rtv_step_};
            targets_(list_, 1, &rtv, FALSE, nullptr);
            // Cockpit and HUD: transparent (premultiplied) where uncovered; quad: opaque black.
            static const FLOAT clear_cockpit[4] = {0.0f, 0.0f, 0.0f, 0.0f}, clear_quad[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (t == kHud && !use_hud) { clear_(list_, rtv, clear_cockpit, 0, nullptr); ++f.clears; continue; } // refused HUD: transparent, no draw
            if (t == kMarkers && !use_markers) { clear_(list_, rtv, clear_cockpit, 0, nullptr); ++f.clears; continue; } // refused markers: transparent, no draw
            // Full viewport/scissor + oversized triangle, no depth/blend/discard,
            // RGBA write mask ALL: ps_main writes every pixel including alpha=0.
            // Thus only a partial rect needs the outside pixels initialized.
            if (needs_clear(job.map[t], w_[t], h_[t])) {
                clear_(list_, rtv, t == kQuad ? clear_quad : clear_cockpit, 0, nullptr); ++f.clears;
            }
            const auto& r = job.map[t].dst;
            const D3D12_VIEWPORT vp{FLOAT(r.x), FLOAT(r.y), FLOAT(r.w), FLOAT(r.h), 0.0f, 1.0f};
            const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
            viewports_(list_, 1, &vp);
            scissors_(list_, 1, &sc);
            const bool model_eye = t < 2 && cockpit_use && job.debug_view == 0;
            const bool zpre = model_eye && zpre_use;
            if (zpre) {   // GPU-COST zpre: the opaque model first (colour + depth); the composite then shades only the pixels it left uncovered
                draw_model(job, t, rtv, f, false, 1);
                setPso_(list_, pso_zt_);
                targets_(list_, 1, &rtv, FALSE, &ck_dsv_);
            }
            set_constants(t, job.debug_view);
            table_(list_, 0, {srv_gpu_.ptr + UINT64(kSlots * t) * srv_step_});
            if (t < 2) pass_mark(kPassEyePre(t));
            draw_(list_, 3, 1, 0, 0); ++f.draws;
            if (t == kMarkers && stamp_copy) clear_stamp_rects(job, rtv, f);   // NBOX: the stamped boxes leave the flat quad (drawn per eye instead)
            if (t < 2) pass_mark(kPassEyeComposite(t));
            if (tac_use && t < 2) draw_tac(rtv, *job.tac, t == 0 ? acz::tac::slot_eye0_pre : acz::tac::slot_eye1_pre, f);   // TACTICAL tracers (under the model)
            if (zpre) { draw_model(job, t, rtv, f, true, 2); f.cockpit_prepass = true; }   // blend + glass over the composite (same depth)
            else if (model_eye) draw_model(job, t, rtv, f, true);   // COCKPIT-2: the model over this eye's composite
            else if (t < 2) { pass_mark(kPassEyeModel(t)); pass_mark(kPassEyeGlass(t)); }
            if (tac_use && t < 2) draw_tac(rtv, *job.tac, t == 0 ? acz::tac::slot_eye0_post : acz::tac::slot_eye1_post, f);   // TACTICAL markers (over all)
            if (stamp_draw && t < 2) draw_stamps(rtv, *job.tac, t == 0 ? acz::tac::slot_eye0_stamp : acz::tac::slot_eye1_stamp, job, f);   // NBOX: the game's boxes
            if (tac_use && t == kHud) draw_tac(rtv, *job.tac, acz::tac::slot_hud, f);   // TACTICAL radar blips in the HUD image
        }
        // J-test8-fixes xr_png_once: render eye 0 three more times into a PRIVATE sRGB target (final composite as configured, cockpit coverage, world layer
        // with effects) and copy each to a readback buffer. Private objects only; the swapchain images are not touched. A failure only skips the capture.
        if (job.png_capture && png_prepare(w_[0], h_[0])) {
            const auto& r = job.map[0].dst;
            const D3D12_VIEWPORT vp{FLOAT(r.x), FLOAT(r.y), FLOAT(r.w), FLOAT(r.h), 0.0f, 1.0f};
            const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
            static const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const uint32_t views[3] = {job.debug_view, 1u, 3u};
            D3D12_RESOURCE_BARRIER pb{};
            pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; pb.Transition.pResource = png_rt_;
            pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            for (unsigned i = 0; i < 3; ++i) {
                targets_(list_, 1, &png_rtv_, FALSE, nullptr);
                clear_(list_, png_rtv_, zero, 0, nullptr);
                viewports_(list_, 1, &vp);
                scissors_(list_, 1, &sc);
                set_constants(0, views[i]);
                table_(list_, 0, {srv_gpu_.ptr});
                draw_(list_, 3, 1, 0, 0); ++f.draws;
                if (i == 0 && cockpit_use && views[0] == 0) draw_model(job, 0, png_rtv_, f, false);   // COCKPIT-2: the final image includes the model
                if (i == 0 && tac_use) draw_tac(png_rtv_, *job.tac, acz::tac::slot_eye0_post, f);   // LAYER-DUMP: the final image also holds the per-eye markers
                if (i == 0 && stamp_draw) draw_stamps(png_rtv_, *job.tac, acz::tac::slot_eye0_stamp, job, f);   // ... and the NBOX stamps (boxes / diamond outside the window)
                pb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                barrier_(list_, 1, &pb);
                D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
                dst.pResource = png_rb_; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = UINT64(i) * png_stride_;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                dst.PlacedFootprint.Footprint.Width = png_w_; dst.PlacedFootprint.Footprint.Height = png_h_;
                dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = UINT(png_pitch_);
                from.pResource = png_rt_; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
                const D3D12_BOX box{0, 0, 0, png_w_, png_h_, 1};
                copyTex_(list_, &dst, 0, 0, 0, &from, &box);
                std::swap(pb.Transition.StateBefore, pb.Transition.StateAfter);
                barrier_(list_, 1, &pb);
            }
            png_pending_ = true; f.png_recorded = true;
        }
        // LAYER-DUMP (runLOCK2): the eye dump cannot show the HUD or the marker quad layers (the compositor adds them), so the same capture renders the HUD layer
        // image and the marker layer image once more into a private target of the HUD layer's size (the marker image is skipped when its size differs) and reads
        // them back with the eye images. Unlike the swapchain marker image the NBOX stamp rectangles are NOT cut out: every game box / diamond of the frame shows.
        png2_mask_ = 0;
        if (job.png_capture && png_pending_ && (use_hud || use_markers) && png2_prepare(w_[kHud], h_[kHud])) {
            static const FLOAT zero2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            D3D12_RESOURCE_BARRIER pb{};
            pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; pb.Transition.pResource = png2_rt_;
            pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            for (unsigned li = 0; li < 2; ++li) {
                const unsigned t = li == 0 ? unsigned(kHud) : unsigned(kMarkers);
                if (li == 0 ? !use_hud : (!use_markers || w_[kMarkers] != png2_w_ || h_[kMarkers] != png2_h_)) continue;
                const auto& r = job.map[t].dst;
                const D3D12_VIEWPORT vp{FLOAT(r.x), FLOAT(r.y), FLOAT(r.w), FLOAT(r.h), 0.0f, 1.0f};
                const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
                targets_(list_, 1, &png2_rtv_, FALSE, nullptr);
                clear_(list_, png2_rtv_, zero2, 0, nullptr);
                viewports_(list_, 1, &vp);
                scissors_(list_, 1, &sc);
                set_constants(t, job.debug_view);
                table_(list_, 0, {srv_gpu_.ptr + UINT64(kSlots * t) * srv_step_});
                draw_(list_, 3, 1, 0, 0); ++f.draws;
                if (tac_use && t == kHud) draw_tac(png2_rtv_, *job.tac, acz::tac::slot_hud, f);   // the radar blips, as in the HUD layer
                pb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                barrier_(list_, 1, &pb);
                D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
                dst.pResource = png2_rb_; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = UINT64(li) * png2_stride_;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                dst.PlacedFootprint.Footprint.Width = png2_w_; dst.PlacedFootprint.Footprint.Height = png2_h_;
                dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = UINT(png2_pitch_);
                from.pResource = png2_rt_; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
                const D3D12_BOX box{0, 0, 0, png2_w_, png2_h_, 1};
                copyTex_(list_, &dst, 0, 0, 0, &from, &box);
                std::swap(pb.Transition.StateBefore, pb.Transition.StateAfter);
                barrier_(list_, 1, &pb);
                png2_mask_ |= 1u << li;
            }
        }
        if (capture) {
            // Copy the (shader-readable, copy-source) world into the held image; the held image goes PIXEL_SHADER_RESOURCE -> COPY_DEST -> back.
            D3D12_RESOURCE_BARRIER hb{};
            hb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; hb.Transition.pResource = held_;
            hb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            hb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; hb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier_(list_, 1, &hb);
            copy_(list_, held_, static_cast<ID3D12Resource*>(job.world.resource));
            std::swap(hb.Transition.StateBefore, hb.Transition.StateAfter);
            barrier_(list_, 1, &hb);
            held_ready_ = true; f.world_captured = true;
        }
        f.held_used = job.world_held && held_ready_;
        barrier_(list_, n, back); f.barriers_back = n;
        if (timestamp_ready_) {
            endQuery_(list_, queries_, D3D12_QUERY_TYPE_TIMESTAMP, 1);
            resolve_(list_, queries_, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback_, 0);
            if (pass_rec_) { pass_mark(kPassEnd); resolve_(list_, queries_, D3D12_QUERY_TYPE_TIMESTAMP, 2, kPassMarks, readback_, 16); pass_done_ = true; }
        }
        f.ok = true;
        bool restored = f.barriers_back == f.barriers_out;
        for (unsigned k = 0; k < n; ++k) restored = restored && back[k].Transition.StateAfter == out[k].Transition.StateBefore;
        f.sources_restored = restored;
        f.swapchain_untouched = true; // no barrier names a swapchain image
        return f;
    }
    bool close() override {
        if (!open_) return false;
        open_ = false;
        return SUCCEEDED(close_(list_));
    }
    bool fence_completed(void* fence, uint64_t value) override {
        if (!fence) return false;
        auto completed = native<UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*)>(fence, kFenceCompleted);
        if (!completed) return false;
        const UINT64 v = completed(static_cast<ID3D12Fence*>(fence));
        return v != (std::numeric_limits<UINT64>::max)() && v >= value;
    }
    bool gpu_wait(void* fence, uint64_t value) override {
        return fence && SUCCEEDED(wait_(q_, static_cast<ID3D12Fence*>(fence), value));
    }
    bool execute(bool& dispatch) override {
        dispatch = false;
        if (!ready_ || open_) return false;
        ID3D12CommandList* lists[] = {list_};
        if (timestamp_ready_) {
            LARGE_INTEGER cpu{};
            calibrated_ = SUCCEEDED(calibrate_(q_, &calibration_gpu_, &calibration_cpu_)) && QueryPerformanceCounter(&cpu);
            execute_cpu_ = uint64_t(cpu.QuadPart);
        }
        dispatch = true; // set BEFORE crossing the native boundary
        execute_(q_, 1, lists); // ORIGINAL: the proxy's Execute hook never sees this list
        return SUCCEEDED(removed_(dev_));
    }
    bool signal(uint64_t value) override {
        const bool ok = fence_ && SUCCEEDED(signal_(q_, fence_, value));
        if (ok) timing_value_ = value;
        return ok;
    }
    bool sample(uint64_t& done) override {
        done = 0;
        if (!fence_) return false;
        auto completed = native<UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*)>(fence_, kFenceCompleted);
        if (!completed) return false;
        done = completed(fence_);
        const bool valid = SUCCEEDED(removed_(dev_)) && done != (std::numeric_limits<uint64_t>::max)();
        if (valid && timing_value_ && done >= timing_value_ && !timing_read_) {
            timing_cache_ = read_timing(); timing_read_ = true;
        }
        return valid;
    }
    GpuTiming timing() override { return timing_cache_; }
    // GPU-COST: one timestamp (index 2 + mark of the shared heap) while a pass_timing recording is open.
    void pass_mark(unsigned mark) { if (pass_rec_ && mark < kPassMarks) endQuery_(list_, queries_, D3D12_QUERY_TYPE_TIMESTAMP, 2 + mark); }
    bool pass_timing(PassTiming& out) override {
        out = PassTiming{};
        uint64_t done = 0;
        if (!pass_done_ || !fence_ || !timing_value_ || !sample(done) || done < timing_value_) return false;   // only a completed recording
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(readback_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(readback_, kResourceUnmap);
        if (!map || !unmap) return false;
        void* data = nullptr;
        const D3D12_RANGE range{16, 16 + 8 * kPassMarks};
        if (FAILED(map(readback_, 0, &range, &data)) || !data) return false;
        std::memcpy(out.ticks, static_cast<const uint8_t*>(data) + 16, sizeof out.ticks);
        const D3D12_RANGE written{0, 0}; unmap(readback_, 0, &written);
        out.hz = gpu_hz_; out.available = gpu_hz_ != 0;
        return out.available;
    }
    GpuTiming read_timing() {
        // sample() calls only after the private fence proves resolve completion.
        if (!timestamp_ready_ || !calibrated_) return {};
        // Resource Map/Unmap slots may carry L3 observation hooks. Private
        // readback must use saved natives so it cannot pollute producer evidence.
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(readback_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(readback_, kResourceUnmap);
        if (!map || !unmap) return {};
        void* data = nullptr;
        const D3D12_RANGE range{0, 16};
        if (FAILED(map(readback_, 0, &range, &data)) || !data) return {};
        uint64_t ticks[2]{}; std::memcpy(ticks, data, sizeof ticks);
        const D3D12_RANGE written{0, 0}; unmap(readback_, 0, &written);
        return timestamp_timing(ticks[0], ticks[1], calibration_gpu_, calibration_cpu_, execute_cpu_, gpu_hz_, cpu_hz_);
    }
    bool png_take(PngCapture& out) override {
        if (!png_pending_ || !png_rb_) return false;
        png_pending_ = false;
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(png_rb_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(png_rb_, kResourceUnmap);
        bool ok = false;
        void* data = nullptr;
        const D3D12_RANGE range{0, SIZE_T(png_stride_ * 3)};
        if (map && unmap && SUCCEEDED(map(png_rb_, 0, &range, &data)) && data) {
            try {
                std::vector<uint8_t>* dst[3] = {&out.final_rgba, &out.alpha_rgba, &out.worldfx_rgba};
                for (unsigned i = 0; i < 3; ++i) {
                    dst[i]->assign(size_t(png_w_) * png_h_ * 4, 0);
                    const uint8_t* base = static_cast<const uint8_t*>(data) + UINT64(i) * png_stride_;
                    for (uint32_t y = 0; y < png_h_; ++y) std::memcpy(dst[i]->data() + size_t(y) * png_w_ * 4, base + UINT64(y) * png_pitch_, size_t(png_w_) * 4);
                }
                out.w = png_w_; out.h = png_h_; ok = true;
                png2_read(out);   // LAYER-DUMP: the HUD / marker layer images (none recorded: the vectors stay empty)
            } catch (...) { ok = false; }
            const D3D12_RANGE none{0, 0}; unmap(png_rb_, 0, &none);
        }
        release_png();
        return ok;
    }
    void release_png() { release(png_rt_); release(png_heap_); release(png_rb_); png_pending_ = false; png_w_ = png_h_ = 0;
        release(png2_rt_); release(png2_heap_); release(png2_rb_); png2_w_ = png2_h_ = 0; png2_mask_ = 0; }
    // LAYER-DUMP: copy the recorded layer images out of the second readback (png_take, the frame is complete).
    void png2_read(PngCapture& out) {
        if (!png2_mask_ || !png2_rb_) return;
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(png2_rb_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(png2_rb_, kResourceUnmap);
        void* data = nullptr;
        const D3D12_RANGE range{0, SIZE_T(png2_stride_ * 2)};
        if (!map || !unmap || FAILED(map(png2_rb_, 0, &range, &data)) || !data) return;
        std::vector<uint8_t>* dst[2] = {&out.hud_rgba, &out.markers_rgba};
        for (unsigned i = 0; i < 2; ++i) {
            if (!(png2_mask_ & (1u << i))) continue;
            dst[i]->assign(size_t(png2_w_) * png2_h_ * 4, 0);
            const uint8_t* base = static_cast<const uint8_t*>(data) + UINT64(i) * png2_stride_;
            for (uint32_t y = 0; y < png2_h_; ++y) std::memcpy(dst[i]->data() + size_t(y) * png2_w_ * 4, base + UINT64(y) * png2_pitch_, size_t(png2_w_) * 4);
        }
        out.lw = png2_w_; out.lh = png2_h_;
        const D3D12_RANGE none{0, 0}; unmap(png2_rb_, 0, &none);
    }
    bool destroy() override {
        // Caller (Output::retire) proved every submission complete first.
        release_png();
        release_terrain(); // J-terrain-render
        if (props_) { props_->release(); props_.reset(); } // WORLD-3
        release_cockpit(); release(ck_pso_opaque_); release(ck_pso_blend_); release(ck_pso_glass_); release(ck_root_); // COCKPIT-2 (+COCKPIT-4 glass PSO)
        release(pso_zt_); zt_dead_ = false;   // GPU-COST zpre
        release(tac_up_); tac_ptr_ = nullptr; release(tac_pso_); release(tac_root_); tac_dead_ = false;   // TACTICAL
        release(st_atlas_); release(st_pso_); release(st_root_); st_dead_ = false; st_fmt_ = st_w_ = st_h_ = 0; st_bad_fmt_ = st_bad_w_ = st_bad_h_ = 0;   // NBOX
        release(queries_); release(readback_); release(held_);
        release_cache(); release(pso_cache_); release(pso_fill_); // J-world-cache
        release(list_); release(alloc_);
        for (auto*& r : rtv_) release(r);
        release(srv_);
        release(pso_); release(root_); release(fence_);
        ready_ = false;
        return true;
    }

private:
    // J-world-cache. The persistent environment cache (proxy-owned DEFAULT-heap textures, resting state PIXEL_SHADER_RESOURCE between recordings):
    // colour R11G11B10_FLOAT w x h, stamp R32_FLOAT w x h (8 bytes per texel, budget kWorldCacheBudgetBytes) and the 4x1 R16G16B16A16_FLOAT fill. Recreated
    // (and so cleared) when the requested size changes; the previous recording's GPU use has completed (completion is CPU-observed per frame).
    bool ensure_cache(uint32_t w, uint32_t h) {
        if (cache_color_ && cache_w_ == w && cache_h_ == h) return true;
        if (cache_dead_w_ == w && cache_dead_h_ == h) return false; // creation already failed for this size: do not retry every frame
        release_cache();
        if (!cache_ok_ || !createCommitted_ || w < 16 || h < 8 || w > 8192 || h > 4096 || uint64_t(w) * h * 8u > kWorldCacheBudgetBytes) { cache_dead_w_ = w; cache_dead_h_ = h; return false; }
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        auto make = [&](UINT tw, UINT th, DXGI_FORMAT fmt, ID3D12Resource*& out) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{}; cv.Format = fmt; // all-zero colour: the value ClearRenderTargetView uses below (no debug-layer clear-value mismatch)
            out = nullptr;
            return SUCCEEDED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&out))) && out;
        };
        bool ok = createHeap && createRtv;
        if (ok) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
            ok = SUCCEEDED(createHeap(dev_, &hd, IID_PPV_ARGS(&cache_rtv_heap_))) && cache_rtv_heap_;
        }
        ok = ok && make(w, h, DXGI_FORMAT_R11G11B10_FLOAT, cache_color_) && make(w, h, DXGI_FORMAT_R32_FLOAT, cache_stamp_) &&
            make(4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, cache_fill_);
        if (!ok) { release_cache(); cache_dead_w_ = w; cache_dead_h_ = h; return false; }
        cache_rtv_ = cache_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        ID3D12Resource* const res[3] = {cache_color_, cache_stamp_, cache_fill_};
        const DXGI_FORMAT fmt[3] = {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (unsigned i = 0; i < 3; ++i) {
            D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = fmt[i]; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            createRtv(dev_, res[i], &rv, {cache_rtv_.ptr + SIZE_T(i) * rtv_step_});
        }
        cache_w_ = w; cache_h_ = h; cache_needs_clear_ = true;
        return true;
    }
    void release_cache() {
        release(cache_color_); release(cache_stamp_); release(cache_fill_); release(cache_rtv_heap_);
        cache_w_ = cache_h_ = 0; cache_needs_clear_ = false; cache_rtv_ = {};
    }
    // The cache passes of one recording, after the sources reached PIXEL_SHADER_RESOURCE: [clear] [write: ps_cache_update over the window's bounding
    // rectangle + ps_cache_fill], all inside PIXEL_SHADER_RESOURCE -> RENDER_TARGET -> PIXEL_SHADER_RESOURCE brackets of the three cache textures.
    void record_cache_update(const BlitJob& job, bool write, RecordFacts& f) {
        ID3D12Resource* const res[3] = {cache_color_, cache_stamp_, cache_fill_};
        D3D12_RESOURCE_BARRIER cb[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            cb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; cb[i].Transition.pResource = res[i];
            cb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cb[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; cb[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        barrier_(list_, 3, cb);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_color = cache_rtv_, rtv_stamp{cache_rtv_.ptr + SIZE_T(rtv_step_)}, rtv_fill{cache_rtv_.ptr + SIZE_T(2) * rtv_step_};
        if (cache_needs_clear_ || job.world_cache_clear) {
            static const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            clear_(list_, rtv_color, zero, 0, nullptr); clear_(list_, rtv_stamp, zero, 0, nullptr); clear_(list_, rtv_fill, zero, 0, nullptr);
            f.clears += 3; cache_needs_clear_ = false;
        }
        if (write) {
            // Constants: the extent in the unused slots 0/1, the window lens through the WorldComposite fields, the look and the stamp clock. Slots 22/23 = fade 1 / extend 0.
            uint32_t c[kBlitConstantsAll] = {};
            c[0] = cache_w_; c[1] = cache_h_;
            const float one = 1.0f;
            std::memcpy(&c[2], &one, sizeof one);
            std::memcpy(&c[5], &job.world_eye[0], sizeof(WorldComposite));
            std::memcpy(&c[22], &one, sizeof one);
            std::memcpy(&c[25], &job.cache_look_yaw, sizeof(float));
            std::memcpy(&c[26], &job.cache_look_pitch, sizeof(float));
            std::memcpy(&c[27], &job.cache_now_s, sizeof(float));
            const CacheRect cr = world_cache_update_rect(job.cache_lens, job.cache_look_yaw, job.cache_look_pitch, cache_w_, cache_h_);
            const D3D12_GPU_DESCRIPTOR_HANDLE table{srv_gpu_.ptr + UINT64(kSlots * kUpdateGroup) * srv_step_};
            if (cr.x1 > cr.x0 && cr.y1 > cr.y0) {
                const D3D12_VIEWPORT vp{0.0f, 0.0f, FLOAT(cache_w_), FLOAT(cache_h_), 0.0f, 1.0f};
                const D3D12_RECT sc{LONG(cr.x0), LONG(cr.y0), LONG(cr.x1), LONG(cr.y1)};
                setPso_(list_, pso_cache_);
                targets_(list_, 2, &rtv_color, TRUE, nullptr); // colour + stamp: contiguous descriptors
                viewports_(list_, 1, &vp); scissors_(list_, 1, &sc);
                constants_(list_, 1, kBlitConstantsAll, c, 0);
                table_(list_, 0, table);
                draw_(list_, 3, 1, 0, 0); ++f.draws;
            }
            // The fill is measured from the same fresh window every time (3 pixels), after the cache write, into the 4x1 target.
            const D3D12_VIEWPORT fvp{0.0f, 0.0f, 4.0f, 1.0f, 0.0f, 1.0f};
            const D3D12_RECT fsc{0, 0, 4, 1};
            setPso_(list_, pso_fill_);
            targets_(list_, 1, &rtv_fill, FALSE, nullptr);
            viewports_(list_, 1, &fvp); scissors_(list_, 1, &fsc);
            constants_(list_, 1, kBlitConstantsAll, c, 0);
            table_(list_, 0, table);
            draw_(list_, 3, 1, 0, 0); ++f.draws;
            f.cache_written = true;
        }
        for (auto& b : cb) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        barrier_(list_, 3, cb);
        setPso_(list_, pso_); // the eye draws below use the main PSO
    }
    // ---- COCKPIT-2 model cockpit (key xr_cockpit_model). Lazily created on first use (key off: none of this exists): a root signature (table t0..t4 + 28
    // constants b0, static linear-WRAP s0), two PSOs (opaque: depth write; blend: no depth write; both GREATER, back-face cull CCW-front, ONE/INV_SRC_ALPHA into the
    // sRGB RTV), the raw vertex buffer and RGBA8 textures (uploaded ONCE through a temporary UPLOAD buffer like the terrain), and one D32 depth target sized to the
    // larger eye, kept in DEPTH_WRITE and cleared per draw_model. A failure marks the generation dead (no retry per frame): the eyes keep the replayed cockpit.
    bool cockpit_pipeline() {
        if (ck_root_ && ck_pso_opaque_ && ck_pso_blend_ && ck_pso_glass_) return true;
        if (!setPso_ || !clearDepth_ || !copyBuf_ || !copyTex_ || !createCommitted_) return false;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = kModelSlots; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0; params[1].Constants.Num32BitValues = acz::cockpit::kConstantDwords;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error) error->Release();
        if (FAILED(hr) || !blob) { if (blob) blob->Release(); return false; }
        auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
        hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&ck_root_)) : E_FAIL;
        blob->Release();
        if (FAILED(hr) || !ck_root_) { ck_root_ = nullptr; return false; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = ck_root_;
        pd.VS = {g_cockpit_vs, sizeof(g_cockpit_vs)};
        pd.PS = {g_cockpit_ps, sizeof(g_cockpit_ps)};
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pd.RasterizerState.FrontCounterClockwise = TRUE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&ck_pso_opaque_))) || !ck_pso_opaque_) { ck_pso_opaque_ = nullptr; return false; }
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&ck_pso_blend_))) || !ck_pso_blend_) { ck_pso_blend_ = nullptr; return false; }
        // COCKPIT-4 canopy glass: dual-source blend rgb = src0 + dst * src1 (per-channel transmission), a = src0.a + dst.a * (1 - src0.a); no depth write.
        pd.PS = {g_cockpit_glass_ps, sizeof(g_cockpit_glass_ps)};
        b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_SRC1_COLOR; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&ck_pso_glass_))) || !ck_pso_glass_) { ck_pso_glass_ = nullptr; return false; }
        return true;
    }
    // GPU-COST zpre (xr_cockpit_model option zpre:1): the eye composite PSO with a read-only depth test against the model's depth (GREATER_EQUAL with the
    // fullscreen triangle at z = 0: only pixels the model left at the cleared 0 pass; ps_main writes no depth and never discards, so early-Z culls the rest).
    // Lazily created; a failure (dead) only means the composite-first order of COCKPIT-2 (zpre is then not used).
    bool zpre_pipeline() {
        if (pso_zt_) return true;
        if (zt_dead_ || !root_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_;
        pd.VS = {g_xr_stereo_vs, sizeof(g_xr_stereo_vs)};
        pd.PS = {g_xr_stereo_ps, sizeof(g_xr_stereo_ps)};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;   // opaque, exactly as pso_
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&pso_zt_))) || !pso_zt_) { pso_zt_ = nullptr; zt_dead_ = true; return false; }
        return true;
    }
    ID3D12PipelineState* pso_zt_ = nullptr;
    bool zt_dead_ = false;
    // ---- TACTICAL (tac:1): root signature (one root SRV t0, vertex stage), one PSO (no depth, cull none, ONE/INV_SRC_ALPHA into the sRGB RTV) and a
    // persistently mapped UPLOAD buffer of kOverlayCap vertices. Lazily created on the first job with an overlay; a failure is final (no overlay).
    bool ensure_tac() {
        if (tac_root_ && tac_pso_ && tac_ptr_) return true;
        if (tac_dead_ || !setPso_ || !createCommitted_) return false;
        auto dead = [&]() { release(tac_up_); tac_ptr_ = nullptr; release(tac_pso_); release(tac_root_); tac_dead_ = true; return false; };
        rootSrv_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(list_, kListRootSrv);
        if (!rootSrv_) return dead();
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; param.Descriptor.ShaderRegister = 0; param.Descriptor.RegisterSpace = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 1; rs.pParameters = &param;
        ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error) error->Release();
        if (FAILED(hr) || !blob) { if (blob) blob->Release(); return dead(); }
        auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
        hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&tac_root_)) : E_FAIL;
        blob->Release();
        if (FAILED(hr) || !tac_root_) { tac_root_ = nullptr; return dead(); }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = tac_root_;
        pd.VS = {g_xr_tac_vs, sizeof(g_xr_tac_vs)};
        pd.PS = {g_xr_tac_ps, sizeof(g_xr_tac_ps)};
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&tac_pso_))) || !tac_pso_) { tac_pso_ = nullptr; return dead(); }
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(acz::tac::kOverlayCap) * sizeof(acz::tac::Vertex); bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(createCommitted_(dev_, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tac_up_))) || !tac_up_) { tac_up_ = nullptr; return dead(); }
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(tac_up_, kResourceMap);
        auto gpuVa = native<D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*)>(tac_up_, kResourceGpuVa);
        void* p = nullptr;
        const D3D12_RANGE none{0, 0};
        if (!map || !gpuVa || FAILED(map(tac_up_, 0, &none, &p)) || !p) return dead();
        tac_ptr_ = static_cast<uint8_t*>(p);   // stays mapped (UPLOAD heap) until destroy
        tac_va_ = gpuVa(tac_up_);
        if (!tac_va_) return dead();
        return true;
    }
    bool upload_tac(const acz::tac::Overlay& o) {
        const size_t n = std::min<size_t>(o.v.size(), acz::tac::kOverlayCap);
        if (!n || !tac_ptr_) return false;
        std::memcpy(tac_ptr_, o.v.data(), n * sizeof(acz::tac::Vertex));
        tac_n_ = uint32_t(n);
        return true;
    }
    // One slot's triangles into `rtv` (viewport/scissor of the target already set by the caller); restores the composite's root signature, PSO and b1.
    void draw_tac(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const acz::tac::Overlay& o, unsigned slot, RecordFacts& f) {
        const uint32_t first = o.first[slot];
        uint32_t count = o.count[slot];
        if (!count || first >= tac_n_) return;
        count = std::min(count, tac_n_ - first);
        count -= count % 3;
        if (!count) return;
        rootSet_(list_, tac_root_);
        setPso_(list_, tac_pso_);
        targets_(list_, 1, &rtv, FALSE, nullptr);
        rootSrv_(list_, 0, tac_va_ + UINT64(first) * sizeof(acz::tac::Vertex));
        draw_(list_, count, 1, 0, 0); ++f.draws; f.tac_vertices += count;
        root_reset();
        setPso_(list_, pso_);
    }
    // The composite's root signature with b1 (terrain) and b2 (DECOUPLE) cleared: every switch back from another root signature (props, model, tactical)
    // invalidates the root arguments; the caller sets b0/b1/b2 and the table again before its next composite draw.
    void root_reset() {
        static const uint32_t zeros1[kTerrainConstants] = {};
        static const uint32_t zeros2[kReprojectConstants] = {};
        rootSet_(list_, root_);
        constants_(list_, 2, kTerrainConstants, zeros1, 0);
        constants_(list_, 3, kReprojectConstants, zeros2, 0);
    }
    // ---- NBOX (mk360:3; marker360.h StampPlan, docs/world-render/markers-360.md section 12). The stamp ATLAS: a DEFAULT-heap texture of the marker
    // image's format, copy destination + shader resource only (no render-target flag: a committed resource is created zeroed = transparent, so a cell is
    // never garbage), resting state PIXEL_SHADER_RESOURCE between recordings. Recreated when the format or the plan's size changes (the previous recording
    // has completed: CPU-observed before every record); a failed size/format is not retried every frame.
    bool ensure_stamp_atlas(uint32_t format, uint32_t w, uint32_t h) {
        if (st_atlas_ && st_fmt_ == format && st_w_ == w && st_h_ == h) return true;
        if (st_bad_fmt_ == format && st_bad_w_ == w && st_bad_h_ == h) return false;
        release(st_atlas_); st_fmt_ = st_w_ = st_h_ = 0;
        if (!createCommitted_ || !w || !h || w > 16384 || h > 16384) { st_bad_fmt_ = format; st_bad_w_ = w; st_bad_h_ = h; return false; }
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT(format); rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&st_atlas_))) || !st_atlas_) {
            st_atlas_ = nullptr; st_bad_fmt_ = format; st_bad_w_ = w; st_bad_h_ = h; return false;
        }
        st_fmt_ = format; st_w_ = w; st_h_ = h;
        return true;
    }
    // The stamp pass: root [0] root SRV t0 = the tac overlay vertices (vertex stage), [1] table t1 = the atlas (pixel), [2] b0 = tonemap + exposure (pixel),
    // static linear-clamp s0; PSO = the tac blend (ONE / INV_SRC_ALPHA into the sRGB RTV, no depth). Created once; a failure is final (no stamps).
    bool ensure_stamp_pso() {
        if (st_root_ && st_pso_) return true;
        if (st_dead_ || !setPso_) return false;
        auto dead = [&]() { release(st_pso_); release(st_root_); st_dead_ = true; return false; };
        if (!rootSrv_) rootSrv_ = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(list_, kListRootSrv);
        if (!rootSrv_) return dead();
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 1; range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[0].Descriptor.ShaderRegister = 0; params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 0; params[2].Constants.RegisterSpace = 0; params[2].Constants.Num32BitValues = 2;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 3; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error) error->Release();
        if (FAILED(hr) || !blob) { if (blob) blob->Release(); return dead(); }
        auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
        hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&st_root_)) : E_FAIL;
        blob->Release();
        if (FAILED(hr) || !st_root_) { st_root_ = nullptr; return dead(); }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = st_root_;
        pd.VS = {g_xr_stamp_vs, sizeof(g_xr_stamp_vs)};
        pd.PS = {g_xr_stamp_ps, sizeof(g_xr_stamp_ps)};
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&st_pso_))) || !st_pso_) { st_pso_ = nullptr; return dead(); }
        return true;
    }
    // This fresh frame's stamps: marker image (PIXEL_SHADER_RESOURCE | COPY_SOURCE since the out[] barriers) -> the atlas cells, inside a
    // PIXEL_SHADER_RESOURCE -> COPY_DEST -> PIXEL_SHADER_RESOURCE bracket of the atlas. Bounds are re-checked (a copy outside the image or the cell is skipped).
    void record_stamp_copies(const BlitJob& job, RecordFacts& f) {
        const acz::mk360::StampPlan& sp = *job.stamps;
        D3D12_RESOURCE_BARRIER ab{};
        ab.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; ab.Transition.pResource = st_atlas_;
        ab.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ab.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; ab.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier_(list_, 1, &ab);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = st_atlas_; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        src.pResource = static_cast<ID3D12Resource*>(job.markers.resource); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        for (const acz::mk360::StampCopy& c : sp.copy) {
            if (c.cell >= acz::mk360::kStampCells || !c.w || !c.h) continue;
            const uint32_t w = std::min(c.w, sp.atlas.cell_w), h = std::min(c.h, sp.atlas.cell_h);
            if (uint64_t(c.x) + w > job.markers.width || uint64_t(c.y) + h > job.markers.height) continue;
            uint32_t ox = 0, oy = 0;
            acz::mk360::cell_origin(sp.atlas, c.cell, ox, oy);
            if (ox + w > st_w_ || oy + h > st_h_) continue;
            const D3D12_BOX box{c.x, c.y, 0, c.x + w, c.y + h, 1};
            copyTex_(list_, &dst, ox, oy, 0, &src, &box); ++f.stamp_copies;
        }
        std::swap(ab.Transition.StateBefore, ab.Transition.StateAfter);
        barrier_(list_, 1, &ab);
    }
    // The stamped rectangles -> transparent in the marker swapchain image, right after its blit (the full-rect mapping of the marker image onto
    // job.map[kMarkers].dst, rounded outward): those boxes are drawn per eye instead, never twice.
    void clear_stamp_rects(const BlitJob& job, D3D12_CPU_DESCRIPTOR_HANDLE rtv, RecordFacts& f) {
        const acz::mk360::StampPlan& sp = *job.stamps;
        const auto& r = job.map[kMarkers].dst;
        if (!job.markers.width || !job.markers.height || !r.w || !r.h) return;
        const double sx = double(r.w) / double(job.markers.width), sy = double(r.h) / double(job.markers.height);
        D3D12_RECT rects[acz::tac::kMarkCap]{};
        UINT n = 0;
        for (const acz::mk360::StampClear& c : sp.clear) {
            if (n >= acz::tac::kMarkCap) break;
            const LONG x0 = std::max(LONG(r.x), LONG(r.x + std::floor(c.x0 * sx))), x1 = std::min(LONG(r.x + r.w), LONG(r.x + std::ceil(c.x1 * sx)));
            const LONG y0 = std::max(LONG(r.y), LONG(r.y + std::floor(c.y0 * sy))), y1 = std::min(LONG(r.y + r.h), LONG(r.y + std::ceil(c.y1 * sy)));
            if (x1 > x0 && y1 > y0) rects[n++] = D3D12_RECT{x0, y0, x1, y1};
        }
        if (!n) return;
        static const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        clear_(list_, rtv, zero, n, rects); ++f.clears; f.stamp_clears += n;
    }
    // One eye's stamp quads (the tac overlay's stamp slot, already uploaded) into `rtv`; restores the composite's root signature and PSO like draw_tac.
    void draw_stamps(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const acz::tac::Overlay& o, unsigned slot, const BlitJob& job, RecordFacts& f) {
        const uint32_t first = o.first[slot];
        uint32_t count = o.count[slot];
        if (!count || first >= tac_n_) return;
        count = std::min(count, tac_n_ - first);
        count -= count % 3;
        if (!count) return;
        rootSet_(list_, st_root_);
        setPso_(list_, st_pso_);
        targets_(list_, 1, &rtv, FALSE, nullptr);
        rootSrv_(list_, 0, tac_va_ + UINT64(first) * sizeof(acz::tac::Vertex));
        table_(list_, 1, {srv_gpu_.ptr + UINT64(kStampSrv) * srv_step_});
        uint32_t c[2] = {uint32_t(job.tonemap), 0u};
        std::memcpy(&c[1], &job.exposure, sizeof(float));
        constants_(list_, 2, 2, c, 0);
        draw_(list_, count, 1, 0, 0); ++f.draws; f.tac_vertices += count;
        root_reset();
        setPso_(list_, pso_);
    }
    ID3D12Resource* st_atlas_ = nullptr;
    ID3D12RootSignature* st_root_ = nullptr;
    ID3D12PipelineState* st_pso_ = nullptr;
    uint32_t st_fmt_ = 0, st_w_ = 0, st_h_ = 0, st_bad_fmt_ = 0, st_bad_w_ = 0, st_bad_h_ = 0;
    bool st_dead_ = false;
    ID3D12RootSignature* tac_root_ = nullptr;
    ID3D12PipelineState* tac_pso_ = nullptr;
    ID3D12Resource* tac_up_ = nullptr;
    uint8_t* tac_ptr_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS tac_va_ = 0;
    uint32_t tac_n_ = 0;
    bool tac_dead_ = false;
    void(STDMETHODCALLTYPE* rootSrv_)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;

    static constexpr UINT64 align_up(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }
    bool ensure_cockpit(const BlitJob& job) {
        const uint32_t dw = std::max(w_[0], w_[1]), dh = std::max(h_[0], h_[1]);
        if (ck_vb_ && ck_gen_ == job.cockpit_gen && ck_dw_ == dw && ck_dh_ == dh) return true;
        if (ck_dead_ && ck_dead_gen_ == job.cockpit_gen) return false;
        release_cockpit();
        auto dead = [&]() { release_cockpit(); ck_dead_ = true; ck_dead_gen_ = job.cockpit_gen; return false; };
        const acz::cockpit::Mesh& m = *job.cockpit_mesh;
        if (!dw || !dh || m.v.empty() || uint64_t(m.opaque_vertices) + m.blend_vertices + m.glass_vertices != m.v.size() || (m.v.size() % 3) || m.textures.size() > acz::cockpit::kGpuTextures) return dead();
        for (const auto& t : m.textures) if (!t.w || !t.h || t.w > acz::cockpit::kMaxTexDim || t.h > acz::cockpit::kMaxTexDim || t.rgba.size() != size_t(t.w) * t.h * 4u) return dead();
        if (!cockpit_pipeline()) return dead();
        auto createDsv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateDsv);
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        if (!createDsv || !createHeap) return dead();
        const UINT64 vbytes = UINT64(m.v.size()) * sizeof(acz::cockpit::GpuVertex);
        D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = vbytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(createCommitted_(dev_, &def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ck_vb_))) || !ck_vb_) { ck_vb_ = nullptr; return dead(); }
        ck_ntex_ = uint32_t(m.textures.size());
        for (uint32_t i = 0; i < ck_ntex_; ++i) {
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = m.textures[i].w; td.Height = m.textures[i].h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(createCommitted_(dev_, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ck_tex_[i]))) || !ck_tex_[i]) { ck_tex_[i] = nullptr; return dead(); }
        }
        {   // depth target (reversed-Z: cleared to 0, GREATER)
            D3D12_RESOURCE_DESC dd{};
            dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width = dw; dd.Height = dh; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
            dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 0.0f;
            if (FAILED(createCommitted_(dev_, &def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&ck_depth_))) || !ck_depth_) { ck_depth_ = nullptr; return dead(); }
            D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
            if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&ck_dsv_heap_))) || !ck_dsv_heap_) { ck_dsv_heap_ = nullptr; return dead(); }
            ck_dsv_ = ck_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            createDsv(dev_, ck_depth_, &dv, ck_dsv_);
        }
        // staging: vertex bytes at 0, then each texture at a 512-byte boundary with a 256-byte row pitch
        UINT64 off = align_up(vbytes, 512);
        for (uint32_t i = 0; i < ck_ntex_; ++i) { ck_tex_off_[i] = off; ck_tex_pitch_[i] = align_up(UINT64(m.textures[i].w) * 4u, 256); off = align_up(off + ck_tex_pitch_[i] * m.textures[i].h, 512); }
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        bd.Width = off;
        if (FAILED(createCommitted_(dev_, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ck_upload_))) || !ck_upload_) { ck_upload_ = nullptr; return dead(); }
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(ck_upload_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(ck_upload_, kResourceUnmap);
        void* data = nullptr;
        const D3D12_RANGE none{0, 0};
        if (!map || !unmap || FAILED(map(ck_upload_, 0, &none, &data)) || !data) return dead();
        uint8_t* dst = static_cast<uint8_t*>(data);
        std::memset(dst, 0, size_t(off));
        std::memcpy(dst, m.v.data(), size_t(vbytes));
        for (uint32_t i = 0; i < ck_ntex_; ++i)
            for (uint32_t y = 0; y < m.textures[i].h; ++y)
                std::memcpy(dst + ck_tex_off_[i] + y * ck_tex_pitch_[i], &m.textures[i].rgba[size_t(y) * m.textures[i].w * 4u], size_t(m.textures[i].w) * 4u);
        unmap(ck_upload_, 0, &none);
        // SRVs of the model table (written once; no other code touches these slots)
        const D3D12_CPU_DESCRIPTOR_HANDLE base{srv_cpu_.ptr + SIZE_T(kModelBase) * srv_step_};
        D3D12_SHADER_RESOURCE_VIEW_DESC vs{};
        vs.Format = DXGI_FORMAT_R32_TYPELESS; vs.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; vs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vs.Buffer.FirstElement = 0; vs.Buffer.NumElements = UINT(vbytes / 4); vs.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        createSrv_(dev_, ck_vb_, &vs, base);
        for (unsigned i = 0; i < acz::cockpit::kGpuTextures; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
            ts.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ts.Texture2D.MipLevels = 1;
            createSrv_(dev_, i < ck_ntex_ ? ck_tex_[i] : nullptr, &ts, {base.ptr + SIZE_T(1 + i) * srv_step_});
        }
        {   // SKIN-REFL t5: null until record_cockpit_refl creates the map texture
            D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
            ts.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ts.Texture2D.MipLevels = 1;
            createSrv_(dev_, nullptr, &ts, {base.ptr + SIZE_T(kModelReflSlot) * srv_step_});
        }
        if (m.anim_vertices) {   // COCKPIT-4: a persistently mapped UPLOAD buffer for the animated range (rewritten each recording, copied into the VB)
            if (uint64_t(m.anim_first) + m.anim_vertices > uint64_t(m.opaque_vertices) + m.blend_vertices) return dead();
            bd.Width = UINT64(m.anim_vertices) * sizeof(acz::cockpit::GpuVertex);
            if (FAILED(createCommitted_(dev_, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ck_anim_up_))) || !ck_anim_up_) { ck_anim_up_ = nullptr; return dead(); }
            auto amap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(ck_anim_up_, kResourceMap);
            void* ap = nullptr;
            if (!amap || FAILED(amap(ck_anim_up_, 0, &none, &ap)) || !ap) return dead();
            ck_anim_ptr_ = static_cast<uint8_t*>(ap);   // stays mapped (UPLOAD heap) until release_cockpit
            ck_anim_first_ = m.anim_first; ck_anim_count_ = m.anim_vertices;
        }
        ck_vbytes_ = vbytes; ck_gen_ = job.cockpit_gen; ck_dw_ = dw; ck_dh_ = dh; ck_dead_ = false; ck_upload_pending_ = true;
        return true;
    }
    void record_cockpit_upload(RecordFacts& f) {
        copyBuf_(list_, ck_vb_, 0, ck_upload_, 0, ck_vbytes_);   // COMMON -> COPY_DEST by implicit buffer promotion
        D3D12_RESOURCE_BARRIER b[1 + acz::cockpit::kGpuTextures]{};
        unsigned n = 0;
        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES z) {
            b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[n].Transition.pResource = r; b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[n].Transition.StateBefore = a; b[n].Transition.StateAfter = z; ++n;
        };
        for (uint32_t i = 0; i < ck_ntex_; ++i) {
            const D3D12_RESOURCE_DESC td = ck_tex_[i]->GetDesc();
            D3D12_TEXTURE_COPY_LOCATION d{}, s{};
            d.pResource = ck_tex_[i]; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            s.pResource = ck_upload_; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint.Offset = ck_tex_off_[i]; s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            s.PlacedFootprint.Footprint.Width = UINT(td.Width); s.PlacedFootprint.Footprint.Height = td.Height; s.PlacedFootprint.Footprint.Depth = 1;
            s.PlacedFootprint.Footprint.RowPitch = UINT(ck_tex_pitch_[i]);
            copyTex_(list_, &d, 0, 0, 0, &s, nullptr);
            tr(ck_tex_[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        tr(ck_vb_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier_(list_, n, b);
        ck_upload_pending_ = false; ck_upload_recorded_ = true; f.cockpit_uploaded = true;
    }
    // COCKPIT-4: copy this job's posed animated vertices into the VB range [anim_first, +count). Recorded after the one-time upload (VB in
    // NON_PIXEL_SHADER_RESOURCE) and before the eye draws; the UPLOAD buffer is rewritten here, after the allocator reset proved the previous recording
    // (the only reader of the previous contents) complete. A count mismatch (another generation) skips the copy: rest pose.
    void record_cockpit_anim(const BlitJob& job, RecordFacts& f) {
        if (!ck_anim_up_ || !ck_anim_ptr_ || job.cockpit_anim_count != ck_anim_count_) return;
        const UINT64 bytes = UINT64(ck_anim_count_) * sizeof(acz::cockpit::GpuVertex);
        std::memcpy(ck_anim_ptr_, job.cockpit_anim, size_t(bytes));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = ck_vb_; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier_(list_, 1, &b);
        copyBuf_(list_, ck_vb_, UINT64(ck_anim_first_) * sizeof(acz::cockpit::GpuVertex), ck_anim_up_, 0, bytes);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        barrier_(list_, 1, &b);
        f.cockpit_anim_uploaded = true;
    }
    // SKIN-REFL: the drawn stage's reflection map into t5. The texture (exact map size) and a persistently mapped UPLOAD buffer are created on first use and
    // kept for the cockpit generation (release_cockpit drops them); the copy is recorded only when the map key (job.cockpit_refl_gen) or size changes. The
    // UPLOAD buffer is rewritten here, after the allocator reset proved the previous recording (its only reader) complete, as for the animated range.
    // Any failure leaves no map resident (ck_refl_gen_ = 0): the eyes are drawn without the term.
    void record_cockpit_refl(const BlitJob& job, RecordFacts& f) {
        const uint32_t w = job.cockpit_refl_w, h = job.cockpit_refl_h;
        if (!job.cockpit_refl || !w || !h || w > acz::cockpit::kReflMaxDim || h > acz::cockpit::kReflMaxDim || !job.cockpit_refl_gen) return;
        if (ck_refl_tex_ && ck_refl_gen_ == job.cockpit_refl_gen && ck_refl_w_ == w && ck_refl_h_ == h) return;
        if (!ck_refl_tex_ || ck_refl_w_ != w || ck_refl_h_ != h) {
            release(ck_refl_tex_); release(ck_refl_up_); ck_refl_ptr_ = nullptr; ck_refl_gen_ = 0;
            D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(createCommitted_(dev_, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ck_refl_tex_))) || !ck_refl_tex_) {
                ck_refl_tex_ = nullptr; return;
            }
            ck_refl_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
            D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = align_up(UINT64(w) * 4u, 256) * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(createCommitted_(dev_, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ck_refl_up_))) || !ck_refl_up_) {
                ck_refl_up_ = nullptr; release(ck_refl_tex_); return;
            }
            auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(ck_refl_up_, kResourceMap);
            void* p = nullptr; const D3D12_RANGE none{0, 0};
            if (!map || FAILED(map(ck_refl_up_, 0, &none, &p)) || !p) { release(ck_refl_up_); release(ck_refl_tex_); return; }
            ck_refl_ptr_ = static_cast<uint8_t*>(p); ck_refl_w_ = w; ck_refl_h_ = h;
            D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
            ts.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ts.Texture2D.MipLevels = 1;
            createSrv_(dev_, ck_refl_tex_, &ts, {srv_cpu_.ptr + SIZE_T(kModelBase + kModelReflSlot) * srv_step_});
        }
        const UINT64 pitch = align_up(UINT64(w) * 4u, 256);
        for (uint32_t y = 0; y < h; ++y) std::memcpy(ck_refl_ptr_ + y * pitch, job.cockpit_refl + size_t(y) * w * 4u, size_t(w) * 4u);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = ck_refl_tex_; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (ck_refl_state_ != D3D12_RESOURCE_STATE_COPY_DEST) {
            b.Transition.StateBefore = ck_refl_state_; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier_(list_, 1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = ck_refl_tex_; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        s.pResource = ck_refl_up_; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = 0; s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.PlacedFootprint.Footprint.Width = w; s.PlacedFootprint.Footprint.Height = h; s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = UINT(pitch);
        copyTex_(list_, &d, 0, 0, 0, &s, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier_(list_, 1, &b);
        ck_refl_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ck_refl_gen_ = job.cockpit_refl_gen;
        f.cockpit_refl_uploaded = true;
    }
    bool cockpit_refl_ready(const BlitJob& job) const noexcept {
        return job.cockpit_refl && ck_refl_tex_ && ck_refl_gen_ != 0 && ck_refl_gen_ == job.cockpit_refl_gen && ck_refl_w_ == job.cockpit_refl_w &&
               ck_refl_h_ == job.cockpit_refl_h && ck_refl_state_ == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    // One eye: opaque then blend draw into `rtv` with the model depth (cleared), then the blit pipeline is restored (root, PSO, b1 zeros; the caller re-sets
    // b0 and the table before its next draw). The viewport/scissor set for the eye target are reused (the depth target covers them).
    // GPU-COST: marks = write the kPassEyeModel / kPassEyeGlass timestamps of this eye (the submitted eye draws only, never the png copy).
    // GPU-COST zpre: phase 0 = every pass (COCKPIT-2 order, after the composite); 1 = clear + the opaque pass only (BEFORE the composite, which then depth-tests
    // against it); 2 = the blend + glass passes only (after the composite; the depth from phase 1 is kept, so they test exactly as in phase 0).
    void draw_model(const BlitJob& job, unsigned eye, D3D12_CPU_DESCRIPTOR_HANDLE rtv, RecordFacts& f, bool marks, unsigned phase = 0) {
        const acz::cockpit::Mesh& m = *job.cockpit_mesh;
        rootSet_(list_, ck_root_);
        table_(list_, 0, {srv_gpu_.ptr + UINT64(kModelBase) * srv_step_});
        targets_(list_, 1, &rtv, FALSE, &ck_dsv_);
        if (phase != 2) { clearDepth_(list_, ck_dsv_, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); ++f.clears; }
        acz::cockpit::Constants k = job.cockpit_eye[eye];
        if (!cockpit_refl_ready(job)) k.flags &= ~acz::cockpit::kCFlagRefl;   // SKIN-REFL: no resident map = no term (t5 may be a null view)
        else if (k.flags & acz::cockpit::kCFlagRefl) f.cockpit_refl_used = true;
        if (m.opaque_vertices && phase != 2) {
            k.base_vertex = 0; k.pass = 0;
            setPso_(list_, ck_pso_opaque_);
            constants_(list_, 1, acz::cockpit::kConstantDwords, &k, 0);
            draw_(list_, m.opaque_vertices, 1, 0, 0); ++f.draws;
        }
        if (phase == 1) {   // zpre: back to the composite's root signature with b1 cleared (the caller sets its PSO and constants again)
            root_reset();
            return;
        }
        if (m.blend_vertices) {
            k.base_vertex = m.opaque_vertices; k.pass = 1;
            setPso_(list_, ck_pso_blend_);
            constants_(list_, 1, acz::cockpit::kConstantDwords, &k, 0);
            draw_(list_, m.blend_vertices, 1, 0, 0); ++f.draws;
        }
        if (marks) pass_mark(kPassEyeModel(eye));
        if (m.glass_vertices) {   // COCKPIT-4 canopy glass (dual-source transmission)
            k.base_vertex = m.opaque_vertices + m.blend_vertices; k.pass = 2;
            setPso_(list_, ck_pso_glass_);
            constants_(list_, 1, acz::cockpit::kConstantDwords, &k, 0);
            draw_(list_, m.glass_vertices, 1, 0, 0); ++f.draws;
        }
        if (marks) pass_mark(kPassEyeGlass(eye));
        root_reset();
        setPso_(list_, pso_);
    }
    void release_cockpit() {
        release(ck_vb_); for (auto*& t : ck_tex_) release(t); release(ck_depth_); release(ck_dsv_heap_); release(ck_upload_);
        release(ck_anim_up_); ck_anim_ptr_ = nullptr; ck_anim_first_ = ck_anim_count_ = 0;   // COCKPIT-4 (an UPLOAD resource may be released while mapped)
        release(ck_refl_tex_); release(ck_refl_up_); ck_refl_ptr_ = nullptr; ck_refl_gen_ = 0; ck_refl_w_ = ck_refl_h_ = 0;   // SKIN-REFL
        ck_refl_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        ck_ntex_ = 0; ck_gen_ = 0; ck_dw_ = ck_dh_ = 0; ck_vbytes_ = 0; ck_dsv_ = {}; ck_upload_pending_ = false; ck_upload_recorded_ = false;
    }
    ID3D12RootSignature* ck_root_ = nullptr;
    ID3D12PipelineState* ck_pso_opaque_ = nullptr;
    ID3D12PipelineState* ck_pso_blend_ = nullptr;
    ID3D12PipelineState* ck_pso_glass_ = nullptr;   // COCKPIT-4
    ID3D12Resource* ck_anim_up_ = nullptr;           // COCKPIT-4 animated-range UPLOAD buffer (persistently mapped)
    uint8_t* ck_anim_ptr_ = nullptr;
    uint32_t ck_anim_first_ = 0, ck_anim_count_ = 0;
    ID3D12Resource* ck_refl_tex_ = nullptr;           // SKIN-REFL t5 map
    ID3D12Resource* ck_refl_up_ = nullptr;            // its persistently mapped UPLOAD buffer
    uint8_t* ck_refl_ptr_ = nullptr;
    uint32_t ck_refl_w_ = 0, ck_refl_h_ = 0;
    uint64_t ck_refl_gen_ = 0;
    D3D12_RESOURCE_STATES ck_refl_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* ck_vb_ = nullptr;
    ID3D12Resource* ck_tex_[acz::cockpit::kGpuTextures] = {};
    ID3D12Resource* ck_depth_ = nullptr;
    ID3D12Resource* ck_upload_ = nullptr;
    ID3D12DescriptorHeap* ck_dsv_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE ck_dsv_{};
    UINT64 ck_tex_off_[acz::cockpit::kGpuTextures] = {}, ck_tex_pitch_[acz::cockpit::kGpuTextures] = {}, ck_vbytes_ = 0;
    uint32_t ck_ntex_ = 0, ck_dw_ = 0, ck_dh_ = 0;
    uint64_t ck_gen_ = 0, ck_dead_gen_ = 0;
    bool ck_dead_ = false, ck_upload_pending_ = false, ck_upload_recorded_ = false;
    void(STDMETHODCALLTYPE* clearDepth_)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) = nullptr;
    void(STDMETHODCALLTYPE* copyBuf_)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) = nullptr;
    // ---- J-terrain-render. Three immutable DEFAULT-heap textures (heights R8_UINT 513x513, tile words R16_UINT 256x256, tile atlas RGBA8 Texture2DArray with 7 mips) built from
    // the loaded stage, uploaded ONCE through a temporary UPLOAD buffer (copies recorded into the first recording that uses them; the buffer is released after that recording has
    // completed). Rest state PIXEL_SHADER_RESOURCE. A creation failure marks the generation dead (no retry per frame): the eyes then keep the game's world image.
    static constexpr UINT64 kTerrainTileStride = 32768;   // per atlas tile: 7 mips, each placed at a 512-byte boundary, row pitch 256
    static constexpr UINT64 kTerrainMipOff[7] = {0, 16384, 24576, 28672, 30720, 31744, 32256};
    static constexpr UINT64 kTerrainHeightPitch = 768, kTerrainWordsPitch = 512, kTerrainEnvPitch = 4096;
    void terrain_srvs(unsigned base, bool use) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UINT;
        createSrv_(dev_, use ? terrain_h_ : nullptr, &sd, {srv_cpu_.ptr + SIZE_T(base) * srv_step_});
        sd.Format = DXGI_FORMAT_R16_UINT;
        createSrv_(dev_, use ? terrain_w_ : nullptr, &sd, {srv_cpu_.ptr + SIZE_T(base + 1) * srv_step_});
        D3D12_SHADER_RESOURCE_VIEW_DESC ad{};
        ad.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ad.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ad.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        ad.Texture2DArray.MostDetailedMip = 0; ad.Texture2DArray.MipLevels = 7; ad.Texture2DArray.FirstArraySlice = 0;
        ad.Texture2DArray.ArraySize = use ? terrain_slices_ : 1;   // DTEX: the tiles + kDtSlices detail slices when the atlas carries the detail
        createSrv_(dev_, use ? terrain_a_ : nullptr, &ad, {srv_cpu_.ptr + SIZE_T(base + 2) * srv_step_});
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;   // WORLD-2 t10 (null view without an environment)
        createSrv_(dev_, use ? terrain_e_ : nullptr, &sd, {srv_cpu_.ptr + SIZE_T(base + 3) * srv_step_});
    }
    bool ensure_terrain(const BlitJob& job) {
        if (terrain_h_ && terrain_gen_ == job.terrain_gen) return true;
        if (terrain_dead_ && terrain_dead_gen_ == job.terrain_gen) return false;
        release_terrain();
        auto dead = [&]() { release_terrain(); terrain_dead_ = true; terrain_dead_gen_ = job.terrain_gen; return false; };
        const acz::terrain::StageData& s = *job.terrain_stage;
        const acz::terrain::Atlas& a = *job.terrain_atlas;
        auto mapFn = [&](ID3D12Resource* r) { return native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(r, kResourceMap); };
        auto unmapFn = [&](ID3D12Resource* r) { return native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(r, kResourceUnmap); };
        if (!createCommitted_ || !copyTex_ || s.ntiles == 0 || s.ntiles > acz::terrain::kMaxTiles || a.ntiles != s.ntiles ||
            s.heights.size() != size_t(acz::terrain::kHDim) * acz::terrain::kHDim || s.words.size() != size_t(acz::terrain::kTGrid) * acz::terrain::kTGrid) return dead();
        for (int l = 0; l < 7; ++l) if (a.level[l].size() != size_t(a.ntiles) * (64u >> l) * (64u >> l) * 4u) return dead();
        // DTEX: a well-formed detail adds kDtSlices slices after the tiles (a malformed one is skipped: no detail, never fatal)
        bool detail = a.has_detail();
        for (uint32_t l = 1; l < acz::terrain::kDtLevels; ++l) if (a.dt[l].size() != size_t(acz::terrain::kDtDim >> l) * (acz::terrain::kDtDim >> l) * 4u) detail = false;
        const uint32_t slices = s.ntiles + (detail ? acz::terrain::kDtSlices : 0u);
        auto tex = [&](UINT w, UINT h, UINT arr, UINT mips, DXGI_FORMAT f, ID3D12Resource*& out) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = UINT16(arr); rd.MipLevels = UINT16(mips);
            rd.Format = f; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            out = nullptr;
            return SUCCEEDED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out))) && out;
        };
        if (!tex(acz::terrain::kHDim, acz::terrain::kHDim, 1, 1, DXGI_FORMAT_R8_UINT, terrain_h_) ||
            !tex(acz::terrain::kTGrid, acz::terrain::kTGrid, 1, 1, DXGI_FORMAT_R16_UINT, terrain_w_) ||
            !tex(64, 64, slices, 7, DXGI_FORMAT_R8G8B8A8_UNORM, terrain_a_)) return dead();
        terrain_w_off_ = (kTerrainHeightPitch * acz::terrain::kHDim + 511) / 512 * 512;
        terrain_a_off_ = terrain_w_off_ + (kTerrainWordsPitch * acz::terrain::kTGrid + 511) / 512 * 512;
        // WORLD-2: the optional environment texture (RGBA32F kEnvTexW x kEnvTexH, row pitch 4096) after the atlas; a malformed one is skipped (legacy shading), never fatal
        const bool env = job.terrain_env && job.terrain_env->size() == size_t(acz::terrain::kEnvTexW) * acz::terrain::kEnvTexH * 4u;
        if (env && !tex(acz::terrain::kEnvTexW, acz::terrain::kEnvTexH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, terrain_e_)) return dead();
        terrain_e_off_ = terrain_a_off_ + UINT64(slices) * kTerrainTileStride;
        const UINT64 total = terrain_e_off_ + (env ? UINT64(kTerrainEnvPitch) * acz::terrain::kEnvTexH : 0u);
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(createCommitted_(dev_, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&terrain_upload_))) || !terrain_upload_) {
            terrain_upload_ = nullptr; return dead();
        }
        auto map = mapFn(terrain_upload_); auto unmap = unmapFn(terrain_upload_);
        void* data = nullptr;
        const D3D12_RANGE none{0, 0};
        if (!map || !unmap || FAILED(map(terrain_upload_, 0, &none, &data)) || !data) return dead();
        uint8_t* dst = static_cast<uint8_t*>(data);
        std::memset(dst, 0, size_t(total));
        for (uint32_t y = 0; y < acz::terrain::kHDim; ++y) std::memcpy(dst + y * kTerrainHeightPitch, &s.heights[size_t(y) * acz::terrain::kHDim], acz::terrain::kHDim);
        for (uint32_t y = 0; y < acz::terrain::kTGrid; ++y) std::memcpy(dst + terrain_w_off_ + y * kTerrainWordsPitch, &s.words[size_t(y) * acz::terrain::kTGrid], acz::terrain::kTGrid * 2u);
        for (uint32_t t = 0; t < s.ntiles; ++t)
            for (int l = 0; l < 7; ++l) {
                const uint32_t sz = 64u >> l;
                uint8_t* base = dst + terrain_a_off_ + UINT64(t) * kTerrainTileStride + kTerrainMipOff[l];
                for (uint32_t y = 0; y < sz; ++y) std::memcpy(base + y * 256u, &a.level[l][(size_t(t) * sz * sz + size_t(y) * sz) * 4u], size_t(sz) * 4u);
            }
        if (detail) {   // DTEX packing (xr_stereo_blit.hlsl t_dt_texel): slices ntiles+0..3 mip 0 = the 128x128 level's 64x64 quadrants, ntiles+4 mips 0..6 = levels 1..7
            for (uint32_t q = 0; q < 4; ++q) {
                uint8_t* base = dst + terrain_a_off_ + UINT64(s.ntiles + q) * kTerrainTileStride;
                for (uint32_t y = 0; y < 64; ++y)
                    std::memcpy(base + y * 256u, &a.dt[0][((size_t(q / 2u) * 64u + y) * acz::terrain::kDtDim + (q % 2u) * 64u) * 4u], 256u);
            }
            for (uint32_t l = 1; l < acz::terrain::kDtLevels; ++l) {
                const uint32_t sz = acz::terrain::kDtDim >> l;
                uint8_t* base = dst + terrain_a_off_ + UINT64(s.ntiles + 4u) * kTerrainTileStride + kTerrainMipOff[l - 1];
                for (uint32_t y = 0; y < sz; ++y) std::memcpy(base + y * 256u, &a.dt[l][size_t(y) * sz * 4u], size_t(sz) * 4u);
            }
        }
        if (env) for (uint32_t y = 0; y < acz::terrain::kEnvTexH; ++y)
            std::memcpy(dst + terrain_e_off_ + UINT64(y) * kTerrainEnvPitch, &(*job.terrain_env)[size_t(y) * acz::terrain::kEnvTexW * 4u], size_t(kTerrainEnvPitch));
        unmap(terrain_upload_, 0, &none);
        terrain_n_ = s.ntiles; terrain_slices_ = slices; terrain_detail_ = detail; terrain_gen_ = job.terrain_gen; terrain_dead_ = false; terrain_upload_pending_ = true;
        return true;
    }
    void record_terrain_upload(RecordFacts& f) {
        auto loc = [&](ID3D12Resource* r, UINT sub, UINT64 off, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch) {
            D3D12_TEXTURE_COPY_LOCATION d{}, sr{};
            d.pResource = r; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = sub;
            sr.pResource = terrain_upload_; sr.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sr.PlacedFootprint.Offset = off; sr.PlacedFootprint.Footprint.Format = fmt; sr.PlacedFootprint.Footprint.Width = w;
            sr.PlacedFootprint.Footprint.Height = h; sr.PlacedFootprint.Footprint.Depth = 1; sr.PlacedFootprint.Footprint.RowPitch = pitch;
            copyTex_(list_, &d, 0, 0, 0, &sr, nullptr);
        };
        loc(terrain_h_, 0, 0, DXGI_FORMAT_R8_UINT, acz::terrain::kHDim, acz::terrain::kHDim, UINT(kTerrainHeightPitch));
        loc(terrain_w_, 0, terrain_w_off_, DXGI_FORMAT_R16_UINT, acz::terrain::kTGrid, acz::terrain::kTGrid, UINT(kTerrainWordsPitch));
        for (uint32_t t = 0; t < terrain_slices_; ++t)   // DTEX: the detail slices are uploaded like tiles
            for (UINT l = 0; l < 7; ++l)
                loc(terrain_a_, l + t * 7u, terrain_a_off_ + UINT64(t) * kTerrainTileStride + kTerrainMipOff[l], DXGI_FORMAT_R8G8B8A8_UNORM, 64u >> l, 64u >> l, 256u);
        if (terrain_e_) loc(terrain_e_, 0, terrain_e_off_, DXGI_FORMAT_R32G32B32A32_FLOAT, acz::terrain::kEnvTexW, acz::terrain::kEnvTexH, UINT(kTerrainEnvPitch));
        D3D12_RESOURCE_BARRIER b[4]{};
        ID3D12Resource* const res[4] = {terrain_h_, terrain_w_, terrain_a_, terrain_e_};
        const int nb = terrain_e_ ? 4 : 3;
        for (int i = 0; i < nb; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition.pResource = res[i];
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        barrier_(list_, UINT(nb), b);
        terrain_upload_pending_ = false; terrain_upload_recorded_ = true; f.terrain_uploaded = true;
    }
    void release_terrain() {
        release(terrain_h_); release(terrain_w_); release(terrain_a_); release(terrain_e_); release(terrain_upload_);
        terrain_n_ = 0; terrain_slices_ = 0; terrain_detail_ = false; terrain_gen_ = 0; terrain_upload_pending_ = false; terrain_upload_recorded_ = false;
    }
    // ---- WORLD-3 props/trees: CPU selection (acz::props::select) + the per-eye clip matrices, then PropsGpu::prepare (before the list opens).
    bool prepare_props(const BlitJob& job) {
        props_async_ready_ = false;
        if (!setPso_) return false;
        const acz::props::Runtime& rt = *job.props;
        if (job.props_async) {   // PERF: never build the props pipeline/static set inside a frame; the terrain is drawn without props until the worker is done
            if (!props_) props_ = std::make_unique<PropsGpu>(dev_);
            props_async_ready_ = props_->async_ready(rt);
            if (!props_async_ready_) return false;
        }
        PropsFrame& fr = props_frame_;
        fr = PropsFrame{};
        fr.rt = &rt; fr.sel = &props_sel_;
        float view[2][3];
        for (unsigned e = 0; e < 2; ++e) {
            const WorldComposite& wc = job.world_eye[e];
            const TerrainEye& te = job.terrain_eye[e];
            if (!acz::props::make_clip(wc.base, wc.ux, wc.vy, te.right, te.up, te.fwd, fr.clip[e])) return false;
            for (int i = 0; i < 3; ++i) fr.eye[e][i] = te.eye[i];
            float d[3], l = 0;
            for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + 0.5f * wc.ux[i] + 0.5f * wc.vy[i];
            for (int i = 0; i < 3; ++i) { view[e][i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]); l += view[e][i] * view[e][i]; }
            l = std::sqrt(l);
            if (!(l > 1e-12f)) return false;
            for (int i = 0; i < 3; ++i) view[e][i] /= l;
            fr.w[e] = job.map[e].dst.w; fr.h[e] = job.map[e].dst.h;
        }
        try { acz::props::select(rt.set, rt.grid, rt.opt, fr.eye[0], view, props_sel_); } catch (...) { return false; }
        if (!props_) props_ = std::make_unique<PropsGpu>(dev_);
        return props_->prepare(fr);
    }
    std::unique_ptr<PropsGpu> props_;
    bool props_async_ready_ = false;   // PERF: this job's async_ready() answer
    PropsFrame props_frame_{};
    acz::props::Selection props_sel_;
    ID3D12Resource* terrain_h_ = nullptr;
    ID3D12Resource* terrain_w_ = nullptr;
    ID3D12Resource* terrain_a_ = nullptr;
    ID3D12Resource* terrain_e_ = nullptr;   // WORLD-2 environment (null = none)
    ID3D12Resource* terrain_upload_ = nullptr;
    UINT64 terrain_w_off_ = 0, terrain_a_off_ = 0, terrain_e_off_ = 0;
    uint32_t terrain_n_ = 0, terrain_slices_ = 0;   // tiles | array slices (tiles + DTEX detail slices)
    bool terrain_detail_ = false;                   // DTEX: the atlas holds the detail slices (t_bits bit 14)
    uint64_t terrain_gen_ = 0, terrain_dead_gen_ = 0;
    bool terrain_dead_ = false, terrain_upload_pending_ = false, terrain_upload_recorded_ = false;
    // J-world-hold: the proxy-owned copy of the last good world image (DEFAULT heap, no flags, PIXEL_SHADER_RESOURCE between recordings).
    // Recreated when the world's size/format changes (the previous recording's GPU use has completed: completion is CPU-observed per frame).
    bool ensure_held(const D3D12_RESOURCE_DESC& wd) {
        if (held_ && held_w_ == wd.Width && held_h_ == wd.Height && held_format_ == uint32_t(wd.Format)) return true;
        release(held_); held_ready_ = false;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = wd.Width; rd.Height = wd.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = wd.Format; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&held_))) || !held_) {
            held_ = nullptr; return false;
        }
        held_w_ = wd.Width; held_h_ = wd.Height; held_format_ = uint32_t(wd.Format);
        return true;
    }
    // J-test8-fixes xr_png_once: private sRGB target + readback buffer, created at the first capture request and released by png_take / destroy.
    bool png_prepare(uint32_t w, uint32_t h) {
        if (png_rt_ && png_w_ == w && png_h_ == h) return true;
        release_png();
        if (!w || !h || !createCommitted_ || !copyTex_) return false;
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        if (!createHeap || !createRtv) return false;
        png_pitch_ = (UINT64(w) * 4 + 255) / 256 * 256;
        png_stride_ = (png_pitch_ * h + 511) / 512 * 512;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (FAILED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&png_rt_))) || !png_rt_) { png_rt_ = nullptr; return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&png_heap_))) || !png_heap_) { png_heap_ = nullptr; release_png(); return false; }
        png_rtv_ = png_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        createRtv(dev_, png_rt_, &rv, png_rtv_);
        D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = png_stride_ * 3; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(createCommitted_(dev_, &rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&png_rb_))) || !png_rb_) { png_rb_ = nullptr; release_png(); return false; }
        png_w_ = w; png_h_ = h;
        return true;
    }
    // LAYER-DUMP: the private target / readback of the HUD-layer-sized images (png2_mask_: bit 0 HUD, bit 1 markers recorded by this capture).
    bool png2_prepare(uint32_t w, uint32_t h) {
        if (png2_rt_ && png2_w_ == w && png2_h_ == h) return true;
        release(png2_rt_); release(png2_heap_); release(png2_rb_); png2_w_ = png2_h_ = 0;
        if (!w || !h || !createCommitted_ || !copyTex_) return false;
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        if (!createHeap || !createRtv) return false;
        png2_pitch_ = (UINT64(w) * 4 + 255) / 256 * 256;
        png2_stride_ = (png2_pitch_ * h + 511) / 512 * 512;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (FAILED(createCommitted_(dev_, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&png2_rt_))) || !png2_rt_) { png2_rt_ = nullptr; return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&png2_heap_))) || !png2_heap_) { png2_heap_ = nullptr; release(png2_rt_); return false; }
        png2_rtv_ = png2_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        createRtv(dev_, png2_rt_, &rv, png2_rtv_);
        D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = png2_stride_ * 2; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(createCommitted_(dev_, &rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&png2_rb_))) || !png2_rb_) {
            png2_rb_ = nullptr; release(png2_rt_); release(png2_heap_); return false; }
        png2_w_ = w; png2_h_ = h;
        return true;
    }
    ID3D12Resource* png2_rt_ = nullptr;
    ID3D12Resource* png2_rb_ = nullptr;
    ID3D12DescriptorHeap* png2_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE png2_rtv_{};
    uint32_t png2_w_ = 0, png2_h_ = 0, png2_mask_ = 0;
    UINT64 png2_pitch_ = 0, png2_stride_ = 0;
    ID3D12Resource* png_rt_ = nullptr;
    ID3D12Resource* png_rb_ = nullptr;
    ID3D12DescriptorHeap* png_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE png_rtv_{};
    uint32_t png_w_ = 0, png_h_ = 0;
    UINT64 png_pitch_ = 0, png_stride_ = 0;
    bool png_pending_ = false;
    void(STDMETHODCALLTYPE* copyTex_)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
        const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) = nullptr;
    ID3D12Resource* cache_color_ = nullptr;   // J-world-cache (see ensure_cache)
    ID3D12Resource* cache_stamp_ = nullptr;
    ID3D12Resource* cache_fill_ = nullptr;
    ID3D12DescriptorHeap* cache_rtv_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cache_rtv_{};
    uint32_t cache_w_ = 0, cache_h_ = 0, cache_dead_w_ = 0, cache_dead_h_ = 0;
    bool cache_needs_clear_ = false, cache_ok_ = false;
    ID3D12PipelineState* pso_cache_ = nullptr;
    ID3D12PipelineState* pso_fill_ = nullptr;
    void(STDMETHODCALLTYPE* setPso_)(ID3D12GraphicsCommandList*, ID3D12PipelineState*) = nullptr;
    ID3D12Resource* held_ = nullptr;
    bool held_ready_ = false;
    uint64_t held_w_ = 0, held_h_ = 0;
    uint32_t held_format_ = 0;
    void(STDMETHODCALLTYPE* copy_)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*) = nullptr;
    HRESULT(STDMETHODCALLTYPE* createCommitted_)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) = nullptr;
    GpuTiming timing_cache_{};
    bool timing_read_ = false;
    uint64_t timing_value_ = 0;
    bool timestamp_ready_ = false, calibrated_ = false;
    bool pass_rec_ = false, pass_done_ = false;   // GPU-COST: this recording writes the pass marks / the last recording resolved them
    ID3D12QueryHeap* queries_ = nullptr;
    ID3D12Resource* readback_ = nullptr;
    UINT64 gpu_hz_ = 0, cpu_hz_ = 0, calibration_gpu_ = 0, calibration_cpu_ = 0, execute_cpu_ = 0;
    HRESULT(STDMETHODCALLTYPE* calibrate_)(ID3D12CommandQueue*, UINT64*, UINT64*) = nullptr;
    void(STDMETHODCALLTYPE* endQuery_)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) = nullptr;
    void(STDMETHODCALLTYPE* resolve_)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) = nullptr;
    ID3D12Device* dev_;
    ID3D12CommandQueue* q_;
    bool setup_ = false, ready_ = false, open_ = false;
    ID3D12Fence* fence_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3D12DescriptorHeap* srv_ = nullptr;
    ID3D12DescriptorHeap* rtv_[kTargets] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D12CommandAllocator* alloc_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;
    UINT srv_step_ = 0, rtv_step_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_{}, rtv_cpu_[kTargets]{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_{};
    std::array<std::array<ID3D12Resource*, max_images>, kTargets> images_{};
    uint32_t count_[kTargets] = {}, w_[kTargets] = {}, h_[kTargets] = {};
    HRESULT(STDMETHODCALLTYPE* close_)(ID3D12GraphicsCommandList*) = nullptr;
    HRESULT(STDMETHODCALLTYPE* reset_)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*) = nullptr;
    void(STDMETHODCALLTYPE* draw_)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT) = nullptr;
    void(STDMETHODCALLTYPE* topology_)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY) = nullptr;
    void(STDMETHODCALLTYPE* viewports_)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*) = nullptr;
    void(STDMETHODCALLTYPE* scissors_)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*) = nullptr;
    void(STDMETHODCALLTYPE* barrier_)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) = nullptr;
    void(STDMETHODCALLTYPE* heaps_)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*) = nullptr;
    void(STDMETHODCALLTYPE* rootSet_)(ID3D12GraphicsCommandList*, ID3D12RootSignature*) = nullptr;
    void(STDMETHODCALLTYPE* table_)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) = nullptr;
    void(STDMETHODCALLTYPE* constants_)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* targets_)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) = nullptr;
    void(STDMETHODCALLTYPE* clear_)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*) = nullptr;
    HRESULT(STDMETHODCALLTYPE* allocReset_)(ID3D12CommandAllocator*) = nullptr;
    void(STDMETHODCALLTYPE* execute_)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = nullptr;
    HRESULT(STDMETHODCALLTYPE* signal_)(ID3D12CommandQueue*, ID3D12Fence*, UINT64) = nullptr;
    HRESULT(STDMETHODCALLTYPE* wait_)(ID3D12CommandQueue*, ID3D12Fence*, UINT64) = nullptr;
    void(STDMETHODCALLTYPE* createSrv_)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) = nullptr;
    HRESULT(STDMETHODCALLTYPE* removed_)(ID3D12Device*) = nullptr;
};
} // namespace

std::shared_ptr<BlitBackend> make_d3d12_blit(ID3D12Device* device, ID3D12CommandQueue* queue) {
    return std::make_shared<Blit>(device, queue);
}
} // namespace acz::xr_stereo
