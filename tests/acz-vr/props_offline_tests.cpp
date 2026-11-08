// props_offline_tests.cpp -- WORLD-3 offline tests (no GPU, no game data): the .zvro loader (byte identity with the Python-baked synthetic fixture,
// error cases), the xr_terrain_stage option extraction, ground snapping, the spatial grid selection (range / behind-the-eyes culling, instance cap),
// the props-pass projection (it must reproduce the composite's own pixel ray) and the terrain occlusion reference. GPU: xr_composite_warp_harness.cpp.
#include "world_props.h"
#include "terrain_stage.h"
#include "world_dedupe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::props;
namespace T = acz::terrain;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> read_file(const char* p) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(p, "rb");
    if (!f) return b;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    b.resize(size_t(n)); if (n > 0 && std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    std::fclose(f);
    return b;
}
static void rehash(std::vector<uint8_t>& b) { T::sha256(b.data() + kZvroHeader, b.size() - kZvroHeader, &b[96]); }
static void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
static void putf(std::vector<uint8_t>& b, size_t o, float v) { std::memcpy(&b[o], &v, 4); }
static uint32_t get32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; }

static void test_fixture_and_errors() {
    const std::vector<uint8_t> fx = read_file(ACZ_PROPS_FIXTURE);
    CHECK(!fx.empty());
    PropSet s;
    CHECK(parse_props(fx.data(), fx.size(), s) == LoadStatus::ok);
    CHECK(std::string(s.id) == "synth" && s.models.size() == 4 && s.ntex == 3 && s.texsize == 8 && s.inst.size() == 2 + 768);
    CHECK(s.models[1].index_count == 0);                         // the locator-only pmmdl model
    CHECK(s.models[0].index_count == 6 && s.models[0].kind == kKindProp && s.models[2].kind == kKindTree && s.models[3].kind == kKindTree);
    CHECK(s.inst[0].pos[0] == 100.0f && s.inst[0].pos[1] == 50.0f && s.inst[0].pos[2] == -200.0f && s.inst[0].model == 0 && s.inst[0].flags == 0);
    size_t trees = 0; for (const Instance& i : s.inst) trees += (i.flags & kInstGroundRelative) ? 1u : 0u;
    CHECK(trees == 768);
    // the C++ serializer reproduces the Python baker byte for byte
    CHECK(serialize_props(s) == fx);
    // errors
    PropSet o;
    CHECK(parse_props(fx.data(), 100, o) == LoadStatus::too_small);
    { auto b = fx; b[0] = 'X'; CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_magic); }
    { auto b = fx; put32(b, 4, 3); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_version); }   // PERF: 1 and 2 are known
    { auto b = fx; b[kZvroHeader + 40] ^= 1; CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_hash); }
    { auto b = fx; b.push_back(0); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_size); }
    { auto b = fx; b[70] = 1; rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_header); }
    { auto b = fx; put32(b, 28, 6); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_header); }   // texsize not a power of two
    { auto b = fx; put32(b, 52, get32(b, 52) + 4); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_size); }
    {   // instance -> model out of range
        auto b = fx; put32(b, get32(b, 60) + 16, 99); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content);
    }
    {   // instance position not finite
        auto b = fx; putf(b, get32(b, 60), std::nanf("")); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content);
    }
    {   // index beyond the vertices
        auto b = fx; put32(b, get32(b, 56), 1000); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content);
    }
    {   // texture layer beyond ntex
        auto b = fx; b[get32(b, 52) + 26] = 7; rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content);
    }
    {   // unknown instance flag
        auto b = fx; put32(b, get32(b, 60) + 20, 4); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content);
    }
    CHECK(o.models.empty() && o.inst.empty());                    // all-or-nothing
    CHECK(load_props_file("Z:\\no\\such\\file.zvro", o) == LoadStatus::io_error);
    CHECK(props_path("m02a", "D:\\x") == "D:\\x\\m02a.zvro" && props_path("bad id!", "D:\\x").empty());
}

static void test_options() {
    std::string rest, why; Options o;
    CHECK(extract_options("m02a;props:1;trees=1;yaw:5", rest, o, why) && rest == "m02a;yaw:5" && o.props && o.trees);
    CHECK(extract_options("m02a", rest, o, why) && rest == "m02a" && !o.any());
    CHECK(extract_options("m02a;blend:1;props:0;treescale:2.5;treerange:9000;proprange:40000;propcap:500", rest, o, why) && rest == "m02a;blend:1" && !o.props &&
          o.tree_scale == 2.5f && o.tree_range == 9000.0f && o.prop_range == 40000.0f && o.cap == 500);
    CHECK(!extract_options("m02a;props:2", rest, o, why) && why.find("props") != std::string::npos);
    CHECK(!extract_options("m02a;treescale:0", rest, o, why));
    CHECK(!extract_options("m02a;propcap:1.5", rest, o, why));
    CHECK(!extract_options("m02a;treerange:abc", rest, o, why));
    CHECK(extract_options("m02a;bogus:1", rest, o, why) && rest == "m02a;bogus:1");   // not ours: left for the terrain parser (which refuses it)
    Options d; CHECK(!d.props && !d.trees && d.tree_scale == 4.5f && d.cap == 16384);  // defaults: OFF
}

