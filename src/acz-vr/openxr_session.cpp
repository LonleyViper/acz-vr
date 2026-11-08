#include "openxr_session.h"
#include <cstring>
#include <cmath>

namespace acz {

OpenXRSession::OpenXRSession() = default;
OpenXRSession::OpenXRSession(const XRDispatch& dispatch) : m_api(dispatch) {}

OpenXRSession::~OpenXRSession() {
    shutdown();
}

Pose OpenXRSession::xrPoseToPose(const XrPosef& xrPose) {
    return Pose{
        { xrPose.position.x, xrPose.position.y, xrPose.position.z },
        { xrPose.orientation.x, xrPose.orientation.y, xrPose.orientation.z, xrPose.orientation.w }
    };
}

FovAngles OpenXRSession::xrFovToFov(const XrFovf& xrFov) {
    return FovAngles{
        xrFov.angleLeft, xrFov.angleRight,
        xrFov.angleUp,   xrFov.angleDown
    };
}

bool OpenXRSession::init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    shutdown();
    if (!device || !queue) { m_lastResult = XR_ERROR_GRAPHICS_DEVICE_INVALID; return false; }
    m_d3d12Device = device;

    // Create XR instance.
    XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::strncpy(instanceInfo.applicationInfo.applicationName,
                 "ACZ VR", XR_MAX_APPLICATION_NAME_SIZE);
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);

    const char* extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;

    if (!check(m_api.xrCreateInstance(&instanceInfo, &m_instance))) { shutdown(); return false; }

    // Get system for HMD.
    XrSystemGetInfo sysInfo{ XR_TYPE_SYSTEM_GET_INFO };
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(m_api.xrGetSystem(m_instance, &sysInfo, &m_systemId))) {
        shutdown();
        return false;
    }

    // Check D3D12 graphics requirements.
    PFN_xrGetD3D12GraphicsRequirementsKHR getGfxReqs = nullptr;
    if (!check(m_api.xrGetInstanceProcAddr(m_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getGfxReqs))) || !getGfxReqs) {
        if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_FUNCTION_UNSUPPORTED;
        shutdown(); return false;
    }
    XrGraphicsRequirementsD3D12KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (!check(getGfxReqs(m_instance, m_systemId, &reqs))) { shutdown(); return false; }
    GraphicsDeviceInfo facts{};
    bool queried = false;
    if (m_api.queryGraphics) {
        queried = m_api.queryGraphics(device, queue, reqs.minFeatureLevel, facts);
    } else {
        facts.adapterLuid = device->GetAdapterLuid();
        facts.queueType = queue->GetDesc().Type;
        D3D_FEATURE_LEVEL level = reqs.minFeatureLevel;
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &level, D3D_FEATURE_LEVEL_11_0};
        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))) {
            IUnknown *a = nullptr, *b = nullptr;
            device->QueryInterface(IID_PPV_ARGS(&a));
            queueDevice->QueryInterface(IID_PPV_ARGS(&b));
            facts.queueUsesDevice = a && b && a == b;
            queried = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
            facts.maxFeatureLevel = levels.MaxSupportedFeatureLevel;
            if (a) a->Release();
            if (b) b->Release();
            queueDevice->Release();
        }
    }
    const bool compatible = queried && facts.queueUsesDevice &&
        facts.queueType == D3D12_COMMAND_LIST_TYPE_DIRECT &&
        facts.adapterLuid.LowPart == reqs.adapterLuid.LowPart &&
        facts.adapterLuid.HighPart == reqs.adapterLuid.HighPart &&
        facts.maxFeatureLevel >= reqs.minFeatureLevel;
    if (!compatible) { m_lastResult = XR_ERROR_GRAPHICS_DEVICE_INVALID; shutdown(); return false; }

    // Create session bound to the game's D3D12 device.
    XrGraphicsBindingD3D12KHR d3d12Binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    d3d12Binding.device = device;
    d3d12Binding.queue  = queue;

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.systemId = m_systemId;
    sessionInfo.next = &d3d12Binding;
    if (!check(m_api.xrCreateSession(m_instance, &sessionInfo, &m_session))) {
        shutdown();
        return false;
    }

    // Create LOCAL reference space (seated).
    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation = { 0, 0, 0, 1 };
    spaceInfo.poseInReferenceSpace.position    = { 0, 0, 0 };
    if (!check(m_api.xrCreateReferenceSpace(m_session, &spaceInfo, &m_referenceSpace))) {
        shutdown();
        return false;
    }

    if (!createSwapchains()) {
        shutdown();
        return false;
    }

    m_recenter.T_R_X = identity();
    m_recenter.valid = false;

    return true;
}

