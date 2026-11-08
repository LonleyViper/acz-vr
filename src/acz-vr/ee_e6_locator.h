#pragma once
// ee_e6_locator.h -- E6-ANCHOR-FREE: find the game's E6 camera/aircraft structure from its CONTENT (docs/world-render/e6-anchor-free.md).
//
// Problem (D-300/D-301, overnight mandate D-302): the terrain/world renderer and the model cockpit read the aircraft and camera from the E6 fields
// of a 64 MiB private read-write block of the game process (block offsets +0xFDD40C..+0xFDD87C). The existing feed (ee_camera_probe_win.cpp
// terrain_tick) reads them at the probe's OBSERVATION base: the validated base, else the best relative-scan candidate (needs the cockpit basis
// and a wide pass over every region, ~4-9 s after the cockpit appears), else the first allocation-exact 64 MiB block - which in every archived
// run (step47..step67, headset tests 15/16) is a DIFFERENT, all-zero block. Until the scan lands, the terrain was fed a zero/junk camera.
//
// This module validates the E6 structure itself at the fixed offsets of every candidate block (a committed private read-write region of exactly
// 64 MiB, or an allocation of exactly 64 MiB made of such regions): plausible stage position, finite angles in range, camera = aircraft + a
// rigid eye offset (15..60 units, pointing along the aircraft's forward within 25 deg), consecutive samples consistent (no teleport, stable eye
// distance) and MOVING. It locks onto the one candidate that passes N consecutive samples, re-validates every sample after that, keeps the last
// good sample for at most 250 ms (hold), then the glue invalidates the mailbox (no camera); after 1 s without a valid sample the lock itself is lost
// and the search restarts (1 s is the lock-loss/re-search threshold, NOT the no-camera threshold). No look events, no cockpit basis, no relative
// scan. READ-ONLY: the ReadMem interface
// has no write operation; nothing here can write game memory or patch code.
//
// Cost caps: one locked sample = 92 bytes per tick (two windows); while searching at most tick_byte_cap bytes per tick (5 candidates), each
// candidate at most once per sample_ms; at most kCandidateCap candidates; the block enumeration itself is a VirtualQuery walk done by the glue
// on the probe's worker thread (no memory contents are read by it).
//
// SDK-free (no Windows types): shared by the DLL glue and tests/acz-vr/e6_locator_offline_tests.cpp.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace acz::e6loc {

// ------------------------------------------------------------------------------------------------ option (inside the xr_terrain_stage value)
// e6loc:auto    (default) the anchor-free locator; while it has no lock, the probe's observation base is used ONLY for samples that pass the
//               same single-sample checks (the all-zero block no longer feeds a camera).
// e6loc:probe   the previous feed exactly (probe observation base, no checks).
// e6loc:content the anchor-free locator only (no probe fallback).
enum class Mode : uint8_t { automatic = 0, probe = 1, content = 2 };
const char* mode_name(Mode m) noexcept;
// Removes e6loc:<v> (':' or '=') from a "<id>;opt:val;..." value; everything else is returned unchanged (a value without e6loc is returned
// byte-identical). false + why for a bad e6loc value (the whole terrain is then refused like any bad option).
bool extract_mode(const std::string& value, std::string& rest, Mode& mode, std::string& why);
// Non-allocating lookup for the probe glue: the mode named in `value` (default automatic); ok=false for a bad value.
Mode mode_in(const char* value, bool& ok) noexcept;

// ------------------------------------------------------------------------------------------------ E6 layout (block-relative, D-237..D-241)
inline constexpr uint64_t kBlockBytes = 0x4000000ull;     // 64 MiB
inline constexpr uint32_t kWinA = 0xFDD40C;                // look pitch/yaw/roll @+0/+4/+8, air x/y/z @+0x20/24/28, heading @+0x3C
inline constexpr uint32_t kWinABytes = 0x40;
inline constexpr uint32_t kWinB = 0xFDD860;                // cam x/y/z @+0/4/8, (+0xC unknown), rot pitch/yaw/roll @+0x10/14/18
inline constexpr uint32_t kWinBBytes = 0x1C;
inline constexpr uint32_t kSampleBytes = kWinABytes + kWinBBytes;   // 92
inline constexpr uint32_t kCandidateCap = 16;

