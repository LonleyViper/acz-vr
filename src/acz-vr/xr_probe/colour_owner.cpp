#include "colour_owner.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace acz::probe {
namespace {
const char* image_name(ImageOp op) {
    static constexpr const char* names[]={"setup","locate","acquire","wait","record","close","execute","signal","sample","release","destroy","readback"};
    return names[static_cast<unsigned>(op)];
}
std::string key_text(const ImageKey& k) {
    return " frame="+std::to_string(k.frame)+" eye="+std::to_string(k.eye)+" index="+std::to_string(k.index)+
        " device="+std::to_string(k.device)+" queue="+std::to_string(k.queue)+" fence="+std::to_string(k.fence)+
        " epoch="+std::to_string(k.epoch)+" resource="+std::to_string(k.resource)+" RTV="+std::to_string(k.rtv)+
        " allocator="+std::to_string(k.allocator)+" list="+std::to_string(k.list)+" issued="+std::to_string(k.value);
}
}
ColourOwner::ColourOwner(ColourBackend& backend,Clock& c,Log l,std::function<std::string()> s,AdmissionGate* admission,bool stereo_mode)
    :b(backend),clock(c),log(std::move(l)),stop(std::move(s)),gate(admission?admission:&local_gate),synthetic(stereo_mode) {}
