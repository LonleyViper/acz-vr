// fx_puff_atlas_warp_harness.cpp -- EFFECTS-OUTSIDE (M6, fxatlas:1; docs/world-render/effects-outside-window.md section 23) on the Windows WARP adapter:
// the REAL capture pipeline description (fx_puff_atlas_d3d12.h copy_pso_desc) with the REAL embedded fx_puff_atlas.hlsl bytecode, the REAL atlas
// texture / SRV heap descriptions and the uv maths of fx_puff_atlas.h, run against a stand-in of the game's 4caf6e2d root signature (0: root constants
// b0, 1: root constants b1, 2: descriptor table t0, static sampler s0) and pixel shader (fx_puff_atlas_warp_game.hlsl).
//   recording 1 (a frame that binds the card's key): the reference card drawn by the "game" from its texture, then the capture of the card's uv rectangle
//     into a tile (and a decoy texture into the neighbouring tile), exactly the command sequence of step5RecordSprite (barrier, copy PSO under the bound
//     root signature, atlas RTV, atlas viewport / scissor, TRIANGLELIST, capture VB, draw, barrier back, the game's viewport / scissor / topology back).
//   recording 2 (a later frame whose draws bind ANOTHER key, the decoy): the same card drawn from the atlas (our shader-visible heap, the key table pointed at
//     our SRV, uv remapped), then the game's heap and the host key bound back and the host's own card drawn: it must equal the decoy drawn without us.
// Checks: atlas card == reference card (1:1 rectangle: <= 2/255; magnified rectangles: small), the restored host draw == the plain host draw (0), the
// atlas card is not empty and no neighbouring tile bleeds into it. If WARP cannot create a D3D12 device the program prints SKIP and exits 0.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "fx_puff_atlas_d3d12.h"
#include "fx_puff_atlas_vs.h"          // g_fx_atlas_vs (the production capture VS)
#include "fx_puff_atlas_ps.h"          // g_fx_atlas_ps (the production capture PS)
#include "fx_puff_atlas_warp_game_vs.h" // g_warp_game_vs (test stand-in)
#include "fx_puff_atlas_warp_game_ps.h" // g_warp_game_ps

using Microsoft::WRL::ComPtr;
namespace A = acz::fxpuff::atlas;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
#define HR(x) do { const HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s:%d %s hr=%08lx\n", __FILE__, __LINE__, #x, (unsigned long)hr_); return 1; } } while (0)

static constexpr UINT kRtW = 256, kRtH = 256;
struct GameVtx { float x, y; float c[4]; float u, v; };   // POSITION, COLOR0, TEXCOORD0 (stride 32)

