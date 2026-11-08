// Synthetic CPU metadata/owned bytes only. No COM, GPU, DLL or XR operation.
#include "stereo_payload.h"
#include "capture_input_provider.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>

// Exercise every allocation boundary without adding production test controls.
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
using namespace acz::capture;
using namespace acz::metadata;
namespace provider=acz::capture_input;
using acz::binding::NativeBindingObserver;

struct Fixture {
    NativeBindingObserver observer;
    State state;
    Command command{true,5,3,3,-7,19};
    provider::Request request;
    std::shared_ptr<std::vector<uint8_t>> srt,vertices,indices;
    uint32_t stride;
    static constexpr uint64_t device=99,heap=4,cpu=0x1000,gpu=0x2000;
    Fixture(uint32_t stride_=416):stride(stride_) {
        srt=std::make_shared<std::vector<uint8_t>>(5*stride);
        vertices=std::make_shared<std::vector<uint8_t>>(80,0xab);
        indices=std::make_shared<std::vector<uint8_t>>(65,0xcd);
        for(size_t i=0;i<srt->size();++i) (*srt)[i]=uint8_t(i*17+3);
        // Explicit fixture layout: per-vertex 16 bytes, StartIndex=3,
        // BaseVertex=-7 fetches [0,3]. No game indexing rule is inferred.
        for(size_t i=0;i<5;++i) {(*indices)[9+2*(3+i)]=uint8_t(7+i%4);(*indices)[10+2*(3+i)]=0;}
        for(size_t e=0;e<5;++e) {
            float local[16]{};local[0]=local[5]=local[10]=local[15]=1;
            local[12]=float(e);
            std::memcpy(srt->data()+e*stride+256,local,sizeof(local));
        }
        observer.heap({heap,cpu,gpu,8,32,0,1,0},device);
        observer.resource({1,0x10000,srt->size()},device);
        observer.resource({2,0x20000,vertices->size()},device);
        observer.resource({3,0x30000,indices->size()},device);
        write();
        RootLayout layout;
        RootParameter table;table.kind=ParameterKind::table;table.visibility=Visibility::geometry;
        table.ranges.push_back({BindingKind::srv,1,10,0,0,2});
        RootParameter vertex;vertex.kind=ParameterKind::srv;vertex.shader_register=20;vertex.visibility=Visibility::vertex;
        RootParameter index=vertex;index.shader_register=21;index.visibility=Visibility::pixel;
        layout.parameters={table,vertex,index};
        state.tracking_complete=true;
        // These complete manifests are supplied fixture facts, never generated
        // by the adapter from an identifier, resource flags or owned storage.
        state.pipeline={true,true,{{BindingKind::srv,10,0,ShaderStage::geometry},
            {BindingKind::srv,20,0,ShaderStage::vertex},{BindingKind::srv,21,0,ShaderStage::pixel}},{1}};
        state.pipeline.identity=new_lifetime_identity();
        state.pipeline.explicit_root_identity=new_lifetime_identity();
        state.set_root(9,RootBindingMetadata::from_adapter(layout),state.pipeline.explicit_root_identity);
        state.set_heaps({heap});state.set_value(0,ParameterKind::table,gpu);
        state.set_value(1,ParameterKind::srv,0x20000);state.set_value(2,ParameterKind::srv,0x30000);
        state.vertices[1]={2,7,64,16};state.index=BufferView{3,9,32,2};
        refresh();
    }
    void write(uint64_t first=1,uint32_t elements=3) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement=first;d.Buffer.NumElements=elements;d.Buffer.StructureByteStride=stride;
        observer.srv(1,&d,cpu+64,device);
    }
    void refresh() {
        request={};request.list_identity=state.list_identity;request.generation=state.generation;
        request.revision=state.revision;request.device=device;
        request.serialized=request.synchronized=true;
        auto a=observer.table(state,0,heap,2,device);
        auto b=observer.root(state,1,0,20,4,device);
        auto c=observer.root(state,2,0,16,4,device);
        CHECK(a && b && c);
        request.buffers={{observer.resource_token(1),a,srt},{observer.resource_token(2),b,vertices},
            {observer.resource_token(3),c,indices}};
        request.descriptors={{0,2,a},{1,0,b},{2,0,c}};
        request.indexing_proven=request.vertex_fetch_proven=true;
        request.srt_indices={0,2,0};request.vertex_access={{1,0,64}};
    }
    provider::Result assemble() {return provider::assemble(observer,state,request);}
    acz::capture::Result freeze(const provider::Lease& lease) {
        return provider::freeze_owned(observer,state,command,lease);
    }
};

