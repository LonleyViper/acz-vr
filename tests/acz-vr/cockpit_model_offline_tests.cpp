// cockpit_model_offline_tests.cpp -- COCKPIT-2 offline tests (no GPU, no game data): .zvrm loader round trip and refusals (corruption, truncation,
// missing file), option parsing, the visible-mesh build (hidden/exterior/glass/blend order/HUD glass), the per-eye 6DoF camera (pvb eye, head
// translation at upm, anchor yaw, asymmetric FOV) and the CPU reference raster on the synthetic model with asymmetric landmarks (left/right/up,
// per-eye parallax, 6DoF rotation, back-face culling, depth order, alpha test, glass, exterior behind the eye).
// The shader-vs-CPU comparison is in xr_composite_warp_harness.cpp (case_cockpit_model).
//   cockpit_model_offline_tests              all cases
//   cockpit_model_offline_tests --load <p>   load a real baked file (manual evidence; prints counts, the eye, the HUD glass; exit 1 on refusal)
#include "cockpit_model.h"
#include "terrain_stage.h"
#include "runtime_config.h"
#include "store_tracker.h"
#include "ee_store_watch.h"
#include "ee_counter_search.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::cockpit;

static unsigned g_checks = 0;
static const char* g_png = nullptr; static float g_ypr[3] = {0, 0, 0};
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static void wr32(std::vector<uint8_t>& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; }
static void reseal(std::vector<uint8_t>& b) { acz::terrain::sha256(b.data() + kZvrmHeader, b.size() - kZvrmHeader, &b[108]); }
static const float kPi = 3.14159265358979f;

static void test_roundtrip() {
    const ModelData m = make_synthetic_model();
    const auto bytes = serialize_model(m);
    CHECK(serialize_model(m) == bytes);
    CHECK(std::memcmp(bytes.data(), "ZVRM", 4) == 0 && rd32(bytes, 4) == 1 && rd32(bytes, 8) == 160 && rd32(bytes, 12) == bytes.size());
    CHECK(rd32(bytes, 16) == m.parts.size() && rd32(bytes, 20) == 2 && rd32(bytes, 24) == m.vertices.size() && rd32(bytes, 28) == m.indices.size());
    CHECK(rd32(bytes, 56) == 32 && rd32(bytes, 60) == 15 && rd32(bytes, 100) == 1);   // CCW | rest pose | RGBA CLUT (COCKPIT-3) | anim rest (COCKPIT-4)
    CHECK((rd32(bytes, 40) & 15u) == 0);   // the baker pads the vertex section to 16
    ModelData t;
    CHECK(parse_model(bytes.data(), bytes.size(), t) == LoadStatus::ok);
    CHECK(t.parts.size() == m.parts.size() && t.vertices.size() == m.vertices.size() && t.indices == m.indices && t.textures.size() == 2);
    CHECK(t.textures[0].w == 8 && t.textures[0].rgba == m.textures[0].rgba && t.textures[1].rgba == m.textures[1].rgba);
    CHECK(t.has_eye && t.eye[0] == 0.0f && t.eye[1] == 3.5f && t.eye[2] == -36.0f);
    CHECK(std::memcmp(t.vertices.data(), m.vertices.data(), m.vertices.size() * sizeof(Vertex)) == 0);
    CHECK(t.part_name(0) == "left_red" && t.part_name(9) == "pilot_hidden");
    CHECK(t.bbox_min[2] == -50.0f && t.bbox_max[2] == -20.0f);
    CHECK(serialize_model(t) == bytes);   // parse -> serialise is the identity
    CHECK(!t.legacy_clut_fixed);
}

// COCKPIT-3: a pre-flag (flags 3) mpl/acm bake has its CLUT read as BGRA: the loader swaps R and B back and marks the model; a prop bake (kind 2) and a
// flagged file are left alone.
static void test_legacy_clut() {
    ModelData m = make_synthetic_model();
    m.flags = 3;
    const auto legacy = serialize_model(m);
    ModelData t;
    CHECK(parse_model(legacy.data(), legacy.size(), t) == LoadStatus::ok && t.legacy_clut_fixed && t.flags == 7);
    // synthetic texture 1 = (150, 160, 170, 77): R and B exchanged, G and A unchanged; every texel of both textures
    CHECK(t.textures[1].rgba[0] == 170 && t.textures[1].rgba[1] == 160 && t.textures[1].rgba[2] == 150 && t.textures[1].rgba[3] == 77);
    for (size_t k = 0; k < 2; ++k)
        for (size_t i = 0; i < m.textures[k].rgba.size(); i += 4)
            CHECK(t.textures[k].rgba[i] == m.textures[k].rgba[i + 2] && t.textures[k].rgba[i + 2] == m.textures[k].rgba[i] &&
                  t.textures[k].rgba[i + 1] == m.textures[k].rgba[i + 1] && t.textures[k].rgba[i + 3] == m.textures[k].rgba[i + 3]);
    CHECK(std::memcmp(t.vertices.data(), m.vertices.data(), m.vertices.size() * sizeof(Vertex)) == 0);   // vertex colours are not CLUT colours: unchanged
    { ModelData k3 = m; k3.source_kind = 3; const auto b = serialize_model(k3); ModelData u;
      CHECK(parse_model(b.data(), b.size(), u) == LoadStatus::ok && u.legacy_clut_fixed && u.textures[1].rgba[0] == 170); }
    { ModelData k2 = m; k2.source_kind = 2; const auto b = serialize_model(k2); ModelData u;   // props: the pmtex CLUT was always read RGBA
      CHECK(parse_model(b.data(), b.size(), u) == LoadStatus::ok && !u.legacy_clut_fixed && u.flags == 7 && u.textures[1].rgba[0] == 150); }
    { ModelData f = make_synthetic_model(); const auto b = serialize_model(f); ModelData u;   // flagged: untouched
      CHECK(parse_model(b.data(), b.size(), u) == LoadStatus::ok && !u.legacy_clut_fixed && u.textures[1].rgba[0] == 150); }
}

static void test_loader_errors() {
    const auto good = serialize_model(make_synthetic_model());
    auto expect = [&](std::vector<uint8_t> b, LoadStatus want) {
        ModelData o; o.flags = 77;
        const LoadStatus st = parse_model(b.data(), b.size(), o);
        if (st != want) std::printf("  got %s want %s\n", status_name(st), status_name(want));
        CHECK(st == want);
        CHECK(o.flags == 77 && o.parts.empty() && o.vertices.empty());   // all-or-nothing: untouched on refusal
    };
    expect({}, LoadStatus::too_small);
    expect(std::vector<uint8_t>(good.begin(), good.begin() + 159), LoadStatus::too_small);
    { auto b = good; b[1] = 'X'; expect(b, LoadStatus::bad_magic); }
    { auto b = good; wr32(b, 4, 2); expect(b, LoadStatus::bad_version); }
    { auto b = good; wr32(b, 4, 0); expect(b, LoadStatus::bad_version); }
    { auto b = good; wr32(b, 8, 128); expect(b, LoadStatus::bad_header); }
    { auto b = good; b.pop_back(); expect(b, LoadStatus::bad_size); }                    // truncated
    { auto b = good; b.push_back(0); expect(b, LoadStatus::bad_size); }                  // trailing garbage
    { auto b = good; b.resize(b.size() / 2); wr32(b, 12, uint32_t(b.size())); expect(b, LoadStatus::bad_size); }   // truncated with a matching size field
    { auto b = good; wr32(b, 56, 36); expect(b, LoadStatus::bad_header); }               // vertex stride
    { auto b = good; wr32(b, 60, 31); expect(b, LoadStatus::bad_header); }               // unknown flag (bit 4)
    { auto b = good; wr32(b, 28, rd32(b, 28) + 1); expect(b, LoadStatus::bad_header); }  // index count not a multiple of 3
    { auto b = good; wr32(b, 20, 99); expect(b, LoadStatus::bad_header); }               // texture count over the cap
    { auto b = good; wr32(b, 100, 2); expect(b, LoadStatus::bad_header); }               // has_eye not 0/1
    { auto b = good; wr32(b, 104, 9); expect(b, LoadStatus::bad_header); }               // source kind
    { auto b = good; const float nan = std::nanf(""); std::memcpy(&b[88], &nan, 4); expect(b, LoadStatus::bad_header); }   // NaN eye
    { auto b = good; wr32(b, 32, 164); expect(b, LoadStatus::bad_size); }                // parts offset
    { auto b = good; wr32(b, 44, rd32(b, 44) + 4); expect(b, LoadStatus::bad_size); }    // index section offset
    { auto b = good; wr32(b, 52, rd32(b, 52) + 1); expect(b, LoadStatus::bad_size); }   // strings length
    { auto b = good; b[b.size() - 3] ^= 0x40; expect(b, LoadStatus::bad_hash); }          // one flipped payload bit
    { auto b = good; b[200] ^= 1; expect(b, LoadStatus::bad_hash); }
    { auto b = good; b[120] ^= 1; expect(b, LoadStatus::bad_hash); }                     // the stored digest itself
    // Content checks behind a valid (resealed) hash: a writer bug or a deliberate edit must still be refused.
    const uint32_t p_off = rd32(good, 32), t_off = rd32(good, 36), v_off = rd32(good, 40), i_off = rd32(good, 44), s_off = rd32(good, 48);
    { auto b = good; wr32(b, i_off, 1000); reseal(b); expect(b, LoadStatus::bad_content); }                 // index >= vertex count
    { auto b = good; wr32(b, i_off, 4); reseal(b); expect(b, LoadStatus::bad_content); }                    // index of another part's vertex
    { auto b = good; b[v_off + 26] = 0xFF; b[v_off + 27] = 0x7F; reseal(b); expect(b, LoadStatus::bad_content); }   // vertex part index out of range
    { auto b = good; const float nan = std::nanf(""); std::memcpy(&b[v_off + 4], &nan, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; const float big = 1e9f; std::memcpy(&b[v_off], &big, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; wr32(b, t_off, 0); reseal(b); expect(b, LoadStatus::bad_content); }                    // texture width 0
    { auto b = good; wr32(b, t_off, 2048); reseal(b); expect(b, LoadStatus::bad_content); }                 // texture width over the cap
    { auto b = good; wr32(b, t_off + 8, 2); reseal(b); expect(b, LoadStatus::bad_content); }                // texture format
    { auto b = good; wr32(b, t_off + 16 + 12, rd32(b, t_off + 12)); reseal(b); expect(b, LoadStatus::bad_content); }   // overlapping texture data
    { auto b = good; wr32(b, p_off + 4, 3); reseal(b); expect(b, LoadStatus::bad_content); }                // forward parent
    { auto b = good; wr32(b, p_off + 28, 5); reseal(b); expect(b, LoadStatus::bad_content); }               // texture index out of range
    { auto b = good; wr32(b, p_off + 36, 9); reseal(b); expect(b, LoadStatus::bad_content); }               // part index range past its vertices
    { auto b = good; wr32(b, p_off + 32, 1); reseal(b); expect(b, LoadStatus::bad_content); }               // first index not a multiple of 3
    { auto b = good; wr32(b, p_off + 44, 1000); reseal(b); expect(b, LoadStatus::bad_content); }            // vertex range past the buffer
    { auto b = good; wr32(b, p_off, 100000); reseal(b); expect(b, LoadStatus::bad_content); }               // name offset
    { auto b = good; const float inf = INFINITY; std::memcpy(&b[p_off + 48], &inf, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; b[b.size() - 1] = 'x'; reseal(b); expect(b, LoadStatus::bad_content); }                // strings not NUL-terminated
    // COCKPIT-4 part fields: rest (f32 +120) must be finite and small, anim flags (+124) known, and both zero in a file without kZvrmAnimRest
    { auto b = good; const float nan = std::nanf(""); std::memcpy(&b[p_off + 120], &nan, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; const float big = 9.0f; std::memcpy(&b[p_off + 120], &big, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; wr32(b, p_off + 124, 2); reseal(b); expect(b, LoadStatus::bad_content); }
    { auto b = good; wr32(b, 60, 7); wr32(b, p_off + 124, 1); reseal(b); expect(b, LoadStatus::bad_content); }      // flag 8 absent: no anim fields
    { auto b = good; wr32(b, 60, 7); const float r = 3.1416f; std::memcpy(&b[p_off + 120], &r, 4); reseal(b); expect(b, LoadStatus::bad_content); }
    (void)s_off;
    // Every single-byte truncation point is refused (never a crash/partial model).
    for (size_t n = 0; n < good.size(); n += 97) { ModelData o; CHECK(parse_model(good.data(), n, o) != LoadStatus::ok); }
    { ModelData o; CHECK(parse_model(nullptr, 1000, o) == LoadStatus::too_small); }
}

static void test_files() {
    CHECK(valid_model_id("j35jk_vrcockpit") && valid_model_id("a") && !valid_model_id("") && !valid_model_id("a.b") && !valid_model_id("..\\x") &&
          !valid_model_id(std::string(25, 'a')) && valid_model_id(std::string(24, 'a')));
    CHECK(model_path("j35jk_vrcockpit", "D:\\m") == "D:\\m\\j35jk_vrcockpit.zvrm");
    CHECK(model_path("bad/id", "D:\\m").empty());
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la) CHECK(model_path("x") == std::string(la) + "\\acz-vr\\models\\x.zvrm");
    ModelData o;
    CHECK(load_model_file("", o) == LoadStatus::io_error);
    CHECK(load_model_file("Z:\\definitely\\missing\\nothing.zvrm", o) == LoadStatus::io_error);
    char tmp[512] = {};
    const char* t = std::getenv("TEMP");
    std::snprintf(tmp, sizeof tmp, "%s\\acz_cockpit_model_test_%u.zvrm", t && *t ? t : ".", unsigned(std::rand()));
    const auto bytes = serialize_model(make_synthetic_model());
    FILE* f = std::fopen(tmp, "wb"); CHECK(f != nullptr);
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()); std::fclose(f);
    CHECK(load_model_file(tmp, o) == LoadStatus::ok && o.parts.size() == 10);
    f = std::fopen(tmp, "r+b"); CHECK(f != nullptr); std::fseek(f, 300, SEEK_SET); std::fputc(0x5A, f); std::fclose(f);   // corrupt on disk
    ModelData p; CHECK(load_model_file(tmp, p) == LoadStatus::bad_hash && p.parts.empty());
    std::remove(tmp);
}

static void test_options() {
    std::string id, why; Options o;
    CHECK(parse_options("j35jk_vrcockpit", id, o, why) && id == "j35jk_vrcockpit" && o.upm == 5.0f && o.exterior && o.glass && !o.hud_glass && !o.keep_replay);
    CHECK(parse_options("m;upm:6.5;gain=1.3;amb:0.4;cut:0.25;near:0.02;ext:0;glass:0;hudglass:1;replay:1;hide:3,4;show:9;eye:0,3.6,-36.2", id, o, why));
    CHECK(o.upm == 6.5f && o.gain == 1.3f && o.ambient == 0.4f && o.cut == 0.25f && o.near_u == 0.02f && !o.exterior && !o.glass && o.hud_glass && o.keep_replay);
    CHECK(o.hide.size() == 2 && o.hide[1] == 4 && o.show.size() == 1 && o.eye_override && o.eye[1] == 3.6f && o.eye[2] == -36.2f);
    const char* bad[] = {"", ";upm:5", "a b", "m;", "m;upm", "m;upm:0.1", "m;upm:x", "m;gain:9", "m;amb:2", "m;ext:2", "m;hide:", "m;hide:1,,2",
                         "m;hide:-1", "m;eye:1,2", "m;eye:1,2,3,4", "m;foo:1", "m;near:0", "m;upm:5;", "j35jk/vr"};
    for (const char* b : bad) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2) && !w2.empty()); }
    // COCKPIT-3 options and defaults: display domain, stage light, floor closure on; gain 1.0
    CHECK(parse_options("m", id, o, why) && o.display && o.stage_light && o.floor && o.gain == 1.0f && o.ambient == 0.6f);
    CHECK(parse_options("m;dom:raw;light:fixed;floor:0", id, o, why) && !o.display && !o.stage_light && !o.floor);
    CHECK(parse_options("m;dom=display;light=stage;floor=1", id, o, why) && o.display && o.stage_light && o.floor);
    for (const char* b : {"m;dom:linear", "m;dom:", "m;light:sun", "m;floor:2", "m;floor:yes"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
}

static void test_mesh() {
    const ModelData m = make_synthetic_model();
    Options o; o.floor = false; o.interior = false; Mesh me; std::string why;   // the COCKPIT-2 counts (the floor closure is tested below; the interior in test_c4)
    CHECK(build_mesh(m, o, me, why));
    CHECK(me.parts_drawn == 9 && me.parts_hidden == 1);              // pilot_hidden (MF_HIDDEN|MF_COCKPIT_DUP) skipped
    CHECK(me.blend_vertices == 6 && me.opaque_vertices == 8 * 6 && me.triangles == 18 && me.v.size() == 54);
    CHECK(me.textures.size() == 2 && me.textures[0].w == 8 && me.textures[1].w == 4);   // slot order = first use (checker part 6, tail part 8)
    CHECK(me.eye[1] == 3.5f && me.eye[2] == -36.0f);
    CHECK(me.v[0].mat == (kNoTexture | kMatUnlit));                   // left_red: unlit flag carried, no texture
    CHECK(me.v[6 * 6].mat == (0 | kMatAlphaTest));                    // checker: slot 0 + alpha test (6 parts before it: 0..5)
    CHECK(me.v[7 * 6].mat == (1 | kMatTexAlphaNotCov));               // tail: slot 1, skin alpha not coverage
    CHECK(me.v[48].mat == (kNoTexture | kMatBlend));                  // the glass in the blend range
    CHECK(me.hud.valid && me.hud.part == 7 && std::fabs(me.hud.distance - std::sqrt(9.0f + 36.0f)) < 1e-4f);
    CHECK(std::fabs(me.hud.tan_l - 0.125f) < 1e-5f && std::fabs(me.hud.tan_r - 0.125f) < 1e-5f && std::fabs(me.hud.tan_u - 3.4f / 6) < 1e-5f && std::fabs(me.hud.tan_d - (-2.6f / 6)) < 1e-5f);
    { Options x = o; x.exterior = false; Mesh y; CHECK(build_mesh(m, x, y, why) && y.parts_drawn == 8 && y.textures.size() == 1); }
    { Options x = o; x.glass = false; Mesh y; CHECK(build_mesh(m, x, y, why) && y.blend_vertices == 0 && !y.hud.valid); }
    { Options x = o; x.hide = {0, 1}; x.show = {9}; Mesh y; CHECK(build_mesh(m, x, y, why) && y.parts_drawn == 8 && y.parts_hidden == 2); }
    { Options x = o; x.hide = {10}; Mesh y; CHECK(!build_mesh(m, x, y, why)); }
    { Options x = o; x.eye_override = true; x.eye[2] = -30; Mesh y; CHECK(build_mesh(m, x, y, why) && y.eye[2] == -30.0f); }
    // COCKPIT-3 floor closure (default on): two up-facing untextured triangles appended to the OPAQUE range at the lowest point of the drawn non-exterior opaque
    // geometry (synthetic: checker/backface quads, y = -0.5), spanning its x/z footprint (x -4..4, z -50..-44); the glass stays in the blend range after it.
    { Options x; x.interior = false; Mesh y; CHECK(build_mesh(m, x, y, why) && y.floor && y.opaque_vertices == 8 * 6 + 6 && y.triangles == 20 && y.blend_vertices == 6);
      CHECK(y.floor_y == -0.5f && y.floor_x[0] == -4.0f && y.floor_x[1] == 4.0f && y.floor_z[0] == -50.0f && y.floor_z[1] == -44.0f);
      for (uint32_t k = 48; k < 54; ++k) CHECK(y.v[k].pos[1] == -0.5f && y.v[k].nrm[1] == 32767 && y.v[k].mat == kNoTexture && y.v[k].rgba[0] == kFloorRgba[0]);
      for (uint32_t k = 48; k < 54; k += 3) {   // CCW seen from above: geometric normal +Y
          const float* a = y.v[k].pos; const float* b = y.v[k + 1].pos; const float* c = y.v[k + 2].pos;
          CHECK((b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]) > 0.0f); }
      CHECK(y.v[54].mat == (kNoTexture | kMatBlend)); }
    { Options x; x.interior = false; x.exterior = false; Mesh y; CHECK(build_mesh(m, x, y, why) && y.floor && y.floor_y == -0.5f); }   // exterior parts never define the floor
    { ModelData n = m; for (auto& v : n.vertices) if (v.pos[1] < 3.6f) v.pos[1] = 3.6f; Options x; Mesh y;   // nothing below the eye: no floor, no interior
      CHECK(build_mesh(n, x, y, why) && !y.floor && y.opaque_vertices == 48); }
    { ModelData n = m; n.has_eye = false; Mesh y; CHECK(!build_mesh(n, o, y, why)); }
    { ModelData n = m; for (auto& p : n.parts) p.mflags |= MF_HIDDEN; Mesh y; CHECK(!build_mesh(n, o, y, why)); }
    { ModelData n = m; for (int k = 0; k < 4; ++k) n.textures.push_back(n.textures[0]);
      for (size_t i = 0; i < 6; ++i) n.parts[i].texture = int32_t(i % 6);   // 6 distinct textures in use -> refused (4 GPU slots)
      Mesh y; CHECK(!build_mesh(n, o, y, why)); }
    // blend order: two glass parts, the farther one first
    { ModelData n = m; n.parts[3].mflags |= MF_BLEND; Mesh y; CHECK(build_mesh(n, o, y, why) && y.blend_vertices == 12);
      CHECK(y.v[y.opaque_vertices].pos[2] == -50.0f && y.v[y.opaque_vertices + 6].pos[2] == -42.0f); }
}

// COCKPIT-3 colour pipeline: display-domain shading (default) = srgb_to_linear(tex * vcol * light * gain) * exposure; raw (dom:raw) = COCKPIT-2. And the
// stage light in the cockpit frame.
static void test_shading() {
    CHECK(std::fabs(srgb_to_linear(0.5f) - 0.214041f) < 1e-5f && srgb_to_linear(0.0f) == 0.0f && std::fabs(srgb_to_linear(1.0f) - 1.0f) < 1e-6f);
    CHECK(std::fabs(srgb_to_linear(0.04f) - 0.04f / 12.92f) < 1e-7f && srgb_to_linear(2.0f) == 1.0f && srgb_to_linear(-1.0f) == 0.0f);
    // the measured case (cockpit-render-impl.md section 10): the game shows the gunsight housing at display 16/255; the COCKPIT-2 path wrote a value v as LINEAR,
    // so the sRGB RTV showed srgb_encode(v) ~ 72/255 for v = 16/255 (4.5x too bright) - the display path writes srgb_to_linear(v) and shows v.
    Mesh me; const ModelData m = make_synthetic_model(); Options o; std::string why; CHECK(build_mesh(m, o, me, why));
    EyeCam cam{}; for (int i = 0; i < 16; ++i) cam.m[i] = 0;
    const float uv[2] = {0, 0}, n[3] = {0, 1, 0};
    const float vc[4] = {16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, 1.0f};
    { Options x; x.ambient = 1.0f; Constants k = make_constants(cam, x, 1.0f, 0); CHECK(k.flags == (kCFlagDisplay | kCFlagFid) && k.gain == 1.0f);
      float out[4]; shade_fragment(me, k, uint16_t(kNoTexture), uv, vc, n, out);
      CHECK(std::fabs(out[0] - srgb_to_linear(16.0f / 255.0f)) < 1e-6f && out[3] == 1.0f); }
    { Options x; x.ambient = 1.0f; x.display = false; Constants k = make_constants(cam, x, 1.0f, 0); CHECK(k.flags == kCFlagFid);
      float out[4]; shade_fragment(me, k, uint16_t(kNoTexture), uv, vc, n, out);
      CHECK(std::fabs(out[0] - 16.0f / 255.0f) < 1e-6f); }
    { Options x; x.ambient = 1.0f; Constants k = make_constants(cam, x, 2.0f, 0);   // exposure applies AFTER the conversion (like the terrain)
      float out[4]; shade_fragment(me, k, uint16_t(kNoTexture), uv, vc, n, out);
      CHECK(std::fabs(out[0] - 2.0f * srgb_to_linear(16.0f / 255.0f)) < 1e-6f); }
    // light: default kLight normalised; an explicit model-space light replaces it; a zero/NaN light falls back to kLight
    { Options x; const float L[3] = {0, 0, 2}; Constants k = make_constants(cam, x, 1.0f, 0, L); CHECK(k.light[0] == 0 && k.light[2] == 1.0f); }
    { Options x; const float Z[3] = {0, 0, 0}; Constants k = make_constants(cam, x, 1.0f, 0, Z), d = make_constants(cam, x, 1.0f, 0);
      CHECK(k.light[0] == d.light[0] && k.light[1] == d.light[1] && std::fabs(d.light[1] - 0.8f / std::sqrt(0.35f * 0.35f + 0.64f + 0.45f * 0.45f)) < 1e-6f); }
    // stage light in the cockpit frame: identity camera (right +X, up +Y, fwd -Z) keeps the stage direction; a camera heading 90 deg left (fwd = -X, right = -Z)
    // puts a sun in the stage's -X direction straight AHEAD (model -Z); a 90 deg right roll (up = +X... right = -Y) puts an overhead sun on the model's left (-X).
    { const float l[3] = {0.3f, 0.8f, -0.52f}, r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, f[3] = {0, 0, -1}; float o3[3];
      CHECK(stage_light_in_model(l, r, u, f, o3)); const float ln = std::sqrt(0.09f + 0.64f + 0.2704f);
      CHECK(std::fabs(o3[0] - 0.3f / ln) < 1e-5f && std::fabs(o3[1] - 0.8f / ln) < 1e-5f && std::fabs(o3[2] + 0.52f / ln) < 1e-5f); }
    { const float l[3] = {-1, 0, 0}, r[3] = {0, 0, -1}, u[3] = {0, 1, 0}, f[3] = {-1, 0, 0}; float o3[3];
      CHECK(stage_light_in_model(l, r, u, f, o3) && std::fabs(o3[2] + 1) < 1e-6f && std::fabs(o3[0]) < 1e-6f); }
    { const float l[3] = {0, 1, 0}, r[3] = {0, -1, 0}, u[3] = {1, 0, 0}, f[3] = {0, 0, -1}; float o3[3];
      CHECK(stage_light_in_model(l, r, u, f, o3) && std::fabs(o3[0] + 1) < 1e-6f); }
    { const float l[3] = {0, 0, 0}, r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, f[3] = {0, 0, -1}; float o3[3] = {7, 7, 7};
      CHECK(!stage_light_in_model(l, r, u, f, o3) && o3[0] == 7); }
    { const float l[3] = {std::nanf(""), 1, 0}, r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, f[3] = {0, 0, -1}; float o3[3]; CHECK(!stage_light_in_model(l, r, u, f, o3)); }
    // light colour: m02a ambient (110,121,105) + sun (83,83,68) -> mean-1 tint; clamped; white without one; applied per channel in shade_fragment
    { const float am[3] = {110 / 255.0f, 121 / 255.0f, 105 / 255.0f}, lc[3] = {83 / 255.0f, 83 / 255.0f, 68 / 255.0f}; float t[3];
      CHECK(stage_tint(am, lc, t) && std::fabs(t[0] - 193.0f / 190.0f) < 1e-4f && std::fabs(t[1] - 204.0f / 190.0f) < 1e-4f && std::fabs(t[2] - 173.0f / 190.0f) < 1e-4f);
      Options x; x.ambient = 1.0f; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, t); CHECK(k.tint[1] == t[1]);
      float out[4]; shade_fragment(me, k, uint16_t(kNoTexture), uv, vc, n, out);
      CHECK(std::fabs(out[1] - srgb_to_linear(16.0f / 255.0f * t[1])) < 1e-6f && std::fabs(out[2] - srgb_to_linear(16.0f / 255.0f * t[2])) < 1e-6f); }
    { const float am[3] = {1, 0, 0}, lc[3] = {0, 0, 0}; float t[3]; CHECK(stage_tint(am, lc, t) && t[0] == 1.5f && t[1] == 0.5f); }
    { const float z3[3] = {0, 0, 0}; float t[3] = {7, 7, 7}; CHECK(!stage_tint(z3, z3, t) && t[0] == 7); }
    { Options x; Constants k = make_constants(cam, x, 1.0f, 0); CHECK(k.tint[0] == 1 && k.tint[1] == 1 && k.tint[2] == 1); }
}

