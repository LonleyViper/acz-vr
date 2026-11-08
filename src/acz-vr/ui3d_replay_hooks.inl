// ui3d_replay_hooks.inl -- UI3D stage 2 (docs/world-render/ui-3d.md 4 items 2-3), the PRODUCER's D3D12 glue. Every decision is in brief3d_live.h
// (Producer::ui3d_draw) and ui3d.h (classifier, vertex rewrite, eye scissor); this file reads the facts and records the decision.
// Included by d3d12_hook.cpp inside acz's anonymous namespace AFTER brief3d_replay_hooks.inl (b3Gpu, b3CreateGpu, b3Completion, B3Restore).
// Off (ui3d:1 not configured): ui3dDraw returns after ONE relaxed atomic load. Held (a non-ps2_v draw within the last second: flight, movies, the
// briefing overview, the aircraft select): one more relaxed load and GetTickCount64, no lock.
//
// Per accepted ps2_v menu draw (hookedDrawInstanced / hookedDrawIndexedInstanced, BEFORE the game's own draw, no hook lock held):
//   barrier(slot eye images COMMON -> RENDER_TARGET) [first of the frame: ClearRTV x2 (transparent), ClearDSV x2 (depth 0, stencil 0)]
//   { OMSet(eye RT[, eye DSV when the PSO has one]), RSSetViewports(eye image), RSSetScissorRects(the game's scissor carried onto the layer),
//     IASetVertexBuffers(0, our eye copy, stride 64), DrawInstanced(n, 1, c * n, startInstance) for each copy c (1; the selection slab 6) } per eye,
//   IASetVertexBuffers(0, the game's view 0), the game's viewport/scissor/OM restored, barrier(RENDER_TARGET -> COMMON).
// PSO, root signature (cb0 = the game's root constants), textures, blend factor, stencil ref and topology stay the game's: the replay IS the
// game's draw with its 4 vertices moved (x, y, q rewritten; z, colours, texcoords unchanged). Nothing of the game's is written.
// Stage 3a (u3p:1, in-flight pause menu): the same recording for the menu WINDOW of a paused flight frame (the producer decides which draws);
// the only glue change is the hold bypass below. Untested live: nothing of stage 3a ran in the game, SteamVR or a headset.
// Stage 3b (u3a:1|2, the hangar room; ui-3d.md 9; UNTESTED LIVE): a DrawACM_v draw (hookedDrawIndexedInstanced) goes to u3HangarDraw: its facts
// (PSO/RS/OM as above, cb10 root constants, the t10 SrtData record read twice through the pinned mapped UPLOAD span) -> Producer::ui3d_acm; a
// granted plan writes the two eye copies of the record (A replaced) into the slot's UI3D span and records, before the game's draw:
//   barrier(COMMON -> RENDER_TARGET) [first of the frame: clears] { OMSet(eye RT, eye DSV), RSSetViewports/ScissorRects(whole eye image),
//   SetGraphicsRootShaderResourceView(t10, eye copy), DrawIndexedInstanced(the game's arguments) } x 2, t10 = the game's VA, viewport/scissor/OM
//   restored, barrier(RENDER_TARGET -> COMMON)   (brief3d::replay_commands, the BRIEF3D icon pattern). PSO, IA, other roots: the game's.
// DEBRIEF-3D stage 2 (dbr3d:1; debrief-3d-map.md 8; UNTESTED LIVE): while Producer::dbr3d_armed (the debrief window quad seen within 1 s), a
// ps2_v draw is read by dbrFacts_locked (state facts + its own vertex range, or for an indexed draw the index range read twice and the referenced
// vertex range [base + imin, base + imax] read twice) and decided by Producer::dbr3d_draw; a granted plan records the stage-2 sequence above with
// the eye copy bound as VB 0 and, for an indexed draw, DrawIndexedInstanced(the game's index count, 1, the game's start index, -imin,
// startInstance) against the game's still-bound index buffer (the game's PSO strip-cut value and topology); DSV bound when the PSO has one.
// (review 2026-10-31 F2) an eye with a vertex behind it draws its own R16 index list of the primitives in front of it (DbrPlan own_list) from our
// upload span: IASetIndexBuffer(ours) + IASetPrimitiveTopology(the list form) + DrawIndexedInstanced(list_count, 1, 0, 0); the game's index buffer
// view (or none) and topology are restored after the eyes. (F4) the gaps of an indexed copy are filled before the decision (fill_unreferenced).
namespace u3live = acz::brief3d::live;
struct U3Restore { B3Restore om; D3D12_VERTEX_BUFFER_VIEW vb{}; D3D12_INDEX_BUFFER_VIEW ib{}; UINT topology = 0; bool ibKnown = false; };

