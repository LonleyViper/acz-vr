// FPS investigation (docs/world-render/fps-investigation.md): offline checks of the lean production profile.
//  1. The indexed registry lookups return EXACTLY what the former whole-map scans returned (randomised differential test against
//     verbatim copies of the old loops, with overlapping/adjacent/huge/removed buffers, empty and overflowing spans, many heaps).
//  2. The old O(N^2) observer lookups (address_resource / cpu_heap: try every tracked token, re-scan the registry for each) equal
//     the new "unique candidate, then the same confirming call" path.
//  3. Timing (informational, printed): per-call cost of the old scan vs the index at N = 1000 / 4000 / 8000 resources.
//  4. perf_lean config key: default off, boolean forms, rejects other values, fits the 32-key cap with the installed 31-key config.
//  5. summary-producer-cost line: fits the shared text capacity (448) and the feature-log line cap (480) with worst-case values.
#include "../../src/acz-vr/buffer_binding_registry.h"
#include "../../src/acz-vr/runtime_config.h"
#include "../../src/acz-vr/perf_counters.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace acz::binding;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1);} } while(0)

// ---- reference implementations: verbatim logic of the pre-index code (buffer_binding_registry.cpp / native_binding_observer.cpp)
struct RefRes {uint64_t id,base,bytes;Token token;};
struct Ref {
    std::map<uint64_t,RefRes> resources;   // same key order as BufferBindingRegistry::resources_ (std::map by id)
    uint64_t observed(uint64_t address,uint64_t bytes) const {
        if(!address||!bytes||bytes>UINT64_MAX-address)return 0;
        if(resources.size()>8192)return 0;
        uint64_t result=0;unsigned matches=0;
        for(const auto& entry:resources) {
            const auto& e=entry.second;
            if(address<e.base+e.bytes&&e.base<address+bytes) {
                ++matches;
                if(address>=e.base&&address-e.base<e.bytes&&bytes<=e.bytes-(address-e.base))result=e.id;
            }
        }
        return matches==1?result:0;
    }
    // buffer_address outcome: "" on success (offset), else the rejection text.
    std::string buffer(Token t,uint64_t address,uint64_t bytes,uint64_t* offset) const {
        auto i=resources.find(t.id);
        if(i==resources.end()||i->second.token!=t)return "unknown/stale resource token";
        if(!(address&&bytes&&bytes<=UINT64_MAX-address))return "null/empty/overflowing GPU address span";
        unsigned matches=0;
        for(const auto& entry:resources) {const auto& e=entry.second;if(address<e.base+e.bytes&&e.base<address+bytes)++matches;}
        if(matches!=1)return matches?"ambiguous GPU address interval":"unknown GPU address";
        const auto& e=i->second;
        if(address<e.base||address-e.base>=e.bytes||bytes>e.bytes-(address-e.base))return "buffer span out of bounds";
        *offset=address-e.base;return "";
    }
};
// Old NativeBindingObserver::address_resource: every tracked token in id order, the first whose buffer_address succeeds.
static Token old_address_resource(const BufferBindingRegistry& r,const std::map<uint64_t,Token>& tokens,uint64_t a,uint64_t b) {
    for(const auto& t:tokens)if(r.buffer_address(t.second,a,b))return t.second;
    return {};
}
// New path (native_binding_observer.cpp): the unique intersecting resource, confirmed by the same call.
static Token new_address_resource(const BufferBindingRegistry& r,const std::map<uint64_t,Token>& tokens,uint64_t a,uint64_t b) {
    const auto id=r.unique_overlap(a,b);if(!id)return {};
    auto t=tokens.find(id);if(t==tokens.end())return {};
    return r.buffer_address(t->second,a,b)?t->second:Token{};
}

