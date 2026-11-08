// J-w1-ee-camera Windows glue (DLL only). Default OFF: unless ee_camera_probe and
// its prerequisites are latched, the only work is one sink lookup per swapchain
// creation; no hook is patched, no memory is scanned or written.
//
//  * Present seam (A): IDXGISwapChain::Present (slot 8) and IDXGISwapChain1::
//    Present1 (slot 22) vtable patches, installed only when armed. After the
//    native call returns: the experiment tick (frame clock, measurement, state
//    machine), the EE-phase sample, then a seam-A write if one is armed.
//  * B / C seams: ee_camera_hooks.h (first replayed member / ECL entry).
//  * Memory: VirtualQuery-checked ranges and SEH-guarded copies/stores; the
//    seams only ever touch the validated 64 MiB block (atomic base).
// One attempt per process; nothing is ever unpatched (process lifetime, the
// module is pinned like the other hooks).
#include "ee_camera_probe.h"
#include "ee_look_write.h"
#include "ee_look_input_trace.h"
#include "ee_lens_scan.h"
#include "ee_ps2_probe.h"
#include "ee_camera_diff.h"
#include "ee_camera_hooks.h"
#include "ee_camera_guarded.h"
#include "ee_camera_logfile.h"
#include "terrain_camera.h"
#include "ee_e6_locator.h"
#include "ee_store_watch.h"   // STORES-RT stwatch:1 launch-signal search (read-only)
#include "ee_counter_search.h"   // STORES-RT wide counter search (worker thread) + the launch detector of the found counter
#include "store_tracker.h"   // acz::stores::signal()
#include "aircraft_auto.h"
#include "view_mode.h"   // VIEW-MODE (xr_terrain_stage pov:1): the raw E6 view sample for the consumer's classifier
#include "pov_render.h"  // POV-RENDER povc:1: the consumer's ACM census request (acm_request generation), polled with the 5 s window
#include "diagnostics_log.h"
#include "perf_counters.h"   // FPS investigation: ee-present / ee-seam cost (observe only)
#include "frame_pacing.h"    // UNCAP: Present override decision, Present statistics, simulation-rate guard
#include "vscreen_capture.h" // VSCREEN: the game's final image copied before the native Present (vscreen:1 only)
#include <windows.h>
#include "briefing_observe.h" // BRIEFING-3D: briefing-observe.arm draw histogram, polled/emitted with the present-stats window
#include "ui3d_census.h"      // UI3D: ui3d-census.arm read-only menu draw census, polled with the window, one sampled frame per second
#include "brief3d_capture.h" // BRIEF3D: brief3d-capture.arm read-only map-draw capture, polled with the window, one line per second
#include "brief3d_live.h"     // BRIEF3D live (brief3d:1): the present closes the map frame (off: one relaxed load)
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>
#include <cstdlib>

namespace acz::ee_camera {
namespace {

// Tick-side memory: reads inside the validated block (checked once per tick)
// are SEH-guarded copies; any other read is VirtualQuery-checked first.
struct ProcessMemory final:Memory {
    uint64_t trusted=0; // block range [trusted, +64 MiB) verified by range_ok this tick (0 = none)
    bool inside(uint64_t a,uint32_t n) const noexcept {return trusted&&a>=trusted&&a+n<=trusted+block_bytes&&a+n>=a;}
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        if(!inside(a,n)&&!range_ok(a,n))return false;
        return guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t a,float v) noexcept override {
        if(!inside(a,4))return false; // writes only ever inside the (validated) block
        return guarded_store(reinterpret_cast<volatile float*>(uintptr_t(a)),v);
    }
};
// Worker-side memory (relative scan): every chunk VirtualQuery-checked, then a
// guarded copy. Never writes.
struct WorkerMemory final:Memory {
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        return range_ok(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t,float) noexcept override {return false;}
};
// Seam-side memory (A/B/C): only the known block range, SEH-guarded, no syscalls.
struct SeamMemory final:Memory {
    Shared* sh=nullptr;
    bool inside(uint64_t a,uint32_t n) const noexcept {return sh&&in_block(*sh,a,n);}
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        return inside(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t a,float v) noexcept override {
        return inside(a,4)&&guarded_store(reinterpret_cast<volatile float*>(uintptr_t(a)),v);
    }
};
// Option 3 stage 2 (ee_look_write.h): the ONLY memory object that ever writes the look fields.
// Every write is preceded by a VirtualQuery of the 4-byte target: committed, MEM_PRIVATE, exactly
// PAGE_READWRITE (no guard/no-access/execute), wholly inside one region; page protection is never
// changed. Reads: range_ok + guarded copy. The store itself is SEH-guarded.
struct LookMemory final:Memory {
    static bool writable_private(uint64_t a,uint32_t n) noexcept {
        MEMORY_BASIC_INFORMATION mbi{};
        if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(a)),&mbi,sizeof mbi)!=sizeof mbi)return false;
        if(mbi.State!=MEM_COMMIT||mbi.Type!=MEM_PRIVATE)return false;
        if((mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS))||(mbi.Protect&0xFF)!=PAGE_READWRITE)return false;
        const uint64_t base=uint64_t(uintptr_t(mbi.BaseAddress));
        return a>=base&&a+n>=a&&a+n<=base+mbi.RegionSize;
    }
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        return range_ok(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t a,float v) noexcept override {
        if((a&3)!=0||!writable_private(a,4))return false;
        return guarded_store(reinterpret_cast<volatile float*>(uintptr_t(a)),v);
    }
};
// D-269 follow-up (ee_look_input_trace.h): READ-ONLY memory for the look-input trace. write_float always fails.
struct TraceMemory final:Memory {
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        return range_ok(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t,float) noexcept override {return false;}
};
// Lens/FOV phase 0 (ee_lens_scan.h): READ-ONLY memory for the lens scanner (Present tick and worker). write_float always fails.
struct LensMemory final:LensMem {
    // Astra review 2026-10-26: committed readable DATA only (no guard/no-access/executable, no image sections); write-combined/uncached tolerated on MEM_PRIVATE only (lens_classify, step 47 diagnosis).
    bool query(uint64_t a,LensRegion& out) noexcept override {
        MEMORY_BASIC_INFORMATION mbi{};
        out=LensRegion{};
        if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(a)),&mbi,sizeof mbi)!=sizeof mbi)return false;
        out.base=uint64_t(uintptr_t(mbi.BaseAddress));out.size=uint64_t(mbi.RegionSize);out.alloc=uint64_t(uintptr_t(mbi.AllocationBase));
        out.committed=mbi.State==MEM_COMMIT;
        out.state=mbi.State;out.type=mbi.Type;out.protect=mbi.Protect;
        const LensPageClass c=lens_classify(out.state,out.type,out.protect);
        out.exec=c==lpc_executable;
        out.data=c==lpc_data;
        return a>=out.base&&a-out.base<out.size;
    }
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {
        return range_ok(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);
    }
    bool write_float(uint64_t,float) noexcept override {return false;}
};
struct ProcessScanner final:Scanner {
    void scan(CandidateCollector& out) noexcept override {
        SYSTEM_INFO si{};GetSystemInfo(&si);
        uint64_t p=uint64_t(uintptr_t(si.lpMinimumApplicationAddress));
        const uint64_t end=uint64_t(uintptr_t(si.lpMaximumApplicationAddress));
        for(unsigned guard=0;p<end&&guard<1000000;++guard) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p)),&mbi,sizeof mbi)!=sizeof mbi)break;
            const uint64_t base=uint64_t(uintptr_t(mbi.BaseAddress)),next=base+mbi.RegionSize;
            if(mbi.State!=MEM_FREE) {
                Region r;r.base=base;r.size=mbi.RegionSize;r.allocation_base=uint64_t(uintptr_t(mbi.AllocationBase));
                r.committed=mbi.State==MEM_COMMIT;r.is_private=mbi.Type==MEM_PRIVATE;r.read_write=rw_protect(mbi.Protect);
                out.add(r);
            }
            if(next<=p)break;
            p=next;
        }
    }
};
LARGE_INTEGER g_qpf{};
uint64_t qpc_us() noexcept {
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    return g_qpf.QuadPart?uint64_t(double(now.QuadPart)*1e6/double(g_qpf.QuadPart)):0;
}
void worker_yield() {Sleep(1);}

// Dedicated bounded ee-camera log (D-245): %LOCALAPPDATA%\acz-vr\logs\ee-camera.log,
// the previous one kept as ee-camera.prev.log. 32 MiB cap, no rotation, so the
// first 300 observation frames can never be evicted (the shared diagnostics
// log rotates at 3 x 512 KB). Every line also still goes to diagnostics.
// (ee_camera_logfile.h; rev 4: shared read/write so the file can be copied mid-run.)
EeLogFile g_eeLog;
void open_ee_log() noexcept {
    wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n||n>=MAX_PATH-64)return;
    wchar_t dir[MAX_PATH],path[MAX_PATH],prev[MAX_PATH];
    _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr",local);CreateDirectoryW(dir,nullptr);
    _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\logs",local);CreateDirectoryW(dir,nullptr);
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\ee-camera.log",dir);_snwprintf_s(prev,MAX_PATH,_TRUNCATE,L"%s\\ee-camera.prev.log",dir);
    g_eeLog.open(path,prev);
}
// FPS investigation (perf_lean, docs/world-render/fps-investigation.md): the per-frame-ish OBSERVATION lines of the probe (obs/obs2 samples,
// look timeline, activity windows, region scans, discovery hexdumps) are diagnostics only; with perf_lean they are not formatted into the
// shared ring or the ee-camera log. Everything else (e6-locate, summaries, arming, refusals, results) is kept. The probe's state machine is
// unchanged: only the line is dropped.
bool lean_drop(const char* text) noexcept {
    static std::atomic<int> s_lean{-1};
    int lean=s_lean.load(std::memory_order_relaxed);
    if(lean<0){auto* l=diagnostics::sink();if(!l)return false;lean=l->perf_lean()?1:0;s_lean.store(lean,std::memory_order_relaxed);}
    if(!lean||!text)return false;
    static constexpr const char* noisy[]={"ee-camera;obs","ee-camera;event=timeline;","ee-camera;event=activity;","ee-camera;event=region-scan;",
                                          "ee-camera;event=hexdump;"};
    for(const char* p:noisy)if(std::strncmp(text,p,std::strlen(p))==0)return true;
    return false;
}
struct LogSink final:Sink {
    void line(bool important,const char* text) noexcept override {
        if(lean_drop(text))return;
        diagnostics::emit(diagnostics::record(important?diagnostics::Event::device:diagnostics::Event::requirements_metadata,0,0,0,text));
        g_eeLog.line(text,double(qpc_us())/1000.0);
    }
};

