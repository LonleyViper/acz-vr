// UI3D stage 3a offline tests (docs/world-render/ui-3d.md 7): the in-flight PAUSE MENU on per-eye depth layers. Pure parts of ui3d_pause.h
// (option, trigger gate, placement, blend maths), the PRODUCER's pause frame kind (brief3d_live.h Producer::ui3d_draw with the trigger live),
// the CONSUMER's PauseCore (xr_brief3d_core.h) with a fake GPU and a fake multi-hold source, and the census TAIL mode (ui3d_census.h).
// Nothing here touches D3D12, the game or a headset: fixture success says nothing about the live pause menu.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d_pause.h"
#include "brief3d_live.h"
#include "ui3d_census.h"
#include "xr_brief3d_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace acz;
using namespace acz::brief3d::live;
namespace u3 = acz::ui3d;
namespace up = acz::ui3d::pause;
using acz::vscreen::Completion;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

namespace {

// ================================================================================================== options
void test_option() {
    const std::string day = "auto;env:1;props:1;trees:1;vscreen:1;vsearly:1;brief3d:1;mk360:3";
    std::string rest, why; u3::Options o;
    CHECK(u3::extract_options(day + ";ui3d:1", rest, o, why) && rest == day && o.on && !o.pause);
    CHECK(u3::describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");   // stage-2 line unchanged without u3p
    CHECK(u3::extract_options(day + ";ui3d:1;u3p:1", rest, o, why) && rest == day && o.on && o.pause && o.any_token);
    CHECK(u3::describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3p=1;u3mn=1.10;u3mf=6.00;u3s=0.70");
    CHECK(u3::extract_options(day + ";u3p:0", rest, o, why) && rest == day && !o.pause);
    CHECK(!u3::extract_options(day + ";u3p:2", rest, o, why) && why == "bad value for u3p (0|1)");
    // the 255-character terrain option buffer still holds the day value + the longest ui3d set + u3p
    CHECK(std::string(day + ";ui3d:1;u3n:1.8;u3f:3.5;u3t:-12.5;u3c:1;u3p:1;u3mn:1.1;u3mf:6.0;u3s:0.7").size() < 255);
}

// ================================================================================================== trigger gate
void test_gate() {
    up::Gate g;
    CHECK(!g.observe(false, 1, 100));
    CHECK(g.observe(true, 2, 110) && g.live());
    CHECK(g.observe(true, 2, 110 + up::kSignalStaleMs));          // same beat, still fresh
    CHECK(!g.observe(true, 2, 111 + up::kSignalStaleMs));         // frozen heartbeat: off by itself
    CHECK(g.observe(true, 3, 400));                               // a new beat: live again
    CHECK(!g.observe(false, 4, 410));                             // the consumer left menu mode
    // the global signal: publish() advances the beat every call
    const uint32_t b0 = up::signal().beat.load();
    up::publish(true); CHECK(up::signal().on.load() && up::signal().beat.load() == b0 + 1);
    up::publish(false); CHECK(!up::signal().on.load() && up::signal().beat.load() == b0 + 2);
}

// ================================================================================================== placement
u3::EyeView eye_at(double x, double y, double z, double yaw) {
    u3::EyeView e; e.p[0] = x; e.p[1] = y; e.p[2] = z;
    e.q[0] = 0; e.q[1] = std::sin(yaw / 2); e.q[2] = 0; e.q[3] = std::cos(yaw / 2);
    return e;
}
void test_place() {
    for (double yaw : {0.0, 1.0, -2.5}) {
        const u3::EyeView a = eye_at(-0.032, 1.2, 0.3, yaw), b = eye_at(0.032, 1.2, 0.3, yaw);
        const u3::Screen s = up::place(a, b);
        CHECK(s.valid && std::fabs(s.dist - up::kScreenDistanceM) < 1e-5);
        CHECK(std::fabs(s.anchor[0]) < 1e-9 && std::fabs(s.anchor[1] - 1.2) < 1e-9 && std::fabs(s.anchor[2] - 0.3) < 1e-9);
        // the screen's forward is the head's forward (rotate (0,0,-1) by the yaw quaternion)
        double f[3]; const double back[3] = {0, 0, -1}; up::rotate(a.q, back, f);
        CHECK(std::fabs(s.fwd[0] - f[0]) < 1e-6 && std::fabs(s.fwd[2] - f[2]) < 1e-6);
        // from the anchor the screen spans exactly the game lens (the HUD quad's angles in hud-menu mode at scale 1)
        CHECK(std::fabs(s.half_w / s.dist - up::kLensTanX) < 1e-5 && std::fabs(s.half_h / s.dist - up::kLensTanY) < 1e-5);
    }
    // head pointing straight down: no yaw, no placement (fail closed)
    u3::EyeView d = eye_at(0, 1.2, 0, 0); const double h = std::sqrt(0.5); d.q[0] = -h; d.q[1] = 0; d.q[2] = 0; d.q[3] = h;
    CHECK(!up::place(d, d).valid);
}

// ================================================================================================== blend maths (mirror of xr_brief3d.hlsl mode 2)
void test_blend_bits() {
    CHECK(up::blit_mode_bits(false, false, 0.6f) == 0u && up::blit_mode_bits(true, false, 0.6f) == 1u);   // table / stage-2 values unchanged
    CHECK(up::blit_mode_bits(false, true, 0.6f) == (2u | (153u << 8)) && up::blit_mode_bits(true, true, 0.0f) == 2u);
    CHECK(up::blit_mode_bits(false, true, 2.0f) == (2u | (242u << 8)) && up::blit_mode_bits(false, true, -1.0f) == 2u);   // clamped to 0..0.95
    CHECK(up::blit_mode_bits(false, true, std::nanf("")) == 2u);
    const double text[3] = {1.0, 0.9, 0.2}, panel[3] = {0.05, 0.05, 0.08}, clear[3] = {0, 0, 0};
    CHECK(up::coverage(text, 0.6) == 1.0 && up::coverage(panel, 0.6) == 0.6 && up::coverage(clear, 0.0) == 0.0);
    // over a bright cockpit (0.8): text replaces it, the panel dims it by the floor, an empty pixel with floor 0 leaves it unchanged
    CHECK(std::fabs(up::blend_one(1.0, 0.8, up::coverage(text, 0.6)) - 1.0) < 1e-12);
    CHECK(std::fabs(up::blend_one(0.05, 0.8, up::coverage(panel, 0.6)) - (0.05 + 0.32)) < 1e-12);
    CHECK(up::blend_one(0.0, 0.8, up::coverage(clear, 0.0)) == 0.8);
}

// ================================================================================================== the producer's pause frames
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
const uint64_t kUploadVa = 0x40000000ull;
int g_scene = 0, g_swap = 0;
// a flight HUD/menu ps2_v draw on the scene target (step124 hud-ring: 2560x1440 fmt 10, DS 20) with the flight cb0 (512,-448,2048,2048):
// GS x 1792..2304 / y 1824..2272 span the whole target.
UiFacts flight_facts(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z, const void* rt = &g_scene) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.vertices = 4; f.instances = 1; f.topology_type = 3;
    f.rt = rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = acz::brief3d::kCmpAlways; f.stencil = false; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[2] = 2560; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = u3::Cb0{512.0f, -448.0f, 2048.0f, 2048.0f}; f.vb_why = nullptr; f.stride = 64;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        float v[16] = {xy[i][0], xy[i][1], z, 1.0f, 0.1f, 0.1f, 0.1f, 0.6f, float(i & 1), float(i >> 1), 0, 0, 0, 0, 0, 0};
        std::memcpy(f.vtx + 64 * i, v, 64);
    }
    return f;
}
UiFacts other(const void* list, int64_t now, uint32_t vs8) { UiFacts f; f.list = list; f.now_ms = now; f.vs8 = vs8; return f; }
acz::brief3d::EyeView make_eye(double x, double y, double z, double yaw, double ipd) {
    acz::brief3d::EyeView e;
    e.orientation[0] = 0; e.orientation[1] = std::sin(yaw / 2); e.orientation[2] = 0; e.orientation[3] = std::cos(yaw / 2);
    e.position[0] = x + ipd * std::cos(yaw); e.position[1] = y; e.position[2] = z - ipd * std::sin(yaw);
    e.fov[0] = -0.785; e.fov[1] = 0.785; e.fov[2] = 0.785; e.fov[3] = -0.785;
    return e;
}
PoseIn pose(double yaw = 0.0, uint32_t gen = 1) {
    PoseIn p; p.ok = true; p.space_generation = gen;
    p.eyes[0] = make_eye(0, 1.2, 0, yaw, -0.032); p.eyes[1] = make_eye(0, 1.2, 0, yaw, 0.032);
    return p;
}
UiPlan step(Producer& p, const UiFacts& f, double yaw = 0.0, uint64_t completed = 0) {
    UiPlan pl = p.ui3d_draw(f, p.wants_poses(f.now_ms) ? pose(yaw) : PoseIn{}, done(completed));
    if (pl.record) p.ui3d_commit(pl);
    return pl;
}
void execute(Producer& p, const void* q, const void* list) {
    const void* ls[1] = {list};
    const auto t = p.before_execute(q, ls, 1);
    p.after_execute(t, true);
}
Producer& pause_producer(Producer& p, bool pause_opt = true) {
    u3::Options o; o.on = true; o.pause = pause_opt; p.configure_ui3d(o); p.gpu_ready(kUploadVa); return p;
}
// the consumer's heartbeat + the Present that latches it (HUD-MENU on)
void beat(Producer& p, bool on, int64_t now) { up::publish(on); p.on_present(now); }

// One paused flight frame in game order: the world (ACM), the HUD (small ps2 quads, a stencil-EQUAL radar draw, an excursion target), then
// the menu window: dimming panel (large), menu frame (medium), a glyph (small, z 1), the final pass to the swapchain.
struct FrameOut { UiPlan opener, panel, glyph; };
FrameOut paused_frame(Producer& p, const void* L, int64_t t) {
    CHECK(streq(step(p, other(L, t, u3::kAcmVs8)).why, "ui3d-pause-prefix"));
    CHECK(streq(step(p, other(L, t, u3::kCptVs8)).why, "ui3d-pause-prefix"));
    CHECK(streq(step(p, flight_facts(L, t, 2000, 2000, 2010, 2020, 64)).why, "ui3d-pause-prefix"));   // a HUD symbol
    UiFacts eq = flight_facts(L, t, 1900, 1900, 2100, 2100, 64); eq.stencil = true; eq.stencil_always = false;   // the radar's EQUAL draws
    CHECK(streq(step(p, eq).why, "ui3d-pause-prefix"));
    FrameOut o;
    o.opener = step(p, flight_facts(L, t, 1800, 1830, 2296, 2266, 64));    // the dimming panel: area ~0.94 -> opens, far
    o.panel = step(p, flight_facts(L, t, 1900, 1900, 2000, 1950, 64));     // the menu frame: medium -> mid
    o.glyph = step(p, flight_facts(L, t, 1950, 1910, 1956, 1930, 1));      // a glyph, z 1 -> near
    CHECK(streq(step(p, flight_facts(L, t, 1792, 1824, 2304, 2272, 0, &g_swap)).why, "ui3d-off-target"));   // the final pass
    return o;
}

void test_pause_off_is_stage2() {
    Producer p; pause_producer(p, false);   // u3p:0: the trigger changes nothing
    int L = 0;
    beat(p, true, 1000);
    CHECK(!p.ui3d_pause_live());
    CHECK(streq(step(p, other(&L, 1001, u3::kAcmVs8)).why, "ui3d-frame-has-3d") && p.ui3d_held(1002));
    CHECK(streq(step(p, flight_facts(&L, 1003, 1800, 1830, 2296, 2266, 64)).why, "ui3d-hold"));
    CHECK(p.ui3d_summary().find("pause-") == std::string::npos);   // the stage-2 line unchanged
    beat(p, false, 1004);
}

void test_pause_frame_published() {
    Producer p; pause_producer(p);
    CHECK(p.ui3d_pause_configured() && !p.ui3d_pause_live());
    int L = 0, Q = 0;
    // without the trigger: the flight's ACM holds UI3D off (stage 2)
    CHECK(streq(step(p, other(&L, 900, u3::kAcmVs8)).why, "ui3d-frame-has-3d") && p.ui3d_held(901));
    beat(p, true, 1000);
    CHECK(p.ui3d_pause_live());
    // in a paused frame the world/HUD before the menu window is skipped without a refusal or a hold
    CHECK(streq(step(p, other(&L, 1001, u3::kAcmVs8)).why, "ui3d-pause-prefix"));
    {   // a large draw to the swapchain (R10G10B10A2) is refused (no frame opens) and counted, nothing else
        UiFacts sw = flight_facts(&L, 1001, 1792, 1824, 2304, 2272, 0, &g_swap); sw.rtv0 = 24; sw.rt_fmt = 24;
        CHECK(streq(step(p, sw).why, "ui3d-pso-rtv") && p.tracked_lists() == 0);
    }
    const UiPlan a = step(p, flight_facts(&L, 1002, 1800, 1830, 2296, 2266, 64));
    CHECK(a.record && a.clear && a.layer == 0 && a.eye_draw[0] && a.eye_draw[1]);
    const UiPlan b = step(p, flight_facts(&L, 1002, 1900, 1900, 2000, 1950, 64));
    CHECK(b.record && !b.clear && b.layer == 1 && b.slot == a.slot);
    const UiPlan c = step(p, flight_facts(&L, 1002, 1950, 1910, 1956, 1930, 1));
    CHECK(c.record && c.layer == 2);
    // the text layer is nearer: larger disparity than the panel
    float ax0, ax1, cx0, cx1;
    std::memcpy(&ax0, a.vtx[0], 4); std::memcpy(&ax1, a.vtx[1], 4); std::memcpy(&cx0, c.vtx[0], 4); std::memcpy(&cx1, c.vtx[1], 4);
    CHECK(std::fabs(cx0 - cx1) > std::fabs(ax0 - ax1));
    CHECK(streq(step(p, flight_facts(&L, 1003, 1792, 1824, 2304, 2272, 0, &g_swap)).why, "ui3d-off-target"));
    beat(p, true, 1016);
    execute(p, &Q, &L);
    p.on_reset(&L, true);
    Held h; CHECK(p.acquire(0, done(1), h));
    CHECK(h.slot == a.slot && h.meta.pause && !h.meta.opaque && h.meta.replays == 3 && h.meta.space_generation == 1);
    p.release(h);
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";pause-live=1;pause-frames=1;pause-published=1;") && has(s, ";pause-placements=1") && has(s, "far=1;mid=1;near=1"));
    CHECK(s.size() < 480);
    beat(p, false, 1017);
}

