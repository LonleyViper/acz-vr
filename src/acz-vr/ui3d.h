#pragma once
// UI3D stage 1 (docs/world-render/ui-3d.md): the game's 2D MENU draws re-rendered per eye onto 3 depth layers (backdrop far, panels, text near).
// SDK-free, header-only CPU core: options, the online layer classifier, the layer surfaces and the per-vertex re-projection of a ps2_v draw.
// Nothing here touches D3D12 or OpenXR. Stage 2 (2026-10-29, not run live): the producer frame kind lives in brief3d_live.h (Producer::ui3d_draw:
// the same ring/slot/list/fence lifetime as the briefing table), the D3D12 replay (a per-eye replay of the game's own ps2_v draw with a VERTEX
// BUFFER COPY, recorded inline before the game's draw) in ui3d_replay_hooks.inl, and the consumer is the BRIEF3D TableCore with the slot
// submitted OPAQUE. Fixture success says nothing about the live game or a headset.
//
// Facts this core relies on (ui-3d.md section 1):
//  * [E: R4 briefing-observe, step92/runR4-logs/ee-camera.log] the main menu and the mission menus draw ONE key only: VS 534f29ca (ps2_v) /
//    PS d34ee153, non-indexed triangles, max 4 vertices per draw (one quad), ~350 (main menu) / ~68-146 (mission menus) draws per present.
//  * [E: dxc disassembly of 534f29ca, step92/R3-shaders] vertex = 4 x float4 (64 B): ATTRIBUTE0 = x, y, z, q (GS screen); SV_Position =
//    ((x - cb0.z) * 2 / cb0.x, (y - cb0.w) * 2 / cb0.y, z * 2^-24, 1) / q; COLOR0, COLOR1 and TEXCOORD are interpolated `linear` (perspective
//    correct w.r.t. w = 1/q). So rewriting x, y and q of the 4 vertices places the game's own quad (its texture, colours, PSO) anywhere in the eye
//    image with exact perspective texture mapping: q' = q x w_anchor / w_eye (section 3.3 of ui-3d.md derives it), z unchanged (depth behaviour unchanged).
//
// Sub-options of xr_terrain_stage (no new config key; the 32-key cap is full). Default OFF.
//   ui3d:<0|1>   1 = re-render the game's ps2_v menu frames per eye on depth layers (needs vscreen:1 + vsearly:1 + xr_hud_layer, like brief3d)
//   u3n:<m>      distance of the NEAREST layer (text) from the anchor eye (default 1.8, 0.5..10)
//   u3f:<m>      distance of the FARTHEST layer (backdrop) (default 3.5, 0.6..30); must be > u3n; the panel layer sits halfway in dioptres
//   u3t:<deg>    tilt of every layer plane about its horizontal axis, top away from the user (default 0, -30..30)
//   u3c:<0|1>    1 = every vertex on a vertical cylinder about the anchor (radius = the layer distance) instead of a plane (default 0)
//   u3p:<0|1>    stage 3a (ui3d_pause.h, ui-3d.md 7): 1 = also the IN-FLIGHT PAUSE MENU on the same depth layers, blended over the dimmed
//                cockpit/world while the HUD-MENU detector is on (default 0 = today's hud-menu behaviour). Needs ui3d:1.
//   u3a:<0|1|2>  stage 3b (ui3d_hangar.h, ui-3d.md 9): the HANGAR screens (aircraft select, special-weapon selects) as a 3D ROOM: every DrawACM_v
//                draw re-rendered per eye with a substituted camera, the 2D overlay on the layers above. 1 = room latched at the screen's first
//                frame (re-latched at a camera cut), 2 = the room follows the game camera's yaw (the camera orbit preserved). Default 0. Needs ui3d:1.
//   u3au:<u/m>   stage 3b: hangar units per metre (head translation scale; default 5.0 [I], 1..200)
//   u3mn:<m>     2026-10-30 (user feedback: the menus read as the flat screen): the nearest (text) layer of the MENU and HANGAR-OVERLAY frames
//                (default 1.1, 0.5..10; the pause menu keeps u3n)
//   u3mf:<m>     the farthest (backdrop) layer of the same frames (default 6.0, 0.6..30; must be > u3mn); a far/near ratio of ~5.5 gives
//                the layers a visible stereo / parallax separation (the older 1.8 / 3.5 pair is a ratio of 1.9)
//   u3s:<f>      angular size of the whole menu panel relative to the vscreen framing (default 0.7, 0.2..1.5): the panel floats smaller
//                in the dark void (menus) / over the room (hangar) instead of filling the flat screen's field of view
//   u3q:<0|1>    2026-10-30b (headset: the flat screen sat in front of the room / the briefing table): 1 (default) = SteamVR draws quad layers ABOVE
//                projection layers whatever the order (xr-stereo.log hypothesis=steamvr-draws-quad-layers-above-projection), so a live opaque
//                menu/hangar slot is submitted ALONE (no screen quad) and the briefing table is submitted as two per-eye quads after the screen quad;
//                0 = the old [screen quad, projection] list (the screen covers the 3D)
//   u3r:<0|1|2>  resample of an opaque UI3D / pause slot (a 1440x1440 eye image) into the 2400x1350 eye swapchain (a 1.67x horizontal
//                UPSCALE; bilinear = the blur the user saw): 0 bilinear (old), 1 Catmull-Rom bicubic, 2 (default) Catmull-Rom + unsharp mask
//   u3k:<0..1>   unsharp amount of u3r:2 (default 0.35): out = CR + k (CR - bilinear)
//   u3sel:<0|1|2> 2026-10-30c (headset test 5: "the selection box could be 3d and closer to the text so i can use it in VR"): the selection box
//                (a bar centred on a text row of the previous frame, SelectionRows) is replayed just behind the TEXT plane: 2 (default) = a slab
//                (back face, 4 walls, front face), 1 = the flat front face only, 0 = on its classifier layer (old). The other panels / lines keep
//                their layer depth.
//   u3sg:<mm>    text plane -> the selection slab's front face (default 4, 0..50)
//   u3sd:<mm>    the selection slab's thickness (default 12, 1..100)
//   u3fs:<k>     angular size of the BACKDROP layer (far, layer 0) relative to the panel framing (default 1.0 = aligned with the flat image, 1..3):
//                > 1 spreads the backdrop (and its scissor) over a larger part of the view behind the unchanged panels and text
//   u3at:<0|1>   hangar (u3a): 0 (default) = the overlay's FULL-SCREEN draws (bbox area >= kLargeArea: the yellow tint / grid / frame quads and the
//                z-0 dim) are dropped, so no translucent screen-sized rectangle floats in the room; 1 = replayed on the far layer (old)
//   u3ag:<0|1>   hangar (u3a): 1 (default) = the pre-projected light glows (non-indexed ps2_v quads with real GS z before the overlay) are
//                re-projected through the frame's game camera into the room per eye (ui3d_hangar.h rewrite_effect); 0 = dropped (old)
//   u3agm:<0|1|2> 2026-10-30d (headset test 6: "hangar lights turn off intermittently"): the game emits a glow only for a light ITS orbiting
//                camera sees, so in the latched room the lamps blinked with the game camera. 1 (default) = a GLOW MEMORY: every re-projected glow
//                is remembered in hangar-world space and redrawn (as an extra copy of a later glow draw with the same PSO / pixel tables, never
//                earlier in the game's order than it was seen) while the game does not emit it; cleared at a room latch and a camera cut;
//                2 = kept across cuts (cleared at a latch only); 0 = the game's glows only (old). ui3d_hangar.h GlowMemory, ui-3d.md 13.
//   u3pp:<0|1>   2026-10-30d (headset test 6: "the hangar UI panel is offset from the background elements"): 1 = the panel layer is placed ON THE
//                TEXT PLANE (one surface for frame lines, panels, selection and text: nothing of one panel can separate with head motion; the
//                backdrop keeps its own depth); 0 (default) = the 3 classifier layers (old). The classifier and its statistics are unchanged.
//   u3lt:<mm>    2026-10-30d: frame thickness (default 0 = flat, 0..50): a panel / frame / tile quad (medium, 4 vertices, the panel priority z > 1)
//                is replayed as a RIM: its 4 edge walls extruded u3lt behind its surface + the front face on it (rewrite_rim); from the anchor the
//                front face is the flat image exactly, from elsewhere the edge texels (the yellow outline) show as a bevelled strip.
//   2026-10-31 (ui-3d.md 14: the 3D reinterpretation stages 2-3, "glass and amber"; ui3d_groups.h). All default OFF; every element is still one
//   of the game's own ps2_v quads on its ray from the anchor, so from the anchor the composite stays the flat image.
//   u3gp:<0..3>  the look, one token (the option buffer has 31 characters left in the user's cfg): 0 off; 1 = stage 2 PANEL GROUPS: the
//                previous UI3D frame's frame quads (frame_quad) are the panel rectangles; every non-backdrop draw of this frame joins the smallest
//                rectangle containing its box centre and is placed on that group's PLANE: the tangent plane of a vertical cylinder about the
//                anchor (radius = the text distance) at the top-level panel's direction, nested panels u3gn in front of their parent; a draw in
//                no rectangle stays on the base surface (the text plane, = u3pp:1). 2 = 1 + stage 3 at its defaults (u3bv 0.4, u3tx 3, u3sr 8);
//                3 = 2 + pane back faces (u3pb 0.35). The knobs below override the level's values when given (any order).
//   u3gk:<0..1>  stage 2: curvature (default 1 = the full cylinder tangent; 0 = every group faces the screen's forward = flat, no wrap)
//   u3gn:<mm>    stage 2: nested panel -> in front of its parent (default 5, 0..20)
//   u3bv:<0..0.9> stage 3: bevel shade: the rim / slab walls facing DOWN or RIGHT on the flat image get COLOR0.rgb x (1 - u3bv) (default 0 = off)
//   u3tx:<mm>    stage 3: text (non-frame draws on the text layer) raised this far in front of its pane (default 0, 0..20)
//   u3sr:<mm>    stage 3: the selection slab raised IN FRONT of its pane: front face u3sr in front, back face on the pane; the glyphs inside
//                the frame's selection box ride on its front face (default 0 = the u3sel rule: just behind the text plane; 0..50)
//   u3pb:<0..1>  stage 3: pane back faces: a rim (u3lt > 0) also draws its back face u3lt behind the pane with COLOR0.a x u3pb (default 0 = none)
// Every layer keeps the DIRECTION of each point of the flat screen as seen from the anchor eye (the head position when the screen was placed):
// from that point the composite is exactly the flat game image (same angles); stereo and head motion give each layer its own depth.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace acz::ui3d {

