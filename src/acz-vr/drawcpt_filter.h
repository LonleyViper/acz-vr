#pragma once
// drawcpt_filter.h — Identifies and captures DrawCPT (cockpit) draws.
//
// From E2: the cockpit pass (#27) uses DrawCPT_v + DrawCPT_g shaders with a
// per-draw structured buffer (SrtData) at t10. The geometry shader reads:
//   +0   local -> PS2 screen (float4x4)
//   +64  local -> clip       (float4x4)
//   +256 local -> world      (float4x4, rotation rows length 16)
//
// We identify DrawCPT draws by PSO (geometry shader presence) and collect
// the per-draw matrices for per-eye substitution.

#include "cockpit_math.h"
#include <d3d12.h>
#include <vector>
#include <mutex>
#include <unordered_set>

namespace acz {

struct CapturedDraw {
    acz::Mat4f localToPs2Screen; // SrtData +0,   float4x4
    acz::Mat4f localToClip;      // SrtData +64,  float4x4
    acz::Mat4f localToWorld;     // SrtData +256, float4x4

    UINT  indexOrVertexCount;
    UINT  instanceCount;
    bool  indexed;
    UINT  startIndexOrVertex;
    INT   baseVertex; // only for indexed draws
    UINT  startInstance;
};

class DrawCPTFilter {
public:
    // Register a PSO's geometry-shader hash so we can identify DrawCPT draws.
    // Called during PSO creation interception (or manual registration).
    void registerDrawCPTPipeline(ID3D12PipelineState* pso);

    // Check whether a PSO is a registered DrawCPT pipeline.
    bool isDrawCPT(ID3D12PipelineState* pso) const;

    // Begin a new frame's capture. Clears the draw list.
    void beginFrame();

    // Record a DrawCPT draw with its SrtData buffer contents.
    // srtData points to the start of this draw's SrtData entry
    // (at least 320 bytes: five float4x4 + constants).
    void captureDraw(const float* srtData, UINT indexOrVertexCount,
                     UINT instanceCount, bool indexed,
                     UINT startIndexOrVertex = 0, INT baseVertex = 0,
                     UINT startInstance = 0);

    // End-of-frame: return the captured draws and the recovered camera VP.
    const std::vector<CapturedDraw>& draws() const { return m_draws; }
    size_t drawCount() const { return m_draws.size(); }

    // Recover the camera VP from the first invertible draw in this frame.
    // Returns false if no invertible draw exists.
    bool recoverFrameVP(acz::Mat4& vpOut) const;

private:
    std::vector<CapturedDraw>                     m_draws;
    std::unordered_set<ID3D12PipelineState*>      m_drawCPTPSOs;
    mutable std::mutex                             m_mutex;
};

} // namespace acz
