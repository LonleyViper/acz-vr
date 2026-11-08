#pragma once
// brief3d_live.h -- BRIEF3D live wiring, the PRODUCER core (docs/world-render/brief3d.md sections 6 and 8). SDK-free, header-only: the D3D12 glue
// (brief3d_replay_hooks.inl in d3d12_hook.cpp) feeds it facts and executes its decisions; the offline tests drive it with fakes.
//
// Facts it relies on (D-370 live capture X1): the map PSO renders R16G16B16A16_FLOAT (10) with D32_FLOAT_S8X24 (20), 1 sample, depth test+write
// GREATER_EQUAL, stencil on, blend SRC_ALPHA/INV_SRC_ALPHA; t10 is a CPU-readable root SRV; the grid VB is a mapped UPLOAD buffer (stride 16);
// every icon record's matrix 0 equals the grid matrix; b10 = (682, -448).
//
// Lifetime model (Astra review 6460640585, five P2 fixed; invariants in brief3d.md section 8):
//  * ONE command list per frame. The first accepted map draw opens a frame bound to its list and takes one ring slot WRITING (per-slot eye
//    images AND per-slot depth images); map draws in any other list while that frame is open are refused (named). The frame's first granted
//    plan carries the clear, so in the ONLY list that writes the slot the clear precedes every replay (command-list order); a cancelled
//    clear-bearing plan invalidates the frame.
//  * Reservations BEFORE native mutation: draw() reserves the recording (in_record) before it returns permission; commit()/cancel() ends it.
//    before_execute() reserves the submission (submitting) before the native Execute; after_execute() ends it with the Signal outcome.
//  * Publication only after RETIREMENT: a slot is published only after a successful Reset of its list (the recording can never be executed
//    again) with no reservation outstanding, with the highest value signalled for any execution of that recording (re-executions before the
//    Reset are allowed and simply raise the value). Before that the slot is WRITING: the consumer can never hold it and the ring never reuses
//    it. A list never executed when reset = discarded: the slot is aborted (no GPU work referenced it).
//  * One queue: every execution (first or repeated) is checked against the producer's queue BEFORE any Signal is requested; another queue
//    quarantines the slot for ever (never published, never reused) and poisons. A failed Signal does the same. Poison is checked inside the
//    same lock as consumer admission (acquire refuses once poisoned).
//
// UI3D (docs/world-render/ui-3d.md 4, stage 2): a SECOND frame kind in the same ring/slot/list/fence lifetime. ui3d_draw() opens a frame at the
// first accepted ps2_v menu draw (no frame open, no hold), replays every ps2_v draw of the UI target per eye with a rewritten vertex copy
// (acz::ui3d) into the slot's eye images, and closes it at the present. ANY refusal inside an open UI3D frame REJECTS it: it stays bound to its
// list (WRITING) until the list's Reset like every frame, and is then retired WITHOUT publication (Ring::retire_unpublished keeps the executed
// value, so the slot is written again only after that GPU work completed). A non-ps2_v draw anywhere (or a briefing map draw) holds UI3D off
// for kUiHoldMs (menus have none [E census]; flight, movies, the overview and the aircraft select always do). Published UI3D slots carry
// SlotMeta::opaque (the consumer submits them without SOURCE_ALPHA, covering the flat screen).
//
// UI3D stage 3a (u3p:1, ui-3d.md 7; NOT run live): while the consumer's HUD-MENU detector is on (ui3d_pause.h Signal, fresh within 250 ms at the
// Present) a UI3D frame may open IN FLIGHT as a PAUSE frame: every draw before the window (the HUD, the world, effects, any shader) is skipped
// without a refusal or a hold; the window opens at the first large ps2_v draw on a usable UI target (the dimming panel); from there the frame
// follows the UI3D rules on that target (any refusal rejects it); a non-ps2_v draw inside the window closes the window (later on-target menu draws
// reject the frame). The placement is ui3d::pause::place (head yaw, lens size) latched per pause. Published pause slots carry SlotMeta::pause
// (not opaque): the consumer (PauseCore) blends them over the flight eye images with the hud-menu alpha floor.
//
// UI3D stage 3b (u3a:1|2, ui-3d.md 9; NOT run live): a HANGAR frame opens at the FIRST draw of a game frame when it is a DrawACM_v draw whose
// record carries the hangar screen constants (682,-448,2048,2048) (Producer::ui3d_acm). Every ACM draw of the frame is replayed per eye with A
// replaced by A'_e (ui3d_hangar.h: the room mapping latched per screen, or following the game camera's yaw), the game's draw arguments, PSO and
// buffers unchanged (the BRIEF3D root-SRV swap). ps2_v draws before the 2D overlay that are scene effects (indexed or GS z > 256: light glows,
// floor shadow) are DROPPED (not replayed; counted fx); the first ps2_v draw with z <= 256 starts the overlay, replayed on the stage-2 layers at
// the vscreen placement over the room. Any refusal (record, camera, A differing in the frame, an ACM draw after the overlay, an effect after
// it, any stage-2 refusal) REJECTS the frame and holds UI3D off for kUiHoldMs: the flat screen (today's behaviour). Published OPAQUE.
//
// DEBRIEF-3D stage 2 (dbr3d:1, debrief-3d-map.md 8; NOT run live): a THIRD use of the UI3D frame kind. ui3d_draw observes the debrief's MISSION
// RESULT window quad (dbr3d::is_window_quad, the census signature) and arms the debrief route for dbr3d::kArmMs; while armed the glue sends every
// ps2_v draw (indexed ones with their referenced vertex range) to dbr3d_draw, which opens a DEBRIEF frame (r.ui && r.dbr: the menu-frame lifetime,
// rejection and publication rules, OPAQUE) and replays the map scene per eye on a world-locked wraparound and the overlay on the panel layers.
// Its vertex copies use a separate per-slot span (kDbrUploadBase). Non-ps2_v draws still go to ui3d_draw (hold + rejection of the open frame).
#include "brief3d.h"
#include "brief3d_latch.h"   // brief3d.md 11 (b3l:1): the latched scene the XR consumer re-draws every XR frame
#include "ui3d.h"
#include "ui3d_pause.h"   // UI3D stage 3a: the in-flight pause menu (u3p:1, ui-3d.md 7)
#include "ui3d_resample.h"   // UI3D 2026-10-30b: the table blit filter bits (u3r / u3k)
#include "ui3d_hangar.h"  // UI3D stage 3b: the hangar screens as a 3D room (u3a:1|2, ui-3d.md 9)
#include "ui3d_groups.h"  // UI3D 2026-10-31: stages 2-3 of the 3D reinterpretation (u3gp / u3bv / u3tx / u3sr / u3pb, ui-3d.md 14)
#include "dbr3d.h"        // DEBRIEF-3D stage 2: the debrief map scene on a wraparound + the results panel on the UI3D layers (dbr3d:1)
#include "vscreen.h"   // acz::vscreen::Ring / Completion (the shared ring lifetime rules)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace acz::brief3d::live {

using acz::vscreen::Completion;
inline constexpr unsigned kSlots = acz::vscreen::kRingSlots;   // 3
inline constexpr uint32_t kEyeW = 1440, kEyeH = 1440;           // eye image extent (square: the eye FOV is ~square; the consumer stretches)
inline constexpr uint32_t kRtvFormat = 10, kDsvFormat = 20;     // D-370: R16G16B16A16_FLOAT, D32_FLOAT_S8X24_UINT (must equal the PSO's)
inline constexpr unsigned kRtBpp = 8, kDsBpp = 8;
inline constexpr int64_t kScreenGapMs = 2000;    // no map draw for this long = the briefing screen was left: the content latch is reset
inline constexpr int64_t kRelatchGapMs = 30000;  // brief3d.md 10: fit and table placement survive gaps up to this long (page flips); only a longer one re-latches
inline constexpr int64_t kFitRetryMs = 1000;     // a refused fit is retried at most once per second (each try reads the grid VB twice)

// ------------------------------------------------------------------------------------------------ facts of one map draw (hook mutex held)
struct DrawFacts {
    Kind kind = Kind::none;
    uint32_t ps8 = kMapPs8;                        // the PSO's pixel-shader creation hash (icons may pair the icon VS with another PS)
    const void* list = nullptr;
    int64_t now_ms = 0;
    const char* root_why = "b3-root-unread";       // nullptr = root_layout_check passed (plan in t10_param)
    uint32_t t10_param = UINT32_MAX;
    uint32_t b10_param = UINT32_MAX;               // brief3d.md 11: the root-constant parameter of b10 (the latched draw sets it in the consumer list)
    uint64_t t10_va = 0;
    const char* t10_why = "b3-t10-unread";        // nullptr = rec holds srt_bytes(kind) bytes (read twice, equal)
    uint8_t rec[kModelIconSrtBytes] = {};
    const char* b10_why = "b3-b10-unread"; float b10[2] = {0, 0};
    bool pso_known = false; uint32_t rt_count = 0, rtv0 = 0, dsv = 0, samples = 0;
    bool depth_enable = false, depth_write = false, stencil = false, stencil_always = false; uint32_t depth_func = 0;
    bool rs_known = false; unsigned viewports = 0, scissors = 0;   // the step-3 RS track of this list (restored after the replay)
    bool om_known = false;                                         // one bound RT, restorable handles (step-3 track, private copies)
};
// Eye poses for a frame (the XR owner's latest located views through the try-only PoseProvider; space_generation ENCODED: 0 = unknown).
struct PoseIn { bool ok = false; EyeView eyes[2]; uint32_t space_generation = 0; };

// ------------------------------------------------------------------------------------------------ UI3D (ui-3d.md 4)
// Upload layout: the brief3d spans (kSlots x kSlotUploadBytes) first, then one UI3D span per slot (acz::ui3d::kUploadPerSlot).
// DEBRIEF-3D stage 2 (dbr3d:1): one more span per slot (acz::dbr3d::kUploadPerSlot) after the UI3D spans for the debrief's vertex copies.
inline constexpr uint64_t kUiUploadBase = uint64_t(kSlots) * kSlotUploadBytes;
inline constexpr uint64_t kDbrUploadBase = kUiUploadBase + uint64_t(kSlots) * acz::ui3d::kUploadPerSlot;
inline constexpr uint64_t kUploadTotal = kDbrUploadBase + uint64_t(kSlots) * acz::dbr3d::kUploadPerSlot;
inline constexpr int64_t kUiHoldMs = 1000;    // a non-ps2_v draw (or a briefing map draw) holds UI3D off this long
inline constexpr int64_t kUiRenewMs = 250;    // the draw seam observes draws again (to renew the hold) only in the hold's last 250 ms
inline constexpr unsigned kUiVtxBytes = acz::ui3d::kMaxVerticesPerDraw * acz::ui3d::kVertexStride;
// Facts of one draw for UI3D (hook mutex held). A non-ps2_v draw needs only vs8 (+ list, now).
struct UiFacts {
    const void* list = nullptr;
    int64_t now_ms = 0;
    uint32_t vs8 = 0;
    bool indexed = false, gs = false;
    uint32_t topology_type = 3;                     // D3D12_PRIMITIVE_TOPOLOGY_TYPE of the PSO (1 point, 2 line, 3 triangle, 4 patch)
    uint32_t vertices = 0, instances = 1;
    const void* rt = nullptr;                       // RT0 resource (step-3 view registry); nullptr = unknown
    uint32_t rt_w = 0, rt_h = 0, rt_fmt = 0;
    bool pso_known = false; uint32_t rt_count = 0, rtv0 = 0, dsv = 0, samples = 0;
    bool depth_enable = false, stencil = false, stencil_always = false; uint32_t depth_func = 0;
    bool rs_known = false; unsigned viewports = 0, scissors = 0; float vp[4] = {0, 0, 0, 0}; int32_t sc[4] = {0, 0, 0, 0};
    bool om_known = false;
    const char* cb_why = "ui3d-cb0-unread"; acz::ui3d::Cb0 cb0;
    const char* vb_why = "ui3d-vb-unread"; uint32_t stride = 0;
    uint8_t vtx[kUiVtxBytes] = {};                  // the draw's own vertices (read twice, equal), `vertices` x 64 B
    // 2026-10-30d (u3agm, the hangar glow memory): the draw's render state as far as a copy drawn with it would differ: the PSO record and the
    // pixel stage's t0 / s0 tables (glue: a hash; 0 = unknown, the draw then carries no remembered glow). Two glow draws with the same key
    // draw a quad identically apart from its vertices [I: the blend factor is not tracked by the proxy].
    uint64_t state_key = 0;
    // DEBRIEF-3D stage 2: the facts of the debrief window signature (dbr3d::is_window_quad) and the depth write of a map draw
    uint32_t ps8 = 0;
    bool blend = false; uint32_t blend_src = 0, blend_dst = 0;
    bool depth_write = false;
};
// DEBRIEF-3D stage 2 (dbr3d:1, debrief-3d-map.md 8): one ps2_v draw while the debrief is armed. `ui` holds the state facts (its vtx / vb_why /
// vertices are not used); the vertices are the draw's own range (non-indexed) or the referenced range [base + imin, base + imax] of an indexed
// draw, read twice through the pinned mapped UPLOAD span (equal), in a buffer owned by the glue.
inline constexpr unsigned kDbrVtxBytes = acz::dbr3d::kMaxVerticesPerDraw * acz::dbr3d::kVertexStride;   // 256 KiB
struct DbrFacts {
    UiFacts ui;
    uint32_t index_count = 0, start_index = 0; int32_t base = 0;   // indexed: the game's arguments
    uint32_t imin = 0;                                            // indexed: vtx[0] is vertex (base + imin)
    const char* vtx_why = "dbr3d-vertices-unread";
    uint32_t nv = 0; const uint8_t* vtx = nullptr;               // nv x 64 B
    // (review 2026-10-31 F2) the per-eye primitive cull: the game's IA topology (D3D_PRIMITIVE_TOPOLOGY, 0 = unknown) and, indexed, the draw's own
    // index_count indices as read (stride 2 / 4, the PSO's strip-cut value). Without them an eye with a culled vertex keeps the whole-draw cull.
    uint32_t ia_topology = 0;
    const uint8_t* ib = nullptr; uint32_t ib_stride = 0, strip_cut = 0;
    uint32_t gaps = 0;                                           // (F4) unreferenced vertices of the copy the glue filled (dbr3d::fill_unreferenced)
};
// What the glue records for one debrief draw. record = true is a RECORDING RESERVATION (dbr3d_commit / dbr3d_cancel). The eye copies were written
// by the producer into the glue's two buffers (dst[e], nv x 64 B each); the glue copies them to upload_offset[e] and draws per eye with them:
// indexed = DrawIndexedInstanced(the game's index count, 1, the game's start index, base_vertex, startInstance) with the game's index buffer;
// else DrawInstanced(nv, 1, 0, startInstance).
struct DbrPlan {
    bool record = false;
    const char* why = nullptr;
    int slot = -1;
    uint64_t frame = 0;
    bool clear = false; float clear_depth = 0.0f;
    bool bind_dsv = false;
    bool map = false; int layer = -1;     // map scene (wraparound) or panel layer
    bool indexed = false; uint32_t nv = 0; int32_t base_vertex = 0;
    bool eye_draw[2] = {false, false};
    uint64_t upload_offset[2] = {0, 0};
    int32_t scissor[2][4] = {};
    // (review 2026-10-31 F2) an eye with a vertex behind it draws its OWN index list (list_dst[e] of dbr3d_draw, list_count[e] R16 indices into its
    // eye copy) from list_offset[e] of the upload buffer: IASetIndexBuffer(R16), IASetPrimitiveTopology(list_topology), DrawIndexedInstanced(
    // list_count, 1, 0, 0, startInstance); the game's index buffer view and topology are restored after the eyes.
    bool own_list[2] = {false, false};
    uint32_t list_count[2] = {0, 0};
    uint64_t list_offset[2] = {0, 0};
    uint32_t list_topology = 0;
};
// What the glue records for one accepted ps2_v draw. record = true is a RECORDING RESERVATION (ui3d_commit / ui3d_cancel).
struct UiPlan {
    bool record = false;
    const char* why = nullptr;
    int slot = -1;
    uint64_t frame = 0;
    bool clear = false;              // the frame's first plan: clear both eye images (transparent) and the slot's depth images (0, stencil 0)
    float clear_depth = 0.0f;
    int layer = -1;
    uint32_t vertices = 0;
    bool bind_dsv = false;           // the PSO has a DSV format (D32_FLOAT_S8X24): bind the slot's eye depth image; else none
    bool eye_draw[2] = {false, false};                // false = culled / empty scissor for that eye: no draw recorded for it
    uint64_t upload_offset[2] = {0, 0};               // byte offsets of the eye vertex copies inside the WHOLE upload buffer
    int32_t scissor[2][4] = {};                       // l, t, r, b in eye-image pixels
    // 2026-10-30c: the eye copy holds `copies` consecutive draws of `vertices` each (the selection slab: 6 faces of 4); the glue issues one
    // DrawInstanced(vertices, 1, c * vertices, startInstance) per copy from the same eye vertex buffer. 1 = the old single draw.
    uint32_t copies = 1;
    uint8_t vtx[2][kUiVtxBytes] = {};
};
// UI3D stage 3b: facts of one DrawACM_v draw (hook mutex held) and the replay plan (a RECORDING RESERVATION when record = true: the glue writes
// rec[e] at upload_offset[e], records the root-swap sequence, then calls ui3d_acm_commit(plan) or ui3d_acm_cancel(plan)).
struct AcmFacts {
    const void* list = nullptr;
    int64_t now_ms = 0;
    bool indexed = false, gs = false;
    uint32_t topology_type = 3, instances = 1;
    const void* rt = nullptr; uint32_t rt_w = 0, rt_h = 0, rt_fmt = 0;
    bool pso_known = false; uint32_t rt_count = 0, rtv0 = 0, dsv = 0, samples = 0;
    bool depth_enable = false, depth_write = false, stencil = false, stencil_always = false; uint32_t depth_func = 0;
    bool rs_known = false; unsigned viewports = 0, scissors = 0;
    bool om_known = false;
    const char* t10_why = "u3a-t10-unread"; uint32_t t10_param = UINT32_MAX; uint64_t t10_va = 0;
    uint8_t rec[acz::ui3d::hangar::kRecBytes] = {};   // read twice, equal (t10_why == nullptr)
    const char* cb_why = "u3a-cb10-unread"; uint32_t cb10_0 = 0;
};
struct AcmPlan {
    bool record = false;
    const char* why = nullptr;
    int slot = -1;
    uint64_t frame = 0;
    bool clear = false;              // the frame's first plan: clear both eye images (transparent) and depth images (0 = far for GREATER)
    float clear_depth = 0.0f;
    RootSwap swap;                   // t10 -> the eye copies, restored to the game's VA
    uint64_t upload_offset[2] = {0, 0};
    uint8_t rec[2][acz::ui3d::hangar::kRecBytes] = {};
};
// brief3d::EyeView -> ui3d::EyeView
inline acz::ui3d::EyeView ui_eye(const EyeView& e) noexcept {
    acz::ui3d::EyeView u;
    for (int i = 0; i < 3; ++i) u.p[i] = e.position[i];
    for (int i = 0; i < 4; ++i) { u.q[i] = e.orientation[i]; u.fov[i] = e.fov[i]; }
    return u;
}
inline const char* ui_other_why(uint32_t vs8) noexcept {
    using namespace acz::ui3d;
    return vs8 == kSelMapVs8 || vs8 == kModelIconVs8 || vs8 == kAcmVs8 || vs8 == kCptVs8 ? "ui3d-frame-has-3d"
         : vs8 == kMovieVs8 ? "ui3d-frame-has-movie" : "ui3d-frame-has-other-shader";
}