struct Sample { float look[3]{}, air[3]{}, heading = 0, cam[3]{}, rot[3]{}; };
void decode(const uint8_t a[kWinABytes], const uint8_t b[kWinBBytes], Sample& s) noexcept;

// Plausibility limits (evidence: docs/world-render/e6-anchor-free.md section 2).
inline constexpr float kWorldHalf = 262144.0f;             // stage X/Z extent (D-292)
inline constexpr float kWorldMargin = 32768.0f;            // flying a little outside the map
inline constexpr float kAltMin = -2000.0f, kAltMax = 200000.0f;
inline constexpr double kAngleMax = 3.14159265358979 + 0.01;          // look and rot (pitch/yaw/roll): [-pi, pi]
inline constexpr double kHeadingMax = 2.0 * 3.14159265358979 + 0.01;  // heading: never seen outside +-0.94; wrap unknown -> 2 pi (inference)
inline constexpr float kEyeMin = 15.0f, kEyeMax = 60.0f;   // |cam - air|: the 36 player aircraft eye points are 19.4..53.2 units
inline constexpr double kEyeAngleMaxDeg = 25.0;            // eye offset vs aircraft forward: 3.0..15.1 deg by type, measured <= 12.9 live
inline constexpr double kYawRelMax = 0.6;                  // |wrap(rot yaw - heading - look yaw)|: measured <= 0.372 (look ramps)
inline constexpr double kMaxSpeed = 20000.0;               // units/s between consecutive samples (+ kJumpSlack); Mach 2.5 ~ 4300
inline constexpr double kJumpSlack = 200.0;
inline constexpr float kEyeDistJitter = 1.5f;              // | |cam-air| - previous | (rigid eye offset; measured range 0.87 over a run)

// nullptr = the single sample is plausible, else the first failed check (stable names, logged).
const char* check_sample(const Sample& s) noexcept;
// Consecutive raw samples dt_ms apart: nullptr = consistent, else "jump" / "eye-distance-unstable".
const char* check_pair(const Sample& prev, const Sample& cur, double dt_ms) noexcept;
bool moved(const Sample& prev, const Sample& cur) noexcept;   // any aircraft position word changed (bit-exact)
float eye_distance(const Sample& s) noexcept;
double eye_angle_deg(const Sample& s) noexcept;               // angle between cam-air and the aircraft forward (heading, rot pitch - look pitch)

// ------------------------------------------------------------------------------------------------ memory + log (read-only by construction)
struct ReadMem {
    virtual bool read(uint64_t address, void* out, uint32_t bytes) noexcept = 0;   // false = unreadable
    virtual ~ReadMem() = default;
};
// Reads the two E6 windows of `base`; false when either is unreadable.
bool read_sample(ReadMem& m, uint64_t base, Sample& s) noexcept;
struct Sink {
    virtual void line(bool important, const char* text) noexcept = 0;
    virtual ~Sink() = default;
};

// ------------------------------------------------------------------------------------------------ candidate blocks
struct RegionDesc { uint64_t base = 0, size = 0, alloc = 0; bool committed = false, is_private = false, read_write = false; };
// Fed with the process's regions in ascending order (a VirtualQuery walk). Candidates: 'r' = one committed private RW region of exactly 64 MiB
// (the E6 block in every archived run: a 64.00 MiB protect-4 region inside a larger allocation), 'a' = an allocation of exactly 64 MiB made of
// committed private RW regions (the probe's CandidateCollector rule; all-zero at the E6 offsets in every archived run). Capped, deduplicated.
class CandidateBuilder {
public:
    void reset() noexcept { *this = CandidateBuilder{}; }
    void add(const RegionDesc& r) noexcept;
    void finish() noexcept;
    // E6-RELOCATED (2026-10-30): a structure base found by StructScanner inside a candidate block ('s'); deduplicated, counted in dropped() past the cap.
    void add_struct(uint64_t base) noexcept { push(base, 's'); }
    unsigned count() const noexcept { return n_; }
    uint64_t base(unsigned i) const noexcept { return i < n_ ? base_[i] : 0; }
    char kind(unsigned i) const noexcept { return i < n_ ? kind_[i] : '?'; }
    unsigned regions_seen() const noexcept { return seen_; }
    unsigned dropped() const noexcept { return dropped_; }
private:
    void push(uint64_t b, char k) noexcept;
    void close() noexcept;
    bool open_ = false, ok_ = false; uint64_t alloc_ = 0, start_ = 0, span_ = 0; unsigned nreg_ = 0;
    uint64_t base_[kCandidateCap]{}; char kind_[kCandidateCap]{}; unsigned n_ = 0, seen_ = 0, dropped_ = 0;
};

