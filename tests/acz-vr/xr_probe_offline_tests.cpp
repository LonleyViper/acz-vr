#include "probe_core.h"
#include "compatibility.h"
#include "format_policy.h"
#include "supervision.h"
#include <atomic>
#include <thread>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <limits>
#include <stdexcept>
using namespace acz::probe;
namespace {
unsigned checks=0;
void check(bool ok,const char* what) { ++checks;if(!ok){std::cerr<<"FAIL "<<what<<'\n';std::exit(1);} }
struct Time final:Clock {
    std::atomic<int64_t> ms{0};
    int64_t now()override{return ms;}
    void sleep(int64_t n)override {check(n>=0 && n<=Caps::poll_ms,"bounded sleep");ms+=n;}
};
// Fake XR and D3D12 operations, no platform includes/loader/device calls.
struct Fake final:Backend {
    Time& time;
    Op failure=Op::count;
    Op partial_failure=Op::count;
    Op qualified_loss=Op::count;
    unsigned fail_at=1;
    std::array<unsigned,static_cast<unsigned>(Op::count)> counts{};
    std::vector<Op> calls;
    bool ready=true,stopping=true,exiting=false;
    bool instance=false,device=false,queue=false,session=false,space=false,running=false;
    bool exit_requested=false;
    int64_t frame_ms=1, running_poll_ms=0;
    std::vector<std::string> extensions{"XR_KHR_D3D12_enable"};
    std::vector<int> configurations{2},blends{1};
    std::vector<int64_t> formats{29,91,2,10,24,40,55,45,20}; // observed SteamVR list, no 28
    Log log=[](const std::string&){};
    int format_count_delta=0;
    unsigned format_calls=0, format_failure_call=0;
    uint32_t format_count_override=0;
    // The phase-1 Backend interface has no create/enumerate-images/acquire/
    // wait/release/destroy swapchain operations. Format enumeration is separate.
    unsigned swapchain_functions_called=0;
    std::vector<ViewLimits> views{{100,100,1,200,200,4},{100,100,1,200,200,4}};
    GraphicsFacts graphics{7,7,0,0,0xb000,0xb000,true,true,true,true,true};
    bool has_system=true, requirements_valid=true, api_compatible=true, local_space=true;
    explicit Fake(Time& t):time(t){}
    Reply call(Op op, unsigned layer_count, const void* layers)override {
        if(op==Op::end_frame)check(layer_count==0&&layers==nullptr,"every frame submits zero layers");
        calls.push_back(op);auto index=static_cast<unsigned>(op);
        if(++counts[index]==fail_at && op==failure)return {false,State::idle};
        switch(op) {
        case Op::instance:check(!instance,"single instance");if(!api_compatible||!d3d12_extension(extensions))return {false};instance=true;break;
        case Op::system:check(instance,"system after instance");if(!has_system)return {false};break;
        case Op::views:if(!stereo_config(configurations)||!stereo_views(views)||!opaque_blend(blends))return {false};break;
        case Op::formats:
            if(!observe_phase1_formats([&](uint32_t capacity,uint32_t* count,int64_t* data){
                if(++format_calls==format_failure_call)return false;
                if(!capacity){*count=format_count_override?format_count_override:static_cast<uint32_t>(formats.size());return true;}
                *count=static_cast<uint32_t>(static_cast<int>(formats.size())+format_count_delta);
                std::copy_n(formats.begin(),std::min<size_t>(capacity,formats.size()),data);
                return true;
            },log))return {false};
            break;
        case Op::spaces:if(!local_space)return {false};break;
        case Op::requirements:check(instance,"requirements after instance");if(!requirements_valid)return {false};break;
        case Op::device:if(!compatible_graphics(graphics))return {false};device=queue=true;break;
        case Op::session:check(device&&queue,"probe device before session");session=true;break;
        case Op::create_space:check(session,"space after session");space=true;break;
        case Op::poll:
            if(running && !exit_requested && n(Op::wait_frame))time.ms+=running_poll_ms;
            if(exiting)return {true,State::loss};
            if(exit_requested)return {true,stopping?State::stopping:State::running};
            return {true,ready?State::ready:State::idle};
        case Op::begin_session:check(session&&ready,"READY before begin");running=true;break;
        case Op::wait_frame:check(running,"wait in session");time.ms+=frame_ms;break;
        case Op::begin_frame:check(running,"begin in session");break;
        case Op::end_frame:check(running,"end in session; zero-layer backend contract");break;
        case Op::request_exit:check(running,"exit only running");exit_requested=true;break;
        case Op::end_session:check(exit_requested&&stopping,"STOPPING before end");running=false;break;
        case Op::destroy_space:space=false;break;
        case Op::destroy_session:check(!running&&!space,"space/end before destroy session");session=false;break;
        case Op::release_queue:check(!session,"session before queue release");queue=false;break;
        case Op::release_device:check(!queue&&!session,"queue before device release");device=false;break;
        case Op::destroy_instance:check(!session&&!device,"device/session before instance destroy");instance=false;break;
        default:break;
        }
        return {op!=partial_failure,op==qualified_loss?State::loss:State::idle};
    }
    unsigned n(Op op)const{return counts[static_cast<unsigned>(op)];}
};
struct Fixture {
    Time time;Fake fake{time};std::vector<std::string> log;
    std::string stop;
    unsigned stop_after=0;
    Op stop_stage=Op::end_frame;
    Run run{fake,time,[&](const std::string& s){log.push_back(s);},[&]{return fake.n(stop_stage)>=stop_after?stop:std::string{};}};
    Fixture(){fake.log=[&](const std::string& s){log.push_back(s);};}
    bool contains(const std::string& text) {return std::any_of(log.begin(),log.end(),[&](const std::string& s){return s.find(text)!=std::string::npos;});}
};
}
int main() {
    static_assert(Caps::attempts==1 && Caps::absolute_ms==15000 && Caps::startup_ms==5000 && Caps::cleanup_ms==2000);
    static_assert(Caps::frames==120 && Caps::progress_ms==250 && Caps::poll_ms==20 && Caps::admission_ms==13000);
    // Pure later-colour policy; these calls are deliberately outside Run.
    check(select_later_colour_format({29})==29,"only sRGB accepted for later colour");
    check(select_later_colour_format({29,91,2,10,24,40,55,45,20})==29,"SteamVR later-colour sRGB preference");
    check(select_later_colour_format({28})==28,"UNORM fallback for later colour");
    check(select_later_colour_format({28,29})==29 && select_later_colour_format({29,28})==29,"sRGB wins regardless of order");
    check(!select_later_colour_format({91,2,10,24,40,55,45,20}),"no BGRA/float/depth substitution");
    check(!select_later_colour_format({}),"empty later-colour list refused");
    check(select_later_colour_format({28,28,29,29})==29 && select_later_colour_format({28,28})==28 &&
        !select_later_colour_format({91,91}),"duplicates preserve later-colour policy");
    for(const auto& formats:std::vector<std::vector<int64_t>>{{29},{29,91,2,10,24,40,55,45,20},{28},{28,29},{91,10},{},{28,28,29,29}}) {
        Fixture f;f.fake.formats=formats;auto r=f.run.execute();
        check(r.success && r.clean && r.ended==120,"phase 1 succeeds regardless of format availability");
        const bool unorm=std::find(formats.begin(),formats.end(),28)!=formats.end();
        const bool srgb=std::find(formats.begin(),formats.end(),29)!=formats.end();
        check(f.contains(std::string("format_observation rgba8_unorm_28=")+(unorm?"1":"0")+
            " rgba8_srgb_29="+(srgb?"1":"0")+" informational_only=1"),"phase 1 logs availability observation");
        const auto logged=std::count_if(f.log.begin(),f.log.end(),[](const std::string& line){return line.find("swapchain_format=")==0;});
        check(static_cast<size_t>(logged)==formats.size(),"every enumerated format logged including duplicates");
        for(auto format:formats)check(f.contains("swapchain_format="+std::to_string(format)),"format value logged");
        check(!f.contains("selected_format="),"phase 1 never selects a format");
        check(f.fake.swapchain_functions_called==0,"phase 1 invokes zero swapchain functions");
        check(f.fake.format_calls==(formats.empty()?1u:2u),"bounded enumeration without retries");
    }
    for(int scenario=0;scenario<5;++scenario) {
        Fixture f;
        if(scenario<2)f.fake.format_count_delta=scenario==0?-1:1;
        if(scenario==2)f.fake.format_count_override=Caps::enumeration_limit+1;
        if(scenario>=3)f.fake.format_failure_call=scenario==3?1:2;
        auto r=f.run.execute();
        check(!r.success && r.clean && r.reason=="backend failure: formats" && r.begun==0,
            "changed count, nonsense count or failed enumeration refuses before frames");
        check(f.fake.format_calls<=2 && f.fake.swapchain_functions_called==0,"enumeration failure never retries or uses swapchains");
        check(!f.contains("format_observation"),"failed enumeration is not an availability observation");
    }
    // Named fake-runtime/graphics refusal fixtures exercise the same selection
    // policy the real enumerated facts use, then verify lifecycle refuses.
    for(int scenario=0;scenario<17;++scenario) {
        Fixture f;
        switch(scenario) {
        case 0:f.fake.extensions.clear();break; // missing D3D12 extension
        case 1:f.fake.has_system=false;break;
        case 2:f.fake.configurations={1};break; // no stereo
        case 3:f.fake.requirements_valid=false;break;
        case 4:f.fake.graphics.selected=0xa000;break; // insufficient feature level
        case 5:f.fake.graphics.adapter_found=false;break;
        case 6:f.fake.graphics.device_created=false;break;
        case 7:f.fake.graphics.queue_created=false;break;
        case 8:f.fake.format_count_override=Caps::enumeration_limit+1;break; // invalid enumeration count
        case 9:f.fake.blends={2};break; // no usable opaque blend
        case 10:f.fake.graphics.selected_low=8;break; // wrong LUID
        case 11:f.fake.graphics.direct=false;break;
        case 12:f.fake.graphics.same_device=false;break;
        case 13:f.fake.views[0].width=0;break;
        case 14:f.fake.views[1].samples=5;break;
        case 15:f.fake.api_compatible=false;break;
        case 16:f.fake.local_space=false;break;
        }
        auto r=f.run.execute();
        check(!r.success&&r.clean&&f.fake.n(Op::wait_frame)==0,"enumeration/device refusal before frames");
        check(f.fake.n(Op::instance)==1,"refusal single attempt");
    }
    {
        Fixture f;auto r=f.run.execute();
        check(r.success&&r.clean&&r.reason=="frame cap","frame cap orderly success");
        check(r.waited==120&&r.begun==120&&r.ended==120,"120 balanced zero-layer frames");
        check(f.contains("format_observation rgba8_unorm_28=0 rgba8_srgb_29=1 informational_only=1"),"SteamVR phase-1 fixture observes missing UNORM without refusal");
        check(f.fake.swapchain_functions_called==0,"default phase-1 fixture calls no swapchain functions");
        check(f.contains("card=a")&&f.contains("phase=1")&&f.contains("xrWaitFrame=non-cancellable")&&
            f.contains("balance wait=120 begin=120 end=120")&&f.contains("clean-retirement"),"core manifest");
        std::vector<Op> expected{Op::instance,Op::system,Op::views,Op::requirements,Op::device,Op::session,Op::formats,Op::spaces,Op::create_space,Op::poll,Op::begin_session};
        check(std::equal(expected.begin(),expected.end(),f.fake.calls.begin()),"startup ordering");
        std::vector<Op> tail{Op::request_exit,Op::poll,Op::end_session,Op::destroy_space,Op::destroy_session,Op::release_queue,Op::release_device,Op::destroy_instance};
        check(std::equal(tail.begin(),tail.end(),f.fake.calls.end()-tail.size()),"reverse retirement order");
        auto count=f.fake.calls.size();check(f.run.cleanup()&&f.fake.calls.size()==count,"idempotent cleanup");
        auto retry=f.run.execute();check(!retry.success&&f.fake.calls.size()==count,"single attempt no retry");
    }
    for(auto stage:{Op::instance,Op::system,Op::views,Op::requirements,Op::device,Op::session,Op::formats,Op::spaces,Op::create_space,Op::begin_session}) {
        Fixture f;f.fake.failure=stage;auto r=f.run.execute();
        check(!r.success&&r.clean,"partial init failure clean");
        check(!f.fake.instance&&!f.fake.device&&!f.fake.queue&&!f.fake.session&&!f.fake.space,"only existing objects retired");
        check(f.fake.n(stage)==1,"no init retry");
        check(f.fake.n(Op::wait_frame)==0,"no frames on refusal");
    }
    for(auto stage:{Op::wait_frame,Op::begin_frame,Op::end_frame}) {
        Fixture f;f.fake.failure=stage;f.fake.fail_at=3;auto r=f.run.execute();
        check(!r.success&&r.clean,"runtime mid-frame failure cleanup");
        check(f.fake.n(stage)==3&&f.fake.n(Op::request_exit)==1&&f.fake.n(Op::end_session)==1,"runtime failure no retry orderly teardown");
        check(r.reason==std::string("backend failure: ")+name(stage),"runtime failure reason");
    }
    for(auto stage:{Op::instance,Op::device,Op::session,Op::create_space}) {
        Fixture f;f.fake.partial_failure=stage;auto r=f.run.execute();
        check(!r.success&&r.clean,"created handle followed by stage failure cleans up");
        check(!f.fake.instance&&!f.fake.device&&!f.fake.queue&&!f.fake.session&&!f.fake.space,"partial stage owners retired");
    }
    {
        Fixture f;f.fake.frame_ms=15001;auto r=f.run.execute();
        check(!r.success&&!r.clean&&f.fake.n(Op::begin_frame)==0,"late wait cannot begin beyond absolute cap");
        check(f.contains("FAILURE"),"late wait result recorded failure");
    }
    {
        Fixture f;f.fake.frame_ms=200;auto r=f.run.execute();
        check(r.success&&r.clean&&r.reason=="time cap"&&r.begun<120,"time cap reserves cleanup inside cap");
        check(f.time.now()<=Caps::absolute_ms,"absolute deadline");
    }
    {
        Fixture f;f.fake.running_poll_ms=12999;auto r=f.run.execute();
        check(r.success && r.clean && r.waited==1 && r.reason=="time cap",
            "poll crossing 13 s cannot admit another wait/frame");
    }
    for(const auto* source:{"Escape","stop file"}) {
        for(auto stage:{Op::instance,Op::poll,Op::begin_session}) {
            Fixture f;f.stop=source;f.stop_stage=stage;
            f.stop_after=stage==Op::instance?0:1;
            // Stop immediately, while awaiting READY, or after session begin
            // before the first frame. All retire cleanly without success.
            if(stage==Op::poll)f.fake.ready=false;
            auto r=f.run.execute();
            check(!r.success&&r.clean&&r.begun==0,"pre-lifecycle stop is not success");
            check(f.contains("ABORTED operator stop before lifecycle")&&!f.contains("SUCCESS "),"pre-lifecycle abort evidence");
            check(f.fake.n(Op::wait_frame)==0,"pre-lifecycle stop never waits frame");
            check(f.fake.n(Op::begin_session)==(stage==Op::begin_session?1u:0u),"session begin evidence");
            check(!f.fake.instance&&!f.fake.session&&!f.fake.device,"aborted owners retired");
        }
        {
            Fixture f;f.stop=source;f.stop_after=1;auto r=f.run.execute();
            check(r.success&&r.clean&&r.waited==1&&r.begun==1&&r.ended==1,"one balanced frame qualifies for success");
        }
        Fixture f;f.stop=source;f.stop_after=2;auto r=f.run.execute();
        check(r.success&&r.clean&&r.reason==source&&r.ended==2,"stop source orderly cleanup");
    }
    check(activation_refusal(StopFileStatus::present)=="stop file already exists at launch","stale stop file refuses activation");
    check(!activation_refusal(StopFileStatus::unavailable).empty(),"unknown stop file status refuses activation");
    check(activation_refusal(StopFileStatus::absent).empty(),"absent stop file permits activation");
    {
        Fixture f;f.fake.ready=false;auto r=f.run.execute();
        check(!r.success&&r.clean&&r.reason=="startup timeout"&&f.time.now()==5000,"event never READY timeout");
        check(f.fake.n(Op::begin_session)==0,"no illegal begin after timeout");
    }
    {
        Fixture f;f.fake.stopping=false;auto r=f.run.execute();
        check(!r.success&&!r.clean&&f.contains("FAILURE running session retained"),"STOPPING timeout FAILURE");
        check(!f.fake.n(Op::destroy_session)&&!f.fake.n(Op::release_device),"retain live owners on timeout");
    }
    for(auto stage:{Op::end_session,Op::destroy_space,Op::destroy_session,Op::release_queue,Op::release_device,Op::destroy_instance}) {
        Fixture f;f.fake.failure=stage;auto r=f.run.execute();
        check(!r.success&&!r.clean,"failed retirement never clean");
        auto count=f.fake.calls.size();check(!f.run.cleanup()&&count==f.fake.calls.size(),"failed cleanup idempotent no retry");
    }
    for(auto stage:{Op::instance,Op::session,Op::create_space,Op::begin_session,Op::wait_frame,Op::begin_frame,
        Op::end_frame,Op::request_exit,Op::end_session,Op::destroy_space,Op::destroy_session,Op::destroy_instance}) {
        Fixture f;f.fake.qualified_loss=stage;auto r=f.run.execute();
        check(!r.success && r.clean,"qualified success retains ownership but vetoes success");
        check(f.contains("failure_observation=") && !f.contains("SUCCESS "),"sticky loss evidence");
        check(!f.fake.instance&&!f.fake.device&&!f.fake.queue&&!f.fake.session&&!f.fake.space&&!f.fake.running,
            "qualified-success handles legally retired");
        if(stage==Op::end_session) {
            check(r.begun==120 && r.ended==120 && r.reason=="frame cap",
                "EndSession loss after balanced frame-cap exit is still failure");
            check(f.fake.n(Op::destroy_instance)==1,"cleanup continues after EndSession qualified loss");
        }
        if(stage==Op::request_exit) {
            check(r.reason=="frame cap" && f.fake.n(Op::end_session)==1,
                "later STOPPING poll clears mutable loss while orderly reason remains; sticky veto required");
        }
        if(stage==Op::wait_frame) {
            check(r.begun==1 && r.ended==1 && f.fake.n(Op::poll)>=3,
                "later STOPPING poll clears protocol loss but cannot clear sticky observation");
        }
    }
    check(frame_admitted(12999) && !frame_admitted(13000),"shared native/core frame admission boundary");
    for(bool after_entry:{false,true}) {
        Time time;Fake fake(time);unsigned waits=0;
        struct Boundary final:Backend {
            Fake& fake;Time& time;unsigned& waits;
            Boundary(Fake& f,Time& t,unsigned& w):fake(f),time(t),waits(w){}
            Reply call(Op op,unsigned layers,const void* data)override {
                if(op==Op::wait_frame && ++waits==2) {
                    // Scheduling advances after the core entry check. The real
                    // backend reports exactly this typed admission disposition.
                    time.ms=13000;return {false,State::idle,!frame_admitted(time.now())};
                }
                return fake.call(op,layers,data);
            }
        } backend(fake,time,waits);
        Run run(backend,time,[&](const std::string& line){
            if(after_entry && fake.n(Op::wait_frame)==1 && line.find("core entry=wait_frame")!=std::string::npos)time.ms=13000;
        },[]{return std::string{};});
        auto result=run.execute();
        check(result.success && result.clean && result.reason=="time cap" && result.waited==1,
            "core entry/native admission races map to orderly time cap");
    }
    // Shared supervisor phase policies: startup total 5 s; steady-state progress
    // 250 ms; cleanup total 2 s. Continued phase progress never extends totals.
    for(auto phase:{Phase::startup,Phase::frames,Phase::cleanup}) {
        Time time;Signals signals;int64_t killed=-1;
        observe_phase(signals,phase,0);
        const int64_t limit=phase==Phase::startup?5000:phase==Phase::frames?250:2000;
        bool survived_before=false;
        supervise(time,signals,[&]{if(time.now()==limit-1)survived_before=true;return false;},[&](int64_t n){
            time.ms=time.now()==limit-1?limit:std::min(time.now()+n,limit-1);
            signals.file_progress=time.now();
        },[&]{killed=time.now();});
        check(survived_before && killed==limit,"phase survives deadline minus one and kills at deadline");
    }
    {
        Time time;Signals signals;int64_t killed=-1;
        time.ms=12999;signals.file_progress=time.now();observe_phase(signals,Phase::cleanup,time.now());
        check(signals.worker_deadline.load()==14999,"cleanup deadline fixed at entry inside outer cap");
        supervise(time,signals,[]{return false;},[&](int64_t n){
            time.sleep(n);++signals.progress;signals.file_progress=time.now();
        },[&]{killed=time.now();});
        check(killed==14999,"cleanup progress cannot renew its total deadline");
        Signals late;time.ms=14000;observe_phase(late,Phase::cleanup,time.now());
        check(late.worker_deadline.load()==15000,"late cleanup clipped to outer cap");
    }
    // Production logging volume through REAL Run, Mailbox and file_loop. The
    // fake native seam emits the real native entry/exit plus frame metadata.
    // Model 90 Hz as 11/12 ms intervals and charge 1 ms for EVERY file append.
    // Each append has a virtual completion time; it cannot finish ahead of the
    // frame clock. After the worker finishes, only the logger advances time.
    // Scheduling cannot change frame cadence, line count or reserved capacity.
    {
        Time time;Fake fake(time);Signals signals;Mailbox box;
        unsigned produced=0,written=0;int64_t write_cost=0;
        auto record=[&](const std::string& line){++produced;box.push(line);};
        struct Cadence final:Backend {
            Fake& fake;Log record;unsigned frames=0;
            Cadence(Fake& f,Log l):fake(f),record(l){}
            Reply call(Op op,unsigned layers,const void* data)override {
                record(std::string("call entry=")+name(op));
                if(op==Op::wait_frame){++frames;fake.frame_ms=(frames*1000/90)-((frames-1)*1000/90);}
                auto reply=fake.call(op,layers,data);
                record(std::string("call exit=")+name(op));
                if(op==Op::wait_frame)record("predicted_time=1 period=11111111 shouldRender=1");
                if(op==Op::end_frame)record("frame_submission layerCount=0 layers=nullptr");
                return reply;
            }
        } backend(fake,record);
        std::thread file([&]{file_loop(time,signals,box,[]{return StopFileStatus::absent;},
            [&](const std::string&){
                ++written;++write_cost;
                while(time.now()<write_cost && !signals.worker_done.load())std::this_thread::yield();
                // Retirement already observed by Run before worker_done: draining
                // can advance virtual time, never retroactively delay XR calls.
                if(signals.worker_done.load()) {
                    auto now=time.now();
                    while(now<write_cost && !time.ms.compare_exchange_weak(now,write_cost)){}
                }
                return true;
            },
            [](int64_t){std::this_thread::yield();});});
        Run run(backend,time,record,[]{return std::string{};});
        auto result=run.execute();signals.worker_done=true;file.join();
        check(result.success && result.clean && result.reason=="frame cap" && result.ended==120,
            "real orchestration logging volume succeeds through 120 modelled 90 Hz frames");
        check(!box.failed.load() && !signals.failed.load() && box.empty() && signals.logger_done.load(),
            "cadence test has no overflow and drains completely");
        check(written==produced+1 && write_cost==written && produced<=LogBounds::run_lines,
            "all production-rate lines charged and written inside full-run reserve");
        check(time.now()==std::max<int64_t>(120*1000/90,write_cost),"deterministic frame cadence and serial per-line completion cost");
    }
    {
        Time time;Signals signals;Mailbox box;unsigned pauses=0,writes=0,probes=0;
        for(unsigned n=0;n<80;++n)box.push("queued");
        signals.worker_done=true;
        file_loop(time,signals,box,[&]{++probes;return StopFileStatus::absent;},
            [&](const std::string&){++writes;time.ms.fetch_add(1);return true;},
            [&](int64_t){++pauses;});
        check(writes==81 && pauses==0 && probes==4 && signals.logger_done.load(),
            "time-budgeted drain resamples files without any sleep while queued");
    }
    {
        Mailbox box;std::atomic<bool> done{false};unsigned received=0;
        std::thread consumer([&]{std::string line;while(!done.load() || !box.empty()) {
            if(box.pop(line))++received;
            (void)box.empty();
        }});
        for(unsigned n=0;n<LogBounds::run_lines;++n)box.push("line "+std::to_string(n));
        done=true;consumer.join();
        check(!box.failed.load() && received==LogBounds::run_lines && box.empty(),
            "concurrent consumer pop/empty cannot cause spurious producer failure");
    }
    {
        Mailbox box;
        for(unsigned n=0;n<LogBounds::capacity;++n)box.push("x");
        check(!box.failed.load(),"full per-run slot reserve fits without any consumer");
        box.push("overflow");check(box.failed.load(),"genuine overflow still fails closed");
    }
    // A truly hung foreign call cannot be simulated synchronously in the core.
    // Exercise the exact pure supervisor policy at its boundary without leaving
    // threads behind or running/terminating the real harness.
    for(const auto* hung_call:{"xrPollEvent","xrWaitFrame","xrDestroySession"}) {
        (void)hung_call;
        check(watch(12999,false)==Watch::keep_waiting,"pre reserve watchdog");
        check(watch(13000,false)==Watch::request_stop,"watchdog stop reserve");
        check(watch(14999,false)==Watch::request_stop,"cleanup inside outer deadline");
        check(watch(15000,false)==Watch::terminate_failure,"hung call FAILURE termination policy");
        check(watch(15000,true)==Watch::keep_waiting,"completed worker never terminate");
    }
    // Exercise the actual orchestration loop with an injected clock. 20 ms is
    // the sampling cadence, NOT the 250 ms foreign-call progress watchdog.
    for(int source:{1,2}) {
        Time time;Signals signals;int64_t killed=-1;
        observe_phase(signals,Phase::frames,time.now());
        const bool ok=supervise(time,signals,
            [&]{return source==1 && time.now()>=100;},
            [&](int64_t n){time.sleep(n);signals.file_progress=time.now();
                if(source==2 && time.now()>=100)observe_file(signals,StopFileStatus::present);},
            [&]{killed=time.now();});
        check(!ok && killed==250,"Escape/stop-file cannot reset the stuck-call 250 ms progress deadline");
        check(signals.stop.load()==source,"correct earlier stop source");
    }
    {
        Time time;Signals signals;int64_t stopped=-1,killed=-1;
        observe_phase(signals,Phase::frames,time.now());
        supervise(time,signals,[]{return false;},[&](int64_t n){
            if(signals.stop.load() && stopped<0){stopped=time.now();observe_phase(signals,Phase::cleanup,time.now());}
            time.sleep(n);++signals.progress;signals.file_progress=time.now();
        },[&]{killed=time.now();});
        check(stopped==13000 && killed==15000,"progressing cleanup: admission 13 s, total cap 15 s");
    }
    {
        Time time;Signals signals;time.ms=15000;
        signals.worker_done=true;signals.logger_done=true;bool killed=false;
        check(!supervise(time,signals,[]{return false;},[&](int64_t n){time.sleep(n);},[&]{killed=true;}) && !killed,
            "completion first observed at outer deadline cannot report success");
    }
    {
        Time time;Signals signals;Mailbox box;signals.worker_done=true;
        file_loop(time,signals,box,[]{return StopFileStatus::unavailable;},
            [](const std::string&){return true;},[&](int64_t n){time.sleep(n);});
        check(signals.failed.load() && signals.stop.load()==4 && signals.logger_done.load(),
            "stop-file access error after activation refuses success");
    }
    // A real file-loop thread stays inside a fake operation that NEVER returns
    // until AFTER the real supervisor seam has decided to terminate. Release
    // afterward solely to join the fixture safely; production never joins it.
    for(bool blocked_probe:{false,true}) {
        Time time;Signals signals;Mailbox box;box.push("manifest");
        signals.worker_done=true;
        std::atomic<bool> entered{false},release{false},returned{false};
        auto stuck=[&]{entered=true;while(!release.load())std::this_thread::yield();returned=true;};
        std::thread file([&]{file_loop(time,signals,box,
            [&]{if(blocked_probe)stuck();return StopFileStatus::absent;},
            [&](const std::string&){if(!blocked_probe)stuck();return true;},
            [&](int64_t n){time.sleep(n);});});
        while(!entered.load())std::this_thread::yield();
        int64_t killed=-1;
        const bool ok=supervise(time,signals,[]{return false;},
            [&](int64_t n){time.sleep(n);},[&]{killed=time.now();});
        check(!ok && killed==250 && !returned.load() && !signals.logger_done.load(),
            "blocked file operation cannot prevent supervisor termination decision");
        release=true;file.join();
    }
    // The production callback uses this exact clamp wrapper before DWORD cast.
    for(auto input:{int64_t(-750),std::numeric_limits<int64_t>::max(),int64_t(0),int64_t(7)}) {
        int64_t requested=-1;
        bounded_pause([&](int64_t n){requested=n;},input);
        check(requested==clamp_pause(input) && requested>=0 && requested<=Caps::poll_ms,
            "direct pause clamps negative and huge inputs before platform conversion");
    }
    for(int state:{0,1,0x8000,0x8001}) {
        unsigned calls=0;
        check(escape_down([&]{++calls;return state;})==((state&0x8000)!=0) && calls==1,
            "injected Escape state uses current-down bit only");
    }
    {
        Time time;Signals signals;time.ms=3000;signals.file_progress=3000;
        bool transitioned=false;int64_t killed=-1;unsigned pauses=0;
        // At now=4000 last_progress is still 3000 and startup deadline 5000.
        // Publish progress + frame phase BETWEEN the check and delay. The old
        // second policy read produced 3000+250-4000 == -750 here.
        supervise(time,signals,[]{return false;},[&](int64_t n){
            check(n>=0 && n<=Caps::poll_ms,"interleaving never requests out-of-range sleep");
            ++pauses;
            if(time.now()==3000)time.ms=4000; // scheduler delay, not a sleep request
            else time.sleep(n);
            signals.file_progress=time.now();
        },[&]{killed=time.now();},[&]{
            if(time.now()==4000){++signals.progress;observe_phase(signals,Phase::frames,time.now());transitioned=true;}
        });
        check(transitioned && pauses>1 && killed==4270,
            "real loop observes new progress at 4020 then enforces unchanged 250 ms deadline");
        check(signals.watchdog_exit.load()==7,"phase transition terminates with worker deadline code");
    }
    {
        Time time;Signals signals;signals.logger_done=true;
        bool advanced=false;int64_t killed=-1;
        supervise(time,signals,[]{return false;},[&](int64_t n){
            check(false,"already due snapshot must re-evaluate without sleeping");time.sleep(n);
        },[&]{killed=time.now();},[&]{if(!advanced){advanced=true;time.ms=5000;}});
        check(killed==5000 && signals.watchdog_exit.load()==7,"scheduling crossing sampled deadline never sleeps");
    }
    for(auto cause:{Cause::worker_deadline,Cause::file_deadline,Cause::absolute_cap}) {
        Time time;Signals signals;int64_t killed=-1;
        if(cause==Cause::worker_deadline)signals.logger_done=true;
        if(cause==Cause::file_deadline)signals.worker_done=true;
        if(cause==Cause::absolute_cap)time.ms=Caps::absolute_ms;
        supervise(time,signals,[]{return false;},[&](int64_t n){time.sleep(n);},[&]{killed=time.now();});
        check(killed==(cause==Cause::absolute_cap?15000:cause==Cause::worker_deadline?5000:250),
            "each watchdog terminates at its own exact deadline");
        check(unpack_terminal(signals.terminal.load()).cause==cause && signals.watchdog_exit.load()==cause_exit(cause),
            "each watchdog publishes its distinct process exit code");
    }
    check(cause_exit(Cause::worker_deadline)==7 && cause_exit(Cause::file_deadline)==8 &&
        cause_exit(Cause::absolute_cap)==11,"watchdog codes are pairwise distinct");
    for(bool worker:{false,true}) {
        Time time;Signals signals;Mailbox box;
        thread_creation_failure(signals,worker,1234);
        observe_phase(signals,Phase::cleanup,0);latch(signals,Cause::logger,4,9999);
        auto terminal=unpack_terminal(signals.terminal.load());
        check(terminal.cause==(worker?Cause::worker_create:Cause::file_create) &&
            terminal.phase==Phase::startup && terminal.detail==1234 && terminal.source==0,
            "thread creation failure latches immediate error and phase before later calls");
        check(cause_exit(terminal.cause)==(worker?4:5),"thread creation failures have distinct codes");
        if(worker) {
            std::vector<std::string> output;
            file_loop(time,signals,box,[]{return StopFileStatus::absent;},
                [&](const std::string& line){output.push_back(line);return true;},[](int64_t){});
            check(output.size()==1 && output.back()==terminal_record(signals) && signals.logger_done.load(),
                "healthy file channel drains cooperative thread-creation terminal record");
        }
    }
    {
        Time time;Signals signals;Mailbox box;box.push("manifest");box.push("after first append");
        signals.worker_done=true;unsigned writes=0;
        file_loop(time,signals,box,[]{return StopFileStatus::absent;},[&](const std::string&){
            if(++writes==1)return true;
            logger_failure(signals,112);return false;
        },[](int64_t){});
        auto t=unpack_terminal(signals.terminal.load());
        check(writes==2 && signals.failed.load() && signals.logger_done.load() && t.cause==Cause::logger && t.detail==112,
            "failure after first append captures immediate error and never retries terminal on failed channel");
    }
    {
        Time time;Signals signals;Mailbox box;box.push("manifest");box.push("throw");
        signals.worker_done=true;unsigned writes=0;
        file_loop(time,signals,box,[]{return StopFileStatus::absent;},[&](const std::string&){
            if(++writes==2)throw std::runtime_error("injected logger exception");return true;
        },[](int64_t){});
        check(writes==2 && signals.logger_done.load() && signals.failed.load() &&
            unpack_terminal(signals.terminal.load()).cause==Cause::logger,"logger exception is a latched cooperative failure");
    }
    for(unsigned source:{1u,2u}) {
        Time time;Signals signals;Mailbox box;std::vector<std::string> output;
        // Real supervisor captures an Escape stop before the worker starts.
        if(source==1)supervise(time,signals,[]{return true;},[&](int64_t n){
            time.sleep(n);signals.worker_done=true;signals.logger_done=true;
        },[]{check(false,"early cooperative stop never terminates");});
        else observe_file(signals,StopFileStatus::present);
        signals.worker_done=true;signals.logger_done=false;
        file_loop(time,signals,box,[]{return StopFileStatus::absent;},
            [&](const std::string& line){output.push_back(line);return true;},[](int64_t){});
        auto t=unpack_terminal(signals.terminal.load());
        check(t.cause==Cause::operator_stop && t.source==source && t.phase==Phase::startup && cause_exit(t.cause)==9,
            "early operator stop preserves category phase source and exit code");
        check(!output.empty() && output.back()==terminal_record(signals),"healthy cooperative early stop gets terminal record");
        latch(signals,Cause::worker_failure);check(unpack_terminal(signals.terminal.load()).cause==Cause::operator_stop,
            "later worker failure cannot erase first operator cause");
    }
    {
        Signals signals;observe_phase(signals,Phase::frames,100);
        observe_file(signals,StopFileStatus::unavailable,5);
        logger_failure(signals,99);auto t=unpack_terminal(signals.terminal.load());
        check(t.cause==Cause::stop_file_error && t.phase==Phase::frames && t.source==4 && t.detail==5 && cause_exit(t.cause)==10,
            "stop-file access error captures immediate Win32 error before logger failure");
    }
    {
        Time time;Signals signals;Mailbox box;std::vector<std::string> output;
        observe_phase(signals,Phase::cleanup,0);signals.worker_done=true;box.push("SUCCESS clean-retirement");
        file_loop(time,signals,box,[]{return StopFileStatus::absent;},
            [&](const std::string& line){output.push_back(line);return true;},[](int64_t){});
        check(output.size()==2 && output.front()=="SUCCESS clean-retirement" && output.back()==terminal_record(signals) &&
            output.back().find("category=none phase=2 stop_source=0")!=std::string::npos &&
            cause_exit(unpack_terminal(signals.terminal.load()).cause)==0 && signals.logger_done.load(),
            "healthy success drains terminal record after all worker output");
    }
    {
        Time time;Signals signals;
        supervise(time,signals,[]{return true;},[&](int64_t n){time.sleep(n);signals.file_progress=time.now();},[]{});
        check(unpack_terminal(signals.terminal.load()).cause==Cause::operator_stop &&
            cause_exit(Cause::operator_stop)==9 && signals.watchdog_exit.load()==7,
            "watchdog exit code overrides an earlier cooperative stop cause without erasing it");
    }
    std::cout<<checks<<" fake-only checks passed; no runtime parity claimed\n";
    return 0;
}