// What the glue records for one accepted draw. A plan with record = true is a RESERVATION: the glue must end it with commit() or cancel().
struct Plan {
    bool record = false;
    const char* why = nullptr;   // the refusal (record = false)
    int slot = -1;
    uint64_t frame = 0;          // the frame the reservation belongs to
    bool clear = false;          // the frame's first plan: clear both eye images (transparent) and the slot's depth images (0 = far, GREATER_EQUAL)
    Kind kind = Kind::none;
    unsigned index = 0;
    uint64_t upload_offset[2] = {0, 0};   // byte offsets of the eye copies inside the WHOLE upload buffer (slot span included)
    unsigned rec_bytes = 0;
    unsigned copies = 1;         // brief3d 9.2: records per eye (icon + stalk dots); copy c of eye e at upload_offset[e] + c * swap.step
    uint8_t eye_rec[2][kMaxIconCopies][kModelIconSrtBytes] = {};
    RootSwap swap;
    float clear_depth = 0.0f;
};

// What the consumer gets with a held slot.
// opaque: a UI3D slot (the consumer submits it without SOURCE_ALPHA and blits rgb with alpha 1: the menu draws leave the eye alpha untouched [E]).
// pause: a UI3D stage-3a pause-menu slot (blended into the flight eye images by PauseCore; never shown by the table on the screen layer).
struct SlotMeta { uint64_t frame = 0; EyeView eyes[2]; uint32_t space_generation = 0; int64_t published_ms = 0; unsigned replays = 0; bool opaque = false;
                  bool pause = false; };
struct Held { int slot = -1; acz::vscreen::Acquired a; SlotMeta meta; };

struct Stats {
    uint64_t frames = 0, published = 0, aborted = 0, replays = 0, refused = 0, selmap = 0, icons = 0, icons_unverified = 0, executes = 0,
             reexecutes = 0, signals = 0, discarded = 0, cancelled = 0, invalidated = 0, quarantined = 0, fits = 0, fit_refused = 0,
             placements = 0, no_pose = 0, no_slot = 0, acquired = 0, other_list = 0, icons_other_ps = 0,
             air_icons = 0, stalk_dots = 0;   // brief3d 9.2: replayed icons treated as AIR (stalk/size/clamps) and the stalk dot copies drawn
    // Consumer lag at the last acquire (XR owner thread): producer frames opened after the acquired slot's frame, and the slot's age on the
    // producer's Present clock (GetTickCount64 at the last on_present: resolution one game Present).
    uint64_t lag_frames = 0, lag_max = 0, age_ms = 0, age_max_ms = 0;
};

// The two xr-stereo.log lines of the BRIEF3D table summary (split: one combined line was cut at the 480-byte log capacity). Counters are clamped
// to 7 digits (replays/icons: 9) and reasons width-limited, so every line stays < kLineLimit for any value (offline-tested).
inline unsigned long long clamp_digits(uint64_t v, unsigned digits) noexcept {
    uint64_t m = 1; for (unsigned i = 0; i < digits; ++i) m *= 10;
    return static_cast<unsigned long long>(v >= m ? m - 1 : v);
}
inline int format_xr_producer(char* buf, size_t n, const Stats& s, bool poisoned, const char* why) noexcept {
    auto c = [](uint64_t v) { return clamp_digits(v, 7); };
    return std::snprintf(buf, n,
        "xr-stereo;event=summary-brief3d-producer;frames=%llu;published=%llu;aborted=%llu;replays=%llu;selmap=%llu;icons=%llu;icons-unverified=%llu;"
        "refused=%llu;other-list=%llu;executes=%llu;reexecutes=%llu;signals=%llu;discarded=%llu;cancelled=%llu;invalidated=%llu;quarantined=%llu;"
        "acquired=%llu;lag-frames=%llu;lag-max=%llu;age-ms=%llu;age-max-ms=%llu;poisoned=%d;why=%.32s",
        c(s.frames), c(s.published), c(s.aborted), clamp_digits(s.replays, 9), c(s.selmap), clamp_digits(s.icons, 9), c(s.icons_unverified),
        c(s.refused), c(s.other_list), c(s.executes), c(s.reexecutes), c(s.signals), c(s.discarded), c(s.cancelled), c(s.invalidated),
        c(s.quarantined), c(s.acquired), c(s.lag_frames), c(s.lag_max), c(s.age_ms), c(s.age_max_ms), poisoned ? 1 : 0, why ? why : "none");
}
// The producer's fit/placement fields, appended to the table line.
inline int format_xr_tail(char* buf, size_t n, const Stats& s, uint64_t screens, const char* fit_why) noexcept {
    auto c = [](uint64_t v) { return clamp_digits(v, 7); };
    return std::snprintf(buf, n, "fits=%llu;fit-refused=%llu;placements=%llu;no-pose=%llu;no-slot=%llu;screens=%llu;fit-why=%.32s",
        c(s.fits), c(s.fit_refused), c(s.placements), c(s.no_pose), c(s.no_slot), c(screens), fit_why ? fit_why : "none");
}

class Producer {
public:
    // ---- configuration (monitor thread, once) and GPU readiness (game thread, once)
    void configure(const Options& o) noexcept {
        std::lock_guard<std::mutex> l(m_); o_ = o; latch_on_.store(o.on && o.latch, std::memory_order_release); on_.store(o.on, std::memory_order_release);
    }
    // brief3d.md 11: b3l:1 (default) with brief3d:1 - the glue calls latch_draw() instead of draw(), the consumer re-draws the latched scene.
    // false again after the automatic fallback (no scene ever latched in kLatchGiveUpFrames game frames whose geometry was unreadable): the old
    // replay path then runs, so an unreadable buffer can never leave the briefing without a table. Switched only at a Present (between frames).
    bool latch_mode() const noexcept { return latch_on_.load(std::memory_order_acquire) && !latch_fallback_.load(std::memory_order_acquire); }
    bool latch_fell_back() const noexcept { return latch_fallback_.load(std::memory_order_acquire); }
    bool on() const noexcept { return on_.load(std::memory_order_acquire) && !poisoned_.load(std::memory_order_acquire); }
    // brief3d:1 or ui3d:1 was parsed (the hooks' cheap gate; stays true after poisoning so resets/executes of pending lists are still seen).
    bool configured() const noexcept { return on_.load(std::memory_order_acquire) || ui_on_.load(std::memory_order_acquire); }
    bool table_configured() const noexcept { return on_.load(std::memory_order_acquire); }
    // UI3D (ui3d:1 with every prerequisite; monitor thread, once).
    void configure_ui3d(const acz::ui3d::Options& o) noexcept {
        std::lock_guard<std::mutex> l(m_); uo_ = o; ui_pause_cfg_.store(o.on && o.pause, std::memory_order_release);
        ui_layering_.store(o.layering, std::memory_order_release); ui_filter_bits_.store(acz::ui3d::resample::filter_bits(o.filter, o.sharpen), std::memory_order_release);
        ui_hangar_cfg_.store(o.on && o.hangar != 0, std::memory_order_release); ui_on_.store(o.on, std::memory_order_release);
    }
    // DEBRIEF-3D stage 2 (dbr3d:1 with every live prerequisite, ui3d:1 configured first; monitor thread, once).
    void configure_dbr3d(const acz::dbr3d::Options& o) noexcept { std::lock_guard<std::mutex> l(m_); dbr_o_ = o; dbr_on_.store(o.on, std::memory_order_release); }
    bool dbr3d_configured() const noexcept { return dbr_on_.load(std::memory_order_acquire) && ui_on_.load(std::memory_order_acquire); }
    // The draw seam's routing test (no lock): the debrief window quad was seen within dbr3d::kArmMs: ps2_v draws go to dbr3d_draw.
    bool dbr3d_armed(int64_t now_ms) const noexcept {
        if (!dbr_on_.load(std::memory_order_relaxed) || !ui_on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) return false;
        // the in-flight pause (u3p trigger live) is never a debrief: its draws stay on the pause path (ui3d_draw), see ui3d_draw's observation
        if (pause_live_.load(std::memory_order_relaxed)) return false;
        const int64_t seen = dbr_seen_ms_.load(std::memory_order_relaxed);
        return seen != 0 && now_ms >= seen && now_ms - seen <= acz::dbr3d::kArmMs;
    }
    // UI3D stage 3b: u3a:1|2 was configured with ui3d:1 (the glue routes DrawACM_v draws to ui3d_acm).
    bool ui3d_hangar_configured() const noexcept { return ui_on_.load(std::memory_order_acquire) && ui_hangar_cfg_.load(std::memory_order_acquire); }
    bool ui3d_on() const noexcept { return ui_on_.load(std::memory_order_relaxed) && !poisoned_.load(std::memory_order_relaxed); }
    bool ui3d_configured() const noexcept { return ui_on_.load(std::memory_order_acquire); }
    // UI3D stage 3a: u3p:1 was configured with ui3d:1 (the consumer creates its PauseCore), and the trigger is live (set at the Present).
    bool ui3d_pause_configured() const noexcept { return ui_on_.load(std::memory_order_acquire) && ui_pause_cfg_.load(std::memory_order_acquire); }
    // 2026-10-30b: the consumer's layer list rule (u3q) and the table blit's resample bits for opaque UI3D / pause slots (u3r / u3k).
    unsigned ui3d_layering() const noexcept { return ui_layering_.load(std::memory_order_acquire); }
    uint32_t ui3d_filter_bits() const noexcept { return ui_filter_bits_.load(std::memory_order_acquire); }
    bool ui3d_pause_live() const noexcept { return pause_live_.load(std::memory_order_relaxed); }
    // The draw seam's cheap skip (no lock): a hold with more than kUiRenewMs left. Draws in the last kUiRenewMs are observed to renew it.
    bool ui3d_held(int64_t now_ms) const noexcept { return now_ms + kUiRenewMs < ui_hold_until_.load(std::memory_order_relaxed); }
    void gpu_ready(uint64_t upload_va) noexcept { std::lock_guard<std::mutex> l(m_); upload_va_ = upload_va; gpu_ = upload_va != 0; }
    bool gpu() const noexcept { std::lock_guard<std::mutex> l(m_); return gpu_; }
    void gpu_failed(const char* why) noexcept { std::lock_guard<std::mutex> l(m_); poison_locked(why ? why : "brief3d-gpu"); }
    // The DLL's GPU objects for the consumer (opaque here): the slot eye images (ID3D12Resource*, resting in COMMON) and the producer fence
    // (ID3D12Fence*). Set once by the game thread BEFORE gpu_ready(); never changed or released afterwards (process lifetime).
    void set_handles(void* const rt[kSlots][2], void* fence) noexcept {
        for (unsigned s = 0; s < kSlots; ++s) for (unsigned e = 0; e < 2; ++e) rt_[s][e].store(rt[s][e], std::memory_order_release);
        fence_.store(fence, std::memory_order_release);
    }
    void* image(unsigned slot, unsigned eye) const noexcept { return slot < kSlots && eye < 2 ? rt_[slot][eye].load(std::memory_order_acquire) : nullptr; }
    void* fence() const noexcept { return fence_.load(std::memory_order_acquire); }

