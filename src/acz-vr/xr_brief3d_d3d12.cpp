// BRIEF3D consumer, DLL side (xr_brief3d_core.h; docs/world-render/brief3d.md section 6): the D3D12 TableGpu (eye image -> eye swapchain blit)
// and the producer-backed TableSource for the header-only TableCore (every decision there is tested offline). XR owner thread only.
// Every D3D12 call goes through acz::nativeDispatch on the XR binding queue (the game's DIRECT queue); objects are created in a proxy creation
// scope; the destructor makes no foreign call (destroy() runs only after TableCore observed every submission complete).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "xr_brief3d_core.h"
#include "brief3d_live.h"
#include "ui3d_pause.h"   // UI3D stage 3a: blit_mode_bits
#include "d3d12_hook.h"
#include "xr_dump_readback_d3d12.h"   // SCREEN-DUMP
#include "xr_brief3d_vs.h"   // g_xr_brief3d_vs (fxc, xr_brief3d.hlsl vs_table)
#include "xr_brief3d_ps.h"   // g_xr_brief3d_ps (fxc, xr_brief3d.hlsl ps_table)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace acz::xr_stereo {
namespace {
enum : int {
    kRelease = 2,
    kQueueExecute = 10, kQueueSignal = 14,
    kDevCreateAllocator = 9, kDevCreateGraphicsPso = 10, kDevCreateList = 12, kDevCreateHeap = 14, kDevIncrement = 15, kDevCreateRoot = 16,
    kDevCreateSrv = 18, kDevCreateRtv = 20, kDevCreateDsv = 21, kDevCreateCommitted = 27, kDevCreateFence = 36,
    kListClose = 9, kListReset = 10, kListDraw = 12, kListDrawIndexed = 13, kListTopology = 20, kListViewports = 21, kListScissors = 22,
    kListPso = 25, kListBarrier = 26, kListHeaps = 28, kListRoot = 30, kListTable = 32, kListConstant = 34, kListConstants = 36, kListRootSrv = 40,
    kListIndexBuffer = 43, kListVertexBuffers = 44, kListTargets = 46, kListClearDsv = 47, kListClearRtv = 48,
    kResMap = 8, kResVa = 11,
    kAllocReset = 8, kFenceCompleted = 8,
};
constexpr unsigned kMaxChainImages = max_images;
template <class Fn> Fn native(void* o, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(o, slot)); }
template <class T> void rel(T*& p) noexcept {
    if (!p) return;
    if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(p, kRelease)) fn(reinterpret_cast<IUnknown*>(p));
    p = nullptr;
}
acz::vscreen::Completion fence_completion(void* f) noexcept {
    auto fn = f ? native<UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*)>(f, kFenceCompleted) : nullptr;
    if (!fn) { acz::vscreen::Completion c; return c; }
    return acz::vscreen::completion_from_raw(fn(static_cast<ID3D12Fence*>(f)));
}