static void quat_ypr(float yaw, float pitch, float roll, float q[4]) {   // Ry(yaw) Rx(pitch) Rz(roll), (x,y,z,w)
    const float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2), cr = std::cos(roll / 2), sr = std::sin(roll / 2);
    const float a[4] = {0, sy, 0, cy}, b[4] = {sp, 0, 0, cp}, c[4] = {0, 0, sr, cr};
    auto mul = [](const float x[4], const float y[4], float o[4]) {
        o[0] = x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1];
        o[1] = x[3] * y[1] - x[0] * y[2] + x[1] * y[3] + x[2] * y[0];
        o[2] = x[3] * y[2] + x[0] * y[1] - x[1] * y[0] + x[2] * y[3];
        o[3] = x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2];
    };
    float t[4]; mul(a, b, t); mul(t, c, q);
}
static void project(const EyeCam& c, const float p[3], float ndc[3], float& w) {
    float v[4];
    for (int r = 0; r < 4; ++r) v[r] = c.m[4 * r] * p[0] + c.m[4 * r + 1] * p[1] + c.m[4 * r + 2] * p[2] + c.m[4 * r + 3];
    w = v[3]; for (int i = 0; i < 3; ++i) ndc[i] = v[i] / v[3];
}
static void test_camera() {
    const float pvb[3] = {0, 3.575f, -36.3f}, anchor[3] = {0.1f, 1.2f, -0.4f}, fov[4] = {-0.9f, 0.7f, 0.8f, -0.6f};
    float q[4]; quat_ypr(0, 0, 0, q);
    EyeCam c;
    CHECK(eye_camera(q, anchor, fov, anchor, 0.0f, pvb, 5.0f, 0.05f, c));
    CHECK(std::fabs(c.eye[0] - 0) < 1e-6f && std::fabs(c.eye[1] - 3.575f) < 1e-6f && std::fabs(c.eye[2] + 36.3f) < 1e-5f);   // at the seat origin: the pvb eye
    // The FOV edges: a point at tan(left) maps to x_ndc = -1, tan(right) to +1, tan(up) to y_ndc = +1, tan(down) to -1 (asymmetric).
    float nd[3], w;
    { const float p[3] = {pvb[0] + std::tan(-0.9f) * 10, pvb[1], pvb[2] - 10}; project(c, p, nd, w); CHECK(std::fabs(nd[0] + 1) < 1e-4f && std::fabs(w - 10) < 1e-4f); }
    { const float p[3] = {pvb[0] + std::tan(0.7f) * 10, pvb[1], pvb[2] - 10}; project(c, p, nd, w); CHECK(std::fabs(nd[0] - 1) < 1e-4f); }
    { const float p[3] = {pvb[0], pvb[1] + std::tan(0.8f) * 4, pvb[2] - 4}; project(c, p, nd, w); CHECK(std::fabs(nd[1] - 1) < 1e-4f); }
    { const float p[3] = {pvb[0], pvb[1] + std::tan(-0.6f) * 4, pvb[2] - 4}; project(c, p, nd, w); CHECK(std::fabs(nd[1] + 1) < 1e-4f); }
    { const float p[3] = {pvb[0], pvb[1], pvb[2] - 0.05f}; project(c, p, nd, w); CHECK(std::fabs(nd[2] - 1) < 1e-4f); }                // reversed-Z: near -> 1
    { const float p[3] = {pvb[0], pvb[1], pvb[2] - 100}; project(c, p, nd, w); CHECK(std::fabs(nd[2] - 0.0005f) < 1e-6f); }            // far -> 0
    // Head translation: 0.1 m right and 0.04 m up of the seat origin -> 0.5 / 0.2 model units (upm 5), NOT the replay's 39.6.
    { const float pos[3] = {anchor[0] + 0.1f, anchor[1] + 0.04f, anchor[2]}; CHECK(eye_camera(q, pos, fov, anchor, 0.0f, pvb, 5.0f, 0.05f, c));
      CHECK(std::fabs(c.eye[0] - 0.5f) < 1e-5f && std::fabs(c.eye[1] - 3.775f) < 1e-5f); }
    // Anchor yaw: the seat faces yaw0 = +90 deg (toward LOCAL -X). A head 0.2 m along LOCAL -X is 0.2 m FORWARD in the cockpit = -1 model unit in z.
    { float qy[4]; quat_ypr(kPi / 2, 0, 0, qy); const float pos[3] = {anchor[0] - 0.2f, anchor[1], anchor[2]};
      CHECK(eye_camera(qy, pos, fov, anchor, kPi / 2, pvb, 5.0f, 0.05f, c));
      CHECK(std::fabs(c.eye[0]) < 1e-5f && std::fabs(c.eye[2] - (pvb[2] - 1.0f)) < 1e-5f);
      CHECK(std::fabs(c.back[2] - 1) < 1e-5f && std::fabs(c.right[0] - 1) < 1e-5f); }   // looking along the seat heading = cockpit forward
    // Head yaw 90 deg left in a seat at yaw 0: the view looks down -X (left wing), right axis = -Z.
    { float qy[4]; quat_ypr(kPi / 2, 0, 0, qy); CHECK(eye_camera(qy, anchor, fov, anchor, 0.0f, pvb, 5.0f, 0.05f, c));
      CHECK(std::fabs(c.back[0] - 1) < 1e-5f && std::fabs(c.right[2] + 1) < 1e-5f); }
    // Pitch up 30 deg, roll 20 deg: orthonormal basis.
    { float qr[4]; quat_ypr(0.3f, 0.5f, 0.35f, qr); CHECK(eye_camera(qr, anchor, fov, anchor, 0.7f, pvb, 5.0f, 0.05f, c));
      auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
      CHECK(std::fabs(dot(c.right, c.up)) < 1e-5f && std::fabs(dot(c.right, c.back)) < 1e-5f && std::fabs(dot(c.right, c.right) - 1) < 1e-5f); }
    // Refusals.
    { const float nanq[4] = {std::nanf(""), 0, 0, 1}; CHECK(!eye_camera(nanq, anchor, fov, anchor, 0, pvb, 5, 0.05f, c)); }
    { const float zq[4] = {0, 0, 0, 0}; CHECK(!eye_camera(zq, anchor, fov, anchor, 0, pvb, 5, 0.05f, c)); }
    { const float bf[4] = {0.7f, -0.9f, 0.8f, -0.6f}; CHECK(!eye_camera(q, anchor, bf, anchor, 0, pvb, 5, 0.05f, c)); }
    { const float bf[4] = {-1.6f, 0.7f, 0.8f, -0.6f}; CHECK(!eye_camera(q, anchor, bf, anchor, 0, pvb, 5, 0.05f, c)); }
    CHECK(!eye_camera(q, anchor, fov, anchor, 0, pvb, 0.0f, 0.05f, c));
    CHECK(!eye_camera(q, anchor, fov, anchor, std::nanf(""), pvb, 5, 0.05f, c));
}

// ---------------------------------------------------------------- COCKPIT-SCALE (cscale:, D-355 "a little bigger")
// Reference = the model PHYSICALLY scaled by s about the pvb eye (p' = pvb + s (p - pvb)) seen with the unscaled camera (upm, near). The implementation
// keeps the mesh and uses view_upm/view_near (upm/s, near/s). They must agree: clip_impl(p) * s == clip_ref(p') for all four components (same NDC and
// reversed-Z depth), and from the centred eye every vertex ray keeps its direction while its distance scales by s.
static void test_cscale() {
    std::string id, why; Options o;
    CHECK(parse_options("m", id, o, why) && o.cscale == 1.0f);
    CHECK(parse_options("m;cscale:1.08", id, o, why) && o.cscale == 1.08f);
    CHECK(parse_options("m;cscale=0.8;upm:5", id, o, why) && o.cscale == 0.8f);
    CHECK(parse_options("m;cscale:1.5", id, o, why) && o.cscale == 1.5f);
    for (const char* b : {"m;cscale:0.79", "m;cscale:1.51", "m;cscale:nan", "m;cscale:NaN", "m;cscale:inf", "m;cscale:-1", "m;cscale:", "m;cscale:big", "m;cscale:0"}) {
        Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2) && w2 == "bad value for cscale"); }
    // cscale 1: bit-identical camera (and the mesh does not depend on cscale at all).
    const float pvb[3] = {0, 3.575f, -36.3f}, anchor[3] = {0.1f, 1.2f, -0.4f}, fov[4] = {-0.9f, 0.7f, 0.8f, -0.6f};
    Options d; CHECK(view_upm(d) == d.upm && view_near(d) == d.near_u);
    { Options u; u.upm = 5.37f; u.near_u = 0.031f; CHECK(view_upm(u) == 5.37f && view_near(u) == 0.031f); }
    float qr[4]; quat_ypr(0.3f, -0.4f, 0.2f, qr);
    const float head[3] = {anchor[0] + 0.032f, anchor[1] + 0.05f, anchor[2] - 0.07f};   // a left-eye-like offset (half IPD) + lean
    { EyeCam a, b; CHECK(eye_camera(qr, head, fov, anchor, 0.4f, pvb, d.upm, d.near_u, a) && eye_camera(qr, head, fov, anchor, 0.4f, pvb, view_upm(d), view_near(d), b));
      CHECK(std::memcmp(&a, &b, sizeof a) == 0); }
    { ModelData m = make_synthetic_model_c4(); Mesh a, b; Options x; Options y; y.cscale = 1.3f;
      CHECK(build_mesh(m, x, a, why) && build_mesh(m, y, b, why) && a.v.size() == b.v.size() && std::memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(GpuVertex)) == 0); }
    // cscale != 1: equivalence with the physically scaled model at several head poses (centred, IPD offset, lean + yawed seat), every mesh vertex.
    ModelData m = make_synthetic_model_c4(); Options base; base.interior = true; Mesh me; CHECK(build_mesh(m, base, me, why));
    for (float s : {0.8f, 1.08f, 1.25f, 1.5f}) {
        Options o2 = base; o2.cscale = s;
        const float poses[3][3] = {{anchor[0], anchor[1], anchor[2]}, {anchor[0] - 0.032f, anchor[1], anchor[2]}, {head[0], head[1], head[2]}};
        for (int pi = 0; pi < 3; ++pi) {
            EyeCam ci, cr;   // implementation (unscaled mesh, view_upm/view_near) and reference (scaled mesh, upm/near)
            CHECK(eye_camera(qr, poses[pi], fov, anchor, 0.4f, me.eye, view_upm(o2), view_near(o2), ci));
            CHECK(eye_camera(qr, poses[pi], fov, anchor, 0.4f, me.eye, o2.upm, o2.near_u, cr));
            // The scaled model's eye: pvb + s * (impl eye - pvb) == the reference eye (head translation enters in metres * upm, IPD included).
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(me.eye[k] + s * (ci.eye[k] - me.eye[k]) - cr.eye[k]) < 1e-4f);
            for (const GpuVertex& gv : me.v) {
                float ps[3]; for (int k = 0; k < 3; ++k) ps[k] = me.eye[k] + s * (gv.pos[k] - me.eye[k]);
                double vi[4], vr[4];
                for (int r = 0; r < 4; ++r) {
                    vi[r] = double(ci.m[4 * r]) * gv.pos[0] + double(ci.m[4 * r + 1]) * gv.pos[1] + double(ci.m[4 * r + 2]) * gv.pos[2] + ci.m[4 * r + 3];
                    vr[r] = double(cr.m[4 * r]) * ps[0] + double(cr.m[4 * r + 1]) * ps[1] + double(cr.m[4 * r + 2]) * ps[2] + cr.m[4 * r + 3];
                }
                for (int r = 0; r < 4; ++r) CHECK(std::fabs(vi[r] * s - vr[r]) < 2e-3 * (1.0 + std::fabs(vr[r])));
                if (std::fabs(vr[3]) > 0.5) for (int r = 0; r < 3; ++r) CHECK(std::fabs(vi[r] / vi[3] - vr[r] / vr[3]) < 1e-3);   // same NDC + depth
                if (pi == 0) {   // centred eye (= pvb): the ray through the vertex keeps its direction, its length scales by s
                    double di[3], dr[3], li = 0, lr = 0;
                    for (int k = 0; k < 3; ++k) { di[k] = double(gv.pos[k]) - ci.eye[k]; dr[k] = double(ps[k]) - cr.eye[k]; li += di[k] * di[k]; lr += dr[k] * dr[k]; }
                    li = std::sqrt(li); lr = std::sqrt(lr);
                    if (li > 1e-3) { CHECK(std::fabs(lr / li - s) < 1e-4 * s);
                        for (int k = 0; k < 3; ++k) CHECK(std::fabs(di[k] / li - dr[k] / lr) < 1e-5); }
                    // and the image of the UNSCALED model is unchanged by cscale at the centred eye (HUD / world registration): same NDC x/y, w unchanged.
                    EyeCam c1; CHECK(eye_camera(qr, poses[0], fov, anchor, 0.4f, me.eye, base.upm, base.near_u, c1));
                    double v1[4]; for (int r = 0; r < 4; ++r) v1[r] = double(c1.m[4 * r]) * gv.pos[0] + double(c1.m[4 * r + 1]) * gv.pos[1] + double(c1.m[4 * r + 2]) * gv.pos[2] + c1.m[4 * r + 3];
                    if (std::fabs(v1[3]) > 0.5) { CHECK(std::fabs(v1[0] / v1[3] - vi[0] / vi[3]) < 1e-5 && std::fabs(v1[1] / v1[3] - vi[1] / vi[3]) < 1e-5);
                        CHECK(std::fabs((v1[2] / v1[3]) / (vi[2] / vi[3]) - s) < 1e-4 * s); }   // reversed-Z depth: physically s times farther
                }
            }
        }
        // Stereo: the eye separation in model units shrinks by s (the model looks s times bigger); the HUD glass (hudglass:1) is placed s times farther in metres
        // at the same angular size (the world-quad HUD keeps its lens-derived angular size; only its distance comes from the glass).
        { const float le[3] = {anchor[0] - 0.032f, anchor[1], anchor[2]}, re[3] = {anchor[0] + 0.032f, anchor[1], anchor[2]}; float q0[4]; quat_ypr(0, 0, 0, q0);
          EyeCam l, r; CHECK(eye_camera(q0, le, fov, anchor, 0.0f, me.eye, view_upm(o2), view_near(o2), l) && eye_camera(q0, re, fov, anchor, 0.0f, me.eye, view_upm(o2), view_near(o2), r));
          CHECK(std::fabs((r.eye[0] - l.eye[0]) - 0.064f * base.upm / s) < 1e-4f); }
        CHECK(me.hud.valid && std::fabs(me.hud.distance / view_upm(o2) - s * me.hud.distance / base.upm) < 1e-5f * s * me.hud.distance);
    }
}

