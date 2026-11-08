// LAUNCH-MODE + NATIVE-VR START offline tests (docs/world-render/launch-mode.md): the launch-mode.txt format, the On/Off latch decision, the
// launcher combo layout (rectangles measured from research/experiments/step75-real-steamvr-run1/shots/launcher.png), the vsstart:/ACZ_XR_START
// start gate and the vscreen option token. Header-only cores; no window, no device, no headset.
#include "launch_mode.h"
#include "xr_start_gate.h"
#include "vscreen.h"
#include "flight_cycle.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace acz::launch_mode;

void test_file_parse() {
    // the default: missing file is On (state missing comes from the Win32 reader; the parser sees text only)
    FileValue v = parse_file("");
    CHECK(v.state == FileState::unrecognised && v.mode == VrMode::on);
    v = parse_file("vr=off\r\n");
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("vr=on");
    CHECK(v.state == FileState::ok && v.mode == VrMode::on);
    v = parse_file("  VR = OFF  \n");
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("off");
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("0");
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("vr_mode:no");
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("vrmode=yes");
    CHECK(v.state == FileState::ok && v.mode == VrMode::on);
    v = parse_file("\xEF\xBB\xBFvr=off\r\n");                       // UTF-8 BOM (Notepad)
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("# comment\r\n; another\r\n\r\n   \r\nvr=off\r\nvr=on\r\n");   // the first deciding line wins
    CHECK(v.state == FileState::ok && v.mode == VrMode::off);
    v = parse_file("vr=maybe\n");                                     // unrecognised value -> On
    CHECK(v.state == FileState::unrecognised && v.mode == VrMode::on);
    v = parse_file("mode=off\n");                                     // wrong key -> On (never guess)
    CHECK(v.state == FileState::unrecognised && v.mode == VrMode::on);
    v = parse_file("# only comments\n#vr=off\n");
    CHECK(v.state == FileState::unrecognised && v.mode == VrMode::on);
    // round trip of what the launcher writes
    CHECK(parse_file(format_file(VrMode::off)).mode == VrMode::off && parse_file(format_file(VrMode::off)).state == FileState::ok);
    CHECK(parse_file(format_file(VrMode::on)).mode == VrMode::on && parse_file(format_file(VrMode::on)).state == FileState::ok);
    CHECK(format_file(VrMode::off).rfind("vr=off\r\n", 0) == 0);
    CHECK(std::strcmp(file_state_name(FileState::missing), "missing") == 0);
}

void test_decide() {
    DecideInput in;
    // no launcher seen, no file: On, latched
    in.file.state = FileState::missing;
    Decision d = decide(in);
    CHECK(d.mode == VrMode::on && d.source == Source::default_on && d.latch);
    // file says off, launcher gone (or SkipLauncher=True): Off, latched
    in.file = FileValue{VrMode::off, FileState::ok};
    d = decide(in);
    CHECK(d.mode == VrMode::off && d.source == Source::file && d.latch);
    // unreadable / unrecognised file: On
    in.file = FileValue{VrMode::off, FileState::unrecognised};
    d = decide(in);
    CHECK(d.mode == VrMode::on && d.source == Source::default_on && d.latch);
    // the launcher's selection wins over the file
    in.file = FileValue{VrMode::off, FileState::ok};
    in.has_selection = true; in.selection = VrMode::on;
    d = decide(in);
    CHECK(d.mode == VrMode::on && d.source == Source::launcher && d.latch);
    in.file = FileValue{VrMode::on, FileState::ok};
    in.selection = VrMode::off;
    d = decide(in);
    CHECK(d.mode == VrMode::off && d.source == Source::launcher && d.latch);
    // a factory call WHILE the launcher still shows Off: pass through, not latched (the user may still switch to On)
    in.launcher_showing = true;
    d = decide(in);
    CHECK(d.mode == VrMode::off && !d.latch);
    // ... but On while showing latches at once (a factory made during the launcher is wrapped exactly as before)
    in.selection = VrMode::on;
    d = decide(in);
    CHECK(d.mode == VrMode::on && d.latch);
    // Off from the file while the launcher shows (combo injection refused): also provisional
    in.has_selection = false; in.file = FileValue{VrMode::off, FileState::ok};
    d = decide(in);
    CHECK(d.mode == VrMode::off && d.source == Source::file && !d.latch);
}

