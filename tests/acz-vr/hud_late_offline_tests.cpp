// hud_late_offline_tests.cpp -- HUD-LATE (xr_terrain_stage hudlate:1; src/acz-vr/hud_late.h; docs/world-render/markers-360.md section 18): the HUD text
// class of the post-tone-map draws (radio subtitles), the late-skip decision, the counters and the status line, the stage switch plumbing (effects_stage_flags.h)
// and the option parse (terrain_stage.cpp). Pure: no device, no game. Exit 0 = every check passed.
#include "hud_late.h"
#include "effects_stage_flags.h"
#include "terrain_stage.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
int g_checks = 0, g_fails = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
namespace s5 = acz::gpu_copy::step5;

// The subtitle draw as recorded (J-radar-analysis.md 1.3): VS 534f29ca / PS d34ee153, blend 5/6, depth ALWAYS no write, stencil ALWAYS/KEEP.
s5::HudLateDraw subtitle() {
    s5::HudLateDraw d; d.vs8 = 0x534f29cau; d.ps8 = 0xd34ee153u; d.blend = true; d.src = 5; d.dst = 6;
    d.depth = true; d.depth_write = false; d.depth_func = 8; d.stencil = true; d.front_func = 8; d.back_func = 8; d.stencil_ops_keep = true;
    return d;
}

void test_text_class() {
    CHECK(s5::hud_late_text(subtitle()) == nullptr);
    { auto d = subtitle(); d.depth = false; d.depth_func = 0; d.stencil = false; d.front_func = d.back_func = 0; CHECK(s5::hud_late_text(d) == nullptr); }   // depth / stencil off
    { auto d = subtitle(); d.vs8 = 0; CHECK(std::strcmp(s5::hud_late_text(d), "shader-unknown") == 0); }
    { auto d = subtitle(); d.ps8 = 0x4caf6e2du; CHECK(std::strcmp(s5::hud_late_text(d), "not-text-shader") == 0); }          // the cloud sprite PS
    { auto d = subtitle(); d.vs8 = 0x127819e3u; CHECK(std::strcmp(s5::hud_late_text(d), "not-text-shader") == 0); }          // the a3050 tone-map VS
    { auto d = subtitle(); d.dst = 2; CHECK(std::strcmp(s5::hud_late_text(d), "not-text-blend") == 0); }                     // gun tracer SRC_ALPHA / ONE
    { auto d = subtitle(); d.src = 2; d.dst = 2; CHECK(std::strcmp(s5::hud_late_text(d), "not-text-blend") == 0); }          // sun flare ONE / ONE
    { auto d = subtitle(); d.blend = false; CHECK(std::strcmp(s5::hud_late_text(d), "not-text-blend") == 0); }
    { auto d = subtitle(); d.depth_func = 5; CHECK(std::strcmp(s5::hud_late_text(d), "depth-tested") == 0); }               // firing wedge GREATER
    { auto d = subtitle(); d.depth_write = true; CHECK(std::strcmp(s5::hud_late_text(d), "depth-tested") == 0); }
    { auto d = subtitle(); d.front_func = d.back_func = 3; CHECK(std::strcmp(s5::hud_late_text(d), "stencil") == 0); }       // radar contact EQUAL
    { auto d = subtitle(); d.front_func = d.back_func = 6; CHECK(std::strcmp(s5::hud_late_text(d), "stencil") == 0); }       // NOT_EQUAL quad
    { auto d = subtitle(); d.stencil_ops_keep = false; CHECK(std::strcmp(s5::hud_late_text(d), "stencil") == 0); }           // REPLACE mask writer
    { auto d = subtitle(); d.back_func = 3; CHECK(std::strcmp(s5::hud_late_text(d), "stencil") == 0); }
}

