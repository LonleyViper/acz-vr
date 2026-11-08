#pragma once
// Offline injected-backend model only: no real COM/fence adapter is supplied.
// Calls require external serialization; synchronous same-thread reentry is
// supported. No caller lock across callbacks. Backends/keeper outlive adapter;
// callbacks must not destroy these objects. No native submission, locks or TLS.
#include "gpu_transaction_contract.h"
#include <cstddef>
#include <cstdint>
namespace acz::native_retirement {
constexpr std::size_t max_quarantined=256;
struct Dependency { uint64_t handle=0,identity=0,epoch=0; };
struct ComBackend {
    virtual ~ComBackend()=default;
    // true transfers exactly one reference; false transfers none. Release false
    // leaves its reference held. A future native adapter must honor this contract.
    virtual bool add_ref(uint64_t handle) noexcept=0;
    virtual bool release(uint64_t handle) noexcept=0;
};
struct FenceObservation {
    Dependency fence{};
    // Independent association with the submitted transaction and target signal.
    // Backend must NOT manufacture this by echoing the poll's expected identity.
    gpu_transaction::Identity transaction{};
    uint64_t completed=0;
    bool valid=false;
    // Private execution dependencies associated independently with this signal.
    // execution_list.identity is NOT the selected game capture.list_lifetime.
    Dependency allocator{},execution_list{};
};
struct FenceBackend {
    virtual ~FenceBackend()=default;
    virtual FenceObservation read(uint64_t handle,uint64_t epoch) noexcept=0;
};
class QuarantineKeeper final {
public:
    std::size_t count() const noexcept { return count_; }
    std::size_t available() const noexcept { return max_quarantined-reserved_; }
    Dependency entry(std::size_t i) const noexcept { return i<reserved_?entries_[i]:Dependency{}; }
    bool contains(uint64_t handle) const noexcept;
private:
    friend class OwnershipAdapter;
    // Never recycle reservations, even after retirement: bounded lifetime attempts
    // and guaranteed space to retain every reference acquired by this adapter.
    bool reserve(std::size_t& start) noexcept;
    void retain(std::size_t slot,Dependency dep) noexcept;
    Dependency entries_[max_quarantined]{};
    std::size_t reserved_=0,count_=0;
};
enum class Phase : uint8_t { Idle,Acquiring,Acquired,HandedOff,Polling,Releasing,Retired,Quarantined,Failed };
enum class FailReason : uint8_t { None,InvalidInput,BackendFailure,QuarantineCapacity,
    InvalidPhase,UnknownSignal,Reentry,IdentityMismatch,Incomplete,InvalidCompletion,Regressed };
class OwnershipAdapter final {
public:
    OwnershipAdapter(ComBackend&,FenceBackend&,QuarantineKeeper&) noexcept;
    ~OwnershipAdapter() noexcept;
    OwnershipAdapter(const OwnershipAdapter&)=delete;
    OwnershipAdapter& operator=(const OwnershipAdapter&)=delete;
    OwnershipAdapter(OwnershipAdapter&&)=delete;
    OwnershipAdapter& operator=(OwnershipAdapter&&)=delete;
    Phase phase() const noexcept { return phase_; }
    FailReason reason() const noexcept { return reason_; }
    std::size_t held() const noexcept;
    Dependency allocator() const noexcept { return deps_[0]; }
    Dependency list() const noexcept { return deps_[1]; }
    Dependency fence() const noexcept { return deps_[2]; }
    // Snapshot the PREPARED capture identity (fence tuple absent) and private
    // execution allocator/list dependencies before calling AddRef. Their binding
    // to the selected capture is independently checked in FenceObservation.
    bool acquire(const Dependency&,const Dependency&,const Dependency&,
                 const gpu_transaction::Identity& origin) noexcept;
    bool handoff(const gpu_transaction::Signal&) noexcept;
    gpu_transaction::Completion poll_completion() noexcept;
    // An exactly matching completed policy transaction AND fresh independent fence
    // completion are required. Previous polling results are not retirement permits.
    bool retire(gpu_transaction::Transaction&) noexcept;
    bool quarantine_all() noexcept;
private:
    bool busy() noexcept;
    bool fail(FailReason) noexcept;
    void quarantine_owned(FailReason) noexcept;
    bool release_owned(Phase,gpu_transaction::Transaction* transaction=nullptr) noexcept;
    ComBackend& com_; FenceBackend& fence_backend_; QuarantineKeeper& keeper_;
    Phase phase_=Phase::Idle; FailReason reason_=FailReason::None;
    Dependency deps_[3]{}; bool owned_[3]{};
    gpu_transaction::Identity expected_{};
    std::size_t reservation_=0;
    uint64_t last_observed_=0;
    bool poisoned_=false;
};
} // namespace acz::native_retirement
