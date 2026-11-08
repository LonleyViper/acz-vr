#pragma once
// Included within metadata_fixture; fake objects exercise the installed Draw path.
static bool t0Throw=false;
static unsigned t0AddCalls=0,t0ReleaseCalls=0;
static std::function<void()> t0ReleaseReentry;
static ULONG STDMETHODCALLTYPE t0Add(IUnknown* self) {
    CHECK(!mutex_depth);CHECK(reinterpret_cast<FakeCOM*>(self)->refs>0);++t0AddCalls;
    if(t0Throw)throw std::runtime_error("injected t0 AddRef exception");
    return ++reinterpret_cast<FakeCOM*>(self)->refs;
}
static ULONG STDMETHODCALLTYPE t0Release(IUnknown* self) {
    CHECK(!mutex_depth);auto& refs=reinterpret_cast<FakeCOM*>(self)->refs;
    CHECK(refs>0);++t0ReleaseCalls;
    // One-shot callback permits deterministic nested Draw/retirement dispatch.
    auto reentry=std::move(t0ReleaseReentry);t0ReleaseReentry={};
    if(reentry)reentry();
    return --refs;
}
// Pure slot storage + logical COM object test. Zero-ref FakeCOM objects reject every
// subsequent AddRef/Release; this is lifetime bookkeeping, not GPU evidence.
__declspec(noinline) static void srvSlotCandidateCases() {
    using Ring=diagnostics::SrvSlotCandidates<ID3D12Resource>;
    t0AddCalls=t0ReleaseCalls=0;t0Throw=false;
    auto ring=std::make_unique<Ring>();
    auto objects=std::make_unique<std::array<FakeCOM,1025>>();
    auto table=hook_fixture::table();table[1]=reinterpret_cast<void*>(&t0Add);
    table[2]=reinterpret_cast<void*>(&t0Release);
    for(auto& object:*objects)object.table=table.data();
    auto resource=[&](unsigned i){return reinterpret_cast<ID3D12Resource*>(&(*objects)[i]);};
    auto key=[&](unsigned i){return Ring::Key{reinterpret_cast<uintptr_t>(resource(i)),1,i+1,i};};
    auto acquire=[&](unsigned i) {
        ring->written(1,i);auto r=ring->reserve(key(i));CHECK(r.slot<Ring::capacity);
        resource(i)->AddRef();CHECK(!ring->finish(r,resource(i)));
    };
    auto discard=[&](unsigned i) {
        auto r=ring->discard(1,i);
        if(r.evicted){r.evicted->Release();ring->released(r);}
    };
    for(unsigned i=0;i<1024;++i)acquire(i);
    CHECK(ring->held==1024&&ring->occupancy()==1024&&ring->acquired==1024);
    CHECK(ring->distinct==1024&&ring->max_slot==1023);
    ring->written(1,1024);CHECK(ring->reserve(key(1024)).slot==Ring::capacity);
    CHECK(ring->cap_refused==1&&ring->max_slot==1024&&ring->held==1024);
    // Rewriting another slot thousands of times never evicts the t0 slot.
    for(unsigned i=0;i<2048;++i){discard(1);acquire(1);}
    CHECK((*objects)[0].refs==2&&(*objects)[1].refs==2);
    CHECK(!ring->promote({key(0).address,1,999,0})); // wrong publication
    CHECK(!ring->promote({key(0).address,2,1,0})); // wrong heap generation
    const auto adds=t0AddCalls,releases=t0ReleaseCalls;
    auto* permanent=ring->promote(key(0));CHECK(permanent==resource(0));
    CHECK(t0AddCalls==adds&&t0ReleaseCalls==releases&&(*objects)[0].refs==2);
    CHECK(ring->acquired==ring->releases+ring->promoted+ring->held);
    ring->retire(1);for(unsigned i=0;i<1024;++i)discard(i);
    CHECK(ring->occupancy()==0&&ring->held==0);
    CHECK(ring->acquired==ring->releases+ring->promoted);
    permanent->Release();for(const auto& o:*objects)CHECK(o.refs==1);
    // Astra interleaving: rewrite while AddRef is pending. The reservation
    // cannot be reused; after last game Release only our own Release is legal.
    ring=std::make_unique<Ring>();
    auto pending=ring->reserve(key(0));resource(0)->AddRef();
    auto none=ring->discard(1,0);CHECK(!none.evicted&&ring->occupancy()==1);
    CHECK(ring->reserve(key(0)).slot==Ring::capacity);
    CHECK(resource(0)->Release()==1);
    CHECK(!ring->promote(key(0)));
    auto* cancelled=ring->finish(pending,resource(0));CHECK(cancelled==resource(0));
    CHECK(ring->occupancy()==1);
    CHECK(cancelled->Release()==0);
    ring->released(pending);
    CHECK(!ring->promote(key(0))&&ring->occupancy()==0);
    CHECK(ring->acquired==1&&ring->releases==1&&ring->promoted==0);
    // Retirement alone must cancel a pending acquisition before drain reaches
    // its slot; it cannot publish held/ready or promote the retired generation.
    auto pendingRetire=ring->reserve(key(6));resource(6)->AddRef();
    ring->retire(1);
    cancelled=ring->finish(pendingRetire,resource(6));
    CHECK(cancelled==resource(6)&&ring->held==0);
    CHECK(!ring->promote(key(6)));
    cancelled->Release();ring->released(pendingRetire);
    CHECK(ring->occupancy()==0&&(*objects)[6].refs==1);
    // Reject an already-ready later slot as soon as retirement is marked.
    acquire(7);ring->retire(1);CHECK(!ring->promote(key(7)));discard(7);
    // Reverse ordering: rewrite Release then last game Release, no further COM.
    auto k=key(2);auto r=ring->reserve(k);resource(2)->AddRef();ring->finish(r,resource(2));
    discard(2);CHECK(resource(2)->Release()==0);CHECK(!ring->promote(k));
    // Fault permanently consumes its slot, not another slot or a renewed cap.
    r=ring->reserve(key(3));ring->finish(r,nullptr,true);
    ring->discard(1,3);CHECK(ring->reserve(key(3)).slot==Ring::capacity);
    auto other=ring->reserve(key(4));CHECK(other.slot<Ring::capacity);
    ring->finish(other,nullptr);CHECK(ring->occupancy()==1);
    // All heap reservations are bounded, including calls in flight.
    ring=std::make_unique<Ring>();
    for(unsigned h=1;h<=Ring::heap_capacity;++h)
        CHECK(ring->reserve({h,h,1,0}).slot<Ring::capacity);
    CHECK(ring->reserve({99,99,1,0}).slot==Ring::capacity);
    // Reset/retire cancels in-flight work and frees the generation only after
    // its owned Release completes; a new generation can then use that storage.
    ring->retire(1);none=ring->discard(1,0);CHECK(!none.evicted);
    (*objects)[5].refs=1;resource(5)->AddRef();
    cancelled=ring->finish({0,1,nullptr},resource(5));CHECK(cancelled==resource(5));
    cancelled->Release();ring->released({0,1,nullptr});
    CHECK(ring->reserve({99,99,1,0}).slot<Ring::capacity);
}
__declspec(noinline) static void t0OwnershipCases(TestPaths& paths,ID3D12GraphicsCommandList* l,
                                                uint64_t heap,uint64_t device) {
    srvSlotCandidateCases();
    using D=diagnostics::DescriptorTableDiagnostics;
    auto& s=state();auto& list=s.states.at(l);
    const auto savedList=std::make_unique<ListState>(list);
    const bool hadDiagnostic=s.nativeBindings.diagnostic()!=nullptr;
    s.nativeBindings.diagnostic_tracking(true);
    auto* diag=const_cast<D*>(s.nativeBindings.diagnostic());CHECK(diag);
    auto savedDiag=std::make_unique<D>(*diag);
    auto savedPins=std::make_unique<decltype(s.ownershipPins)>(s.ownershipPins);
    auto savedCandidates=std::make_unique<SrvCandidates>(s.srvCandidates);
    auto savedL3=std::make_unique<diagnostics::L3Observation>(s.l3);
    s.srvCandidates=std::move(*std::make_unique<SrvCandidates>());
    auto* d=reinterpret_cast<ID3D12Device*>(uintptr_t(device));auto** dt=*reinterpret_cast<void***>(d);
    const auto originalSrv=s.devices.at(dt).originals[18];
    s.devices.at(dt).originals[18]=reinterpret_cast<void*>(&hook_fixture::textureSrv);
    const auto viewCallsBefore=hook_fixture::textureViewCalls;
    auto* sink=diagnostics::sink();const auto attempts=s.requirements_attempts;
    const auto last=s.requirements_last_us,savedTick=tick;
    const auto savedConstantCounts=s.constantWitnessCounts;
    const auto savedTableCounts=s.tableWitnessCounts;
    const auto savedCR=s.cb10ConstantDraws,savedCW=s.constantWordCalls;
    // Synthetic complete history, explicitly seeded; never runtime evidence.
    diag->history_complete=true;diag->overflow=diag->gap=false;
    diag->heap(heap,device,1000,10000,64,32,0,1);
    auto rt=hook_fixture::table();rt[1]=reinterpret_cast<void*>(&t0Add);
    rt[2]=reinterpret_cast<void*>(&t0Release);
    hook_fixture::TextureDescDispatch dispatch;
    rt[10]=(*reinterpret_cast<void***>(&dispatch))[10];
    FakeCOM resources[7]{};for(auto& r:resources)r.table=rt.data();
    hook_fixture::textureWriteDesc={};auto& desc=hook_fixture::textureWriteDesc;
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=512;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;
    t0AddCalls=0;t0ReleaseCalls=0;t0Throw=false;
    auto write=[&](unsigned index) {
        auto* resource=reinterpret_cast<ID3D12Resource*>(&resources[index]);
        writeBindingSrv(d,resource,nullptr,{1032});
        l->SetGraphicsRootDescriptorTable(0,{10032});
    };
    auto draw=[&](unsigned reason,const char* next) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
        auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
        s.requirements_attempts=0;s.requirements_last_us=0;tick+=requirements_interval_us;
        l->DrawIndexedInstanced(9,1,0,0,0);log->flush();
        const auto output=io.evidence+io.files[0];
        char op[24]{};std::snprintf(op,sizeof op,";op=%x",reason);
        CHECK(output.find(op)!=output.npos);
        CHECK(output.find(";ca=")!=output.npos&&output.find(";ce=")!=output.npos&&
              output.find(";cp=")!=output.npos&&output.find(";cf=")!=output.npos);
        CHECK(output.find("srv-slot-counts;held=")!=output.npos);
        if(next)CHECK(output.find(next)!=output.npos);
        CHECK(!list.bindings.pipeline.requirements_proven&&!capture_enablement::decide({}).enabled);
        set_sink(sink);
    };
    write(0);
    draw(D::pin_owned,";cs=covered;cn=s0");
    CHECK(t0AddCalls==1&&resources[0].refs==2);
    draw(D::pin_owned,"rejected:s0 missing/stale/unsupported descriptor");
    CHECK(t0AddCalls==1&&resources[0].refs==2); // one permanent +1, never decremented
    s0SamplerCases(paths,l,device);
    const auto loc=list.tableWitness.location;
    auto& slot=diag->slots.at({loc.serial,loc.slot});
    const auto savedSlot=slot;
    ++slot.ordinal; // later publication, description still belongs to old write
    draw(D::pin_overwritten,"rejected:t0 missing/stale/unsupported descriptor");slot=savedSlot;
    // CopyDescriptors replacement also changes destination ordinal, retaining vn.
    slot.writer=D::copy_simple;slot.flags|=D::copied;++slot.ordinal;
    draw(D::pin_overwritten,";cs=covered;cn=t0");slot=savedSlot;
    diag->heaps.at(heap).live=false;
    draw(D::pin_retired,";cs=covered;cn=t0");diag->heaps.at(heap).live=true;
    ++diag->heaps.at(heap).serial;
    draw(D::pin_retired,";cs=covered;cn=t0");--diag->heaps.at(heap).serial;
    slot.view_write.reasons=diagnostics::ViewWriteDescription::format;
    draw(D::pin_shape,";cs=covered;cn=t0");slot=savedSlot;
    slot.view_write.address=0;
    draw(D::pin_address,";cs=covered;cn=t0");slot=savedSlot;
    CHECK(resources[0].refs==2&&t0AddCalls==1);
    const auto saved=std::make_unique<ListState>(list);
    l->ExecuteBundle(nullptr);draw(D::pin_uncovered,nullptr);list=*saved;
    l->ExecuteIndirect(nullptr,1,nullptr,0,nullptr,0);draw(D::pin_uncovered,nullptr);list=*saved;
    // A remains alive ONLY through the candidate after its final game release.
    // Replacing its volatile descriptor dispatches exactly our own Release,
    // logically destroys A, and the old publication can never promote.
    write(1);CHECK(resources[1].refs==2&&t0AddCalls==2);
    const SrvCandidates::Key oldKey{reinterpret_cast<uintptr_t>(&resources[1]),
        loc.serial,diag->slots.at({loc.serial,loc.slot}).ordinal,loc.slot};
    CHECK(t0Release(reinterpret_cast<IUnknown*>(&resources[1]))==1); // last game ref
    const auto releaseBefore=t0ReleaseCalls;
    write(2);CHECK(resources[1].refs==0&&t0ReleaseCalls==releaseBefore+1);
    CHECK(!s.srvCandidates.promote(oldKey)); // no COM on logically dead A
    const auto callsBefore=t0AddCalls;
    draw(D::pin_owned,";cs=covered;cn=s0");
    CHECK(t0AddCalls==callsBefore&&resources[2].refs==2); // transfer, no AddRef
    // A descriptive publication without a held candidate is explicitly refused.
    auto* missing=reinterpret_cast<ID3D12Resource*>(&resources[3]);
    auto view=diagnostics::ViewWriteDescription::read(true,missing);
    s.nativeBindings.srv(reinterpret_cast<uintptr_t>(missing),nullptr,1032,device,&view);
    l->SetGraphicsRootDescriptorTable(0,{10032});
    draw(D::pin_candidate_missing,";cs=covered;cn=t0");
    CHECK(resources[3].refs==1&&t0AddCalls==callsBefore);
    // Faulted live-call acquisition is never retried at Draw.
    t0Throw=true;writeBindingSrv(d,missing,nullptr,{1064});
    l->SetGraphicsRootDescriptorTable(0,{10064});t0Throw=false;
    const auto faultCalls=t0AddCalls;
    draw(D::pin_candidate_missing,";cs=covered;cn=t0");
    draw(D::pin_candidate_missing,";cs=covered;cn=t0");CHECK(t0AddCalls==faultCalls);
    write(4);draw(D::pin_owned,";cs=covered;cn=s0");
    write(5);draw(D::pin_owned,";cs=covered;cn=s0");
    write(6);draw(D::pin_cap,";cs=covered;cn=t0");
    CHECK(resources[6].refs==2); // candidate remains bounded, permanent cap unchanged
    // Exercise the shared actual-hook destination preparation with both copy
    // shapes and a non-SRV write: no AddRef and exactly one owned Release.
    D3D12_CPU_DESCRIPTOR_HANDLE destination{1032},source{1096};UINT one=1;
    for(bool simple:{false,true}) {
        const auto beforeRelease=t0ReleaseCalls,beforeAdd=t0AddCalls;
        {BindingEvent copy(0,true,0,d,1,&destination,&one,1,&source,&one,
            binding::ObservationKind::copy,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
         copy.prepareCandidates();
         s.nativeBindings.copy(1,&destination,&one,1,&source,&one,
             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,device,simple);}
        CHECK(resources[6].refs==1&&t0ReleaseCalls==beforeRelease+1&&t0AddCalls==beforeAdd);
        write(6);CHECK(resources[6].refs==2);
    }
    {const auto beforeRelease=t0ReleaseCalls,beforeAdd=t0AddCalls;
     BindingEvent cbv(0,true,0,d,1,&destination);
     cbv.prepareCandidates();
     CHECK(resources[6].refs==1&&t0ReleaseCalls==beforeRelease+1&&t0AddCalls==beforeAdd);}
    write(6);CHECK(resources[6].refs==2);
    const auto offAdd=t0AddCalls,offRelease=t0ReleaseCalls;
    // Default/OFF does not call AddRef, including a new covered address.
    MemoryIO offIO;offIO.readState=ReadState::ok;offIO.config="log_enabled=true";
    auto offLog=std::make_unique<Log>(paths,offIO,clockNow,true);offLog->flush();set_sink(offLog.get());
    s.requirements_attempts=0;s.requirements_last_us=0;tick+=requirements_interval_us;
    writeBindingSrv(d,reinterpret_cast<ID3D12Resource*>(&resources[6]),nullptr,{1032});
    l->DrawIndexedInstanced(9,1,0,0,0);offLog->flush();
    CHECK(t0AddCalls==offAdd&&t0ReleaseCalls==offRelease&&resources[6].refs==2);
    CHECK(offIO.evidence.find("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported")!=offIO.evidence.npos);
    CHECK(offIO.evidence.find(";op=")==offIO.evidence.npos);
    set_sink(sink);
    // Actual heap retirement helper drains held refs exactly once, including
    // when a different slot in that generation is permanently faulted.
    // Two occupied slots plus an existing permanent pin in the SAME heap.
    // First owned Release re-enters the installed Draw against the later slot
    // and the old permanent pin before the drain reaches either.
    diag->slots.at({loc.serial,loc.slot})=savedSlot; // permanent resources[0]
    diag->history_complete=true; // earlier copy fixtures leave history incomplete; restore for new writes
    writeBindingSrv(d,reinterpret_cast<ID3D12Resource*>(&resources[3]),nullptr,{1096});
    writeBindingSrv(d,reinterpret_cast<ID3D12Resource*>(&resources[6]),nullptr,{1128});
    const auto laterLoc=diag->locate(10128,device,true);
    const SrvCandidates::Key laterKey{reinterpret_cast<uintptr_t>(&resources[6]),
        loc.serial,diag->slots.at({loc.serial,laterLoc.slot}).ordinal,laterLoc.slot};
    // resources[6] also holds the earlier slot-1032 candidate (refs 3).
    CHECK(resources[3].refs==2&&resources[6].refs==3);
    const auto reentryReleases=t0ReleaseCalls,reentryAdds=t0AddCalls;
    unsigned reentered=0;
    l->SetGraphicsRootDescriptorTable(0,{10128});
    t0ReleaseReentry=[&] {
        ++reentered;CHECK(!mutex_depth);
        CHECK(!diag->heaps.at(heap).live);
        CHECK(!s.srvCandidates.promote(laterKey));
        draw(D::pin_retired,";cs=covered;cn=t0"); // table preset to the later slot
        CHECK(resources[0].refs==2); // existing permanent pin remains owned
    };
    retireSrvCandidates(loc.serial,heap);
    CHECK(reentered==1&&!t0ReleaseReentry);
    CHECK(t0ReleaseCalls==reentryReleases+3&&t0AddCalls==reentryAdds);
    CHECK(resources[3].refs==1&&resources[6].refs==1&&resources[0].refs==2);
    retireSrvCandidates(loc.serial,heap);CHECK(t0ReleaseCalls==reentryReleases+3);
    // Old-generation retirement cannot mark a new heap at the same address dead.
    diag->heap(heap,device,1000,10000,64,32,0,1);
    retireSrvCandidates(loc.serial,heap);CHECK(diag->heaps.at(heap).live);
    // Test-only cleanup of candidates; permanent simulated pins stay retained.
    for(auto& e:s.srvCandidates.entries)if(e.held)t0Release(reinterpret_cast<IUnknown*>(e.held));
    s.srvCandidates=std::move(*savedCandidates);s.devices.at(dt).originals[18]=originalSrv;
    hook_fixture::textureViewCalls=viewCallsBefore;s.l3=*savedL3;
    s.ownershipPins=std::move(*savedPins);
    s.nativeBindings.diagnostic_tracking(true);*diag=std::move(*savedDiag);list=*savedList;
    if(!hadDiagnostic)s.nativeBindings.diagnostic_tracking(false);
    s.requirements_attempts=attempts;s.requirements_last_us=last;tick=savedTick;
    s.constantWitnessCounts=savedConstantCounts;s.tableWitnessCounts=savedTableCounts;
    s.cb10ConstantDraws=savedCR;s.constantWordCalls=savedCW;
    CHECK(resources[0].refs==2&&resources[1].refs==0&&resources[2].refs==2&&resources[3].refs==1);
}
