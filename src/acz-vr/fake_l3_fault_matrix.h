#pragma once

#include <cstdint>

namespace acz::fake_l3_fault {
// Pure state machine for test-owned records. It has no native handles and is
// not wired to a hook, queue, device, copy provider, or replay implementation.
enum class Fault : uint8_t { None, Reentry, AmbiguousSubmission, Timeout, DeviceLost, InFlightTeardown, CapExceeded };
enum class State : uint8_t { Empty, Armed, Quarantined, Retired };
class Session {
public:
    explicit Session(unsigned cap) noexcept : cap_(cap) {}
    bool arm(uint64_t identity, uint64_t submission) noexcept;
    bool complete(uint64_t identity, uint64_t submission) noexcept;
    void fault(Fault) noexcept;
    bool shutdown() noexcept;
    State state() const noexcept { return state_; }
    Fault refusal() const noexcept { return refusal_; }
    bool output_published() const noexcept { return output_published_; }
    bool native_proof_available() const noexcept { return false; }
private:
    void quarantine(Fault) noexcept;
    unsigned cap_=0, used_=0;
    uint64_t identity_=0, submission_=0;
    State state_=State::Empty;
    Fault refusal_=Fault::None;
    bool output_published_=false, shutdown_=false;
};
} // namespace acz::fake_l3_fault
