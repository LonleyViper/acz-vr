#pragma once
// Real OpenXR/D3D12 backend for xr_owner::Owner. Linked into dxgi.dll since
// J-xr-stereo-output, where only xr_stereo_proxy.cpp constructs it, and only
// with the xr_stereo_output keys on. Never referenced by DllMain or a hook.
#include "xr_owner.h"
#include <memory>

struct ID3D12Device;
struct ID3D12CommandQueue;
namespace acz::xr_stereo { struct SwapchainRuntime; }

namespace acz::xr_owner {
// ADOPTS one already-owned COM reference to each of the game's device and
// DIRECT queue (the capturing hook AddRefs them inside its own admission
// interval and transfers them here). They are released only by
// Runtime::release_binding(), after the session is destroyed. The destructor
// makes NO foreign call: unretired handles/references are deliberately leaked.
std::shared_ptr<Runtime> make_openxr_runtime(ID3D12Device* adopted_device, ID3D12CommandQueue* adopted_queue);
// Same adoption rule. Both pointers share ONE object: the swapchain facade
// operates on the owner's session (consumer calls stay on the owner thread).
struct StereoRuntimes {
    std::shared_ptr<Runtime> runtime;
    std::shared_ptr<xr_stereo::SwapchainRuntime> swapchains;
};
StereoRuntimes make_openxr_stereo_runtime(ID3D12Device* adopted_device, ID3D12CommandQueue* adopted_queue);
}
