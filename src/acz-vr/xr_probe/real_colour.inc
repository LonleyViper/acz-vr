// Included only inside Real. No native symbols enter the fake-only core.
struct NativeEye {
    XrSwapchain chain=XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    ID3D12DescriptorHeap* heap=nullptr;
    ID3D12CommandAllocator* allocator=nullptr;
    ID3D12GraphicsCommandList* list=nullptr;
    SIZE_T rtv_base=0;UINT rtv_stride=0;
    ImageKey key{};
    bool acquired=false,waited=false,recorded=false,closed=false,submitted=false;
    bool signalled=false,completed=false,released=false,release_attempted=false;
};
std::array<NativeEye,2> native_eyes{};
ID3D12Fence* private_fence=nullptr;
uint64_t fence_issued=0, fence_observed=0, located_serial=0, located_frame=0;
bool colour_setup=false, native_destroy_attempted=false;
DXGI_FORMAT colour_format=DXGI_FORMAT_UNKNOWN;
std::array<XrView,2> raw_views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
XrTime located_time=0;
XrSpace located_space=XR_NULL_HANDLE;
template<class T> static uint64_t identity(T* p) {return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));}
bool valid_view(const XrView& v) {
    const float values[]={v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w,
        v.pose.position.x,v.pose.position.y,v.pose.position.z,v.fov.angleLeft,v.fov.angleRight,v.fov.angleUp,v.fov.angleDown};
    for(auto x:values)if(!std::isfinite(x))return false;
    return v.fov.angleLeft<v.fov.angleRight && v.fov.angleDown<v.fov.angleUp;
}
bool setup_colour() {
    if(colour_setup)return false;
    colour_setup=true; // partial ownership retained on any later failure
    auto selected=select_later_colour_format(offered_formats);
    if(!selected){log("refusal=a2 no RGBA8 format 28/29");return false;}
    colour_format=static_cast<DXGI_FORMAT>(*selected);
    log("selected_swapchain_format="+std::to_string(*selected)+
        " RTV_format=29 linear_clear=0.02,0.02,0.02,1 alpha=opaque encoding=sRGB-RTV-once no-preencode=1");
    HRESULT hr=timed("CreateFence.private.GPU-only",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&private_fence));});
    if(FAILED(hr))return false;
    uint64_t logical_bytes=0;
    for(unsigned eye=0;eye<2;++eye) {
        auto& e=native_eyes[eye];const auto& v=view_limits[eye];
        if(!v.recommendedImageRectWidth || !v.recommendedImageRectHeight ||
            v.recommendedImageRectWidth>8192 || v.recommendedImageRectHeight>8192)return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        // The UNORM fallback is allowed only with mutable-format permission AND
        // an enumerated typeless/sRGB resource permitting an sRGB RTV. A typed
        // UNORM resource is refused rather than assuming relaxed format casting.
        if(colour_format==DXGI_FORMAT_R8G8B8A8_UNORM)info.usageFlags|=XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        info.format=*selected;info.sampleCount=1;info.width=v.recommendedImageRectWidth;
        info.height=v.recommendedImageRectHeight;info.faceCount=1;info.arraySize=1;info.mipCount=1;
        if(!xr("xrCreateSwapchain",[&]{return xrCreateSwapchain(session,&info,&e.chain);}) || loss_seen)return false;
        uint32_t count=0;
        if(!xr("xrEnumerateSwapchainImages.count",[&]{return xrEnumerateSwapchainImages(e.chain,0,&count,nullptr);}) || loss_seen || !count || count>8)return false;
        // Logical inventory only, not a residency/runtime/driver allocation cap.
        logical_bytes+=uint64_t(info.width)*info.height*4*count;
        if(logical_bytes>512ull*1024*1024){log("refusal=image logical inventory exceeds 512 MiB");return false;}
        e.images.assign(count,XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        uint32_t actual=0;
        if(!xr("xrEnumerateSwapchainImages.data",[&]{return xrEnumerateSwapchainImages(e.chain,count,&actual,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));}) || loss_seen || actual!=count)return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=count;
        hr=timed("CreateDescriptorHeap.RTV",[&]{return device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&e.heap));});
        if(FAILED(hr))return false;
        D3D12_CPU_DESCRIPTOR_HANDLE base{};UINT stride=0;
        timed("RTV.heap.start",[&]{base=e.heap->GetCPUDescriptorHandleForHeapStart();return S_OK;});
        timed("RTV.heap.stride",[&]{stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);return S_OK;});
        if(!base.ptr || !stride || call_refused)return false;
        e.rtv_base=base.ptr;e.rtv_stride=stride;
        for(uint32_t index=0;index<count;++index) {
            auto* resource=e.images[index].texture;if(!resource)return false;
            D3D12_RESOURCE_DESC rd{};
            timed("XR.image.GetDesc",[&]{rd=resource->GetDesc();return S_OK;});
            ID3D12Device* owner=nullptr;
            hr=timed("XR.image.GetDevice",[&]{return resource->GetDevice(IID_PPV_ARGS(&owner));});
            const bool associated=SUCCEEDED(hr) && owner==device;
            if(owner)timed("XR.image.owner.Release",[&]{return owner->Release();});
            if(!associated || rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width!=info.width || rd.Height!=info.height ||
                rd.DepthOrArraySize!=1 || rd.MipLevels!=1 || rd.SampleDesc.Count!=1 ||
                !(rd.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                (rd.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS && rd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))return false;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;rtv.Texture2D.MipSlice=0;rtv.Texture2D.PlaneSlice=0;
            D3D12_CPU_DESCRIPTOR_HANDLE handle{base.ptr+SIZE_T(index)*stride};
            timed("CreateRenderTargetView.exact-XR-image",[&]{device->CreateRenderTargetView(resource,&rtv,handle);return S_OK;});
            log("image_binding eye="+std::to_string(eye)+" index="+std::to_string(index)+" resource="+
                std::to_string(identity(resource))+" RTV="+std::to_string(handle.ptr)+" resource_format="+std::to_string(rd.Format));
        }
        hr=timed("CreateCommandAllocator.private",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&e.allocator));});
        if(FAILED(hr))return false;
        hr=timed("CreateCommandList.private",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,e.allocator,nullptr,IID_PPV_ARGS(&e.list));});
        if(FAILED(hr))return false;
        hr=timed("Close.initial-empty-list",[&]{return e.list->Close();});if(FAILED(hr))return false;
    }
    log("image_inventory logical_bytes="+std::to_string(logical_bytes)+" epoch=1 private_fence_GPU_signal_only=1");
    // Card (d) only: probe-owned scene resources after the swapchain inventory.
    if(synthetic && !call_refused && !loss_seen && !setup_stereo()){stereo_setup_failed=true;return false;}
    return !call_refused && !loss_seen;
}
bool end_projection(unsigned count,const void* opaque) {
    auto refuse=[&] {
        log("projection_refused=1 disposition=zero-layers-one-end-attempt");
        if(fatal_loss)return false;
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime=predicted;info.environmentBlendMode=blend;
        info.layerCount=0;info.layers=nullptr;
        return xr("xrEndFrame.refused-projection-zero",[&]{return xrEndFrame(session,&info);});
    };
    if(count!=1 || !opaque || reference_changed || loss_seen || frame_discarded || !should_render)return refuse();
    const auto& p=*static_cast<const Projection*>(opaque);
    if(p.sample!=located_serial || p.frame!=located_frame || located_space!=space || located_time!=predicted)return refuse();
    std::array<XrCompositionLayerProjectionView,2> views{};
    for(unsigned eye=0;eye<2;++eye) {
        const auto& e=native_eyes[eye];
        if(e.key.frame!=p.frame || !e.completed || !e.released || e.acquired)return refuse();
        auto& v=views[eye];v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        v.pose=raw_views[eye].pose;v.fov=raw_views[eye].fov; // raw, applied exactly once
        v.subImage.swapchain=e.chain;v.subImage.imageArrayIndex=0;
        v.subImage.imageRect.extent={static_cast<int32_t>(view_limits[eye].recommendedImageRectWidth),
            static_cast<int32_t>(view_limits[eye].recommendedImageRectHeight)};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space=located_space;layer.viewCount=2;layer.views=views.data();
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=located_time;
    info.environmentBlendMode=blend;info.layerCount=1;info.layers=layers;
    log("frame_submission layerCount=1 viewCount=2 raw_views_sample="+std::to_string(p.sample)+
        " predicted_time="+std::to_string(located_time));
    return xr("xrEndFrame.projection",[&]{return xrEndFrame(session,&info);});
}
ImageReply image_call(ImageOp op,const ImageRequest& q) {
    call_refused=false;current_op=Op::count;
    if(op==ImageOp::destroy && !cleanup_mode){cleanup_mode=true;deadline=std::min(Caps::absolute_ms,clock.now()+Caps::cleanup_ms);}
    if(!cleanup_mode)deadline=Caps::absolute_ms;
    ImageReply out;out.key=q.key;
    if(op==ImageOp::setup){out.ok=setup_colour();out.loss=loss_seen;out.synthetic_failure=stereo_setup_failed;
        out.key.device=identity(device);out.key.queue=identity(queue);out.key.fence=identity(private_fence);out.key.epoch=1;return out;}
    if(op==ImageOp::locate) {
        if(loss_seen || reference_changed || !should_render || frame_discarded)return out;
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};info.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime=predicted;info.space=space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};uint32_t count=0;
        raw_views={{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
        out.ok=xr("xrLocateViews",[&]{return xrLocateViews(session,&info,&view_state,2,&count,raw_views.data());});
        out.views_valid=out.ok && count==2 && (view_state.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (view_state.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT) && valid_view(raw_views[0]) && valid_view(raw_views[1]);
        if(out.views_valid){out.sample=++located_serial;located_time=predicted;located_space=space;located_frame=0;}
        if(out.views_valid && synthetic)out.views_valid=capture_stereo_views(out.sample);
        out.loss=loss_seen;return out;
    }
    if(op==ImageOp::destroy) {
        if(native_destroy_attempted)return out;
        native_destroy_attempted=true;
        for(auto& e:native_eyes) {
            if(e.acquired || (e.submitted && !e.completed))return out;
            if(e.chain) {
                if(!xr("xrDestroySwapchain",[&]{return xrDestroySwapchain(e.chain);})) {out.loss=loss_seen;return out;}
                e.chain=XR_NULL_HANDLE;e.images.clear();
            }
        }
        // All swapchains are retired before any private dependency is dropped.
        for(auto& e:native_eyes) {
            if(e.list)timed("private-list.Release",[&]{auto refs=e.list->Release();e.list=nullptr;return refs;});
            if(e.allocator)timed("private-allocator.Release",[&]{auto refs=e.allocator->Release();e.allocator=nullptr;return refs;});
            if(e.heap)timed("private-RTV-heap.Release",[&]{auto refs=e.heap->Release();e.heap=nullptr;return refs;});
        }
        if(synthetic)destroy_stereo();
        if(private_fence)timed("private-fence.Release",[&]{auto refs=private_fence->Release();private_fence=nullptr;return refs;});
        out.ok=!call_refused;out.loss=loss_seen;return out;
    }
    if(q.key.eye>=2)return out;
    auto& e=native_eyes[q.key.eye];
    if(op==ImageOp::acquire) {
        if(loss_seen || reference_changed || !e.chain || e.acquired || (e.submitted && !e.completed))return out;
        e.released=false;e.waited=e.recorded=e.closed=e.submitted=e.signalled=e.completed=e.release_attempted=false;
        if(synthetic){auto& s=stereo_eyes[q.key.eye];s.copied=s.restored=s.read=false;}
        uint32_t index=0;XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        out.ok=xr("xrAcquireSwapchainImage",[&]{return xrAcquireSwapchainImage(e.chain,&info,&index);});
        if(out.ok) {
            e.acquired=true;e.key=q.key;e.key.index=index;
            // Invalid successful index retains acquisition; never access it.
            if(index>=e.images.size()){out.key=e.key;out.loss=loss_seen;return out;}
            e.key.device=identity(device);e.key.queue=identity(queue);e.key.fence=identity(private_fence);e.key.epoch=1;
            e.key.resource=identity(e.images[index].texture);e.key.rtv=e.rtv_base+SIZE_T(index)*e.rtv_stride;
            e.key.allocator=identity(e.allocator);e.key.list=identity(e.list);
            located_frame=q.key.frame;out.key=e.key;
        }
        out.loss=loss_seen;return out;
    }
    // Exact immutable recording and same epoch, before any foreign image call.
    auto expected=e.key;expected.value=q.key.value;
    if(!e.acquired || !same_key(expected,q.key))return out;
    switch(op) {
    case ImageOp::wait: {
        if(e.waited || (q.wait_ns!=0 && q.wait_ns!=1000000))break;
        XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};info.timeout=q.wait_ns;
        out.ok=xr("xrWaitSwapchainImage",[&]{return xrWaitSwapchainImage(e.chain,&info);});
        out.timeout=last_xr==XR_TIMEOUT_EXPIRED;
        if(out.ok && !out.timeout)e.waited=true;
        break;
    }
    case ImageOp::record: {
        if(loss_seen || reference_changed || !e.waited || e.recorded || e.submitted)break;
        if(synthetic) {
            // Card (d): scene draw + copy; only a restored, copied list may Close.
            const bool recorded=record_stereo(e,q.key.eye,out) && !call_refused;
            e.recorded=recorded && out.restored && out.copied;out.ok=recorded;break;
        }
        HRESULT hr=timed("private-allocator.Reset",[&]{return e.allocator->Reset();});if(FAILED(hr))break;
        hr=timed("private-list.Reset",[&]{return e.list->Reset(e.allocator,nullptr);});if(FAILED(hr))break;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{static_cast<SIZE_T>(e.key.rtv)};
        // Successful XR wait establishes RENDER_TARGET. No ResourceBarrier:
        // clear and return in RENDER_TARGET, never COMMON/PRESENT.
        timed("ClearRenderTargetView.linear-dim.exact-image",[&]{e.list->ClearRenderTargetView(handle,dim_clear.data(),0,nullptr);return S_OK;});
        e.recorded=!call_refused;out.ok=e.recorded;break;
    }
    case ImageOp::close:
        if(e.recorded && !e.closed && !e.submitted) {
            out.ok=SUCCEEDED(timed("private-list.Close.actual-HRESULT",[&]{return e.list->Close();}));e.closed=out.ok;
        }break;
    case ImageOp::execute: {
        if(loss_seen || reference_changed || !e.closed || e.submitted || q.key.value!=fence_issued+1)break;
        // Reauthenticate the probe-owned DIRECT queue, without an observer hook
        // or game queue. Serial owner has exclusive access to this private list.
        ID3D12Device* owner=nullptr;D3D12_COMMAND_QUEUE_DESC desc{};
        HRESULT hr=timed("private-queue.GetDevice",[&]{return queue->GetDevice(IID_PPV_ARGS(&owner));});
        timed("private-queue.GetDesc",[&]{desc=queue->GetDesc();return S_OK;});
        bool authenticated=SUCCEEDED(hr) && owner==device && desc.Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(owner)timed("private-queue.owner.Release",[&]{return owner->Release();});
        if(!authenticated || call_refused)break;
        ID3D12CommandList* lists[]={e.list};e.submitted=true;e.key=q.key;out.dispatch_possible=true;
        timed("private-queue.ExecuteCommandLists.exact-one",[&]{queue->ExecuteCommandLists(1,lists);return S_OK;});
        hr=timed("device.GetDeviceRemovedReason.after-Execute",[&]{return device->GetDeviceRemovedReason();});
        out.ok=SUCCEEDED(hr) && !call_refused;break;
    }
    case ImageOp::signal:
        if(e.submitted && !e.signalled && e.key.value==fence_issued+1) {
            out.ok=SUCCEEDED(timed("private-queue.Signal.GPU-only",[&]{return queue->Signal(private_fence,e.key.value);}));
            if(out.ok){e.signalled=true;fence_issued=e.key.value;}
        }break;
    case ImageOp::sample:
        if(e.submitted && e.signalled) {
            uint64_t value=0;
            timed("private-fence.GetCompletedValue",[&]{value=private_fence->GetCompletedValue();return S_OK;});
            HRESULT hr=timed("device.GetDeviceRemovedReason.sample",[&]{return device->GetDeviceRemovedReason();});
            out.completed=value;
            out.ok=SUCCEEDED(hr) && value!=std::numeric_limits<uint64_t>::max() && value>=fence_observed && !call_refused;
            if(out.ok){fence_observed=value;e.completed=value>=e.key.value;}
        }break;
    case ImageOp::readback:
        out.ok=synthetic && readback_stereo(e,q.key.eye);break;
    case ImageOp::release:
        if(e.waited && (!e.submitted || e.completed) && !e.release_attempted) {
            e.release_attempted=true;XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            out.ok=xr("xrReleaseSwapchainImage",[&]{return xrReleaseSwapchainImage(e.chain,&info);});
            if(out.ok){e.acquired=false;e.released=true;}
        }break;
    default:break;
    }
    out.ok &= !call_refused;out.loss=loss_seen;return out;
}
