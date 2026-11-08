// loader_fuzz_offline_tests.cpp -- adversarial tests of the baked-file loaders and option parsers (docs/review/overnight-loaders-review.md).
// No GPU, no game data, fixed seeds (deterministic). For each format (.zvrs v2/v3, .zvro v1/v2, .zvrm, fingerprints.json):
//   * the valid synthetic file parses and re-serialises byte for byte;
//   * truncation at EVERY length and a one-byte extension are refused;
//   * every header word set to boundary values (0, 1, 0x7fffffff, 0x80000000, 0xffffffff, n +- k, NaN/inf/-1 float bits ...) never crashes;
//   * random bit / byte / float mutations of the payload WITH a recomputed hash (so the content checks, not the hash, are what is tested) never crash;
//   * whatever is accepted is driven through the consumers the proxy runs on it (atlas/env texture/ray march; snap/grid/select; mesh/camera/raster/pose)
//     so an accepted-but-hostile file that makes them read out of bounds is caught (run this binary under /fsanitize=address for the strong form);
//   * the option strings (xr_terrain_stage incl. props/e6loc keys, xr_cockpit_model incl. auto) are fuzzed over the config charset: an accepted value
//     never yields a path outside the asset directory.
// Plus one regression per defect fixed in the review (L-1 .. L-5).
#include "aircraft_auto.h"
#include "cockpit_model.h"
#include "ee_e6_locator.h"
#include "stage_fingerprint.h"
#include "terrain_stage.h"
#include "world_props.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace T = acz::terrain;
namespace P = acz::props;
namespace C = acz::cockpit;
namespace F = acz::stagefp;

static unsigned g_checks = 0;
static unsigned long long g_accepted = 0, g_refused = 0;
static int g_mul = 1;   // iteration multiplier: second argument (or 1); fixed seeds either way
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {
struct Rng {   // splitmix64
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    uint32_t below(uint32_t n) { return n ? uint32_t(next() % n) : 0u; }
};
void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
uint32_t get32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; }
uint32_t fbits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
std::vector<uint8_t> read_file(const char* p) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(p, "rb");
    if (!f) return b;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    b.resize(n > 0 ? size_t(n) : 0u); if (n > 0 && std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    std::fclose(f);
    return b;
}
// boundary values for a header word whose current value is `orig` in a file of n bytes
std::vector<uint32_t> extremes(uint32_t orig, size_t n) {
    const uint32_t N = uint32_t(n);
    return {0u, 1u, 2u, 3u, 4u, 16u, 255u, 256u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, N, N - 1, N + 1, N - 4, N + 4, N / 2,
            orig + 1, orig - 1, orig + 4, orig - 4, orig * 2, orig ^ 0x80000000u, orig + 0x10000u,
            fbits(std::numeric_limits<float>::quiet_NaN()), fbits(INFINITY), fbits(-INFINITY), fbits(-1.0f), fbits(0.0f), fbits(1e30f), fbits(FLT_MIN / 4)};
}
// payload mutation: 1..8 bit flips, or a run of random bytes, or one aligned word set to an extreme / special float
void mutate(std::vector<uint8_t>& b, size_t lo, Rng& r) {
    const size_t span = b.size() - lo;
    if (!span) return;
    const uint32_t kind = r.below(4);
    if (kind == 0) { const uint32_t k = 1 + r.below(8); for (uint32_t i = 0; i < k; ++i) b[lo + r.below(uint32_t(span))] ^= uint8_t(1u << r.below(8)); }
    else if (kind == 1) { const size_t at = lo + r.below(uint32_t(span)); const uint32_t k = 1 + r.below(16); for (uint32_t i = 0; i < k && at + i < b.size(); ++i) b[at + i] = uint8_t(r.next()); }
    else if (span >= 4) {
        const size_t at = lo + (r.below(uint32_t(span - 3)) & ~size_t(3));
        const std::vector<uint32_t> v = extremes(get32(b, at), b.size());
        put32(b, at, v[r.below(uint32_t(v.size()))]);
    }
}