// Every committed readable region >= 64 KiB (private, mapped, image), minus
// this proxy's own module/allocations and write-combined/uncached (GPU upload)
// mappings.
struct ProcessRegions final:RegionSource {
    uint64_t ownAlloc=0;uint64_t exclLo[8]{},exclHi[8]{};
    unsigned enumerate(RegionInfo* out,unsigned cap,RegionCensus& c) noexcept override {
        c=RegionCensus{};unsigned n=0;
        SYSTEM_INFO si{};GetSystemInfo(&si);
        uint64_t p=uint64_t(uintptr_t(si.lpMinimumApplicationAddress));const uint64_t end=uint64_t(uintptr_t(si.lpMaximumApplicationAddress));
        for(unsigned guard=0;p<end&&guard<2000000;++guard) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p)),&mbi,sizeof mbi)!=sizeof mbi)break;
            const uint64_t base=uint64_t(uintptr_t(mbi.BaseAddress)),next=base+mbi.RegionSize;
            if(mbi.State==MEM_COMMIT) {
                ++c.seen;
                const DWORD pr=mbi.Protect,b=pr&0xFF;
                const bool readable=!(pr&(PAGE_GUARD|PAGE_NOACCESS))&&(b==PAGE_READONLY||b==PAGE_READWRITE||b==PAGE_WRITECOPY||
                                    b==PAGE_EXECUTE_READ||b==PAGE_EXECUTE_READWRITE||b==PAGE_EXECUTE_WRITECOPY);
                bool own=uint64_t(uintptr_t(mbi.AllocationBase))==ownAlloc;
                for(int i=0;i<8;++i)own=own||(exclHi[i]&&base<exclHi[i]&&next>exclLo[i]);
                if(!readable)++c.unreadable;
                else if(pr&(PAGE_WRITECOMBINE|PAGE_NOCACHE))++c.gpu;
                else if(own)++c.own;
                else if(mbi.RegionSize<region_min)++c.tiny;
                else if(n>=cap)++c.overflow;
                else {
                    RegionInfo& r=out[n++];r.base=base;r.size=mbi.RegionSize;r.alloc=uint64_t(uintptr_t(mbi.AllocationBase));r.protect=pr;
                    r.type=mbi.Type==MEM_PRIVATE?rt_private:mbi.Type==MEM_MAPPED?rt_mapped:mbi.Type==MEM_IMAGE?rt_image:rt_unknown;
                    r.writable=b==PAGE_READWRITE||b==PAGE_WRITECOPY||b==PAGE_EXECUTE_READWRITE||b==PAGE_EXECUTE_WRITECOPY;
                    ++c.kept;c.bytes+=r.size;
                }
            }
            if(next<=p)break;
            p=next;
        }
        return n;
    }
};
struct MailboxBasis final:BasisSource {
    const BasisMailbox* mb=nullptr;
    bool latest(Basis& out) noexcept override {return mb&&mb->try_read(out);}
};
struct SleepWaiter final:Waiter {void wait_ms(unsigned ms) noexcept override {Sleep(ms);}};
void* alloc_pages(size_t bytes) noexcept {return VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);}
// Worker thread (never the game's draw or Present path), read-only:
//  * on request: a WIDE relative-geometry pass over all regions, budget 1 GiB
//    per request, resuming across requests (complete = the list was covered);
//  * otherwise, while the cockpit basis is live: differential windows (8 MiB)
//    triggered by > 5 deg turns of the recovered camera;
//  * every 10 s: region table summary + diff summary + top-10 candidates.
void trace_background() noexcept;   // writes closed look-input-trace windows (worker thread, never the render path)
void ps2_background() noexcept;     // PS2 address probe (read-only; worker thread)
bool store_watch_on() noexcept;     // STORES-RT (defined with the E6 tick)
const acz::e6search::Cfg& store_cfg() noexcept;
void e6_background() noexcept;      // E6-ANCHOR-FREE: candidate-block enumeration (VirtualQuery walk only; worker thread)
void lens_background() noexcept;    // lens/FOV phase 0: bounded read-only scan step + report (worker thread, never the render path)
class WorkerRequester final:public ScanRequester {
public:
    static constexpr uint64_t wide_budget=1ull<<30;
    static constexpr uint32_t diff_window_bytes=8u<<20;
    const BasisMailbox* mailbox=nullptr;ProcessRegions regionsSrc;
    void start() noexcept {
        scratch_=static_cast<float*>(alloc_pages(scan_scratch_floats*4));
        A_=static_cast<uint32_t*>(alloc_pages(diff_window_bytes));B_=static_cast<uint32_t*>(alloc_pages(diff_window_bytes));
        regions_=static_cast<RegionInfo*>(alloc_pages(sizeof(RegionInfo)*region_cap));
        diff_=static_cast<DiffScanner*>(alloc_pages(sizeof(DiffScanner)));
        if(!scratch_||!A_||!B_||!regions_||!diff_){started_=false;return;}
        new(diff_) DiffScanner();
        const void* own[4]={scratch_,A_,B_,diff_};const size_t sz[4]={scan_scratch_floats*4,diff_window_bytes,diff_window_bytes,sizeof(DiffScanner)};
        for(int i=0;i<4;++i){regionsSrc.exclLo[i]=uint64_t(uintptr_t(own[i]));regionsSrc.exclHi[i]=regionsSrc.exclLo[i]+sz[i];}
        basis_.mb=mailbox;
        try {std::thread([this]{loop();}).detach();started_=true;} catch(...) {started_=false;}
    }
    bool request(uint64_t block,const Basis* basis,bool dump) noexcept override {
        if(!started_)return false;
        std::lock_guard<std::mutex> l(m_);
        if(pending_||ready_)return false;
        block_=block;dump_=dump;hasBasis_=basis!=nullptr;if(basis)basis_snap_=*basis;pending_=true;
        return true;
    }
    const ScanResult* poll() noexcept override {
        std::lock_guard<std::mutex> l(m_);
        if(!ready_)return nullptr;
        ready_=false;return &result_;
    }
    // Callback reads only the worker's last complete immutable report.
    // Both snapshot acquisition and file output are try-only.
    void final_report(const char* trigger) noexcept {
        struct TrySink final:Sink {void line(bool,const char* t) noexcept override {g_eeLog.try_line(t,double(qpc_us())/1000.0);}} s;
        reports_.try_report(trigger,s);
        g_eeLog.final_flush();
    }
private:
    void enumerate(bool full) noexcept {
        nregions_=regionsSrc.enumerate(regions_,region_cap,census_);lastEnum_=qpc_us();
        log_region_table(regions_,nregions_,census_,sink_,full);
        reports_.publish(regions_,nregions_,census_,cursor_.passes,*diff_);
    }
    void loop() noexcept {
        uint64_t lastPublished=0,lastBasisUs=0,lastReport=0,lastWideEnd=0;bool tableLogged=false;
        for(;;) {
            Sleep(5);
            const uint64_t now=qpc_us();
            g_eeLog.flush_if_due(double(now)/1000.0); // rev 4: flushed at least once per second
            trace_background();
            lens_background();
            ps2_background();
            e6_background();
            uint64_t block=0;bool pending=false,dump=false,hb=false;Basis b{};
            {std::lock_guard<std::mutex> l(m_);if(pending_&&!ready_){pending=true;block=block_;dump=dump_;hb=hasBasis_;b=basis_snap_;}}
            // Time-share: while the cockpit basis is live, at least 3 s of differential
            // windows run between wide passes.
            const bool live=mailbox&&mailbox->published()!=lastPublished?true:(now-lastBasisUs<=250000&&lastBasisUs!=0);
            if(pending&&(!live||now-lastWideEnd>=3000000)) {
                (void)block; // wide pass: every region (block 0 by contract)
                enumerate(!tableLogged);tableLogged=true; // rev 4: re-enumerated every pass (64 MiB blocks included)
                if(dump)for(unsigned i=0;i<nregions_;++i)
                    if(regions_[i].type==rt_private&&regions_[i].size==block_bytes)dump_e6(mem_,regions_[i].base,sink_,"scan");
                ScanResult r{};
                const uint64_t t0=qpc_us();
                wide_pass(mem_,regions_,nregions_,hb?&b:nullptr,scratch_,wide_budget,cursor_,r,sink_,&worker_yield);
                r.ms=double(qpc_us()-t0)/1000.0;
                log_scan(r,sink_);
                lastWideEnd=qpc_us();
                reports_.publish(regions_,nregions_,census_,cursor_.passes,*diff_);
                std::lock_guard<std::mutex> l(m_);result_=r;pending_=false;ready_=true;
                continue;
            }
            // Cockpit = the producer published a basis within the last 250 ms.
            const uint64_t pub=mailbox?mailbox->published():0;
            if(pub!=lastPublished){lastPublished=pub;lastBasisUs=now;}
            if(tableLogged&&now-lastReport>=10000000) {
                lastReport=now;log_region_table(regions_,nregions_,census_,sink_,false);log_diff(*diff_,regions_,nregions_,sink_,"periodic");
            }
            if(!pub||now-lastBasisUs>250000){Sleep(45);continue;}
            if(!tableLogged||now-lastEnum_>30000000){enumerate(!tableLogged);tableLogged=true;}
            uint64_t addr=0;uint32_t bytes=0;
            if(driver_.next(regions_,nregions_,*diff_,diff_window_bytes,addr,bytes)) {
                (void)diff_window(mem_,basis_,waiter_,addr,bytes,A_,B_,*diff_);
                reports_.publish(regions_,nregions_,census_,cursor_.passes,*diff_);
            }
            else Sleep(50);
        }
    }
    std::mutex m_;bool pending_=false,ready_=false,started_=false,dump_=false,hasBasis_=false;uint64_t block_=0;Basis basis_snap_{};
    ScanResult result_{};WorkerMemory mem_;LogSink sink_;
    float* scratch_=nullptr;uint32_t* A_=nullptr;uint32_t* B_=nullptr;RegionInfo* regions_=nullptr;DiffScanner* diff_=nullptr;
    unsigned nregions_=0;RegionCensus census_{};uint64_t lastEnum_=0;WideCursor cursor_{};DiffDriver driver_{};
    MailboxBasis basis_;SleepWaiter waiter_;
    FinalReportBox reports_;
};

// ------------------------------------------------------------- state
struct Probe {
    Shared sh;ProcessMemory mem;SeamMemory seamMem;ProcessScanner scanner;LogSink sink;BasisMailbox basis;WorkerRequester req;
    Experiment exp;
    LookMemory lookMem;LookWriter look;LookDrive drive;
    TraceMemory traceMem;LookInputTrace* trace=nullptr;   // allocated on its own pages only when ee_camera_look_input_trace is latched
    Ps2Prober* ps2=nullptr;                                // PS2 address probe: own pages, only when ee_ps2_probe is latched (read-only)
    LensMemory lensMem;LensScanner* lens=nullptr;          // allocated on its own pages only when ee_lens_scan is latched (read-only)
    Probe(Options o,LookWriter::Options lw,LookDrive::Options ld):exp(o),look(lw),drive(ld) {seamMem.sh=&sh;req.mailbox=&basis;}
};
Probe* g_probe=nullptr;                       // published before any hook can see it
// rev 4 end-of-run report: on every final factory Release (rate-limited to one per
// 5 s) and once at CRT exit. Try-lock file writes only (a terminated thread may
// have held a lock); nothing is waited for.
std::atomic<uint64_t> g_lastFinalUs{0};std::atomic<bool> g_exitDone{false};
void look_shutdown(const char* why) noexcept;
void trace_drain(bool finish_open) noexcept;
void lens_shutdown(bool exiting) noexcept;
void ps2_shutdown(bool exiting) noexcept;
void final_on_release() noexcept {
    look_shutdown("factory-final-release"); // option 3: restore any look write BEFORE the rate limit below
    trace_drain(false);
    lens_shutdown(false);
    ps2_shutdown(false);
    const uint64_t now=qpc_us(),last=g_lastFinalUs.load();
    if(last&&now-last<5000000)return;
    g_lastFinalUs.store(now);if(g_probe)g_probe->req.final_report("factory-final-release");
}
void final_on_exit() {
    look_shutdown("process-exit");
    trace_drain(true);
    lens_shutdown(true);
    ps2_shutdown(true);
    if(!g_exitDone.exchange(true)&&g_probe)g_probe->req.final_report("process-exit");
}
std::atomic<bool> g_armed{false},g_finished{false};
std::atomic<void*> g_swapchain{nullptr};      // latest game swapchain (identity only, no reference)
std::atomic_flag g_tickBusy=ATOMIC_FLAG_INIT;
std::atomic<uint32_t> g_tid[4]{};std::atomic<uint32_t> g_tidMulti[4]{};
thread_local unsigned t_presentDepth=0;

