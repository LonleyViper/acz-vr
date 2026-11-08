// LOOK-STICK GATE offline fixtures (docs/world-render/look-stick-gate.md, D-355 problem 3): the lookstick option inside xr_terrain_stage, the
// heartbeat IPC block (src/skip-intro/xr_view_ipc.h: freshness, staleness, garbage, torn, inactive), the proxy's StickGate policy (right stick
// centred only while the heartbeat is fresh, packet bump on a gate change with the stick deflected) and the writer's Heartbeat decision, plus an
// end-to-end timeline (menus, flight, pause of the composite, session end, writer death). No OS object, no XInput, no game.
#include "look_stick_option.h"
#include "../skip-intro/xr_view_ipc.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace ipc = acz::xr_view_ipc;
static constexpr uint32_t kPid = 4242;   // the fixture's "current game process"
using acz::look_stick::Mode;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_option() {
    std::string rest, why; Mode m = Mode::game;
    // the day-install value (D-355): no lookstick option -> default block, the value reaches the next parser byte for byte
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1";
    CHECK(acz::look_stick::extract_option(day, rest, m, why) && rest == day && m == Mode::block);
    CHECK(acz::look_stick::extract_option("m02a", rest, m, why) && rest == "m02a" && m == Mode::block);
    CHECK(acz::look_stick::extract_option("auto;lookstick:game;env:1", rest, m, why) && rest == "auto;env:1" && m == Mode::game);
    CHECK(acz::look_stick::extract_option("auto;env:1;lookstick=block", rest, m, why) && rest == "auto;env:1" && m == Mode::block);
    CHECK(acz::look_stick::extract_option("auto;lookstick:0", rest, m, why) && rest == "auto" && m == Mode::game);
    CHECK(acz::look_stick::extract_option("auto;lookstick:1", rest, m, why) && rest == "auto" && m == Mode::block);
    CHECK(acz::look_stick::extract_option("auto;lookstick:game;lookstick:block", rest, m, why) && rest == "auto" && m == Mode::block);   // last wins
    // look:1 (WORLD-4 game look shading) is a different option and is never touched
    CHECK(acz::look_stick::extract_option("auto;look:1;lookfog:fit", rest, m, why) && rest == "auto;look:1;lookfog:fit" && m == Mode::block);
    // a bad value refuses (the terrain then refuses like any bad option)
    why.clear();
    CHECK(!acz::look_stick::extract_option("auto;lookstick:maybe", rest, m, why) && !why.empty());
    CHECK(!acz::look_stick::extract_option("auto;lookstick:", rest, m, why));
    // the first item is the stage id, never an option
    CHECK(acz::look_stick::extract_option("lookstick:game", rest, m, why) && rest == "lookstick:game" && m == Mode::block);
    CHECK(std::strcmp(acz::look_stick::mode_name(Mode::block), "block") == 0 && std::strcmp(acz::look_stick::mode_name(Mode::game), "game") == 0);
}

static void test_block() {
    ipc::Block b{};
    CHECK(!ipc::read_block(nullptr, 1000, 500, kPid).block);
    CHECK(std::string(ipc::read_block(&b, 1000, 500, kPid).why) == "bad-header");   // zeroed memory
    ipc::publish(&b, true, 1000, kPid);
    CHECK(ipc::read_block(&b, 1000, 500, kPid).block && std::string(ipc::read_block(&b, 1000, 500, kPid).why) == "in-flight");
    CHECK(ipc::read_block(&b, 1500, 500, kPid).block);                                // exactly the window: still fresh
    CHECK(!ipc::read_block(&b, 1501, 500, kPid).block && std::string(ipc::read_block(&b, 1501, 500, kPid).why) == "stale");
    CHECK(ipc::read_block(&b, 1060, 500, kPid).block && !ipc::read_block(&b, 900, 500, kPid).block);   // 40 ms ahead ok, 100 ms ahead = future
    CHECK(std::string(ipc::read_block(&b, 900, 500, kPid).why) == "future-stamp");
    CHECK(!ipc::read_block(&b, 1150, 1, kPid).block && ipc::read_block(&b, 1100, 1, kPid).block);      // window raised to 100 ms
    CHECK(ipc::read_block(&b, 3000, 99999, kPid).block && !ipc::read_block(&b, 3001, 99999, kPid).block); // window cut to 2000 ms
    ipc::publish(&b, false, 2000, kPid);
    CHECK(!ipc::read_block(&b, 2000, 500, kPid).block && std::string(ipc::read_block(&b, 2000, 500, kPid).why) == "inactive");
    ipc::publish(&b, true, 3000, kPid);
    b.flags = 0x7;                                                               // tampered payload
    CHECK(!ipc::read_block(&b, 3000, 500, kPid).block && std::string(ipc::read_block(&b, 3000, 500, kPid).why) == "bad-check");
    ipc::publish(&b, true, 3000, kPid);
    b.seq |= 1u;                                                                 // writer mid-update
    CHECK(!ipc::read_block(&b, 3000, 500, kPid).block && std::string(ipc::read_block(&b, 3000, 500, kPid).why) == "torn");
    b.seq += 1u;
    CHECK(ipc::read_block(&b, 3000, 500, kPid).block);
    b.version = 1;                                                               // the v1 (session-wide) layout
    CHECK(std::string(ipc::read_block(&b, 3000, 500, kPid).why) == "bad-header");
}

