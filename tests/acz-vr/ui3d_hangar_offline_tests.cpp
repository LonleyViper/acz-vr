// UI3D stage 3b offline tests (docs/world-render/ui-3d.md 9): the hangar screens as a 3D room. Pure parts of ui3d_hangar.h (record checks,
// camera decomposition, room latch, per-eye matrix) on the REAL K10 records (step130 null run K10, ee-camera.log `ui3d-census;kind=acm-a`), and
// the PRODUCER's hangar frame kind (brief3d_live.h Producer::ui3d_acm + the hangar rules of ui3d_draw) with fakes. Nothing here touches D3D12,
// the game or a headset: fixture success says nothing about the live hangar.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d_hangar.h"
#include "brief3d_live.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::brief3d::live;
namespace u3 = acz::ui3d;
namespace hz = acz::ui3d::hangar;
using acz::vscreen::Completion;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

namespace {

// ================================================================================================== the real records (K10, A of the first ACM draw)
struct RealA { const char* tag; const char* a; double eye[3]; };
// tick | screen: aircraft select s1..s7 (127149386..127155537), PLAYER weapon s8..s10, WINGMAN weapon s1..s6 and the next screen s7..s10.
const RealA kReal[] = {
    {"127149386 aircraft s1", "808.52179,1085.18433,-142.494934,0.558786631,-443.357697,-909.892456,55.2049141,-0.21648325,1873.98059,1554.71289,-204.148453,0.800557733,258920.094,294018.25,16740630,144.465683", {-155.2, 27.5, -64.7}},
    {"127150411 aircraft s2", "1046.46313,1281.87622,-167.993103,0.658776402,-428.137177,-895.44519,53.3097229,-0.209051356,1756.0437,1406.28296,-184.296921,0.722710967,284829.062,315385.625,16737858,155.335571", {-163.2, 26.9, -58.4}},
    {"127155537 aircraft s7", "2034.84534,2066.98926,-254.661118,0.998640597,93.3442917,-384.025757,-11.6228132,0.0455782674,461.267944,52.3506699,-6.44980621,0.0252925865,384865.375,393146.656,16729406,188.479553", {-189.0, 6.3, -2.0}},
    {"127156570 weapon s8", "800.754822,971.829407,-139.397934,0.546641886,-1007.97119,-1423.95276,125.508057,-0.492173433,1644.64648,1204.39478,-172.756821,0.677457035,105163.766,108777.688,16764018,52.7503281", {-33.8, 27.3, -30.7}},
    {"127164420 wingman s1", "685.203613,885.060547,-127.096161,0.498401105,-1013.65942,-1428.8877,126.216331,-0.494950891,1692.65076,1263.96521,-181.507492,0.711772382,-34075.8516,-70572.0938,16789776,-48.2576027", {170.3, 27.5, -32.4}},
    {"127169586 wingman s6", "1935.25244,1943.26526,-249.43071,0.978129804,-260.516022,-734.500732,32.4382858,-0.127205089,740.950012,326.941467,-41.9650612,0.16456385,-290662.094,-289874.219,16815296,-148.329041", {154.0, 10.0, -6.6}},
    {"127170619 next s7", "2048.1709,1859.41321,-236.288986,0.926595211,-176.27597,-652.369263,21.9490948,-0.0860722512,-368.779999,-734.613892,93.3526611,-0.366077691,456331.406,452640.219,16721961,217.673889", {-210.0, 33.0, 55.3}},
    {"127171635 next s8", "2053.59741,1869.33374,-237.80304,0.932532489,-185.332352,-661.238342,23.0767536,-0.0904943123,-332.36557,-700.725708,89.1412277,-0.349562764,454922.375,451542.031,16722078,217.216019", {-210.0, 33.6, 52.5}},
};
void parse16(const char* s, float out[16]) {
    char* e = nullptr; const char* p = s;
    for (int i = 0; i < 16; ++i) { out[i] = std::strtof(p, &e); CHECK(e != p); p = e; if (*p == ',') ++p; }
}
// a hangar record: A (+0), B = identity (+256, as all 640 logged hangar draws), +360 = 1.0, +400 = (682,-448,2048,2048), the rest a pattern
void make_record(const float A[16], uint8_t rec[hz::kRecBytes], const float k400[4] = hz::kScreen) {
    for (unsigned i = 0; i < hz::kRecBytes; ++i) rec[i] = uint8_t(i * 7 + 3);
    std::memcpy(rec, A, 64);
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(rec + 256, I, 64);
    const float one = 1.0f; std::memcpy(rec + 360, &one, 4);
    std::memcpy(rec + 400, k400, 16);
}
void real_record(unsigned i, uint8_t rec[hz::kRecBytes]) { float A[16]; parse16(kReal[i].a, A); make_record(A, rec); }

// ================================================================================================== options
void test_option() {
    const std::string day = "auto;env:1;props:1;trees:1;vscreen:1;vsearly:1;brief3d:1;mk360:3";
    std::string rest, why; u3::Options o;
    CHECK(u3::extract_options(day + ";ui3d:1", rest, o, why) && rest == day && o.hangar == 0);
    CHECK(u3::describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");   // the stage-2 line unchanged without u3a
    CHECK(u3::extract_options(day + ";ui3d:1;u3a:1", rest, o, why) && rest == day && o.hangar == 1 && o.hangar_upm == 5.0f && o.any_token);
    CHECK(u3::describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3a=1;u3au=5.0;u3mn=1.10;u3mf=6.00;u3s=0.70");
    CHECK(u3::extract_options(day + ";ui3d:1;u3a:2;u3au:12.5", rest, o, why) && o.hangar == 2 && o.hangar_upm == 12.5f);
    CHECK(u3::describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3a=2;u3au=12.5;u3mn=1.10;u3mf=6.00;u3s=0.70");
    CHECK(!u3::extract_options(day + ";u3a:3", rest, o, why) && why == "bad value for u3a (0|1|2)");
    CHECK(!u3::extract_options(day + ";u3au:0.5", rest, o, why) && why == "bad value for u3au (1..200 units/m)");
    CHECK(std::string(day + ";ui3d:1;u3n:1.8;u3f:3.5;u3t:-12.5;u3c:1;u3p:1;u3a:2;u3au:12.5;u3mn:1.1;u3mf:6.0;u3s:0.7").size() < 255);   // the 255-character buffer
    Producer p; o = u3::Options{}; o.on = true; o.hangar = 1; p.configure_ui3d(o);
    CHECK(p.ui3d_hangar_configured());
    Producer q; o.hangar = 0; q.configure_ui3d(o);
    CHECK(!q.ui3d_hangar_configured());
}

// ================================================================================================== record checks
void test_record_check() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    CHECK(hz::record_check(rec) == nullptr);
    { uint8_t r2[hz::kRecBytes]; float A[16]; parse16(kReal[0].a, A); const float flight[4] = {512, -448, 2048, 2048}; make_record(A, r2, flight);
      CHECK(streq(hz::record_check(r2), "u3a-not-hangar")); }
    { uint8_t r2[hz::kRecBytes]; std::memcpy(r2, rec, sizeof rec); const float h = 2.0f; std::memcpy(r2 + 360, &h, 4); CHECK(streq(hz::record_check(r2), "u3a-record-depth-scale")); }
    { uint8_t r2[hz::kRecBytes]; std::memcpy(r2, rec, sizeof rec); const float h = 0.5f; std::memcpy(r2 + 256 + 12, &h, 4); CHECK(streq(hz::record_check(r2), "u3a-record-b-not-affine")); }
    { uint8_t r2[hz::kRecBytes]; std::memcpy(r2, rec, sizeof rec); const float h = std::nanf(""); std::memcpy(r2 + 20, &h, 4); CHECK(streq(hz::record_check(r2), "u3a-record-a-nonfinite")); }
    CHECK(hz::cb10_check(nullptr, 0x102) == nullptr && streq(hz::cb10_check(nullptr, 8), "u3a-cb10-fade") && streq(hz::cb10_check("x", 2), "x"));
    // same_a: bit-identical / a relative change above 1e-6 differs
    uint8_t r3[hz::kRecBytes]; std::memcpy(r3, rec, sizeof rec);
    CHECK(hz::same_a(r3, rec));
    float a0; std::memcpy(&a0, r3, 4); a0 *= 1.00001f; std::memcpy(r3, &a0, 4);
    CHECK(!hz::same_a(r3, rec));
}

// ================================================================================================== the camera from the real A
void test_decompose_real() {
    for (const RealA& r : kReal) {
        float A[16]; parse16(r.a, A);
        hz::Camera c;
        CHECK(hz::decompose(A, hz::kScreen, c) == nullptr && c.ok);
        CHECK(std::fabs(c.tanX - 0.8325) < 5e-4 && std::fabs(c.tanY - 0.4688) < 5e-4);   // [E] square pixels on 2560 x 1440
        CHECK(std::fabs(c.right[1]) < 1e-3 && c.up[1] > 0);
        CHECK(std::fabs(c.b + 255.008) < 0.01 && std::fabs(c.z0 - 16777470.0) < 50.0);
        for (int i = 0; i < 3; ++i) CHECK(std::fabs(c.eye[i] - r.eye[i]) < 0.15);
        // the eye is the camera centre: A maps it to w = 0
        double nx, ny, w, d; const double e[3] = {c.eye[0], c.eye[1], c.eye[2]};
        hz::shader_ndc(A, hz::kScreen, e, nx, ny, w, d);
        CHECK(std::fabs(w) < 1e-2);
    }
    // a flight constant set changes the lens (the k400 is part of the decomposition), a rolled / mirrored camera refuses
    float A[16]; parse16(kReal[0].a, A);
    hz::Camera c;
    float B[16]; std::memcpy(B, A, sizeof A); for (int r = 0; r < 4; ++r) B[r * 4 + 0] = -B[r * 4 + 0] + 4096.0f * B[r * 4 + 3];   // mirror x
    CHECK(streq(hz::decompose(B, hz::kScreen, c), "u3a-camera-handedness"));
    float C[16]; std::memcpy(C, A, sizeof A); for (int r = 0; r < 4; ++r) C[r * 4 + 3] *= 2.0f;   // w not unit
    CHECK(streq(hz::decompose(C, hz::kScreen, c), "u3a-camera-w-not-unit"));
    // a camera rolled by 20 degrees about its forward axis
    {
        hz::Camera c0; CHECK(hz::decompose(A, hz::kScreen, c0) == nullptr);
        const double t = 20.0 * 3.14159265358979 / 180.0;
        double R[3], U[3];
        for (int i = 0; i < 3; ++i) { R[i] = std::cos(t) * c0.right[i] + std::sin(t) * c0.up[i]; U[i] = -std::sin(t) * c0.right[i] + std::cos(t) * c0.up[i]; }
        float D[16]; std::memcpy(D, A, sizeof A);
        const double sx = 341.0 / c0.tanX, sy = 224.0 / c0.tanY;
        for (int i = 0; i < 3; ++i) { D[i * 4 + 0] = float(sx * R[i] + 2048.0 * D[i * 4 + 3]); D[i * 4 + 1] = float(-sy * U[i] + 2048.0 * D[i * 4 + 3]); }
        D[12] = float(-sx * hz::dot3(R, c0.eye) + 2048.0 * D[15]); D[13] = float(sy * hz::dot3(U, c0.eye) + 2048.0 * D[15]);
        CHECK(streq(hz::decompose(D, hz::kScreen, c), "u3a-camera-roll"));
    }
}

void test_cut() {
    hz::Camera a, b, c;
    float A[16];
    parse16(kReal[2].a, A); CHECK(hz::decompose(A, hz::kScreen, a) == nullptr);   // aircraft s7
    parse16(kReal[3].a, A); CHECK(hz::decompose(A, hz::kScreen, b) == nullptr);   // weapon s8 (next screen)
    CHECK(hz::cut(a, b));
    parse16(kReal[6].a, A); CHECK(hz::decompose(A, hz::kScreen, b) == nullptr);   // next s7
    parse16(kReal[7].a, A); CHECK(hz::decompose(A, hz::kScreen, c) == nullptr);   // next s8, one second later (orbit: 2.9 units, ~1 degree)
    CHECK(!hz::cut(b, c));
    parse16(kReal[5].a, A); CHECK(hz::decompose(A, hz::kScreen, a) == nullptr);   // wingman s6 -> next s7: a cut
    CHECK(hz::cut(a, b));
}

// ================================================================================================== the per-eye matrix
void quat_mul(const double a[4], const double b[4], double o[4]) {
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
}
void yaw_pitch(double yaw, double pitch, double q[4]) {
    const double qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, qx[4] = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    quat_mul(qy, qx, q);
}
// With the eye AT the latched head, looking with the game camera's pitch through the game lens, A' reproduces A exactly (any head yaw, any anchor).
void test_eye_matrix_reproduces_game_camera() {
    for (const RealA& r : kReal) {
        float A[16]; parse16(r.a, A);
        hz::Camera c; CHECK(hz::decompose(A, hz::kScreen, c) == nullptr);
        for (double yaw : {0.0, 0.7, -2.4}) {
            const double head[3] = {0.3, 1.6, -0.2};
            double qh[4]; yaw_pitch(yaw, 0.0, qh);
            hz::Room room; CHECK(hz::make_room(c, head, qh, 5.0, room) == nullptr);
            hz::Eye e; for (int i = 0; i < 3; ++i) e.p[i] = head[i];
            yaw_pitch(yaw, std::asin(c.fwd[1]), e.q);
            e.fov[0] = -std::atan(c.tanX); e.fov[1] = std::atan(c.tanX); e.fov[2] = std::atan(c.tanY); e.fov[3] = -std::atan(c.tanY);
            float Ae[16]; CHECK(hz::eye_matrix(room, c, hz::kScreen, e, Ae) == nullptr);
            for (double d : {5.0, 40.0, 300.0}) for (double a : {-0.6, 0.0, 0.5}) for (double b : {-0.4, 0.3}) {
                double p[3]; for (int i = 0; i < 3; ++i) p[i] = c.eye[i] + d * (c.fwd[i] + a * c.right[i] + b * c.up[i]);
                double nx0, ny0, w0, d0, nx1, ny1, w1, d1;
                CHECK(hz::shader_ndc(A, hz::kScreen, p, nx0, ny0, w0, d0) && hz::shader_ndc(Ae, hz::kScreen, p, nx1, ny1, w1, d1));
                CHECK(std::fabs(nx0 - nx1) < 2e-4 && std::fabs(ny0 - ny1) < 2e-4);
                CHECK(std::fabs(w0 - w1) < 1e-3 * w0 && std::fabs(d0 - d1) < 1e-4 * std::fabs(d0) + 1e-9);
            }
        }
    }
}
// A hangar-world point seen by an eye: A' gives the eye's own projection of the corresponding LOCAL point (stereo, head translation, asymmetric FOV).
void test_eye_matrix_matches_local_projection() {
    float A[16]; parse16(kReal[0].a, A);
    hz::Camera c; CHECK(hz::decompose(A, hz::kScreen, c) == nullptr);
    const double head[3] = {0.0, 1.6, 0.0}; double qh[4]; yaw_pitch(0.4, 0.0, qh);
    for (double upm : {5.0, 39.6}) {
        hz::Room room; CHECK(hz::make_room(c, head, qh, upm, room) == nullptr);
        for (int eye = 0; eye < 2; ++eye) {
            hz::Eye e; e.p[0] = (eye ? 0.032 : -0.032) + 0.05; e.p[1] = 1.55; e.p[2] = 0.1;   // the head moved 5 cm right, 5 cm down, 10 cm back
            yaw_pitch(0.55, -0.3, e.q);
            e.fov[0] = eye ? -0.70 : -0.85; e.fov[1] = eye ? 0.85 : 0.70; e.fov[2] = 0.80; e.fov[3] = -0.90;
            float Ae[16]; CHECK(hz::eye_matrix(room, c, hz::kScreen, e, Ae) == nullptr);
            u3::EyeView ev; for (int i = 0; i < 3; ++i) ev.p[i] = e.p[i]; for (int i = 0; i < 4; ++i) { ev.q[i] = e.q[i]; ev.fov[i] = e.fov[i]; }
            for (double lx : {-1.0, 0.4}) for (double ly : {0.5, 1.2}) for (double lz : {-3.0, -8.0}) {
                const double l[3] = {lx, ly, lz};
                double g[3]; hz::local_to_world(room, l, g);
                double nx0, ny0, w0, nx1, ny1, w1, d1;
                if (!u3::project(ev, l, nx0, ny0, w0)) continue;
                CHECK(hz::shader_ndc(Ae, hz::kScreen, g, nx1, ny1, w1, d1));
                CHECK(std::fabs(nx0 - nx1) < 2e-4 && std::fabs(ny0 - ny1) < 2e-4);
                CHECK(std::fabs(w1 - w0 * upm) < 1e-3 * w1);   // w = the eye depth in hangar units
                CHECK(d1 > 0 && d1 < 1);                         // reversed depth inside the clip range for these distances
            }
        }
    }
    // the room is level: the game camera's pitch is dropped (a head looking level sees the hangar's horizontal straight ahead)
    hz::Room room; CHECK(hz::make_room(c, head, qh, 5.0, room) == nullptr);
    const double ahead[3] = {head[0] - std::sin(0.4) * 2.0, head[1], head[2] - std::cos(0.4) * 2.0};
    double g[3]; hz::local_to_world(room, ahead, g);
    CHECK(std::fabs(g[1] - c.eye[1]) < 1e-6);                       // same height as the game camera
    double fh[3] = {c.fwd[0], 0, c.fwd[2]}; hz::normalize3(fh);
    const double dg[3] = {g[0] - c.eye[0], 0, g[2] - c.eye[2]};
    CHECK(std::fabs(hz::dot3(dg, fh) - 10.0) < 1e-6);                // 2 m ahead = 10 units along the camera's level forward
    // refusals
    hz::Eye bad; bad.fov[0] = 0.5; bad.fov[1] = 0.4; float Ae[16];
    CHECK(streq(hz::eye_matrix(room, c, hz::kScreen, bad, Ae), "u3a-eye-fov"));
    const double down[4] = {-std::sqrt(0.5), 0, 0, std::sqrt(0.5)};
    CHECK(streq(hz::make_room(c, head, down, 5.0, room), "u3a-room-head-vertical"));
    CHECK(streq(hz::make_room(c, head, qh, 0.5, room), "u3a-room-upm"));
    CHECK(hz::classify_ps2(true, 0) == hz::Ps2Class::effect && hz::classify_ps2(false, 82631.0f) == hz::Ps2Class::effect &&
          hz::classify_ps2(false, 64.0f) == hz::Ps2Class::overlay && hz::classify_ps2(false, 0.0f) == hz::Ps2Class::overlay);
}

// ================================================================================================== the producer's hangar frames
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
const uint64_t kUploadVa = 0x40000000ull;
int g_ui_rt = 0, g_swap = 0;
acz::brief3d::EyeView make_eye(double yaw, double ipd) {
    acz::brief3d::EyeView e;
    e.orientation[0] = 0; e.orientation[1] = std::sin(yaw / 2); e.orientation[2] = 0; e.orientation[3] = std::cos(yaw / 2);
    e.position[0] = ipd * std::cos(yaw); e.position[1] = 1.6; e.position[2] = -ipd * std::sin(yaw);
    e.fov[0] = -0.785; e.fov[1] = 0.785; e.fov[2] = 0.785; e.fov[3] = -0.785;
    return e;
}
PoseIn pose(uint32_t gen = 1, double yaw = 0.0) {
    PoseIn p; p.ok = true; p.space_generation = gen; p.eyes[0] = make_eye(yaw, -0.032); p.eyes[1] = make_eye(yaw, 0.032); return p;
}
void screen(uint32_t gen = 1) {
    acz::vscreen::Options o; const float head[3] = {0.0f, 1.6f, 0.0f}, q[4] = {0, 0, 0, 1};
    acz::vscreen::publish_placement(acz::vscreen::place(o, head, q, 16.0f / 9.0f, gen - 1), head, gen);
}
// 2026-10-30c: these regression tests pin the stage-3b frame structure they were written for: the overlay's full-screen draws replayed (u3at:1)
// and the glows dropped (u3ag:0). The new defaults (tint dropped, glows re-projected) are tested in ui3d_select_offline_tests.cpp.
Producer& fresh(Producer& p, unsigned mode = 1) {
    u3::Options o; o.on = true; o.hangar = mode; o.hangar_tint = true; o.hangar_glow = false; p.configure_ui3d(o); p.gpu_ready(kUploadVa); screen(); return p;
}
// one hangar ACM draw as K10 logged it: UI target 2560 x 1440 fmt 10, PSO RTV 10 / DSV 20, depth GREATER + write, stencil ALWAYS, cb10 = 2
AcmFacts acm(const void* list, int64_t now, const uint8_t* rec) {
    AcmFacts f; f.list = list; f.now_ms = now; f.indexed = true; f.topology_type = 3; f.instances = 1;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_write = true; f.depth_func = acz::brief3d::kCmpGreater; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.om_known = true;
    f.t10_why = nullptr; f.t10_param = 5; f.t10_va = 0x30be00000ull; std::memcpy(f.rec, rec, hz::kRecBytes);
    f.cb_why = nullptr; f.cb10_0 = 2;
    return f;
}
// a ps2_v draw on the UI target with the menu cb0 (682,-448,2048,2048)
UiFacts ps2(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z, bool indexed = false, const void* rt = &g_ui_rt) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.vertices = indexed ? 6 : 4; f.instances = 1; f.topology_type = 3; f.indexed = indexed;
    f.rt = rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = acz::brief3d::kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f};
    f.vb_why = indexed ? "ui3d-indexed-draw" : nullptr; f.stride = 64;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        float v[16] = {xy[i][0], xy[i][1], z, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0, 0, 0, 0};
        std::memcpy(f.vtx + 64 * i, v, 64);
    }
    return f;
}
AcmPlan acm_step(Producer& p, const AcmFacts& f, const PoseIn* ps = nullptr, uint64_t completed = 0) {
    AcmPlan pl = p.ui3d_acm(f, ps ? *ps : (p.wants_poses(f.now_ms) ? pose() : PoseIn{}), done(completed));
    if (pl.record) p.ui3d_acm_commit(pl);
    return pl;
}
UiPlan ui_step(Producer& p, const UiFacts& f, uint64_t completed = 0) {
    UiPlan pl = p.ui3d_draw(f, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(completed));
    if (pl.record) p.ui3d_commit(pl);
    return pl;
}
void execute(Producer& p, const void* q, const void* list) {
    const void* ls[1] = {list};
    const auto t = p.before_execute(q, ls, 1);
    p.after_execute(t, true);
}
// One aircraft-select frame in K10 order (s=1): ACM 1-24, glow quads (z 1.2e5, 8e4), ACM 35-60, glows + the shadow (indexed, rev-subtract),
// ACM 73+ (here 3 + 3 + 3 draws), then the overlay: the dim (z 0), the frame (z 64), glyphs (z 1); then the final pass to the swapchain.
struct FrameOut { std::vector<AcmPlan> acm; std::vector<UiPlan> overlay; };
FrameOut hangar_frame(Producer& p, const void* L, int64_t t, const uint8_t* rec, uint64_t completed = 0) {
    FrameOut o;
    for (int i = 0; i < 3; ++i) o.acm.push_back(acm_step(p, acm(L, t, rec), nullptr, completed));
    CHECK(streq(ui_step(p, ps2(L, t, 2081, 2170, 2172, 2276, 116550.0f)).why, "u3a-fx-dropped"));
    CHECK(streq(ui_step(p, ps2(L, t, 1897, 2103, 1961, 2179, 82631.0f)).why, "u3a-fx-dropped"));
    for (int i = 0; i < 3; ++i) o.acm.push_back(acm_step(p, acm(L, t, rec)));
    CHECK(streq(ui_step(p, ps2(L, t, 1708, 1732, 1809, 1850, 129110.0f)).why, "u3a-fx-dropped"));
    CHECK(streq(ui_step(p, ps2(L, t, 0, 0, 0, 0, 0, true)).why, "u3a-fx-dropped"));   // the shadow (indexed, unread)
    for (int i = 0; i < 3; ++i) o.acm.push_back(acm_step(p, acm(L, t, rec)));
    o.overlay.push_back(ui_step(p, ps2(L, t, 1728, 1824, 2368, 2272, 0)));     // the dim (full screen, z 0)
    o.overlay.push_back(ui_step(p, ps2(L, t, 1808, 2051, 2024, 2187, 64)));    // a panel
    o.overlay.push_back(ui_step(p, ps2(L, t, 2249, 1893, 2265, 1905, 1)));     // a glyph
    CHECK(streq(ui_step(p, ps2(L, t, 1792, 1824, 2304, 2272, 0, false, &g_swap)).why, "ui3d-off-target"));
    return o;
}

