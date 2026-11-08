#pragma once
// Card (d) synthetic stereo: portable scene, matrix pipeline and pixel verifier.
// No platform, XR or D3D12 symbols; the real backend and the fake-only tests
// use exactly these functions, so the CPU expectation and the GPU upload share
// one float matrix per eye.
//
// Reviewed conventions (also recorded in J-155 findings):
// - World is the located XR reference space X (LOCAL): right-handed, +X right,
//   +Y up, -Z forward, metres. T_W_X is identity: there is no aircraft, seat or
//   recenter transform in this synthetic scene, and it is static in LOCAL.
// - Row vectors (cockpit_math.h): clip = [x y z 1] * VP, VP = V * P where
//   V = viewMatrixFromPose(raw XrView pose) (world->eye, applied exactly once;
//   the located pose already contains head motion and IPD) and
//   P = projectionFromFov(raw XrFovf, near, far, 0, 1, flipY=false).
// - Clip depth: D3D NDC z in [0,1], ordinary (near->0, far->1), finite far,
//   depth test LESS, depth clear 1.0. Not reversed-Z.
// - Viewport: px=(ndcX+1)*w/2, py=(1-ndcY)*h/2, top-left origin; pixel (i,j)
//   is sampled at its centre (i+0.5, j+0.5). No additional projection Y flip.
// - Culling NONE, so winding/handedness can never hide a quad.
// - Colours are linear; the sRGB RTV encodes once; readback bytes are sRGB.
#include "../cockpit_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace acz::probe::stereo {
constexpr double near_m = 0.05, far_m = 100.0;
constexpr double position_tolerance_px = 2.0;
constexpr double disparity_tolerance_px = 2.0;
constexpr double minimum_ipd_m = 0.01;
constexpr unsigned grid = 16;
constexpr int edge_guard_px = 2;
constexpr int colour_tolerance = 8; // per 8-bit sRGB channel

struct EyeView {
    double position[3]{};
    double orientation[4]{0, 0, 0, 1}; // x y z w, active local->X
    double angle_left = 0, angle_right = 0, angle_up = 0, angle_down = 0; // raw XrFovf radians
    uint32_t width = 0, height = 0; // recommended image rect, full swapchain image
};
using Matrix = std::array<float, 16>; // row-major, row-vector, world(X)->clip
struct FrameViews {
    std::array<EyeView, 2> eyes{};
    std::array<Matrix, 2> uploaded{}; // what the backend records as root constants
    uint64_t sample = 0;
};

enum class Paint : uint8_t { background, floor, red, green, blue, yellow, unknown };
struct Linear { float r, g, b; };
// Background equals the a2 dim clear. Everything stays dim and opaque.
inline constexpr std::array<Linear, 6> palette{{
    {0.02f, 0.02f, 0.02f}, {0.07f, 0.07f, 0.07f}, {0.30f, 0.02f, 0.02f},
    {0.02f, 0.30f, 0.02f}, {0.02f, 0.02f, 0.30f}, {0.30f, 0.30f, 0.02f}}};
inline const char* paint_name(Paint p) {
    static constexpr const char* names[] = {"background", "floor", "red", "green", "blue", "yellow", "unknown"};
    return names[static_cast<unsigned>(p)];
}

// Vertical quads lie in z=c (facing the LOCAL origin); the floor lies in y=c.
struct Quad { Paint paint; double c[3]; double half_u, half_v; bool horizontal; };
inline constexpr std::array<Quad, 5> scene{{
    {Paint::floor, {0.0, -1.6, -6.0}, 4.0, 4.0, true},
    {Paint::red, {-0.8, 0.25, -3.0}, 0.12, 0.12, false},
    {Paint::green, {0.0, -0.10, -2.0}, 0.10, 0.10, false},   // near disparity marker
    {Paint::blue, {0.8, 0.25, -3.5}, 0.15, 0.15, false},
    {Paint::yellow, {0.0, 0.55, -5.0}, 0.25, 0.25, false},   // far disparity marker
}};
constexpr unsigned marker_count = 4;      // scene[1..4]
constexpr unsigned near_marker = 1;       // index into markers (green)
constexpr unsigned far_marker = 3;        // index into markers (yellow)
inline const Quad& marker(unsigned m) { return scene[1 + m]; }

