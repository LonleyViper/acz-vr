// tactical_offline_tests.cpp -- TACTICAL (xr_terrain_stage option tac:1) CPU-reference cases (no GPU, no game): option extraction and key-off identity,
// the producer helpers (marker-sprite clustering, label glyph counts, marker <-> world-draw matching through the frame's A matrix, the firing-wedge key),
// the tracker (clusters, association, team learning, coasting), radar pixels at the measured widget, world-anchored markers for a head turned 40 deg
// off the game window, the firing debounce on synthetic draw logs, the cosmetic tracers and the no-double-marker rule. docs/world-render/tactical.md.
#include "tactical.h"
#include "tactical_producer.h"
#include "world_props.h"
#include "xr_stereo_output.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::tac;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// A world -> PS2 GS screen matrix (row vectors, h = (p, 1) x A) for a camera at C with axes right/up/fwd and lens tangents tx/ty, built like the game's
// record +0 (gpu_copy_step5_world.h: x_s = 258.048 x_c + 2048 w, y_s = 225.792 y_c + 2048 w, w = view depth). Screen y grows downward (y_c = -up).
static void make_A(const float C[3], const float r[3], const float u[3], const float f[3], float tx, float ty, float A[16]) {
    float vp[4][4] = {};   // columns x_c, y_c, z (unused), w ; rows x, y, z, translation
    for (int i = 0; i < 3; ++i) { vp[i][0] = r[i] / tx; vp[i][1] = -u[i] / ty; vp[i][3] = f[i]; }
    float dr = 0, du = 0, df = 0;
    for (int i = 0; i < 3; ++i) { dr += r[i] * C[i]; du += u[i] * C[i]; df += f[i] * C[i]; }
    vp[3][0] = -dr / tx; vp[3][1] = du / ty; vp[3][3] = -df;
    for (int row = 0; row < 4; ++row) {
        A[row * 4 + 0] = 258.048f * vp[row][0] + 2048.0f * vp[row][3];
        A[row * 4 + 1] = 225.792f * vp[row][1] + 2048.0f * vp[row][3];
        A[row * 4 + 2] = 0.0f;
        A[row * 4 + 3] = vp[row][3];
    }
}

static void test_options() {
    Options o; std::string rest, why;
    // key off: the value is untouched (the props/terrain parsers see exactly the old string)
    CHECK(extract_options("m02a;env:1;look:1;props:1;trees:1;propdedupe:1;e6x:1", rest, o, why));
    CHECK(rest == "m02a;env:1;look:1;props:1;trees:1;propdedupe:1;e6x:1"); CHECK(!o.on);
    CHECK(extract_options("auto", rest, o, why)); CHECK(rest == "auto"); CHECK(!o.on);
    CHECK(extract_options("m02a;tac:1;props:1;tacr=8000;tacgun:0;tacsup:0", rest, o, why));
    CHECK(rest == "m02a;props:1"); CHECK(o.on); CHECK(near(o.range_ring3, 8000, 0)); CHECK(!o.tracers); CHECK(!o.suppress);
    // D-355: our boxes and blips are OFF by default (the game's original markers/radar); tac:1 alone = tracers only, nothing suppressed
    CHECK(!o.radar && !o.markers);
    CHECK(!Options{}.radar && !Options{}.markers && Options{}.tracers && !Options{}.on);
    CHECK(extract_options("auto;env:1;tac:1;interp:1", rest, o, why) && rest == "auto;env:1;interp:1" && o.on && o.tracers && !o.radar && !o.markers && o.suppress);
    CHECK(producer_flags_for(o) == kProducerOn);                                       // collect facts, never suppress a game marker
    CHECK(describe(o).find("radar=0;markers=0;tracers=1;") != std::string::npos);
    CHECK(extract_options("m02a;tac:1;tacboxes:1;tacradar:1", rest, o, why) && rest == "m02a" && o.markers && o.radar);
    CHECK(producer_flags_for(o) == (kProducerOn | kProducerSuppress));
    CHECK(extract_options("m02a;tac:1;tacmark:1;tacsup:0", rest, o, why) && o.markers && !o.suppress && producer_flags_for(o) == kProducerOn); // old alias
    CHECK(extract_options("m02a;tac:1;tacboxes:0;tacradar:0;tacgun:0", rest, o, why) && rest == "m02a" && !o.markers && !o.radar && !o.tracers);
    CHECK(!extract_options("m02a;tac:1;tacboxes:2", rest, o, why) && why == "bad value for tacboxes");
    // tac off: no producer flags whatever the sub-keys say (nothing of it draws or touches the game's markers)
    CHECK(extract_options("m02a;tacboxes:1;tacradar:1", rest, o, why) && !o.on && producer_flags_for(o) == 0u);
    CHECK(producer_flags_for(Options{}) == 0u);
    CHECK(!extract_options("m02a;tac:2", rest, o, why)); CHECK(why == "bad value for tac");
    CHECK(!extract_options("m02a;tac:1;tacr:10", rest, o, why));
    // the first item is the stage id, never an option (a stage called "tac:1" is not parsed)
    CHECK(extract_options("tac:1", rest, o, why)); CHECK(!o.on); CHECK(rest == "tac:1");
    CHECK(producer_flags() == 0);   // nobody armed the producer: the key-off producer path is never entered
}

