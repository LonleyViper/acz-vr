#pragma once
// J-w1-ee-camera rev 3 (D-245): live run 2 showed the only 64 MiB private block
// is all zeros at the E6 offsets while the GPU-side camera (cockpit SrtData)
// followed every look sweep. Two READ-ONLY locator routes that do not assume a
// 64 MiB block:
//  * region census + a WIDE relative-geometry pass over every committed
//    readable region >= 64 KiB (private, mapped, image), byte-budgeted and
//    resumable, per-region relation counts;
//  * a DIFFERENTIAL scan triggered by the recovered camera basis: a window is
//    read, the worker waits until the SrtData forward vector turned > 5 deg
//    (or times out = a steady control pair), reads the window again and ranks
//    the 4-byte words whose change matches the recovered yaw/pitch change in
//    radians, degrees or fixed point (4096 / 65536 per turn). No E6 offsets.
// Worker thread only; never writes. SDK-free (fixtures drive it with fakes).
#include "ee_camera_probe.h"

namespace acz::ee_camera {

enum RegionType : uint8_t {rt_unknown=0,rt_private=1,rt_mapped=2,rt_image=3};
inline const char* region_type_name(uint8_t t) noexcept {static constexpr const char* n[]={"?","private","mapped","image"};return t<4?n[t]:"?";}
struct RegionInfo {uint64_t base=0,size=0,alloc=0;uint8_t type=rt_unknown;uint32_t protect=0;bool writable=false;};
struct RegionCensus {unsigned seen=0,kept=0,tiny=0,own=0,gpu=0,unreadable=0,overflow=0;uint64_t bytes=0;};
inline constexpr unsigned region_cap=4096;
inline constexpr uint64_t region_min=64*1024;
struct RegionSource {virtual unsigned enumerate(RegionInfo* out,unsigned cap,RegionCensus& census) noexcept=0;virtual ~RegionSource()=default;};
// Census table (by type and protection), plus the largest regions when `full`.
void log_region_table(const RegionInfo* r,unsigned n,const RegionCensus& c,Sink& log,bool full) noexcept;
const RegionInfo* find_region(const RegionInfo* r,unsigned n,uint64_t addr) noexcept; // r sorted by base

// Wide pass: relative-geometry relations over regions [cur.region..] until
// `budget` bytes are scanned (a first region larger than the budget is still
// scanned whole). out.complete when the end of the list was reached.
struct WideCursor {unsigned region=0;uint64_t passes=0;};
void wide_pass(Memory& mem,const RegionInfo* r,unsigned n,const Basis* basis,float* scratch,uint64_t budget,WideCursor& cur,
               ScanResult& out,Sink& log,void (*yield)()=nullptr) noexcept; // caller resets `out`

// Differential scan.
enum Encoding : uint8_t {enc_rad=0,enc_deg,enc_fx4096,enc_fx65536,enc_count};
inline const char* encoding_name(uint8_t e) noexcept {static constexpr const char* n[]={"rad","deg","fx4096/turn","fx65536/turn"};return e<enc_count?n[e]:"?";}
inline constexpr double diff_trigger_rad=5*pi/180;
// Change of a 4-byte word in radians under an encoding; false when the word
// cannot be an angle in that encoding.
bool encoded_delta(uint8_t enc,uint32_t a,uint32_t b,double& d) noexcept;
struct DiffCandidate {uint64_t addr=0;uint8_t enc=0,axis=0;int8_t sign=1;uint32_t hits=0,misses=0,born=0;double ratio=0;uint32_t last=0;
                      double vmin=0,vmax=0; // decoded values seen (encoding units)
                      int score() const noexcept {return int(hits)-2*int(misses);}};
class DiffScanner {
public:
    static constexpr unsigned cap=4096,new_per_window=2048;
    // A/B: the window [addr, addr+4*words) read before/after; dyaw/dpitch: the
    // recovered camera change (rad) between the two reads.
    void analyse(uint64_t addr,const uint32_t* A,const uint32_t* B,size_t words,double dyaw,double dpitch) noexcept;
    unsigned top(DiffCandidate* out,unsigned k,uint32_t minHits=2) const noexcept; // ranked by score
    unsigned size() const noexcept {return n_;}
    uint64_t windows=0,moving=0,steady=0,changed=0,created=0,evicted=0,dropped=0;double maxTurn=0;
private:
    int find(uint64_t addr,uint8_t enc,uint8_t axis,int8_t sign) const noexcept;
    bool insert(const DiffCandidate& c) noexcept;
    DiffCandidate t_[cap]{};unsigned n_=0;uint32_t window_=0;
};
struct BasisSource {virtual bool latest(Basis& out) noexcept=0;virtual ~BasisSource()=default;};
struct Waiter {virtual void wait_ms(unsigned ms) noexcept=0;virtual ~Waiter()=default;};
// One window: read A; wait up to 600 ms for a > 5 deg turn of the recovered
// camera, then keep waiting while it still moves (<= 1200 ms total) so the
// change is large relative to a frame of lag; read B; analyse. Returns false
// when no basis is available or a read failed.
bool diff_window(Memory& mem,BasisSource& bs,Waiter& w,uint64_t addr,uint32_t bytes,uint32_t* A,uint32_t* B,DiffScanner& d) noexcept;
// Ranked table (rev 4: printed even when every candidate has a single hit):
// diff-summary + up to 10 diff-top lines; `tag` = periodic | final.
void log_diff(const DiffScanner& d,const RegionInfo* r,unsigned n,Sink& log,const char* tag="periodic") noexcept;
// Decoded value of a word under an encoding (float for rad/deg, integer otherwise).
double decoded_value(uint8_t enc,uint32_t w) noexcept;
// Window order: every other window walks the WRITABLE regions sequentially
// (the camera fields are written by the game); the others revisit a window
// holding one of the current top-10 candidates so evidence accumulates
// without waiting for a full pass.
// rev 4 order: tier 0 = 64 MiB and 32 MiB regions (the E6 / PS2 RAM targets),
// tier 1 = writable regions within 256 MiB of a tier-0 region, tier 2 = the rest.
struct DiffDriver {
    static constexpr uint64_t near_bytes=256ull<<20;
    unsigned region=0;uint64_t offset=0,step=0,passes=0,rng=0x9E3779B97F4A7C15ull;unsigned hot=0;
    uint16_t order[region_cap]{};unsigned norder=0,orderN=0;uint64_t orderFirst=0;
    void build_order(const RegionInfo* r,unsigned n) noexcept;
    bool next(const RegionInfo* r,unsigned n,const DiffScanner& d,uint32_t window,uint64_t& addr,uint32_t& bytes) noexcept;
};

} // namespace acz::ee_camera
