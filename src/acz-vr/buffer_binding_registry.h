#pragma once
#include "draw_capture.h"
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace acz::binding {
// CPU metadata only, serialized by the caller. No COM ownership, GPU reads,
// device calls or synchronization proofs. All addresses are opaque integers.
// Native wiring still needs heap/resource creation & destruction, descriptor
// writes (including unsupported/null writes), copies, and recording-point
// serialization. Missing events invalidate provenance; this does not hook them.
// Write/copy exceptions clear every descriptor slot before propagating, without
// partial copy commits. The observer must still discard incomplete native tracking.
// The same rule applies to rejected creation/destruction events: do not treat
// an unsupported or missed native lifetime event as complete tracking.
struct Token {
    uint64_t id=0, generation=0;
    friend bool operator==(Token a, Token b) { return a.id==b.id && a.generation==b.generation; }
    friend bool operator!=(Token a, Token b) { return !(a==b); }
};
struct Status {
    std::string rejection;
    explicit operator bool() const { return rejection.empty(); }
};
struct Registration : Status {
    Token token;
    explicit operator bool() const { return rejection.empty() && token.id && token.generation; }
};
// Explicit SDK adapter: Type=CBV_SRV_UAV/SAMPLER (0/1), Flags=NONE/SHADER_VISIBLE
// (0/1), single-node only. Increment comes from the provider, never guessed.
struct HeapEvent {
    uint64_t id=0, cpu_base=0, gpu_base=0;
    uint32_t count=0, increment=0, type=0, flags=0, node_mask=0;
};
// Only D3D12_RESOURCE_DIMENSION_BUFFER (1), unaliased committed-style buffers
// are supported. Provider must explicitly report aliasing/unsupported forms.
struct ResourceEvent {
    uint64_t id=0, gpu_base=0, bytes=0;
    uint32_t dimension=1;
    bool aliased=false;
};
struct Slot : Status { Token heap; uint32_t index=0; };
struct Address : Status { Token resource; uint64_t offset=0, resource_bytes=0; };
// Mirrors the BUFFER SRV union; dimension=BUFFER (1), format=UNKNOWN (0),
// flags=NONE (0). Explicitly rejects raw/typed/texture/null/opaque descriptions.
struct StructuredSrv {
    Token resource;
    uint64_t first_element=0;
    uint32_t elements=0, stride=0, dimension=1, format=0, flags=0;
};
struct Cbv { Token resource; uint64_t gpu_address=0; uint32_t bytes=0; };
struct CopyRange { Token heap; uint64_t cpu_start=0; uint32_t count=1; };
// Words: Filter, AddressU/V/W, MipLODBias, MaxAnisotropy, ComparisonFunc,
// BorderColor[0..3], MinLOD, MaxLOD. Floats retain their IEEE bit patterns; no padding,
// borrowed pointers, resource ownership or capture readiness.
struct Sampler { std::array<uint32_t,13> words{}; };
struct Footprint {
    Token heap; uint64_t begin=0,end=0; uint32_t first=0,count=0;
    uint32_t type=UINT32_MAX,flags=0; // copied from the already-resolved heap
    explicit operator bool() const noexcept { return heap.id!=0; }
};
struct Snapshot {
    Token heap, resource;
    uint32_t slot=0;
    uint64_t revision=0, resource_bytes=0;
    capture::Descriptor descriptor;
    Sampler sampler;
    // Immediate source provenance survives source heap destruction/mutation.
    Token source_heap;
    uint32_t source_slot=0;
    uint64_t source_revision=0;
};
struct Resolution : Status {
    std::shared_ptr<const Snapshot> snapshot;
    explicit operator bool() const { return rejection.empty() && bool(snapshot); }
};

class BufferBindingRegistry {
public:
    BufferBindingRegistry() = default;
    BufferBindingRegistry(const BufferBindingRegistry&) = delete;
    BufferBindingRegistry& operator=(const BufferBindingRegistry&) = delete;
    Registration add_heap(const HeapEvent&);
    Registration add_resource(const ResourceEvent&);
    Status remove_heap(Token);
    Status remove_resource(Token);
    Slot cpu_slot(Token expected, uint64_t handle) const;
    Slot gpu_slot(Token expected, uint64_t handle) const;
    Address buffer_address(Token expected, uint64_t address, uint64_t bytes) const;
    // Allocation-free diagnostic counterpart; returns only a unique contained
    // opaque address. No status strings, snapshots or ownership are produced.
    uint64_t observed_address(uint64_t address,uint64_t bytes) const noexcept;
    // FPS investigation: the id of the ONLY registered buffer whose interval intersects [address, address+bytes), else 0
    // (none, ambiguous, or an empty/overflowing span). O(log N + k) through the base index; the same overlap predicate as
    // buffer_address/observed_address, so a token succeeds in buffer_address only if it is this resource's.
    uint64_t unique_overlap(uint64_t address,uint64_t bytes) const noexcept;
    // The id of the ONLY heap whose CPU (gpu=false) or shader-visible GPU (gpu=true) interval contains `handle`, else 0:
    // the matches==1 precondition of cpu_slot/gpu_slot, computed once (O(H)).
    uint64_t unique_heap(uint64_t handle,bool gpu) const noexcept;
    size_t resource_count() const noexcept {return resources_.size();}
    size_t heap_count() const noexcept {return heaps_.size();}
    Status write_srv(Token heap, uint64_t cpu_handle, const StructuredSrv&);
    Status write_cbv(Token heap, uint64_t cpu_handle, const Cbv&);
    Status write_sampler(Token heap, uint64_t cpu_handle, const Sampler&);
    // Allocation-free whole-interval proof. Unknown/ambiguous ranges return {}.
    // Invalidation rechecks the live token/range; a retired lifetime is ignored,
    // an unprovable range clears all slots. Neither operation calls COM.
    Footprint footprint(Token, uint64_t cpu, uint32_t count) const noexcept;
    void invalidate(const Footprint&) noexcept;
    // Call on every unsupported/null/unobserved write. Rejected writes/copies
    // erase only fully proven destinations for supported invocation shapes.
    // Copies with empty/oversized source counts, unequal totals or proven whole
    // source/destination interval overlap erase all slot provenance. Unknown or
    // stale source tokens cannot establish overlap from bare handles; their
    // existing destination-local rejection remains. Unknown, stale, misaligned, empty,
    // oversized, overflowing, ambiguous or duplicate/overlapping destination
    // sets, unsupported copy heap types, and exceptions clear ALL live slots.
    // Otherwise unknown/stale/unsupported source metadata and duplicate sources
    // erase ALL proven destinations, with no partial copy publish.
    // Prior snapshots remain readable historical values; current() rejects them.
    Status invalidate_write(Token heap, uint64_t cpu_handle);
    // Flatten ranges in SDK order. An omitted SDK sizes array is adapted as
    // count=1 per range. Sources MUST be non-shader-visible; destinations may
    // be either. Unknown, duplicate or overlapping ranges fail closed. Source
    // snapshots are staged before atomic metadata assignment to destinations.
    Status copy(const std::vector<CopyRange>& destinations,
                const std::vector<CopyRange>& sources, uint32_t heap_type=0);
    Resolution resolve_cpu(Token heap, uint64_t handle) const;
    Resolution resolve_gpu(Token heap, uint64_t handle) const;
    // Root descriptors carry no stride/size in D3D12. These are explicit
    // provider-declared byte spans/structured layout, NOT shader inference.
    Resolution root_srv(Token resource, uint64_t address, uint32_t elements, uint32_t stride) const;
    Resolution root_cbv(Token resource, uint64_t address, uint32_t bytes) const;
    // Validate current lifetime and slot revision before adapting a snapshot.
    // This cannot authorize ReadableCopy, requirements_proven, indexing_proven,
    // CPU copying, GPU completion or a renderable/replay-complete snapshot.
    Status current(const Snapshot&) const;
private:
    struct Heap { HeapEvent event; Token token; std::map<uint32_t,std::shared_ptr<const Snapshot>> slots; };
    struct Resource { ResourceEvent event; Token token; };
    std::map<uint64_t,Heap> heaps_;
    std::map<uint64_t,Resource> resources_;
    // FPS investigation: GPU-VA index of resources_ (base -> {id, bytes}) and the multiset of sizes (exact maximum), kept in
    // step with resources_ by add_resource/remove_resource. Overlap queries scan only bases in (address - max_bytes, address+bytes)
    // and stop at the second match: results are identical to the former whole-map scans (registry offline tests compare both).
    struct Span { uint64_t id=0, bytes=0; };
    std::multimap<uint64_t,Span> by_base_;
    std::multiset<uint64_t> sizes_;
    // Number of resources intersecting [address, address+bytes) (caller: valid span), counting stops at `stop`; `only`
    // receives the id of the last match counted.
    unsigned overlaps(uint64_t address,uint64_t bytes,unsigned stop,uint64_t* only) const noexcept;
    Slot slot(Token, uint64_t, bool gpu) const;
    Resolution resolve(const Slot&) const;
    // Allocation-free proof checks whole CPU intervals, including inter-slot
    // overlaps with another tracked heap. No untracked destination is exempt.
    bool destination_range(const CopyRange&) const noexcept;
    bool destination_set(const std::vector<CopyRange>&) const noexcept;
    void invalidate_all() noexcept;
    void invalidate_destinations(const std::vector<CopyRange>&) noexcept;
    Status fail_write(const std::string&, const Slot* proven=nullptr);
    Status store(const Slot&, Snapshot);
};
} // namespace acz::binding
