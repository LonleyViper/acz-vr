#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include "real_backend.h"
#include "supervision.h"

#ifndef ACZ_XR_PROBE_COMMIT
#define ACZ_XR_PROBE_COMMIT "unspecified"
#endif
#define ACZ_STRING_IMPL(x) #x
#define ACZ_STRING(x) ACZ_STRING_IMPL(x)
namespace {
using namespace acz::probe;
struct WallClock final : Clock {
    ULONGLONG activated = GetTickCount64();
    int64_t now() override { return static_cast<int64_t>(GetTickCount64()-activated); }
    void sleep(int64_t ms) override { bounded_pause([](int64_t n){Sleep(static_cast<DWORD>(n));},ms); }
};
// Overlapped, write-through append: completed writes are flushed by the OS.
// File I/O must not turn the supervisor into another unbounded waiter. On an
// I/O timeout the cancelled operation's storage/event deliberately survives
// until process exit (CancelIoEx alone does not prove completion). Failure closes
// normal logging. Only the separate file thread ever calls this function.
// WriteFile itself can block; enforcement never depends on its return.
bool bounded_write(HANDLE file,const std::string& line,DWORD timeout,DWORD& error) {
    struct Pending {
        OVERLAPPED io{}; std::string line;
        ~Pending(){if(io.hEvent)CloseHandle(io.hEvent);}
    };
    auto p=std::make_unique<Pending>();p->line=line;
    p->io.Offset=p->io.OffsetHigh=0xffffffff; // atomic append with overlapped handle
    p->io.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!p->io.hEvent){error=GetLastError();return false;}
    DWORD written=0;
    BOOL done=WriteFile(file,p->line.data(),static_cast<DWORD>(p->line.size()),&written,&p->io);
    if(!done){error=GetLastError();if(error!=ERROR_IO_PENDING)return false;}
    if(!done) {
        const auto wait=WaitForSingleObject(p->io.hEvent,timeout);
        if(wait!=WAIT_OBJECT_0){
            error=wait==WAIT_FAILED?GetLastError():wait;
            CancelIoEx(file,&p->io);p.release();return false;
        }
    }
    if(!GetOverlappedResult(file,&p->io,&written,FALSE)){error=GetLastError();return false;}
    if(written!=p->line.size()){error=ERROR_WRITE_FAULT;return false;}
    error=0;return true;
}
// Owned exclusively by the file thread after activation. Logs are capped.
struct FileLog {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::atomic<unsigned> bytes{0};
    std::atomic<bool> failed{false};
    ULONGLONG deadline=0;
    DWORD error=0; // captured before any cleanup/API can overwrite last-error
    bool open(const wchar_t* path) {
        file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED|FILE_FLAG_WRITE_THROUGH,nullptr);
        return file!=INVALID_HANDLE_VALUE;
    }
    void write(const std::string& text) {
        if(failed.load())return;
        std::string line=text+"\r\n";
        unsigned size=static_cast<unsigned>(line.size());
        if(size>16384 || bytes.fetch_add(size)>Caps::log_bytes-32768-size){error=ERROR_BUFFER_OVERFLOW;failed=true;return;}
        auto now=GetTickCount64();
        DWORD timeout=static_cast<DWORD>(now>=deadline?0:std::min<ULONGLONG>(Caps::write_ms,deadline-now));
        if(!bounded_write(file,line,timeout,error))failed=true;
    }
};
struct Context {
    WallClock clock; FileLog log; Mailbox mailbox; Signals signals;
    std::wstring stop_file;
    Mode mode=Mode::zero_layers;
    std::atomic<int> outcome{1};
    // phase2 flag = explicit colour mode (a2 or card d): same 13 s cap latch.
    explicit Context(Mode selected):mailbox(selected),mode(selected) {signals.phase2=colour_mode(selected);}
    void record(const std::string& line) {
        if(signals.phase2 && (line=="image_stop_disposition=time cap" ||
            line=="frame admission stopped reason=time cap" || line=="frame admission refused at 13 s"))
            latch(signals,Cause::admission_cap,3);
        if(line.find("image_failure_code=")==0) {
            const auto cause=image_cause(std::stoi(line.substr(19)));
            if(cause!=Cause::none)latch(signals,cause);
        }
        if(line.find("failure_reason=")==0 || line.find("failure_observation=")==0)
            latch(signals,Cause::worker_failure);
        signals.progress.fetch_add(1);
        mailbox.push(line);
        if(mailbox.failed.load())logger_failure(signals,ERROR_BUFFER_OVERFLOW);
    }
};
StopFileStatus file_status(const std::wstring& path,DWORD* captured=nullptr) {
    if(captured)*captured=0;
    if(path.empty())return StopFileStatus::absent;
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES)return StopFileStatus::present;
    const auto error=GetLastError();
    if(captured)*captured=error;
    return (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)?
        StopFileStatus::absent:StopFileStatus::unavailable;
}
DWORD WINAPI logger(void* arg) {
    auto& ctx=*static_cast<Context*>(arg);
    try {
        file_loop(ctx.clock,ctx.signals,ctx.mailbox,
            [&]{DWORD error=0;auto status=file_status(ctx.stop_file,&error);
                observe_file(ctx.signals,status,error);return status;},
            [&](const std::string& line){ctx.log.write(line);
                if(ctx.log.failed.load())logger_failure(ctx.signals,ctx.log.error);
                return !ctx.log.failed.load();},
            [](int64_t ms){bounded_pause([](int64_t n){Sleep(static_cast<DWORD>(n));},ms);});
    } catch(...) {logger_failure(ctx.signals);ctx.signals.logger_done=true;}
    return 0;
}
DWORD WINAPI worker(void* arg) {
    auto& ctx=*static_cast<Context*>(arg);
    try {
        auto backend=make_real_backend(ctx.clock,[&](const std::string& line){ctx.record(line);},ctx.mode);
        Run run(*backend,ctx.clock,[&](const std::string& line){ctx.record(line);},[&]{
            if((ctx.mailbox.failed.load() || ctx.signals.failed.load()))return std::string("log failure/size cap");
            switch(ctx.signals.stop.load()) {
            case 1:return std::string("Escape");
            case 2:return std::string("stop file");
            case 3:return std::string("time cap");
            case 4:return std::string("stop file/log unavailable");
            default:break;
            }
            return std::string{};
        },[&](Phase phase){observe_phase(ctx.signals,phase,ctx.clock.now());},ctx.mode,&ctx.signals.admission);
        auto result=run.execute();
        if(!result.success)latch(ctx.signals,Cause::worker_failure);
        // Card (d): exit 0 additionally requires SYNTHETIC_STEREO_RESULT=PASS.
        const bool verdict=ctx.mode!=Mode::phase3_synthetic_stereo || result.stereo_pass;
        ctx.outcome.store(result.success && verdict && !ctx.mailbox.failed.load() && !ctx.signals.failed.load()?0:1);
    } catch(...) { latch(ctx.signals,Cause::worker_failure);ctx.record("FAILURE worker exception; retirement unproven");ctx.outcome=1; }
    ctx.signals.worker_done=true;
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    // No activation until explicit log argument parsed; no default path and no
    // silent overwrite. No explicit runtime process launch/configuration change;
    // the loader/runtime may have their own activation behavior on a future run.
    std::wstring log_path,stop_path;
    Mode selected=Mode::zero_layers;
    for(int i=1;i<argc;++i) {
        if(std::wstring(argv[i])==L"--log" && i+1<argc)log_path=argv[++i];
        else if(std::wstring(argv[i])==L"--stop-file" && i+1<argc)stop_path=argv[++i];
        else if(std::wstring(argv[i])==L"--phase2-dim-clear" && selected==Mode::zero_layers)selected=Mode::phase2_dim_clear;
        else if(std::wstring(argv[i])==L"--phase3-synthetic-stereo" && selected==Mode::zero_layers)selected=Mode::phase3_synthetic_stereo;
        else { std::fputs("usage: xr_probe --log NEW_FILE [--stop-file LOCAL_PATH] [--phase2-dim-clear | --phase3-synthetic-stereo]\n",stderr);return 2; }
    }
    if(log_path.empty() || (!stop_path.empty() && log_path==stop_path))return 2;
    // Require local absolute drive paths, never UNC/network logging/stop probes.
    auto local=[](const std::wstring& p){return p.size()>3 && p[1]==L':' && (p[2]==L'\\' || p[2]==L'/') && GetDriveTypeW(p.substr(0,3).c_str())==DRIVE_FIXED;};
    if(!local(log_path) || (!stop_path.empty() && !local(stop_path)))return 2;
    Context ctx(selected);ctx.stop_file=stop_path;
    if(!ctx.log.open(log_path.c_str())){std::fputs("refused: log exists or cannot be created\n",stderr);return 2;}
    // Preflight is before activation and before any runtime worker exists.
    const auto refusal=activation_refusal(file_status(stop_path));
    if(!refusal.empty()) {
        std::fprintf(stderr,"refused: %s\n",refusal.c_str());
        CloseHandle(ctx.log.file);return 2;
    }
    ctx.clock.activated=GetTickCount64();
    ctx.log.deadline=ctx.clock.activated+Caps::absolute_ms;
    ctx.record(std::string("manifest card=a phase=1 commit=")+ACZ_XR_PROBE_COMMIT+
        " compiler=MSVC-"+ACZ_STRING(_MSC_FULL_VER)+" process="+std::to_string(GetCurrentProcessId())+
        " supervisor_thread="+std::to_string(GetCurrentThreadId())+
        " log_limit_bytes="+std::to_string(Caps::log_bytes)+" xrWaitFrame_non_cancellable=1 startup_watchdog_ms=5000 frame_progress_watchdog_ms=250 cleanup_watchdog_ms=2000");
    ctx.record(selected==Mode::zero_layers?
        "scope=standalone probe-owned device/queue zero-layer lifecycle only; game/proxy/sharing/swapchain-images/pixels/comfort/tracking/other-runtimes unproven":
        selected==Mode::phase2_dim_clear?
        "scope=standalone a1 then explicit a2 dim clear; game/proxy/headset-visible-pixels/comfort/tracking unproven":
        "scope=standalone a1 then explicit card-d synthetic stereo with readback pixel verification of the probe's own swapchain images; "
        "compositor/headset-visible-pixels/game/proxy/comfort/eye-labels/tracking-accuracy unproven");
    DWORD tid=0;
    HANDLE file_thread=CreateThread(nullptr,0,logger,&ctx,0,nullptr);
    if(!file_thread){const auto error=GetLastError();thread_creation_failure(ctx.signals,false,error);
        // No file thread means no best-effort record; never add synchronous
        // fallback I/O here. The distinct exit code remains observable.
        CloseHandle(ctx.log.file);return cause_exit(Cause::file_create);}
    HANDLE thread=CreateThread(nullptr,0,worker,&ctx,0,&tid);
    if(!thread){const auto error=GetLastError();thread_creation_failure(ctx.signals,true,error);}
    // ONLY atomic state, key query and bounded sleep on the enforcement thread.
    // Even an append, stop-file lookup or CancelIoEx that never returns cannot
    // delay the termination decision. Emergency disk evidence is best effort
    // via the mailbox and is deliberately not required before termination.
    const bool supervised=supervise(ctx.clock,ctx.signals,
        []{return escape_down([]{return GetAsyncKeyState(VK_ESCAPE);});},
        [](int64_t ms){bounded_pause([](int64_t n){Sleep(static_cast<DWORD>(n));},ms);},
        [&]{
            // No file writes, log queue lock, allocation or foreign cleanup here.
            const auto code=static_cast<UINT>(ctx.signals.watchdog_exit.load());
            TerminateProcess(GetCurrentProcess(),code);
            // If termination unexpectedly fails, do not destroy thread storage.
            ExitProcess(code);
        });
    if(thread)CloseHandle(thread);
    CloseHandle(file_thread);
    // Both completion flags prove no thread can still touch the file/storage.
    // Pending I/O storage on cancellation timeout is deliberately retained.
    CloseHandle(ctx.log.file);
    const auto cause=unpack_terminal(ctx.signals.terminal.load()).cause;
    if(ctx.signals.watchdog_exit.load())return ctx.signals.watchdog_exit.load();
    if(cause!=Cause::none)return cause_exit(cause);
    return supervised && !ctx.mailbox.failed.load()?ctx.outcome.load():1;
}
