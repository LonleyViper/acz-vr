#include "stereo_renderer.h"

namespace acz {

void StereoRenderer::setAircraftPose(const Mat4& T_W_A, const Mat4& T_A_S) {
    m_T_W_A = T_W_A;
    m_T_A_S = T_A_S;
}

void StereoRenderer::setDrawCPTCalibration(const Mat4& C_W_N) {
    m_C_W_N = C_W_N;
}

void StereoRenderer::computeEyeMatrices(const FrameState& frame, const Mat4& T_R_X,
                                          const StereoConfig& cfg) {
    m_eyes = {};
    if (!frame.shouldRender) return;
    requireFiniteMatrix(m_T_W_A);
    requireFiniteMatrix(m_T_A_S);
    requireFiniteMatrix(m_C_W_N);
    requireFiniteMatrix(T_R_X);
    // Check intermediate products too: finite operands can overflow, and
    // later arithmetic must not mask that failure before atomic publication.
    const auto checkedMultiply = [](const Mat4& a, const Mat4& b) {
        auto result = multiply(a, b);
        requireFiniteMatrix(result);
        return result;
    };
    std::array<PerEyeMatrices, 2> computed{};
    for (int i = 0; i < 2; i++) {
        if (!frame.eyes[i].valid) continue;

        const auto& eyeView = frame.eyes[i];

        // All stored transforms are ROW-vector maps. Eye -> tracking ->
        // recentered seat -> aircraft -> world (reverse of column notation).
        Mat4 T_X_E = poseToWorld(eyeView.pose);
        requireFiniteMatrix(T_X_E);
        Mat4 T_W_E = checkedMultiply(checkedMultiply(checkedMultiply(T_X_E, T_R_X), m_T_A_S), m_T_W_A);
        Mat4 V_E_W = inverse(T_W_E);
        Mat4 P = projectionFromFov(eyeView.fov, cfg.nearZ, cfg.farZ, 0.0, 1.0, cfg.flipY);
        requireFiniteMatrix(V_E_W);
        requireFiniteMatrix(P);
        // Calibration is already the row-vector native -> canonical-world map.
        Mat4 VP_N = checkedMultiply(checkedMultiply(m_C_W_N, V_E_W), P);

        computed[i] = { T_W_E, V_E_W, P, VP_N, true };
    }
    m_eyes = computed;
}

SubstitutedDraw StereoRenderer::substituteDraw(int eyeIdx, const CapturedDraw& draw) const {
    if (eyeIdx < 0 || eyeIdx >= 2 || !m_eyes[eyeIdx].valid)
        throw std::invalid_argument("eye matrices are unavailable");
    Mat4 ltw = fromFloat32(draw.localToWorld);
    return substituteEye(ltw, m_eyes[eyeIdx].VP_N);
}

std::array<std::vector<StereoRenderer::SrtDataReplacement>, 2>
StereoRenderer::substituteAllDraws(const DrawCPTFilter& filter) const {
    std::array<std::vector<SrtDataReplacement>, 2> result;
    const auto& draws = filter.draws();

    for (int eye = 0; eye < 2; eye++) {
        if (!m_eyes[eye].valid) continue;
        result[eye].reserve(draws.size());
        for (const auto& draw : draws) {
            auto sub = substituteDraw(eye, draw);
            result[eye].push_back({
                toFloat32(sub.localToPs2Screen),
                toFloat32(sub.localToClip)
            });
        }
    }
    return result;
}

} // namespace acz