struct Table {void** vt=nullptr;void* present=nullptr;void* present1=nullptr;};
constexpr unsigned table_cap=4;
Table g_tables[table_cap]{};std::atomic<unsigned> g_tableCount{0};
SRWLOCK g_tableLock=SRWLOCK_INIT;

const Table* find_table(void* self) noexcept {
    void** vt=*reinterpret_cast<void***>(self);
    const unsigned n=g_tableCount.load(std::memory_order_acquire);
    for(unsigned i=0;i<n;++i)if(g_tables[i].vt==vt)return &g_tables[i];
    return nullptr;
}
void note_thread(uint32_t seam) noexcept {
    const uint32_t id=GetCurrentThreadId();uint32_t expected=0;
    if(!g_tid[seam].compare_exchange_strong(expected,id,std::memory_order_relaxed)&&expected!=id)g_tidMulti[seam].store(1,std::memory_order_relaxed);
}
void emit_threads(const char* when) noexcept {
    char t[192];
    std::snprintf(t,sizeof t,"ee-camera;event=threads;when=%s;A=%u%s;B=%u%s;C=%u%s",when,g_tid[1].load(),g_tidMulti[1].load()?"+":"",
        g_tid[2].load(),g_tidMulti[2].load()?"+":"",g_tid[3].load(),g_tidMulti[3].load()?"+":"");
    g_probe->sink.line(false,t);
}