// ================================================================================================== constants
inline constexpr uint32_t kPs2Vs8 = 0x534f29cau;          // ps2_v / ps2AW_v (identical DXIL): the emulated-GS pre-projected 2D draws
inline constexpr uint32_t kSelMapVs8 = 0x384ade02u, kModelIconVs8 = 0xd5a41c2fu, kAcmVs8 = 0xb569109eu, kCptVs8 = 0xf4cfd928u;  // GPU-transformed 3D
inline constexpr uint32_t kMovieVs8 = 0x15c8c0bdu;        // movie_yuv_v (cinematics, map presentation)
inline constexpr uint32_t kVertexStride = 64;             // 4 x float4
inline constexpr uint32_t kMaxVerticesPerDraw = 64;       // menus: 4 [E]; more = the frame is refused (stage 1)
inline constexpr uint32_t kMaxDrawsPerFrame = 1024;       // main menu ~350 [E]
inline constexpr unsigned kLayers = 3;                    // 0 = backdrop (far), 1 = panels, 2 = text (near)
inline constexpr double kNearM = 0.05;                    // a vertex nearer than this (or behind the eye) culls the draw for that eye
inline constexpr double kLargeArea = 0.30;                // bbox area (fraction of the screen) at or above which a draw is "large" (backdrop / overlay)
// bbox width AND height at or below this fraction of the shown image: "small" (glyph / particle). Census 2026-10-29 (step124/runD1, ui-3d.md 6):
// menu glyphs are 0.015 x 0.0536 of the 2560 x 1440 image (main / mission menus) and 0.009 x 0.036 (overview); the smallest panel tile seen is
// 0.047 x 0.071. 0.05 (stage 1) put every main-menu glyph on the panel layer; 0.06 separates them [E].
inline constexpr double kSmallDim = 0.06;

