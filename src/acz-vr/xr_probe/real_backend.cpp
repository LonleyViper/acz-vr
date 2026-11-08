#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "real_backend.h"
#include "compatibility.h"
#include "format_policy.h"
#include "colour_owner.h"
#include "synthetic_vs.h" // build-time fxc bytecode (card d)
#include "synthetic_ps.h"
#include <climits>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>
#include <array>
#include <cmath>
#include <limits>

namespace acz::probe {
static_assert(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO==2 && XR_ENVIRONMENT_BLEND_MODE_OPAQUE==1 &&
    DXGI_FORMAT_R8G8B8A8_UNORM==rgba8_unorm && DXGI_FORMAT_R8G8B8A8_UNORM_SRGB==rgba8_srgb,
    "Portable compatibility policy constants must match the SDK");
namespace {
class Real final : public Backend, public ColourBackend {
    Clock& clock; Log log;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* queue_owner = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrTime predicted = 0;
    bool should_render=false, frame_discarded=false, reference_changed=false, loss_seen=false;
    bool fatal_loss=false;
    XrResult last_xr=XR_SUCCESS;
    std::array<XrViewConfigurationView,2> view_limits{};
    std::vector<int64_t> offered_formats;
    State state = State::idle;
    bool session_started=false,cleanup_mode=false,call_refused=false,admission_closed=false;
    Op current_op=Op::count;
    int64_t deadline=Caps::startup_ms;
    bool synthetic=false; // card (d) only; a1/a2 never reach real_stereo.inc
    template<class F> auto timed(const char* label, F fn) -> decltype(fn()) {
        auto start = clock.now();
        if(current_op==Op::wait_frame && !frame_admitted(start)) {
            admission_closed=true;call_refused=true;log("time cap: native frame admission closed");
            return static_cast<decltype(fn())>(-1);
        }
        if(start>=deadline) {
            call_refused=true;log(std::string("FAILURE deadline refuses call=")+label);
            return static_cast<decltype(fn())>(-1);
        }
        std::ostringstream s;
        s << "call entry=" << label << " ms=" << start << " thread=" << GetCurrentThreadId();
        log(s.str());
        if(current_op==Op::wait_frame && !frame_admitted(clock.now())) {
            admission_closed=true;call_refused=true;log("time cap: native frame admission closed after log");
            return static_cast<decltype(fn())>(-1);
        }
        if(clock.now()>=deadline) {
            call_refused=true;log(std::string("FAILURE deadline after entry log=")+label);
            return static_cast<decltype(fn())>(-1);
        }
        auto result = fn();
        log(std::string("call exit=") + label + " ms=" + std::to_string(clock.now()) +
            " duration_ms=" + std::to_string(clock.now()-start) + " result=" + std::to_string(static_cast<int64_t>(result)));
        return result;
    }
    template<class F> bool xr(const char* label, F fn) {
        auto result=timed(label, fn);
        // XR_FRAME_DISCARDED still begins a frame and requires xrEndFrame.
        // SESSION_LOSS_PENDING is also a successful call: finish any current
        // frame, then refuse success and enter the failure teardown path.
        last_xr=result;
        if(result==XR_ERROR_SESSION_LOST || result==XR_ERROR_INSTANCE_LOST)fatal_loss=true;
        if(result==XR_SESSION_LOSS_PENDING || result==XR_ERROR_SESSION_LOST || result==XR_ERROR_INSTANCE_LOST) {
            state=State::loss;loss_seen=true;
        }
        return XR_SUCCEEDED(result);
    }
    template<class T, class F> bool enumerate(const char* label, std::vector<T>& out, T initial, F fn) {
        uint32_t count = 0;
        if (!xr(label, [&]{ return fn(0, &count, nullptr); }) || count > Caps::enumeration_limit) return false;
        out.assign(count, initial);
        if (!count) { log(std::string("enumeration ")+label+" count=0"); return true; }
        uint32_t actual = 0;
        if (!xr(label, [&]{ return fn(count, &actual, out.data()); }) || actual > count) return false;
        out.resize(actual);
        log(std::string("enumeration ") + label + " count=" + std::to_string(actual));
        return true; // no retry if runtime changes the count
    }
    bool make_instance() {
        log("sdk_version="+std::to_string(XR_CURRENT_API_VERSION)+" requested_api_version="+std::to_string(XR_MAKE_VERSION(1,0,0)));
        // Link only to the SDK loader. Its default active-runtime discovery is
        // used unchanged; never set XR_RUNTIME_JSON or change the registry.
        // Refuse an inherited override rather than silently using another runtime.
        if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0) != 0) {
            log("refusal=inherited XR_RUNTIME_JSON override"); return false;
        }
        constexpr XrVersion requested = XR_MAKE_VERSION(1, 0, 0);
        // OpenXR has no xrEnumerateInstanceVersion. xrCreateInstance is the
        // authoritative version negotiation and refuses API_VERSION_UNSUPPORTED.
        std::vector<XrExtensionProperties> extensions;
        if (!enumerate("xrEnumerateInstanceExtensionProperties", extensions,
            XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES},
            [&](uint32_t n, uint32_t* count, XrExtensionProperties* p){ return xrEnumerateInstanceExtensionProperties(nullptr,n,count,p); })) return false;
        std::vector<std::string> extension_names;
        for (auto& e : extensions) {
            log(std::string("extension=")+e.extensionName+" version="+std::to_string(e.extensionVersion));
            extension_names.emplace_back(e.extensionName);
        }
        if (!d3d12_extension(extension_names)) { log("refusal=missing XR_KHR_D3D12_enable"); return false; }
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "ACZ standalone xr_probe phase1");
        info.applicationInfo.apiVersion = requested;
        const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        info.enabledExtensionCount = 1; info.enabledExtensionNames = enabled;
        if (!xr("xrCreateInstance", [&]{return xrCreateInstance(&info,&instance);})) return false;
        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        if (!xr("xrGetInstanceProperties", [&]{return xrGetInstanceProperties(instance,&props);})) return false;
        // OpenXR exposes runtimeVersion (not a separate runtime API-version field)
        // here. Record the successfully negotiated application API separately.
        log(std::string("runtime_name=")+props.runtimeName+" runtime_version="+std::to_string(props.runtimeVersion)+
            " api_version="+std::to_string(requested)+" sdk_version="+std::to_string(XR_CURRENT_API_VERSION));
        return true;
    }
    bool views() {
        std::vector<XrViewConfigurationType> types;
        if (!enumerate("xrEnumerateViewConfigurations",types,XrViewConfigurationType{},
            [&](uint32_t n,uint32_t* count,XrViewConfigurationType* p){return xrEnumerateViewConfigurations(instance,system,n,count,p);})) return false;
        std::vector<int> configurations;
        for (auto v:types) { log("view_config="+std::to_string(v)); configurations.push_back(v); }
        if (!stereo_config(configurations)) { log("refusal=no primary stereo"); return false; }
        std::vector<XrViewConfigurationView> eyes;
        if (!enumerate("xrEnumerateViewConfigurationViews",eyes,XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
            [&](uint32_t n,uint32_t* count,XrViewConfigurationView* p){return xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,n,count,p);})) return false;
        if (eyes.size()!=2) { log("refusal=stereo view count"); return false; }
        std::copy(eyes.begin(),eyes.end(),view_limits.begin());
        std::vector<ViewLimits> limits;
        for (auto& e:eyes) {
            log("stereo_view recommended_width="+std::to_string(e.recommendedImageRectWidth)+" max_width="+std::to_string(e.maxImageRectWidth)+
                " recommended_height="+std::to_string(e.recommendedImageRectHeight)+" max_height="+std::to_string(e.maxImageRectHeight)+
                " recommended_samples="+std::to_string(e.recommendedSwapchainSampleCount)+" max_samples="+std::to_string(e.maxSwapchainSampleCount));
            limits.push_back({e.recommendedImageRectWidth,e.recommendedImageRectHeight,e.recommendedSwapchainSampleCount,
                e.maxImageRectWidth,e.maxImageRectHeight,e.maxSwapchainSampleCount});
        }
        std::vector<XrEnvironmentBlendMode> modes;
        if (!enumerate("xrEnumerateEnvironmentBlendModes",modes,XrEnvironmentBlendMode{},
            [&](uint32_t n,uint32_t* count,XrEnvironmentBlendMode* p){return xrEnumerateEnvironmentBlendModes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,n,count,p);})) return false;
        std::vector<int> blend_modes;
        for (auto m:modes) { log("blend_mode="+std::to_string(m)); blend_modes.push_back(m); }
        if (!stereo_views(limits) || !opaque_blend(blend_modes)) { log("refusal=invalid stereo dimensions or no opaque blend mode"); return false; }
        log("selected_view_config=PRIMARY_STEREO selected_blend=OPAQUE"); return true;
    }
    bool make_device() {
        HRESULT hr = timed("CreateDXGIFactory1", [&]{return CreateDXGIFactory1(IID_PPV_ARGS(&factory));});
        if (FAILED(hr)) return false;
        hr = timed("EnumAdapterByLuid", [&]{return factory->EnumAdapterByLuid(requirements.adapterLuid,IID_PPV_ARGS(&adapter));});
        if (FAILED(hr)) return false;
        // No WARP, default adapter, or lower-feature fallback.
        hr = timed("D3D12CreateDevice", [&]{return D3D12CreateDevice(adapter,requirements.minFeatureLevel,IID_PPV_ARGS(&device));});
        if (FAILED(hr)) return false;
        D3D_FEATURE_LEVEL requested = requirements.minFeatureLevel;
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1,&requested,D3D_FEATURE_LEVEL_11_0};
        hr = timed("CheckFeatureSupport", [&]{return device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&levels,sizeof(levels));});
        LUID luid{};
        timed("device.GetAdapterLuid",[&]{luid=device->GetAdapterLuid();return S_OK;});
        log("selected_adapter_luid="+std::to_string(luid.HighPart)+":"+std::to_string(luid.LowPart)+
            " selected_feature_level="+std::to_string(levels.MaxSupportedFeatureLevel));
        if (FAILED(hr) || levels.MaxSupportedFeatureLevel<requested ||
            luid.HighPart!=requirements.adapterLuid.HighPart || luid.LowPart!=requirements.adapterLuid.LowPart) return false;
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = timed("CreateCommandQueue", [&]{return device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));});
        if (FAILED(hr)) return false;
        D3D12_COMMAND_QUEUE_DESC actual{};
        timed("queue.GetDesc",[&]{actual=queue->GetDesc();return S_OK;});
        hr = timed("queue.GetDevice", [&]{return queue->GetDevice(IID_PPV_ARGS(&queue_owner));});
        bool matches = SUCCEEDED(hr) && queue_owner==device && actual.Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
        log(std::string("queue_type=DIRECT own_device=1 own_queue=1 device_association=")+(matches?"1":"0"));
        return compatible_graphics({requirements.adapterLuid.LowPart,luid.LowPart,requirements.adapterLuid.HighPart,luid.HighPart,
            static_cast<uint32_t>(requested),static_cast<uint32_t>(levels.MaxSupportedFeatureLevel),true,true,true,
            actual.Type==D3D12_COMMAND_LIST_TYPE_DIRECT,matches});
    }
