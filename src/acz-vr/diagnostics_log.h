#pragma once
#include "runtime_config.h"
#include "cpu_upload_snapshot.h"
#include "shader_metadata.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <memory>

namespace acz::requirements {struct MetadataResult;}
namespace acz::capture {struct Command;}

namespace acz::diagnostics {
// Hard limits, never configurable upwards. max_records is the process-log
// lifetime write budget (including summaries), not a per-flush resettable cap.
// Ordinary current + two rotations: 3 * 512 KiB. Production also has ONE
// nonrotating, reset-on-activation evidence file with disjoint lifetime quotas:
// 4096 PSO/shader records + 512 root + 256 startup + 32 checkpoint records.
// Each emitted PSO stage/outcome and root reason is retained verbatim.
// Reasons use 64 fixed-width, in-place snapshots plus one overflow snapshot.
// Metadata outcomes add 32 distinct first-example slots + one overflow slot.
// The generic checker + hook supply 80 static literals (78 rejected,
// two metadata-proved), plus the logger's single overflow literal. The generic
// checker's legacy incomplete-state literal is unreachable through the native
// hook's precise precheck; native draws can emit at most 78 of these literals.
// This exceeds 32 example slots deliberately: excess distinct outcomes use the
// existing counted overflow slot. Its quota is unchanged. Different
// cb10 kinds may consume separate slots; overflow counts every excess attempt.
// Dedup key is static outcome text and cb10 kind; fields retain the FIRST draw
// for that key, b is its cumulative count and a its last sequence. Proved text
// distinguishes indexed (index-range-unverified-without-bytes) from nonindexed.
// First-example text also retains lp=0/1/2/3 (base/closed/Reset/unobserved);
// the dedup key stays outcome + cb10 kind, and the record schema is unchanged.
// Explicit-lifetime refusals may append pd/rd/bd creation-condition masks
// (known/value). These belong to the first example, never to the dedup key.
// Three list-creation paths add three deduplicated counter snapshots. Each uses
// sequence=first, a=last, b=repeats, c=path (0 base, 1 closed, 2 Reset).
// Maximum disk bytes = 3*524288 + (4096+512+256+32+65+33+3+81)*576
// plus twelve 576-byte L3/CPU/candidate slots, 24 step-1/step-2 experiment
// slots and four <=2368-byte raw snapshots = 4528000 < 4.5 MiB.
// Ordinary traffic cannot consume these slots.
// Summaries have a separate 60 s minimum interval (300 s unchanged refresh).
// At most 121 batches in 2 h, each <= 1+2*Event::count+1 = 30 records:
// <=3630 ordinary records, leaving >56790 slots for other ordinary traffic
// (J-3b6: was 56800 before the experiment partition grew by 9 slots).
// This holds even for 28800 due 250 ms flushes; identities remain reserved.
inline constexpr size_t max_record_length=576, max_records=65536;
inline constexpr size_t max_file_size=512*1024, max_rotated_files=2, ring_capacity=64;
inline constexpr size_t pso_records=4096,root_records=512,startup_records=256,checkpoint_records=32;
inline constexpr size_t reason_capacity=64,reason_records=reason_capacity+1;
inline constexpr size_t outcome_capacity=32,outcome_records=outcome_capacity+1;
inline constexpr size_t list_path_records=3; // CreateCommandList, CreateCommandList1, registered-at-reset
// GPU-copy step-1/step-2 experiment evidence ("gpu-copy-step1;" or, J-3b6,
// "gpu-copy-step2;" requirements_metadata records). Produced only when
// gpu_copy_step1_experiment or gpu_copy_step2_experiment is latched (never
// both: that configuration refuses); its own reserved, nonrotating evidence
// partition, so hook/startup pressure cannot drop the experiment's pins,
// progression or final verdict. Budget, consciously raised 15 -> 24 by J-3b6:
// step 1's longest path emits 14 records; step 2's longest path emits 22
// (step-1's 14 + 3 role + 3 role-data + 1 index + 1 STEP2_RESULT). Two spare.
// Periodic waits use another prefix. +9 slots = +5184 disk bytes.
// J-3b7 consciously raises 24 -> 40 for step 3 ("gpu-copy-step3;"): its longest
// path emits 35 records (pins, prepare, timeline, observe, game-rt, 6 srt, crop,
// targets, insert, replay, close, execute, signal, poll, reset-closure, map,
// 4 pixel+png, 4 f16 (FP16 targets), stereo, disposition, creation, counters,
// STEP3_RESULT, EXPERIMENT_RESULT). Five spare. +16 slots = +9216 disk bytes.
inline constexpr size_t step1_records=56; // J-3b7c: 40 -> 56, longest step-3 path 45 records (+vertices, 4 clip, 4 png)
// Certification diagnostics: 27 static reasons, three calling seams. No new
// Event kind: these are reserved device records, c=seam, sequence=first,
// a=last, b=count. Keys are reason + seam; type is the first observed type.
enum class ListSeam : unsigned { creation, reset, execute };
enum class ListRefusal : unsigned {
    null_list, pin, canonical_qi, non_direct_bundle, non_direct_other,
    variant_unexpected, variant_qi, variant_canonical, ancestor_unexpected,
    ancestor_qi, ancestor_canonical, base_variant_missing, stable_qi, roundtrip_qi,
    self_missing, canonical_alias, variant_alias, ancestor_alias, canonical_table,
    canonical_lifetime, variant_table, variant_lifetime, ancestor_table,
    ancestor_lifetime, setter_table, setter_patch, exception, count
};
inline constexpr const char* list_refusal_reasons[]={
    "rejected:list certification null list", "rejected:list certification proxy pin failed",
    "rejected:list certification canonical QI failed/null", "rejected:list certification non-direct BUNDLE",
    "rejected:list certification non-direct other", "rejected:list certification variant E_NOINTERFACE output",
    "rejected:list certification variant QI failed/null", "rejected:list certification variant canonical mismatch",
    "rejected:list certification ancestor E_NOINTERFACE output", "rejected:list certification ancestor QI failed/null",
    "rejected:list certification ancestor canonical mismatch", "rejected:list certification base variant missing",
    "rejected:list certification stable QI mismatch", "rejected:list certification round-trip QI mismatch",
    "rejected:list certification self absent from variants", "rejected:list certification canonical alias conflict",
    "rejected:list certification variant alias conflict", "rejected:list certification ancestor alias conflict",
    "rejected:list certification canonical table unavailable", "rejected:list certification canonical lifetime patch failed",
    "rejected:list certification variant table unavailable", "rejected:list certification variant lifetime patch failed",
    "rejected:list certification ancestor table unavailable", "rejected:list certification ancestor lifetime patch failed",
    "rejected:list certification setter table unavailable", "rejected:list certification setter patch failed",
    "rejected:list certification exception/allocation failure"
};
inline constexpr size_t list_refusal_count=static_cast<size_t>(ListRefusal::count);
inline constexpr size_t list_refusal_records=list_refusal_count*3;
static_assert(list_refusal_count==27&&std::size(list_refusal_reasons)==list_refusal_count);
// Conservative native bound; null-list has no draw, and the generic checker's
// incomplete-state literal is refined by the hook. Keep all literals inventoried.
// Propagation is an outcome only, never a direct certification-refusal key.
inline constexpr const char* shared_table_invalidation="rejected:list shared-table certification invalidation";
inline constexpr size_t metadata_static_outcome_count=53+list_refusal_count;
inline constexpr size_t native_metadata_outcome_count=metadata_static_outcome_count-2;
static_assert(outcome_capacity==32&&outcome_records==33);

inline constexpr uint64_t summary_min_us=60000000,summary_refresh_us=300000000;
inline constexpr size_t reserved_records=pso_records+root_records+startup_records+checkpoint_records+reason_records+outcome_records+list_path_records+list_refusal_records+12+step1_records; // nine L3/CPU first examples + three latest snapshots + step-1 evidence
inline constexpr size_t max_evidence_size=reserved_records*max_record_length;
inline constexpr size_t max_disk_bytes=(max_rotated_files+1)*max_file_size+max_evidence_size+CpuUploadSnapshot::limit*CpuUploadSnapshot::file_cap;
static_assert(max_disk_bytes<=9*1024*1024/2 && max_records>reserved_records);
inline constexpr size_t batch_capacity=ring_capacity*max_record_length;
inline constexpr size_t text_capacity=448;
// Each companion's first accepted sample shares the existing startup evidence
// quota; the remaining samples use the ordinary ring/files. Five L3 first
// examples, four CPU first examples and three latest counter slots are independent of ring/startup pressure.
inline constexpr size_t companion_kind_count=18,companion_sample_limit=64;
enum class Event : unsigned { proxy_load, device, hook_pin, hook_patch, pso, root,
    descriptor, capture_decision, metadata_disabled, execute_summary, summary,
    config, missing_present, requirements_metadata, count };
static_assert(checkpoint_records>=4+2*static_cast<size_t>(Event::count));
static_assert(std::atomic<uint64_t>::is_always_lock_free&&std::atomic<size_t>::is_always_lock_free&&
              std::atomic<bool>::is_always_lock_free&&std::atomic<const char*>::is_always_lock_free);
inline constexpr size_t two_hour_summary_bound=(7200000000ULL/summary_min_us+1)*(2+2*static_cast<size_t>(Event::count));
static_assert(two_hour_summary_bound+56758<max_records-reserved_records); // J-3b7/J-3b7c: -32 for the step-3 partition
// Reason schema: sequence=first, a=last, b=repeat count, c=first known
// disabling operation (0 unknown, 1 write, 2 copy, 3 creation, 4 release).
// Overflow has the same schema with text="observer reason overflow"; its
// count is also exported as metadata_disabled priority overflow in aggregates.
enum class DisableEvent : unsigned { unknown, descriptor_write, descriptor_copy, creation, release };
enum class Identity : unsigned { unknown, hash_only_no_container_match, match };
struct ShaderIdentity {
    Identity identity=Identity::unknown;
    std::array<char,33> hash{};
    std::array<char,17> container_prefix{};
};
// Caller supplies stable readable shader bytes, <=64KiB. CPU metadata + SHA256
// only. Never run while holding the hook mutex; this is not draw-path work.
ShaderIdentity shader_identity(const void*,size_t) noexcept;
// J-world-objects (only called with gpu_copy_step5_world_objects): the DrawACM_v / DrawACM_p pair of
// stored-draw-manifest-2.md. VS: decoded 128-bit HASH b569109e...e5 AND the full container SHA-256
// 7c6972d0...b32dc (both in the manifests). PS: decoded HASH c1b58903...6d AND the full container SHA-256 c3e3b1a7...b72f
// (acm-shader-dataflow.md section 0; J-180: both are required; the 8+4 hex fingerprint alone is only a diagnostic).
// vs_hash_only: the HASH matched but the container SHA-256 did not (logged, never a match).
// J-180: the PS is accepted only with ps_match (decoded hash AND full container SHA-256 equal the pins). ps_fingerprint (8+4 hex) alone is a
// diagnostic: ps_unverified = the fingerprint matched but the full pair did not (ps_hash / ps_digest then hold the live values for the log).
struct AcmWorldIdentity {bool vs_match=false,vs_hash_only=false,ps_fingerprint=false,ps_match=false,ps_unverified=false;std::array<char,33> ps_hash{};std::array<char,65> ps_digest{};};
// `pins` (offline tests only) replaces the DrawACM constants so the comparison logic can be exercised with
// the repository's real E10 containers; nullptr = the DrawACM pins above.
struct AcmPins {const char* vs_hash;const char* vs_digest;const char* ps_prefix8;const char* ps_suffix4;const char* ps_hash=nullptr;const char* ps_digest=nullptr;};
AcmWorldIdentity acm_world_identity(const void* vs,size_t vsBytes,const void* ps,size_t psBytes,const AcmPins* pins=nullptr) noexcept;
struct Record {
    uint64_t sequence=0;
    Event kind=Event::summary;
    uint64_t a=0,b=0,c=0;
    std::array<char,text_capacity> text{};
    ShaderIdentity shader;
};
Record record(Event,uint64_t a=0,uint64_t b=0,uint64_t c=0,const char* text=nullptr) noexcept;
struct Formatted { std::array<char,max_record_length+1> bytes{};size_t size=0;bool valid=false; };
// Pure fixed schema. Includes one terminating newline, no interior newlines.
// Never allocates. Invalid enum / overlength formatted record is rejected.
Formatted format_record(const Record&) noexcept;
struct Paths {std::filesystem::path logs,game,config;};
struct PathProvider {virtual ~PathProvider()=default;virtual Paths paths()=0;};
struct DefaultPaths final : PathProvider {Paths paths() override;};
struct IOProvider : ConfigReader {
    virtual bool create_directory(const std::filesystem::path&)=0;
    virtual bool size(const std::filesystem::path&,uint64_t&)=0;
    virtual size_t append(const std::filesystem::path&,const char*,size_t)=0;
    // Consumer-only truncation of a validated evidence file at activation.
    // Previous invocation's evidence is replaced, never mixed with this one.
    virtual bool reset(const std::filesystem::path&)=0;
    // Consumer-only bounded replacement of an already reserved evidence slot.
    // Providers without replacement support fail optional logging safely.
    virtual size_t overwrite(const std::filesystem::path&,uint64_t,const char*,size_t) {return 0;}
    // Remove oldest, shift .1 -> .2, current -> .1; count <=hard cap.
    virtual bool rotate(const std::filesystem::path&,size_t count)=0;
    // FPS hook cost (docs/world-render/fps-investigation.md section 8): one consumer flush is one write session. Between begin_batch and
    // end_batch a provider may keep the file of consecutive append/overwrite calls open instead of opening and closing it per call (the
    // evidence file is rewritten once per changed reserved slot per flush). Bytes, offsets and order on disk are unchanged; any other
    // operation, another path, or end_batch closes it first. end_batch false = a deferred close failed (the caller fails the log, as a
    // failed per-call close did). Defaults: no session (fixtures unchanged).
    virtual void begin_batch() noexcept {}
    virtual bool end_batch() noexcept {return true;}
};
struct FileIO final : IOProvider {
    bool create_directory(const std::filesystem::path&) override;
    bool size(const std::filesystem::path&,uint64_t&) override;
    size_t append(const std::filesystem::path&,const char*,size_t) override;
    bool reset(const std::filesystem::path&) override;
    size_t overwrite(const std::filesystem::path&,uint64_t,const char*,size_t) override;
    bool rotate(const std::filesystem::path&,size_t) override;
    ReadResult read_config(const std::filesystem::path&,char*,size_t) override;
    void begin_batch() noexcept override;
    bool end_batch() noexcept override;
    FileIO()=default;
    FileIO(const FileIO&)=delete;FileIO& operator=(const FileIO&)=delete;
    ~FileIO();
    uint64_t opens=0,updateOpens=0; // file opens made by append/overwrite, and how many were "r+b" (consumer only; fps_growth_offline)
    uint64_t closes=0; // fclose calls on handles counted in `opens` (balanced with opens once no session handle is held)
    // Fault injection (fps_growth_offline only; always null in production): called in held() after a successful session open and
    // before the handle is committed; it may throw to simulate an allocation failure at that point.
    void (*fault_after_open)()=nullptr;
private:
    std::FILE* held_=nullptr;std::filesystem::path heldPath_;
    bool batching_=false,batchOk_=true,heldUpdate_=false;
    void close_held() noexcept; // fflush+fclose the held file; a failure is reported by end_batch
    // The held handle for this path, opened "ab" (append) or "r+b" (update) like the per-call path (nullptr: not batching / cannot open).
    std::FILE* held(const std::filesystem::path&,bool update) noexcept;
};
// Pure caller-clock accumulator. Microseconds; last baseline is not a sample.
// Bounded to 1M samples, intervals clamped to 1s, sum <=1e12; no overflow.
inline constexpr uint64_t max_interval_samples=1000000,max_interval_us=1000000;
struct IntervalSummary {uint64_t count=0,min=0,max=0,mean=0;std::array<uint64_t,4> buckets{};};
class IntervalCounter {
public:
    void observe(uint64_t monotonic_us) noexcept;
    IntervalSummary summary() const noexcept;
private:
    bool seen_=false;uint64_t last_=0,sum_=0;
    IntervalSummary value_;
};
// rotation_failures (U-soak): ordinary-log rotations that could not rename (e.g. an external reader without FILE_SHARE_DELETE held a
// file); the pending batch is dropped (counted in `dropped`) and the rotation is retried on the next flush. Never a log failure.
struct Counters {uint64_t dropped=0,io_failures=0,config_failures=0,unknown_keys=0,written=0,rotations=0,rotation_failures=0;};
// Aggregate/summary schema: a=Event index, b=cumulative observed attempts,
// c=cumulative dropped records of THAT kind. Read the latest snapshot; never
// sum snapshots. execute_summary attempts include submission observations and
// their consumer summary records. Priority overflow uses its own tagged line.
class Log final {
public:
    using Clock=uint64_t(*)() noexcept;
    // Explicit standalone flush callers retain the library's original policy.
    // The DLL always selects production=true: require valid explicit opt-in,
    // and batch output into bounded 32 KiB appends rather than per-record I/O.
    explicit Log(PathProvider& paths,IOProvider& io,Clock clock=nullptr,bool production=false) noexcept;
    ~Log();
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    // No allocation/COM/I/O/wait. Ordinary traffic tries the ring once, dropping
    // on contention/full. Production identities use independent append-only
    // atomic reservations; high-volume kinds only update atomic counters.
    bool push(Record,unsigned verbosity=1) noexcept;
    bool raw_snapshot(const unsigned char*,size_t) noexcept; // fixed producer slots, no I/O
    void execute_interval() noexcept;
    // DEDICATED serialized producer: existing capture-state mutex MUST be held.
    // Static literal text, <=127 chars; same text deduplicates across addresses.
    // No allocation/I/O/new lock/CAS loop. Exact updates under that mutex;
    // per-field atomics plus generation publish a nonblocking safe snapshot.
    void observer_disabled(const char* static_reason,DisableEvent=DisableEvent::unknown) noexcept;
    // D-81 serialized producer, caller holds the capture-state mutex. POD first
    // example, deduplicated static outcome. No allocation/I/O/native calls.
    void list_refused(ListRefusal,ListSeam,unsigned type) noexcept;
    void list_registered(unsigned path) noexcept; // caller holds capture-state mutex
    void metadata_outcome(const requirements::MetadataResult&,const capture::Command&,unsigned list_path=3,const char* context="") noexcept;
    // Sole consumer, caller responsibility: OUTSIDE hook/lifecycle critical
    // sections and loader locks. No thread/destructor I/O. Concurrent flush
    // returns at once. Production configuration is read once on activation
    // from the factory export, never from a D3D12 hook or DllMain.
    void flush() noexcept;
    // Activation consumer gate required. A Release checkpoint, NOT a process
    // teardown assertion: first final object Release after observed submissions.
    // Pre-submission enumeration factories cannot consume the checkpoint.
    void final_summary() noexcept;
    bool has_submissions() const noexcept {return submitted_.load(std::memory_order_relaxed);}
    // U-soak (Batch U, 2026-10-29): fail-safe. A LATER optional I/O failure of this shared log (fail(): enabled() turns false for good)
    // must not change runtime behaviour. Before, ownership_pins() followed enabled(), so one failed append/rename mid-flight switched
    // the L3 observation off, every cockpit draw went out-of-class (t10 unknown) and the step-5 producer, its XR frames and the
    // xr-stereo.log routing stopped for the rest of the process. started(): the first flush initialized the log with logging enabled;
    // it never clears. A failure DURING initialization still leaves every pin off (started() stays false).
    bool started() const noexcept {return started_.load(std::memory_order_relaxed);}
    // Dedicated feature logs (xr-stereo.log; they have their own files) and runtime consumers gate on this, never on enabled().
    bool feature_logs_enabled() const noexcept {return enabled()||started();}
    bool ownership_pins() const noexcept {return feature_logs_enabled()&&ownership_pins_.load(std::memory_order_relaxed);}
    // Latched at activation: optional log I/O failure cannot erase history.
    bool gpu_copy_witness_observation() const noexcept {return gpu_copy_witness_observation_.load(std::memory_order_relaxed);}
    // Latched like the witness switch; the hook additionally requires
    // ownership pins and witness observation before anything is armed.
    bool gpu_copy_step1_experiment() const noexcept {return gpu_copy_step1_experiment_.load(std::memory_order_relaxed);}
    // J-3b4: latched like the step-1 switch; the hook additionally requires the
    // whole step-1 enablement before the early device notification does anything.
    bool gpu_copy_early_device_hook() const noexcept {return gpu_copy_early_device_hook_.load(std::memory_order_relaxed);}
    bool gpu_copy_step2_experiment() const noexcept {return gpu_copy_step2_experiment_.load(std::memory_order_relaxed);}
    bool gpu_copy_step3_experiment() const noexcept {return gpu_copy_step3_experiment_.load(std::memory_order_relaxed);}
    bool gpu_copy_step3_whole_set() const noexcept {return gpu_copy_step3_whole_set_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_hud_observe() const noexcept {return gpu_copy_step5_hud_observe_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_hud() const noexcept {return gpu_copy_step5_hud_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_continuous() const noexcept {return gpu_copy_step5_continuous_.load(std::memory_order_relaxed);}
    // J-cockpit-fov: latched like the other step-5 keys (opt-in + logging); hmd_fov additionally
    // requires gpu_copy_step5_continuous in the same config.
    bool gpu_copy_step5_hmd_fov() const noexcept {return gpu_copy_step5_hmd_fov_.load(std::memory_order_relaxed);}
    float gpu_copy_step5_units_per_metre() const noexcept {return gpu_copy_step5_units_per_metre_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_precockpit_observe() const noexcept {return gpu_copy_step5_precockpit_observe_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_members_by_class() const noexcept {return gpu_copy_step5_members_by_class_.load(std::memory_order_relaxed);}
    // J-world-objects: latched only together with gpu_copy_step5_continuous AND gpu_copy_step5_members_by_class (same config).
    bool gpu_copy_step5_world_objects() const noexcept {return gpu_copy_step5_world_objects_.load(std::memory_order_relaxed);}
    // J-world-objects v2 step 1: latched with logging + the step-5 producer (the observer logs from the producer's Draw seam).
    bool gpu_copy_step5_world_observe() const noexcept {return gpu_copy_step5_world_observe_.load(std::memory_order_relaxed);}
    // J-phase-observe: latched with logging + the step-5 producer (the observer logs from the producer's worker).
    bool gpu_copy_step5_phase_observe() const noexcept {return gpu_copy_step5_phase_observe_.load(std::memory_order_relaxed);}
    // J-hud-colour: latched like the other step-5 keys; requires gpu_copy_step5_continuous in the same config.
    bool gpu_copy_step5_png_once() const noexcept {return gpu_copy_step5_png_once_.load(std::memory_order_relaxed);}
    // J-xr-stereo-output: latched like the experiment switches (opt-in + logging).
    bool xr_stereo_output() const noexcept {return xr_stereo_output_.load(std::memory_order_relaxed);}
    // J-hud-consumer: latched only together with xr_stereo_output (opt-in + logging).
    bool xr_hud_layer() const noexcept {return xr_hud_layer_.load(std::memory_order_relaxed);}
    unsigned hud_mode() const noexcept {return hud_mode_.load(std::memory_order_relaxed);}
    // J-feather: latched only together with xr_stereo_output (opt-in + logging); else 0 / 40 / unset.
    float xr_edge_feather() const noexcept {return xr_edge_feather_.load(std::memory_order_relaxed);}
    float xr_world_quad_distance_m() const noexcept {return xr_world_quad_distance_m_.load(std::memory_order_relaxed);}
    bool xr_comfort_configured() const noexcept {return xr_comfort_configured_.load(std::memory_order_relaxed);}
    // J-world-in-projection: latched only together with xr_stereo_output (opt-in + logging).
    bool xr_world_in_projection() const noexcept {return xr_world_in_projection_.load(std::memory_order_relaxed);}
    // J-world-hold / J-world-extend: latched only with xr_world_in_projection latched (so with xr_stereo_output).
    bool xr_world_hold_last() const noexcept {return xr_world_hold_last_.load(std::memory_order_relaxed);}
    bool xr_world_extend() const noexcept {return xr_world_extend_.load(std::memory_order_relaxed);}
    bool xr_world_cache() const noexcept {return xr_world_cache_.load(std::memory_order_relaxed);} // J-world-cache: latched with xr_world_in_projection
    // J-terrain-render: latched with xr_world_in_projection (and a non-empty stage); the stage string is written once at config load, before any reader.
    bool xr_terrain_render() const noexcept {return xr_terrain_render_.load(std::memory_order_relaxed);}
    const char* xr_terrain_stage() const noexcept {return xr_terrain_stage_;}
    bool xr_terrain_preset_vrn() const noexcept {return xr_terrain_preset_vrn_.load(std::memory_order_relaxed);} // D-400
    // COCKPIT-2: latched with xr_world_in_projection (non-empty value = on); written once at config load, before any reader.
    const char* xr_cockpit_model() const noexcept {return xr_cockpit_model_;}
    // J-hud-placement: latched only together with xr_stereo_output + xr_hud_layer; else unset / 1 / 2.5 / 1.6.
    float xr_hud_quad_distance_m() const noexcept {return xr_hud_quad_distance_m_.load(std::memory_order_relaxed);}
    float xr_hud_scale() const noexcept {return xr_hud_scale_.load(std::memory_order_relaxed);}
    float xr_hud_head_distance_m() const noexcept {return xr_hud_head_distance_m_.load(std::memory_order_relaxed);}
    float xr_hud_head_width_m() const noexcept {return xr_hud_head_width_m_.load(std::memory_order_relaxed);}
    bool xr_hud_placement_configured() const noexcept {return xr_hud_placement_configured_.load(std::memory_order_relaxed);}
    // J-w1-ee-camera: latched like the experiment switches (opt-in + logging).
    bool ee_camera_probe() const noexcept {return ee_camera_probe_.load(std::memory_order_relaxed);}
    bool ee_camera_probe_position() const noexcept {return ee_camera_probe_position_.load(std::memory_order_relaxed);}
    unsigned ee_camera_probe_points() const noexcept {return ee_camera_probe_points_.load(std::memory_order_relaxed);}
    bool ee_camera_write_trials() const noexcept {return ee_camera_write_trials_.load(std::memory_order_relaxed);}
    // J-hud-markers: producer key latched with gpu_copy_step5_hud (opt-in + logging); consumer key latched
    // with xr_stereo_output + xr_hud_layer. Both default OFF.
    bool gpu_copy_step5_hud_markers() const noexcept {return gpu_copy_step5_hud_markers_.load(std::memory_order_relaxed);}
    bool xr_hud_markers_layer() const noexcept {return xr_hud_markers_layer_.load(std::memory_order_relaxed);}
    // Option 3 stage 2 (ee_look_write.h): latched only with ee_camera_probe; drive also needs the trial key.
    bool ee_camera_look_write_trial() const noexcept {return ee_camera_look_write_trial_.load(std::memory_order_relaxed);}
    bool ee_camera_look_drive() const noexcept {return ee_camera_look_drive_.load(std::memory_order_relaxed);}
    bool ee_camera_look_input_trace() const noexcept {return ee_camera_look_input_trace_.load(std::memory_order_relaxed);}
    // Lens/FOV widen plan phase 0 (ee_lens_scan.h): read-only lens scan; latched via Config::lens_scan_latch (needs logging + ee_camera_probe, refused with any write key).
    bool ee_lens_scan() const noexcept {return ee_lens_scan_.load(std::memory_order_relaxed);}
    // PS2 address probe (ee_ps2_probe.h): read-only; latched via Config::ps2_probe_latch (same rules as ee_lens_scan).
    bool ee_ps2_probe() const noexcept {return ee_ps2_probe_.load(std::memory_order_relaxed);}
    // Option 3 via keyboard (look_drive_keys.h): latched only with xr_stereo_output + gpu_copy_step5_continuous (opt-in + logging); the sweep also needs the key.
    bool xr_look_drive_keys() const noexcept {return xr_look_drive_keys_.load(std::memory_order_relaxed);}
    bool xr_look_drive_test_sweep() const noexcept {return xr_look_drive_test_sweep_.load(std::memory_order_relaxed);}
    bool xr_look_drive_xinput() const noexcept {return xr_look_drive_xinput_.load(std::memory_order_relaxed);}           // latched with the keys
    bool xr_look_drive_closed_loop() const noexcept {return xr_look_drive_closed_loop_.load(std::memory_order_relaxed);} // latched with the keys
    // POV fixes (docs/review/pov-fixes-impl.md): ref cluster latched with the look-drive keys; lean (lever, scale; 0 / 1 = off) with
    // gpu_copy_step5_continuous + xr_stereo_output; hud follow (0..2) with xr_stereo_output + xr_hud_layer.
    bool xr_look_ref_cluster() const noexcept {return xr_look_ref_cluster_.load(std::memory_order_relaxed);}
    float pov_neck_lever_m() const noexcept {return pov_neck_lever_m_.load(std::memory_order_relaxed);}
    float pov_translation_scale() const noexcept {return pov_translation_scale_.load(std::memory_order_relaxed);}
    unsigned xr_hud_follow() const noexcept {return xr_hud_follow_.load(std::memory_order_relaxed);}
    // J-effects-layer: producer key latched with gpu_copy_step5_continuous + gpu_copy_step5_hud (opt-in + logging); consumer key latched
    // with xr_stereo_output + xr_world_in_projection. Both default OFF.
    bool gpu_copy_step5_effects() const noexcept {return gpu_copy_step5_effects_.load(std::memory_order_relaxed);}
    bool gpu_copy_step5_effects_dual() const noexcept {return gpu_copy_step5_effects_dual_.load(std::memory_order_relaxed);}
    // J-hud-radar: producer key latched with gpu_copy_step5_hud (opt-in + logging). Default OFF.
    bool gpu_copy_step5_hud_radar() const noexcept {return gpu_copy_step5_hud_radar_.load(std::memory_order_relaxed);}
    bool xr_effects_layer() const noexcept {return xr_effects_layer_.load(std::memory_order_relaxed);}
    // J-test8-fixes: xr_world_objects_depth_fix is latched with gpu_copy_step5_world_objects (producer); xr_png_once and xr_debug_view with
    // xr_stereo_output (consumer; xr_debug_view 0 = off). All default OFF.
    bool xr_world_objects_depth_fix() const noexcept {return xr_world_objects_depth_fix_.load(std::memory_order_relaxed);}
    bool xr_world_window_occlude() const noexcept {return xr_world_window_occlude_.load(std::memory_order_relaxed);} // J-occlusion-fix (consumer)
    bool xr_png_once() const noexcept {return xr_png_once_.load(std::memory_order_relaxed);}
    unsigned xr_debug_view() const noexcept {return xr_debug_view_.load(std::memory_order_relaxed);}
    // FPS investigation: perf_lean (diagnostic log volume cuts only; latched with logging). Default OFF.
    bool perf_lean() const noexcept {return perf_lean_.load(std::memory_order_relaxed);}
    // The sweep latched WITHOUT xr_stereo_output (no-XR test sweep: scripted head, no pose, no consumer).
    bool xr_look_drive_no_xr() const noexcept {return xr_look_drive_no_xr_.load(std::memory_order_relaxed);}
    Counters counters() const noexcept;
    uint64_t dropped(Event) const noexcept;
    uint64_t overflow(Event) const noexcept;
    // Attempts at the configured verbosity, including consumer snapshots.
    // Dropped records are a subset; rotation evicts only ordinary history.
    uint64_t observed(Event) const noexcept;
private:
    struct Reserved;
    // Allocated once on the activation consumer, never by a producer. Failure
    // disables optional logging. Standalone fixtures retain the ring policy.
    std::unique_ptr<Reserved> reserved_;
    PathProvider& paths_;IOProvider& io_;Clock clock_;
    std::atomic_flag ring_lock_=ATOMIC_FLAG_INIT,flush_lock_=ATOMIC_FLAG_INIT;
    std::array<Record,ring_capacity> ring_{};size_t head_=0,count_=0;
    std::array<size_t,companion_kind_count> companion_samples_{}; // ring_lock_ only
    std::atomic<uint64_t> sequence_{0};
    IntervalCounter intervals_;
    std::array<std::atomic<uint64_t>,static_cast<size_t>(Event::count)> events_{};
    std::array<std::atomic<uint64_t>,static_cast<size_t>(Event::count)> drops_{};
    std::array<std::atomic<uint64_t>,static_cast<size_t>(Event::count)> overflow_{};
    std::atomic<uint64_t> dropped_{0},io_failures_{0},config_failures_{0},unknown_keys_{0},written_{0},rotations_{0},rotation_failures_{0};
    std::atomic<bool> ownership_pins_{false},gpu_copy_witness_observation_{false},gpu_copy_step1_experiment_{false};
    std::atomic<bool> gpu_copy_early_device_hook_{false},gpu_copy_step2_experiment_{false},gpu_copy_step3_experiment_{false},gpu_copy_step3_whole_set_{false};
    std::atomic<bool> gpu_copy_step5_hud_observe_{false},gpu_copy_step5_hud_{false};
    std::atomic<bool> gpu_copy_step5_continuous_{false},xr_stereo_output_{false};
    std::atomic<bool> gpu_copy_step5_hmd_fov_{false},gpu_copy_step5_precockpit_observe_{false},gpu_copy_step5_members_by_class_{false};
    std::atomic<bool> gpu_copy_step5_world_objects_{false},gpu_copy_step5_world_observe_{false},gpu_copy_step5_phase_observe_{false};
    std::atomic<bool> gpu_copy_step5_png_once_{false};
    std::atomic<float> gpu_copy_step5_units_per_metre_{39.6f};
    std::atomic<bool> xr_hud_layer_{false};std::atomic<unsigned> hud_mode_{0};
    std::atomic<float> xr_edge_feather_{0.0f},xr_world_quad_distance_m_{40.0f};std::atomic<bool> xr_comfort_configured_{false};
    std::atomic<float> xr_hud_quad_distance_m_{0.0f},xr_hud_scale_{1.0f},xr_hud_head_distance_m_{2.5f},xr_hud_head_width_m_{1.6f};
    std::atomic<bool> xr_hud_placement_configured_{false};
    std::atomic<bool> xr_world_in_projection_{false},xr_world_hold_last_{false},xr_world_extend_{false},xr_world_cache_{false};
    std::atomic<bool> xr_terrain_render_{false},xr_terrain_preset_vrn_{false};char xr_terrain_stage_[acz::diagnostics::xr_terrain_stage_capacity]={};
    char xr_cockpit_model_[192]={}; // COCKPIT-2 (was 100: the step169 stwatch+sttrack line overflowed it and refused the whole config)
    std::atomic<bool> ee_camera_probe_{false},ee_camera_probe_position_{false},ee_camera_write_trials_{false};std::atomic<unsigned> ee_camera_probe_points_{7};
    std::atomic<bool> gpu_copy_step5_hud_markers_{false},xr_hud_markers_layer_{false};
    std::atomic<bool> ee_camera_look_write_trial_{false},ee_camera_look_drive_{false};
    std::atomic<bool> ee_camera_look_input_trace_{false};
    std::atomic<bool> ee_lens_scan_{false};
    std::atomic<bool> ee_ps2_probe_{false};
    std::atomic<bool> xr_look_drive_keys_{false},xr_look_drive_test_sweep_{false},xr_look_drive_no_xr_{false},xr_look_drive_xinput_{false},xr_look_drive_closed_loop_{false};
    std::atomic<bool> gpu_copy_step5_effects_{false},xr_effects_layer_{false},gpu_copy_step5_effects_dual_{false};
    std::atomic<bool> xr_look_ref_cluster_{false};std::atomic<float> pov_neck_lever_m_{0.0f},pov_translation_scale_{1.0f};std::atomic<unsigned> xr_hud_follow_{0};
    std::atomic<bool> gpu_copy_step5_hud_radar_{false};
    std::atomic<bool> xr_world_objects_depth_fix_{false},xr_world_window_occlude_{false},xr_png_once_{false};std::atomic<unsigned> xr_debug_view_{0};
    std::atomic<bool> perf_lean_{false};
    std::atomic<bool> enabled_{true};std::atomic<unsigned> verbosity_{1};
    std::atomic<bool> started_{false}; // U-soak: set once the first flush initialized the log with logging enabled; never cleared
    bool initialized_=false,failed_=false;
    bool production_=false,final_=false,checkpoint_pending_=false;
    std::atomic<bool> submitted_{false};
    std::array<char,batch_capacity> batch_{};size_t batch_size_=0,batch_records_=0;
    // Sole flush consumer, guarded by flush_lock_. Recursive/concurrent flush
    // returns before accessing this buffer; producers never access it.
    std::array<Record,1+2*static_cast<size_t>(Event::count)> summary_records_{};
    std::filesystem::path file_,evidence_file_;
    std::filesystem::path snapshot_directory_,snapshot_game_;
    size_t raw_drained_=0;
    size_t ordinary_written_=0,evidence_bytes_=0,evidence_written_=0;
    bool evidence_batch_=false;
    bool summary_seen_=false;uint64_t summary_us_=0;
    std::array<uint64_t,static_cast<size_t>(Event::count)> last_events_{},last_drops_{};
    bool pop(Record&) noexcept;
    void write(const Record&);
    void append_batch();
    void write_evidence(const Record&);
    void drain_reserved();
    void drain_reasons();
    void drain_outcomes();
    void drain_l3_counts();
    void drain_raw_snapshots();
    void drop(Event,uint64_t count=1) noexcept;
    void fail() noexcept;
};
// Borrowed process-lifetime optional sink. Install/remove only while producers
// are quiescent, and keep it alive through all in-flight emits AND flushes.
// No default allocation, files, thread, config read or native enablement.
void set_sink(Log*) noexcept;
Log* sink() noexcept;
void emit(Record,unsigned verbosity=1) noexcept;
void execute_interval() noexcept;
void decision(bool enabled,const char* static_reason) noexcept;
// Same capture-state mutex/static text precondition as Log::observer_disabled.
// A null sink is harmless and does not assert or allocate.
void observer_disabled(const char*,DisableEvent=DisableEvent::unknown) noexcept;
void list_refused(ListRefusal,ListSeam,unsigned type) noexcept;
void list_registered(unsigned path) noexcept;
void metadata_outcome(const requirements::MetadataResult&,const capture::Command&,unsigned list_path=3,const char* context="") noexcept;

// Track acz hook/lifecycle mutex ownership, including native Release reentry.
// A nested flush skips while ANY such mutex is held by its calling thread.
inline thread_local unsigned mutex_depth=0;
class Mutex {
    std::mutex value_;
public:
    void lock() {value_.lock();++mutex_depth;}
    bool try_lock() {if(!value_.try_lock())return false;++mutex_depth;return true;}
    void unlock() noexcept {--mutex_depth;value_.unlock();}
};
inline constexpr uint64_t flush_submissions=64,flush_window_us=250000;
// Production ordinary files rotate throughout play. The separate evidence file
// preserves startup, PSO/root identity and one Release checkpoint. N/M bounds
// flush attempts, not session duration. No producer performs allocation or I/O.
// Lazy export-only activation, after real DXGI load succeeds OR fails. Terminal
// on exception, no loader-lock work, threads or destructors. Installed Log and
// providers are deliberately never destroyed: hooked Release may run at CRT
// teardown. Providers injected by CPU fixtures must outlive their activation.
struct Activation {
    std::once_flag once;
    PathProvider* paths=nullptr;IOProvider* io=nullptr;Log::Clock clock=nullptr;
    std::atomic<Log*> log{nullptr};
    std::atomic_flag flushing=ATOMIC_FLAG_INIT;
    uint64_t submissions=0,last_us=0;
    bool checkpoint=false;
    std::atomic<uint64_t> worker_us{0}; // FPS investigation (perf_lean): clock of the last flush made by the producer worker
};
void activate() noexcept;
// Post-original Execute only: hook/callback locals and their mutex guards have
// unwound. Atomic try-acquire, others skip; no allocation on the disabled path.
// Due on N submissions OR M microseconds, subtraction after monotonic check.
// Contending submissions skip both I/O and accounting; the count saturates.
// Final wrapper/device Release: native Release and cleanup have returned; no
// hook/lifecycle mutex held. Same atomic gate suppresses recursive Release I/O.
// The first post-submission Release writes a single reserved checkpoint; this
// means final Release of THAT object, never proven process/session teardown.
// Later Releases use the clock limit only (do not increment submission count),
// and pre-submission Releases do nothing. Tracked device references may prevent
// a device-final Release entirely; no teardown flush is promised.
// Errors never escape. Ordinary drain is <=64 records plus compact summaries;
// priority drain is <=4864 append-only records plus <=65 reason snapshots, in <=32KiB appends. At most two ordinary
// plus <=33 metadata outcome and <=3 creation-path and <=81 refusal snapshots. At most two ordinary rotations per
// flush, each two renames + removal. Reserved data never rotates.
// OS I/O duration is not bounded; real-process latency remains unmeasured.
void flush_after_execute() noexcept;
void flush_after_release() noexcept;
// FPS investigation (perf_lean, docs/world-render/fps-investigation.md): the periodic flush (ring drain + file appends/rotations) runs on
// the step-5 worker thread (every 100 ms tick, same 250 ms clock limit) instead of inside the game's ExecuteCommandLists. With perf_lean,
// flush_after_execute skips while a worker flush happened within the last 2 s (so the render-thread path still flushes if no worker runs).
// perf_lean off: returns at once (the render-thread flush is unchanged). Release checkpoints are unchanged either way.
void flush_from_worker() noexcept;
#ifdef ACZ_DIAGNOSTICS_OFFLINE_TEST
// Per-scenario one-shot state, never a production reset/retry facility.
extern Activation* activation_test;
#endif
}
