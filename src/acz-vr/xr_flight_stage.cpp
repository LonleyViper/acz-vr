// VSEARLY per-flight terrain (xr_flight_stage.h; flight_cycle.h; docs/world-render/vsearly.md). DLL only. Default OFF: nothing here runs unless the
// proxy monitor called flight_stage_start (vsearly effective + xr_terrain_stage=auto).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "xr_flight_stage.h"
#include "flight_cycle.h"
#include "world_clouds.h" // WORLD-CLOUDS
#include "effects_game_clouds.h"
#include "effects_stage_flags.h"
#include "object_interp.h"
#include "tactical_frame.h"
#include "world_dedupe.h"

#include <cstdio>
#include <cstring>
#include <thread>

namespace acz::xr_stereo {
namespace {
using Stage = acz::flight::FlightStage<TerrainRuntime>;
using LoadedT = acz::flight::Loaded<TerrainRuntime>;

struct Ctl {
    FlightStageTemplate t;
    StageDetectFn detect = nullptr;
    StageEmitFn emit = nullptr;
    const std::atomic<bool>* stop = nullptr;
    Stage stage;
    std::atomic<uint64_t> next_gen{1001};   // unique per load: the monitor's own loads use 1/2; mission = odd, takeoff = odd + 1
    int64_t last_summary_ms = -1;
    FlightLoadFn test_load = nullptr;   // offline fixtures only
};
Ctl* g_ctl = nullptr;   // process lifetime (never destroyed: the worker and adapters may outlive any scope)

void say(const char* t) noexcept { if (g_ctl && g_ctl->emit) g_ctl->emit(true, t); }

// One stage with the template's options; generation `gen` for the terrain and its props. null + why on any failure.
std::shared_ptr<TerrainRuntime> build(const std::string& id, uint64_t gen, std::string& why) {
    const TerrainRuntime& tp = *g_ctl->t.tpl;
    auto rt = std::make_shared<TerrainRuntime>();
    const std::string path = acz::terrain::stage_path(id);
    const acz::terrain::LoadStatus st = acz::terrain::load_stage_file(path, rt->stage);
    if (st != acz::terrain::LoadStatus::ok) { why = std::string("load-failed(") + acz::terrain::status_name(st) + ")"; return nullptr; }
    rt->conv = tp.conv; rt->tac = tp.tac; rt->pacing = tp.pacing;
    acz::terrain::apply_sea_convention(rt->stage, rt->conv);
    acz::terrain::build_atlas(rt->stage, rt->atlas, rt->conv.clut_raw);
    if (rt->conv.detail) {   // DTEX (dtex:1): the stage's detail texture next to its .zvrs; any failure = no detail (logged)
        std::string dwhy;
        acz::terrain::attach_detail(id, rt->conv, rt->atlas, dwhy);
        const std::string dl = "xr-stereo;event=terrain-detail;stage=" + id + ";state=" + dwhy;
        say(dl.c_str());
    }
    if (rt->conv.env_bits) acz::terrain::build_env_texture(rt->stage, rt->conv, rt->env);
    rt->generation = gen;
    why = "terrain-only";
    const acz::props::Options& popt = g_ctl->t.popt;
    if (popt.any()) {
        auto pr = std::make_shared<acz::props::Runtime>();
        if (acz::props::load_props_file(acz::props::props_path(id), pr->set) == acz::props::LoadStatus::ok && std::strcmp(pr->set.id, rt->stage.id) == 0 &&
            acz::props::snap_to_terrain(pr->set, rt->stage)) {
            pr->opt = popt;
            acz::props::build_grid(pr->set, pr->opt, pr->grid);
            pr->generation = gen;   // unique: the props worker's (pointer, generation) cache can never alias a freed set
            rt->props = pr;
            why = "props-loaded";
        } else why = "props-unavailable(terrain-only)";
    }
    return rt;
}

// Worker thread: detection (the proxy's bounded read-only scan) + load. Never touches the consumer.
LoadedT load_flight(uint64_t flight) {
    LoadedT r;
    if (g_ctl->test_load) {   // offline fixture seam: no scan, no files
        auto rt = g_ctl->test_load(flight);
        if (!rt) { r.why = "test-no-stage"; return r; }
        r.rt = rt; r.id = rt->stage.id; r.gens.push_back(rt->generation);
        if (rt->takeoff) r.gens.push_back(rt->takeoff->generation);
        r.has_props = bool(rt->props) || (rt->takeoff && rt->takeoff->props);
        return r;
    }
    char t[480];
    std::snprintf(t, sizeof t, "xr-stereo;event=flight-stage-detect;flight=%llu;stage=auto;scan=read-only(stage_detect_win)", (unsigned long long)flight);
    say(t);
    const ULONGLONG t0 = GetTickCount64();
    std::string id, sha;
    if (!g_ctl->detect || !g_ctl->detect(id, sha)) { r.why = "detection-no-stage(see-terrain-disabled-line)"; return r; }
    const uint64_t gen = g_ctl->next_gen.fetch_add(2);
    std::string why;
    auto rt = build(id, gen, why);
    if (!rt) { r.why = why; return r; }
    r.has_props = bool(rt->props);
    r.gens.push_back(gen);
    const char* tid = rt->conv.takeoff_id;   // TAKEOFF: the airbase stage of this flight's mission (same rules as the monitor's load)
    std::string twhy = tid[0] ? "same-as-mission-stage" : "option-off";
    if (tid[0] && std::strcmp(tid, rt->stage.id) != 0) {
        auto tk = build(tid, gen + 1, twhy);
        if (tk && acz::terrain::adopt_mission_env(tk->stage, rt->stage, tk->conv) && tk->conv.env_bits)   // TAKEOFF-LOOK (toenv:, default mission)
            acz::terrain::build_env_texture(tk->stage, tk->conv, tk->env);
        if (tk) { r.has_props = r.has_props || bool(tk->props); r.gens.push_back(gen + 1); rt->takeoff = tk; twhy = "loaded(" + twhy + ")"; }
    }
    if (!sha.empty() && sha != acz::terrain::hex(rt->stage.payload_sha, 32)) {
        std::snprintf(t, sizeof t, "xr-stereo;event=terrain-auto-warning;reason=zvrs-differs-from-fingerprint-table;stage=%s;table-sha256=%s", rt->stage.id, sha.c_str());
        say(t);
    }
    // K00A-SNOW / SNOW-LOOK: clut= (the vsearly path had no terrain-loaded line, so batch I could not show that tclut:raw reached the loader) and the
    // look profile of the mission and takeoff stages (gain / sky override / haze floor) as built into their env textures.
    auto look_tag = [](const TerrainRuntime* r) -> std::string {
        if (!r) return "none";
        if (!r->conv.look || r->env.empty()) return "off";
        const acz::terrain::StageLook l = acz::terrain::stage_look(r->stage.id);
        char b[96];
        std::snprintf(b, sizeof b, "%.2f/%.2f/%.2f%s,floor=%.2f,desat=%.2f,hazepow=%.2f", double(l.gain[0]), double(l.gain[1]), double(l.gain[2]), l.sky_set ? ",sky=profile" : ",sky=own", double(l.haze_floor), double(l.desat), double(l.haze_pow));
        return b;
    };
    const std::string lm = look_tag(rt.get()), lt = look_tag(rt->takeoff.get());
    std::snprintf(t, sizeof t, "xr-stereo;event=flight-stage-loaded;flight=%llu;stage=%s;gen=%llu;tiles=%u;env=%s;%s;takeoff=%s:%s;clut=%s;look=%s;takeoff-look=%s;"
        "takeoff-env=%s;load-ms=%llu", (unsigned long long)flight,
        rt->stage.id, (unsigned long long)gen, rt->stage.ntiles, rt->env.empty() ? "no" : "yes", why.c_str(), tid[0] ? tid : "off", twhy.c_str(),
        rt->conv.clut_raw ? "raw" : "csm1", lm.c_str(), lt.c_str(), rt->conv.takeoff_env_own ? "own" : "mission", (unsigned long long)(GetTickCount64() - t0));
    say(t);
    r.id = rt->stage.id;
    r.rt = rt;
    return r;
}

// Producer-side switches that follow the drawn stage (the monitor sets them once after its own load; here per flight, cleared between flights).
// Producer-side switches that follow the drawn stage: the same switches (and lines) as the monitor's one-time load in xr_stereo_proxy.cpp, set at each
// install and cleared between flights / at a session rollover / when an attempt ends. The effects switches go through effects_stage_flags.h, shared with
// the monitor (live batch Y2: the copy here had missed fxmem:5 and logged nothing, so fxmem:4/5 GPU smoke and explosions never armed with vsearly).
// fxmem:4/5 smoke GPU resources: the step-5 worker creates them LATE on its first tick after effects_smoke_gpu_flag turns on (gpu_copy_step5_hooks.inl
// step5CreateSmokeGpu, once per process), which is exactly this path: the flag arrives at the first flight's install, after the ring.
void producer_flags(const TerrainRuntime* rt) {
    namespace s5 = acz::gpu_copy::step5;
    if (!rt) {
        acz::props::clear_dedupe();
        acz::tac::set_producer_flags(0);
        acz::interp::arm(false, 0.0f, 0.0f, false);
        s5::clear_stage_effects();
        return;
    }
    const auto& cv = rt->conv;
    char tt[480];
    if (rt->props && rt->props->opt.props && rt->props->opt.dedupe) {   // WORLD-4
        const std::vector<float> pl = acz::props::prop_placements(rt->props->set);
        acz::props::publish_dedupe(pl, rt->props->opt.dedupe_tol);
        std::snprintf(tt, sizeof tt, "xr-stereo;event=props-dedupe;stage=%s;placements=%zu;tol=%.2f;refusal=world-dedupe-prop(category-target,not-a-loss)", rt->stage.id,
            pl.size() / 3, double(rt->props->opt.dedupe_tol));
        say(tt);
    } else acz::props::clear_dedupe();
    const auto& po = rt->pacing;
    acz::interp::arm(po.interp, po.interp_max_frames, po.extrap_max_ms, po.extrap_attitude);
    if (po.interp) {
        std::snprintf(tt, sizeof tt, "xr-stereo;event=interp-armed;%s;needs=gpu_copy_step5_world_objects;camera=producer-delta(replaces-extrap:1-for-the-terrain);"
            "moves=replayed-objects,terrain,props,clouds,tactical-markers;game-rate=hud,effects,game-marker-sprites,replayed-cockpit;source=flight-stage", acz::pacing::describe(po).c_str());
        say(tt);
    }
    acz::tac::set_producer_flags((rt->tac.on || rt->tac.mk360) ? acz::tac::producer_flags_for(rt->tac) : 0);
    if (rt->tac.on || rt->tac.mk360) {
        std::snprintf(tt, sizeof tt, "xr-stereo;event=tac-armed;%s;producer-flags=%u;source=flight-stage", acz::tac::describe(rt->tac).c_str(), acz::tac::producer_flags());
        say(tt);
    }
    const bool ours = (cv.env_bits & acz::terrain::kEnvCloud) != 0 && !rt->env.empty();
    const bool drop = acz::terrain::drop_game_clouds(cv, ours);
    if (drop || cv.game_cloud >= 0) {
        std::snprintf(tt, sizeof tt, "xr-stereo;event=game-clouds;replay=%s;reason=%s;our-clouds=%s;class=effects-ps-4caf6e2d+depth-greater-equal", drop ? "off" : "on",
            cv.game_cloud >= 0 ? "gamecloud-option" : "default(cloud:1-active)", ours ? "yes" : "no");
        say(tt);
    }
    {   // WORLD-CLOUDS (world_clouds.h): the config line and the producer's puff drop edge / census switch
        const bool envp = !rt->env.empty();
        const acz::wcloud::Params wp = envp ? acz::wcloud::params_from(&rt->env[size_t(acz::wcloud::kPWc) * 4], &rt->env[size_t(acz::wcloud::kPWc2) * 4]) : acz::wcloud::Params{};
        if (cv.wcloud) { acz::wcloud::config_line(tt, sizeof tt, true, envp, wp, acz::wcloud::producer_drop_edge(cv.wcloud, envp && wp.on, cv.fx_memory, cv.wc_drop_edge), cv.wc_diag,
            acz::wcloud::band_source(rt->stage.env_id[0] ? rt->stage.env_id : rt->stage.id, cv.wc_alt)); say(tt); }
        s5::apply_stage_effects(s5::stage_effects(cv.fx_memory, drop, cv.fx_sig, cv.hud_rs_skip,
            acz::wcloud::producer_drop_edge(cv.wcloud, envp && wp.on, cv.fx_memory, cv.wc_drop_edge), cv.wc_diag, cv.fx_trail_s, cv.fx_trail_max_edge, cv.fx_trail_cls, cv.fx_trail_near,
            s5::PuffCloud{cv.fx_cloud_s, cv.fx_cloud_far, cv.fx_trail_xkey, cv.fx_trail_clip, cv.fx_atlas}, cv.hud_late, cv.fx_add));   // (P) fxadd; (M4) fxcloud / fxcfar / fxtxkey / (M4c) fxtclip; HUD-LATE hudlate
    }
    if (const char* fl = s5::fx_memory_line(cv.fx_memory)) say(fl);
    if (cv.hud_late) say(s5::hud_late_option_line());   // HUD-LATE (hud_late.h)
    if (s5::fx_additive_line(tt, sizeof tt, cv.fx_memory, cv.fx_add)) say(tt);   // EFFECTS-OUTSIDE (P) fxadd
    if (s5::fx_puff_trail_line(tt, sizeof tt, cv.fx_memory, cv.fx_trail_s, cv.fx_trail_max_edge, cv.fx_trail_cls, cv.fx_trail_near,
            s5::PuffCloud{cv.fx_cloud_s, cv.fx_cloud_far, cv.fx_trail_xkey, cv.fx_trail_clip, cv.fx_atlas})) say(tt);   // EFFECTS-OUTSIDE (M) + (M4)
}

class Adapter final : public FlightStageHook {
public:
    bool frame(int64_t now_ms, int64_t last_new_ms, std::shared_ptr<const TerrainRuntime>& terrain) override {
        if (!g_ctl) return false;
        if (!begun_) {
            begun_ = true;
            g_ctl->last_summary_ms = -1;   // P3: the summary timer belongs to the previous session's owner clock
            g_ctl->stage.begin_session(now_ms);   // runs the rollover hook (producer switches cleared) before this consumer's first frame work
            say(("xr-stereo;event=flight-stage-session;" + g_ctl->stage.summary()).c_str());
        }
        const bool changed = g_ctl->stage.frame(now_ms, last_new_ms, terrain, alive_ms_);
        if (g_ctl->last_summary_ms < 0 || now_ms - g_ctl->last_summary_ms >= 10000) {
            g_ctl->last_summary_ms = now_ms;
            if (g_ctl->emit) g_ctl->emit(false, ("xr-stereo;event=summary-flight-stage;" + g_ctl->stage.summary()).c_str());
        }
        return changed;
    }
    void recorded(uint64_t gen, bool props_job, bool settled) override { if (g_ctl) g_ctl->stage.recorded(gen, props_job, settled); }
    void view_alive(int64_t alive_ms) override { alive_ms_ = alive_ms; }   // VIEW-MODE pov:1 (view_mode.h)
private:
    bool begun_ = false;
    int64_t alive_ms_ = -1;
};
} // namespace

bool flight_stage_start(FlightStageTemplate t, StageDetectFn detect, StageEmitFn emit, const std::atomic<bool>* stop, bool worker) {
    if (g_ctl || !t.tpl) return false;
    auto* c = new Ctl;
    c->t = std::move(t); c->detect = detect; c->emit = emit; c->stop = stop;
    Stage::Hooks h;
    h.installed = [](const LoadedT& l, bool reused, uint64_t flight) {
        // CAM-EPOCH: a newly drawn stage starts a new stage epoch (samples published before it are refused by the admission), as at the monitor's load.
        const uint32_t ep = reused ? acz::terrain::cam_epochs().stage() : acz::terrain::cam_epochs().bump_stage();
        producer_flags(l.rt.get());
        char tt[420];
        std::snprintf(tt, sizeof tt, "xr-stereo;event=flight-stage-installed;flight=%llu;stage=%s;reused=%u;gens=%zu;props=%u;stage-epoch=%u;lock-epoch=%u;%s",
            (unsigned long long)flight, l.id.c_str(), reused ? 1u : 0u, l.gens.size(), l.has_props ? 1u : 0u, ep, acz::terrain::cam_epochs().lock(),
            g_ctl->stage.summary().c_str());
        say(tt);
    };
    h.ended = [](uint64_t flight, bool had) {
        producer_flags(nullptr);
        char tt[420];
        std::snprintf(tt, sizeof tt, "xr-stereo;event=flight-stage-ended;flight=%llu;had-stage=%u;drawn=none-until-next-flight;%s", (unsigned long long)flight,
            had ? 1u : 0u, g_ctl->stage.summary().c_str());
        say(tt);
    };
    h.rollover = [](uint64_t flight, bool in_flight, bool had) {
        producer_flags(nullptr);   // Astra 6458816088 P2: a session lost/stalled in flight never ran `ended`
        char tt[200];
        std::snprintf(tt, sizeof tt, "xr-stereo;event=flight-stage-rollover;last-flight=%llu;was-in-flight=%u;had-stage=%u;producer-switches=cleared;drawn=none-until-next-install",
            (unsigned long long)flight, in_flight ? 1u : 0u, had ? 1u : 0u);
        say(tt);
    };
    h.no_stage = [](const LoadedT& l, uint64_t flight) {
        char tt[300];
        std::snprintf(tt, sizeof tt, "xr-stereo;event=flight-stage-none;flight=%llu;reason=%s;fallback=game-world-image", (unsigned long long)flight, l.why.c_str());
        say(tt);
    };
    c->stage.set_hooks(std::move(h));
    g_ctl = c;
    producer_flags(nullptr);   // nothing stage-bound runs in the menus
    if (worker) try {
        std::thread([] {
            for (;;) {
                if (g_ctl->stop && g_ctl->stop->load(std::memory_order_acquire)) return;
                if (!g_ctl->stage.work(&load_flight)) Sleep(50);
            }
        }).detach();
    } catch (...) { say("xr-stereo;event=flight-stage-disabled;reason=worker-thread-failed;fallback=no-terrain"); return false; }
    say("xr-stereo;event=flight-stage-config;stage=auto;detect=per-flight(first-fresh-composite);end=no-fresh-composite-8000ms;park-ms=60000;"
        "between-flights=no-terrain;retire=props-settled-on-newer-stage");
    return true;
}

std::shared_ptr<FlightStageHook> flight_stage_attempt() {
    if (!g_ctl) return nullptr;
    return std::make_shared<Adapter>();
}
void flight_stage_attempt_ended() noexcept {
    if (!g_ctl) return;
    producer_flags(nullptr);   // atomics only; the controller's stage state stays owner-thread (the next session parks it)
    say("xr-stereo;event=flight-stage-attempt-ended;producer-switches=cleared");
}
void flight_stage_set_loader_for_test(FlightLoadFn load) { if (g_ctl) g_ctl->test_load = load; }
bool flight_stage_run_pending_for_test() { return g_ctl && g_ctl->stage.work(&load_flight); }

} // namespace acz::xr_stereo
