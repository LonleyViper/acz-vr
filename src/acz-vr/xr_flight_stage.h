#pragma once
// VSEARLY per-flight terrain (DLL only; xr_flight_stage.cpp; docs/world-render/vsearly.md). Created by the proxy monitor ONLY when vsearly is
// effective AND xr_terrain_stage names the stage `auto`; then nothing is detected or loaded at XR start (the menus): every flight start asks
// one worker thread to run the existing read-only stage detection and to load that stage (+ its takeoff stage) with the template's options;
// the consumer draws it from the first frame after it is installed, and draws no terrain between flights. Absent = the previous behaviour.
#include <atomic>
#include <memory>
#include <string>

#include "world_props.h"
#include "xr_stereo_output.h"

namespace acz::xr_stereo {

struct FlightStageTemplate {
    std::shared_ptr<const TerrainRuntime> tpl;   // conv/tac/pacing parsed from xr_terrain_stage (no stage loaded)
    acz::props::Options popt;                     // props:/trees: options
};
using StageDetectFn = bool (*)(std::string& id, std::string& table_sha);   // the proxy's detect_auto_stage (logs its own lines)
using StageEmitFn = void (*)(bool lifecycle, const char* text);

// The process-wide controller and its worker thread (started here; the module is pinned by the caller). Returns false on a thread failure.
// `worker` false = no worker thread (offline fixtures run flight_stage_run_pending_for_test instead).
bool flight_stage_start(FlightStageTemplate t, StageDetectFn detect, StageEmitFn emit, const std::atomic<bool>* stop, bool worker = true);
// One adapter per XR attempt (its first frame() begins a new session of the controller). null when flight_stage_start did not run.
std::shared_ptr<FlightStageHook> flight_stage_attempt();
// Monitor thread, after an attempt's owner finished (no consumer frame can run): clears every stage-bound producer switch (dedupe, tac/mk360,
// interp, game-cloud suppression, fx flags), so the game's own records are replayed until the next attempt installs a stage. No-op when off.
void flight_stage_attempt_ended() noexcept;
// Offline fixtures only: replace detection + load by `load` (null runtime = no stage) and run one pending request synchronously.
using FlightLoadFn = std::shared_ptr<const TerrainRuntime> (*)(uint64_t flight);
void flight_stage_set_loader_for_test(FlightLoadFn load);
bool flight_stage_run_pending_for_test();

} // namespace acz::xr_stereo
