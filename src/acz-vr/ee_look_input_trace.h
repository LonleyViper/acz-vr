#pragma once
// Option 3 stage 2, follow-up to D-269 (key ee_camera_look_input_trace, default OFF, needs ee_camera_probe).
//
// MEASURED (D-269): the look-yaw float at base+0xFDD410 is an OUTPUT the game recomputes every frame, so writing it is
// useless; it follows a smoothed first-order filter driven by a digital key input, and neither the filter STATE nor the raw
// input words are located. This module is a strictly READ-ONLY, bounded, high-rate trace of the 64 KiB window
// [base+0xFDD410-0x8000, base+0xFDD410+0x8000) so an OFFLINE tool (tools/analysis/look_input_trace.py) can rank the words
// that change with the look. It never calls Memory::write_float (the glue hands it a read-only Memory) and writes no game state.
//
//  * SDK-free core shared by the Windows glue (ee_camera_probe_win.cpp) and the offline fixtures.
//  * One sample per Present tick (the probe's cadence; nothing finer exists): ONE guarded 64 KiB read, compared with the
//    previous tick; only the changed 4-byte words are kept as (offset, old, new). Words that never change in a window
//    are therefore never logged.
//  * A window opens when the look value starts moving away from neutral (yaw or pitch) or a probe look event begins,
//    runs 3 s, and contains the preceding 1 s (pre-roll, from a ring buffer of delta records). At most 6 windows per
//    process. Hard cap of 4 MiB of trace storage in total: a 3 MiB window arena + a 1 MiB pre-roll ring, both preallocated
//    inside the object (the render callback never allocates). The per-tick working buffers (2 x 64 KiB snapshots, 48 KiB
//    scratch) are not trace data.
//  * The render callback NEVER touches the disk: closed windows are immutable slices of the arena; drain() (called by the
//    glue from the worker thread / exit path) writes them to look-input-trace.bin.
//
// FILE FORMAT (little endian; a file is a sequence of windows, each = 96-byte header + data_bytes of tick records):
//   header : char magic[8]="ACZLIT1\0"; u32 version=1, header_bytes=96, window_index, open_reason(1 look-value,2 look-event);
//            u64 base, region_start; u32 region_bytes, l_offset(byte offset of the look yaw inside the region), tick_count,
//            preroll_ticks, delta_count, data_bytes, flags(bit0 words dropped/budget, bit1 closed early), truncated_words;
//            f64 open_ms, close_ms; u32 close_reason(1 timeout,2 budget,3 candidate-lost,4 identity-changed,5 read-failure,
//            6 process-exit,7 region-outside-block), trigger_tick
//   tick   : u32 tick_index, tid; f64 now_ms; u64 frame; u32 n_stored, n_total; n_stored x {u32 offset(bytes, multiple of 4,
//            relative to region_start), old, new}
// The analysis tool subtracts l_offset to print offsets relative to base+0xFDD410.
#include "ee_look_write.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace acz::ee_camera {

inline constexpr uint32_t trace_l_off=0xFDD410;            // == field_offset[look_yaw]
inline constexpr uint32_t trace_half=0x8000;
inline constexpr uint32_t trace_region_bytes=2*trace_half;  // 64 KiB
inline constexpr uint32_t trace_words=trace_region_bytes/4;
inline constexpr uint32_t trace_l_word=trace_half/4;        // index of the look yaw word inside the region
inline constexpr double   trace_window_ms=3000,trace_preroll_ms=1000;
inline constexpr unsigned trace_max_windows=6;
inline constexpr size_t   trace_arena_bytes=3u<<20,trace_ring_bytes=1u<<20;   // 3 + 1 = 4 MiB of trace data in total
inline constexpr size_t   trace_total_bytes=trace_arena_bytes+trace_ring_bytes;
inline constexpr unsigned trace_max_deltas=4096;            // per tick; more are counted in n_total but not stored
inline constexpr unsigned trace_ring_ticks=512;
inline constexpr unsigned trace_max_window_ticks=4096;
inline constexpr size_t   trace_header_bytes=96,trace_tick_header_bytes=32,trace_delta_bytes=12;
inline constexpr size_t   trace_min_window_bytes=65536;     // a window needs at least this much budget to open (one maximal tick fits)
inline constexpr float    trace_zero_band=look_neutral_band,trace_pitch_zero_band=look_pitch_neutral_band;
inline constexpr float    trace_move_eps=1e-3f;             // == track_look_chg_rad
inline constexpr double   trace_wait_log_ms=10000;

enum TraceOpen : uint32_t {to_value=1,to_event=2};
enum TraceClose : uint32_t {tc_none=0,tc_timeout=1,tc_budget=2,tc_lost=3,tc_identity=4,tc_read=5,tc_exit=6,tc_region=7};
inline const char* trace_open_name(uint32_t r) noexcept {return r==to_value?"look-value":r==to_event?"look-event":"?";}
inline const char* trace_close_name(uint32_t r) noexcept {
    static constexpr const char* n[]={"-","timeout","budget","candidate-lost","identity-changed","read-failure","process-exit","region-outside-block"};
    return r<8?n[r]:"?";
}