void test_hangar_frame_published() {
    Producer p; fresh(p);
    int L = 0, Q = 0;
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    p.on_present(1000);
    const FrameOut f = hangar_frame(p, &L, 1001, rec);
    CHECK(f.acm.size() == 9 && f.overlay.size() == 3);
    const AcmPlan& a = f.acm[0];
    CHECK(a.record && a.clear && a.slot >= 0 && a.swap.parameter == 5 && a.swap.restore_va == 0x30be00000ull && a.swap.copies == 1);
    for (size_t i = 1; i < f.acm.size(); ++i) CHECK(f.acm[i].record && !f.acm[i].clear && f.acm[i].slot == a.slot);
    // upload: the slot's UI3D span, 256-aligned eye copies, the eye VA = the upload VA + the offset
    for (const AcmPlan& x : f.acm) for (unsigned e = 0; e < 2; ++e) {
        CHECK(x.upload_offset[e] % 256 == 0 && x.swap.eye_va[e] == kUploadVa + x.upload_offset[e]);
        CHECK(x.upload_offset[e] >= kUiUploadBase + uint64_t(a.slot) * u3::kUploadPerSlot);
        CHECK(x.upload_offset[e] + hz::kRecBytes <= kUiUploadBase + uint64_t(a.slot + 1) * u3::kUploadPerSlot);
    }
    CHECK(f.acm[1].upload_offset[0] == a.upload_offset[0] + 2 * hz::kRecStride);
    // the eye copies: A replaced by the eye matrices (both eyes differ), every other byte the game's
    CHECK(std::memcmp(a.rec[0] + 64, rec + 64, hz::kRecBytes - 64) == 0 && std::memcmp(a.rec[1] + 64, rec + 64, hz::kRecBytes - 64) == 0);
    CHECK(std::memcmp(a.rec[0], rec, 64) != 0 && std::memcmp(a.rec[0], a.rec[1], 64) != 0);
    {   // = eye_matrix of the latched room
        hz::Camera c; CHECK(hz::decompose_record(rec, c) == nullptr);
        const PoseIn ps = pose();
        double hp[3]; for (int i = 0; i < 3; ++i) hp[i] = 0.5 * (ps.eyes[0].position[i] + ps.eyes[1].position[i]);
        hz::Room room; CHECK(hz::make_room(c, hp, ps.eyes[0].orientation, 5.0, room) == nullptr);
        for (unsigned e = 0; e < 2; ++e) {
            hz::Eye ev; for (int i = 0; i < 3; ++i) ev.p[i] = ps.eyes[e].position[i];
            for (int i = 0; i < 4; ++i) { ev.q[i] = ps.eyes[e].orientation[i]; ev.fov[i] = ps.eyes[e].fov[i]; }
            float Ae[16]; CHECK(hz::eye_matrix(room, c, hz::kScreen, ev, Ae) == nullptr);
            CHECK(std::memcmp(a.rec[e], Ae, 64) == 0);
        }
    }
    // the overlay: stage-2 layers, the slot shared, after the room
    CHECK(f.overlay[0].record && f.overlay[0].layer == 0 && f.overlay[0].slot == a.slot && !f.overlay[0].clear);
    CHECK(f.overlay[1].record && f.overlay[2].record && f.overlay[2].layer == 2);
    CHECK(f.overlay[0].upload_offset[0] >= f.acm.back().upload_offset[1] + hz::kRecStride);
    p.on_present(1016);
    execute(p, &Q, &L);
    p.on_reset(&L, true);
    Held h; CHECK(p.acquire(0, done(1), h));
    CHECK(h.slot == a.slot && h.meta.opaque && !h.meta.pause && h.meta.replays == 12 && h.meta.space_generation == 1);
    p.release(h);
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";u3a-frames=1;u3a-published=1;u3a-rejected=0;u3a-acm=9;u3a-fx=4;u3a-latches=1;u3a-cuts=0;u3a-skips=0;u3a-late=0;u3a-dropped=0;u3a-moving=0;u3a-foreign=0;u3a-why=none"));
    CHECK(s.size() < 480 && !p.ui3d_held(1017));
}