static void test_gate() {
    ipc::Block b{};
    ipc::StickGate g(500, kPid);
    // no heartbeat: the real stick passes, no packet bump
    auto r = g.apply(-20000, 9000, nullptr, 1000, true);
    CHECK(!r.blocked && r.rx == -20000 && r.ry == 9000 && r.packet_bump == 0 && std::string(r.why) == "absent");
    // fresh heartbeat: centred; the gate change with a deflected stick bumps the packet once
    ipc::publish(&b, true, 1000, kPid);
    r = g.apply(-20000, 9000, &b, 1010, true);
    CHECK(r.blocked && r.rx == 0 && r.ry == 0 && r.packet_bump == 1);
    r = g.apply(-32768, 32767, &b, 1020, true);                                  // full deflection, both extremes
    CHECK(r.blocked && r.rx == 0 && r.ry == 0 && r.packet_bump == 1);
    CHECK(g.blocked() == 2);
    r = g.apply(0, 0, &b, 1030, true);                                           // centred stick while blocked: not counted
    CHECK(r.blocked && r.rx == 0 && r.ry == 0 && g.blocked() == 2);
    // disabled in the ini: never gates, even with a fresh heartbeat
    r = g.apply(12000, 0, &b, 1040, false);
    CHECK(!r.blocked && r.rx == 12000 && std::string(r.why) == "disabled" && r.packet_bump == 2);   // gate opened with the stick deflected
    // stale heartbeat (writer gone): passes again
    r = g.apply(12000, 0, &b, 1600, true);
    CHECK(!r.blocked && r.rx == 12000 && std::string(r.why) == "stale" && r.packet_bump == 2);
    // a gate change with the stick centred does not bump (the reported state is the same)
    ipc::publish(&b, true, 2000, kPid);
    r = g.apply(0, 0, &b, 2000, true);
    CHECK(r.blocked && r.packet_bump == 2);
    // the left stick, buttons and triggers are not part of the policy at all (the proxy passes only sThumbRX/RY)
}

static void test_heartbeat() {
    ipc::Heartbeat h;
    CHECK(!h.tick(0, true));            // first tick: no history
    CHECK(!h.tick(0, true));            // menus: no in-flight composite
    CHECK(h.tick(5, true));             // in flight
    CHECK(h.tick(11, true) && h.active());
    CHECK(!h.tick(11, true));           // the composite stopped (no camera, stall): pass
    CHECK(h.tick(12, true));
    CHECK(!h.tick(20, false));          // lookstick:game or no hooked session: never
    CHECK(!h.active());
}

