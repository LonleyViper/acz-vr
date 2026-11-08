// Fully synthetic metadata and source reports; no runtime or GPU operations.
#include "capture_enablement.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static int allocation_budget=-1;
void* operator new(std::size_t n) {
    if(allocation_budget==0) throw std::bad_alloc();
    if(allocation_budget>0) --allocation_budget;
    if(auto p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}} while(0)
namespace enable=acz::capture_enablement;
namespace copy=acz::native_copy;
namespace input=acz::capture_input;
using namespace acz::capture;
using namespace acz::metadata;

struct Fake final:copy::Source {
    int fail=0;
    copy::Stamp inspect(const copy::Request&) override {
        if(fail==1) throw 17;
        if(fail==2) throw std::bad_alloc();
        return {{7,12},1};
    }
    size_t copy(const copy::Request&,uint8_t* out,size_t n) override {
        if(fail==3) throw 42;
        std::fill(out,out+n,0);
        return n;
    }
};
struct Fixture {
    acz::binding::NativeBindingObserver observer;
    State state;
    Fake source;
    copy::Request request;
    copy::Result owned;
    input::Request caller;
    enable::Inputs facts;
    static constexpr uint64_t device=99,heap=4,cpu=0x1000,gpu=0x2000;
    Fixture() {
        observer.heap({heap,cpu,gpu,8,32,0,1,0},device);
        observer.resource({1,0x10000,832},device);write();
        RootLayout layout;
        RootParameter table;table.kind=ParameterKind::table;table.visibility=Visibility::all;
        table.ranges.push_back({BindingKind::srv,2,10,0,0,2});layout.parameters={table};
        state.tracking_complete=true;
        // This manifest is a caller assertion, never shader decoder output.
        state.pipeline={true,true,{{BindingKind::srv,10,0,ShaderStage::geometry}}, {}};
        state.pipeline.identity=new_lifetime_identity();
        state.pipeline.explicit_root_identity=new_lifetime_identity();
        state.set_root(9,RootBindingMetadata::from_adapter(layout),state.pipeline.explicit_root_identity);
        state.set_heaps({heap});state.set_value(0,ParameterKind::table,gpu);
        request={observer.resource_token(1),observer.table(state,0,heap,2,device),0,832,1,{7,10},device};
        owned=copy::acquire(observer,state,source,request);CHECK(owned);
        caller.list_identity=state.list_identity;caller.generation=state.generation;caller.revision=state.revision;
        caller.device=device;caller.serialized=caller.synchronized=true;
        caller.indexing_proven=caller.vertex_fetch_proven=true;caller.srt_indices={1};
        caller.buffers={owned.buffer};caller.descriptors={{0,2,request.witness}};
        facts.opt_in=true;facts.state=&state;facts.observer=&observer;facts.caller=&caller;
        facts.root_id=state.root_id;facts.root_identity=state.root_identity;
        facts.pso_identity=state.pipeline.identity;facts.pso_root_identity=state.pipeline.explicit_root_identity;
        facts.copies={{&request,&owned}};
        CHECK(enable::decide(facts).enabled);
    }
    void write(uint64_t address=cpu+64) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements=2;d.Buffer.StructureByteStride=416;
        observer.srv(1,&d,address,device);
    }
};
template<class F> void reject(const char* reason,F change) {
    Fixture f;change(f);
    const auto result=enable::decide(f.facts);
    CHECK(!result.enabled);CHECK(result.reason && std::strcmp(result.reason,reason)==0);
}
void missingFacts() {
    auto d=enable::decide({});CHECK(!d.enabled);CHECK(d.reason);
    reject("caller opt-in absent",[](auto& f){f.facts.opt_in=false;});
    reject("missing inputs",[](auto& f){f.facts.state=nullptr;});
    reject("missing inputs",[](auto& f){f.facts.observer=nullptr;});
    reject("missing inputs",[](auto& f){f.facts.caller=nullptr;});
    reject("tracking incomplete",[](auto& f){f.state.tracking_complete=false;});
    reject("tracking incomplete",[](auto& f){f.observer.disable();});
    reject("cockpit pipeline unidentified",[](auto& f){f.state.pipeline.drawcpt=false;});
    reject("all-stage manifest assertion absent",[](auto& f){f.state.pipeline.requirements_proven=false;});
    reject("indexing assertion absent",[](auto& f){f.caller.indexing_proven=false;});
    reject("vertex-fetch assertion absent",[](auto& f){f.caller.vertex_fetch_proven=false;});
    reject("explicit shader indices absent or oversized",[](auto& f){f.caller.srt_indices.clear();});
    reject("vertex-fetch manifest mismatch",[](auto& f){f.state.pipeline.vertex_slots={0};});
    reject("serialization/synchronization assertion absent",[](auto& f){f.caller.serialized=false;});
    reject("serialization/synchronization assertion absent",[](auto& f){f.caller.synchronized=false;});
    reject("device identity absent",[](auto& f){f.caller.device=0;});
    reject("owned copy results absent or mismatched",[](auto& f){f.facts.copies.clear();});
    reject("owned copy results absent or mismatched",[](auto& f){f.caller.buffers.clear();});
    reject("copy provider failed",[](auto& f){f.facts.copies[0].request=nullptr;});
    reject("copy provider failed",[](auto& f){f.facts.copies[0].result=nullptr;});
    reject("copy provider failed",[](auto& f){f.owned.buffer.bytes.reset();});
    reject("copy provider failed",[](auto& f){f.owned.rejection="copy failed";});
    reject("manifest requirements absent or oversized",[](auto& f){f.state.pipeline.resources.clear();});
    reject("manifest descriptor absent",[](auto& f){f.caller.descriptors.clear();});
    reject("manifest descriptor absent",[](auto& f){f.state.pipeline.resources.push_back({BindingKind::srv,11,0,ShaderStage::pixel});});
    reject("manifest root binding unresolved",[](auto& f){f.state.pipeline.resources.push_back({BindingKind::srv,12,0,ShaderStage::vertex});});
    reject("SrtData manifest requirement absent",[](auto& f){f.state.pipeline.resources[0].stage=ShaderStage::pixel;});
    reject("incomplete owned copy range",[](auto& f){f.request.elements--;});
    reject("incomplete owned copy range",[](auto& f){f.request.offset=1;});
    reject("incomplete owned copy range",[](auto& f){f.request.stride=0;});
    reject("incomplete owned copy range",[](auto& f){f.request.elements=UINT64_MAX;f.request.stride=2;});
}
void identities() {
    for(int field=0;field<3;++field) {
        reject("stale list identity/generation/revision",[&](auto& f){
            if(field==0) ++f.caller.list_identity;
            if(field==1) ++f.caller.generation;
            if(field==2) ++f.caller.revision;
        });
        reject("stale list identity/generation/revision",[&](auto& f){
            if(field==0) ++f.state.list_identity;
            if(field==1) ++f.state.generation;
            if(field==2) ++f.state.revision;
        });
        reject("stale copy resource/descriptor provenance",[&](auto& f){
            if(field==0) ++f.owned.buffer.witness.list_identity;
            if(field==1) ++f.owned.buffer.witness.generation;
            if(field==2) ++f.owned.buffer.witness.revision;
        });
    }
    for(int field=0;field<4;++field) for(int absent=0;absent<2;++absent)
        reject("stale or incompatible root/PSO identity",[&](auto& f){
            uint64_t* ids[]={&f.facts.root_id,&f.facts.root_identity,&f.facts.pso_identity,&f.facts.pso_root_identity};
            if(absent) *ids[field]=0;else ++*ids[field];
        });
    reject("stale or incompatible root/PSO identity",[](auto& f){++f.state.root_identity;});
    reject("stale or incompatible root/PSO identity",[](auto& f){++f.state.pipeline.identity;});
    reject("stale or incompatible root/PSO identity",[](auto& f){++f.state.pipeline.explicit_root_identity;});
    reject("stale or incompatible root/PSO identity",[](auto& f){f.state.root.reset();});
    reject("copy provenance mismatch",[](auto& f){++f.request.device;});
    reject("copy provenance mismatch",[](auto& f){++f.caller.device;});
    reject("copy provenance mismatch",[](auto& f){++f.request.resource.generation;});
    reject("copy provenance mismatch",[](auto& f){++f.caller.buffers[0].resource.id;});
    reject("copy provenance mismatch",[](auto& f){f.caller.buffers[0].bytes=std::make_shared<const std::vector<uint8_t>>(832);});
    reject("stale copy resource/descriptor provenance",[](auto& f){++f.owned.buffer.witness.device;});
    reject("stale copy resource/descriptor provenance",[](auto& f){++f.request.witness.revision;});
    reject("stale copy resource/descriptor provenance",[](auto& f){++f.caller.buffers[0].witness.generation;});
    reject("stale copy resource/descriptor provenance",[](auto& f){f.observer.remove_resource(f.request.resource);});
    reject("stale copy resource/descriptor provenance",[](auto& f){f.observer.resource({1,0x10000,832},Fixture::device);f.write();});
    reject("stale copy resource/descriptor provenance",[](auto& f){f.observer.remove_heap(f.observer.heap_token(Fixture::heap));});
    reject("stale copy resource/descriptor provenance",[](auto& f){f.observer.heap({Fixture::heap,Fixture::cpu,Fixture::gpu,8,32,0,1,0},Fixture::device);f.write();});
    reject("stale copy resource/descriptor provenance",[](auto& f){f.write();});
    reject("caller descriptor/resource provenance rejected",[](auto& f){++f.caller.descriptors[0].metadata.revision;});
    reject("caller descriptor/resource provenance rejected",[](auto& f){++f.caller.descriptors[0].table_offset;});
    reject("caller descriptor/resource provenance rejected",[](auto& f){++f.caller.descriptors[0].parameter;});
    reject("caller descriptor/resource provenance rejected",[](auto& f){
        f.write(Fixture::cpu+96);f.caller.descriptors[0].metadata=f.observer.table(f.state,0,Fixture::heap,3,Fixture::device);
    });
}
void reportsAndFailures() {
    for(int field=0;field<8;++field)
        reject("source fence/version assertion absent or inconsistent",[&](auto& f){
            switch(field) {
            case 0:f.request.required.identity=0;break;
            case 1:f.request.required.value=0;break;
            case 2:f.owned.before.version=0;break;
            case 3:++f.owned.after.version;break;
            case 4:++f.owned.before.completed.identity;break;
            case 5:++f.owned.after.completed.identity;break;
            case 6:f.owned.before.completed.value=9;break;
            case 7:f.owned.after.completed.value=11;break;
            }
        });
    for(int fail=1;fail<=3;++fail) {
        Fixture f;f.source.fail=fail;
        f.owned=copy::acquire(f.observer,f.state,f.source,f.request);
        CHECK(!f.owned && !f.owned.buffer.bytes && !f.owned.buffer.witness.snapshot);
        const auto d=enable::decide(f.facts);CHECK(!d.enabled);CHECK(std::strcmp(d.reason,"copy provider failed")==0);
    }
    Fixture f;
    allocation_budget=0;
    auto noCopy=copy::acquire(f.observer,f.state,f.source,f.request);
    allocation_budget=-1;
    CHECK(!noCopy && !noCopy.buffer.bytes && !noCopy.buffer.witness.snapshot);
    // Enumerate all allocation failure positions on the decision's valid path.
    // assemble may consume its exception; either way no decision is published.
    bool reachedSuccess=false;
    for(int budget=0;budget<64;++budget) {
        const auto owners=f.owned.buffer.bytes.use_count();
        allocation_budget=budget;
        const auto d=enable::decide(f.facts);
        allocation_budget=-1;
        CHECK(f.owned.buffer.bytes.use_count()==owners);CHECK(d.reason);
        if(d.enabled) {CHECK(budget>0);reachedSuccess=true;break;}
        CHECK(std::strcmp(d.reason,"caller descriptor/resource provenance rejected")==0 ||
              std::strcmp(d.reason,"allocation failure")==0);
    }
    CHECK(reachedSuccess);
}
void nativeDefaultAndManifest() {
    Fixture f;
    // No decoder manufactures the assertion: a native-style default Pipeline
    // with known identities/GS alone still has requirements_proven=false.
    Pipeline native;
    CHECK(!native.requirements_proven);
    f.state.pipeline.requirements_proven=native.requirements_proven;
    const auto before=f.state.revision;
    auto lease=input::assemble(f.observer,f.state,f.caller);CHECK(lease);
    auto frozen=input::freeze_owned(f.observer,f.state,{false,3,1},*lease.lease);
    CHECK(!frozen && !frozen.snapshot);
    CHECK(frozen.rejection=="all-stage resource requirements unproven");
    CHECK(!enable::decide(f.facts).enabled);
    CHECK(!f.state.pipeline.requirements_proven && f.state.revision==before);
    // A second caller-asserted stage needs its own current descriptor.
    f.state.pipeline.requirements_proven=true;
    f.state.pipeline.resources.push_back({BindingKind::srv,11,0,ShaderStage::pixel});
    CHECK(!enable::decide(f.facts).enabled);
    f.write(Fixture::cpu+96);
    f.caller.descriptors.push_back({0,3,f.observer.table(f.state,0,Fixture::heap,3,Fixture::device)});
    CHECK(enable::decide(f.facts).enabled);
    f.observer.unsupported_write(Fixture::cpu+96,Fixture::device);
    CHECK(!enable::decide(f.facts).enabled);
}
int main() {
    missingFacts();identities();reportsAndFailures();nativeDefaultAndManifest();
    std::printf("%u synthetic offline capture-enablement checks passed; no runtime parity claimed\n",checks);
}