bool OpenXRSession::createSwapchains() {
    uint32_t viewCount = 0;
    if (!check(m_api.xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr)) || viewCount != 2) {
        if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_VALIDATION_FAILURE;
        return false;
    }
    std::array<XrViewConfigurationView, 2> views{};
    for (auto& v : views) v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    if (!check(m_api.xrEnumerateViewConfigurationViews(m_instance, m_systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views.data())) || viewCount != 2) {
        if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_VALIDATION_FAILURE;
        return false;
    }
    uint32_t formatCount = 0;
    if (!check(m_api.xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)) || !formatCount) {
        if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (!check(m_api.xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()))) return false;
    if (formatCount > formats.size()) { m_lastResult = XR_ERROR_VALIDATION_FAILURE; return false; }
    formats.resize(formatCount);
    // This implementation supports this format only; never guess a game target format.
    m_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (std::find(formats.begin(), formats.end(), m_format) == formats.end()) {
        m_lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        auto& sc = m_swapchains[eye];
        sc.width = views[eye].recommendedImageRectWidth;
        sc.height = views[eye].recommendedImageRectHeight;
        if (!sc.width || !sc.height || sc.width > INT32_MAX || sc.height > INT32_MAX) {
            m_lastResult = XR_ERROR_VALIDATION_FAILURE; return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = m_format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = sc.width; info.height = sc.height;
        if (!check(m_api.xrCreateSwapchain(m_session, &info, &sc.handle))) return false;
        uint32_t count = 0;
        if (!check(m_api.xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr)) || !count) {
            if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_VALIDATION_FAILURE;
            return false;
        }
        sc.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        const uint32_t capacity = count;
        if (!check(m_api.xrEnumerateSwapchainImages(sc.handle, capacity, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()))) || count != capacity) {
            if (XR_SUCCEEDED(m_lastResult)) m_lastResult = XR_ERROR_VALIDATION_FAILURE;
            return false;
        }
    }

    m_eyeWidth  = m_swapchains[0].width;
    m_eyeHeight = m_swapchains[0].height;

    return true;
}

void OpenXRSession::shutdown() {
    if (m_frameBegun) { m_frame.shouldRender = false; endFrame(m_frame); }
    for (auto& sc : m_swapchains) {
        if (sc.handle != XR_NULL_HANDLE) {
            m_api.xrDestroySwapchain(sc.handle);
            sc.handle = XR_NULL_HANDLE;
        }
        sc = {};
    }
    if (m_referenceSpace != XR_NULL_HANDLE) {
        m_api.xrDestroySpace(m_referenceSpace);
        m_referenceSpace = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE) {
        m_api.xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE) {
        m_api.xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }
    m_sessionRunning = false;
    m_sessionState = XR_SESSION_STATE_UNKNOWN;
    m_systemId = XR_NULL_SYSTEM_ID;
    m_frameBegun = m_viewsValid = m_frameFailed = false;
    m_frame = {};
    m_frameFailure = XR_SUCCESS;
    m_views = {};
    m_eyeWidth = m_eyeHeight = 0;
    m_format = 0;
    m_recenter = {};
    m_d3d12Device = nullptr;
}

bool OpenXRSession::pollEvents() {
    if (!m_instance) return false;
    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = m_api.xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (!check(result)) return false;
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            m_sessionRunning = m_viewsValid = false; return false;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* change = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (change->session == m_session && !handleSessionStateChange(change->state)) return false;
        }
    }
    return m_sessionState != XR_SESSION_STATE_EXITING && m_sessionState != XR_SESSION_STATE_LOSS_PENDING;
}