static void put_quad(SpriteScratch& sc, unsigned& n, float x0, float y0, float x1, float y1, const float rgb[3]) {
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (int k = 0; k < 4; ++k) { SpriteVert& v = sc.v[n++]; v.x = xs[k]; v.y = ys[k]; v.rgba[0] = rgb[0]; v.rgba[1] = rgb[1]; v.rgba[2] = rgb[2]; v.rgba[3] = 1; }
}

static void test_producer_helpers() {
    static SpriteScratch sc;
    static TacFrame f;
    // camera at the origin looking north (-Z), lens of the game
    const float C[3] = {0, 4000, 0}, r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, fw[3] = {0, 0, -1};
    make_A(C, r, u, fw, 0.6152f, 0.3460f, f.A);
    f.a_valid = true; f.nobj = 0; f.nmark = 0;
    // three aircraft (world draws) ahead, one a two-part object
    const float P[4][3] = {{-600, 4300, -5000}, {500, 4200, -4800}, {505, 4200, -4790}, {1500, 4500, -6000}};
    for (const auto& p : P) { ObjDraw& o = f.obj[f.nobj++]; for (int k = 0; k < 3; ++k) o.t[k] = p[k]; }
    float xs[4], ys[4], w;
    for (int i = 0; i < 4; ++i) CHECK(project_ps2(f.A, P[i], xs[i], ys[i], w));
    CHECK(xs[0] < 2048 && xs[1] > 2048 && ys[0] < 2048);   // left object left of centre, above the horizon (screen y down)
    // the box draw: one 15-unit square (4 corners, plus an inner second outline) around each of objects 0, 1, 3 (blue), as one draw
    const float blue[3] = {0.35f, 0.43f, 0.9f};
    unsigned n = 0;
    for (int i : {0, 1, 3}) { put_quad(sc, n, xs[i] - 7.5f, ys[i] - 7.5f, xs[i] + 7.5f, ys[i] + 7.5f, blue); put_quad(sc, n, xs[i] - 6.5f, ys[i] - 6.5f, xs[i] + 6.5f, ys[i] + 6.5f, blue); }
    uint32_t dropped = 0;
    unsigned nc = cluster_vertices(sc, n, kBoxLink, dropped);
    CHECK(nc == 3); CHECK(dropped == 0);
    add_boxes(f, sc.c, nc);
    CHECK(f.nmark == 3);
    for (unsigned k = 0; k < 3; ++k) { CHECK(near(f.mark[k].hw, 7.5, 1e-3)); CHECK(near(f.mark[k].rgb[2], 1.0, 1e-6)); CHECK(f.mark[k].verts == 8); }
    CHECK(near(f.mark[0].cx, xs[0], 1e-3) && near(f.mark[0].cy, ys[0], 1e-3));
    // the label draw: "FRND" (4 glyph quads) above-left of box 0, "TGT" (3) above-left of box 2
    n = 0;
    for (int g = 0; g < 4; ++g) put_quad(sc, n, xs[0] - 30 + 6.0f * g, ys[0] - 14, xs[0] - 30 + 6.0f * g + 5, ys[0] - 9, blue);
    for (int g = 0; g < 3; ++g) put_quad(sc, n, xs[3] - 24 + 6.0f * g, ys[3] - 14, xs[3] - 24 + 6.0f * g + 5, ys[3] - 9, blue);
    nc = cluster_vertices(sc, n, kLabelLink, dropped);
    CHECK(nc == 2);
    add_labels(f, 0, sc.c, nc);
    CHECK(f.mark[0].glyphs == 4); CHECK(f.mark[1].glyphs == 0); CHECK(f.mark[2].glyphs == 3);
    // matching: every box matches its draw (the two-part object matches its nearest part)
    CHECK(match_markers(f, 0));
    CHECK(f.mark[0].obj == 1); CHECK(f.mark[1].obj == 2 || f.mark[1].obj == 3); CHECK(f.mark[2].obj == 4);
    CHECK(f.obj[0].marker == 1); CHECK(f.obj[3].marker == 3);
    // a box with no draw under it: not all matched (the draw is then NOT suppressed)
    f.nmark = 3;
    for (unsigned i = 0; i < f.nobj; ++i) f.obj[i].marker = 0;
    MarkerSprite extra; extra.cx = 2300; extra.cy = 1900; extra.hw = extra.hh = 7.5f; f.mark[f.nmark++] = extra;
    CHECK(!match_markers(f, 0)); CHECK(f.mark[3].obj == 0);
    // off-window / non-finite vertices are ignored, the cluster cap holds
    n = 0;
    for (int i = 0; i < 40; ++i) put_quad(sc, n, 1800.0f + 40.0f * float(i % 10), 1850.0f + 60.0f * float(i / 10), 1805.0f + 40.0f * float(i % 10), 1855.0f + 60.0f * float(i / 10), blue);
    sc.v[0].x = NAN; sc.v[1].x = 9000;
    nc = cluster_vertices(sc, n, kBoxLink, dropped);
    CHECK(nc == kMarkCap); CHECK(dropped == 40 - kMarkCap);
    // the firing wedge key (D-318 step69: VS 534f29ca / PS d34ee153, non-indexed 4 vertices, blend src5/dst2/op1, depth GREATER no write)
    WedgeFacts wf; wf.vs8 = 0x534f29ca; wf.ps8 = 0xd34ee153; wf.count = 4; wf.blend = true; wf.src = 5; wf.dst = 2; wf.op = 1; wf.depth = true; wf.depth_func = 5;
    CHECK(is_wedge(wf));
    WedgeFacts x = wf; x.dst = 6; CHECK(!is_wedge(x));          // the ordinary translucent HUD quads of the same shaders (blend 5/6)
    x = wf; x.depth_func = 8; CHECK(!is_wedge(x));                // depth ALWAYS
    x = wf; x.indexed = true; CHECK(!is_wedge(x));
    x = wf; x.count = 6; CHECK(!is_wedge(x));
    x = wf; x.depth_write = true; CHECK(!is_wedge(x));
    // object_from_record reads +256 B: translation floats 76..78, rows 64..74
    uint8_t rec[416] = {};
    const float tr3[3] = {12120.0f, 512.0f, 132150.0f}, ax = 2.0f;
    std::memcpy(rec + 4 * 76, tr3, 12); std::memcpy(rec + 4 * 64, &ax, 4);
    ObjDraw od; object_from_record(rec, od);
    CHECK(od.t[0] == 12120.0f && od.t[2] == 132150.0f && od.axes[0] == 2.0f);
    CHECK(mesh_signature(true, 203, 64, 5) == mesh_signature(true, 203, 64, 5)); CHECK(mesh_signature(true, 203, 64, 5) != mesh_signature(true, 204, 64, 5));
}

