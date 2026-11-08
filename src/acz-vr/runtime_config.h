#pragma once
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace acz::diagnostics {
// D-400: xr_terrain_stage value <= 1023 characters (was 255; capacity 1024), max_config_line 1056 = 17 + 1023 + CRLF slack, max_config_bytes 4096 (was 2048).
// The value is only ever read through xr_terrain_stage() into std::string consumers, and the preset vrn:1 (terrain_preset.h) shortens the line anyway.
inline constexpr size_t max_config_bytes=4096, max_config_line=1056, max_config_keys=32;
inline constexpr size_t xr_terrain_stage_capacity=1024;  // value <= 1023 characters (+ NUL), AFTER preset expansion; runtime_config.cpp, diagnostics_log.h
// Diagnostic storage ceilings are compile-time constants in diagnostics_log.h.
// No config key raises them. The existing parser only disables logging/capture
// or tunes verbosity; ownership pins are an explicit diagnostics opt-in.
// Unknown numeric-cap keys invalidate the whole config.
// Owned value; no capture/GPU/XR enable switch exists. Only the parser can construct
// a non-default configuration. Publication is by value, never borrowed text.
class Config {
public:
    bool gpu_copy_witness_observation() const noexcept { return gpu_copy_witness_observation_; }
    // GPU-copy step 3b part 2 / design section 6 step 1: one known-pattern copy
    // between two proxy-owned buffers at a matched game-list Draw seam. Default
    // OFF; effective only with logging, ownership pins and witness observation.
    bool gpu_copy_step1_experiment() const noexcept { return gpu_copy_step1_experiment_; }
    // J-3b4: early device witness through the optional d3d12.dll proxy's
    // device-created notification. Default OFF; effective only together with the
    // step-1 experiment and every opt-in it requires.
    bool gpu_copy_early_device_hook() const noexcept { return gpu_copy_early_device_hook_; }
    // J-3b6: design section 6 step 2, read-only copies of the selected draw's
    // bound game buffers. Default OFF; effective only with logging, ownership
    // pins, witness observation AND gpu_copy_early_device_hook. Mutually
    // exclusive with gpu_copy_step1_experiment (both on refuses both).
    bool gpu_copy_step2_experiment() const noexcept { return gpu_copy_step2_experiment_; }
    // J-3b7: step 3, per-eye replays of the matched draw into proxy-owned
    // targets with substituted SrtData. Same opt-ins as step 2; mutually
    // exclusive with steps 1 and 2 (any two on refuses all).
    bool gpu_copy_step3_experiment() const noexcept { return gpu_copy_step3_experiment_; }
    // J-3b7f: step 3 replays the whole visible DrawCPT set of one list per eye.
    // Effective only together with gpu_copy_step3_experiment.
    bool gpu_copy_step3_whole_set() const noexcept { return gpu_copy_step3_whole_set_; }
    // J-171: CPU-only tail observation; default OFF, only while step 5 is active.
    bool gpu_copy_step5_hud_observe() const noexcept { return gpu_copy_step5_hud_observe_; }
    // J-hud-capture: replay of the post-cockpit HUD tail draws into a private per-slot target
    // (StereoFrame::hud). Default OFF; effective only while the step-5 producer runs.
    bool gpu_copy_step5_hud() const noexcept { return gpu_copy_step5_hud_; }
    // J-3b8: step 5, the continuous per-frame stereo producer (FrameSource).
    // Effective only together with gpu_copy_step3_experiment + gpu_copy_step3_whole_set.
    bool gpu_copy_step5_continuous() const noexcept { return gpu_copy_step5_continuous_; }
    // J-cockpit-fov (all default OFF; effective only while the step-5 producer runs):
    //  * hmd_fov: the pose-driven eye matrices use each eye's own XR FOV instead of the game lens
    //    (StereoFrame::cockpit_fov carries what was rendered); requires gpu_copy_step5_continuous;
    //  * precockpit_observe: read-only log of the DrawCPT draws that precede the cockpit members;
    //  * members_by_class: frame membership by signature class instead of the selected ordinal range.
    bool gpu_copy_step5_hmd_fov() const noexcept { return gpu_copy_step5_hmd_fov_; }
    // gpu_copy_step5_units_per_metre [20, 100] (default 39.6 = step3::units_per_metre, the E9 scale ASSUMPTION): game units per
    // real metre for the head-pose offsets; larger = the cockpit looks smaller (eyes further apart in cockpit units).
    float gpu_copy_step5_units_per_metre() const noexcept { return gpu_copy_step5_units_per_metre_; }
    bool gpu_copy_step5_precockpit_observe() const noexcept { return gpu_copy_step5_precockpit_observe_; }
    bool gpu_copy_step5_members_by_class() const noexcept { return gpu_copy_step5_members_by_class_; }
    // J-world-objects (default OFF; effective only together with gpu_copy_step5_continuous AND
    // gpu_copy_step5_members_by_class): the 3D-path world-object draws (DrawACM_v, per-draw SrtData record
    // with ABSOLUTE world coordinates) that follow the first cockpit member of a frame are replayed per eye
    // with the same pose substitution as the cockpit (src/acz-vr/J-world-objects-findings.md).
    bool gpu_copy_step5_world_objects() const noexcept { return gpu_copy_step5_world_objects_; }
    // J-world-objects v2 plan step 1 (default OFF; effective only together with gpu_copy_step5_continuous): a read-only,
    // bounded observer of the DrawACM_v draws wherever they are recorded (command-list route/ordinal, Execute order and
    // queue, render target and depth-stencil identity, producer frame state, decoded camera of the first records) and of
    // where the other draw types sit relative to them. Records no command and adds no GPU work.
    bool gpu_copy_step5_world_observe() const noexcept { return gpu_copy_step5_world_observe_; }
    // J-phase-observe (default OFF; effective only together with gpu_copy_step5_continuous): a read-only, bounded observer that
    // tags EVERY draw of the cockpit command list with a frame phase (before the first cockpit member, member, between members,
    // HUD region, after the region end, after the tone-map) and logs the per-phase draw tables only when the per-phase counts or
    // shapes change versus the previous frame (plus a 1-per-5-s sample). Records no command and adds no GPU work.
    bool gpu_copy_step5_phase_observe() const noexcept { return gpu_copy_step5_phase_observe_; }
    // J-hud-colour: once per process (two samples) read back the producer's HUD target, world
    // snapshot and eye-0 image of a frame with a valid HUD and world and write 8-bit PNGs plus
    // raw statistics (src/acz-vr/J-hud-colour-findings.md). Default OFF; effective only while the
    // step-5 producer runs with gpu_copy_step5_hud (requires gpu_copy_step5_continuous).
    bool gpu_copy_step5_png_once() const noexcept { return gpu_copy_step5_png_once_; }
    // J-xr-stereo-output: in-proxy XR consumer; default OFF; effective only with gpu_copy_early_device_hook and gpu_copy_step5_continuous (and logging).
    bool xr_stereo_output() const noexcept { return xr_stereo_output_; }
    // J-hud-consumer: HUD quad layer in the XR consumer; default OFF; effective only with xr_stereo_output.
    // hud_mode: 0 = world-quad (default), 1 = head-locked (fallback).
    bool xr_hud_layer() const noexcept { return xr_hud_layer_; }
    unsigned hud_mode() const noexcept { return hud_mode_; }
    // J-feather (both default OFF / unchanged; effective only with xr_stereo_output):
    // xr_edge_feather = unsigned decimal fraction of the layer half-extent, clamped to
    // [0, 0.25] (0 = OFF); xr_world_quad_distance_m = unsigned decimal metres, clamped to
    // [5, 2000] (default 40). A non-decimal value rejects the config like every key.
    float xr_edge_feather() const noexcept { return xr_edge_feather_; }
    float xr_world_quad_distance_m() const noexcept { return xr_world_quad_distance_m_; }
    bool xr_edge_feather_set() const noexcept { return xr_edge_feather_set_; }
    bool xr_world_quad_distance_set() const noexcept { return xr_world_quad_distance_set_; }
    // J-hud-placement (all default unset / unchanged; effective only with xr_stereo_output and
    // xr_hud_layer), unsigned decimals like the J-feather keys, clamped:
    // xr_hud_quad_distance_m [0.5, 2000] (unset = 0: the world quad's distance),
    // xr_hud_scale [0.25, 2] (default 1), xr_hud_head_distance_m [0.5, 20] (default 2.5),
    // xr_hud_head_width_m [0.3, 10] (default 1.6).
    float xr_hud_quad_distance_m() const noexcept { return xr_hud_quad_distance_m_; }
    float xr_hud_scale() const noexcept { return xr_hud_scale_; }
    float xr_hud_head_distance_m() const noexcept { return xr_hud_head_distance_m_; }
    float xr_hud_head_width_m() const noexcept { return xr_hud_head_width_m_; }
    bool xr_hud_placement_set() const noexcept { return xr_hud_quad_distance_set_||xr_hud_scale_set_||xr_hud_head_distance_set_||xr_hud_head_width_set_; }
    bool xr_hud_quad_distance_set() const noexcept { return xr_hud_quad_distance_set_; }
    // J-world-in-projection: composite the world image into the per-eye projection images under the cockpit
    // instead of submitting a world quad layer; default OFF; effective only with xr_stereo_output.
    bool xr_world_in_projection() const noexcept { return xr_world_in_projection_; }
    // J-world-hold / J-world-extend: keep compositing the last good world when a frame has none (750 ms, fading) / fill the periphery
    // outside the world window; both default OFF; effective only with xr_world_in_projection (and so xr_stereo_output).
    bool xr_world_hold_last() const noexcept { return xr_world_hold_last_; }
    bool xr_world_extend() const noexcept { return xr_world_extend_; }
    // J-world-cache (periphery fill v2): a world-registered environment cache; the eye pixels outside the world window sample it (age-faded to a
    // horizon-fit sky/ground fill) instead of the extend smear. Default OFF; effective only with xr_world_in_projection; replaces xr_world_extend.
    bool xr_world_cache() const noexcept { return xr_world_cache_; }
    // J-terrain-render (docs/world-render/terrain-render-impl.md; both default OFF/empty): xr_terrain_render draws the baked stage terrain (stage_baker.py .zvrs) as the base
    // world layer of the in-projection composite; xr_terrain_stage = "<id>[;option=value...]" (id [A-Za-z0-9_]{1,8}; options use ":" (a config line has only one "="), validated by terrain_stage.cpp: yaw: sgn: mirror:
    // blend: alt: fixy: upm: fog0: fog1: sea: cam:). Effective only with xr_world_in_projection AND the E6 camera (ee_camera_probe) -- or a cam= fixed camera.
    // The id 'auto' is RESERVED (STAGES, docs/world-render/stages-and-detection.md): the stage is detected at consumer start from the game's loaded stage files by a
    // read-only memory scan against %LOCALAPPDATA%\acz-vr\stage\fingerprints.json; no match -> no terrain (logged). No extra key.
    bool xr_terrain_render() const noexcept { return xr_terrain_render_; }
    const char* xr_terrain_stage() const noexcept { return xr_terrain_stage_; }
    bool xr_terrain_preset_vrn() const noexcept { return xr_terrain_preset_vrn_; } // D-400: the stored value came from a vrn:1 expansion
    // COCKPIT-2 (docs/world-render/cockpit-render-impl.md; default empty = OFF): xr_cockpit_model = "<model id>[;option:value...]" (id = the .zvrm file stem in
    // %LOCALAPPDATA%\acz-vr\models, [A-Za-z0-9_]{1,24}; options validated by cockpit_model.cpp: upm: gain: amb: cut: near: ext: glass: hudglass: replay: hide: show: eye:).
    // ONE key (no separate bool: a non-empty value enables it). Effective only with xr_world_in_projection.
    const char* xr_cockpit_model() const noexcept { return xr_cockpit_model_; }
    // J-w1-ee-camera: in-process EE camera input probe; default OFF; effective
    // only with logging, gpu_copy_early_device_hook and the step-3/whole-set/
    // step-5 producer keys. Independent of xr_stereo_output.
    bool ee_camera_probe() const noexcept { return ee_camera_probe_; }
    bool ee_camera_probe_position() const noexcept { return ee_camera_probe_position_; }
    unsigned ee_camera_probe_points() const noexcept { return ee_camera_probe_points_; } // bit0 A, bit1 B, bit2 C
    // Astra (issue 29): writes are separate from observation; default OFF and
    // additionally gated in code (validated block, demonstrated ordering,
    // independent world-path observer).
    bool ee_camera_write_trials() const noexcept { return ee_camera_write_trials_; }
    // J-hud-markers (src/acz-vr/J-hud-markers-findings.md; both default OFF, appended last):
    //  * gpu_copy_step5_hud_markers: producer; the HUD draws classified as target/friendly markers
    //    (depth func GREATER + the next shared-text draw) are replayed into a SECOND private target per
    //    slot (StereoFrame::hud_markers), the rest stays in StereoFrame::hud. Effective only with
    //    gpu_copy_step5_hud (latched with it).
    //  * xr_hud_markers_layer: consumer; an extra swapchain and a marker quad placed exactly like the
    //    world quad (scale forced to 1). Effective only with xr_stereo_output + xr_hud_layer.
    bool gpu_copy_step5_hud_markers() const noexcept { return gpu_copy_step5_hud_markers_; }
    bool xr_hud_markers_layer() const noexcept { return xr_hud_markers_layer_; }
    // Option 3 stage 2 (D-266/D-267, ee_look_write.h; both default OFF, need ee_camera_probe):
    //  * ee_camera_look_write_trial: ONE bounded 120-tick write of the look-yaw float, then restore;
    //  * ee_camera_look_drive: head-driven look; additionally needs a successful stage-2a trial in the same process.
    bool ee_camera_look_write_trial() const noexcept { return ee_camera_look_write_trial_; }
    bool ee_camera_look_drive() const noexcept { return ee_camera_look_drive_; }
    // D-269 follow-up (ee_look_input_trace.h): READ-ONLY bounded trace of the 64 KiB around the look yaw; default OFF, needs ee_camera_probe.
    bool ee_camera_look_input_trace() const noexcept { return ee_camera_look_input_trace_; }
    // Lens/FOV widen plan, PHASE 0 (ee_lens_scan.h, docs/review/lens-scan-impl.md; default OFF): a READ-ONLY, bounded scan of the EE memory image for
    // the lens candidates (tan pair 0.6152/0.3460 and scaled forms). It latches only with logging and ee_camera_probe (its clock, its tracks-look
    // anchor and its logs) and is REFUSED when any key that can write game memory is set (ee_camera_write_trials, ee_camera_look_write_trial,
    // ee_camera_look_drive): a scan run is a pure observation run.
    bool ee_lens_scan() const noexcept { return ee_lens_scan_; }
    bool lens_scan_latch(bool gate) const noexcept {
        return gate && logging_ && ee_camera_probe_ && ee_lens_scan_ &&
               !ee_camera_write_trials_ && !ee_camera_look_write_trial_ && !ee_camera_look_drive_;
    }
    // PS2 address probe (ee_ps2_probe.h, docs/world-render/ps2-probe-impl.md; default OFF): READ-ONLY, bounded reads/scans of the 64 MiB EE-image block at
    // PS2 offsets (mission byte, game id, camera struct signature) for offline correlation. Same latch rules as ee_lens_scan: logging + ee_camera_probe
    // (clock, tracks-look anchor, ee-camera log), refused with any key that can write game memory.
    bool ee_ps2_probe() const noexcept { return ee_ps2_probe_; }
    bool ps2_probe_latch(bool gate) const noexcept {
        return gate && logging_ && ee_camera_probe_ && ee_ps2_probe_ &&
               !ee_camera_write_trials_ && !ee_camera_look_write_trial_ && !ee_camera_look_drive_;
    }
    // Option 3 via OS keyboard input (look_drive_keys.h, J-look-drive-findings.md; both default OFF, appended last):
    //  * xr_look_drive_keys: modulate the NumPad look keys (SendInput, only while the game window is foreground) so the game's own
    //    look follows the head; the per-eye head rotation then excludes the look the game rendered. Effective only with
    //    xr_stereo_output AND gpu_copy_step5_continuous (and the keys they need).
    //  * xr_look_drive_test_sweep: TEST MODE, a scripted head sweep replaces the head (null-HMD validation). Needs xr_look_drive_keys.
    bool xr_look_drive_keys() const noexcept { return xr_look_drive_keys_; }
    bool xr_look_drive_test_sweep() const noexcept { return xr_look_drive_test_sweep_; }
    // XInput route (docs/review/xinput-look-impl.md; both default OFF, appended last, both need xr_look_drive_keys):
    //  * xr_look_drive_xinput: send the look as a RIGHT-STICK offset through the user's XINPUT1_4.dll proxy (skip-intro, look_xinput=1) instead of
    //    NumPad keys; no window-focus dependence. Player 1 must be the CONTROLLER in the launcher. Without the proxy feature nothing happens.
    //  * xr_look_drive_closed_loop: opt in to the measured-look integral/gain correction (the default controller is open loop).
    bool xr_look_drive_xinput() const noexcept { return xr_look_drive_xinput_; }
    bool xr_look_drive_closed_loop() const noexcept { return xr_look_drive_closed_loop_; }
    // POV fixes after headset test 14 (docs/review/pov-fixes-impl.md; all default OFF, appended last; 3 keys):
    //  * xr_look_ref_cluster: the look reference is the previous frame's MAJORITY orientation cluster of the replayed members' M256 instead of the
    //    first member (needs xr_look_drive_keys; latched with it).
    //  * gpu_copy_step5_neck_lever_m = "lever[,scale]": lever in metres [0, 0.30] (0 = off) subtracts the neck-pivot arc R n - R0 n from the head
    //    position (producer setup_poses and the consumer's HUD/world/marker quads); optional scale [0, 1] (default 1) multiplies the remaining
    //    head-centre translation (0 = rotation-only). Latched with gpu_copy_step5_continuous + xr_stereo_output.
    //  * xr_hud_follow 0|1|2: 0 = today's LOCAL HUD; 1 = the HUD quad follows the lean-compensated virtual eye; 2 = additionally turned by a
    //    rate-limited fraction of the rendered look. Latched with xr_stereo_output + xr_hud_layer (world-quad mode).
    bool xr_look_ref_cluster() const noexcept { return xr_look_ref_cluster_; }
    float neck_lever_m() const noexcept { return neck_lever_m_; }
    float translation_scale() const noexcept { return translation_scale_; }
    unsigned xr_hud_follow() const noexcept { return xr_hud_follow_; }
    // J-effects-layer (src/acz-vr/J-effects-layer-findings.md; both default OFF, appended last):
    //  * gpu_copy_step5_effects: producer; non-member scene draws between the cockpit members (missiles, explosions, tracers) are
    //    replayed into a third private target per slot (StereoFrame::effects). Effective only with gpu_copy_step5_continuous AND
    //    gpu_copy_step5_hud (it reuses the HUD replay machinery and its depth-stencil).
    //  * xr_effects_layer: consumer; the effects image is composited over the world INSIDE the world-in-projection pass.
    //    Effective only with xr_stereo_output + xr_world_in_projection.
    bool gpu_copy_step5_effects() const noexcept { return gpu_copy_step5_effects_; }
    // J-effects-dual (docs/review/dual-source-impl.md; default OFF, appended last): producer + representation; the effects layer models the game's
    // dual-source alpha blend (blend 5,6,1/18,19,1): matched draws are replayed with a private PSO clone whose alpha blend is ONE/INV_SRC_ALPHA so the
    // effects image holds (premultiplied RGB, RGB coverage) "coverage_v1", and the consumer composites it with coverage maths. One key serves both
    // sides (the consumer follows the frame's representation tag, never this key). Effective only with gpu_copy_step5_effects (latched with it).
    bool gpu_copy_step5_effects_dual() const noexcept { return gpu_copy_step5_effects_dual_; }
    // J-hud-radar (src/acz-vr/J-hud-radar-findings.md; default OFF, appended last): producer; the HUD region is segmented (a draw on another
    // target is an excursion, the replay resumes when the scene target returns), the radar group (stencil REPLACE quads and EQUAL contact
    // draws) is replayed, and the HUD caps are raised to 256 draws / 960 commands. Effective only with gpu_copy_step5_hud (latched with it).
    bool gpu_copy_step5_hud_radar() const noexcept { return gpu_copy_step5_hud_radar_; }
    bool xr_effects_layer() const noexcept { return xr_effects_layer_; }
    // J-test8-fixes (src/acz-vr/J-test8-fixes-findings.md; all default OFF, appended last):
    //  * xr_world_objects_depth_fix: producer; the replayed world objects (DrawACM_v) write a COMPRESSED depth (record +360 scale 2^-10) so a
    //    cockpit pixel always wins the eye depth test against an object. Latched only with gpu_copy_step5_world_objects.
    //  * xr_png_once: consumer; ONE bounded readback of the final composited eye 0 image (+ its cockpit alpha as grey) as PNG.
    //  * xr_debug_view [0..4]: consumer; 0 = off, 1 = cockpit coverage alpha as grey, 2 = cockpit colour with alpha forced 1,
    //    3 = world layer only, 4 = false colour (red = cockpit alpha 1, green = world layer, blue = effects/other).
    bool xr_world_objects_depth_fix() const noexcept { return xr_world_objects_depth_fix_; }
    // J-occlusion-fix (docs/review/occlusion-fix-impl.md; default OFF, appended last): consumer; inside the world window the replayed world OBJECTS
    // (compressed-depth band of xr_world_objects_depth_fix) are hidden because the world snapshot already holds them correctly occluded; they stay in
    // the periphery. Latched only with xr_stereo_output + xr_world_in_projection + the LATCHED xr_world_objects_depth_fix (else inert).
    bool xr_world_window_occlude() const noexcept { return xr_world_window_occlude_; }
    bool xr_png_once() const noexcept { return xr_png_once_; }
    unsigned xr_debug_view() const noexcept { return xr_debug_view_; }
    // What the two keys latch to (diagnostics sink): `gate` = opt-in and logging. The REAL head-driven mode needs xr_stereo_output AND
    // gpu_copy_step5_continuous. The TEST SWEEP alone (xr_look_drive_test_sweep with xr_look_drive_keys and gpu_copy_step5_continuous) also
    // runs WITHOUT xr_stereo_output: no_xr = the sweep latched with no XR output (the head is the scripted sweep; no pose, no consumer).
    // xr_look_drive_keys without the sweep and without xr_stereo_output latches nothing.
    struct LookDriveLatch { bool keys=false, sweep=false, no_xr=false, xinput=false, closed_loop=false, ref_cluster=false; };
    LookDriveLatch look_drive_latch(bool gate) const noexcept {
        LookDriveLatch l;
        const bool base = gate && logging_ && gpu_copy_step5_continuous_ && xr_look_drive_keys_;
        l.keys = base && (xr_stereo_output_ || xr_look_drive_test_sweep_);
        l.sweep = l.keys && xr_look_drive_test_sweep_;
        l.no_xr = l.sweep && !xr_stereo_output_;
        l.xinput = l.keys && xr_look_drive_xinput_;
        l.closed_loop = l.keys && xr_look_drive_closed_loop_;
        l.ref_cluster = l.keys && xr_look_ref_cluster_;
        return l;
    }
    // FPS investigation (docs/world-render/fps-investigation.md; default OFF): the LEAN production profile. Cuts DIAGNOSTIC LOG volume only
    // (ee-camera per-frame obs/timeline/activity/region-scan/hexdump lines, per-Execute l3-link lines, l3-counts/l3-repeats at most once per
    // second instead of every Execute). No observation, latch, replay, pin or render path reads it. Latched with logging.
    bool perf_lean() const noexcept { return perf_lean_; }
    bool ownership_pins() const noexcept { return ownership_pins_; }
    bool logging() const noexcept { return logging_; }
    unsigned verbosity() const noexcept { return verbosity_; }
    bool logging_explicit() const noexcept { return logging_explicit_; }
    constexpr bool capture_enabled() const noexcept { return false; }
private:
    bool gpu_copy_witness_observation_=false;
    bool gpu_copy_step1_experiment_=false;
    bool gpu_copy_early_device_hook_=false;
    bool gpu_copy_step2_experiment_=false;
    bool gpu_copy_step3_experiment_=false;
    bool gpu_copy_step3_whole_set_=false;
    bool gpu_copy_step5_hud_observe_=false;
    bool gpu_copy_step5_hud_=false;
    bool gpu_copy_step5_continuous_=false;
    bool gpu_copy_step5_hmd_fov_=false,gpu_copy_step5_precockpit_observe_=false,gpu_copy_step5_members_by_class_=false;
    bool gpu_copy_step5_world_objects_=false,gpu_copy_step5_world_observe_=false,gpu_copy_step5_phase_observe_=false;
    bool gpu_copy_step5_png_once_=false;
    float gpu_copy_step5_units_per_metre_=39.6f;
    bool xr_stereo_output_=false;
    bool xr_hud_layer_=false;
    unsigned hud_mode_=0;
    float xr_edge_feather_=0.0f,xr_world_quad_distance_m_=40.0f;
    bool xr_edge_feather_set_=false,xr_world_quad_distance_set_=false;
    float xr_hud_quad_distance_m_=0.0f,xr_hud_scale_=1.0f,xr_hud_head_distance_m_=2.5f,xr_hud_head_width_m_=1.6f;
    bool xr_hud_quad_distance_set_=false,xr_hud_scale_set_=false,xr_hud_head_distance_set_=false,xr_hud_head_width_set_=false;
    bool ee_camera_probe_=false,ee_camera_probe_position_=false,ee_camera_write_trials_=false;
    bool gpu_copy_step5_hud_markers_=false,xr_hud_markers_layer_=false;
    bool xr_world_in_projection_=false,xr_world_hold_last_=false,xr_world_extend_=false,xr_world_cache_=false;
    bool xr_terrain_render_=false,xr_terrain_preset_vrn_=false;char xr_terrain_stage_[xr_terrain_stage_capacity]={};
    char xr_cockpit_model_[192]={}; // COCKPIT-2 (was 100: the step169 stwatch+sttrack line overflowed it and refused the whole config)
    unsigned ee_camera_probe_points_=7;
    bool ee_camera_look_write_trial_=false,ee_camera_look_drive_=false;
    bool ee_camera_look_input_trace_=false;
    bool ee_lens_scan_=false;
    bool ee_ps2_probe_=false;
    bool xr_look_drive_keys_=false,xr_look_drive_test_sweep_=false,xr_look_drive_xinput_=false,xr_look_drive_closed_loop_=false;
    bool xr_look_ref_cluster_=false;float neck_lever_m_=0.0f,translation_scale_=1.0f;unsigned xr_hud_follow_=0;
    bool gpu_copy_step5_effects_=false,xr_effects_layer_=false,gpu_copy_step5_effects_dual_=false;
    bool gpu_copy_step5_hud_radar_=false;
    bool xr_world_objects_depth_fix_=false,xr_world_window_occlude_=false,xr_png_once_=false;unsigned xr_debug_view_=0;
    bool ownership_pins_=false;
    bool perf_lean_=false;
    bool logging_=true;
    bool logging_explicit_=false;
    unsigned verbosity_=1;
    friend struct ConfigParser;
};
enum class ConfigIssue { none, missing, unreadable, oversized, malformed, duplicate,
                         unknown_key, out_of_range, capture_enable_rejected, exception };
struct ConfigResult {
    Config config;
    ConfigIssue issue=ConfigIssue::none;
    size_t unknown_keys=0;
    const char* reason="logging-only configuration"; // process-owned static text
};
enum class ReadState { ok, missing, unreadable, oversized };
struct ReadResult { ReadState state=ReadState::missing; size_t size=0; };
struct ConfigReader {
    virtual ~ConfigReader()=default;
    // Must report oversized when input does not fit; never silently truncate.
    virtual ReadResult read_config(const std::filesystem::path&,char*,size_t)=0;
};
ConfigResult parse_config(std::string_view) noexcept;
ConfigResult load_config(ConfigReader&,const std::filesystem::path&) noexcept;
}
