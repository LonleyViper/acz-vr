#pragma once
// fx_puff_atlas_d3d12.h -- EFFECTS-OUTSIDE (M6, fxatlas:1; docs/world-render/effects-outside-window.md section 23): the D3D12 descriptions of the atlas
// pieces, shared by the proxy (d3d12_hook.cpp: the capture PSO; gpu_copy_step5_hooks.inl: the atlas texture and its SRVs) and the WARP harness
// (tests/acz-vr/fx_puff_atlas_warp_harness.cpp), so the harness executes the same descriptions on the GPU. No calls, descriptions only.
#include "fx_puff_atlas.h"

#include <d3d12.h>
#include <climits>

namespace acz::fxpuff::atlas {

// The capture pipeline: the GAME's root signature (t0 = its key table, s0 = its static sampler), fx_puff_atlas.hlsl vs_copy / ps_copy, RT = the atlas
// format, no blend (the texel is written as sampled), no depth, no culling, TRIANGLELIST quads of CopyVtx (POSITION float2 @0, TEXCOORD float2 @8).
inline const D3D12_INPUT_ELEMENT_DESC* copy_input_layout() noexcept {
    static const D3D12_INPUT_ELEMENT_DESC il[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                   {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    return il;
}
inline D3D12_GRAPHICS_PIPELINE_STATE_DESC copy_pso_desc(ID3D12RootSignature* root, UINT node_mask, const void* vs, size_t vs_bytes, const void* ps,
                                                        size_t ps_bytes) noexcept {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = root;
    cp.VS = {vs, vs_bytes}; cp.PS = {ps, ps_bytes};
    auto& rt = cp.BlendState.RenderTarget[0];
    rt.BlendEnable = FALSE; rt.LogicOpEnable = FALSE; rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_ZERO; rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD; rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    cp.SampleMask = UINT_MAX;
    cp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; cp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; cp.RasterizerState.DepthClipEnable = TRUE;
    auto& ds = cp.DepthStencilState;
    ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; ds.StencilEnable = FALSE;
    ds.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS}; ds.BackFace = ds.FrontFace;
    cp.InputLayout = {copy_input_layout(), 2}; cp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    cp.NumRenderTargets = 1; cp.RTVFormats[0] = DXGI_FORMAT(kFormat); cp.DSVFormat = DXGI_FORMAT_UNKNOWN;
    cp.SampleDesc.Count = 1; cp.NodeMask = node_mask;
    return cp;
}
// The atlas texture: kWidth x kHeight, kFormat, one mip, render target; created in PIXEL_SHADER_RESOURCE (its state between our uses in every list).
inline D3D12_RESOURCE_DESC texture_desc() noexcept {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = kWidth; d.Height = kHeight; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT(kFormat); d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return d;
}
inline constexpr D3D12_RESOURCE_STATES kBetweenUses = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
// Our shader-visible heap: kTableMax descriptors, every one an SRV of the whole atlas.
inline D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc() noexcept {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kTableMax; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return hd;
}
inline D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc() noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT(kFormat); sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    return sd;
}
// The two transitions around a capture.
inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* atlas, bool to_target) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = atlas; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = to_target ? kBetweenUses : D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = to_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : kBetweenUses;
    return b;
}
// (M6b) The atlas group of one host draw (per eye): what is bound and what is set back. host_why admitted only tables that are the key's (SRV) or sampler
// tables; a sampler table keeps the game's SAMPLER heap bound next to ours (D3D12: at most one heap per type) and is set again after each heap change
// ("descriptor table state is undefined after descriptor heaps are changed", Microsoft Learn: Setting and Populating Descriptor Heaps).
struct GroupBind {
    ID3D12DescriptorHeap* ours = nullptr; D3D12_GPU_DESCRIPTOR_HANDLE our_srv{};
    ID3D12DescriptorHeap* game[2] = {}; unsigned ngame = 0;   // the game's bound heaps, in its order
    ID3D12DescriptorHeap* game_sampler = nullptr;              // the one of them that is the SAMPLER heap (null: none bound)
    uint32_t key_param = 0xffffffffu; uint64_t host_key = 0;
    uint32_t tparam[kMaxTables] = {}; uint64_t tvalue[kMaxTables] = {}; unsigned ntables = 0;   // every descriptor table of the root signature + its game value
};
// nullptr = recordable.
inline const char* group_why(const GroupBind& g) noexcept {
    if (!g.ours || !g.our_srv.ptr) return "atlas-not-ready";
    if (!g.ngame || g.ngame > 2) return "atlas-heap-count";
    if (g.key_param == 0xffffffffu || !g.ntables || g.ntables > kMaxTables) return "atlas-key-param";
    bool key = false, samplers = false;
    for (unsigned i = 0; i < g.ntables; ++i) { if (g.tparam[i] == g.key_param) key = true; else samplers = true; }
    if (!key) return "atlas-key-param";
    if (samplers && !g.game_sampler) return "atlas-sampler-heap";
    return nullptr;
}
// Records the group (heaps(n, heaps*), table(param, D3D12_GPU_DESCRIPTOR_HANDLE), draw()); returns the commands recorded. The proxy passes its native vtable
// calls, the WARP harness the list's methods: the same sequence runs in both.
template <class Heaps, class Table, class Draw>
inline unsigned record_group(const GroupBind& g, Heaps&& heaps, Table&& table, Draw&& draw) {
    unsigned n = 0;
    ID3D12DescriptorHeap* const ours[2] = {g.ours, g.game_sampler};
    heaps(g.game_sampler ? 2u : 1u, ours); ++n;
    for (unsigned i = 0; i < g.ntables; ++i) if (g.tparam[i] != g.key_param) { D3D12_GPU_DESCRIPTOR_HANDLE h{}; h.ptr = g.tvalue[i]; table(g.tparam[i], h); ++n; }
    table(g.key_param, g.our_srv); ++n;
    draw(); ++n;
    heaps(g.ngame, g.game); ++n;
    for (unsigned i = 0; i < g.ntables; ++i) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{}; h.ptr = g.tparam[i] == g.key_param ? g.host_key : g.tvalue[i];
        table(g.tparam[i], h); ++n;
    }
    return n;
}
inline D3D12_VIEWPORT viewport() noexcept { return D3D12_VIEWPORT{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f}; }
inline D3D12_RECT scissor() noexcept { return D3D12_RECT{0, 0, LONG(kWidth), LONG(kHeight)}; }

} // namespace acz::fxpuff::atlas
