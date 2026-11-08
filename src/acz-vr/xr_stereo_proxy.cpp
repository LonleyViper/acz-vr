// J-xr-stereo-output proxy wiring. Default OFF (see xr_stereo_proxy.h).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "xr_stereo_proxy.h"
#include "stereo_pose_provider.h"
#include "d3d12_hook.h"
#include "diagnostics_log.h"
#include "xr_owner_openxr.h"
#include "xr_stereo_d3d12.h"
#include "xr_stereo_output.h"
#include "xr_stereo_log.h"
#include "xr_start.h"            // XR-START: attempt diagnosis + persistent retry policy
#include "stage_detect_win.h" // STAGES: xr_terrain_stage=auto
#include "gpu_copy_step3.h"      // png_stream (J-test8-fixes xr_png_once; PERF streamed)
#include "png_dump_queue.h"      // PERF: one sequential xr_png_once writer
#include "world_dedupe.h"        // WORLD-4 propdedupe
#include "world_clouds.h" // WORLD-CLOUDS
#include "effects_game_clouds.h" // EFFECTS-OUTSIDE (A): gamecloud option -> producer
#include "effects_stage_flags.h" // the stage-bound effects switches (shared with xr_flight_stage.cpp)
#include "ee_e6_locator.h"       // E6-ANCHOR-FREE: the e6loc option inside xr_terrain_stage
#include "frame_pacing.h"        // UNCAP / EXTRAP: uncap:1 / extrap:1 inside xr_terrain_stage
#include "object_interp.h"       // OBJECT-INTERP: interp:1 inside xr_terrain_stage
#include "look_stick_option.h"   // LOOK-STICK GATE: lookstick:block|game inside xr_terrain_stage
#include "eye_res.h"             // EYE-RES: eyeres:<pct> inside xr_terrain_stage (docs/world-render/eye-resolution.md)
#include "eye_ring.h"            // EYE-RING: eyeobj:<0|1>, the per-eye producer targets at the eye size (section 5.1 (a))
#include "recenter.h"            // SEATED RECENTER: rc:/rckey:/rcauto: inside xr_terrain_stage, the key trigger (docs/world-render/recenter.md)
#include "null_head.h"           // NULL-HEAD (test only): nh:/nhs: inside xr_terrain_stage + head.now (docs/world-render/null-head-look.md)
#include "xr_vscreen.h"          // VSCREEN: vscreen:1 inside xr_terrain_stage (the game's screen outside flight)
#include "brief3d.h"             // BRIEF3D: brief3d:1 / b3* inside xr_terrain_stage (tabletop briefing map)
#include "ui3d.h"                // UI3D: ui3d:1 / u3* inside xr_terrain_stage (menus on depth layers, ui-3d.md)
#include "dbr3d.h"               // DEBRIEF-3D: dbr3d:1 / dbr* inside xr_terrain_stage (debrief map on a table; core only, glue pending)
#include "xr_brief3d_core.h"     // BRIEF3D live: make_brief3d_hook (the table on the vscreen layer), the producer configuration
#include "vscreen_capture.h"     // VSEARLY: the Present counter of the early start gate
#include "flight_cycle.h"        // VSEARLY: vsearly:1 inside xr_terrain_stage (XR in the menus, per-flight stage)
#include "xr_flight_stage.h"     // VSEARLY: per-flight stage detection/load (xr_terrain_stage=auto)
#include "../skip-intro/xr_view_ipc.h" // LOOK-STICK GATE: the heartbeat block read by the XINPUT1_4.dll proxy

#include <bcrypt.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>

