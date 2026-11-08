#pragma once
// J-3b7: GPU-copy "Step 3" - per-eye replays of the matched DrawCPT draw into
// proxy-owned render targets with substituted SrtData. SDK-free pieces shared
// by the hook adapter (gpu_copy_step3_hooks.inl) and its offline fixtures.
// Default OFF: nothing here runs unless gpu_copy_step3_experiment plus every
// opt-in is latched (log_enabled, diagnostic_ownership_pins,
// gpu_copy_witness_observation, gpu_copy_early_device_hook).
//
// Step 3 reuses ALL step-1 machinery (owner registry, timeline, witness,
// ledger, Draw/Execute/Close/Reset seams, worker, disposition). Differences:
//  - two-stage arming: the first qualified matched draw is only OBSERVED (its
//    shape is recorded; nothing is recorded into the game's list). The owner
//    context then creates the private targets sized/formatted from that shape
//    and writes the substituted SrtData once. The single attempt is consumed
//    at the next qualified draw whose shape equals the observed one.
//  - the recipe: four replays (original, zero-offset control, left, right) of
//    the same draw into ONE private RT/DS pair, each copied to its own
//    footprint of a private READBACK buffer, then the game's OM targets and t10
//    root SRV are restored, then the original Draw is forwarded unchanged.
//  - worker: per-replay pixel statistics, control equality, disparity, PNG.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
// cockpit_math.h uses std::min/std::max and #undefs near/far; windows.h may
// already define all four macros (FAR in later SDK headers expands to far).
#pragma push_macro("min")
#pragma push_macro("max")
#pragma push_macro("near")
#pragma push_macro("far")
#undef min
#undef max
#include "cockpit_math.h"
#pragma pop_macro("far")
#pragma pop_macro("near")
#pragma pop_macro("max")
#pragma pop_macro("min")