static void test_snap() {
    T::StageData st = T::make_synthetic_stage(3, false);
    const uint8_t red[4] = {200, 40, 40, 255};
    PropSet s = make_box_set(100.0f, red, {Instance{{1234.0f, 0.0f, -5678.0f}, 1.0f, 0, kInstGroundRelative}, Instance{{10.0f, 77.0f, 20.0f}, 1.0f, 0, 0}});
    snap_to_terrain(s, st);
    CHECK(s.inst[0].pos[1] == T::height_bilinear(st, 1234.0f, -5678.0f) && s.inst[0].flags == 0);   // a prop on a slope sits on the ray-marched surface
    CHECK(s.inst[1].pos[1] == 77.0f);                                                                // absolute props untouched
    const float y = s.inst[0].pos[1];
    snap_to_terrain(s, st);
    CHECK(s.inst[0].pos[1] == y);                                                                    // once only
    CHECK(serialize_props(s).size() == kZvroHeader + 32 + 8 * 8 * 4 + 24 * 32 + 36 * 4 + 2 * 24);
}

static void test_select() {
    const uint8_t c[4] = {1, 2, 3, 255};
    std::vector<Instance> in;
    for (int k = 0; k < 40; ++k) in.push_back(Instance{{float(k) * 1000.0f - 5000.0f, 0.0f, 300.0f}, 1.0f, 0, 0});   // x = -5000 .. 34000
    PropSet s = make_box_set(50.0f, c, in);
    // a second (tree) model sharing the box geometry
    Model tm = s.models[0]; tm.kind = kKindTree; s.models.push_back(tm);
    for (int k = 0; k < 10; ++k) s.inst.push_back(Instance{{float(k) * 500.0f + 250.0f, 0.0f, -100.0f}, 1.0f, 1, 0});   // x = 250 .. 4750
    Options o; o.props = true; o.trees = true; o.prop_range = 20000.0f; o.tree_range = 3000.0f; o.tree_scale = 2.0f;
    Grid g; build_grid(s, o, g);
    CHECK(g.start[0].size() == size_t(kGridDim) * kGridDim + 1 && g.start[1].size() == g.start[0].size() && g.items[0].size() == 40 && g.items[1].size() == 10);
    const float eye[3] = {0.0f, 100.0f, 0.0f};
    const float view[2][3] = {{1, 0, 0}, {1, 0, 0}};
    Selection sel; select(s, g, o, eye, view, sel);
    // props: x in [0, 20000+r] in front (x=-1000..-5000 behind both eye planes; the r ~ 43 margin keeps none of them), trees: x <= 3000 (+ r*2)
    size_t props = 0, trees = 0;
    for (const Batch& b : sel.batches) (b.model == 0 ? props : trees) += b.count;
    CHECK(props == 21 && trees == 6);                      // x = 0..20000 (21), trees 250..2750 (6)
    // far cells are not even scanned (PERF: per-kind windows: the props scan stops at 20000 + the prop reach, so x >= 21000 is never touched)
    CHECK(sel.culled_behind == 5 && sel.culled_range <= 4 && sel.considered == 21 + 6 + 5 + sel.culled_range);
    CHECK(sel.batches.size() == 2 && sel.batches[0].model == 0 && sel.batches[1].model == 1 && sel.batches[1].first == 21);
    for (uint32_t k = sel.batches[1].first; k < sel.inst.size(); ++k) CHECK(sel.inst[k].scale == 2.0f);   // tree scale applied
    // cap: the nearest are kept
    o.cap = 5; select(s, g, o, eye, view, sel);
    CHECK(sel.inst.size() == 5 && sel.capped == 22);
    float far = 0; for (const DrawInst& d : sel.inst) far = std::max(far, std::fabs(d.pos[0]));
    CHECK(far <= 2250.0f);                                 // the 5 nearest: x = 0, 250, 750, 1000, 1250 ... all within 2250
    // keys off -> nothing
    Options off; select(s, g, off, eye, view, sel);
    CHECK(sel.inst.empty() && sel.batches.empty());
    // props only
    o.cap = 16384; o.trees = false; select(s, g, o, eye, view, sel);
    CHECK(sel.batches.size() == 1 && sel.batches[0].model == 0 && sel.inst.size() == 21);
    // determinism
    Selection a, b; select(s, g, o, eye, view, a); select(s, g, o, eye, view, b);
    CHECK(a.inst.size() == b.inst.size() && std::memcmp(a.inst.data(), b.inst.data(), a.inst.size() * sizeof(DrawInst)) == 0);
}