void lifetimeAndStereo() {
    for(auto stride:{416u,512u,2048u}) {
        Fixture f(stride);auto lease=f.assemble();CHECK(lease);
        const auto& in=lease.lease->inputs();
        CHECK(in.resources.size()==3 && in.tables.size()==1 && in.root_descriptors.size()==2);
        CHECK(in.tables[0].descriptors.size()==3 && !in.tables[0].descriptors[0].buffer);
        CHECK(in.tables[0].descriptors[2].first_element==1 && in.tables[0].descriptors[2].view.offset==0);
        for(const auto& r:in.resources) {
            CHECK(r.list_identity==f.state.list_identity && r.generation==f.state.generation && r.revision==f.state.revision);
        }
        std::weak_ptr<const std::vector<uint8_t>> weak=f.srt;
        auto expected=*f.srt;
        CHECK(in.resources[0].bytes==f.srt->data()); // Exact storage, not a partial reconstruction.
        f.request={};f.srt.reset();f.vertices.reset();f.indices.reset();
        CHECK(!weak.expired());
        auto result=f.freeze(*lease.lease);CHECK(result);
        auto snapshot=result.snapshot;
        CHECK(snapshot->cpu_data_complete && !snapshot->gpu_replay_complete);
        CHECK(snapshot->resources.at(1)==expected && snapshot->resources.size()==3);
        CHECK(snapshot->command.start==3 && snapshot->command.base_vertex==-7 && snapshot->command.start_instance==19);
        CHECK(snapshot->state.vertices.at(1).offset==7 && snapshot->state.index->offset==9);
        CHECK(snapshot->srt_elements.size()==3 && snapshot->srt_elements[0]==snapshot->srt_elements[2]);
        CHECK(snapshot->srt_elements[0]==std::vector<uint8_t>(expected.begin()+stride,expected.begin()+2*stride));
        CHECK(snapshot->srt_elements[1]==std::vector<uint8_t>(expected.begin()+3*stride,expected.begin()+4*stride));
        // Explicit synthetic K/eyeVP. One immutable source for both eyes and all instances.
        std::array<acz::Mat4,2> eyes{acz::identity(),acz::identity()};eyes[0][12]=-0.03;eyes[1][12]=0.03;
        const auto k=acz::identity();auto batch=acz::stereo_payload::prepare(snapshot,eyes,k);CHECK(batch);
        CHECK(batch.batch->source==snapshot && batch.batch->eyes[0].size()==3 && batch.batch->eyes[1].size()==3);
        CHECK(batch.batch->eyes[0][0]!=batch.batch->eyes[1][0]);
        for(size_t eye=0;eye<2;++eye) for(size_t instance=0;instance<3;++instance) {
            CHECK(batch.batch->eyes[eye][0]==batch.batch->eyes[eye][2]);
            for(size_t byte=128;byte<stride;++byte)
                CHECK(batch.batch->eyes[eye][instance][byte]==snapshot->srt_elements[instance][byte]);
        }
        f.observer.remove_resource(f.observer.resource_token(1));
        f.observer.remove_heap(f.observer.heap_token(Fixture::heap));f.state.reset();
        lease={};CHECK(weak.expired());
        auto again=acz::stereo_payload::prepare(snapshot,eyes,k);CHECK(again);
        CHECK(again.batch->eyes==batch.batch->eyes && snapshot->resources.at(1)==expected);
    }
    Fixture f;auto lease=f.assemble();CHECK(lease);auto result=f.freeze(*lease.lease);CHECK(result);
    auto before=result.snapshot->resources.at(1);
    std::array<acz::Mat4,2> eyes{acz::identity(),acz::identity()};eyes[1][12]=0.04;
    auto batch=acz::stereo_payload::prepare(result.snapshot,eyes,acz::identity());CHECK(batch);
    std::fill(f.srt->begin(),f.srt->end(),0); // Serialized mutation AFTER freeze only.
    CHECK(result.snapshot->resources.at(1)==before);
    CHECK(result.snapshot->srt_elements[0]==result.snapshot->srt_elements[2]);
    auto again=acz::stereo_payload::prepare(result.snapshot,eyes,acz::identity());CHECK(again);
    CHECK(again.batch->eyes==batch.batch->eyes);
}