class D3D12TableGpu final : public TableGpu {
public:
    // `pause`: also create the stage-3a blend PSO (PauseCore's instance only; F4: a pause PSO failure must never fail the table/stage-2 instance).
    // `latch` (brief3d.md 11): this instance also renders the latched scene (render(); its eye images / upload span are created at the first one).
    D3D12TableGpu(ID3D12Device* d, ID3D12CommandQueue* q, bool pause = false, bool latch = false) : dev_(d), q_(q), pause_(pause), latch_(latch) {}
    bool setup(const SwapchainImages& e0, const SwapchainImages& e1, const char*& why) override {
        if (!dev_ || !q_ || !e0.count || !e1.count || e0.count > kMaxChainImages || e1.count > kMaxChainImages) { why = "no-device-or-images"; return false; }
        const SwapchainImages* chains[2] = {&e0, &e1};
        for (const auto* c : chains)
            for (uint32_t i = 0; i < c->count; ++i) {   // an sRGB RTV needs an sRGB or TYPELESS resource (the consumer picks 29, or 28 + MUTABLE_FORMAT)
                auto* r = static_cast<ID3D12Resource*>(c->images[i]);
                const DXGI_FORMAT f = r ? r->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
                if (f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && f != DXGI_FORMAT_R8G8B8A8_TYPELESS) { why = "eye-swapchain-format"; return false; }
            }
        acz::ProxyCreationScope scope(false);
        auto createFence = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**)>(dev_, kDevCreateFence);
        if (!createFence || FAILED(createFence(dev_, 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || !fence_) { fence_ = nullptr; why = "fence"; return false; }
        auto createAlloc = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**)>(dev_, kDevCreateAllocator);
        for (auto& a : alloc_)
            if (!createAlloc || FAILED(createAlloc(dev_, D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) || !a) { a = nullptr; why = "allocator"; return false; }
        auto createList = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**)>(dev_, kDevCreateList);
        if (!createList || FAILED(createList(dev_, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_))) || !list_) { list_ = nullptr; why = "list"; return false; }
        auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(list_, kListClose);
        if (!close || FAILED(close(list_))) { why = "list-close"; return false; }
        // root: [0] table t0 (pixel), [1] one root constant b0 (pixel: UI3D opaque flag), static bilinear clamp s0
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER param[2]{};
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param[0].DescriptorTable.NumDescriptorRanges = 1; param[0].DescriptorTable.pDescriptorRanges = &range;
        param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param[1].Constants.ShaderRegister = 0; param[1].Constants.RegisterSpace = 0; param[1].Constants.Num32BitValues = 1;
        param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2; rs.pParameters = param; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
        ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error) error->Release();
        if (FAILED(hr) || !blob) { if (blob) blob->Release(); why = "root-serialize"; return false; }
        auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
        hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)) : E_FAIL;
        blob->Release();
        if (FAILED(hr) || !root_) { root_ = nullptr; why = "root"; return false; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_;
        pd.VS = {g_xr_brief3d_vs, sizeof(g_xr_brief3d_vs)};
        pd.PS = {g_xr_brief3d_ps, sizeof(g_xr_brief3d_ps)};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;   // no blend: the image is written whole (premultiplied)
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        pd.SampleDesc.Count = 1;
        auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
        if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&pso_))) || !pso_) { pso_ = nullptr; why = "pso"; return false; }
        // UI3D stage 3a (UNTESTED LIVE): the pause blend over the flight eye image, premultiplied: out = src + dst x (1 - src.a) (sRGB RTV: in linear)
        if (pause_) {
            auto& bt = pd.BlendState.RenderTarget[0];
            bt.BlendEnable = TRUE;
            bt.SrcBlend = D3D12_BLEND_ONE; bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOp = D3D12_BLEND_OP_ADD;
            bt.SrcBlendAlpha = D3D12_BLEND_ONE; bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&pso_pause_))) || !pso_pause_) { pso_pause_ = nullptr; why = "pso-pause"; return false; }
        }
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(dev_, kDevIncrement);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        if (!createHeap || !increment || !createRtv) { why = "device-natives"; return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};   // kTableAllocs allocators x 2 eyes (review 2026-10-31 F3: 3 allocators)
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2 * kTableAllocs; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&srv_))) || !srv_) { srv_ = nullptr; why = "srv-heap"; return false; }
        srv_step_ = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = e0.count + e1.count;
        if (FAILED(createHeap(dev_, &rd, IID_PPV_ARGS(&rtv_))) || !rtv_) { rtv_ = nullptr; why = "rtv-heap"; return false; }
        rtv_step_ = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!srv_step_ || !rtv_step_) { why = "descriptor-step"; return false; }
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_->GetCPUDescriptorHandleForHeapStart();
        unsigned n = 0;
        for (unsigned e = 0; e < 2; ++e)
            for (uint32_t i = 0; i < chains[e]->count; ++i, ++n) {
                D3D12_RENDER_TARGET_VIEW_DESC v{}; v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE hi = h; hi.ptr += SIZE_T(n) * rtv_step_;
                createRtv(dev_, static_cast<ID3D12Resource*>(chains[e]->images[i]), &v, hi);
                img_[e][i] = chains[e]->images[i]; rtvh_[e][i] = hi;
            }
        count_[0] = e0.count; count_[1] = e1.count;
        return true;
    }
    acz::vscreen::Completion completed() override { return fence_completion(fence_); }
    Submit blit(unsigned k, void* dst0, void* dst1, const TableImages& src, Extent e0, Extent e1) override {
        Submit s;
        k &= 1u;
        if (src.pause && !pso_pause_) return s;   // a pause blit needs the pause PSO (only the pause instance has one)
        void* dst[2] = {dst0, dst1};
        const Extent ext[2] = {e0, e1};
        D3D12_CPU_DESCRIPTOR_HANDLE rt[2] = {};
        for (unsigned e = 0; e < 2; ++e) {   // the RTV made at setup for this exact swapchain image
            bool found = false;
            for (uint32_t i = 0; i < count_[e]; ++i) if (img_[e][i] == dst[e]) { rt[e] = rtvh_[e][i]; found = true; }
            if (!found || !src.eye[e] || !ext[e].w || !ext[e].h) return s;
        }
        auto allocReset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*)>(alloc_[k], kAllocReset);
        auto reset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(list_, kListReset);
        auto heaps = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(list_, kListHeaps);
        auto rootSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(list_, kListRoot);
        auto table = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(list_, kListTable);
        auto constant = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(list_, kListConstant);
        auto topology = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(list_, kListTopology);
        auto viewports = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(list_, kListViewports);
        auto scissors = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(list_, kListScissors);
        auto targets = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>(list_, kListTargets);
        auto draw = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)>(list_, kListDraw);
        auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(list_, kListClose);
        auto execute = native<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(q_, kQueueExecute);
        auto signal = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(q_, kQueueSignal);
        auto createSrv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateSrv);
        if (!allocReset || !reset || !heaps || !rootSet || !table || !constant || !topology || !viewports || !scissors || !targets || !draw || !close || !execute ||
            !signal || !createSrv) return s;
        // This allocator's previous submission completed (TableCore checked its value): its two SRV slots are free to rewrite.
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_->GetGPUDescriptorHandleForHeapStart();
        for (unsigned e = 0; e < 2; ++e) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE c = cpu; c.ptr += SIZE_T(k * 2 + e) * srv_step_;
            createSrv(dev_, static_cast<ID3D12Resource*>(src.eye[e]), &v, c);
        }
        if (FAILED(allocReset(alloc_[k])) || FAILED(reset(list_, alloc_[k], src.pause ? pso_pause_ : pso_))) return s;
        ID3D12DescriptorHeap* hs[1] = {srv_};
        heaps(list_, 1, hs);
        rootSet(list_, root_);
        // mode bits (ui3d_pause.h blit_mode_bits): 0 table (premultiplied), 1 UI3D opaque (rgb, alpha 1), 2 | floor << 8 the stage-3a pause blend
        // + bits 16..17 / 24..31 (ui3d_resample.h): the Catmull-Rom (+ unsharp) upscale for the opaque UI3D / pause slots (u3r / u3k); the table keeps bilinear
        constant(list_, 1, acz::ui3d::pause::blit_mode_bits(src.opaque, src.pause, src.floor) |
                           ((src.opaque || src.pause) ? acz::brief3d::live::producer().ui3d_filter_bits() : 0u), 0);
        topology(list_, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (unsigned e = 0; e < 2; ++e) {
            // destination: RENDER_TARGET (an acquired, waited colour swapchain image); source: COMMON -> PIXEL_SHADER_RESOURCE by implicit promotion
            const D3D12_VIEWPORT vp{0.0f, 0.0f, float(ext[e].w), float(ext[e].h), 0.0f, 1.0f};
            const D3D12_RECT sr{0, 0, LONG(ext[e].w), LONG(ext[e].h)};
            viewports(list_, 1, &vp);
            scissors(list_, 1, &sr);
            targets(list_, 1, &rt[e], FALSE, nullptr);
            D3D12_GPU_DESCRIPTOR_HANDLE g = gpu; g.ptr += UINT64(k * 2 + e) * srv_step_;
            table(list_, 0, g);
            draw(list_, 3, 1, 0, 0);
        }
        // SCREEN-DUMP: eye 0 as just written (RENDER_TARGET -> COPY_SOURCE -> RENDER_TARGET) into the readback buffer
        const bool rb = rb_.want && rb_.record(dev_, list_, static_cast<ID3D12Resource*>(dst0), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, ext[0].w, ext[0].h);
        if (FAILED(close(list_))) { if (rb) rb_.abandon(); return s; }
        ID3D12CommandList* lists[1] = {list_};
        execute(q_, 1, lists);
        s.executed = true; s.readback = rb;
        if (FAILED(signal(q_, fence_, value_ + 1))) return s;
        s.signalled = true; s.value = ++value_;
        return s;
    }
    // brief3d.md 11 (b3l:1). One list on the XR binding queue: [this allocator's upload span: the scene's geometry (only when the scene changed
    // since this span last held one), both eyes' records and culled indices] clear the private eye images (transparent; depth = the scene's far
    // value) -> per eye, per latched draw: the GAME's root signature and PSO, IA = the latched index / vertex blobs in our upload span, t11 = the
    // latched colour table, b10 = the game's two root constants, then per drawn copy t10 = the eye record and DrawIndexedInstanced -> eye images
    // RENDER_TARGET -> PIXEL_SHADER_RESOURCE -> the table blit into dst0 / dst1 (premultiplied, as blit()) -> back to RENDER_TARGET -> [readback]
    // -> Close / Execute / Signal. The game's objects are only read (the scene keeps their references until this value completed: LatchCore).
    Submit render(unsigned k, void* dst0, void* dst1, const acz::brief3d::latch::FramePlan& fp, Extent e0, Extent e1) override {
        namespace lt = acz::brief3d::latch; namespace b3 = acz::brief3d;
        Submit s;
        if (k >= kTableAllocs) return s;   // (F3) LatchCore cycles kTableAllocs allocators / upload spans (never folded onto a busy one)
        if (!latch_ || !fp.scene || !ensure_latch()) return s;
        const lt::Scene& sc = *fp.scene;
        void* dst[2] = {dst0, dst1};
        const Extent ext[2] = {e0, e1};
        D3D12_CPU_DESCRIPTOR_HANDLE rt[2] = {};
        for (unsigned e = 0; e < 2; ++e) {
            bool found = false;
            for (uint32_t i = 0; i < count_[e]; ++i) if (img_[e][i] == dst[e]) { rt[e] = rtvh_[e][i]; found = true; }
            if (!found || !ext[e].w || !ext[e].h) return s;
            if (fp.eye[e].rec.size() > size_t(lt::kMaxRecs) * lt::kRecStride || fp.eye[e].cull.size() > lt::kCullCap) return s;
            if (fp.eye[e].mode.size() != sc.draws.size() || fp.eye[e].mask.size() != sc.draws.size()) return s;
        }
        if (sc.geom_bytes > lt::kGeomCap) return s;
        auto allocReset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*)>(alloc_[k], kAllocReset);
        auto reset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(list_, kListReset);
        auto heaps = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(list_, kListHeaps);
        auto rootSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(list_, kListRoot);
        auto psoSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list_, kListPso);
        auto table = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(list_, kListTable);
        auto constant = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(list_, kListConstant);
        auto constants = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(list_, kListConstants);
        auto rootSrv = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(list_, kListRootSrv);
        auto topology = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(list_, kListTopology);
        auto ibSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*)>(list_, kListIndexBuffer);
        auto vbSet = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*)>(list_, kListVertexBuffers);
        auto viewports = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(list_, kListViewports);
        auto scissors = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(list_, kListScissors);
        auto targets = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>(list_, kListTargets);
        auto clearRtv = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)>(list_, kListClearRtv);
        auto clearDsv = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>(list_, kListClearDsv);
        auto barrier = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(list_, kListBarrier);
        auto drawIndexed = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT)>(list_, kListDrawIndexed);
        auto draw = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)>(list_, kListDraw);
        auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(list_, kListClose);
        auto execute = native<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(q_, kQueueExecute);
        auto signal = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(q_, kQueueSignal);
        auto createSrv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateSrv);
        if (!allocReset || !reset || !heaps || !rootSet || !psoSet || !table || !constant || !constants || !rootSrv || !topology || !ibSet || !vbSet ||
            !viewports || !scissors || !targets || !clearRtv || !clearDsv || !barrier || !drawIndexed || !draw || !close || !execute || !signal || !createSrv) return s;
        // This allocator's previous submission completed (LatchCore checked its value): its upload span and SRV slots are free to rewrite.
        uint8_t* up = lup_ + uint64_t(k) * lt::kUploadPerFrame;
        const uint64_t va = lva_ + uint64_t(k) * lt::kUploadPerFrame;
        if (geom_serial_[k] != sc.serial) {
            for (const lt::Draw& d : sc.draws) {
                if (d.g.ib) std::memcpy(up + d.ib_off, d.g.ib->data(), d.g.ib->size());
                if (d.g.vb) std::memcpy(up + d.vb_off, d.g.vb->data(), d.g.vb->size());
                if (d.g.t11) std::memcpy(up + d.t11_off, d.g.t11->data(), d.g.t11->size());
            }
            geom_serial_[k] = sc.serial;
        }
        for (unsigned e = 0; e < 2; ++e) {
            const lt::EyePlan& ep = fp.eye[e];
            if (!ep.rec.empty()) std::memcpy(up + lt::kGeomCap + uint64_t(e) * lt::kMaxRecs * lt::kRecStride, ep.rec.data(), ep.rec.size());
            if (!ep.cull.empty()) std::memcpy(up + lt::kGeomCap + lt::kRecArea + uint64_t(e) * lt::kCullCap, ep.cull.data(), ep.cull.size());
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_->GetGPUDescriptorHandleForHeapStart();
        for (unsigned e = 0; e < 2; ++e) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE c = cpu; c.ptr += SIZE_T(k * 2 + e) * srv_step_;
            createSrv(dev_, leye_[e], &v, c);
        }
        if (FAILED(allocReset(alloc_[k])) || FAILED(reset(list_, alloc_[k], nullptr))) return s;
        static constexpr FLOAT transparent[4] = {0, 0, 0, 0};
        for (unsigned e = 0; e < 2; ++e) {
            clearRtv(list_, lrtv_[e], transparent, 0, nullptr);
            clearDsv(list_, ldsv_[e], D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, sc.clear_depth, 0, 0, nullptr);
        }
        const D3D12_VIEWPORT eyeVp{0.0f, 0.0f, float(acz::brief3d::live::kEyeW), float(acz::brief3d::live::kEyeH), 0.0f, 1.0f};
        const D3D12_RECT eyeSc{0, 0, LONG(acz::brief3d::live::kEyeW), LONG(acz::brief3d::live::kEyeH)};
        for (unsigned e = 0; e < 2; ++e) {
            const lt::EyePlan& ep = fp.eye[e];
            targets(list_, 1, &lrtv_[e], FALSE, &ldsv_[e]);
            viewports(list_, 1, &eyeVp);
            scissors(list_, 1, &eyeSc);
            unsigned r = 0;
            for (size_t i = 0; i < sc.draws.size(); ++i) {
                const lt::Draw& d = sc.draws[i];
                const unsigned n = d.kind == b3::Kind::selmap ? 1u : d.copies;
                const unsigned base_r = r; r += n;
                if (ep.mode[i] == lt::kModeSkip || !ep.mask[i]) continue;
                rootSet(list_, static_cast<ID3D12RootSignature*>(d.g.root.get()));   // the game's own objects (the scene holds their references)
                psoSet(list_, static_cast<ID3D12PipelineState*>(d.g.pso.get()));
                topology(list_, D3D12_PRIMITIVE_TOPOLOGY(d.g.ia_topology));
                const bool culled = ep.mode[i] == lt::kModeCulled;
                const uint32_t count = culled ? ep.cull_count[i] : d.g.index_count;
                D3D12_INDEX_BUFFER_VIEW ib{};
                ib.BufferLocation = culled ? va + lt::kGeomCap + lt::kRecArea + uint64_t(e) * lt::kCullCap + ep.cull_off[i] : va + d.ib_off;
                ib.SizeInBytes = UINT(uint64_t(count) * d.g.index_bytes);
                ib.Format = d.g.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                ibSet(list_, &ib);
                D3D12_VERTEX_BUFFER_VIEW vb{};
                vb.BufferLocation = va + d.vb_off; vb.SizeInBytes = UINT(d.g.vb ? d.g.vb->size() : 0); vb.StrideInBytes = d.g.stride;
                vbSet(list_, 0, 1, &vb);
                if (d.kind == b3::Kind::selmap) rootSrv(list_, d.g.t11_param, va + d.t11_off);
                constants(list_, d.b10_param, 2, d.b10, 0);
                for (unsigned c = 0; c < n; ++c) {
                    if (!((ep.mask[i] >> c) & 1u)) continue;
                    rootSrv(list_, d.t10_param, va + lt::kGeomCap + (uint64_t(e) * lt::kMaxRecs + base_r + c) * lt::kRecStride);
                    drawIndexed(list_, count, 1, 0, d.g.base_vertex, 0);
                }
            }
        }
        D3D12_RESOURCE_BARRIER bar[2] = {};
        for (unsigned e = 0; e < 2; ++e) {
            bar[e].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[e].Transition.pResource = leye_[e];
            bar[e].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar[e].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; bar[e].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        barrier(list_, 2, bar);
        ID3D12DescriptorHeap* hs[1] = {srv_};
        heaps(list_, 1, hs);
        rootSet(list_, root_);
        psoSet(list_, pso_);
        constant(list_, 1, acz::ui3d::pause::blit_mode_bits(false, false, 0.0f), 0);   // mode 0: the premultiplied table (bilinear)
        topology(list_, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (unsigned e = 0; e < 2; ++e) {
            const D3D12_VIEWPORT vp{0.0f, 0.0f, float(ext[e].w), float(ext[e].h), 0.0f, 1.0f};
            const D3D12_RECT sr{0, 0, LONG(ext[e].w), LONG(ext[e].h)};
            viewports(list_, 1, &vp);
            scissors(list_, 1, &sr);
            targets(list_, 1, &rt[e], FALSE, nullptr);
            D3D12_GPU_DESCRIPTOR_HANDLE g = gpu; g.ptr += UINT64(k * 2 + e) * srv_step_;
            table(list_, 0, g);
            draw(list_, 3, 1, 0, 0);
        }
        for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        barrier(list_, 2, bar);
        const bool rb = rb_.want && rb_.record(dev_, list_, static_cast<ID3D12Resource*>(dst0), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, ext[0].w, ext[0].h);
        if (FAILED(close(list_))) { if (rb) rb_.abandon(); return s; }
        ID3D12CommandList* lists[1] = {list_};
        execute(q_, 1, lists);
        s.executed = true; s.readback = rb;
        if (FAILED(signal(q_, fence_, value_ + 1))) return s;
        s.signalled = true; s.value = ++value_;
        return s;
    }
    void readback_want(bool w) override { rb_.want = w; }
    bool readback_take(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override { return rb_.take(rgba, w, h); }
    void readback_discard() override { rb_.discard(); }
    void destroy() override {
        rb_.release(); rel(list_); for (auto& a : alloc_) rel(a); rel(pso_); rel(pso_pause_); rel(root_); rel(srv_); rel(rtv_); rel(fence_);
        for (auto& r : leye_) rel(r);
        for (auto& r : ldepth_) rel(r);
        rel(lupload_); rel(lrtvHeap_); rel(ldsvHeap_);   // the mapped upload buffer is released mapped (D3D12 allows it)
        lup_ = nullptr;
    }
private:
    // brief3d.md 11: the private eye images (the game PSO's formats), their RTV/DSV heaps and the mapped upload buffer (kTableAllocs allocator spans).
    // Created once at the first render (proxy creation scope); a failure leaves render() refusing for the rest of the session.
    bool ensure_latch() {
        namespace lt = acz::brief3d::latch;
        if (latch_tried_) return latch_ok_;
        latch_tried_ = true;
        acz::ProxyCreationScope scope(false);
        using CreateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, REFIID, void**);
        auto create = native<CreateFn>(dev_, kDevCreateCommitted);
        auto createHeap = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)>(dev_, kDevCreateHeap);
        auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(dev_, kDevIncrement);
        auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
        auto createDsv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateDsv);
        if (!create || !createHeap || !increment || !createRtv || !createDsv) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&lrtvHeap_))) || !lrtvHeap_) { lrtvHeap_ = nullptr; return false; }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&ldsvHeap_))) || !ldsvHeap_) { ldsvHeap_ = nullptr; return false; }
        const UINT rs = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), ds = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!rs || !ds) return false;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = acz::brief3d::live::kEyeW; td.Height = acz::brief3d::live::kEyeH;
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        for (unsigned e = 0; e < 2; ++e) {
            D3D12_RESOURCE_DESC d = td; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{}; cv.Format = d.Format;
            if (FAILED(create(dev_, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&leye_[e]))) || !leye_[e]) { leye_[e] = nullptr; return false; }
            D3D12_CPU_DESCRIPTOR_HANDLE h = lrtvHeap_->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(e) * rs;
            D3D12_RENDER_TARGET_VIEW_DESC v{}; v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            createRtv(dev_, leye_[e], &v, h); lrtv_[e] = h;
            D3D12_RESOURCE_DESC z = td; z.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; z.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE zv{}; zv.Format = z.Format; zv.DepthStencil.Depth = 0.0f; zv.DepthStencil.Stencil = 0;
            if (FAILED(create(dev_, &hp, D3D12_HEAP_FLAG_NONE, &z, D3D12_RESOURCE_STATE_DEPTH_WRITE, &zv, IID_PPV_ARGS(&ldepth_[e]))) || !ldepth_[e]) { ldepth_[e] = nullptr; return false; }
            D3D12_CPU_DESCRIPTOR_HANDLE hz = ldsvHeap_->GetCPUDescriptorHandleForHeapStart(); hz.ptr += SIZE_T(e) * ds;
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            createDsv(dev_, ldepth_[e], &dv, hz); ldsv_[e] = hz;
        }
        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = uint64_t(kTableAllocs) * lt::kUploadPerFrame; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(create(dev_, &uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&lupload_))) || !lupload_) { lupload_ = nullptr; return false; }
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(lupload_, kResMap);
        auto gva = native<D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*)>(lupload_, kResVa);
        const D3D12_RANGE none{0, 0}; void* p = nullptr;
        if (!map || !gva || FAILED(map(lupload_, 0, &none, &p)) || !p) return false;
        lva_ = gva(lupload_);
        if (!lva_ || lva_ % 256) return false;
        lup_ = static_cast<uint8_t*>(p);
        latch_ok_ = true;
        return true;
    }
    bool latch_ = false, latch_tried_ = false, latch_ok_ = false;
    ID3D12Resource* leye_[2] = {}; ID3D12Resource* ldepth_[2] = {}; ID3D12Resource* lupload_ = nullptr;
    ID3D12DescriptorHeap* lrtvHeap_ = nullptr; ID3D12DescriptorHeap* ldsvHeap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE lrtv_[2] = {}, ldsv_[2] = {};
    uint8_t* lup_ = nullptr; uint64_t lva_ = 0;
    uint64_t geom_serial_[kTableAllocs] = {};   // the scene serial whose geometry each allocator span holds (0 = none)
    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* q_ = nullptr;
    bool pause_ = false;
    ID3D12Fence* fence_ = nullptr;
    ID3D12CommandAllocator* alloc_[kTableAllocs] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3D12PipelineState* pso_pause_ = nullptr;   // UI3D stage 3a: blend ONE / INV_SRC_ALPHA
    ID3D12DescriptorHeap* srv_ = nullptr;
    ID3D12DescriptorHeap* rtv_ = nullptr;
    UINT srv_step_ = 0, rtv_step_ = 0;
    void* img_[2][kMaxChainImages] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvh_[2][kMaxChainImages] = {};
    uint32_t count_[2] = {0, 0};
    uint64_t value_ = 0;
    dumprb::Readback rb_;   // SCREEN-DUMP
};