// ---------------------------------------------------------------- CPU reference raster: landmark checks
struct Shot { Target t; };
static char cls(const float* p) {   // dominant colour class of a premultiplied pixel ('.' = empty)
    if (p[3] < 0.05f) return '.';
    const float r = p[0], g = p[1], b = p[2];
    if (r > 0.45f && g > 0.45f && b < 0.3f) return 'Y';
    if (g > 0.45f && b > 0.45f && r < 0.3f) return 'C';
    if (r > 0.45f && b > 0.45f && g < 0.3f) return 'M';
    if (r > 0.45f && g < 0.25f && b < 0.25f) return 'R';
    if (g > 0.4f && r < 0.25f && b < 0.25f) return 'G';
    if (b > 0.45f && r < 0.25f && g < 0.35f) return 'B';
    if (r > 0.5f && g > 0.5f && b > 0.5f) return 'W';
    return '?';
}
static Target shoot(const Mesh& me, const Options& o, const float q[4], const float pos[3], const float anchor[3], float yaw, uint32_t w = 320, uint32_t h = 180) {
    const float fov[4] = {-0.95f, 0.95f, 0.75f, -0.75f};
    EyeCam c; CHECK(eye_camera(q, pos, fov, anchor, yaw, me.eye, o.upm, o.near_u, c));
    Target t; t.w = w; t.h = h; t.rgba.assign(size_t(w) * h * 4, 0.0f); t.depth.assign(size_t(w) * h, 0.0f);
    render_reference(me, o, 1.0f, 0, c, t);
    return t;
}
struct Blob { size_t n = 0; float x = 0, y = 0; };
static Blob blob(const Target& t, char k) {
    Blob b; double sx = 0, sy = 0;
    for (uint32_t y = 0; y < t.h; ++y) for (uint32_t x = 0; x < t.w; ++x) if (cls(&t.rgba[(size_t(y) * t.w + x) * 4]) == k) { ++b.n; sx += x; sy += y; }
    if (b.n) { b.x = float(sx / b.n); b.y = float(sy / b.n); }
    return b;
}
static void test_reference_landmarks() {
    const ModelData m = make_synthetic_model();
    // Geometry landmarks: the COCKPIT-2 colour path (raw domain, gain 1.15) keeps the colour classes saturated; no floor closure (tested in test_mesh).
    Options o; o.display = false; o.gain = 1.15f; o.floor = false; o.interior = false; Mesh me; std::string why; CHECK(build_mesh(m, o, me, why));
    const float anchor[3] = {0, 1.2f, 0};
    float q0[4]; quat_ypr(0, 0, 0, q0);
    const Target t = shoot(me, o, q0, anchor, anchor, 0.0f);
    const Blob R = blob(t, 'R'), G = blob(t, 'G'), B = blob(t, 'B'), Y = blob(t, 'Y'), C = blob(t, 'C'), M = blob(t, 'M');
    std::printf("  forward: R n=%zu (%.1f,%.1f) G n=%zu (%.1f,%.1f) B n=%zu (%.1f,%.1f) Y n=%zu C n=%zu M n=%zu\n", R.n, R.x, R.y, G.n, G.x, G.y, B.n, B.x, B.y, Y.n, C.n, M.n);
    CHECK(R.n > 200 && G.n > 200 && B.n > 100);
    CHECK(R.x < 160 && G.x > 160 && std::fabs(R.y - G.y) < 1.0f);   // red LEFT, green RIGHT, same height: no mirror
    CHECK(B.y < R.y - 20 && std::fabs(B.x - 160) < 2.0f);             // blue UP, centred (row 0 = top)
    CHECK(M.n == 0);                                                  // the magenta quad faces away: culled
    CHECK(Y.n > 100 && C.n > 100);
    // depth order: in the overlap the near yellow wins; the far cyan shows only outside it
    { const float yc = Y.x, yr = Y.y; size_t ylap = 0;
      for (uint32_t y = 0; y < t.h; ++y) for (uint32_t x = 0; x < t.w; ++x) if (cls(&t.rgba[(size_t(y) * t.w + x) * 4]) == 'Y' && x < 165) ++ylap;
      CHECK(ylap > 30); (void)yc; (void)yr;
      CHECK(cls(&t.rgba[(size_t(108) * 320 + 163) * 4]) == 'Y' && cls(&t.rgba[(size_t(108) * 320 + 152) * 4]) == 'C'); }
    { Options rev = o; Mesh far_first; ModelData n = m; std::swap(n.parts[3], n.parts[4]);   // draw order must not matter for opaque depth
      for (size_t i = 0; i < n.parts.size(); ++i) n.parts[i].node = uint32_t(i);
      CHECK(build_mesh(n, rev, far_first, why)); const Target t2 = shoot(far_first, rev, q0, anchor, anchor, 0.0f);
      size_t diff = 0; for (size_t i = 0; i < t.rgba.size(); ++i) if (std::fabs(t.rgba[i] - t2.rgba[i]) > 1e-6f) ++diff;
      CHECK(diff == 0); }
    // alpha test: the checker's hole (texels 3..4) is transparent, its rim is opaque white/red
    { const float c3[3] = {3.0f, 0.5f, -45.0f}; float nd[3], w; EyeCam cam; const float fov[4] = {-0.95f, 0.95f, 0.75f, -0.75f};
      CHECK(eye_camera(q0, anchor, fov, anchor, 0.0f, me.eye, 5.0f, 0.05f, cam)); project(cam, c3, nd, w);
      const int px = int((nd[0] * 0.5f + 0.5f) * 320), py = int((0.5f - nd[1] * 0.5f) * 180);
      CHECK(t.rgba[(size_t(py) * 320 + px) * 4 + 3] == 0.0f);                         // hole centre: nothing drawn
      CHECK(t.rgba[(size_t(py) * 320 + px + 6) * 4 + 3] == 1.0f); }                   // 6 px right: checker texel, opaque
    // glass: translucent (0 < a < 1) over nothing
    { size_t glass = 0; for (size_t i = 0; i < t.rgba.size(); i += 4) if (t.rgba[i + 3] > 0.1f && t.rgba[i + 3] < 0.4f) ++glass; CHECK(glass > 50); }
    // the hidden pilot (1 unit ahead, 6 units wide) would cover everything: nothing magenta, and the exterior tail is behind the eye
    CHECK(blob(t, 'W').n > 0);
    // --- 6DoF rotation: yaw 180 (looking back) shows the exterior tail (skin grey, opaque even though its texture alpha is 0.3) and no landmark
    { float qb[4]; quat_ypr(kPi, 0, 0, qb); const Target b = shoot(me, o, qb, anchor, anchor, 0.0f);
      size_t opaque = 0; for (size_t i = 0; i < b.rgba.size(); i += 4) if (b.rgba[i + 3] == 1.0f) ++opaque;
      std::printf("  back: opaque=%zu R=%zu\n", opaque, blob(b, 'R').n);
      CHECK(opaque > 2000 && blob(b, 'R').n == 0 && blob(b, 'G').n == 0);
      Options noext = o; noext.exterior = false; Mesh m2; CHECK(build_mesh(m, noext, m2, why));
      const Target b2 = shoot(m2, noext, qb, anchor, anchor, 0.0f); size_t op2 = 0; for (size_t i = 0; i < b2.rgba.size(); i += 4) if (b2.rgba[i + 3] > 0) ++op2;
      CHECK(op2 == 0); }
    // --- roll 30 deg (head tilted left = CCW): the blue bar above the centre moves LEFT on screen and red/green are no longer level
    { float qr[4]; quat_ypr(0, 0, 0.5236f, qr); const Target r = shoot(me, o, qr, anchor, anchor, 0.0f);
      const Blob rR = blob(r, 'R'), rG = blob(r, 'G'), rB = blob(r, 'B');
      std::printf("  roll+30: B (%.1f,%.1f) R (%.1f,%.1f) G (%.1f,%.1f)\n", rB.x, rB.y, rR.x, rR.y, rG.x, rG.y);
      CHECK(rB.x > 165 && rR.y < rG.y - 10); }   // head rolled left (CCW): the world rotates CW on screen: up-bar moves right, left panel rises
    // --- pitch up 20 deg: everything moves DOWN on screen
    { float qp[4]; quat_ypr(0, 0.349f, 0, qp); const Target p = shoot(me, o, qp, anchor, anchor, 0.0f);
      CHECK(blob(p, 'R').y > R.y + 20 && blob(p, 'B').y > B.y + 20); }
    // --- yaw left 15 deg (head turns toward -X): landmarks move RIGHT on screen
    { float qy[4]; quat_ypr(0.2618f, 0, 0, qy); const Target p = shoot(me, o, qy, anchor, anchor, 0.0f);
      CHECK(blob(p, 'R').x > R.x + 20 && blob(p, 'G').x > G.x + 20); }
    // --- per-eye parallax: left eye (-32 mm) sees the near yellow further RIGHT than the right eye does; the disparity of the near yellow (8 u)
    //     exceeds that of the far cyan (14 u) and of the red panel (10 u)
    { const float le[3] = {anchor[0] - 0.032f, anchor[1], anchor[2]}, re[3] = {anchor[0] + 0.032f, anchor[1], anchor[2]};
      const Target L = shoot(me, o, q0, le, anchor, 0.0f), Rr = shoot(me, o, q0, re, anchor, 0.0f);
      const float dY = blob(L, 'Y').x - blob(Rr, 'Y').x, dR = blob(L, 'R').x - blob(Rr, 'R').x, dG = blob(L, 'G').x - blob(Rr, 'G').x;
      std::printf("  disparity px: yellow(8u)=%.2f red(10u)=%.2f green(10u)=%.2f\n", dY, dR, dG);
      CHECK(dY > 0.5f && dR > 0.3f && dY > dR && std::fabs(dR - dG) < 0.3f);
      // expected ~ f * baseline / depth = (160/tan(0.95)) * 0.32 u / 10 u
      const float f = 160.0f / std::tan(0.95f);
      CHECK(std::fabs(dR - f * 0.32f / 10.0f) < 0.4f); }
    // --- head translation (lean 0.3 m left): the near landmarks move right more than the far ones (motion parallax at 5 u/m)
    { const float lp[3] = {anchor[0] - 0.3f, anchor[1], anchor[2]}; const Target p = shoot(me, o, q0, lp, anchor, 0.0f);
      const float dY = blob(p, 'Y').x - Y.x, dB = blob(p, 'B').x - B.x;
      std::printf("  lean-left shifts: yellow(8u)=%.1f blue(10u)=%.1f\n", dY, dB);
      CHECK(dY > dB && dB > 5.0f); }
    // --- determinism
    { const Target t2 = shoot(me, o, q0, anchor, anchor, 0.0f); CHECK(t2.rgba == t.rgba && t2.depth == t.depth); }
}

// ---------------------------------------------------------------- COCKPIT-4: canopy glass, procedural interior, animation
static float srgb_encode(float v) { v = std::min(1.0f, std::max(0.0f, v)); return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }
static void centroid_of(const std::vector<GpuVertex>& v, uint32_t first, uint32_t count, float c[3]) {
    double s[3] = {0, 0, 0};
    for (uint32_t i = 0; i < count; ++i) for (int a = 0; a < 3; ++a) s[a] += v[first + i].pos[a];
    for (int a = 0; a < 3; ++a) c[a] = float(s[a] / count);
}
static void test_c4_options_loader() {
    std::string id, why; Options o;
    CHECK(parse_options("m", id, o, why) && o.canopy && o.interior && !o.anim && o.refl == 0.2f && o.dirt == 0.0f && o.tint[0] == 0.951f && o.tint[2] == 0.935f);
    CHECK(parse_options("m;canopy:0;interior:0;anim:1;refl:0.5;dirt:0.1;tint:0.9,0.8,0.7", id, o, why) && !o.canopy && !o.interior && o.anim && o.refl == 0.5f &&
          o.dirt == 0.1f && o.tint[0] == 0.9f && o.tint[1] == 0.8f && o.tint[2] == 0.7f);
    CHECK(parse_options("m;stick:side", id, o, why) && o.stick == 1 && parse_options("m;stick:none", id, o, why) && o.stick == 2 &&
          parse_options("m;stick:centre", id, o, why) && o.stick == 0 && parse_options("m", id, o, why) && o.stick == 0);
    for (const char* b : {"m;stick:left", "m;canopy:2", "m;anim:yes", "m;interior:", "m;refl:2", "m;dirt:0.6", "m;tint:1,1", "m;tint:1,1,1,1", "m;tint:1.2,1,1", "m;tint:a,b,c"}) {
        Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    // round trip of the COCKPIT-4 part fields
    const ModelData m = make_synthetic_model_c4();
    const auto bytes = serialize_model(m);
    ModelData t; CHECK(parse_model(bytes.data(), bytes.size(), t) == LoadStatus::ok);
    CHECK(t.parts.size() == 12 && t.parts[11].rest == 0.0f && t.parts[11].aflags == kPartHingeFitted && t.parts[11].group == 2 && t.parts[11].anim == 0);
    CHECK(std::fabs(t.parts[11].hinge - 0.349f) < 1e-6f && (t.flags & kZvrmAnimRest) && serialize_model(t) == bytes);
    { ModelData r = m; r.parts[11].rest = 3.14159265f; const auto b2 = serialize_model(r); ModelData u;
      CHECK(parse_model(b2.data(), b2.size(), u) == LoadStatus::ok && u.parts[11].rest == r.parts[11].rest); }
}
static void test_c4_mesh() {
    const ModelData m = make_synthetic_model_c4();
    std::string why;
    // defaults: canopy glass (part 10) leaves the blend range for the glass range; the HUD glass is still part 7; interior on; anim off
    { Options o; Mesh me; CHECK(build_mesh(m, o, me, why));
      std::printf("  c4 default: tris=%u opaque=%u blend=%u glass=%u anim=%u+%u pieces=%zu interior=%u tris (floor y %.3f)\n", me.triangles, me.opaque_vertices,
          me.blend_vertices, me.glass_vertices, me.anim_first, me.anim_vertices, me.anim.size(), me.interior_triangles, me.interior_floor_y);
      CHECK(me.canopy_parts == 1 && !me.canopy_exterior && me.glass_vertices == 6 && me.blend_vertices == 6 && me.hud.valid && me.hud.part == 7);
      CHECK(me.v[me.opaque_vertices + me.blend_vertices].pos[2] == -40.0f);
      CHECK(me.interior && !me.floor && me.interior_triangles > 60 && me.anim.size() == 4 && me.anim_surfaces == 0);   // stick, throttle, 2 pedals
      CHECK(me.anim_first + me.anim_vertices == me.opaque_vertices && me.anim_vertices == (3 + 2 + 1 + 1) * 36);
      CHECK(me.anim[0].role == AnimRole::stick && me.anim[0].naxes == 2 && me.anim[1].role == AnimRole::throttle && me.anim[2].role == AnimRole::pedal);
      // the tub floor lies 1.05 m (5.25 u) below the eye; every interior vertex is inside the cockpit footprint x -4..4, z -50..-44 or the seat volume behind
      CHECK(std::fabs(me.interior_floor_y - (3.5f - 5.25f)) < 1e-5f);
      // winding: every triangle (model quads, tub, seat, stick, throttle, pedals) is CCW-front on the side its vertex normal points to (back-face culling keeps
      // exactly the faces seen from inside the tub / outside the boxes)
      size_t bad = 0;
      for (size_t i = 0; i + 2 < me.v.size(); i += 3) {
          const float* a = me.v[i].pos; const float* b = me.v[i + 1].pos; const float* c = me.v[i + 2].pos;
          const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
          const float g[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
          if (g[0] * me.v[i].nrm[0] + g[1] * me.v[i].nrm[1] + g[2] * me.v[i].nrm[2] <= 0.0f) ++bad;
      }
      CHECK(bad == 0);
      // the elevator (part 11) is static without anim:1 (it is in the opaque static range)
      bool found = false; for (uint32_t i = 0; i < me.anim_first; ++i) if (me.v[i].pos[0] == 5.0f && me.v[i].pos[2] == -43.0f) found = true; CHECK(found); }
    // stick variants: side-stick = a short grip on the right console (x > +0.25 m), none = no stick piece
    { Options o; o.stick = 1; Mesh me; CHECK(build_mesh(m, o, me, why) && me.anim.size() == 4 && me.anim[0].role == AnimRole::stick && me.anim[0].pivot[0] > 1.25f && me.anim[0].count == 72);
      o.stick = 2; Mesh mn; CHECK(build_mesh(m, o, mn, why) && mn.anim.size() == 3 && mn.anim[0].role == AnimRole::throttle); }
    { Options o; o.canopy = false; o.interior = false; Mesh me; CHECK(build_mesh(m, o, me, why) && me.glass_vertices == 0 && me.blend_vertices == 12 && me.canopy_parts == 0 && me.anim.empty()); }
    { Options o; o.glass = false; o.interior = false; Mesh me; CHECK(build_mesh(m, o, me, why) && me.glass_vertices == 0 && me.canopy_parts == 0); }   // glass:0 hides it
    // anim:1: the elevator joins the animated range (6 corners) ahead of the interior pieces; its hinge = local X through (4, 1, -44)
    { Options o; o.anim = true; Mesh me; CHECK(build_mesh(m, o, me, why) && me.anim_surfaces == 1 && me.anim.size() == 5 && me.anim[0].role == AnimRole::pitch_surface);
      CHECK(me.anim[0].first == me.anim_first && me.anim[0].count == 6 && me.anim[0].pivot[0] == 4.0f && me.anim[0].pivot[2] == -44.0f && me.anim[0].ax[0].axis[0] == 1.0f);
      CHECK(std::fabs(std::fabs(me.anim[0].ax[0].gain) - 0.349f) < 1e-6f && me.anim[0].ax[0].cp == 1.0f && me.anim[0].ax[0].cr == 0.5f);
      // not located / old bake / no role / no range: not driven
      ModelData n = m; n.parts[11].aflags = 0; n.parts[11].rest = 3.14159265f; n.parts[11].hinge = 3.14159265f - 0.349f; Mesh y;   // unfitted non-zero rest
      CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 0 && y.anim_unlocated == 1);
      n = m; n.parts[11].aflags = 0; CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 1);                    // unfitted, rest 0, stored 20 deg: the classic range
      n = m; n.flags &= ~kZvrmAnimRest; n.parts[11].aflags = 0; CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 1);   // an old bake: same reading
      n = m; n.parts[11].hinge = 2.79f; n.parts[11].aflags = 0; CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 0);  // old-bake outer elevon (159.9 deg): no
      n = m; n.parts[11].group = 0xf; CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 0 && y.anim_unlocated == 1);
      n = m; n.parts[11].hinge = 0.0f; CHECK(build_mesh(n, o, y, why) && y.anim_surfaces == 0); }
    // exterior canopy fallback: no cockpit canopy -> an exterior blend part around the eye, flipped to face the eye (winding + normals)
    { ModelData n = m; n.parts[10].mflags = MF_BLEND | MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE;
      for (uint32_t k = 0; k < 4; ++k) n.vertices[n.parts[10].first_vertex + k].nrm[2] = -32767;   // outward normal (away from the eye)
      std::swap(n.indices[n.parts[10].first_index + 1], n.indices[n.parts[10].first_index + 2]); std::swap(n.indices[n.parts[10].first_index + 4], n.indices[n.parts[10].first_index + 5]);
      Options o; o.interior = false; Mesh me; CHECK(build_mesh(n, o, me, why) && me.canopy_parts == 1 && me.canopy_exterior && me.glass_vertices == 6);
      const GpuVertex* g = &me.v[me.opaque_vertices + me.blend_vertices];
      CHECK(g[0].nrm[2] == 32767);
      const float ax = g[1].pos[0] - g[0].pos[0], ay = g[1].pos[1] - g[0].pos[1], bx = g[2].pos[0] - g[0].pos[0], by = g[2].pos[1] - g[0].pos[1];
      CHECK(ax * by - ay * bx > 0.0f); }   // CCW seen from +Z (the eye side) again
    // both present: the exterior canopy glass wins and the cockpit part's canopy glass is not drawn (no double tint)
    { ModelData n = m; Part e = n.parts[10]; e.mflags = MF_BLEND | MF_EXTERIOR; n.parts.push_back(e);
      Options o; o.interior = false; Mesh me; CHECK(build_mesh(n, o, me, why) && me.canopy_parts == 1 && me.canopy_exterior && me.glass_vertices == 6 && me.blend_vertices == 6); }
    // the interior footprint clamp: a model whose cockpit ends 0.5 m behind the eye keeps the seat inside it
    { Options o; Mesh me; ModelData n = m; for (auto& v : n.vertices) if (v.pos[2] > -36.0f && !(n.parts[v.part].mflags & MF_EXTERIOR)) v.pos[2] = -36.0f;
      CHECK(build_mesh(n, o, me, why) && me.interior); }
}
static void test_c4_glass() {
    // shade_glass at normal incidence (view along the normal): F = F0; veil = refl F0 + dirt; trans = tint^2.2 (1 - veil); add = sky_linear * veil
    Options o; o.refl = 0.25f; o.dirt = 0.05f;
    EyeCam cam{}; cam.eye[0] = 0; cam.eye[1] = 3.5f; cam.eye[2] = -36.0f;
    const float sky[3] = {0.6f, 0.7f, 0.8f};
    Constants k = make_constants(cam, o, 1.0f, 0, nullptr, nullptr, sky);
    CHECK(k.eye[2] == -36.0f && k.f0 == kGlassF0 && k.refl == 0.25f && k.dirt == 0.05f && k.sky[1] == 0.7f);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(k.trans[i] - std::pow(o.tint[i], 2.2f)) < 1e-6f);
    const float p[3] = {0, 3.5f, -40.0f}, n[3] = {0, 0, 1};
    float add[3], tr[3], a;
    shade_glass(k, p, n, add, tr, a);
    const float veil = 0.25f * 0.04f + 0.05f;
    for (int i = 0; i < 3; ++i) { CHECK(std::fabs(tr[i] - k.trans[i] * (1 - veil)) < 1e-6f); CHECK(std::fabs(add[i] - srgb_to_linear(sky[i]) * veil) < 1e-6f); }
    CHECK(std::fabs(a - (1.0f - (tr[0] + tr[1] + tr[2]) / 3.0f)) < 1e-6f);
    // the back side gives the same (abs(n.v)); grazing incidence reflects more and transmits less (Fresnel)
    { const float nb[3] = {0, 0, -1}; float a2[3], t2[3], g2; shade_glass(k, p, nb, a2, t2, g2); CHECK(a2[0] == add[0] && t2[1] == tr[1]); }
    { const float pg[3] = {0, 3.5f + 40.0f, -40.0f}; float a2[3], t2[3], g2; shade_glass(k, pg, n, a2, t2, g2); CHECK(a2[2] > 2 * add[2] && t2[0] < tr[0]); }
    // the measured transmission: with no reflection and no veil, a mid-grey/sky-level display value seen through the glass keeps the game's ratio (WORLD-4
    // (0.951, 0.940, 0.935) on the sky rows, display ~0.55-0.80) within 0.005 per channel
    { Options g; g.refl = 0; Constants kg = make_constants(cam, g, 1.0f, 0); float a3[3], t3[3], g3; shade_glass(kg, p, n, a3, t3, g3);
      for (float d : {0.55f, 0.65f, 0.80f}) for (int i = 0; i < 3; ++i) {
          const float out = srgb_encode(a3[i] + t3[i] * srgb_to_linear(d)) / d;
          if (std::fabs(out - g.tint[i]) >= 0.005f) std::printf("  tint d=%.2f c=%d ratio %.4f want %.4f\n", d, i, out, g.tint[i]);
          CHECK(std::fabs(out - g.tint[i]) < 0.005f); } }
    // CPU reference: only the canopy drawn over a known background; each covered pixel = add + trans * bg exactly (pass 2 dual-source rule), alpha = a + (1-a) bg.a
    const ModelData m = make_synthetic_model_c4();
    Options c; c.interior = false; c.refl = 0.25f; c.dirt = 0.05f;
    for (uint32_t i = 0; i < 12; ++i) if (i != 10) c.hide.push_back(i);
    Mesh me; std::string why; CHECK(build_mesh(m, c, me, why) && me.opaque_vertices == 0 && me.blend_vertices == 0 && me.glass_vertices == 6);
    const float fov[4] = {-0.95f, 0.95f, 0.75f, -0.75f}, zero[3] = {0, 0, 0}; float q[4]; quat_ypr(0, 0, 0, q);
    EyeCam ec; CHECK(eye_camera(q, zero, fov, zero, 0.0f, me.eye, c.upm, c.near_u, ec));
    Target t; t.w = 160; t.h = 90; t.rgba.assign(size_t(160) * 90 * 4, 0.0f); t.depth.assign(size_t(160) * 90, 0.0f);
    const float bg[4] = {0.5f, 0.4f, 0.3f, 0.6f};
    for (size_t i = 0; i < size_t(160) * 90; ++i) for (int ch = 0; ch < 4; ++ch) t.rgba[i * 4 + ch] = bg[ch];
    render_reference(me, c, 1.0f, 0, ec, t, nullptr, nullptr, sky);
    const Constants kk = make_constants(ec, c, 1.0f, 0, nullptr, nullptr, sky);
    size_t covered = 0; float worst = 0;
    for (uint32_t y = 0; y < 90; ++y) for (uint32_t x = 0; x < 160; ++x) {
        const float* d = &t.rgba[(size_t(y) * 160 + x) * 4];
        if (d[0] == bg[0] && d[1] == bg[1] && d[2] == bg[2]) continue;
        ++covered;
        // the ray through the pixel centre hits z = -40 at (eye + s * dir)
        const float nx = ((x + 0.5f) / 160.0f) * 2 - 1, ny = 1 - ((y + 0.5f) / 90.0f) * 2;
        const float dx = nx * std::tan(0.95f), dy = ny * std::tan(0.75f);
        const float pp[3] = {me.eye[0] + dx * 4.0f, me.eye[1] + dy * 4.0f, -40.0f};
        float ad[3], tt[3], ga; shade_glass(kk, pp, n, ad, tt, ga);
        for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::fabs(d[ch] - (ad[ch] + tt[ch] * bg[ch])));
        worst = std::max(worst, std::fabs(d[3] - (ga + (1 - ga) * bg[3])));
    }
    std::printf("  canopy CPU reference: covered=%zu worst |error| vs the dual-source rule %.2e\n", covered, worst);
    CHECK(covered > 3000 && worst < 2e-4f);
    // depth: the glass does not cover an opaque quad in front of it (the near yellow at z -44 is BEHIND the canopy at -40: covered; a quad at -38 is not)
}
static void quat_to_axes(const float q[4], float right[3], float up[3], float fwd[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float c0[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
    const float c1[3] = {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)};
    const float c2[3] = {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; ++i) { right[i] = c0[i]; up[i] = c1[i]; fwd[i] = -c2[i]; }
}
static void test_c4_anim() {
    // anim_angle: gain * clamp(bias + cp p + cr r + cy y + ct t)
    { AnimAxis a; a.gain = 0.5f; a.cp = 1; a.cr = 0.5f; AnimInputs in; in.pitch = 0.4f; in.roll = 0.4f; CHECK(std::fabs(anim_angle(a, in) - 0.3f) < 1e-6f);
      in.pitch = 2; CHECK(anim_angle(a, in) == 0.5f); in.pitch = std::nanf(""); CHECK(anim_angle(a, in) == 0.0f); }
    CHECK(surface_role(1) == AnimRole::pitch_surface && surface_role(2) == AnimRole::pitch_surface && surface_role(3) == AnimRole::rudder &&
          surface_role(5) == AnimRole::roll_surface && surface_role(6) == AnimRole::roll_surface && surface_role(7) == AnimRole::none && surface_role(0xf) == AnimRole::none);
    const ModelData m = make_synthetic_model_c4();
    Options o; o.anim = true; Mesh me; std::string why; CHECK(build_mesh(m, o, me, why));
    std::vector<GpuVertex> rest, pose;
    pose_anim(me, AnimInputs{}, rest);
    CHECK(rest.size() == me.anim_vertices);
    { AnimInputs neutral; neutral.throttle = 0.5f; std::vector<GpuVertex> p0; pose_anim(me, neutral, p0);   // neutral = rest within rounding
      float worst = 0; for (size_t i = 0; i < p0.size(); ++i) for (int a = 0; a < 3; ++a) worst = std::max(worst, std::fabs(p0[i].pos[a] - me.v[me.anim_first + i].pos[a]));
      CHECK(worst < 1e-5f); }
    // elevator: pitch +1 (stick aft) lifts the trailing edge (z -43) by sin(20 deg) about the hinge (z -44); the hinge line stays; the normal tilts forward
    AnimInputs in; in.pitch = 1.0f;
    pose_anim(me, in, pose);
    const AnimPiece& el = me.anim[0];
    for (uint32_t i = 0; i < el.count; ++i) {
        const GpuVertex& r0 = me.v[el.first + i]; const GpuVertex& g = pose[el.first - me.anim_first + i];
        if (r0.pos[2] == -44.0f) CHECK(std::fabs(g.pos[1] - 1.0f) < 1e-5f && std::fabs(g.pos[2] + 44.0f) < 1e-5f);
        else { CHECK(std::fabs(g.pos[1] - (1.0f + std::sin(0.349f))) < 1e-5f && std::fabs(g.pos[2] - (-44.0f + std::cos(0.349f))) < 1e-5f); }
        CHECK(g.nrm[2] < -10000 && g.nrm[1] > 30000);
    }
    // right roll +1: the right elevator (taileron share 0.5) lifts its TE by ~10 deg; left roll lowers it
    { AnimInputs r; r.roll = 1; std::vector<GpuVertex> pr; pose_anim(me, r, pr);
      float c[3]; centroid_of(pr, el.first - me.anim_first, el.count, c); CHECK(std::fabs(c[1] - (1.0f + 0.5f * std::sin(0.1745f))) < 2e-3f);
      r.roll = -1; pose_anim(me, r, pr); centroid_of(pr, el.first - me.anim_first, el.count, c); CHECK(c[1] < 1.0f - 0.05f); }
    // stick: pitch +1 moves the grip aft (+z) and down; roll +1 moves it right (+x); rigid (pairwise distances kept)
    const AnimPiece& st = me.anim[1];
    CHECK(st.role == AnimRole::stick);
    float c0[3], c1[3]; centroid_of(me.v, st.first, st.count, c0);
    centroid_of(pose, st.first - me.anim_first, st.count, c1);
    std::printf("  stick centroid rest (%.3f,%.3f,%.3f) pitch+1 (%.3f,%.3f,%.3f)\n", c0[0], c0[1], c0[2], c1[0], c1[1], c1[2]);
    CHECK(c1[2] > c0[2] + 0.2f && c1[1] < c0[1] && std::fabs(c1[0] - c0[0]) < 1e-4f);
    { AnimInputs r; r.roll = 1; std::vector<GpuVertex> pr; pose_anim(me, r, pr); float c2[3]; centroid_of(pr, st.first - me.anim_first, st.count, c2);
      CHECK(c2[0] > c0[0] + 0.2f && std::fabs(c2[2] - c0[2]) < 1e-4f);
      float worst = 0;
      for (uint32_t i = 1; i < st.count; i += 7) {
          float d0 = 0, d1 = 0;
          for (int a = 0; a < 3; ++a) { const float u = me.v[st.first + i].pos[a] - me.v[st.first].pos[a], w = pr[st.first - me.anim_first + i].pos[a] - pr[st.first - me.anim_first].pos[a]; d0 += u * u; d1 += w * w; }
          worst = std::max(worst, std::fabs(std::sqrt(d0) - std::sqrt(d1)));
      }
      CHECK(worst < 1e-4f); }
    // throttle forward (1) moves the lever knob forward (-z) vs idle (0); pedals: nose-right (yaw +1) pushes the RIGHT pedal forward and the left one back
    { AnimInputs a0; a0.throttle = 0; AnimInputs a1; a1.throttle = 1; std::vector<GpuVertex> p0, p1; pose_anim(me, a0, p0); pose_anim(me, a1, p1);
      const AnimPiece& th = me.anim[2]; CHECK(th.role == AnimRole::throttle);
      float k0[3], k1[3]; centroid_of(p0, th.first - me.anim_first, th.count, k0); centroid_of(p1, th.first - me.anim_first, th.count, k1); CHECK(k1[2] < k0[2] - 0.2f); }
    { AnimInputs a; a.yaw = 1; std::vector<GpuVertex> p; pose_anim(me, a, p);
      const AnimPiece& pl = me.anim[3]; const AnimPiece& pr = me.anim[4]; CHECK(pl.role == AnimRole::pedal && pr.role == AnimRole::pedal && pl.pivot[0] < pr.pivot[0]);
      float l0[3], l1[3], r0[3], r1[3];
      centroid_of(me.v, pl.first, pl.count, l0); centroid_of(p, pl.first - me.anim_first, pl.count, l1);
      centroid_of(me.v, pr.first, pr.count, r0); centroid_of(p, pr.first - me.anim_first, pr.count, r1);
      CHECK(r1[2] < r0[2] - 0.1f && l1[2] > l0[2] + 0.1f); }
    // apply_pose writes exactly the range
    { Mesh y = me; apply_pose(y, pose); CHECK(std::memcmp(&y.v[me.anim_first], pose.data(), pose.size() * sizeof(GpuVertex)) == 0);
      CHECK(std::memcmp(y.v.data(), me.v.data(), me.anim_first * sizeof(GpuVertex)) == 0); }
    // ---- anim_update: a synthetic flight at 60 Hz. Constant body rates -> the commands converge to rate / full scale with the right signs.
    auto fly = [](float wx, float wy, float wz, float accel, int steps, AnimState& s) {
        float q[4] = {0, 0, 0, 1}; float pos[3] = {0, 1000, 0}; float v = 200.0f;
        const double dt = 1.0 / 60.0;
        for (int i = 0; i < steps; ++i) {
            float r[3], u[3], f[3]; quat_to_axes(q, r, u, f);
            anim_update(s, r, u, f, pos, 10.0 + i * dt);
            // body-frame rotation increment: q <- q * dq(w dt)
            const float wl = std::sqrt(wx * wx + wy * wy + wz * wz), h = 0.5f * wl * float(dt);
            const float sn = wl > 0 ? std::sin(h) / wl : 0, dq[4] = {wx * sn, wy * sn, wz * sn, std::cos(h)};
            const float a[4] = {q[0], q[1], q[2], q[3]};
            q[0] = a[3] * dq[0] + a[0] * dq[3] + a[1] * dq[2] - a[2] * dq[1];
            q[1] = a[3] * dq[1] - a[0] * dq[2] + a[1] * dq[3] + a[2] * dq[0];
            q[2] = a[3] * dq[2] + a[0] * dq[1] - a[1] * dq[0] + a[2] * dq[3];
            q[3] = a[3] * dq[3] - a[0] * dq[0] - a[1] * dq[1] - a[2] * dq[2];
            for (int k = 0; k < 3; ++k) pos[k] += f[k] * v * float(dt);
            v += accel * float(dt);
        }
    };
    const AnimTuning tune;
    { AnimState s; fly(0.3f, 0, 0, 0, 90, s); std::printf("  anim pitch-up 0.3 rad/s -> pitch %.3f (want %.3f)\n", s.in.pitch, 0.3f / tune.pitch_full);
      CHECK(std::fabs(s.in.pitch - 0.3f / tune.pitch_full) < 0.01f && std::fabs(s.in.roll) < 1e-3f && std::fabs(s.in.yaw) < 1e-3f && s.updates == 89); }
    { AnimState s; fly(0, 0, -1.0f, 0, 90, s); CHECK(std::fabs(s.in.roll - 1.0f / tune.roll_full) < 0.01f && std::fabs(s.in.pitch) < 1e-3f); }   // right roll = -w_z
    { AnimState s; fly(0, -0.13f, 0, 0, 90, s); CHECK(std::fabs(s.in.yaw - 0.13f / tune.yaw_full) < 0.01f); }                                  // nose right = -w_y
    { AnimState s; fly(0, 0, 0, 12.5f, 120, s); std::printf("  anim accel 12.5 u/s^2 -> throttle %.3f\n", s.in.throttle); CHECK(s.in.throttle > 0.7f); }
    { AnimState s; fly(0, 0, 0, -12.5f, 120, s); CHECK(s.in.throttle < 0.3f); }
    { AnimState s; fly(2.0f, 0, 0, 0, 90, s); CHECK(s.in.pitch > 0.98f && s.in.pitch <= 1.0f); }   // saturates
    // gaps / bad input restart the differentiation (no spike)
    { AnimState s; const float r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, f[3] = {0, 0, -1}, p[3] = {0, 0, 0};
      anim_update(s, r, u, f, p, 1.0); const float u2[3] = {0, 0.8f, 0.6f}, f2[3] = {0, 0.6f, -0.8f};
      anim_update(s, r, u2, f2, p, 2.0);   // 1 s gap (> max_dt): reset, no rate
      CHECK(s.in.pitch == 0.0f && s.resets == 1 && s.updates == 0);
      const float bad[3] = {std::nanf(""), 0, 0}; anim_update(s, bad, u, f, p, 2.01); CHECK(!s.have && s.resets == 2); }
}

// Minimal PNG writer (stored deflate blocks) for the --load preview; no compression library.
static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c;
}
static bool write_png(const char* path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw; raw.reserve(size_t(h) * (w * 3 + 1));
    for (uint32_t y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t o = 0; o < raw.size(); o += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n == raw.size() ? 1 : 0); z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
    }
    const uint32_t ad = (b << 16) | a; for (int i = 3; i >= 0; --i) z.push_back(uint8_t(ad >> (8 * i)));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(d.size() >> (8 * i)));
        std::vector<uint8_t> td(t, t + 4); td.insert(td.end(), d.begin(), d.end());
        out.insert(out.end(), td.begin(), td.end());
        const uint32_t c = ~crc32_of(td.data(), td.size()); for (int i = 3; i >= 0; --i) out.push_back(uint8_t(c >> (8 * i)));
    };
    std::vector<uint8_t> ihdr; for (uint32_t v : {w, h}) for (int i = 3; i >= 0; --i) ihdr.push_back(uint8_t(v >> (8 * i)));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
    FILE* f = std::fopen(path, "wb"); if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size(); std::fclose(f); return ok;
}
// --load ... --png <out> [yaw_deg pitch_deg roll_deg] [--tan l r u d] [--size W H] [--opts "<opt:val;...>"]: CPU reference render (= the shader, WARP-verified on
// the synthetic model) from the pvb eye over a grey sky. Default 100x75 deg at 800x600. COCKPIT-3 evidence: --tan 0.6152 0.6152 0.3460 0.3460 --size 2400 1350 = the
// game's own cockpit lens (compare with the game window); --tan 1 1 1 1 --size 2400 1350 = the SteamVR null-driver eye (+-45 deg, anisotropic pixels) of the PNG dumps.
static float g_tan[4] = {1.1918f, 1.1918f, 0.7673f, 0.7673f};   // tan(50 deg), tan(37.5 deg)
static uint32_t g_w = 800, g_h = 600;
static std::string g_opts;
static bool g_light_set = false; static float g_light[3] = {0, 1, 0};   // --light x y z: model-space light (e.g. the stage sun at the capture's attitude)
static bool g_tint_set = false; static float g_tint[3] = {1, 1, 1};      // --tint r g b: light colour (stage_tint)
static bool g_anim_set = false; static AnimInputs g_anim;                // --anim pitch roll yaw throttle: pose the animated range (COCKPIT-4)
static float g_eye_off[3] = {0, 0, 0};                                    // --eyeoff x y z: head offset in metres (6DoF lean preview)
static const char* g_dump = nullptr;                                      // --dump <file.tri>: the built mesh (COCKPIT-CLIP evidence, cockpit_clip_survey.py)
static void preview_png(const Mesh& me, const Options& o, const char* out, float yaw, float pitch, float roll) {
    const uint32_t W = g_w, H = g_h;
    const float fov[4] = {-std::atan(g_tan[0]), std::atan(g_tan[1]), std::atan(g_tan[2]), -std::atan(g_tan[3])}, zero[3] = {0, 0, 0};
    float q[4]; quat_ypr(yaw * kPi / 180, pitch * kPi / 180, roll * kPi / 180, q);
    EyeCam cam; if (!eye_camera(q, g_eye_off, fov, zero, 0.0f, me.eye, view_upm(o), view_near(o), cam)) { std::printf("eye_camera refused\n"); return; }
    Target t; t.w = W; t.h = H; t.rgba.assign(size_t(W) * H * 4, 0.0f); t.depth.assign(size_t(W) * H, 0.0f);
    for (size_t i = 0; i < size_t(W) * H; ++i) { t.rgba[i * 4] = t.rgba[i * 4 + 1] = 0.6f; t.rgba[i * 4 + 2] = 0.75f; t.rgba[i * 4 + 3] = 1.0f; }   // grey sky UNDER the model (the canopy multiplies it)
    render_reference(me, o, 1.0f, 0, cam, t, g_light_set ? g_light : nullptr, g_tint_set ? g_tint : nullptr);
    std::vector<uint8_t> rgb(size_t(W) * H * 3);
    for (size_t i = 0; i < size_t(W) * H; ++i) for (int c = 0; c < 3; ++c) {
        const float v = t.rgba[i * 4 + c];
        const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;   // the sRGB RTV encode
        rgb[i * 3 + c] = uint8_t(std::lround(std::min(1.0f, std::max(0.0f, s)) * 255.0f));
    }
    std::printf("preview %s: %s\n", out, write_png(out, rgb, W, H) ? "written" : "FAILED");
}

