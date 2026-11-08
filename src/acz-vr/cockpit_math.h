#pragma once
// cockpit_math.h — C++ port of src/cockpit-math (ES module).
// Row-vector 4x4 matrices: index = row*4 + col, p' = [x,y,z,1] * M,
// translation in the last row. Double precision for computation; convert
// to float only at the GPU upload boundary.
//
// Matches the conventions in docs/proposals/pose-contract.md and the
// measured DrawCPT layout in E2: local->PS2screen +0, local->clip +64,
// local->world +256 (bytes), rotation rows length 16.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <array>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace acz {

using Mat4 = std::array<double, 16>;
using Vec3 = std::array<double, 3>;
using Vec4 = std::array<double, 4>;
using Quat = std::array<double, 4>; // x, y, z, w

inline void requireFiniteMatrix(const Mat4& m) {
    for (double v : m) if (!std::isfinite(v))
        throw std::domain_error("matrix must contain only finite values");
}

// ── 4x4 helpers ──

inline Mat4 identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

inline Mat4 translation(double x, double y, double z) {
    Mat4 m = identity();
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

// Row-vector multiply: p' = p * a then p' * b. multiply(a, b) applies a first.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r*4+k] * b[k*4+c];
            out[r*4+c] = s;
        }
    return out;
}

inline Mat4 transpose(const Mat4& m) {
    Mat4 out{};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[c*4+r] = m[r*4+c];
    return out;
}

// General inverse (Gauss-Jordan with partial pivoting). Throws on singular.
inline Mat4 inverse(const Mat4& m) {
    requireFiniteMatrix(m);
    std::array<double, 16> a;
    for (int i = 0; i < 16; i++) a[i] = m[i];
    Mat4 inv = identity();

    double scale = 0;
    for (int i = 0; i < 16; i++) scale = std::max(scale, std::abs(a[i]));
    if (!(scale > 0) || !std::isfinite(scale))
        throw std::domain_error("matrix is zero or non-finite");

    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (std::abs(a[r*4+col]) > std::abs(a[piv*4+col])) piv = r;
        if (std::abs(a[piv*4+col]) <= scale * 1e-14)
            throw std::domain_error("matrix is singular");
        if (piv != col) {
            for (int c = 0; c < 4; c++) {
                std::swap(a[col*4+c], a[piv*4+c]);
                std::swap(inv[col*4+c], inv[piv*4+c]);
            }
        }
        double d = a[col*4+col];
        for (int c = 0; c < 4; c++) { a[col*4+c] /= d; inv[col*4+c] /= d; }
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            double f = a[r*4+col];
            if (f == 0) continue;
            for (int c = 0; c < 4; c++) {
                a[r*4+c] -= f * a[col*4+c];
                inv[r*4+c] -= f * inv[col*4+c];
            }
        }
    }
    requireFiniteMatrix(inv);
    return inv;
}

// Row-vector transform of [x, y, z, w]; no perspective divide.
inline Vec4 transformVec4(const Mat4& m, const Vec4& v) {
    Vec4 out{};
    for (int c = 0; c < 4; c++)
        out[c] = v[0]*m[c] + v[1]*m[4+c] + v[2]*m[8+c] + v[3]*m[12+c];
    return out;
}

inline Vec4 transformPoint(const Mat4& m, const Vec3& p) {
    return transformVec4(m, {p[0], p[1], p[2], 1.0});
}

// ── Quaternion ──

// Unit quaternion (x,y,z,w) -> rotation as row-vector matrix.
// Matches the JS: the transpose of the column-vector rotation.
inline Mat4 quatToMat4(const Quat& q) {
    for (auto v : q) if (!std::isfinite(v))
        throw std::invalid_argument("quaternion components must be finite");
    double n = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
    if (!(n > 0)) throw std::domain_error("zero-length quaternion");
    double x = q[0]/n, y = q[1]/n, z = q[2]/n, w = q[3]/n;
    double r00 = 1 - 2*(y*y + z*z), r01 = 2*(x*y - z*w), r02 = 2*(x*z + y*w);
    double r10 = 2*(x*y + z*w),     r11 = 1 - 2*(x*x + z*z), r12 = 2*(y*z - x*w);
    double r20 = 2*(x*z - y*w),     r21 = 2*(y*z + x*w),      r22 = 1 - 2*(x*x + y*y);
    // Row-vector: transpose of column-vector rotation.
    return Mat4{{
        r00, r10, r20, 0,
        r01, r11, r21, 0,
        r02, r12, r22, 0,
        0,   0,   0,   1
    }};
}

