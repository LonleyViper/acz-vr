// AIRCRAFT-AUTO offline tests (docs/world-render/aircraft-auto.md): the type table against aircraft_cockpits.json, E6 sample -> aircraft-frame eye on
// real E6 samples, the detector on fake E6 streams (all 36 types with noise and look turns, identical-eye tie, near pairs, no match, unstable,
// aircraft switch mid-session with hysteresis, no lock), the 'auto' config value, and (optional, ACZ_MODELS_ALL=<dir>) every baked model through
// the runtime loader + mesh builder. No game, no GPU.
#include "aircraft_auto.h"
#include "ee_e6_locator.h"
#include "cockpit_model.h"
#include "runtime_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace acz::aircraft;
using acz::e6loc::Sample;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
const unsigned W = DetectOptions{}.window;   // AIRCRAFT-AUTO-FAST: 12 (was 24)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    double uni() { s = s * 6364136223846793005ull + 1442695040888963407ull; return double((s >> 11) & ((1ull << 53) - 1)) / double(1ull << 53); }
    double range(double a, double b) { return a + (b - a) * uni(); }
    double gauss() { const double u = std::max(uni(), 1e-12), v = uni(); return std::sqrt(-2 * std::log(u)) * std::cos(2 * kPi * v); }
};

// world = Ry(yaw) Rx(pitch) Rz(roll) * local (the convention eye_in_aircraft inverts)
void rotate(double p, double y, double r, const double l[3], double o[3]) {
    const double cz = std::cos(r), sz = std::sin(r);
    const double a0 = cz * l[0] - sz * l[1], a1 = sz * l[0] + cz * l[1], a2 = l[2];
    const double cx = std::cos(p), sx = std::sin(p);
    const double b0 = a0, b1 = cx * a1 - sx * a2, b2 = sx * a1 + cx * a2;
    const double cy = std::cos(y), sy = std::sin(y);
    o[0] = cy * b0 + sy * b2; o[1] = b1; o[2] = -sy * b0 + cy * b2;
}

// One E6 sample of an aircraft whose eye point is `eye` (model units), flying at a random attitude/position. noise = per-axis gaussian sigma on the
// camera offset (world units), look = a turned game look (look yaw/pitch, the camera rotation includes it and the eye pivots by ~1 unit).
Sample fake(const float eye[3], Rng& g, double noise, bool look = false) {
    Sample s;
    const double p = g.range(-0.5, 0.5), y = g.range(-0.9, 0.9), r = g.range(-3.0, 3.0);
    const double air[3] = {g.range(-150000, 150000), g.range(500, 4500), g.range(-150000, 150000)};
    double l[3] = {eye[0], eye[1], eye[2]};
    double lp = 0, ly = 0;
    if (look) {   // step50-like: look turned, the camera pivots (|d| 35.69 vs 36.48 for the Draken): shift the eye by up to ~1 unit
        ly = g.range(-2.5, 2.5); lp = g.range(-0.3, 0.3);
        l[0] += g.range(-1.0, 1.0); l[2] += g.range(0.0, 0.8);
    }
    double w[3]; rotate(p, y, r, l, w);
    for (int i = 0; i < 3; ++i) { s.air[i] = float(air[i]); s.cam[i] = float(air[i] + w[i] + noise * g.gauss()); }
    s.rot[0] = float(p + lp); s.rot[1] = float(y + ly); s.rot[2] = float(r); s.heading = float(y);
    s.look[0] = float(lp); s.look[1] = float(ly); s.look[2] = 0.0f;
    return s;
}

void feed_sample(Detector& d, const Sample& s, uint32_t epoch) {
    float l[3]; bool c = false;
    CHECK(eye_in_aircraft(s, l, c));
    d.feed(l, c, epoch);
}

std::string read_file(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
} // namespace

static void test_table() {
    // every row equals research/world-render-data/aircraft_cockpits.json (type order, model_id, pvb eye, stick style)
    const std::string j = read_file(ACZ_AIRCRAFT_TABLE);
    CHECK(!j.empty());
    size_t pos = j.find("\"aircraft\"");
    CHECK(pos != std::string::npos);
    unsigned n = 0;
    for (;;) {
        const size_t t = j.find("\"type\": \"", pos);
        if (t == std::string::npos) break;
        const size_t te = j.find('"', t + 9);
        const std::string type = j.substr(t + 9, te - t - 9);
        const size_t m = j.find("\"model_id\": \"", te);
        const std::string mid = j.substr(m + 13, j.find('"', m + 13) - m - 13);
        const size_t e = j.find("\"eye\": [", m);
        const size_t ee = j.find(']', e);
        float eye[3]; CHECK(std::sscanf(j.substr(e + 8, ee - e - 8).c_str(), " %f , %f , %f", &eye[0], &eye[1], &eye[2]) == 3);
        const size_t st = j.find("\"stick\": \"", ee);
        const std::string stick = j.substr(st + 10, j.find('"', st + 10) - st - 10);
        CHECK(n < kAircraftCount);
        CHECK(type == kAircraft[n].type && mid == kAircraft[n].model_id);
        for (int i = 0; i < 3; ++i) CHECK(std::fabs(eye[i] - kAircraft[n].eye[i]) < 1e-5f);
        CHECK((stick == "side") == (kAircraft[n].stick == 1));
        CHECK(find_type(type.c_str()) == int(n));
        ++n; pos = st;
    }
    CHECK(n == kAircraftCount);
    // identical eyes: only the F-15 family; the smallest distinct separation (e18g/m29a 0.1616) leaves room for the 0.08 tolerance
    for (unsigned i = 0; i < kAircraftCount; ++i) {
        const uint64_t m = same_eye_mask(i);
        const bool f15 = std::strncmp(kAircraft[i].type, "f15", 3) == 0;
        CHECK(f15 ? m == ((1ull << find_type("f15c")) | (1ull << find_type("f15e")) | (1ull << find_type("f15s"))) : m == (1ull << i));
    }
    const float sep = min_distinct_eye_separation();
    CHECK(std::fabs(sep - 0.16155f) < 1e-3f);
    CHECK(DetectOptions{}.match_tol < sep / 2);
    CHECK(find_type("zzzz") == -1 && find_type(nullptr) == -1);
    std::printf("  table: %u types, smallest distinct eye separation %.4f (match tol %.3f)\n", n, double(sep), double(DetectOptions{}.match_tol));
}

