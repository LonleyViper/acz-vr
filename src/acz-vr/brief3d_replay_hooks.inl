// brief3d_replay_hooks.inl -- BRIEF3D live wiring, the PRODUCER's D3D12 glue (brief3d_live.h holds every decision; docs/world-render/brief3d.md 6).
// Included by d3d12_hook.cpp inside acz's anonymous namespace AFTER the step-1/3/5 adapters (step1Native, step3MappedSpan_locked, guardedCpuRead,
// g_step5Pose) and the brief3d capture glue. Off (brief3d:1 not parsed): every entry point returns after ONE relaxed atomic load.
//
// Per accepted map draw (the game's DrawSelMap_v grid / DrawModelIcon_v icon, hookedDrawIndexedInstanced, BEFORE the game's own draw):
//   the facts are copied under the hook-state mutex (root layout, t10 read twice, b10, PSO, the step-3 RS/OM track, the grid VB box when the core
//   asks), the core decides (brief3d_live.h Producer::draw), the two eye copies of the matrix record are written into the slot's span of OUR
//   upload buffer, and the replay is recorded with native vtable calls into the game's list:
//   barrier(slot eye images COMMON -> RENDER_TARGET) [first of the frame: ClearRTV x2 (transparent), ClearDSV x2 (depth 0 = far for
//   GREATER_EQUAL, stencil 0)] { OMSet(eye RT, eye DSV), RSSetViewports/ScissorRects(eye image), SetGraphicsRootShaderResourceView(t10, eye
//   copy), DrawIndexedInstanced(the game's arguments) } x 2, t10 = the game's VA, the game's viewport/scissor/OM restored, barrier(RENDER_TARGET
//   -> COMMON). The PSO, root signature, IA buffers, topology, t11 and b10 stay the game's (no clone PSO: our images have the PSO's formats).
// GPU objects (created once, on the game thread, at the first map draw; never released: process lifetime): 3 slots x 2 eye colour images
// (R16G16B16A16_FLOAT, rest in COMMON), 3 slots x 2 depth images (D32_FLOAT_S8X24_UINT, always DEPTH_WRITE, producer-only), one mapped UPLOAD buffer
// (3 x 192 KiB brief3d spans + 3 x 512 KiB UI3D spans: b3live::kUploadTotal), one fence, RTV/DSV heaps. A failure poisons the producer (named reason); partial objects are retained, never used.
// ExecuteCommandLists: a batch with tracked lists takes the submit mutex, runs the native call, Signals the fence once on that queue, settles.
namespace b3live = acz::brief3d::live;
struct B3Gpu {
    std::atomic<bool> tried{false};
    ID3D12DescriptorHeap* rtvHeap = nullptr; ID3D12DescriptorHeap* dsvHeap = nullptr;
    ID3D12Resource* rt[b3live::kSlots][2] = {}; ID3D12Resource* ds[b3live::kSlots][2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[b3live::kSlots][2] = {}; D3D12_CPU_DESCRIPTOR_HANDLE dsv[b3live::kSlots][2] = {};
    ID3D12Resource* upload = nullptr; uint8_t* uploadPtr = nullptr; uint64_t uploadVa = 0;
    std::atomic<ID3D12Fence*> fence{nullptr};
    std::mutex submit;                 // orders our Signals: one value per Execute that carries replays
    std::vector<uint8_t> vb[2];        // grid VB read scratch (hook-state mutex held)
    uint64_t charged = 0;
    acz::brief3d::latch::ReadCache cache;   // brief3d.md 11 (b3l:1): latched geometry reads (hook-state mutex held)
    acz::brief3d::latch::ColourMemo colours;   // the t11 entries the latched grid VB indexes (hook-state mutex held)
};
B3Gpu& b3Gpu() noexcept { static B3Gpu g; return g; }

// Game thread, once. true = producer().gpu_ready() was called.
__declspec(noinline) bool b3CreateGpu(ID3D12GraphicsCommandList* list) noexcept {
    auto& G = b3Gpu(); auto& P = b3live::producer();
    using GetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, REFIID, void**);
    ID3D12Device* dev = nullptr;
    auto getDevice = reinterpret_cast<GetDeviceFn>(step1Native(list, 7));
    if (!getDevice || FAILED(getDevice(list, IID_PPV_ARGS(&dev))) || !dev) { P.gpu_failed("brief3d-gpu-device"); return false; }
    struct DevRef { ID3D12Device* d; ~DevRef() { using R = ULONG(STDMETHODCALLTYPE*)(IUnknown*); if (auto f = reinterpret_cast<R>(step1Native(d, 2))) f(d); } } devRef{dev};
    const char* why = nullptr;
    if (const char* b = acz::brief3d::budget(b3live::kRtBpp, b3live::kDsBpp, b3live::kEyeW, b3live::kEyeH, b3live::kSlots, G.charged)) { P.gpu_failed(b); return false; }
    acz::ProxyCreationScope scope(false);
    using CreateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, REFIID, void**);
    using HeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    using IncFn = UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE);
    using RtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    using DsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    using FenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
    auto create = reinterpret_cast<CreateFn>(step1Native(dev, 27));
    auto heap = reinterpret_cast<HeapFn>(step1Native(dev, 14));
    auto inc = reinterpret_cast<IncFn>(step1Native(dev, 15));
    auto mkRtv = reinterpret_cast<RtvFn>(step1Native(dev, 20));
    auto mkDsv = reinterpret_cast<DsvFn>(step1Native(dev, 21));
    auto mkFence = reinterpret_cast<FenceFn>(step1Native(dev, 36));
    if (!create || !heap || !inc || !mkRtv || !mkDsv || !mkFence) { P.gpu_failed("brief3d-gpu-device-natives"); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = b3live::kSlots * 2;
    if (FAILED(heap(dev, &hd, IID_PPV_ARGS(&G.rtvHeap))) || !G.rtvHeap) why = "brief3d-gpu-rtv-heap";
    D3D12_DESCRIPTOR_HEAP_DESC dd{}; dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dd.NumDescriptors = b3live::kSlots * 2;
    if (!why && (FAILED(heap(dev, &dd, IID_PPV_ARGS(&G.dsvHeap))) || !G.dsvHeap)) why = "brief3d-gpu-dsv-heap";
    const UINT rs = why ? 0 : inc(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), ds = why ? 0 : inc(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if (!why && (!rs || !ds)) why = "brief3d-gpu-descriptor-step";
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = b3live::kEyeW; td.Height = b3live::kEyeH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    for (unsigned s = 0; s < b3live::kSlots && !why; ++s)
        for (unsigned e = 0; e < 2 && !why; ++e) {
            D3D12_RESOURCE_DESC d = td; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{}; cv.Format = d.Format;   // transparent black (the clear value of every frame)
            if (FAILED(create(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&G.rt[s][e]))) || !G.rt[s][e]) {
                G.rt[s][e] = nullptr; why = "brief3d-gpu-create-rt"; break;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(s * 2 + e) * rs;
            D3D12_RENDER_TARGET_VIEW_DESC v{}; v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            mkRtv(dev, G.rt[s][e], &v, h); G.rtv[s][e] = h;
        }
    for (unsigned s = 0; s < b3live::kSlots && !why; ++s)   // per-slot depth (Astra 6460640585 P2-3): frames never share a depth image
        for (unsigned e = 0; e < 2 && !why; ++e) {
            D3D12_RESOURCE_DESC d = td; d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE cv{}; cv.Format = d.Format; cv.DepthStencil.Depth = 0.0f; cv.DepthStencil.Stencil = 0;
            if (FAILED(create(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&G.ds[s][e]))) || !G.ds[s][e]) {
                G.ds[s][e] = nullptr; why = "brief3d-gpu-create-ds"; break;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = G.dsvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(s * 2 + e) * ds;
            D3D12_DEPTH_STENCIL_VIEW_DESC v{}; v.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            mkDsv(dev, G.ds[s][e], &v, h); G.dsv[s][e] = h;
        }
    if (!why) {
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = b3live::kUploadTotal; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(create(dev, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&G.upload))) || !G.upload) {
            G.upload = nullptr; why = "brief3d-gpu-create-upload";
        } else {
            using VaFn = D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
            using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
            auto va = reinterpret_cast<VaFn>(step1Native(G.upload, 11));
            auto map = reinterpret_cast<MapFn>(step1Native(G.upload, 8));
            const D3D12_RANGE none{0, 0}; void* p = nullptr;
            G.uploadVa = va ? va(G.upload) : 0;
            if (!map || FAILED(map(G.upload, 0, &none, &p)) || !p || !G.uploadVa || G.uploadVa % 256) why = "brief3d-gpu-upload-map";
            else { G.uploadPtr = static_cast<uint8_t*>(p); std::memset(G.uploadPtr, 0, size_t(bd.Width)); }
        }
    }
    ID3D12Fence* f = nullptr;
    if (!why && (FAILED(mkFence(dev, 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) || !f)) why = "brief3d-gpu-fence";
    if (why) { P.gpu_failed(why); return false; }   // partial objects retained (process lifetime), never used
    G.fence.store(f, std::memory_order_release);
    void* rt[b3live::kSlots][2];
    for (unsigned s = 0; s < b3live::kSlots; ++s) for (unsigned e = 0; e < 2; ++e) rt[s][e] = G.rt[s][e];
    P.set_handles(rt, f);
    P.gpu_ready(G.uploadVa);   // last
    return true;
}
acz::vscreen::Completion b3Completion() noexcept {
    ID3D12Fence* f = b3Gpu().fence.load(std::memory_order_acquire);
    using Fn = UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*);
    auto fn = f ? reinterpret_cast<Fn>(step1Native(f, 8)) : nullptr;
    return acz::vscreen::completion_from_raw(fn ? fn(f) : 0);
}