static void differential_resources(unsigned seed,unsigned n,bool dense) {
    std::mt19937_64 rng(seed);
    BufferBindingRegistry reg;Ref ref;std::map<uint64_t,Token> tokens;
    uint64_t nextId=0x1000;
    auto rnd=[&](uint64_t lo,uint64_t hi){return std::uniform_int_distribution<uint64_t>(lo,hi)(rng);};
    auto add=[&]{
        const uint64_t id=nextId+=0x40;
        uint64_t base,bytes;
        switch(rnd(0,5)) {
            case 0: base=rnd(1,1ull<<20)*0x10000;bytes=rnd(1,0x10000);break;                    // committed-style, aligned, small
            case 1: base=rnd(1,1ull<<24);bytes=rnd(1,4096);break;                               // dense, overlaps likely
            case 2: base=rnd(1,1ull<<20)*0x10000;bytes=rnd(1,64)<<20;break;                      // large (up to 64 MiB): widens the window
            case 3: base=UINT64_MAX-rnd(1,1ull<<20);bytes=rnd(1,UINT64_MAX-base);break;          // near the top of the address space
            case 4: { if(ref.resources.empty()){base=0x100000;bytes=256;break;}                  // adjacent to / inside an existing one
                      auto it=ref.resources.begin();std::advance(it,rnd(0,ref.resources.size()-1));
                      base=rnd(0,1)?it->second.base+it->second.bytes:it->second.base+rnd(0,it->second.bytes-1);
                      if(base==0)base=1;bytes=rnd(1,8192);if(bytes>UINT64_MAX-base)bytes=UINT64_MAX-base;break; }
            default: base=dense?rnd(1,1ull<<16):rnd(1,1ull<<40);bytes=rnd(1,1ull<<16);break;
        }
        if(!bytes||bytes>UINT64_MAX-base){return;}
        auto r=reg.add_resource({id,base,bytes,1,false});
        CHECK(bool(r));
        ref.resources[id]={id,base,bytes,r.token};tokens[id]=r.token;
    };
    for(unsigned i=0;i<n;++i)add();
    bool full=true;
    auto query=[&] {
        uint64_t a=0,b=0;
        const auto pick=rnd(0,9);
        if(!ref.resources.empty()&&pick<7) {
            auto it=ref.resources.begin();std::advance(it,rnd(0,ref.resources.size()-1));const auto& e=it->second;
            switch(rnd(0,4)) {
                case 0: a=e.base;b=e.bytes;break;                                         // exact
                case 1: a=e.base+rnd(0,e.bytes-1);b=rnd(1,e.bytes);break;                  // inside / straddling the end
                case 2: a=e.base>8?e.base-rnd(1,8):1;b=rnd(1,64);break;                   // straddling the start
                case 3: a=e.base+e.bytes;b=rnd(1,4096);break;                              // just after
                default: a=e.base;b=rnd(0,3)==0?0:416;break;                               // the producer's t10 span (416) / empty
            }
        } else if(pick<9) {a=rnd(0,1ull<<41);b=rnd(0,1ull<<20);}
        else {a=rnd(0,UINT64_MAX);b=rnd(0,UINT64_MAX);}                                   // overflowing spans
        // observed_address
        CHECK(reg.observed_address(a,b)==ref.observed(a,b));
        // buffer_address for a random tracked token and an unknown one
        if(!tokens.empty()) {
            auto it=tokens.begin();std::advance(it,rnd(0,tokens.size()-1));
            uint64_t off=0;const auto want=ref.buffer(it->second,a,b,&off);const auto got=reg.buffer_address(it->second,a,b);
            CHECK(got.rejection==want);if(want.empty())CHECK(got.offset==off&&got.resource==it->second);
        }
        // the observer's address_resource: old O(N^2) loop vs the new unique-candidate path (every query for small sets)
        if(full)CHECK(old_address_resource(reg,tokens,a,b)==new_address_resource(reg,tokens,a,b));
    };
    for(unsigned q=0;q<4000;++q) {
        full=n<=400||q%40==0;
        query();
        if(q%50==0&&!tokens.empty()) { // remove some, add some (index maintenance)
            for(unsigned k=0;k<5&&!tokens.empty();++k) {
                auto it=tokens.begin();std::advance(it,rnd(0,tokens.size()-1));
                CHECK(bool(reg.remove_resource(it->second)));ref.resources.erase(it->first);tokens.erase(it);
            }
            for(unsigned k=0;k<5;++k)add();
        }
    }
    CHECK(reg.resource_count()==ref.resources.size());
}

