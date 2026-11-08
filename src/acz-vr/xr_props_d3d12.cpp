// WORLD-3 props/trees GPU pass (see xr_props_d3d12.h). Owner thread only, called from the D3D12 blit backend's record().
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "xr_props_d3d12.h"
#include "d3d12_hook.h"
#include "xr_props_vs.h"   // g_xr_props_vs (fxc, xr_props.hlsl vs_props)
#include "xr_props_ps.h"   // g_xr_props_ps (fxc, xr_props.hlsl ps_props)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace acz::xr_stereo {
namespace {
enum : int {
    kRelease = 2,
    kDevCreateGraphicsPso = 10, kDevCreateHeap = 14, kDevIncrement = 15, kDevCreateRoot = 16, kDevCreateSrv = 18, kDevCreateRtv = 20, kDevCreateDsv = 21,
    kDevCreateCommittedResource = 27,
    kListDrawIndexed = 13, kListCopyTexture = 16, kListTopology = 20, kListViewports = 21, kListScissors = 22, kListSetPso = 25, kListBarrier = 26,
    kListHeaps = 28, kListRoot = 30, kListTable = 32, kListConstants = 36, kListIndexBuffer = 43, kListVertexBuffers = 44, kListTargets = 46,
    kListClearDepth = 47, kListClear = 48,
    kResourceMap = 8, kResourceUnmap = 9, kResourceGpuVa = 11,
};
constexpr unsigned kPassConstants = 20;   // r0..r3 (16) | eye (3) | pad
template <class Fn> Fn native(void* object, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(object, slot)); }
template <class T> void rel(T*& p) noexcept {
    if (!p) return;
    if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(p, kRelease)) fn(reinterpret_cast<IUnknown*>(p));
    p = nullptr;
}
using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
bool buffer(ID3D12Device* dev, UINT64 bytes, ID3D12Resource*& out) {
    auto create = native<CreateCommittedFn>(dev, kDevCreateCommittedResource);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes < 256 ? 256 : bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    out = nullptr;
    return create && SUCCEEDED(create(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out))) && out;
}
bool write(ID3D12Resource* r, const void* data, size_t bytes, size_t offset = 0) {
    auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(r, kResourceMap);
    auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(r, kResourceUnmap);
    void* p = nullptr; const D3D12_RANGE none{0, 0};
    if (!map || !unmap || FAILED(map(r, 0, &none, &p)) || !p) return false;
    if (bytes) std::memcpy(static_cast<uint8_t*>(p) + offset, data, bytes);
    unmap(r, 0, nullptr);
    return true;
}
UINT64 gpu_va(ID3D12Resource* r) {
    auto fn = native<D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*)>(r, kResourceGpuVa);
    return fn ? fn(r) : 0;
}
} // namespace

void PropsGpu::release_static() {
    rel(vb_); rel(ib_); rel(tex_); rel(tex_up_); rel(srv_);
    static_rt_ = nullptr; static_gen_ = 0; upload_pending_ = false; mip_off_.clear(); mip_pitch_.clear();
}
void PropsGpu::release_targets() {
    for (unsigned e = 0; e < 2; ++e) { rel(color_[e]); rel(dist_[e]); tw_[e] = th_[e] = 0; }
    rel(depth_); rel(rtv_); rel(dsv_);
}
void PropsGpu::release() {
    join_worker();   // PERF: the init worker never outlives the objects it builds
    release_static(); release_targets(); rel(inst_); inst_cap_ = 0; rel(pso_); rel(root_);
    state_.store(kIdle, std::memory_order_release); async_rt_ = nullptr; async_gen_ = 0;
}
void PropsGpu::join_worker() noexcept {
    try { if (worker_.joinable()) worker_.join(); } catch (...) {}
}
bool PropsGpu::async_ready(const acz::props::Runtime& rt) {
    const int st = state_.load(std::memory_order_acquire);
    if (st == kRunning) return false;                                  // never wait
    if (async_rt_ == &rt && async_gen_ == rt.generation) return st == kReady;   // finished for THIS runtime (ready, or failed: stays off)
    // first use, or a new runtime/generation after the previous worker finished: (re)start
    join_worker();
    async_rt_ = &rt; async_gen_ = rt.generation;
    init_ms_.store(0, std::memory_order_release);
    state_.store(kRunning, std::memory_order_release);
    try {
        worker_ = std::thread([this, &rt] {
            const ULONGLONG t0 = GetTickCount64();
            acz::enterProxyCreationScope(false);   // proxy-owned objects (the hooks' creation bookkeeping), no XR window
            bool ok = false;
            try { ok = ensure_pipeline() && ensure_static(rt); } catch (...) { ok = false; }
            acz::leaveProxyCreationScope(false);
            init_ms_.store(uint32_t(GetTickCount64() - t0), std::memory_order_release);
            state_.store(ok ? kReady : kFailed, std::memory_order_release);
        });
    } catch (...) { state_.store(kFailed, std::memory_order_release); }
    return false;
}