static int load_main(const char* path) {    ModelData m;
    const LoadStatus st = load_model_file(path, m);
    std::printf("load %s: %s\n", path, status_name(st));
    if (st != LoadStatus::ok) return 1;
    std::printf("parts=%zu textures=%zu vertices=%zu triangles=%zu eye=%s(%.3f,%.3f,%.3f) bbox=(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) sha=%s\n", m.parts.size(),
        m.textures.size(), m.vertices.size(), m.indices.size() / 3, m.has_eye ? "pvb" : "none", m.eye[0], m.eye[1], m.eye[2], m.bbox_min[0], m.bbox_min[1],
        m.bbox_min[2], m.bbox_max[0], m.bbox_max[1], m.bbox_max[2], acz::terrain::hex(m.payload_sha, 32).c_str());
    for (size_t i = 0; i < m.textures.size(); ++i) std::printf("  texture %zu: %ux%u\n", i, m.textures[i].w, m.textures[i].h);
    Options o; Mesh me; std::string why;
    if (m.legacy_clut_fixed) std::printf("clut: legacy BGRA bake, R/B swapped back at load (rebake with model_baker.py for flag 4)\n");
    { std::string id; if (!parse_options("x" + (g_opts.empty() ? std::string() : ";" + g_opts), id, o, why)) { std::printf("bad --opts: %s\n", why.c_str()); return 1; } }
    {   // COCKPIT-CLIP: the type's clip volume from the table, by the file stem (as the runtime does by the model id)
        std::string stem = path; const size_t sl = stem.find_last_of("/\\"); if (sl != std::string::npos) stem = stem.substr(sl + 1);
        const size_t dot = stem.rfind('.'); if (dot != std::string::npos) stem = stem.substr(0, dot);
        apply_clip_table(o, stem);
    }
    if (!build_mesh(m, o, me, why)) { std::printf("build_mesh refused: %s\n", why.c_str()); return 1; }
    if (me.floor) std::printf("floor closure: y=%.3f x=%.3f..%.3f z=%.3f..%.3f\n", me.floor_y, me.floor_x[0], me.floor_x[1], me.floor_z[0], me.floor_z[1]);
    std::printf("mesh: drawn parts=%u hidden=%u triangles=%u opaque-vertices=%u blend-vertices=%u gpu-textures=%zu bytes=%zu\n", me.parts_drawn, me.parts_hidden,
        me.triangles, me.opaque_vertices, me.blend_vertices, me.textures.size(), me.v.size() * sizeof(GpuVertex));
    if (me.hud.valid)
        std::printf("hud glass: part %u '%s' centre=(%.3f,%.3f,%.3f) distance=%.3f u (%.3f m at 5 u/m) tan l/r/u/d=%.3f/%.3f/%.3f/%.3f (deg %.1f/%.1f/%.1f/%.1f)\n",
            me.hud.part, m.part_name(me.hud.part).c_str(), me.hud.centre[0], me.hud.centre[1], me.hud.centre[2], me.hud.distance, me.hud.distance / 5.0f,
            me.hud.tan_l, me.hud.tan_r, me.hud.tan_u, me.hud.tan_d, std::atan(me.hud.tan_l) * 57.2958f, std::atan(me.hud.tan_r) * 57.2958f,
            std::atan(me.hud.tan_u) * 57.2958f, std::atan(me.hud.tan_d) * 57.2958f);
    else std::printf("hud glass: none found\n");
    // COCKPIT-4
    std::printf("canopy: parts=%u (%s) glass-vertices=%u; interior=%s triangles=%u floor-y=%.3f; anim: surfaces=%u not-driven=%u pieces=%zu vertices=%u\n",
        me.canopy_parts, me.canopy_exterior ? "exterior-flipped" : "cockpit-part", me.glass_vertices, me.interior ? "on" : "off", me.interior_triangles,
        me.interior_floor_y, me.anim_surfaces, me.anim_unlocated, me.anim.size(), me.anim_vertices);
    std::printf("cfid: %s srefl=%s shadow-box=%s sill=%.3f half-x=%.3f z=%.3f..%.3f\n", o.fidelity ? "on" : "off", o.skin_refl ? "on" : "off",
        me.shadow ? "yes" : "none", me.shadow_box[0], me.shadow_box[1], me.shadow_box[2], me.shadow_box[3]);
    for (const AnimPiece& a : me.anim)
        std::printf("  anim piece role=%u part=%d '%s' corners=%u pivot=(%.2f,%.2f,%.2f) axis=(%.2f,%.2f,%.2f) gain=%.1f deg cp=%.2f cr=%.2f cy=%.2f ct=%.2f\n",
            unsigned(a.role), a.part, a.part >= 0 ? m.part_name(size_t(a.part)).c_str() : "interior", a.count, a.pivot[0], a.pivot[1], a.pivot[2],
            a.ax[0].axis[0], a.ax[0].axis[1], a.ax[0].axis[2], a.ax[0].gain * 57.29578f, a.ax[0].cp, a.ax[0].cr, a.ax[0].cy, a.ax[0].ct);
    for (size_t i = 0; i < m.parts.size(); ++i)
        if (m.parts[i].anim >= 0 && m.parts[i].index_count)
            std::printf("  model anim part %zu '%s' group=0x%x chan=%d stored=%.1f rest=%.1f fitted=%u\n", i, m.part_name(i).c_str(), m.parts[i].group, m.parts[i].anim,
                m.parts[i].hinge * 57.29578f, m.parts[i].rest * 57.29578f, m.parts[i].aflags & kPartHingeFitted);
    if (g_anim_set && me.anim_vertices) { std::vector<GpuVertex> p; pose_anim(me, g_anim, p); apply_pose(me, p);
        std::printf("posed: pitch=%.2f roll=%.2f yaw=%.2f throttle=%.2f\n", g_anim.pitch, g_anim.roll, g_anim.yaw, g_anim.throttle); }
    std::printf("clip: %s box-m=%.2f,%.2f,%.2f,%.2f,%.2f game-removed=%u game-cut=%u interior-removed=%u interior-cut=%u fit-moved=%u fit-skin=%u tri-src=%zu/%u\n",
        o.clip ? (me.clip_box ? "on(fit+box)" : "on(fit;no-box)") : "off", o.clip_box[0], o.clip_box[1], o.clip_box[2], o.clip_box[3], o.clip_box[4], me.clip_removed,
        me.clip_cut, me.clip_interior_removed, me.clip_interior_cut, me.fit_moved, me.fit_skin, me.tri_src.size(), me.triangles);
    if (g_dump) {   // u32 count, then per triangle 9 f32 (model units) + u8 source (0 exterior, 1 game cockpit, 2 synthetic)
        FILE* f = std::fopen(g_dump, "wb"); if (!f) { std::printf("dump: cannot open %s\n", g_dump); return 1; }
        const uint32_t n = uint32_t(me.v.size() / 3); std::fwrite(&n, 4, 1, f);
        for (uint32_t t = 0; t < n; ++t) {
            for (int k = 0; k < 3; ++k) std::fwrite(me.v[t * 3 + k].pos, 4, 3, f);
            const uint8_t src = t < me.tri_src.size() ? me.tri_src[t] : 255; std::fwrite(&src, 1, 1, f);
        }
        std::fclose(f); std::printf("dump %s: %u triangles\n", g_dump, n);
    }
    if (g_png) preview_png(me, o, g_png, g_ypr[0], g_ypr[1], g_ypr[2]);
    return 0;
}

static void test_config_key() {
    using acz::diagnostics::parse_config; using acz::diagnostics::ConfigIssue;
    auto r = parse_config("xr_cockpit_model=j35jk_vrcockpit\n");
    CHECK(r.issue == ConfigIssue::none && std::string(r.config.xr_cockpit_model()) == "j35jk_vrcockpit");
    r = parse_config("xr_cockpit_model=j35jk_vrcockpit;upm:5.5;hide:12,13;eye:0,3.575,-36.3;replay:1\n");
    CHECK(r.issue == ConfigIssue::none && std::string(r.config.xr_cockpit_model()) == "j35jk_vrcockpit;upm:5.5;hide:12,13;eye:0,3.575,-36.3;replay:1");
    { std::string id, why; Options o; CHECK(parse_options(r.config.xr_cockpit_model(), id, o, why) && o.keep_replay && o.upm == 5.5f && o.hide.size() == 2 && o.eye_override); }
    for (const char* off : {"xr_cockpit_model=off\n", "xr_cockpit_model=0\n", "xr_cockpit_model=false\n"}) CHECK(parse_config(off).issue == ConfigIssue::none && !parse_config(off).config.xr_cockpit_model()[0]);
    CHECK(!parse_config("").config.xr_cockpit_model()[0]);   // default OFF
    for (const char* bad : {"xr_cockpit_model=\n", "xr_cockpit_model=a/../b\n", "xr_cockpit_model=j35 jk\n", "xr_cockpit_model=a\"b\n", "xr_cockpit_model=a\\b\n"})
        CHECK(parse_config(bad).issue != ConfigIssue::none);
    CHECK(parse_config(std::string("xr_cockpit_model=") + std::string(192, 'a') + "\n").issue != ConfigIssue::none);   // >= 192 chars (the latch buffer; was 100 before step170)
    CHECK(parse_config(std::string("xr_cockpit_model=") + std::string(191, 'a') + "\n").issue == ConfigIssue::none);
    CHECK(parse_config("xr_cockpit_model=a\nxr_cockpit_model=b\n").issue == ConfigIssue::duplicate);
}