// ================================================================================================== options
struct Options {
    bool on = false;
    float near_m = 1.8f, far_m = 3.5f, tilt_deg = 0.0f;
    bool curve = false;
    bool pause = false;   // u3p:1 (stage 3a)
    unsigned hangar = 0;  // u3a:<0|1|2> (stage 3b): 0 off, 1 latched room, 2 room follows the game camera yaw
    float hangar_upm = 5.0f;   // u3au (stage 3b)
    float menu_near_m = 1.1f, menu_far_m = 6.0f;   // u3mn / u3mf: layer distances of menu + hangar-overlay frames (not the pause menu)
    float scale = 0.7f;        // u3s: angular size of the menu panel relative to the vscreen framing
    unsigned layering = 1;     // u3q (2026-10-30b): 1 = the flat screen quad never covers a UI3D / BRIEF3D layer (SteamVR draws quad layers above projection layers), 0 = the old [screen quad, projection] list
    unsigned filter = 2;       // u3r: resample of an opaque UI3D / pause slot into the eye swapchain: 0 bilinear (old), 1 Catmull-Rom, 2 Catmull-Rom + unsharp mask
    float sharpen = 0.35f;     // u3k: unsharp amount of filter 2 (0..1)
    // 2026-10-30c (headset test 5, items 1-2): the selection box as a 3D slab just behind the text, the hangar tint dropped, the hangar light
    // glows re-projected, the backdrop layer optionally enlarged. See ui-3d.md section 12.
    unsigned select = 2;       // u3sel: 0 = the selection box stays on its classifier layer (old), 1 = a flat quad just behind the text plane, 2 = a slab
    float sel_gap_mm = 4.0f;   // u3sg: text plane -> the slab's front face (0..50 mm)
    float sel_depth_mm = 12.0f;   // u3sd: the slab's thickness (1..100 mm)
    float far_scale = 1.0f;    // u3fs: angular size of the BACKDROP layer (layer 0) relative to the panel framing (1..3); 1 = aligned with the flat image
    bool hangar_tint = false;  // u3at: 1 = keep the hangar overlay's full-screen draws (the yellow tint / frame / dim, old); 0 = drop them
    bool hangar_glow = true;   // u3ag: 1 = re-project the hangar's pre-projected light glows into the room per eye; 0 = drop them (old)
    // 2026-10-30d (headset test 6, items 1-2): ui-3d.md section 13
    unsigned glow_memory = 1;  // u3agm: 0 = the game's glows only (old), 1 = glow memory cleared at a latch / cut, 2 = kept across cuts
    bool panel_plane = false;  // u3pp: 1 = the panel layer on the text plane (one surface per panel: no offset), 0 = the 3 layers (old)
    float frame_mm = 0.0f;     // u3lt: 0 = flat; else panel / frame quads as rims extruded this far behind their surface (0..50 mm)
    // 2026-10-31 (ui-3d.md 14; ui3d_groups.h): stages 2-3 of the 3D reinterpretation, all default off
    unsigned look = 0;         // u3gp: 0 off, 1 groups, 2 groups + stage 3, 3 = 2 + pane back faces
    bool groups = false;       // u3gp >= 1: panel groups on their own (curved) planes
    float curve_k = 1.0f;      // u3gk: 0..1 curvature of the group planes
    float nest_mm = 5.0f;      // u3gn: nested panel in front of its parent (0..20 mm)
    float bevel = 0.0f;        // u3bv: 0..0.9 darkening of the down / right facing walls
    float text_mm = 0.0f;      // u3tx: text raised in front of its pane (0..20 mm)
    float sel_raise_mm = 0.0f; // u3sr: the selection slab raised in front of its pane (0..50 mm; 0 = the u3sel rule)
    float back_alpha = 0.0f;   // u3pb: rim back face alpha factor (0..1; 0 = no back face)
    bool any_token = false;
    // any stage-2/3 option on: the producer takes the surface path of ui3d_groups.h for non-backdrop draws (else the stage-1 path, unchanged)
    bool reinterpret() const noexcept { return groups || bevel > 0.0f || text_mm > 0.0f || sel_raise_mm > 0.0f || back_alpha > 0.0f; }
};
namespace detail {
inline bool num(const std::string& v, double lo, double hi, double& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end || !std::isfinite(d) || d < lo || d > hi) return false;
    out = d;
    return true;
}
} // namespace detail
// Removes every ui3d token from `value` (the first item, the stage id, is never an option); a value without them comes back byte for byte.
inline bool extract_options(const std::string& value, std::string& rest, Options& o, std::string& why) {
    o = Options{};
    rest.clear();
    size_t pos = 0; bool first = true;
    bool given[4] = {false, false, false, false};   // u3bv, u3tx, u3sr, u3pb given explicitly (the u3gp level does not override them)
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        bool ours = false;
        if (!first) {
            const size_t eq = item.find_first_of(":=");
            if (eq != std::string::npos) {
                const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
                double d = 0;
                ours = true;
                if (k == "ui3d") { if (v == "1") o.on = true; else if (v == "0") o.on = false; else { why = "bad value for ui3d (0|1)"; return false; } }
                else if (k == "u3n") { if (!detail::num(v, 0.5, 10.0, d)) { why = "bad value for u3n (0.5..10 m)"; return false; } o.near_m = float(d); }
                else if (k == "u3f") { if (!detail::num(v, 0.6, 30.0, d)) { why = "bad value for u3f (0.6..30 m)"; return false; } o.far_m = float(d); }
                else if (k == "u3t") { if (!detail::num(v, -30.0, 30.0, d)) { why = "bad value for u3t (-30..30 deg)"; return false; } o.tilt_deg = float(d); }
                else if (k == "u3c") { if (v == "1") o.curve = true; else if (v == "0") o.curve = false; else { why = "bad value for u3c (0|1)"; return false; } }
                else if (k == "u3p") { if (v == "1") o.pause = true; else if (v == "0") o.pause = false; else { why = "bad value for u3p (0|1)"; return false; } }
                else if (k == "u3a") { if (v == "0" || v == "1" || v == "2") o.hangar = unsigned(v[0] - '0'); else { why = "bad value for u3a (0|1|2)"; return false; } }
                else if (k == "u3au") { if (!detail::num(v, 1.0, 200.0, d)) { why = "bad value for u3au (1..200 units/m)"; return false; } o.hangar_upm = float(d); }
                else if (k == "u3mn") { if (!detail::num(v, 0.5, 10.0, d)) { why = "bad value for u3mn (0.5..10 m)"; return false; } o.menu_near_m = float(d); }
                else if (k == "u3mf") { if (!detail::num(v, 0.6, 30.0, d)) { why = "bad value for u3mf (0.6..30 m)"; return false; } o.menu_far_m = float(d); }
                else if (k == "u3s") { if (!detail::num(v, 0.2, 1.5, d)) { why = "bad value for u3s (0.2..1.5)"; return false; } o.scale = float(d); }
                else if (k == "u3q") { if (v == "0" || v == "1") o.layering = unsigned(v[0] - '0'); else { why = "bad value for u3q (0|1)"; return false; } }
                else if (k == "u3r") { if (v == "0" || v == "1" || v == "2") o.filter = unsigned(v[0] - '0'); else { why = "bad value for u3r (0|1|2)"; return false; } }
                else if (k == "u3k") { if (!detail::num(v, 0.0, 1.0, d)) { why = "bad value for u3k (0..1)"; return false; } o.sharpen = float(d); }
                else if (k == "u3sel") { if (v == "0" || v == "1" || v == "2") o.select = unsigned(v[0] - '0'); else { why = "bad value for u3sel (0|1|2)"; return false; } }
                else if (k == "u3sg") { if (!detail::num(v, 0.0, 50.0, d)) { why = "bad value for u3sg (0..50 mm)"; return false; } o.sel_gap_mm = float(d); }
                else if (k == "u3sd") { if (!detail::num(v, 1.0, 100.0, d)) { why = "bad value for u3sd (1..100 mm)"; return false; } o.sel_depth_mm = float(d); }
                else if (k == "u3fs") { if (!detail::num(v, 1.0, 3.0, d)) { why = "bad value for u3fs (1..3)"; return false; } o.far_scale = float(d); }
                else if (k == "u3at") { if (v == "1") o.hangar_tint = true; else if (v == "0") o.hangar_tint = false; else { why = "bad value for u3at (0|1)"; return false; } }
                else if (k == "u3ag") { if (v == "1") o.hangar_glow = true; else if (v == "0") o.hangar_glow = false; else { why = "bad value for u3ag (0|1)"; return false; } }
                else if (k == "u3agm") { if (v == "0" || v == "1" || v == "2") o.glow_memory = unsigned(v[0] - '0'); else { why = "bad value for u3agm (0|1|2)"; return false; } }
                else if (k == "u3pp") { if (v == "1") o.panel_plane = true; else if (v == "0") o.panel_plane = false; else { why = "bad value for u3pp (0|1)"; return false; } }
                else if (k == "u3lt") { if (!detail::num(v, 0.0, 50.0, d)) { why = "bad value for u3lt (0..50 mm)"; return false; } o.frame_mm = float(d); }
                else if (k == "u3gp") { if (v.size() == 1 && v[0] >= '0' && v[0] <= '3') { o.look = unsigned(v[0] - '0'); o.groups = o.look >= 1; } else { why = "bad value for u3gp (0..3)"; return false; } }
                else if (k == "u3gk") { if (!detail::num(v, 0.0, 1.0, d)) { why = "bad value for u3gk (0..1)"; return false; } o.curve_k = float(d); }
                else if (k == "u3gn") { if (!detail::num(v, 0.0, 20.0, d)) { why = "bad value for u3gn (0..20 mm)"; return false; } o.nest_mm = float(d); }
                else if (k == "u3bv") { if (!detail::num(v, 0.0, 0.9, d)) { why = "bad value for u3bv (0..0.9)"; return false; } o.bevel = float(d); given[0] = true; }
                else if (k == "u3tx") { if (!detail::num(v, 0.0, 20.0, d)) { why = "bad value for u3tx (0..20 mm)"; return false; } o.text_mm = float(d); given[1] = true; }
                else if (k == "u3sr") { if (!detail::num(v, 0.0, 50.0, d)) { why = "bad value for u3sr (0..50 mm)"; return false; } o.sel_raise_mm = float(d); given[2] = true; }
                else if (k == "u3pb") { if (!detail::num(v, 0.0, 1.0, d)) { why = "bad value for u3pb (0..1)"; return false; } o.back_alpha = float(d); given[3] = true; }
                else ours = false;
                if (ours) o.any_token = true;
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    // u3gp levels 2 / 3: the stage-3 defaults for every knob that was not given explicitly
    if (o.look >= 2) {
        if (!given[0]) o.bevel = 0.4f;
        if (!given[1]) o.text_mm = 3.0f;
        if (!given[2]) o.sel_raise_mm = 8.0f;
        if (o.look >= 3 && !given[3]) o.back_alpha = 0.35f;
    }
    if (!(o.near_m < o.far_m)) { why = "u3n must be smaller than u3f"; return false; }
    if (!(o.menu_near_m < o.menu_far_m)) { why = "u3mn must be smaller than u3mf"; return false; }
    return true;
}
// nullptr = can run in this configuration; else the missing prerequisite (logged once, the option then does nothing).
inline const char* prerequisites(const Options& o, bool vscreen_on, bool vsearly_on, bool hud_layer) noexcept {
    if (!o.on) return "off";
    if (!vscreen_on) return "needs-vscreen:1";
    if (!vsearly_on) return "needs-vsearly:1";
    if (!hud_layer) return "needs-xr_hud_layer";
    return nullptr;
}
inline std::string describe(const Options& o) {
    char t[480], h[48] = "", x[64] = "", y[96] = "", g[32] = "", m[48] = "";
    if (o.hangar) std::snprintf(h, sizeof h, ";u3a=%u;u3au=%.1f", o.hangar, double(o.hangar_upm));   // u3a only when on (stage 3b)
    if (o.layering != 1 || o.filter != 2 || o.sharpen != 0.35f)   // 2026-10-30b: shown only when changed (the default line is byte-identical to before)
        std::snprintf(x, sizeof x, ";u3q=%u;u3r=%u;u3k=%.2f", o.layering, o.filter, double(o.sharpen));
    if (o.select != 2 || o.sel_gap_mm != 4.0f || o.sel_depth_mm != 12.0f || o.far_scale != 1.0f)   // 2026-10-30c: shown only when changed
        std::snprintf(y, sizeof y, ";u3sel=%u;u3sg=%.1f;u3sd=%.1f;u3fs=%.2f", o.select, double(o.sel_gap_mm), double(o.sel_depth_mm), double(o.far_scale));
    if (o.hangar && (o.hangar_tint || !o.hangar_glow)) std::snprintf(g, sizeof g, ";u3at=%u;u3ag=%u", o.hangar_tint ? 1u : 0u, o.hangar_glow ? 1u : 0u);
    // 2026-10-30d: shown only when changed (the default line stays byte-identical); u3agm only with u3a
    {
        int k = 0;
        if (o.panel_plane || o.frame_mm != 0.0f) k = std::snprintf(m, sizeof m, ";u3pp=%u;u3lt=%.1f", o.panel_plane ? 1u : 0u, double(o.frame_mm));
        if (o.hangar && o.glow_memory != 1 && k >= 0 && size_t(k) < sizeof m) std::snprintf(m + k, sizeof m - size_t(k), ";u3agm=%u", o.glow_memory);
    }
    // 2026-10-31 (stages 2-3): shown only when one of them is set (the default line stays byte-identical)
    char s23[112] = "";
    if (o.groups || o.curve_k != 1.0f || o.nest_mm != 5.0f || o.reinterpret())
        std::snprintf(s23, sizeof s23, ";u3gp=%u;u3gk=%.2f;u3gn=%.1f;u3bv=%.2f;u3tx=%.1f;u3sr=%.1f;u3pb=%.2f", o.look, double(o.curve_k),
                      double(o.nest_mm), double(o.bevel), double(o.text_mm), double(o.sel_raise_mm), double(o.back_alpha));
    std::snprintf(t, sizeof t, "ui3d=%u;u3n=%.2f;u3f=%.2f;u3t=%.1f;u3c=%u%s%s;u3mn=%.2f;u3mf=%.2f;u3s=%.2f%s%s%s%s%s", o.on ? 1u : 0u, double(o.near_m), double(o.far_m),
                  double(o.tilt_deg), o.curve ? 1u : 0u, o.pause ? ";u3p=1" : "", h, double(o.menu_near_m), double(o.menu_far_m), double(o.scale), x, y, g, m, s23);
    return t;
}
// Layer distances: evenly spaced in dioptres (vergence) from the far to the near distance. k = 0 far (backdrop) .. kLayers-1 near (text).
inline double layer_distance(const Options& o, unsigned k) noexcept {
    const double a = 1.0 / double(o.far_m), b = 1.0 / double(o.near_m);
    const double t = kLayers > 1 ? double(std::min(k, kLayers - 1)) / double(kLayers - 1) : 0.0;
    return 1.0 / (a + t * (b - a));
}

// ================================================================================================== source side (the game's GS coordinates)
// cb0 of the ps2_v draw (root constants / CBV, read by the glue): X_ndc = (x - ox) * 2 / sx, Y_ndc = (y - oy) * 2 / sy.
struct Cb0 { float sx = 0, sy = 0, ox = 0, oy = 0; };
inline bool cb0_ok(const Cb0& c) noexcept {
    return std::isfinite(c.sx) && std::isfinite(c.sy) && std::isfinite(c.ox) && std::isfinite(c.oy) && std::fabs(c.sx) > 1e-3f && std::fabs(c.sy) > 1e-3f;
}
// The game's viewport as a rectangle of the IMAGE SHOWN ON THE FLAT SCREEN, normalized: x0, y0 = top-left (0..1, y down), w, h = size.
// For a draw straight into the back buffer it is the viewport / back-buffer size; a pillarboxed 4:3 viewport in 16:9 is {0.125, 0, 0.75, 1}.
struct SourceRect { double x0 = 0, y0 = 0, w = 1, h = 1; };
inline bool rect_ok(const SourceRect& r) noexcept {
    return std::isfinite(r.x0 + r.y0 + r.w + r.h) && r.w > 1e-4 && r.h > 1e-4 && r.x0 > -0.01 && r.y0 > -0.01 && r.x0 + r.w < 1.01 && r.y0 + r.h < 1.01;
}
// GS x, y -> flat-screen coordinates u (right), v (up), both -1..1 over the whole shown image.
inline void gs_to_screen(double x, double y, const Cb0& c, const SourceRect& r, double& u, double& v) noexcept {
    const double X = (x - double(c.ox)) * 2.0 / double(c.sx), Y = (y - double(c.oy)) * 2.0 / double(c.sy);
    const double px = r.x0 + (X + 1.0) * 0.5 * r.w, py = r.y0 + (1.0 - Y) * 0.5 * r.h;
    u = 2.0 * px - 1.0;
    v = 1.0 - 2.0 * py;
}

// ================================================================================================== the flat screen and the layer surfaces (LOCAL space)
struct Screen {
    bool valid = false;
    double c[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};
    double half_w = 0, half_h = 0;
    double anchor[3] = {0, 0, 0};   // the eye position the layers are aligned for (the head at placement)
    double dist = 0;                // anchor -> screen plane along fwd
};
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// The vscreen placement (centre, yaw about +Y, width, height; acz::vscreen::Placement) and the head position it was placed from.
inline Screen make_screen(const float centre[3], float yaw, float width, float height, const double anchor[3]) noexcept {
    Screen s;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(centre[i]) || !std::isfinite(anchor[i])) return s;
    if (!std::isfinite(yaw) || !(width > 0.01f) || !(height > 0.01f)) return s;
    const double cy = std::cos(double(yaw)), sy = std::sin(double(yaw));
    s.right[0] = cy; s.right[1] = 0; s.right[2] = -sy;   // R_y(yaw) (1,0,0)
    s.fwd[0] = -sy;  s.fwd[1] = 0;   s.fwd[2] = -cy;     // R_y(yaw) (0,0,-1): vscreen::place's forward
    for (int i = 0; i < 3; ++i) { s.c[i] = centre[i]; s.anchor[i] = anchor[i]; }
    s.half_w = 0.5 * double(width); s.half_h = 0.5 * double(height);
    double d[3] = {s.c[0] - s.anchor[0], s.c[1] - s.anchor[1], s.c[2] - s.anchor[2]};
    s.dist = dot3(d, s.fwd);
    s.valid = s.dist > 0.1;
    return s;
}
// The same screen plane and anchor with the rectangle shrunk (or grown) about its centre: every layer keeps the direction of each point of the
// scaled rectangle, so the panel's angular size is ~k times the vscreen's (u3s). A bad k makes the screen invalid.
inline Screen scaled(const Screen& s, double k) noexcept {
    Screen o = s;
    if (!(k > 0.0) || !std::isfinite(k)) { o.valid = false; return o; }
    o.half_w *= k; o.half_h *= k;
    return o;
}
inline void screen_point(const Screen& s, double u, double v, double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = s.c[i] + u * s.half_w * s.right[i] + v * s.half_h * s.up[i];
}
// The point of a surface at distance D (the layers' plane / cylinder rule: tilt and curve from `o`) on the ray anchor -> screen(u * k, v * k).
// k = 1 keeps the direction of the flat screen's point; k > 1 spreads the surface over a larger angle about the screen centre (u3fs). nullptr = written.
// 2026-10-31 (stage 2): a placement SURFACE as a value, so a draw can be put on a surface other than its layer's (a panel group's plane).
// Plane: dot(P - anchor, n) = D * ct (n unit; for the layer planes n = fwd tilted by u3t and ct = cos(tilt): the plane through anchor + D fwd);
// cylinder (u3c:1): radius D about the anchor's vertical axis. k = the angular scale (screen(u k, v k)). Every surface point lies on the ray
// anchor -> screen(u k, v k), so from the anchor every surface shows the flat screen's directions. The layer surfaces are computed with exactly
// the arithmetic of the stage-1 surface_point (byte-identical vertices).
struct Surf {
    bool cyl = false;
    double n[3] = {0, 0, -1};
    double D = 0, ct = 1, k = 1;
};
inline Surf layer_surf(const Screen& s, const Options& o, double D, double k) noexcept {
    Surf f; f.D = D; f.k = k; f.cyl = o.curve;
    if (!o.curve) {
        const double tr = double(o.tilt_deg) * 3.14159265358979323846 / 180.0;
        for (int i = 0; i < 3; ++i) f.n[i] = std::cos(tr) * s.fwd[i] - std::sin(tr) * s.up[i];   // fwd tilted so the top of the plane recedes
        f.ct = std::cos(tr);
    }
    return f;
}
// The same surface (normal, tilt, scale) at distance D from the anchor (for the cylinder: radius D).
inline Surf surf_at(const Surf& f, double D) noexcept { Surf g = f; g.D = D; return g; }
inline const char* surf_point(const Screen& s, const Surf& f, double u, double v, double out[3]) noexcept {
    if (!s.valid) return "ui3d-screen-invalid";
    const double D = f.D, k = f.k;
    if (!(D > 0) || !std::isfinite(D) || !(k > 0) || !std::isfinite(k)) return "ui3d-surface-invalid";
    double S[3]; screen_point(s, u * k, v * k, S);
    const double dir[3] = {S[0] - s.anchor[0], S[1] - s.anchor[1], S[2] - s.anchor[2]};
    double t;
    if (f.cyl) {
        const double a = dot3(dir, s.right), b = dot3(dir, s.fwd);
        const double hl = std::sqrt(a * a + b * b);
        if (!(hl > 1e-6) || !(b > 0)) return "ui3d-layer-ray-parallel";
        t = D / hl;
    } else {
        const double den = dot3(dir, f.n);
        if (!(den > 1e-6)) return "ui3d-layer-ray-parallel";
        t = D * f.ct / den;
    }
    if (!(t > 0) || !std::isfinite(t)) return "ui3d-layer-ray-parallel";
    for (int i = 0; i < 3; ++i) out[i] = s.anchor[i] + t * dir[i];
    return nullptr;
}
inline const char* surface_point(const Screen& s, const Options& o, double D, double k, double u, double v, double out[3]) noexcept {
    return surf_point(s, layer_surf(s, o, D, k), u, v, out);
}
// The angular scale of layer k: the backdrop (k = 0) is spread by u3fs (default 1: every layer keeps the flat screen's directions).
inline double layer_scale(const Options& o, unsigned k) noexcept { return k == 0 ? double(o.far_scale) : 1.0; }
// The point of layer k on the ray anchor -> screen(u, v) (for the backdrop: screen(u * u3fs, v * u3fs)). nullptr = written.
inline const char* layer_point(const Screen& s, const Options& o, unsigned k, double u, double v, double out[3]) noexcept {
    return surface_point(s, o, layer_distance(o, k), layer_scale(o, k), u, v, out);
}

// ================================================================================================== the eye side
// One located view: LOCAL position, orientation (x, y, z, w) and the FOV angles in radians (left < 0 < right, down < 0 < up).
struct EyeView { double p[3] = {0, 0, 0}; double q[4] = {0, 0, 0, 1}; double fov[4] = {-0.785, 0.785, 0.785, -0.785}; };
// v' = conj(q) v q (LOCAL -> view).
inline void to_view(const EyeView& e, const double P[3], double V[3]) noexcept {
    const double x = -e.q[0], y = -e.q[1], z = -e.q[2], w = e.q[3];
    const double a[3] = {P[0] - e.p[0], P[1] - e.p[1], P[2] - e.p[2]};
    // t = 2 (q.xyz x a); v = a + w t + q.xyz x t
    const double t[3] = {2 * (y * a[2] - z * a[1]), 2 * (z * a[0] - x * a[2]), 2 * (x * a[1] - y * a[0])};
    V[0] = a[0] + w * t[0] + (y * t[2] - z * t[1]);
    V[1] = a[1] + w * t[1] + (z * t[0] - x * t[2]);
    V[2] = a[2] + w * t[2] + (x * t[1] - y * t[0]);
}
// LOCAL point -> eye NDC (x right, y up) and w = distance along the view axis. false = behind / nearer than kNearM or a bad FOV.
inline bool project(const EyeView& e, const double P[3], double& ndc_x, double& ndc_y, double& w) noexcept {
    const double tl = std::tan(e.fov[0]), tr = std::tan(e.fov[1]), tu = std::tan(e.fov[2]), td = std::tan(e.fov[3]);
    if (!std::isfinite(tl + tr + tu + td) || !(tr - tl > 1e-6) || !(tu - td > 1e-6)) return false;
    double V[3]; to_view(e, P, V);
    w = -V[2];
    if (!(w >= kNearM) || !std::isfinite(w)) return false;
    ndc_x = (2.0 * (V[0] / w) - (tr + tl)) / (tr - tl);
    ndc_y = (2.0 * (V[1] / w) - (tu + td)) / (tu - td);
    return std::isfinite(ndc_x) && std::isfinite(ndc_y);
}

// ================================================================================================== one draw's vertex copy for one eye
struct RewriteResult { const char* why = nullptr; bool culled = false; };
// src: the draw's `n` vertices (64 B each, as the game's VB holds them); dst: the same layout for the eye replay (the glue's upload span).
// Bytes 8..11 (z) and 16..63 (colours, texcoord) are copied unchanged; x, y, q are rewritten. The eye's viewport is the whole eye image and the
// replay keeps the game's cb0, so x' = ox + ndc_x sx / 2 (the shader's own mapping inverted). why != nullptr = the draw is unusable (the frame is
// refused, fail closed); culled = a vertex is behind / too near this eye: the glue records NO draw for this eye (the others are unaffected).
// rewrite_at: the same for an explicit surface (distance D from the anchor, angular scale k; surface_point); rewrite_vertices = layer `layer`.
// `cull` (optional, n bytes; DEBRIEF-3D, review 2026-10-31 F2): cull[i] = 1 when vertex i is the culled one (behind / too near / unusable after the
// rewrite), else 0, so a caller can drop only the primitives that use such a vertex instead of the whole draw.
inline RewriteResult rewrite_surf(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Surf& sf,
                                  const EyeView& eye, uint8_t* dst, uint8_t* cull = nullptr) noexcept {
    RewriteResult res;
    if (!src || !dst || n == 0) { res.why = "ui3d-no-vertices"; return res; }
    if (n > kMaxVerticesPerDraw) { res.why = "ui3d-draw-too-many-vertices"; return res; }
    if (!cb0_ok(cb)) { res.why = "ui3d-cb0"; return res; }
    if (!rect_ok(r)) { res.why = "ui3d-viewport"; return res; }
    if (!s.valid) { res.why = "ui3d-screen-invalid"; return res; }
    std::memcpy(dst, src, size_t(n) * kVertexStride);
    for (uint32_t i = 0; i < n; ++i) {
        float f[4]; std::memcpy(f, src + size_t(i) * kVertexStride, 16);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3])) { res.why = "ui3d-vertex-nonfinite"; return res; }
        if (!(f[3] > 0.0f)) { res.why = "ui3d-vertex-q"; return res; }
        double u, v; gs_to_screen(f[0], f[1], cb, r, u, v);
        double P[3];
        if (const char* why = surf_point(s, sf, u, v, P)) { res.why = why; return res; }
        double nx, ny, w;
        if (cull) cull[i] = 0;
        if (!project(eye, P, nx, ny, w)) { res.culled = true; if (cull) cull[i] = 1; continue; }
        const float x2 = float(double(cb.ox) + nx * double(cb.sx) * 0.5), y2 = float(double(cb.oy) + ny * double(cb.sy) * 0.5);
        const double A[3] = {P[0] - s.anchor[0], P[1] - s.anchor[1], P[2] - s.anchor[2]};
        const double wa = dot3(A, s.fwd);   // the layer point's depth seen from the anchor (the view the source image is affine in)
        const float q2 = float(double(f[3]) * wa / w);   // perspective-correct attributes on the layer (ui-3d.md 3.3): q' = q w_anchor / w_eye
        if (!std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(q2) || !(q2 > 0.0f)) { res.culled = true; if (cull) cull[i] = 1; continue; }
        std::memcpy(dst + size_t(i) * kVertexStride + 0, &x2, 4);
        std::memcpy(dst + size_t(i) * kVertexStride + 4, &y2, 4);
        std::memcpy(dst + size_t(i) * kVertexStride + 12, &q2, 4);
    }
    return res;
}
inline RewriteResult rewrite_at(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Options& o, double D, double k,
                                const EyeView& eye, uint8_t* dst, uint8_t* cull = nullptr) noexcept {
    return rewrite_surf(src, n, cb, r, s, layer_surf(s, o, D, k), eye, dst, cull);
}
inline RewriteResult rewrite_vertices(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Options& o, unsigned layer,
                                      const EyeView& eye, uint8_t* dst) noexcept {
    if (layer >= kLayers) { RewriteResult res; res.why = "ui3d-layer"; return res; }
    return rewrite_at(src, n, cb, r, s, o, layer_distance(o, layer), layer_scale(o, layer), eye, dst);
}