// the room stays latched (mode 1) for the same eyes while the game camera orbits slowly; a cut re-targets it; mode 2 follows every frame
void test_latch_cut_follow() {
    for (unsigned mode : {1u, 2u}) {
        Producer p; fresh(p, mode);
        int L = 0, Q = 0;
        uint8_t r0[hz::kRecBytes], r1[hz::kRecBytes], r2[hz::kRecBytes];
        real_record(6, r0);     // next-screen s7
        real_record(7, r1);     // next-screen s8: the orbit, 2.9 units / ~1 degree later (no cut)
        real_record(3, r2);     // the weapon screen (a cut)
        p.on_present(2000);
        const AcmPlan a = acm_step(p, acm(&L, 2001, r0));
        CHECK(a.record);
        p.on_present(2016); execute(p, &Q, &L); p.on_reset(&L, true);
        const AcmPlan b = acm_step(p, acm(&L, 2017, r1), nullptr, 1);
        CHECK(b.record);
        // mode 1: the same eyes see the same room (A' identical although the game's A moved); mode 2: the room moved with the camera
        // (the depth column comes from each frame's own camera: b / Z0 differ in the last float bits, so x, y and w are compared)
        bool same_xyw = true;
        for (int i = 0; i < 16; ++i) if (i % 4 != 2 && std::memcmp(a.rec[0] + 4 * i, b.rec[0] + 4 * i, 4) != 0) same_xyw = false;
        CHECK(same_xyw == (mode == 1));
        p.on_present(2032); execute(p, &Q, &L); p.on_reset(&L, true);
        const AcmPlan c = acm_step(p, acm(&L, 2033, r2), nullptr, 2);
        CHECK(c.record && std::memcmp(c.rec[0], b.rec[0], 64) != 0);
        p.on_present(2048); execute(p, &Q, &L); p.on_reset(&L, true);
        std::string s = p.ui3d_summary();
        CHECK(has(s, ";u3a-frames=3;u3a-published=3;") && has(s, ";u3a-latches=1;") && has(s, ";u3a-cuts=1;"));
        // a gap > 2 s: re-latched (head anchor too)
        p.on_present(2048 + hz::kGapMs + 100);
        CHECK(acm_step(p, acm(&L, 2049 + hz::kGapMs + 100, r2), nullptr, 3).record);
        p.on_present(2200 + hz::kGapMs); execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(has(p.ui3d_summary(), ";u3a-latches=2;"));
    }
}

