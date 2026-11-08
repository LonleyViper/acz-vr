#pragma once
// stage_fingerprint.h -- runtime stage detection for xr_terrain_stage=auto (docs/world-render/stages-and-detection.md section 3).
// SDK-free (no Windows/D3D12 types): the fingerprint table loader (fingerprints.json from research/world-render-data/stage_fingerprints.py)
// and the byte-stream Matcher. The Windows memory scan that feeds the Matcher is stage_detect_win.cpp; tests/acz-vr/stage_fingerprint_offline_tests.cpp
// drives the Matcher with fake streams.
//
// A signature is a 32-byte window cut from a stage's RAW game files (.mhd/.mti/.mtc/.mtd/mapset/sph) and lists the identity groups whose
// files contain it (a group = stages whose baked data are byte-identical, e.g. m03k/m03m/m03n). decide(): every distinct signature seen
// counts once for each owning group; the best group wins when it has >= min_hits AND strictly more hits than every other group.
// Nothing here reads or writes game memory; the Matcher only looks at the bytes it is given.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acz::stagefp {

inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kWindow = 32;            // signature length (bytes); the table must say the same
inline constexpr size_t kMaxJsonBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxSigs = 8192, kMaxGroups = 256;
// The signature bytes are kept XOR-masked in memory so a scan of this process can never find the table itself.
inline constexpr uint8_t kMask = 0xA5;

struct Group {
    std::string stage;                  // representative stage id (loaded as <stage>.zvrs / .zvro)
    std::vector<std::string> members;   // every stage id with identical baked data
    std::string zvrs_sha256;            // payload hash of the representative's .zvrs at table build time (informational, may be empty)
};
struct Sig {
    uint8_t masked[kWindow];            // window bytes XOR kMask
    uint64_t key_masked;                // first 8 bytes (little endian) XOR the mask, for the hash table
    std::vector<uint16_t> groups;       // owning group indices
    char src[8];                        // "mhd", "mtd", ... (diagnostics)
};
struct Table {
    uint32_t window = kWindow, min_hits = 3;
    std::vector<Group> groups;
    std::vector<Sig> sigs;
};

// Parse fingerprints.json. All-or-nothing: false + why on any schema/range problem (unknown format/version, window != 32, bad hex,
// group index out of range, duplicate signature, invalid stage id, too many entries).
bool parse_table(const char* json, size_t n, Table& out, std::string& why);
bool load_table_file(const std::string& path, Table& out, std::string& why);
// %LOCALAPPDATA%\acz-vr\stage\fingerprints.json (empty when LOCALAPPDATA is unset); dir_override replaces the folder.
std::string table_path(const std::string& dir_override = std::string());

struct Result {
    enum class Kind : uint8_t { match, no_match, ambiguous };
    Kind kind = Kind::no_match;
    std::string stage;                  // the winning group's representative (match only)
    int group = -1;
    uint32_t best = 0, second = 0;      // hits of the best group / the best other group
    uint32_t sigs_seen = 0;             // distinct signatures found
    std::vector<std::pair<int, uint32_t>> ranking;   // (group, hits), best first, groups with hits only
    std::string reason;                 // "match", "no-signature", "below-min-hits", "tie"
};
const char* kind_name(Result::Kind) noexcept;

class Matcher {
public:
    explicit Matcher(const Table& t);
    // A stream is one contiguous memory range; feed() may be called repeatedly for consecutive chunks of it (windows across chunk
    // boundaries are found: the last window-1 bytes are carried). end_stream() drops the carry (separate ranges never join).
    void begin_stream() noexcept { carry_n_ = 0; }
    void feed(const uint8_t* p, size_t n) noexcept;
    void end_stream() noexcept { carry_n_ = 0; }
    void reset() noexcept;
    Result decide() const;
    uint64_t bytes_fed() const noexcept { return bytes_; }
    // per-source hit counts (diagnostics): "mhd=3,mtc=1,..."
    std::string source_summary() const;
    const std::vector<uint8_t>& seen() const noexcept { return seen_; }
    // Distinct signatures seen so far (cheap progress counter) and the current leader without allocating (best/second hits, best
    // group or -1); decide() gives the same best/second. stage_scan.cpp uses both for the decisive early stop.
    uint32_t seen_count() const noexcept { return seen_n_; }
    void lead(uint32_t& best, uint32_t& second, int& group) const noexcept;
    const Table& table() const noexcept { return t_; }
private:
    void scan(const uint8_t* p, size_t n) noexcept;
    const Table& t_;
    std::vector<int32_t> slots_;        // open addressing: sig index or -1
    uint64_t mask_ = 0;
    unsigned shift_ = 0;
    std::vector<uint8_t> seen_;         // per signature
    uint8_t carry_[kWindow * 2] = {};
    size_t carry_n_ = 0;
    uint64_t bytes_ = 0;
    uint32_t seen_n_ = 0;
};

} // namespace acz::stagefp