static void test_real_samples() {
    // real E6 samples (docs/world-render/e6-anchor-free.md, tests/acz-vr/e6_locator_offline_tests.cpp), all from the Draken of mission 02
    const float* j35 = kAircraft[find_type("j35j")].eye;
    auto res = [&](const Sample& s, bool& c) { float l[3]; CHECK(eye_in_aircraft(s, l, c)); return std::sqrt((l[0] - j35[0]) * (l[0] - j35[0]) + (l[1] - j35[1]) * (l[1] - j35[1]) + (l[2] - j35[2]) * (l[2] - j35[2])); };
    // step67 level flight (heading -45 deg): cam - air from the hexdump floats
    { Sample s; s.air[0] = -13953.72f; s.air[1] = 4000.0f; s.air[2] = 121615.36f; s.heading = -0.7853982f; s.rot[1] = -0.7853982f;
      const double l[3] = {0, 3.575, -36.3}; double w[3]; rotate(0, -0.7853982, 0, l, w);
      for (int i = 0; i < 3; ++i) s.cam[i] = float(double(s.air[i]) + w[i]);
      bool c; CHECK(res(s, c) < 0.02f && c); }
    // step56 nose-down (pitch -0.517): residual 0.06 from the logged (rounded) values
    { Sample n; n.air[0] = -13778.32f; n.air[1] = 3586.286f; n.air[2] = 121439.7f; n.cam[0] = -13754.76f; n.cam[1] = 3571.441f; n.cam[2] = 121416.2f;
      n.heading = -0.785398f; n.rot[0] = -0.517313f; n.rot[1] = -0.785398f; bool c; const float r = res(n, c); CHECK(r < 0.08f && c);
      std::printf("  step56 nose-down: residual %.3f\n", double(r)); }
    // headset test 16 inverted (roll -2.96): the roll convention matters (Rz last)
    { Sample r; r.heading = -0.556928f; r.air[0] = -26055.68f; r.air[1] = 3695.608f; r.air[2] = 132071.1f; r.cam[0] = -26036.3f; r.cam[1] = 3687.822f; r.cam[2] = 132041.2f;
      r.rot[0] = -0.118526f; r.rot[1] = -0.556928f; r.rot[2] = -2.96203f; bool c; const float d = res(r, c); CHECK(d < 0.05f && c);
      std::printf("  headset test 16 inverted: residual %.3f\n", double(d));
      Sample w = r; w.rot[2] = 2.96203f; CHECK(res(w, c) > 1.0f); }   // the opposite roll sign (170 vs -170 deg) is >1 unit off
    // step50 look turned back (look yaw 2.475): not centred (excluded), and indeed 0.8+ units off the pvb eye
    { Sample l; l.look[1] = 2.47519f; l.heading = -0.785398f; l.air[0] = -3227.176f; l.air[1] = 4000; l.air[2] = 110891; l.cam[0] = -3202.793f; l.cam[1] = 4003.67f; l.cam[2] = 110865.2f;
      l.rot[0] = 3.47329e-06f; l.rot[1] = 1.69106f; bool c = true; float lo[3]; CHECK(eye_in_aircraft(l, lo, c) && !c);
      CHECK(acz::e6loc::check_sample(l) == nullptr);   // a valid locked sample all the same
      const float d = acz::e6loc::eye_distance(l); CHECK(d > 35.6f && d < 35.8f); }
    { Sample s; s.cam[0] = std::nanf(""); float l[3]; bool c; CHECK(!eye_in_aircraft(s, l, c)); }
}

static void test_all_types() {
    // every type, random attitudes over the whole map, 0.03 u gaussian noise + 10% look-turned samples (excluded) + 5% 0.25 u outliers (median)
    unsigned exact = 0; float worst_conf = 1.0f;
    for (unsigned t = 0; t < kAircraftCount; ++t) {
        Rng g(100 + t);
        Detector d;
        for (int k = 0; k < int(W) * 3; ++k) {
            const bool look = g.uni() < 0.10;
            const bool outlier = !look && g.uni() < 0.05;
            feed_sample(d, fake(kAircraft[t].eye, g, outlier ? 0.25 : 0.03, look), 1);
        }
        const int want = (std::strncmp(kAircraft[t].type, "f15", 3) == 0) ? find_type("f15c") : int(t);   // tie rule: first in table order
        CHECK(d.current() == want);
        // AIRCRAFT-AUTO-FAST: a 12-sample median is ~1.4x noisier than a 24-sample one (f15s here: residual 0.040 = confidence 0.49); the
        // tolerance is unchanged, so the bound is residual < 0.06 (confidence > 0.25) instead of 0.04 (0.5) with window 24.
        CHECK(d.last().verdict == Verdict::match && d.last().confidence > 0.25f);
        worst_conf = std::min(worst_conf, d.last().confidence);
        CHECK(d.off_centre_samples > 0 && d.switches == 1);
        if (d.current() == int(t)) ++exact;
    }
    CHECK(exact == kAircraftCount - 2);   // f15e and f15s resolve to f15c by default
    // with heavier noise (0.08 u per axis, single samples ~0.14 u off) the median still lands within the tolerance for every type
    for (unsigned t = 0; t < kAircraftCount; ++t) {
        Rng g(900 + t); Detector d;
        for (int k = 0; k < 72; ++k) feed_sample(d, fake(kAircraft[t].eye, g, 0.08), 7);
        CHECK(d.current() >= 0 && (same_eye_mask(t) & (1ull << unsigned(d.current()))));
    }
    std::printf("  36 types: %u exact, F-15E/S -> f15c (tie rule); lowest last-window confidence %.2f (window %u)\n", exact, double(worst_conf), W);
}