// ================================================================================================== the selection box as a 3D slab (2026-10-30c)
// Headset test 5 item 1 (user, verbatim meaning): "offset the lines is fine, but the selection box could be 3d and closer to the text so i can
// use it in VR". The selection box (a bar-shaped ps2_v quad centred on a text row: SelectionRows below) is replayed as a SLAB whose front face
// lies u3sg behind the TEXT plane and whose back face lies u3sd further: back face, the 4 side walls, front face = 6 quads of 4 vertices, each
// one DrawInstanced of the game's own PSO (a strip of 4). Every face is the game's quad with x, y, q rewritten (rewrite_at) for a point at the
// face's depth; the wall of the edge (a, b) is [front a, back a, front b, back b] with the attributes (colour, texcoord) of a and b, so it shows
// the quad's edge texels as a wall. From the anchor eye the front face covers exactly the flat image's box (directions kept), and the text in
// front of it stays registered to it (a few mm: no visible parallax). The other panels and lines keep their layer depth (user: "offset the
// lines is fine").
inline constexpr unsigned kSlabCopies = 6;
inline constexpr unsigned kSlabEdges[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};   // the perimeter of a 4-vertex strip (triangles 0,1,2 / 1,2,3)
// dst: kSlabCopies x 4 vertices (back, 4 walls, front). why = unusable (as rewrite_vertices); culled = a face vertex culled: no draw for this eye.
// rewrite_slab_surf: the same between two explicit surfaces (stage 2: a panel group's plane and the same plane further back).
inline RewriteResult rewrite_slab_surf(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Surf& sf_front,
                                       const Surf& sf_back, const EyeView& eye, uint8_t* dst) noexcept {
    RewriteResult res;
    if (n != 4) { res.why = "ui3d-slab-not-quad"; return res; }
    if (!(sf_back.D > sf_front.D)) { res.why = "ui3d-slab-depth"; return res; }
    uint8_t front[4 * kVertexStride], back[4 * kVertexStride];
    const RewriteResult a = rewrite_surf(src, 4, cb, r, s, sf_front, eye, front);
    if (a.why) return a;
    const RewriteResult b = rewrite_surf(src, 4, cb, r, s, sf_back, eye, back);
    if (b.why) return b;
    res.culled = a.culled || b.culled;
    const size_t q = 4 * size_t(kVertexStride);
    std::memcpy(dst, back, q);
    for (unsigned e = 0; e < 4; ++e) {
        const unsigned va = kSlabEdges[e][0], vb = kSlabEdges[e][1];
        uint8_t* face = dst + q * (1 + e);
        std::memcpy(face + 0 * kVertexStride, front + va * kVertexStride, kVertexStride);
        std::memcpy(face + 1 * kVertexStride, back + va * kVertexStride, kVertexStride);
        std::memcpy(face + 2 * kVertexStride, front + vb * kVertexStride, kVertexStride);
        std::memcpy(face + 3 * kVertexStride, back + vb * kVertexStride, kVertexStride);
    }
    std::memcpy(dst + q * 5, front, q);
    return res;
}
inline RewriteResult rewrite_slab(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Options& o, double d_front,
                                  double d_back, const EyeView& eye, uint8_t* dst) noexcept {
    if (n == 4 && d_back > d_front)
        return rewrite_slab_surf(src, n, cb, r, s, layer_surf(s, o, d_front, 1.0), layer_surf(s, o, d_back, 1.0), eye, dst);
    RewriteResult res; res.why = n != 4 ? "ui3d-slab-not-quad" : "ui3d-slab-depth"; return res;
}
// The selection surfaces: front = the text layer's distance + u3sg, back = front + u3sd (u3sel:1: the flat front face only).
inline double selection_front(const Options& o) noexcept { return layer_distance(o, kLayers - 1) + double(o.sel_gap_mm) * 1e-3; }
inline double selection_back(const Options& o) noexcept { return selection_front(o) + double(o.sel_depth_mm) * 1e-3; }