// ------------------------------------------------------------------------------------------------------------------ .zvrs
void rehash_stage(std::vector<uint8_t>& b) { if (b.size() >= T::kZvrsHeader) T::sha256(b.data() + T::kZvrsHeader, b.size() - T::kZvrsHeader, &b[192]); }
// What the proxy does with an accepted stage (xr_stereo_proxy.cpp terrain block) + the CPU reference march.
void exercise_stage(T::StageData s) {
    T::Conventions c; c.env_bits = T::kEnvAll; c.look = true;
    T::apply_sea_convention(s, c);
    T::Atlas a; T::build_atlas(s, a);
    std::vector<float> env; T::build_env_texture(s, c, env);
    const T::EnvView ev{env.empty() ? nullptr : env.data(), env.empty() ? 0u : T::kEnvAll};
    const float o[3] = {0.0f, 3000.0f, 0.0f};
    const float dirs[4][3] = {{0.0f, -1.0f, 0.0f}, {0.6f, -0.2f, -0.774597f}, {0.0f, 0.3f, -0.953939f}, {-0.707107f, -0.707107f, 0.0f}};
    for (const auto& d : dirs) {
        const T::Trace t0 = T::trace(s, a, o, d, 55000.0f, 160000.0f);
        const T::Trace t1 = T::trace_env(s, a, ev, o, d, 21500.0f, 160000.0f);
        CHECK(t0.steps <= T::kMarchSteps && t1.steps <= T::kMarchSteps);
    }
    (void)T::height_effective(s, 1e30f, -1e30f);
}
void test_l4(const T::StageData& s2);
void test_stage() {
    T::StageData s2 = T::make_synthetic_stage(3, true);
    s2.tile_water.assign(s2.ntiles, 0); s2.tile_water[1] = 1; s2.water_key = 16; s2.flags |= T::kFlagWater;
    for (int k = 0; k < 6; ++k) s2.objects.insert(s2.objects.end(), {float(k) * 1000.0f, 0.0f, -float(k) * 900.0f, 0.0f});
    s2.nobj = 6;
    T::StageData s3 = s2; T::add_synthetic_env(s3);
    for (const T::StageData* src : {&s2, &s3}) {
        const std::vector<uint8_t> good = T::serialize_stage(*src);
        T::StageData p;
        CHECK(T::parse_stage(good.data(), good.size(), p) == T::LoadStatus::ok);
        CHECK(T::serialize_stage(p) == good);                       // valid files: unchanged behaviour, byte-identical round trip
        exercise_stage(p);
        // truncation at every length, one extra byte
        for (size_t n = 0; n < good.size(); ++n) { T::StageData q; CHECK(T::parse_stage(good.data(), n, q) != T::LoadStatus::ok); CHECK(q.heights.empty()); }
        { std::vector<uint8_t> e = good; e.push_back(0); T::StageData q; CHECK(T::parse_stage(e.data(), e.size(), q) != T::LoadStatus::ok); }
        CHECK(T::parse_stage(nullptr, good.size(), p) == T::LoadStatus::too_small);
        // header words at boundary values (the header is not hashed: no rehash needed)
        for (size_t off = 4; off + 4 <= 192; off += 4)
            for (uint32_t v : extremes(get32(good, off), good.size())) {
                std::vector<uint8_t> b = good; put32(b, off, v);
                T::StageData q;
                if (T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::ok) { ++g_accepted; exercise_stage(q); } else ++g_refused;
            }
        // payload mutations with a valid hash
        Rng r(0x5eed0001ull + src->flags);
        for (int it = 0; it < 900 * g_mul; ++it) {
            std::vector<uint8_t> b = good;
            const int rounds = 1 + int(r.below(3));
            for (int k = 0; k < rounds; ++k) mutate(b, T::kZvrsHeader, r);
            rehash_stage(b);
            T::StageData q;
            if (T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::ok) { ++g_accepted; exercise_stage(q); } else ++g_refused;
        }
        // a word out of the atlas, a water flag > 1: refused
        { std::vector<uint8_t> b = good; const uint32_t ow = get32(b, 60); const uint16_t bad = uint16_t(src->ntiles); std::memcpy(&b[ow + 2 * 777], &bad, 2); rehash_stage(b);
          T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::bad_content); }
        { std::vector<uint8_t> b = good; b[get32(b, 80) + 1] = 2; rehash_stage(b); T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::bad_content); }
    }
    // L-5: environment parameters the renderer divides by
    {
        const std::vector<uint8_t> good = T::serialize_stage(s3);
        const uint32_t oe = get32(good, 88);
        CHECK(oe != 0);
        auto param = [&](uint32_t i, int ch) { return size_t(oe) + 8u + (size_t(i) * 4u + size_t(ch)) * 4u; };
        for (int ch = 0; ch < 3; ++ch) {   // P[25].x sprite size, .y cloud repeat, .z wave repeat = 0 / negative
            for (float bad : {0.0f, -4096.0f}) {
                std::vector<uint8_t> b = good; std::memcpy(&b[param(T::kPMisc, ch)], &bad, 4); rehash_stage(b);
                T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::bad_content);
            }
        }
        { std::vector<uint8_t> b = good; const float st = 0.5f; std::memcpy(&b[param(T::kPSkyStops, 2)], &st, 4); rehash_stage(b);   // stop 2 below stop 1
          T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::bad_content); }
        { std::vector<uint8_t> b = good; std::memcpy(&b[param(T::kPSkyStops, 1)], &b[param(T::kPSkyStops, 0)], 4); rehash_stage(b);   // two equal stops
          T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::bad_content); }
        { std::vector<uint8_t> b = good; const float w = 0.0f; std::memcpy(&b[param(T::kPMisc, 3)], &w, 4); rehash_stage(b);   // cloud shadow strength 0 is fine
          T::StageData q; CHECK(T::parse_stage(b.data(), b.size(), q) == T::LoadStatus::ok); }
    }
    test_l4(s2);
    // files on disk: missing, empty, a directory-like path
    { T::StageData q; CHECK(T::load_stage_file("", q) == T::LoadStatus::io_error); CHECK(T::load_stage_file("Z:\\no\\such\\file.zvrs", q) == T::LoadStatus::io_error); }
}
// L-4: a NaN / infinite coordinate clamps (was: int(floor(NaN)) indexed the height field / tile words out of bounds)
void test_l4(const T::StageData& s2) {
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        CHECK(T::height_bilinear(s2, nan, nan) == float(s2.heights[0]) * s2.hstep);
        CHECK(T::height_bilinear(s2, INFINITY, -INFINITY) == float(s2.heights[size_t(0) * T::kHDim + 512]) * s2.hstep);
        CHECK(std::isfinite(T::height_bilinear(s2, 1e38f, 1e38f)));
        T::Atlas a; T::build_atlas(s2, a);
        const float o[3] = {0.0f, 3000.0f, 0.0f}, dn[3] = {nan, -1.0f, nan};
        const T::Trace t = T::trace(s2, a, o, dn, 55000.0f, 160000.0f);   // must not crash; the result itself is meaningless
        CHECK(t.steps <= T::kMarchSteps);
        const float on[3] = {nan, nan, nan}, d[3] = {0.0f, -1.0f, 0.0f};
        (void)T::trace(s2, a, on, d, 55000.0f, 160000.0f);
    }
}

