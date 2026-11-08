#pragma once
// Proxy wiring for the XR stereo consumer (J-xr-stereo-output). DLL only.
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace acz::xr_stereo {
// True only when the latched diagnostics configuration has logging AND
// xr_stereo_output AND gpu_copy_early_device_hook AND gpu_copy_step5_continuous.
bool configured() noexcept;
// Device-ready seam (dllmain's onDeviceReady, called from swap-chain discovery
// on the game's thread, outside loader lock, after the proxy accepted the
// game's device and DIRECT present queue). Default OFF: returns after one sink
// lookup. ON (one arming per process): pin the DLL, AddRef device+queue, install
// the owner call bracket (proxy creation scope) and the pose provider, and
// start a monitor thread that polls (100 ms) for the producer's first published
// StereoFrame; only then is the XR owner built and launched, with ONE retry
// after a clean cold-start timeout. Never makes an XR call on the calling
// thread; never waits.
void on_device_ready(ID3D12Device*, ID3D12CommandQueue*) noexcept;
}