// ---------------------------------------------------------------- COCKPIT-CLIP (cockpit-render-impl.md section 16)
// A closed exterior "fuselage" shell (outward faces) x -hw..hw, y y0..y1, z z0..z1 appended to a model (the synthetic cockpit is 8 u wide: x -4..4).
static void add_fuselage(ModelData& m, float hw, float y0, float y1, float z0, float z1) {
    Part p; p.name_off = uint32_t(m.strings.size()); m.strings += "fuselage"; m.strings.push_back('\0');
    p.node = uint32_t(m.parts.size()); p.mflags = MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE; p.texture = -1;
    for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
    p.first_vertex = uint32_t(m.vertices.size()); p.first_index = uint32_t(m.indices.size());
    const float x0 = -hw, x1 = hw;
    const float f[6][4][3] = {
        {{x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}}, {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},
        {{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}}, {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},
        {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}, {{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}}};
    const int16_t n[6][3] = {{32767, 0, 0}, {-32767, 0, 0}, {0, 32767, 0}, {0, -32767, 0}, {0, 0, 32767}, {0, 0, -32767}};
    for (int k = 0; k < 6; ++k) {
        const uint32_t b = uint32_t(m.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex v{}; std::memcpy(v.pos, f[k][i], 12); std::memcpy(v.nrm, n[k], 6); v.part = uint16_t(m.parts.size());
            v.rgba[0] = v.rgba[1] = v.rgba[2] = v.rgba[3] = 255; m.vertices.push_back(v);
        }
        for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) m.indices.push_back(ix);
    }
    p.vertex_count = 24; p.index_count = 36;
    m.parts.push_back(p);
}
static bool ccw_on_normal_side(const GpuVertex* t) {   // the geometric normal agrees with each corner's stored normal
    const float* a = t[0].pos; const float* b = t[1].pos; const float* c = t[2].pos;
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float g[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    for (int k = 0; k < 3; ++k) if (g[0] * t[k].nrm[0] + g[1] * t[k].nrm[1] + g[2] * t[k].nrm[2] <= 0.0f) return false;
    return true;
}
static void test_clip() {
    std::string id, why;
    // options: default on, clip:0, clipbox: (5 values 0.05..5 m), refusals; the table
    { Options o; CHECK(parse_options("m", id, o, why) && o.clip && !o.clip_box_set); }
    { Options o; CHECK(parse_options("m;clip:0", id, o, why) && !o.clip); }
    { Options o; CHECK(parse_options("m;clipbox:1,0.8,1.2,0.3,0.45", id, o, why) && o.clip_box_set && o.clip_box[0] == 1.0f && o.clip_box[1] == 0.8f && o.clip_box[4] == 0.45f); }
    for (const char* b : {"m;clip:2", "m;clipbox:1,1,1,1", "m;clipbox:1,1,1,1,1,1", "m;clipbox:1,1,1,1,0", "m;clipbox:1,1,1,1,6", "m;clipbox:a,1,1,1,1", "m;clipbox:"}) {
        Options o; std::string w; CHECK(!parse_options(b, id, o, w)); }
    CHECK(kClipVolumeCount == 36);
    for (const ClipVolume& c : kClipVolumes) {
        CHECK(valid_model_id(c.model_id) && clip_volume_for(c.model_id) == &c);
        for (float v : {c.ahead, c.behind, c.below, c.above, c.half}) CHECK(v >= 0.05f && v <= 5.0f);
        CHECK(c.below >= 1.10f && c.half >= 0.30f && c.above >= 0.15f);   // the procedural floor (1.05 m down) and the seat (rails +-0.27 m) fit inside
    }
    { const ClipVolume* j = clip_volume_for("j35jk_vrcockpit"); CHECK(j && j->ahead == 1.12f && j->behind == 0.84f && j->below == 1.12f && j->above == 0.18f && j->half == 0.42f); }
    CHECK(clip_volume_for("nope_vrcockpit") == nullptr);
    { Options o; CHECK(apply_clip_table(o, "j35jk_vrcockpit") && o.clip_box_set && o.clip_box[3] == 0.18f); }
    { Options o; o.clip = false; CHECK(!apply_clip_table(o, "j35jk_vrcockpit") && !o.clip_box_set); }
    { Options o; CHECK(!apply_clip_table(o, "x") && !o.clip_box_set); }
    { Options o; CHECK(parse_options("m;clipbox:2,2,2,2,2", id, o, why) && apply_clip_table(o, "j35jk_vrcockpit") && o.clip_box[0] == 2.0f); }   // explicit wins

    // no airframe around the cockpit and no box: clip:1 == clip:0 byte for byte (nothing tessellated or moved)
    { const ModelData m0 = make_synthetic_model(); Options a; Options b; b.clip = false; Mesh ma, mb; CHECK(build_mesh(m0, a, ma, why) && build_mesh(m0, b, mb, why));
      CHECK(ma.v.size() == mb.v.size() && std::memcmp(ma.v.data(), mb.v.data(), ma.v.size() * sizeof(GpuVertex)) == 0 && ma.fit_skin == 0 && ma.fit_moved == 0);
      CHECK(ma.tri_src.size() == ma.triangles && size_t(ma.triangles) * 3 == ma.v.size()); }

    const ModelData m = make_synthetic_model_c4();
    // (b) the box: exact clip of the game cockpit triangles; exterior, glass and animated pieces untouched; HUD glass unchanged
    {   // synthetic eye (0, 3.5, -36): ahead 1.9 m (9.5 u: z >= -45.5) removes the panels at z -46 / -50 and keeps the near quads (z -44 / -45)
        Options o; CHECK(parse_options("m;anim:1;clipbox:1.9,1,1.2,1,1", id, o, why));
        Options n; n.anim = true; Mesh me, ref; CHECK(build_mesh(m, o, me, why) && build_mesh(m, n, ref, why));
        std::printf("  clip box: game removed=%u cut=%u interior removed=%u cut=%u tris %u -> %u\n", me.clip_removed, me.clip_cut, me.clip_interior_removed,
            me.clip_interior_cut, ref.triangles, me.triangles);
        CHECK(me.clip_box && me.clip_removed == 8 && std::fabs(me.clip_lo[2] - (-45.5f)) < 1e-4f && std::fabs(me.clip_hi[1] - 8.5f) < 1e-4f);
        CHECK(me.hud.valid && me.hud.part == ref.hud.part && me.hud.distance == ref.hud.distance && me.hud.tan_l == ref.hud.tan_l && me.hud.tan_u == ref.hud.tan_u);
        CHECK(me.tri_src.size() == me.triangles && size_t(me.triangles) * 3 == me.v.size());
        size_t outside = 0, ext = 0, kept = 0;
        for (size_t t = 0; t < me.tri_src.size(); ++t) {
            const bool fixed = t * 3 >= me.anim_first;   // animated pieces, blend, glass: never clipped
            if (me.tri_src[t] == 0) { ++ext; continue; }
            if (fixed) { ++kept; continue; }
            for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) {
                const float v = me.v[t * 3 + k].pos[a];
                if (v < me.clip_lo[a] - 1e-4f || v > me.clip_hi[a] + 1e-4f) ++outside;
            }
        }
        CHECK(outside == 0 && ext > 0 && kept > 0);
        size_t ext_ref = 0; for (uint8_t s2 : ref.tri_src) ext_ref += s2 == 0; CHECK(ext == ext_ref);   // the exterior (tail, elevator) is all there
        size_t bad = 0; for (size_t i = 0; i + 2 < me.anim_first; i += 3) if (!ccw_on_normal_side(&me.v[i])) ++bad;
        CHECK(bad == 0);   // a cut keeps the polygon's order: still CCW-front on the normal side
        // a cut corner's attributes are interpolated: the checker quad (x 2..4 at z -45, u 0..1) cut at half = 0.5 m (2.5 u) keeps u in [0, 0.25]
        Options h; CHECK(parse_options("m;clipbox:3,1,2,1,0.5", id, h, why)); Mesh mh; CHECK(build_mesh(m, h, mh, why) && mh.clip_cut > 0);
        bool seen = false;
        for (size_t k = 0; k < mh.anim_first; ++k)
            if ((mh.v[k].mat & 7) == 0 && mh.v[k].pos[2] == -45.0f) { seen = true; CHECK(mh.v[k].pos[0] <= 2.5f + 1e-4f && mh.v[k].uv[0] <= 0.25f + 1e-4f); }
        CHECK(seen);
        // clip:0 ignores clipbox: (the off switch)
        Options off; CHECK(parse_options("m;anim:1;clip:0;clipbox:1.9,1,1.2,1,1", id, off, why)); Options n0 = n; n0.clip = false; Mesh mo, r0;
        CHECK(build_mesh(m, off, mo, why) && build_mesh(m, n0, r0, why) && !mo.clip_box && mo.v.size() == r0.v.size() &&
              std::memcmp(mo.v.data(), r0.v.data(), mo.v.size() * sizeof(GpuVertex)) == 0); }

    // (a) the fit: a fuselage 6 u wide (x +-3) around the 8 u wide synthetic cockpit -> before, the tub walls/floor poke 1 u out of both sides; after, every
    // synthetic static vertex is inside the shell by the margin, normals re-derived, the game cockpit, the exterior and the HUD glass untouched.
    {
        ModelData f = m; add_fuselage(f, 3.0f, -3.0f, 6.0f, -52.0f, -30.0f);
        Options on; Options off; off.clip = false; Mesh a, b; CHECK(build_mesh(f, on, a, why) && build_mesh(f, off, b, why));
        const float mg = kClipMarginM * kModelUnitsPerMetre;
        auto synth_outside = [&](const Mesh& me, float tol) {
            size_t n = 0;
            for (size_t t = 0; t < me.tri_src.size(); ++t) {
                if (me.tri_src[t] != 2 || t * 3 >= me.anim_first) continue;
                for (int k = 0; k < 3; ++k) { const float* p = me.v[t * 3 + k].pos; if (std::fabs(p[0]) > 3.0f - tol || p[1] < -3.0f + tol || p[1] > 6.0f - tol) ++n; }
            }
            return n;
        };
        std::printf("  clip fit: skin=%u moved=%u synthetic corners outside: before %zu after %zu (interior tris %u -> %u)\n", a.fit_skin, a.fit_moved,
            synth_outside(b, 0.0f), synth_outside(a, 0.0f), b.interior_triangles, a.interior_triangles);
        // skin = the 8 side/top/bottom triangles + the elevator (the end caps z -52/-30 lie outside the cockpit's z slice)
        CHECK(a.fit_skin == 10 && a.fit_moved > 0 && synth_outside(b, 0.0f) > 0 && synth_outside(a, mg - 1e-3f) == 0);
        {   // STORES: an attached store (source part 7: the F-5E belly tank sits in the cockpit's z slice) is not skin: the fit is unchanged by it
            ModelData g = f; const size_t vb = g.vertices.size();
            for (int i = 0; i < 4; ++i) { Vertex v{}; v.pos[0] = (i == 1 || i == 2) ? 2.0f : -2.0f; v.pos[1] = -2.9f; v.pos[2] = (i >= 2) ? -45.0f : -48.0f; v.nrm[1] = 32767; v.part = uint16_t(g.parts.size()); g.vertices.push_back(v); }
            Part tp; tp.name_off = uint32_t(g.strings.size()); g.strings += "t/tank79/n0"; g.strings.push_back('\0'); tp.node = uint32_t(g.parts.size()); tp.mflags = MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE;
            tp.texture = 1; tp.source_part = kStoresSourcePart; for (int i = 0; i < 4; ++i) tp.local_to_model[i * 5] = 1.0f;
            tp.first_vertex = uint32_t(vb); tp.vertex_count = 4; tp.first_index = uint32_t(g.indices.size()); tp.index_count = 6;
            for (uint32_t ix : {uint32_t(vb), uint32_t(vb) + 1, uint32_t(vb) + 2, uint32_t(vb), uint32_t(vb) + 2, uint32_t(vb) + 3}) g.indices.push_back(ix);
            g.parts.push_back(tp);
            Mesh c; CHECK(build_mesh(g, on, c, why) && c.fit_skin == a.fit_skin && c.fit_moved == a.fit_moved);
            g.parts.back().source_part = 0; Mesh d; CHECK(build_mesh(g, on, d, why) && d.fit_skin == a.fit_skin + 2);   // the same quad as airframe WOULD be skin (2 triangles)
        }
        CHECK(a.interior && a.interior_triangles > b.interior_triangles);   // tessellated tub faces
        size_t bad = 0;
        for (size_t t = 0; t < a.tri_src.size(); ++t) if (a.tri_src[t] == 2 && t * 3 < a.anim_first && !ccw_on_normal_side(&a.v[t * 3])) ++bad;
        CHECK(bad == 0);
        std::vector<GpuVertex> ga, gb;   // the game cockpit part + exterior: identical bytes in the same order (no box here)
        for (size_t t = 0; t < a.tri_src.size(); ++t) if (a.tri_src[t] != 2) ga.insert(ga.end(), a.v.begin() + std::ptrdiff_t(t * 3), a.v.begin() + std::ptrdiff_t(t * 3 + 3));
        for (size_t t = 0; t < b.tri_src.size(); ++t) if (b.tri_src[t] != 2) gb.insert(gb.end(), b.v.begin() + std::ptrdiff_t(t * 3), b.v.begin() + std::ptrdiff_t(t * 3 + 3));
        CHECK(ga.size() == gb.size() && std::memcmp(ga.data(), gb.data(), ga.size() * sizeof(GpuVertex)) == 0);
        CHECK(a.hud.valid && a.hud.distance == b.hud.distance && a.anim_vertices == b.anim_vertices);
        // the COCKPIT-3 floor closure (interior:0) is fitted the same way
        Options fl; fl.interior = false; Mesh c; CHECK(build_mesh(f, fl, c, why) && c.floor && synth_outside(c, mg - 1e-3f) == 0 && c.fit_moved > 0);
        // a single sheet (the c4 elevator plate) is not an airframe: nothing is moved by it
        Mesh d; Options dd; CHECK(build_mesh(m, dd, d, why) && d.fit_moved == 0);
        // cscale with the clip (section 15): the mesh does not depend on cscale (byte-identical at 1.15); at the centred eye every vertex keeps its NDC x/y and
        // its reversed-Z depth is divided by s: the clipped/fitted cockpit stays angularly registered.
        Options s1 = on; s1.cscale = 1.15f; Mesh e; CHECK(build_mesh(f, s1, e, why) && e.v.size() == a.v.size() && std::memcmp(e.v.data(), a.v.data(), a.v.size() * sizeof(GpuVertex)) == 0);
        const float anchor[3] = {0, 1.2f, 0}, fov[4] = {-0.9f, 0.9f, 0.8f, -0.8f}; float q0[4]; quat_ypr(0.2f, -0.3f, 0.1f, q0);
        EyeCam c1, cs; CHECK(eye_camera(q0, anchor, fov, anchor, 0.0f, a.eye, on.upm, on.near_u, c1) && eye_camera(q0, anchor, fov, anchor, 0.0f, a.eye, view_upm(s1), view_near(s1), cs));
        size_t checked = 0;
        for (const GpuVertex& g : a.v) {
            double v1[4], v2[4];
            for (int r = 0; r < 4; ++r) {
                v1[r] = double(c1.m[4 * r]) * g.pos[0] + double(c1.m[4 * r + 1]) * g.pos[1] + double(c1.m[4 * r + 2]) * g.pos[2] + c1.m[4 * r + 3];
                v2[r] = double(cs.m[4 * r]) * g.pos[0] + double(cs.m[4 * r + 1]) * g.pos[1] + double(cs.m[4 * r + 2]) * g.pos[2] + cs.m[4 * r + 3];
            }
            if (std::fabs(v1[3]) < 0.5) continue;
            ++checked;
            CHECK(std::fabs(v1[0] / v1[3] - v2[0] / v2[3]) < 1e-5 && std::fabs(v1[1] / v1[3] - v2[1] / v2[3]) < 1e-5);
            CHECK(std::fabs((v2[2] / v2[3]) * 1.15 - v1[2] / v1[3]) < 1e-4 * std::fabs(v1[2] / v1[3]) + 1e-9);
        }
        CHECK(checked > 100);
    }
}

// ---------------------------------------------------------------- SKIN-REFL: the engine's cb10-bit-0 reflection pass on the exterior skin
static bool near3(const float a[3], float x, float y, float z, float e = 1e-5f) { return std::fabs(a[0] - x) < e && std::fabs(a[1] - y) < e && std::fabs(a[2] - z) < e; }
static void test_skin_refl() {
    std::string id, why; Options o;
    // options: default = cfid (on since CFID), strength 1; srefl/sreflk parse; bad values refused
    CHECK(parse_options("auto", id, o, why) && o.skin_refl && o.fidelity && o.skin_refl_k == 1.0f);
    CHECK(parse_options("auto;cfid:0", id, o, why) && !o.skin_refl && !o.fidelity);
    CHECK(parse_options("auto;anim:1;srefl:1;sreflk:0.5", id, o, why) && o.skin_refl && o.skin_refl_k == 0.5f && o.anim);
    CHECK(parse_options("m;srefl=0;sreflk=4", id, o, why) && !o.skin_refl && o.skin_refl_k == 4.0f);
    for (const char* b : {"m;srefl:2", "m;srefl:", "m;sreflk:-0.1", "m;sreflk:4.5", "m;sreflk:nan", "m;sreflk:x"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    CHECK(sizeof(Constants) == kConstantDwords * 4 && kConstantDwords == 60 && offsetof(Constants, rx) == 44 * 4 && offsetof(Constants, rz) == 52 * 4);
    // basis: identity camera (right +X, up +Y, fwd -Z: model +Z = stage +Z) with the default convention = identity
    const float r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0}, f0[3] = {0, 0, -1};
    { ReflBasis b; CHECK(refl_basis(r0, u0, f0, 1.0f, 0.0f, b) && near3(b.x, 1, 0, 0) && near3(b.y, 0, 1, 0) && near3(b.z, 0, 0, 1)); }
    // heading 90 deg left (fwd = stage -X, right = stage -Z): the game's +X is BEHIND the pilot (model +Z), the game's +Z on his right (model +X)
    { const float r[3] = {0, 0, -1}, f[3] = {-1, 0, 0}; ReflBasis b; CHECK(refl_basis(r, u0, f, 1.0f, 0.0f, b) && near3(b.x, 0, 0, 1) && near3(b.z, -1, 0, 0) && near3(b.y, 0, 1, 0)); }
    // 90 deg right roll (up = stage +X... right = stage -Y): world up = model -X (left)
    { const float r[3] = {0, -1, 0}, u[3] = {1, 0, 0}; ReflBasis b; CHECK(refl_basis(r, u, f0, 1.0f, 0.0f, b) && near3(b.y, -1, 0, 0)); }
    // terrain convention: yaw offset 90 deg turns the game's +X onto the stage -Z (heading 0); yaw sign -1 mirrors it onto stage -X, +Z stays
    { ReflBasis b; CHECK(refl_basis(r0, u0, f0, 1.0f, 90.0f, b) && near3(b.x, 0, 0, -1) && near3(b.z, 1, 0, 0)); }
    { ReflBasis b; CHECK(refl_basis(r0, u0, f0, -1.0f, 0.0f, b) && near3(b.x, -1, 0, 0) && near3(b.z, 0, 0, 1)); }
    { const float bad[3] = {std::nanf(""), 0, 0}; ReflBasis b; b.x[0] = 7; CHECK(!refl_basis(bad, u0, f0, 1.0f, 0.0f, b) && b.x[0] == 7); }
    // constants: the flag needs the option AND a finite basis; strength clamped
    EyeCam cam{}; for (int i = 0; i < 16; ++i) cam.m[i] = 0;
    const ReflBasis id3{};
    { Options x; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, &id3); CHECK(!(k.flags & kCFlagRefl)); }
    { Options x; x.skin_refl = true; Constants k = make_constants(cam, x, 1.0f, 0); CHECK(!(k.flags & kCFlagRefl)); }
    { Options x; x.skin_refl = true; x.skin_refl_k = 2.0f; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, &id3);
      CHECK((k.flags & kCFlagRefl) && (k.flags & kCFlagDisplay) && k.refl_k == 2.0f && k.ry[1] == 1.0f); }
    { Options x; x.skin_refl = true; ReflBasis nb; nb.z[2] = std::nanf(""); Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, &nb); CHECK(!(k.flags & kCFlagRefl)); }
    // per-vertex term (DrawCPT_v): u = n.x/2 + 1/2, v = n.z/2 + 1/2, mask = saturate(10 n.y), no normalisation
    { Options x; x.skin_refl = true; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, &id3); float r[3];
      const float up[3] = {0, 1, 0}, ex[3] = {1, 0, 0}, dn[3] = {0, -1, 0}, sl[3] = {-0.6f, 0.05f, 0.8f};
      refl_vertex(k, up, r); CHECK(near3(r, 0.5f, 0.5f, 1.0f));
      refl_vertex(k, ex, r); CHECK(near3(r, 1.0f, 0.5f, 0.0f));
      refl_vertex(k, dn, r); CHECK(near3(r, 0.5f, 0.5f, 0.0f));
      refl_vertex(k, sl, r); CHECK(near3(r, 0.2f, 0.9f, 0.5f)); }
    // fragment: display += k * skin_alpha * map * mask on kMatTexAlphaNotCov texels of the opaque pass only
    const ModelData m = make_synthetic_model();
    Options o2; o2.display = false; o2.floor = false; o2.interior = false; o2.skin_refl = true; o2.skin_refl_k = 1.5f; o2.ambient = 1.0f;
    Mesh me; CHECK(build_mesh(m, o2, me, why));
    Texture map; map.w = 4; map.h = 4; map.rgba.resize(64);
    for (size_t i = 0; i < 16; ++i) { map.rgba[i * 4] = 40; map.rgba[i * 4 + 1] = 80; map.rgba[i * 4 + 2] = 160; map.rgba[i * 4 + 3] = 128; }
    { Constants k = make_constants(cam, o2, 1.0f, 0, nullptr, nullptr, nullptr, &id3);
      const float uv[2] = {0.5f, 0.5f}, vc[4] = {1, 1, 1, 1}, n[3] = {0, 1, 0}, rr[3] = {0.5f, 0.5f, 0.8f};
      float a[4], b[4], c[4], d[4];
      shade_fragment(me, k, uint16_t(1 | kMatTexAlphaNotCov), uv, vc, n, a, &map, rr);
      shade_fragment(me, k, uint16_t(1 | kMatTexAlphaNotCov), uv, vc, n, b);           // no map: COCKPIT-3 colour
      shade_fragment(me, k, uint16_t(1), uv, vc, n, c, &map, rr);                         // not skin: no term
      Constants kb = k; kb.pass = 1; shade_fragment(me, kb, uint16_t(1 | kMatTexAlphaNotCov), uv, vc, n, d, &map, rr);   // blend pass: no term
      const float sa = 77.0f / 255.0f;   // synthetic skin alpha
      CHECK(std::fabs(b[0] - 150.0f / 255.0f) < 1e-5f && std::fabs(b[2] - 170.0f / 255.0f) < 1e-5f);
      CHECK(std::fabs(a[0] - (b[0] + 1.5f * sa * 0.8f * 40.0f / 255.0f)) < 1e-5f && std::fabs(a[1] - (b[1] + 1.5f * sa * 0.8f * 80.0f / 255.0f)) < 1e-5f);
      CHECK(std::fabs(a[2] - std::min(1.0f, b[2] + 1.5f * sa * 0.8f * 160.0f / 255.0f)) < 1e-5f && a[3] == 1.0f);
      CHECK(c[0] == b[0] && c[2] == b[2]);
      float e[4]; Constants kn = k; kn.flags &= ~kCFlagRefl; shade_fragment(me, kn, uint16_t(1 | kMatTexAlphaNotCov), uv, vc, n, e, &map, rr); CHECK(e[0] == b[0]);
      (void)d; }
    // reference raster, looking back at the exterior tail (its visible face has the normal model -Z): world up = model -Z (aircraft pitched 90 deg nose-up)
    // -> mask 1, uv centre
    { const float anchor[3] = {0, 1.2f, 0}, fov[4] = {-0.95f, 0.95f, 0.75f, -0.75f}; float qb[4]; quat_ypr(kPi, 0, 0, qb);
      EyeCam c2; CHECK(eye_camera(qb, anchor, fov, anchor, 0.0f, me.eye, 5.0f, 0.05f, c2));
      ReflBasis pitched; pitched.x[0] = 1; pitched.x[1] = 0; pitched.x[2] = 0; pitched.y[0] = 0; pitched.y[1] = 0; pitched.y[2] = -1; pitched.z[0] = 0; pitched.z[1] = 1; pitched.z[2] = 0;
      auto shot = [&](const Options& oo, const ReflBasis* bs, const Texture* mp) {
          Target t; t.w = 160; t.h = 90; t.rgba.assign(size_t(t.w) * t.h * 4, 0.0f); t.depth.assign(size_t(t.w) * t.h, 0.0f);
          render_reference(me, oo, 1.0f, 0, c2, t, nullptr, nullptr, nullptr, bs, mp); return t; };
      const Target base = shot(o2, nullptr, nullptr), with = shot(o2, &pitched, &map), nomap = shot(o2, &pitched, nullptr), level = shot(o2, &id3, &map);
      Options off = o2; off.skin_refl = false; const Target dis = shot(off, &pitched, &map);
      const size_t c = (size_t(45) * 160 + 80) * 4;
      CHECK(base.rgba[c + 3] == 1.0f);   // the tail fills the centre
      const float sa = 77.0f / 255.0f;
      std::printf("  skin-refl tail centre: base %.4f %.4f %.4f with %.4f %.4f %.4f\n", base.rgba[c], base.rgba[c + 1], base.rgba[c + 2], with.rgba[c], with.rgba[c + 1], with.rgba[c + 2]);
      CHECK(std::fabs(with.rgba[c] - (base.rgba[c] + 1.5f * sa * 40.0f / 255.0f)) < 1e-4f && std::fabs(with.rgba[c + 1] - (base.rgba[c + 1] + 1.5f * sa * 80.0f / 255.0f)) < 1e-4f);
      CHECK(nomap.rgba == base.rgba && dis.rgba == base.rgba);   // no map / option off: COCKPIT-3 image exactly
      CHECK(level.rgba == base.rgba);                          // level flight: the vertical tail faces the horizon (n.y = 0) -> mask 0
      // forward view: no skin part in sight -> unchanged
      float q0[4]; quat_ypr(0, 0, 0, q0); EyeCam cf; CHECK(eye_camera(q0, anchor, fov, anchor, 0.0f, me.eye, 5.0f, 0.05f, cf));
      Target t1; t1.w = 160; t1.h = 90; t1.rgba.assign(size_t(t1.w) * t1.h * 4, 0.0f); t1.depth.assign(size_t(t1.w) * t1.h, 0.0f); Target t2 = t1;
      render_reference(me, o2, 1.0f, 0, cf, t1); render_reference(me, o2, 1.0f, 0, cf, t2, nullptr, nullptr, nullptr, &pitched, &map);
      CHECK(t1.rgba == t2.rgba); }
}