static TacFrame* frame_with(const std::vector<std::array<float, 3>>& objs) {
    static TacFrame f;
    f = TacFrame{};
    f.valid = true;
    for (const auto& p : objs) { ObjDraw& o = f.obj[f.nobj++]; for (int k = 0; k < 3; ++k) o.t[k] = p[k]; }
    return &f;
}

static void test_tracker_and_radar() {
    Options o; o.on = true; o.range_ring3 = 10000;
    Tracker tk;
    const float cam[3] = {0, 4000, 0};
    // frame 1: own aircraft parts at the camera (self), a two-part contact 10000 north, a static building 5000 east
    TacFrame* f = frame_with({{1, 4001, 2}, {0, 4000, -10000}, {20, 4000, -10010}, {5000, 400, 0}});
    tracker_update(tk, *f, cam, 1.0, o);
    CHECK(tk.tracks.size() == 2); CHECK(tk.self_draws == 1);
    // frame 2 (0.1 s): the contact moved 40 u north (400 u/s), the building did not
    f = frame_with({{0, 4000, -10040}, {20, 4000, -10050}, {5000, 400, 0}});
    tracker_update(tk, *f, cam, 1.1, o);
    CHECK(tk.tracks.size() == 2); CHECK(tk.created == 2);
    const Track* air = nullptr; const Track* bld = nullptr;
    for (const auto& t : tk.tracks) { if (t.pos[0] < 1000) air = &t; else bld = &t; }
    CHECK(air && bld); CHECK(air->vel_valid && near(air->vel[2], -400, 1)); CHECK(air->speed() >= kMovingSpeed); CHECK(bld->speed() < 1);
    // radar: heading north (fwd -Z), the moving contact 10050 north = the 3rd ring straight up; the static unteamed building is NOT shown
    float fh[3], rh[3];
    const float fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
    CHECK(heading_frame(fwd, up, fh, rh)); CHECK(near(rh[0], 1, 1e-6));
    const RadarGeom g; const float aspect = 1440.0f / 2560.0f;
    std::vector<Blip> b;
    radar_blips(tk, cam, fh, rh, 1.1, o, g, aspect, b);
    CHECK(b.size() == 1); CHECK(b[0].arrow); CHECK(!b[0].edge);
    CHECK(near(b[0].u, g.cu, 1e-4)); CHECK(near(b[0].v, g.cv - g.ring[2] * 10045.0 / 10000.0, 2e-4));
    CHECK(near(b[0].dir, 0.0, 0.01));   // flying away north = arrow pointing up
    // pixels at 2560x1440: centre (357.4, 1141.2) (step62/71 measurement), the contact 123.6 px above it
    CHECK(near(b[0].u * 2560, 357.4, 0.2)); CHECK(near((g.cv - b[0].v) * 1440, 0.0858 * 1440 * 1.0045, 0.3));
    // teach the building a team through a matched (enemy-coloured) marker: it appears as a coloured square at 5000 east = 2nd ring, right
    f = frame_with({{0, 4000, -10080}, {20, 4000, -10090}, {5000, 400, 0}});
    MarkerSprite m; m.rgb[0] = 1.0f; m.rgb[1] = 0.85f; m.rgb[2] = 0.2f; m.glyphs = 3; m.suppressed = true; m.obj = 3;
    f->mark[f->nmark++] = m; f->obj[2].marker = 1;
    tracker_update(tk, *f, cam, 1.2, o);
    radar_blips(tk, cam, fh, rh, 1.2, o, g, aspect, b);
    CHECK(b.size() == 2);
    const Blip* sq = b[0].arrow ? &b[1] : &b[0];
    CHECK(!sq->arrow); CHECK(near(sq->rgb[1], 0.85, 1e-6)); CHECK(near(sq->u, g.cu + g.ring[1] * 5000.0f / 5000.0f * aspect * (0.0858f / 0.0430f) * 0.5f, 2e-3));
    CHECK(near(sq->u * 2560 - 357.4, 0.0858 * 0.5 * 1440, 0.5));   // 5000 u = half the ring-3 radius, to the right, in pixels
    for (const auto& t : tk.tracks) if (t.pos[0] > 1000) { CHECK(t.team == team_enemy); CHECK(t.label == label_tgt); CHECK(!t.game_marker_now); }
    // heading east (fwd +X): the northern contact is now on the LEFT of the radar (u < centre, v at the centre)
    const float east[3] = {1, 0, 0};
    CHECK(heading_frame(east, up, fh, rh));
    radar_blips(tk, cam, fh, rh, 1.2, o, g, aspect, b);
    const Blip* ab = b[0].arrow ? &b[0] : &b[1];
    CHECK(ab->u < g.cu - 0.03f); CHECK(near(ab->v, g.cv, 1e-3)); CHECK(near(ab->dir, -1.5708, 0.02));
    // a contact beyond the box: clamped to the border as an edge chevron
    float uu, vv;
    const float far[3] = {0, 0, -60000};
    const float north[3] = {0, 0, -1};
    CHECK(heading_frame(north, up, fh, rh));
    CHECK(!radar_uv(far, fh, rh, o, g, aspect, uu, vv)); CHECK(near(uu, g.cu, 1e-5)); CHECK(vv >= g.cv - g.half_v && vv < g.cv - 0.9f * g.half_v);
    // coasting: unseen for 3 s the unteamed contact is dropped (kCoastUnknown 2 s), the teamed building stays (dead reckoned, dimmer)
    f = frame_with({});
    tracker_update(tk, *f, cam, 4.5, o);
    CHECK(tk.tracks.size() == 1); CHECK(tk.tracks[0].team == team_enemy); CHECK(!tk.tracks[0].seen_now);
    tracker_update(tk, *f, cam, 1.2 + kCoastTeamed + 0.5, o);
    CHECK(tk.tracks.empty());
    // near-vertical nose: heading from the camera up vector
    const float upnose[3] = {0, 1, 0}, camup[3] = {0, 0, 1};   // nose straight up, camera up pointing south: the nose came from flying north
    CHECK(heading_frame(upnose, camup, fh, rh)); CHECK(near(fh[2], -1, 1e-5));
    // team / label rules
    const float frnd[3] = {0.39f, 0.48f, 1.0f}, grey[3] = {1, 1, 1}, green[3] = {0.3f, 1.0f, 0.3f};
    CHECK(team_of(frnd) == team_friend); CHECK(team_of(grey) == team_unknown); CHECK(team_of(green) == team_enemy);
    CHECK(label_of(4, team_enemy) == label_frnd); CHECK(label_of(3, team_friend) == label_tgt); CHECK(label_of(0, team_friend) == label_frnd); CHECK(label_of(0, team_enemy) == label_none);
}

