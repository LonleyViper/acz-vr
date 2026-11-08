// Interface between the in-game per-eye cockpit replay (producer, runs inside the
// game's command list on the game's DIRECT queue) and the in-proxy XR frame
// consumer (xr owner thread). Header only; no behaviour. D-229/D-230.
//
// Contract (both sides must implement exactly this; deviations need a DECISIONS entry):
//  * The producer owns a ring of kSlots eye-target pairs, all proxy-created on the
//    GAME'S device, R16G16B16A16_FLOAT (or the format the matched PSO writes), the game's
//    viewport size. Resources are created in an owner context, never on the Draw path.
//  * A slot is PRODUCING while the game list that replays into it is being recorded or has
//    not completed on the GPU, PUBLISHED when the producer has recorded the whole replay set
//    into it AND has signalled `fence` (the proxy's private fence on the game's DIRECT queue)
//    with `fence_value` after the native ExecuteCommandLists that contains the replays, and
//    HELD while a consumer uses it. The consumer must GPU-wait (queue Wait) or CPU-observe
//    fence completion >= fence_value before touching the images.
//  * Resource state at publish and at release: both eye images are in D3D12_RESOURCE_STATE_
//    RENDER_TARGET. A consumer that needs another state must transition on its own recording and
//    restore RENDER_TARGET before release.
//  * Publication is try-only and bounded: the producer never waits for a consumer; if all slots
//    are HELD or PRODUCING it skips replaying that frame (counted). The consumer never blocks
//    the game thread: try_acquire_latest returns false when nothing newer than `after_frame_id`
//    is published.
//  * Poses: the consumer owns the OpenXR poses; the producer reads the latest qualified eye
//    pose snapshot (xr_owner try_read_pose) at replay-arming time and records it in the slot so
//    the consumer can submit exactly the poses that were rendered (pose-contract: raw eye poses
//    and FOV applied once, one simulation state for both eyes).
#pragma once
#include <cstdint>
#include "tactical_frame.h"   // TACTICAL (tac:1) additive frame facts
#include "object_interp.h"    // OBJECT-INTERP (interp:1) additive frame facts

struct ID3D12Resource;
struct ID3D12Fence;

