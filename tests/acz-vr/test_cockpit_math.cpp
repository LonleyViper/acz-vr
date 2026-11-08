// test_cockpit_math.cpp — C++ port validation of cockpit_math.h.
// Tests match the JS cockpit-math test expectations.
// Build: cl /EHsc /std:c++17 /I../../src/acz-vr test_cockpit_math.cpp
// Run: test_cockpit_math.exe

#define _USE_MATH_DEFINES
#include <cmath>
#include "cockpit_math.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* name) {
    if (cond) {
        printf("  PASS: %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", name);
        g_fail++;
    }
}

static bool near(double a, double b, double tol = 1e-9) {
    return std::abs(a - b) <= tol;
}

static bool mat4Near(const acz::Mat4& a, const acz::Mat4& b, double tol = 1e-9) {
    for (int i = 0; i < 16; i++)
        if (!near(a[i], b[i], tol)) return false;
    return true;
}

static bool vec4Near(const acz::Vec4& a, const acz::Vec4& b, double tol = 1e-9) {
    for (int i = 0; i < 4; i++)
        if (!near(a[i], b[i], tol)) return false;
    return true;
}

static void test_identity_and_multiply() {
    printf("identity and multiply:\n");
    auto I = acz::identity();
    check(I[0] == 1 && I[5] == 1 && I[10] == 1 && I[15] == 1, "identity diagonal");
    check(I[1] == 0 && I[4] == 0, "identity off-diagonal");

    auto T = acz::translation(3, 4, 5);
    auto prod = acz::multiply(I, T);
    check(mat4Near(prod, T), "I * T = T");
    check(prod[12] == 3 && prod[13] == 4 && prod[14] == 5, "translation in last row");
}

static void test_inverse() {
    printf("inverse:\n");
    auto T = acz::translation(1, 2, 3);
    auto inv = acz::inverse(T);
    auto prod = acz::multiply(T, inv);
    check(mat4Near(prod, acz::identity()), "T * T^-1 = I");

    bool threw = false;
    try {
        acz::Mat4 singular{};
        acz::inverse(singular);
    } catch (...) { threw = true; }
    check(threw, "singular matrix throws");
}

static void test_transpose() {
    printf("transpose:\n");
    acz::Mat4 m{};
    m[1] = 7; m[4] = 0;
    auto t = acz::transpose(m);
    check(t[4] == 7 && t[1] == 0, "transpose swaps [0][1] and [1][0]");
}

static void test_quat_yaw90() {
    printf("quaternion yaw +90:\n");
    double s = std::sin(M_PI / 4), c = std::cos(M_PI / 4);
    acz::Quat q = { 0, s, 0, c };
    auto R = acz::quatToMat4(q);
    // Row-vector matrix (transpose of column-vector).
    // Column-vector Ry(pi/2) rows: (0,0,1), (0,1,0), (-1,0,0).
    // Transposed (row-vector form): row0=(0,0,-1), row1=(0,1,0), row2=(1,0,0).
    check(near(R[0], 0, 1e-9) && near(R[1], 0, 1e-9) && near(R[2], -1, 1e-9), "row 0");
    check(near(R[4], 0, 1e-9) && near(R[5], 1, 1e-9) && near(R[6], 0, 1e-9), "row 1");
    check(near(R[8], 1, 1e-9) && near(R[9], 0, 1e-9) && near(R[10], 0, 1e-9), "row 2");

    // Scaled quaternion (x3) should give same rotation.
    acz::Quat qs = { 0, 3*s, 0, 3*c };
    auto Rs = acz::quatToMat4(qs);
    check(mat4Near(R, Rs), "scaled quat gives same rotation");

    // Negated quaternion should give same rotation.
    acz::Quat qn = { 0, -s, 0, -c };
    auto Rn = acz::quatToMat4(qn);
    check(mat4Near(R, Rn), "negated quat gives same rotation");
}