void test_pause_full_frame_and_latched_placement() {
    Producer p; pause_producer(p);
    int L = 0, Q = 0;
    beat(p, true, 2000);
    FrameOut f1 = paused_frame(p, &L, 2001);
    CHECK(f1.opener.record && f1.opener.layer == 0 && f1.panel.layer == 1 && f1.glyph.layer == 2);
    beat(p, true, 2016); execute(p, &Q, &L); p.on_reset(&L, true);
    // the next frame with the head turned 30 degrees: the placement stays latched (same vertices for the same eye pose would need the same
    // screen; here we check the counter) while the pause goes on
    FrameOut f2 = paused_frame(p, &L, 2017);
    CHECK(f2.opener.record);
    beat(p, true, 2033); execute(p, &Q, &L); p.on_reset(&L, true);
    CHECK(has(p.ui3d_summary(), ";pause-frames=2;pause-published=2;") && has(p.ui3d_summary(), ";pause-placements=1"));
    // a pause gap > kPlacementGapMs: the next pause re-latches
    beat(p, false, 2040);
    beat(p, true, 2040 + up::kPlacementGapMs + 100);
    FrameOut f3 = paused_frame(p, &L, 2041 + up::kPlacementGapMs + 100);
    CHECK(f3.opener.record && has(p.ui3d_summary(), ";pause-placements=2"));
    beat(p, true, 2200 + up::kPlacementGapMs); execute(p, &Q, &L); p.on_reset(&L, true);
    beat(p, false, 2201 + up::kPlacementGapMs);
}