void test_decide() {
    CHECK(s5::hud_late_decide(false, true, nullptr, false) == s5::hud_late_skip);        // key off: the original late skip
    CHECK(s5::hud_late_decide(true, false, nullptr, false) == s5::hud_late_skip);        // not text: still skipped
    CHECK(s5::hud_late_decide(true, true, nullptr, false) == s5::hud_late_replay);
    CHECK(s5::hud_late_decide(true, true, "stencil-test-not-always", false) == s5::hud_late_skip_refused);   // a refusal never invalidates: skipped
    CHECK(s5::hud_late_decide(true, true, nullptr, true) == s5::hud_late_skip_capped);
    CHECK(s5::hud_late_decide(true, true, "x", true) == s5::hud_late_skip_refused);      // the refusal is named first
    CHECK(s5::hud_late_decide(true, false, "x", true) == s5::hud_late_skip);
}

void test_sums_and_line() {
    s5::HudLateSums s;
    // the step146 runAGB2 shape: quiet frames (0 late draws) and subtitle frames (frame 312: 87 late draws, frame 1212: 43)
    s5::HudLateFrame q; s.add(q); s.add(q);
    s5::HudLateFrame a; a.draws = 87; s.add(a);
    s5::HudLateFrame b; b.draws = 40; b.capped = 3; b.refused = 1; b.other = 2; b.lastRefusal = "hud-cap"; s.add(b);
    CHECK(s.frames == 4 && s.framesWithText == 2 && s.draws == 127 && s.maxDraws == 87 && s.capped == 3 && s.refused == 1 && s.other == 2);
    CHECK(s.lastRefusal && std::strcmp(s.lastRefusal, "hud-cap") == 0);
    char line[480];
    const int n = s5::format_hud_late(line, sizeof line, "continuous", s);
    CHECK(n > 0 && size_t(n) == std::strlen(line));
    CHECK(std::strstr(line, "continuous-hud-late;on=1;frames=4(text:2);text-draws=127;max-per-frame=87;refused=1;capped=3;late-other=2;last-refusal=hud-cap;") == line);
    s5::HudLateSums big; big.frames = big.framesWithText = big.draws = big.refused = big.capped = big.other = big.maxDraws = 18446744073709551615ull;
    big.lastRefusal = "a-very-long-refusal-reason-that-is-cut-at-forty-characters-by-the-format";
    const int m = s5::format_hud_late(line, sizeof line, "continuous-with-a-long-prefix-name", big);
    CHECK(m > 0 && m <= 440);   // under the 480-byte log line cap with room for the tick prefix
    std::printf("hud-late line max %d bytes\n", m);
    char tiny[16];
    CHECK(s5::format_hud_late(tiny, sizeof tiny, "continuous", s) == 15 && std::strlen(tiny) == 15);
    CHECK(s5::format_hud_late(nullptr, 10, "x", s) == 0);
    CHECK(std::strstr(s5::hud_late_option_line(), "event=hud-late-option;on=1;") != nullptr);
    CHECK(std::strstr(s5::hud_late_option_line(), "gpu_copy_step5_hud_radar") != nullptr);
}

void test_stage_switch() {
    CHECK(!s5::hud_late_flag().load());   // default off
    s5::StageEffects e = s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, true);
    CHECK(e.hud_late && e.sprite_gpu && e.hud_rs_skip);   // independent of the other switches
    s5::apply_stage_effects(e);
    CHECK(s5::hud_late_flag().load());
    CHECK(!s5::stage_effects(0, false).hud_late);           // the old call shape keeps it off
    s5::clear_stage_effects();
    CHECK(!s5::hud_late_flag().load());
    CHECK(s5::hud_rs_skip_flag().load());                   // the cleared defaults are unchanged
}

