#include "colour_owner.h"
#include "format_policy.h"
#include "supervision.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace acz::probe;
namespace {
unsigned checks=0;
void check(bool b,const char* why){++checks;if(!b){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
struct Time final:Clock {
    int64_t ms=0;
    int64_t now() override{return ms;}
    void sleep(int64_t n) override{check(n>=0 && n<=Caps::poll_ms,"bounded fake pause");ms+=n;}
};
double encode(double x){return x<=0.0031308?12.92*x:1.055*std::pow(x,1.0/2.4)-0.055;}
// Independently injected transfer simulator. This checks the linear input / RTV
// policy; it cannot verify a driver, actual stored texel or displayed luminance.
struct Fake final:Backend,ColourBackend {
    Time& t;
    std::vector<int64_t> formats{29};
    int count_delta=0;
    ImageOp failure=ImageOp::count, qualified_loss=ImageOp::count;
    unsigned fault_eye=0;
    Op core_failure=Op::count;
    unsigned core_fail_at=1;
    bool render=true,space_change=false,discard=false,sticky_loss=false,stopping=false;
    bool timeout=false,first_timeout=false,pending=false,max_value=false,regress=false,bad_epoch=false;
    bool bad_rtv=false,unorm_typeless=true,stop_on_record=false,stop_on_close=false;
    bool blocked=false,handles_destroyed=false,after_cap_call=false;
    unsigned delay_samples=0,sample_count=0,projection_calls=0,zero_calls=0;
    bool right_pending=false;
    std::vector<int64_t> sample_times;
    unsigned active_frame=0,format_calls=0;
    int64_t frame_ms=1,operation_ms=0;
    uint64_t last_issued=0;
    std::array<unsigned,static_cast<unsigned>(ImageOp::count)> calls{};
    std::array<unsigned,static_cast<unsigned>(Op::count)> core_calls{};
    std::array<ImageRecord,2> image_state{};
    std::array<unsigned,2> waits{},releases{};
    std::string stop;
    std::vector<std::string> log;
    std::function<double(double)> transfer=encode;
    std::function<void(ImageOp)> before_image=[](ImageOp){};
    std::array<double,2> observed{};
    int64_t selected=0,rtv_format=0;
    explicit Fake(Time& clock):t(clock){}
    Fake(const Fake&)=delete;Fake& operator=(const Fake&)=delete;
    ColourBackend* colour_backend() override{return this;}
    Reply call(Op op,unsigned layer_count,const void* opaque) override {
        auto n=++core_calls[static_cast<unsigned>(op)];
        if(op==core_failure && n==core_fail_at)return {false};
        if(op==Op::formats) {
            bool good=observe_phase1_formats([&](uint32_t cap,uint32_t* count,int64_t* data){
                ++format_calls;
                if(!cap){*count=static_cast<uint32_t>(formats.size());return true;}
                *count=static_cast<uint32_t>(static_cast<int>(formats.size())+count_delta);
                std::copy_n(formats.begin(),std::min<size_t>(cap,formats.size()),data);return true;
            },[&](const std::string& s){log.push_back(s);});
            return {good};
        }
        if(op==Op::poll)return {true,stopping?State::stopping:State::ready,false,render,false,
            space_change && active_frame>=Caps::frames};
        if(op==Op::request_exit)stopping=true;
        if(op==Op::wait_frame){t.ms+=frame_ms;++active_frame;}
        if(op==Op::end_frame) {
            if(!layer_count){check(!opaque,"zero end has null layer");++zero_calls;}
            else {
                check(layer_count==1 && opaque,"projection is one two-view layer");
                const auto& p=*static_cast<const Projection*>(opaque);
                check(p.frame==active_frame && p.sample==active_frame,"exact located frame and sample");
                for(const auto& s:image_state)check(s.complete && s.released && !s.acquired,"both eye completion/release witnesses");
                ++projection_calls;
            }
        }
        return {true,sticky_loss?State::loss:State::idle,false,render,discard,false};
    }
    ImageReply image(ImageOp op,const ImageRequest& q) override {
        ++calls[static_cast<unsigned>(op)];
        after_cap_call |= t.ms>=Caps::absolute_ms;
        t.ms+=operation_ms;
        before_image(op);
        ImageReply p;p.ok=true;p.key=q.key;
        const bool fault=q.key.eye==fault_eye;
        const bool lose=op==qualified_loss && fault;
        if(lose)sticky_loss=true;
        p.loss=sticky_loss;
        // A failed Execute may have dispatched, so simulate the side effect.
        if(op==failure && fault && op!=ImageOp::execute){p.ok=false;return p;}
        if(op==ImageOp::setup) {
            auto choice=select_later_colour_format(formats);
            p.ok=choice.has_value() && !bad_rtv && (*choice!=28 || unorm_typeless);
            if(p.ok){selected=*choice;rtv_format=29;}
            p.key.device=1;p.key.queue=2;p.key.fence=3;p.key.epoch=4;
            return p;
        }
        if(op==ImageOp::locate){p.sample=active_frame?active_frame:1;p.views_valid=true;return p;}
        if(op==ImageOp::destroy){
            for(const auto& s:image_state)check(!s.acquired && (!s.submitted || s.complete),"destroy only after idle and legal release");
            handles_destroyed=true;return p;
        }
        check(q.key.eye<2,"bounded eye");auto& s=image_state[q.key.eye];
        switch(op) {
        case ImageOp::acquire:
            check(!s.acquired && (!s.submitted || s.complete),"one outstanding acquisition per eye");
            s={};s.key={1,2,3,4,100+q.key.eye,200+q.key.eye,300+q.key.eye,400+q.key.eye,
                q.key.frame,0,q.key.eye,0};s.acquired=true;p.key=s.key;waits[q.key.eye]=0;break;
        case ImageOp::wait:
            check(s.acquired && !s.waited,"wait only acquisition");
            check(q.wait_ns==(waits[q.key.eye]?1000000:0),"exact wait sequence 0 then one 1 ms");
            ++waits[q.key.eye];check(waits[q.key.eye]<=2,"at most two image waits");
            p.timeout=(timeout && fault) || (first_timeout && waits[q.key.eye]==1);
            if(!p.timeout)s.waited=true;break;
        case ImageOp::record:
            check(s.waited && !s.released,"write only after wait, before release");
            check(rtv_format==29,"sRGB RTV for single encoding");
            for(float x:dim_clear)check(std::isfinite(x) && x>=0 && x<=1,"finite bounded linear constants");
            check(dim_clear[3]==1 && dim_clear[0]>=0.01f && dim_clear[0]<=0.05f,"dim opaque clear");
            observed[q.key.eye]=transfer(dim_clear[0]);s.recorded=true;
            if(stop_on_record)stop="stop file";
            break;
        case ImageOp::close:check(s.recorded,"Close after exact clear");s.closed=true;if(stop_on_close)stop="stop file";break;
        case ImageOp::execute:
            check(s.closed && !s.submitted && q.key.value==last_issued+1,"one authenticated submit with next exact value");
            s.key=q.key;s.submitted=true;p.dispatch_possible=true;
            p.ok=!(op==failure && fault);break;
        case ImageOp::signal:
            check(s.submitted && !s.signalled,"GPU Signal follows execute exactly once");
            s.signalled=true;last_issued=q.key.value;break;
        case ImageOp::sample:
            check(s.submitted && s.signalled,"completion only after Signal");
            ++sample_count;
            sample_times.push_back(t.ms);
            p.completed=(pending || sample_count<=delay_samples)?0:last_issued;
            if(right_pending && q.key.eye==1)p.completed=last_issued-1; // valid pending, not regression
            if(max_value)p.completed=std::numeric_limits<uint64_t>::max();
            if(regress && q.key.eye==1)p.completed=0;
            if(bad_epoch)++p.key.epoch;
            s.complete=p.ok && !bad_epoch && p.completed!=std::numeric_limits<uint64_t>::max() && p.completed>=s.key.value;
            break;
        case ImageOp::release:
            check(s.waited && (!s.submitted || s.complete),"release before completion refused by owner");
            check(!s.release_attempted,"release never retried");s.release_attempted=true;
            ++releases[q.key.eye];s.acquired=false;s.released=true;break;
        default:break;
        }
        return p;
    }
    unsigned n(ImageOp op) const{return calls[static_cast<unsigned>(op)];}
};
struct Fixture {
    Time time;Fake fake{time};std::vector<std::string> lines;
    ColourOwner owner{fake,time,[&](const std::string& s){lines.push_back(s);},[&]{return fake.stop;}};
    Fixture()=default;Fixture(const Fixture&)=delete;Fixture& operator=(const Fixture&)=delete;
    Result run(Mode mode=Mode::phase2_dim_clear) {
        Run run(fake,time,[&](const std::string& s){lines.push_back(s);},[&]{return fake.stop;},{},mode);
        return run.execute();
    }
};
}
int main() {
    // Format enumeration is the shared bounded seam, including duplicates and
    // changed counts; a1 remains informational and performs no image calls.
    for(const auto& list:std::vector<std::vector<int64_t>>{{29},{28},{28,29},{91},{},{28,28,29,29}}) {
        Fixture f;f.fake.formats=list;auto r=f.run();
        const auto choice=select_later_colour_format(list);
        check(r.success==choice.has_value(),"a2 format refusal/preference");
        if(choice)check(f.fake.selected==*choice && f.fake.rtv_format==29,"swapchain family and compatible sRGB RTV");
        check(f.fake.zero_calls>=120,"clean a1 frames precede all colour work");
        check(f.fake.format_calls==(list.empty()?1u:2u),"format enumeration bounded");
    }
    for(int delta:{-1,1}) {
        Fixture f;f.fake.count_delta=delta;check(!f.run().success,"count change refuses");
        check(f.fake.n(ImageOp::setup)==0,"a1 enumeration failure refuses a2");
    }
    {Fixture f;auto r=f.run(Mode::zero_layers);check(r.success && r.begun==120,"a1 unchanged budget");
        for(auto n:f.fake.calls)check(n==0,"a1 zero swapchain/render operations");}
    {Fixture f;f.fake.core_failure=Op::end_frame;auto r=f.run();check(!r.success && f.fake.n(ImageOp::setup)==0,"any a1 failure refuses a2");}
    {Fixture f;auto r=f.run();check(r.success && r.clean && r.begun==180 && r.ended==180,"a1 then at most 60 dim frames");
        check(f.fake.projection_calls==60 && f.fake.handles_destroyed,"two-eye projection and ordered retirement");
        check(std::abs(f.fake.observed[0]-encode(0.02))<1e-7,"single encoding dim reference");}
    // The injected transfer check distinguishes missing and double encoding.
    for(int mode:{0,1,2}) {
        Fixture f;
        if(mode==0)f.fake.transfer=[](double x){return x;};
        if(mode==2)f.fake.transfer=[](double x){return encode(encode(x));};
        check(f.owner.setup(),"reference setup");check(f.owner.render(1,true).sample!=0,"reference two-eye clear");
        const bool matches=std::abs(f.fake.observed[0]-encode(0.02))<1e-7;
        check(matches==(mode==1),"dim reference rejects missing/double encoding");
        check(f.owner.retire(2000),"reference retires");
    }
    {Fixture f;f.fake.bad_rtv=true;check(!f.owner.setup(),"incompatible RTV refused");}
    {Fixture f;f.fake.formats={28};f.fake.unorm_typeless=false;check(!f.owner.setup(),"typed UNORM without sRGB compatibility refused");}
    {Fixture f;f.fake.first_timeout=true;check(f.owner.setup(),"wait retry setup");check(f.owner.render(1,true).sample!=0,"0 timeout then 1 ms success");
        check(f.fake.waits[0]==2 && f.fake.waits[1]==2,"bounded retry per eye");check(f.owner.retire(2000),"wait success retirement");}
    {Fixture f;f.fake.timeout=true;check(f.owner.setup(),"timeout setup");check(!f.owner.render(1,true).sample,"timeout zero projection");
        check(f.owner.records()[0].acquired && !f.owner.records()[0].waited,"timeout retains acquisition");
        check(f.fake.n(ImageOp::record)==0 && f.fake.n(ImageOp::release)==0,"timeout no write/release");
        check(!f.owner.retire(2000) && !f.fake.handles_destroyed,"unready image prevents destruction");}
    {Fixture f;f.fake.timeout=true;f.fake.fault_eye=1;auto r=f.run();
        check(!r.success && f.fake.projection_calls==0,"left complete/right timeout zero layers");
        check(f.fake.image_state[0].released && f.fake.image_state[1].acquired,"asymmetric ownership retained");}
    for(auto op:{ImageOp::record,ImageOp::close}) {
        Fixture f;f.fake.failure=op;check(f.owner.setup(),"record/Close setup");check(!f.owner.render(1,true).sample,"failed command zero layers");
        check(f.fake.n(ImageOp::execute)==0,"failed recording/Close never executes");
        check(f.owner.retire(2000) && f.fake.releases[0]==1,"waited no submitted work legal release without fence");
    }
    for(auto op:{ImageOp::execute,ImageOp::signal}) {
        Fixture f;f.fake.failure=op;check(f.owner.setup(),"ambiguous submit setup");check(!f.owner.render(1,true).sample,"ambiguous submit zero layer");
        check(!f.owner.retire(100) && f.fake.n(ImageOp::release)==0 && !f.fake.handles_destroyed,"ambiguous submission retains dependencies");
    }
    {Fixture f;f.fake.pending=true;check(f.owner.setup(),"pending setup");check(!f.owner.render(1,true).sample,"Signal success alone not completion");
        check(f.time.ms==100 && f.fake.sample_count==100,"shared frame window has at most one hundred 1 ms samples");
        const auto frame_samples=f.fake.sample_count;
        check(!f.owner.retire(350) && f.time.ms==350,"pending retirement bounded");
        check(f.fake.sample_count-frame_samples==2,"cleanup no faster than ten polls per second");
        for(size_t i=frame_samples;i<f.fake.sample_times.size();++i)
            check(f.fake.sample_times[i]-f.fake.sample_times[i-1]>=100,"cleanup samples separated by at least 100 ms");
        check(f.fake.n(ImageOp::release)==0 && !f.fake.handles_destroyed,"no early release or timeout destruction");}
    {Fixture f;f.fake.pending=true;check(f.owner.setup(),"cleanup cap setup");(void)f.owner.render(1,true);
        const auto n=f.fake.sample_count;
        check(!f.owner.retire(10000) && f.time.ms==2100,"retirement never widens two second budget");
        check(f.fake.sample_count-n<=20,"at most twenty cleanup samples in two seconds");}
    {Fixture f;f.fake.delay_samples=12;f.fake.right_pending=true;check(f.owner.setup(),"shared eyes setup");
        check(!f.owner.render(1,true).sample && f.time.ms==100,"second eye shares first eye's 100 ms deadline");
        check(f.owner.records()[0].released && f.owner.records()[1].acquired,"pending right eye gives no stale/one-eye layer");
        for(size_t i=1;i<f.fake.sample_times.size();++i)
            check(f.fake.sample_times[i]-f.fake.sample_times[i-1]>=1,"sample spacing includes eye boundary");}
    {Fixture f;f.fake.delay_samples=150;check(f.owner.setup(),"late setup");check(!f.owner.render(1,true).sample,"late frame zero layers");
        f.fake.delay_samples=f.fake.sample_count+1;
        check(f.owner.retire(350) && f.fake.releases[0]==1,"delayed proven completion legally retires");
        check(f.owner.failed(),"late completion cannot turn failed frame into success");}
    for(int fault:{0,1,2}) {
        Fixture f;f.fake.max_value=fault==0;f.fake.bad_epoch=fault==1;f.fake.regress=fault==2;
        check(f.owner.setup(),"invalid completion setup");check(!f.owner.render(1,true).sample,"loss/epoch/regression zero layers");
        check(!f.owner.retire(100) && !f.fake.handles_destroyed,"invalid completion quarantines");
    }
    {Fixture f;f.fake.delay_samples=2;check(f.owner.setup(),"bounded progress setup");
        check(f.owner.render(1,true).sample!=0 && f.time.ms==3,"valid delayed completion inside shared frame budget");
        check(f.owner.retire(100),"bounded progress retirement");}
    {Fixture f;f.fake.failure=ImageOp::release;check(f.owner.setup(),"release failure setup");
        check(!f.owner.render(1,true).sample,"failed release zero layers");check(!f.owner.retire(100),"failed release retains");
        check(f.fake.n(ImageOp::release)==1,"failed release not retried");}
    {Fixture f;f.fake.core_failure=Op::end_frame;f.fake.core_fail_at=121;auto r=f.run();
        check(!r.success && f.fake.core_calls[static_cast<unsigned>(Op::end_frame)]==121,"failed end never retried");}
    {Fixture f;f.fake.render=false;auto r=f.run();check(r.success && r.begun==180,"shouldRender=false loop continues");
        check(f.fake.n(ImageOp::acquire)==0 && f.fake.n(ImageOp::locate)==0 && f.fake.n(ImageOp::record)==0,"shouldRender=false skips image/render/locate");}
    {Fixture f;f.fake.discard=true;auto r=f.run();check(r.success && f.fake.zero_calls==180,"FRAME_DISCARDED accepted begin ends zero once");
        check(f.fake.n(ImageOp::acquire)==0,"discarded frame no images");}
    {Fixture f;f.fake.space_change=true;auto r=f.run();check(!r.success && f.fake.projection_calls==0,"space change stops, no stale-space layer");}
    for(auto op:{ImageOp::setup,ImageOp::locate,ImageOp::acquire,ImageOp::wait,ImageOp::record,
            ImageOp::close,ImageOp::execute,ImageOp::signal,ImageOp::sample,ImageOp::release,ImageOp::destroy}) {
        Fixture f;f.fake.qualified_loss=op;auto r=f.run();
        check(!r.success,"qualified loss at each operation is sticky veto");
        if(op==ImageOp::release)check(f.fake.n(ImageOp::release)==1,"accepted loss-pending release not retried");
    }
    {Fixture f;f.fake.stop_on_record=true;auto r=f.run();check(r.success && r.image_exit_code==0 && r.reason=="stop file" && f.fake.n(ImageOp::execute)==0,"orderly stop closes further render admission without image failure");
        check(f.fake.projection_calls==0,"stop prevents projection");}
    {Fixture f;f.fake.stop_on_close=true;auto r=f.run();
        check(r.success && r.image_exit_code==0 && f.fake.n(ImageOp::execute)==0 && f.fake.releases[0]==1,"stop before Execute is known unsubmitted and releases legally");}
    {Fixture f;f.fake.before_image=[&](ImageOp op){if(op==ImageOp::record)f.time.ms=Caps::admission_ms;};
        auto r=f.run();check(r.success && r.image_exit_code==0 && r.reason=="time cap","mid-frame 13 s stop is orderly, no ledger failure");
        check(f.fake.projection_calls==0 && f.fake.n(ImageOp::execute)==0,"cap prevents projection and submission");}
    {Signals s;observe_file(s,StopFileStatus::present);
        check(s.admission.closed() && s.stop==2 && unpack_terminal(s.terminal.load()).cause==Cause::operator_stop,"operator cause published before admission closure");}
    {Fixture f;AdmissionGate gate;bool worker_failure=false;
        Run run(f.fake,f.time,[&](const std::string& line){
            worker_failure|=line.find("failure_reason=")==0;
            if(f.fake.active_frame==121 && line.find("core entry=wait_frame")==0){
                f.fake.stop="stop file";gate.close();
            }
        },[&]{return f.fake.stop;},{},Mode::phase2_dim_clear,&gate);
        auto r=run.execute();
        check(r.success && r.clean && r.reason=="stop file" && r.image_exit_code==0 && !worker_failure,"stop racing frame admission retires cooperatively");
        check(r.waited==121 && r.begun==121 && r.ended==121,"refused wait_frame has no foreign call or unbalanced frame");}
    {Fixture f;Signals s;s.phase2=true;s.worker_deadline=15000;f.time.ms=13000;s.file_progress=13000;
        supervise(f.time,s,[]{return false;},[&](int64_t){s.worker_done=true;s.logger_done=true;},[]{});
        check(s.stop==3 && s.admission.closed() && unpack_terminal(s.terminal.load()).cause==Cause::admission_cap,"13 s a2 cap latches distinct cooperative cause");
        check(cause_exit(Cause::admission_cap)==11,"a2 cap uses cap code rather than image code");}
    {Fixture f;f.fake.failure=ImageOp::destroy;auto r=f.run();check(!r.clean && !r.success,"failed destroy prevents parent cleanup");
        check(f.fake.core_calls[static_cast<unsigned>(Op::destroy_space)]==0,"failed swapchain destroy retains parent space");}
    {Fixture f;f.time.ms=13000;check(!f.owner.setup() && f.fake.n(ImageOp::setup)==0,"13 s image admission");}
    {Fixture f;f.time.ms=15000;check(!f.owner.setup() && !f.fake.after_cap_call,"15 s refuses even cleanup calls");}
    {Fixture f;f.fake.frame_ms=110;auto r=f.run();check(!r.success && f.fake.n(ImageOp::setup)==0,"time cap before complete a1 refuses a2");
        check(!f.fake.after_cap_call,"no call at absolute cap");}
    // Actual supervisor policy with a blocked image operation simulated by no
    // progress publication. Enforcement never calls the image backend or frees.
    for(int kind:{0,1}) {
        Fixture f;Signals s;s.worker_deadline=-1;
        check(f.owner.setup(),"blocked worker owns native-like handles");
        bool terminated=false;
        if(kind==1){f.time.ms=14900;s.worker_deadline=15000;}
        s.file_progress=f.time.now();
        auto ok=supervise(f.time,s,[]{return false;},[&](int64_t n){f.time.sleep(n);s.file_progress=f.time.now();},[&]{terminated=true;});
        check(!ok && terminated && !f.fake.handles_destroyed,"watchdog never destroys blocked owner's handles");
        check(s.watchdog_exit==(kind?11:7),"same 250 ms / 15 s watchdog code family");
    }
    for(int code=12;code<=17;++code) {
        auto cause=static_cast<Cause>(static_cast<unsigned>(Cause::image_ledger)+code-12);
        Signals s;latch(s,cause);latch(s,Cause::worker_failure);
        check(cause_exit(unpack_terminal(s.terminal.load()).cause)==code,"image first cause sticky and distinct");
    }
    {
        Fixture f;std::mutex mutex;std::condition_variable cv;bool entered=false,resume=false;
        Signals s;s.worker_deadline=-1;
        ColourOwner owner{f.fake,f.time,[&](const std::string& line){f.lines.push_back(line);},[]{return std::string{};},&s.admission};
        check(owner.setup(),"blocked record setup owns dependencies");
        f.fake.before_image=[&](ImageOp op){if(op==ImageOp::record){
            std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return resume;});}};
        std::thread worker([&]{(void)owner.render(1,true);});
        {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return entered;});}
        bool terminated=false;
        supervise(f.time,s,[]{return false;},[&](int64_t n){f.time.sleep(n);s.file_progress=f.time.now();},[&]{terminated=true;});
        check(terminated && f.fake.image_state[0].acquired && !f.fake.handles_destroyed && f.fake.n(ImageOp::destroy)==0,"actually blocked backend never destroyed by supervisor");
        {std::lock_guard<std::mutex> lock(mutex);resume=true;}cv.notify_all();worker.join();
        check(!owner.stop_reason().empty() && s.failed && f.fake.n(ImageOp::execute)==0,"late admitted result cannot reopen rendering after watchdog failure");
    }
    {AdmissionGate g;check(g.enter(),"first atomic admission");g.close();
        check(!g.enter(),"stop denies concurrent or later admission");g.leave();
        check(g.closed() && !g.enter(),"retiring admitted call never reopens stop");}
    std::cout<<checks<<" synthetic colour-owner checks passed; no runtime parity\n";
}