static void test_view_matrix() {
    printf("view matrix from pose:\n");
    acz::Pose pose = { {1, 2, 3}, {0, 0, 0, 1} };
    auto V = acz::viewMatrixFromPose(pose);
    // Identity rotation, translation -p in last row of row-vector matrix.
    auto p = acz::transformPoint(V, {1, 2, 3});
    check(near(p[0], 0) && near(p[1], 0) && near(p[2], 0) && near(p[3], 1),
          "eye at pose position maps to origin");

    // A point 1m forward in world (-Z) from the eye.
    auto fw = acz::transformPoint(V, {1, 2, 2});
    check(near(fw[0], 0) && near(fw[1], 0) && near(fw[2], -1),
          "point 1m forward maps to (0,0,-1)");
}

static void test_symmetric_projection() {
    printf("symmetric projection (90 deg):\n");
    acz::FovAngles fov = { -M_PI/4, M_PI/4, M_PI/4, -M_PI/4 };
    auto P = acz::projectionFromFov(fov, 0.1, 100.0, 0.0, 1.0, false);
    // tan(45°) = 1, so 2/(r-l) = 2/2 = 1.
    check(near(P[0], 1.0), "Pxx = 1 for 90 deg");
    check(near(P[5], 1.0), "Pyy = 1 for 90 deg (no flip)");
    check(near(P[8], 0.0), "no off-centre x");
    check(near(P[11], -1.0), "perspective w = -1");

    // Point at z=-1: ndc = (0, 0, depth)
    auto pt = acz::transformVec4(P, {0, 0, -1, 1});
    check(near(pt[3], 1.0), "w = 1 at z = -1");
}

static void test_asymmetric_projection() {
    printf("asymmetric projection:\n");
    // Tangents (l,r,b,t) = (-1, 2, -0.5, 1.5)
    double al = std::atan(-1.0), ar = std::atan(2.0);
    double ad = std::atan(-0.5), au = std::atan(1.5);
    acz::FovAngles fov = { al, ar, au, ad };
    auto P = acz::projectionFromFov(fov, 0.1, 100.0, 0.0, 1.0, false);
    check(near(P[0], 2.0/3.0, 1e-9), "Pxx = 2/3");
    check(near(P[8], 1.0/3.0, 1e-9), "off-centre x = 1/3");

    // Left edge at z=-2: x = -1 * 2 = -2 -> ndc should be -1.
    auto left = acz::transformVec4(P, {-2, 0, -2, 1});
    check(near(left[0] / left[3], -1.0, 1e-9), "left edge -> ndc -1");

    // Right edge at z=-2: x = 2 * 2 = 4 -> ndc should be +1.
    auto right = acz::transformVec4(P, {4, 0, -2, 1});
    check(near(right[0] / right[3], 1.0, 1e-9), "right edge -> ndc +1");
}

static void test_ps2_viewport() {
    printf("PS2 viewport matrix:\n");
    auto V = acz::ps2ViewportMatrix();
    check(near(V[0], 258.048), "scaleX");
    check(near(V[5], 225.792), "scaleY");
    check(near(V[12], 2048.0), "centreX");
    check(near(V[13], 2048.0), "centreY");

    // Centre of clip (0,0,0,1) -> (2048, 2048, 0, 1).
    auto c = acz::transformVec4(V, {0, 0, 0, 1});
    check(near(c[0], 2048.0) && near(c[1], 2048.0), "clip centre -> PS2 (2048, 2048)");
}

static void test_camera_recovery() {
    printf("camera recovery:\n");
    // Synthetic: model = translation(1,0,0), VP = translation(0,0,-5).
    auto model = acz::translation(1, 0, 0);
    auto vp = acz::translation(0, 0, -5);
    auto ltc = acz::multiply(model, vp);
    auto recovered = acz::recoverCameraVP(model, ltc);
    check(mat4Near(recovered, vp, 1e-9), "recovered VP matches");
}