bool OpenXRSession::handleSessionStateChange(XrSessionState state) {
    m_sessionState = state;
    if (state == XR_SESSION_STATE_READY && !m_sessionRunning) {
        XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
        info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (!check(m_api.xrBeginSession(m_session, &info))) return false;
        m_sessionRunning = true;
    } else if (state == XR_SESSION_STATE_STOPPING) {
        bool frameOK = true;
        if (m_frameBegun) {
            m_frame.shouldRender = false;
            frameOK = endFrame(m_frame);
        }
        const auto frameError = m_lastResult;
        m_viewsValid = false;
        if (m_sessionRunning && !check(m_api.xrEndSession(m_session))) return false;
        m_sessionRunning = false;
        if (!frameOK) { m_lastResult = frameError; return false; }
    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
        m_sessionRunning = m_viewsValid = false;
    }
    return true;
}

bool OpenXRSession::beginFrame(FrameState& out) {
    out = {};
    if (!m_sessionRunning || m_frameBegun || m_swapchains[0].acquired || m_swapchains[1].acquired) {
        m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false;
    }
    m_viewsValid = false;
    m_frameFailed = false;
    m_frameFailure = XR_SUCCESS;
    for (auto& sc : m_swapchains) sc.rendered = sc.released = false;

    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    if (!check(m_api.xrWaitFrame(m_session, &waitInfo, &frameState)))
        return false;

    out.predictedDisplayTime   = frameState.predictedDisplayTime;
    out.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
    out.shouldRender           = frameState.shouldRender == XR_TRUE;

    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!check(m_api.xrBeginFrame(m_session, &beginInfo)))
        return false;

    m_frameBegun = true;
    m_frame = out;
    if (!out.shouldRender) return true;

    // Locate views.
    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = m_referenceSpace;

    XrViewState viewState{ XR_TYPE_VIEW_STATE };
    uint32_t viewCount = 2;
    m_views[0] = { XR_TYPE_VIEW };
    m_views[1] = { XR_TYPE_VIEW };
    if (!check(m_api.xrLocateViews(m_session, &locateInfo, &viewState, 2, &viewCount, m_views.data())) || viewCount != 2) {
        const XrResult failure = XR_FAILED(m_lastResult) ? m_lastResult : XR_ERROR_VALIDATION_FAILURE;
        out = {};
        // Caller will not end a failed beginFrame. Preserve a cleanup error if any.
        if (endFrame(m_frame)) m_lastResult = failure;
        return false;
    }

    bool posValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    bool oriValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

    if (!posValid || !oriValid) {
        out.shouldRender = false;
        m_frame = out;
        return true; // begun frame still belongs to caller, zero layers
    }
    try {
        for (const auto& v : m_views) {
            poseToWorld(xrPoseToPose(v.pose));
            projectionFromFov(xrFovToFov(v.fov), 0.05, 100.0);
        }
    } catch (const std::exception&) {
        out = {};
        if (endFrame(m_frame)) m_lastResult = XR_ERROR_VALIDATION_FAILURE;
        return false;
    }
    m_viewsValid = true;
    for (int i = 0; i < 2; i++) {
        out.eyes[i].pose  = xrPoseToPose(m_views[i].pose);
        out.eyes[i].fov   = xrFovToFov(m_views[i].fov);
        out.eyes[i].valid = posValid && oriValid;
    }

    m_frame = out;
    return true;
}

bool OpenXRSession::acquireSwapchainImage(int eye, ID3D12Resource** outResource,
                                           uint32_t* outIndex) {
    if (outResource) *outResource = nullptr;
    if (outIndex) *outIndex = 0;
    if (eye < 0 || eye >= 2 || !outResource || !outIndex || !m_frameBegun ||
        !m_frame.shouldRender || !m_viewsValid || m_frameFailed) {
        m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false;
    }
    auto& sc = m_swapchains[eye];
    if (sc.acquired || sc.released) { m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false; }
    XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(m_api.xrAcquireSwapchainImage(sc.handle, &info, &sc.index))) {
        failFrame(m_lastResult); return false;
    }
    sc.acquired = true;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    XrResult result = m_api.xrWaitSwapchainImage(sc.handle, &wait);
    // Timeout is a positive status, but the image is not ready and may not be released.
    if (result != XR_SUCCESS) {
        failFrame(result); return false;
    }
    sc.waited = true;
    if (sc.index >= sc.images.size() || !sc.images[sc.index].texture) {
        m_lastResult = XR_ERROR_VALIDATION_FAILURE; failFrame(m_lastResult); return false;
    }
    *outResource = sc.images[sc.index].texture;
    *outIndex = sc.index;
    return true;
}

