#include "diagnostics_log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "perf_counters.h"
#include "capture_enablement.h"
#include "capture_requirements_proof.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <stdexcept>
#include <vector>
#include <memory>
#include <new>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <share.h>

namespace acz::diagnostics {
namespace {
std::atomic<Log*> active{nullptr};
constexpr const char* names[]={"proxy_load","device","hook_pin","hook_patch","pso","root",
    "descriptor","capture_decision","metadata_disabled","execute_summary","summary","config","missing_present","requirements_metadata"};
static_assert(std::size(names)==static_cast<size_t>(Event::count));
// Independently pinned by registered E10 JSON and E11 SOURCE.md. Full SHA256,
// NOT the DXBC header checksum or decoded 128-bit HASH, controls match status.
constexpr const char* hashes[]={"f4cfd928c11d0a822baeb92e09767134","e1372ac10d7191c7e4d1c03b66465260","06084b6262839d7b336b1d8d0d0b86b8"};
constexpr const char* digests[]={"adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd",
    "7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd",
    "847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da"};
constexpr const char* acm_vs_hash="b569109edcbb3904d48011cfb0dad5e4";
constexpr const char* acm_vs_digest="7c6972d0fd0b8261a1745113d6bc636daf0f5b808c3629c4aebadcb82abb32dc";
// J-180 (Codex review, P2 "pipeline authentication"): the PS is pinned by its decoded 128-bit shader hash AND its FULL container SHA-256
// (docs/interception/acm-shader-dataflow.md section 0, recomputed there from blobs 000033). The partial fingerprint (first 8 + last 4 hex)
// is kept as a DIAGNOSTIC only: a fingerprint without the full pair is counted and logged, never accepted.
constexpr const char* acm_ps_hash="c1b58903d5b4acf5a46f0a70b5fdb06d";
constexpr const char* acm_ps_digest="c3e3b1a7d7903ed3aa0b6b9df72fcb986af241c68efb48bef5c64ea9d715b72f";
template<size_t N> void copy_text(std::array<char,N>& out,const char* p) noexcept {
    if(!p)return;
    for(size_t i=0;i+1<N&&p[i];++i) {
        const auto c=static_cast<unsigned char>(p[i]);
        out[i]=c>=32&&c<=126&&c!='|'?char(c):'_';
    }
    out[N-1]=0;
}
template<size_t N> std::array<char,N> safe(const std::array<char,N>& in) noexcept {
    std::array<char,N> out{};copy_text(out,in.data());return out;
}
template<size_t N> std::array<char,N*2+1> hex(const std::array<uint8_t,N>& bytes) noexcept {
    std::array<char,N*2+1> out{};const char* digits="0123456789abcdef";
    for(size_t i=0;i<N;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}return out;
}
struct FlagGuard {std::atomic_flag& flag;~FlagGuard(){flag.clear(std::memory_order_release);}};
uint64_t default_clock() noexcept {
    auto ms=GetTickCount64();return ms>UINT64_MAX/1000?UINT64_MAX:ms*1000;
}
// Resolve existing symlinks/junctions before component comparison. A prefix
// string is insufficient (game vs game-other). Windows paths are case-folded.
std::filesystem::path normalized_path(const std::filesystem::path& p) {
    if(p.empty()||!p.is_absolute())throw std::runtime_error("absolute path required");
    // Reject Win32 ambiguous component spellings and alternate data streams;
    // they can otherwise compare differently while naming the same directory.
    for(const auto& component:p.relative_path()) {
        auto s=component.native();
        if(s==L"."||s==L".."||s.empty())continue;
        if(s.back()==L'.'||s.back()==L' '||s.find(L':')!=s.npos)
            throw std::runtime_error("ambiguous path component");
    }
    auto resolved=std::filesystem::weakly_canonical(p).lexically_normal().native();
    for(auto& c:resolved)c=static_cast<wchar_t>(std::towlower(c));
    auto result=std::filesystem::path(resolved);
    while(result!=result.root_path()&&result.filename().empty())result=result.parent_path();
    return result;
}
bool beneath(const std::filesystem::path& path,const std::filesystem::path& parent) {
    auto a=path.begin(),b=parent.begin();
    for(;b!=parent.end();++a,++b)if(a==path.end()||*a!=*b)return false;
    return true;
}
bool safe_path(const std::filesystem::path& p,const std::filesystem::path& game) {
    return !beneath(normalized_path(p),normalized_path(game));
}
std::filesystem::path rotated(const std::filesystem::path& p,size_t i) {return p.wstring()+L"."+std::to_wstring(i);}
}
ShaderIdentity shader_identity(const void* bytes,size_t length) noexcept {
    ShaderIdentity out;
    try {
        if(!bytes||!length||length>65536)return out;
        auto decoded=metadata::decode_shader_metadata(bytes,length);
        if(!decoded.valid)return out;
        out.hash=hex(decoded.identifier);
        // Windows 10+ pseudo-handle: no provider open/close, loader activity or
        // provider initialization lock on the PSO observation path.
        const auto alg=BCRYPT_SHA256_ALG_HANDLE;
        std::array<uint8_t,32> digest{};
        auto status=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),
                              static_cast<ULONG>(length),digest.data(),static_cast<ULONG>(digest.size()));
        for(size_t i=0;i<std::size(hashes);++i)if(std::string_view(out.hash.data())==hashes[i]) {
            out.identity=Identity::hash_only_no_container_match;
            if(status>=0&&std::string_view(hex(digest).data())==digests[i])out.identity=Identity::match;
        }
        if(status>=0){auto text=hex(digest);std::copy_n(text.begin(),16,out.container_prefix.begin());}
    } catch(...) {} // Allocation/decoder/hash failure is optional diagnostics only.
    return out;
}
AcmWorldIdentity acm_world_identity(const void* vs,size_t vsBytes,const void* ps,size_t psBytes,const AcmPins* pins) noexcept {
    const AcmPins pin=pins?*pins:AcmPins{acm_vs_hash,acm_vs_digest,"c3e3b1a7","b72f",acm_ps_hash,acm_ps_digest};
    AcmWorldIdentity out;
    try {
        auto sha=[](const void* bytes,size_t length,std::array<char,65>& text)->bool {
            std::array<uint8_t,32> digest{};
            const auto status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),
                                         static_cast<ULONG>(length),digest.data(),static_cast<ULONG>(digest.size()));
            if(status<0)return false;
            text=hex(digest);return true;
        };
        if(vs&&vsBytes&&vsBytes<=65536) {
            auto decoded=metadata::decode_shader_metadata(vs,vsBytes);
            if(decoded.valid&&std::string_view(hex(decoded.identifier).data())==pin.vs_hash) {
                out.vs_hash_only=true;std::array<char,65> text{};
                if(sha(vs,vsBytes,text)&&std::string_view(text.data())==pin.vs_digest){out.vs_match=true;out.vs_hash_only=false;}
            }
        }
        if(ps&&psBytes&&psBytes<=65536) {
            auto decoded=metadata::decode_shader_metadata(ps,psBytes);std::array<char,65> text{};
            if(decoded.valid&&sha(ps,psBytes,text)) {
                const std::string_view t(text.data());
                out.ps_fingerprint=t.substr(0,8)==pin.ps_prefix8&&t.substr(t.size()-4)==pin.ps_suffix4;
                out.ps_hash=hex(decoded.identifier);out.ps_digest=text;
                // The full pair: decoded hash AND complete container SHA-256 (a null pin = unknown = never a match).
                const bool hashOk=pin.ps_hash&&std::string_view(out.ps_hash.data())==pin.ps_hash;
                const bool digestOk=pin.ps_digest&&t==pin.ps_digest;
                out.ps_match=hashOk&&digestOk;
                out.ps_unverified=out.ps_fingerprint&&!out.ps_match;
            }
        }
    } catch(...) {out=AcmWorldIdentity{};}
    return out;
}
Record record(Event kind,uint64_t a,uint64_t b,uint64_t c,const char* text) noexcept {
    Record out;out.kind=kind;out.a=a;out.b=b;out.c=c;copy_text(out.text,text);return out;
}
Formatted format_record(const Record& r) noexcept {
    Formatted out;const auto kind=static_cast<size_t>(r.kind);
    if(kind>=std::size(names)||static_cast<unsigned>(r.shader.identity)>2)return out;
    auto text=safe(r.text);auto hash=safe(r.shader.hash);auto sha=safe(r.shader.container_prefix);
    const char* identity=r.shader.identity==Identity::match?"match":r.shader.identity==Identity::hash_only_no_container_match?
        "hash-only-no-container-match":"unknown";
    int n=std::snprintf(out.bytes.data(),out.bytes.size(),
        "%llu|%s|a=%llu|b=%llu|c=%llu|identity=%s|hash=%s|sha256=%s|text=%s\n",
        static_cast<unsigned long long>(r.sequence),names[kind],static_cast<unsigned long long>(r.a),
        static_cast<unsigned long long>(r.b),static_cast<unsigned long long>(r.c),identity,hash.data(),sha.data(),text.data());
    if(n>=0&&size_t(n)<=max_record_length){out.size=size_t(n);out.valid=true;}return out;
}
Paths DefaultPaths::paths() {
    // Flush-only allocation; failure is contained by Log::flush().
    std::vector<wchar_t> local(32768),module(32768);
    auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local.data(),static_cast<DWORD>(local.size()));
    auto m=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!n||n>=local.size()||!m||m>=module.size())throw std::runtime_error("path unavailable");
    auto base=std::filesystem::path(local.data())/L"acz-vr";
    return {base/L"logs",std::filesystem::path(module.data()).parent_path(),base/L"runtime.cfg"};
}
// U-soak (Batch U, 2026-10-29): every writable open shares read/write (_SH_DENYNO). _wfopen_s opens a writable file NOT shareable,
// so any concurrent reader of a live log (a sampler's Select-String, a tail, an editor, a scan) made the append/overwrite fail and
// Log::fail() ended the shared diagnostics log for the rest of the process. A rename still needs the reader to share DELETE: a failed
// rotation is transient (Log::write / append_batch). Readers should open with FILE_SHARE_READ|WRITE|DELETE.
static std::FILE* open_shared(const std::filesystem::path& p,const wchar_t* mode) noexcept {return _wfsopen(p.c_str(),mode,_SH_DENYNO);}
// Section 8 write session. Only the single flush consumer (Log::flush, under flush_lock_) calls FileIO, so no lock is needed here.
FileIO::~FileIO() {close_held();}
void FileIO::close_held() noexcept {
    if(!held_)return;
    bool ok=std::fflush(held_)==0;if(std::fclose(held_)!=0)ok=false;
    ++closes;held_=nullptr;heldUpdate_=false;try {heldPath_.clear();}catch(...) {}
    if(!ok)batchOk_=false;
}
void FileIO::begin_batch() noexcept {close_held();batching_=true;batchOk_=true;}
bool FileIO::end_batch() noexcept {close_held();batching_=false;const bool ok=batchOk_;batchOk_=true;return ok;}
// Each call opens with the SAME mode the per-call path uses: "ab" for appends, "r+b" for slot overwrites. Measured (2026-10-27,
// this machine): an existing file opened "r+b", written and closed costs ~1.5 ms vs ~0.08 ms with "ab", so appends must never
// open "r+b" (the first version did: diagnostics_offline realFiles crawled). An append may reuse a held "r+b" handle (no open);
// an overwrite reuses only a held "r+b" handle. So a session never makes more opens of either mode than the per-call path.
std::FILE* FileIO::held(const std::filesystem::path& p,bool update) noexcept {
    if(!batching_)return nullptr;
    try {
        if(held_&&heldPath_==p&&(heldUpdate_||!update))return held_;
        close_held();
        // D-358: everything that can throw happens before the open (path copy) or while RAII owns the new handle; the commit
        // below is nonthrowing, so no exception can lose an opened FILE*.
        std::filesystem::path path=p;
        FILE* raw=open_shared(p,update?L"r+b":L"ab");if(!raw)return nullptr;
        ++opens;if(update)++updateOpens;
        auto closer=[this](FILE* f) noexcept {std::fclose(f);++closes;};
        std::unique_ptr<FILE,decltype(closer)> f(raw,closer);
        if(fault_after_open)fault_after_open();
        heldPath_.swap(path);held_=f.release();heldUpdate_=update;return held_;   // noexcept commit
    } catch(...) {close_held();return nullptr;}
}
bool FileIO::create_directory(const std::filesystem::path& p) {
    close_held();
    std::error_code ec;std::filesystem::create_directories(p,ec);return !ec&&std::filesystem::is_directory(p,ec)&&!ec;
}
bool FileIO::size(const std::filesystem::path& p,uint64_t& value) {
    close_held(); // stat a closed file, exactly as before
    std::error_code ec;bool exists=std::filesystem::exists(p,ec);if(ec)return false;
    if(!exists){value=0;return true;}auto n=std::filesystem::file_size(p,ec);if(ec)return false;value=n;return true;
}
size_t FileIO::append(const std::filesystem::path& p,const char* bytes,size_t n) {
    if(auto* f=held(p,false)) {
        // Single writer: end-of-file positioning == "ab" (a no-op for an "ab" handle). The per-call fflush keeps the write result per call.
        if(_fseeki64(f,0,SEEK_END)!=0){close_held();batchOk_=false;return 0;}
        auto written=std::fwrite(bytes,1,n,f);
        if(std::fflush(f)!=0){close_held();batchOk_=false;return 0;}
        return written;
    }
    close_held();
    FILE* f=open_shared(p,L"ab");if(!f)return 0;
    ++opens;
    auto written=std::fwrite(bytes,1,n,f);
    bool ok=std::fflush(f)==0;if(std::fclose(f)!=0)ok=false;++closes;
    return ok?written:0;
}
size_t FileIO::overwrite(const std::filesystem::path& p,uint64_t offset,const char* bytes,size_t n) {
    if(n!=max_record_length||offset>max_evidence_size-n)return 0;
    FILE* f=held(p,true);const bool session=f!=nullptr;
    if(!session) {close_held();f=open_shared(p,L"r+b");if(!f)return 0;++opens;++updateOpens;}
    size_t written=0;
    if(_fseeki64(f,0,SEEK_END)==0) {
        auto end=_ftelli64(f);
        if(end>=0&&offset<=uint64_t(end)&&n<=uint64_t(end)-offset&&
           _fseeki64(f,static_cast<__int64>(offset),SEEK_SET)==0)written=std::fwrite(bytes,1,n,f);
    }
    if(session) {
        if(std::fflush(f)!=0){close_held();batchOk_=false;return 0;}
        return written;
    }
    bool ok=std::fflush(f)==0;if(std::fclose(f)!=0)ok=false;++closes;return ok?written:0;
}
bool FileIO::reset(const std::filesystem::path& p) {
    close_held();
    FILE* f=open_shared(p,L"wb");if(!f)return false;
    return std::fclose(f)==0;
}
bool FileIO::rotate(const std::filesystem::path& p,size_t count) {
    close_held(); // never rename an open file
    if(count>max_rotated_files||count==0)return false;
    std::error_code ec;
    std::filesystem::remove(rotated(p,count),ec);if(ec)return false;
    for(size_t i=count;i>0;--i) {
        auto from=i==1?p:rotated(p,i-1),to=rotated(p,i);
        bool exists=std::filesystem::exists(from,ec);if(ec)return false;
        if(exists){std::filesystem::rename(from,to,ec);if(ec)return false;}
    }return true;
}
ReadResult FileIO::read_config(const std::filesystem::path& p,char* out,size_t cap) {
    close_held();
    uint64_t n=0;std::error_code ec;bool exists=std::filesystem::exists(p,ec);
    if(ec)return {ReadState::unreadable,0};if(!exists)return {};
    if(!size(p,n))return {ReadState::unreadable,0};if(n>cap)return {ReadState::oversized,0};
    FILE* f=nullptr;if(_wfopen_s(&f,p.c_str(),L"rb")||!f)return {ReadState::unreadable,0};
    auto got=std::fread(out,1,cap,f);int extra=std::fgetc(f);bool ok=!std::ferror(f);
    if(std::fclose(f)!=0)ok=false;
    if(!ok||got!=n)return {ReadState::unreadable,0};
    return extra==EOF?ReadResult{ReadState::ok,got}:ReadResult{ReadState::oversized,0};
}
void IntervalCounter::observe(uint64_t now) noexcept {
    if(!seen_){seen_=true;last_=now;return;}
    if(now<last_)return; // Regressing injected clocks never create huge intervals.
    auto dt=std::min(now-last_,max_interval_us);last_=now;
    if(value_.count==max_interval_samples)return;
    if(!value_.count)value_.min=dt;else value_.min=std::min(value_.min,dt);
    value_.max=std::max(value_.max,dt);++value_.count;sum_+=dt;
    ++value_.buckets[dt<1000?0:dt<10000?1:dt<100000?2:3];
}
IntervalSummary IntervalCounter::summary() const noexcept {
    auto out=value_;out.mean=out.count?sum_/out.count:0;return out;
}
// Append-only MPSC reservations: one ticket owns one slot for the entire
// session. Release publication separates reservation from initialization; a
// consumer stops at an unpublished slot and resumes on a later due flush.
// Unlike the ordinary try-lock ring, contention cannot discard identities.
// The full flag stops further tickets once the last slot has been reserved.
struct Log::Reserved {
    template<size_t N> struct Partition {
        struct Slot {Record value;std::atomic<bool> ready{false};};
        std::array<Slot,N> slots{};
        std::atomic<size_t> next{0};std::atomic<bool> full{false};size_t consumed=0;
        bool push(const Record& r) noexcept {
            if(full.load(std::memory_order_relaxed))return false;
            const auto i=next.fetch_add(1,std::memory_order_relaxed);
            if(i>=N-1)full.store(true,std::memory_order_relaxed);
            if(i>=N)return false;
            slots[i].value=r;slots[i].ready.store(true,std::memory_order_release);return true;
        }
        void drain(Log& log) {
            while(consumed<N&&slots[consumed].ready.load(std::memory_order_acquire))
                log.write_evidence(slots[consumed++].value);
        }
    };
    struct Reason {
        // SC field operations make the generation check a consistent snapshot
        // in the C++ memory model, including on weakly ordered machines.
        std::atomic<uint64_t> generation{0},first{0},last{0},count{0},kind{0};
        std::atomic<const char*> text{nullptr};
        uint64_t offset=UINT64_MAX,persisted=0; // Consumer-only.
    };
    std::array<Reason,reason_records> reasons{};
    struct Outcome {
        // First example is immutable after generation's first release/SC
        // publication. Only SC counters change; consumer never locks producer.
        Record example;
        const char* key=nullptr;uint32_t cb10=UINT32_MAX; // serialized producer only
        std::atomic<uint64_t> generation{0},last{0},count{0};
        uint64_t offset=UINT64_MAX,persisted=0; // sole consumer only
    };
    std::array<Outcome,outcome_records+list_path_records+list_refusal_records> outcomes{};
    size_t outcomes_used=0;
    // Producer-only key table, serialized by the existing capture-state mutex.
    std::array<const char*,reason_capacity> keys{};
    size_t used=0;
    struct Latest {
        std::atomic<uint64_t> generation{0},sequence{0},a{0},b{0},c{0};
        std::array<std::atomic<char>,text_capacity> text{};
        uint64_t offset=UINT64_MAX,persisted=0; // consumer only
    };
    struct Raw {std::array<unsigned char,CpuUploadSnapshot::file_cap> bytes{};size_t size=0;std::atomic<bool> ready{false};};
    std::array<Raw,CpuUploadSnapshot::limit> raw{};
    std::atomic<size_t> raw_count{0};
    std::array<Latest,3> l3_counts{};
    Partition<pso_records> pso;Partition<root_records> root;Partition<startup_records> startup;Partition<9> l3_first;
    Partition<step1_records> step1;
};
namespace {
bool startup(Event e) noexcept {
    return e==Event::proxy_load||e==Event::device||e==Event::hook_pin||e==Event::hook_patch||
        e==Event::capture_decision||e==Event::config||e==Event::missing_present;
}
bool aggregated(Event e) noexcept {return e==Event::descriptor||e==Event::metadata_disabled;}
bool step1_evidence(const Record& r) noexcept {
    // J-3b6: step-2 lines share the partition (same 15-character prefix width).
    return r.kind==Event::requirements_metadata&&(std::strncmp(r.text.data(),"gpu-copy-step1;",15)==0||
        std::strncmp(r.text.data(),"gpu-copy-step2;",15)==0||std::strncmp(r.text.data(),"gpu-copy-step3;",15)==0);
}
size_t companion_kind(const Record& r) noexcept {
    if(r.kind!=Event::requirements_metadata)return companion_kind_count;
    constexpr const char* prefixes[]={"root-constant-witness;","root-table-witness;",
        "t0-resource-description;","t0-write-witness;","s0-table-witness;","s0-description;",
        "l3-t10;","l3-vb;","l3-ib;","l3-state;","l3-link;","l3-counts;","l3-repeats;","cpu-t10;","cpu-vb;","cpu-ib;","cpu-snapshot;","srv-slot-counts;"};
    static_assert(std::size(prefixes)==companion_kind_count);
    for(size_t i=0;i<std::size(prefixes);++i)
        if(std::strncmp(r.text.data(),prefixes[i],std::strlen(prefixes[i]))==0)return i;
    return companion_kind_count;
}
}
Log::Log(PathProvider& paths,IOProvider& io,Clock clock,bool production) noexcept:
    paths_(paths),io_(io),clock_(clock?clock:default_clock),production_(production) {
    static_assert(sizeof(Reserved)<=3*1024*1024);
    if(production_)try {reserved_=std::make_unique<Reserved>();}catch(...){fail();}
}
Log::~Log()=default;
void Log::drop(Event e,uint64_t count) noexcept {
    dropped_.fetch_add(count,std::memory_order_relaxed);
    auto i=static_cast<size_t>(e);if(i<drops_.size())drops_[i].fetch_add(count,std::memory_order_relaxed);
}
bool Log::push(Record r,unsigned verbosity) noexcept {
    if(!enabled_.load(std::memory_order_relaxed)||verbosity>verbosity_.load(std::memory_order_relaxed))return false;
    perf::Scope cost(perf::log); // FPS investigation (observe only)
    const auto companion=production_?companion_kind(r):companion_kind_count;
    if(companion<companion_kind_count&&!ownership_pins())return false;
    const auto kind=static_cast<size_t>(r.kind);
    // Exactly one relaxed atomic update for production high-volume events.
    // No formatting, allocation, ring access, lock or I/O; raw payloads are
    // intentionally absent. Hook-patch rechecks are already change-rate-limited
    // by their emitting seam, and their initial outcomes use the startup quota.
    if(production_&&aggregated(r.kind)){events_[kind].fetch_add(1,std::memory_order_relaxed);return true;}
    if(kind<events_.size())events_[kind].fetch_add(1,std::memory_order_relaxed);
    if(production_&&step1_evidence(r)) {
        // Experiment-only reserved partition; never produced while OFF.
        if(!gpu_copy_step1_experiment()&&!gpu_copy_step2_experiment()&&!gpu_copy_step3_experiment()){drop(r.kind);return false;}
        r.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
        if(!format_record(r).valid){drop(r.kind);return false;}
        if(!reserved_->step1.push(r)){overflow_[kind].fetch_add(1,std::memory_order_relaxed);drop(r.kind);return false;}
        return true;
    }
    if(production_&&(r.kind==Event::pso||r.kind==Event::root||startup(r.kind))) {
        r.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
        if(!format_record(r).valid){drop(r.kind);return false;}
        const bool ok=r.kind==Event::pso?reserved_->pso.push(r):r.kind==Event::root?
            reserved_->root.push(r):reserved_->startup.push(r);
        if(!ok){overflow_[kind].fetch_add(1,std::memory_order_relaxed);drop(r.kind);return false;}
        return true;
    }
    // Validate/sanitize/format outside the ring flag. The sequence contributes
    // only its decimal digit count, checked after assigning it under the flag.
    r.sequence=0;const auto formatted=format_record(r);
    if(!formatted.valid){drop(r.kind);return false;}
    if(ring_lock_.test_and_set(std::memory_order_acquire)){drop(r.kind);return false;}
    FlagGuard guard{ring_lock_};
    if(companion==11||companion==12||companion==17) {
        // Independent latest counter slot; ordinary sample caps, startup
        // exhaustion and ring pressure cannot hide the final observed totals.
        auto& v=reserved_->l3_counts[companion==17?2:companion-11];const auto gen=v.generation.load();
        v.generation.store(gen+1);v.sequence.store(sequence_.fetch_add(1)+1);
        v.a.store(r.a);v.b.store(r.b);v.c.store(r.c);
        for(size_t i=0;i<text_capacity;++i)v.text[i].store(r.text[i]);
        v.generation.store(gen+2);return true;
    }
    if(companion<companion_kind_count) {
        auto& samples=companion_samples_[companion];
        if(samples>=companion_sample_limit){drop(r.kind);return false;}
        if(!samples) {
            // The raw emit path never uses the immutable outcome table. Retain
            // its first example in the EXISTING startup reservation instead of
            // losing it when only the evidence file is collected/ordinary logs
            // rotate. Startup exhaustion still drops it; no quota is enlarged.
            r.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
            if(!format_record(r).valid){drop(r.kind);return false;}
            const bool stored=companion>=6?reserved_->l3_first.push(r):reserved_->startup.push(r);
            if(!stored){drop(r.kind);return false;}
            ++samples;return true;
        }
    }
    if(count_==ring_capacity||sequence_.load()==UINT64_MAX){drop(r.kind);return false;}
    r.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    size_t extra=0;for(auto n=r.sequence;n>=10;n/=10)++extra;
    if(extra>max_record_length-formatted.size){drop(r.kind);return false;}
    ring_[(head_+count_)%ring_capacity]=r;++count_;
    if(companion<companion_kind_count)++companion_samples_[companion];
    return true;
}
bool Log::raw_snapshot(const unsigned char* bytes,size_t n) noexcept {
    if(!ownership_pins()||!reserved_||!bytes||n<CpuUploadSnapshot::header_size||n>CpuUploadSnapshot::file_cap)return false;
    auto slot=reserved_->raw_count.load();
    if(slot>=CpuUploadSnapshot::limit||!reserved_->raw_count.compare_exchange_strong(slot,slot+1))return false;
    auto& r=reserved_->raw[slot];std::memcpy(r.bytes.data(),bytes,n);r.size=n;r.ready.store(true,std::memory_order_release);return true;
}
void Log::drain_raw_snapshots() {
    // Sole consumer, outside every hook mutex. Never derive paths from producer data.
    while(raw_drained_<CpuUploadSnapshot::limit) {
        auto& r=reserved_->raw[raw_drained_];if(!r.ready.load(std::memory_order_acquire))break;
        const auto path=snapshot_directory_/(L"diagnostics.cpu-snapshot-"+std::to_wstring(raw_drained_)+L".bin");
        ++raw_drained_;
        uint64_t n=0;
        if(!safe_path(path,snapshot_game_)||normalized_path(path).parent_path()!=normalized_path(snapshot_directory_)||!io_.size(path,n)||n>CpuUploadSnapshot::file_cap||!io_.reset(path)||
           io_.append(path,reinterpret_cast<const char*>(r.bytes.data()),r.size)!=r.size){fail();return;}
    }
}
void Log::drain_l3_counts() {
    for(auto& v:reserved_->l3_counts) {
    const auto gen=v.generation.load();
    if(!gen||(gen&1)||gen==v.persisted||failed_)continue;
    auto r=record(Event::requirements_metadata,v.a.load(),v.b.load(),v.c.load());r.sequence=v.sequence.load();
    for(size_t i=0;i<text_capacity;++i)r.text[i]=v.text[i].load();
    if(gen!=v.generation.load())continue;
    auto line=format_record(r);if(!line.valid){fail();return;}
    std::fill(line.bytes.begin()+line.size-1,line.bytes.begin()+max_record_length-1,' ');line.bytes[max_record_length-1]='\n';
    if(v.offset==UINT64_MAX) {
        if(evidence_bytes_>max_evidence_size-max_record_length||evidence_written_>=reserved_records){fail();return;}
        v.offset=evidence_bytes_;
        if(io_.append(evidence_file_,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
        evidence_bytes_+=max_record_length;++evidence_written_;++written_;
    } else if(io_.overwrite(evidence_file_,v.offset,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
    v.persisted=gen;
    }
}
void Log::observer_disabled(const char* reason,DisableEvent kind) noexcept {
    assert(mutex_depth && "observer_disabled requires capture-state mutex");
#ifdef ACZ_DIAGNOSTICS_OFFLINE_TEST
    // Release fixtures retain this precondition check even with NDEBUG.
    if(!mutex_depth)std::abort();
#endif
    if(!enabled()||!reason||verbosity_.load(std::memory_order_relaxed)<1)return;
    if(!production_){push(record(Event::metadata_disabled,0,0,static_cast<unsigned>(kind),reason));return;}
    events_[static_cast<size_t>(Event::metadata_disabled)].fetch_add(1,std::memory_order_relaxed);
    auto& table=*reserved_;size_t index=0;
    for(;index<table.used;++index)if(std::strncmp(table.keys[index],reason,128)==0)break;
    if(index==table.used) {
        if(table.used<reason_capacity&&strnlen_s(reason,128)<128)
            table.keys[table.used++]=reason;
        else index=reason_capacity;
    }
    auto& r=table.reasons[index];
    const auto seq=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    const auto count=r.count.load();
    const auto generation=r.generation.load();r.generation.store(generation+1);
    if(!count){r.first.store(seq);r.text.store(index==reason_capacity?"observer reason overflow":reason);r.kind.store(static_cast<unsigned>(kind));}
    if(!r.kind.load()&&kind!=DisableEvent::unknown)r.kind.store(static_cast<unsigned>(kind));
    r.last.store(seq);r.count.store(count+1);
    r.generation.store(generation+2);
    if(index==reason_capacity)overflow_[static_cast<size_t>(Event::metadata_disabled)].fetch_add(1,std::memory_order_relaxed);
}
void Log::drain_reasons() {
    if(failed_)return;
    // Consumer never waits for a producer or takes its capture-state mutex.
    // Fixed-size overwrite keeps exactly one on-disk record per distinct text.
    for(auto& r:reserved_->reasons) {
        if(failed_)return;
        const auto version=r.generation.load();if((version&1)||version==r.persisted)continue;
        auto value=record(Event::metadata_disabled,r.last.load(),r.count.load(),r.kind.load(),r.text.load());
        value.sequence=r.first.load();
        if(version!=r.generation.load()||!value.b)continue;
        auto line=format_record(value);if(!line.valid){fail();return;}
        // Pad BEFORE newline: one parseable line, exactly one reserved slot.
        std::fill(line.bytes.begin()+line.size-1,line.bytes.begin()+max_record_length-1,' ');
        line.bytes[max_record_length-1]='\n';
        if(r.offset==UINT64_MAX) {
            if(evidence_bytes_>max_evidence_size-max_record_length||evidence_written_>=reserved_records){fail();return;}
            r.offset=evidence_bytes_;
            if(io_.append(evidence_file_,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
            evidence_bytes_+=max_record_length;++evidence_written_;++written_;
        } else if(io_.overwrite(evidence_file_,r.offset,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
        r.persisted=version;
    }
}
void Log::list_registered(unsigned path) noexcept {
    assert(mutex_depth && "list_registered requires capture-state mutex");
    static constexpr const char* paths[]={"CreateCommandList","CreateCommandList1","registered-at-reset"};
    static_assert(std::size(paths)==list_path_records);
    if(!enabled()||path>=list_path_records||verbosity_.load(std::memory_order_relaxed)<1)return;
    if(!production_){push(record(Event::device,0,1,path,paths[path]));return;}
    events_[static_cast<size_t>(Event::device)].fetch_add(1,std::memory_order_relaxed);
    auto& item=reserved_->outcomes[outcome_records+path];
    const auto generation=item.generation.load();item.generation.store(generation+1);
    const auto seq=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    const auto count=item.count.load();
    if(!count){item.example=record(Event::device,0,0,path,paths[path]);item.example.sequence=seq;}
    item.last.store(seq);item.count.store(count==UINT64_MAX?count:count+1);
    item.generation.store(generation+2);
}
void Log::list_refused(ListRefusal reason,ListSeam seam,unsigned type) noexcept {
    assert(mutex_depth && "list_refused requires capture-state mutex");
    const auto r=static_cast<size_t>(reason);const auto p=static_cast<unsigned>(seam);
    if(!enabled()||r>=list_refusal_count||p>=3||verbosity_.load(std::memory_order_relaxed)<1)return;
    const auto example=[&] {
        static constexpr const char* seams[]={"creation","Reset","ExecuteCommandLists"};
        auto out=record(Event::device,0,1,p);
        std::snprintf(out.text.data(),out.text.size(),"%s;seam=%s;type=%s",list_refusal_reasons[r],seams[p],
            type==0?"DIRECT":type==1?"BUNDLE":type==UINT_MAX?"unobserved":"other");
        return out;
    };
    if(!production_){push(example());return;}
    events_[static_cast<size_t>(Event::device)].fetch_add(1,std::memory_order_relaxed);
    auto& item=reserved_->outcomes[outcome_records+list_path_records+r*3+p];
    const auto generation=item.generation.load();item.generation.store(generation+1);
    const auto seq=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    const auto count=item.count.load();
    if(!count){item.example=example();item.example.sequence=seq;}
    item.last.store(seq);item.count.store(count==UINT64_MAX?count:count+1);
    item.generation.store(generation+2);
}
void Log::metadata_outcome(const requirements::MetadataResult& value,const capture::Command& cmd,unsigned list_path,const char* context) noexcept {
    assert(mutex_depth && "metadata_outcome requires capture-state mutex");
    if(!enabled()||!value.outcome||verbosity_.load(std::memory_order_relaxed)<1)return;
    // Standalone Log fixtures retain their ordinary policy. The DLL always
    // activates production mode, with a dedicated priority partition.
    if(!production_){auto example=record(Event::requirements_metadata,0,1,value.cb10_kind);
        std::snprintf(example.text.data(),example.text.size(),"%s;cb10=%u;draw=%u,%u,%u,%u,%d,%u",
            value.outcome,value.cb10_kind,unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        auto used=std::strlen(example.text.data());
        std::snprintf(example.text.data()+used,example.text.size()-used,";lp=%u%s",std::min(list_path,3u),context);
        push(example);return;}
    events_[static_cast<size_t>(Event::requirements_metadata)].fetch_add(1,std::memory_order_relaxed);
    auto& table=*reserved_;size_t index=0;
    for(;index<table.outcomes_used;++index) {
        const auto& item=table.outcomes[index];
        if(item.cb10==value.cb10_kind&&std::strcmp(item.key,value.outcome)==0)break;
    }
    if(index==table.outcomes_used) {
        if(table.outcomes_used<outcome_capacity)++table.outcomes_used;else index=outcome_capacity;
    }
    auto& item=table.outcomes[index];
    const auto generation=item.generation.load();item.generation.store(generation+1);
    const auto seq=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    const auto count=item.count.load();
    if(!count) {
        item.key=value.outcome;item.cb10=value.cb10_kind;
        auto first=record(Event::requirements_metadata,0,0,value.cb10_kind);
        first.sequence=seq;
        // Worst case: 20 digits per uint64, 10 per uint32, signed base 11;
        // success <=358 chars including NUL with ;lp=N; rejection <=225.
        // Only explicit-lifetime rejections receive three 17-character masks.
        static_assert(text_capacity>=358&&max_record_length>=358+139);
        static_assert(text_capacity>=225+3*17&&max_record_length>=225+3*17+139);
        if(index==outcome_capacity) {
            first.c=0;std::snprintf(first.text.data(),first.text.size(),
                "metadata outcome overflow;cb10=%u;draw=%u,%u,%u,%u,%d,%u",value.cb10_kind,
                unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        }
        else if(value.proved)std::snprintf(first.text.data(),first.text.size(),
            "%s;t10=0;r=0:176,192:304,352:368,400:416;span=%llu:%llu;first=%llu;stride=%llu;cb10=%u;ia=%u:%llu:%llu;draw=%u,%u,%u,%u,%d,%u;bytes=%s",
            value.outcome,value.srt_begin,value.srt_end,value.first_element,value.srt_stride,value.cb10_kind,
            value.ia_stride,value.vertex_begin,value.vertex_end,unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance,
            value.index_bytes_verified?"index-verified;sampled-diagnostic;draw-use-unproven;gpu-completion-unproven":"not-evaluated");
        else std::snprintf(first.text.data(),first.text.size(),"%s;cb10=%u;draw=%u,%u,%u,%u,%d,%u",
            value.outcome,value.cb10_kind,unsigned(cmd.indexed),cmd.count,cmd.instances,cmd.start,cmd.base_vertex,cmd.start_instance);
        auto used=std::strlen(first.text.data());
        std::snprintf(first.text.data()+used,first.text.size()-used,";lp=%u%s",std::min(list_path,3u),context);
        first.sequence=seq;item.example=first;
    }
    item.last.store(seq);item.count.store(count==UINT64_MAX?count:count+1);
    item.generation.store(generation+2);
    if(index==outcome_capacity)overflow_[static_cast<size_t>(Event::requirements_metadata)].fetch_add(1,std::memory_order_relaxed);
}
void Log::drain_outcomes() {
    for(auto& item:reserved_->outcomes) {
        if(failed_)return;
        const auto version=item.generation.load();
        if(!version||(version&1)||version==item.persisted)continue;
        auto value=item.example;value.a=item.last.load();value.b=item.count.load();
        if(version!=item.generation.load())continue;
        auto line=format_record(value);if(!line.valid){fail();return;}
        std::fill(line.bytes.begin()+line.size-1,line.bytes.begin()+max_record_length-1,' ');
        line.bytes[max_record_length-1]='\n';
        if(item.offset==UINT64_MAX) {
            if(evidence_bytes_>max_evidence_size-max_record_length||evidence_written_>=reserved_records){fail();return;}
            item.offset=evidence_bytes_;
            if(io_.append(evidence_file_,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
            evidence_bytes_+=max_record_length;++evidence_written_;++written_;
        } else if(io_.overwrite(evidence_file_,item.offset,line.bytes.data(),max_record_length)!=max_record_length){fail();return;}
        item.persisted=version;
    }
}
bool Log::pop(Record& out) noexcept {
    if(ring_lock_.test_and_set(std::memory_order_acquire))return false;
    FlagGuard guard{ring_lock_};if(!count_)return false;
    out=ring_[head_];head_=(head_+1)%ring_capacity;--count_;return true;
}
void Log::execute_interval() noexcept {
    if(!enabled_.load(std::memory_order_relaxed))return;
    // This kind accounts for both submission observations and their consumer
    // summary records; timing fields below remain explicitly submission-only.
    events_[static_cast<size_t>(Event::execute_summary)].fetch_add(1,std::memory_order_relaxed);
    submitted_.store(true,std::memory_order_relaxed);
    const auto now=clock_();
    if(ring_lock_.test_and_set(std::memory_order_acquire)){drop(Event::execute_summary);return;}
    FlagGuard guard{ring_lock_};intervals_.observe(now);
}
void Log::fail() noexcept {failed_=true;enabled_.store(false,std::memory_order_relaxed);++io_failures_;}
void Log::write(const Record& r) {
    if(failed_)return;
    if(!production_&&written_.load()==max_records){drop(r.kind);return;}
    auto line=format_record(r);if(!line.valid){drop(r.kind);return;}
    if(production_) {
        const auto limit=max_records-reserved_records;
        if(ordinary_written_>=limit||batch_records_>=limit-ordinary_written_) {drop(r.kind);return;}
        if(line.size>batch_.size()-batch_size_)append_batch();
        if(failed_)return;
        std::copy_n(line.bytes.data(),line.size,batch_.data()+batch_size_);
        batch_size_+=line.size;++batch_records_;return;
    }
    uint64_t size=0;
    if(!io_.size(file_,size)||size>max_file_size){fail();return;}
    // Subtraction after checked size avoids unchecked size+record overflow.
    if(line.size>max_file_size-size) {
        // U-soak: a failed rename (a reader without FILE_SHARE_DELETE) is transient: drop this record, retry on the next one.
        if(!io_.rotate(file_,max_rotated_files)){++rotation_failures_;drop(r.kind);return;}++rotations_;
        if(!io_.size(file_,size)||size!=0){fail();return;}
    }
    if(io_.append(file_,line.bytes.data(),line.size)!=line.size){fail();return;}
    ++written_;
}
void Log::write_evidence(const Record& r) {
    if(failed_)return;
    auto line=format_record(r);
    if(!line.valid){drop(r.kind);return;}
    if(evidence_written_>=reserved_records||batch_records_>=reserved_records-evidence_written_||
       evidence_bytes_>max_evidence_size||batch_size_>max_evidence_size-evidence_bytes_||
       line.size>max_evidence_size-evidence_bytes_-batch_size_){drop(r.kind);return;}
    if(line.size>batch_.size()-batch_size_)append_batch();
    if(failed_)return;
    std::copy_n(line.bytes.data(),line.size,batch_.data()+batch_size_);
    batch_size_+=line.size;++batch_records_;
}
void Log::drain_reserved() {
    evidence_batch_=true;
    reserved_->startup.drain(*this);reserved_->pso.drain(*this);reserved_->root.drain(*this);reserved_->l3_first.drain(*this);
    reserved_->step1.drain(*this);
    append_batch();evidence_batch_=false;
}
void Log::append_batch() {
    if(failed_||!batch_size_)return;
    const auto& target=evidence_batch_?evidence_file_:file_;
    const auto limit=evidence_batch_?max_evidence_size:max_file_size;
    uint64_t size=0;
    if(!io_.size(target,size)||size>limit){fail();return;}
    if(batch_size_>limit-size) {
        if(evidence_batch_){fail();return;}
        // U-soak: transient (see write()): the batch is dropped and counted, the file never exceeds its cap, the next flush retries.
        if(!io_.rotate(file_,max_rotated_files)) {
            ++rotation_failures_;dropped_.fetch_add(batch_records_,std::memory_order_relaxed);batch_size_=batch_records_=0;return;
        }
        ++rotations_;
        if(!io_.size(file_,size)||size!=0){fail();return;}
    }
    if(io_.append(target,batch_.data(),batch_size_)!=batch_size_){fail();return;}
    written_+=batch_records_;
    if(evidence_batch_){evidence_written_+=batch_records_;evidence_bytes_+=batch_size_;}
    else ordinary_written_+=batch_records_;
    batch_size_=batch_records_=0;
}
void Log::final_summary() noexcept {
    // Caller owns the activation gate. This is an object-lifetime checkpoint,
    // not evidence that the process is ending. No pre-submission consumption.
    if(!final_&&has_submissions()){final_=true;checkpoint_pending_=true;}
}
void Log::flush() noexcept {
    if(mutex_depth)return;
    if(flush_lock_.test_and_set(std::memory_order_acquire))return;
    FlagGuard flushGuard{flush_lock_};
    if(failed_)return;
    // Section 8: one write session per flush (consecutive evidence-slot rewrites share one open file). Runs on every exit path; a
    // deferred close failure fails the log exactly like a failed per-call close.
    io_.begin_batch();
    struct BatchEnd {Log& log;~BatchEnd(){if(!log.io_.end_batch())log.fail();}} batchEnd{*this};
    try {
        batch_size_=batch_records_=0;
        if(!initialized_) {
            initialized_=true;auto paths=paths_.paths();
            // Also reject config reads inside the game directory. Validate
            // every existing rotated file to reject redirected output files.
            auto file=paths.logs/L"diagnostics.log",evidence=paths.logs/L"diagnostics.evidence.log";
            if(!safe_path(paths.logs,paths.game)||!safe_path(paths.config,paths.game)||!safe_path(file,paths.game)||
               (production_&&!safe_path(evidence,paths.game))) {fail();return;}
            for(size_t i=1;i<=max_rotated_files;++i)if(!safe_path(rotated(file,i),paths.game)){fail();return;}
            auto config=load_config(io_,paths.config);
            // The standalone library preserves its explicit-flush API. The DLL
            // publishes only a valid, explicitly enabled production Log.
            const bool opt_in=!production_||(config.issue==ConfigIssue::none&&config.config.logging_explicit());
            if(config.issue!=ConfigIssue::none&&config.issue!=ConfigIssue::missing)++config_failures_;
            unknown_keys_+=config.unknown_keys;
            // Count/reason precede applying an explicit log disable. The queued
            // config record stays bounded and undrained when logging is disabled.
            // Ownership diagnostics never change capture state or runtime callbacks.
            push(record(Event::config,static_cast<unsigned>(config.issue),config.unknown_keys,0,config.reason));
            // Default policy description, once per log, on the explicit consumer
            // path only. This does not wire capture enablement into draw hooks.
            const auto policy=capture_enablement::decide({});
            push(record(Event::capture_decision,policy.enabled,0,0,policy.reason));
            enabled_.store(opt_in&&config.config.logging(),std::memory_order_relaxed);
            verbosity_.store(config.config.verbosity(),std::memory_order_relaxed);
            ownership_pins_.store(opt_in&&config.config.logging()&&config.config.ownership_pins(),std::memory_order_relaxed);
            gpu_copy_witness_observation_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_witness_observation(),std::memory_order_relaxed);
            gpu_copy_step1_experiment_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step1_experiment(),std::memory_order_relaxed);
            gpu_copy_early_device_hook_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_early_device_hook(),std::memory_order_relaxed);
            gpu_copy_step2_experiment_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step2_experiment(),std::memory_order_relaxed);
            gpu_copy_step3_experiment_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step3_experiment(),std::memory_order_relaxed);
            gpu_copy_step3_whole_set_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step3_whole_set(),std::memory_order_relaxed);
            gpu_copy_step5_hud_observe_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_hud_observe(),std::memory_order_relaxed);
            gpu_copy_step5_hud_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_hud(),std::memory_order_relaxed);
            gpu_copy_step5_continuous_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous(),std::memory_order_relaxed);
            gpu_copy_step5_hmd_fov_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&config.config.gpu_copy_step5_hmd_fov(),std::memory_order_relaxed);
            gpu_copy_step5_units_per_metre_.store(config.config.gpu_copy_step5_units_per_metre(),std::memory_order_relaxed);
            gpu_copy_step5_precockpit_observe_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_precockpit_observe(),std::memory_order_relaxed);
            gpu_copy_step5_members_by_class_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_members_by_class(),std::memory_order_relaxed);
            gpu_copy_step5_world_objects_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&
                config.config.gpu_copy_step5_members_by_class()&&config.config.gpu_copy_step5_world_objects(),std::memory_order_relaxed);
            gpu_copy_step5_world_observe_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&
                config.config.gpu_copy_step5_world_observe(),std::memory_order_relaxed);
            gpu_copy_step5_phase_observe_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&
                config.config.gpu_copy_step5_phase_observe(),std::memory_order_relaxed);
            gpu_copy_step5_png_once_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&config.config.gpu_copy_step5_png_once(),std::memory_order_relaxed);
            xr_stereo_output_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output(),std::memory_order_relaxed);
            xr_hud_layer_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()&&config.config.xr_hud_layer(),std::memory_order_relaxed);
            hud_mode_.store(config.config.hud_mode(),std::memory_order_relaxed);
            {   // J-feather: values reach the consumer only with xr_stereo_output latched.
                const bool xr=opt_in&&config.config.logging()&&config.config.xr_stereo_output();
                xr_edge_feather_.store(xr?config.config.xr_edge_feather():0.0f,std::memory_order_relaxed);
                xr_world_quad_distance_m_.store(xr?config.config.xr_world_quad_distance_m():40.0f,std::memory_order_relaxed);
                xr_comfort_configured_.store(xr&&(config.config.xr_edge_feather_set()||config.config.xr_world_quad_distance_set()),std::memory_order_relaxed);
                // J-hud-placement: only with the HUD layer itself latched.
                const bool hud=xr&&config.config.xr_hud_layer();
                xr_hud_quad_distance_m_.store(hud&&config.config.xr_hud_quad_distance_set()?config.config.xr_hud_quad_distance_m():0.0f,std::memory_order_relaxed);
                xr_hud_scale_.store(hud?config.config.xr_hud_scale():1.0f,std::memory_order_relaxed);
                xr_hud_head_distance_m_.store(hud?config.config.xr_hud_head_distance_m():2.5f,std::memory_order_relaxed);
                xr_hud_head_width_m_.store(hud?config.config.xr_hud_head_width_m():1.6f,std::memory_order_relaxed);
                xr_hud_placement_configured_.store(hud&&config.config.xr_hud_placement_set(),std::memory_order_relaxed);
                xr_world_in_projection_.store(xr&&config.config.xr_world_in_projection(),std::memory_order_relaxed); // J-world-in-projection
                xr_world_hold_last_.store(xr&&config.config.xr_world_in_projection()&&config.config.xr_world_hold_last(),std::memory_order_relaxed); // J-world-hold
                xr_world_extend_.store(xr&&config.config.xr_world_in_projection()&&config.config.xr_world_extend(),std::memory_order_relaxed); // J-world-extend
                xr_world_cache_.store(xr&&config.config.xr_world_in_projection()&&config.config.xr_world_cache(),std::memory_order_relaxed); // J-world-cache
                {   // J-terrain-render: needs the composite and a stage string
                    const char* st=config.config.xr_terrain_stage();
                    const bool on=xr&&config.config.xr_world_in_projection()&&config.config.xr_terrain_render()&&st&&st[0];
                    for(size_t i=0;i<sizeof xr_terrain_stage_;++i)xr_terrain_stage_[i]=on?st[i]:0;
                    xr_terrain_render_.store(on,std::memory_order_relaxed);
                    xr_terrain_preset_vrn_.store(on&&config.config.xr_terrain_preset_vrn(),std::memory_order_relaxed);
                }
                {   // COCKPIT-2: needs the composite; a non-empty value enables the model cockpit
                    const char* cm=config.config.xr_cockpit_model();
                    const bool on=xr&&config.config.xr_world_in_projection()&&cm&&cm[0];
                    for(size_t i=0;i<sizeof xr_cockpit_model_;++i)xr_cockpit_model_[i]=on?cm[i]:0;
                }
            }
            ee_camera_probe_.store(opt_in&&config.config.logging()&&config.config.ee_camera_probe(),std::memory_order_relaxed);
            ee_camera_probe_position_.store(opt_in&&config.config.logging()&&config.config.ee_camera_probe_position(),std::memory_order_relaxed);
            ee_camera_write_trials_.store(opt_in&&config.config.logging()&&config.config.ee_camera_write_trials(),std::memory_order_relaxed);
            ee_camera_probe_points_.store(config.config.ee_camera_probe_points(),std::memory_order_relaxed);
            // J-hud-markers: only together with the key each one extends (a lone key stays inert).
            gpu_copy_step5_hud_markers_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_hud()&&config.config.gpu_copy_step5_hud_markers(),std::memory_order_relaxed);
            xr_hud_markers_layer_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()&&config.config.xr_hud_layer()&&config.config.xr_hud_markers_layer(),std::memory_order_relaxed);
            {   // Option 3 stage 2: only with the probe itself latched; the drive additionally needs the trial key.
                const bool lw=opt_in&&config.config.logging()&&config.config.ee_camera_probe()&&config.config.ee_camera_look_write_trial();
                ee_camera_look_write_trial_.store(lw,std::memory_order_relaxed);
                ee_camera_look_drive_.store(lw&&config.config.ee_camera_look_drive(),std::memory_order_relaxed);
            }
            {   // Option 3 via keyboard input: the head-driven mode needs the consumer AND the continuous producer; the sweep needs the key and
                // alone (without xr_stereo_output) is the no-XR test sweep (Config::look_drive_latch).
                const auto ld=config.config.look_drive_latch(opt_in);
                xr_look_drive_keys_.store(ld.keys,std::memory_order_relaxed);
                xr_look_drive_test_sweep_.store(ld.sweep,std::memory_order_relaxed);
                xr_look_drive_no_xr_.store(ld.no_xr,std::memory_order_relaxed);
                xr_look_drive_xinput_.store(ld.xinput,std::memory_order_relaxed);
                xr_look_drive_closed_loop_.store(ld.closed_loop,std::memory_order_relaxed);
                xr_look_ref_cluster_.store(ld.ref_cluster,std::memory_order_relaxed);
                {   // POV fixes: lean needs the producer and the consumer; hud follow needs the HUD layer.
                    const bool leanOk=opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&config.config.xr_stereo_output();
                    pov_neck_lever_m_.store(leanOk?config.config.neck_lever_m():0.0f,std::memory_order_relaxed);
                    pov_translation_scale_.store(leanOk?config.config.translation_scale():1.0f,std::memory_order_relaxed);
                    xr_hud_follow_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()&&config.config.xr_hud_layer()?config.config.xr_hud_follow():0u,std::memory_order_relaxed);
                }
            }
            ee_camera_look_input_trace_.store(opt_in&&config.config.logging()&&config.config.ee_camera_probe()&&config.config.ee_camera_look_input_trace(),std::memory_order_relaxed);
            ee_lens_scan_.store(config.config.lens_scan_latch(opt_in),std::memory_order_relaxed); // lens/FOV phase 0: read-only; refused with any write key
            ee_ps2_probe_.store(config.config.ps2_probe_latch(opt_in),std::memory_order_relaxed); // PS2 address probe: read-only; refused with any write key
            // J-hud-radar: extends the HUD replay only (a lone key stays inert).
            gpu_copy_step5_hud_radar_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_hud()&&config.config.gpu_copy_step5_hud_radar(),std::memory_order_relaxed);
            // J-effects-layer: each key latches only together with the keys it extends (a lone key stays inert).
            gpu_copy_step5_effects_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&config.config.gpu_copy_step5_hud()&&
                config.config.gpu_copy_step5_effects(),std::memory_order_relaxed);
            // J-effects-dual: extends the effects producer only (a lone key stays inert).
            gpu_copy_step5_effects_dual_.store(opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&config.config.gpu_copy_step5_hud()&&
                config.config.gpu_copy_step5_effects()&&config.config.gpu_copy_step5_effects_dual(),std::memory_order_relaxed);
            xr_effects_layer_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()&&config.config.xr_world_in_projection()&&
                config.config.xr_effects_layer(),std::memory_order_relaxed);
            // J-test8-fixes: the depth fix extends the world-object replay (latched with it); the two consumer diagnostics need xr_stereo_output.
            const bool depthFixLatched=opt_in&&config.config.logging()&&config.config.gpu_copy_step5_continuous()&&
                config.config.gpu_copy_step5_members_by_class()&&config.config.gpu_copy_step5_world_objects()&&
                config.config.xr_world_objects_depth_fix();
            xr_world_objects_depth_fix_.store(depthFixLatched,std::memory_order_relaxed);
            // J-occlusion-fix: consumer; needs the consumer chain AND the depth fix actually latched (the object depth band it classifies on).
            xr_world_window_occlude_.store(depthFixLatched&&config.config.xr_stereo_output()&&config.config.xr_world_in_projection()&&
                config.config.xr_world_window_occlude(),std::memory_order_relaxed);
            xr_png_once_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()&&config.config.xr_png_once(),std::memory_order_relaxed);
            xr_debug_view_.store(opt_in&&config.config.logging()&&config.config.xr_stereo_output()?config.config.xr_debug_view():0u,std::memory_order_relaxed);
            perf_lean_.store(opt_in&&config.config.logging()&&config.config.perf_lean(),std::memory_order_relaxed); // FPS investigation
            if(!enabled_.load(std::memory_order_relaxed))return;
            snapshot_directory_=paths.logs;snapshot_game_=paths.game;
            if(!io_.create_directory(paths.logs)){fail();return;}file_=std::move(file);
            // Existing rotations may belong to an older invocation. Enforce
            // bounds on them too; never append to preexisting oversized logs.
            for(size_t i=1;i<=max_rotated_files;++i){uint64_t n=0;if(!io_.size(rotated(file_,i),n)||n>max_file_size){fail();return;}}
            if(production_) {
                evidence_file_=std::move(evidence);uint64_t n=0;
                if(!io_.size(evidence_file_,n)||n>max_evidence_size||!io_.reset(evidence_file_)){fail();return;}
                // Reset only opted-in diagnostic outputs from the prior invocation.
                // Empty files mean no snapshot, never old bytes from another run.
                if(ownership_pins())for(size_t i=0;i<CpuUploadSnapshot::limit;++i) {
                    const auto raw=snapshot_directory_/(L"diagnostics.cpu-snapshot-"+std::to_wstring(i)+L".bin");
                    if(!safe_path(raw,snapshot_game_)||normalized_path(raw).parent_path()!=normalized_path(snapshot_directory_)||
                       !io_.size(raw,n)||n>CpuUploadSnapshot::file_cap||!io_.reset(raw)){fail();return;}
                }
            }
            started_.store(true,std::memory_order_relaxed); // U-soak: initialization complete with logging enabled (never cleared)
        }
        if(!enabled_.load(std::memory_order_relaxed))return;
        // Snapshot summaries on the consumer only; no producer runs I/O.
        IntervalSummary timing;std::array<uint64_t,static_cast<size_t>(Event::count)> events{};
        bool snapshot=false;
        if(!ring_lock_.test_and_set(std::memory_order_acquire)) {
            FlagGuard guard{ring_lock_};timing=intervals_.summary();snapshot=true;
        }
        for(size_t i=0;i<events.size();++i)events[i]=events_[i].load(std::memory_order_relaxed);
        if(production_){drain_reserved();drain_reasons();drain_outcomes();drain_l3_counts();drain_raw_snapshots();}
        // Bound work even if producers refill the ring while flushing.
        Record next;for(size_t i=0;i<ring_capacity&&pop(next);++i) {
            write(next);
        }
        if(snapshot&&!production_) {
            // Summary records share the same sequence/ring/write budgets.
            // Push after draining to give ordinary events priority.
            push(record(Event::execute_summary,timing.count,timing.min,timing.max,"execute-interval-us; Present seam absent"));
            push(record(Event::execute_summary,timing.mean,timing.buckets[0],timing.buckets[1],"mean-us; buckets <1ms,<10ms"));
            push(record(Event::execute_summary,timing.buckets[2],timing.buckets[3],0,"buckets <100ms,>=100ms (clamped 1s)"));
            for(size_t i=0;i<events.size();++i)if(events[i]||drops_[i].load())push(record(Event::summary,i,events[i],drops_[i].load(),names[i]));
            for(size_t i=0;i<ring_capacity&&pop(next);++i)write(next);
        }
        if(production_) {
            // Consumer records bypass the producer ring: saturation cannot
            // prevent accounting. These are cumulative snapshots, never raw
            // descriptor/metadata records. Their lifetime quota is ordinary.
            const auto now=clock_();bool changed=false;
            for(size_t i=0;i<events.size();++i)
                changed|=events[i]!=last_events_[i]||drops_[i].load()!=last_drops_[i];
            const auto elapsed=now>=summary_us_?now-summary_us_:0;
            const bool periodic_data=has_submissions()||events[static_cast<size_t>(Event::descriptor)]||
                events[static_cast<size_t>(Event::metadata_disabled)]||dropped_.load();
            if(periodic_data&&(!summary_seen_||(elapsed>=summary_min_us&&(changed||elapsed>=summary_refresh_us)))) {
                // Pre-count consumer attempts so every reported per-kind total,
                // including summary itself, includes this entire batch.
                auto& records=summary_records_;size_t n=0;
                if(snapshot&&has_submissions())records[n++]=record(Event::execute_summary,timing.count,timing.min,timing.max,
                    "execute-interval-us; Present seam absent");
                for(size_t i=0;i<events.size();++i) {
                    records[n++]=record(Event::summary,i,0,0,names[i]);
                    if(overflow_[i].load())records[n++]=record(Event::summary,i,overflow_[i].load(),0,"priority overflow");
                }
                for(size_t j=0;j<n;++j)events_[static_cast<size_t>(records[j].kind)].fetch_add(1,std::memory_order_relaxed);
                std::array<uint64_t,static_cast<size_t>(Event::count)> reported{},lost{};
                for(size_t i=0;i<events.size();++i){reported[i]=events_[i].load();lost[i]=drops_[i].load();}
                for(size_t j=0;j<n;++j) {
                    auto& r=records[j];
                    if(r.kind==Event::summary&&r.text[0]&&std::strcmp(r.text.data(),"priority overflow")!=0) {
                        r.b=reported[r.a];r.c=lost[r.a];
                    }
                    r.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;write(r);
                }
                append_batch();summary_seen_=true;summary_us_=now;
                // Keep exactly the values reported, never later producer values
                // that arrived during I/O; those must still count as a change.
                last_events_=reported;last_drops_=lost;
            }
            append_batch(); // Ring traffic drains even when summaries are suppressed.
            if(checkpoint_pending_) {
                checkpoint_pending_=false;evidence_batch_=true;
                std::array<uint64_t,static_cast<size_t>(Event::count)> over{},produced{},lost{};
                size_t summaries=1+events.size();
                for(size_t i=0;i<events.size();++i){over[i]=overflow_[i].load();if(over[i])++summaries;}
                events_[static_cast<size_t>(Event::summary)].fetch_add(summaries);
                events_[static_cast<size_t>(Event::execute_summary)].fetch_add(3);
                for(size_t i=0;i<events.size();++i){produced[i]=events_[i].load();lost[i]=drops_[i].load();}
                auto checkpoint=[&](Record r) {r.sequence=sequence_.fetch_add(1)+1;write_evidence(r);};
                checkpoint(record(Event::summary,0,0,0,"Release checkpoint; not process teardown"));
                checkpoint(record(Event::execute_summary,timing.count,timing.min,timing.max,"execute-interval-us; Present seam absent"));
                checkpoint(record(Event::execute_summary,timing.mean,timing.buckets[0],timing.buckets[1],"mean-us; buckets <1ms,<10ms"));
                checkpoint(record(Event::execute_summary,timing.buckets[2],timing.buckets[3],0,"buckets <100ms,>=100ms (clamped 1s)"));
                for(size_t i=0;i<events.size();++i) {
                    checkpoint(record(Event::summary,i,produced[i],lost[i],names[i]));
                    if(over[i])checkpoint(record(Event::summary,i,over[i],0,"priority overflow"));
                }
                append_batch();evidence_batch_=false;
            }
        }
    } catch(...) {fail();}
}
Counters Log::counters() const noexcept {return {dropped_.load(),io_failures_.load(),config_failures_.load(),unknown_keys_.load(),written_.load(),rotations_.load(),rotation_failures_.load()};}
uint64_t Log::dropped(Event e) const noexcept {auto i=static_cast<size_t>(e);return i<drops_.size()?drops_[i].load():0;}
uint64_t Log::overflow(Event e) const noexcept {auto i=static_cast<size_t>(e);return i<overflow_.size()?overflow_[i].load():0;}
uint64_t Log::observed(Event e) const noexcept {auto i=static_cast<size_t>(e);return i<events_.size()?events_[i].load():0;}
void set_sink(Log* log) noexcept {active.store(log,std::memory_order_release);}
Log* sink() noexcept {return active.load(std::memory_order_acquire);}
void emit(Record r,unsigned verbosity) noexcept {if(auto* log=sink())log->push(r,verbosity);}
void execute_interval() noexcept {if(auto* log=sink())log->execute_interval();}
void observer_disabled(const char* reason,DisableEvent kind) noexcept {if(auto* log=sink())log->observer_disabled(reason,kind);}
void list_refused(ListRefusal reason,ListSeam seam,unsigned type) noexcept {
    if(auto* log=sink())log->list_refused(reason,seam,type);
}
void list_registered(unsigned path) noexcept {if(auto* log=sink())log->list_registered(path);}
void metadata_outcome(const requirements::MetadataResult& value,const capture::Command& cmd,unsigned list_path,const char* context) noexcept {
    if(auto* log=sink())log->metadata_outcome(value,cmd,list_path,context);
}
void decision(bool enabled,const char* reason) noexcept {emit(record(Event::capture_decision,enabled,0,0,reason));}