// ------------------------------------------------------------- option 3 stage 2 (look write / drive)
// Runs on the Present tick (the probe's cadence) after the read-only tick. One lock: the exit paths
// (factory final release, CRT exit) take it too (bounded wait, then proceed regardless: the restore
// is one idempotent store of a known value).
std::atomic_flag g_lookLock=ATOMIC_FLAG_INIT;
wchar_t g_stopPath[MAX_PATH]{};std::atomic<bool> g_stopPathOk{false};bool g_lastStop=false;
wchar_t g_lensStopPath[MAX_PATH]{};std::atomic<bool> g_lensStopPathOk{false};
bool stop_file_present() noexcept {
    return g_stopPathOk.load(std::memory_order_relaxed)&&GetFileAttributesW(g_stopPath)!=INVALID_FILE_ATTRIBUTES;
}
void init_stop_path() noexcept {
    wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n||n>=MAX_PATH-64)return;
    _snwprintf_s(g_stopPath,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\look-write.stop",local);
    g_stopPathOk.store(true);
    _snwprintf_s(g_lensStopPath,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\lens-scan.stop",local);
    g_lensStopPathOk.store(true);
}
void look_tick_impl(Probe* p,double ms) noexcept {
    static unsigned s_calls=0;++s_calls;
    const bool active=p->look.phase()==LookWriter::p_settling||p->look.phase()==LookWriter::p_writing||p->drive.phase()==LookDrive::p_active;
    LookIn in;in.now_ms=ms;in.frame=p->sh.frame.load(std::memory_order_acquire);in.tid=GetCurrentThreadId();
    in.stop_file=(active||s_calls%30==0)?stop_file_present():g_lastStop;
    g_lastStop=in.stop_file;
    if(p->look.phase()!=LookWriter::p_off) {
        p->exp.look_view(p->look.locked_eff(),in.v);
        p->look.tick(p->lookMem,p->sink,in);
    }
    if(p->drive.phase()!=LookDrive::p_off&&p->drive.phase()!=LookDrive::p_done&&p->drive.phase()!=LookDrive::p_aborted) {
        DriveIn d;d.base=in;p->exp.look_view(p->drive.locked_eff(),d.base.v);
        d.gate_ok=p->look.succeeded();d.gate_eff=p->look.eff();
        const char* src="";double upr=1.0;d.calibrated=p->look.calibration(upr,&src);d.units_per_rad=d.calibrated?upr:1.0;
        double hy=0,hp=0,hms=0;
        if(g_look_head.read(hy,hp,hms)){d.head.valid=true;d.head.yaw=hy;d.head.pitch=hp;d.head.age_ms=ms-hms;}
        p->drive.tick(p->lookMem,p->sink,d);
    }
}
// Last-resort SEH net around the whole look tick (a bug in our own code, never the guarded game
// accesses): restore and give up. These two frames hold no C++ objects with destructors (C2712).
void look_emergency(Probe* p) noexcept {
    __try {p->drive.abort_and_restore(p->lookMem,p->sink,"internal-exception");p->look.abort_and_restore(p->lookMem,p->sink,"internal-exception");}
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void look_tick(Probe* p,double ms) noexcept {
    if(p->look.phase()==LookWriter::p_off&&p->drive.phase()==LookDrive::p_off)return;
    if(g_lookLock.test_and_set(std::memory_order_acquire))return; // an exit path is restoring: do nothing
    __try {look_tick_impl(p,ms);}
    __except(EXCEPTION_EXECUTE_HANDLER) {look_emergency(p);}
    g_lookLock.clear(std::memory_order_release);
}
// Every exit path (factory final release, CRT exit): restore anything written. Try-lock file sink.
struct TryLookSink final:Sink {void line(bool,const char* t) noexcept override {g_eeLog.try_line(t,double(qpc_us())/1000.0);}};
void look_shutdown_seh(Probe* p,Sink& s,const char* why) noexcept {
    __try {p->drive.abort_and_restore(p->lookMem,s,why);p->look.abort_and_restore(p->lookMem,s,why);}
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void look_shutdown(const char* why) noexcept {
    Probe* p=g_probe;if(!p)return;
    if(p->look.phase()==LookWriter::p_off&&p->drive.phase()==LookDrive::p_off)return;
    for(unsigned i=0;i<200&&g_lookLock.test_and_set(std::memory_order_acquire);++i)Sleep(1);
    TryLookSink s;
    look_shutdown_seh(p,s,why);
    g_eeLog.final_flush();
}

// ------------------------------------------------------------- D-269 follow-up: read-only look-input trace
// File: %LOCALAPPDATA%\acz-vr\logs\look-input-trace.bin (previous run kept as .prev.bin), shared read, windows appended as
// they close. Only drain() (worker thread / exit path) touches it; the render callback only fills the in-memory arena.
struct TraceFileOut final:TraceOut {
    HANDLE h=INVALID_HANDLE_VALUE;bool tried=false;
    void open() noexcept {
        tried=true;
        wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        if(!n||n>=MAX_PATH-96)return;
        wchar_t dir[MAX_PATH],path[MAX_PATH],prev[MAX_PATH];
        _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr",local);CreateDirectoryW(dir,nullptr);
        _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\logs",local);CreateDirectoryW(dir,nullptr);
        _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\look-input-trace.bin",dir);_snwprintf_s(prev,MAX_PATH,_TRUNCATE,L"%s\\look-input-trace.prev.bin",dir);
        MoveFileExW(path,prev,MOVEFILE_REPLACE_EXISTING);
        h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    bool write(const void* p,size_t n) noexcept override {
        if(!tried)open();
        if(h==INVALID_HANDLE_VALUE)return false;
        const uint8_t* s=static_cast<const uint8_t*>(p);
        while(n) {
            DWORD w=0;const DWORD chunk=n>(1u<<20)?(1u<<20):DWORD(n);
            if(!WriteFile(h,s,chunk,&w,nullptr)||!w)return false;
            s+=w;n-=w;
        }
        return true;
    }
} g_traceOut;
void trace_drain(bool finish_open) noexcept {
    Probe* p=g_probe;if(!p||!p->trace)return;
    if(finish_open) {
        // Exit path only: take the tick lock (never released; the process is ending) so no Present tick runs concurrently.
        bool locked=false;
        for(unsigned i=0;i<200&&!locked;++i) {
            locked=!g_tickBusy.test_and_set(std::memory_order_acquire);
            if(!locked)Sleep(1);
        }
        if(locked) {
            struct TrySink final:Sink {void line(bool,const char* t) noexcept override {g_eeLog.try_line(t,double(qpc_us())/1000.0);}} s;
            p->trace->finish(s,tc_exit);
        }
    }
    p->trace->drain(g_traceOut);
}
void trace_background() noexcept {
    Probe* p=g_probe;if(!p||!p->trace||p->trace->windows_closed()==0)return;
    p->trace->drain(g_traceOut);
}
void trace_tick(Probe* p,double ms) noexcept {
    LookInputTrace* t=p->trace;
    if(!t||t->exhausted())return;
    LookIn in;in.now_ms=ms;in.frame=p->sh.frame.load(std::memory_order_acquire);in.tid=GetCurrentThreadId();
    p->exp.look_view(0,in.v);
    const uint64_t t0=qpc_us();
    t->tick(p->traceMem,p->sink,in);
    t->note_cost_us(double(qpc_us()-t0));
}

// ------------------------------------------------------------- lens/FOV widen plan, phase 0: READ-ONLY lens scanner
// Present tick: waiting gate + per-frame re-read of the candidates. Worker thread: the bounded scan and the report. Neither ever writes game
// memory (LensMemory::write_float fails). Kill switch: %LOCALAPPDATA%\acz-vr\lens-scan.stop (the look-write.stop switch also stops it).
// File: %LOCALAPPDATA%\acz-vr\logs\lens-scan-candidates.txt (previous run kept as .prev.txt), last line `sha256=<hex of the bytes before it>`.
bool lens_stop_present() noexcept {
    if(g_lensStopPathOk.load(std::memory_order_relaxed)&&GetFileAttributesW(g_lensStopPath)!=INVALID_FILE_ATTRIBUTES)return true;
    return stop_file_present();
}
struct LensFileOut final:LensOut {
    bool moved=false;const wchar_t* stem=L"lens-scan-candidates";    // logs\<stem>.txt, the previous run kept as <stem>.prev.txt
    bool write_file(const char* text,size_t n) noexcept override {
        wchar_t local[MAX_PATH];const DWORD ln=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        if(!ln||ln>=MAX_PATH-96)return false;
        wchar_t dir[MAX_PATH],path[MAX_PATH],prev[MAX_PATH];
        _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr",local);CreateDirectoryW(dir,nullptr);
        _snwprintf_s(dir,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\logs",local);CreateDirectoryW(dir,nullptr);
        _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\%s.txt",dir,stem);_snwprintf_s(prev,MAX_PATH,_TRUNCATE,L"%s\\%s.prev.txt",dir,stem);
        if(!moved){MoveFileExW(path,prev,MOVEFILE_REPLACE_EXISTING);moved=true;}
        HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        const char* s=text;bool ok=true;
        while(n&&ok){DWORD w=0;if(!WriteFile(h,s,DWORD(n),&w,nullptr)||!w){ok=false;break;}s+=w;n-=w;}
        CloseHandle(h);
        return ok;
    }
} g_lensOut,g_ps2Out;
LensMemory g_lensWorkerMem;
bool g_lensStopLast=false;
void lens_tick(Probe* p,double ms) noexcept {
    LensScanner* L=p->lens;
    if(!L||L->terminal())return;
    static unsigned s_calls=0;++s_calls;
    LensIn in;in.now_ms=ms;in.frame=p->sh.frame.load(std::memory_order_acquire);
    if(s_calls%15==0)g_lensStopLast=lens_stop_present();
    in.stop_file=g_lensStopLast;
    p->exp.look_view(0,in.v);
    L->tick(p->lensMem,p->sink,in);
}
// Second sample point (seam B, first replayed member): cheap, read-only, try-lock, once per frame (see LensScanner::sample_b).
void lens_sample_b(Probe* p) noexcept {
    LensScanner* L=p->lens;
    if(L&&L->phase()==LensScanner::p_tracking)L->sample_b(p->lensMem);
}
void lens_background() noexcept {
    Probe* p=g_probe;
    if(!p||!p->lens)return;
    LensScanner& L=*p->lens;
    LogSink sink;
    if(L.phase()==LensScanner::p_scanning) {
        LensWorkIn in;in.now_ms=double(qpc_us())/1000.0;in.stop_file=lens_stop_present();
        uint64_t blocks[CandidateCollector::cap]{};
        if(L.needs_blocks()) {
            CandidateCollector coll;ProcessScanner ps;ps.scan(coll);
            for(unsigned i=0;i<coll.count();++i)blocks[i]=coll.base(i);
            in.have_blocks=true;in.blocks=blocks;in.nblocks=coll.count();
        }
        L.work(g_lensWorkerMem,sink,in);
    }
    if(L.terminal())L.drain(sink,g_lensOut);
}
// Exit paths: the factory final release only reports a finished scan; the CRT exit also ends a running one (partial result).
void lens_shutdown(bool exiting) noexcept {
    Probe* p=g_probe;if(!p||!p->lens)return;
    if(exiting) {
        for(unsigned i=0;i<200&&g_tickBusy.test_and_set(std::memory_order_acquire);++i)Sleep(1);   // never released: the process is ending
        p->lens->finish("process-exit");
    }
    if(!p->lens->terminal())return;
    TryLookSink s;
    p->lens->drain(s,g_lensOut);
    g_eeLog.final_flush();
}

// ------------------------------------------------------------- PS2 address probe (ee_ps2_probe.h): READ-ONLY
// Present tick only records the tracks-look anchor + the stop flag; the worker does every read (LensMemory::write_float fails) and writes
// %LOCALAPPDATA%\acz-vr\logs\ps2-probe.txt (previous run kept as ps2-probe.prev.txt), last line `sha256=<hex of the bytes before it>`.
bool g_ps2StopLast=false;
void ps2_tick(Probe* p,double ms) noexcept {
    Ps2Prober* P=p->ps2;
    if(!P||P->terminal())return;
    static unsigned s_calls=0;++s_calls;
    LensIn in;in.now_ms=ms;in.frame=p->sh.frame.load(std::memory_order_acquire);
    if(s_calls%15==0)g_ps2StopLast=lens_stop_present();
    in.stop_file=g_ps2StopLast;
    p->exp.look_view(0,in.v);
    P->tick(in);
}
void ps2_background() noexcept {
    Probe* p=g_probe;
    if(!p||!p->ps2)return;
    Ps2Prober& P=*p->ps2;
    LogSink sink;
    if(!P.terminal()) {
        Ps2WorkIn in;in.now_ms=double(qpc_us())/1000.0;in.stop_file=lens_stop_present();
        P.work(g_lensWorkerMem,sink,in);
    }
    if(P.terminal())P.drain(sink,g_ps2Out);
}
void ps2_shutdown(bool exiting) noexcept {
    Probe* p=g_probe;if(!p||!p->ps2)return;
    if(exiting)p->ps2->finish("process-exit");
    else if(!p->ps2->terminal())return;
    TryLookSink s;
    p->ps2->drain(s,g_ps2Out);
    g_eeLog.final_flush();
}

// ------------------------------------------------------------- J-terrain-render camera feed (xr_terrain_render): READ-ONLY
// Once per Present: the E6 camera (campos +0xFDD860..68, rot pitch/yaw/roll +0xFDD870..78, heading +0xFDD448) is copied from the validated 64 MiB block into the
// terrain mailbox (terrain_camera.h). Guarded copies inside the block only; no writes. Off (the default): one cached flag test.
// E6-ANCHOR-FREE (ee_e6_locator.h, docs/world-render/e6-anchor-free.md): the xr_terrain_stage option e6loc:auto (default) | content selects the
// content-validated locator below; e6loc:probe keeps exactly the previous feed (the code after the mode test). Terrain key off: unchanged.
// -1 = terrain key off; else the e6loc mode (a bad e6loc value refuses the whole terrain in the proxy; the feed then falls back to 'probe').
// Called by the Present tick AND the render thread (seam B): the lazily computed value is an atomic (review R-2; both threads compute the same
// value from the latched config, so a duplicate computation is harmless, but a plain static int was a data race).
int terrain_feed_mode() noexcept {
    static std::atomic<int> s_mode{-2};
    int v=s_mode.load(std::memory_order_acquire);
    if(v==-2) {
        auto* l=diagnostics::sink();
        if(!l||!l->xr_terrain_render())v=-1;
        else {bool ok=true;const auto m=acz::e6loc::mode_in(l->xr_terrain_stage(),ok);v=ok?int(m):int(acz::e6loc::Mode::probe);}
        s_mode.store(v,std::memory_order_release);
    }
    return v;
}
// Read-only memory for the locator: VirtualQuery-checked (committed read-write pages) + SEH-guarded copy. No write operation exists.
struct E6ReadMem final:acz::e6loc::ReadMem {
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {return range_ok(a,n)&&guarded_copy(out,reinterpret_cast<const void*>(uintptr_t(a)),n);}
};
// The probe's observation base (e6loc:auto fallback): the tick-side ProcessMemory, both windows inside the probe's block.
struct E6ProbeMem final:acz::e6loc::ReadMem {
    ProcessMemory* m=nullptr;const Shared* sh=nullptr;
    bool read(uint64_t a,void* out,uint32_t n) noexcept override {return m&&sh&&in_block(*sh,a,n)&&m->read(a,out,n);}
};
struct E6Sink final:acz::e6loc::Sink {
    void line(bool important,const char* t) noexcept override {LogSink s;s.line(important,t);}
};
struct E6State {
    acz::e6loc::Mode mode=acz::e6loc::Mode::automatic;
    acz::e6loc::Locator loc;
    std::mutex m;acz::e6loc::CandidateBuilder pending;bool ready=false;   // worker -> Present tick (try-lock on the Present side)
    std::atomic<bool> want{false};                                      // Present tick -> worker: enumerate now
    // E6-RELOCATED (e6-anchor-free.md section 11): the structure is not always at +0xFDD40C (m03k: +0xCA811C / +0xCAAC6C). Worker-only: the last
    // region enumeration and the content scan of those blocks; the hits reach the locator as 's' candidates with every enumeration/finished pass.
    acz::e6loc::CandidateBuilder regions;acz::e6loc::StructScanner scan;
    std::atomic<bool> searching{true};                                  // Present tick -> worker: the locator has no lock (scan while true)
    // E6-RELOCATED: the e6loc:auto probe-base fallback publishes only a base that passed the lock rules (FallbackGate, Present tick); the render-thread
    // frame tick reads the probe base only while it equals this qualified base (0 = none).
    acz::e6loc::FallbackGate gate;std::atomic<uint64_t> fbBase{0};uint64_t fbLogged=0;
    acz::e6store::AmmoWatch watch;   // STORES-RT: only started (allocated) when the xr_cockpit_model option stwatch:1 is set; Present tick only
    // STORES-RT wide search (ee_counter_search.h): the Present tick publishes the locked block base (0 = not locked); the worker thread scans the whole 64 MiB block
    // for the HUD missile count (stwatch:1 only; worker-only object). det/detBase: the launch detector of the found counter (stctr:), Present tick only.
    std::atomic<uint64_t> stBase{0};
    acz::e6search::CounterSearch search;
    acz::e6search::LaunchDetector det;uint64_t detBase=0;unsigned detLines=0;bool detYieldLogged=false;
    // CAM-EPOCH review 2: the locked base reaches the render-thread frame tick ONLY through acz::terrain::cam_epochs().set_source(), together with its
    // lock epoch (one coherent transition); the frame tick captures both before its read (acz::terrain::read_publish).
    bool published=false;
    uint32_t lockEpoch=0;uint64_t lockedBase=0,discont=0;               // CAM-EPOCH: last values pushed to acz::terrain::cam_epochs()
};
std::atomic<E6State*> g_e6{nullptr};   // created once by the Present tick (terrain key on, mode != probe), never freed (process lifetime)
// CAM-EPOCH: t_read = steady-clock ns when the E6 read started (the publication stamp is taken after the read). lock/stage = the identity OF THE READ
// (the Present tick is the lock writer itself; its stage is the one captured before the read, see e6_terrain_tick).
void e6_publish(acz::terrain::CamMailbox& mb,const acz::e6loc::Sample& s,int64_t t_read,uint32_t lock,uint32_t stage) noexcept {
    mb.publish_epochs(lock,stage,s.cam,s.rot[0],s.rot[1],s.rot[2],s.heading,0,t_read);   // the same words as the probe feed: campos, rot pitch/yaw/roll, heading
}
void sample_words(const acz::e6loc::Sample& s,float v[7]) noexcept {for(int i=0;i<3;++i){v[i]=s.cam[i];v[3+i]=s.rot[i];}v[6]=s.heading;}
// Worker thread: the candidate list (a VirtualQuery walk; no memory contents read), at most every 500 ms and only when asked; E6-RELOCATED: while the
// locator searches, a bounded read-only content scan of those blocks (StructScanner, 4 MiB per call, a new pass at most every 3 s or at once when a search
// starts) whose hits are appended as 's' candidates. Reads: E6ReadMem (VirtualQuery-checked + SEH-guarded copies); nothing is written.
void e6_background() noexcept {
    E6State* e=g_e6.load(std::memory_order_acquire);
    if(!e)return;
    if(store_watch_on()) {   // STORES-RT wide counter search: read-only, bounded (one scan per second), worker thread only (never the render/Present path)
        if(!e->search.active()&&e->search.start(store_cfg().target)&&store_cfg().ntrack&&!store_cfg().bad)e->search.set_track(store_cfg().track,store_cfg().ntrack);
        if(const uint64_t sb=e->stBase.load(std::memory_order_acquire)) {
            E6ReadMem rm;E6Sink sk;
            e->search.tick(rm,sb,sb+acz::e6loc::kWinA,double(qpc_us())/1000.0,sk);
        }
    }
    constexpr uint64_t kScanBudget=4ull<<20,kRescanUs=3000000;
    static uint64_t s_last=0,s_passEnd=0,s_passStart=0;static bool s_wasSearching=false;
    const uint64_t now=qpc_us();
    const bool searching=e->searching.load(std::memory_order_acquire);
    bool publish=false;
    if(e->want.load(std::memory_order_acquire)&&(!s_last||now-s_last>=500000)) {
        s_last=now;
        acz::e6loc::CandidateBuilder b;
        SYSTEM_INFO si{};GetSystemInfo(&si);
        uint64_t p=uint64_t(uintptr_t(si.lpMinimumApplicationAddress));const uint64_t end=uint64_t(uintptr_t(si.lpMaximumApplicationAddress));
        for(unsigned guard=0;p<end&&guard<2000000;++guard) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p)),&mbi,sizeof mbi)!=sizeof mbi)break;
            const uint64_t base=uint64_t(uintptr_t(mbi.BaseAddress)),next=base+mbi.RegionSize;
            if(mbi.State!=MEM_FREE) {
                acz::e6loc::RegionDesc r;r.base=base;r.size=mbi.RegionSize;r.alloc=uint64_t(uintptr_t(mbi.AllocationBase));
                r.committed=mbi.State==MEM_COMMIT;r.is_private=mbi.Type==MEM_PRIVATE;r.read_write=rw_protect(mbi.Protect);
                b.add(r);
            }
            if(next<=p)break;
            p=next;
        }
        b.finish();
        e->regions=b;publish=true;
        e->want.store(false,std::memory_order_release);
    }
    if(!searching) {if(e->scan.active())e->scan.cancel();}
    else if(!e->scan.active()&&e->regions.count()&&(!s_wasSearching||!s_passEnd||now-s_passEnd>=kRescanUs)) {e->scan.start(e->regions);s_passStart=now;}
    s_wasSearching=searching;
    if(searching&&e->scan.active()) {
        E6ReadMem rm;
        if(e->scan.step(rm,kScanBudget)) {
            s_passEnd=qpc_us();publish=true;
            char t[448];int n=std::snprintf(t,sizeof t,"e6-locate;event=struct-scan;pass=%llu;blocks=%u;bytes=%llu;unreadable-chunks=%u;ms=%.0f;hits=%u;dropped=%u;list=",
                static_cast<unsigned long long>(e->scan.passes()),e->scan.blocks(),static_cast<unsigned long long>(e->scan.pass_bytes()),e->scan.pass_unreadable_chunks(),
                double(s_passEnd-s_passStart)/1000.0,e->scan.hits(),e->scan.hits_dropped());
            for(unsigned i=0;i<e->scan.hits()&&n>0&&n<int(sizeof t)-48;++i)
                n+=std::snprintf(t+n,sizeof t-size_t(n),"%s%llx(block+%llX)",i?",":"",static_cast<unsigned long long>(e->scan.hit(i)),
                    static_cast<unsigned long long>(e->scan.hit(i)+acz::e6loc::kWinA-e->scan.hit_block(i)));
            E6Sink sk;sk.line(false,t);
        }
    }
    if(publish) {
        acz::e6loc::CandidateBuilder c=e->regions;e->scan.append_hits(c);
        std::lock_guard<std::mutex> l(e->m);e->pending=c;e->ready=true;
    }
}
// AIRCRAFT-AUTO (aircraft_auto.h, docs/world-render/aircraft-auto.md): with xr_cockpit_model=auto[;...] every validated sample of the LOCKED block
// also publishes the eye offset in the aircraft frame (camera - aircraft, rotated back by the camera rotation) for the consumer's type detector.
// Nothing for the probe-base fallback (no lock) or the frame tick. Key not 'auto': one cached flag test, nothing published.
void aircraft_eye_publish(const acz::e6loc::Sample& s,unsigned lock_epoch) noexcept {
    static std::atomic<int> s_on{-1};   // review 2026-11-01 F7: read by more than one thread (relaxed: every writer stores the same verdict)
    int on=s_on.load(std::memory_order_relaxed);
    if(on<0){auto* l=diagnostics::sink();on=(l&&acz::aircraft::is_auto(l->xr_cockpit_model()))?1:0;s_on.store(on,std::memory_order_relaxed);}
    if(!on)return;
    float local[3];bool centred=false;
    if(acz::aircraft::eye_in_aircraft(s,local,centred))acz::aircraft::eye_mailbox().publish(local,centred,lock_epoch);
}
// STORES-RT: xr_cockpit_model=...;stwatch:1 (or stwatch=1) turns the read-only counter search on (ee_store_watch.h). Cached after the first call.
const acz::e6search::Cfg& store_cfg() noexcept {   // stwatch-start / stctr / stores from the raw option string (parsed once; the model parser refuses bad values)
    static const acz::e6search::Cfg c=[]{auto* l=diagnostics::sink();return acz::e6search::parse_cfg(l?l->xr_cockpit_model():nullptr);}();
    return c;
}
bool store_watch_on() noexcept {
    static std::atomic<int> s_on{-1};   // review 2026-11-01 F7: the worker (e6_background) and the Present tick both call this
    int on=s_on.load(std::memory_order_relaxed);
    if(on<0){
        auto* l=diagnostics::sink();const char* v=l?l->xr_cockpit_model():nullptr;
        on=(v&&(std::strstr(v,";stwatch:1")||std::strstr(v,";stwatch=1")))?1:0;s_on.store(on,std::memory_order_relaxed);
    }
    return on==1;
}
// Present tick, e6loc:auto|content.
void e6_terrain_tick(Probe* p,acz::e6loc::Mode mode) noexcept {
    using namespace acz::e6loc;
    E6Sink sink;
    E6State* e=g_e6.load(std::memory_order_acquire);
    if(!e) {
        e=new(std::nothrow) E6State();
        if(!e)return;
        e->mode=mode;g_e6.store(e,std::memory_order_release);
        char t[448];
        std::snprintf(t,sizeof t,"e6-locate;event=armed;mode=%s;key=xr_terrain_stage(e6loc:auto|probe|content);read-only=1;writes-game-memory=0;look-events-needed=0;"
            "sample-bytes=%u;tick-byte-cap=512;candidate-cap=%u;lock=8-samples,3-moves,100ms;hold-ms=250;lose-ms=1000;fallback=%s",
            mode_name(mode),kSampleBytes,kCandidateCap,mode==acz::e6loc::Mode::automatic?"probe-base-if-sample-valid":"none");
        sink.line(true,t);
    }
    const double ms=double(qpc_us())/1000.0;
    if(e->loc.wants_candidates(ms))e->want.store(true,std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(e->m,std::try_to_lock);
        if(lk.owns_lock()&&e->ready) {e->ready=false;const CandidateBuilder b=e->pending;lk.unlock();e->loc.set_candidates(b,ms,sink);}
    }
    E6ReadMem rm;TickOut o;
    auto& ep=acz::terrain::cam_epochs();
    // CAM-EPOCH review 2: the stage epoch is captured BEFORE the read (another thread may bump it: stage install). A bump during the read leaves this
    // tick's sample stamped with the old stage (refused by admission), never relabelled with the new one.
    const uint32_t stage0=ep.stage();uint32_t stampStage=stage0;
    const int64_t tRead=acz::terrain::cam_clock_ns();   // CAM-EPOCH: the locked sample is read inside tick()
    e->loc.tick(rm,ms,sink,o);
    // CAM-EPOCH (terrain_camera.h, e6-anchor-free.md section 10): the epochs change BEFORE this tick's sample is published, so the sample carries
    // the new epoch and every sample of the previous lock/stage is refused by the consumer. One camera-epoch line per transition. This thread is the
    // only lock writer: the locked base and the lock epoch change as ONE transition (set_source), so the frame tick never pairs them wrongly.
    const uint32_t le=e->loc.lock_epoch();
    {
        const uint64_t lb=e->loc.locked_base();
        if(le!=e->lockEpoch||lb!=e->lockedBase){e->lockedBase=lb;ep.set_source(le,lb);}
        if(le!=e->lockEpoch) {
            e->lockEpoch=le;
            const bool lk=e->loc.state()==acz::e6loc::Locator::locked;
            char t[256];
            std::snprintf(t,sizeof t,"camera-epoch;event=%s;lock-epoch=%u;stage-epoch=%u;base=%llx;locks=%llu;losses=%llu;consumer=refuses-samples-of-other-epochs",
                lk?(e->loc.locks()>1?"relock":"lock"):"loss",le,ep.stage(),static_cast<unsigned long long>(e->loc.locked_base()),
                static_cast<unsigned long long>(e->loc.locks()),static_cast<unsigned long long>(e->loc.losses()));
            sink.line(true,t);
        }
        const uint64_t dc=e->loc.discontinuities();
        if(dc!=e->discont) {
            e->discont=dc;const uint32_t se=ep.bump_stage();
            stampStage=se==stage0+1?se:stage0;   // this tick's (post-jump) sample belongs to the new stage unless another bump interleaved (then: refused)
            char t[256];
            std::snprintf(t,sizeof t,"camera-epoch;event=stage-change;reason=position-discontinuity(locked-jump);lock-epoch=%u;stage-epoch=%u;discontinuities=%llu",
                le,se,static_cast<unsigned long long>(dc));
            sink.line(true,t);
        }
    }
    bool pub=false;
    // AIRCRAFT-AUTO epoch: the lock epoch (locks) and, CAM-EPOCH, the stage epoch: a teleport without a lock loss also restarts the detector window.
    if(o.valid){e6_publish(acz::terrain::cam_mailbox(),o.s,tRead,le,stampStage);pub=true;aircraft_eye_publish(o.s,unsigned(e->loc.locks())^(stampStage<<20));}
    else if(o.hold)pub=true;   // locked, a short invalid stretch: keep the last published sample
    else if(mode==acz::e6loc::Mode::automatic&&e->loc.state()==acz::e6loc::Locator::searching) {
        // E6-RELOCATED: the probe base only after it passed the lock rules (FallbackGate): a static plausible junk structure is never the camera.
        const uint64_t base=p->sh.obs_base.load(std::memory_order_acquire);
        E6ProbeMem pm;pm.m=&p->mem;pm.sh=&p->sh;Sample s;const int64_t tr=acz::terrain::cam_clock_ns();
        if(base&&p->mem.trusted&&read_sample(pm,base,s)) {
            if(!e->gate.admit(base,s,ms)){e6_publish(acz::terrain::cam_mailbox(),s,tr,le,stampStage);pub=true;++e->loc.fallback_published;}
            else ++e->loc.fallback_rejected;
        } else e->gate.reset();
    }
    if(e->loc.state()==acz::e6loc::Locator::locked)e->gate.reset();
    e->searching.store(e->loc.state()==acz::e6loc::Locator::searching,std::memory_order_release);
    {
        const uint64_t qb=mode==acz::e6loc::Mode::automatic?e->gate.qualified_base():0;
        acz::terrain::set_fallback_base(ep,e->fbBase,qb);   // W15 F1: a fallback withdrawal/requalification is an epoch transition (refuses in-flight frame reads)
        if(qb!=e->fbLogged) {
            char t[200];
            std::snprintf(t,sizeof t,"e6-locate;event=fallback-gate;state=%s;base=%llx;rule=8-consistent-samples,3-moves(probe-observation-base)",qb?"qualified":"withdrawn",
                static_cast<unsigned long long>(qb?qb:e->fbLogged));
            sink.line(true,t);e->fbLogged=qb;
        }
    }
    if(!pub){++e->loc.no_camera_ticks;if(e->published)acz::terrain::cam_mailbox().invalidate();}
    e->published=pub;
    if(acz::viewmode::enabled()) {   // VIEW-MODE pov:1 (view_mode.h): the RAW sample of the locked (else last locked) block, also when rejected (chase)
        static uint64_t s_povBase=0;
        if(const uint64_t lb=e->loc.locked_base())s_povBase=lb;
        Sample vs;bool vok=o.valid;
        if(vok)vs=o.s;else vok=s_povBase&&read_sample(rm,s_povBase,vs);
        if(vok)acz::viewmode::view_mailbox().publish(acz::viewmode::geo_of(vs));else acz::viewmode::view_mailbox().invalidate();
    }
    if(store_watch_on()) {   // STORES-RT: counters near the locked E6 structure (window A base = block base + kWinA); 0 while not locked drops the baseline
        if(!e->watch.active())e->watch.start();
        e->watch.tick(rm,e->loc.state()==acz::e6loc::Locator::locked?e->loc.locked_base()+acz::e6loc::kWinA:0,ms,sink);
        e->stBase.store(e->loc.state()==acz::e6loc::Locator::locked?e->loc.locked_base():0,std::memory_order_release);   // the worker's wide search follows the lock
    }
    {   // STORES-RT wiring: the found missile counter (stctr:<block offset>,<width>; none known = unwired) -> acz::stores::signal() with hysteresis (LaunchDetector)
        const acz::e6search::Cfg& sc=store_cfg();
        // review 2026-11-01 F6: exclusive with MSL-HUD (its reader calls the same note_launch): stctr drives the mailbox only with mslhud:0
        if(sc.yields()&&!e->detYieldLogged) {
            e->detYieldLogged=true;char t[128];
            std::snprintf(t,sizeof t,"store-counter;event=unwired;why=msl-hud-is-the-launch-source(set-mslhud:0);block-off=%X;width=%u",sc.ctr_off,sc.ctr_w);
            sink.line(true,t);
        }
        if(sc.drives()) {
            const uint64_t lb=e->loc.state()==acz::e6loc::Locator::locked?e->loc.locked_base():0;
            if(!lb||lb!=e->detBase){e->det.reset();e->detBase=lb;}
            if(lb) {
                uint8_t b[4]={0,0,0,0};uint32_t v=0;const bool ok=rm.read(lb+sc.ctr_off,b,sc.ctr_w);
                if(ok)for(uint32_t i=0;i<sc.ctr_w;++i)v|=uint32_t(b[i])<<(8*i);
                const acz::e6search::LaunchEvent ev=e->det.update(ok,v,ms);
                if(ev.rearm)acz::stores::signal().note_rearm();
                for(unsigned i=0;i<ev.launches;++i)acz::stores::signal().note_launch();
                if((ev.rearm||ev.launches)&&e->detLines<100) {
                    ++e->detLines;char t[160];
                    std::snprintf(t,sizeof t,"store-counter;event=%s;launches=%u;value=%u;block-off=%X;width=%u;t-ms=%.0f",ev.rearm?"rearm":"launch",ev.launches,v,sc.ctr_off,sc.ctr_w,ms);
                    sink.line(true,t);
                }
            }
        }
    }
    e->loc.maybe_summary(ms,sink,mode_name(mode));
}
// Render thread (seam B), e6loc:auto|content: the locked block (else, auto only, the probe base) -> the frame mailbox, single-sample checked.
void e6_frame_tick(Probe* p,acz::e6loc::Mode mode) noexcept {
    using namespace acz::e6loc;
    E6State* e=g_e6.load(std::memory_order_acquire);
    if(!e)return;
    // CAM-EPOCH review 2 (Astra P2): the locked base, its lock epoch and the stage epoch are captured together BEFORE the read; a loss/relock or stage
    // change during the read refuses the sample; the published sample carries the captured identity (acz::terrain::read_publish).
    (void)acz::terrain::read_publish(acz::terrain::frame_cam_mailbox(),acz::terrain::cam_epochs(),[&](uint64_t base,float v[7]) {
        Sample s;E6ReadMem rm;
        if(base){if(!read_sample(rm,base,s))return false;}
        else {
            if(mode!=acz::e6loc::Mode::automatic)return false;
            base=p->sh.obs_base.load(std::memory_order_acquire);   // E6-RELOCATED: only the gate-qualified probe base (FallbackGate)
            if(!base||base!=e->fbBase.load(std::memory_order_acquire)||!in_block(p->sh,base+kWinA,kWinABytes)||!in_block(p->sh,base+kWinB,kWinBBytes)||!read_sample(rm,base,s))return false;
        }
        if(check_sample(s))return false;
        sample_words(s,v);return true;
    });
}
void terrain_tick(Probe* p) noexcept {
    static std::atomic<int> s_on{-1};   // review 2026-11-01 F7: relaxed atomic (every writer stores the same verdict)
    int on=s_on.load(std::memory_order_relaxed);
    if(on<0){auto* l=diagnostics::sink();on=(l&&l->xr_terrain_render())?1:0;s_on.store(on,std::memory_order_relaxed);}
    if(!on)return;
    if(const int fm=terrain_feed_mode();fm>=0&&fm!=int(acz::e6loc::Mode::probe)){e6_terrain_tick(p,acz::e6loc::Mode(fm));return;}
    // CAM-EPOCH review 2: the stage epoch of the read is captured before it (read_publish); the probe feed has no lock source (the base is ignored).
    (void)acz::terrain::read_publish(acz::terrain::cam_mailbox(),acz::terrain::cam_epochs(),[&](uint64_t,float v[7]) {
        const uint64_t base=p->sh.obs_base.load(std::memory_order_acquire);
        if(!base||!p->mem.trusted||!in_block(p->sh,base+0xFDD860,28))return false;
        float f[7]{};
        if(!p->mem.read(base+0xFDD860,f,28))return false;
        float hd=0.0f;
        if(!in_block(p->sh,base+0xFDD448,4)||!p->mem.read(base+0xFDD448,&hd,4))hd=0.0f;
        v[0]=f[0];v[1]=f[1];v[2]=f[2];v[3]=f[4];v[4]=f[5];v[5]=f[6];v[6]=hd;return true;
    });
}
// WORLD-1: the same read-only copy at seam B (the producer's first replayed member, the frame being recorded) into the frame mailbox. Runs on the
// render thread: it does not touch p->mem (owned by the Present tick); every read is VirtualQuery-checked (range_ok) and SEH-guarded.
void terrain_frame_tick(Probe* p) noexcept {
    static std::atomic<int> s_on{-1};   // review 2026-11-01 F7: relaxed atomic (every writer stores the same verdict)
    int on=s_on.load(std::memory_order_relaxed);
    if(on<0){auto* l=diagnostics::sink();on=(l&&l->xr_terrain_render())?1:0;s_on.store(on,std::memory_order_relaxed);}
    if(!on)return;
    if(const int fm=terrain_feed_mode();fm>=0&&fm!=int(acz::e6loc::Mode::probe)){e6_frame_tick(p,acz::e6loc::Mode(fm));return;}
    (void)acz::terrain::read_publish(acz::terrain::frame_cam_mailbox(),acz::terrain::cam_epochs(),[&](uint64_t,float v[7]) {   // CAM-EPOCH review 2
        const uint64_t base=p->sh.obs_base.load(std::memory_order_acquire);
        if(!base||!in_block(p->sh,base+0xFDD860,28)||!range_ok(base+0xFDD860,28))return false;
        float f[7]{};
        if(!guarded_copy(f,reinterpret_cast<const void*>(uintptr_t(base+0xFDD860)),28))return false;
        float hd=0.0f;
        if(!in_block(p->sh,base+0xFDD448,4)||!range_ok(base+0xFDD448,4)||!guarded_copy(&hd,reinterpret_cast<const void*>(uintptr_t(base+0xFDD448)),4))hd=0.0f;
        v[0]=f[0];v[1]=f[1];v[2]=f[2];v[3]=f[4];v[4]=f[5];v[5]=f[6];v[6]=hd;return true;
    });
}

