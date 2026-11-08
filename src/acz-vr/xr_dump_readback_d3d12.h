#pragma once
// SCREEN-DUMP (xr_screen_dump.h), DLL side: one private READBACK buffer that copies an RGBA8 swapchain image the caller's list just wrote.
// Used by the vscreen copy (xr_vscreen_d3d12.cpp) and the BRIEF3D table blit (xr_brief3d_d3d12.cpp) on the XR owner thread. Every D3D12 call
// goes through acz::nativeDispatch; the buffer is created in a proxy creation scope at the first armed recording and released by take() (after
// the caller observed the recording's fence value) or by release() (the GPU object's destroy(): every submission completed).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "d3d12_hook.h"

namespace acz::xr_stereo::dumprb {

class Readback {
public:
    bool want = false;   // set by readback_want(); consumed by the next record()

    // Inside the caller's open list, right after it wrote `src` (whole subresource 0 in `before`): `src` -> COPY_SOURCE -> copy (w x h) to the
    // buffer -> `after`. false = nothing was recorded (the caller then makes its own before -> after transition).
    bool record(ID3D12Device* dev, ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after, uint32_t w, uint32_t h) noexcept {
        want = false;
        if (!dev || !list || !src || !w || !h || pending_) return false;
        const DXGI_FORMAT f = src->GetDesc().Format;
        if (f != DXGI_FORMAT_R8G8B8A8_UNORM && f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && f != DXGI_FORMAT_R8G8B8A8_TYPELESS) return false;
        auto barrier = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(list, kListBarrier);
        auto copyTex = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
            const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>(list, kListCopyTexture);
        if (!barrier || !copyTex) return false;
        const UINT64 pitch = (UINT64(w) * 4 + 255) / 256 * 256;
        if (!buf_ || pitch * h > size_) {
            release();
            auto create = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)>(dev, kDevCreateCommittedResource);
            if (!create) return false;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            acz::ProxyCreationScope scope(false);
            if (FAILED(create(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf_))) || !buf_) {
                buf_ = nullptr; return false;
            }
            size_ = pitch * h;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src; b.Transition.Subresource = 0;
        b.Transition.StateBefore = before; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(list, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = buf_; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = f; dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = UINT(pitch);
        from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        copyTex(list, &dst, 0, 0, 0, &from, &box);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = after;
        barrier(list, 1, &b);
        pending_ = true; w_ = w; h_ = h; pitch_ = pitch;
        return true;
    }
    // After the caller observed the recording's fence value: the rows (top-down, as stored), then the buffer is released.
    bool take(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) noexcept {
        if (!pending_ || !buf_) return false;
        pending_ = false;
        bool ok = false;
        auto map = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**)>(buf_, kResourceMap);
        auto unmap = native<void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*)>(buf_, kResourceUnmap);
        void* data = nullptr;
        const D3D12_RANGE range{0, SIZE_T(pitch_ * h_)};
        if (map && unmap && SUCCEEDED(map(buf_, 0, &range, &data)) && data) {
            try {
                rgba.assign(size_t(w_) * h_ * 4, 0);
                for (uint32_t y = 0; y < h_; ++y)
                    std::memcpy(rgba.data() + size_t(y) * w_ * 4, static_cast<const uint8_t*>(data) + UINT64(y) * pitch_, size_t(w_) * 4);
                w = w_; h = h_; ok = true;
            } catch (...) { rgba.clear(); ok = false; }
            const D3D12_RANGE none{0, 0}; unmap(buf_, 0, &none);
        }
        release();
        return ok;
    }
    // A given-up (timed-out / lost) readback: ONLY after the caller PROVED its fence value complete (the GPU can no longer write the buffer).
    void discard() noexcept { release(); }
    // The recorded list was never executed (its Close failed): nothing can write the buffer, it may be recorded again.
    void abandon() noexcept { pending_ = false; }
    // Only when no submission that recorded into the buffer can still run (destroy(), or take()).
    void release() noexcept {
        if (buf_) if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(buf_, kRelease)) fn(reinterpret_cast<IUnknown*>(buf_));
        buf_ = nullptr; size_ = 0; pending_ = false;
    }

private:
    enum : int { kRelease = 2, kDevCreateCommittedResource = 27, kListCopyTexture = 16, kListBarrier = 26, kResourceMap = 8, kResourceUnmap = 9 };
    template <class Fn> static Fn native(void* o, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(o, slot)); }
    ID3D12Resource* buf_ = nullptr;
    UINT64 size_ = 0, pitch_ = 0;
    uint32_t w_ = 0, h_ = 0;
    bool pending_ = false;
};

} // namespace acz::xr_stereo::dumprb