// ------------------------------------------------------------------------------------------------------------------ .zvro
void rehash_props(std::vector<uint8_t>& b) { if (b.size() >= P::kZvroHeader) T::sha256(b.data() + P::kZvroHeader, b.size() - P::kZvroHeader, &b[96]); }
const T::StageData& props_stage() { static const T::StageData s = T::make_synthetic_stage(5, false); return s; }
void exercise_props(P::PropSet s) {
    const bool snapped = P::snap_to_terrain(s, props_stage());
    if (!snapped) return;   // the proxy drops the props
    P::Options o; o.props = o.trees = true; o.cap = 7;
    P::Grid g; P::build_grid(s, o, g);
    const float view[2][3] = {{0.0f, 0.0f, -1.0f}, {0.05f, 0.0f, -0.99875f}};
    const float eyes[2][3] = {{0.0f, 500.0f, 0.0f}, {250000.0f, 100.0f, -250000.0f}};
    for (const auto& eye : eyes) {
        P::Selection sel; P::select(s, g, o, eye, view, sel);
        CHECK(sel.inst.size() <= o.cap);
        uint32_t n = 0;
        for (const P::Batch& b : sel.batches) { CHECK(b.model < s.models.size()); CHECK(b.first == n); n += b.count; }
        CHECK(n == sel.inst.size());
    }
    (void)P::prop_placements(s);
}
P::PropSet props_valid() {
    const uint8_t rgba[4] = {200, 100, 50, 255};
    std::vector<P::Instance> inst;
    for (int k = 0; k < 12; ++k) inst.push_back(P::Instance{{float(k) * 3000.0f - 15000.0f, 0.0f, float(k % 4) * -2500.0f}, 1.0f + 0.1f * float(k), 0u, uint32_t(k & 1)});
    P::PropSet s = P::make_box_set(40.0f, rgba, inst, 8);
    // a second (tree) model with its own index range, as objects_baker.py lays them out
    const uint32_t vb = uint32_t(s.verts.size()), ib = uint32_t(s.idx.size());
    for (int k = 0; k < 3; ++k) { P::Vertex v{}; v.pos[0] = float(k); v.pos[1] = float(k == 2) * 5.0f; v.nrm[1] = 32767; s.verts.push_back(v); }
    for (uint32_t k = 0; k < 3; ++k) s.idx.push_back(k);
    s.models.push_back(P::Model{ib, 3u, vb, P::kKindTree, {0.5f, 2.0f, 0.0f}, 3.0f});
    for (int k = 0; k < 5; ++k) s.inst.push_back(P::Instance{{float(k) * 700.0f, 2.0f, 800.0f}, 1.0f, 1u, P::kInstGroundRelative});
    std::memcpy(s.id, "synth", 6);
    return s;
}
void props_round(const std::vector<uint8_t>& good, uint64_t seed) {
    P::PropSet p;
    CHECK(P::parse_props(good.data(), good.size(), p) == P::LoadStatus::ok);
    CHECK(P::serialize_props(p) == good);
    exercise_props(p);
    for (size_t n = 0; n < good.size(); ++n) { P::PropSet q; CHECK(P::parse_props(good.data(), n, q) != P::LoadStatus::ok); CHECK(q.inst.empty()); }
    { std::vector<uint8_t> e = good; e.push_back(0); P::PropSet q; CHECK(P::parse_props(e.data(), e.size(), q) != P::LoadStatus::ok); }
    for (size_t off = 4; off + 4 <= 96; off += 4)
        for (uint32_t v : extremes(get32(good, off), good.size())) {
            std::vector<uint8_t> b = good; put32(b, off, v);
            P::PropSet q;
            if (P::parse_props(b.data(), b.size(), q) == P::LoadStatus::ok) { ++g_accepted; exercise_props(q); } else ++g_refused;
        }
    Rng r(seed);
    for (int it = 0; it < 4000 * g_mul; ++it) {
        std::vector<uint8_t> b = good;
        const int rounds = 1 + int(r.below(3));
        for (int k = 0; k < rounds; ++k) mutate(b, P::kZvroHeader, r);
        rehash_props(b);
        P::PropSet q;
        if (P::parse_props(b.data(), b.size(), q) == P::LoadStatus::ok) { ++g_accepted; exercise_props(q); } else ++g_refused;
    }
}
void test_l3();
void test_props() {
    P::PropSet v1 = props_valid();
    props_round(P::serialize_props(v1), 0x5eed0101ull);
    P::PropSet v2 = props_valid();
    CHECK(P::bake_snap(v2, props_stage()));
    props_round(P::serialize_props(v2), 0x5eed0102ull);
#ifdef ACZ_PROPS_FIXTURE
    { const std::vector<uint8_t> fx = read_file(ACZ_PROPS_FIXTURE); CHECK(!fx.empty()); P::PropSet q; CHECK(P::parse_props(fx.data(), fx.size(), q) == P::LoadStatus::ok); exercise_props(q);
      Rng r(0x5eed0103ull);
      for (int it = 0; it < 1500 * g_mul; ++it) { std::vector<uint8_t> b = fx; mutate(b, P::kZvroHeader, r); rehash_props(b); P::PropSet m;
          if (P::parse_props(b.data(), b.size(), m) == P::LoadStatus::ok) { ++g_accepted; exercise_props(m); } else ++g_refused; } }
#endif
    // L-2: models sharing one index range (objects_baker.py never writes that): refused (was: O(models * indices) validation)
    {
        P::PropSet s = props_valid();
        const P::Model m0 = s.models[0];
        for (uint32_t k = 1; k < 10; ++k) { P::Model m = m0; m.first_index += 3 * k; m.index_count -= 3 * k; s.models.push_back(m); }   // nested, partially overlapping
        const std::vector<uint8_t> b = P::serialize_props(s);
        P::PropSet q; CHECK(P::parse_props(b.data(), b.size(), q) == P::LoadStatus::bad_content);
        P::PropSet same = props_valid();
        for (int k = 0; k < 64; ++k) same.models.push_back(same.models[0]);   // an IDENTICAL shared range (a mesh reused by several models) stays accepted
        const std::vector<uint8_t> bs = P::serialize_props(same);
        CHECK(P::parse_props(bs.data(), bs.size(), q) == P::LoadStatus::ok);
        P::PropSet one = props_valid();
        one.models.push_back(P::Model{0u, 0u, 0u, P::kKindProp, {0, 0, 0}, 0.0f});   // an empty (locator) model is fine
        const std::vector<uint8_t> b1 = P::serialize_props(one);
        CHECK(P::parse_props(b1.data(), b1.size(), q) == P::LoadStatus::ok);
    }
    test_l3();
    { P::PropSet q; CHECK(P::load_props_file("", q) == P::LoadStatus::io_error); }
}
// L-3: a finite but overflowing scale (tree scale x FLT_MAX = inf, centre 0 x inf = NaN), huge positions, a NaN eye
void test_l3() {
    {
        P::PropSet s = props_valid();
        s.inst.clear();
        s.inst.push_back(P::Instance{{100.0f, 0.0f, 100.0f}, FLT_MAX, 1u, 0u});      // tree: sc = inf -> NaN distance
        s.inst.push_back(P::Instance{{3e38f, 0.0f, -3e38f}, 1.0f, 0u, 0u});          // far outside the grid
        s.inst.push_back(P::Instance{{10.0f, 0.0f, -40.0f}, 1.0f, 0u, 0u});          // a normal prop
        const std::vector<uint8_t> b = P::serialize_props(s);
        P::PropSet q; CHECK(P::parse_props(b.data(), b.size(), q) == P::LoadStatus::ok);
        CHECK(P::snap_to_terrain(q, props_stage()));
        P::Options o; o.props = o.trees = true;
        P::Grid g; P::build_grid(q, o, g);
        CHECK(g.start[0].back() == 2u && g.start[1].back() == 1u);
        // the huge-position prop lands in the clamped corner cell (column kGridDim-1, row 0)
        const uint32_t corner = uint32_t(0 * P::kGridDim + (P::kGridDim - 1));
        CHECK(g.start[0][corner + 1] - g.start[0][corner] == 1u);
        const float view[2][3] = {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}};
        const float eye[3] = {0.0f, 50.0f, 0.0f};
        P::Selection sel; P::select(q, g, o, eye, view, sel);
        CHECK(sel.inst.size() == 1u && sel.inst[0].pos[0] == 10.0f);   // the NaN-distance tree is culled, not sorted
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float bad_eye[3] = {nan, 0.0f, 0.0f};
        P::select(q, g, o, bad_eye, view, sel);
        CHECK(sel.inst.empty() && sel.batches.empty());
    }
}