// ── Pose ──

struct Pose {
    Vec3 position; // metres
    Quat orientation; // x, y, z, w
};

// Pose (eye -> world) as row-vector matrix.
inline Mat4 poseToWorld(const Pose& pose) {
    for (auto v : pose.position) if (!std::isfinite(v))
        throw std::invalid_argument("position must be finite");
    Mat4 rot = quatToMat4(pose.orientation);
    return multiply(rot, translation(pose.position[0], pose.position[1], pose.position[2]));
}

// View matrix (world -> eye) in closed form: T(-p) * R^T.
inline Mat4 viewMatrixFromPose(const Pose& pose) {
    for (auto v : pose.position) if (!std::isfinite(v))
        throw std::invalid_argument("position must be finite");
    Mat4 rot = quatToMat4(pose.orientation);
    return multiply(
        translation(-pose.position[0], -pose.position[1], -pose.position[2]),
        transpose(rot)
    );
}

// ── Projection ──

struct FovAngles {
    double angleLeft, angleRight, angleUp, angleDown; // radians
};

struct DepthTerms {
    double A, B;
};

inline DepthTerms depthTerms(double nearZ, double farZ,
                             double zNdcNear = 0.0, double zNdcFar = 1.0) {
    if (!(nearZ > 0) || !std::isfinite(nearZ))
        throw std::domain_error("near must be finite > 0");
    if (!(farZ > nearZ)) throw std::domain_error("far must be > near");
    if (!std::isfinite(zNdcNear) || !std::isfinite(zNdcFar) || zNdcNear == zNdcFar)
        throw std::domain_error("depth range must be two distinct finite values");
    if (std::isinf(farZ)) return { zNdcFar, (zNdcFar - zNdcNear) * nearZ };
    double B = ((zNdcFar - zNdcNear) * farZ * nearZ) / (farZ - nearZ);
    double A = zNdcFar + B / farZ;
    return { A, B };
}

// Asymmetric clip projection matching the measured form (E4 camera block):
//   w = forward distance (column 3 is the depth axis)
//   z_c = A*w - B  (z ~ w minus a constant)
// flipY=true matches the measured negative y_c scale.
inline Mat4 projectionFromFov(const FovAngles& fov, double nearZ, double farZ,
                              double zNdcNear = 0.0, double zNdcFar = 1.0,
                              bool flipY = true) {
    constexpr double halfPi = 1.5707963267948966;
    double angles[] = { fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown };
    for (auto a : angles) {
        if (!std::isfinite(a)) throw std::invalid_argument("fov angles must be finite");
        if (std::abs(a) >= halfPi) throw std::domain_error("fov angles must be in (-pi/2, pi/2)");
    }
    if (!(fov.angleRight > fov.angleLeft) || !(fov.angleUp > fov.angleDown))
        throw std::domain_error("fov must satisfy right > left and up > down");

    auto dt = depthTerms(nearZ, farZ, zNdcNear, zNdcFar);
    double tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight);
    double tu = std::tan(fov.angleUp),   td = std::tan(fov.angleDown);
    double sy = flipY ? -1.0 : 1.0;

    Mat4 m{};
    m[0]  = 2.0 / (tr - tl);
    m[8]  = (tr + tl) / (tr - tl);
    m[5]  = (sy * 2.0) / (tu - td);
    m[9]  = (sy * (tu + td)) / (tu - td);
    m[10] = -dt.A;
    m[14] = -dt.B;
    m[11] = -1.0;
    return m;
}

