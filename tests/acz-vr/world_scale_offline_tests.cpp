// world_scale_offline_tests.cpp -- WORLD-SCALE (docs/world-render/world-scale.md): one evidenced world head scale for every world layer.
// (1) xr_terrain_stage parsing: default 5.0 u/m, wscale:<f>, upm: x wscale, refusals; (2) perceived size of a known object (Draken 76.7 u, 15.35 m)
// triangulated through the terrain eyes with the real IPD: 15.3 m at the evidenced scale, 1.2 m at the legacy 65 u/m (the test-19 miniature);
// (3) the producer: world_scale.h publish/fallback and gpu_copy_step5 setup_poses accepting 5 u/m (it used to fall back to 39.6 below 20).
// Offline only: nothing here shows how the headset looks.
#include "terrain_stage.h"
#include "world_scale.h"
#include "gpu_copy_step5.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace acz::terrain;
namespace ws = acz::world_scale;
namespace s5 = acz::gpu_copy::step5;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static double half_baseline_units(double upm);
static void test_options() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && c.head_units_per_metre == ws::kWorldUnitsPerMetre && c.world_scale == 1.0f && !c.upm_set);
    CHECK(ws::kWorldUnitsPerMetre == 5.0f);
    // the day-install option set (props/interp/decouple/... are removed by the proxy before this parser): still the evidenced scale
    CHECK(parse_stage_option("auto;env:1;look:1;objdom:display", id, c, why) && id == "auto" && c.head_units_per_metre == 5.0f);
    CHECK(parse_stage_option("m02a;wscale:2", id, c, why) && c.world_scale == 2.0f && c.head_units_per_metre == 10.0f);
    CHECK(parse_stage_option("m02a;wscale=0.5", id, c, why) && c.head_units_per_metre == 2.5f);
    CHECK(parse_stage_option("m02a;wscale:0.25", id, c, why) && c.head_units_per_metre == 1.25f);
    CHECK(parse_stage_option("m02a;wscale:4", id, c, why) && c.head_units_per_metre == 20.0f);
    CHECK(parse_stage_option("m02a;upm:65;wscale:0.5", id, c, why) && c.upm_set && c.head_units_per_metre == 32.5f);   // explicit upm: x wscale
    CHECK(parse_stage_option("m02a;wscale:0.5;upm:65", id, c, why) && c.head_units_per_metre == 32.5f);                 // order-independent
    CHECK(parse_stage_option("m02a;upm:65", id, c, why) && c.head_units_per_metre == 65.0f);                            // upm: alone unchanged
    // re-parsing is idempotent (the proxy may re-run the parser; the scale must not compound)
    CHECK(parse_stage_option("m02a;upm:10;wscale:2", id, c, why) && parse_stage_option("m02a;upm:10;wscale:2", id, c, why) && c.head_units_per_metre == 20.0f);
    for (const char* bad : {"m02a;wscale:0", "m02a;wscale:0.2", "m02a;wscale:4.5", "m02a;wscale:-1", "m02a;wscale:x", "m02a;wscale:", "m02a;wscale:nan", "m02a;wscale:inf"}) {
        std::string i2, w2; Conventions c2; CHECK(!parse_stage_option(bad, i2, c2, w2) && !w2.empty());
    }
    CHECK(ws::world_head_units_per_metre(0.0f, 1.0f) == 5.0f && ws::world_head_units_per_metre(65.0f, 1.0f) == 65.0f && ws::world_head_units_per_metre(-1.0f, 2.0f) == 10.0f);
    // W3-2 (A2): the FINAL product (upm x wscale) is validated against the producer's range [1, 100] before it is accepted/published: below 1 and above 100 are
    // refused (terrain and replayed objects would otherwise use different scales); exactly 1 and 100 are accepted and BOTH consumers get the same value.
    for (const char* bad : {"m02a;upm:1;wscale:0.25", "m02a;upm:3;wscale:0.25", "m02a;upm:101", "m02a;upm:1000;wscale:4", "m02a;upm:26;wscale:4"}) {
        std::string i2, w2; Conventions c2; CHECK(!parse_stage_option(bad, i2, c2, w2) && !w2.empty());
    }
    for (const char* good : {"m02a;upm:1", "m02a;upm:100", "m02a;upm:25;wscale:4", "m02a;upm:4;wscale:0.25", "m02a;wscale:0.25;upm:4"}) {
        std::string i2, w2; Conventions c2; CHECK(parse_stage_option(good, i2, c2, w2));
        const float v = c2.head_units_per_metre; CHECK(v == 1.0f || v == 100.0f);
        ws::published_world_units_per_metre().store(v);
        CHECK(ws::producer_world_units_per_metre(65.0) == double(v) && std::fabs(half_baseline_units(double(v)) - 0.0315 * double(v)) < 1e-6);
        ws::published_world_units_per_metre().store(0.0f);
    }
    CHECK(!ws::final_scale_supported(0.0f) && !ws::final_scale_supported(0.99f) && ws::final_scale_supported(1.0f) && ws::final_scale_supported(100.0f) && !ws::final_scale_supported(100.01f));
}