namespace acz::gpu_copy::step3 {
// ---------------------------------------------------------------- replays
// Recording order. `original` keeps the game's own t10 binding (the exact
// draw, different target); control/left/right bind proxy blobs 0/1/2.
enum Replay : unsigned {replay_original=0,replay_control=1,replay_left=2,replay_right=3};
constexpr unsigned replay_count=4,blob_count=3;
constexpr const char* replay_names[replay_count]={"original","control","left","right"};
// Eye sign along the recovered camera right axis (control is the zero offset).
constexpr double blob_sign[blob_count]={0.0,-1.0,1.0};
// ---------------------------------------------------------------- constants
// IPD 0.063 m: the brief's value and the located IPD of the D-210 null-HMD run.
constexpr double ipd_metres=0.063;
// 39.6 DrawCPT world units per metre: E9 section 3 "Scale (assumption)" - the
// main panel taken at 0.75 m gives ~39.6 units/m (cockpit geometry 18-34 units
// in front of the eye, median 29.7). Metres per unit remain UNMEASURED; this is
// an assumption with documented provenance, not a calibration.
constexpr double units_per_metre=39.6;
constexpr double half_ipd_units=ipd_metres*0.5*units_per_metre; // 1.2474 units
// E9 cockpit depth band (world units) used only for the logged disparity estimate.
constexpr double depth_near_units=18.0,depth_median_units=29.7,depth_far_units=34.0;
// ---------------------------------------------------------------- caps
// J-3b7b: the live cockpit target is 2560x1440 R16G16B16A16_FLOAT with a
// D32_FLOAT_S8X24_UINT depth target (D-220). Both private targets are created at
// the SAME size (28.1 MiB each); hard ceilings 32 MiB each, refused above.
constexpr uint64_t rt_byte_cap=32ull<<20;     // one colour target, refuses above
constexpr uint64_t ds_byte_cap=32ull<<20;     // one depth target (D32S8X24 is 8 B/px)
// J-3b7c: the FULL target is read back for every replay (2560x1440 FP16 is
// 28.1 MiB per footprint, 112.5 MiB for four); hard ceiling 160 MiB. The crop
// below is DIAGNOSTIC ONLY (logged, never a refusal).
constexpr uint32_t crop_max_dimension=1024,crop_pad=64,crop_max_vertices=32;
constexpr uint64_t readback_byte_cap=160ull<<20; // four pitch-aligned full-target footprints
constexpr uint32_t max_dimension=16384;       // D3D12 Texture2D limit
constexpr unsigned command_cap=40;            // inserted native commands, refuses above
constexpr uint64_t blob_bytes=416,blob_stride=512,upload_bytes=4096;
static_assert(blob_count*blob_stride<=upload_bytes,"three blobs fit the private upload buffer");
constexpr uint64_t row_pitch_alignment=256;   // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
constexpr uint64_t placement_alignment=512;   // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
constexpr uint64_t charge_alignment=65536;    // default resource placement (conservative charge)
constexpr uint64_t private_token=0x53e1,source_token=0x53e2; // opaque ledger tokens
// E11: bytes of SrtData element zero the VS/GS read; only +0 and +64 change.
constexpr const char* srt_read_mask="0-175,192-303,352-367,400-415";
// ---------------------------------------------------------------- numeric D3D12/DXGI values
namespace dxgi {
constexpr uint32_t unknown=0,r16g16b16a16_float=10,d32_s8x24=20,r8g8b8a8=28,r8g8b8a8_srgb=29,d32=40,d24_s8=45,d16=55,
    b8g8r8a8=87,b8g8r8a8_srgb=91;
}
namespace cmp {constexpr uint32_t never=1,less=2,equal=3,less_equal=4,greater=5,not_equal=6,greater_equal=7,always=8;}
inline uint64_t align_up(uint64_t v,uint64_t a) noexcept {return (v+a-1)/a*a;}

// Creation-time facts of the matched PSO (copied from the graphics PSO desc).
struct PsoOutput {
    bool known=false;
    // EFFECTS-OUTSIDE (P) fxadd:1: the blend is EXACTLY the additive 4caf6e2d tuple (RGB SRC_ALPHA/ONE/ADD, alpha SRC1_ALPHA/INV_SRC1_ALPHA/ADD, full write mask) and the
    // private clone (alpha ZERO/ONE) exists. dual_ready stays FALSE for it: the effects class gate admits it only while fxadd is on (default: refused exactly as before).
    // Placed in the padding after `known` so Shape keeps its 448-byte bound (gpu_copy_step3_hooks.inl static_assert).
    bool additive_ready=false;
    uint32_t rt_count=0;std::array<uint32_t,8> rtv{};uint32_t dsv=0,samples=0,quality=0;
    bool depth_enable=false,depth_write=false,stencil=false;
    // HUD-RS-SKIP: RT0's blend factors are EXACTLY colour ONE/ONE and alpha ONE/ZERO (D3D12_BLEND 2,2 / 2,1); with the ops (REV_SUBTRACT / ADD) the observed sun-flare
    // tuple. Set at graphics PSO creation. Sits in the padding after the three bools (the Draw-seam Shape is capped at 448 bytes).
    bool rs_flare_factors=false;
    uint32_t depth_func=0,so_entries=0;
    bool blend0=false,independent_blend=false;
    // HUD-RS-SKIP: render target 0's colour and alpha blend OPs (D3D12_BLEND_OP: 1 ADD, 2 SUBTRACT, 3 REV_SUBTRACT, 4 MIN, 5 MAX; 0 = not recorded).
    // In the padding after the two bools: the Draw-seam Shape is capped at 448 bytes (gpu_copy_step3_hooks.inl static_assert). The HUD replay skips
    // colour SUBTRACT / REV_SUBTRACT draws (gpu_copy_step5.h hud_rs_subtract): over the transparent private HUD image dest - src clamps to black
    // (the sun-flare dark ring, docs/world-render/headset-0-3-4-9-10-analysis.md item 3).
    uint8_t blend_op0=0,blend_op_alpha0=0;
    uint32_t write_mask0=0;
    // J-3b7b: stencil test state (the replays clear stencil to 0; the game's
    // OMSetStencilRef value is untracked and left unchanged).
    uint32_t stencil_read=0,stencil_write=0,front_func=0,back_func=0,front_pass=0;
    // J-effects-layer (appended; only the effects classifier reads them): render target 0's blend factors
    // (D3D12_BLEND values; 14/15/20/21 use the constant blend factor / alpha factor, which the proxy does not
    // track) and whether every stencil op of both faces is KEEP (a stencil-enabled PSO with a non-zero write
    // mask and a non-KEEP op writes the stencil plane). Defaults: no factors, stencil ops KEEP.
    bool blend_constant=false,stencil_ops_keep=true;
    // J-effects-dual (appended; only the effects classifier reads it): the PSO's blend is EXACTLY the game's dual-source alpha tuple
    // (RGB SRC_ALPHA/INV_SRC_ALPHA/ADD, alpha SRC1_ALPHA/INV_SRC1_ALPHA/ADD, full write mask) AND the producer holds the private alpha-blend
    // clone of it (created with the PSO while gpu_copy_step5_effects_dual was on). False for every other PSO.
    bool dual_ready=false;
    // EFFECTS-OUTSIDE (G) fxmem:6 tracer class: the blend is EXACTLY the gun tracer tuple (RGB SRC_ALPHA/ONE/ADD, alpha ZERO/ONE/ADD, full write mask) and the private
    // clone exists (dual_ready is then true too). False for every other PSO. Only the effects class gate reads it (together with PS d34ee153 / VS 534f29ca).
    bool tracer_ready=false;
};
struct ColorFormat {uint32_t bytes=0;bool bgra=false,srgb=false;const char* name="unknown";bool half=false;};
// 8-bit four-channel formats (PNG holds the raw channel bytes) and, J-3b7b,
// R16G16B16A16_FLOAT (PNG holds clamp(value,0,1)*255 rounded: raw linear
// values scaled, no gamma/tone mapping; raw half statistics are logged).
inline bool color_format(uint32_t f,ColorFormat& out) noexcept {
    switch(f) {
    case dxgi::r16g16b16a16_float:out={8,false,false,"R16G16B16A16_FLOAT",true};return true;
    case dxgi::r8g8b8a8:out={4,false,false,"R8G8B8A8_UNORM"};return true;
    case dxgi::r8g8b8a8_srgb:out={4,false,true,"R8G8B8A8_UNORM_SRGB"};return true;
    case dxgi::b8g8r8a8:out={4,true,false,"B8G8R8A8_UNORM"};return true;
    case dxgi::b8g8r8a8_srgb:out={4,true,true,"B8G8R8A8_UNORM_SRGB"};return true;
    default:out={};return false;
    }
}
struct DepthFormat {uint32_t bytes=0;bool stencil=false;const char* name="none";};
inline bool depth_format(uint32_t f,DepthFormat& out) noexcept {
    switch(f) {
    case dxgi::d16:out={2,false,"D16_UNORM"};return true;
    case dxgi::d32:out={4,false,"D32_FLOAT"};return true;
    case dxgi::d24_s8:out={4,true,"D24_UNORM_S8_UINT"};return true;
    case dxgi::d32_s8x24:out={8,true,"D32_FLOAT_S8X24_UINT"};return true;
    default:out={};return false;
    }
}
inline const char* func_name(uint32_t f) noexcept {
    static constexpr const char* n[]={"0","NEVER","LESS","EQUAL","LESS_EQUAL","GREATER","NOT_EQUAL","GREATER_EQUAL","ALWAYS"};
    return f<9?n[f]:"unknown";
}
// The clear value that lets every fragment pass the PSO's own depth test once:
// GREATER/GREATER_EQUAL (E2's PS2 depth, larger is nearer) clear to 0, LESS/
// LESS_EQUAL to 1, ALWAYS/NEVER or depth disabled to 0 (value irrelevant).
// EQUAL/NOT_EQUAL depend on the game's depth contents: refused.
inline const char* depth_clear(const PsoOutput& p,float& value) noexcept {
    value=0.0f;
    if(!p.depth_enable)return nullptr;
    switch(p.depth_func) {
    case cmp::greater:case cmp::greater_equal:case cmp::always:case cmp::never:value=0.0f;return nullptr;
    case cmp::less:case cmp::less_equal:value=1.0f;return nullptr;
    case cmp::equal:case cmp::not_equal:return "depth-func-equality-unsupported";
    default:return "depth-func-unknown";
    }
}
struct Viewport {float x=0,y=0,w=0,h=0,min=0,max=0;};
struct Rect {int32_t l=0,t=0,r=0,b=0;};
// Everything the Draw seam copies (under the hook-state mutex) about the
// qualified draw. Addresses are opaque comparison keys, never dereferenced.
struct Shape {
    PsoOutput pso{};uint64_t pso_route=0;
    bool indexed=false;uint32_t count=0,instances=0,start=0,start_instance=0;int32_t base=0;
    bool tracking_complete=false;const char* tracking_gap=nullptr;
    // J-3b7 RS/OM tracking (default-state known from an observed Reset/ClearState
    // with every variant table hooked; gap = an unrecoverable setter call).
    bool rs_known=false,rs_gap=false;uint32_t viewports=0,scissors=0;Viewport vp0{};Rect sc0{};
    float vp_right=0,vp_bottom=0,vp_left=0,vp_top=0;
    bool om_known=false,om_gap=false,om_set=false,om_single=false,om_dsv=false;uint32_t om_count=0;
    uint32_t t10_parameter=UINT32_MAX;bool t10_table=false;bool t10_vs=false; /* J-180: t10 resolved through the VERTEX-stage binding of a DrawACM_v draw */uint64_t t10_va=0,t10_address=0;
    bool t10_root_value=false; // capture::State root value for t10 is an SRV equal to t10_va
    uint64_t draw_ordinal=0,list=0,generation=0;uint32_t topology=0;
    // J-3b7b diagnostic: the game's bound RTV 0 resolved through the proxy's
    // CreateRenderTargetView tracking and the L3 creation record (never a rule).
    uint64_t game_rtv=0,game_rt=0,game_rt_width=0;uint32_t game_rt_height=0,game_rt_format=0,game_rt_heap=0;
    const char* game_rt_status="unresolved:not-captured";
    // J-3b9 world snapshot facts (resource desc + the RTV's subresource; data only).
    uint32_t game_rt_samples=0,game_rt_flags=0,game_rt_dim=0;bool game_rt_view_sub0=false;
    // J-3b9: OM descriptors unchanged since the bind (restore validity), root signature,
    // and the watched world RT's barrier facts in this list generation.
    bool om_desc_proved=false,world_lifetime_covered=false;uint64_t root_route=0;
    bool world_barriers_tracked=false,world_rt_transition=false,world_enhanced=false,world_inherited_ok=false;uint32_t world_rt_after=0;
    const char* world_barrier_why=nullptr;
    // J-world-hold (diagnostics only): WHY world_rt_transition is false when no transition survived (nullptr: it did); see step3Capture.
    const char* world_entry_sub=nullptr;
    // J-3b7d: the draw's bound IA views (setter tracking), opaque keys only.
    uint64_t vb_va=0,vb_bytes=0,vb_address=0,ib_va=0,ib_bytes=0,ib_address=0;
    uint32_t vb_stride=0,ib_stride=0,strip_cut=0xffffffffu;
};
// ---------------------------------------------------------------- selection (J-3b7d)
// The Draw seam only records fixed-size candidates (a Shape each) into a
// bounded ring; the worker reads each candidate's SrtData/IB/VB bytes from
// the game's mapped UPLOAD memory and evaluates it offline.
constexpr unsigned candidate_ring=64;          // ring slots; a full ring drops and counts, never blocks
constexpr unsigned candidate_cap=400;          // matched draws recorded before selection closes
constexpr unsigned selection_ticks=450;        // or 45 s after the first candidate (100 ms ticks)
constexpr unsigned table_line_cap=96;          // candidate-table lines (first 32 + visible ones)
constexpr uint32_t max_vertices=4096,max_indices=12288,max_vertex_stride=256;
constexpr unsigned stale_rearm_cap=5;          // fresh-content re-arms before REFUSED snapshot-stale
struct Targets {
    uint32_t width=0,height=0;ColorFormat color{};DepthFormat depth{};bool has_depth=false;float depth_clear=0;
    // Crop box (texels of the private RT, right/bottom exclusive), set by finalize().
    uint32_t crop_left=0,crop_top=0,crop_w=0,crop_h=0;
    uint64_t row_pitch=0,footprint=0,footprint_stride=0,readback_bytes=0,rt_bytes=0,ds_bytes=0,charged=0;
    unsigned commands=0;
};
inline unsigned planned_commands(bool depth) noexcept {
    // OMSet + per replay {ClearRTV, [ClearDSV], Draw, Barrier, CopyTextureRegion,
    // Barrier} + one SRV set per proxy blob + restore {SRV, OMSet}.
    return 1+replay_count*(depth?6u:5u)+blob_count+2;
}
// Static refusal (first failing rule) or nullptr; fills `t` on success.
inline const char* decide(const Shape& s,Targets& t) noexcept {
    t=Targets{};
    if(!s.tracking_complete||s.tracking_gap)return "binding-tracking-incomplete";
    if(!s.pso.known)return "pso-output-unknown";
    if(s.pso.samples!=1||s.pso.quality!=0)return "msaa-unsupported";
    if(s.pso.rt_count==0)return "no-render-target";
    if(s.pso.rt_count!=1)return "multiple-render-targets-unsupported";
    if(!color_format(s.pso.rtv[0],t.color))return "rt-format-unsupported";
    if(s.pso.so_entries)return "stream-output-unsupported";
    t.has_depth=s.pso.dsv!=dxgi::unknown;
    if(t.has_depth&&!depth_format(s.pso.dsv,t.depth))return "ds-format-unsupported";
    if(!t.has_depth&&s.pso.depth_enable)return "depth-enabled-without-dsv-format";
    if(const char* why=depth_clear(s.pso,t.depth_clear))return why;
    if(s.om_gap)return "om-state-gap";
    if(!s.om_known)return "om-state-unknown";
    if(!s.om_set)return "om-targets-never-set";
    if(s.om_count<s.pso.rt_count)return "om-rt-count-below-pso";
    if(t.has_depth&&!s.om_dsv)return "om-dsv-unbound";
    if(!s.om_desc_proved)return "om-descriptor-unproved"; // J-3b9: a handle restore would not restore the bind-time view
    if(s.rs_gap)return "rs-state-gap";
    if(!s.rs_known)return "rs-state-unknown";
    if(!s.viewports)return "viewport-unset";
    if(!(s.vp_left>=0.0f)||!(s.vp_top>=0.0f))return "viewport-negative-origin";
    if(!(s.vp_right>=1.0f)||!(s.vp_bottom>=1.0f)||!std::isfinite(s.vp_right)||!std::isfinite(s.vp_bottom))return "viewport-empty";
    if(s.vp_right>float(max_dimension)||s.vp_bottom>float(max_dimension))return "viewport-over-dimension";
    t.width=uint32_t(std::ceil(double(s.vp_right)));t.height=uint32_t(std::ceil(double(s.vp_bottom)));
    t.rt_bytes=uint64_t(t.width)*t.height*t.color.bytes;
    if(t.rt_bytes>rt_byte_cap)return "rt-over-cap";
    t.ds_bytes=t.has_depth?uint64_t(t.width)*t.height*t.depth.bytes:0;
    if(t.ds_bytes>ds_byte_cap)return "ds-over-cap";
    if(s.t10_parameter==UINT32_MAX)return "t10-parameter-unknown";
    if(s.t10_table)return "t10-descriptor-table-unsupported";
    if(!s.t10_va)return "t10-unbound";
    if(!s.t10_root_value)return "t10-trackers-disagree";
    if(!s.count||!s.instances)return "empty-draw";
    t.commands=planned_commands(t.has_depth);
    if(t.commands>command_cap)return "command-count-over-cap";
    return nullptr;
}
// J-3b7b: the readback covers only the crop box. Footprint row pitch aligned
// to 256, placement to 512; four replays. Conservative logical charge: every
// committed resource rounded to 64 KiB, plus one 64 KiB per descriptor heap.

// J-3b7f whole-set mode: `pairs` RT/DS pairs (2: left and right) and `footprints`
// readback footprints; the single-draw mode keeps 1 pair and 4 footprints.
inline const char* finalize(Targets& t,uint32_t left,uint32_t top,uint32_t w,uint32_t h,unsigned footprints=replay_count,
                            unsigned pairs=1,uint64_t upload=upload_bytes) noexcept {
    if(!w||!h||left+w>t.width||top+h>t.height)return "crop-outside-target";
    t.crop_left=left;t.crop_top=top;t.crop_w=w;t.crop_h=h;
    t.row_pitch=align_up(uint64_t(w)*t.color.bytes,row_pitch_alignment);
    t.footprint=t.row_pitch*h;
    t.footprint_stride=align_up(t.footprint,placement_alignment);
    t.readback_bytes=t.footprint_stride*footprints;
    if(t.readback_bytes>readback_byte_cap)return "readback-over-cap";
    t.charged=(align_up(t.rt_bytes,charge_alignment)+align_up(t.ds_bytes,charge_alignment))*pairs+
        align_up(t.readback_bytes,charge_alignment)+align_up(upload,charge_alignment)+
        charge_alignment*(t.has_depth?2u:1u)*pairs;
    return nullptr;
}
// ---------------------------------------------------------------- whole set (J-3b7f)
// One persistent RT/DS pair per eye, cleared once; per member: OMSet(L),
// SRV(L), Draw, OMSet(R), SRV(R), Draw, SRV(original), OMSet(game) = 8; plus
// the one-time clears (2 or 4) and two copies (barrier, CopyTextureRegion,
// barrier each = 6). 64 members -> 522 commands; refused above 600.
constexpr unsigned set_cap=64,set_command_cap=600,set_per_member=8,set_copies=6;
constexpr uint64_t set_upload_bytes=uint64_t(set_cap)*2*blob_stride; // left/right slot per member
inline unsigned set_commands(unsigned members,bool depth) noexcept {return (depth?4u:2u)+set_per_member*members+set_copies;}
// linear -> sRGB (IEC 61966-2-1), clamped to [0,1], 8-bit rounded.
inline uint8_t srgb8(float v) noexcept {
    if(!(v>0.0f))return 0;
    if(v>=1.0f)return 255;
    const double c=v<=0.0031308?12.92*v:1.055*std::pow(double(v),1.0/2.4)-0.055;
    return uint8_t(c*255.0+0.5);
}
// The insertion draw must have the observed shape (the targets were built
// for it). t10 VA and OM handles may differ (they are re-captured for the
// restore); everything that sized/formatted the targets must match.
inline const char* same_shape(const Shape& a,const Shape& b) noexcept {
    if(a.pso_route!=b.pso_route)return "pso-changed";
    if(a.indexed!=b.indexed||a.count!=b.count||a.instances!=b.instances||a.start!=b.start||
       a.base!=b.base||a.start_instance!=b.start_instance)return "draw-args-changed";
    if(a.draw_ordinal!=b.draw_ordinal)return "draw-ordinal-changed";
    if(a.vp_right!=b.vp_right||a.vp_bottom!=b.vp_bottom||a.viewports!=b.viewports)return "viewport-changed";
    if(a.om_count!=b.om_count||a.om_dsv!=b.om_dsv)return "om-shape-changed";
    // J-3b7e: the t10 VA/resource is NOT part of the match: the game's SrtData
    // for the same draw lives in a ring of upload buffers (D-223); the live
    // bytes at the matched draw are what the replays substitute.
    if(a.t10_parameter!=b.t10_parameter)return "t10-parameter-changed";
    return nullptr;
}

// ---------------------------------------------------------------- substitution
// Row-vector float32 matrices at SrtData +0 (local->PS2 screen), +64 (local->
// clip), +256 (local->world, x16 scale), as in cockpit_math.h / E2.
inline bool load_matrix(const uint8_t* p,acz::Mat4& m) noexcept {
    float f[16];std::memcpy(f,p,sizeof f);
    for(int i=0;i<16;++i){if(!std::isfinite(f[i]))return false;m[size_t(i)]=double(f[i]);}
    return true;
}
struct Substitution {
    const char* error=nullptr;
    acz::Mat4 m0{},m64{},m256{},vp{},screen{};
    acz::CameraDecomposition camera{};
    std::array<acz::Mat4f,blob_count> m0f{},m64f{};
    std::array<std::array<uint8_t,size_t(blob_bytes)>,blob_count> blobs{};
    std::array<std::array<double,3>,blob_count> eye{};
    std::array<double,blob_count> eye_clip_residual{};
    double control_m0_rel=0,control_m64_rel=0;
    unsigned control_bytes_differ=0;
    bool untouched_equal=false;
};
inline double max_abs(const acz::Mat4& m) noexcept {double v=0;for(double x:m)v=std::fabs(x)>v?std::fabs(x):v;return v;}
// ---------------------------------------------------------------- delta substitution (J-3b7e)
// With an affine local->world M256 = [R 0; t 1] (row vectors; column 3 =
// (0,0,0,1), as live), the earlier full form
//   M64'_e = M256 * T(-o_e) * inverse(M256) * M64,   M0'_e = M64'_e * inverse(M64) * M0
// reduces EXACTLY to a translation in local space:
//   M256 * T(-o) * inverse(M256) = T(-u),  u = o * R^-1
//   M64'_e = T(-u) * M64  and  M0'_e = T(-u) * M0
// so only ROW 3 (the translation row, bytes 48..63 and 112..127) of the +0 and
// +64 matrices changes: row3' = row3 - u * rows0..2. Here o = sign * half_ipd * right,
// right = normalize(column 0 xyz of VP = inverse(M256) * M64) = normalize(R^-1 * M64[0..2][0]).
// The control (sign 0) is the RAW byte copy; the eyes are the raw bytes with
// those 8 floats replaced. No heap, small fixed stack (Draw-seam safe).
struct DeltaFacts {double right[3]{};double u[blob_count][3]{};double det=0;};
inline float raw_float(const uint8_t* p,unsigned i) noexcept {float f;std::memcpy(&f,p+4*size_t(i),4);return f;}
inline const char* delta_substitute(const uint8_t* raw,std::array<std::array<uint8_t,size_t(blob_bytes)>,blob_count>& blobs,
                                    DeltaFacts& f) noexcept {
    for(unsigned i=0;i<32;++i)if(!std::isfinite(raw_float(raw,i)))return "srt-matrix-nonfinite";
    for(unsigned i=64;i<80;++i)if(!std::isfinite(raw_float(raw,i)))return "srt-matrix-nonfinite";
    const uint8_t* a=raw+256; // M256
    if(raw_float(a,3)!=0.0f||raw_float(a,7)!=0.0f||raw_float(a,11)!=0.0f||raw_float(a,15)!=1.0f)return "m256-not-affine";
    auto A=[&](unsigned r,unsigned c){return double(raw_float(a,r*4+c));};
    const double c00=A(1,1)*A(2,2)-A(1,2)*A(2,1),c01=A(1,2)*A(2,0)-A(1,0)*A(2,2),c02=A(1,0)*A(2,1)-A(1,1)*A(2,0);
    f.det=A(0,0)*c00+A(0,1)*c01+A(0,2)*c02;
    double scale=0;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)scale=std::fabs(A(r,c))>scale?std::fabs(A(r,c)):scale;
    if(!(scale>0)||!(std::fabs(f.det)>1e-12*scale*scale*scale))return "srt-matrix-singular";
    double inv[9];const double id=1.0/f.det;
    inv[0]=c00*id;inv[1]=(A(0,2)*A(2,1)-A(0,1)*A(2,2))*id;inv[2]=(A(0,1)*A(1,2)-A(0,2)*A(1,1))*id;
    inv[3]=c01*id;inv[4]=(A(0,0)*A(2,2)-A(0,2)*A(2,0))*id;inv[5]=(A(0,2)*A(1,0)-A(0,0)*A(1,2))*id;
    inv[6]=c02*id;inv[7]=(A(0,1)*A(2,0)-A(0,0)*A(2,1))*id;inv[8]=(A(0,0)*A(1,1)-A(0,1)*A(1,0))*id;
    const uint8_t* m64=raw+64;const uint8_t* m0=raw;
    double n=0;
    for(unsigned k=0;k<3;++k){f.right[k]=0;for(unsigned j=0;j<3;++j)f.right[k]+=inv[k*3+j]*double(raw_float(m64,j*4));n+=f.right[k]*f.right[k];}
    n=std::sqrt(n);if(!(n>0)||!std::isfinite(n))return "camera-degenerate";
    for(double& r:f.right)r/=n;
    for(unsigned b=0;b<blob_count;++b) {
        std::memcpy(blobs[b].data(),raw,size_t(blob_bytes));
        const double k=blob_sign[b]*half_ipd_units;
        const double o[3]={k*f.right[0],k*f.right[1],k*f.right[2]};
        for(unsigned c=0;c<3;++c)f.u[b][c]=o[0]*inv[0*3+c]+o[1]*inv[1*3+c]+o[2]*inv[2*3+c];
        if(blob_sign[b]==0.0)continue; // control: exactly the raw bytes
        for(unsigned c=0;c<4;++c) {
            double d64=0,d0=0;
            for(unsigned j=0;j<3;++j){d64+=f.u[b][j]*double(raw_float(m64,j*4+c));d0+=f.u[b][j]*double(raw_float(m0,j*4+c));}
            const float n64=float(double(raw_float(m64,12+c))-d64),n0=float(double(raw_float(m0,12+c))-d0);
            if(!std::isfinite(n64)||!std::isfinite(n0))return "substituted-matrix-nonfinite";
            std::memcpy(blobs[b].data()+64+48+4*c,&n64,4);std::memcpy(blobs[b].data()+48+4*c,&n0,4);
        }
    }
    return nullptr;
}
// Worker form (logs and diagnostics): the same delta blobs, plus the camera
// decomposition and checks. Never throws outward.
inline bool substitute(const uint8_t* original,Substitution& out) noexcept {
    out=Substitution{};
    try {
        if(!load_matrix(original,out.m0)||!load_matrix(original+64,out.m64)||!load_matrix(original+256,out.m256)) {
            out.error="srt-matrix-nonfinite";return false;
        }
        DeltaFacts f;
        if(const char* why=delta_substitute(original,out.blobs,f)){out.error=why;return false;}
        out.vp=acz::recoverCameraVP(out.m256,out.m64);
        out.camera=acz::decomposeCameraVP(out.vp,true);
        out.screen=acz::multiply(acz::inverse(out.m64),out.m0);
        out.untouched_equal=true;
        for(unsigned b=0;b<blob_count;++b) {
            std::memcpy(out.m0f[b].data(),out.blobs[b].data(),64);std::memcpy(out.m64f[b].data(),out.blobs[b].data()+64,64);
            out.untouched_equal=out.untouched_equal&&std::memcmp(out.blobs[b].data()+128,original+128,size_t(blob_bytes)-128)==0&&
                std::memcmp(out.blobs[b].data(),original,48)==0&&std::memcmp(out.blobs[b].data()+64,original+64,48)==0;
            const double k=blob_sign[b]*half_ipd_units;
            const double o[3]={k*f.right[0],k*f.right[1],k*f.right[2]};
            const auto vpe=acz::multiply(acz::translation(-o[0],-o[1],-o[2]),out.vp);
            for(int j=0;j<3;++j)out.eye[b][size_t(j)]=out.camera.eye[size_t(j)]+o[j];
            // The eye's own position must map to clip x=y=w=0 under its VP_e.
            const auto c=acz::transformPoint(vpe,{out.eye[b][0],out.eye[b][1],out.eye[b][2]});
            const double scale=max_abs(out.vp)*(1.0+std::fabs(out.eye[b][0])+std::fabs(out.eye[b][1])+std::fabs(out.eye[b][2]));
            out.eye_clip_residual[b]=(std::fabs(c[0])+std::fabs(c[1])+std::fabs(c[3]))/(scale>0?scale:1.0);
        }
        for(size_t i=0;i<size_t(blob_bytes);++i)if(out.blobs[0][i]!=original[i])++out.control_bytes_differ;
        out.control_m0_rel=out.control_m64_rel=0; // the control is the raw copy by construction
        return true;
    } catch(...) {out.error="srt-matrix-singular";return false;}
}
// ---------------------------------------------------------------- projection (J-3b7c, diagnostic only)
// Reproduces the DrawCPT GS (tools/evidence/e11-drawcpt-dxil/drawcpt-gs.dxil.txt):
//  - VS passes ATTRIBUTE0.xyz unchanged as the position (drawcpt-vs lines 104-106,
//    286-288); the GS uses it with w=1.
//  - clip = pos x M(+64); per-vertex outcode bits: w+x<0 (16), w+y<0 (8),
//    w+z<0 (4), w-x<0 (2), w-y<0 (1) (gs lines 218-236). A triangle whose three
//    outcodes share a bit is not emitted (trivial reject; partial triangles go
//    to the GS's own clipper). So the shader's "in front" convention is w>0,
//    as in D3D; w<=0 everywhere means the draw is rejected by the game's GS too.
//  - final position: s = pos x M(+0); ndc = 2*(s.xy/s.w - P.zw)/P.xy with
//    P = SrtData +400 (gs lines 263-299); SV_Position = (ndc*s.w, z, s.w).
//    Screen = viewport-0 mapping of ndc (y down).
// Triangle assembly exactly as the draw consumes its indices: `count` indices
// from `ib` (16/32-bit) or sequential from `start`; base vertex added; strip
// (topology 5) restarts at the cut value, list (4) takes triples. Calls
// `tri(v0,v1,v2)` with absolute vertex numbers; returns false on a bad index.
template<class F> bool assemble(const uint8_t* ib,uint32_t ibStride,uint32_t count,uint32_t start,int32_t base,bool indexed,
                                uint32_t topology,uint32_t cut,F&& tri) noexcept {
    // `cut`: the PSO IBStripCutValue enum (0 disabled, 1 = 0xFFFF, 2 = 0xFFFFFFFF;
    // unknown treated as enabled for the index width in use).
    const bool list=topology==4,restart=cut!=0;uint32_t run[3]{},r=0;uint32_t parity=0;
    for(uint32_t k=0;k<count;++k) {
        uint32_t idx=start+k;
        if(indexed) {
            if(!ib)return false;
            if(ibStride==2){uint16_t x;std::memcpy(&x,ib+2*size_t(k),2);idx=x;if(idx==0xffff&&!list&&restart){r=0;parity=0;continue;}}
            else {uint32_t x;std::memcpy(&x,ib+4*size_t(k),4);idx=x;if(idx==0xffffffffu&&!list&&restart){r=0;parity=0;continue;}}
        }
        const int64_t v=int64_t(idx)+(indexed?base:0);if(v<0)return false;
        if(list){run[r++]=uint32_t(v);if(r==3){tri(run[0],run[1],run[2]);r=0;}continue;}
        if(r<2){run[r++]=uint32_t(v);continue;}
        tri(run[0],run[1],uint32_t(v));++parity;run[0]=run[1];run[1]=uint32_t(v);
    }
    return true;
}
// Per-candidate visibility with the candidate's OWN +64 matrix: the GS rule
// (a triangle is dropped when its three clip outcodes share a bit). Area: NDC
// area (of 4) of surviving triangles whose three w are > 0, vertices clamped to
// [-1,1] (an approximation of clipping, used only to rank candidates).
struct CandidateEval {
    const char* status="not-evaluated";
    uint32_t vertices=0,indices=0,triangles=0,visible=0;
    double wmin=0,wmax=0,area=0;double bbox[4]{0,0,0,0};
};
inline uint32_t outcode(const std::array<double,4>& c) noexcept;
inline const char* evaluate_draw(const uint8_t* srt,const uint8_t* vb,uint32_t vbFirst,uint32_t vbCount,uint32_t stride,
                                 const uint8_t* ib,uint32_t ibStride,const Shape& d,CandidateEval& e) noexcept {
    e=CandidateEval{};e.vertices=vbCount;e.indices=d.indexed?d.count:0;
    auto fail=[&](const char* why){e.status=why;return why;};
    if(!srt||!vb||!vbCount)return fail("no-bytes");
    if(stride<12)return fail("vertex-stride");
    acz::Mat4 m64;if(!load_matrix(srt+64,m64))return fail("srt-matrix-nonfinite");
    e.wmin=1e300;e.wmax=-1e300;e.bbox[0]=e.bbox[1]=1e300;e.bbox[2]=e.bbox[3]=-1e300;
    bool bad=false;
    auto clipOf=[&](uint32_t v,std::array<double,4>& c) {
        if(v<vbFirst||v-vbFirst>=vbCount){bad=true;c={0,0,0,0};return;}
        float x[3];std::memcpy(x,vb+size_t(v-vbFirst)*stride,12);
        const auto r=acz::transformVec4(m64,{double(x[0]),double(x[1]),double(x[2]),1.0});c={r[0],r[1],r[2],r[3]};
        if(c[3]<e.wmin)e.wmin=c[3];if(c[3]>e.wmax)e.wmax=c[3];
    };
    const bool ok=assemble(ib,ibStride,d.count,d.start,d.base,d.indexed,d.topology,d.strip_cut,[&](uint32_t a,uint32_t b,uint32_t c){
        std::array<double,4> A,B,C;clipOf(a,A);clipOf(b,B);clipOf(c,C);++e.triangles;
        if(outcode(A)&outcode(B)&outcode(C))return;
        ++e.visible;
        if(A[3]>0&&B[3]>0&&C[3]>0) {
            auto cl=[](double v){return v<-1?-1.0:v>1?1.0:v;};
            const double ax=cl(A[0]/A[3]),ay=cl(A[1]/A[3]),bx=cl(B[0]/B[3]),by=cl(B[1]/B[3]),cx=cl(C[0]/C[3]),cy=cl(C[1]/C[3]);
            e.area+=std::fabs((bx-ax)*(cy-ay)-(by-ay)*(cx-ax))*0.5;
            for(double x:{ax,bx,cx}){if(x<e.bbox[0])e.bbox[0]=x;if(x>e.bbox[2])e.bbox[2]=x;}
            for(double y:{ay,by,cy}){if(y<e.bbox[1])e.bbox[1]=y;if(y>e.bbox[3])e.bbox[3]=y;}
        }
    });
    if(!ok)return fail("index-invalid");
    if(bad)return fail("index-outside-sampled-vertices");
    if(e.bbox[0]>e.bbox[2]){e.bbox[0]=e.bbox[1]=e.bbox[2]=e.bbox[3]=0;}
    if(!e.triangles)return fail("no-triangles");
    e.status=e.visible?"visible":"gs-rejected";
    return nullptr;
}
// Selection policy: visible first, then the largest visible NDC area, ties by
// the lowest draw ordinal, then the earliest record.
inline bool better(const CandidateEval& a,uint64_t aOrdinal,uint64_t aSeq,const CandidateEval& b,uint64_t bOrdinal,uint64_t bSeq) noexcept {
    if((a.visible>0)!=(b.visible>0))return a.visible>0;
    if(a.area!=b.area)return a.area>b.area;
    if(aOrdinal!=bOrdinal)return aOrdinal<bOrdinal;
    return aSeq<bSeq;
}
struct Projection {
    const char* status="not-run";uint32_t vertices=0,triangles=0;
    std::array<std::array<float,3>,crop_max_vertices> xyz{};
    std::array<std::array<std::array<double,4>,crop_max_vertices>,replay_count> clip{};   // pos x M64
    std::array<std::array<std::array<double,2>,crop_max_vertices>,replay_count> screen{}; // pixels via M0 (+400 params)
    std::array<uint32_t,replay_count> w_nonpositive{},culled{},screen_valid{};
    std::array<std::array<double,4>,replay_count> extent{}; // minx,miny,maxx,maxy of valid screen points
    uint32_t left=0,top=0,right=0,bottom=0;bool clamped=false,box=false;
    float params[4]{};
};
using Crop=Projection;
inline uint32_t outcode(const std::array<double,4>& c) noexcept {
    const double w=c[3];
    return (w+c[0]<0?16u:0u)|(w+c[1]<0?8u:0u)|(w+c[2]<0?4u:0u)|(w-c[0]<0?2u:0u)|(w-c[1]<0?1u:0u);
}
// Diagnostic projection of the chosen draw for all four replays. Per-vertex
// facts (first 32 sampled vertices are stored for the log) over the sampled
// vertex range; triangles assembled from the draw's own indices.
inline const char* project(const Shape& s,const Targets& t,const uint8_t* vb,uint32_t vbFirst,uint32_t vertices,uint32_t stride,
                           const uint8_t* ib,uint32_t ibStride,const Substitution& sub,Projection& p) noexcept {
    p=Projection{};
    auto done=[&](const char* why){p.status=why;return why;};
    if(!vb||!vertices)return done("no-vertices");
    if(vertices>max_vertices)return done("vertex-count-over-cap");
    if(stride<12)return done("vertex-stride");
    p.vertices=vertices;
    std::memcpy(p.params,sub.blobs[0].data()+400,16); // bytes 400..415 are never substituted
    for(uint32_t i=0;i<vertices&&i<crop_max_vertices;++i)std::memcpy(p.xyz[i].data(),vb+size_t(i)*stride,12);
    double ux0=1e300,uy0=1e300,ux1=-1e300,uy1=-1e300;
    for(unsigned r=0;r<replay_count;++r) {
        const acz::Mat4 m64=r==replay_original?sub.m64:acz::fromFloat32(sub.m64f[r-1]);
        const acz::Mat4 m0=r==replay_original?sub.m0:acz::fromFloat32(sub.m0f[r-1]);
        double x0=1e300,y0=1e300,x1=-1e300,y1=-1e300;
        auto clipOf=[&](uint32_t i){float x[3];std::memcpy(x,vb+size_t(i)*stride,12);
            const auto c=acz::transformVec4(m64,{double(x[0]),double(x[1]),double(x[2]),1.0});return std::array<double,4>{c[0],c[1],c[2],c[3]};};
        for(uint32_t i=0;i<vertices;++i) {
            float x[3];std::memcpy(x,vb+size_t(i)*stride,12);
            const acz::Vec4 v{double(x[0]),double(x[1]),double(x[2]),1.0};
            const auto c=clipOf(i);if(i<crop_max_vertices)p.clip[r][i]=c;
            if(!(c[3]>0))++p.w_nonpositive[r];
            const auto q=acz::transformVec4(m0,v);
            if(q[3]==0||!(p.params[0]!=0)||!(p.params[1]!=0))continue;
            const double nx=2.0*(q[0]/q[3]-double(p.params[2]))/double(p.params[0]);
            const double ny=2.0*(q[1]/q[3]-double(p.params[3]))/double(p.params[1]);
            const double px=double(s.vp0.x)+(nx+1.0)*0.5*double(s.vp0.w),py=double(s.vp0.y)+(1.0-ny)*0.5*double(s.vp0.h);
            if(!std::isfinite(px)||!std::isfinite(py))continue;
            if(i<crop_max_vertices)p.screen[r][i]={px,py};
            ++p.screen_valid[r];
            if(c[3]>0){x0=px<x0?px:x0;y0=py<y0?py:y0;x1=px>x1?px:x1;y1=py>y1?py:y1;}
        }
        uint32_t tris=0;bool bad=false;
        const bool ok=assemble(ib,ibStride,s.count,s.start,s.base,s.indexed,s.topology,s.strip_cut,[&](uint32_t a,uint32_t b,uint32_t c){
            ++tris;
            if(a<vbFirst||b<vbFirst||c<vbFirst||a-vbFirst>=vertices||b-vbFirst>=vertices||c-vbFirst>=vertices){bad=true;return;}
            if(outcode(clipOf(a-vbFirst))&outcode(clipOf(b-vbFirst))&outcode(clipOf(c-vbFirst)))++p.culled[r];
        });
        if(!ok||bad)return done("index-invalid");
        p.triangles=tris;
        p.extent[r]={x0,y0,x1,y1};
        if(x0<=x1){ux0=x0<ux0?x0:ux0;uy0=y0<uy0?y0:uy0;ux1=x1>ux1?x1:ux1;uy1=y1>uy1?y1:uy1;}
    }
    if(p.culled[replay_original]==p.triangles&&p.triangles)return done("original-all-triangles-gs-rejected");
    if(!(ux0<=ux1))return done("no-front-vertex");
    const double l=std::floor(ux0)-crop_pad,tp=std::floor(uy0)-crop_pad,rr=std::ceil(ux1)+crop_pad,bb=std::ceil(uy1)+crop_pad;
    if(rr<=0||bb<=0||l>=double(t.width)||tp>=double(t.height))return done("offscreen");
    p.clamped=l<0||tp<0||rr>double(t.width)||bb>double(t.height);
    p.left=uint32_t(l<0?0:l);p.top=uint32_t(tp<0?0:tp);
    p.right=uint32_t(rr>double(t.width)?t.width:rr);p.bottom=uint32_t(bb>double(t.height)?t.height:bb);
    p.box=p.right>p.left&&p.bottom>p.top;
    return done(p.box?"ok":"offscreen");
}
// Box-filter downscale of tight RGBA8 rows by an integer factor so the width is <= max_w.
inline uint32_t downscale_factor(uint32_t w,uint32_t max_w) noexcept {return w<=max_w?1u:(w+max_w-1)/max_w;}
inline bool downscale_rgba8(const std::vector<uint8_t>& in,uint32_t w,uint32_t h,uint32_t f,std::vector<uint8_t>& out,
                            uint32_t& ow,uint32_t& oh) noexcept {
    try {
        if(!f||in.size()<size_t(w)*h*4)return false;
        ow=w/f;oh=h/f;if(!ow||!oh)return false;
        out.assign(size_t(ow)*oh*4,0);
        for(uint32_t y=0;y<oh;++y)for(uint32_t x=0;x<ow;++x)for(int c=0;c<4;++c) {
            uint32_t sum=0;
            for(uint32_t dy=0;dy<f;++dy)for(uint32_t dx=0;dx<f;++dx)sum+=in[((size_t(y)*f+dy)*w+size_t(x)*f+dx)*4+size_t(c)];
            out[(size_t(y)*ow+x)*4+size_t(c)]=uint8_t((sum+f*f/2)/(f*f));
        }
        return true;
    } catch(...) {out.clear();return false;}
}
// Expected horizontal disparity (pixels, left minus right) of a point straight
// ahead at `depth` world units, for a viewport `width` pixels wide. Estimate only:
// assumes the symmetric recovered lens and PS2-screen x increasing with clip x.
inline double expected_disparity_px(const Substitution& s,double depth,double width) noexcept {
    if(!(depth>0)||!(s.camera.tanHalfFovX>0))return 0;
    return (2.0*half_ipd_units/depth)/s.camera.tanHalfFovX*(width*0.5);
}

