// CPU-only synthetic observations. No GPU, game, XR or runtime parity claim.
#include "gpu_copy_hud_observe.h"
#include "runtime_config.h"
#include <cstdlib>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace acz::gpu_copy::hud;
static Record draw(uint64_t rt=100,uint32_t format=10) {
    Record r;r.list=1;r.generation=7;r.rt=rt;r.rt_known=true;r.format=format;
    r.pso_known=r.viewport_known=true;r.pipeline=55;r.id=66;r.count=6;r.instances=1;
    r.depth_func=8;r.blend=1;r.mask=15;r.viewport[2]=2560;r.viewport[3]=1440;r.viewport[5]=1;
    std::strcpy(r.vs,"f4cfd928c11d0a822baeb92e09767134");std::strcpy(r.ps,"e1372ac10d7191c7e4d1c03b66465260");
    return r;
}
static std::vector<Record> drain(State& s) {std::vector<Record> out;Record r;while(s.pop(r))out.push_back(r);return out;}
static void config() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_hud_observe());
    CHECK(!parse_config("").config.gpu_copy_step5_hud_observe());
    for(const auto* v:{"true","1"}) {auto c=parse_config(std::string("gpu_copy_step5_hud_observe=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_hud_observe());}
    for(const auto* v:{"false","0"}) {auto c=parse_config(std::string("gpu_copy_step5_hud_observe=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_hud_observe());}
    CHECK(parse_config("gpu_copy_step5_hud_observe=yes").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_hud_observe=true\ngpu_copy_step5_hud_observe=false").issue==ConfigIssue::duplicate);
}
static void fields() {
    State s;s.begin(1,7,100);s.matched_member(29);s.draw(draw());s.draw(draw(200,24));
    auto records=drain(s);unsigned tails=0,changes=0,r10=0;
    for(const auto& r:records) {
        char a[line_bytes],b[line_bytes];format(r,a,b);std::string x=a,y=b;
        CHECK(x.find("back_buffer=unknown")!=x.npos&&y.find("back_buffer=unknown")!=y.npos);
        CHECK(x.find("truncated=0")!=x.npos&&y.find("truncated=0")!=y.npos);
        for(auto* key:{"pipeline=37;id=66;vs=f4cfd928c11d0a822baeb92e09767134","after_member=29","ps=e1372ac10d7191c7e4d1c03b66465260"})CHECK(x.find(key)!=x.npos);
        for(auto* key:{"depth_func=8;depth_write=0;stencil=0,0,0,0,0,0;blend=1,0;write_mask=15","viewport=0,0,2560,1440,0,1","indexed=0;count=6;instances=1"})CHECK(y.find(key)!=y.npos);
        if(r.event==Event::tail) {++tails;CHECK(x.find("provisional=1")!=x.npos);CHECK(r.member==29&&r.seq==tails);
            CHECK(x.find(tails==1?"rt_class=scene;rt=64;rt_format=10;fmt_r10g10b10a2=0":"rt_class=other;rt=c8;rt_format=24;fmt_r10g10b10a2=1")!=x.npos);}
        changes+=r.event==Event::rt_change;r10+=r.event==Event::r10;
    }
    CHECK(tails==2&&changes==1&&r10==1);
    // Unknown fields remain explicitly unknown/incomplete, including absent PSO.
    auto r=draw();r.rt_known=false;r.format=0;r.pso_known=false;r.vs[0]=r.ps[0]=0;
    s.draw(r);r=drain(s).back();char a[line_bytes],b[line_bytes];format(r,a,b);
    CHECK(std::strstr(a,"rt_class=unknown")&&std::strstr(a,"fmt_r10g10b10a2=unknown")&&std::strstr(a,"vs=0/unknown")&&std::strstr(a,"truncated=1"));
    // Maximum integer widths and pathological float values cannot overflow lines.
    r.frame=r.list=r.generation=r.seq=r.member=r.pipeline=r.id=UINT64_MAX;
    r.viewport[0]=3.4e38f;format(r,a,b);CHECK(std::strlen(a)<line_bytes&&std::strlen(b)<line_bytes);
    CHECK(std::strstr(a,"truncated=1")&&std::strstr(b,"truncated="));
}
static void events() {
    State s;s.begin(1,7,100);s.matched_member(29);
    auto r=draw(200,24);s.target(r);s.target(r);s.draw(r);
    auto b=draw();b.before=4;b.after=128;s.barrier(b);s.barrier(b);
    s.matched_member(30);s.draw(r);s.matched_member(31);
    auto wrong=r;wrong.list=2;s.draw(wrong);wrong=r;wrong.generation=8;s.draw(wrong);
    s.close(1,8,true);CHECK(s.active);s.close(1,7,true);s.close(1,7,true);s.draw(r);
    unsigned counts[7]{};for(const auto& e:drain(s)) {++counts[unsigned(e.event)];
        if(e.event==Event::member_resume)CHECK(e.member==29&&e.resume_member==30&&e.seq==2);
        if(e.event==Event::tail)CHECK(e.member==29||e.member==30);
    }
    CHECK(counts[unsigned(Event::tail)]==2);
    for(auto e:{Event::rt_change,Event::scene_barrier,Event::r10,Event::close,Event::member_resume})CHECK(counts[unsigned(e)]==1);
    s.begin(1,8,100);CHECK(!s.member&&!s.seq&&!s.tailSeen);s.draw(draw());CHECK(drain(s).empty());
}
static void bounds() {
    State s;auto r=draw();unsigned sampled=0;
    for(uint64_t f=1;f<=1200;++f) {s.begin(1,7,100);s.matched_member(29);s.draw(r);s.close(1,7,true);
        const auto v=drain(s);CHECK(v.empty()==!State::sample(f));if(!v.empty())++sampled;}
    CHECK(sampled==5); // frames 1,2,3,600,1200
    s.begin(1,7,100);s.sampled=true;s.matched_member(29);
    for(unsigned i=0;i<200;++i)s.draw(r);
    auto v=drain(s);CHECK(v.size()==frame_records&&v.back().event==Event::truncated&&v.back().truncated);
    // Repeated sampled frames cannot exceed the process/run record budget.
    for(unsigned f=0;f<100;++f) {s.begin(1,7,100);s.sampled=true;s.matched_member(29);for(unsigned i=0;i<100;++i)s.draw(r);drain(s);}
    CHECK(s.runCount==run_records);s.begin(1,7,100);s.draw(r);CHECK(drain(s).empty());
    State q;for(unsigned f=0;f<8;++f) {q.begin(1,7,100);q.sampled=true;q.matched_member(29);for(unsigned i=0;i<70;++i)q.draw(r);}
    CHECK(q.head-q.tail==queue_records&&q.lost>0);CHECK(drain(q).back().truncated);
    State a;a.begin(1,7,100);a.matched_member(29);
    for(unsigned i=0;i<rt_addresses+2;++i)a.target(draw(200+i,24));
    unsigned seen=0;bool cut=false;for(const auto& e:drain(a)){seen+=e.event==Event::r10;cut|=e.truncated;}
    CHECK(seen==rt_addresses&&cut);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;const std::string scenario=argv[1];
    if(scenario=="config")config();else if(scenario=="fields")fields();else if(scenario=="events")events();else if(scenario=="bounds")bounds();else return 2;
    std::puts("HUD observer: synthetic CPU checks only; no runtime evidence");
}