#ifdef ACZ_DIAGNOSTICS_OFFLINE_TEST
Activation* activation_test=nullptr;
#endif
namespace {
Activation& activation() noexcept {
#ifdef ACZ_DIAGNOSTICS_OFFLINE_TEST
    if(activation_test)return *activation_test;
#endif
    static Activation value;return value;
}
struct ProductionLog {
    DefaultPaths paths;FileIO io;Log log;
    ProductionLog():log(paths,io,nullptr,true) {}
};
}
void activate() noexcept {
#ifdef ACZ_DIAGNOSTICS_OFFLINE_TEST
    if(!activation_test)return; // Older fixtures must never read the user profile.
#endif
    static thread_local bool entering=false;
    if(entering||mutex_depth)return;
    entering=true;
    try {
        auto& a=activation();
        std::call_once(a.once,[&] {
            try {
                Log* log=nullptr;
                if(a.paths&&a.io)log=new Log(*a.paths,*a.io,a.clock,true);
                else log=&(new ProductionLog)->log;
                // Deliberately retain even disabled/failed holders: no teardown
                // ownership transitions or destructor I/O on a game call.
                log->push(record(Event::proxy_load,1,0,0,"lazy diagnostics activation"));
                log->push(record(Event::missing_present,1,0,0,"Present seam absent"));
                log->flush();
                if(log->enabled()) {
                    a.last_us=(a.clock?a.clock:default_clock)();
                    a.log.store(log,std::memory_order_release);set_sink(log);
                }
            } catch(...) {} // INSIDE once: failure is terminal, including new.
        });
    } catch(...) {}
    entering=false;
}
namespace {
void flush_production(bool release) noexcept {
    if(mutex_depth)return;
    auto& a=activation();auto* log=a.log.load(std::memory_order_acquire);
    if(!log)return;
    if(a.flushing.test_and_set(std::memory_order_acquire))return;
    FlagGuard guard{a.flushing}; // Also guards recursive Release from providers.
    const auto now=(a.clock?a.clock:default_clock)();
    if(release&&!log->has_submissions())return;
    if(!release&&log->perf_lean()) { // FPS investigation: the worker flushes (flush_from_worker); keep the render thread free of file I/O
        const auto w=a.worker_us.load(std::memory_order_relaxed);
        if(w&&now>=w&&now-w<2000000)return;
    }
    if(!release) {
        if(a.submissions<flush_submissions)++a.submissions; // Saturating.
        if(a.submissions<flush_submissions&&!(now>=a.last_us&&now-a.last_us>=flush_window_us))return;
    }
    // One checkpoint bypasses the periodic clock limit. All later object
    // Releases share the same limiter as Execute and cannot force I/O storms.
    if(release) {
        if(a.checkpoint&&!(now>=a.last_us&&now-a.last_us>=flush_window_us))return;
        if(!a.checkpoint){a.checkpoint=true;log->final_summary();}
    }
    a.submissions=0;if(now>=a.last_us)a.last_us=now;
    log->flush();
}
}
void flush_after_execute() noexcept {perf::Scope cost(perf::flush);flush_production(false);}
void flush_after_release() noexcept {flush_production(true);}
void flush_from_worker() noexcept {
    if(mutex_depth)return;
    auto& a=activation();auto* log=a.log.load(std::memory_order_acquire);
    if(!log||!log->perf_lean())return;
    if(a.flushing.test_and_set(std::memory_order_acquire))return;
    FlagGuard guard{a.flushing};
    const auto now=(a.clock?a.clock:default_clock)();
    a.worker_us.store(now?now:1,std::memory_order_relaxed);
    if(a.last_us&&!(now>=a.last_us&&now-a.last_us>=flush_window_us))return;
    a.submissions=0;if(now>=a.last_us)a.last_us=now;
    log->flush();
}
}
