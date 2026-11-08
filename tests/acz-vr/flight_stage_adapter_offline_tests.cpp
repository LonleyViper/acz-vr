// VSEARLY adapter fixture (Astra 6458816088 P2): the REAL xr_flight_stage.cpp adapter and its producer switches (process-wide atomics read by
// the step-5 producer): install A with suppression -> the session is lost IN FLIGHT (no flight-end callback) -> a new attempt with detection
// pending, then failing -> every stage-bound switch is cleared (the game's own records replayed) -> install B re-applies only B's switches.
// Also: flight_stage_attempt_ended() clears them when the old attempt ends. Loader seam instead of the memory scan; no device, no headset.
#include "xr_flight_stage.h"
#include "terrain_stage.h"
#include "effects_game_clouds.h"
#include "effects_stage_flags.h"
#include "object_interp.h"
#include "tactical_frame.h"
#include "world_dedupe.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace acz::xr_stereo;
namespace s5 = acz::gpu_copy::step5;

std::vector<std::string> g_lines;
void emit_line(bool, const char* t) { g_lines.push_back(t); }
bool has_line(const char* needle) { for (const auto& l : g_lines) if (l.find(needle) != std::string::npos) return true; return false; }

std::shared_ptr<const TerrainRuntime> stage_a() {   // every stage-bound producer switch ON
    auto rt = std::make_shared<TerrainRuntime>();
    std::strcpy(rt->stage.id, "aaaa");
    rt->generation = 3001;
    rt->conv.env_bits = acz::terrain::kEnvCloud; rt->env.assign(4, 1.0f);   // our clouds -> the game's cloud billboards suppressed
    rt->conv.fx_memory = 4;
    rt->tac.mk360 = 1;
    rt->pacing.interp = true;
    auto pr = std::make_shared<acz::props::Runtime>();
    pr->opt.props = true; pr->opt.dedupe = true; pr->generation = 3001;
    rt->props = pr;
    return rt;
}
std::shared_ptr<const TerrainRuntime> stage_b() {   // terrain only: no stage-bound switch
    auto rt = std::make_shared<TerrainRuntime>();
    std::strcpy(rt->stage.id, "bbbb");
    rt->generation = 3003;
    return rt;
}
std::shared_ptr<const TerrainRuntime> stage_c() {   // fxmem:5 (explosions; live batch Y2: never armed with vsearly before the shared switch table)
    auto rt = std::make_shared<TerrainRuntime>();
    std::strcpy(rt->stage.id, "cccc");
    rt->generation = 3005;
    rt->conv.fx_memory = 5;
    return rt;
}
int g_mode = 0;   // 0 = A, 1 = none (detection fails), 2 = B, 3 = C
std::shared_ptr<const TerrainRuntime> loader(uint64_t) { return g_mode == 0 ? stage_a() : g_mode == 2 ? stage_b() : g_mode == 3 ? stage_c() : nullptr; }
size_t count_lines(const char* needle) { size_t n = 0; for (const auto& l : g_lines) if (l.find(needle) != std::string::npos) ++n; return n; }

struct Flags { bool dedupe, clouds, gpu, replay, capture, census, interp; uint32_t tac; bool explosion, sprite; };
Flags flags() {
    return {acz::props::dedupe_slot().load() != nullptr, s5::effects_drop_game_clouds_flag().load(), s5::effects_smoke_gpu_flag().load(),
            s5::effects_smoke_replay_flag().load(), s5::effects_smoke_capture_flag().load(), s5::effects_census_flag().load(),
            acz::interp::shared().on.load() != 0, acz::tac::producer_flags(), s5::effects_explosion_flag().load(), s5::effects_sprite_gpu_flag().load()};
}
bool all_clear(const Flags& f) { return !f.dedupe && !f.clouds && !f.gpu && !f.replay && !f.capture && !f.census && !f.interp && f.tac == 0 && !f.explosion && !f.sprite; }
} // namespace