// ------------------------------------------------------------------------------------------------ E6-RELOCATED: content scan of the candidate blocks
// Evidence (docs/world-render/e6-anchor-free.md section 11): the E6 structure is NOT always at block +0xFDD40C. On m03k (The Round Table, airborne
// start) it sat at region +0xCA811C (J35) / +0xCAAC6C (F-5E) in every archived run (headset 2026-10-30 x2, step124 x30, L1 runs), the fixed offset
// held junk, and the locator never locked. Same layout (window B = window A + 0x454), only the position moves. The scanner reads every candidate
// block in bounded chunks on the probe's worker thread and reports each 4-aligned position whose 92 bytes pass check_sample; the Locator then
// validates those bases exactly like the fixed-offset ones (8 consistent samples, 3 moves, unique), so static plausible junk never locks.
inline constexpr uint32_t kWinGap = kWinB - kWinA;                   // 0x454
inline constexpr uint32_t kStructBytes = kWinGap + kWinBBytes;       // 0x470: window A start .. window B end
// Scans `len` bytes copied from process address `addr`: every 4-aligned offset o with o + kStructBytes <= len whose decoded sample passes
// check_sample yields base = addr + o - kWinA (written while n < cap; the return value counts all hits). Positions are scanned in address order; a full list keeps the best-scored hits (scan_struct_scored).
unsigned scan_struct(const uint8_t* buf, size_t len, uint64_t addr, uint64_t* bases, unsigned cap) noexcept;
// E6-JUNK-LOCK: with a full list a hit replaces the lowest-scored entry when its hit_score is higher (first of equals), so the cap keeps the BEST hits, not
// the first in address order. hit_score (0..4) = heading non-zero + rot yaw non-zero + rot pitch within +-1.6 + look near rest: junk structures are zero / denormal / 2.0-filled
// there, a flying aircraft scores 4. `scores` parallel to `bases` (cap <= kMaxScoredCap).
inline constexpr unsigned kMaxScoredCap = 64;
int hit_score(const Sample& s) noexcept;
bool insert_best(uint64_t* bases, uint8_t* scores, unsigned n, uint64_t b, uint8_t sc) noexcept;
unsigned scan_struct_scored(const uint8_t* buf, size_t len, uint64_t addr, uint64_t* bases, uint8_t* scores, unsigned cap) noexcept;
class StructScanner {
public:
    static constexpr uint32_t kChunk = 1u << 20;                     // bytes scanned per read (+ kStructBytes - 4 overlap)
    static constexpr unsigned kHitCap = 12;
    // Starts a pass over every candidate block of `b` (each scanned as [base, base + 64 MiB)); a running pass is restarted.
    void start(const CandidateBuilder& b) noexcept;
    bool active() const noexcept { return active_; }
    void cancel() noexcept { active_ = false; }               // a running pass is dropped (its partial hits are discarded)
    // Scans at most `budget` bytes (whole chunks, at least one); true when this call finished the pass (hits of the pass are then current).
    bool step(ReadMem& m, uint64_t budget) noexcept;
    // Hits of the last FINISHED pass (a running pass does not replace them until it ends).
    unsigned hits() const noexcept { return nh_; }
    uint64_t hit(unsigned i) const noexcept { return i < nh_ ? hits_[i] : 0; }
    uint64_t hit_block(unsigned i) const noexcept { return i < nh_ ? hitBlock_[i] : 0; }
    unsigned hits_dropped() const noexcept { return dropped_; }
    void append_hits(CandidateBuilder& b) const noexcept { for (unsigned i = 0; i < nh_; ++i) b.add_struct(hits_[i]); }
    uint64_t passes() const noexcept { return passes_; }
    uint64_t pass_bytes() const noexcept { return passBytes_; }
    unsigned pass_unreadable_chunks() const noexcept { return passUnread_; }
    unsigned blocks() const noexcept { return nb_; }
private:
    void add_hits(const uint64_t* b, const uint8_t* sc, unsigned n, unsigned total, uint64_t block) noexcept;
    bool active_ = false;
    uint64_t blocks_[kCandidateCap]{}; unsigned nb_ = 0, bi_ = 0; uint64_t off_ = 0;
    uint64_t cur_[kHitCap]{}, curBlock_[kHitCap]{}; uint8_t curScore_[kHitCap]{}; unsigned nc_ = 0, curSeen_ = 0, unread_ = 0; uint64_t bytes_ = 0;
    uint64_t hits_[kHitCap]{}, hitBlock_[kHitCap]{}; unsigned nh_ = 0, dropped_ = 0, passUnread_ = 0; uint64_t passes_ = 0, passBytes_ = 0;
    std::unique_ptr<uint8_t[]> buf_;                                  // kChunk + kStructBytes, allocated by the first start() (nothrow)
};