// ================================================================================================== one surface per panel + frame rims (2026-10-30d)
// Headset test 6 item 1 (docs/headset-tests/2026-10-30d-user-report.md; user-shots 10.webp): "the hangar UI is offset from the background
// elements". Cause [S + E]: the classifier puts a panel / frame / tile quad on layer 1 (1.86 m with u3mn 1.1 / u3mf 6.0) and the glyphs on
// layer 2 (1.1 m), and the layer never decreases, so a frame drawn after the first glyph lands on the text plane while its siblings drawn
// before it stay 0.76 m behind. Every layer keeps the anchor's directions, so from the anchor they coincide; from any other head position
// (10.webp: the user looks up and right) parallax moves the outlines, boxes and colour-column squares off the text they belong to.
// Stage 1 of the 3D reinterpretation (ui-3d.md 13.4): u3pp:1 places the panel layer ON the text plane: frame lines, panel fills, tiles,
// selection and text of every panel share one surface (no element of a panel can separate from another with head motion); the backdrop
// (layer 0) keeps its own depth behind. The classifier, its layer numbers and statistics are unchanged; only the placement surface moves.
inline unsigned placement_layer(const Options& o, unsigned classifier_layer) noexcept {
    return o.panel_plane && classifier_layer >= 1 ? kLayers - 1 : classifier_layer;
}
// The frame quads that get 3D thickness (u3lt): one quad (4 vertices), medium (neither small nor large), drawn at the PANEL priority. Census
// [E: step124 runK10 hangar overlay + runD1 menus, ui-3d.md 6 / 12.5]: the panels / frames / tiles are ONE textured quad each with z 64 (the
// outline is in the texture: no thin line quads exist, 3 of 3,371 overlay quads were thin), the glyphs z 1, the full-screen dim z 0.
inline constexpr float kGlyphZ = 1.0f;
inline bool frame_quad(uint32_t vertices, double bbox_w, double bbox_h, bool z_known, float z) noexcept {
    if (vertices != 4 || !(bbox_w > 0) || !(bbox_h > 0) || !std::isfinite(bbox_w) || !std::isfinite(bbox_h)) return false;
    if (bbox_w * bbox_h >= kLargeArea) return false;
    if (bbox_w <= kSmallDim && bbox_h <= kSmallDim) return false;
    return z_known && std::isfinite(z) && z > kGlyphZ;
}
// A RIM: the 4 edge walls of the quad from its surface (d_front) back to d_back, then the front face (the quad itself on its surface) =
// kRimCopies quads of 4 (the slab without its back face: a translucent fill is not doubled). The walls carry the edge vertices' attributes
// (texcoords at the quad's border: the outline texels), so from off the anchor the panel outline reads as a strip with depth.
inline constexpr unsigned kRimCopies = 5;
inline RewriteResult rewrite_rim(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, const Screen& s, const Options& o, double d_front,
                                 double d_back, const EyeView& eye, uint8_t* dst) noexcept {
    uint8_t slab[kSlabCopies * 4 * kVertexStride];
    const RewriteResult res = rewrite_slab(src, n, cb, r, s, o, d_front, d_back, eye, slab);
    if (res.why) return res;
    std::memcpy(dst, slab + 4 * size_t(kVertexStride), kRimCopies * 4 * size_t(kVertexStride));   // walls 1..4, front 5
    return res;
}
// The rim surfaces of a draw placed on placement layer `layer`: its surface and u3lt behind it.
inline double rim_front(const Options& o, unsigned layer) noexcept { return layer_distance(o, layer); }
inline double rim_back(const Options& o, unsigned layer) noexcept { return rim_front(o, layer) + double(o.frame_mm) * 1e-3; }