// The writer (acz-vr glue, 100 ms tick) and the reader (proxy, every poll) on one timeline.
static void test_timeline() {
    ipc::Block b{};
    ipc::Heartbeat hb;
    ipc::StickGate g(500, kPid);
    uint64_t composites = 0;
    bool was = false;
    auto writer = [&](uint64_t now, bool want) {
        const bool on = hb.tick(composites, want);
        if (on || was) ipc::publish(&b, on, now, kPid);
        was = on;
    };
    auto stick = [&](uint64_t now) { return g.apply(25000, -8000, &b, now, true); };
    // menus: XR hooked, no in-flight composites -> the stick works (hangar, aircraft select)
    for (uint64_t t = 0; t <= 1000; t += 100) writer(t, true);
    CHECK(!stick(1050).blocked);
    // flight: composites at ~90/s
    for (uint64_t t = 1100; t <= 3000; t += 100) { composites += 9; writer(t, true); CHECK(stick(t + 50).blocked); }
    // the composite stops (camera refused): the writer publishes inactive on the next tick -> passes at once
    writer(3100, true);
    CHECK(!stick(3150).blocked);
    // flight again, then the writer dies (process crash / DLL unload): passes once the stamp is older than 500 ms
    for (uint64_t t = 3200; t <= 3600; t += 100) { composites += 9; writer(t, true); }
    CHECK(stick(4000).blocked && !stick(4101).blocked);
    // lookstick:game: never blocked even in flight
    ipc::Block b2{}; ipc::Heartbeat hb2; bool was2 = false;
    for (uint64_t t = 5000; t <= 6000; t += 100) {
        composites += 9;
        const bool on = hb2.tick(composites, false);
        if (on || was2) ipc::publish(&b2, on, t, kPid);
        was2 = on;
        CHECK(!g.apply(25000, 0, &b2, t + 10, true).blocked);
    }
}

// D-358 (Astra finding 3): two game processes in one Windows session. A model of the session namespace (name -> mapping) with the writer's
// create/adopt rule (xr_stereo_proxy.cpp LookStickHeartbeat::open) and the reader's open-by-name (skip_intro.cpp gate_open_mapping).
struct Session {
    std::map<std::wstring, ipc::Block> objects;
    ipc::Block* create(uint32_t pid, uint64_t now, bool& refused) {   // CreateFileMappingW + can_adopt
        wchar_t name[ipc::kMappingNameMax];
        refused = false;
        if (!ipc::mapping_name(name, ipc::kMappingNameMax, pid)) return nullptr;
        const bool existed = objects.count(name) != 0;
        ipc::Block* b = &objects[name];
        if (existed && !ipc::can_adopt(b, now, ipc::kMaxStaleMs, pid)) { refused = true; return nullptr; }
        return b;
    }
    const ipc::Block* open(uint32_t pid) {                            // OpenFileMappingW
        wchar_t name[ipc::kMappingNameMax];
        if (!ipc::mapping_name(name, ipc::kMappingNameMax, pid)) return nullptr;
        const auto it = objects.find(name);
        return it == objects.end() ? nullptr : &it->second;
    }
};
struct Process {                                                      // one game process: acz-vr writer + XInput proxy reader
    uint32_t pid; bool publisher; bool want;                          // publisher: acz-vr loaded; want: lookstick:block (false = lookstick:game)
    ipc::Heartbeat hb; ipc::StickGate gate; ipc::Block* blk = nullptr; bool was = false; uint64_t composites = 0;
    Process(uint32_t p, bool pub, bool w) : pid(p), publisher(pub), want(w), gate(500, p) {}
    void tick(Session& s, uint64_t now, bool flying) {
        if (!publisher) return;
        if (flying) composites += 9;
        const bool on = hb.tick(composites, want);
        bool refused = false;
        if (on && !blk) blk = s.create(pid, now, refused);
        if (blk && (on || was)) ipc::publish(blk, on, now, pid);
        was = on;
    }
    void stop(uint64_t now) { if (blk && hb.active()) ipc::publish(blk, false, now, pid); hb = {}; was = false; }
    bool blocked(Session& s, uint64_t now) { return gate.apply(25000, -8000, s.open(pid), now, true).blocked; }
};