// ---------------------------------------------------------------- CFID: cockpit fidelity bundle (docs/world-render/cockpit-fidelity.md)
static void cfid_add_quad(ModelData& m, const float q[4][3], const float n[3], uint32_t mflags) {
    Part p;
    p.name_off = uint32_t(m.strings.size()); m.strings += "q"; m.strings.push_back('\0');
    p.node = uint32_t(m.parts.size()); p.mflags = mflags; p.texture = -1;
    for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
    p.first_vertex = uint32_t(m.vertices.size()); p.vertex_count = 4; p.first_index = uint32_t(m.indices.size()); p.index_count = 6;
    for (int i = 0; i < 4; ++i) {
        Vertex v{};
        for (int a = 0; a < 3; ++a) { v.pos[a] = q[i][a]; v.nrm[a] = int16_t(std::lround(n[a] * 32767.0f)); }
        v.part = uint16_t(m.parts.size()); v.rgba[0] = v.rgba[1] = v.rgba[2] = 200; v.rgba[3] = 255;
        m.vertices.push_back(v);
    }
    const uint32_t b = p.first_vertex;
    for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) m.indices.push_back(ix);
    m.parts.push_back(p);
}
// ---------------------------------------------------------------- SEAT-CLEARANCE (headset test 6 item 4)
static void test_seat() {
    std::string id, why;
    { Options o; CHECK(parse_options("m", id, o, why) && o.seat_back == 0.30f); }
    { Options o; CHECK(parse_options("m;seatback:0.4", id, o, why) && o.seat_back == 0.4f); }
    { Options o; CHECK(parse_options("m;seatback=0", id, o, why) && o.seat_back == 0.0f); }
    { Options o; CHECK(parse_options("m;seatback:0.8", id, o, why) && o.seat_back == 0.8f); }
    for (const char* b : {"m;seatback:-0.1", "m;seatback:0.81", "m;seatback:nan", "m;seatback:inf", "m;seatback:", "m;seatback:far"}) {
        Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2) && w2 == "bad value for seatback"); }
    // the rule: the seat is only ever moved back; the headrest front = back_max - 0.15
    CHECK(std::fabs(seat_back_max(0.36f, 0.30f) - 0.45f) < 1e-6f);
    CHECK(seat_back_max(0.36f, 0.0f) == 0.36f);                       // off = the COCKPIT-4 value
    CHECK(seat_back_max(0.36f, 0.10f) == 0.36f);                      // never forward
    CHECK(std::fabs(seat_back_max(0.10f, 0.30f) - 0.45f) < 1e-6f);    // a short footprint is pushed back too
    CHECK(seat_back_max(0.36f, -1.0f) == 0.36f && seat_back_max(0.36f, std::nanf("")) == 0.36f);
    // meshes on the synthetic model: seatback:0 = the old placement (headrest front <= 0.21 m), default >= 0.30 m behind the eye; only seat vertices move
    const ModelData m = make_synthetic_model_c4();
    Options off; off.seat_back = 0.0f; Options on; Mesh a, b;
    CHECK(build_mesh(m, off, a, why) && build_mesh(m, on, b, why));
    const float U = kModelUnitsPerMetre;
    CHECK(a.seat_front_u <= 0.21f * U + 1e-4f);
    CHECK(b.seat_front_u >= 0.30f * U - 1e-4f && b.seat_front_u >= a.seat_front_u);
    CHECK(a.v.size() == b.v.size() && a.interior_triangles == b.interior_triangles && a.triangles == b.triangles);
    size_t moved = 0, moved_forward = 0;
    for (size_t i = 0; i < a.v.size(); ++i) {
        for (int k = 0; k < 3; ++k) if (a.v[i].pos[k] != b.v[i].pos[k]) { ++moved; if (k == 2 && b.v[i].pos[2] < a.v[i].pos[2] - 1e-5f) ++moved_forward; }
    }
    CHECK(moved > 0 && moved_forward == 0);                           // seat boxes / consoles shift aft, nothing moves toward the nose
    {   // the tub encloses the moved seat: an up-facing floor triangle reaches the seat's rear (xr_composite_warp "looking down" had a hole under the eye when the
        // synthetic cockpit footprint ends ahead of the eye and only the seat moved aft); with seatback:0 the floor is unchanged
        auto floor_rear = [&](const Mesh& me) { float zmax = -1e30f;
            for (size_t i = 0; i < me.anim_first; ++i) if (me.v[i].nrm[1] > 30000 && std::fabs(me.v[i].pos[1] - me.interior_floor_y) < 1e-4f) zmax = std::max(zmax, me.v[i].pos[2]);
            return zmax; };
        CHECK(floor_rear(b) >= b.eye[2] + 0.45f * U - 1e-3f);
        CHECK(floor_rear(a) < b.eye[2]);   // the synthetic cockpit ends ahead of the eye: the old floor did too
    }
    { Options z = off; z.interior = false; Options y = on; y.interior = false; Mesh c, d;   // no interior: the option has no effect at all
      CHECK(build_mesh(m, z, c, why) && build_mesh(m, y, d, why) && c.v.size() == d.v.size() && std::memcmp(c.v.data(), d.v.data(), c.v.size() * sizeof(GpuVertex)) == 0 && d.seat_front_u == 0.0f); }
    // the mesh stays independent of cscale (the option is in unscaled model metres): physical headrest distance = seat_front_u * cscale / upm
    { Options x = on; x.cscale = 1.15f; Mesh c; CHECK(build_mesh(m, x, c, why) && c.v.size() == b.v.size() && std::memcmp(c.v.data(), b.v.data(), b.v.size() * sizeof(GpuVertex)) == 0);
      CHECK(c.seat_front_u * x.cscale / x.upm >= 0.30f * 1.15f - 1e-3f); }
    // the interior still obeys the clip box: a box 0.35 m behind the eye clips the (now further back) seat there, never beyond
    { Options x; CHECK(parse_options("m;clipbox:1.9,0.35,1.2,1,1", id, x, why)); Mesh c; CHECK(build_mesh(m, x, c, why));
      for (size_t t = 0; t < c.tri_src.size(); ++t) { if (c.tri_src[t] != 2 || t * 3 >= c.anim_first) continue;
          for (int k = 0; k < 3; ++k) CHECK(c.v[t * 3 + k].pos[2] <= c.clip_hi[2] + 1e-4f); } }
}

static void test_cfid() {
    std::string id, why; Options o;
    // options: default ON and srefl follows it; explicit srefl wins; cfid:0 = off; bad values refused
    CHECK(parse_options("auto;anim:1;zpre:1;cscale:1.15", id, o, why) && o.fidelity && o.skin_refl && !o.skin_refl_set);
    CHECK(parse_options("auto;cfid:0", id, o, why) && !o.fidelity && !o.skin_refl);
    CHECK(parse_options("auto;cfid:0;srefl:1", id, o, why) && !o.fidelity && o.skin_refl);
    CHECK(parse_options("auto;srefl:0;cfid:1", id, o, why) && o.fidelity && !o.skin_refl);
    for (const char* b : {"m;cfid:2", "m;cfid:", "m;cfid=on"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    CHECK(offsetof(Constants, shb) == 56 * 4 && offsetof(Constants, shs) == 31 * 4 && kConstantDwords + 1 <= 64);   // + the table: root signature <= 64 DWORDs
    // constants
    EyeCam cam{}; for (int i = 0; i < 16; ++i) cam.m[i] = 0;
    const float box[4] = {4.27f, 1.78f, -37.53f, -32.13f};   // the j35jk_vrcockpit box (cockpit_shadow_box survey)
    { Options x; Constants k = make_constants(cam, x, 1.0f, 0); CHECK((k.flags & kCFlagFid) && !(k.flags & kCFlagShadow)); }
    { Options x; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, nullptr, box);
      CHECK((k.flags & kCFlagShadow) && k.shb[0] == 4.27f && k.shb[3] == -32.13f && k.shs == kShadowSoftU); }
    { Options x; x.fidelity = false; Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, nullptr, box);
      CHECK(!(k.flags & (kCFlagFid | kCFlagShadow)) && k.shs == 0.0f && k.shb[0] == 0.0f); }
    { Options x; const float bad[4] = {4.0f, 0.0f, -1, 1}, nan4[4] = {std::nanf(""), 1, -1, 1}, inv[4] = {4, 1, 1, -1};
      for (const float* b : {bad, nan4, inv}) { Constants k = make_constants(cam, x, 1.0f, 0, nullptr, nullptr, nullptr, nullptr, b); CHECK(!(k.flags & kCFlagShadow)); } }
    // sun response: skin 2 d^3 (DrawCPT_v +356 == 0), others saturate(d); the mean over d in [0,1] is the same (0.5) -> calibrated level kept
    { Options x; Constants k = make_constants(cam, x, 1.0f, 0); const uint16_t sk = uint16_t(1 | kMatTexAlphaNotCov), ck = 1;
      CHECK(std::fabs(sun_term(k, sk, 1.0f, nullptr) - 2.0f) < 1e-6f && std::fabs(sun_term(k, sk, 0.5f, nullptr) - 0.25f) < 1e-6f && sun_term(k, sk, -0.5f, nullptr) == 0.0f);
      CHECK(sun_term(k, ck, 0.5f, nullptr) == 0.5f && sun_term(k, ck, 1.5f, nullptr) == 1.0f && sun_term(k, ck, -0.3f, nullptr) == 0.0f);
      double a = 0, b = 0; const int N = 100000; for (int i = 0; i < N; ++i) { const float d = (i + 0.5f) / N; a += sun_term(k, sk, d, nullptr); b += sun_term(k, ck, d, nullptr); }
      CHECK(std::fabs(a / N - 0.5) < 1e-4 && std::fabs(b / N - 0.5) < 1e-4);
      Constants k0 = k; k0.flags &= ~kCFlagFid; CHECK(sun_term(k0, sk, 0.5f, nullptr) == 0.5f); }
    // shadow visibility (Draken box): overhead sun on the panel aft edge = partial (penumbra), sun ahead-high = glare-shield shadow, behind-high = lit,
    // outside the footprint / above the sill = 1, sun below the opening plane = 0
    { const float soft = kShadowSoftU; const float up[3] = {0, 1, 0}, ahead[3] = {0, 0.7f, -0.714143f}, behind[3] = {0, 0.7f, 0.714143f}, low[3] = {0.99f, 0.0f, 0.141f};
      const float panel[3] = {0, 3.0f, -37.4f}, out_side[3] = {2.5f, 3.0f, -36.0f}, above[3] = {0, 4.4f, -36.0f};
      const float x = (0.13f + soft) / (2 * soft), want = x * x * (3 - 2 * x);
      CHECK(std::fabs(shadow_visibility(box, soft, up, panel) - want) < 1e-4f);
      CHECK(shadow_visibility(box, soft, ahead, panel) == 0.0f && shadow_visibility(box, soft, behind, panel) == 1.0f);
      CHECK(shadow_visibility(box, soft, ahead, out_side) == 1.0f && shadow_visibility(box, soft, ahead, above) == 1.0f && shadow_visibility(box, soft, low, panel) == 0.0f);
      // left console under a sun from the right at 45 deg: the right rail shades it? no: the ray goes up-right and leaves through the opening -> lit;
      // the right console under the same sun: crossing x beyond half_x -> shadow
      const float sr[3] = {0.7071068f, 0.7071068f, 0}, lcon[3] = {-1.5f, 3.2f, -35.0f}, rcon[3] = {1.5f, 3.2f, -35.0f};
      CHECK(shadow_visibility(box, soft, sr, lcon) == 1.0f && shadow_visibility(box, soft, sr, rcon) == 0.0f);
      CHECK(shadow_visibility(box, 0.0f, up, panel) == 1.0f); }   // hard edge
    // cockpit_shadow_box: a glare shield (y 4.3, |x| <= 1.5, z -40..-37.5), two side rails (|x| 1.6..1.8, y up to 4.1, z -37..-33), an exterior plate and a
    // glass pane that must be ignored
    { ModelData m; m.has_eye = true; m.eye[0] = 0; m.eye[1] = 3.5f; m.eye[2] = -36.0f;
      const float nU[3] = {0, 1, 0}, nX[3] = {1, 0, 0};
      const float gs[4][3] = {{-1.5f, 4.3f, -37.5f}, {0.0f, 4.3f, -37.5f}, {0.0f, 4.3f, -40.0f}, {-1.5f, 4.3f, -40.0f}}, gs2[4][3] = {{0.0f, 4.3f, -37.5f}, {1.5f, 4.3f, -37.5f}, {1.5f, 4.3f, -40.0f}, {0.0f, 4.3f, -40.0f}};   // two halves: centre vertices
      const float rl[4][3] = {{-1.8f, 3.6f, -33.0f}, {-1.6f, 4.1f, -33.0f}, {-1.6f, 4.1f, -37.0f}, {-1.8f, 3.6f, -37.0f}};
      const float rr[4][3] = {{1.6f, 4.1f, -33.0f}, {1.8f, 3.6f, -33.0f}, {1.8f, 3.6f, -37.0f}, {1.6f, 4.1f, -37.0f}};
      const float ex[4][3] = {{-9, 9, -30}, {9, 9, -30}, {9, 9, -50}, {-9, 9, -50}}, gl[4][3] = {{-0.3f, 4.8f, -36}, {0.3f, 4.8f, -36}, {0.3f, 4.8f, -30}, {-0.3f, 4.8f, -30}};
      cfid_add_quad(m, gs, nU, 0); cfid_add_quad(m, gs2, nU, 0); cfid_add_quad(m, rl, nX, 0); cfid_add_quad(m, rr, nX, 0);
      cfid_add_quad(m, ex, nU, MF_EXTERIOR); cfid_add_quad(m, gl, nU, MF_GLASS | MF_BLEND);
      float b[4] = {0, 0, 0, 0};
      CHECK(cockpit_shadow_box(m, m.eye, b) && std::fabs(b[0] - 4.1f) < 1e-5f && std::fabs(b[1] - 0.9f * 1.8f) < 1e-5f && b[2] == -37.5f && b[3] == -33.0f);
      // build_mesh stores it; a fragment on the panel under a sun ahead gets the ambient only, under the sun behind the full Lambert term
      Options op; op.floor = false; op.interior = false; op.clip = false; op.display = false; op.ambient = 0.6f; Mesh me; CHECK(build_mesh(m, op, me, why) && me.shadow && me.shadow_box[0] == b[0]);
      const float ah[3] = {0, 0.7f, -0.714143f}, bh[3] = {0, 0.7f, 0.714143f};
      Constants ka = make_constants(cam, op, 1.0f, 0, ah, nullptr, nullptr, nullptr, me.shadow_box), kb = make_constants(cam, op, 1.0f, 0, bh, nullptr, nullptr, nullptr, me.shadow_box);
      const float uv[2] = {0.5f, 0.5f}, vc[4] = {1, 1, 1, 1}, nB[3] = {0, 0.6f, 0.8f}, pp[3] = {0, 3.0f, -37.4f};
      float fa[4], fb[4], f0[4];
      shade_fragment(me, ka, uint16_t(kNoTexture), uv, vc, nB, fa, nullptr, nullptr, pp);
      shade_fragment(me, kb, uint16_t(kNoTexture), uv, vc, nB, fb, nullptr, nullptr, pp);
      shade_fragment(me, ka, uint16_t(kNoTexture), uv, vc, nB, f0);   // no position: no shadow term
      const float lamA = 0.6f * 0.7f + 0.8f * -0.714143f, lamB = 0.6f * 0.7f + 0.8f * 0.714143f;   // lamA < 0 anyway: use the rim case below
      CHECK(std::fabs(fa[0] - 0.6f) < 1e-5f && std::fabs(fb[0] - (0.6f + 0.4f * lamB)) < 1e-5f && std::fabs(f0[0] - (0.6f + 0.4f * std::max(lamA, 0.0f))) < 1e-5f);
      // a back-facing panel normal (+Z, toward the pilot) lit from ahead-high: Lambert 0 either way; an up-facing knee panel lit from ahead-high: shadowed
      const float nUp[3] = {0, 1, 0}; float g1[4], g0[4];
      shade_fragment(me, ka, uint16_t(kNoTexture), uv, vc, nUp, g1, nullptr, nullptr, pp);
      shade_fragment(me, ka, uint16_t(kNoTexture), uv, vc, nUp, g0);
      CHECK(std::fabs(g1[0] - 0.6f) < 1e-5f && std::fabs(g0[0] - (0.6f + 0.4f * 0.7f)) < 1e-5f);
      // the same fragment flagged as exterior skin takes no shadow (only the 2 d^3 response)
      float s1[4]; shade_fragment(me, ka, uint16_t(kNoTexture | kMatTexAlphaNotCov), uv, vc, nUp, s1, nullptr, nullptr, pp);
      CHECK(std::fabs(s1[0] - std::min(1.0f, 0.6f + 0.4f * 2.0f * 0.343f)) < 1e-5f);
      // cfid:0 = the pre-CFID colour exactly
      Options o0 = op; o0.fidelity = false; Constants kz = make_constants(cam, o0, 1.0f, 0, ah, nullptr, nullptr, nullptr, me.shadow_box); float z1[4];
      shade_fragment(me, kz, uint16_t(kNoTexture), uv, vc, nUp, z1, nullptr, nullptr, pp); CHECK(z1[0] == g0[0]); }
    // no glare shield / only exterior parts: no box (and build_mesh keeps shadow false)
    { ModelData m; m.has_eye = true; m.eye[1] = 3.5f; m.eye[2] = -36.0f; const float nU[3] = {0, 1, 0};
      const float ex[4][3] = {{-9, 9, -30}, {9, 9, -30}, {9, 9, -50}, {-9, 9, -50}}; cfid_add_quad(m, ex, nU, MF_EXTERIOR);
      float b[4] = {7, 7, 7, 7}; CHECK(!cockpit_shadow_box(m, m.eye, b) && b[0] == 7.0f); }
    // synthetic model (no glare shield): the reference image with cfid:1 vs cfid:0 differs ONLY on skin pixels (the 2 d^3 response), looking back at the tail
    { const ModelData m = make_synthetic_model(); Options on; on.floor = false; on.interior = false; Options off = on; off.fidelity = false;
      Mesh me; CHECK(build_mesh(m, on, me, why) && !me.shadow);
      const float anchor[3] = {0, 1.2f, 0}, fov[4] = {-0.95f, 0.95f, 0.75f, -0.75f}; float q0[4], qb[4]; quat_ypr(0, 0, 0, q0); quat_ypr(kPi, 0, 0, qb);
      const float L[3] = {0.2f, 0.5f, -0.84f};   // toward the tail's visible face (normal model -Z) and up
      for (int view = 0; view < 2; ++view) {
          EyeCam c; CHECK(eye_camera(view ? qb : q0, anchor, fov, anchor, 0.0f, me.eye, 5.0f, 0.05f, c));
          Target a; a.w = 96; a.h = 54; a.rgba.assign(size_t(a.w) * a.h * 4, 0.0f); a.depth.assign(size_t(a.w) * a.h, 0.0f); Target b = a;
          render_reference(me, on, 1.0f, 0, c, a, L); render_reference(me, off, 1.0f, 0, c, b, L);
          if (view == 0) CHECK(a.rgba == b.rgba);   // forward: no skin in sight -> identical
          else { size_t diff = 0; for (size_t i = 0; i < a.rgba.size(); ++i) diff += a.rgba[i] != b.rgba[i]; CHECK(diff > 0); }
      } }
}


