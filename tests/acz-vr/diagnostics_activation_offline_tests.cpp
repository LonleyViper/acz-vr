// Real exports and patched slots, fake COM only. Never loads the built DLL.
#define ACZ_VR_OFFLINE_TEST
#define ACZ_VR_DLL_LIFECYCLE_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include "../../src/acz-vr/cockpit_math.h"
namespace acz {
struct OpenXRSession {bool init(ID3D12Device*,ID3D12CommandQueue*){return false;}
    bool pollEvents(){return false;}bool isSessionRunning(){return false;}};
struct StereoConfig {};
struct StereoRenderer {void setDrawCPTCalibration(const Mat4&){}
    void setAircraftPose(const Mat4&,const Mat4&) {}};
}
#include "../../src/acz-vr/dllmain.cpp"
#include "../../src/acz-vr/capture_enablement.h"
#include <fstream>
#include <thread>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>

static std::atomic<unsigned> checks{0};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
// Diagnostic only: flushed phase markers with elapsed time, and bounded waits so a
// stalled handshake fails with a name instead of hitting the CTest timeout.
static void phase(const char* name,unsigned long long a=0,unsigned long long b=0) {
    static const auto start=std::chrono::steady_clock::now();
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::fprintf(stderr,"[phase +%lldms] %s a=%llu b=%llu\n",static_cast<long long>(ms),name,a,b);std::fflush(stderr);
}
static const auto handshakeBound=std::chrono::seconds(20);
static bool failNew=false;
static unsigned failedNew=0;
void* operator new(size_t n) {
    if(failNew){failNew=false;++failedNew;throw std::bad_alloc();}
    if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using namespace acz::diagnostics;
namespace fs=std::filesystem;
static std::atomic<uint64_t> tick{0};
static uint64_t now() noexcept {return tick.load();}
static const GUID extension={0x12345,2,3,{4,5,6,7,8,9,10,11}};
struct Object {void** table;ULONG refs=1;};
static Object* factory=nullptr;
static UINT seenFlags=0;static const GUID* seenIID=nullptr;static void** seenOut=nullptr;
static unsigned nativeCalls=0,executes=0,loadCalls=0;
static bool loadFails=false;
static unsigned* pathCallsDuringLoad=nullptr;
static HRESULT nativeHR=S_FALSE;
static HRESULT STDMETHODCALLTYPE query(IUnknown* self,REFIID,void** out) {
    *out=self;++reinterpret_cast<Object*>(self)->refs;return S_OK;
}
static ULONG STDMETHODCALLTYPE add(IUnknown* self){return ++reinterpret_cast<Object*>(self)->refs;}
static ULONG STDMETHODCALLTYPE release(IUnknown* self){return --reinterpret_cast<Object*>(self)->refs;}
static void unused() {}
static std::array<void*,64> table() {
    std::array<void*,64> t{};t.fill(reinterpret_cast<void*>(&unused));
    t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&add);t[2]=reinterpret_cast<void*>(&release);return t;
}
static HRESULT WINAPI create2(UINT flags,REFIID iid,void** out) {
    ++nativeCalls;seenFlags=flags;seenIID=&iid;seenOut=out;*out=factory;++factory->refs;return nativeHR;
}
static HRESULT WINAPI create0(REFIID iid,void** out){return create2(0,iid,out);}
static HMODULE load() {
    ++loadCalls;
    if(pathCallsDuringLoad) {
        void* out=reinterpret_cast<void*>(1);
        CHECK(ProxyCreateDXGIFactory(extension,&out)==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)&&!out);
        CHECK(*pathCallsDuringLoad==0&&!sink());
    }
    return loadFails?nullptr:reinterpret_cast<HMODULE>(1);
}
static BOOL WINAPI protect(LPVOID,SIZE_T,DWORD,DWORD* old){*old=PAGE_READWRITE;return TRUE;}
static void STDMETHODCALLTYPE nativeExecute(ID3D12CommandQueue*,UINT n,ID3D12CommandList* const* lists) {
    CHECK(n==0&&!lists);++executes;
}
static Object* device=nullptr;
static HRESULT STDMETHODCALLTYPE getDevice(ID3D12CommandQueue*,REFIID iid,void** out) {
    CHECK(iid==__uuidof(ID3D12Device));*out=device;++device->refs;return S_OK;
}
struct QueueDesc {virtual D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE desc() {
    D3D12_COMMAND_QUEUE_DESC d{};d.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;return d;}};
