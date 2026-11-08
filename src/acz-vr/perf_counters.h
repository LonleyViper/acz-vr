#pragma once
// FPS investigation (docs/world-render/fps-investigation.md): cheap in-DLL cost counters for the GAME-side hooks.
// Header-only (inline variables) on purpose: d3d12_hook.cpp is compiled into the DLL and directly into many offline tests.
//
// * Each category accumulates calls, TSC ticks and a (racy, monotone) maximum with relaxed atomics: two __rdtsc() reads
//   (~20-40 cycles each) and three relaxed atomic updates per measured scope. No lock, allocation, I/O or COM.
// * The step-5 producer's 5 s status tick (worker thread) turns the deltas into one compact line,
//   `gpu-copy-step5;event=summary-producer-cost;...` in step5-producer.log (TSC calibrated against QPC over the window).
// * Gauges (registry sizes) are plain relaxed stores at registration/retirement.
// Nothing here changes behaviour: the counters only observe how long the existing hook code takes.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <windows.h>

namespace acz::perf {
enum Cat : unsigned {
    setter=0,       // binding setters' observation (observeBinding: root/heaps/tables/root views/constants/IA/PSO/ClearState/bundle)
    addr_lookup,    // registry address/heap lookups (inside setter/descriptor; the J-130 setter scan and the CBV/root address resolution)
    draw_observe,   // Draw hook: observeDraw + ConstantCall::drawReturned (L3, witnesses, requirements sampler, t0 promotion)
    draw_step5,     // Draw hook: step5Draw + step5HudDraw (+ step-3 set members; the producer's per-draw work incl. its replay recording)
    draw_other,     // Draw hook: hud/phase observers (default off)
    draw_native,    // the game's own Draw forwarded to the driver (reference: what a draw costs without us)
    execute,        // ExecuteCommandLists hook minus the native call (list discovery, L3 links, step-5 submit/signal)
    descriptor,     // descriptor writes/copies (observeNativeBinding: SRV/CBV/UAV/sampler/copy, heap/resource creation, releases)
    ee_present,     // ee_camera Present tick (game thread): probe observation, E6 locator, look/lens/ps2/terrain ticks
    ee_seam,        // ee_camera seam B (render thread, first replayed member): basis publish + E6 frame sample
    log,            // feature-log writes (queue copy) and diagnostics ring pushes
    flush,          // diagnostics flush_after_execute on the game thread (ring drain + file appends/rotations, every 64 submissions/250 ms);
                    // nested inside `execute`. perf_lean moves it to the producer worker (then ~0 here).
    // FPS growth investigation (docs/world-render/fps-investigation.md section 7). All three are NESTED in other categories or run on
    // threads that are not the game's (XR runtime / producer Releases), so none of them is in hooks-us-per-frame.
    release,        // releaseMetadata: every Release (final or not) through a hooked root/PSO/heap/resource table, any thread, incl.
                    // the native Release; nested in `setter` when a root/heap setter drops the old binding
    list_cert,      // hookCaptureCommandList at ExecuteCommandLists (per list per Execute: ~30 QIs + certification); nested in `exec`
    pso_scan,       // a remaining whole-map walk of HookState::pipelines (index rebuild, or an address some record claims); nested
    count
};
inline constexpr const char* cat_name[count]={"setter","addr","draw","step5","draw-x","native","exec","descr","ee-pres","ee-seam","log",
                                              "flush","rel","cert","pscan"};
// Nested / not-game-thread categories: reported, but not summed into hooks-us-per-frame.
inline constexpr bool nested(unsigned c) noexcept {return c==draw_native||c==addr_lookup||c==flush||c==release||c==list_cert||c==pso_scan;}
struct Counter {std::atomic<uint64_t> calls{0},ticks{0},max{0};};
inline Counter g_counters[count];
// pipelines = live HookState::pipelines records (grows with every PSO the game creates); lists = HookState::states (tracked lists).
enum Gauge : unsigned {registry_resources=0,registry_heaps,pipelines,list_states,gauge_count};
inline std::atomic<uint64_t> g_gauges[gauge_count];
inline void gauge(Gauge g,uint64_t v) noexcept {g_gauges[g].store(v,std::memory_order_relaxed);}
inline uint64_t ticks() noexcept {return __rdtsc();}
inline void add(Cat c,uint64_t dt) noexcept {
    auto& x=g_counters[c];
    x.calls.fetch_add(1,std::memory_order_relaxed);x.ticks.fetch_add(dt,std::memory_order_relaxed);
    if(dt>x.max.load(std::memory_order_relaxed))x.max.store(dt,std::memory_order_relaxed);   // racy by design (diagnostic maximum)
}
// Second segment of an already counted call (no call count; the maximum is per segment).
inline void add_time(Cat c,uint64_t dt) noexcept {
    auto& x=g_counters[c];x.ticks.fetch_add(dt,std::memory_order_relaxed);
    if(dt>x.max.load(std::memory_order_relaxed))x.max.store(dt,std::memory_order_relaxed);
}
struct Scope {
    Cat c;uint64_t t0;
    explicit Scope(Cat cat) noexcept:c(cat),t0(__rdtsc()) {}
    ~Scope() {add(c,__rdtsc()-t0);}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
// Window snapshot: deltas since the previous call (single caller: the producer status tick).
struct Window {
    uint64_t calls[count]{},ticks[count]{},max[count]{};uint64_t qpc_us=0,tsc=0;
};
struct Report {
    double us_total[count]{},calls[count]{},max_us[count]{};double window_us=0,tsc_per_us=0;
};
inline uint64_t qpc_us() noexcept {
    LARGE_INTEGER f{},c{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);
    return f.QuadPart?uint64_t(double(c.QuadPart)*1e6/double(f.QuadPart)):0;
}
// Takes the deltas since `last` (updated in place); maxima are reset per window (exchange).
inline Report take(Window& last) noexcept {
    Report r;const uint64_t now_us=qpc_us(),now_tsc=__rdtsc();
    const bool first=last.qpc_us==0;
    const double dus=first?0.0:double(now_us-last.qpc_us),dtsc=first?0.0:double(now_tsc-last.tsc);
    r.window_us=dus;r.tsc_per_us=dus>0?dtsc/dus:0;
    for(unsigned i=0;i<count;++i) {
        const uint64_t c=g_counters[i].calls.load(std::memory_order_relaxed),t=g_counters[i].ticks.load(std::memory_order_relaxed);
        const uint64_t m=g_counters[i].max.exchange(0,std::memory_order_relaxed);
        if(!first&&r.tsc_per_us>0) {
            r.calls[i]=double(c-last.calls[i]);r.us_total[i]=double(t-last.ticks[i])/r.tsc_per_us;r.max_us[i]=double(m)/r.tsc_per_us;
        }
        last.calls[i]=c;last.ticks[i]=t;
    }
    last.qpc_us=now_us;last.tsc=now_tsc;
    return r;
}
// Two compact lines per window (one line would exceed the 448-byte text capacity with large values): per category
// "<us per frame>/<calls per frame>/<max us>"; frames = game frames in the window (the producer's published+skipped delta).
// part 1: window, frames, fps, hooks sum, registry gauges and the Draw-side categories (setter..native);
// part 2: window id and the remaining categories (exec..flush). out must hold >= 448 bytes.
inline void format(char* out,size_t bytes,const Report& r,uint64_t frames,unsigned part=1) noexcept {
    const double f=frames?double(frames):1.0;
    double hooks=0;
    for(unsigned i=0;i<count;++i)if(!nested(i))hooks+=r.us_total[i];   // addr/flush/rel/cert/pscan nested; native = the game's own cost
    int n=part==1?
        std::snprintf(out,bytes,"gpu-copy-step5;event=summary-producer-cost;part=1;window-ms=%.0f;frames=%llu;fps=%.1f;hooks-us-per-frame=%.0f;"
            "registry=res:%llu,heaps:%llu;tsc-mhz=%.0f;u=us/n/maxus",r.window_us/1000.0,static_cast<unsigned long long>(frames),
            r.window_us>0?double(frames)*1e6/r.window_us:0.0,hooks/f,
            static_cast<unsigned long long>(g_gauges[registry_resources].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_gauges[registry_heaps].load(std::memory_order_relaxed)),r.tsc_per_us):
        std::snprintf(out,bytes,"gpu-copy-step5;event=summary-producer-cost;part=2;frames=%llu",static_cast<unsigned long long>(frames));
    const unsigned first=part==1?0u:unsigned(execute),last=part==1?unsigned(execute):unsigned(count);
    for(unsigned i=first;i<last&&n>0&&size_t(n)<bytes;++i)
        n+=std::snprintf(out+n,bytes-size_t(n),";%s=%.0f/%.0f/%.0f",cat_name[i],r.us_total[i]/f,r.calls[i]/f,r.max_us[i]);
    if(part==2&&n>0&&size_t(n)<bytes)
        std::snprintf(out+n,bytes-size_t(n),";objects=pso:%llu,lists:%llu",
            static_cast<unsigned long long>(g_gauges[pipelines].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_gauges[list_states].load(std::memory_order_relaxed)));
}
} // namespace acz::perf