// A head turned 40 deg LEFT of the game window: a contact 40 deg left of the nose lands in the centre of that eye; one straight ahead is 40 deg to
// the right of the view centre (still in a 100 deg view); one behind is not drawn (no clamping).
static void test_markers_head_40() {
    acz::xr_owner::EyeSample eye;
    const float a = 40.0f * 3.14159265f / 180.0f;
    eye.pose.orientation[1] = std::sin(a / 2); eye.pose.orientation[3] = std::cos(a / 2);   // +40 deg about +Y = turned left
    eye.fov = {-0.87f, 0.87f, 0.87f, -0.87f};                                                // ~100 deg symmetric eye
    const float anchor[3] = {0, 0, 0}, lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    acz::xr_stereo::WorldComposite wc{};
    CHECK(acz::xr_stereo::world_composite_params(eye, anchor, 0.0f, lens, 40.0f, wc));
    EyeView ev;
    const float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};
    CHECK(acz::props::make_clip(wc.base, wc.ux, wc.vy, right, up, fwd, ev.clip));
    ev.eye[0] = 100; ev.eye[1] = 4000; ev.eye[2] = 200; ev.up[1] = 1; ev.ok = true;
    const float d = 5000.0f;
    const float left40[3] = {ev.eye[0] - d * std::sin(a), ev.eye[1], ev.eye[2] - d * std::cos(a)};
    float u, v;
    CHECK(eye_uv(ev, left40, u, v)); CHECK(near(u, 0.5, 1e-3)); CHECK(near(v, 0.5, 1e-3));
    const float ahead[3] = {ev.eye[0], ev.eye[1], ev.eye[2] - d};
    CHECK(eye_uv(ev, ahead, u, v));
    CHECK(near(u, 0.5 + 0.5 * std::tan(a) / std::tan(0.87), 2e-3)); CHECK(near(v, 0.5, 1e-3));
    const float behind[3] = {ev.eye[0], ev.eye[1], ev.eye[2] + d};
    CHECK(!eye_uv(ev, behind, u, v));
    // the marker geometry: box + "FRND" around the projected centre, its size the game's angular size
    Overlay o;
    begin_slot(o, slot_eye0_post);
    WorldMarker m; std::memcpy(m.pos, left40, sizeof m.pos); m.rgb[0] = 0.39f; m.rgb[1] = 0.48f; m.rgb[2] = 1.0f; m.label = label_frnd;
    CHECK(build_marker(o, ev, m));
    WorldMarker mb = m; std::memcpy(mb.pos, behind, sizeof mb.pos);
    CHECK(!build_marker(o, ev, mb));
    end_slot(o, slot_eye0_post);
    CHECK(o.count[slot_eye0_post] > 24 && o.count[slot_eye0_post] % 3 == 0);
    double umin = 1e9, umax = -1e9, vmax = -1e9;
    unsigned box = 4 * 6;   // the first four segments are the box
    for (unsigned i = 0; i < box; ++i) {
        const Vertex& x = o.v[i];
        CHECK(x.pos[3] > 0 && x.pos[2] == 0.0f);
        const double uu = 0.5 * (x.pos[0] / x.pos[3] + 1), vv = 0.5 * (1 - x.pos[1] / x.pos[3]);
        umin = std::min(umin, uu); umax = std::max(umax, uu); vmax = std::max(vmax, vv);
        CHECK(near(x.col[3], 1.0, 1e-6)); CHECK(x.col[2] <= x.col[3]);   // premultiplied
    }
    // box half-size kMarkerHalfTan at the view centre: half-width in uv = tan / (2 tan(0.87))
    const double half = (kMarkerHalfTan + 0.5 * kMarkerLineTan) / (2.0 * std::tan(0.87));
    CHECK(near(0.5 * (umin + umax), 0.5, 2e-4)); CHECK(near(umax - 0.5, half, 2e-4));
    // the label lies left of and above the box (screen v up = smaller v)
    bool label_left = true;
    for (unsigned i = box; i < o.count[slot_eye0_post]; ++i) {
        const Vertex& x = o.v[i];
        const double uu = 0.5 * (x.pos[0] / x.pos[3] + 1), vv = 0.5 * (1 - x.pos[1] / x.pos[3]);
        if (uu > 0.5 - half + 1e-4 || vv > 0.5 - kMarkerHalfTan / (2 * std::tan(0.87)) + 2e-3) label_left = false;
    }
    CHECK(label_left);
    (void)vmax;
}

