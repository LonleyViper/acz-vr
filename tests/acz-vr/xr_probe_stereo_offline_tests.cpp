// Card (d) synthetic stereo: fake XR/D3D12 receipts plus a pure C++ software
// rasteriser. The rasteriser (near/far clipping, edge functions at pixel
// centres, depth LESS, sRGB transfer) is independent of the verifier's ray
// caster, so agreement checks the matrix pipeline and expected-pixel logic.
// Nothing here touches a GPU, loader, runtime or driver.
#include "colour_owner.h"
#include "format_policy.h"
#include "supervision.h"
#include "synthetic_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace acz::probe;
using namespace acz::probe::stereo;
namespace {
unsigned checks = 0;
void check(bool b, const std::string& why) { ++checks; if (!b) { std::cerr << "FAIL " << why << '\n'; std::exit(1); } }
struct Time final : Clock {
    int64_t ms = 0;
    int64_t now() override { return ms; }
    void sleep(int64_t n) override { check(n >= 0 && n <= Caps::poll_ms, "bounded fake pause"); ms += n; }
};

// ---------- fixtures ----------
acz::Quat qmul(const acz::Quat& a, const acz::Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
acz::Quat axis(int i, double angle) { acz::Quat q{0, 0, 0, std::cos(angle / 2)}; q[i] = std::sin(angle / 2); return q; }
acz::Vec3 rotate(const acz::Quat& q, const acz::Vec3& v) { auto r = acz::transformVec4(acz::quatToMat4(q), {v[0], v[1], v[2], 0}); return {r[0], r[1], r[2]}; }
EyeView eye(const acz::Vec3& p, const acz::Quat& q, double l, double r, double u, double d, uint32_t w, uint32_t h) {
    EyeView e;
    for (int i = 0; i < 3; ++i) e.position[i] = p[i];
    for (int i = 0; i < 4; ++i) e.orientation[i] = q[i];
    e.angle_left = l; e.angle_right = r; e.angle_up = u; e.angle_down = d; e.width = w; e.height = h;
    return e;
}
FrameViews frame(const EyeView& L, const EyeView& R, uint64_t sample = 1) {
    FrameViews f; f.eyes = {L, R}; f.sample = sample;
    for (unsigned i = 0; i < 2; ++i) check(eye_matrix(f.eyes[i], f.uploaded[i]), "fixture matrix");
    return f;
}
// Head-centre fixture constructed ONCE: T_X_E = T_X_H T_H_E (pose-contract fixture rule).
FrameViews head(const acz::Vec3& H, const acz::Quat& q, double half_ipd, std::array<double, 4> fl, std::array<double, 4> fr,
                uint32_t w, uint32_t h) {
    auto off = [&](double x) { auto o = rotate(q, {x, 0, 0}); return acz::Vec3{H[0] + o[0], H[1] + o[1], H[2] + o[2]}; };
    return frame(eye(off(-half_ipd), q, fl[0], fl[1], fl[2], fl[3], w, h), eye(off(half_ipd), q, fr[0], fr[1], fr[2], fr[3], w, h));
}
const acz::Quat ident{0, 0, 0, 1};
FrameViews symmetric() { return head({0, 0, 0}, ident, 0.032, {-0.8, 0.8, 0.75, -0.75}, {-0.8, 0.8, 0.75, -0.75}, 800, 720); }
FrameViews asymmetric() { return head({0, 0, 0}, ident, 0.0315, {-0.98, 0.75, 0.85, -0.92}, {-0.75, 0.98, 0.85, -0.92}, 640, 704); }
acz::Quat head_q() { return qmul(qmul(axis(1, 0.2), axis(0, -0.1)), axis(2, 0.05)); }
FrameViews posed() { return head({0.05, 0.1, 0.08}, head_q(), 0.0315, {-0.98, 0.75, 0.85, -0.92}, {-0.75, 0.98, 0.85, -0.92}, 640, 704); }

// ---------- software rasteriser ----------
struct Image {
    uint32_t w = 0, h = 0; uint64_t pitch = 0; std::vector<uint8_t> data;
    PixelView view() const { return {data.data(), w, h, pitch}; }
    uint8_t* at(uint32_t x, uint32_t y) { return data.data() + y * pitch + uint64_t(x) * 4; }
};
enum class Transfer { single, none, twice };
uint8_t to_byte(float c, Transfer t) {
    double v = c;
    if (t != Transfer::none) v = encode_srgb(v);
    if (t == Transfer::twice) v = encode_srgb(v);
    return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255));
}
Image rasterise(const EyeView& v, const Matrix& m, Transfer tf = Transfer::single,
                std::function<bool(Paint)> include = [](Paint) { return true; }) {
    Image img; img.w = v.width; img.h = v.height; img.pitch = uint64_t(v.width) * 4 + 64; // padded rows
    img.data.assign(img.pitch * img.h, 0xCD);
    const auto& bg = palette[0];
    for (uint32_t y = 0; y < img.h; ++y)
        for (uint32_t x = 0; x < img.w; ++x) { auto* p = img.at(x, y); p[0] = to_byte(bg.r, tf); p[1] = to_byte(bg.g, tf); p[2] = to_byte(bg.b, tf); p[3] = 255; }
    std::vector<float> depth(size_t(img.w) * img.h, 1.0f);
    const auto M = acz::fromFloat32(m);
    const auto verts = vertices();
    for (unsigned t = 0; t < vertex_count / 3; ++t) {
        const Paint paint = scene[t / 2].paint;
        if (!include(paint)) continue;
        std::vector<acz::Vec4> poly;
        for (unsigned k = 0; k < 3; ++k) { const auto& p = verts[t * 3 + k].p; poly.push_back(acz::transformVec4(M, {p[0], p[1], p[2], 1.0})); }
        // Clip against D3D near (z>=0) and far (z<=w) planes.
        for (int plane = 0; plane < 2; ++plane) {
            auto f = [&](const acz::Vec4& c) { return plane == 0 ? c[2] : c[3] - c[2]; };
            std::vector<acz::Vec4> out;
            for (size_t i = 0; i < poly.size(); ++i) {
                const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
                const double fa = f(a), fb = f(b);
                if (fa >= 0) out.push_back(a);
                if ((fa >= 0) != (fb >= 0)) {
                    const double s = fa / (fa - fb); acz::Vec4 c;
                    for (int j = 0; j < 4; ++j) c[j] = a[j] + s * (b[j] - a[j]);
                    out.push_back(c);
                }
            }
            poly = out;
            if (poly.size() < 3) break;
        }
        if (poly.size() < 3) continue;
        struct S { double x, y, z; };
        std::vector<S> sp;
        for (const auto& c : poly) sp.push_back({(c[0] / c[3] + 1) * img.w / 2.0, (1 - c[1] / c[3]) * img.h / 2.0, c[2] / c[3]});
        const auto& col = palette[static_cast<unsigned>(paint)];
        const uint8_t rgb[3] = {to_byte(col.r, tf), to_byte(col.g, tf), to_byte(col.b, tf)};
        for (size_t k = 1; k + 1 < sp.size(); ++k) {
            const S a = sp[0], b = sp[k], c = sp[k + 1];
            const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 1e-12) continue;
            const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(int(img.w) - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
            const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(int(img.h) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                    double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                    double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float z = float(w0 * a.z + w1 * b.z + w2 * c.z);
                    auto& d = depth[size_t(y) * img.w + x];
                    if (!(z < d)) continue; // depth LESS
                    d = z; auto* p = img.at(x, y); p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = 255;
                }
        }
    }
    return img;
}
std::array<Image, 2> render(const FrameViews& f, Transfer tf = Transfer::single) {
    return {rasterise(f.eyes[0], f.uploaded[0], tf), rasterise(f.eyes[1], f.uploaded[1], tf)};
}
struct Verdict { EyeReport l, r; PairReport p; };
Verdict verify(const FrameViews& f, const Image& L, const Image& R) {
    Verdict v; v.l = verify_eye(f.eyes[0], f.uploaded[0], L.view()); v.r = verify_eye(f.eyes[1], f.uploaded[1], R.view());
    v.p = verify_pair(f, v.l, v.r); return v;
}
Image shifted(const Image& src, int dx, int dy) {
    Image out = src;
    for (uint32_t y = 0; y < src.h; ++y)
        for (uint32_t x = 0; x < src.w; ++x) {
            const int sx = std::clamp(int(x) - dx, 0, int(src.w) - 1), sy = std::clamp(int(y) - dy, 0, int(src.h) - 1);
            std::copy_n(src.data.data() + sy * src.pitch + uint64_t(sx) * 4, 4, out.at(x, y));
        }
    return out;
}