// PERF: the per-kind grid selects EXACTLY what the WORLD-3 single grid selected (reference copy of the old algorithm below), for random sets, poses,
// option combinations and caps; a disabled kind is never scanned.
static void ref_select_v1(const PropSet& s, const Options& opt, const float eye[3], const float view[2][3], Selection& out) {
    out.inst.clear(); out.batches.clear();
    struct Cand { float d2; uint32_t idx; float pos[3]; float scale; };
    std::vector<Cand> cand;
    for (uint32_t ii = 0; ii < s.inst.size(); ++ii) {   // brute force over every instance (the old grid only bounded this loop)
        const Instance& in = s.inst[ii];
        const Model& m = s.models[in.model];
        const bool tree = m.kind == kKindTree;
        if (tree ? !opt.trees : !opt.props) continue;
        if (m.index_count == 0) continue;
        const float sc = in.scale * (tree ? opt.tree_scale : 1.0f);
        const float cxw = in.pos[0] + m.centre[0] * sc, cyw = in.pos[1] + m.centre[1] * sc, czw = in.pos[2] + m.centre[2] * sc;
        const float r = m.radius * sc;
        const float dx = cxw - eye[0], dy = cyw - eye[1], dz = czw - eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        const float range = (tree ? opt.tree_range : opt.prop_range) + r;
        if (d2 > range * range) continue;
        const float s0 = dx * view[0][0] + dy * view[0][1] + dz * view[0][2], s1 = dx * view[1][0] + dy * view[1][1] + dz * view[1][2];
        if (s0 < -r && s1 < -r) continue;
        cand.push_back(Cand{d2, ii, {in.pos[0], in.pos[1], in.pos[2]}, sc});
    }
    auto nearer = [](const Cand& a, const Cand& b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.idx < b.idx); };
    if (cand.size() > opt.cap) { std::nth_element(cand.begin(), cand.begin() + opt.cap, cand.end(), nearer); cand.resize(opt.cap); }
    std::sort(cand.begin(), cand.end(), [&](const Cand& a, const Cand& b) {
        const uint32_t ma = s.inst[a.idx].model, mb = s.inst[b.idx].model; return ma < mb || (ma == mb && a.idx < b.idx); });
    for (const Cand& c : cand) {
        const uint32_t m = s.inst[c.idx].model;
        if (out.batches.empty() || out.batches.back().model != m) out.batches.push_back(Batch{m, uint32_t(out.inst.size()), 0});
        ++out.batches.back().count;
        out.inst.push_back(DrawInst{{c.pos[0], c.pos[1], c.pos[2]}, c.scale});
    }
}
static void test_select_equivalence() {
    const uint8_t c[4] = {1, 2, 3, 255};
    PropSet s = make_box_set(40.0f, c, {});
    Model tm = s.models[0]; tm.kind = kKindTree; tm.centre[0] = 30.0f; s.models.push_back(tm);           // a tree model with an off-centre bound
    Model loc = s.models[0]; loc.index_count = 0; s.models.push_back(loc);                              // locator-only (never drawn)
    uint32_t r = 777u; auto rnd = [&]() { r = r * 1664525u + 1013904223u; return float(r >> 8) / float(1u << 24); };
    for (int k = 0; k < 60000; ++k) {
        const uint32_t m = k % 7 == 0 ? 0u : (k % 13 == 0 ? 2u : 1u);
        s.inst.push_back(Instance{{(rnd() - 0.5f) * 120000.0f, rnd() * 3000.0f, (rnd() - 0.5f) * 120000.0f}, 0.5f + rnd(), m, 0});
    }
    size_t compared = 0;
    for (int pass = 0; pass < 24; ++pass) {
        Options o; o.props = pass % 3 != 1; o.trees = pass % 3 != 2; o.tree_scale = 1.0f + float(pass % 5);
        o.prop_range = 5000.0f + 2000.0f * float(pass); o.tree_range = 3000.0f + 900.0f * float(pass); o.cap = pass % 4 == 0 ? 50u : 16384u;
        Grid g; build_grid(s, o, g);
        const float eye[3] = {(rnd() - 0.5f) * 100000.0f, 500.0f + rnd() * 3000.0f, (rnd() - 0.5f) * 100000.0f};
        const float a = rnd() * 6.2831853f;
        const float view[2][3] = {{std::cos(a), -0.2f, std::sin(a)}, {std::cos(a + 0.1f), -0.2f, std::sin(a + 0.1f)}};
        Selection got, want;
        select(s, g, o, eye, view, got);
        ref_select_v1(s, o, eye, view, want);
        CHECK(got.inst.size() == want.inst.size() && got.batches.size() == want.batches.size());
        CHECK(std::memcmp(got.inst.data(), want.inst.data(), got.inst.size() * sizeof(DrawInst)) == 0);
        for (size_t b = 0; b < got.batches.size(); ++b) CHECK(got.batches[b].model == want.batches[b].model && got.batches[b].first == want.batches[b].first && got.batches[b].count == want.batches[b].count);
        compared += got.inst.size();
        if (!o.trees) {   // trees off: only the prop grid is scanned (props are every 7th instance)
            CHECK(got.considered <= 60000u / 7u + 1u);
        }
    }
    CHECK(compared > 1000);
    // locator-only instances never enter the grid
    Options o; o.props = o.trees = true; Grid g; build_grid(s, o, g);
    size_t loc_n = 0; for (const Instance& i : s.inst) loc_n += i.model == 2;
    CHECK(g.items[0].size() + g.items[1].size() + loc_n == s.inst.size());
    std::printf("select equivalence: %zu instances compared over 24 poses\n", compared);
}
// PERF: .zvro v2 (bake-time snapped heights): round trip, the stage binding, refusals, v1 compatibility.
static void test_v2_baked_snap() {
    T::StageData st = T::make_synthetic_stage(3, false);
    for (int k = 0; k < 32; ++k) st.payload_sha[k] = uint8_t(17 * k + 3);
    const uint8_t red[4] = {200, 40, 40, 255};
    std::vector<Instance> in;
    for (int k = 0; k < 500; ++k) in.push_back(Instance{{float(k) * 731.0f - 180000.0f, k % 3 ? 0.0f : 55.0f, float(k) * -517.0f + 90000.0f}, 1.0f, 0, k % 3 ? kInstGroundRelative : 0u});
    PropSet run = make_box_set(10.0f, red, in);
    std::memcpy(run.id, "m02a", 5);
    PropSet bake = run;
    CHECK(snap_to_terrain(run, st));                                   // runtime snap (v1 path)
    CHECK(bake_snap(bake, st) && bake.baked_snap && !bake.snapped);
    const std::vector<uint8_t> file = serialize_props(bake);
    CHECK(get32(file, 4) == 2 && get32(file, 16) == kZvroFlagSnapped && std::memcmp(&file[68], st.payload_sha, kZvroStageShaBytes) == 0);
    PropSet p; CHECK(parse_props(file.data(), file.size(), p) == LoadStatus::ok);
    CHECK(p.baked_snap && !p.snapped && std::memcmp(p.stage_sha, st.payload_sha, kZvroStageShaBytes) == 0);
    for (const Instance& i : p.inst) CHECK(i.flags == 0);
    CHECK(serialize_props(p) == file);                                 // byte round trip
    CHECK(snap_to_terrain(p, st) && p.snapped);                        // accepted by its own stage, no CPU snapping
    CHECK(p.inst.size() == run.inst.size());
    for (size_t k = 0; k < p.inst.size(); ++k) CHECK(std::memcmp(&p.inst[k], &run.inst[k], sizeof(Instance)) == 0);   // identical to the runtime snap
    // another stage bake (one payload byte differs) -> refused, positions untouched
    T::StageData other = st; other.payload_sha[5] ^= 1;
    PropSet q; CHECK(parse_props(file.data(), file.size(), q) == LoadStatus::ok);
    CHECK(!snap_to_terrain(q, other) && !q.snapped);
    CHECK(!bake_snap(q, other));
    // refusals: unknown flag bit, stray bytes after the sha prefix, a ground-relative instance in a snapped file, a future version
    PropSet o;
    { auto b = file; put32(b, 16, 3); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_header); }
    { auto b = file; b[90] = 1; rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_header); }
    { auto b = file; put32(b, get32(b, 60) + 20, kInstGroundRelative); rehash(b); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_content); }
    { auto b = file; put32(b, 4, 3); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_version); }
    { auto b = file; put32(b, 4, 0); CHECK(parse_props(b.data(), b.size(), o) == LoadStatus::bad_version); }
    // v2 WITHOUT the flag: v1 semantics (ground-relative instances allowed, 68..95 must be zero)
    { auto b = serialize_props(make_box_set(10.0f, red, in)); CHECK(get32(b, 4) == 1); put32(b, 4, 2); rehash(b);
      PropSet v; CHECK(parse_props(b.data(), b.size(), v) == LoadStatus::ok && !v.baked_snap); CHECK(snap_to_terrain(v, st) && v.inst[1].pos[1] == run.inst[1].pos[1]);
      b[70] = 1; rehash(b); CHECK(parse_props(b.data(), b.size(), v) == LoadStatus::bad_header); }
    // a runtime-snapped v1 set without a height field: refused (was silently skipped)
    { PropSet v = make_box_set(10.0f, red, in); T::StageData empty; CHECK(!snap_to_terrain(v, empty)); }
}
static void test_projection() {
    // an anchor-frame lens like WorldComposite: base = R*(tanL, tanU, -1), ux = R*(tanR - tanL, 0, 0), vy = R*(0, tanD - tanU, 0); R = a head yaw
    const float l = -0.9f, r = 1.1f, u = 0.8f, dn = -1.0f;
    const float yaw = 0.3f, cy = std::cos(yaw), sy = std::sin(yaw);
    auto R = [&](const float v[3], float o[3]) { o[0] = cy * v[0] + sy * v[2]; o[1] = v[1]; o[2] = -sy * v[0] + cy * v[2]; };
    const float b0[3] = {l, u, -1}, x0[3] = {r - l, 0, 0}, y0[3] = {0, dn - u, 0};
    float base[3], ux[3], vy[3]; R(b0, base); R(x0, ux); R(y0, vy);
    // a stage camera basis (heading -45, pitch -20)
    T::Conventions cv; T::CamFrame cf{};
    const float campos[3] = {1000.0f, 3000.0f, -2000.0f};
    CHECK(T::camera_frame(campos, -0.35f, -0.7854f, 0.1f, cv, cf));
    float m[16];
    CHECK(make_clip(base, ux, vy, cf.right, cf.up, cf.fwd, m));
    double worst = 0;
    for (int k = 0; k < 200; ++k) {
        const float uu = float((k * 37) % 101) / 100.0f, vv = float((k * 53) % 97) / 96.0f, t = 50.0f + 400.0f * float(k);
        float d[3], dw[3];
        for (int i = 0; i < 3; ++i) d[i] = base[i] + uu * ux[i] + vv * vy[i];
        for (int i = 0; i < 3; ++i) dw[i] = cf.right[i] * d[0] + cf.up[i] * d[1] + cf.fwd[i] * (-d[2]);
        const float p[3] = {dw[0] * t, dw[1] * t, dw[2] * t};
        float pu, pv, c;
        CHECK(project(m, p, pu, pv, c) && c > 0);
        worst = std::max(worst, double(std::max(std::fabs(pu - uu), std::fabs(pv - vv))));
        const float q[3] = {-p[0], -p[1], -p[2]};
        CHECK(!project(m, q, pu, pv, c));                   // the mirrored point is behind
    }
    CHECK(worst < 2e-4);
    std::printf("projection: worst uv error %.2e over 200 rays\n", worst);
    // degenerate lens refused
    const float z3[3] = {0, 0, 0};
    CHECK(!make_clip(z3, ux, vy, cf.right, cf.up, cf.fwd, m));
}