template<class F> void rejectAssembly(F mutate) {
    Fixture f;mutate(f);auto r=f.assemble();CHECK(!r && !r.lease && !r.rejection.empty());
}
template<class F> void rejectFreeze(F mutate) {
    Fixture f;auto lease=f.assemble();CHECK(lease);mutate(f);
    auto r=f.freeze(*lease.lease);CHECK(!r && !r.snapshot && !r.rejection.empty());
}
void rejections() {
    rejectAssembly([](auto& f){f.request.serialized=false;});
    rejectAssembly([](auto& f){f.request.synchronized=false;});
    rejectAssembly([](auto& f){f.request.list_identity=0;});
    rejectAssembly([](auto& f){f.request.list_identity=new_lifetime_identity();});
    rejectAssembly([](auto& f){++f.request.generation;});
    rejectAssembly([](auto& f){++f.request.revision;});
    rejectAssembly([](auto& f){++f.request.device;});
    rejectAssembly([](auto& f){++f.request.buffers[0].resource.generation;});
    rejectAssembly([](auto& f){f.request.buffers[0].bytes.reset();});
    rejectAssembly([](auto& f){f.request.buffers[0].bytes=std::make_shared<const std::vector<uint8_t>>(384);});
    rejectAssembly([](auto& f){f.request.buffers.push_back(f.request.buffers[0]);});
    rejectAssembly([](auto& f){f.request.buffers.resize(65);});
    rejectAssembly([](auto& f){f.request.descriptors.resize(4097);});
    rejectAssembly([](auto& f){f.request.srt_indices.resize(4097);});
    rejectAssembly([](auto& f){f.request.vertex_access.resize(33);});
    rejectAssembly([](auto& f){f.request.descriptors[0].table_offset=64;});
    rejectAssembly([](auto& f){f.request.descriptors[0].parameter=64;});
    rejectAssembly([](auto& f){f.request.descriptors[0].table_offset=1;});
    rejectAssembly([](auto& f){auto m=std::make_shared<acz::binding::Snapshot>(*f.request.descriptors[0].metadata.snapshot);
        m->descriptor.first_element=UINT64_MAX;f.request.descriptors[0].metadata.snapshot=m;});
    rejectAssembly([](auto& f){auto m=std::make_shared<acz::binding::Snapshot>(*f.request.descriptors[1].metadata.snapshot);
        m->descriptor.view.offset=UINT64_MAX;f.request.descriptors[1].metadata.snapshot=m;});
    rejectAssembly([](auto& f){f.request.descriptors.push_back(f.request.descriptors[0]);});
    rejectAssembly([](auto& f){f.request.descriptors.push_back(f.request.descriptors[1]);});
    rejectAssembly([](auto& f){f.request.buffers.erase(f.request.buffers.begin());});
    rejectAssembly([](auto& f){f.observer.disable();});
    rejectAssembly([](auto& f){f.write(UINT64_MAX,3);});
    rejectAssembly([](auto& f){f.write(1,UINT32_MAX);});
    rejectAssembly([](auto& f){f.observer.unsupported_write(Fixture::cpu+64,Fixture::device);});
    rejectAssembly([](auto& f){f.state.tracking_complete=false;});
    rejectFreeze([](auto& f){++f.state.revision;});
    rejectFreeze([](auto& f){++f.state.generation;});
    rejectFreeze([](auto& f){f.state.list_identity=new_lifetime_identity();});
    rejectFreeze([](auto& f){f.state.reset();});
    rejectFreeze([](auto& f){f.write();}); // Even identical bytes have a new slot revision.
    rejectFreeze([](auto& f){f.observer.remove_resource(f.observer.resource_token(1));});
    rejectFreeze([](auto& f){auto old=f.observer.resource_token(1);f.observer.remove_resource(old);
        f.observer.resource({1,0x10000,f.srt->size()},Fixture::device);f.write();
        CHECK(f.observer.resource_token(1).id==old.id && f.observer.resource_token(1)!=old);});
    rejectFreeze([](auto& f){auto old=f.observer.heap_token(Fixture::heap);f.observer.remove_heap(old);
        f.observer.heap({Fixture::heap,Fixture::cpu,Fixture::gpu,8,32,0,1,0},Fixture::device);f.write();
        CHECK(f.observer.heap_token(Fixture::heap).id==old.id && f.observer.heap_token(Fixture::heap)!=old);});
    rejectFreeze([](auto& f){f.observer.remove_resource(f.observer.resource_token(2));});
    rejectFreeze([](auto& f){f.state.pipeline.requirements_proven=false;}); // Native observed default remains disabled.
    rejectFreeze([](auto& f){f.state.pipeline.resources.clear();});
    rejectFreeze([](auto& f){f.state.pipeline.resources.push_back({BindingKind::srv,22,0,ShaderStage::pixel});});
    rejectFreeze([](auto& f){f.state.vertices[1].offset=UINT64_MAX;});
    rejectFreeze([](auto& f){f.command.count=UINT32_MAX;});
    rejectFreeze([](auto& f){f.command.instances=4097;});
    // Facts stay caller-provided; the provider must never elevate missing ones.
    for(int what=0;what<6;++what) {
        Fixture f;
        if(what==0) f.request.indexing_proven=false;
        if(what==1) f.request.vertex_fetch_proven=false;
        if(what==2) f.request.srt_indices[1]=3;
        if(what==3) f.request.vertex_access[0].offset=UINT64_MAX;
        if(what==4) f.request.vertex_access.clear();
        if(what==5) f.request.descriptors.erase(f.request.descriptors.begin());
        auto lease=f.assemble();CHECK(lease);auto r=f.freeze(*lease.lease);CHECK(!r && !r.snapshot);
    }
    Fixture unsupported;unsupported.observer.resource({8,0x80000,80,1,true},Fixture::device);
    CHECK(!unsupported.assemble()); // Unsupported aliasing disables observer.
}