// Every refusal rejects the hangar frame (never published) and holds UI3D off: the flat screen.
void test_rejections() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    int Q = 0;
    auto run = [&](Producer& p, int& L, int64_t t, auto&& body, const char* want) {
        p.on_present(t);
        body(p, L, t + 1);
        p.on_present(t + 16); execute(p, &Q, &L); p.on_reset(&L, true);
        Held h; CHECK(!p.acquire(0, done(100), h));
        CHECK(p.ui3d_held(t + 17));
        CHECK(has(p.ui3d_summary(), (std::string(";u3a-why=") + want).c_str()));
    };
    {   // an ACM draw whose A differs from the frame's (another camera)
        Producer p; fresh(p); int L = 0;
        run(p, L, 1000, [&](Producer& q, int& l, int64_t t) {
            CHECK(acm_step(q, acm(&l, t, rec)).record);
            uint8_t r2[hz::kRecBytes]; real_record(1, r2);
            CHECK(streq(acm_step(q, acm(&l, t, r2)).why, "u3a-a-differs"));
        }, "u3a-a-differs");
        CHECK(has(p.ui3d_summary(), ";u3a-frames=1;u3a-published=0;u3a-rejected=1;"));
    }
    {   // an ACM draw after the overlay started
        Producer p; fresh(p); int L = 0;
        run(p, L, 1000, [&](Producer& q, int& l, int64_t t) {
            CHECK(acm_step(q, acm(&l, t, rec)).record);
            CHECK(ui_step(q, ps2(&l, t, 1808, 2051, 2024, 2187, 64)).record);
            CHECK(streq(acm_step(q, acm(&l, t, rec)).why, "u3a-acm-after-overlay"));
        }, "u3a-acm-after-overlay");
    }
    {   // L11 fix: a real-depth quad AFTER the overlay started is replayed as an overlay draw (u3a-late), the frame is published
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        CHECK(acm_step(p, acm(&L, 1001, rec)).record);
        CHECK(ui_step(p, ps2(&L, 1001, 1808, 2051, 2024, 2187, 64)).record);
        const UiPlan late = ui_step(p, ps2(&L, 1001, 2081, 2170, 2172, 2276, 116550.0f));
        CHECK(late.record && late.layer >= 1);
        p.on_present(1016); execute(p, &Q, &L); p.on_reset(&L, true);
        Held h; CHECK(p.acquire(0, done(1), h) && h.meta.replays == 3); p.release(h);
        CHECK(has(p.ui3d_summary(), ";u3a-published=1;u3a-rejected=0;") && has(p.ui3d_summary(), ";u3a-late=1;"));
    }
    {   // the fade flag, an unknown stencil, a depth func other than GREATER, a non-indexed ACM, an unreadable record mid-frame
        struct Case { const char* want; void (*mod)(AcmFacts&); };
        const Case cases[] = {
            {"u3a-cb10-fade", [](AcmFacts& f) { f.cb10_0 = 0x108; }},
            {"u3a-stencil-not-always", [](AcmFacts& f) { f.stencil_always = false; }},
            {"u3a-depth-func", [](AcmFacts& f) { f.depth_func = acz::brief3d::kCmpLess; }},
            {"u3a-acm-not-indexed", [](AcmFacts& f) { f.indexed = false; }},
            {"u3a-t10-record-torn", [](AcmFacts& f) { f.t10_why = "u3a-t10-record-torn"; }},
            {"u3a-pso-dsv", [](AcmFacts& f) { f.dsv = 0; }},
        };
        for (const Case& k : cases) {
            Producer p; fresh(p); int L = 0;
            run(p, L, 1000, [&](Producer& q, int& l, int64_t t) {
                CHECK(acm_step(q, acm(&l, t, rec)).record);
                AcmFacts f = acm(&l, t, rec); k.mod(f);
                CHECK(streq(acm_step(q, f).why, k.want));
            }, k.want);
        }
    }
    {   // an indexed ps2_v draw after the overlay started (vertices unread): DROPPED for the frame (step133 headset: it used to reject + hold 1 s)
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        CHECK(acm_step(p, acm(&L, 1001, rec)).record);
        CHECK(ui_step(p, ps2(&L, 1001, 1808, 2051, 2024, 2187, 64)).record);
        UiFacts ix = ps2(&L, 1001, 1808, 2051, 2024, 2187, 64); ix.indexed = true; ix.vb_why = nullptr;
        const UiPlan dp = ui_step(p, ix);
        CHECK(!dp.record && streq(dp.why, "u3a-late-indexed-dropped"));
        CHECK(ui_step(p, ps2(&L, 1001, 1792, 1824, 2304, 2272, 0)).record);   // the overlay goes on after the dropped draw
        p.on_present(1016); execute(p, &Q, &L); p.on_reset(&L, true);
        Held h; CHECK(p.acquire(0, done(100), h) && !p.ui3d_held(1017));
        p.release(h);
        CHECK(has(p.ui3d_summary(), ";u3a-published=1;u3a-rejected=0;u3a-acm=1;") && has(p.ui3d_summary(), ";u3a-dropped=1;u3a-moving=0;u3a-foreign=0;u3a-why=none"));
    }
    {   // a stage-2 refusal of an overlay draw (stencil test not ALWAYS) rejects the hangar frame and holds
        Producer p; fresh(p); int L = 0;
        run(p, L, 1000, [&](Producer& q, int& l, int64_t t) {
            CHECK(acm_step(q, acm(&l, t, rec)).record);
            UiFacts st = ps2(&l, t, 1808, 2051, 2024, 2187, 64); st.stencil_always = false;
            CHECK(streq(ui_step(q, st).why, "ui3d-stencil-not-always"));
        }, "ui3d-stencil-not-always");
    }
    {   // a non-ps2_v, non-ACM draw (e.g. a movie / CPT) inside the hangar frame: today's 3D refusal rejects it
        Producer p; fresh(p); int L = 0;
        run(p, L, 1000, [&](Producer& q, int& l, int64_t t) {
            CHECK(acm_step(q, acm(&l, t, rec)).record);
            UiFacts o; o.list = &l; o.now_ms = t; o.vs8 = u3::kCptVs8;
            CHECK(streq(ui_step(q, o).why, "ui3d-frame-has-3d"));
        }, "ui3d-frame-has-3d");
    }
}

