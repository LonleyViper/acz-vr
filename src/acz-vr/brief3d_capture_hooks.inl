// brief3d_capture_hooks.inl -- BRIEF3D phase-2 capture, the draw-seam glue (brief3d_capture.h; docs/world-render/brief3d.md 6.6 / 7 B3-2, B3-5).
// Included by d3d12_hook.cpp inside acz's anonymous namespace AFTER the step-1/3 adapters (step3MappedSpan_locked, guardedCpuRead).
// READ ONLY: nothing is recorded into a command list, no native call is made, no GPU or game memory is written. Called from observeDraw with the
// hook-state mutex held, only while acz::b3cap::sampling() (one sampled frame per second while brief3d-capture.arm exists). The b3cap mutex is a
// leaf lock taken inside it (admit / commit). No allocation, no logging (the Present thread formats and logs the line).
__declspec(noinline) void b3capDraw_locked(const ListState& ls,const PipelineRecord& p,bool indexed,uint32_t count,uint32_t instances) noexcept {
    namespace bc=acz::b3cap;namespace b3=acz::brief3d;
    const b3::Kind k=bc::kind_of(p.brVs8);
    if(k==b3::Kind::none)return;
    uint64_t serial=0;const bc::Admit admit=bc::admit(k,serial);
    if(admit!=bc::Admit::selmap_full&&admit!=bc::Admit::icon_read)return;   // not sampling, or only counted (a later SelMap, icons over the cap)
    bc::DrawFacts d;
    d.kind=k;d.classified=b3::classify(p.brVs8,p.brPs8,p.topology_type,indexed)==k;d.count=count;d.instances=instances;
    const auto& b=ls.bindings;d.ia_topology=b.topology;
    // The bound root must be the dumped map layout (brief3d::root_layout_check): t10 root SRV, b10 root constants (fail closed otherwise).
    b3::RootPlan plan;const char* rootWhy=nullptr;
    if(!b.tracking_complete||b.tracking_gap)rootWhy="b3cap-binding-tracking-gap";
    else if(!b.root||!b.root->valid())rootWhy="b3cap-root-layout-unknown";
    else if(b.root->layout().parameters.size()>64)rootWhy="b3cap-root-over-64";
    else {
        const auto& L=b.root->layout();b3::RootParam v[64];const unsigned n=unsigned(L.parameters.size());
        for(unsigned i=0;i<n;++i){const auto& q=L.parameters[i];v[i]=b3::RootParam{uint32_t(q.kind),uint32_t(q.visibility),q.shader_register,q.space,q.constant_count};}
        rootWhy=b3::root_layout_check(k,v,n,L.flags,plan);
    }
    auto& s=state();
    if(rootWhy){d.t10_why=rootWhy;d.b10_why=rootWhy;}
    else {
        // t10: the root SRV's VA (tracked value), its resource through the setter-time address registry, then the shared safe reader.
        const auto t=b.roots.find(plan.t10);
        if(t==b.roots.end()||t->second.kind!=metadata::ParameterKind::srv||!t->second.value)d.t10_why="b3cap-t10-unbound";
        else {
            const uint64_t va=t->second.value;const unsigned bytes=b3::srt_bytes(k);
            const uint64_t address=s.nativeBindings.diagnostic_address(va,bytes,ls.native_binding_device);
            uint8_t scratch[bc::kRecordCap];
            d.t10_why=bc::read_twice([](uint64_t a,uint64_t v,uint64_t n,const uint8_t*& out){return step3MappedSpan_locked(a,v,n,out);},
                                     [](const uint8_t* src,uint8_t* dst,size_t n){return guardedCpuRead(src,dst,n);},address,va,bytes,d.rec,scratch);
        }
        if(admit==bc::Admit::selmap_full) {   // cb10 from the tracked root constants
            const auto c=b.roots.find(plan.b10);
            if(c==b.roots.end()||c->second.kind!=metadata::ParameterKind::constants||c->second.constants.size()<2||c->second.written.size()<2)
                d.b10_why="b3cap-b10-unwritten";
            else {const bool w[2]={bool(c->second.written[0]),bool(c->second.written[1])};d.b10_why=bc::b10_pair(c->second.constants.data(),w,2,d.b10);}
        }
    }
    if(admit==bc::Admit::selmap_full) {
        const auto& o=p.output;d.pso_known=o.known;d.depth_enable=o.depth_enable;d.depth_write=o.depth_write;d.stencil=o.stencil;d.depth_func=o.depth_func;
        d.blend_enable=o.blend0;d.blend[0]=p.blend_src;d.blend[1]=p.blend_dst;d.blend[2]=p.blend_op;
        d.blend[3]=p.blend_src_alpha;d.blend[4]=p.blend_dst_alpha;d.blend[5]=p.blend_op_alpha;
        d.rt_count=o.rt_count;d.rtv0=o.rtv[0];d.dsv=o.dsv;d.samples=o.samples;d.topo_type=p.topology_type;
        if(b.index)d.index_stride=b.index->stride;
        // VB view 0: address/size/stride, and whether it lies inside a pinned, mapped UPLOAD span (no vertex byte is read).
        if(const auto vb=b.vertices.find(0);vb==b.vertices.end()||!vb->second.resource)d.vb_why="b3cap-vb-unbound";
        else {
            d.vb_va=vb->second.resource;d.vb_bytes=vb->second.size;d.vb_stride=vb->second.stride;
            const uint64_t address=d.vb_bytes?s.nativeBindings.diagnostic_address(d.vb_va,d.vb_bytes,ls.native_binding_device):0;
            const uint8_t* span=nullptr;
            d.vb_why=!d.vb_bytes?"b3cap-vb-empty":!address?"b3cap-vb-address-unresolved":step3MappedSpan_locked(address,d.vb_va,d.vb_bytes,span);
        }
    }
    bc::commit(serial,d);
}
