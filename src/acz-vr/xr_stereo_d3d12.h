#pragma once
// Real D3D12 BlitBackend for xr_stereo::Output (J-xr-stereo-output). DLL only.
// Every device/list/queue/allocator call goes through acz::nativeDispatch (the
// saved original of any slot this proxy patched), so no proxy hook observes,
// registers or replays the consumer's own objects or recording.
#include "xr_stereo_output.h"
#include <memory>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace acz::xr_stereo {
// BORROWS the device and queue: the OpenXR runtime object adopted their
// references and releases them only after the session (and this backend's
// destroy(), which the owner's cleanup runs first) has retired.
std::shared_ptr<BlitBackend> make_d3d12_blit(ID3D12Device* device, ID3D12CommandQueue* queue);
}
