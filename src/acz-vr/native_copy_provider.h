#pragma once
#include "capture_input_provider.h"
#include "draw_input_provenance_contract.h"
#include "native_provenance_evidence.h"

namespace acz::native_copy {
// CPU-only opt-in seam, never wired to hooks. All tokens/addresses are opaque.
// Caller holds the SAME external serialization guard required by J027 across
// resolution, acquire, assemble and freeze_owned. Observer/State are not thread
// safe. A source must not race C++ memory accesses; version checks cannot cure
// data races, ABA/wraparound, dishonest reports or missed native events.
struct Fence { uint64_t identity=0, value=0; };
struct Stamp { Fence completed; uint64_t version=0; };
struct Request {
    binding::Token resource;
    binding::NativeResolution witness; // Owns full list/device/heap provenance.
    // Checked byte range: offset + elements*stride. J027 requires the ENTIRE
    // resource, so partial ranges are rejected even if they are in bounds.
    uint64_t offset=0, elements=0, stride=1;
    Fence required; // Nonzero identity and required completion value.
    uint64_t device=0; // Independent caller-expected device, not echoed witness.
};
class Source {
public:
    virtual ~Source()=default;
    // Reports concern this exact resource lifetime/range/capture point.
    // Nonzero, non-recycled versions advance on every mutation. This is an
    // injected-source obligation, not something this adapter can prove.
    virtual Stamp inspect(const Request&)=0;
    // Only byte source. Write at most capacity bytes, return actual count;
    // retain no destination alias after return. Throw on unavailable copy.
    virtual size_t copy(const Request&, uint8_t* destination, size_t capacity)=0;
};
struct Result {
    capture_input::Buffer buffer;
    Stamp before, after;
    std::string rejection;
    explicit operator bool() const noexcept { return bool(buffer.bytes); }
};
// CAN establish: owned storage independent of source lifetime/mutation; exact
// full byte count within max_copy_bytes; two current metadata checks surrounding
// copy with matching resource/heap tokens and list identity/generation/revision;
// source REPORTED the same nonzero version and completed-fence identity matching
// required.identity, with values >= required.value (and no reported regression).
// CANNOT establish: a token represents a real GPU fence/queue completion, GPU
// readiness, absence of concurrent byte mutation, native history completeness,
// all-stage usage/indexing/vertex-fetch, or runtime VR/replay correctness.
// Does NOT assert J027 serialized/synchronized, requirements_proven,
// cpu_data_complete or gpu_replay_complete. Caller supplies independent facts.
// J027 assemble enforces the aggregate budget if several copies are combined.
// No GPU VA dereference, Map, device/queue/fence creation/submission/wait.
// Failure (including exceptions/allocation exhaustion) publishes no bytes;
// rejection text can be empty on allocation failure.
Result acquire(const binding::NativeBindingObserver&, const capture::State&,
               Source&, const Request&) noexcept;

// Inert, single-use fake backend for test-owned bytes. The only writer is
// write() before seal(); no pointer, map, or mutable byte alias is exposed.
// Injected events are synthetic reports, not native queue/fence observations.
struct FakeIdentity {
    uint64_t device=0, list=0, generation=0, revision=0;
    uint64_t queue=0, queue_epoch=0, submission=0;
};
struct FakeSubmission {
    FakeIdentity identity{};
    binding::Token resource{};
    uint64_t bytes=0;
    Fence signal{};
    uint64_t source_version=0;
    bool submitted=false, ambiguous=false;
};
enum class FakeCompletionStatus : uint8_t { Pending, Confirmed, Timeout, DeviceLost, Ambiguous };
struct FakeCompletion {
    FakeIdentity identity{};
    Fence completed{};
    FakeCompletionStatus status=FakeCompletionStatus::Pending;
    bool independently_confirmed=false;
};
struct FakeSubresource {
    uint32_t index=0;
    draw_provenance::State original=draw_provenance::State::Unknown;
    draw_provenance::State copy_read=draw_provenance::State::Unknown;
    draw_provenance::State restored=draw_provenance::State::Unknown;
    bool original_observed=false, copy_transition_observed=false;
    bool restoration_observed=false, alias_excluded=false;
    bool split_barrier_excluded=false, implicit_transition_excluded=false;
};
class OwnedFakeAttempt final : private Source {
public:
    // Allocates and owns fresh CPU storage. At most one attempt and one copy.
    OwnedFakeAttempt(size_t bytes, uint32_t subresources);
    OwnedFakeAttempt(const OwnedFakeAttempt&)=delete;
    OwnedFakeAttempt& operator=(const OwnedFakeAttempt&)=delete;
    bool write(size_t offset, const uint8_t* data, size_t count) noexcept;
    bool seal() noexcept;
    void unobservable_writer() noexcept; // Injected persistent-map/missed-write fault.
    void submit(const FakeSubmission&) noexcept;
    void complete(const FakeCompletion&) noexcept;
    Result finish(const binding::NativeBindingObserver&, const capture::State&,
                  const Request&, const FakeIdentity& expected,
                  const FakeSubresource*, size_t state_count) noexcept;
    // An uncertain or failed attempt retains its fake source and identity until
    // destruction; retire() only releases after a completed, copied attempt.
    bool retire() noexcept;
    bool quarantined() const noexcept { return quarantined_; }
    bool retired() const noexcept { return retired_; }
    size_t retained_bytes() const noexcept { return bytes_.size(); }
    FakeSubmission retained_submission() const noexcept { return submission_; }
    FakeCompletion retained_completion() const noexcept { return completion_; }
    unsigned copies() const noexcept { return copies_; }
    uint64_t version() const noexcept { return version_; }
    bool native_proof_available() const noexcept { return false; }
    uint32_t unknown_mask() const noexcept { return native_evidence::permanent_unknown_mask; }
private:
    Stamp inspect(const Request&) override;
    size_t copy(const Request&, uint8_t*, size_t) override;
    Result refuse(const char*) noexcept;
    std::vector<uint8_t> bytes_;
    uint32_t subresources_=0;
    uint64_t version_=1;
    FakeSubmission submission_{};
    FakeCompletion completion_{};
    bool sealed_=false, submitted_=false, completed_=false;
    bool attempted_=false, copied_=false, quarantined_=false, retired_=false;
    unsigned copies_=0;
};
} // namespace acz::native_copy