static void test_two_owners() {
    // names: per process, distinct, bounded, the documented prefix
    wchar_t a[ipc::kMappingNameMax], b[ipc::kMappingNameMax];
    CHECK(ipc::mapping_name(a, ipc::kMappingNameMax, 1111) && ipc::mapping_name(b, ipc::kMappingNameMax, 2222));
    CHECK(std::wstring(a) == L"Local\\acz-vr-xr-view-v2-1111" && std::wstring(a) != std::wstring(b));
    CHECK(ipc::mapping_name(a, ipc::kMappingNameMax, 4294967295u) && std::wstring(a) == L"Local\\acz-vr-xr-view-v2-4294967295");
    CHECK(!ipc::mapping_name(a, 10, 1111));

    // A active; B has no publisher: B's stick always passes
    {
        Session s; Process A(1111, true, true), B(2222, false, true);
        for (uint64_t t = 0; t <= 2000; t += 100) { A.tick(s, t, true); B.tick(s, t, true);
            if (t >= 200) CHECK(A.blocked(s, t + 10)); CHECK(!B.blocked(s, t + 10)); }
        CHECK(s.objects.size() == 1);
    }
    // A active; B runs acz-vr with lookstick:game: B never gated
    {
        Session s; Process A(1111, true, true), B(2222, true, false);
        for (uint64_t t = 0; t <= 2000; t += 100) { A.tick(s, t, true); B.tick(s, t, true);
            if (t >= 200) CHECK(A.blocked(s, t + 10)); CHECK(!B.blocked(s, t + 10)); }
    }
    // Both active: one owner's stop / pause never changes the other's signal
    {
        Session s; Process A(1111, true, true), B(2222, true, true);
        for (uint64_t t = 0; t <= 1000; t += 100) { A.tick(s, t, true); B.tick(s, t, true); }
        CHECK(A.blocked(s, 1010) && B.blocked(s, 1010) && s.objects.size() == 2);
        A.stop(1050);                                                 // A's session ends (explicit inactive)
        for (uint64_t t = 1100; t <= 2000; t += 100) { B.tick(s, t, true); CHECK(!A.blocked(s, t + 10)); CHECK(B.blocked(s, t + 10)); }
        A.hb = {}; for (uint64_t t = 2100; t <= 3000; t += 100) { A.tick(s, t, true); B.tick(s, t, false); }   // B's composite stops; A flies again
        CHECK(A.blocked(s, 3010) && !B.blocked(s, 3010));
        B.stop(3050); CHECK(A.blocked(s, 3060));
    }
    // A block that somehow carries another process's id (foreign writer under this name) never gates
    {
        ipc::Block blk{}; ipc::publish(&blk, true, 1000, 1111);
        ipc::StickGate gb(500, 2222);
        const auto r = gb.apply(25000, 0, &blk, 1010, true);
        CHECK(!r.blocked && std::string(r.why) == "foreign-owner" && r.rx == 25000);
        CHECK(!ipc::read_block(&blk, 1010, 500, 0).block);            // an unbound reader (pid 0) never gates
    }
    // Instance generation: the writer adopts an existing mapping only if no live heartbeat and it is unwritten or this process's own
    {
        ipc::Block blk{};
        CHECK(ipc::can_adopt(&blk, 1000, ipc::kMaxStaleMs, 1111));                 // never written
        ipc::publish(&blk, true, 1000, 1111);
        CHECK(!ipc::can_adopt(&blk, 1100, ipc::kMaxStaleMs, 1111));                // a live writer of this process: no second writer
        CHECK(ipc::can_adopt(&blk, 1000 + ipc::kMaxStaleMs + 1, ipc::kMaxStaleMs, 1111));   // that writer went silent (DLL reload)
        ipc::publish(&blk, false, 1200, 1111);
        CHECK(ipc::can_adopt(&blk, 1300, ipc::kMaxStaleMs, 1111));                 // explicitly inactive
        CHECK(!ipc::can_adopt(&blk, 99999, ipc::kMaxStaleMs, 2222));               // another process's block: never
        CHECK(!ipc::can_adopt(&blk, 1300, ipc::kMaxStaleMs, 0));
        // the session model: a second writer instance in A while A's first is live is refused, the reader keeps A's single signal
        Session s; Process A(1111, true, true);
        for (uint64_t t = 0; t <= 500; t += 100) A.tick(s, t, true);
        bool refused = false; CHECK(!s.create(1111, 520, refused) && refused);
        CHECK(A.blocked(s, 530));
    }
}

int main() {    test_option();
    test_block();
    test_gate();
    test_heartbeat();
    test_timeline();
    test_two_owners();
    std::printf("look_stick_gate_offline: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