inline std::array<std::array<double, 3>, 4> corners(const Quad& q) {
    const double u = q.half_u, v = q.half_v;
    if (q.horizontal)
        return {{{q.c[0] - u, q.c[1], q.c[2] - v}, {q.c[0] + u, q.c[1], q.c[2] - v},
                 {q.c[0] + u, q.c[1], q.c[2] + v}, {q.c[0] - u, q.c[1], q.c[2] + v}}};
    return {{{q.c[0] - u, q.c[1] - v, q.c[2]}, {q.c[0] + u, q.c[1] - v, q.c[2]},
             {q.c[0] + u, q.c[1] + v, q.c[2]}, {q.c[0] - u, q.c[1] + v, q.c[2]}}};
}
struct Vertex { float p[3]; float c[3]; };
constexpr unsigned vertex_count = 6 * static_cast<unsigned>(scene.size());
inline std::array<Vertex, vertex_count> vertices() {
    std::array<Vertex, vertex_count> out{};
    unsigned n = 0;
    for (const auto& q : scene) {
        const auto k = corners(q);
        const auto& col = palette[static_cast<unsigned>(q.paint)];
        for (unsigned i : {0u, 1u, 2u, 0u, 2u, 3u})
            out[n++] = {{float(k[i][0]), float(k[i][1]), float(k[i][2])}, {col.r, col.g, col.b}};
    }
    return out;
}
inline uint64_t fnv(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
inline uint64_t scene_hash() { auto v = vertices(); return fnv(v.data(), sizeof(v)); }

inline double encode_srgb(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}
inline std::array<int, 3> expected_bytes(Paint p) {
    const auto& c = palette[static_cast<unsigned>(p)];
    auto b = [](float v) { return static_cast<int>(std::lround(encode_srgb(v) * 255.0)); };
    return {b(c.r), b(c.g), b(c.b)};
}
inline Paint classify(const uint8_t* rgba) {
    for (unsigned i = 0; i < palette.size(); ++i) {
        auto e = expected_bytes(static_cast<Paint>(i));
        if (std::abs(int(rgba[0]) - e[0]) <= colour_tolerance && std::abs(int(rgba[1]) - e[1]) <= colour_tolerance &&
            std::abs(int(rgba[2]) - e[2]) <= colour_tolerance && rgba[3] >= 255 - colour_tolerance)
            return static_cast<Paint>(i);
    }
    return Paint::unknown;
}

// One matrix per eye from the raw located view. Exceptions become refusal.
inline bool eye_matrix(const EyeView& v, Matrix& out) {
    try {
        if (!v.width || !v.height) return false;
        Pose pose{{v.position[0], v.position[1], v.position[2]},
                  {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]}};
        const auto view = viewMatrixFromPose(pose);
        const auto proj = projectionFromFov({v.angle_left, v.angle_right, v.angle_up, v.angle_down},
                                            near_m, far_m, 0.0, 1.0, false);
        const auto vp = multiply(view, proj);
        requireFiniteMatrix(vp);
        out = toFloat32(vp);
        for (float x : out) if (!std::isfinite(x)) return false;
        return true;
    } catch (...) { return false; }
}
inline bool same_matrix(const Matrix& a, const Matrix& b) { return std::memcmp(a.data(), b.data(), sizeof(Matrix)) == 0; }
inline uint64_t matrix_hash(const Matrix& m) { return fnv(m.data(), sizeof(m)); }

