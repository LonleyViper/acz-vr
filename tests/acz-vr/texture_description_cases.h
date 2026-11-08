#pragma once
// CPU-only historical metadata. No native device, lifetime or GPU assertion.
static void textureDescriptionCases() {
    using R=acz::diagnostics::TextureDescriptionRegistry;
    auto registry=std::make_unique<R>();
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=256;d.Height=128;d.DepthOrArraySize=1;d.MipLevels=4;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;
    const auto iid=__uuidof(ID3D12Resource);
    for(unsigned seam=0;seam<3;++seam) {
        registry->creation(true,100+seam,7,&d,seam,1,&props,0,&iid);
        CHECK(registry->count==seam+1&&registry->entries[seam].seam==seam);
        auto r=registry->view(100+seam,7,true);
        CHECK(r.hit&&r.resolved==(seam==0)&&r.dimension==(seam==0?D3D12_SRV_DIMENSION_TEXTURE2D:0));
        CHECK(r.format==28&&r.mips==4&&r.array_size==1&&r.samples==1&&!r.lifetime_known);
        CHECK(!registry->view(100+seam,7,false).resolved);
    }
    auto buffer=d;buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    registry->creation(true,500,7,&buffer,0);registry->creation(false,501,7,&d,0);
    registry->creation(true,0,7,&d,0);registry->creation(true,502,7,nullptr,0);
    CHECK(registry->count==3&&!registry->view(500,7,true).hit&&!registry->view(501,7,true).hit);
    CHECK(!registry->view(100,8,true).hit&&!registry->view(999,7,true).hit);
    // Null-desc defaults refuse arrays, multisampling, implicit mips, typeless,
    // depth and planar formats. Historical facts remain explicitly non-owning.
    for(unsigned variant=0;variant<11;++variant) {
        auto bad=d;
        switch(variant) {
        case 0:bad.DepthOrArraySize=2;break;
        case 1:bad.SampleDesc.Count=4;break;
        case 2:bad.SampleDesc.Quality=1;break;
        case 3:bad.MipLevels=0;break;
        case 4:bad.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;break;
        case 5:bad.Format=DXGI_FORMAT_D32_FLOAT;break;
        case 6:bad.Format=DXGI_FORMAT_NV12;break;
        case 7:bad.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;bad.DepthOrArraySize=8;break;
        case 8:bad.Flags=D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;break;
        case 9:bad.Flags=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;break;
        case 10:bad.Flags=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;break;
        }
        registry->creation(true,600+variant,7,&bad,0,1,&props,0,&iid);
        auto r=registry->view(600+variant,7,true);CHECK(r.hit&&!r.resolved&&!r.lifetime_known);
        if(variant==7)CHECK(r.array_size==1); // depth is not array size
    }
    registry->creation(true,100,7,&d,0);auto duplicate=registry->view(100,7,true);
    CHECK(duplicate.hit&&!duplicate.resolved&&!duplicate.lifetime_known);
    auto narrow=std::make_unique<R>();
    for(unsigned variant=0;variant<7;++variant) {
        auto shape=d;auto heap=props;unsigned flags=0;const GUID* output=&iid;bool overlap=false;
        switch(variant) {
        case 0:shape.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;break;
        case 1:flags=D3D12_HEAP_FLAG_SHARED;break;
        case 2:flags=D3D12_HEAP_FLAG_HARDWARE_PROTECTED;break;
        case 3:heap.Type=D3D12_HEAP_TYPE_CUSTOM;break;
        case 4:output=nullptr;break;
        case 5:heap.VisibleNodeMask=3;break;
        case 6:overlap=true;break;
        }
        narrow->creation(true,700+variant,7,&shape,0,1,&heap,flags,output,overlap);
        auto r=narrow->view(700+variant,7,true);CHECK(r.hit&&!r.resolved&&!r.lifetime_known);
    }
    CHECK(narrow->gaps&R::callback_overlap);
    CHECK(narrow->entries[0].canonical==0&&narrow->entries[0].device_generation==0);
    for(uint64_t i=registry->count;i<R::capacity;++i)registry->creation(true,10000+i,7,&d,0);
    CHECK(registry->count==R::capacity);
    registry->creation(true,20000,7,&d,2);CHECK(registry->count==R::capacity&&registry->overflow==1);
    CHECK(!registry->view(20000,7,true).hit);
    registry->overflow=65535;registry->creation(true,20001,7,&d,2);CHECK(registry->overflow==65535);
    registry->unknown_views=65535;registry->view(30000,7,true);CHECK(registry->unknown_views==65535);
    char text[448]="t0-resource-description;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295";
    R::Resolution worst;worst.hit=worst.resolved=worst.dimension=worst.format=worst.mips=worst.array_size=worst.samples=UINT32_MAX;
    worst.append(text,sizeof text);registry->append(text,sizeof text);
    CHECK(std::strlen(text)<sizeof(text)-1&&std::strstr(text,";rl=0")&&std::strstr(text,";shared=32,33:unpatched"));
    CHECK(std::strstr(text,"Device4:53,55,Device8:69,70,Device10:76,77,78"));

    using D=acz::diagnostics::DescriptorTableDiagnostics;
    acz::binding::NativeBindingObserver observer;
    observer.diagnostic_texture(true,99,7,&d,0,1,&props,0,&iid);CHECK(!observer.diagnostic()); // switch OFF
    observer.diagnostic_tracking(true);observer.heap({1,1000,2000,4,32,0,1,0},7);
    observer.heap({2,3000,0,4,32,0,0,0},7);
    observer.diagnostic_texture(true,99,7,&d,0,1,&props,0,&iid);
    observer.srv(99,nullptr,3000,7);
    D3D12_CPU_DESCRIPTOR_HANDLE dest{1000},source{3000};
    observer.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7,true);
    auto loc=observer.diagnostic()->bound(1,2000,7);
    auto snap=observer.diagnostic_snapshot(loc,0);
    CHECK(snap.texture.hit&&snap.texture.resolved&&snap.texture.mips==4&&!snap.texture.lifetime_known);
    CHECK(snap.dimension==0&&snap.format==0&&snap.desc==1); // original fields unchanged
    CHECK((snap.flags&(D::unsupported_srv|D::unknown_resource))==(D::unsupported_srv|D::unknown_resource));
    CHECK(!observer.resource_token(99).id); // never enters buffer eligibility
    observer.srv(999,nullptr,1000,7);snap=observer.diagnostic_snapshot(loc,0);
    CHECK(!snap.texture.hit&&!snap.texture.resolved&&!snap.texture.lifetime_known);
    observer.diagnostic_view(998,7);char counts[438]{};observer.append_texture_diagnostics(counts,sizeof counts);
    CHECK(std::strstr(counts,";uv=2"));
    // Reuse and observed retirement invalidate copied derived facts without a Draw scan.
    observer.srv(99,nullptr,1000,7);CHECK(observer.diagnostic_snapshot(loc,0).texture.resolved);
    observer.invalidate_object(99);CHECK(!observer.diagnostic_snapshot(loc,0).texture.resolved);
    observer.diagnostic_texture(true,99,7,&d,0,1,&props,0,&iid);
    observer.srv(99,nullptr,1000,7);CHECK(!observer.diagnostic_snapshot(loc,0).texture.resolved);
    observer.diagnostic_device_interface(1);observer.diagnostic_device_interface(4);
    counts[0]=0;observer.append_texture_diagnostics(counts,sizeof counts);CHECK(std::strstr(counts,";cv=7;qe=5"));
    observer.diagnostic_tracking(false);observer.srv(99,nullptr,1000,7);
    CHECK(!observer.diagnostic());
}