namespace acz::stereo {

constexpr unsigned kSlots = 3;

struct StereoFrame;
// EYE-RING: the per-eye image size of a frame (eye_width/eye_height when set, else the game frame size).
inline uint32_t eye_image_width(const StereoFrame& f) noexcept;
inline uint32_t eye_image_height(const StereoFrame& f) noexcept;

struct EyePose {                 // OpenXR-style, raw located values (LOCAL space), applied once
    float position[3];           // metres
    float orientation[4];        // x,y,z,w
    float fov[4];                // angleLeft, angleRight, angleUp, angleDown (radians)
};

struct StereoFrame {
    uint64_t frame_id = 0;       // monotonic, producer assigned; 0 = none
    unsigned slot = 0;           // ring slot (opaque to the consumer except for release)
    ID3D12Resource* eye[2] = {}; // [0] left, [1] right, RENDER_TARGET at publish/release
    uint32_t dxgi_format = 0;    // DXGI_FORMAT of the eye images
    uint32_t width = 0, height = 0;   // the GAME frame size (world copy, HUD, markers, effects: game-screen images)
    // EYE-RING additive (eye_ring.h; eyeres > 0 with eyeobj on): the size of the PER-EYE images (eye[], depth[], smoke[], sprite[]) when it differs from
    // width x height (the eye swapchain size); 0 = the per-eye images are width x height (every frame without the eye ring: unchanged).
    uint32_t eye_width = 0, eye_height = 0;
    ID3D12Fence* fence = nullptr;     // private fence on the game's DIRECT queue (not owned)
    uint64_t fence_value = 0;         // completed >= value means the replays finished on the GPU
    EyePose pose[2] = {};             // poses the replays used (zeros + valid=false if synthetic)
    bool pose_valid = false;
    uint64_t predicted_display_time = 0;  // XrTime of the pose sample
    uint32_t members = 0;        // number of cockpit draws replayed into the pair
    bool partial = false;
    // J-180 additive (Codex review of the world-object replay, P2): xr world objects (gpu_copy_step5_world_objects) are replayed INTO the eye images,
    // so the consumer cannot drop them; world_incomplete says that at least one ELIGIBLE scene-target DrawACM_v draw of this frame's list was NOT
    // replayed (world-cap, unreadable record, record/camera/pose/cb10 refusal, or lost before the frame opened) and world_lost counts them. The
    // frame still carries the cockpit (default behaviour); offscreen / out-of-scope draws never set it. Key off: false / 0.
    bool world_incomplete = false;
    uint32_t world_lost = 0;
    // D-236 additive (revision 2): coverage depth, world snapshot and the lens the cockpit was rendered with.
    //  * depth[eye]: the per-eye depth(+stencil) buffer of the replay (DXGI_FORMAT in depth_format,
    //    typically D32_FLOAT_S8X24_UINT), in DEPTH_WRITE at publish/release. Cleared to 0 for the
    //    PS2 GREATER test: texels with depth > 0 were covered by a replayed cockpit draw (coverage mask).
    //  * world: a private copy of the game's scene render target taken at the first replayed member's
    //    seam (everything drawn before the cockpit: sky, terrain, world; HUD is drawn later and is not
    //    in it), same format/size as eye[], in RENDER_TARGET at publish/release. nullptr if unavailable.
    //  * lens_tan: tan of the half-angles the cockpit was rendered with (left, right, up, down) as
    //    recovered from the game's own matrices (about 0.6152, 0.6152, 0.3460, 0.3460); the XR layer must use
    //    the matching field of view: fov = atan(lens_tan).
    ID3D12Resource* depth[2] = {};
    uint32_t depth_format = 0;
    ID3D12Resource* world = nullptr;
    bool world_valid = false;
    float lens_tan[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    bool lens_valid = false;
    // Issue-29 additive (fence correlation for the consumer): `fence_epoch` identifies the producer's
    // private fence lifetime (non-zero; changes if the producer ever recreates its fence) and
    // `last_signalled` is the producer's own bookkeeping of the highest value it has already
    // Signal()ed on the game's DIRECT queue at publish time. The consumer enqueues a GPU queue Wait
    // only when fence/epoch match the producer fence it registered and fence_value <= last_signalled;
    // otherwise it defers the frame (CPU-observes completion later) and never waits on the game's
    // queue for an unauthenticated value. 0 = not provided (the consumer then only proceeds after
    // CPU-observed completion).
    uint64_t fence_epoch = 0;
    uint64_t last_signalled = 0;
    // Issue-29 additive (source verdict 2, finding 5): the LOCAL reference-space generation the
    // replayed poses were sampled under, ENCODED as (PoseSnapshot::space_generation + 1); 0 = unknown.
    // The producer copies the value the PoseProvider returns through try_read_ex() for the pose it
    // rendered with. The consumer releases unused any posed frame whose rendered generation differs
    // from the current one (a pre-recenter render is never relabelled into new coordinates).
    uint32_t space_generation = 0;
    // J-hud-capture additive (src/acz-vr/J-hud-capture-findings.md, "Interface decision"; config key
    // gpu_copy_step5_hud, default OFF): the game's post-cockpit HUD draws (the tail drawn into the
    // scene render target after the last cockpit member and before that target leaves
    // RENDER_TARGET) re-issued unchanged (screen space, MONO: one image for both eyes, no matrix
    // substitution) into a private per-slot target cleared to transparent black (0,0,0,0) once per
    // frame. Same size as world/eye[], format hud_format (the scene format, e.g. 10 = FP16), in
    // RENDER_TARGET at publish/release, covered by the same fence/fence_value as the eyes.
    //  * hud_valid is true only when every HUD-tail draw of the frame was replayed (no refusal) and
    //    at least one was; otherwise hud == nullptr. Frames without HUD capture: hud_valid=false,
    //    hud=nullptr, hud_draws=hud_refused=0.
    //  * The layer is PREMULTIPLIED alpha (the game's own blend into a zero target), meant for a
    //    world-locked quad at the world-quad pose, submitted after the cockpit layer.
    //  * hud_draws / hud_refused: draws replayed / refused in this frame's HUD region (diagnostic).
    ID3D12Resource* hud = nullptr;
    bool hud_valid = false;
    uint32_t hud_format = 0;
    uint32_t hud_draws = 0;
    uint32_t hud_refused = 0;
    // cockpit FOV additive (gpu_copy_step5_hmd_fov, default OFF): when true the eye images were rendered with cockpit_fov[eye] = {angleLeft, angleRight, angleUp, angleDown} radians (XrFovf convention, left/down negative) instead of the game lens; the consumer must submit exactly that FOV for the projection layer.
    bool cockpit_fov_valid = false;
    float cockpit_fov[2][4] = {};
    // neutral frame additive: the producer's seated-recentering neutral (head centre p0 in LOCAL metres, heading yaw0 radians about +Y, same convention as gpu_copy_step5.h Neutral) valid for this frame's space_generation; the cockpit forward direction in LOCAL is Ry(yaw0)*(0,0,-1) from p0.
    bool neutral_valid = false;
    float neutral_pos[3] = {};
    float neutral_yaw = 0.0f;
    // xr_look_drive_keys additive (look_drive_keys.h; default invalid): the look the GAME rendered in this frame (the world image and
    // the target markers are views along it), relative to the cockpit: camera-local -> cockpit-local = Ry(look_yaw) Rx(look_pitch),
    // radians, yaw > 0 left, pitch > 0 up (the same signs as head yaw/pitch). The world and marker planes are placed turned by it
    // about the neutral anchor; the cockpit/HUD planes are not. Key off: false/0 and every placement is exactly as before.
    bool look_valid = false;
    float look_yaw = 0.0f, look_pitch = 0.0f;
    // J-hud-markers additive (src/acz-vr/J-hud-markers-findings.md; producer key gpu_copy_step5_hud_markers, default OFF):
    // the HUD draws classified as target markers (depth func GREATER boxes and their text label) are replayed
    // into a SECOND private per-slot target of the same size/format/premultiplied convention as `hud`, in
    // RENDER_TARGET at publish/release, covered by the same fence; `hud` then holds only the remaining
    // (instrument) draws and the marker draws are NOT also drawn into it. Key off: nullptr/false/0 and the
    // marker draws stay in `hud` exactly as before.
    //  * hud_markers_valid: >= 1 marker draw replayed and 0 marker refusals in this frame; else hud_markers is
    //    nullptr (no marker layer; the frame's markers are simply absent, never taken from another frame).
    //  * hud_valid keeps its meaning for the instruments only (marker refusals do not affect it).
    ID3D12Resource* hud_markers = nullptr;
    bool hud_markers_valid = false;
    uint32_t hud_marker_draws = 0, hud_marker_refused = 0;
    // J-effects-layer additive (src/acz-vr/J-effects-layer-findings.md; producer key gpu_copy_step5_effects, default OFF):
    // the non-member scene draws BETWEEN the cockpit members (missiles, explosions, gun tracers: drawn after the world snapshot
    // and not covered by the HUD tail) replayed into a THIRD private per-slot target of the same size/format/premultiplied
    // convention as `hud`, cleared to transparent once per frame, in RENDER_TARGET at publish/release, covered by the same fence.
    // Screen space and mono, like the world: the consumer maps its pixels through the SAME plane mapping as the world image.
    //  * effects_valid: >= 1 between-members draw replayed (committed); else effects == nullptr. Refusals do not invalidate it
    //    (a missing effect is not an inconsistent picture) but are counted.
    //  * effects_draws: committed replays (between members); effects_refused: refused candidates in committed epochs;
    //    effects_tail: speculative replays recorded after the LAST member (predicated off on the GPU, the HUD covers them).
    //  * Key off: nullptr/false/0 and nothing else changes.
    ID3D12Resource* effects = nullptr;
    bool effects_valid = false;
    uint32_t effects_draws = 0, effects_refused = 0, effects_tail = 0;
    // J-effects-dual (producer key gpu_copy_step5_effects_dual, default OFF): the representation of the `effects` image.
    //  * effects_repr_legacy (0, the default and every key-off frame): the blended PS2 image into a zero-cleared target (premultiplied colour,
    //    alpha = the game's alpha; NOT RGB coverage). The consumer keeps its colour-derived alpha heuristic.
    //  * effects_repr_coverage_v1 (1): FP16 (premultiplied RGB P, RGB coverage A = 1 - product(1 - a)) built from the game's dual-source blend
    //    (RGB SRC_ALPHA/INV_SRC_ALPHA, alpha SRC1_ALPHA/INV_SRC1_ALPHA) with a private PSO clone whose alpha blend is ONE/INV_SRC_ALPHA:
    //    RGB_final = P + (1 - A) * world. Any other value is unknown: the consumer must not composite the image (fail closed).
    uint32_t effects_repr = 0;
    // EFFECTS-OUTSIDE (C) fxmem:4 (fx_smoke_gpu.h; docs/world-render/effects-outside-window.md section 8.7): the missile/aircraft smoke replayed PER EYE
    // from the eye's own camera (world-registered, any direction), one image per eye at the eye size, coverage_v1 (premultiplied RGB P, coverage A),
    // depth-tested on the producer against that eye's objects depth (smoke behind a replayed object is absent, in front of it present). RENDER_TARGET at
    // publish/release, same fence. Composite: after terrain/objects, before the cockpit, at the eye pixel (the objects' decouple re-projection).
    //  * smoke_valid: the frame cleared both images and >= 1 replay executes; else both nullptr. While it is produced the smoke is NOT in `effects`.
    //  * smoke_draws: replays the GPU executes (committed); smoke_refused: smoke draws that fell back to the window effects image.
    //  * fxmem < 4 (default): nullptr/false/0 and nothing else changes.
    ID3D12Resource* smoke[2] = {nullptr, nullptr};
    bool smoke_valid = false;
    uint32_t smoke_format = 0, smoke_draws = 0, smoke_refused = 0;
    // EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h; docs section 11): the game's pre-projected SPRITE effects (explosion PS 542a11d7, indexed GREATER 4caf6e2d)
    // re-projected per eye from vertex-buffer copies and replayed into one image per eye at the eye size: premultiplied LIGHT P + coverage A (the explosion
    // adds light with A unchanged), depth-tested against that eye's objects depth. RENDER_TARGET at publish/release, same fence. Composite: AFTER the smoke,
    // s = tonemap(P) (no division by A), out = s + (1 - A) x under. sprite_valid / sprite_draws / sprite_refused as the smoke fields; while produced those
    // draws are NOT in `effects`. fxmem < 6 (default): nullptr/false/0 and nothing else changes.
    ID3D12Resource* sprite[2] = {nullptr, nullptr};
    bool sprite_valid = false;
    uint32_t sprite_format = 0, sprite_draws = 0, sprite_refused = 0;
    // Timing-anchor diagnostics (docs/review/timing-anchor-fixtures.md; additive, DIAGNOSTIC ONLY: nothing in the consumer's placement,
    // pose or composite decisions reads these). diag_valid=false (default / producers that do not fill it) leaves every record unchanged.
    //  * diag_start: 0 unknown, 1 = opened by the first cockpit member, 2 = opened by an early world draw and adopted by the cockpit
    //    member, 3 = opened by an early world draw and NOT adopted (poisoned/abandoned).
    //  * diag_look_src: 0 no look, 1 = the look is the game camera measured this frame, 2 = the commanded (key-model) estimate.
    //  * diag_look_meas_yaw/pitch, diag_look_est_yaw/pitch: both candidates (radians), meaningful only when diag_look_src != 0 (the
    //    measured pair only when diag_look_meas_valid).
    //  * diag_pose_seq: PoseSnapshot::sequence of the owner snapshot the replay matrices were built from (0 = unknown).
    //  * diag_generation: the recording generation (step-1 list generation) of the replayed list.
    //  * diag_refusal: static string naming the world-refusal reason of the frame (nullptr = none); diag_start_us / diag_publish_us:
    //    std::chrono::steady_clock microseconds at frame start (first accepted draw) and at publication (0 = unknown).
    bool diag_valid = false, diag_look_meas_valid = false;
    uint8_t diag_start = 0, diag_look_src = 0;
    float diag_look_meas_yaw = 0.0f, diag_look_meas_pitch = 0.0f, diag_look_est_yaw = 0.0f, diag_look_est_pitch = 0.0f;
    uint64_t diag_pose_seq = 0, diag_generation = 0;
    const char* diag_refusal = nullptr;
    int64_t diag_start_us = 0, diag_publish_us = 0;
    // WORLD-1 additive (terrain registration; docs/world-render/world1-impl.md). Facts of the GAME camera this frame's eyes were rendered from, so the
    // consumer's terrain uses the same camera as the replayed objects and the marker HUD. Producers that do not fill them leave them invalid.
    //  * game_cam_valid: the cockpit camera recovered from this frame's first member (gpu_copy_step5 decompose of the game's VP): world-space screen right,
    //    screen up and view forward of the game camera (right x up = -forward), BEFORE any head pose / look compensation.
    //  * game_eye_valid: the absolute camera position recovered from this frame's first accepted DrawACM_v record (world::camera_setup eyeAbs), world units.
    //  * e6_valid: the E6 camera sample read (read-only) at this frame's first cockpit member (ee_camera_probe seam B, terrain key only): campos, rot
    //    pitch/yaw/roll radians, E6 heading; e6_seq = the frame-sample sequence number.
    //  * no_cockpit (POV-RENDER stage 2, pov:2; docs/world-render/other-povs.md section 10): a chase frame published without cockpit members (opened by
    //    a scene DrawACM_v draw, never adopted): the game_cam axes are the eyes' rig (level rig or the game camera), no cockpit model is drawn, and
    //    the view classifier does NOT count it as a cockpit composite. Producers that do not fill it leave it false.
    bool no_cockpit = false;
    bool game_cam_valid = false;
    float game_cam_right[3] = {}, game_cam_up[3] = {}, game_cam_fwd[3] = {};
    bool game_eye_valid = false;
    float game_eye[3] = {};
    bool e6_valid = false;
    float e6_campos[3] = {}, e6_pitch = 0.0f, e6_yaw = 0.0f, e6_roll = 0.0f, e6_heading = 0.0f;
    uint64_t e6_seq = 0;
    // CAM-EPOCH (terrain_camera.h): the frame sample's publication time (steady_clock ns, the diag_*_us domain), the E6 lock/stage epochs it was
    // published under and the recording generation it was stamped with (= diag_generation when the producer correlated it). 0 = unknown.
    int64_t e6_t_ns = 0, e6_t_read_ns = 0;   // publication (after the read) / read start
    uint32_t e6_lock_epoch = 0, e6_stage_epoch = 0;
    uint64_t e6_tag = 0;
    // TACTICAL additive (xr_terrain_stage option tac:1, default OFF; tactical_frame.h, docs/world-render/tactical.md): the replayed world draws' model
    // translations, the frame's world -> screen matrix, the game's in-window marker boxes (matched to the draws) and the firing-wedge count of THIS frame.
    // Producer-owned storage of the slot, valid while the slot is HELD (like the images). nullptr = tac off / not collected (every key-off frame).
    const acz::tac::TacFrame* tac = nullptr;
    // OBJECT-INTERP additive (xr_terrain_stage option interp:1, default OFF; object_interp.h, docs/world-render/interp.md): the replayed world objects of
    // this frame were drawn at k game frames ahead, as the camera moved by (dt, dR) sees them; the consumer moves the terrain camera by the SAME delta.
    // Producers that do not fill it (key off, no world camera) leave valid = false.
    acz::interp::Facts interp{};
};
inline uint32_t eye_image_width(const StereoFrame& f) noexcept { return f.eye_width && f.eye_height ? f.eye_width : f.width; }
inline uint32_t eye_image_height(const StereoFrame& f) noexcept { return f.eye_width && f.eye_height ? f.eye_height : f.height; }
inline constexpr uint32_t effects_repr_legacy = 0, effects_repr_coverage_v1 = 1;

class FrameSource {                // implemented by the producer (gpu_copy step 5)
public:
    virtual ~FrameSource() = default;
    // Latest published frame with frame_id > after_frame_id; marks the slot HELD. Never blocks.
    virtual bool try_acquire_latest(uint64_t after_frame_id, StereoFrame& out) = 0;
    // Consumer is done (GPU work it recorded has completed); images are back in RENDER_TARGET.
    virtual void release(const StereoFrame& frame) = 0;
    virtual uint64_t frames_published() const = 0;
    virtual uint64_t frames_skipped() const = 0;   // producer skipped (no free slot / refused)
    // DECOUPLE immutable-image contract (review 2, Astra P2). true ONLY when no producer write to `frame`'s images has been submitted since its
    // publication: the slot is still HELD for this frame (a re-execution of its owning recording quarantines it) and no Execute went untracked
    // since the frame's submission (review 3: bound to the publication, never reset by the acquisition).
    // The producer records every such write BEFORE submitting it to the GPU, so a consumer checks this before AND after its GPU work completed,
    // for the first composite of a frame and for every re-composite: true afterwards proves that work read the published bytes.
    // Default false (fail closed): a producer that cannot prove it gets no re-composition from a held frame.
    virtual bool images_unchanged(const StereoFrame& frame) const { (void)frame; return false; }
};

}  // namespace acz::stereo