bool OpenXRSession::markEyeRendered(int eye) {
    if (eye < 0 || eye >= 2 || !m_frameBegun || m_frameFailed ||
        !m_swapchains[eye].acquired || !m_swapchains[eye].waited) {
        m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false;
    }
    m_swapchains[eye].rendered = true;
    return true;
}

bool OpenXRSession::releaseSwapchainImage(int eye) {
    if (eye < 0 || eye >= 2 || !m_swapchains[eye].acquired || !m_swapchains[eye].waited) {
        m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false;
    }
    auto& sc = m_swapchains[eye];
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!check(m_api.xrReleaseSwapchainImage(sc.handle, &info))) {
        failFrame(m_lastResult); return false; // retain ownership for cleanup
    }
    sc.acquired = sc.waited = false;
    sc.released = true;
    return true;
}

bool OpenXRSession::endFrame(const FrameState& /*state*/) {
    if (!m_frameBegun) { m_lastResult = XR_ERROR_CALL_ORDER_INVALID; return false; }
    // Ignore caller-mutated poses/timing; submission uses the located frame snapshot.
    bool ok = !m_frameFailed;
    XrResult failure = m_frameFailure;
    for (int eye = 0; eye < 2; ++eye) {
        auto& sc = m_swapchains[eye];
        if (sc.acquired && !sc.waited) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            const auto result = m_api.xrWaitSwapchainImage(sc.handle, &wait);
            sc.waited = result == XR_SUCCESS;
            if (!sc.waited) { ok = false; failure = result; }
        }
        if (sc.acquired && sc.waited && !releaseSwapchainImage(eye)) {
            ok = false; failure = m_lastResult;
        }
    }
    bool submit = ok && m_frame.shouldRender && m_viewsValid;
    for (const auto& sc : m_swapchains)
        submit = submit && sc.rendered && sc.released && !sc.acquired;

    std::array<XrCompositionLayerProjectionView, 2> projViews{};
    for (int i = 0; i < 2; i++) {
        projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projViews[i].pose = m_views[i].pose;
        projViews[i].fov  = m_views[i].fov;
        projViews[i].subImage.swapchain = m_swapchains[i].handle;
        projViews[i].subImage.imageRect.offset = { 0, 0 };
        projViews[i].subImage.imageRect.extent = {
            static_cast<int32_t>(m_swapchains[i].width),
            static_cast<int32_t>(m_swapchains[i].height)
        };
        projViews[i].subImage.imageArrayIndex = 0;
    }

    XrCompositionLayerProjection projLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    projLayer.space     = m_referenceSpace;
    projLayer.viewCount = 2;
    projLayer.views     = projViews.data();

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer)
    };

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime          = m_frame.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount           = submit ? 1 : 0;
    endInfo.layers               = submit ? layers : nullptr;

    const auto result = m_api.xrEndFrame(m_session, &endInfo);
    m_frameBegun = false; // end was attempted; never retry an ambiguously consumed frame
    if (!check(result)) return false;
    if (!ok) { m_lastResult = failure; return false; }
    return true;
}

void OpenXRSession::recenter() {
    // Use the midpoint of the two eye poses as head center.
    // In a real implementation, we'd use a dedicated HEAD action pose.
    if (!m_sessionRunning || !m_viewsValid) return;

    // Average the two eye positions for approximate head center.
    Vec3 headPos = {
        (m_views[0].pose.position.x + m_views[1].pose.position.x) * 0.5,
        (m_views[0].pose.position.y + m_views[1].pose.position.y) * 0.5,
        (m_views[0].pose.position.z + m_views[1].pose.position.z) * 0.5
    };

    // Use the left eye orientation as an approximation for head orientation.
    Quat headOri = {
        m_views[0].pose.orientation.x,
        m_views[0].pose.orientation.y,
        m_views[0].pose.orientation.z,
        m_views[0].pose.orientation.w
    };

    try {
        double yaw;
        if (extractHeadingYaw(headOri, yaw)) {
            m_recenter.T_R_X = computeRecenter(headPos, yaw);
            m_recenter.valid = true;
        }
    } catch (const std::exception&) { m_lastResult = XR_ERROR_VALIDATION_FAILURE; }
}

} // namespace acz