// ------------------------------------------------------------------------------------------------ E6-RELOCATED: the e6loc:auto probe-base fallback gate
// The probe's observation base is published (no lock) only after the same rules a lock needs: lock_samples consecutive samples of ONE base that
// pass check_sample and check_pair, lock_moves of them moving the aircraft. Evidence: headset 2026-10-30 the probe followed a static junk structure
// (air 2,0,45.3 / cam 2,2,2: passes check_sample) and the single-sample fallback published it as the camera for the rest of the flight
// (camera-admit e6-only-game-refused, object-registration refused, boxes refused-view). Once qualified, identical samples (pause) stay published.
class FallbackGate {
public:
    explicit FallbackGate(unsigned samples = 8, unsigned moves = 3) noexcept : need_(samples), moves_need_(moves) {}
    // nullptr = publish this sample; else why not ("qualifying", "static" or a check_sample / check_pair reason).
    const char* admit(uint64_t base, const Sample& s, double now_ms) noexcept;
    void reset() noexcept { base_ = 0; streak_ = moves_ = 0; have_ = qualified_ = false; }
    uint64_t qualified_base() const noexcept { return qualified_ ? base_ : 0; }
private:
    unsigned need_, moves_need_;
    uint64_t base_ = 0; unsigned streak_ = 0, moves_ = 0; bool have_ = false, qualified_ = false; Sample prev_{}; double lastT_ = 0;
};

