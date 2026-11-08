#pragma once
// Inert CPU fixture consistency policy. No native acquisition or certification.
// J067 metadata remains Unknown for every permanent domain, even on success.
// Scope: resource epochs/history/restoration only. Shader input/profile and
// descriptor entry completeness are not checked; no replay/copy readiness.
// A caller can fabricate fixture records; this API does not authenticate them.
#include "native_provenance_evidence.h"
#include "draw_input_provenance_contract.h"
#include <cstddef>
#include <cstdint>

namespace acz::evidence_epochs {
namespace dp = draw_provenance;
constexpr std::size_t max_resources = dp::max_resources, max_states = dp::max_states;
enum class Refusal : std::uint8_t {
    None, ExportRefused, MetadataMaskChanged, RecordingMismatch, IdentityMismatch,
    MissingWitness, WitnessMismatch, CapacityExceeded, ProofForgery, Origin,
    CopyPoint, ResourceIdentity, ResourceRange, ResourceHistory, Restoration
};
const char* diagnostic(Refusal reason) noexcept;
struct Limits { std::size_t resources=0, states=0; };
// These are independently retained fixture records, NOT authenticated native
// witnesses. Each epoch observation includes its full selected-draw context.
struct EpochWitness {
    std::uint64_t record_id=0;
    dp::Origin origin=dp::Origin::Missing;
    dp::Identity identity{};
};
struct ResourceWitness {
    std::uint64_t record_id=0;
    dp::Resource resource{};
    // Independently specified complete contiguous subresource set for this
    // resource. This bounded subset excludes aliases and cross-queue copies.
    std::uint32_t first_subresource=0, subresources=0;
};
struct StateWitness {
    std::uint64_t record_id=0, resource_id=0, lifetime=0, generation=0;
    dp::Origin origin=dp::Origin::Missing;
    std::uint32_t subresource=0;
    dp::State original=dp::State::Unknown, copy_read=dp::State::Unknown,
              restored=dp::State::Unknown;
};
struct Witnesses {
    EpochWitness device{}, list{}, queue{}, submission{}, draw{};
    std::uint64_t copy_record_id=0;
    dp::Snapshot::CopyPoint copy{};
    std::size_t resource_count=0, state_count=0;
    ResourceWitness resources[max_resources]{};
    StateWitness states[max_states]{};
};
class EpochSnapshot;
EpochSnapshot build(const native_evidence::Export&, const dp::Snapshot&,
                    const Witnesses&, Limits) noexcept;
// Immutable checked result: callers cannot forge success or inventory counts.
// Copying this object retains all checked facts and the independent witnesses.
class EpochSnapshot final {
public:
    Refusal refusal() const noexcept { return refusal_; }
    bool synthetic_consistent() const noexcept { return refusal_==Refusal::None; }
    bool native_proof_available() const noexcept { return false; }
    std::uint32_t unknown_mask() const noexcept { return native_evidence::permanent_unknown_mask; }
    const dp::Identity& identity() const noexcept { return identity_; }
    const dp::Snapshot::CopyPoint& copy_point() const noexcept { return witnesses_.copy; }
    const Witnesses& witnesses() const noexcept { return witnesses_; }
    std::size_t resource_count() const noexcept { return witnesses_.resource_count; }
    const dp::Resource* resource(std::size_t i) const noexcept {
        return synthetic_consistent() && i<resource_count() ? &witnesses_.resources[i].resource : nullptr;
    }
private:
    friend EpochSnapshot build(const native_evidence::Export&, const dp::Snapshot&,
                               const Witnesses&, Limits) noexcept;
    Refusal refusal_=Refusal::ExportRefused;
    dp::Identity identity_{};
    Witnesses witnesses_{};
};
Refusal verify_epochs(const EpochSnapshot&, const dp::Identity&) noexcept;
// Check current candidate facts against the retained selection before a fake
// backend reads its owned bytes. Success alone says nothing about actual bytes.
Refusal verify_copy_point(const EpochSnapshot&, const dp::Snapshot&) noexcept;
} // namespace acz::evidence_epochs