// A draw's box on the flat screen as fractions of the shown image (x right, y DOWN, 0..1, clipped). false = unusable vertices.
struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double w() const noexcept { return x1 - x0; }
    double h() const noexcept { return y1 - y0; }
};
inline bool screen_box(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, Box& b) noexcept {
    if (!src || n == 0 || !cb0_ok(cb) || !rect_ok(r)) return false;
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (uint32_t i = 0; i < n; ++i) {
        float f[2]; std::memcpy(f, src + size_t(i) * kVertexStride, 8);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1])) return false;
        double u, v; gs_to_screen(f[0], f[1], cb, r, u, v);
        const double x = std::clamp((u + 1.0) * 0.5, 0.0, 1.0), y = std::clamp((1.0 - v) * 0.5, 0.0, 1.0);
        lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x); lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
    }
    b.x0 = lo[0]; b.y0 = lo[1]; b.x1 = hi[0]; b.y1 = hi[1];
    return true;
}
// The text rows of a UI3D frame (its glyph boxes merged per line) and the selection-box rule. No look-ahead exists at a draw (the replay is
// recorded before the game's draw, and the box is drawn BEFORE the text it highlights), so a frame's bars are matched against the PREVIOUS
// frame's rows (menu text is static between frames; a changed screen costs one frame on the old rule).
// Census facts the rule is sized on (ui-3d.md 6.4 and 12.1) [E]: glyphs 0.015 x 0.0536 (rows every 0.067); the main-menu box 0.293 x 0.143 at
// y 0.127 is drawn right before the glyphs and its centre (0.1985) lies on the first text row's centre (0.201); panel strips (0.023 x 0.36..0.6),
// tiles (0.047 x 0.071) and the backdrop are not bars and never match.
inline constexpr unsigned kMaxRows = 64;
inline constexpr double kSelMaxH = 0.16;      // a selection bar is at most this high (fraction of the image; ~3 text rows)
inline constexpr double kSelMinAspect = 1.5;  // and at least this many times as wide as it is high
// its centre within this fraction of the row height of a row's centre: the main-menu box is 0.0025 off row 1 (0.05 rows); the 0.375 x 0.143
// header panel at y 0.109 is 0.0205 off (0.38 rows) and stays on its layer
inline constexpr double kSelCentreTol = 0.33;
class SelectionRows {
public:
    void clear() noexcept { n_ = 0; }
    unsigned size() const noexcept { return n_; }
    // one glyph (a small draw on the text layer) of the current frame
    void add_glyph(const Box& g) noexcept {
        if (!(g.h() > 0) || !(g.w() > 0)) return;
        const double cy = 0.5 * (g.y0 + g.y1);
        for (unsigned i = 0; i < n_; ++i) {
            Box& r = rows_[i];
            const double rc = 0.5 * (r.y0 + r.y1);
            if (std::fabs(cy - rc) <= 0.25 * std::max(r.h(), g.h()) && g.x0 <= r.x1 + 0.05 && g.x1 >= r.x0 - 0.05) {
                r.x0 = std::min(r.x0, g.x0); r.x1 = std::max(r.x1, g.x1); r.y0 = std::min(r.y0, g.y0); r.y1 = std::max(r.y1, g.y1);
                return;
            }
        }
        if (n_ < kMaxRows) rows_[n_++] = g;
    }
    // A bar-shaped draw centred on one of these rows (vertically within kSelCentreTol of a row height, 0.8..3.2 rows high, overlapping it horizontally by more
    // than half of the narrower of the two): the selection box.
    bool selects(const Box& b) const noexcept {
        const double h = b.h(), w = b.w();
        if (!(h > 0) || h > kSelMaxH || w < kSelMinAspect * h) return false;
        const double bc = 0.5 * (b.y0 + b.y1);
        for (unsigned i = 0; i < n_; ++i) {
            const Box& r = rows_[i];
            const double rh = r.h(), rc = 0.5 * (r.y0 + r.y1);
            if (!(rh > 0) || std::fabs(bc - rc) > kSelCentreTol * rh || h < 0.8 * rh || h > 3.2 * rh) continue;
            const double ov = std::min(b.x1, r.x1) - std::max(b.x0, r.x0);
            if (ov > 0.5 * std::min(w, r.w())) return true;
        }
        return false;
    }
private:
    Box rows_[kMaxRows];
    unsigned n_ = 0;
};
// The draw's bounding box on the flat screen (fractions of the shown image), for the classifier. false = unusable vertices.
inline bool screen_bbox(const uint8_t* src, uint32_t n, const Cb0& cb, const SourceRect& r, double& w, double& h) noexcept {
    if (!src || n == 0 || !cb0_ok(cb) || !rect_ok(r)) return false;
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (uint32_t i = 0; i < n; ++i) {
        float f[2]; std::memcpy(f, src + size_t(i) * kVertexStride, 8);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1])) return false;
        double u, v; gs_to_screen(f[0], f[1], cb, r, u, v);
        lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u);
        lo[1] = std::min(lo[1], v); hi[1] = std::max(hi[1], v);
    }
    // clip to the shown image (-1..1), then fractions of it
    for (int a = 0; a < 2; ++a) { lo[a] = std::max(lo[a], -1.0); hi[a] = std::min(hi[a], 1.0); }
    w = std::max(0.0, hi[0] - lo[0]) * 0.5;
    h = std::max(0.0, hi[1] - lo[1]) * 0.5;
    return true;
}