// ── PS2 viewport ──

constexpr double PS2_CENTRE = 2048.0;
constexpr double PS2_SCALE_X = 258.048;
constexpr double PS2_SCALE_Y = 225.792;

struct PS2Depth {
    double fromClipZ = 0;
    double fromW = 0;
};

inline Mat4 ps2ViewportMatrix(double scaleX = PS2_SCALE_X, double scaleY = PS2_SCALE_Y,
                              double centre = PS2_CENTRE, PS2Depth depth = {}) {
    Mat4 m{};
    m[0]  = scaleX;
    m[12] = centre;
    m[5]  = scaleY;
    m[13] = centre;
    m[10] = depth.fromClipZ;
    m[14] = depth.fromW;
    m[15] = 1.0;
    return m;
}

// ── Camera recovery / decomposition / per-eye substitution ──

// VP = inverse(local_to_world) * local_to_clip.
inline Mat4 recoverCameraVP(const Mat4& localToWorld, const Mat4& localToClip) {
    return multiply(inverse(localToWorld), localToClip);
}

namespace detail {
    inline double dot3(const double* a, const double* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    }
    inline double norm3(const double* v) {
        return std::hypot(std::hypot(v[0], v[1]), v[2]);
    }
}

struct CameraDecomposition {
    Vec3 eye;
    Vec3 right, up, forward;
    double tanHalfFovX, tanHalfFovY;
    double forwardNorm;
    double depthSlopeA, depthOffsetC;
    double handedness;
    double axesOrthogonalityResidual;
};

// Decompose a symmetric-lens world_to_clip VP.
inline CameraDecomposition decomposeCameraVP(const Mat4& vp, bool flipY = true) {
    auto col = [&](int c) -> Vec4 {
        return { vp[c], vp[4+c], vp[8+c], vp[12+c] };
    };
    Vec4 c0 = col(0), c1 = col(1), c2 = col(2), c3 = col(3);
    double n0 = detail::norm3(c0.data());
    double n1 = detail::norm3(c1.data());
    double n3 = detail::norm3(c3.data());
    if (!(n0 > 0) || !(n1 > 0) || !(n3 > 0))
        throw std::domain_error("degenerate camera VP");

    Vec3 right   = { c0[0]/n0, c0[1]/n0, c0[2]/n0 };
    double sy = flipY ? -1.0 : 1.0;
    Vec3 up      = { sy*c1[0]/n1, sy*c1[1]/n1, sy*c1[2]/n1 };
    Vec3 forward = { c3[0]/n3, c3[1]/n3, c3[2]/n3 };

    // Solve eye * C = -t using Cramer's rule on 3x3 [c0 c1 c3] xyz rows.
    double C[3][3] = {
        { c0[0], c1[0], c3[0] },
        { c0[1], c1[1], c3[1] },
        { c0[2], c1[2], c3[2] }
    };
    double t[3] = { c0[3], c1[3], c3[3] };
    double A_cf = C[1][1]*C[2][2] - C[1][2]*C[2][1];
    double B_cf = -(C[1][0]*C[2][2] - C[1][2]*C[2][0]);
    double C_cf = C[1][0]*C[2][1] - C[1][1]*C[2][0];
    double det = C[0][0]*A_cf + C[0][1]*B_cf + C[0][2]*C_cf;
    if (!(std::abs(det) > 0)) throw std::domain_error("camera VP columns linearly dependent");

    double inv3[3][3] = {
        { A_cf/det, -(C[0][1]*C[2][2] - C[0][2]*C[2][1])/det, (C[0][1]*C[1][2] - C[0][2]*C[1][1])/det },
        { B_cf/det, (C[0][0]*C[2][2] - C[0][2]*C[2][0])/det, -(C[0][0]*C[1][2] - C[0][2]*C[1][0])/det },
        { C_cf/det, -(C[0][0]*C[2][1] - C[0][1]*C[2][0])/det, (C[0][0]*C[1][1] - C[0][1]*C[1][0])/det }
    };
    Vec3 eye{};
    for (int j = 0; j < 3; j++)
        eye[j] = -t[0]*inv3[0][j] - t[1]*inv3[1][j] - t[2]*inv3[2][j];

    double hdet = right[0]*(up[1]*forward[2] - up[2]*forward[1])
                - right[1]*(up[0]*forward[2] - up[2]*forward[0])
                + right[2]*(up[0]*forward[1] - up[1]*forward[0]);

    double A_depth = detail::dot3(c2.data(), c3.data()) / (n3*n3);
    double offCentre = std::max({
        std::abs(detail::dot3(right.data(), forward.data())),
        std::abs(detail::dot3(up.data(), forward.data())),
        std::abs(detail::dot3(right.data(), up.data()))
    });

    return {
        eye, right, up, forward,
        1.0/n0, 1.0/n1, n3,
        A_depth, c2[3] - A_depth * c3[3],
        (hdet > 0) ? 1.0 : -1.0,
        offCentre
    };
}