    // ---- draw path (game render thread)
    bool wants_poses(int64_t) const noexcept { std::lock_guard<std::mutex> l(m_); return open_ < 0; }
    bool wants_box(int64_t now_ms) const noexcept {
        std::lock_guard<std::mutex> l(m_);
        return relatch_locked(now_ms) || (!fit_.valid && (!fit_tried_ms_ || now_ms - fit_tried_ms_ >= kFitRetryMs));
    }
    // One map draw. record = true: a RECORDING RESERVATION was taken for (slot, frame); the glue writes plan.eye_rec at plan.upload_offset,
    // records the sequence, then calls commit(plan) (or cancel(plan) if it recorded nothing). Every refusal leaves the game's draw alone.
    Plan draw(const DrawFacts& f, const PoseIn& pose, const Box* box, const char* box_why, const Completion& fence) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Plan p; p.kind = f.kind;
        auto refuse = [&](const char* why) { p.why = why; ++st_.refused; last_why_ = why; return p; };
        if (!on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) return refuse(poison_why_ ? poison_why_ : "brief3d-off");
        if (!gpu_) return refuse("brief3d-gpu-not-ready");
        if (f.kind == Kind::none) return refuse("brief3d-not-a-map-draw");
        // UI3D: a map draw holds the menu layers off (the briefing overview belongs to the table) and rejects an open UI3D frame (the glue's
        // UI3D seam normally did that already for this very draw); this draw then waits for the next frame (its list holds the rejected one).
        ui_hold_locked(f.now_ms);
        if (open_ >= 0 && run_[open_].ui) ui_reject_locked("ui3d-frame-has-3d");
        if (relatch_locked(f.now_ms)) {   // a long absence = a new briefing: fit and placement are latched afresh (brief3d.md 10)
            fit_ = Fit{}; fit_box_ = Box{}; placement_ = Placement{}; fit_tried_ms_ = 0; ++screens_;
        }
        if (screen_left_locked(f.now_ms)) {
            seen_top_ = 0; seen_any_ = false; done_top_ = 0; done_any_ = false; latched_top_ = 0; content_k_ = 1.0; latch_ = ContentLatch{}; k_ms_ = 0;
        }
        last_draw_ms_ = f.now_ms;
        if (f.kind == Kind::selmap) ++st_.selmap; else { ++st_.icons; if (f.ps8 != kMapPs8) ++st_.icons_other_ps; }
        if (f.root_why) return refuse(f.root_why);
        if (f.t10_why) return refuse(f.t10_why);
        if (f.b10_why) return refuse(f.b10_why);
        if (!fit_.valid && f.kind == Kind::selmap && (box || box_why)) {
            fit_tried_ms_ = f.now_ms ? f.now_ms : 1;
            const char* why = box_why ? box_why : fit_model(*box, load(f.rec), f.b10, o_, fit_);
            if (why) { ++st_.fit_refused; fit_why_ = why; fit_ = Fit{}; }
            else { ++st_.fits; fit_box_ = *box; fit_why_ = nullptr; fit_line_ = true; }
        }
        if (!fit_.valid) return refuse(fit_why_ ? fit_why_ : "brief3d-no-fit-yet");
        if (f.kind == Kind::modelicon) {   // zoom-aware content fit: the highest icon the game drew since the last opened frame (refused or not)
            double h = 0;
            if (icon_height(f.rec, fit_, h)) { if (!seen_any_ || h > seen_top_) seen_top_ = h; seen_any_ = true; }
        }
        GateIn g; g.kind = f.kind; g.ready = true; g.frame_ok = true; g.root_ok = true; g.t10_va = f.t10_va != 0; g.b10_written = true;
        g.predication_off = true;   // the replay never sets predication; a game predicate applies to the game's draw and ours alike
        g.pso_rtv0 = f.rtv0; g.pso_rt_count = f.rt_count; g.pso_dsv = f.dsv; g.pso_samples = f.samples;
        g.image_format = kRtvFormat; g.image_ds_format = kDsvFormat;
        g.depth_enable = f.depth_enable; g.depth_write = f.depth_write; g.depth_func = f.depth_func; g.stencil = f.stencil; g.stencil_always = f.stencil_always;
        g.vp_known = f.rs_known; g.scissor_known = f.rs_known; g.viewports = f.viewports; g.scissors = f.scissors;
        if (!f.pso_known) return refuse("brief3d-pso-unknown");
        if (const char* why = gate(g)) return refuse(why);
        if (!f.om_known) return refuse("brief3d-om-unknown");
        DepthMode dm; float clear = 0.0f;
        if (const char* why = depth_mode(f.depth_enable, f.depth_func, dm, clear)) return refuse(why);
        if (open_ >= 0 && run_[open_].list != f.list) { ++st_.other_list; return refuse("brief3d-frame-other-list"); }
        if (f.kind == Kind::modelicon) {
            if (open_ >= 0 && have_grid_) {
                int idx = -1;
                if (const char* why = icon_camera_index(f.rec, grid_m_, idx)) return refuse(why);
                if (idx != 0) return refuse("brief3d-icon-camera-not-matrix0");
            } else ++st_.icons_unverified;
        }
        if (open_ < 0) {
            for (const Run& r : run_) if (r.state == run_writing && r.list == f.list) return refuse("brief3d-list-holds-previous-frame");
            if (!pose.ok) { ++st_.no_pose; return refuse("brief3d-no-pose"); }
            if (!placement_.valid || placement_.space_generation != pose.space_generation) {
                EyeView head = pose.eyes[0];
                for (int k = 0; k < 3; ++k) head.position[k] = 0.5 * (pose.eyes[0].position[k] + pose.eyes[1].position[k]);
                const Placement pl = place(o_, head.position, head.orientation, pose.space_generation);
                if (!pl.valid) return refuse("brief3d-placement-invalid");
                placement_ = pl; ++st_.placements;
            }
            // The content top of the icons seen since the last opened frame, with hysteresis (grow at once with 10% margin; shrink below half):
            // a candidate here, committed only when the frame really opens (a refused open keeps accumulating).
            // W8-1 P2: the observation window is the last COMPLETED game frame interval (rolled at Present, open or refused) plus the draws seen so
            // far in this one; a refused open therefore cannot preserve an obsolete high icon after the game zoomed out.
            double top = 0.0;
            if (done_any_ && done_top_ > 0) top = done_top_;
            if (seen_any_ && seen_top_ > top) top = seen_top_;
            ContentLatch trial = latch_;   // committed only when the frame really opens
            const double next_top = latch_content_top(trial, top, f.now_ms);
            double next_k = content_scale(fit_, next_top, content_max_height(o_));
            if (!(next_k > 0)) return refuse("brief3d-content-too-tall");
            next_k = relax_content_scale(content_k_, next_k, k_ms_ ? f.now_ms - k_ms_ : 0);   // tightening at once, relaxing slowly   // W8-1 P1: the 0.35 m cap needs a scale below the minimum: no slot, no claim
            const Fit fe = squashed_fit(fit_, next_k);   // brief3d 9.2: vertical only (the table keeps its width)
            const Box cb = content_box(fit_box_, fit_, next_top);
            FrameState fs; fs.begin();
            EyeCfg cfg; cfg.depth = dm;
            if (const char* why = open_eyes(fs, fe, cb, placement_, pose.eyes, f.b10, cfg)) return refuse(why);
            const int s = ring_.begin_write(fence);
            if (s < 0) { ++st_.no_slot; return refuse(fence.lost ? "brief3d-fence-lost" : "brief3d-no-free-slot"); }
            if (next_k != content_k_) { ++content_changes_; content_line_ = true; }
            latch_ = trial; k_ms_ = f.now_ms ? f.now_ms : 1; latched_top_ = next_top; content_k_ = next_k; seen_top_ = 0; seen_any_ = false; done_top_ = 0; done_any_ = false;
            Run& r = run_[s]; r = Run{}; r.state = run_writing; r.list = f.list; r.frame = ++frame_serial_; r.open = true;
            meta_[s] = SlotMeta{}; meta_[s].frame = r.frame; meta_[s].eyes[0] = pose.eyes[0]; meta_[s].eyes[1] = pose.eyes[1];
            meta_[s].space_generation = pose.space_generation;
            eye_m_[0] = fs.eye[0]; eye_m_[1] = fs.eye[1]; dm_ = dm; have_grid_ = false;
            open_ = s;
            ++st_.frames;
        } else if (dm != dm_) return refuse("brief3d-depth-mode-changed");
        Run& r = run_[open_];
        if (f.kind == Kind::selmap && !have_grid_) { std::memcpy(grid_m_, f.rec, 64); have_grid_ = true; }
        const unsigned idx = f.kind == Kind::selmap ? r.sel_n : r.icon_n;
        const uint64_t span = uint64_t(open_) * kSlotUploadBytes;
        IconCopies ic;   // brief3d 9.2: the icon's model matrix per copy (vertical compensation, air size/clamps, stalk dots)
        if (f.kind == Kind::modelicon)
            icon_copies(f.rec, fit_, content_k_, fit_box_, content_max_height(o_), idx < kStalkCopyBudget ? kStalkCopyBudget - idx : 0, ic);
        if (const char* why = plan_root_swap(f.kind, f.t10_param, f.t10_va, upload_va_ + span, kSlotUploadBytes, idx, p.swap, ic.copies)) return refuse(why);
        for (unsigned e = 0; e < 2; ++e) {
            for (unsigned c = 0; c < ic.copies; ++c) {
                const char* why = f.kind == Kind::selmap ? selmap_record(eye_m_[e], p.eye_rec[e][c]) : modelicon_record(f.rec, 0, eye_m_[e], p.eye_rec[e][c]);
                if (why) return refuse(why);
                if (f.kind == Kind::modelicon) std::memcpy(p.eye_rec[e][c] + 64 * kIconModelMatrix, ic.m3[c], 64);
            }
            p.upload_offset[e] = span + copy_offset(f.kind, idx, e);
        }
        p.copies = ic.copies;
        if (f.kind == Kind::selmap) ++r.sel_n; else { r.icon_n += ic.copies; if (ic.air) ++st_.air_icons; st_.stalk_dots += ic.copies - 1; }
        p.rec_bytes = srt_bytes(f.kind);
        p.record = true; p.slot = open_; p.frame = r.frame; p.index = idx; p.clear = !r.clear_granted; p.clear_depth = clear;
        r.clear_granted = true;
        ++r.in_record;   // the reservation: no settlement while the glue may still mutate the list / our upload span
        return p;
    }
    // ---- brief3d.md 11 (b3l:1): one map draw LATCHED for the consumer's per-XR-frame render (game render thread). Nothing is recorded into
    // the game's list and no slot of the ring is used. `g` = the draw's geometry read by the glue (geom_why != nullptr: unreadable). The draws
    // of one game frame (between two Presents) form the pending scene; on_present() publishes it only when no map draw of that frame was
    // refused (else the previous scene stays: never a scene with missing icons). `g` is a by-value parameter: whatever is not latched (the
    // game's PSO / root signature references) is released after the producer lock is dropped (lock order: the hooked Release takes the
    // hook-state mutex, which is held around wants_box()).
    void latch_draw(const DrawFacts& f, const PoseIn& pose, const Box* box, const char* box_why, latch::Geom g, const char* geom_why) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!latch_mode() || poisoned_.load(std::memory_order_relaxed) || f.kind == Kind::none) return;
        auto refuse = [&](const char* why) { ++st_.refused; ++lst_.refused; last_why_ = why; lt_bad_ = true; lt_why_ = why; };
        ui_hold_locked(f.now_ms);
        if (open_ >= 0 && run_[open_].ui) ui_reject_locked("ui3d-frame-has-3d");
        if (relatch_locked(f.now_ms)) { fit_ = Fit{}; fit_box_ = Box{}; placement_ = Placement{}; fit_tried_ms_ = 0; ++screens_; }
        if (screen_left_locked(f.now_ms)) {
            seen_top_ = 0; seen_any_ = false; done_top_ = 0; done_any_ = false; latched_top_ = 0; content_k_ = 1.0; latch_ = ContentLatch{}; k_ms_ = 0;
        }
        last_draw_ms_ = f.now_ms;
        ++lst_.draws;
        if (f.kind == Kind::selmap) ++st_.selmap; else { ++st_.icons; if (f.ps8 != kMapPs8) ++st_.icons_other_ps; }
        if (f.root_why) return refuse(f.root_why);
        if (f.t10_why) return refuse(f.t10_why);
        if (f.b10_why) return refuse(f.b10_why);
        if (!fit_.valid && f.kind == Kind::selmap && (box || box_why)) {
            fit_tried_ms_ = f.now_ms ? f.now_ms : 1;
            const char* why = box_why ? box_why : fit_model(*box, load(f.rec), f.b10, o_, fit_);
            if (why) { ++st_.fit_refused; fit_why_ = why; fit_ = Fit{}; }
            else { ++st_.fits; fit_box_ = *box; fit_why_ = nullptr; fit_line_ = true; }
        }
        if (!fit_.valid) return refuse(fit_why_ ? fit_why_ : "brief3d-no-fit-yet");
        if (f.kind == Kind::modelicon) {
            double h = 0;
            if (icon_height(f.rec, fit_, h)) { if (!seen_any_ || h > seen_top_) seen_top_ = h; seen_any_ = true; }
        }
        // the consumer draws with the game's PSO into eye images of exactly these formats (the gate's format/stencil/depth rules; no RS/OM of the
        // game's list is touched any more, so its viewport/OM track is not needed)
        if (!f.pso_known) return refuse("brief3d-pso-unknown");
        if (f.samples != 1) return refuse("brief3d-pso-multisample");
        if (f.rt_count != 1 || f.rtv0 != kRtvFormat) return refuse("brief3d-rt-format-differs");
        if (f.dsv != kDsvFormat) return refuse("brief3d-dsv-format-differs");
        if (f.stencil && !f.stencil_always) return refuse("brief3d-pso-stencil-test");
        DepthMode dm; float clear = 0.0f;
        if (const char* why = depth_mode(f.depth_enable, f.depth_func, dm, clear)) return refuse(why);
        if (f.b10_param == UINT32_MAX || f.t10_param == UINT32_MAX) return refuse("b3l-root-params");
        if (geom_why) { ++lst_.geom_refused; lt_geom_bad_ = true; return refuse(geom_why); }
        if (!g.pso || !g.root || !g.ib || !g.vb || !g.stride || (f.kind == Kind::selmap && (!g.t11 || g.t11_param == UINT32_MAX)))
            { ++lst_.geom_refused; lt_geom_bad_ = true; return refuse("b3l-geometry-incomplete"); }
        // (review 2026-10-31 F5) our list binds only IA slot 0: a draw with another bound vertex buffer slot would read 0 for those attributes (wrong
        // colour / position). Refused: the frame keeps the previous scene. Live evidence (L1 logs 2026-10-31): extra-vb=0 on every map draw.
        if (g.extra_vb) { ++lst_.extra_vb; return refuse("b3l-extra-vb"); }
        if (f.kind == Kind::modelicon) {
            if (lt_have_grid_) {
                int idx = -1;
                if (const char* why = icon_camera_index(f.rec, lt_grid_m_, idx)) return refuse(why);
                if (idx != 0) return refuse("brief3d-icon-camera-not-matrix0");
            } else ++st_.icons_unverified;
        }
        // placement: latched once per briefing (and per LOCAL generation), from the head pose at that moment; never moved by the head after
        if (!placement_.valid || (pose.ok && placement_.space_generation != pose.space_generation)) {
            if (!pose.ok) { ++st_.no_pose; return refuse("brief3d-no-pose"); }
            EyeView head = pose.eyes[0];
            for (int k = 0; k < 3; ++k) head.position[k] = 0.5 * (pose.eyes[0].position[k] + pose.eyes[1].position[k]);
            const Placement pl = place(o_, head.position, head.orientation, pose.space_generation);
            if (!pl.valid) return refuse("brief3d-placement-invalid");
            placement_ = pl; ++st_.placements;
            // headset test 9 (b3p:1): the table quad's world-fixed plane, once per placement (the fit is latched before the placement)
            plane_ = o_.fixed_plane ? latch::table_plane(placement_, volume(fit_box_, fit_), content_max_height(o_), -double(o_.y_m), double(o_.dist_m))
                                    : latch::TablePlane{};
        }
        if (!pend_) {   // the first accepted map draw of this game frame: the frame's content scale (the old path's frame-open rule, brief3d.md 9-10)
            double top = 0.0;
            if (done_any_ && done_top_ > 0) top = done_top_;
            if (seen_any_ && seen_top_ > top) top = seen_top_;
            ContentLatch trial = latch_;
            const double next_top = latch_content_top(trial, top, f.now_ms);
            double next_k = content_scale(fit_, next_top, content_max_height(o_));
            if (!(next_k > 0)) return refuse("brief3d-content-too-tall");
            next_k = relax_content_scale(content_k_, next_k, k_ms_ ? f.now_ms - k_ms_ : 0);
            std::shared_ptr<latch::Scene> s;
            try { s = std::make_shared<latch::Scene>(); s->draws.reserve(kSelMapCap + 64); } catch (...) { return refuse("b3l-alloc"); }
            if (next_k != content_k_) { ++content_changes_; content_line_ = true; }
            latch_ = trial; k_ms_ = f.now_ms ? f.now_ms : 1; latched_top_ = next_top; content_k_ = next_k;
            seen_top_ = 0; seen_any_ = false; done_top_ = 0; done_any_ = false;
            s->fit = squashed_fit(fit_, next_k); s->base_fit = fit_; s->placement = placement_; s->plane = plane_; s->space_generation = placement_.space_generation;
            s->depth = dm; s->clear_depth = clear; s->b10[0] = f.b10[0]; s->b10[1] = f.b10[1]; s->content_k = next_k;
            pend_ = std::move(s); lt_have_grid_ = false; lt_icon_n_ = 0;
        } else {
            if (dm != pend_->depth) return refuse("brief3d-depth-mode-changed");
            if (f.b10[0] != pend_->b10[0] || f.b10[1] != pend_->b10[1]) return refuse("b3l-b10-differs");
        }
        latch::Draw d;
        d.kind = f.kind; std::memcpy(d.rec, f.rec, srt_bytes(f.kind)); d.b10[0] = f.b10[0]; d.b10[1] = f.b10[1];
        d.t10_param = f.t10_param; d.b10_param = f.b10_param;
        if (f.kind == Kind::selmap) {
            if (pend_->selmap >= kSelMapCap) return refuse("brief3d-replay-cap");
            if (!lt_have_grid_) { std::memcpy(lt_grid_m_, f.rec, 64); lt_have_grid_ = true; }
        } else {
            IconCopies ic;   // brief3d 9.2: vertical compensation, air size/clamps and the stalk dots, computed once per game frame
            icon_copies(f.rec, fit_, content_k_, fit_box_, content_max_height(o_), lt_icon_n_ < kStalkCopyBudget ? kStalkCopyBudget - lt_icon_n_ : 0, ic);
            if (ic.copies < 1 || ic.copies > kMaxIconCopies || lt_icon_n_ + ic.copies > kIconCap) return refuse("brief3d-replay-cap");
            d.copies = ic.copies; d.air = ic.air; std::memcpy(d.m3, ic.m3, sizeof d.m3);
            d.radius = latch::mesh_radius(g.vb->data(), g.vb->size(), g.stride);
        }
        d.g = std::move(g);
        try { pend_->draws.push_back(std::move(d)); } catch (...) { g = std::move(d.g); return refuse("b3l-alloc"); }   // strong guarantee: d intact; its references go back to the parameter (released after the unlock)
        if (f.kind == Kind::selmap) ++pend_->selmap;
        else { ++pend_->icons; lt_icon_n_ += pend_->draws.back().copies; if (pend_->draws.back().air) ++st_.air_icons; st_.stalk_dots += pend_->draws.back().copies - 1; }
        ++st_.replays;
    }
    // XR owner thread: the newest published scene (shared: the consumer keeps it alive for its in-flight frames).
    std::shared_ptr<const latch::Scene> latched_scene() const noexcept { std::lock_guard<std::mutex> l(m_); return scene_; }
    // ee-camera.log: one brief3d-latch line per 5 s window while latch mode is configured.
    bool take_latch_line(char* buf, size_t n) const noexcept {
        if (!latch_on_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> l(m_);
        latch::format_line(buf, n, lst_, cache_stats_, scene_ ? scene_->serial : 0, scene_ ? scene_->geom_bytes : 0, scene_ ? scene_->records : 0, latch_why_,
                           latch_fallback_.load(std::memory_order_relaxed), fallback_why_);
        return true;
    }
    // The glue's read statistics (hook-state mutex: the cache lives in the glue), copied for the line.
    void set_read_stats(const latch::ReadStats& r) noexcept { std::lock_guard<std::mutex> l(m_); cache_stats_ = r; }    // ---- UI3D draw path (game render thread; ui-3d.md 4 items 2-3). Every draw the seam sees while ui3d is on and not held comes here.
    // record = true: a RECORDING RESERVATION for (slot, frame); the glue writes plan.vtx[e] at plan.upload_offset[e] for each eye_draw[e], records
    // the sequence, then calls ui3d_commit(plan) (or ui3d_cancel(plan) if nothing reached the list).
    UiPlan ui3d_draw(const UiFacts& f, const PoseIn& pose, const Completion& fence) noexcept {
        namespace u3 = acz::ui3d;
        std::lock_guard<std::mutex> l(m_);
        UiPlan p;
        const bool open_ui = open_ >= 0 && run_[open_].ui;
        // a refusal: inside an open UI3D frame it REJECTS the frame (fail closed: nothing of it is shown); without one nothing opens
        auto refuse = [&](const char* why) {
            if (open_ >= 0 && run_[open_].ui) {
                if (run_[open_].hangar) ui_hold_locked(f.now_ms);   // stage 3b: a rejected hangar frame = the flat screen for kUiHoldMs
                ui_reject_locked(why);
            } else ui_last_why_ = why;
            ++ui_refused_draws_;
            p.why = why; return p;
        };
        if (!ui_on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) { p.why = poison_why_ ? poison_why_ : "ui3d-off"; return p; }
        ui_seen_draw_ = true;   // stage 3b: a hangar frame opens only at the first draw of a game frame
        // stage 3a: a PAUSE frame is open, or none is open and the pause trigger is live (u3p:1). Menu frames keep the stage-2 rules exactly.
        const bool open_pause = open_ui && run_[open_].pause;
        const bool pause = open_pause || (!open_ui && uo_.pause && pause_live_.load(std::memory_order_relaxed));
        if (f.vs8 != u3::kPs2Vs8) {
            if (open_pause) { ui_pause_closed_ = true; p.why = "ui3d-pause-window-closed"; return p; }   // the menu window ended (no hold)
            if (pause) { ++ui_pause_prefix_; p.why = "ui3d-pause-prefix"; return p; }                    // the paused HUD/world: not ours
            ui_hold_locked(f.now_ms); return refuse(ui_other_why(f.vs8));
        }
        // DEBRIEF-3D stage 2: the debrief window quad arms the debrief route (from the NEXT draw on: the glue asks dbr3d_armed per draw). Observed
        // before any refusal: the debrief's first frames are rejected here at their first indexed draw, before the window quad.
        // 2026-10-30 (step146 regression): never while the pause trigger is live or a pause frame is open. The in-flight pause menu's dimming
        // panel carries the debrief window signature (ps2_v / d34ee153, SRC_ALPHA / INV_SRC_ALPHA, z 2^24, large); arming on it routed every
        // later ps2_v draw of the paused frames to dbr3d_draw (the open pause frame rejected as dbr3d-menu-frame-open, then DEBRIEF frames opened
        // and rejected by the paused world's draws: ui3d-frame-has-other-shader), so no pause frame was published (step146 runAGU1: arms=2 =
        // the two pauses, pause-frames 2, pause-published 0).
        if (dbr_on_.load(std::memory_order_relaxed) && !pause && !f.indexed && !f.vb_why && !f.cb_why && f.vertices == 4)
            dbr_observe_locked(f.now_ms, f.vs8, f.ps8, false, f.cb0, f.blend, f.blend_src, f.blend_dst, f.vtx, 4);
        if (!gpu_) { p.why = "ui3d-gpu-not-ready"; return p; }
        if (open_ >= 0 && !run_[open_].ui) { p.why = "ui3d-table-frame-open"; return p; }   // the briefing table owns this frame
        if (open_ui && run_[open_].reset) return refuse("ui3d-draw-after-reset");   // the frame's list was Reset before the Present: nothing more may join it
        if (!pause && !open_ui && f.now_ms < ui_hold_until_.load(std::memory_order_relaxed)) { p.why = "ui3d-hold"; return p; }
        if (open_ui && run_[open_].list != f.list) return refuse("ui3d-frame-other-list");
        if (open_ui && run_[open_].hangar) {   // stage 3b: the ps2_v draws of a hangar frame (ui-3d.md 9.2)
            namespace hz = acz::ui3d::hangar;
            if (!(f.rt != nullptr && f.rt == ui_rt_)) { p.why = "ui3d-off-target"; return p; }
            float z = std::nanf("");
            if (!f.indexed) {
                if (f.vb_why) return refuse(f.vb_why);
                std::memcpy(&z, f.vtx + 8, 4);
                if (!std::isfinite(z)) return refuse("u3a-z-unknown");
            }
            const bool effect = hz::classify_ps2(f.indexed, z) == hz::Ps2Class::effect;
            if (!hz_overlay_) {
                // 2026-10-30c (u3ag:1, default): a non-indexed effect quad (the ceiling-light glows) is re-projected into the room; an indexed one
                // (the floor shadow, vertices unread) and every glow whose facts fail are dropped as before (never a frame rejection)
                if (effect && uo_.hangar_glow && !f.indexed) return hz_glow_locked(f);
                if (effect) { ++hz_fx_; p.why = "u3a-fx-dropped"; return p; }   // the floor shadow (u3ag:0: the glows too): not replayed
                hz_overlay_ = true;                                              // the 2D overlay starts: stage-2 rules from here
            } else if (effect) {
                // L11 fix: after the overlay started, a real-depth quad is a 2D draw of the overlay (the flat image paints it there): replayed
                // by the stage-2 rules (counted u3a-late). 2026-10-30 (step133 headset, live-1144): an INDEXED draw (vertices unread, so it cannot be
                // placed on a layer) used to reject the frame and hold the room off for 1 s per occurrence (17 times in one visit = the spotty
                // hangar). It is now DROPPED for this frame (counted u3a-dropped): the room and the replayed overlay stay; the game's own draw
                // still paints it on the flat image only.
                if (f.indexed) { ++hz_dropped_; p.why = "u3a-late-indexed-dropped"; return p; }
                ++hz_late_;
            }
        }
        // L11 fix: a game frame whose hangar ACM draw could not open (hold / not the first draw) is SKIPPED as a whole: its ps2_v draws (glows,
        // shadow, overlay) never open a stage-2 menu frame and never refuse (no hold renewal)
        if (!open_ui && uo_.hangar && ui_skip_frame_) { ++hz_skipped_draws_; p.why = "u3a-frame-skipped"; return p; }
        const bool on_target = open_ui ? f.rt != nullptr && f.rt == ui_rt_ : true;
        if (open_pause && ui_pause_closed_) {   // a non-ps2_v draw ended the window: more menu draws on the target = an unknown structure
            if (on_target) return refuse("ui3d-pause-draw-after-3d");
            p.why = "ui3d-off-target"; return p;
        }
        if (pause && !open_ui) {   // before the window: only a large ps2_v draw with readable geometry may open it; anything else is the HUD
            const u3::SourceRect wr = u3::rect_from_viewport(f.vp[0], f.vp[1], f.vp[2], f.vp[3], double(f.rt_w), double(f.rt_h));
            double bw = 0, bh = 0;
            if (f.indexed || f.cb_why || f.vb_why || !f.rs_known || f.vertices == 0 || f.vertices > u3::kMaxVerticesPerDraw || !u3::cb0_ok(f.cb0) ||
                !u3::rect_ok(wr) || !u3::screen_bbox(f.vtx, f.vertices, f.cb0, wr, bw, bh) || !u3::pause::opens_window(bw, bh)) {
                ++ui_pause_prefix_; p.why = "ui3d-pause-prefix"; return p;
            }
        }
        u3::DrawFacts d; d.vs8 = f.vs8; d.on_target = on_target; d.indexed = f.indexed; d.vertices = f.vertices;
        if (on_target) {   // the facts of a draw that is replayed (off-target draws are skipped and need none)
            if (f.indexed) return refuse("ui3d-indexed-draw");
            if (f.gs) return refuse("ui3d-gs-draw");
            if (f.topology_type < 1 || f.topology_type > 3) return refuse("ui3d-topology");
            if (f.instances != 1) return refuse("ui3d-instances");
            if (f.vertices == 0 || f.vertices > u3::kMaxVerticesPerDraw) return refuse("ui3d-draw-too-many-vertices");
            if (!f.pso_known) return refuse("ui3d-pso-unknown");
            if (f.rt_count != 1 || f.rtv0 != kRtvFormat || f.samples != 1) return refuse("ui3d-pso-rtv");
            if (f.dsv != 0 && f.dsv != kDsvFormat) return refuse("ui3d-pso-dsv");
            if (f.depth_enable && f.depth_func != kCmpAlways) return refuse("ui3d-depth-func");   // census: ALWAYS (8) or off [E]
            if (f.stencil && !f.stencil_always) return refuse("ui3d-stencil-not-always");     // the eye stencil is ours (cleared 0)
            if (!f.rs_known) return refuse("ui3d-viewport-unknown");
            if (f.viewports != 1 || f.scissors != 1) return refuse("ui3d-viewport-count");   // census: 1 / 1 [E]; exactly restorable
            if (!f.om_known) return refuse("ui3d-om-unknown");
            if (!f.rt || !f.rt_w || !f.rt_h || f.rt_fmt != kRtvFormat) return refuse("ui3d-target-unknown");
            if (f.cb_why) return refuse(f.cb_why);
            if (!u3::cb0_ok(f.cb0)) return refuse("ui3d-cb0");
            if (f.vb_why) return refuse(f.vb_why);
            if (f.stride != u3::kVertexStride) return refuse("ui3d-vb-stride");
        }
        const u3::SourceRect rect = u3::rect_from_viewport(f.vp[0], f.vp[1], f.vp[2], f.vp[3], double(f.rt_w), double(f.rt_h));
        if (on_target) {
            if (!u3::rect_ok(rect)) return refuse("ui3d-viewport");
            if (!u3::screen_bbox(f.vtx, f.vertices, f.cb0, rect, d.bbox_w, d.bbox_h)) return refuse("ui3d-bbox");
            std::memcpy(&d.z, f.vtx + 8, 4); d.z_known = std::isfinite(d.z);
        }
        if (!open_ui) {   // open a UI3D frame at this draw
            if (!on_target) { p.why = "ui3d-off-target"; return p; }
            for (const Run& r : run_) if (r.state == run_writing && r.list == f.list) return refuse("ui3d-list-holds-previous-frame");
            if (!pose.ok || !pose.space_generation) { ++ui_no_pose_; return refuse("ui3d-no-pose"); }
            u3::Screen scr;
            if (pause) {   // stage 3a: the pause placement (head yaw at the first pause frame, lens size), latched while the pause goes on
                if (!ui_pause_screen_.valid || ui_pause_gen_ != pose.space_generation || f.now_ms - ui_pause_last_ms_ > u3::pause::kPlacementGapMs) {
                    ui_pause_screen_ = u3::pause::place(ui_eye(pose.eyes[0]), ui_eye(pose.eyes[1]));
                    ui_pause_gen_ = pose.space_generation;
                    if (ui_pause_screen_.valid) ++ui_pause_placements_;
                }
                if (!ui_pause_screen_.valid) return refuse("ui3d-pause-placement-invalid");
                scr = ui_pause_screen_;
            } else {
                const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
                if (!sp.valid || sp.generation != pose.space_generation) return refuse("ui3d-no-screen-placement");
                scr = u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor);
                if (!scr.valid) return refuse("ui3d-screen-invalid");
                scr = u3::scaled(scr, double(uo_.scale));   // u3s: the menu panel floats smaller than the vscreen framing
                if (!scr.valid) return refuse("ui3d-screen-invalid");
            }
            ui_lopts_ = uo_;   // the layer distances of this frame: menus use u3mn / u3mf (a wider depth separation), the pause menu keeps u3n / u3f
            if (!pause) { ui_lopts_.near_m = uo_.menu_near_m; ui_lopts_.far_m = uo_.menu_far_m; }
            const int s = ring_.begin_write(fence);
            if (s < 0) { ++st_.no_slot; return refuse(fence.lost ? "ui3d-fence-lost" : "ui3d-no-free-slot"); }
            Run& r = run_[s]; r = Run{}; r.state = run_writing; r.list = f.list; r.frame = ++frame_serial_; r.open = true; r.ui = true; r.pause = pause;
            meta_[s] = SlotMeta{}; meta_[s].frame = r.frame; meta_[s].eyes[0] = pose.eyes[0]; meta_[s].eyes[1] = pose.eyes[1];
            meta_[s].space_generation = pose.space_generation; meta_[s].opaque = !pause; meta_[s].pause = pause;
            if (pause) { ui_pause_last_ms_ = f.now_ms; ui_pause_closed_ = false; ++ui_pause_frames_; }
            ui_assigner_ = u3::LayerAssigner{}; ui_rt_ = f.rt; ui_screen_ = scr; ui_culled_ = 0;
            ui_eye_[0] = ui_eye(pose.eyes[0]); ui_eye_[1] = ui_eye(pose.eyes[1]);
            ui_rows_roll_locked();
            open_ = s;
        }
        const int layer = ui_assigner_.assign(d);
        if (layer == u3::kSkip) { p.why = "ui3d-off-target"; return p; }   // the final pass to another target: not replayed, the frame goes on
        if (layer == u3::kRefused) return refuse(ui_assigner_.why());
        Run& r = run_[open_];
        // 2026-10-30c: the frame's text rows (for the next frame's selection rule), the hangar tint, the selection box
        u3::Box box;
        const bool have_box = u3::screen_box(f.vtx, f.vertices, f.cb0, rect, box);
        const bool is_small = d.bbox_w > 0 && d.bbox_h > 0 && d.bbox_w <= u3::kSmallDim && d.bbox_h <= u3::kSmallDim;
        const bool is_large = d.bbox_w * d.bbox_h >= u3::kLargeArea;
        if (have_box && is_small && layer == int(u3::kLayers) - 1) ui_rows_cur_.add_glyph(box);
        // u3at:0 (default): the hangar overlay's full-screen draws (the yellow tint / grid / frame quads, the z-0 dim) are not replayed. The
        // classifier saw the draw above (its state is the same as before); nothing of it reaches the eye images.
        if (r.hangar && is_large && !uo_.hangar_tint) { ++hz_tint_; p.why = "u3a-tint-dropped"; return p; }
        const bool sel = uo_.select != 0 && have_box && !is_small && !is_large && ui_rows_prev_.selects(box);
        // 2026-10-30d (u3pp): the placement surface (the panel layer on the text plane: one surface per panel); the classifier layer is unchanged
        const unsigned place = u3::placement_layer(ui_lopts_, unsigned(layer));
        // 2026-10-31 (ui-3d.md 14): any stage-2/3 option on: a non-backdrop draw goes through the surface path (panel groups, raised selection /
        // text, shaded rims); the backdrop and every draw with all of them off keep the stage-1 path below byte for byte
        if (ui_lopts_.reinterpret() && layer >= 1 && have_box) return ui3d_draw_s23_locked(f, d, rect, box, layer, sel, p);
        // 2026-10-30d (u3lt): a panel / frame / tile quad as a rim (edge walls behind its surface + the front face). It falls back to the flat
        // quad when the frame's upload span cannot hold the 5 copies (the frame is never refused for a rim).
        bool rim = !sel && ui_lopts_.frame_mm > 0.0f && u3::frame_quad(f.vertices, d.bbox_w, d.bbox_h, d.z_known, d.z);
        if (rim && r.ui_bytes + 2 * uint64_t(f.vertices) * u3::kVertexStride * u3::kRimCopies > u3::kUploadPerSlot) { rim = false; ++ui_rim_fallback_; }
        const uint32_t copies = sel && uo_.select == 2 && f.vertices == 4 ? u3::kSlabCopies : rim ? u3::kRimCopies : 1u;
        const double sel_front = rim ? u3::rim_front(ui_lopts_, place) : u3::selection_front(ui_lopts_);
        const double sel_back = rim ? u3::rim_back(ui_lopts_, place) : u3::selection_back(ui_lopts_);
        const bool surf = sel || rim;   // an explicit front / back surface pair (the slab, the flat selection face, the rim)
        const uint64_t need = uint64_t(f.vertices) * u3::kVertexStride * copies;
        if (r.ui_bytes + 2 * need > u3::kUploadPerSlot) return refuse("ui3d-upload-full");
        for (unsigned e = 0; e < 2; ++e) {
            u3::RewriteResult rw;
            if (!surf) rw = u3::rewrite_vertices(f.vtx, f.vertices, f.cb0, rect, ui_screen_, ui_lopts_, place, ui_eye_[e], p.vtx[e]);
            else if (rim) rw = u3::rewrite_rim(f.vtx, f.vertices, f.cb0, rect, ui_screen_, ui_lopts_, sel_front, sel_back, ui_eye_[e], p.vtx[e]);
            else if (copies > 1) rw = u3::rewrite_slab(f.vtx, f.vertices, f.cb0, rect, ui_screen_, ui_lopts_, sel_front, sel_back, ui_eye_[e], p.vtx[e]);
            else rw = u3::rewrite_at(f.vtx, f.vertices, f.cb0, rect, ui_screen_, ui_lopts_, sel_front, 1.0, ui_eye_[e], p.vtx[e]);
            if (rw.why) return refuse(rw.why);
            bool draw = !rw.culled;
            if (draw) {
                u3::PixRect pr;
                if (!surf) draw = u3::eye_scissor(f.sc, double(f.rt_w), double(f.rt_h), ui_screen_, ui_lopts_, place, ui_eye_[e], kEyeW, kEyeH, pr);
                else {   // the game's scissor carried onto the slab's / rim's front and back faces (their union: the walls lie between them)
                    u3::PixRect pb;
                    const bool df = u3::eye_scissor_at(f.sc, double(f.rt_w), double(f.rt_h), ui_screen_, ui_lopts_, sel_front, 1.0, ui_eye_[e], kEyeW, kEyeH, pr);
                    const bool db = copies > 1 && u3::eye_scissor_at(f.sc, double(f.rt_w), double(f.rt_h), ui_screen_, ui_lopts_, sel_back, 1.0, ui_eye_[e], kEyeW, kEyeH, pb);
                    if (df && db) { pr.l = std::min(pr.l, pb.l); pr.t = std::min(pr.t, pb.t); pr.r = std::max(pr.r, pb.r); pr.b = std::max(pr.b, pb.b); }
                    else if (db) pr = pb;
                    draw = df || db;
                }
                p.scissor[e][0] = pr.l; p.scissor[e][1] = pr.t; p.scissor[e][2] = pr.r; p.scissor[e][3] = pr.b;
            }
            p.eye_draw[e] = draw;
            if (!draw) ++ui_culled_;
        }
        if (!p.eye_draw[0] && !p.eye_draw[1] && r.clear_granted) { p.why = "ui3d-culled"; return p; }   // nothing to record (the clear is done)
        const uint64_t base = kUiUploadBase + uint64_t(open_) * u3::kUploadPerSlot + r.ui_bytes;
        p.upload_offset[0] = base; p.upload_offset[1] = base + need;
        r.ui_bytes += 2 * need;
        if (sel) ++ui_sel_;
        if (rim) ++ui_rim_;
        p.copies = copies;
        p.record = true; p.slot = open_; p.frame = r.frame; p.layer = layer; p.vertices = f.vertices; p.bind_dsv = f.dsv == kDsvFormat;
        p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record;
        return p;
    }
    // 2026-10-30c: a new UI3D frame opened: the last frame's text rows become the selection reference, the new frame collects its own.
    void ui_rows_roll_locked() noexcept {
        ui_rows_prev_ = ui_rows_cur_; ui_rows_cur_.clear();
        // 2026-10-31 (u3gp): the last frame's panel rectangles become this frame's groups; the frame's selection box is not known yet
        ui_groups_prev_ = ui_groups_cur_; ui_groups_prev_.finish(); ui_groups_cur_.clear(); ui_have_sel_box_ = false;
        if (uo_.groups) { ui_grp_n_ = ui_groups_prev_.size(); ui_grp_dropped_ += ui_groups_prev_.dropped(); }
    }
    // 2026-10-31 (ui-3d.md 14; ui3d_groups.h): the stage-2/3 surface path of ui3d_draw for a non-backdrop draw of an open UI3D frame (the draw's
    // facts are checked and the classifier has placed it). The draw's PANE is its panel group's plane (u3gp:1; else the base text plane); a frame
    // quad is a rim on it (u3lt; + its back face with u3pb), the selection bar a slab raised in front of it (u3sr) or behind it (the u3sel rule),
    // text on it (u3tx in front; on the raised selection's front face inside the frame's selection box), anything else flat on it. Every copy lies
    // on its ray from the anchor (the flat image from there). Upload: a shape that does not fit (with gp::kShapeReserve kept free) steps down
    // (no back face, then flat) before the frame is refused, as the stage-1 rim.
    UiPlan ui3d_draw_s23_locked(const UiFacts& f, const acz::ui3d::DrawFacts& d, const acz::ui3d::SourceRect& rect, const acz::ui3d::Box& box,
                                int layer, bool sel, UiPlan& p) noexcept {
        namespace u3 = acz::ui3d; namespace gp = acz::ui3d::groups;
        Run& r = run_[open_];
        const u3::Options& lo = ui_lopts_;
        auto refuse = [&](const char* why) {
            if (r.hangar) ui_hold_locked(f.now_ms);   // as ui3d_draw: a rejected hangar frame = the flat screen for kUiHoldMs
            ui_reject_locked(why); ++ui_refused_draws_; p.why = why; return p;
        };
        const bool frame = !sel && u3::frame_quad(f.vertices, d.bbox_w, d.bbox_h, d.z_known, d.z);
        if (lo.groups && frame) ui_groups_cur_.add(box);
        // a full-screen draw (a tint / dim drawn late, e.g. the z-0 dim of the menus) never joins a group: on a turned group plane it would
        // spread far beyond its panel; it stays on the base surface (the stage-1 u3pp vertices)
        const bool large = d.bbox_w * d.bbox_h >= u3::kLargeArea;
        const gp::Pane pane = gp::pane_for(ui_screen_, lo, large ? nullptr : &ui_groups_prev_, box);
        gp::Shape sh;
        bool on_sel = false;
        if (sel) { sh = gp::shape_selection(lo, pane, f.vertices); ui_sel_box_ = box; ui_have_sel_box_ = true; }
        else if (frame && lo.frame_mm > 0.0f) sh = gp::shape_rim(lo, pane);
        else if (layer == int(u3::kLayers) - 1 && !frame) {
            const double cx = 0.5 * (box.x0 + box.x1), cy = 0.5 * (box.y0 + box.y1);
            on_sel = ui_have_sel_box_ && cx >= ui_sel_box_.x0 && cx <= ui_sel_box_.x1 && cy >= ui_sel_box_.y0 && cy <= ui_sel_box_.y1;
            sh = gp::shape_text(lo, pane, on_sel);
        } else sh = gp::shape_flat(pane);
        auto need = [&](unsigned copies) { return uint64_t(f.vertices) * u3::kVertexStride * copies; };
        // a multi-copy shape keeps gp::kShapeReserve of the slot's span free (128 flat quads) so that rims never crowd out the frame's text
        while (sh.copies > 1 && r.ui_bytes + 2 * need(sh.copies) + gp::kShapeReserve > u3::kUploadPerSlot) {
            const bool was_rim = sh.role == gp::Role::rim;
            gp::downgrade(sh);
            if (was_rim && sh.copies == 1) ++ui_rim_fallback_;
        }
        const uint32_t copies = sh.copies;
        if (r.ui_bytes + 2 * need(copies) > u3::kUploadPerSlot) return refuse("ui3d-upload-full");
        gp::ShadeCount shc[2];
        for (unsigned e = 0; e < 2; ++e) {
            const u3::RewriteResult rw = gp::rewrite_shape(f.vtx, f.vertices, f.cb0, rect, ui_screen_, sh, ui_eye_[e], p.vtx[e]);
            if (rw.why) return refuse(rw.why);
            shc[e] = gp::shade(p.vtx[e], f.vtx, f.cb0, rect, sh, lo.bevel, lo.back_alpha);
            bool draw = !rw.culled;
            if (draw) {   // the game's scissor carried onto the front surface (and the back one: the walls lie between them)
                u3::PixRect pr, pb;
                const bool df = u3::eye_scissor_surf(f.sc, double(f.rt_w), double(f.rt_h), ui_screen_, sh.front, ui_eye_[e], kEyeW, kEyeH, pr);
                const bool db = copies > 1 && u3::eye_scissor_surf(f.sc, double(f.rt_w), double(f.rt_h), ui_screen_, sh.back, ui_eye_[e], kEyeW, kEyeH, pb);
                if (df && db) { pr.l = std::min(pr.l, pb.l); pr.t = std::min(pr.t, pb.t); pr.r = std::max(pr.r, pb.r); pr.b = std::max(pr.b, pb.b); }
                else if (db) pr = pb;
                draw = df || db;
                p.scissor[e][0] = pr.l; p.scissor[e][1] = pr.t; p.scissor[e][2] = pr.r; p.scissor[e][3] = pr.b;
            }
            p.eye_draw[e] = draw;
            if (!draw) ++ui_culled_;
        }
        if (!p.eye_draw[0] && !p.eye_draw[1] && r.clear_granted) { p.why = "ui3d-culled"; return p; }
        const uint64_t base = kUiUploadBase + uint64_t(open_) * u3::kUploadPerSlot + r.ui_bytes;
        p.upload_offset[0] = base; p.upload_offset[1] = base + need(copies);
        r.ui_bytes += 2 * need(copies);
        if (sel) ++ui_sel_;
        if (sh.role == gp::Role::rim && copies > 1) ++ui_rim_;
        if (pane.group >= 0) ++ui_grp_; else ++ui_grp_base_;
        if (on_sel && lo.sel_raise_mm > 0.0f) ++ui_sel_text_;
        ui_bevel_ += shc[0].walls; ui_pane_back_ += shc[0].back; ui_shade_skip_ += shc[0].skipped;
        p.copies = copies;
        p.record = true; p.slot = open_; p.frame = r.frame; p.layer = layer; p.vertices = f.vertices; p.bind_dsv = f.dsv == kDsvFormat;
        p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record;
        return p;
    }
    // 2026-10-30c (u3ag:1; ui3d_hangar.h rewrite_effect): one pre-projected light glow of an open hangar frame (before its overlay), re-projected
    // through the frame's game camera into the latched room for each eye; scissor = the whole eye image (the room is not cut to the 4:3 window,
    // as the ACM replays). Any unusable fact DROPS the glow (counted u3a-fx, reason u3a-glow-why): a glow never rejects the room.
    UiPlan hz_glow_locked(const UiFacts& f) noexcept {
        namespace u3 = acz::ui3d; namespace hz = acz::ui3d::hangar;
        UiPlan p;
        auto drop = [&](const char* why) { ++hz_fx_; hz_glow_why_ = why; p.why = "u3a-fx-dropped"; return p; };
        if (f.gs || f.topology_type < 1 || f.topology_type > 3 || f.instances != 1 || f.vertices == 0 || f.vertices > u3::kMaxVerticesPerDraw) return drop("u3a-glow-draw");
        if (!f.pso_known || f.rt_count != 1 || f.rtv0 != kRtvFormat || f.samples != 1 || (f.dsv != 0 && f.dsv != kDsvFormat)) return drop("u3a-glow-pso");
        if (f.depth_enable && f.depth_func != kCmpAlways) return drop("u3a-glow-depth");   // K10: dp=1/8/0 (ALWAYS, no write) [E]
        if (f.stencil && !f.stencil_always) return drop("u3a-glow-stencil");
        if (!f.rs_known || f.viewports != 1 || f.scissors != 1 || !f.om_known) return drop("u3a-glow-state");
        if (!f.rt || !f.rt_w || !f.rt_h || f.rt_fmt != kRtvFormat) return drop("u3a-glow-target");
        if (f.cb_why || !u3::cb0_ok(f.cb0) || f.vb_why || f.stride != u3::kVertexStride) return drop("u3a-glow-vertices");
        const u3::SourceRect rect = u3::rect_from_viewport(f.vp[0], f.vp[1], f.vp[2], f.vp[3], double(f.rt_w), double(f.rt_h));
        // the camera's ndc spans the whole target (k400 over 2560 x 1440 [E]); a draw with another viewport would map elsewhere
        if (!(std::fabs(rect.x0) < 1e-3 && std::fabs(rect.y0) < 1e-3 && std::fabs(rect.w - 1.0) < 1e-3 && std::fabs(rect.h - 1.0) < 1e-3)) return drop("u3a-glow-viewport");
        if (open_ < 0 || !run_[open_].hangar) return drop("u3a-glow-no-frame");
        Run& r = run_[open_];
        // 2026-10-30d (u3agm, ui3d_hangar.h GlowMemory): a 4-vertex glow with a known state key goes through the memory
        if (uo_.glow_memory != 0 && f.state_key != 0 && f.vertices == 4) return hz_glow_mem_locked(f, p);
        const uint64_t need = uint64_t(f.vertices) * u3::kVertexStride;
        if (r.ui_bytes + 2 * need > u3::kUploadPerSlot) return drop("u3a-glow-upload-full");
        for (unsigned e = 0; e < 2; ++e) {
            const u3::RewriteResult rw = hz::rewrite_effect(f.vtx, f.vertices, f.cb0, hz_prev_cam_, hz_room_, ui_eye_[e], p.vtx[e]);
            if (rw.why) return drop(rw.why);
            p.eye_draw[e] = !rw.culled;
            p.scissor[e][0] = 0; p.scissor[e][1] = 0; p.scissor[e][2] = int32_t(kEyeW); p.scissor[e][3] = int32_t(kEyeH);
        }
        if (!p.eye_draw[0] && !p.eye_draw[1] && r.clear_granted) { p.why = "u3a-glow-culled"; return p; }
        const uint64_t base = kUiUploadBase + uint64_t(open_) * u3::kUploadPerSlot + r.ui_bytes;
        p.upload_offset[0] = base; p.upload_offset[1] = base + need;
        r.ui_bytes += 2 * need;
        ++hz_glow_;
        p.record = true; p.slot = open_; p.frame = r.frame; p.layer = -1; p.vertices = f.vertices; p.copies = 1; p.bind_dsv = f.dsv == kDsvFormat;
        p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record;
        return p;
    }
    // 2026-10-30d (u3agm): the glow memory path of hz_glow_locked (the draw's facts are already checked). The real glow refreshes (or enters)
    // the memory; the plan draws, as consecutive copies of this draw (its PSO, textures, blend), first this glow unless the memory already drew
    // it in this frame, then every remembered glow with the same state key that this frame has not drawn and that the game drew no later in its
    // order than here (acm_before <= the ACM draws so far), up to kGlowCarrierQuads and the slot's upload span. A single first-seen glow gives
    // exactly the plan of the memory-less path (one copy, the same vertices).
    UiPlan hz_glow_mem_locked(const UiFacts& f, UiPlan& p) noexcept {
        namespace u3 = acz::ui3d; namespace hz = acz::ui3d::hangar;
        Run& r = run_[open_];   // the open hangar frame (hz_glow_locked checked it)
        const uint64_t F = r.frame;
        double c[3], rad = 0;
        if (const char* why = hz::effect_centre(f.vtx, 4, f.cb0, hz_prev_cam_, c, rad)) { ++hz_fx_; hz_glow_why_ = why; p.why = "u3a-fx-dropped"; return p; }
        int idx = glow_mem_.match(f.state_key, c, rad, F);
        const bool already = idx >= 0 && glow_mem_.at(unsigned(idx)).drawn == F;   // drawn from memory earlier in this frame
        idx = glow_mem_.remember(idx, f.state_key, f.vtx, f.cb0, hz_prev_cam_, c, rad, hz_acm_, F);
        // the copies: -1 = this draw's own vertices (memory full), else a memory index
        int list[hz::kGlowCarrierQuads]; unsigned n = 0;
        if (idx < 0) list[n++] = -1;
        else if (!already) list[n++] = idx;
        unsigned fit = hz::kGlowCarrierQuads;   // the slot's upload span (both eyes)
        while (fit > 0 && r.ui_bytes + 2ull * fit * 4u * u3::kVertexStride > u3::kUploadPerSlot) --fit;
        if (n > fit) { hz_glow_why_ = "u3a-glow-upload-full"; ++hz_fx_; p.why = "u3a-fx-dropped"; return p; }
        n += glow_mem_.pending(f.state_key, hz_acm_, F, idx, fit - n, list + n);
        if (n == 0) { ++hz_glow_; p.why = "u3a-glow-remembered"; return p; }   // already drawn from memory, nothing else to carry: nothing to record
        bool any[2] = {false, false};
        for (unsigned k = 0; k < n; ++k) {
            const bool own = list[k] < 0;
            const hz::GlowEntry* g = own ? nullptr : &glow_mem_.at(unsigned(list[k]));
            for (unsigned e = 0; e < 2; ++e) {
                uint8_t* q = p.vtx[e] + size_t(k) * 4u * u3::kVertexStride;
                const u3::RewriteResult rw = own ? hz::rewrite_effect(f.vtx, 4, f.cb0, hz_prev_cam_, hz_room_, ui_eye_[e], q)
                                                 : hz::rewrite_effect(g->src, 4, g->cb, g->cam, hz_room_, ui_eye_[e], q);
                if (rw.why) {
                    if (k == 0 && (own || list[k] == idx)) {   // this glow: the old drop (and not remembered)
                        if (idx >= 0) glow_mem_.forget(unsigned(idx));
                        ++hz_fx_; hz_glow_why_ = rw.why; p.why = "u3a-fx-dropped"; return p;
                    }
                    std::memcpy(q, g->src, 4u * u3::kVertexStride); hz::degenerate_quad(q);   // a remembered one that no longer re-projects:
                    glow_mem_.forget(unsigned(list[k]));                                       // drawn as nothing, forgotten at the next frame
                    continue;
                }
                if (rw.culled) hz::degenerate_quad(q); else any[e] = true;
            }
        }
        for (unsigned k = 0; k < n; ++k) if (list[k] >= 0) {
            glow_mem_.at(unsigned(list[k])).drawn = F;
            if (list[k] != idx) ++hz_glow_mem_;   // a remembered glow the game did not emit (yet) in this frame
        }
        for (unsigned e = 0; e < 2; ++e) {
            p.eye_draw[e] = any[e];
            p.scissor[e][0] = 0; p.scissor[e][1] = 0; p.scissor[e][2] = int32_t(kEyeW); p.scissor[e][3] = int32_t(kEyeH);
        }
        if (!p.eye_draw[0] && !p.eye_draw[1] && r.clear_granted) { p.why = "u3a-glow-culled"; return p; }
        ++hz_glow_;   // a real glow of the game, re-projected (as the memory-less path: counted when recorded or already drawn from memory)
        const uint64_t need = uint64_t(n) * 4u * u3::kVertexStride;
        const uint64_t base = kUiUploadBase + uint64_t(open_) * u3::kUploadPerSlot + r.ui_bytes;
        p.upload_offset[0] = base; p.upload_offset[1] = base + need;
        r.ui_bytes += 2 * need;
        p.record = true; p.slot = open_; p.frame = r.frame; p.layer = -1; p.vertices = 4; p.copies = n; p.bind_dsv = f.dsv == kDsvFormat;
        p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record;
        return p;
    }
    void ui3d_commit(const UiPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++ui_replays_; ++meta_[p.slot].replays;
        settle_locked(p.slot);
    }
    void ui3d_cancel(const UiPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++st_.cancelled;
        if (p.clear && !r->invalid) { r->invalid = true; ++st_.invalidated; }
        settle_locked(p.slot);
    }
    // ---- DEBRIEF-3D stage 2 (dbr3d:1; debrief-3d-map.md 8; NOT run live): one ps2_v draw while dbr3d_armed() (game render thread).
    // A DEBRIEF frame is a UI3D frame (same ring / slot / list / fence lifetime, published OPAQUE, rejected by any refusal exactly like a menu
    // frame, and by any non-ps2_v draw through ui3d_draw). The draws before the frame's first non-indexed GS-mapped draw (indexed or pixel-space:
    // the grid, the flight paths, their labels) are re-rendered per eye on the wraparound; from that draw on (the result panel, its text batches,
    // the narration) on the UI3D panel layers. Indexed draws are allowed here (their referenced range was read); depth GREATER(_EQUAL) is allowed
    // with the PSO's DSV (the slot's eye depth image, cleared to 0 at the frame's first plan, as the game's own clear). The producer writes the two
    // eye copies into dst[0] / dst[1] (each >= nv x 64 B). record = true: a RECORDING RESERVATION (dbr3d_commit / dbr3d_cancel).
    // (review 2026-10-31 F2) list_dst[e] (optional, dbr3d::kMaxListIndices R16 each): an eye with a culled vertex gets its own index list there.
    DbrPlan dbr3d_draw(const DbrFacts& df, const PoseIn& pose, const Completion& fence, uint8_t* const dst[2], uint16_t* const list_dst[2] = nullptr) noexcept {
        namespace u3 = acz::ui3d; namespace db = acz::dbr3d;
        std::lock_guard<std::mutex> l(m_);
        const UiFacts& f = df.ui;
        DbrPlan p;
        const bool open_ui = open_ >= 0 && run_[open_].ui;
        auto refuse = [&](const char* why) {
            if (open_ >= 0 && run_[open_].ui) ui_reject_locked(why); else ui_last_why_ = why;
            ++ui_refused_draws_; ++dbr_st_.refused_draws; dbr_st_.last_why = why;
            p.why = why; return p;
        };
        if (!ui_on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) { p.why = poison_why_ ? poison_why_ : "ui3d-off"; return p; }
        if (!dbr_on_.load(std::memory_order_relaxed)) { p.why = "dbr3d-off"; return p; }
        if (f.vs8 != u3::kPs2Vs8) { p.why = "dbr3d-not-ps2"; return p; }   // the glue routes only ps2_v draws here (others: ui3d_draw)
        ui_seen_draw_ = true;
        if (!f.indexed && !df.vtx_why && !f.cb_why && df.nv == 4)   // keeps the route armed while the window is drawn
            dbr_observe_locked(f.now_ms, f.vs8, f.ps8, false, f.cb0, f.blend, f.blend_src, f.blend_dst, df.vtx, 4);
        if (!gpu_) { p.why = "ui3d-gpu-not-ready"; return p; }
        if (open_ >= 0 && !run_[open_].ui) { p.why = "ui3d-table-frame-open"; return p; }
        if (open_ui && !run_[open_].dbr) return refuse("dbr3d-menu-frame-open");   // armed in the middle of a menu frame: that frame is not ours
        if (open_ui && run_[open_].reset) return refuse("ui3d-draw-after-reset");
        if (!open_ui && f.now_ms < ui_hold_until_.load(std::memory_order_relaxed)) { p.why = "ui3d-hold"; return p; }
        if (open_ui && run_[open_].list != f.list) return refuse("ui3d-frame-other-list");
        const bool on_target = open_ui ? f.rt != nullptr && f.rt == ui_rt_ : true;
        if (!on_target) { p.why = "ui3d-off-target"; return p; }   // a later pass to another target: not replayed, the frame goes on
        // the facts of a replayed draw (the stage-2 list, with indexed draws and depth GREATER(_EQUAL) + DSV admitted)
        if (f.gs) return refuse("ui3d-gs-draw");
        if (f.topology_type < 1 || f.topology_type > 3) return refuse("ui3d-topology");
        if (f.instances != 1) return refuse("ui3d-instances");
        if (!f.pso_known) return refuse("ui3d-pso-unknown");
        if (f.rt_count != 1 || f.rtv0 != kRtvFormat || f.samples != 1) return refuse("ui3d-pso-rtv");
        if (f.dsv != 0 && f.dsv != kDsvFormat) return refuse("ui3d-pso-dsv");
        if (f.depth_enable && f.depth_func != kCmpAlways) {
            if (f.depth_func != kCmpGreater && f.depth_func != kCmpGreaterEqual) return refuse("dbr3d-depth-func");
            if (f.dsv != kDsvFormat) return refuse("dbr3d-depth-without-dsv");
        }
        if (f.stencil && !f.stencil_always) return refuse("ui3d-stencil-not-always");
        if (!f.rs_known) return refuse("ui3d-viewport-unknown");
        if (f.viewports != 1 || f.scissors != 1) return refuse("ui3d-viewport-count");
        if (!f.om_known) return refuse("ui3d-om-unknown");
        if (!f.rt || !f.rt_w || !f.rt_h || f.rt_fmt != kRtvFormat) return refuse("ui3d-target-unknown");
        if (f.cb_why) return refuse(f.cb_why);
        if (!u3::cb0_ok(f.cb0)) return refuse("ui3d-cb0");
        if (df.vtx_why) return refuse(df.vtx_why);
        if (!df.vtx || df.nv == 0 || df.nv > db::kMaxVerticesPerDraw) return refuse("dbr3d-draw-too-many-vertices");
        if (f.indexed && (df.index_count == 0 || df.index_count > db::kMaxIndicesPerDraw || df.imin > uint32_t(INT32_MAX))) return refuse("dbr3d-index-facts");
        const u3::SourceRect rect = u3::rect_from_viewport(f.vp[0], f.vp[1], f.vp[2], f.vp[3], double(f.rt_w), double(f.rt_h));
        if (!u3::rect_ok(rect)) return refuse("ui3d-viewport");
        db::RouteIn ri; ri.indexed = f.indexed; ri.pixel_space = db::pixel_space(f.cb0); ri.nv = df.nv;
        if (!u3::screen_bbox(df.vtx, df.nv, f.cb0, rect, ri.bbox_w, ri.bbox_h)) return refuse("ui3d-bbox");
        db::GsRect wr;
        ri.window = !f.indexed && db::window_from_vertices(f.vs8, f.ps8, false, f.cb0, f.blend, f.blend_src, f.blend_dst, df.vtx, df.nv, wr);
        if (df.nv > 4) ri.quad_max = db::quad_max_dim(df.vtx, df.nv, f.cb0, rect);
        if (!open_ui) {   // open a DEBRIEF frame at this draw (the menu-frame rules: vscreen placement, menu layer distances)
            for (const Run& r : run_) if (r.state == run_writing && r.list == f.list) return refuse("ui3d-list-holds-previous-frame");
            if (!pose.ok || !pose.space_generation) { ++ui_no_pose_; return refuse("ui3d-no-pose"); }
            const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
            if (!sp.valid || sp.generation != pose.space_generation) return refuse("ui3d-no-screen-placement");
            const u3::Screen wrap = u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor);
            if (!wrap.valid) return refuse("ui3d-screen-invalid");
            const u3::Screen scr = u3::scaled(wrap, double(uo_.scale));
            if (!scr.valid) return refuse("ui3d-screen-invalid");
            const int s = ring_.begin_write(fence);
            if (s < 0) { ++st_.no_slot; return refuse(fence.lost ? "ui3d-fence-lost" : "ui3d-no-free-slot"); }
            Run& r = run_[s]; r = Run{}; r.state = run_writing; r.list = f.list; r.frame = ++frame_serial_; r.open = true; r.ui = true; r.dbr = true;
            meta_[s] = SlotMeta{}; meta_[s].frame = r.frame; meta_[s].eyes[0] = pose.eyes[0]; meta_[s].eyes[1] = pose.eyes[1];
            meta_[s].space_generation = pose.space_generation; meta_[s].opaque = true;
            ui_assigner_ = u3::LayerAssigner{}; ui_rt_ = f.rt; ui_screen_ = scr; ui_culled_ = 0;
            ui_lopts_ = db::panel_options(uo_, dbr_o_);
            ui_eye_[0] = ui_eye(pose.eyes[0]); ui_eye_[1] = ui_eye(pose.eyes[1]);
            dbr_wrap_ = wrap; dbr_wopts_ = db::wrap_options(uo_); dbr_router_.begin_frame(); dbr_frame_draws_ = 0;
            ++dbr_st_.frames;
            open_ = s;
        }
        Run& r = run_[open_];
        unsigned layer = 0;
        const db::Route route = dbr_router_.route(ri, layer);
        const bool map = route == db::Route::map;
        const double D = map ? double(dbr_o_.radius_m) : u3::layer_distance(ui_lopts_, layer);
        const double K = map ? double(dbr_o_.spread) : u3::layer_scale(ui_lopts_, layer);
        const u3::Screen& S = map ? dbr_wrap_ : ui_screen_;
        const u3::Options& O = map ? dbr_wopts_ : ui_lopts_;
        const uint64_t need = uint64_t(df.nv) * db::kVertexStride;
        if (r.dbr_bytes + 2 * need > db::kUploadPerSlot) return refuse("dbr3d-upload-full");
        uint64_t list_bytes[2] = {0, 0};
        for (unsigned e = 0; e < 2; ++e) {
            if (!dst || !dst[e]) return refuse("dbr3d-no-eye-buffer");
            const u3::RewriteResult rw = db::rewrite_surface(df.vtx, df.nv, f.cb0, rect, S, O, D, K, ui_eye_[e], dst[e], dbr_cull_);
            if (rw.why) return refuse(rw.why);
            bool draw = !rw.culled;
            // (F2) a vertex behind / too near this eye: keep the primitives that do not use one (own index list) instead of dropping the draw
            if (!draw && list_dst && list_dst[e] && db::list_topology(df.ia_topology) && (!f.indexed || df.ib)) {
                const db::EyeList el = db::cull_primitives(f.indexed, df.ib, df.ib_stride, df.strip_cut, f.indexed ? df.index_count : df.nv, df.imin, df.nv,
                                                           df.ia_topology, dbr_cull_, list_dst[e], db::kMaxListIndices);
                if (el.why) return refuse(el.why);
                if (el.count) {
                    draw = true; p.own_list[e] = true; p.list_count[e] = el.count;
                    list_bytes[e] = (uint64_t(el.count) * 2 + 3) & ~uint64_t(3);
                }
            }
            if (draw) {
                u3::PixRect pr;
                draw = u3::eye_scissor_at(f.sc, double(f.rt_w), double(f.rt_h), S, O, D, K, ui_eye_[e], kEyeW, kEyeH, pr);
                p.scissor[e][0] = pr.l; p.scissor[e][1] = pr.t; p.scissor[e][2] = pr.r; p.scissor[e][3] = pr.b;
            }
            p.eye_draw[e] = draw;
            if (!draw) { ++ui_culled_; ++dbr_st_.culled; p.own_list[e] = false; p.list_count[e] = 0; list_bytes[e] = 0; }
        }
        if (r.dbr_bytes + 2 * need + list_bytes[0] + list_bytes[1] > db::kUploadPerSlot) return refuse("dbr3d-upload-full");
        ++dbr_frame_draws_;
        if (map) ++dbr_st_.map_draws; else ++dbr_st_.panel_draws;
        if (!p.eye_draw[0] && !p.eye_draw[1] && r.clear_granted) { p.why = "dbr3d-culled"; return p; }   // nothing to record (the clear is done)
        const uint64_t base = kDbrUploadBase + uint64_t(open_) * db::kUploadPerSlot + r.dbr_bytes;
        p.upload_offset[0] = base; p.upload_offset[1] = base + need;
        p.list_offset[0] = base + 2 * need; p.list_offset[1] = p.list_offset[0] + list_bytes[0];
        p.list_topology = db::list_topology(df.ia_topology);
        r.dbr_bytes += 2 * need + list_bytes[0] + list_bytes[1];
        if (f.indexed) ++dbr_st_.indexed;
        dbr_st_.vertices += df.nv; dbr_st_.gaps += df.gaps;
        for (unsigned e = 0; e < 2; ++e) if (p.own_list[e]) ++dbr_st_.clipped;
        p.map = map; p.layer = map ? -1 : int(layer);
        p.indexed = f.indexed; p.nv = df.nv; p.base_vertex = f.indexed ? -int32_t(df.imin) : 0;
        p.record = true; p.slot = open_; p.frame = r.frame; p.bind_dsv = f.dsv == kDsvFormat;
        p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record;
        return p;
    }
    void dbr3d_commit(const DbrPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++ui_replays_; ++meta_[p.slot].replays;
        settle_locked(p.slot);
    }
    void dbr3d_cancel(const DbrPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++st_.cancelled;
        if (p.clear && !r->invalid) { r->invalid = true; ++st_.invalidated; }
        settle_locked(p.slot);
    }
    acz::dbr3d::LiveStats dbr3d_stats() const noexcept { std::lock_guard<std::mutex> l(m_); return dbr_st_; }
    // summary-dbr3d (dbr3d::format_live; bounded < 400 B). `armed` on the producer's last Present clock.
    int format_dbr3d(char* buf, size_t n) const noexcept {
        std::lock_guard<std::mutex> l(m_);
        const int64_t seen = dbr_seen_ms_.load(std::memory_order_relaxed);
        const bool armed = seen != 0 && now_ms_ >= seen && now_ms_ - seen <= acz::dbr3d::kArmMs;
        return acz::dbr3d::format_live(buf, n, dbr_st_, armed, dbr_latch_.screens);
    }
    // ---- UI3D stage 3b (u3a:1|2; ui-3d.md 9; NOT run live): one DrawACM_v draw (game render thread). Only called while ui3d_hangar_configured().
    // Outside a hangar frame and without the hangar screen constants it is exactly the stage-2 / 3a non-ps2_v rule (hold + refusal, or the pause
    // prefix). A refusal REJECTS an open hangar frame and holds UI3D off (the flat screen). record = true: a RECORDING RESERVATION (see AcmPlan).
    AcmPlan ui3d_acm(const AcmFacts& f, const PoseIn& pose, const Completion& fence) noexcept {
        namespace u3 = acz::ui3d; namespace hz = acz::ui3d::hangar;
        std::lock_guard<std::mutex> l(m_);
        AcmPlan p;
        if (!ui_on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) { p.why = poison_why_ ? poison_why_ : "ui3d-off"; return p; }
        const bool first_draw = !ui_seen_draw_;
        ui_seen_draw_ = true;
        const bool open_ui = open_ >= 0 && run_[open_].ui;
        const bool open_h = open_ui && run_[open_].hangar;
        const bool open_pause = open_ui && run_[open_].pause;
        const bool pause = open_pause || (!open_ui && uo_.pause && pause_live_.load(std::memory_order_relaxed));
        auto fail = [&](const char* why) {
            ui_hold_locked(f.now_ms);
            // W14-1 F2: the rest of this game frame is skipped whatever failed (an opening failure, a 3D refusal, a rejection): its later ps2_v
            // draws (possibly after the hold ran out inside a stalled frame) never open a stage-2 frame from a partial overlay; until the Present
            ui_skip_frame_ = true;
            if (open_ >= 0 && run_[open_].ui) ui_reject_locked(why); else ui_last_why_ = why;
            ++ui_refused_draws_; hz_why_ = why;
            p.why = why; return p;
        };
        // the stage-2 / 3a non-ps2_v rules (unchanged): the pause window, a stage-2 menu frame, the briefing table, no GPU yet
        if (open_pause) { ui_pause_closed_ = true; p.why = "ui3d-pause-window-closed"; return p; }
        if (pause) { ++ui_pause_prefix_; p.why = "ui3d-pause-prefix"; return p; }
        if ((open_ui && !open_h) || (open_ >= 0 && !open_ui) || !gpu_) return fail("ui3d-frame-has-3d");
        // the record decides whether this is a hangar draw at all (+400 = 682,-448,2048,2048); anything else is today's 3D refusal
        if (f.t10_why) {
            if (open_h) return fail(f.t10_why);
            const char* keep = hz_why_; ++hz_foreign_; fail("ui3d-frame-has-3d"); hz_why_ = keep; return p;   // unreadable record outside a hangar frame: foreign (as below)
        }
        if (const char* why = hz::record_check(f.rec)) {
            if (std::strcmp(why, "u3a-not-hangar") == 0 && !open_h) {
                // An ACM draw with another screen constant (flight 512, offscreen 128, the mission load: live-1144 after t=154165) is by design not ours:
                // today's hold + refusal. It is counted (u3a-foreign) and must NOT overwrite u3a-why, which would otherwise read "ui3d-frame-has-3d"
                // for the rest of the session and look like a hangar failure.
                const char* keep = hz_why_;
                ++hz_foreign_;
                fail("ui3d-frame-has-3d");
                hz_why_ = keep;
                return p;
            }
            return fail(why);
        }
        if (open_h) {
            if (run_[open_].reset) return fail("ui3d-draw-after-reset");
            if (run_[open_].list != f.list) return fail("ui3d-frame-other-list");
            if (hz_overlay_) return fail("u3a-acm-after-overlay");
            if (!(f.rt != nullptr && f.rt == ui_rt_)) return fail("u3a-acm-other-target");
            if (!hz::same_a(f.rec, hz_ref_)) return fail("u3a-a-differs");
        }
        if (const char* why = hz::cb10_check(f.cb_why, f.cb10_0)) return fail(why);
        if (!f.indexed) return fail("u3a-acm-not-indexed");
        if (f.gs) return fail("u3a-acm-gs");
        if (f.topology_type != 3) return fail("u3a-acm-topology");
        if (f.instances != 1) return fail("u3a-acm-instances");
        if (!f.pso_known) return fail("u3a-pso-unknown");
        if (f.rt_count != 1 || f.rtv0 != kRtvFormat || f.samples != 1) return fail("u3a-pso-rtv");
        if (f.dsv != kDsvFormat) return fail("u3a-pso-dsv");
        if (!f.depth_enable || (f.depth_func != acz::brief3d::kCmpGreater && f.depth_func != acz::brief3d::kCmpGreaterEqual)) return fail("u3a-depth-func");
        if (f.stencil && !f.stencil_always) return fail("u3a-stencil-not-always");
        if (!f.rs_known || f.viewports != 1 || f.scissors != 1) return fail("u3a-viewport-unknown");
        if (!f.om_known) return fail("u3a-om-unknown");
        if (!f.rt || !f.rt_w || !f.rt_h || f.rt_fmt != kRtvFormat) return fail("u3a-target-unknown");
        if (f.t10_param >= 64 || !f.t10_va) return fail("u3a-t10-parameter");
        if (!upload_va_ || upload_va_ % 256) return fail("u3a-upload-va");
        if (!open_h) {   // open a hangar frame at this draw
            // Only the FIRST draw of a game frame whose whole interval lies after any hold may open (a frame opened mid-way would miss room
            // draws). L11 fix: these timing refusals NEVER renew the hold (step131/L11: the frame straddling the hold's end refused with
            // u3a-not-first-draw and renewed the hold, once per second for the whole hangar visit) and skip the rest of the game frame.
            const bool held = f.now_ms < ui_hold_until_.load(std::memory_order_relaxed);
            const bool late = !first_draw || !ui_present_ms_ || ui_hold_until_.load(std::memory_order_relaxed) > ui_present_ms_;
            if (held || late) {
                if (!ui_skip_frame_) ++hz_skipped_frames_;
                ui_skip_frame_ = true;
                p.why = held ? "ui3d-hold" : "u3a-not-first-draw"; return p;
            }
            for (const Run& r : run_) if (r.state == run_writing && r.list == f.list) return fail("ui3d-list-holds-previous-frame");
            if (!pose.ok || !pose.space_generation) { ++ui_no_pose_; return fail("ui3d-no-pose"); }
            hz::Camera cam;
            if (const char* why = hz::decompose_record(f.rec, cam)) return fail(why);
            const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();   // the 2D overlay's layers (stage 2)
            if (!sp.valid || sp.generation != pose.space_generation) return fail("ui3d-no-screen-placement");
            u3::Screen scr = u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor);
            if (!scr.valid) return fail("ui3d-screen-invalid");
            scr = u3::scaled(scr, double(uo_.scale));   // u3s: the overlay is a floating panel over the room, not the whole framing
            if (!scr.valid) return fail("ui3d-screen-invalid");
            // the room: re-latched after a gap or a LOCAL generation change (head anchor + yaw too); re-targeted at a camera cut (u3a:1) or at
            // every frame's camera (u3a:2, the room follows the game camera's yaw)
            const bool gap = !hz_room_.valid || hz_gen_ != pose.space_generation || f.now_ms - hz_last_ms_ > hz::kGapMs;
            // 2026-10-30 (live-1144: u3a-cuts +37 in 5 s = a continuously moving camera counted as a cut EVERY frame, the room re-latched at 60/s):
            // an isolated cut re-targets at once (as before); consecutive cuts = a camera in fast motion: the room stays latched (static, comfortable)
            // and is re-targeted ONCE at the first frame whose camera is quiet again (u3a-moving counts the held frames).
            const bool cutraw = !gap && hz_have_prev_ && hz::cut(cam, hz_prev_cam_);
            const bool moving = cutraw && hz_prev_cut_;
            const bool settled = !cutraw && !gap && hz_pending_;
            const bool new_pending = moving || (hz_pending_ && cutraw);
            const bool cutnow = (cutraw && !hz_prev_cut_) || settled;
            double head_p[3], head_q[4];
            for (int i = 0; i < 3; ++i) head_p[i] = gap ? 0.5 * (pose.eyes[0].position[i] + pose.eyes[1].position[i]) : hz_head_p_[i];
            for (int i = 0; i < 4; ++i) head_q[i] = gap ? pose.eyes[0].orientation[i] : hz_head_q_[i];
            hz::Room room = hz_room_;
            if (gap || cutnow || uo_.hangar == 2)
                if (const char* why = hz::make_room(cam, head_p, head_q, double(uo_.hangar_upm), room)) return fail(why);
            float a[2][16]; float k[4]; std::memcpy(k, f.rec + 400, 16);
            for (unsigned e = 0; e < 2; ++e) {
                hz::Eye ev;
                for (int i = 0; i < 3; ++i) ev.p[i] = pose.eyes[e].position[i];
                for (int i = 0; i < 4; ++i) { ev.q[i] = pose.eyes[e].orientation[i]; ev.fov[i] = pose.eyes[e].fov[i]; }
                if (const char* why = hz::eye_matrix(room, cam, k, ev, a[e])) return fail(why);
            }
            const int s = ring_.begin_write(fence);
            if (s < 0) { ++st_.no_slot; return fail(fence.lost ? "ui3d-fence-lost" : "ui3d-no-free-slot"); }
            // committed: the room state changes only for a frame that opened
            if (gap) { ++hz_latches_; std::memcpy(hz_head_p_, head_p, sizeof head_p); std::memcpy(hz_head_q_, head_q, sizeof head_q); }
            else if (cutnow) ++hz_cuts_;
            hz_room_ = room; hz_gen_ = pose.space_generation;
            // 2026-10-30d (u3agm): the glow memory of the last hangar frame (its misses counted), cleared at a latch / gap (both modes) and at a
            // camera cut (u3agm:1: another screen may show another part of the hangar); otherwise its forgotten entries leave
            if (hz_mem_frame_) hz_glow_missed_ += glow_mem_.missed(hz_mem_frame_);
            if (gap || (cutnow && uo_.glow_memory == 1) || uo_.glow_memory == 0) glow_mem_.clear(); else glow_mem_.compact();
            Run& r = run_[s]; r = Run{}; r.state = run_writing; r.list = f.list; r.frame = ++frame_serial_; r.open = true; r.ui = true; r.hangar = true;
            hz_mem_frame_ = r.frame;
            meta_[s] = SlotMeta{}; meta_[s].frame = r.frame; meta_[s].eyes[0] = pose.eyes[0]; meta_[s].eyes[1] = pose.eyes[1];
            meta_[s].space_generation = pose.space_generation; meta_[s].opaque = true;
            ui_assigner_ = u3::LayerAssigner{}; ui_rt_ = f.rt; ui_screen_ = scr; ui_lopts_ = uo_; ui_lopts_.near_m = uo_.menu_near_m; ui_lopts_.far_m = uo_.menu_far_m; ui_culled_ = 0;
            ui_eye_[0] = ui_eye(pose.eyes[0]); ui_eye_[1] = ui_eye(pose.eyes[1]);
            ui_rows_roll_locked();
            std::memcpy(hz_ref_, f.rec, 64); std::memcpy(hz_a_, a, sizeof a);
            hz_prev_cam_ = cam; hz_have_prev_ = true; hz_prev_cut_ = gap ? false : cutraw; hz_pending_ = gap ? false : new_pending; if (moving) ++hz_moving_; hz_last_ms_ = f.now_ms;
            hz_acm_ = 0; hz_overlay_ = false; ++hz_frames_;
            open_ = s;
        }
        Run& r = run_[open_];
        if (hz_acm_ >= hz::kMaxAcmPerFrame) return fail("u3a-too-many-acm");
        const uint64_t base = (r.ui_bytes + 255u) & ~uint64_t(255u), need = 2u * hz::kRecStride;
        if (base + need > u3::kUploadPerSlot) return fail("u3a-upload-full");
        for (unsigned e = 0; e < 2; ++e) {
            p.upload_offset[e] = kUiUploadBase + uint64_t(open_) * u3::kUploadPerSlot + base + e * hz::kRecStride;
            hz::substitute(f.rec, hz_a_[e], p.rec[e]);
            p.swap.eye_va[e] = upload_va_ + p.upload_offset[e];
        }
        p.swap.parameter = f.t10_param; p.swap.restore_va = f.t10_va; p.swap.copies = 1; p.swap.step = 0;
        r.ui_bytes = base + need;
        hz_last_ms_ = f.now_ms;
        p.record = true; p.slot = open_; p.frame = r.frame; p.clear = !r.clear_granted; p.clear_depth = 0.0f;
        r.clear_granted = true;
        ++r.in_record; ++hz_acm_;
        return p;
    }
    void ui3d_acm_commit(const AcmPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++ui_replays_; ++hz_acm_replays_; ++meta_[p.slot].replays;
        settle_locked(p.slot);
    }
    void ui3d_acm_cancel(const AcmPlan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p.record, p.slot, p.frame); if (!r) return;
        --r->in_record; ++st_.cancelled;
        if (p.clear && !r->invalid) { r->invalid = true; ++st_.invalidated; }
        settle_locked(p.slot);
    }
    // The draw seam met a draw whose list/PSO facts are unknown (P1-2): fail closed. An open UI3D frame is rejected and new frames are held off.
    void ui3d_unknown_draw(int64_t now_ms, const char* why) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!ui_on_.load(std::memory_order_relaxed) || poisoned_.load(std::memory_order_relaxed)) return;
        ui_hold_locked(now_ms);
        ++ui_refused_draws_;
        if (open_ >= 0 && run_[open_].ui) ui_reject_locked(why); else ui_last_why_ = why;
    }
    acz::ui3d::Stats ui3d_stats() const noexcept { std::lock_guard<std::mutex> l(m_); return ui_st_; }
    // summary-ui3d (ui3d.h summary_line) + the producer facts after it (each bounded; < 256 + 200 B).
    std::string ui3d_summary() const {
        std::lock_guard<std::mutex> l(m_);
        char t[224], pz[160] = "";
        auto c = [](uint64_t v) { return clamp_digits(v, 9); };
        std::snprintf(t, sizeof t, ";replays=%llu;published=%llu;rejected=%llu;refused-draws=%llu;no-pose=%llu;held=%u;last-refusal=%.32s;sel=%llu",
                      c(ui_replays_), c(ui_published_), c(ui_rejected_), c(ui_refused_draws_), c(ui_no_pose_),
                      now_ms_ < ui_hold_until_.load(std::memory_order_relaxed) ? 1u : 0u, ui_last_why_ ? ui_last_why_ : "none", c(ui_sel_));
        if (uo_.pause)   // stage 3a (u3p:1 only: the stage-2 line is unchanged otherwise)
            std::snprintf(pz, sizeof pz, ";pause-live=%u;pause-frames=%llu;pause-published=%llu;pause-prefix=%llu;pause-placements=%llu",
                          pause_live_.load(std::memory_order_relaxed) ? 1u : 0u, c(ui_pause_frames_), c(ui_pause_published_), c(ui_pause_prefix_),
                          c(ui_pause_placements_));
        char rm[64] = "";
        if (uo_.frame_mm > 0.0f)   // 2026-10-30d (u3lt only: the default line is unchanged): frame quads drawn as rims, rims that fell back to flat
            std::snprintf(rm, sizeof rm, ";rim=%llu;rim-flat=%llu", c(ui_rim_), c(ui_rim_fallback_));
        char grp[176] = "";
        if (uo_.reinterpret())   // 2026-10-31 (stages 2-3 only: the default line is unchanged): group / base placements, groups in the reference
            std::snprintf(grp, sizeof grp, ";grp=%llu;grp-base=%llu;grp-n=%u;grp-over=%llu;bevel=%llu;pane-back=%llu;sel-text=%llu;shade-skip=%llu",
                          c(ui_grp_), c(ui_grp_base_), ui_grp_n_, c(ui_grp_dropped_), c(ui_bevel_), c(ui_pane_back_), c(ui_sel_text_), c(ui_shade_skip_));
        char hz[560] = "";
        if (uo_.hangar) {  // stage 3b (u3a:1|2 only); 2026-10-30c: + the dropped tint draws, the re-projected glows and the last glow drop reason
            const int k = std::snprintf(hz, sizeof hz, ";u3a-frames=%llu;u3a-published=%llu;u3a-rejected=%llu;u3a-acm=%llu;u3a-fx=%llu;u3a-latches=%llu;u3a-cuts=%llu;u3a-skips=%llu;u3a-late=%llu;u3a-dropped=%llu;u3a-moving=%llu;u3a-foreign=%llu;u3a-why=%.28s;u3a-tint=%llu;u3a-glow=%llu;u3a-glow-why=%.24s",
                          c(hz_frames_), c(hz_published_), c(hz_rejected_), c(hz_acm_replays_), c(hz_fx_), c(hz_latches_), c(hz_cuts_), c(hz_skipped_frames_),
                          c(hz_late_), c(hz_dropped_), c(hz_moving_), c(hz_foreign_), hz_why_ ? hz_why_ : "none", c(hz_tint_), c(hz_glow_),
                          hz_glow_why_ ? hz_glow_why_ : "none");
            // 2026-10-30d (u3ag:1 with u3agm:1|2): remembered glows drawn (the game did not emit them), lights remembered now, remembered glows a frame missed
            if (uo_.hangar_glow && uo_.glow_memory != 0 && k > 0 && size_t(k) < sizeof hz)
                std::snprintf(hz + k, sizeof hz - size_t(k), ";u3a-glow-mem=%llu;u3a-glow-lights=%u;u3a-glow-missed=%llu", c(hz_glow_mem_), glow_mem_.size(),
                              c(hz_glow_missed_));
        }
        return acz::ui3d::summary_line(ui_st_) + t + rm + grp + pz + hz;
    }

    // The glue recorded the plan's commands into the frame's list.
    void commit(const Plan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p); if (!r) return;
        --r->in_record; ++st_.replays; ++meta_[p.slot].replays;
        settle_locked(p.slot);
    }
    // The glue recorded nothing for the plan (no command reached the list). A cancelled clear invalidates the frame (later replays would not be
    // preceded by a clear).
    void cancel(const Plan& p) noexcept {
        std::lock_guard<std::mutex> l(m_);
        Run* r = reserved_run(p); if (!r) return;
        --r->in_record; ++st_.cancelled;
        if (p.clear && !r->invalid) { r->invalid = true; ++st_.invalidated; }
        settle_locked(p.slot);
    }

    // ---- list lifetime (game render thread)
    // After the native Reset. ok = the Reset succeeded: the old recording can never be executed again (the retirement witness).
    void on_reset(const void* list, bool ok) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!ok) return;
        for (unsigned s = 0; s < kSlots; ++s) {
            Run& r = run_[s];
            if (r.state != run_writing || r.list != list || r.reset) continue;
            r.reset = true;
            // A UI3D frame reset BEFORE its Present stays the open frame (P1-1): it is validated only at the Present (on_present) and any later
            // refusal of the same game frame (non-ps2 draw, another list, unknown facts) still rejects it; settle_locked() waits for r.open.
            if (r.ui && r.open && !r.rejected) continue;
            r.open = false;
            if (open_ == int(s)) open_ = -1;
            if (!r.executions && !r.submitting) ++st_.discarded;
            settle_locked(int(s));
        }
    }
    // Before the native ExecuteCommandLists: a SUBMISSION RESERVATION for every pending (not yet reset) frame list in the batch, after the queue
    // check (also for re-executions). true = the glue must Signal(fence, value) on `queue` right after the native call (holding its submit mutex
    // from here to after_execute so values stay ordered) and then call after_execute.
    struct ExecToken { bool signal = false; uint64_t value = 0; unsigned slots = 0; };
    ExecToken before_execute(const void* queue, const void* const* lists, unsigned n) noexcept {
        std::lock_guard<std::mutex> l(m_);
        ExecToken t;
        if (!lists) return t;
        for (unsigned k = 0; k < n; ++k)
            for (unsigned s = 0; s < kSlots; ++s) {
                Run& r = run_[s];
                if (r.state != run_writing || r.list != lists[k] || r.reset) continue;
                if (queue_ && queue_ != queue) {   // fence values are ordered on ONE queue only: never signal for it, never publish it
                    if (!r.quarantined) { r.quarantined = true; ++st_.quarantined; }
                    poison_locked("brief3d-second-queue");
                    continue;
                }
                queue_ = queue;
                if (r.executions || r.submitting) ++st_.reexecutes;
                ++r.submitting; t.slots |= 1u << s;
            }
        if (t.slots) { t.signal = true; t.value = ++value_; ++st_.executes; }
        return t;
    }
    // After the native Execute and the glue's Signal attempt: the submission reservations end with the Signal outcome.
    void after_execute(const ExecToken& t, bool signalled) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!t.signal) return;
        if (signalled) ++st_.signals; else poison_locked("brief3d-signal-failed");
        for (unsigned s = 0; s < kSlots; ++s) {
            if (!(t.slots & (1u << s))) continue;
            Run& r = run_[s];
            if (r.submitting) --r.submitting;
            if (signalled) { ++r.executions; if (t.value > r.value) r.value = t.value; }
            else if (!r.quarantined) { r.quarantined = true; ++st_.quarantined; }   // executed, completion never provable
            settle_locked(int(s));
        }
    }
    // ---- frame close (Present thread): no more map draws join the open frame; publication still waits for its list's Reset.
    void on_present(int64_t now_ms) noexcept {
        std::shared_ptr<const latch::Scene> old_scene; std::shared_ptr<latch::Scene> old_pend;   // declared before the lock: released after it
        std::lock_guard<std::mutex> l(m_);
        now_ms_ = now_ms;
        latch_present_locked(now_ms, old_scene, old_pend);   // brief3d.md 11: the game frame's latched draws become the shown scene
        ui_present_ms_ = now_ms; ui_seen_draw_ = false;   // stage 3b: the next draw is the first of a new game frame
        // the skip latch ends with this Present, also when closing the frame below rejects it (W14-1 F2 sets the latch on rejection)
        struct SkipEnds { bool& s; ~SkipEnds() { s = false; } } skip_ends{ui_skip_frame_};
        if (uo_.pause) {   // stage 3a: the consumer's HUD-MENU heartbeat (ui3d_pause.h): live for the NEXT game frame's draws
            const auto& sig = acz::ui3d::pause::signal();
            const uint32_t beat = sig.beat.load(std::memory_order_acquire);
            pause_live_.store(pause_gate_.observe(sig.on.load(std::memory_order_relaxed), beat, now_ms), std::memory_order_relaxed);
        }
        // roll the observed icon maximum per completed game frame interval, independent of whether this frame opened (W8-1 P2)
        done_top_ = seen_top_; done_any_ = seen_any_; seen_top_ = 0; seen_any_ = false;
        if (open_ < 0) return;
        if (run_[open_].ui && run_[open_].hangar) {   // stage 3b: the room is needed (the overlay may be empty); overlay refusals rejected already
            if (!hz_acm_) { ui_hold_locked(now_ms); ui_reject_locked("u3a-frame-empty"); return; }
        } else if (run_[open_].ui && run_[open_].dbr) {   // DEBRIEF-3D: a frame with no draw is never shown
            if (!dbr_frame_draws_) { ui_reject_locked("dbr3d-frame-empty"); return; }
            ++ui_st_.frames; ++ui_st_.usable; ++dbr_st_.usable;
        } else if (run_[open_].ui) {   // UI3D: the frame's statistics; a frame without any replayed draw is never shown
            if (!ui_assigner_.usable()) { ui_reject_locked("ui3d-frame-empty"); return; }
            acz::ui3d::count_frame(ui_st_, ui_assigner_, ui_culled_);
        }
        const int s = open_;
        run_[s].open = false; run_[s].closed_ms = now_ms;
        open_ = -1;
        if (run_[s].ui) settle_locked(s);   // reset before the Present: only now is the interval validated
    }

    // ---- consumer (XR owner thread). Poison is checked under the same lock as the slot admission.
    bool acquire(uint64_t after, const Completion& c, Held& out) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (poisoned_.load(std::memory_order_relaxed)) return false;
        acz::vscreen::Acquired a;
        if (!ring_.acquire_latest(after, c, a)) return false;
        out.slot = a.slot; out.a = a; out.meta = meta_[a.slot];
        ++st_.acquired;
        st_.lag_frames = frame_serial_ > out.meta.frame ? frame_serial_ - out.meta.frame : 0;
        st_.age_ms = now_ms_ > out.meta.published_ms ? uint64_t(now_ms_ - out.meta.published_ms) : 0;
        if (st_.lag_frames > st_.lag_max) st_.lag_max = st_.lag_frames;
        if (st_.age_ms > st_.age_max_ms) st_.age_max_ms = st_.age_ms;
        return true;
    }
    void release(const Held& h) noexcept { std::lock_guard<std::mutex> l(m_); ring_.release(h.a); }

    // ---- diagnostics
    bool poisoned() const noexcept { return poisoned_.load(std::memory_order_acquire); }
    const char* poison_why() const noexcept { std::lock_guard<std::mutex> l(m_); return poison_why_; }
    Stats stats() const noexcept { std::lock_guard<std::mutex> l(m_); return st_; }
    uint64_t signalled_value() const noexcept { std::lock_guard<std::mutex> l(m_); return value_; }
    // Frame lists still able to execute replays (not reset): the Execute hook's cheap gate.
    unsigned tracked_lists() const noexcept {
        std::lock_guard<std::mutex> l(m_);
        unsigned n = 0; for (const Run& r : run_) if (r.state == run_writing && !r.reset) ++n;
        return n;
    }
    acz::vscreen::RingSlot ring_slot(unsigned i) noexcept { return ring_.slot(i); }
    bool slot_quarantined(unsigned s) const noexcept { std::lock_guard<std::mutex> l(m_); return s < kSlots && run_[s].quarantined; }
    bool take_fit_line(char* buf, size_t n) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!fit_line_) return false;
        fit_line_ = false;
        format_fit(buf, n, fit_box_, fit_, nullptr);
        return true;
    }
    // The zoom-aware content fit changed since the last call (brief3d.md 9.1): one brief3d-content line.
    bool take_content_line(char* buf, size_t n) noexcept {
        std::lock_guard<std::mutex> l(m_);
        if (!content_line_) return false;
        content_line_ = false;
        format_content(buf, n, latched_top_, content_k_, fit_, content_max_height(o_), double(o_.width_m), content_changes_);
        return true;
    }
    double content_scale_now() const noexcept { std::lock_guard<std::mutex> l(m_); return content_k_; }
    int format_summary(char* buf, size_t n) const noexcept {
        std::lock_guard<std::mutex> l(m_);
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v > 999999999ull ? 999999999ull : v); };
        return std::snprintf(buf, n,
            "summary-brief3d;frames=%llu;published=%llu;aborted=%llu;replays=%llu;selmap=%llu;icons=%llu;icons-unverified=%llu;icons-other-ps=%llu;refused=%llu;"
            "other-list=%llu;executes=%llu;reexecutes=%llu;signals=%llu;discarded=%llu;cancelled=%llu;invalidated=%llu;quarantined=%llu;fits=%llu;"
            "fit-refused=%llu;placements=%llu;no-pose=%llu;no-slot=%llu;acquired=%llu;screens=%u;air=%llu;dots=%llu;poisoned=%d;why=%.32s;last-why=%.32s;fit-why=%.32s",
            u(st_.frames), u(st_.published), u(st_.aborted), u(st_.replays), u(st_.selmap), u(st_.icons), u(st_.icons_unverified), u(st_.icons_other_ps), u(st_.refused),
            u(st_.other_list), u(st_.executes), u(st_.reexecutes), u(st_.signals), u(st_.discarded), u(st_.cancelled), u(st_.invalidated),
            u(st_.quarantined), u(st_.fits), u(st_.fit_refused), u(st_.placements), u(st_.no_pose), u(st_.no_slot), u(st_.acquired), screens_,
            static_cast<unsigned long long>(clamp_digits(st_.air_icons, 7)), static_cast<unsigned long long>(clamp_digits(st_.stalk_dots, 7)),
            poisoned_.load() ? 1 : 0, poison_why_ ? poison_why_ : "none", last_why_ ? last_why_ : "none", fit_why_ ? fit_why_ : "none");
    }
    // xr-stereo.log: the table-line tail and the separate producer line (format_xr_tail / format_xr_producer).
    void format_xr(char* tail, size_t tn, char* line, size_t ln) const noexcept {
        std::lock_guard<std::mutex> l(m_);
        format_xr_tail(tail, tn, st_, screens_, fit_why_);
        format_xr_producer(line, ln, st_, poisoned_.load(), poison_why_);
    }