static void test_occlusion() {
    T::StageData st = T::make_synthetic_stage(1, false);
    std::fill(st.heights.begin(), st.heights.end(), uint8_t(5));           // flat at 400 units
    for (uint32_t j = 0; j < T::kHDim; ++j) for (uint32_t i = 254; i <= 258; ++i) st.heights[size_t(j) * T::kHDim + i] = 40;   // a ridge at x ~ 0 (3200 u)
    st.hmax_byte = 40;
    T::Atlas at; T::build_atlas(st, at);
    const float o[3] = {-20000.0f, 1000.0f, 0.0f}, d[3] = {1.0f, 0.0f, 0.0f};
    const T::Trace tr = T::trace(st, at, o, d, 55000.0f, 160000.0f);
    CHECK(tr.hit && tr.t > 16000.0f && tr.t < 20000.0f);                  // the ridge
    CHECK(!prop_visible(40000.0f, 160000.0f, tr.hit, tr.t));               // a prop at x = +20000 behind the ridge
    CHECK(prop_visible(15000.0f, 160000.0f, tr.hit, tr.t));                // x = -5000: in front of the ridge
    CHECK(!prop_visible(15000.0f, 10000.0f, tr.hit, tr.t));                // beyond the march end
    CHECK(!prop_visible(0.0f, 160000.0f, false, 0.0f) && prop_visible(500.0f, 160000.0f, false, 0.0f));   // no prop / sky behind
    const float down[3] = {0.0f, -1.0f, 0.0f};
    const T::Trace g = T::trace(st, at, o, down, 55000.0f, 160000.0f);
    CHECK(prop_visible(590.0f, 160000.0f, g.hit, g.t) && !prop_visible(610.0f, 160000.0f, g.hit, g.t));   // 10 u above / below the ground
    // prop colour reference (WORLD-1 domain): display shading, haze toward kHaze, linear once
    const float alb[3] = {0.8f, 0.2f, 0.2f}, up[3] = {0, 1, 0}, view[3] = {0, -1, 0}, vback[3] = {0, 1, 0};
    const float sl = std::sqrt(T::kSun[0] * T::kSun[0] + T::kSun[1] * T::kSun[1] + T::kSun[2] * T::kSun[2]);
    float c0[3]; prop_color(alb, up, view, 0.0f, 55000.0f, 160000.0f, c0);
    CHECK(std::fabs(c0[0] - T::srgb_to_linear(std::min(1.0f, 0.8f * (T::kAmbient + T::kDiffuse * T::kSun[1] / sl)))) < 1e-6f);
    float c1[3]; prop_color(alb, up, vback, 0.0f, 55000.0f, 160000.0f, c1);   // seen from below: the normal flips to face the eye
    CHECK(std::fabs(c1[0] - T::srgb_to_linear(std::min(1.0f, 0.8f * T::kAmbient))) < 1e-6f);
    float c2[3]; prop_color(alb, up, view, 160000.0f, 55000.0f, 160000.0f, c2);   // fully hazed at the march end
    CHECK(std::fabs(c2[1] - T::srgb_to_linear(T::kHaze[1])) < 1e-5f);
    // PROPS-SHADING propshade:game reference: unlit albedo, linear fog over kPropFogEnd toward kHaze, one decode; the normal/view are irrelevant
    float g0[3]; prop_color_game(alb, 0.0f, g0);
    CHECK(std::fabs(g0[0] - T::srgb_to_linear(0.8f)) < 1e-6f && std::fabs(g0[1] - T::srgb_to_linear(0.2f)) < 1e-6f);
    float g1[3]; prop_color_game(alb, 0.5f * kPropFogEnd, g1);
    CHECK(std::fabs(g1[0] - T::srgb_to_linear(0.8f + (T::kHaze[0] - 0.8f) * 0.5f)) < 1e-6f);
    float g2[3]; prop_color_game(alb, 3.0f * kPropFogEnd, g2);
    CHECK(std::fabs(g2[2] - T::srgb_to_linear(T::kHaze[2])) < 1e-6f && prop_fog(-1.0f) == 0.0f && prop_fog(kPropFogEnd) == 1.0f);
    const float over[3] = {1.5f, -0.2f, 0.5f}; float g3[3]; prop_color_game(over, 0.0f, g3);
    CHECK(g3[0] == 1.0f && g3[1] == 0.0f);   // saturated like the GPU (RGBA8 albedo target)
    // the game's record fog slope: rec[+340] = 0xBBB55555 (headset test 17 building/aircraft records), 255 / -that = kPropFogEnd within 1 unit
    { const uint32_t bits = 0xBBB55555u; float k1; std::memcpy(&k1, &bits, 4); CHECK(std::fabs(255.0f / -k1 - kPropFogEnd) < 1.0f); }
    // options: default game, propshade:w3 / game, bad value refused, the key is removed from the terrain string
    {
        Options o; std::string rest, why;
        CHECK(extract_options("m02a;props:1", rest, o, why) && o.shade_game);
        CHECK(extract_options("m02a;props:1;propshade:w3;env:1", rest, o, why) && !o.shade_game && rest == "m02a;env:1");
        CHECK(extract_options("m02a;propshade:game", rest, o, why) && o.shade_game && rest == "m02a");
        CHECK(!extract_options("m02a;propshade:lit", rest, o, why) && why.find("propshade") != std::string::npos);
    }
}