void on_present() noexcept {
    Probe* p=g_probe;
    if(!p||g_finished.load(std::memory_order_acquire))return;
    if(g_tickBusy.test_and_set(std::memory_order_acquire))return;
    perf::Scope cost(perf::ee_present);
    note_thread(seam_A);
    const uint64_t us=qpc_us();const double ms=double(us)/1000.0;
    const uint64_t lo=p->sh.block_lo.load(std::memory_order_acquire);
    p->mem.trusted=lo&&range_ok(lo,block_bytes)?lo:0;
    const auto before=p->exp.phase();
    if(lo&&!p->mem.trusted&&before==Experiment::running)p->exp.abort(p->mem,p->sh,p->sink,"block-no-longer-committed-rw");
    Basis b;const bool hb=p->basis.try_read(b);
    p->exp.tick(p->mem,p->sh,p->scanner,p->req,p->sink,ms,hb?&b:nullptr);
    const auto after=p->exp.phase();
    if(before!=after&&(after==Experiment::observing||after==Experiment::running))emit_threads(after==Experiment::running?"trials":"validated");
    // Observation continues in every phase (read-only); writes only while a
    // trial armed a command AND writes_enabled (never set in production).
    if(p->mem.trusted||!lo) {
        seam_observe(p->sh,seam_A,p->seamMem,qpc_us(),GetCurrentThreadId(),p->basis.published());
        seam_apply(p->sh,seam_A,p->seamMem);
    }
    look_tick(p,ms); // option 3 stage 2a/2b (default off: returns at once)
    trace_tick(p,ms); // D-269 follow-up, read-only look-input trace (default off: p->trace is null)
    lens_tick(p,ms); // lens/FOV phase 0, read-only lens scanner (default off: p->lens is null)
    ps2_tick(p,ms); // PS2 address probe, read-only (default off: p->ps2 is null)
    terrain_tick(p); // J-terrain-render camera feed (read-only; default off)
    g_tickBusy.clear(std::memory_order_release);
}
// ------------------------------------------------------------- UNCAP (xr_terrain_stage uncap:1, default OFF; frame_pacing.h, docs/world-render/uncap.md)
// The game's own Present arguments are forwarded unchanged unless uncap:1 was parsed AND an XR session is running (pacing::uncap_active): then the
// sync interval becomes 0 and, only on a swap chain the GAME created with ALLOW_TEARING (windowed, factory support), DXGI_PRESENT_ALLOW_TEARING is
// added. If the changed call is refused (INVALID_CALL) the game's original call is issued at once and the override is switched off for good.
// Always on with the probe (observe only): per-5-s Present statistics (the sync interval/flags the GAME passes, fps, longest gap) and the
// simulation-rate guard (fixed-step test + game speed) in ee-camera.log as 'pacing;event=present-stats'. No game code is patched; this is the
// existing Present vtable hook.
std::atomic<bool> g_tearingOk{false};     // set once per swap chain creation (describe_swapchain)
std::atomic<bool> g_uncapBroken{false};   // the overridden call was refused once: never override again
std::atomic_flag g_pacingBusy=ATOMIC_FLAG_INIT;
acz::pacing::PresentStats g_pstats;       // game Present thread only (g_pacingBusy guards a second thread)
acz::pacing::SimRateGuard g_simGuard;
acz::pacing::PresentDecision pacing_decide(const void* self,UINT sync,UINT flags) noexcept {
    const bool mine=self==g_swapchain.load(std::memory_order_relaxed)&&!g_uncapBroken.load(std::memory_order_relaxed);
    return acz::pacing::present_override(mine&&acz::pacing::uncap_active(),sync,flags,g_tearingOk.load(std::memory_order_relaxed));
}
void pacing_refused(HRESULT hr) noexcept {
    if(g_uncapBroken.exchange(true))return;
    char t[200];
    std::snprintf(t,sizeof t,"pacing;event=uncap-refused;hr=0x%08lx;action=original-present-reissued,override-off-for-process",static_cast<unsigned long>(hr));
    LogSink s;s.line(true,t);
}
void pacing_tick(UINT gameSync,UINT gameFlags,const acz::pacing::PresentDecision& d) noexcept {
    if(g_pacingBusy.test_and_set(std::memory_order_acquire))return;
    const uint64_t now=qpc_us();
    if(now) {
        g_pstats.note(now,gameSync,gameFlags,d);
        acz::terrain::CamSample cs;
        const bool pv=acz::terrain::cam_mailbox().try_read(cs);
        g_simGuard.feed(now,cs.campos,pv);
        if(g_pstats.due(now)) {
            const auto r=g_simGuard.report(now);
            const uint32_t st=acz::pacing::state_slot().load(std::memory_order_acquire);
            char t[720];
            std::snprintf(t,sizeof t,"pacing;event=present-stats;window-ms=%.0f;presents=%u;fps=%.2f;max-gap-ms=%.1f;game-sync=0:%u,1:%u,2:%u,3+:%u;game-flags-or=0x%x;"
                "uncap-requested=%u;xr-active=%u;uncap-active=%u;overridden=%u;tearing-added=%u;tearing-ok=%u;uncap-broken=%u;"
                "sim=%s;slope=%.2f;short=%u;long=%u;moving=%u;units-per-frame=%.2f;units-per-s=%.1f;game-speed-if-fixed=%.2f;warn=%s",
                double(now-g_pstats.window_start_us)/1000.0,g_pstats.presents,g_pstats.fps(now),double(g_pstats.max_interval_us)/1000.0,
                g_pstats.sync_count[0],g_pstats.sync_count[1],g_pstats.sync_count[2],g_pstats.sync_count[3],g_pstats.flags_or,
                (st&acz::pacing::kUncapRequested)?1u:0u,(st&acz::pacing::kXrActive)?1u:0u,acz::pacing::uncap_active()?1u:0u,g_pstats.overridden,
                g_pstats.tearing_added,g_tearingOk.load()?1u:0u,g_uncapBroken.load()?1u:0u,
                r.model,r.slope,r.short_n,r.long_n,r.moving,r.units_per_frame,r.units_per_s,r.game_speed,
                r.warn?"game-faster-than-60-fixed-step-sim-runs-fast":"none");
            LogSink s;s.line(true,t);
            g_pstats.reset(now);g_simGuard.reset(now);
            acz::brobs::Window bw;double bms=0; // BRIEFING-3D: arm/disarm by the trigger file; an armed window gives two lines
            if(acz::brobs::roll(acz::brobs::arm_file_present(),now,bw,bms)) {
                char b[448];
                acz::brobs::format_window(bw,bms,b,sizeof b);s.line(true,b);
                acz::brobs::format_top(bw,b,sizeof b);s.line(true,b);
                unsigned from=acz::brobs::kTop; // DEBRIEF-3D: the keys after the top 8 (bounded: kRestLinesMax lines per armed window)
                for(unsigned li=0;li<acz::brobs::kRestLinesMax&&from<bw.n;++li){unsigned nx=bw.n;if(acz::brobs::format_rest(bw,from,b,sizeof b,&nx)<=0)break;s.line(true,b);from=nx;}
            }
            acz::b3cap::set_armed(acz::b3cap::arm_file_present()); // BRIEF3D capture: arm/disarm by the trigger file
            {const bool tail=acz::u3c::tail_file_present(); // UI3D census: arm/disarm by the trigger file (a re-arm resets the latch); tail file = TAIL mode
             // POV-RENDER povc:1 (pov_render.h): a new consumer request arms the same read-only ACM census for ~70 s (a fresh latch: disarm first).
             // No request ever made (povc off): req stays false and this is exactly the previous rule.
             static acz::povr::AcmPoll s_povAcm;bool povRearm=false;
             const bool povReq=s_povAcm.poll(acz::povr::acm_request().load(std::memory_order_relaxed),povRearm);
             const bool acmMode=acz::u3c::acm_file_present()||povReq;
             if(povRearm){acz::u3c::set_armed(false);
                 char pb[160];std::snprintf(pb,sizeof pb,"ee-camera;event=pov-census-acm;armed=1;request=%u;arms=%llu;windows=%u;source=povc:1;read-only",
                     unsigned(s_povAcm.last),static_cast<unsigned long long>(s_povAcm.arms),acz::povr::kAcmWindows);s.line(true,pb);}
             acz::u3c::set_armed(acz::u3c::arm_file_present()||tail||acmMode,tail,acmMode);}
            if(acz::brief3d::live::producer().configured()) { // BRIEF3D live: the fit (once per screen) and the producer summary, every window
                char b[700];
                if(acz::brief3d::live::producer().take_fit_line(b,sizeof b))s.line(true,b);
                if(acz::brief3d::live::producer().take_content_line(b,sizeof b))s.line(true,b); // zoom-aware content scale (brief3d.md 9.1)
                if(acz::brief3d::live::producer().table_configured()&&acz::brief3d::live::producer().format_summary(b,sizeof b)>0)s.line(true,b);
                if(acz::brief3d::live::producer().take_latch_line(b,sizeof b))s.line(true,b); // brief3d.md 11 (b3l:1): latched scenes + the geometry reads
                if(acz::brief3d::live::producer().ui3d_configured()){ // UI3D: summary-ui3d (ui-3d.md 5). Every window goes to ee-camera.log (the complete record); the shared diagnostics evidence log (1 MB cap,
              // filled by the PSO census within ~4 minutes: step133 live-1144 stopped at t=154,292) gets only every 6th window (30 s) so it lasts.
                static unsigned u3Windows=0;
                const std::string u=acz::brief3d::live::producer().ui3d_summary();
                if((u3Windows++%6u)==0)s.line(true,u.c_str());else g_eeLog.line(u.c_str(),double(qpc_us())/1000.0);}
                if(acz::brief3d::live::producer().dbr3d_configured()&&acz::brief3d::live::producer().format_dbr3d(b,sizeof b)>0) // DEBRIEF-3D stage 2 (debrief-3d-map.md 8)
                    g_eeLog.line(b,double(qpc_us())/1000.0);
            }
        }
        acz::brobs::present(); // BRIEFING-3D: off = one relaxed load
        {char b[acz::b3cap::kLineLimit],c2[acz::b3cap::kLineLimit],c3[acz::b3cap::kLineLimit];int n2=0,n3=0;if(acz::b3cap::present(now,b,sizeof b,c2,sizeof c2,&n2,c3,sizeof c3,&n3)>0){LogSink s;s.line(true,b);if(n2>0)s.line(true,c2);if(n3>0)s.line(true,c3);}} // BRIEF3D capture: off = one relaxed load
        acz::u3c::present(now,[](const char* t){LogSink s;s.line(true,t);}); // UI3D census: off = one relaxed load
    }
    g_pacingBusy.clear(std::memory_order_release);
}
// BRIEF3D live (brief3d_live.h): the game's present closes the briefing map frame (the slot is published once its lists executed). Off: one load.
inline void brief3d_present() noexcept {
    auto& p=acz::brief3d::live::producer();
    if(p.configured())p.on_present(int64_t(GetTickCount64()));
}
HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* self,UINT sync,UINT flags) {
    const Table* t=find_table(self);
    using Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
    const bool outer=!t_presentDepth;
    const acz::pacing::PresentDecision d=outer?pacing_decide(self,sync,flags):acz::pacing::PresentDecision{sync,flags,false,false};
    if(outer&&!(flags&DXGI_PRESENT_TEST)&&self==g_swapchain.load(std::memory_order_relaxed))acz::vscreen::on_present(self); // VSCREEN (default off: returns at once)
    if(outer&&!(flags&DXGI_PRESENT_TEST)&&self==g_swapchain.load(std::memory_order_relaxed))brief3d_present(); // BRIEF3D live: closes the map frame (off: one load)
    ++t_presentDepth;
    HRESULT hr=t&&t->present?reinterpret_cast<Fn>(t->present)(self,d.sync,d.flags):DXGI_ERROR_INVALID_CALL;
    if(d.changed&&hr==DXGI_ERROR_INVALID_CALL&&t&&t->present){pacing_refused(hr);hr=reinterpret_cast<Fn>(t->present)(self,sync,flags);}
    --t_presentDepth;
    if(!t_presentDepth&&!(flags&DXGI_PRESENT_TEST)&&self==g_swapchain.load(std::memory_order_relaxed)){on_present();pacing_tick(sync,flags,d);}
    return hr;
}
HRESULT STDMETHODCALLTYPE hooked_present1(IDXGISwapChain1* self,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) {
    const Table* t=find_table(self);
    using Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
    const bool outer=!t_presentDepth;
    const acz::pacing::PresentDecision d=outer?pacing_decide(self,sync,flags):acz::pacing::PresentDecision{sync,flags,false,false};
    if(outer&&!(flags&DXGI_PRESENT_TEST)&&static_cast<void*>(self)==g_swapchain.load(std::memory_order_relaxed))acz::vscreen::on_present(self); // VSCREEN
    if(outer&&!(flags&DXGI_PRESENT_TEST)&&static_cast<void*>(self)==g_swapchain.load(std::memory_order_relaxed))brief3d_present(); // BRIEF3D live
    ++t_presentDepth;
    HRESULT hr=t&&t->present1?reinterpret_cast<Fn>(t->present1)(self,d.sync,d.flags,params):DXGI_ERROR_INVALID_CALL;
    if(d.changed&&hr==DXGI_ERROR_INVALID_CALL&&t&&t->present1){pacing_refused(hr);hr=reinterpret_cast<Fn>(t->present1)(self,sync,flags,params);}
    --t_presentDepth;
    if(!t_presentDepth&&!(flags&DXGI_PRESENT_TEST)&&static_cast<void*>(self)==g_swapchain.load(std::memory_order_relaxed)){on_present();pacing_tick(sync,flags,d);}
    return hr;
}
// Once per swap chain creation (not on the Present path): the game's swap-chain parameters, read-only (GetDesc1/GetFullscreenDesc, the factory's
// tearing support, the frame-latency setting of a waitable swap chain). Decides whether ALLOW_TEARING may ever be added (uncap:1).
void describe_swapchain(void* swapchain) noexcept {
    auto* sc=static_cast<IDXGISwapChain*>(swapchain);
    IDXGISwapChain1* sc1=nullptr;
    DXGI_SWAP_CHAIN_DESC1 d{};DXGI_SWAP_CHAIN_FULLSCREEN_DESC fd{};
    bool hd=false,hfd=false;
    if(SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(&sc1)))&&sc1) {
        hd=SUCCEEDED(sc1->GetDesc1(&d));hfd=SUCCEEDED(sc1->GetFullscreenDesc(&fd));
    }
    BOOL tearing=FALSE;
    IDXGIFactory5* f5=nullptr;
    if(SUCCEEDED(sc->GetParent(__uuidof(IDXGIFactory5),reinterpret_cast<void**>(&f5)))&&f5) {
        if(FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof tearing)))tearing=FALSE;
        f5->Release();
    }
    UINT latency=0;bool hl=false;
    if(hd&&(d.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        IDXGISwapChain2* sc2=nullptr;
        if(SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2),reinterpret_cast<void**>(&sc2)))&&sc2){hl=SUCCEEDED(sc2->GetMaximumFrameLatency(&latency));sc2->Release();}
    }
    if(sc1)sc1->Release();
    const bool created_tearing=hd&&(d.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    const bool windowed=hfd?fd.Windowed!=FALSE:true;
    g_tearingOk.store(created_tearing&&tearing&&windowed,std::memory_order_relaxed);
    char t[520];
    std::snprintf(t,sizeof t,"pacing;event=swapchain-desc;desc=%s;size=%ux%u;format=%u;buffers=%u;swap-effect=%u;scaling=%u;alpha=%u;flags=0x%x;"
        "waitable=%u;max-frame-latency=%s%u;allow-tearing-flag=%u;factory-tearing=%u;windowed=%s;refresh=%u/%u;tearing-ok=%u",
        hd?"ok":"unavailable",d.Width,d.Height,unsigned(d.Format),d.BufferCount,unsigned(d.SwapEffect),unsigned(d.Scaling),unsigned(d.AlphaMode),d.Flags,
        (d.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)?1u:0u,hl?"":"n/a:",latency,created_tearing?1u:0u,tearing?1u:0u,
        hfd?(fd.Windowed?"1":"0"):"unknown",hfd?fd.RefreshRate.Numerator:0u,hfd?fd.RefreshRate.Denominator:0u,g_tearingOk.load()?1u:0u);
    LogSink s;s.line(true,t);
}
bool patch_slot(void** vt,int slot,void* replacement,void* expected) noexcept {
    DWORD old=0;
    if(!VirtualProtect(&vt[slot],sizeof(void*),PAGE_READWRITE,&old))return false;
    // Only replace the entry we recorded as the original (no lost foreign patch).
    void* prior=InterlockedCompareExchangePointer(&vt[slot],replacement,expected);
    DWORD ignored=0;VirtualProtect(&vt[slot],sizeof(void*),old,&ignored);
    return prior==expected;
}
// Patches this swapchain's table once (Present always; Present1 when the
// object is an IDXGISwapChain1 sharing the table). The originals are recorded
// and the entry published BEFORE the slot is patched, so a racing Present on
// another thread always finds its original. false: table cap or patch failed.
bool hook_table(IDXGISwapChain* sc,bool& fresh) noexcept {
    fresh=false;
    AcquireSRWLockExclusive(&g_tableLock);
    void** vt=*reinterpret_cast<void***>(sc);bool ok=true;
    const unsigned n=g_tableCount.load(std::memory_order_relaxed);
    bool known=false;for(unsigned i=0;i<n;++i)known|=g_tables[i].vt==vt;
    if(!known) {
        if(n>=table_cap)ok=false;
        else {
            IDXGISwapChain1* sc1=nullptr;
            const bool has1=SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),reinterpret_cast<void**>(&sc1)))&&sc1&&
                            static_cast<void*>(sc1)==static_cast<void*>(sc);
            if(sc1)sc1->Release();
            Table& t=g_tables[n];t.vt=vt;t.present=vt[8];t.present1=has1?vt[22]:nullptr;
            g_tableCount.store(n+1,std::memory_order_release);fresh=true;
            ok=t.present&&patch_slot(vt,8,reinterpret_cast<void*>(&hooked_present),t.present);
            if(ok&&t.present1)ok=patch_slot(vt,22,reinterpret_cast<void*>(&hooked_present1),t.present1);
        }
    }
    ReleaseSRWLockExclusive(&g_tableLock);
    return ok;
}

