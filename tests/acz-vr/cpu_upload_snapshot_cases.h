#pragma once
namespace cpu_fixture {
using C=acz::diagnostics::CpuUploadSnapshot;
static unsigned reads=0,mutate_at=0,fault_at=0;
static unsigned char* mutate=nullptr;
static bool reader(const void* from,void* to,size_t n) noexcept {
    ++reads;if(reads==fault_at)return false;
    if(reads==mutate_at&&mutate)++*mutate;
    std::memcpy(to,from,n);return true;
}
static uint64_t word(const unsigned char* p) {uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);return v;}
static C::Request request(std::array<unsigned char,1024>& data) {
    C::Request q;q.accepted=true;q.list=1;q.generation=2;q.revision=3;q.draw=4;
    q.vertex_stride=64;q.index_stride=2;q.count=9;
    q.ranges[0]={10,1,1,0x1040,64,416,1024,data.data(),true,true};
    q.ranges[1]={10,1,1,0x11e0,480,512,1024,data.data(),true,true};
    q.ranges[2]={11,2,2,0x13e0,992,18,1024,data.data(),true,true};
    return q;
}
static void bytesAndFailures(TestPaths& paths) {
    auto c=std::make_unique<C>();std::array<unsigned char,1024> data{};
    for(size_t i=0;i<992;++i)data[i]=static_cast<unsigned char>(i);
    const uint16_t indices[]={0,1,2,2,3,4,5,6,7};std::memcpy(data.data()+992,indices,sizeof indices);
    auto q=request(data);reads=mutate_at=fault_at=0;mutate=nullptr;
    CHECK(!c->reserve(false,q)&&!c->attempts&&!reads);
    CHECK(c->reserve(true,q)&&!c->reserve(true,q));
    failNew=true;CHECK(c->read(q,reader)&&failNew);failNew=false;
    CHECK(reads==6&&c->stable&&c->file_bytes==C::header_size+946);
    CHECK(c->minimum==0&&c->maximum==7&&c->vertices==8);
    CHECK(c->matches(q));
    auto different=q;++different.draw;CHECK(!c->matches(different));
    different=q;++different.generation;CHECK(!c->matches(different));
    different=q;++different.ranges[2].map_generation;CHECK(!c->matches(different));
    different=q;++different.ranges[2].lifetime;CHECK(!c->matches(different));
    different=q;different.ranges[2].pinned=false;CHECK(!c->matches(different));
    c->stable=false;CHECK(!c->matches(q));c->stable=true;
    CHECK(std::string_view(c->index_reason)=="verified:index-vertex-range");
    CHECK(c->hashes[0]==C::hash(data.data()+64,416));
    CHECK(c->hashes[1]==C::hash(data.data()+480,512)&&c->hashes[2]==C::hash(data.data()+992,18));
    CHECK(C::hash(reinterpret_cast<const unsigned char*>("hello"),5)==0xa430d84680aabd0bULL);
    CHECK(!std::memcmp(c->file.data(),"ACZCPU1",7)&&word(c->file.data()+8)==320&&word(c->file.data()+16)==946);
    for(size_t i=0,at=0;i<3;++i) {
        CHECK(word(c->file.data()+56+i*64+40)==q.ranges[i].bytes);
        CHECK(word(c->file.data()+56+i*64+48)==c->hashes[i]);
        CHECK(!std::memcmp(c->file.data()+C::header_size+at,data.data()+q.ranges[i].offset,size_t(q.ranges[i].bytes)));
        at+=size_t(q.ranges[i].bytes);
    }
    auto refusal=[&](const C::Request& bad,const char* why) {
        reads=0;CHECK(!c->read(bad,reader)&&!c->stable&&!c->file_bytes);
        CHECK(std::string_view(c->reason)==why);
    };
    auto bad=q;bad.ranges[0].pinned=false;refusal(bad,"unpinned");CHECK(!reads);
    bad=q;bad.ranges[0].mapped=nullptr;refusal(bad,"unmapped");CHECK(!reads);
    bad=q;bad.ranges[0].upload=false;refusal(bad,"not-upload");CHECK(!reads);
    bad=q;bad.ranges[2].offset=1010;refusal(bad,"out-of-range");CHECK(!reads);
    bad=q;bad.ranges[0].offset=UINT64_MAX;refusal(bad,"out-of-range");CHECK(!reads);
    bad=q;bad.ranges[1].bytes=2048;refusal(bad,"out-of-range");CHECK(!reads);
    bad=q;bad.accepted=false;refusal(bad,"metadata-unaccepted");CHECK(!reads);
    reads=0;fault_at=4;refusal(q,"read-fault");fault_at=0;
    mutate=data.data()+64;mutate_at=4;refusal(q,"unstable");mutate_at=0;mutate=nullptr;
    CHECK(c->read(q,reader));
    // Live D-155 index sequence: cut only when BOTH exact facts permit it.
    const uint16_t strips[]={0,1,2,3,0xffff,4,5,6,7};
    std::memcpy(data.data()+992,strips,sizeof strips);
    q.strip_cut=1;
    for(uint32_t topology:{3u,5u,11u,13u}) {
        q.topology=topology;CHECK(c->read(q,reader));
        CHECK(c->minimum==0&&c->maximum==7&&c->vertices==8&&
            std::string_view(c->index_reason)=="verified:index-vertex-range");
    }
    q.topology=4;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range"&&c->maximum==0xffff);
    q.start=4;q.count=1;q.base=-65535; // ordinary sentinel can pass normal signed-base bounds
    CHECK(c->read(q,reader)&&c->minimum==0xffff&&c->maximum==0xffff&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    q.start=0;q.count=9;q.base=0;
    q.topology=5;q.strip_cut=0;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.strip_cut=2;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.strip_cut=1;q.topology=0;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="strip-cut index requires unavailable topology metadata");
    q.topology=5;q.strip_cut=acz::index_range::unknown_cut;
    CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="strip-cut index requires unavailable topology metadata");
    q.strip_cut=1;q.base=-1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.base=0;q.start=5;q.count=4;
    CHECK(c->read(q,reader)&&c->minimum==4&&c->maximum==7&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    q.start=4;q.count=1;q.base=INT32_MIN; // a restart fetches nothing, even with a negative base
    CHECK(c->read(q,reader)&&c->minimum==0&&c->maximum==0&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    q.start=0;q.count=9;q.base=0;std::memcpy(data.data()+992,indices,sizeof indices);
    q.base=-1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.base=1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.base=0;q.start=1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-byte-range");
    q.start=0;q.index_stride=4;q.count=4;
    const uint32_t indices32[]={1,2,3,7};std::memcpy(data.data()+992,indices32,sizeof indices32);
    CHECK(c->read(q,reader)&&c->minimum==1&&c->maximum==7&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    const uint32_t cuts32[]={1,0xffffffffu,3,7};std::memcpy(data.data()+992,cuts32,sizeof cuts32);
    q.strip_cut=2;q.topology=13;
    CHECK(c->read(q,reader)&&c->minimum==1&&c->maximum==7&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    q.base=-1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="verified:index-vertex-range");
    q.base=1;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");q.base=0;
    q.strip_cut=1;CHECK(c->read(q,reader)&&c->maximum==0xffffffffu&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.strip_cut=0;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    q.strip_cut=2;q.topology=12;CHECK(c->read(q,reader)&&std::string_view(c->index_reason)=="failed:index-vertex-range");
    std::memcpy(data.data()+992,indices32,sizeof indices32);
    for(unsigned i=0;i<3;++i){++q.draw;CHECK(c->reserve(true,q));}++q.draw;CHECK(!c->reserve(true,q)&&c->attempts==4);
    // The actual SEH leaf catches a fault; no native API or allocation is involved.
    unsigned char output=0;CHECK(!acz::guardedCpuRead(nullptr,&output,1));
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();
    const auto calls=io.calls;failNew=true;
    for(unsigned i=0;i<4;++i)CHECK(log->raw_snapshot(c->file.data(),c->file_bytes));
    CHECK(!log->raw_snapshot(c->file.data(),c->file_bytes)&&failNew&&calls==io.calls);failNew=false;
    log->flush();CHECK(!log->counters().io_failures);
    for(const auto& raw:io.raw)CHECK(raw.size()==c->file_bytes&&!std::memcmp(raw.data(),c->file.data(),raw.size()));
    // The exact hard payload boundary is accepted; one additional byte refuses.
    auto largeData=std::make_unique<std::array<unsigned char,4096>>();
    auto maximum=q;maximum.index_stride=2;maximum.count=1;
    for(auto& x:maximum.ranges){x.mapped=largeData->data();x.resource_bytes=largeData->size();}
    maximum.ranges[0].offset=0;maximum.ranges[1].offset=416;maximum.ranges[1].bytes=1614;
    maximum.ranges[2].offset=2030;
    CHECK(c->read(maximum,reader)&&c->file_bytes==C::file_cap);
    ++maximum.ranges[1].bytes;refusal(maximum,"out-of-range");CHECK(!reads);
    CHECK(c->read(q,reader));
    MemoryIO off;off.readState=ReadState::ok;off.config="log_enabled=true";
    auto offLog=std::make_unique<Log>(paths,off,clockNow,true);offLog->flush();
    CHECK(!offLog->raw_snapshot(c->file.data(),c->file_bytes));offLog->flush();for(const auto& raw:off.raw)CHECK(raw.empty());
    // Unmap and reincarnation clear pointers; old data never revives by address.
    auto o=std::make_unique<acz::diagnostics::L3Observation>();o->enabled=true;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=1024;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    o->create(77,&desc,1,&heap);o->map(77,true,true,0,data.data());auto* r=o->resources.get(77);
    CHECK(r->mapped==data.data()&&r->mapped_subresource==0);const auto gen=r->map_generation;
    o->unmap(77,0);CHECK(!r->mapped&&r->map_generation>gen);
    o->map(77,true,true,0,data.data());o->create(77,&desc,1,&heap);CHECK(!r->mapped&&!r->outstanding);
    o->map(77,true,true,0,data.data());r->invalidate_map();r->retired=true;CHECK(!r->mapped);
    // Actual FileIO through the consumer validates bytes and four-file cap.
    TestPaths disk(paths.value.logs.parent_path()/L"cpu-real-files");
    fs::create_directories(disk.value.game);fs::create_directories(disk.value.logs);
    {std::ofstream config(disk.value.config);config<<"log_enabled=true\ndiagnostic_ownership_pins=true\n";}
    FileIO fileIO;auto fileLog=std::make_unique<Log>(disk,fileIO,clockNow,true);fileLog->flush();
    for(unsigned i=0;i<4;++i)CHECK(fileLog->raw_snapshot(c->file.data(),c->file_bytes));
    CHECK(!fileLog->raw_snapshot(c->file.data(),c->file_bytes));fileLog->flush();CHECK(!fileLog->counters().io_failures);
    for(unsigned i=0;i<4;++i) {
        const auto path=disk.value.logs/(L"diagnostics.cpu-snapshot-"+std::to_wstring(i)+L".bin");
        std::ifstream file(path,std::ios::binary);std::ostringstream bytes;bytes<<file.rdbuf();const auto raw=bytes.str();
        CHECK(raw.size()==c->file_bytes&&raw.size()<=C::file_cap&&!std::memcmp(raw.data(),c->file.data(),raw.size()));
    }
    CHECK(!fs::exists(disk.value.logs/L"diagnostics.cpu-snapshot-4.bin"));
}
}