struct Projected { bool valid = false; double x = 0, y = 0, depth = 0, w = 0; };
// The CPU projection uses the float matrix exactly as uploaded.
inline Projected project(const Matrix& m, const double p[3], uint32_t width, uint32_t height) {
    const auto c = transformPoint(fromFloat32(m), {p[0], p[1], p[2]});
    Projected out;
    if (!(c[3] > 0) || !std::isfinite(c[3])) return out;
    const double x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
    if (!(z >= 0 && z <= 1)) return out;
    out.valid = true; out.w = c[3]; out.depth = z;
    out.x = (x + 1) * width / 2.0; out.y = (1 - y) * height / 2.0;
    return out;
}

// Reference ray caster (independent of rasterisation): nearest scene quad hit
// by the ray through an image position, between the near and far planes.
class RayCaster {
    Mat4 inv{}; uint32_t w = 0, h = 0; bool ok = false;
public:
    RayCaster(const Matrix& m, uint32_t width, uint32_t height) : w(width), h(height) {
        try { inv = inverse(fromFloat32(m)); ok = width && height; } catch (...) { ok = false; }
    }
    bool valid() const { return ok; }
    Paint at(double px, double py) const {
        if (!ok) return Paint::unknown;
        const double nx = 2.0 * px / w - 1.0, ny = 1.0 - 2.0 * py / h;
        auto unproject = [&](double z) {
            auto v = transformVec4(inv, {nx, ny, z, 1.0});
            return std::array<double, 3>{v[0] / v[3], v[1] / v[3], v[2] / v[3]};
        };
        const auto a = unproject(0.0), b = unproject(1.0);
        const double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        double best = 2.0; Paint hit = Paint::background;
        for (const auto& q : scene) {
            const int axis = q.horizontal ? 1 : 2;
            if (std::abs(d[axis]) < 1e-12) continue;
            const double t = (q.c[axis] - a[axis]) / d[axis];
            if (!(t >= 0 && t <= 1) || t >= best) continue;
            const double x = a[0] + t * d[0], y = a[1] + t * d[1], z = a[2] + t * d[2];
            const double du = std::abs(x - q.c[0]);
            const double dv = q.horizontal ? std::abs(z - q.c[2]) : std::abs(y - q.c[1]);
            if (du <= q.half_u && dv <= q.half_v) { best = t; hit = q.paint; }
        }
        return hit;
    }
};

struct PixelView { const uint8_t* data = nullptr; uint32_t width = 0, height = 0; uint64_t row_pitch = 0; };
inline const uint8_t* texel(const PixelView& p, uint32_t x, uint32_t y) { return p.data + y * p.row_pitch + uint64_t(x) * 4; }
// Probe checksum: FNV-1a-64 over each row's width*4 meaningful bytes, row by
// row (row padding excluded), folding 8-byte little-endian words then the tail.
inline uint64_t checksum(const PixelView& p) {
    uint64_t h = 1469598103934665603ull;
    const size_t row = size_t(p.width) * 4;
    for (uint32_t y = 0; y < p.height; ++y) {
        const uint8_t* r = p.data + y * p.row_pitch;
        size_t i = 0;
        for (; i + 8 <= row; i += 8) { uint64_t w; std::memcpy(&w, r + i, 8); h ^= w; h *= 1099511628211ull; }
        for (; i < row; ++i) { h ^= r[i]; h *= 1099511628211ull; }
    }
    return h;
}

struct MarkerReport {
    Paint paint = Paint::unknown, centre_paint = Paint::unknown;
    bool visible = false, pass = false;
    double px = 0, py = 0, depth_m = 0;     // CPU projection of the known world centre
    double cx = 0, cy = 0, ex = 0, ey = 0;  // measured / ray-expected centroids
    uint32_t count = 0, expected_count = 0;
    double error_px = 0;
};
struct EyeReport {
    bool pass = false; std::string reason;
    uint64_t checksum = 0, matrix = 0;
    std::array<MarkerReport, marker_count> markers{};
    unsigned visible = 0, grid_samples = 0, grid_edge_skipped = 0, grid_mismatch = 0, grid_background = 0;
    std::string first_mismatch;
};