static void differential_heaps(unsigned seed) {
    std::mt19937_64 rng(seed);auto rnd=[&](uint64_t lo,uint64_t hi){return std::uniform_int_distribution<uint64_t>(lo,hi)(rng);};
    BufferBindingRegistry reg;std::map<uint64_t,Token> tokens;std::map<uint64_t,HeapEvent> events;
    for(unsigned i=0;i<300;++i) {
        HeapEvent e;e.id=0x9000+i*8;e.increment=32;e.count=uint32_t(rnd(1,512));e.type=uint32_t(rnd(0,1));e.flags=uint32_t(rnd(0,1));
        e.cpu_base=rnd(0,3)==0&&!events.empty()?events.begin()->second.cpu_base+32*rnd(0,8):rnd(1,1ull<<22)*32;   // some overlaps
        e.gpu_base=e.flags?rnd(1,1ull<<22)*32:0;
        auto r=reg.add_heap(e);if(!r)continue;tokens[e.id]=r.token;events[e.id]=e;
    }
    // Old NativeBindingObserver::cpu_heap: the first tracked heap whose cpu_slot succeeds.
    auto oldCpu=[&](uint64_t cpu){for(const auto& t:tokens)if(reg.cpu_slot(t.second,cpu))return t.second;return Token{};};
    auto newCpu=[&](uint64_t cpu){const auto id=reg.unique_heap(cpu,false);if(!id)return Token{};auto t=tokens.find(id);
        if(t==tokens.end())return Token{};return reg.cpu_slot(t->second,cpu)?t->second:Token{};};
    for(unsigned q=0;q<20000;++q) {
        uint64_t cpu=0;
        if(rnd(0,9)<8) {auto it=events.begin();std::advance(it,rnd(0,events.size()-1));cpu=it->second.cpu_base+rnd(0,uint64_t(it->second.count)*32+64);}
        else cpu=rnd(0,1ull<<28);
        CHECK(oldCpu(cpu)==newCpu(cpu));
        // gpu: unique_heap(gpu) agrees with gpu_slot's success for the unique candidate
        uint64_t gpu=rnd(0,1ull<<28);
        if(rnd(0,1)){auto it=events.begin();std::advance(it,rnd(0,events.size()-1));if(it->second.flags)gpu=it->second.gpu_base+rnd(0,uint64_t(it->second.count)*32);}
        Token oldGpu{};for(const auto& t:tokens)if(reg.gpu_slot(t.second,gpu)){oldGpu=t.second;break;}
        const auto gid=reg.unique_heap(gpu,true);Token newGpu{};
        if(gid){auto t=tokens.find(gid);if(t!=tokens.end()&&reg.gpu_slot(t->second,gpu))newGpu=t->second;}
        CHECK(oldGpu==newGpu);
    }
}

static void timing() {
    for(unsigned n:{1000u,4000u,8000u}) {
        BufferBindingRegistry reg;Ref ref;std::map<uint64_t,Token> tokens;
        for(unsigned i=0;i<n;++i) {
            const uint64_t id=0x10000+i*0x40,base=0x100000000ull+uint64_t(i)*0x20000,bytes=0x10000;
            auto r=reg.add_resource({id,base,bytes,1,false});CHECK(bool(r));ref.resources[id]={id,base,bytes,r.token};tokens[id]=r.token;
        }
        const unsigned calls=n>=8000?2000:5000;
        auto t0=std::chrono::steady_clock::now();uint64_t sink=0;
        for(unsigned i=0;i<calls;++i)sink+=ref.observed(0x100000000ull+uint64_t(i%n)*0x20000+64,416);
        auto t1=std::chrono::steady_clock::now();
        for(unsigned i=0;i<calls;++i)sink+=reg.observed_address(0x100000000ull+uint64_t(i%n)*0x20000+64,416);
        auto t2=std::chrono::steady_clock::now();
        const unsigned cbv=n>=4000?3:50;   // the old O(N^2) observer path is slow: few calls
        for(unsigned i=0;i<cbv;++i) { // the pre-index observer: every token, each with the pre-index whole-map buffer_address
            const uint64_t a=0x100000000ull+uint64_t((n/2+i*97)%n)*0x20000+256;uint64_t off=0;
            for(const auto& t:tokens)if(ref.buffer(t.second,a,256,&off).empty()){sink+=t.first;break;}
        }
        auto t3=std::chrono::steady_clock::now();
        for(unsigned i=0;i<cbv;++i)sink+=new_address_resource(reg,tokens,0x100000000ull+uint64_t((n/2+i*97)%n)*0x20000+256,256).id;
        auto t4=std::chrono::steady_clock::now();
        auto us=[](auto a,auto b,unsigned k){return std::chrono::duration<double,std::micro>(b-a).count()/k;};
        std::printf("timing n=%u: setter observed_address old %.2f us/call, indexed %.3f us/call; observer address_resource (CBV/root view) old %.1f us/call, new %.3f us/call (sink %llu)\n",
            n,us(t0,t1,calls),us(t1,t2,calls),us(t2,t3,cbv),us(t3,t4,cbv),static_cast<unsigned long long>(sink&1));
        CHECK(us(t1,t2,calls)<=us(t0,t1,calls)+0.5);   // soft: the index is never meaningfully slower
    }
}

