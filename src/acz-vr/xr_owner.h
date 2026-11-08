#pragma once
// xr_owner.h -- in-proxy XR frame owner for ladder cards (b)/(c), OFFLINE ONLY.
//
// Scope (docs/proposals/xr-stage-design.md sections 2-3, cards (b)/(c);
// Astra issues 25/26/28):
//   * One dedicated owner thread performs EVERY runtime call: instance, system,
//     graphics-binding checks, session, LOCAL + identity VIEW spaces, bounded
//     event polling, BeginSession, wait/begin/locate/end with ZERO layers,
//     requested exit, EndSession and destruction. No call happens from a game
//     hook, DllMain, the loader lock or under any lock shared with a hook.
//   * The Runtime interface below has no swapchain, image, layer, command or
//     fence operation at all: zero layers is structural, not a flag.
//   * Default OFF. With Config::enabled == false, Owner::run() and launch()
//     make zero runtime calls and create no thread.
//   * Head/eye poses are published as owned, fixed-size, immutable value
//     snapshots through a try-only slot: neither the writer nor any reader
//     ever waits; contention drops the publication (writer) or the read
//     (reader). Stop, loss or failure retracts the slot.
//   * Exit is xrRequestExitSession -> STOPPING -> xrEndSession -> destroy.
//     A session in LOSS_PENDING is destroyed without the exit handshake.
//     Handles are never destroyed while another call is in flight (only the
//     owner thread calls the runtime, so this is structural), and the
//     watchdog never calls the runtime: it can only latch a policy failure
//     and request a stop. It cannot cancel a blocked foreign call.
//
// Nothing here is wired into the proxy. See J-xr-owner-findings.md.
// J-xr-stereo-output (additive, default OFF): Config::continuous and an
// optional FrameConsumer that may turn a begun frame into ONE projection
// layer; with no consumer the frame loop is unchanged (zero layers). The
// proxy wiring lives in xr_stereo_proxy.cpp (J-xr-stereo-output-findings.md).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace acz::xr_owner {

// ---------------------------------------------------------------- policy ---
// Ceilings from card (c): 15 s absolute (cleanup reserve inside it), 5 s
// startup, 2 s cleanup, 250 ms foreign-call progress, 120 begun frames,
// at most 16 events per poll batch. Limits may be tightened, never widened.
struct Limits {
    int64_t absolute_ms = 15000;
    int64_t startup_ms = 5000;
    int64_t cleanup_ms = 2000;
    int64_t progress_ms = 250;
    int64_t poll_ms = 20;
    unsigned max_frames = 120;
    unsigned events_per_batch = 16;
    // VSEARLY (docs/world-render/vsearly.md; 0 = off = the 250 ms rule exactly as before): the progress limit of the FRAMES phase after the
    // first frame for a session that also runs through the game's menus and loads (multi-second GPU-queue stalls, test 19: ~2 s). The only
    // widening Limits allows, capped at kFrameStallCeilingMs; startup, READY, cleanup and the first frame keep their budgets.
    int64_t frame_stall_ms = 0;
    static constexpr int64_t kFrameStallCeilingMs = 10000;
    int64_t frame_progress_ms() const noexcept { return frame_stall_ms > 0 ? frame_stall_ms : progress_ms; }
    int64_t admission_ms() const noexcept { return absolute_ms - cleanup_ms; }
    bool valid() const noexcept;
};

// The ONLY activation switch. Default OFF. A future runtime_config key would
// construct this; no parser change is part of this job.
// `continuous` (J-xr-stereo-output, additive): the in-game consumer mode. The
// 120-frame cap, the 13 s admission close and the 15 s absolute cap do NOT
// apply; the 5 s startup, 250 ms progress and 2 s cleanup rules still do. The
// call-record array keeps the first record_capacity calls and counts the rest
// (records_overflow) instead of failing. Default false: ladder behaviour.
struct Config {
    bool enabled = false;
    bool continuous = false;
};

// ------------------------------------------------------- runtime facade ---
// Result categories, mapped from XrResult by the real backend.
enum class Rc : uint8_t {
    ok,
    loss_pending,      // XR_SESSION_LOSS_PENDING: qualified success (side effects happened)
    frame_discarded,   // XR_FRAME_DISCARDED from xrBeginFrame: frame IS begun
    event_unavailable, // XR_EVENT_UNAVAILABLE from xrPollEvent
    error,
    session_lost,      // XR_ERROR_SESSION_LOST
    instance_lost,     // XR_ERROR_INSTANCE_LOST
    refused,           // owner refused to make the call (deadline/admission)
};
inline bool succeeded(Rc rc) noexcept {
    return rc == Rc::ok || rc == Rc::loss_pending || rc == Rc::frame_discarded || rc == Rc::event_unavailable;
}

