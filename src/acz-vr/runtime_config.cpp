#include "runtime_config.h"
#include <array>
#include <string>
#include "terrain_preset.h"

namespace acz::diagnostics {
namespace {
ConfigResult rejected(ConfigIssue issue,const char* reason,size_t unknown=0) noexcept {
    ConfigResult r;r.issue=issue;r.reason=reason;r.unknown_keys=unknown;return r;
}
std::string_view trim(std::string_view v) noexcept {
    while(!v.empty()&&(v.front()==' '||v.front()=='\t'||v.front()=='\r'))v.remove_prefix(1);
    while(!v.empty()&&(v.back()==' '||v.back()=='\t'||v.back()=='\r'))v.remove_suffix(1);
    return v;
}
// J-feather: unsigned decimal "D[.D]" (at most 12 characters, no sign/exponent/locale).
bool decimal(std::string_view v,double& out) noexcept {
    if(v.empty()||v.size()>12)return false;
    double whole=0,scale=0;bool digits=false,dot=false;
    for(char c:v) {
        if(c=='.'){if(dot)return false;dot=true;scale=1;continue;}
        if(c<'0'||c>'9')return false;
        digits=true;
        if(dot){scale/=10;whole+=(c-'0')*scale;}else whole=whole*10+(c-'0');
    }
    if(!digits||v.back()=='.'||v.front()=='.')return false;
    out=whole;return true;
}
}
struct ConfigParser {
static ConfigResult parse(std::string_view bytes) noexcept {
    if(bytes.size()>max_config_bytes)return rejected(ConfigIssue::oversized,"config oversized");
    ConfigResult out;
    std::array<std::string_view,max_config_keys> keys{};size_t count=0;
    while(!bytes.empty()) {
        auto end=bytes.find('\n');auto line=bytes.substr(0,end);
        if(end==bytes.npos)bytes={};else bytes.remove_prefix(end+1);
        if(line.size()>max_config_line)return rejected(ConfigIssue::oversized,"config line oversized");
        for(unsigned char c:line)if(c<32&&c!='\r'&&c!='\t')return rejected(ConfigIssue::malformed,"config control character");
        line=trim(line);if(line.empty())continue;
        auto eq=line.find('=');
        if(eq==line.npos||line.find('=',eq+1)!=line.npos)return rejected(ConfigIssue::malformed,"config malformed");
        auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1));
        if(key.empty()||value.empty())return rejected(ConfigIssue::malformed,"config malformed");
        if(count==keys.size())return rejected(ConfigIssue::oversized,"config too many keys");
        for(size_t i=0;i<count;++i)if(keys[i]==key)return rejected(ConfigIssue::duplicate,"config duplicate key");
        keys[count++]=key;
        if(key=="capture"||key=="capture_enabled"||key=="native_capture"||key=="enable_capture"||
           key=="gpu_copy"||key=="xr_submission") {
            if(value!="disabled"&&value!="false"&&value!="0")
                return rejected(ConfigIssue::capture_enable_rejected,"config capture enable rejected");
        } else if(key=="log_enabled") {
            out.config.logging_explicit_=true;
            if(value=="true"||value=="1")out.config.logging_=true;
            else if(value=="false"||value=="0")out.config.logging_=false;
            else return rejected(ConfigIssue::out_of_range,"config log_enabled out of range");
        } else if(key=="diagnostic_ownership_pins") {
            if(value=="true"||value=="1")out.config.ownership_pins_=true;
            else if(value=="false"||value=="0")out.config.ownership_pins_=false;
            else return rejected(ConfigIssue::out_of_range,"config ownership pins out of range");
        } else if(key=="gpu_copy_witness_observation") {
            if(value=="true"||value=="1")out.config.gpu_copy_witness_observation_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_witness_observation_=false;
            else return rejected(ConfigIssue::out_of_range,"config witness observation out of range");
        } else if(key=="gpu_copy_step1_experiment") {
            if(value=="true"||value=="1")out.config.gpu_copy_step1_experiment_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step1_experiment_=false;
            else return rejected(ConfigIssue::out_of_range,"config step1 experiment out of range");
        } else if(key=="gpu_copy_early_device_hook") {
            if(value=="true"||value=="1")out.config.gpu_copy_early_device_hook_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_early_device_hook_=false;
            else return rejected(ConfigIssue::out_of_range,"config early device hook out of range");
        } else if(key=="gpu_copy_step2_experiment") {
            if(value=="true"||value=="1")out.config.gpu_copy_step2_experiment_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step2_experiment_=false;
            else return rejected(ConfigIssue::out_of_range,"config step2 experiment out of range");
        } else if(key=="gpu_copy_step3_experiment") {
            if(value=="true"||value=="1")out.config.gpu_copy_step3_experiment_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step3_experiment_=false;
            else return rejected(ConfigIssue::out_of_range,"config step3 experiment out of range");
        } else if(key=="gpu_copy_step3_whole_set") {
            if(value=="true"||value=="1")out.config.gpu_copy_step3_whole_set_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step3_whole_set_=false;
            else return rejected(ConfigIssue::out_of_range,"config step3 whole set out of range");
        } else if(key=="gpu_copy_step5_hud_observe") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_hud_observe_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_hud_observe_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 hud observe out of range");
        } else if(key=="gpu_copy_step5_hud") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_hud_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_hud_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 hud out of range");
        } else if(key=="gpu_copy_step5_continuous") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_continuous_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_continuous_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 continuous out of range");
        } else if(key=="gpu_copy_step5_hmd_fov") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_hmd_fov_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_hmd_fov_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 hmd fov out of range");
        } else if(key=="gpu_copy_step5_precockpit_observe") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_precockpit_observe_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_precockpit_observe_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 precockpit observe out of range");
        } else if(key=="gpu_copy_step5_members_by_class") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_members_by_class_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_members_by_class_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 members by class out of range");
        } else if(key=="gpu_copy_step5_world_objects") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_world_objects_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_world_objects_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 world objects out of range");
        } else if(key=="gpu_copy_step5_world_observe") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_world_observe_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_world_observe_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 world observe out of range");
        } else if(key=="gpu_copy_step5_phase_observe") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_phase_observe_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_phase_observe_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 phase observe out of range");
        } else if(key=="gpu_copy_step5_png_once") {
            if(value=="true"||value=="1")out.config.gpu_copy_step5_png_once_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_png_once_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 png once out of range");
        } else if(key=="xr_stereo_output") {
            if(value=="true"||value=="1")out.config.xr_stereo_output_=true;
            else if(value=="false"||value=="0")out.config.xr_stereo_output_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr stereo output out of range");
        } else if(key=="xr_hud_layer") {
            if(value=="true"||value=="1")out.config.xr_hud_layer_=true;
            else if(value=="false"||value=="0")out.config.xr_hud_layer_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr hud layer out of range");
        } else if(key=="hud_mode") {
            if(value=="world-quad")out.config.hud_mode_=0;
            else if(value=="head-locked")out.config.hud_mode_=1;
            else return rejected(ConfigIssue::out_of_range,"config hud mode out of range");
        } else if(key=="gpu_copy_step5_units_per_metre") {
            double u=0;
            if(!decimal(value,u))return rejected(ConfigIssue::out_of_range,"config units per metre out of range");
            out.config.gpu_copy_step5_units_per_metre_=float(u<20?20:u>100?100:u);
        } else if(key=="xr_edge_feather") {
            double f=0;
            if(!decimal(value,f))return rejected(ConfigIssue::out_of_range,"config xr edge feather out of range");
            out.config.xr_edge_feather_=float(f>0.25?0.25:f);out.config.xr_edge_feather_set_=true;
        } else if(key=="xr_world_quad_distance_m") {
            double d=0;
            if(!decimal(value,d))return rejected(ConfigIssue::out_of_range,"config xr world quad distance out of range");
            out.config.xr_world_quad_distance_m_=float(d<5?5:d>2000?2000:d);out.config.xr_world_quad_distance_set_=true;
        } else if(key=="xr_hud_quad_distance_m") { // J-hud-placement
            double d=0;
            if(!decimal(value,d))return rejected(ConfigIssue::out_of_range,"config xr hud quad distance out of range");
            out.config.xr_hud_quad_distance_m_=float(d<0.5?0.5:d>2000?2000:d);out.config.xr_hud_quad_distance_set_=true;
        } else if(key=="xr_hud_scale") {
            double s=0;
            if(!decimal(value,s))return rejected(ConfigIssue::out_of_range,"config xr hud scale out of range");
            out.config.xr_hud_scale_=float(s<0.25?0.25:s>2?2:s);out.config.xr_hud_scale_set_=true;
        } else if(key=="xr_hud_head_distance_m") {
            double d=0;
            if(!decimal(value,d))return rejected(ConfigIssue::out_of_range,"config xr hud head distance out of range");
            out.config.xr_hud_head_distance_m_=float(d<0.5?0.5:d>20?20:d);out.config.xr_hud_head_distance_set_=true;
        } else if(key=="xr_hud_head_width_m") {
            double w=0;
            if(!decimal(value,w))return rejected(ConfigIssue::out_of_range,"config xr hud head width out of range");
            out.config.xr_hud_head_width_m_=float(w<0.3?0.3:w>10?10:w);out.config.xr_hud_head_width_set_=true;
        } else if(key=="gpu_copy_step5_hud_markers") { // J-hud-markers
            if(value=="true"||value=="1")out.config.gpu_copy_step5_hud_markers_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_hud_markers_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 hud markers out of range");
        } else if(key=="xr_hud_markers_layer") {
            if(value=="true"||value=="1")out.config.xr_hud_markers_layer_=true;
            else if(value=="false"||value=="0")out.config.xr_hud_markers_layer_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr hud markers layer out of range");
        } else if(key=="xr_world_in_projection") { // J-world-in-projection
            if(value=="true"||value=="1")out.config.xr_world_in_projection_=true;
            else if(value=="false"||value=="0")out.config.xr_world_in_projection_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world in projection out of range");
        } else if(key=="xr_world_hold_last") { // J-world-hold
            if(value=="true"||value=="1")out.config.xr_world_hold_last_=true;
            else if(value=="false"||value=="0")out.config.xr_world_hold_last_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world hold last out of range");
        } else if(key=="xr_world_extend") { // J-world-extend
            if(value=="true"||value=="1")out.config.xr_world_extend_=true;
            else if(value=="false"||value=="0")out.config.xr_world_extend_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world extend out of range");
        } else if(key=="xr_world_cache") { // J-world-cache
            if(value=="true"||value=="1")out.config.xr_world_cache_=true;
            else if(value=="false"||value=="0")out.config.xr_world_cache_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world cache out of range");
        } else if(key=="ee_camera_probe") {
            if(value=="true"||value=="1")out.config.ee_camera_probe_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_probe_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera probe out of range");
        } else if(key=="ee_camera_probe_position") {
            if(value=="true"||value=="1")out.config.ee_camera_probe_position_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_probe_position_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera probe position out of range");
        } else if(key=="ee_camera_write_trials") {
            if(value=="true"||value=="1")out.config.ee_camera_write_trials_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_write_trials_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera write trials out of range");
        } else if(key=="ee_camera_probe_points") {
            // Hook points to trial (always run in A,B,C order): 1-3 distinct letters.
            unsigned mask=0;
            if(value.size()>3)return rejected(ConfigIssue::out_of_range,"config ee camera probe points out of range");
            for(char c:value) {
                const unsigned bit=c=='A'?1u:c=='B'?2u:c=='C'?4u:0u;
                if(!bit||(mask&bit))return rejected(ConfigIssue::out_of_range,"config ee camera probe points out of range");
                mask|=bit;
            }
            out.config.ee_camera_probe_points_=mask;
        } else if(key=="ee_camera_look_write_trial") {
            if(value=="true"||value=="1")out.config.ee_camera_look_write_trial_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_look_write_trial_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera look write trial out of range");
        } else if(key=="ee_camera_look_drive") {
            if(value=="true"||value=="1")out.config.ee_camera_look_drive_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_look_drive_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera look drive out of range");
        } else if(key=="ee_camera_look_input_trace") {
            if(value=="true"||value=="1")out.config.ee_camera_look_input_trace_=true;
            else if(value=="false"||value=="0")out.config.ee_camera_look_input_trace_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee camera look input trace out of range");
        } else if(key=="ee_lens_scan") { // lens/FOV widen plan phase 0 (read-only scan)
            if(value=="true"||value=="1")out.config.ee_lens_scan_=true;
            else if(value=="false"||value=="0")out.config.ee_lens_scan_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee lens scan out of range");
        } else if(key=="xr_terrain_render") { // J-terrain-render
            if(value=="true"||value=="1")out.config.xr_terrain_render_=true;
            else if(value=="false"||value=="0")out.config.xr_terrain_render_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr terrain render out of range");
        } else if(key=="xr_terrain_stage") { // J-terrain-render: "<id>[;opt=val...]", charset-checked here, semantically in terrain_stage.cpp
            if(value.size()>=sizeof out.config.xr_terrain_stage_)return rejected(ConfigIssue::out_of_range,"config xr terrain stage out of range");
            for(char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c==';'||c==':'||c==','||c=='.'||c=='-'))
                return rejected(ConfigIssue::out_of_range,"config xr terrain stage out of range");
            // PRESET (D-400, terrain_preset.h): vrn:1 is expanded here, once, so every consumer of xr_terrain_stage() sees the long form.
            std::string expanded,why;bool applied=false;
            if(!acz::preset::expand(value,expanded,applied,why))return rejected(ConfigIssue::out_of_range,"config xr terrain stage preset unknown");
            if(expanded.size()>=sizeof out.config.xr_terrain_stage_)return rejected(ConfigIssue::out_of_range,"config xr terrain stage expanded out of range");
            for(size_t i=0;i<expanded.size();++i)out.config.xr_terrain_stage_[i]=expanded[i];
            out.config.xr_terrain_stage_[expanded.size()]=0;
            out.config.xr_terrain_preset_vrn_=applied;
        } else if(key=="xr_cockpit_model") { // COCKPIT-2: "<id>[;opt:val...]", charset-checked here, semantically in cockpit_model.cpp; "off" = disabled
            if(value.size()>=sizeof out.config.xr_cockpit_model_)return rejected(ConfigIssue::out_of_range,"config xr cockpit model out of range");
            for(char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c==';'||c==':'||c==','||c=='.'||c=='-'))
                return rejected(ConfigIssue::out_of_range,"config xr cockpit model out of range");
            const bool off=value=="off"||value=="false"||value=="0";
            for(size_t i=0;i<value.size();++i)out.config.xr_cockpit_model_[i]=off?0:value[i];
            out.config.xr_cockpit_model_[off?0:value.size()]=0;
        } else if(key=="ee_ps2_probe") { // PS2 address probe (read-only; docs/world-render/ps2-probe-impl.md)
            if(value=="true"||value=="1")out.config.ee_ps2_probe_=true;
            else if(value=="false"||value=="0")out.config.ee_ps2_probe_=false;
            else return rejected(ConfigIssue::out_of_range,"config ee ps2 probe out of range");
        } else if(key=="xr_look_drive_keys") { // option 3 via keyboard input
            if(value=="true"||value=="1")out.config.xr_look_drive_keys_=true;
            else if(value=="false"||value=="0")out.config.xr_look_drive_keys_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr look drive keys out of range");
        } else if(key=="xr_look_drive_test_sweep") {
            if(value=="true"||value=="1")out.config.xr_look_drive_test_sweep_=true;
            else if(value=="false"||value=="0")out.config.xr_look_drive_test_sweep_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr look drive test sweep out of range");
        } else if(key=="xr_look_drive_xinput") { // look as right-stick input through the XInput proxy
            if(value=="true"||value=="1")out.config.xr_look_drive_xinput_=true;
            else if(value=="false"||value=="0")out.config.xr_look_drive_xinput_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr look drive xinput out of range");
        } else if(key=="xr_look_drive_closed_loop") { // opt-in measured-look correction
            if(value=="true"||value=="1")out.config.xr_look_drive_closed_loop_=true;
            else if(value=="false"||value=="0")out.config.xr_look_drive_closed_loop_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr look drive closed loop out of range");
        } else if(key=="gpu_copy_step5_hud_radar") { // J-hud-radar
            if(value=="true"||value=="1")out.config.gpu_copy_step5_hud_radar_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_hud_radar_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 hud radar out of range");
        } else if(key=="gpu_copy_step5_effects") { // J-effects-layer
            if(value=="true"||value=="1")out.config.gpu_copy_step5_effects_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_effects_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 effects out of range");
        } else if(key=="gpu_copy_step5_effects_dual") { // J-effects-dual
            if(value=="true"||value=="1")out.config.gpu_copy_step5_effects_dual_=true;
            else if(value=="false"||value=="0")out.config.gpu_copy_step5_effects_dual_=false;
            else return rejected(ConfigIssue::out_of_range,"config step5 effects dual out of range");
        } else if(key=="xr_effects_layer") {
            if(value=="true"||value=="1")out.config.xr_effects_layer_=true;
            else if(value=="false"||value=="0")out.config.xr_effects_layer_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr effects layer out of range");
        } else if(key=="xr_world_objects_depth_fix") { // J-test8-fixes
            if(value=="true"||value=="1")out.config.xr_world_objects_depth_fix_=true;
            else if(value=="false"||value=="0")out.config.xr_world_objects_depth_fix_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world objects depth fix out of range");
        } else if(key=="xr_world_window_occlude") { // J-occlusion-fix
            if(value=="true"||value=="1")out.config.xr_world_window_occlude_=true;
            else if(value=="false"||value=="0")out.config.xr_world_window_occlude_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr world window occlude out of range");
        } else if(key=="xr_look_ref_cluster") { // POV fixes: F-REF
            if(value=="true"||value=="1")out.config.xr_look_ref_cluster_=true;
            else if(value=="false"||value=="0")out.config.xr_look_ref_cluster_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr look ref cluster out of range");
        } else if(key=="gpu_copy_step5_neck_lever_m") { // POV fixes: F-LEAN "lever[,scale]"
            const auto comma=value.find(',');
            double lv=0,sc=1;
            if(!decimal(value.substr(0,comma),lv))return rejected(ConfigIssue::out_of_range,"config neck lever out of range");
            if(comma!=value.npos&&!decimal(value.substr(comma+1),sc))return rejected(ConfigIssue::out_of_range,"config translation scale out of range");
            out.config.neck_lever_m_=float(lv>0.30?0.30:lv);out.config.translation_scale_=float(sc>1.0?1.0:sc);
        } else if(key=="xr_hud_follow") { // POV fixes: F-HUD
            if(value.size()!=1||value[0]<'0'||value[0]>'2')return rejected(ConfigIssue::out_of_range,"config xr hud follow out of range");
            out.config.xr_hud_follow_=unsigned(value[0]-'0');
        } else if(key=="xr_png_once") {
            if(value=="true"||value=="1")out.config.xr_png_once_=true;
            else if(value=="false"||value=="0")out.config.xr_png_once_=false;
            else return rejected(ConfigIssue::out_of_range,"config xr png once out of range");
        } else if(key=="xr_debug_view") {
            if(value.size()!=1||value[0]<'0'||value[0]>'4')return rejected(ConfigIssue::out_of_range,"config xr debug view out of range");
            out.config.xr_debug_view_=unsigned(value[0]-'0');
        } else if(key=="perf_lean") { // FPS investigation (docs/world-render/fps-investigation.md): diagnostics-only volume cuts
            if(value=="true"||value=="1")out.config.perf_lean_=true;
            else if(value=="false"||value=="0")out.config.perf_lean_=false;
            else return rejected(ConfigIssue::out_of_range,"config perf lean out of range");
        } else if(key=="verbosity") {
            if(value.size()!=1||value[0]<'0'||value[0]>'2')return rejected(ConfigIssue::out_of_range,"config verbosity out of range");
            out.config.verbosity_=unsigned(value[0]-'0');
        } else ++out.unknown_keys;
    }
    // Unknown keys are ignored and counted, but invalidate all tuning. This
    // prevents a misspelled restriction from publishing a partial configuration.
    if(out.unknown_keys)return rejected(ConfigIssue::unknown_key,"config unknown key",out.unknown_keys);
    return out;
}};
ConfigResult parse_config(std::string_view bytes) noexcept {return ConfigParser::parse(bytes);}
ConfigResult load_config(ConfigReader& reader,const std::filesystem::path& path) noexcept {
    try {
        std::array<char,max_config_bytes> data{};
        auto r=reader.read_config(path,data.data(),data.size());
        if(r.state==ReadState::missing)return rejected(ConfigIssue::missing,"config missing");
        if(r.state==ReadState::unreadable)return rejected(ConfigIssue::unreadable,"config unreadable");
        if(r.state==ReadState::oversized||r.size>data.size())return rejected(ConfigIssue::oversized,"config oversized");
        return parse_config({data.data(),r.size});
    } catch(...) {return rejected(ConfigIssue::exception,"config allocation/exception failure");}
}
}
