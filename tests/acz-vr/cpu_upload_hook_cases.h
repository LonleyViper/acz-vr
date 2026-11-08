#pragma once
// Included within metadata_fixture, with the existing synthetic covered t0/s0.
static unsigned cpuAdds=0,cpuReads=0;
static bool cpuCountingReader(const void* from,void* to,size_t n) noexcept {
    CHECK(mutex_depth);++cpuReads;std::memcpy(to,from,n);return true;
}
static ULONG STDMETHODCALLTYPE cpuAdd(IUnknown* raw) {
    CHECK(!mutex_depth);CHECK(reinterpret_cast<FakeCOM*>(raw)->refs>0);++cpuAdds;return ++reinterpret_cast<FakeCOM*>(raw)->refs;
}
static void* cpuMapPointer=nullptr;
static HRESULT STDMETHODCALLTYPE cpuNativeMap(ID3D12Resource*,UINT,const D3D12_RANGE*,void** out) {
    CHECK(!mutex_depth);*out=cpuMapPointer;return S_OK;
}
static void STDMETHODCALLTYPE cpuNativeUnmap(ID3D12Resource*,UINT,const D3D12_RANGE*) {CHECK(!mutex_depth);}
__declspec(noinline) static void cpuHookCases(TestPaths& paths,ID3D12GraphicsCommandList* l,uint64_t device) {
    auto& s=state();auto& list=s.states.at(l);
    auto savedList=std::make_unique<ListState>(list);
    auto savedPins=std::make_unique<decltype(s.ownershipPins)>(s.ownershipPins);
    auto savedUploads=std::make_unique<decltype(s.uploadPins)>(s.uploadPins);
    s.uploadPins=*std::make_unique<decltype(s.uploadPins)>();
    auto savedL3=std::make_unique<diagnostics::L3Observation>(s.l3);
    auto savedCpu=std::make_unique<diagnostics::CpuUploadSnapshot>(s.cpuSnapshot);
    auto& pipeline=s.pipelines.at(list.pso.get());
    const auto savedCut=pipeline.strip_cut,savedType=pipeline.topology_type;
    pipeline.strip_cut=1;pipeline.topology_type=3;
    auto pipelineCopy=std::make_unique<PipelineRecord>(pipeline);
    CHECK(pipelineCopy->strip_cut==1&&pipelineCopy->topology_type==3);
    auto* savedSink=diagnostics::sink();const auto attempts=s.requirements_attempts;
    const auto last=s.requirements_last_us,savedTick=tick;
    auto rt=hook_fixture::table();rt[1]=reinterpret_cast<void*>(&cpuAdd);
    FakeCOM upload{rt.data()},indices{rt.data()};
    const bool hadHooks=s.objects.count(rt.data())!=0;
    auto savedHooks=std::make_unique<Hooks>();if(hadHooks)*savedHooks=s.objects.at(rt.data());
    s.objects[rt.data()].originals[8]=reinterpret_cast<void*>(&cpuNativeMap);
    s.objects[rt.data()].originals[9]=reinterpret_cast<void*>(&cpuNativeUnmap);
    rt[8]=reinterpret_cast<void*>(&l3Map);rt[9]=reinterpret_cast<void*>(&l3Unmap);
    auto dataStorage=std::make_unique<std::array<unsigned char,4096>>();
    auto indexStorage=std::make_unique<std::array<unsigned char,4096>>();
    auto& data=*dataStorage;auto& indexData=*indexStorage;
    for(size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>(i);
    const uint16_t values[]={0,1,2,3,0xffff,4,5,6,7};std::memcpy(indexData.data()+32,values,sizeof values);
    const uint64_t source=reinterpret_cast<uintptr_t>(&upload),ibSource=reinterpret_cast<uintptr_t>(&indices);
    s.nativeBindings.resource({source,0x131000,4096,1,false},device);
    s.nativeBindings.resource({ibSource,0x141000,4096,1,false},device);
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4096;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    s.l3.enabled=true;s.l3.create(source,&desc,1,&heap);s.l3.create(ibSource,&desc,1,&heap);
    // Positive fixture explicitly supplies complete installed Map + Unmap coverage.
    for(auto address:{source,ibSource}) {
        auto* r=s.l3.resources.get(address);r->hooks=true;r->map_table=rt.data();
    }
    s.l3.resources.get(source)->gpu_va=0x131000;s.l3.resources.get(ibSource)->gpu_va=0x141000;
    s.cpuSnapshot.~CpuUploadSnapshot();new (&s.cpuSnapshot) diagnostics::CpuUploadSnapshot;
    l->SetPipelineState(list.pso.get());
    CHECK(list.bindings.pipeline.strip_cut==1&&list.bindings.pipeline.topology_type==3);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    l->SetGraphicsRootShaderResourceView(2,0x131040);
    D3D12_VERTEX_BUFFER_VIEW vb{0x1311e0,512,64};D3D12_INDEX_BUFFER_VIEW ib{0x141020,18,DXGI_FORMAT_R16_UINT};
    l->IASetVertexBuffers(0,1,&vb);l->IASetIndexBuffer(&ib);
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());cpuAdds=0;
    auto draw=[&] {
        s.requirements_attempts=0;s.requirements_last_us=0;tick+=requirements_interval_us;
        const auto calls=io.calls;l->DrawIndexedInstanced(9,1,0,0,0);CHECK(calls==io.calls);
        CHECK(!list.bindings.pipeline.requirements_proven&&!capture_enablement::decide({}).enabled);log->flush();
    };
    // Model native creation return BEFORE the last game reference disappears.
    // Exercise the actual Map hook with a CPU-only native forwarding seam.
    auto acquire=[&](FakeCOM& object,uint64_t address) {
        void* out=&object;pinUploadCreation(S_OK,&out,__uuidof(ID3D12Resource),&desc,&heap);
        const auto* r=s.l3.resources.get(address);CHECK(r);
        bindUploadCreation_locked(address);
        cpuMapPointer=address==source?data.data():indexData.data();void* mapped=nullptr;
        CHECK(l3Map(reinterpret_cast<ID3D12Resource*>(&object),0,nullptr,&mapped)==S_OK);
        CHECK(mapped==cpuMapPointer&&r->outstanding==1&&r->map_generation&&r->mapped_lifetime==s.nativeBindings.resource_token(address).generation);
    };
    acquire(upload,source);acquire(indices,ibSource);
    CHECK(cpuAdds==2&&upload.refs==2&&indices.refs==2);
    reinterpret_cast<IUnknown*>(&upload)->Release();reinterpret_cast<IUnknown*>(&indices)->Release();
    CHECK(upload.refs==1&&indices.refs==1);
    draw();
    CHECK(cpuAdds==2&&upload.refs==1&&indices.refs==1&&s.cpuSnapshot.attempts==1);
    CHECK(s.cpuSnapshot.stable&&std::string_view(s.cpuSnapshot.reason)=="snapshot-stable");
    CHECK(io.evidence.find("pin=1;mapped=1;stable=1;first=snapshot-stable")!=io.evidence.npos);
    CHECK(io.evidence.find("metadata-proved:index-range-verified")!=io.evidence.npos);
    CHECK(io.evidence.find("bytes=index-verified")!=io.evidence.npos);
    CHECK(s.cpuSnapshot.minimum==0&&s.cpuSnapshot.maximum==7&&s.cpuSnapshot.vertices==8);
    CHECK(io.raw[0].size()==diagnostics::CpuUploadSnapshot::header_size+946);
    CHECK(!std::memcmp(io.raw[0].data()+diagnostics::CpuUploadSnapshot::header_size,data.data()+64,416));
    // Pure lookup rejects either resource-generation incarnation or mapped record.
    // Match the returned-but-open Draw interval used by native currentness checks.
    {
        ConstantCall interval(l);interval.returned();std::lock_guard lock(s.mutex);
        const auto& q=s.cpuSnapshot.completed;CHECK(cpuRequestCurrent_locked(l,q));
        CHECK(cpuMapCoverage_locked(s.l3.resources.get(source)));
        auto bad=q;++bad.ranges[2].lifetime;CHECK(!cpuRequestCurrent_locked(l,bad));
        bad=q;++bad.ranges[2].map_generation;CHECK(!cpuRequestCurrent_locked(l,bad));
        for(auto& entry:s.uploadPins.entries)if(entry.address==ibSource) {
            ++entry.creation;CHECK(!cpuRequestCurrent_locked(l,q));--entry.creation;
            const auto* pointer=entry.mapped;entry.mapped=nullptr;
            CHECK(!cpuRequestCurrent_locked(l,q));entry.mapped=pointer;
        }
    }
    // Force the actual installation sequence: Map succeeds, Unmap protection
    // fails. The reader must never run; raw and index proof remain absent.
    {
        auto q=std::make_unique<diagnostics::CpuUploadSnapshot::Request>(s.cpuSnapshot.completed);
        const auto reader=acz::g_cpuReader;acz::g_cpuReader=cpuCountingReader;cpuReads=0;
        const auto protects=hook_fixture::protects;auto protectSaved=g_protect;g_protect=hook_fixture::protect;
        rt[8]=reinterpret_cast<void*>(&cpuNativeMap);rt[9]=reinterpret_cast<void*>(&cpuNativeUnmap);
        rt[12]=reinterpret_cast<void*>(&hook_fixture::unused);
        hook_fixture::failProtect=protects+3; // Map's two protects succeed, first Unmap protect fails
        void* output=&indices;l3Created(S_OK,&output,__uuidof(ID3D12Resource),&desc,1,&heap,false);
        hook_fixture::failProtect=0;g_protect=protectSaved;
        CHECK(rt[8]==reinterpret_cast<void*>(&l3Map)&&rt[9]==reinterpret_cast<void*>(&cpuNativeUnmap));
        CHECK(!s.l3.resources.get(ibSource)->hooks);
        const auto raw=io.raw;const auto evidenceBefore=io.evidence.size();
        {
            ConstantCall interval(l);interval.returned();
            CHECK(!cpuRequestCurrent_locked(l,*q));finishCpuSnapshot(l,*q);
            CHECK(!cpuReads&&!s.cpuSnapshot.stable&&!s.cpuSnapshot.file_bytes);
            CHECK(std::string_view(s.cpuSnapshot.reason)=="map-unmap-coverage-incomplete");
            CHECK(std::string_view(s.cpuSnapshot.index_reason)=="not-evaluated");
        }
        log->flush();CHECK(io.raw==raw);
        CHECK(io.evidence.substr(evidenceBefore).find("bytes=index-verified")==std::string::npos);
        // Restored flag alone cannot authorize a read after live slot coverage is lost.
        s.l3.resources.get(ibSource)->hooks=true;
        {ConstantCall interval(l);interval.returned();finishCpuSnapshot(l,*q);CHECK(!cpuReads&&!s.cpuSnapshot.file_bytes);}
        rt[9]=reinterpret_cast<void*>(&l3Unmap);acz::g_cpuReader=reader;
    }
    // Same pointer, new Map generation: the old request remains unusable.
    {
        const auto generation=s.l3.resources.get(ibSource)->map_generation;
        l3Unmap(reinterpret_cast<ID3D12Resource*>(&indices),0,nullptr);
        cpuMapPointer=indexData.data();void* mapped=nullptr;
        CHECK(l3Map(reinterpret_cast<ID3D12Resource*>(&indices),0,nullptr,&mapped)==S_OK);
        CHECK(mapped==indexData.data()&&s.l3.resources.get(ibSource)->map_generation>generation);
    }
    draw();CHECK(cpuAdds==2&&s.cpuSnapshot.attempts==2);
    const auto reader=acz::g_cpuReader;
    acz::g_cpuReader=+[](const void*,void*,size_t) noexcept {return false;};
    draw();acz::g_cpuReader=reader;
    CHECK(s.cpuSnapshot.attempts==3&&std::string_view(s.cpuSnapshot.reason)=="read-fault"&&io.raw[2].empty());
    l3Unmap(reinterpret_cast<ID3D12Resource*>(&indices),0,nullptr);draw();
    CHECK(s.cpuSnapshot.attempts==4&&!s.cpuSnapshot.file_bytes);
    CHECK(std::string_view(s.cpuSnapshot.reason)=="upload-pin-map-mismatch");
    draw();CHECK(s.cpuSnapshot.attempts==4&&cpuAdds==2);
    CHECK(!log->counters().io_failures);
    MemoryIO off;off.readState=ReadState::ok;off.config="log_enabled=true";
    auto offLog=std::make_unique<Log>(paths,off,clockNow,true);offLog->flush();set_sink(offLog.get());
    const auto before=s.cpuSnapshot.attempts;draw();CHECK(s.cpuSnapshot.attempts==before&&cpuAdds==2);
    offLog->flush();CHECK(off.evidence.find("text=cpu-snapshot;")==off.evidence.npos);
    set_sink(savedSink);
    const auto priorGeneration=list.bindings.generation;
    CHECK(l->Reset(nullptr,nullptr)==S_OK);
    CHECK(list.bindings.generation!=priorGeneration&&!list.bindings.topology&&
        list.bindings.pipeline.strip_cut==index_range::unknown_cut);
    // Fake fixture cleanup only. No reference decrement on the production pins.
    s.nativeBindings.remove_resource(s.nativeBindings.resource_token(source));
    s.nativeBindings.remove_resource(s.nativeBindings.resource_token(ibSource));
    for(size_t i=0;i<s.uploadPins.capacity;++i)if(auto* owned=s.uploadPins.take(i))owned->Release();
    CHECK(upload.refs==0&&indices.refs==0);s.uploadPins=*savedUploads;
    if(hadHooks)s.objects[rt.data()]=*savedHooks;else s.objects.erase(rt.data());cpuMapPointer=nullptr;
    s.ownershipPins=*savedPins;s.l3=*savedL3;s.cpuSnapshot=*savedCpu;list=*savedList;
    pipeline.strip_cut=savedCut;pipeline.topology_type=savedType;
    s.requirements_attempts=attempts;s.requirements_last_us=last;tick=savedTick;
}