// The producer (brief3d_live.h) as the table source: READY slots whose producer-fence value completed (CPU-observed, loss-aware).
class ProducerSource final : public TableSource {
public:
    bool acquire(uint64_t after, acz::brief3d::live::Held& out, TableImages& img) override {
        auto& p = acz::brief3d::live::producer();
        void* f = p.fence();
        if (!f) return false;
        const acz::vscreen::Completion c = fence_completion(f);
        if (c.lost) { lost_ = true; return false; }
        if (!p.acquire(after, c, out)) return false;
        img.eye[0] = p.image(unsigned(out.slot), 0); img.eye[1] = p.image(unsigned(out.slot), 1);
        img.width = acz::brief3d::live::kEyeW; img.height = acz::brief3d::live::kEyeH; img.format = acz::brief3d::live::kRtvFormat;
        img.opaque = out.meta.opaque;   // UI3D slot
        img.pause = out.meta.pause;     // UI3D stage 3a: PauseCore uses only these (TableCore never sees a pause flag: it blits as before)
        return true;
    }
    void release(const acz::brief3d::live::Held& h) override { acz::brief3d::live::producer().release(h); }
    bool dead() override { return lost_ || acz::brief3d::live::producer().poisoned(); }
    int64_t clock_ms(int64_t) override { return int64_t(GetTickCount64()); }   // SlotMeta::published_ms = on_present(GetTickCount64)
    std::string describe() override {
        char t[acz::brief3d::kLineLimit], l[acz::brief3d::kLineLimit];   // table-line tail + '\n' + the producer's own line
        acz::brief3d::live::producer().format_xr(t, sizeof t, l, sizeof l);
        return std::string(t) + "\n" + l;
    }
private:
    bool lost_ = false;
};
// brief3d.md 11: the producer's latched scene (Scene::published_ms is on the producer's Present clock, GetTickCount64).
class ProducerLatchSource final : public LatchSource {
public:
    std::shared_ptr<const acz::brief3d::latch::Scene> scene() override { return acz::brief3d::live::producer().latched_scene(); }
    bool dead() override { return acz::brief3d::live::producer().poisoned(); }
    int64_t clock_ms(int64_t) override { return int64_t(GetTickCount64()); }
};
} // namespace