// What does NOT open a hangar frame (the frame stays flat; today's behaviour)
void test_no_open() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    {   // the flight: +400 = 512: today's refusal + hold, nothing opens, nothing recorded
        Producer p; fresh(p); int L = 0;
        uint8_t fl[hz::kRecBytes]; float A[16]; parse16(kReal[0].a, A); const float k[4] = {512, -448, 2048, 2048}; make_record(A, fl, k);
        p.on_present(1000);
        const AcmPlan a = acm_step(p, acm(&L, 1001, fl));
        CHECK(!a.record && streq(a.why, "ui3d-frame-has-3d") && p.ui3d_held(1002) && p.tracked_lists() == 0);
        CHECK(has(p.ui3d_summary(), ";u3a-frames=0;"));
    }
    {   // an unreadable record with no frame open: today's refusal
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        AcmFacts f = acm(&L, 1001, rec); f.t10_why = "u3a-t10-address-unresolved";
        CHECK(streq(acm_step(p, f).why, "ui3d-frame-has-3d") && p.ui3d_held(1002));
    }
    {   // not the first draw of the game frame (a ps2 draw came first: it opened a stage-2 menu frame -> the ACM rejects it, today's rule)
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        CHECK(ui_step(p, ps2(&L, 1001, 1808, 2051, 2024, 2187, 64)).record);
        CHECK(streq(acm_step(p, acm(&L, 1001, rec)).why, "ui3d-frame-has-3d") && p.ui3d_held(1002));
    }
    {   // a ps2 draw refused before the ACM (no frame opened): the ACM is not the first draw -> no mid-frame opening
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        UiFacts x = ps2(&L, 1001, 1808, 2051, 2024, 2187, 64); x.pso_known = false;
        CHECK(streq(ui_step(p, x).why, "ui3d-pso-unknown"));
        CHECK(streq(acm_step(p, acm(&L, 1001, rec)).why, "u3a-not-first-draw") && p.tracked_lists() == 0);
    }
    {   // the hold covered part of the last frame interval: the first frame entirely after it opens
        Producer p; fresh(p); int L = 0, Q = 0;
        p.on_present(1000);
        AcmFacts bad = acm(&L, 1001, rec); bad.cb10_0 = 8;
        CHECK(streq(acm_step(p, bad).why, "u3a-cb10-fade"));                     // hold until 2001
        p.on_present(1990);
        CHECK(streq(acm_step(p, acm(&L, 1991, rec)).why, "ui3d-hold"));
        p.on_present(2002);   // the hold ended at 2001: this frame's whole interval lies after it
        CHECK(acm_step(p, acm(&L, 2003, rec)).record);
        p.on_present(2018); execute(p, &Q, &L); p.on_reset(&L, true);
        Held h; CHECK(p.acquire(0, done(1), h)); p.release(h);
    }
    {   // no pose / no screen placement
        Producer p; fresh(p); int L = 0;
        p.on_present(1000);
        const PoseIn none;
        CHECK(streq(acm_step(p, acm(&L, 1001, rec), &none).why, "ui3d-no-pose"));
        Producer q; fresh(q); screen(7);   // a placement of another LOCAL generation
        q.on_present(1000);
        CHECK(streq(acm_step(q, acm(&L, 1001, rec)).why, "ui3d-no-screen-placement"));
        screen(1);
    }
    {   // u3a configured but the pause window is live: the pause rules (the prefix), never a hangar frame
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.pause = true; p.configure_ui3d(o); p.gpu_ready(kUploadVa);
        int L = 0;
        acz::ui3d::pause::publish(true); p.on_present(1000);
        CHECK(p.ui3d_pause_live() && streq(acm_step(p, acm(&L, 1001, rec)).why, "ui3d-pause-prefix"));
        acz::ui3d::pause::publish(false); p.on_present(1002);
    }
    {   // a frame of only effects + no room: never opens (the ps2 draws alone open a stage-2 frame: the menu rules; not the hangar's)
        Producer p; fresh(p, 0); int L = 0;
        p.on_present(1000);
        CHECK(!p.ui3d_hangar_configured());
        CHECK(p.ui3d_summary().find("u3a-") == std::string::npos);   // the stage-2 line unchanged without u3a
    }
}

