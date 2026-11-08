#pragma once
namespace acz::gpu_copy::slots {
// Derived from SDK 10.0.26100.0 d3d12.h ID3D12GraphicsCommandList10Vtbl.
// The CINTERFACE fixture verifies these constants for every admitted revision.
constexpr int begin_query=52,end_query=53,resolve_query=54,predication=55,
    protected_session=67,begin_pass=68,end_pass=69;
}
