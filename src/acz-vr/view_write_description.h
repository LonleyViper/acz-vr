#pragma once
#include <d3d12.h>
#include <cstdio>
#include <cstring>

namespace acz::diagnostics {
// Immutable descriptive facts, never an eligibility/lifetime certificate.
struct ViewWriteDescription {
    D3D12_RESOURCE_DESC desc{};
    uint64_t address=0; // opaque internal address only; NEVER serialized
    unsigned observed=0,ordinal=0,dimension=0,reasons=0;
    enum Reason : unsigned { wrong_dimension=1, format=2, extent=4, array=8,
        mips=16, samples=32, layout=64, flags=128 };
    static ViewWriteDescription read(bool enabled,ID3D12Resource* resource) noexcept {
        ViewWriteDescription v;
        if(!enabled||!resource)return v;
        // One public API getter. No SEH getter precedent in this codebase.
        v.desc=resource->GetDesc();v.address=reinterpret_cast<uintptr_t>(resource);v.observed=1;
        const auto& d=v.desc;
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)v.reasons|=wrong_dimension;
        if(d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&
           d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)v.reasons|=format;
        if(!d.Width||!d.Height)v.reasons|=extent;
        if(d.DepthOrArraySize!=1)v.reasons|=array;
        if(!d.MipLevels)v.reasons|=mips;
        if(d.SampleDesc.Count!=1||d.SampleDesc.Quality)v.reasons|=samples;
        if(d.Layout!=D3D12_TEXTURE_LAYOUT_UNKNOWN)v.reasons|=layout;
        if(unsigned(d.Flags)&~unsigned(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))v.reasons|=flags;
        if(!v.reasons)v.dimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        return v;
    }
    void append(char* text,size_t bytes) const noexcept {
        const auto n=std::strlen(text);if(n>=bytes)return;
        // rt is an observation-local resource tag (write ordinal), never an
        // address hash or a claim of canonical identity across different writes.
        std::snprintf(text+n,bytes-n,
            ";vo=%x;vn=%x;rt=%x;vd=%x;vf=%x;vw=%llx;vh=%x;va=%x;vm=%x;vs=%x;vq=%x;vl=%x;vg=%x;vb=%llx;dd=%x;dv=%x;du=%x;life=0;seam=unknown",
            observed,ordinal,observed?ordinal:0,unsigned(desc.Dimension),unsigned(desc.Format),
            static_cast<unsigned long long>(desc.Width),desc.Height,unsigned(desc.DepthOrArraySize),
            unsigned(desc.MipLevels),desc.SampleDesc.Count,desc.SampleDesc.Quality,unsigned(desc.Layout),
            unsigned(desc.Flags),static_cast<unsigned long long>(desc.Alignment),dimension,
            unsigned(observed&&!reasons),reasons);
    }
};
static_assert(sizeof(D3D12_RESOURCE_DESC)==56); // public x64 ABI includes alignment/padding
static_assert(sizeof(ViewWriteDescription)<=88);
}