// Synthetic draw logs: per producer frame the list of between-members draws (shader/blend/depth facts); the wedge count feeds the debounce.
static unsigned wedges_in(const std::vector<WedgeFacts>& draws) { unsigned n = 0; for (const auto& d : draws) n += is_wedge(d) ? 1 : 0; return n; }
static void test_firing() {
    WedgeFacts wedge; wedge.vs8 = 0x534f29ca; wedge.ps8 = 0xd34ee153; wedge.count = 4; wedge.blend = true; wedge.src = 5; wedge.dst = 2; wedge.op = 1;
    wedge.depth = true; wedge.depth_func = 5;
    WedgeFacts quad = wedge; quad.dst = 6;   // ordinary HUD quad of the same shaders (every frame)
    WedgeFacts tri; tri.vs8 = 0x9eff1f92; tri.ps8 = 0xd2464ed2; tri.indexed = true; tri.count = 96;   // missile smoke
    const std::vector<WedgeFacts> quiet = {quad, quad, tri}, firing = {quad, wedge, quad};
    CHECK(wedges_in(quiet) == 0); CHECK(wedges_in(firing) == 1);
    FiringDetector d;
    double t = 10.0; const double dt = 1.0 / 60.0;
    // a single stray frame never fires
    CHECK(!d.update(wedges_in(firing) > 0, t)); t += dt;
    for (int i = 0; i < 10; ++i) { CHECK(!d.update(wedges_in(quiet) > 0, t)); t += dt; }
    // a held gun: on after 3 frames AND 50 ms (60 fps: the 4th frame)
    bool on = false; int frames = 0;
    while (!on && frames < 20) { on = d.update(wedges_in(firing) > 0, t); t += dt; ++frames; }
    CHECK(on); CHECK(frames == 4); CHECK(d.on_events == 1);
    // the wedge flickers off for 2 frames (game frame pacing): stays on
    CHECK(d.update(false, t)); t += dt; CHECK(d.update(false, t)); t += dt; CHECK(d.update(true, t)); t += dt;
    // released: off after >= 150 ms and >= 4 frames
    int off_frames = 0;
    while (d.on && off_frames < 60) { d.update(false, t); t += dt; ++off_frames; }
    CHECK(!d.on); CHECK(off_frames >= 9 && off_frames <= 11);
    // 15 fps producer (headset test 17): 3 frames span 133 ms -> on at the 3rd; off after 4 absent frames (267 ms)
    FiringDetector s; t = 0; on = false; frames = 0;
    while (!on && frames < 10) { on = s.update(true, t); t += 1.0 / 15; ++frames; }
    CHECK(frames == 3);
    off_frames = 0;
    while (s.on && off_frames < 20) { s.update(false, t); t += 1.0 / 15; ++off_frames; }
    CHECK(off_frames == 4);
}

