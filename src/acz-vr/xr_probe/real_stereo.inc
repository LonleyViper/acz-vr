// Card (d) only. Included inside Real after real_colour.inc. Every resource is
// probe-owned on the probe's own device; nothing here is reached by a1/a2.
struct StereoEye {
    ID3D12Resource* depth=nullptr;      // private D32 depth, never an XR depth swapchain
    ID3D12Resource* readback=nullptr;   // private READBACK buffer, COPY_DEST forever
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_bytes=0;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    std::vector<uint8_t> cpu;           // CPU copy made after completion, before release
    // Recorded image-state ledger for the current recording.
    enum class Recorded { render_target, copy_source } state=Recorded::render_target;
    bool copied=false, restored=false, read=false;
};
std::array<StereoEye,2> stereo_eyes{};
ID3D12RootSignature* stereo_root=nullptr;
ID3D12PipelineState* stereo_pso=nullptr;
ID3D12Resource* stereo_vertices=nullptr;
ID3D12DescriptorHeap* stereo_dsv_heap=nullptr;
D3D12_VERTEX_BUFFER_VIEW stereo_vbv{};
stereo::FrameViews stereo_frame{};
bool stereo_setup_failed=false;

bool setup_stereo() {
    log("synthetic_shaders compiler=fxc-build-time vs=vs_5_0 bytes="+std::to_string(sizeof(g_synthetic_vs))+
        " hash="+stereo::hex(stereo::fnv(g_synthetic_vs,sizeof(g_synthetic_vs)))+" ps=ps_5_0 bytes="+std::to_string(sizeof(g_synthetic_ps))+
        " hash="+stereo::hex(stereo::fnv(g_synthetic_ps,sizeof(g_synthetic_ps))));
    // One root-constant parameter: 16 floats at b0, vertex stage only.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister=0;param.Constants.RegisterSpace=0;param.Constants.Num32BitValues=16;
    param.ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters=1;rs.pParameters=&param;
    rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS|
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS|D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS|
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    ID3DBlob* blob=nullptr;ID3DBlob* error=nullptr;
    HRESULT hr=timed("D3D12SerializeRootSignature.synthetic",[&]{return D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);});
    if(error)timed("root-signature-error-blob.Release",[&]{auto refs=error->Release();error=nullptr;return refs;});
    if(FAILED(hr) || !blob){if(blob)blob->Release();return false;}
    hr=timed("CreateRootSignature.synthetic",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&stereo_root));});
    timed("root-signature-blob.Release",[&]{auto refs=blob->Release();blob=nullptr;return refs;});
    if(FAILED(hr) || !stereo_root)return false;
    const D3D12_INPUT_ELEMENT_DESC layout[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    static_assert(sizeof(stereo::Vertex)==24,"vertex layout");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature=stereo_root;
    pso.VS={g_synthetic_vs,sizeof(g_synthetic_vs)};pso.PS={g_synthetic_ps,sizeof(g_synthetic_ps)};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL; // no blending
    pso.SampleMask=UINT_MAX;
    pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; // handedness/winding cannot hide geometry
    pso.RasterizerState.DepthClipEnable=TRUE;
    pso.DepthStencilState.DepthEnable=TRUE;
    pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS; // ordinary [0,1] depth, clear 1
    pso.InputLayout={layout,2};
    pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets=1;pso.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    pso.DSVFormat=DXGI_FORMAT_D32_FLOAT;pso.SampleDesc.Count=1;
    hr=timed("CreateGraphicsPipelineState.synthetic",[&]{return device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&stereo_pso));});
    if(FAILED(hr) || !stereo_pso)return false;
    // Static vertex data in a probe-owned upload buffer, written once in setup.
    const auto data=stereo::vertices();
    D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizeof(data);
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr=timed("CreateCommittedResource.synthetic-vertices.upload",[&]{return device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&stereo_vertices));});
    if(FAILED(hr) || !stereo_vertices)return false;
    void* mapped=nullptr;D3D12_RANGE none{0,0};
    hr=timed("synthetic-vertices.Map",[&]{return stereo_vertices->Map(0,&none,&mapped);});
    if(FAILED(hr) || !mapped)return false;
    std::memcpy(mapped,data.data(),sizeof(data));
    timed("synthetic-vertices.Unmap",[&]{stereo_vertices->Unmap(0,nullptr);return S_OK;});
    timed("synthetic-vertices.GetGPUVirtualAddress",[&]{stereo_vbv.BufferLocation=stereo_vertices->GetGPUVirtualAddress();return S_OK;});
    stereo_vbv.SizeInBytes=sizeof(data);stereo_vbv.StrideInBytes=sizeof(stereo::Vertex);
    if(!stereo_vbv.BufferLocation || call_refused)return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=2;
    hr=timed("CreateDescriptorHeap.DSV.synthetic",[&]{return device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&stereo_dsv_heap));});
    if(FAILED(hr) || !stereo_dsv_heap)return false;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_base{};UINT dsv_stride=0;
    timed("DSV.heap.start",[&]{dsv_base=stereo_dsv_heap->GetCPUDescriptorHandleForHeapStart();return S_OK;});
    timed("DSV.heap.stride",[&]{dsv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);return S_OK;});
    if(!dsv_base.ptr || !dsv_stride || call_refused)return false;
    uint64_t readback_total=0;
    for(unsigned eye=0;eye<2;++eye) {
        auto& s=stereo_eyes[eye];const auto& n=native_eyes[eye];
        const auto w=view_limits[eye].recommendedImageRectWidth,h=view_limits[eye].recommendedImageRectHeight;
        if(n.images.empty() || !n.images[0].texture)return false;
        D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC depth{};depth.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;depth.Width=w;depth.Height=h;
        depth.DepthOrArraySize=1;depth.MipLevels=1;depth.Format=DXGI_FORMAT_D32_FLOAT;depth.SampleDesc.Count=1;
        depth.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;depth.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1.0f;
        hr=timed("CreateCommittedResource.private-depth.D32",[&]{return device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&s.depth));});
        if(FAILED(hr) || !s.depth)return false;
        s.dsv.ptr=dsv_base.ptr+SIZE_T(eye)*dsv_stride;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        timed("CreateDepthStencilView.private",[&]{device->CreateDepthStencilView(s.depth,&dv,s.dsv);return S_OK;});
        // Every enumerated image has the identical validated description.
        D3D12_RESOURCE_DESC image{};
        timed("XR.image0.GetDesc.footprint",[&]{image=n.images[0].texture->GetDesc();return S_OK;});
        UINT rows=0;UINT64 row_bytes=0,total=0;
        timed("GetCopyableFootprints.readback",[&]{device->GetCopyableFootprints(&image,0,1,0,&s.footprint,&rows,&row_bytes,&total);return S_OK;});
        if(call_refused || rows!=h || s.footprint.Footprint.Width!=w || s.footprint.Footprint.Height!=h ||
            row_bytes!=uint64_t(w)*4 || s.footprint.Footprint.RowPitch<w*4 || !total)return false;
        s.readback_bytes=total;readback_total+=total;
        if(readback_total>256ull*1024*1024){log("refusal=synthetic readback inventory exceeds 256 MiB");return false;}
        D3D12_HEAP_PROPERTIES back{};back.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb=buffer;rb.Width=total;
        hr=timed("CreateCommittedResource.private-readback",[&]{return device->CreateCommittedResource(&back,D3D12_HEAP_FLAG_NONE,&rb,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});
        if(FAILED(hr) || !s.readback)return false;
        try {s.cpu.assign(static_cast<size_t>(total),0);} catch(...) {log("refusal=synthetic CPU readback copy allocation");return false;}
        log("synthetic_eye_resources eye="+std::to_string(eye)+" size="+std::to_string(w)+"x"+std::to_string(h)+
            " depth=D32_FLOAT private=1 xr_depth_swapchain=0 DSV="+std::to_string(s.dsv.ptr)+" readback_bytes="+std::to_string(total)+
            " row_pitch="+std::to_string(s.footprint.Footprint.RowPitch)+" footprint_format="+std::to_string(s.footprint.Footprint.Format));
    }
    log("synthetic_inventory readback_bytes="+std::to_string(readback_total)+" vertex_bytes="+std::to_string(sizeof(data))+
        " scene_id="+stereo::hex(stereo::scene_hash()));
    return !call_refused && !loss_seen;
}
bool capture_stereo_views(uint64_t sample) {
    stereo_frame={};
    for(unsigned eye=0;eye<2;++eye) {
        const auto& v=raw_views[eye];auto& out=stereo_frame.eyes[eye];
        out.position[0]=v.pose.position.x;out.position[1]=v.pose.position.y;out.position[2]=v.pose.position.z;
        out.orientation[0]=v.pose.orientation.x;out.orientation[1]=v.pose.orientation.y;
        out.orientation[2]=v.pose.orientation.z;out.orientation[3]=v.pose.orientation.w;
        out.angle_left=v.fov.angleLeft;out.angle_right=v.fov.angleRight;out.angle_up=v.fov.angleUp;out.angle_down=v.fov.angleDown;
        out.width=view_limits[eye].recommendedImageRectWidth;out.height=view_limits[eye].recommendedImageRectHeight;
        if(!stereo::eye_matrix(out,stereo_frame.uploaded[eye])){log("refusal=synthetic matrix pipeline refused located view eye="+std::to_string(eye));
            stereo_frame={};return false;}
    }
    stereo_frame.sample=sample;
    return true;
}
bool record_stereo(NativeEye& e,unsigned eye,ImageReply& out) {
    auto& s=stereo_eyes[eye];
    if(!stereo_pso || !stereo_root || !s.depth || !s.readback || stereo_frame.sample!=located_serial){out.synthetic_failure=true;return false;}
    // Successful XR wait establishes RENDER_TARGET for the exact image.
    s.state=StereoEye::Recorded::render_target;s.copied=s.restored=s.read=false;
    auto* image=e.images[e.key.index].texture;
    HRESULT hr=timed("private-allocator.Reset",[&]{return e.allocator->Reset();});if(FAILED(hr))return false;
    hr=timed("private-list.Reset.synthetic-PSO",[&]{return e.list->Reset(e.allocator,stereo_pso);});if(FAILED(hr))return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(e.key.rtv)};
    const auto w=stereo_frame.eyes[eye].width,h=stereo_frame.eyes[eye].height;
    const D3D12_VIEWPORT viewport{0.0f,0.0f,float(w),float(h),0.0f,1.0f};
    const D3D12_RECT scissor{0,0,LONG(w),LONG(h)};
    const auto& bg=stereo::palette[static_cast<unsigned>(stereo::Paint::background)];
    const float background[4]={bg.r,bg.g,bg.b,1.0f}; // linear; sRGB RTV encodes once
    timed("SetGraphicsRootSignature.synthetic",[&]{e.list->SetGraphicsRootSignature(stereo_root);return S_OK;});
    timed("SetGraphicsRoot32BitConstants.eye-VP",[&]{e.list->SetGraphicsRoot32BitConstants(0,16,stereo_frame.uploaded[eye].data(),0);return S_OK;});
    timed("RSSetViewports.full-image",[&]{e.list->RSSetViewports(1,&viewport);return S_OK;});
    timed("RSSetScissorRects.full-image",[&]{e.list->RSSetScissorRects(1,&scissor);return S_OK;});
    timed("OMSetRenderTargets.exact-image+private-depth",[&]{e.list->OMSetRenderTargets(1,&rtv,FALSE,&s.dsv);return S_OK;});
    timed("ClearRenderTargetView.linear-dim.exact-image",[&]{e.list->ClearRenderTargetView(rtv,background,0,nullptr);return S_OK;});
    timed("ClearDepthStencilView.private-depth=1",[&]{e.list->ClearDepthStencilView(s.dsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);return S_OK;});
    timed("IASetPrimitiveTopology.triangle-list",[&]{e.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);return S_OK;});
    timed("IASetVertexBuffers.synthetic",[&]{e.list->IASetVertexBuffers(0,1,&stereo_vbv);return S_OK;});
    timed("DrawInstanced.synthetic-scene",[&]{e.list->DrawInstanced(stereo::vertex_count,1,0,0);return S_OK;});
    if(call_refused)return false; // nothing irreversible yet; list never closed/executed
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=image;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    timed("ResourceBarrier.exact-image.RENDER_TARGET->COPY_SOURCE",[&]{e.list->ResourceBarrier(1,&barrier);return S_OK;});
    if(!call_refused)s.state=StereoEye::Recorded::copy_source;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=s.readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=s.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=image;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    if(s.state==StereoEye::Recorded::copy_source) {
        timed("CopyTextureRegion.exact-image->private-readback",[&]{e.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);return S_OK;});
        s.copied=!call_refused;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        timed("ResourceBarrier.exact-image.COPY_SOURCE->RENDER_TARGET",[&]{e.list->ResourceBarrier(1,&barrier);return S_OK;});
        if(!call_refused){s.state=StereoEye::Recorded::render_target;s.restored=true;}
    }
    out.copied=s.copied;out.restored=s.restored && s.state==StereoEye::Recorded::render_target;
    log("synthetic_record eye="+std::to_string(eye)+" index="+std::to_string(e.key.index)+" vertices="+std::to_string(stereo::vertex_count)+
        " matrix_hash="+stereo::hex(stereo::matrix_hash(stereo_frame.uploaded[eye]))+" sample="+std::to_string(stereo_frame.sample)+
        " copied="+std::to_string(out.copied)+" final_state="+(s.state==StereoEye::Recorded::render_target?"RENDER_TARGET":"COPY_SOURCE"));
    // A recorded-but-unrestored list is reported to the owner, which then
    // never Closes/Executes it: the image remains known unsubmitted.
    return true;
}
bool readback_stereo(NativeEye& e,unsigned eye) {
    auto& s=stereo_eyes[eye];
    if(auto why=stereo::readback_refusal(e.acquired,e.submitted,e.completed,s.copied,e.released)){log(std::string("readback_refused=")+why);return false;}
    if(s.read || !s.readback || s.cpu.size()!=s.readback_bytes)return false;
    void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(s.readback_bytes)};
    HRESULT hr=timed("private-readback.Map.after-completion",[&]{return s.readback->Map(0,&range,&mapped);});
    if(FAILED(hr) || !mapped)return false;
    std::memcpy(s.cpu.data(),mapped,static_cast<size_t>(s.readback_bytes));
    const D3D12_RANGE written{0,0};
    timed("private-readback.Unmap",[&]{s.readback->Unmap(0,&written);return S_OK;});
    s.read=!call_refused;
    return s.read;
}
void destroy_stereo() {
    for(auto& s:stereo_eyes) {
        if(s.readback)timed("private-readback.Release",[&]{auto refs=s.readback->Release();s.readback=nullptr;return refs;});
        if(s.depth)timed("private-depth.Release",[&]{auto refs=s.depth->Release();s.depth=nullptr;return refs;});
        s.cpu.clear();
    }
    if(stereo_dsv_heap)timed("private-DSV-heap.Release",[&]{auto refs=stereo_dsv_heap->Release();stereo_dsv_heap=nullptr;return refs;});
    if(stereo_vertices)timed("synthetic-vertices.Release",[&]{auto refs=stereo_vertices->Release();stereo_vertices=nullptr;return refs;});
    if(stereo_pso)timed("synthetic-PSO.Release",[&]{auto refs=stereo_pso->Release();stereo_pso=nullptr;return refs;});
    if(stereo_root)timed("synthetic-root-signature.Release",[&]{auto refs=stereo_root->Release();stereo_root=nullptr;return refs;});
}
public:
bool synthetic_frame(stereo::FrameViews& out) override {
    if(!synthetic || !stereo_frame.sample || stereo_frame.sample!=located_serial)return false;
    out=stereo_frame;return true;
}
stereo::PixelView readback_pixels(unsigned eye) override {
    if(!synthetic || eye>=2 || !stereo_eyes[eye].read)return {};
    const auto& s=stereo_eyes[eye];
    return {s.cpu.data()+s.footprint.Offset,s.footprint.Footprint.Width,s.footprint.Footprint.Height,s.footprint.Footprint.RowPitch};
}
private:
