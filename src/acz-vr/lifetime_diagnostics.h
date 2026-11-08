#pragma once
#include <cstdint>

namespace acz::diagnostics {
// Diagnostic observations only. Unknown is distinct from an observed failure;
// these masks must never feed capture eligibility. Serialized as known/value.
enum LifetimeCondition : uint32_t {
    stable_qi=1u<<0, round_trip=1u<<1,
    object_base=1u<<2, object_canonical=1u<<3,
    child_base=1u<<4, child_canonical=1u<<5,
    pageable_base=1u<<6, pageable_canonical=1u<<7,
    base_covered=1u<<8, canonical_covered=1u<<9,
    base_owner=1u<<10, canonical_owner=1u<<11,
    base_pending=1u<<12, canonical_pending=1u<<13,
    split_address=1u<<14, dependency_accepted=1u<<15,
    dependency_before=1u<<16, distinct_event=1u<<17,
    supported_output=1u<<18, base_install=1u<<19,
    canonical_install=1u<<20, unique_routing=1u<<21, pinned=1u<<22
};
static_assert(pinned<0x1000000u); // each mask fits six hex digits
struct LifetimeConditions {
    uint32_t known=0,value=0;
    void set(uint32_t bit,bool pass) noexcept {
        known|=bit;value=(value&~bit)|(pass?bit:0);
    }
};
}
