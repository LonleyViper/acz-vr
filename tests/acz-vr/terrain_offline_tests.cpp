// terrain_offline_tests.cpp -- J-terrain-render offline tests: .zvrs loader errors, option/convention parsing, camera frame, heightfield sampling,
// the CPU ray march (vs a brute-force reference on a synthetic stage), determinism, the camera mailbox and the config keys/latch budget.
// No GPU: the shader-vs-CPU comparison is in xr_composite_warp_harness.cpp.
#include "terrain_stage.h"
#include "terrain_camera.h"
#include "runtime_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::terrain;
using acz::diagnostics::ConfigIssue;
using acz::diagnostics::parse_config;

static unsigned g_checks = 0;
static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t o) { return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24); }
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static StageData flat_stage(uint8_t h, bool sea = false, float sea_level = 0.0f) {
    StageData s = make_synthetic_stage(1, false);
    std::fill(s.heights.begin(), s.heights.end(), h);
    s.hmax_byte = h;
    if (sea) { s.flags = kFlagSeaPresent; s.sea_level = sea_level; }
    return s;
}
static void norm3(float v[3]) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int i = 0; i < 3; ++i) v[i] /= l; }

static void test_sha_and_roundtrip() {
    uint8_t h[32]; sha256("abc", 3, h);
    CHECK(hex(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("", 0, h);
    CHECK(hex(h, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::vector<uint8_t> big(1000, 0x61);
    sha256(big.data(), big.size(), h);
    CHECK(hex(h, 32) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

    StageData s = make_synthetic_stage(3, true);
    const auto bytes = serialize_stage(s);
    CHECK(serialize_stage(s) == bytes);                       // deterministic
    StageData t; CHECK(parse_stage(bytes.data(), bytes.size(), t) == LoadStatus::ok);
    CHECK(std::strcmp(t.id, "synth") == 0 && t.ntiles == 4 && t.heights == s.heights && t.words == s.words && t.tiles == s.tiles && t.sea_present() && t.sea_level == s.sea_level);
    CHECK(std::memcmp(t.clut, s.clut, 1024) == 0 && t.hmax_byte == s.hmax_byte && std::memcmp(t.source_sha, s.source_sha, 32) == 0);
    CHECK(serialize_stage(t) == bytes);                       // parse -> serialise is the identity
}
static void test_loader_errors() {
    const StageData s = make_synthetic_stage(2, false);
    const auto good = serialize_stage(s);
    StageData t;
    auto expect = [&](std::vector<uint8_t> b, LoadStatus want) { StageData o; const LoadStatus st = parse_stage(b.data(), b.size(), o); CHECK(st == want); CHECK(o.heights.empty() && o.ntiles == 0); };
    expect({}, LoadStatus::too_small);
    expect(std::vector<uint8_t>(good.begin(), good.begin() + 100), LoadStatus::too_small);
    { auto b = good; b[0] = 'X'; expect(b, LoadStatus::bad_magic); }
    { auto b = good; b[4] = 9; expect(b, LoadStatus::bad_version); }
    { auto b = good; b[4] = 0; expect(b, LoadStatus::bad_version); }
    { auto b = good; b.pop_back(); expect(b, LoadStatus::bad_size); }
    { auto b = good; b.push_back(0); expect(b, LoadStatus::bad_size); }
    { auto b = good; b[kZvrsHeader + 1000] ^= 1; expect(b, LoadStatus::bad_hash); }             // payload corruption
    { auto b = good; b[192] ^= 1; expect(b, LoadStatus::bad_hash); }                            // stored hash corruption
    { auto b = good; b[20] = 7; expect(b, LoadStatus::bad_header); }                            // hdim
    { auto b = good; b[28] = 0; b[29] = 0; expect(b, LoadStatus::bad_header); }                 // zero tiles
    { auto b = good; b[29] = 0x10; expect(b, LoadStatus::bad_header); }                         // > kMaxTiles
    { auto b = good; b[56] = 3; expect(b, LoadStatus::bad_size); }                              // unaligned section offset
    { auto b = good; b[8] = '/'; expect(b, LoadStatus::bad_header); }                           // bad stage id
    { StageData bad = s; bad.words[5] = 99; auto b = serialize_stage(bad); expect(b, LoadStatus::bad_content); }   // tile id >= ntiles with a VALID hash
    { StageData bad = s; bad.cell = 0.0f; auto b = serialize_stage(bad); expect(b, LoadStatus::bad_header); }
    std::vector<uint8_t> huge(kMaxFileBytes + 1, 0);
    { StageData o; CHECK(parse_stage(huge.data(), huge.size(), o) == LoadStatus::too_large); }
    // file level: missing file, empty path, directory-ish, a good file, a truncated file
    CHECK(load_stage_file("Z:\\definitely\\missing\\none.zvrs", t) == LoadStatus::io_error);
    CHECK(load_stage_file("", t) == LoadStatus::io_error);
    const char* tmp = std::getenv("TEMP"); std::string dir = tmp ? tmp : ".";
    const std::string p = dir + "\\acz_terrain_test.zvrs";
    { FILE* f = std::fopen(p.c_str(), "wb"); CHECK(f); std::fwrite(good.data(), 1, good.size(), f); std::fclose(f); }
    CHECK(load_stage_file(p, t) == LoadStatus::ok && t.ntiles == 4);
    { FILE* f = std::fopen(p.c_str(), "wb"); CHECK(f); std::fwrite(good.data(), 1, good.size() / 2, f); std::fclose(f); }
    { StageData o; CHECK(load_stage_file(p, o) == LoadStatus::bad_size && o.heights.empty()); }
    std::remove(p.c_str());
    CHECK(valid_stage_id("m02a") && valid_stage_id("m16n") && !valid_stage_id("") && !valid_stage_id("toolongid") && !valid_stage_id("a/b") && !valid_stage_id("a b") && !valid_stage_id("..\\x"));
    CHECK(stage_path("../x", "C:\\d").empty() && stage_path("m02a", "C:\\d") == "C:\\d\\m02a.zvrs");
    for (int k = 0; k <= int(LoadStatus::bad_content); ++k) CHECK(std::strlen(status_name(LoadStatus(k))) > 0);
}
// SKIN-REFL: the <stage>_ref.zvrt sidecar (format, refusals, path, optional load next to the stage; never fails the stage).
static void test_refl_sidecar() {
    std::vector<uint8_t> px(32 * 32 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7 + 3);
    const auto b = serialize_refl_map(32, 32, px);
    CHECK(b.size() == kReflMapHeader + px.size() && std::memcmp(b.data(), "ZVRT", 4) == 0);
    { uint32_t w = 0, h = 0; std::vector<uint8_t> o; CHECK(parse_refl_map(b.data(), b.size(), w, h, o) == LoadStatus::ok && w == 32 && h == 32 && o == px); }
    auto expect = [&](std::vector<uint8_t> c, LoadStatus want) {
        uint32_t w = 9, h = 9; std::vector<uint8_t> o{1};
        CHECK(parse_refl_map(c.data(), c.size(), w, h, o) == want && w == 9 && h == 9 && o.size() == 1);   // outputs untouched on refusal
    };
    { auto c = b; c[0] = 'X'; expect(c, LoadStatus::bad_magic); }
    { auto c = b; c[4] = 2; expect(c, LoadStatus::bad_version); }
    { auto c = b; c[8] = 0; c[9] = 0; expect(c, LoadStatus::bad_header); }                // w 0
    { auto c = b; c[8] = 65; expect(c, LoadStatus::bad_header); }                         // w 65 > max
    { auto c = b; c[12] = 2; expect(c, LoadStatus::bad_header); }                         // kind
    { auto c = b; c.pop_back(); expect(c, LoadStatus::bad_size); }
    { auto c = b; c.push_back(0); expect(c, LoadStatus::bad_size); }
    { auto c = b; c[kReflMapHeader + 5] ^= 1; expect(c, LoadStatus::bad_hash); }
    { std::vector<uint8_t> c(b.begin(), b.begin() + 20); expect(c, LoadStatus::too_small); }
    CHECK(refl_map_path("C:\\d\\m02a.zvrs") == "C:\\d\\m02a_ref.zvrt" && refl_map_path("C:\\d\\M02A.ZVRS") == "C:\\d\\M02A_ref.zvrt" && refl_map_path("x") == "x_ref.zvrt");
    // next to a stage file
    const char* tmp = std::getenv("TEMP"); std::string dir = tmp ? tmp : ".";
    const std::string p = dir + "\\acz_refl_test.zvrs", q = refl_map_path(p);
    const auto stage = serialize_stage(make_synthetic_stage(2, false));
    { FILE* f = std::fopen(p.c_str(), "wb"); CHECK(f); std::fwrite(stage.data(), 1, stage.size(), f); std::fclose(f); }
    std::remove(q.c_str());
    { StageData t; CHECK(load_stage_file(p, t) == LoadStatus::ok && !t.has_refl() && t.refl_status == LoadStatus::io_error); }   // absent: stage fine, no map
    { FILE* f = std::fopen(q.c_str(), "wb"); CHECK(f); std::fwrite(b.data(), 1, b.size(), f); std::fclose(f); }
    { StageData t; CHECK(load_stage_file(p, t) == LoadStatus::ok && t.has_refl() && t.refl_w == 32 && t.refl_rgba == px && t.refl_status == LoadStatus::ok); }
    { auto c = b; c.back() ^= 1; FILE* f = std::fopen(q.c_str(), "wb"); CHECK(f); std::fwrite(c.data(), 1, c.size(), f); std::fclose(f); }
    { StageData t; CHECK(load_stage_file(p, t) == LoadStatus::ok && !t.has_refl() && t.refl_status == LoadStatus::bad_hash && t.ntiles == 4); }   // bad map: stage fine
    std::remove(q.c_str()); std::remove(p.c_str());
    // the stage format is unchanged: parse -> serialise is still the identity (the map is not serialised)
    { StageData t; CHECK(parse_stage(stage.data(), stage.size(), t) == LoadStatus::ok && serialize_stage(t) == stage && !t.has_refl()); }
}
static void test_options() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && id == "m02a" && !c.mirror_x && c.yaw_offset_deg == 0 && !c.blend_window && !c.fixed_cam && c.haze_l == 55000.0f && c.far_dist == 160000.0f);
    CHECK(parse_stage_option("m02a;yaw=12.5;mirror=1;blend=1;sgn=-1;alt=-30;upm=25;fogl=30000;far=90000;sea=off;fixy=4000", id, c, why));
    CHECK(c.yaw_offset_deg == 12.5f && c.mirror_x && c.blend_window && c.yaw_sign == -1 && c.alt_offset == -30 && c.head_units_per_metre == 25 && c.haze_l == 30000 && c.far_dist == 90000 && c.sea_off && c.fixed_alt);
    CHECK(parse_stage_option("m16n;cam=1,2,3,90;sea=-5", id, c, why) && c.fixed_cam && c.cam[0] == 1 && c.cam[1] == 2 && c.cam[2] == 3 && c.cam[3] == 90 && c.sea_override_set && c.sea_override == -5);
    for (const char* bad : {"", ";", "m02a;", "m02a;yaw", "m02a;yaw=", "m02a;yaw=abc", "m02a;nope=1", "m02a;mirror=2", "m02a;sgn=2", "m02a;fog0=5000", "m02a;far=500", "m02a;fogl=10", "m02a;far=1e6", "toolongstage", "m02a;cam=1,2,3", "m02a;cam=1,2,3,4,5,6,7",
                            "a/b", "m02a;;yaw=1", "m02a;upm=0", "m02a;yaw=1e99", "m02a;cam=1,2,3,4,90", "m02a;cam=1,2,3,4,0,181", "m02a;objdom:yes", "m02a;cam=1,2,3,4,"}) {
        std::string i2, w2; Conventions c2; CHECK(!parse_stage_option(bad, i2, c2, w2) && !w2.empty());
    }
    // PROPS-SHADING: cam with optional pitch/roll (4..6 values; 4 = the old level camera, pitch/roll 0), objdom:display|raw (default raw)
    CHECK(parse_stage_option("m02a;cam=1,2,3,90", id, c, why) && c.cam[4] == 0 && c.cam[5] == 0 && !c.obj_display);
    CHECK(parse_stage_option("m02a;cam:14500,900,14500,45,-35", id, c, why) && c.fixed_cam && c.cam[3] == 45 && c.cam[4] == -35 && c.cam[5] == 0);
    CHECK(parse_stage_option("m02a;cam:1,2,3,4,-89,180;objdom:display", id, c, why) && c.cam[4] == -89 && c.cam[5] == 180 && c.obj_display);
    CHECK(parse_stage_option("m02a;objdom:raw", id, c, why) && !c.obj_display);
    {   // a pitched fixed camera looks down along the same convention as an E6 sample (camera_frame): forward y = sin(pitch)
        CamFrame f{}; const float p0[3] = {0, 1000, 0};
        CHECK(camera_frame(p0, -35.0f * 3.14159265f / 180.0f, 0.0f, 0.0f, Conventions{}, f) && std::fabs(f.fwd[1] - std::sin(-35.0f * 3.14159265f / 180.0f)) < 1e-6f);
    }
    // WORLD-1 options: camera source, E6 sample, game-camera tolerance, object occlusion, explicit upm
    CHECK(parse_stage_option("m02a", id, c, why) && c.cam_source_game && c.e6_frame && c.game_cam_max_deg == 15.0f && c.occlude_objects && !c.upm_set);
    CHECK(parse_stage_option("m02a;csrc:e6;e6:latest;gmax:5;occl:0;upm:65", id, c, why) && !c.cam_source_game && !c.e6_frame && c.game_cam_max_deg == 5.0f && !c.occlude_objects && c.upm_set && c.head_units_per_metre == 65.0f);
    for (const char* bad : {"m02a;csrc:x", "m02a;e6:now", "m02a;gmax:0", "m02a;gmax:200", "m02a;occl:2"}) { std::string i2, w2; Conventions c2; CHECK(!parse_stage_option(bad, i2, c2, w2)); }
    { // camera_frame_axes: accepts a right-handed orthonormal basis, refuses a mirrored or non-orthonormal one; frame_angle_deg measures the rotation
        CamFrame e{}; const float p[3] = {1, 2, 3}; CHECK(camera_frame(p, 0.2f, -0.7f, 0.4f, Conventions{}, e));
        CamFrame g{}; CHECK(camera_frame_axes(p, e.right, e.up, e.fwd, Conventions{}, g) && frame_angle_deg(e, g) < 1e-3f && g.eye[1] == 2.0f);
        const float mr[3] = {-e.right[0], -e.right[1], -e.right[2]}; CHECK(!camera_frame_axes(p, mr, e.up, e.fwd, Conventions{}, g));
        const float sk[3] = {e.right[0] + 0.1f, e.right[1], e.right[2]}; CHECK(!camera_frame_axes(p, sk, e.up, e.fwd, Conventions{}, g));
        CamFrame r{}; camera_frame(p, 0.2f, -0.7f + 0.1f, 0.4f, Conventions{}, r); CHECK(std::fabs(frame_angle_deg(e, r) - 5.73f) < 0.3f);
    }
    StageData s = flat_stage(1, true, 100.0f);
    Conventions off; off.sea_off = true; apply_sea_convention(s, off); CHECK(!s.sea_present());
    Conventions set; set.sea_override_set = true; set.sea_override = 55.0f; apply_sea_convention(s, set); CHECK(s.sea_present() && s.sea_level == 55.0f && (s.flags & kFlagSeaGuess));
}
static void test_camera() {
    Conventions c; CamFrame f{};
    const float pos[3] = {100, 4000, -200};
    CHECK(camera_frame(pos, 0, -0.78539816f, 0, c, f));   // heading -45 deg: forward NE = (+0.707, 0, -0.707) (calibration doc)
    CHECK(std::fabs(f.fwd[0] - 0.70710678f) < 1e-6f && std::fabs(f.fwd[1]) < 1e-6f && std::fabs(f.fwd[2] + 0.70710678f) < 1e-6f);
    CHECK(std::fabs(f.right[0] - 0.70710678f) < 1e-6f && std::fabs(f.right[2] - 0.70710678f) < 1e-6f && std::fabs(f.up[1] - 1.0f) < 1e-6f);
    CHECK(f.eye[0] == 100 && f.eye[1] == 4000 && f.eye[2] == -200);
    camera_frame(pos, 0, 0, 0, c, f);                      // heading 0: forward -Z (north), right +X (east), up +Y
    CHECK(std::fabs(f.fwd[2] + 1) < 1e-6f && std::fabs(f.right[0] - 1) < 1e-6f && std::fabs(f.up[1] - 1) < 1e-6f);
    camera_frame(pos, 0, 1.5707963f, 0, c, f);             // positive yaw turns toward -X
    CHECK(std::fabs(f.fwd[0] + 1) < 1e-5f);
    Conventions m = c; m.mirror_x = true; camera_frame(pos, 0, 0, 0, m, f); CHECK(std::fabs(f.right[0] + 1) < 1e-6f && std::fabs(f.fwd[2] + 1) < 1e-6f);
    Conventions o = c; o.yaw_offset_deg = 90; camera_frame(pos, 0, 0, 0, o, f); CHECK(std::fabs(f.fwd[0] + 1) < 1e-5f);
    Conventions sg = c; sg.yaw_sign = -1; camera_frame(pos, 0, 1.5707963f, 0, sg, f); CHECK(std::fabs(f.fwd[0] - 1) < 1e-5f);
    camera_frame(pos, 0.3f, 0, 0, c, f); CHECK(f.fwd[1] > 0.29f && f.up[2] > 0.29f);              // nose up tilts forward up, up leans back (+Z is behind a -Z forward)
    camera_frame(pos, 0, 0, 0.5f, c, f);                                                          // roll: still orthonormal, forward unchanged
    CHECK(std::fabs(f.fwd[2] + 1) < 1e-6f && std::fabs(f.right[0] * f.up[0] + f.right[1] * f.up[1] + f.right[2] * f.up[2]) < 1e-6f && std::fabs(f.up[1] - std::cos(0.5f)) < 1e-6f);
    Conventions fy = c; fy.fixed_alt = true; fy.fixed_alt_value = 4000; fy.alt_offset = 10; const float p2[3] = {0, 123, 0}; camera_frame(p2, 0, 0, 0, fy, f); CHECK(f.eye[1] == 4010);
    const float bad[3] = {NAN, 0, 0}; CHECK(!camera_frame(bad, 0, 0, 0, c, f) && !camera_frame(pos, INFINITY, 0, 0, c, f) && !camera_frame(pos, 0, NAN, 0, c, f));
    for (int k = 0; k < 50; ++k) {     // orthonormal for arbitrary angles
        const float pit = -1.4f + 0.057f * k, yw = -3.1f + 0.13f * k, rl = 0.02f * k;
        camera_frame(pos, pit, yw, rl, c, f);
        auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        CHECK(std::fabs(dot(f.right, f.right) - 1) < 1e-5f && std::fabs(dot(f.up, f.up) - 1) < 1e-5f && std::fabs(dot(f.fwd, f.fwd) - 1) < 1e-5f);
        CHECK(std::fabs(dot(f.right, f.up)) < 1e-5f && std::fabs(dot(f.right, f.fwd)) < 1e-5f && std::fabs(dot(f.up, f.fwd)) < 1e-5f);
    }
    camera_frame(pos, 0, 0, 0, c, f);
    const float o0[3] = {0, 0, 0}, o1[3] = {1, 0, 0}, o2[3] = {0, 0, -1}; float w[3];
    eye_world(f, o0, 39.6f, w); CHECK(w[0] == 100 && w[1] == 4000 && w[2] == -200);
    eye_world(f, o1, 39.6f, w); CHECK(std::fabs(w[0] - (100 + 39.6f)) < 1e-4f);                    // +x metres = along the camera right
    eye_world(f, o2, 10.0f, w); CHECK(std::fabs(w[2] - (-200 - 10.0f)) < 1e-4f);                   // anchor -z (forward) = along the camera forward
}
// WORLD-1 step 1: the roll sign. The game's cockpit camera sits at campos = air + F*fwd + K*up_actual (F ~36.3, K ~3.57 units), so the logged
// offset campos - air measures the aircraft's real up axis. Rows = (campos - air xyz, E6 pitch, heading rot[1], roll), copied from headset test 16
// (D:\acz-evidence-archive\headset-test16\ee-camera.log, timeline slot 2, heading == H rows; t = the log's tick).
static void test_roll_sign() {
    CHECK(Conventions{}.roll_sign == -1.0f && Conventions{}.pitch_sign == 1.0f && Conventions{}.yaw_sign == 1.0f);
    struct Row { float off[3], pitch, heading, roll; };
    static const Row rows[] = {
        {{21.2100f, 4.5710f, -29.3000f}, 0.070947f, -0.708023f, 0.976515f},      // t=197177793.925
        {{24.8400f, -0.2240f, -26.7000f}, -0.089972f, -0.697735f, -0.548466f},   // t=197180932.824
        {{11.5410f, -9.2650f, -33.3400f}, -0.267659f, -0.434284f, 1.473450f},    // t=197223592.440
        {{29.2400f, -7.4170f, -20.5000f}, -0.189314f, -0.860278f, -1.738260f},   // t=197189786.581
        {{17.2000f, -6.1720f, -31.5000f}, -0.081375f, -0.541659f, 2.699740f},    // t=197187957.454
        {{19.3800f, -7.7860f, -29.9000f}, -0.118526f, -0.556928f, -2.962030f},   // t=197188210.866
        {{25.5700f, -13.3440f, -22.3000f}, -0.276803f, -0.862896f, 3.047580f},   // t=197190293.678
        {{24.7600f, 10.2130f, -24.7000f}, 0.185606f, -0.785393f, 0.000001f},     // t=197175444.217
        {{23.1650f, -0.2680f, -28.1800f}, -0.082042f, -0.751905f, 0.707891f},    // t=197220508.599
    };
    const float zero[3] = {0, 0, 0};
    double worst = 0, worst_wrong = 1e9;
    for (const Row& r : rows) {
        CamFrame f{}; CHECK(camera_frame(zero, r.pitch, r.heading, r.roll, Conventions{}, f));
        float u[3]; for (int i = 0; i < 3; ++i) u[i] = r.off[i] - 36.3f * f.fwd[i];
        const float k = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        CHECK(k > 3.3f && k < 3.9f);                                   // |K up| = 3.57
        norm3(u);
        double e = 0; for (int i = 0; i < 3; ++i) e = std::max(e, double(std::fabs(u[i] - f.up[i])));
        worst = std::max(worst, e);
        CHECK(e < 0.04);                                                // measured up axis == camera_frame up (default conventions)
        Conventions plus; plus.roll_sign = 1.0f; CamFrame g{}; camera_frame(zero, r.pitch, r.heading, r.roll, plus, g);
        if (std::fabs(std::sin(r.roll)) > 0.3f) { double e2 = 0; for (int i = 0; i < 3; ++i) e2 = std::max(e2, double(std::fabs(u[i] - g.up[i]))); worst_wrong = std::min(worst_wrong, e2); }
    }
    std::printf("  roll sign: worst up-axis error %.4f (default -1), best with +1: %.3f\n", worst, worst_wrong);
    CHECK(worst_wrong > 0.4);                                           // the old +1 convention is clearly refuted by the same rows
    // hand-derived sign case: roll +0.5, pitch 0, heading 0 (forward -Z = north): a LEFT bank, the up axis leans to -X (west = left of north),
    // the right axis tilts upward (+Y); roll -0.5 the opposite
    CamFrame f{}; CHECK(camera_frame(zero, 0, 0, 0.5f, Conventions{}, f));
    CHECK(f.up[0] < -0.4f && f.right[1] > 0.4f && std::fabs(f.up[0] + std::sin(0.5f)) < 1e-5f && std::fabs(f.right[1] - std::sin(0.5f)) < 1e-5f);
    CHECK(camera_frame(zero, 0, 0, -0.5f, Conventions{}, f) && f.up[0] > 0.4f && f.right[1] < -0.4f);
    // rsgn stays an override: rsgn:1 restores the old sense, rsgn:-1 is the default
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && c.roll_sign == -1.0f);
    CHECK(parse_stage_option("m02a;rsgn:1", id, c, why) && c.roll_sign == 1.0f && camera_frame(zero, 0, 0, 0.5f, c, f) && f.up[0] > 0.4f);
    CHECK(parse_stage_option("m02a;rsgn:-1", id, c, why) && c.roll_sign == -1.0f);
}
static void test_sampling() {
    StageData s = make_synthetic_stage(5, false);
    // exact at grid nodes
    for (int k = 0; k < 40; ++k) {
        const int i = (k * 37) % 513, j = (k * 91) % 513;
        const float x = -s.half + float(i) * s.cell, z = -s.half + float(j) * s.cell;
        CHECK(std::fabs(height_bilinear(s, x, z) - float(s.heights[size_t(j) * 513 + i]) * s.hstep) < 1e-2f);
    }
    // midpoint of a cell = mean of its four corners
    { const int i = 100, j = 200; const float x = -s.half + (float(i) + 0.5f) * s.cell, z = -s.half + (float(j) + 0.5f) * s.cell;
      const float m = 0.25f * (s.heights[size_t(j) * 513 + i] + s.heights[size_t(j) * 513 + i + 1] + s.heights[size_t(j + 1) * 513 + i] + s.heights[size_t(j + 1) * 513 + i + 1]) * s.hstep;
      CHECK(std::fabs(height_bilinear(s, x, z) - m) < 0.05f); }
    // clamped outside
    CHECK(height_bilinear(s, -1e9f, -1e9f) == float(s.heights[0]) * s.hstep && height_bilinear(s, 1e9f, 1e9f) == float(s.heights[513 * 513 - 1]) * s.hstep);
    // +X is the column axis, +Z the row axis
    StageData t = flat_stage(0); t.heights[size_t(10) * 513 + 20] = 50;
    CHECK(height_bilinear(t, -t.half + 20 * t.cell, -t.half + 10 * t.cell) == 50 * t.hstep);
    CHECK(height_bilinear(t, -t.half + 10 * t.cell, -t.half + 20 * t.cell) == 0.0f);
    // sea plane raises low ground only
    StageData sea = flat_stage(1, true, 200.0f); CHECK(height_effective(sea, 0, 0) == 200.0f);
    sea.heights[size_t(256) * 513 + 256] = 40; CHECK(height_effective(sea, 0, 0) == 40 * sea.hstep);
    // atlas: lod 0 returns the CLUT colour of the texel; mips average
    Atlas a; build_atlas(s, a);
    float c[4]; atlas_sample(a, 1, 0.5f / 64, 0.5f / 64, 0, c);
    const uint8_t idx = s.tiles[4096];
    const uint8_t ent = clut_entry(idx);   // K00A-SNOW: the CSM1-ordered entry
    CHECK(std::fabs(c[0] - s.clut[ent * 4] / 255.0f) < 1e-6f && std::fabs(c[1] - s.clut[ent * 4 + 1] / 255.0f) < 1e-6f);
    float c6[4]; atlas_sample(a, 1, 0.3f, 0.7f, 6, c6); CHECK(c6[0] >= 0 && c6[0] <= 1);
    CHECK(a.level[6].size() == size_t(s.ntiles) * 4 && a.level[3].size() == size_t(s.ntiles) * 8 * 8 * 4);
}
static void test_trace() {
    const float fog0 = 55000, fog1 = 58000;   // WORLD-1: (haze length L, march end far)
    { // flat ground at 10 bytes = 800 units; ray straight down from 1800 hits at t = 1000
        StageData s = flat_stage(10); Atlas a; build_atlas(s, a);
        const float o[3] = {0, 1800, 0}, d[3] = {0, -1, 0};
        Trace r = trace(s, a, o, d, fog0, fog1);
        CHECK(r.hit && !r.water && std::fabs(r.t - 1000.0f) < 1.0f && std::fabs(r.p[1] - 800.0f) < 1.0f);
        // flat ground: gradient 0 -> light = ambient + diffuse * sun.y/|sun|
        float sun[3] = {kSun[0], kSun[1], kSun[2]}; norm3(sun);
        float tex[4]; atlas_sample(a, s.words[size_t(128) * 256 + 128] & 0x3ff, 0.0f, 0.0f, 0, tex);       // tile uv irrelevant for the lum bound check below
        const float shade = kAmbient + kDiffuse * sun[1];
        for (int k = 0; k < 3; ++k) CHECK(r.rgb[k] >= 0 && r.rgb[k] <= shade + 1e-3f);
        // up: sky
        const float up[3] = {0, 1, 0}; Trace u = trace(s, a, o, up, fog0, fog1); CHECK(u.sky && !u.hit && u.steps == 0 + u.steps);
        float sk[3]; sky_color(up, sk); for (int k = 0; k < 3; ++k) CHECK(std::fabs(u.display[k] - sk[k]) < 1e-6f && std::fabs(u.rgb[k] - srgb_to_linear(sk[k])) < 1e-6f);
        // looking slightly down from 1000 units over flat ground: hit distance = h / sin(angle)
        float d2[3] = {0.0f, -0.1f, -1.0f}; norm3(d2);
        Trace g = trace(s, a, o, d2, fog0, fog1); CHECK(g.hit && std::fabs(g.t - 1000.0f / (-d2[1])) < 3.0f);
        // far horizon: fog colour (smoothstep reaches 1 at fog1) -> sky horizon
        float d3[3] = {0.0f, -0.0001f, -1.0f}; norm3(d3);
        Trace h = trace(s, a, o, d3, fog0, fog1);
        CHECK(h.hit ? (h.t >= fog1 * 0.99f) : h.sky);
        // eye underground: immediate hit
        const float ug[3] = {0, 100, 0}; Trace q = trace(s, a, ug, d, fog0, fog1); CHECK(q.hit && q.t < 1.0f);
        // determinism: bit-identical repeat
        Trace r2 = trace(s, a, o, d, fog0, fog1); CHECK(std::memcmp(&r, &r2, sizeof r) == 0);
    }
    { // water: ground below the sea level renders as the plane, hit at the plane height
        StageData s = flat_stage(2, true, 400.0f); Atlas a; build_atlas(s, a);
        const float o[3] = {0, 1400, 0}, d[3] = {0, -1, 0};
        Trace r = trace(s, a, o, d, 20000, 58000);
        CHECK(r.hit && r.water && std::fabs(r.p[1] - 400.0f) < 1.0f);
        StageData dry = s; dry.flags = 0; Trace r2 = trace(dry, a, o, d, 20000, 58000); CHECK(r2.hit && !r2.water && std::fabs(r2.p[1] - 160.0f) < 1.0f);
    }
    { // rolling synthetic terrain: adaptive march vs a brute-force fine march
        StageData s = make_synthetic_stage(4, false); Atlas a; build_atlas(s, a);
        unsigned n = 0, agree = 0, hits = 0; double worst = 0;
        for (int iy = 0; iy < 24; ++iy)
            for (int ix = 0; ix < 24; ++ix) {
                const float o[3] = {-40000.0f + 3000.0f * ix, 6000.0f + 20.0f * iy, 80000.0f - 3500.0f * iy};
                float d[3] = {0.35f + 0.02f * ix - 0.2f, -0.12f - 0.012f * iy, -0.9f}; norm3(d);
                Trace r = trace(s, a, o, d, 20000, 58000);
                // brute force: first t where the ray is below the terrain
                float tb = -1;
                for (float t = 0; t < 58000.0f; t += 4.0f) { const float x = o[0] + d[0] * t, y = o[1] + d[1] * t, z = o[2] + d[2] * t; if (y < height_effective(s, x, z)) { tb = t; break; } }
                ++n;
                if (tb < 0) { if (!r.hit || r.t >= 57000.0f) ++agree; continue; }
                ++hits;
                const double e = r.hit ? std::fabs(double(r.t) - tb) : 1e9;
                worst = std::max(worst, e);
                if (e < 12.0) ++agree;
            }
        std::printf("  march vs brute force: %u rays, %u hit, %u agree, worst |dt| = %.1f units\n", n, hits, agree, worst);
        CHECK(hits > 100 && agree * 100 >= n * 98);
    }
    { // pixel-level determinism over a small grid of rays (same inputs, same bytes) and a different eye changes the result
        StageData s = make_synthetic_stage(2, false); Atlas a; build_atlas(s, a);
        std::vector<float> img1, img2;
        for (int pass = 0; pass < 2; ++pass)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    const float o[3] = {1000, 7000, 2000}; float d[3] = {(x - 8) * 0.05f, -0.2f - y * 0.03f, -1.0f}; norm3(d);
                    Trace r = trace(s, a, o, d, 20000, 58000);
                    auto& im = pass ? img2 : img1; im.push_back(r.rgb[0]); im.push_back(r.rgb[1]); im.push_back(r.rgb[2]);
                }
        CHECK(img1.size() == img2.size() && std::memcmp(img1.data(), img2.data(), img1.size() * 4) == 0);
    }
}
// WORLD-1 step 2: colour domain, sky elevation table, exponential haze, no seam at the march end.
static void dir_at(float depression_deg, float az_deg, float d[3]) {   // depression > 0 = below the horizon; az 0 = north (-Z)
    const float e = -depression_deg * 3.14159265f / 180.0f, a = az_deg * 3.14159265f / 180.0f;
    d[0] = std::cos(e) * std::sin(a); d[1] = std::sin(e); d[2] = -std::cos(e) * std::cos(a);
}
static void test_colour_haze() {
    // sRGB transfer: exact round trip, the standard value at 0.5, endpoints
    for (int i = 0; i <= 255; ++i) { const float x = i / 255.0f; CHECK(std::fabs(linear_to_srgb(srgb_to_linear(x)) - x) < 2e-5f); }
    CHECK(std::fabs(srgb_to_linear(0.5f) - 0.21404f) < 1e-4f && srgb_to_linear(0) == 0 && std::fabs(srgb_to_linear(1) - 1) < 1e-6f);
    // sky table: every measured elevation reproduces the game's display value (what the sRGB RTV shows = linear_to_srgb(trace.rgb)) within 0.5/255;
    // below the horizon the sky is the haze colour; the gradient darkens monotonically with elevation
    StageData flat = flat_stage(5); Atlas fa; build_atlas(flat, fa);
    const float eye[3] = {0, 3900, 0};                                       // 3500 units above the flat ground (the step56 shot: ~3500 AGL)
    float prev_r = 2;
    for (unsigned k = 0; k < kSkyRows; ++k) {
        float d[3]; dir_at(-kSkyElevDeg[k], 30.0f, d);
        const Trace r = trace(flat, fa, eye, d, 55000, 160000);
        CHECK(r.sky && !r.hit);
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(linear_to_srgb(r.rgb[c]) * 255.0f - kSkyTable[k][c] * 255.0f) < 0.5f);
        CHECK(r.display[0] <= prev_r); prev_r = r.display[0];
    }
    CHECK(std::fabs(kSkyTable[0][0] * 255.0f - 130.0f) < 1e-3f && std::fabs(kSkyTable[0][1] * 255.0f - 148.0f) < 1e-3f && std::fabs(kSkyTable[0][2] * 255.0f - 141.0f) < 1e-3f);
    { float d[3] = {0, -1, 0}, s[3]; sky_color(d, s); for (int c = 0; c < 3; ++c) CHECK(s[c] == kHaze[c]); }
    { float d[3] = {0, 1, 0}, s[3]; sky_color(d, s); for (int c = 0; c < 3; ++c) CHECK(s[c] == kSkyTable[kSkyRows - 1][c]); }
    // haze: f = 1 - exp(-t/L) toward the haze colour IN THE DISPLAY DOMAIN (how the game's fit was measured), at three depressions
    for (const float dep : {2.9f, 5.8f, 15.0f}) {
        float d[3]; dir_at(dep, 30.0f, d);
        const Trace clear = trace(flat, fa, eye, d, 1e12f, 1e7f), hz = trace(flat, fa, eye, d, 55000, 160000);
        CHECK(clear.hit && hz.hit && clear.t == hz.t);
        const float f = haze_fraction(hz.t, 55000, 160000), f0 = 1.0f - std::exp(-hz.t / 55000.0f);
        CHECK(std::fabs(f - f0) < 1e-6f);                                    // inside 0.8 far the fade term is exactly 0
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(hz.display[c] - (clear.display[c] + (kHaze[c] - clear.display[c]) * f)) < 0.01f);
    }
    // the model against the game's measured haze fractions (terrain-issues-test16.md section 4: distance -> f)
    const float meas[5][2] = {{133000, 0.89f}, {91000, 0.80f}, {68000, 0.72f}, {45000, 0.61f}, {34000, 0.53f}};
    for (const auto& m : meas) CHECK(std::fabs(haze_fraction(m[0], 55000, 160000) - m[1]) < 0.08f);
    // no seam at the march end: terrain hit just inside `far` vs a ray that misses (passes far) differ by < 2/255 on screen; both are the haze colour
    {
        float dn[3], dm[3]; dir_at(std::asin(3500.0f / 158400.0f) * 57.2957795f, 30.0f, dn); dir_at(1.20f, 30.0f, dm);
        const Trace near_end = trace(flat, fa, eye, dn, 55000, 160000), missed = trace(flat, fa, eye, dm, 55000, 160000);
        CHECK(near_end.hit && near_end.t > 150000.0f && near_end.t < 160000.0f && !missed.hit && missed.sky);
        for (int c = 0; c < 3; ++c) {
            CHECK(std::fabs(linear_to_srgb(near_end.rgb[c]) - linear_to_srgb(missed.rgb[c])) * 255.0f < 2.0f);
            CHECK(std::fabs(missed.display[c] - kHaze[c]) < 1e-6f);
        }
        // and the old flat band is gone: a ray just above the horizon is the haze colour too (continuous across the horizon)
        float du[3]; dir_at(-0.05f, 30.0f, du); const Trace up = trace(flat, fa, eye, du, 55000, 160000);
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(up.display[c] - kHaze[c]) * 255.0f < 0.5f);
    }
    // a column from the ground up to the sky has no jump larger than the texture contrast: sample depressions 0.2 .. 1.8 deg (the old 58 km band edge was ~3.4 deg)
    {
        float last[3] = {0, 0, 0}; bool have = false; float worst = 0;
        for (float dep = 0.2f; dep <= 6.0f; dep += 0.1f) {
            float d[3]; dir_at(dep, 30.0f, d); const Trace r = trace(flat, fa, eye, d, 55000, 160000);
            if (have) for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(r.display[c] - last[c]));
            for (int c = 0; c < 3; ++c) last[c] = r.display[c];
            have = true;
        }
        std::printf("  haze column 0.2..6 deg below the horizon: largest step between 0.1-deg rows = %.1f/255\n", worst * 255.0f);
        CHECK(worst * 255.0f < 25.0f);
    }
}
static void test_mailbox_and_config() {
    CamMailbox& m = cam_mailbox();
    CamSample c; const bool had = m.try_read(c);
    const float p[3] = {1, 2, 3};
    m.publish(p, 0.1f, 0.2f, 0.3f, 0.4f);
    CHECK(m.try_read(c) && c.campos[0] == 1 && c.campos[2] == 3 && c.pitch == 0.1f && c.yaw == 0.2f && c.roll == 0.3f && c.heading == 0.4f && c.seq >= 1);
    (void)had;
    const uint64_t n = m.published();
    const float bad[3] = {NAN, 0, 0}; m.publish(bad, 0, 0, 0, 0);
    CHECK(!m.try_read(c) && m.published() == n + 1);   // non-finite values are never handed out
    m.publish(p, 0, 0, 0, 0); CHECK(m.try_read(c));
    // config keys
    auto r = parse_config("xr_terrain_render=true\nxr_terrain_stage=m02a\n");
    CHECK(r.issue == ConfigIssue::none && r.config.xr_terrain_render() && std::string(r.config.xr_terrain_stage()) == "m02a");
    r = parse_config("xr_terrain_stage=m02a;yaw:-12.5;mirror:1;blend:1\n");
    CHECK(r.issue == ConfigIssue::none && !r.config.xr_terrain_render() && std::string(r.config.xr_terrain_stage()) == "m02a;yaw:-12.5;mirror:1;blend:1");
    CHECK(!parse_config("").config.xr_terrain_render() && std::string(parse_config("").config.xr_terrain_stage()).empty());
    for (const char* bad : {"xr_terrain_render=maybe\n", "xr_terrain_render=\n", "xr_terrain_stage=\n", "xr_terrain_stage=m02a/../x\n", "xr_terrain_stage=m 02a\n", "xr_terrain_stage=m02a\"\n"})
        CHECK(parse_config(bad).issue != ConfigIssue::none);
    // VSEARLY/D-367, D-400: the value holds up to 1023 characters (xr_terrain_stage_capacity 1024, max_config_line 1056); 1024 is refused.
    CHECK(parse_config((std::string("xr_terrain_stage=") + std::string(1024, 'a') + "\n").c_str()).issue != ConfigIssue::none);
    CHECK(parse_config((std::string("xr_terrain_stage=") + std::string(1023, 'a') + "\n").c_str()).issue == ConfigIssue::none);
    CHECK(std::string(parse_config((std::string("xr_terrain_stage=") + std::string(1023, 'a') + "\n").c_str()).config.xr_terrain_stage()) == std::string(1023, 'a'));
    {   // the day value + vscreen/vsearly and more options (~200 characters) survives byte for byte, also with CRLF
        const std::string v = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:1;vscreen:1;vsearly:1;vsd:2.5;"
                              "vsw:3.2;vsenter:500;takeoff:k00a;wscale:1;e6loc:auto;lookstick:block;fxmem:0;gamecloud:-1;q:0";
        CHECK(v.size() > 190 && v.size() < 255);
        const auto rv = parse_config(("xr_terrain_stage=" + v + "\r\n").c_str());
        CHECK(rv.issue == ConfigIssue::none && std::string(rv.config.xr_terrain_stage()) == v);
    }
    CHECK(parse_config("xr_terrain_render=true\nxr_terrain_render=false\n").issue != ConfigIssue::none);   // duplicates stay rejected
    // budget: the full headset set + camera probe + the two terrain keys (28 of 32 keys) stays inside 32 keys / 2048 bytes, with a long option string
    const std::string base =
        "log_enabled=true\ndiagnostic_ownership_pins=true\ngpu_copy_witness_observation=true\ngpu_copy_early_device_hook=true\n"
        "gpu_copy_step3_experiment=true\ngpu_copy_step3_whole_set=true\ngpu_copy_step5_continuous=true\ngpu_copy_step5_members_by_class=true\n"
        "xr_stereo_output=true\ngpu_copy_step5_hmd_fov=true\ngpu_copy_step5_hud=true\nxr_hud_layer=true\nxr_hud_quad_distance_m=0.5\n"
        "xr_hud_scale=0.5\nxr_world_quad_distance_m=500\ngpu_copy_step5_units_per_metre=65\nxr_world_in_projection=true\ngpu_copy_step5_world_objects=true\n"
        "gpu_copy_step5_hud_markers=true\nxr_hud_markers_layer=true\ngpu_copy_step5_effects=true\nxr_effects_layer=true\nxr_world_hold_last=true\nxr_world_extend=true\n"
        "ee_camera_probe=true\n";
    const std::string full = base + "xr_terrain_render=true\nxr_terrain_stage=m02a;yaw:-123.456;mirror:1;blend:1;sgn:-1;alt:-30;fogl:55000;far:160000\n";
    size_t keys = 0; for (char ch : full) keys += ch == '\n';
    r = parse_config(full.c_str());
    CHECK(keys == 27 && keys <= 32 && full.size() < 2048 && r.issue == ConfigIssue::none && r.config.xr_terrain_render());
    CHECK(keys + 1 <= 32);   // the ee_ps2_probe key can be added too (28 of 32)
}