// ------------------------------------------------------------------------------------------------ the locator (Present-tick owned, single thread)
struct Options {
    unsigned lock_samples = 8;        // consecutive valid + consistent samples ...
    unsigned lock_moves = 3;          // ... of which this many moved the aircraft (static decoys never lock)
    double sample_ms = 100;           // per-candidate sampling interval while searching
    double hold_ms = 250;             // locked: an invalid stretch shorter than this keeps the last published sample (no invalidate)
    double lose_ms = 1000;            // locked: no valid sample for this long -> lost (no camera), search again
    double enum_ms = 2000;            // searching: ask the glue for a fresh candidate list this often
    unsigned tick_byte_cap = 512;     // searching: E6 bytes read per tick (5 candidates)
    double search_log_ms = 5000, summary_ms = 10000;
};
struct TickOut {
    bool valid = false;               // `s` is a validated sample of the locked block (publish it)
    bool hold = false;                // locked, this sample invalid, within hold_ms: publish nothing, keep the last one
    Sample s{};
    uint64_t base = 0;
    uint32_t bytes = 0;               // E6 bytes read this tick
};
class Locator {
public:
    enum State : uint8_t { searching, locked };
    explicit Locator(Options o = Options{}) noexcept : o_(o) {}
    // Glue: a candidate list wanted now (searching and none received for enum_ms).
    bool wants_candidates(double now_ms) const noexcept { return state_ == searching && (!haveCands_ || now_ms - candAt_ >= o_.enum_ms); }
    void set_candidates(const CandidateBuilder& b, double now_ms, Sink& log) noexcept;
    void tick(ReadMem& m, double now_ms, Sink& log, TickOut& out) noexcept;
    State state() const noexcept { return state_; }
    uint64_t locked_base() const noexcept { return state_ == locked ? lockedBase_ : 0; }
    // Counters (summary line + tests).
    uint64_t locks() const noexcept { return locks_; }
    uint64_t alias_refused() const noexcept { return aliasRefused_; }   // E6-ALIAS: candidate samples refused as a shifted view of the home block
    uint64_t losses() const noexcept { return losses_; }
    // CAM-EPOCH: increments on every lock, loss and relock (locks + losses); a sample carries the epoch it was published under.
    uint32_t lock_epoch() const noexcept { return uint32_t(locks_ + losses_); }
    // CAM-EPOCH: locked samples rejected as a position jump (a teleport: mission restart/change, or a block reused for another stage). The glue
    // bumps the stage epoch on each one, so samples from before the discontinuity are refused by the consumer.
    uint64_t discontinuities() const noexcept { return discont_; }
    uint64_t valid_samples() const noexcept { return valid_; }
    uint64_t rejected_samples() const noexcept { return rejected_; }
    uint32_t max_tick_bytes() const noexcept { return maxTickBytes_; }
    unsigned candidates() const noexcept { return nc_; }
    const char* last_reason() const noexcept { return lastReason_; }
    // Glue-maintained counters reported in the summary line (fallback = probe base samples published in e6loc:auto without a lock).
    uint64_t fallback_published = 0, fallback_rejected = 0, no_camera_ticks = 0;
    void maybe_summary(double now_ms, Sink& log, const char* mode) noexcept;
private:
    struct Cand { uint64_t base = 0; char kind = '?'; unsigned streak = 0, moves = 0; double lastT = -1e300; Sample prev{}; bool havePrev = false;
                  const char* why = "unchecked"; uint32_t rejects = 0; };
    const char* sample_one(ReadMem& m, uint64_t base, double now, Sample& s, Sample& prev, bool& havePrev, double& lastT, bool& mv) noexcept;
    void count_reason(const char* r) noexcept;
    Options o_;
    State state_ = searching;
    Cand c_[kCandidateCap]{}; unsigned nc_ = 0, rr_ = 0; bool haveCands_ = false; double candAt_ = -1e300;
    uint64_t lockedBase_ = 0; char lockedKind_ = '?'; Sample lprev_{}; bool lhavePrev_ = false; double llastT_ = -1e300, lastValid_ = -1e300;
    // E6-ALIAS (POV1 null run 2026-10-31): the last locked base. A candidate overlapping it (|base - home| < kStructBytes, base != home) is the same
    // memory read at a shifted offset; in the chase view the -4 byte alias passed every check (cam-air = (1, dx, dy), 58.5 units) and locked, which
    // turned chase into "HUD-only". Refused while the home structure still reads as a live camera (valid, or only refused for its geometry).
    uint64_t homeBase_ = 0; double homeCheckT_ = -1e300; bool homeAlive_ = false, homeHavePrev_ = false; Sample homePrev_{}; uint64_t aliasRefused_ = 0;
    bool home_alive(ReadMem& m, double now, uint32_t& bytes, bool may_read) noexcept;
    double lastSearchLog_ = -1e300, lastSummary_ = -1e300, lastAmbig_ = -1e300;
    uint64_t locks_ = 0, losses_ = 0, valid_ = 0, rejected_ = 0, discont_ = 0; uint32_t maxTickBytes_ = 0; const char* lastReason_ = "none";
    static constexpr unsigned kReasons = 12;
    const char* rname_[kReasons]{}; uint64_t rcount_[kReasons]{};
};

} // namespace acz::e6loc