inline EyeReport verify_eye(const EyeView& view, const Matrix& uploaded, const PixelView& pixels) {
    EyeReport r;
    auto refuse = [&](const std::string& why) { r.pass = false; r.reason = why; return r; };
    Matrix cpu{};
    if (!eye_matrix(view, cpu)) return refuse("CPU matrix refused for located view");
    r.matrix = matrix_hash(cpu);
    if (!same_matrix(cpu, uploaded)) return refuse("uploaded matrix differs from CPU matrix");
    if (!pixels.data || pixels.width != view.width || pixels.height != view.height || pixels.row_pitch < uint64_t(pixels.width) * 4)
        return refuse("readback dimensions/pitch do not match the located view image");
    r.checksum = checksum(pixels);
    const RayCaster ray(cpu, view.width, view.height);
    if (!ray.valid()) return refuse("matrix not invertible");
    const auto W = int64_t(view.width), H = int64_t(view.height);
    for (unsigned m = 0; m < marker_count; ++m) {
        auto& mr = r.markers[m];
        const auto& q = marker(m);
        mr.paint = q.paint;
        const auto centre = project(cpu, q.c, view.width, view.height);
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        bool corners_valid = centre.valid;
        for (const auto& k : corners(q)) {
            auto p = project(cpu, k.data(), view.width, view.height);
            corners_valid &= p.valid;
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
        mr.px = centre.x; mr.py = centre.y; mr.depth_m = centre.w;
        mr.visible = corners_valid && x0 >= 4 && y0 >= 4 && x1 <= W - 4 && y1 <= H - 4 && (x1 - x0) >= 3 && (y1 - y0) >= 3;
        if (!mr.visible) continue;
        ++r.visible;
        mr.centre_paint = classify(texel(pixels, uint32_t(std::floor(mr.px)), uint32_t(std::floor(mr.py))));
        const int64_t wx0 = std::max<int64_t>(0, int64_t(std::floor(x0)) - 6), wx1 = std::min<int64_t>(W - 1, int64_t(std::ceil(x1)) + 6);
        const int64_t wy0 = std::max<int64_t>(0, int64_t(std::floor(y0)) - 6), wy1 = std::min<int64_t>(H - 1, int64_t(std::ceil(y1)) + 6);
        double sx = 0, sy = 0, ex = 0, ey = 0;
        for (int64_t y = wy0; y <= wy1; ++y)
            for (int64_t x = wx0; x <= wx1; ++x) {
                if (classify(texel(pixels, uint32_t(x), uint32_t(y))) == q.paint) { ++mr.count; sx += x + 0.5; sy += y + 0.5; }
                if (ray.at(x + 0.5, y + 0.5) == q.paint) { ++mr.expected_count; ex += x + 0.5; ey += y + 0.5; }
            }
        if (mr.count) { mr.cx = sx / mr.count; mr.cy = sy / mr.count; }
        if (mr.expected_count) { mr.ex = ex / mr.expected_count; mr.ey = ey / mr.expected_count; }
        mr.error_px = mr.count ? std::hypot(mr.cx - mr.px, mr.cy - mr.py) : 1e9;
        const double e = mr.expected_count;
        mr.pass = mr.centre_paint == q.paint && mr.count > 0 && mr.expected_count > 0 &&
            mr.error_px <= position_tolerance_px && mr.count >= 0.8 * e - 4 && mr.count <= 1.25 * e + 4;
    }
    for (unsigned gy = 0; gy < grid; ++gy)
        for (unsigned gx = 0; gx < grid; ++gx) {
            const auto x = uint32_t((gx + 0.5) * view.width / grid), y = uint32_t((gy + 0.5) * view.height / grid);
            const double cx = x + 0.5, cy = y + 0.5;
            const Paint expected = ray.at(cx, cy);
            bool uniform = true;
            for (int dy = -edge_guard_px; dy <= edge_guard_px && uniform; dy += edge_guard_px)
                for (int dx = -edge_guard_px; dx <= edge_guard_px && uniform; dx += edge_guard_px)
                    uniform = ray.at(cx + dx, cy + dy) == expected;
            if (!uniform) { ++r.grid_edge_skipped; continue; }
            ++r.grid_samples;
            if (expected == Paint::background) ++r.grid_background;
            const Paint actual = classify(texel(pixels, x, y));
            if (actual != expected) {
                if (!r.grid_mismatch)
                    r.first_mismatch = std::to_string(x) + "," + std::to_string(y) + " expected=" + paint_name(expected) + " actual=" + paint_name(actual);
                ++r.grid_mismatch;
            }
        }
    if (r.visible < 3) return refuse("fewer than 3 known markers visible in this eye");
    for (const auto& mr : r.markers)
        if (mr.visible && !mr.pass) return refuse(std::string("marker ") + paint_name(mr.paint) + " position/colour mismatch");
    if (r.grid_mismatch) return refuse("expected background/scene colour mismatch at " + r.first_mismatch);
    if (r.grid_background < 8) return refuse("insufficient background samples");
    r.pass = true; r.reason = "ok";
    return r;
}

struct PairReport {
    bool pass = false; std::string reason;
    double ipd_m = 0, baseline_m = 0;
    std::array<bool, marker_count> both{};
    std::array<double, marker_count> measured{}, expected{};
    double relative_measured = 0, relative_expected = 0, relative_model = 0;
};
// Horizontal disparity d = x_left - x_right (pixels). For two markers at
// different depths the relative disparity d_near - d_far cancels per-eye
// asymmetric-FOV offsets and has the sign of the located baseline.
inline PairReport verify_pair(const FrameViews& f, const EyeReport& left, const EyeReport& right) {
    PairReport p;
    auto refuse = [&](const std::string& why) { p.pass = false; p.reason = why; return p; };
    const auto& L = f.eyes[0]; const auto& R = f.eyes[1];
    const double d[3] = {R.position[0] - L.position[0], R.position[1] - L.position[1], R.position[2] - L.position[2]};
    p.ipd_m = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    try {
        const auto rot = quatToMat4({L.orientation[0], L.orientation[1], L.orientation[2], L.orientation[3]});
        p.baseline_m = d[0] * rot[0] + d[1] * rot[1] + d[2] * rot[2]; // along left-eye +X
    } catch (...) { return refuse("invalid left orientation"); }
    if (!left.pass || !right.pass) return refuse("per-eye verification failed");
    for (unsigned m = 0; m < marker_count; ++m) {
        const auto& a = left.markers[m]; const auto& b = right.markers[m];
        p.both[m] = a.visible && b.visible;
        if (!p.both[m]) continue;
        p.measured[m] = a.cx - b.cx; p.expected[m] = a.px - b.px;
        if (std::abs(p.measured[m] - p.expected[m]) > disparity_tolerance_px)
            return refuse(std::string("disparity mismatch for ") + paint_name(a.paint));
    }
    if (!p.both[near_marker] || !p.both[far_marker]) return refuse("near/far disparity markers not visible in both eyes");
    p.relative_measured = p.measured[near_marker] - p.measured[far_marker];
    p.relative_expected = p.expected[near_marker] - p.expected[far_marker];
    const double fx = L.width / (std::tan(L.angle_right) - std::tan(L.angle_left));
    const double zn = left.markers[near_marker].depth_m, zf = left.markers[far_marker].depth_m;
    p.relative_model = fx * p.baseline_m * (1.0 / zn - 1.0 / zf);
    if (p.ipd_m < minimum_ipd_m) return refuse("located IPD below 10 mm; disparity sign not testable");
    // View 0 is labelled left: it must lie on the -X side of view 1 in its own frame.
    if (!(p.baseline_m >= minimum_ipd_m)) return refuse("located left view is not to the left of the right view");
    if (!(p.relative_expected * p.baseline_m > 0)) return refuse("CPU-expected relative disparity sign disagrees with located baseline");
    if (!(p.relative_measured * p.relative_expected > 0) || std::abs(p.relative_measured) < 1.0)
        return refuse("measured relative disparity zero or wrong sign");
    if (std::abs(p.relative_measured - p.relative_expected) > disparity_tolerance_px)
        return refuse("measured relative disparity differs from located-IPD expectation");
    p.pass = true; p.reason = "ok";
    return p;
}

// Readback (Map) is admissible only for this frame's completed, copied,
// still-acquired image. Shared by the owner and the native backend.
inline const char* readback_refusal(bool acquired, bool submitted, bool complete, bool copied, bool released) {
    if (!acquired || released) return "image not held";
    if (!submitted || !complete) return "readback before GPU completion refused";
    if (!copied) return "no recorded copy to read";
    return nullptr;
}

inline std::string fixed(double v) {
    char b[48]; std::snprintf(b, sizeof b, "%.3f", v); return b;
}
inline std::string hex(uint64_t v) {
    char b[24]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b;
}
inline std::string describe_view(unsigned eye, const EyeView& v, const Matrix& m) {
    std::string s = "stereo_view eye=" + std::to_string(eye) + " pos=" + fixed(v.position[0]) + "," + fixed(v.position[1]) + "," + fixed(v.position[2]) +
        " quat=" + fixed(v.orientation[0]) + "," + fixed(v.orientation[1]) + "," + fixed(v.orientation[2]) + "," + fixed(v.orientation[3]) +
        " fov_lrud=" + fixed(v.angle_left) + "," + fixed(v.angle_right) + "," + fixed(v.angle_up) + "," + fixed(v.angle_down) +
        " size=" + std::to_string(v.width) + "x" + std::to_string(v.height) + " matrix_hash=" + hex(matrix_hash(m)) + " VP=";
    for (unsigned i = 0; i < 16; ++i) { char b[24]; std::snprintf(b, sizeof b, "%s%.6g", i ? "," : "", m[i]); s += b; }
    return s;
}
inline std::string describe_marker(unsigned eye, const MarkerReport& m) {
    return "stereo_marker eye=" + std::to_string(eye) + " paint=" + paint_name(m.paint) + " visible=" + std::to_string(m.visible) +
        " projected=" + fixed(m.px) + "," + fixed(m.py) + " depth_m=" + fixed(m.depth_m) + " centre_pixel=" + paint_name(m.centre_paint) +
        " measured_centroid=" + fixed(m.cx) + "," + fixed(m.cy) + " expected_centroid=" + fixed(m.ex) + "," + fixed(m.ey) +
        " count=" + std::to_string(m.count) + " expected_count=" + std::to_string(m.expected_count) +
        " error_px=" + fixed(m.error_px) + " pass=" + std::to_string(m.pass);
}
inline std::string describe_eye(uint64_t frame, unsigned eye, const EyeReport& r) {
    return "stereo_eye frame=" + std::to_string(frame) + " eye=" + std::to_string(eye) + " checksum=" + hex(r.checksum) +
        " matrix_hash=" + hex(r.matrix) + " visible_markers=" + std::to_string(r.visible) +
        " grid_samples=" + std::to_string(r.grid_samples) + " grid_edge_skipped=" + std::to_string(r.grid_edge_skipped) +
        " grid_background=" + std::to_string(r.grid_background) + " grid_mismatch=" + std::to_string(r.grid_mismatch) +
        " pass=" + std::to_string(r.pass) + " reason=" + r.reason;
}
inline std::string describe_pair(uint64_t frame, const PairReport& p) {
    std::string s = "stereo_pair frame=" + std::to_string(frame) + " ipd_m=" + fixed(p.ipd_m) + " baseline_m=" + fixed(p.baseline_m);
    for (unsigned m = 0; m < marker_count; ++m)
        if (p.both[m]) s += std::string(" disparity_") + paint_name(marker(m).paint) + "=" + fixed(p.measured[m]) + "/" + fixed(p.expected[m]);
    return s + " relative_near_far=" + fixed(p.relative_measured) + " expected=" + fixed(p.relative_expected) +
        " ipd_model=" + fixed(p.relative_model) + " pass=" + std::to_string(p.pass) + " reason=" + p.reason;
}
}
