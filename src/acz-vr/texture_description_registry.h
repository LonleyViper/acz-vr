#pragma once
#include <d3d12.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace acz::diagnostics {
// Historical descriptions keyed by observed output address and device, NOT
// canonical identity or live ownership. No COM calls, pins, eviction or reuse.
// Caller serializes access. Never look up this registry at Draw.
struct TextureDescriptionRegistry {
    static constexpr size_t capacity=1024;
    struct Resolution {
        unsigned hit=0, resolved=0, dimension=0, format=0, mips=0, array_size=0, samples=0;
        unsigned status=0; uint64_t epoch=0;
        bool lifetime_known=false; // no texture alias/retirement certificate exists
        void append(char* text,size_t bytes) const noexcept {
            auto n=std::strlen(text);if(n>=bytes)return;
            std::snprintf(text+n,bytes-n,";rh=%x;rr=%x;rd=%x;rf=%x;rm=%x;ra=%x;rs=%x;rl=%u;ri=%x",
                hit,resolved,dimension,format,mips,array_size,samples,unsigned(lifetime_known),status);
        }
    };
    struct Entry {
        uint64_t address=0,device=0;
        D3D12_RESOURCE_DESC desc{};
        // Zero identities/generations explicitly mean NOT OBSERVED, never current.
        uint64_t device_generation=0,canonical=0,canonical_generation=0,observation=0;
        GUID output_iid{};
        D3D12_HEAP_PROPERTIES properties{};
        unsigned seam=0,heap_type=0,heap_flags=0,status=0;
        bool properties_known=false,output_base=false;
        bool ambiguous=false;
    };
    std::array<Entry,capacity> entries{};
    size_t count=0;
    enum Incomplete : unsigned { lifetime_uncovered=1, identity_unknown=2, unsupported_seam=4,
        heap_unknown=8, alias_unknown=16, callback_overlap=32, capacity_gap=64,
        history_gap=128, allocation_gap=256, unsupported_interface=512 };
    unsigned overflow=0,unknown_views=0,gaps=lifetime_uncovered|identity_unknown|unsupported_interface;
    unsigned exposed=0; // successful observed QI only: Device4=1, Device8=2, Device10=4
    uint64_t epoch=1;
    void invalidate(unsigned reason=history_gap) noexcept {gaps|=reason;if(epoch!=UINT64_MAX)++epoch;}
    void retire(uint64_t address) noexcept {
        bool found=false;
        for(size_t i=0;i<count;++i)if(entries[i].address==address){entries[i].ambiguous=true;found=true;}
        if(found)invalidate(0);
    }
    static void increment(unsigned& n) noexcept {if(n<65535)++n;}
    void creation(bool succeeded,uint64_t address,uint64_t device,const D3D12_RESOURCE_DESC* d,
                  unsigned seam,unsigned heap_type=0,const D3D12_HEAP_PROPERTIES* props=nullptr,
                  unsigned heap_flags=0,const GUID* iid=nullptr,bool overlap=false) noexcept {
        if(!succeeded||!address)return;
        if(!d||seam>2){invalidate(history_gap);return;}
        if(d->Dimension<D3D12_RESOURCE_DIMENSION_TEXTURE1D||
           d->Dimension>D3D12_RESOURCE_DIMENSION_TEXTURE3D)return;
        // A repeated address can be another generation. Keep both historical
        // records and refuse to associate a later view with either generation.
        bool duplicate=false;
        for(size_t i=0;i<count;++i)if(entries[i].address==address) {
            entries[i].ambiguous=true;duplicate=true;
        }
        if(duplicate)invalidate(0);
        if(count==capacity){increment(overflow);invalidate(capacity_gap);return;}
        auto& e=entries[count++];e.address=address;e.device=device;e.desc=*d;
        e.seam=seam;e.heap_type=heap_type;e.ambiguous=duplicate;e.observation=count;
        e.heap_flags=heap_flags;e.properties_known=props!=nullptr;if(props)e.properties=*props;
        if(iid)e.output_iid=*iid;
        e.output_base=iid&&*iid==__uuidof(ID3D12Resource);
        e.status=lifetime_uncovered|identity_unknown;
        if(seam)e.status|=unsupported_seam;
        if(!props)e.status|=heap_unknown;
        if(!e.output_base)e.status|=alias_unknown;
        if(overlap){e.status|=callback_overlap;invalidate(callback_overlap);}
    }
    Resolution view(uint64_t address,uint64_t device,bool null_desc) noexcept {
        Resolution r;r.epoch=epoch;r.status=gaps;
        const Entry* found=nullptr;
        for(size_t i=0;i<count;++i)if(entries[i].address==address&&entries[i].device==device)found=&entries[i];
        if(!found){if(address)increment(unknown_views);return r;}
        r.hit=1;r.status|=found->status;
        if(found->ambiguous)return r;
        const auto& d=found->desc;
        r.format=unsigned(d.Format);r.mips=d.MipLevels;
        r.array_size=d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:d.DepthOrArraySize;
        r.samples=d.SampleDesc.Count;
        // Deliberately narrow documented default SRV shape. Other descriptions
        // remain useful historical facts, without a derived view dimension.
        const bool typed=d.Format==DXGI_FORMAT_R8G8B8A8_UNORM||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
            d.Format==DXGI_FORMAT_B8G8R8A8_UNORM||d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool safe_heap=found->properties_known&&found->properties.Type==D3D12_HEAP_TYPE_DEFAULT&&
            found->properties.CreationNodeMask<=1&&found->properties.VisibleNodeMask<=1&&!found->heap_flags;
        if(null_desc&&!found->seam&&safe_heap&&found->output_base&&
           !(found->status&callback_overlap)&&!(gaps&(capacity_gap|history_gap|allocation_gap|callback_overlap))&&
           d.Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Height&&
           d.DepthOrArraySize==1&&d.MipLevels&&d.SampleDesc.Count==1&&!d.SampleDesc.Quality&&typed&&
           !(unsigned(d.Flags)&~unsigned(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))) {
            r.resolved=1;r.dimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        }
        return r;
    }
    void append(char* text,size_t bytes) const noexcept {
        auto n=std::strlen(text);if(n>=bytes)return;
        std::snprintf(text+n,bytes-n,";tr=%x;tx=%x;uv=%x;ti=%x;cv=7;qe=%x;unpatched=Device4:53,55,Device8:69,70,Device10:76,77,78;shared=32,33:unpatched",
            unsigned(count),overflow,unknown_views,gaps,exposed);
    }
};
}