// The game's viewport as the SourceRect of the shown image when the UI target IS the shown image (census: the menu target 2560 x 1440, viewport
// 0,0,2560,1440, the 4:3 pillarbox comes from cb0 (682, -448, 2048, 2048) and the scissor 320..2240, not from the viewport) [E].
inline SourceRect rect_from_viewport(double x, double y, double w, double h, double rt_w, double rt_h) noexcept {
    SourceRect r;
    if (!(rt_w > 0) || !(rt_h > 0)) { r.w = r.h = 0; return r; }
    r.x0 = x / rt_w; r.y0 = y / rt_h; r.w = w / rt_w; r.h = h / rt_h;
    return r;
}
// The game's scissor rectangle (RT pixels, D3D12_RECT l, t, r, b) carried onto layer `layer` and projected into one eye image (pixels): the
// bounding box of 9 samples per edge (exact for planes: straight edges; conservative within a sample spacing on the cylinder), clamped to the
// eye image. Census [E]: the menus scissor 320..2240 of 2560 (the 4:3 area) and the first backdrop quads overshoot it (s=1: 1.04 x 1.2 of the
// image): without the scissor they would spill outside the screen. false = the rectangle is empty in this eye (no draw for it). A sample
// behind / too near the eye gives the whole eye image (the vertices themselves cull such a draw).
struct PixRect { int32_t l = 0, t = 0, r = 0, b = 0; };
inline bool eye_scissor_surf(const int32_t sc[4], double rt_w, double rt_h, const Screen& s, const Surf& sf, const EyeView& eye,
                             uint32_t eye_w, uint32_t eye_h, PixRect& out) noexcept {
    out = PixRect{};
    if (!sc || !(rt_w > 0) || !(rt_h > 0) || !eye_w || !eye_h || !(sc[2] > sc[0]) || !(sc[3] > sc[1])) return false;
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    bool whole = false;
    constexpr int kSamples = 9;
    for (int edge = 0; edge < 4 && !whole; ++edge)
        for (int i = 0; i < kSamples; ++i) {
            const double t = double(i) / double(kSamples - 1);
            double px, py;
            switch (edge) {
            case 0: px = sc[0] + t * (sc[2] - sc[0]); py = sc[1]; break;
            case 1: px = sc[2]; py = sc[1] + t * (sc[3] - sc[1]); break;
            case 2: px = sc[0] + t * (sc[2] - sc[0]); py = sc[3]; break;
            default: px = sc[0]; py = sc[1] + t * (sc[3] - sc[1]); break;
            }
            const double u = 2.0 * px / rt_w - 1.0, v = 1.0 - 2.0 * py / rt_h;
            double P[3], nx, ny, w;
            if (surf_point(s, sf, u, v, P) || !project(eye, P, nx, ny, w)) { whole = true; break; }
            const double x = (nx + 1.0) * 0.5 * double(eye_w), y = (1.0 - ny) * 0.5 * double(eye_h);
            lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x); lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
        }
    if (whole) { out.r = int32_t(eye_w); out.b = int32_t(eye_h); return true; }
    const double W = double(eye_w), H = double(eye_h);
    out.l = int32_t(std::clamp(std::floor(lo[0]), 0.0, W)); out.r = int32_t(std::clamp(std::ceil(hi[0]), 0.0, W));
    out.t = int32_t(std::clamp(std::floor(lo[1]), 0.0, H)); out.b = int32_t(std::clamp(std::ceil(hi[1]), 0.0, H));
    return out.r > out.l && out.b > out.t;
}
inline bool eye_scissor_at(const int32_t sc[4], double rt_w, double rt_h, const Screen& s, const Options& o, double D, double k, const EyeView& eye,
                           uint32_t eye_w, uint32_t eye_h, PixRect& out) noexcept {
    return eye_scissor_surf(sc, rt_w, rt_h, s, layer_surf(s, o, D, k), eye, eye_w, eye_h, out);
}
inline bool eye_scissor(const int32_t sc[4], double rt_w, double rt_h, const Screen& s, const Options& o, unsigned layer, const EyeView& eye,
                        uint32_t eye_w, uint32_t eye_h, PixRect& out) noexcept {
    return eye_scissor_at(sc, rt_w, rt_h, s, o, layer_distance(o, layer), layer_scale(o, layer), eye, eye_w, eye_h, out);
}