static void test_ties_and_edges() {
    const int f15c = find_type("f15c"), f15e = find_type("f15e"), f15s = find_type("f15s");
    // the F-15 tie: reported, resolved by prefer: (first preferred in table order) or the first in table order
    { Rng g(5); DetectOptions o; o.prefer = 1ull << f15e; Detector d(o);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f15s].eye, g, 0.02), 1);
      CHECK(d.current() == f15e && d.last().ties == ((1ull << f15c) | (1ull << f15e) | (1ull << f15s))); }
    { Rng g(6); DetectOptions o; o.prefer = (1ull << f15s) | (1ull << find_type("j35j")); Detector d(o);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f15c].eye, g, 0.02), 1);
      CHECK(d.current() == f15s); }
    { Rng g(7); DetectOptions o; o.prefer = 1ull << find_type("j35j"); Detector d(o);   // a preference outside the tie group changes nothing
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f15e].eye, g, 0.02), 1);
      CHECK(d.current() == f15c); }
    // the nearest distinct pair (e18g / m29a, 0.1616 apart): a point exactly between them matches neither (no guess) ...
    { const float* a = kAircraft[find_type("e18g")].eye; const float* b = kAircraft[find_type("m29a")].eye;
      std::vector<float> mid; for (int k = 0; k < int(W); ++k) for (int i = 0; i < 3; ++i) mid.push_back(0.5f * (a[i] + b[i]));
      const WindowResult w = Detector::evaluate(mid, DetectOptions{});
      CHECK(w.verdict == Verdict::no_match && w.chosen == -1 && w.best_res > 0.08f && w.second_res > 0.08f);
      // ... and 0.03 u towards either side picks that side
      std::vector<float> near_a; for (int k = 0; k < int(W); ++k) for (int i = 0; i < 3; ++i) near_a.push_back(a[i] + 0.3f * (b[i] - a[i]));
      CHECK(Detector::evaluate(near_a, DetectOptions{}).chosen == find_type("e18g")); }
    // f02a / f16c (0.22 apart) and su27 / su37 (0.30): separated
    for (const char* pr : {"f02a", "f16c", "su27", "su37", "su32"}) {
        Rng g(11); Detector d; const int t = find_type(pr);
        for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[t].eye, g, 0.03), 1);
        CHECK(d.current() == t);
    }
    // an eye offset that is no player aircraft (a 'HUD view' camera, a modded aircraft): no match -> no model
    { Rng g(12); Detector d; const float odd[3] = {0.0f, 10.0f, -42.0f};
      for (int k = 0; k < 72; ++k) feed_sample(d, fake(odd, g, 0.02), 1);
      CHECK(d.current() == -1 && d.last().verdict == Verdict::no_match && d.windows == 72 / W && d.switches == 0); }
    // unstable window (half the samples > 0.3 off the median: e.g. a torn-read storm): decides nothing; a few outliers do not matter
    { Rng g(13); Detector d; const float* e = kAircraft[find_type("j35j")].eye;
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(e, g, k % 2 == 0 ? 1.5 : 0.02), 1);
      CHECK(d.last().verdict == Verdict::unstable && d.current() == -1 && d.last().inlier_frac < 0.75f);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(e, g, k % 6 == 0 ? 1.5 : 0.02), 1);
      CHECK(d.last().verdict == Verdict::match && d.current() == find_type("j35j") && d.last().spread > 0.3f); }
    // no lock => nothing published => no samples => no model; look-only flight (the player keeps the view turned) => no window, no model.
    // These look-turned Draken samples still pass the locator (|cam-air| inside the live 35.7..36.6 band, <= 12.8 deg off forward): the type is
    // decided from look-centred samples only.
    { Detector d; CHECK(d.current() == -1 && d.windows == 0); }
    { Rng g(14); Detector d; const float* e = kAircraft[find_type("j35j")].eye; unsigned inband = 0;
      for (int k = 0; k < 200; ++k) {
          Sample s = fake(e, g, 0.0, true);
          if (acz::e6loc::check_sample(s) == nullptr) {
              const float dist = acz::e6loc::eye_distance(s);
              if (dist >= 35.6f && dist <= 36.7f && acz::e6loc::eye_angle_deg(s) <= 12.8) ++inband;
          }
          feed_sample(d, s, 1);
      }
      CHECK(d.windows == 0 && d.current() == -1 && d.off_centre_samples == 200 && inband > 100);
      std::printf("  look-only Draken stream: %u/200 samples inside the live 35.7-36.6 u / 12.8 deg band, 0 windows, no model\n", inband); }
}