static void config() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.perf_lean()&&!parse_config("").config.perf_lean());
    for(const char* v:{"true","1"}){auto c=parse_config(std::string("perf_lean=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.perf_lean());}
    for(const char* v:{"false","0"}){auto c=parse_config(std::string("perf_lean=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.perf_lean());}
    CHECK(parse_config("perf_lean=yes").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("perf_lean=1\nperf_lean=0").issue==ConfigIssue::duplicate);
    // The installed 31-key morning config (step78, headset test 17) + perf_lean = 32 keys: the cap, still accepted, nothing else changes.
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=10",
        "xr_hud_scale=1.0","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=65","xr_world_in_projection=true","gpu_copy_step5_world_objects=true",
        "gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true","gpu_copy_step5_effects=true","xr_effects_layer=true","gpu_copy_step5_effects_dual=true",
        "xr_world_hold_last=true","xr_world_objects_depth_fix=true","xr_world_window_occlude=true","xr_terrain_render=true",
        "xr_terrain_stage=auto;env:1;look:1;props:1;trees:1;propdedupe:1","ee_camera_probe=true","xr_cockpit_model=auto;anim:1"};
    static_assert(std::size(keys)==31);
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\r\n";
    const auto base=parse_config(cfg);CHECK(base.issue==ConfigIssue::none&&!base.config.perf_lean());
    const auto lean=parse_config(cfg+"perf_lean=true\r\n");
    CHECK(lean.issue==ConfigIssue::none&&lean.config.perf_lean()&&(cfg.size()+17)<=max_config_bytes);
    // Every other value is identical with and without perf_lean (it gates nothing else).
    const auto& a=base.config;const auto& b=lean.config;
    CHECK(a.logging()==b.logging()&&a.ownership_pins()==b.ownership_pins()&&a.gpu_copy_witness_observation()==b.gpu_copy_witness_observation()&&
          a.gpu_copy_early_device_hook()==b.gpu_copy_early_device_hook()&&a.gpu_copy_step3_experiment()==b.gpu_copy_step3_experiment()&&
          a.gpu_copy_step3_whole_set()==b.gpu_copy_step3_whole_set()&&a.gpu_copy_step5_continuous()==b.gpu_copy_step5_continuous()&&
          a.xr_stereo_output()==b.xr_stereo_output()&&a.ee_camera_probe()==b.ee_camera_probe()&&a.xr_terrain_render()==b.xr_terrain_render()&&
          std::strcmp(a.xr_terrain_stage(),b.xr_terrain_stage())==0&&std::strcmp(a.xr_cockpit_model(),b.xr_cockpit_model())==0&&
          a.xr_world_hold_last()==b.xr_world_hold_last()&&a.gpu_copy_step5_effects_dual()==b.gpu_copy_step5_effects_dual()&&a.verbosity()==b.verbosity());
    CHECK(parse_config(cfg+"perf_lean=true\r\nxr_png_once=true\r\n").issue==ConfigIssue::oversized);   // 33 keys: refused (the script warns)
}

static void cost_line() {
    using namespace acz::perf;
    Report r;r.window_us=5000000;r.tsc_per_us=3000;
    for(unsigned i=0;i<count;++i){r.us_total[i]=9.99e6;r.calls[i]=9.99e6;r.max_us[i]=9.99e6;}   // 7-digit fields everywhere (10 s per frame)
    gauge(registry_resources,999999);gauge(registry_heaps,99999);
    char t[448];
    for(unsigned part=1;part<=2;++part) {
        format(t,sizeof t,r,1,part);
        std::printf("worst-case cost line part %u: %zu chars\n",part,std::strlen(t));
        CHECK(std::strlen(t)<440);   // not truncated at the shared text capacity (448) nor the feature-log line cap (480)
    }
    Report z;z.window_us=5000000;z.tsc_per_us=3000;for(unsigned i=0;i<count;++i){z.us_total[i]=300*1000.0*(i+1);z.calls[i]=300*100.0;z.max_us[i]=12;}
    format(t,sizeof t,z,300,1);
    std::printf("typical: %s\n",t);
    CHECK(std::strstr(t,"event=summary-producer-cost;part=1;window-ms=5000;frames=300;fps=60.0;hooks-us-per-frame=58000;registry=res:999999,heaps:99999;")&&
          std::strstr(t,";setter=1000/100/12;")&&std::strstr(t,";native=6000/100/12")&&!std::strstr(t,"exec="));
    format(t,sizeof t,z,300,2);
    std::printf("typical: %s\n",t);
    CHECK(std::strstr(t,"event=summary-producer-cost;part=2;frames=300;exec=7000/100/12;")&&std::strstr(t,";flush=12000/100/12")&&!std::strstr(t,"setter="));
    // Scope/take: counts calls and time; the window resets the maxima.
    Window w;(void)take(w);
    {Scope s(setter);volatile int x=0;for(int i=0;i<1000;++i)x=x+i;}
    Sleep(2);
    const auto rep=take(w);
    CHECK(rep.calls[setter]==1&&rep.us_total[setter]>0&&rep.window_us>1000&&rep.tsc_per_us>100);
    const auto rep2=take(w);CHECK(rep2.calls[setter]==0&&rep2.max_us[setter]==0);
}

int main() {
    for(unsigned s=1;s<=6;++s)differential_resources(s,s<=3?400:2500,s%2==0);
    differential_heaps(11);differential_heaps(12);
    timing();
    config();
    cost_line();
    std::printf("fps_lean_offline: %u checks passed\n",checks);
    return 0;
}