// Hook-state mutex held: the facts of one map draw (the b3capDraw_locked readers) + the restore state. false = not a map draw.
struct B3Restore {
    D3D12_VIEWPORT vp{}; D3D12_RECT sc{};
    UINT omCount = 0; BOOL omSingle = FALSE; std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> omRtv{}; bool omDsv = false; D3D12_CPU_DESCRIPTOR_HANDLE omDsvHandle{};
};
__declspec(noinline) bool b3Facts_locked(const ListState& ls, const PipelineRecord& p, b3live::DrawFacts& f, B3Restore& r, bool wantBox,
                                         acz::brief3d::Box& box, const char*& boxWhy, bool& boxRead, acz::brief3d::RootPlan* planOut = nullptr) noexcept {
    namespace b3 = acz::brief3d;
    f.kind = b3::classify(p.brVs8, p.brPs8, p.topology_type, true);
    if (f.kind == b3::Kind::none) return false;
    f.ps8 = p.brPs8;
    const auto& b = ls.bindings;
    b3::RootPlan plan; const char* rootWhy = nullptr;
    if (!b.tracking_complete || b.tracking_gap) rootWhy = "b3-binding-tracking-gap";
    else if (!b.root || !b.root->valid()) rootWhy = "b3-root-layout-unknown";
    else if (b.root->layout().parameters.size() > 64) rootWhy = "b3-root-over-64";
    else {
        const auto& L = b.root->layout(); b3::RootParam v[64]; const unsigned n = unsigned(L.parameters.size());
        for (unsigned i = 0; i < n; ++i) { const auto& q = L.parameters[i]; v[i] = b3::RootParam{uint32_t(q.kind), uint32_t(q.visibility), q.shader_register, q.space, q.constant_count}; }
        rootWhy = b3::root_layout_check(f.kind, v, n, L.flags, plan);
    }
    f.root_why = rootWhy;
    if (planOut) *planOut = plan;
    auto& s = state();
    if (!rootWhy) {
        f.t10_param = plan.t10; f.b10_param = plan.b10;
        const auto t = b.roots.find(plan.t10);
        if (t == b.roots.end() || t->second.kind != metadata::ParameterKind::srv || !t->second.value) f.t10_why = "b3-t10-unbound";
        else {
            f.t10_va = t->second.value;
            const unsigned bytes = b3::srt_bytes(f.kind);
            const uint64_t address = s.nativeBindings.diagnostic_address(f.t10_va, bytes, ls.native_binding_device);
            uint8_t scratch[acz::b3cap::kRecordCap];
            f.t10_why = acz::b3cap::read_twice([](uint64_t a, uint64_t v, uint64_t n, const uint8_t*& out) { return step3MappedSpan_locked(a, v, n, out); },
                                               [](const uint8_t* src, uint8_t* dst, size_t n) { return guardedCpuRead(src, dst, n); }, address, f.t10_va, bytes, f.rec, scratch);
        }
        const auto c = b.roots.find(plan.b10);
        if (c == b.roots.end() || c->second.kind != metadata::ParameterKind::constants || c->second.constants.size() < 2 || c->second.written.size() < 2)
            f.b10_why = "b3-b10-unwritten";
        else { const bool w[2] = {bool(c->second.written[0]), bool(c->second.written[1])}; f.b10_why = acz::b3cap::b10_pair(c->second.constants.data(), w, 2, f.b10); }
    }
    const auto& o = p.output;
    f.pso_known = o.known; f.rt_count = o.rt_count; f.rtv0 = o.rtv[0]; f.dsv = o.dsv; f.samples = o.samples;
    f.depth_enable = o.depth_enable; f.depth_write = o.depth_write; f.depth_func = o.depth_func; f.stencil = o.stencil;
    f.stencil_always = o.front_func == gpu_copy::step3::cmp::always && o.back_func == gpu_copy::step3::cmp::always;
    const auto& t = ls.step3;
    f.rs_known = t.known && !t.gap; f.viewports = t.viewports; f.scissors = t.scissors; r.vp = t.vp0; r.sc = t.sc0;
    f.om_known = t.known && !t.gap && t.omSet && t.omCount == 1 && !(t.omSingle && t.omCount > 1);
    r.omCount = t.omCount; r.omSingle = t.omSingle; r.omRtv = t.privOk ? t.privRtv : t.rtv; r.omDsv = t.dsv; r.omDsvHandle = t.privOk ? t.privDsv : t.dsvHandle;
    // the grid VB box (the core asked: no fit latched; at most once per second): the whole VB view, read twice from the pinned mapped UPLOAD span
    if (wantBox && f.kind == b3::Kind::selmap) {
        boxRead = true;
        const auto vb = b.vertices.find(0);
        if (vb == b.vertices.end() || !vb->second.resource || !vb->second.size || !vb->second.stride) boxWhy = "b3-vb-unbound";
        else if (vb->second.size > (4u << 20)) boxWhy = "b3-vb-too-large";
        else {
            const uint64_t va = vb->second.resource, bytes = vb->second.size;
            const uint64_t address = s.nativeBindings.diagnostic_address(va, bytes, ls.native_binding_device);
            const uint8_t* src = nullptr;
            auto& G = b3Gpu();
            if (!address) boxWhy = "b3-vb-address-unresolved";
            else if (const char* why = step3MappedSpan_locked(address, va, bytes, src)) boxWhy = why;
            else {
                G.vb[0].resize(size_t(bytes)); G.vb[1].resize(size_t(bytes));
                if (!guardedCpuRead(src, G.vb[0].data(), size_t(bytes)) || !guardedCpuRead(src, G.vb[1].data(), size_t(bytes))) boxWhy = "b3-vb-read-fault";
                else if (std::memcmp(G.vb[0].data(), G.vb[1].data(), size_t(bytes)) != 0) boxWhy = "b3-vb-torn";
                else boxWhy = b3::box_from_vertices(G.vb[0].data(), size_t(bytes), vb->second.stride, 0, uint32_t(bytes / vb->second.stride), box);
            }
        }
    }
    return true;
}