void copiedDescriptors() {
    Fixture f;
    f.observer.heap({5,0x4000,0,4,32,0,0,0},Fixture::device);
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement=1;d.Buffer.NumElements=3;d.Buffer.StructureByteStride=f.stride;
    f.observer.srv(1,&d,0x4000,Fixture::device);
    D3D12_CPU_DESCRIPTOR_HANDLE dest{Fixture::cpu+64},source{0x4000};
    auto copy=[&]{f.observer.copy(1,&dest,nullptr,1,&source,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,Fixture::device);};
    copy();f.refresh();auto lease=f.assemble();CHECK(lease && f.freeze(*lease.lease));
    CHECK(f.request.descriptors[0].metadata.snapshot->source_heap==f.observer.heap_token(5));
    copy();CHECK(!f.freeze(*lease.lease)); // Copied destination revision changed.
    f.refresh();lease=f.assemble();CHECK(lease);
    f.observer.remove_heap(f.observer.heap_token(5));
    CHECK(f.freeze(*lease.lease)); // Source history is owned; destination remains current.
}

void rootOriginAndBudgets() {
    Fixture f(512);
    f.srt->resize(12+5*f.stride);
    f.observer.resource({1,0x10000,f.srt->size()},Fixture::device);
    RootLayout layout=f.state.root->layout();layout.parameters[0].kind=ParameterKind::srv;
    layout.parameters[0].shader_register=10;layout.parameters[0].ranges.clear();
    f.state.set_root(10,RootBindingMetadata::from_adapter(layout),f.state.pipeline.explicit_root_identity);
    f.state.set_value(0,ParameterKind::srv,0x10000+12);
    f.state.set_value(1,ParameterKind::srv,0x20000);f.state.set_value(2,ParameterKind::srv,0x30000);
    f.request.revision=f.state.revision;
    auto m=f.observer.root(f.state,0,0,5,f.stride,Fixture::device);CHECK(m);
    f.request.buffers[0]={f.observer.resource_token(1),m,f.srt};f.request.descriptors[0]={0,0,m};
    for(size_t i=1;i<3;++i) {
        auto w=f.observer.root(f.state,uint32_t(i),0,i==1?20:16,4,Fixture::device);
        f.request.buffers[i].witness=w;f.request.descriptors[i].metadata=w;
    }
    auto lease=f.assemble();CHECK(lease);auto result=f.freeze(*lease.lease);CHECK(result);
    CHECK(result.snapshot->bindings[0].descriptor.view.offset==12 && result.snapshot->bindings[0].descriptor.first_element==0);
    CHECK(result.snapshot->srt_elements[0]==std::vector<uint8_t>(f.srt->begin()+12,f.srt->begin()+12+f.stride));
    Fixture big;big.srt->resize(max_copy_bytes);
    big.observer.resource({1,0x10000,big.srt->size()},Fixture::device);
    // Separate resource VA ranges to keep this a budget fixture, not ambiguity.
    big.observer.resource({2,0x3000000,80},Fixture::device);
    big.observer.resource({3,0x4000000,65},Fixture::device);
    big.state.set_value(1,ParameterKind::srv,0x3000000);big.state.set_value(2,ParameterKind::srv,0x4000000);
    big.write();big.refresh();auto rejected=big.assemble();CHECK(!rejected && !rejected.rejection.empty());
    big.srt->resize(max_copy_bytes-80-65);
    big.observer.resource({1,0x10000,big.srt->size()},Fixture::device);big.write();big.refresh();
    auto exact=big.assemble();CHECK(exact); // Provider resource-byte equality is allowed.
    CHECK(!big.freeze(*exact.lease)); // Existing freeze also budgets immutable element copies.
}

void allocationFailures() {
    Fixture f;unsigned failures=0;bool completed=false;
    for(int budget=0;budget<128;++budget) {
        allocation_budget=budget;auto r=f.assemble();allocation_budget=-1;
        if(!r) {CHECK(!r.lease);++failures;}
        else {CHECK(f.freeze(*r.lease));completed=true;break;}
    }
    CHECK(completed && failures>=8);
    auto lease=f.assemble();CHECK(lease);failures=0;completed=false;
    for(int budget=0;budget<128;++budget) {
        allocation_budget=budget;auto r=f.freeze(*lease.lease);allocation_budget=-1;
        if(!r) {CHECK(!r.snapshot);++failures;}
        else {CHECK(r.snapshot->cpu_data_complete && !r.snapshot->gpu_replay_complete);completed=true;break;}
    }
    CHECK(completed && failures>=8);
    CHECK(f.freeze(*lease.lease)); // Failures publish nothing and don't poison metadata.
}
int main() {
    lifetimeAndStereo();rejections();copiedDescriptors();rootOriginAndBudgets();allocationFailures();
    std::printf("%u owned CPU capture provider checks passed (synthetic only)\n",checks);
}
