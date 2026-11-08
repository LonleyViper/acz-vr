#pragma once
// openxr_session.h — OpenXR instance, session, and swapchain lifecycle.
//
// Creates an OpenXR session bound to the game's D3D12 device and queue,
// manages per-eye swapchains, and handles the frame/view submission loop.
// The pose-contract seated recentering policy is applied here.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "cockpit_math.h"
#include <d3d12.h>
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <string>
#include <vector>

namespace acz {

struct EyeView {
    Pose    pose;       // eye pose in tracking space (T_X_E[i])
    FovAngles fov;      // per-eye asymmetric FOV
    bool    valid;
};

struct FrameState {
    XrTime          predictedDisplayTime;
    XrDuration      predictedDisplayPeriod;
    bool            shouldRender;
    std::array<EyeView, 2> eyes;
};

// Facts queried from COM in production, supplied by a fake in offline tests.
struct GraphicsDeviceInfo {
    LUID adapterLuid{};
    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool queueUsesDevice = false;
};

// Injectable dispatch for offline tests; default dispatch uses the loader.
struct XRDispatch {
    PFN_xrCreateInstance xrCreateInstance = &::xrCreateInstance;
    PFN_xrGetSystem xrGetSystem = &::xrGetSystem;
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = &::xrGetInstanceProcAddr;
    PFN_xrCreateSession xrCreateSession = &::xrCreateSession;
    PFN_xrCreateReferenceSpace xrCreateReferenceSpace = &::xrCreateReferenceSpace;
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = &::xrEnumerateViewConfigurationViews;
    PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = &::xrEnumerateSwapchainFormats;
    PFN_xrCreateSwapchain xrCreateSwapchain = &::xrCreateSwapchain;
    PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = &::xrEnumerateSwapchainImages;
    PFN_xrDestroySwapchain xrDestroySwapchain = &::xrDestroySwapchain;
    PFN_xrDestroySpace xrDestroySpace = &::xrDestroySpace;
    PFN_xrDestroySession xrDestroySession = &::xrDestroySession;
    PFN_xrDestroyInstance xrDestroyInstance = &::xrDestroyInstance;
    PFN_xrPollEvent xrPollEvent = &::xrPollEvent;
    PFN_xrBeginSession xrBeginSession = &::xrBeginSession;
    PFN_xrEndSession xrEndSession = &::xrEndSession;
    PFN_xrWaitFrame xrWaitFrame = &::xrWaitFrame;
    PFN_xrBeginFrame xrBeginFrame = &::xrBeginFrame;
    PFN_xrLocateViews xrLocateViews = &::xrLocateViews;
    PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = &::xrAcquireSwapchainImage;
    PFN_xrWaitSwapchainImage xrWaitSwapchainImage = &::xrWaitSwapchainImage;
    PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = &::xrReleaseSwapchainImage;
    PFN_xrEndFrame xrEndFrame = &::xrEndFrame;
    bool (*queryGraphics)(ID3D12Device*, ID3D12CommandQueue*, D3D_FEATURE_LEVEL, GraphicsDeviceInfo&) = nullptr;
};

class OpenXRSession {
public:
    OpenXRSession();
    explicit OpenXRSession(const XRDispatch& dispatch);
    OpenXRSession(const OpenXRSession&) = delete;
    OpenXRSession& operator=(const OpenXRSession&) = delete;
    ~OpenXRSession();

    // Initialize OpenXR with the game's D3D12 device and queue.
    // Returns false on failure (no runtime, incompatible device, etc.).
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue);

    // Tear down the session and instance.
    void shutdown();

    // Poll XR events. Returns false if the session has ended.
    bool pollEvents();

    // Begin a frame: wait, get predicted views. Returns the frame state.
    // A true result owns one begun frame, including shouldRender=false.
    // Locate/validation failure after a new begin ends that frame internally.
    // Call-order rejection leaves an already-owned frame with its caller.
    // Caller must call endFrame() exactly once after a true result.
    bool beginFrame(FrameState& out);

    // Acquire a swapchain image for the given eye. Returns the D3D12
    // resource to render into and the image index.
    bool acquireSwapchainImage(int eye, ID3D12Resource** outResource, uint32_t* outIndex);

    // Release the swapchain image for the given eye after rendering.
    bool releaseSwapchainImage(int eye);

    // Explicit extension: renderer calls this only after GPU work is complete,
    // before release. Acquire/release alone never counts as rendered.
    bool markEyeRendered(int eye);
    XrResult lastResult() const { return m_lastResult; }
    int64_t swapchainFormat() const { return m_format; }

    // End a begun frame, submitting only two completed and released eyes.
    bool endFrame(const FrameState& state);

    // Apply seated recentering from the current head pose.
    void recenter();

    // Get the current recenter transform T_R_X.
    const Mat4& getRecenterTransform() const { return m_recenter.T_R_X; }

    bool isSessionRunning() const { return m_sessionRunning; }

    // Per-eye render target dimensions.
    uint32_t eyeWidth() const { return m_eyeWidth; }
    uint32_t eyeHeight() const { return m_eyeHeight; }
    uint32_t eyeWidth(int eye) const { return m_swapchains.at(eye).width; }
    uint32_t eyeHeight(int eye) const { return m_swapchains.at(eye).height; }

private:
    XRDispatch m_api;
    XrResult m_lastResult = XR_SUCCESS;
    bool check(XrResult result) { m_lastResult = result; return XR_SUCCEEDED(result); }
    bool m_frameBegun = false;
    bool m_viewsValid = false;
    bool m_frameFailed = false;
    XrResult m_frameFailure = XR_SUCCESS;
    void failFrame(XrResult result) { m_frameFailed = true; m_frameFailure = m_lastResult = result; }
    FrameState m_frame{};
    int64_t m_format = 0;
    XrInstance        m_instance        = XR_NULL_HANDLE;
    XrSystemId        m_systemId        = XR_NULL_SYSTEM_ID;
    XrSession         m_session         = XR_NULL_HANDLE;
    XrSpace           m_referenceSpace  = XR_NULL_HANDLE;
    XrSessionState    m_sessionState    = XR_SESSION_STATE_UNKNOWN;
    bool              m_sessionRunning  = false;

    struct SwapchainInfo {
        XrSwapchain                   handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D12KHR> images;
        bool acquired = false;
        bool waited = false;
        bool rendered = false;
        bool released = false;
        uint32_t index = 0;
        uint32_t                      width  = 0;
        uint32_t                      height = 0;
    };
    std::array<SwapchainInfo, 2>  m_swapchains;
    std::array<XrView, 2>         m_views{};
    uint32_t                      m_eyeWidth  = 0;
    uint32_t                      m_eyeHeight = 0;

    RecenterState    m_recenter;
    ID3D12Device*    m_d3d12Device = nullptr;

    bool createSwapchains();
    bool handleSessionStateChange(XrSessionState newState);

    static Pose xrPoseToPose(const XrPosef& xrPose);
    static FovAngles xrFovToFov(const XrFovf& xrFov);
};

} // namespace acz
