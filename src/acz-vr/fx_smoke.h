#pragma once
// fx_smoke.h -- EFFECTS-OUTSIDE (C) missile smoke outside the game window (docs/world-render/effects-outside-window.md, section 8). SDK-free, header only.
//
// Evidence (live run Z1, D-352: the smoke pipeline's three shaders dumped at PSO creation; `dxc -dumpbin` of the DXIL containers):
//  * VS 9eff1f92 = EffDrawMissileSmoke_v (debug name): forwards SV_VertexID as PASSINDEX, nothing else (no vertex buffer).
//  * GS bcb04cf9 = EffDrawMissileSmoke_g: InputPrimitive=point, OutputTopology=triangle, max 64 vertices. Reads StructuredBuffer<SrtData> t10 element 0
//    (1588-byte record), StructuredBuffer<float4> t11 (a random table, indices 0..126), cb10 and cb11 (8 bytes each = 2 floats). One input point = ONE
//    TRAIL SEGMENT vid between the record's world-space points vid and vid+1 (20 points at +944, 32 bytes each). It ages both points, clips the segment
//    against the record's CLIP matrix (+64, test -|w| <= x, y, z <= |w|), projects the clipped endpoints with the record's WORLD -> PS2-SCREEN matrix (+0, the
//    same A the DrawACM_v records carry), subdivides the screen segment into 1..16 puffs and emits each puff as a 4-vertex diamond strip
//    (offsets (-1,0), (0,-1), (0,1), (1,0) x the puff's screen half size). SV_Position = ((X - cb10.x) * 2 / cb11.x, (Y - cb10.y) * 2 / cb11.y, z_s * 2^-24, 1).
//  * PS d2464ed2 = EffDrawMissileSmoke_p: texture t0 x COLOR, discard alpha <= 0, SV_Target1 = (1,1,1, cb12.x >= alpha ? 0 : 1) (dual-source output).
// Consequence: the smoke positions are WORLD points in a per-draw record, and the culling to the game window happens ON THE GPU (the clip matrix). Replaying
// the draw with +0 and +64 replaced by an eye's matrices draws the trail for that eye in every direction (route S2). Whether the game also culls trail points
// on the CPU before filling the record is NOT known (the summary line below measures it live).
//
// This header: the record layout, a validity check, the eye substitution (+0 = the eye's A, +64 = the clip matrix derived from it), a CPU emulation of the
// GS (for the offline tests; exact except the GPU's sin() in the random-index hash), the bounded per-frame capture store and the live summary line.
// Live wiring (fxmem:1 only, steps 1-2 = capture + log, NO replay): gpu_copy_step5_hooks.inl step5FxSmokeCapture / step5FxSmokeAccount (one FrameCapture
// in the producer state, reset per effects-armed frame), the roots from find_root_constants / root_pair, the line rate-limited by LogGate.
// Replay core (section 8.6, D-361 facts): the per-eye record preparation from the world frame's D_e x S (eye_a_from_delta, prepare_eye_records), the bounded
// per-slot copy store (ReplayStore), the root-SRV swap plan and the replay command sequence (plan_root_swap, replay_commands), and the fxmem:2 DRY RUN
// (ReplayDryRun: every captured draw is prepared for both eyes on the CPU and its trail points counted per eye; nothing is recorded on the GPU).
//
// Re-read of the GS DXIL after run Q (D-361; `dxc -dumpbin` of bcb04cf9...dxbc):
//  * cb10 / cb11: handle %4 = CBV range 0 = b10 is the DIVISOR and handle %3 = range 1 = b11 is the SUBTRACTED centre:
//    SV_Position.xy = (X - cb11.xy) * 2 / cb10.xy. Live: cb10 = (512, -448), cb11 = (2048, 2048) = the DrawACM_v +400 constants. (Section 8.1 had the names
//    swapped; the arithmetic of the emulation was right for those values.)
//  * +144: .x / .y multiply the puff size (curve / w) into the diamond's screen half extents (%539 / %540); .z / .w multiply the screen segment's dx / dy for
//    the subdivision length (%469 / %470). Live: |.x| / fx(A) = |.y| / fy(A) = 0.0019 and .z x .x = .w x .y = 1.0000 on every logged frame. So
//    +144 = (g fx, g fy, 1 / (g fx), 1 / (g fy)): A's screen focal lengths times a world-size gain g (~0.0019), NOT the focal lengths themselves. A puff's world
//    half size is curve(+304/+336) / 255 x +160.y x g whatever the lens; for an eye with focal lengths (ex, ey) the field must be (g ex, g ey, 1/(g ex), 1/(g ey)),
//    which is what `substitute` with rescale_lens writes (it scales by ex / fx: g cancels). The subdivision count is then the game's for the same geometry.
//  * Clip / expand: the segment (points vid+1 -> vid after ageing) is trivially rejected when both ends are outside one plane of -|w| <= x, y, z <= |w| of
//    [p,1] x M(+64), otherwise clipped parametrically (signed test c * s > w, plane order +x -x +y -y +z -z); the clipped ends are projected with M(+0) (h / h.w);
//    the GS never clips the diamonds themselves: the rasteriser does (SV_Position.w = 1, so only the viewport / depth clip of SV z = z_s 2^-24 applies).
//  * Alpha: curve(+368/+400) / 255 x +160.x x (+128.x + +128.y w), then if NOT (+192.x < w): x (w - +192.y) x +192.z; dropped below 1 (both ends) or when
//    both sizes < 1/16. Colour = int4 corners (+224) / 128 with alpha / 128; uv from +288 and the 32-entry table +432 indexed by the t11 random table.
#include "fx_memory.h"   // kZ0, kDepthB, kGsCentre, kGsHalfW, kGsHalfH

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace acz::fxsmoke {

// ---- SrtData record layout (bytes; DXIL hostlayout.struct.SrtData, 1588 bytes) [E: GS disassembly]
inline constexpr unsigned kRecordBytes = 1588;
inline constexpr unsigned kOffA = 0;            // float4x4 world -> PS2 screen (row vectors, h = [P,1] A): puff centres
inline constexpr unsigned kOffClip = 64;        // float4x4 world -> clip (row vectors): segment clip -|w| <= x, y, z <= |w|
inline constexpr unsigned kOffAlphaDist = 128;  // alpha *= (.x + .y * w)
inline constexpr unsigned kOffLens = 144;       // (g fx, g fy, 1/(g fx), 1/(g fy)): .x/.y puff half extents per unit size, .z/.w the subdivision dx/dy scale
inline constexpr unsigned kOffScale = 160;      // .x alpha curve scale, .y size curve scale, .z subdivision size factor, .w seed offset
inline constexpr unsigned kOffAge = 176;        // age = sat(((n * .y + .x) * .z) * (1 + 0.2 r) + .w)
inline constexpr unsigned kOffNearFade = 192;   // w <= .x: alpha *= (w - .y) * .z
inline constexpr unsigned kOffInt208 = 208;     // int4, not read by the GS
inline constexpr unsigned kOffColour = 224;     // int4[4] corner colours (x 1/128)
inline constexpr unsigned kOffUv = 288;         // .x, .y texel base (-8), .z uv scale, .w random u column scale
inline constexpr unsigned kOffCurveBKeys = 304, kOffCurveBVals = 336;   // size curve: 8 keys (age * 255), 8 values (/255)
inline constexpr unsigned kOffCurveAKeys = 368, kOffCurveAVals = 400;   // alpha curve
inline constexpr unsigned kOffUvTable = 432;    // float4[32]
inline constexpr unsigned kOffPoints = 944;     // 20 x { float4 (pos.xyz, -), float4 (drift.xyz, seed) }
inline constexpr unsigned kPointStride = 32, kMaxPoints = 20, kMaxSegments = kMaxPoints - 1;
inline constexpr unsigned kRandomEntries = 128; // t11 float4 entries (the hash reaches 0..126)
inline constexpr double kDepthUnit = 1.0 / 16777216.0;   // SV_Position.z = z_s * 2^-24
inline constexpr unsigned kSmokePs8 = 0xd2464ed2u, kSmokeVs8 = 0x9eff1f92u;
static_assert(kOffPoints + kMaxPoints * kPointStride + 4 == kRecordBytes, "SrtData: points end at +1584, one int follows");

inline float rf(const uint8_t* r, unsigned off) noexcept { float f; std::memcpy(&f, r + off, 4); return f; }
inline int32_t ri(const uint8_t* r, unsigned off) noexcept { int32_t v; std::memcpy(&v, r + off, 4); return v; }
inline void wf(uint8_t* r, unsigned off, float f) noexcept { std::memcpy(r + off, &f, 4); }
inline void load_m(const uint8_t* r, unsigned off, double m[16]) noexcept { for (unsigned i = 0; i < 16; ++i) m[i] = rf(r, off + 4 * i); }
// h = [p,1] M, M row-major (rows 0..2 = x, y, z coefficients, row 3 = translation), as the GS reads it.
inline void xform(const double m[16], const double p[3], double h[4]) noexcept {
    for (int c = 0; c < 4; ++c) h[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
}

// Screen focal lengths of an A-form matrix (GS units per unit of lateral offset at view depth 1): |(col0 - 2048 col3).xyz| / |col3.xyz|, same for col1.
inline bool screen_focal(const double a[16], double& fx, double& fy) noexcept {
    double nx = 0, ny = 0, nw = 0;
    for (int r = 0; r < 3; ++r) {
        const double w = a[r * 4 + 3], x = a[r * 4 + 0] - acz::fxmem::kGsCentre * w, y = a[r * 4 + 1] - acz::fxmem::kGsCentre * w;
        nx += x * x; ny += y * y; nw += w * w;
    }
    if (!(nw > 0)) return false;
    fx = std::sqrt(nx / nw); fy = std::sqrt(ny / nw);
    return std::isfinite(fx) && std::isfinite(fy) && fx > 0 && fy > 0;
}
// A's z column is Z0 e3 + b A3 (the validated PS2 depth model, gpu_copy_step5_world.h): rows 0..2 of column 2 equal b x column 3 within 1e-4 relative.
// The model is PER VIEW (headset test 7, effects-outside-window.md 22.3): cockpit b = -255.01 / Z0 = 2^24, chase and HUD-only b ~ -1,341..-1,475 / Z0 ~ 8.8e7..9.7e7.
// b is solved from A (least squares over the w column) and must be a plausible model (fxmem::depth_model_ok); `model` returns it (nullptr = not wanted).
inline bool depth_column_ok(const double a[16], double* b_out = nullptr, double* z0_out = nullptr) noexcept {
    double bs = 0, ws = 0;
    for (int r = 0; r < 3; ++r) { bs += a[r * 4 + 2] * a[r * 4 + 3]; ws += a[r * 4 + 3] * a[r * 4 + 3]; }
    if (!(ws > 1e-30)) return false;
    const double b = bs / ws;
    double res = 0, mag = 0;
    for (int r = 0; r < 3; ++r) { const double z = a[r * 4 + 2], d = z - b * a[r * 4 + 3]; res += d * d; mag += z * z; }
    const double z0 = a[14] - b * a[15];
    if (!(mag > 0 && std::sqrt(res) <= 1e-4 * std::sqrt(mag) && acz::fxmem::depth_model_ok(b, z0))) return false;
    if (b_out) *b_out = b;
    if (z0_out) *z0_out = z0;
    return true;
}
// The clip matrix the GS needs for an eye whose screen matrix is `a` (unscaled depth column): x_c = (h.x - 2048 h.w) / (256 g), y_c = (h.y - 2048 h.w) / (224 g),
// z_c = 2 h.z 2^-24 - h.w, w_c = h.w. Inside (-|w| <= x, y, z <= |w|) <=> the image (grown by the guard factor g) and 0 <= z_s 2^-24 <= 1 (view depth ~1 ..
// 65,793 units); a point behind the eye has z_c > |w| and is clipped like the game's near plane. Independent of the game's own clip convention (the GS test
// is fixed in the shader).
inline void clip_from_a(const double a[16], double guard, double c[16]) noexcept {
    for (int r = 0; r < 4; ++r) {
        const double w = a[r * 4 + 3];
        c[r * 4 + 0] = (a[r * 4 + 0] - acz::fxmem::kGsCentre * w) / (acz::fxmem::kGsHalfW * guard);
        c[r * 4 + 1] = (a[r * 4 + 1] - acz::fxmem::kGsCentre * w) / (acz::fxmem::kGsHalfH * guard);
        c[r * 4 + 2] = 2.0 * a[r * 4 + 2] * kDepthUnit - w;
        c[r * 4 + 3] = w;
    }
}

// ---- record check
struct DrawArgs { uint32_t vertex_count = 0, instance_count = 1, start_vertex = 0, start_instance = 0; };
inline bool finite_range(const uint8_t* r, unsigned off, unsigned bytes, float lim) noexcept {
    for (unsigned o = off; o < off + bytes; o += 4) { const float f = rf(r, o); if (!std::isfinite(f) || std::fabs(f) > lim) return false; }
    return true;
}
// nullptr = replayable: the draw is one instance of <= 19 segments, A is of the PS2 screen form, everything the GS reads for the used points is finite.
inline const char* record_check(const uint8_t* raw, size_t bytes, const DrawArgs& d) noexcept {
    if (!raw || bytes < kRecordBytes) return "smoke-record-size";
    if (d.instance_count != 1 || d.start_instance != 0) return "smoke-draw-instanced";
    if (d.vertex_count == 0 || uint64_t(d.start_vertex) + d.vertex_count > kMaxSegments) return "smoke-draw-segments";
    if (!finite_range(raw, kOffA, 128, 1e12f)) return "smoke-record-nonfinite";
    double a[16]; load_m(raw, kOffA, a);
    if (!depth_column_ok(a)) return "smoke-a-depth-column";
    double fx = 0, fy = 0;
    if (!screen_focal(a, fx, fy)) return "smoke-a-degenerate";
    // floats +128..+207 (+208 is an int4 the GS never reads, +224 the int colours) and +288..+943
    if (!finite_range(raw, kOffAlphaDist, kOffInt208 - kOffAlphaDist, 1e30f) || !finite_range(raw, kOffUv, kOffPoints - kOffUv, 1e30f))
        return "smoke-params-nonfinite";
    const unsigned first = d.start_vertex, last = d.start_vertex + d.vertex_count;   // points first .. last inclusive
    for (unsigned n = first; n <= last; ++n) if (!finite_range(raw, kOffPoints + n * kPointStride, kPointStride, 1e12f)) return "smoke-point-nonfinite";
    return nullptr;
}

// ---- lens field (+144) [E: DXIL + live run Q, see the header: (g fx, g fy, 1/(g fx), 1/(g fy)) with a world-size gain g ~ 0.0019]
// The puff screen half size is size x lens.x with size = curve / w, i.e. a world half size of curve x g whatever the lens. An eye with another lens needs the
// field rescaled by its focal lengths over A's; that is applied only when the record proves the FORM: .x / fx and .y / fy agree (one common gain g > 0)
// within 2%, and .z x .x and .w x .y are 1 within 2%. (Before D-361 the rule also required g = 1, which the game never has: rx = ry = 0.0019 live.)
struct LensFit { double fx = 0, fy = 0, rx = 0, ry = 0, rzx = 0, rwy = 0, gain = 0; bool proven = false; };
inline LensFit lens_fit(const uint8_t* raw) noexcept {
    LensFit L; double a[16]; load_m(raw, kOffA, a);
    if (!screen_focal(a, L.fx, L.fy)) return L;
    const double lx = rf(raw, kOffLens), ly = rf(raw, kOffLens + 4), lz = rf(raw, kOffLens + 8), lw = rf(raw, kOffLens + 12);
    L.rx = std::fabs(lx) / L.fx; L.ry = std::fabs(ly) / L.fy; L.rzx = lz * lx; L.rwy = lw * ly;
    auto near1 = [](double v) { return std::isfinite(v) && std::fabs(v - 1.0) <= 0.02; };
    const bool gainOk = std::isfinite(L.rx) && std::isfinite(L.ry) && L.rx > 1e-9 && L.ry > 1e-9 && L.rx < 1e6 && L.ry < 1e6 && near1(L.rx / L.ry);
    L.gain = gainOk ? 0.5 * (L.rx + L.ry) : 0.0;
    L.proven = gainOk && lx > 0 && near1(L.rzx) && near1(L.rwy);
    return L;
}

// ---- eye substitution (the replayed COPY only; the game's record is never written)
struct SubstCfg {
    double guard = 1.25;        // clip guard band: puffs centred up to 25% beyond the image edge are still emitted (the rasteriser clips them)
    bool rescale_lens = false;  // rescale +144 to the eye's focal lengths (refused unless lens_fit proves the field)
    float depth_scale = 1.0f;   // the objects pass's depth scale (xr_world_objects_depth_fix: 2^-10): applied to A's z column only, never to the clip
};
// out = raw with +0 = a_eye (z column x depth_scale) and +64 = clip_from_a(a_eye, guard); +144 rescaled when asked. `a_eye` = the eye's world -> PS2-screen
// matrix of the objects replay (world::substitute's A + D_e x S for that eye). nullptr or why (out undefined on failure).
inline const char* substitute(const uint8_t* raw, const double a_eye[16], const SubstCfg& cfg, uint8_t* out) noexcept {
    if (!raw || !out || !a_eye) return "smoke-subst-null";
    if (!(cfg.guard >= 1.0 && cfg.guard <= 4.0)) return "smoke-subst-guard";
    if (!(cfg.depth_scale > 0.0f) || !std::isfinite(cfg.depth_scale)) return "smoke-subst-depth-scale";
    for (int i = 0; i < 16; ++i) if (!std::isfinite(a_eye[i])) return "smoke-subst-nonfinite";
    if (!depth_column_ok(a_eye)) return "smoke-subst-depth-column";
    double ex = 0, ey = 0;
    if (!screen_focal(a_eye, ex, ey)) return "smoke-subst-degenerate";
    LensFit L;
    if (cfg.rescale_lens) { L = lens_fit(raw); if (!L.proven) return "smoke-lens-field-unproven"; }
    double c[16]; clip_from_a(a_eye, cfg.guard, c);
    std::memcpy(out, raw, kRecordBytes);
    for (unsigned i = 0; i < 16; ++i) {
        const double av = (i % 4 == 2 && cfg.depth_scale != 1.0f) ? a_eye[i] * double(cfg.depth_scale) : a_eye[i];
        const float af = float(av), cf = float(c[i]);
        if (!std::isfinite(af) || !std::isfinite(cf)) return "smoke-subst-nonfinite";
        wf(out, kOffA + 4 * i, af); wf(out, kOffClip + 4 * i, cf);
    }
    if (cfg.rescale_lens) {
        const double sx = ex / L.fx, sy = ey / L.fy;
        wf(out, kOffLens, float(double(rf(raw, kOffLens)) * sx)); wf(out, kOffLens + 4, float(double(rf(raw, kOffLens + 4)) * sy));
        wf(out, kOffLens + 8, float(double(rf(raw, kOffLens + 8)) / sx)); wf(out, kOffLens + 12, float(double(rf(raw, kOffLens + 12)) / sy));
    }
    return nullptr;
}

// ---- CPU emulation of EffDrawMissileSmoke_g (offline tests; mirrors the DXIL step by step)
struct RandomTable { float v[kRandomEntries][4] = {}; };
// SV_Position.xy = (X - cb11.xy) * 2 / cb10.xy [E: DXIL handles, D-361]; the defaults are the live run Q values (= the DrawACM_v +400 constants).
struct ScreenConsts { float cb10[2] = {512.0f, -448.0f}; float cb11[2] = {2048.0f, 2048.0f}; };
// The replay reuses the game's own cb10/cb11 (root constants, never rebound): the eye matrices map the eye's frustum onto the GS screen 2048 +- 256 / 224,
// which is the game window only when the constants are exactly these. roots6 = cb10, cb11, cb12 (as FrameCapture stores them). nullptr = replayable.
inline const char* screen_consts_check(const float* roots6) noexcept {
    if (!roots6) return "smoke-roots-unknown";
    const ScreenConsts k;
    if (roots6[0] != k.cb10[0] || roots6[1] != k.cb10[1] || roots6[2] != k.cb11[0] || roots6[3] != k.cb11[1]) return "smoke-screen-consts";
    return nullptr;
}
// frac(sin(dot((s,0), (12.9898, 78.233))) * 43758.5453) * 127, truncated. The GPU's sin differs from the CPU's at large arguments: tests use tables whose
// entries are all equal so the result does not depend on the index.
inline unsigned hash_index(double seed) noexcept {
    const float s = std::sin(float(seed) * 12.9898f) * 43758.5453f;
    const float fr = s - std::floor(s);
    int i = int(fr * 127.0f);
    if (i < 0) i = 0;
    if (i > int(kRandomEntries) - 1) i = int(kRandomEntries) - 1;
    return unsigned(i);
}
// Piecewise-linear curve over 8 (key, value) pairs; key = age * 255. Below key0: value0; above key6: extrapolates the last piece (as the shader does).
inline double curve(const uint8_t* r, unsigned keys, unsigned vals, double a) noexcept {
    int j = -1;
    if (a > rf(r, keys)) { j = 0; while (j < 6 && a > rf(r, keys + 4u * unsigned(j + 1))) ++j; }
    if (j < 0) return rf(r, vals);
    const double k0 = rf(r, keys + 4u * unsigned(j)), k1 = rf(r, keys + 4u * unsigned(j + 1));
    const double v0 = rf(r, vals + 4u * unsigned(j)), v1 = rf(r, vals + 4u * unsigned(j + 1));
    return v0 + (v1 - v0) * (a - k0) / (k1 - k0);
}
// Point n after ageing: world position (pos + age x drift), its seed and age.
inline void point_state(const uint8_t* r, const RandomTable& rnd, unsigned n, double pos[3], double& seed, double& age) noexcept {
    const unsigned b = kOffPoints + n * kPointStride;
    const double t = (double(n) * rf(r, kOffAge + 4) + rf(r, kOffAge)) * rf(r, kOffAge + 8);
    seed = double(rf(r, b + 28)) * 53.0 + rf(r, kOffScale + 12);
    const double rr = rnd.v[hash_index(seed)][0];
    age = t * (1.0 + 0.2 * rr) + rf(r, kOffAge + 12);
    age = age < 0 ? 0 : age > 1 ? 1 : age;
    for (int k = 0; k < 3; ++k) pos[k] = double(rf(r, b + 4 * unsigned(k))) + age * double(rf(r, b + 16 + 4 * unsigned(k)));
}
struct Puff {
    unsigned segment = 0, sub = 0, subs = 0;
    double u = 0;               // position along the segment (0 = point `segment`, 1 = point `segment`+1)
    double screen[3] = {};      // puff centre: GS x, y and z_s
    double size = 0, alpha = 0; // size = size curve / w; alpha in 0..255 (before the /128 of the output colour)
    double half[2] = {};        // screen half extents (GS units)
    float sv[4][4] = {};        // SV_Position of the 4 strip vertices
    float colour[4][4] = {};
    float uv[4][2] = {};
};
// (`too_small`, not `small`: rpcndr.h, pulled in by <windows.h> before this header in d3d12_hook.cpp, defines `small` as a macro.)
struct GsStats { unsigned segments = 0, culled_outside = 0, faint = 0, too_small = 0, out_of_range = 0, alpha_skip = 0, puffs = 0; };
inline void emulate_segment(const uint8_t* r, const RandomTable& rnd, const ScreenConsts& sc, unsigned vid, std::vector<Puff>& out, GsStats& st) {
    ++st.segments;
    double A[16], C[16]; load_m(r, kOffA, A); load_m(r, kOffClip, C);
    double P[2][3], seed[2], age[2], al0[2], sz0[2], cl[2][4];
    double par[2] = {1.0, 0.0};   // endpoint 0 = point vid+1 (param 1), endpoint 1 = point vid (param 0)
    for (int e = 0; e < 2; ++e) {
        const unsigned n = e == 0 ? vid + 1 : vid;
        point_state(r, rnd, n, P[e], seed[e], age[e]);
        const double a255 = age[e] * 255.0;
        al0[e] = curve(r, kOffCurveAKeys, kOffCurveAVals, a255) / 255.0 * rf(r, kOffScale);
        sz0[e] = curve(r, kOffCurveBKeys, kOffCurveBVals, a255) / 255.0 * rf(r, kOffScale + 4);
        xform(C, P[e], cl[e]);
    }
    for (int p = 0; p < 6; ++p) {   // trivial reject: both endpoints outside the same plane
        const int k = p / 2;
        auto outside = [&](int e) { const double w = std::fabs(cl[e][3]); return (p % 2 == 0) ? cl[e][k] < -w : w < cl[e][k]; };
        if (outside(0) && outside(1)) { ++st.culled_outside; return; }
    }
    for (int i = 0; i < 6; ++i) {   // clip: planes +x, -x, +y, -y, +z, -z (s * comp > w)
        const double s = (i % 2 == 0) ? 1.0 : -1.0; const int k = i / 2;
        int o = -1, in = -1;
        if (cl[0][k] * s > cl[0][3]) { o = 0; in = 1; } else if (cl[1][k] * s > cl[1][3]) { o = 1; in = 0; } else continue;
        const double din = cl[in][k] - s * cl[in][3], dout = cl[o][k] - s * cl[o][3];
        const double t = std::fabs(din / (dout - din));
        for (int q = 0; q < 3; ++q) P[o][q] = P[in][q] + (P[o][q] - P[in][q]) * t;
        par[o] = par[in] + (par[o] - par[in]) * t;
        xform(C, P[o], cl[o]);
    }
    double S[2][3], W[2];
    for (int e = 0; e < 2; ++e) { double h[4]; xform(A, P[e], h); W[e] = h[3]; for (int k = 0; k < 3; ++k) S[e][k] = h[k] / h[3]; }
    const double inv = 1.0 / (par[0] - par[1]);
    double al[2], sz[2];
    for (int e = 0; e < 2; ++e) {
        al[e] = al0[e] * (W[e] * rf(r, kOffAlphaDist + 4) + rf(r, kOffAlphaDist));
        sz[e] = sz0[e] / W[e];
        if (!(rf(r, kOffNearFade) < W[e])) al[e] = (W[e] - rf(r, kOffNearFade + 4)) * rf(r, kOffNearFade + 8) * al[e];
        al[e] = al[e] > 0 ? al[e] : 0;
    }
    if (al[1] < 1.0 && al[0] < 1.0) { ++st.faint; return; }
    if (sz[1] < 0.0625 && sz[0] < 0.0625) { ++st.too_small; return; }
    double d[3], slope[3], base[3];
    for (int k = 0; k < 3; ++k) { d[k] = S[0][k] - S[1][k]; slope[k] = d[k] * inv; base[k] = S[1][k] - slope[k] * par[1]; }
    const double lx = rf(r, kOffLens), ly = rf(r, kOffLens + 4), lz = rf(r, kOffLens + 8), lw = rf(r, kOffLens + 12);
    const double len = std::sqrt(d[0] * lz * d[0] * lz + d[1] * lw * d[1] * lw) * inv;
    const double ratio = std::fabs(len / ((sz[1] < sz[0] ? sz[1] : sz[0]) * rf(r, kOffScale + 8)));
    const int n = ratio > 7.8 ? 16 : ratio > 3.8 ? 8 : ratio > 1.8 ? 4 : ratio > 0.8 ? 2 : 1;
    double f = ratio / n; f = f < 1.0 ? f : 1.0;
    if (f < 0.7) { f = f * 3.5 - 1.75; f = f > 0 ? f : 0; }
    static constexpr float ofs[4][2] = {{-1, 0}, {0, -1}, {0, 1}, {1, 0}};
    for (int j = 0; j < n; ++j) {
        const double u = double(j) / n;
        if (u < par[1] || par[0] < u) { ++st.out_of_range; continue; }
        double alpha = al[1] + u * (al[0] - al[1]);
        if (j & 1) alpha *= f;
        if (alpha < 1.0) { ++st.alpha_skip; continue; }
        Puff pf; pf.segment = vid; pf.sub = unsigned(j); pf.subs = unsigned(n); pf.u = u; pf.alpha = alpha;
        pf.size = sz[1] + u * (sz[0] - sz[1]);
        for (int k = 0; k < 3; ++k) pf.screen[k] = base[k] + u * slope[k];
        pf.half[0] = pf.size * lx; pf.half[1] = pf.size * ly;
        const double sd = seed[1] + (seed[0] - seed[1]) * u;
        const float* rv = rnd.v[hash_index(sd)];
        const int frame = int(rv[0] * 8.0f) * 4;
        const double uoff = double(int(rv[1] * rf(r, kOffUv + 12)));
        for (int c = 0; c < 4; ++c) {
            const unsigned cb = kOffColour + 16u * unsigned(c);
            for (int k = 0; k < 3; ++k) pf.colour[c][k] = float(ri(r, cb + 4u * unsigned(k))) * 0.0078125f;
            pf.colour[c][3] = float(alpha * 0.0078125);
            int ti = frame + c; ti = ti < 0 ? 0 : ti > 31 ? 31 : ti;   // GPU: out-of-range reads return 0; the tests stay in range
            const unsigned tb = kOffUvTable + 16u * unsigned(ti);
            pf.uv[c][0] = float((rf(r, kOffUv) - 8.0 + (rf(r, tb) + uoff) * rf(r, kOffUv + 8)) / 4096.0);
            pf.uv[c][1] = float((rf(r, kOffUv + 4) - 8.0 + rf(r, tb + 4) * rf(r, kOffUv + 8)) / 4096.0);
            const double X = pf.screen[0] + pf.half[0] * ofs[c][0], Y = pf.screen[1] + pf.half[1] * ofs[c][1];
            pf.sv[c][0] = float((X - sc.cb11[0]) * 2.0 / sc.cb10[0]);
            pf.sv[c][1] = float((Y - sc.cb11[1]) * 2.0 / sc.cb10[1]);
            pf.sv[c][2] = float(pf.screen[2] * kDepthUnit);
            pf.sv[c][3] = 1.0f;
        }
        out.push_back(pf); ++st.puffs;
    }
}
// The whole draw (DrawInstanced(vertex_count, 1, start_vertex, 0)): one GS invocation per segment.
inline GsStats emulate_draw(const uint8_t* r, const RandomTable& rnd, const ScreenConsts& sc, const DrawArgs& d, std::vector<Puff>& out) {
    GsStats st;
    for (unsigned i = 0; i < d.vertex_count; ++i) emulate_segment(r, rnd, sc, d.start_vertex + i, out, st);
    return st;
}

// ---- bounded per-frame capture (producer side; capture-log-only uses ONE instance; the replay (step 3) will need one per ring slot)
struct Captured {
    uint8_t rec[kRecordBytes] = {};
    DrawArgs args;
    uint64_t t10_va = 0, t11_va = 0, pso = 0;
    float roots[3][2] = {};    // cb10, cb11, cb12 when the root layout makes them readable (root constants); else root_known = false
    bool root_known = false;
    const char* roots_why = nullptr;   // why the roots are unknown (static string), nullptr when known or not given
};
class FrameCapture {
public:
    // live: 10-12 smoke draws per frame after one missile (D-352); m03k dogfight (headset 2026-10-30, step133) up to 27 per frame: with the old cap of 16 every
    // overflow (2,125 draws = all of that session's window fallbacks) went to the WINDOW effects image, i.e. aircraft/missile trails cut at the game window
    // (docs section 18). 64 = 2.4 x the live peak; more are counted (`smoke-capture-cap`, fx-smoke-totals `over-cap=`), not stored.
    static constexpr unsigned kCap = 64;
    void begin(uint64_t frame) noexcept { frame_ = frame; n_ = 0; overflow_ = 0; refused_ = 0; last_why_ = nullptr; }
    // Copies one draw's inputs. nullptr = stored; else why (refused or over the cap: counted; the game's draw is never affected).
    const char* add(const uint8_t* rec, size_t bytes, const DrawArgs& a, uint64_t t10, uint64_t t11, uint64_t pso, const float* roots6,
                    const char* roots_why = nullptr) noexcept {
        if (const char* why = record_check(rec, bytes, a)) { ++refused_; last_why_ = why; return why; }
        if (n_ >= kCap) { ++overflow_; last_why_ = "smoke-capture-cap"; return last_why_; }
        Captured& c = d_[n_++];
        std::memcpy(c.rec, rec, kRecordBytes); c.args = a; c.t10_va = t10; c.t11_va = t11; c.pso = pso;
        c.root_known = roots6 != nullptr;
        c.roots_why = roots6 ? nullptr : roots_why;
        for (int i = 0; i < 6; ++i) c.roots[i / 2][i % 2] = roots6 ? roots6[i] : 0.0f;
        return nullptr;
    }
    // Live producer: the cap is tested BEFORE the game's memory is read (a draw over the cap costs no read), and a draw whose record could not be read
    // (span / mapping / fault / torn) is counted as refused with its reason. Neither stores anything.
    bool full() const noexcept { return n_ >= kCap; }
    void note_overflow() noexcept { ++overflow_; last_why_ = "smoke-capture-cap"; }
    void note_refused(const char* why) noexcept { ++refused_; last_why_ = why ? why : "smoke-refused"; }
    unsigned count() const noexcept { return n_; }
    const Captured& at(unsigned i) const noexcept { return d_[i]; }
    uint64_t frame() const noexcept { return frame_; }
    unsigned overflow() const noexcept { return overflow_; }
    unsigned refused() const noexcept { return refused_; }
    const char* last_why() const noexcept { return last_why_; }
private:
    Captured d_[kCap];
    unsigned n_ = 0, overflow_ = 0, refused_ = 0;
    uint64_t frame_ = 0;
    const char* last_why_ = nullptr;
};
static_assert(sizeof(FrameCapture) <= 108 * 1024, "smoke capture: bounded, <= 108 KB (one instance in the heap-allocated producer state)");

// ---- the live measurement: does the game put trail points OUTSIDE its window into the records? (route S2 needs them)
struct FrameSummary {
    unsigned draws = 0, segments = 0, points = 0, in_window = 0, outside_window = 0, behind = 0, a_differs = 0, overflow = 0, refused = 0;
    double lens_rx = 0, lens_ry = 0, lens_rzx = 0, lens_rwy = 0;
    bool lens_proven = false, roots = false;
    float cb10[2] = {}, cb11[2] = {}, cb12[2] = {};
    // record +0 vs the SAME frame's DrawACM_v A (the first accepted world-objects record; needs gpu_copy_step5_world_objects): max |difference| over the
    // 16 floats of every captured draw, and that maximum relative to max |A|. obj_a = false: no world record this frame (not compared).
    bool obj_a = false; double obj_a_diff = 0, obj_a_rel = 0;
    const char* roots_why = nullptr;   // draw 0's reason when the roots are unknown
    const char* last_why = nullptr;    // the last refusal / overflow reason of the frame
};
// Max |rec+0 - objA| over the captured draws (float32 values compared as doubles); rel = that / max |objA|. false when there is nothing to compare.
inline bool compare_object_a(const FrameCapture& fc, const float obj_a16[16], double& diff, double& rel) noexcept {
    diff = 0; rel = 0;
    if (!obj_a16 || fc.count() == 0) return false;
    double mag = 0;
    for (int k = 0; k < 16; ++k) { if (!std::isfinite(obj_a16[k])) return false; mag = std::fmax(mag, std::fabs(double(obj_a16[k]))); }
    for (unsigned i = 0; i < fc.count(); ++i)
        for (unsigned k = 0; k < 16; ++k) diff = std::fmax(diff, std::fabs(double(rf(fc.at(i).rec, kOffA + 4 * k)) - double(obj_a16[k])));
    rel = mag > 0 ? diff / mag : 0;
    return true;
}
// Points are taken un-aged (the drift needs the random table): a position check, not the exact puff centre.
inline FrameSummary summarise(const FrameCapture& fc, const float* obj_a16 = nullptr) noexcept {
    FrameSummary s; s.draws = fc.count(); s.overflow = fc.overflow(); s.refused = fc.refused(); s.last_why = fc.last_why();
    s.obj_a = compare_object_a(fc, obj_a16, s.obj_a_diff, s.obj_a_rel);
    for (unsigned i = 0; i < fc.count(); ++i) {
        const Captured& c = fc.at(i);
        double a[16]; load_m(c.rec, kOffA, a);
        if (i > 0 && std::memcmp(c.rec + kOffA, fc.at(0).rec + kOffA, 64) != 0) ++s.a_differs;
        s.segments += c.args.vertex_count;
        for (unsigned n = c.args.start_vertex; n <= c.args.start_vertex + c.args.vertex_count; ++n) {
            const unsigned b = kOffPoints + n * kPointStride;
            const double p[3] = {rf(c.rec, b), rf(c.rec, b + 4), rf(c.rec, b + 8)};
            double h[4]; xform(a, p, h); ++s.points;
            if (!(h[3] > 0)) { ++s.behind; continue; }
            if (acz::fxmem::in_window(h[0] / h[3], h[1] / h[3])) ++s.in_window; else ++s.outside_window;
        }
    }
    if (fc.count()) {
        const LensFit L = lens_fit(fc.at(0).rec);
        s.lens_rx = L.rx; s.lens_ry = L.ry; s.lens_rzx = L.rzx; s.lens_rwy = L.rwy; s.lens_proven = L.proven;
        const Captured& c = fc.at(0);
        s.roots = c.root_known; s.roots_why = c.roots_why;
        for (int k = 0; k < 2; ++k) { s.cb10[k] = c.roots[0][k]; s.cb11[k] = c.roots[1][k]; s.cb12[k] = c.roots[2][k]; }
    }
    return s;
}
// The feature log (step5-producer.log) truncates a line at 480 bytes; the caller prefixes "gpu-copy-step1;event=" (21 bytes).
inline constexpr size_t kLineLimit = 480, kLinePrefix = 21;
// step5-producer.log line (one per frame while smoke is captured, rate-limited by the caller: `skipped` = smoke frames not logged since the last line).
// Every number is clamped so the worst case stays under kLineLimit - kLinePrefix (offline-tested). Returns snprintf's result.
inline int format_summary(char* buf, size_t n, uint64_t frame, const FrameSummary& s, unsigned skipped = 0) noexcept {
    auto cl = [](double v) { return v > 9999.0 ? 9999.0 : v < -9999.0 ? -9999.0 : v; };   // NaN passes through ("nan")
    auto cf = [](float v) { return double(v > 1e9f ? 1e9f : v < -1e9f ? -1e9f : v); };
    auto sw = [](const char* w) { return w ? w : "none"; };
    auto cu = [](unsigned v) { return v > 99999u ? 99999u : v; };
    char obj[48];
    if (s.obj_a) std::snprintf(obj, sizeof obj, "%.3g(rel=%.2g)", cl(s.obj_a_diff), cl(s.obj_a_rel));
    else std::snprintf(obj, sizeof obj, "none");
    return std::snprintf(buf, n,
        "fx-smoke-frame;frame=%llu;draws=%u;segments=%u;points=%u;pts-in-window=%u;pts-outside-window=%u;pts-behind=%u;a-differs=%u;"
        "lens=%.4g,%.4g,%.4f,%.4f(%s);roots=%s;cb10=%.5g,%.5g;cb11=%.5g,%.5g;cb12=%.5g,%.5g;overflow=%u;refused=%u;a-vs-obj=%s;roots-why=%.20s;last-why=%.20s;"
        "skipped=%u",
        static_cast<unsigned long long>(frame), cu(s.draws), cu(s.segments), cu(s.points), cu(s.in_window), cu(s.outside_window), cu(s.behind),
        cu(s.a_differs), cl(s.lens_rx), cl(s.lens_ry), cl(s.lens_rzx), cl(s.lens_rwy), s.lens_proven ? "proven" : "unproven", s.roots ? "known" : "unknown",
        cf(s.cb10[0]), cf(s.cb10[1]), cf(s.cb11[0]), cf(s.cb11[1]), cf(s.cb12[0]), cf(s.cb12[1]), cu(s.overflow), cu(s.refused), obj,
        s.roots ? "none" : sw(s.roots_why), sw(s.last_why), cu(skipped));
}

// ---- rate limit of the live line: the first `first` smoke frames, then at most one per `period_ms` (frames in between are counted as skipped).
struct LogGate {
    uint64_t last_ms = 0; unsigned emitted = 0, skipped = 0;
    // true = log this frame (`skipped_out` = smoke frames dropped since the previous line); false = dropped (counted).
    bool admit(uint64_t now_ms, unsigned& skipped_out, unsigned first = 3, uint64_t period_ms = 1000) noexcept {
        skipped_out = 0;
        if (emitted < first || now_ms - last_ms >= period_ms) { skipped_out = skipped; skipped = 0; last_ms = now_ms; ++emitted; return true; }
        ++skipped; return false;
    }
};

// ---- cb10 / cb11 / cb12 from the bound root signature (SDK-free mirror of acz::metadata; the producer fills the views from the layout)
// kind: 0 table, 1 root constants, 2 root CBV (acz::metadata::ParameterKind); visibility: 0 all, 4 geometry, 5 pixel (acz::metadata::Visibility).
// table_has_reg: a descriptor table whose CBV ranges include register `reg` of space 0 (precomputed by the producer per register).
struct RootParamView { uint32_t kind = 0, visibility = 0, reg = 0, space = 0, constants = 0; bool table_cbv[3] = {}; };
inline constexpr uint32_t kDenyGs = 16, kDenyPs = 32;   // D3D12_ROOT_SIGNATURE_FLAG_DENY_{GEOMETRY,PIXEL}_SHADER_ROOT_ACCESS
// The root-constants parameter that holds cb`reg` (10 or 11 for the GS, 12 for the PS) for that stage. nullptr = found (index set), else why not.
// Only root constants are readable by the producer (the words are tracked at the SetGraphicsRoot32BitConstant(s) seam); a root CBV or a table is
// reported, never dereferenced.
inline const char* find_root_constants(const RootParamView* p, unsigned n, uint32_t root_flags, uint32_t reg, unsigned& index) noexcept {
    if (reg < 10 || reg > 12) return "register";
    const bool pixel = reg == 12;
    if (root_flags & (pixel ? kDenyPs : kDenyGs)) return "stage-denied";
    const uint32_t stage = pixel ? 5u : 4u;
    unsigned found = 0; const char* other = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        if (!(p[i].visibility == 0 || p[i].visibility == stage)) continue;
        if (p[i].kind == 0) { if (p[i].table_cbv[reg - 10]) other = "descriptor-table"; continue; }
        if (p[i].reg != reg || p[i].space != 0) continue;
        if (p[i].kind == 2) { other = "root-cbv"; continue; }
        if (p[i].kind != 1) continue;
        if (p[i].constants < 2) { other = "constants-short"; continue; }
        index = i; ++found;
    }
    if (found > 1) return "ambiguous";
    if (found == 1) return other ? "ambiguous" : nullptr;
    return other ? other : "absent";
}
// Two 32-bit words of a root-constants parameter as floats; both must have been written in this binding generation.
inline const char* root_pair(const uint32_t* words, const bool* written, size_t n, float out[2]) noexcept {
    if (!words || !written || n < 2) return "unwritten";
    if (!written[0] || !written[1]) return "unwritten";
    std::memcpy(&out[0], &words[0], 4); std::memcpy(&out[1], &words[1], 4);
    return nullptr;
}

// ================================================================================== replay core (section 8.6; GPU recording NOT built)
// The eye's A exactly as world::substitute builds the DrawACM_v copy for that eye (gpu_copy_step5_world.h): A'_e = A + D_e x S, element by element
// n = (d == 0 ? raw : float(raw + d)) with d = sum_k D[r][k] S[k][c] (a zero delta keeps the raw bits). D = PoseFrame::D[eye], S = PoseFrame::S of the frame's
// world camera; `raw` = the smoke record (whose +0 the caller proved equal to the world reference A). out = the float values as doubles. false: non-finite.
inline bool eye_a_from_delta(const uint8_t* raw, const double D[16], const double S[16], double out[16]) noexcept {
    if (!raw || !D || !S || !out) return false;
    for (int i = 0; i < 16; ++i) {
        double d0 = 0; for (int k = 0; k < 4; ++k) d0 += D[(i / 4) * 4 + k] * S[k * 4 + (i % 4)];
        const float r0 = rf(raw, kOffA + 4u * unsigned(i));
        const float n0 = d0 == 0.0 ? r0 : float(double(r0) + d0);
        if (!std::isfinite(n0)) return false;
        out[i] = double(n0);
    }
    return true;
}
// +0 of the smoke record against the frame's world reference A (world::Frame::ref, the first accepted DrawACM_v record): the same tolerance as
// world::record_matches. Live run Q: a-vs-obj = 0 exactly on all 32 lines. nullptr = the same camera.
inline const char* a_matches_world(const uint8_t* raw, const uint8_t* ref64) noexcept {
    if (!raw || !ref64) return "smoke-world-a-missing";
    for (unsigned i = 0; i < 16; ++i) {
        const double a = rf(raw, kOffA + 4 * i), b = rf(ref64, 4 * i);
        if (!(std::fabs(a - b) <= 1e-6 * std::fmax(std::fabs(b), 1.0))) return "smoke-a-differs-from-world";
    }
    return nullptr;
}
// What the replay of one eye needs from the world frame (both pointers into the producer's world::Frame of THIS frame).
struct EyeDelta { const double* D[2] = {nullptr, nullptr}; const double* S = nullptr; const uint8_t* world_ref64 = nullptr; };
struct ReplayCfg {
    double guard = 1.25;        // clip guard band of the eye's clip matrix
    float depth_scale = 1.0f;   // = the objects' depth scale (xr_world_objects_depth_fix: 2^-10), so the smoke depth-tests against the objects' eye depth
};
// Both eye copies of one captured draw. Every precondition is a refusal (fail closed): the record (record_check), the screen constants (the replay keeps the
// game's cb10/cb11), the lens FORM (the puffs would be the wrong size in an eye with another lens), +0 = the world frame's A (the eyes are built from it),
// a finite eye A of the depth form. out0 / out1: kRecordBytes each (undefined on failure).
inline const char* prepare_eye_records(const uint8_t* raw, size_t bytes, const DrawArgs& args, const float* roots6, const EyeDelta& ed, const ReplayCfg& cfg,
                                       uint8_t* out0, uint8_t* out1) noexcept {
    if (const char* why = record_check(raw, bytes, args)) return why;
    if (const char* why = screen_consts_check(roots6)) return why;
    if (!lens_fit(raw).proven) return "smoke-lens-field-unproven";
    if (!ed.D[0] || !ed.D[1] || !ed.S) return "smoke-world-pose-missing";
    if (const char* why = a_matches_world(raw, ed.world_ref64)) return why;
    SubstCfg sc; sc.guard = cfg.guard; sc.rescale_lens = true; sc.depth_scale = cfg.depth_scale;
    uint8_t* out[2] = {out0, out1};
    for (unsigned e = 0; e < 2; ++e) {
        double ae[16];
        if (!eye_a_from_delta(raw, ed.D[e], ed.S, ae)) return "smoke-eye-a-nonfinite";
        if (const char* why = substitute(raw, ae, sc, out[e])) return why;
    }
    return nullptr;
}

// The per-slot copy store: kCap draws x 2 eyes, each copy at a 16-byte aligned stride (a root SRV VA needs 4-byte alignment; the GPU path will place this
// block in the slot's UPLOAD region and bind eye e of draw i at upload_va + offset(i, e)). Bounded: 204,800 bytes (64 draws; 51,200 with the old cap 16).
inline constexpr unsigned kRecordStride = 1600;
static_assert(kRecordStride >= kRecordBytes && kRecordStride % 16 == 0, "16-byte aligned record copies");
class ReplayStore {
public:
    static constexpr unsigned kCap = FrameCapture::kCap;
    static constexpr size_t kBytes = size_t(kCap) * 2 * kRecordStride;
    static constexpr uint64_t offset(unsigned i, unsigned eye) noexcept { return (uint64_t(i) * 2 + eye) * kRecordStride; }
    void begin() noexcept { n_ = 0; refused_ = 0; last_why_ = nullptr; }
    // nullptr = stored as draw `index`; else why (counted; nothing stored).
    const char* put(const uint8_t* raw, size_t bytes, const DrawArgs& args, const float* roots6, const EyeDelta& ed, const ReplayCfg& cfg, unsigned& index) noexcept {
        if (n_ >= kCap) { ++refused_; last_why_ = "smoke-replay-cap"; return last_why_; }
        if (const char* why = prepare_eye_records(raw, bytes, args, roots6, ed, cfg, b_ + offset(n_, 0), b_ + offset(n_, 1))) { ++refused_; last_why_ = why; return why; }
        a_[n_] = args; index = n_++;
        return nullptr;
    }
    unsigned count() const noexcept { return n_; }
    unsigned refused() const noexcept { return refused_; }
    const char* last_why() const noexcept { return last_why_; }
    const uint8_t* eye(unsigned i, unsigned e) const noexcept { return i < n_ && e < 2 ? b_ + offset(i, e) : nullptr; }
    const DrawArgs& args(unsigned i) const noexcept { return a_[i < kCap ? i : 0]; }
private:
    uint8_t b_[kBytes] = {};
    DrawArgs a_[kCap] = {};
    unsigned n_ = 0, refused_ = 0;
    const char* last_why_ = nullptr;
};
static_assert(sizeof(ReplayStore) <= 202 * 1024, "smoke replay store: bounded, <= 202 KB (one instance in the heap-allocated producer state)");

// ---- root-argument substitution: the smoke GS reads t10 through ONE root SRV parameter (the capture refuses a table / vertex binding / unresolved t10),
// so the replay of eye e binds its copy's VA at that parameter, draws, and binds the game's own VA back before anything of the game runs again.
struct RootSwap { uint32_t parameter = UINT32_MAX; uint64_t eye_va[2] = {}; uint64_t restore_va = 0; };
inline const char* plan_root_swap(uint32_t parameter, bool root_srv, uint64_t game_va, uint64_t upload_va, uint64_t upload_bytes, unsigned index,
                                  RootSwap& out) noexcept {
    out = RootSwap{};
    if (parameter == UINT32_MAX || parameter >= 64) return "smoke-t10-parameter";
    if (!root_srv) return "smoke-t10-not-root-srv";
    if (!game_va) return "smoke-t10-game-va";
    if (!upload_va || upload_va % 16 != 0) return "smoke-upload-va";
    if (index >= ReplayStore::kCap) return "smoke-replay-cap";
    const uint64_t end = ReplayStore::offset(index, 1) + kRecordStride;
    if (upload_bytes < end || upload_va > UINT64_MAX - end) return "smoke-upload-span";
    out.parameter = parameter; out.restore_va = game_va;
    for (unsigned e = 0; e < 2; ++e) out.eye_va[e] = upload_va + ReplayStore::offset(index, e);
    return nullptr;
}
// The replay's command sequence after the game's own smoke draw (the GPU path records exactly this, native vtable calls, under the effects predicate):
// [SetPipelineState(clone)] { OMSet(eye e FX RT, eye e objects DSV), SetGraphicsRootShaderResourceView(param, eye_va[e]), Draw(args) } x 2,
// SetGraphicsRootShaderResourceView(param, game VA), [SetPipelineState(game)], OMSet(the game's targets). <= 10 commands.
enum class CmdKind : uint8_t { set_pso_clone, om_eye, root_srv, draw, set_pso_game, om_restore };
struct Cmd { CmdKind kind = CmdKind::draw; uint8_t eye = 0; uint32_t parameter = 0; uint64_t va = 0; };
inline constexpr unsigned kMaxReplayCmds = 10;
inline unsigned replay_commands(const RootSwap& r, bool dual, Cmd out[kMaxReplayCmds]) noexcept {
    if (r.parameter == UINT32_MAX) return 0;
    unsigned n = 0;
    if (dual) out[n++] = Cmd{CmdKind::set_pso_clone, 0, 0, 0};
    for (uint8_t e = 0; e < 2; ++e) {
        out[n++] = Cmd{CmdKind::om_eye, e, 0, 0};
        out[n++] = Cmd{CmdKind::root_srv, e, r.parameter, r.eye_va[e]};
        out[n++] = Cmd{CmdKind::draw, e, 0, 0};
    }
    out[n++] = Cmd{CmdKind::root_srv, 0, r.parameter, r.restore_va};
    if (dual) out[n++] = Cmd{CmdKind::set_pso_game, 0, 0, 0};
    out[n++] = Cmd{CmdKind::om_restore, 0, 0, 0};
    return n;
}

// ---- fxmem:2 dry run (live, CPU only): per frame, every captured draw prepared for both eyes (ReplayStore::put) and its trail points (un-aged) counted:
// visible in eye e = in front of the eye (w > 1) and inside the eye's image (the same 2048 +- 256 / 224 GS window, now the eye's frustum); eye-only = visible
// in at least one eye but NOT inside the game window (the off-window trail the replay would add).
struct ReplayDryRun {
    unsigned draws = 0, prepared = 0, refused = 0, points = 0, eye_visible[2] = {0, 0}, eye_only = 0;
    double gain = 0; float depth_scale = 1.0f; const char* last_why = nullptr; const char* world_why = nullptr;
    void begin() noexcept { *this = ReplayDryRun{}; }
};
inline void count_eye_points(const uint8_t* game, const uint8_t* eye0, const uint8_t* eye1, const DrawArgs& d, ReplayDryRun& dr) noexcept {
    double ag[16], ae[2][16]; load_m(game, kOffA, ag); load_m(eye0, kOffA, ae[0]); load_m(eye1, kOffA, ae[1]);
    for (unsigned n = d.start_vertex; n <= d.start_vertex + d.vertex_count && n < kMaxPoints; ++n) {
        const unsigned b = kOffPoints + n * kPointStride;
        const double p[3] = {rf(game, b), rf(game, b + 4), rf(game, b + 8)};
        double h[4]; xform(ag, p, h);
        const bool inGame = h[3] > 0 && acz::fxmem::in_window(h[0] / h[3], h[1] / h[3]);
        bool anyEye = false;
        for (unsigned e = 0; e < 2; ++e) {
            xform(ae[e], p, h);
            if (h[3] > 1.0 && acz::fxmem::in_window(h[0] / h[3], h[1] / h[3])) { ++dr.eye_visible[e]; anyEye = true; }
        }
        ++dr.points;
        if (anyEye && !inGame) ++dr.eye_only;
    }
}
inline int format_dry_run(char* buf, size_t n, uint64_t frame, const ReplayDryRun& r) noexcept {
    auto cu = [](unsigned v) { return v > 99999u ? 99999u : v; };
    const double g = r.gain > 9999.0 ? 9999.0 : r.gain;
    return std::snprintf(buf, n,
        "fx-smoke-replay-plan;frame=%llu;draws=%u;prepared=%u;refused=%u;points=%u;eye0-visible=%u;eye1-visible=%u;eye-only=%u;lens-gain=%.4g;"
        "depth-scale=%.4g;world=%.24s;last-why=%.24s;mode=dry-run(cpu-only,no-gpu-replay)",
        static_cast<unsigned long long>(frame), cu(r.draws), cu(r.prepared), cu(r.refused), cu(r.points), cu(r.eye_visible[0]), cu(r.eye_visible[1]),
        cu(r.eye_only), g, double(r.depth_scale), r.world_why ? r.world_why : "ok", r.last_why ? r.last_why : "none");
}

} // namespace acz::fxsmoke
