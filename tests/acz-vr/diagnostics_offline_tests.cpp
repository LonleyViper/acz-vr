// Production hook source with fake COM/loader seams only. Never loads a DLL.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cwctype>
#include "../../src/acz-vr/capture_enablement.h"

static std::atomic<size_t> checks{0};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
// Bounded phase evidence survives a native exception before CHECK can report.
// Keep this outside the injected allocator and flush before entering a fixture:
// a stack-probe fault can happen before the callee executes its first statement.
static void fixtureCheckpoint(const char* phase) {
    std::fprintf(stderr,"diagnostics fixture: %s (checks=%zu)\n",phase,checks.load());
    std::fflush(stderr);
}
// Consume injected failures only on the thread that arms them.
static thread_local bool failNew=false;
static thread_local unsigned failedNew=0;
void* operator new(size_t n) {
    if(failNew){failNew=false;++failedNew;throw std::bad_alloc();}
    if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using namespace acz::diagnostics;
namespace fs=std::filesystem;
static uint64_t tick=0;
static uint64_t clockNow() noexcept {return tick;}
static bool fixtureWorkingDirectory(const fs::path& current) {
    std::array<wchar_t,32768> module{};
    const DWORD length=GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()));
    if(!length||length>=module.size())return false;
    std::error_code ec;
    return fs::equivalent(current,fs::path(module.data()).parent_path(),ec)&&!ec;
}
struct TestPaths : PathProvider {
    Paths value;bool allocate=false;
    explicit TestPaths(const fs::path& base):value{base/L"logs",base/L"game",base/L"runtime.cfg"}{}
    Paths paths() override {
        if(allocate){auto* p=new char[8192];delete[] p;}
        return value;
    }
};
// All failure modes cross the same public virtual I/O boundary as FileIO.
// No synthetic failure changes the native fake-COM result below.
struct MemoryIO : IOProvider {
    enum class Fault { none, directory, open, short_write, disk_full, rotate, size, allocation, reset } fault=Fault::none;
    std::array<std::string,max_rotated_files+1> files;
    std::string evidence;
    std::array<std::string,4> raw;
    static int rawIndex(const fs::path& p) {
        for(int i=0;i<4;++i)if(p.filename()==L"diagnostics.cpu-snapshot-"+std::to_wstring(i)+L".bin")return i;return -1;
    }
    ReadState readState=ReadState::missing;std::string config;
    size_t calls=0,rotations=0,maxRotation=0;
    uint64_t lastSequence=0;
    bool observeNextAppend=false;
    Log* observeDuringAppend=nullptr;
    size_t observedSize=0,observedLength=0,observedRotations=0;
    bool create_directory(const fs::path&) override {++calls;return fault!=Fault::directory;}
    bool size(const fs::path& p,uint64_t& n) override {
        ++calls;if(fault==Fault::size)return false;
        if(p.filename()==L"diagnostics.evidence.log"){n=evidence.size();return true;}
        if(const auto i=rawIndex(p);i>=0){n=raw[size_t(i)].size();return true;}
        auto ext=p.extension().wstring();size_t i=ext==L".1"?1:ext==L".2"?2:0;n=files[i].size();return true;
    }
    bool reset(const fs::path& p) override {++calls;if(const auto i=rawIndex(p);i>=0)raw[size_t(i)].clear();else evidence.clear();return fault!=Fault::open&&fault!=Fault::reset;}
    size_t overwrite(const fs::path&,uint64_t off,const char* p,size_t n) override {
        ++calls;CHECK(!mutex_depth);CHECK(n==max_record_length&&off<=evidence.size()&&n<=evidence.size()-off);
        if(fault==Fault::open||fault==Fault::disk_full)return 0;
        if(fault==Fault::allocation)throw std::bad_alloc();
        auto written=fault==Fault::short_write?n-1:n;evidence.replace(size_t(off),written,p,written);return written;
    }
    size_t append(const fs::path& path,const char* p,size_t n) override {
        ++calls;
        if(observeDuringAppend){auto* log=observeDuringAppend;observeDuringAppend=nullptr;log->push(record(Event::descriptor));}
        if(observeNextAppend){observeNextAppend=false;observedSize=files[0].size();observedLength=n;observedRotations=rotations;}
        if(fault==Fault::allocation){failNew=true;auto* q=new char[8192];delete[] q;}
        if(fault==Fault::open||fault==Fault::disk_full)return 0;
        const auto rawSlot=rawIndex(path);
        auto& target=rawSlot>=0?raw[size_t(rawSlot)]:path.filename()==L"diagnostics.evidence.log"?evidence:files[0];
        if(fault==Fault::short_write){target.append(p,n-1);return n-1;}
        lastSequence=std::strtoull(p,nullptr,10);
        target.append(p,n);return n;
    }
    bool rotate(const fs::path&,size_t count) override {
        ++calls;++rotations;maxRotation=std::max(count,maxRotation);
        if(fault==Fault::rotate)return false;
        CHECK(count==max_rotated_files);
        for(size_t i=count;i>0;--i)files[i]=std::move(files[i-1]);files[0].clear();return true;
    }
    ReadResult read_config(const fs::path&,char* p,size_t cap) override {
        ++calls;if(readState!=ReadState::ok)return {readState,0};
        if(config.size()>cap)return {ReadState::oversized,0};
        std::copy(config.begin(),config.end(),p);return {ReadState::ok,config.size()};
    }
};
static std::string rendered(const Record& r) {
    auto f=format_record(r);CHECK(f.valid);return {f.bytes.data(),f.size};
}
static void formatting() {
    auto r=record(Event::device,2,3,4,"A\nB\rC|D\tE");r.sequence=7;
    CHECK(rendered(r)=="7|device|a=2|b=3|c=4|identity=unknown|hash=|sha256=|text=A_B_C_D_E\n");
    // Caller-written arrays need not be terminated; formatter bounds all reads.
    r.text.fill('x');r.shader.hash.fill('h');r.shader.container_prefix.fill('s');
    auto f=format_record(r);CHECK(f.valid&&f.size<=max_record_length);
    CHECK(f.bytes[f.size-1]=='\n');
    r.a=r.b=r.c=UINT64_MAX;r.sequence=UINT64_MAX;
    r.shader.identity=Identity::hash_only_no_container_match;
    CHECK(!format_record(r).valid);
    r=record(Event::summary);r.sequence=1;
    r.shader.identity=Identity::hash_only_no_container_match;
    r.a=r.b=r.c=UINT64_MAX;
    // Build exact max and max+1 schema lengths without changing any hard cap.
    auto base=rendered(r).size();CHECK(max_record_length-base+1<text_capacity);
    const auto textSize=max_record_length-base;
    std::fill_n(r.text.begin(),textSize,'x');CHECK(format_record(r).valid);
    CHECK(format_record(r).size==max_record_length);
    r.text[textSize]='x';CHECK(!format_record(r).valid);
    r.kind=Event::count;CHECK(!format_record(r).valid);
}
static void configTests(TestPaths& paths) {
    struct Case {const char* bytes;ConfigIssue issue;bool logging;unsigned verbosity;};
    const Case cases[]={
        {"",ConfigIssue::none,true,1},{"log_enabled=false\nverbosity=0\ncapture=disabled",ConfigIssue::none,false,0},
        {"log_enabled=true\nverbosity=2",ConfigIssue::none,true,2},
        {"bad",ConfigIssue::malformed,true,1},{"verbosity=0\nverbosity=1",ConfigIssue::duplicate,true,1},
        {"log_enabled=false\nunknown=1",ConfigIssue::unknown_key,true,1},
        {"verbosity=3",ConfigIssue::out_of_range,true,1},{"log_enabled=bad",ConfigIssue::out_of_range,true,1},
        {"capture=true",ConfigIssue::capture_enable_rejected,true,1},
        {"capture=1",ConfigIssue::capture_enable_rejected,true,1},
        {"enable_capture=true",ConfigIssue::capture_enable_rejected,true,1},
        {"gpu_copy=true",ConfigIssue::capture_enable_rejected,true,1},
        {"xr_submission=true",ConfigIssue::capture_enable_rejected,true,1},
        {"log_enabled=false\ncapture=enabled",ConfigIssue::capture_enable_rejected,true,1},
        {"a=b\na=c",ConfigIssue::duplicate,true,1},{"=1",ConfigIssue::malformed,true,1},
        {"verbosity=1=2",ConfigIssue::malformed,true,1}};
    for(auto& c:cases) {
        auto r=parse_config(c.bytes);CHECK(r.issue==c.issue);CHECK(!r.config.capture_enabled());
        CHECK(r.config.logging()==c.logging&&r.config.verbosity()==c.verbosity);
    }
    CHECK(!Config{}.ownership_pins());
    CHECK(parse_config("diagnostic_ownership_pins=true").config.ownership_pins());
    CHECK(!parse_config("diagnostic_ownership_pins=false").config.ownership_pins());
    CHECK(!parse_config("diagnostic_ownership_pins=true\nunknown=1").config.ownership_pins());
    CHECK(parse_config("diagnostic_ownership_pins=perhaps").issue==ConfigIssue::out_of_range);
    CHECK(!parse_config("diagnostic_ownership_pins=true\ncapture=true").config.ownership_pins());
    CHECK(!parse_config("diagnostic_ownership_pins=true").config.capture_enabled());
    CHECK(parse_config("unknown=1").unknown_keys==1);
    for(auto key:{"max_records=65537","max_file_size=524289","max_rotated_files=3","pso_records=4097"})
        CHECK(parse_config(key).issue==ConfigIssue::unknown_key);
    CHECK(parse_config("capture=true").reason==std::string_view("config capture enable rejected"));
    std::string exact(max_config_bytes,'\n');CHECK(parse_config(exact).issue==ConfigIssue::none);
    exact+='\n';CHECK(parse_config(exact).issue==ConfigIssue::oversized);
    exact="x="+std::string(max_config_line-2,'x');CHECK(parse_config(exact).issue==ConfigIssue::unknown_key);
    exact+='x';CHECK(parse_config(exact).issue==ConfigIssue::oversized);
    std::string keys;for(size_t i=0;i<max_config_keys;++i)keys+="k"+std::to_string(i)+"=1\n";
    CHECK(parse_config(keys).unknown_keys==max_config_keys);keys+="extra=1";
    CHECK(parse_config(keys).issue==ConfigIssue::oversized);
    CHECK(parse_config(std::string_view("a=\0",3)).issue==ConfigIssue::malformed);
    MemoryIO io;
    for(auto state:{ReadState::missing,ReadState::unreadable,ReadState::oversized}) {
        io.readState=state;auto r=load_config(io,paths.value.config);
        CHECK(r.issue!=ConfigIssue::none&&r.config.logging()&&!r.config.capture_enabled());
    }
    io.readState=ReadState::ok;io.config="capture=true";
    Log log(paths,io,clockNow);log.flush();CHECK(log.counters().config_failures==1);
    CHECK(io.files[0].find("config capture enable rejected")!=std::string::npos);
    MemoryIO unknown;unknown.readState=ReadState::ok;unknown.config="x=1\ny=2";
    Log u(paths,unknown);u.flush();CHECK(u.counters().unknown_keys==2);
    MemoryIO disabled;disabled.readState=ReadState::ok;disabled.config="log_enabled=false";
    Log d(paths,disabled);d.push(record(Event::device));d.flush();CHECK(disabled.files[0].empty());
}
static void caps(TestPaths& paths) {
    MemoryIO io;Log log(paths,io,clockNow);
    for(size_t i=0;i<ring_capacity;++i)CHECK(log.push(record(Event::device,i)));
    CHECK(!log.push(record(Event::device)));CHECK(log.counters().dropped==1);
    CHECK(io.calls==0);log.flush();CHECK(log.counters().written>0);
    // Log lifetime record budget includes summary/config records.
    while(log.counters().written<max_records) {
        for(size_t i=0;i<ring_capacity;++i)log.push(record(Event::device));log.flush();
    }
    CHECK(log.counters().written==max_records);
    auto before=log.counters().dropped;log.push(record(Event::device));log.flush();
    CHECK(log.counters().written==max_records&&log.counters().dropped>before);
    CHECK(io.maxRotation==max_rotated_files&&io.rotations>max_rotated_files);
    for(auto& file:io.files)CHECK(file.size()<=max_file_size);
    // At exactly the file cap, an incoming record rotates before appending.
    MemoryIO exact;exact.files[0].assign(max_file_size,'x');Log e(paths,exact);e.push(record(Event::device));e.flush();
    CHECK(e.counters().io_failures==0&&exact.rotations>0&&exact.files[1].size()==max_file_size);
    MemoryIO oversized;oversized.files[0].assign(max_file_size+1,'x');Log o(paths,oversized);o.flush();CHECK(o.counters().io_failures==1);
    // Exactly fitting a record does not rotate; max+1 byte would rotate.
    MemoryIO fit;Log f(paths,fit);f.flush();
    auto r=record(Event::device);r.sequence=fit.lastSequence+1;const auto n=rendered(r).size();
    fit.files[0].assign(max_file_size-n,'x');auto rotations=fit.rotations;
    fit.observeNextAppend=true;
    CHECK(f.push(record(Event::device)));f.flush();
    CHECK(fit.observedSize+fit.observedLength==max_file_size&&fit.observedRotations==rotations);
    r.sequence=fit.lastSequence+1;const auto nextLength=rendered(r).size();
    fit.files[0].assign(max_file_size-nextLength+1,'x');rotations=fit.rotations;
    fit.observeNextAppend=true;CHECK(f.push(record(Event::device)));f.flush();
    CHECK(fit.observedSize==0&&fit.observedLength==nextLength&&fit.observedRotations==rotations+1);
    // Oversized records increment drops before touching ring or I/O.
    Record longRecord=record(Event::summary,UINT64_MAX,UINT64_MAX,UINT64_MAX);
    longRecord.text.fill('x');longRecord.shader.hash.fill('h');longRecord.shader.container_prefix.fill('s');
    longRecord.shader.identity=Identity::hash_only_no_container_match;
    before=f.counters().dropped;CHECK(!f.push(longRecord));CHECK(f.counters().dropped==before+1);
    // Concurrent producers finish without a consumer; once full every extra
    // attempt drops rather than waits for a flush. No elapsed-time assertion.
    MemoryIO concurrent;Log concurrentLog(paths,concurrent);
    std::array<std::thread,4> workers;
    for(auto& thread:workers)thread=std::thread([&]{for(size_t i=0;i<500;++i)concurrentLog.push(record(Event::device));});
    for(auto& thread:workers)thread.join();
    CHECK(concurrentLog.counters().dropped>=2000-ring_capacity&&concurrent.calls==0);
}
static void intervals(TestPaths& paths) {
    IntervalCounter c;c.observe(100);c.observe(600);c.observe(2600);c.observe(22600);c.observe(222600);
    auto s=c.summary();CHECK(s.count==4&&s.min==500&&s.max==200000&&s.mean==55625);
    for(auto n:s.buckets)CHECK(n==1);
    c.observe(1);CHECK(c.summary().count==4);
    c.observe(UINT64_MAX);CHECK(c.summary().max==max_interval_us);
    IntervalCounter bounded;for(uint64_t i=0;i<=max_interval_samples+1;++i)bounded.observe(i);
    CHECK(bounded.summary().count==max_interval_samples&&bounded.summary().mean==1);
    MemoryIO io;Log log(paths,io,clockNow);tick=0;log.execute_interval();tick=42;log.execute_interval();
    log.flush();CHECK(io.files[0].find("execute_summary|a=1|b=42|c=42")!=std::string::npos);
    CHECK(io.files[0].find("Present seam absent")!=std::string::npos);
}
static size_t linesWith(const std::string& text,const std::string& marker) {
    size_t off=0,count=0;while((off=text.find(marker,off))!=text.npos){++count;off+=marker.size();}return count;
}
static void srvSlotCounterPersistence(TestPaths& paths);
static void companionPersistence(TestPaths& paths) {
    srvSlotCounterPersistence(paths);
    constexpr const char* kinds[]={"root-constant-witness","root-table-witness",
        "t0-resource-description","t0-write-witness","s0-table-witness","s0-description","l3-t10","l3-vb","l3-ib","l3-state","l3-link","cpu-t10","cpu-vb","cpu-ib","cpu-snapshot"};
    static_assert(std::size(kinds)==companion_kind_count-3&&companion_sample_limit==64);
    static_assert(max_records==65536&&startup_records==256&&outcome_capacity==32&&ring_capacity==64);
    static_assert(reserved_records==5146&&max_disk_bytes==4546432);
    // Heap Log: do not add its ring/batch arrays to this fixture's stack frame.
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    Mutex mutex;
    acz::requirements::MetadataResult refusal;
    refusal.outcome="rejected:t0 missing/stale/unsupported descriptor";refusal.cb10_kind=1;
    {std::lock_guard lock(mutex);metadata_outcome(refusal,{true,9,1,0,0,0},2,";cn=t0;first=1");}
    // Regression reproducer: refusal, then each raw companion, real emit ->
    // production Log -> serialization -> IOProvider bytes (not format-only).
    const auto calls=io.calls;
    for(auto kind:kinds) {
        const auto text=std::string(kind)+";draw=1,9,1,0,0,0;sample=0";
        emit(record(Event::requirements_metadata,0,0,0,text.c_str()));
    }
    CHECK(io.calls==calls);log->flush();
    CHECK(linesWith(io.evidence,"|requirements_metadata|")==companion_kind_count-2);
    for(auto kind:kinds)CHECK(linesWith(io.evidence,std::string("text=")+kind+";")==1);
    CHECK(linesWith(io.evidence,";cn=t0;first=1")==1);
    for(size_t sample=1;sample<companion_sample_limit+3;++sample) {
        for(auto kind:kinds) {
            const auto text=std::string(kind)+";draw=1,9,1,0,0,0;sample="+std::to_string(sample);
            emit(record(Event::requirements_metadata,0,0,0,text.c_str()));
        }
        log->flush(); // No ring/rotation pressure obscures the per-kind cap.
    }
    {std::lock_guard lock(mutex);metadata_outcome(refusal,{true,99,1,0,0,0},0,";changed=1");}
    log->flush();
    CHECK(linesWith(io.evidence,"|requirements_metadata|")==companion_kind_count-2);
    CHECK(io.evidence.find(";changed=1")==io.evidence.npos);
    CHECK(io.evidence.find("|b=2|c=1|")!=io.evidence.npos);
    auto all=io.evidence;for(const auto& file:io.files)all+=file;
    for(auto kind:kinds) {
        CHECK(linesWith(all,std::string("text=")+kind+";")==companion_sample_limit);
        CHECK(linesWith(io.evidence,std::string("text=")+kind+";draw=1,9,1,0,0,0;sample=0")==1);
    }
    CHECK(log->dropped(Event::requirements_metadata)==3*(companion_kind_count-3));
    CHECK(!log->overflow(Event::requirements_metadata)&&!log->counters().io_failures);
    // Exhaust the total ordinary budget and rotate: first examples survive.
    for(size_t i=0;i<max_records/ring_capacity+2;++i) {
        for(size_t j=0;j<ring_capacity;++j)log->push(record(Event::summary,0,0,0,"ordinary pressure"));
        log->flush();
    }
    CHECK(log->counters().written<=max_records&&io.evidence.size()<=max_evidence_size);
    size_t bytes=io.evidence.size();for(const auto& file:io.files){CHECK(file.size()<=max_file_size);bytes+=file.size();}
    CHECK(bytes<=max_disk_bytes);
    for(auto kind:kinds)CHECK(linesWith(io.evidence,std::string("text=")+kind+";")==1);
    set_sink(nullptr);log.reset();
    for(auto config:{"log_enabled=true","log_enabled=true\ndiagnostic_ownership_pins=false",
                     "log_enabled=false\ndiagnostic_ownership_pins=true"}) {
        MemoryIO off;off.readState=ReadState::ok;off.config=config;
        auto disabled=std::make_unique<Log>(paths,off,clockNow,true);disabled->flush();set_sink(disabled.get());
        for(auto kind:kinds) {
            const auto text=std::string(kind)+";draw=1,9,1,0,0,0";
            emit(record(Event::requirements_metadata,0,0,0,text.c_str()));
        }
        disabled->flush();set_sink(nullptr);
        auto output=off.evidence;for(const auto& file:off.files)output+=file;
        for(auto kind:kinds)CHECK(output.find(kind)==output.npos);
    }
}
static void companionPressure(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    for(size_t i=0;i<ring_capacity;++i)CHECK(log->push(record(Event::summary)));
    auto r=record(Event::requirements_metadata,0,0,0,"root-table-witness;sample=first");
    const auto calls=io.calls;failNew=true;
    emit(r); // First example uses the existing startup reservation, no allocation.
    CHECK(failNew&&io.calls==calls);failNew=false;
    emit(record(Event::requirements_metadata,0,0,0,"root-table-witness;sample=ring-full"));
    CHECK(log->dropped(Event::requirements_metadata)==1);log->flush();
    CHECK(linesWith(io.evidence,"text=root-table-witness;sample=first")==1);
    CHECK(io.files[0].find("sample=ring-full")==io.files[0].npos);
    emit(record(Event::requirements_metadata,0,0,0,"root-table-witness;sample=retry"));log->flush();
    CHECK(linesWith(io.files[0],"text=root-table-witness;sample=retry")==1);
    // Two activation records plus one first companion already own startup slots.
    for(size_t i=3;i<startup_records;++i)CHECK(log->push(record(Event::device)));
    emit(record(Event::requirements_metadata,0,0,0,"t0-write-witness;sample=startup-full"));
    log->flush();set_sink(nullptr);
    CHECK(log->dropped(Event::requirements_metadata)==2);
    CHECK(io.evidence.find("sample=startup-full")==io.evidence.npos);
    CHECK(io.evidence.size()<=max_evidence_size&&!log->counters().io_failures);
}
// Dedicated latest snapshot survives ordinary saturation, sample cap and rotation.
static void srvSlotCounterPersistence(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    for(unsigned i=0;i<100;++i) {
        for(size_t j=0;j<ring_capacity;++j)emit(record(Event::summary,0,0,0,"pressure"));
        char text[256]{};std::snprintf(text,sizeof text,
            "srv-slot-counts;held=%x;replaced=%x;released=%x;promoted=%x;cap=%x;max=%x;distinct=%x",i,i,i,i,i,i,i);
        emit(record(Event::requirements_metadata,0,0,0,text));
        log->flush();
    }
    CHECK(linesWith(io.evidence,"text=srv-slot-counts;")==1);
    CHECK(io.evidence.find("srv-slot-counts;held=63;replaced=63;released=63;promoted=63;cap=63;max=63;distinct=63")!=io.evidence.npos);
    CHECK(!log->counters().io_failures);
    set_sink(nullptr);
}
static void companionFilePersistence(const fs::path& base) {
    TestPaths paths(base/L"companion-real-files");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.logs/L"diagnostics.log",paths.value.logs/L"diagnostics.log.1",
                         paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    {std::ofstream config(paths.value.config);config<<"log_enabled=true\ndiagnostic_ownership_pins=true\n";}
    FileIO io;auto log=std::make_unique<Log>(paths,io,clockNow,true);log->flush();set_sink(log.get());
    {Mutex mutex;std::lock_guard lock(mutex);acz::requirements::MetadataResult refusal;
     refusal.outcome="rejected:t0 missing/stale/unsupported descriptor";
     metadata_outcome(refusal,{true,9,1,0,0,0});}
    constexpr const char* kinds[]={"root-constant-witness;","root-table-witness;",
        "t0-resource-description;","t0-write-witness;","s0-table-witness;","s0-description;","l3-t10;","l3-vb;","l3-ib;","l3-state;","l3-link;","cpu-t10;","cpu-vb;","cpu-ib;","cpu-snapshot;","srv-slot-counts;held=3;replaced=2;released=2;promoted=1;cap=0;max=90;distinct=91"};
    for(auto kind:kinds)emit(record(Event::requirements_metadata,0,0,0,kind));
    log->flush();set_sink(nullptr);CHECK(!log->counters().io_failures);
    std::ifstream file(paths.value.logs/L"diagnostics.evidence.log",std::ios::binary);
    std::ostringstream bytes;bytes<<file.rdbuf();const auto persisted=bytes.str();
    CHECK(linesWith(persisted,"|requirements_metadata|")==companion_kind_count-1);
    for(auto kind:kinds)CHECK(linesWith(persisted,std::string("text=")+kind)==1);
}
static void reasonSnapshots(TestPaths& paths) {
    static const char* reasons[]={
        "binding ordering identity exhausted",
        "overlapping binding observations",
        "unknown binding footprint",
        "unresolved overlapping binding footprint",
        "native observation exception",
        "binding interface alias unsupported",
        "heap creation missing description",
        "heap creation IID unsupported",
        "heap creation QI failed",
        "heap creation identity mismatch",
        "heap lifetime hook incomplete",
        "heap creation observation exception",
        "resource creation missing description",
        "resource creation IID unsupported",
        "resource creation layout unsupported",
        "resource creation QI failed",
        "resource creation identity mismatch",
        "resource lifetime hook incomplete",
        "resource creation observation exception",
        "placed buffer unsupported",
        "reserved buffer unsupported",
        "binding device interface unsupported: IUnknown",
        "binding device interface unsupported: ID3D12Object",
        "binding device interface unsupported: ID3D12Device",
        "binding device interface unsupported: ID3D12Device1",
        "binding device interface unsupported: ID3D12Device2",
        "binding device interface unsupported: ID3D12Device3",
        "binding device interface unsupported: ID3D12Device4",
        "binding device interface unsupported: ID3D12Device5",
        "binding device interface unsupported: ID3D12Device6",
        "binding device interface unsupported: ID3D12Device7",
        "binding device interface unsupported: ID3D12Device8",
        "binding device interface unsupported: ID3D12Device9",
        "binding device interface unsupported: ID3D12Device10",
        "binding device interface unsupported: ID3D12Device11",
        "binding device interface unsupported: ID3D12Device12",
        "binding device interface unsupported: ID3D12Device13",
        "binding device interface unsupported: ID3D12Device14",
        "binding device interface unsupported: unknown",
        "binding device final release",
        "capture device hook incomplete",
        "binding device hook incomplete",
        "native table resolution exception",
        "native root resolution exception",
        "native currentness exception",
        "binding footprint allocation failed",
        "binding footprint capacity exhausted",
        "native binding observer disabled",
        "heap registration rejected",
        "resource registration rejected"
    };
    CHECK(std::size(reasons)<reason_capacity);
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    tick=0;Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
    // Exercise the hook's actual serialized disable producer, not generic push.
    auto& state=acz::state();
    const auto calls=io.calls;failNew=true;
    for(auto reason:reasons){std::lock_guard lock(state.mutex);acz::BindingEvent::disable(reason,true);}
    CHECK(failNew&&io.calls==calls);failNew=false;log.flush();
    auto parse=[&](const char* reason) {
        auto pos=io.evidence.find(std::string("|text=")+reason+" ");CHECK(pos!=io.evidence.npos);
        auto begin=io.evidence.rfind('\n',pos);begin=begin==io.evidence.npos?0:begin+1;
        std::array<unsigned long long,4> out{};
        CHECK(std::sscanf(io.evidence.c_str()+begin,"%llu|metadata_disabled|a=%llu|b=%llu|c=%llu",&out[0],&out[1],&out[2],&out[3])==4);
        return out;
    };
    std::array<uint64_t,reason_capacity> first{};
    for(size_t i=0;i<std::size(reasons);++i){auto v=parse(reasons[i]);first[i]=v[0];CHECK(v[0]==v[1]&&v[2]==1);}
    const auto sequence_gap=log.observed(Event::summary);
    const auto bytes=io.evidence.size();failNew=true;
    for(unsigned repeat=0;repeat<1000;++repeat)for(auto reason:reasons) {
        std::lock_guard lock(state.mutex);acz::BindingEvent::disable(reason,true);
    }
    CHECK(failNew);failNew=false;log.flush();
    CHECK(io.evidence.size()==bytes); // Updates overwrite; never duplicate lines.
    for(size_t i=0;i<std::size(reasons);++i) {
        auto v=parse(reasons[i]);CHECK(v[0]==first[i]&&v[1]==first[i]+1000*std::size(reasons)+sequence_gap&&v[2]==1001);
        CHECK(linesWith(io.evidence,std::string("|text=")+reasons[i]+" ")==1);
    }
    // Equal static text at a different address is the same key.
    static const char duplicate[]="heap registration rejected";
    {std::lock_guard lock(state.mutex);observer_disabled(duplicate,DisableEvent::creation);}
    log.flush();CHECK(parse(duplicate)[2]==1002&&parse(duplicate)[3]==static_cast<unsigned>(DisableEvent::creation)&&io.evidence.size()==bytes);
    // Static storage for overflow exercises; fill every remaining slot.
    static std::array<std::array<char,64>,reason_capacity+3> extra{};
    for(size_t i=0;i<extra.size();++i)std::snprintf(extra[i].data(),extra[i].size(),"synthetic reason %zu",i);
    failNew=true;
    for(auto& reason:extra){std::lock_guard lock(state.mutex);observer_disabled(reason.data(),DisableEvent::descriptor_copy);}
    CHECK(failNew);failNew=false;log.flush();
    const auto overflow=std::size(reasons)+3;
    CHECK(log.overflow(Event::metadata_disabled)==overflow);
    CHECK(parse("observer reason overflow")[2]==overflow);
    CHECK(parse(extra[0].data())[3]==static_cast<unsigned>(DisableEvent::descriptor_copy));
    CHECK(io.evidence.size()<=max_evidence_size);
    log.execute_interval();log.final_summary();log.flush();
    CHECK(linesWith(io.evidence,"text=Release checkpoint; not process teardown")==1);
    // Ordinary lifetime exhaustion cannot evict or consume reason/startup/
    // checkpoint slots. This is deliberate overload, separate from live shape.
    for(size_t w=0;w<max_records/ring_capacity+1;++w) {
        for(size_t i=0;i<ring_capacity;++i)log.push(record(Event::summary,w,i));
        log.flush();
    }
    CHECK(log.dropped(Event::summary)>0);
    CHECK(linesWith(io.evidence,"text=Release checkpoint; not process teardown")==1);
    CHECK(linesWith(io.evidence,"|config|")==1&&linesWith(io.evidence,"|capture_decision|")==1);
    for(auto reason:reasons)CHECK(linesWith(io.evidence,std::string("|text=")+reason+" ")==1);
    CHECK(log.counters().written<=max_records);
    // Concurrent publication: one serialized producer, consumer never waits
    // for its mutex and only persists a consistent generation snapshot.
    const auto before=parse(reasons[0]);std::atomic<bool> done{false};
    std::thread producer([&] {
        for(unsigned i=0;i<5000;++i){std::lock_guard lock(state.mutex);observer_disabled(reasons[0]);}
        done.store(true,std::memory_order_release);
    });
    while(!done.load(std::memory_order_acquire)) {
        log.flush();auto value=parse(reasons[0]);
        CHECK(value[0]==before[0]&&value[1]>=before[1]&&value[2]>=before[2]&&value[2]<=before[2]+5000);
    }
    producer.join();log.flush();CHECK(parse(reasons[0])[2]==before[2]+5000);
    // A replacement I/O failure only disables optional logging.
    io.fault=MemoryIO::Fault::short_write;
    {std::lock_guard lock(state.mutex);observer_disabled(reasons[0]);}
    log.flush();CHECK(!log.enabled()&&log.counters().io_failures==1);
    set_sink(nullptr);state.nativeBindings.~NativeBindingObserver();
    new (&state.nativeBindings) acz::binding::NativeBindingObserver;
}
static void overlapReasonMatrix(TestPaths& paths) {
    using K=acz::binding::ObservationKind;
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
    unsigned count=0;
    std::array<const char*,acz::binding::overlap_reason_count> seen{};
    const auto calls=io.calls;failNew=true;
    for(unsigned a=0;a<unsigned(K::count);++a)for(unsigned b=a;b<unsigned(K::count);++b)for(bool same:{false,true}) {
        auto reason=acz::binding::overlap_reason(K(a),K(b),same);
        CHECK(reason==acz::binding::overlap_reason(K(b),K(a),same));
        CHECK(std::strlen(reason)<96);
        for(unsigned i=0;i<count;++i)CHECK(std::strcmp(seen[i],reason)!=0);
        seen[count]=reason;
        {std::lock_guard lock(acz::state().mutex);observer_disabled(reason);observer_disabled(reason);}
        ++count;
    }
    CHECK(failNew&&io.calls==calls);failNew=false;log.flush();
    CHECK(count==182 && count==acz::binding::overlap_reason_count);
    // Known operation names are literals; only metadata_release adds a kind.
    // Existing device_release pair expectations remain unchanged.
    CHECK(std::strcmp(acz::binding::overlap_reason(K::committed_create,K::device_release,true),
        "overlap:committed_create/device_release:same")==0);
    CHECK(std::strcmp(acz::binding::overlap_reason(K::device_release,K::committed_create,false),
        "overlap:committed_create/device_release:cross")==0);
    CHECK(log.overflow(Event::metadata_disabled)==2*(count-reason_capacity));
    CHECK(linesWith(io.evidence,"text=overlap:descriptor_write/descriptor_write:cross ")==1);
    CHECK(linesWith(io.evidence,"text=overlap:descriptor_write/descriptor_write:same ")==1);
    // With 13 kinds the committed_create/device_release pair now falls past
    // the logger's 64 retained reasons; its exact spelling is checked above.
    // Check the newly named Release pair inside the retained prefix instead.
    CHECK(linesWith(io.evidence,"text=overlap:descriptor_write/metadata_release:same ")==1);
    CHECK(io.evidence.find("|b=2|c=")!=io.evidence.npos);
    CHECK(linesWith(io.evidence,"text=observer reason overflow ")==1);
    CHECK(io.evidence.size()<=max_evidence_size);set_sink(nullptr);
}
static void summaryCadence(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    tick=0;Log log(paths,io,clockNow,true);log.flush();
    auto written=log.counters().written;log.flush();CHECK(log.counters().written==written);
    log.push(record(Event::descriptor));log.flush();CHECK(log.counters().written>written);
    written=log.counters().written;log.push(record(Event::descriptor));log.flush();CHECK(log.counters().written==written);
    tick=summary_min_us;log.flush();CHECK(log.counters().written>written);
    written=log.counters().written;
    tick+=summary_min_us;log.flush();CHECK(log.counters().written==written); // Unchanged.
    tick=summary_min_us+summary_refresh_us;log.flush();CHECK(log.counters().written>written);
    written=log.counters().written;tick=0;log.flush();CHECK(log.counters().written==written);
    // A producer arriving during consumer I/O must remain a pending change.
    tick=summary_min_us+summary_refresh_us+summary_min_us;
    log.push(record(Event::descriptor));io.observeDuringAppend=&log;log.flush();
    const auto observed=log.observed(Event::descriptor);written=log.counters().written;
    tick+=summary_min_us;log.flush();CHECK(log.counters().written>written);
    CHECK(io.files[0].find("|summary|a=6|b="+std::to_string(observed)+"|c=0|")!=io.files[0].npos);
}
static void twoHourBudget(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    tick=0;Log log(paths,io,clockNow,true);log.flush();
    for(unsigned i=0;i<610;++i)CHECK(log.push(record(Event::pso,i)));
    for(unsigned i=0;i<28;++i)CHECK(log.push(record(Event::root,i)));
    for(unsigned i=0;i<234;++i)CHECK(log.push(record(Event::metadata_disabled)));
    for(size_t i=0;i<ring_capacity;++i)CHECK(log.push(record(Event::summary,i)));
    CHECK(!log.push(record(Event::summary))); // Known drop carried by every later aggregate.
    log.flush(); // Align the first aggregate with the injected session origin.
    for(unsigned w=1;w<=28800;++w) {
        for(unsigned i=0;i<74;++i)log.push(record(Event::descriptor));
        tick=uint64_t(w)*250000;
        for(unsigned i=0;i<9;++i)log.execute_interval();
        if(w==14400)log.final_summary();
        log.flush();
        CHECK(io.evidence.size()<=max_evidence_size);
        size_t bytes=io.evidence.size();for(auto& f:io.files){CHECK(f.size()<=max_file_size);bytes+=f.size();}
        CHECK(bytes<=max_disk_bytes);
    }
    log.final_summary();log.flush();
    CHECK(log.dropped(Event::summary)==1);
    CHECK(log.dropped(Event::execute_summary)==0);
    CHECK(log.counters().written<10000); // Large margin below the lifetime cap.
    CHECK(log.observed(Event::descriptor)==2131200);
    const auto& tail=io.files[0];
    for(size_t i=0;i<static_cast<size_t>(Event::count);++i) {
        const auto kind=static_cast<Event>(i);const auto total=log.observed(kind);
        if(!total)continue;
        const auto marker="|summary|a="+std::to_string(i)+"|b="+std::to_string(total)+"|c="+std::to_string(log.dropped(kind))+"|";
        CHECK(tail.find(marker)!=tail.npos);
    }
    std::printf("2h simulation: %llu lifetime records, descriptor=2131200, execute submissions=259200\n",
        static_cast<unsigned long long>(log.counters().written));
    CHECK(linesWith(io.evidence,"text=Release checkpoint; not process teardown")==1);
    CHECK(io.evidence.find("|summary|a=6|b=1065600|c=0|")!=io.evidence.npos);
    CHECK(io.evidence.find("|c=1|identity=unknown|hash=|sha256=|text=summary")!=io.evidence.npos);
    CHECK(linesWith(io.evidence,"|pso|")==610&&linesWith(io.evidence,"|root|")==28);
}
static void reservedCapacity(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    Log log(paths,io,clockNow,true);log.flush();
    const auto calls=io.calls;
    for(size_t i=0;i<ring_capacity;++i)CHECK(log.push(record(Event::summary,i)));
    CHECK(!log.push(record(Event::summary)));
    CHECK(log.dropped(Event::summary)==1&&log.observed(Event::summary)==ring_capacity+1);
    // Include a creation outcome and every shader stage exactly as emitted;
    // repetitions consume explicit reserved slots, never a dedup identity key.
    failNew=true; // Any accidental producer allocation would fail this burst.
    for(size_t i=0;i<pso_records;++i) {
        auto r=record(Event::pso,i,416,0,"reserved PSO");r.shader.identity=static_cast<Identity>(i%3);
        std::snprintf(r.shader.hash.data(),r.shader.hash.size(),"%032x",unsigned(i));
        CHECK(log.push(r));
    }
    for(size_t i=0;i<root_records;++i)CHECK(log.push(record(Event::root,i,0,0,"reserved root")));
    // Initial config and capture-decision records already own two startup slots.
    for(size_t i=2;i<startup_records;++i)CHECK(log.push(record(Event::device,i)));
    for(auto kind:{Event::pso,Event::root,Event::device}) {
        CHECK(!log.push(record(kind)));CHECK(log.overflow(kind)==1&&log.dropped(kind)==1);
    }
    CHECK(io.calls==calls&&failNew);failNew=false; // No producer allocation or I/O.
    for(size_t i=0;i<300000;++i)CHECK(log.push(record(Event::descriptor)));
    CHECK(log.observed(Event::descriptor)==300000&&log.dropped(Event::descriptor)==0);
    log.execute_interval();log.flush();log.final_summary();log.flush();log.final_summary();log.flush();
    CHECK(log.counters().io_failures==0);
    CHECK(linesWith(io.evidence,"text=reserved PSO")==pso_records);
    CHECK(linesWith(io.evidence,"text=reserved root")==root_records);
    CHECK(linesWith(io.evidence,"text=Release checkpoint; not process teardown")==1);
    CHECK(linesWith(io.evidence,"text=priority overflow")==3);
    CHECK(linesWith(io.evidence,"|descriptor|")==0);
    CHECK(io.evidence.find("|summary|a=6|b=300000|c=0|")!=io.evidence.npos);
    CHECK(io.evidence.size()<=max_evidence_size&&log.counters().written<=max_records);
    for(auto status:{"identity=unknown","identity=hash-only-no-container-match","identity=match"})
        CHECK(io.evidence.find(status)!=io.evidence.npos);
    // Four producers, ordinary ring full, no consumer: every reserved ticket
    // below the quota must succeed without taking the ordinary ring flag.
    MemoryIO concurrent;concurrent.readState=ReadState::ok;concurrent.config="log_enabled=true";
    Log parallel(paths,concurrent,clockNow,true);parallel.flush();
    for(size_t i=0;i<ring_capacity;++i)CHECK(parallel.push(record(Event::summary)));
    std::atomic<unsigned> accepted{0};std::array<std::thread,4> threads;
    for(auto& t:threads)t=std::thread([&]{for(size_t i=0;i<256;++i) {
        if(parallel.push(record(Event::pso,i)))++accepted;
        if(i<64&&parallel.push(record(Event::root,i)))++accepted;
    }});
    for(auto& t:threads)t.join();CHECK(accepted==1280&&parallel.counters().dropped==0);
    parallel.flush();CHECK(linesWith(concurrent.evidence,"|pso|")==1024&&linesWith(concurrent.evidence,"|root|")==256);
    // Each initialization resets prior invocation evidence within its own cap.
    Log restart(paths,concurrent,clockNow,true);restart.flush();
    CHECK(linesWith(concurrent.evidence,"|pso|")==0&&linesWith(concurrent.evidence,"|config|")==1);
    MemoryIO oversized;oversized.readState=ReadState::ok;oversized.config="log_enabled=true";
    oversized.evidence.assign(max_evidence_size+1,'x');Log bad(paths,oversized,clockNow,true);bad.flush();
    CHECK(bad.counters().io_failures==1);
    MemoryIO exact;exact.readState=ReadState::ok;exact.config="log_enabled=true";
    exact.evidence.assign(max_evidence_size,'x');Log good(paths,exact,clockNow,true);good.flush();
    CHECK(good.counters().io_failures==0&&exact.evidence.size()<max_evidence_size);
}
// A native fake export publishes a caller-owned output even on a failing
// HRESULT. The optional sink must preserve that unusual native contract too.
struct FakeCOM {void** table;ULONG refs=1;FakeCOM* owner=nullptr;FakeCOM* rootInterface=nullptr;FakeCOM* unknownInterface=nullptr;};
static FakeCOM* factory=nullptr;
static HRESULT nativeResult=S_FALSE;
static const GUID unknownIID={0xabcde,1,2,{3,4,5,6,7,8,9,10}};
static std::function<void()> deviceReleaseAction;
static ULONG STDMETHODCALLTYPE fakeRelease(IUnknown* p){
    CHECK(mutex_depth==0);
    if(deviceReleaseAction){auto action=std::move(deviceReleaseAction);deviceReleaseAction={};action();}
    auto* f=reinterpret_cast<FakeCOM*>(p);return --(f->owner?f->owner:f)->refs;
}
static HRESULT WINAPI nativeFactory(REFIID iid,void** out) {
    CHECK(iid==unknownIID);*out=factory;++factory->refs;return nativeResult;
}
static HMODULE fakeLoad(){return reinterpret_cast<HMODULE>(1);}
static void fakeInitialize(){emit(record(Event::proxy_load,1));}
static void forwarded(Log* log) {
    set_sink(log);
    for(auto hr:{S_FALSE,E_FAIL}) {
        nativeResult=hr;auto refs=factory->refs;void* out=nullptr;
        CHECK(ProxyCreateDXGIFactory(unknownIID,&out)==hr);CHECK(out==factory&&factory->refs==refs+1);
        CHECK(static_cast<IUnknown*>(out)->Release()==refs);
    }
    set_sink(nullptr);
}
static void failureTests(TestPaths& paths) {
    // Earlier fixtures also inject failures; count only this fixture's failures.
    const auto failedAtEntry=failedNew;
    std::array<void*,3> table{};table[2]=reinterpret_cast<void*>(&fakeRelease);FakeCOM obj{table.data()};factory=&obj;
    acz::g_loadTest=fakeLoad;acz::g_factoryTest=nativeFactory;acz::g_initializeTest=fakeInitialize;
    forwarded(nullptr);
    // U-soak: a failed rotation is transient, not a log failure (rotationIsTransient below).
    for(auto fault:{MemoryIO::Fault::directory,MemoryIO::Fault::open,MemoryIO::Fault::short_write,
        MemoryIO::Fault::disk_full,MemoryIO::Fault::size,MemoryIO::Fault::allocation}) {
        MemoryIO io;io.fault=fault;
        Log log(paths,io,clockNow);forwarded(&log);log.flush();CHECK(log.counters().io_failures==1);
        auto calls=io.calls;forwarded(&log);log.flush();CHECK(io.calls==calls&&log.counters().io_failures==1);
        CHECK(obj.refs==1);
    }
    CHECK(failedNew==failedAtEntry+1);
    for(auto suffix:{L"",L"/subdir"}) {
        TestPaths bad(paths.value.logs.parent_path());bad.value.logs=paths.value.game.wstring()+suffix;
        bad.value.game=paths.value.game;MemoryIO io;Log log(bad,io);forwarded(&log);log.flush();
        CHECK(log.counters().io_failures==1&&io.calls==0);
    }
    // Case, trailing slash and lexical parent traversal cannot bypass the
    // game-directory component check. Sibling game-other is allowed.
    for(auto logs:{paths.value.game/L"subdir",paths.value.game/L"subdir"/L"..",paths.value.game/L".."/L"game"}) {
        TestPaths bad(paths.value.logs.parent_path());bad.value.logs=logs;
        bad.value.game=paths.value.game.wstring()+L"/";MemoryIO io;Log log(bad,io);log.flush();
        CHECK(log.counters().io_failures==1&&io.calls==0);
    }
    {
        TestPaths bad(paths.value.logs.parent_path());bad.value.logs=paths.value.game/L"inside";
        auto upper=paths.value.game.wstring();for(auto& c:upper)c=wchar_t(std::towupper(c));bad.value.game=upper;
        MemoryIO io;Log log(bad,io);log.flush();CHECK(log.counters().io_failures==1&&io.calls==0);
    }
    {
        TestPaths bad(paths.value.logs.parent_path());bad.value.config=paths.value.game/L"runtime.cfg";
        bad.value.game=paths.value.game;MemoryIO io;Log log(bad,io);log.flush();CHECK(log.counters().io_failures==1&&io.calls==0);
    }
    {
        TestPaths sibling(paths.value.logs.parent_path());sibling.value.logs=paths.value.game.wstring()+L"-other";
        // Materialize this allowed path for weakly_canonical in the sandbox.
        fs::create_directories(sibling.value.logs);
        for(const auto* file:{L"diagnostics.log",L"diagnostics.log.1",L"diagnostics.log.2"})
            std::ofstream(sibling.value.logs/file).close();
        sibling.value.game=paths.value.game;MemoryIO io;Log log(sibling,io);log.flush();CHECK(log.counters().io_failures==0);
    }
    TestPaths alloc(paths.value.game);alloc.allocate=true;MemoryIO io;Log log(alloc,io);
    failNew=true;log.flush();CHECK(!failNew&&log.counters().io_failures==1&&failedNew==failedAtEntry+2);
    forwarded(&log);
    // Reentry into enqueue and flush while the consumer is doing I/O proves
    // that no I/O holds the producer ring lock (no timers or sleeps).
    struct ReentrantIO : MemoryIO {
        Log* log=nullptr;bool tested=false;
        size_t append(const fs::path& p,const char* b,size_t n) override {
            if(!tested){tested=true;CHECK(log->push(record(Event::descriptor)));log->flush();}
            return MemoryIO::append(p,b,n);
        }
    } reentrant;
    Log reentry(paths,reentrant);reentrant.log=&reentry;reentry.flush();CHECK(reentrant.tested);
    struct BlockedIO : MemoryIO {
        std::mutex mutex;std::condition_variable changed;bool held=false,release=false;
        size_t append(const fs::path& p,const char* b,size_t n) override {
            {std::unique_lock lock(mutex);if(!held){held=true;changed.notify_all();changed.wait(lock,[&]{return release;});}}
            return MemoryIO::append(p,b,n);
        }
    } blocked;
    Log slow(paths,blocked);std::thread consumer([&]{slow.flush();});
    {std::unique_lock lock(blocked.mutex);blocked.changed.wait(lock,[&]{return blocked.held;});}
    // This must complete while the consumer remains blocked inside I/O.
    CHECK(slow.push(record(Event::device)));
    {std::lock_guard lock(blocked.mutex);blocked.release=true;blocked.changed.notify_all();}
    consumer.join();CHECK(slow.counters().io_failures==0);
    MemoryIO policy;Log p(paths,policy);set_sink(&p);
    auto d=acz::capture_enablement::decide({});decision(d.enabled,d.reason);set_sink(nullptr);p.flush();
    CHECK(policy.files[0].find(d.reason)!=std::string::npos&&!d.enabled);
    // Repeat every public I/O failure against the production reserved/batched
    // path, including evidence initialization. Native HRESULT/out/IID/refs must
    // survive failure just as on the standalone ring path above.
    const auto failedBefore=failedNew;
    for(auto fault:{MemoryIO::Fault::directory,MemoryIO::Fault::open,MemoryIO::Fault::short_write,
        MemoryIO::Fault::disk_full,MemoryIO::Fault::size,
        MemoryIO::Fault::allocation,MemoryIO::Fault::reset}) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";io.fault=fault;
        Log log(paths,io,clockNow,true);log.push(record(Event::summary));
        forwarded(&log);log.execute_interval();log.flush();
        CHECK(!log.enabled()&&log.counters().io_failures==1&&obj.refs==1);
        const auto calls=io.calls;forwarded(&log);log.flush();CHECK(io.calls==calls);
    }
    CHECK(failedNew==failedBefore+1);
    // A failed reserved batch must stop this very flush before either a new
    // reason append or an existing reason overwrite can perform any more I/O.
    for(bool persisted:{false,true}) {
        struct FailedEvidenceIO : MemoryIO {
            bool armed=false;size_t callsAtFailure=0;
            size_t append(const fs::path& p,const char* bytes,size_t n) override {
                if(armed&&p.filename()==L"diagnostics.evidence.log") {
                    armed=false;fault=Fault::short_write;
                    auto result=MemoryIO::append(p,bytes,n);
                    fault=Fault::none;callsAtFailure=calls;return result;
                }
                return MemoryIO::append(p,bytes,n);
            }
        } io;
        io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();
        Mutex producer;
        if(persisted) {
            {std::lock_guard lock(producer);log.observer_disabled("failure reason");}
            log.flush();
        }
        CHECK(log.push(record(Event::pso,0,0,0,"pending reserved batch")));
        {std::lock_guard lock(producer);log.observer_disabled("failure reason");}
        io.armed=true;log.flush();
        CHECK(io.callsAtFailure!=0&&io.calls==io.callsAtFailure);
        CHECK(!log.enabled()&&log.counters().io_failures==1);
        const auto calls=io.calls;forwarded(&log);log.flush();CHECK(io.calls==calls);
    }
    MemoryIO allocation;allocation.readState=ReadState::ok;allocation.config="log_enabled=true";
    failNew=true;Log failedReserve(paths,allocation,clockNow,true);
    CHECK(!failNew&&!failedReserve.enabled()&&failedReserve.counters().io_failures==1);
    forwarded(&failedReserve);failedReserve.flush();CHECK(allocation.calls==0&&obj.refs==1);
}
static std::vector<uint8_t> base64(std::string_view text) {
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;unsigned v=0,bits=0;
    for(char c:text){if(c=='=')break;auto n=alphabet.find(c);CHECK(n!=std::string::npos);v=(v<<6)|unsigned(n);bits+=6;
        if(bits>=8){bits-=8;out.push_back(uint8_t(v>>bits));}}
    return out;
}
// Exercise actual patched D3D12 slots. Every native stub checks its arguments;
// each scenario is repeated with null, active, full and failed sinks.
namespace hook_fixture {
using namespace acz;
static FakeCOM* output=nullptr;
static thread_local FakeCOM* concurrentOutput=nullptr;
static thread_local void** concurrentOut=nullptr;
static HRESULT result=S_FALSE;
static const D3D12_GRAPHICS_PIPELINE_STATE_DESC* expectedPSO=nullptr;
static IID psoIID=__uuidof(ID3D12PipelineState);
static const void* expectedRoot=nullptr;
static size_t rootLength=0;
static unsigned draws=0,executes=0,writes=0,protects=0,failProtect=0;
static ID3D12CommandList* const* expectedLists=nullptr;
static FakeCOM* discoveredDevice=nullptr;
static UINT rootNode=7;
static IID rootIID=__uuidof(ID3D12RootSignature);
static void** rootOut=nullptr;
static bool checkRootOut=false;
static unsigned queryMode=0;
static IUnknown* queryAlias=nullptr;
static FakeCOM* deviceExtensionOutput=nullptr;
static IID deviceProbeIID=__uuidof(ID3D12Device14);
static std::function<void()> rootAction,queryAction,bindingAction,psoAction;
static bool nullRoot=false;
static HRESULT STDMETHODCALLTYPE getDevice(ID3D12CommandQueue*,REFIID iid,void** out) {
    CHECK(iid==__uuidof(ID3D12Device));*out=discoveredDevice;++discoveredDevice->refs;return S_OK;
}
// A virtual member supplies the SDK struct-return ABI (free functions differ).
struct QueueDesc {
    virtual D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE desc() {
        D3D12_COMMAND_QUEUE_DESC value{};value.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;return value;
    }
};
static HRESULT STDMETHODCALLTYPE query(IUnknown* self,REFIID iid,void** out) {
    for(unsigned i=1;i<std::size(acz::listIids);++i)if(iid==acz::listIids[i]){*out=nullptr;return E_NOINTERFACE;}

    CHECK(mutex_depth==0);
    if(queryAction){auto action=std::move(queryAction);queryAction={};action();}
    if(queryMode==13&&iid==deviceProbeIID)throw std::runtime_error("native QI exception");
    if(deviceExtensionOutput && iid==deviceProbeIID) {
        *out=deviceExtensionOutput;
        ++(deviceExtensionOutput->owner?deviceExtensionOutput->owner:deviceExtensionOutput)->refs;return S_OK;
    }
    if(queryMode==7&&iid==__uuidof(IUnknown)){*out=nullptr;return S_OK;}
    if(queryMode==10&&iid==__uuidof(IUnknown))throw std::bad_alloc();
    if(queryMode==9&&iid==deviceProbeIID){*out=reinterpret_cast<void*>(0x123);return E_NOINTERFACE;}
    if((queryMode==11||queryMode==12)&&iid==deviceProbeIID){
        *out=nullptr;return queryMode==11?E_NOINTERFACE:S_OK;
    }
    auto* f=reinterpret_cast<FakeCOM*>(self);
    if(f->unknownInterface) {
        auto* value=iid==__uuidof(IUnknown)?f->unknownInterface:f->rootInterface;
        ++(value->owner?value->owner:value)->refs;*out=value;return S_OK;
    }
    if((queryMode==1&&iid==__uuidof(ID3D12RootSignature)) ||
       (queryMode==2&&iid==__uuidof(IUnknown))) {*out=nullptr;return E_NOINTERFACE;}
    if(queryMode==3&&iid==__uuidof(ID3D12RootSignature)){*out=nullptr;return S_OK;}
    if(queryAlias && ((queryMode==4&&iid==__uuidof(ID3D12RootSignature)) ||
                      (queryMode==5&&iid==__uuidof(IUnknown)))) {
        *out=queryAlias;queryAlias->AddRef();return S_OK;
    }
    *out=self;++reinterpret_cast<FakeCOM*>(self)->refs;
    return queryMode==8&&iid==deviceProbeIID?S_FALSE:S_OK;
}
static ULONG STDMETHODCALLTYPE add(IUnknown* self){auto* f=reinterpret_cast<FakeCOM*>(self);return ++(f->owner?f->owner:f)->refs;}
static HRESULT STDMETHODCALLTYPE pso(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
    CHECK(mutex_depth==0);
    CHECK(desc==expectedPSO&&iid==psoIID);
    auto* target=output;
    if(psoAction){auto action=std::move(psoAction);psoAction={};action();}
    *out=target;++(target->owner?target->owner:target)->refs;return result;
}
static HRESULT STDMETHODCALLTYPE root(ID3D12Device*,UINT node,const void* bytes,SIZE_T n,REFIID iid,void** out) {
    CHECK(mutex_depth==0&&node==rootNode&&bytes==expectedRoot&&n==rootLength&&iid==rootIID);
    if(checkRootOut)CHECK(out==rootOut);
    if(concurrentOutput)CHECK(out==concurrentOut);
    if(rootAction){auto action=std::move(rootAction);rootAction={};action();}
    auto* target=concurrentOutput?concurrentOutput:output;
    if(SUCCEEDED(result)&&out){*out=nullRoot?nullptr:target;if(!nullRoot)++target->refs;}return result;
}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,UINT d) {
    CHECK(a==17&&b==2&&c==3&&d==4);++draws;
}
static void STDMETHODCALLTYPE execute(ID3D12CommandQueue*,UINT n,ID3D12CommandList* const* lists) {
    CHECK(n==1&&lists==expectedLists);++executes;
}
static void STDMETHODCALLTYPE cbv(ID3D12Device*,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    CHECK(!desc&&(dest.ptr==123||dest.ptr==124));++writes;
    CHECK(mutex_depth==0);
    if(bindingAction){auto action=std::move(bindingAction);bindingAction={};action();}
}
static void unused() {}
static HRESULT STDMETHODCALLTYPE sharedResource(ID3D12CompatibilityDevice*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,const D3D11_RESOURCE_FLAGS*,
    D3D12_COMPATIBILITY_SHARED_FLAGS,ID3D12LifetimeTracker*,ID3D12SwapChainAssistant*,REFIID,void** out) {
    CHECK(!mutex_depth);if(out)*out=output;return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE sharedHeap(ID3D12CompatibilityDevice*,const D3D12_HEAP_DESC*,D3D12_COMPATIBILITY_SHARED_FLAGS,REFIID,void** out) {
    CHECK(!mutex_depth);if(out)*out=output;return S_FALSE;
}
static BOOL WINAPI protect(LPVOID,SIZE_T,DWORD,DWORD* old) {
    *old=PAGE_READONLY;return ++protects!=failProtect;
}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE directListType(ID3D12CommandList*) {return D3D12_COMMAND_LIST_TYPE_DIRECT;}
static std::array<void*,64> table() {
    std::array<void*,64> t{};t.fill(reinterpret_cast<void*>(&unused));
    t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&add);t[2]=reinterpret_cast<void*>(&fakeRelease);t[8]=reinterpret_cast<void*>(&directListType);return t;
}
static size_t kinds(const std::string& text,const char* kind) {
    size_t count=0,off=0;const auto needle=std::string("|")+kind+"|";
    while((off=text.find(needle,off))!=text.npos){++count;off+=needle.size();}return count;
}
struct TraceIO : MemoryIO {
    std::string trace;
    size_t append(const fs::path& p,const char* bytes,size_t n) override {
        auto count=MemoryIO::append(p,bytes,n);trace.append(bytes,count);return count;
    }
};
static void run(TestPaths& paths,const std::vector<uint8_t>& shader,int mode) {
    TraceIO io;Log log(paths,io,clockNow);
    auto dt=table(),lt=table(),qt=table(),pt=table(),rt=table();
    dt[10]=reinterpret_cast<void*>(&pso);dt[16]=reinterpret_cast<void*>(&root);
    dt[17]=reinterpret_cast<void*>(&cbv);lt[12]=reinterpret_cast<void*>(&draw);qt[10]=reinterpret_cast<void*>(&execute);
    QueueDesc descDispatch;qt[7]=reinterpret_cast<void*>(&getDevice);
    qt[18]=(*reinterpret_cast<void***>(&descDispatch))[0];
    FakeCOM device{dt.data()},list{lt.data()},queue{qt.data()},pipeline{pt.data()},signature{rt.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);
    auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    auto* q=reinterpret_cast<ID3D12CommandQueue*>(&queue);
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&pipeline);
    auto* r=reinterpret_cast<ID3D12RootSignature*>(&signature);
    auto& s=acz::state();
    // Fixture-only reset between independent scenarios; no live producers.
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    g_protect=protect;g_pinResult=false;set_sink(mode?&log:nullptr);
    CHECK(!hookCommandList(l));g_pinResult=true;
    // Fail restoration after installing Release: partial patch retains dispatch.
    failProtect=protects+2;CHECK(!hookCommandList(l));
    CHECK(lt[2]==reinterpret_cast<void*>(&hookedRelease));
    failProtect=0;CHECK(hookCommandList(l)&&hookCommandQueue(q)&&hookCaptureDevice(d));
    {std::lock_guard lock(s.mutex);
        CHECK(patch(dt.data(),17,reinterpret_cast<void*>(&writeBindingCbv),s.devices[dt.data()]));
        CHECK(patch(dt.data(),0,reinterpret_cast<void*>(&queryBindingDevice),s.devices[dt.data()]));}
    if(mode) {
        log.flush();CHECK(log.counters().dropped==0);
        CHECK(kinds(io.trace,"hook_pin")==2);
        CHECK(io.trace.find("hook_pin|a=0")!=io.trace.npos&&io.trace.find("hook_pin|a=1")!=io.trace.npos);
        CHECK(io.trace.find("hook_patch|a=2|b=0|c=0|identity=unknown|hash=|sha256=|text=failed or partial patch")!=io.trace.npos);
        CHECK(io.trace.find("hook_patch|a=2|b=1|c=0|identity=unknown|hash=|sha256=|text=installed")!=io.trace.npos);
        CHECK(kinds(io.trace,"capture_decision")==1);
        CHECK(io.trace.find(capture_enablement::decide({}).reason)!=io.trace.npos);
    }
    const auto hookRecords=kinds(io.trace,"hook_patch"),pinRecords=kinds(io.trace,"hook_pin");
    const auto drawsBefore=draws,execBefore=executes;
    ID3D12CommandList* lists[]={l};expectedLists=lists;
    for(unsigned i=0;i<1000;++i){l->DrawInstanced(17,2,3,4);tick+=42;q->ExecuteCommandLists(1,lists);}
    CHECK(draws==drawsBefore+1000&&executes==execBefore+1000&&list.refs==1&&queue.refs==1);
    CHECK(log.counters().dropped==0); // Rechecks/draws used no ring slots.
    if(mode){log.flush();CHECK(kinds(io.trace,"hook_patch")==hookRecords&&kinds(io.trace,"hook_pin")==pinRecords);
        CHECK(kinds(io.trace,"capture_decision")==1);CHECK(io.trace.find("execute_summary|a=999|b=42|c=42")!=io.trace.npos);}
    if(mode==2)for(size_t i=0;i<ring_capacity;++i)CHECK(log.push(record(Event::device)));
    if(mode==3){io.fault=MemoryIO::Fault::open;log.push(record(Event::device));log.flush();CHECK(log.counters().io_failures==1);}
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.GS={shader.data(),shader.size()};expectedPSO=&desc;
    for(auto hr:{S_FALSE,E_FAIL}) {
        result=hr;output=&pipeline;void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==hr);
        CHECK(out==&pipeline&&pipeline.refs==2);CHECK(p->Release()==1);
    }
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};ID3DBlob* bytes=nullptr;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr)));
    expectedRoot=bytes->GetBufferPointer();rootLength=bytes->GetBufferSize();output=&signature;result=S_FALSE;void* out=nullptr;
    CHECK(d->CreateRootSignature(7,expectedRoot,rootLength,__uuidof(ID3D12RootSignature),&out)==S_FALSE);
    CHECK(out==&signature&&signature.refs==2&&s.roots.count(r)==1);CHECK(r->Release()==1);
    uint8_t bad[4]{};expectedRoot=bad;rootLength=sizeof bad;
    CHECK(d->CreateRootSignature(7,expectedRoot,rootLength,__uuidof(ID3D12RootSignature),&out)==S_FALSE);
    CHECK(out==&signature&&signature.refs==2&&!s.roots.count(r));CHECK(r->Release()==1);bytes->Release();
    const auto writesBefore=writes;d->CreateConstantBufferView(nullptr,{123});CHECK(writes==writesBefore+1);
    void* alias=nullptr;CHECK(d->QueryInterface(unknownIID,&alias)==S_OK&&alias==d&&device.refs==2);
    CHECK(d->Release()==1&&!s.nativeBindings.enabled());
    // A nested binding observation exercises the production disable event.
    {BindingEvent outer;d->CreateConstantBufferView(nullptr,{123});}
    CHECK(writes==writesBefore+2&&!s.nativeBindings.enabled());
    CHECK(device.refs==1&&pipeline.refs==1&&signature.refs==1&&list.refs==1&&queue.refs==1);
    discoveredDevice=&device;captureDeviceAndQueue(reinterpret_cast<IUnknown*>(&queue));
    CHECK(getHookedDevice()==d&&getHookedQueue()==q&&device.refs==2&&queue.refs==2);
    // Retained discovery references are explicitly quiesced by this CPU fixture.
    s.device=nullptr;s.queue=nullptr;CHECK(d->Release()==1&&q->Release()==1);
    if(mode==1) {
        CHECK(log.counters().dropped==0);log.flush();
        CHECK(kinds(io.trace,"pso")==4&&kinds(io.trace,"root")==2&&kinds(io.trace,"descriptor")==2);
        // Two discovery records plus the copied opaque unknown-IID detail.
        CHECK(kinds(io.trace,"device")==3&&io.trace.find("device and direct queue discovered")!=io.trace.npos);
        CHECK(io.trace.find("iid=000abcde-0001-0002-0304-05060708090a")!=io.trace.npos);
        const auto identity=shader_identity(shader.data(),shader.size());
        CHECK(io.trace.find(std::string("identity=match|hash=")+identity.hash.data()+"|sha256="+identity.container_prefix.data())!=io.trace.npos);
        CHECK(io.trace.find("pso|a=1|b=1|c=0")!=io.trace.npos&&io.trace.find("pso|a=0|b=1|c=0")!=io.trace.npos);
        CHECK(io.trace.find("root metadata accepted: explicit canonical provenance")!=io.trace.npos);
        CHECK(io.trace.find("root rejected: serialized_failure")!=io.trace.npos);
        CHECK(io.trace.find("text=CBV metadata write")!=io.trace.npos);
        CHECK(kinds(io.trace,"metadata_disabled")==2&&io.trace.find("text=overlap:descriptor_write/committed_create:same")!=io.trace.npos);
        CHECK(io.trace.find("text=binding device interface unsupported")!=io.trace.npos);
    }
    if(mode==2)CHECK(log.counters().dropped>0);
    if(mode==3)CHECK(log.counters().io_failures==1);
    set_sink(nullptr);
    // No owners remain in these fixtures; retire metadata before stack tables.
    CHECK(p->Release()==0&&r->Release()==0&&l->Release()==0);
    s.devices.erase(dt.data());s.objects.erase(pt.data());s.objects.erase(rt.data());
    s.lists.erase(lt.data());s.queues.erase(qt.data());
}
static HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*){CHECK(!mutex_depth);return S_OK;}
static ID3D12Device* listDevice=nullptr;
static HRESULT STDMETHODCALLTYPE getListDevice(ID3D12GraphicsCommandList*,REFIID,void** out) {
    CHECK(!mutex_depth);*out=listDevice;if(!listDevice)return E_NOINTERFACE;listDevice->AddRef();return S_OK;
}
static void STDMETHODCALLTYPE topology(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY){CHECK(!mutex_depth);}
#include "texture_creation_hook_cases.h"
static void tests(TestPaths& paths,const char* evidence) {
    textureCreationHookCases(paths);
    std::ifstream f(evidence,std::ios::binary);std::string text((std::istreambuf_iterator<char>(f)),{});
    auto off=text.find("\"base64\"");CHECK(off!=text.npos);
    auto begin=text.find('"',text.find(':',off))+1,end=text.find('"',begin);
    auto shader=base64(std::string_view(text).substr(begin,end-begin));
    // E10's first blob is the exact VS container used to create real root 317.
    // shader_identity compares the full SHA256 to the compiled-in E10 pin.
    CHECK(shader.size()==4268);
    const auto pin=shader_identity(shader.data(),shader.size());
    CHECK(pin.identity==Identity::match&&std::string(pin.container_prefix.data())=="adeb9ef4ff8fda08");
    const auto decoded=metadata::RootBindingMetadata::from_serialized(shader.data(),shader.size());
    CHECK(decoded.valid()&&decoded.metadata->layout().flags==0x30d);

    TraceIO io;Log log(paths,io,clockNow);
    auto dt=table(),qt=table(),rt=table(),at=table();
    dt[10]=reinterpret_cast<void*>(&pso);
    dt[16]=reinterpret_cast<void*>(&root);qt[7]=reinterpret_cast<void*>(&getDevice);
    dt[17]=reinterpret_cast<void*>(&cbv);qt[10]=reinterpret_cast<void*>(&execute);
    QueueDesc descDispatch;qt[18]=(*reinterpret_cast<void***>(&descDispatch))[0];
    FakeCOM device{dt.data()},queue{qt.data()},signature{rt.data()},alias{at.data()};
    auto d=reinterpret_cast<ID3D12Device*>(&device);
    auto q=reinterpret_cast<ID3D12CommandQueue*>(&queue);
    auto r=reinterpret_cast<ID3D12RootSignature*>(&signature);
    auto& s=acz::state();g_protect=protect;set_sink(&log);
    discoveredDevice=&device;captureDeviceAndQueue(reinterpret_cast<IUnknown*>(q));
    CHECK(getHookedDevice()==d&&getHookedQueue()==q);
    // Offline captureDeviceAndQueue intentionally omits the production binding
    // hook installation, so reproduce that discovery step explicitly.
    CHECK(hookNativeBindingDevice(d));
    // Canonical device identity accepts all SDK-known extensions; no wrapping.
    // All 14 known IID tags and the unknown GUID exercise the actual QI hook.
    const IID deviceIIDs[]={__uuidof(ID3D12Device1),__uuidof(ID3D12Device2),__uuidof(ID3D12Device3),__uuidof(ID3D12Device4),__uuidof(ID3D12Device5),__uuidof(ID3D12Device6),__uuidof(ID3D12Device7),__uuidof(ID3D12Device8),__uuidof(ID3D12Device9),__uuidof(ID3D12Device10),__uuidof(ID3D12Device11),__uuidof(ID3D12Device12),__uuidof(ID3D12Device13),__uuidof(ID3D12Device14),unknownIID};
    for(const auto& iid:deviceIIDs) {
        s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
        log.flush();io.trace.clear();
        const auto refs=device.refs;void* queried=nullptr;
        CHECK(d->QueryInterface(iid,&queried)==S_OK&&queried==d&&device.refs==refs+1);
        CHECK(d->Release()==refs);
        CHECK(s.nativeBindings.enabled()==(iid!=unknownIID));
        if(iid==unknownIID)CHECK(std::strcmp(s.nativeBindings.disable_reason(),unsupportedDeviceReason(iid))==0);
        log.flush();
        CHECK((io.trace.find(unsupportedDeviceReason(iid))!=io.trace.npos)==(iid==unknownIID));
        if(iid==unknownIID)CHECK(io.trace.find("iid=000abcde-0001-0002-0304-05060708090a")!=io.trace.npos);
    }
    // Every vetted SDK companion is queried through the patched native slot.
    // Distinct vtables, canonical identity, HRESULT, ownership and logging are
    // tested independently of the production IID classifier.
    const IID companions[]={__uuidof(ID3D12InfoQueue),__uuidof(ID3D12InfoQueue1),
        __uuidof(ID3D12DebugDevice),__uuidof(ID3D12DebugDevice1),__uuidof(ID3D12DebugDevice2),
        __uuidof(ID3D12DeviceRemovedExtendedData),__uuidof(ID3D12DeviceRemovedExtendedData1),
        __uuidof(ID3D12DeviceRemovedExtendedDataSettings),__uuidof(ID3D12DeviceRemovedExtendedDataSettings1),
        __uuidof(ID3D12DeviceConfiguration),__uuidof(ID3D12Tools),__uuidof(ID3D12Tools1)};
    auto resetObserver=[&] {
        s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
        log.flush();io.trace.clear();
    };
    auto checkGuid=[&](REFIID iid) {
        char guid[48]{};
        std::snprintf(guid,sizeof guid,"iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            iid.Data1,iid.Data2,iid.Data3,iid.Data4[0],iid.Data4[1],iid.Data4[2],iid.Data4[3],
            iid.Data4[4],iid.Data4[5],iid.Data4[6],iid.Data4[7]);
        log.flush();CHECK(io.trace.find(guid)!=io.trace.npos);
        CHECK(io.trace.find(unsupportedDeviceReason(iid))!=io.trace.npos);
    };
    unsigned companionTag=0;
    for(const auto& iid:companions) {
        deviceProbeIID=iid;++companionTag;
        CHECK(companionDeviceInterface(iid)==companionTag);
        for(bool separate:{false,true}) {
            resetObserver();const auto refs=device.refs;const auto untouched=at;
            if(separate){alias.owner=&device;alias.unknownInterface=&device;alias.rootInterface=&alias;deviceExtensionOutput=&alias;}
            void* queried=nullptr;
            CHECK(d->QueryInterface(iid,&queried)==S_OK);
            CHECK(queried==(separate?static_cast<void*>(&alias):static_cast<void*>(d)));
            CHECK(device.refs==refs+1&&s.nativeBindings.enabled()&&at==untouched);
            CHECK(!s.devices.count(at.data())); // No vtable mapping or patching.
            CHECK(static_cast<IUnknown*>(queried)->Release()==refs);
            log.flush();CHECK(io.trace.find("binding companion interface accepted")!=io.trace.npos);
            CHECK(io.trace.find("|device|a="+std::to_string(companionTag)+"|b="+
                std::to_string(separate?0:1)+"|c=0|")!=io.trace.npos);
            CHECK(io.trace.find(unsupportedDeviceReason(iid))==io.trace.npos);
            deviceExtensionOutput=nullptr;alias.owner=nullptr;alias.unknownInterface=nullptr;alias.rootInterface=nullptr;
        }
        for(unsigned failure=0;failure<4;++failure) {
            resetObserver();const auto refs=device.refs;
            queryMode=failure==0?2:failure==2?7:failure==3?10:0;
            deviceExtensionOutput=failure==1?&alias:nullptr;
            void* queried=nullptr;CHECK(d->QueryInterface(iid,&queried)==S_OK);
            CHECK(queried==(failure==1?static_cast<void*>(&alias):static_cast<void*>(d)));
            CHECK(!s.nativeBindings.enabled()&&device.refs==refs+(failure!=1));checkGuid(iid);
            queryMode=0;deviceExtensionOutput=nullptr;
            CHECK(static_cast<IUnknown*>(queried)->Release()==(failure==1?1:refs));
        }
        for(unsigned nativeMode:{8u,9u,11u,12u}) {
            resetObserver();queryMode=nativeMode;void* queried=nullptr;const auto refs=device.refs;
            CHECK(d->QueryInterface(iid,&queried)==(nativeMode==8?S_FALSE:nativeMode==12?S_OK:E_NOINTERFACE));
            CHECK(queried==(nativeMode==8?static_cast<void*>(d):nativeMode==9?reinterpret_cast<void*>(0x123):nullptr));
            CHECK(s.nativeBindings.enabled()&&device.refs==refs+(nativeMode==8));
            queryMode=0;if(nativeMode==8)CHECK(d->Release()==refs);
        }
        resetObserver();queryMode=13;void* queried=nullptr;const auto refs=device.refs;bool threw=false;
        try {d->QueryInterface(iid,&queried);}catch(const std::runtime_error&){threw=true;}
        CHECK(threw&&!s.nativeBindings.enabled()&&device.refs==refs&&!queried);checkGuid(iid);queryMode=0;
        // Canonical validation and diagnostic production do not allocate.
        resetObserver();failNew=true;queried=nullptr;
        CHECK(d->QueryInterface(iid,&queried)==S_OK&&s.nativeBindings.enabled()&&failNew);
        failNew=false;CHECK(d->Release()==refs);
    }
    // Documented limitation, NOT desired safety behavior: a companion's own
    // unpatched QueryInterface bypasses device-hook rejection. Exercise the
    // actual returned alias vtable, including interop and unknown IID escapes.
    for(const auto& escapedIID:{__uuidof(ID3D12CompatibilityDevice),
            __uuidof(ID3D11On12Device),__uuidof(ID3D11On12Device1),
            __uuidof(ID3D11On12Device2),unknownIID}) {
        resetObserver();const auto refs=device.refs;const auto untouched=at;
        deviceProbeIID=__uuidof(ID3D12InfoQueue);
        alias.owner=&device;alias.unknownInterface=&device;alias.rootInterface=&alias;
        deviceExtensionOutput=&alias;
        void* companion=nullptr;
        CHECK(d->QueryInterface(deviceProbeIID,&companion)==S_OK&&companion==&alias);
        CHECK(s.nativeBindings.enabled()&&device.refs==refs+1);
        log.flush();CHECK(io.trace.find("|device|a=1|b=0|c=0|")!=io.trace.npos);
        io.trace.clear();deviceExtensionOutput=nullptr;
        void* escaped=nullptr;
        CHECK(static_cast<IUnknown*>(companion)->QueryInterface(escapedIID,&escaped)==S_OK);
        CHECK(escaped==&alias&&device.refs==refs+2&&s.nativeBindings.enabled());
        CHECK(at==untouched&&!s.devices.count(at.data()));
        log.flush();CHECK(kinds(io.trace,"device")==0); // No device-hook record.
        CHECK(io.trace.find(unsupportedDeviceReason(escapedIID))==io.trace.npos);
        CHECK(static_cast<IUnknown*>(escaped)->Release()==refs+1);
        CHECK(static_cast<IUnknown*>(companion)->Release()==refs);
        alias.owner=nullptr;alias.unknownInterface=nullptr;alias.rootInterface=nullptr;
    }
    // Same-device-table compatibility results remain rejected by the clarified
    // scope. Distinct compatibility vtables are tested separately below.
    // These named fail-closed checks apply to queries through the device hook.
    const IID interop[]={__uuidof(ID3D12CompatibilityDevice),__uuidof(ID3D11On12Device),
        __uuidof(ID3D11On12Device1),__uuidof(ID3D11On12Device2)};
    for(const auto& iid:interop) {
        resetObserver();CHECK(!companionDeviceInterface(iid));void* queried=nullptr;const auto refs=device.refs;
        CHECK(d->QueryInterface(iid,&queried)==S_OK&&queried==d&&device.refs==refs+1);
        CHECK(!s.nativeBindings.enabled());CHECK(std::string_view(unsupportedDeviceReason(iid)).find("unknown")==std::string_view::npos);
        checkGuid(iid);CHECK(d->Release()==refs);
    }
    // Classification/reason cardinality remains fixed; ordinary companion
    // records consume no reason slots. Existing overlap matrix is unchanged.
    CHECK(std::size(companions)==12&&std::size(interop)==4);
    const char* reasons[34]{};size_t reasonCount=0;
    for(const auto& iid:deviceIIDs)reasons[reasonCount++]=unsupportedDeviceReason(iid);
    for(const auto& iid:{__uuidof(IUnknown),__uuidof(ID3D12Object),__uuidof(ID3D12Device)})
        reasons[reasonCount++]=unsupportedDeviceReason(iid);
    for(const auto& iid:companions)reasons[reasonCount++]=unsupportedDeviceReason(iid);
    for(const auto& iid:interop)reasons[reasonCount++]=unsupportedDeviceReason(iid);
    CHECK(reasonCount==std::size(reasons));
    for(size_t i=0;i<reasonCount;++i)for(size_t j=0;j<i;++j)CHECK(std::strcmp(reasons[i],reasons[j])!=0);

    {
        auto ct=table();ct[3]=reinterpret_cast<void*>(&sharedResource);ct[4]=reinterpret_cast<void*>(&sharedHeap);
        FakeCOM compat{ct.data(),1,&device,nullptr,&device};
        compat.rootInterface=&compat;
        resetObserver();deviceProbeIID=__uuidof(ID3D12CompatibilityDevice);deviceExtensionOutput=&compat;
        void* queried=nullptr;auto refs=device.refs;
        CHECK(d->QueryInterface(deviceProbeIID,&queried)==S_OK && queried==&compat && device.refs==refs+1);
        CHECK(s.nativeBindings.enabled());deviceExtensionOutput=nullptr;
        log.flush();CHECK(io.trace.find("binding compatibility interface accepted")!=io.trace.npos);
        CHECK(io.trace.find("|device|a=13|b=0|c=0|")!=io.trace.npos);
        auto native=reinterpret_cast<ID3D12CompatibilityDevice*>(&compat);
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        void* created=nullptr;auto previousOutput=output;output=&alias;
        CHECK(native->CreateSharedResource(nullptr,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
            D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&created)==S_FALSE);
        CHECK(created==&alias && s.nativeBindings.enabled());
        output=nullptr;
        CHECK(native->CreateSharedResource(nullptr,D3D12_HEAP_FLAG_NONE,nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,
            D3D12_COMPATIBILITY_SHARED_FLAG_NONE,nullptr,nullptr,__uuidof(ID3D12Resource),&created)==S_FALSE && !created);
        CHECK(native->CreateSharedHeap(nullptr,D3D12_COMPATIBILITY_SHARED_FLAG_NONE,__uuidof(ID3D12Heap),&created)==S_FALSE);
        CHECK(s.nativeBindings.enabled());output=previousOutput;
        log.flush();CHECK(io.trace.find("compat resource: unsupported form")!=io.trace.npos);
        CHECK(io.trace.find("compat resource: failed or null output")!=io.trace.npos);
        CHECK(io.trace.find("compat heap: unsupported memory heap")!=io.trace.npos);
        resetObserver();void* companion=nullptr;
        CHECK(native->QueryInterface(__uuidof(ID3D12InfoQueue),&companion)==S_OK && companion==native);
        CHECK(s.nativeBindings.enabled());log.flush();
        CHECK(io.trace.find("|device|a=1|b=0|c=0|")!=io.trace.npos); // Compare to device, not compat table.
        CHECK(static_cast<IUnknown*>(companion)->Release()==refs+1);
        for(auto iid:{unknownIID,__uuidof(ID3D11On12Device),__uuidof(ID3D11On12Device1),__uuidof(ID3D11On12Device2)}) {
            resetObserver();void* escaped=nullptr;
            CHECK(native->QueryInterface(iid,&escaped)==S_OK && escaped==native);
            CHECK(!s.nativeBindings.enabled());checkGuid(iid);
            CHECK(static_cast<IUnknown*>(escaped)->Release()==refs+1);
        }
        CHECK(native->Release()==refs);
        s.compatibilityOwners.erase(reinterpret_cast<IUnknown*>(&compat));s.compatibility.erase(ct.data());resetObserver();
    }
    {
        // Canonical-equal secondary address sharing a DEVICE table must also
        // reject before patching compatibility slots over device dispatch.
        FakeCOM sameTable{dt.data(),1,&device,nullptr,&device};sameTable.rootInterface=&sameTable;
        deviceProbeIID=__uuidof(ID3D12CompatibilityDevice);deviceExtensionOutput=&sameTable;
        const auto untouched=dt;auto refs=device.refs;void* queried=nullptr;
        CHECK(d->QueryInterface(deviceProbeIID,&queried)==S_OK && queried==&sameTable);
        CHECK(device.refs==refs+1 && !s.nativeBindings.enabled() && dt==untouched);
        checkGuid(deviceProbeIID);CHECK(static_cast<IUnknown*>(queried)->Release()==refs);
        deviceExtensionOutput=nullptr;resetObserver();
    }

    CHECK(binding::overlap_kind_count==13&&binding::overlap_reason_count==182);
    deviceProbeIID=__uuidof(ID3D12Device14);
    // A distinct extension interface with the same canonical IUnknown is only
    // identity-mapped. Its separate vtable must remain byte-for-byte unchanged.
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    const auto originalAliasTable=at;
    alias.owner=&device;alias.unknownInterface=&device;alias.rootInterface=&alias;
    deviceExtensionOutput=&alias;
    const auto deviceRefsBeforeAlias=device.refs;void* extension=nullptr;
    CHECK(d->QueryInterface(__uuidof(ID3D12Device14),&extension)==S_OK&&extension==&alias);
    CHECK(device.refs==deviceRefsBeforeAlias+1&&s.nativeBindings.enabled()&&at==originalAliasTable);
    CHECK(static_cast<IUnknown*>(extension)->Release()==deviceRefsBeforeAlias);
    deviceExtensionOutput=nullptr;alias.owner=nullptr;alias.unknownInterface=nullptr;alias.rootInterface=nullptr;
    // Native failure/null canonical identity and a different canonical object
    // preserve the original queried output/refcount but disable with its IID.
    for(unsigned failure=0;failure<4;++failure) {
        s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
        log.flush();io.trace.clear();
        queryMode=failure==0?2:failure==2?7:failure==3?10:0;
        deviceExtensionOutput=failure==1?&alias:nullptr;
        void* queried=nullptr;const auto refs=device.refs;
        CHECK(d->QueryInterface(__uuidof(ID3D12Device14),&queried)==S_OK);
        CHECK(queried==(failure==1?static_cast<void*>(&alias):static_cast<void*>(d)));
        CHECK(device.refs==refs+(failure!=1)&&!s.nativeBindings.enabled());
        CHECK(std::strcmp(s.nativeBindings.disable_reason(),unsupportedDeviceReason(__uuidof(ID3D12Device14)))==0);
        queryMode=0;deviceExtensionOutput=nullptr;
        CHECK(static_cast<IUnknown*>(queried)->Release()==(failure==1?1:refs));
    }
    // Native probes returning no interface publish no alias. Only failure of
    // the internal canonical QI after a successful native output disables.
    for(unsigned nativeMode:{8u,9u,11u,12u}) {
        s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
        queryMode=nativeMode;const auto refs=device.refs;void* queried=nullptr;
        log.flush();io.trace.clear();
        CHECK(d->QueryInterface(__uuidof(ID3D12Device14),&queried)==(nativeMode==8?S_FALSE:nativeMode==12?S_OK:E_NOINTERFACE));
        CHECK(queried==(nativeMode==8?static_cast<void*>(d):nativeMode==9?reinterpret_cast<void*>(0x123):nullptr));
        CHECK(s.nativeBindings.enabled());
        CHECK(device.refs==refs+(nativeMode==8));queryMode=0;
        if(nativeMode==8)CHECK(d->Release()==refs);
        log.flush();CHECK(io.trace.find(unsupportedDeviceReason(__uuidof(ID3D12Device14)))==io.trace.npos);
    }
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    expectedRoot=shader.data();rootLength=shader.size();rootNode=0;output=&signature;result=S_FALSE;
    auto clear=[&] {s.rootIdentities.erase(r);s.roots.erase(r);s.rootCreations.erase(r);};
    unsigned rootReferenceDelta=1;
    auto call=[&](const char* reason,REFIID iid=__uuidof(ID3D12RootSignature),bool noOut=false) {
        log.flush();io.trace.clear();const auto refs=signature.refs;void* out=reinterpret_cast<void*>(0x123);
        rootIID=iid;rootOut=noOut?nullptr:&out;checkRootOut=true;
        CHECK(d->CreateRootSignature(rootNode,expectedRoot,rootLength,iid,noOut?nullptr:&out)==result);
        checkRootOut=false;
        if(SUCCEEDED(result)&&!noOut&&!nullRoot) {
            CHECK(out==r&&signature.refs==refs+rootReferenceDelta);
            CHECK(r->Release()==refs+rootReferenceDelta-1);
        } else CHECK(signature.refs==refs);
        log.flush();
        if(io.trace.find(reason)==io.trace.npos)std::fprintf(stderr,"expected %s; observed %s\n",reason,io.trace.c_str());
        CHECK(io.trace.find(reason)!=io.trace.npos);
        if(std::string_view(reason)=="root rejected: ambiguous_order") {
            CHECK(io.trace.find("; overlap:")!=io.trace.npos);
            CHECK(io.trace.find("overlap:unresolved")==io.trace.npos);
        }
        CHECK(io.trace.find(" ids=")!=io.trace.npos&&io.trace.find(" eq=")!=io.trace.npos);
        char rawAddress[40]{};std::snprintf(rawAddress,sizeof rawAddress,"%p",r);
        CHECK(io.trace.find(rawAddress)==io.trace.npos);
        CHECK(log.counters().dropped==0&&log.counters().io_failures==0);
        if(std::string_view(reason).find("root rejected: ")==0&&std::string_view(reason).find("stale_")==std::string_view::npos)
            CHECK(!s.rootIdentities.count(r)&&!s.roots.count(r)&&!s.rootCreations.count(r));
    };
    // Baseline mirror: fresh Release slot; then shared slot already patched,
    // sink absent/present, and unrelated binding and metadata events active.
    // This mirror is accepted even before J-049: it does NOT reproduce L1b.
    const char* accepted="root metadata accepted: explicit canonical provenance";
    // J-056 discriminating fake-COM case: a shared-table, untracked object's
    // native Release calls CreateRootSignature synchronously on this thread.
    // This uses the real patched Release entry, not a fabricated parent guard.
    CHECK(trackMetadataLifetime(reinterpret_cast<IUnknown*>(&alias)));
    // Expected-red acceptance probe on unchanged production emitted
    // ambiguous_order; overlap:root_create/other:same, block=reentry.
    // Non-final Release now has no observation open across the native call.
    CHECK(reinterpret_cast<IUnknown*>(&alias)->AddRef()==2);
    deviceReleaseAction=[&] {
        call(accepted);
        CHECK(s.rootIdentities.count(r));
    };
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);
    alias.refs=1;clear();
    call(accepted);CHECK(s.rootIdentities.count(r));auto first=s.rootIdentities.at(r);
    CHECK(s.rootCreations.at(r).node==0&&s.rootCreations.at(r).iid==__uuidof(ID3D12RootSignature));
    clear();set_sink(nullptr);void* out=nullptr;
    CHECK(d->CreateRootSignature(0,expectedRoot,rootLength,__uuidof(ID3D12RootSignature),&out)==S_FALSE);
    CHECK(out==r&&signature.refs==2&&s.rootIdentities.at(r)!=first);CHECK(r->Release()==1);
    clear();set_sink(&log);
    // Overlay-like non-final device Release is not a metadata parent. Roots
    // and PSOs created inside it retain the same explicit root identity as
    // ordinary independent calls. Alternate 200 device and 200 metadata Release
    // cases, preserving the earlier device-repeat coverage. Synthetic only.
    for(unsigned iteration=0;iteration<400;++iteration) {
        clear();
        const auto deviceRefs=device.refs;
        deviceReleaseAction=[&] {
            call(accepted);
            CHECK(s.rootIdentities.count(r) && s.bindingEvents.load()==0);
            const auto identity=s.rootIdentities.at(r);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=r;
            output=&alias;expectedPSO=&pd;void* psoOut=nullptr;
            CHECK(d->CreateGraphicsPipelineState(&pd,__uuidof(ID3D12PipelineState),&psoOut)==S_FALSE);
            auto* p=reinterpret_cast<ID3D12PipelineState*>(psoOut);
            CHECK(psoOut==&alias && s.pipelines.at(p).root_identity==identity);
            CHECK(p->Release()==1 && p->Release()==0 && signature.refs==(iteration%2?2:1));
            alias.refs=1;
            output=&signature;expectedPSO=nullptr;
        };
        if(iteration%2) {
            CHECK(r->AddRef()==2);
            CHECK(r->Release()==1 && s.nativeBindings.enabled());
        } else {
            CHECK(d->AddRef()==deviceRefs+1);
            CHECK(d->Release()==deviceRefs && s.nativeBindings.enabled());
        }
    }
    clear();
    // Returned-zero of this very address invalidates the nested publication
    // after native return; its earlier accepted diagnostic is not final truth.
    deviceReleaseAction=[&] {call(accepted);CHECK(s.rootIdentities.count(r));};
    CHECK(r->Release()==0&&!s.rootIdentities.count(r));
    log.flush();
    CHECK(io.trace.find("root rejected: final_release_conflict; parent=metadata_release")!=io.trace.npos);
    signature.refs=1;
    // Give descriptor observations a known disjoint heap footprint. The old
    // fixture used an unknown handle and encoded unsafe overlap acceptance.
    s.nativeBindings.heap({987,123,0,1,1,0,0,0},reinterpret_cast<uintptr_t>(d));
    bool bindingContext=false;
    bindingAction=[&] {
        // Native CBV stub is inside the actual patched descriptor call. A
        // second thread creates the exact real-style root before it returns.
        MetadataEvent unrelated(reinterpret_cast<IUnknown*>(&alias),false);
        std::thread creator([&] {
            call(accepted);CHECK(s.rootIdentities.count(r));
            bindingContext=io.trace.find("bindings=1")!=io.trace.npos;
        });creator.join();
    };
    const auto writesBefore=writes;d->CreateConstantBufferView(nullptr,{123});CHECK(writes==writesBefore+1);
    // Continuity is retained only under isolated identical creation metadata.
    clear();call(accepted);auto token=s.rootIdentities.at(r);call(accepted);
    CHECK(s.rootIdentities.at(r)==token);clear();
    result=E_FAIL;call("root rejected: native_failure");result=S_FALSE;
    CHECK(bindingContext);
    // J-053 discriminating check, observed expected-red before the repair:
    // rule. Hold native CreateRootSignature in its fake callback while another
    // thread completes a genuinely different object's Release. join is the
    // ordering barrier: no sleeps, driver, GPU or runtime are involved. The
    // unchanged code rejected with ambiguous_order, order=5 creates=1 releases=0.
    clear();
    CHECK(trackMetadataLifetime(reinterpret_cast<IUnknown*>(&alias)));
    rootAction=[&] {
        alias.refs=2;
        ULONG remaining=0;
        std::thread retirement([&] {
            remaining=reinterpret_cast<IUnknown*>(&alias)->Release();
        });
        retirement.join();CHECK(remaining==1);
    };
    call(accepted);clear(); // J-053: completed distinct retirement must not poison creation.
    // A descriptor write alone has no MetadataEvent and does not exclude root
    // publication. This separates the two bookkeeping systems, rather than
    // attributing every concurrent observation to the binding cohort count.
    rootAction=[&] {
        std::thread writer([&] {d->CreateConstantBufferView(nullptr,{123});});
        writer.join();
    };
    call(accepted);clear();
    // J-053 review regression: metadata retention must not change J-047's
    // binding-cohort validation or consume its pool. All ordering is by join/CV.
    auto resetBindings=[&] {
        CHECK(!s.bindingEvents.load()&&!s.metadataEvents);
        s.nativeBindings.~NativeBindingObserver();
        new(&s.nativeBindings) binding::NativeBindingObserver;
        s.nativeBindings.heap({987,123,0,2,32,0,0,0},reinterpret_cast<uintptr_t>(d));
        CHECK(s.nativeBindings.enabled());
    };
    resetBindings();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC retentionPSO{};
    expectedPSO=&retentionPSO;output=&alias;
    bool capacityPSO=false;
    psoAction=[&] {
        std::thread writer([&] {for(unsigned n=0;n<300;++n){d->CreateConstantBufferView(nullptr,{123});log.flush();}});
        writer.join();capacityPSO=s.nativeBindings.enabled();
    };
    void* retentionOut=nullptr;
    CHECK(d->CreateGraphicsPipelineState(&retentionPSO,__uuidof(ID3D12PipelineState),&retentionOut)==S_FALSE);
    CHECK(retentionOut==&alias&&alias.refs==2);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);alias.refs=1;
    resetBindings();output=&signature;expectedPSO=nullptr;
    bool capacityRoot=false;
    rootAction=[&] {
        std::thread writer([&] {for(unsigned n=0;n<300;++n){d->CreateConstantBufferView(nullptr,{123});log.flush();}});
        writer.join();capacityRoot=s.nativeBindings.enabled();
    };
    // Before repair, pool exhaustion already loses root history, but must not
    // disable unrelated binding observation merely to retain root evidence.
    call("root rejected: unresolved_order");
    CHECK(io.trace.find("block=history_lost")!=io.trace.npos);clear();
    resetBindings();output=&alias;expectedPSO=&retentionPSO;
    // A lone malformed destination has registry-local policy. Metadata may
    // retain it, but a later non-overlapping call cannot turn it into overlap.
    psoAction=[&] {
        std::thread writer([&] {
            d->CreateConstantBufferView(nullptr,{124});
            CHECK(s.nativeBindings.enabled());
            d->CreateConstantBufferView(nullptr,{123});
            CHECK(s.nativeBindings.enabled());
        });writer.join();
    };
    CHECK(d->CreateGraphicsPipelineState(&retentionPSO,__uuidof(ID3D12PipelineState),&retentionOut)==S_FALSE);
    CHECK(retentionOut==&alias&&alias.refs==2&&s.nativeBindings.enabled());
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);alias.refs=1;
    resetBindings();
    bool unknownDisabled=false;
    psoAction=[&] {
        std::mutex m;std::condition_variable cv;bool entered=false,proceed=false;
        bindingAction=[&] {
            std::unique_lock lock(m);entered=true;cv.notify_all();cv.wait(lock,[&]{return proceed;});
        };
        std::thread unknown([&] {d->CreateConstantBufferView(nullptr,{124});});
        {std::unique_lock lock(m);cv.wait(lock,[&]{return entered;});}
        d->CreateConstantBufferView(nullptr,{123});
        {std::lock_guard lock(m);proceed=true;}cv.notify_all();unknown.join();
        // Validate while the PSO event still spans the completed cohort.
        unknownDisabled=!s.nativeBindings.enabled();
    };
    CHECK(d->CreateGraphicsPipelineState(&retentionPSO,__uuidof(ID3D12PipelineState),&retentionOut)==S_FALSE);
    CHECK(retentionOut==&alias&&alias.refs==2);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);alias.refs=1;
    std::printf("metadata retention: capacity PSO=%d root=%d unknown disabled=%d\n",
                capacityPSO,capacityRoot,unknownDisabled);
    CHECK(capacityPSO&&capacityRoot&&unknownDisabled);
    log.flush();CHECK(io.trace.find("overlap:descriptor_write/descriptor_write:cross")!=io.trace.npos);
    resetBindings();output=&signature;expectedPSO=nullptr;
    // An independent PSO (including its temporary hooked Releases) retains a
    // known disjoint footprint even after it has completed.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC independentPSO{};
    rootAction=[&] {
        output=&alias;expectedPSO=&independentPSO;
        std::thread creator([&] {
            void* psoOut=nullptr;
            CHECK(d->CreateGraphicsPipelineState(&independentPSO,__uuidof(ID3D12PipelineState),&psoOut)==S_FALSE);
            CHECK(psoOut==&alias&&alias.refs==2);
            CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
            CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);
        });
        creator.join();alias.refs=1;output=&signature;expectedPSO=nullptr;
    };
    call(accepted);clear();
    // Another root finishes entirely before the older native callback returns.
    // Both outputs are distinct, but the child publishes while the parent is
    // still unknown; only the parent can prove independence at publication.
    rootAction=[&] {
        output=&alias;checkRootOut=false;
        std::thread creator([&] {
            void* childOut=nullptr;
            CHECK(d->CreateRootSignature(rootNode,expectedRoot,rootLength,rootIID,&childOut)==S_FALSE);
            CHECK(childOut==&alias&&alias.refs==2);
            CHECK(!s.rootIdentities.count(reinterpret_cast<ID3D12RootSignature*>(&alias)));
            CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
        });
        creator.join();output=&signature;checkRootOut=true;
    };
    call(accepted);clear();
    CHECK(io.trace.find("block=active_root_unknown")!=io.trace.npos);
    // A PSO whose native output is still unknown blocks publication. The CV
    // keeps it in the actual native callback until after the root diagnostic.
    std::mutex pendingMutex;std::condition_variable pendingCV;
    bool psoEntered=false,allowPSO=false;std::thread pendingPSO;
    rootAction=[&] {
        output=&alias;expectedPSO=&independentPSO;
        psoAction=[&] {
            CHECK(mutex_depth==0);std::unique_lock lock(pendingMutex);
            psoEntered=true;pendingCV.notify_all();pendingCV.wait(lock,[&]{return allowPSO;});
        };
        pendingPSO=std::thread([&] {
            void* value=nullptr;
            CHECK(d->CreateGraphicsPipelineState(&independentPSO,__uuidof(ID3D12PipelineState),&value)==S_FALSE);
            CHECK(value==&alias&&alias.refs==2);
            CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
            CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);
        });
        {std::unique_lock lock(pendingMutex);pendingCV.wait(lock,[&]{return psoEntered;});}
        output=&signature;
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=active_pso_unknown")!=io.trace.npos);
    CHECK(io.trace.find("overlap:root_create/pso_create:cross")!=io.trace.npos);
    {std::lock_guard lock(pendingMutex);allowPSO=true;}pendingCV.notify_all();pendingPSO.join();
    alias.refs=1;expectedPSO=nullptr;
    // Retained history distinguishes completed same-address retirement from a
    // genuinely disjoint retirement. Repetition is probabilistic evidence only.
    for(unsigned iteration=0;iteration<200;++iteration) {
        for(auto* address:{reinterpret_cast<IUnknown*>(&alias),reinterpret_cast<IUnknown*>(r)}) {
            RootCreationEvent pendingCreation;
            std::thread retirement([&] {MetadataEvent completed(address,false);});
            retirement.join();
            std::lock_guard lock(s.mutex);
            pendingCreation.identify(reinterpret_cast<IUnknown*>(r));
            const bool conflict=address==reinterpret_cast<IUnknown*>(r);
            CHECK(pendingCreation.ambiguous==conflict);
            CHECK(pendingCreation.distinct(s)==!conflict);
            CHECK(s.metadataEvents==&pendingCreation&&pendingCreation.next==nullptr);
            CHECK(s.rootCreates==1&&s.metadataReleases==0);
        }
    }
    // Three real patched native calls overlap, and all canonical footprints
    // become known before any publishes. The test-only ready seam runs after
    // COM identity validation, outside the mutex. No sleeps establish ordering.
    // Repeating this schedule is probabilistic evidence, not runtime proof.
    std::array<capture::Id,3> priorTokens{};
    for(unsigned iteration=0;iteration<200;++iteration) {
        TraceIO concurrentIO;concurrentIO.readState=ReadState::ok;concurrentIO.config="log_enabled=true";
        Log concurrentLog(paths,concurrentIO,clockNow,true);concurrentLog.flush();
        set_sink(iteration%2?&concurrentLog:nullptr);
        auto ct=table();
        std::array<FakeCOM,3> roots{{{ct.data()},{ct.data()},{ct.data()}}};
        std::mutex mutex;std::condition_variable cv;unsigned ready=0;
        std::array<std::thread,3> creators;
        for(size_t i=0;i<creators.size();++i)creators[i]=std::thread([&,i] {
            concurrentOutput=&roots[i];void* value=nullptr;concurrentOut=&value;
            g_rootReady=[&] {
                CHECK(mutex_depth==0);
                std::unique_lock lock(mutex);++ready;cv.notify_all();
                cv.wait(lock,[&]{return ready==creators.size();});
            };
            CHECK(d->CreateRootSignature(rootNode,expectedRoot,rootLength,rootIID,&value)==S_FALSE);
            CHECK(value==&roots[i]&&roots[i].refs==2);
            concurrentOutput=nullptr;concurrentOut=nullptr;
        });
        for(auto& creator:creators)creator.join();
        const auto previousHigh=*std::max_element(priorTokens.begin(),priorTokens.end());
        for(size_t i=0;i<roots.size();++i) {
            auto* value=reinterpret_cast<ID3D12RootSignature*>(&roots[i]);
            CHECK(s.rootIdentities.count(value));const auto token=s.rootIdentities.at(value);
            CHECK(token>previousHigh);
            for(size_t j=0;j<i;++j)CHECK(token!=priorTokens[j]);
            priorTokens[i]=token;
            CHECK(value->Release()==1&&value->Release()==0);
        }
        set_sink(nullptr);concurrentLog.flush();
        // Production reserved root records cannot be lost to ordinary-ring
        // contention; ordinary hook records retain their documented drop policy.
        if(iteration%2)CHECK(kinds(concurrentIO.trace,"root")==3&&concurrentLog.counters().io_failures==0);
        s.objects.erase(ct.data());
    }
    set_sink(&log);
    // The metadata history itself never allocates on begin, identification or
    // completion. An allocation trap remains armed through the entire cohort.
    failNew=true;
    {RootCreationEvent event;std::lock_guard lock(s.mutex);event.identify(reinterpret_cast<IUnknown*>(r));}
    CHECK(failNew);failNew=false;
    // A late-discovered canonical key must match completed history even when
    // the initially returned interface had a different address.
    {
        RootCreationEvent event;
        {std::lock_guard lock(s.mutex);event.identify(reinterpret_cast<IUnknown*>(r));}
        std::thread worker([&]{MetadataEvent retired(reinterpret_cast<IUnknown*>(&alias),false);});worker.join();
        std::lock_guard lock(s.mutex);CHECK(event.distinct(s));
        event.touch(reinterpret_cast<IUnknown*>(&alias));CHECK(event.ambiguous&&!event.distinct(s));
    }
    // Actual patched Release completes while the native creator is held. Its
    // same-address footprint must survive late identification of the output.
    rootAction=[&] {
        std::thread worker([&] {CHECK(r->Release()==0);});worker.join();
        signature.refs=1; // Fake address reuse after the native final release.
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=completed_conflict")!=io.trace.npos);
    // A completed event with no identified output remains unknown; absence of
    // an active event is insufficient evidence of independence.
    rootAction=[&] {
        std::thread worker([] {MetadataEvent unknown(nullptr,false);});worker.join();
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=completed_unknown")!=io.trace.npos);
    // Overflow is an unresolved history gap, not proof that evicted events
    // were disjoint. It recovers only after the overlapping root has retired.
    rootAction=[&] {
        std::thread worker([&] {
            for(size_t i=0;i<bindingCohort().completed.size()+1;++i) {
                MetadataEvent completed(reinterpret_cast<IUnknown*>(&alias),false);
            }
        });worker.join();
    };
    call("root rejected: unresolved_order");
    CHECK(io.trace.find("block=history_lost")!=io.trace.npos);
    call(accepted);clear();
    // BindingEvent completed history uses the SAME clock and remains retained
    // while a metadata creator has an unidentified output.
    rootAction=[&] {
        std::thread worker([&] {BindingEvent retired(reinterpret_cast<uintptr_t>(r),true);});worker.join();
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=binding_conflict")!=io.trace.npos);
    rootAction=[&] {
        std::thread worker([&] {
            BindingEvent created;created.identify(reinterpret_cast<uintptr_t>(&alias));
        });worker.join();
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=binding_create_identity_unknown")!=io.trace.npos);
    CHECK(io.trace.find("overlap:committed_create/root_create:cross")!=io.trace.npos);
    // An unidentified active binding creation blocks even an otherwise isolated
    // metadata event. Neither an empty active-metadata set nor a binding count
    // alone can prove independence.
    bool bindingEntered=false,allowBinding=false;std::thread pendingBinding;
    rootAction=[&] {
        pendingBinding=std::thread([&] {
            BindingEvent unknown;
            std::unique_lock lock(pendingMutex);bindingEntered=true;pendingCV.notify_all();
            pendingCV.wait(lock,[&]{return allowBinding;});
        });
        std::unique_lock lock(pendingMutex);pendingCV.wait(lock,[&]{return bindingEntered;});
    };
    call("root rejected: ambiguous_order");
    CHECK(io.trace.find("block=binding_create_unknown")!=io.trace.npos);
    {std::lock_guard lock(pendingMutex);allowBinding=true;}pendingCV.notify_all();pendingBinding.join();
    // Non-final PSO Release does not retire its root dependency. A final Release
    // still records the dependency and conflicts with overlapping publication.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dependent{};dependent.pRootSignature=r;
    output=&alias;expectedPSO=&dependent;void* dependentOut=nullptr;
    CHECK(d->CreateGraphicsPipelineState(&dependent,__uuidof(ID3D12PipelineState),&dependentOut)==S_FALSE);
    CHECK(dependentOut==&alias&&alias.refs==2&&signature.refs==2);
    output=&signature;expectedPSO=nullptr;
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==1);
    rootAction=[&] {
        std::thread worker([&] {CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0);});worker.join();
    };
    rootReferenceDelta=0; // Final PSO Release drops its retained root reference.
    call("root rejected: ambiguous_order");
    rootReferenceDelta=1;
    CHECK(io.trace.find("block=completed_conflict")!=io.trace.npos);
    CHECK(signature.refs==1);alias.refs=1;
    call("root rejected: null_out",__uuidof(ID3D12RootSignature),true);
    nullRoot=true;call("root rejected: null_object");nullRoot=false;
    call("root rejected: unsupported_iid",unknownIID);
    queryMode=1;call("root rejected: root_qi_failed");
    CHECK(io.trace.find("c=2147500034")!=io.trace.npos); // E_NOINTERFACE bits
    queryMode=3;call("root rejected: root_qi_null");
    queryAlias=reinterpret_cast<IUnknown*>(&alias);queryMode=4;call("root rejected: root_alias");
    queryMode=2;call("root rejected: canonical_qi_failed");
    queryMode=5;call("root rejected: canonical_mismatch");queryMode=0;queryAlias=nullptr;
    CHECK(alias.refs==1);
    // Stable split COM identity: canonical differs from the root interface.
    // End the earlier base-address lifetime before changing this fake's layout.
    CHECK(r->Release()==0);signature.refs=1;
    signature.rootInterface=alias.rootInterface=&signature;
    signature.unknownInterface=alias.unknownInterface=&alias;alias.owner=&signature;
    auto key=reinterpret_cast<ID3D12RootSignature*>(&alias);
    call(accepted);CHECK(s.rootIdentities.count(key)&&!s.rootIdentities.count(r));
    CHECK(io.trace.find("ids=1/1/3 eq=1")!=io.trace.npos);
    char address[40]{};std::snprintf(address,sizeof address,"%p",r);
    CHECK(io.trace.find(address)==io.trace.npos);
    auto splitToken=s.rootIdentities.at(key);
    set_sink(nullptr);rootIID=__uuidof(ID3D12RootSignature);out=nullptr;
    CHECK(d->CreateRootSignature(0,expectedRoot,rootLength,rootIID,&out)==S_FALSE);
    CHECK(out==r&&signature.refs==2&&s.rootIdentities.at(key)==splitToken);
    CHECK(r->Release()==1);set_sink(&log);
    // Even a self-consistent claimant cannot take over another live record's
    // canonical address. The creation still forwards its original result.
    auto collisionTable=table();FakeCOM collision{collisionTable.data()};
    collision.rootInterface=&collision;collision.unknownInterface=&alias;
    alias.rootInterface=&collision;output=&collision;log.flush();io.trace.clear();out=nullptr;
    CHECK(d->CreateRootSignature(0,expectedRoot,rootLength,rootIID,&out)==S_FALSE&&out==&collision);
    CHECK(collision.refs==2&&signature.refs==1&&!s.rootIdentities.count(key));
    CHECK(reinterpret_cast<IUnknown*>(&collision)->Release()==1);log.flush();
    CHECK(io.trace.find("root rejected: root_alias")!=io.trace.npos);
    CHECK(io.trace.find("ids=1/1/3 eq=1")!=io.trace.npos);
    alias.rootInterface=&signature;output=&signature;call(accepted);
    CHECK(s.rootIdentities.at(key)!=splitToken);
    CHECK(reinterpret_cast<IUnknown*>(&alias)->Release()==0&&!s.rootIdentities.count(key));
    signature.refs=1;alias.owner=nullptr;signature.rootInterface=signature.unknownInterface=nullptr;
    alias.rootInterface=alias.unknownInterface=nullptr;
    expectedRoot=nullptr;call("root rejected: null_blob");expectedRoot=shader.data();
    rootLength=0;call("root rejected: empty_blob");
    rootLength=metadata::RootBindingMetadata::max_serialized_bytes+1;call("root rejected: oversized_blob");
    rootLength=4;call("root rejected: serialized_failure");
    CHECK(io.trace.find("|b="+std::to_string(static_cast<unsigned>(metadata::SerializedRootFailure::truncated_header))+"|")!=io.trace.npos);
    CHECK(io.trace.find("detail=serialized container header is truncated")!=io.trace.npos);
    D3D12_ROOT_SIGNATURE_DESC localDesc{};localDesc.Flags=D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    ID3DBlob* localBytes=nullptr;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&localDesc,D3D_ROOT_SIGNATURE_VERSION_1,&localBytes,nullptr)));
    expectedRoot=localBytes->GetBufferPointer();rootLength=localBytes->GetBufferSize();call("root rejected: serialized_failure");
    CHECK(io.trace.find("|b="+std::to_string(static_cast<unsigned>(metadata::SerializedRootFailure::unsupported_layout))+"|")!=io.trace.npos);
    CHECK(io.trace.find("detail=unsupported root signature flags")!=io.trace.npos);
    // A byte count such as the live lead's 184 cannot identify a failure.
    // Two synthetic 184-byte inputs take different gates. The bounded header
    // check permits trailing readable storage; the SDK rejects this padded
    // representation. A declared extent beyond storage fails before the SDK.
    std::array<uint8_t,184> sized{};
    CHECK(localBytes->GetBufferSize()<sized.size());
    std::memcpy(sized.data(),localBytes->GetBufferPointer(),localBytes->GetBufferSize());
    expectedRoot=sized.data();rootLength=sized.size();call("root rejected: serialized_failure");
    CHECK(io.trace.find("bytes=184")!=io.trace.npos);
    if(io.trace.find("|b=4|")==io.trace.npos)std::fprintf(stderr,"184-byte classification: %s\n",io.trace.c_str());
    CHECK(io.trace.find("|b="+std::to_string(static_cast<unsigned>(metadata::SerializedRootFailure::sdk_rejected))+"|")!=io.trace.npos);
    sized[24]=0xff;sized[25]=0xff; // declared DXBC size exceeds 184 readable bytes
    call("root rejected: serialized_failure");
    CHECK(io.trace.find("|b="+std::to_string(static_cast<unsigned>(metadata::SerializedRootFailure::header_bounds))+"|")!=io.trace.npos);
    CHECK(io.trace.find("detail=serialized container header extent exceeds input")!=io.trace.npos);
    CHECK(io.trace.find("form=dxbc")!=io.trace.npos);
    localBytes->Release();expectedRoot=shader.data();
    rootLength=shader.size();
    g_rootMetadataAllocation=false;call("root rejected: allocation_failure");g_rootMetadataAllocation=true;
    for(unsigned stage:{1u,2u,3u,4u}) {
        g_rootPublishFailure=stage;
        call(stage<3?"root rejected: allocation_failure":stage==3?"root rejected: observation_exception":"root rejected: identity_exhausted");
        CHECK(!s.rootIdentities.count(r)&&!s.roots.count(r)&&!s.rootCreations.count(r));
    }g_rootPublishFailure=0;
    // Force restoration of the Release prerequisite to fail, never the native call.
    const auto releaseHook=rt[2];rt[2]=reinterpret_cast<void*>(&fakeRelease);
    failProtect=protects+1;call("root rejected: lifetime_hook");failProtect=0;rt[2]=releaseHook;
    call(accepted);
    {MetadataEvent overlap(reinterpret_cast<IUnknown*>(&alias),false);call("root rejected: continuity_overlap");}
    call(accepted);
    rootAction=[&] {
        std::thread worker([] {BindingEvent disjoint(998,true);});worker.join();
    };
    call("root rejected: continuity_overlap");
    CHECK(io.trace.find("block=continuity_requires_isolation")!=io.trace.npos);
    clear();{MetadataEvent overlap(reinterpret_cast<IUnknown*>(r),false);call("root rejected: ambiguous_order");}
    clear();std::unique_ptr<MetadataEvent> pending;
    queryAction=[&]{std::thread worker([&]{pending=std::make_unique<MetadataEvent>(nullptr,false);});worker.join();};
    call("root rejected: unresolved_order");
    CHECK(io.trace.find("block=active_metadata_unknown")!=io.trace.npos);pending.reset();
    auto publishNewer=[&] {
        RootCreationRecord record;record.publication=s.rootEventSerial+1;
        s.rootCreations.insert_or_assign(r,std::move(record));
    };
    rootAction=publishNewer;call("root rejected: stale_invalidation");clear();
    queryAction=publishNewer;call("root rejected: stale_publication");clear();
    // No fake acceptance supplies a native capture proof or enables capture.
    capture::State native;CHECK(!native.pipeline.requirements_proven);
    CHECK(!capture::freeze(native,capture::Command{false,1,1},capture::Inputs{}));
    CHECK(!capture_enablement::decide({}).enabled);
    CHECK(device.refs==2&&queue.refs==2&&signature.refs==1&&alias.refs==1);
    s.device=nullptr;s.queue=nullptr;CHECK(d->Release()==1&&q->Release()==1);
    CHECK(r->Release()==0);set_sink(nullptr);
    s.devices.erase(dt.data());s.queues.erase(qt.data());s.objects.erase(rt.data());s.objects.erase(at.data());
    rootNode=7;rootIID=__uuidof(ID3D12RootSignature);expectedRoot=nullptr;rootLength=0;discoveredDevice=nullptr;
    for(int mode=0;mode<4;++mode)run(paths,shader,mode);
}
}
namespace metadata_fixture {
using namespace acz;
// Independent fake-QI fault per source and IID: no production-only test seam.
static IUnknown* deniedQuerySource=nullptr;
static IID deniedQueryIID{};
static HRESULT deniedQueryResult=E_NOINTERFACE;
static IUnknown* diagnosticOutput=nullptr;
static unsigned diagnosticQueryCalls=0;
static bool identityQueryFault=false;
static HRESULT identityQueryResult=E_NOINTERFACE;
// Optional six-address shape, used only by the J-105 real-wrapper controls.
static IUnknown *ancestorBase=nullptr,*ancestorCanonical=nullptr;
static std::array<IUnknown*,6> ancestorOutputs{};
static HRESULT STDMETHODCALLTYPE conditionQuery(IUnknown* self,REFIID iid,void** out) {
    if(ancestorBase&&(self==ancestorBase||self==ancestorCanonical)) {
        const IID iids[]={__uuidof(ID3D12Object),__uuidof(ID3D12DeviceChild),__uuidof(ID3D12Pageable)};
        for(size_t i=0;i<std::size(iids);++i)if(iid==iids[i]) {
            auto* value=ancestorOutputs[2*i+(self==ancestorCanonical)];
            *out=value;if(value)value->AddRef();return S_OK;
        }
    }
    if(identityQueryFault&&self==diagnosticOutput&&iid==__uuidof(IUnknown)) {
        *out=nullptr;return identityQueryResult;
    }
    if(self==deniedQuerySource&&iid==deniedQueryIID) {
        ++diagnosticQueryCalls;*out=diagnosticOutput;
        if(diagnosticOutput)diagnosticOutput->AddRef();
        return deniedQueryResult;
    }
    return hook_fixture::query(self,iid,out);
}
static void conditionDiagnostics() {
    using namespace acz::diagnostics;
    auto bt=hook_fixture::table(),ct=hook_fixture::table();
    bt[0]=ct[0]=reinterpret_cast<void*>(&conditionQuery);
    FakeCOM base{bt.data()},canonical{ct.data()};
    base.rootInterface=canonical.rootInterface=&base;
    base.unknownInterface=canonical.unknownInterface=&canonical;canonical.owner=&base;
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&base);
    auto* c=reinterpret_cast<IUnknown*>(&canonical);
    auto probe=[&] {
        PipelineInterfaces observed;observed.observe(p);return observed.conditions;
    };
    const auto good=probe();CHECK((good.known&0xff)==0xff&&(good.value&0xff)==0xff);
    CHECK(good.value&split_address);
    const IID iids[]={__uuidof(IUnknown),__uuidof(ID3D12PipelineState),
        __uuidof(ID3D12Object),__uuidof(ID3D12Object),
        __uuidof(ID3D12DeviceChild),__uuidof(ID3D12DeviceChild),
        __uuidof(ID3D12Pageable),__uuidof(ID3D12Pageable)};
    for(unsigned bit=0;bit<8;++bit) {
        deniedQueryIID=iids[bit];
        deniedQuerySource=bit<2||bit%2?c:reinterpret_cast<IUnknown*>(p);
        const auto bad=probe();
        CHECK((bad.known&(1u<<bit))&&!(bad.value&(1u<<bit)));
        CHECK((bad.value&((1u<<bit)-1))==((1u<<bit)-1));
        // Legacy bits stay unknown after the first failure; independent fields continue.
        CHECK(!(bad.known&(0xffu&~((1u<<(bit+1))-1))));
    }
    deniedQuerySource=nullptr;
    CHECK(probe().value==good.value&&base.refs==1&&canonical.refs==1);
    // All six slots run independently, including after an earlier failed QI.
    auto detail=[&] {PipelineInterfaces v;v.observe(p);return std::array<uint32_t,5>{
        v.qiFailed,v.identityMismatch,v.nullOutput,v.alias,v.hrClasses};};
    const auto healthy=detail();CHECK(healthy[0]==0&&healthy[1]==0&&healthy[2]==0&&healthy[3]==0);
    CHECK(healthy[4]==0x555555u);
    const IID ancestors[]={__uuidof(ID3D12Object),__uuidof(ID3D12DeviceChild),__uuidof(ID3D12Pageable)};
    for(unsigned slot=0;slot<6;++slot) {
        deniedQuerySource=slot%2?c:reinterpret_cast<IUnknown*>(p);deniedQueryIID=ancestors[slot/2];
        for(auto hr:{E_NOINTERFACE,E_FAIL,S_OK}) {
            deniedQueryResult=hr;diagnosticQueryCalls=0;
            const auto bad=detail();CHECK(diagnosticQueryCalls==1);
            const auto q=2*slot;
            CHECK(bad[0]==(FAILED(hr)?1u<<q:0));CHECK(bad[1]==0);
            CHECK(bad[2]==(1u<<q)&&bad[3]==0);
            const auto expected=(healthy[4]&~(15u<<(2*q)))|
                ((SUCCEEDED(hr)?1u:hr==E_NOINTERFACE?2u:3u)<<(2*q));
            CHECK(bad[4]==expected); // all OTHER slots and both later interfaces survived
        }
        deniedQuerySource=nullptr;
    }
    deniedQueryResult=E_NOINTERFACE;
    // A third interface isolates IUnknown failures/mismatches from address failure.
    auto tt=hook_fixture::table();tt[0]=reinterpret_cast<void*>(&conditionQuery);
    FakeCOM third{tt.data()};third.unknownInterface=&canonical;third.rootInterface=&base;
    auto* t=reinterpret_cast<IUnknown*>(&third);
    for(unsigned slot=0;slot<6;++slot) {
        deniedQuerySource=slot%2?c:reinterpret_cast<IUnknown*>(p);deniedQueryIID=ancestors[slot/2];
        deniedQueryResult=S_OK;diagnosticOutput=t;
        {const auto bad=detail();CHECK(bad[0]==0&&bad[1]==0&&bad[3]==(1u<<slot));CHECK(bad[4]==healthy[4]);}
        // Change only the returned ancestor's identity; successful QI mismatch.
        third.unknownInterface=&third;
        {const auto bad=detail();CHECK(bad[0]==0&&bad[1]==(1u<<slot)&&bad[3]==(1u<<slot));}
        third.unknownInterface=&canonical;
        for(auto hr:{E_NOINTERFACE,E_FAIL,S_OK}) {
            identityQueryFault=true;identityQueryResult=hr;
            const auto bad=detail();const auto q=2*slot+1;
            CHECK(bad[0]==(FAILED(hr)?1u<<q:0));CHECK(bad[1]==0);
            CHECK(bad[2]==(1u<<q)&&bad[3]==(1u<<slot));
            const auto expected=(healthy[4]&~(3u<<(2*q)))|
                ((SUCCEEDED(hr)?1u:hr==E_NOINTERFACE?2u:3u)<<(2*q));
            CHECK(bad[4]==expected);
        }
        identityQueryFault=false;
        // An anomalous failed QI with a nonnull output is failed, not null;
        // its reference is still balanced and its identity query is skipped.
        deniedQueryResult=E_FAIL;
        {const auto bad=detail();const auto q=2*slot;
            CHECK(bad[0]==(1u<<q)&&bad[1]==0&&bad[2]==0&&bad[3]==0);
            CHECK(bad[4]==((healthy[4]&~(15u<<(2*q)))|(3u<<(2*q))));}
        diagnosticOutput=nullptr;deniedQuerySource=nullptr;
    }
    // A failed canonical-to-base round trip also leaves all ancestor probes running.
    canonical.rootInterface=&third;
    {const auto bad=detail();CHECK(bad[0]==0&&bad[1]==0&&bad[2]==0&&bad[3]==0x2a);
        CHECK(bad[4]==healthy[4]);}
    canonical.rootInterface=&base;
    deniedQueryResult=E_NOINTERFACE;
    CHECK(detail()==healthy&&base.refs==1&&canonical.refs==1&&third.refs==1);
    {
        PipelineInterfaces max;max.qiFailed=max.nullOutput=0xfff;
        max.identityMismatch=max.alias=0x3f;max.hrClasses=0xffffff;
        LifetimeConditions order;order.known=order.value=0x7fff;
        char fields[96]{};max.format(fields,sizeof fields,order);
        CHECK(std::strcmp(fields,";qf=fff;qm=3f;qn=fff;qa=3f;qh=ffffff;od=7fff/7fff;ac=00")==0);
        char context[192]{};
        std::snprintf(context,sizeof context,";pd=ffffff/ffffff;rd=ffffff/ffffff;bd=ffffff/ffffff%s",fields);
        char full[diagnostics::text_capacity]{};
        const auto size=std::snprintf(full,sizeof full,
            "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven;"
            "cb10=4294967295;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;lp=3%s",context);
        CHECK(size>0&&size<int(sizeof full)&&std::strstr(full,fields));
    }

    base.unknownInterface=nullptr;{PipelineInterfaces same;same.observe(p);
        CHECK(same.valid&&(same.conditions.known&split_address)&&!(same.conditions.value&split_address));
        CHECK(!(same.conditions.known&stable_qi));} // self path does not ask this QI
    base.unknownInterface=&canonical;
    auto& s=state();g_protect=hook_fixture::protect;
    CHECK(trackMetadataLifetime(p)&&trackMetadataLifetime(c));
    auto release=[&] {
        LifetimeConditions out;std::lock_guard lock(s.mutex);
        releaseConditions_locked(out,p,false);releaseConditions_locked(out,c,true);return out;
    };
    const auto covered=release();
    CHECK((covered.value&(base_covered|canonical_covered|base_owner|canonical_owner))==
        (base_covered|canonical_covered|base_owner|canonical_owner));
    CHECK(!(covered.value&(base_pending|canonical_pending)));
    for(unsigned side=0;side<2;++side) {
        auto* table=side?ct.data():bt.data();auto& owner=s.objects.at(table);
        const auto pending=side?canonical_pending:base_pending;
        const auto coverage=side?canonical_covered:base_covered;
        const auto ownership=side?canonical_owner:base_owner;
        owner.pendingProtection[2]=PAGE_READONLY;
        auto changed=release();CHECK((changed.value^covered.value)==(pending|coverage));
        owner.pendingProtection[2]=0;
        s.lists[table].originals[2]=owner.originals[2]; // ambiguous dispatch owners
        changed=release();CHECK((changed.value^covered.value)==(ownership|coverage));
        s.lists.erase(table);
        const auto handler=table[2];table[2]=owner.originals[2];
        changed=release();CHECK((changed.value^covered.value)==coverage);table[2]=handler;
    }
    CHECK(release().value==covered.value);
    s.objects.erase(bt.data());s.objects.erase(ct.data());
    // Encoding independence for all bits, including creation/dependency bits
    // whose real-wrapper controls are in identityDiagnostics below.
    for(unsigned bit=0;bit<22;++bit) {
        LifetimeConditions value;value.set(1u<<bit,false);
        CHECK(value.known==(1u<<bit)&&value.value==0);
        value.set(1u<<bit,true);CHECK(value.value==(1u<<bit));
        value.set(1u<<bit,false);CHECK(value.known==(1u<<bit)&&value.value==0);
    }
}
// Synthetic cohort mutations test each component without scheduling or native COM.
static void orderingDiagnostics() {
    auto& s=state();auto& c=bindingCohort();
    CHECK(!s.metadataEvents&&c.active==0);
    FakeCOM object{nullptr},peer{nullptr};
    MetadataEvent other(reinterpret_cast<IUnknown*>(&peer),false);
    MetadataEvent event(reinterpret_cast<IUnknown*>(&object),false,"pso_create");
    std::lock_guard lock(s.mutex);
    event.ambiguous=other.ambiguous=false; // isolate each component below
    constexpr uint32_t all=0x7fff;
    auto probe=[&] {auto value=event.orderConditions(s);CHECK(value.known==all);return value.value;};
    CHECK(probe()==all&&event.distinct(s));
    auto fails=[&](unsigned bit) {CHECK(probe()==(all&~(1u<<bit)));CHECK(!event.distinct(s));};
    const auto serial=event.serial;event.serial=0;fails(0);event.serial=serial;
    auto* address=event.address;event.address=nullptr;fails(1);event.address=address;
    event.footprint.known=false;fails(2);event.footprint.known=true;
    const auto clock=c.clock;c.clock=UINT64_MAX;fails(3);c.clock=clock;
    c.lostThrough=event.footprint.begin;fails(4);c.lostThrough=0;
    event.ambiguous=true;fails(5);event.ambiguous=false;
    c.active=1;fails(6);c.active=0;
    other.footprint.known=false;fails(7);other.footprint.known=true;
    other.footprint.objects[0]=reinterpret_cast<uintptr_t>(&object);fails(8);
    other.footprint.objects[0]=reinterpret_cast<uintptr_t>(&peer);
    MetadataFootprint saved=c.completed[0];
    c.completed[0].end=event.footprint.begin+1;c.completed[0].known=false;fails(9);
    c.completed[0].known=true;c.completed[0].objects[0]=reinterpret_cast<uintptr_t>(&object);fails(10);
    c.completed[0]=saved;
    auto& r=c.records[0];auto savedRecord=std::move(r);r=BindingRecord{};r.used=true;r.known=true;
    r.thread=event.footprint.thread;r.active=true;c.active=1;fails(11);
    r.active=false;c.active=0;r.known=false;fails(12);r.known=true;
    r.object=reinterpret_cast<uintptr_t>(&object);fails(13);
    r.object=0;r.dependency=reinterpret_cast<uintptr_t>(&object);fails(13);
    r.dependency=0;BindingRange range;range.slots.heap.id=reinterpret_cast<uintptr_t>(&object);
    r.ranges.push_back(range);fails(13);r.ranges.clear();
    r.kind=diagnostics::DisableEvent::creation;r.object=reinterpret_cast<uintptr_t>(&peer);fails(14);
    r=std::move(savedRecord);
    CHECK(probe()==all&&event.distinct(s));
    // A bounded footprint still fails closed when more keys are observed.
    std::array<FakeCOM,21> extra{};
    for(auto& key:extra)event.touch(reinterpret_cast<IUnknown*>(&key));
    CHECK(event.footprint.overflow&&!event.footprint.known&&event.ambiguous);
    CHECK(!(event.orderConditions(s).value&(1u<<2))&&!event.distinct(s));
}

static void ambiguityDiagnostics() {
    auto& s=state();CHECK(!s.metadataEvents);
    FakeCOM object{nullptr},peer{nullptr};
    auto* a=reinterpret_cast<IUnknown*>(&object);
    auto* b=reinterpret_cast<IUnknown*>(&peer);
    // Decode the actual append-only encoding, including zero fields.
    auto fields=[&](const MetadataEvent& event) {
        char text[104]="prefix";
        {std::lock_guard lock(s.mutex);event.appendAmbiguity(text,sizeof text);}
        CHECK(std::strncmp(text,"prefix;ar=",10)==0);
        std::array<unsigned,10> result{};
        CHECK(std::sscanf(text+6,";ar=%3x;ak=%1x;mc=%2x%2x%2x%2x;mh=%2x%2x%2x%2x",
            &result[0],&result[1],&result[2],&result[3],&result[4],&result[5],
            &result[6],&result[7],&result[8],&result[9])==10);
        return result;
    };
    {
        MetadataEvent event(a,false,"pso_create");
        CHECK(fields(event)==(std::array<unsigned,10>{}));
        CHECK(event.distinct(s));
        // These refuse distinctness but never write the ambiguity flag.
        const auto serial=event.serial;event.serial=0;
        CHECK(!event.distinct(s)&&fields(event)[0]==0);event.serial=serial;
        auto& cohort=bindingCohort();const auto clock=cohort.clock;
        cohort.clock=UINT64_MAX;CHECK(!event.distinct(s)&&fields(event)[0]==0);cohort.clock=clock;
        const auto lost=cohort.lostThrough;cohort.lostThrough=event.footprint.begin;
        CHECK(!event.distinct(s)&&fields(event)[0]==0);cohort.lostThrough=lost;
        event.footprint.known=false;CHECK(!event.distinct(s)&&fields(event)[0]==0);
        event.footprint.known=true;CHECK(event.distinct(s)&&!event.ambiguous);
    }
    for(bool same:{true,false})for(bool conflict:{false,true}) {
        MetadataEvent event(a,false,"pso_create");
        std::unique_ptr<MetadataEvent> other;
        auto create=[&] {other=std::make_unique<MetadataEvent>(conflict?a:b,true);};
        if(same)create();else {std::thread worker(create);worker.join();}
        auto value=fields(event);
        const unsigned reasons=(same?MetadataEvent::reentry:0u)|
            (conflict?(same?MetadataEvent::active_same:MetadataEvent::active_other):0u);
        CHECK(value[0]==reasons&&value[1]==(reasons?1u:0u));
        CHECK(value[same?2:3]==1&&value[same?3:2]==0);
        CHECK(value[same?6:7]==(reasons?1u:0u));
        // Both sides retain the active-conflict reason and the other's kind.
        const auto reverse=fields(*other);
        CHECK(reverse[0]==reasons&&reverse[1]==(reasons?2u:0u));
        CHECK(event.distinct(s)==!bool(reasons));
        other.reset();
        value=fields(event);
        CHECK(value[0]==reasons&&value[2]==0&&value[3]==0);
        CHECK(value[same?4:5]==1&&value[same?5:4]==0);
        // This touch discovers an overlap after the other event has finished.
        // The completed writer is independent of the earlier active writer.
        {std::lock_guard lock(s.mutex);event.touch(conflict?a:b);}
        value=fields(event);
        CHECK(value[0]==(reasons|(same?MetadataEvent::completed_same:MetadataEvent::completed_other)));
        CHECK(value[1]==1&&value[same?8:9]==1);
        CHECK(event.ambiguous&&!event.distinct(s));
    }
    {
        MetadataEvent event(a,false,"pso_create");
        std::array<FakeCOM,21> extra{};
        {std::lock_guard lock(s.mutex);
         for(auto& key:extra)event.touch(reinterpret_cast<IUnknown*>(&key));}
        const auto value=fields(event);
        CHECK(value[0]==MetadataEvent::capacity&&value[1]==0);
        CHECK(event.ambiguous&&event.footprint.overflow&&!event.distinct(s));
    }
    for(unsigned mode=0;mode<5;++mode) {
        MetadataEvent event(a,false,"pso_create");
        BindingRecord r;r.known=true;r.thread=event.footprint.thread;
        r.active=mode==0;
        if(mode==1)r.object=reinterpret_cast<uintptr_t>(a);
        if(mode==2)r.dependency=reinterpret_cast<uintptr_t>(a);
        if(mode==3) {BindingRange range;range.slots.heap.id=reinterpret_cast<uintptr_t>(a);r.ranges.push_back(range);}
        if(mode==4) {r.thread^=1;r.object=reinterpret_cast<uintptr_t>(a);}
        {std::lock_guard lock(s.mutex);compareRootBinding(r);}
        const auto value=fields(event);
        CHECK(value[0]==(mode==0?MetadataEvent::binding_reentry:
            mode==4?MetadataEvent::binding_key_other:MetadataEvent::binding_key_same));
        CHECK(value[1]==0&&event.ambiguous&&!event.distinct(s));
    }
    // Release/other kind bits, retained even after the nested event retires.
    {
        MetadataEvent event(a,false,"pso_create");
        {MetadataEvent nested(b,false);}
        {MetadataEvent nested(b,false,"other");}
        {std::lock_guard lock(s.mutex);event.resolveNestedReleases();}
        const auto value=fields(event);
        CHECK(value[0]==MetadataEvent::reentry&&value[1]==12);
        CHECK(value[4]==2&&value[6]==1&&event.ambiguous);
        // Maximum encoded masks/high-water counts fit the bounded buffers.
        event.ambiguityReasons=511;event.ambiguityKinds=15;event.ambiguityHigh.fill(255);
        event.nestedReleases=255;
        event.nestedRefusals=0x1fff;event.nestedReasonCounts.fill(255);event.passedChildren=255;
        event.unregisteredSightings.fill(255);
        event.unavailableSightings.fill(255);
        const auto savedSightings=state().creationSightings;
        state().creationSightings.occupancy=state().creationSightings.highWater=CreationSightings::capacity;
        char text[208]{};PipelineInterfaces max;
        max.qiFailed=max.nullOutput=0xfff;max.identityMismatch=max.alias=max.ancestorCovered=0x3f;
        max.hrClasses=0xffffff;diagnostics::LifetimeConditions order;order.known=order.value=0x7fff;
        max.format(text,sizeof text,order);
        {std::lock_guard lock(s.mutex);event.appendAmbiguity(text,sizeof text);}
        CHECK(std::strlen(text)==179&&std::strstr(text,";ar=1ff;ak=f;mc=00000200;mh=ffffffff;nr=ff"));
        CHECK(std::strstr(text,";nf=1fff;nc=ffffffffffffffffffffffffff;np=ff"));
        CHECK(std::strstr(text,";np=ff;uc=ffffff"));
        CHECK(std::strstr(text,";uu=ffffffff;st=100010000000"));
        state().creationSightings=savedSightings;
        char context[256]{};
        std::snprintf(context,sizeof context,";pd=ffffff/ffffff;rd=ffffff/ffffff;bd=ffffff/ffffff%s",text);
        char full[diagnostics::text_capacity]{};
        const auto size=std::snprintf(full,sizeof full,
            "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven;"
            "cb10=4294967295;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;lp=3%s",context);
        CHECK(size>0&&size<int(sizeof full)&&std::strstr(full,text));
        auto maxRecord=diagnostics::record(diagnostics::Event::pso,UINT64_MAX,UINT64_MAX,UINT64_MAX,full);
        maxRecord.sequence=UINT64_MAX;
        CHECK(diagnostics::format_record(maxRecord).valid);
        // Ownership suffix retains the same context/text/serialized ceilings.
        const auto contextSize=std::strlen(context);
        CHECK(std::snprintf(context+contextSize,sizeof(context)-contextSize,";pt=ffff;px=ffff;pb=1")==21);
        CHECK(std::strlen(context)==251);
        const auto pinnedSize=std::snprintf(full,sizeof full,
            "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven;"
            "cb10=4294967295;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;lp=3%s",context);
        CHECK(pinnedSize==423&&pinnedSize<int(sizeof full));
        maxRecord=diagnostics::record(diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,full);
        maxRecord.sequence=UINT64_MAX;
        CHECK(diagnostics::format_record(maxRecord).valid);
    }
    CHECK(!s.metadataEvents);
}

static void nestedReleaseProof() {
    auto& s=state();CHECK(!s.metadataEvents);
    std::array<FakeCOM,4> objects{};
    auto key=[&](size_t n) {return reinterpret_cast<IUnknown*>(&objects[n]);};
    // Parent output is deliberately unknown until AFTER child retirement.
    // No certificate is inferred from identify() or disjoint raw addresses.
    for(unsigned mode=0;mode<11;++mode) {
        MetadataEvent parent(nullptr,false,"pso_create");
        {std::lock_guard lock(s.mutex);parent.touch(key(1));}
        {
            MetadataEvent child(mode==1?key(0):mode==2?key(1):mode==3?nullptr:key(2),
                mode==5,mode==5?"root_create":mode==6?"pso_create":mode==7?"other":"release");
            std::lock_guard lock(s.mutex);
            child.footprint.releaseComplete=mode!=3&&mode!=4;
            if(mode==8)child.footprint.overflow=true;
            if(mode==9)child.mark(MetadataEvent::active_other);
            if(mode==10) {
                BindingRecord r;r.active=true;r.thread=parent.footprint.thread;
                compareRootBinding(r);
            }
        }
        {std::lock_guard lock(s.mutex);
         parent.identify(key(0));
         CHECK(!parent.distinct(s)); // even disjoint children require resolution
         parent.resolveNestedReleases();}
        CHECK(parent.distinct(s)==(mode==0));
        CHECK(parent.ambiguous==(mode!=0));
        CHECK(parent.nestedReleases==((mode>=5&&mode<=7)?0u:1u));
        char text[104]{};
        {std::lock_guard lock(s.mutex);parent.appendAmbiguity(text,sizeof text);}
        CHECK(std::strstr(text,mode==0?";ar=000;ak=0":";ar="));
        CHECK(std::strstr(text,(mode>=5&&mode<=7)?";nr=00":";nr=01"));
    }
    // Missing history, an active child and overflow cannot resolve the proof.
    for(unsigned mode=0;mode<3;++mode) {
        MetadataEvent parent(key(0),false,"pso_create");
        auto child=std::make_unique<MetadataEvent>(key(2),false);
        child->footprint.releaseComplete=true;
        if(mode!=0)child.reset();
        {std::lock_guard lock(s.mutex);
         if(mode==1)bindingCohort().lostThrough=parent.footprint.begin;
         if(mode==2)parent.nestedReleases=255;
         parent.resolveNestedReleases();}
        CHECK(parent.ambiguous&&!parent.distinct(s));
    }
    // A peer that completed before the child began still overlaps the parent.
    // Child.touch cannot see it using the child's later begin clock.
    {
        MetadataEvent parent(key(0),false,"pso_create");
        std::thread peer([&] {MetadataEvent completed(key(2),false);});peer.join();
        {MetadataEvent child(key(2),false);child.footprint.releaseComplete=true;}
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.nestedReleases==1&&parent.ambiguous&&!parent.distinct(s));
    }
    CHECK(!s.metadataEvents);
}

static void nestedReleaseRefusalDiagnostics() {
    auto& s=state();CHECK(!s.metadataEvents);
    std::array<FakeCOM,4> objects{};
    auto key=[&](size_t n) {return reinterpret_cast<IUnknown*>(&objects[n]);};
    // Exercise each resolution predicate independently. Producer provenance
    // flags are also transported through the actual final-Release fixture below.
    for(unsigned mode=0;mode<23;++mode) {
        MetadataEvent parent(key(0),false,"pso_create");
        const auto savedClock=bindingCohort().clock;
        unsigned expected=0;
        if(mode!=1) {
            MetadataEvent child(mode==7?key(0):key(1),false);
            child.footprint.releaseComplete=true;
            switch(mode) {
            case 2: child.footprint.releaseRefusals=unregistered;
                child.footprint.releaseComplete=false;expected=unregistered|child_incomplete;break;
            case 3: child.footprint.releaseRefusals=unsupported_identity;
                child.footprint.releaseComplete=false;expected=unsupported_identity|child_incomplete;break;
            case 4: child.footprint.releaseRefusals=binding_owner;
                child.footprint.releaseComplete=false;expected=binding_owner|child_incomplete;break;
            case 5: child.footprint.releaseRefusals=native_activity;
                child.footprint.releaseComplete=false;expected=native_activity|child_incomplete;break;
            case 6: child.footprint.overflow=true;expected=certificate_overflow;break;
            case 7: expected=parent_key|child_ambiguity|child_incomplete;break;
            case 8: child.mark(MetadataEvent::active_other);
                expected=child_ambiguity|child_incomplete;break;
            case 14: child.footprint.releaseComplete=false;expected=child_incomplete;break;
            case 18: child.mark(MetadataEvent::binding_reentry);
                expected=child_ambiguity|child_incomplete;break;
            case 19: child.mark(MetadataEvent::capacity);
                expected=child_ambiguity|child_incomplete;break;
            case 20: child.footprint.known=false;expected=child_incomplete;break;
            case 21: child.serial=0;expected=clock_serial;break;
            case 22: child.mark(MetadataEvent::completed_other);
                expected=child_ambiguity|child_incomplete;break;
            }
        } else {
            // An unfinished child is audited but not retained as completed.
            MetadataEvent child(key(1),false);
            std::lock_guard lock(s.mutex);parent.resolveNestedReleases();
            expected=child_incomplete;
        }
        BindingRecord* binding=nullptr;
        {
            std::lock_guard lock(s.mutex);
            if(mode==9||mode==10||mode==15||mode==16) {
                auto& r=bindingCohort().records[0];CHECK(!r.used);
                // Reclamation leaves end/ranges behind. This synthetic peer is
                // unfinished, so it must not inherit an older cohort's end.
                r.end=parent.footprint.begin; // force the reclaimed-slot regression
                r=BindingRecord{};
                CHECK(!r.end&&r.ranges.count==0);
                r.used=true;r.known=mode!=10;r.begin=parent.footprint.begin;
                if(mode==9)r.object=reinterpret_cast<uintptr_t>(key(1));
                if(mode==15)r.dependency=reinterpret_cast<uintptr_t>(key(1));
                if(mode==16) {BindingRange range;range.slots.heap.id=reinterpret_cast<uintptr_t>(key(1));r.ranges.push_back(range);}
                binding=&r;expected=mode==10?unknown_peer:retained_key;
            }
            if(mode==11) {bindingCohort().lostThrough=parent.footprint.begin;expected=missing_history;}
            if(mode==12) {parent.serial=0;expected=clock_serial;}
            if(mode==13) {parent.footprint.known=false;expected=parent_incomplete;}
            if(mode==17) {bindingCohort().clock=UINT64_MAX;expected=clock_serial;}
            parent.resolveNestedReleases();
            if(parent.nestedRefusals!=expected)
                std::fprintf(stderr,"nested refusal mode=%u actual=%04x expected=%04x\n",
                    mode,parent.nestedRefusals,expected);
            CHECK(parent.nestedRefusals==expected);
            CHECK(parent.passedChildren==(expected?0u:1u));
            for(size_t i=0;i<parent.nestedReasonCounts.size();++i)
                CHECK(parent.nestedReasonCounts[i]==unsigned(bool(expected&(1u<<i))));
            const auto counts=parent.nestedReasonCounts;
            parent.resolveNestedReleases();CHECK(parent.nestedReasonCounts==counts);
            CHECK(parent.passedChildren==(expected?0u:1u)); // no duplicate counting
            if(binding)*binding=BindingRecord{};
            if(mode==17)bindingCohort().clock=savedClock+3;
        }
    }
    // Retained metadata keys and unknown peers, including peers whose end
    // precedes the child's begin, are separate from parent-key conflicts.
    for(bool unknown:{false,true}) {
        MetadataEvent parent(key(0),false,"pso_create");
        std::thread peer([&] {MetadataEvent e(unknown?nullptr:key(1),false);});peer.join();
        {MetadataEvent child(key(1),false);child.footprint.releaseComplete=true;}
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.nestedRefusals==(unknown?unknown_peer:retained_key));
        CHECK(parent.nestedReasonCounts[unknown?9:7]==1&&parent.passedChildren==0);
    }
    // Saturation is bounded even when certificates are no longer retained.
    {
        MetadataEvent parent(key(0),false,"pso_create");
        for(unsigned i=0;i<260;++i) {MetadataEvent child(key(1),false);}
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.nestedReleases==255&&parent.nestedReasonCounts[0]==255);
        CHECK(parent.nestedReasonCounts[5]==255&&parent.nestedReasonCounts[10]==255);
        CHECK(parent.passedChildren==0&&parent.ambiguous);
    }
    CHECK(!s.metadataEvents);
}

static void creationSightingDiagnostics() {
    auto& s=state();CHECK(!s.metadataEvents);
    const auto saved=s.creationSightings;s.creationSightings={};
    FakeCOM output{};void* raw=&output;
    observeCreationReturn(E_FAIL,&raw,CreationSightings::pso);
    observeCreationReturn(S_OK,nullptr,CreationSightings::pso);
    void* nullOutput=nullptr;observeCreationReturn(S_OK,&nullOutput,CreationSightings::root);
    CHECK(s.creationSightings.classify(reinterpret_cast<uintptr_t>(raw))==1);
    CHECK(!s.creationSightings.occupancy&&!s.creationSightings.highWater&&!s.creationSightings.evictions);
    observeCreationReturn(S_FALSE,&raw,CreationSightings::pso);
    observeCreationReturn(S_OK,&raw,CreationSightings::root);
    CHECK(s.creationSightings.classify(reinterpret_cast<uintptr_t>(raw))==0);
    CHECK(s.creationSightings.entries[0].seams==(CreationSightings::pso|CreationSightings::root));
    CHECK(s.creationSightings.occupancy==1&&s.creationSightings.highWater==1&&!s.creationSightings.evictions);
    s.creationSightings={};
    // Old 512-key limit no longer loses observations at the 513th key.
    for(uintptr_t i=1;i<=512;++i)s.creationSightings.observe(i,CreationSightings::list);
    CHECK(!s.creationSightings.overflow&&s.creationSightings.classify(513)==1);
    s.creationSightings.observe(513,CreationSightings::pso);
    CHECK(!s.creationSightings.overflow&&s.creationSightings.classify(513)==0);
    CHECK(s.creationSightings.occupancy==513&&s.creationSightings.highWater==513&&!s.creationSightings.evictions);
    for(uintptr_t i=514;i<=CreationSightings::capacity;++i)s.creationSightings.observe(i,CreationSightings::list);
    CHECK(!s.creationSightings.overflow&&s.creationSightings.classify(CreationSightings::capacity+1)==1);
    s.creationSightings.observe(CreationSightings::capacity+1,CreationSightings::pso);
    CHECK(s.creationSightings.overflow&&s.creationSightings.classify(CreationSightings::capacity+1)==2);
    CHECK(s.creationSightings.occupancy==CreationSightings::capacity&&s.creationSightings.highWater==CreationSightings::capacity);
    CHECK(!s.creationSightings.evictions); // refusal at capacity is not an eviction
    CHECK(s.creationSightings.classify(1)==0&&s.creationSightings.classify(0)==2);
    // Historical sightings survive address reuse but convey no lifetime proof.
    s.creationSightings.observe(1,CreationSightings::root);
    CHECK(s.creationSightings.entries[0].seams==(CreationSightings::list|CreationSightings::root));
    CHECK(s.creationSightings.occupancy==CreationSightings::capacity&&!s.creationSightings.evictions);
    std::array<FakeCOM,3> objects{};
    for(unsigned category=0;category<3;++category) {
        MetadataEvent parent(reinterpret_cast<IUnknown*>(&objects[0]),false,"pso_create");
        parent.unregisteredSightings[category]=254;
        for(unsigned i=1;i<3;++i) {
            MetadataEvent child(reinterpret_cast<IUnknown*>(&objects[i]),false);
            child.footprint.releaseRefusals=unregistered;
            child.footprint.creationSighting=category;
        }
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.nestedRefusals==(unregistered|child_incomplete));
        CHECK(parent.nestedReasonCounts[0]==2&&parent.nestedReasonCounts[1]==2);
        CHECK(parent.unregisteredSightings[category]==255&&parent.passedChildren==0);
        CHECK(parent.ambiguous&&!parent.distinct(s));
        const auto counts=parent.unregisteredSightings;
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.unregisteredSightings==counts);
        for(unsigned other=0;other<3;++other)if(other!=category)CHECK(!counts[other]);
    }
    // Every unavailable cause is independent, saturated, and resolved once.
    // No generation token exists in this address-only table. The mismatch bit
    // diagnoses an impossible/exhausted cohort snapshot clock, never identity.
    for(unsigned mode=0;mode<8;++mode) {
        MetadataEvent parent(reinterpret_cast<IUnknown*>(&objects[0]),false,"pso_create");
        unsigned causes=mode<4?(1u<<mode):mode==4?5u:mode==6?4u:8u;
        parent.unavailableSightings.fill(254);
        for(unsigned i=1;i<3;++i) {
            MetadataEvent child(reinterpret_cast<IUnknown*>(&objects[i]),false);
            child.footprint.releaseRefusals=unregistered;
            child.footprint.creationSighting=mode==5?99u:2u;
            child.footprint.sightingUnavailable=(mode==2||mode==5||mode==6||mode==7)?0u:mode==4?1u:causes;
            child.footprint.sightingClock=(mode==2||mode==4)?UINT64_MAX:
                mode==6?child.footprint.begin+1:0;
        }
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        for(size_t i=0;i<4;++i)CHECK(parent.unavailableSightings[i]==((causes&(1u<<i))?255u:254u));
        CHECK(parent.unregisteredSightings[2]==2&&parent.passedChildren==0);
        CHECK(parent.nestedRefusals==(unregistered|child_incomplete)&&parent.ambiguous&&!parent.distinct(s));
        const auto counts=parent.unavailableSightings;
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.unavailableSightings==counts);
    }
    CHECK(!s.metadataEvents);s.creationSightings=saved;
}

// J-111 counter-example: zero on an independently counted tear-off does not
// establish destruction of its canonical COM identity. This models only the
// relevant QI/AddRef/Release seams, not a complete native D3D12 implementation.
static HRESULT STDMETHODCALLTYPE tearoffIdentityQuery(IUnknown* self,REFIID iid,void** out) {
    CHECK(!mutex_depth);
    *out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
    auto* value=reinterpret_cast<FakeCOM*>(self);
    if(value->unknownInterface)value=value->unknownInterface;
    ++value->refs;*out=value;return S_OK;
}
static ULONG STDMETHODCALLTYPE tearoffRelease(IUnknown* self) {
    CHECK(!mutex_depth);
    auto* value=reinterpret_cast<FakeCOM*>(self);
    const auto refs=--value->refs;
    // A tear-off owns one reference to its canonical object until it retires.
    if(!refs&&value->unknownInterface)
        reinterpret_cast<IUnknown*>(value->unknownInterface)->Release();
    return refs;
}
static void untrackedTearoffRefusal() {
    auto& s=state();CHECK(!s.metadataEvents);
    const auto savedSightings=s.creationSightings;s.creationSightings={};
    auto table=hook_fixture::table();
    table[0]=reinterpret_cast<void*>(&tearoffIdentityQuery);
    table[2]=reinterpret_cast<void*>(&tearoffRelease);
    FakeCOM canonical{table.data()},tearoff{table.data()};
    canonical.refs=2; // caller + tear-off's owned canonical reference
    // owner remains null: the tear-off has its own interface reference count.
    tearoff.unknownInterface=&canonical;tearoff.rootInterface=&canonical;
    auto* identity=reinterpret_cast<IUnknown*>(&canonical);
    auto* alias=reinterpret_cast<IUnknown*>(&tearoff);
    CHECK(trackMetadataLifetime(identity)); // table coverage is shared
    IUnknown* observed=nullptr;
    CHECK(alias->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&observed))==S_OK);
    CHECK(observed==identity&&canonical.refs==3);
    CHECK(observed->Release()==2);
    CHECK(!s.creationSightings.overflow&&!s.creationSightings.evictions);
    CHECK(s.creationSightings.classify(reinterpret_cast<uintptr_t>(alias))==1);
    CHECK(pipelineFor_locked(alias)==s.pipelines.end()&&!s.rootKeys.count(alias));
    CHECK(!s.nativeBindings.heap_token(reinterpret_cast<uintptr_t>(alias)).id);
    CHECK(!s.nativeBindings.resource_token(reinterpret_cast<uintptr_t>(alias)).id);
    CHECK(!s.listKeys.count(alias));
    {
        MetadataEvent parent(nullptr,false,"pso_create");
        // The output/canonical identity is not published until after this call.
        // Do not query or dereference the logically retired alias afterwards.
        CHECK(alias->Release()==0);
        CHECK(canonical.refs==1);
        void* output=identity;observeCreationReturn(S_OK,&output,CreationSightings::pso);
        {std::lock_guard lock(s.mutex);
         parent.identify(identity);
         parent.resolveNestedReleases();}
        CHECK(parent.nestedReleases==1&&parent.unregisteredSightings[1]==1);
        CHECK(parent.nestedRefusals==(unregistered|child_incomplete));
        CHECK(!(parent.nestedRefusals&(parent_key|retained_key|binding_owner|native_activity)));
        CHECK(parent.passedChildren==0&&parent.provenNestedReleases==0);
        CHECK(parent.ambiguous&&!parent.distinct(s));
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.unregisteredSightings[1]==1&&parent.passedChildren==0);
    }
    // Stack fake storage remains; only its simulated interface lifetime ended.
    // Restore the patched slot before discarding this table/dispatch owner.
    table[2]=s.objects.at(table.data()).originals[2];s.objects.erase(table.data());
    s.creationSightings=savedSightings;
    CHECK(!s.metadataEvents&&canonical.refs==1);
}

// J-112 rejection fixture: SEH is not an ownership transaction. Model a fault
// after QI acquires a canonical reference but before it publishes the pointer.
// This is a fault-path counter-model, not a compliant successful QI or evidence
// that any live object behaves this way. Never raise an actual access violation.
static constexpr DWORD identityObservationFault=0xe0420112u;
static unsigned identityObservationQueries=0;
static HRESULT STDMETHODCALLTYPE interruptedIdentityQuery(IUnknown* self,REFIID iid,void** out) {
    CHECK(!mutex_depth);
    ++identityObservationQueries;
    *out=nullptr;
    if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
    auto* value=reinterpret_cast<FakeCOM*>(self);
    CHECK(value->refs==1&&value->unknownInterface);
    ++value->unknownInterface->refs; // acquired, not yet published to *out
    RaiseException(identityObservationFault,0,0,nullptr);
    return E_FAIL;
}
// No C++ objects requiring unwinding, allocations or hook locks in the guard.
// Catch only our simulated fault; unexpected exceptions must remain failures.
static DWORD guardedInterruptedIdentityQuery(IUnknown* self,IUnknown** out) {
    *out=nullptr;
    __try {
        self->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(out));
    } __except(GetExceptionCode()==identityObservationFault?
               EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return identityObservationFault;
    }
    return 0;
}
static void rejectedIdentityObservationOwnership() {
    auto& s=state();CHECK(!s.metadataEvents);
    const auto savedSightings=s.creationSightings;s.creationSightings={};
    auto table=hook_fixture::table();
    table[0]=reinterpret_cast<void*>(&interruptedIdentityQuery);
    table[2]=reinterpret_cast<void*>(&tearoffRelease);
    FakeCOM canonical{table.data()},aliasStorage{table.data()};
    canonical.refs=2; // caller plus tear-off, before the speculative observation
    aliasStorage.unknownInterface=&canonical;
    auto* identity=reinterpret_cast<IUnknown*>(&canonical);
    auto* alias=reinterpret_cast<IUnknown*>(&aliasStorage);
    CHECK(trackMetadataLifetime(identity));
    CHECK(pipelineFor_locked(alias)==s.pipelines.end()&&!s.rootKeys.count(alias));
    CHECK(s.creationSightings.classify(reinterpret_cast<uintptr_t>(alias))==1);
    CHECK(!s.creationSightings.overflow);
    identityObservationQueries=0;
    {
        MetadataEvent parent(nullptr,false,"pso_create");
        IUnknown* output=nullptr;
        // Exactly one pre-destruction call, with a valid caller-owned alias.
        CHECK(guardedInterruptedIdentityQuery(alias,&output)==identityObservationFault);
        CHECK(identityObservationQueries==1&&output==nullptr);
        CHECK(aliasStorage.refs==1&&canonical.refs==3);
        // No returned pointer exists to balance. Releasing self would consume
        // the caller's reference, not the missing canonical QI reference.
        CHECK(alias->Release()==0);
        CHECK(identityObservationQueries==1); // production Release adds no QI
        CHECK(canonical.refs==2); // without the observation it would be one
        {std::lock_guard lock(s.mutex);
         parent.identify(identity);parent.resolveNestedReleases();}
        CHECK(parent.nestedReleases==1&&parent.unregisteredSightings[1]==1);
        CHECK(parent.nestedRefusals==(unregistered|child_incomplete));
        CHECK(parent.ambiguous&&!parent.distinct(s));
        CHECK(parent.passedChildren==0&&parent.provenNestedReleases==0);
        CHECK(!Config{}.capture_enabled());
    }
    // Only this fake's owner knows where the hidden reference went. Restore
    // the fixture, outside the guard, via the saved native Release dispatch.
    // A production observer has neither this knowledge nor a safe rollback.
    using Release=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
    auto nativeRelease=reinterpret_cast<Release>(s.objects.at(table.data()).originals[2]);
    CHECK(nativeRelease(identity)==1);
    table[2]=s.objects.at(table.data()).originals[2];s.objects.erase(table.data());
    s.creationSightings=savedSightings;
    CHECK(!s.metadataEvents&&canonical.refs==1);
}

static void* deniedPipelineSlot=nullptr;
static BOOL WINAPI pipelineProtect(LPVOID address,SIZE_T bytes,DWORD flags,DWORD* old) {
    if(address==deniedPipelineSlot) {*old=PAGE_READONLY;return FALSE;}
    return hook_fixture::protect(address,bytes,flags,old);
}
static UINT forwarded[5]{};static unsigned nativeDraws=0,clockChecks=0;
static void (*drawNativeProbe)(ID3D12GraphicsCommandList*)=nullptr;
static uint64_t clock() noexcept {++clockChecks;CHECK(mutex_depth==1);return tick;}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* list,UINT n,UINT i,UINT start,UINT instance) {
    CHECK(!mutex_depth);if(drawNativeProbe)drawNativeProbe(list);++nativeDraws;forwarded[0]=n;forwarded[1]=i;forwarded[2]=start;forwarded[3]=0;forwarded[4]=instance;
}
static void STDMETHODCALLTYPE indexed(ID3D12GraphicsCommandList* list,UINT n,UINT i,UINT start,INT base,UINT instance) {
    CHECK(!mutex_depth);if(drawNativeProbe)drawNativeProbe(list);++nativeDraws;forwarded[0]=n;forwarded[1]=i;forwarded[2]=start;forwarded[3]=UINT(base);forwarded[4]=instance;
}
static void STDMETHODCALLTYPE pso(ID3D12GraphicsCommandList*,ID3D12PipelineState*) {CHECK(!mutex_depth);}
static void STDMETHODCALLTYPE root(ID3D12GraphicsCommandList*,ID3D12RootSignature*) {CHECK(!mutex_depth);}
static void STDMETHODCALLTYPE heaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*) {CHECK(!mutex_depth);}
static void STDMETHODCALLTYPE table(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE) {CHECK(!mutex_depth);}
static void (*constantNativeProbe)(ID3D12GraphicsCommandList*)=nullptr;
static void STDMETHODCALLTYPE constant(ID3D12GraphicsCommandList* list,UINT,UINT,UINT) {
    CHECK(!mutex_depth);if(constantNativeProbe)constantNativeProbe(list);
}
static void STDMETHODCALLTYPE constants(ID3D12GraphicsCommandList* list,UINT,UINT,const void*,UINT) {
    CHECK(!mutex_depth);if(constantNativeProbe)constantNativeProbe(list);
}
static void STDMETHODCALLTYPE bundleCall(ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*) {CHECK(!mutex_depth);}
static void STDMETHODCALLTYPE indirectCall(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64) {CHECK(!mutex_depth);}
static UINT addressParameter=0;static uint64_t addressValue=0;
static void STDMETHODCALLTYPE address(ID3D12GraphicsCommandList*,UINT parameter,D3D12_GPU_VIRTUAL_ADDRESS value) {
    CHECK(!mutex_depth);addressParameter=parameter;addressValue=value;
}
static void STDMETHODCALLTYPE vertices(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*) {CHECK(!mutex_depth);}
static void STDMETHODCALLTYPE indices(ID3D12GraphicsCommandList*,const D3D12_INDEX_BUFFER_VIEW*) {CHECK(!mutex_depth);}
static HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*){CHECK(!mutex_depth);return S_OK;}
static ID3D12Device* listDevice=nullptr;
static HRESULT STDMETHODCALLTYPE getListDevice(ID3D12GraphicsCommandList*,REFIID,void** out) {
    CHECK(!mutex_depth);*out=listDevice;if(!listDevice)return E_NOINTERFACE;listDevice->AddRef();return S_OK;
}
static void STDMETHODCALLTYPE topology(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY){CHECK(!mutex_depth);}
static void ownershipRetirementCertificates(ID3D12GraphicsCommandList* list,IUnknown* pinnedAddress) {
    auto& s=state();CHECK(!s.metadataEvents);
    for(unsigned mode=0;mode<14;++mode) {
        FakeCOM parentObject{},childObject{},aliasObject{};
        auto* parentKey=reinterpret_cast<IUnknown*>(&parentObject);
        auto* childKey=mode==9?pinnedAddress:reinterpret_cast<IUnknown*>(&childObject);
        auto* aliasKey=reinterpret_cast<IUnknown*>(&aliasObject);
        MetadataEvent parent(parentKey,false,"pso_create");parent.ownershipPinned=true;
        {
            MetadataEvent child(childKey,false);
            child.footprint.ownershipCandidate=mode!=1;
            child.footprint.releaseRefusals=unregistered;
            if(mode==2)child.footprint.releaseRefusals|=native_activity;
            if(mode==3)child.footprint.releaseRefusals|=binding_owner;
            if(mode==4)child.footprint.releaseRefusals|=child_ambiguity;
            if(mode==5)child.footprint.overflow=true;
            if(mode==6)child.footprint.known=false;
        }
        if(mode==7)s.listKeys[childKey]=list;
        if(mode==8)s.rootKeys[aliasKey]=reinterpret_cast<ID3D12RootSignature*>(childKey);
        const auto lost=bindingCohort().lostThrough;
        if(mode==10)bindingCohort().lostThrough=parent.footprint.begin;
        if(mode==11)parent.mark(MetadataEvent::binding_reentry);
        if(mode==12)parent.serial=0;
        if(mode==13)parent.footprint.known=false;
        {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
        CHECK(parent.distinct(s)==(mode==0));
        CHECK(parent.ownershipChildren==unsigned(mode==0||mode==11));
        if(mode==11)CHECK(parent.ambiguous&&(parent.ambiguityReasons&MetadataEvent::binding_reentry));
        if(mode==7)s.listKeys.erase(childKey);
        if(mode==8)s.rootKeys.erase(aliasKey);
        bindingCohort().lostThrough=lost;
    }
    // An unfinished child cannot be accepted by the ownership alternative.
    FakeCOM parentObject{},childObject{};
    MetadataEvent parent(reinterpret_cast<IUnknown*>(&parentObject),false,"pso_create");parent.ownershipPinned=true;
    MetadataEvent child(reinterpret_cast<IUnknown*>(&childObject),false);
    child.footprint.ownershipCandidate=true;child.footprint.releaseRefusals=unregistered;
    {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
    CHECK(parent.ambiguous&&!parent.distinct(s)&&!parent.ownershipChildren);
}
// Only the retained root base uses this vtable; PSO validation is unaffected.
static unsigned rootPinMode=0,rootPinCalls=0;
static HRESULT STDMETHODCALLTYPE rootPinQuery(IUnknown* self,REFIID iid,void** out) {
    CHECK(!mutex_depth&&iid==__uuidof(IUnknown));++rootPinCalls;
    *out=nullptr;
    if(rootPinMode==1)return E_NOINTERFACE;
    if(rootPinMode==2){*out=self;self->AddRef();return S_OK;}
    if(rootPinMode==3)return S_OK;
    const auto hr=hook_fixture::query(self,iid,out);
    if(rootPinMode==4)throw std::bad_alloc(); // published AddRef, unknown completion
    return rootPinMode==5?E_FAIL:hr;
}
// J-113 actual successful creation, real fake-COM Release hooks and Draw path.
static void pinRefusalDiagnostics(TestPaths& paths,ID3D12Device* d,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    auto& s=state();using R=PinRefusals;
    CHECK(!s.pinsTaken&&!s.pinsCapRefused);
    s.pinRefusals={};
    // Real successful creation wrapper: independently fail each front gate.
    const unsigned expected[]={R::hash,R::shape,R::validation,R::seams,R::native_return};
    for(unsigned mode=0;mode<std::size(expected);++mode) {
        auto table=hook_fixture::table();FakeCOM object{table.data()};
        auto* p=reinterpret_cast<ID3D12PipelineState*>(&object);
        MemoryIO io;io.readState=ReadState::ok;
        io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
        Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
        const auto vs=desc.VS;if(mode==0)desc.VS={};
        const auto iid=mode==1?unknownIID:__uuidof(ID3D12PipelineState);
        hook_fixture::output=&object;hook_fixture::psoIID=iid;
        hook_fixture::queryMode=mode==2?2:0;
        if(mode==3)hook_fixture::failProtect=hook_fixture::protects+1;
        hook_fixture::result=mode==4?E_FAIL:S_FALSE;
        void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,iid,&out)==hook_fixture::result&&out==p);
        hook_fixture::failProtect=0;hook_fixture::queryMode=0;
        desc.VS=vs;hook_fixture::result=S_FALSE;
        CHECK(!s.pinsTaken&&!s.pinsCapRefused);
        CHECK(s.pinRefusals.sticky&expected[mode]);
        CHECK(s.pinRefusals.counts[mode==4?10:mode]==1);
        char suffix[16]{};std::snprintf(suffix,sizeof suffix,";pf=%03x",expected[mode]);
        log.flush();CHECK(io.evidence.find(suffix)!=io.evidence.npos);
        CHECK(io.evidence.find(";pt=0000;px=0000;pf=")!=io.evidence.npos);
        CHECK(p->Release()==1);s.pipelines.erase(p);s.gs.erase(p);
        CHECK(p->Release()==0);s.objects.erase(table.data());set_sink(nullptr);
    }
    hook_fixture::psoIID=__uuidof(ID3D12PipelineState);
    // No COM is needed to explain a root gate refusal. Poison addresses in
    // these records must never be dereferenced by the pin path.
    auto table=hook_fixture::table();FakeCOM object{table.data()},canonical{table.data()};
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&object);
    auto* r=desc.pRootSignature;
    auto root=retain(r);const auto refs=reinterpret_cast<FakeCOM*>(r)->refs;
    auto attempt=[&](unsigned expected,unsigned count=1) {
        unsigned flags=0;
        CHECK(!pinCreatedPipeline(p,1,reinterpret_cast<IUnknown*>(p),{},root,count,flags));
        CHECK(flags==expected&&!s.pinsTaken&&!s.pinsCapRefused);
        CHECK(object.refs==1&&canonical.refs==1&&reinterpret_cast<FakeCOM*>(r)->refs==refs);
        return flags;
    };
    set_sink(nullptr);attempt(R::switch_off);
    MemoryIO off;off.readState=ReadState::ok;off.config="log_enabled=true";
    Log offLog(paths,off,clockNow,true);offLog.flush();set_sink(&offLog);
    const auto before=s.pinRefusals.counts;
    attempt(R::switch_off);CHECK(s.pinRefusals.counts==before);
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
    attempt(R::reference,0);
    const auto key=s.rootKeys.at(r);
    s.rootKeys.erase(r);attempt(R::provenance);s.rootKeys[r]=key;
    auto& creation=s.rootCreations.at(key);const auto token=creation.identity;
    creation.identity=0;attempt(R::provenance);creation.identity=token;
    creation.interfaceAddress=nullptr;attempt(R::creation_interface);creation.interfaceAddress=r;
    set_sink(nullptr);root.reset();
    // Sticky, per-reason saturating counts and append-only formatting, including
    // switch-off's diagnostic classification (publishing is suppressed OFF).
    R counters;
    for(unsigned bit=0;bit<12;++bit) {
        counters.record(1u<<bit);
        CHECK(counters.counts[bit]==1&&counters.sticky==((1u<<(bit+1))-1));
    }
    counters.counts.fill(65534);counters.record(0xfff);counters.record(0xfff);
    for(auto count:counters.counts)CHECK(count==65535);
    char text[text_capacity]{};
    // Maximal old PSO summary (no Draw/bd fields) remains a byte-exact prefix.
    std::string prefix=";pd=ffffff/ffffff;rd=ffffff/ffffff";
    prefix.append(179,'x');prefix+=";pt=ffff;px=ffff";
    counters.qiAttempts=counters.qiSuccesses=65535;
    counters.record(R::root_qi);CHECK(counters.sticky==0xfff);
    std::snprintf(text,sizeof text,"%s",prefix.c_str());counters.append(text,sizeof text,0x1fff);
    CHECK(std::string_view(text).substr(0,prefix.size())==prefix);
    CHECK(std::strlen(text)==prefix.size()+87);
    CHECK(std::strstr(text,";pf=fff;ps=fff;pn=ffffffffffffffffffffffffffffffffffffffffffffffff"));
    CHECK(std::strstr(text,";rq=1;ra=ffff;rs=ffff"));
    const auto oldSize=std::strlen(text);
    CHECK(std::snprintf(text+oldSize,sizeof(text)-oldSize,";pu=ffff;ph=ffff;cr=ffff;cw=ffff")==32);
    CHECK(std::strlen(text)==oldSize+32&&std::strlen(text)<sizeof(text));
    RootConstantCounters witnessCounters;witnessCounters.reasons.fill(65535);witnessCounters.values=16;
    witnessCounters.append(text,sizeof text);CHECK(std::strlen(text)==oldSize+32+54);
    auto record=diagnostics::record(Event::pso,UINT64_MAX,UINT64_MAX,UINT64_MAX,text);
    record.sequence=UINT64_MAX;CHECK(format_record(record).valid);
    // The one generic observation bit covers a swallowed pre-pin exception.
    // It is exercised through the real wrapper, rather than just arithmetic.
    MemoryIO exceptionIO;exceptionIO.readState=ReadState::ok;
    exceptionIO.config="log_enabled=true\ndiagnostic_ownership_pins=true";
    Log exceptionLog(paths,exceptionIO,clockNow,true);exceptionLog.flush();set_sink(&exceptionLog);
    hook_fixture::output=&object;hook_fixture::queryMode=10;void* out=nullptr;
    CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE);
    hook_fixture::queryMode=0;exceptionLog.flush();
    CHECK(exceptionIO.evidence.find(";pf=800")!=exceptionIO.evidence.npos);
    CHECK(p->Release()==1);s.pipelines.erase(p);s.gs.erase(p);CHECK(p->Release()==0);
    s.objects.erase(table.data());set_sink(nullptr);s.pinRefusals={};
}

// Fixture owners alone balance deliberately leaked pins after all assertions.
static void ownershipPinFixtures(TestPaths& paths,ID3D12Device* d,ID3D12GraphicsCommandList* l,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    auto& s=state();CHECK(s.pinsTaken==0&&s.pinsCapRefused==0);
    auto bt=hook_fixture::table(),ct=hook_fixture::table(),at=hook_fixture::table(),rt=hook_fixture::table();
    bt[0]=ct[0]=reinterpret_cast<void*>(&conditionQuery);
    std::array<FakeCOM,17> bases{},canonicals{};
    std::array<std::array<FakeCOM,6>,17> ancestors{};
    const auto rootRefs=reinterpret_cast<FakeCOM*>(desc.pRootSignature)->refs;
    for(unsigned mode=0;mode<17;++mode) {
        auto& base=bases[mode];auto& canonical=canonicals[mode];
        base.table=bt.data();canonical.table=ct.data();canonical.owner=&base;
        base.rootInterface=canonical.rootInterface=&base;
        base.unknownInterface=canonical.unknownInterface=&canonical;
        auto* p=reinterpret_cast<ID3D12PipelineState*>(&base);
        ancestorBase=p;ancestorCanonical=reinterpret_cast<IUnknown*>(&canonical);
        for(size_t i=0;i<6;++i) {
            auto& a=ancestors[mode][i];a.table=at.data();a.owner=&base;
            a.rootInterface=&base;a.unknownInterface=&canonical;ancestorOutputs[i]=reinterpret_cast<IUnknown*>(&a);
        }
        FakeCOM retired{rt.data()},peer{rt.data()};
        auto* child=reinterpret_cast<IUnknown*>(&retired);
        CHECK(trackMetadataLifetime(child));
        MemoryIO io;io.readState=ReadState::ok;
        io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
        Log log(paths,io,clockNow,true);log.flush();CHECK(log.ownership_pins());set_sink(&log);
        if(mode!=2)hook_fixture::psoAction=[&] {
            if(mode==1)deviceReleaseAction=[&] {MetadataEvent nested(reinterpret_cast<IUnknown*>(&peer),false,"other");};
            CHECK(child->Release()==0);
        };
        hook_fixture::output=&base;hook_fixture::psoIID=__uuidof(ID3D12PipelineState);void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE&&out==p);
        const auto& record=s.pipelines.at(p);
        const bool pinned=mode<16,accepted=mode!=1&&mode<16;
        CHECK(bool(record.ownershipPin)==pinned);
        CHECK(bool(record.conditions.value&diagnostics::pinned)==pinned);
        CHECK(bool(record.root_identity)==accepted);
        // Eight distinct addresses, each +1; temporary creation QIs are balanced.
        CHECK(base.refs==2+(pinned?8:0));CHECK(p->Release()==1+(pinned?8:0));
        if(pinned) {
            const auto& pin=s.ownershipPins[record.ownershipPin-1];CHECK(pin.complete&&pin.rootCount>0);
            for(size_t i=0;i<8;++i)CHECK(pin.interfaces[i]&&pin.counts[i]>0);
        }
        CHECK(s.pinsTaken==(std::min)(mode+1,16u));CHECK(s.pinsCapRefused==unsigned(mode==16));
        if(mode==16)CHECK(s.pinRefusals.sticky&PinRefusals::cap);
        l->SetPipelineState(p);l->SetGraphicsRootSignature(desc.pRootSignature);
        s.requirements_attempts=0;s.requirements_last_us=0;tick=0;
        const auto calls=io.calls;const auto draws=nativeDraws;
        l->DrawIndexedInstanced(9,1,0,0,0);
        CHECK(io.calls==calls&&nativeDraws==draws+1);
        CHECK(!s.states.at(l).bindings.pipeline.requirements_proven&&!capture_enablement::decide({}).enabled);
        log.flush();
        CHECK(io.evidence.find(accepted?"rejected:missing t10 root parameter observation":
            "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven")!=io.evidence.npos);
        CHECK(io.evidence.find(accepted?";pb=1":";pb=0")!=io.evidence.npos);
        if(mode==0) {
            CHECK(record.ownershipRequired&&std::strstr(record.detail.data(),";nr=01"));
            CHECK(std::strstr(record.detail.data(),";ar=000;ak=0"));
            // Pointer equality with the actual bound PSO is checked at Draw.
            auto& pin=s.ownershipPins[record.ownershipPin-1];auto* savedBase=pin.base;
            pin.base=nullptr;s.requirements_attempts=0;s.requirements_last_us=0;
            l->DrawIndexedInstanced(9,1,0,0,0);log.flush();
            CHECK(io.evidence.find("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven")!=io.evidence.npos);
            CHECK(io.evidence.find(";pb=0")!=io.evidence.npos);
            CHECK(!s.states.at(l).bindings.pipeline.requirements_proven);
            pin.base=savedBase;
            // A saved retirement at a pinned address remains refused. This is
            // a synthetic contradictory snapshot, not a real final pinned Release.
            MetadataEvent parent(reinterpret_cast<IUnknown*>(p),false,"pso_create");
            parent.ownershipPinned=true;
            {MetadataEvent childEvent(reinterpret_cast<IUnknown*>(p),false);
             childEvent.footprint.ownershipCandidate=true;childEvent.footprint.releaseRefusals=unregistered;}
            {std::lock_guard lock(s.mutex);parent.resolveNestedReleases();}
            CHECK(parent.ambiguous&&!parent.distinct(s)&&(parent.nestedRefusals&parent_key));
        }
        CHECK(l->Reset(nullptr,nullptr)==S_OK);
        CHECK(base.refs==1+(pinned?8:0)); // proxy never consumed a pin
        if(mode==0||mode==15) {
            // Repeated creation shares the complete pin even at capacity.
            // Do not retain a reference to the replaced map record.
            const auto oldIdentity=record.identity;
            const auto oldPin=record.ownershipPin;
            const auto refs=base.refs,rootCount=reinterpret_cast<FakeCOM*>(desc.pRootSignature)->refs;
            const auto qi=s.pinRefusals.qiAttempts;
            hook_fixture::psoAction={};out=nullptr;
            CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE&&out==p);
            const auto& repeated=s.pipelines.at(p);
            CHECK(repeated.identity!=oldIdentity&&repeated.ownershipPin==oldPin);
            CHECK(repeated.ownershipIdentity==s.ownershipPins[oldPin-1].identity);
            CHECK(s.pinsTaken==mode+1&&s.pinsCapRefused==0&&s.pinDedupeHits==unsigned(mode==0?1:2));
            CHECK(s.pinRefusals.qiAttempts==qi&&base.refs==refs+1);
            CHECK(p->Release()==refs);
            CHECK(reinterpret_cast<FakeCOM*>(desc.pRootSignature)->refs==rootCount);
            CHECK(pinnedPipeline_locked(repeated,p));
            if(mode==0) {
                FakeCOM newAlias{};unsigned flags=0;
                CHECK(!pinCreatedPipeline(p,repeated.identity,reinterpret_cast<IUnknown*>(&newAlias),{},
                    repeated.root,1,flags));
                CHECK(flags==PinRefusals::reference&&s.pinsTaken==1&&s.pinDedupeHits==1);
            }
            log.flush();
            CHECK(io.evidence.find(mode==0?";pu=0001;ph=0001":";pu=0010;ph=0002")!=io.evidence.npos);
        }
        if(!pinned)CHECK(p->Release()==0); // ordinary unpinned lifetime unchanged
        set_sink(nullptr);
    }
    CHECK(s.pinsTaken==16&&s.pinsCapRefused==1&&s.pinDedupeHits==2);
    ownershipRetirementCertificates(l,reinterpret_cast<IUnknown*>(&bases[0]));
    // Clear records without PSO Release; fixture-held root sharing is released
    // outside the mutex. Production has no such reset/recycle/cleanup path.
    for(auto& base:bases) {
        auto* p=reinterpret_cast<ID3D12PipelineState*>(&base);
        s.pipelines.erase(p);s.gs.erase(p);
    }
    for(auto& pin:s.ownershipPins) {
        CHECK(pin.complete);
        auto* base=reinterpret_cast<FakeCOM*>(pin.base);
        CHECK(base->refs==9);
        for(auto* address:pin.interfaces)if(address)fakeRelease(address);
        CHECK(base->refs==1);pin=OwnershipPin{};
    }
    for(unsigned i=0;i<16;++i)CHECK(fakeRelease(reinterpret_cast<IUnknown*>(&bases[i]))==0);
    CHECK(reinterpret_cast<FakeCOM*>(desc.pRootSignature)->refs==rootRefs);
    s.pinsTaken=s.pinsCapRefused=0;
    s.pinDedupeHits=0;
    s.pinRefusals={};
    for(auto* table:{bt.data(),ct.data(),at.data(),rt.data()})s.objects.erase(table);
    ancestorBase=ancestorCanonical=nullptr;ancestorOutputs={};
}
// D-117 discriminator, authored before the diagnostic change. These fixtures
// must fail on the old combined PSO/root reason. They identify synthetic gates,
// not the unknown association in the pinned live log. No new target/COM device.
static void identityDiagnostics(TestPaths& paths,const std::array<std::vector<uint8_t>,3>& stages) {
    // MSVC /Od /RTC1 reserves locals in disjoint scopes for the whole frame.
    // Log owns a 64-record ring plus a 36 KiB batch. Keep fixture-owned Logs
    // off the stack, including those in lambdas called with this frame active.
    // Allocate before arming failNew; the Log and its sink retain their original
    // scopes and all producer allocation-failure assertions remain in place.
    fixtureCheckpoint("identity helpers begin");
    conditionDiagnostics();orderingDiagnostics();ambiguityDiagnostics();nestedReleaseProof();nestedReleaseRefusalDiagnostics();creationSightingDiagnostics();untrackedTearoffRefusal();rejectedIdentityObservationOwnership();
    fixtureCheckpoint("identity helpers end; identity draw fixtures begin");
    auto dt=hook_fixture::table(),lt=hook_fixture::table(),pt=hook_fixture::table();
    auto rt=hook_fixture::table(),at=hook_fixture::table(),otherTable=hook_fixture::table(),rct=hook_fixture::table();
    dt[10]=reinterpret_cast<void*>(&hook_fixture::pso);dt[16]=reinterpret_cast<void*>(&hook_fixture::root);
    lt[7]=reinterpret_cast<void*>(&getListDevice);lt[10]=reinterpret_cast<void*>(&reset);
    lt[13]=reinterpret_cast<void*>(&indexed);lt[25]=reinterpret_cast<void*>(&pso);lt[30]=reinterpret_cast<void*>(&root);
    FakeCOM device{dt.data()},list{lt.data()},pipeline{pt.data()},signature{rt.data()},canonical{at.data()},other{otherTable.data()},rootCanonical{rct.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&pipeline);auto* r=reinterpret_cast<ID3D12RootSignature*>(&signature);
    auto* otherRoot=reinterpret_cast<ID3D12RootSignature*>(&other);
    auto& s=state();g_protect=hook_fixture::protect;g_pinResult=true;g_requirementsClock=clock;
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    listDevice=d;set_sink(nullptr);setDrawCallback({});setBindingCallback({});
    // Foundation hooks expose Reset without certifying capture interfaces.
    // Full certification here would give the list an identity before Reset,
    // bypassing the late-registration branch whose lp=2 diagnostics we test.
    CHECK(hookCaptureDevice(d)&&hookCommandList(l));
    CHECK(!s.states.at(l).interfaces_observed&&!s.states.at(l).bindings.tracking_complete);
    CHECK(s.states.at(l).registration_path==3);
    CHECK(l->Reset(nullptr,nullptr)==S_OK);
    auto& point=s.states.at(l);CHECK(point.registration_path==2&&point.bindings.tracking_complete);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=r;
    desc.VS={stages[0].data(),stages[0].size()};desc.PS={stages[1].data(),stages[1].size()};desc.GS={stages[2].data(),stages[2].size()};
    hook_fixture::result=S_FALSE;hook_fixture::expectedPSO=&desc;
    auto createRoot=[&](FakeCOM& object,const void* bytes,size_t length) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();set_sink(&log);
        const auto refs=object.refs;
        hook_fixture::output=&object;hook_fixture::expectedRoot=bytes;hook_fixture::rootLength=length;hook_fixture::rootNode=0;
        hook_fixture::rootIID=__uuidof(ID3D12RootSignature);void* out=nullptr;
        CHECK(d->CreateRootSignature(0,bytes,length,__uuidof(ID3D12RootSignature),&out)==S_FALSE&&out==&object);
        CHECK(reinterpret_cast<IUnknown*>(out)->Release()==refs);
        log.flush();CHECK(io.evidence.find(";ld=")!=io.evidence.npos);set_sink(nullptr);
    };
    // Actual E10 VS container, through the real CreateRootSignature wrapper.
    createRoot(signature,stages[0].data(),stages[0].size());
    CHECK(s.roots.at(r).valid()&&s.rootIdentities.at(r));
    const auto token=s.rootIdentities.at(r);
    pinRefusalDiagnostics(paths,d,desc);
    ownershipPinFixtures(paths,d,l,desc);
    diagnostics::LifetimeConditions lastPSOConditions;
    desc.IBStripCutValue=D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    auto createPSO=[&](REFIID iid=__uuidof(ID3D12PipelineState)) {
        const auto refusalCounts=s.pinRefusals.counts;const auto refusalFlags=s.pinRefusals.sticky;
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();set_sink(&log);
        hook_fixture::psoIID=iid;
        hook_fixture::output=&pipeline;void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,iid,&out)==S_FALSE&&out==p);
        CHECK(p->Release()==1);l->SetPipelineState(p);
        log.flush();CHECK(io.evidence.find("|pso|a=1|b=1|c=0|identity=unknown|hash=|sha256=|text=;pd=")!=io.evidence.npos);
        CHECK(io.evidence.find(";pf=")==io.evidence.npos&&io.evidence.find(";pt=")==io.evidence.npos);
        CHECK(s.pinRefusals.counts==refusalCounts&&s.pinRefusals.sticky==refusalFlags);
        const auto context=io.evidence.find(";pd=");
        CHECK(context!=io.evidence.npos);
        unsigned known=0,value=0;
        CHECK(std::sscanf(io.evidence.c_str()+context,";pd=%x/%x",&known,&value)==2);
        lastPSOConditions.known=known;lastPSOConditions.value=value;
        auto found=s.pipelines.find(p);
        if(found!=s.pipelines.end()) {
            char fields[80]{};const auto& record=found->second;
            CHECK(record.strip_cut==static_cast<uint32_t>(desc.IBStripCutValue)&&
                record.topology_type==static_cast<uint32_t>(desc.PrimitiveTopologyType));
            CHECK(s.states.at(l).bindings.pipeline.strip_cut==record.strip_cut&&
                s.states.at(l).bindings.pipeline.topology_type==record.topology_type);
            std::snprintf(fields,sizeof fields,";pd=%06x/%06x;rd=%06x/%06x",
                record.conditions.known,record.conditions.value,record.dependency_conditions.known,record.dependency_conditions.value);
            CHECK(io.evidence.find(fields)!=io.evidence.npos);
            CHECK(record.detail[0]&&io.evidence.find(record.detail.data())!=io.evidence.npos);
        }
        set_sink(nullptr);
    };
    diagnostics::LifetimeConditions lastBoundConditions;
    auto attempt=[&](const char* expected) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();set_sink(&log);
        s.requirements_attempts=0;s.requirements_last_us=0;tick=0;
        const auto draws=nativeDraws;const auto calls=io.calls;
        // Identity refusals stop before metadata resolution and must not allocate.
        // The positive control reaches NativeBindingObserver::root: its existing
        // "missing root binding" std::string may allocate (MSVC SSO is 15 bytes).
        // Arming failure there would test allocator refusal, disable the observer,
        // and mask the intended missing-t10 diagnostic in all later attempts.
        const bool identityFailure=std::string_view(expected).find("rejected:PSO/root")==0;
        const auto allocationsFailed=failedNew;
        failNew=identityFailure;l->DrawIndexedInstanced(9,1,0,0,0);
        CHECK(failNew==identityFailure&&failedNew==allocationsFailed&&io.calls==calls&&nativeDraws==draws+1);failNew=false;
        CHECK(forwarded[0]==9&&forwarded[1]==1&&!forwarded[2]&&!forwarded[3]&&!forwarded[4]);
        CHECK(s.requirements_attempts==1&&!point.bindings.pipeline.requirements_proven);
        log.flush();CHECK(io.evidence.find(expected)!=io.evidence.npos);
        CHECK(io.evidence.find(";draw=1,9,1,0,0,0;lp=2")!=io.evidence.npos);
        if(identityFailure)CHECK(io.evidence.find(std::string(expected)+
            ";cb10=4294967295;draw=1,9,1,0,0,0;lp=2")!=io.evidence.npos);
        if(std::string_view(expected).find("rejected:PSO/root explicit lifetime unavailable")==0) {
            const auto& record=s.pipelines.at(p);
            char fields[80]{};
            std::snprintf(fields,sizeof fields,";lp=2;pd=%06x/%06x;rd=%06x/%06x;bd=",
                record.conditions.known,record.conditions.value,record.dependency_conditions.known,record.dependency_conditions.value);
            CHECK(io.evidence.find(fields)!=io.evidence.npos);
            const auto bound=io.evidence.find(";bd=");unsigned known=0,value=0;
            CHECK(bound!=io.evidence.npos&&std::sscanf(io.evidence.c_str()+bound,";bd=%x/%x",&known,&value)==2);
            lastBoundConditions.known=known;lastBoundConditions.value=value;
            CHECK(record.detail[0]&&io.evidence.find(record.detail.data(),bound)!=io.evidence.npos);
        }
        tick=requirements_interval_us;l->DrawIndexedInstanced(9,1,0,0,0);log.flush();
        CHECK(linesWith(io.evidence,std::string("text=")+expected)==1);
        // b is the repeat count; c is cb10_kind, not the count. Identity
        // failures precede cb10 resolution, so c stays UINT_MAX there.
        CHECK(s.requirements_attempts==2);
        const auto outcome=io.evidence.find(std::string("text=")+expected);
        const auto lineBegin=io.evidence.rfind('\n',outcome);
        const auto line=io.evidence.substr(lineBegin==io.evidence.npos?0:lineBegin+1,
            io.evidence.find('\n',outcome)-(lineBegin==io.evidence.npos?0:lineBegin+1));
        CHECK(line.find("|b=2|c=")!=line.npos);
        if(identityFailure)CHECK(line.find("|c=4294967295|")!=line.npos);
        if(std::string_view(expected).find("canonical-release=unproven")!=std::string_view::npos)
            CHECK(line.find(std::string("text=")+expected+
                ";cb10=4294967295;draw=1,9,1,0,0,0;lp=2")!=line.npos);
        CHECK(!capture::freeze(point.bindings,{},{}));set_sink(nullptr);
    };
    deniedPipelineSlot=&pt[2];g_protect=pipelineProtect;createPSO();
    CHECK((lastPSOConditions.known&diagnostics::base_install)&&
        !(lastPSOConditions.value&diagnostics::base_install)&&!s.pipelines.count(p));
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    deniedPipelineSlot=nullptr;g_protect=hook_fixture::protect;
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(point.bindings.compatible_root()&&s.pipelines.at(p).root_identity==token);
    CHECK(lastPSOConditions.value&diagnostics::base_install);
    CHECK(s.pipelines.at(p).conditions.value&diagnostics::dependency_accepted);
    CHECK(s.pipelines.at(p).conditions.value&diagnostics::dependency_before);
    const requirements::MetadataResolver identityResolver{nullptr,
        [](void*,const capture::State&,uint32_t,capture::Id,uint32_t){return binding::NativeResolution{};},
        [](void*,const capture::State&,uint32_t,uint32_t,uint32_t,uint32_t){return binding::NativeResolution{};},
        [](void*,const capture::State&,const capture::BufferView&){return binding::NativeResolution{};},
        [](void*,const capture::State&,const binding::NativeResolution&){return false;},true};
    // Exhaust the state checker's conjuncts separately from COM provenance.
    // Diagnostic decomposition must be equivalent to compatible_root(), not
    // an alternate acceptance policy. No resolver/native calls are needed.
    // Construct present-but-invalid metadata through the public adapter path.
    metadata::RootLayout invalidLayout;invalidLayout.static_sampler_count=1;
    const auto invalidRoot=metadata::RootBindingMetadata::from_adapter(invalidLayout);
    CHECK(!invalidRoot.valid()&&invalidRoot.rejection()=="static samplers unsupported by adapter");
    for(unsigned bits=0;bits<128;++bits) {
        auto b=point.bindings;
        if(bits&1)b.pipeline.identity=0;
        if(bits&2)b.root_id=0;
        if(bits&4)b.root.reset();
        if(bits&8)b.root=invalidRoot;
        if(bits&16)b.pipeline.explicit_root_identity=0;
        if(bits&32)b.root_identity=0;
        if(bits&64)++b.pipeline.explicit_root_identity;
        requirements::MetadataResult result;requirements::check_metadata(b,{true,9,1,0,0,0},identityResolver,result);
        CHECK((std::string_view(result.outcome).find("rejected:PSO/root")==0)==!b.compatible_root());
    }
    {
        // Finite reason cardinality, bounded text, repeat dedup and existing
        // overflow/disk quotas. All lifetime/equality tags are static literals.
        // Keep expectations independent of newly added production declarations
        // so this fixture also compiles against the unchanged source for the
        // executor's expected-red run.
        const char* reasons[]={
            "rejected:PSO/root PSO lifetime unavailable;ps=0",
            "rejected:PSO/root bound root unobserved;bk=0",
            "rejected:PSO/root bound metadata unaccepted;layout=0",
            "rejected:PSO/root bound metadata invalid;layout=invalid",
            "rejected:PSO/root explicit lifetime unavailable;pr=0",
            "rejected:PSO/root bound lifetime unavailable;br=0",
            "rejected:PSO/root lifetime mismatch;life=ne",
            "rejected:PSO/root stale PSO observation;ps=ne",
            "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven",
            "rejected:PSO/root explicit lifetime unavailable;pc=unknown;pr=0",
            "rejected:PSO/root explicit lifetime unavailable;pi=unsupported;pr=0",
            "rejected:PSO/root lifetime mismatch;key=eq;life=ne",
            "rejected:PSO/root lifetime mismatch;key=ne;life=ne",
            "rejected:PSO/root lifetime mismatch;key=unknown;life=ne"
        };
        constexpr auto count=std::size(reasons);static_assert(count==14&&count<outcome_capacity);
        // diagnostics_log.h retains the pre-D117 inventory; add this scoped
        // extension conservatively (including its replaced legacy outcome).
        static_assert(metadata_static_outcome_count+count==94);
        static_assert(native_metadata_outcome_count+count==92);
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();const auto calls=io.calls;
        const char* seen[count]{};size_t n=0;
        {std::lock_guard lock(s.mutex);failNew=true;
            for(const auto* reason:reasons) {
                CHECK(std::strlen(reason)<110);
                for(size_t j=0;j<n;++j)CHECK(std::strcmp(seen[j],reason));seen[n++]=reason;
                requirements::MetadataResult sample;sample.outcome=reason;
                log.metadata_outcome(sample,{true,9,1,0,0,0},2);log.metadata_outcome(sample,{true,9,1,0,0,0},2);
            }
            CHECK(failNew&&io.calls==calls);failNew=false;
        }
        log.flush();CHECK(n==count&&!log.overflow(Event::requirements_metadata));
        for(auto reason:seen)CHECK(linesWith(io.evidence,std::string("text=")+reason+";")==1);
        CHECK(max_disk_bytes==4546432&&max_disk_bytes<9*1024*1024/2&&io.evidence.size()<=max_evidence_size);
    }
    attempt("rejected:missing t10 root parameter observation");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    createPSO(__uuidof(IUnknown));l->SetGraphicsRootSignature(r);
    CHECK(point.bindings.compatible_root());attempt("rejected:missing t10 root parameter observation");
    // Same table, distinct root object: identical E10 bytes do not make it the
    // PSO's root. Retained root refs prevent real final retirement here.
    createRoot(other,stages[0].data(),stages[0].size());l->SetGraphicsRootSignature(otherRoot);
    attempt("rejected:PSO/root lifetime mismatch;key=ne;life=ne");
    // A PSO created before root provenance exists never borrows a later root
    // publication at Draw. Creation order is a distinct pr=0 control.
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0&&r->Release()==0);
    signature.refs=1;pipeline.refs=1;createPSO();CHECK(!s.pipelines.at(p).root_identity);
    CHECK((s.pipelines.at(p).conditions.known&diagnostics::dependency_accepted)&&
        !(s.pipelines.at(p).conditions.value&diagnostics::dependency_accepted));
    CHECK(!(s.pipelines.at(p).conditions.known&diagnostics::dependency_before));
    createRoot(signature,stages[0].data(),stages[0].size());l->SetGraphicsRootSignature(r);
    attempt("rejected:PSO/root explicit lifetime unavailable;pr=0");
    CHECK((lastBoundConditions.known&diagnostics::dependency_before)&&
        !(lastBoundConditions.value&diagnostics::dependency_before));
    // D-119 discriminator: accepted self-canonical E10 root, split-canonical
    // PSO. No root publication ordering or root-alias ambiguity is involved.
    // On unchanged production the same wrapper sequence emits exactly:
    // rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;cb10=4294967295;draw=1,9,1,0,0,0;lp=2
    // The refined expectation below is expected-red against that production.
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    pipeline.rootInterface=canonical.rootInterface=&pipeline;
    pipeline.unknownInterface=canonical.unknownInterface=&canonical;canonical.owner=&pipeline;
    pt[0]=at[0]=reinterpret_cast<void*>(&conditionQuery);
    // J-102 extends the existing pre-fix discriminator. The base
    // PSO is in objects, canonical Release is separate. Denying canonical
    // patching preserves the exact live refusal after the repair too.
    deniedPipelineSlot=&at[2];g_protect=pipelineProtect;
    createPSO();l->SetGraphicsRootSignature(r);
    {
        const auto& record=s.pipelines.at(p);
        CHECK(record.creation_interface_supported&&record.canonical==PipelineRecord::Canonical::split);
        CHECK(record.identity==point.bindings.pipeline.identity&&record.root.get()==r);
        CHECK(s.rootIdentities.at(r)&&point.bindings.root_identity==s.rootIdentities.at(r));
        CHECK(point.bindings.root&&point.bindings.root->valid());
        CHECK(!record.root_identity&&!point.bindings.pipeline.explicit_root_identity);
        CHECK(requirements::metadata_identity_failure(point.bindings)==requirements::metadata_identity_reasons[4]);
        CHECK(s.objects.at(pt.data()).metadataRelease);
        CHECK(pt[2]==reinterpret_cast<void*>(&releaseMetadata));
        CHECK(at[2]==reinterpret_cast<void*>(&fakeRelease));
        CHECK((record.conditions.known&diagnostics::canonical_install)&&
            !(record.conditions.value&diagnostics::canonical_install));
        CHECK((record.conditions.value&diagnostics::base_covered)&&
            !(record.conditions.value&diagnostics::canonical_covered));
        CHECK(record.conditions.value&diagnostics::dependency_accepted);
        auto* identity=reinterpret_cast<IUnknown*>(&canonical);
        const auto refs=pipeline.refs;CHECK(identity->AddRef()==refs+1);
        CHECK(identity->Release()==refs&&s.pipelines.at(p).identity==record.identity);
    }
    attempt("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
    CHECK(lastBoundConditions.value&diagnostics::dependency_before);
    deniedPipelineSlot=nullptr;g_protect=hook_fixture::protect;
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    pipeline.rootInterface=pipeline.unknownInterface=nullptr;canonical.owner=nullptr;
    canonical.rootInterface=canonical.unknownInterface=nullptr;
    createPSO();l->SetGraphicsRootSignature(r);
    // A split canonical ROOT is already supported. Prove it independently of
    // split-PSO validation using exact E10 creation bytes.
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0&&r->Release()==0);
    signature.refs=1;signature.rootInterface=rootCanonical.rootInterface=&signature;
    signature.unknownInterface=rootCanonical.unknownInterface=&rootCanonical;rootCanonical.owner=&signature;
    const auto rootKey=reinterpret_cast<ID3D12RootSignature*>(&rootCanonical);
    createRoot(signature,stages[0].data(),stages[0].size());
    CHECK(s.roots.count(rootKey)&&!s.roots.count(r)&&s.rootIdentities.at(rootKey)!=token);
    // Independently counted canonical tear-off: QI must leave its own reference.
    rootCanonical.owner=nullptr;
    for(unsigned mode=0;mode<8;++mode) {
        auto t=hook_fixture::table(),retireTable=hook_fixture::table();
        FakeCOM object{t.data()},retired{retireTable.data()};
        auto* splitPSO=reinterpret_cast<ID3D12PipelineState*>(&object);
        auto* child=reinterpret_cast<IUnknown*>(&retired);
        CHECK(trackMetadataLifetime(child));
        MemoryIO io;io.readState=ReadState::ok;
        io.config=mode==6?"log_enabled=true":"log_enabled=true\ndiagnostic_ownership_pins=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();set_sink(&log);
        const auto refs=signature.refs,canonicalRefs=rootCanonical.refs;
        const auto query=rt[0];rt[0]=reinterpret_cast<void*>(&rootPinQuery);
        rootPinMode=mode;rootPinCalls=0;
        if(mode==7)for(auto& pin:s.ownershipPins)pin.reserved=true;
        if(mode==0)hook_fixture::psoAction=[&]{CHECK(child->Release()==0);};
        hook_fixture::output=&object;hook_fixture::psoIID=__uuidof(ID3D12PipelineState);void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE);
        rt[0]=query;
        const auto& record=s.pipelines.at(splitPSO);
        const bool pinned=mode==0,queried=mode<6;
        CHECK(bool(record.ownershipPin)==pinned&&s.pinsTaken==unsigned(pinned));
        CHECK(rootPinCalls==unsigned(queried));
        CHECK(s.pinRefusals.qiAttempts==unsigned(queried));
        CHECK(s.pinRefusals.qiSuccesses==unsigned(pinned));
        CHECK(signature.refs==refs+1+unsigned(mode==2));
        CHECK(rootCanonical.refs==canonicalRefs+unsigned(mode==0||mode==4||mode==5));
        log.flush();
        if(mode<6) {
            CHECK(io.evidence.find(pinned?";pf=000":";pf=010")!=io.evidence.npos);
            CHECK(io.evidence.find(pinned?";rq=0;ra=0001;rs=0001":";rq=1;ra=0001;rs=0000")!=io.evidence.npos);
            CHECK(s.ownershipPins[0].reserved&&s.ownershipPins[0].complete==pinned);
        }
        if(mode==6)CHECK(io.evidence.find(";rq=")==io.evidence.npos);
        if(mode==7)CHECK(s.pinsCapRefused==1&&io.evidence.find(";pf=080")!=io.evidence.npos);
        std::array<FakeCOM,16> failedObjects{};
        if(mode==1) {
            // A repeated partial acquisition cannot retry COM or gain proof.
            unsigned repeatedFlags=0;
            CHECK(!pinCreatedPipeline(splitPSO,record.identity,reinterpret_cast<IUnknown*>(splitPSO),{},
                record.root,1,repeatedFlags));
            CHECK(repeatedFlags==PinRefusals::reference&&rootPinCalls==1&&!s.pinDedupeHits);
            // Sixteen distinct partial reservations exhaust the permanent cap.
            rt[0]=reinterpret_cast<void*>(&rootPinQuery);
            for(unsigned i=0;i<15;++i) {
                unsigned flags=0;
                auto* failed=reinterpret_cast<ID3D12PipelineState*>(&failedObjects[i]);
                CHECK(!pinCreatedPipeline(failed,record.identity,reinterpret_cast<IUnknown*>(failed),{},
                    record.root,1,flags));
                CHECK(flags==(PinRefusals::split_root|PinRefusals::root_qi));
            }
            unsigned flags=0;
            auto* overflow=reinterpret_cast<ID3D12PipelineState*>(&failedObjects[15]);
            CHECK(!pinCreatedPipeline(overflow,record.identity,reinterpret_cast<IUnknown*>(overflow),{},
                record.root,1,flags));
            CHECK(flags==PinRefusals::cap&&rootPinCalls==16&&s.pinRefusals.qiAttempts==16);
            CHECK(std::all_of(s.ownershipPins.begin(),s.ownershipPins.end(),[](const auto& pin){return pin.reserved&&!pin.complete;}));
            rt[0]=query;
        }
        std::string drawEvidence;
        for(auto* bound:{r,rootKey,otherRoot}) {
            l->SetPipelineState(splitPSO);l->SetGraphicsRootSignature(bound);
            CHECK(pinnedRoot_locked(record,bound)==(pinned&&bound!=otherRoot));
            // Production logging retains the first example per outcome/cb10
            // and overwrites its counters in place. Base and canonical reach
            // the same outcome: each needs a fresh logger to test its own pb.
            MemoryIO drawIO;drawIO.readState=ReadState::ok;drawIO.config=io.config;
            auto drawLogStorage=std::make_unique<Log>(paths,drawIO,clockNow,true);auto& drawLog=*drawLogStorage;drawLog.flush();set_sink(&drawLog);
            s.requirements_attempts=0;s.requirements_last_us=0;
            rt[0]=reinterpret_cast<void*>(&rootPinQuery);
            const auto calls=rootPinCalls;
            l->DrawIndexedInstanced(9,1,0,0,0);
            CHECK(rootPinCalls==calls&&!point.bindings.pipeline.requirements_proven);
            CHECK(s.requirements_attempts==1);
            rt[0]=query;drawLog.flush();
            if(pinned) {
                const auto& observed=drawIO.evidence;
                CHECK(observed.find(bound==otherRoot?";pb=0":";pb=1")!=observed.npos);
                if(bound!=otherRoot)CHECK(observed.find("rejected:missing t10 root parameter observation")!=observed.npos);
            }
            drawEvidence+=drawIO.evidence;
            set_sink(&log);
        }
        log.flush();
        if(pinned) {
            CHECK(record.ownershipRequired&&record.root_identity);
            CHECK(drawEvidence.find("rejected:missing t10 root parameter observation")!=drawEvidence.npos);
            CHECK(drawEvidence.find(";pb=1")!=drawEvidence.npos);
            CHECK(drawEvidence.find(";pb=0")!=drawEvidence.npos);
            ownershipRetirementCertificates(l,reinterpret_cast<IUnknown*>(r));
            ownershipRetirementCertificates(l,reinterpret_cast<IUnknown*>(rootKey));
        }
        CHECK(!capture_enablement::decide({}).enabled);
        CHECK(l->Reset(nullptr,nullptr)==S_OK);
        CHECK(splitPSO->Release()==1+unsigned(pinned));
        s.pipelines.erase(splitPSO);s.gs.erase(splitPSO);
        CHECK(rootCanonical.refs==canonicalRefs+unsigned(mode==0||mode==4||mode==5));
        if(mode<6&&s.ownershipPins[0].rootCanonical) {
            std::lock_guard lock(s.mutex);
            CHECK(registeredOwnershipKey_locked(reinterpret_cast<uintptr_t>(s.ownershipPins[0].rootCanonical)));
        }
        // Fixture owners alone balance deliberate leaks AFTER all assertions.
        // Production never releases even anomalous failed/throwing QI outputs.
        for(auto& pin:s.ownershipPins) {
            for(auto* address:pin.interfaces)if(address&&pin.complete)fakeRelease(address);
            if(pin.rootCanonical)fakeRelease(pin.rootCanonical);
            pin=OwnershipPin{};
        }
        CHECK(splitPSO->Release()==0);
        CHECK(signature.refs==refs&&rootCanonical.refs==canonicalRefs);
        if(mode!=0)CHECK(child->Release()==0);
        s.pinsTaken=s.pinsCapRefused=0;s.pinRefusals={};
        s.objects.erase(t.data());s.objects.erase(retireTable.data());set_sink(nullptr);
    }
    rootCanonical.owner=&signature;
    pipeline.refs=1;createPSO();l->SetGraphicsRootSignature(r);
    CHECK(point.bindings.compatible_root()&&point.bindings.root_id==reinterpret_cast<uintptr_t>(rootKey));
    attempt("rejected:missing t10 root parameter observation");
    // Genuine two-interface COM shape: stable canonical QI and round trip,
    // common refcount, not the old fixture's arbitrary queryAlias claimant.
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
    pipeline.refs=1;pipeline.rootInterface=canonical.rootInterface=&pipeline;
    pipeline.unknownInterface=canonical.unknownInterface=&canonical;canonical.owner=&pipeline;
    pt[0]=at[0]=reinterpret_cast<void*>(&conditionQuery);
    // The canonical table is already registered as metadata, and also has
    // secondary lists membership. Its existing sole Release owner is reused.
    CHECK(trackMetadataLifetime(reinterpret_cast<IUnknown*>(&canonical)));
    s.lists.try_emplace(at.data());
    const auto canonicalOriginal=s.objects.at(at.data()).originals[2];
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(s.objects.at(at.data()).originals[2]==canonicalOriginal&&!s.lists.at(at.data()).originals[2]);
    {
        FakeCOM unknownPipeline{pt.data()};
        auto* unknownPSO=reinterpret_cast<ID3D12PipelineState*>(&unknownPipeline);
        l->SetPipelineState(unknownPSO);CHECK(!point.bindings.pipeline.identity);
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();set_sink(&log);s.requirements_attempts=0;
        // Without the observed E10 pipeline identity this is deliberately not
        // an eligible diagnostic attempt. No classification at Draw.
        l->DrawIndexedInstanced(9,1,0,0,0);CHECK(!s.requirements_attempts);set_sink(nullptr);
        l->SetPipelineState(p);CHECK(unknownPipeline.refs==1);
    }
    CHECK(s.pipelines.at(p).root_identity&&point.bindings.compatible_root());
    CHECK(s.pipelines.at(p).conditions.value&diagnostics::canonical_install);
    CHECK(s.pipelines.at(p).conditions.value&diagnostics::distinct_event);
    CHECK(s.pipelines.at(p).conditions.value&diagnostics::unique_routing);
    CHECK(s.pipelines.at(p).canonical_address==reinterpret_cast<IUnknown*>(&canonical));
    CHECK(at[2]==reinterpret_cast<void*>(&releaseMetadata));
    attempt("rejected:missing t10 root parameter observation");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    {
        MetadataEvent inFlight(reinterpret_cast<IUnknown*>(p),false);
        createPSO();
        CHECK((lastPSOConditions.known&diagnostics::distinct_event)&&
            !(lastPSOConditions.value&diagnostics::distinct_event));
        CHECK(!s.pipelines.at(p).root_identity);
    }
    l->SetGraphicsRootSignature(r);
    attempt("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(lastPSOConditions.value&diagnostics::distinct_event);
    const auto splitToken=s.pipelines.at(p).identity;
    {
        // A shared canonical TABLE never merges objects. Untracked peers and
        // another validated PSO keep their own native dispatch and lifetime.
        FakeCOM peer{at.data()};
        CHECK(reinterpret_cast<IUnknown*>(&peer)->Release()==0);
        CHECK(s.pipelines.at(p).identity==splitToken);
        FakeCOM second{pt.data()},secondCanonical{at.data()};
        second.rootInterface=secondCanonical.rootInterface=&second;
        second.unknownInterface=secondCanonical.unknownInterface=&secondCanonical;
        secondCanonical.owner=&second;
        hook_fixture::output=&second;void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE&&out==&second);
        auto* secondPSO=reinterpret_cast<ID3D12PipelineState*>(&second);
        CHECK(secondPSO->Release()==1);
        CHECK(s.pipelines.at(secondPSO).root_identity&&s.pipelines.at(secondPSO).identity!=splitToken);
        CHECK(reinterpret_cast<IUnknown*>(&secondCanonical)->Release()==0);
        CHECK(!s.pipelines.count(secondPSO)&&s.pipelines.at(p).identity==splitToken);
    }
    auto* canonicalUnknown=reinterpret_cast<IUnknown*>(&canonical);
    const auto refs=pipeline.refs;CHECK(canonicalUnknown->AddRef()==refs+1);
    CHECK(canonicalUnknown->Release()==refs&&s.pipelines.at(p).identity==splitToken);
    CHECK(l->Reset(nullptr,nullptr)==S_OK);
    // The final native Release arrives ONLY through canonical IUnknown.
    // The bound base reference was released by Reset, outside the hook mutex.
    CHECK(canonicalUnknown->Release()==0&&!s.pipelines.count(p)&&!s.gs.count(p));
    CHECK(pipelineFor_locked(canonicalUnknown)==s.pipelines.end());
    // Reusing both fake addresses publishes a fresh token, never the old one.
    pipeline.refs=1;createPSO();l->SetGraphicsRootSignature(r);
    CHECK(s.pipelines.at(p).identity!=splitToken&&point.bindings.compatible_root());
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
    // J-105: six distinct third addresses, each with the same canonical
    // identity and shared native refcount. Use actual creation and Release slots.
    {
        const auto ancestorRootToken=s.rootIdentities.at(rootKey);
        std::array<decltype(hook_fixture::table()),6> tables{};
        std::array<FakeCOM,6> ancestors{};
        ancestorBase=reinterpret_cast<IUnknown*>(p);
        ancestorCanonical=canonicalUnknown;
        for(size_t i=0;i<ancestors.size();++i) {
            tables[i]=hook_fixture::table();
            ancestors[i].table=tables[i].data();ancestors[i].owner=&pipeline;
            ancestors[i].rootInterface=&pipeline;ancestors[i].unknownInterface=&canonical;
            ancestorOutputs[i]=reinterpret_cast<IUnknown*>(&ancestors[i]);
        }
        auto cleanup=[&] {
            CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
            pipeline.refs=1;
        };
        // Identity/QI success cannot bypass even one uncovered ancestor seam.
        // First creation patches five tables and fails the denied sixth table.
        pipeline.refs=1;deniedPipelineSlot=&tables[0][2];g_protect=pipelineProtect;
        createPSO();l->SetGraphicsRootSignature(r);
        CHECK(!s.pipelines.at(p).root_identity&&!point.bindings.compatible_root());
        CHECK(std::strstr(s.pipelines.at(p).detail.data(),";qa=3f;qh=555555;od=7fff/7fff;ac=3e"));
        CHECK(!releaseCovered_locked(tables[0].data()));
        CHECK((s.pipelines.at(p).ancestor_addresses==std::array<IUnknown*,6>{}));
        attempt("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
        cleanup();deniedPipelineSlot=nullptr;g_protect=hook_fixture::protect;
        capture::Id previous=0;
        for(size_t final=0;final<ancestors.size();++final) {
            createPSO();l->SetGraphicsRootSignature(r);
            const auto& record=s.pipelines.at(p);
            CHECK(record.root_identity==ancestorRootToken&&point.bindings.compatible_root());
            CHECK(record.identity!=previous);previous=record.identity;
            CHECK(record.ancestor_addresses==ancestorOutputs);
            CHECK(std::strstr(record.detail.data(),";qa=3f;qh=555555;od=7fff/7fff;ac=3f"));
            CHECK(record.conditions.value&diagnostics::distinct_event);
            for(size_t i=0;i<ancestors.size();++i) {
                CHECK(releaseCovered_locked(tables[i].data()));
                CHECK(pipelineFor_locked(ancestorOutputs[i])->first==p);
                const auto before=pipeline.refs;
                CHECK(ancestorOutputs[i]->AddRef()==before+1);
                CHECK(ancestorOutputs[i]->Release()==before);
                CHECK(s.pipelines.at(p).identity==previous);
            }
            attempt("rejected:missing t10 root parameter observation");
            CHECK(l->Reset(nullptr,nullptr)==S_OK);
            // Only this ancestor receives final Release; every alias is retired.
            CHECK(ancestorOutputs[final]->Release()==0&&!s.pipelines.count(p)&&!s.gs.count(p));
            for(auto* alias:ancestorOutputs)CHECK(pipelineFor_locked(alias)==s.pipelines.end());
            pipeline.refs=1;
        }
        // Covered Release tables never excuse a canonical identity mismatch.
        ancestors[0].unknownInterface=&other;
        createPSO();CHECK(!s.pipelines.at(p).root_identity);
        CHECK(std::strstr(s.pipelines.at(p).detail.data(),";qm=01;"));
        cleanup();ancestors[0].unknownInterface=&canonical;
        // Distinct addresses may share one covered table with a secondary
        // list registry entry, but still have exactly one native Release owner.
        for(auto& ancestor:ancestors)ancestor.table=tables[0].data();
        s.lists.try_emplace(tables[0].data());
        createPSO();CHECK(s.pipelines.at(p).root_identity==ancestorRootToken);
        CHECK(!s.lists.at(tables[0].data()).originals[2]);
        {FakeCOM peer{tables[0].data()};
         CHECK(reinterpret_cast<IUnknown*>(&peer)->Release()==0&&s.pipelines.count(p));}
        CHECK(l->Reset(nullptr,nullptr)==S_OK);
        CHECK(ancestorOutputs[5]->Release()==0&&!s.pipelines.count(p));pipeline.refs=1;
        s.lists.erase(tables[0].data());
        for(size_t i=0;i<ancestors.size();++i)ancestors[i].table=tables[i].data();
        // Actual patched final Release inside native PSO creation. A validated
        // independent root has a complete immutable retirement footprint;
        // an unregistered peer sharing its table does not acquire that proof.
        auto releaseTable=hook_fixture::table();
        const auto savedSightings=s.creationSightings;
        for(unsigned refusalCase=0;refusalCase<7;++refusalCase) {
            const bool certified=refusalCase==0;
            const bool untracked=refusalCase==1||refusalCase>=5;
            s.creationSightings={};
            FakeCOM independent{releaseTable.data()};
            auto* retired=reinterpret_cast<IUnknown*>(&independent);
            FakeCOM retainedAlias{},nativePeer{};
            auto* aliasKey=reinterpret_cast<IUnknown*>(&retainedAlias);
            if(!untracked) {
                createRoot(independent,stages[0].data(),stages[0].size());
                CHECK(s.creationSightings.classify(reinterpret_cast<uintptr_t>(retired))==0);
                CHECK(s.rootCreations.at(reinterpret_cast<ID3D12RootSignature*>(retired)).retirementKnown);
                auto& certificate=s.rootCreations.at(reinterpret_cast<ID3D12RootSignature*>(retired));
                if(refusalCase==2) {
                    for(auto& key:certificate.retirementKeys)
                        if(key==reinterpret_cast<uintptr_t>(retired))key=0;
                }
                if(refusalCase==3) {
                    certificate.retirementKeys.back()=reinterpret_cast<uintptr_t>(aliasKey);
                    s.listKeys[aliasKey]=l;
                }
            }
            if(refusalCase==5) {
                void* seen=retired;
                observeCreationReturn(S_OK,&seen,CreationSightings::root);
            }
            if(refusalCase==6)s.creationSightings.overflow=true;
            hook_fixture::psoAction=[&] {
                if(refusalCase==4)deviceReleaseAction=[&] {
                    std::thread nested([&] {
                        MetadataEvent e(reinterpret_cast<IUnknown*>(&nativePeer),false,"other");
                    });nested.join();
                };
                CHECK(retired->Release()==0);
            };
            pipeline.refs=1;createPSO();l->SetGraphicsRootSignature(r);
            const auto& record=s.pipelines.at(p);
            CHECK(bool(record.root_identity)==certified);
            CHECK(bool(record.conditions.value&diagnostics::distinct_event)==certified);
            CHECK(std::strstr(record.detail.data(),";nr=01"));
            CHECK(std::strstr(record.detail.data(),certified?";ar=000;ak=0":";ar=001;ak=4"));
            const char* expected[]={";nf=0000;nc=00000000000000000000000000;np=01",
                ";nf=0003;nc=01010000000000000000000000;np=00",
                ";nf=0005;nc=01000100000000000000000000;np=00",
                ";nf=0009;nc=01000001000000000000000000;np=00",
                ";nf=0011;nc=01000000010000000000000000;np=00"};
            CHECK(std::strstr(record.detail.data(),expected[untracked?1:refusalCase]));
            CHECK(std::strstr(record.detail.data(),refusalCase==1?";uc=000100":
                refusalCase==5?";uc=010000":refusalCase==6?";uc=000001":";uc=000000"));
            CHECK(std::strstr(record.detail.data(),refusalCase==6?";uu=01000000":";uu=00000000"));
            attempt(certified?"rejected:missing t10 root parameter observation":
                "rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
            CHECK(!s.rootCreations.count(reinterpret_cast<ID3D12RootSignature*>(retired)));
            if(refusalCase==3)s.listKeys.erase(aliasKey);
            cleanup();
        }
        s.creationSightings=savedSightings;
        s.objects.erase(releaseTable.data());
        // No newly introduced alias may conflict with an existing list owner.
        s.listKeys[ancestorOutputs[0]]=l;createPSO();
        CHECK(!s.pipelines.count(p)&&!(lastPSOConditions.value&diagnostics::unique_routing));
        s.listKeys.erase(ancestorOutputs[0]);cleanup();
        // Same-thread, disjoint active metadata still sets sticky reentry.
        // Fixing self-footprint knowledge must not erase that independent fact.
        {
            MetadataEvent peer(reinterpret_cast<IUnknown*>(&other),false);
            createPSO();
            CHECK(!s.pipelines.at(p).root_identity);
            CHECK(std::strstr(s.pipelines.at(p).detail.data(),";od=7fff/7fdf;ac=3f"));
            CHECK(std::strstr(s.pipelines.at(p).detail.data(),";ar=001;ak=4;mc=01000000;mh=01000000"));
        }
        cleanup();
        // A known disjoint event on another thread is permitted. A shared root
        // dependency on that thread remains an actual footprint conflict.
        auto withPeer=[&](IUnknown* key,bool accepted) {
            std::mutex mutex;std::condition_variable cv;bool ready=false,finish=false;
            std::thread worker([&] {
                MetadataEvent peer(key,false);
                std::unique_lock lock(mutex);ready=true;cv.notify_one();
                CHECK(cv.wait_for(lock,std::chrono::seconds(10),[&]{return finish;}));
            });
            {std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(10),[&]{return ready;}));}
            createPSO();CHECK(bool(s.pipelines.at(p).root_identity)==accepted);
            const auto* detail=s.pipelines.at(p).detail.data();
            if(accepted) {
                CHECK(std::strstr(detail,";od=7fff/7fff;ac=3f"));
                CHECK(std::strstr(detail,";ar=000;ak=0;mc=00010000;mh=00000000"));
            } else {
                CHECK(!(s.pipelines.at(p).conditions.value&diagnostics::distinct_event));
                CHECK(std::strstr(detail,";ar=004;ak=4;mc=00010000;mh=00010000"));
            }
            {std::lock_guard lock(mutex);finish=true;}cv.notify_one();worker.join();
            cleanup();
        };
        withPeer(reinterpret_cast<IUnknown*>(&other),true);
        withPeer(reinterpret_cast<IUnknown*>(r),false);
        // Third addresses participate in overlap checks too.
        withPeer(ancestorOutputs[5],false);
        ancestorBase=ancestorCanonical=nullptr;ancestorOutputs={};
        for(auto& table:tables)s.objects.erase(table.data());
    }
    // A claimed canonical identity that round-trips to another base PSO is
    // still refused even though BOTH physical Release tables are covered.
    pipeline.refs=1;canonical.rootInterface=&other;
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.at(p).root_identity&&!point.bindings.compatible_root());
    CHECK((s.pipelines.at(p).conditions.known&diagnostics::round_trip)&&
        !(s.pipelines.at(p).conditions.value&diagnostics::round_trip));
    attempt("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
    pipeline.refs=1;canonical.rootInterface=&pipeline;canonical.unknownInterface=&other;
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.at(p).root_identity&&!point.bindings.compatible_root());
    CHECK((s.pipelines.at(p).conditions.known&diagnostics::stable_qi)&&
        !(s.pipelines.at(p).conditions.value&diagnostics::stable_qi));
    attempt("rejected:PSO/root explicit lifetime unavailable;pc=split;pr=0;canonical-release=unproven");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
    // Conflicting per-object list ownership refuses publication, regardless
    // of shared table coverage. No merging into the list owner is allowed.
    pipeline.refs=1;canonical.unknownInterface=&canonical;
    s.listKeys[reinterpret_cast<IUnknown*>(&canonical)]=l;
    createPSO();l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.count(p)&&!point.bindings.pipeline.identity);
    CHECK((lastPSOConditions.known&diagnostics::unique_routing)&&
        !(lastPSOConditions.value&diagnostics::unique_routing));
    s.listKeys.erase(reinterpret_cast<IUnknown*>(&canonical));
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);
    s.lists.erase(at.data());
    pipeline.refs=1;pipeline.rootInterface=pipeline.unknownInterface=nullptr;canonical.owner=nullptr;
    canonical.rootInterface=canonical.unknownInterface=nullptr;
    hook_fixture::queryMode=2;createPSO();hook_fixture::queryMode=0;l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.at(p).root_identity);
    attempt("rejected:PSO/root explicit lifetime unavailable;pc=unknown;pr=0");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    desc.pRootSignature=nullptr;createPSO();l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.at(p).root_identity);
    attempt("rejected:PSO/root explicit lifetime unavailable;pr=0");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;desc.pRootSignature=r;
    // Unknown requested creation IID cannot acquire explicit-root eligibility
    // merely because its output happens to answer the base-pipeline QI. This
    // is a separate expected-red behavior fixture on unchanged source.
    createPSO(unknownIID);l->SetGraphicsRootSignature(r);
    CHECK(!s.pipelines.at(p).root_identity&&!point.bindings.compatible_root());
    CHECK((lastPSOConditions.known&diagnostics::supported_output)&&
        !(lastPSOConditions.value&diagnostics::supported_output));
    attempt("rejected:PSO/root explicit lifetime unavailable;pi=unsupported;pr=0");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    {
        auto aliasTable=hook_fixture::table();FakeCOM alias{aliasTable.data()};alias.owner=&pipeline;
        alias.rootInterface=alias.unknownInterface=&pipeline;
        hook_fixture::output=&alias;hook_fixture::psoIID=__uuidof(ID3D12PipelineState);void* out=nullptr;
        CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&out)==S_FALSE&&out==&alias);
        CHECK(reinterpret_cast<IUnknown*>(out)->Release()==1);l->SetPipelineState(p);l->SetGraphicsRootSignature(r);
        CHECK(!s.pipelines.at(p).root_identity&&!point.bindings.compatible_root());
        attempt("rejected:PSO/root explicit lifetime unavailable;pi=unsupported;pr=0");
        CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(p->Release()==0);pipeline.refs=1;
    }
    createPSO();l->SetGraphicsRootSignature(r);
    // Source-including fault seams distinguish stale snapshots without
    // pretending a retained live COM root could actually have been freed.
    const auto bound=point.bindings.root_identity;point.bindings.root_identity=0;
    attempt("rejected:PSO/root bound lifetime unavailable;br=0");point.bindings.root_identity=bound;
    ++point.bindings.root_identity;
    attempt("rejected:PSO/root lifetime mismatch;key=eq;life=ne");point.bindings.root_identity=bound;
    const auto ps=point.bindings.pipeline.identity;++point.bindings.pipeline.identity;
    attempt("rejected:PSO/root stale PSO observation;ps=ne");point.bindings.pipeline.identity=ps;
    // Unknown root: setter observes the address but cannot accept a layout or
    // manufacture an identity. A separate SDK-serialized static sampler root
    // takes the same fail-closed metadata gate, never a PSO-root match.
    FakeCOM unknown{otherTable.data()};l->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(&unknown));
    attempt("rejected:PSO/root bound metadata unaccepted;layout=0");
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC unsupported{};unsupported.NumStaticSamplers=1;unsupported.pStaticSamplers=&sampler;
    ID3DBlob* blob=nullptr;CHECK(SUCCEEDED(D3D12SerializeRootSignature(&unsupported,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr)));
    // No inference from this byte count to the distinct live 184-byte object.
    l->SetGraphicsRootSignature(r);createRoot(other,blob->GetBufferPointer(),blob->GetBufferSize());blob->Release();
    CHECK(!s.roots.count(otherRoot)&&!s.rootIdentities.count(otherRoot));
    l->SetGraphicsRootSignature(otherRoot);attempt("rejected:PSO/root bound metadata unaccepted;layout=0");
    CHECK(l->Reset(nullptr,nullptr)==S_OK);CHECK(l->Release()==0&&p->Release()==0);
    CHECK(r->Release()==0&&otherRoot->Release()==0&&unknown.refs==1&&canonical.refs==1&&rootCanonical.refs==1);
    s.devices.erase(dt.data());s.lists.erase(lt.data());
    for(auto table:{pt.data(),rt.data(),at.data(),otherTable.data(),rct.data()})s.objects.erase(table);
    CHECK(device.refs==1&&pipeline.refs==0&&signature.refs==0&&other.refs==0);
    hook_fixture::output=nullptr;hook_fixture::expectedPSO=nullptr;hook_fixture::expectedRoot=nullptr;
    hook_fixture::psoIID=__uuidof(ID3D12PipelineState);
    hook_fixture::rootLength=0;hook_fixture::rootNode=7;listDevice=nullptr;
}
static FakeCOM* list1Output=nullptr;
static HRESULT STDMETHODCALLTYPE list1(ID3D12Device4*,UINT node,D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** out) {
    CHECK(!mutex_depth&&node==7&&type==D3D12_COMMAND_LIST_TYPE_DIRECT&&flags==D3D12_COMMAND_LIST_FLAG_NONE);
    CHECK(iid==__uuidof(ID3D12GraphicsCommandList));*out=list1Output;return S_FALSE;
}
#include "cpu_upload_hook_cases.h"
#include "s0_sampler_cases.h"
#include "t0_ownership_pin_cases.h"
static void tests(TestPaths& paths,const char* evidence) {
    // Real hydrated E10 containers drive the production PSO creation/identity
    // path; success uses a SYNTHETIC accepted root and buffer-only t0. This is
    // a metadata plumbing test, never executable-shader or runtime proof.
    std::ifstream file(evidence,std::ios::binary);CHECK(bool(file));
    std::string json((std::istreambuf_iterator<char>(file)),{});
    std::array<std::vector<uint8_t>,3> stages;
    size_t off=0;
    for(auto& bytes:stages) {
        off=json.find("\"base64\"",off);CHECK(off!=json.npos);
        auto begin=json.find('"',json.find(':',off))+1,end=json.find('"',begin);CHECK(end!=json.npos);
        bytes=base64(std::string_view(json).substr(begin,end-begin));off=end+1;
    }
    fixtureCheckpoint("metadata identity fixtures begin");
    identityDiagnostics(paths,stages);
    fixtureCheckpoint("metadata identity fixtures end; metadata draw fixtures begin");
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    tick=0;auto logStorage=std::make_unique<Log>(paths,io,clockNow,true);auto& log=*logStorage;log.flush();
    auto dt=hook_fixture::table(),lt=hook_fixture::table(),pt=hook_fixture::table(),rt=hook_fixture::table();
    auto ht=hook_fixture::table(),st=hook_fixture::table();
    dt[10]=reinterpret_cast<void*>(&hook_fixture::pso);dt[51]=reinterpret_cast<void*>(&list1);
    lt[10]=reinterpret_cast<void*>(&reset);lt[7]=reinterpret_cast<void*>(&getListDevice);lt[20]=reinterpret_cast<void*>(&topology);
    lt[12]=reinterpret_cast<void*>(&draw);lt[13]=reinterpret_cast<void*>(&indexed);lt[25]=reinterpret_cast<void*>(&pso);
    lt[28]=reinterpret_cast<void*>(&heaps);lt[30]=reinterpret_cast<void*>(&root);lt[32]=reinterpret_cast<void*>(&table);
    lt[34]=reinterpret_cast<void*>(&constant);lt[36]=reinterpret_cast<void*>(&constants);
    lt[27]=reinterpret_cast<void*>(&bundleCall);lt[59]=reinterpret_cast<void*>(&indirectCall);
    lt[38]=reinterpret_cast<void*>(&address);lt[43]=reinterpret_cast<void*>(&indices);lt[44]=reinterpret_cast<void*>(&vertices);
    FakeCOM device{dt.data()},list{lt.data()},pipeline{pt.data()},signature{rt.data()},heap{ht.data()},samplerHeap{st.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&pipeline);auto* rootObject=reinterpret_cast<ID3D12RootSignature*>(&signature);
    auto& s=state();g_protect=hook_fixture::protect;g_pinResult=true;g_requirementsClock=clock;
    s.requirements_attempts=0;s.requirements_last_us=0;setDrawCallback({});setBindingCallback({});
    s.nativeBindings.~NativeBindingObserver();new(&s.nativeBindings) binding::NativeBindingObserver;
    listDevice=d;CHECK(hookCaptureDevice(d)&&hookCaptureCommandList(l));
    auto& listState=s.states.at(l);CHECK(!listState.native_binding_history&&!listState.bindings.tracking_complete);
    CHECK(!listState.bindings.topology&&listState.bindings.pipeline.strip_cut==index_range::unknown_cut);
    using namespace metadata;
    RootLayout layout;
    RootParameter t10;t10.ranges={{BindingKind::srv,1,10,0,0,0}};
    RootParameter cb;cb.kind=ParameterKind::cbv;cb.shader_register=10;
    RootParameter t0;t0.visibility=Visibility::pixel;t0.ranges={{BindingKind::srv,1,0,0,0,0}};
    RootParameter s0;s0.visibility=Visibility::pixel;s0.ranges={{BindingKind::sampler,1,0,0,0,0}};
    layout.parameters={t10,cb,t0,s0};auto decoded=RootBindingMetadata::from_adapter(layout);CHECK(decoded.valid());
    s.roots.insert_or_assign(rootObject,decoded);s.rootIdentities[rootObject]=capture::new_lifetime_identity();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=rootObject;
    D3D12_INPUT_ELEMENT_DESC elements[4]{};
    for(unsigned i=0;i<4;++i)elements[i]={"ATTRIBUTE",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16*i,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    desc.InputLayout={elements,4};
    desc.VS={stages[0].data(),stages[0].size()};desc.PS={stages[1].data(),stages[1].size()};desc.GS={stages[2].data(),stages[2].size()};
    hook_fixture::expectedPSO=&desc;hook_fixture::output=&pipeline;hook_fixture::result=S_OK;
    void* created=nullptr;
    CHECK(d->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),&created)==S_OK&&created==p);
    CHECK(s.pipelines.at(p).drawcpt_identity_matched&&s.pipelines.at(p).drawcpt_ia_layout);CHECK(p->Release()==1);
    CHECK(s.pipelines.at(p).vs_cb10_words==1&&s.pipelines.at(p).ps_cb10_words==3);
    // A real hooked draw before the first observed Reset emits a precise
    // reason and all six numeric arguments; unchanged J059 marked it complete.
    l->SetPipelineState(p);set_sink(&log);l->DrawInstanced(1,9,0,0);log.flush();
    CHECK(io.evidence.find("rejected:created before hook/creation unobserved; no observed Reset;cb10=4294967295;draw=0,1,9,0,0,0")!=io.evidence.npos);
    set_sink(nullptr);s.requirements_attempts=0;s.requirements_last_us=0;
    CHECK(l->Reset(nullptr,p)==S_OK&&listState.reset_observed&&listState.bindings.tracking_complete);
    l->SetPipelineState(p);l->SetGraphicsRootSignature(rootObject);
    ID3D12DescriptorHeap* bound[]={reinterpret_cast<ID3D12DescriptorHeap*>(&heap),reinterpret_cast<ID3D12DescriptorHeap*>(&samplerHeap)};
    l->SetDescriptorHeaps(2,bound);l->SetGraphicsRootDescriptorTable(0,{10000});
    l->SetGraphicsRootConstantBufferView(1,0x20000);l->SetGraphicsRootDescriptorTable(2,{10032});l->SetGraphicsRootDescriptorTable(3,{20000});
    const auto dev=listState.native_binding_device;
    s.nativeBindings.heap({reinterpret_cast<uintptr_t>(&heap),1000,10000,64,32,0,1,0},dev);
    s.nativeBindings.heap({reinterpret_cast<uintptr_t>(&samplerHeap),5000,20000,1,32,1,1,0},dev);
    for(const auto& resource:std::initializer_list<binding::ResourceEvent>{{101,0x10000,416*3},{102,0x20000,512},{103,0x30000,64*32},{104,0x40000,128},{105,0x50000,64}})
        s.nativeBindings.resource(resource,dev);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer={2,1,416,D3D12_BUFFER_SRV_FLAG_NONE};
    s.nativeBindings.srv(101,&srv,1000,dev);
    auto restoreT10=[&]{s.nativeBindings.srv(101,&srv,1000,dev);};
    auto texture=srv;texture.Buffer={0,1,64,D3D12_BUFFER_SRV_FLAG_NONE};s.nativeBindings.srv(105,&texture,1032,dev);
    D3D12_SAMPLER_DESC sampler{};s.nativeBindings.sampler(&sampler,5000,dev);
    D3D12_VERTEX_BUFFER_VIEW vb{0x30000,64*32,64};l->IASetVertexBuffers(0,1,&vb);
    D3D12_INDEX_BUFFER_VIEW ib{0x40000,128,DXGI_FORMAT_R16_UINT};l->IASetIndexBuffer(&ib);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Production-shaped late discovery: A has patched this shared table, but
    // B has never been certified. Setters/Reset on B must not be mistaken for
    // an installer failure. Exercise actual wrappers, not fabricated flags.
    for(unsigned route=0;route<3;++route) {
        const bool earlyReset=route==1;
        FakeCOM late{lt.data()};auto* b=reinterpret_cast<ID3D12GraphicsCommandList*>(&late);
        CHECK(!s.states.count(b));
        if(route==2) {
            list1Output=&late;void* out=nullptr;
            CHECK(reinterpret_cast<ID3D12Device4*>(d)->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,
                D3D12_COMMAND_LIST_FLAG_NONE,__uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&out==b&&late.refs==1);
        }
        if(earlyReset)CHECK(b->Reset(nullptr,p)==S_OK);
        b->SetPipelineState(p);
        auto& lateState=s.states.at(b);
        CHECK(!lateState.creation_observed&&!lateState.observation_gap);
        CHECK(lateState.interfaces_observed==earlyReset&&lateState.capture_hooks==earlyReset);
        CHECK(lateState.bindings.tracking_complete==earlyReset);
        MemoryIO lateIO;lateIO.readState=ReadState::ok;lateIO.config="log_enabled=true";
        auto lateLogStorage=std::make_unique<Log>(paths,lateIO,clockNow,true);auto& lateLog=*lateLogStorage;lateLog.flush();set_sink(&lateLog);
        // Reset now registers/certifies the unknown shared-table object.
        const char* reason=earlyReset?"rejected:PSO/root bound root unobserved;bk=0":
            "rejected:created before hook/creation unobserved; no observed Reset";
        auto lateDraw=[&](const char* expected) {
            tick+=requirements_interval_us;auto before=nativeDraws;auto calls=lateIO.calls;
            // The early diagnostic gate allocates nothing. Full metadata
            // resolution below retains the existing allocating resolver API.
            const bool early=!lateState.bindings.tracking_complete;
            failNew=early;b->DrawIndexedInstanced(17,2,3,-7,4);CHECK(failNew==early);failNew=false;
            CHECK(nativeDraws==before+1&&lateIO.calls==calls);
            CHECK(forwarded[0]==17&&forwarded[1]==2&&forwarded[2]==3&&forwarded[3]==UINT(-7)&&forwarded[4]==4);
            lateLog.flush();
            std::istringstream records(lateIO.evidence);std::string line;bool found=false;
            while(std::getline(records,line))if(line.find(std::string(expected)+";")!=line.npos) {
                CHECK(line.find(";draw=1,17,2,3,-7,4")!=line.npos);found=true;
            }
            CHECK(found);
        };
        lateDraw(reason);
        // A genuine failed-install marker takes precedence over late discovery.
        lateState.observation_gap="rejected:unhooked list vtable or partial installation";
        lateDraw(lateState.observation_gap);lateState.observation_gap=nullptr;
        CHECK(hookCaptureCommandList(b)); // Submission discovery/certification seam.
        CHECK(lateState.interfaces_observed&&lateState.capture_hooks&&lateState.bindings.tracking_complete==earlyReset);
        lateDraw(reason); // Certification cannot recover the prior recording.
        CHECK(b->Reset(nullptr,p)==S_OK&&lateState.reset_observed&&lateState.bindings.tracking_complete);
        b->SetPipelineState(p);b->SetGraphicsRootSignature(rootObject);
        b->SetDescriptorHeaps(2,bound);b->SetGraphicsRootDescriptorTable(0,{10000});
        b->SetGraphicsRootConstantBufferView(1,0x20000);b->SetGraphicsRootDescriptorTable(2,{10032});
        b->SetGraphicsRootDescriptorTable(3,{20000});b->IASetVertexBuffers(0,1,&vb);
        b->IASetIndexBuffer(&ib);b->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        lateDraw("metadata-proved:index-range-unverified-without-bytes");
        CHECK(!lateState.bindings.pipeline.requirements_proven&&!capture::freeze(lateState.bindings,{},{}));
        set_sink(nullptr);CHECK(b->Release()==0&&!s.states.count(b));
        CHECK(listState.bindings.tracking_complete); // A stays isolated.
    }
    tick=0;s.requirements_attempts=0;s.requirements_last_us=0;
    // One absent component per case. Refinement preserves the checker order:
    // missing later IA state must not hide an earlier t10 failure.
    auto missing=[&](auto change,const char* gate,const char* expected,bool indexed=false) {
        auto b=listState.bindings;change(b);
        CHECK(std::string_view(missingComponent(b,{indexed,17,2,3,0,4},gate))==expected);
    };
    missing([](auto& b){b.pipeline.identity=0;},"rejected:PSO/root identity mismatch or unaccepted root","rejected:missing PSO observation");
    missing([](auto& b){b.root.reset();},"rejected:PSO/root identity mismatch or unaccepted root","rejected:missing root signature observation");
    missing([](auto& b){b.roots.erase(0);},"rejected:t10 missing/stale/unsupported descriptor","rejected:missing t10 root parameter observation");
    missing([](auto& b){b.roots.erase(1);},"rejected:cb10 missing/stale/unsupported descriptor","rejected:missing cb10 root parameter observation");
    missing([](auto& b){b.heaps.clear();},"rejected:t10 missing/stale/unsupported descriptor","rejected:missing descriptor heaps observation");
    missing([](auto& b){b.vertices.clear();},"rejected:IA slot zero view/stride 64 required","rejected:missing IA vertex buffer observation");
    missing([](auto& b){b.index.reset();},"rejected:index view missing/unsupported stride","rejected:missing index buffer observation",true);
    missing([](auto& b){b.topology=0;},"metadata-proved","rejected:missing topology observation");
    missing([](auto& b){b.vertices.clear();},"rejected:t10 missing/stale/unsupported descriptor","rejected:t10 missing/stale/unsupported descriptor");
    missing([](auto& b){b.vertices.clear();},"rejected:PSO input layout differs from E10 IA contract","rejected:PSO input layout differs from E10 IA contract");
    missing([](auto& b){b.heaps.clear();},"rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported","rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported");
    missing([](auto& b){b.topology=0;},"rejected:IA slot zero view/stride 64 required","rejected:IA slot zero view/stride 64 required");
    static_assert(metadata_static_outcome_count==80&&native_metadata_outcome_count==78);
    const auto revision=listState.bindings.revision;
    auto drawNow=[&](bool indexedDraw=false) {
        const auto calls=io.calls;const auto before=nativeDraws;
        if(indexedDraw)l->DrawIndexedInstanced(17,2,3,-7,4);else l->DrawInstanced(17,2,3,4);
        CHECK(nativeDraws==before+1&&io.calls==calls);
        CHECK(forwarded[0]==17&&forwarded[1]==2&&forwarded[2]==3&&forwarded[4]==4);
        CHECK(forwarded[3]==(indexedDraw?UINT(-7):0));
        CHECK(listState.bindings.revision==revision&&!listState.bindings.pipeline.requirements_proven);
    };
    // Null-sink path and nonmatching PSOs perform no diagnostic clock/attempt,
    // allocation or I/O. Sink installation changes no forwarded arguments.
    set_sink(nullptr);failNew=true;drawNow();CHECK(failNew&&!s.requirements_attempts);failNew=false;
    MemoryIO disabledIO;disabledIO.readState=ReadState::ok;disabledIO.config="log_enabled=false";
    auto disabledStorage=std::make_unique<Log>(paths,disabledIO,clockNow,true);auto& disabled=*disabledStorage;disabled.flush();CHECK(!disabled.enabled());set_sink(&disabled);
    failNew=true;drawNow();CHECK(failNew&&!s.requirements_attempts);failNew=false;
    set_sink(&log);s.pipelines.at(p).drawcpt_identity_matched=false;
    const auto clocks=clockChecks;failNew=true;drawNow();CHECK(failNew&&clockChecks==clocks);failNew=false;
    s.pipelines.at(p).drawcpt_identity_matched=true;
    drawNow();CHECK(s.requirements_attempts==1);log.flush();
    CHECK(linesWith(io.evidence,"text=metadata-proved;")==1);
    CHECK(io.evidence.find("span=832:1248;first=2;stride=416;cb10=2;ia=64:192:1280;draw=0,17,2,3,0,4;bytes=not-evaluated")!=io.evidence.npos);
    CHECK(log.counters().io_failures==0);
    // Also exercise the metadata checker's own catch boundary (independently
    // of the outer draw-hook probe), with no fabricated successful facts.
    bool resolverEntered=false;
    requirements::MetadataResolver throwing{&resolverEntered,
        [](void* context,const capture::State&,uint32_t,capture::Id,uint32_t)->binding::NativeResolution {
            *static_cast<bool*>(context)=true;auto* value=new char[32];delete[] value;
            throw std::runtime_error("injected resolver exception");
        },
        [](void*,const capture::State&,uint32_t,uint32_t,uint32_t,uint32_t){return binding::NativeResolution{};},
        [](void*,const capture::State&,const capture::BufferView&){return binding::NativeResolution{};},
        [](void*,const capture::State&,const binding::NativeResolution&){return false;},true};
    for(bool allocation:{false,true}) {
        resolverEntered=false;failNew=allocation;
        requirements::MetadataResult failed;requirements::check_metadata(listState.bindings,{false,17,2,3,0,4},throwing,failed);
        CHECK(resolverEntered&&!failNew&&!failed.proved&&std::string_view(failed.outcome)=="rejected:metadata exception/allocation failure");
        CHECK(!listState.bindings.pipeline.requirements_proven);
    }
    // One clock check, no allocation even with an installed sink. Backward
    // clocks fail closed; 999999 us is too soon; exactly 1 second is due.
    for(auto time:{uint64_t(0),uint64_t(999999)}) {tick=time;failNew=true;drawNow();CHECK(failNew&&s.requirements_attempts==1);failNew=false;}
    tick=1000000;drawNow();CHECK(s.requirements_attempts==2);log.flush();
    CHECK(linesWith(io.evidence,"text=metadata-proved;")==1&&io.evidence.find("|b=2|c=2|")!=io.evidence.npos);
    tick=0;failNew=true;drawNow();CHECK(failNew&&s.requirements_attempts==2);failNew=false;tick=1000000;
    auto attempt=[&](const char* why,bool index=false) {
        tick+=requirements_interval_us;const auto before=s.requirements_attempts;drawNow(index);CHECK(s.requirements_attempts==before+1);
        log.flush();CHECK(io.evidence.find(why)!=io.evidence.npos);CHECK(!log.counters().io_failures);
    };
    attempt("metadata-proved:index-range-unverified-without-bytes",true);
    listState.native_binding_history=false;attempt("rejected:recording history unobserved");listState.native_binding_history=true;
    auto gen=listState.bindings.generation;listState.bindings.generation=0;attempt("rejected:list generation mismatch");listState.bindings.generation=gen;
    auto rootToken=listState.bindings.root_identity;listState.bindings.root_identity=0;attempt("rejected:PSO/root bound lifetime unavailable;br=0");listState.bindings.root_identity=rootToken;
    s.bindingEvents.store(1);attempt("rejected:busy native capture point");s.bindingEvents.store(0);
    // Historical copies cannot resolve after the list revision/generation moves.
    auto historical=listState.bindings;++listState.bindings.generation;
    CHECK(!resolveNativeTableBinding(historical,0,reinterpret_cast<uintptr_t>(&heap),0));--listState.bindings.generation;
    auto descriptor=resolveNativeTableBinding(listState.bindings,0,reinterpret_cast<uintptr_t>(&heap),0);CHECK(descriptor);
    s.nativeBindings.unsupported_write(1000,dev);CHECK(!currentNativeBinding(descriptor,listState.bindings));
    attempt("rejected:t10 missing/stale/unsupported descriptor");restoreT10();
    srv.Buffer.StructureByteStride=400;restoreT10();attempt("rejected:t10 structured stride 416");srv.Buffer.StructureByteStride=416;restoreT10();
    // Real-root outcome: cb10 constants remain unsupported by the merged proof.
    layout.parameters[1].kind=ParameterKind::constants;layout.parameters[1].constant_count=4;
    listState.bindings.root=RootBindingMetadata::from_adapter(layout);
    attempt("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported");
    CHECK(io.evidence.find("unsupported;cb10=1;draw=0,17,2,3,0,4")!=io.evidence.npos);
    listState.bindings.root=decoded;
    // Repeat with the actual SDK-decoded E10 root, not an inferred layout.
    const auto realRoot=RootBindingMetadata::from_serialized(stages[0].data(),stages[0].size());CHECK(realRoot.valid());
    auto savedRoots=listState.bindings.roots;listState.bindings.root=realRoot.metadata;
    listState.bindings.roots.clear();
    for(auto stage:{ShaderStage::vertex,ShaderStage::geometry}) {
        const auto resolved=realRoot.metadata->resolve(BindingKind::srv,10,0,stage);CHECK(resolved.valid);
        if(resolved.parameter_kind==ParameterKind::table) {
            CHECK(resolved.table_offset<64);
            listState.bindings.roots[resolved.root_index]={ParameterKind::table,10000};
            s.nativeBindings.srv(101,&srv,1000+resolved.table_offset*32,dev);
        } else {
            CHECK(resolved.parameter_kind==ParameterKind::srv);
            listState.bindings.roots[resolved.root_index]={ParameterKind::srv,0x10000+2*416};
        }
    }
    const auto realCB=realRoot.metadata->resolve(BindingKind::cbv,10,0,ShaderStage::pixel);
    CHECK(realCB.valid&&realCB.parameter_kind==ParameterKind::constants);
    attempt("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported");
    {
        // Test-owned counted PSO reference plus the already retained root:
        // exercise the complete pin recognition at the real cb10 refusal.
        // This fixture supplies ownership, never a native capture certificate.
        auto& record=s.pipelines.at(p);CHECK(!record.ownershipPin);
        CHECK(!s.ownershipPins[0].reserved);
        const auto savedRevision=listState.bindings.revision;
        const auto savedCR=s.cb10ConstantDraws,savedCW=s.constantWordCalls;
        CHECK(s.pinsTaken==0&&s.pinsCapRefused==0);s.pinsTaken=1;
        s.cb10ConstantDraws=s.constantWordCalls=0;
        auto& pin=s.ownershipPins[0];pin.reserved=pin.complete=true;
        pin.base=p;pin.identity=record.identity;pin.root=record.root;
        pin.rootKey=rootObject;pin.rootIdentity=record.root_identity;pin.rootCount=1;
        pin.interfaces[0]=p;pin.counts[0]=p->AddRef();CHECK(pin.counts[0]>0);
        record.ownershipPin=1;record.ownershipIdentity=pin.identity;
        for(unsigned observed=0;observed<3;++observed) {
            MemoryIO observedIO;observedIO.readState=ReadState::ok;
            observedIO.config="log_enabled=true\ndiagnostic_ownership_pins=true";
            auto observedLogStorage=std::make_unique<Log>(paths,observedIO,clockNow,true);auto& observedLog=*observedLogStorage;observedLog.flush();set_sink(&observedLog);
            if(!observed) {
                s.roots.insert_or_assign(rootObject,*realRoot.metadata);
                l->SetGraphicsRootSignature(rootObject);
            }
            const uint32_t word=0x12345678;
            if(observed==1)l->SetGraphicsRoot32BitConstant(realCB.root_index,word,0);
            if(observed==2)l->SetGraphicsRoot32BitConstants(realCB.root_index,1,&word,0);
            CHECK(s.constantWordCalls==observed);
            if(observed) {
                const auto& value=listState.bindings.roots.at(realCB.root_index);
                CHECK(value.written[0]&&value.constants[0]==word);
            }
            tick+=requirements_interval_us;const auto nativeBefore=nativeDraws;
            const auto calls=observedIO.calls;
            l->DrawIndexedInstanced(9,1,0,0,0);
            CHECK(nativeDraws==nativeBefore+1&&observedIO.calls==calls&&s.cb10ConstantDraws==observed+1);
            CHECK(!listState.bindings.pipeline.requirements_proven&&!capture_enablement::decide({}).enabled);
            observedLog.flush();
            CHECK(observedIO.evidence.find("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported;cb10=1;draw=1,9,1,0,0,0;lp=")!=observedIO.evidence.npos);
            CHECK(observedIO.evidence.find(";pt=0001;px=0000;pb=1;cr=")!=observedIO.evidence.npos);
            CHECK(observedIO.evidence.find(observed==0?";cr=0001;cw=0000":observed==1?";cr=0002;cw=0001":";cr=0003;cw=0002")!=observedIO.evidence.npos);
            set_sink(&log);
        }
        {
            using W=diagnostics::RootConstantWitness;
            MemoryIO witnessIO;witnessIO.readState=ReadState::ok;
            witnessIO.config="log_enabled=true\ndiagnostic_ownership_pins=true";
            auto witnessLogStorage=std::make_unique<Log>(paths,witnessIO,clockNow,true);auto& witnessLog=*witnessLogStorage;witnessLog.flush();set_sink(&witnessLog);
            const auto length=realRoot.metadata->layout().parameters[realCB.root_index].constant_count;
            CHECK(length&&length<=W::capacity);
            std::array<uint32_t,W::capacity> values{};
            for(unsigned i=0;i<length;++i)values[i]=0x80000000u+i;
            // Configure from the accepted E10 layout and fill via installed setter hooks.
            listState.constantWitness={};l->SetGraphicsRootSignature(rootObject);
            const auto t0=realRoot.metadata->resolve(BindingKind::srv,0,0,ShaderStage::pixel);
            CHECK(t0.valid&&t0.root_index==0&&t0.table_offset==0&&t0.parameter_kind==ParameterKind::table);
            // An observed table with an unknown base still refuses at t0.
            l->SetGraphicsRootDescriptorTable(t0.root_index,{30000});
            CHECK(listState.tableWitness.written&&listState.tableWitness.parameter==0&&listState.tableWitness.offset==0);
            constantNativeProbe=[](ID3D12GraphicsCommandList* value) {
                const auto& w=state().states.at(listKey_locked(value)).constantWitness;
                CHECK(!mutex_depth&&!w.words[0].written); // publication must follow native return
            };
            l->SetGraphicsRoot32BitConstants(realCB.root_index,length,values.data(),0);constantNativeProbe=nullptr;
            tick+=requirements_interval_us;const auto before=s.constantWitnessCounts.reasons[W::covered];
            fixtureCheckpoint("covered witness installed draw begin");
            l->DrawIndexedInstanced(9,1,0,0,0);witnessLog.flush();
            fixtureCheckpoint("covered witness installed draw end");
            CHECK(s.constantWitnessCounts.reasons[W::covered]==before+1);
            CHECK(witnessIO.evidence.find(";rc=0;rv=3;")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find(";rw=80000000")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find("rejected:t0 missing/stale/unsupported descriptor;cb10=1;draw=1,9,1,0,0,0")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find(";cs=covered;cn=t0")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find(";cs=covered;cn=t0;tf=")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find(";tf=")!=witnessIO.evidence.npos);
            CHECK(witnessIO.evidence.find(";tp=0;to=0;")!=witnessIO.evidence.npos);
            const auto tableCompanions=witnessIO.evidence+witnessIO.files[0];
            CHECK(tableCompanions.find("root-table-witness;draw=1,9,1,0,0,0")!=tableCompanions.npos);
            CHECK(witnessIO.evidence.find(";sf=0;wf=8;wo=0;wn=0;wk=0;nd=0")!=witnessIO.evidence.npos);
            CHECK(tableCompanions.find("t0-write-witness;draw=1,9,1,0,0,0")!=tableCompanions.npos);
            CHECK(witnessIO.evidence.find("t0-resource-description;view-write;draw=1,9,1,0,0,0;vo=0;")!=witnessIO.evidence.npos);
            CHECK(tableCompanions.find("t0-resource-description;draw=1,9,1,0,0,0;rh=0;rr=0;rd=0;rf=0;rm=0;ra=0;rs=0;rl=0")!=tableCompanions.npos);
            CHECK(tableCompanions.find(";wf=8;wo=0;wn=0;wk=0;ok=0;nd=0")!=tableCompanions.npos);
            CHECK(witnessIO.evidence.find("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported")==witnessIO.evidence.npos);
            CHECK(!capture_enablement::decide({}).enabled&&!listState.bindings.pipeline.requirements_proven);
            t0OwnershipCases(paths,l,reinterpret_cast<uintptr_t>(&heap),dev);
            const auto attemptsBefore=s.requirements_attempts;
            failNew=true;l->DrawIndexedInstanced(9,1,0,0,0);
            CHECK(failNew&&s.requirements_attempts==attemptsBefore);failNew=false;
            auto savedList=listState;
            // Reuse a real state-machine witness and the current native metadata
            // resolvers. Any invalid witness must retain the exact cb10 refusal.
            fixtureCheckpoint("J-121 witness adapter matrix begin");
            {
                auto w=listState.constantWitness;const auto& b=listState.bindings;
                auto call=w.begin(b.list_identity,b.generation,GetCurrentThreadId());w.returned(call);
                auto covered=w.snapshot(b.list_identity,b.generation,b.root_id,b.root_identity,GetCurrentThreadId(),true);
                covered.binding_revision=b.revision;CHECK(covered.reason==W::covered);
                auto snap=covered;
                requirements::MetadataResolver resolver{nullptr,
                    [](void*,const capture::State& b,uint32_t p,capture::Id h,uint32_t o){return resolveNativeTableBinding_locked(b,p,h,o);},
                    [](void*,const capture::State& b,uint32_t p,uint32_t n,uint32_t e,uint32_t stride){return resolveNativeRootBinding_locked(b,p,n,e,stride);},
                    [](void*,const capture::State& b,const capture::BufferView& v){return resolveNativeView_locked(b,v);},
                    [](void*,const capture::State& b,const binding::NativeResolution& v){return bool(currentNativeBinding_locked(v,b));},
                    true,&snap,1,3,GetCurrentThreadId()};
                std::lock_guard lock(s.mutex);
                auto checkWitness=[&](bool accepted) {
                    requirements::MetadataResult result;requirements::check_metadata(b,{true,9,1,0,0,0},resolver,result);
                    CHECK(!result.proved&&result.constants_covered==accepted);
                    CHECK(std::string_view(result.outcome)==(accepted?"rejected:t0 missing/stale/unsupported descriptor":
                        "rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported"));
                    if(accepted)for(unsigned i=0;i<2;++i) {
                        const auto& x=result.root_constants[i];
                        CHECK(x.key.kind==draw_provenance::Kind::RootConstants&&x.key.root_parameter==realCB.root_index);
                        CHECK(x.word_count==2&&x.bytes==8&&x.root_id==b.root_id&&x.root_lifetime==b.root_identity);
                        CHECK(x.words[0]==snap.values[0]&&x.words[1]==snap.values[1]);
                        CHECK(x.origin==draw_provenance::Origin::Missing);
                    }
                };
                checkWitness(true);
                for(unsigned reason=W::history;reason<W::count;++reason) {
                    snap=covered;snap.reason=static_cast<W::Reason>(reason);checkWitness(false);
                }
                for(unsigned field=0;field<14;++field) {
                    snap=covered;
                    switch(field) {
                    case 0:++snap.list;break;case 1:++snap.generation;break;
                    case 2:++snap.binding_revision;break;case 3:++snap.root;break;
                    case 4:++snap.token;break;case 5:++snap.index;break;
                    case 6:++snap.size;break;case 7:snap.mask=1;break;
                    case 8:snap.mask=2;break;case 9:snap.thread=0;break;
                    case 10:snap.stages=1;break;case 11:snap.stages=2;break;
                    case 12:snap.before=snap.after;break;case 13:snap.revision=0;break;
                    }
                    checkWitness(false);
                }
                snap=covered;resolver.constants=nullptr;checkWitness(false);resolver.constants=&snap;
                resolver.vs_cb10_words=0;checkWitness(false);resolver.vs_cb10_words=1;
                resolver.ps_cb10_words=0;checkWitness(false);resolver.ps_cb10_words=3;
                auto retainedRoot=listState.bindings.root;
                auto wrongCount=retainedRoot->layout();wrongCount.parameters[realCB.root_index].constant_count=4;
                listState.bindings.root=RootBindingMetadata::from_adapter(wrongCount);checkWitness(false);
                listState.bindings.root=std::move(retainedRoot);
                // Native source words are carried as exact bits, even zero and
                // NaN bit patterns: this does not interpret them as float values.
                snap.values[0]=0x7fc00001u;snap.values[1]=0;checkWitness(true);
                w.end(call);
            }
            fixtureCheckpoint("J-121 witness adapter matrix end; witness invalidation fixtures begin");
            // Actual Reset clears all words; rebinding cannot recover old coverage.
            const auto generation=listState.bindings.generation;
            CHECK(l->Reset(nullptr,p)==S_OK);
            CHECK(listState.bindings.generation!=generation&&!listState.constantWitness.words[0].written);
            l->SetGraphicsRootSignature(rootObject);
            CHECK(!listState.constantWitness.words[0].written);
            listState=std::move(savedList);
            // Distinct accepted root identity clears root arguments.
            FakeCOM otherSignature{rt.data()};auto* otherRoot=reinterpret_cast<ID3D12RootSignature*>(&otherSignature);
            s.roots.insert_or_assign(otherRoot,*realRoot.metadata);s.rootIdentities[otherRoot]=capture::new_lifetime_identity();
            l->SetGraphicsRootSignature(otherRoot);
            CHECK(!listState.constantWitness.words[0].written&&listState.constantWitness.changed);
            l->SetGraphicsRootSignature(rootObject);s.roots.erase(otherRoot);s.rootIdentities.erase(otherRoot);
            CHECK(otherSignature.refs==1);
            l->SetGraphicsRoot32BitConstants(realCB.root_index,length,values.data(),0);
            savedList=listState;
            l->ExecuteBundle(nullptr);CHECK(listState.constantWitness.poison==W::bundle);
            const auto bundleBefore=s.constantWitnessCounts.reasons[W::bundle];
            const auto bundleAttempts=s.requirements_attempts;
            tick+=requirements_interval_us;l->DrawIndexedInstanced(9,1,0,0,0);witnessLog.flush();
            CHECK(s.constantWitnessCounts.reasons[W::bundle]==bundleBefore+1&&s.requirements_attempts==bundleAttempts+1);
            // First companion is retained in existing startup evidence; later
            // samples still use the ordinary ring/log. Keep the exact payload.
            const auto companionOutput=witnessIO.evidence+witnessIO.files[0];
            CHECK(companionOutput.find("root-constant-witness;draw=1,9,1,0,0,0;rc=8")!=companionOutput.npos);
            CHECK(witnessLog.counters().io_failures==0);
            CHECK(!capture_enablement::decide({}).enabled&&!listState.bindings.pipeline.requirements_proven);
            listState=savedList;
            l->ExecuteIndirect(nullptr,1,nullptr,0,nullptr,0);CHECK(listState.constantWitness.poison==W::indirect);
            listState=savedList;
            // Overlap that begins INSIDE native Draw must never log coverage.
            drawNativeProbe=[](ID3D12GraphicsCommandList* value) {value->SetGraphicsRoot32BitConstant(0,1,0);};
            const auto coveredBefore=s.constantWitnessCounts.reasons[W::covered];
            l->DrawIndexedInstanced(9,1,0,0,0);drawNativeProbe=nullptr;
            CHECK(s.constantWitnessCounts.reasons[W::covered]==coveredBefore&&listState.constantWitness.poison==W::overlap);
            listState=savedList;
            // A native setter reenters the installed setter, then two native
            // setter calls on distinct threads overlap. Both remain refused.
            constantNativeProbe=[](ID3D12GraphicsCommandList* value) {
                constantNativeProbe=nullptr;value->SetGraphicsRoot32BitConstant(0,1,0);
            };
            l->SetGraphicsRoot32BitConstant(realCB.root_index,1,0);constantNativeProbe=nullptr;
            CHECK(listState.constantWitness.poison==W::overlap);
            listState.constantWitness={};l->SetGraphicsRootSignature(rootObject);
            constantNativeProbe=[](ID3D12GraphicsCommandList* value) {
                constantNativeProbe=nullptr;
                std::thread other([&]{value->SetGraphicsRoot32BitConstant(0,1,0);});other.join();
            };
            l->SetGraphicsRoot32BitConstant(realCB.root_index,1,0);constantNativeProbe=nullptr;
            CHECK(listState.constantWitness.poison==W::thread);
            // Resolve stage visibility independently at root binding time.
            auto visibilityLayout=realRoot.metadata->layout();
            visibilityLayout.parameters[realCB.root_index].visibility=Visibility::vertex;
            s.roots.insert_or_assign(rootObject,RootBindingMetadata::from_adapter(visibilityLayout));
            listState.constantWitness={};l->SetGraphicsRootSignature(rootObject);CHECK(listState.constantWitness.stages==1);
            visibilityLayout.parameters[realCB.root_index].visibility=Visibility::pixel;
            s.roots.insert_or_assign(rootObject,RootBindingMetadata::from_adapter(visibilityLayout));
            l->SetGraphicsRootSignature(rootObject);CHECK(listState.constantWitness.stages==2);
            s.roots.insert_or_assign(rootObject,*realRoot.metadata);l->SetGraphicsRootSignature(rootObject);
            set_sink(&log);
        }
        s.constantWordCalls=2; // restore the older counter fixture's own baseline
        // OFF observes the existing setter state but no new diagnostic count.
        const auto witnessRevision=listState.constantWitness.revision;
        const auto witnessCounts=s.constantWitnessCounts.reasons;
        l->SetGraphicsRoot32BitConstant(realCB.root_index,0,0);CHECK(s.constantWordCalls==2);
        CHECK(listState.constantWitness.revision==witnessRevision&&s.constantWitnessCounts.reasons==witnessCounts);
        s.roots.insert_or_assign(rootObject,decoded);
        record.ownershipPin=0;record.ownershipIdentity=0;
        pin=OwnershipPin{};fakeRelease(reinterpret_cast<IUnknown*>(p));
        s.cb10ConstantDraws=savedCR;s.constantWordCalls=savedCW;
        s.pinsTaken=0;
        listState.bindings.revision=savedRevision;
    }
    listState.bindings.root=decoded;listState.bindings.roots=std::move(savedRoots);restoreT10();s.nativeBindings.srv(105,&texture,1032,dev);
    auto cbAddress=listState.bindings.roots.at(1).value;listState.bindings.roots.at(1).value=0;
    attempt("rejected:cb10 missing/stale/unsupported descriptor");listState.bindings.roots.at(1).value=cbAddress;
    s.nativeBindings.unsupported_write(1032,dev);attempt("rejected:t0 missing/stale/unsupported descriptor");s.nativeBindings.srv(105,&texture,1032,dev);
    s.nativeBindings.unsupported_write(5000,dev);attempt("rejected:s0 missing/stale/unsupported descriptor");s.nativeBindings.sampler(&sampler,5000,dev);
    s.pipelines.at(p).drawcpt_ia_layout=false;attempt("rejected:PSO input layout differs from E10 IA contract");s.pipelines.at(p).drawcpt_ia_layout=true;
    listState.bindings.vertices.at(0).stride=60;attempt("rejected:IA slot zero view/stride 64 required");listState.bindings.vertices.at(0).stride=64;
    listState.bindings.vertices.at(0).resource=0x30004;listState.bindings.vertices.at(0).size=64*32;
    attempt("rejected:IA slot zero missing/stale resource span");listState.bindings.vertices.at(0)={0x30000,0,64*32,64};
    listState.bindings.vertices.at(0).size=64*19;attempt("rejected:draw vertex range exceeds IA slot zero view");listState.bindings.vertices.at(0).size=64*32;
    listState.bindings.index->resource=0x40080;attempt("rejected:index view missing/stale resource span",true);listState.bindings.index->resource=0x40000;
    listState.bindings.index->size=16;attempt("rejected:draw index range exceeds index view",true);listState.bindings.index->size=128;
    // Allocate-failure evaluation is caught; draw still forwards, no I/O and
    // no readiness. The fixture's first allocation is a root-resolution check.
    g_requirementsProbe=[] {CHECK(mutex_depth==1);auto* value=new char[32];delete[] value;};
    tick+=requirements_interval_us;failNew=true;drawNow();CHECK(!failNew);log.flush();
    CHECK(io.evidence.find("rejected:metadata exception/allocation failure")!=io.evidence.npos);
    g_requirementsProbe=[] {CHECK(mutex_depth==1);throw std::runtime_error("injected metadata evaluation");};
    attempt("rejected:metadata exception/allocation failure");g_requirementsProbe=nullptr;
    // A setter allocation failure happens AFTER identical native forwarding,
    // poisons tracking, and is diagnosed on the next draw. Restore only the
    // synthetic fixture's saved state afterwards; production has no such seam.
    auto savedBindings=listState.bindings;
    failNew=true;l->SetGraphicsRootConstantBufferView(63,123);
    CHECK(!failNew&&addressParameter==63&&addressValue==123&&!listState.bindings.tracking_complete);
    tick+=requirements_interval_us;const auto beforeFault=nativeDraws;const auto faultIO=io.calls;
    l->DrawIndexedInstanced(17,2,3,-7,4);CHECK(nativeDraws==beforeFault+1&&io.calls==faultIO);
    log.flush();CHECK(io.evidence.find("rejected:setter observation exception/allocation failure;cb10=4294967295;draw=1,17,2,3,-7,4")!=io.evidence.npos);
    listState.bindings=std::move(savedBindings);
    s.nativeBindings.disable();attempt("rejected:native binding observer disabled");
    while(s.requirements_attempts<requirements_attempt_limit) {tick+=requirements_interval_us;drawNow();}
    auto attempts=s.requirements_attempts;const auto clockBefore=clockChecks;
    tick=UINT64_MAX;failNew=true;drawNow(true);CHECK(failNew&&s.requirements_attempts==attempts&&clockChecks==clockBefore);failNew=false;
    log.flush();CHECK(linesWith(io.evidence,"text=rejected:native binding observer disabled;")==1);
    const auto priorDistinct=hook_fixture::kinds(io.evidence,"requirements_metadata");CHECK(priorDistinct<outcome_capacity);
    CHECK(!capture::freeze(listState.bindings,{false,17,2,3,0,4},{}));CHECK(!capture_enablement::decide({}).enabled);
    // Priority quotas are independent from a flooded ordinary ring. Counts
    // update in-place and overflow has a single reserved record.
    for(size_t i=0;i<ring_capacity+1;++i)log.push(record(Event::summary));
    const auto calls=io.calls;
    {std::lock_guard lock(s.mutex);failNew=true;
        for(unsigned i=0;i<outcome_capacity+5;++i) {
            requirements::MetadataResult sample;sample.outcome="rejected:fixture distinct outcome";sample.cb10_kind=100+i;
            log.metadata_outcome(sample,{});log.metadata_outcome(sample,{});
        }
        CHECK(failNew&&io.calls==calls);failNew=false;
    }
    {std::lock_guard lock(s.mutex);const auto beforeIO=io.calls;failNew=true;
        for(unsigned repeat=0;repeat<1000;++repeat)for(unsigned path=0;path<3;++path)log.list_registered(path);
        CHECK(failNew&&io.calls==beforeIO);failNew=false;
    }
    log.flush();CHECK(log.overflow(Event::requirements_metadata)==2*(priorDistinct+5));
    for(const char* path:{"CreateCommandList","CreateCommandList1","registered-at-reset"}) {
        CHECK(linesWith(io.evidence,std::string("text=")+path+" ")==1);
    }
    CHECK(linesWith(io.evidence,"|b=1000|")==3);
    const auto pathSize=io.evidence.size();
    {std::lock_guard lock(s.mutex);log.list_registered(1);}
    log.flush();CHECK(io.evidence.size()==pathSize&&linesWith(io.evidence,"|b=1001|")==1);

    CHECK(linesWith(io.evidence,"text=metadata outcome overflow")==1);
    std::istringstream records(io.evidence);std::string line;
    while(std::getline(records,line))if(line.find("|requirements_metadata|")!=line.npos)
        CHECK(line.find(";draw=")!=line.npos); // overflow also retains first draw
    CHECK(hook_fixture::kinds(io.evidence,"requirements_metadata")==outcome_records);
    CHECK(io.evidence.size()<=max_evidence_size&&max_disk_bytes==4546432);
    CHECK(!log.counters().io_failures);
    set_sink(nullptr);g_requirementsClock=requirementsClock;
    CHECK(l->Release()==0);s.pipelines.erase(p);CHECK(p->Release()==0);s.roots.erase(rootObject);s.rootIdentities.erase(rootObject);
    s.devices.erase(dt.data());s.lists.erase(lt.data());s.objects.erase(pt.data());
    CHECK(signature.refs==1&&heap.refs==1&&samplerHeap.refs==1);
    hook_fixture::expectedPSO=nullptr;hook_fixture::output=nullptr;
}
}
// J-122: these are object-size guards, NOT measured compiler frame sizes.
// In particular the J-121 adapter must not acquire a whole provenance Contract
// (128 Inputs) on the native Draw path. Its actual locals are two Requireds,
// and the caller/checker each retain one small MetadataResult.
static_assert(sizeof(acz::draw_provenance::Input)<=1280);
static_assert(sizeof(acz::draw_provenance::Required)<=1280);
static_assert(sizeof(acz::requirements::MetadataResult)<=2688);
static_assert(sizeof(acz::diagnostics::RootConstantWitness)<=1536);
static_assert(sizeof(acz::diagnostics::RootConstantWitness::Snapshot)<=192);
static_assert(sizeof(acz::diagnostics::DescriptorTableDiagnostics::Snapshot)<=256);
static_assert(sizeof(acz::PipelineRecord)<=2048);
static_assert(sizeof(acz::ListState)<=4096);

static void metadataStackRegression(TestPaths& paths,const char* evidence) {
    struct Context {TestPaths* paths;const char* evidence;} context{&paths,evidence};
    // Always use a 1 MiB reservation, even when an executor has relinked the
    // executable with /STACK:67108864 for diagnosis. No larger-stack workaround.
    // Run the complete existing metadata/identity tree, including installed
    // Draw wrappers and the J-121 witness matrix, with every old CHECK intact.
    fixtureCheckpoint("metadata 1 MiB stack regression begin");
    const auto run=+[](void* opaque)->DWORD {
        auto& c=*static_cast<Context*>(opaque);
        int marker=0;MEMORY_BASIC_INFORMATION memory{};
        CHECK(VirtualQuery(&marker,&memory,sizeof memory)==sizeof memory);
        ULONG_PTR low=0,high=0;GetCurrentThreadStackLimits(&low,&high);
        const auto base=reinterpret_cast<ULONG_PTR>(memory.AllocationBase);
        CHECK(high>low&&low>=base&&high>base&&high-base<=1024*1024);
        std::fprintf(stderr,"diagnostics stack regression: reservation=%zu; Log=%zu; Input=%zu; Required=%zu; MetadataResult=%zu; Witness=%zu; Snapshot=%zu; TableSnapshot=%zu; PipelineRecord=%zu; ListState=%zu\n",
            size_t(high-base),sizeof(Log),sizeof(acz::draw_provenance::Input),
            sizeof(acz::draw_provenance::Required),sizeof(acz::requirements::MetadataResult),
            sizeof(acz::diagnostics::RootConstantWitness),sizeof(acz::diagnostics::RootConstantWitness::Snapshot),
            sizeof(acz::diagnostics::DescriptorTableDiagnostics::Snapshot),
            sizeof(acz::PipelineRecord),sizeof(acz::ListState));
        std::fflush(stderr);
        try {metadata_fixture::tests(*c.paths,c.evidence);}
        catch(...) {fixtureCheckpoint("metadata 1 MiB stack regression threw");return 1;}
        return 0;
    };
    HANDLE worker=CreateThread(nullptr,1024*1024,run,&context,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    CHECK(worker!=nullptr);
    // Existing tests are synchronous; the context and providers outlive join.
    CHECK(WaitForSingleObject(worker,60000)==WAIT_OBJECT_0);
    DWORD code=1;CHECK(GetExitCodeThread(worker,&code));CHECK(CloseHandle(worker));CHECK(code==0);
    fixtureCheckpoint("metadata 1 MiB stack regression end");
}

static void shaderTests(const char* evidence) {
    std::ifstream file(evidence,std::ios::binary);CHECK(bool(file));
    std::string text((std::istreambuf_iterator<char>(file)),{});size_t off=0,count=0;
    std::vector<std::vector<uint8_t>> blobs; // J-world-objects: the three E10 containers (VS, PS, GS)
    while((off=text.find("\"base64\"",off))!=text.npos) {
        auto begin=text.find('"',text.find(':',off))+1;auto end=text.find('"',begin);CHECK(end!=text.npos);
        auto bytes=base64(std::string_view(text).substr(begin,end-begin));
        blobs.push_back(std::vector<uint8_t>(bytes.begin(),bytes.end()));
        auto identity=shader_identity(bytes.data(),bytes.size());CHECK(identity.identity==Identity::match);
        CHECK(std::char_traits<char>::length(identity.hash.data())==32);
        CHECK(std::char_traits<char>::length(identity.container_prefix.data())==16);
        // Mutate executable bitcode without altering the HASH/metadata parts.
        bytes.back()^=1;auto mismatch=shader_identity(bytes.data(),bytes.size());
        CHECK(mismatch.identity==Identity::hash_only_no_container_match);
        CHECK(mismatch.hash==identity.hash&&mismatch.container_prefix!=identity.container_prefix);
        bool mutated=false;
        for(size_t i=0;i+28<=bytes.size();++i)if(std::memcmp(bytes.data()+i,"HASH",4)==0) {
            bytes[i+12]^=1;mutated=true;break;
        }
        CHECK(mutated&&shader_identity(bytes.data(),bytes.size()).identity==Identity::unknown);
        off=end+1;++count;
    }
    CHECK(count==3);CHECK(shader_identity(nullptr,0).identity==Identity::unknown);
    uint8_t junk[8]{};CHECK(shader_identity(junk,sizeof junk).identity==Identity::unknown);
    // J-world-objects: the DrawACM identity comparison, exercised with the real E10 containers standing in for the
    // (non-repository) DrawACM_v / DrawACM_p containers: VS = HASH + full container SHA-256, PS = partial fingerprint.
    CHECK(blobs.size()==3&&blobs[0].size()==4268&&blobs[1].size()==3160);
    const AcmPins e10{"f4cfd928c11d0a822baeb92e09767134","adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd","78298093","33dd"};
    std::string psHash,psDigest;
    {const auto id=acm_world_identity(blobs[0].data(),blobs[0].size(),blobs[1].data(),blobs[1].size(),&e10);
     CHECK(id.vs_match&&!id.vs_hash_only&&id.ps_fingerprint&&std::char_traits<char>::length(id.ps_hash.data())==32);
     // J-180 (Codex review P2, pipeline authentication): the 8+4 hex fingerprint ALONE is only a diagnostic. Without the full pair (decoded hash +
     // complete container SHA-256) the PS is NOT accepted, the live values are reported (ps_unverified) and the caller counts the refusal.
     CHECK(!id.ps_match&&id.ps_unverified&&std::char_traits<char>::length(id.ps_digest.data())==64&&std::string_view(id.ps_digest.data()).substr(0,8)=="78298093"&&
           std::string_view(id.ps_digest.data()).substr(60)=="33dd");
     psHash=id.ps_hash.data();psDigest=id.ps_digest.data();}
    {// The full pair accepts; one wrong hex digit anywhere in either half refuses (and stays visible as unverified while the fingerprint matches).
     AcmPins full=e10;full.ps_hash=psHash.c_str();full.ps_digest=psDigest.c_str();
     auto run=[&](const AcmPins& p){return acm_world_identity(blobs[0].data(),blobs[0].size(),blobs[1].data(),blobs[1].size(),&p);};
     {const auto id=run(full);CHECK(id.vs_match&&id.ps_fingerprint&&id.ps_match&&!id.ps_unverified);}
     std::string badDigest=psDigest;badDigest[20]=badDigest[20]=='0'?'1':'0';AcmPins bd=full;bd.ps_digest=badDigest.c_str();
     {const auto id=run(bd);CHECK(id.ps_fingerprint&&!id.ps_match&&id.ps_unverified);}
     std::string badHash=psHash;badHash[5]=badHash[5]=='0'?'1':'0';AcmPins bh=full;bh.ps_hash=badHash.c_str();
     {const auto id=run(bh);CHECK(id.ps_fingerprint&&!id.ps_match&&id.ps_unverified);}
     AcmPins noHash=full;noHash.ps_hash=nullptr;{const auto id=run(noHash);CHECK(!id.ps_match&&id.ps_unverified);}   // an unknown pin never matches
     AcmPins noDigest=full;noDigest.ps_digest=nullptr;{const auto id=run(noDigest);CHECK(!id.ps_match&&id.ps_unverified);}
     // The production pins are the documented pair (acm-shader-dataflow.md section 0); they do not match the cockpit shaders.
     const auto prod=acm_world_identity(blobs[0].data(),blobs[0].size(),blobs[1].data(),blobs[1].size());
     CHECK(!prod.ps_match&&!prod.ps_unverified&&!prod.ps_fingerprint);
     auto changed=blobs[1];changed.back()^=1;const auto mut=run(full);(void)mut;
     const auto id2=acm_world_identity(blobs[0].data(),blobs[0].size(),changed.data(),changed.size(),&full);CHECK(!id2.ps_match&&!id2.ps_fingerprint&&!id2.ps_unverified);}
    {// The real DrawACM pins do not match the cockpit shaders (default pins).
     const auto id=acm_world_identity(blobs[0].data(),blobs[0].size(),blobs[1].data(),blobs[1].size());
     CHECK(!id.vs_match&&!id.vs_hash_only&&!id.ps_fingerprint);}
    {// Mutation: executable bitcode changed, HASH untouched -> hash-only (never a match); a changed PS breaks the fingerprint.
     auto vs=blobs[0],ps=blobs[1];vs.back()^=1;ps.back()^=1;
     const auto id=acm_world_identity(vs.data(),vs.size(),ps.data(),ps.size(),&e10);
     CHECK(!id.vs_match&&id.vs_hash_only&&!id.ps_fingerprint);}
    {// Pin mutation: one wrong hex digit in each pin -> no match at all.
     AcmPins bad=e10;bad.vs_hash="f4cfd928c11d0a822baeb92e09767135";bad.ps_suffix4="33de";
     const auto id=acm_world_identity(blobs[0].data(),blobs[0].size(),blobs[1].data(),blobs[1].size(),&bad);
     CHECK(!id.vs_match&&!id.vs_hash_only&&!id.ps_fingerprint);
     AcmPins badDigest=e10;badDigest.vs_digest="adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dc";
     const auto id2=acm_world_identity(blobs[0].data(),blobs[0].size(),nullptr,0,&badDigest);
     CHECK(!id2.vs_match&&id2.vs_hash_only);}
    {// Roles are not interchangeable: the PS container in the VS slot, nothing, junk.
     const auto id=acm_world_identity(blobs[1].data(),blobs[1].size(),blobs[0].data(),blobs[0].size(),&e10);
     CHECK(!id.vs_match&&!id.ps_fingerprint);
     const auto none=acm_world_identity(nullptr,0,nullptr,0,&e10);CHECK(!none.vs_match&&!none.vs_hash_only&&!none.ps_fingerprint);
     const auto jk=acm_world_identity(junk,sizeof junk,junk,sizeof junk,&e10);CHECK(!jk.vs_match&&!jk.vs_hash_only&&!jk.ps_fingerprint);}
}
// U-soak (Batch U soak, 2026-10-29): a LATER optional I/O failure of the shared log stops that log only. Before, ownership_pins()
// followed enabled(): the L3 observation switched off, every cockpit draw went out-of-class (t10 unknown), the step-5 producer and its
// XR frames stopped and xr-stereo.log lost its routing for the rest of the process. Pins and feature logs now keep their latched state.
static void failSafeAfterStart(TestPaths& paths) {
    for(auto fault:{MemoryIO::Fault::open,MemoryIO::Fault::short_write,MemoryIO::Fault::disk_full,MemoryIO::Fault::size}) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";
        Log log(paths,io,clockNow,true);log.flush();
        CHECK(log.enabled()&&log.started()&&log.feature_logs_enabled()&&log.ownership_pins()&&!log.counters().io_failures);
        io.fault=fault;CHECK(log.push(record(Event::summary,777777)));log.flush();
        CHECK(!log.enabled()&&log.counters().io_failures==1);                       // the shared log itself is off for good
        CHECK(log.started()&&log.feature_logs_enabled()&&log.ownership_pins());      // runtime state unchanged
        const auto calls=io.calls;CHECK(!log.push(record(Event::summary,1)));log.flush();CHECK(io.calls==calls);
    }
    // A failure DURING initialization (after the configuration latched the pins) leaves every pin off.
    for(auto fault:{MemoryIO::Fault::directory,MemoryIO::Fault::reset,MemoryIO::Fault::size}) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true\ndiagnostic_ownership_pins=true";io.fault=fault;
        Log log(paths,io,clockNow,true);log.flush();
        CHECK(!log.enabled()&&!log.started()&&!log.feature_logs_enabled()&&!log.ownership_pins()&&log.counters().io_failures==1);
    }
    {MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=false\ndiagnostic_ownership_pins=true";
     Log log(paths,io,clockNow,true);log.flush();CHECK(!log.enabled()&&!log.started()&&!log.feature_logs_enabled()&&!log.ownership_pins());}
}
// U-soak: a rotation that cannot rename (an external reader without FILE_SHARE_DELETE) drops the pending ordinary records, keeps the
// file at its cap and retries on the next flush; never a log failure. Both the standalone and the production (batched) writer.
static void rotationIsTransient(TestPaths& paths) {
    for(bool production:{false,true}) {
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,production);log.flush();CHECK(log.enabled());
        io.files[0].assign(max_file_size,'x');io.fault=MemoryIO::Fault::rotate;
        const auto before=log.counters();
        CHECK(log.push(record(Event::summary,777777)));log.flush();
        const auto c=log.counters();
        CHECK(log.enabled()&&c.io_failures==0&&c.rotation_failures>=1&&c.dropped>before.dropped&&io.files[0].size()==max_file_size);
        CHECK(io.files[0].find("|a=777777|")==std::string::npos);
        io.fault=MemoryIO::Fault::none;CHECK(log.push(record(Event::summary,888888)));log.flush();
        const auto d=log.counters();
        CHECK(log.enabled()&&d.io_failures==0&&d.rotation_failures==c.rotation_failures&&d.rotations>c.rotations);
        CHECK(io.files[1].size()==max_file_size&&io.files[0].size()<max_file_size&&io.files[0].find("|a=888888|")!=std::string::npos);
    }
}
// U-soak, real files: a concurrent reader that shares read+write but not delete (PowerShell Select-String / .NET FileShare.ReadWrite,
// what the Batch U soak sampler did every 30 s). Appends and session appends must succeed (the old _wfopen_s open was not shareable:
// the append failed and Log::fail() ended the log); the rename fails while the reader is open and the log survives it.
static void sharedReaderFiles(const fs::path& base) {
    TestPaths paths(base/L"shared-reader");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    const auto file=paths.value.logs/L"diagnostics.log";
    for(const auto& f:{paths.value.config,file,paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2"})std::ofstream(f).close();
    auto openReader=[&]{return CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);};
    FileIO io;
    {HANDLE r=openReader();CHECK(r!=INVALID_HANDLE_VALUE);
     CHECK(io.append(file,"a\n",2)==2);
     io.begin_batch();CHECK(io.append(file,"b\n",2)==2);CHECK(io.end_batch());
     CHECK(!io.rotate(file,max_rotated_files));
     CloseHandle(r);}
    CHECK(io.rotate(file,max_rotated_files));
    Log log(paths,io,clockNow);log.flush();CHECK(log.enabled());
    {std::ofstream full(file,std::ios::binary|std::ios::trunc);full<<std::string(max_file_size,'x');}
    {HANDLE r=openReader();CHECK(r!=INVALID_HANDLE_VALUE);
     log.push(record(Event::device,424242));log.flush();
     CHECK(log.enabled()&&log.counters().io_failures==0&&log.counters().rotation_failures>=1&&fs::file_size(file)==max_file_size);
     CloseHandle(r);}
    log.push(record(Event::device,434343));log.flush();
    CHECK(log.enabled()&&log.counters().io_failures==0&&fs::file_size(file)<max_file_size);
    CHECK(fs::file_size(paths.value.logs/L"diagnostics.log.1")==max_file_size);
}
static void realFiles(const fs::path& base) {
    TestPaths paths(base/L"real");
    // Resolve sandbox paths before exercising the actual file writer/rotations.
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2"})
        std::ofstream(file).close();
    FileIO io;Log log(paths,io,clockNow);
    for(size_t i=0;i<=max_records/ring_capacity;++i) {for(size_t j=0;j<ring_capacity;++j)log.push(record(Event::device,i,j));log.flush();}
    CHECK(log.counters().io_failures==0&&log.counters().written==max_records);
    size_t files=0;for(auto& entry:fs::directory_iterator(paths.value.logs)) {
        CHECK(entry.file_size()<=max_file_size);++files;
        std::ifstream f(entry.path(),std::ios::binary);std::string line;
        while(std::getline(f,line))CHECK(line.size()+1<=max_record_length);
    }CHECK(files==max_rotated_files+1);
    CHECK(!io.rotate(paths.value.logs/L"diagnostics.log",max_rotated_files+1));
    fs::create_directories(paths.value.config.parent_path());
    {std::ofstream config(paths.value.config,std::ios::binary);config<<"log_enabled=false\n";}
    CHECK(!load_config(io,paths.value.config).config.logging());
    {std::ofstream config(paths.value.config,std::ios::binary);config<<std::string(max_config_bytes+1,'x');}
    CHECK(load_config(io,paths.value.config).issue==ConfigIssue::oversized);
}
static void hudMarkersLatch(TestPaths& input) {
    // J-hud-markers: each key latches only together with the keys it extends (explicit logging + prerequisites); never alone.
    TestPaths paths(input.value.logs.parent_path()/L"markers-latch-paths");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    using P=std::pair<bool,bool>;
    auto latched=[&](const char* config) {
        MemoryIO io;io.readState=ReadState::ok;io.config=config;
        Log log(paths,io,clockNow,true);log.flush();
        return P(log.gpu_copy_step5_hud_markers(),log.xr_hud_markers_layer());
    };
    CHECK(latched("log_enabled=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud_markers=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=true")==P(true,false));
    CHECK(latched("log_enabled=true\nxr_hud_markers_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_stereo_output=true\nxr_hud_markers_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_hud_layer=true\nxr_hud_markers_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_stereo_output=true\nxr_hud_layer=true\nxr_hud_markers_layer=true")==P(false,true));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=true\nxr_stereo_output=true\nxr_hud_layer=true\nxr_hud_markers_layer=true")==P(true,true));
    CHECK(latched("log_enabled=false\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=true\nxr_stereo_output=true\nxr_hud_layer=true\nxr_hud_markers_layer=true")==P(false,false));
    CHECK(latched("gpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=true\nxr_stereo_output=true\nxr_hud_layer=true\nxr_hud_markers_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_markers=maybe")==P(false,false)); // a rejected config latches nothing
}
static void hudRadarLatch(TestPaths& input) {
    // J-hud-radar: gpu_copy_step5_hud_radar latches only together with gpu_copy_step5_hud (explicit logging); never alone; default off.
    TestPaths paths(input.value.logs.parent_path()/L"radar-latch-paths");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    auto latched=[&](const char* config) {
        MemoryIO io;io.readState=ReadState::ok;io.config=config;
        Log log(paths,io,clockNow,true);log.flush();
        return log.gpu_copy_step5_hud_radar();
    };
    CHECK(!latched("log_enabled=true"));
    CHECK(!latched("log_enabled=true\ngpu_copy_step5_hud_radar=true"));
    CHECK(!latched("log_enabled=true\ngpu_copy_step5_hud=true"));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_radar=true"));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=1\ngpu_copy_step5_hud_radar=1"));
    CHECK(!latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_radar=false"));
    CHECK(!latched("log_enabled=false\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_radar=true"));
    CHECK(!latched("gpu_copy_step5_hud=true\ngpu_copy_step5_hud_radar=true"));
    CHECK(!latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_hud_radar=maybe")); // a rejected config latches nothing
}
static void effectsLatch(TestPaths& input) {
    // J-effects-layer: the producer key latches only with gpu_copy_step5_continuous AND gpu_copy_step5_hud; the consumer key only with
    // xr_stereo_output AND xr_world_in_projection (explicit logging); never alone.
    TestPaths paths(input.value.logs.parent_path()/L"effects-latch-paths");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    using P=std::pair<bool,bool>;
    auto latched=[&](const char* config) {
        MemoryIO io;io.readState=ReadState::ok;io.config=config;
        Log log(paths,io,clockNow,true);log.flush();
        return P(log.gpu_copy_step5_effects(),log.xr_effects_layer());
    };
    CHECK(latched("log_enabled=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_effects=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_effects=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true")==P(true,false));
    CHECK(latched("log_enabled=true\nxr_effects_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_stereo_output=true\nxr_effects_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_world_in_projection=true\nxr_effects_layer=true")==P(false,false));
    CHECK(latched("log_enabled=true\nxr_stereo_output=true\nxr_world_in_projection=true\nxr_effects_layer=true")==P(false,true));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\nxr_stereo_output=true\nxr_world_in_projection=true\nxr_effects_layer=true")==P(true,true));
    CHECK(latched("log_enabled=false\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\nxr_stereo_output=true\nxr_world_in_projection=true\nxr_effects_layer=true")==P(false,false));
    CHECK(latched("gpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true")==P(false,false));
    CHECK(latched("log_enabled=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=maybe")==P(false,false)); // a rejected config latches nothing
    // J-effects-dual: the dual-source key latches only together with the whole effects producer chain (a lone key stays inert).
    {auto dual=[&](const char* config){
        MemoryIO io;io.readState=ReadState::ok;io.config=config;
        Log log(paths,io,clockNow,true);log.flush();return log.gpu_copy_step5_effects_dual();};
     CHECK(!dual("log_enabled=true\ngpu_copy_step5_effects_dual=true"));
     CHECK(!dual("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects_dual=true"));
     CHECK(!dual("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true"));
     CHECK(dual("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\ngpu_copy_step5_effects_dual=true"));
     CHECK(!dual("log_enabled=false\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\ngpu_copy_step5_effects_dual=true"));
     CHECK(!dual("log_enabled=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_hud=true\ngpu_copy_step5_effects=true\ngpu_copy_step5_effects_dual=maybe"));}
    // J-occlusion-fix: xr_world_window_occlude latches only with the consumer chain (xr_stereo_output + xr_world_in_projection) AND the LATCHED depth fix
    // (continuous + members_by_class + world_objects + xr_world_objects_depth_fix); every missing link leaves it inert.
    {auto occ=[&](const char* config){
        MemoryIO io;io.readState=ReadState::ok;io.config=config;
        Log log(paths,io,clockNow,true);log.flush();return log.xr_world_window_occlude();};
     const std::string producer="gpu_copy_step5_continuous=true\ngpu_copy_step5_members_by_class=true\ngpu_copy_step5_world_objects=true\nxr_world_objects_depth_fix=true\n";
     const std::string consumer="xr_stereo_output=true\nxr_world_in_projection=true\n";
     const std::string key="xr_world_window_occlude=true\n";
     CHECK(occ(("log_enabled=true\n"+producer+consumer+key).c_str()));
     CHECK(!occ(("log_enabled=true\n"+key).c_str()));
     CHECK(!occ(("log_enabled=true\n"+consumer+key).c_str()));                       // no producer depth fix
     CHECK(!occ(("log_enabled=true\n"+producer+key).c_str()));                       // no consumer
     CHECK(!occ(("log_enabled=true\n"+producer+"xr_stereo_output=true\n"+key).c_str())); // no world-in-projection
     CHECK(!occ(("log_enabled=true\n"+producer+"xr_world_in_projection=true\n"+key).c_str())); // no xr_stereo_output
     CHECK(!occ(("log_enabled=true\n"+producer+consumer).c_str()));                  // key off (default)
     CHECK(!occ(("log_enabled=true\n"+producer+consumer+"xr_world_window_occlude=false\n").c_str()));
     CHECK(!occ(("log_enabled=true\n"+std::string("gpu_copy_step5_continuous=true\ngpu_copy_step5_members_by_class=true\ngpu_copy_step5_world_objects=true\n")+consumer+key).c_str())); // depth fix key missing
     CHECK(!occ(("log_enabled=true\n"+std::string("gpu_copy_step5_continuous=true\ngpu_copy_step5_world_objects=true\nxr_world_objects_depth_fix=true\n")+consumer+key).c_str())); // members_by_class missing
     CHECK(!occ(("log_enabled=false\n"+producer+consumer+key).c_str()));
     CHECK(!occ(("log_enabled=true\n"+producer+consumer+"xr_world_window_occlude=maybe\n").c_str()));}
}
static void listPathSnapshots(TestPaths& input) {
    // Keep paths real in this sandbox: weakly_canonical on an absent child may
    // return access_denied rather than not_found. Providers still use memory.
    TestPaths paths(input.value.logs.parent_path()/L"list-paths");
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    Log log(paths,io,clockNow,true);log.flush();auto calls=io.calls;
    {Mutex mutex;std::lock_guard lock(mutex);failNew=true;
        for(unsigned i=0;i<1000;++i)for(unsigned path=0;path<3;++path)log.list_registered(path);
        CHECK(failNew&&io.calls==calls);failNew=false;
    }
    log.flush();CHECK(io.evidence.size()<=max_evidence_size);
    for(const char* path:{"CreateCommandList","CreateCommandList1","registered-at-reset"})
        CHECK(linesWith(io.evidence,std::string("text=")+path+" ")==1);
    CHECK(linesWith(io.evidence,"|b=1000|")==3);
    const auto bytes=io.evidence.size();
    {Mutex mutex;std::lock_guard lock(mutex);log.list_registered(1);}
    log.flush();CHECK(io.evidence.size()==bytes&&linesWith(io.evidence,"|b=1001|")==1);
    CHECK(max_disk_bytes==4546432&&max_disk_bytes<=9*1024*1024/2&&!log.counters().io_failures);
    {Mutex mutex;std::lock_guard lock(mutex);acz::requirements::MetadataResult result;
     result.outcome="rejected:fixture path";log.metadata_outcome(result,{},1);}
    log.flush();CHECK(io.evidence.find(";lp=1")!=io.evidence.npos);
    // Unintercepted CreateCommandList1 plus the real Reset wrapper, with the fixture's
    // allocation injector and no E10 dependency. Native results stay intact.
    using namespace acz;
    auto dt=hook_fixture::table(),lt=hook_fixture::table();
    dt[51]=reinterpret_cast<void*>(&metadata_fixture::list1);
    lt[7]=reinterpret_cast<void*>(&metadata_fixture::getListDevice);
    lt[10]=reinterpret_cast<void*>(&metadata_fixture::reset);lt[25]=reinterpret_cast<void*>(&metadata_fixture::pso);
    FakeCOM device{dt.data()},object{lt.data()},late{lt.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
    auto* b=reinterpret_cast<ID3D12GraphicsCommandList*>(&late);
    g_protect=hook_fixture::protect;g_pinResult=true;set_sink(&log);
    metadata_fixture::listDevice=d;metadata_fixture::list1Output=&object;
    CHECK(hookCaptureDevice(d));
    FakeCOM seed{lt.data()};auto* seeded=reinterpret_cast<ID3D12GraphicsCommandList*>(&seed);
    CHECK(hookCaptureCommandList(seeded));CHECK(seeded->Release()==0);void* out=nullptr;
    failNew=true;
    CHECK(reinterpret_cast<ID3D12Device4*>(d)->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_FLAG_NONE,__uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&out==l);
    CHECK(failNew&&object.refs==1&&device.refs==1);failNew=false;
    auto it=state().states.find(l);CHECK(it==state().states.end()||!it->second.bindings.tracking_complete);
    CHECK(reinterpret_cast<ID3D12Device4*>(d)->CreateCommandList1(7,D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_FLAG_NONE,__uuidof(ID3D12GraphicsCommandList),&out)==S_FALSE&&out==l);
    CHECK(!state().states.count(l)&&l->Reset(nullptr,nullptr)==S_OK);
    CHECK(state().states.at(l).registration_path==2);
    CHECK(state().states.at(l).bindings.tracking_complete&&object.refs==1);
    failNew=true;CHECK(b->Reset(nullptr,nullptr)==S_OK);CHECK(!failNew&&late.refs==1&&device.refs==1);
    it=state().states.find(b);CHECK(it==state().states.end()||!it->second.bindings.tracking_complete);
    CHECK(b->Reset(nullptr,nullptr)==S_OK&&state().states.at(b).bindings.tracking_complete);
    CHECK(l->Release()==0&&b->Release()==0);set_sink(nullptr);
    state().devices.erase(dt.data());state().lists.erase(lt.data());
    metadata_fixture::listDevice=nullptr;metadata_fixture::list1Output=nullptr;
}

// Exhaust every reason/seam partition with allocation failure armed. Refusal
// retention is independent of ordinary-ring saturation and the outcome quota.
static void refusalSnapshots(TestPaths& paths) {
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    Log log(paths,io,clockNow,true);log.flush();
    const auto calls=io.calls,initialBytes=io.evidence.size();
    {Mutex mutex;std::lock_guard lock(mutex);failNew=true;
        for(unsigned n=0;n<1000;++n)for(unsigned r=0;r<list_refusal_count;++r)
            for(unsigned seam=0;seam<3;++seam)
                log.list_refused(static_cast<ListRefusal>(r),static_cast<ListSeam>(seam),0);
        CHECK(failNew&&io.calls==calls);failNew=false;
    }
    log.flush();CHECK(linesWith(io.evidence,"|b=1000|")==list_refusal_records);
    CHECK(io.evidence.size()==initialBytes+list_refusal_records*max_record_length);
    const auto first=io.evidence.substr(initialBytes,io.evidence.find('|',initialBytes)-initialBytes);
    for(auto* reason:list_refusal_reasons) {
        CHECK(std::strlen(reason)<128);
        CHECK(linesWith(io.evidence,std::string("text=")+reason+";seam=")==3);
    }
    const auto bytes=io.evidence.size();
    {Mutex mutex;std::lock_guard lock(mutex);
        log.list_refused(ListRefusal::null_list,ListSeam::creation,1);}
    log.flush();CHECK(io.evidence.size()==bytes&&linesWith(io.evidence,"|b=1001|")==1);
    CHECK(io.evidence.substr(initialBytes,io.evidence.find('|',initialBytes)-initialBytes)==first);
    CHECK(io.evidence.find(std::string("text=")+list_refusal_reasons[0]+";seam=creation;type=DIRECT")!=io.evidence.npos); // first example remains
    CHECK(!log.counters().io_failures&&max_disk_bytes==4546432);
}
namespace refusal_fixture {
using namespace acz;
// Faults are supplied by fake COM or the pre-existing protection/pin test
// boundary. No production callback or certification bypass is introduced.
namespace exits {
using Reason=ListRefusal;
static Reason fault=Reason::count;
static FakeCOM *selfObject=nullptr,*canonical=nullptr,*variant=nullptr,*ancestor=nullptr,*wrong=nullptr;
static unsigned unknownQueries=0;
static LPVOID deniedSlot=nullptr;
static HRESULT answer(FakeCOM* value,void** out,HRESULT hr=S_OK) {
    *out=value;if(value)hook_fixture::add(reinterpret_cast<IUnknown*>(value));return hr;
}
static HRESULT STDMETHODCALLTYPE query(IUnknown* raw,REFIID iid,void** out) {
    CHECK(!mutex_depth);*out=nullptr;auto* object=reinterpret_cast<FakeCOM*>(raw);
    if(iid==__uuidof(IUnknown)) {
        ++unknownQueries;
        if(fault==Reason::exception && unknownQueries==1)throw std::bad_alloc();
        if(fault==Reason::canonical_qi && unknownQueries==1)return E_NOINTERFACE;
        if((fault==Reason::variant_canonical && unknownQueries==2) ||
           (fault==Reason::ancestor_canonical && object==ancestor) ||
           (fault==Reason::stable_qi && object==canonical))return answer(wrong,out);
        return answer(canonical,out);
    }
    if(iid==listIids[0]) {
        if(object==canonical && fault==Reason::roundtrip_qi)return answer(variant,out);
        if(fault==Reason::base_variant_missing)return E_NOINTERFACE;
        if(fault==Reason::variant_unexpected)return answer(selfObject,out,E_NOINTERFACE);
        if(fault==Reason::variant_qi)return S_OK; // success/null is invalid COM output
        return answer(fault==Reason::self_missing?variant:selfObject,out);
    }
    if(iid==listIids[1])return answer(variant,out);
    for(unsigned i=2;i<std::size(listIids);++i)if(iid==listIids[i])return E_NOINTERFACE;
    if(iid==listBaseIids[0]) {
        if(fault==Reason::ancestor_unexpected)return answer(ancestor,out,E_NOINTERFACE);
        if(fault==Reason::ancestor_qi)return S_OK;
        return answer(ancestor,out);
    }
    return E_NOINTERFACE;
}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE getType(ID3D12CommandList*) {
    CHECK(!mutex_depth);
    return fault==Reason::non_direct_bundle?D3D12_COMMAND_LIST_TYPE_BUNDLE:
        fault==Reason::non_direct_other?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT;
}
static BOOL WINAPI protect(LPVOID address,SIZE_T size,DWORD flags,DWORD* old) {
    if(address==deniedSlot){*old=PAGE_READONLY;return FALSE;}
    return hook_fixture::protect(address,size,flags,old);
}
static std::array<void*,128> table() {
    std::array<void*,128> value{};value.fill(reinterpret_cast<void*>(&hook_fixture::unused));
    const auto base=hook_fixture::table();std::copy(base.begin(),base.end(),value.begin());
    value[7]=reinterpret_cast<void*>(&metadata_fixture::getListDevice);
    value[10]=reinterpret_cast<void*>(&metadata_fixture::reset);
    value[13]=reinterpret_cast<void*>(&metadata_fixture::indexed);
    return value;
}
static unsigned creations=0;
static HRESULT STDMETHODCALLTYPE create(ID3D12Device*,UINT node,D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator,ID3D12PipelineState* initial,REFIID iid,void** out) {
    CHECK(!mutex_depth&&node==7&&type==D3D12_COMMAND_LIST_TYPE_DIRECT);
    CHECK(!allocator&&!initial&&iid==__uuidof(ID3D12GraphicsCommandList)&&out);
    ++creations;*out=selfObject;return S_FALSE;
}
struct Case {Reason reason;const char* text;};
static constexpr Case cases[]={
    {Reason::null_list,"rejected:list certification null list"},
    {Reason::pin,"rejected:list certification proxy pin failed"},
    {Reason::canonical_qi,"rejected:list certification canonical QI failed/null"},
    {Reason::non_direct_bundle,"rejected:list certification non-direct BUNDLE"},
    {Reason::non_direct_other,"rejected:list certification non-direct other"},
    {Reason::variant_unexpected,"rejected:list certification variant E_NOINTERFACE output"},
    {Reason::variant_qi,"rejected:list certification variant QI failed/null"},
    {Reason::variant_canonical,"rejected:list certification variant canonical mismatch"},
    {Reason::ancestor_unexpected,"rejected:list certification ancestor E_NOINTERFACE output"},
    {Reason::ancestor_qi,"rejected:list certification ancestor QI failed/null"},
    {Reason::ancestor_canonical,"rejected:list certification ancestor canonical mismatch"},
    {Reason::base_variant_missing,"rejected:list certification base variant missing"},
    {Reason::stable_qi,"rejected:list certification stable QI mismatch"},
    {Reason::roundtrip_qi,"rejected:list certification round-trip QI mismatch"},
    {Reason::self_missing,"rejected:list certification self absent from variants"},
    {Reason::canonical_alias,"rejected:list certification canonical alias conflict"},
    {Reason::variant_alias,"rejected:list certification variant alias conflict"},
    {Reason::ancestor_alias,"rejected:list certification ancestor alias conflict"},
    {Reason::canonical_table,"rejected:list certification canonical table unavailable"},
    {Reason::canonical_lifetime,"rejected:list certification canonical lifetime patch failed"},
    {Reason::variant_table,"rejected:list certification variant table unavailable"},
    {Reason::variant_lifetime,"rejected:list certification variant lifetime patch failed"},
    {Reason::ancestor_table,"rejected:list certification ancestor table unavailable"},
    {Reason::ancestor_lifetime,"rejected:list certification ancestor lifetime patch failed"},
    {Reason::setter_patch,"rejected:list certification setter patch failed"},
    {Reason::exception,"rejected:list certification exception/allocation failure"}
};
static_assert(std::size(cases)+1==list_refusal_count);
// setter_table is intentionally absent: all variant tables pass the same
// availability predicate in the lifetime loop under the same mutex, without
// intervening registry mutation. Do not manufacture an unsafe vtable race.
static void run(TestPaths& paths,const Case& test,ListSeam seam,unsigned category=8,bool shared=false,bool sameSelf=false,bool sameAncestor=false) {
    auto lt=table(),ct=table(),vt=table(),at=table(),wt=table(),qt=table();
    auto& s=state();fault=Reason::count;g_pinResult=true;g_protect=hook_fixture::protect;
    metadata_fixture::listDevice=nullptr;hook_fixture::queryMode=0;
    // A sibling established the shared graphics dispatch, not this object's
    // identity. The later draw therefore exercises the real patched wrapper.
    FakeCOM seed{lt.data()};
    CHECK(hookCaptureCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(&seed)));
    CHECK(reinterpret_cast<IUnknown*>(&seed)->Release()==0);
    FakeCOM object{lt.data()},identity{ct.data()},extension{vt.data()},base{at.data()},other{wt.data()},queue{qt.data()};
    identity.owner=extension.owner=base.owner=&object;
    auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
    selfObject=&object;canonical=&identity;variant=&extension;ancestor=&base;wrong=&other;
    FakeCOM* conflict=test.reason==Reason::canonical_alias?canonical:
        test.reason==Reason::variant_alias?variant:test.reason==Reason::ancestor_alias?ancestor:nullptr;
    if(conflict) {
        // Establish another certified routing first. The subsequent scripted
        // COM answers contradict that prior identity and must be refused.
        conflict->owner=nullptr;
        CHECK(hookCaptureCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(conflict)));
        CHECK(conflict->refs==1);conflict->owner=&object;
    }
    for(auto* t:{&lt,&ct,&vt,&at})(*t)[0]=reinterpret_cast<void*>(&query);
    lt[8]=reinterpret_cast<void*>(&getType);
    void** collision=test.reason==Reason::canonical_table?ct.data():
        test.reason==Reason::variant_table?vt.data():test.reason==Reason::ancestor_table?at.data():nullptr;
    // A readable table already classified as another interface family is
    // unavailable to list certification. This is fixture registry setup only.
    if(collision && test.reason==Reason::canonical_table) {
        if(sameSelf)canonical=&object;
        if(sameAncestor)ancestor=canonical;
        if(shared)canonical->table=lt.data();
        collision=canonical->table;
    }
    if(collision) {
        if(category&1)s.compatibility.try_emplace(collision);
        if(category&2)s.devices.try_emplace(collision);
        if(category&4)s.objects.try_emplace(collision);
        if(category&8)s.queues.try_emplace(collision);
    }

    deniedSlot=test.reason==Reason::canonical_lifetime?static_cast<LPVOID>(&ct[2]):
        test.reason==Reason::variant_lifetime?static_cast<LPVOID>(&vt[2]):
        test.reason==Reason::ancestor_lifetime?static_cast<LPVOID>(&at[2]):nullptr;
    if(test.reason==Reason::setter_patch) {
        lt[28]=s.lists.at(lt.data()).originals[28];deniedSlot=&lt[28];
    }
    qt[10]=reinterpret_cast<void*>(&hook_fixture::execute);
    auto* q=reinterpret_cast<ID3D12CommandQueue*>(&queue);
    if(seam==ListSeam::execute)CHECK(hookCommandQueue(q));
    auto dt=table();dt[12]=reinterpret_cast<void*>(&create);FakeCOM device{dt.data()};
    auto* d=reinterpret_cast<ID3D12Device*>(&device);
    const bool created=test.reason==Reason::canonical_table&&seam==ListSeam::creation;
    if(created)CHECK(hookCaptureDevice(d));
    std::array<void*,128> collisionBefore{};
    if(collision)std::copy_n(collision,collisionBefore.size(),collisionBefore.begin());
    const auto statesBefore=s.states.size(),aliasesBefore=s.listKeys.size();
    MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
    Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
    // Each invocation models a fresh process's first category example.
    s.canonicalTableReported.fill(false);
    fault=test.reason;unknownQueries=0;g_protect=protect;g_pinResult=fault!=Reason::pin;
    if(seam==ListSeam::reset)CHECK(l->Reset(nullptr,nullptr)==S_OK);
    else if(seam==ListSeam::execute) {
        ID3D12CommandList* submitted[]={l};hook_fixture::expectedLists=submitted;
        const auto before=hook_fixture::executes;g_captureDiscoveryTest=true;
        q->ExecuteCommandLists(1,submitted);g_captureDiscoveryTest=false;
        CHECK(hook_fixture::executes==before+1);hook_fixture::expectedLists=nullptr;
    } else if(created) {
        const auto before=creations;void* output=nullptr;
        CHECK(d->CreateCommandList(7,D3D12_COMMAND_LIST_TYPE_DIRECT,nullptr,nullptr,
            __uuidof(ID3D12GraphicsCommandList),&output)==S_FALSE);
        CHECK(output==l&&creations==before+1&&device.refs==1);
    } else {
        bool threw=false;
        try {CHECK(!hookCaptureCommandList(fault==Reason::null_list?nullptr:l));}
        catch(const std::bad_alloc&){threw=true;}
        CHECK(threw==(fault==Reason::exception)); // preserve installer exception contract
    }
    deniedSlot=nullptr;g_protect=hook_fixture::protect;g_pinResult=true;
    CHECK(object.refs==1&&other.refs==1); // every temporary QI output was released
    const bool retained=fault==Reason::setter_patch;
    if(retained) {
        // Complete Release routing legitimately retains a fail-closed gap
        // until final Release; requiring immediate erasure would be incorrect.
        const auto& point=s.states.at(l);
        CHECK(!point.capture_hooks&&!point.interfaces_observed&&!point.bindings.list_identity);
        CHECK(std::string_view(point.observation_gap)==test.text);
    } else CHECK(!s.states.count(l)&&!s.listKeys.count(l));
    for(auto* alias:{canonical,variant,ancestor}) {
        if(alias==conflict)CHECK(s.listKeys.at(alias)==reinterpret_cast<ID3D12GraphicsCommandList*>(conflict));
        else if(!retained)CHECK(!s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(alias))&&!s.listKeys.count(alias));
    }
    CHECK(s.states.size()==statesBefore+(retained?1:0));
    CHECK(s.listKeys.size()==aliasesBefore+(retained?4:0));
    const auto beforeDraw=metadata_fixture::nativeDraws;
    // Null input has no object on which a meaningful post-refusal draw exists.
    if(fault!=Reason::null_list) {
        if(fault==Reason::canonical_table) {
            l->DrawIndexedInstanced(9,1,0,0,0);
            CHECK(metadata_fixture::forwarded[0]==9&&metadata_fixture::forwarded[1]==1&&
                metadata_fixture::forwarded[2]==0&&metadata_fixture::forwarded[3]==0&&metadata_fixture::forwarded[4]==0);
        } else {
            l->DrawIndexedInstanced(19,2,3,-4,5);
            CHECK(metadata_fixture::forwarded[0]==19&&metadata_fixture::forwarded[1]==2&&
                metadata_fixture::forwarded[2]==3&&metadata_fixture::forwarded[3]==UINT(-4)&&metadata_fixture::forwarded[4]==5);
        }
        CHECK(metadata_fixture::nativeDraws==beforeDraw+1);
    }
    CHECK(s.states.size()==statesBefore+(retained?1:0));
    CHECK(s.listKeys.size()==aliasesBefore+(retained?4:0));
    log.flush();
    const char* seamText=seam==ListSeam::creation?"creation":seam==ListSeam::reset?"Reset":"ExecuteCommandLists";
    const char* typeText=fault==Reason::null_list||fault==Reason::pin||fault==Reason::canonical_qi||fault==Reason::exception?
        "unobserved":fault==Reason::non_direct_bundle?"BUNDLE":fault==Reason::non_direct_other?"other":"DIRECT";
    if(fault==Reason::canonical_table) {
        const auto mask=category|(shared?16:0);
        const auto equality=(sameSelf?3:0)|(sameAncestor?36:0)|(shared?24:0);
        const auto tag="|device|a="+std::to_string(mask)+"|b="+std::to_string(equality)+
            "|c="+std::to_string(static_cast<unsigned>(seam))+"|identity=unknown|hash=|sha256=|text=list canonical table categories";
        CHECK(io.evidence.find(tag)!=io.evidence.npos);
        const auto protections=hook_fixture::protects;
        // Repeated refusal never patches the conflicting table or emits an
        // unbounded category stream. Refusal snapshots still count repeats.
        CHECK(!hookCaptureCommandList(l,seam));log.flush();
        CHECK(linesWith(io.evidence,"text=list canonical table categories")==1);
        CHECK(hook_fixture::protects==protections);
        CHECK(std::equal(collisionBefore.begin(),collisionBefore.end(),collision));
        CHECK(object.refs==1&&!s.states.count(l)&&!s.listKeys.count(l));
    }
    CHECK(linesWith(io.evidence,";seam=")==1);
    CHECK(io.evidence.find(std::string("text=")+test.text+";seam="+seamText+";type="+typeText)!=io.evidence.npos);
    fault=Reason::count;
    CHECK(reinterpret_cast<IUnknown*>(conflict?conflict:&object)->Release()==0);
    for(auto* alias:{&object,canonical,variant,ancestor})
        CHECK(!s.states.count(reinterpret_cast<ID3D12GraphicsCommandList*>(alias))&&!s.listKeys.count(alias));
    CHECK(other.refs==1);set_sink(nullptr);
    if(collision) {
        if(category&1)s.compatibility.erase(collision);
        if(category&2)s.devices.erase(collision);
        if(category&4)s.objects.erase(collision);
        if(category&8)s.queues.erase(collision);
    }
    s.queues.erase(qt.data());s.devices.erase(dt.data());
    for(auto* t:{&lt,&ct,&vt,&at})s.lists.erase(t->data());
    selfObject=canonical=variant=ancestor=wrong=nullptr;
}
// Expected-red on J-096: real metadata/list hooks establish a20/b0,
// then each native seam must certify the distinct canonical interface.
static HRESULT STDMETHODCALLTYPE failedReset(ID3D12GraphicsCommandList*,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso) {
    CHECK(!mutex_depth&&!allocator&&!pso);return E_FAIL;
}
static HRESULT STDMETHODCALLTYPE allocationDevice(ID3D12GraphicsCommandList* self,REFIID iid,void** out) {
    auto hr=metadata_fixture::getListDevice(self,iid,out);failNew=true;return hr;
}
static void sharedCanonical(TestPaths& paths) {
    unsigned refusedSeams=0;
    for(auto seam:{ListSeam::creation,ListSeam::reset,ListSeam::execute})
    for(unsigned finalAlias=0;finalAlias<4;++finalAlias)
    for(bool metadataFirst:{false,true}) {
        auto lt=table(),ct=table(),vt=table(),at=table(),dt=table(),qt=table();
        auto& s=state();fault=Reason::count;g_pinResult=true;g_protect=hook_fixture::protect;
        metadata_fixture::listDevice=nullptr;hook_fixture::queryMode=0;
        FakeCOM seed{lt.data()},canonicalSeed{ct.data()};
        CHECK(hookCaptureCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(&seed)));
        CHECK(reinterpret_cast<IUnknown*>(&seed)->Release()==0);
        if(!metadataFirst) {
            CHECK(hookCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(&canonicalSeed)));
            CHECK(reinterpret_cast<IUnknown*>(&canonicalSeed)->Release()==0);
        }
        FakeCOM peer{ct.data()};CHECK(trackMetadataLifetime(reinterpret_cast<IUnknown*>(&peer)));
        const auto releaseEntry=ct[2];
        FakeCOM object{lt.data()},identity{ct.data()},extension{vt.data()},base{at.data()};
        identity.owner=extension.owner=base.owner=&object;
        selfObject=&object;canonical=&identity;variant=&extension;ancestor=&base;wrong=nullptr;
        for(auto* t:{&lt,&ct,&vt,&at})(*t)[0]=reinterpret_cast<void*>(&query);
        lt[8]=reinterpret_cast<void*>(&getType);
        auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
        dt[12]=reinterpret_cast<void*>(&create);FakeCOM device{dt.data()};
        auto* d=reinterpret_cast<ID3D12Device*>(&device);CHECK(hookCaptureDevice(d));
        qt[10]=reinterpret_cast<void*>(&hook_fixture::execute);FakeCOM queue{qt.data()};
        auto* q=reinterpret_cast<ID3D12CommandQueue*>(&queue);CHECK(hookCommandQueue(q));
        CHECK(s.objects.count(ct.data())&&((s.lists.count(ct.data())!=0)==!metadataFirst)); // a20 or metadata-first a4
        CHECK(ct.data()!=lt.data()&&ct.data()!=vt.data()&&ct.data()!=at.data()); // b0
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();set_sink(&log);s.canonicalTableReported.fill(false);
        if(seam==ListSeam::creation) {
            void* output=nullptr;const auto before=creations;
            CHECK(d->CreateCommandList(7,D3D12_COMMAND_LIST_TYPE_DIRECT,nullptr,nullptr,
                __uuidof(ID3D12GraphicsCommandList),&output)==S_FALSE);
            CHECK(output==l&&creations==before+1);
        } else if(seam==ListSeam::reset) CHECK(l->Reset(nullptr,nullptr)==S_OK);
        else {
            ID3D12CommandList* submitted[]={l};hook_fixture::expectedLists=submitted;
            g_captureDiscoveryTest=true;q->ExecuteCommandLists(1,submitted);g_captureDiscoveryTest=false;
            hook_fixture::expectedLists=nullptr;
        }
        auto cleanup=[&] {
            set_sink(nullptr);s.objects.erase(ct.data());s.devices.erase(dt.data());s.queues.erase(qt.data());
            for(auto* t:{&lt,&ct,&vt,&at})s.lists.erase(t->data());
            selfObject=canonical=variant=ancestor=wrong=nullptr;
        };
        if(!s.states.count(l)||!s.states.at(l).interfaces_observed) {
            // Unchanged production reaches ALL three real native seams before
            // the aggregate expected-red assertion, retaining the a20/b0 proof.
            ++refusedSeams;log.flush();
            const auto tag="|device|a="+std::to_string(metadataFirst?4:20)+"|b=0|c="+
                std::to_string(static_cast<unsigned>(seam))+"|";
            CHECK(io.evidence.find(tag)!=io.evidence.npos);
            CHECK(io.evidence.find("text=rejected:list certification canonical table unavailable")!=io.evidence.npos);
            CHECK(!s.states.count(l)&&object.refs==1&&peer.refs==1);
            CHECK(reinterpret_cast<IUnknown*>(&identity)->Release()==0);
            CHECK(reinterpret_cast<IUnknown*>(&peer)->Release()==0);cleanup();continue;
        }
        // Exactly one registry retains the native Release original.
        CHECK((s.objects.at(ct.data()).originals[2]!=nullptr)==metadataFirst);
        CHECK(s.states.at(l).cleanup_established&&object.refs==1&&peer.refs==1&&ct[2]==releaseEntry);
        CHECK(!trackMetadataLifetime(reinterpret_cast<IUnknown*>(&identity))); // per-object collision
        if(seam==ListSeam::execute) {
            CHECK(!s.states.at(l).bindings.tracking_complete&&!s.states.at(l).reset_observed);
            l->ClearState(nullptr);CHECK(!s.states.at(l).bindings.tracking_complete);
            auto& originalReset=s.lists.at(lt.data()).originals[10];auto savedReset=originalReset;
            originalReset=reinterpret_cast<void*>(&failedReset);
            CHECK(l->Reset(nullptr,nullptr)==E_FAIL&&!s.states.at(l).reset_observed);
            CHECK(!s.states.at(l).bindings.tracking_complete);originalReset=savedReset;
            CHECK(l->Reset(nullptr,nullptr)==S_OK);
        }
        CHECK(s.states.at(l).bindings.tracking_complete);
        for(auto* alias:{&object,&identity,&extension,&base})CHECK(s.listKeys.at(alias)==l);
        const auto token=s.states.at(l).bindings.list_identity;
        auto* peerRoot=reinterpret_cast<ID3D12RootSignature*>(&peer);
        s.rootKeys[reinterpret_cast<IUnknown*>(&peer)]=peerRoot;
        s.rootIdentities[peerRoot]=capture::new_lifetime_identity();
        CHECK(reinterpret_cast<IUnknown*>(&peer)->AddRef()==2);
        CHECK(reinterpret_cast<IUnknown*>(&peer)->Release()==1);
        CHECK(reinterpret_cast<IUnknown*>(&peer)->Release()==0);
        CHECK(!s.rootIdentities.count(peerRoot)&&!s.rootKeys.count(reinterpret_cast<IUnknown*>(&peer)));
        CHECK(s.states.at(l).bindings.list_identity==token); // independent peer
        if(seam==ListSeam::execute&&finalAlias==0&&!metadataFirst) {
            FakeCOM sibling{lt.data()},siblingIdentity{ct.data()},siblingVariant{vt.data()},siblingBase{at.data()};
            siblingIdentity.owner=siblingVariant.owner=siblingBase.owner=&sibling;
            selfObject=&sibling;canonical=&siblingIdentity;variant=&siblingVariant;ancestor=&siblingBase;
            auto* other=reinterpret_cast<ID3D12GraphicsCommandList*>(&sibling);
            CHECK(hookCaptureCommandList(other)&&other->Reset(nullptr,nullptr)==S_OK);
            const auto siblingToken=s.states.at(other).bindings.list_identity;
            selfObject=&object;canonical=&identity;variant=&extension;ancestor=&base;
            CHECK(s.states.at(l).bindings.list_identity==token); // independent list sharing canonical table
            // A partial setter patch retains per-object lifetime routing for
            // both owners, with direct versus propagated refusal attribution.
            lt[28]=s.lists.at(lt.data()).originals[28];deniedSlot=&lt[28];g_protect=protect;
            CHECK(!hookCaptureCommandList(l));deniedSlot=nullptr;g_protect=hook_fixture::protect;
            CHECK(s.states.at(l).cleanup_established&&s.states.at(other).cleanup_established);
            CHECK(std::string_view(s.states.at(l).observation_gap)==
                "rejected:list certification setter patch failed");
            CHECK(s.states.at(other).observation_gap==diagnostics::shared_table_invalidation);
            CHECK(!s.states.at(l).bindings.list_identity&&!s.states.at(other).bindings.list_identity);
            CHECK(reinterpret_cast<IUnknown*>(&siblingIdentity)->Release()==0&&!s.states.count(other));
            CHECK(s.states.count(l)&&s.listKeys.at(&identity)==l);
            CHECK(hookCaptureCommandList(l));
            CHECK(s.states.at(l).bindings.list_identity!=siblingToken&&!s.states.at(l).bindings.tracking_complete);
            CHECK(l->Reset(nullptr,nullptr)==S_OK&&s.states.at(l).bindings.tracking_complete);
            // A newer lifetime published during native final Release survives
            // retirement of the pre-call token, even through canonical dispatch.
            const auto newer=capture::new_lifetime_identity();
            deviceReleaseAction=[&] {s.states.at(l).bindings.list_identity=newer;};
            CHECK(reinterpret_cast<IUnknown*>(&identity)->Release()==0);
            CHECK(s.states.at(l).bindings.list_identity==newer&&s.listKeys.at(&identity)==l);
            object.refs=1; // fake replacement lifetime's caller-owned reference
        }
        l->DrawIndexedInstanced(9,1,0,0,0);
        CHECK(metadata_fixture::forwarded[0]==9&&metadata_fixture::forwarded[1]==1&&
            !metadata_fixture::forwarded[2]&&!metadata_fixture::forwarded[3]&&!metadata_fixture::forwarded[4]);
        FakeCOM* aliases[]={&object,&identity,&extension,&base};
        CHECK(reinterpret_cast<IUnknown*>(aliases[finalAlias])->Release()==0);
        CHECK(!s.states.count(l));for(auto* alias:aliases)CHECK(!s.listKeys.count(alias));
        // Same address is a new lifetime and cannot inherit a recording/gap.
        object.refs=1;
        if(seam==ListSeam::execute&&finalAlias==0&&!metadataFirst) {
            auto* canonicalAddress=reinterpret_cast<IUnknown*>(&identity);
            s.rootKeys[canonicalAddress]=reinterpret_cast<ID3D12RootSignature*>(&identity);
            CHECK(!hookCaptureCommandList(l)&&!s.states.count(l)&&object.refs==1);
            s.rootKeys.erase(canonicalAddress); // contradictory per-object ownership is refused
            auto savedGetDevice=lt[7];lt[7]=reinterpret_cast<void*>(&allocationDevice);
            const auto allocationFailures=failedNew;bool threw=false;
            try {hookCaptureCommandList(l);}catch(const std::bad_alloc&){threw=true;}
            CHECK(threw&&failedNew==allocationFailures+1&&!failNew);
            CHECK(!s.states.count(l)&&!s.listKeys.count(&identity)&&object.refs==1);
            lt[7]=savedGetDevice;
            // Recover the owning registry's one pending Release protection;
            // no secondary registry may hide or duplicate that recovery.
            ct[2]=s.lists.at(ct.data()).originals[2];
            hook_fixture::failProtect=hook_fixture::protects+2;
            CHECK(!hookCaptureCommandList(l));hook_fixture::failProtect=0;
            CHECK(ct[2]==releaseEntry&&s.lists.at(ct.data()).pendingProtection[2]);
            CHECK(!s.states.count(l)&&!s.listKeys.count(&identity)&&object.refs==1);
        }
        CHECK(hookCaptureCommandList(l));
        CHECK(!s.lists.count(ct.data())||!s.lists.at(ct.data()).pendingProtection[2]);
        CHECK(s.states.at(l).bindings.list_identity!=token&&!s.states.at(l).bindings.tracking_complete);
        CHECK(!s.states.at(l).observation_gap&&l->Reset(nullptr,nullptr)==S_OK);
        CHECK(reinterpret_cast<IUnknown*>(&identity)->Release()==0&&!s.states.count(l));
        log.flush();CHECK(io.evidence.find("text=rejected:list certification canonical table unavailable")==io.evidence.npos);
        cleanup();
    }
    CHECK(refusedSeams==0); // expected red against unchanged J-096, green after repair
}
static void tests(TestPaths& paths) {
    for(const auto& test:cases)run(paths,test,ListSeam::creation);
    // Category ownership cannot be inferred from a canonical-table refusal.
    // Every excluded registry, overlapping registries and graphics-shared
    // tables retain fail-closed semantics at all three real calling seams.
    const Case collision{Reason::canonical_table,"rejected:list certification canonical table unavailable"};
    for(auto seam:{ListSeam::creation,ListSeam::reset,ListSeam::execute})
        for(unsigned category:{1u,2u,4u,8u,15u})for(bool shared:{false,true})
            run(paths,collision,seam,category,shared);
    for(auto seam:{ListSeam::creation,ListSeam::reset,ListSeam::execute}) {
        run(paths,collision,seam,8,true,true,false); // canonical == graphics self
        run(paths,collision,seam,8,false,false,true); // canonical == ancestor
        run(paths,collision,seam,8,true,true,true); // all equality bits
    }
    // Malformed queue submission deliberately probes defensive discovery; it
    // is not a claim that a native API permits submitting bundles to a queue.
    for(const auto& test:cases)if(test.reason==Reason::non_direct_bundle||test.reason==Reason::variant_canonical)
        for(auto seam:{ListSeam::reset,ListSeam::execute})run(paths,test,seam);
}
} // namespace exits
static D3D12_COMMAND_LIST_TYPE type=D3D12_COMMAND_LIST_TYPE_DIRECT;
static FakeCOM* mismatch=nullptr;
static unsigned qiCalls=0;
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE getType(ID3D12CommandList*) {CHECK(!mutex_depth);return type;}
static HRESULT STDMETHODCALLTYPE query(IUnknown* self,REFIID iid,void** out) {
    CHECK(!mutex_depth);
    // First canonical QI succeeds; the variant's canonical QI disagrees.
    if(mismatch&&iid==__uuidof(IUnknown)&&++qiCalls==2) {
        *out=mismatch;++mismatch->refs;return S_OK;
    }
    return hook_fixture::query(self,iid,out);
}
static void tests(TestPaths& paths) {
    auto lt=hook_fixture::table(),pt=hook_fixture::table(),qt=hook_fixture::table();
    lt[0]=reinterpret_cast<void*>(&query);lt[8]=reinterpret_cast<void*>(&getType);
    lt[7]=reinterpret_cast<void*>(&metadata_fixture::getListDevice);
    lt[10]=reinterpret_cast<void*>(&metadata_fixture::reset);
    lt[25]=reinterpret_cast<void*>(&metadata_fixture::pso);
    lt[13]=reinterpret_cast<void*>(&metadata_fixture::indexed);
    qt[10]=reinterpret_cast<void*>(&hook_fixture::execute);
    FakeCOM seed{lt.data()},pipeline{pt.data()},queue{qt.data()},wrong{pt.data()};
    auto* p=reinterpret_cast<ID3D12PipelineState*>(&pipeline);
    auto* q=reinterpret_cast<ID3D12CommandQueue*>(&queue);
    auto& s=state();g_protect=hook_fixture::protect;g_pinResult=true;
    metadata_fixture::listDevice=nullptr;hook_fixture::queryMode=0;
    // No seed, no lifetime hook: an early canonical-QI refusal must not
    // publish stale state that an ordinary final Release cannot remove.
    {
        auto raw=lt;const auto originalTable=raw;
        FakeCOM early{raw.data()};auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&early);
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
        hook_fixture::queryMode=2;
        CHECK(!hookCaptureCommandList(l));hook_fixture::queryMode=0;
        CHECK(raw==originalTable&&!s.states.count(l)&&!s.listKeys.count(l));
        log.flush();
        CHECK(io.evidence.find("text=rejected:list certification canonical QI failed/null;seam=creation;type=unobserved")!=io.evidence.npos);
        CHECK(l->Release()==0&&!s.states.count(l)&&!s.listKeys.count(l));
        // Reuse the same address with a fresh lifetime; no stale gap survives.
        early.refs=1;CHECK(!s.states.count(l));CHECK(hookCaptureCommandList(l));
        CHECK(s.states.at(l).bindings.list_identity&&!s.states.at(l).observation_gap);
        CHECK(l->Release()==0&&!s.states.count(l));
        s.lists.erase(raw.data());set_sink(nullptr);
    }
    CHECK(hookCaptureCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(&seed)));
    CHECK(reinterpret_cast<IUnknown*>(&seed)->Release()==0);
    // Shared graphics hooks say nothing about this object's canonical alias.
    // Even an already-hooked canonical table cannot retire an unmapped object.
    for(bool patchedCanonical:{false,true}) {
        auto canonicalTable=hook_fixture::table();
        FakeCOM canonicalSeed{canonicalTable.data()};
        if(patchedCanonical) {
            CHECK(hookCaptureCommandList(reinterpret_cast<ID3D12GraphicsCommandList*>(&canonicalSeed)));
            CHECK(reinterpret_cast<IUnknown*>(&canonicalSeed)->Release()==0);
        }
        FakeCOM object{lt.data()},canonical{canonicalTable.data()};
        canonical.owner=&object;object.unknownInterface=&canonical;object.rootInterface=&object;
        auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
        type=D3D12_COMMAND_LIST_TYPE_BUNDLE; // Refuse before alias publication.
        CHECK(l->Reset(nullptr,nullptr)==S_OK);
        CHECK(!s.states.count(l)&&!s.listKeys.count(l)&&!s.listKeys.count(&canonical));
        log.flush();CHECK(linesWith(io.evidence,";seam=")==1);
        CHECK(io.evidence.find("text=rejected:list certification non-direct BUNDLE;seam=Reset;type=BUNDLE")!=io.evidence.npos);
        CHECK(reinterpret_cast<IUnknown*>(&canonical)->Release()==0);
        CHECK(!s.states.count(l)&&!s.listKeys.count(l)&&!s.listKeys.count(&canonical));
        object.refs=1;object.unknownInterface=nullptr;object.rootInterface=nullptr;
        type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(hookCaptureCommandList(l));
        CHECK(s.states.at(l).bindings.list_identity&&!s.states.at(l).observation_gap);
        CHECK(l->Release()==0&&!s.states.count(l)&&!s.listKeys.count(l));
        s.lists.erase(canonicalTable.data());set_sink(nullptr);
    }
    CHECK(hookCommandQueue(q));g_captureDiscoveryTest=true;
    PipelineRecord metadata;metadata.identity=capture::new_lifetime_identity();metadata.drawcpt_identity_matched=true;
    s.pipelines.insert_or_assign(p,metadata);
    g_requirementsClock=metadata_fixture::clock;
    // Rejection first: unchanged code reports bare no-list-identity here.
    for(unsigned route:{2u,0u,1u,3u}) {
        // Outcome dedup retains the first lp, so each seam has its own sink.
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();
        FakeCOM object{lt.data()};auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
        CHECK(!s.states.count(l));
        // Per-list refusal outcomes require this object's previously published
        // alias routing and lifetime hooks. Unknown early refusals are log-only.
        if(route>=2) {
            CHECK(hookCaptureCommandList(l));
            s.states.at(l).interfaces_observed=false;
        }
        type=route==2?D3D12_COMMAND_LIST_TYPE_BUNDLE:D3D12_COMMAND_LIST_TYPE_DIRECT;
        mismatch=route==3?&wrong:nullptr;qiCalls=0;
        set_sink(&log);s.requirements_attempts=0;s.requirements_last_us=0;
        if(route==1) {
            ID3D12CommandList* lists[]={l};hook_fixture::expectedLists=lists;
            q->ExecuteCommandLists(1,lists);
            const auto& discovered=s.states.at(l);
            CHECK(discovered.bindings.list_identity&&discovered.interfaces_observed&&discovered.capture_hooks);
            CHECK(!discovered.bindings.tracking_complete&&!discovered.reset_observed&&discovered.registration_path==3);
        }
        CHECK(l->Reset(nullptr,p)==S_OK);l->SetPipelineState(p);
        auto& point=s.states.at(l);
        const char* expected=route==2?"rejected:list certification non-direct BUNDLE":
            route==3?"rejected:list certification variant canonical mismatch":
            "rejected:list device identity unobserved";
        CHECK(std::string_view(listPointFailure_locked(point,point.bindings))==expected);
        CHECK((point.bindings.list_identity!=0)==(route<2));
        CHECK(point.bindings.tracking_complete==(route<2)&&point.reset_observed==(route<2));
        CHECK(point.registration_path==(route==0?2:3));
        const auto calls=io.calls;const auto before=metadata_fixture::nativeDraws;
        failNew=true;l->DrawIndexedInstanced(9,1,0,0,0);CHECK(failNew);failNew=false;
        CHECK(io.calls==calls&&metadata_fixture::nativeDraws==before+1);
        CHECK(metadata_fixture::forwarded[0]==9&&metadata_fixture::forwarded[1]==1&&
            metadata_fixture::forwarded[2]==0&&metadata_fixture::forwarded[3]==0&&metadata_fixture::forwarded[4]==0);
        log.flush();
        CHECK(io.evidence.find(std::string(expected)+";cb10=4294967295;draw=1,9,1,0,0,0;lp="+
            std::to_string(route==0?2:3))!=io.evidence.npos);
        if(route>=2)CHECK(io.evidence.find(std::string("text=")+expected+";seam=Reset")!=io.evidence.npos);
        if(route==0)CHECK(io.evidence.find("text=registered-at-reset")!=io.evidence.npos);
        CHECK(!point.bindings.pipeline.requirements_proven&&!capture::freeze(point.bindings,{},{}));
        mismatch=nullptr;type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(l->Release()==0&&!s.states.count(l));set_sink(nullptr);
    }
    // One actual setter-patch attempt invalidates the shared-table sibling,
    // but must never accuse the sibling of making that certification attempt.
    {
        FakeCOM attempted{lt.data()},sibling{lt.data()};
        auto* a=reinterpret_cast<ID3D12GraphicsCommandList*>(&attempted);
        auto* b=reinterpret_cast<ID3D12GraphicsCommandList*>(&sibling);
        CHECK(hookCaptureCommandList(a)&&hookCaptureCommandList(b));
        CHECK(a->Reset(nullptr,p)==S_OK&&b->Reset(nullptr,p)==S_OK);
        MemoryIO io;io.readState=ReadState::ok;io.config="log_enabled=true";
        Log log(paths,io,clockNow,true);log.flush();set_sink(&log);
        lt[28]=s.lists.at(lt.data()).originals[28];
        hook_fixture::failProtect=hook_fixture::protects+1;
        CHECK(!hookCaptureCommandList(a));hook_fixture::failProtect=0;
        const auto direct=list_refusal_reasons[static_cast<unsigned>(ListRefusal::setter_patch)];
        CHECK(std::string_view(s.states.at(a).observation_gap)==direct);
        CHECK(std::string_view(s.states.at(b).observation_gap)==shared_table_invalidation);
        for(auto* l:{a,b}) {
            const auto& point=s.states.at(l);
            CHECK(!point.bindings.list_identity&&!point.bindings.tracking_complete&&!point.capture_hooks);
            CHECK(listPointFailure_locked(point,point.bindings)==point.observation_gap);
            CHECK(!capture::freeze(point.bindings,{},{}));
            s.requirements_attempts=0;s.requirements_last_us=0;
            l->SetPipelineState(p);l->DrawIndexedInstanced(9,1,0,0,0);
        }
        log.flush();
        CHECK(linesWith(io.evidence,";seam=")==1);
        CHECK(linesWith(io.evidence,";seam=creation;type=DIRECT")==1);
        CHECK(io.evidence.find(std::string("text=")+direct+";seam=creation")!=io.evidence.npos);
        CHECK(io.evidence.find(std::string(shared_table_invalidation)+";cb10=")!=io.evidence.npos);
        CHECK(io.evidence.find(std::string(direct)+";cb10=")!=io.evidence.npos);
        CHECK(a->Release()==0&&b->Release()==0&&!s.states.count(a)&&!s.states.count(b));
        set_sink(nullptr);
    }
    set_sink(nullptr);g_captureDiscoveryTest=false;s.pipelines.erase(p);s.lists.erase(lt.data());s.queues.erase(qt.data());
    CHECK(pipeline.refs==1&&wrong.refs==1);hook_fixture::expectedLists=nullptr;
}
}
// Pure bounded-state checks complement the actual hooked fake-COM cases.
static void exactConstantWitnessStates() {
    using W=acz::diagnostics::RootConstantWitness;
    auto prepared=[] {
        W w;w.bind(11,2,33,44,1,4,3,true);return w;
    };
    const uint32_t data[]={0,0xffffffff,0x80000000,0x12345678};
    auto write=[&](W& w,unsigned offset,unsigned n) {
        auto c=w.begin(11,2,7);w.returned(c);w.write(c,11,2,1,offset,data+offset,n);w.end(c);
    };
    auto snap=[](W& w,uint64_t gen=2) {
        auto c=w.begin(11,gen,7);w.returned(c);
        auto result=w.snapshot(11,gen,33,44,7,true);w.end(c);return result;
    };
    auto w=prepared();write(w,0,2);
    auto partial=snap(w);CHECK(partial.reason==W::partial&&partial.mask==3);
    char text[272]{};partial.append(text,sizeof text);
    CHECK(std::string_view(text).find("00000000,ffffffff,????????,????????")!=std::string_view::npos);
    write(w,2,2);auto full=snap(w);CHECK(full.reason==W::covered&&full.mask==15);
    CHECK(full.list==11&&full.generation==2&&full.before<full.after);
    for(unsigned i=0;i<4;++i)CHECK(full.values[i]==data[i]&&w.words[i].token==44&&w.words[i].root==33&&w.words[i].before<w.words[i].after);
    text[0]=0;full.append(text,sizeof text);
    CHECK(std::string_view(text).find("00000000,ffffffff,80000000,12345678")!=std::string_view::npos);
    CHECK(snap(w,3).reason==W::stale);
    auto oldRevision=w.words[0].revision;w.words[0].revision=w.revision+100;
    CHECK(snap(w).reason==W::stale);w.words[0].revision=oldRevision;
    w.reset(11,3);CHECK(w.words[0].written==false&&w.size==0);
    w.bind(11,3,33,44,1,4,3,true);CHECK(snap(w,3).reason==W::partial);
    w=prepared();write(w,0,4);w.bind(11,2,34,45,1,4,3,true);
    auto c=w.begin(11,2,7);w.returned(c);
    CHECK(w.snapshot(11,2,34,45,7,true).reason==W::root_changed);w.end(c);
    w=prepared();write(w,0,4);w.bind(11,2,33,44,1,4,3,true);CHECK(snap(w).reason==W::covered);
    for(auto reason:{W::bundle,W::indirect}) {w=prepared();write(w,0,4);w.fail(reason);CHECK(snap(w).reason==reason);w.reset(11,3);CHECK(w.poison==W::covered);}
    w=prepared();w.fail(W::bundle);w.fail(W::thread);w.reset(11,3);CHECK(w.poison==W::thread);
    w=prepared();auto outer=w.begin(11,2,7),inner=w.begin(11,2,7);
    w.returned(inner);w.end(inner);w.returned(outer);w.end(outer);CHECK(snap(w).reason==W::overlap);
    w=prepared();outer=w.begin(11,2,7);inner=w.begin(11,2,8);
    w.returned(inner);w.end(inner);w.returned(outer);w.end(outer);CHECK(snap(w).reason==W::thread);
    w.reset(11,3);CHECK(w.poison==W::thread); // unsafe lifetime thread history cannot be reset away
    w=prepared();outer=w.begin(11,2,7);w.returned(outer);w.end(outer);
    inner=w.begin(11,2,8);w.returned(inner);w.end(inner);CHECK(w.poison==W::thread);
    w=prepared();c=w.begin(11,2,7);w.write(c,11,2,1,0,data,4);CHECK(w.poison==W::interval&&!w.words[0].written);w.returned(c);w.end(c);
    w=prepared();w.bind(11,2,33,44,1,W::capacity+1,3,true);CHECK(snap(w).reason==W::layout);
    w=prepared();write(w,0,4);acz::diagnostics::RootConstantCounters counts;
    counts.record(snap(w));CHECK(counts.reasons[W::covered]==1&&counts.values==4);
    counts.reasons[W::covered]=65535;counts.record(snap(w));CHECK(counts.reasons[W::covered]==65535&&counts.values==4);
    for(unsigned i=0;i<32;++i) {full.values[0]=i;counts.record(full);}CHECK(counts.values==16);
    // Maximum append bound: all 16 values, all counters saturated.
    full.size=W::capacity;full.mask=0xffff;full.reason=W::interval;
    std::strcpy(text,";pt=ffff;px=ffff;pb=1;cr=ffff;cw=ffff");full.append(text,sizeof text);counts.append(text,sizeof text);
    CHECK(std::strlen(text)==256&&std::strlen(text)+161<acz::diagnostics::text_capacity);
    char worst[acz::diagnostics::text_capacity]{};
    std::snprintf(worst,sizeof worst,"rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported;cb10=1;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;lp=3%s",text);
    auto record=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    record.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(record).valid);
    // The new accepted source has exactly two words, no wc/wv suffix, and
    // continues to a success/refusal with a bounded source/next-category suffix.
    full.size=2;full.mask=3;full.reason=W::covered;full.stages=3;
    std::strcpy(text,";pt=ffff;px=ffff;pb=1;cr=ffff;cw=ffff");full.append(text,sizeof text);
    std::strcat(text,";cs=covered;cn=index");CHECK(std::strlen(text)<=100);
    std::snprintf(worst,sizeof worst,"metadata-proved:index-range-unverified-without-bytes;t10=0;r=0:176,192:304,352:368,400:416;span=%llu:%llu;first=%llu;stride=416;cb10=1;ia=64:%llu:%llu;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;bytes=not-evaluated;lp=3%s",
        UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,text);
    CHECK(std::strlen(worst)<=418);
    record=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    record.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(record).valid);
    // Maximum-width t0 suffix preserves all preceding fields in one record.
    acz::diagnostics::DescriptorTableDiagnostics::Snapshot table;
    table.flags=0x3fff;table.parameter=table.offset=table.slot=table.type=table.dimension=table.format=table.heap_class=UINT32_MAX;
    table.heap=UINT64_MAX;
    std::strcpy(text,";pt=ffff;px=ffff;pb=1;cr=ffff;cw=ffff");full.append(text,sizeof text);
    std::strcat(text,";cs=covered;cn=t0");table.append(text,sizeof text);
    CHECK(std::strstr(text,";cs=covered;cn=t0;tf=3fff")!=nullptr&&std::strlen(text)<240);
    table.ordinal=UINT32_MAX;table.writes=65535;table.writer=7;table.desc=2;table.timing=8;
    table.append_last(text,sizeof text);CHECK(std::strlen(text)<=253);
    CHECK(std::strstr(text,";wf=8;wo=ffffffff;wn=ffff;wk=7;nd=2")!=nullptr);
    std::snprintf(worst,sizeof worst,"rejected:t0 missing/stale/unsupported descriptor;cb10=1;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295;lp=3%s",text);
    record=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    record.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(record).valid);
    // New companion has a separate bound; legacy first-example text is exact.
    using D=acz::diagnostics::DescriptorTableDiagnostics;
    D history;
    history.creations.fill(65535);history.textures.fill(65535);history.missing_desc=65535;
    table.writes=65535;table.ordinal=table.bind_before=table.bind_after=table.draw_before=UINT32_MAX;
    table.writer=D::copy_simple;table.origin=D::create_srv;table.desc=2;table.timing=D::unordered;
    table.source_heap=UINT64_MAX;table.source_class=table.source_slot=table.source_flags=UINT32_MAX;
    std::strcpy(worst,"t0-write-witness;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295");
    table.append_write(worst,sizeof worst);history.append_creations(worst,sizeof worst);
    CHECK(std::strlen(worst)<=437&&std::strstr(worst,";rn=ffff")!=nullptr);
    CHECK(std::strstr(worst,";wf=8;wo=ffffffff;wn=ffff;wk=7;ok=1;nd=2")!=nullptr);
    CHECK(std::strstr(worst,";sh=ffffffffffffffff;sc=ffffffff;ss=ffffffff;sv=ffffffff")!=nullptr);
    record=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    record.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(record).valid);
    std::strcpy(worst,"s0-table-witness;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295");
    table.append(worst,sizeof worst);table.append_write(worst,sizeof worst);
    CHECK(std::strlen(worst)<=437&&std::strstr(worst,";sv=ffffffff")!=nullptr);
    auto samplerRecord=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    samplerRecord.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(samplerRecord).valid);
    std::array<uint32_t,13> samplerWords;samplerWords.fill(UINT32_MAX);
    table.sampler_words=&samplerWords;table.sampler_ordinal=UINT32_MAX;table.sampler_observed=true;
    std::strcpy(worst,"s0-description;draw=1,4294967295,4294967295,4294967295,-2147483648,4294967295");
    table.append_sampler(worst,sizeof worst);
    CHECK(std::strlen(worst)<=437&&std::strstr(worst,";sw=ffffffff,ffffffff")!=nullptr);
    samplerRecord=acz::diagnostics::record(acz::diagnostics::Event::requirements_metadata,UINT64_MAX,UINT64_MAX,UINT64_MAX,worst);
    samplerRecord.sequence=UINT64_MAX;CHECK(acz::diagnostics::format_record(samplerRecord).valid);

}

#include "l3_observation_cases.h"
#include "cpu_upload_snapshot_cases.h"
int main(int argc,char** argv) {
    // Preserve a failed run's evidence; a later run owns a different directory.
    // PIDs can be reused during repeated process launches. Include a monotonic
    // timestamp so preserved evidence from a failed prior process cannot collide.
    CHECK(argc==2);auto base=fs::current_path()/(L"diagnostics-fixture-"+std::to_wstring(GetCurrentProcessId())+
        L"-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    // CTest must run beside this offline test binary, regardless of the CMake
    // generator's binary-directory name. Never write through a divergent CWD.
    const auto fixtureDirectory=fs::current_path();
    CHECK(fixtureWorkingDirectory(fixtureDirectory));
    CHECK(!fixtureWorkingDirectory(fixtureDirectory.parent_path()));
    CHECK(!fs::exists(base));fs::create_directories(base);TestPaths paths(base);
    // weakly_canonical can return access_denied for absent sandbox children.
    // Materialize the same paths used by the in-memory production log fixtures.
    fs::create_directories(paths.value.game);fs::create_directories(paths.value.logs);
    for(const auto& file:{paths.value.config,paths.value.logs/L"diagnostics.log",
        paths.value.logs/L"diagnostics.log.1",paths.value.logs/L"diagnostics.log.2",paths.value.logs/L"diagnostics.evidence.log"})
        std::ofstream(file).close();
    if(std::string_view(argv[1])=="--companion-persistence") {
        cpu_fixture::bytesAndFailures(paths);l3_fixture::cases(paths);
        companionPersistence(paths);companionPressure(paths);companionFilePersistence(base);
        fs::remove_all(base);std::printf("%zu companion persistence checks passed\n",checks.load());return 0;
    }
    fixtureCheckpoint("exact constant witness states begin");
    exactConstantWitnessStates();
    fixtureCheckpoint("exact constant witness states end; registration fixtures begin");
    listPathSnapshots(paths);
    hudMarkersLatch(paths);
    hudRadarLatch(paths);
    effectsLatch(paths);
    refusal_fixture::tests(paths);
    refusal_fixture::exits::sharedCanonical(paths);
    refusal_fixture::exits::tests(paths);
    refusalSnapshots(paths);
    if(std::string_view(argv[1])=="--list-registration") {
        fs::remove_all(base);std::printf("%zu creation-path snapshot checks passed\n",checks.load());return 0;
    }
    fixtureCheckpoint("registration fixtures end; log and shader fixtures begin");
    companionPersistence(paths);
    companionPressure(paths);companionFilePersistence(base);
    cpu_fixture::bytesAndFailures(paths);l3_fixture::cases(paths);
    formatting();configTests(paths);caps(paths);intervals(paths);reasonSnapshots(paths);overlapReasonMatrix(paths);summaryCadence(paths);twoHourBudget(paths);reservedCapacity(paths);failureTests(paths);shaderTests(argv[1]);
    fixtureCheckpoint("log and shader fixtures end; hook fixtures begin");
    hook_fixture::tests(paths,argv[1]);
    fixtureCheckpoint("hook fixtures end; metadata fixtures begin");
    metadataStackRegression(paths,argv[1]);
    fixtureCheckpoint("metadata fixtures end; file fixtures begin");
    realFiles(base);
    failSafeAfterStart(paths);rotationIsTransient(paths);sharedReaderFiles(base); // U-soak
    fixtureCheckpoint("file fixtures end");
    fs::remove_all(base);
    std::printf("%zu diagnostics checks passed; CPU/fake COM only, no runtime claim\n",checks.load());
}
