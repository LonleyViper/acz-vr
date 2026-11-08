// acz_testhmd: a SteamVR test HMD for unattended Ace Combat Zero VR runs.
// Display: same as SteamVR's stock null driver (window HMD on the desktop, 1512x1680 per eye, 90 Hz,
// raw projection tan +-1 on both axes = 90x90 deg). Pose: scripted by a pose file (see pose_core.h),
// slewed smoothly at a configurable rate, polled every ~50 ms by the pose thread.
// Based on the structure of Valve's OpenVR SDK v2.5.1 samples/drivers/drivers/simplehmd (BSD-3-Clause);
// see tools/testhmd/THIRD_PARTY.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <share.h>
#include <string>
#include <thread>

#include "openvr_driver.h"
#include "pose_core.h"

using namespace testhmd;

namespace {

const char* const kSection = "driver_acz_testhmd";

// ---------------------------------------------------------------------------------------------
// Settings helpers (missing keys fall back to the defaults that match the null driver).
int32_t SetInt(const char* key, int32_t def) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    int32_t v = vr::VRSettings()->GetInt32(kSection, key, &e);
    return e == vr::VRSettingsError_None ? v : def;
}
float SetFloat(const char* key, float def) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    float v = vr::VRSettings()->GetFloat(kSection, key, &e);
    return e == vr::VRSettingsError_None ? v : def;
}
bool SetBool(const char* key, bool def) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    bool v = vr::VRSettings()->GetBool(kSection, key, &e);
    return e == vr::VRSettingsError_None ? v : def;
}
std::string SetString(const char* key, const char* def) {
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    char buf[1024] = {};
    vr::VRSettings()->GetString(kSection, key, buf, sizeof(buf), &e);
    if (e != vr::VRSettingsError_None || !buf[0]) return def;
    return buf;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring AczVrDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\Users\\Public");
    d += L"\\acz-vr";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// ---------------------------------------------------------------------------------------------
// Bounded file log (shared access: readers never block it). Rotates to <log>.1 above 1 MiB.
class FileLog {
public:
    void Open(const std::wstring& path) { path_ = path; }
    void Line(const char* fmt, ...) {
        if (path_.empty()) return;
        char msg[768];
        va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
        SYSTEMTIME t; GetLocalTime(&t);
        std::lock_guard<std::mutex> g(mu_);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (1u << 20)))
            MoveFileExW(path_.c_str(), (path_ + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        FILE* f = _wfsopen(path_.c_str(), L"ab", _SH_DENYNO);
        if (!f) return;
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, msg);
        fclose(f);
    }
private:
    std::wstring path_;
    std::mutex mu_;
};

FileLog g_log;

void DriverLog(const char* fmt, ...) {
    char msg[768];
    va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
    if (vr::VRDriverLog()) vr::VRDriverLog()->Log(msg);
    g_log.Line("%s", msg);
}

// Reads the whole pose file with full sharing (the writer may replace it at any moment). false = absent/unreadable.
bool ReadSmallFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.assign(buf, got);
    return true;
}

// ---------------------------------------------------------------------------------------------
struct DisplayConfig {
    int32_t window_x = 0, window_y = 0, window_width = 2160, window_height = 1200;
    int32_t render_width = 1512, render_height = 1680;
};

class TestDisplay : public vr::IVRDisplayComponent {
public:
    explicit TestDisplay(const DisplayConfig& c) : c_(c) {}
    bool IsDisplayOnDesktop() override { return true; }     // window HMD on the desktop, like the null driver
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override { *w = c_.render_width; *h = c_.render_height; }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override {
        *y = 0;
        *w = c_.window_width / 2;
        *h = c_.window_height;
        *x = eye == vr::Eye_Left ? 0 : c_.window_width / 2;
    }
    void GetProjectionRaw(vr::EVREye, float* l, float* r, float* t, float* b) override {
        *l = -1.0f; *r = 1.0f; *t = -1.0f; *b = 1.0f;   // 90x90 deg per eye: our null-HMD logs show eye fov -45/+45/+45/-45
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t c{};
        c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
        c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
        return c;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t* out, vr::EVREye, uint32_t, float u, float v) override {
        out->v[0] = u; out->v[1] = v;   // identity distortion: its inverse is the identity
        return true;
    }
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        *x = c_.window_x; *y = c_.window_y; *w = c_.window_width; *h = c_.window_height;
    }
private:
    DisplayConfig c_;
};

