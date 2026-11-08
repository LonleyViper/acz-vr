#pragma once
// Called after a synthetic t0 ownership pin is established. Installed setters,
// Draw, metadata checker and production logger; no runtime parity is implied.
__declspec(noinline) static void s0SamplerCases(TestPaths& paths,ID3D12GraphicsCommandList* l,uint64_t device) {
    using D=diagnostics::DescriptorTableDiagnostics;using S=D::Snapshot;
    auto& s=state();auto& list=s.states.at(l);
    const auto savedList=std::make_unique<ListState>(list);
    auto* diag=const_cast<D*>(s.nativeBindings.diagnostic());CHECK(diag);
    const auto savedDiag=std::make_unique<D>(*diag);
    auto* sink=diagnostics::sink();
    CHECK(list.bindings.heaps.size()==2);
    const auto heap=list.bindings.heaps[1];
    diag->heap(heap,device,5000,20000,1,32,1,1);
    D3D12_SAMPLER_DESC desc{};
    desc.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU=D3D12_TEXTURE_ADDRESS_MODE_WRAP;desc.AddressV=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_MIRROR;desc.MipLODBias=-0.5f;
    desc.MaxAnisotropy=4;desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BorderColor[0]=0.25f;desc.BorderColor[1]=0.5f;desc.BorderColor[2]=0.75f;desc.BorderColor[3]=1;
    desc.MinLOD=0;desc.MaxLOD=16;
    s.nativeBindings.sampler(&desc,5000,device);
    desc.MaxLOD=999; // The creator's pointer must not survive publication.
    l->SetGraphicsRootDescriptorTable(1,{20000});
    const auto loc=list.samplerWitness.location;
    CHECK(list.samplerWitness.written&&list.samplerWitness.parameter==1&&loc.type==1);
    auto& slot=diag->slots.at({loc.serial,loc.slot});const auto savedSlot=slot;
    CHECK(slot.sampler_observed&&slot.sampler_words[12]==0x41800000u);
    const std::array<uint32_t,13> expected{0x15,1,3,2,0xbf000000u,4,8,
        0x3e800000u,0x3f000000u,0x3f400000u,0x3f800000u,0,0x41800000u};
    CHECK(slot.sampler_words==expected);
    {
        const auto snapshot=diag->snapshot(loc,0,D::sampler);
        CHECK(snapshot.sampler_words==&slot.sampler_words);
        CHECK(*snapshot.sampler_words==expected);
        auto missing=snapshot;missing.sampler_words=nullptr;
        missing.classify(diag->write_ordinal,diag->write_ordinal,diag->write_ordinal,true);
        CHECK(missing.sampler_reason()==S::sampler_unobserved);
    }
    auto draw=[&](unsigned reason,const char* next,bool on=true) {
        MemoryIO io;io.readState=ReadState::ok;
        io.config=on?"log_enabled=true\ndiagnostic_ownership_pins=true":"log_enabled=true";
        auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
        s.requirements_attempts=0;s.requirements_last_us=0;tick+=requirements_interval_us;
        l->DrawIndexedInstanced(9,1,0,0,0);log->flush();
        CHECK(!log->counters().io_failures);
        // Both first companions must survive through the REAL evidence writer.
        if(on) {
            CHECK(linesWith(io.evidence,"text=s0-table-witness;draw=1,9,1,0,0,0;")==1);
            CHECK(linesWith(io.evidence,"text=s0-description;draw=1,9,1,0,0,0;")==1);
            char why[24]{};std::snprintf(why,sizeof why,";sr=%x;",reason);
            const auto description=io.evidence.find("text=s0-description;");
            const auto end=io.evidence.find('\n',description);
            const auto persisted=io.evidence.substr(description,end-description);
            if(persisted.find(why)==persisted.npos) {
                std::fprintf(stderr,"s0 fixture expected %s next=%s; persisted evidence:\n%s\n",
                    why,next?next:"(none)",io.evidence.c_str());
                std::fflush(stderr);
            }
            CHECK(persisted.find(why)!=persisted.npos);
        } else CHECK(io.evidence.find("text=s0-")==io.evidence.npos);
        if(next)CHECK(io.evidence.find(next)!=io.evidence.npos);
        CHECK(!list.bindings.pipeline.requirements_proven&&!capture_enablement::decide({}).enabled);
        set_sink(sink);
    };
    // Existing t10 metadata/span and cb10 already pass; after s0 the next
    // unresolved condition is the indexed draw's content-dependent index range.
    draw(S::sampler_covered,"metadata-proved:index-range-unverified-without-bytes");
    draw(S::sampler_covered,";cs=covered;cn=index");
    cpuHookCases(paths,l,device);
    // A fabricated ordinal beyond the Draw entry's publication clock has
    // unknown ordering, so it must refuse as uncovered, before desc matching.
    ++slot.ordinal;draw(S::sampler_uncovered,";cs=covered;cn=s0");slot=savedSlot;
    // Publish the replacement inside the observed interval. Keep the old
    // description ordinal to exercise the distinct replaced-desc refusal.
    slot.ordinal=++diag->write_ordinal;
    draw(S::sampler_replaced,";cs=covered;cn=s0");slot=savedSlot;
    slot.sampler_observed=false;draw(S::sampler_unobserved,";cs=covered;cn=s0");slot=savedSlot;
    diag->heaps.at(heap).live=false;draw(S::sampler_retired,";cs=covered;cn=s0");diag->heaps.at(heap).live=true;
    ++diag->heaps.at(heap).serial;draw(S::sampler_retired,";cs=covered;cn=s0");--diag->heaps.at(heap).serial;
    // Wrong class at cached bind is fail closed even if sampler words exist.
    list.samplerWitness.location.type=0;draw(S::sampler_wrong_heap,";cs=covered;cn=s0");list.samplerWitness.location=loc;
    slot.type=D::cbv;draw(S::sampler_wrong_type,";cs=covered;cn=s0");slot=savedSlot;
    // Real observer copy: destination gets a new ordinal, source desc stays
    // unchanged. Neither ranges nor simple copies certify a sampler replacement.
    s.nativeBindings.heap({0x129,6000,0,1,32,1,0,0},device);
    s.nativeBindings.sampler(&desc,6000,device);
    D3D12_CPU_DESCRIPTOR_HANDLE dest{5000},source{6000};
    const auto sourceLoc=diag->locate(6000,device,false);
    const auto sourceOrdinal=diag->slots.at({sourceLoc.serial,0}).ordinal;
    for(bool simple:{false,true}) {
        s.nativeBindings.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,device,simple);
        CHECK(slot.writer==(simple?D::copy_simple:D::copy_ranges));
        CHECK(slot.source_heap==sourceLoc.serial&&slot.source_class==1&&slot.source_slot==0);
        CHECK(diag->slots.at({sourceLoc.serial,0}).ordinal==sourceOrdinal);
        draw(S::sampler_copy,";cs=covered;cn=s0");slot=savedSlot;
    }
    // Subsequent direct creation safely replaces old copied history.
    s.nativeBindings.sampler(&desc,5000,device);
    draw(S::sampler_covered,";cs=covered;cn=index");
    {
        // Hypothetical static variant, CPU SDK serializer/deserializer only.
        // Keep E10 t0/t10/cb10 indices stable; parameter 1 becomes unused b33.
        D3D12_ROOT_PARAMETER1 parameters[4]{};
        D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,D3D12_DESCRIPTOR_RANGE_FLAG_NONE,0};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;parameters[1].Constants={33,0,1};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor={10,0,D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={10,0,2};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=desc.Filter;sampler.AddressU=desc.AddressU;sampler.AddressV=desc.AddressV;sampler.AddressW=desc.AddressW;
        sampler.MipLODBias=desc.MipLODBias;sampler.MaxAnisotropy=desc.MaxAnisotropy;sampler.ComparisonFunc=desc.ComparisonFunc;
        sampler.MinLOD=desc.MinLOD;sampler.MaxLOD=desc.MaxLOD;sampler.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root{};root.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
        root.Desc_1_1={4,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAGS(0x30d)};
        ID3DBlob* bytes=nullptr;ID3DBlob* errors=nullptr;
        CHECK(SUCCEEDED(D3D12SerializeVersionedRootSignature(&root,&bytes,&errors))&&bytes);
        if(errors)errors->Release();
        auto off=metadata::RootBindingMetadata::from_serialized(bytes->GetBufferPointer(),bytes->GetBufferSize());
        CHECK(!off.valid());
        auto decoded=metadata::RootBindingMetadata::from_serialized(bytes->GetBufferPointer(),bytes->GetBufferSize(),true);
        bytes->Release();CHECK(decoded.valid()&&decoded.metadata->serialized_ps_s0());
        CHECK((*decoded.metadata->serialized_ps_s0())[10]==0x3f800000u);
        const auto savedRoot=list.bindings.root;list.bindings.root=*decoded.metadata;
        draw(S::sampler_static,";cs=covered;cn=index");
        // The switch still refuses at cb10 and emits no companions.
        draw(S::sampler_static,"rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported",false);
        list.bindings.root=savedRoot;
    }
    auto coveredList=std::make_unique<ListState>(list);
    l->ExecuteBundle(nullptr);draw(S::sampler_uncovered,nullptr);list=*coveredList;
    l->ExecuteIndirect(nullptr,1,nullptr,0,nullptr,0);draw(S::sampler_uncovered,nullptr);list=*coveredList;
    ++list.samplerWitness.generation;draw(S::sampler_uncovered,";cs=covered;cn=s0");list=*coveredList;
    ++list.samplerWitness.thread;draw(S::sampler_uncovered,";cs=covered;cn=s0");list=*coveredList;
    list.samplerWitness.revision=UINT64_MAX;draw(S::sampler_uncovered,";cs=covered;cn=s0");list=*coveredList;
    list.samplerWitness.written=false;draw(S::sampler_uncovered,";cs=covered;cn=s0");list=*coveredList;
    // Heap history completeness is global: incomplete history invalidates the t0
    // table/slot witness first (t0 is evaluated before s0), so the refusal is cn=t0
    // while the s0 companion still reports sampler_uncovered.
    diag->history_complete=false;draw(S::sampler_uncovered,";cs=covered;cn=t0");diag->history_complete=true;
    draw(S::sampler_covered,"rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported",false);
    s.nativeBindings.remove_heap(s.nativeBindings.heap_token(0x129));
    D3D12_SAMPLER_DESC original{};s.nativeBindings.sampler(&original,5000,device);
    *diag=std::move(*savedDiag);list=*savedList;set_sink(sink);
}
