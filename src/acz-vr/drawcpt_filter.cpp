#include "drawcpt_filter.h"
#include <cstring>

namespace acz {

void DrawCPTFilter::registerDrawCPTPipeline(ID3D12PipelineState* pso) {
    std::lock_guard lock(m_mutex);
    m_drawCPTPSOs.insert(pso);
}

bool DrawCPTFilter::isDrawCPT(ID3D12PipelineState* pso) const {
    std::lock_guard lock(m_mutex);
    return m_drawCPTPSOs.count(pso) > 0;
}

void DrawCPTFilter::beginFrame() {
    std::lock_guard lock(m_mutex);
    m_draws.clear();
}

void DrawCPTFilter::captureDraw(const float* srtData,
                                 UINT indexOrVertexCount,
                                 UINT instanceCount,
                                 bool indexed,
                                 UINT startIndexOrVertex,
                                 INT baseVertex,
                                 UINT startInstance) {
    std::lock_guard lock(m_mutex);
    CapturedDraw d{};
    // SrtData layout (bytes): +0 local->PS2screen, +64 local->clip, +256 local->world
    // In floats: +0 = 0..15, +64/4 = 16..31, +256/4 = 64..79
    std::memcpy(d.localToPs2Screen.data(), srtData,      16 * sizeof(float));
    std::memcpy(d.localToClip.data(),      srtData + 16, 16 * sizeof(float));
    std::memcpy(d.localToWorld.data(),     srtData + 64, 16 * sizeof(float));
    d.indexOrVertexCount = indexOrVertexCount;
    d.instanceCount      = instanceCount;
    d.indexed            = indexed;
    d.startIndexOrVertex = startIndexOrVertex;
    d.baseVertex         = baseVertex;
    d.startInstance      = startInstance;
    m_draws.push_back(d);
}

bool DrawCPTFilter::recoverFrameVP(acz::Mat4& vpOut) const {
    std::lock_guard lock(m_mutex);
    for (const auto& d : m_draws) {
        try {
            Mat4 ltw = fromFloat32(d.localToWorld);
            Mat4 ltc = fromFloat32(d.localToClip);
            vpOut = recoverCameraVP(ltw, ltc);
            return true;
        } catch (...) {
            // Singular (e.g. eid 1534); try next draw.
            continue;
        }
    }
    return false;
}

} // namespace acz