void test_onset() {
    s5::HudLateOnset o;
    CHECK(!o.step(0, 1000) && !o.step(7, 1000));                     // quiet / a stray glyph is not an onset
    CHECK(o.step(40, 1000) && o.shots == 1);                          // the first subtitle frame (quiet since the start)
    for (int i = 0; i < 100; ++i) CHECK(!o.step(40, 1000 + i));      // the same subtitle continues: no second request
    for (int i = 0; i < 29; ++i) CHECK(!o.step(0, 5000));
    CHECK(!o.step(40, 5000));                                         // 29 quiet frames are a flicker, not a new onset
    for (int i = 0; i < 30; ++i) CHECK(!o.step(0, 6000));
    CHECK(!o.step(40, 6000));                                         // a new onset, but within 20 s of the last shot
    for (int i = 0; i < 30; ++i) CHECK(!o.step(0, 30000));
    CHECK(o.step(40, 30000) && o.shots == 2);
    for (int i = 0; i < 30; ++i) CHECK(!o.step(0, 60000));
    CHECK(o.step(12, 60000) && o.shots == 3);
    for (int i = 0; i < 30; ++i) CHECK(!o.step(0, 90000));
    CHECK(!o.step(40, 90000) && o.shots == 3);                        // at most 3 shots per session
    CHECK(!s5::hud_late_eye_request().load());                        // the detector itself never sets the flag
}

void test_option() {
    using namespace acz::terrain;
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && !c.hud_late);
    CHECK(parse_stage_option("m02a;hudlate:1", id, c, why) && c.hud_late && id == "m02a");
    CHECK(parse_stage_option("m02a;hudlate:0;hudrs:0", id, c, why) && !c.hud_late && !c.hud_rs_skip);
    CHECK(!parse_stage_option("m02a;hudlate:2", id, c, why));
}

// F6: the display-value marker clone and its gating.
void test_marker() {
    // Creation: only the text pipeline with the exact blend and a full write mask gets the clone.
    CHECK(s5::hud_late_clone_wanted(0x534f29cau, 0xd34ee153u, true, 5, 6, 0xF));
    CHECK(!s5::hud_late_clone_wanted(0x534f29cau, 0xd34ee153u, false, 5, 6, 0xF));       // no blend
    CHECK(!s5::hud_late_clone_wanted(0x534f29cau, 0xd34ee153u, true, 5, 2, 0xF));        // the gun tracer's SRC_ALPHA / ONE
    CHECK(!s5::hud_late_clone_wanted(0x534f29cau, 0xd34ee153u, true, 5, 6, 0x7));        // no alpha write
    CHECK(!s5::hud_late_clone_wanted(0x534f29cau, 0x4caf6e2du, true, 5, 6, 0xF));        // another pixel shader
    CHECK(!s5::hud_late_clone_wanted(0, 0, true, 5, 6, 0xF));
    // D3D12 numerics of the clone's alpha blend: ONE = 2, REV_SUBTRACT = 3 (a_out = a_dst - a_src).
    CHECK(s5::kLateCloneSrcAlpha == 2u && s5::kLateCloneDstAlpha == 2u && s5::kLateCloneOpAlpha == 3u);
    // Gating: the default consumer (clamp, exposure 1) never needs the clone; Reinhard or any other exposure does; a NaN exposure is treated as non-default.
    CHECK(!s5::hud_late_needs_marker(0, 1.0f));
    CHECK(s5::hud_late_needs_marker(1, 1.0f));
    CHECK(s5::hud_late_needs_marker(0, 0.8f));
    CHECK(s5::hud_late_needs_marker(0, 1.0001f));
    CHECK(s5::hud_late_needs_marker(0, std::nanf("")));
    CHECK(!s5::hud_late_marker_flag().load());                                            // default: the game's PSO is replayed
    // The marker reading: alpha < 0 only; coverage = saturate(-alpha).
    CHECK(s5::hud_late_is_display(-0.001f) && !s5::hud_late_is_display(0.0f) && !s5::hud_late_is_display(1.0f) && !s5::hud_late_is_display(std::nanf("")));
    CHECK(s5::hud_late_coverage(-0.25f) == 0.25f && s5::hud_late_coverage(-3.0f) == 1.0f);
}
} // namespace

int main() {
    test_text_class();
    test_decide();
    test_sums_and_line();
    test_stage_switch();
    test_onset();
    test_option();
    test_marker();
    std::printf("hud_late_offline: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