static void test_tracers() {
    TracerSim sim;
    const float cam[3] = {0, 4000, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1}, vel[3] = {0, 0, -800};
    float gl[3], gr[3];
    gun_positions(cam, right, up, fwd, gl, gr);
    CHECK(near(gl[0], -6, 1e-5) && near(gr[0], 6, 1e-5) && near(gl[1], 3995, 1e-3) && near(gl[2], -10, 1e-5));
    double t = 5.0;
    for (int i = 0; i < 18; ++i) { sim.update(true, t, gl, gr, fwd, vel); t += 1.0 / 60; }   // 0.3 s of fire
    CHECK(sim.spawned >= 10 && sim.spawned <= 12);   // 2 guns x (0.3 s / 60 ms)
    float a[3], b[3], alpha;
    unsigned live = 0;
    for (const Round& r : sim.rounds) if (TracerSim::streak(r, t, a, b, alpha)) {
        ++live; CHECK(b[2] < a[2]); CHECK(alpha > 0 && alpha <= 1); CHECK(b[2] < -10);   // flying forward (-Z), ahead of the muzzles
        CHECK(near(a[2] - b[2], (800 + TracerSim::kMuzzle) * TracerSim::kStreak, 5) || a[2] - b[2] < (800 + TracerSim::kMuzzle) * TracerSim::kStreak);
    }
    CHECK(live == sim.rounds.size());
    // projected: streaks ahead in a forward eye (game window, no head turn)
    acz::xr_owner::EyeSample eye; eye.pose.orientation[3] = 1; eye.fov = {-0.87f, 0.87f, 0.87f, -0.87f};
    const float anchor[3] = {0, 0, 0}, lens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f};
    acz::xr_stereo::WorldComposite wc{};
    CHECK(acz::xr_stereo::world_composite_params(eye, anchor, 0.0f, lens, 40.0f, wc));
    EyeView ev; CHECK(acz::props::make_clip(wc.base, wc.ux, wc.vy, right, up, fwd, ev.clip));
    std::memcpy(ev.eye, cam, sizeof ev.eye); ev.ok = true;
    Overlay o; begin_slot(o, slot_eye0_pre);
    const unsigned q = build_tracers(o, ev, sim, t);
    end_slot(o, slot_eye0_pre);
    CHECK(q == live); CHECK(o.count[slot_eye0_pre] == 6 * q);
    for (unsigned i = 0; i < o.count[slot_eye0_pre]; ++i) {
        const Vertex& x = o.v[i];
        const double uu = 0.5 * (x.pos[0] / x.pos[3] + 1), vv = 0.5 * (1 - x.pos[1] / x.pos[3]);
        CHECK(uu > 0.45 && uu < 0.55 && vv > 0.49 && vv < 0.56);   // converging toward the centre, slightly below it
    }
    // trigger released: no new rounds; after the life all are gone
    const uint64_t before = sim.spawned;
    sim.update(false, t + 0.5, gl, gr, fwd, vel); CHECK(sim.spawned == before);
    sim.update(false, t + TracerSim::kLife + 0.1, gl, gr, fwd, vel); CHECK(sim.rounds.empty());
}