// brief3d.md 11 (b3l:1), hook-state mutex held: the geometry of one map draw for the latched scene, read from the game's pinned, mapped UPLOAD
// buffers (the reader of t10 and of the grid box: step3MappedSpan_locked + guardedCpuRead): this draw's index range, the vertex range it
// references (slot 0), the SelMap t11 colour table (4 KiB = 256 float4 entries, fmcolor.gim [I]; less when the resource ends earlier) and the
// game's own PSO / root signature references (the proxy's retained shared_ptrs). nullptr = g complete. Nothing is written to the GPU or the list.
__declspec(noinline) const char* b3LatchGeom_locked(const ListState& ls, const PipelineRecord& p, const acz::brief3d::RootPlan& plan, acz::brief3d::Kind kind,
                                                    UINT count, UINT instances, UINT start, INT base, int64_t now, acz::brief3d::latch::Geom& g) noexcept {
    namespace b3 = acz::brief3d; namespace lt = acz::brief3d::latch;
    auto& s = state(); auto& G = b3Gpu();
    const auto& b = ls.bindings;
    if (instances != 1) return "b3l-instances";
    if (!ls.pso || !ls.root) return "b3l-pso-or-root-unknown";
    if (!b.index || !b.index->resource || (b.index->stride != 2 && b.index->stride != 4)) return "b3l-ib-unbound";
    const auto vb = b.vertices.find(0);
    if (vb == b.vertices.end() || !vb->second.resource || !vb->second.size || vb->second.stride < 12) return "b3l-vb-unbound";
    // one span of a mapped upload buffer; `read(off, n, dst)` reads inside it (the WC memory is read only through guardedCpuRead)
    auto cached = [&](uint64_t va, uint64_t bytes, lt::Bytes& out) -> const char* {
        const uint64_t address = s.nativeBindings.diagnostic_address(va, bytes, ls.native_binding_device);
        if (!address) return "b3l-address-unresolved";
        const uint8_t* src = nullptr;
        if (const char* why = step3MappedSpan_locked(address, va, bytes, src)) return why;
        auto read = [&](uint64_t off, uint64_t n, uint8_t* dst) { return off <= bytes && n <= bytes - off && guardedCpuRead(src + off, dst, size_t(n)); };
        return G.cache.get(bytes, now, read, out, va);   // F7: keyed by the source VA too
    };
    const uint32_t isz = b.index->stride;
    const uint64_t ib_off = uint64_t(start) * isz, ib_bytes = uint64_t(count) * isz;
    if (!count || ib_off > b.index->size || ib_bytes > b.index->size - ib_off) return "b3l-ib-range";
    if (const char* why = cached(b.index->resource + ib_off, ib_bytes, g.ib)) return why;
    const uint32_t topo = b.topology, cut = p.strip_cut;
    const bool strip = topo == 3 || topo == 5 || topo == 11 || topo == 13;
    const bool skip_cut = strip && ((isz == 2 && cut == 1) || (isz == 4 && cut == 2));
    uint32_t lo = 0, hi = 0;
    if (!lt::index_range(g.ib->data(), isz, count, skip_cut, lo, hi)) return "b3l-ib-empty";
    const int64_t first = int64_t(lo) + base, last = int64_t(hi) + base;
    const uint32_t stride = vb->second.stride;
    if (first < 0 || uint64_t(last + 1) * stride > vb->second.size) return "b3l-vb-range";
    if (const char* why = cached(vb->second.resource + uint64_t(first) * stride, uint64_t(last - first + 1) * stride, g.vb)) return why;
    g.index_bytes = isz; g.index_count = count; g.stride = stride; g.base_vertex = int32_t(-int64_t(lo)); g.ia_topology = topo; g.strip_cut = cut;
    for (const auto& v : b.vertices) if (v.first != 0 && v.second.resource && v.second.size) ++g.extra_vb;
    if (kind == b3::Kind::selmap) {
        g.t11_param = plan.t11;
        const auto t = b.roots.find(plan.t11);
        if (t == b.roots.end() || t->second.kind != metadata::ParameterKind::srv || !t->second.value) return "b3l-t11-unbound";
        const uint64_t va = t->second.value;
        const uint64_t address = s.nativeBindings.diagnostic_address(va, 16, ls.native_binding_device);
        const auto* r = address ? s.l3.resources.get(address) : nullptr;
        if (!r || !r->description || !r->gpu_va || va < r->gpu_va || va - r->gpu_va >= r->desc.Width) return "b3l-t11-resource";
        const uint64_t avail = (r->desc.Width - (va - r->gpu_va)) / 16 * 16;
        if (!avail) return "b3l-t11-empty";
        // every ColorMap entry the latched vertices index (DrawSelMap_v: ColorMap[int(in.w)], no bounds), not the first 4 KiB only: headset test 7 /
        // step164 runAGU1 showed only the 4 grid rows whose indices were < 256 (brief3d_latch.h colour_entries). Short = layout() refuses the draw.
        // review 2026-11-01 F4: only the vertices the latched index list references (a bad index is refused by layout(): b3l-t11-index)
        const uint64_t entries = G.colours.get(g.vb, g.ib, stride, isz, count, skip_cut, g.base_vertex);
        if (const char* why = cached(va, lt::t11_read_bytes(entries, avail), g.t11)) return why;
    }
    g.pso = ls.pso; g.root = ls.root;   // shared_ptr<void> from the proxy's retained references (their deleters release)
    return nullptr;
}// hookedDrawIndexedInstanced, BEFORE the game's draw (no hook lock held). Off: one relaxed load.
__declspec(noinline) void brief3dDraw(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT start, INT base, UINT startInstance) noexcept {
    auto& P = b3live::producer();
    if (!P.on()) return;
    namespace b3 = acz::brief3d;
    const int64_t now = int64_t(GetTickCount64());
    b3live::DrawFacts f; f.list = self; f.now_ms = now;
    B3Restore r; b3::Box box; const char* boxWhy = nullptr; bool boxRead = false;
    // brief3d.md 11 (b3l:1): declared OUTSIDE the hook-state lock: the game PSO / root references it may hold are released after that lock
    const bool latchOn = P.latch_mode();
    acz::brief3d::latch::Geom geom; const char* geomWhy = "b3l-geom-unread"; acz::brief3d::latch::ReadStats readStats;
    {
        auto& s = state(); std::lock_guard lock(s.mutex);
        const auto it = s.states.find(listKey_locked(self)); if (it == s.states.end()) return;
        const auto p = s.pipelines.find(it->second.pso.get()); if (p == s.pipelines.end()) return;
        const bool wantBox = P.wants_box(now);
        b3::RootPlan plan;
        if (!b3Facts_locked(it->second, p->second, f, r, wantBox, box, boxWhy, boxRead, &plan)) return;
        if (latchOn && !f.root_why && !f.t10_why && !f.b10_why) geomWhy = b3LatchGeom_locked(it->second, p->second, plan, f.kind, count, instances, start, base, now, geom);
        if (latchOn) readStats = b3Gpu().cache.stats();
    }
    auto& G = b3Gpu();
    if (!P.gpu() && !G.tried.exchange(true)) b3CreateGpu(self);   // UI3D uses the producer's slot images (the latched table does not)
    b3live::PoseIn pose;
    if (latchOn || P.wants_poses(now))
        if (auto* pp = g_step5Pose.load()) {
            acz::stereo::EyePose e[2]; uint64_t t = 0; uint32_t gen = 0;
            if (pp->try_read_ex(e, &t, &gen)) {
                pose.ok = true; pose.space_generation = gen;
                for (unsigned k = 0; k < 2; ++k) {
                    for (int i = 0; i < 3; ++i) pose.eyes[k].position[i] = e[k].position[i];
                    for (int i = 0; i < 4; ++i) pose.eyes[k].orientation[i] = e[k].orientation[i];
                    for (int i = 0; i < 4; ++i) pose.eyes[k].fov[i] = e[k].fov[i];
                }
            }
        }
    if (latchOn) {   // nothing is recorded into the game's list: the XR consumer re-draws the latched scene every XR frame
        P.latch_draw(f, pose, boxRead && !boxWhy ? &box : nullptr, boxRead ? boxWhy : nullptr, std::move(geom), geomWhy);
        P.set_read_stats(readStats);
        return;
    }
    using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
    using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
    using OmFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using VpFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
    using ScFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
    using SrvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
    const auto barrier = reinterpret_cast<BarrierFn>(step1Native(self, 26));
    const auto clearRtv = reinterpret_cast<ClearRtvFn>(step1Native(self, 48));
    const auto clearDsv = reinterpret_cast<ClearDsvFn>(step1Native(self, 47));
    const auto om = reinterpret_cast<OmFn>(step1Native(self, 46));
    const auto vps = reinterpret_cast<VpFn>(step1Native(self, 21));
    const auto scs = reinterpret_cast<ScFn>(step1Native(self, 22));
    const auto srv = reinterpret_cast<SrvFn>(step1Native(self, 40));
    const auto draw = reinterpret_cast<DrawFn>(step1Native(self, 13));
    if (!barrier || !clearRtv || !clearDsv || !om || !vps || !scs || !srv || !draw) { P.gpu_failed("brief3d-list-natives"); return; }
    const b3live::Plan pl = P.draw(f, pose, boxRead && !boxWhy ? &box : nullptr, boxRead ? boxWhy : nullptr, b3Completion());
    if (!pl.record) return;   // a granted plan is a recording reservation: it ends with commit() or cancel() below
    if (!G.uploadPtr || !G.rt[pl.slot][0] || !G.rt[pl.slot][1]) { P.cancel(pl); return; }   // nothing recorded (a cancelled clear invalidates the frame)
    for (unsigned e = 0; e < 2; ++e)   // our span: the slot is WRITING (brief3d 9.2: copy c = the icon / its stalk dots, inside the span plan_root_swap checked)
        for (unsigned c = 0; c < pl.copies && c < b3::kMaxIconCopies; ++c)
            std::memcpy(G.uploadPtr + pl.upload_offset[e] + uint64_t(c) * pl.swap.step, pl.eye_rec[e][c], pl.rec_bytes);
    std::atomic_thread_fence(std::memory_order_release);
    ID3D12Resource* const img[2] = {G.rt[pl.slot][0], G.rt[pl.slot][1]};
    D3D12_RESOURCE_BARRIER bar[2] = {};
    for (unsigned e = 0; e < 2; ++e) {
        bar[e].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[e].Transition.pResource = img[e]; bar[e].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[e].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar[e].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    barrier(self, 2, bar);
    static constexpr FLOAT transparent[4] = {0, 0, 0, 0};
    const D3D12_VIEWPORT eyeVp{0.0f, 0.0f, float(b3live::kEyeW), float(b3live::kEyeH), 0.0f, 1.0f};
    const D3D12_RECT eyeSc{0, 0, LONG(b3live::kEyeW), LONG(b3live::kEyeH)};
    b3::Cmd cmds[b3::kMaxCmds];
    const unsigned n = b3::replay_commands(pl.swap, pl.clear, true, cmds);
    for (unsigned i = 0; i < n; ++i) {
        const b3::Cmd& k = cmds[i];
        switch (k.kind) {
        case b3::CmdKind::clear_rt: clearRtv(self, G.rtv[pl.slot][k.eye], transparent, 0, nullptr); break;
        case b3::CmdKind::clear_ds: clearDsv(self, G.dsv[pl.slot][k.eye], D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, pl.clear_depth, 0, 0, nullptr); break;
        case b3::CmdKind::om_eye: om(self, 1, &G.rtv[pl.slot][k.eye], FALSE, &G.dsv[pl.slot][k.eye]); break;
        case b3::CmdKind::vp_eye: vps(self, 1, &eyeVp); break;
        case b3::CmdKind::sc_eye: scs(self, 1, &eyeSc); break;
        case b3::CmdKind::root_srv: srv(self, k.parameter, k.va); break;
        case b3::CmdKind::draw: draw(self, count, instances, start, base, startInstance); break;
        case b3::CmdKind::vp_restore: vps(self, 1, &r.vp); break;
        case b3::CmdKind::sc_restore: scs(self, 1, &r.sc); break;
        case b3::CmdKind::om_restore: om(self, r.omCount, r.omCount ? r.omRtv.data() : nullptr, r.omSingle, r.omDsv ? &r.omDsvHandle : nullptr); break;
        }
    }
    for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    barrier(self, 2, bar);
    P.commit(pl);
}
// hookedReset, after the native Reset. Only a SUCCESSFUL Reset is the retirement witness (the old recording can never be executed again).
inline void brief3dReset(ID3D12GraphicsCommandList* self, HRESULT hr) noexcept {
    auto& P = b3live::producer();
    if (P.configured()) P.on_reset(self, SUCCEEDED(hr));
}
struct B3Exec { b3live::Producer::ExecToken t; std::unique_lock<std::mutex> lock; };
// hookedExecuteCommandLists, right before the native call.
inline void brief3dBeforeExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, B3Exec& x) noexcept {
    auto& P = b3live::producer();
    if (!P.configured() || !lists || !P.tracked_lists()) return;
    x.lock = std::unique_lock<std::mutex>(b3Gpu().submit);
    x.t = P.before_execute(q, reinterpret_cast<const void* const*>(lists), n);
    if (!x.t.signal) x.lock.unlock();
}
// Right after the native call: one Signal of our fence on that queue, then the core settles the touched slots.
inline void brief3dAfterExecute(ID3D12CommandQueue* q, B3Exec& x) noexcept {
    if (!x.t.signal) return;
    using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    ID3D12Fence* f = b3Gpu().fence.load(std::memory_order_acquire);
    const auto signal = reinterpret_cast<SignalFn>(step1Native(q, 14));
    const bool ok = f && signal && SUCCEEDED(signal(q, f, x.t.value));
    b3live::producer().after_execute(x.t, ok);
    x.lock.unlock();
}