// ================================================================================================== the online layer classifier (one game frame)
// Per draw in game order, decided at the draw (no look-ahead: the replay is recorded right before the game's draw).
struct DrawFacts {
    uint32_t vs8 = 0;          // VS creation-hash prefix
    bool on_target = true;     // RT0 is the frame's UI target (the target of the frame's first ps2_v draw); else not replayed
    bool indexed = false;
    uint32_t vertices = 0;
    double bbox_w = 0, bbox_h = 0;   // screen_bbox
    bool z_known = false;      // the draw's GS z (vertex 0, bytes 8..11) was read
    float z = 0.0f;            // census [E]: menu glyphs z = 1, backdrop/panels/decorations z = 64 (the game's own 2D priority)
};
enum : int { kRefused = -1, kSkip = -2 };
// Rules (ui-3d.md 3.2, refined by the census of 2026-10-29, ui-3d.md 6): class by size: large (area >= kLargeArea) -> 0, medium -> 1, small
// (both dims <= kSmallDim) -> text or decoration: with the draw's z known, a small draw whose z differs from the last large/medium draw's z is
// TEXT (2) and one with the same z is a decoration of the current layer (1 after the first medium draw, else 0: the backdrop's particles);
// without a known z the stage-1 rule applies (small after the first medium draw -> 2, before -> 0). A zero-area draw (degenerate quad: the
// census main menu has 14+ per frame, they rasterise nothing) keeps the current layer and never promotes. The layer NEVER decreases within a
// frame (a far draw is never painted over a near one: the painter's order stays consistent with depth). Any non-ps2_v draw on the UI target
// refuses the frame (3D content, movies: other routes).
class LayerAssigner {
public:
    int assign(const DrawFacts& d) noexcept {
        if (refused_) return kRefused;
        if (d.vs8 != kPs2Vs8) {
            if (!d.on_target) { ++skipped_; return kSkip; }
            return refuse(d.vs8 == kSelMapVs8 || d.vs8 == kModelIconVs8 || d.vs8 == kAcmVs8 || d.vs8 == kCptVs8 ? "ui3d-frame-has-3d"
                          : d.vs8 == kMovieVs8 ? "ui3d-frame-has-movie" : "ui3d-frame-has-other-shader");
        }
        if (!d.on_target) { ++skipped_; return kSkip; }
        if (d.indexed) return refuse("ui3d-indexed-draw");
        if (d.vertices == 0 || d.vertices > kMaxVerticesPerDraw) return refuse("ui3d-draw-too-many-vertices");
        if (!std::isfinite(d.bbox_w) || !std::isfinite(d.bbox_h)) return refuse("ui3d-bbox");
        if (draws_ >= kMaxDrawsPerFrame) return refuse("ui3d-too-many-draws");
        ++draws_;
        int cls;
        if (!(d.bbox_w > 0.0) || !(d.bbox_h > 0.0)) cls = layer_;   // degenerate: draws nothing, never promotes
        else if (d.bbox_w * d.bbox_h >= kLargeArea) { cls = 0; note_ref(d); }
        else if (d.bbox_w <= kSmallDim && d.bbox_h <= kSmallDim) {
            if (d.z_known && has_ref_z_) cls = d.z != ref_z_ ? 2 : (seen_medium_ ? 1 : 0);
            else cls = seen_medium_ ? 2 : 0;
        } else { cls = 1; seen_medium_ = true; note_ref(d); }
        layer_ = std::max(layer_, cls);
        ++per_layer_[layer_];
        return layer_;
    }
    bool refused() const noexcept { return refused_ != nullptr; }
    const char* why() const noexcept { return refused_; }
    unsigned draws() const noexcept { return draws_; }
    unsigned skipped() const noexcept { return skipped_; }
    unsigned per_layer(unsigned k) const noexcept { return k < kLayers ? per_layer_[k] : 0; }
    // A frame is worth showing in 3D only if it is not refused and has at least one draw.
    bool usable() const noexcept { return !refused_ && draws_ > 0; }
private:
    int refuse(const char* why) noexcept { refused_ = why; return kRefused; }
    void note_ref(const DrawFacts& d) noexcept { if (d.z_known) { ref_z_ = d.z; has_ref_z_ = true; } }
    const char* refused_ = nullptr;
    int layer_ = 0;
    bool seen_medium_ = false, has_ref_z_ = false;
    float ref_z_ = 0.0f;
    unsigned draws_ = 0, skipped_ = 0;
    unsigned per_layer_[kLayers] = {0, 0, 0};
};

// Upload bytes for one frame's vertex copies (both eyes). The slot span must hold at least this (stage 2 sizes it for the cap).
inline uint64_t upload_bytes(uint32_t draws, uint32_t vertices_per_draw) noexcept { return uint64_t(draws) * 2u * vertices_per_draw * kVertexStride; }
inline constexpr uint64_t kUploadPerSlot = uint64_t(kMaxDrawsPerFrame) * 2u * 4u * kVertexStride;   // 512 KiB: the cap with 4-vertex quads

// ================================================================================================== statistics line (bounded)
struct Stats { uint64_t frames = 0, usable = 0, refused = 0, draws = 0, culled = 0, layer[kLayers] = {0, 0, 0}; const char* last_why = nullptr; };
inline void count_frame(Stats& st, const LayerAssigner& a, uint64_t culled) noexcept {
    ++st.frames;
    if (a.refused()) { ++st.refused; st.last_why = a.why(); return; }
    if (!a.usable()) return;
    ++st.usable; st.draws += a.draws(); st.culled += culled;
    for (unsigned k = 0; k < kLayers; ++k) st.layer[k] += a.per_layer(k);
}
inline std::string summary_line(const Stats& st) {
    char t[256];
    auto c = [](uint64_t v) { return (unsigned long long)std::min<uint64_t>(v, 9999999999ull); };
    std::snprintf(t, sizeof t, "summary-ui3d;frames=%llu;usable=%llu;refused=%llu;draws=%llu;culled=%llu;far=%llu;mid=%llu;near=%llu;last-why=%.40s",
                  c(st.frames), c(st.usable), c(st.refused), c(st.draws), c(st.culled), c(st.layer[0]), c(st.layer[1]), c(st.layer[2]),
                  st.last_why ? st.last_why : "none");
    return t;
}

} // namespace acz::ui3d
