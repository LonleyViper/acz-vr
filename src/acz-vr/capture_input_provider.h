#pragma once
#include "native_binding_observer.h"

namespace acz::capture_input {
using capture::Id;
using capture::Inputs;
using capture::State;
using capture::Command;
// Offline CPU seam only. The caller must hold one external serialization guard
// across metadata resolution, byte acquisition, assemble and freeze_owned.
// That guard excludes list/observer events AND mutations through any byte alias.
// shared_ptr<const> alone does not establish synchronization or exclude aliases.
// No Map, GPU VA dereference, native copy, hook, queue or runtime operation.
struct Buffer {
    binding::Token resource;
    // A current observed descriptor/root snapshot witnesses the resource's full
    // size and lifetime. IA-only resources without such a witness are unsupported
    // by this bounded adapter; never guess size from creation/readability flags.
    binding::NativeResolution witness;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
struct Descriptor {
    uint32_t parameter=0, table_offset=0;
    binding::NativeResolution metadata;
};
struct Request {
    Id list_identity=0;
    uint64_t generation=0, revision=0, device=0;
    bool serialized=false, synchronized=false;
    std::vector<Buffer> buffers;
    // Only explicitly supplied descriptors are adapted; holes remain absent.
    std::vector<Descriptor> descriptors;
    bool indexing_proven=false, vertex_fetch_proven=false;
    std::vector<uint64_t> srt_indices;
    std::vector<Inputs::VertexAccess> vertex_access;
};
class Lease;
struct Result {
    std::shared_ptr<const Lease> lease;
    std::string rejection;
    explicit operator bool() const { return bool(lease); }
};
// On allocation exhaustion the rejection string may be empty; a null lease or
// snapshot always means failure, with no partial publication.
// An immutable lease owns every span and metadata token referenced by Inputs.
// It is not a COM reference or a guarantee that native history is complete.
class Lease {
public:
    const Inputs& inputs() const noexcept { return inputs_; }
private:
    Request owners_;
    Inputs inputs_;
    friend Result assemble(const binding::NativeBindingObserver&, const State&, const Request&);
    friend capture::Result freeze_owned(const binding::NativeBindingObserver&, const State&, const Command&, const Lease&);
};
Result assemble(const binding::NativeBindingObserver&, const State&, const Request&);
// Rechecks live provenance immediately before existing freeze. All-stage usage,
// shader indexing and vertex fetch remain explicit caller facts. Native State
// requirements_proven=false still fails. Returns only a freeze-owned snapshot,
// never partial publication or gpu_replay_complete. Both eyes/all instances must
// use this single snapshot (e.g. stereo_payload::prepare with explicit eyeVP/K).
// Metadata currentness does not independently prove byte synchronization. A
// raced/uncertain provider must withhold its assertions and fails closed.
capture::Result freeze_owned(const binding::NativeBindingObserver&, const State&, const Command&, const Lease&);
} // namespace acz::capture_input