void test_pause_window_closed_and_rejections() {
    Producer p; pause_producer(p);
    int L = 0, M = 0, Q = 0;
    beat(p, true, 3000);
    CHECK(step(p, flight_facts(&L, 3001, 1800, 1830, 2296, 2266, 64)).record);
    // a non-ps2_v draw inside the window closes it (no hold, no rejection by itself)
    CHECK(streq(step(p, other(&L, 3002, u3::kMovieVs8)).why, "ui3d-pause-window-closed") && !p.ui3d_held(3003));
    CHECK(streq(step(p, flight_facts(&L, 3003, 1792, 1824, 2304, 2272, 0, &g_swap)).why, "ui3d-off-target"));   // other targets: skipped
    // ... a later menu draw on the target is an unknown structure: the frame is rejected (never shown)
    CHECK(streq(step(p, flight_facts(&L, 3004, 1900, 1900, 2000, 1950, 64)).why, "ui3d-pause-draw-after-3d"));
    beat(p, true, 3016); execute(p, &Q, &L); p.on_reset(&L, true);
    Held h; CHECK(!p.acquire(0, done(10), h));
    CHECK(has(p.ui3d_summary(), ";rejected=1;") && has(p.ui3d_summary(), ";pause-published=0;"));
    // a refusal inside the window (stencil EQUAL after the opener) rejects the frame as in stage 2
    CHECK(step(p, flight_facts(&M, 3020, 1800, 1830, 2296, 2266, 64), 0.0, 10).record);
    UiFacts eq = flight_facts(&M, 3021, 1900, 1900, 2000, 1950, 64); eq.stencil = true; eq.stencil_always = false;
    CHECK(streq(step(p, eq).why, "ui3d-stencil-not-always"));
    beat(p, true, 3032); execute(p, &Q, &M); p.on_reset(&M, true);
    CHECK(!p.acquire(0, done(10), h) && has(p.ui3d_summary(), ";rejected=2;"));
    // a frame without any large opener publishes nothing (only prefix draws)
    CHECK(streq(step(p, flight_facts(&L, 3040, 2000, 2000, 2010, 2020, 64)).why, "ui3d-pause-prefix") && p.tracked_lists() == 0);
    beat(p, false, 3050);
}