// ---------------------------------------------------------------- pixels
// Background = the clear colour (0,0,0,0): a pixel is "covered" when any of its
// four bytes differs from zero. Coordinates are pixel indices (centre at +0.5).
struct Pixels {
    uint64_t count=0;uint32_t minx=UINT32_MAX,maxx=0,miny=UINT32_MAX,maxy=0;double cx=0,cy=0;
    uint32_t alpha_min=255,alpha_max=0;
};
// `bpp` 4 (8-bit RGBA/BGRA, alpha = byte 3) or 8 (FP16 RGBA: alpha stats unused).
inline bool covered(const uint8_t* p,uint32_t bpp) noexcept {for(uint32_t i=0;i<bpp;++i)if(p[i])return true;return false;}
inline void analyse(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,uint32_t bpp,Pixels& out) noexcept {
    out=Pixels{};double sx=0,sy=0;
    for(uint32_t y=0;y<h;++y) {
        const uint8_t* row=base+uint64_t(y)*pitch;
        for(uint32_t x=0;x<w;++x) {
            const uint8_t* p=row+uint64_t(x)*bpp;
            if(!covered(p,bpp))continue;
            ++out.count;sx+=x+0.5;sy+=y+0.5;
            if(x<out.minx)out.minx=x;if(x>out.maxx)out.maxx=x;if(y<out.miny)out.miny=y;if(y>out.maxy)out.maxy=y;
            if(bpp==4){if(p[3]<out.alpha_min)out.alpha_min=p[3];if(p[3]>out.alpha_max)out.alpha_max=p[3];}
        }
    }
    if(out.count){out.cx=sx/double(out.count);out.cy=sy/double(out.count);}
    else {out.minx=out.miny=0;}
    if(!out.count||bpp!=4)out.alpha_min=out.alpha_max=0;
}
// Count of pixels whose bytes differ; `first` is the first differing pixel index or -1.
inline uint64_t differing(const uint8_t* a,const uint8_t* b,uint32_t w,uint32_t h,uint64_t pitch,uint32_t bpp,int64_t& first) noexcept {
    uint64_t n=0;first=-1;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const uint64_t at=uint64_t(y)*pitch+uint64_t(x)*bpp;
        if(std::memcmp(a+at,b+at,bpp)){if(first<0)first=int64_t(uint64_t(y)*w+x);++n;}
    }
    return n;
}
// IEEE 754 binary16 -> float (exact; subnormals, Inf and NaN preserved).
inline float half_to_float(uint16_t h) noexcept {
    const uint32_t sign=uint32_t(h&0x8000)<<16,exp=(h>>10)&0x1f,man=h&0x3ff;
    uint32_t bits;
    if(exp==0) {
        if(!man)bits=sign;
        else {int e=-1;uint32_t m=man;do{++e;m<<=1;}while(!(m&0x400));bits=sign|uint32_t(127-15-e)<<23|(m&0x3ff)<<13;}
    } else if(exp==31)bits=sign|0x7f800000u|man<<13;
    else bits=sign|(exp+127-15)<<23|man<<13;
    float f;std::memcpy(&f,&bits,4);return f;
}
// Raw FP16 channel statistics over covered (non-clear) texels.
struct FloatStats {uint64_t count=0,nonfinite=0;std::array<double,4> min{},max{},mean{};};
inline void float_stats(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,FloatStats& out) noexcept {
    out=FloatStats{};std::array<double,4> sum{};
    for(int c=0;c<4;++c){out.min[size_t(c)]=1e300;out.max[size_t(c)]=-1e300;}
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const uint8_t* p=base+uint64_t(y)*pitch+uint64_t(x)*8;if(!covered(p,8))continue;
        ++out.count;
        for(int c=0;c<4;++c) {
            uint16_t v;std::memcpy(&v,p+2*c,2);const double f=half_to_float(v);
            if(!std::isfinite(f)){++out.nonfinite;continue;}
            sum[size_t(c)]+=f;if(f<out.min[size_t(c)])out.min[size_t(c)]=f;if(f>out.max[size_t(c)])out.max[size_t(c)]=f;
        }
    }
    for(int c=0;c<4;++c) {
        if(out.count){out.mean[size_t(c)]=sum[size_t(c)]/double(out.count);}
        else {out.min[size_t(c)]=out.max[size_t(c)]=0;}
    }
}
inline uint8_t unorm8(float f) noexcept {
    if(!(f>0.0f))return 0; // also NaN
    if(f>=1.0f)return 255;
    return uint8_t(f*255.0f+0.5f);
}
// Tight 8-bit RGBA rows for the PNG: 8-bit formats copied (BGRA reordered),
// FP16 converted as clamp(value,0,1)*255 rounded (linear scale, no gamma).
// Display variant: linear values (FP16, or 8-bit UNORM as v/255) encoded to sRGB;
// an _SRGB target's bytes are already sRGB-encoded and are copied unchanged.
inline bool to_srgb8(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,const ColorFormat& f,std::vector<uint8_t>& out) noexcept {
    try {
        out.assign(size_t(w)*h*4,0);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            uint8_t* o=&out[(size_t(y)*w+x)*4];
            const uint8_t* p=base+uint64_t(y)*pitch+uint64_t(x)*f.bytes;
            float c[4];
            if(f.half)for(int i=0;i<4;++i){uint16_t v;std::memcpy(&v,p+2*i,2);c[i]=half_to_float(v);}
            else {const uint8_t r=f.bgra?p[2]:p[0],b=f.bgra?p[0]:p[2];c[0]=r/255.0f;c[1]=p[1]/255.0f;c[2]=b/255.0f;c[3]=p[3]/255.0f;}
            if(f.srgb&&!f.half){o[0]=uint8_t(c[0]*255.0f+0.5f);o[1]=uint8_t(c[1]*255.0f+0.5f);o[2]=uint8_t(c[2]*255.0f+0.5f);}
            else {o[0]=srgb8(c[0]);o[1]=srgb8(c[1]);o[2]=srgb8(c[2]);}
            o[3]=unorm8(c[3]); // alpha stays linear
        }
        return true;
    } catch(...) {out.clear();return false;}
}
// Robust horizontal disparity: column profiles of luminance summed over the
// central rows [h/4, 3h/4), mean-removed; the shift s in [-max_shift, max_shift]
// maximising the normalised cross-correlation of L(x) with R(x - s) (overlap
// only), refined by a parabola through the peak. s > 0: the left image is
// shifted right of the right image (the expected sign for content in front).
inline double luminance(const uint8_t* p,const ColorFormat& f) noexcept {
    float c[3];
    if(f.half)for(int i=0;i<3;++i){uint16_t v;std::memcpy(&v,p+2*i,2);c[i]=half_to_float(v);if(!std::isfinite(c[i]))c[i]=0;}
    else {c[0]=(f.bgra?p[2]:p[0])/255.0f;c[1]=p[1]/255.0f;c[2]=(f.bgra?p[0]:p[2])/255.0f;}
    return 0.2126*c[0]+0.7152*c[1]+0.0722*c[2];
}
struct XcorrResult {double shift=0,score=0;int peak=0;bool valid=false;};
inline bool profile_disparity(const uint8_t* L,const uint8_t* R,uint32_t w,uint32_t h,uint64_t pitch,const ColorFormat& f,int max_shift,
                              XcorrResult& out) noexcept {
    out=XcorrResult{};
    try {
        if(w<8||h<4)return false;
        std::vector<double> pl(w,0.0),pr(w,0.0);
        for(uint32_t y=h/4;y<3*h/4;++y)for(uint32_t x=0;x<w;++x) {
            pl[x]+=luminance(L+uint64_t(y)*pitch+uint64_t(x)*f.bytes,f);pr[x]+=luminance(R+uint64_t(y)*pitch+uint64_t(x)*f.bytes,f);
        }
        double ml=0,mr=0;for(uint32_t x=0;x<w;++x){ml+=pl[x];mr+=pr[x];}ml/=w;mr/=w;
        for(uint32_t x=0;x<w;++x){pl[x]-=ml;pr[x]-=mr;}
        if(max_shift>int(w/2))max_shift=int(w/2);
        std::vector<double> score(size_t(2*max_shift+1),-2.0);
        double best=-2.0;int bi=0;
        for(int s=-max_shift;s<=max_shift;++s) {
            double sxy=0,sxx=0,syy=0;
            for(int x=0;x<int(w);++x){const int xr=x-s;if(xr<0||xr>=int(w))continue;sxy+=pl[size_t(x)]*pr[size_t(xr)];sxx+=pl[size_t(x)]*pl[size_t(x)];syy+=pr[size_t(xr)]*pr[size_t(xr)];}
            const double c=sxx>0&&syy>0?sxy/std::sqrt(sxx*syy):-2.0;
            score[size_t(s+max_shift)]=c;if(c>best){best=c;bi=s;}
        }
        if(best<=-2.0)return false;
        double shift=bi;
        if(bi>-max_shift&&bi<max_shift) {
            const double a=score[size_t(bi-1+max_shift)],b=score[size_t(bi+max_shift)],c=score[size_t(bi+1+max_shift)];
            const double d=a-2*b+c;if(d<0)shift+=0.5*(a-c)/d;
        }
        out.shift=shift;out.score=best;out.peak=bi;out.valid=true;return true;
    } catch(...) {return false;}
}
inline bool to_rgba8(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,const ColorFormat& f,std::vector<uint8_t>& out) noexcept {
    try {
        out.assign(size_t(w)*h*4,0);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            uint8_t* o=&out[(size_t(y)*w+x)*4];
            const uint8_t* p=base+uint64_t(y)*pitch+uint64_t(x)*f.bytes;
            if(f.half)for(int c=0;c<4;++c){uint16_t v;std::memcpy(&v,p+2*c,2);o[c]=unorm8(half_to_float(v));}
            else if(f.bgra){o[0]=p[2];o[1]=p[1];o[2]=p[0];o[3]=p[3];}
            else std::memcpy(o,p,4);
        }
        return true;
    } catch(...) {out.clear();return false;}
}