static void test_switch_and_hysteresis() {
    const int j35 = find_type("j35j"), f16 = find_type("f16c"), m21 = find_type("m21b");
    Rng g(21);
    Detector d;   // window W (12), confirm 2
    for (int k = 0; k < int(W) * 4; ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
    CHECK(d.current() == j35 && d.switches == 1);
    // one window of garbage (no match) does not unload; the next good window keeps the Draken
    { const float odd[3] = {0, 12, -30}; for (int k = 0; k < int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 1); }
    CHECK(d.current() == j35 && d.last().verdict == Verdict::no_match);
    for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
    CHECK(d.current() == j35 && d.switches == 1);
    // a different aircraft in the SAME lock epoch (never seen live; the hysteresis must hold): one window = pending, the second confirms
    for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
    CHECK(d.current() == j35 && !d.changed());
    for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
    CHECK(d.current() == f16 && d.changed() && d.switches == 2);
    // mission change: the locator loses and re-acquires the block (new epoch): the partial window is dropped, the old choice stays until two windows
    // of the new aircraft agree
    for (int k = 0; k < 10; ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
    for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[m21].eye, g, 0.02), 2);
    CHECK(d.current() == f16 && d.epochs == 2 && d.last().chosen == m21);   // a pure MiG-21 window (the 10 F-16 samples were dropped)
    for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[m21].eye, g, 0.02), 2);
    CHECK(d.current() == m21 && d.switches == 3);
    // headset test 2026-10-28 item 11: no-match windows (eye wobble on the ground) must NOT unload; W5-3 P2 #1: not even a long streak in the same lock epoch
    { const float odd[3] = {0, 12, -30};
      for (int k = 0; k < 6 * int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 2);
      CHECK(d.current() == m21 && d.switches == 3);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[m21].eye, g, 0.02), 2);   // a match resets the streak
      for (int k = 0; k < 60 * int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 2);             // far beyond drop_confirm (25): kept, same epoch
      CHECK(d.current() == m21 && d.switches == 3 && !d.changed());
      // looking away afterwards cannot unload either, and a centred match is accepted without a rest-gaze window
      for (int k = 0; k < 200; ++k) feed_sample(d, fake(odd, g, 0.02, true), 2);
      CHECK(d.current() == m21);
      // a later lock epoch (mission change) with drop_confirm no-match windows does drop it
      for (int k = 0; k < 24 * int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 3);
      CHECK(d.current() == m21);
      for (int k = 0; k < 2 * int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 3);
      CHECK(d.current() == -1 && d.switches == 4); }
    // confirm:1 switches on the first window
    { DetectOptions o; o.confirm = 1; Detector q(o);
      for (int k = 0; k < int(W); ++k) feed_sample(q, fake(kAircraft[j35].eye, g, 0.02), 1);
      for (int k = 0; k < int(W); ++k) feed_sample(q, fake(kAircraft[f16].eye, g, 0.02), 1);
      CHECK(q.current() == f16 && q.switches == 2); }
}

static void test_config() {
    AutoConfig c; std::string why;
    CHECK(is_auto("auto") && is_auto("auto;anim:1") && !is_auto("autox") && !is_auto("j35jk_vrcockpit") && !is_auto("") && !is_auto("Auto"));
    CHECK(parse_auto("auto", c, why) && !c.stick_explicit && c.prefer == 0 && c.opt.upm == 5.0f);
    CHECK(options_for(c, unsigned(find_type("f16c"))).stick == 1 && options_for(c, unsigned(find_type("j35j"))).stick == 0);   // the table's stick style
    CHECK(parse_auto("auto;anim:1;stick:centre;upm:5.5", c, why) && c.stick_explicit && c.opt.anim && c.opt.upm == 5.5f);
    CHECK(options_for(c, unsigned(find_type("f16c"))).stick == 0);   // explicit stick: wins
    // COCKPIT-SCALE: cscale goes through the shared cockpit option parser (default 1 = unscaled), carried per type, range-checked.
    CHECK(c.opt.cscale == 1.0f && options_for(c, unsigned(find_type("f16c"))).cscale == 1.0f);
    CHECK(parse_auto("auto;cscale:1.08;anim:1", c, why) && c.opt.cscale == 1.08f && options_for(c, unsigned(find_type("j35j"))).cscale == 1.08f);
    for (const char* bad : {"auto;cscale:0.5", "auto;cscale:1.6", "auto;cscale:nan", "auto;cscale:", "auto;cscale:inf"}) { AutoConfig x; std::string w2; CHECK(!parse_auto(bad, x, w2)); }
    CHECK(parse_auto("auto;prefer:f15e", c, why) && c.prefer == (1ull << find_type("f15e")) && c.prefer_text == "f15e");
    CHECK(parse_auto("auto;prefer=f15s,f15e;canopy:0", c, why) && c.prefer == ((1ull << find_type("f15e")) | (1ull << find_type("f15s"))) && !c.opt.canopy);
    for (const char* bad : {"auto;", "auto;prefer:zzzz", "auto;prefer:", "auto;hide:3", "auto;show:1", "auto;eye:0,1,2", "auto;upm:999", "auto;bogus:1", "autox", "auto;anim",
                            "auto;clipbox:1,1,1,1,1"}) {
        AutoConfig x; std::string w; CHECK(!parse_auto(bad, x, w) && !w.empty());
    }
    // COCKPIT-CLIP: clip on by default, clip:0 carried per type; every table model has a clip volume (cockpit_clip_volumes.json, same 36 stems)
    CHECK(parse_auto("auto", c, why) && c.opt.clip && options_for(c, 0).clip && parse_auto("auto;clip:0", c, why) && !options_for(c, 0).clip);
    for (unsigned t = 0; t < kAircraftCount; ++t) {
        const acz::cockpit::ClipVolume* cv = acz::cockpit::clip_volume_for(kAircraft[t].model_id);
        CHECK(cv != nullptr && std::string(cv->model_id) == kAircraft[t].model_id);
        acz::cockpit::Options o; CHECK(acz::cockpit::apply_clip_table(o, kAircraft[t].model_id) && o.clip_box[0] == cv->ahead && o.clip_box[4] == cv->half);
    }
    // the runtime config accepts the value under the existing key (no new key)
    auto r = acz::diagnostics::parse_config("xr_cockpit_model=auto;anim:1;prefer:f15e\n");
    CHECK(r.issue == acz::diagnostics::ConfigIssue::none && std::string(r.config.xr_cockpit_model()) == "auto;anim:1;prefer:f15e");
    CHECK(acz::diagnostics::parse_config("xr_cockpit_model=auto\n").issue == acz::diagnostics::ConfigIssue::none);
    // step169: a 108-char value (stwatch + sttrack list) was refused at capacity 100 and took the whole config down; 191 chars fit, 192 do not
    {
        const std::string base = "auto;anim:1;default:j35jk_vrcockpit;zpre:1;cscale:1.15;stwatch:1;sttrack:67D886,67D91E,67D9B6,67DA4E,67DAE6";
        auto r2 = acz::diagnostics::parse_config("xr_cockpit_model=" + base + "\n");
        CHECK(r2.issue == acz::diagnostics::ConfigIssue::none && std::string(r2.config.xr_cockpit_model()) == base);
        CHECK(acz::diagnostics::parse_config("xr_cockpit_model=" + base + std::string(191 - base.size(), '0') + "\n").issue == acz::diagnostics::ConfigIssue::none);
        CHECK(acz::diagnostics::parse_config("xr_cockpit_model=" + base + std::string(192 - base.size(), '0') + "\n").issue != acz::diagnostics::ConfigIssue::none);
    }
    // the regular parser keeps treating a fixed id exactly as before (auto is not a model file id for it)
    { std::string id; acz::cockpit::Options o; CHECK(acz::cockpit::parse_options("j35jk_vrcockpit;anim:1", id, o, why) && id == "j35jk_vrcockpit" && o.anim); }
}