// Hook-state mutex held: the facts of one draw (+ the restore state). A non-ps2_v draw needs only vs8.
__declspec(noinline) void u3Facts_locked(const ListState& ls, const PipelineRecord& p, bool gsFlag, bool indexed, UINT count, UINT instances, UINT start,
                                         u3live::UiFacts& f, U3Restore& r) noexcept {
    namespace u3 = acz::ui3d;
    f.vs8 = p.brVs8; f.indexed = indexed; f.gs = gsFlag; f.topology_type = p.topology_type; f.vertices = count; f.instances = instances;
    if (f.vs8 != u3::kPs2Vs8) return;
    auto& s = state(); const auto& b = ls.bindings; const auto& t = ls.step3;
    const auto& o = p.output;
    f.pso_known = o.known; f.rt_count = o.rt_count; f.rtv0 = o.rtv[0]; f.dsv = o.dsv; f.samples = o.samples;
    f.depth_enable = o.depth_enable; f.depth_func = o.depth_func; f.stencil = o.stencil;
    f.stencil_always = o.front_func == gpu_copy::step3::cmp::always && o.back_func == gpu_copy::step3::cmp::always;
    f.ps8 = p.brPs8; f.blend = o.blend0; f.blend_src = p.blend_src; f.blend_dst = p.blend_dst; f.depth_write = o.depth_write;   // DEBRIEF-3D window signature
    const bool track = t.known && !t.gap;
    f.rs_known = track; f.viewports = t.viewports; f.scissors = t.scissors;
    f.vp[0] = t.vp0.TopLeftX; f.vp[1] = t.vp0.TopLeftY; f.vp[2] = t.vp0.Width; f.vp[3] = t.vp0.Height;
    f.sc[0] = int32_t(t.sc0.left); f.sc[1] = int32_t(t.sc0.top); f.sc[2] = int32_t(t.sc0.right); f.sc[3] = int32_t(t.sc0.bottom);
    f.om_known = track && t.omSet && t.omCount == 1 && !(t.omSingle && t.omCount > 1);
    r.om.vp = t.vp0; r.om.sc = t.sc0;
    r.om.omCount = t.omCount; r.om.omSingle = t.omSingle; r.om.omRtv = t.privOk ? t.privRtv : t.rtv; r.om.omDsv = t.dsv;
    r.om.omDsvHandle = t.privOk ? t.privDsv : t.dsvHandle;
    if (track && t.omSet && t.omCount >= 1 && t.rtv[0].ptr)   // RT0's resource identity, size and format (the census lookup)
        for (const auto& v : s.step3Views) if (v.handle && v.handle == t.rtv[0].ptr && !v.depth) {
            f.rt = reinterpret_cast<const void*>(v.resource); f.rt_w = uint32_t(v.width); f.rt_h = v.height; f.rt_fmt = v.vd.present ? v.vd.format : v.format; break;
        }
    // VB view 0: the draw's own vertices, read twice through the pinned mapped UPLOAD span (must agree), and the view to restore
    if (const auto vb = b.vertices.find(0); vb == b.vertices.end() || !vb->second.resource) f.vb_why = "ui3d-vb-unbound";
    else {
        r.vb.BufferLocation = vb->second.resource; r.vb.SizeInBytes = UINT(vb->second.size); r.vb.StrideInBytes = UINT(vb->second.stride);
        f.stride = uint32_t(vb->second.stride);
        const uint64_t stride = f.stride, first = uint64_t(start) * stride, bytes = uint64_t(count) * stride;
        if (indexed) f.vb_why = "ui3d-indexed-draw";
        else if (stride != u3::kVertexStride) f.vb_why = "ui3d-vb-stride";
        else if (!count || count > u3::kMaxVerticesPerDraw) f.vb_why = "ui3d-draw-too-many-vertices";
        else if (vb->second.size > UINT32_MAX || first + bytes > vb->second.size || first + bytes < first) f.vb_why = "ui3d-vb-range";
        else {
            const uint64_t va = vb->second.resource + first;
            const uint64_t address = s.nativeBindings.diagnostic_address(va, bytes, ls.native_binding_device);
            const uint8_t* span = nullptr;
            const char* why = !address ? "ui3d-vb-address-unresolved" : step3MappedSpan_locked(address, va, bytes, span);
            if (why) f.vb_why = why;
            else {
                uint8_t again[u3live::kUiVtxBytes];
                if (!guardedCpuRead(span, f.vtx, size_t(bytes)) || !guardedCpuRead(span, again, size_t(bytes))) f.vb_why = "ui3d-vb-read-fault";
                else if (std::memcmp(f.vtx, again, size_t(bytes)) != 0) f.vb_why = "ui3d-vb-torn";
                else f.vb_why = nullptr;
            }
        }
    }
    // cb0 (b0 of the vertex stage): root constants (census: rc0, 4 dwords written [E]) or a root CBV read through the mapped span
    if (!b.tracking_complete || b.tracking_gap) f.cb_why = "ui3d-binding-tracking-gap";
    else if (!b.root || !b.root->valid()) f.cb_why = "ui3d-root-layout-unknown";
    else {
        const auto rs = b.root->resolve(metadata::BindingKind::cbv, 0, 0, metadata::ShaderStage::vertex);
        float cb[4] = {0, 0, 0, 0};
        if (!rs.valid) f.cb_why = "ui3d-cb0-unresolved";
        else if (rs.parameter_kind == metadata::ParameterKind::table) f.cb_why = "ui3d-cb0-in-descriptor-table";
        else if (const auto v = b.roots.find(rs.root_index); v == b.roots.end()) f.cb_why = "ui3d-cb0-unbound";
        else if (rs.parameter_kind == metadata::ParameterKind::constants && v->second.kind == metadata::ParameterKind::constants) {
            unsigned mask = 0;
            for (unsigned i = 0; i < 4; ++i) if (i < v->second.constants.size() && i < v->second.written.size() && v->second.written[i]) {
                std::memcpy(&cb[i], &v->second.constants[i], 4); mask |= 1u << i;
            }
            f.cb_why = mask == 15u ? nullptr : "ui3d-cb0-unwritten";
        } else if (rs.parameter_kind == metadata::ParameterKind::cbv && v->second.kind == metadata::ParameterKind::cbv && v->second.value) {
            const uint64_t address = s.nativeBindings.diagnostic_address(v->second.value, 16, ls.native_binding_device);
            const uint8_t* span = nullptr;
            const char* why = !address ? "ui3d-cb0-address-unresolved" : step3MappedSpan_locked(address, v->second.value, 16, span);
            if (why) f.cb_why = why;
            else if (!guardedCpuRead(span, reinterpret_cast<uint8_t*>(cb), 16)) f.cb_why = "ui3d-cb0-read-fault";
            else f.cb_why = nullptr;
        } else f.cb_why = "ui3d-cb0-unbound";
        if (!f.cb_why) { f.cb0.sx = cb[0]; f.cb0.sy = cb[1]; f.cb0.ox = cb[2]; f.cb0.oy = cb[3]; }
        // 2026-10-30d (u3agm): the state key a remembered hangar glow must share with the draw that carries it: the PSO record and the pixel
        // stage's t0 binding (root value + table offset) and s0 when it is a bound root parameter (a static sampler is part of the root
        // signature, i.e. of the PSO record's identity already). An unresolved / unbound t0 leaves 0 (the draw carries no remembered glow).
        const auto t0 = b.root->resolve(metadata::BindingKind::srv, 0, 0, metadata::ShaderStage::pixel);
        const auto s0 = b.root->resolve(metadata::BindingKind::sampler, 0, 0, metadata::ShaderStage::pixel);
        const auto vt = t0.valid ? b.roots.find(t0.root_index) : b.roots.end();
        const auto vs = s0.valid ? b.roots.find(s0.root_index) : b.roots.end();
        if (vt != b.roots.end() && vt->second.value) {
            uint64_t h = 1469598103934665603ull;   // FNV-1a over the identities
            auto mix = [&h](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xffu; h *= 1099511628211ull; } };
            mix(uint64_t(reinterpret_cast<uintptr_t>(&p))); mix(vt->second.value); mix(uint64_t(t0.table_offset));
            mix(s0.valid ? 1u : 0u);
            if (vs != b.roots.end()) { mix(vs->second.value); mix(uint64_t(s0.table_offset)); }
            f.state_key = h ? h : 1u;
        }
    }
}