// ---------------------------------------------------------------- PNG
// Minimal uncompressed PNG: 8-bit RGBA (colour type 6), one IDAT holding a
// zlib stream of stored (uncompressed) deflate blocks, filter 0 per row.
// Channel bytes are written raw, reordered to RGBA for B8G8R8A8 targets. sRGB
// targets hold sRGB-encoded bytes and UNORM targets linear bytes; neither is
// converted (no gAMA/sRGB chunk is written).
inline uint32_t crc32(const uint8_t* p,size_t n,uint32_t crc=0) noexcept {
    static const auto table=[]{std::array<uint32_t,256> t{};
        for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int k=0;k<8;++k)c=c&1?0xedb88320u^(c>>1):c>>1;t[i]=c;}return t;}();
    crc=~crc;for(size_t i=0;i<n;++i)crc=table[(crc^p[i])&0xff]^(crc>>8);return ~crc;
}
inline uint32_t adler32(const uint8_t* p,size_t n,uint32_t adler=1) noexcept {
    uint32_t a=adler&0xffff,b=adler>>16;
    for(size_t i=0;i<n;++i){a=(a+p[i])%65521u;b=(b+a)%65521u;}
    return (b<<16)|a;
}
inline void put32(std::vector<uint8_t>& o,uint32_t v){o.push_back(uint8_t(v>>24));o.push_back(uint8_t(v>>16));o.push_back(uint8_t(v>>8));o.push_back(uint8_t(v));}
inline bool png_encode(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,bool bgra,std::vector<uint8_t>& out) noexcept {
    try {
        out.clear();
        if(!w||!h||uint64_t(w)*4>pitch)return false;
        const uint64_t rowBytes=uint64_t(w)*4+1,raw=rowBytes*h;
        if(raw>(uint64_t(1)<<30))return false;
        std::vector<uint8_t> data;data.reserve(size_t(raw));
        for(uint32_t y=0;y<h;++y) {
            data.push_back(0);const uint8_t* row=base+uint64_t(y)*pitch;
            for(uint32_t x=0;x<w;++x) {
                const uint8_t* p=row+uint64_t(x)*4;
                if(bgra){data.push_back(p[2]);data.push_back(p[1]);data.push_back(p[0]);data.push_back(p[3]);}
                else data.insert(data.end(),p,p+4);
            }
        }
        std::vector<uint8_t> z;const size_t blocks=(data.size()+65534)/65535;
        z.reserve(2+data.size()+blocks*5+4);
        z.push_back(0x78);z.push_back(0x01); // CMF/FLG: deflate, 32 KiB window, no dictionary
        for(size_t at=0,b=0;b<blocks;++b) {
            const size_t len=data.size()-at<65535?data.size()-at:65535;
            z.push_back(b+1==blocks?1:0); // BFINAL, BTYPE=00 (stored)
            z.push_back(uint8_t(len));z.push_back(uint8_t(len>>8));z.push_back(uint8_t(~len));z.push_back(uint8_t((~len)>>8));
            z.insert(z.end(),data.begin()+std::ptrdiff_t(at),data.begin()+std::ptrdiff_t(at+len));at+=len;
        }
        put32(z,adler32(data.data(),data.size()));
        static constexpr uint8_t signature[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
        out.insert(out.end(),signature,signature+8);
        auto chunk=[&](const char* type,const uint8_t* p,size_t n) {
            put32(out,uint32_t(n));const size_t start=out.size();
            out.insert(out.end(),type,type+4);if(n)out.insert(out.end(),p,p+n);
            put32(out,crc32(out.data()+start,n+4));
        };
        uint8_t ihdr[13]={uint8_t(w>>24),uint8_t(w>>16),uint8_t(w>>8),uint8_t(w),uint8_t(h>>24),uint8_t(h>>16),uint8_t(h>>8),uint8_t(h),
            8,6,0,0,0}; // bit depth 8, RGBA, deflate, filter 0, no interlace
        chunk("IHDR",ihdr,sizeof ihdr);chunk("IDAT",z.data(),z.size());chunk("IEND",nullptr,0);
        return true;
    } catch(...) {out.clear();return false;}
}
// PERF (docs/world-render/perf-impl.md): the SAME bytes as png_encode (stored deflate blocks of 65535 raw bytes, filter 0, one IDAT), streamed: the
// rows are read from `base` one at a time and handed to `write(const uint8_t*, size_t) -> bool` in pieces of at most `buffer_bytes`. Heap use is
// one row (w*4+1) plus the buffer, independent of the image size (png_encode holds ~4 copies of the image). Sizes/CRCs/Adler are computed on the
// fly; the IDAT length is known up front. false = bad arguments, an allocation failure or a writer failure (the stream is then incomplete).
inline uint64_t png_stored_size(uint32_t w,uint32_t h) noexcept {
    const uint64_t raw=(uint64_t(w)*4+1)*h,blocks=(raw+65534)/65535;
    return 8+(12+13)+(12+2+raw+blocks*5+4)+12;
}
template<class Write>
inline bool png_stream(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,bool bgra,Write&& write,size_t buffer_bytes=size_t(64)<<10) noexcept {
    try {
        if(!base||!w||!h||uint64_t(w)*4>pitch||buffer_bytes<64)return false;
        const uint64_t rowBytes=uint64_t(w)*4+1,raw=rowBytes*h;
        if(raw>(uint64_t(1)<<30))return false;
        const uint64_t blocks=(raw+65534)/65535,zlen=2+raw+blocks*5+4;
        std::vector<uint8_t> buf;buf.reserve(buffer_bytes);
        std::vector<uint8_t> row(static_cast<size_t>(rowBytes));
        bool ok=true;
        auto flush=[&]{if(ok&&!buf.empty()){ok=write(static_cast<const uint8_t*>(buf.data()),buf.size());}buf.clear();};
        auto put=[&](const uint8_t* p,size_t n){
            while(n&&ok){const size_t room=buffer_bytes-buf.size(),k=n<room?n:room;buf.insert(buf.end(),p,p+k);p+=k;n-=k;if(buf.size()==buffer_bytes)flush();}
        };
        auto be32=[](uint32_t v,uint8_t o[4]){o[0]=uint8_t(v>>24);o[1]=uint8_t(v>>16);o[2]=uint8_t(v>>8);o[3]=uint8_t(v);};
        static constexpr uint8_t signature[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
        put(signature,8);
        {   // IHDR
            uint8_t c[4+4+13+4];be32(13,c);std::memcpy(c+4,"IHDR",4);
            const uint8_t ihdr[13]={uint8_t(w>>24),uint8_t(w>>16),uint8_t(w>>8),uint8_t(w),uint8_t(h>>24),uint8_t(h>>16),uint8_t(h>>8),uint8_t(h),8,6,0,0,0};
            std::memcpy(c+8,ihdr,13);be32(crc32(c+4,17),c+21);put(c,sizeof c);
        }
        if(zlen>0xffffffffull)return false;
        uint8_t lenType[8];be32(uint32_t(zlen),lenType);std::memcpy(lenType+4,"IDAT",4);put(lenType,8);
        uint32_t crc=crc32(lenType+4,4),adler=1;
        auto zput=[&](const uint8_t* p,size_t n){crc=crc32(p,n,crc);put(p,n);};
        static constexpr uint8_t zhdr[2]={0x78,0x01};zput(zhdr,2);
        uint64_t done=0,blockLeft=0;
        auto rawput=[&](const uint8_t* p,size_t n){
            adler=adler32(p,n,adler);
            while(n&&ok){
                if(!blockLeft){
                    const uint64_t rest=raw-done;const uint32_t len=uint32_t(rest<65535?rest:65535);
                    const uint8_t bh[5]={uint8_t(rest<=65535?1:0),uint8_t(len),uint8_t(len>>8),uint8_t(~len),uint8_t((~len)>>8)};
                    zput(bh,5);blockLeft=len;
                }
                const size_t k=size_t(n<blockLeft?n:blockLeft);
                zput(p,k);p+=k;n-=k;blockLeft-=k;done+=k;
            }
        };
        for(uint32_t y=0;y<h&&ok;++y) {
            const uint8_t* src=base+uint64_t(y)*pitch;row[0]=0;
            if(bgra){for(uint32_t x=0;x<w;++x){const uint8_t* p=src+uint64_t(x)*4;uint8_t* o=&row[1+size_t(x)*4];o[0]=p[2];o[1]=p[1];o[2]=p[0];o[3]=p[3];}}
            else std::memcpy(&row[1],src,size_t(w)*4);
            rawput(row.data(),row.size());
        }
        uint8_t ad[4];be32(adler,ad);zput(ad,4);
        uint8_t cr[4];be32(crc,cr);put(cr,4);
        static constexpr uint8_t iend[12]={0,0,0,0,'I','E','N','D',0xae,0x42,0x60,0x82};
        put(iend,12);
        flush();
        return ok&&done==raw;
    } catch(...) {return false;}
}
} // namespace acz::gpu_copy::step3