bool PropsGpu::ensure_pipeline() {
    if (pso_) return true;
    if (pipeline_dead_ || !dev_) return false;
    pipeline_dead_ = true;   // cleared on success
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = kPassConstants;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP; sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (error) error->Release();
    if (FAILED(hr) || !blob) { if (blob) blob->Release(); return false; }
    auto createRoot = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev_, kDevCreateRoot);
    hr = createRoot ? createRoot(dev_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)) : E_FAIL;
    blob->Release();
    if (FAILED(hr) || !root_) { root_ = nullptr; return false; }
    const D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R16G16_SNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 1, DXGI_FORMAT_R16_SNORM, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16_UINT, 0, 26, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_;
    pd.VS = {g_xr_props_vs, sizeof(g_xr_props_vs)};
    pd.PS = {g_xr_props_ps, sizeof(g_xr_props_ps)};
    pd.InputLayout = {il, UINT(sizeof il / sizeof il[0])};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.IndependentBlendEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // two-sided cards
    pd.RasterizerState.DepthClipEnable = TRUE;                                                                 // the near plane: c >= kNearC
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;                                            // reversed Z: depth = kNearC / c
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 2; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    auto createPso = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(dev_, kDevCreateGraphicsPso);
    if (!createPso || FAILED(createPso(dev_, &pd, IID_PPV_ARGS(&pso_))) || !pso_) { pso_ = nullptr; rel(root_); return false; }
    pipeline_dead_ = false;
    return true;
}

