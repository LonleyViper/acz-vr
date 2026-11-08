#pragma once
// VSCREEN capture (DLL only; vscreen_capture_win.cpp; docs/world-render/vscreen.md section 3). Process-wide.
//   * on_present(): called by the existing Present vtable hook (ee_camera_probe_win.cpp) BEFORE the native Present, on the game's Present
//     thread. Only while configured (vscreen:1) AND requested by the XR consumer (capture_request() within the last second) it records ONE
//     private list on the swap chain's own queue: back buffer PRESENT -> COPY_SOURCE, CopyResource into a private staging texture, back buffer
//     back to PRESENT, then one full-screen draw of the staging texture into the slot's private R8G8B8A8_UNORM image (raw display values;
//     the XR swapchain is R8G8B8A8_UNORM_SRGB, so a byte copy shows them exactly as the monitor does). ORIGINAL ExecuteCommandLists +
//     Signal(private fence). Never waits; any failure skips the frame (after kMaxFailures: disabled for the process, logged once).
//   * capture_acquire()/capture_release(): the XR owner thread takes the newest COMPLETED image (CPU-observed fence; no queue wait) and
//     gives it back after its own copy completed. Private objects live for the process (never destroyed while a reader could use them).
#include <cstdint>
#include <string>
#include "vscreen.h"

namespace acz::vscreen {

struct CaptureImage {
    void* resource = nullptr;     // ID3D12Resource*, R8G8B8A8_UNORM, COMMON between uses; valid while HELD
    uint32_t width = 0, height = 0;
    uint32_t format = 0;          // DXGI_FORMAT (28)
    Acquired held;
};
struct CaptureStats {
    uint64_t presents = 0, wanted = 0, recorded = 0, skipped_busy = 0, skipped_no_slot = 0, failures = 0, recreated = 0;
    uint32_t src_w = 0, src_h = 0, src_format = 0, dst_w = 0, dst_h = 0;
    bool disabled = false;
    const char* last_failure = "none";
};

void capture_configure(const Options&) noexcept;                // once, before the first on_present that matters
// DXGI factory wrappers (d3d12_hook.cpp), after a SUCCESSFUL swap-chain creation whose pDevice is a DIRECT ID3D12CommandQueue: the queue
// (one reference kept for the process by the caller) and its device. The capture uses no other queue (vscreen.h QueueAssoc).
void note_swapchain_queue(const void* swapchain, void* queue, void* device) noexcept;
void capture_request() noexcept;                                // the XR consumer wants images (stamped with GetTickCount64)
void capture_set_extent(uint32_t w, uint32_t h) noexcept;      // the screen image size (the XR screen swapchain extent)
void on_present(void* swapchain) noexcept;                      // game Present thread, before the native Present
bool capture_acquire(uint64_t after, CaptureImage& out) noexcept;
void capture_release(const CaptureImage&) noexcept;
CaptureStats capture_stats() noexcept;
std::string capture_describe() noexcept;                        // one summary fragment for the XR log

} // namespace acz::vscreen