// The empty hangar frame (the room but the frame's list reset before the Present: the stage-2 rule) and the frame without any overlay.
void test_no_overlay_and_reset() {
    uint8_t rec[hz::kRecBytes]; real_record(4, rec);
    Producer p; fresh(p); int L = 0, Q = 0;
    p.on_present(1000);
    for (int i = 0; i < 4; ++i) CHECK(acm_step(p, acm(&L, 1001, rec)).record);
    CHECK(streq(ui_step(p, ps2(&L, 1001, 2081, 2170, 2172, 2276, 116550.0f)).why, "u3a-fx-dropped"));   // effects only, no overlay (K10 next-screen s7-10)
    p.on_present(1016); execute(p, &Q, &L); p.on_reset(&L, true);
    Held h; CHECK(p.acquire(0, done(1), h) && h.meta.replays == 4 && h.meta.opaque); p.release(h);
    // a reset BEFORE the Present keeps the frame open; a later ACM draw of the same frame is refused (draw after reset) -> rejected
    p.on_present(1032);
    CHECK(acm_step(p, acm(&L, 1033, rec), nullptr, 1).record);
    execute(p, &Q, &L); p.on_reset(&L, true);
    CHECK(streq(acm_step(p, acm(&L, 1034, rec)).why, "ui3d-draw-after-reset"));
    p.on_present(1048);
    CHECK(!p.acquire(1, done(10), h));
}

