// ui3d_census_hooks.inl -- UI3D menu draw census, the draw-seam glue (ui3d_census.h; docs/world-render/ui-3d.md section 4 item 1).
// Included by d3d12_hook.cpp inside acz's anonymous namespace AFTER the step-1/3 adapters (step3MappedSpan_locked, guardedCpuRead) and the
// brief3d capture glue. READ ONLY: nothing is recorded into a command list, no native call is made, no GPU or game memory is written; the only
// CPU reads of game data go through step3MappedSpan_locked (pinned mapped UPLOAD span) + guardedCpuRead. Called from observeDraw with the hook-state
// mutex held, only while acz::u3c::sampling() (one sampled frame per second while ui3d-census.arm exists). The u3c mutex is a leaf lock taken
// inside it (admit / commit). No allocation, no logging (the Present thread formats and logs).
// The PSO output / RS / OM facts shared by the menu and ACM records (hook-state mutex held; `?` = left unknown).
__declspec(noinline) void u3cFillState_locked(const ListState& ls,const PipelineRecord& p,acz::u3c::DrawRec& d) noexcept {
    auto& s=state();const auto& t=ls.step3;
    // PSO output facts
    const auto& o=p.output;
    d.pso_known=o.known;d.pso_rtv0=o.rtv[0];d.pso_dsv=o.dsv;
    d.depth_enable=o.depth_enable;d.depth_write=o.depth_write;d.stencil=o.stencil;d.depth_func=o.depth_func;d.blend_enable=o.blend0;
    d.blend[0]=p.blend_src;d.blend[1]=p.blend_dst;d.blend[2]=p.blend_op;d.blend[3]=p.blend_src_alpha;d.blend[4]=p.blend_dst_alpha;d.blend[5]=p.blend_op_alpha;
    // RS/OM track (only exact while the step-3 hooks run; otherwise `?`)
    const bool track=t.known&&!t.gap;
    if(track&&t.viewports>=1){d.vp_known=true;d.vp[0]=t.vp0.TopLeftX;d.vp[1]=t.vp0.TopLeftY;d.vp[2]=t.vp0.Width;d.vp[3]=t.vp0.Height;d.nvp=t.viewports;}
    if(track&&t.scissors>=1){d.sc_known=true;d.sc[0]=t.sc0.left;d.sc[1]=t.sc0.top;d.sc[2]=t.sc0.right;d.sc[3]=t.sc0.bottom;d.nsc=t.scissors;}
    if(track&&t.omSet&&t.omCount>=1&&t.rtv[0].ptr){
        for(const auto& v:s.step3Views)if(v.handle&&v.handle==t.rtv[0].ptr&&!v.depth){
            d.rt_known=true;d.rt_res=v.resource;d.rt_vfmt=v.vd.present?v.vd.format:0;d.rt_rfmt=v.format;d.rt_w=uint32_t(v.width);d.rt_h=v.height;break;
        }
    }
    if(track&&t.omSet&&t.dsv&&t.dsvHandle.ptr){
        for(const auto& v:s.step3Views)if(v.handle&&v.handle==t.dsvHandle.ptr&&v.depth){d.dsv_view_known=true;d.dsv_view_fmt=v.vd.present?v.vd.format:v.format;break;}
    }
}
// ACM mode (ui3d-census-acm.arm): one DrawACM_v draw -- state facts, cb10 root constants (vertex then pixel stage), and the t10 SrtData record as the flight path
// decodes it (A +0, B +256, depth scale +360, screen constants +400) through the pinned mapped UPLOAD span and two agreeing guarded copies. Read only.
__declspec(noinline) void u3cAcm_locked(const ListState& ls,const PipelineRecord& p,bool indexed,uint32_t start,int32_t base,uint32_t count,uint32_t instances,
                                        uint32_t serial,uint32_t slot,uint32_t all,uint32_t ord) noexcept {
    namespace uc=acz::u3c;
    uc::AcmRec r;auto& d=r.c;d.all=all;d.ord=ord;d.count=count;d.instances=instances;d.start=start;d.indexed=indexed;r.base=base;
    u3cFillState_locked(ls,p,d);
    auto& s=state();const auto& b=ls.bindings;
    if(b.index)r.ib_stride=uint32_t(b.index->stride);
    if(!b.tracking_complete||b.tracking_gap){r.cb_why="u3c-binding-tracking-gap";r.t10_why="u3c-binding-tracking-gap";}
    else if(!b.root||!b.root->valid()){r.cb_why="u3c-root-layout-unknown";r.t10_why="u3c-root-layout-unknown";}
    else {
        // cb10: root constants of the bound layout
        auto c=b.root->resolve(metadata::BindingKind::cbv,10,0,metadata::ShaderStage::vertex);
        if(!c.valid)c=b.root->resolve(metadata::BindingKind::cbv,10,0,metadata::ShaderStage::pixel);
        if(!c.valid)r.cb_why="u3c-cb10-unresolved";
        else if(c.parameter_kind!=metadata::ParameterKind::constants)r.cb_why="u3c-cb10-not-root-constants";
        else if(const auto v=b.roots.find(c.root_index);v==b.roots.end()||v->second.kind!=metadata::ParameterKind::constants)r.cb_why="u3c-cb10-unbound";
        else {
            r.cb_root=c.root_index;
            for(unsigned i=0;i<4;++i)if(i<v->second.constants.size()&&i<v->second.written.size()&&v->second.written[i]){r.cb[i]=uint32_t(v->second.constants[i]);r.cb_mask|=1u<<i;}
            r.cb_why=r.cb_mask?nullptr:"u3c-cb10-unwritten";
        }
        // t10: the root SRV VA (tracked value) -> setter-time address registry -> pinned mapped UPLOAD span -> two agreeing guarded copies
        const auto t=b.root->resolve(metadata::BindingKind::srv,10,0,metadata::ShaderStage::vertex);
        if(!t.valid)r.t10_why="u3c-t10-unresolved";
        else if(t.parameter_kind==metadata::ParameterKind::table)r.t10_why="u3c-t10-in-descriptor-table";
        else if(const auto v=b.roots.find(t.root_index);v==b.roots.end()||v->second.kind!=metadata::ParameterKind::srv||!v->second.value)r.t10_why="u3c-t10-unbound";
        else {
            r.t10_va=v->second.value;
            const uint64_t address=s.nativeBindings.diagnostic_address(r.t10_va,uc::kRecBytes,ls.native_binding_device);
            const uint8_t* span=nullptr;uint8_t one[uc::kRecBytes],two[uc::kRecBytes];
            const char* why=!address?"u3c-t10-address-unresolved":step3MappedSpan_locked(address,r.t10_va,uc::kRecBytes,span);
            if(!why&&(!span||!guardedCpuRead(span,one,uc::kRecBytes)||!guardedCpuRead(span,two,uc::kRecBytes)))why="u3c-t10-read-fault";
            if(!why&&std::memcmp(one,two,uc::kRecBytes)!=0)why="u3c-t10-record-torn";
            if(!why){
                std::memcpy(r.a,one,64);std::memcpy(r.b,one+256,64);std::memcpy(&r.k360,one+360,4);std::memcpy(r.k400,one+400,16);
            }
            r.t10_why=why;
        }
    }
    uc::commit_acm(serial,slot,r);
}
// GEO read of one indexed menu draw (debrief-3d-map.md 7): the draw's index range (bounded) through the pinned mapped UPLOAD span + one guarded copy,
// then every referenced vertex's x, y, z, q (imin..imax, bounded) one guarded 16-byte copy each. Read only; any failure = geo.why (printed `?`).
__declspec(noinline) void u3cGeo_locked(const ListState& ls,const PipelineRecord& p,uint32_t start,int32_t base,uint32_t count,acz::u3c::DrawRec& d) noexcept {
    namespace uc=acz::u3c;
    auto& s=state();const auto& b=ls.bindings;auto& g=d.geo;
    g.cut=p.strip_cut;g.base=base;g.ni=count;g.why=nullptr;
    if(!b.index||!b.index->resource){g.why="u3c-ib-unbound";return;}
    const uint32_t ibs=b.index->stride;g.ibs=ibs;
    if(ibs!=2&&ibs!=4){g.why="u3c-ib-stride";return;}
    const uint32_t ni=count<uc::kGeoMaxIdx?count:uc::kGeoMaxIdx;
    const uint64_t first=uint64_t(start)*ibs,bytes=uint64_t(ni)*ibs;
    if(!ni){g.why="u3c-geo-no-indices";return;}
    if(first+bytes>b.index->size||first+bytes<first){g.why="u3c-ib-range";return;}
    static uint8_t ib[uc::kGeoMaxIdx*4];
    {
        const uint64_t va=b.index->resource+first;
        const uint64_t address=s.nativeBindings.diagnostic_address(va,bytes,ls.native_binding_device);
        const uint8_t* span=nullptr;
        const char* why=!address?"u3c-ib-address-unresolved":step3MappedSpan_locked(address,va,bytes,span);
        if(!why&&(!span||!guardedCpuRead(span,ib,size_t(bytes))))why="u3c-ib-read-fault";
        if(why){g.why=why;return;}
    }
    uc::geo_scan_indices(ib,ni,ibs,g);
    if(g.why)return;
    const auto vb=b.vertices.find(0);
    if(vb==b.vertices.end()||!vb->second.resource){g.why="u3c-vb-unbound";return;}
    const uint64_t stride=vb->second.stride;
    if(stride<16){g.why="u3c-vb-stride";return;}
    const int64_t lo=int64_t(base)+int64_t(g.imin);
    if(lo<0){g.why="u3c-geo-base";return;}
    const uint64_t want=uint64_t(g.imax-g.imin)+1,nv=want<uc::kGeoMaxVerts?want:uc::kGeoMaxVerts;
    g.vtrunc=nv<want;
    const uint64_t vfirst=uint64_t(lo)*stride,vbytes=(nv-1)*stride+16;
    if(vfirst+vbytes>vb->second.size||vfirst+vbytes<vfirst){g.why="u3c-geo-vb-range";return;}
    const uint64_t va=vb->second.resource+vfirst;
    const uint64_t address=s.nativeBindings.diagnostic_address(va,vbytes,ls.native_binding_device);
    const uint8_t* span=nullptr;
    const char* why=!address?"u3c-vb-address-unresolved":step3MappedSpan_locked(address,va,vbytes,span);
    if(why||!span){g.why=why?why:"u3c-vb-read-fault";return;}
    for(uint64_t i=0;i<nv&&!g.why;++i){
        float f[4];
        if(!guardedCpuRead(span+i*stride,f,16)){g.why="u3c-vb-read-fault";break;}
        uc::geo_add_vertex(g,uint32_t(i),f);
    }
}
__declspec(noinline) void u3cDraw_locked(const ListState& ls,const PipelineRecord& p,bool gsFlag,bool indexed,uint32_t start,int32_t base,uint32_t count,uint32_t instances) noexcept {
    namespace uc=acz::u3c;
    const bool menu=p.brVs8==uc::kMenuVs8&&p.brPs8==uc::kMenuPs8;
    uint32_t serial=0,slot=0,all=0,ord=0;
    const uc::Admit ad=uc::admit(menu,p.brVs8,p.brPs8,uint8_t(p.topology_type),indexed,gsFlag,count,instances,serial,slot,all,ord);
    if(ad==uc::Admit::take_acm){u3cAcm_locked(ls,p,indexed,start,base,count,instances,serial,slot,all,ord);return;}
    if(ad!=uc::Admit::take)return;
    uc::DrawRec d;d.all=all;d.ord=ord;d.count=count;d.instances=instances;d.start=start;d.indexed=indexed;
    u3cFillState_locked(ls,p,d);
    auto& s=state();const auto& b=ls.bindings;
    // VB view 0: address/size/stride, mapped-UPLOAD status, and (non-indexed) up to kMaxVerts vertices x,y,z,q of the draw's own range
    if(const auto vb=b.vertices.find(0);vb==b.vertices.end()||!vb->second.resource){d.vb_why="u3c-vb-unbound";d.vtx_why="u3c-vb-unbound";}
    else {
        d.vb_bound=true;d.vb_va=vb->second.resource;d.vb_size=vb->second.size;d.vb_stride=vb->second.stride;
        const uint64_t stride=d.vb_stride;
        const uint64_t nv=std::min<uint64_t>(count,uc::kMaxVerts);
        const uint64_t first=uint64_t(start)*stride,bytes=nv*stride;bool spanChecked=false;
        if(indexed)d.vtx_why="u3c-indexed-unread";
        else if(stride<16)d.vtx_why="u3c-vb-stride";
        else if(!nv)d.vtx_why="u3c-no-vertices";
        else if(first+bytes>d.vb_size||first+bytes<first)d.vtx_why="u3c-vb-range";
        else {
            const uint64_t va=d.vb_va+first;
            const uint64_t address=s.nativeBindings.diagnostic_address(va,bytes,ls.native_binding_device);
            const uint8_t* span=nullptr;
            const char* why=!address?"u3c-vb-address-unresolved":step3MappedSpan_locked(address,va,bytes,span);
            d.vb_why=why;spanChecked=true;
            if(why)d.vtx_why=why;
            else {
                bool ok=true;
                for(uint64_t i=0;i<nv&&ok;++i)ok=guardedCpuRead(span+i*stride,d.vtx[i],16);
                if(ok)d.nv=uint32_t(nv);else d.vtx_why="u3c-vb-read-fault";
            }
        }
        if(!d.vb_why&&!spanChecked)d.vb_why="u3c-vb-not-checked";   // indexed / stride / range: the span itself was not tested
    }
    d.ia=b.topology;
    // GEO (debrief-3d-map.md 7): the first kMaxGeo indexed menu draws of the sampled frame (hook-state mutex held: the statics are exclusive)
    if(indexed){
        static uint32_t geoSerial=0,geoCount=0;
        if(serial!=geoSerial){geoSerial=serial;geoCount=0;}
        if(geoCount<uc::kMaxGeo){++geoCount;u3cGeo_locked(ls,p,start,base,count,d);}
    }
    // cb0: b0 of the vertex stage through the root layout: root constants (tracked dwords) or a root CBV (read through the mapped span)
    if(!b.tracking_complete||b.tracking_gap)d.cb_why="u3c-binding-tracking-gap";
    else if(!b.root||!b.root->valid())d.cb_why="u3c-root-layout-unknown";
    else {
        const auto r=b.root->resolve(metadata::BindingKind::cbv,0,0,metadata::ShaderStage::vertex);
        if(!r.valid)d.cb_why="u3c-cb0-unresolved";
        else if(r.parameter_kind==metadata::ParameterKind::table)d.cb_why="u3c-cb0-in-descriptor-table";
        else if(const auto v=b.roots.find(r.root_index);v==b.roots.end())d.cb_why="u3c-cb0-unbound";
        else if(r.parameter_kind==metadata::ParameterKind::constants&&v->second.kind==metadata::ParameterKind::constants){
            d.cb_kind="rc";d.cb_root=r.root_index;
            for(unsigned i=0;i<4;++i)if(i<v->second.constants.size()&&i<v->second.written.size()&&v->second.written[i]){
                std::memcpy(&d.cb[i],&v->second.constants[i],4);d.cb_mask|=1u<<i;
            }
            if(!d.cb_mask){d.cb_kind=nullptr;d.cb_why="u3c-cb0-unwritten";}
        }
        else if(r.parameter_kind==metadata::ParameterKind::cbv&&v->second.kind==metadata::ParameterKind::cbv&&v->second.value){
            d.cb_root=r.root_index;d.cb_va=v->second.value;
            const uint64_t address=s.nativeBindings.diagnostic_address(d.cb_va,16,ls.native_binding_device);
            const uint8_t* span=nullptr;
            const char* why=!address?"u3c-cb0-address-unresolved":step3MappedSpan_locked(address,d.cb_va,16,span);
            if(why)d.cb_why=why;
            else if(!guardedCpuRead(span,d.cb,16))d.cb_why="u3c-cb0-read-fault";
            else {d.cb_kind="cbv";d.cb_mask=15u;}
        }
        else d.cb_why="u3c-cb0-unbound";
    }
    uc::commit(serial,slot,d);
}