// ---------------------------------------------------------------------------------------------------------------- STORES-RT
// The baked weapon parts ("<model>/store<station>/n<node>", source part 7) form one piece per station in the animated range; apply_store_mask collapses
// the stations a mask names. Offline mirror of f05ek_vrcockpit (stations 67, 68, 75, 76) with synthetic weapon quads. BEFORE (HEAD 16e39b6f) the weapons were
// ordinary static parts: no store_stations, no mask, no way to hide a fired weapon (these tests do not even compile against that source).
static ModelData stores_model(bool left_only = false, bool sp = false) {
    ModelData m = make_synthetic_model_c4();
    auto add = [&](const char* name, float cx, float cy, float cz, uint32_t group, int32_t anim) {
        Part p;
        p.name_off = uint32_t(m.strings.size()); m.strings += name; m.strings.push_back('\0');
        p.node = uint32_t(m.parts.size()); p.mflags = MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE; p.texture = 1; p.source_part = 7; p.group = group; p.anim = anim;
        for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
        p.first_vertex = uint32_t(m.vertices.size()); p.vertex_count = 4;
        p.first_index = uint32_t(m.indices.size()); p.index_count = 6;
        const float xs[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, zs[4] = {0, 0, -3, -3};
        for (int i = 0; i < 4; ++i) {
            Vertex v{};
            v.pos[0] = cx + xs[i]; v.pos[1] = cy; v.pos[2] = cz + zs[i]; v.nrm[1] = 32767; v.part = uint16_t(m.parts.size()); v.rgba[0] = v.rgba[1] = v.rgba[2] = v.rgba[3] = 255;
            m.vertices.push_back(v);
        }
        const uint32_t b = p.first_vertex;
        for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) m.indices.push_back(ix);
        m.parts.push_back(p);
    };
    // 12/13: station 67 (two nodes; the second carries the aileron node group 5 and an anim channel: it must NOT become a control surface)
    add("t/store67/n0", left_only ? -15.4f : 15.4f, -4.2f, -4.3f, 1, -1);
    add("t/store67/n1", left_only ? -15.4f : 15.4f, -4.2f, -4.3f, 5, 0);
    add("t/store68/n0", -15.4f, -4.2f, -4.3f, 1, -1);          // 14
    add("t/store75/n0", left_only ? -11.8f : 11.8f, -4.4f, -4.3f, 1, -1);   // 15
    add("t/store76/n0", -11.8f, -4.4f, -4.3f, 1, -1);          // 16
    if (sp) {   // SP-STORES: the special-weapon group (the F-5E rack 54: stations 71..74; 71 has two nodes like station 67 above)
        add("t/sp71/n0", -15.4f, -4.3f, -5.4f, 1, -1);         // 17
        add("t/sp71/n1", -15.4f, -4.3f, -5.4f, 5, 0);          // 18 (aileron node group + an anim channel: still no control surface)
        add("t/sp72/n0", 15.4f, -4.3f, -5.4f, 1, -1);          // 19
        add("t/sp73/n0", -11.8f, -4.3f, -4.3f, 1, -1);         // 20
        add("t/sp74/n0", 11.8f, -4.3f, -4.3f, 1, -1);          // 21
    }
    return m;
}
static void test_stores() {
    uint32_t st = 99;
    CHECK(store_station_of_name("f05ek_vrcockpit/store67/n3", st) && st == 67);
    CHECK(store_station_of_name("a/store5/b/store76/n0", st) && st == 5);
    CHECK(!store_station_of_name("f05ek_vrcockpit/p0/n3", st) && !store_station_of_name("x/store/n0", st) && !store_station_of_name("x/store67", st));
    CHECK(!store_station_of_name("x/storeX/n0", st) && !store_station_of_name("", st) && !store_station_of_name("x/store12345678901/n0", st));
    std::string why, id; Options o;
    CHECK(parse_options("m", id, o, why) && o.stores && !o.stwatch);
    CHECK(parse_options("m;stores:0;stwatch:1", id, o, why) && !o.stores && o.stwatch);
    for (const char* b : {"m;stores:2", "m;stwatch:yes", "m;stores:"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    // MSL-HUD (hud_msl.h): mslhud:0|1|2, default 1 (on since the step172 live pass; mslhud:0 = off); anything else refuses the model
    { Options x; std::string i2, w2; CHECK(parse_options("m", i2, x, w2) && x.mslhud == 1 && x.strel == 3.0f); }
    { Options x; std::string i2, w2; CHECK(parse_options("m;mslhud:1", i2, x, w2) && x.mslhud == 1); }
    { Options x; std::string i2, w2; CHECK(parse_options("m;mslhud:0", i2, x, w2) && x.mslhud == 0); }
    { Options x; std::string i2, w2; CHECK(parse_options("m;mslhud:2", i2, x, w2) && x.mslhud == 2); }
    for (const char* b : {"m;mslhud:3", "m;mslhud:", "m;mslhud:12", "m;mslhud:on"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    { Options x; std::string i2, w2; CHECK(parse_options("m;strel:1.5", i2, x, w2) && x.strel == 1.5f); }
    { Options x; std::string i2, w2; CHECK(parse_options("m;strel:0", i2, x, w2) && x.strel == 0.0f); }
    for (const char* b : {"m;strel:-1", "m;strel:61", "m;strel:", "m;strel:x"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    const ModelData m = stores_model();
    const auto bytes = serialize_model(m);
    ModelData rt; CHECK(parse_model(bytes.data(), bytes.size(), rt) == LoadStatus::ok && serialize_model(rt) == bytes && rt.part_name(12) == "t/store67/n0");
    Options on; Mesh me; CHECK(build_mesh(m, on, me, why));
    CHECK(me.store_stations.size() == 4 && me.store_stations[0] == 67 && me.store_stations[1] == 68 && me.store_stations[2] == 75 && me.store_stations[3] == 76);
    unsigned pieces = 0; uint32_t sv = 0;
    for (const AnimPiece& a : me.anim) if (a.role == AnimRole::store) { ++pieces; sv += a.count; CHECK(a.naxes == 0 && a.first >= me.anim_first && a.first + a.count <= me.anim_first + me.anim_vertices); }
    CHECK(pieces == 4 && sv == 5 * 6);                 // 5 weapon parts x 6 corners
    for (const AnimPiece& a : me.anim) if (a.role == AnimRole::store && a.station == 67) CHECK(std::fabs(a.pivot[0] - 15.4f) < 1e-4f && std::fabs(a.pivot[1] + 4.2f) < 1e-4f);
    // rest = the baked geometry: the stores-on mesh holds the same vertices as stores:0 (same triangles, the weapon parts moved to the animated range)
    Options off = on; off.stores = false; Mesh m0; CHECK(build_mesh(m, off, m0, why) && m0.store_stations.empty() && m0.triangles == me.triangles);
    auto count_at = [](const Mesh& x, float px) { unsigned n = 0; for (const GpuVertex& v : x.v) if (std::fabs(v.pos[0] - px) < 0.6f && std::fabs(v.pos[1] + 4.2f) < 1e-3f) ++n; return n; };
    CHECK(count_at(me, 15.4f) == count_at(m0, 15.4f) && count_at(me, 15.4f) == 12);
    // the mask
    std::vector<GpuVertex> pose(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices), keep = pose;
    CHECK(apply_store_mask(me, 0, pose) == 0 && std::memcmp(pose.data(), keep.data(), pose.size() * sizeof(GpuVertex)) == 0);   // mask 0 = rest
    CHECK(apply_store_mask(me, 1u, pose) == 12);       // station 67 (bit 0): two parts x 6 corners
    unsigned collapsed = 0, intact = 0;
    for (const AnimPiece& a : me.anim) {
        if (a.role != AnimRole::store) continue;
        for (uint32_t i = 0; i < a.count; ++i) {
            const GpuVertex& g = pose[a.first - me.anim_first + i];
            const bool at_pivot = g.pos[0] == a.pivot[0] && g.pos[1] == a.pivot[1] && g.pos[2] == a.pivot[2];
            if (a.station == 67) { CHECK(at_pivot); ++collapsed; } else { CHECK(!at_pivot); ++intact; }
        }
    }
    CHECK(collapsed == 12 && intact == 18);
    for (size_t i = 0; i + 2 < pose.size(); i += 3) {   // a collapsed triangle has zero area (nothing is rasterised)
        const float* a = pose[i].pos; const float* b = pose[i + 1].pos; const float* c = pose[i + 2].pos;
        if (a[0] == b[0] && a[0] == c[0] && a[1] == b[1] && a[1] == c[1] && a[2] == b[2] && a[2] == c[2]) continue;
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float g[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        CHECK(g[0] * g[0] + g[1] * g[1] + g[2] * g[2] > 0.0f);   // every surviving triangle still has area
    }
    pose = keep; CHECK(apply_store_mask(me, 0xFu, pose) == 5 * 6 && apply_store_mask(me, 0xFFF0u, keep) == 0);   // bits beyond the stations change nothing
    std::vector<GpuVertex> small(3); CHECK(apply_store_mask(me, 0xFu, small) == 0);   // wrong size: untouched
    std::vector<GpuVertex> only, ref(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices);
    pose_stores_only(me, 0x2u, only); CHECK(only.size() == me.anim_vertices && apply_store_mask(me, 0x2u, ref) == 6 && std::memcmp(only.data(), ref.data(), only.size() * sizeof(GpuVertex)) == 0);
    // anim:1: the node-group-5 fin of station 67 is not an aileron; the stations stay one piece each next to the control surface (the elevator, part 11)
    { Options oa = on; oa.anim = true; Mesh ma; CHECK(build_mesh(m, oa, ma, why) && ma.anim_surfaces == 1 && ma.store_stations.size() == 4);
      std::vector<GpuVertex> p1; pose_anim(ma, AnimInputs{}, p1); CHECK(p1.size() == ma.anim_vertices);
      unsigned n = 0; for (const AnimPiece& a : ma.anim) if (a.role == AnimRole::store) ++n; CHECK(n == 4);
      CHECK(apply_store_mask(ma, 0x5u, p1) == 12 + 6); }
    // a store part that is blended or hidden is never a piece (the mask cannot reach blend parts, hidden parts are not drawn)
    { ModelData b = m; b.parts[14].mflags |= MF_BLEND; Mesh mb; CHECK(build_mesh(b, on, mb, why) && mb.store_stations.size() == 3);
      b = m; b.parts[14].mflags |= MF_HIDDEN; CHECK(build_mesh(b, on, mb, why) && mb.store_stations.size() == 3); }
    // a model without weapon parts: no stations, the old behaviour
    { Mesh mc; CHECK(build_mesh(make_synthetic_model_c4(), on, mc, why) && mc.store_stations.empty()); }
}
static void test_store_tracker() {
    namespace S = acz::stores;
    std::string why; Options o; Mesh me; CHECK(build_mesh(stores_model(), o, me, why));
    // firing order: right wing first, outboard first, alternating: 67 (+15.4) 68 (-15.4) 75 (+11.8) 76 (-11.8) = bits 0, 1, 2, 3
    const auto ord = S::firing_order(me);
    CHECK(ord.size() == 4 && ord[0] == 0 && ord[1] == 1 && ord[2] == 2 && ord[3] == 3);
    S::Tracker t; CHECK(t.stations() == 0 && t.hidden_mask(0) == 0);   // unconfigured
    t.launch(0); CHECK(t.hidden_mask(0) == 0);                          // nothing to hide
    t.configure(me);
    CHECK(t.stations() == 4 && t.fired() == 0 && t.hidden_mask() == 0 && t.reload_ms() == 3000.0);
    // a single launch: the first station empties for the reload time, then shows again (the old tracker kept it hidden for good)
    t.launch(1000); CHECK(t.fired() == 1 && t.hidden_mask(1000) == 0x1u);
    CHECK(t.hidden_mask(3999) == 0x1u && t.hidden_mask(4000) == 0u && t.fired() == 0);
    // step172 salvo: 4 launches 7 s apart, remaining 50 -> 46: each station is back before the next shot, the order alternates 67, 68, 75, 76, 67
    { S::Tracker s; s.configure(me); s.set_remaining(50);
      const uint32_t want[5] = {0x1u, 0x2u, 0x4u, 0x8u, 0x1u};
      for (int i = 0; i < 5; ++i) {
          const double tl = 70000.0 + 7000.0 * i;
          s.set_remaining(unsigned(50 - i)); s.launch(tl);
          CHECK(s.hidden_mask(tl + 100) == want[i]);                        // the fired pylon is empty right after the launch, only that one
          CHECK(s.hidden_mask(tl + 2999) == want[i] && s.hidden_mask(tl + 3000) == 0u);   // and full again after the reload
          CHECK(s.hidden_mask(tl + 6900) == 0u);                            // before the next shot every station is shown
      } }
    // rapid fire 1 s apart: several stations are empty at once, each returns 3 s after its own shot
    { S::Tracker s; s.configure(me);
      s.launch(0); s.launch(1000); s.launch(2000);
      CHECK(s.hidden_mask(2000) == 0x7u && s.fired() == 3);
      CHECK(s.hidden_mask(3000) == 0x6u);                                   // station 0 is back
      s.launch(3000); CHECK(s.hidden_mask(3000) == 0xEu);                   // the 4th shot takes the next shown one: 76 (bit 3)
      CHECK(s.hidden_mask(4500) == 0xCu && s.hidden_mask(5500) == 0x8u && s.hidden_mask(6000) == 0u);
      // every station empty and one more shot: the turn restarts that station's reload, the others keep theirs
      S::Tracker u; u.configure(me); u.launch(0); u.launch(100); u.launch(200); u.launch(300); CHECK(u.hidden_mask(300) == 0xFu);
      u.launch(400); CHECK(u.hidden_mask(3350) == 0x1u && u.hidden_mask(3400) == 0u); }
    // remaining < stations: only `remaining` stay shown (2 left of 4: two stations hidden), whatever the reload says
    { S::Tracker s; s.configure(me); s.set_remaining(2);
      CHECK(s.hidden_mask(0) == 0x3u);                                      // the two first in the firing order are held empty
      s.launch(10000); CHECK(s.hidden_mask(10000) == 0x5u);   // the shown stations stay 2: bit 2 reloading, bit 0 held
      // after a launch with 2 left the shown count is still capped at remaining (the count falls with it)
      s.set_remaining(1); CHECK(s.hidden_mask(10000) == 0x7u);
      s.set_remaining(0); CHECK(s.hidden_mask(20000) == 0xFu);
      s.set_remaining(3); CHECK(s.hidden_mask(30000) == 0x1u);
      s.set_remaining(4); CHECK(s.hidden_mask(30000) == 0u); s.set_remaining(99); CHECK(s.hidden_mask(30000) == 0u);
      s.set_remaining(0); s.rearm(); CHECK(s.hidden_mask(30000) == 0u && s.rearms() == 1); }   // a rearm shows all (the cap is cleared)
    { S::Tracker s; s.configure(me); s.set_remaining(2); CHECK(s.hidden_mask(0) == 0x3u && s.fired() == 2); }
    // reload time option: 0 = instant return, 10 s = held longer
    { S::Tracker s; s.configure(me); s.set_reload_ms(0.0); s.launch(5); CHECK(s.hidden_mask(5) == 0u);
      s.set_reload_ms(10000.0); s.launch(5); CHECK(s.hidden_mask(9999) == 0x2u && s.hidden_mask(10005) == 0u); s.set_reload_ms(-5.0); CHECK(s.reload_ms() == 0.0); }
    // one wing only: that wing's stations fire in turn (outboard first)
    { Mesh ml; CHECK(build_mesh(stores_model(true), o, ml, why));   // stations 67, 75 left (-15.4, -11.8), 68, 76 left too
      const auto ol = S::firing_order(ml); CHECK(ol.size() == 4 && ol[0] == 0 && ol[1] == 1 && ol[2] == 2 && ol[3] == 3); }
    // the mailbox: events are applied once, in order (rearm, absolute count, launches), launches stamped with the sync time
    S::Signal sg; S::Tracker u; u.configure(me);
    CHECK(!sg.sync(u, 0) && u.hidden_mask() == 0);
    sg.note_launch(); sg.note_launch(); CHECK(sg.sync(u, 100) && u.hidden_mask() == 0x3u && !sg.sync(u, 150));
    CHECK(u.hidden_mask(3099) == 0x3u && u.hidden_mask(3101) == 0u);   // both launched at t = 100: reload 3 s
    sg.note_rearm(); CHECK(sg.sync(u, 4000) && u.hidden_mask() == 0);
    sg.note_launch(); sg.note_rearm(); sg.note_launch(); CHECK(sg.sync(u, 5000) && u.fired() == 2 && u.rearms() == 2);   // rearm first, then the two launches (events are counted, not ordered)
    sg.set_remaining(1); CHECK(sg.sync(u, 5100) && u.fired() == 3);          // 1 left: three held empty (the reloading one is one of them)
    sg.note_launch(); sg.discard(); CHECK(!sg.sync(u, 5200) && u.fired() == 3);   // signals of the previous aircraft are dropped
    CHECK(&S::signal() == &S::signal() && S::signal().launches() == 0);     // the process-wide mailbox is one object and nobody has signalled
}
// SP-STORES (aircraft-auto.md section 15): the baked special-weapon parts ("<model>/sp<station>/...") are their own pieces with their own mask and tracker, so the
// SP pylons empty and reload from the HUD's SP row independently of the MSSL ones. BEFORE this change an "sp" part was an ordinary static part: no sp_stations, no
// sp mask, no second tracker (these tests do not compile against that source).
static void test_sp_stores() {
    namespace S = acz::stores;
    uint32_t st = 99;
    CHECK(sp_station_of_name("f05ek_vrcockpit/sp71/n3", st) && st == 71);
    CHECK(!sp_station_of_name("f05ek_vrcockpit/store67/n3", st) && !sp_station_of_name("f05ek_vrcockpit/p0/n3", st) && !sp_station_of_name("f05ek_vrcockpit/tank79/n0", st));
    CHECK(!sp_station_of_name("x/sp/n0", st) && !sp_station_of_name("x/spX/n0", st) && !sp_station_of_name("x/sp71", st) && !sp_station_of_name("x/sp12345678901/n0", st) && !sp_station_of_name("", st));
    CHECK(!store_station_of_name("f05ek_vrcockpit/sp71/n3", st));       // the standard mask never claims an SP part
    std::string why, id; Options o;
    CHECK(parse_options("m", id, o, why) && o.spstores && o.stores);
    CHECK(parse_options("m;spstores:0", id, o, why) && !o.spstores && o.stores);
    CHECK(parse_options("m;spstores:1;strel:2", id, o, why) && o.spstores);
    for (const char* b : {"m;spstores:2", "m;spstores:", "m;spstores:on"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
    const ModelData m = stores_model(false, true);
    Options on; Mesh me; CHECK(build_mesh(m, on, me, why));
    CHECK(me.store_stations.size() == 4 && me.store_stations[0] == 67 && me.store_stations[3] == 76);       // the standard group is what it was
    CHECK(me.sp_stations.size() == 4 && me.sp_stations[0] == 71 && me.sp_stations[1] == 72 && me.sp_stations[2] == 73 && me.sp_stations[3] == 74);
    unsigned std_pieces = 0, sp_pieces = 0, sp_verts = 0;
    for (const AnimPiece& a : me.anim) {
        if (a.role != AnimRole::store) continue;
        if (a.sp) { ++sp_pieces; sp_verts += a.count; } else ++std_pieces;
        CHECK(a.naxes == 0 && a.first >= me.anim_first && a.first + a.count <= me.anim_first + me.anim_vertices);
    }
    CHECK(std_pieces == 4 && sp_pieces == 4 && sp_verts == 5 * 6);       // 5 SP parts x 6 corners (station 71 has two nodes)
    for (const AnimPiece& a : me.anim) if (a.role == AnimRole::store && a.sp && a.station == 72) CHECK(std::fabs(a.pivot[0] - 15.4f) < 1e-4f && std::fabs(a.pivot[1] + 4.3f) < 1e-4f);
    // spstores:0 and stores:0: the SP parts are ordinary static parts (same triangles, no pieces, no stations, never hidden)
    { Options x = on; x.spstores = false; Mesh ms; CHECK(build_mesh(m, x, ms, why) && ms.sp_stations.empty() && ms.store_stations.size() == 4 && ms.triangles == me.triangles);
      unsigned n = 0; for (const AnimPiece& a : ms.anim) if (a.sp) ++n; CHECK(n == 0);
      Options y = on; y.stores = false; Mesh m0; CHECK(build_mesh(m, y, m0, why) && m0.sp_stations.empty() && m0.store_stations.empty() && m0.triangles == me.triangles); }
    // the two masks are independent bit spaces over one animated range
    std::vector<GpuVertex> rest(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices), pose = rest;
    CHECK(apply_sp_mask(me, 0, pose) == 0 && std::memcmp(pose.data(), rest.data(), pose.size() * sizeof(GpuVertex)) == 0);   // mask 0 = rest
    CHECK(apply_sp_mask(me, 1u, pose) == 12);                             // station 71 (bit 0): two nodes x 6 corners
    for (const AnimPiece& a : me.anim) {
        if (a.role != AnimRole::store) continue;
        for (uint32_t i = 0; i < a.count; ++i) {
            const GpuVertex& g = pose[a.first - me.anim_first + i];
            const bool at_pivot = g.pos[0] == a.pivot[0] && g.pos[1] == a.pivot[1] && g.pos[2] == a.pivot[2];
            if (a.sp && a.station == 71) CHECK(at_pivot); else CHECK(!at_pivot);   // only SP 71 collapsed: not SP 72..74, not any standard piece
        }
    }
    { std::vector<GpuVertex> p = rest; const uint32_t n = apply_store_mask(me, 0xFu, p); CHECK(n == 5 * 6);   // the 5 standard parts only
      for (const AnimPiece& a : me.anim) if (a.role == AnimRole::store && a.sp) for (uint32_t i = 0; i < a.count; ++i) CHECK(std::memcmp(&p[a.first - me.anim_first + i], &rest[a.first - me.anim_first + i], sizeof(GpuVertex)) == 0); }
    { std::vector<GpuVertex> p = rest; CHECK(apply_sp_mask(me, 0xFu, p) == 5 * 6 && apply_sp_mask(me, 0xFFF0u, rest) == 0); }   // bits beyond the stations change nothing
    { std::vector<GpuVertex> p = rest, sm(3); CHECK(apply_sp_mask(me, 0xFu, sm) == 0); }                  // wrong size: untouched
    { std::vector<GpuVertex> only, ref = rest;
      pose_stores_only(me, 0x2u, 0x4u, only);
      apply_store_mask(me, 0x2u, ref); apply_sp_mask(me, 0x4u, ref);
      CHECK(only.size() == me.anim_vertices && std::memcmp(only.data(), ref.data(), only.size() * sizeof(GpuVertex)) == 0);
      std::vector<GpuVertex> o3; pose_stores_only(me, 0x2u, 0u, o3); std::vector<GpuVertex> o2; pose_stores_only(me, 0x2u, o2);
      CHECK(o3.size() == o2.size() && std::memcmp(o3.data(), o2.data(), o3.size() * sizeof(GpuVertex)) == 0); }   // sp mask 0: the old overload's result
    // anim:1: the group-5 node of SP station 71 is not an aileron either
    { Options oa = on; oa.anim = true; Mesh ma; CHECK(build_mesh(m, oa, ma, why) && ma.anim_surfaces == 1 && ma.sp_stations.size() == 4 && ma.store_stations.size() == 4);
      std::vector<GpuVertex> p1; pose_anim(ma, AnimInputs{}, p1); CHECK(apply_sp_mask(ma, 0x6u, p1) == 6 + 6); }
    // two trackers: the standard order is untouched by the SP pieces, the SP order is its own (right wing outboard first: bit 1 = 72 (+15.4), then 71, 74, 73)
    const auto ord = S::firing_order(me);
    CHECK(ord.size() == 4 && ord[0] == 0 && ord[1] == 1 && ord[2] == 2 && ord[3] == 3);
    const auto sord = S::firing_order(me, true);
    CHECK(sord.size() == 4 && sord[0] == 1 && sord[1] == 0 && sord[2] == 3 && sord[3] == 2);
    S::Tracker t, tsp; t.configure(me); tsp.configure(me, true); tsp.set_reload_ms(3000.0);
    CHECK(t.stations() == 4 && tsp.stations() == 4);
    tsp.launch(1000); CHECK(tsp.hidden_mask(1000) == 0x2u && t.hidden_mask(1000) == 0u);   // an SP shot empties SP station 72 and no standard one
    t.launch(1200);   CHECK(t.hidden_mask(1200) == 0x1u && tsp.hidden_mask(1200) == 0x2u);  // and the other way round
    CHECK(tsp.hidden_mask(3999) == 0x2u && tsp.hidden_mask(4000) == 0u && t.hidden_mask(4199) == 0x1u && t.hidden_mask(4200) == 0u);   // the same 3 s reload for both
    // the SP mailbox is a different object from the standard one: SP signals never reach the MSSL tracker
    CHECK(&S::sp_signal() != &S::signal() && &S::sp_signal() == &S::sp_signal());
    { const uint32_t before = S::signal().launches(); S::sp_signal().note_launch();
      S::Tracker u, usp; u.configure(me); usp.configure(me, true);
      CHECK(S::sp_signal().sync(usp, 500) && usp.fired() == 1 && S::signal().launches() == before);
      CHECK(!S::signal().sync(u, 500) && u.fired() == 0);
      S::sp_signal().set_remaining(1); CHECK(S::sp_signal().sync(usp, 600) && usp.fired() == 3);   // 1 SP left of 4: three held empty
      S::sp_signal().note_rearm(); CHECK(S::sp_signal().sync(usp, 700) && usp.fired() == 0); S::sp_signal().discard(); }
}
// STORES-RT search (ee_store_watch.h): a fake process whose window holds a missile counter 4 -> 3 -> 2 (reload to 4 later), a ticking timer and static data.
namespace {
struct FakeMem final : acz::e6loc::ReadMem {
    std::vector<uint8_t> mem; uint64_t base = 0; bool fail = false; unsigned reads = 0;
    bool read(uint64_t a, void* out, uint32_t n) noexcept override {
        ++reads;
        if (fail || a < base || a + n > base + mem.size()) return false;
        std::memcpy(out, mem.data() + (a - base), n); return true;
    }
    void set(uint32_t off, uint32_t v) { std::memcpy(&mem[off], &v, 4); }
};
struct CaptureSink final : acz::e6loc::Sink {
    std::vector<std::string> lines;
    void line(bool, const char* t) noexcept override { lines.emplace_back(t); }
    unsigned count(const char* needle) const { unsigned n = 0; for (const auto& l : lines) if (l.find(needle) != std::string::npos) ++n; return n; }
};
}
static void test_store_watch() {
    namespace W = acz::e6store;
    FakeMem fm; fm.base = 0x10000000ull; fm.mem.assign(0x20000, 0);
    const uint64_t structure = fm.base + 0x8000;            // window A start; the watch reads [structure - 0x4000, + 0x4000)
    const uint32_t counter = 0x8000 - 0x1230;               // offset in the fake block: -0x1230 from the structure
    for (uint32_t i = 0; i < 64; ++i) fm.set(0x4000 + i * 4, 0x3f800000u + i);   // static floats
    fm.set(counter, 4);
    CaptureSink sink; W::AmmoWatch w;
    CHECK(!w.active() && w.tick(fm, structure, 0, sink) == 0 && fm.reads == 0);    // not started: no read at all
    CHECK(w.start() && w.active() && w.start());
    double t = 1000;
    CHECK(w.tick(fm, 0, t, sink) == 0 && fm.reads == 0);                           // not locked: nothing read
    CHECK(w.tick(fm, structure, t, sink) == 0 && fm.reads == 1);                   // baseline
    CHECK(w.tick(fm, structure, t + 100, sink) == 0 && fm.reads == 1);             // inside the period
    uint32_t timer = 1000;
    for (int k = 1; k <= 60; ++k) {                                                // a timer word changes every tick: muted, never a counter
        t += 250; fm.set(0x9000, timer--); w.tick(fm, structure, t, sink);
    }
    CHECK(w.muted() == 1 && w.decrements() == 0 && sink.count("event=decrement") == 0);
    t += 250; fm.set(counter, 3); CHECK(w.tick(fm, structure, t, sink) == 1);      // a launch
    CHECK(sink.count("event=decrement;off=-4656;") == 1 && sink.count("old=4;new=3") == 1);
    t += 250; fm.set(counter, 2); w.tick(fm, structure, t, sink);
    CHECK(w.decrements() == 2 && sink.count("old=3;new=2") == 1);
    t += 250; fm.set(counter, 4); w.tick(fm, structure, t, sink);                  // refill
    CHECK(w.rises() == 1 && sink.count("event=rise;") == 1 && sink.count("old=2;new=4") == 1);
    // a word that was never a counter and jumps up is not a rise; a change of more than one is not a decrement
    t += 250; fm.set(0x6000, 7); w.tick(fm, structure, t, sink); t += 250; fm.set(0x6000, 20); w.tick(fm, structure, t, sink);
    CHECK(w.rises() == 1 && w.decrements() == 2);
    // unreadable window: skipped, no line, baseline kept; a relock at another base restarts the baseline (no spurious events)
    fm.fail = true; t += 250; CHECK(w.tick(fm, structure, t, sink) == 0); fm.fail = false;
    t += 250; fm.set(counter, 3); CHECK(w.tick(fm, structure, t, sink) == 1);     // the baseline survived the failed read
    t += 250; const uint64_t other = structure + 0x40; CHECK(w.tick(fm, other, t, sink) == 0);
    // the summary is unconditional: a quiet watch is told from a dead one
    t += 11000; const unsigned before = sink.count("event=summary"); w.tick(fm, other, t, sink); CHECK(sink.count("event=summary") == before + 1 && sink.count("muted-words=1") >= 1);
    // the line budget: 32 counters all counting down every tick (each logs until it is muted after kMuteChanges changes): never more than kMaxLines lines
    { FakeMem fm2; fm2.base = fm.base; fm2.mem.assign(0x20000, 0); CaptureSink s2; W::AmmoWatch w2; CHECK(w2.start());
      double t2 = 0; for (unsigned c = 0; c < 32; ++c) fm2.set(0x6000 + c * 4, 99);
      w2.tick(fm2, structure, t2, s2);
      for (unsigned k = 1; k <= 40; ++k) { t2 += 250; for (unsigned c = 0; c < 32; ++c) fm2.set(0x6000 + c * 4, 99 - k); w2.tick(fm2, structure, t2, s2); }
      CHECK(w2.logged() == W::kMaxLines && w2.muted() == 32 && s2.count("event=decrement") == W::kMaxLines); }
}

// STORES-RT wide counter search (ee_counter_search.h): synthetic images; the API did not exist before, so these cannot pass at the previous commit.
static void test_counter_search() {
    namespace S = acz::e6search;
    const uint32_t kChunk = 0x10000;
    auto set8 = [](FakeMem& fm, uint32_t off, uint8_t v) { fm.mem[off] = v; };
    auto set16 = [](FakeMem& fm, uint32_t off, uint16_t v) { std::memcpy(&fm.mem[off], &v, 2); };
    auto setf = [](FakeMem& fm, uint32_t off, float v) { std::memcpy(&fm.mem[off], &v, 4); };
    FakeMem fm; fm.base = 0x20000000ull; fm.mem.assign(0x40000, 0);   // 4 chunks of 64 KiB
    const uint32_t kCtr = 0x1FFFC, kCtr2 = 0x2A000, kFlt = 0x31000, kStatic = 0x5000, kTimer = 0x6000, kRise = 0x7000, kJump = 0x8000, kText = 0x9000, kU8 = 0x3F001;
    fm.set(kCtr, 50);                        // u32 counter, zero neighbours: u8, u16 and u32 all match (w=124)
    set16(fm, kCtr2, 50); set16(fm, kCtr2 + 2, 0x1234);   // u16 counter with a busy upper half: u8 + u16 only (w=12)
    setf(fm, kFlt, 50.0f);                   // float counter
    fm.set(kStatic, 50);                     // a 50 that never moves
    fm.set(kTimer, 50); fm.set(kRise, 50); fm.set(kJump, 50);
    for (uint32_t i = 0; i < 16; ++i) set8(fm, kText + i * 2, 0x32);   // ASCII '2' = 50 as u8 (text noise)
    set8(fm, kU8, 50);                       // a u8-only counter at an odd address
    CaptureSink sink; S::CounterSearch cs;
    CHECK(!cs.active() && cs.tick(fm, fm.base, fm.base + 0x8000, 0, sink) == 0 && fm.reads == 0);   // not started: nothing read
    CHECK(cs.start(50, fm.mem.size(), kChunk) && cs.active() && cs.start(50, fm.mem.size(), kChunk));
    { S::CounterSearch bad1, bad2; CHECK(!bad1.start(0, 100, 10) && !bad2.start(50, 0, 10)); }
    const uint64_t sb = fm.base + 0x8000; double t = 1000;
    CHECK(cs.tick(fm, 0, 0, t, sink) == 0 && fm.reads == 0);               // not locked: no read
    CHECK(cs.tick(fm, fm.base, sb, t, sink) == 0 && cs.scans() == 1 && fm.reads == 4);   // seed: every chunk read once
    CHECK(cs.alive() >= 7 && cs.moved() == 0 && !cs.truncated());
    const unsigned reads = fm.reads; CHECK(cs.tick(fm, fm.base, sb, t + 400, sink) == 0 && fm.reads == reads);   // inside the period
    t += 1000; CHECK(cs.tick(fm, fm.base, sb, t, sink) == 0 && sink.count("event=survivors") == 0);   // nothing moved: quiet
    // 1 Hz steps: the counters count 50 -> 49 -> 48 -> 47 -> 46 (the HUD), the timer wanders, the 'rise' goes up, the 'jump' falls by 5
    const uint32_t seq[5] = {50, 49, 48, 47, 46};
    for (int k = 1; k <= 4; ++k) {
        t += 1000;
        fm.set(kCtr, seq[k]); set16(fm, kCtr2, uint16_t(seq[k])); setf(fm, kFlt, float(seq[k])); set8(fm, kU8, uint8_t(seq[k]));
        fm.set(kTimer, 50 - k * 7); fm.set(kRise, 50 + k); fm.set(kJump, 45);
        cs.tick(fm, fm.base, sb, t, sink);
    }
    CHECK(sink.count("event=survivors") == 4);
    std::string last; for (const auto& l : sink.lines) if (l.find("event=survivors") != std::string::npos) last = l;
    char want[160];
    std::snprintf(want, sizeof want, "%X(ee=%08X|rel=%+lld|w=124|v=46|d=4)", kCtr, kCtr, (long long)kCtr - 0x8000); CHECK(last.find(want) != std::string::npos);   // with its structure-relative offset
    std::snprintf(want, sizeof want, "%X(ee=%08X|rel=%+lld|w=f|v=46|d=4)", kFlt, kFlt, (long long)kFlt - 0x8000); CHECK(last.find(want) != std::string::npos);
    std::snprintf(want, sizeof want, "%X(ee=%08X|rel=%+lld|w=12|v=46|d=4)", kCtr2, kCtr2, (long long)kCtr2 - 0x8000); CHECK(last.find(want) != std::string::npos);
    std::snprintf(want, sizeof want, "%X(ee=%08X|rel=%+lld|w=1|v=46|d=4)", kU8, kU8, (long long)kU8 - 0x8000); CHECK(last.find(want) != std::string::npos);
    CHECK(last.find("list=") != std::string::npos && last.find("target=50;") != std::string::npos && last.find("groups=4;") != std::string::npos);
    for (const uint32_t dead : {kTimer, kRise, kJump, kStatic}) { char d[24]; std::snprintf(d, sizeof d, "%X(", dead); CHECK(last.find(d) == std::string::npos); }
    // quiet candidates (static 50, ASCII text) are KEPT (step168: pruning them lost the real counter); the movers stay
    t += 5000; cs.tick(fm, fm.base, sb, t, sink);
    CHECK(cs.alive() > cs.moved() && cs.moved() >= 7);
    // the summary is unconditional
    t += 11000; const unsigned sums = sink.count("store-search;event=summary;"); cs.tick(fm, fm.base, sb, t, sink); CHECK(sink.count("store-search;event=summary;") == sums + 1);
    // an unreadable chunk keeps its candidates; a lock loss keeps the state; the same block again continues; another block restarts
    fm.fail = true; t += 1000; const size_t alive = cs.alive(); cs.tick(fm, fm.base, sb, t, sink); CHECK(cs.alive() == alive); fm.fail = false;
    t += 1000; CHECK(cs.tick(fm, 0, 0, t, sink) == 0 && cs.alive() == alive);
    t += 1000; fm.set(kCtr, 45); cs.tick(fm, fm.base, sb, t, sink); CHECK(cs.alive() == alive);
    t += 1000; cs.tick(fm, fm.base + 0x40, sb + 0x40, t, sink); CHECK(cs.moved() == 0);   // restarted (re-seeded from scratch)
}
static void test_counter_search_edges() {
    namespace S = acz::e6search;
    // a counter that is initialised after the first scan (within the seed window) is still found; an HUD value other than 50 (stwatch-start:12)
    { FakeMem fm; fm.base = 0x30000000ull; fm.mem.assign(0x20000, 0); CaptureSink sink; S::CounterSearch cs; CHECK(cs.start(12, fm.mem.size(), 0x8000));
      const uint64_t sb = fm.base + 0x100; double t = 0;
      cs.tick(fm, fm.base, sb, t, sink); CHECK(cs.alive() == 0);
      fm.set(0x1230, 12); t += 5000; cs.tick(fm, fm.base, sb, t, sink); CHECK(cs.alive() >= 3);   // seeded on the second seed pass (u8 + u16 + u32)
      t += 1000; fm.set(0x1230, 11); cs.tick(fm, fm.base, sb, t, sink);
      CHECK(sink.count("v=11|d=1") == 1 && sink.count("w=124") == 1); }
    // a 2-step fall inside one sample is accepted (two quick launches); a 3-step fall and any rise kill the candidate
    { FakeMem fm; fm.base = 0x30000000ull; fm.mem.assign(0x10000, 0); CaptureSink sink; S::CounterSearch cs; CHECK(cs.start(50, fm.mem.size(), 0x4000));
      fm.set(0x100, 50); fm.set(0x200, 50); fm.set(0x300, 50); double t = 0; const uint64_t sb = fm.base;
      cs.tick(fm, fm.base, sb, t, sink); t += 1000; fm.set(0x100, 48); fm.set(0x200, 47); fm.set(0x300, 51); cs.tick(fm, fm.base, sb, t, sink);
      CHECK(sink.count("list=100(") == 1 && sink.count("v=48|d=2") == 1 && sink.count("200(") == 0 && sink.count("300(") == 0);
      t += 1000; fm.set(0x100, 49); cs.tick(fm, fm.base, sb, t, sink);
      bool has100 = false; for (const S::Cand& c : cs.candidates()) has100 |= c.off == 0x100; CHECK(!has100); }
    // the line budget: a counter that keeps counting down logs at most kMaxLines survivor lines
    { FakeMem fm; fm.base = 0x30000000ull; fm.mem.assign(0x10000, 0); CaptureSink sink; S::CounterSearch cs; CHECK(cs.start(99, fm.mem.size(), 0x4000));
      for (uint32_t c = 0; c < 8; ++c) fm.set(0x1000 + c * 4, 99);
      double t = 0; cs.tick(fm, fm.base, fm.base, t, sink);
      for (uint32_t k = 1; k <= 60; ++k) { t += 1000; fm.set(0x1000, 99 - (k % 90)); cs.tick(fm, fm.base, fm.base, t, sink); }
      CHECK(cs.logged() == S::kMaxLines && sink.count("event=survivors") == S::kMaxLines); }
    { uint32_t v = 0; const uint8_t f1[4] = {0, 0, 0x48, 0x42}; CHECK(S::decode(S::kF32, f1, v) && v == 50); const uint8_t f2[4] = {0, 0, 0x49, 0x42}; CHECK(!S::decode(S::kF32, f2, v));
      const uint8_t u[4] = {0, 0, 1, 0}; CHECK(!S::decode(S::kU32, u, v)); const uint8_t nan4[4] = {0xFF, 0xFF, 0xFF, 0x7F}; CHECK(!S::decode(S::kF32, nan4, v)); }
}
// step168 regression: five u8 at stride 0x98 fell 50 -> 49 117 s before the first missile; the 4 s grace prune then dropped the real (quiet) counter, and the
// summary lines went to ee-camera.log only. The real counter must still be caught 50 -> 46 two minutes later, with summaries all along and the table tracked.
static void test_counter_search_step168() {
    namespace S = acz::e6search;
    FakeMem fm; fm.base = 0x24960054d10ull; fm.mem.assign(0x20000, 0); CaptureSink sink; S::CounterSearch cs; CHECK(cs.start(50, fm.mem.size(), 0x8000));
    const uint32_t kReal = 0x1230, kTab = 0x4000, kStride = 0x98; const uint64_t sb = fm.base + 0x10000; double t = 0;
    fm.set(kReal, 50); for (int i = 0; i < 5; ++i) fm.mem[kTab + i * kStride] = 50;
    cs.tick(fm, fm.base, sb, t, sink);
    t += 7000; for (int i = 0; i < 5; ++i) fm.mem[kTab + i * kStride] = 49;          // the early table decrement
    cs.tick(fm, fm.base, sb, t, sink);
    CHECK(cs.moved() >= 5 && sink.count("event=survivors") == 1 && sink.count("event=track") == 1 && sink.count("4000=49,4098=49,4130=49,41C8=49,4260=49") == 1);   // table tracked from its first move
    t += 5000; for (int i = 0; i < 5; ++i) fm.mem[kTab + i * kStride] = 7;          // the table wanders off: its candidates die
    cs.tick(fm, fm.base, sb, t, sink); CHECK(sink.count("4000=7,") == 1);
    unsigned sums = 0;
    for (int k = 0; k < 110; ++k) { t += 1000; cs.tick(fm, fm.base, sb, t, sink); }   // 110 quiet seconds (the real counter has not moved yet)
    sums = sink.count("store-search;event=summary;"); CHECK(sums >= 10 && cs.alive() >= 3);   // a summary every 10 s all the way, the quiet counter still a candidate
    const uint32_t seq[5] = {50, 49, 48, 47, 46};
    for (int k = 1; k <= 4; ++k) { t += 7000; fm.set(kReal, seq[k]); cs.tick(fm, fm.base, sb, t, sink); }
    std::string last; for (const auto& l : sink.lines) if (l.find("event=survivors") != std::string::npos) last = l;
    CHECK(last.find("1230(ee=00001230|rel=-60880|w=124|v=46|d=4)") != std::string::npos);
    // an explicit track list is logged on change and at least every 10 s; unreadable offsets log -1
    { FakeMem f2; f2.base = fm.base; f2.mem.assign(0x20000, 0); CaptureSink s2; S::CounterSearch c2; CHECK(c2.start(50, f2.mem.size(), 0x8000));
      const uint32_t offs[3] = {0x100, 0x101, 0x30000}; c2.set_track(offs, 3); CHECK(c2.tracked() == 3);
      double t2 = 0; c2.tick(f2, f2.base, f2.base, t2, s2); CHECK(s2.count("event=track") == 1 && s2.count("list=100=0,101=0,30000=-1") == 1);
      t2 += 1000; c2.tick(f2, f2.base, f2.base, t2, s2); CHECK(s2.count("event=track") == 1);                  // unchanged: quiet
      t2 += 1000; f2.mem[0x101] = 9; c2.tick(f2, f2.base, f2.base, t2, s2); CHECK(s2.count("101=9") == 1);       // change: logged
      t2 += 10000; c2.tick(f2, f2.base, f2.base, t2, s2); CHECK(s2.count("event=track") == 3); }                // keep-alive after 10 s
    namespace SS = acz::e6search; SS::Cfg c = SS::parse_cfg("auto;stwatch:1;sttrack:67D886,67d91e;vrn:1");
    CHECK(!c.bad && c.ntrack == 2 && c.track[0] == 0x67D886 && c.track[1] == 0x67D91E);
    CHECK(SS::parse_cfg("m;sttrack:zz").bad && SS::parse_cfg("m;sttrack:1,2,3,4,5,6,7,8,9").bad && SS::parse_cfg("m;sttrack:4000000").bad);
    std::string why, id; Options o; CHECK(parse_options("m;sttrack:67D886,67D91E", id, o, why));
    for (const char* b : {"m;sttrack:", "m;sttrack:zz", "m;sttrack:1,,2", "m;sttrack:4000000", "m;sttrack:1,2,3,4,5,6,7,8,9"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
}
static void test_launch_detector() {
    namespace S = acz::e6search;
    S::LaunchDetector d; double t = 0;
    auto feed = [&](uint32_t v, int n) { S::LaunchEvent sum; for (int i = 0; i < n; ++i) { t += 100; const S::LaunchEvent e = d.update(true, v, t); sum.launches += e.launches; sum.rearm |= e.rearm; } return sum; };
    CHECK(feed(50, 1).launches == 0 && !d.has_baseline());                // one read is no baseline (needs kStableReads and kStableMs)
    CHECK(feed(50, 5).launches == 0 && d.has_baseline() && d.baseline() == 50);
    CHECK(feed(49, 1).launches == 0 && feed(50, 3).launches == 0);        // a one-read glitch changes nothing (hysteresis)
    CHECK(d.baseline() == 50 && feed(49, 2).launches == 0);               // 2 reads but < 300 ms
    CHECK(feed(49, 5).launches == 1 && d.baseline() == 49);               // a stable fall = one launch
    CHECK(feed(46, 6).launches == 3 && d.baseline() == 46);               // two taps inside one stable step = three launches
    CHECK(feed(46, 10).launches == 0);                                    // unchanged: nothing
    { const S::LaunchEvent e = d.update(false, 0, t += 100); CHECK(e.launches == 0 && !e.rearm); }   // unreadable tick: nothing
    CHECK(feed(20, 6).launches == 0 && d.baseline() == 20);               // a fall bigger than kMaxLaunches is a reload, not launches
    { const S::LaunchEvent e = feed(50, 6); CHECK(e.rearm && e.launches == 0 && d.baseline() == 50); }   // a stable rise = rearm
    d.reset(); CHECK(!d.has_baseline() && feed(30, 6).launches == 0 && d.baseline() == 30);   // a new block restarts the baseline
}
static void test_counter_cfg() {
    namespace S = acz::e6search;
    S::Cfg c = S::parse_cfg("auto;stwatch:1");
    CHECK(c.target == 50 && c.ctr_off == 0 && c.wire && !c.bad);
    c = S::parse_cfg("auto;stwatch:1;stwatch-start:46;stctr:1a2b3c,2;vrn:1");
    CHECK(c.target == 46 && c.ctr_off == 0x1A2B3C && c.ctr_w == 2 && c.wire && !c.bad);
    CHECK(S::parse_cfg("m;stores:0;stctr:100,1").wire == false && S::parse_cfg("m;stctr=100,4").ctr_off == 0x100);
    for (const char* b : {"m;stwatch-start:0", "m;stwatch-start:x", "m;stwatch-start:10000", "m;stctr:0,4", "m;stctr:100,3", "m;stctr:100", "m;stctr:4000000,4", "m;stctr:zz,4"}) CHECK(S::parse_cfg(b).bad);
    CHECK(!S::parse_cfg(nullptr).bad && S::parse_cfg(nullptr).target == 50);
    // review 2026-11-01 F6: ONE launch source for the stores mailbox. MSL-HUD (default on) and a configured stctr both called note_launch, so each
    // missile counted twice. Failing before: the probe wired stctr whenever `wire && !bad && ctr_off` (here: true with MSL-HUD on by default).
    {
        const S::Cfg both = S::parse_cfg("auto;stctr:1a2b3c,2;vrn:1");   // mslhud not given: on (hud_msl.h parse_mode default)
        CHECK(both.wire && !both.bad && both.ctr_off && both.msl_hud && !both.drives() && both.yields());
        const S::Cfg ctr = S::parse_cfg("auto;stctr:1a2b3c,2;mslhud:0");
        CHECK(!ctr.msl_hud && ctr.drives() && !ctr.yields());
        CHECK(!S::parse_cfg("auto;stctr:1a2b3c,2;mslhud:2").drives() && !S::parse_cfg("auto;mslhud:0").drives());
        CHECK(!S::parse_cfg("auto;stores:0;stctr:1a2b3c,2;mslhud:0").drives() && !S::parse_cfg("auto;stores:0;stctr:1a2b3c,2").yields());
    }
    // the model-option parser accepts / refuses the same values (a bad value refuses the model like every option)
    std::string why, id; Options o;
    CHECK(parse_options("m;stwatch:1;stwatch-start:46;stctr:1a2b3c,2", id, o, why) && o.stwatch_start == 46 && o.stctr_off == 0x1A2B3C && o.stctr_width == 2);
    CHECK(parse_options("m", id, o, why) && o.stwatch_start == 50 && o.stctr_off == 0);
    for (const char* b : {"m;stwatch-start:0", "m;stwatch-start:1.5", "m;stctr:100,3", "m;stctr:0,4", "m;stctr:100", "m;stctr:100,44", "m;stctr:"}) { Options x; std::string i2, w2; CHECK(!parse_options(b, i2, x, w2)); }
}

int main(int argc, char** argv) {    if (argc >= 3 && std::strcmp(argv[1], "--load") == 0) {
        for (int a = 3; a < argc; ++a) {
            if (std::strcmp(argv[a], "--png") == 0 && a + 1 < argc) {
                g_png = argv[++a];
                for (int i = 0; i < 3 && a + 1 < argc; ++i) {   // up to 3 numeric angles (negative allowed)
                    char* e = nullptr; const double v = std::strtod(argv[a + 1], &e);
                    if (e == argv[a + 1] || *e) break;
                    g_ypr[i] = float(v); ++a;
                }
            } else if (std::strcmp(argv[a], "--tan") == 0 && a + 4 < argc) { for (int i = 0; i < 4; ++i) g_tan[i] = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--size") == 0 && a + 2 < argc) { g_w = uint32_t(std::atoi(argv[++a])); g_h = uint32_t(std::atoi(argv[++a])); }
            else if (std::strcmp(argv[a], "--opts") == 0 && a + 1 < argc) g_opts = argv[++a];
            else if (std::strcmp(argv[a], "--light") == 0 && a + 3 < argc) { g_light_set = true; for (int i = 0; i < 3; ++i) g_light[i] = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--tint") == 0 && a + 3 < argc) { g_tint_set = true; for (int i = 0; i < 3; ++i) g_tint[i] = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--ypr") == 0 && a + 3 < argc) { for (int i = 0; i < 3; ++i) g_ypr[i] = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--anim") == 0 && a + 4 < argc) { g_anim_set = true; g_anim.pitch = float(std::atof(argv[++a])); g_anim.roll = float(std::atof(argv[++a]));
                g_anim.yaw = float(std::atof(argv[++a])); g_anim.throttle = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--eyeoff") == 0 && a + 3 < argc) { for (int i = 0; i < 3; ++i) g_eye_off[i] = float(std::atof(argv[++a])); }
            else if (std::strcmp(argv[a], "--dump") == 0 && a + 1 < argc) g_dump = argv[++a];
        }
        if (g_w < 16 || g_h < 16 || g_w > 4096 || g_h > 4096) { std::printf("bad --size\n"); return 1; }
        return load_main(argv[2]);
    }
    test_roundtrip();
    test_loader_errors();
    test_files();
    test_options();
    test_mesh();
    test_legacy_clut();
    test_shading();
    test_camera();
    test_cscale();              // COCKPIT-SCALE
    test_reference_landmarks();
    test_config_key();
    test_c4_options_loader();   // COCKPIT-4
    test_c4_mesh();
    test_c4_glass();
    test_c4_anim();
    test_clip();                // COCKPIT-CLIP
    test_skin_refl();           // SKIN-REFL
    test_cfid();                // CFID
    test_seat();                // SEAT-CLEARANCE
    test_stores();              // STORES-RT
    test_store_tracker();
    test_sp_stores();
    test_store_watch();
    test_counter_search();      // STORES-RT wide search
    test_counter_search_edges();
    test_counter_search_step168();
    test_launch_detector();
    test_counter_cfg();
    std::printf("cockpit_model_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
