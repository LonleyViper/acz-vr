// Run the existing fixture unchanged, plus independent storage regressions.
#define main j138_existing_main
#pragma warning(push)
#pragma warning(disable:4715) // renamed, unused original main has implicit return
#include "diagnostics_offline_tests.cpp"
#pragma warning(pop)
#undef main
static_assert(sizeof(acz::CopyNativeCall)<=32,"Observation wrapper automatic storage stays scalar");
static_assert(sizeof(acz::gpu_copy::ObservationFeed)<=256,"Observation state stays off stack/Draw");

struct NestedFlushIO : MemoryIO {
    Log* log=nullptr;
    size_t append(const fs::path& path,const char* p,size_t n) override {
        if(log) {
            log->flush(); // same-thread recursive consumer
            std::thread other([&]{log->flush();});other.join();
        }
        return MemoryIO::append(path,p,n);
    }
};

static void flushStorage(TestPaths& paths) {
    constexpr const char* names[]={"proxy_load","device","hook_pin","hook_patch","pso","root",
        "descriptor","capture_decision","metadata_disabled","execute_summary","summary","config",
        "missing_present","requirements_metadata"};
    NestedFlushIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    tick=0;Log log(paths,io,clockNow,true);io.log=&log;log.flush();
    CHECK(io.files[0].empty());
    for(unsigned round=0;round<2;++round) {
        CHECK(log.push(record(Event::descriptor)));
        tick=round*summary_min_us;log.flush();
        std::string expected;
        for(unsigned batch=0;batch<=round;++batch)for(unsigned i=0;i<14;++i) {
            const unsigned count=i==6?batch+1:i==7||i==11?1:i==10?14*(batch+1):0;
            expected+=std::to_string(3+batch*14+i)+"|summary|a="+std::to_string(i)+"|b="+
                std::to_string(count)+"|c=0|identity=unknown|hash=|sha256=|text="+names[i]+"\n";
        }
        CHECK(io.files[0]==expected); // full bytes, including sequences and order
    }
}

static void nestedResetStorage() {
    using namespace acz;
    auto& s=state();
    auto* key=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(0x138));
    unsigned releases=0;
    {
        std::lock_guard lock(s.mutex);auto& list=s.states[key];
        list.capture_hooks=true;list.creation_observed=true;
        list.constantWitness.words[0].written=true;
        list.tableWitness.written=true;list.samplerWitness.written=true;
        list.pso=std::shared_ptr<ID3D12PipelineState>(nullptr,[&](ID3D12PipelineState*) {
            CHECK(!mutex_depth);++releases;acz::resetBindings(key,true);
        });
        list.root=std::shared_ptr<ID3D12RootSignature>(nullptr,[&](ID3D12RootSignature*) {
            CHECK(!mutex_depth);++releases;acz::resetBindings(key,true);
        });
    }
    acz::resetBindings(key,true);
    CHECK(releases==2);
    {
        std::lock_guard lock(s.mutex);const auto& list=s.states.at(key);
        CHECK(list.capture_hooks&&list.creation_observed&&list.reset_observed);
        CHECK(!list.constantWitness.words[0].written&&!list.tableWitness.written&&!list.samplerWitness.written);
        CHECK(list.bindings.tracking_complete);
    }
    // A detached retired node can coexist with nested use of the same key.
    ListStates::node_type retired;
    {std::lock_guard lock(s.mutex);retired=s.states.extract(key);}
    acz::resetBindings(key,true);
    CHECK(!retired.empty());
    {std::lock_guard lock(s.mutex);s.states.erase(key);}
}

static void constantStorage() {
    using namespace acz;
    capture::State s;s.root_id=33;s.root_identity=44;s.revision=5;
    metadata::RootLayout layout;metadata::RootParameter p;
    p.kind=metadata::ParameterKind::constants;p.shader_register=10;p.constant_count=2;
    layout.parameters.push_back(p);s.root=metadata::RootBindingMetadata::from_adapter(layout);
    CHECK(s.root->valid());
    diagnostics::RootConstantWitness::Snapshot w;
    w.reason=diagnostics::RootConstantWitness::covered;w.size=2;w.stages=3;w.mask=3;
    w.revision=1;w.before=1;w.after=2;w.thread=7;
    w.list=s.list_identity;w.generation=s.generation;w.binding_revision=s.revision;
    w.root=s.root_id;w.token=s.root_identity;w.values[0]=0x80000000;w.values[1]=0xffffffff;
    requirements::MetadataResolver r;r.constants=&w;r.recording_thread=7;
    r.vs_cb10_words=shader_access::e11_vs_cb10_words;r.ps_cb10_words=shader_access::e11_ps_cb10_words;
    std::array<draw_provenance::Input,2> values{};
    CHECK(requirements::covered_constants(s,r,values));
    for(const auto& value:values) {
        CHECK(value.word_count==2&&value.bytes==8&&value.root_id==33&&value.root_lifetime==44);
        CHECK(value.words[0]==0x80000000&&value.words[1]==0xffffffff);
    }
    values[0].offset=1;CHECK(!requirements::covered_constants(s,r,values));
    values[0].offset=0;values[1].descriptor=draw_provenance::DescriptorKind::Dynamic;
    CHECK(!requirements::covered_constants(s,r,values));
}

static void metadataStorage(TestPaths& paths) {
    using namespace acz;
    capture::State bindings;capture::Command cmd{true,3,1,0,0,0};
    requirements::MetadataResolver resolver;
    std::string baseline;
    for(unsigned pass=0;pass<2;++pass) {
        MemoryIO io;Log log(paths,io,clockNow);
        {
            auto& s=state();std::lock_guard lock(s.mutex);
            requirements::MetadataResult local;
            auto& scratch=s.requirementsScratch;
            scratch.result.cb10_kind=2;scratch.result.proved=true;
            scratch.result.root_constants[0].words[0]=0xffffffff;
            scratch.~RequirementsScratch();new (&scratch) HookState::RequirementsScratch{};
            auto& result=pass?scratch.result:local;
            requirements::check_metadata(bindings,cmd,resolver,result);
            CHECK(!result.proved&&result.root_constants[0].words[0]==0);
            log.metadata_outcome(result,cmd);
        }
        log.flush();
        const std::string expected="1|requirements_metadata|a=0|b=1|c=4294967295|identity=unknown|hash=|sha256=|text="
            "rejected:unknown/stale list generation or incomplete observation;cb10=4294967295;draw=1,3,1,0,0,0;lp=3\n";
        CHECK(io.files[0].compare(0,expected.size(),expected)==0);
        if(!pass)baseline=io.files[0];else CHECK(io.files[0]==baseline);
    }
}

int main(int argc,char** argv) {
    auto base=fs::current_path()/L"j138-storage-fixture";
    TestPaths paths(base);fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    flushStorage(paths);nestedResetStorage();constantStorage();metadataStorage(paths);fs::remove_all(base);
    std::printf("%zu stack storage checks passed; CPU/fake COM only\n",checks.load());
    return 0;
}