void test_pause_stale_trigger() {
    Producer p; pause_producer(p);
    int L = 0;
    beat(p, true, 4000);
    CHECK(p.ui3d_pause_live());
    p.on_present(4000 + up::kSignalStaleMs + 1);   // no new heartbeat (the XR owner stopped publishing): pause off by itself
    CHECK(!p.ui3d_pause_live());
    CHECK(streq(step(p, other(&L, 4300, u3::kAcmVs8)).why, "ui3d-frame-has-3d") && p.ui3d_held(4301));
    CHECK(streq(step(p, flight_facts(&L, 4302, 1800, 1830, 2296, 2266, 64)).why, "ui3d-hold"));
    // the trigger off at the Present: an OPEN pause frame keeps its own rules until its Present (the frame kind is the frame's)
    beat(p, true, 5000);
    CHECK(step(p, flight_facts(&L, 5001, 1800, 1830, 2296, 2266, 64)).record);
    up::publish(false);
    CHECK(step(p, flight_facts(&L, 5002, 1900, 1900, 2000, 1950, 64)).record);
    beat(p, false, 5016);
}

// ================================================================================================== step146 regression: pause + dbr3d
// step146 runAGU1 (cfg ...;fxtrail:8;u3pp:1;u3lt:6;dbr3d:1;ui3d:1;u3p:1;u3a:1): pause-frames 2, pause-published 0, consumer no-slot 1304,
// summary-dbr3d arms=2 at exactly the two pause entries. The pause menu's dimming panel has the debrief window signature (ps2_v / d34ee153,
// SRC_ALPHA / INV_SRC_ALPHA, z 2^24, large), so it armed the debrief route and the glue sent the pause's ps2_v draws to dbr3d_draw.
// The glue's routing (ui3d_replay_hooks.inl ui3dDraw) is mirrored here: a ps2_v draw goes to dbr3d_draw while dbr3d_armed(now), else ui3d_draw.
struct Router {
    Producer& p;
    std::vector<uint8_t> eye[2];
    unsigned to_dbr = 0;
    uint64_t completed = 0;   // the producer fence value the GPU has reached (slots of earlier frames become free)
    explicit Router(Producer& q) : p(q) { for (auto& e : eye) e.assign(size_t(acz::dbr3d::kMaxVerticesPerDraw) * acz::dbr3d::kVertexStride, 0); }
    const char* draw(const UiFacts& f) {
        if (f.vs8 == u3::kAcmVs8 && p.ui3d_hangar_configured()) {   // u3a: DrawACM_v goes to ui3d_acm
            AcmFacts a; a.list = f.list; a.now_ms = f.now_ms;
            const AcmPlan pl = p.ui3d_acm(a, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(completed));
            if (pl.record) { p.ui3d_acm_commit(pl); return nullptr; }
            return pl.why;
        }
        if (f.vs8 == u3::kPs2Vs8 && p.dbr3d_armed(f.now_ms)) {
            ++to_dbr;
            DbrFacts d; d.ui = f; d.nv = f.vertices; d.vtx = f.vtx; d.vtx_why = f.vb_why;
            uint8_t* const dst[2] = {eye[0].data(), eye[1].data()};
            const DbrPlan pl = p.dbr3d_draw(d, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(completed), dst);
            if (pl.record) { p.dbr3d_commit(pl); return nullptr; }
            return pl.why;
        }
        const UiPlan pl = step(p, f, 0.0, completed);
        return pl.record ? nullptr : pl.why;
    }
};
// the pause menu's dimming panel as the live census shows the debrief window signature: menu ps8, SRC_ALPHA / INV_SRC_ALPHA, z 2^24
UiFacts pause_window(const void* L, int64_t t) {
    UiFacts f = flight_facts(L, t, 1800, 1830, 2296, 2266, 16777216.0f);
    f.ps8 = acz::dbr3d::kMenuPs8; f.blend = true; f.blend_src = acz::dbr3d::kBlendSrcAlpha; f.blend_dst = acz::dbr3d::kBlendInvSrcAlpha;
    return f;
}
void test_pause_with_step146_options() {
    Producer p;
    std::string rest, why; u3::Options uo;
    CHECK(u3::extract_options("auto;env:1;vscreen:1;vsearly:1;brief3d:1;fxtrail:8;u3pp:1;u3lt:6;dbr3d:1;ui3d:1;u3p:1;u3a:1", rest, uo, why));
    CHECK(uo.on && uo.pause && uo.hangar == 1 && uo.panel_plane && std::fabs(uo.frame_mm - 6.0f) < 1e-6f);
    acz::dbr3d::Options dbo; std::string rest2;
    CHECK(acz::dbr3d::extract_options(rest, rest2, dbo, why) && dbo.on);
    p.configure_ui3d(uo); p.configure_dbr3d(dbo); p.gpu_ready(kUploadVa);
    CHECK(p.dbr3d_configured() && p.ui3d_pause_configured());
    {   // the pause window's facts ARE the debrief window signature (the reason the route armed live)
        const UiFacts w = pause_window(nullptr, 0); acz::dbr3d::GsRect r;
        CHECK(acz::dbr3d::window_from_vertices(w.vs8, w.ps8, false, w.cb0, w.blend, w.blend_src, w.blend_dst, w.vtx, 4, r));
    }
    Router R(p);
    int L = 0, Q = 0;
    int64_t t = 6000;
    beat(p, true, t);
    for (int k = 0; k < 4; ++k, t += 16) {   // four paused flight frames in game order
        CHECK(streq(R.draw(other(&L, t + 1, u3::kAcmVs8)), "ui3d-pause-prefix"));
        CHECK(streq(R.draw(flight_facts(&L, t + 1, 2000, 2000, 2010, 2020, 64)), "ui3d-pause-prefix"));   // a HUD symbol
        CHECK(R.draw(pause_window(&L, t + 2)) == nullptr);                                                // opens the pause frame
        CHECK(!p.dbr3d_armed(t + 2));                                                                     // ... and arms nothing
        CHECK(R.draw(flight_facts(&L, t + 3, 1900, 1900, 2000, 1950, 64)) == nullptr);                   // menu frame (a u3lt rim)
        CHECK(R.draw(flight_facts(&L, t + 3, 1950, 1910, 1956, 1930, 1)) == nullptr);                    // a glyph
        CHECK(streq(R.draw(other(&L, t + 4, u3::kMovieVs8)), "ui3d-pause-window-closed"));               // a later non-ps2_v draw
        CHECK(streq(R.draw(flight_facts(&L, t + 4, 1792, 1824, 2304, 2272, 0, &g_swap)), "ui3d-off-target"));   // the final pass
        beat(p, true, t + 16); execute(p, &Q, &L); p.on_reset(&L, true);
        R.completed = uint64_t(k) + 1;   // this frame's submission completed
        Held h; CHECK(p.acquire(0, done(R.completed), h) && h.meta.pause && !h.meta.opaque && h.meta.replays == 3);
        p.release(h);
    }
    CHECK(R.to_dbr == 0 && p.dbr3d_stats().arms == 0 && p.dbr3d_stats().frames == 0);
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";pause-frames=4;pause-published=4;") && has(s, ";rejected=0;") && has(s, ";sel=3;rim=1;"));
    // (frame 1: the menu frame is a u3lt rim; frames 2-4: the same box under the previous frame's text row = the selection slab)
    // the trigger off: the same window signature arms the debrief route again (DEBRIEF-3D unchanged outside the pause)
    beat(p, false, t + 20);
    CHECK(!p.ui3d_pause_live());
    UiFacts w = pause_window(&L, t + 21);
    R.draw(w);
    CHECK(p.dbr3d_armed(t + 22) && p.dbr3d_stats().arms == 1);
    // and an armed route is ignored while the trigger is live (an in-flight pause is never a debrief)
    beat(p, true, t + 30);
    CHECK(p.ui3d_pause_live() && !p.dbr3d_armed(t + 31));
    beat(p, false, t + 40);
}

