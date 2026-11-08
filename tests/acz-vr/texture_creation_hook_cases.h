#pragma once
// Included inside hook_fixture after its fake table/log definitions.
static unsigned textureViewCalls=0,textureDescCalls=0;
static D3D12_RESOURCE_DESC textureWriteDesc{};
// Let the compiler generate the same virtual aggregate-return ABI as GetDesc.
struct TextureDescDispatch {
    virtual void f0(){} virtual void f1(){} virtual void f2(){} virtual void f3(){} virtual void f4(){}
    virtual void f5(){} virtual void f6(){} virtual void f7(){} virtual void f8(){} virtual void f9(){}
    virtual D3D12_RESOURCE_DESC STDMETHODCALLTYPE get() {
        CHECK(!mutex_depth);++textureDescCalls;return textureWriteDesc;
    }
};
static void STDMETHODCALLTYPE textureSrv(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) {
    CHECK(!mutex_depth);++textureViewCalls;
}
static void STDMETHODCALLTYPE textureUav(ID3D12Device*,ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE) {
    CHECK(!mutex_depth);++textureViewCalls;
}
template<class Desc> static void STDMETHODCALLTYPE textureOtherView(ID3D12Device*,ID3D12Resource*,const Desc*,D3D12_CPU_DESCRIPTOR_HANDLE) {
    CHECK(!mutex_depth);++textureViewCalls;
}
static HRESULT STDMETHODCALLTYPE textureCommitted(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void** out) {
    CHECK(!mutex_depth);*out=output;return result;
}
static HRESULT STDMETHODCALLTYPE texturePlaced(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void** out) {
    CHECK(!mutex_depth);*out=output;return result;
}
static HRESULT STDMETHODCALLTYPE textureReserved(ID3D12Device*,const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void** out) {
    CHECK(!mutex_depth);*out=output;return result;
}
__declspec(noinline) static void textureCreationHookCases(TestPaths& paths) {
    auto& s=state();
    auto savedCandidates=std::make_unique<SrvCandidates>(s.srvCandidates);
    s.srvCandidates=std::move(*std::make_unique<SrvCandidates>());
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    const auto sightings=std::make_unique<CreationSightings>(s.creationSightings);auto* savedSink=diagnostics::sink();
    const auto savedResult=result;auto* savedOutput=output;
    TraceIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    auto dt=table();dt[27]=reinterpret_cast<void*>(&textureCommitted);
    dt[29]=reinterpret_cast<void*>(&texturePlaced);dt[30]=reinterpret_cast<void*>(&textureReserved);
    dt[18]=reinterpret_cast<void*>(&textureSrv);dt[19]=reinterpret_cast<void*>(&textureUav);
    dt[20]=reinterpret_cast<void*>(&textureOtherView<D3D12_RENDER_TARGET_VIEW_DESC>);
    dt[21]=reinterpret_cast<void*>(&textureOtherView<D3D12_DEPTH_STENCIL_VIEW_DESC>);
    TextureDescDispatch descDispatch;
    auto resourceTable=table();resourceTable[10]=(*reinterpret_cast<void***>(&descDispatch))[10];
    FakeCOM device{dt.data()},textures[4]{};
    for(auto& texture:textures)texture.table=resourceTable.data();auto* d=reinterpret_cast<ID3D12Device*>(&device);
    g_protect=protect;
    {
        std::lock_guard lock(s.mutex);auto& hooks=s.devices[dt.data()];
        CHECK(patch(dt.data(),27,reinterpret_cast<void*>(&createBindingResource),hooks));
        CHECK(patch(dt.data(),29,reinterpret_cast<void*>(&unsupportedPlaced),hooks));
        CHECK(patch(dt.data(),30,reinterpret_cast<void*>(&unsupportedReserved),hooks));
        CHECK(patch(dt.data(),18,reinterpret_cast<void*>(&writeBindingSrv),hooks));
        CHECK(patch(dt.data(),19,reinterpret_cast<void*>(&writeBindingUav),hooks));
        CHECK(patch(dt.data(),20,reinterpret_cast<void*>(&unsupportedView<20,D3D12_RENDER_TARGET_VIEW_DESC>),hooks));
        CHECK(patch(dt.data(),21,reinterpret_cast<void*>(&unsupportedView<21,D3D12_DEPTH_STENCIL_VIEW_DESC>),hooks));
    }
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=64;desc.Height=32;desc.DepthOrArraySize=1;desc.MipLevels=3;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    textureWriteDesc=desc;textureDescCalls=0;
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;
    for(unsigned seam=0;seam<3;++seam) {
        result=S_FALSE;output=&textures[seam];void* returned=nullptr;
        auto before=protects;
        auto hr=seam==0?d->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned):
            seam==1?d->CreatePlacedResource(nullptr,0,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned):
            d->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned);
        CHECK(hr==S_FALSE&&returned==output&&textures[seam].refs==1&&protects==before);
        auto& observer=s.nativeBindings;
        if(!seam)observer.heap({1,1000,2000,4,32,0,1,0},reinterpret_cast<uintptr_t>(d));
        d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(returned),nullptr,{1000});
        auto loc=observer.diagnostic()->bound(1,2000,reinterpret_cast<uintptr_t>(d));
        auto snapshot=observer.diagnostic_snapshot(loc,0);
        CHECK(snapshot.texture.hit&&snapshot.texture.resolved==(seam==0)&&snapshot.texture.mips==3&&!snapshot.texture.lifetime_known);
        CHECK(!observer.resource_token(reinterpret_cast<uintptr_t>(returned)).id&&observer.enabled());
        CHECK(snapshot.view_write.observed&&snapshot.view_write.dimension==D3D12_SRV_DIMENSION_TEXTURE2D&&
            !snapshot.view_write.reasons&&snapshot.view_write.ordinal==snapshot.ordinal);
        CHECK(snapshot.dimension==0&&snapshot.format==0); // legacy null-desc diagnostics
    }
    result=E_FAIL;output=&textures[3];void* returned=nullptr;
    CHECK(d->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned)==E_FAIL);
    CHECK(returned==output); // deliberately hostile failed native output
    d->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(returned),nullptr,{1000});
    auto loc=s.nativeBindings.diagnostic()->bound(1,2000,reinterpret_cast<uintptr_t>(d));
    CHECK(!s.nativeBindings.diagnostic_snapshot(loc,0).texture.hit);
    char counts[438]{};s.nativeBindings.append_texture_diagnostics(counts,sizeof counts);
    CHECK(std::strstr(counts,";tr=3;tx=0;uv=1"));
    auto unknown=reinterpret_cast<ID3D12Resource*>(&textures[3]);
    d->CreateShaderResourceView(unknown,nullptr,{1000});
    d->CreateUnorderedAccessView(unknown,unknown,nullptr,{1000});
    d->CreateRenderTargetView(unknown,nullptr,{1000});
    d->CreateDepthStencilView(unknown,nullptr,{1000});
    counts[0]=0;s.nativeBindings.append_texture_diagnostics(counts,sizeof counts);
    CHECK(std::strstr(counts,";tr=3;tx=0;uv=6")&&textureViewCalls==8);
    CHECK(textureDescCalls==5);
    // Explicit description and non-visible/unknown destinations never call GetDesc.
    const auto descBefore=textureDescCalls;
    D3D12_SHADER_RESOURCE_VIEW_DESC explicitDesc{};
    explicitDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;explicitDesc.Format=desc.Format;
    d->CreateShaderResourceView(unknown,&explicitDesc,{1000});
    CHECK(!s.nativeBindings.diagnostic_snapshot(loc,0).view_write.observed);
    s.nativeBindings.heap({2,3000,0,4,32,0,0,0},reinterpret_cast<uintptr_t>(d));
    d->CreateShaderResourceView(unknown,nullptr,{3000});
    d->CreateShaderResourceView(unknown,nullptr,{9999});
    d->CreateShaderResourceView(nullptr,nullptr,{1000});
    CHECK(textureDescCalls==descBefore);
    // Whitelist failures retain actual description and an explicit reason bitmap.
    using V=diagnostics::ViewWriteDescription;
    const auto valid=textureWriteDesc;
    for(unsigned reason:{V::wrong_dimension,V::format,V::extent,V::array,V::mips,V::samples,V::layout,V::flags}) {
        textureWriteDesc=valid;
        switch(reason) {
        case V::wrong_dimension:textureWriteDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;break;
        case V::format:textureWriteDesc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;break;
        case V::extent:textureWriteDesc.Width=0;break;
        case V::array:textureWriteDesc.DepthOrArraySize=2;break;
        case V::mips:textureWriteDesc.MipLevels=0;break;
        case V::samples:textureWriteDesc.SampleDesc.Count=2;break;
        case V::layout:textureWriteDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;break;
        case V::flags:textureWriteDesc.Flags=D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;break;
        }
        d->CreateShaderResourceView(unknown,nullptr,{1000});
        const auto snap=s.nativeBindings.diagnostic_snapshot(loc,0);
        CHECK(snap.view_write.observed&&!snap.view_write.dimension&&(snap.view_write.reasons&reason));
    }
    textureWriteDesc=valid;textureWriteDesc.SampleDesc.Quality=1;
    d->CreateShaderResourceView(unknown,nullptr,{1000});
    CHECK(s.nativeBindings.diagnostic_snapshot(loc,0).view_write.reasons==V::samples);
    for(auto typed:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        textureWriteDesc=valid;textureWriteDesc.Format=typed;
        d->CreateShaderResourceView(unknown,nullptr,{1000});
        CHECK(s.nativeBindings.diagnostic_snapshot(loc,0).view_write.dimension==D3D12_SRV_DIMENSION_TEXTURE2D);
    }
    textureWriteDesc=valid;
    d->CreateShaderResourceView(unknown,nullptr,{1000});
    const auto original=s.nativeBindings.diagnostic_snapshot(loc,0).view_write;
    D3D12_CPU_DESCRIPTOR_HANDLE source{1000},destination{1032};
    for(bool simple:{false,true}) {
        s.nativeBindings.copy(1,&destination,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,reinterpret_cast<uintptr_t>(d),simple);
        const auto copy=s.nativeBindings.diagnostic_snapshot(loc,1).view_write;
        CHECK(copy.observed&&copy.ordinal==original.ordinal&&copy.address==original.address&&copy.desc.Width==valid.Width);
    }
    textureWriteDesc.Width=128;d->CreateShaderResourceView(unknown,nullptr,{1000});
    const auto replacement=s.nativeBindings.diagnostic_snapshot(loc,0).view_write;
    CHECK(replacement.ordinal>original.ordinal&&replacement.desc.Width==128);
    CHECK(s.nativeBindings.diagnostic_snapshot(loc,1).view_write.desc.Width==valid.Width);
    char viewText[diagnostics::text_capacity]="t0-resource-description;view-write";
    replacement.append(viewText,sizeof viewText);
    CHECK(std::strstr(viewText,";vo=1;")&&std::strstr(viewText,";dv=1;du=0;life=0;seam=unknown"));
    diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,viewText));log->flush();
    CHECK((io.evidence+io.files[0]).find(viewText)!=std::string::npos); // real logger, not format-only
    V worstWrite;worstWrite.observed=1;worstWrite.ordinal=UINT32_MAX;worstWrite.desc.Width=UINT64_MAX;
    worstWrite.desc.Alignment=UINT64_MAX;worstWrite.desc.Height=UINT32_MAX;worstWrite.desc.DepthOrArraySize=UINT16_MAX;
    worstWrite.desc.MipLevels=UINT16_MAX;worstWrite.desc.SampleDesc={UINT32_MAX,UINT32_MAX};
    worstWrite.desc.Dimension=static_cast<D3D12_RESOURCE_DIMENSION>(UINT32_MAX);
    worstWrite.desc.Format=static_cast<DXGI_FORMAT>(UINT32_MAX);
    worstWrite.desc.Layout=static_cast<D3D12_TEXTURE_LAYOUT>(UINT32_MAX);
    worstWrite.desc.Flags=static_cast<D3D12_RESOURCE_FLAGS>(UINT32_MAX);
    worstWrite.dimension=UINT32_MAX;worstWrite.reasons=UINT32_MAX;
    std::strcpy(viewText,"t0-resource-description;view-write;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295");worstWrite.append(viewText,sizeof viewText);
    CHECK(std::strlen(viewText)<sizeof viewText-1&&std::strstr(viewText,";seam=unknown"));
    auto viewRecord=diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,viewText);
    viewRecord.sequence=UINT64_MAX;CHECK(diagnostics::format_record(viewRecord).valid);
    textureViewCalls=0;
    // Worst-width companion survives complete serialization without truncation.
    using R=diagnostics::TextureDescriptionRegistry;
    R::Resolution worst;worst.hit=worst.resolved=worst.dimension=worst.format=worst.mips=worst.array_size=worst.samples=UINT32_MAX;
    worst.status=0x3ff;auto history=std::make_unique<R>();history->gaps=0x3ff;history->exposed=7;history->count=R::capacity;history->overflow=history->unknown_views=65535;
    char companion[diagnostics::text_capacity]="t0-resource-description;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295";
    worst.append(companion,sizeof companion);history->append(companion,sizeof companion);
    CHECK(std::strlen(companion)<=437&&std::strstr(companion,";shared=32,33:unpatched"));
    auto formatted=diagnostics::record(diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,companion);
    formatted.sequence=UINT64_MAX;CHECK(diagnostics::format_record(formatted).valid);
    set_sink(nullptr);result=S_FALSE;output=&textures[3];
    CHECK(d->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),&returned)==S_FALSE);
    CHECK(!s.nativeBindings.diagnostic()); // default OFF, native result unchanged
    const auto offCalls=textureDescCalls;d->CreateShaderResourceView(unknown,nullptr,{1000});
    CHECK(textureDescCalls==offCalls);
    for(auto& e:s.srvCandidates.entries)if(e.held)e.held->Release();
    s.srvCandidates=std::move(*savedCandidates);
    for(const auto& texture:textures)CHECK(texture.refs==1);
    set_sink(savedSink);s.devices.erase(dt.data());s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    s.creationSightings=*sightings;result=savedResult;output=savedOutput;
}