static HRESULT STDMETHODCALLTYPE swap(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**){return S_OK;}

struct TestPaths : PathProvider {
    Paths value;unsigned calls=0;bool throwing=false;
    explicit TestPaths(const fs::path& base):value{base/L"logs",base/L"game",base/L"runtime.cfg"}{}
    Paths paths() override {++calls;if(throwing)throw std::bad_alloc();return value;}
};
// FileIO is final; delegate through the public production I/O boundary.
struct IO : IOProvider {
    FileIO real;unsigned appends=0,rotations=0,reads=0;std::string trace;
    uint64_t peakBytes=0;
    bool directoryFails=false,shortWrite=false,recurse=false,probe=false;
    IUnknown* recursiveFactory=nullptr;
    std::mutex mutex;std::condition_variable changed;bool block=false,held=false,resume=false;
    ReadResult read_config(const fs::path& p,char* b,size_t n) override {++reads;return real.read_config(p,b,n);}
    bool create_directory(const fs::path& p) override {return !directoryFails&&real.create_directory(p);}
    bool reset(const fs::path& p) override {return real.reset(p);}
    size_t overwrite(const fs::path& p,uint64_t off,const char* b,size_t n) override {
        CHECK(!mutex_depth);const auto result=real.overwrite(p,off,b,n);checkDisk(p.parent_path());return result;
    }
    bool size(const fs::path& p,uint64_t& n) override {return real.size(p,n);}
    bool rotate(const fs::path& p,size_t n) override {++rotations;CHECK(n<=max_rotated_files);return real.rotate(p,n);}
    void checkDisk(const fs::path& logs) {
        uint64_t bytes=0;size_t ordinary=0,evidence=0;
        for(const auto& e:fs::directory_iterator(logs)) {
            const auto size=e.file_size();
            if(e.path().filename()==L"diagnostics.evidence.log"){CHECK(size<=max_evidence_size);++evidence;}
            else {CHECK(size<=max_file_size);++ordinary;}
            CHECK(size<=max_disk_bytes-bytes);bytes+=size;
        }
        CHECK(ordinary<=max_rotated_files+1&&evidence<=1&&bytes<=max_disk_bytes);
        peakBytes=std::max(bytes,peakBytes);
    }
    size_t append(const fs::path& p,const char* b,size_t n) override {
        ++appends;CHECK(n<=max_file_size);CHECK(!mutex_depth);
        if(probe) {
            bool hook=false,life=false;
            std::thread worker([&]{auto& h=acz::state().mutex;hook=h.try_lock();if(hook)h.unlock();
                auto& m=lifecycle().value->mutex;life=m.try_lock();if(life)m.unlock();});
            worker.join();CHECK(hook&&life);
        }
        if(recurse) {
            recurse=false;const auto before=appends;
            emit(record(Event::summary,0,0,0,"recursive Release probe"));
            if(recursiveFactory){CHECK(recursiveFactory->Release()==0);recursiveFactory=nullptr;}
            flush_after_release();CHECK(appends==before);
        }
        {std::unique_lock lock(mutex);if(block){CHECK(!held);held=true;changed.notify_all();if(!changed.wait_for(lock,handshakeBound,[&]{return resume;})){phase("TIMEOUT blocked-append resume wait");CHECK(false);}}}
        if(shortWrite)return n?real.append(p,b,n-1):0;
        trace.append(b,n);const auto result=real.append(p,b,n);checkDisk(p.parent_path());return result;
    }
};
struct Scenario {
    TestPaths paths;IO io;Activation activation;
    explicit Scenario(const fs::path& base):paths(base) {
        CHECK(!sink());tick=0;activation.paths=&paths;activation.io=&io;activation.clock=now;
        activation_test=&activation;
    }
    ~Scenario(){set_sink(nullptr);activation_test=nullptr;}
    void config(const char* text) {
        fs::create_directories(paths.value.config.parent_path());
        std::ofstream f(paths.value.config,std::ios::binary);f<<text;CHECK(bool(f));
    }
    void forward(unsigned version=2,REFIID iid=extension) {
        const auto refs=factory->refs;void* out=nullptr;const auto before=nativeCalls;
        auto hr=version==0?ProxyCreateDXGIFactory(iid,&out):version==1?ProxyCreateDXGIFactory1(iid,&out):
            ProxyCreateDXGIFactory2(0x89,iid,&out);
        CHECK(hr==nativeHR&&nativeCalls==before+1&&seenOut==&out&&seenIID==&iid);
        CHECK(seenFlags==(version==2?0x89:0));
        CHECK(out==factory&&factory->refs==refs+1);CHECK(static_cast<IUnknown*>(out)->Release()==refs);
    }
};
static size_t occurrences(const std::string& s,const char* text) {
    size_t n=0,p=0;while((p=s.find(text,p))!=s.npos){++n;p+=std::strlen(text);}return n;
}
static std::string persisted(const TestPaths& paths) {
    std::string text;
    auto evidence=paths.value.logs/L"diagnostics.evidence.log";
    if(fs::exists(evidence)) {
        std::ifstream f(evidence,std::ios::binary);
        text.append(std::istreambuf_iterator<char>(f),{});
    }
    for(size_t i=0;i<=max_rotated_files;++i) {
        auto p=paths.value.logs/(i?L"diagnostics.log."+std::to_wstring(i):L"diagnostics.log");
        if(!fs::exists(p))continue;
        CHECK(fs::file_size(p)<=max_file_size);std::ifstream f(p,std::ios::binary);
        text.append(std::istreambuf_iterator<char>(f),{});
    }
    return text;
}
// D-51: exercise only records the existing hooks can emit. Draw callbacks and
// decoded-root identity keys are deliberately absent. No wall-clock sleeps.
static void sessionCapacity(const fs::path& base) {
    phase("sessionCapacity begin");
    Scenario s(base);s.config("log_enabled=true");s.forward();
    auto* activated=sink();
    if(!activated||!activated->enabled()) {
        std::error_code ec;
        const bool configExists=fs::exists(s.paths.value.config,ec);
        std::fprintf(stderr,
            "diagnostics activation failed: sink=%p enabled=%d config=%ls exists=%d path_calls=%u reads=%u appends=%u rotations=%u io_trace_bytes=%zu\n",
            static_cast<const void*>(activated),activated?int(activated->enabled()):0,
            s.paths.value.config.c_str(),int(configExists),s.paths.calls,s.io.reads,
            s.io.appends,s.io.rotations,s.io.trace.size());
        std::fflush(stderr);
    }
    CHECK(activated&&activated->enabled());
    emit(record(Event::device,1,0,0,"synthetic device discovery"));
    emit(record(Event::hook_pin,1));emit(record(Event::hook_patch,10,1,0,"installed"));
    for(unsigned i=0;i<114;++i)emit(record(Event::metadata_disabled,0,0,0,"metadata churn"));
    for(unsigned i=0;i<230;++i) {
        auto r=record(Event::pso,i,416,0,"L1 synthetic shader identity");
        r.shader.identity=static_cast<Identity>(i%3);
        std::snprintf(r.shader.hash.data(),r.shader.hash.size(),"%032x",i);
        std::snprintf(r.shader.container_prefix.data(),r.shader.container_prefix.size(),"%016x",i);
        emit(r);
    }
    for(unsigned i=0;i<28;++i)emit(record(Event::root,i,0,0,"L1 synthetic root outcome"));
    for(uint64_t wave=0;wave<7200;++wave) {
        for(unsigned i=0;i<24;++i)emit(record(Event::summary,wave,i,0,"ordinary session traffic"));
        for(unsigned i=0;i<42;++i)emit(record(Event::descriptor,1,0,0,"descriptor traffic"));
        tick=wave*flush_window_us+flush_window_us;
        activated->execute_interval();flush_after_execute();
        if(wave%1200==1199)phase("sessionCapacity wave",wave+1,s.io.appends);
    }
    phase("sessionCapacity waves done",s.io.appends,s.io.rotations);
    CHECK(s.io.rotations>0); // Must rotate before any Release checkpoint.
    flush_after_release();
    const auto disk=persisted(s.paths);
    CHECK(occurrences(disk,"text=L1 synthetic shader identity")==230);
    CHECK(occurrences(disk,"text=L1 synthetic root outcome")==28);
    CHECK(occurrences(disk,"text=Release checkpoint; not process teardown")==1);
    for(auto kind:{"|proxy_load|","|config|","|capture_decision|","|device|","|hook_pin|","|hook_patch|"})
        CHECK(disk.find(kind)!=disk.npos);
    for(unsigned i=0;i<230;++i) {
        auto r=record(Event::pso,i,416,0,"L1 synthetic shader identity");
        r.shader.identity=static_cast<Identity>(i%3);
        std::snprintf(r.shader.hash.data(),r.shader.hash.size(),"%032x",i);
        std::snprintf(r.shader.container_prefix.data(),r.shader.container_prefix.size(),"%016x",i);
        const auto f=format_record(r);
        CHECK(f.valid&&disk.find(std::string(f.bytes.data()+1,f.size-1))!=disk.npos);
    }
    for(unsigned i=0;i<28;++i) {
        const auto f=format_record(record(Event::root,i,0,0,"L1 synthetic root outcome"));
        CHECK(f.valid&&disk.find(std::string(f.bytes.data()+1,f.size-1))!=disk.npos);
    }
    CHECK(occurrences(s.io.trace,"|descriptor|")==0);
    CHECK(disk.find("|summary|a=6|b=302400|c=0|")!=disk.npos);
    CHECK(sink()->observed(Event::descriptor)==302400&&sink()->dropped(Event::descriptor)==0);
    CHECK(sink()->observed(Event::metadata_disabled)==114&&sink()->dropped(Event::metadata_disabled)==0);
    CHECK(disk.find("|summary|a=8|b=114|c=0|")!=disk.npos&&occurrences(s.io.trace,"|metadata_disabled|")==0);
    CHECK(sink()->observed(Event::summary)>=172800); // Includes consumer snapshots.
    CHECK(s.io.peakBytes<=max_disk_bytes&&s.io.rotations>max_rotated_files);
    phase("sessionCapacity disk verified",s.io.appends,s.io.rotations);
    // Further traffic after the checkpoint cannot consume its protected quota.
    for(uint64_t wave=0;wave<100;++wave) {
        for(unsigned i=0;i<ring_capacity;++i)emit(record(Event::summary,wave,i));
        tick.fetch_add(flush_window_us);flush_after_execute();
    }
    CHECK(occurrences(persisted(s.paths),"text=Release checkpoint; not process teardown")==1);
    CHECK(occurrences(persisted(s.paths),"text=L1 synthetic shader identity")==230);
    for(auto kind:{"|proxy_load|","|config|","|capture_decision|","|device|","|hook_pin|","|hook_patch|"})
        CHECK(persisted(s.paths).find(kind)!=std::string::npos);
}
static IUnknown* wrapped() {
    nativeHR=S_OK;void* out=nullptr;CHECK(ProxyCreateDXGIFactory(__uuidof(IDXGIFactory),&out)==S_OK);
    CHECK(out&&out!=factory);nativeHR=S_FALSE;return static_cast<IUnknown*>(out);
}
static void disabled(const fs::path& base) {
    const char* configs[]={nullptr,"log_enabled=false","broken","verbosity=1","capture=true",
        "log_enabled=true\ncapture=true","log_enabled=true\nunknown=1","log_enabled=bad"};
    unsigned i=0;
    for(auto text:configs) {
        Scenario s(base/std::to_wstring(i++));if(text)s.config(text);
        const auto before=nativeCalls;
        CHECK(DllMain(nullptr,DLL_PROCESS_ATTACH,nullptr));CHECK(DllMain(nullptr,DLL_PROCESS_DETACH,nullptr));
        CHECK(!sink()&&!s.paths.calls&&!s.io.reads&&!fs::exists(s.paths.value.logs));
        for(unsigned v=0;v<3;++v)s.forward(v);
        flush_after_execute();flush_after_release();
        CHECK(nativeCalls==before+3&&!sink()&&s.paths.calls==1&&s.io.reads==1);
        CHECK(!s.io.appends&&!fs::exists(s.paths.value.logs));
        CHECK(!acz::capture_enablement::decide({}).enabled);
    }
}
static void active(const fs::path& base) {
    phase("active begin");
    Scenario s(base);s.config("log_enabled=true\ncapture=disabled");
    CHECK(DllMain(nullptr,DLL_PROCESS_ATTACH,nullptr)&&DllMain(nullptr,DLL_PROCESS_DETACH,nullptr));
    CHECK(!sink()&&!s.paths.calls&&!s.io.reads&&!fs::exists(s.paths.value.logs));
    CHECK(ProxyCreateDXGIFactory(extension,nullptr)==E_POINTER); // First export still activates once.
    s.forward();
    CHECK(sink()&&s.paths.calls==1&&s.io.reads==1&&s.io.appends==1);
    CHECK(fs::exists(s.paths.value.logs/L"diagnostics.evidence.log")&&!fs::exists(s.paths.value.game));
    CHECK(s.io.trace.find("proxy_load")!=s.io.trace.npos);
    CHECK(s.io.trace.find(acz::capture_enablement::decide({}).reason)!=s.io.trace.npos);
    for(unsigned v=0;v<3;++v) {
        emit(record(Event::summary,v,0,0,"startup forward limiter probe"));
        s.forward(v);CHECK(s.io.appends==1);
    }
    CHECK(s.paths.calls==1&&s.io.reads==1&&s.io.appends==1);
    auto enumeration=wrapped();const auto startupAppends=s.io.appends;
    emit(record(Event::summary,0,0,0,"pre-submission Release probe"));
    CHECK(enumeration->Release()==0&&s.io.appends==startupAppends);
    CHECK(!s.activation.checkpoint); // Enumeration Release is not teardown.
    auto qt=table(),dt=table();Object queue{qt.data()},dev{dt.data()};device=&dev;
    auto q=reinterpret_cast<ID3D12CommandQueue*>(&queue);auto d=reinterpret_cast<ID3D12Device*>(&dev);
    qt[10]=reinterpret_cast<void*>(&nativeExecute);qt[7]=reinterpret_cast<void*>(&getDevice);
    QueueDesc desc;qt[18]=(*reinterpret_cast<void***>(&desc))[0];
    acz::g_protect=protect;
    auto w=static_cast<IDXGIFactory*>(wrapped());DXGI_SWAP_CHAIN_DESC swapDesc{};IDXGISwapChain* chain=nullptr;
    CHECK(w->CreateSwapChain(reinterpret_cast<IUnknown*>(q),&swapDesc,&chain)==S_OK);
    CHECK(acz::getHookedDevice()==d&&acz::getHookedQueue()==q&&queue.refs==2&&dev.refs==2);
    const auto first=s.io.appends;
    for(uint64_t i=0;i<flush_submissions-1;++i)q->ExecuteCommandLists(0,nullptr);
    CHECK(s.io.appends==first);q->ExecuteCommandLists(0,nullptr);CHECK(s.io.appends==first+2);
    CHECK(s.io.trace.find("device and direct queue discovered")!=s.io.trace.npos);
    CHECK(s.io.trace.find("hook_patch")!=s.io.trace.npos&&s.io.trace.find("hook_pin")!=s.io.trace.npos);
    CHECK(s.io.trace.find("execute-interval-us")!=s.io.trace.npos);
    const auto disk=persisted(s.paths);
    for(auto kind:{"|proxy_load|","|device|","|hook_pin|","|hook_patch|","|execute_summary|"})
        CHECK(disk.find(kind)!=disk.npos);
    CHECK(disk.find(acz::capture_enablement::decide({}).reason)!=disk.npos);
    const auto countBefore=s.io.appends;
    for(uint64_t i=0;i<4*flush_submissions;++i) {
        // Pending ordinary data makes every premature flush observable.
        emit(record(Event::summary,i,0,0,"flush limiter probe"));
        q->ExecuteCommandLists(0,nullptr);
        CHECK(s.io.appends-countBefore==(i+1)/flush_submissions);
    }
    CHECK(s.io.appends-countBefore==4); // K/N, without any clock advancement.
    const auto byCount=s.io.appends;
    // Independent injected clock trigger; regressing clocks do not underflow.
    emit(record(Event::summary,0,0,0,"clock limiter probe"));
    tick=flush_window_us-1;q->ExecuteCommandLists(0,nullptr);CHECK(s.io.appends==byCount);
    tick=flush_window_us;q->ExecuteCommandLists(0,nullptr);CHECK(s.io.appends==byCount+1);
    emit(record(Event::summary,0,0,0,"regressing clock probe"));
    tick=0;q->ExecuteCommandLists(0,nullptr);CHECK(s.io.appends==byCount+1);
    phase("active flush limiter done",s.io.appends);
    // A blocked OS append does not block other submission threads or forwarding.
    emit(record(Event::summary,0,0,0,"blocked append probe"));
    s.io.block=true;tick=2*flush_window_us;
    std::thread flusher([&]{q->ExecuteCommandLists(0,nullptr);});
    {std::unique_lock lock(s.io.mutex);
        if(!s.io.changed.wait_for(lock,handshakeBound,[&]{return s.io.held;})){phase("TIMEOUT blocked-append entry wait");CHECK(false);}}
    phase("blocked-append entered",s.io.appends);
    const auto blockedAppends=s.io.appends;const auto calls=executes;
    for(unsigned i=0;i<4*flush_submissions;++i) {
        emit(record(Event::summary,i,0,0,"concurrent flusher probe"));
        q->ExecuteCommandLists(0,nullptr);
    }
    CHECK(executes==calls+4*flush_submissions&&s.io.appends==blockedAppends);
    {std::lock_guard lock(s.io.mutex);s.io.resume=true;s.io.changed.notify_all();}flusher.join();s.io.block=false;
    phase("active blocked-append released",s.io.appends);
    // Mutex probe uses real hook and lifecycle locks on another thread.
    CHECK(runtime(lifecycle()));s.io.probe=true;
    const auto lockedAppends=s.io.appends;
    {std::lock_guard lock(acz::state().mutex);
        emit(record(Event::summary,0,0,0,"hook mutex probe"));flush_after_release();}
    {std::lock_guard lock(lifecycle().value->mutex);
        emit(record(Event::summary,0,0,0,"lifecycle mutex probe"));flush_after_release();}
    CHECK(s.io.appends==lockedAppends);
    phase("active mutex probes done",s.io.appends);
    // Discriminating long-run retention regression: both rate triggers fire.
    for(uint64_t wave=0;wave<160;++wave) {
        for(uint64_t i=0;i<ring_capacity;++i)emit(record(Event::summary,wave,i,0,"retention traffic"));
        tick.fetch_add(flush_window_us);
        for(uint64_t i=0;i<flush_submissions;++i)q->ExecuteCommandLists(0,nullptr);
        if(wave%40==39)phase("active retention loop 1",wave+1,s.io.appends);
    }
    auto recursive=wrapped();s.io.recursiveFactory=recursive;s.io.recurse=true;
    CHECK(s.io.rotations>0);
    emit(record(Event::summary,0,0,0,"checkpoint flush probe"));
    const auto before=s.io.appends;CHECK(w->Release()==0);CHECK(s.io.appends==before+2);
    CHECK(occurrences(s.io.trace,"text=Release checkpoint; not process teardown")==1);
    CHECK(occurrences(persisted(s.paths),"text=Release checkpoint; not process teardown")==1);
    const auto rotationsAtCheckpoint=s.io.rotations;
    const auto retained=persisted(s.paths);
    for(auto kind:{"|proxy_load|","|device|","|hook_pin|","|hook_patch|"})
        CHECK(retained.find(kind)!=retained.npos);
    CHECK(retained.find(acz::capture_enablement::decide({}).reason)!=retained.npos);
    // Later submissions and clock-limited Releases cannot rotate out either
    // startup evidence or the one checkpoint, even after another long run.
    for(uint64_t wave=0;wave<160;++wave) {
        for(uint64_t i=0;i<ring_capacity;++i)emit(record(Event::summary,wave,i,0,"retention traffic"));
        tick.fetch_add(flush_window_us);
        for(uint64_t i=0;i<flush_submissions;++i)q->ExecuteCommandLists(0,nullptr);
        auto later=wrapped();CHECK(later->Release()==0);
        if(wave%40==39)phase("active retention loop 2",wave+1,s.io.appends);
    }
    const auto afterCheckpoint=persisted(s.paths);
    for(auto kind:{"|proxy_load|","|device|","|hook_pin|","|hook_patch|"})
        CHECK(afterCheckpoint.find(kind)!=afterCheckpoint.npos);
    CHECK(afterCheckpoint.find(acz::capture_enablement::decide({}).reason)!=afterCheckpoint.npos);
    CHECK(occurrences(afterCheckpoint,"text=Release checkpoint; not process teardown")==1);
    CHECK(sink()->counters().written<=max_records&&s.io.rotations>rotationsAtCheckpoint);
    const auto afterLongRun=s.io.appends;
    // Existing patched base-device Release seam, no new queue interception.
    CHECK(acz::hookNativeBindingDevice(d));
    acz::state().device=nullptr;acz::state().queue=nullptr;
    emit(record(Event::summary,0,0,0,"device Release limiter probe"));
    CHECK(d->Release()==1&&s.io.appends==afterLongRun);
    emit(record(Event::summary,0,0,0,"queue Release limiter probe"));
    CHECK(q->Release()==1&&s.io.appends==afterLongRun);
    emit(record(Event::summary,0,0,0,"final device Release limiter probe"));
    CHECK(d->Release()==0);CHECK(occurrences(s.io.trace,"text=Release checkpoint; not process teardown")==1);
    CHECK(s.io.appends==afterLongRun); // Repeated final object Releases cannot force I/O.
    auto failing=wrapped();s.io.shortWrite=true;const auto failingAppends=s.io.appends;
    nativeHR=E_FAIL;for(unsigned v=0;v<3;++v) {
        emit(record(Event::summary,v,0,0,"failed forward limiter probe"));
        s.forward(v);CHECK(s.io.appends==failingAppends);
    }nativeHR=S_FALSE;
    // Test the injected short write in a fresh scenario below. These Releases
    // share the unchanged clock limiter and must forward without an append.
    emit(record(Event::summary,0,0,0,"factory Release limiter probe"));
    CHECK(failing->Release()==0&&s.io.appends==failingAppends);
    CHECK(sink()&&sink()->enabled());
    for(unsigned v=0;v<3;++v) {
        emit(record(Event::summary,v,0,0,"forward limiter probe"));
        s.forward(v);CHECK(s.io.appends==failingAppends);
    }
    CHECK(s.io.appends==failingAppends);
    CHECK(factory->refs==1);
    acz::state().devices.erase(dt.data());acz::state().queues.erase(qt.data());
}
static void reasonFile(const fs::path& base) {
    Scenario s(base);s.config("log_enabled=true");s.forward();
    auto& state=acz::state();
    acz::observeNativeBinding<DisableEvent::creation>([](auto& b){b.heap({});});
    CHECK(!state.nativeBindings.enabled());
    sink()->execute_interval();tick=flush_window_us;flush_after_execute();
    auto read=[&] {
        std::ifstream f(s.paths.value.logs/L"diagnostics.evidence.log",std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f),{});
    };
    auto first=read();CHECK(occurrences(first,"text=heap registration rejected ")==1);
    CHECK(first.find("|b=1|c=3|")!=first.npos);
    {std::lock_guard lock(state.mutex);acz::BindingEvent::disable("heap registration rejected",true,DisableEvent::creation);}
    tick=2*flush_window_us;flush_after_execute();
    auto second=read();CHECK(first.size()==second.size());
    CHECK(occurrences(second,"text=heap registration rejected ")==1&&second.find("|b=2|c=3|")!=second.npos);
    s.io.checkDisk(s.paths.value.logs);
    CHECK(!s.io.real.overwrite(s.paths.value.logs/L"diagnostics.evidence.log",max_evidence_size,second.data(),max_record_length));
    CHECK(!s.io.real.overwrite(s.paths.value.logs/L"diagnostics.evidence.log",0,second.data(),max_record_length-1));
    state.nativeBindings.~NativeBindingObserver();new (&state.nativeBindings) acz::binding::NativeBindingObserver;
}
// FPS investigation (docs/world-render/fps-investigation.md): perf_lean moves the periodic flush (ring drain + file appends) off the game's
// ExecuteCommandLists onto the producer worker; the game thread skips while a worker flush happened within 2 s and resumes otherwise.
// perf_lean off: flush_from_worker does nothing and flush_after_execute is unchanged.
static void leanWorkerFlush(const fs::path& base) {
    {
        Scenario s(base/L"off");s.config("log_enabled=true");s.forward();CHECK(sink()&&!sink()->perf_lean());
        sink()->execute_interval();emit(record(Event::device,1,0,0,"lean off probe"));tick=flush_window_us;
        const auto a=s.io.appends;flush_from_worker();CHECK(s.io.appends==a);
        flush_after_execute();CHECK(s.io.appends>a);
    }
    Scenario s(base/L"on");s.config("log_enabled=true\nperf_lean=true");s.forward();
    CHECK(sink()&&sink()->perf_lean());
    sink()->execute_interval();emit(record(Event::device,1,0,0,"lean probe 1"));tick=flush_window_us;
    const auto a0=s.io.appends;flush_after_execute();            // no worker yet: the game-thread flush runs as before
    const auto a1=s.io.appends;CHECK(a1>a0);
    emit(record(Event::device,1,0,0,"lean probe 2"));tick=2*flush_window_us;
    flush_from_worker();                                         // the worker flushes (no hook mutex held: IO::append checks)
    const auto a2=s.io.appends;CHECK(a2>a1);
    emit(record(Event::device,1,0,0,"lean probe 3"));tick=2*flush_window_us+flush_window_us/2;
    flush_from_worker();CHECK(s.io.appends==a2);                 // the 250 ms clock limit applies to the worker too
    tick=4*flush_window_us;flush_after_execute();CHECK(s.io.appends==a2);   // worker alive (< 2 s): no I/O on the game thread
    emit(record(Event::device,1,0,0,"lean probe 4"));
    tick=3*flush_window_us+2000000;flush_after_execute();CHECK(s.io.appends>a2);   // worker silent for 2 s: the game thread flushes again
    s.io.checkDisk(s.paths.value.logs);
}
static void releaseFailure(const fs::path& base) {
    Scenario s(base);s.config("log_enabled=true");s.forward();
    auto failing=wrapped();sink()->execute_interval();s.io.shortWrite=true;
    const auto failingAppends=s.io.appends;
    CHECK(failing->Release()==0&&s.io.appends==failingAppends+1);
    CHECK(sink()&&!sink()->enabled()); // Active I/O failure becomes terminal too.
    for(unsigned v=0;v<3;++v)s.forward(v);
    CHECK(s.io.appends==failingAppends+1);
    CHECK(factory->refs==1);
}
static void failures(const fs::path& base) {
    for(unsigned failure=0;failure<5;++failure) {
        Scenario s(base/std::to_wstring(failure));s.config("log_enabled=true");
        if(failure==0)s.paths.throwing=true;
        if(failure==1)s.io.directoryFails=true;
        if(failure==2)s.io.shortWrite=true;
        if(failure==3)failNew=true;
        if(failure==4)s.paths.value.logs=s.paths.value.game/L"forbidden";
        for(auto hr:{S_FALSE,E_FAIL}) {nativeHR=hr;for(unsigned v=0;v<3;++v)s.forward(v);}
        nativeHR=S_FALSE;CHECK(!sink()&&!failNew&&s.paths.calls<2&&s.io.appends<=1);
        const auto calls=s.paths.calls;s.forward();CHECK(s.paths.calls==calls);
    }
    CHECK(failedNew==1);
    { // Native load failure still attempts diagnostics once, preserving HRESULT.
        Scenario s(base/L"load");s.config("log_enabled=true");std::once_flag once;
        auto old=acz::g_loadOnceTest;auto module=acz::g_realDXGI;acz::g_loadOnceTest=&once;acz::g_realDXGI=nullptr;
        pathCallsDuringLoad=&s.paths.calls;
        loadFails=true;void* out=reinterpret_cast<void*>(1);
        CHECK(ProxyCreateDXGIFactory(extension,&out)==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)&&!out);
        CHECK(s.paths.calls==1&&s.io.appends==1);loadFails=false;pathCallsDuringLoad=nullptr;
        acz::g_loadOnceTest=old;acz::g_realDXGI=module;
    }
}
int main() {
    auto base=fs::temp_directory_path()/(L"acz-activation-"+std::to_wstring(GetCurrentProcessId()));
    CHECK(!fs::exists(base));fs::create_directories(base);
    auto ft=table();ft[10]=reinterpret_cast<void*>(&swap);Object real{ft.data()};factory=&real;
    acz::g_loadTest=load;acz::g_factoryTest=create0;acz::g_factory1Test=create0;acz::g_factory2Test=create2;
    acz::g_initializeTest=acz::initializeProxyCallbacks;
    sessionCapacity(base/L"session");
    phase("disabled");disabled(base/L"disabled");phase("reasonFile");reasonFile(base/L"reason-file");
    active(base/L"active");phase("leanWorkerFlush");leanWorkerFlush(base/L"lean");phase("releaseFailure");releaseFailure(base/L"release-failure");
    phase("failures");failures(base/L"failures");phase("done");
    CHECK(real.refs==1);fs::remove_all(base);
    std::printf("%u activation checks passed; offline fake COM only\n",checks.load());
}