int main() {
    FlightStageTemplate t;
    t.tpl = std::make_shared<TerrainRuntime>();
    CHECK(flight_stage_start(t, nullptr, &emit_line, nullptr, false));
    flight_stage_set_loader_for_test(&loader);
    CHECK(all_clear(flags()));   // nothing stage-bound in the menus

    // Attempt 1: flight 1 installs A (every switch on).
    auto a1 = flight_stage_attempt();
    std::shared_ptr<const TerrainRuntime> io1;
    CHECK(!a1->frame(0, -1, io1));
    CHECK(!a1->frame(100, 90, io1));          // flight 1 starts: request
    CHECK(flight_stage_run_pending_for_test());
    CHECK(a1->frame(111, 100, io1) && io1 && std::strcmp(io1->stage.id, "aaaa") == 0);
    Flags f = flags();
    CHECK(f.dedupe && f.clouds && f.gpu && f.replay && f.capture && !f.census && f.interp && f.tac != 0 && !f.explosion);
    // the same lines as the monitor's one-time load (vsearly + auto + fxmem:4)
    CHECK(count_lines("event=fx-memory;requested=4;active=capture+replay-dry-run+replay-per-eye-gpu") == 1);
    CHECK(has_line("event=interp-armed;") && has_line("event=tac-armed;") && has_line("event=game-clouds;replay=off") && has_line("event=props-dedupe;stage=aaaa"));

    // The session is lost IN FLIGHT (no 8 s flight end). Path 1: the new attempt's first frame (rollover) clears everything, even if the monitor
    // did not run flight_stage_attempt_ended.
    auto a2 = flight_stage_attempt();
    std::shared_ptr<const TerrainRuntime> io2;
    g_mode = 1;
    CHECK(!a2->frame(5, -1, io2) && !io2);
    CHECK(all_clear(flags()));
    CHECK(has_line("flight-stage-rollover;last-flight=1;was-in-flight=1;had-stage=1;producer-switches=cleared"));
    CHECK(!a2->frame(50, 40, io2) && !io2);   // flight 2 starts; detection pending: still the game's own records
    CHECK(all_clear(flags()));
    CHECK(flight_stage_run_pending_for_test());
    CHECK(!a2->frame(60, 50, io2) && !io2);   // detection failed: no stage, switches stay clear
    CHECK(all_clear(flags()));
    CHECK(has_line("flight-stage-none;flight=2"));

    // Flight 3 installs B: only B's switches (none).
    CHECK(!a2->frame(9000, 50, io2));         // flight 2 ends (8 s)
    g_mode = 2;
    CHECK(!a2->frame(9100, 9090, io2));       // flight 3
    CHECK(flight_stage_run_pending_for_test());
    CHECK(a2->frame(9111, 9100, io2) && io2 && std::strcmp(io2->stage.id, "bbbb") == 0);
    CHECK(all_clear(flags()));

    // Path 2: the monitor clears when an attempt ends (A installed again, then the attempt ends).
    CHECK(a2->frame(20000, 9100, io2) && !io2);   // flight 3 ends
    g_mode = 0;
    CHECK(!a2->frame(20100, 20090, io2));
    CHECK(flight_stage_run_pending_for_test());
    CHECK(a2->frame(20111, 20100, io2) && io2);
    CHECK(!all_clear(flags()));
    flight_stage_attempt_ended();
    CHECK(all_clear(flags()));
    CHECK(has_line("flight-stage-attempt-ended;producer-switches=cleared"));

    // fxmem:5 in a new attempt: GPU smoke + replay + capture + explosion, the requested=5 line; cleared at the next rollover.
    auto a3 = flight_stage_attempt();
    std::shared_ptr<const TerrainRuntime> io3;
    g_mode = 3;
    CHECK(!a3->frame(0, -1, io3));
    CHECK(!a3->frame(100, 90, io3));
    CHECK(flight_stage_run_pending_for_test());
    CHECK(a3->frame(110, 100, io3) && io3 && std::strcmp(io3->stage.id, "cccc") == 0);
    f = flags();
    CHECK(f.gpu && f.replay && f.capture && f.explosion && !f.sprite && !f.census && !f.dedupe && !f.interp && f.tac == 0);
    CHECK(count_lines("event=fx-memory;requested=5;active=4+explosion-window-effects") == 1);
    auto a4 = flight_stage_attempt();
    std::shared_ptr<const TerrainRuntime> io4;
    CHECK(!a4->frame(0, -1, io4) && all_clear(flags()));
    // EFFECTS-OUTSIDE (F) fxmem:6 (the shared switch table): 5 + the sprite GPU replay; 0..5 never set it.
    for (int m = 0; m <= 5; ++m) CHECK(!s5::stage_effects(m, false).sprite_gpu);
    {const auto e6 = s5::stage_effects(6, false);
     CHECK(e6.sprite_gpu && e6.explosion && e6.smoke_gpu && e6.smoke_replay && e6.smoke_capture && !e6.census);
     s5::apply_stage_effects(e6); f = flags();
     CHECK(f.sprite && f.explosion && f.gpu && f.replay && f.capture && !f.census);
     const char* l6 = s5::fx_memory_line(6);
     CHECK(l6 && std::strstr(l6, "event=fx-memory;requested=6;active=5+sprite-replay-per-eye-gpu;") && std::strlen(l6) < 480);
     s5::clear_stage_effects(); CHECK(all_clear(flags()));}
    // EFFECTS-OUTSIDE (D) fxsig:<ps8>: parsed into the conventions, carried by the shared switch table (fxmem >= 1 only), cleared with the rest.
    {std::string id, why; acz::terrain::Conventions c;
     CHECK(acz::terrain::parse_stage_option("cccc;fxmem:6;fxsig:d34ee153", id, c, why) && c.fx_memory == 6 && c.fx_sig == 0xd34ee153u);
     CHECK(acz::terrain::parse_stage_option("cccc;fxsig:D34EE153;fxmem:3", id, c, why) && c.fx_sig == 0xd34ee153u);
     CHECK(acz::terrain::parse_stage_option("cccc;fxmem:3", id, c, why) && c.fx_sig == 0);
     for (const char* bad : {"cccc;fxsig:d34ee15", "cccc;fxsig:d34ee1530", "cccc;fxsig:d34ee15g", "cccc;fxsig:00000000", "cccc;fxsig:"})
         CHECK(!acz::terrain::parse_stage_option(bad, id, c, why));
     CHECK(s5::stage_effects(6, false, 0xd34ee153u).fx_sig == 0xd34ee153u && s5::stage_effects(3, false, 0x12345678u).fx_sig == 0x12345678u);
     CHECK(s5::stage_effects(0, false, 0xd34ee153u).fx_sig == 0 && s5::stage_effects(6, false).fx_sig == 0);   // needs the effects seam (fxmem >= 1)
     s5::apply_stage_effects(s5::stage_effects(5, false, 0xd34ee153u)); CHECK(s5::effects_sig_filter().load() == 0xd34ee153u);
     s5::clear_stage_effects(); CHECK(s5::effects_sig_filter().load() == 0 && all_clear(flags()));}
    // HUD-RS-SKIP hudrs:0|1: parsed into the conventions (default 1), carried by the shared switch table, cleared back to ON (the default).
    {std::string id, why; acz::terrain::Conventions c;
     CHECK(acz::terrain::parse_stage_option("cccc", id, c, why) && c.hud_rs_skip);
     CHECK(acz::terrain::parse_stage_option("cccc;hudrs:0", id, c, why) && !c.hud_rs_skip);
     CHECK(acz::terrain::parse_stage_option("cccc;fxmem:6;hudrs:1", id, c, why) && c.hud_rs_skip && c.fx_memory == 6);
     CHECK(!acz::terrain::parse_stage_option("cccc;hudrs:2", id, c, why) && !acz::terrain::parse_stage_option("cccc;hudrs:", id, c, why));
     CHECK(s5::hud_rs_skip_flag().load() && s5::stage_effects(0, false).hud_rs_skip && !s5::stage_effects(6, false, 0, false).hud_rs_skip);
     s5::apply_stage_effects(s5::stage_effects(0, false, 0, false)); CHECK(!s5::hud_rs_skip_flag().load());
     s5::clear_stage_effects(); CHECK(s5::hud_rs_skip_flag().load());}

    std::printf("flight_stage_adapter_offline: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
