// terrain_preset_offline_tests.cpp -- D-400: the xr_terrain_stage preset token vrn:1 (terrain_preset.h), expanded in the config reader.
#include "terrain_preset.h"
#include "runtime_config.h"
#include "ui3d.h"
#include "view_mode.h"
#include "pov_render.h"
#include "dbr3d.h"
#include "brief3d.h"
#include "vscreen.h"
#include "frame_pacing.h"
#include "recenter.h"
#include "look_stick_option.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using acz::diagnostics::ConfigIssue;
using acz::diagnostics::parse_config;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static const char* kHead = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;";
static const char* kTail = "ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;pov:3;povrig:level";

static std::string stage_of(const std::string& value) {
    const auto r = parse_config(("xr_terrain_render=true\nxr_terrain_stage=" + value + "\n").c_str());
    CHECK(r.issue == ConfigIssue::none);
    return r.config.xr_terrain_stage();
}

// Every extractor's (ok, rest) over one string, concatenated: equal strings give equal text; the point of the test is that the
// real extractors accept the expanded form and strip the same tokens as in the long form.
template <class O> static std::string run(bool (*f)(const std::string&, std::string&, O&, std::string&), const std::string& v) {
    O o; std::string rest, why; const bool ok = f(v, rest, o, why);
    return std::string(ok ? "1:" : "0:") + rest + "|";
}
static std::string all_extractors(const std::string& v) {
    std::string s;
    s += run<acz::ui3d::Options>(&acz::ui3d::extract_options, v);
    s += run<acz::viewmode::Options>(&acz::viewmode::extract_options, v);
    s += run<acz::povr::Options>(&acz::povr::extract_options, v);
    s += run<acz::dbr3d::Options>(&acz::dbr3d::extract_options, v);
    s += run<acz::brief3d::Options>(&acz::brief3d::extract_options, v);
    s += run<acz::vscreen::Options>(&acz::vscreen::extract_options, v);
    s += run<acz::pacing::Options>(&acz::pacing::extract_options, v);
    s += run<acz::recenter::Options>(&acz::recenter::extract_options, v);
    { acz::look_stick::Mode m = acz::look_stick::Mode::block; std::string rest, why; const bool ok = acz::look_stick::extract_option(v, rest, m, why); s += (ok ? "1:" : "0:") + rest + "|"; }
    return s;
}

int main() {
    const std::string long_form = std::string(kHead) + kTail;
    CHECK(long_form.size() > 230 && long_form.size() < 255);   // the user's value: 241 characters
    CHECK(std::string(acz::preset::kVrn) == kTail);

    // expansion: replaced in place, and the stored value equals the long form byte for byte
    CHECK(stage_of(std::string(kHead) + "vrn:1") == long_form);
    CHECK(parse_config(("xr_terrain_stage=" + std::string(kHead) + "vrn:1\n").c_str()).config.xr_terrain_preset_vrn());
    CHECK(!parse_config(("xr_terrain_stage=" + long_form + "\n").c_str()).config.xr_terrain_preset_vrn());
    CHECK(stage_of("auto;vrn:1;env:1") == std::string("auto;") + kTail + ";env:1");   // in place, not appended
    CHECK(stage_of("vrn:1") == kTail);
    CHECK(stage_of("auto;vrn:0;env:1") == "auto;env:1");                              // vrn:0 = no expansion
    CHECK(!parse_config("xr_terrain_stage=auto;vrn:0\n").config.xr_terrain_preset_vrn());
    CHECK(stage_of("auto;env:1") == "auto;env:1");                                    // no token: untouched

    // explicit tokens override the preset's, wherever they sit; keys compare whole (pov vs povrig)
    CHECK(stage_of("auto;vrn:1;pov:0") == "auto;ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;povrig:level;pov:0");
    CHECK(stage_of("auto;pov:0;vrn:1") == "auto;pov:0;ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;povrig:level");
    CHECK(stage_of("auto;vrn:1;povrig:seat;fxcloud:60") == "auto;ui3d:1;u3p:1;u3a:1;fxtrail:8;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;pov:3;povrig:seat;fxcloud:60");

    // unknown / repeated preset values are refused (the config is rejected with a reason; nothing is stored)
    for (const char* bad : {"auto;vrn:2", "auto;vrn:", "auto;vrn", "auto;vrn:yes", "auto;vrn:1;vrn:1", "auto;vrn:1;vrn:0", "auto;vrn:11"}) {
        const auto r = parse_config((std::string("xr_terrain_stage=") + bad + "\n").c_str());
        CHECK(r.issue == ConfigIssue::out_of_range && std::string(r.reason) == "config xr terrain stage preset unknown");
    }
    // a key that merely starts with vrn is not the preset
    CHECK(stage_of("auto;vrnx:1") == "auto;vrnx:1");

    // the expanded value parses identically to the long form for every extractor, and a user override for a preset key behaves like the long form with that token
    CHECK(all_extractors(stage_of(std::string(kHead) + "vrn:1")) == all_extractors(long_form));
    CHECK(all_extractors(stage_of(std::string(kHead) + "vrn:1;pov:0")) == all_extractors(std::string(kHead) + "ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;povrig:level;pov:0"));
    CHECK(run<acz::ui3d::Options>(&acz::ui3d::extract_options, long_form).find("ui3d") == std::string::npos);   // an extractor removes its own tokens
    CHECK(all_extractors(long_form) != all_extractors(std::string(kHead) + "ui3d:1"));  // and the tokens matter (the comparison is not vacuous)

    // the short user value fits easily, a bare long-form value beyond 255 now also fits (limit raised to 1023), 1024 is refused
    CHECK(std::string("auto;vrn:1;pov:0").size() < 20);
    CHECK(stage_of(long_form + ";" + std::string(400, 'a')).size() == long_form.size() + 401);
    CHECK(parse_config(("xr_terrain_stage=" + std::string(1023, 'a') + "\n").c_str()).issue == ConfigIssue::none);
    CHECK(parse_config(("xr_terrain_stage=" + std::string(1024, 'a') + "\n").c_str()).issue != ConfigIssue::none);
    // expansion that would overflow the capacity is refused
    {
        const std::string near_full = "vrn:1;" + std::string(1023 - 6 - 1, 'a');   // 1016 chars raw; + expansion > 1023
        CHECK(near_full.size() <= 1023);
        CHECK(parse_config(("xr_terrain_stage=" + near_full + "\n").c_str()).issue == ConfigIssue::out_of_range);
    }
    std::printf("terrain_preset_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