#ifndef ACZ_VR_SOURCE_PIN
#define ACZ_VR_SOURCE_PIN "unconfigured"
#endif
namespace acz::xr_stereo {
namespace {
// D-232: try-only PoseProvider over the owner's single-slot pose mailbox. Never blocks; false unless
// the sample has valid, tracked, finite eye poses for two views.
class OwnerPoseProvider final : public stereo::PoseProvider {
public:
    bool try_read(stereo::EyePose out[2], uint64_t* predicted_display_time) override {
        xr_owner::PoseSnapshot snap;
        if (!try_read_pose(snap) || !snap.eyes_tracked()) return false;
        for (int i = 0; i < 2; ++i) {
            const auto& e = snap.eyes[static_cast<size_t>(i)];
            for (int k = 0; k < 3; ++k) out[i].position[k] = e.pose.position[k];
            for (int k = 0; k < 4; ++k) out[i].orientation[k] = e.pose.orientation[k];
            out[i].fov[0] = e.fov.left; out[i].fov[1] = e.fov.right; out[i].fov[2] = e.fov.up; out[i].fov[3] = e.fov.down;
        }
        if (predicted_display_time) *predicted_display_time = static_cast<uint64_t>(snap.predicted_time);
        return true;
    }
    // Issue 29: also pass the snapshot's LOCAL space generation (encoded +1) through.
    bool try_read_ex(stereo::EyePose out[2], uint64_t* predicted_display_time, uint32_t* space_generation) override {
        if (space_generation) *space_generation = 0;
        xr_owner::PoseSnapshot snap;
        if (!try_read_pose(snap) || !snap.eyes_tracked()) return false;
        for (int i = 0; i < 2; ++i) {
            const auto& e = snap.eyes[static_cast<size_t>(i)];
            for (int k = 0; k < 3; ++k) out[i].position[k] = e.pose.position[k];
            for (int k = 0; k < 4; ++k) out[i].orientation[k] = e.pose.orientation[k];
            out[i].fov[0] = e.fov.left; out[i].fov[1] = e.fov.right; out[i].fov[2] = e.fov.up; out[i].fov[3] = e.fov.down;
        }
        if (predicted_display_time) *predicted_display_time = static_cast<uint64_t>(snap.predicted_time);
        if (space_generation) *space_generation = encode_space_generation(snap.space_generation);
        last_seq_.store(snap.sequence, std::memory_order_relaxed);
        return true;
    }
    uint64_t last_sequence() const noexcept override { return last_seq_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> last_seq_{0};
};
} // namespace

namespace {
std::atomic<bool> g_attempted{false}, g_stop{false};
std::atomic<bool> g_persistent_retry{true}; // XR-START: ACZ_XR_RETRY=0 turns the persistent retry off (read once when armed)
// LOOK-STICK GATE (docs/world-render/look-stick-gate.md): lookstick:block (default) asks the XINPUT1_4.dll proxy to centre the right stick while the
// in-flight world is composed from the game camera; lookstick:game never creates the mapping. Set when the terrain option is parsed.
std::atomic<bool> g_look_stick_block{false};
acz::vscreen::Options g_vscreen_opt;   // VSCREEN: parsed once by the monitor thread before the first attempt (only that thread reads it)
acz::recenter::Options g_recenter_opt; // SEATED RECENTER: parsed once by the monitor thread before the first attempt (only that thread reads it); defaults = on, Home
bool g_early = false;                  // VSEARLY: vsearly:1 effective (monitor thread only; docs/world-render/vsearly.md)
// J-xr-log: every `xr-stereo;` line goes to the dedicated xr-stereo.log
// (xr_stereo_log.h). Lifecycle / first-frame / result lines (at most 48) are
// still mirrored into the shared log's bounded STARTUP evidence partition
// (Event::device, shared 256-record quota); periodic summaries no longer use the
// shared ordinary ring unless the dedicated file is unavailable (legacy fallback).
void shared_emit(bool startup_partition, const char* text) noexcept {
    diagnostics::emit(diagnostics::record(startup_partition ? diagnostics::Event::device : diagnostics::Event::requirements_metadata,
        0, 0, 0, text));
}
// U-soak: the dedicated xr-stereo.log keeps its routing after a later I/O failure of the SHARED log (feature_logs_enabled, never enabled()).
bool logging_enabled() noexcept {
    auto* log = diagnostics::sink();
    return log && log->feature_logs_enabled();
}
// Process-lifetime storage: never wait for logging from a DLL static destructor.
evidence::Router& g_evidence = *new evidence::Router(&shared_emit, &logging_enabled);
void emit(bool lifecycle, const char* text) noexcept { g_evidence.emit(lifecycle, text); }
// SEATED RECENTER (docs/world-render/recenter.md): polled by the XR attempt thread every ~20 ms (the same thread that already ticks the look-stick
// heartbeat every 100 ms). The key is read with GetAsyncKeyState (no hook, no window message, nothing sent to the game); the decision logic is
// recenter.h (offline-tested). A request calls Owner::request_recenter(); a log line is written for the request and again when the new neutral is
// captured (before/after offsets, the neutral shift).
class RecenterPoller {
public:
    void poll(xr_owner::Owner& owner, uint64_t flight_composites) noexcept {
        const auto& o = g_recenter_opt;
        if (!o.on) return;
        if (!ctl_) { ctl_.emplace(o); announce(); }
        acz::recenter::Input in;
        in.now_ms = static_cast<int64_t>(GetTickCount64());
        in.flight_composites = flight_composites;
        in.key_down = (GetAsyncKeyState(static_cast<int>(o.vk)) & 0x8000) != 0 && (!o.focus || game_foreground());
        acz::recenter::Head head;
        xr_owner::PoseSnapshot snap;
        if (owner.try_read_pose(snap) && snap.eyes_tracked()) {
            float pos[2][3], ori[2][4];
            for (int i = 0; i < 2; ++i) {
                const auto& e = snap.eyes[static_cast<size_t>(i)];
                for (int k = 0; k < 3; ++k) pos[i][k] = e.pose.position[k];
                for (int k = 0; k < 4; ++k) ori[i][k] = e.pose.orientation[k];
            }
            head = acz::recenter::head_of(pos, ori);
            in.generation = snap.space_generation;
            if (head.ok) in.head = &head;
        }
        const acz::recenter::Output out = ctl_->step(in);
        if (out.applied && ++logged_ <= 60) emit(false, acz::recenter::format_applied(out).c_str());
        if (out.request) {
            owner.request_recenter();
            if (++logged_ <= 60) emit(false, acz::recenter::format_request(out, in.generation).c_str());
        }
    }
private:
    static bool game_foreground() noexcept {
        DWORD pid = 0;
        const HWND w = GetForegroundWindow();
        return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
    }
    void announce() noexcept {
        char t[300];
        std::snprintf(t, sizeof t, "xr-stereo;event=recenter-armed;%s;trigger=key(GetAsyncKeyState,attempt-thread);controller-chord=not-implemented;mode=instant", acz::recenter::describe(g_recenter_opt).c_str());
        emit(true, t);
    }
    std::optional<acz::recenter::Controller> ctl_;
    unsigned logged_ = 0;
};
// NULL-HEAD (TEST ONLY; null_head.h, docs/world-render/null-head-look.md): nh:1 inside xr_terrain_stage. The attempt thread polls
// %LOCALAPPDATA%\acz-vr\head.now (at most every 250 ms, deleted when read) and posts each command to the mailbox; the owner thread's PoseFilter slews
// the synthetic head and rotates the located views/head before the snapshot and the consumer. Off (no filter, no file poll) unless nh:1.
acz::nullhead::Options g_nullhead_opt;   // parsed once by the monitor thread before the first attempt (only that thread reads it)
acz::nullhead::Mailbox g_nullhead_mail;  // process lifetime: single writer (the attempt thread), single reader (the current owner thread)
class NullHeadFilter final : public xr_owner::PoseFilter {
public:
    explicit NullHeadFilter(double dps) : ctl_(dps, g_nullhead_mail.sequence()) {}   // commands posted before this attempt are not replayed
    void apply(int64_t now_ms, uint32_t generation, bool views_ok, xr_owner::ViewSample& v, bool head_ok, xr_owner::HeadSample& h) noexcept override {
        const acz::nullhead::StepOut o = ctl_.step(now_ms, generation, &g_nullhead_mail);
        if (acz::nullhead::apply(o.cur, views_ok, v, head_ok, h)) ++rotated_;
        try {
            const std::string c = acz::nullhead::format_change(o, now_ms);
            if (!c.empty() && ++changes_ <= 400) emit(false, c.c_str());
            if (o.summary) emit(false, acz::nullhead::format_summary(o, ctl_, rotated_).c_str());
        } catch (...) {}
    }
private:
    acz::nullhead::Controller ctl_;
    uint64_t rotated_ = 0;
    unsigned changes_ = 0;
};
class HeadTriggerPoller {
public:
    void poll() noexcept {
        if (!g_nullhead_opt.on) return;
        const ULONGLONG now = GetTickCount64();
        if (now < next_) return;
        next_ = now + static_cast<ULONGLONG>(acz::nullhead::kPollMs);
        try {
            wchar_t base[MAX_PATH];
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            if (!n || n >= MAX_PATH - 32) return;
            std::wstring path(base, n); path += L"\\acz-vr\\head.now";
            const DWORD a = GetFileAttributesW(path.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return;
            char buf[128] = {};
            DWORD got = 0;
            const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;   // being written: the next poll reads it
            const BOOL rd = ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
            CloseHandle(f);
            if (!rd) return;
            size_t len = got;
            if (len >= 3 && uint8_t(buf[0]) == 0xEF && uint8_t(buf[1]) == 0xBB && uint8_t(buf[2]) == 0xBF) { std::memmove(buf, buf + 3, len - 3); len -= 3; buf[len] = 0; }   // UTF-8 BOM
            if (len == 0) return;   // created but not written yet
            const bool deleted = DeleteFileW(path.c_str()) != FALSE;
            acz::nullhead::Command cmd; std::string why;
            const bool ok = acz::nullhead::parse_command(buf, len, cmd, why);
            if (ok) g_nullhead_mail.post(cmd);
            if (++logged_ <= 400) emit(false, acz::nullhead::format_command(cmd, ok, why, deleted ? "deleted" : "delete-failed").c_str());
        } catch (...) {}
    }
private:
    ULONGLONG next_ = 0;
    unsigned logged_ = 0;
};
// LOOK-STICK GATE: The writer side: one named mapping (created lazily on the first in-flight tick), written only by the attempt loop's thread.
class LookStickHeartbeat {
public:
    void tick(uint64_t composites, bool session_hooked) noexcept {
        const bool want = g_look_stick_block.load(std::memory_order_relaxed) && session_hooked;
        const bool was = hb_.active();
        const bool on = hb_.tick(composites, want);
        if (on && !blk_ && !failed_) open();
        if (blk_ && (on || was)) acz::xr_view_ipc::publish(blk_, on, GetTickCount64(), pid_);   // on: fresh stamp; turning off: explicit inactive
        if (on != was) {
            ++transitions_;
            if (transitions_ <= 40) {
                char t[200];
                std::snprintf(t, sizeof t, "xr-stereo;event=look-stick-gate;state=%s;mapping=%s;composites=%llu;transitions=%u;reader=XINPUT1_4.dll(skip-intro,xr_block_look_stick)",
                    on ? "block" : "pass", blk_ ? "ok" : refused_ ? "exists-not-adopted" : failed_ ? "create-failed" : "none", static_cast<unsigned long long>(composites), transitions_);
                emit(false, t);
            }
        }
    }
    void stop() noexcept { if (blk_ && hb_.active()) acz::xr_view_ipc::publish(blk_, false, GetTickCount64(), pid_); hb_ = {}; }
private:
    void open() noexcept {
        // Kept for the process lifetime (the reader holds its own handle; a stale stamp opens the gate on its side).
        // D-358: the name is bound to THIS game process (the XInput proxy reader runs in the same process and derives the same name).
        pid_ = GetCurrentProcessId();
        wchar_t name[acz::xr_view_ipc::kMappingNameMax];
        if (!acz::xr_view_ipc::mapping_name(name, std::size(name), pid_)) { failed_ = true; return; }
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(acz::xr_view_ipc::Block)), name);
        const bool existed = map_ && GetLastError() == ERROR_ALREADY_EXISTS;
        if (!map_) { failed_ = true; return; }
        void* v = MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, sizeof(acz::xr_view_ipc::Block));
        if (!v) { CloseHandle(map_); map_ = nullptr; failed_ = true; return; }
        auto* b = static_cast<acz::xr_view_ipc::Block*>(v);
        // Instance generation: never share a mapping with a live writer or another process's block (single-writer protocol).
        if (existed && !acz::xr_view_ipc::can_adopt(b, GetTickCount64(), acz::xr_view_ipc::kMaxStaleMs, pid_)) {
            UnmapViewOfFile(v); CloseHandle(map_); map_ = nullptr; failed_ = refused_ = true; return;
        }
        blk_ = b;
    }
    acz::xr_view_ipc::Heartbeat hb_;
    HANDLE map_ = nullptr;
    acz::xr_view_ipc::Block* blk_ = nullptr;
    uint32_t pid_ = 0;
    bool failed_ = false, refused_ = false;
    unsigned transitions_ = 0;
};
LookStickHeartbeat g_look_stick_hb;   // used only by the XR attempt thread
// STAGES: xr_terrain_stage=auto. Up to 3 read-only scans (3 s apart, only while nothing at all matched); on success `id` = the detected
// group's representative stage. Every attempt and the outcome are logged; false = no terrain (game world image).
// R-3: each scan is bounded (stage_scan.h): likely regions first, decisive early stop, 1 GiB read cap (2 GiB on the last attempt, which
// only runs when nothing at all matched before), <= 4 MiB chunks with a yield after each, resident pages only (never pages memory in);
// a `terrain-auto-cost` line logs what it cost. R-11: an orderly stop (g_stop) ends a scan and the 3 s wait early.
bool detect_auto_stage(std::string& id, std::string& table_sha) {
    char line[900];
    acz::stagefp::Table table;
    std::string why;
    const std::string path = acz::stagefp::table_path();
    if (!acz::stagefp::load_table_file(path, table, why)) {
        std::snprintf(line, sizeof line, "xr-stereo;event=terrain-disabled;reason=auto-table;detail=%s;path=%s;fallback=game-world-image;hint=stage_fingerprints.py+install-assets.ps1",
            why.c_str(), path.c_str());
        emit(true, line);
        return false;
    }
    acz::stagefp::Result r;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        acz::stagefp::Matcher m(table);
        acz::stagefp::ScanStats st;
        acz::stagefp::ScanLimits lim;
        if (attempt == 3) lim.max_bytes = 2ull << 30;
        lim.stop = []() noexcept { return g_stop.load(std::memory_order_acquire); };
        acz::stagefp::scan_process(m, lim, st);
        r = m.decide();
        std::string top;
        for (size_t i = 0; i < r.ranking.size() && i < 5; ++i)
            top += (i ? "," : "") + table.groups[size_t(r.ranking[i].first)].stage + ":" + std::to_string(r.ranking[i].second);
        std::snprintf(line, sizeof line, "xr-stereo;event=terrain-auto;attempt=%d;result=%s;reason=%s;stage=%s;best=%u;second=%u;min-hits=%u;sigs-seen=%u/%zu;sources=%s;top=%s;"
            "regions=%u;bytes=%llu;ms=%u;truncated=%u;skipped-gpu=%u;skipped-image=%u;read-failures=%u;table=%s",
            attempt, acz::stagefp::kind_name(r.kind), r.reason.c_str(), r.stage.empty() ? "-" : r.stage.c_str(), r.best, r.second, table.min_hits, r.sigs_seen,
            table.sigs.size(), m.source_summary().c_str(), top.empty() ? "-" : top.c_str(), st.regions, static_cast<unsigned long long>(st.bytes), st.ms,
            st.truncated ? 1u : 0u, st.regions_skipped_gpu, st.regions_skipped_image, st.read_failures, path.c_str());
        emit(true, line);
        std::string hits;
        for (uint32_t i = 0; i < st.hit_n; ++i) {
            char h[64];
            std::snprintf(h, sizeof h, "%s%s@%u:%lluM/%lluK:%u", i ? "," : "", acz::stagefp::class_name(st.hit[i].cls), st.hit[i].rank,
                static_cast<unsigned long long>(st.hit[i].alloc_span >> 20), static_cast<unsigned long long>(st.hit[i].size >> 10), st.hit[i].hits);
            hits += h;
        }
        std::snprintf(line, sizeof line, "xr-stereo;event=terrain-auto-cost;attempt=%d;stop=%s;bytes-read=%llu;nonresident-skipped=%llu;eligible=%llu;"
            "regions=%u/%u;cap=%llu;candidate-at=%llu;decisive-at=%llu;ms=%u;yields=%u;ws-query-failures=%u;skipped-own=%u;hit-regions=%s",
            attempt, st.stop, static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.bytes_nonresident),
            static_cast<unsigned long long>(st.bytes_eligible), st.regions, st.regions_eligible, static_cast<unsigned long long>(st.cap_bytes),
            static_cast<unsigned long long>(st.candidate_at), static_cast<unsigned long long>(st.decisive_at), st.ms, st.yields,
            st.ws_query_failures, st.regions_skipped_own, hits.empty() ? "-" : hits.c_str());
        emit(true, line);
        if (r.kind != acz::stagefp::Result::Kind::no_match || r.sigs_seen > 0 || attempt == 3 || g_stop.load(std::memory_order_acquire)) break;
        for (int w = 0; w < 30 && !g_stop.load(std::memory_order_acquire); ++w) Sleep(100);   // 3 s, ends early on an orderly stop
        if (g_stop.load(std::memory_order_acquire)) break;
    }
    if (r.kind != acz::stagefp::Result::Kind::match) {
        std::snprintf(line, sizeof line, "xr-stereo;event=terrain-disabled;reason=auto-%s;detail=%s;best=%u;second=%u;fallback=game-world-image",
            r.kind == acz::stagefp::Result::Kind::ambiguous ? "ambiguous" : "no-match", r.reason.c_str(), r.best, r.second);
        emit(true, line);
        return false;
    }
    const auto& g = table.groups[size_t(r.group)];
    std::string members;
    for (const auto& s : g.members) members += (members.empty() ? "" : "+") + s;
    std::snprintf(line, sizeof line, "xr-stereo;event=terrain-auto-selected;stage=%s;identical-members=%s", g.stage.c_str(), members.c_str());
    emit(true, line);
    id = g.stage;
    table_sha = g.zvrs_sha256;
    return true;
}
// TAKEOFF (docs/world-render/takeoff-stage.md): the airbase stage of the takeoff segment (conv.takeoff_id, default k00a) loaded next to the mission stage:
// its own terrain, atlas, environment and props (same options), generation 2 so the backend re-uploads on a switch. Any failure = no takeoff stage
// (the mission stage everywhere, the previous behaviour), logged once. Headset test 19: mission 02 took off on k00a's runway while m02a was drawn.
std::shared_ptr<const TerrainRuntime> load_takeoff_runtime(const TerrainRuntime& main, const acz::props::Options& popt) {
    char tt[420];
    const char* tid = main.conv.takeoff_id;
    if (!tid[0] || std::strcmp(tid, main.stage.id) == 0) {
        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-takeoff-off;reason=%s;stage=%s", tid[0] ? "same-as-mission-stage" : "option-off", main.stage.id);
        emit(true, tt);
        return nullptr;
    }
    auto t = std::make_shared<TerrainRuntime>();
    const std::string path = acz::terrain::stage_path(tid);
    const acz::terrain::LoadStatus st = acz::terrain::load_stage_file(path, t->stage);
    if (st != acz::terrain::LoadStatus::ok) {
        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-takeoff-off;reason=load-failed;status=%s;takeoff=%s;path=%s;fallback=mission-stage-only",
            acz::terrain::status_name(st), tid, path.c_str());
        emit(true, tt);
        return nullptr;
    }
    t->conv = main.conv;
    acz::terrain::apply_sea_convention(t->stage, t->conv);
    acz::terrain::build_atlas(t->stage, t->atlas, t->conv.clut_raw);
    if (t->conv.detail) {   // DTEX (dtex:1): the takeoff stage's own detail texture
        std::string dwhy;
        acz::terrain::attach_detail(tid, t->conv, t->atlas, dwhy);
        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-detail;stage=%s;role=takeoff;state=%s", tid, dwhy.c_str());
        emit(true, tt);
    }
    const bool menv = acz::terrain::adopt_mission_env(t->stage, main.stage, t->conv);   // TAKEOFF-LOOK (toenv:, default mission)
    if (t->conv.env_bits) acz::terrain::build_env_texture(t->stage, t->conv, t->env);
    t->generation = main.generation + 1;
    const char* props = "off";
    if (popt.any()) {
        auto pr = std::make_shared<acz::props::Runtime>();
        if (acz::props::load_props_file(acz::props::props_path(tid), pr->set) == acz::props::LoadStatus::ok && std::strcmp(pr->set.id, t->stage.id) == 0 &&
            acz::props::snap_to_terrain(pr->set, t->stage)) {
            pr->opt = popt;
            acz::props::build_grid(pr->set, pr->opt, pr->grid);
            pr->generation = 1;
            t->props = pr;
            props = "loaded";
        } else props = "unavailable(terrain-only)";
    }
    std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-takeoff-loaded;takeoff=%s;mission=%s;tiles=%u;objects=%u;env=%s;props=%s;payload-sha256=%s;"
        "rule=parked-at-cockpit-height-on-base-ground-and-clear-of-mission-ground,leave-on-cut", t->stage.id, main.stage.id, t->stage.ntiles, t->stage.nobj,
        t->env.empty() ? "no" : menv ? "yes(mission)" : "yes(own)", props, acz::terrain::hex(t->stage.payload_sha, 32).c_str());
    emit(true, tt);
    return t;
}
// Issue 29 (6): build/source identity for the evidence log. Streaming SHA-256 of
// this DLL's own file (bounded reads), on the monitor thread only.
void identity_line() noexcept {
    char hex[65] = "unavailable";
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&identity_line), &self) && GetModuleFileNameW(self, path, MAX_PATH)) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            BCRYPT_HASH_HANDLE hash = nullptr;
            bool ok = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
            static unsigned char buffer[65536];
            uint64_t total = 0;
            while (ok) {
                DWORD got = 0;
                if (!ReadFile(file, buffer, sizeof buffer, &got, nullptr)) { ok = false; break; }
                if (!got) break;
                total += got; if (total > (uint64_t{64} << 20)) { ok = false; break; }
                ok = BCryptHashData(hash, buffer, got, 0) >= 0;
            }
            unsigned char digest[32]{};
            if (hash) { ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0; BCryptDestroyHash(hash); }
            CloseHandle(file);
            if (ok) for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
    }
    char text[diagnostics::text_capacity]{};
    std::snprintf(text, sizeof text, "xr-stereo;event=identity;source-pin=%s;build=%s %s;dll-sha256=%s;no-headset-claim",
        ACZ_VR_SOURCE_PIN, __DATE__, __TIME__, hex);
    emit(true, text);
}
void stop_hook() noexcept { g_stop.store(true, std::memory_order_release); request_stop(); }
stereo::FrameSource* current_source() noexcept {
    auto* registered = registered_frame_source();
    return registered ? registered : stereo::frame_source(); // D-232: producer wiring glue
}
// xr_owner call bracket: every owner-thread runtime/consumer call runs inside
// the proxy-internal creation scope (d3d12_hook.h). The owner thread id is
// logged once per owner thread so a live run can join creation lines to it.
std::atomic<unsigned long> g_owner_tid{0};
void call_bracket(bool enter, bool window) noexcept {
    if (enter) {
        enterProxyCreationScope(window);
        const unsigned long tid = GetCurrentThreadId();
        if (g_owner_tid.exchange(tid, std::memory_order_acq_rel) != tid) {
            char text[96];
            std::snprintf(text, sizeof text, "xr-stereo;event=owner-thread;tid=%lu;creation-scope=on", tid);
            emit(true, text);
        }
    } else leaveProxyCreationScope(window);
}
// XR-START diagnostics (docs/world-render/xr-start.md): one `xr-call` line per start-path call
// (startup, READY wait, first frame, cleanup; xrPollEvent only when it fails) with its Rc, raw
// XrResult and timing, plus the runtime/system identity right after xrCreateInstance/xrGetSystem.
// Owner thread only; emit() only queues.
void xr_result_text(char* out, size_t size, int32_t xr) noexcept {
    if (xr == xr_owner::kNoXrResult) std::snprintf(out, size, "none");
    else std::snprintf(out, size, "%s(%d)", xr_owner::xr_result_name(xr), static_cast<int>(xr));
}
const char* phase_text(xr_owner::Phase p, bool first_frame) noexcept {
    if (first_frame) return "first-frame";
    switch (p) {
    case xr_owner::Phase::startup: return "startup";
    case xr_owner::Phase::await_ready: return "await-ready";
    case xr_owner::Phase::cleanup: return "cleanup";
    default: return "other";
    }
}
class StartObserver final : public xr_owner::CallObserver {
public:
    StartObserver(unsigned attempt, std::shared_ptr<xr_owner::Runtime> rt) : n_(attempt), rt_(std::move(rt)) {}
    void call(const xr_owner::CallRecord& c, xr_owner::Phase p, bool first_frame) noexcept override {
        try {
            char xr[96], text[480];
            xr_result_text(xr, sizeof xr, c.xr);
            xr_owner::RuntimeFacts f;
            const bool facts = rt_ && rt_->runtime_facts(f);
            const bool ok = xr_owner::succeeded(c.rc);
            std::snprintf(text, sizeof text, "xr-stereo;event=xr-call;n=%u;op=%s;rc=%s;xr-result=%s;t-ms=%lld;ms=%lld;phase=%s%s%s", n_,
                xr_owner::op_name(c.op), xr_start::rc_name(c.rc), xr, static_cast<long long>(c.entry_ms), static_cast<long long>(c.exit_ms - c.entry_ms),
                phase_text(p, first_frame), (!ok && facts && f.refusal && (c.op == xr_owner::Op::create_instance || c.op == xr_owner::Op::query_system)) ? ";detail=" : "",
                (!ok && facts && f.refusal && (c.op == xr_owner::Op::create_instance || c.op == xr_owner::Op::query_system)) ? f.refusal : "");
            emit(false, text);
            if (c.op == xr_owner::Op::create_instance && ok && facts && f.instance_props) {
                std::snprintf(text, sizeof text, "xr-stereo;event=xr-runtime;n=%u;runtime=%s;runtime-version=%u.%u.%u;create-instance-ms=%lld", n_, f.runtime_name,
                    unsigned(f.runtime_version >> 48), unsigned((f.runtime_version >> 32) & 0xffff), unsigned(f.runtime_version & 0xffffffffu),
                    static_cast<long long>(c.exit_ms - c.entry_ms));
                emit(false, text);
            } else if (c.op == xr_owner::Op::query_system && facts) {
                std::snprintf(text, sizeof text, "xr-stereo;event=xr-system;n=%u;form-factor=%u(HEAD_MOUNTED_DISPLAY);result=%s;system-id=%llu;system-name=%s;vendor-id=0x%x;"
                    "max-layers=%u;orientation-tracking=%u;position-tracking=%u;get-system-ms=%lld", n_, f.form_factor, xr,
                    static_cast<unsigned long long>(f.system_id), f.system_props ? f.system_name : "unavailable", f.vendor_id, f.max_layers,
                    f.orientation_tracking ? 1u : 0u, f.position_tracking ? 1u : 0u, static_cast<long long>(c.exit_ms - c.entry_ms));
                emit(false, text);
            }
        } catch (...) {}
    }
private:
    unsigned n_;
    std::shared_ptr<xr_owner::Runtime> rt_;
};
// XR-START: the structured attempt line. `retry` = the monitor's decision text.
void start_attempt_line(unsigned n, const xr_start::Diagnosis& d, const xr_owner::Report& r, const char* retry, int64_t next_ms) noexcept {
    char xr[96], text[480];
    if (r.first_cause == xr_owner::Cause::compatibility) {
        // Separate line (the start-attempt line is near the 480-byte line cap):
        // the refusing gate and, for the device gate, every compared field.
        xr_owner::format_compat_refusal(text, sizeof text, n, r);
        emit(true, text);
    }
    xr_result_text(xr, sizeof xr, d.has_call ? d.xr : xr_owner::kNoXrResult);
    std::snprintf(text, sizeof text, "xr-stereo;event=start-attempt;n=%u;outcome=%s;reason=%s;first-failing-call=%s;xr-result=%s;call-rc=%s;call-t-ms=%lld;call-ms=%lld;"
        "first-cause=%s;clean=%u;loss=%u;begun=%u;first-frame-done-ms=%lld;retry=%s;next-attempt-in-ms=%lld",
        n, xr_start::class_name(d.cls), d.reason, d.has_call ? xr_owner::op_name(d.call) : "none", xr, d.has_call ? xr_start::rc_name(d.call_rc) : "none",
        static_cast<long long>(d.has_call ? d.call_entry_ms : -1), static_cast<long long>(d.has_call ? d.call_ms : -1), xr_owner::cause_name(r.first_cause),
        unsigned(r.clean), unsigned(r.loss_observed), r.begun, static_cast<long long>(r.first_frame_done_ms), retry, static_cast<long long>(next_ms));
    emit(true, text);
}
// Monitor thread (pinned module, detached): waits for the producer's first
// published StereoFrame (atomic loads + 100 ms sleeps only), then runs owner
// attempts: one immediate cold-start retry (unchanged) and, unless ACZ_XR_RETRY=0,
// persistent retries with backoff for transient start failures (XR-START).
void monitor(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    // Monitor owns evidence shutdown, after every attempt/final lifecycle line.
    struct EvidenceStop { ~EvidenceStop() {
        if(!g_evidence.shutdown(2000))shared_emit(true,"xr-stereo;event=incomplete-evidence;reason=log-owner-shutdown-timeout");
    }} evidenceStop;
    try {
        identity_line();
        char text[diagnostics::text_capacity]{};
        const ULONGLONG armed = GetTickCount64();
        stereo::FrameSource* src = nullptr;
        Extent source{};
        unsigned waits = 0;
        SourceProbe probe;                 // XR-START: last metadata probe (waiting-for-source evidence)
        // VSEARLY: vsearly:1 (with vscreen:1, xr_hud_layer, the Present hook) starts XR once the game presents steadily (menus) instead of at the
        // producer's first StereoFrame. Without the token nothing here runs and no line is added.
        // NATIVE-VR START (launch-mode.md 3): vsstart:first (default) starts at the game's first Present after Launch Game, vsstart:steady keeps
        // the previous 120-present/3 s gate, vsstart:lazy (or vsearly:0) the lazy first-published-frame start; ACZ_XR_START overrides the token.
        acz::xr_gate::StartMode start_mode = acz::xr_gate::StartMode::first;
        if (auto* log = diagnostics::sink(); log && log->xr_terrain_render()) {
            acz::vscreen::Options vo; std::string vrest, vwhy;
            if (acz::vscreen::extract_options(log->xr_terrain_stage(), vrest, vo, vwhy) && vo.early) {
                char env[32]{};
                const DWORD env_n = GetEnvironmentVariableA("ACZ_XR_START", env, sizeof env);
                const acz::xr_gate::Resolved rs = acz::xr_gate::resolve(vo.start_given, vo.start, env_n && env_n < sizeof env ? env : nullptr);
                start_mode = rs.mode;
                const char* refused = acz::flight::early_refusal(true, vo.on, log->xr_hud_layer(), log->ee_camera_probe());
                if (!refused && start_mode == acz::xr_gate::StartMode::lazy) refused = "vsstart:lazy(as-vsearly:0)";
                g_early = refused == nullptr;
                if (g_early) acz::vscreen::capture_configure(vo);   // counts the game's presents (records only when the XR consumer asks)
                std::snprintf(text, sizeof text, "xr-stereo;event=vsearly-config;effective=%u;reason=%s;start=%s;start-mode=%s;start-mode-source=%s;assumed-src=%ux%u;"
                    "frame-stall-ms=%lld;stage=per-flight-if-auto;retry=after-start-stalls-and-losses", g_early ? 1u : 0u, refused ? refused : "ok",
                    acz::xr_gate::start_rule(start_mode), acz::xr_gate::start_mode_name(start_mode), rs.source, acz::flight::kAssumedSrcW,
                    acz::flight::kAssumedSrcH, static_cast<long long>(acz::flight::kEarlyFrameStallMs));
                emit(true, text);
            }
        }
        bool early_fired = false;
        unsigned probes = 0, probe_none = 0, probe_zero = 0;
        for (;;) {
            if (g_stop.load(std::memory_order_acquire)) { emit(true, "xr-stereo;event=not-started;reason=stop-before-first-frame"); return; }
            src = current_source();
            if (start_ready(src)) {
                probe = probe_source(src);
                ++probes;
                if (!probe.acquired) ++probe_none;
                source = probe.extent;
                // Acquisition takes the producer metadata lock; no eligible
                // publication retries at the existing lazy-start cadence.
                // The probe releases its held slot, consuming that publication;
                // the consumer's first frame must be a later publication.
                if (source.w && source.h) break;
                if (probe.acquired) ++probe_zero;
            }
            acz::xr_gate::Input gate_in;
            gate_in.early_effective = g_early;
            gate_in.presents = g_early ? acz::vscreen::capture_stats().presents : 0;
            gate_in.since_armed_ms = static_cast<int64_t>(GetTickCount64() - armed);
            if (acz::xr_gate::decide(start_mode, gate_in) == acz::xr_gate::Fire::early) {
                early_fired = true;   // VSEARLY: no StereoFrame needed; the eye swapchains are sized for the game's swap chain
                if (!(source.w && source.h)) source = Extent{acz::flight::kAssumedSrcW, acz::flight::kAssumedSrcH};
                break;
            }
            // Waiting status every 5 s (diagnostic only): why the lazy start has not fired.
            // XR-START tokens: try-acquire = result of the last try_acquire_latest(0) probe (not-tried: nothing
            // published yet; none: no eligible publication, e.g. every slot HELD or PRODUCING; ok: a slot was
            // HELD by the probe and released at once, held-slot/held-frame-id name it).
            if ((++waits % 50) == 1) {
                std::snprintf(text, sizeof text, "xr-stereo;event=waiting-for-source;src=%s;registered=%d;published=%llu;skipped=%llu;extent=%ux%u;waited-ms=%llu;"
                    "try-acquire=%s;held-slot=%d;held-frame-id=%llu;held-released=%u;probes=%u;probe-none=%u;probe-zero-extent=%u",
                    src ? "present" : "null", registered_frame_source() ? 1 : 0,
                    static_cast<unsigned long long>(src ? src->frames_published() : 0), static_cast<unsigned long long>(src ? src->frames_skipped() : 0),
                    source.w, source.h, static_cast<unsigned long long>(GetTickCount64() - armed),
                    !probe.tried ? "not-tried" : probe.acquired ? "ok" : "none", probe.acquired ? int(probe.slot) : -1,
                    static_cast<unsigned long long>(probe.frame_id), probe.acquired ? 1u : 0u, probes, probe_none, probe_zero);
                emit(true, text);
            }
            Sleep(100);
        }
        if (early_fired) {   // VSEARLY (src may be null: nothing published yet)
            std::snprintf(text, sizeof text, "xr-stereo;event=source-ready;published=%llu;waited-ms=%llu;start=early-game-presents;presents=%llu;src-extent=%ux%u;start-mode=%s",
                static_cast<unsigned long long>(src ? src->frames_published() : 0), static_cast<unsigned long long>(GetTickCount64() - armed),
                static_cast<unsigned long long>(acz::vscreen::capture_stats().presents), source.w, source.h, acz::xr_gate::start_mode_name(start_mode));
        } else
        std::snprintf(text, sizeof text, "xr-stereo;event=source-ready;published=%llu;skipped=%llu;waited-ms=%llu;start=lazy-first-published-frame;"
            "probe-slot=%u;probe-frame-id=%llu;probes=%u;probe-none=%u",
            static_cast<unsigned long long>(src->frames_published()), static_cast<unsigned long long>(src->frames_skipped()),
            static_cast<unsigned long long>(GetTickCount64() - armed), probe.slot, static_cast<unsigned long long>(probe.frame_id), probes, probe_none);
        emit(true, text);
        const ULONGLONG source_ready_tick = GetTickCount64();
        Settings settings;
        settings.assumed_src_w = source.w; settings.assumed_src_h = source.h;
        char cap[32]{};
        const DWORD cap_size = GetEnvironmentVariableA("ACZ_XR_STEREO_CAP_WIDTH", cap, sizeof cap);
        settings.cap_width = configured_cap(cap_size < sizeof cap ? cap : nullptr);
        // J-feather: keys xr_edge_feather / xr_world_quad_distance_m (default OFF / 40 m,
        // latched only with xr_stereo_output); the consumer clamps them again.
        if (auto* log = diagnostics::sink(); log && log->xr_comfort_configured()) {
            settings.edge_feather = clamp_feather(log->xr_edge_feather());
            settings.quad_distance_m = clamp_quad_distance(log->xr_world_quad_distance_m());
            settings.comfort_log = true;
        }
        // J-world-in-projection: key xr_world_in_projection (default OFF, latched only with xr_stereo_output).
        if (auto* log = diagnostics::sink(); log && log->xr_world_in_projection()) settings.world_in_projection = true;
        // J-world-hold / J-world-extend: keys xr_world_hold_last / xr_world_extend (default OFF, latched only with xr_world_in_projection).
        if (auto* log = diagnostics::sink(); log && log->xr_world_hold_last()) settings.world_hold_last = true;
        if (auto* log = diagnostics::sink(); log && log->xr_world_extend()) settings.world_extend = true;
        if (auto* log = diagnostics::sink(); log && log->xr_world_cache()) settings.world_cache = true; // J-world-cache (default OFF)
        // J-terrain-render: keys xr_terrain_render + xr_terrain_stage (default OFF, latched only with xr_world_in_projection). Any parse/load failure logs once and leaves the
        // setting null: the frame keeps the game's own world exactly as with the key off.
        if (auto* log = diagnostics::sink(); log && log->xr_terrain_render()) {
            auto rt = std::make_shared<TerrainRuntime>();
            std::string id, why;
            char tt[420];
            std::string stage_value;
            if (log->xr_terrain_preset_vrn())   // D-400: the preset was expanded at config parse; log the result once
                emit(true, (std::string("xr-stereo;event=preset;vrn=1;expanded=") + log->xr_terrain_stage()).c_str());
            acz::props::Options popt;   // WORLD-3: props:1 / trees:1 (+ treescale/treerange/proprange/propcap) are removed before the terrain parser sees the value
            // E6-ANCHOR-FREE: e6loc:auto|probe|content selects the E6 camera feed (ee_camera_probe_win.cpp); removed here, a value without it is unchanged.
            // LOOK-STICK GATE: lookstick:block (default) | game removed first; a value without it is unchanged.
            std::string ls_rest;
            acz::look_stick::Mode ls_mode = acz::look_stick::Mode::block;
            // VSCREEN (vscreen.h, docs/world-render/vscreen.md): vscreen:/vs* removed first; a value without them is unchanged.
            std::string vs_rest;
            const bool vs_ok = acz::vscreen::extract_options(log->xr_terrain_stage(), vs_rest, g_vscreen_opt, why);
            if (!vs_ok) { g_vscreen_opt = acz::vscreen::Options{}; vs_rest = log->xr_terrain_stage(); }
            if (!vs_ok || g_vscreen_opt.any_token) {
                std::snprintf(tt, sizeof tt, "xr-stereo;event=vscreen-option;ok=%u;%s;detail=%s", vs_ok ? 1u : 0u, acz::vscreen::describe(g_vscreen_opt).c_str(),
                    vs_ok ? (settings.hud_layer || log->xr_hud_layer() ? "needs-xr_hud_layer(present)" : "needs-xr_hud_layer(ABSENT:off)") : why.c_str());
                emit(true, tt);
            }
            // BRIEF3D (brief3d.h, docs/world-render/brief3d.md): brief3d:/b3* removed here; a value without them is unchanged. With every
            // prerequisite the producer (brief3d_live.h: the d3d12 hooks' per-eye map replay) is configured here and the XR attempt wraps its vscreen
            // hook with the table (make_brief3d_hook); otherwise nothing changes.
            std::string b3_rest; acz::brief3d::Options b3opt;
            if (!acz::brief3d::extract_options(vs_rest, b3_rest, b3opt, why)) {
                std::snprintf(tt, sizeof tt, "xr-stereo;event=brief3d-option;ok=0;detail=%s;effect=ignored", why.c_str());
                emit(true, tt);
                b3_rest = vs_rest; b3opt = acz::brief3d::Options{};
            } else if (b3opt.any_token) {
                const char* pre = acz::brief3d::prerequisites(b3opt, g_vscreen_opt.on, g_vscreen_opt.early, settings.hud_layer || log->xr_hud_layer());
                if (!pre && !log->ee_camera_probe()) pre = "needs-ee_camera_probe(present-hook)";
                if (!pre) acz::brief3d::live::producer().configure(b3opt);   // before any XR attempt: the hooks start replaying map draws
                std::snprintf(tt, sizeof tt, "xr-stereo;event=brief3d-option;ok=1;%s;prerequisites=%s;live=%s;eye=%ux%u;rtv=%u;dsv=%u;slots=%u",
                    acz::brief3d::describe(b3opt).c_str(), pre ? pre : "ok", pre ? "off" : "producer-configured(table-on-the-screen-layer)",
                    acz::brief3d::live::kEyeW, acz::brief3d::live::kEyeH, acz::brief3d::live::kRtvFormat, acz::brief3d::live::kDsvFormat,
                    acz::brief3d::live::kSlots);
                emit(true, tt);
            }
            vs_rest = b3_rest;
            {   // UI3D (ui3d.h, docs/world-render/ui-3d.md 3.1/4/5): ui3d:/u3n/u3f/u3t/u3c removed here; a value without them is unchanged. With every
                // prerequisite the producer's UI3D kind is configured (the d3d12 hooks replay the ps2_v menu draws per eye) and the XR attempt wraps
                // the vscreen hook with the table consumer (make_brief3d_hook); otherwise nothing changes.
                std::string u3_rest; acz::ui3d::Options u3opt;
                if (!acz::ui3d::extract_options(vs_rest, u3_rest, u3opt, why)) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=ui3d-option;ok=0;detail=%s;effect=ignored", why.c_str());
                    emit(true, tt);
                    u3_rest = vs_rest;
                } else if (u3opt.any_token) {
                    const char* pre = acz::ui3d::prerequisites(u3opt, g_vscreen_opt.on, g_vscreen_opt.early, settings.hud_layer || log->xr_hud_layer());
                    if (!pre && !log->ee_camera_probe()) pre = "needs-ee_camera_probe(present-hook)";
                    if (!pre) acz::brief3d::live::producer().configure_ui3d(u3opt);   // before any XR attempt
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=ui3d-option;ok=1;%s;prerequisites=%s", acz::ui3d::describe(u3opt).c_str(), pre ? pre : "ok");
                    emit(true, tt);
                }
                vs_rest = u3_rest;
            }
            {   // DEBRIEF-3D (dbr3d.h, docs/world-render/debrief-3d-map.md 7-8): dbr3d:/dbrr/dbrk/dbrw/dbrd/dbry/dbrl removed here; a value without them is
                // unchanged. Stage 2: with every prerequisite (ui3d:1 configured above, the present hook) the producer's debrief route is configured (the
                // d3d12 hooks replay the debrief's map scene on a wraparound and its result panel on the UI3D layers); otherwise nothing changes.
                std::string db_rest; acz::dbr3d::Options dbopt;
                if (!acz::dbr3d::extract_options(vs_rest, db_rest, dbopt, why)) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=dbr3d-option;ok=0;detail=%s;effect=ignored", why.c_str());
                    emit(true, tt);
                    db_rest = vs_rest;
                } else if (dbopt.any_token) {
                    const char* pre = acz::dbr3d::live_prerequisites(dbopt, g_vscreen_opt.on, g_vscreen_opt.early, settings.hud_layer || log->xr_hud_layer(),
                                                                     acz::brief3d::live::producer().ui3d_configured(), log->ee_camera_probe());
                    if (!pre) acz::brief3d::live::producer().configure_dbr3d(dbopt);   // before any XR attempt
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=dbr3d-option;ok=1;%s;prerequisites=%s;live=%s",
                                  acz::dbr3d::describe(dbopt).c_str(), pre ? pre : "ok", pre ? "off" : "producer-configured(wraparound+panel)");
                    emit(true, tt);
                }
                vs_rest = db_rest;
            }
            {   // VIEW-MODE (view_mode.h, docs/world-render/pov.md): pov:<0|1> removed here (also a bad value: logged, ignored); a value without it is unchanged.
                std::string pv_rest; acz::viewmode::Options pvo;
                const bool pv_ok = acz::viewmode::extract_options(vs_rest, pv_rest, pvo, why);
                vs_rest = pv_rest;
                settings.pov = pv_ok && pvo.on;
                settings.pov_stage2 = pv_ok && pvo.stage2;   // POV-RENDER stage 2 (pov:2): no-cockpit chase frames in stereo (other-povs.md section 10)
                settings.pov_stage3 = pv_ok && pvo.stage3;   // POV-RENDER stages 3 + 4 (pov:3): + HUD-only, cinematics, the drawn vignette (section 11)
                acz::viewmode::enabled_flag().store(settings.pov, std::memory_order_relaxed);
                if (pvo.any_token) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=pov-config;ok=%u;%s;detail=%s;needs=vsearly:1+stage-auto(flight-stage)+e6loc:auto|content;"
                        "classifier=composite-age+E6-geometry;keep-alive=hud-only,chase(frozen-cap-%llds);renders=%s",
                        pv_ok ? 1u : 0u, acz::viewmode::describe(pvo).c_str(), pv_ok ? "ok" : why.c_str(),
                        static_cast<long long>(acz::viewmode::kFrozenKeepMs / 1000),
                        settings.pov_stage3 ? "chase+hud-only+cinematic-in-stereo(no-cockpit-producer-frames;needs-gpu_copy_step5_world_objects;"
                                              "cinematic-anchor-rig;vignette-drawn;flat-fallback-1s)"
                        : settings.pov_stage2 ? "chase-in-stereo(no-cockpit-producer-frames;needs-gpu_copy_step5_world_objects;povrig-game-or-level)"
                                            : "unchanged(no-cockpit-views-have-no-producer-frame)");
                    emit(true, tt);
                }
            }
            {   // EYE-RES (eye_res.h, docs/world-render/eye-resolution.md): eyeres:<0|50..200> removed here; a value without it is unchanged. The eye swapchains
                // follow the runtime's recommended view size only when the projection layer carries the headset's FOV (gpu_copy_step5_hmd_fov); otherwise logged, off.
                std::string er_rest; acz::eyeres::Options ero;
                const bool er_ok = acz::eyeres::extract_options(vs_rest, er_rest, ero, why);
                vs_rest = er_rest;
                const char* er_pre = ero.pct && !log->gpu_copy_step5_hmd_fov() ? "needs-gpu_copy_step5_hmd_fov" : nullptr;
                settings.eye_res_pct = er_ok && !er_pre ? ero.pct : 0u;
                // EYE-RING (eye_ring.h): the producer reads this request once, at its ring creation. eyeres off: never published (the producer is untouched).
                if (settings.eye_res_pct) acz::eyering::request(true, ero.objects);
                if (ero.any_token || ero.any_obj_token) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=eye-res-config;ok=%u;%s;prerequisites=%s;effective-pct=%u;detail=%s%s", er_ok ? 1u : 0u,
                        acz::eyeres::describe(ero).c_str(), er_pre ? er_pre : "ok", settings.eye_res_pct, er_ok ? "ok" : why.c_str(),
                        acz::eyeres::describe_objects(ero).c_str());
                    emit(true, tt);
                }
            }
            {   // SEATED RECENTER (recenter.h, docs/world-render/recenter.md): rc:/rckey:/rcauto:/rcyaw:/rcfocus: removed here; a bad value is logged and the defaults apply.
                std::string rc_rest; acz::recenter::Options rco;
                const bool rc_ok = acz::recenter::extract_options(vs_rest, rc_rest, rco, why);
                vs_rest = rc_rest;
                g_recenter_opt = rco;
                if (rco.any_token || !rc_ok) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=recenter-config;ok=%u;%s;detail=%s", rc_ok ? 1u : 0u, acz::recenter::describe(rco).c_str(), rc_ok ? "ok" : why.c_str());
                    emit(true, tt);
                }
            }
            {   // NULL-HEAD (TEST ONLY; null_head.h, docs/world-render/null-head-look.md): nh:/nhs: removed here; a bad value is logged and the feature stays off.
                std::string nh_rest; acz::nullhead::Options nho;
                const bool nh_ok = acz::nullhead::extract_options(vs_rest, nh_rest, nho, why);
                vs_rest = nh_rest;
                g_nullhead_opt = nh_ok ? nho : acz::nullhead::Options{};
                if (nho.any_token || !nh_ok) emit(true, acz::nullhead::format_config(nho, nh_ok, nh_ok ? std::string() : why).c_str());
            }
            {   // POV-RENDER stage 1 (pov_render.h, docs/world-render/other-povs.md): povc / povrig / povv / povtc removed here (a bad value: logged, the
                // whole set ignored); a value without them is unchanged. Needs pov:1 (the tracker). Logs only: no render change.
                std::string pr_rest; acz::povr::Options pro;
                const bool pr_ok = acz::povr::extract_options(vs_rest, pr_rest, pro, why);
                vs_rest = pr_rest;
                const char* pr_pre = pr_ok ? acz::povr::prerequisites(pro, settings.pov) : nullptr;
                settings.pov_render = pr_ok && !pr_pre ? pro : acz::povr::Options{};
                // POV-RENDER pov:3: the consumer rig drives the drawn vignette / blink and the census; without povrig it runs as the level rig.
                if (settings.pov_stage3 && settings.pov_render.rig == acz::povr::RigMode::off) settings.pov_render.rig = acz::povr::RigMode::level;
                if (pro.any_token) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=pov-render-config;ok=%u;%s;detail=%s;prerequisites=%s;render=unchanged(stage-1-dry-run);"
                        "census=%s", pr_ok ? 1u : 0u, acz::povr::describe(settings.pov_render).c_str(), pr_ok ? "ok" : why.c_str(), pr_pre ? pr_pre : "ok",
                        settings.pov_render.census ? "pov-census-lines+one-ui3d-acm-census-per-new-view" : "off");
                    emit(true, tt);
                }
            }
            const bool ls_ok = acz::look_stick::extract_option(vs_rest, ls_rest, ls_mode, why);
            g_look_stick_block.store(ls_ok && ls_mode == acz::look_stick::Mode::block, std::memory_order_relaxed);
            if (ls_ok) {
                std::snprintf(tt, sizeof tt, "xr-stereo;event=look-stick-config;lookstick=%s;detail=%s", acz::look_stick::mode_name(ls_mode),
                    ls_mode == acz::look_stick::Mode::block ? "right-stick-centred-for-the-game-while-in-flight-composites(needs-skip-intro-XINPUT1_4.dll-with-the-gate)"
                                                            : "game-receives-the-real-right-stick(previous-behaviour)");
                emit(true, tt);
            }
            std::string feed_rest;
            acz::e6loc::Mode feed_mode = acz::e6loc::Mode::automatic;
            const bool feed_ok = ls_ok && acz::e6loc::extract_mode(ls_rest, feed_rest, feed_mode, why);
            // UNCAP / EXTRAP (frame_pacing.h, docs/world-render/uncap.md): uncap:1 / extrap:1 / extrapms / extrapatt removed here; a value without them is unchanged.
            std::string pacing_rest;
            acz::pacing::Options pacing_opt;
            const bool pacing_ok = feed_ok && acz::pacing::extract_options(feed_rest, pacing_rest, pacing_opt, why);
            if (pacing_ok && pacing_opt.any()) {
                // uncap needs the existing Present hook, which is armed only with ee_camera_probe (the terrain camera feed); it takes effect while an
                // XR session runs (set_xr_active below), never before the first XR frame or after the session ends.
                const bool hook = log->ee_camera_probe();
                acz::pacing::set_uncap_requested(pacing_opt.uncap && hook);
                std::snprintf(tt, sizeof tt, "xr-stereo;event=pacing-config;%s;present-hook=%s;uncap-armed=%u;applies=while-xr-session-runs;"
                    "game-limiter=internal-qpc-sleep(inference,uncap.md);sim=fixed-step-per-frame(measured)", acz::pacing::describe(pacing_opt).c_str(),
                    hook ? "ee_camera_probe" : "absent(ee_camera_probe-off:uncap-ignored)", (pacing_opt.uncap && hook) ? 1u : 0u);
                emit(true, tt);
            }
            // TACTICAL: tac:1 (+ tacr/tacradar/tacboxes|tacmark/tacgun/tacsup/tacself/tacclu) removed here; a value without them is unchanged.
            std::string tac_rest;
            acz::tac::Options topt;
            const bool tac_ok = pacing_ok && acz::tac::extract_options(pacing_rest, tac_rest, topt, why);
            const bool props_ok = tac_ok && acz::props::extract_options(tac_rest, stage_value, popt, why);
            bool stage_ok = props_ok && acz::terrain::parse_stage_option(stage_value, id, rt->conv, why);
            std::string auto_sha;   // STAGES: the fingerprint table's payload hash of the detected stage (informational)
            if (stage_ok) {   // WORLD-SCALE (world_scale.h, docs/world-render/world-scale.md): one world head scale for terrain AND the producer's world objects/smoke
                acz::world_scale::published_world_units_per_metre().store(rt->conv.head_units_per_metre, std::memory_order_relaxed);
                std::snprintf(tt, sizeof tt, "xr-stereo;event=world-scale;head-units-per-metre=%.3f;source=%s;wscale=%.3f;evidenced-u-per-m=%.1f;layers=terrain,props,trees,clouds,"
                    "markers,world-objects,smoke;cockpit=model-upm/cscale(unchanged);replayed-cockpit-units-per-metre=%.1f(unchanged)", double(rt->conv.head_units_per_metre),
                    rt->conv.upm_set ? "upm-option" : "evidenced", double(rt->conv.world_scale), double(acz::world_scale::kWorldUnitsPerMetre),
                    double(log->gpu_copy_step5_units_per_metre()));
                emit(true, tt);
            }
            if (!stage_ok) {
                std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-disabled;reason=bad-stage-option;detail=%s;fallback=game-world-image", why.c_str());
                emit(true, tt);
            } else if (id == "auto" && g_early) {
                // VSEARLY (xr_flight_stage.h): XR starts in the menus, where no stage is loaded: nothing is detected now; every flight start detects and
                // loads its own stage (+ takeoff) on one worker thread; no terrain is drawn between flights. This runtime is only the options template.
                if (feed_mode == acz::e6loc::Mode::probe) rt->conv.cam_admit = false;
                rt->tac = topt; rt->pacing = pacing_opt;
                if (!flight_stage_start(FlightStageTemplate{rt, popt}, &detect_auto_stage, &emit, &g_stop))
                    emit(true, "xr-stereo;event=terrain-disabled;reason=flight-stage-start-failed;fallback=game-world-image");
                stage_ok = false;   // nothing loaded here (settings.terrain stays null; the consumer receives each flight's stage)
            } else if (id == "auto") {
                // STAGES (docs/world-render/stages-and-detection.md section 3): reserved id 'auto' = detect the stage from the game's loaded stage files
                // by a READ-ONLY scan of this process's memory for the offline fingerprints (%LOCALAPPDATA%\acz-vr\stage\fingerprints.json).
                // No match / a tie / no table -> no terrain (the game's own world image), with the reason logged.
                stage_ok = detect_auto_stage(id, auto_sha);
            }
            if (stage_ok) {
                const std::string path = acz::terrain::stage_path(id);
                const acz::terrain::LoadStatus st = acz::terrain::load_stage_file(path, rt->stage);
                if (st != acz::terrain::LoadStatus::ok) {
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-disabled;reason=load-failed;status=%s;stage=%s;path=%s;fallback=game-world-image;hint=run-stage_baker.py",
                        acz::terrain::status_name(st), id.c_str(), path.c_str());
                    emit(true, tt);
                } else {
                    // WORLD-SCALE: the world head scale was set by parse_stage_option (evidenced 5.0 u/m * wscale, or upm:) and published to the producer below
                    // (one scale for terrain and replayed objects); it no longer inherits gpu_copy_step5_units_per_metre (the replayed cockpit's scale).
                    acz::terrain::apply_sea_convention(rt->stage, rt->conv);
                    acz::terrain::build_atlas(rt->stage, rt->atlas, rt->conv.clut_raw);
                    if (rt->conv.detail) {   // DTEX (dtex:1): <stage>.zvdt next to the .zvrs; any failure = no detail (every image as before), logged once
                        std::string dwhy;
                        acz::terrain::attach_detail(id, rt->conv, rt->atlas, dwhy);
                        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-detail;stage=%s;role=mission;state=%s;repeat-units=%.0f;gain=%.2f;fade=%.0f-%.0f", id.c_str(), dwhy.c_str(),
                            double(acz::terrain::kDtRepeat), double(acz::terrain::kDtGain), double(acz::terrain::kDtFadeNear), double(acz::terrain::kDtFadeFar));
                        emit(true, tt);
                    }
                    rt->generation = 1;
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-loaded;stage=%s;tiles=%u;objects=%u;clut=%s;payload-sha256=%s;path=%s", rt->stage.id, rt->stage.ntiles, rt->stage.nobj,
                        rt->conv.clut_raw ? "raw" : "csm1", acz::terrain::hex(rt->stage.payload_sha, 32).c_str(), path.c_str());
                    emit(true, tt);
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-camera-feed;e6loc=%s;detail=%s;log=ee-camera.log(e6-locate)", acz::e6loc::mode_name(feed_mode),
                        feed_mode == acz::e6loc::Mode::probe ? "probe-observation-base(previous-feed)" :
                        feed_mode == acz::e6loc::Mode::content ? "content-validated-block-only" : "content-validated-block,else-checked-probe-base");
                    emit(true, tt);
                    // CAM-EPOCH (e6-anchor-free.md section 10): e6loc:probe keeps the previous feed AND the previous camera selection exactly; a newly installed
                    // stage starts a new stage epoch (samples published before it are refused by the admission).
                    if (feed_mode == acz::e6loc::Mode::probe) rt->conv.cam_admit = false;
                    const uint32_t stage_epoch = acz::terrain::cam_epochs().bump_stage();
                    std::snprintf(tt, sizeof tt, "xr-stereo;event=camera-epoch;change=stage;reason=terrain-stage-installed;stage=%s;stage-epoch=%u;lock-epoch=%u;admission=%s;"
                        "age-limit-ms=%.0f;fallback-window-ms=%.0f", rt->stage.id, stage_epoch, acz::terrain::cam_epochs().lock(),
                        rt->conv.cam_admit ? "on" : feed_mode == acz::e6loc::Mode::probe ? "off(e6loc:probe)" : "off(camadmit:0)",
                        double(rt->conv.cam_age_ms), double(rt->conv.cam_window_ms));
                    emit(true, tt);
                    if (!auto_sha.empty() && auto_sha != acz::terrain::hex(rt->stage.payload_sha, 32)) {   // STAGES: installed bake differs from the fingerprinted one
                        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-auto-warning;reason=zvrs-differs-from-fingerprint-table;stage=%s;table-sha256=%s", rt->stage.id, auto_sha.c_str());
                        emit(true, tt);
                    }
                    if (rt->conv.env_bits) {   // WORLD-2 environment sub-options (sky/light/cloud/water); a stage without the v3 section keeps the legacy shading
                        acz::terrain::build_env_texture(rt->stage, rt->conv, rt->env);
                        const uint32_t b = rt->conv.env_bits;
                        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain-env;stage=%s;requested=%s%s%s%s;env-section=%s;active=%s", rt->stage.id,
                            (b & acz::terrain::kEnvSky) ? "sky+" : "", (b & acz::terrain::kEnvLight) ? "light+" : "", (b & acz::terrain::kEnvCloud) ? "cloud+" : "",
                            (b & acz::terrain::kEnvWater) ? "water" : "", rt->stage.has_env() ? "present" : "absent(rebake-v3)", rt->env.empty() ? "no(legacy-shading)" : "yes");
                        emit(true, tt);
                    }
                    if (popt.any()) {   // WORLD-3: the companion <stage>.zvro; any failure leaves the terrain running without props/trees
                        auto pr = std::make_shared<acz::props::Runtime>();
                        const std::string ppath = acz::props::props_path(id);
                        const ULONGLONG load_t0 = GetTickCount64();   // PERF: the whole CPU initialisation (read, parse + sha256, snap, grid) is timed and logged
                        const acz::props::LoadStatus ps = acz::props::load_props_file(ppath, pr->set);
                        const bool baked = ps == acz::props::LoadStatus::ok && pr->set.baked_snap;
                        if (ps != acz::props::LoadStatus::ok) {
                            std::snprintf(tt, sizeof tt, "xr-stereo;event=props-disabled;reason=load-failed;status=%s;stage=%s;path=%s;fallback=terrain-only;hint=run-objects_baker.py",
                                acz::props::status_name(ps), id.c_str(), ppath.c_str());
                            emit(true, tt);
                        } else if (std::strcmp(pr->set.id, rt->stage.id) != 0) {
                            std::snprintf(tt, sizeof tt, "xr-stereo;event=props-disabled;reason=stage-mismatch;zvro=%s;zvrs=%s;fallback=terrain-only", pr->set.id, rt->stage.id);
                            emit(true, tt);
                        } else if (!acz::props::snap_to_terrain(pr->set, rt->stage)) {   // PERF: a v2 bake snapped against ANOTHER .zvrs bake of this stage
                            std::snprintf(tt, sizeof tt, "xr-stereo;event=props-disabled;reason=snapped-against-another-stage-bake;stage=%s;zvro-stage-sha-prefix=%s;zvrs-sha=%s;"
                                "fallback=terrain-only;hint=rerun-objects_baker.py---stage", pr->set.id, acz::terrain::hex(pr->set.stage_sha, acz::props::kZvroStageShaBytes).c_str(),
                                acz::terrain::hex(rt->stage.payload_sha, 32).c_str());
                            emit(true, tt);
                        } else {
                            pr->opt = popt;
                            acz::props::build_grid(pr->set, pr->opt, pr->grid);
                            pr->generation = 1;
                            std::snprintf(tt, sizeof tt, "xr-stereo;event=props-loaded;stage=%s;instances=%zu;models=%zu;textures=%u;payload-sha256=%s;path=%s;snap=%s;load-ms=%llu", pr->set.id,
                                pr->set.inst.size(), pr->set.models.size(), pr->set.ntex, acz::terrain::hex(pr->set.payload_sha, 32).c_str(), ppath.c_str(),
                                baked ? "baked" : "runtime", static_cast<unsigned long long>(GetTickCount64() - load_t0));
                            emit(true, tt);
                            rt->props = pr;
                            if (pr->opt.props && pr->opt.dedupe) {   // WORLD-4: the producer stops replaying the game's copies of these buildings
                                const std::vector<float> pl = acz::props::prop_placements(pr->set);
                                acz::props::publish_dedupe(pl, pr->opt.dedupe_tol);
                                std::snprintf(tt, sizeof tt, "xr-stereo;event=props-dedupe;stage=%s;placements=%zu;tol=%.2f;refusal=world-dedupe-prop(category-target,not-a-loss)", pr->set.id,
                                    pl.size() / 3, double(pr->opt.dedupe_tol));
                                emit(true, tt);
                            }
                        }
                    }
                    rt->tac = topt;
                    rt->pacing = pacing_opt;   // EXTRAP (extrap:1): the consumer's camera / marker extrapolation (default off)
                    if (pacing_opt.interp) {   // OBJECT-INTERP (interp:1): the producer plans each frame from now on (needs the terrain: armed only here)
                        acz::interp::arm(true, pacing_opt.interp_max_frames, pacing_opt.extrap_max_ms, pacing_opt.extrap_attitude);
                        std::snprintf(tt, sizeof tt, "xr-stereo;event=interp-armed;%s;needs=gpu_copy_step5_world_objects;camera=producer-delta(replaces-extrap:1-for-the-terrain);"
                            "moves=replayed-objects,terrain,props,clouds,tactical-markers;game-rate=hud,effects,game-marker-sprites,replayed-cockpit",
                            acz::pacing::describe(pacing_opt).c_str());
                        emit(true, tt);
                    }
                    if (topt.on || topt.mk360) {   // TACTICAL / MARKERS-360 (mk360 needs the facts, not tac:1): the producer starts collecting the frame facts (and suppressing matched game marker boxes) from now on
                        acz::tac::set_producer_flags(acz::tac::producer_flags_for(topt));   // D-355: suppression only with tacboxes:1
                        std::snprintf(tt, sizeof tt, "xr-stereo;event=tac-armed;%s;producer-flags=%u", acz::tac::describe(topt).c_str(), acz::tac::producer_flags());
                        emit(true, tt);
                    }
                    rt->takeoff = load_takeoff_runtime(*rt, popt);   // TAKEOFF (option takeoff:, default k00a; null when off/same/missing)
                    settings.terrain = rt;
                    {   // EFFECTS-OUTSIDE (A): with our own clouds drawn, the game's cloud billboards are not replayed into the effects image (gamecloud:0|1 overrides).
                        const bool ours = (rt->conv.env_bits & acz::terrain::kEnvCloud) != 0 && !rt->env.empty();
                        const bool drop = acz::terrain::drop_game_clouds(rt->conv, ours);
                        if (drop || rt->conv.game_cloud >= 0) {
                            std::snprintf(tt, sizeof tt, "xr-stereo;event=game-clouds;replay=%s;reason=%s;our-clouds=%s;class=effects-ps-4caf6e2d+depth-greater-equal", drop ? "off" : "on",
                                rt->conv.game_cloud >= 0 ? "gamecloud-option" : "default(cloud:1-active)", ours ? "yes" : "no");
                            emit(true, tt);
                        }
                        // EFFECTS-OUTSIDE (C/D/E) fxmem:1..5 + the game-cloud switch: effects_stage_flags.h, shared with the VSEARLY per-flight install
                        // (xr_flight_stage.cpp) so both stage paths set the same producer switches. Written on every terrain load (fxmem:0 clears all).
                        {   // WORLD-CLOUDS (world_clouds.h): the config line and the producer's puff drop edge / census switch
                            const bool envp = !rt->env.empty();
                            const acz::wcloud::Params wp = envp ? acz::wcloud::params_from(&rt->env[size_t(acz::wcloud::kPWc) * 4], &rt->env[size_t(acz::wcloud::kPWc2) * 4]) : acz::wcloud::Params{};
                            const float wcd = acz::wcloud::producer_drop_edge(rt->conv.wcloud, envp && wp.on, rt->conv.fx_memory, rt->conv.wc_drop_edge);
                            if (rt->conv.wcloud) { acz::wcloud::config_line(tt, sizeof tt, true, envp, wp, wcd, rt->conv.wc_diag,
                                acz::wcloud::band_source(rt->stage.env_id[0] ? rt->stage.env_id : rt->stage.id, rt->conv.wc_alt)); emit(true, tt); }
                            acz::gpu_copy::step5::apply_stage_effects(acz::gpu_copy::step5::stage_effects(rt->conv.fx_memory, drop, rt->conv.fx_sig, rt->conv.hud_rs_skip, wcd, rt->conv.wc_diag,
                                rt->conv.fx_trail_s, rt->conv.fx_trail_max_edge, rt->conv.fx_trail_cls, rt->conv.fx_trail_near,
                                acz::gpu_copy::step5::PuffCloud{rt->conv.fx_cloud_s, rt->conv.fx_cloud_far, rt->conv.fx_trail_xkey, rt->conv.fx_trail_clip, rt->conv.fx_atlas}, rt->conv.hud_late, rt->conv.fx_add));   // (M4) + (M4c); HUD-LATE hudlate; (P) fxadd
                        }
                        if (const char* fl = acz::gpu_copy::step5::fx_memory_line(rt->conv.fx_memory)) emit(true, fl);
                        if (rt->conv.hud_late) emit(true, acz::gpu_copy::step5::hud_late_option_line());   // HUD-LATE (hud_late.h)
                        if (acz::gpu_copy::step5::fx_additive_line(tt, sizeof tt, rt->conv.fx_memory, rt->conv.fx_add)) emit(true, tt);   // EFFECTS-OUTSIDE (P) fxadd
                        if (acz::gpu_copy::step5::fx_puff_trail_line(tt, sizeof tt, rt->conv.fx_memory, rt->conv.fx_trail_s, rt->conv.fx_trail_max_edge, rt->conv.fx_trail_cls, rt->conv.fx_trail_near,
                            acz::gpu_copy::step5::PuffCloud{rt->conv.fx_cloud_s, rt->conv.fx_cloud_far, rt->conv.fx_trail_xkey, rt->conv.fx_trail_clip, rt->conv.fx_atlas})) emit(true, tt);   // EFFECTS-OUTSIDE (M) + (M4)
                    }
                }
            }
        }
        // COCKPIT-2: key xr_cockpit_model (default OFF, latched only with xr_world_in_projection). Any parse/load/build failure logs once and leaves the setting null:
        // the replayed cockpit exactly as with the key off.
        std::shared_ptr<CockpitRuntime> cockpit_rt;
        if (auto* log = diagnostics::sink(); log && acz::aircraft::is_auto(log->xr_cockpit_model())) {
            // AIRCRAFT-AUTO (docs/world-render/aircraft-auto.md): no model is loaded here; the consumer detects the aircraft from the locked E6 block's eye
            // offset and loads <type><skin>_vrcockpit lazily. Needs the anchor-free locator (the terrain key with e6loc:auto|content), else refused.
            char ct[480];
            auto ca = std::make_shared<CockpitAuto>();
            std::string why;
            bool feed_ok = true;
            const acz::e6loc::Mode fm = acz::e6loc::mode_in(log->xr_terrain_stage(), feed_ok);
            if (!acz::aircraft::parse_auto(log->xr_cockpit_model(), ca->cfg, why)) {
                std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-disabled;reason=bad-option;detail=%s;mode=auto;fallback=replayed-cockpit", why.c_str());
                emit(true, ct);
            } else if (!log->xr_terrain_render() || !feed_ok || fm == acz::e6loc::Mode::probe) {
                std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-disabled;reason=auto-needs-e6-locator;detail=xr_terrain_render-on-and-xr_terrain_stage-e6loc:auto|content;"
                    "terrain-key=%u;e6loc=%s;fallback=replayed-cockpit", log->xr_terrain_render() ? 1u : 0u, feed_ok ? acz::e6loc::mode_name(fm) : "bad");
                emit(true, ct);
            } else {
                ca->det.prefer = ca->cfg.prefer;
                ca->stale_ms = ca->cfg.stale_ms;   // AIRCRAFT-AUTO-STALE
                ca->grace_ms = ca->cfg.grace_ms;   // AIRCRAFT-AUTO-GRACE
                // AIRCRAFT-AUTO-FAST: the remembered last aircraft (validated; never a path), else default:<model>; the Output constructor loads its
                // model before the session so the first XR frame already draws the model cockpit; the detector then confirms or switches.
                ca->state_path = acz::aircraft::remembered_path();
                int remembered = -1; std::string rwhy;
                ca->assumed = acz::aircraft::resolve_assumed(ca->cfg, ca->state_path, remembered, ca->assumed_source, rwhy);
                ca->remembered = remembered;
                const bool have = remembered >= 0;
                std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-auto-remembered;file=%s;result=%s;reason=%s;default=%s;assumed=%s;source=%s",
                    ca->state_path.empty() ? "none(no-LOCALAPPDATA)" : ca->state_path.c_str(), have ? "ok" : "none", have ? "-" : rwhy.c_str(),
                    ca->cfg.default_type >= 0 ? acz::aircraft::kAircraft[ca->cfg.default_type].model_id : "none",
                    ca->assumed >= 0 ? acz::aircraft::kAircraft[ca->assumed].model_id : "none", ca->assumed_source.empty() ? "none(detect-only)" : ca->assumed_source.c_str());
                emit(true, ct);
                settings.cockpit_auto = ca;
                std::string dir = acz::cockpit::model_path("x");   // "<dir>\x.zvrm" -> "<dir>\"
                if (dir.size() >= 6) dir.resize(dir.size() - 6);
                std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-auto-config;value=%s;models=%s<model_id>.zvrm;source=E6-eye-offset(locked-block);requires=ee_camera_probe;"
                    "no-headset-claim", log->xr_cockpit_model(), dir.c_str());
                emit(true, ct);
            }
        } else if (auto* log = diagnostics::sink(); log && log->xr_cockpit_model()[0]) {
            auto rt = std::make_shared<CockpitRuntime>();
            std::string id, why;
            char ct[480];
            if (!acz::cockpit::parse_options(log->xr_cockpit_model(), id, rt->opt, why)) {
                std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-disabled;reason=bad-option;detail=%s;fallback=replayed-cockpit", why.c_str());
                emit(true, ct);
            } else {
                acz::cockpit::apply_clip_table(rt->opt, id);   // COCKPIT-CLIP: the type's clip volume (unless clipbox: / clip:0)
                const std::string path = acz::cockpit::model_path(id);
                acz::cockpit::ModelData model;
                const acz::cockpit::LoadStatus st = acz::cockpit::load_model_file(path, model);
                if (st != acz::cockpit::LoadStatus::ok) {
                    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-disabled;reason=load-failed;status=%s;model=%s;path=%s;fallback=replayed-cockpit;hint=tools/live/install-assets.ps1",
                        acz::cockpit::status_name(st), id.c_str(), path.c_str());
                    emit(true, ct);
                } else if (!acz::cockpit::build_mesh(model, rt->opt, rt->mesh, why)) {
                    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-disabled;reason=mesh;detail=%s;model=%s;fallback=replayed-cockpit", why.c_str(), id.c_str());
                    emit(true, ct);
                } else {
                    rt->id = id; rt->generation = 1;
                    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-loaded;model=%s;parts=%zu;triangles=%u;payload-sha256=%s;path=%s;clut=%s", id.c_str(), model.parts.size(),
                        rt->mesh.triangles, acz::terrain::hex(model.payload_sha, 32).c_str(), path.c_str(),
                        model.legacy_clut_fixed ? "legacy-bgra-bake(r-b-swapped-at-load;rebake-recommended)" : "rgba");
                    emit(true, ct);
                    if (rt->mesh.hud.valid) {
                        const auto& hg = rt->mesh.hud;
                        std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-hud-glass;part=%u;centre=%.3f,%.3f,%.3f;distance-u=%.3f;distance-m=%.3f;deg-l-r-u-d=%.1f,%.1f,%.1f,%.1f;hudglass-option=%u",
                            hg.part, double(hg.centre[0]), double(hg.centre[1]), double(hg.centre[2]), double(hg.distance), double(hg.distance / acz::cockpit::view_upm(rt->opt)),
                            std::atan(double(hg.tan_l)) * 57.29578, std::atan(double(hg.tan_r)) * 57.29578, std::atan(double(hg.tan_u)) * 57.29578,
                            std::atan(double(hg.tan_d)) * 57.29578, rt->opt.hud_glass ? 1u : 0u);
                        emit(true, ct);
                    }
                    settings.cockpit = rt;
                    cockpit_rt = rt;
                }
            }
        }
        // J-occlusion-fix: key xr_world_window_occlude (default OFF; latched only with xr_world_in_projection and the latched xr_world_objects_depth_fix).
        if (auto* log = diagnostics::sink(); log && log->xr_world_window_occlude()) settings.world_window_occlude = true;
        // WORLD-1: the producer's object depth fix (needed by the terrain's object occlusion; latched with the producer key).
        if (auto* log = diagnostics::sink(); log && log->xr_world_objects_depth_fix()) settings.objects_depth_fix = true;
        // J-effects-layer: key xr_effects_layer (default OFF, latched only with xr_world_in_projection); needs the producer key gpu_copy_step5_effects.
        if (auto* log = diagnostics::sink(); log && log->xr_effects_layer()) settings.effects_layer = true;
        // Timing-anchor diagnostics (docs/review/timing-anchor-fixtures.md): no key of its own; the capped select-corr record is latched with xr_look_drive_keys (default OFF).
        if (auto* log = diagnostics::sink(); log && log->xr_look_drive_keys()) settings.select_corr_log = true;
        // POV fixes (docs/review/pov-fixes-impl.md): lean values (latched with the continuous producer + xr_stereo_output) and xr_hud_follow (latched with the HUD layer).
        if (auto* log = diagnostics::sink(); log) {
            settings.lean = acz::pov::make_lean(double(log->pov_neck_lever_m()), double(log->pov_translation_scale()));
            settings.hud_follow = log->xr_hud_follow();
            if (settings.lean.active() || settings.hud_follow) {
                char povText[200];
                std::snprintf(povText, sizeof povText, "xr-stereo;event=pov-config;neck-lever-m=%.3f;translation-scale=%.2f;hud-follow=%u",
                    settings.lean.lever, settings.lean.scale, settings.hud_follow);
                emit(true, povText);
            }
        }
        // J-test8-fixes: keys xr_debug_view (0 = off) and xr_png_once (default OFF), latched only with xr_stereo_output. The PNG sink copies the pixels to a
        // detached thread that encodes and writes <logs>\png\<name> (CREATE_NEW; the owner thread never waits on disk).
        if (auto* log = diagnostics::sink(); log) {
            settings.debug_view = log->xr_debug_view();
            settings.png_once = log->xr_png_once();
            {   // also the SCREEN-DUMP sink (png-screen.now; xr_screen_dump.h): the queue is created at the first image only
                // PERF (step65: commit 94-99.8 % right after the three 13 MB files): the sink TAKES the pixels (no copy) into one sequential writer
                // (png_dump_queue.h) that streams each file through png_stream (one row + 64 KiB) and frees each image after its file; same file bytes.
                settings.png_sink = [](const std::string& name, std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) -> bool {
                    try {
                        static png_dump::Queue* queue = new png_dump::Queue([](const png_dump::Job& j) -> bool {   // process lifetime (never destroyed)
                            const std::wstring dir = diagnostics::DefaultPaths().paths().logs.wstring() + L"\\png";
                            std::wstring path = dir + L"\\";
                            for (char c : j.name) path.push_back(wchar_t(static_cast<unsigned char>(c)));
                            CreateDirectoryW(dir.c_str(), nullptr);
                            HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (hf == INVALID_HANDLE_VALUE) return false;
                            const bool ok = gpu_copy::step3::png_stream(j.rgba.data(), j.w, j.h, uint64_t(j.w) * 4, false, [hf](const uint8_t* p, size_t n) {
                                while (n) {
                                    DWORD wrote = 0;
                                    if (!WriteFile(hf, p, DWORD(n), &wrote, nullptr) || !wrote) return false;
                                    p += wrote; n -= wrote;
                                }
                                return true;
                            });
                            CloseHandle(hf);
                            if (!ok) DeleteFileW(path.c_str());   // never leave a truncated PNG behind
                            return ok;
                        }, [](const png_dump::Job& j, bool ok) {   // the FINAL outcome of this one attempt (push() only means queued)
                            const std::string t = std::string("xr-stereo;event=png-file;outcome=") + (ok ? "written" : "failed") + ";file=" + j.name +
                                ";size=" + std::to_string(j.w) + "x" + std::to_string(j.h);
                            emit(true, t.c_str());
                        });
                        return queue->push(name, rgba, w, h);
                    } catch (...) { return false; }
                };
            }
        }
        // J-hud-consumer: key xr_hud_layer (default OFF, latched only with xr_stereo_output) and hud_mode.
        if (auto* log = diagnostics::sink(); log && log->xr_hud_layer()) {
            settings.hud_layer = true;
            settings.hud_mode = log->hud_mode() == 1 ? HudMode::head_locked : HudMode::world_quad;
            // J-hud-placement: keys xr_hud_quad_distance_m / xr_hud_scale / xr_hud_head_distance_m /
            // xr_hud_head_width_m (default unset / 1 / 2.5 / 1.6); the consumer clamps them again.
            const bool placed = log->xr_hud_placement_configured();
            if (placed) {
                settings.hud_quad_distance_m = clamp_hud_quad_distance(log->xr_hud_quad_distance_m());
                settings.hud_scale = clamp_hud_scale(log->xr_hud_scale());
                settings.hud_head_distance_m = clamp_hud_head_distance(log->xr_hud_head_distance_m());
                settings.hud_head_width_m = clamp_hud_head_width(log->xr_hud_head_width_m());
                settings.hud_placement_log = true;
            }
            const char* placement = settings.hud_mode == HudMode::head_locked
                ? (placed ? "VIEW-hud-head-keys-fallback-no-world-alignment" : "VIEW-2.5m-1.6m-wide-fallback-no-world-alignment")
                : (placed ? "world-quad-direction-and-angular-size-hud-keys" : "world-quad-pose-and-size");
            int n = std::snprintf(text, sizeof text, "xr-stereo;event=hud-config;key=xr_hud_layer;hud_mode=%s;requires=producer-gpu_copy_step5_hud;"
                "placement=%s", hud_mode_name(settings.hud_mode), placement);
            if (settings.comfort_log && n > 0 && size_t(n) < sizeof text) { // J-feather tokens only when a J-feather key is configured
                const int m = std::snprintf(text + n, sizeof text - size_t(n), ";edge-feather=%.3f;world-quad-distance-m=%.1f",
                    double(settings.edge_feather), double(settings.quad_distance_m));
                n = m > 0 ? n + m : n;
            }
            if (placed && n > 0 && size_t(n) < sizeof text) // J-hud-placement tokens only when a placement key is configured
                std::snprintf(text + n, sizeof text - size_t(n), "%s", hud_placement_tokens(settings).c_str());
            emit(true, text);
            // COCKPIT-2 option hudglass:1: the world-quad HUD (already fixed in the cockpit frame: the seat anchor = the model's pvb eye, heading yaw0) is placed at the
            // distance of the model's HUD glass instead of the collimated/configured distance (same direction and angular size).
            if (cockpit_rt && cockpit_rt->opt.hud_glass && cockpit_rt->mesh.hud.valid && settings.hud_mode == HudMode::world_quad) {
                settings.hud_quad_distance_m = clamp_hud_quad_distance(cockpit_rt->mesh.hud.distance / acz::cockpit::view_upm(cockpit_rt->opt));   // COCKPIT-SCALE: scaled glass
                settings.hud_placement_log = true;
                std::snprintf(text, sizeof text, "xr-stereo;event=cockpit-hud-anchor;hud-quad-distance-m=%.3f;source=model-hud-glass;frame=cockpit(seat-anchor,yaw0)",
                    double(settings.hud_quad_distance_m));
                emit(true, text);
            }
            // J-hud-markers: key xr_hud_markers_layer (default OFF, latched only with xr_stereo_output + xr_hud_layer);
            // the consumer ignores it in head-locked HUD mode (no world alignment there).
            if (log->xr_hud_markers_layer()) {
                settings.hud_markers_layer = true;
                std::snprintf(text, sizeof text, "xr-stereo;event=markers-config;key=xr_hud_markers_layer;requires=producer-gpu_copy_step5_hud_markers;"
                    "placement=%s;marker-quad-distance-m=%.1f;marker-scale=1;order=quad-then-markers-then-projection-then-hud",
                    settings.hud_mode == HudMode::head_locked ? "ignored-head-locked-hud" : "world-quad-pose-and-size",
                    double(settings.comfort_log ? settings.quad_distance_m : kQuadDistanceDefault));
                emit(true, text);
            }
        }
        // XR-START: attempts run through xr_start::run_attempts: attempt 1, the legacy immediate cold-start retry, then (default ON,
        // ACZ_XR_RETRY=0 to disable) persistent retries with backoff for transient start failures. Every attempt fully retires its
        // handles first (a transient diagnosis requires clean=1 and nothing held); permanent failures stop as before.
        xr_start::RetryPolicy policy(g_persistent_retry.load(std::memory_order_acquire));
        policy.set_early(g_early);   // VSEARLY: retry after a stalled/lost running session; schedule restarts after a hooked one
        bool last_budget = false;
        xr_start::LoopHooks hooks;
        hooks.now_ms = [] { return static_cast<int64_t>(GetTickCount64()); };
        hooks.wait = [](int64_t ms) {
            const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
            while (GetTickCount64() < until) {
                if (g_stop.load(std::memory_order_acquire)) return false;
                Sleep(100);
            }
            return !g_stop.load(std::memory_order_acquire);
        };
        hooks.attempt = [&](unsigned attempt, xr_start::AttemptOutcome& out) -> bool {
            // Owned references for the XR graphics binding, adopted by this
            // attempt's runtime object and released only by its release_binding()
            // after session destruction (retained forever on a refusal).
            device->AddRef();
            queue->AddRef();
            auto rts = xr_owner::make_openxr_stereo_runtime(device, queue);
            auto blit = make_d3d12_blit(device, queue);
            Settings attempt_settings = settings;   // VSCREEN: one hook (private GPU objects) per attempt; null = off
            if (g_vscreen_opt.on) attempt_settings.vscreen = make_brief3d_hook(make_vscreen_hook(g_vscreen_opt, device, queue), device, queue);   // BRIEF3D: unchanged unless configured
            if (attempt_settings.vscreen) attempt_settings.screen_dump_trigger = [](bool consume) -> bool {   // SCREEN-DUMP: %LOCALAPPDATA%\acz-vr\png-screen.now
                wchar_t base[MAX_PATH];
                const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
                if (!n || n >= MAX_PATH - 32) return false;
                std::wstring path(base, n); path += L"\\acz-vr\\png-screen.now";
                if (consume) return DeleteFileW(path.c_str()) != FALSE;
                const DWORD a = GetFileAttributesW(path.c_str());
                return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
            };
            if (attempt_settings.png_once) attempt_settings.png_eye_trigger = [](bool consume) -> bool {   // FLIGHT-DUMP: %LOCALAPPDATA%\acz-vr\png-eye.now
                wchar_t base[MAX_PATH];
                const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
                if (!n || n >= MAX_PATH - 32) return false;
                std::wstring path(base, n); path += L"\\acz-vr\\png-eye.now";
                if (consume) return DeleteFileW(path.c_str()) != FALSE;
                const DWORD a = GetFileAttributesW(path.c_str());
                return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
            };
            attempt_settings.flight_stage = flight_stage_attempt();   // VSEARLY: per-flight stage (null unless started above)
            auto output = std::make_shared<Output>([] { return current_source(); }, rts.swapchains, blit, &emit, attempt_settings);
            xr_owner::Config cfg;
            cfg.enabled = true;
            cfg.continuous = true;
            xr_owner::Limits limits;   // VSEARLY: the frame phase tolerates the game's load stalls (default 0 = the 250 ms rule)
            if (g_early) limits.frame_stall_ms = acz::flight::kEarlyFrameStallMs;
            auto owner = std::make_shared<xr_owner::Owner>(rts.runtime, xr_owner::make_steady_clock(), cfg, limits);
            if (!owner->set_consumer(output)) { emit(true, "xr-stereo;event=refused;reason=consumer attach"); return false; }
            owner->set_call_observer(std::make_shared<StartObserver>(attempt, rts.runtime)); // XR-START: diagnostics only
            if (g_nullhead_opt.on && !owner->set_pose_filter(std::make_shared<NullHeadFilter>(g_nullhead_opt.slew_dps)))   // NULL-HEAD (test only)
                emit(true, "xr-stereo;event=null-head;stage=refused;detail=pose-filter-attach");
            publish_owner(owner); // producer try_read_pose; owners retained for the process lifetime
            std::snprintf(text, sizeof text, "xr-stereo;event=launch;attempt=%u;device=%llx;queue=%llx;mode=continuous;"
                "limits=startup5000(first-frame-included),progress250(from-frame-2)%s,cleanup2000;"
                "keys=xr_stereo_output+gpu_copy_early_device_hook+gpu_copy_step5_continuous;source=%s;submit=original-ExecuteCommandLists",
                attempt, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(device)),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(queue)), g_early ? ",frame-stall8000(vsearly,frames-after-the-first)" : "",
                registered_frame_source() ? "registered" : "producer-frame_source");
            emit(true, text);
            if (g_stop.load(std::memory_order_acquire)) owner->request_stop();
            const unsigned finalBefore=g_evidence.final_count();
            const auto launched = xr_owner::launch(owner);
            const char* outcome = launched == xr_owner::Launch::started ? "started" : launched == xr_owner::Launch::disabled ? "disabled" :
                launched == xr_owner::Launch::already_used ? "already-used" : "thread-failure";
            std::snprintf(text, sizeof text, "xr-stereo;event=threads;attempt=%u;outcome=%s", attempt, outcome);
            emit(true, text);
            if (launched != xr_owner::Launch::started) return false;
            bool hooked_told = false;
            RecenterPoller recenter_poller;   // SEATED RECENTER: per attempt (a new session has a new neutral)
            HeadTriggerPoller head_poller;    // NULL-HEAD (test only): head.now, only with nh:1
            uint64_t early_fc = 0;   // VSEARLY: flight composites at the last poll
            while (!owner->finished()) {
                if (g_stop.load(std::memory_order_acquire)) owner->request_stop();
                if (!hooked_told && owner->first_frame_done_ms() >= 0) { // XR-START: the attempt hooked (first frame complete)
                    hooked_told = true;
                    acz::pacing::set_xr_active(true);   // UNCAP: the Present override (uncap:1 only) applies from now until this attempt ends
                    std::snprintf(text, sizeof text, "xr-stereo;event=start-attempt;n=%u;outcome=hooked;reason=first-frame-done;first-failing-call=none;xr-result=none;"
                        "first-frame-done-ms=%lld;since-source-ready-ms=%llu", attempt, static_cast<long long>(owner->first_frame_done_ms()),
                        static_cast<unsigned long long>(GetTickCount64() - source_ready_tick));
                    emit(true, text);
                }
                g_look_stick_hb.tick(output->flight_composites(), hooked_told);   // LOOK-STICK GATE: heartbeat while in-flight composites arrive
                if (g_early && hooked_told) {   // VSEARLY: the session also runs in the menus: uncap:1 applies only while flight composites arrive
                    const uint64_t fc = output->flight_composites();
                    acz::pacing::set_xr_active(fc != early_fc);
                    early_fc = fc;
                }
                // SEATED RECENTER: five 20 ms slices keep the old 100 ms cadence of everything above while a key tap (>= ~50 ms) is never missed.
                for (int slice = 0; slice < 5 && !owner->finished(); ++slice) {
                    if (hooked_told) recenter_poller.poll(*owner, output->flight_composites());
                    if (hooked_told) head_poller.poll();
                    Sleep(20);
                }
            }
            g_look_stick_hb.stop();   // LOOK-STICK GATE: the session ended: explicit inactive (the real right stick again)
            flight_stage_attempt_ended();   // VSEARLY: no stage-bound producer switch outlives the consumer that drew the stage (no-op when off)
            acz::pacing::set_xr_active(false);   // UNCAP: the session ended: the game's own Present arguments again
            // Owner publishes finished before Output::finished queues its final
            // line. Wait for that admission fence, independently of disk progress.
            if(!g_evidence.wait_final_after(finalBefore,2000)) {
                g_evidence.mark_incomplete();
                emit(true,"xr-stereo;event=incomplete-evidence;reason=consumer-final-callback-timeout");
                return false;
            }
            const auto r = owner->report();
            const xr_start::Diagnosis diag = xr_start::diagnose(r, owner->records(), owner->record_count(), output->startup_budget_refused(), limits);
            owner->release_records(); // XR-START: the retained owner keeps no record array
            {   // R-5: a finished attempt that failed transiently retired every handle (clean=1, nothing held): drop the registry's reference once no
                // pose reader can hold its pointer (xr_owner_registry.h). Others stay retained as before. Counts are logged per attempt.
                const char* rel = diag.cls == xr_start::Class::transient ? release_owner(owner, 500) : "kept(not-transient)";
                const OwnerCounts oc = owner_counts();
                std::snprintf(text, sizeof text, "xr-stereo;event=owner-retention;attempt=%u;release=%s;published=%llu;released=%llu;retained=%zu;busy=%llu",
                    attempt, rel, static_cast<unsigned long long>(oc.published), static_cast<unsigned long long>(oc.released), oc.retained,
                    static_cast<unsigned long long>(oc.busy));
                emit(true, text);
            }
            // PERF: a first frame refused for the startup budget (cold runtime start + cold PSO compile in setup, nothing submitted) is a cold-start
            // timeout too: the one retry runs with warm caches (step65 launch 1: first-frame-done-ms=4242, retry=no).
            out.report = r;
            out.diag = diag;
            out.stop = g_stop.load(std::memory_order_acquire);
            last_budget = output->startup_budget_refused();
            out.legacy_retry = !out.stop && retry_allowed(attempt, r, last_budget);
            return true;
        };
        hooks.ended = [&](unsigned attempt, const xr_start::AttemptOutcome& a, const xr_start::Decision& d) {
            const auto& r = a.report;
            const bool legacy = d.retry && std::strcmp(d.text, "yes-cold-start") == 0;
            const char* retry_text = !d.retry ? "no" : legacy ? (last_budget ? "yes-cold-start(first-frame-budget)" : "yes-cold-start") : d.text;
            std::snprintf(text, sizeof text, "xr-stereo;event=attempt-ended;attempt=%u;success=%u;first-cause=%s;loss=%u;clean=%u;"
                "first-wait-ms=%lld;first-begin-ms=%lld;first-end-ms=%lld;first-frame-done-ms=%lld;retry=%s",
                attempt, unsigned(r.success), xr_owner::cause_name(r.first_cause), unsigned(r.loss_observed), unsigned(r.clean),
                static_cast<long long>(r.first_wait_ms), static_cast<long long>(r.first_begin_ms), static_cast<long long>(r.first_end_ms),
                static_cast<long long>(r.first_frame_done_ms), retry_text);
            emit(true, text);
            start_attempt_line(attempt, a.diag, r, d.text, d.retry ? d.delay_ms : -1);
            if (d.retry && d.tier_changed) {   // R-5: each slowdown step is logged once
                std::snprintf(text, sizeof text, "xr-stereo;event=retry-slowdown;tier=%u;min-interval-ms=%lld;elapsed-ms=%lld;attempts=%u;never-stops=1",
                    d.tier, static_cast<long long>(xr_start::tier_interval_ms(d.tier)), static_cast<long long>(d.elapsed_ms), attempt);
                emit(true, text);
            }
        };
        xr_start::run_attempts(policy, hooks);
    } catch (...) {
        emit(true, "xr-stereo;event=refused;reason=exception in monitor");
    }
}
}