bool PropsGpu::ensure_static(const acz::props::Runtime& rt) {
    if (vb_ && static_rt_ == &rt && static_gen_ == rt.generation) return true;
    if (static_dead_ && static_rt_ == &rt && static_gen_ == rt.generation) return false;
    release_static();
    static_rt_ = &rt; static_gen_ = rt.generation;
    auto dead = [&]() { const auto* r = static_rt_; const uint64_t g = static_gen_; release_static(); static_rt_ = r; static_gen_ = g; static_dead_ = true; return false; };
    const acz::props::PropSet& s = rt.set;
    if (s.verts.empty() || s.idx.empty() || s.ntex == 0 || s.texsize == 0 || s.tex.size() != size_t(s.ntex) * s.texsize * s.texsize * 4u) return dead();
    // vertices: indices are model-local (base_vertex is applied by DrawIndexedInstanced)
    vb_bytes_ = uint32_t(s.verts.size() * sizeof(acz::props::Vertex)); ib_bytes_ = uint32_t(s.idx.size() * 4u);
    if (!buffer(dev_, vb_bytes_, vb_) || !write(vb_, s.verts.data(), vb_bytes_)) return dead();
    if (!buffer(dev_, ib_bytes_, ib_) || !write(ib_, s.idx.data(), ib_bytes_)) return dead();
    // texture array + full mip chain (box filter, CPU)
    ntex_ = s.ntex; texsize_ = s.texsize;
    mips_ = 1; while ((texsize_ >> mips_) > 0) ++mips_;
    mip_off_.assign(size_t(ntex_) * mips_, 0); mip_pitch_.assign(mips_, 0);
    UINT64 off = 0;
    for (uint32_t sl = 0; sl < ntex_; ++sl)
        for (uint32_t m = 0; m < mips_; ++m) {
            const uint32_t sz = std::max(1u, texsize_ >> m);
            const uint32_t pitch = (sz * 4u + 255u) & ~255u;
            mip_pitch_[m] = pitch;
            off = (off + 511u) & ~UINT64(511u);
            mip_off_[size_t(sl) * mips_ + m] = off;
            off += UINT64(pitch) * sz;
        }
    if (!buffer(dev_, off, tex_up_)) return dead();
    {
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(tex_up_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(tex_up_, kResourceUnmap);
        void* p = nullptr; const D3D12_RANGE none{0, 0};
        if (!map || !unmap || FAILED(map(tex_up_, 0, &none, &p)) || !p) return dead();
        uint8_t* dst = static_cast<uint8_t*>(p);
        std::vector<uint8_t> cur, nxt;
        for (uint32_t sl = 0; sl < ntex_; ++sl) {
            const size_t bytes0 = size_t(texsize_) * texsize_ * 4u;
            cur.assign(s.tex.begin() + std::ptrdiff_t(sl * bytes0), s.tex.begin() + std::ptrdiff_t((sl + 1) * bytes0));
            uint32_t sz = texsize_;
            for (uint32_t m = 0; m < mips_; ++m) {
                for (uint32_t y = 0; y < sz; ++y) std::memcpy(dst + mip_off_[size_t(sl) * mips_ + m] + UINT64(y) * mip_pitch_[m], &cur[size_t(y) * sz * 4u], size_t(sz) * 4u);
                if (sz == 1) break;
                const uint32_t nz = sz / 2;
                nxt.assign(size_t(nz) * nz * 4u, 0);
                for (uint32_t y = 0; y < nz; ++y) for (uint32_t x = 0; x < nz; ++x) for (int c = 0; c < 4; ++c) {
                    const unsigned v = cur[((2 * y) * sz + 2 * x) * 4 + c] + cur[((2 * y) * sz + 2 * x + 1) * 4 + c] + cur[((2 * y + 1) * sz + 2 * x) * 4 + c] + cur[((2 * y + 1) * sz + 2 * x + 1) * 4 + c];
                    nxt[(size_t(y) * nz + x) * 4 + c] = uint8_t((v + 2) / 4);
                }
                cur.swap(nxt); sz = nz;
            }
        }
        unmap(tex_up_, 0, nullptr);
    }
    auto create = native<CreateCommittedFn>(dev_, kDevCreateCommittedResource);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = texsize_; td.Height = texsize_;
    td.DepthOrArraySize = UINT16(ntex_); td.MipLevels = UINT16(mips_); td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    if (!create || FAILED(create(dev_, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_))) || !tex_) { tex_ = nullptr; return dead(); }
    auto createHeap = native<CreateHeapFn>(dev_, kDevCreateHeap);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!createHeap || FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&srv_))) || !srv_) { srv_ = nullptr; return dead(); }
    auto createSrv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateSrv);
    if (!createSrv) return dead();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2DArray.MipLevels = mips_; sd.Texture2DArray.ArraySize = ntex_;
    createSrv(dev_, tex_, &sd, srv_->GetCPUDescriptorHandleForHeapStart());
    upload_pending_ = true; static_dead_ = false;
    return true;
}