// ------------------------------------------------------------------------------------------------ AIRCRAFT-AUTO-FAST
// Window 12 vs 24: false-match risk on the nearest distinct pairs (table gaps e18g/m29a 0.1616, f02a/f16c 0.224, su27/su37 0.300). Samples = the
// true eye + gaussian noise per axis (aircraft frame); 4000 windows per case through Detector::evaluate. A WRONG type needs the median to move
// > gap - 0.08 (0.0816 for e18g/m29a) towards the neighbour; the median of n samples has sigma ~1.25 s / sqrt(n) per axis.
static void test_window_risk() {
    struct Pair { const char* a; const char* b; };
    const Pair pairs[] = {{"e18g", "m29a"}, {"m29a", "e18g"}, {"f02a", "f16c"}, {"f16c", "f02a"}, {"su27", "su37"}, {"su37", "su27"}};
    for (unsigned n : {24u, 12u}) {
        for (double sigma : {0.02, 0.03, 0.05, 0.08}) {
            unsigned wrong = 0, nomatch = 0, unstable = 0, total = 0;
            for (const Pair& p : pairs) {
                const int t = find_type(p.a);
                Rng g(uint64_t(n * 1000 + unsigned(sigma * 1000)) + uint64_t(t));
                std::vector<float> xyz(size_t(n) * 3);
                DetectOptions o; o.window = n;
                for (int w = 0; w < 4000; ++w) {
                    for (unsigned k = 0; k < n; ++k) for (int i = 0; i < 3; ++i) xyz[3 * k + i] = float(kAircraft[t].eye[i] + sigma * g.gauss());
                    const WindowResult r = Detector::evaluate(xyz, o);
                    ++total;
                    if (r.verdict == Verdict::match && r.chosen != t) ++wrong;
                    else if (r.verdict == Verdict::no_match) ++nomatch;
                    else if (r.verdict == Verdict::unstable) ++unstable;
                }
            }
            std::printf("  window %2u, noise %.2f u/axis: %u windows, wrong type %u, no-match %u, unstable %u\n", n, sigma, total, wrong, nomatch, unstable);
            if (sigma <= 0.05) CHECK(wrong == 0);   // 0.08 u/axis (3-8x the live spread) is reported, not bounded
            if (sigma <= 0.03) CHECK(nomatch == 0 && unstable == 0);
        }
    }
}

static void test_assume() {
    const int j35 = find_type("j35j"), f16 = find_type("f16c");
    Rng g(31);
    // with file / default, right aircraft: current() is the assumption from the start; the first matching window confirms (no switch)
    { Detector d; d.assume(j35);
      CHECK(d.current() == j35 && d.assumed() && !d.confirmed() && d.switches == 0);
      for (int k = 0; k < int(W) - 1; ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(d.current() == j35 && d.assumed());
      feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(d.current() == j35 && !d.assumed() && d.confirmed() && !d.changed() && d.switches == 0 && d.windows == 1);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(!d.confirmed()); }   // one confirmation event only
    // wrong remembered aircraft: one contradicting window is pending, the second switches (hysteresis kept for switches)
    { Detector d; d.assume(f16);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(d.current() == f16 && d.assumed() && !d.changed());
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(d.current() == j35 && !d.assumed() && d.changed() && d.switches == 1); }
    // an interrupted contradiction (assumed type in between) resets the streak
    { Detector d; d.assume(f16);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
      CHECK(d.current() == f16 && d.confirmed());
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 1);
      CHECK(d.current() == f16); }
    // no-match never drops an ASSUMED type (only a confirmed other aircraft replaces it) ...
    { Detector d; d.assume(j35); const float odd[3] = {0, 12, -30};
      for (int k = 0; k < 5 * int(W); ++k) feed_sample(d, fake(odd, g, 0.02), 1);
      CHECK(d.current() == j35 && d.assumed() && d.switches == 0); }
    // ... a new lock epoch keeps it too; out-of-range assumptions are ignored (the plain detector)
    { Detector d; d.assume(j35);
      for (int k = 0; k < 5; ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[j35].eye, g, 0.02), 2);
      CHECK(d.current() == j35 && d.confirmed() && d.epochs == 2); }
    { Detector d; d.assume(-1); d.assume(int(kAircraftCount)); CHECK(d.current() == -1 && !d.assumed());
      for (int k = 0; k < int(W); ++k) feed_sample(d, fake(kAircraft[f16].eye, g, 0.02), 1);
      CHECK(d.current() == f16 && d.switches == 1 && !d.confirmed()); }   // cold start: the first window chooses (unchanged)
}

