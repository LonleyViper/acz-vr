// Production ownership drain and hook refusal with counted fake COM only.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <thread>
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
using namespace acz;
static unsigned calls=0,adds=0,releases=0;
static bool inspectDrain=false;
struct Fake {void** table;ULONG refs=1;};
static ULONG STDMETHODCALLTYPE add(IUnknown* p){++calls;++adds;return ++reinterpret_cast<Fake*>(p)->refs;}
static ULONG STDMETHODCALLTYPE drop(IUnknown* p){
    ++calls;++releases;
    auto* f=reinterpret_cast<Fake*>(p);CHECK(f->refs>1);
    if(inspectDrain){
        CHECK(diagnostics::mutex_depth==0);
        // CPU admission is closed, but this Release is still executing. A
        // second shutdown must not report that cleanup has completed.
        CHECK(!shutdownDiagnosticOwnership(false));
        auto& s=state();std::lock_guard lock(s.mutex);
        CHECK(s.states.empty()&&s.pipelines.empty()&&s.srvCandidates.held==0);
        for(auto& pin:s.ownershipPins)CHECK(!pin.reserved&&!pin.root&&!pin.texture&&!pin.buffer);
        for(const auto& upload:s.uploadPins.entries)CHECK(!upload.held);
    }
    // Same-thread AND competing-thread hook entry while Release is in progress.
    const auto before=calls;
    hookedDrawInstanced(nullptr,0,0,0,0);
    std::thread other([]{writeBindingSrv(nullptr,nullptr,nullptr,{});});other.join();
    CHECK(calls==before);
    return --f->refs;
}
struct Paths final:diagnostics::PathProvider {diagnostics::Paths paths() override{auto base=std::filesystem::current_path();return {base/L"logs",base/L"game",base/L"config"};}};
struct IO final:diagnostics::IOProvider {
    bool create_directory(const std::filesystem::path&) override{return true;}
    bool size(const std::filesystem::path&,uint64_t& n) override{n=0;return true;}
    size_t append(const std::filesystem::path&,const char*,size_t n) override{return n;}
    bool reset(const std::filesystem::path&) override{return true;}
    bool rotate(const std::filesystem::path&,size_t) override{return true;}
    diagnostics::ReadResult read_config(const std::filesystem::path&,char* out,size_t cap) override {
        constexpr char text[]="log_enabled=true\ndiagnostic_ownership_pins=true\n";
        CHECK(cap>=sizeof text);std::memcpy(out,text,sizeof text-1);return {diagnostics::ReadState::ok,sizeof text-1};
    }
};
static void switchOff(){
    CHECK(!diagnostics::sink());CHECK(!hookCommandList(nullptr));
    CHECK(shutdownDiagnosticOwnership(false));CHECK(!ownershipShutdown.stopped());
    CHECK(calls==0);CHECK(!ownershipWasEnabled.load());
}
static void populate(std::array<Fake,8>& f){
    auto& s=state();auto& pin=s.ownershipPins[0];pin.reserved=true;
    pin.interfaces[0]=reinterpret_cast<IUnknown*>(&f[0]);pin.counts[0]=add(pin.interfaces[0]);
    // An opaque failed QI output and unacquired validated address are NOT owned.
    pin.interfaces[1]=reinterpret_cast<IUnknown*>(&f[7]);
    pin.rootCanonical=reinterpret_cast<IUnknown*>(&f[7]);
    auto& split=s.ownershipPins[1];split.reserved=true;
    split.rootCanonical=reinterpret_cast<IUnknown*>(&f[1]);add(split.rootCanonical);split.rootCanonicalOwned=true;
    pin.root=retain(reinterpret_cast<ID3D12RootSignature*>(&f[2]));
    // Multiple shared copies still represent just one acquired reference.
    s.pipelines[reinterpret_cast<ID3D12PipelineState*>(&f[0])].root=pin.root;
    s.states[reinterpret_cast<ID3D12GraphicsCommandList*>(&f[0])].root=pin.root;
    auto& texture=s.ownershipPins[2];texture.reserved=true;
    texture.texture=reinterpret_cast<ID3D12Resource*>(&f[3]);add(texture.texture);
    auto& buffer=s.ownershipPins[3];buffer.reserved=true;
    buffer.buffer=reinterpret_cast<ID3D12Resource*>(&f[4]);buffer.counts[0]=add(buffer.buffer);
    // Partial buffer acquisition has no owned reference.
    s.ownershipPins[4].reserved=true;s.ownershipPins[4].buffer=reinterpret_cast<ID3D12Resource*>(&f[7]);
    const auto r=s.srvCandidates.reserve({reinterpret_cast<uintptr_t>(&f[5]),1,1,0});
    add(reinterpret_cast<IUnknown*>(&f[5]));CHECK(!s.srvCandidates.finish(r,reinterpret_cast<ID3D12Resource*>(&f[5])));
    // A second acquisition of the same object must receive a second Release.
    const auto r2=s.srvCandidates.reserve({reinterpret_cast<uintptr_t>(&f[5]),1,2,1});
    add(reinterpret_cast<IUnknown*>(&f[5]));CHECK(!s.srvCandidates.finish(r2,reinterpret_cast<ID3D12Resource*>(&f[5])));
    const auto upload=s.uploadPins.reserve(reinterpret_cast<uintptr_t>(&f[6]),1);
    add(reinterpret_cast<IUnknown*>(&f[6]));s.uploadPins.finish(upload,reinterpret_cast<ID3D12Resource*>(&f[6]));
    // Fault consumes another slot but gives teardown no reference to Release.
    const auto fault=s.uploadPins.reserve(reinterpret_cast<uintptr_t>(&f[7]),2);s.uploadPins.finish(fault,nullptr);
    // Hooked table cleanup must call the saved native original, not the refusing hook.
    auto& table=s.objects[f[0].table];table.originals[2]=reinterpret_cast<void*>(&drop);
    f[0].table[2]=reinterpret_cast<void*>(&releaseMetadata);
}
static void busy(){
    std::atomic<bool> entered{false},finish{false};
    std::thread hook([&]{HookAdmission admission;CHECK(admission);entered=true;while(!finish.load())std::this_thread::yield();});
    while(!entered.load())std::this_thread::yield();
    auto before=calls;CHECK(!shutdownDiagnosticOwnership(false));
    CHECK(!ownershipShutdown.stopped());CHECK(calls==before);
    finish=true;hook.join();
}
static void run(){
    switchOff();
    Paths paths;IO io;auto log=std::make_unique<diagnostics::Log>(paths,io);log->flush();
    CHECK(log->ownership_pins());diagnostics::set_sink(log.get());
    CHECK(!hookCommandList(nullptr));CHECK(ownershipWasEnabled.load());
    // Separate per-object tables, so one hooked table cannot disguise others.
    std::array<std::array<void*,3>,8> tables{};std::array<Fake,8> f{};
    for(size_t i=0;i<f.size();++i){tables[i][1]=reinterpret_cast<void*>(&add);tables[i][2]=reinterpret_cast<void*>(&drop);f[i].table=tables[i].data();}
    populate(f);CHECK(adds==8);auto before=calls;
    CHECK(shutdownDiagnosticOwnership(true));CHECK(calls==before&&!ownershipShutdown.stopped());
    busy();inspectDrain=true;
    CHECK(shutdownDiagnosticOwnership(false));CHECK(ownershipShutdown.stopped());
    CHECK(adds==releases);for(auto& object:f)CHECK(object.refs==1);
    before=calls;CHECK(shutdownDiagnosticOwnership(false));CHECK(calls==before);
    hookedDrawIndexedInstanced(nullptr,0,0,0,0,0);writeBindingSrv(nullptr,nullptr,nullptr,{});
    CHECK(FAILED(createGraphicsPSO(nullptr,nullptr,__uuidof(IUnknown),nullptr)));
    CHECK(!hookCaptureDevice(nullptr));CHECK(!hookCaptureCommandList(nullptr));
    CHECK(calls==before);CHECK(state().srvCandidates.occupancy()==0);
    diagnostics::set_sink(nullptr);
}
int main(){run();std::puts("ownership shutdown: counted acquisitions/releases, busy, termination, reentry, OFF and double shutdown passed");}