// DEBRIEF-3D stage 2 (dbr3d:1; dbr3d.h, debrief-3d-map.md 8): the glue's scratch for one debrief draw (heap, one per recording thread that met a
// debrief draw; never freed before thread exit): the facts, the vertex range read twice, the index range read twice, the two eye copies.
struct DbrScratch {
    u3live::DbrFacts f;
    uint8_t vtx[u3live::kDbrVtxBytes], again[u3live::kDbrVtxBytes], eye[2][u3live::kDbrVtxBytes];
    uint8_t ib[acz::dbr3d::kMaxIndicesPerDraw * 4], ib2[acz::dbr3d::kMaxIndicesPerDraw * 4];
    uint16_t list[2][acz::dbr3d::kMaxListIndices];   // (F2) the per-eye index lists (2 x 48 KiB)
};
// Hook-state mutex held: the state facts (u3Facts_locked) + the draw's vertices: a non-indexed draw's own range, or an indexed draw's referenced
// range [base + imin, base + imax] (index range read twice, equal, scanned with the PSO's strip-cut value). Every read goes through
// step3MappedSpan_locked + guardedCpuRead (pinned mapped UPLOAD spans; a default-heap buffer is unreadable = refused). Read only.
__declspec(noinline) void dbrFacts_locked(const ListState& ls, const PipelineRecord& p, bool gsFlag, bool indexed, UINT count, UINT instances, UINT start,
                                          INT base, DbrScratch& sc, U3Restore& r) noexcept {
    namespace db = acz::dbr3d;
    auto& f = sc.f;
    f.nv = 0; f.vtx = sc.vtx; f.imin = 0; f.index_count = indexed ? count : 0; f.start_index = indexed ? start : 0; f.base = indexed ? base : 0;
    u3Facts_locked(ls, p, gsFlag, indexed, count, instances, start, f.ui, r);   // state, cb0, restore (its own <= 64-vertex read is not used)
    auto& s = state(); const auto& b = ls.bindings;
    // two agreeing guarded copies of [va, va + bytes) through the pinned mapped UPLOAD span
    auto read2 = [&](uint64_t va, uint64_t bytes, uint8_t* one, uint8_t* two, const char* unresolved, const char* fault, const char* torn) -> const char* {
        const uint64_t address = s.nativeBindings.diagnostic_address(va, bytes, ls.native_binding_device);
        const uint8_t* span = nullptr;
        if (!address) return unresolved;
        if (const char* why = step3MappedSpan_locked(address, va, bytes, span)) return why;
        if (!span || !guardedCpuRead(span, one, size_t(bytes)) || !guardedCpuRead(span, two, size_t(bytes))) return fault;
        return std::memcmp(one, two, size_t(bytes)) != 0 ? torn : nullptr;
    };
    // (F2) the IA topology and the game's index buffer view, restored after an eye drew its own index list
    f.ia_topology = b.topology; r.topology = UINT(b.topology);
    if (b.index && b.index->resource && (b.index->stride == 2 || b.index->stride == 4) && b.index->size <= UINT32_MAX) {
        r.ib.BufferLocation = b.index->resource; r.ib.SizeInBytes = UINT(b.index->size);
        r.ib.Format = b.index->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }
    r.ibKnown = !b.index || r.ib.BufferLocation;   // the game's IB state can be put back exactly (none bound, or a view we can re-set)
    const auto vb = b.vertices.find(0);
    if (vb == b.vertices.end() || !vb->second.resource) { f.vtx_why = "ui3d-vb-unbound"; return; }
    const uint64_t stride = vb->second.stride;
    if (stride != db::kVertexStride) { f.vtx_why = "ui3d-vb-stride"; return; }
    uint64_t firstVertex = start, nv = count;
    if (indexed) {
        if (!b.index || !b.index->resource) { f.vtx_why = "dbr3d-ib-unbound"; return; }
        const uint32_t ibs = b.index->stride;
        if (ibs != 2 && ibs != 4) { f.vtx_why = "dbr3d-ib-stride"; return; }
        if (!count || count > db::kMaxIndicesPerDraw) { f.vtx_why = "dbr3d-too-many-indices"; return; }
        const uint64_t first = uint64_t(start) * ibs, bytes = uint64_t(count) * ibs;
        if (first + bytes > b.index->size || first + bytes < first) { f.vtx_why = "dbr3d-ib-range"; return; }
        if (const char* why = read2(b.index->resource + first, bytes, sc.ib, sc.ib2, "dbr3d-ib-address-unresolved", "dbr3d-ib-read-fault", "dbr3d-ib-torn")) {
            f.vtx_why = why; return;
        }
        const db::IndexScan is = db::scan_indices(sc.ib, count, ibs, p.strip_cut);
        if (is.why) { f.vtx_why = is.why; return; }
        const int64_t lo = int64_t(base) + int64_t(is.imin);
        if (lo < 0) { f.vtx_why = "dbr3d-base-vertex"; return; }
        firstVertex = uint64_t(lo); nv = is.span(); f.imin = is.imin;
        f.ib = sc.ib; f.ib_stride = ibs; f.strip_cut = p.strip_cut;
    }
    if (!nv || nv > db::kMaxVerticesPerDraw) { f.vtx_why = "dbr3d-draw-too-many-vertices"; return; }
    const uint64_t vfirst = firstVertex * stride, vbytes = nv * stride;
    if (vb->second.size > UINT32_MAX || vfirst + vbytes > vb->second.size || vfirst + vbytes < vfirst) { f.vtx_why = "ui3d-vb-range"; return; }
    if (const char* why = read2(vb->second.resource + vfirst, vbytes, sc.vtx, sc.again, "ui3d-vb-address-unresolved", "ui3d-vb-read-fault", "ui3d-vb-torn")) {
        f.vtx_why = why; return;
    }
    // (F4) vertices of [imin, imax] no index references: the nearest referenced vertex before them (never fetched; they no longer refuse the frame)
    if (indexed) f.gaps = db::fill_unreferenced(sc.vtx, uint32_t(nv), sc.ib, count, b.index->stride, p.strip_cut, f.imin);
    f.nv = uint32_t(nv); f.vtx_why = nullptr;
}