// ---------- fake XR/D3D12 backend for the owner/Run ----------
struct Fake final : Backend, ColourBackend {
    Time& t;
    FrameViews views = posed();
    std::array<Image, 2> cache, served;
    bool cached = false;
    ImageOp failure = ImageOp::count;
    bool restored = true, copied = true, setup_synthetic_failure = false, record_synthetic_failure = false;
    bool matrix_tamper = false, sample_tamper = false, pending = false, stopping = false;
    bool render_frames = true;
    std::function<void(unsigned, Image&)> corrupt;
    std::array<unsigned, static_cast<unsigned>(ImageOp::count)> calls{};
    std::array<ImageRecord, 2> s{};
    std::array<unsigned, 2> releases{}, reads{};
    unsigned projection_calls = 0, zero_calls = 0, synthetic_calls = 0, pixel_calls = 0, active_frame = 0;
    uint64_t last_issued = 0;
    bool destroyed = false;
    explicit Fake(Time& clock) : t(clock) {}
    Fake(const Fake&) = delete; Fake& operator=(const Fake&) = delete;
    ColourBackend* colour_backend() override { return this; }
    unsigned n(ImageOp op) const { return calls[static_cast<unsigned>(op)]; }
    Reply call(Op op, unsigned layer_count, const void* opaque) override {
        if (op == Op::formats) {
            bool good = observe_phase1_formats([&](uint32_t cap, uint32_t* count, int64_t* data) {
                *count = 1; if (cap) data[0] = 29; return true; }, [](const std::string&) {});
            return {good};
        }
        if (op == Op::poll) return {true, stopping ? State::stopping : State::ready};
        if (op == Op::request_exit) stopping = true;
        if (op == Op::wait_frame) { t.ms += 1; ++active_frame; }
        if (op == Op::end_frame) {
            if (!layer_count) { check(!opaque, "zero end has null layer"); ++zero_calls; }
            else {
                const auto& p = *static_cast<const Projection*>(opaque);
                check(layer_count == 1 && p.frame == active_frame && p.sample == active_frame, "exact located frame and sample");
                for (const auto& r : s) check(r.complete && r.released && !r.acquired, "both eyes complete and released before projection");
                if (synthetic_calls) check(reads[0] == reads[1] && reads[0] == synthetic_calls, "card d projection only after both readbacks of this frame");
                ++projection_calls;
            }
        }
        return {true, State::idle, false, render_frames};
    }
    bool synthetic_frame(FrameViews& out) override {
        ++synthetic_calls;
        out = views; out.sample = sample_tamper ? active_frame + 1 : active_frame;
        if (matrix_tamper) out.uploaded[1][0] = std::nextafter(out.uploaded[1][0], 10.0f);
        return true;
    }
    PixelView readback_pixels(unsigned eye) override { ++pixel_calls; return eye < 2 ? served[eye].view() : PixelView{}; }
    ImageReply image(ImageOp op, const ImageRequest& q) override {
        ++calls[static_cast<unsigned>(op)];
        ImageReply p; p.ok = true; p.key = q.key;
        if (op == failure && op != ImageOp::execute && op != ImageOp::setup && op != ImageOp::record) { p.ok = false; return p; }
        switch (op) {
        case ImageOp::setup:
            p.ok = !setup_synthetic_failure && failure != ImageOp::setup; p.synthetic_failure = setup_synthetic_failure;
            p.key.device = 1; p.key.queue = 2; p.key.fence = 3; p.key.epoch = 4; return p;
        case ImageOp::locate: p.sample = active_frame; p.views_valid = true; return p;
        case ImageOp::destroy:
            for (const auto& r : s) check(!r.acquired && (!r.submitted || r.complete), "destroy only after idle and legal release");
            destroyed = true; return p;
        default: break;
        }
        check(q.key.eye < 2, "bounded eye"); auto& r = s[q.key.eye];
        switch (op) {
        case ImageOp::acquire:
            check(!r.acquired, "one outstanding acquisition per eye");
            r = {}; r.key = {1, 2, 3, 4, 100 + q.key.eye, 200 + q.key.eye, 300 + q.key.eye, 400 + q.key.eye, q.key.frame, 0, q.key.eye, 0};
            r.acquired = true; p.key = r.key; break;
        case ImageOp::wait: check(r.acquired && !r.waited, "wait only acquisition"); r.waited = true; break;
        case ImageOp::record:
            check(r.waited && !r.released, "record after wait before release");
            if (record_synthetic_failure || failure == ImageOp::record) { p.ok = false; p.synthetic_failure = record_synthetic_failure; break; }
            p.restored = restored; p.copied = copied; r.recorded = true; break;
        case ImageOp::close:
            check(r.recorded && restored && copied, "owner never Closes an unrestored/uncopied recording"); r.closed = true; break;
        case ImageOp::execute:
            check(r.closed && !r.submitted && q.key.value == last_issued + 1, "one submit with next value");
            r.key = q.key; r.submitted = true; p.dispatch_possible = true; break;
        case ImageOp::signal: check(r.submitted && !r.signalled, "Signal once"); r.signalled = true; last_issued = q.key.value; break;
        case ImageOp::sample:
            check(r.submitted && r.signalled, "sample after Signal");
            p.completed = pending ? last_issued - 1 : last_issued; r.complete = p.completed >= r.key.value; break;
        case ImageOp::readback:
            // The copy is read only after proven completion and before release.
            check(r.acquired && r.submitted && r.complete && !r.released, "readback only after completion, before release");
            check(readback_refusal(r.acquired, r.submitted, r.complete, true, r.released) == nullptr, "shared readback guard admits");
            if (failure == ImageOp::readback) { p.ok = false; break; }
            if (!cached) { cache = render(views); cached = true; }
            served[q.key.eye] = cache[q.key.eye];
            if (corrupt) corrupt(q.key.eye, served[q.key.eye]);
            ++reads[q.key.eye]; r.read = true; break;
        case ImageOp::release:
            check(r.waited && (!r.submitted || r.complete) && !r.release_attempted, "legal single release");
            r.release_attempted = true; r.acquired = false; r.released = true; ++releases[q.key.eye]; break;
        default: break;
        }
        return p;
    }
};
struct Fixture {
    Time time; Fake fake{time}; std::vector<std::string> lines;
    Fixture() = default; Fixture(const Fixture&) = delete; Fixture& operator=(const Fixture&) = delete;
    Result run(Mode mode = Mode::phase3_synthetic_stereo) {
        Run run(fake, time, [&](const std::string& s) { lines.push_back(s); }, [] { return std::string{}; }, {}, mode);
        return run.execute();
    }
    bool has(const std::string& prefix) const {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; });
    }
    unsigned count(const std::string& prefix) const {
        return unsigned(std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; }));
    }
};
}

