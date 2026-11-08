#pragma once
// stereo_renderer.h — Per-eye cockpit rendering coordination.
//
// Implements the cockpit-openxr-layer.md design:
//   T_W_E_row[i] = T_X_E_row[i] * T_R_X_row * T_A_S_row * T_W_A_row
//   VP_N[i]  = C_row * V_row[i] * P_row[i]
//   M64'[i]  = M[+256] * VP_N[i]
//
// World/sky/HUD quad composition and GPU replay remain unimplemented.

#include "cockpit_math.h"
#include "drawcpt_filter.h"
#include "openxr_session.h"
#include <d3d12.h>
#include <array>
#include <vector>

namespace acz {

struct StereoConfig {
    double nearZ    = 0.05;   // synthetic metre near plane; native depth is unresolved
    double farZ     = 1638.4; // synthetic metre far plane
    bool   flipY    = false;  // canonical OpenXR projection; native adaptation is separate
};

struct PerEyeMatrices {
    Mat4 T_W_E;       // world eye pose
    Mat4 V_E_W;       // view matrix (world -> eye)
    Mat4 P;           // asymmetric clip projection
    Mat4 VP_N;        // combined VP in DrawCPT N space
    bool valid = false;
};

class StereoRenderer {
public:
    // Set the aircraft and seat transforms for the current frame.
    // Row maps: T_W_A aircraft -> world, T_A_S seated reference -> aircraft.
    // These come from the game state (future: read from game memory).
    void setAircraftPose(const Mat4& T_W_A, const Mat4& T_A_S);

    // Set row-vector DrawCPT-to-world calibration C_W_N (no implicit transpose).
    // Until measured, this defaults to identity (synthetic scale).
    void setDrawCPTCalibration(const Mat4& C_W_N);

    // Compute per-eye matrices from the OpenXR view poses and the current
    // aircraft/seat/recenter state.
    // Throws on nonfinite inputs/results or other numerical failure; both eyes
    // remain invalid on failure. Positive infinite farZ is supported.
    void computeEyeMatrices(const FrameState& frame, const Mat4& T_R_X,
                            const StereoConfig& cfg = {});

    // Get the per-eye matrices.
    const PerEyeMatrices& eye(int i) const { return m_eyes.at(i); }

    // Substitute a single draw's matrices for one eye.
    // Returns the replacement M[+64] (local->clip) and M[+0] (local->PS2screen).
    SubstitutedDraw substituteDraw(int eyeIdx, const CapturedDraw& draw) const;

    // Substitute all captured draws for both eyes. Returns two vectors
    // (left, right) of replacement SrtData blocks ready for GPU upload.
    // Each entry contains only two replacement matrices, not a complete SrtData.
    // Native screen/depth reconstruction and upload layout remain unresolved.
    struct SrtDataReplacement {
        Mat4f localToPs2Screen; // replacement +0
        Mat4f localToClip;      // replacement +64
    };
    std::array<std::vector<SrtDataReplacement>, 2>
        substituteAllDraws(const DrawCPTFilter& filter) const;

private:
    Mat4 m_T_W_A = identity();
    Mat4 m_T_A_S = identity();
    Mat4 m_C_W_N = identity();
    std::array<PerEyeMatrices, 2> m_eyes{};
};

} // namespace acz
