#pragma once
// Inert fixed-storage CPU model: identities, facts, and lease actions are
// synthetic declarations. This file performs no native operation or I/O.
#include <cstddef>
#include <cstdint>
#include <atomic>

namespace acz::gpu_transaction {
constexpr std::size_t max_attempts=1, max_in_flight=1, max_leases=64,
                      max_allocations=128, max_operations=128;
constexpr std::uint64_t max_source_bytes=16ull*1024*1024;
constexpr std::uint64_t max_allocation_bytes=32ull*1024*1024;
enum class Phase : std::uint8_t { Disabled, Prepared, Submitted, Completed, Retired, Refused, Quarantined };
enum class Reason : std::uint8_t { None, MissingFact, IdentityMismatch, InvalidCategory,
    InvalidBounds, InvalidAggregate, Capacity, Admission, PreCommitFailure,
    AmbiguousCommit, CompletionIdentity, CompletionEpoch, CompletionBelow,
    CompletionAbove, CompletionRegressed, CompletionPending, CompletionUnconfirmed,
    CompletionTimeout, CompletionDeviceLost, CompletionInvalid,
    TeardownBeforeCommit, TeardownAfterCommit };
enum class Intent : std::uint8_t { Unknown, Copy, Replay };
enum class Outcome : std::uint8_t { None, SyntheticCopyCompleted, SyntheticReplayCompleted };
enum class Category : std::uint8_t { Unknown, SourceCopy, DescriptorCopy, ReplayInput };
enum class Commit : std::uint8_t { DeniedBeforeCommit, Submitted, AmbiguousAfterCommit };
enum class Fault : std::uint8_t { None, Prepare, Record, Close };
enum class CompletionStatus : std::uint8_t { Pending, Confirmed, Timeout, DeviceLost, Invalid };
struct Limits { std::size_t leases=0, allocations=0, operations=0;
    std::uint64_t source_bytes=0, allocation_bytes=0; };
struct Lease { std::uint64_t identity=0; };
// Allocation inventory is independent of operations. Unused allocations are
// still held and charged; several operations may reference one allocation.
struct Allocation { std::uint64_t identity=0; Category role=Category::Unknown;
    std::uint64_t extent=0; std::size_t lease_index=0; };
struct Operation { Category category=Category::Unknown;
    std::uint64_t offset=0, bytes=0; std::size_t lease_index=0, allocation_index=0; };
struct CaptureIdentity { std::uint64_t capture=0, device_epoch=0,
    list_lifetime=0, reset_generation=0, reset_revision=0,
    draw_ordinal=0, submission_serial=0; };
struct Manifest {
    Intent intent=Intent::Unknown;
    CaptureIdentity capture{};
    std::uint64_t source_version_before=0, source_version_after=0;
    std::uint64_t device_id=0, queue_id=0, queue_epoch=0;
    std::uint64_t source_bytes=0, aggregate_bytes=0;
    bool capture_identity_known=false, source_version_equivalent=false;
    bool ownership_established=false, source_state_known=false;
    bool restoration_established=false, category_known=false, bounds_known=false;
};
// Opaque correlation inputs, never proof of native copy, replay, or restoration.
struct Identity { CaptureIdentity capture{}; std::uint64_t version=0, device=0, queue=0, queue_epoch=0;
    std::uint64_t fence=0, fence_epoch=0, value=0; };
struct Signal { std::uint64_t fence=0, epoch=0, value=0; };
struct Completion { Identity observed{}; CompletionStatus status=CompletionStatus::Invalid;
    bool independently_confirmed=false; };
// Share one domain across transactions. Domain must outlive every transaction.
// Neither refusal nor retirement replenishes its one-attempt budget.
struct Admission { std::atomic<std::size_t> attempts{0}, in_flight{0}; };
struct LeaseLedger { std::uint64_t identity=0;
    std::uint8_t prepared=0, recorded=0, closed=0, disposed=0, retired=0; };
class Transaction final {
public:
    explicit Transaction(Admission& domain) noexcept : domain_(domain) {}
    ~Transaction() noexcept { teardown(); }
    Transaction(const Transaction&)=delete; Transaction& operator=(const Transaction&)=delete;
    Transaction(Transaction&&)=delete; Transaction& operator=(Transaction&&)=delete;
    Phase phase() const noexcept { return phase_; }
    Reason reason() const noexcept { return reason_; }
    Outcome outcome() const noexcept { return outcome_; }
    std::size_t held_leases() const noexcept { return lease_count_; }
    std::size_t held_allocations() const noexcept { return allocation_count_; }
    Allocation held_allocation(std::size_t i) const noexcept {
        return i<allocation_count_ ? allocations_[i] : Allocation{};
    }
    std::size_t operation_count() const noexcept { return operation_count_; }
    std::uint64_t last_observed_value() const noexcept { return last_observed_value_; }
    bool completion_anomaly() const noexcept { return completion_anomaly_; }
    bool attempted() const noexcept { return attempted_; }
    Identity expected_identity() const noexcept { return expected_; }
    LeaseLedger ledger(std::size_t i) const noexcept { return i<max_leases ? ledger_[i] : LeaseLedger{}; }
    bool prepare(const Limits& limits, const Manifest& m, const Lease* leases,
                 std::size_t lease_count, const Allocation* allocations,
                 std::size_t allocation_count, const Operation* ops, std::size_t op_count,
                 Fault fault=Fault::None) noexcept {
        if (attempted_ || phase_!=Phase::Disabled) return false;
        attempted_=true;
        std::size_t unused=0;
        if (domain_.in_flight.load()!=0 ||
            !domain_.attempts.compare_exchange_strong(unused,1))
            return refuse(Reason::Admission);
        domain_.in_flight.store(1); admitted_=true;
        if (!limits.leases || limits.leases>max_leases ||
            !limits.allocations || limits.allocations>max_allocations || !limits.operations ||
            limits.operations>max_operations || !limits.source_bytes ||
            limits.source_bytes>max_source_bytes || !limits.allocation_bytes ||
            limits.allocation_bytes>max_allocation_bytes ||
            limits.allocation_bytes<limits.source_bytes ||
            !leases || !allocations || !ops || !lease_count || !allocation_count || !op_count ||
            lease_count>limits.leases || allocation_count>limits.allocations ||
            op_count>limits.operations)
            return refuse(Reason::Capacity);
        if (!m.capture_identity_known || !m.source_version_equivalent ||
            !m.ownership_established || !m.source_state_known ||
            !m.restoration_established || !m.category_known || !m.bounds_known ||
            !m.capture.capture || !m.capture.device_epoch ||
            !m.capture.list_lifetime || !m.capture.reset_generation ||
            !m.capture.reset_revision || !m.capture.draw_ordinal ||
            !m.capture.submission_serial ||
            !m.source_version_before || !m.source_version_after ||
            !m.device_id || !m.queue_id || !m.queue_epoch ||
            (m.intent!=Intent::Copy && m.intent!=Intent::Replay))
            return refuse(Reason::MissingFact);
        if (m.source_version_before!=m.source_version_after)
            return refuse(Reason::IdentityMismatch);
        for (std::size_t i=0;i<lease_count;++i) {
            if (!leases[i].identity) return refuse(Reason::MissingFact);
            for (std::size_t j=0;j<i;++j)
                if (leases[j].identity==leases[i].identity) return refuse(Reason::IdentityMismatch);
        }
        std::uint64_t source=0, aggregate=0;
        for (std::size_t i=0;i<allocation_count;++i) {
            const auto& a=allocations[i];
            if (!a.identity) return refuse(Reason::MissingFact);
            for (std::size_t j=0;j<i;++j)
                if (allocations[j].identity==a.identity) return refuse(Reason::IdentityMismatch);
            if ((m.intent==Intent::Copy && a.role!=Category::SourceCopy &&
                 a.role!=Category::DescriptorCopy) ||
                (m.intent==Intent::Replay && a.role!=Category::ReplayInput))
                return refuse(Reason::InvalidCategory);
            if (!a.extent || a.lease_index>=lease_count)
                return refuse(Reason::InvalidBounds);
            if (a.extent>limits.allocation_bytes || aggregate>limits.allocation_bytes-a.extent)
                return refuse(Reason::InvalidAggregate);
            aggregate+=a.extent;
            if (a.role==Category::SourceCopy) {
                if (a.extent>limits.source_bytes || source>limits.source_bytes-a.extent)
                    return refuse(Reason::InvalidAggregate);
                source+=a.extent;
            }
        }
        for (std::size_t i=0;i<op_count;++i) {
            const auto& op=ops[i];
            if (op.allocation_index>=allocation_count || op.lease_index>=lease_count)
                return refuse(Reason::InvalidBounds);
            const auto& a=allocations[op.allocation_index];
            if (op.category!=a.role) return refuse(Reason::InvalidCategory);
            if (op.lease_index!=a.lease_index || !op.bytes ||
                op.offset>a.extent || op.bytes>a.extent-op.offset)
                return refuse(Reason::InvalidBounds);
        }
        if (m.source_bytes!=source || m.aggregate_bytes!=aggregate)
            return refuse(Reason::InvalidAggregate);
        for (std::size_t i=0;i<lease_count;++i) {
            leases_[i]=leases[i]; ledger_[i].identity=leases[i].identity;
            ledger_[i].prepared=1;
        }
        for (std::size_t i=0;i<allocation_count;++i) allocations_[i]=allocations[i];
        for (std::size_t i=0;i<op_count;++i) operations_[i]=ops[i];
        lease_count_=lease_count; allocation_count_=allocation_count;
        operation_count_=op_count; intent_=m.intent;
        // Prepared identity is frozen. Submit can append only a fence tuple.
        expected_={m.capture,m.source_version_before,m.device_id,
                   m.queue_id,m.queue_epoch,0,0,0};
        if (fault==Fault::Prepare) return refuse(Reason::PreCommitFailure);
        phase_=Phase::Prepared; return true;
    }
    bool record(Fault fault=Fault::None) noexcept {
        if (phase_!=Phase::Prepared || recorded_) return false;
        if (fault==Fault::Record) return refuse(Reason::PreCommitFailure);
        for (std::size_t i=0;i<lease_count_;++i) ledger_[i].recorded=1;
        recorded_=true; return true;
    }
    bool close(Fault fault=Fault::None) noexcept {
        if (phase_!=Phase::Prepared || !recorded_ || closed_) return false;
        if (fault==Fault::Close) return refuse(Reason::PreCommitFailure);
        for (std::size_t i=0;i<lease_count_;++i) ledger_[i].closed=1;
        closed_=true; return true;
    }
    bool submit(Commit result, Signal signal) noexcept {
        if (phase_!=Phase::Prepared || !closed_) return false;
        if (result==Commit::DeniedBeforeCommit) return refuse(Reason::PreCommitFailure);
        // A bad signal after commit is ambiguous: retain every lease.
        if (result!=Commit::Submitted || !signal.fence || !signal.epoch ||
            !signal.value || signal.value==UINT64_MAX)
            return quarantine(Reason::AmbiguousCommit);
        expected_.fence=signal.fence; expected_.fence_epoch=signal.epoch;
        expected_.value=signal.value; phase_=Phase::Submitted; return true;
    }
    bool complete(const Completion& result) noexcept {
        if (phase_!=Phase::Submitted) return false;
        if (result.status==CompletionStatus::Timeout)
            return quarantine(Reason::CompletionTimeout);
        if (result.status==CompletionStatus::DeviceLost)
            return quarantine(Reason::CompletionDeviceLost);
        if (result.status==CompletionStatus::Invalid)
            return quarantine(Reason::CompletionInvalid);
        if (result.status!=CompletionStatus::Confirmed &&
            result.status!=CompletionStatus::Pending)
            return quarantine(Reason::CompletionInvalid);
        if (result.status==CompletionStatus::Pending) {
            reason_=Reason::CompletionPending; return false;
        }
        if (!result.independently_confirmed) {
            reason_=Reason::CompletionUnconfirmed; return false;
        }
        const auto& a=result.observed; const auto& b=expected_;
        if (a.capture.capture!=b.capture.capture ||
            a.capture.device_epoch!=b.capture.device_epoch ||
            a.capture.list_lifetime!=b.capture.list_lifetime ||
            a.capture.reset_generation!=b.capture.reset_generation ||
            a.capture.reset_revision!=b.capture.reset_revision ||
            a.capture.draw_ordinal!=b.capture.draw_ordinal ||
            a.capture.submission_serial!=b.capture.submission_serial ||
            a.version!=b.version || a.device!=b.device ||
            a.queue!=b.queue || a.fence!=b.fence) {
            return quarantine(Reason::CompletionIdentity);
        }
        if (a.queue_epoch!=b.queue_epoch || a.fence_epoch!=b.fence_epoch) {
            return quarantine(Reason::CompletionEpoch);
        }
        if (a.value==UINT64_MAX) return quarantine(Reason::CompletionInvalid);
        if (a.value<last_observed_value_) {
            completion_anomaly_=true; return quarantine(Reason::CompletionRegressed);
        }
        last_observed_value_=a.value;
        if (a.value>b.value) {
            completion_anomaly_=true; return quarantine(Reason::CompletionAbove);
        }
        if (a.value<b.value) { reason_=Reason::CompletionBelow; return false; }
        reason_=Reason::None; phase_=Phase::Completed;
        outcome_=intent_==Intent::Copy ? Outcome::SyntheticCopyCompleted
                                       : Outcome::SyntheticReplayCompleted;
        return true;
    }
    bool fail_after_commit() noexcept {
        if (phase_!=Phase::Submitted && phase_!=Phase::Completed) return false;
        outcome_=Outcome::None; return quarantine(Reason::AmbiguousCommit);
    }
    bool retire() noexcept {
        if (phase_!=Phase::Completed) return false;
        for (std::size_t i=0;i<lease_count_;++i) {
            ledger_[i].retired=1; leases_[i]={};
        }
        for (std::size_t i=0;i<operation_count_;++i) operations_[i]={};
        for (std::size_t i=0;i<allocation_count_;++i) allocations_[i]={};
        lease_count_=allocation_count_=operation_count_=0; release_admission();
        phase_=Phase::Retired; return true;
    }
    void teardown() noexcept {
        if (phase_==Phase::Prepared) (void)refuse(Reason::TeardownBeforeCommit);
        else if (phase_==Phase::Submitted || phase_==Phase::Completed) {
            outcome_=Outcome::None; (void)quarantine(Reason::TeardownAfterCommit);
        }
        // Quarantine stays held; destruction performs no native disposal.
    }
private:
    void release_admission() noexcept {
        if (admitted_) { domain_.in_flight.store(0); admitted_=false; }
    }
    bool refuse(Reason why) noexcept {
        phase_=Phase::Refused; reason_=why;
        for (std::size_t i=0;i<lease_count_;++i) {
            if (!ledger_[i].disposed) ledger_[i].disposed=1;
            leases_[i]={};
        }
        for (std::size_t i=0;i<operation_count_;++i) operations_[i]={};
        for (std::size_t i=0;i<allocation_count_;++i) allocations_[i]={};
        lease_count_=allocation_count_=operation_count_=0; release_admission(); return false;
    }
    bool quarantine(Reason why) noexcept {
        phase_=Phase::Quarantined; reason_=why; return false;
    }
    Admission& domain_;
    Phase phase_=Phase::Disabled; Reason reason_=Reason::None;
    Outcome outcome_=Outcome::None; Intent intent_=Intent::Unknown;
    bool attempted_=false, admitted_=false, recorded_=false, closed_=false;
    std::size_t lease_count_=0, allocation_count_=0, operation_count_=0;
    std::uint64_t last_observed_value_=0;
    bool completion_anomaly_=false;
    Identity expected_{};
    Lease leases_[max_leases]{}; Allocation allocations_[max_allocations]{};
    Operation operations_[max_operations]{};
    LeaseLedger ledger_[max_leases]{};
};
constexpr const char* diagnostic(Reason r) noexcept {
    switch(r) {
    case Reason::None:return "none";
    case Reason::MissingFact:return "missing fact";
    case Reason::IdentityMismatch:return "identity mismatch";
    case Reason::InvalidCategory:return "invalid category";
    case Reason::InvalidBounds:return "invalid bounds";
    case Reason::InvalidAggregate:return "invalid aggregate";
    case Reason::Capacity:return "capacity";
    case Reason::Admission:return "admission";
    case Reason::PreCommitFailure:return "pre-commit failure";
    case Reason::AmbiguousCommit:return "ambiguous commit";
    case Reason::CompletionIdentity:return "completion identity";
    case Reason::CompletionEpoch:return "completion epoch";
    case Reason::CompletionBelow:return "completion below";
    case Reason::CompletionAbove:return "completion above";
    case Reason::CompletionRegressed:return "completion regressed";
    case Reason::CompletionPending:return "completion pending";
    case Reason::CompletionUnconfirmed:return "completion unconfirmed";
    case Reason::CompletionTimeout:return "completion timeout";
    case Reason::CompletionDeviceLost:return "completion device lost";
    case Reason::CompletionInvalid:return "completion invalid";
    case Reason::TeardownBeforeCommit:return "teardown before commit";
    case Reason::TeardownAfterCommit:return "teardown after commit";
    }
    return "unknown";
}
} // namespace acz::gpu_transaction
