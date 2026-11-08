#include "fake_l3_fault_matrix.h"
namespace acz::fake_l3_fault {
void Session::quarantine(Fault reason) noexcept {
    refusal_=reason;output_published_=false;state_=State::Quarantined;
}
bool Session::arm(uint64_t identity,uint64_t submission) noexcept {
    // Retired fake attempts may consume further capacity until shutdown.  This
    // makes the bounded-attempt policy testable without publishing output.
    if(shutdown_ || (state_!=State::Empty && state_!=State::Retired) || !identity || !submission) {
        quarantine(Fault::Reentry);return false;
    }
    if(!cap_ || used_>=cap_) { quarantine(Fault::CapExceeded);return false; }
    ++used_;identity_=identity;submission_=submission;state_=State::Armed;return true;
}
bool Session::complete(uint64_t identity,uint64_t submission) noexcept {
    if(state_!=State::Armed || identity!=identity_ || submission!=submission_) { quarantine(Fault::AmbiguousSubmission);return false; }
    // Completion is a private fake success; it intentionally publishes no output.
    state_=State::Retired;return true;
}
void Session::fault(Fault reason) noexcept {
    if(reason==Fault::None) { quarantine(Fault::AmbiguousSubmission);return; }
    quarantine(reason);
}
bool Session::shutdown() noexcept {
    if(state_==State::Armed) { quarantine(Fault::InFlightTeardown);return false; }
    if(state_==State::Quarantined) return false;
    shutdown_=true;state_=State::Retired;return true;
}
} // namespace acz::fake_l3_fault