// ---------------------------------------------------------------------------------------------
class TestHmd : public vr::ITrackedDeviceServerDriver {
public:
    TestHmd() {
        serial_ = SetString("serialNumber", "ACZ-TESTHMD-0001");
        model_ = SetString("modelNumber", "ACZ Test HMD");
        DisplayConfig dc;
        dc.window_x = SetInt("windowX", 0);
        dc.window_y = SetInt("windowY", 0);
        dc.window_width = SetInt("windowWidth", 2160);
        dc.window_height = SetInt("windowHeight", 1200);
        dc.render_width = SetInt("renderWidth", 1512);
        dc.render_height = SetInt("renderHeight", 1680);
        display_ = std::make_unique<TestDisplay>(dc);
        freq_ = SetFloat("displayFrequency", 90.0f);
        vsync_to_photons_ = SetFloat("secondsFromVsyncToPhotons", 0.01111111f);
        default_slew_ = SetFloat("defaultSlewDegPerSec", 120.0f);
        pos_rate_ = SetFloat("positionSlewMetresPerSec", 1.0f);
        head_model_ = SetBool("applyHeadModel", false);
        roll_fix_ = SetBool("localRollFix", true);   // pose_core.h local_roll_fix (testhmd-first-flights.md section 3)
        local_ang_vel_ = SetString("angularVelocityFrame", "world") == std::string("local");
        std::string pf = SetString("poseFile", "");
        pose_file_ = pf.empty() ? AczVrDir() + L"\\testhmd-pose.txt" : Widen(pf);
        DriverLog("testhmd: serial=%s model=%s window=%d,%d %dx%d render=%dx%d freq=%.2f slew=%.1f deg/s posrate=%.2f m/s headmodel=%d angvel=%s localrollfix=%d",
                  serial_.c_str(), model_.c_str(), dc.window_x, dc.window_y, dc.window_width, dc.window_height, dc.render_width,
                  dc.render_height, freq_, default_slew_, pos_rate_, head_model_ ? 1 : 0, local_ang_vel_ ? "local" : "world", roll_fix_ ? 1 : 0);
    }

    virtual ~TestHmd() {   // vrserver normally calls Deactivate first; never leave a joinable thread behind
        if (active_.exchange(false) && thread_.joinable()) thread_.join();
    }

    vr::EVRInitError Activate(uint32_t id) override {
        index_ = id;
        vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(id);
        vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String, model_.c_str());
        vr::VRProperties()->SetStringProperty(c, vr::Prop_ManufacturerName_String, "acz-vr");
        vr::VRProperties()->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float,
                                             vr::VRSettings()->GetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float));
        vr::VRProperties()->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, freq_);
        vr::VRProperties()->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        vr::VRProperties()->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, vsync_to_photons_);
        vr::VRProperties()->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
        vr::VRProperties()->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, true);
        vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{acz_testhmd}/input/acz_testhmd_profile.json");

        {
            std::lock_guard<std::mutex> g(mu_);
            target_ = PoseTarget{};
            target_.slew = default_slew_;
            st_ = SlewState{};
            st_.pos_rate = pos_rate_;
            set_target(st_, target_);
            pose_ = MakePose();
        }
        active_ = true;
        thread_ = std::thread(&TestHmd::PoseThread, this);
        DriverLog("testhmd: activated as device %u; pose file %ls", id, pose_file_.c_str());
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        if (active_.exchange(false) && thread_.joinable()) thread_.join();
        index_ = vr::k_unTrackedDeviceIndexInvalid;
        DriverLog("testhmd: deactivated");
    }
    void EnterStandby() override {}
    void* GetComponent(const char* name) override {
        if (std::strcmp(name, vr::IVRDisplayComponent_Version) == 0) return display_.get();
        return nullptr;
    }
    void DebugRequest(const char*, char* resp, uint32_t n) override { if (n) resp[0] = 0; }
    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> g(mu_);
        return pose_;
    }
    const std::string& Serial() const { return serial_; }