// ================================================================================================== regression: step131 null run L11 (2026-10-30)
// L11 summary-ui3d: u3a-frames=24, u3a-published=23, u3a-rejected=1 (u3a-fx-after-overlay), then `held=1;last-refusal=u3a-not-first-draw` with
// refused-draws +5 per 5 s (one refusal per hold period) for the rest of the hangar visit, and no eye0-table dump (`table=table-not-live`).
// Cause: the game frame straddling the hold's end refused its ACM draw with u3a-not-first-draw through fail(), which RENEWED the hold: a
// self-sustaining 1 s hold. This drives the producer like the glue does (a draw reaches the core only while !ui3d_held(now)) at 60 fps.
uint64_t field(const std::string& s, const char* key);
struct Sim {
    Producer p; int L = 0, Q = 0; uint64_t executed = 0, consumed = 0; unsigned published = 0;
    uint8_t rec[hz::kRecBytes];
    Sim() { fresh(p); real_record(0, rec); }
    bool reaches(int64_t now) const { return !p.ui3d_held(now); }
    // one game frame: present at t, draws at t+1..t+12 (ACM x3, glow, ACM x2, overlay x2, [an indexed overlay draw], the final pass)
    void frame(int64_t t, bool bad = false, bool late_quad = false, bool late_indexed = false) {
        p.on_present(t);
        int64_t now = t + 1;
        auto a = [&] { if (reaches(now)) acm_step(p, acm(&L, now, rec), nullptr, executed); ++now; };
        auto u = [&](const UiFacts& f) { if (reaches(f.now_ms)) ui_step(p, f, executed); ++now; };
        a(); a(); a();
        u(ps2(&L, now, 2081, 2170, 2172, 2276, 116550.0f));
        a(); a();
        u(ps2(&L, now, 1728, 1824, 2368, 2272, 0));
        u(ps2(&L, now, 1808, 2051, 2024, 2187, 64));
        if (late_quad) u(ps2(&L, now, 2081, 2170, 2172, 2276, 116550.0f));
        if (late_indexed) { UiFacts ix = ps2(&L, now, 1808, 2051, 2024, 2187, 64); ix.indexed = true; ix.vb_why = nullptr; u(ix); }
        if (bad) { UiFacts st = ps2(&L, now, 1808, 2051, 2024, 2187, 64); st.stencil_always = false; u(st); }   // a refusal that still rejects the frame
        u(ps2(&L, now, 1792, 1824, 2304, 2272, 0, false, &g_swap));
        // the list executes and resets after the frame (one list per frame); the consumer takes the newest slot and releases it at once
        const void* ls[1] = {&L};
        const auto tok = p.before_execute(&Q, ls, 1);
        if (tok.signal) { p.after_execute(tok, true); executed = tok.value; }
        p.on_reset(&L, true);
        Held h;
        if (p.acquire(0, done(executed), h)) p.release(h);   // the consumer keeps the ring moving
        published = unsigned(field(p.ui3d_summary(), ";u3a-published="));
    }
};
uint64_t field(const std::string& s, const char* key) {
    const size_t i = s.find(key); CHECK(i != std::string::npos);
    return std::strtoull(s.c_str() + i + std::strlen(key), nullptr, 10);
}
void test_l11_hold_recovers() {
    Sim s;
    int64_t t = 10000;
    for (int i = 0; i < 10; ++i, t += 16) s.frame(t);
    // a frame's list is reset before its Present (the stage-2 rule: published at the Present): the consumer sees it one frame later
    CHECK(s.published == 9 && has(s.p.ui3d_summary(), ";u3a-frames=10;u3a-published=9;"));
    const uint64_t refused0 = field(s.p.ui3d_summary(), ";refused-draws=");
    s.frame(t, true); t += 16;                            // the L11 rejection (here a stage-2 stencil refusal of an overlay draw): hold for 1 s
    CHECK(has(s.p.ui3d_summary(), ";u3a-rejected=1;") && s.p.ui3d_held(t));
    const int64_t hold_end = t - 16 + 13 + 1000;          // the rejecting draw ran at t-16+12; the hold lasts kUiHoldMs
    int64_t first_open = 0;
    unsigned misses = 0;
    for (; t < 13000; t += 16) {
        const unsigned b = s.published;
        s.frame(t);
        if (s.published > b) { if (!first_open) first_open = t; }
        else if (first_open) ++misses;
    }
    const std::string sum = s.p.ui3d_summary();
    // recovered: the first frame whose present lies after the hold's end opened, and every frame after it published
    if (!(first_open >= hold_end && first_open <= hold_end + 48 && misses == 0))
        std::printf("first_open=%lld hold_end=%lld misses=%u %s\n", (long long)first_open, (long long)hold_end, misses, s.p.ui3d_summary().c_str());
    CHECK(first_open >= hold_end && first_open <= hold_end + 48 && misses == 0);
    // no further refusal (the only one is the rejection itself), no hold renewal, the straddling frame skipped (not refused), no stage-2 frame
    CHECK(field(sum, ";refused-draws=") == refused0 + 1 && field(sum, ";u3a-rejected=") == 1);
    CHECK(field(sum, ";u3a-skips=") >= 1 && has(sum, ";u3a-why=ui3d-stencil-not-always"));
    CHECK(field(sum, "summary-ui3d;frames=") == 1 && field(sum, ";usable=") == 0);   // the one ui_st_ frame = the rejection; no menu frame
    CHECK(!s.p.ui3d_held(13000));
}
// L11 rejection cause: a real-depth quad after the overlay no longer rejects (it is a 2D overlay draw); the frames keep publishing.
void test_l11_late_quad_publishes() {
    Sim s;
    int64_t t = 20000;
    for (int i = 0; i < 30; ++i, t += 16) s.frame(t, false, true);
    const std::string sum = s.p.ui3d_summary();
    CHECK(s.published == 29 && field(sum, ";u3a-rejected=") == 0 && field(sum, ";u3a-late=") == 30 && sum.size() < 480);
}

// step133 headset (live-1144): `u3a-indexed-after-overlay` rejected the hangar frame 17 times in one visit (1 s flat each, 269 skipped frames).
// An indexed ps2_v draw after the overlay is dropped now: every frame publishes, none is rejected, the hold is never entered.
void test_late_indexed_never_rejects() {
    Sim s;
    int64_t t = 30000;
    for (int i = 0; i < 60; ++i, t += 16) s.frame(t, false, true, true);   // a late real-depth quad AND a late indexed draw in every frame
    const std::string sum = s.p.ui3d_summary();
    CHECK(s.published == 59 && field(sum, ";u3a-rejected=") == 0 && field(sum, ";u3a-skips=") == 0);
    CHECK(field(sum, ";u3a-dropped=") == 60 && field(sum, ";u3a-late=") == 60 && !s.p.ui3d_held(t) && sum.size() < 520);
    CHECK(has(sum, ";held=0;"));
}

// W14-1 F2: an opening failure (or a rejection) followed by a game frame that stalls past the hold must not publish the frame's remaining overlay
// as a stage-2 frame (partial opaque output); the next complete eligible frame recovers.
void test_w14_f2_failure_then_stall() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    {   // opening failure (no pose) at t=1001, hold until 2001, the same frame's overlay arrives at 2002 with a pose
        Producer p; fresh(p); int L = 0, Q = 0;
        p.on_present(1000);
        const PoseIn none;
        CHECK(streq(acm_step(p, acm(&L, 1001, rec), &none).why, "ui3d-no-pose") && p.ui3d_held(1002));
        CHECK(!p.ui3d_held(2002));
        const UiPlan o = ui_step(p, ps2(&L, 2002, 1808, 2051, 2024, 2187, 64));
        CHECK(!o.record && streq(o.why, "u3a-frame-skipped") && p.tracked_lists() == 0);
        CHECK(!ui_step(p, ps2(&L, 2003, 2249, 1893, 2265, 1905, 1)).record);
        p.on_present(2010);
        Held h; CHECK(!p.acquire(0, done(10), h));                           // nothing published from that frame
        CHECK(field(p.ui3d_summary(), "summary-ui3d;frames=") == 0);        // no stage-2 frame at all
        const FrameOut f = hangar_frame(p, &L, 2011, rec);                   // the next complete frame recovers
        CHECK(f.acm[0].record && f.overlay[2].record);
        p.on_present(2027); execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(p.acquire(0, done(1), h) && h.meta.opaque && h.meta.replays == 12); p.release(h);
    }
    {   // a hangar frame rejected mid-frame (A differs); the stall passes the hold; the rest arrives on ANOTHER list (no WRITING slot there)
        Producer p; fresh(p); int L = 0, M = 0;
        p.on_present(1000);
        CHECK(acm_step(p, acm(&L, 1001, rec)).record);
        uint8_t r2[hz::kRecBytes]; real_record(1, r2);
        CHECK(streq(acm_step(p, acm(&L, 1001, r2)).why, "u3a-a-differs"));
        const UiPlan o = ui_step(p, ps2(&M, 2002, 1808, 2051, 2024, 2187, 64));
        CHECK(!o.record && streq(o.why, "u3a-frame-skipped"));
        p.on_present(2010);
        CHECK(field(p.ui3d_summary(), ";u3a-rejected=") == 1 && field(p.ui3d_summary(), ";usable=") == 0);
    }
    {   // the closing rejection at the Present must not skip the NEXT frame (the latch ends with that Present)
        Producer p; fresh(p); int L = 0, Q = 0;
        p.on_present(1000);
        CHECK(acm_step(p, acm(&L, 1001, rec)).record);
        execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(streq(acm_step(p, acm(&L, 1002, rec)).why, "ui3d-draw-after-reset"));   // rejected; hold until 2002
        p.on_present(2003);
        CHECK(acm_step(p, acm(&L, 2004, rec), nullptr, 1).record);
    }
}

} // namespace

