#pragma once
#include "root_binding_metadata.h"
#include "srt_payload.h"
#include "index_range.h"
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace acz::capture {
using Id = uint64_t;
// Process-unique saturating identities, shared by lists, PSOs and roots.
Id new_lifetime_identity();
struct Command {
    bool indexed=false;
    uint32_t count=0, instances=0, start=0;
    int32_t base_vertex=0;
    uint32_t start_instance=0;
};
struct BufferView { Id resource=0; uint64_t offset=0, size=0; uint32_t stride=0; };
struct Descriptor {
    metadata::BindingKind kind=metadata::BindingKind::srv;
    BufferView view;
    // view.offset is the byte origin of the buffer view BEFORE FirstElement.
    // Element bytes start at view.offset + (first_element + index)*stride.
    // Do not also fold FirstElement into view.offset.
    uint64_t first_element=0, elements=0;
    bool structured=false;
    bool buffer=true; // Textures/opaque subresource layouts are unsupported.
};
// CPU-only seam. The provider must supply a synchronized, stable readable copy,
// never a mapped GPU pointer. No GPU mapping/readback is performed by this API.
// Resource size must equal byte_length; partial dumps are rejected. Generation
// and list_identity identify the recording/lifetime at whose capture point the
// copy was acquired. Providers echo State provenance; never use pointer IDs.
struct ReadableCopy {
    Id resource=0;
    uint64_t generation=0, resource_size=0;
    const void* bytes=nullptr;
    size_t byte_length=0;
    bool synchronized=false, cpu_readable=false;
    uint64_t revision=0;
    Id list_identity=0;
};
struct Requirement { metadata::BindingKind kind; uint32_t reg=0, space=0; metadata::ShaderStage stage=metadata::ShaderStage::geometry; };
// Requires independently proven shader resource usage, including all stages,
// IA slots and indexing. A GS identifier alone cannot establish this manifest.
struct Pipeline {
    bool drawcpt=false, requirements_proven=false;
    std::vector<Requirement> resources;
    std::vector<uint32_t> vertex_slots;
    Id identity=0, explicit_root_identity=0;
    uint32_t strip_cut=index_range::unknown_cut,topology_type=0;
};
struct RootValue {
    metadata::ParameterKind kind=metadata::ParameterKind::constants;
    uint64_t value=0;
    std::vector<uint32_t> constants;
    std::vector<bool> written;
};
struct State {
    // Process-unique, non-recycled lifetime token. Copies preserve identity;
    // reset preserves it and advances generation. Exhaustion rejects capture.
    State();
    Id list_identity=0;
    uint64_t generation=1, revision=0;
    Pipeline pipeline;
    std::optional<metadata::RootBindingMetadata> root;
    Id root_id=0;
    Id root_identity=0;
    std::vector<Id> heaps;
    std::map<uint32_t,RootValue> roots;
    std::map<uint32_t,BufferView> vertices;
    std::optional<BufferView> index;
    const char* tracking_gap=nullptr; // static first irrecoverable observation gap
    uint32_t topology=0; // zero is unobserved/UNDEFINED; native IA setter only
    bool tracking_complete=false;
    void reset();
    void set_root(Id id, std::optional<metadata::RootBindingMetadata> layout, Id identity=0);
    // Supported subset: the identical observed immutable root object/lifetime.
    // Equivalent layouts at distinct objects are deliberately not certified.
    bool compatible_root() const;
    void set_heaps(std::vector<Id> ids);
    void set_value(uint32_t index, metadata::ParameterKind kind, uint64_t value);
    void set_constants(uint32_t index, uint32_t offset, const uint32_t* data, uint32_t count);
};
// Descriptor addresses are opaque tokens. Provider resolves the actual heap,
// descriptor increment/bounds and writes/copies at the same capture point.
struct TableCopy { Id heap=0; uint64_t base=0; std::vector<Descriptor> descriptors; uint64_t generation=0, revision=0; Id list_identity=0; };
struct Inputs {
    uint64_t generation=0, revision=0;
    Id list_identity=0;
    std::vector<TableCopy> tables;
    std::map<uint64_t,Descriptor> root_descriptors;
    std::vector<ReadableCopy> resources;
    // Explicit shader element indices for EACH instance, proven by provider.
    // Never infer these from StartInstanceLocation or draw start/base arguments.
    std::vector<uint64_t> srt_indices;
    bool indexing_proven=false;
    // Provider resolves vertex/instance input layout, indices and signed base
    // vertex before declaring these relative byte spans. No fetch rule guessed.
    struct VertexAccess { uint32_t slot=0; uint64_t offset=0, length=0; };
    bool vertex_fetch_proven=false;
    std::vector<VertexAccess> vertex_access;
    uint32_t topology=0,strip_cut=index_range::unknown_cut;
};
struct Snapshot {
    State state;
    Command command;
    std::map<Id,std::vector<uint8_t>> resources;
    struct ResolvedResource { Requirement requirement; Descriptor descriptor; };
    std::vector<ResolvedResource> bindings;
    std::vector<Inputs::VertexAccess> vertex_access;
    std::vector<std::vector<uint8_t>> srt_elements;
    // CPU-data completeness only. PSO/RTV/DSV/texture/sampler state, scheduling
    // and GPU completion are NOT implemented; this is never replay readiness.
    bool cpu_data_complete=false;
    bool gpu_replay_complete=false;
};
struct Result {
    std::shared_ptr<const Snapshot> snapshot;
    std::string rejection;
    explicit operator bool() const { return bool(snapshot); }
};
inline constexpr size_t max_copy_bytes=16*1024*1024;
Result freeze(const State& state, const Command& command, const Inputs& inputs);
// Both eyes use the same immutable source; no mutation of captured bytes.
// An incomplete snapshot or invalid instance returns inconsistent_length.
srt::Result eye_payload(const Snapshot& snapshot, size_t instance,
                        const srt::Matrix& first, const srt::Matrix& second);
} // namespace acz::capture