void ColourOwner::fail(int code,const char* why) {
    // A refused new-work call on cooperative stop has no image failure receipt.
    // Retirement failures and observed loss must still be reported.
    if(!admission_stop.empty() && code>=12 && code<=14)return;
    if(!error){error=code;log("image_failure_code="+std::to_string(code)+" reason="+why);}
    closed_admission=true;
    gate->close();
}
ImageReply ColourOwner::call(ImageOp op,const ImageRequest& q,bool admission) {
    auto stopped=[&] {
        auto why=stop();
        if(!closed_admission && frame_admitted(clock.now()) && why.empty() && !gate->closed())return false;
        admission_stop=!why.empty()?why:!frame_admitted(clock.now())?"time cap":"stop requested";
        closed_admission=true;gate->close();
        log("image_stop_disposition="+admission_stop);
        return true;
    };
    if(admission && stopped())return {};
    if(clock.now()>=operation_deadline){fail(12,"deadline refuses image call");return {};}
    auto start=clock.now();
    log(std::string("image entry=")+image_name(op)+key_text(q.key)+" wait_ns="+std::to_string(q.wait_ns)+" ms="+std::to_string(start));
    // Recheck after potentially delayed logging. Only the serial owner dispatches.
    if(admission && stopped())return {};
    if(op==ImageOp::sample && clock.now()>=sample_deadline) {
        ImageReply pending;pending.timeout=true;return pending;
    }
    if(clock.now()>=operation_deadline) {
        fail(12,"image admission closed after log");return {};
    }
    // A grant spans exactly one backend operation. Concurrent stop closes the
    // gate for all later work while this owner retains the admitted operation.
    struct Grant { AdmissionGate* g=nullptr;~Grant(){if(g)g->leave();} } grant;
    if(admission) {
        if(!gate->enter()){
            if(!stopped())fail(12,"atomic image admission refused");
            return {};
        }
        grant.g=gate;
    }
    auto r=b.image(op,q);
    log(std::string("image exit=")+image_name(op)+key_text(r.key)+" ok="+std::to_string(r.ok)+
        " loss="+std::to_string(r.loss)+" timeout="+std::to_string(r.timeout)+
        " dispatch_possible="+std::to_string(r.dispatch_possible)+
        " completed="+std::to_string(r.completed)+" duration_ms="+std::to_string(clock.now()-start));
    // The caller records successful side effects even on qualified session loss.
    if(r.loss){loss=true;fail(15,"sticky image/session loss");}
    if(op!=ImageOp::setup && op!=ImageOp::locate && op!=ImageOp::acquire && op!=ImageOp::destroy &&
        !same_key(q.key,r.key))fail(12,"image receipt identity mismatch");
    return r;
}
bool ColourOwner::setup() {
    if(setup_attempted){fail(12,"duplicate setup");return false;}
    setup_attempted=true;
    auto r=call(ImageOp::setup,{},true);
    if(!r.ok)fail(synthetic && r.synthetic_failure?18:13,synthetic && r.synthetic_failure?
        "synthetic scene PSO/root-signature/vertex/depth/readback setup":"format/swapchain/private dependency setup");
    context=r.key;
    if(r.ok && (!context.device || !context.queue || !context.fence || !context.epoch))
        fail(12,"missing authenticated private timeline");
    log("release_policy=CPU-observed-GPU-completion-before-release diagnostic=1");
    if(synthetic)log("synthetic_scene id="+stereo::hex(stereo::scene_hash())+" quads="+std::to_string(stereo::scene.size())+
        " vertices="+std::to_string(stereo::vertex_count)+" near_m="+stereo::fixed(stereo::near_m)+" far_m="+stereo::fixed(stereo::far_m)+
        " depth=D3D-[0,1]-ordinary-LESS row_vector=1 world=LOCAL handedness=right +Y_up -Z_forward cull=none"
        " readback=after-completion-before-release verify=every-frame tolerance_px=2");
    return r.ok && !failed() && admission_stop.empty();
}
bool ColourOwner::completion(ImageRecord& r) {
    if(!r.submitted || !r.signalled || r.complete)return r.complete;
    auto p=call(ImageOp::sample,{r.key});
    last_sample_ms=clock.now(); // spacing measured from return, including delayed logging
    if(p.timeout)return false; // local sample window expired; no completion observation
    if(!p.ok || !same_key(p.key,r.key) || p.completed==std::numeric_limits<uint64_t>::max() ||
        p.completed<last_completed) {
        r.ambiguous=true;fail(15,"invalid/regressing/device-lost fence sample");return false;
    }
    last_completed=p.completed;
    r.complete=p.completed>=r.key.value;
    return r.complete;
}
bool ColourOwner::release(ImageRecord& r) {
    if(!r.acquired || !r.waited || r.release_attempted || (r.submitted && !r.complete))return false;
    r.release_attempted=true; // accepted failure is never retried
    auto p=call(ImageOp::release,{r.key});
    if(p.ok){r.released=true;r.acquired=false;}
    else fail(16,"image release failed; not retried");
    return r.released;
}
Projection ColourOwner::render(uint64_t frame,bool should_render) {
    if(!should_render || failed() || !admission_stop.empty())return {};
    const auto completion_deadline=std::min(Caps::admission_ms,clock.now()+FenceCaps::frame_ms);
    sample_deadline=completion_deadline;
    for(const auto& r:eyes)if(r.acquired || (r.submitted && !r.complete)) {
        fail(12,"previous frame retained; no stale fallback");return {};
    }
    eyes={};
    auto located=call(ImageOp::locate,{},true);
    if(!located.ok || !located.views_valid || !located.sample){fail(13,"invalid raw views");return {};}
    if(failed())return {};
    if(synthetic) {
        // One located sample, one immutable static scene, one matrix per eye.
        views={};pixel_views={};
        if(!b.synthetic_frame(views) || views.sample!=located.sample){fail(12,"synthetic views not joined to located sample");return {};}
        for(unsigned eye=0;eye<2;++eye) {
            stereo::Matrix cpu{};
            if(!stereo::eye_matrix(views.eyes[eye],cpu)){fail(13,"located view refused by matrix pipeline");return {};}
            if(!stereo::same_matrix(cpu,views.uploaded[eye])){fail(12,"recorded matrix differs from CPU matrix");return {};}
            if(!tally.frames_rendered)log(stereo::describe_view(eye,views.eyes[eye],cpu));
        }
    }
    for(unsigned eye=0;eye<2 && !failed();++eye) {
        auto& r=eyes[eye];r.key.frame=frame;r.key.eye=eye;
        auto acquired=call(ImageOp::acquire,{r.key},true);
        // Successful acquire owns the image even when SESSION_LOSS_PENDING.
        if(acquired.ok){r.key=acquired.key;r.acquired=true;}
        else {fail(13,"acquire failed");break;}
        if(r.key.eye!=eye || r.key.frame!=frame || !r.key.device || !r.key.queue || !r.key.fence ||
            !r.key.epoch || !r.key.resource || !r.key.rtv || !r.key.allocator || !r.key.list) {
            fail(12,"incomplete acquired image identity");break;
        }
        if(r.key.device!=context.device || r.key.queue!=context.queue || r.key.fence!=context.fence || r.key.epoch!=context.epoch) {
            fail(12,"acquired image does not join authenticated private timeline");break;
        }
        if(failed())break;
        auto waited=call(ImageOp::wait,{r.key,0});
        if(waited.ok && waited.timeout && !failed())waited=call(ImageOp::wait,{r.key,1000000});
        if(waited.ok && !waited.timeout && same_key(waited.key,r.key))r.waited=true;
        else {fail(13,"wait not ready; retain acquisition, no write/release");break;}
        if(failed())break;
        auto recorded=call(ImageOp::record,{r.key},true);
        if(!recorded.ok){fail(synthetic && recorded.synthetic_failure?18:14,"record failed");break;}
        r.recorded=true;
        if(synthetic) {
            // Unproven state restore or copy: never Close/Execute; known unsubmitted.
            if(!recorded.restored || !recorded.copied){fail(21,"image state restore/readback copy not proven; no submit");break;}
            r.copied=true;
        }
        if(failed())break;
        if(!call(ImageOp::close,{r.key},true).ok){fail(14,"actual Close HRESULT failed");break;}
        r.closed=true;
        if(failed())break;
        r.key.value=++issued;
        // Execute is void; any uncertain dispatch/join may already reference
        // the image. Set submitted BEFORE crossing the foreign boundary.
        r.submitted=true;
        auto executed=call(ImageOp::execute,{r.key},true);
        r.submitted=executed.dispatch_possible;
        if(!executed.ok || !r.submitted){r.ambiguous=r.submitted;fail(14,"Execute refused or join ambiguous");break;}
        if(failed())break;
        auto signal=call(ImageOp::signal,{r.key});
        r.signalled=signal.ok && same_key(signal.key,r.key);
        if(!r.signalled){r.ambiguous=true;fail(15,"Signal failed/ambiguous");break;}
        if(failed())break;
        // CPU polling is a deliberate diagnostic policy, not a queue wait.
        // One 100 ms window (D-207; was 20 ms) shared by both eyes; samples at least 1 ms apart,
        // including the boundary between eyes. Never sample at/after its end.
        while(!failed() && clock.now()<completion_deadline && stop().empty() && !gate->closed()) {
            const auto next=last_sample_ms+FenceCaps::sample_ms;
            if(clock.now()<next) {
                clock.sleep(std::min(next,completion_deadline)-clock.now());continue;
            }
            if(completion(r))break;
        }
        if(!stop().empty() || !frame_admitted(clock.now()) || gate->closed()) {
            admission_stop=stop();if(admission_stop.empty())admission_stop=frame_admitted(clock.now())?"stop requested":"time cap";
            closed_admission=true;gate->close();log("image_stop_disposition="+admission_stop);break;
        }
        if(!r.complete){fail(15,"GPU completion pending; quarantine");break;}
        if(failed())break;
        // Card (d): Map the completed copy while the image is still held.
        if(synthetic && !readback(r))break;
        if(!release(r))break;
    }
    if(failed() || !admission_stop.empty())return {};
    for(const auto& r:eyes)if(!r.complete || !r.released)return {};
    if(synthetic && !verify_frame(frame))return {};
    return {located.sample,frame};
}
bool ColourOwner::readback(ImageRecord& r) {
    if(auto why=stereo::readback_refusal(r.acquired,r.submitted,r.complete,r.copied,r.released)){fail(19,why);return false;}
    auto p=call(ImageOp::readback,{r.key});
    if(!p.ok || failed()){fail(19,"readback Map/copy failed");return false;}
    auto v=b.readback_pixels(r.key.eye);
    if(!v.data){fail(19,"readback produced no CPU pixels");return false;}
    pixel_views[r.key.eye]=v;r.read=true;
    return true;
}
bool ColourOwner::verify_frame(uint64_t frame) {
    ++tally.frames_rendered;
    const bool detail=tally.frames_rendered==1;
    std::array<stereo::EyeReport,2> report;
    for(unsigned eye=0;eye<2;++eye)report[eye]=stereo::verify_eye(views.eyes[eye],views.uploaded[eye],pixel_views[eye]);
    const auto pair=stereo::verify_pair(views,report[0],report[1]);
    for(unsigned eye=0;eye<2;++eye) {
        log(stereo::describe_eye(frame,eye,report[eye]));
        if(detail || !pair.pass)for(const auto& m:report[eye].markers)log(stereo::describe_marker(eye,m));
    }
    log(stereo::describe_pair(frame,pair));
    pixel_views={};
    if(!pair.pass) {
        ++tally.failures;
        if(tally.first_failure.empty())tally.first_failure="frame "+std::to_string(frame)+": "+
            (!report[0].pass?"eye 0 "+report[0].reason:!report[1].pass?"eye 1 "+report[1].reason:pair.reason);
        fail(20,"synthetic stereo pixel verification failed");return false;
    }
    ++tally.frames_verified;
    return true;
}
std::string ColourOwner::stereo_verdict(bool run_success) const {
    const bool pass=run_success && !failed() && !tally.failures && tally.frames_verified>=1 &&
        tally.frames_verified==tally.frames_rendered;
    std::string reason;
    if(pass)reason="verified_frames="+std::to_string(tally.frames_verified)+"/"+std::to_string(tally.frames_rendered)+
        " both eyes within 2 px and relative disparity matches located IPD";
    else if(!tally.first_failure.empty())reason=tally.first_failure;
    else if(failed())reason="image failure code "+std::to_string(error)+" before verification";
    else if(!tally.frames_verified)reason="no synthetic frame was rendered and verified";
    else reason="run not successful";
    return std::string("SYNTHETIC_STEREO_RESULT=")+(pass?"PASS":"FAIL")+" reason="+reason+
        " frames_rendered="+std::to_string(tally.frames_rendered)+" frames_verified="+std::to_string(tally.frames_verified);
}
bool ColourOwner::retire(int64_t deadline) {
    closed_admission=true;
    gate->close();
    if(!setup_attempted || destroyed)return true;
    operation_deadline=std::min(operation_deadline,std::min(deadline,Caps::absolute_ms));
    deadline=std::min(operation_deadline,clock.now()+Caps::cleanup_ms);
    operation_deadline=deadline;
    sample_deadline=deadline;
    b.begin_retirement(operation_deadline);
    // Never manufacture readiness or retry an accepted release. Late valid
    // completion may discharge quarantine, but no new rendering is admitted.
    for(auto& r:eyes) {
        if(r.ambiguous)return false; // no authenticated timeline remains
        while(r.submitted && r.signalled && !r.complete && clock.now()<deadline) {
            const auto next=last_sample_ms+FenceCaps::retire_sample_ms;
            if(clock.now()<next) {
                clock.sleep(std::min(Caps::poll_ms,std::min(next,deadline)-clock.now()));continue;
            }
            if(completion(r))break;
            if(r.ambiguous)break;
        }
        if(r.submitted && !r.complete)return false;
        if(r.acquired) {
            if(!r.waited || clock.now()>=deadline || !release(r))return false;
        }
        if(r.release_attempted && !r.released)return false;
    }
    if(clock.now()>=deadline)return false;
    auto p=call(ImageOp::destroy,{});
    if(!p.ok){fail(17,"swapchain/dependency destruction failed");return false;}
    destroyed=true;return true;
}
}