enum class SessionState : uint8_t { unknown, idle, ready, synchronized, visible, focused, stopping, loss_pending, exiting };
const char* session_state_name(SessionState) noexcept; // XR-START: "IDLE", "READY", ... (logged next to the number)
enum class EventType : uint8_t { none, session_state, instance_loss_pending, events_lost, reference_space_change, other };
enum class SpaceType : uint8_t { view, local, stage, other };

struct Event {
    EventType type = EventType::none;
    SessionState state = SessionState::unknown;
    bool same_session = true;          // event names the owner's session
    SpaceType space = SpaceType::other; // reference_space_change only
    int64_t change_time = 0;            // XrTime (ns)
    int64_t event_time = 0;             // session-state XrTime (ns), additive trailing field
};

// OpenXR numeric values; the real backend static_asserts them.
constexpr uint32_t blend_opaque = 1;
constexpr unsigned max_blend_modes = 8;

struct SystemFacts {
    bool hmd_system = false;
    bool primary_stereo = false;
    uint32_t view_count = 0;
    uint32_t blend_count = 0; // count reported by the runtime (may exceed capacity)
    std::array<uint32_t, max_blend_modes> blend_modes{};
};
struct GraphicsRequirements { uint32_t luid_low = 0; int32_t luid_high = 0; uint32_t min_feature_level = 0; };
// Facts about the game's D3D12 device/queue the backend was given (owned refs).
struct BindingFacts {
    bool device_present = false, queue_present = false;
    uint32_t luid_low = 0; int32_t luid_high = 0;
    uint32_t max_feature_level = 0;
    bool queue_direct = false;   // D3D12_COMMAND_LIST_TYPE_DIRECT
    bool queue_on_device = false; // queue->GetDevice == device
};
// XR-START diagnostics (additive): which startup gate raised Cause::compatibility.
// system = HMD/stereo/view-count/blend gate, binding = compatible(req, bind),
// spaces = LOCAL/VIEW reference-space gate.
enum class CompatStage : uint8_t { none, system, binding, spaces };
const char* compat_stage_name(CompatStage) noexcept;
// Bitmask of the compatible() terms that failed (0 = all terms passed).
enum CompatMismatch : unsigned {
    compat_device_absent = 1u << 0, compat_queue_absent = 1u << 1, compat_min_fl_zero = 1u << 2,
    compat_luid_low = 1u << 3, compat_luid_high = 1u << 4, compat_feature_level = 1u << 5,
    compat_queue_not_direct = 1u << 6, compat_queue_off_device = 1u << 7,
};
struct SpaceFacts { bool local = false, view = false; };
struct FrameTiming { int64_t predicted_time = 0, predicted_period = 0; bool should_render = false; };

// Location flag bits: identical values for XrViewStateFlags and XrSpaceLocationFlags.
constexpr uint64_t orientation_valid = 1, position_valid = 2, orientation_tracked = 4, position_tracked = 8;
constexpr uint64_t valid_bits = orientation_valid | position_valid;
constexpr uint64_t tracked_bits = orientation_tracked | position_tracked;

