#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr_owner_openxr.h"
#include "xr_stereo_output.h"
#include <cstring>
#include <vector>

namespace acz::xr_owner {
static_assert(XR_ENVIRONMENT_BLEND_MODE_OPAQUE == blend_opaque, "blend constant");
// XR-START: xr_result_name() and the start policy (xr_start.cpp) hardcode these core values.
static_assert(XR_ERROR_RUNTIME_FAILURE == -2 && XR_ERROR_INSTANCE_LOST == -13 && XR_ERROR_SESSION_LOST == -17 &&
    XR_ERROR_SESSION_NOT_READY == -28 && XR_ERROR_FORM_FACTOR_UNAVAILABLE == -35 && XR_ERROR_FORM_FACTOR_UNSUPPORTED == -34 &&
    XR_ERROR_RUNTIME_UNAVAILABLE == -51 && XR_ERROR_GRAPHICS_DEVICE_INVALID == -38 && XR_ERROR_EXTENSION_NOT_PRESENT == -9 &&
    XR_ERROR_API_VERSION_UNSUPPORTED == -4 && XR_ERROR_INITIALIZATION_FAILED == -6 && XR_FRAME_DISCARDED == 9 &&
    XR_SESSION_LOSS_PENDING == 3 && XR_EVENT_UNAVAILABLE == 4, "XrResult values used by xr_result_name/xr_start");
static_assert(XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY == 1, "form factor value logged as 1");
static_assert(XR_VIEW_STATE_ORIENTATION_VALID_BIT == orientation_valid && XR_VIEW_STATE_POSITION_VALID_BIT == position_valid &&
    XR_VIEW_STATE_ORIENTATION_TRACKED_BIT == orientation_tracked && XR_VIEW_STATE_POSITION_TRACKED_BIT == position_tracked,
    "view flag constants");
static_assert(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT == orientation_valid && XR_SPACE_LOCATION_POSITION_VALID_BIT == position_valid &&
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT == orientation_tracked && XR_SPACE_LOCATION_POSITION_TRACKED_BIT == position_tracked,
    "space flag constants");
namespace {
constexpr uint32_t enumeration_limit = 1024;

Rc map(XrResult r) {
    switch (r) {
    case XR_SUCCESS: return Rc::ok;
    case XR_SESSION_LOSS_PENDING: return Rc::loss_pending;
    case XR_FRAME_DISCARDED: return Rc::frame_discarded;
    case XR_EVENT_UNAVAILABLE: return Rc::event_unavailable;
    case XR_ERROR_SESSION_LOST: return Rc::session_lost;
    case XR_ERROR_INSTANCE_LOST: return Rc::instance_lost;
    default: return Rc::error; // includes unexpected positive codes: conservative
    }
}
Pose pose(const XrPosef& p) {
    Pose o;
    o.position[0] = p.position.x; o.position[1] = p.position.y; o.position[2] = p.position.z;
    o.orientation[0] = p.orientation.x; o.orientation[1] = p.orientation.y;
    o.orientation[2] = p.orientation.z; o.orientation[3] = p.orientation.w;
    return o;
}
SessionState state(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return SessionState::idle;
    case XR_SESSION_STATE_READY: return SessionState::ready;
    case XR_SESSION_STATE_SYNCHRONIZED: return SessionState::synchronized;
    case XR_SESSION_STATE_VISIBLE: return SessionState::visible;
    case XR_SESSION_STATE_FOCUSED: return SessionState::focused;
    case XR_SESSION_STATE_STOPPING: return SessionState::stopping;
    case XR_SESSION_STATE_LOSS_PENDING: return SessionState::loss_pending;
    case XR_SESSION_STATE_EXITING: return SessionState::exiting;
    default: return SessionState::unknown;
    }
}

XrPosef xr_pose(const Pose& p) {
    XrPosef o{};
    o.position = {p.position[0], p.position[1], p.position[2]};
    o.orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
    return o;
}

// J-xr-stereo-output: the same object also implements the consumer's
// swapchain facade, so swapchains live on the owner's session and every call
// still happens on the owner thread (through the owner's Gate).
class OpenXr final : public Runtime, public xr_stereo::SwapchainRuntime {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    struct Chain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        std::vector<XrSwapchainImageD3D12KHR> images;
    };
    Chain chains[xr_stereo::kTargets]; // 0/1 cockpit eyes, 2 world quad, 3 HUD quad (xr_hud_layer only), 4 marker quad (xr_hud_markers_layer only)
    // XR-START diagnostics: the raw XrResult of the last OpenXR function (taken per owner call) and the
    // instance/system facts. Owner thread only.
    int32_t last_xr_ = kNoXrResult;
    RuntimeFacts facts_{};
    Rc m(XrResult r) { last_xr_ = static_cast<int32_t>(r); return map(r); }
    bool failed(XrResult r) { last_xr_ = static_cast<int32_t>(r); return XR_FAILED(r); }
    Rc refuse(const char* why) { last_xr_ = kNoXrResult; facts_.refusal = why; return Rc::error; }
public:
    OpenXr(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {}
    ~OpenXr() override = default; // no foreign calls
    int32_t take_last_xr_result() override { const int32_t r = last_xr_; last_xr_ = kNoXrResult; return r; }
    bool runtime_facts(RuntimeFacts& out) const override { out = facts_; return true; }

    Rc create_instance() override {
        // Never redirect runtime discovery; refuse an inherited override.
        facts_ = RuntimeFacts{};
        if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0) != 0) return refuse("XR_RUNTIME_JSON-set(override-refused)");
        uint32_t count = 0;
        XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        if (failed(r)) { facts_.refusal = "xrEnumerateInstanceExtensionProperties"; return Rc::error; }
        if (count == 0 || count > enumeration_limit) return refuse("extension-count-out-of-range");
        std::vector<XrExtensionProperties> ext(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        uint32_t actual = 0;
        r = xrEnumerateInstanceExtensionProperties(nullptr, count, &actual, ext.data());
        if (failed(r)) { facts_.refusal = "xrEnumerateInstanceExtensionProperties"; return Rc::error; }
        if (actual > count) return refuse("extension-count-out-of-range");
        bool d3d12 = false, time_conversion = false;
        for (uint32_t i = 0; i < actual; ++i) d3d12 |= std::strcmp(ext[i].extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
        for (uint32_t i = 0; i < actual; ++i) time_conversion |= std::strcmp(ext[i].extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
        if (!d3d12) return refuse("no-XR_KHR_D3D12_enable");
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "ACZ in-proxy xr_owner");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
        info.enabledExtensionCount = time_conversion ? 2 : 1;
        info.enabledExtensionNames = enabled;
        const Rc rc = m(xrCreateInstance(&info, &instance));
        if (!succeeded(rc)) facts_.refusal = "xrCreateInstance";
        if (succeeded(rc) && time_conversion)
            xrGetInstanceProcAddr(instance, "xrConvertWin32PerformanceCounterToTimeKHR", reinterpret_cast<PFN_xrVoidFunction*>(&convert_time_));
        if (succeeded(rc)) { // XR-START: runtime identity (diagnostic only; its own result is not recorded)
            XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &props))) {
                facts_.instance_props = true;
                facts_.runtime_version = props.runtimeVersion;
                strncpy_s(facts_.runtime_name, props.runtimeName, _TRUNCATE);
            }
        }
        return rc;
    }
    Rc query_system(SystemFacts& out) override {
        XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
        info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        facts_.form_factor = static_cast<uint32_t>(info.formFactor);
        facts_.system_props = false;
        facts_.refusal = nullptr;
        Rc rc = m(xrGetSystem(instance, &info, &system));
        if (!succeeded(rc)) { facts_.refusal = "xrGetSystem"; return rc; }
        out.hmd_system = system != XR_NULL_SYSTEM_ID;
        facts_.system_id = static_cast<uint64_t>(system);
        { // XR-START: system identity (diagnostic only; its own result is not recorded)
            XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
            if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp))) {
                facts_.system_props = true;
                facts_.vendor_id = sp.vendorId;
                strncpy_s(facts_.system_name, sp.systemName, _TRUNCATE);
                facts_.max_layers = sp.graphicsProperties.maxLayerCount;
                facts_.orientation_tracking = sp.trackingProperties.orientationTracking == XR_TRUE;
                facts_.position_tracking = sp.trackingProperties.positionTracking == XR_TRUE;
            }
        }
        uint32_t count = 0;
        if (failed(xrEnumerateViewConfigurations(instance, system, 0, &count, nullptr)) || count > enumeration_limit) return Rc::error;
        std::vector<XrViewConfigurationType> types(count);
        uint32_t actual = 0;
        if (count && (failed(xrEnumerateViewConfigurations(instance, system, count, &actual, types.data())) || actual > count)) return Rc::error;
        for (uint32_t i = 0; i < actual; ++i) out.primary_stereo |= types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (!out.primary_stereo) return rc;
        count = 0;
        if (failed(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) ||
            count > enumeration_limit) return Rc::error;
        out.view_count = count;
        count = 0;
        if (failed(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)))
            return Rc::error;
        out.blend_count = count;
        if (count == 0 || count > max_blend_modes) return rc; // owner refuses
        XrEnvironmentBlendMode modes[max_blend_modes]{};
        actual = 0;
        if (failed(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &actual, modes)) ||
            actual > count) return Rc::error;
        out.blend_count = actual;
        for (uint32_t i = 0; i < actual; ++i) out.blend_modes[i] = static_cast<uint32_t>(modes[i]);
        return rc;
    }
    Rc graphics_requirements(GraphicsRequirements& out) override {
        PFN_xrGetD3D12GraphicsRequirementsKHR get = nullptr;
        Rc rc = m(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get)));
        if (!succeeded(rc) || !get) return Rc::error;
        XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        rc = m(get(instance, system, &req));
        out.luid_low = req.adapterLuid.LowPart;
        out.luid_high = req.adapterLuid.HighPart;
        out.min_feature_level = static_cast<uint32_t>(req.minFeatureLevel);
        return rc;
    }
    Rc query_binding(BindingFacts& out) override {
        out.device_present = device != nullptr;
        out.queue_present = queue != nullptr;
        if (!device || !queue) return Rc::ok; // owner refuses
        const LUID luid = device->GetAdapterLuid();
        out.luid_low = luid.LowPart;
        out.luid_high = luid.HighPart;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS data{static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), levels, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data)))) return Rc::error;
        out.max_feature_level = static_cast<uint32_t>(data.MaxSupportedFeatureLevel);
        out.queue_direct = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device* owner = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owner))) && owner) {
            out.queue_on_device = owner == device;
            owner->Release(); // temporary reference from GetDevice only
        }
        return Rc::ok;
    }
    Rc create_session() override {
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = device;
        binding.queue = queue;
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        info.systemId = system;
        return m(xrCreateSession(instance, &info, &session));
    }
    Rc query_spaces(SpaceFacts& out) override {
        uint32_t count = 0;
        if (failed(xrEnumerateReferenceSpaces(session, 0, &count, nullptr)) || count > enumeration_limit) return Rc::error;
        std::vector<XrReferenceSpaceType> spaces(count);
        uint32_t actual = 0;
        if (count && (failed(xrEnumerateReferenceSpaces(session, count, &actual, spaces.data())) || actual > count)) return Rc::error;
        for (uint32_t i = 0; i < actual; ++i) {
            out.local |= spaces[i] == XR_REFERENCE_SPACE_TYPE_LOCAL;
            out.view |= spaces[i] == XR_REFERENCE_SPACE_TYPE_VIEW;
        }
        return Rc::ok;
    }
    Rc create_space(XrReferenceSpaceType type, XrSpace& out) {
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = type;
        info.poseInReferenceSpace.orientation.w = 1; // identity offset
        return m(xrCreateReferenceSpace(session, &info, &out));
    }
    Rc create_local_space() override { return create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, local); }
    Rc create_view_space() override { return create_space(XR_REFERENCE_SPACE_TYPE_VIEW, view); }
    Rc poll_event(Event& out) override {
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = xrPollEvent(instance, &buffer);
        last_xr_ = static_cast<int32_t>(r);
        if (r != XR_SUCCESS) return m(r);
        switch (buffer.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
            out.type = EventType::session_state;
            out.same_session = e.session == session;
            out.state = state(e.state);
            out.event_time = e.time;
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: out.type = EventType::instance_loss_pending; break;
        case XR_TYPE_EVENT_DATA_EVENTS_LOST: out.type = EventType::events_lost; break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& e = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&buffer);
            out.type = EventType::reference_space_change;
            out.same_session = e.session == session;
            out.space = e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? SpaceType::local :
                e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? SpaceType::view :
                e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? SpaceType::stage : SpaceType::other;
            out.change_time = e.changeTime;
            break;
        }
        default: out.type = EventType::other; break;
        }
        return Rc::ok;
    }
    Rc begin_session() override {
        XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
        info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        return m(xrBeginSession(session, &info));
    }
    Rc wait_frame(FrameTiming& out) override {
        XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        const Rc rc = m(xrWaitFrame(session, &info, &frame)); // not cancellable
        out.predicted_time = frame.predictedDisplayTime;
        out.predicted_period = frame.predictedDisplayPeriod;
        out.should_render = frame.shouldRender == XR_TRUE;
        return rc;
    }
    Rc begin_frame() override {
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        return m(xrBeginFrame(session, &info));
    }
    Rc locate_views(int64_t time, ViewSample& out) override {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = time;
        info.space = local;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        const Rc rc = m(xrLocateViews(session, &info, &vs, 2, &count, views));
        out.view_count = count;
        out.view_state_flags = vs.viewStateFlags;
        for (uint32_t i = 0; i < 2 && i < count; ++i) {
            out.eyes[i].pose = pose(views[i].pose);
            out.eyes[i].fov = {views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown};
        }
        return rc;
    }
    Rc locate_head(int64_t time, HeadSample& out) override {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const Rc rc = m(xrLocateSpace(view, local, time, &location));
        out.location_flags = location.locationFlags;
        out.pose = pose(location.pose);
        return rc;
    }
    Rc end_frame_zero_layers(int64_t display_time, uint32_t blend) override {
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = display_time;
        info.environmentBlendMode = static_cast<XrEnvironmentBlendMode>(blend);
        info.layerCount = 0;
        info.layers = nullptr;
        const Rc rc = m(xrEndFrame(session, &info));
        sample_end_lateness(display_time);
        return rc;
    }
    Rc end_frame_projection(int64_t display_time, uint32_t blend, const ProjectionLayer& l) override {
        late_available_ = false;
        if (!local || ((!l.screen_only || l.table) && (!chains[0].handle || !chains[1].handle))) return Rc::refused;   // VSCREEN: no eye swapchain needed (BRIEF3D table: needed)
        XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& v = views[eye];
            v.pose = xr_pose(l.views[eye].pose); // raw, applied exactly once by the runtime
            v.fov = {l.views[eye].fov.left, l.views[eye].fov.right, l.views[eye].fov.up, l.views[eye].fov.down};
            v.subImage.swapchain = chains[eye].handle;
            v.subImage.imageArrayIndex = 0;
            v.subImage.imageRect.offset = {0, 0};
            v.subImage.imageRect.extent = {static_cast<int32_t>(chains[eye].width), static_cast<int32_t>(chains[eye].height)};
        }
        // Layer list, order and flags from the pure xr_stereo::compose_layers (tested
        // offline): [world quad (opaque, LOCAL)] -> cockpit projection -> [HUD quad].
        // Premultiplied alpha = SOURCE_ALPHA without UNPREMULTIPLIED_ALPHA.
        auto flags = [](const xr_stereo::LayerEntry& e) -> XrCompositionLayerFlags {
            return (e.source_alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0) |
                (e.unpremultiplied ? XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT : 0);
        };
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = local;
        layer.viewCount = 2;
        layer.views = views;
        XrCompositionLayerQuad quads[5] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}}; // world, HUD, markers, table eye 0, table eye 1
        const XrCompositionLayerBaseHeader* layers[4] = {};
        const xr_stereo::LayerList list = xr_stereo::compose_layers(l);
        uint32_t count = 0;
        for (unsigned i = 0; i < list.count; ++i) {
            const xr_stereo::LayerEntry& e = list.e[i];
            if (e.kind == xr_stereo::LayerKind::projection) {
                layer.layerFlags = flags(e);
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                continue;
            }
            if (e.kind == xr_stereo::LayerKind::table_quad) {   // UI3D 2026-10-30b: the table's eye image as a quad seen by that eye only (ProjectionLayer::table_front)
                const unsigned eye = e.target & 1u;
                const Chain& c = chains[eye];
                if (!c.handle || !local) return Rc::refused;
                XrCompositionLayerQuad& q = quads[3 + eye];
                // b3p:1 (headset test 9): the table's world-fixed plane, the same pose for both eyes; else a plane along that eye's view axis
                const xr_stereo::Quad tq = l.table_quad_fixed ? xr_stereo::Quad{l.table_quad_pose, l.table_quad_width, l.table_quad_height}
                                                              : xr_stereo::view_quad(l.views[eye], xr_stereo::table_quad_distance(l.table_quad_distance_m[eye]));
                q.layerFlags = flags(e);
                q.space = local;
                q.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q.subImage.swapchain = c.handle;
                q.subImage.imageArrayIndex = 0;
                q.subImage.imageRect.offset = {0, 0};
                q.subImage.imageRect.extent = {static_cast<int32_t>(c.width), static_cast<int32_t>(c.height)};
                q.pose = xr_pose(tq.pose);
                q.size = XrExtent2Df{tq.width, tq.height};
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
                continue;
            }
            // Quads: both eyes, full image of their own swapchain.
            const bool is_hud = e.kind == xr_stereo::LayerKind::hud_quad;
            const bool is_marker = e.kind == xr_stereo::LayerKind::marker_quad; // J-hud-markers
            const Chain& c = chains[e.target];
            const XrSpace space = e.space == xr_stereo::LayerSpace::view ? view : local;
            if (!c.handle || !space) return Rc::refused;
            XrCompositionLayerQuad& q = quads[is_marker ? 2 : is_hud ? 1 : 0];
            q.layerFlags = flags(e);
            q.space = space;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = c.handle;
            q.subImage.imageArrayIndex = 0;
            q.subImage.imageRect.offset = {0, 0};
            q.subImage.imageRect.extent = {static_cast<int32_t>(c.width), static_cast<int32_t>(c.height)};
            q.pose = xr_pose(is_marker ? l.marker_pose : is_hud ? l.hud_pose : l.quad_pose);
            q.size = is_marker ? XrExtent2Df{l.marker_width, l.marker_height}
                : is_hud ? XrExtent2Df{l.hud_width, l.hud_height} : XrExtent2Df{l.quad_width, l.quad_height};
            if (e.kind == xr_stereo::LayerKind::screen_quad) { q.pose = xr_pose(l.screen_pose); q.size = XrExtent2Df{l.screen_width, l.screen_height}; }   // VSCREEN
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
        }
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = display_time;
        info.environmentBlendMode = static_cast<XrEnvironmentBlendMode>(blend);
        info.layerCount = count;
        info.layers = layers;
        const Rc rc = m(xrEndFrame(session, &info));
        sample_end_lateness(display_time);
        return rc;
    }
    // ---- xr_stereo::SwapchainRuntime (owner thread, through the Gate) ----
    Rc view_configs(std::array<xr_stereo::ViewConfig, 2>& out) override {
        uint32_t count = 0;
        XrResult r = xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
        if (failed(r)) return m(r);
        if (count != 2) return Rc::error;
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t actual = 0;
        r = xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &actual, v);
        if (failed(r)) return m(r);
        if (actual != 2) return Rc::error;
        for (unsigned i = 0; i < 2; ++i)
            out[i] = {v[i].recommendedImageRectWidth, v[i].recommendedImageRectHeight, v[i].maxImageRectWidth,
                v[i].maxImageRectHeight, v[i].recommendedSwapchainSampleCount};
        return Rc::ok;
    }
    Rc formats(std::array<int64_t, xr_stereo::max_formats>& out, uint32_t& n) override {
        uint32_t count = 0;
        XrResult r = xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        if (failed(r)) return m(r);
        n = count;
        if (count > out.size()) return Rc::ok; // consumer refuses an over-capacity list
        uint32_t actual = 0;
        r = count ? xrEnumerateSwapchainFormats(session, count, &actual, out.data()) : XR_SUCCESS;
        if (failed(r)) return m(r);
        if (actual != count) return Rc::error;
        return Rc::ok;
    }
    Rc create_swapchain(unsigned eye, int64_t format, uint32_t w, uint32_t h, xr_stereo::SwapchainImages& out) override {
        if (eye >= xr_stereo::kTargets || chains[eye].handle) return Rc::error;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        // xr_probe policy: a UNORM (28) chain needs mutable-format permission so
        // the RTV can be sRGB; the blit backend refuses a typed UNORM resource.
        if (format == xr_stereo::rgba8_unorm) info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        info.format = format; info.sampleCount = 1; info.width = w; info.height = h;
        info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
        Chain& c = chains[eye];
        const Rc rc = m(xrCreateSwapchain(session, &info, &c.handle));
        if (!succeeded(rc)) { c.handle = XR_NULL_HANDLE; return rc; }
        c.width = w; c.height = h;
        uint32_t count = 0;
        XrResult r = xrEnumerateSwapchainImages(c.handle, 0, &count, nullptr);
        if (failed(r)) return m(r);
        if (!count || count > xr_stereo::max_images) { out.count = count; return Rc::ok; } // consumer refuses
        c.images.assign(count, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        uint32_t actual = 0;
        r = xrEnumerateSwapchainImages(c.handle, count, &actual, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.images.data()));
        if (failed(r)) return m(r);
        if (actual != count) return Rc::error;
        out.count = count;
        for (uint32_t i = 0; i < count; ++i) out.images[i] = c.images[i].texture;
        return rc;
    }
    Rc acquire(unsigned eye, uint32_t& index) override {
        if (eye >= xr_stereo::kTargets || !chains[eye].handle) return Rc::error;
        XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        return m(xrAcquireSwapchainImage(chains[eye].handle, &info, &index));
    }
    Rc wait(unsigned eye, int64_t timeout_ns, bool& timed_out) override {
        timed_out = false;
        if (eye >= xr_stereo::kTargets || !chains[eye].handle) return Rc::error;
        XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        info.timeout = timeout_ns;
        const XrResult r = xrWaitSwapchainImage(chains[eye].handle, &info);
        if (r == XR_TIMEOUT_EXPIRED) { timed_out = true; return Rc::ok; } // success code: not waited
        return m(r);
    }
    Rc release(unsigned eye) override {
        if (eye >= xr_stereo::kTargets || !chains[eye].handle) return Rc::error;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        return m(xrReleaseSwapchainImage(chains[eye].handle, &info));
    }
    Rc destroy_swapchain(unsigned eye) override {
        if (eye >= xr_stereo::kTargets) return Rc::error;
        Chain& c = chains[eye];
        const Rc rc = c.handle ? m(xrDestroySwapchain(c.handle)) : Rc::ok;
        if (succeeded(rc)) { c.handle = XR_NULL_HANDLE; c.images.clear(); }
        return rc;
    }
    PFN_xrConvertWin32PerformanceCounterToTimeKHR convert_time_ = nullptr;
    bool late_available_ = false;
    double late_ms_ = 0;
    void sample_end_lateness(int64_t predicted) {
        late_available_ = false;
        LARGE_INTEGER cpu{}; XrTime time = 0;
        if (convert_time_ && QueryPerformanceCounter(&cpu) && XR_SUCCEEDED(convert_time_(instance, &cpu, &time))) {
            late_ms_ = double((static_cast<long double>(time) - predicted) / 1e6L);
            late_available_ = true;
        }
    }
    bool end_lateness_ms(double& out) const override { out = late_ms_; return late_available_; }
    Rc request_exit() override { return m(xrRequestExitSession(session)); }
    Rc end_session() override { return m(xrEndSession(session)); }
    Rc destroy_view_space() override {
        const Rc rc = view ? m(xrDestroySpace(view)) : Rc::ok;
        if (succeeded(rc)) view = XR_NULL_HANDLE;
        return rc;
    }
    Rc destroy_local_space() override {
        const Rc rc = local ? m(xrDestroySpace(local)) : Rc::ok;
        if (succeeded(rc)) local = XR_NULL_HANDLE;
        return rc;
    }
    Rc destroy_session() override {
        const Rc rc = session ? m(xrDestroySession(session)) : Rc::ok;
        if (succeeded(rc)) {
            session = XR_NULL_HANDLE;
            for (auto& c : chains) { c.handle = XR_NULL_HANDLE; c.images.clear(); } // children destroyed with the session
        }
        return rc;
    }
    Rc release_binding() override {
        if (session) return Rc::error; // dependants must be settled first
        if (queue) { queue->Release(); queue = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        return Rc::ok;
    }
    Rc destroy_instance() override {
        if (session) return Rc::error;
        const Rc rc = instance ? m(xrDestroyInstance(instance)) : Rc::ok;
        if (succeeded(rc)) instance = XR_NULL_HANDLE;
        return rc;
    }
};
}
std::shared_ptr<Runtime> make_openxr_runtime(ID3D12Device* d, ID3D12CommandQueue* q) {
    return std::make_shared<OpenXr>(d, q);
}
StereoRuntimes make_openxr_stereo_runtime(ID3D12Device* d, ID3D12CommandQueue* q) {
    StereoRuntimes out;
    auto rt = std::make_shared<OpenXr>(d, q);
    out.runtime = rt;
    out.swapchains = std::shared_ptr<xr_stereo::SwapchainRuntime>(rt, static_cast<xr_stereo::SwapchainRuntime*>(rt.get()));
    return out;
}
}