// Perceived size: eyes from eye_world (head scale upm, real IPD 0.063 m), a horizontal object of length L units at distance Z straight ahead.
// The viewer triangulates with the REAL IPD: distance = (ipd/2)/tan(half disparity); size = 2 * distance * tan(half angular size).
static double perceived_size_m(float upm, double L_units, double Z_units) {
    const double ipd = 0.063;
    CamFrame f{}; f.eye[0] = 17110.4f; f.eye[1] = 1851.9f; f.eye[2] = 21311.9f;   // the test-19 world-objects camera (absolute world units)
    f.right[0] = 1; f.up[1] = 1; f.fwd[2] = -1;
    float e[2][3];
    const float oL[3] = {float(-ipd / 2), 0, 0}, oR[3] = {float(ipd / 2), 0, 0};
    eye_world(f, oL, upm, e[0]); eye_world(f, oR, upm, e[1]);
    const double cx = f.eye[0], cz = f.eye[2] - Z_units;   // object centre
    // azimuth (tangent, +right) of a world point seen from eye k, both eyes looking down -Z
    auto tan_az = [&](int k, double x) { return (x - double(e[k][0])) / (double(e[k][2]) - cz); };
    // disparity of the object centre: tan(aL) - tan(aR) ~ baseline/Z
    const double aL = std::atan(tan_az(0, cx)), aR = std::atan(tan_az(1, cx));
    const double half_disp = 0.5 * (aL - aR);
    const double dist_m = (ipd / 2) / std::tan(half_disp);
    // angular size from the cyclopean point (mid-eye)
    const double half_ang = std::atan((L_units / 2) / Z_units);
    return 2.0 * dist_m * std::tan(half_ang);
}
static void test_perceived_size() {
    const double draken_u = 76.70, draken_m = 15.35;   // model-decode.md: j35j exterior length vs the real J 35 Draken
    for (double Z : {50.0, 200.0, 1000.0, 5000.0}) {    // 10 m .. 1 km at 5 u/m
        const double at5 = perceived_size_m(5.0f, draken_u, Z), at65 = perceived_size_m(65.0f, draken_u, Z), at39 = perceived_size_m(39.6f, draken_u, Z);
        if (Z == 200.0) std::printf("perceived Draken length at %.0f u: 5 u/m %.2f m, 39.6 u/m %.2f m, 65 u/m %.2f m (real %.2f m)\n", Z, at5, at39, at65, draken_m);
        CHECK(std::fabs(at5 - draken_m) < 0.01 * draken_m);   // the evidenced scale reproduces the real size (5.00 u/m -> 15.34 m)
        CHECK(std::fabs(at65 - draken_u / 65.0) < 0.01);      // legacy: L/65 = 1.18 m: a 13x miniature
        CHECK(at65 < draken_m / 12.0 && at39 < draken_m / 7.0);
    }
    // wscale scales the perceived size inversely (2 -> half size, 0.5 -> double)
    CHECK(std::fabs(perceived_size_m(10.0f, draken_u, 300.0) - draken_m / 2) < 0.01 * draken_m);
    CHECK(std::fabs(perceived_size_m(2.5f, draken_u, 300.0) - draken_m * 2) < 0.02 * draken_m);
}

// Producer: setup_poses with a synthetic camera; the eye offset must be ipd/2 * scale for scales below the old 20 u/m floor.
static double half_baseline_units(double upm) {
    s5::PoseFrame f; s5::Neutral n; s5::Scratch sc;
    s5::Camera& c = f.cam; c.right[0] = 1; c.up[1] = 1; c.fwd[2] = -1; c.eye[0] = 17110.4; c.eye[1] = 1851.9; c.eye[2] = 21311.9;
    c.n0 = c.n1 = c.n3 = 1; c.A = 1; c.C = 0; c.tanX = 0.61517; c.tanY = 0.34603;
    acz::stereo::EyePose p[2]{};
    for (int k = 0; k < 2; ++k) { p[k].position[0] = k ? 0.0315f : -0.0315f; p[k].position[1] = 1.2f; p[k].orientation[3] = 1.0f; }
    if (s5::setup_poses(p, n, f, sc, false, upm)) return -1.0;
    double d = 0; for (int k = 0; k < 3; ++k) { const double x = f.eyeWorld[0][k] - c.eye[k]; d += x * x; }
    return std::sqrt(d);
}
static void test_producer() {
    CHECK(std::fabs(half_baseline_units(5.0) - 0.0315 * 5.0) < 1e-6);          // was 0.0315 * 39.6 (fallback below 20)
    CHECK(std::fabs(half_baseline_units(65.0) - 0.0315 * 65.0) < 1e-6);        // the replayed cockpit scale is unchanged
    CHECK(std::fabs(half_baseline_units(0.5) - 0.0315 * 39.6) < 1e-6);         // out of [1, 100]: the step3 default, as before
    CHECK(std::fabs(half_baseline_units(250.0) - 0.0315 * 39.6) < 1e-6);
    // publish/fallback: 0 (never published, no terrain option) keeps gpu_copy_step5_units_per_metre
    CHECK(ws::producer_world_units_per_metre(65.0) == 65.0);
    ws::published_world_units_per_metre().store(5.0f);
    CHECK(ws::producer_world_units_per_metre(65.0) == 5.0);
    ws::published_world_units_per_metre().store(250.0f);   // upm:65;wscale:4 = 260 is outside setup_poses' range: the configured value is used
    CHECK(ws::producer_world_units_per_metre(65.0) == 65.0);
    ws::published_world_units_per_metre().store(0.0f);
    CHECK(ws::producer_world_units_per_metre(39.6) == 39.6);
    // test-19 log: eye-offset-units=2.243 per eye at 65 u/m -> half IPD 0.0345 m; at 5 u/m the same head gives 0.1726 u
    CHECK(std::fabs(2.243 / 65.0 * 5.0 - 0.1725) < 1e-3);
}

int main() {
    test_options();
    test_perceived_size();
    test_producer();
    std::printf("world_scale_offline_tests: PASS (%u checks)\n", g_checks);
    return 0;
}