bool configured() noexcept {
    auto* log = diagnostics::sink();
    return log && log->feature_logs_enabled() && log->xr_stereo_output() && log->gpu_copy_early_device_hook() && log->gpu_copy_step5_continuous();
}

void on_device_ready(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        if (!configured()) {
            // Partial configuration: say so once (the key alone never starts XR).
            auto* log = diagnostics::sink();
            static std::atomic<bool> told{false};
            if (log && log->xr_stereo_output() && !told.exchange(true))
                emit(true, "xr-stereo;event=refused;reason=requires gpu_copy_early_device_hook+gpu_copy_step5_continuous+logging");
            return;
        }
        if (g_attempted.exchange(true, std::memory_order_acq_rel)) return; // one arming per process
        if (!device || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            emit(true, "xr-stereo;event=refused;reason=no game device/DIRECT queue");
            return;
        }
        // The detached monitor/owner/watchdog threads execute this module's
        // code: pin it for the process lifetime BEFORE any thread exists.
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&on_device_ready), &self)) {
            emit(true, "xr-stereo;event=refused;reason=module pin failed");
            return;
        }
        // The monitor's own references, retained for the process lifetime (each
        // owner attempt adds and later releases its own pair).
        device->AddRef();
        queue->AddRef();
        // D-232 glue: the producer pulls the latest qualified eye poses through
        // this try-only adapter (false until an owner publishes a tracked pose).
        static OwnerPoseProvider pose_provider;
        stereo::set_pose_provider(&pose_provider);
        xr_owner::set_call_bracket(&call_bracket);
        setOrderlyStopHook(&stop_hook); // orderly drain path only; never DllMain
        {   // XR-START: the retry policy is fixed for the process when armed.
            char rv[16]{};
            const DWORD rv_size = GetEnvironmentVariableA("ACZ_XR_RETRY", rv, sizeof rv);
            const bool persistent = xr_start::persistent_retry_enabled(rv_size == 0 ? nullptr : (rv_size < sizeof rv ? rv : "1"));
            g_persistent_retry.store(persistent, std::memory_order_release);
            emit(true, persistent
                ? "xr-stereo;event=armed;source-pin=" ACZ_VR_SOURCE_PIN ";start=lazy-first-published-frame;poll-ms=100;attempts=1+1-cold-start-retry;"
                  "persistent-retry=on(transient-only,3s,5s,10s,then-15s;cap-6/min;after-10min-60s;after-60min-5min;ACZ_XR_RETRY=0-disables)"
                : "xr-stereo;event=armed;source-pin=" ACZ_VR_SOURCE_PIN ";start=lazy-first-published-frame;poll-ms=100;attempts=1+1-cold-start-retry;"
                  "persistent-retry=off(ACZ_XR_RETRY)");
        }
        std::thread(monitor, device, queue).detach();
    } catch (...) {
        emit(true, "xr-stereo;event=refused;reason=exception during wiring");
    }
}
} // namespace acz::xr_stereo