// ------------------------------------------------------------------------------------------------------------------ .zvrm
void rehash_model(std::vector<uint8_t>& b) { if (b.size() >= C::kZvrmHeader) T::sha256(b.data() + C::kZvrmHeader, b.size() - C::kZvrmHeader, &b[108]); }
void exercise_model(const C::ModelData& m) {
    for (const char* optv : {"x", "x;anim:1;stick:side", "x;canopy:0;interior:0;floor:1;ext:0", "x;eye:0,3.5,-36;hide:0,1;show:9"}) {
        std::string id, why; C::Options o; CHECK(C::parse_options(optv, id, o, why));
        C::Mesh mesh;
        if (!C::build_mesh(m, o, mesh, why)) continue;
        CHECK(mesh.opaque_vertices + mesh.blend_vertices + mesh.glass_vertices == mesh.v.size());
        CHECK(mesh.anim_first + mesh.anim_vertices <= mesh.v.size());
        for (const C::GpuVertex& g : mesh.v) CHECK((g.mat & 7u) == C::kNoTexture || (g.mat & 7u) < mesh.textures.size());
        const float q[4] = {0, 0, 0, 1}, pos[3] = {0, 0, 0}, fov[4] = {-0.8f, 0.8f, 0.7f, -0.7f}, anchor[3] = {0, 0, 0};
        C::EyeCam cam;
        if (!C::eye_camera(q, pos, fov, anchor, 0.0f, mesh.eye, o.upm, o.near_u, cam)) continue;
        C::Target t; t.w = 24; t.h = 16; t.rgba.assign(size_t(t.w) * t.h * 4, 0.0f); t.depth.assign(size_t(t.w) * t.h, 0.0f);
        C::render_reference(mesh, o, 1.0f, 1u, cam, t);
        C::AnimInputs in; in.pitch = 1.0f; in.roll = -0.5f; in.yaw = 0.25f; in.throttle = 1.0f;
        std::vector<C::GpuVertex> posed; C::pose_anim(mesh, in, posed);
        CHECK(posed.size() == mesh.anim_vertices);
        C::apply_pose(mesh, posed);
    }
    for (size_t i = 0; i < m.parts.size(); ++i) (void)m.part_name(i);
}
void test_model() {
    for (const C::ModelData& src : {C::make_synthetic_model(), C::make_synthetic_model_c4()}) {
        const std::vector<uint8_t> good = C::serialize_model(src);
        C::ModelData p;
        CHECK(C::parse_model(good.data(), good.size(), p) == C::LoadStatus::ok);
        CHECK(C::serialize_model(p) == good);
        exercise_model(p);
        for (size_t n = 0; n < good.size(); ++n) { C::ModelData q; CHECK(C::parse_model(good.data(), n, q) != C::LoadStatus::ok); CHECK(q.parts.empty()); }
        { std::vector<uint8_t> e = good; e.push_back(0); C::ModelData q; CHECK(C::parse_model(e.data(), e.size(), q) != C::LoadStatus::ok); }
        for (size_t off = 4; off + 4 <= 108; off += 4)
            for (uint32_t v : extremes(get32(good, off), good.size())) {
                std::vector<uint8_t> b = good; put32(b, off, v);
                C::ModelData q;
                if (C::parse_model(b.data(), b.size(), q) == C::LoadStatus::ok) { ++g_accepted; exercise_model(q); } else ++g_refused;
            }
        Rng r(0x5eed0201ull + src.parts.size());
        for (int it = 0; it < 4000 * g_mul; ++it) {
            std::vector<uint8_t> b = good;
            const int rounds = 1 + int(r.below(3));
            for (int k = 0; k < rounds; ++k) mutate(b, C::kZvrmHeader, r);
            rehash_model(b);
            C::ModelData q;
            if (C::parse_model(b.data(), b.size(), q) == C::LoadStatus::ok) { ++g_accepted; exercise_model(q); } else ++g_refused;
        }
    }
    // L-1: parts sharing one index range (model_baker.py never writes that): refused. Before: accepted, the per-part index check was O(parts * indices)
    // and build_mesh expanded every copy (8192 parts x 6.3M indices = 1.6 TB of GpuVertex).
    {
        C::ModelData m = C::make_synthetic_model();
        const C::Part p0 = m.parts[1];
        for (int k = 0; k < 64; ++k) { C::Part p = p0; p.parent = -1; m.parts.push_back(p); }
        const std::vector<uint8_t> b = C::serialize_model(m);
        C::ModelData q; CHECK(C::parse_model(b.data(), b.size(), q) == C::LoadStatus::bad_content);
        C::ModelData e = C::make_synthetic_model();
        C::Part empty; empty.name_off = 0; e.parts.push_back(empty);   // an empty part (the baker's mesh-less nodes) is fine
        const std::vector<uint8_t> b1 = C::serialize_model(e);
        CHECK(C::parse_model(b1.data(), b1.size(), q) == C::LoadStatus::ok);
    }
    { C::ModelData q; CHECK(C::load_model_file("", q) == C::LoadStatus::io_error); }
}