// WORLD-4: option keys, placement list (props only, trees excluded), the matcher (tolerance per axis, sorted search, non-finite), the defaults.
static void test_dedupe() {
    Options o; std::string rest, why;
    CHECK(extract_options("m02a;env:1;props:1;propdedupe:1;dedupetol:0.5", rest, o, why) && rest == "m02a;env:1" && o.dedupe && o.dedupe_tol == 0.5f);
    CHECK(extract_options("m02a;props:1", rest, o, why) && !o.dedupe && o.dedupe_tol == 2.0f && o.tree_range == 16000.0f);
    CHECK(!extract_options("m02a;propdedupe:2", rest, o, why) && !extract_options("m02a;dedupetol:0", rest, o, why) && !extract_options("m02a;dedupetol:500", rest, o, why));
    const uint8_t c[4] = {200, 50, 50, 255};
    PropSet s = make_box_set(10.0f, c, {{{100, 5, 200}, 1.0f, 0, 0}, {{-50, 7, 30}, 1.0f, 0, 0}});
    s.models.push_back(s.models[0]); s.models.back().kind = kKindTree; s.inst.push_back({{1, 2, 3}, 1.0f, 1, 1});
    const std::vector<float> pl = prop_placements(s);
    CHECK(pl.size() == 6 && pl[0] == 100 && pl[3] == -50);   // the tree instance is not a placement
    Dedupe d; d.tol = 2.0f; d.xyz = {-50, 7, 30, 0, 0, 0, 100, 5, 200, 100, 5, 260};   // sorted by x (two at x 100)
    CHECK(dedupe_match(d, 100.5, 5.2, 259.0) && dedupe_match(d, -51.9, 8.9, 28.1) && dedupe_match(d, 101.9, 3.1, 201.9));
    CHECK(!dedupe_match(d, 102.1, 5, 200) && !dedupe_match(d, 100, 7.1, 200) && !dedupe_match(d, 100, 5, 230) && !dedupe_match(d, NAN, 5, 200));
    Dedupe e; CHECK(!dedupe_match(e, 0, 0, 0));
}
// WORLD-4: a real forest-filled bake, if present (D:\acz-extract\stage-w4\m02a.zvro + the v3 stage in stage-w2, or ACZ_W4_STAGE_DIR / ACZ_TERRAIN_W2_DIR):
// load, snap, grid, and the per-frame selection at the step56 / step62 poses (counts and CPU time; no game data in the repository).
static void test_real_forest_if_present() {
    const char* e4 = std::getenv("ACZ_W4_STAGE_DIR"); const char* e2 = std::getenv("ACZ_TERRAIN_W2_DIR");
    const std::string d4 = e4 ? e4 : "D:\\acz-extract\\stage-w4", d2 = e2 ? e2 : "D:\\acz-extract\\stage-w2";
    PropSet s; const LoadStatus ls = load_props_file(props_path("m02a", d4), s);
    if (ls == LoadStatus::io_error) { std::printf("  (no forest bake in %s: skipped)\n", d4.c_str()); return; }
    CHECK(ls == LoadStatus::ok);
    T::StageData st; if (T::load_stage_file(T::stage_path("m02a", d2), st) != T::LoadStatus::ok) { std::printf("  (no m02a stage in %s: skipped)\n", d2.c_str()); return; }
    snap_to_terrain(s, st);
    Options o; o.props = true; o.trees = true;
    Grid g; build_grid(s, o, g);
    struct P { const char* tag; float eye[3]; float fwd[3]; };
    const P poses[] = {{"step56", {-13854.0f, 3647.3f, 121515.3f}, {0.6158f, -0.4889f, -0.6158f}}, {"step62", {-14340.0f, 4003.575f, 122000.0f}, {0.7071f, 0.0f, -0.7071f}}};
    for (const P& p : poses) {
        const float view[2][3] = {{p.fwd[0], p.fwd[1], p.fwd[2]}, {p.fwd[0], p.fwd[1], p.fwd[2]}};
        Selection sel;
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k) select(s, g, o, p.eye, view, sel);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
        std::printf("  m02a forest bake (%zu instances) at %s: considered %u, drawn %zu (cap %u, capped %u), select %.2f ms\n", s.inst.size(), p.tag, sel.considered,
                    sel.inst.size(), o.cap, sel.capped, ms);
        CHECK(sel.inst.size() > 0 && sel.inst.size() <= o.cap);
    }
}
// PERF: CPU cost of every props/trees initialisation phase (file read, parse incl. sha256 + validation, ground snap, grid) and of one per-frame selection,
// for a synthetic 1.2M-instance forest (always) and the real m02a forest bake (if present). Thresholds are SOFT (printed, never failing): machine-dependent.
static double ms_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
static unsigned g_soft = 0;
static void soft(bool ok, const char* what, double v, double limit) {
    std::printf("  %-44s %9.2f ms (soft limit %.0f)%s\n", what, v, limit, ok ? "" : "  SOFT-EXCEEDED");
    if (!ok) ++g_soft;
}
static void bench_phases(const char* tag, const std::vector<uint8_t>& file, const T::StageData& st) {
    std::printf(" bench %s (%zu bytes)\n", tag, file.size());
    auto t0 = std::chrono::steady_clock::now();
    PropSet s; CHECK(parse_props(file.data(), file.size(), s) == LoadStatus::ok);
    const double parse = ms_since(t0);
    t0 = std::chrono::steady_clock::now();
    uint8_t sha[32]; T::sha256(file.data() + kZvroHeader, file.size() - kZvroHeader, sha);
    const double hash = ms_since(t0);
    t0 = std::chrono::steady_clock::now();
    const bool baked = s.baked_snap;
    snap_to_terrain(s, st);
    const double snap = ms_since(t0);
    Options o; o.props = true; o.trees = true;
    t0 = std::chrono::steady_clock::now();
    Grid g; build_grid(s, o, g);
    const double grid = ms_since(t0);
    const float eye[3] = {-13854.0f, 3647.3f, 121515.3f}, view[2][3] = {{0.6158f, -0.4889f, -0.6158f}, {0.6158f, -0.4889f, -0.6158f}};
    Selection sel;
    t0 = std::chrono::steady_clock::now();
    select(s, g, o, eye, view, sel);
    const double first = ms_since(t0);
    soft(parse < 400.0, "parse (read buffer -> PropSet, incl. sha256)", parse, 400.0);
    soft(hash < 300.0, "  of which sha256 alone", hash, 300.0);
    std::printf("  %-44s %9.2f ms (%s)\n", "ground snap", snap, baked ? "baked at bake time: no-op" : "runtime height_bilinear");
    soft(grid < 150.0, "grid build", grid, 150.0);
    soft(first < 20.0, "first selection (props+trees)", first, 20.0);
    std::printf("  selection: considered %u, drawn %zu, capped %u\n", sel.considered, sel.inst.size(), sel.capped);
}
static void bench_init() {
    std::printf("PERF benchmark (soft thresholds)\n");
    T::StageData st = T::make_synthetic_stage(3, false);
    // synthetic forest: 1.2M ground-relative trees on a 4 x 4 km-spaced jittered lattice around the stage centre, plus 2000 props
    const uint8_t c[4] = {40, 120, 40, 255};
    PropSet s = make_box_set(30.0f, c, {});
    Model tm = s.models[0]; tm.kind = kKindTree; s.models.push_back(tm);
    uint32_t r = 12345u; auto rnd = [&]() { r = r * 1664525u + 1013904223u; return float(r >> 8) / float(1u << 24); };
    for (int k = 0; k < 2000; ++k) s.inst.push_back(Instance{{(rnd() - 0.5f) * 400000.0f, 10.0f, (rnd() - 0.5f) * 400000.0f}, 1.0f, 0, 0});
    for (int k = 0; k < 1200000; ++k) s.inst.push_back(Instance{{(rnd() - 0.5f) * 200000.0f, 0.0f, (rnd() - 0.5f) * 200000.0f}, 0.8f + 0.4f * rnd(), 1, kInstGroundRelative});
    std::memcpy(s.id, "m02a", 5);
    bench_phases("synthetic 1.2M (ground-relative)", serialize_props(s), st);
    const char* e4 = std::getenv("ACZ_W4_STAGE_DIR"); const char* e2 = std::getenv("ACZ_TERRAIN_W2_DIR");
    const std::string d4 = e4 ? e4 : "D:\\acz-extract\\stage-w4", d2 = e2 ? e2 : "D:\\acz-extract\\stage-w2";
    const std::vector<uint8_t> real = read_file(props_path("m02a", d4).c_str());
    T::StageData rs;
    if (real.empty() || T::load_stage_file(T::stage_path("m02a", d2), rs) != T::LoadStatus::ok) { std::printf("  (no real m02a forest bake / stage: real bench skipped)\n"); return; }
    bench_phases("real m02a.zvro", real, rs);
    // PERF: the Python bake-time snap (objects_baker.py --forest 3 --stage <m02a.zvrs>, D:\acz-extract\stage-perf or ACZ_PERF_STAGE_DIR) against the
    // C++ runtime snap of the v1 bake: same instances, y equal (float32, same formula), and no CPU snap at load.
    const char* ep = std::getenv("ACZ_PERF_STAGE_DIR");
    const std::vector<uint8_t> v2 = read_file(props_path("m02a", ep ? ep : "D:\\acz-extract\\stage-perf").c_str());
    if (v2.empty()) { std::printf("  (no v2 bake in stage-perf: Python/C++ snap cross-check skipped)\n"); return; }
    bench_phases("real m02a.zvro v2 (baked snap)", v2, rs);
    PropSet a, b;
    CHECK(parse_props(real.data(), real.size(), a) == LoadStatus::ok && parse_props(v2.data(), v2.size(), b) == LoadStatus::ok);
    CHECK(!a.baked_snap && b.baked_snap && snap_to_terrain(a, rs) && snap_to_terrain(b, rs) && a.inst.size() == b.inst.size());
    size_t exact = 0; double worst = 0;
    for (size_t k = 0; k < a.inst.size() && k < b.inst.size(); ++k) {
        const Instance &p = a.inst[k], &q = b.inst[k];
        CHECK(p.pos[0] == q.pos[0] && p.pos[2] == q.pos[2] && p.model == q.model && p.scale == q.scale && q.flags == 0);
        exact += p.pos[1] == q.pos[1];
        worst = std::max(worst, double(std::fabs(p.pos[1] - q.pos[1])));
    }
    std::printf("  Python bake-time snap vs C++ runtime snap: %zu/%zu y bit-identical, worst |dy| %.4g units\n", exact, a.inst.size(), worst);
    CHECK(worst <= 0.01);   // hard: trees stay on the ground (0.01 units = 0.15 mm)
}
int main() {
    bench_init();
    test_fixture_and_errors();
    test_real_forest_if_present();
    test_dedupe();
    test_options();
    test_snap();
    test_select();
    test_select_equivalence();
    test_v2_baked_snap();
    test_projection();
    test_occlusion();
    std::printf("props_offline_tests: %u checks passed (%u soft benchmark limits exceeded)\n", g_checks, g_soft);
    return 0;
}