// live-1144: `u3a-cuts` +37 in 5 s (228 frames): a camera in continuous fast motion counted a cut at EVERY frame and re-latched the room each time.
// An isolated cut re-targets at once; consecutive cuts hold the room (u3a-moving) and it is re-targeted once when the camera is quiet again.
void test_moving_camera_does_not_relatch_every_frame() {
    Producer p; fresh(p, 1);
    int L = 0, Q = 0;
    uint8_t ra[hz::kRecBytes], rb[hz::kRecBytes];
    real_record(6, ra); real_record(3, rb);   // two cameras that differ by a cut
    int64_t t = 5000;
    uint64_t executed = 0;
    auto frame = [&](const uint8_t* rec) {
        p.on_present(t);
        CHECK(acm_step(p, acm(&L, t + 1, rec), nullptr, executed).record);
        const void* ls[1] = {&L};
        const auto tok = p.before_execute(&Q, ls, 1);
        if (tok.signal) { p.after_execute(tok, true); executed = tok.value; }
        p.on_reset(&L, true);
        Held h; if (p.acquire(0, done(executed), h)) p.release(h);
        t += 16;
    };
    frame(ra); frame(rb); frame(ra); frame(rb); frame(ra); frame(rb);   // 6 frames, every one a cut against the previous
    std::string s = p.ui3d_summary();
    CHECK(has(s, ";u3a-latches=1;") && has(s, ";u3a-cuts=1;") && has(s, ";u3a-moving=4;"));   // the old rule: cuts=5
    frame(rb);                                                           // the camera is quiet again: re-targeted once
    s = p.ui3d_summary();
    CHECK(has(s, ";u3a-cuts=2;") && has(s, ";u3a-moving=4;"));
    for (int i = 0; i < 20; ++i) frame(rb);                              // and stays
    s = p.ui3d_summary();
    CHECK(has(s, ";u3a-cuts=2;") && has(s, ";u3a-frames=27;") && has(s, ";u3a-rejected=0;"));
    // an isolated cut later still re-targets at once
    frame(ra);
    CHECK(has(p.ui3d_summary(), ";u3a-cuts=3;"));
}

// live-1144: after the hangar (mission load, flight) every ACM draw carries another screen constant: it is today's refusal + hold, counted
// u3a-foreign, and must not overwrite u3a-why (which read `ui3d-frame-has-3d` for the rest of the session and looked like a hangar failure).
void test_foreign_acm_does_not_mask_the_hangar_reason() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    uint8_t fl[hz::kRecBytes]; float A[16]; parse16(kReal[0].a, A); const float k[4] = {512, -448, 2048, 2048}; make_record(A, fl, k);
    Producer p; fresh(p); int L = 0, Q = 0;
    p.on_present(1000);
    CHECK(acm_step(p, acm(&L, 1001, rec)).record);
    AcmFacts bad = acm(&L, 1002, rec); bad.stencil_always = false;       // a real hangar failure
    CHECK(streq(acm_step(p, bad).why, "u3a-stencil-not-always"));
    p.on_present(1016); execute(p, &Q, &L); p.on_reset(&L, true);
    p.on_present(3000);                                                   // later: the mission load / flight
    const AcmPlan f = acm_step(p, acm(&L, 3001, fl));
    CHECK(!f.record && streq(f.why, "ui3d-frame-has-3d"));
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";u3a-foreign=1;u3a-why=u3a-stencil-not-always") && p.ui3d_held(3002));
}

// 2026-10-30: the hangar overlay is a floating panel: the scaled screen (u3s) on the menu layer distances (u3mn / u3mf), not the vscreen framing.
void test_overlay_panel_scale_and_depth() {
    uint8_t rec[hz::kRecBytes]; real_record(0, rec);
    auto run_with = [&](float scale, float mn, float mf, UiPlan& plan, UiFacts& facts) {
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.scale = scale; o.menu_near_m = mn; o.menu_far_m = mf;
        p.configure_ui3d(o); p.gpu_ready(kUploadVa); screen();
        int L = 0; p.on_present(1000);
        CHECK(acm_step(p, acm(&L, 1001, rec)).record);
        facts = ps2(&L, 1001, 1808, 2051, 2024, 2187, 64);
        plan = ui_step(p, facts);
        CHECK(plan.record);
    };
    UiPlan a, b, c; UiFacts fa, fb, fc;
    run_with(0.7f, 1.1f, 6.0f, a, fa);       // the defaults
    run_with(1.0f, 1.8f, 3.5f, b, fb);       // the old framing (full vscreen size, the 1.8 / 3.5 pair)
    run_with(0.7f, 1.1f, 6.0f, c, fc);
    CHECK(std::memcmp(a.vtx[0], c.vtx[0], sizeof a.vtx[0]) == 0 && std::memcmp(a.vtx[0], b.vtx[0], 4 * 64) != 0);
    // expected: rewrite_vertices on the scaled screen with the menu distances
    const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
    CHECK(sp.valid);
    const u3::Screen full = u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor);
    const PoseIn ps = pose();
    for (int variant = 0; variant < 2; ++variant) {
        const u3::Screen scr = variant == 0 ? u3::scaled(full, 0.7) : full;
        u3::Options lo; lo.near_m = variant == 0 ? 1.1f : 1.8f; lo.far_m = variant == 0 ? 6.0f : 3.5f;
        const UiPlan& pl = variant == 0 ? a : b; const UiFacts& f = variant == 0 ? fa : fb;
        for (unsigned e = 0; e < 2; ++e) {
            uint8_t ex[4 * 64];
            u3::EyeView ev; for (int i = 0; i < 3; ++i) ev.p[i] = ps.eyes[e].position[i];
            for (int i = 0; i < 4; ++i) { ev.q[i] = ps.eyes[e].orientation[i]; ev.fov[i] = ps.eyes[e].fov[i]; }
            const u3::SourceRect r = u3::rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);
            const u3::RewriteResult rw = u3::rewrite_vertices(f.vtx, 4, f.cb0, r, scr, lo, unsigned(pl.layer), ev, ex);
            CHECK(rw.why == nullptr && !rw.culled && std::memcmp(ex, pl.vtx[e], sizeof ex) == 0);
        }
    }
    // the scaled panel is narrower on screen: the quad's eye-image x extent shrinks ~0.7
    float xa[2], xb[2]; std::memcpy(&xa[0], a.vtx[0], 4); std::memcpy(&xa[1], a.vtx[0] + 128, 4); std::memcpy(&xb[0], b.vtx[0], 4); std::memcpy(&xb[1], b.vtx[0] + 128, 4);
    CHECK(std::fabs(xa[1] - xa[0]) < std::fabs(xb[1] - xb[0]));
}

int main() {
    test_option();
    test_record_check();
    test_decompose_real();
    test_cut();
    test_eye_matrix_reproduces_game_camera();
    test_eye_matrix_matches_local_projection();
    test_hangar_frame_published();
    test_latch_cut_follow();
    test_rejections();
    test_no_open();
    test_no_overlay_and_reset();
    test_l11_hold_recovers();
    test_l11_late_quad_publishes();
    test_late_indexed_never_rejects();
    test_overlay_panel_scale_and_depth();
    test_moving_camera_does_not_relatch_every_frame();
    test_foreign_acm_does_not_mask_the_hangar_reason();
    test_w14_f2_failure_then_stall();
    std::printf("ui3d_hangar_offline: %u checks passed\n", g_checks);
    return 0;
}