private:
    vr::DriverPose_t MakePose() const {   // caller holds mu_
        vr::DriverPose_t p{};
        p.qWorldFromDriverRotation.w = 1.0;
        p.qDriverFromHeadRotation.w = 1.0;
        // localRollFix (default on, pose_core.h local_roll_fix): the pose file is the look in the game's LOCAL frame, not in driver space.
        // q' = R q R^-1 (R = Rz(180)): position, linear velocity and BOTH angular velocities (world: dq'/dt = 0.5 (R w) q'; head frame:
        // dq'/dt = 0.5 q' (R w)) map by R as well.
        const Quat q = roll_fix_ ? local_roll_fix(st_.q) : st_.q;
        const Vec3 pos = roll_fix_ ? local_roll_fix(st_.pos) : st_.pos;
        const Vec3 lv = roll_fix_ ? local_roll_fix(st_.lin_vel) : st_.lin_vel;
        const Vec3& wsrc = local_ang_vel_ ? st_.ang_vel_local : st_.ang_vel_world;
        const Vec3 w = roll_fix_ ? local_roll_fix(wsrc) : wsrc;
        p.qRotation.w = q.w; p.qRotation.x = q.x; p.qRotation.y = q.y; p.qRotation.z = q.z;
        p.vecPosition[0] = pos.x; p.vecPosition[1] = pos.y; p.vecPosition[2] = pos.z;
        p.vecVelocity[0] = lv.x; p.vecVelocity[1] = lv.y; p.vecVelocity[2] = lv.z;
        p.vecAngularVelocity[0] = w.x; p.vecAngularVelocity[1] = w.y; p.vecAngularVelocity[2] = w.z;
        p.result = vr::TrackingResult_Running_OK;
        p.poseIsValid = true;
        p.deviceIsConnected = true;
        p.willDriftInYaw = false;
        p.shouldApplyHeadModel = head_model_;
        return p;
    }

    void PollPoseFile() {
        std::string text;
        if (!ReadSmallFile(pose_file_, text)) {
            if (file_state_ != 1) { file_state_ = 1; DriverLog("testhmd: pose file absent; keeping target"); }
            return;
        }
        PoseTarget t;
        std::string why;
        Vec3 keep;
        { std::lock_guard<std::mutex> g(mu_); keep = target_.pos; }
        if (!parse_pose_text(text, default_slew_, keep, t, &why)) {
            if (file_state_ != 2 || why != last_why_) { file_state_ = 2; last_why_ = why; DriverLog("testhmd: pose file rejected (%s); keeping target", why.c_str()); }
            return;
        }
        file_state_ = 0;
        std::lock_guard<std::mutex> g(mu_);
        if (same_target(t, target_)) return;
        target_ = t;
        set_target(st_, t);
        g_log.Line("target yaw=%.2f pitch=%.2f roll=%.2f pos=%.3f,%.3f,%.3f slew=%.1f", t.yaw, t.pitch, t.roll, t.pos.x, t.pos.y,
                   t.pos.z, t.slew);
    }

    void PoseThread() {
        using clk = std::chrono::steady_clock;
        auto last = clk::now();
        auto next_poll = last;
        auto next_log = last + std::chrono::seconds(5);
        while (active_) {
            auto now = clk::now();
            if (now >= next_poll) { PollPoseFile(); next_poll = now + std::chrono::milliseconds(50); }
            vr::DriverPose_t p;
            double remaining;
            {
                std::lock_guard<std::mutex> g(mu_);
                double dt = std::chrono::duration<double>(now - last).count();
                if (dt > 0.1) dt = 0.1;   // a stalled thread must not jump
                remaining = step(st_, dt);
                pose_ = MakePose();
                p = pose_;
            }
            last = now;
            uint32_t idx = index_;
            if (idx != vr::k_unTrackedDeviceIndexInvalid) vr::VRServerDriverHost()->TrackedDevicePoseUpdated(idx, p, sizeof(p));
            if (now >= next_log) {
                next_log = now + std::chrono::seconds(5);
                double y, pi, r;
                Vec3 pos;
                { std::lock_guard<std::mutex> g(mu_); ypr_deg_from_quat(st_.q, y, pi, r); pos = st_.pos; }
                g_log.Line("pose yaw=%.2f pitch=%.2f roll=%.2f pos=%.3f,%.3f,%.3f remaining=%.2f deg w=%.3f,%.3f,%.3f", y, pi, r, pos.x, pos.y,
                           pos.z, remaining / kDeg, p.vecAngularVelocity[0], p.vecAngularVelocity[1], p.vecAngularVelocity[2]);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    std::unique_ptr<TestDisplay> display_;
    std::string serial_, model_;
    float freq_ = 90.f, vsync_to_photons_ = 0.0111f, default_slew_ = 120.f, pos_rate_ = 1.f;
    bool head_model_ = false, local_ang_vel_ = false, roll_fix_ = true;
    std::wstring pose_file_;
    std::atomic<uint32_t> index_{ vr::k_unTrackedDeviceIndexInvalid };
    std::atomic<bool> active_{ false };
    std::thread thread_;
    std::mutex mu_;
    PoseTarget target_;
    SlewState st_;
    vr::DriverPose_t pose_{};
    int file_state_ = -1;   // pose thread only: 0 ok, 1 absent, 2 rejected
    std::string last_why_;
};

// ---------------------------------------------------------------------------------------------
class Provider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        g_log.Open(AczVrDir() + L"\\testhmd.log");
        DriverLog("testhmd: driver init (acz_testhmd, OpenVR driver API %s)", vr::IServerTrackedDeviceProvider_Version);
        hmd_ = std::make_unique<TestHmd>();
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_->Serial().c_str(), vr::TrackedDeviceClass_HMD, hmd_.get())) {
            DriverLog("testhmd: TrackedDeviceAdded failed");
            return vr::VRInitError_Driver_Unknown;
        }
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        DriverLog("testhmd: cleanup");
        hmd_.reset();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {
        vr::VREvent_t ev{};
        while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev))) {}
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
private:
    std::unique_ptr<TestHmd> hmd_;
};

Provider g_provider;

}  // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* iface, int* rc) {
    if (std::strcmp(vr::IServerTrackedDeviceProvider_Version, iface) == 0) return &g_provider;
    if (rc) *rc = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