// No double markers: a contact whose game marker is DRAWN this frame gets none of ours; a suppressed one (or one out of the window) gets ours.
static void test_no_doubles() {
    Options o; o.on = true;
    Tracker tk;
    const float cam[3] = {0, 4000, 0};
    TacFrame* f = frame_with({{0, 4000, -5000}, {3000, 4000, -5000}});
    MarkerSprite a; a.rgb[0] = 0.39f; a.rgb[1] = 0.48f; a.rgb[2] = 1.0f; a.glyphs = 4; a.suppressed = false; a.obj = 1;
    MarkerSprite b = a; b.suppressed = true; b.obj = 2;
    f->mark[0] = a; f->mark[1] = b; f->nmark = 2; f->obj[0].marker = 1; f->obj[1].marker = 2;
    tracker_update(tk, *f, cam, 1.0, o);
    std::vector<WorldMarker> ms; uint32_t skipped = 0;
    collect_markers(tk, 1.0, ms, skipped);
    CHECK(ms.size() == 1); CHECK(skipped == 1); CHECK(near(ms[0].pos[0], 3000, 1e-3)); CHECK(ms[0].label == label_frnd);
    // next frame: both contacts outside the game window (no game marker at all): we draw both, all view directions
    f = frame_with({{0, 4000, -5010}, {3000, 4000, -5010}});
    tracker_update(tk, *f, cam, 1.05, o);
    collect_markers(tk, 1.05, ms, skipped);
    CHECK(ms.size() == 2); CHECK(skipped == 0);
    // an unteamed contact never gets a marker
    f = frame_with({{0, 4000, -5020}, {3000, 4000, -5020}, {-9000, 4000, -100}});
    tracker_update(tk, *f, cam, 1.1, o);
    collect_markers(tk, 1.1, ms, skipped);
    CHECK(ms.size() == 2);
}