static void test_default_and_remembered() {
    const int j35 = find_type("j35j");
    // stale: in the config value (AIRCRAFT-AUTO-STALE) and Detector::forget
    { AutoConfig c; std::string why;
      CHECK(parse_auto("auto", c, why) && c.stale_ms == 15000);
      CHECK(parse_auto("auto;stale:4000;default:j35j", c, why) && c.stale_ms == 4000);
      CHECK(parse_auto("auto;stale:0", c, why) && c.stale_ms == 0);
      // AIRCRAFT-AUTO-GRACE: extra time an unconfirmed (remembered / default) model is trusted
      CHECK(parse_auto("auto", c, why) && c.grace_ms == 60000);
      CHECK(parse_auto("auto;grace:0", c, why) && c.grace_ms == 0);
      CHECK(parse_auto("auto;stale:4000;grace:120000;default:j35j", c, why) && c.stale_ms == 4000 && c.grace_ms == 120000);
      for (const char* bad : {"auto;grace:", "auto;grace:-1", "auto;grace:600001", "auto;grace:abc"}) { AutoConfig d; CHECK(!parse_auto(bad, d, why)); }
      for (const char* bad : {"auto;stale:", "auto;stale:-1", "auto;stale:600001", "auto;stale:abc", "auto;stale:1234567"}) { AutoConfig d; CHECK(!parse_auto(bad, d, why)); }
      Detector d; d.assume(find_type("j35j")); CHECK(d.current() == find_type("j35j") && d.assumed());
      d.forget(); CHECK(d.current() == -1 && !d.assumed() && d.forgets == 1);
      const int f05 = find_type("f05e"); const float* e = kAircraft[f05].eye; bool done = false;
      for (unsigned i = 0; i < d.options().window && !done; ++i) done = d.feed(e, true, 2);
      CHECK(done && d.current() == f05 && d.changed());
    }
    // default: in the config value
    { AutoConfig c; std::string why;
      CHECK(parse_auto("auto;anim:1;default:j35jk_vrcockpit", c, why) && c.default_type == j35 && c.default_text == "j35jk_vrcockpit" && c.opt.anim);
      CHECK(parse_auto("auto;default:f16c", c, why) && c.default_type == find_type("f16c"));
      CHECK(parse_auto("auto;default=su47k_vrcockpit;prefer:f15e", c, why) && c.default_type == find_type("su47") && c.prefer != 0);
      CHECK(parse_auto("auto", c, why) && c.default_type == -1 && c.default_text.empty());
      for (const char* bad : {"auto;default:", "auto;default:zzzz_vrcockpit", "auto;default:..\\..\\evil", "auto;default:C:\\x\\j35jk_vrcockpit",
                              "auto;default:models/j35jk_vrcockpit", "auto;default:j35jk_vrcockpit.zvrm", "auto;default:%TEMP%", "auto;default:j35jk_cockpit",
                              "auto;default:j35jk_vrcockpit_with_a_very_long_tail"}) {
          AutoConfig x; std::string w; CHECK(!parse_auto(bad, x, w) && !w.empty());
      }
      auto r = acz::diagnostics::parse_config("xr_cockpit_model=auto;anim:1;default:j35jk_vrcockpit\n");
      CHECK(r.issue == acz::diagnostics::ConfigIssue::none && std::string(r.config.xr_cockpit_model()) == "auto;anim:1;default:j35jk_vrcockpit");
      CHECK(find_type_or_model("j35j") == j35 && find_type_or_model("j35jk_vrcockpit") == j35 && find_type_or_model("../j35j") == -1 && find_type_or_model("") == -1); }
    // the file content: round trip for every type; refusals
    for (unsigned t = 0; t < kAircraftCount; ++t) { int got = -2; std::string w; CHECK(parse_remembered(remembered_json(t), got, w) && got == int(t)); }
    CHECK(remembered_json(kAircraftCount).empty());
    { int got = 5; std::string w;
      CHECK(parse_remembered("  {\"type\" : \"j35j\",\n \"model\":\"j35jk_vrcockpit\", \"version\": 1}\r\n", got, w) && got == j35);
      const char* bad[] = {"", "{}", "not json", "{\"type\":\"j35j\"}", "{\"model\":\"j35jk_vrcockpit\"}", "{\"type\":\"zzzz\",\"model\":\"zzzzk_vrcockpit\"}",
          "{\"type\":\"j35j\",\"model\":\"f16ck_vrcockpit\"}",                    // model is not the type's table model
          "{\"type\":\"j35j\",\"model\":\"..\\\\..\\\\x\"}", "{\"type\":\"j35j\",\"model\":\"../../x\"}", "{\"type\":\"j35j\",\"model\":\"C:\\\\x\"}",
          "{\"type\":\"j35j\",\"model\":\"j35jk_vrcockpit.zvrm\"}", "{\"type\":\"j35j\",\"model\":\"%APPDATA%\"}", "{\"type\":\"j3\\u0035j\",\"model\":\"j35jk_vrcockpit\"}",
          "{\"type\":\"j35j\",\"type\":\"f16c\",\"model\":\"j35jk_vrcockpit\"}", "{\"type\":j35j,\"model\":\"j35jk_vrcockpit\"}", "{\"type\":\"j35j\",\"model\":\"j35jk_vrcockpit\""};
      for (const char* b : bad) { got = 5; w.clear(); CHECK(!parse_remembered(b, got, w) && got == -1 && !w.empty()); }
      std::string big = remembered_json(unsigned(j35)); big.insert(1, 600, ' ');
      CHECK(!parse_remembered(big, got, w) && w == "too-large");
      std::string nul = remembered_json(unsigned(j35)); nul[3] = '\0';
      CHECK(!parse_remembered(nul, got, w)); }
    // on disk: missing -> no-file; write (temp + rename) -> read back; overwrite; corrupt / oversized / directory -> refused
    {
        const char* tmpdir = std::getenv("TEMP");
        CHECK(tmpdir && *tmpdir);
        const std::string dir = std::string(tmpdir) + "\\acz-vr-aircraft-fast-test-" + std::to_string(unsigned(std::rand()) ^ unsigned(reinterpret_cast<uintptr_t>(&g_checks)));
        const std::string path = dir + "\\sub\\last-aircraft.json";
        int got = 7; std::string w;
        CHECK(!read_remembered(path, got, w) && w == "no-file" && got == -1);
        CHECK(!read_remembered("", got, w) && w == "no-path");
        CHECK(write_remembered(path, unsigned(j35), w));
        CHECK(read_remembered(path, got, w) && got == j35);
        CHECK(read_file(path.c_str()) == remembered_json(unsigned(j35)));
        CHECK(write_remembered(path, unsigned(find_type("f16c")), w) && read_remembered(path, got, w) && got == find_type("f16c"));   // replace
        { std::ifstream probe(path + ".tmp"); CHECK(!probe.good()); }   // no temp file left behind
        CHECK(!write_remembered(path, kAircraftCount, w) && w == "bad-type");
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << "{\"type\":\"j35j\",\"mod"; }
        CHECK(!read_remembered(path, got, w) && got == -1);
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << std::string(4096, '{'); }
        CHECK(!read_remembered(path, got, w) && w == "too-large");
        CHECK(!read_remembered(dir + "\\sub", got, w) && w == "not-a-file");
        // resolution at XR start: valid file > default: > none; a corrupt or injected file falls back to default: (never to its content)
        { AutoConfig withdef, nodef; CHECK(parse_auto("auto;default:j35jk_vrcockpit", withdef, w) && parse_auto("auto", nodef, w));
          int rem = 9; std::string src;
          CHECK(resolve_assumed(withdef, path, rem, src, w) == j35 && rem == -1 && src == "config-default" && w == "too-large");   // oversized file from above
          CHECK(resolve_assumed(nodef, path, rem, src, w) == -1 && rem == -1 && src.empty());
          { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << "{\"type\":\"f16c\",\"model\":\"..\\\\..\\\\..\\\\Windows\\\\evil\"}"; }
          CHECK(resolve_assumed(withdef, path, rem, src, w) == j35 && rem == -1 && w == "path-characters-refused");
          { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << "{\"type\":\"f16c\",\"model\":\"f16ck_vrcockpit\"}"; }
          CHECK(resolve_assumed(withdef, path, rem, src, w) == find_type("f16c") && rem == find_type("f16c") && src == "remembered-file");
          CHECK(resolve_assumed(nodef, dir + "\\none.json", rem, src, w) == -1 && w == "no-file"); }
        std::error_code ec; std::filesystem::remove_all(dir, ec);
        const std::string rp = remembered_path();
        if (std::getenv("LOCALAPPDATA")) CHECK(rp.size() >= 26 && rp.compare(rp.size() - 26, 26, "\\acz-vr\\last-aircraft.json") == 0);
        std::printf("  remembered file: write/replace/read ok; corrupt, oversized, directory and path-injection contents refused\n");
    }
}