std::shared_ptr<VScreenHook> make_brief3d_hook(std::shared_ptr<VScreenHook> inner, void* device, void* queue) {
    if (!inner || !device || !queue || !acz::brief3d::live::producer().configured()) return inner;
    auto table = std::make_shared<TableCore>(std::make_shared<D3D12TableGpu>(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue)),
                                             std::make_shared<ProducerSource>());
    table->set_layering(acz::brief3d::live::producer().ui3d_layering());   // UI3D 2026-10-30b (u3q; 1 unless ui3d configured u3q:0)
    std::shared_ptr<PauseCore> pause;   // UI3D stage 3a (u3p:1): its own list/fence/RTVs (the eye swapchains are the flight's)
    if (acz::brief3d::live::producer().ui3d_pause_configured())
        pause = std::make_shared<PauseCore>(std::make_shared<D3D12TableGpu>(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue), true),
                                            std::make_shared<ProducerSource>());
    std::shared_ptr<LatchCore> latch;   // brief3d.md 11 (b3l:1): its own list / fence / eye images / upload span (D3D12TableGpu latch instance)
    if (acz::brief3d::live::producer().latch_mode()) {
        latch = std::make_shared<LatchCore>(std::make_shared<D3D12TableGpu>(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue), false, true),
                                            std::make_shared<ProducerLatchSource>());
        latch->set_layering(acz::brief3d::live::producer().ui3d_layering());
    }
    return std::make_shared<Brief3dHook>(std::move(inner), std::move(table), std::move(pause), std::move(latch));
}

} // namespace acz::xr_stereo