// ================================================================================================== the consumer's PauseCore
struct FakeGate final : xr_owner::Gate {
    int64_t t = 0;
    int64_t now_ms() override { return t; }
    void sleep_ms(int64_t ms) override { t += ms; }
    bool lost() const override { return false; }
    bool stopping() const override { return false; }
protected:
    xr_owner::Rc invoke(xr_owner::Op, xr_owner::Rc (*fn)(void*), void* p) override { return fn(p); }
};
struct FakeGpu final : xr_stereo::TableGpu {
    uint64_t value = 0, completed_v = 0; bool lose = false, signal_fails = false, record_fails = false, setup_fails = false;
    unsigned blits = 0, setups = 0, destroys = 0; xr_stereo::TableImages last; void* d0 = nullptr; void* d1 = nullptr;
    bool setup(const xr_stereo::SwapchainImages&, const xr_stereo::SwapchainImages&, const char*& why) override {
        ++setups; if (setup_fails) { why = "rtv-heap"; return false; } return true;
    }
    Completion completed() override { Completion c; if (lose) c.lost = true; else c.value = completed_v; return c; }
    Submit blit(unsigned, void* a, void* b, const xr_stereo::TableImages& src, xr_stereo::Extent, xr_stereo::Extent) override {
        ++blits; Submit s; last = src; d0 = a; d1 = b;
        if (record_fails) return s;
        s.executed = true;
        if (signal_fails) return s;
        s.signalled = true; s.value = ++value; return s;
    }
    void destroy() override { ++destroys; }
};
// A source that hands out queued slots (several may be held, as the ring allows) and checks every release.
struct FakeSource final : xr_stereo::TableSource {
    std::deque<Held> ready; std::vector<uint64_t> held; unsigned released = 0, violations = 0; bool dead_ = false;
    int64_t stamp = 10;   // published_ms of the next add() (the fake clock is the Gate's: TableSource::clock_ms default)
    void add(uint64_t id, bool pause_slot, uint32_t gen = 1) {
        Held h; h.slot = int(id % 3); h.a.slot = h.slot; h.a.id = id; h.a.value = id; h.meta.pause = pause_slot; h.meta.opaque = !pause_slot;
        h.meta.space_generation = gen; h.meta.frame = id; h.meta.published_ms = stamp; ready.push_back(h);
    }
    bool acquire(uint64_t after, Held& out, xr_stereo::TableImages& img) override {
        while (!ready.empty() && ready.front().a.id <= after) ready.pop_front();
        if (ready.empty()) return false;
        out = ready.back(); ready.clear();   // the newest
        img.eye[0] = reinterpret_cast<void*>(uintptr_t(0x100 + out.slot)); img.eye[1] = reinterpret_cast<void*>(uintptr_t(0x200 + out.slot));
        img.width = kEyeW; img.height = kEyeH; img.format = kRtvFormat; img.opaque = out.meta.opaque; img.pause = out.meta.pause;
        held.push_back(out.a.id);
        return true;
    }
    void release(const Held& h) override {
        bool found = false;
        for (size_t i = 0; i < held.size(); ++i) if (held[i] == h.a.id) { held.erase(held.begin() + long(i)); found = true; break; }
        if (!found) ++violations;
        ++released;
    }
    bool dead() override { return dead_; }
    std::string describe() override { return ""; }
};
struct PauseRig {
    FakeGate g; std::shared_ptr<FakeGpu> gpu = std::make_shared<FakeGpu>(); std::shared_ptr<FakeSource> src = std::make_shared<FakeSource>();
    xr_stereo::PauseCore core{gpu, src};
    xr_stereo::SwapchainImages i0, i1; int img0 = 0, img1 = 0;
    acz::brief3d::EyeView eyes[2];   // the composite's eyes (default == the fake slots' default eyes)
    PauseRig() { i0.count = i1.count = 1; i0.images[0] = &img0; i1.images[0] = &img1; }
    bool frame(uint32_t gen = 1, float floor = 0.6f) {
        return core.frame(g, i0, i1, &img0, &img1, xr_stereo::Extent{2400, 1350}, xr_stereo::Extent{2400, 1350}, gen, floor, eyes);
    }
};
// encode_space_generation(raw) of the consumer = raw + 1 (the slots carry the encoded value)
void test_pause_core() {
    {   // nothing published: no layer
        PauseRig r; r.g.t = 10;
        CHECK(!r.frame(0) && r.core.stats().no_slot == 1 && r.gpu->blits == 0);
    }
    {   // a pause slot: blended into the two acquired eye images with the floor; held while shown, re-blended without a new slot
        PauseRig r; r.g.t = 10;
        r.src->add(1, true, xr_stereo::encode_space_generation(0));
        CHECK(r.frame(0) && r.gpu->blits == 1 && r.gpu->last.pause && !r.gpu->last.opaque && std::fabs(r.gpu->last.floor - 0.6f) < 1e-6f);
        CHECK(r.gpu->d0 == &r.img0 && r.gpu->d1 == &r.img1 && r.gpu->setups == 1 && r.core.holding() && r.src->held.size() == 1);
        r.g.t = 21; r.gpu->completed_v = 0;   // allocator 1 is free (value 0), allocator 0 busy until 1 completes
        CHECK(r.frame(0) && r.gpu->blits == 2 && r.src->held.size() == 1);
        r.g.t = 32;
        CHECK(!r.frame(0) && r.core.stats().alloc_busy == 1);   // allocator 0 still busy (value 1 not completed)
        r.gpu->completed_v = 2;
        r.g.t = 33;
        CHECK(r.frame(0) && r.gpu->blits == 3);
        // a newer pause slot replaces it: the old one is released only after its last blend completed
        r.src->add(2, true, 1);
        r.g.t = 40; r.gpu->completed_v = 2;
        CHECK(r.frame(0) && r.src->held.size() == 2 && r.core.retiring() == 1);
        r.gpu->completed_v = 3; r.g.t = 50;   // blend value 3 (the old slot's last) completed
        CHECK(r.frame(0) && r.src->held.size() == 1 && r.src->held[0] == 2 && r.src->violations == 0);
        // stale: no new slot for > 500 ms -> no layer (Output keeps the HUD quad)
        r.g.t = 50 + xr_stereo::PauseCore::kStaleMs + 1;
        CHECK(!r.frame(0) && r.core.stats().stale == 1);
        // menu mode off: idle retires the shown slot; released once its blends completed
        r.gpu->completed_v = r.gpu->value;
        r.core.idle(r.g);
        CHECK(!r.core.holding() && r.src->held.empty() && r.src->violations == 0);
        CHECK(r.core.retire(r.g) && r.gpu->destroys == 1);
        const std::string line = r.core.summary();
        CHECK(has(line, "xr-stereo;event=ui3d-pause-result;") && has(line, ";blends=5;") && line.size() < acz::brief3d::kLineLimit);
    }
    {   // a non-pause slot (menu UI3D / briefing) is released unused; another LOCAL generation is refused
        PauseRig r; r.g.t = 10;
        r.src->add(1, false, 1);
        CHECK(!r.frame(0) && r.core.stats().not_pause == 1 && r.src->held.empty());
        r.src->add(2, true, 7);
        CHECK(!r.frame(0) && r.core.stats().generation_refused == 1 && r.src->held.empty());
        // a held slot of the previous generation is dropped when the generation changes
        r.src->add(3, true, 1);
        CHECK(r.frame(0));
        r.gpu->completed_v = r.gpu->value;
        CHECK(!r.frame(5) && r.core.stats().generation_dropped == 1 && !r.core.holding());
        r.core.idle(r.g);
        CHECK(r.src->held.empty() && r.src->violations == 0);
    }
    {   // Execute without Signal: quarantined + poisoned (Output fails the session); the slot stays held for ever
        PauseRig r; r.g.t = 10; r.gpu->signal_fails = true;
        r.src->add(1, true, 1);
        CHECK(!r.frame(0) && r.core.poisoned() && r.core.holding());
        CHECK(!r.core.retire(r.g) && r.src->released == 0);
    }
    {   // a fence loss releases nothing (loss is never completion)
        PauseRig r; r.g.t = 10;
        r.src->add(1, true, 1); CHECK(r.frame(0));
        r.src->add(2, true, 1); r.gpu->completed_v = 1; CHECK(r.frame(0) && r.core.retiring() == 1);
        r.gpu->lose = true; r.g.t = 20;
        CHECK(!r.frame(0) && r.core.dead() && r.core.retiring() == 1 && r.src->released == 0);
    }
    {   // setup failure: never usable, nothing blitted
        PauseRig r; r.gpu->setup_fails = true; r.src->add(1, true, 1);
        CHECK(!r.frame(0) && r.core.dead() && r.gpu->blits == 0 && streq(r.core.why(), "rtv-heap"));
        // F3: the partial setup (fence, allocators, list, root, PSOs created before the failure) is destroyed once by a successful retirement
        CHECK(r.core.retire(r.g) && r.gpu->destroys == 1 && r.src->held.empty() && r.src->violations == 0);
        CHECK(r.core.retire(r.g) && r.gpu->destroys == 1);   // not twice
    }
    {   // F2: the age counts from the PUBLICATION, not from the acquisition
        PauseRig r; r.g.t = 1000;
        r.src->stamp = 1000 - xr_stereo::PauseCore::kStaleMs - 1;   // published > 500 ms ago, acquired only now
        r.src->add(1, true, 1);
        CHECK(!r.frame(0) && r.gpu->blits == 0 && r.core.stats().stale_published == 1 && !r.core.holding() && r.src->held.empty() && r.src->violations == 0);
        r.src->stamp = 1000 - xr_stereo::PauseCore::kStaleMs;       // exactly at the limit: accepted
        r.src->add(2, true, 1);
        CHECK(r.frame(0) && r.gpu->blits == 1);
        r.g.t = 1001;                                                // a re-composite of a slot that turned too old expires by its publication time
        r.gpu->completed_v = r.gpu->value;
        CHECK(!r.frame(0) && r.core.stats().stale == 1 && r.gpu->blits == 1);
        r.src->stamp = 0; r.src->add(3, true, 1);                    // unset publication time: invalid
        CHECK(!r.frame(0) && r.core.stats().stale_published == 2);
        r.src->stamp = 1001 + xr_stereo::PauseCore::kFutureSlackMs + 1; r.src->add(4, true, 1);   // from the future: invalid
        CHECK(!r.frame(0) && r.core.stats().stale_published == 3);
        r.src->stamp = 1001 + xr_stereo::PauseCore::kFutureSlackMs; r.src->add(5, true, 1);        // within the slack: accepted
        CHECK(r.frame(0) && r.gpu->blits == 2);
    }
    {   // F1: the slot is blended only into a composite submitted with the same eyes (pose and FOV); otherwise the HUD quad stays
        PauseRig r; r.g.t = 10;
        r.src->add(1, true, 1);
        CHECK(r.frame(0) && r.gpu->blits == 1);
        r.gpu->completed_v = r.gpu->value;
        r.eyes[1].position[0] += 1e-3;   // right eye moved 1 mm (head motion between producer capture and the XR composite)
        r.g.t = 11;
        CHECK(!r.frame(0) && r.gpu->blits == 1 && r.core.stats().view_mismatch == 1);
        r.eyes[1].position[0] -= 1e-3;
        r.eyes[0].position[2] += 1e-4;   // 0.1 mm: within the tolerance
        CHECK(r.frame(0) && r.gpu->blits == 2);
        r.gpu->completed_v = r.gpu->value;
        r.eyes[0].orientation[0] = 0.01; // ~1.1 degrees (unnormalised on purpose: it is normalised for the angle)
        CHECK(!r.frame(0) && r.core.stats().view_mismatch == 2);
        r.eyes[0].orientation[0] = 0.0;
        r.eyes[1].fov[1] = 0.9;          // another FOV (the composite's lens/HMD FOV differs from the one the slot was rendered with)
        CHECK(!r.frame(0) && r.core.stats().view_mismatch == 3);
        r.eyes[1].fov[1] = 0.8;
        CHECK(r.frame(0) && r.gpu->blits == 3);
        CHECK(!r.core.frame(r.g, r.i0, r.i1, &r.img0, &r.img1, {2400, 1350}, {2400, 1350}, 0, 0.6f, nullptr) && r.core.stats().view_mismatch == 4);   // eyes unknown
        // the producer's eyes can be anything (a moved head): a slot rendered with them blends into a composite with exactly them
        PauseRig m; m.g.t = 10;
        m.src->add(1, true, 1);
        m.src->ready.back().meta.eyes[0].position[0] = -0.032; m.src->ready.back().meta.eyes[0].orientation[1] = 0.25881904510252074; m.src->ready.back().meta.eyes[0].orientation[3] = 0.9659258262890683;
        m.eyes[0] = m.src->ready.back().meta.eyes[0];
        CHECK(m.frame(0) && m.gpu->blits == 1);
        char buf[acz::brief3d::kLineLimit + 64];   // the summary line carries the two new counters and stays under the limit
        xr_stereo::PauseCore::Stats big; big.stale_published = big.view_mismatch = big.blends = ~0ull;
        CHECK(xr_stereo::PauseCore::format_line(buf, sizeof buf, true, ~0ll, big, 9, true, true, "0123456789012345678901234567890123") < int(acz::brief3d::kLineLimit));
        CHECK(has(buf, ";stale-pub=9999999;view-mismatch=9999999;"));
    }
    {   // the hook: the base VScreenHook has no pause layer; Brief3dHook forwards to its PauseCore only when it has one
        struct Base final : xr_stereo::VScreenHook {
            bool frame(xr_owner::Gate&, const xr_owner::FrameInfo&, xr_stereo::SwapchainRuntime&, unsigned, const xr_stereo::SwapchainImages&, xr_stereo::Extent,
                       const xr_stereo::VScreenIn&, xr_owner::LayerPlan&) override { return false; }
            bool exclusive() const noexcept override { return false; }
            bool retire(xr_owner::Gate&) override { return true; }
            std::string summary() override { return {}; }
            bool poisoned() const noexcept override { return false; }
            void target_written(unsigned) noexcept override {}
        };
        PauseRig r; auto base = std::make_shared<Base>();
        CHECK(!base->pause_frame(r.g, r.i0, r.i1, &r.img0, &r.img1, {1, 1}, {1, 1}, 0, 0.6f, nullptr));
        auto pc = std::make_shared<xr_stereo::PauseCore>(r.gpu, r.src);
        xr_stereo::Brief3dHook none(base, std::make_shared<xr_stereo::TableCore>(r.gpu, r.src));
        r.src->add(1, true, 1);
        CHECK(!none.pause_frame(r.g, r.i0, r.i1, &r.img0, &r.img1, {2400, 1350}, {2400, 1350}, 0, 0.6f, r.eyes) && r.gpu->blits == 0);
        xr_stereo::Brief3dHook with(base, std::make_shared<xr_stereo::TableCore>(r.gpu, r.src), pc);
        CHECK(with.pause_frame(r.g, r.i0, r.i1, &r.img0, &r.img1, {2400, 1350}, {2400, 1350}, 0, 0.6f, r.eyes) && r.gpu->blits == 1);
        r.gpu->completed_v = r.gpu->value;
        with.pause_idle(r.g);
        CHECK(!pc->holding() && r.src->held.empty());
    }
}

