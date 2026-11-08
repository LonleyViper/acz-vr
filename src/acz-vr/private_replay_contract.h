#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acz::private_replay {
// CPU-only contract for tests. These are opaque injected witnesses, never D3D12
// objects, descriptors, GPU addresses, or observations of a game draw.
struct Witness {
    uint64_t identity=0;
    uint64_t revision=0;
    uint64_t submission=0;
    bool private_owned=false;
    bool immutable=false;
};

struct Draw {
    uint64_t selected_draw=0;
    uint64_t submission=0;
    Witness pso{};
    Witness root{};
    Witness ia{};
    Witness descriptors{};
    Witness textures{};
    Witness samplers{};
    Witness output{};
    std::vector<uint8_t> test_bytes;
};

enum class Refusal : uint8_t {
    None,
    MissingSelection,
    MissingSubmission,
    MissingState,
    StaleState,
    AliasedState,
    NonPrivateState,
    MutableState,
    EmptyInput,
    OversizeInput,
};

struct Result {
    std::vector<uint8_t> private_bytes;
    Refusal refusal=Refusal::None;
    explicit operator bool() const noexcept { return refusal==Refusal::None; }
};

// Produces only a private CPU byte vector. It neither changes Draw nor accepts
// a game/native/GPU identity; an absent, mutable, or non-private witness fails
// before any output is published.
Result replay(const Draw&) noexcept;
const char* refusal_text(Refusal) noexcept;
} // namespace acz::private_replay
