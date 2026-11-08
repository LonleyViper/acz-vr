#pragma once
// WORLD-3 props/trees GPU pass for the XR terrain composite (DLL + WARP harness). Owned by the D3D12 blit backend (xr_stereo_d3d12.cpp) and driven from
// its record(): prepare() before the list opens (resources, instance upload), record() into the backend's open list BEFORE the eye composite. Every
// device/list call goes through acz::nativeDispatch like the backend. Private objects only; no game resource is touched.
// Design (docs/world-render/world3-impl.md): per eye, instanced rasterisation with its own reversed-Z depth into two private eye-sized targets
// (colour RGBA8, ray distance R32F); the terrain composite reads them (t10/t11) and keeps the prop only where the terrain is not in front
// (the same ray march as the terrain, bounded by the prop distance). Targets rest in PIXEL_SHADER_RESOURCE between recordings.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "world_props.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12DescriptorHeap;

namespace acz::xr_stereo {

struct PropsFrame {
    const acz::props::Runtime* rt = nullptr;
    const acz::props::Selection* sel = nullptr;
    uint32_t w[2] = {}, h[2] = {};      // eye viewport sizes (the composite's dst rects)
    float clip[2][16] = {};             // acz::props::make_clip per eye
    float eye[2][3] = {};               // stage-space eye positions (the terrain eyes)
};
struct PropsFacts { bool used = false, uploaded = false; uint32_t instances = 0, batches = 0, draws = 0; };

class PropsGpu {
public:
    explicit PropsGpu(ID3D12Device* d) : dev_(d) {}
    ~PropsGpu() { join_worker(); }   // release() is called by the backend's destroy() after completion; a running init worker is always joined
    PropsGpu(const PropsGpu&) = delete;
    PropsGpu& operator=(const PropsGpu&) = delete;
    // Before the list opens (previous use of every private object completed). false = no props this frame (nothing recorded, bit3 stays clear).
    bool prepare(const PropsFrame&);
    // PERF (BlitJob::props_async): the pipeline (root signature + PSO) and the static set (VB/IB/texture array + staging, SRV heap) of `rt` are
    // built by ONE worker thread (inside a proxy creation scope, device calls only: thread-safe), started by the first call for that runtime/generation.
    // Returns true only once that worker has FINISHED successfully (then prepare() finds everything built); never waits. While it runs, or after it
    // failed, the caller draws no props. A new runtime/generation restarts the worker once the previous one has finished.
    bool async_ready(const acz::props::Runtime& rt);
    bool async_running() const noexcept { return state_.load(std::memory_order_acquire) == kRunning; }
    bool async_failed() const noexcept { return state_.load(std::memory_order_acquire) == kFailed; }
    uint32_t async_init_ms() const noexcept { return init_ms_.load(std::memory_order_acquire); }
    // Into the open list; leaves the targets in PIXEL_SHADER_RESOURCE and the list's root signature/PSO/heaps/IA state CHANGED (the caller re-sets its own).
    void record(ID3D12GraphicsCommandList*, const PropsFrame&, PropsFacts&);
    ID3D12Resource* color(unsigned e) const { return e < 2 ? color_[e] : nullptr; }
    ID3D12Resource* dist(unsigned e) const { return e < 2 ? dist_[e] : nullptr; }
    void release();
private:
    bool ensure_pipeline();
    bool ensure_static(const acz::props::Runtime&);
    bool ensure_targets(const PropsFrame&);
    bool ensure_instances(size_t n);
    void release_static();
    void release_targets();
    void join_worker() noexcept;
    // PERF async init: the worker owns every pipeline/static member below while state_ == kRunning (the owner thread touches none of them then).
    enum : int { kIdle = 0, kRunning = 1, kReady = 2, kFailed = 3 };
    std::thread worker_;
    std::atomic<int> state_{kIdle};
    std::atomic<uint32_t> init_ms_{0};
    const acz::props::Runtime* async_rt_ = nullptr;
    uint64_t async_gen_ = 0;
    ID3D12Device* dev_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    bool pipeline_dead_ = false;
    // static (per runtime generation): vertex + index buffers (UPLOAD heap, read directly), texture array (DEFAULT) + its staging, SRV heap
    ID3D12Resource* vb_ = nullptr;
    ID3D12Resource* ib_ = nullptr;
    ID3D12Resource* tex_ = nullptr;
    ID3D12Resource* tex_up_ = nullptr;
    ID3D12DescriptorHeap* srv_ = nullptr;
    const acz::props::Runtime* static_rt_ = nullptr;
    uint64_t static_gen_ = 0;
    bool static_dead_ = false, upload_pending_ = false;
    uint32_t vb_bytes_ = 0, ib_bytes_ = 0, ntex_ = 0, texsize_ = 0, mips_ = 0;
    std::vector<uint64_t> mip_off_;     // per (slice, mip) offset in tex_up_
    std::vector<uint32_t> mip_pitch_;
    // per frame
    ID3D12Resource* inst_ = nullptr;
    uint32_t inst_cap_ = 0, inst_count_ = 0;
    ID3D12Resource* color_[2] = {};
    ID3D12Resource* dist_[2] = {};
    ID3D12Resource* depth_ = nullptr;
    ID3D12DescriptorHeap* rtv_ = nullptr;
    ID3D12DescriptorHeap* dsv_ = nullptr;
    uint32_t tw_[2] = {}, th_[2] = {};
};

} // namespace acz::xr_stereo
