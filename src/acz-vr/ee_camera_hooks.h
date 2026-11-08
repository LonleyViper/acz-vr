#pragma once
// J-w1-ee-camera: the minimal additive seams the probe needs in existing hooks.
// Header-only (no link dependency for fixtures that include d3d12_hook.cpp).
// Default OFF: g_seam_hooks stays null unless ee_camera_probe and its
// prerequisites are latched; each seam then costs one atomic load. The
// swapchain notification is set at DLL static initialisation by
// ee_camera_probe_win.cpp and itself returns after one sink lookup when the
// key is off.
#include <atomic>
#include <cstdint>

namespace acz::ee_camera {
struct SeamHooks {
    void (*first_member)(const uint8_t* srt) noexcept; // B: producer's first replayed member (Draw path)
    void (*execute)() noexcept;                        // C: ExecuteCommandLists entry
};
inline std::atomic<const SeamHooks*> g_seam_hooks{nullptr};
using SwapchainHook=void(*)(void* swapchain) noexcept;
inline std::atomic<SwapchainHook> g_swapchain_hook{nullptr};

// rev 4: end-of-run report (set only when armed; called on a DXGI factory's
// final Release, the closest orderly seam the game has before exit).
using FinalHook=void(*)() noexcept;
inline std::atomic<FinalHook> g_final_hook{nullptr};
inline void notify_final() noexcept {
    if(auto f=g_final_hook.load(std::memory_order_acquire))f();
}
inline void notify_swapchain(void* swapchain) noexcept {
    if(auto f=g_swapchain_hook.load(std::memory_order_acquire))f(swapchain);
}
inline void notify_execute() noexcept {
    if(auto* h=g_seam_hooks.load(std::memory_order_acquire))h->execute();
}
inline void notify_first_member(const uint8_t* srt) noexcept {
    if(auto* h=g_seam_hooks.load(std::memory_order_acquire))h->first_member(srt);
}
} // namespace acz::ee_camera
