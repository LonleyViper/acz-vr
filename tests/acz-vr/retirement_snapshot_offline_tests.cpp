// Focused CPU/fake-COM lifetime regression with no shader-evidence dependency.
// Reuse the same fake interfaces and checks as the installed-Draw fixture.
#define main j139_existing_main
#pragma warning(push)
#pragma warning(disable:4715)
#include "diagnostics_offline_tests.cpp"
#pragma warning(pop)
#undef main
#include "upload_creation_pin_cases.h"
int main() {
    using namespace acz;
    metadata_fixture::srvSlotCandidateCases();
    TestPaths paths(fs::current_path()/L"retirement-snapshot-fixture");
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    upload_fixture::cases(paths);
    auto& s=state();s.nativeBindings.diagnostic_tracking(true);
    auto* diag=const_cast<DescriptorTableDiagnostics*>(s.nativeBindings.diagnostic());
    s.nativeBindings.heap({139,1000,10000,8,32,0,1},1);diag->history_complete=true;
    const auto serial=diag->heaps.at(139).serial;
    auto table=hook_fixture::table();table[1]=reinterpret_cast<void*>(&metadata_fixture::t0Add);
    table[2]=reinterpret_cast<void*>(&metadata_fixture::t0Release);
    FakeCOM first{table.data()},later{table.data()},permanent{table.data()};
    auto acquire=[&](FakeCOM& o,unsigned slot) {
        auto* r=reinterpret_cast<ID3D12Resource*>(&o);
        SrvCandidates::Key key{reinterpret_cast<uintptr_t>(&o),serial,slot+1,slot};
        auto reservation=s.srvCandidates.reserve(key);r->AddRef();
        CHECK(!s.srvCandidates.finish(reservation,r));return key;
    };
    acquire(first,0);const auto laterKey=acquire(later,1);
    // Existing permanent ownership must also become ineligible immediately.
    using D=DescriptorTableDiagnostics;
    auto& slot=diag->slots[{serial,2}];
    slot.type=D::srv;slot.origin=D::create_srv;slot.desc=1;slot.ordinal=3;
    slot.view_write.observed=true;slot.view_write.ordinal=3;
    slot.view_write.dimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    slot.view_write.address=reinterpret_cast<uintptr_t>(&permanent);
    auto& pin=s.ownershipPins[0];pin.reserved=pin.complete=true;
    pin.texture=reinterpret_cast<ID3D12Resource*>(&permanent);
    pin.textureKey={slot.view_write.address,serial,3,2};pin.texture->AddRef();
    auto permanentReason=[&] {
        auto snapshot=s.nativeBindings.diagnostic_snapshot({139,serial,2,0},0);
        snapshot.timing=D::before_bind;
        return t0Ownership_locked(snapshot);
    };
    CHECK(permanentReason()==D::pin_owned);
    unsigned reentered=0;
    metadata_fixture::t0ReleaseReentry=[&] {
        ++reentered;CHECK(!mutex_depth);CHECK(!diag->heaps.at(139).live);
        CHECK(permanentReason()==D::pin_retired&&permanent.refs==2);
        CHECK(!s.srvCandidates.promote(laterKey));
        CHECK(s.nativeBindings.diagnostic_snapshot({139,serial,0,0},0).ownership_reason()==DescriptorTableDiagnostics::pin_retired);
    };
    const auto releases=metadata_fixture::t0ReleaseCalls;
    retireSrvCandidates(serial,139);
    CHECK(reentered==1&&first.refs==1&&later.refs==1);
    CHECK(metadata_fixture::t0ReleaseCalls==releases+2);
    const auto releasesAfter=metadata_fixture::t0ReleaseCalls;
    retireSrvCandidates(serial,139);
    CHECK(metadata_fixture::t0ReleaseCalls==releasesAfter);
    pin.texture->Release();pin.texture=nullptr;CHECK(permanent.refs==1);
    diag->heap(139,1,1000,10000,8,32,0,1);
    retireSrvCandidates(serial,139);CHECK(diag->heaps.at(139).live);
    CpuUploadSnapshot::Request q;q.accepted=true;
    for(auto& x:q.ranges){x.resource=1;x.mapped=reinterpret_cast<const unsigned char*>(1);x.pinned=true;}
    const auto adds=metadata_fixture::t0AddCalls;
    finishCpuSnapshot(nullptr,q);
    CHECK(!s.cpuSnapshot.stable&&!s.cpuSnapshot.file_bytes);
    CHECK(std::string_view(s.cpuSnapshot.reason)=="upload-pin-not-created");
    CHECK(metadata_fixture::t0AddCalls==adds);
    for(const auto& x:q.ranges)CHECK(!x.pinned);
    for(const auto& raw:io.raw)CHECK(raw.empty());
    set_sink(nullptr);
    std::printf("%zu J-139 focused CPU/fake-COM checks passed\n",checks.load());
}