struct SubstitutedDraw {
    Mat4 localToClip;
    Mat4 localToPs2Screen;
};

// Per-eye substitution: given a draw's local_to_world and an eye VP,
// produce local_to_clip and local_to_ps2_screen.
inline SubstitutedDraw substituteEye(const Mat4& localToWorld, const Mat4& eyeVP,
                                     double scaleX = PS2_SCALE_X,
                                     double scaleY = PS2_SCALE_Y,
                                     double centre = PS2_CENTRE,
                                     PS2Depth depth = {}) {
    Mat4 ltc = multiply(localToWorld, eyeVP);
    Mat4 ltps = multiply(ltc, ps2ViewportMatrix(scaleX, scaleY, centre, depth));
    return { ltc, ltps };
}

// ── Seated recentering (pose-contract policy) ──

struct RecenterState {
    Mat4 T_R_X = identity(); // recentered-tracking-to-reference transform
    bool valid = false;
};

// Extract yaw from horizontal head forward. Returns false if heading is unavailable
// (near-vertical, |h| <= threshold).
inline bool extractHeadingYaw(const Quat& orientation, double& yawOut,
                              double threshold = 1e-6) {
    if (!std::isfinite(threshold) || threshold < 0)
        throw std::invalid_argument("heading threshold must be finite and nonnegative");
    Mat4 rot = quatToMat4(orientation);
    // forward = Q * (0, 0, -1) = -column2 of column-vector rotation = -row2 of row-vec matrix
    double fx = -rot[8], fz = -rot[10];
    double h = std::hypot(fx, fz);
    if (h <= threshold) return false;
    yawOut = std::atan2(-fx, -fz);
    return true;
}

// Compute T_R_X from a valid head-center position and yaw.
// Row-vector form: translate to remove p0, then rotate to remove yaw.
inline Mat4 computeRecenter(const Vec3& p0, double yaw0) {
    for (auto v : p0) if (!std::isfinite(v))
        throw std::invalid_argument("recenter position must be finite");
    if (!std::isfinite(yaw0)) throw std::invalid_argument("recenter yaw must be finite");
    double cy = std::cos(-yaw0), sy_val = std::sin(-yaw0);
    Mat4 ry = identity();
    ry[0] = cy; ry[2] = -sy_val;
    ry[8] = sy_val; ry[10] = cy;
    // T_R_X = T(-p0) * Ry(-yaw0): first translate, then rotate.
    return multiply(translation(-p0[0], -p0[1], -p0[2]), ry);
}

// ── Float32 conversion for GPU upload ──

using Mat4f = std::array<float, 16>;

inline Mat4f toFloat32(const Mat4& m) {
    Mat4f out;
    for (int i = 0; i < 16; i++) out[i] = static_cast<float>(m[i]);
    return out;
}

inline Mat4 fromFloat32(const Mat4f& m) {
    Mat4 out;
    for (int i = 0; i < 16; i++) out[i] = static_cast<double>(m[i]);
    return out;
}

} // namespace acz