// ------------------------------------------------------------------------------------------------------------------ fingerprints.json
std::string fp_valid() {
    std::string sig1, sig2, sig3;
    for (int i = 0; i < 32; ++i) { char h[3]; std::snprintf(h, sizeof h, "%02x", (i * 37 + 11) & 255); sig1 += h; std::snprintf(h, sizeof h, "%02x", (i * 53 + 7) & 255); sig2 += h;
                                   std::snprintf(h, sizeof h, "%02x", (i * 29 + 201) & 255); sig3 += h; }
    return std::string("{\"format\": \"acz-stage-fingerprints\", \"version\": 1, \"window\": 32, \"min_hits\": 1,\n"
        " \"groups\": [{\"stage\": \"m02a\", \"members\": [\"m02a\"], \"zvrs_sha256\": \"00ff\"}, {\"stage\": \"m03k\", \"members\": [\"m03k\", \"m03m\"]}],\n"
        " \"sigs\": [{\"hex\": \"") + sig1 + "\", \"groups\": [0], \"src\": \"mhd\"}, {\"hex\": \"" + sig2 + "\", \"groups\": [1], \"src\": \"mtd\"}, {\"hex\": \"" + sig3 +
        "\", \"groups\": [0, 1], \"src\": \"sph\\u0041\"}]}";
}
void exercise_fp(const F::Table& t) {
    for (const F::Group& g : t.groups) { CHECK(T::valid_stage_id(g.stage)); CHECK(!T::stage_path(g.stage, "D:\\x").empty()); }
    F::Matcher m(t);
    Rng r(7);
    std::vector<uint8_t> buf(4096);
    for (auto& c : buf) c = uint8_t(r.next());
    for (const F::Sig& s : t.sigs) { const size_t at = 100 + r.below(3000); for (uint32_t i = 0; i < F::kWindow; ++i) buf[at + i] = uint8_t(s.masked[i] ^ F::kMask); }
    m.begin_stream();
    for (size_t o = 0; o < buf.size();) { const size_t n = std::min<size_t>(1 + r.below(97), buf.size() - o); m.feed(buf.data() + o, n); o += n; }
    m.end_stream();
    const F::Result res = m.decide();
    if (res.kind == F::Result::Kind::match) CHECK(res.group >= 0 && size_t(res.group) < t.groups.size());
    (void)m.source_summary();
}
void test_fingerprints() {
    const std::string good = fp_valid();
    F::Table t; std::string why;
    CHECK(F::parse_table(good.data(), good.size(), t, why));
    exercise_fp(t);
    for (size_t n = 0; n < good.size(); ++n) { F::Table q; std::string w; CHECK(!F::parse_table(good.data(), n, q, w)); CHECK(q.groups.empty()); }
    Rng r(0x5eed0301ull);
    const char alphabet[] = "{}[]\",:\\0123456789abcdefuE.-+ tnrxyz/";
    for (int it = 0; it < 20000 * g_mul; ++it) {
        std::string b = good;
        const int k = 1 + int(r.below(4));
        for (int i = 0; i < k; ++i) {
            const size_t at = r.below(uint32_t(b.size()));
            const uint32_t op = r.below(3);
            if (op == 0) b[at] = alphabet[r.below(sizeof alphabet - 1)];
            else if (op == 1) b.erase(at, 1 + r.below(8));
            else b.insert(at, 1, alphabet[r.below(sizeof alphabet - 1)]);
            if (b.empty()) b = "{";
        }
        F::Table q; std::string w;
        if (F::parse_table(b.data(), b.size(), q, w)) { ++g_accepted; exercise_fp(q); } else ++g_refused;
    }
    // nesting, huge numbers, path-like stage ids
    { std::string deep(100000, '['); F::Table q; std::string w; CHECK(!F::parse_table(deep.data(), deep.size(), q, w)); }
    for (const char* bad_id : {"..", "..\\\\x", "a/b", "m02a\\\\..", "123456789", ""}) {
        std::string b = good; const size_t at = b.find("\"m02a\""); b.replace(at, 6, std::string("\"") + bad_id + "\"");
        F::Table q; std::string w; CHECK(!F::parse_table(b.data(), b.size(), q, w));
    }
    { std::string b = good; b.replace(b.find("\"min_hits\": 1"), 13, "\"min_hits\": 1e999"); F::Table q; std::string w; CHECK(!F::parse_table(b.data(), b.size(), q, w)); }
    { std::string b = good; b.replace(b.find("\"groups\": [0]"), 13, "\"groups\": [4294967296]"); F::Table q; std::string w; CHECK(!F::parse_table(b.data(), b.size(), q, w)); }
    { std::string b = good; b.replace(b.find("\"groups\": [0]"), 13, "\"groups\": [-1]"); F::Table q; std::string w; CHECK(!F::parse_table(b.data(), b.size(), q, w)); }
    { std::string b = good; b.replace(b.find("\"groups\": [0]"), 13, "\"groups\": [0.5]"); F::Table q; std::string w; CHECK(!F::parse_table(b.data(), b.size(), q, w)); }
    { F::Table q; std::string w; CHECK(!F::parse_table(nullptr, 10, q, w)); CHECK(!F::load_table_file("", q, w)); }
}