struct Pose { float position[3] = {0, 0, 0}; float orientation[4] = {0, 0, 0, 1}; }; // xyz, xyzw
struct Fov { float left = 0, right = 0, up = 0, down = 0; };
struct EyeSample { Pose pose; Fov fov; };
struct ViewSample { uint32_t view_count = 0; uint64_t view_state_flags = 0; std::array<EyeSample, 2> eyes{}; };
struct HeadSample { uint64_t location_flags = 0; Pose pose; };
// J-xr-stereo-output (additive): one projection layer of two views in LOCAL.
// The backend supplies the swapchain per eye and its full image rectangle;
// poses/FOV are raw (applied exactly once by the runtime).
// Revision 3 (additive): `source_alpha` submits the projection with
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT (premultiplied: no
// UNPREMULTIPLIED bit); `quad` adds a world quad layer (both eyes, LOCAL) as
// layer 0, BEHIND the projection layer (layer 1).
struct ProjectionLayer {
    std::array<EyeSample, 2> views{};
    bool source_alpha = false;
    bool quad = false;
    Pose quad_pose;
    float quad_width = 0, quad_height = 0;
    // J-hud-consumer (additive, default false = the existing 1/2-layer submission):
    // `hud` adds a HUD quad (both eyes, premultiplied SOURCE_ALPHA blend) AFTER
    // (on top of) the projection layer. `hud_view_space` selects VIEW instead of
    // LOCAL for the head-locked fallback.
    bool hud = false;
    bool hud_view_space = false;
    Pose hud_pose;
    float hud_width = 0, hud_height = 0;
    // J-feather (additive, default false = the existing OPAQUE world quad): submit the
    // world quad with SOURCE_ALPHA (premultiplied, no UNPREMULTIPLIED bit). Set only
    // while xr_edge_feather > 0 makes the world image fade out toward its border.
    bool quad_source_alpha = false;
    // J-hud-markers (additive, default false = the existing submission): `markers` adds a quad (both eyes, LOCAL,
    // premultiplied SOURCE_ALPHA) with the world quad's pose/size, submitted AFTER the world quad and BEFORE the projection.
    bool markers = false;
    Pose marker_pose;
    float marker_width = 0, marker_height = 0;
    // J-world-in-projection (key xr_world_in_projection, default false = the world quad layer as
    // before): the world image was composited INTO the per-eye projection images under the
    // cockpit, so `quad` stays false and the projection is submitted opaque (source_alpha false).
    // Informational for logs; no layer other than the projection (and the HUD) is submitted.
    bool world_composite = false;
    // VSCREEN (xr_terrain_stage vscreen:1, default false = every submission above unchanged): submit ONLY one opaque quad (both eyes, LOCAL)
    // from the HUD quad swapchain with this pose/size: the game's own screen outside flight (docs/world-render/vscreen.md). No projection layer.
    bool screen_only = false;
    Pose screen_pose;
    float screen_width = 0, screen_height = 0;
    // BRIEF3D (brief3d:1, default false): with screen_only, ALSO submit the projection layer (eye swapchains, `views`, premultiplied SOURCE_ALPHA)
    // AFTER (on top of) the screen quad: the briefing map table (docs/world-render/brief3d.md).
    bool table = false;
    // UI3D (ui3d:1, default false): with table, the projection is a UI3D menu frame: submitted OPAQUE (no SOURCE_ALPHA), fully covering the quad
    // (docs/world-render/ui-3d.md 4 item 4).
    bool table_opaque = false;
    // UI3D 2026-10-30b (u3q:1, default; docs/world-render/ui-3d.md 11): SteamVR draws QUAD layers above projection layers whatever the list order
    // (hypothesis=steamvr-draws-quad-layers-above-projection), so with table: an OPAQUE slot is submitted ALONE (no screen quad underneath it) and a
    // premultiplied table is submitted as one quad per eye (after the screen quad, same quad class = list order holds). false = the old list.
    bool table_front = false;
    // Headset test 8 (2026-11-01, docs/world-render/brief3d-worldlock.md): with table_front the table is a quad per eye at a distance along the view
    // axis. A quad whose plane is NOT at the depth of the content shifts against the world by (1/content - 1/quad) x (head error) as the head moves;
    // the briefing table is ~0.75 m away and the default plane is 1.5 m. > 0 = the plane depth for that eye (the table's own depth; the consumer
    // computes it), 0 = kTableQuadDistanceM (UI3D menus, hangar: their content is at the quad's own distance).
    float table_quad_distance_m[2] = {0.0f, 0.0f};
    // Headset test 9 (b3p:1, docs/world-render/brief3d-worldlock.md): with table_front, true = both eyes' table quads use this WORLD-FIXED pose
    // and size (LOCAL; the plane each eye's picture was drawn onto, `views` = those plane views) instead of a plane along each eye's view axis.
    bool table_quad_fixed = false;
    Pose table_quad_pose;
    float table_quad_width = 0, table_quad_height = 0;
};

// XR-START diagnostics (additive, docs/world-render/xr-start.md). The raw XrResult of
// the last OpenXR function a Runtime method called, kept per CallRecord. kNoXrResult =
// no OpenXR function result (a D3D12 op, a proxy-side refusal or an owner deadline).
constexpr int32_t kNoXrResult = INT32_MIN;
// Name of a core OpenXR 1.0 XrResult ("XR_ERROR_FORM_FACTOR_UNAVAILABLE"), "none" for
// kNoXrResult, "XR_UNKNOWN_SUCCESS"/"XR_UNKNOWN_FAILURE" for codes outside the table.
const char* xr_result_name(int32_t) noexcept;
// Facts the real backend learns during create_instance / query_system (xrGetInstanceProperties,
// xrGetSystemProperties). Read ONLY on the owner thread (e.g. by a CallObserver).
struct RuntimeFacts {
    bool instance_props = false;
    char runtime_name[128] = {};
    uint64_t runtime_version = 0;        // XrVersion: major<<48 | minor<<32 | patch
    bool system_props = false;
    uint64_t system_id = 0;
    uint32_t vendor_id = 0;
    char system_name[256] = {};
    uint32_t form_factor = 0;            // requested XrFormFactor (1 = HEAD_MOUNTED_DISPLAY)
    uint32_t max_layers = 0;
    bool orientation_tracking = false, position_tracking = false;
    const char* refusal = nullptr;       // static text when a method failed without an XrResult
};

// Every method is a potentially blocking, non-cancellable foreign call made
// ONLY by the owner thread. Implementations must not call back into Owner.
struct Runtime {
    // XR-START (additive, no foreign call): returns and clears the raw XrResult of the last
    // OpenXR function called by any method (kNoXrResult = none since the last take).
    virtual int32_t take_last_xr_result() { return kNoXrResult; }
    virtual bool runtime_facts(RuntimeFacts&) const { return false; }
    virtual ~Runtime() = default;
    virtual Rc create_instance() = 0;
    virtual Rc query_system(SystemFacts&) = 0;
    virtual Rc graphics_requirements(GraphicsRequirements&) = 0;
    virtual Rc query_binding(BindingFacts&) = 0;
    virtual Rc create_session() = 0;
    virtual Rc query_spaces(SpaceFacts&) = 0;
    virtual Rc create_local_space() = 0;
    virtual Rc create_view_space() = 0; // identity-offset VIEW
    virtual Rc poll_event(Event&) = 0;
    virtual Rc begin_session() = 0;
    virtual Rc wait_frame(FrameTiming&) = 0;
    virtual Rc begin_frame() = 0;
    virtual Rc locate_views(int64_t time, ViewSample&) = 0;  // PRIMARY_STEREO in LOCAL
    virtual Rc locate_head(int64_t time, HeadSample&) = 0;   // VIEW located in LOCAL
    virtual Rc end_frame_zero_layers(int64_t display_time, uint32_t blend_mode) = 0;
    // Additive: only an owner with a FrameConsumer ever calls this, and only
    // after the consumer released both eyes' images for this frame (or reuses
    // the previous frame's released images). Default: refused (no layer path).
    virtual Rc end_frame_projection(int64_t /*display_time*/, uint32_t /*blend_mode*/, const ProjectionLayer&) { return Rc::refused; }
    virtual bool end_lateness_ms(double&) const { return false; } // cached, no foreign call
    virtual Rc request_exit() = 0;
    virtual Rc end_session() = 0;
    virtual Rc destroy_view_space() = 0;
    virtual Rc destroy_local_space() = 0;
    virtual Rc destroy_session() = 0;
    virtual Rc release_binding() = 0; // drop the owned device/queue references
    virtual Rc destroy_instance() = 0;
};

// Monotonic milliseconds. sleep_ms receives values in [0, Limits::poll_ms].
struct Clock {
    virtual ~Clock() = default;
    virtual int64_t now_ms() = 0;
    virtual void sleep_ms(int64_t) = 0;
};

// ------------------------------------------------------------- snapshots ---
// Owned, fixed-size value. Eye poses are the RAW LocateViews results in LOCAL
// and already include head motion and IPD: never compose `head` onto them.
// `head` is the identity VIEW space located in the same LOCAL space at the
// same predicted time: the runtime's viewer origin, not an anatomical centre.
struct PoseSnapshot {
    uint64_t sequence = 0;          // 1-based publication serial
    uint64_t frame_index = 0;       // owner's begun-frame serial
    int64_t predicted_time = 0, predicted_period = 0;
    bool should_render = false;     // informational; readout does not depend on it
    uint32_t session_generation = 1;
    uint32_t space_generation = 0;  // LOCAL generation honouring changeTime
    uint32_t view_count = 0;
    uint64_t view_state_flags = 0;  // raw
    std::array<EyeSample, 2> eyes{};
    uint64_t head_flags = 0;        // raw
    Pose head;
    bool views_located = false, head_located = false;
    bool eyes_finite = false, head_finite = false;
    bool eyes_valid() const noexcept { return views_located && view_count == 2 && eyes_finite && (view_state_flags & valid_bits) == valid_bits; }
    bool eyes_tracked() const noexcept { return eyes_valid() && (view_state_flags & tracked_bits) == tracked_bits; }
    bool head_valid() const noexcept { return head_located && head_finite && (head_flags & valid_bits) == valid_bits; }
    bool head_tracked() const noexcept { return head_valid() && (head_flags & tracked_bits) == tracked_bits; }
    // Card (c) head-centre acceptance: valid AND tracked, finite, normalized.
    bool head_centre_qualified() const noexcept { return head_tracked(); }
};

// Single-slot try-only mailbox. Writers and readers never wait, sleep, spin
// or allocate. A contended publish is dropped (counted); a contended read
// returns false. retract() is an unconditional atomic store.
class SnapshotSlot {
public:
    bool try_publish(const PoseSnapshot&) noexcept;
    bool try_read(PoseSnapshot& out) const noexcept;
    void retract() noexcept { live_.store(false, std::memory_order_release); }
    bool live() const noexcept { return live_.load(std::memory_order_acquire); }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    mutable std::atomic<bool> busy_{false};
    std::atomic<bool> live_{false};
    std::atomic<uint64_t> dropped_{0};
    PoseSnapshot value_{};
};

// ----------------------------------------------------------------- owner ---
enum class Phase : uint8_t { off, startup, await_ready, frames, cleanup, done };
enum class Cause : uint8_t {
    none, disabled, invalid_limits, already_used, startup_call, compatibility,
    ready_timeout, event_flood, events_lost, session_mismatch, loss, exiting,
    frame_call, end_frame_failed, startup_deadline, progress_deadline,
    absolute_cap, cleanup_deadline, teardown_incomplete, space_change_overlap,
    exception, records_lost, operator_stop_before_lifecycle,
    consumer, // additive: the FrameConsumer refused setup or disabled itself
};
const char* cause_name(Cause) noexcept;
enum class StopReason : uint8_t { none, requested, time_cap, frame_cap, runtime_stopping, failure };
const char* stop_name(StopReason) noexcept;
enum class Op : uint8_t {
    create_instance, query_system, graphics_requirements, query_binding, create_session,
    query_spaces, create_local_space, create_view_space, poll_event, begin_session,
    wait_frame, begin_frame, locate_views, locate_head, end_frame, request_exit,
    end_session, destroy_view_space, destroy_local_space, destroy_session,
    release_binding, destroy_instance,
    // Additive (J-xr-stereo-output): consumer foreign calls through the Gate.
    swapchain_query, create_swapchain, acquire_image, wait_image, release_image,
    destroy_swapchain, end_frame_projection, d3d_setup, d3d_bind, d3d_record,
    d3d_close, d3d_gpu_wait, d3d_execute, d3d_signal, d3d_sample, d3d_destroy,
    count,
};
const char* op_name(Op) noexcept;

struct CallRecord { Op op = Op::count; Rc rc = Rc::ok; int64_t entry_ms = 0, exit_ms = 0; int32_t xr = kNoXrResult; };
constexpr unsigned record_capacity = 4096;

struct Report {
    bool complete = false;         // run() returned
    bool success = false;          // orderly, no sticky failure/loss, clean, balanced
    bool clean = false;            // every created handle retired inside its deadline
    bool session_begun = false;
    bool loss_observed = false;    // sticky: qualified LOSS_PENDING, loss event or lost error
    bool failure_observed = false; // sticky
    bool policy_failure = false;   // sticky: deadline/watchdog
    Cause first_cause = Cause::none;
    StopReason stop = StopReason::none;
    uint32_t selected_blend = 0;
    unsigned waited = 0, begun = 0, ended = 0, discarded = 0, waited_unbegun = 0;
    unsigned located = 0, head_qualified = 0, published = 0, publish_dropped = 0;
    unsigned events = 0, space_changes = 0;
    unsigned recenters = 0;        // software recenters honoured (request_recenter -> a new LOCAL generation)
    unsigned runtime_calls = 0, records_lost = 0;
    // Handles still owned after run(): non-zero means retained, not leaked by accident.
    bool instance_held = false, session_held = false, session_running = false;
    bool local_space_held = false, view_space_held = false, binding_held = false;
    // Additive (consumer / continuous mode).
    unsigned projection_layers = 0; // successful xrEndFrame calls that carried the layer
    bool consumer_attached = false, consumer_retired = false;
    uint64_t records_overflow = 0;  // continuous mode: calls beyond record_capacity
    // First begun frame (continuous mode budget: part of startup, see below).
    int64_t first_wait_ms = -1, first_begin_ms = -1, first_end_ms = -1, first_frame_done_ms = -1;
    // Additive (XR-START diagnostics): set when a compatibility gate refused.
    // compat_req/compat_bind hold every compared field of both sides and are
    // meaningful only for compat_stage == binding.
    CompatStage compat_stage = CompatStage::none;
    GraphicsRequirements compat_req{};
    BindingFacts compat_bind{};
};

// Additive: optional process-wide bracket around EVERY runtime/consumer call
// the owner makes (entered on the owner thread right before the foreign call,
// left right after it returns or throws). `window` is false for calls that
// mostly block (xrWaitFrame, xrPollEvent, locate, fence samples). The proxy
// uses it for its creation scope (J-xr-stereo-output). Null = none.
using CallBracket = void (*)(bool enter, bool window) noexcept;
void set_call_bracket(CallBracket) noexcept;

// XR-START (additive, default none): per-owner observer of the start path. Called on the
// owner thread right after a recorded call returns (or is refused by a deadline) while the
// owner is in startup / await_ready, inside the first begun frame (continuous mode) or in
// cleanup; xrPollEvent calls are reported only when they fail. Must not block or re-enter
// the Owner; exceptions are swallowed. `first_frame` = the call belongs to frame 1.
struct CallObserver {
    virtual ~CallObserver() = default;
    virtual void call(const CallRecord&, Phase, bool first_frame) noexcept = 0;
};

// ------------------------------------------------- consumer (additive) ---
// Owner-thread services handed to a FrameConsumer. Every foreign call the
// consumer makes (XR swapchain or D3D12) goes through call(): it is recorded,
// refused past the phase deadline, progress-checked (250 ms) and a qualified
// LOSS_PENDING / lost result latches the owner's sticky loss veto.
class Gate {
public:
    virtual ~Gate() = default;
    template <class F> Rc call(Op op, F&& fn) {
        using T = std::remove_reference_t<F>;
        return invoke(op, [](void* p) -> Rc { return (*static_cast<T*>(p))(); }, const_cast<void*>(static_cast<const void*>(&fn)));
    }
    virtual int64_t now_ms() = 0;     // owner elapsed time
    // Startup deadline in the same elapsed-time domain as now_ms(). Unknown
    // gates grant no startup budget; OwnerGate exposes the actual configured limit.
    virtual int64_t startup_deadline_ms() const noexcept { return 0; }
    virtual void sleep_ms(int64_t) = 0; // clamped to [0, poll_ms]
    virtual bool lost() const = 0;    // sticky loss (qualified or hard)
    virtual bool stopping() const = 0; // stop requested or policy failure latched
protected:
    virtual Rc invoke(Op, Rc (*)(void*), void*) = 0;
};
struct FrameInfo {
    uint64_t frame_index = 0;   // owner's begun-frame serial
    FrameTiming timing;
    bool discarded = false;     // xrBeginFrame returned XR_FRAME_DISCARDED
    bool views_valid = false;   // located, two views, valid flags, finite
    ViewSample views;           // raw located views of THIS frame
    int64_t wait_ms = 0;        // duration of this frame's xrWaitFrame call
    int64_t begin_ms = 0;       // duration of this frame's xrBeginFrame call
    bool views_tracked = false; // views_valid AND orientation+position TRACKED bits
    uint32_t space_generation = 0; // LOCAL space generation of this frame (honours changeTime)
};
struct LayerPlan { bool submit = false; ProjectionLayer layer; };
// Called ONLY on the owner thread. Never re-enters the Owner.
struct FrameConsumer {
    virtual ~FrameConsumer() = default;
    // Startup, after the VIEW space and before READY/BeginSession. One attempt.
    virtual bool setup(Gate&) = 0;
    // Between locate and end of a begun frame. Fill `plan` to submit a layer.
    virtual void frame(Gate&, const FrameInfo&, LayerPlan&) = 0;
    virtual void ended(Gate&, Rc end_rc, bool projection, int64_t end_ms) = 0;
    virtual void session_event(Gate&, const Event&) {}
    virtual void end_diagnostics(int64_t /*begin-to-end ms*/, bool /*late available*/, double /*late ms*/) {}
    // Cleanup, before the exit handshake. false = some image/GPU work may still
    // be referenced: the owner then retains the session and every parent.
    virtual bool retire(Gate&) = 0;
    virtual bool failed() const noexcept = 0;
    virtual void finished(const Report&) noexcept {}
};
// NULL-HEAD (additive, TEST ONLY, default none; null_head.h, docs/world-render/null-head-look.md): called ONLY on the owner thread, once per begun
// frame right after locate_views/locate_head and BEFORE the snapshot publication and the consumer, with that frame's LOCAL space generation. It may
// rewrite the located views/head (a synthetic head turn) so every pose reader and the submitted layers see one consistent head. Must not block,
// call the runtime or re-enter the Owner. `views_ok` / `head_ok` = the locate call succeeded.
struct PoseFilter {
    virtual ~PoseFilter() = default;
    virtual void apply(int64_t now_ms, uint32_t space_generation, bool views_ok, ViewSample&, bool head_ok, HeadSample&) noexcept = 0;
};

// Starts the owner thread plus a watchdog thread (both detached, both holding
// the shared_ptr). Refuses without creating any thread when disabled. Must be
// called outside DllMain/the loader lock and outside every hook lock. The
// module must stay pinned while either thread can run (see findings).
enum class Launch : uint8_t { started, disabled, already_used, thread_failure };
class Owner;
Launch launch(const std::shared_ptr<Owner>&);
// Real monotonic clock (std::chrono::steady_clock); sleeps clamped to poll_ms.
std::shared_ptr<Clock> make_steady_clock();

class Owner {
public:
    // Runtime and Clock are shared so a detached owner thread keeps them alive.
    Owner(std::shared_ptr<Runtime>, std::shared_ptr<Clock>, Config, Limits = {});
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    ~Owner() = default; // never calls the runtime

    // Body of the owner thread. Exactly one attempt per Owner; a second call
    // returns an already_used report without touching the runtime.
    Report run();

    // Any thread, nonblocking. Stop never erases an accepted wait/begin.
    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }
    // Any thread, nonblocking. SEATED RECENTER (docs/world-render/recenter.md): the owner thread starts a NEW LOCAL space generation at its next
    // sample, exactly as a runtime reference-space change does (the producer re-captures its neutral from the first pose of the generation, the
    // consumer re-anchors and releases frames rendered under the old one). The XR space itself is untouched; poses stay raw LOCAL.
    void request_recenter() noexcept { recenter_req_.fetch_add(1, std::memory_order_release); }
    // Any thread, nonblocking. Never calls the runtime or destroys anything.
    // Returns true when it latched a policy failure (the caller may stop polling).
    bool watchdog_tick() noexcept;
    // Any thread, try-only.
    bool try_read_pose(PoseSnapshot& out) const noexcept { return slot_.try_read(out); }
    // Additive: attach the single consumer BEFORE run()/launch(). Returns
    // false (and attaches nothing) once the owner has been used or launched.
    bool set_consumer(std::shared_ptr<FrameConsumer>) noexcept;
    // XR-START (additive): attach the start-path observer BEFORE run()/launch(); same rules as set_consumer.
    bool set_call_observer(std::shared_ptr<CallObserver>) noexcept;
    // NULL-HEAD (additive, test only): attach the pose filter BEFORE run()/launch(); same rules as set_consumer. Null/none = poses untouched.
    bool set_pose_filter(std::shared_ptr<PoseFilter>) noexcept;
    // XR-START: any thread. -1 until the first begun frame completed (continuous mode), then its owner time.
    int64_t first_frame_done_ms() const noexcept { return first_frame_done_atomic_.load(std::memory_order_acquire); }
    // XR-START: free the call records once finished() (the caller is their only reader). A retained
    // owner of a failed start attempt then keeps no 4096-record array alive.
    void release_records() noexcept { if (finished()) { records_.reset(); records_used_ = 0; } }
    bool continuous() const noexcept { return cfg_.continuous; }

    bool enabled() const noexcept { return cfg_.enabled; }
    bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
    Phase phase() const noexcept { return phase_.load(std::memory_order_acquire); }
    Cause first_cause() const noexcept { return static_cast<Cause>(first_cause_.load(std::memory_order_acquire)); }
    // Valid only once finished() is true; otherwise returns a non-complete report.
    Report report() const;
    // Owner-thread evidence; read only after finished().
    unsigned record_count() const noexcept { return finished() ? records_used_ : 0; }
    const CallRecord* records() const noexcept { return finished() ? records_.get() : nullptr; }

private:
    template <class F> Rc call(Op, F&&);
    int64_t elapsed() { return clock_->now_ms() - activation_; }
    void latch(Cause) noexcept;
    void fail(Cause c) noexcept { failure_ = true; latch(c); }
    void observe_loss() noexcept { loss_ = true; latch(Cause::loss); }
    bool startup();
    bool await_ready();
    void frames();
    bool poll_batch();
    void handle(const Event&);
    void publish(uint64_t frame, const FrameTiming&, Rc views_rc, const ViewSample&, Rc head_rc, const HeadSample&);
    uint32_t generation_at(int64_t time) noexcept;
    void cleanup();
    bool await_stopping();
    void destroy(Op, bool& held);
    void set_phase(Phase p) noexcept { phase_.store(p, std::memory_order_release); }
    bool stop_requested() const noexcept { return stop_.load(std::memory_order_acquire); }
    // Continuous mode has no absolute cap (a far-future sentinel instead).
    int64_t absolute() const noexcept { return cfg_.continuous ? (INT64_MAX / 4) : lim_.absolute_ms; }
    int64_t admission() const noexcept { return cfg_.continuous ? (INT64_MAX / 4) : lim_.admission_ms(); }
    // Continuous mode: the FIRST begun frame (wait/begin/locate/consumer/end,
    // including the runtime's first compositor handshake) is budgeted as part
    // of startup (startup_ms since activation); the 250 ms progress rule applies
    // from the second frame on. Ladder mode is unchanged.
    std::atomic<bool> first_frame_open_{false};

    struct OwnerGate final : Gate {
        Owner& o;
        explicit OwnerGate(Owner& owner) : o(owner) {}
        int64_t now_ms() override { return o.elapsed(); }
        int64_t startup_deadline_ms() const noexcept override { return o.lim_.startup_ms; }
        void sleep_ms(int64_t ms) override;
        bool lost() const override { return o.loss_ || o.hard_lost_; }
        bool stopping() const override { return o.stop_requested() || o.policy_failure_.load(std::memory_order_acquire); }
    protected:
        Rc invoke(Op op, Rc (*fn)(void*), void* ctx) override; // xr_owner.cpp
    };

    std::shared_ptr<Runtime> rt_;
    std::shared_ptr<Clock> clock_;
    Config cfg_;
    Limits lim_;
    SnapshotSlot slot_;
    std::shared_ptr<FrameConsumer> consumer_;
    std::shared_ptr<CallObserver> observer_;
    std::shared_ptr<PoseFilter> pose_filter_;   // NULL-HEAD (test only; none by default)
    std::atomic<int64_t> first_frame_done_atomic_{-1};
    OwnerGate gate_{*this};
    bool consumer_setup_ = false;

    friend Launch launch(const std::shared_ptr<Owner>&);
    std::atomic<bool> used_{false}, launched_{false}, stop_{false}, finished_{false};
    std::atomic<bool> policy_failure_{false}, time_cap_requested_{false};
    std::atomic<Phase> phase_{Phase::off};
    std::atomic<uint8_t> first_cause_{0};
    std::atomic<int64_t> activation_atomic_{0};
    std::atomic<int64_t> last_progress_{0};
    std::atomic<int64_t> cleanup_deadline_{0};
    std::atomic<bool> activated_{false};

    // Owner-thread-only state.
    int64_t activation_ = 0;
    bool failure_ = false, loss_ = false, hard_lost_ = false, flood_ = false;
    SessionState state_ = SessionState::unknown;
    bool instance_ = false, session_ = false, local_ = false, view_ = false, binding_ = true, running_ = false;
    bool teardown_ok_ = true;
    uint32_t blend_ = 0;
    uint32_t space_gen_ = 0, pending_gen_ = 0;
    std::atomic<uint32_t> recenter_req_{0};
    uint32_t recenter_seen_ = 0;
    bool change_pending_ = false;
    int64_t change_time_ = 0;
    uint64_t sequence_ = 0;
    Report r_{};
    std::unique_ptr<CallRecord[]> records_;
    unsigned records_used_ = 0;
};

// Pure policy helpers (exposed for tests).
bool compatible(const GraphicsRequirements&, const BindingFacts&) noexcept;
// CompatMismatch bits for the terms of compatible() that fail; 0 iff compatible().
unsigned compat_mismatch(const GraphicsRequirements&, const BindingFacts&) noexcept;
// One log line naming the refusing stage and, for the binding stage, every
// compared field of both sides plus the failing terms. Always NUL-terminated;
// returns the snprintf length (>= size means truncated). Fits within 448 bytes.
int format_compat_refusal(char* out, size_t size, unsigned attempt, const Report&) noexcept;
bool select_blend(const SystemFacts&, uint32_t& out) noexcept;
bool pose_finite(const Pose&) noexcept;

} // namespace acz::xr_owner
