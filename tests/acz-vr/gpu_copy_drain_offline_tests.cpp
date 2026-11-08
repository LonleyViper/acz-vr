// Production hooks + production drain, with fake vtables; no runtime/device.
#define main j144_unused_hook_fixture_main
#pragma warning(push)
#pragma warning(disable:4715 4716) // Included main loses its implicit return when renamed.
#include "d3d12_hook_offline_tests.cpp"
#pragma warning(pop)
#undef main
#include "gpu_copy_ledger_fixture.h"
using namespace acz;
using namespace acz::gpu_copy;
struct LedgerBackend final:Backend {
    unsigned maps=0,releases=0;
    FenceSample poll() noexcept override {return {};}
    bool read() noexcept override {++maps;return false;}
    void drop_logical_source_lease(uint64_t) noexcept override {++releases;}
    void release_private(uint64_t) noexcept override {++releases;}
};
int main() {
    auto& owner=diagnostic_session();LedgerBackend backend;const auto a=fixture_admission();
    CHECK(FixtureAccess::arm(owner,true,backend,a));CHECK(FixtureAccess::reserve(owner,a));
    CHECK(FixtureAccess::created(owner));CHECK(FixtureAccess::record(owner));
    auto table=listTable(reinterpret_cast<void*>(&drawA));Fake list{table.data()};
    auto* nativeList=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    g_protect=fakeProtect;g_pinResult=true;CHECK(hookCommandList(nativeList));
    std::array<void*,20> queueTable{};Fake queue{queueTable.data()};
    state().queues[queue.table].originals[VTIDX_ExecuteCommandLists]=reinterpret_cast<void*>(&executeA);
    // Permanent creation-time upload owner, representing a dependency of the
    // logical lease. A drain must not take or Release it in either veto case.
    Fake upload{table.data(),2};auto& pins=state().uploadPins;
    auto r=pins.reserve(reinterpret_cast<uintptr_t>(&upload),1);
    pins.finish(r,reinterpret_cast<ID3D12Resource*>(&upload));ownershipWasEnabled=true;
    CHECK(!shutdownDiagnosticOwnership(false)); // recorded/in-flight veto
    CHECK(!ownershipShutdown.stopped());CHECK(upload.refs==2);
    CHECK(pins.entries[0].held==reinterpret_cast<ID3D12Resource*>(&upload));
    FixtureAccess::uncertain(owner);
    CHECK(!shutdownDiagnosticOwnership(false)); // quarantine veto persists
    CHECK(!ownershipShutdown.stopped());CHECK(upload.refs==2);
    CHECK(pins.entries[0].held==reinterpret_cast<ID3D12Resource*>(&upload));
    // Stop and both failed drains leave saved native Draw/Execute forwarding.
    owner.stop();hookedDrawInstanced(nativeList,9,2,3,4);
    CHECK(drawsA==1&&values[0]==9&&values[1]==2&&values[2]==3&&values[3]==4);
    hookedExecuteCommandLists(reinterpret_cast<ID3D12CommandQueue*>(&queue),0,nullptr);
    CHECK(execA==1&&values[0]==0&&arraySeen==nullptr);
    CHECK(backend.maps==0&&backend.releases==0&&upload.refs==2);
    std::puts("GPU ledger veto: production drain blocked; native forwarding preserved (fake COM only)");
}