// Client coordinates of the launcher at 2560x1440 (physical pixels, step75 launcher.png x2): two combo columns with a 60 px pitch, the
// Launch Game button in the right column, client 798x598.
LayoutInput launcher() {
    LayoutInput in;
    in.client = Rect{0, 0, 798, 598};
    in.combos = {Rect{68, 342, 268, 366}, Rect{398, 342, 698, 366}, Rect{68, 402, 268, 426}, Rect{398, 402, 698, 426}};
    in.others = {Rect{399, 461, 487, 479}};   // Launch Game
    return in;
}

void test_layout() {
    Layout L = place(launcher());
    CHECK(L.ok);
    CHECK(L.pitch == 60);
    CHECK(L.combo.l == 68 && L.combo.r == 268 && L.combo.t == 462 && L.combo.b == 486);   // one row below Display Resolution, beside Launch Game
    CHECK(L.label.l == 68 && L.label.t == 426 && L.label.b == 462);                       // the gap above it, like the launcher's labels
    // order of the children does not matter
    LayoutInput in = launcher();
    std::swap(in.combos[0], in.combos[3]);
    L = place(in);
    CHECK(L.ok && L.combo.t == 462 && L.combo.l == 68);
    // refusals (the fallback is launch-mode.txt / tools/acz-vr-mode.ps1)
    in = launcher();
    in.combos.resize(1);
    CHECK(!place(in).ok && std::strcmp(place(in).why, "fewer-than-2-combos") == 0);
    in = launcher();
    in.others.push_back(Rect{60, 470, 200, 490});            // something already there
    CHECK(!place(in).ok && std::strcmp(place(in).why, "overlaps-a-child") == 0);
    in = launcher();
    in.client.b = 480;                                        // no room below
    CHECK(!place(in).ok && std::strcmp(place(in).why, "outside-client") == 0);
    in = launcher();
    in.combos = {Rect{68, 342, 268, 366}, Rect{398, 342, 698, 366}};   // one combo per column
    CHECK(!place(in).ok && std::strcmp(place(in).why, "left-column-has-fewer-than-2-combos") == 0);
    in = launcher();
    in.combos[2] = Rect{68, 368, 268, 392};                   // overlapping rows: no regular pitch
    CHECK(!place(in).ok && std::strcmp(place(in).why, "irregular-pitch") == 0);
    // a third left combo (a future launcher) still lands below the last one
    in = launcher();
    in.combos.push_back(Rect{68, 462, 268, 486});
    in.client.b = 700;
    L = place(in);
    CHECK(L.ok && L.combo.t == 522);
}

