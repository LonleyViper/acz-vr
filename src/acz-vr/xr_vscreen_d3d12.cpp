// VSCREEN consumer hook, DLL side (xr_vscreen.h; docs/world-render/vscreen.md sections 2 and 7): the D3D12 ScreenGpu and the capture-backed
// ScreenSource for the header-only ScreenCore (xr_vscreen_core.h holds every decision and is tested offline). XR owner thread only.
// Every D3D12 call goes through acz::nativeDispatch on the XR binding queue (the game's DIRECT queue); objects are created in a proxy creation
// scope; the destructor makes no foreign call (destroy() runs only after ScreenCore observed every submission complete).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "xr_vscreen_core.h"
#include "vscreen_capture.h"
#include "d3d12_hook.h"
#include "xr_dump_readback_d3d12.h"   // SCREEN-DUMP

#include <algorithm>
#include <cstdint>
#include <utility>

namespace acz::xr_stereo {
namespace {
enum : int {
    kRelease = 2,
    kQueueExecute = 10, kQueueSignal = 14,
    kDevCreateAllocator = 9, kDevCreateList = 12, kDevCreateFence = 36,
    kListClose = 9, kListReset = 10, kListCopyTexture = 16, kListBarrier = 26,
    kAllocReset = 8, kFenceCompleted = 8,
};
template <class Fn> Fn native(void* o, int slot) noexcept { return reinterpret_cast<Fn>(acz::nativeDispatch(o, slot)); }
template <class T> void rel(T*& p) noexcept {
    if (!p) return;
    if (auto fn = native<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(p, kRelease)) fn(reinterpret_cast<IUnknown*>(p));
    p = nullptr;
}
bool rgba8_family(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

class D3D12ScreenGpu final : public ScreenGpu {
public:
    D3D12ScreenGpu(ID3D12Device* d, ID3D12CommandQueue* q) : dev_(d), q_(q) {}
    bool setup(const SwapchainImages& imgs, const char*& why) override {
        if (!dev_ || !q_ || !imgs.count) { why = "no-device-or-images"; return false; }
        for (uint32_t i = 0; i < imgs.count; ++i) {   // the screen copy needs an R8G8B8A8 swapchain (the consumer selects 29 or 28)
            auto* r = static_cast<ID3D12Resource*>(imgs.images[i]);
            if (!r || !rgba8_family(r->GetDesc().Format)) { why = "swapchain-format-not-rgba8"; return false; }
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
        return true;
    }
    acz::vscreen::Completion completed() override {
        auto fn = fence_ ? native<UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*)>(fence_, kFenceCompleted) : nullptr;
        if (!fn) { acz::vscreen::Completion c; return c; }   // no fence: nothing was ever signalled (value 0)
        return acz::vscreen::completion_from_raw(fn(fence_));   // UINT64_MAX (device removed) -> lost
    }
    Submit copy(unsigned k, void* dstv, const acz::vscreen::CaptureImage& ci, Extent ext) override {
        Submit s;
        auto* dst = static_cast<ID3D12Resource*>(dstv);
        auto* src = static_cast<ID3D12Resource*>(ci.resource);
        auto allocReset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*)>(alloc_[k & 1u], kAllocReset);
        auto reset = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(list_, kListReset);
        auto barrier = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(list_, kListBarrier);
        auto copyTex = native<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
            const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>(list_, kListCopyTexture);
        auto close = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*)>(list_, kListClose);
        auto execute = native<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(q_, kQueueExecute);
        auto signal = native<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64)>(q_, kQueueSignal);
        if (!dst || !src || !allocReset || !reset || !barrier || !copyTex || !close || !execute || !signal) return s;
        if (FAILED(allocReset(alloc_[k & 1u])) || FAILED(reset(list_, alloc_[k & 1u], nullptr))) return s;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst; b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;   // after the wait: RENDER_TARGET
        barrier(list_, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION d{}, sl{};
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        sl.pResource = src; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;   // COMMON -> COPY_SOURCE by implicit promotion
        const D3D12_BOX box{0, 0, 0, std::min(ci.width, ext.w), std::min(ci.height, ext.h), 1};
        copyTex(list_, &d, 0, 0, 0, &sl, &box);
        // SCREEN-DUMP: the image just written (= the one released and submitted) also into the readback buffer (COPY_DEST -> COPY_SOURCE -> RT)
        const bool rb = rb_.want && rb_.record(dev_, list_, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET, ext.w, ext.h);
        if (!rb) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); barrier(list_, 1, &b); }
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
    void destroy() override { rb_.release(); rel(list_); for (auto& a : alloc_) rel(a); rel(fence_); }
private:
    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* q_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    ID3D12CommandAllocator* alloc_[2] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    uint64_t value_ = 0;
    dumprb::Readback rb_;   // SCREEN-DUMP
};

class CaptureSource final : public ScreenSource {
public:
    void request() override { acz::vscreen::capture_request(); }
    void set_extent(uint32_t w, uint32_t h) override { acz::vscreen::capture_set_extent(w, h); }
    bool acquire(uint64_t after, acz::vscreen::CaptureImage& out) override { return acz::vscreen::capture_acquire(after, out); }
    void release(const acz::vscreen::CaptureImage& img) override { acz::vscreen::capture_release(img); }
    std::string describe() override { return acz::vscreen::capture_describe(); }
};
} // namespace

std::shared_ptr<VScreenHook> make_vscreen_hook(const acz::vscreen::Options& o, void* device, void* queue) {
    if (!o.on || !device || !queue) return nullptr;
    acz::vscreen::capture_configure(o);
    return std::make_shared<ScreenCore>(o, std::make_shared<D3D12ScreenGpu>(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue)),
                                        std::make_shared<CaptureSource>());
}

} // namespace acz::xr_stereo