void seam_first_member(const uint8_t* srt) noexcept {
    Probe* p=g_probe;if(!p)return;
    perf::Scope cost(perf::ee_seam);
    p->basis.publish(srt); // basis of the frame being recorded, BEFORE any B write
    note_thread(seam_B);
    lens_sample_b(p);
    seam_observe(p->sh,seam_B,p->seamMem,qpc_us(),GetCurrentThreadId(),p->basis.published());
    seam_apply(p->sh,seam_B,p->seamMem);
    terrain_frame_tick(p); // WORLD-1: the E6 camera of the frame being recorded (read-only; terrain key only)
}
void seam_execute() noexcept {
    Probe* p=g_probe;if(!p)return;
    note_thread(seam_C);
    seam_observe(p->sh,seam_C,p->seamMem,qpc_us(),GetCurrentThreadId(),p->basis.published());
    seam_apply(p->sh,seam_C,p->seamMem);
}
constexpr SeamHooks g_hooks_value{&seam_first_member,&seam_execute};

bool requirements(diagnostics::Log* log) noexcept {
    return log&&log->feature_logs_enabled()&&log->gpu_copy_early_device_hook()&&log->gpu_copy_step3_experiment()&&
           log->gpu_copy_step3_whole_set()&&log->gpu_copy_step5_continuous();
}
void on_swapchain_created(void* swapchain) noexcept {
    auto* log=diagnostics::sink();
    if(!log||!log->ee_camera_probe()||!swapchain||g_finished.load())return; // default OFF: this line only
    static std::atomic<bool> told{false};
    if(!requirements(log)) {
        if(!told.exchange(true))
            diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,
                "ee-camera;event=refused;reason=requires log_enabled+gpu_copy_early_device_hook+gpu_copy_step3_experiment+gpu_copy_step3_whole_set+gpu_copy_step5_continuous"));
        return;
    }
    if(!g_armed.exchange(true)) {
        // The hooks execute this module's code for the process lifetime.
        HMODULE self=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&on_swapchain_created),&self)) {
            g_finished.store(true);
            diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,"ee-camera;event=refused;reason=module pin failed"));
            return;
        }
        QueryPerformanceFrequency(&g_qpf);
        Options o;o.points=log->ee_camera_probe_points()&7u;if(!o.points)o.points=7;
        o.write_trials=log->ee_camera_write_trials();o.position=o.write_trials&&log->ee_camera_probe_position();
        o.world=nullptr; // no independent world-path observer exists: write trials can never start
        // The probe's own state (seam records, basis mailbox) lives in its own
        // pages so the scans can exclude it.
        LookWriter::Options lw;lw.enabled=log->ee_camera_look_write_trial();
        LookDrive::Options ld;ld.enabled=log->ee_camera_look_drive();
        init_stop_path();
        void* pm=alloc_pages(sizeof(Probe));
        auto* p=pm?new(pm) Probe(o,lw,ld):nullptr;
        if(p) {
            p->req.regionsSrc.ownAlloc=uint64_t(uintptr_t(self));
            p->req.regionsSrc.exclLo[4]=uint64_t(uintptr_t(pm));p->req.regionsSrc.exclHi[4]=p->req.regionsSrc.exclLo[4]+sizeof(Probe);
            open_ee_log();
        }
        if(!p) {
            g_finished.store(true);
            diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,"ee-camera;event=refused;reason=allocation failed"));
            return;
        }
        if(log->ee_camera_look_input_trace()) {
            // Own pages (~4.3 MiB, committed up front) so no allocation ever happens in the render callback, and excluded
            // from the wide/diff scans: its snapshots hold COPIES of the camera structure that must not be seen as candidates.
            void* tm=alloc_pages(sizeof(LookInputTrace));
            if(tm) {
                LookInputTrace::Options to;to.enabled=true;
                p->trace=new(tm) LookInputTrace(to);
                p->req.regionsSrc.exclLo[5]=uint64_t(uintptr_t(tm));p->req.regionsSrc.exclHi[5]=p->req.regionsSrc.exclLo[5]+sizeof(LookInputTrace);
            } else diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,"ee-camera;event=look-input-trace-refused;reason=allocation failed"));
        }
        if(log->ee_lens_scan()) {
            // Own pages (~0.4 MiB, committed up front); excluded from the wide/diff scans (it holds copies of the lens values).
            void* lm=alloc_pages(sizeof(LensScanner));
            if(lm) {
                LensScanner::Options lo;lo.enabled=true;
                p->lens=new(lm) LensScanner(lo);
                p->req.regionsSrc.exclLo[6]=uint64_t(uintptr_t(lm));p->req.regionsSrc.exclHi[6]=p->req.regionsSrc.exclLo[6]+sizeof(LensScanner);
                char lt[448];
                std::snprintf(lt,sizeof lt,"lens-scan;event=armed;key=ee_lens_scan;read-only=1;writes-game-memory=0;scope=tracks-look-block+private-rw-64MiB-blocks;"
                    "caps=regions:%u,bytes:%llu,scan-ms:%.0f,frames:%u,candidates:%u;kill-switch=%%LOCALAPPDATA%%\\acz-vr\\lens-scan.stop;"
                    "file=%%LOCALAPPDATA%%\\acz-vr\\logs\\lens-scan-candidates.txt",lens_region_cap,static_cast<unsigned long long>(lens_byte_cap),lens_scan_cap_ms,lens_frames,lens_cand_cap);
                diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,lt));
            } else diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,"lens-scan;event=refused;reason=allocation failed"));
        }
        g_ps2Out.stem=L"ps2-probe";
        if(log->ee_ps2_probe()) {
            // Own pages (~0.4 MiB, committed up front); excluded from the wide/diff scans (it holds copies of camera values).
            void* pm2=alloc_pages(sizeof(Ps2Prober));
            if(pm2) {
                Ps2Prober::Options po;po.enabled=true;
                p->ps2=new(pm2) Ps2Prober(po);
                p->req.regionsSrc.exclLo[7]=uint64_t(uintptr_t(pm2));p->req.regionsSrc.exclHi[7]=p->req.regionsSrc.exclLo[7]+sizeof(Ps2Prober);
                char pt[640];
                std::snprintf(pt,sizeof pt,"ps2-probe;event=armed;key=ee_ps2_probe;read-only=1;writes-game-memory=0;scope=tracks-look-64MiB-block;"
                    "caps=bytes:%llu,scan-ms:%.0f,cam-hits:%u,gid-hits:%u,aob-hits:%u,fixed-samples:%u,cam-samples:%u,stage-samples:%u,file-bytes:%zu,log-lines:%u;"
                    "kill-switch=%%LOCALAPPDATA%%\\acz-vr\\lens-scan.stop;file=%%LOCALAPPDATA%%\\acz-vr\\logs\\ps2-probe.txt",
                    static_cast<unsigned long long>(pp_byte_cap),pp_scan_cap_ms,pp_cam_cap,pp_gid_cap,pp_aob_cap,pp_fixed_samples,pp_cam_samples,pp_stage_samples,pp_file_cap,pp_log_cap);
                diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,pt));
            } else diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,"ps2-probe;event=refused;reason=allocation failed"));
        }
        p->sh.position_allowed.store(o.position);
        g_probe=p;
        std::atomic_thread_fence(std::memory_order_release);
        p->req.start();
        g_seam_hooks.store(&g_hooks_value,std::memory_order_release);
        g_final_hook.store(&final_on_release,std::memory_order_release);
        std::atexit(&final_on_exit); // CRT termination of this DLL; try-lock writes only
        char t[448];
        std::snprintf(t,sizeof t,"ee-camera;event=armed;mode=%s;points=%s%s%s;position=%u;clock=Present;seams=A:after-native-Present,"
            "B:first-replayed-member,C:ECL-entry;block=0x4000000;scan=relative-geometry-worker;world-path-observer=none;"
            "look-write-trial=%u;look-drive=%u;kill-switch=%%LOCALAPPDATA%%\\acz-vr\\look-write.stop",
            o.write_trials?"observe+write-trials-gated(ordering+world-observer)":"observe-only-read-only",
            o.points&1?"A":"",o.points&2?"B":"",o.points&4?"C":"",unsigned(o.position),unsigned(lw.enabled),unsigned(ld.enabled));
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,t));
    }
    if(!g_probe)return;
    g_swapchain.store(swapchain,std::memory_order_relaxed); // the latest created swapchain is the clock
    bool fresh=false;
    const bool ok=hook_table(static_cast<IDXGISwapChain*>(swapchain),fresh);
    if(fresh||!ok) {
        char t[160];
        std::snprintf(t,sizeof t,"ee-camera;event=present-hook;installed=%u;tables=%u;slots=8,22",unsigned(ok),g_tableCount.load());
        diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,t));
    }
    describe_swapchain(swapchain); // UNCAP: the game's swap-chain parameters (read-only) + whether ALLOW_TEARING may be added
}
// Static initialisation stores one function pointer (no I/O, no allocation).
struct Registration {Registration() noexcept {g_swapchain_hook.store(&on_swapchain_created,std::memory_order_release);}} g_registration;
} // namespace
} // namespace acz::ee_camera