// Key off on the consumer side: a BlitJob carries no overlay unless the tactical path set one; an empty overlay draws nothing.
static void test_key_off() {
    acz::xr_stereo::BlitJob job;
    CHECK(job.tac == nullptr);
    acz::xr_stereo::RecordFacts rf;
    CHECK(!rf.tac_used && rf.tac_vertices == 0);
    acz::stereo::StereoFrame sf;
    CHECK(sf.tac == nullptr);
    acz::xr_stereo::TerrainRuntime tr;
    CHECK(!tr.tac.on);
    Overlay o; CHECK(o.empty());
    std::vector<Blip> none; begin_slot(o, slot_hud); build_radar(o, none, RadarGeom{}, 0.5625f); end_slot(o, slot_hud);
    CHECK(o.empty() && o.count[slot_hud] == 0);
    // the overlay cap holds
    Overlay big;
    std::vector<Blip> many(20000);
    build_radar(big, many, RadarGeom{}, 0.5625f);
    CHECK(big.v.size() <= kOverlayCap);
}

int main() {
    test_options();
    test_producer_helpers();
    test_tracker_and_radar();
    test_markers_head_40();
    test_firing();
    test_tracers();
    test_no_doubles();
    test_key_off();
    std::printf("tactical_offline: %u checks passed\n", g_checks);
    return 0;
}
