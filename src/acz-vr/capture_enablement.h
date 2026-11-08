#pragma once
#include "native_copy_provider.h"

namespace acz::capture_enablement {
// Offline metadata decision ONLY. enabled=true claims no runtime readiness,
// GPU completion, byte synchronization, replay correctness or VR. Not wired to
// hooks/callbacks. Never changes requirements_proven or snapshot readiness flags.
// All-stage manifest (State::pipeline.requirements_proven), indexing and vertex
// fetch (capture_input::Request -> capture::Inputs) are independent CALLER
// ASSERTIONS, not derived from decoded executable usage/stages/input layouts.
// Caller holds the same external serialization guard as acquire/assemble.
// State/observer are the current metadata; identities below are the independently
// retained capture-point expectation. No token proves missed native history.
struct Copy {
    const native_copy::Request* request=nullptr;
    const native_copy::Result* result=nullptr;
};
struct Inputs {
    bool opt_in=false;
    const capture::State* state=nullptr;
    const binding::NativeBindingObserver* observer=nullptr;
    const capture_input::Request* caller=nullptr;
    capture::Id root_id=0, root_identity=0, pso_identity=0, pso_root_identity=0;
    // Exactly one successful acquire result per caller-owned buffer.
    std::vector<Copy> copies;
};
struct Decision {
    bool enabled=false;
    // Static text, including allocation failure: no allocation/publication here.
    const char* reason="missing inputs";
};
Decision decide(const Inputs&) noexcept;
} // namespace acz::capture_enablement