struct Gpu {
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence; HANDLE ev = nullptr; UINT64 v = 0;
    bool run() {
        if (FAILED(cl->Close())) return false;
        ID3D12CommandList* l[] = {cl.Get()}; q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++v);
        if (fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, ev); WaitForSingleObject(ev, 60000); }
        if (FAILED(dev->GetDeviceRemovedReason())) return false;
        al->Reset(); cl->Reset(al.Get(), nullptr);
        return true;
    }
};
static ComPtr<ID3D12Resource> buffer(ID3D12Device* d, D3D12_HEAP_TYPE t, UINT64 bytes, D3D12_RESOURCE_STATES s) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r; d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s, nullptr, IID_PPV_ARGS(&r)); return r;
}
static ComPtr<ID3D12Resource> upload(ID3D12Device* d, const void* p, size_t n) {
    auto r = buffer(d, D3D12_HEAP_TYPE_UPLOAD, n, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* m = nullptr; const D3D12_RANGE none{0, 0}; r->Map(0, &none, &m); std::memcpy(m, p, n); r->Unmap(0, nullptr); return r;
}
// An RGBA8 texture from `px` (w x h x 4), left in PIXEL_SHADER_RESOURCE. `keep` holds the staging buffer until the recording ran.
static ComPtr<ID3D12Resource> texture(Gpu& g, UINT w, UINT h, const std::vector<uint8_t>& px, std::vector<ComPtr<ID3D12Resource>>& keep) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> t; g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    std::vector<uint8_t> staged(size_t(total), 0);
    for (UINT y = 0; y < h; ++y) std::memcpy(staged.data() + size_t(y) * fp.Footprint.RowPitch, px.data() + size_t(y) * w * 4, size_t(w) * 4);
    auto up = upload(g.dev.Get(), staged.data(), staged.size()); keep.push_back(up);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = t.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = t.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; g.cl->ResourceBarrier(1, &b);
    return t;
}
struct Rt { ComPtr<ID3D12Resource> r; ComPtr<ID3D12DescriptorHeap> h; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}; };
static Rt target(ID3D12Device* d) {
    Rt t; D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = kRtW; rd.Height = kRtH; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{}; cv.Format = rd.Format;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&t.r));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1; d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.h));
    t.rtv = t.h->GetCPUDescriptorHandleForHeapStart(); d->CreateRenderTargetView(t.r.Get(), nullptr, t.rtv);
    return t;
}
static std::vector<uint8_t> readback(Gpu& g, ID3D12Resource* rt) {
    D3D12_RESOURCE_DESC rd = rt->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = buffer(g.dev.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = rt; b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; g.cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g.cl->ResourceBarrier(1, &b);
    std::vector<uint8_t> out(size_t(kRtW) * kRtH * 4);
    if (!g.run()) return {};
    void* m = nullptr; const D3D12_RANGE all{0, size_t(total)}; rb->Map(0, &all, &m);
    for (UINT y = 0; y < kRtH; ++y) std::memcpy(out.data() + size_t(y) * kRtW * 4, static_cast<uint8_t*>(m) + size_t(y) * fp.Footprint.RowPitch, size_t(kRtW) * 4);
    const D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
    return out;
}
// The card: a quad over clip [-0.8, 0.8] x [-0.6, 0.7] (TRIANGLELIST 0,1,2 / 2,1,3), COLOR0 (0.9, 0.7, 0.5, 0.85), uv the rectangle r (or its atlas map)
static void card(GameVtx out[6], const A::Rect& r, int tile) {
    const float x0 = -0.8f, x1 = 0.8f, y0 = 0.7f, y1 = -0.6f;
    GameVtx c[4] = {{x0, y0, {0.9f, 0.7f, 0.5f, 0.85f}, r.u0, r.v0}, {x1, y0, {0.9f, 0.7f, 0.5f, 0.85f}, r.u1, r.v0},
                    {x0, y1, {0.9f, 0.7f, 0.5f, 0.85f}, r.u0, r.v1}, {x1, y1, {0.9f, 0.7f, 0.5f, 0.85f}, r.u1, r.v1}};
    if (tile >= 0) for (auto& v : c) A::map_uv(unsigned(tile), r, v.u, v.v, v.u, v.v);
    const int o[6] = {0, 1, 2, 2, 1, 3}; for (int k = 0; k < 6; ++k) out[k] = c[o[k]];
}

int main() {
    Gpu g;
    bool debugLayer = false;
    {   // the D3D12 debug layer when installed (Graphics Tools): state / heap / root-signature misuse becomes an error message checked below
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) { dbg->EnableDebugLayer(); debugLayer = true; }
    }
    {
        ComPtr<IDXGIFactory4> f; ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) { std::printf("SKIP: no WARP D3D12 device\n"); return 0; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HR(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)));
        HR(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.al)));
        HR(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.al.Get(), nullptr, IID_PPV_ARGS(&g.cl)));
        HR(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
        g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ID3D12Device* d = g.dev.Get();
    // ---- the "game": two root signatures. L = 0, a static s0 (what M6 assumed); L = 1, ROOT SIGNATURE 310 as live (docs/interception/
    // stored-draw-manifest-1.md 3.2; the step164 null run's atlas-other-table): 0: b0 x4 VS, 1: b1 x4 PS, 2: SRV table t0 PS, 3: SAMPLER table s0 PS,
    // no static samplers (the sampler lives in the game's shader-visible SAMPLER heap). Each with its PSO (alpha blend) and the capture PSO.
    ComPtr<ID3D12RootSignature> rsL[2]; ComPtr<ID3D12PipelineState> gamePsoL[2], copyPsoL[2];
    for (int L = 0; L < 2; ++L) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_DESCRIPTOR_RANGE srange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants = {0, 0, 4}; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[1].Constants = {1, 0, 4}; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable = {1, &range}; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable = {1, &srange}; p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC s{}; s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD = D3D12_FLOAT32_MAX; s.ShaderRegister = 0; s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rd = L == 0 ? D3D12_ROOT_SIGNATURE_DESC{3, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
                                              : D3D12_ROOT_SIGNATURE_DESC{4, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob, err;
        HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
        HR(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rsL[L])));
        const D3D12_INPUT_ELEMENT_DESC il[3] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = A::copy_pso_desc(rsL[L].Get(), 0, g_warp_game_vs, sizeof g_warp_game_vs, g_warp_game_ps, sizeof g_warp_game_ps);
        pd.InputLayout = {il, 3}; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        auto& b = pd.BlendState.RenderTarget[0]; b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        HR(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gamePsoL[L])));
        // THE PRODUCTION DESCRIPTION: the game's root signature + the embedded fx_puff_atlas.hlsl bytecode
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC cp = A::copy_pso_desc(rsL[L].Get(), 0, g_fx_atlas_vs, sizeof g_fx_atlas_vs, g_fx_atlas_ps, sizeof g_fx_atlas_ps);
        HR(d->CreateGraphicsPipelineState(&cp, IID_PPV_ARGS(&copyPsoL[L])));
    }
    // the game's shader-visible SAMPLER heap (layout 1): slot 0 a decoy POINT / WRAP sampler, slot 1 the game's LINEAR / CLAMP one (the table value)
    ComPtr<ID3D12DescriptorHeap> samplerHeap; D3D12_GPU_DESCRIPTOR_HANDLE gameSamplerTable{};
    {
        D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; sh.NumDescriptors = 2; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HR(d->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&samplerHeap)));
        const UINT sinc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        D3D12_SAMPLER_DESC pt{}; pt.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; pt.AddressU = pt.AddressV = pt.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP; pt.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_SAMPLER_DESC ln = pt; ln.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; ln.AddressU = ln.AddressV = ln.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        D3D12_CPU_DESCRIPTOR_HANDLE c0 = samplerHeap->GetCPUDescriptorHandleForHeapStart(); d->CreateSampler(&pt, c0);
        c0.ptr += sinc; d->CreateSampler(&ln, c0);
        gameSamplerTable = samplerHeap->GetGPUDescriptorHandleForHeapStart(); gameSamplerTable.ptr += sinc;
    }
    std::vector<ComPtr<ID3D12Resource>> keep;
    // textures: 0 = noise 128 (1:1), 1 = smooth 64 with an alpha falloff (2x), 2 = a 128 sheet of smooth cells (a 32-texel sub-rectangle, 4x), 3 = decoy
    std::vector<uint8_t> px[4]; const UINT sz[4] = {128, 64, 128, 64};
    {
        unsigned seed = 7u; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
        px[0].resize(128 * 128 * 4); for (size_t i = 0; i < px[0].size(); ++i) px[0][i] = rnd();
        for (int k = 1; k < 4; ++k) {
            const UINT n = sz[k]; px[k].resize(size_t(n) * n * 4);
            for (UINT y = 0; y < n; ++y) for (UINT x = 0; x < n; ++x) {
                const double fx = (x + 0.5) / n, fy = (y + 0.5) / n, rr = std::hypot(fx - 0.5, fy - 0.5);
                uint8_t* p = &px[k][(size_t(y) * n + x) * 4];
                if (k == 3) { p[0] = 20; p[1] = uint8_t(152 + rnd() % 100); p[2] = 40; p[3] = 255; continue; }   // the decoy: green noise (a wrong sampler after the restore shows)
                p[0] = uint8_t(128 + 100 * std::sin(fx * 6.0 + k)); p[1] = uint8_t(128 + 90 * std::cos(fy * 5.0)); p[2] = uint8_t(160 + 60 * std::sin((fx + fy) * 4.0));
                p[3] = uint8_t(255.0 * std::fmax(0.0, 1.0 - 1.6 * rr));   // a soft puff: alpha falls to 0 toward the edge
            }
        }
    }
    ComPtr<ID3D12Resource> tex[4]; for (int k = 0; k < 4; ++k) tex[k] = texture(g, sz[k], sz[k], px[k], keep);
    // the game's shader-visible heap: 6 slots, the textures at 1..4 (the key is never the heap start)
    ComPtr<ID3D12DescriptorHeap> gameHeap;
    D3D12_DESCRIPTOR_HEAP_DESC ghd{}; ghd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; ghd.NumDescriptors = 6; ghd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(d->CreateDescriptorHeap(&ghd, IID_PPV_ARGS(&gameHeap)));
    const UINT inc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gameKey = [&](int k) { D3D12_GPU_DESCRIPTOR_HANDLE h = gameHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(k + 1) * inc; return h; };
    for (int k = 0; k < 4; ++k) { D3D12_CPU_DESCRIPTOR_HANDLE h = gameHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(k + 1) * inc; d->CreateShaderResourceView(tex[k].Get(), nullptr, h); }
    // ---- OUR atlas: the production descriptions
    ComPtr<ID3D12Resource> atlas; ComPtr<ID3D12DescriptorHeap> atlasRtvHeap, ourHeap; D3D12_CPU_DESCRIPTOR_HANDLE atlasRtv{}; D3D12_GPU_DESCRIPTOR_HANDLE ourSrv{};
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; const D3D12_RESOURCE_DESC rd = A::texture_desc();
        D3D12_CLEAR_VALUE cv{}; cv.Format = rd.Format;
        HR(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, A::kBetweenUses, &cv, IID_PPV_ARGS(&atlas)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1; HR(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&atlasRtvHeap)));
        atlasRtv = atlasRtvHeap->GetCPUDescriptorHandleForHeapStart(); d->CreateRenderTargetView(atlas.Get(), nullptr, atlasRtv);
        const D3D12_DESCRIPTOR_HEAP_DESC sh = A::srv_heap_desc(); HR(d->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&ourHeap)));
        const D3D12_SHADER_RESOURCE_VIEW_DESC sd = A::srv_desc();
        for (unsigned k = 0; k < A::kTableMax; ++k) { D3D12_CPU_DESCRIPTOR_HANDLE h = ourHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(k) * inc; d->CreateShaderResourceView(atlas.Get(), &sd, h); }
        ourSrv = ourHeap->GetGPUDescriptorHandleForHeapStart();
    }
    HR(g.cl->Close()); { ID3D12CommandList* l[] = {g.cl.Get()}; g.q->ExecuteCommandLists(1, l); g.q->Signal(g.fence.Get(), ++g.v);
        if (g.fence->GetCompletedValue() < g.v) { g.fence->SetEventOnCompletion(g.v, g.ev); WaitForSingleObject(g.ev, 60000); } g.al->Reset(); g.cl->Reset(g.al.Get(), nullptr); }

    struct Case { const char* name; int tex; A::Rect r; unsigned tile; int tol; };
    const Case cases[3] = {{"1:1 noise 128, whole texture", 0, {0, 0, 1, 1}, 17, 2},
                           {"2x soft puff 64, whole texture", 1, {0, 0, 1, 1}, 18, 3},
                           {"4x sheet cell (32 texels of 128)", 2, {0.25f, 0.5f, 0.5f, 0.75f}, 33, 4}};
    const D3D12_VIEWPORT gvp{0, 0, float(kRtW), float(kRtH), 0, 1}; const D3D12_RECT gsc{0, 0, LONG(kRtW), LONG(kRtH)};
    for (int L = 0; L < 2; ++L)
    for (const Case& cs : cases) {
        ID3D12RootSignature* const rs = rsL[L].Get(); ID3D12PipelineState* const gamePso = gamePsoL[L].Get(); ID3D12PipelineState* const copyPso = copyPsoL[L].Get();
        Rt ref = target(d), viaAtlas = target(d), hostAfter = target(d), hostPlain = target(d);
        GameVtx vRef[6], vAtl[6], vDecoy[6]; card(vRef, cs.r, -1); card(vAtl, cs.r, int(cs.tile)); card(vDecoy, A::Rect{0, 0, 1, 1}, -1);
        auto vbRef = upload(d, vRef, sizeof vRef), vbAtl = upload(d, vAtl, sizeof vAtl), vbDec = upload(d, vDecoy, sizeof vDecoy);
        const D3D12_VERTEX_BUFFER_VIEW vRefV{vbRef->GetGPUVirtualAddress(), sizeof vRef, sizeof(GameVtx)}, vAtlV{vbAtl->GetGPUVirtualAddress(), sizeof vAtl, sizeof(GameVtx)},
                                       vDecV{vbDec->GetGPUVirtualAddress(), sizeof vDecoy, sizeof(GameVtx)};
        A::CopyVtx q[A::kCopyVerts * 2]; A::copy_quad(cs.tile, cs.r, q); A::copy_quad(cs.tile + 1, A::Rect{0, 0, 1, 1}, q + A::kCopyVerts);
        auto vbCap = upload(d, q, sizeof q);
        const D3D12_VERTEX_BUFFER_VIEW capV{vbCap->GetGPUVirtualAddress(), UINT(A::kCopyBytes), A::kCopyStride};
        const D3D12_VERTEX_BUFFER_VIEW capV2{vbCap->GetGPUVirtualAddress() + A::kCopyBytes, UINT(A::kCopyBytes), A::kCopyStride};
        const float zero[4] = {0, 0, 0, 0}; const UINT b1[4] = {1, 0, 0, 0};
        ID3D12DescriptorHeap* const gameHeaps[2] = {gameHeap.Get(), samplerHeap.Get()};
        auto gameState = [&](D3D12_GPU_DESCRIPTOR_HANDLE key) {
            g.cl->SetDescriptorHeaps(L ? 2 : 1, gameHeaps);
            g.cl->SetGraphicsRootSignature(rs); g.cl->SetGraphicsRoot32BitConstants(1, 4, b1, 0); g.cl->SetGraphicsRootDescriptorTable(2, key);
            if (L) g.cl->SetGraphicsRootDescriptorTable(3, gameSamplerTable);
            g.cl->SetPipelineState(gamePso); g.cl->RSSetViewports(1, &gvp); g.cl->RSSetScissorRects(1, &gsc);
            g.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        };
        // ---- recording 1: the game binds the card's key; the reference card; the capture (step5RecordSprite's sequence) + a decoy in the next tile
        gameState(gameKey(cs.tex));
        g.cl->ClearRenderTargetView(ref.rtv, zero, 0, nullptr); g.cl->OMSetRenderTargets(1, &ref.rtv, FALSE, nullptr);
        g.cl->IASetVertexBuffers(0, 1, &vRefV); g.cl->DrawInstanced(6, 1, 0, 0);
        auto captureNow = [&](const D3D12_VERTEX_BUFFER_VIEW& v) {
            const D3D12_RESOURCE_BARRIER toRt = A::transition(atlas.Get(), true), back = A::transition(atlas.Get(), false);
            g.cl->ResourceBarrier(1, &toRt);
            g.cl->SetPipelineState(copyPso);
            g.cl->OMSetRenderTargets(1, &atlasRtv, FALSE, nullptr);
            const D3D12_VIEWPORT av = A::viewport(); const D3D12_RECT ar = A::scissor(); g.cl->RSSetViewports(1, &av); g.cl->RSSetScissorRects(1, &ar);
            g.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.cl->IASetVertexBuffers(0, 1, &v); g.cl->DrawInstanced(A::kCopyVerts, 1, 0, 0);
            g.cl->ResourceBarrier(1, &back);
            g.cl->RSSetViewports(1, &gvp); g.cl->RSSetScissorRects(1, &gsc); g.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.cl->SetPipelineState(gamePso);
        };
        captureNow(capV);
        g.cl->SetGraphicsRootDescriptorTable(2, gameKey(3)); captureNow(capV2);   // the neighbouring tile holds the decoy (bleeding would show green)
        // the plain host draw (decoy key, no atlas) for the restore comparison
        gameState(gameKey(3));
        g.cl->ClearRenderTargetView(hostPlain.rtv, zero, 0, nullptr); g.cl->OMSetRenderTargets(1, &hostPlain.rtv, FALSE, nullptr);
        g.cl->IASetVertexBuffers(0, 1, &vDecV); g.cl->DrawInstanced(6, 1, 0, 0);
        CHECK(g.run(), "recording 1");
        // ---- recording 2: the game binds ONLY the decoy key; our card from the atlas, then the game's heap + host key back and the host's own draw
        gameState(gameKey(3));
        g.cl->ClearRenderTargetView(viaAtlas.rtv, zero, 0, nullptr); g.cl->ClearRenderTargetView(hostAfter.rtv, zero, 0, nullptr);
        g.cl->OMSetRenderTargets(1, &viaAtlas.rtv, FALSE, nullptr);
        // the atlas group: THE PRODUCTION SEQUENCE (fx_puff_atlas_d3d12.h record_group, as gpu_copy_step5_fxsprite.inl records it)
        g.cl->IASetVertexBuffers(0, 1, &vAtlV);
        A::GroupBind gb; gb.ours = ourHeap.Get(); gb.our_srv = ourSrv; gb.ngame = L ? 2u : 1u; gb.game[0] = gameHeap.Get(); gb.game[1] = samplerHeap.Get();
        gb.game_sampler = L ? samplerHeap.Get() : nullptr; gb.key_param = 2; gb.host_key = gameKey(3).ptr;
        gb.ntables = L ? 2u : 1u; gb.tparam[0] = 2; gb.tvalue[0] = gameKey(3).ptr; gb.tparam[1] = 3; gb.tvalue[1] = gameSamplerTable.ptr;
        CHECK(A::group_why(gb) == nullptr, "group_why");
        const unsigned cmds = A::record_group(gb, [&](UINT n, ID3D12DescriptorHeap* const* hs) { g.cl->SetDescriptorHeaps(n, hs); },
            [&](UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h) { g.cl->SetGraphicsRootDescriptorTable(p, h); }, [&] { g.cl->DrawInstanced(6, 1, 0, 0); });
        CHECK(cmds == A::group_commands(gb.ntables), "group commands %u", cmds);
        g.cl->OMSetRenderTargets(1, &hostAfter.rtv, FALSE, nullptr);
        g.cl->IASetVertexBuffers(0, 1, &vDecV); g.cl->DrawInstanced(6, 1, 0, 0);
        CHECK(g.run(), "recording 2");
        const auto a = readback(g, ref.r.Get()), b = readback(g, viaAtlas.r.Get()), c = readback(g, hostAfter.r.Get()), e = readback(g, hostPlain.r.Get());
        CHECK(!a.empty() && a.size() == b.size() && c.size() == e.size() && !c.empty(), "readback");
        if (a.empty() || b.empty() || c.empty() || e.empty()) continue;
        int dmax = 0, restoreMax = 0; size_t covered = 0, green = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
            for (int ch = 0; ch < 4; ++ch) dmax = std::max(dmax, std::abs(int(a[i + ch]) - int(b[i + ch])));
            for (int ch = 0; ch < 4; ++ch) restoreMax = std::max(restoreMax, std::abs(int(c[i + ch]) - int(e[i + ch])));
            covered += b[i + 3] > 8;
            green += b[i + 1] > 150 && b[i] < 60 && b[i + 2] < 80 && a[i + 1] < 120;   // decoy colour where the reference has none
        }
        std::printf("  %s %-34s tile %3u: max |atlas - game| = %d/255 (tolerance %d), covered px %zu, decoy bleed px %zu, restore max |d| = %d\n",
                    L ? "[rs310 sampler table]" : "[static s0]          ", cs.name, cs.tile, dmax, cs.tol, covered, green, restoreMax);
        CHECK(dmax <= cs.tol, "%s: %d", cs.name, dmax);
        CHECK(covered > 5000, "%s: the atlas card drew %zu pixels", cs.name, covered);
        CHECK(green == 0, "%s: %zu decoy pixels", cs.name, green);
        CHECK(restoreMax == 0, "%s: the host draw after the atlas group differs by %d", cs.name, restoreMax);
    }
    CHECK(SUCCEEDED(g.dev->GetDeviceRemovedReason()), "device removed");
    {   // debug-layer verdict: no ERROR / CORRUPTION message (resource states, descriptor heaps, root signature / PSO compatibility)
        ComPtr<ID3D12InfoQueue> iq;
        if (debugLayer && SUCCEEDED(g.dev.As(&iq)) && iq) {
            unsigned errors = 0; const UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0; iq->GetMessage(i, nullptr, &len);
                std::vector<char> buf(len); auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (FAILED(iq->GetMessage(i, m, &len))) continue;
                if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) { ++errors; std::printf("  debug layer: %s\n", m->pDescription); }
            }
            std::printf("  D3D12 debug layer: on, %llu messages, %u errors\n", static_cast<unsigned long long>(n), errors);
            CHECK(errors == 0, "debug-layer errors");
            // negative control: a transition whose StateBefore is wrong (the atlas is in PIXEL_SHADER_RESOURCE) must be reported, else the verdict is void
            const D3D12_RESOURCE_BARRIER wrong = A::transition(atlas.Get(), false);   // claims RENDER_TARGET -> PIXEL_SHADER_RESOURCE
            g.cl->ResourceBarrier(1, &wrong); g.run();
            const UINT64 n2 = iq->GetNumStoredMessages();
            std::printf("  debug-layer negative control (a wrong StateBefore): %s (%llu new messages)\n", n2 > n ? "reported" : "NOT reported: the verdict above is void",
                        static_cast<unsigned long long>(n2 - n));
        } else std::printf("  D3D12 debug layer: not available (Graphics Tools not installed); pixel checks only\n");
    }
    std::printf("fx_puff_atlas_warp: %d checks, %d failed\n", g_checks, g_fail);
    CloseHandle(g.ev);
    return g_fail ? 1 : 0;
}