// Stage 3b: hook-state mutex held: the facts of one DrawACM_v draw (+ the restore state). Read only (the u3cAcm_locked readers).
__declspec(noinline) void u3AcmFacts_locked(const ListState& ls, const PipelineRecord& p, bool gsFlag, bool indexed, UINT instances,
                                            u3live::AcmFacts& f, B3Restore& r) noexcept {
    namespace hz = acz::ui3d::hangar;
    f.indexed = indexed; f.gs = gsFlag; f.topology_type = p.topology_type; f.instances = instances;
    auto& s = state(); const auto& b = ls.bindings; const auto& t = ls.step3;
    const auto& o = p.output;
    f.pso_known = o.known; f.rt_count = o.rt_count; f.rtv0 = o.rtv[0]; f.dsv = o.dsv; f.samples = o.samples;
    f.depth_enable = o.depth_enable; f.depth_write = o.depth_write; f.depth_func = o.depth_func; f.stencil = o.stencil;
    f.stencil_always = o.front_func == gpu_copy::step3::cmp::always && o.back_func == gpu_copy::step3::cmp::always;
    const bool track = t.known && !t.gap;
    f.rs_known = track; f.viewports = t.viewports; f.scissors = t.scissors;
    f.om_known = track && t.omSet && t.omCount == 1 && !(t.omSingle && t.omCount > 1);
    r.vp = t.vp0; r.sc = t.sc0;
    r.omCount = t.omCount; r.omSingle = t.omSingle; r.omRtv = t.privOk ? t.privRtv : t.rtv; r.omDsv = t.dsv; r.omDsvHandle = t.privOk ? t.privDsv : t.dsvHandle;
    if (track && t.omSet && t.omCount >= 1 && t.rtv[0].ptr)
        for (const auto& v : s.step3Views) if (v.handle && v.handle == t.rtv[0].ptr && !v.depth) {
            f.rt = reinterpret_cast<const void*>(v.resource); f.rt_w = uint32_t(v.width); f.rt_h = v.height; f.rt_fmt = v.vd.present ? v.vd.format : v.format; break;
        }
    if (!b.tracking_complete || b.tracking_gap) { f.cb_why = "u3a-binding-tracking-gap"; f.t10_why = "u3a-binding-tracking-gap"; return; }
    if (!b.root || !b.root->valid()) { f.cb_why = "u3a-root-layout-unknown"; f.t10_why = "u3a-root-layout-unknown"; return; }
    // cb10 dword 0 (the fade flag word): root constants of the bound layout, vertex stage then pixel stage (the census rule)
    auto c = b.root->resolve(metadata::BindingKind::cbv, 10, 0, metadata::ShaderStage::vertex);
    if (!c.valid) c = b.root->resolve(metadata::BindingKind::cbv, 10, 0, metadata::ShaderStage::pixel);
    if (!c.valid) f.cb_why = "u3a-cb10-unresolved";
    else if (c.parameter_kind != metadata::ParameterKind::constants) f.cb_why = "u3a-cb10-not-root-constants";
    else if (const auto v = b.roots.find(c.root_index); v == b.roots.end() || v->second.kind != metadata::ParameterKind::constants) f.cb_why = "u3a-cb10-unbound";
    else if (v->second.constants.empty() || v->second.written.empty() || !v->second.written[0]) f.cb_why = "u3a-cb10-unwritten";
    else { f.cb10_0 = uint32_t(v->second.constants[0]); f.cb_why = nullptr; }
    // t10: a ROOT SRV (swappable), its record read twice through the pinned mapped UPLOAD span
    const auto tr = b.root->resolve(metadata::BindingKind::srv, 10, 0, metadata::ShaderStage::vertex);
    if (!tr.valid) { f.t10_why = "u3a-t10-unresolved"; return; }
    if (tr.parameter_kind != metadata::ParameterKind::srv) { f.t10_why = "u3a-t10-not-root-srv"; return; }
    const auto v = b.roots.find(tr.root_index);
    if (v == b.roots.end() || v->second.kind != metadata::ParameterKind::srv || !v->second.value) { f.t10_why = "u3a-t10-unbound"; return; }
    f.t10_param = tr.root_index; f.t10_va = v->second.value;
    const uint64_t address = s.nativeBindings.diagnostic_address(f.t10_va, hz::kRecBytes, ls.native_binding_device);
    const uint8_t* span = nullptr; uint8_t again[hz::kRecBytes];
    const char* why = !address ? "u3a-t10-address-unresolved" : step3MappedSpan_locked(address, f.t10_va, hz::kRecBytes, span);
    if (!why && (!span || !guardedCpuRead(span, f.rec, hz::kRecBytes) || !guardedCpuRead(span, again, hz::kRecBytes))) why = "u3a-t10-read-fault";
    if (!why && std::memcmp(f.rec, again, hz::kRecBytes) != 0) why = "u3a-t10-record-torn";
    f.t10_why = why;
}
// hookedDrawInstanced / hookedDrawIndexedInstanced, BEFORE the game's draw (no hook lock held). Off: one relaxed load.
__declspec(noinline) void ui3dDraw(ID3D12GraphicsCommandList* self, UINT count, UINT instances, UINT start, UINT startInstance, bool indexed, INT base = 0) noexcept {
    auto& P = u3live::producer();
    if (!P.ui3d_on()) return;
    const int64_t now = int64_t(GetTickCount64());
    // UI3D stage 3a (u3p:1; ui-3d.md 7; UNTESTED LIVE): while the pause trigger is live (the consumer's HUD-MENU state, checked at the Present)
    // the flight's hold is ignored and every draw is looked at: the core skips the paused HUD/world and replays only the menu window.
    if (P.ui3d_held(now) && !P.ui3d_pause_live()) return;
    u3live::UiFacts f; f.list = self; f.now_ms = now;
    U3Restore r;
    // stage 3b (u3a:1|2): a DrawACM_v draw is decided by ui3d_acm (the hangar room); the facts struct is large: static per thread
    const bool hangar = P.ui3d_hangar_configured();
    thread_local u3live::AcmFacts af;
    bool acm = false;
    // DEBRIEF-3D stage 2 (dbr3d:1; NOT run live): while the debrief window was seen within dbr3d::kArmMs every ps2_v draw (indexed too) is read
    // with its vertex range into this thread's scratch and decided by dbr3d_draw. Not armed: one relaxed load more than before.
    const bool dbrArmed = P.dbr3d_armed(now);
    thread_local std::unique_ptr<DbrScratch> dbrScratch;
    if (dbrArmed && !dbrScratch) dbrScratch.reset(new (std::nothrow) DbrScratch);   // a failed allocation keeps the stage-2 path (the flat screen)
    bool dbr = false;
    {
        auto& s = state(); std::lock_guard lock(s.mutex);
        const auto it = s.states.find(listKey_locked(self)); if (it == s.states.end()) { P.ui3d_unknown_draw(now, "ui3d-list-unknown"); return; }
        const auto p = s.pipelines.find(it->second.pso.get()); if (p == s.pipelines.end()) { P.ui3d_unknown_draw(now, "ui3d-pso-unknown-lookup"); return; }
        const auto g = s.gs.find(it->second.pso.get());
        if (hangar && p->second.brVs8 == acz::ui3d::kAcmVs8) {
            acm = true; af = u3live::AcmFacts{}; af.list = self; af.now_ms = now;
            u3AcmFacts_locked(it->second, p->second, g != s.gs.end() && g->second, indexed, instances, af, r.om);
        } else if (dbrArmed && dbrScratch && p->second.brVs8 == acz::ui3d::kPs2Vs8) {
            dbr = true; dbrScratch->f = u3live::DbrFacts{}; dbrScratch->f.ui.list = self; dbrScratch->f.ui.now_ms = now;
            dbrFacts_locked(it->second, p->second, g != s.gs.end() && g->second, indexed, count, instances, start, base, *dbrScratch, r);
        } else u3Facts_locked(it->second, p->second, g != s.gs.end() && g->second, indexed, count, instances, start, f, r);
    }
    auto& G = b3Gpu();
    if (dbr) {   // DEBRIEF-3D stage 2: UNTESTED LIVE
        DbrScratch& sc = *dbrScratch;
        if (!P.gpu() && !G.tried.exchange(true)) b3CreateGpu(self);
        u3live::PoseIn pose;
        if (P.wants_poses(now))
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
        using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
        using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
        using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
        using OmFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
        using VpFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
        using ScFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
        using IaFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
        using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
        using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
        using IbFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
        using TopoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
        const auto setIb = reinterpret_cast<IbFn>(step1Native(self, 43));
        const auto topo = reinterpret_cast<TopoFn>(step1Native(self, 20));
        const auto barrier = reinterpret_cast<BarrierFn>(step1Native(self, 26));
        const auto clearRtv = reinterpret_cast<ClearRtvFn>(step1Native(self, 48));
        const auto clearDsv = reinterpret_cast<ClearDsvFn>(step1Native(self, 47));
        const auto om = reinterpret_cast<OmFn>(step1Native(self, 46));
        const auto vps = reinterpret_cast<VpFn>(step1Native(self, 21));
        const auto scs = reinterpret_cast<ScFn>(step1Native(self, 22));
        const auto ia = reinterpret_cast<IaFn>(step1Native(self, 44));
        const auto draw = reinterpret_cast<DrawFn>(step1Native(self, 12));
        const auto drawIdx = reinterpret_cast<DrawIdxFn>(step1Native(self, 13));
        if (!barrier || !clearRtv || !clearDsv || !om || !vps || !scs || !ia || !draw || !drawIdx || !setIb || !topo) { P.gpu_failed("dbr3d-list-natives"); return; }
        uint8_t* const dst[2] = {sc.eye[0], sc.eye[1]};
        uint16_t* const lists[2] = {sc.list[0], sc.list[1]};
        const u3live::DbrPlan pl = P.dbr3d_draw(sc.f, pose, b3Completion(), dst, r.ibKnown ? lists : nullptr);   // no lists: whole-draw cull (as before)
        if (!pl.record) return;   // a granted plan is a recording reservation: it ends with dbr3d_commit() or dbr3d_cancel() below
        if (!G.uploadPtr || !G.uploadVa || !G.rt[pl.slot][0] || !G.rt[pl.slot][1] || !G.ds[pl.slot][0] || !G.ds[pl.slot][1] || !r.vb.BufferLocation) {
            P.dbr3d_cancel(pl); return;
        }
        const uint32_t bytes = pl.nv * acz::dbr3d::kVertexStride;
        for (unsigned e = 0; e < 2; ++e) {   // our span of the WRITING slot (the core checked it against dbr3d::kUploadPerSlot, the lists included)
            if (pl.eye_draw[e]) std::memcpy(G.uploadPtr + pl.upload_offset[e], sc.eye[e], bytes);
            if (pl.eye_draw[e] && pl.own_list[e]) std::memcpy(G.uploadPtr + pl.list_offset[e], sc.list[e], size_t(pl.list_count[e]) * 2);
        }
        bool ownList = false;
        std::atomic_thread_fence(std::memory_order_release);
        ID3D12Resource* const img[2] = {G.rt[pl.slot][0], G.rt[pl.slot][1]};
        D3D12_RESOURCE_BARRIER bar[2] = {};
        for (unsigned e = 0; e < 2; ++e) {
            bar[e].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[e].Transition.pResource = img[e]; bar[e].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar[e].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar[e].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        barrier(self, 2, bar);
        if (pl.clear) {
            static constexpr FLOAT transparent[4] = {0, 0, 0, 0};
            for (unsigned e = 0; e < 2; ++e) {
                clearRtv(self, G.rtv[pl.slot][e], transparent, 0, nullptr);
                clearDsv(self, G.dsv[pl.slot][e], D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, pl.clear_depth, 0, 0, nullptr);
            }
        }
        const D3D12_VIEWPORT eyeVp{0.0f, 0.0f, float(u3live::kEyeW), float(u3live::kEyeH), 0.0f, 1.0f};
        for (unsigned e = 0; e < 2; ++e) {
            if (!pl.eye_draw[e]) continue;
            om(self, 1, &G.rtv[pl.slot][e], FALSE, pl.bind_dsv ? &G.dsv[pl.slot][e] : nullptr);
            vps(self, 1, &eyeVp);
            const D3D12_RECT sr{LONG(pl.scissor[e][0]), LONG(pl.scissor[e][1]), LONG(pl.scissor[e][2]), LONG(pl.scissor[e][3])};
            scs(self, 1, &sr);
            D3D12_VERTEX_BUFFER_VIEW v{}; v.BufferLocation = G.uploadVa + pl.upload_offset[e]; v.SizeInBytes = bytes; v.StrideInBytes = acz::dbr3d::kVertexStride;
            ia(self, 0, 1, &v);
            if (pl.own_list[e]) {   // (F2) only the primitives in front of this eye: our R16 list into this eye's copy, the list topology
                D3D12_INDEX_BUFFER_VIEW iv{}; iv.BufferLocation = G.uploadVa + pl.list_offset[e]; iv.SizeInBytes = pl.list_count[e] * 2; iv.Format = DXGI_FORMAT_R16_UINT;
                setIb(self, &iv);
                topo(self, D3D12_PRIMITIVE_TOPOLOGY(pl.list_topology));
                drawIdx(self, pl.list_count[e], 1, 0, 0, startInstance);
                ownList = true;
                continue;
            }
            if (ownList) {   // the other eye drew its own list first: the game's index buffer and topology back for this eye's game-shaped draw
                setIb(self, r.ib.BufferLocation ? &r.ib : nullptr); topo(self, D3D12_PRIMITIVE_TOPOLOGY(r.topology)); ownList = false;
            }
            // indexed: the game's index buffer (still bound) with the copy rebased (vertex i of the draw = copy[i - imin]); else the copy itself
            if (pl.indexed) drawIdx(self, sc.f.index_count, 1, sc.f.start_index, pl.base_vertex, startInstance);
            else draw(self, pl.nv, 1, 0, startInstance);
        }
        if (ownList) { setIb(self, r.ib.BufferLocation ? &r.ib : nullptr); topo(self, D3D12_PRIMITIVE_TOPOLOGY(r.topology)); }   // the game's IA state
        ia(self, 0, 1, &r.vb);
        vps(self, 1, &r.om.vp);
        scs(self, 1, &r.om.sc);
        om(self, r.om.omCount, r.om.omCount ? r.om.omRtv.data() : nullptr, r.om.omSingle, r.om.omDsv ? &r.om.omDsvHandle : nullptr);
        for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        barrier(self, 2, bar);
        P.dbr3d_commit(pl);
        return;
    }
    if (acm) {   // stage 3b: UNTESTED LIVE
        if (!P.gpu() && !G.tried.exchange(true)) b3CreateGpu(self);
        u3live::PoseIn pose;
        if (P.wants_poses(now))
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
        using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
        using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
        using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
        using OmFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
        using VpFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
        using ScFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
        using SrvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
        using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
        const auto barrier = reinterpret_cast<BarrierFn>(step1Native(self, 26));
        const auto clearRtv = reinterpret_cast<ClearRtvFn>(step1Native(self, 48));
        const auto clearDsv = reinterpret_cast<ClearDsvFn>(step1Native(self, 47));
        const auto om = reinterpret_cast<OmFn>(step1Native(self, 46));
        const auto vps = reinterpret_cast<VpFn>(step1Native(self, 21));
        const auto scs = reinterpret_cast<ScFn>(step1Native(self, 22));
        const auto srv = reinterpret_cast<SrvFn>(step1Native(self, 40));
        const auto drawIdx = reinterpret_cast<DrawIdxFn>(step1Native(self, 13));
        if (!barrier || !clearRtv || !clearDsv || !om || !vps || !scs || !srv || !drawIdx) { P.gpu_failed("ui3d-hangar-list-natives"); return; }
        const u3live::AcmPlan pl = P.ui3d_acm(af, pose, b3Completion());
        if (!pl.record) return;   // a granted plan is a recording reservation: it ends with ui3d_acm_commit() or ui3d_acm_cancel() below
        if (!indexed || !G.uploadPtr || !G.rt[pl.slot][0] || !G.rt[pl.slot][1] || !G.ds[pl.slot][0] || !G.ds[pl.slot][1]) { P.ui3d_acm_cancel(pl); return; }
        for (unsigned e = 0; e < 2; ++e)   // our span of the WRITING slot (the core checked it against kUploadPerSlot)
            std::memcpy(G.uploadPtr + pl.upload_offset[e], pl.rec[e], acz::ui3d::hangar::kRecBytes);
        std::atomic_thread_fence(std::memory_order_release);
        ID3D12Resource* const img[2] = {G.rt[pl.slot][0], G.rt[pl.slot][1]};
        D3D12_RESOURCE_BARRIER bar[2] = {};
        for (unsigned e = 0; e < 2; ++e) {
            bar[e].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[e].Transition.pResource = img[e]; bar[e].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar[e].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar[e].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        barrier(self, 2, bar);
        static constexpr FLOAT transparent[4] = {0, 0, 0, 0};
        const D3D12_VIEWPORT eyeVp{0.0f, 0.0f, float(u3live::kEyeW), float(u3live::kEyeH), 0.0f, 1.0f};
        const D3D12_RECT eyeSc{0, 0, LONG(u3live::kEyeW), LONG(u3live::kEyeH)};
        namespace b3 = acz::brief3d;
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
            case b3::CmdKind::draw: drawIdx(self, count, instances, start, base, startInstance); break;
            case b3::CmdKind::vp_restore: vps(self, 1, &r.om.vp); break;
            case b3::CmdKind::sc_restore: scs(self, 1, &r.om.sc); break;
            case b3::CmdKind::om_restore: om(self, r.om.omCount, r.om.omCount ? r.om.omRtv.data() : nullptr, r.om.omSingle, r.om.omDsv ? &r.om.omDsvHandle : nullptr); break;
            }
        }
        for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        barrier(self, 2, bar);
        P.ui3d_acm_commit(pl);
        return;
    }
    if (f.vs8 == acz::ui3d::kPs2Vs8 && !P.gpu() && !G.tried.exchange(true)) b3CreateGpu(self);
    u3live::PoseIn pose;
    if (f.vs8 == acz::ui3d::kPs2Vs8 && P.wants_poses(now))
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
    using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
    using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
    using OmFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using VpFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
    using ScFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
    using IaFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
    using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    const auto barrier = reinterpret_cast<BarrierFn>(step1Native(self, 26));
    const auto clearRtv = reinterpret_cast<ClearRtvFn>(step1Native(self, 48));
    const auto clearDsv = reinterpret_cast<ClearDsvFn>(step1Native(self, 47));
    const auto om = reinterpret_cast<OmFn>(step1Native(self, 46));
    const auto vps = reinterpret_cast<VpFn>(step1Native(self, 21));
    const auto scs = reinterpret_cast<ScFn>(step1Native(self, 22));
    const auto ia = reinterpret_cast<IaFn>(step1Native(self, 44));
    const auto draw = reinterpret_cast<DrawFn>(step1Native(self, 12));
    if (f.vs8 == acz::ui3d::kPs2Vs8 && (!barrier || !clearRtv || !clearDsv || !om || !vps || !scs || !ia || !draw)) { P.gpu_failed("ui3d-list-natives"); return; }
    const u3live::UiPlan pl = P.ui3d_draw(f, pose, b3Completion());
    if (!pl.record) return;   // a granted plan is a recording reservation: it ends with ui3d_commit() or ui3d_cancel() below
    if (!G.uploadPtr || !G.uploadVa || !G.rt[pl.slot][0] || !G.rt[pl.slot][1] || !G.ds[pl.slot][0] || !G.ds[pl.slot][1]) { P.ui3d_cancel(pl); return; }
    const uint32_t copies = pl.copies ? pl.copies : 1u;   // 2026-10-30c: the selection slab = 6 faces of 4 vertices, one draw each
    const uint32_t bytes = pl.vertices * acz::ui3d::kVertexStride * copies;
    for (unsigned e = 0; e < 2; ++e)   // our span of the WRITING slot (the core checked it against kUploadPerSlot)
        if (pl.eye_draw[e]) std::memcpy(G.uploadPtr + pl.upload_offset[e], pl.vtx[e], bytes);
    std::atomic_thread_fence(std::memory_order_release);
    ID3D12Resource* const img[2] = {G.rt[pl.slot][0], G.rt[pl.slot][1]};
    D3D12_RESOURCE_BARRIER bar[2] = {};
    for (unsigned e = 0; e < 2; ++e) {
        bar[e].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[e].Transition.pResource = img[e]; bar[e].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[e].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar[e].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    barrier(self, 2, bar);
    if (pl.clear) {
        static constexpr FLOAT transparent[4] = {0, 0, 0, 0};
        for (unsigned e = 0; e < 2; ++e) {
            clearRtv(self, G.rtv[pl.slot][e], transparent, 0, nullptr);
            clearDsv(self, G.dsv[pl.slot][e], D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, pl.clear_depth, 0, 0, nullptr);
        }
    }
    const D3D12_VIEWPORT eyeVp{0.0f, 0.0f, float(u3live::kEyeW), float(u3live::kEyeH), 0.0f, 1.0f};
    for (unsigned e = 0; e < 2; ++e) {
        if (!pl.eye_draw[e]) continue;
        om(self, 1, &G.rtv[pl.slot][e], FALSE, pl.bind_dsv ? &G.dsv[pl.slot][e] : nullptr);
        vps(self, 1, &eyeVp);
        const D3D12_RECT sc{LONG(pl.scissor[e][0]), LONG(pl.scissor[e][1]), LONG(pl.scissor[e][2]), LONG(pl.scissor[e][3])};
        scs(self, 1, &sc);
        D3D12_VERTEX_BUFFER_VIEW v{}; v.BufferLocation = G.uploadVa + pl.upload_offset[e]; v.SizeInBytes = bytes; v.StrideInBytes = acz::ui3d::kVertexStride;
        ia(self, 0, 1, &v);
        for (uint32_t c = 0; c < copies; ++c) draw(self, pl.vertices, 1, c * pl.vertices, startInstance);
    }
    ia(self, 0, 1, &r.vb);
    vps(self, 1, &r.om.vp);
    scs(self, 1, &r.om.sc);
    om(self, r.om.omCount, r.om.omCount ? r.om.omRtv.data() : nullptr, r.om.omSingle, r.om.omDsv ? &r.om.omDsvHandle : nullptr);
    for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    barrier(self, 2, bar);
    P.ui3d_commit(pl);
}