// ------------------------------------------------------------------------------------------------------------------ option strings
bool path_inside(const std::string& p, const std::string& dir, const char* ext) {
    if (p.empty()) return true;   // refused: no path
    if (p.compare(0, dir.size() + 1, dir + "\\") != 0) return false;
    const std::string leaf = p.substr(dir.size() + 1);
    if (leaf.find('\\') != std::string::npos || leaf.find('/') != std::string::npos || leaf.find("..") != std::string::npos) return false;
    return leaf.size() > std::strlen(ext) && leaf.compare(leaf.size() - std::strlen(ext), std::strlen(ext), ext) == 0;
}
void test_options() {
    const std::string dir = "D:\\assets";
    // config charset (runtime_config.cpp: [A-Za-z0-9_;:,.-]) plus the characters it already rejects, to show the parsers refuse them on their own
    const char cs[] = "abcmxyz0123456789_;:,.-=AZ/\\ ";
    const char* keys[] = {"yaw:", "sgn:", "mirror:", "sea:", "cam:", "props:", "trees:", "treescale:", "propcap:", "e6loc:", "look:", "sky:", "env:", "q:", "far:",
                          "upm:", "gain:", "tint:", "eye:", "hide:", "show:", "stick:", "prefer:", "auto", "m02a", "..", "f16ck_vrcockpit"};
    Rng r(0x5eed0401ull);
    for (int it = 0; it < 60000 * g_mul; ++it) {
        std::string v;
        const uint32_t len = r.below(100);
        while (v.size() < len) {
            if (r.below(4) == 0) v += keys[r.below(sizeof keys / sizeof keys[0])];
            else v.push_back(cs[r.below(sizeof cs - 1)]);
        }
        if (v.size() > 99) v.resize(99);
        // xr_terrain_stage: e6loc -> props -> terrain, as xr_stereo_proxy.cpp chains them
        std::string rest, stage_value, id, why; acz::e6loc::Mode mode{}; P::Options po; T::Conventions conv;
        if (acz::e6loc::extract_mode(v, rest, mode, why) && P::extract_options(rest, stage_value, po, why) && T::parse_stage_option(stage_value, id, conv, why)) {
            CHECK(T::valid_stage_id(id));
            CHECK(path_inside(T::stage_path(id, dir), dir, ".zvrs"));
            CHECK(path_inside(P::props_path(id, dir), dir, ".zvro"));
            CHECK(std::isfinite(conv.yaw_offset_deg) && std::isfinite(conv.alt_offset) && std::isfinite(conv.haze_l) && std::isfinite(conv.far_dist));
            CHECK(po.cap >= 1 && std::isfinite(po.tree_scale) && po.tree_scale > 0);
        }
        bool ok = true; (void)acz::e6loc::mode_in(v.c_str(), ok);
        // xr_cockpit_model (fixed id or auto)
        C::Options co; std::string cid, cwhy;
        if (acz::aircraft::is_auto(v)) {
            acz::aircraft::AutoConfig ac;
            if (acz::aircraft::parse_auto(v, ac, cwhy)) { CHECK(ac.opt.hide.empty() && ac.opt.show.empty() && !ac.opt.eye_override); }
        } else if (C::parse_options(v, cid, co, cwhy)) {
            CHECK(C::valid_model_id(cid));
            CHECK(path_inside(C::model_path(cid, dir), dir, ".zvrm"));
            CHECK(std::isfinite(co.upm) && co.upm > 0 && co.hide.size() <= 64 && co.show.size() <= 64);
        }
    }
    // explicit traversal attempts are refused before any path is built
    for (const char* bad : {"..", "..\\x", "../x", "a\\b", "a/b", "m02a\\..", "c:", "m02a.zvrs", "a b", ""}) {
        std::string id, why; T::Conventions c; CHECK(!T::parse_stage_option(bad, id, c, why));
        CHECK(T::stage_path(bad, dir).empty()); CHECK(P::props_path(bad, dir).empty());
        C::Options o; CHECK(!C::parse_options(bad, id, o, why)); CHECK(C::model_path(bad, dir).empty());
    }
    CHECK(C::model_path("abcdefghijklmnopqrstuvwxy", dir).empty());   // 25 chars
    for (unsigned i = 0; i < acz::aircraft::kAircraftCount; ++i) CHECK(path_inside(C::model_path(acz::aircraft::kAircraft[i].model_id, dir), dir, ".zvrm"));
}
} // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";   // optional: stage | props | model | fp | options | l3 | l4 (one regression alone)
    if (argc > 2) g_mul = std::max(1, std::atoi(argv[2]));
    if (only == "l3") test_l3();
    if (only == "l4") test_l4(T::make_synthetic_stage(3, true));
    if (only.empty() || only == "stage") test_stage();
    if (only.empty() || only == "props") test_props();
    if (only.empty() || only == "model") test_model();
    if (only.empty() || only == "fp") test_fingerprints();
    if (only.empty() || only == "options") test_options();
    std::printf("loader_fuzz_offline_tests: %u checks passed (mutants accepted %llu, refused %llu)\n", g_checks, g_accepted, g_refused);
    return 0;
}
