#pragma once
#include <new>
// Included by the existing diagnostics target; fake CPU inventory and COM only.
namespace l3_fixture {
using O=acz::diagnostics::L3Observation;
static D3D12_RESOURCE_DESC description() {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Alignment=65536;d.Width=4096;
    d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;
}
static void inventory() {
    auto o=std::make_unique<O>();const auto d=description();D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_UPLOAD;
    o->create(1,&d,1,&p);o->map(1,true,true,0);CHECK(!o->creation&&!o->resources.get(1)); // default OFF
    o->enabled=true;char text[text_capacity]{};
    o->facts(text,sizeof text,"l3-t10",{},0);
    CHECK(std::strstr(text,"addr=0;va=0;bytes=0;obs=0;heap=unknown;kind=0"));
    CHECK(std::strstr(text,"held=unknown")&&std::strstr(text,"gap=1")&&std::strstr(text,"cov=partial"));
    for(unsigned heap=1;heap<=4;++heap) {
        p.Type=static_cast<D3D12_HEAP_TYPE>(heap);p.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;p.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;
        o->create(heap,&d,1,&p);auto* r=o->resources.get(heap);CHECK(r&&r->heap==heap&&r->page==3&&r->pool==1);
        CHECK(r->description&&r->desc.Dimension==d.Dimension&&r->desc.Alignment==d.Alignment&&r->desc.Width==d.Width);
        CHECK(r->desc.Height==1&&r->desc.DepthOrArraySize==1&&r->desc.MipLevels==1&&r->desc.Format==DXGI_FORMAT_UNKNOWN);
        CHECK(r->desc.SampleDesc.Count==1&&!r->desc.SampleDesc.Quality&&r->desc.Layout==d.Layout&&r->desc.Flags==d.Flags);
    }
    o->create(5,&d,2);o->create(6,&d,3);o->create(7,nullptr,0);
    CHECK(o->resources.get(5)->kind==2&&!o->resources.get(5)->heap&&o->resources.get(6)->kind==3);
    o->facts(text,sizeof text,"l3-vb",{5,0x1000,256},1);CHECK(std::strstr(text,"heap=unknown;kind=2"));
    o->view(7,d);CHECK(o->resources.get(7)->description&&o->resources.get(7)->kind==0);
    auto* r=o->resources.get(2);o->map(2,false,true,0);CHECK(!r->maps&&!r->outstanding);
    o->map(2,true,true,0);o->map(2,true,false,0);CHECK(r->maps==2&&r->outstanding==2&&r->map_pointers==1);
    o->unmap(2,0);CHECK(r->outstanding==1&&r->map_pointers==1);
    o->facts(text,sizeof text,"l3-ib",{2,0x2000,18},33);
    CHECK(std::strstr(text,"map=2;unmap=1;out=1;ptr=1;held=unknown;cpu=3;copy=0;wts=0"));
    o->unmap(2,0);CHECK(!r->outstanding&&!r->map_pointers&&!r->map_gap);
    o->unmap(2,0);CHECK(r->map_gap);o->map(2,true,true,1);CHECK(r->map_gap);
    o->cpu_write(2);CHECK(r->writes==1&&r->cpu==7);
    auto* l=o->list(100,1);CHECK(l&&!l->draws&&!l->closed);
    l->sources[0]={2,0x2000,416};l->sources[1]={2,0x2000,256};l->sources[33]={2,0x2000,18};
    o->draw(*l);o->draw(*l);CHECK(l->draws==2&&o->draws==2&&l->selected_count==1);
    o->begin_submission();const auto count=o->execute(*l);CHECK(count==2&&o->execute(*l)==0);o->end_submission(count);
    CHECK(l->executes==2&&o->submissions_with_draw==1&&o->sub_total==2&&o->sub_min==2&&o->sub_max==2);
    CHECK(o->list_total==2&&o->list_min==2&&o->list_max==2);
    for(const auto& role:o->repeats)CHECK(role.fresh==1&&!role.same&&!role.write);
    o->begin_submission();o->end_submission(o->execute(*l));
    for(const auto& role:o->repeats)CHECK(role.same==1); // outstanding maps do not invent CPU stores
    o->cpu_write(2);o->begin_submission();o->end_submission(o->execute(*l));
    for(const auto& role:o->repeats)CHECK(role.write==1);
    o->begin_submission();o->end_submission(0);CHECK(o->sub_min==0&&o->sub_max==2&&o->sub_total==6);
    o->counts(text,sizeof text);
    CHECK(std::strcmp(text,"l3-counts;unit=sub;present=unobserved;sub=4;recorded=2;subs=3;sub-total=6;sub-min=0;sub-max=2;list-total=2;list-min=2;list-max=2;new=1;repeat=1;rewrite=1;unknown=0;heaps=0,0,3,0,0;resource-loss=0;state-loss=0;patch-gap=0;list-loss=0;untracked=0;saturated=0")==0);
    o->repeat_text(text,sizeof text);
    CHECK(std::strcmp(text,"l3-repeats;unit=sub;sub=4;roles=t10,vb,ib;new=1,1,1;repeat=1,1,1;rewrite=1,1,1;unknown=0,0,0;held-pointer-writes=unknown")==0);
    o->copy(100,1,2);CHECK(r->copies==1);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(uintptr_t(2));b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;o->barrier(*l,100,1,b);
    auto* s=o->state(100,1,2);CHECK(s&&s->before==1&&s->after==64&&s->subresource==UINT32_MAX&&s->flags==1&&!s->earlier);
    o->state_text(text,sizeof text,*l,100,1,l->sources[0],0);
    CHECK(std::strstr(text,"state=observed")&&std::strstr(text,"prior=unknown")&&std::strstr(text,"bt=1;bu=0;ba=0"));
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=nullptr;o->barrier(*l,100,1,b);
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;b.Aliasing.pResourceBefore=nullptr;b.Aliasing.pResourceAfter=nullptr;o->barrier(*l,100,1,b);
    o->state_text(text,sizeof text,*l,100,1,l->sources[0],0);CHECK(std::strstr(text,"bt=1;bu=1;ba=1"));
    auto* other=o->list(101,1);o->state_text(text,sizeof text,*other,101,1,l->sources[0],0);
    CHECK(std::strstr(text,"state=unknown")&&std::strstr(text,"prior=observed"));
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(uintptr_t(2));
    b.Transition.Subresource=0;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_INDEX_BUFFER;
    b.Flags=D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;o->barrier(*other,101,1,b);
    auto* later=o->state(101,1,2);CHECK(later&&later->earlier==s->transition&&later->earlier_before==1&&later->earlier_after==64);
    CHECK(later->flags==2&&!later->write); // closing transition is not a write
    const auto generation=l->generation;l->resets=5;l=o->list(100,2);
    CHECK(l->generation!=generation&&!l->draws&&!l->executes&&!l->closes&&!l->selected_count&&l->resets==5);
    o->state_text(text,sizeof text,*l,100,2,{2,0,0},0);CHECK(std::strstr(text,"state=unknown"));
    // No-address/unmapped VA stays unknown and is counted by source class.
    l->sources[1]={0,0xbad0,100};o->draw(*l);o->begin_submission();o->end_submission(o->execute(*l));
    CHECK(o->repeats[0].unknown==1&&o->repeats[1].unknown==1&&o->repeats[2].unknown==1);
    auto tinyStore=std::make_unique<O::Store<O::Resource,8>>();for(uint64_t i=1;i<=8;++i)CHECK(tinyStore->get(i,true));
    CHECK(!tinyStore->get(9,true)&&tinyStore->lost==1);for(uint64_t i=1;i<=8;++i)CHECK(tinyStore->get(i));
    uint64_t saturated=UINT32_MAX;O::inc(saturated);CHECK(saturated==UINT32_MAX&&O::add(saturated,1)==UINT32_MAX);
    // Allocation trap covers draw, CPU/copy/barrier writes, lookups and all
    // formatters. Large registries live only on the heap, never on this stack.
    const auto failed=failedNew;failNew=true;
    o->draw(*l);o->map(2,true,false,0);o->unmap(2,0);o->cpu_write(2);o->copy(100,2,2);o->barrier(*l,100,2,b);
    o->facts(text,sizeof text,"l3-t10",{2,0x2000,416},0);o->state_text(text,sizeof text,*l,100,2,{2,0,0},0);o->counts(text,sizeof text);o->repeat_text(text,sizeof text);
    CHECK(failNew&&failedNew==failed);failNew=false;
    acz::binding::BufferBindingRegistry registry;auto a=registry.add_resource({70,0x10000,4096,1,false});CHECK(a);
    CHECK(registry.observed_address(0x10010,32)==70&&!registry.observed_address(0x10,1)&&!registry.observed_address(UINT64_MAX,2));
    auto overlap=registry.add_resource({71,0x10100,256,1,false});CHECK(overlap);
    CHECK(!registry.observed_address(0x10110,8));CHECK(registry.remove_resource(overlap.token));
    failNew=true;CHECK(registry.observed_address(0x10010,32)==70&&failNew);failNew=false;
    auto diag=std::make_unique<DescriptorTableDiagnostics>();
    diag->heap(1000,2000,0x10000,0x20000,4,32,0,1);
    diag->write(0x10000,2000,DescriptorTableDiagnostics::srv,1,0,2,1);
    l=o->list(102,1);l->t10_table=true;l->table_heap=&diag->heaps.at(1000);l->table_epoch=l->table_heap->serial;
    l->table_slot=&diag->slots.at({l->table_epoch,0});l->table_diagnostics=diag.get();
    o->draw(*l);CHECK(l->sources[0].address==2);
    diag->slots.at({l->table_epoch,0}).resource=3;o->draw(*l);CHECK(l->sources[0].address==3);
    diag->retire(1000);o->draw(*l);CHECK(!l->sources[0].address);
    diag->heap(1000,2000,0x10000,0x20000,4,32,0,1);o->draw(*l);CHECK(!l->sources[0].address);
    for(unsigned i=1;i<=128;++i){l->sources[1].address=0x90000+i;o->draw(*l);}
    CHECK(l->gap&&l->selected_count<=l->selected.size());
    // Same address with a new creation is ambiguous lifetime/facts, never a
    // claim that a write completed into the previous incarnation.
    auto* single=o->list(103,1);single->sources[0]={4,0,416};o->draw(*single,false);
    o->begin_submission();o->end_submission(o->execute(*single));
    const auto unresolved=o->repeats[0].unknown;o->create(4,&d,1,&p);
    o->begin_submission();o->end_submission(o->execute(*single));CHECK(o->repeats[0].unknown==unresolved+1);
}
static void writeRegressions() {
    auto o=std::make_unique<O>();o->enabled=true;const auto d=description();
    o->create(1,&d,1);auto* l=o->list(100,1);l->sources[33]={1,0x1000,18};
    char text[text_capacity]{};
    // CopyBufferRegion destination -> closing transition -> selected draw.
    o->copy(100,1,1);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_INDEX_BUFFER;
    o->barrier(*l,100,1,b);o->draw(*l);
    auto* s=o->state(100,1,1);CHECK(s&&s->write&&s->transition>s->write);
    CHECK(o->resources.get(1)->version==s->transition); // conservative rewrite tracking stays
    o->state_text(text,sizeof text,*l,100,1,l->sources[33],33);CHECK(std::strstr(text,"bt=1;bu=0;ba=0"));
    o->copy(100,1,1);o->draw(*l);
    o->state_text(text,sizeof text,*l,100,1,l->sources[33],33);CHECK(std::strstr(text,"bt=0;bu=0;ba=0"));
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_INDEX_BUFFER;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    o->barrier(*l,100,1,b);CHECK(s->write==s->transition); // entering a write state still counts
    // Unrecorded/texture destinations must not consume resource or state slots.
    const auto clock=o->clock;
    o->copy(100,1,2);CHECK(o->untracked_events==1&&!o->resources.get(2)&&!o->state(100,1,2));
    o->map(3,false,true,0);o->map(4,true,true,0);o->unmap(5,0);o->cpu_write(6);
    CHECK(o->untracked_events==5&&o->clock==clock);
    auto texture=d;texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;o->create(7,&texture,1);
    o->copy(100,1,7);b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(uintptr_t(7));o->barrier(*l,100,1,b);
    CHECK(o->untracked_events==7&&!o->resources.lost&&!o->states.lost);
    for(uint64_t address=2;address<=7;++address)CHECK(!o->resources.get(address)&&!o->state(100,1,address));
    size_t resources=0,states=0;
    for(const auto& r:o->resources.entries)if(r.key)++resources;
    for(const auto& state:o->states.entries)if(state.key)++states;
    CHECK(resources==1&&states==1);
    o->enabled=false;o->copy(100,1,8);o->map(8,true,true,0);o->unmap(8,0);o->cpu_write(8);
    CHECK(o->untracked_events==7&&!o->resources.get(8)&&!o->state(100,1,8));
}
static void latestPersistence(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();
    for(size_t i=2;i<startup_records;++i)CHECK(log->push(record(Event::device))); // no startup room
    for(size_t i=0;i<ring_capacity;++i)CHECK(log->push(record(Event::summary)));
    const auto calls=io.calls;const auto failed=failedNew;failNew=true;
    for(const char* prefix:{"l3-t10;","l3-vb;","l3-ib;","l3-state;","l3-link;"})
        CHECK(log->push(record(Event::requirements_metadata,0,0,0,prefix)));
    for(unsigned i=0;i<1000;++i) {
        char text[128]{};std::snprintf(text,sizeof text,"l3-counts;sub=%u;unit=sub;present=unobserved",i);
        CHECK(log->push(record(Event::requirements_metadata,0,0,0,text)));
        std::snprintf(text,sizeof text,"l3-repeats;sub=%u;roles=t10,vb,ib",i);
        CHECK(log->push(record(Event::requirements_metadata,0,0,0,text)));
    }
    CHECK(failNew&&failedNew==failed&&io.calls==calls);failNew=false;log->flush();
    CHECK(linesWith(io.evidence,"text=l3-counts;")==1&&linesWith(io.evidence,"text=l3-repeats;")==1);
    for(const char* prefix:{"l3-t10;","l3-vb;","l3-ib;","l3-state;","l3-link;"})CHECK(linesWith(io.evidence,std::string("text=")+prefix)==1);
    CHECK(io.evidence.find("text=l3-counts;sub=999;")!=io.evidence.npos);
    CHECK(io.evidence.find("text=l3-repeats;sub=999;")!=io.evidence.npos);
    const auto bytes=io.evidence.size();log->push(record(Event::requirements_metadata,0,0,0,"l3-counts;sub=1000"));log->flush();
    CHECK(io.evidence.size()==bytes&&io.evidence.find("text=l3-counts;sub=1000")!=io.evidence.npos);
    CHECK(!log->counters().io_failures&&!log->dropped(Event::requirements_metadata));
    MemoryIO off;off.readState=ReadState::ok;off.config="log_enabled=true";
    auto disabled=std::make_unique<Log>(paths,off,clockNow,true);disabled->flush();
    for(const char* prefix:{"l3-t10;","l3-vb;","l3-ib;","l3-state;","l3-link;","l3-counts;","l3-repeats;"})
        CHECK(!disabled->push(record(Event::requirements_metadata,0,0,0,prefix)));
    disabled->flush();CHECK(off.evidence.find("text=l3-")==off.evidence.npos);
}
static void formatBounds() {
    auto o=std::make_unique<O>();o->enabled=true;auto d=description();
    d.Width=d.Alignment=UINT64_MAX;
    d.Height=UINT32_MAX;d.DepthOrArraySize=d.MipLevels=UINT16_MAX;d.Format=static_cast<DXGI_FORMAT>(UINT32_MAX);
    d.SampleDesc.Count=d.SampleDesc.Quality=UINT32_MAX;d.Layout=static_cast<D3D12_TEXTURE_LAYOUT>(UINT32_MAX);d.Flags=static_cast<D3D12_RESOURCE_FLAGS>(UINT32_MAX);
    D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_READBACK;p.CPUPageProperty=static_cast<D3D12_CPU_PAGE_PROPERTY>(UINT32_MAX);p.MemoryPoolPreference=static_cast<D3D12_MEMORY_POOL>(UINT32_MAX);
    o->create(UINT64_MAX,&d,3,&p);auto* r=o->resources.get(UINT64_MAX);
    CHECK(r);r->desc.Dimension=static_cast<D3D12_RESOURCE_DIMENSION>(UINT32_MAX);
    r->maps=r->unmaps=r->outstanding=r->map_pointers=r->cpu=r->copies=r->writes=r->serial=UINT32_MAX;
    char text[text_capacity]{};o->facts(text,sizeof text,"l3-t10",{UINT64_MAX,UINT64_MAX,UINT64_MAX},33);
    CHECK(std::strstr(text,";cov=partial")&&std::strlen(text)<=437);
    auto value=record(Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,text);value.sequence=UINT64_MAX;
    CHECK(format_record(value).valid);
    o->submission=o->draws=o->submissions_with_draw=o->sub_total=o->sub_min=o->sub_max=o->list_total=o->list_min=o->list_max=UINT32_MAX;
    o->new_address=o->repeat=o->rewritten=o->unknown=o->resources.lost=o->states.lost=o->lists.lost=o->patch_gaps=o->clock=UINT32_MAX;
    o->heaps.fill(UINT32_MAX);o->counts(text,sizeof text);CHECK(std::strstr(text,";saturated=1")&&std::strlen(text)<=437);
    value=record(Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,text);value.sequence=UINT64_MAX;CHECK(format_record(value).valid);
    for(auto& role:o->repeats)role.fresh=role.same=role.write=role.unknown=UINT32_MAX;
    o->repeat_text(text,sizeof text);CHECK(std::strstr(text,"held-pointer-writes=unknown")&&std::strlen(text)<=437);
}
static void realCounterFiles(const fs::path& base) {
    TestPaths paths(base/L"l3-counter-files");fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.logs/L"diagnostics.log",paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})std::ofstream(file).close();
    {std::ofstream config(paths.value.config);config<<"log_enabled=true\ndiagnostic_ownership_pins=true\n";}
    FileIO io;auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();
    for(unsigned sample=0;sample<2;++sample) {
        const char* count=sample?"l3-counts;sub=2":"l3-counts;sub=1";
        const char* repeat=sample?"l3-repeats;sub=2":"l3-repeats;sub=1";
        CHECK(log->push(record(Event::requirements_metadata,0,0,0,count))&&log->push(record(Event::requirements_metadata,0,0,0,repeat)));log->flush();
        std::ifstream file(paths.value.logs/L"diagnostics.evidence.log",std::ios::binary);std::ostringstream bytes;bytes<<file.rdbuf();const auto persisted=bytes.str();
        CHECK(linesWith(persisted,"text=l3-counts;")==1&&linesWith(persisted,"text=l3-repeats;")==1);
        CHECK(persisted.find(count)!=persisted.npos&&persisted.find(repeat)!=persisted.npos);
    }
    CHECK(!log->counters().io_failures);
}
struct Fake {void** table=nullptr;unsigned maps=0,unmaps=0,writes=0,copies=0,barriers=0,closes=0,ia=0,submits=0,last_count=0;const void* last_array=nullptr;};
static HRESULT STDMETHODCALLTYPE map(ID3D12Resource* raw,UINT,const D3D12_RANGE*,void** data) {
    ++reinterpret_cast<Fake*>(raw)->maps;if(data)*data=reinterpret_cast<void*>(uintptr_t(0x1234));return S_OK;
}
static void STDMETHODCALLTYPE unmap(ID3D12Resource* raw,UINT,const D3D12_RANGE*) {++reinterpret_cast<Fake*>(raw)->unmaps;}
static HRESULT STDMETHODCALLTYPE write(ID3D12Resource* raw,UINT,const D3D12_BOX*,const void*,UINT,UINT) {++reinterpret_cast<Fake*>(raw)->writes;return S_OK;}
static void STDMETHODCALLTYPE copyBuffer(ID3D12GraphicsCommandList* raw,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64) {++reinterpret_cast<Fake*>(raw)->copies;}
static void STDMETHODCALLTYPE copyResource(ID3D12GraphicsCommandList* raw,ID3D12Resource*,ID3D12Resource*) {++reinterpret_cast<Fake*>(raw)->copies;}
static void STDMETHODCALLTYPE barriers(ID3D12GraphicsCommandList* raw,UINT,const D3D12_RESOURCE_BARRIER*) {++reinterpret_cast<Fake*>(raw)->barriers;}
static HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList* raw) {++reinterpret_cast<Fake*>(raw)->closes;return S_OK;}
static HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*) {return S_OK;}
static void STDMETHODCALLTYPE vertices(ID3D12GraphicsCommandList* raw,UINT,UINT n,const D3D12_VERTEX_BUFFER_VIEW* v) {
    auto& f=*reinterpret_cast<Fake*>(raw);++f.ia;f.last_count=n;f.last_array=v;
}
static void STDMETHODCALLTYPE index(ID3D12GraphicsCommandList* raw,const D3D12_INDEX_BUFFER_VIEW* v) {auto& f=*reinterpret_cast<Fake*>(raw);++f.ia;f.last_array=v;}
static void STDMETHODCALLTYPE rootAddress(ID3D12GraphicsCommandList* raw,UINT,D3D12_GPU_VIRTUAL_ADDRESS value) {auto& f=*reinterpret_cast<Fake*>(raw);++f.ia;f.last_array=reinterpret_cast<void*>(uintptr_t(value));}
static void STDMETHODCALLTYPE rootTable(ID3D12GraphicsCommandList* raw,UINT,D3D12_GPU_DESCRIPTOR_HANDLE value) {auto& f=*reinterpret_cast<Fake*>(raw);++f.ia;f.last_array=reinterpret_cast<void*>(uintptr_t(value.ptr));}
static void STDMETHODCALLTYPE submit(ID3D12CommandQueue* raw,UINT n,ID3D12CommandList* const* lists) {
    auto& f=*reinterpret_cast<Fake*>(raw);++f.submits;f.last_count=n;f.last_array=lists;
}
static void nativeSeams(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    static std::array<void*,14> rt{};rt[8]=reinterpret_cast<void*>(&map);rt[9]=reinterpret_cast<void*>(&unmap);rt[12]=reinterpret_cast<void*>(&write);
    static Fake resource{rt.data()};auto* raw=reinterpret_cast<ID3D12Resource*>(&resource);void* output=raw;
    auto& s=acz::state();s.objects.try_emplace(rt.data());const auto desc=description();D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_UPLOAD;
    const auto gaps=s.l3.patch_gaps;
    acz::l3Created(S_OK,&output,__uuidof(IUnknown),&desc,1,&p);
    CHECK(s.l3.patch_gaps==gaps); // record-only IID coverage does not attempt a patch
    acz::l3Created(S_OK,&output,__uuidof(IUnknown),&desc,1,&p,false);
    CHECK(s.l3.patch_gaps==gaps+1); // actual unsupported patch route is a gap
    acz::l3Created(S_OK,&output,__uuidof(ID3D12Resource),&desc,1,&p);
    CHECK(rt[8]==reinterpret_cast<void*>(&map)); // raw creation cannot patch an unvalidated table
    acz::l3Created(S_OK,&output,__uuidof(ID3D12Resource),&desc,1,&p,false); // injected prior validation seam
    CHECK(output==raw&&s.l3.resources.get(reinterpret_cast<uintptr_t>(raw))->hooks);
    s.nativeBindings.resource({reinterpret_cast<uintptr_t>(raw),0xabc000,4096,1,false},0xface);
    void* data=nullptr;CHECK(raw->Map(0,nullptr,&data)==S_OK&&data==reinterpret_cast<void*>(uintptr_t(0x1234)));
    auto* mappedResource=s.l3.resources.get(reinterpret_cast<uintptr_t>(raw));
    CHECK(mappedResource->mapped==data&&mappedResource->mapped_subresource==0);
    raw->Unmap(0,nullptr);CHECK(!mappedResource->mapped);CHECK(raw->WriteToSubresource(0,nullptr,nullptr,0,0)==S_OK);
    CHECK(resource.maps==1&&resource.unmaps==1&&resource.writes==1);
    auto* r=s.l3.resources.get(reinterpret_cast<uintptr_t>(raw));CHECK(r->maps==1&&r->unmaps==1&&r->writes==1&&!r->outstanding);
    static std::array<void*,60> lt{};lt[9]=reinterpret_cast<void*>(&close);lt[15]=reinterpret_cast<void*>(&copyBuffer);
    lt[17]=reinterpret_cast<void*>(&copyResource);lt[26]=reinterpret_cast<void*>(&barriers);
    static Fake list{lt.data()};auto* commands=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    auto& state=s.states[commands];const auto id=state.bindings.list_identity,gen=state.bindings.generation;
    auto& hooks=s.lists[lt.data()];acz::l3InstallList_locked(lt.data(),hooks);
    commands->CopyBufferRegion(raw,5,raw,7,9);commands->CopyResource(raw,raw);
    static Fake unrecorded{rt.data()};const auto untracked=s.l3.untracked_events;
    commands->CopyResource(reinterpret_cast<ID3D12Resource*>(&unrecorded),raw);
    CHECK(s.l3.untracked_events==untracked+1&&!s.l3.resources.get(reinterpret_cast<uintptr_t>(&unrecorded)));
    CHECK(!s.l3.state(id,gen,reinterpret_cast<uintptr_t>(&unrecorded)));
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=raw;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_INDEX_BUFFER;
    commands->ResourceBarrier(1,&b);CHECK(commands->Close()==S_OK);
    CHECK(list.copies==3&&list.barriers==1&&list.closes==1&&r->copies==2);
    auto* l=s.l3.list(id,gen);CHECK(l&&l->closed&&l->closes==1);
    CHECK(s.l3.state(id,gen,reinterpret_cast<uintptr_t>(raw))->after==D3D12_RESOURCE_STATE_INDEX_BUFFER);
    // Same registry used by native IA/root seams, with exact unmapped refusal.
    // NativeBindingObserver is neither copyable nor movable: use the existing observer with a unique address range and device.
    CHECK(s.nativeBindings.diagnostic_address(0xabc020,64,0xface)==reinterpret_cast<uintptr_t>(raw));
    CHECK(!s.nativeBindings.diagnostic_address(0xabc020,64,0xbeef)&&!s.nativeBindings.diagnostic_address(0xbad,64,0xface));
    state.native_binding_device=0xface;lt[43]=reinterpret_cast<void*>(&index);lt[44]=reinterpret_cast<void*>(&vertices);
    CHECK(acz::patch(lt.data(),43,reinterpret_cast<void*>(&acz::setIndex),hooks));
    CHECK(acz::patch(lt.data(),44,reinterpret_cast<void*>(&acz::setVertices),hooks));
    D3D12_VERTEX_BUFFER_VIEW vb{0xabc020,128,64};D3D12_INDEX_BUFFER_VIEW ib{0xabc100,18,DXGI_FORMAT_R16_UINT};
    commands->IASetVertexBuffers(0,1,&vb);CHECK(list.last_count==1&&list.last_array==&vb&&l->sources[1].address==reinterpret_cast<uintptr_t>(raw));
    commands->IASetIndexBuffer(&ib);CHECK(list.last_array==&ib&&l->sources[33].address==reinterpret_cast<uintptr_t>(raw));
    vb.BufferLocation=0xbad;commands->IASetVertexBuffers(0,1,&vb);CHECK(!l->sources[1].address&&l->sources[1].va==0xbad);
    vb.BufferLocation=0xabc020;commands->IASetVertexBuffers(0,1,&vb);CHECK(list.ia==4);
    acz::metadata::RootLayout layout;acz::metadata::RootParameter parameter;parameter.kind=acz::metadata::ParameterKind::srv;
    parameter.shader_register=10;parameter.visibility=acz::metadata::Visibility::geometry;layout.parameters.push_back(parameter);
    auto root=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(root.valid());acz::l3ConfigureT10(*l,root);
    {// J-world-objects: a vertex-visible root SRV t10 (DrawACM_v has no GS) qualifies only with gpu_copy_step5_world_objects.
     CHECK(l->t10_parameter==0&&!l->t10_table);
     layout.parameters[0].visibility=acz::metadata::Visibility::vertex;
     auto vertexRoot=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(vertexRoot.valid());
     acz::g_worldObjectsTest=false;acz::l3ConfigureT10(*l,vertexRoot);CHECK(l->t10_parameter==UINT32_MAX&&!l->t10_table);
     // J-world-objects v2 step 1: the read-only observer needs the same record location (key world_observe alone also accepts it).
     acz::g_worldObserveTest=true;acz::l3ConfigureT10(*l,vertexRoot);CHECK(l->t10_parameter==0&&!l->t10_table);
     acz::g_worldObserveTest=false;acz::l3ConfigureT10(*l,vertexRoot);CHECK(l->t10_parameter==UINT32_MAX&&!l->t10_table);
     acz::g_worldObjectsTest=true;acz::l3ConfigureT10(*l,vertexRoot);CHECK(l->t10_parameter==0&&!l->t10_table);
     layout.parameters[0].visibility=acz::metadata::Visibility::pixel; // never a t10 reader
     auto pixelRoot=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(pixelRoot.valid());
     acz::l3ConfigureT10(*l,pixelRoot);CHECK(l->t10_parameter==UINT32_MAX);
     acz::g_worldObjectsTest=false;layout.parameters[0].visibility=acz::metadata::Visibility::geometry;acz::l3ConfigureT10(*l,root);CHECK(l->t10_parameter==0);}
    {// J-world-objects v2: DrawACM_v has no GS, so its root signature very probably carries DENY_GEOMETRY_SHADER_ROOT_ACCESS (16). That flag must
     // not hide the VERTEX-visible root SRV t10 (live step30: every ACM draw had t10-va=0 = no record location); key off keeps the old rule.
     layout.flags=16;layout.parameters[0].visibility=acz::metadata::Visibility::vertex;
     auto denyGs=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(denyGs.valid());
     acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;acz::l3ConfigureT10(*l,denyGs);CHECK(l->t10_parameter==UINT32_MAX&&!l->t10_table);
     acz::g_worldObserveTest=true;acz::l3ConfigureT10(*l,denyGs);CHECK(l->t10_parameter==0&&!l->t10_table);
     acz::g_worldObserveTest=false;acz::g_worldObjectsTest=true;acz::l3ConfigureT10(*l,denyGs);CHECK(l->t10_parameter==0&&!l->t10_table);
     layout.flags=16|2; // vertex shader root access denied as well: nothing can read it
     auto denyBoth=acz::metadata::RootBindingMetadata::from_adapter(layout);
     acz::l3ConfigureT10(*l,denyBoth);CHECK(l->t10_parameter==UINT32_MAX);
     layout.flags=16;layout.parameters[0].visibility=acz::metadata::Visibility::geometry; // the DrawCPT rule is unchanged: DENY_GS hides a GS-visible t10
     auto gsDenied=acz::metadata::RootBindingMetadata::from_adapter(layout);
     acz::l3ConfigureT10(*l,gsDenied);CHECK(l->t10_parameter==UINT32_MAX);
     acz::g_worldObjectsTest=false;layout.flags=0;layout.parameters[0].visibility=acz::metadata::Visibility::geometry;
     acz::l3ConfigureT10(*l,root);CHECK(l->t10_parameter==0);}
    {// t10-va=0 hunt (live step31: the vertex-only fix did not help). An ACM-like root signature (manifest 2: param 0 PS SRV table t1, param 1 PS sampler
     // table, param 2 root SRV t10, param 3 two root constants b10) with the root SRV at index 2, ALL- or VERTEX-visible, with and without DENY flags.
     // The cockpit rule stays exactly as it was.
     namespace m=acz::metadata;
     auto acmLike=[&](m::Visibility t10Visibility,uint32_t flags,unsigned t10Index) {
         m::RootLayout x;x.flags=flags;x.parameters.resize(4);
         x.parameters[0].kind=m::ParameterKind::table;x.parameters[0].visibility=m::Visibility::pixel;
         {m::DescriptorRange r;r.kind=m::BindingKind::srv;r.count=1;r.base_register=1;x.parameters[0].ranges.push_back(r);}
         x.parameters[1].kind=m::ParameterKind::table;x.parameters[1].visibility=m::Visibility::pixel;
         {m::DescriptorRange r;r.kind=m::BindingKind::sampler;r.count=1;r.base_register=0;x.parameters[1].ranges.push_back(r);}
         x.parameters[3].kind=m::ParameterKind::constants;x.parameters[3].visibility=m::Visibility::all;x.parameters[3].shader_register=10;x.parameters[3].constant_count=2;
         x.parameters[2].kind=m::ParameterKind::srv;x.parameters[2].visibility=t10Visibility;x.parameters[2].shader_register=10;
         if(t10Index!=2)std::swap(x.parameters[2],x.parameters[t10Index]);
         auto built=m::RootBindingMetadata::from_adapter(x);CHECK(built.valid());return built;
     };
     auto run=[&](const m::RootBindingMetadata& r,bool worldKey,acz::L3T10Outcome* out=nullptr) {
         acz::g_worldObjectsTest=false;acz::g_worldObserveTest=worldKey;acz::l3ConfigureT10(*l,r,out);acz::g_worldObserveTest=false;return l->t10_parameter;
     };
     const uint32_t denyAll=1|4|8|16; // ALLOW_IA + DENY HS/DS/GS: the usual VS+PS signature
     // ALL-visible root SRV t10 at index 2 in a signature that denies the geometry stage: found only with a world key (cockpit rule: DENY_GS hides it)
     {auto r=acmLike(m::Visibility::all,denyAll,2);acz::L3T10Outcome o;
      CHECK(run(r,false)==UINT32_MAX&&!l->t10_table);
      CHECK(run(r,true,&o)==2&&!l->t10_table&&o.matches==1&&o.worldMatches==1&&o.cockpitMatches==0);
      acz::g_worldObserveTest=false;acz::g_worldObjectsTest=false;acz::g_phaseObserveTest=true;acz::l3ConfigureT10(*l,r);acz::g_phaseObserveTest=false;CHECK(l->t10_parameter==2);
      acz::g_worldObserveTest=false;acz::g_worldObjectsTest=true;acz::l3ConfigureT10(*l,r);acz::g_worldObjectsTest=false;CHECK(l->t10_parameter==2);}
     // VERTEX-visible at index 2 (the previous fix), with and without the deny flags; at another index
     {auto r=acmLike(m::Visibility::vertex,denyAll,2);CHECK(run(r,false)==UINT32_MAX&&run(r,true)==2);
      auto r0=acmLike(m::Visibility::vertex,0,2);CHECK(run(r0,false)==UINT32_MAX&&run(r0,true)==2);
      auto r3=acmLike(m::Visibility::vertex,denyAll,3);CHECK(run(r3,true)==3&&run(r3,false)==UINT32_MAX);
      auto a3=acmLike(m::Visibility::all,denyAll,3);CHECK(run(a3,true)==3&&run(a3,false)==UINT32_MAX);}
     // the vertex shader reads it: DENY_VERTEX_SHADER_ROOT_ACCESS (2) hides it for every key
     {for(auto v:{m::Visibility::all,m::Visibility::vertex}){auto r=acmLike(v,denyAll|2,2);CHECK(run(r,true)==UINT32_MAX&&run(r,false)==UINT32_MAX);}}
     // pixel/hull/domain-visible never qualifies as the world record location
     {for(auto v:{m::Visibility::pixel,m::Visibility::hull,m::Visibility::domain}){auto r=acmLike(v,0,2);CHECK(run(r,true)==UINT32_MAX&&run(r,false)==UINT32_MAX);}}
     // cockpit root signature unchanged: ALL or GEOMETRY visible, no DENY_GS -> found with every key combination; DENY_GS hides a GS-visible one
     {for(auto v:{m::Visibility::all,m::Visibility::geometry}){auto r=acmLike(v,0,2);acz::L3T10Outcome o;
       CHECK(run(r,false,&o)==2&&o.cockpitMatches==1&&o.worldMatches==0&&o.matches==1);CHECK(run(r,true,&o)==2&&o.cockpitMatches==1&&o.worldMatches==0);}
      auto gs=acmLike(m::Visibility::geometry,16,2);CHECK(run(gs,false)==UINT32_MAX&&run(gs,true)==UINT32_MAX);}
     // ambiguity stays refused: two stage-disjoint tables that both cover t10 (a vertex one and a geometry one) are two candidates for the world rule, one for the cockpit rule
     // (the adapter itself refuses an ALL-visible root SRV t10 next to an overlapping table)
     {m::RootLayout x;x.flags=0;x.parameters.resize(2);
      for(unsigned k=0;k<2;++k){x.parameters[k].kind=m::ParameterKind::table;x.parameters[k].visibility=k?m::Visibility::geometry:m::Visibility::vertex;
       m::DescriptorRange rg;rg.kind=m::BindingKind::srv;rg.count=16;rg.base_register=0;x.parameters[k].ranges.push_back(rg);}
      auto amb=m::RootBindingMetadata::from_adapter(x);CHECK(amb.valid());acz::L3T10Outcome o;
      // J-180 (Codex review P2): the two candidates are STAGE-DISJOINT (a vertex one and a geometry one): each stage has exactly one, so the cockpit's
      // geometry binding (index 1) and the world's vertex binding (index 0) are both kept (the old code cleared both because `matches` crossed the stages).
      CHECK(run(amb,true,&o)==1&&o.matches==2&&l->t10_table&&l->t10_offset==10&&l->t10_vs_parameter==0&&l->t10_vs_table&&l->t10_vs_offset==10);
      CHECK(run(amb,false,&o)==1&&l->t10_table&&l->t10_offset==10&&o.matches==1&&o.cockpitMatches==1&&l->t10_vs_parameter==UINT32_MAX); // key off: the geometry-visible table only, as before
      x.parameters[1].visibility=m::Visibility::pixel;auto vertexTable=m::RootBindingMetadata::from_adapter(x);CHECK(vertexTable.valid());
      CHECK(run(vertexTable,true)==0&&l->t10_table&&l->t10_offset==10);
      CHECK(run(vertexTable,false)==UINT32_MAX);}
     // a rejected (invalid) layout configures nothing
     {m::RootLayout x;x.static_sampler_count=1;auto bad=m::RootBindingMetadata::from_adapter(x);CHECK(!bad.valid()&&run(bad,true)==UINT32_MAX);}
     acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;
     acz::l3ConfigureT10(*l,root);CHECK(l->t10_parameter==0); // back to the fixture's root (geometry-visible index 0)
    }
    lt[40]=reinterpret_cast<void*>(&rootAddress);
    CHECK(acz::patch(lt.data(),40,reinterpret_cast<void*>(&acz::setAddress<40,acz::metadata::ParameterKind::srv>),hooks));
    commands->SetGraphicsRootShaderResourceView(0,0xabc200);CHECK(l->sources[0].address==reinterpret_cast<uintptr_t>(raw)&&l->sources[0].va==0xabc200);
    commands->SetGraphicsRootShaderResourceView(0,0xbad);CHECK(!l->sources[0].address&&l->sources[0].va==0xbad);
    {// ACM-like flow: root SRV t10 ALL-visible at index 2 in a DENY_GS signature. With a world key the setter records the VA of parameter 2 only.
     acz::metadata::RootLayout x;x.flags=1|4|8|16;x.parameters.resize(3);
     x.parameters[0].kind=acz::metadata::ParameterKind::table;x.parameters[0].visibility=acz::metadata::Visibility::pixel;
     {acz::metadata::DescriptorRange rg;rg.kind=acz::metadata::BindingKind::srv;rg.count=1;rg.base_register=1;x.parameters[0].ranges.push_back(rg);}
     x.parameters[1].kind=acz::metadata::ParameterKind::srv;x.parameters[1].visibility=acz::metadata::Visibility::pixel;x.parameters[1].shader_register=3;
     x.parameters[2].kind=acz::metadata::ParameterKind::srv;x.parameters[2].visibility=acz::metadata::Visibility::all;x.parameters[2].shader_register=10;
     auto acm=acz::metadata::RootBindingMetadata::from_adapter(x);CHECK(acm.valid());
     acz::g_worldObserveTest=true;acz::l3ConfigureT10(*l,acm);acz::g_worldObserveTest=false;CHECK(l->t10_parameter==2);
     l->sources[0]={};commands->SetGraphicsRootShaderResourceView(1,0xabc300);CHECK(l->sources[0].va==0);
     commands->SetGraphicsRootShaderResourceView(2,0xabc200);CHECK(l->sources[0].va==0xabc200&&l->sources[0].address==reinterpret_cast<uintptr_t>(raw));
     acz::l3ConfigureT10(*l,root);CHECK(l->t10_parameter==0);
     commands->SetGraphicsRootShaderResourceView(0,0xbad);CHECK(!l->sources[0].address&&l->sources[0].va==0xbad);}
    {// J-180 (Codex review P2): a VERTEX-visible root SRV t10 at parameter 2 (the ACM VS) and a GEOMETRY-visible t10 at parameter 4 (the cockpit GS): disjoint
     // stages, each unambiguous. Both bindings stay usable and are tracked separately; each stage honours its own deny flag.
     namespace m=acz::metadata;
     auto split=[&](uint32_t flags) {
         m::RootLayout x;x.flags=flags;x.parameters.resize(5);
         x.parameters[0].kind=m::ParameterKind::table;x.parameters[0].visibility=m::Visibility::pixel;
         {m::DescriptorRange r;r.kind=m::BindingKind::srv;r.count=1;r.base_register=1;x.parameters[0].ranges.push_back(r);}
         x.parameters[1].kind=m::ParameterKind::table;x.parameters[1].visibility=m::Visibility::pixel;
         {m::DescriptorRange r;r.kind=m::BindingKind::sampler;r.count=1;r.base_register=0;x.parameters[1].ranges.push_back(r);}
         x.parameters[2].kind=m::ParameterKind::srv;x.parameters[2].visibility=m::Visibility::vertex;x.parameters[2].shader_register=10;
         x.parameters[3].kind=m::ParameterKind::constants;x.parameters[3].visibility=m::Visibility::all;x.parameters[3].shader_register=10;x.parameters[3].constant_count=2;
         x.parameters[4].kind=m::ParameterKind::srv;x.parameters[4].visibility=m::Visibility::geometry;x.parameters[4].shader_register=10;
         auto built=m::RootBindingMetadata::from_adapter(x);CHECK(built.valid());return built;
     };
     auto configure=[&](const m::RootBindingMetadata& r,bool worldKey,acz::L3T10Outcome* out=nullptr){
         acz::g_worldObjectsTest=false;acz::g_worldObserveTest=worldKey;acz::l3ConfigureT10(*l,r,out);acz::g_worldObserveTest=false;};
     {acz::L3T10Outcome o;configure(split(0),true,&o);
      CHECK(l->t10_parameter==4&&!l->t10_table&&l->t10_vs_parameter==2&&!l->t10_vs_table&&o.matches==2&&o.cockpitMatches==1&&o.worldMatches==1);
      l->sources[0]={};l->vs_source={};
      commands->SetGraphicsRootShaderResourceView(2,0xabc200); // the VS binding: tracked on its own, the cockpit's source untouched
      CHECK(l->vs_source.va==0xabc200&&l->vs_source.address==reinterpret_cast<uintptr_t>(raw)&&l->sources[0].va==0);
      commands->SetGraphicsRootShaderResourceView(4,0xabc300); // the GS binding: the cockpit's source, the VS one untouched
      CHECK(l->sources[0].va==0xabc300&&l->vs_source.va==0xabc200);
      commands->SetGraphicsRootShaderResourceView(3,0xabc400);CHECK(l->sources[0].va==0xabc300&&l->vs_source.va==0xabc200);} // an unrelated parameter moves neither
     // key off: the cockpit rule alone, exactly as before (no vertex-stage binding)
     configure(split(0),false);CHECK(l->t10_parameter==4&&l->t10_vs_parameter==UINT32_MAX);
     // DENY_GEOMETRY_SHADER_ROOT_ACCESS hides the GS one: the VS binding is the only candidate of the root (general binding = it, as the single-candidate rule)
     configure(split(16),true);CHECK(l->t10_parameter==2&&l->t10_vs_parameter==2);
     configure(split(16),false);CHECK(l->t10_parameter==UINT32_MAX&&l->t10_vs_parameter==UINT32_MAX);
     // DENY_VERTEX_SHADER_ROOT_ACCESS hides the VS one: the GS binding stays the cockpit's, no vertex-stage binding
     configure(split(2),true);CHECK(l->t10_parameter==4&&l->t10_vs_parameter==UINT32_MAX);
     configure(split(16|2),true);CHECK(l->t10_parameter==UINT32_MAX&&l->t10_vs_parameter==UINT32_MAX);
     // two VERTEX-visible candidates stay ambiguous INSIDE their stage (the geometry one remains the cockpit's)
     {m::RootLayout x;x.flags=0;x.parameters.resize(3);
      x.parameters[0].kind=m::ParameterKind::srv;x.parameters[0].visibility=m::Visibility::vertex;x.parameters[0].shader_register=10;
      x.parameters[1].kind=m::ParameterKind::table;x.parameters[1].visibility=m::Visibility::vertex;x.parameters[1].shader_register=0;
      {m::DescriptorRange rg;rg.kind=m::BindingKind::srv;rg.count=16;rg.base_register=0;x.parameters[1].ranges.push_back(rg);}
      x.parameters[2].kind=m::ParameterKind::srv;x.parameters[2].visibility=m::Visibility::geometry;x.parameters[2].shader_register=10;
      auto vv=m::RootBindingMetadata::from_adapter(x);
      if(vv.valid()){configure(vv,true);CHECK(l->t10_parameter==2&&l->t10_vs_parameter==UINT32_MAX);}}
     acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;
     acz::l3ConfigureT10(*l,root);CHECK(l->t10_parameter==0&&l->t10_vs_parameter==UINT32_MAX);
     commands->SetGraphicsRootShaderResourceView(0,0xbad);}
    s.nativeBindings.diagnostic_tracking(true);s.nativeBindings.heap({0xcafe,0x10000,0x20000,4,32,0,1,0},0xface);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_UNKNOWN;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=8;srv.Buffer.StructureByteStride=416;
    s.nativeBindings.srv(reinterpret_cast<uintptr_t>(raw),&srv,0x10000,0xface);
    layout.parameters[0].kind=acz::metadata::ParameterKind::table;layout.parameters[0].shader_register=0;layout.parameters[0].ranges={{acz::metadata::BindingKind::srv,1,10,0,0,0}};
    auto tableRoot=acz::metadata::RootBindingMetadata::from_adapter(layout);CHECK(tableRoot.valid());acz::l3ConfigureT10(*l,tableRoot);
    state.bindings.heaps={0xcafe};lt[32]=reinterpret_cast<void*>(&rootTable);
    CHECK(acz::patch(lt.data(),32,reinterpret_cast<void*>(&acz::setTable),hooks));
    commands->SetGraphicsRootDescriptorTable(0,{0x20000});s.l3.draw(*l);CHECK(l->sources[0].address==reinterpret_cast<uintptr_t>(raw));
    // Submission join is at call entry and duplicates keep the list ordinals,
    // while distinct recorded draws are counted only once in that call.
    s.l3.draw(*l);s.l3.draw(*l);
    static std::array<void*,11> qt{};qt[10]=reinterpret_cast<void*>(&submit);static Fake queue{qt.data()};
    auto* q=reinterpret_cast<ID3D12CommandQueue*>(&queue);auto& qhooks=s.queues[qt.data()];
    CHECK(acz::patch(qt.data(),10,reinterpret_cast<void*>(&acz::hookedExecuteCommandLists),qhooks));
    ID3D12CommandList* submitted[]={commands,commands};const auto depth=acz::g_callbackDepth;acz::g_callbackDepth=1;
    const auto before=s.l3.sub_total;CHECK(l->draws==3);
    q->ExecuteCommandLists(2,submitted);acz::g_callbackDepth=depth;
    CHECK(queue.submits==1&&queue.last_count==2&&queue.last_array==submitted&&l->executes==2);
    CHECK(s.l3.sub_total==O::add(before,3)&&s.l3.submission);
    // Feed actual producer facts through production serialization, not only
    // string format checks. Each sample has an independent companion key.
    char text[text_capacity]{};
    for(const char* prefix:{"l3-t10","l3-vb","l3-ib"}) {
        s.l3.facts(text,sizeof text,prefix,{reinterpret_cast<uintptr_t>(raw),0xabc020,64},0);
        emit(record(Event::requirements_metadata,id,gen,1,text));
    }
    s.l3.state_text(text,sizeof text,*l,id,gen,{reinterpret_cast<uintptr_t>(raw),0,0},0);emit(record(Event::requirements_metadata,id,gen,1,text));
    log->flush();set_sink(nullptr);
    CHECK(raw->Map(0,nullptr,&data)==S_OK&&resource.maps==2&&r->maps==1); // installed switch-OFF forwarding
    for(const char* prefix:{"l3-t10;","l3-vb;","l3-ib;","l3-state;"})CHECK(linesWith(io.evidence,std::string("text=")+prefix)==1);
    CHECK(io.evidence.find("heap=UPLOAD")!=io.evidence.npos&&io.evidence.find("map=1;unmap=1;out=0;ptr=0")!=io.evidence.npos);
    CHECK(io.evidence.find(";bt=1;bu=0;ba=0;")!=io.evidence.npos); // closing transition persisted
    CHECK(linesWith(io.evidence,"text=l3-link;")==1&&io.evidence.find(";ordinal=0;")!=io.evidence.npos);
    CHECK(io.files[0].find(";ordinal=1;")!=io.files[0].npos);
    CHECK(io.evidence.find("text=l3-counts;")!=io.evidence.npos&&io.evidence.find("text=l3-repeats;")!=io.evidence.npos);
    set_sink(log.get());const auto oldGeneration=l->generation,oldDraws=l->draws;
    acz::resetBindings(commands,false);CHECK(acz::l3_list_locked(s.states.at(commands))==l&&l->generation==oldGeneration&&l->draws==oldDraws);
    CHECK(!l->sources[0].address&&!l->sources[1].address&&l->selected_count==1);
    lt[10]=reinterpret_cast<void*>(&reset);CHECK(acz::patch(lt.data(),10,reinterpret_cast<void*>(&acz::hookedReset),hooks));
    s.states.at(commands).interfaces_observed=true;s.states.at(commands).capture_hooks=true;
    CHECK(commands->Reset(nullptr,nullptr)==S_OK&&l->resets==1&&l->generation==oldGeneration+1&&!l->draws&&!l->executes&&!l->closes&&!l->selected_count);
    CHECK(!s.l3.state(id,l->generation,reinterpret_cast<uintptr_t>(raw)));set_sink(nullptr);
    CHECK(!log->counters().io_failures);s.objects.erase(rt.data());s.lists.erase(lt.data());s.queues.erase(qt.data());s.states.erase(commands);
}
static void cases(TestPaths& paths) {
    inventory();writeRegressions();formatBounds();latestPersistence(paths);nativeSeams(paths);realCounterFiles(paths.value.logs.parent_path());
    // Reconstruct in place: no multi-MiB automatic temporary, and no stale
    // native fixture inventory/counters leaking into the later hook fixtures.
    auto& o=acz::state().l3;o.~L3Observation();new (&o) O;
    CHECK(!o.enabled&&!o.clock&&!o.submission&&!o.creation&&!o.untracked_events&&!o.patch_gaps);
    for(const auto& r:o.resources.entries)CHECK(!r.key);
    for(const auto& s:o.states.entries)CHECK(!s.key);
    for(const auto& l:o.lists.entries)CHECK(!l.key);
}
}