public:
    Real(Clock& c,Log l,Mode m):clock(c),log(l),synthetic(m==Mode::phase3_synthetic_stereo) {}
    // No implicit foreign calls in destructor. Explicit cleanup is logged and
    // deadline controlled; unretired handles die only with the harness process.
    Reply call(Op op, unsigned layer_count, const void* layers) override {
        bool retiring=op==Op::request_exit || op==Op::end_session || op==Op::destroy_space ||
            op==Op::destroy_session || op==Op::release_queue || op==Op::release_device || op==Op::destroy_instance;
        if(retiring && !cleanup_mode) {cleanup_mode=true;deadline=std::min(Caps::absolute_ms,clock.now()+Caps::cleanup_ms);}
        if(!cleanup_mode)deadline=session_started?Caps::absolute_ms:Caps::startup_ms;
        // Final native-call admission guard, even if scheduling crosses the
        // reserve boundary after the core's check. Finish an already waited
        // frame normally; only a NEW xrWaitFrame is stopped at 13 s.
        if(op==Op::wait_frame)deadline=std::min(deadline,Caps::admission_ms);
        call_refused=false;admission_closed=false;current_op=op;
        bool ok = false;
        switch(op) {
        case Op::instance: ok=make_instance(); break;
        case Op::system: {
            XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO}; info.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            ok=xr("xrGetSystem",[&]{return xrGetSystem(instance,&info,&system);});
            ok &= system!=XR_NULL_SYSTEM_ID;
            log("system="+std::to_string(system)); break;
        }
        case Op::views: ok=views(); break;
        case Op::requirements: {
            PFN_xrGetD3D12GraphicsRequirementsKHR get = nullptr;
            ok=xr("xrGetInstanceProcAddr",[&]{return xrGetInstanceProcAddr(instance,"xrGetD3D12GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&get));});
            if (ok && get) ok=xr("xrGetD3D12GraphicsRequirementsKHR",[&]{return get(instance,system,&requirements);}); else ok=false;
            log("required_adapter_luid="+std::to_string(requirements.adapterLuid.HighPart)+":"+std::to_string(requirements.adapterLuid.LowPart)+
                " min_feature_level="+std::to_string(requirements.minFeatureLevel)); break;
        }
        case Op::device: ok=make_device(); break;
        case Op::session: {
            XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR}; binding.device=device; binding.queue=queue;
            XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO}; info.next=&binding; info.systemId=system;
            ok=xr("xrCreateSession",[&]{return xrCreateSession(instance,&info,&session);}); break;
        }
        case Op::formats: {
            // Phase 1 observes availability only: no selection, images or swapchains.
            ok=observe_phase1_formats([&](uint32_t n,uint32_t* count,int64_t* p){
                auto good=xr("xrEnumerateSwapchainFormats",[&]{return xrEnumerateSwapchainFormats(session,n,count,p);});
                if(good && n && *count==n)offered_formats.assign(p,p+n);
                if(!n)offered_formats.clear();
                return good;
            },log);
            break;
        }
        case Op::spaces: {
            std::vector<XrReferenceSpaceType> spaces;
            ok=enumerate("xrEnumerateReferenceSpaces",spaces,XrReferenceSpaceType{},[&](uint32_t n,uint32_t* count,XrReferenceSpaceType* p){return xrEnumerateReferenceSpaces(session,n,count,p);});
            bool found=false;
            for(auto s:spaces) { log("reference_space="+std::to_string(s)); found |= s==XR_REFERENCE_SPACE_TYPE_LOCAL; }
            ok &= found; if(ok) log("selected_reference_space=LOCAL"); break;
        }
        case Op::create_space: {
            XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; info.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
            info.poseInReferenceSpace.orientation.w=1;
            ok=xr("xrCreateReferenceSpace",[&]{return xrCreateReferenceSpace(session,&info,&space);}); break;
        }
        case Op::poll: {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            auto result=timed("xrPollEvent",[&]{return xrPollEvent(instance,&event);});
            ok=result==XR_SUCCESS || result==XR_EVENT_UNAVAILABLE;
            if(result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto& change=*reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                log("session_state="+std::to_string(change.state)+" time="+std::to_string(change.time));
                if(change.session!=session) {ok=false;break;}
                switch(change.state) {
                case XR_SESSION_STATE_READY:state=State::ready;break;
                case XR_SESSION_STATE_STOPPING:state=State::stopping;break;
                case XR_SESSION_STATE_EXITING:state=State::exiting;break;
                case XR_SESSION_STATE_LOSS_PENDING:state=State::loss;break;
                default:break;
                }
            }
            if(result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) state=State::loss;
            if(result==XR_SESSION_LOSS_PENDING || result==XR_ERROR_SESSION_LOST || result==XR_ERROR_INSTANCE_LOST)state=State::loss;
            if(result==XR_ERROR_SESSION_LOST || result==XR_ERROR_INSTANCE_LOST)fatal_loss=true;
            if(state==State::loss || state==State::exiting)loss_seen=true;
            if(result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                auto& change=*reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                if(change.session!=session){ok=false;break;}
                if(change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    reference_changed=true;log("reference_space_change_pending LOCAL: conservative stop");
                }
            }
            if(result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_EVENTS_LOST) { log("FAILURE dropped session events");ok=false; }
            break;
        }
        case Op::begin_session: {
            XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};info.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            ok=xr("xrBeginSession",[&]{return xrBeginSession(session,&info);}); break;
        }
        case Op::wait_frame: {
            XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            ok=xr("xrWaitFrame",[&]{return xrWaitFrame(session,&info,&frame);}); predicted=frame.predictedDisplayTime;
            should_render=frame.shouldRender==XR_TRUE;
            log("predicted_time="+std::to_string(predicted)+" period="+std::to_string(frame.predictedDisplayPeriod)+" shouldRender="+std::to_string(frame.shouldRender));break;
        }
        case Op::begin_frame: {
            XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};ok=xr("xrBeginFrame",[&]{return xrBeginFrame(session,&info);});
            frame_discarded=last_xr==XR_FRAME_DISCARDED;
            if(frame_discarded)log("begin_disposition=FRAME_DISCARDED accepted; end once with zero layers");break;
        }
        case Op::end_frame: {
            if(layer_count!=0 || layers!=nullptr){ok=end_projection(layer_count,layers);break;}
            XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=predicted;info.environmentBlendMode=blend;
            info.layerCount=0;info.layers=nullptr;
            log("frame_submission layerCount=0 layers=nullptr");ok=xr("xrEndFrame",[&]{return xrEndFrame(session,&info);});break;
        }
        case Op::request_exit:ok=xr("xrRequestExitSession",[&]{return xrRequestExitSession(session);});break;
        case Op::end_session:ok=xr("xrEndSession",[&]{return xrEndSession(session);});break;
        case Op::destroy_space:ok=!space || xr("xrDestroySpace",[&]{return xrDestroySpace(space);});if(ok)space=XR_NULL_HANDLE;break;
        case Op::destroy_session:ok=!session || xr("xrDestroySession",[&]{return xrDestroySession(session);});if(ok)session=XR_NULL_HANDLE;break;
        case Op::release_queue:
            // Clear ownership only inside the executed call, never on refusal.
            if(queue_owner){timed("queue-device.Release",[&]{auto refs=queue_owner->Release();queue_owner=nullptr;return refs;});}
            if(queue){timed("queue.Release",[&]{auto refs=queue->Release();queue=nullptr;return refs;});}
            ok=true;break;
        case Op::release_device:
            if(device){timed("device.Release",[&]{auto refs=device->Release();device=nullptr;return refs;});}
            if(adapter){timed("adapter.Release",[&]{auto refs=adapter->Release();adapter=nullptr;return refs;});}
            if(factory){timed("factory.Release",[&]{auto refs=factory->Release();factory=nullptr;return refs;});}
            ok=true;break;
        case Op::destroy_instance:ok=!instance || xr("xrDestroyInstance",[&]{return xrDestroyInstance(instance);});if(ok)instance=XR_NULL_HANDLE;break;
        default:break;
        }
        if(op==Op::begin_session && ok)session_started=true;
        ok &= !call_refused;
        if(!ok && !admission_closed)log(std::string("refusal_or_error=")+name(op));
        // Core and image owner keep the sticky veto independently. A later
        // legal STOPPING event must remain observable for orderly retirement.
        return {ok,state,admission_closed,should_render,frame_discarded,reference_changed};
    }
private:
#include "real_colour.inc"
#include "real_stereo.inc"
public:
    ColourBackend* colour_backend() override {return this;}
    bool frame_calls_legal() const override {return !fatal_loss;}
    ImageReply image(ImageOp op,const ImageRequest& q) override {return image_call(op,q);}
    void begin_retirement(int64_t end) override {cleanup_mode=true;deadline=std::min(deadline,end);}
};
}
std::unique_ptr<Backend> make_real_backend(Clock& c,Log log,Mode mode) { return std::make_unique<Real>(c,log,mode); }
}
