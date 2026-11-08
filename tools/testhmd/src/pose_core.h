// acz_testhmd: pose-file parser and slew maths (no OpenVR/Windows dependency, unit-tested by testhmd_test).
//
// Conventions (SteamVR/OpenVR tracking space): +X right, +Y up, -Z forward, right-handed.
//   yaw   = rotation about +Y; POSITIVE yaw turns the head LEFT  (forward -Z -> -X).
//   pitch = rotation about +X; POSITIVE pitch looks UP           (forward -Z -> +Y).
//   roll  = rotation about +Z; POSITIVE roll tilts the head LEFT (up +Y -> -X, left ear down).
//   Orientation q = Ry(yaw) * Rx(pitch) * Rz(roll)   (intrinsic yaw, then pitch, then roll).
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace testhmd {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Quat { double w = 1, x = 0, y = 0, z = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };

inline Quat qmul(const Quat& a, const Quat& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
inline Quat qconj(const Quat& q) { return { q.w, -q.x, -q.y, -q.z }; }
inline double qdot(const Quat& a, const Quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
inline Quat qnorm(const Quat& q) {
    double n = std::sqrt(qdot(q, q));
    if (!(n > 1e-12)) return Quat{};
    return { q.w / n, q.x / n, q.y / n, q.z / n };
}
inline Quat qaxis(double ax, double ay, double az, double rad) {
    double s = std::sin(rad * 0.5);
    return { std::cos(rad * 0.5), ax * s, ay * s, az * s };
}
inline Vec3 qrot(const Quat& q, const Vec3& v) {
    Quat p{ 0, v.x, v.y, v.z };
    Quat r = qmul(qmul(q, p), qconj(q));
    return { r.x, r.y, r.z };
}

inline Quat quat_from_ypr_deg(double yaw, double pitch, double roll) {
    return qnorm(qmul(qmul(qaxis(0, 1, 0, yaw * kDeg), qaxis(1, 0, 0, pitch * kDeg)), qaxis(0, 0, 1, roll * kDeg)));
}

// Inverse of quat_from_ypr_deg (pitch in [-90, 90]); used for logging and the status line.
inline void ypr_deg_from_quat(const Quat& q, double& yaw, double& pitch, double& roll) {
    Vec3 f = qrot(q, { 0, 0, -1 });
    Vec3 xa = qrot(q, { 1, 0, 0 });
    Vec3 ya = qrot(q, { 0, 1, 0 });
    double fy = f.y > 1 ? 1 : (f.y < -1 ? -1 : f.y);
    pitch = std::asin(fy) / kDeg;
    yaw = std::atan2(-f.x, -f.z) / kDeg;
    roll = std::atan2(xa.y, ya.y) / kDeg;
}

// ---------------------------------------------------------------------------------------------
// Pose file: first non-empty, non-comment line ('#' starts a comment); separators: blanks, tabs, commas.
//   3 numbers: yaw pitch roll
//   4 numbers: yaw pitch roll slew
//   6 numbers: yaw pitch roll x y z
//   7 numbers: yaw pitch roll x y z slew
// Angles in degrees, position in metres (tracking space, see conventions), slew in deg/s
// (<= 0 snaps instantly). Anything else (other counts, non-numbers, non-finite, out of range)
// is rejected and the caller keeps its last target.
struct PoseTarget {
    double yaw = 0, pitch = 0, roll = 0;
    Vec3 pos{};
    double slew = 120.0;
    bool has_pos = false;
    bool has_slew = false;
};

inline bool parse_number(const std::string& tok, double& out) {
    if (tok.empty()) return false;
    const char* s = tok.c_str();
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s || *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// Returns true and fills 'out' on success. 'default_slew' is used when the line carries none.
// 'keep_pos' is the position kept when the line has no position.
inline bool parse_pose_text(const std::string& text, double default_slew, const Vec3& keep_pos, PoseTarget& out,
                            std::string* why = nullptr) {
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find_first_of("\r\n", i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line = line.substr(3);   // UTF-8 BOM (PowerShell 5 Set-Content -Encoding utf8)
        std::vector<std::string> toks;
        std::string cur;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == ',') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) toks.push_back(cur);
        if (toks.empty()) continue;   // blank or comment-only line

        size_t n = toks.size();
        if (n != 3 && n != 4 && n != 6 && n != 7) { if (why) *why = "field count " + std::to_string(n); return false; }
        double v[7] = {};
        for (size_t k = 0; k < n; ++k)
            if (!parse_number(toks[k], v[k])) { if (why) *why = "not a number: '" + toks[k] + "'"; return false; }
        PoseTarget t;
        t.yaw = v[0]; t.pitch = v[1]; t.roll = v[2];
        for (int k = 0; k < 3; ++k)
            if (std::fabs(v[k]) > 3600.0) { if (why) *why = "angle out of range"; return false; }
        t.pos = keep_pos;
        t.slew = default_slew;
        if (n >= 6) {
            t.pos = { v[3], v[4], v[5] }; t.has_pos = true;
            if (std::fabs(v[3]) > 10 || std::fabs(v[4]) > 10 || std::fabs(v[5]) > 10) { if (why) *why = "position out of range (|m| > 10)"; return false; }
        }
        if (n == 4 || n == 7) {
            t.slew = v[n - 1]; t.has_slew = true;
            if (t.slew > 100000.0) { if (why) *why = "slew out of range"; return false; }
        }
        out = t;
        return true;
    }
    if (why) *why = "empty";
    return false;
}

// ---------------------------------------------------------------------------------------------
// Slew state: the current orientation turns toward the target at a constant angular rate along the
// shortest arc; the position moves toward its target at a constant linear rate.
struct SlewState {
    Quat q{};             // current orientation
    Vec3 pos{};           // current position (m)
    Quat target_q{};
    Vec3 target_pos{};
    double slew_rad = 120.0 * kDeg;   // rad/s; <= 0 snaps
    double pos_rate = 1.0;            // m/s; <= 0 snaps
    Vec3 ang_vel_world{};             // rad/s, world (tracking) frame: dq/dt = 0.5 * w * q
    Vec3 ang_vel_local{};             // rad/s, head frame:             dq/dt = 0.5 * q * w
    Vec3 lin_vel{};                   // m/s
};

inline void set_target(SlewState& s, const PoseTarget& t) {
    s.target_q = quat_from_ypr_deg(t.yaw, t.pitch, t.roll);
    s.target_pos = t.pos;
    s.slew_rad = t.slew * kDeg;
}

// Advances by dt seconds. Returns the remaining angle to the target (rad) after the step.
inline double step(SlewState& s, double dt) {
    if (!(dt >= 0)) dt = 0;
    Quat tq = s.target_q;
    if (qdot(s.q, tq) < 0) tq = { -tq.w, -tq.x, -tq.y, -tq.z };   // shortest arc
    Quat d = qnorm(qmul(tq, qconj(s.q)));                         // world-frame delta: tq = d * q
    double dw = d.w > 1 ? 1 : (d.w < -1 ? -1 : d.w);
    double angle = 2.0 * std::acos(dw);
    double sn = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    double remaining = 0;
    if (angle < 1e-9 || sn < 1e-12 || s.slew_rad <= 0) {   // there already, or snap
        s.q = qnorm(tq);
        s.ang_vel_world = {}; s.ang_vel_local = {};
    } else {
        Vec3 ax{ d.x / sn, d.y / sn, d.z / sn };
        double stepang = s.slew_rad * dt;
        if (stepang >= angle) {
            s.q = qnorm(tq);
            s.ang_vel_world = {}; s.ang_vel_local = {};
        } else {
            s.q = qnorm(qmul(qaxis(ax.x, ax.y, ax.z, stepang), s.q));
            remaining = angle - stepang;
            s.ang_vel_world = { ax.x * s.slew_rad, ax.y * s.slew_rad, ax.z * s.slew_rad };
            Vec3 la = qrot(qconj(s.q), ax);
            s.ang_vel_local = { la.x * s.slew_rad, la.y * s.slew_rad, la.z * s.slew_rad };
        }
    }
    Vec3 dp{ s.target_pos.x - s.pos.x, s.target_pos.y - s.pos.y, s.target_pos.z - s.pos.z };
    double dist = std::sqrt(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z);
    if (dist < 1e-9 || s.pos_rate <= 0 || s.pos_rate * dt >= dist) {
        s.pos = s.target_pos; s.lin_vel = {};
    } else {
        double k = s.pos_rate * dt / dist;
        s.pos = { s.pos.x + dp.x * k, s.pos.y + dp.y * k, s.pos.z + dp.z * k };
        s.lin_vel = { dp.x / dist * s.pos_rate, dp.y / dist * s.pos_rate, dp.z / dist * s.pos_rate };
    }
    return remaining;
}

// LOCAL-ROLL FIX (docs/world-render/testhmd-first-flights.md section 3). The first live flights on this driver (step185) showed every head turn
// rendered MIRRORED in yaw and pitch: a pose file "90 0 0" (look left) gave the right side of the cockpit, "-90.6 53.3 0" (right and up) a view
// left and DOWN into the side console; at rest the eye images are upside down (the long-known "eye dumps / Headset Window rotated 180 deg" of
// the null runs). Both fit one cause: the OpenXR LOCAL space the game sees is the driver space turned 180 deg about the forward axis,
// head_local = Rz(180) * head_driver (a frame-side roll: it negates the yaw and the pitch of a turn and keeps the roll; a head-side roll would
// only rotate the image). Reporting Rz(180) * q * Rz(180)^-1 makes head_local = q_cmd * Rz(180): the commanded look direction exactly, while
// the rest pose keeps its 180 deg image roll (so every existing "rotate 180" analysis step stays valid). Conjugation by Rz(180) negates the
// x and y components of the quaternion; vectors (position, velocities) map the same way: (x, y, z) -> (-x, -y, z).
inline Quat local_roll_fix(const Quat& q) { return { q.w, -q.x, -q.y, q.z }; }
inline Vec3 local_roll_fix(const Vec3& v) { return { -v.x, -v.y, v.z }; }

inline bool same_target(const PoseTarget& a, const PoseTarget& b) {
    return a.yaw == b.yaw && a.pitch == b.pitch && a.roll == b.roll && a.pos.x == b.pos.x && a.pos.y == b.pos.y &&
           a.pos.z == b.pos.z && a.slew == b.slew;
}

}  // namespace testhmd