// ACZ_MODELS_ALL=<dir> (e.g. D:\acz-extract\models-all): every table model through the runtime loader + mesh builder with the auto options.
static void test_real_models() {
    const char* dir = std::getenv("ACZ_MODELS_ALL");
    if (!dir || !*dir) { std::printf("  real models: SKIP (set ACZ_MODELS_ALL=<dir with the bake_all_aircraft.py output>)\n"); return; }
    AutoConfig c; std::string why; CHECK(parse_auto("auto;anim:1", c, why));   // anim:1 also reports the driven control surfaces
    unsigned ok = 0;
    for (unsigned t = 0; t < kAircraftCount; ++t) {
        acz::cockpit::ModelData m; acz::cockpit::Mesh mesh; std::string w;
        const std::string path = acz::cockpit::model_path(kAircraft[t].model_id, dir);
        const auto st = acz::cockpit::load_model_file(path, m);
        const bool built = st == acz::cockpit::LoadStatus::ok && acz::cockpit::build_mesh(m, options_for(c, t), mesh, w);
        float de = 0; for (int i = 0; i < 3; ++i) de = std::max(de, std::fabs(m.eye[i] - kAircraft[t].eye[i]));
        std::printf("  real %-16s %s parts=%zu tris=%u drawn=%u hud=%s canopy=%u%s interior=%u anim=%u/%u eye-diff=%.5f%s\n", kAircraft[t].model_id,
            built ? "ok  " : acz::cockpit::status_name(st), m.parts.size(), mesh.triangles, mesh.parts_drawn, mesh.hud.valid ? "found" : "none",
            mesh.canopy_parts, mesh.canopy_exterior ? "(ext)" : "", mesh.interior_triangles, mesh.anim_surfaces, mesh.anim_unlocated, double(de), built ? "" : (" " + w).c_str());
        if (built && m.has_eye && de < 1e-4f) ++ok;
        // STORES (headset test 7, F-5E: 'the missiles aren't showing on the racks'): the weapons are separate game models the bake adds at the weapon-station
        // locators (research/world-render-data/bake_all_aircraft.py STORES): 4 missiles x (body + nozzle) = 8 drawn exterior parts named '<model>/store<station>/..'
        // with source part 7, all inside the GPU texture budget. The pre-STORES bake (racks only) fails this check. STORES-RT: every station is one mask piece in the
        // built mesh (Mesh::store_stations), so a fired weapon can be hidden. TANK: the F-5E belly tank (mpl part 7) is '<model>/tank79/n0', never a mask station.
        auto store_parts = [&](const char* what) {
            unsigned n = 0, tris = 0; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (size_t i = 0; i < m.parts.size(); ++i) {
                const auto& p = m.parts[i];
                if (p.source_part != 7 || !p.index_count || (p.mflags & acz::cockpit::MF_HIDDEN) || !(p.mflags & acz::cockpit::MF_EXTERIOR)) continue;
                if (m.part_name(i).find(what) == std::string::npos) continue;
                ++n; tris += p.index_count / 3;
                for (uint32_t k = 0; k < p.index_count; ++k) {
                    const float* q = m.vertices[m.indices[p.first_index + k]].pos;
                    for (int j = 0; j < 3; ++j) { lo[j] = std::min(lo[j], q[j]); hi[j] = std::max(hi[j], q[j]); }
                }
            }
            return std::make_tuple(n, tris, std::array<float, 6>{lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]});
        };
        if (built) {
            size_t pieces = 0;
            for (const auto& a : mesh.anim) if (a.role == acz::cockpit::AnimRole::store) ++pieces;
            std::printf("  %s stores: %zu stations + %zu sp stations, %zu mask pieces\n", kAircraft[t].type, mesh.store_stations.size(), mesh.sp_stations.size(), pieces);
            CHECK(pieces == mesh.store_stations.size() + mesh.sp_stations.size());
            if (std::string(kAircraft[t].type) != "f05e") CHECK(mesh.sp_stations.empty());   // SP-STORES: only the F-5E carries a special-weapon group so far
        }
        if (built && std::string(kAircraft[t].type) == "f05e") {
            // SP-STORES (aircraft-auto.md section 15): the special weapon (SAAM = ex.bin entry 0) hangs on stations 67/68 as '<model>/sp<station>/..' and the standard
            // group keeps 75/76 (no locator carries two weapons): 2 stations x (body + nozzle) each; the pre-SP bake had store67/68/75/76 and no sp parts.
            const auto [stores, stores_tris, b] = store_parts("/store");
            const auto [sps, sp_tris, sb] = store_parts("/sp");
            std::printf("  f05e stores: %u std + %u sp drawn weapon parts, %u + %u triangles, textures %zu (GPU limit %u)\n", stores, sps, stores_tris, sp_tris, mesh.textures.size(), acz::cockpit::kGpuTextures);
            CHECK(stores == 4 && sps == 4 && stores_tris > 200 && sp_tris == stores_tris);   // the same weapon model on both groups
            CHECK(mesh.textures.size() <= acz::cockpit::kGpuTextures && mesh.textures.size() == 3);   // one weapon atlas shared by both groups
            CHECK(b[1] < -4.0f && b[4] < -2.5f && sb[1] < -4.0f && sb[4] < -2.5f);   // under the wing (the pylon plates end at y -4.1): hangs on the racks, not in the airframe
            CHECK(b[0] < -11.0f && b[3] > 11.0f && b[0] > -13.5f && b[3] < 13.5f);   // standard group: the inboard pylons at |x| 11.8 (missile fins +-1.1)
            CHECK(sb[0] < -15.0f && sb[3] > 15.0f && sb[0] > -17.0f && sb[3] < 17.0f);   // SP group: both wings, the outboard pylons at |x| 15.4
            CHECK(sb[2] < -10.0f && sb[5] > 2.0f && sb[5] < 5.0f && b[2] < -10.0f && b[5] > 2.0f && b[5] < 5.0f);   // body ahead of the station (nose toward -z), tail just aft of it
            CHECK(mesh.store_stations.size() == 2 && mesh.store_stations[0] == 75 && mesh.store_stations[1] == 76);   // STORES-RT: one mask piece per station
            CHECK(mesh.sp_stations.size() == 2 && mesh.sp_stations[0] == 67 && mesh.sp_stations[1] == 68);             // SP-STORES: its own mask pieces
            const auto [tanks, tank_tris, tb] = store_parts("/tank79/");
            std::printf("  f05e tank: %u part, %u triangles, y %.1f..%.1f z %.1f..%.1f\n", tanks, tank_tris, double(tb[1]), double(tb[4]), double(tb[2]), double(tb[5]));
            CHECK(tanks == 1 && tank_tris == 216);                 // mpl part 7: one node, 216 triangles
            CHECK(tb[0] > -3.5f && tb[3] < 3.5f && tb[1] < -2.0f);   // on the centre line, below the airframe's belly line
            CHECK(tb[2] < -15.0f && tb[5] > -5.0f);                // 5.3 m long, nose forward of the locator (z -8.6)
        }
        if (built && std::string(kAircraft[t].type) == "j35j") {   // the Draken the null batches fly: four parallel missiles, two per wing (stations 71..74)
            const auto [stores, stores_tris, b] = store_parts("/store");
            std::printf("  j35j stores: %u drawn weapon parts, %u triangles\n", stores, stores_tris);
            CHECK(stores == 8 && stores_tris > 400 && mesh.textures.size() <= acz::cockpit::kGpuTextures);
            CHECK(mesh.store_stations.size() == 4 && mesh.store_stations[0] == 71 && mesh.store_stations[3] == 74);
            CHECK(b[0] < -14.0f && b[3] > 14.0f && b[4] < -2.0f);   // outer stations at |x| 15, under the wing
        }
        if (built && (std::string(kAircraft[t].type) == "e06b" || std::string(kAircraft[t].type) == "f16c")) CHECK(mesh.store_stations.empty());   // no pair proposed: unchanged
    }
    std::printf("  real models: %u/%u load + build with the table eye\n", ok, kAircraftCount);
    CHECK(ok == kAircraftCount);
}

int main() {
    test_table();
    test_real_samples();
    test_all_types();
    test_ties_and_edges();
    test_switch_and_hysteresis();
    test_config();
    test_window_risk();
    test_assume();
    test_default_and_remembered();
    test_real_models();
    std::printf("aircraft_auto_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