// Where closed windows go (the glue: a file; fixtures: a vector). Called off the render path only.
struct TraceOut {virtual bool write(const void* p,size_t n) noexcept=0;virtual ~TraceOut()=default;};

class LookInputTrace {
public:
    struct Options {bool enabled=false;unsigned max_windows=trace_max_windows;double window_ms=trace_window_ms,preroll_ms=trace_preroll_ms;};
    struct Stats {unsigned windows_started=0,windows_closed=0,windows_drained=0;uint64_t ticks=0,read_failures=0,tick_overflows=0;
                  size_t arena_used=0;double cost_avg_us=0,cost_max_us=0;bool exhausted=false;unsigned write_failures=0;};
    // Large object (~4.3 MiB): construct in place on preallocated pages (glue) or on the heap (fixtures). The big arrays are
    // deliberately not initialised, so constructing on zeroed pages commits nothing up front.
    explicit LookInputTrace(Options o=Options{}) noexcept;
    LookInputTrace(const LookInputTrace&)=delete;LookInputTrace& operator=(const LookInputTrace&)=delete;

    // One Present tick (single caller at a time). Read-only on `mem`: only Memory::read is ever called.
    void tick(Memory& mem,Sink& log,const LookIn& in) noexcept;
    // Closes the open window (if any) with `reason`; used by the exit path with the tick lock held. Idempotent.
    void finish(Sink& log,uint32_t reason) noexcept;
    // Glue cost bookkeeping (microseconds spent in one tick()).
    void note_cost_us(double us) noexcept {++costN_;costSum_+=us;if(us>costMax_)costMax_=us;}
    // Off the render path: writes every closed, not yet written window. Safe to call from several threads (try-lock).
    // Returns the number of windows written by THIS call.
    unsigned drain(TraceOut& out) noexcept;

    bool enabled() const noexcept {return o_.enabled;}
    bool is_open() const noexcept {return open_;}
    bool exhausted() const noexcept {return exhausted_;}
    unsigned windows_closed() const noexcept {return closed_.load(std::memory_order_acquire);}
    Stats stats() const noexcept;
    // Closed window slices (valid for i < windows_closed()): immutable once published.
    bool window(unsigned i,const uint8_t*& p,size_t& n) const noexcept {
        if(i>=closed_.load(std::memory_order_acquire))return false;
        p=arena_+segs_[i].off;n=segs_[i].len;return true;
    }

private:
    struct Meta {size_t pos=0;uint32_t bytes=0;double ms=0;};
    struct Seg {size_t off=0,len=0;};
    // ring
    void ring_clear() noexcept {rhead_=0;rused_=0;mhead_=0;mcount_=0;}
    void ring_pop() noexcept {rused_-=mq_[mhead_].bytes;mhead_=(mhead_+1)%trace_ring_ticks;--mcount_;}
    void ring_push(const uint8_t* hdr,const uint8_t* deltas,size_t dbytes,double ms) noexcept;
    void ring_copy(size_t pos,uint8_t* dst,size_t n) const noexcept;
    // window
    bool open_window(Sink& log,uint32_t reason,float yaw,float pitch,double quiet,const LookIn& in,uint32_t tickIdx) noexcept;
    bool arena_put(const uint8_t* hdr,const uint8_t* deltas,size_t dbytes) noexcept;
    void account(const uint8_t* rec) noexcept;
    void close_window(Sink& log,uint32_t reason) noexcept;
    void wait_log(Sink& log,double now,const char* why) noexcept;
    static bool moved_from_zero(float prev,float cur,float band) noexcept;

    Options o_;
    // state
    bool open_=false,haveRegion_=false,havePrev_=false,armed_=true,exhausted_=false,armedLogged_=false;
    uint64_t base_=0,regionStart_=0;uint32_t tickIdx_=0;
    double prevQuiet_=-1,lastWait_=-1e300;const char* lastWhy_="";
    // open window
    size_t segStart_=0,segEnd_=0,w_=0;unsigned winIndex_=0,winTicks_=0,winPre_=0;uint64_t winDeltas_=0,winTrunc_=0;unsigned winChanged_=0;
    uint32_t winReason_=0,winTrigger_=0;double winOpenMs_=0,winLastMs_=0;bool winBudget_=false;
    size_t arenaUsed_=0;unsigned started_=0;
    std::atomic<unsigned> closed_{0};unsigned drained_=0,writeFail_=0;std::atomic_flag drainBusy_=ATOMIC_FLAG_INIT;
    Seg segs_[trace_max_windows]{};
    uint64_t ticks_=0,readFail_=0,overflows_=0,costN_=0;double costSum_=0,costMax_=0;
    // ring bookkeeping
    size_t rhead_=0,rused_=0;unsigned mhead_=0,mcount_=0;Meta mq_[trace_ring_ticks]{};
    // big storage (uninitialised on purpose)
    struct Delta {uint32_t off,oldv,newv;};
    uint32_t bufA_[trace_words];uint32_t bufB_[trace_words];uint32_t* prev_;uint32_t* cur_;
    uint32_t bitmap_[trace_words/32];
    Delta scratch_[trace_max_deltas];
    uint8_t ring_[trace_ring_bytes];
    uint8_t arena_[trace_arena_bytes];
};

} // namespace acz::ee_camera