private:
    enum RunState : uint8_t { run_idle, run_writing, run_done };
    struct Run {
        RunState state = run_idle;
        const void* list = nullptr;
        uint64_t frame = 0;
        unsigned in_record = 0, submitting = 0, executions = 0, sel_n = 0, icon_n = 0;
        uint64_t value = 0; int64_t closed_ms = 0;
        bool open = false, reset = false, clear_granted = false, invalid = false, quarantined = false;
        bool ui = false, rejected = false;   // UI3D frame; rejected = refused after it opened: retired without publication
        bool pause = false;                  // UI3D stage 3a: a pause-menu frame (in flight)
        bool hangar = false;                 // UI3D stage 3b: a hangar room frame (ACM replayed per eye + the 2D overlay)
        uint64_t ui_bytes = 0;               // UI3D upload bytes used in the slot's span
        bool dbr = false;                    // DEBRIEF-3D stage 2: a debrief frame (map scene on the wraparound + the panel layers)
        uint64_t dbr_bytes = 0;              // DEBRIEF-3D upload bytes used in the slot's dbr3d span
    };
    // DEBRIEF-3D: one non-indexed 4-vertex ps2_v draw; the debrief window signature refreshes the arming clock (and counts a new rectangle).
    void dbr_observe_locked(int64_t now_ms, uint32_t vs8, uint32_t ps8, bool indexed, const acz::ui3d::Cb0& cb, bool blend, uint32_t src, uint32_t dst,
                            const uint8_t* vtx, uint32_t n) noexcept {
        acz::dbr3d::GsRect w;
        if (!acz::dbr3d::window_from_vertices(vs8, ps8, indexed, cb, blend, src, dst, vtx, n, w)) return;
        const int64_t prev = dbr_seen_ms_.load(std::memory_order_relaxed);
        if (!(prev != 0 && now_ms >= prev && now_ms - prev <= acz::dbr3d::kArmMs)) ++dbr_st_.arms;
        acz::dbr3d::latch_window(dbr_latch_, w, cb);
        dbr_seen_ms_.store(now_ms ? now_ms : 1, std::memory_order_relaxed);
    }
    Run* reserved_run(const Plan& p) noexcept { return reserved_run(p.record, p.slot, p.frame); }
    Run* reserved_run(bool record, int slot, uint64_t frame) noexcept {
        if (!record || slot < 0 || unsigned(slot) >= kSlots) return nullptr;
        Run& r = run_[slot];
        if (r.state != run_writing || r.frame != frame || !r.in_record) return nullptr;
        return &r;
    }
    void ui_hold_locked(int64_t now_ms) noexcept {
        const int64_t until = now_ms + kUiHoldMs;
        if (until > ui_hold_until_.load(std::memory_order_relaxed)) ui_hold_until_.store(until, std::memory_order_relaxed);
    }
    // The open UI3D frame is refused: closed to further draws, kept WRITING until its list's Reset, then retired unpublished (settle_locked).
    void ui_reject_locked(const char* why) noexcept {
        if (open_ < 0 || !run_[open_].ui) return;
        Run& r = run_[open_];
        r.rejected = true; r.open = false; r.closed_ms = now_ms_;
        ++ui_st_.frames; ++ui_st_.refused; ui_st_.last_why = why; ui_last_why_ = why;
        if (r.hangar) { ++hz_rejected_; hz_why_ = why; ui_skip_frame_ = true; }   // W14-1 F2: the rest of the game frame is skipped
        if (r.dbr) { ++dbr_st_.rejected; dbr_st_.last_why = why; }
        const int s = open_;
        open_ = -1;
        settle_locked(s);
    }
    // brief3d.md 11: the game frame ended. A pending scene with at least one grid draw and no refused map draw in the frame is published
    // (stamped with this Present's clock); otherwise it is dropped and the previous scene stays. The replaced objects leave through the outs.
    void latch_present_locked(int64_t now_ms, std::shared_ptr<const latch::Scene>& old_scene, std::shared_ptr<latch::Scene>& old_pend) noexcept {
        const bool bad = lt_bad_, geom_bad = lt_geom_bad_; const char* why = lt_why_;
        lt_bad_ = false; lt_geom_bad_ = false; lt_why_ = nullptr;
        // headset test 7: the grid matrix belongs to THIS game frame only. Kept across the Present, a frame whose grid draw was refused checked its
        // icons against the previous frame's (orbiting) camera and refused every one (brief3d-icon-camera-unmatched) - the likely source [I] of
        // the 1,051 non-geometry refusals in test 7 (all inside the 225 frames dropped for b3l-read-rate). The dropped frame is unchanged.
        lt_have_grid_ = false;
        // the automatic fallback: geometry never readable (no scene ever published) for kLatchGiveUpFrames consecutive map frames
        auto give_up = [&](const char* w) {
            if (!geom_bad || lst_.scenes) { lt_fail_run_ = 0; return; }
            if (++lt_fail_run_ >= kLatchGiveUpFrames && !latch_fallback_.load(std::memory_order_relaxed)) {
                latch_fallback_.store(true, std::memory_order_release); fallback_why_ = w;
            }
        };
        if (!pend_) { if (bad) { ++lst_.incomplete; latch_why_ = why; give_up(why); } return; }
        const char* lw = bad ? why : !pend_->selmap ? "b3l-no-grid-draw" : nullptr;
        if (!lw) {
            try { lw = latch::layout(*pend_, scene_.get()); } catch (...) { lw = "b3l-alloc"; }
        }
        if (lw) { ++lst_.incomplete; latch_why_ = lw; give_up(lw); old_pend = std::move(pend_); pend_.reset(); return; }
        lt_fail_run_ = 0;
        pend_->published_ms = now_ms; pend_->serial = ++scene_serial_;
        old_scene = std::move(scene_);
        scene_ = std::move(pend_); pend_.reset();
        ++lst_.scenes; ++st_.frames; ++st_.published; latch_why_ = nullptr;
    }
    bool screen_left_locked(int64_t now_ms) const noexcept { return last_draw_ms_ && now_ms - last_draw_ms_ > kScreenGapMs; }
    bool relatch_locked(int64_t now_ms) const noexcept { return last_draw_ms_ && now_ms - last_draw_ms_ > kRelatchGapMs; }
    void poison_locked(const char* why) noexcept {
        if (!poisoned_.load(std::memory_order_relaxed)) poison_why_ = why;
        poisoned_.store(true, std::memory_order_release);
    }
    // A retired frame (its list Reset, no recording or submission reservation outstanding) is published (executed and signalled, valid) or
    // aborted (never executed). Quarantined (second queue / failed Signal) or invalid-but-executed slots stay WRITING for ever: GPU work may
    // still target them and nothing proves its completion order, so they are never read or reused.
    void settle_locked(int s) noexcept {
        if (s < 0 || unsigned(s) >= kSlots) return;
        Run& r = run_[s];
        if (r.state != run_writing || !r.reset || r.in_record || r.submitting) return;
        if (r.ui && r.open) return;   // UI3D: the Present interval is not validated yet (on_present / ui_reject_locked close it)
        if (r.quarantined) { r.state = run_done; return; }
        if (r.invalid && r.executions) { r.quarantined = true; ++st_.quarantined; r.state = run_done; return; }
        r.state = run_done;
        if (r.ui && (r.rejected || r.invalid)) {   // UI3D refused frame: never shown; an executed one keeps its value (no reuse before completion)
            if (r.executions && r.value) ring_.retire_unpublished(s, r.value); else ring_.abort(s);
            ++ui_rejected_;
            return;
        }
        if (r.executions && r.value) {
            meta_[s].published_ms = now_ms_; ring_.publish(s, r.value); ++st_.published;
            if (r.ui) ++ui_published_;
            if (r.pause) ++ui_pause_published_;
            if (r.hangar) ++hz_published_;
            if (r.dbr) ++dbr_st_.published;
        }
        else { ring_.abort(s); ++st_.aborted; }
    }

    mutable std::mutex m_;
    // brief3d.md 11 (b3l:1): the latched scene
    std::atomic<bool> latch_on_{false}, latch_fallback_{false};
    static constexpr unsigned kLatchGiveUpFrames = 120;   // ~2 s of map frames at 60 per second
    unsigned lt_fail_run_ = 0; bool lt_geom_bad_ = false; const char* fallback_why_ = nullptr;
    std::shared_ptr<latch::Scene> pend_;              // the game frame being latched (published at its Present)
    std::shared_ptr<const latch::Scene> scene_;       // the newest published scene (the consumer copies the pointer)
    uint64_t scene_serial_ = 0;
    bool lt_bad_ = false, lt_have_grid_ = false;      // a map draw of this game frame was refused; the frame's grid matrix (icon camera check)
    const char* lt_why_ = nullptr; const char* latch_why_ = nullptr;
    uint8_t lt_grid_m_[64] = {};
    unsigned lt_icon_n_ = 0;
    latch::Stats lst_;
    latch::ReadStats cache_stats_;
    std::atomic<void*> rt_[kSlots][2] = {}; std::atomic<void*> fence_{nullptr};
    Options o_;
    std::atomic<bool> on_{false}, poisoned_{false};
    const char* poison_why_ = nullptr; const char* last_why_ = nullptr; const char* fit_why_ = nullptr;
    bool gpu_ = false; uint64_t upload_va_ = 0;
    acz::vscreen::Ring ring_;
    Run run_[kSlots]; SlotMeta meta_[kSlots];
    int open_ = -1;
    bool have_grid_ = false, fit_line_ = false;
    unsigned screens_ = 0;
    uint8_t grid_m_[64] = {};
    Mat4 eye_m_[2]; DepthMode dm_ = DepthMode::reverse;
    Fit fit_; Box fit_box_; Placement placement_;
    latch::TablePlane plane_;                         // b3p:1: the world-fixed table quad of placement_
    // zoom-aware content fit (brief3d.md 9.1): icons seen since the last opened frame, the latched content top, the uniform table scale
    double seen_top_ = 0, done_top_ = 0; bool done_any_ = false;
    ContentLatch latch_; int64_t k_ms_ = 0;
    double latched_top_ = 0, content_k_ = 1.0; bool seen_any_ = false, content_line_ = false; unsigned long long content_changes_ = 0;
    int64_t fit_tried_ms_ = 0, last_draw_ms_ = 0, now_ms_ = 0;
    uint64_t frame_serial_ = 0, value_ = 0;
    const void* queue_ = nullptr;
    Stats st_;
    // UI3D
    acz::ui3d::Options uo_;
    std::atomic<bool> ui_on_{false};
    std::atomic<int64_t> ui_hold_until_{0};
    acz::ui3d::LayerAssigner ui_assigner_;
    acz::ui3d::Screen ui_screen_;
    acz::ui3d::Options ui_lopts_;   // the layer options of the open UI3D frame (menu distances, or u3n/u3f for the pause menu)
    acz::ui3d::EyeView ui_eye_[2];
    const void* ui_rt_ = nullptr;
    uint64_t ui_culled_ = 0, ui_replays_ = 0, ui_published_ = 0, ui_rejected_ = 0, ui_refused_draws_ = 0, ui_no_pose_ = 0;
    const char* ui_last_why_ = nullptr;
    acz::ui3d::Stats ui_st_;
    // UI3D stage 3a (pause menu)
    std::atomic<bool> ui_pause_cfg_{false}, pause_live_{false};
    std::atomic<unsigned> ui_layering_{1};      // u3q (configure_ui3d)
    std::atomic<uint32_t> ui_filter_bits_{acz::ui3d::resample::filter_bits(acz::ui3d::resample::kFilterCatmullRomSharp, 0.35f)};   // u3r / u3k
    acz::ui3d::pause::Gate pause_gate_;
    acz::ui3d::Screen ui_pause_screen_;
    uint32_t ui_pause_gen_ = 0;
    int64_t ui_pause_last_ms_ = 0;
    bool ui_pause_closed_ = false;
    uint64_t ui_pause_frames_ = 0, ui_pause_published_ = 0, ui_pause_prefix_ = 0, ui_pause_placements_ = 0;
    // UI3D stage 3b (hangar room)
    std::atomic<bool> ui_hangar_cfg_{false};
    int64_t ui_present_ms_ = 0;          // the last Present (the first-draw rule)
    bool ui_seen_draw_ = false;          // a draw reached the UI3D core since the last Present
    acz::ui3d::hangar::Room hz_room_;
    acz::ui3d::hangar::Camera hz_prev_cam_;
    bool hz_have_prev_ = false, hz_overlay_ = false, hz_prev_cut_ = false, hz_pending_ = false;
    int64_t hz_last_ms_ = 0;
    uint32_t hz_gen_ = 0;
    double hz_head_p_[3] = {0, 0, 0}, hz_head_q_[4] = {0, 0, 0, 1};
    uint8_t hz_ref_[64] = {};
    float hz_a_[2][16] = {};
    unsigned hz_acm_ = 0;
    uint64_t hz_frames_ = 0, hz_published_ = 0, hz_rejected_ = 0, hz_acm_replays_ = 0, hz_fx_ = 0, hz_latches_ = 0, hz_cuts_ = 0;
    bool ui_skip_frame_ = false;          // L11 fix: this game frame's hangar ACM draw could not open: skip the frame's ps2_v draws
    uint64_t hz_skipped_frames_ = 0, hz_skipped_draws_ = 0, hz_late_ = 0, hz_dropped_ = 0, hz_moving_ = 0, hz_foreign_ = 0;
    // 2026-10-30c: the selection slab (text rows of the previous / current UI3D frame), the dropped hangar tint, the re-projected glows
    acz::ui3d::SelectionRows ui_rows_prev_, ui_rows_cur_;
    uint64_t ui_sel_ = 0, hz_tint_ = 0, hz_glow_ = 0;
    const char* hz_glow_why_ = nullptr;
    // 2026-10-30d: frame rims (u3lt) and their upload fallbacks; the hangar glow memory (u3agm): remembered quads drawn, misses, its frame
    uint64_t ui_rim_ = 0, ui_rim_fallback_ = 0, hz_glow_mem_ = 0, hz_glow_missed_ = 0, hz_mem_frame_ = 0;
    acz::ui3d::hangar::GlowMemory glow_mem_;
    // 2026-10-31 (ui-3d.md 14): stage-2 panel groups (the previous / current frame's panel rectangles), the frame's selection box (stage 3)
    acz::ui3d::groups::PanelGroups ui_groups_prev_, ui_groups_cur_;
    acz::ui3d::Box ui_sel_box_;
    bool ui_have_sel_box_ = false;
    unsigned ui_grp_n_ = 0;
    uint64_t ui_grp_ = 0, ui_grp_base_ = 0, ui_grp_dropped_ = 0, ui_bevel_ = 0, ui_pane_back_ = 0, ui_sel_text_ = 0, ui_shade_skip_ = 0;
    const char* hz_why_ = nullptr;
    // DEBRIEF-3D stage 2 (dbr3d:1)
    acz::dbr3d::Options dbr_o_;
    std::atomic<bool> dbr_on_{false};
    std::atomic<int64_t> dbr_seen_ms_{0};   // the last debrief window quad (the arming clock)
    acz::dbr3d::Latch dbr_latch_;
    acz::dbr3d::Router dbr_router_;
    acz::ui3d::Screen dbr_wrap_;            // the open debrief frame's wraparound (the unscaled vscreen placement)
    acz::ui3d::Options dbr_wopts_;
    unsigned dbr_frame_draws_ = 0;
    acz::dbr3d::LiveStats dbr_st_;
    uint8_t dbr_cull_[acz::dbr3d::kMaxVerticesPerDraw] = {};   // (F2) one eye's culled-vertex flags (dbr3d_draw, under m_)
};

// One producer per module (the draw/execute/reset hooks in d3d12_hook.cpp, the present hook, the XR consumer and the proxy share it).
inline Producer& producer() noexcept { static Producer p; return p; }

} // namespace acz::brief3d::live
