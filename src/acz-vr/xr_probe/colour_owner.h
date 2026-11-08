#pragma once
#include "probe_core.h"
#include "synthetic_scene.h"
#include <array>
#include <vector>

namespace acz::probe {
struct FenceCaps {
    static constexpr int64_t frame_ms=100, sample_ms=1, retire_sample_ms=100;
};
// Standalone XR owner only. No dependency on the game GPU-copy ledger.
// readback (card d only) maps the probe-owned readback buffer after proven
// completion and before release; a1/a2 never issue it.
enum class ImageOp { setup, locate, acquire, wait, record, close, execute, signal,
    sample, release, destroy, readback, count };
// Every receipt is an observation, not an instruction. The owner validates joins.
struct ImageKey {
    uint64_t device=0, queue=0, fence=0, epoch=0, resource=0, rtv=0, allocator=0, list=0;
    uint64_t frame=0, value=0; unsigned eye=0, index=0;
};
inline bool same_key(const ImageKey& a,const ImageKey& b) {
    return a.device==b.device && a.queue==b.queue && a.fence==b.fence && a.epoch==b.epoch &&
        a.resource==b.resource && a.rtv==b.rtv && a.allocator==b.allocator && a.list==b.list &&
        a.frame==b.frame && a.value==b.value && a.eye==b.eye && a.index==b.index;
}
struct ImageRequest { ImageKey key; int64_t wait_ns=0; };
struct ImageReply {
    bool ok=false, loss=false, timeout=false;
    ImageKey key;
    uint64_t completed=0, sample=0; // sample identifies this frame's immutable raw views
    bool views_valid=false;
    bool dispatch_possible=false; // Execute boundary may have referenced this image
    // Card (d) receipts only. restored: the recorded image-state sequence ends
    // in RENDER_TARGET; copied: CopyTextureRegion to the private readback buffer
    // was recorded before Close. synthetic_failure: probe-owned scene resource fault.
    bool restored=false, copied=false, synthetic_failure=false;
};
struct Projection { uint64_t sample=0, frame=0; };
struct ColourBackend {
    virtual ~ColourBackend()=default; // deliberately no foreign cleanup
    virtual ImageReply image(ImageOp,const ImageRequest&)=0;
    virtual void begin_retirement(int64_t) {} // policy publication only; no foreign calls
    // Card (d) only. Views/matrices captured at the last successful locate and
    // the CPU copy of the last readback. Portable data; no foreign calls.
    virtual bool synthetic_frame(stereo::FrameViews&) {return false;}
    virtual stereo::PixelView readback_pixels(unsigned) {return {};}
};
// Opaque alpha=1, RGB=linear 0.02. Only a compatible sRGB RTV encodes once.
inline constexpr std::array<float,4> dim_clear{0.02f,0.02f,0.02f,1.0f};
static_assert(dim_clear[0]>=0.01f && dim_clear[0]<=0.05f && dim_clear[1]>=0.01f && dim_clear[1]<=0.05f &&
    dim_clear[2]>=0.01f && dim_clear[2]<=0.05f && dim_clear[3]==1.0f,"finite dim linear RGB and opaque alpha");
struct ImageRecord {
    ImageKey key;
    bool acquired=false, waited=false, recorded=false, closed=false;
    bool submitted=false, signalled=false, complete=false, release_attempted=false, released=false;
    bool ambiguous=false;
    bool copied=false, read=false; // card (d) only
};
// Card (d) verification tally; PASS needs every rendered frame verified.
struct StereoTally {
    unsigned frames_rendered=0, frames_verified=0, failures=0;
    std::string first_failure;
};
class ColourOwner {
public:
    ColourOwner(ColourBackend&,Clock&,Log,std::function<std::string()>,AdmissionGate* gate=nullptr,bool synthetic=false);
    ColourOwner(const ColourOwner&)=delete;
    ColourOwner& operator=(const ColourOwner&)=delete;
    bool setup();
    Projection render(uint64_t frame,bool should_render);
    bool retire(int64_t deadline);
    bool failed() const {return error!=0;}
    int failure_code() const {return error;}
    bool loss_seen() const {return loss;}
    const std::string& stop_reason() const {return admission_stop;}
    const std::array<ImageRecord,2>& records() const {return eyes;}
    bool synthetic_mode() const {return synthetic;}
    const StereoTally& stereo_tally() const {return tally;}
    std::string stereo_verdict(bool run_success) const;
private:
    bool readback(ImageRecord&);
    bool verify_frame(uint64_t frame);
    ImageReply call(ImageOp,const ImageRequest&,bool admission=false);
    bool release(ImageRecord&);
    bool completion(ImageRecord&);
    void fail(int,const char*);
    ColourBackend& b; Clock& clock; Log log; std::function<std::string()> stop;
    std::array<ImageRecord,2> eyes{};
    ImageKey context{};
    uint64_t issued=0, last_completed=0;
    int error=0; bool loss=false, setup_attempted=false, destroyed=false, closed_admission=false;
    int64_t operation_deadline=Caps::absolute_ms;
    int64_t last_sample_ms=-FenceCaps::retire_sample_ms;
    int64_t sample_deadline=Caps::absolute_ms;
    std::string admission_stop;
    AdmissionGate local_gate; AdmissionGate* gate;
    bool synthetic=false;
    stereo::FrameViews views{};
    std::array<stereo::PixelView,2> pixel_views{};
    StereoTally tally{};
};
}