static void test_decompose_camera() {
    printf("camera decomposition:\n");
    // Build a VP from a known eye and lens.
    acz::Pose eyePose = { {0, 0, 0}, {0, 0, 0, 1} };
    auto V = acz::viewMatrixFromPose(eyePose);
    double h = 0.6152, v = 0.3460;
    acz::FovAngles fov = { -std::atan(h), std::atan(h), std::atan(v), -std::atan(v) };
    auto P = acz::projectionFromFov(fov, 2.0, 65536.0, 0.0, 1.0, true);
    auto VP = acz::multiply(V, P);

    auto dec = acz::decomposeCameraVP(VP, true);
    check(near(dec.tanHalfFovX, h, 1e-3), "tanHalfFovX ~ 0.6152");
    check(near(dec.tanHalfFovY, v, 1e-3), "tanHalfFovY ~ 0.3460");
    check(near(dec.eye[0], 0) && near(dec.eye[1], 0) && near(dec.eye[2], 0),
          "eye at origin");
}

static void test_per_eye_substitution() {
    printf("per-eye substitution:\n");
    auto model = acz::translation(5, 0, 0);
    auto vp1 = acz::translation(0, 0.032, 0);
    auto vp2 = acz::translation(0, -0.032, 0);
    auto sub1 = acz::substituteEye(model, vp1);
    auto sub2 = acz::substituteEye(model, vp2);
    // The two eyes should produce different local->clip.
    bool differ = false;
    for (int i = 0; i < 16; i++)
        if (std::abs(sub1.localToClip[i] - sub2.localToClip[i]) > 1e-12) differ = true;
    check(differ, "two eyes produce different clips (parallax)");
}

static void test_recenter() {
    printf("seated recentering:\n");
    // Identity recenter.
    acz::Vec3 origin = {0, 0, 0};
    auto trx = acz::computeRecenter(origin, 0.0);
    check(mat4Near(trx, acz::identity()), "identity recenter");

    // Nonidentity: p0 = (1, 2, 3), yaw = pi/2.
    acz::Vec3 p0 = {1, 2, 3};
    auto trx2 = acz::computeRecenter(p0, M_PI / 2);
    // After recenter, the head at p0 with yaw pi/2 should map to origin with no yaw.
    // T_R_X * p0 should be near zero.
    auto p = acz::transformPoint(trx2, {1, 2, 3});
    check(near(p[0], 0, 1e-9) && near(p[1], 0, 1e-9) && near(p[2], 0, 1e-9),
          "recenter maps head position to origin");
}

static void test_heading_extraction() {
    printf("heading extraction:\n");
    double s = std::sin(M_PI / 4), c = std::cos(M_PI / 4);
    acz::Quat q90 = { 0, s, 0, c }; // Ry(pi/2)
    double yaw;
    bool ok = acz::extractHeadingYaw(q90, yaw);
    check(ok, "heading available for yaw=pi/2");
    check(near(yaw, M_PI / 2, 1e-9), "extracted heading = pi/2");

    // Near-vertical (pitch ~ pi/2): heading unavailable.
    double sp = std::sin(M_PI / 4); // Rx(pi/2) as quat: (sin(pi/4), 0, 0, cos(pi/4))
    acz::Quat qVert = { sp, 0, 0, c };
    ok = acz::extractHeadingYaw(qVert, yaw);
    check(!ok, "heading unavailable at pitch=pi/2");
}

static void test_float32_conversion() {
    printf("float32 conversion:\n");
    auto m = acz::translation(1.5, 2.5, 3.5);
    auto f = acz::toFloat32(m);
    auto back = acz::fromFloat32(f);
    check(mat4Near(m, back, 1e-6), "double -> float32 -> double round-trips");
}

int main() {
    printf("cockpit_math C++ tests\n\n");

    test_identity_and_multiply();
    test_inverse();
    test_transpose();
    test_quat_yaw90();
    test_view_matrix();
    test_symmetric_projection();
    test_asymmetric_projection();
    test_ps2_viewport();
    test_camera_recovery();
    test_decompose_camera();
    test_per_eye_substitution();
    test_recenter();
    test_heading_extraction();
    test_float32_conversion();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