// ------------------------------------------------------------------------------------------------------------- D-299: format v2 (water) + river water shading
static void rehash(std::vector<uint8_t>& b) { sha256(b.data() + kZvrsHeader, b.size() - kZvrsHeader, &b[192]); }
static StageData water_stage() {
    StageData s = make_synthetic_stage(1, false);
    std::fill(s.heights.begin(), s.heights.end(), uint8_t(6)); s.hmax_byte = 6;
    s.tile_water.assign(4, 0); s.tile_water[1] = 1; s.water_key = 40; s.flags |= kFlagWater;
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) s.tiles[1 * 4096 + y * 64 + x] = x < 32 ? 40 : 16;   // left half = river key colour
    const uint32_t k = clut_entry(40), l = clut_entry(16);                                                                     // K00A-SNOW: palette entries of texels 40/16
    s.clut[k * 4] = 74; s.clut[k * 4 + 1] = 39; s.clut[k * 4 + 2] = 25;                                                       // a red-brown key colour (the pre-CSM1 m02a decode)
    s.clut[l * 4] = 90; s.clut[l * 4 + 1] = 110; s.clut[l * 4 + 2] = 60;
    for (auto& w : s.words) w = 1 | 0x1000;                                                                                      // every cell = tile 1, flag bit 12
    return s;
}
static void test_water_format() {
    const StageData s = water_stage();
    const auto bytes = serialize_stage(s);
    CHECK(rd_u32(bytes, 4) == 2 && (rd_u32(bytes, 16) & kFlagWater) && rd_u32(bytes, 84) == 40);
    StageData t; CHECK(parse_stage(bytes.data(), bytes.size(), t) == LoadStatus::ok);
    CHECK(t.has_water() && t.water_key == 40 && t.tile_water == s.tile_water && serialize_stage(t) == bytes);
    // a stage without water still serialises as v2 with an all-zero section and no water flag
    const StageData nw = make_synthetic_stage(2, false); const auto nb = serialize_stage(nw);
    StageData u; CHECK(parse_stage(nb.data(), nb.size(), u) == LoadStatus::ok && !u.has_water() && !(u.flags & kFlagWater) && serialize_stage(u) == nb);
    // v1 compatibility: a v1 file (no water fields, no water section) still loads, with no water
    {
        const uint32_t off_tw = rd_u32(nb, 80);
        std::vector<uint8_t> v1(nb.begin(), nb.begin() + off_tw);
        v1[4] = 1; for (int i = 80; i < 88; ++i) v1[i] = 0; v1[16] &= ~uint8_t(4);
        v1[76] = uint8_t(off_tw); v1[77] = uint8_t(off_tw >> 8); v1[78] = uint8_t(off_tw >> 16); v1[79] = uint8_t(off_tw >> 24);
        rehash(v1);
        StageData o; CHECK(parse_stage(v1.data(), v1.size(), o) == LoadStatus::ok && !o.has_water() && o.tile_water.empty() && o.heights == nw.heights && o.words == nw.words);
        Atlas a; build_atlas(o, a); CHECK(a.level[0][3] == 255);   // alpha untouched
        auto bad = v1; bad[16] |= 4; rehash(bad); StageData z; CHECK(parse_stage(bad.data(), bad.size(), z) == LoadStatus::bad_header);   // v1 can not carry water
        bad = v1; bad[84] = 1; rehash(bad); CHECK(parse_stage(bad.data(), bad.size(), z) == LoadStatus::bad_header);
    }
    // v2 refusals
    auto refuse = [&](std::vector<uint8_t> b, LoadStatus want) { StageData o; CHECK(parse_stage(b.data(), b.size(), o) == want); CHECK(o.heights.empty()); };
    { auto b = bytes; b[84] = 0; b[85] = 1; rehash(b); refuse(b, LoadStatus::bad_size); }                      // key 256
    { auto b = bytes; b[rd_u32(bytes, 80) + 1] = 2; rehash(b); refuse(b, LoadStatus::bad_content); }              // flag byte 2
    { auto b = bytes; b[80] += 1; rehash(b); refuse(b, LoadStatus::bad_size); }                                   // misaligned section
    { auto b = bytes; b[80] = 0; b[81] = 0; b[82] = 0; b[83] = 0; rehash(b); refuse(b, LoadStatus::bad_size); }    // offset inside the header
    { auto b = bytes; b[16] |= 0x20; rehash(b); refuse(b, LoadStatus::bad_header); }                              // unknown flag bit
    { auto b = bytes; b.resize(b.size() - 4); b[76] -= 4; rehash(b); refuse(b, LoadStatus::bad_size); }           // section cut off
}
static void test_water_shading() {
    StageData s = water_stage();
    Atlas a; build_atlas(s, a);
    // texel level 0: key texels get alpha 0 and the mean land colour (90,110,60), land texels keep alpha 255
    const uint8_t* wpx = &a.level[0][(size_t(1) * 4096 + 10 * 64 + 5) * 4]; const uint8_t* lpx = &a.level[0][(size_t(1) * 4096 + 10 * 64 + 50) * 4];
    CHECK(wpx[3] == 0 && wpx[0] == 90 && wpx[1] == 110 && wpx[2] == 60 && lpx[3] == 255 && lpx[0] == 90);
    float o4[4]; atlas_sample(a, 1, 0.1f, 0.5f, 0, o4); CHECK(o4[3] < 0.01f);
    atlas_sample(a, 1, 0.9f, 0.5f, 0, o4); CHECK(o4[3] > 0.99f);
    atlas_sample(a, 1, 0.5f, 0.5f, 0, o4); CHECK(o4[3] > 0.2f && o4[3] < 0.8f);       // the shoreline is filtered, not a hard edge
    atlas_sample(a, 1, 0.1f, 0.5f, 6, o4); CHECK(o4[3] > 0.3f && o4[3] < 0.7f);       // coarsest mip: half the tile is water
    // trace straight down onto the water half: grey-teal (g,b > r), not the red key colour; the land half is lit land colour
    const float dn[3] = {0, -1, 0};
    const float cell2 = 2.0f * s.cell;                                                  // one tile = 2048 units
    const float x_tile = 10 * cell2 - s.half;                                           // tile column 10
    const float ow[3] = {x_tile + 0.25f * cell2, 3000.0f, 0}, ol[3] = {x_tile + 0.75f * cell2, 3000.0f, 0};
    Trace rw = trace(s, a, ow, dn, 1e6f, 2e6f), rl = trace(s, a, ol, dn, 1e6f, 2e6f);
    CHECK(rw.hit && rw.river && rw.display[1] > rw.display[0] && rw.display[2] > rw.display[0] && rw.display[0] < 0.3f);
    CHECK(rl.hit && !rl.river && rl.display[0] > 0.3f);
    // without the water section the same texels are the red key colour (no change for stages without rivers)
    StageData s0 = s; s0.tile_water.clear(); s0.flags &= ~kFlagWater; Atlas a0; build_atlas(s0, a0);
    Trace r0 = trace(s0, a0, ow, dn, 1e6f, 2e6f);
    CHECK(!r0.river && r0.display[0] > r0.display[1] * 1.3f);
}
static void test_real_stage_if_present() {
    const char* la = std::getenv("ACZ_TERRAIN_TEST_DIR");
    const std::string dir = la ? la : "D:\\acz-extract\\stage";
    for (const char* id : {"m02a", "m16n"}) {
        StageData s; const LoadStatus st = load_stage_file(stage_path(id, dir), s);
        if (st == LoadStatus::io_error) { std::printf("  (no baked %s in %s: skipped)\n", id, dir.c_str()); continue; }
        CHECK(st == LoadStatus::ok && std::string(s.id) == id && s.ntiles > 100 && s.ntiles <= kMaxTiles);
        Atlas a; build_atlas(s, a);
        const float o[3] = {0, 20000, 0}, d[3] = {0, -1, 0};
        Trace r = trace(s, a, o, d, 20000, 58000);
        CHECK(r.hit && r.rgb[0] >= 0 && r.rgb[0] <= 1);
        // re-serialising a baked file reproduces the python baker's bytes exactly (cross-implementation check of the format)
        FILE* f = std::fopen(stage_path(id, dir).c_str(), "rb"); std::vector<uint8_t> orig(size_t(8) << 20); const size_t got = std::fread(orig.data(), 1, orig.size(), f); std::fclose(f); orig.resize(got);
        CHECK(serialize_stage(s) == orig);
        std::printf("  %s: tiles=%u objects=%u hmax=%u sea=%d payload-sha256=%s\n", id, s.ntiles, s.nobj, s.hmax_byte, s.sea_present() ? 1 : 0, hex(s.payload_sha, 32).c_str());
        if (std::string(id) == "m02a") {
            // WORLD-1 validation against the game: level flight on the mission-02 NE track (step54 pose: camera (-6961, 3880, 114622), heading -45 deg = azimuth 45),
            // median display G over azimuth 25..65 per depression row vs the game's measured rows (step56 pre-V shot, ~3500 AGL, terrain-issues-test16.md section 4).
            const float cam[3] = {-6961.3f, 3879.5f, 114622.3f};
            const float rows[6][2] = {{1.5f, 138}, {2.2f, 129}, {2.9f, 122}, {4.4f, 111}, {5.8f, 104}, {0.0f, 148}};
            for (const auto& row : rows) {
                std::vector<float> g;
                for (float az = 25.0f; az <= 65.0f; az += 0.5f) { float d[3]; dir_at(row[0], az, d); const Trace r = trace(s, a, cam, d, 55000, 160000); g.push_back(linear_to_srgb(r.rgb[1]) * 255.0f); }
                std::sort(g.begin(), g.end());
                const float med = g[g.size() / 2];
                std::printf("  m02a haze row %.1f deg below the horizon: our G %.0f, game G %.0f\n", row[0], med, row[1]);
                CHECK(std::fabs(med - row[1]) < 25.0f);
            }
            // a nearer ground sample (12 deg down, ~17 km): the game's lower ground at ~3.5-6 km measured G 69..83 in the step56 matched view
            std::vector<float> g;
            for (float az = 25.0f; az <= 65.0f; az += 0.5f) { float d[3]; dir_at(30.0f, az, d); const Trace r = trace(s, a, cam, d, 55000, 160000); g.push_back(linear_to_srgb(r.rgb[1]) * 255.0f); }
            std::sort(g.begin(), g.end());
            std::printf("  m02a ground 30 deg below the horizon (~7 km): our median G %.0f (game lower ground 69..83)\n", g[g.size() / 2]);
            CHECK(g[g.size() / 2] > 45.0f && g[g.size() / 2] < 110.0f);
        }
    }
}
// ------------------------------------------------------------------------------------------------------------- WORLD-2: format v3 (environment) + sky/light/cloud/water
static void test_env_format_and_options() {
    // options: default off; each sub-option sets its bit; env:1 all; bad values refused
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && c.env_bits == 0);
    CHECK(parse_stage_option("m02a;sky:1;water:1", id, c, why) && c.env_bits == (kEnvSky | kEnvWater));
    CHECK(parse_stage_option("m02a;light:1;clouds:1", id, c, why) && c.env_bits == (kEnvLight | kEnvCloud));
    CHECK(parse_stage_option("m02a;env:1;cloud:0", id, c, why) && c.env_bits == (kEnvAll & ~kEnvCloud));
    CHECK(!parse_stage_option("m02a;sky:2", id, c, why) && !parse_stage_option("m02a;sky", id, c, why));
    // v3 round trip; a stage without env still writes v2 (old files and the baked v2 stages round-trip unchanged)
    StageData s = make_synthetic_stage(1, false); add_synthetic_env(s);
    const auto b = serialize_stage(s);
    CHECK(rd_u32(b, 4) == 3 && (rd_u32(b, 16) & kFlagEnv) && rd_u32(b, 92) == kEnvSectionBytes);
    StageData t; CHECK(parse_stage(b.data(), b.size(), t) == LoadStatus::ok && t.has_env() && t.env_params == s.env_params && t.env_cloud == s.env_cloud &&
                       t.env_wave == s.env_wave && t.env_sprite_a == s.env_sprite_a && serialize_stage(t) == b);
    const auto b2 = serialize_stage(make_synthetic_stage(1, false)); CHECK(rd_u32(b2, 4) == 2);
    auto refuse = [&](std::vector<uint8_t> x, LoadStatus want) { StageData o; CHECK(parse_stage(x.data(), x.size(), o) == want); CHECK(o.heights.empty()); };
    const uint32_t oe = rd_u32(b, 88);
    { auto x = b; x[oe] = 'X'; rehash(x); refuse(x, LoadStatus::bad_content); }                                  // section magic
    { auto x = b; x[92] += 4; rehash(x); refuse(x, LoadStatus::bad_size); }                                       // size
    { auto x = b; x[88] += 2; rehash(x); refuse(x, LoadStatus::bad_size); }                                       // misaligned
    { auto x = b; const float nan = std::nanf(""); std::memcpy(&x[oe + 8 + 4], &nan, 4); rehash(x); refuse(x, LoadStatus::bad_content); }  // non-finite parameter
    { auto x = b; x[16] &= ~uint8_t(kFlagEnv); rehash(x); refuse(x, LoadStatus::bad_header); }                     // offsets without the flag
    { auto x = b2; x[16] |= uint8_t(kFlagEnv); rehash(x); refuse(x, LoadStatus::bad_header); }                     // v2 can not carry env
    // env texture: params copied, maps scaled, wave tiled, sprite only in the corner; yaw convention rotates the directions
    std::vector<float> tex; build_env_texture(s, Conventions{}, tex);
    CHECK(tex.size() == size_t(kEnvTexW) * kEnvTexH * 4 && tex[kPMoon * 4 + 3] == 0.85f);
    CHECK(tex[(size_t(1 + 32) * kEnvTexW + 32) * 4 + 0] == 1.0f && tex[(size_t(1) * kEnvTexW + 0) * 4 + 0] == 0.0f);
    CHECK(tex[(size_t(1 + 5) * kEnvTexW + 7) * 4 + 1] == tex[(size_t(1 + 5 + 128) * kEnvTexW + 7 + 128) * 4 + 1]);
    CHECK(tex[(size_t(1 + 16) * kEnvTexW + 16) * 4 + 3] > 0.5f && tex[(size_t(1 + 16) * kEnvTexW + 40) * 4 + 3] == 0.0f);
    Conventions rot; rot.yaw_offset_deg = 90.0f; std::vector<float> tex2; build_env_texture(s, rot, tex2);
    const float* l0 = &tex[kPLight * 4]; const float* l1 = &tex2[kPLight * 4];
    const float a0 = std::atan2(-l0[0], -l0[2]), a1 = std::atan2(-l1[0], -l1[2]);
    float da = a1 - a0; while (da > 3.14159f) da -= 6.2831853f; while (da < -3.14159f) da += 6.2831853f;
    CHECK(std::fabs(da - 1.5707963f) < 1e-4f && std::fabs(l1[1] - l0[1]) < 1e-6f);
    StageData noenv = make_synthetic_stage(1, false); std::vector<float> te; build_env_texture(noenv, Conventions{}, te); CHECK(te.empty());
}
static void dir_el_az(float el_deg, float az_deg, float d[3]) {
    const float e = el_deg * 3.14159265f / 180.0f, a = az_deg * 3.14159265f / 180.0f;
    d[0] = -std::sin(a) * std::cos(e); d[1] = std::sin(e); d[2] = -std::cos(a) * std::cos(e);
}
static void test_env_shading() {
    StageData s = make_synthetic_stage(1, false); add_synthetic_env(s, 9000.0f);
    std::vector<float> tex; build_env_texture(s, Conventions{}, tex);
    Atlas a; build_atlas(s, a);
    const EnvView off{tex.data(), 0}, sky{tex.data(), kEnvSky}, all{tex.data(), kEnvAll};
    // bits 0 = trace() exactly (legacy)
    for (int k = 0; k < 40; ++k) {
        float d[3]; dir_el_az(-30.0f + 1.7f * k, 13.0f * k, d);
        const float o[3] = {1000.0f + 50 * k, 6000.0f, -2000.0f};
        const Trace t0 = trace(s, a, o, d, 20000, 58000), t1 = trace_env(s, a, off, o, d, 20000, 58000);
        CHECK(t0.rgb[0] == t1.rgb[0] && t0.rgb[1] == t1.rgb[1] && t0.rgb[2] == t1.rgb[2] && t0.hit == t1.hit);
    }
    // sky: the m02a stops reproduce the measured game profile toward the sprite (step56 pre-V shot, terrain-issues-test16.md section 4) within 8/255
    const float prof[6][4] = {{5.6f, 101, 133, 136}, {7.9f, 90, 127, 134}, {10.2f, 82, 120, 130}, {12.4f, 77, 115, 125}, {14.5f, 70, 108, 119}, {16.6f, 64, 103, 115}};
    for (const auto& p : prof) {
        float d[3]; dir_el_az(p[0], -45.0f, d);   // the shot looked NE (heading -45 deg), 8 deg left of the sprite
        float c[3]; env_sky(sky, d, false, c);
        for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(c[ch] * 255.0f - p[1 + ch]) < 8.0f);
    }
    // the horizon (elevation 0) is the stage horizon colour; the haze colour equals the sky at elevation 0 (no seam); away from the sprite the sky is darker (rows 1/2)
    { float d[3]; dir_el_az(0, -53, d); float c[3], h[3]; env_sky(sky, d, false, c); env_haze(sky, d, h);
      CHECK(std::fabs(c[1] * 255 - 156) < 0.5f && c[0] == h[0] && c[1] == h[1] && c[2] == h[2]);
      float db[3]; dir_el_az(10, 127, db); float cb[3]; env_sky(sky, db, false, cb); float df[3]; dir_el_az(10, -53, df); float cf[3]; env_sky(sky, df, false, cf);
      CHECK(cb[1] < cf[1] - 0.1f); }
    // the sky sprite: exactly at its direction the sky is brighter, 3 degrees off it is not
    { float d[3]; dir_el_az(10, -53, d); float c0[3], c1[3]; env_sky(sky, d, false, c0); env_sky(sky, d, true, c1); CHECK(c1[1] > c0[1] + 0.1f);
      float d2[3]; dir_el_az(13, -53, d2); env_sky(sky, d2, false, c0); env_sky(sky, d2, true, c1); CHECK(c1[1] == c0[1]); }
    // the sun disc at the light direction (el 50, az -130): the light colour normalised to its max channel; 2 degrees off only a faint glow
    { float d[3]; dir_el_az(50, -130, d); float c[3]; env_sky(sky, d, true, c); CHECK(std::fabs(c[0] - 1.0f) < 0.02f && std::fabs(c[2] - 68.0f / 83.0f) < 0.02f);
      float d2[3]; dir_el_az(52, -130, d2); float c2[3], c0[3]; env_sky(sky, d2, true, c2); env_sky(sky, d2, false, c0); CHECK(c2[1] > c0[1] && c2[1] < c0[1] + 0.3f && c2[1] < c[1] - 0.05f); }
    // a missed ray with sky:1 is the env sky (+ clouds off here)
    { float d[3]; dir_el_az(20, 30, d); const float o[3] = {0, 6000, 0}; Trace r = trace_env(s, a, sky, o, d, 20000, 58000); float c[3]; env_sky(sky, d, true, c);
      CHECK(r.sky && r.display[0] == c[0] && r.display[1] == c[1]); }
    // light: flat ground keeps its texel colour (normalised); a slope facing the light is brighter, facing away darker (within the clamp)
    {
        StageData f = flat_stage(10); add_synthetic_env(f); std::vector<float> tf; build_env_texture(f, Conventions{}, tf); Atlas af; build_atlas(f, af);
        const EnvView lt{tf.data(), kEnvLight};
        const float o[3] = {0, 3000, 0}, dn[3] = {0, -1, 0};
        Trace rl = trace_env(f, af, lt, o, dn, 1e6f, 2e6f);
        float tx[4]; const float u = (0 + f.half) / f.cell * 0.5f, v = (0 + f.half) / f.cell * 0.5f; const int tx0 = int(u), tz0 = int(v);
        atlas_sample(af, f.words[size_t(tz0) * kTGrid + tx0] & 0x3ffu, u - tx0, v - tz0, 0, tx);
        CHECK(rl.hit && std::fabs(rl.display[0] - tx[0]) < 3e-3f && std::fabs(rl.display[1] - tx[1]) < 3e-3f);   // (WORLD-1 haze has no start distance: 0.2% at 2.2 km, L 1e6)
        StageData sl = f;   // a 13-degree ramp rising toward +X (the light comes from +X/+Z: el 50 az -130 -> dir (0.49, 0.77, 0.41)); one tile id everywhere
        for (auto& wd : sl.words) wd = 0;
        StageData sr = sl;
        for (uint32_t j = 0; j < kHDim; ++j) for (uint32_t i = 0; i < kHDim; ++i) {
            sl.heights[size_t(j) * kHDim + i] = uint8_t(std::min(250u, 3 * i));
            sr.heights[size_t(j) * kHDim + i] = uint8_t(std::min(250u, 3 * (512 - i)));
        }
        sl.hmax_byte = sr.hmax_byte = 250; Atlas as, ar; build_atlas(sl, as); build_atlas(sr, ar);
        const float ol[3] = {-262144.0f + 40 * 1024.0f, 40000, 0}, orr[3] = {-262144.0f + 472 * 1024.0f, 40000, 0};
        Trace away = trace_env(sl, as, lt, ol, dn, 1e6f, 2e6f), toward = trace_env(sr, ar, lt, orr, dn, 1e6f, 2e6f);
        // ramp up toward +X faces -X (away from the light): darker; the mirrored ramp faces +X: brighter
        CHECK(away.hit && toward.hit && toward.display[1] > away.display[1] * 1.05f);
    }
    // water: looking straight down at the river half of a water tile gives (about) the stage water colour; at grazing angles the sky reflection dominates
    {
        StageData w = make_synthetic_stage(1, false);
        std::fill(w.heights.begin(), w.heights.end(), uint8_t(6)); w.hmax_byte = 6;
        w.tile_water.assign(4, 0); w.tile_water[1] = 1; w.water_key = 40; w.flags |= kFlagWater;
        for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) w.tiles[1 * 4096 + y * 64 + x] = x < 32 ? 40 : 16;
        for (auto& wd : w.words) wd = 1 | 0x1000;
        add_synthetic_env(w); std::vector<float> tw; build_env_texture(w, Conventions{}, tw); Atlas aw; build_atlas(w, aw);
        const EnvView wv{tw.data(), kEnvWater | kEnvSky};
        const float cell2 = 2.0f * w.cell, x_tile = 10 * cell2 - w.half;
        const float ow[3] = {x_tile + 0.25f * cell2, 3000.0f, 0}, dn[3] = {0, -1, 0};
        Trace r = trace_env(w, aw, wv, ow, dn, 1e6f, 2e6f);
        CHECK(r.hit && r.river && std::fabs(r.display[0] * 255 - 70) < 12 && std::fabs(r.display[1] * 255 - 89) < 12 && std::fabs(r.display[2] * 255 - 84) < 12);
    }
    // clouds: from below, a ray through a cloud blob is cloud-coloured; through a gap it is the sky; the cloud shadow darkens the ground under a blob
    {
        const EnvView cl{tex.data(), kEnvCloud | kEnvSky};
        const float H = 9000.0f;
        // blob centre texel (32,32) -> world (32/256*rep) = 8192; gap at texel (0,0)
        const float o[3] = {8192.0f, 6000.0f, 8192.0f - 3000.0f};
        float d[3] = {0, (H - o[1]) / 3000.0f, 1}; norm3(d);
        Trace r = trace_env(s, a, cl, o, d, 20000, 58000);
        float skyc[3]; env_sky(cl, d, true, skyc);
        CHECK(r.sky && std::fabs(r.display[1] - skyc[1]) > 0.02f);
        const float og[3] = {100.0f, 6000.0f, 100.0f - 3000.0f};
        Trace rg = trace_env(s, a, cl, og, d, 20000, 58000); float skyg[3]; env_sky(cl, d, true, skyg);
        CHECK(rg.sky && std::fabs(rg.display[1] - skyg[1]) < 1e-5f);
        // cloud shadow: straight down under the blob's shadow (light (0.49,0.77,0.41)) vs in a gap, light:1 + cloud:1 vs light:1 only
        StageData f = flat_stage(10); add_synthetic_env(f, 9000.0f); std::vector<float> tf; build_env_texture(f, Conventions{}, tf); Atlas af; build_atlas(f, af);
        const float* L = &tf[kPLight * 4]; const float sd = (H - 800.0f) / L[1];
        const float og2[3] = {8192.0f - L[0] * sd, 3000.0f, 8192.0f - L[2] * sd}, dn[3] = {0, -1, 0};
        Trace sh = trace_env(f, af, EnvView{tf.data(), kEnvLight | kEnvCloud}, og2, dn, 1e6f, 2e6f), ns = trace_env(f, af, EnvView{tf.data(), kEnvLight}, og2, dn, 1e6f, 2e6f);
        CHECK(sh.hit && ns.hit && sh.display[1] < ns.display[1] * 0.95f);
    }
}
// EFFECTS-OUTSIDE (A) gamecloud:0|1 (docs/world-render/effects-outside-window.md): parse + the default rule (off only when our clouds are active).
static void test_gamecloud() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && c.game_cloud == -1);
    CHECK(!drop_game_clouds(c, false) && drop_game_clouds(c, true));                    // default: follow our clouds
    CHECK(parse_stage_option("m02a;env:1;gamecloud:1", id, c, why) && c.game_cloud == 1 && !drop_game_clouds(c, true));   // explicit keep
    CHECK(parse_stage_option("m02a;gamecloud:0", id, c, why) && c.game_cloud == 0 && drop_game_clouds(c, false));         // explicit drop
    CHECK(!parse_stage_option("m02a;gamecloud:2", id, c, why) && why.find("gamecloud") != std::string::npos);
    CHECK(!parse_stage_option("m02a;gamecloud", id, c, why));
    CHECK(parse_stage_option("m02a;cloud:1", id, c, why) && (c.env_bits & kEnvCloud) && c.game_cloud == -1);
    CHECK(parse_stage_option("m02a;env:1;look:1", id, c, why) && c.game_cloud == -1 && !c.fx_memory);   // existing values parse exactly as before
    CHECK(parse_stage_option("m02a;fxmem:1", id, c, why) && c.fx_memory && parse_stage_option("m02a;fxmem:0", id, c, why) && !c.fx_memory);   // (B) reserved
    CHECK(!parse_stage_option("m02a;fxmem:yes", id, c, why));
    CHECK(parse_stage_option("m02a;fxmem:2", id, c, why) && c.fx_memory == 2 && parse_stage_option("m02a;fxmem:1", id, c, why) && c.fx_memory == 1);   // (C) 2 = + replay dry run
    CHECK(parse_stage_option("m02a;fxmem:3", id, c, why) && c.fx_memory == 3);   // (D) 3 = 1 + draw census
    CHECK(parse_stage_option("m02a;fxmem:4", id, c, why) && c.fx_memory == 4);   // (C) 4 = 2 + GPU replay per eye (fx_smoke_gpu.h)
    CHECK(parse_stage_option("m02a;fxmem:5", id, c, why) && c.fx_memory == 5);   // (E) 5 = 4 + explosion window effects + sprite plan
    CHECK(parse_stage_option("m02a;fxmem:6", id, c, why) && c.fx_memory == 6);   // (F) 6 = 5 + sprite (explosion) GPU replay per eye
    CHECK(!parse_stage_option("m02a;fxmem:7", id, c, why) &&!parse_stage_option("m02a;fxmem:", id, c, why) && !parse_stage_option("m02a;fxmem:22", id, c, why));
}
// WORLD-4 look:1 (docs/world-render/world4-impl.md): options, texture slots, the fitted sky against the game's HUD-view rows, gain, haze target.
static void test_look() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a;env:1", id, c, why) && !c.look && c.haze_l == 55000.0f);
    CHECK(parse_stage_option("m02a;env:1;look:1", id, c, why) && c.look && c.haze_l == kLookHazeL);
    CHECK(parse_stage_option("m02a;look:1;fogl:30000;sky:1", id, c, why) && c.look && c.haze_l == 30000.0f);   // an explicit fogl: wins
    CHECK(parse_stage_option("m02a;sky:1;look:0", id, c, why) && !c.look && c.haze_l == 55000.0f);
    CHECK(!parse_stage_option("m02a;look:1", id, c, why) && why.find("sky") != std::string::npos);                // needs the environment
    CHECK(!parse_stage_option("m02a;light:1;look:1", id, c, why) && !parse_stage_option("m02a;env:1;look:2", id, c, why));
    StageData s = make_synthetic_stage(1, false); add_synthetic_env(s, 9000.0f);
    std::vector<float> t0, t1; Conventions off, on; on.look = true;
    build_env_texture(s, off, t0); build_env_texture(s, on, t1);
    for (uint32_t i = kPLook; i <= kPLookSky2; ++i) for (int k = 0; k < 4; ++k) CHECK(t0[i * 4 + k] == 0.0f);   // look:0: the slots stay zero (baked zero)
    // D-344 C2: F = the stage's sph row-2 horizon colour P[kPSky + 8] (the synthetic env carries m02a's (85,102,98)), inside the m02a fit's +-4/255 of kLookFog
    const float* Fst = &s.env_params[size_t(kPSky + 8u) * 4];
    CHECK(t1[kPLook * 4 + 3] == 1.0f && t1[kPLook * 4] == kLookGain[0] && t1[kPLookSky * 4] == kLookK);
    for (int ch = 0; ch < 3; ++ch) CHECK(t1[kPLookFog * 4 + ch] == Fst[ch] && std::fabs(t1[kPLookFog * 4 + ch] - kLookFog[ch]) <= kLookFogTol + 1e-6f);
    CHECK(std::fabs(t1[kPMoon * 4 + 3] - 0.85f * kLookMoon) < 1e-6f);
    size_t diff = 0; for (size_t i = 0; i < t0.size(); ++i) if (t0[i] != t1[i]) ++diff;
    CHECK(diff <= 14);   // only P[26..29] and the moon strength differ
    const EnvView sky0{t0.data(), kEnvSky}, sky1{t1.data(), kEnvSky};
    // the game's HUD-view sky (step56 pre-V shot, no canopy glass; world4_fit.py rows) looking NE (heading -45, the moon sprite at -53): within 6/255
    const float prof[9][4] = {{18.15f, 62, 100, 113}, {8.72f, 93, 128, 135}, {7.07f, 102, 133, 136}, {5.16f, 113, 139, 138}, {3.24f, 124, 145, 139},
                              {2.27f, 128, 147, 140}, {1.30f, 125, 143, 136}, {0.33f, 116, 134, 128}, {-0.61f, 104, 123, 118}};
    float worst1 = 0, worst0 = 0;
    for (const auto& p : prof) {
        float d[3]; dir_el_az(p[0], -45.0f, d);
        float a[3], b[3]; env_sky(sky1, d, false, a); env_sky(sky0, d, false, b);
        for (int ch = 0; ch < 3; ++ch) { worst1 = std::max(worst1, std::fabs(a[ch] * 255 - p[1 + ch])); worst0 = std::max(worst0, std::fabs(b[ch] * 255 - p[1 + ch])); }
    }
    std::printf("  look:1 sky vs the game's HUD-view rows: worst %.1f/255 (look:0 %.1f/255)\n", worst1, worst0);
    CHECK(worst1 < 6.0f && worst1 < worst0);
    // the haze target: looking steeply down = F; just below the horizon brighter than F (the glow gradient); look:0 = the horizon colour for every ray
    { float d[3]; dir_el_az(-30, -45, d); float h[3]; env_haze(sky1, d, h); for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(h[ch] - Fst[ch]) < 1e-6f);
      float d2[3]; dir_el_az(-1, -45, d2); float h2[3]; env_haze(sky1, d2, h2); CHECK(h2[1] > h[1] + 0.05f);
      float h0[3]; env_haze(sky0, d, h0); CHECK(h0[1] > h[1] + 0.1f); }   // look:0: the (much brighter) horizon colour even looking down
    // the ground: flat terrain straight down keeps texel x gain (lighting normalised to flat ground, haze negligible at L = 1e6)
    {
        StageData f = flat_stage(10); add_synthetic_env(f); std::vector<float> tf; build_env_texture(f, on, tf); Atlas af; build_atlas(f, af);
        const float o[3] = {0, 3000, 0}, dn[3] = {0, -1, 0};
        Trace r = trace_env(f, af, EnvView{tf.data(), kEnvLight | kEnvSky}, o, dn, 1e6f, 2e6f);
        float tx[4]; const float u = (0 + f.half) / f.cell * 0.5f, v = (0 + f.half) / f.cell * 0.5f; const int tx0 = int(u), tz0 = int(v);
        atlas_sample(af, f.words[size_t(tz0) * kTGrid + tx0] & 0x3ffu, u - tx0, v - tz0, 0, tx);
        for (int ch = 0; ch < 3; ++ch) CHECK(r.hit && std::fabs(r.display[ch] - tx[ch] * kLookGain[ch]) < 4e-3f);
        // far ground at a steep angle with a short haze is the fog colour F
        Trace rf = trace_env(f, af, EnvView{tf.data(), kEnvSky}, o, dn, 50.0f, 2e6f);
        for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(rf.display[ch] - tf[kPLookFog * 4 + ch]) < 2e-3f);
    }
    // D-344 C2: options; lookfog:fit restores the constant; on the m02a values every look:1 colour moves by at most the fit tolerance; a night stage's F follows its sky
    CHECK(parse_stage_option("m02a;env:1;look:1", id, c, why) && !c.look_fog_fit);
    CHECK(parse_stage_option("m02a;env:1;look:1;lookfog:fit", id, c, why) && c.look_fog_fit);
    CHECK(parse_stage_option("m02a;env:1;look:1;lookfog:stage", id, c, why) && !c.look_fog_fit);
    CHECK(!parse_stage_option("m02a;env:1;look:1;lookfog:1", id, c, why));
    {
        Conventions fit = on; fit.look_fog_fit = true;
        std::vector<float> tfit; build_env_texture(s, fit, tfit);
        for (int ch = 0; ch < 3; ++ch) CHECK(tfit[kPLookFog * 4 + ch] == kLookFog[ch]);
        size_t dfit = 0; for (size_t i = 0; i < t1.size(); ++i) if (t1[i] != tfit[i]) ++dfit;
        CHECK(dfit >= 1 && dfit <= 3);   // only F differs between stage and fit
        const EnvView skyf{tfit.data(), kEnvSky};
        float worst = 0;
        for (int el = -40; el <= 30; el += 2)
            for (int az = -180; az < 180; az += 30) {
                float d[3]; dir_el_az(float(el), float(az), d);
                float a[3], b[3], ha[3], hb[3]; env_sky(sky1, d, true, a); env_sky(skyf, d, true, b); env_haze(sky1, d, ha); env_haze(skyf, d, hb);
                for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::max(std::fabs(a[ch] - b[ch]), std::fabs(ha[ch] - hb[ch])));
            }
        std::printf("  D-344 C2 on m02a values: stage F vs fitted F, worst sky/haze change %.2f/255\n", worst * 255.0f);
        CHECK(worst <= kLookFogTol + 1e-5f);
        // whole-trace check: hazed flat ground straight down (the F-dominated case) moves by at most the tolerance
        StageData f = flat_stage(10); add_synthetic_env(f); Atlas af; build_atlas(f, af);
        std::vector<float> ts, tq; build_env_texture(f, on, ts); build_env_texture(f, fit, tq);
        const float o[3] = {0, 3000, 0}, dn[3] = {0, -1, 0};
        const Trace ra = trace_env(f, af, EnvView{ts.data(), kEnvAll}, o, dn, 2000.0f, 2e6f), rb = trace_env(f, af, EnvView{tq.data(), kEnvAll}, o, dn, 2000.0f, 2e6f);
        for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(ra.display[ch] - rb.display[ch]) <= kLookFogTol + 1e-5f);
        // a night stage (m11a's row-2 horizon (24,15,12), stage-sweep.md section 2): the far ground fades to the stage's dark colour, not the day fog
        StageData n = s; float* r2 = &n.env_params[size_t(kPSky + 8u) * 4]; r2[0] = 24 / 255.0f; r2[1] = 15 / 255.0f; r2[2] = 12 / 255.0f;
        std::vector<float> tn; build_env_texture(n, on, tn);
        float d[3]; dir_el_az(-30, -45, d); float h[3]; env_haze(EnvView{tn.data(), kEnvSky}, d, h);
        for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(h[ch] - r2[ch]) < 1e-6f && h[ch] < kLookFog[ch] - 0.2f);
    }
    // the moon: a much fainter excess with look:1
    { float d[3]; dir_el_az(10, -53, d); float a0[3], a1[3], b0[3], b1[3]; env_sky(sky0, d, false, a0); env_sky(sky0, d, true, a1); env_sky(sky1, d, false, b0); env_sky(sky1, d, true, b1);
      CHECK(b1[1] - b0[1] > 0.0f && b1[1] - b0[1] < 0.2f * (a1[1] - a0[1])); }
}
// Real v3 (environment) bakes, if present (D:\acz-extract\stage-w2 or ACZ_TERRAIN_W2_DIR): load, round-trip byte identity, env texture; optional PPM renders of
// the step56 pre-V pose (ACZ_W2_RENDER_DIR set): legacy / sky / light / all, game window lens (vertical 38.1 deg, 2421x1362 aspect), values = display domain x 255.
static void test_real_env_if_present() {
    const char* la = std::getenv("ACZ_TERRAIN_W2_DIR");
    const std::string dir = la ? la : "D:\\acz-extract\\stage-w2";
    for (const char* id : {"m02a", "m16n", "m07a"}) {
        StageData s; const LoadStatus st = load_stage_file(stage_path(id, dir), s);
        if (st == LoadStatus::io_error) { std::printf("  (no v3 %s in %s: skipped)\n", id, dir.c_str()); continue; }
        CHECK(st == LoadStatus::ok && s.has_env());
        FILE* f = std::fopen(stage_path(id, dir).c_str(), "rb"); std::vector<uint8_t> orig(size_t(8) << 20); const size_t got = std::fread(orig.data(), 1, orig.size(), f); std::fclose(f); orig.resize(got);
        CHECK(serialize_stage(s) == orig);
        std::vector<float> tex; build_env_texture(s, Conventions{}, tex); CHECK(!tex.empty());
        const float* L = &tex[kPLight * 4]; const float* mo = &tex[kPMoon * 4];
        std::printf("  %s v3: light (%.3f %.3f %.3f) moon (%.3f %.3f %.3f) w=%.2f water (%.0f %.0f %.0f) cloud alt %.0f on %.0f\n", id, L[0], L[1], L[2], mo[0], mo[1], mo[2], mo[3],
                    tex[kPWater * 4] * 255, tex[kPWater * 4 + 1] * 255, tex[kPWater * 4 + 2] * 255, tex[kPCloudAlt * 4], tex[kPCloudOn * 4]);
        // WORLD-4: the step62 capture pose (E6 campos, heading -45, level) in the game lens (tan 0.6152 x 0.3460, 1203x677 = half the game client),
        // env:1 with look:0 and look:1 (haze 55000 / kLookHazeL) -> PPM for world4_compare.py (ACZ_W4_RENDER_DIR set; m02a only)
        if (const char* o4 = std::getenv("ACZ_W4_RENDER_DIR"); o4 && std::strcmp(id, "m02a") == 0) {
            Atlas a; build_atlas(s, a);
            const float cam[3] = {-14340.0f, 4003.575f, 122000.0f};
            for (int look = 0; look < 2; ++look) {
                Conventions cv; cv.look = look != 0; std::vector<float> tl; build_env_texture(s, cv, tl);
                CamFrame cf{}; camera_frame(cam, 0.0f, -0.785398f, 0.0f, cv, cf);
                const int W = 1203, H = 677; const float tx = 0.6152f, ty = 0.3460f;
                std::vector<uint8_t> img(size_t(W) * H * 3);
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    const float sx = ((x + 0.5f) / W * 2 - 1) * tx, sy = (1 - (y + 0.5f) / H * 2) * ty;
                    float d[3]; for (int i = 0; i < 3; ++i) d[i] = cf.fwd[i] + cf.right[i] * sx + cf.up[i] * sy;
                    norm3(d);
                    const Trace t = trace_env(s, a, EnvView{tl.data(), kEnvAll}, cf.eye, d, look ? kLookHazeL : 55000.0f, 160000.0f);
                    for (int c = 0; c < 3; ++c) img[(size_t(y) * W + x) * 3 + c] = uint8_t(std::lround(std::min(std::max(t.display[c], 0.0f), 1.0f) * 255.0f));
                }
                const std::string p = std::string(o4) + "\\m02a-step62-env-look" + char('0' + look) + ".ppm";
                FILE* o = std::fopen(p.c_str(), "wb");
                if (o) { std::fprintf(o, "P6\n%d %d\n255\n", W, H); std::fwrite(img.data(), 1, img.size(), o); std::fclose(o); }
            }
        }
        const char* out = std::getenv("ACZ_W2_RENDER_DIR");
        if (!out) continue;
        Atlas a; build_atlas(s, a);
        struct V { const char* tag; float cam[3]; float hdg_deg, pitch_deg; };
        const V views[] = {{"preV", {-15182.0f, 4003.6f, 122843.0f}, -45.0f, -2.2f}, {"south-up", {-15182.0f, 4003.6f, 122843.0f}, 135.0f, 12.0f},
                           {"down", {-15182.0f, 4003.6f, 122843.0f}, -45.0f, -30.0f}};
        const uint32_t modes[] = {0u, kEnvSky, kEnvSky | kEnvLight, kEnvAll};
        const char* mtag[] = {"legacy", "sky", "skylight", "all"};
        const int W = 968, H = 545; const float tv = std::tan(38.1f * 0.5f * 3.14159265f / 180.0f), th = tv * 2421.0f / 1362.0f;
        for (const V& v : views) {
            CamFrame cf{}; Conventions cv; camera_frame(v.cam, v.pitch_deg * 3.14159265f / 180.0f, v.hdg_deg * 3.14159265f / 180.0f, 0.0f, cv, cf);
            for (int mi = 0; mi < 4; ++mi) {
                const EnvView ev{tex.data(), modes[mi]};
                std::vector<uint8_t> img(size_t(W) * H * 3);
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    const float sx = ((x + 0.5f) / W * 2 - 1) * th, sy = (1 - (y + 0.5f) / H * 2) * tv;
                    float d[3]; for (int i = 0; i < 3; ++i) d[i] = cf.fwd[i] + cf.right[i] * sx + cf.up[i] * sy;
                    norm3(d);
                    const Trace t = trace_env(s, a, ev, cf.eye, d, 55000.0f, 160000.0f);   // WORLD-1 haze defaults; PPM = display domain
                    for (int c = 0; c < 3; ++c) img[(size_t(y) * W + x) * 3 + c] = uint8_t(std::lround(std::min(std::max(t.display[c], 0.0f), 1.0f) * 255.0f));
                }
                const std::string p = std::string(out) + "\\" + id + "-" + v.tag + "-" + mtag[mi] + ".ppm";
                FILE* o = std::fopen(p.c_str(), "wb");
                if (o) { std::fprintf(o, "P6\n%d %d\n255\n", W, H); std::fwrite(img.data(), 1, img.size(), o); std::fclose(o); }
            }
        }
    }
}
// D-344 C1 (docs/world-render/stage-sweep.md sections 4/5): the sea water test is masked by tile-word bit 13 when the stage has sea tiles (v03a: a flat city at
// the sea level with sea tiles on half the map turned entirely into water). Stages without bit 13, and seamask:0, keep the old height-only rule.
static void test_sea_mask() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("v03a", id, c, why) && c.sea_mask);
    CHECK(parse_stage_option("v03a;seamask:0", id, c, why) && !c.sea_mask);
    CHECK(parse_stage_option("v03a;seamask:1;sea=off", id, c, why) && c.sea_mask && c.sea_off);
    CHECK(!parse_stage_option("v03a;seamask:2", id, c, why) && !parse_stage_option("v03a;seamask:yes", id, c, why));
    // flat stage at height 0, sea plane at 0; bit 13 on the west half of the tile grid (tile x < 128 = world x < 0)
    StageData s = flat_stage(0, true, 0.0f);
    CHECK(!sea_tiles_flagged(s) && !s.sea_masked);
    for (uint32_t j = 0; j < kTGrid; ++j) for (uint32_t i = 0; i < kTGrid / 2; ++i) s.words[size_t(j) * kTGrid + i] |= kTileSeaBit;
    CHECK(sea_tiles_flagged(s));
    // the format is unchanged: bit 13 survives the round trip, sea_masked is derived by the loader (not serialised)
    const std::vector<uint8_t> bytes = serialize_stage(s);
    StageData t; CHECK(parse_stage(bytes.data(), bytes.size(), t) == LoadStatus::ok);
    CHECK(t.sea_masked && t.words == s.words && serialize_stage(t) == bytes);
    { StageData plain; const std::vector<uint8_t> pb = serialize_stage(flat_stage(0, true, 0.0f)); CHECK(parse_stage(pb.data(), pb.size(), plain) == LoadStatus::ok && !plain.sea_masked); }
    Atlas a; build_atlas(t, a);
    const float dn[3] = {0, -1, 0};
    const float ow[3] = {-50000.0f, 2000.0f, 1000.0f}, oe[3] = {50000.0f, 2000.0f, 1000.0f};
    const Trace w = trace(t, a, ow, dn, 20000, 58000), e = trace(t, a, oe, dn, 20000, 58000);
    CHECK(w.hit && w.water && e.hit && !e.water && std::fabs(e.p[1]) < 1.0f);   // sea tile = water; land tile at the same height = land
    CHECK(sea_water_at(t, -50000.0f, 1000.0f, 0.0f) && !sea_water_at(t, 50000.0f, 1000.0f, 0.0f));
    CHECK(!sea_water_at(t, -50000.0f, 1000.0f, 100.0f));                       // above the plane: never water
    // the tile boundary: world x = 0 is tile column 128 (land); one tile cell (2 * cell) further west is column 127 (sea)
    CHECK(!sea_water_at(t, 1.0f, 0.0f, 0.0f) && sea_water_at(t, -2.0f * t.cell + 1.0f, 0.0f, 0.0f));
    // the env water branch (water:1) uses the same test
    {
        StageData te = t; add_synthetic_env(te); std::vector<float> tex; build_env_texture(te, Conventions{}, tex);
        const EnvView ev{tex.data(), kEnvWater | kEnvSky};
        CHECK(trace_env(te, a, ev, ow, dn, 20000, 58000).water && !trace_env(te, a, ev, oe, dn, 20000, 58000).water);
    }
    // the old rule stays available: seamask:0 makes every cell at the plane water again (the v03a picture)
    StageData old = t; Conventions c0; c0.sea_mask = false; apply_sea_convention(old, c0);
    CHECK(!old.sea_masked && trace(old, a, oe, dn, 20000, 58000).water && trace(old, a, ow, dn, 20000, 58000).water);
    Conventions c1; apply_sea_convention(old, c1); CHECK(old.sea_masked);       // the default re-derives the mask
    Conventions coff; coff.sea_off = true; StageData dry = t; apply_sea_convention(dry, coff);
    CHECK(!dry.sea_present() && !trace(dry, a, ow, dn, 20000, 58000).water);    // sea=off still removes the plane
    // ground below the plane on a LAND tile: the march still stops at the plane (height_effective is unchanged), the hit is shaded as land
    {
        StageData lo = flat_stage(2, true, 400.0f);
        for (uint32_t j = 0; j < kTGrid; ++j) for (uint32_t i = 0; i < kTGrid / 2; ++i) lo.words[size_t(j) * kTGrid + i] |= kTileSeaBit;
        apply_sea_convention(lo, Conventions{}); CHECK(lo.sea_masked);
        Atlas al; build_atlas(lo, al);
        const float o2[3] = {50000.0f, 1400.0f, 1000.0f};
        const Trace r = trace(lo, al, o2, dn, 20000, 58000);
        CHECK(r.hit && !r.water && std::fabs(r.p[1] - 400.0f) < 1.0f);
    }
    // the threshold (stage_baker.sea_rule: >= 1% of the tile words): 655 of 65536 words keep the old rule, 656 mask
    {
        StageData few = flat_stage(0, true, 0.0f);
        for (size_t i = 0; i < 655; ++i) few.words[i] |= kTileSeaBit;
        apply_sea_convention(few, Conventions{});
        CHECK(!sea_tiles_flagged(few) && !few.sea_masked);
        Atlas af; build_atlas(few, af);
        CHECK(trace(few, af, oe, dn, 20000, 58000).water);                     // no (real) sea tiles: the old rule, unchanged
        few.words[655] |= kTileSeaBit; apply_sea_convention(few, Conventions{});
        CHECK(sea_tiles_flagged(few) && few.sea_masked);
    }
    // T4-SEA (m03k 'lighter blue-grey rectangles'): a GUESSED plane on a stage without bit-13 sea tiles (the pre-STAGE-SWEEP bakes of the T4 group / m06a, header
    // flags 11) is the false sea of stage_baker.sea_rule: removed at load. seamask:0 keeps it (old rule); sea=<units> sets it; a non-guessed plane and a stage
    // with sea tiles are unchanged.
    {
        StageData g = flat_stage(0, true, 0.0f); g.flags |= kFlagSeaGuess;
        Atlas ag; build_atlas(g, ag);
        StageData g1 = g; apply_sea_convention(g1, Conventions{});
        CHECK(!g1.sea_present() && !g1.sea_masked && !trace(g1, ag, oe, dn, 20000, 58000).water);   // no water rectangles on land at height 0
        StageData g0 = g; Conventions c0; c0.sea_mask = false; apply_sea_convention(g0, c0);
        CHECK(g0.sea_present() && trace(g0, ag, oe, dn, 20000, 58000).water);                      // seamask:0: the old rule, the plane kept
        StageData gs = g; Conventions cs; cs.sea_override_set = true; cs.sea_override = 0.0f; apply_sea_convention(gs, cs);
        CHECK(gs.sea_present() && trace(gs, ag, oe, dn, 20000, 58000).water);                      // an explicit sea=0 keeps a plane
        StageData gt = g; for (size_t i = 0; i < 656; ++i) gt.words[i] |= kTileSeaBit; apply_sea_convention(gt, Conventions{});
        CHECK(gt.sea_present() && gt.sea_masked);                                                   // real sea tiles: the plane stays (masked)
        StageData gn = flat_stage(0, true, 0.0f); apply_sea_convention(gn, Conventions{});
        CHECK(gn.sea_present() && !gn.sea_masked);                                                  // a plane that is not a guess: unchanged
        std::string i2, w2; Conventions cp; CHECK(parse_stage_option("m03k", i2, cp, w2));
        StageData gp = g; apply_sea_convention(gp, cp); CHECK(!gp.sea_present());                  // the parsed defaults apply the rule
    }
}
// TAKEOFF (docs/world-render/takeoff-stage.md): the mission/airbase stage selector, on synthetic stages shaped like k00a/m02a and the test-19 path.
using TEv = TakeoffSelector::Event;
static TEv feed(TakeoffSelector& t, const StageData& m, const StageData& b, float x, float y, float z, int n, TEv* first_ev = nullptr) {
    TEv last = TEv::none;
    for (int i = 0; i < n; ++i) { const float e[3] = {x, y, z}; const TEv ev = t.update(e, m, b); if (ev != TEv::none) { last = ev; if (first_ev && *first_ev == TEv::none) *first_ev = ev; } }
    return last;
}
static void test_takeoff() {
    {   // option
        std::string id, why; Conventions c;
        CHECK(parse_stage_option("m02a", id, c, why) && std::string(c.takeoff_id) == "k00a");          // default on, k00a
        CHECK(parse_stage_option("auto;takeoff:off", id, c, why) && c.takeoff_id[0] == 0);
        CHECK(parse_stage_option("m02a;takeoff:0", id, c, why) && c.takeoff_id[0] == 0);
        CHECK(parse_stage_option("m02a;takeoff:v01a;env:1", id, c, why) && std::string(c.takeoff_id) == "v01a");
        CHECK(!parse_stage_option("m02a;takeoff:..\\x", id, c, why) && !why.empty());
        CHECK(!parse_stage_option("m02a;takeoff:toolongid", id, c, why));
    }
    // base: hills at byte 43 (3440) with a flat runway plateau at byte 23 (1840) over x 13000..19000, z 10000..30000 (k00a); mission: flat byte 6 (480, m02a there)
    StageData base = flat_stage(43), mission = flat_stage(6);
    for (int j = 0; j < 513; ++j) for (int i = 0; i < 513; ++i) {
        const float x = float(i) * 1024.0f - 262144.0f, z = float(j) * 1024.0f - 262144.0f;
        if (x >= 12000 && x <= 20000 && z >= 9000 && z <= 31000) base.heights[size_t(j) * 513 + i] = 23;
    }
    {   // test 19: parked on the runway, the roll and climb, the cut to the mission area, the frozen mission start, flight, a restart, a crash-low leave
        TakeoffSelector t;
        CHECK(feed(t, mission, base, 17110.39f, 1851.91f, 21311.87f, 29) == TEv::none && !t.base_active());   // 29 parked samples: not yet
        CHECK(feed(t, mission, base, 17110.39f, 1851.91f, 21311.87f, 1) == TEv::enter && t.base_active() && t.enters == 1);
        CHECK(std::strcmp(t.reason(), "parked-on-base-ground") == 0 && std::fabs(t.base_ground() - 1840.0f) < 0.01f && std::fabs(t.mission_ground() - 480.0f) < 0.01f);
        float z = 21311.87f, y = 1851.91f, v = 0.4f;
        // lift-off at sample 200, climbing 8 units/sample: clears the synthetic 3440 hills past the runway end (z < 9000) reached near sample 520
        // (the previous 3/sample path from sample 300 hit them at y 2545 = a crash, correctly left as under-base-ground)
        for (int i = 0; i < 600; ++i) { z -= v; v = std::min(v + 0.1f, 35.0f); if (i > 200) y += 8.0f; const float e[3] = {17110.4f, y, z}; CHECK(t.update(e, mission, base) == TEv::none); }
        CHECK(t.base_active());                                                                            // rolling + climbing: still the base
        CHECK(feed(t, mission, base, -33547.81f, 4003.5f, 141208.0f, 1) == TEv::leave && !t.base_active() && std::strcmp(t.reason(), "cut") == 0 && t.jumps == 1);
        CHECK(feed(t, mission, base, -33547.81f, 4003.5f, 141208.0f, 200) == TEv::none);                   // frozen mission start (2.5 s): stays the mission
        CHECK(feed(t, mission, base, -33547.81f, 3452.0f, 141208.0f, 200) == TEv::none && !t.base_active()); // even at "cockpit height" over base hills: disarmed
        for (int i = 0; i < 400; ++i) { const float e[3] = {-21633.0f + 2.0f * float(i), 3460.0f, 116516.0f}; CHECK(t.update(e, mission, base) == TEv::none); }   // slow + low
        CHECK(feed(t, mission, base, 17110.39f, 1851.91f, 21311.87f, 30) == TEv::enter && t.enters == 2);   // mission restart: back on the runway, re-armed
        CHECK(feed(t, mission, base, 17110.39f, 1700.0f, 21000.0f, 1) == TEv::leave && std::strcmp(t.reason(), "under-base-ground") == 0);
        CHECK(feed(t, mission, base, 17110.39f, 1851.91f, 21311.87f, 100) == TEv::none && !t.base_active()); // disarmed until a cut
        CHECK(t.leaves == 2 && t.refused == 0);
    }
    {   // a session that starts in flight (the null-driver mission start), mid-roll, or with the base map identical to the mission: never the base
        TakeoffSelector a; CHECK(feed(a, mission, base, 54127.09f, 9144.02f, -48803.62f, 300) == TEv::none && a.enters == 0);
        TakeoffSelector r;
        for (int i = 0; i < 200; ++i) { const float e[3] = {17110.4f, 1851.9f, 21311.0f - 20.0f * float(i)}; CHECK(r.update(e, mission, base) == TEv::none); }
        TakeoffSelector s; CHECK(feed(s, base, base, 17110.39f, 1851.91f, 21311.87f, 100) == TEv::none);   // the mission ground explains it
        TakeoffSelector h; CHECK(feed(h, mission, base, 17110.39f, 1851.91f + 250.0f, 21311.87f, 100) == TEv::none);   // 250 over the runway: not parked
    }
    {   // unusable input: refused, never an event, never a crash
        TakeoffSelector t; StageData empty;
        const float e[3] = {17110.39f, 1851.91f, 21311.87f}, bad[3] = {NAN, 1851.91f, 0};
        for (int i = 0; i < 40; ++i) CHECK(t.update(e, mission, empty) == TEv::none && t.update(bad, mission, base) == TEv::none && t.update(nullptr, mission, base) == TEv::none);
        CHECK(t.refused == 120 && !t.base_active());
    }
    {   // the installed bakes (if present): the test-19 path against the real k00a / m02a
        const char* la = std::getenv("LOCALAPPDATA");
        const std::string dir = std::string(la ? la : "") + "\\acz-vr\\stage";
        StageData k, m;
        if (!la || load_stage_file(stage_path("k00a", dir), k) != LoadStatus::ok || load_stage_file(stage_path("m02a", dir), m) != LoadStatus::ok) {
            std::printf("  (no installed k00a/m02a in %s: real takeoff replay skipped)\n", dir.c_str());
            return;
        }
        CHECK(std::fabs(height_bilinear(k, 17110.4f, 21311.9f) - 1840.0f) < 1.0f && std::fabs(height_bilinear(m, 17110.4f, 21311.9f) - 480.0f) < 1.0f);
        TakeoffSelector t; TEv first = TEv::none;
        CHECK(feed(t, m, k, 17110.39f, 1851.91f, 21311.87f, 40, &first) == TEv::enter && first == TEv::enter && t.base_active());
        const float roll[][3] = {{17110.40f, 1851.91f, 21307.76f}, {17110.40f, 1851.91f, 21294.77f}, {17110.40f, 1851.92f, 21243.08f}, {17110.40f, 1851.91f, 21211.96f}};
        for (const auto& p : roll) CHECK(t.update(p, m, k) == TEv::none);
        CHECK(feed(t, m, k, -33547.81f, 4003.5f, 141208.0f, 1) == TEv::leave && !t.base_active());
        CHECK(feed(t, m, k, -33547.81f, 4003.5f, 141208.0f, 200) == TEv::none);
        const float flight[][3] = {{-21633.54f, 2360.89f, 116516.93f}, {-12141.68f, 2941.94f, 109483.23f}, {16840.46f, 719.40f, 83351.11f}, {14963.75f, 5268.27f, 15481.58f}};
        for (const auto& p : flight) CHECK(feed(t, m, k, p[0], p[1], p[2], 100) == TEv::none || !t.base_active());
        CHECK(!t.base_active() && t.enters == 1);
        TakeoffSelector n; CHECK(feed(n, m, k, 54127.09f, 9144.02f, -48803.62f, 300) == TEv::none && !n.base_active());
    }
}
// K00A-SNOW (docs/world-render/k00a-snow-clut.md): the terrain CLUT is CSM1-ordered; tclut:raw restores the old identity decode for an A/B capture.
static void test_clut_csm1() {
    // the CSM1 permutation: an involution swapping index bits 3 and 4
    for (int i = 0; i < 256; ++i) { const uint8_t e = clut_entry(uint8_t(i)); CHECK(clut_entry(e) == i && clut_entry(uint8_t(i), true) == i); CHECK((e & 0xE7) == (i & 0xE7)); }
    CHECK(clut_entry(8) == 16 && clut_entry(16) == 8 && clut_entry(24) == 24 && clut_entry(7) == 7 && clut_entry(40) == 48 && clut_entry(111) == 119);
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("k00a", id, c, why) && !c.clut_raw);
    CHECK(parse_stage_option("k00a;tclut:raw", id, c, why) && c.clut_raw);
    CHECK(parse_stage_option("k00a;tclut:raw;tclut:csm1", id, c, why) && !c.clut_raw);
    CHECK(!parse_stage_option("k00a;tclut:1", id, c, why) && !parse_stage_option("k00a;tclut:", id, c, why));
    // atlas: texel 8 shows entry 16 (csm1) or entry 8 (raw); texels with bits 3 == 4 are unchanged
    StageData s = make_synthetic_stage(1, false);
    for (uint32_t c2 = 0; c2 < 256; ++c2) { s.clut[c2 * 4] = uint8_t(c2); s.clut[c2 * 4 + 1] = 0; s.clut[c2 * 4 + 2] = uint8_t(255 - c2); }
    s.tiles[0] = 8; s.tiles[1] = 16; s.tiles[2] = 24; s.tiles[3] = 111;
    Atlas a, r; build_atlas(s, a); build_atlas(s, r, true);
    CHECK(a.level[0][0] == 16 && a.level[0][4] == 8 && a.level[0][8] == 24 && a.level[0][12] == 119 && a.level[0][14] == 255 - 119);
    CHECK(r.level[0][0] == 8 && r.level[0][4] == 16 && r.level[0][8] == 24 && r.level[0][12] == 111);
    // real bakes (if installed): the CLUT luminance run is smoother in CSM1 order on every stage present (the evidence for the swap), and m02a's
    // river key texel resolves to the CLUT's translucent entry (alpha 0x40) instead of the opaque red-brown one
    const char* la = std::getenv("LOCALAPPDATA");
    const std::string dir = std::string(la ? la : "") + "\\acz-vr\\stage";
    unsigned seen = 0;
    for (const char* sid : {"k00a", "m01a", "m02a", "m07a", "m15a"}) {
        StageData t;
        if (!la || load_stage_file(stage_path(sid, dir), t) != LoadStatus::ok) continue;
        ++seen;
        double tv_raw = 0, tv_csm = 0;
        auto lum = [&](int i, bool raw) { const uint8_t e = clut_entry(uint8_t(i), raw); return (double(t.clut[e * 4]) + t.clut[e * 4 + 1] + t.clut[e * 4 + 2]) / 3.0; };
        for (int i = 1; i < 256; ++i) { tv_raw += std::fabs(lum(i, true) - lum(i - 1, true)); tv_csm += std::fabs(lum(i, false) - lum(i - 1, false)); }
        std::printf("  %s CLUT luminance run: raw %.0f, csm1 %.0f\n", sid, tv_raw, tv_csm);
        CHECK(tv_csm < tv_raw * 0.9);
        if (std::string(sid) == "m02a" && t.has_water()) {
            const uint8_t e = clut_entry(uint8_t(t.water_key));
            std::printf("  m02a water key %u: raw entry (%u,%u,%u,a%u), csm1 entry %u (%u,%u,%u,a%u)\n", t.water_key, t.clut[t.water_key * 4], t.clut[t.water_key * 4 + 1],
                t.clut[t.water_key * 4 + 2], t.clut[t.water_key * 4 + 3], e, t.clut[e * 4], t.clut[e * 4 + 1], t.clut[e * 4 + 2], t.clut[e * 4 + 3]);
            CHECK(t.clut[e * 4 + 3] == 0x40 && t.clut[t.water_key * 4 + 3] == 0x80);
        }
    }
    if (!seen) std::printf("  (no installed stage bakes in %s: real CLUT order skipped)\n", dir.c_str());
}
// TAKEOFF-LOOK (docs/world-render/takeoff-stage.md section 'look'): the takeoff stage draws with the mission's environment (toenv:, default mission) and
// k00a's look gain is neutral; m02a's env texture is bit-identical to before (kLookGain). Real bakes (if installed): k00a's own sky is brown (R > B in all
// 12 stops), m02a's is not; k00a's snow under the new look stays neutral, the old gain made it khaki.
static void test_takeoff_look() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && !c.takeoff_env_own);
    CHECK(parse_stage_option("m02a;toenv:own", id, c, why) && c.takeoff_env_own);
    CHECK(parse_stage_option("m02a;toenv:own;toenv:mission", id, c, why) && !c.takeoff_env_own);
    CHECK(!parse_stage_option("m02a;toenv:1", id, c, why) && !parse_stage_option("m02a;toenv:", id, c, why));
    float g[3];
    look_gain_for("m02a", g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kLookGain[ch]);
    look_gain_for("m07a", g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kLookGain[ch]);
    look_gain_for(nullptr, g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kLookGain[ch]);
    // SNOW-LOOK: k00a = neutral 0.90 + flat sky (sph +0x40) + haze floor; m01a/m15a neutral (kLookGain's luminance), own sky, no floor; m02a none
    look_gain_for("k00a", g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kK00aGain[ch]);
    look_gain_for("m01a", g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kLookGainSnowFit);
    look_gain_for("m15a", g); for (int ch = 0; ch < 3; ++ch) CHECK(g[ch] == kLookGainSnowFit);
    CHECK(std::fabs(0.2126f * kLookGain[0] + 0.7152f * kLookGain[1] + 0.0722f * kLookGain[2] - kLookGainSnowFit) < 0.01f);
    { const StageLook a = stage_look("m02a"), k = stage_look("k00a"), s = stage_look("m15a");
      CHECK(!a.sky_set && a.haze_floor == 0.0f && k.sky_set && k.haze_floor == kK00aHazeFloor && !s.sky_set && s.haze_floor == 0.0f);
      for (int ch = 0; ch < 3; ++ch) CHECK(k.sky[ch] == kK00aSky[ch]); }
    // white snow under each look: old kLookGain khaki (R - B 21/255), the snow profiles neutral
    { const float snow[3] = {154 / 255.0f, 156 / 255.0f, 156 / 255.0f}; float o[3], n[3];
      look_gain_for("m02a", o); look_gain_for("m01a", n);
      const float old_rb = (snow[0] * o[0] - snow[2] * o[2]) * 255, new_rb = (snow[0] * n[0] - snow[2] * n[2]) * 255;
      std::printf("  m01a white snow (154,156,156): kLookGain R-B %+.1f/255 (khaki), snow profile %+.1f/255\n", old_rb, new_rb);
      CHECK(old_rb > 20.0f && std::fabs(new_rb) < 2.0f); }
    // synthetic: mission env adopted (params + maps), own env kept with toenv:own, nothing without a mission env
    Conventions on; on.look = true;
    StageData mis = make_synthetic_stage(1, false); add_synthetic_env(mis, 9000.0f); std::memcpy(mis.id, "m02a", 5);
    StageData tko = make_synthetic_stage(2, false); add_synthetic_env(tko, 5000.0f); std::memcpy(tko.id, "k00a", 5);
    for (size_t i = 0; i < tko.env_params.size(); ++i) tko.env_params[i] *= 0.5f;   // a different (darker) own sky
    std::vector<float> tm, tk0, tk1;
    build_env_texture(mis, on, tm);
    { StageData own = tko; Conventions oc = on; oc.takeoff_env_own = true; CHECK(!adopt_mission_env(own, mis, oc) && own.env_params == tko.env_params); }
    { StageData none = make_synthetic_stage(3, false); StageData t2 = tko; CHECK(!adopt_mission_env(t2, none, on) && t2.env_params == tko.env_params); }
    CHECK(!adopt_mission_env(mis, mis, on));
    build_env_texture(tko, on, tk0);
    CHECK(adopt_mission_env(tko, mis, on) && tko.has_env() && tko.env_params == mis.env_params && tko.env_cloud == mis.env_cloud && tko.env_sprite_a == mis.env_sprite_a);
    build_env_texture(tko, on, tk1);
    size_t dm = 0, d0 = 0;
    for (size_t i = 0; i < tm.size(); ++i) { if (tk1[i] != tm[i]) ++dm; if (tk1[i] != tk0[i]) ++d0; }
    // = the mission's env texture except the k00a profile: 3 gain channels, the 12 sky stops (rgb) and the fog F (rgb + floor)
    size_t dmo = 0;
    for (size_t i = 0; i < tm.size(); ++i) {
        const size_t p = i / 4, ch = i % 4;
        const bool prof = p == kPLook || p == kPLookFog || p == kPLookSky2 || p == kPLookFar || p == kPLookFar2 || (p >= kPSky && p < kPSky + 12u && ch < 3);
        if (tk1[i] != tm[i] && !prof) ++dmo;
    }
    CHECK(dmo == 0 && dm > 3 && d0 > 20);
    for (int ch = 0; ch < 3; ++ch) CHECK(tk1[kPLook * 4 + ch] == kK00aGain[ch] && tm[kPLook * 4 + ch] == kLookGain[ch]);
    CHECK(tk1[kPLookFog * 4 + 3] == kK00aHazeFloor && tm[kPLookFog * 4 + 3] == 0.0f);
    // K00A-FIT2: only the k00a profile lets the propshade:game props take the look gain (P[29].w); every other stage's texture stays as before (w = 0)
    CHECK(tk1[kPLookSky2 * 4 + 3] == kK00aPropsGain && tm[kPLookSky2 * 4 + 3] == 0.0f);
    CHECK(props_look_on(EnvView{tk1.data(), kEnvAll}) && !props_look_on(EnvView{tm.data(), kEnvAll}) && !props_look_on(EnvView{tk1.data(), 0u}));
    CHECK(stage_look("k00a").props_gain == kK00aPropsGain && stage_look("m02a").props_gain == 0.0f && stage_look("m01a").props_gain == 0.0f && stage_look("m15a").props_gain == 0.0f);
    { // the live props ground: albedo (50.6,44.8,33.6)/255 at ~290 units (pf 0.0063) -> game (58,53,52) within 6/255 (step113/117 calibration)
      float c[3] = {50.6f / 255, 44.8f / 255, 33.6f / 255}; const EnvView e{tk1.data(), kEnvAll};
      look_gain_props(e, c); const float f = look_haze(e, 290.0f / 46080.0f);
      for (int ch = 0; ch < 3; ++ch) c[ch] += (tk1[kPLookFog * 4 + ch] - c[ch]) * f;
      const float tg[3] = {58, 53, 52};
      std::printf("  k00a props ground (%.0f,%.0f,%.0f) [game (58,53,52)]\n", c[0] * 255, c[1] * 255, c[2] * 255);
      for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(c[ch] * 255 - tg[ch]) < 6); }
    { // W8-2 P2: white albedo -> red gain 1.30 x trim 0.92 = 1.196 unclamped; the shader saturates before the fog blend, so the CPU mirror must too
      float w[3] = {1.0f, 1.0f, 1.0f}; const EnvView e{tk1.data(), kEnvAll};
      look_gain_props(e, w);
      for (int ch = 0; ch < 3; ++ch) CHECK(w[ch] >= 0.0f && w[ch] <= 1.0f);
      CHECK(w[0] == 1.0f);
      float hi[3] = {2.0f, 1.5f, -0.5f}; look_gain_props(e, hi);   // out-of-range input is saturated first
      for (int ch = 0; ch < 3; ++ch) CHECK(hi[ch] >= 0.0f && hi[ch] <= 1.0f);
      float off[3] = {2.0f, 0.5f, 0.1f}; look_gain_props(EnvView{tk1.data(), 0u}, off);   // disabled path stays a no-op
      CHECK(off[0] == 2.0f && off[1] == 0.5f && off[2] == 0.1f); }
    // the k00a sky is flat = sph +0x40 at every elevation/azimuth (also below the horizon: the haze target), with either env
    {
        const EnvView ev{tk1.data(), kEnvAll};
        std::vector<float> tk0p; { StageData own = make_synthetic_stage(2, false); add_synthetic_env(own, 5000.0f); std::memcpy(own.id, "k00a", 5); build_env_texture(own, on, tk0p); }
        const EnvView eo{tk0p.data(), kEnvAll};
        for (float el : {-10.0f, -2.0f, 0.0f, 1.0f, 3.0f, 6.0f, 12.0f, 25.0f, 60.0f})
            for (float az : {0.0f, 1.5f, 3.0f}) {
                const float r = el * 3.14159265f / 180.0f, d[3] = {std::sin(az) * std::cos(r), std::sin(r), -std::cos(az) * std::cos(r)};
                float s1[3], s2[3]; env_haze(ev, d, s1); env_haze(eo, d, s2);
                for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(s1[ch] - kK00aSky[ch]) < 1e-5f && std::fabs(s2[ch] - kK00aSky[ch]) < 1e-5f);
            }
        // haze floor: f' = floor + (1 - floor) f with the k00a texture; unchanged on m02a and without an env bit
        CHECK(std::fabs(look_haze(ev, 0.0f) - kK00aHazeFloor) < 1e-7f && std::fabs(look_haze(ev, 1.0f) - 1.0f) < 1e-7f &&
              std::fabs(look_haze(ev, 0.5f) - (kK00aHazeFloor + (1 - kK00aHazeFloor) * std::pow(0.5f, kK00aHazePow))) < 1e-6f);   // K00A-FIT exponent
        CHECK(look_haze(EnvView{tm.data(), kEnvAll}, 0.3f) == 0.3f && look_haze(EnvView{tk1.data(), 0u}, 0.3f) == 0.3f);
        // K00A-FIT3: far-haze tint bump (0 at f = 0, 1 at f0, 0 from f1); k00a only; no-op elsewhere and without an env bit
        for (int ch = 0; ch < 3; ++ch) CHECK(tk1[kPLookFar * 4 + ch] == kK00aFarTint[ch] && tm[kPLookFar * 4 + ch] == 0.0f);
        CHECK(tk1[kPLookFar * 4 + 3] == kK00aFarF0 && tk1[kPLookFar2 * 4] == kK00aFarF1 && tm[kPLookFar * 4 + 3] == 0.0f && tm[kPLookFar2 * 4] == 0.0f);
        const float sk[3] = {kK00aSky[0], kK00aSky[1], kK00aSky[2]};
        for (float fr : {0.0f, 0.0063f, 0.1f, kK00aFarF0, 0.4f, kK00aFarF1, 0.9f, 1.0f}) {
            float h[3] = {sk[0], sk[1], sk[2]}; look_haze_tint(ev, fr, h);
            const float w = fr <= 0 ? 0.0f : fr < kK00aFarF0 ? fr / kK00aFarF0 : fr < kK00aFarF1 ? 1.0f - (fr - kK00aFarF0) / (kK00aFarF1 - kK00aFarF0) : 0.0f;
            for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(h[ch] - (sk[ch] + (kK00aFarTint[ch] - sk[ch]) * w)) < 1e-6f);
            float h2[3] = {sk[0], sk[1], sk[2]}; look_haze_tint(EnvView{tm.data(), kEnvAll}, fr, h2); look_haze_tint(EnvView{tk1.data(), 0u}, fr, h2);
            for (int ch = 0; ch < 3; ++ch) CHECK(h2[ch] == sk[ch]);
        }
        { float h[3] = {sk[0], sk[1], sk[2]}; look_haze_tint(ev, 1.0f, h); for (int ch = 0; ch < 3; ++ch) CHECK(h[ch] == sk[ch]); }   // the far horizon meets the sky
        { float h[3] = {sk[0], sk[1], sk[2]}; look_haze_tint(ev, 0.0063f, h);   // near props ground: < 4% of the tint, < 2.5 levels
          for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs(h[ch] - sk[ch]) * 255 < 2.5f); }
        // the FIT3 mountain bin: unit colour (65.6,63,55.7) at raw f 0.205 through the full model -> CPU target (112,105,104): live A1 with W (131,131,134) read (92,93,99) for CPU (102,98,97) (ratio 0.90,0.95,1.02), game (99,99,104)
        { float c[3] = {65.6f / 255, 63.0f / 255, 55.7f / 255}; look_gain(ev, c); const float fr = 0.205f, f = look_haze(ev, fr);
          float h[3] = {sk[0], sk[1], sk[2]}; look_haze_tint(ev, fr, h);
          const float tgt[3] = {112, 105, 104};
          std::printf("  k00a FIT3 mountain bin CPU (%.0f,%.0f,%.0f) [target (112,105,104)]\n", (c[0] + (h[0] - c[0]) * f) * 255, (c[1] + (h[1] - c[1]) * f) * 255, (c[2] + (h[2] - c[2]) * f) * 255);
          for (int ch = 0; ch < 3; ++ch) CHECK(std::fabs((c[ch] + (h[ch] - c[ch]) * f) * 255 - tgt[ch]) < 4); }
    }
    // real bakes
    const char* la = std::getenv("LOCALAPPDATA");
    const std::string dir = std::string(la ? la : "") + "\\acz-vr\\stage";
    StageData k, m;
    if (!la || load_stage_file(stage_path("k00a", dir), k) != LoadStatus::ok || load_stage_file(stage_path("m02a", dir), m) != LoadStatus::ok || !k.has_env() || !m.has_env()) {
        std::printf("  (no installed k00a/m02a with env in %s: real takeoff look skipped)\n", dir.c_str());
        return;
    }
    unsigned k_rb = 0, m_rb = 0;
    for (uint32_t i = 0; i < 12; ++i) { const float* a = &k.env_params[size_t(kPSky + i) * 4]; const float* b = &m.env_params[size_t(kPSky + i) * 4]; k_rb += a[0] > a[2]; m_rb += b[0] > b[2]; }
    std::printf("  takeoff look: sky stops with R > B: k00a %u/12, m02a %u/12 (game shot R1b T10 sky (97,109,133): B > R)\n", k_rb, m_rb);
    CHECK(k_rb == 12 && m_rb <= 2);
    // the m02a mission env texture is bit-identical to the pre-change build (gain = kLookGain)
    std::vector<float> me; build_env_texture(m, on, me);
    for (int ch = 0; ch < 3; ++ch) CHECK(me[kPLook * 4 + ch] == kLookGain[ch]);
    // k00a's brightest (snow) texels: old gain khaki (R - B > 20/255 of display), the snow profile neutral (R - B ratio kept)
    Atlas ak; build_atlas(k, ak);
    double sr = 0, sg = 0, sb = 0; unsigned n = 0;
    for (uint32_t c2 = 0; c2 < 256; ++c2) {
        const float r = k.clut[c2 * 4] / 255.0f, gg = k.clut[c2 * 4 + 1] / 255.0f, b = k.clut[c2 * 4 + 2] / 255.0f;
        if (0.2126f * r + 0.7152f * gg + 0.0722f * b > 0.80f) { sr += r; sg += gg; sb += b; ++n; }
    }
    CHECK(n > 0);
    sr /= n; sg /= n; sb /= n;
    const double old_rb = (sr * kLookGain[0] - sb * kLookGain[2]) * 255, snl = 0.2126 * sr + 0.7152 * sg + 0.0722 * sb,
                 new_rb = ((sr + (snl - sr) * kK00aDesat) * kK00aGain[0] - (sb + (snl - sb) * kK00aDesat) * kK00aGain[2]) * 255;   // desat then per-channel gain (K00A-FIT2)
    std::printf("  k00a snow texel (%.0f,%.0f,%.0f): kLookGain R-B %+.1f/255, snow profile %+.1f/255\n", sr * 255, sg * 255, sb * 255, old_rb, new_rb);
    CHECK(old_rb > 20.0 && std::fabs(new_rb) < old_rb);
    // SNOW-LOOK numeric check against batch I (step111 F2, parked T10, game window): sky 5..25 deg (98..105,107..113,129..133); snowy mountains
    // (94,94,98) / (79,82,93); ground (57,53,52) / (37,36,37). The CPU reference from the real bakes at the logged parked eye, look:1, the k00a takeoff with
    // m02a's env adopted (toenv:mission, the default) and with its own (toenv:own): the sky is the profile colour with either env; the far snow ridges
    // (mean of the terrain hits beyond 3000 units, 0.5..5 deg, +-45 deg of the runway) are grey, not khaki/teal.
    {
        Conventions lk = on; lk.env_bits = kEnvAll;
        StageData km = k; CHECK(adopt_mission_env(km, m, lk));
        for (int variant = 0; variant < 2; ++variant) {
            const StageData& ks = variant ? k : km;
            std::vector<float> tex; build_env_texture(ks, lk, tex);
            const EnvView ev{tex.data(), kEnvAll};
            const float eye[3] = {17110.4f, 1851.9f, 21311.9f};
            const float up = 15.0f * 3.14159265f / 180.0f, dsk[3] = {0.0f, std::sin(up), -std::cos(up)};
            const Trace s0 = trace_env(ks, ak, ev, eye, dsk, lk.haze_l, lk.far_dist);
            double mr = 0, mg = 0, mb = 0, mt = 0; unsigned mn = 0;
            for (float az = -45.0f; az <= 45.0f; az += 3.0f)
                for (float el = 0.5f; el <= 5.0f; el += 0.5f) {
                    const float ra = az * 3.14159265f / 180.0f, re = el * 3.14159265f / 180.0f;
                    const float d[3] = {std::sin(ra) * std::cos(re), std::sin(re), -std::cos(ra) * std::cos(re)};
                    const Trace h = trace_env(ks, ak, ev, eye, d, lk.haze_l, lk.far_dist);
                    if (h.hit && h.t > 3000.0f) { mr += h.display[0]; mg += h.display[1]; mb += h.display[2]; mt += h.t; ++mn; }
                }
            const float e3 = -3.0f * 3.14159265f / 180.0f, dg[3] = {0.0f, std::sin(e3), -std::cos(e3)};
            const Trace g0 = trace_env(ks, ak, ev, eye, dg, lk.haze_l, lk.far_dist);
            std::printf("  k00a look (%s env): sky 15 deg (%.0f,%.0f,%.0f) [game (100,111,133)]; far ridges n=%u mean t=%.0f (%.0f,%.0f,%.0f) [game (94,94,98)/(79,82,93)]; "
                "ground -3 deg (%.0f,%.0f,%.0f) [game (57,53,52)/(37,36,37)]\n", variant ? "own" : "mission", s0.display[0] * 255, s0.display[1] * 255, s0.display[2] * 255,
                mn, mn ? mt / mn : 0.0, mn ? mr / mn * 255 : 0.0, mn ? mg / mn * 255 : 0.0, mn ? mb / mn * 255 : 0.0, g0.display[0] * 255, g0.display[1] * 255, g0.display[2] * 255);
            CHECK(s0.sky && std::fabs(s0.display[0] * 255 - 100) < 3 && std::fabs(s0.display[1] * 255 - 113) < 3 && std::fabs(s0.display[2] * 255 - 136) < 3);
            CHECK(mn > 10);
            if (mn) { mr /= mn; mg /= mn; mb /= mn;
                CHECK(mb >= mr - 6.0 / 255 && mg >= mr - 8.0 / 255);              // grey/blue-grey snow, not khaki (R > B) or brown
                CHECK(mr * 255 > 70 && mr * 255 < 115 && mb * 255 > 75 && mb * 255 < 120);
                // K00A-FIT: within ~6% of the K3 game ridges (mean of left (94,94,98), right (79,82,93) -> (86,88,95)) and the near ground within ~8% of (57,53,52), R-B <= 8
                CHECK(std::fabs(mr * 255 - 108) < 8 && std::fabs(mg * 255 - 103) < 8 && std::fabs(mb * 255 - 103) < 8); }   // K00A-FIT3: CPU ridge mean target ~(108,103,103) (ratio-corrected game ridges) = game ridges (97,96,101) / the live-to-CPU ratio of the three pairs
            // K00A-FIT2: the live ground beside the runway (props whose unit colour equals the terrain's there) is the ground-right bin (az 15..45, el -6..-2, ~190 units):
            // within 6/255 of the game (58,53,52) = the live-calibrated target (step113/117 pairs, docs section 7)
            { double gr = 0, gg2 = 0, gb = 0; unsigned gn = 0;
              for (float az = 15.0f; az <= 45.0f; az += 2.0f) for (float el = -6.0f; el <= -2.0f; el += 0.5f) {
                  const float ra = az * 3.14159265f / 180.0f, re = el * 3.14159265f / 180.0f;
                  const float d[3] = {std::sin(ra) * std::cos(re), std::sin(re), -std::cos(ra) * std::cos(re)};
                  const Trace h = trace_env(ks, ak, ev, eye, d, lk.haze_l, lk.far_dist);
                  if (h.hit) { gr += h.display[0]; gg2 += h.display[1]; gb += h.display[2]; ++gn; } }
              CHECK(gn > 50);
              if (gn) { gr = gr / gn * 255; gg2 = gg2 / gn * 255; gb = gb / gn * 255;
                std::printf("  k00a ground-right bin (%.0f,%.0f,%.0f) [game (58,53,52)]\n", gr, gg2, gb);
                CHECK(std::fabs(gr - 58) < 6 && std::fabs(gg2 - 53) < 6 && std::fabs(gb - 52) < 6); } }
            // near terrain straight ahead: in the game (and in our frames, tclut A/B) this pixel is covered by the airbase props; the terrain under
            // them stays a little warm (R-B ~11 with the floor; batch I ours R-B 15, game 5 / 0)
            CHECK(g0.hit && std::fabs(g0.display[0] - g0.display[2]) * 255 < 14);
        }
    }
}
// K00A-FIT diag (ACZ_LOOKFIT=1): pre-haze colour / haze fraction of ground and ridge samples at the parked eye, gain 1, floor 0.
static void diag_k00a_fit() {
    if (!std::getenv("ACZ_LOOKFIT")) return;
    const char* la = std::getenv("LOCALAPPDATA");
    const std::string dir = std::string(la ? la : "") + "\\acz-vr\\stage";
    StageData k, m;
    if (load_stage_file(stage_path("k00a", dir), k) != LoadStatus::ok || load_stage_file(stage_path("m02a", dir), m) != LoadStatus::ok) return;
    Conventions lk; lk.look = true; lk.env_bits = kEnvAll;
    Atlas ak; build_atlas(k, ak);
    StageData km = k; adopt_mission_env(km, m, lk);
    std::vector<float> tex; build_env_texture(km, lk, tex);
    for (int ch = 0; ch < 3; ++ch) tex[kPLook * 4 + ch] = 1.0f;
    tex[kPLookFar * 4 + 3] = 0.0f; tex[kPLookFog * 4 + 3] = 0.0f; tex[kPLookSky2 * 4 + 1] = 0.0f; tex[kPLookSky2 * 4 + 2] = 0.0f;
    const EnvView ev{tex.data(), kEnvAll};
    const float eye[3] = {17110.4f, 1851.9f, 21311.9f};
    const float* F = &tex[kPLookFog * 4];
    struct Bin { double c[3] = {0, 0, 0}, f = 0, t = 0; unsigned n = 0; };
    auto acc = [&](Bin& b, float az, float el) {
        const float ra = az * 3.14159265f / 180.0f, re = el * 3.14159265f / 180.0f;
        const float d[3] = {std::sin(ra) * std::cos(re), std::sin(re), -std::cos(ra) * std::cos(re)};
        const Trace h = trace_env(km, ak, ev, eye, d, lk.haze_l, lk.far_dist);
        if (!h.hit) return;
        const float f = haze_fraction(h.t, lk.haze_l, lk.far_dist);
        for (int c = 0; c < 3; ++c) b.c[c] += (h.display[c] - F[c] * f) / (1 - f);
        b.f += f; b.t += h.t; ++b.n;
    };
    auto pr = [&](const char* nm, const Bin& b) {
        if (!b.n) { std::printf("DIAG %s n=0\n", nm); return; }
        std::printf("DIAG %-14s n=%4u t=%7.0f f=%.3f unit-col (%.1f,%.1f,%.1f)\n", nm, b.n, b.t / b.n, b.f / b.n, b.c[0] / b.n * 255, b.c[1] / b.n * 255, b.c[2] / b.n * 255);
    };
    { Bin g; for (float az = 15; az <= 45; az += 2) for (float el = -6; el <= -2; el += 0.5f) acc(g, az, el); pr("ground-right", g); }
    { Bin g; for (float az = -45; az <= -15; az += 2) for (float el = -6; el <= -2; el += 0.5f) acc(g, az, el); pr("ground-left", g); }
    const float tb[] = {3000, 6000, 10000, 20000, 40000, 1e9f};
    for (int b = 0; b < 5; ++b) {
        Bin r;
        for (float az = -60; az <= 60; az += 1.5f) for (float el = 0.2f; el <= 6.0f; el += 0.2f) {
            const float ra = az * 3.14159265f / 180.0f, re = el * 3.14159265f / 180.0f;
            const float d[3] = {std::sin(ra) * std::cos(re), std::sin(re), -std::cos(ra) * std::cos(re)};
            const Trace h = trace_env(km, ak, ev, eye, d, lk.haze_l, lk.far_dist);
            if (h.hit && h.t >= tb[b] && h.t < tb[b + 1]) acc(r, az, el);
        }
        char nm[32]; std::snprintf(nm, sizeof nm, "ridge t%.0f-%.0f", tb[b], tb[b + 1] > 1e8f ? 99999.0f : tb[b + 1]); pr(nm, r);
    }
    std::printf("DIAG F=(%.1f,%.1f,%.1f) haze_l=%.0f far=%.0f\n", F[0] * 255, F[1] * 255, F[2] * 255, lk.haze_l, lk.far_dist);
}// DTEX (docs/world-render/detail-texture.md): the .zvdt format, the encoded zero-mean term and its mips, detail_term (tiling, fade, lod) and trace() with it.
static void test_detail() {
    // option
    { std::string id, why; Conventions c; CHECK(parse_stage_option("m02a", id, c, why) && !c.detail);
      CHECK(parse_stage_option("m02a;dtex:1", id, c, why) && c.detail);
      CHECK(parse_stage_option("m02a;dtex:1;dtex:0", id, c, why) && !c.detail);
      CHECK(!parse_stage_option("m02a;dtex:2", id, c, why)); }
    // format round trip + every refusal
    const DetailData d = make_synthetic_detail(7);
    const std::vector<uint8_t> b = serialize_detail(d);
    CHECK(b.size() == kZvdtBytes);
    { DetailData o; CHECK(parse_detail(b.data(), b.size(), o) == LoadStatus::ok); CHECK(std::strcmp(o.id, "synth") == 0 && o.rgba == d.rgba); CHECK(serialize_detail(o) == b); }
    { DetailData o; std::vector<uint8_t> x = b; x[0] = 'X'; CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_magic); }
    { DetailData o; std::vector<uint8_t> x = b; x[4] = 2; CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_version); }
    { DetailData o; std::vector<uint8_t> x = b; x[8] = 64; CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_header); }
    { DetailData o; std::vector<uint8_t> x = b; x[kZvdtHeader + 5] ^= 1; CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_hash); }
    { DetailData o; std::vector<uint8_t> x = b; x.pop_back(); CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_size); }
    { DetailData o; std::vector<uint8_t> x = b; x.push_back(0); CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::too_large); }
    { DetailData o; CHECK(parse_detail(b.data(), 10, o) == LoadStatus::too_small); CHECK(load_detail_file("Z:\\no\\such\\file.zvdt", o) == LoadStatus::io_error); }
    { DetailData o; std::vector<uint8_t> x = b; x[16] = '-'; CHECK(parse_detail(x.data(), x.size(), o) == LoadStatus::bad_header); }   // bad id (header is not hashed)
    CHECK(detail_path("k00a", "D:\\s") == "D:\\s\\k00a.zvdt" && detail_path("bad id", "D:\\s").empty());
    // attach: off by default, nothing attached
    { Atlas a; std::string why; Conventions c; CHECK(!attach_detail("k00a", c, a, why) && why == "off" && !a.has_detail()); }
    // encoding: 8 levels, zero mean (the 1x1 level is 128), alpha 255, a known texel decodes to amp * (g - mean)
    Atlas a; CHECK(build_detail(d, a) && a.has_detail());
    for (uint32_t L = 0; L < kDtLevels; ++L) CHECK(a.dt[L].size() == size_t(kDtDim >> L) * (kDtDim >> L) * 4);
    CHECK(a.dt[7][0] >= 127 && a.dt[7][0] <= 129 && a.dt[7][3] == 255);
    {
        double mean = 0; for (size_t i = 0; i < size_t(kDtDim) * kDtDim; ++i) mean += d.rgba[i * 4] * (0x40 / 128.0) * kDtGain / 255.0;
        mean /= double(kDtDim) * kDtDim;
        for (size_t i : {size_t(0), size_t(77), size_t(5000)}) {
            const double want = d.rgba[i * 4] * (0x40 / 128.0) * kDtGain / 255.0 - mean;
            CHECK(std::fabs((a.dt[0][i * 4] - 128.0) / 255.0 - want) <= 0.5 / 255.0 + 1e-6);
        }
    }
    { Atlas bad; DetailData x = d; x.rgba.resize(100); CHECK(!build_detail(x, bad) && !bad.has_detail()); }
    // detail_term: zero without detail, beyond the fade end and for NaN; non-zero near; tiles with kDtRepeat; fades
    float o[3];
    { Atlas none; detail_term(none, 10.0f, 20.0f, 50.0f, o); CHECK(o[0] == 0 && o[1] == 0 && o[2] == 0); }
    detail_term(a, 10.0f, 20.0f, kDtFadeFar, o); CHECK(o[0] == 0 && o[1] == 0 && o[2] == 0);
    detail_term(a, 10.0f, 20.0f, std::nanf(""), o); CHECK(o[0] == 0);
    double sabs = 0, ssum = 0; int n = 0;
    for (int k = 0; k < 400; ++k) {
        const float x = -5000.0f + 37.3f * float(k), z = 1234.5f - 11.7f * float(k);
        float p[3], q[3]; detail_term(a, x, z, 60.0f, p); detail_term(a, x + 2.0f * kDtRepeat, z - kDtRepeat, 60.0f, q);
        CHECK(std::fabs(p[0] - q[0]) < 2e-3f && p[0] == p[1] && p[1] == p[2]);   // grey CLUT: grey term; periodic in kDtRepeat
        sabs += std::fabs(p[0]); ssum += p[0]; ++n;
    }
    CHECK(sabs / n > 0.004 && std::fabs(ssum / n) < 0.004);   // a visible grain (> 1/255 mean |term|) with a near-zero mean
    { float p[3], q[3]; detail_term(a, 33.3f, 77.7f, 0.5f * (kDtFadeNear + kDtFadeFar), p); detail_term(a, 33.3f, 77.7f, 100.0f, q);
      CHECK(std::fabs(p[0]) < std::fabs(q[0]) + 1e-6f); }
    // trace(): a near land hit changes by exactly the clamped term (before the haze); a hit beyond the fade end is byte-identical; no detail = identical
    {
        StageData s = flat_stage(10);
        Atlas plain; build_atlas(s, plain);
        Atlas det = plain; CHECK(build_detail(d, det));
        const float eye[3] = {100.0f, 10.0f * s.hstep + 120.0f, 200.0f};
        float dn[3] = {0.3f, -1.0f, 0.2f}; norm3(dn);
        const Trace t0 = trace(s, plain, eye, dn, 55000.0f, 160000.0f), t1 = trace(s, det, eye, dn, 55000.0f, 160000.0f);
        CHECK(t0.hit && t1.hit && t0.t == t1.t);
        float dt[3]; detail_term(det, t1.p[0], t1.p[2], t1.t, dt);
        const float f = haze_fraction(t1.t, 55000.0f, 160000.0f);
        for (int c = 0; c < 3; ++c) {
            const float col0 = (t0.display[c] - kHaze[c] * f) / (1 - f);
            const float want = (std::min(std::max(col0 + dt[c], 0.0f), 1.0f)) * (1 - f) + kHaze[c] * f;
            CHECK(std::fabs(t1.display[c] - want) < 1e-4f);
        }
        float df[3] = {0.0f, -0.02f, -1.0f}; norm3(df);   // grazing: the hit is far beyond the fade end
        const Trace f0 = trace(s, plain, eye, df, 55000.0f, 160000.0f), f1 = trace(s, det, eye, df, 55000.0f, 160000.0f);
        CHECK(f0.hit && f0.t > kDtFadeFar && std::memcmp(f0.display, f1.display, sizeof f0.display) == 0);
    }
    // a real baked detail next to the stages, when installed (skipped otherwise)
    {
        DetailData r;
        const std::string p = detail_path("k00a");
        if (!p.empty() && load_detail_file(p, r) == LoadStatus::ok) {
            CHECK(std::strcmp(r.id, "k00a") == 0);
            Atlas ra; CHECK(build_detail(r, ra));
            std::printf("  real k00a.zvdt: alpha=%u texel0=%u\n", unsigned(r.rgba[3]), unsigned(r.rgba[0]));
        } else std::printf("  (no installed k00a.zvdt: real-detail check skipped)\n");
    }
}
int main() {
    test_detail();
    test_sha_and_roundtrip();
    test_takeoff();
    test_takeoff_look();
    diag_k00a_fit();
    test_clut_csm1();
    test_loader_errors();
    test_refl_sidecar();     // SKIN-REFL
    test_options();
    test_camera();
    test_roll_sign();
    test_sampling();
    test_trace();
    test_colour_haze();
    test_mailbox_and_config();
    test_water_format();
    test_water_shading();
    test_real_stage_if_present();
    test_env_format_and_options();
    test_env_shading();
    test_look();
    test_sea_mask();
    test_gamecloud();
    test_real_env_if_present();
    std::printf("terrain_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