// ================================================================================================== census TAIL mode
void test_census_tail() {
    using namespace acz::u3c;
    Accumulator a; a.set_armed(true, true);
    CHECK(a.tail);
    CHECK(!a.present(1000000));   // starts sampling
    uint32_t serial = 0, slot = 0, all = 0, ord = 0;
    // a paused flight frame: 120 menu-key draws (HUD first, menu last) + other keys
    for (uint32_t i = 1; i <= 120; ++i) {
        if (i % 10 == 0) CHECK(a.admit(false, 0xb569109eu, 0x1234u, 3, true, false, 300, 1, serial, slot, all, ord) == Admit::count);
        CHECK(a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
        CHECK(ord == i && slot == (i - 1) % kMaxDraws);
        DrawRec d; d.ord = ord; d.all = all; d.count = 4;
        if (i != 119) a.commit(serial, slot, d);   // 119 stays admitted-not-committed
    }
    // a late commit for an ordinal whose slot was reused is dropped (ord 30 lived in slot 29, now owned by ord 78)
    { DrawRec late; late.ord = 30; late.count = 99; a.commit(serial, 29, late); }
    CHECK(a.present(1000001));
    Frame out; a.take(out);
    CHECK(out.tail && out.menu == 120 && out.rec[29].ord == 78 && out.rec[29].count == 4);
    std::vector<std::string> lines;
    const unsigned n = emit_frame(out, 1, 100, [&](const char* t) { lines.push_back(t); });
    CHECK(n == lines.size() && lines.size() == 1 + kMaxDraws + 1);
    CHECK(has(lines[0], ";menu=120;logged=48;") && has(lines[0], ";mode=tail"));
    CHECK(has(lines[1], "kind=draw;s=1;ord=73;"));        // the oldest kept: 120 - 48 + 1
    CHECK(has(lines[47], "ord=119;incomplete=admitted-not-committed"));
    CHECK(has(lines[48], "kind=draw;s=1;ord=120;"));
    CHECK(has(lines[49], "kind=other;"));
    // head mode is unchanged: the first 48 menu draws, no mode token
    Accumulator h; h.set_armed(true);
    CHECK(!h.tail && !h.present(1000000));
    for (uint32_t i = 1; i <= 60; ++i) {
        const Admit ad = h.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord);
        CHECK(ad == (i <= kMaxDraws ? Admit::take : Admit::count));
        if (ad == Admit::take) { DrawRec d; d.ord = ord; h.commit(serial, slot, d); }
    }
    CHECK(h.present(1000001));
    Frame ho; h.take(ho); lines.clear();
    emit_frame(ho, 1, 100, [&](const char* t) { lines.push_back(t); });
    CHECK(!has(lines[0], "mode=tail") && has(lines[1], "ord=1;") && has(lines[48], "ord=48;"));
}

} // namespace

int main() {
    test_option();
    test_gate();
    test_place();
    test_blend_bits();
    test_pause_off_is_stage2();
    test_pause_frame_published();
    test_pause_full_frame_and_latched_placement();
    test_pause_window_closed_and_rejections();
    test_pause_stale_trigger();
    test_pause_with_step146_options();
    test_pause_core();
    test_census_tail();
    std::printf("ui3d_pause_offline: %u checks passed\n", g_checks);
    return 0;
}