void test_start_gate() {
    using namespace acz::xr_gate;
    StartMode m{};
    CHECK(parse_start_mode("first", m) && m == StartMode::first);
    CHECK(parse_start_mode("steady", m) && m == StartMode::steady);
    CHECK(parse_start_mode("lazy", m) && m == StartMode::lazy);
    CHECK(!parse_start_mode("First", m) && !parse_start_mode("", m) && !parse_start_mode(nullptr, m) && !parse_start_mode("first ", m));
    // first: the first counted Present starts XR at once, whatever the time since armed
    CHECK(!early_ready(StartMode::first, 0, 100000));
    CHECK(early_ready(StartMode::first, 1, 0));
    // steady: exactly the previous gate (120 presents AND 3000 ms)
    CHECK(!early_ready(StartMode::steady, 119, 10000));
    CHECK(!early_ready(StartMode::steady, 500, 2999));
    CHECK(early_ready(StartMode::steady, 120, 3000));
    CHECK(early_ready(StartMode::steady, 120, 3000) == acz::flight::early_start_ready(120, 3000));
    CHECK(early_ready(StartMode::steady, 119, 3000) == acz::flight::early_start_ready(119, 3000));
    // lazy: never early
    CHECK(!early_ready(StartMode::lazy, 100000, 100000));
    // decide(): the producer's frame always starts (the lazy start), the early start only when vsearly is effective
    Input in;
    in.presents = 1; in.since_armed_ms = 50;
    CHECK(decide(StartMode::first, in) == Fire::wait);                  // vsearly not effective: the old lazy path only
    in.early_effective = true;
    CHECK(decide(StartMode::first, in) == Fire::early);
    CHECK(decide(StartMode::steady, in) == Fire::wait);
    CHECK(decide(StartMode::lazy, in) == Fire::wait);
    in.source_ready = true;
    CHECK(decide(StartMode::lazy, in) == Fire::source && decide(StartMode::first, in) == Fire::source);
    in = Input{}; in.early_effective = true;
    CHECK(decide(StartMode::first, in) == Fire::wait);                  // no present yet (device created, swap chain not presented)
    // the state sequence of a launch: launcher (no device: the monitor does not exist yet), device armed, first Present -> early
    {
        int fired_at = -1;
        for (int poll = 0; poll < 40; ++poll) {                         // 100 ms polls
            Input p; p.early_effective = true; p.since_armed_ms = poll * 100;
            p.presents = poll < 4 ? 0 : uint64_t(poll - 3) * 6;         // the first Present ~0.35 s after armed, then 60 fps
            if (decide(StartMode::first, p) == Fire::early) { fired_at = poll; break; }
        }
        CHECK(fired_at == 4);                                           // the poll right after the first Present
        int steady_at = -1;
        for (int poll = 0; poll < 60; ++poll) {
            Input p; p.early_effective = true; p.since_armed_ms = poll * 100;
            p.presents = poll < 4 ? 0 : uint64_t(poll - 3) * 6;
            if (decide(StartMode::steady, p) == Fire::early) { steady_at = poll; break; }
        }
        CHECK(steady_at == 30);                                         // the old gate: 3 s (presents passed 120 at poll 23)
    }
    // the mode resolution: default first, the token, the environment override
    Resolved r = resolve(false, StartMode::first, nullptr);
    CHECK(r.mode == StartMode::first && std::strcmp(r.source, "default") == 0);
    r = resolve(true, StartMode::steady, nullptr);
    CHECK(r.mode == StartMode::steady && std::strcmp(r.source, "option") == 0);
    r = resolve(true, StartMode::steady, "lazy");
    CHECK(r.mode == StartMode::lazy && std::strcmp(r.source, "env(ACZ_XR_START)") == 0);
    r = resolve(false, StartMode::first, "steady");
    CHECK(r.mode == StartMode::steady);
    r = resolve(true, StartMode::steady, "bogus");
    CHECK(r.mode == StartMode::steady && std::strcmp(r.source, "option(bad-env-ignored)") == 0);
    r = resolve(false, StartMode::first, "");
    CHECK(r.mode == StartMode::first && std::strcmp(r.source, "default") == 0);
    CHECK(std::strcmp(start_rule(StartMode::steady), "presents>=120-and-armed>=3000ms") == 0);
    CHECK(std::strcmp(start_rule(StartMode::first), "first-game-present(presents>=1)") == 0);
}

void test_vscreen_token() {
    namespace vs = acz::vscreen;
    vs::Options o; std::string rest, why;
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;vrn:1";
    CHECK(vs::extract_options(day, rest, o, why));
    CHECK(o.early && o.start == acz::xr_gate::StartMode::first && !o.start_given);   // the user's day value: the new default
    CHECK(rest == "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;fxmem:6;brief3d:1;fxsig:f1a4e000;vrn:1");
    CHECK(vs::extract_options(day + ";vsstart:steady", rest, o, why));
    CHECK(o.start == acz::xr_gate::StartMode::steady && o.start_given);
    CHECK(rest.find("vsstart") == std::string::npos);                              // removed before the other parsers
    CHECK(vs::extract_options(day + ";vsstart=lazy", rest, o, why) && o.start == acz::xr_gate::StartMode::lazy);
    CHECK(!vs::extract_options(day + ";vsstart:soon", rest, o, why));
    CHECK(why == "bad value for vsstart (first|steady|lazy)");
    CHECK(vs::extract_options(day + ";vsstart:first", rest, o, why));
    const std::string de = vs::describe(o);
    CHECK(de.find("vsearly=1;early=see-vsearly-config(vsearly.md);vsstart=first") != std::string::npos);
    CHECK(vs::extract_options("auto;vscreen:1", rest, o, why) && vs::describe(o).find("vsstart") == std::string::npos);   // no vsearly: unchanged text
}
} // namespace

int main() {
    test_file_parse();
    test_decide();
    test_layout();
    test_start_gate();
    test_vscreen_token();
    std::printf("launch_mode_offline: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