int main() {
    // ---------- matrix pipeline (pure maths) ----------
    {
        auto f = asymmetric();
        const auto& v = f.eyes[0];
        const double l = std::tan(v.angle_left), r = std::tan(v.angle_right), u = std::tan(v.angle_up), d = std::tan(v.angle_down);
        auto px = [&](double ex, double ey, double ez) {
            const double world[3] = {ex + v.position[0], ey + v.position[1], ez + v.position[2]}; // identity orientation
            return project(f.uploaded[0], world, v.width, v.height);
        };
        auto ahead = px(0, 0, -3);
        check(ahead.valid && std::abs(ahead.x - v.width * (-l) / (r - l)) < 0.01 && std::abs(ahead.y - v.height * u / (u - d)) < 0.01,
              "asymmetric FOV: straight-ahead point at the off-centre principal point");
        check(std::abs(px(l * 2, 0, -2).x) < 0.01 && std::abs(px(r * 2, 0, -2).x - v.width) < 0.01, "left/right frustum edges map to pixel 0/width");
        check(std::abs(px(0, u * 2, -2).y) < 0.01 && std::abs(px(0, d * 2, -2).y - v.height) < 0.01, "up/down edges map to top/bottom (top-left origin)");
        check(std::abs(px(0, 0, -near_m).depth) < 1e-5 && std::abs(px(0, 0, -far_m).depth - 1) < 1e-4, "ordinary D3D [0,1] depth: near 0, far 1");
        check(px(0, 0, -2).depth < px(0, 0, -20).depth, "depth increases with distance (LESS test)");
        check(!px(0, 0, 1).valid, "point behind the eye is not projected");
        check(std::abs(px(0, 0, -3).w - 3) < 1e-5, "clip w is forward distance");
    }
    {
        // Nonzero head pose with asymmetric FOV: eye-space point -> world via the
        // located pose once -> CPU projection equals the analytic eye-space pixel.
        auto f = posed();
        for (unsigned e = 0; e < 2; ++e) {
            const auto& v = f.eyes[e];
            acz::Pose pose{{v.position[0], v.position[1], v.position[2]}, {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]}};
            const acz::Vec3 pe{0.3, -0.2, -2.5};
            const auto w4 = acz::transformPoint(acz::poseToWorld(pose), pe);
            const double world[3] = {w4[0], w4[1], w4[2]};
            const auto p = project(f.uploaded[e], world, v.width, v.height);
            const double l = std::tan(v.angle_left), r = std::tan(v.angle_right), u = std::tan(v.angle_up), d = std::tan(v.angle_down);
            const double sx = pe[0] / -pe[2], sy = pe[1] / -pe[2];
            check(p.valid && std::abs(p.x - v.width * (sx - l) / (r - l)) < 0.02 && std::abs(p.y - v.height * (u - sy) / (u - d)) < 0.02,
                  "nonzero head pose: world point projects to its analytic eye-space pixel");
            const auto back = acz::transformPoint(acz::viewMatrixFromPose(pose), {w4[0], w4[1], w4[2]});
            check(std::abs(back[0] - pe[0]) < 1e-9 && std::abs(back[2] - pe[2]) < 1e-9, "row-vector view matrix inverts the located pose");
        }
        // Pose-contract example: Ry(pi/2) looks along -X (left).
        auto look = frame(eye({0, 0, 0}, axis(1, 1.5707963267948966), -0.8, 0.8, 0.8, -0.8, 400, 400),
                          eye({0, 0, 0}, axis(1, 1.5707963267948966), -0.8, 0.8, 0.8, -0.8, 400, 400));
        const double left_point[3] = {-3, 0, 0};
        auto c = project(look.uploaded[0], left_point, 400, 400);
        check(c.valid && std::abs(c.x - 200) < 0.01 && std::abs(c.y - 200) < 0.01, "positive yaw pi/2 looks left (pose contract)");
        Matrix m{};
        auto bad = look.eyes[0];
        bad.angle_right = 1.5707963267948966; check(!eye_matrix(bad, m), "FOV at pi/2 refused");
        bad = look.eyes[0]; bad.angle_right = bad.angle_left; check(!eye_matrix(bad, m), "empty horizontal FOV refused");
        bad = look.eyes[0]; bad.orientation[0] = bad.orientation[1] = bad.orientation[2] = bad.orientation[3] = 0; check(!eye_matrix(bad, m), "zero quaternion refused");
        bad = look.eyes[0]; bad.position[1] = std::numeric_limits<double>::quiet_NaN(); check(!eye_matrix(bad, m), "NaN position refused");
        bad = look.eyes[0]; bad.width = 0; check(!eye_matrix(bad, m), "zero width refused");
        // Unnormalized quaternion is normalized (contract), identical matrix.
        auto scaled = look.eyes[0]; for (auto& x : scaled.orientation) x *= 3;
        Matrix a{}, b{}; check(eye_matrix(scaled, a) && eye_matrix(look.eyes[0], b), "scaled quaternion accepted");
        for (unsigned i = 0; i < 16; ++i) check(std::abs(a[i] - b[i]) < 1e-6f, "quaternion normalized once");
    }
    // ---------- software rasteriser + verifier: PASS cases ----------
    for (int which = 0; which < 3; ++which) {
        const auto f = which == 0 ? symmetric() : which == 1 ? asymmetric() : posed();
        const auto img = render(f);
        const auto v = verify(f, img[0], img[1]);
        check(v.l.pass && v.r.pass, "software-rasterised eyes verify: " + v.l.reason + " / " + v.r.reason);
        check(v.p.pass, "software-rasterised pair verifies: " + v.p.reason);
        check(v.l.visible == 4 && v.r.visible == 4, "all four known points visible");
        for (const auto* e : {&v.l, &v.r}) {
            for (const auto& m : e->markers) {
                check(m.error_px < 0.75, "rasterised centroid within 0.75 px of the CPU projection");
                check(std::hypot(m.ex - m.cx, m.ey - m.cy) < 0.5, "ray-caster centroid agrees with rasteriser centroid");
                check(m.count == m.expected_count || std::abs(double(m.count) - m.expected_count) <= 0.02 * m.expected_count + 4, "pixel counts agree");
            }
            check(e->grid_mismatch == 0 && e->grid_background >= 8 && e->grid_samples > 100, "background/scene grid agrees");
        }
        check(v.p.relative_measured > 1 && v.p.baseline_m > 0, "near point has larger left-minus-right disparity");
        check(std::abs(v.p.relative_model - v.p.relative_expected) <= std::max(1.0, 0.25 * std::abs(v.p.relative_expected)),
              "located-IPD model agrees with exact CPU projection");
        if (which == 0) {
            // Symmetric, parallel, identity: d = fx * ipd / z exactly.
            const double fx = 800 / (2 * std::tan(0.8));
            check(std::abs(v.p.expected[near_marker] - fx * 0.064 / 2.0) < 0.05 && std::abs(v.p.expected[far_marker] - fx * 0.064 / 5.0) < 0.05,
                  "symmetric disparity equals fx*IPD/z");
            check(v.l.checksum != v.r.checksum, "eye images differ (checksum)");
        }
        if (which == 1)
            check(std::abs(v.p.expected[far_marker]) > 2 * disparity_tolerance_px, "asymmetric FOV offset dominates absolute disparity");
        // One-pixel shift stays within the documented 2 px tolerance.
        auto s1 = verify(f, shifted(img[0], 1, 0), img[1]);
        check(s1.p.pass, "1 px shift tolerated: " + s1.p.reason);
        auto s3 = verify(f, shifted(img[0], 3, 0), img[1]);
        check(!s3.l.pass && !s3.p.pass, "3 px shift rejected");
        auto sy = verify(f, img[0], shifted(img[1], 0, -3));
        check(!sy.r.pass, "3 px vertical shift rejected");
    }
    // ---------- negative pipeline cases ----------
    {
        const auto f = posed(); const auto img = render(f);
        auto sw = verify(f, img[1], img[0]); check(!sw.p.pass && (!sw.l.pass || !sw.r.pass), "swapped eye images rejected");
        // Y flip (cockpit_math default) must not be applied for D3D viewport.
        FrameViews flip = f;
        for (unsigned e = 0; e < 2; ++e) {
            const auto& v = f.eyes[e];
            acz::Pose pose{{v.position[0], v.position[1], v.position[2]}, {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]}};
            flip.uploaded[e] = acz::toFloat32(acz::multiply(acz::viewMatrixFromPose(pose),
                acz::projectionFromFov({v.angle_left, v.angle_right, v.angle_up, v.angle_down}, near_m, far_m, 0, 1, true)));
        }
        auto fl = verify(f, rasterise(f.eyes[0], flip.uploaded[0]), rasterise(f.eyes[1], flip.uploaded[1]));
        check(!fl.l.pass && !fl.r.pass, "extra projection Y flip rejected");
        // Head pose applied twice (p + R p, q*q).
        FrameViews twice = f;
        for (unsigned e = 0; e < 2; ++e) {
            auto& v = twice.eyes[e];
            acz::Quat q{v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]};
            auto rp = rotate(q, {v.position[0], v.position[1], v.position[2]});
            for (int i = 0; i < 3; ++i) v.position[i] += rp[i];
            auto q2 = qmul(q, q); for (int i = 0; i < 4; ++i) v.orientation[i] = q2[i];
            check(eye_matrix(v, twice.uploaded[e]), "double-head matrix");
        }
        auto dh = verify(f, rasterise(f.eyes[0], twice.uploaded[0]), rasterise(f.eyes[1], twice.uploaded[1]));
        check(!dh.l.pass && !dh.r.pass, "double head-pose application rejected");
        // Averaged/symmetric FOV instead of each eye's raw asymmetric FOV.
        auto sym = posed();
        for (auto& v : sym.eyes) { v.angle_left = -0.865; v.angle_right = 0.865; }
        for (unsigned e = 0; e < 2; ++e) check(eye_matrix(sym.eyes[e], sym.uploaded[e]), "sym matrix");
        auto sf = verify(f, rasterise(f.eyes[0], sym.uploaded[0]), rasterise(f.eyes[1], sym.uploaded[1]));
        check(!sf.l.pass && !sf.r.pass, "symmetric FOV substitution rejected");
        for (auto tf : {Transfer::none, Transfer::twice}) {
            auto t = render(f, tf); auto e = verify(f, t[0], t[1]);
            check(!e.l.pass && !e.r.pass, "missing/double sRGB encoding rejected");
        }
        auto missing = rasterise(f.eyes[0], f.uploaded[0], Transfer::single, [](Paint p) { return p != Paint::yellow; });
        auto mi = verify(f, missing, img[1]); check(!mi.l.pass && !mi.p.pass, "missing marker rejected");
        auto blob = img[0];
        for (uint32_t y = 0; y < blob.h / 4; ++y) for (uint32_t x = 0; x < blob.w / 4; ++x) { auto* p = blob.at(x, y); p[0] = p[1] = p[2] = 75; }
        auto bl = verify(f, blob, img[1]); check(!bl.l.pass && bl.l.grid_mismatch > 0, "corrupted background rejected");
        auto narrow = img[0]; PixelView pv = narrow.view(); pv.width -= 1;
        check(!verify_eye(f.eyes[0], f.uploaded[0], pv).pass, "readback dimension mismatch rejected");
        check(!verify_eye(f.eyes[0], f.uploaded[0], PixelView{}).pass, "null readback rejected");
        auto tm = f.uploaded[0]; tm[5] = std::nextafter(tm[5], 10.0f);
        check(!verify_eye(f.eyes[0], tm, img[0].view()).pass, "uploaded/CPU matrix mismatch rejected");
        // Zero IPD: eyes individually correct, disparity not testable.
        auto zero = head({0, 0, 0}, ident, 0.0, {-0.8, 0.8, 0.75, -0.75}, {-0.8, 0.8, 0.75, -0.75}, 400, 360);
        auto zi = render(zero); auto z = verify(zero, zi[0], zi[1]);
        check(z.l.pass && z.r.pass && !z.p.pass && z.p.reason.find("IPD") != std::string::npos, "zero IPD rejected by pair check");
        // Runtime eye labels reversed: pixels match the labels, baseline sign does not.
        auto rev = head({0, 0, 0}, ident, -0.032, {-0.8, 0.8, 0.75, -0.75}, {-0.8, 0.8, 0.75, -0.75}, 400, 360);
        auto ri = render(rev); auto rv = verify(rev, ri[0], ri[1]);
        check(rv.l.pass && rv.r.pass && !rv.p.pass && rv.p.reason.find("left view") != std::string::npos, "reversed eye labels rejected");
        // Looking backwards: known points out of view.
        auto back = head({0, 0, 0}, axis(1, 3.14159), 0.032, {-0.8, 0.8, 0.75, -0.75}, {-0.8, 0.8, 0.75, -0.75}, 400, 360);
        auto bi = render(back); check(!verify(back, bi[0], bi[1]).l.pass, "known points out of view rejected");
        check(scene_hash() == scene_hash() && vertex_count == 30, "stable synthetic scene identity");
        check(classify(std::array<uint8_t, 4>{0, 0, 0, 255}.data()) == Paint::unknown, "unknown colour is not background");
    }
    // ---------- shared readback guard ----------
    check(readback_refusal(true, true, true, true, false) == nullptr, "completed copied held image readable");
    check(readback_refusal(true, true, false, true, false) != nullptr, "copy before completion refused");
    check(readback_refusal(true, false, false, true, false) != nullptr, "unsubmitted copy refused");
    check(readback_refusal(true, true, true, false, false) != nullptr, "no recorded copy refused");
    check(readback_refusal(false, true, true, true, true) != nullptr, "released image refused");
    // ---------- owner/Run with fake receipts ----------
    {
        Fixture f; auto r = f.run();
        check(r.success && r.clean && r.stereo_pass && r.image_exit_code == 0 && r.begun == 180, "card d run passes after 120 a1 frames");
        check(f.fake.projection_calls == 60 && f.fake.zero_calls == 120 && f.fake.destroyed, "60 verified two-view projections");
        check(f.fake.n(ImageOp::readback) == 120 && f.fake.pixel_calls == 120 && f.fake.synthetic_calls == 60, "one readback per eye per frame");
        check(f.has("SYNTHETIC_STEREO_RESULT=PASS ") && !f.has("SYNTHETIC_STEREO_RESULT=FAIL"), "PASS verdict line");
        check(f.count("stereo_pair frame=") == 60 && f.count("stereo_eye frame=") == 120, "every frame verified and logged");
        check(f.count("stereo_marker ") == 8 && f.count("stereo_view ") == 2 && f.has("synthetic_scene id="), "first-frame detail and scene identity logged");
        check(f.has("card=d phase=synthetic-stereo"), "card d phase marker");
    }
    {
        Fixture f; f.fake.failure = ImageOp::readback; auto r = f.run();
        check(!r.success && !r.stereo_pass && r.image_exit_code == 19, "readback Map failure -> code 19");
        check(f.fake.projection_calls == 0 && f.fake.releases[0] == 1 && f.fake.destroyed && r.clean, "failed Map: completed image still released legally");
        check(f.has("SYNTHETIC_STEREO_RESULT=FAIL "), "FAIL verdict after map failure");
    }
    {
        Fixture f; f.fake.corrupt = [](unsigned eye, Image& img) { if (eye == 1) img = shifted(img, 5, 0); };
        auto r = f.run();
        check(!r.success && !r.stereo_pass && r.image_exit_code == 20 && f.fake.projection_calls == 0, "readback mismatch -> code 20, frame not submitted");
        check(f.has("SYNTHETIC_STEREO_RESULT=FAIL reason=frame 121: eye 1 "), "verdict names the failing eye");
        check(f.fake.releases[0] == 1 && f.fake.releases[1] == 1 && r.clean, "both images released before verification failure");
    }
    {
        Fixture f; f.fake.views = head({0, 0, 0}, ident, 0.0, {-0.8, 0.8, 0.75, -0.75}, {-0.8, 0.8, 0.75, -0.75}, 400, 360);
        auto r = f.run(); check(r.image_exit_code == 20 && !r.stereo_pass && f.has("SYNTHETIC_STEREO_RESULT=FAIL"), "zero located IPD fails verification");
    }
    {
        Fixture f; f.fake.pending = true; auto r = f.run();
        check(f.fake.n(ImageOp::readback) == 0 && r.image_exit_code == 15 && !r.stereo_pass, "copy is never read before completion");
        check(!r.clean && !f.fake.destroyed, "pending completion quarantines");
    }
    for (int which = 0; which < 2; ++which) {
        Fixture f; (which ? f.fake.copied : f.fake.restored) = false; auto r = f.run();
        check(r.image_exit_code == 21 && f.fake.n(ImageOp::close) == 0 && f.fake.n(ImageOp::execute) == 0, "unproven state restore/copy -> 21, never submitted");
        check(f.fake.releases[0] == 1 && r.clean && !r.stereo_pass, "known-unsubmitted image released legally");
    }
    {
        Fixture f; f.fake.setup_synthetic_failure = true; auto r = f.run();
        check(r.image_exit_code == 18 && f.fake.n(ImageOp::acquire) == 0 && !r.stereo_pass, "synthetic resource setup -> 18");
    }
    {
        Fixture f; f.fake.record_synthetic_failure = true; auto r = f.run();
        check(r.image_exit_code == 18 && f.fake.n(ImageOp::execute) == 0, "synthetic record resource fault -> 18");
    }
    {
        Fixture f; f.fake.matrix_tamper = true; auto r = f.run();
        check(r.image_exit_code == 12 && f.fake.n(ImageOp::acquire) == 0, "backend/CPU matrix mismatch refused before acquire");
    }
    {
        Fixture f; f.fake.sample_tamper = true; auto r = f.run();
        check(r.image_exit_code == 12 && f.fake.n(ImageOp::acquire) == 0, "views not joined to located sample refused");
    }
    {
        Fixture f; f.fake.render_frames = false; auto r = f.run();
        check(r.success && !r.stereo_pass && f.fake.n(ImageOp::readback) == 0, "shouldRender=false: lifecycle ok but no verified frame");
        check(f.has("SYNTHETIC_STEREO_RESULT=FAIL reason=no synthetic frame was rendered and verified"), "no-frame verdict is FAIL");
    }
    {
        Fixture f; auto r = f.run(Mode::phase2_dim_clear);
        check(f.fake.n(ImageOp::readback) == 0 && f.fake.synthetic_calls == 0 && !f.has("SYNTHETIC_STEREO_RESULT"), "a2 unchanged: no synthetic/readback");
        check(!r.stereo_pass, "a2 never reports a stereo verdict");
    }
    {
        Fixture f; auto r = f.run(Mode::zero_layers);
        check(r.success && r.begun == 120, "a1 unchanged budget");
        for (auto n : f.fake.calls) check(n == 0, "a1 zero image operations");
    }
    // ---------- exit-code family ----------
    for (int code = 12; code <= 21; ++code) check(cause_exit(image_cause(code)) == code, "image/stereo failure code maps to its own exit code");
    check(image_cause(11) == Cause::none && image_cause(22) == Cause::none, "no other code is an image cause");
    check(std::string(cause_name(Cause::stereo_pixels)) == "stereo-pixel-verification", "category names");
    check(cause_exit(Cause::admission_cap) == 11 && cause_exit(Cause::image_teardown) == 17, "earlier codes unchanged");
    check(LogBounds::stereo_capacity > LogBounds::colour_capacity && LogBounds::colour_capacity > LogBounds::capacity, "card d log reserve");
    std::cout << checks << " synthetic stereo checks passed; software rasteriser only, no GPU/runtime parity\n";
}
