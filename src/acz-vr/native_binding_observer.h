#pragma once
#include "buffer_binding_registry.h"
#include "descriptor_table_diagnostics.h"
#include "perf_counters.h"
#include <d3d12.h>
#include <set>
#include <array>
#include <atomic>

namespace acz::binding {
// Canonical pair + relation: 13 * 14 / 2 * 2 = 182 static literal reasons.
// metadata_release names the shared Release hook; other remains the fallback
// for an unclassified/invalid kind, never a known metadata Release.
// Only observed pairs consume the logger's 64 slots; excess uses its overflow.
enum class ObservationKind : unsigned char {
    descriptor_write, copy, committed_create, placed_create, reserved_create,
    heap_create, resource_release, heap_release, root_create, pso_create, device_release, metadata_release, other, count
};
inline constexpr unsigned overlap_kind_count=static_cast<unsigned>(ObservationKind::count);
inline constexpr unsigned overlap_reason_count=overlap_kind_count*(overlap_kind_count+1);
const char* overlap_reason(ObservationKind, ObservationKind, bool same_thread) noexcept;

// Owned METADATA ONLY. No ReadableCopy/TableCopy, byte access, synchronization,
// all-stage requirements, indexing proof or replay readiness is manufactured.
struct NativeResolution : Status {
    capture::Id list_identity=0;
    uint64_t generation=0, revision=0;
    uint64_t device=0;
    std::shared_ptr<const Snapshot> snapshot;
    explicit operator bool() const { return rejection.empty() && bool(snapshot); }
};
// Caller serializes all methods with the capture-state mutex. No COM calls or
// ownership here. A missed event/allocation/partial hook disables this instance
// permanently; reinstallation cannot repair lost native history.
class NativeBindingObserver {
public:
    void diagnostic_tracking(bool on) noexcept {
        if(!on) {
            if(diagnostic_active_&&diagnostic_)diagnostic_->history_complete=false;
            if(textures_)textures_->invalidate();
            diagnostic_active_=false;return;
        }
        diagnostic_active_=true;
        if(diagnostic_attempted_)return;
        diagnostic_attempted_=true;
        try {
            diagnostic_=std::make_unique<diagnostics::DescriptorTableDiagnostics>();
            diagnostic_identity_=next_diagnostic_identity();
            textures_=std::make_unique<diagnostics::TextureDescriptionRegistry>();
            diagnostic_->history_complete=heaps_.empty()&&resources_.empty()&&enabled_;
        }catch(...) {texture_allocation_gap_=true;if(diagnostic_){diagnostic_->overflow=true;diagnostic_->history_complete=false;}}
    }
    const diagnostics::DescriptorTableDiagnostics* diagnostic() const noexcept {return diagnostic_active_?diagnostic_.get():nullptr;}
    // Publish diagnostic retirement before an owned candidate Release can reenter.
    // The serial protects a newer heap at the same address.
    void diagnostic_retire_heap(uint64_t key,uint64_t serial) noexcept {
        if(!diagnostic_active_||!diagnostic_)return;
        auto h=diagnostic_->heaps.find(key);
        if(h!=diagnostic_->heaps.end()&&h->second.serial==serial)h->second.live=false;
    }
    uint64_t diagnostic_identity() const noexcept {return diagnostic_active_?diagnostic_identity_:0;}
    diagnostics::DescriptorTableDiagnostics::Snapshot diagnostic_snapshot(
        diagnostics::DescriptorTableDiagnostics::Location,uint32_t offset,unsigned expected=diagnostics::DescriptorTableDiagnostics::srv) const noexcept;
    void diagnostic_creation(unsigned seam,unsigned dimension) noexcept {
        diagnose([&](auto& d){d.creation(seam,dimension);});
    }
    // Same default-OFF diagnostic_ownership_pins switch as descriptor witnesses.
    // Textures get no lifetime hook, QI, pin or eligibility record.
    void diagnostic_texture(bool success,uint64_t address,uint64_t device,const D3D12_RESOURCE_DESC* desc,
                            unsigned seam,unsigned heap_type=0,const D3D12_HEAP_PROPERTIES* props=nullptr,
                            unsigned flags=0,const GUID* iid=nullptr,bool overlap=false) noexcept {
        if(!diagnostic_active_||!success||!address)return;
        if(desc&&desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
            if(textures_)textures_->retire(address);return;
        }
        try {
            if(!textures_)textures_=std::make_unique<diagnostics::TextureDescriptionRegistry>();
            if(texture_allocation_gap_)textures_->invalidate(diagnostics::TextureDescriptionRegistry::allocation_gap);
            textures_->creation(success,address,device,desc,seam,heap_type,props,flags,iid,overlap);
        }catch(...) {texture_allocation_gap_=true;if(diagnostic_)diagnostic_->overflow=true;}
    }
    void diagnostic_device_interface(unsigned bit) noexcept {
        if(diagnostic_active_&&textures_)textures_->exposed|=bit;
    }
    void diagnostic_view(uint64_t address,uint64_t device) noexcept {
        const auto owner=resource_devices_.find(address);
        if(diagnostic_active_&&textures_&&
           (owner==resource_devices_.end()||owner->second!=device||!resource_token(address).id))textures_->view(address,device,false);
    }
    void append_texture_diagnostics(char* text,size_t bytes) const noexcept {
        if(diagnostic_active_&&textures_)textures_->append(text,bytes);
        else if(diagnostic_active_) {
            const auto n=std::strlen(text);if(n<bytes)std::snprintf(text+n,bytes-n,";ti=100;registry=unavailable");
        }
    }
    bool enabled() const noexcept { return enabled_; }
    void disable(const char* static_reason="native binding observer disabled") noexcept { enabled_=false;reason_=static_reason; }
    const char* disable_reason() const noexcept {return reason_;}
    Token heap_token(uint64_t id) const;
    Token resource_token(uint64_t id) const;
    // Setter-only address resolution. Draw retains this opaque result; it
    // never scans the registry or allocates a NativeResolution.
    uint64_t diagnostic_address(uint64_t address,uint64_t bytes,uint64_t device) const noexcept {
        perf::Scope cost(perf::addr_lookup);
        const auto key=registry_.observed_address(address,bytes);
        if(!key)return 0;
        const auto owner=resource_devices_.find(key);
        return owner!=resource_devices_.end()&&owner->second==device?key:0;
    }
    // `tolerate` (J-xr-stereo-output): a rejected registration made while a
    // proxy XR call is in flight on ANOTHER thread is counted and NOT tracked,
    // instead of disabling the observer. Default false: unchanged behaviour.
    void heap(const HeapEvent&, uint64_t device=0, bool tolerate=false);
    // A heap created inside the proxy-internal creation scope (the XR owner
    // thread's runtime/consumer calls) is not the game's: never registered,
    // never a reason to disable; a stale same-address registration is dropped.
    void foreign_heap(uint64_t id);
    // Last registry rejection text (fixed storage) and the scope/window counters.
    const char* last_rejection() const noexcept {return last_rejection_.data();}
    uint64_t foreign_heaps() const noexcept {return foreign_heaps_;}
    uint64_t tolerated_rejections() const noexcept {return tolerated_rejections_;}
    void resource(const ResourceEvent&, uint64_t device=0);
    void remove_heap(Token);
    void remove_resource(Token);
    void srv(uint64_t resource, const D3D12_SHADER_RESOURCE_VIEW_DESC*, uint64_t cpu, uint64_t device=0,
        const diagnostics::ViewWriteDescription* view_write=nullptr);
    void cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, uint64_t cpu, uint64_t device=0);
    void sampler(const D3D12_SAMPLER_DESC*, uint64_t cpu, uint64_t device=0);
    Footprint footprint(uint64_t cpu, uint32_t count, uint64_t device) const noexcept;
    void invalidate(const Footprint& f) noexcept {
        registry_.invalidate(f);
        // Existing overlap invalidation is an observation gap for diagnostics
        // too. Never publish a pristine copied/write history after a gap.
        if(diagnostic_active_&&diagnostic_)diagnostic_->invalidate();
        if(textures_)textures_->invalidate();
    }
    void invalidate_object(uint64_t id);
    // Permanent opaque-address tombstone for an observed, unprovable shared
    // object. Even later same-address creation cannot manufacture provenance.
    // Allocation failure is handled by the caller's fail-closed observation.
    void mark_untracked(uint64_t id);
    bool untracked(uint64_t id) const noexcept {return untracked_.count(id)!=0;}
    void unsupported_write(uint64_t cpu, uint64_t device=0,
        unsigned diagnostic_type=diagnostics::DescriptorTableDiagnostics::unknown);
    void copy(UINT dest_count, const D3D12_CPU_DESCRIPTOR_HANDLE* dest, const UINT* dest_sizes,
              UINT source_count, const D3D12_CPU_DESCRIPTOR_HANDLE* source, const UINT* source_sizes,
              D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t device=0, bool simple=false);
    NativeResolution table(const capture::State&, uint32_t parameter, uint64_t heap_id, uint32_t offset, uint64_t device=0) const;
    // Explicit provider layout only. A root descriptor itself declares no size
    // or stride. CBV uses bytes; SRV uses elements/stride; UAV is unsupported.
    NativeResolution root(const capture::State&, uint32_t parameter, uint32_t bytes,
                          uint32_t elements, uint32_t stride, uint64_t device=0) const;
    Status current(const NativeResolution&, const capture::State&, uint64_t device=0) const;
private:
    // Store freshness only, not a game-object identity. Assigned once at the
    // existing allocation seam; no draw/write retry or shared capture ID use.
    inline static std::atomic<uint64_t> diagnostic_serial_{1};
    uint64_t diagnostic_identity_=0;
    static uint64_t next_diagnostic_identity() noexcept {
        auto id=diagnostic_serial_.load();
        while(id!=UINT64_MAX)if(diagnostic_serial_.compare_exchange_weak(id,id+1))return id;
        return 0;
    }
    std::unique_ptr<diagnostics::DescriptorTableDiagnostics> diagnostic_;
    std::unique_ptr<diagnostics::TextureDescriptionRegistry> textures_;
    bool diagnostic_attempted_=false,diagnostic_active_=false,texture_allocation_gap_=false;
    template<class F> void diagnose(F&& f) noexcept {
        if(!diagnostic_active_||!diagnostic_)return;
        try {f(*diagnostic_);}catch(...) {diagnostic_->overflow=true;}
    }
    void diagnostic_copy(UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,
        UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE,uint64_t,bool) noexcept;
    BufferBindingRegistry registry_;
    std::map<uint64_t,Token> heaps_, resources_;
    std::map<uint64_t,HeapEvent> heap_events_;
    std::map<uint64_t,uint64_t> heap_devices_,resource_devices_;
    std::set<uint64_t> untracked_;
    std::array<char,96> last_rejection_{};
    uint64_t foreign_heaps_=0,tolerated_rejections_=0;
    bool enabled_=true;
    const char* reason_="native binding observer disabled";
    Token cpu_heap(uint64_t cpu, uint64_t device) const;
    Token address_resource(uint64_t address, uint64_t bytes, uint64_t device) const;
    static NativeResolution rejected(const char*);
    NativeResolution stamp(const capture::State&, const Resolution&, uint64_t device) const;
};
} // namespace acz::binding