bool PropsGpu::ensure_targets(const PropsFrame& fr) {
    if (rtv_ && tw_[0] == fr.w[0] && th_[0] == fr.h[0] && tw_[1] == fr.w[1] && th_[1] == fr.h[1]) return true;
    release_targets();
    auto create = native<CreateCommittedFn>(dev_, kDevCreateCommittedResource);
    auto createHeap = native<CreateHeapFn>(dev_, kDevCreateHeap);
    auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(dev_, kDevIncrement);
    auto createRtv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateRtv);
    auto createDsv = native<void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)>(dev_, kDevCreateDsv);
    if (!create || !createHeap || !increment || !createRtv || !createDsv) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 4;
    if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&rtv_))) || !rtv_) { rtv_ = nullptr; return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    if (FAILED(createHeap(dev_, &hd, IID_PPV_ARGS(&dsv_))) || !dsv_) { dsv_ = nullptr; release_targets(); return false; }
    const UINT step = increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE r0 = rtv_->GetCPUDescriptorHandleForHeapStart();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    uint32_t mw = 1, mh = 1;
    for (unsigned e = 0; e < 2; ++e) {
        if (fr.w[e] == 0 || fr.h[e] == 0 || fr.w[e] > 16384 || fr.h[e] > 16384) { release_targets(); return false; }
        mw = std::max(mw, fr.w[e]); mh = std::max(mh, fr.h[e]);
        for (int k = 0; k < 2; ++k) {
            D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = fr.w[e]; rd.Height = fr.h[e]; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = k == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{}; cv.Format = rd.Format;
            ID3D12Resource*& r = k == 0 ? color_[e] : dist_[e];
            if (FAILED(create(dev_, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&r))) || !r) { r = nullptr; release_targets(); return false; }
            D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = rd.Format; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            createRtv(dev_, r, &rv, {r0.ptr + SIZE_T(e * 2 + k) * step});
        }
        tw_[e] = fr.w[e]; th_[e] = fr.h[e];
    }
    D3D12_RESOURCE_DESC dd{}; dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width = mw; dd.Height = mh; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_CLEAR_VALUE dc{}; dc.Format = DXGI_FORMAT_D32_FLOAT; dc.DepthStencil.Depth = 0.0f;
    if (FAILED(create(dev_, &hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&depth_))) || !depth_) { depth_ = nullptr; release_targets(); return false; }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    createDsv(dev_, depth_, &dv, dsv_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool PropsGpu::ensure_instances(size_t n) {
    if (inst_ && n <= inst_cap_) return true;
    rel(inst_); inst_cap_ = 0;
    const uint32_t cap = uint32_t(std::max<size_t>(n, 1024));
    if (!buffer(dev_, UINT64(cap) * sizeof(acz::props::DrawInst), inst_)) { inst_ = nullptr; return false; }
    inst_cap_ = cap;
    return true;
}

bool PropsGpu::prepare(const PropsFrame& fr) {
    inst_count_ = 0;
    if (!dev_ || !fr.rt || !fr.sel || fr.sel->inst.empty()) return false;
    if (async_running()) join_worker();   // a synchronous job (props_async false) after an async start: the worker owns the members until it is done
    if (!ensure_pipeline() || !ensure_static(*fr.rt) || !ensure_targets(fr) || !ensure_instances(fr.sel->inst.size())) return false;
    if (!write(inst_, fr.sel->inst.data(), fr.sel->inst.size() * sizeof(acz::props::DrawInst))) return false;
    inst_count_ = uint32_t(fr.sel->inst.size());
    return true;
}

void PropsGpu::record(ID3D12GraphicsCommandList* list, const PropsFrame& fr, PropsFacts& f) {
    if (!list || !inst_count_ || !pso_ || !vb_) return;
    auto barrier = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(list, kListBarrier);
    auto copyTex = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>(list, kListCopyTexture);
    auto targets = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>(list, kListTargets);
    auto clear = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)>(list, kListClear);
    auto clearDepth = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>(list, kListClearDepth);
    auto viewports = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(list, kListViewports);
    auto scissors = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(list, kListScissors);
    auto setPso = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list, kListSetPso);
    auto setRoot = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(list, kListRoot);
    auto heaps = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(list, kListHeaps);
    auto table = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(list, kListTable);
    auto constants = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(list, kListConstants);
    auto topology = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(list, kListTopology);
    auto ib = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*)>(list, kListIndexBuffer);
    auto vbs = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*)>(list, kListVertexBuffers);
    auto drawIndexed = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT)>(list, kListDrawIndexed);
    if (!barrier || !copyTex || !targets || !clear || !clearDepth || !viewports || !scissors || !setPso || !setRoot || !heaps || !table || !constants ||
        !topology || !ib || !vbs || !drawIndexed) return;
    auto tb = [](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b; return x;
    };
    if (upload_pending_) {
        for (uint32_t sl = 0; sl < ntex_; ++sl)
            for (uint32_t m = 0; m < mips_; ++m) {
                const uint32_t sz = std::max(1u, texsize_ >> m);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = tex_; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = m + sl * mips_;
                src.pResource = tex_up_; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = mip_off_[size_t(sl) * mips_ + m];
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                src.PlacedFootprint.Footprint.Width = sz; src.PlacedFootprint.Footprint.Height = sz; src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = mip_pitch_[m];
                copyTex(list, &dst, 0, 0, 0, &src, nullptr);
            }
        const D3D12_RESOURCE_BARRIER b = tb(tex_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(list, 1, &b);
        upload_pending_ = false; f.uploaded = true;   // tex_up_ stays alive until the static set is released (never reused for writing)
    }
    D3D12_RESOURCE_BARRIER to_rt[4], to_srv[4];
    for (unsigned e = 0; e < 2; ++e) {
        to_rt[e * 2] = tb(color_[e], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        to_rt[e * 2 + 1] = tb(dist_[e], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        to_srv[e * 2] = tb(color_[e], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        to_srv[e * 2 + 1] = tb(dist_[e], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    barrier(list, 4, to_rt);
    setRoot(list, root_);
    setPso(list, pso_);
    ID3D12DescriptorHeap* hs[] = {srv_};
    heaps(list, 1, hs);
    table(list, 1, srv_->GetGPUDescriptorHandleForHeapStart());
    topology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vv[2] = {{gpu_va(vb_), vb_bytes_, UINT(sizeof(acz::props::Vertex))},
                                           {gpu_va(inst_), UINT(inst_count_ * sizeof(acz::props::DrawInst)), UINT(sizeof(acz::props::DrawInst))}};
    vbs(list, 0, 2, vv);
    const D3D12_INDEX_BUFFER_VIEW iv{gpu_va(ib_), ib_bytes_, DXGI_FORMAT_R32_UINT};
    ib(list, &iv);
    const D3D12_CPU_DESCRIPTOR_HANDLE r0 = rtv_->GetCPUDescriptorHandleForHeapStart();
    auto increment = native<UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE)>(dev_, kDevIncrement);
    const UINT step = increment ? increment(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_->GetCPUDescriptorHandleForHeapStart();
    static const FLOAT zero[4] = {0, 0, 0, 0};
    const acz::props::PropSet& s = fr.rt->set;
    for (unsigned e = 0; e < 2; ++e) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rt[2] = {{r0.ptr + SIZE_T(e * 2) * step}, {r0.ptr + SIZE_T(e * 2 + 1) * step}};
        targets(list, 2, rt, FALSE, &dsv);
        clear(list, rt[0], zero, 0, nullptr); clear(list, rt[1], zero, 0, nullptr);
        clearDepth(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, FLOAT(tw_[e]), FLOAT(th_[e]), 0.0f, 1.0f};
        const D3D12_RECT sc{0, 0, LONG(tw_[e]), LONG(th_[e])};
        viewports(list, 1, &vp); scissors(list, 1, &sc);
        float c[kPassConstants] = {};
        std::memcpy(c, fr.clip[e], 16 * sizeof(float));
        c[16] = fr.eye[e][0]; c[17] = fr.eye[e][1]; c[18] = fr.eye[e][2];
        constants(list, 0, kPassConstants, c, 0);
        for (const acz::props::Batch& b : fr.sel->batches) {
            const acz::props::Model& m = s.models[b.model];
            if (!m.index_count || !b.count) continue;
            drawIndexed(list, m.index_count, b.count, m.first_index, INT(m.base_vertex), b.first);
            ++f.draws;
        }
    }
    barrier(list, 4, to_srv);
    f.used = true; f.instances = inst_count_; f.batches = uint32_t(fr.sel->batches.size());
}

} // namespace acz::xr_stereo
