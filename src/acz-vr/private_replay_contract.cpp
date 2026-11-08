#include "private_replay_contract.h"

namespace acz::private_replay {
namespace {
constexpr size_t max_test_bytes=64u*1024u;
bool present(const Witness& value) noexcept {
    return value.identity && value.revision;
}
bool private_immutable(const Witness& value) noexcept {
    return value.private_owned && value.immutable;
}
Refusal validate(const Draw& draw) noexcept {
    if(!draw.selected_draw) return Refusal::MissingSelection;
    if(!draw.submission) return Refusal::MissingSubmission;
    const Witness witnesses[]={draw.pso,draw.root,draw.ia,draw.descriptors,
        draw.textures,draw.samplers,draw.output};
    for(const auto& witness:witnesses) if(!present(witness)) return Refusal::MissingState;
    for(const auto& witness:witnesses) if(witness.submission!=draw.submission) return Refusal::StaleState;
    for(size_t left=0;left<sizeof(witnesses)/sizeof(*witnesses);++left)
        for(size_t right=left+1;right<sizeof(witnesses)/sizeof(*witnesses);++right)
            if(witnesses[left].identity==witnesses[right].identity) return Refusal::AliasedState;
    for(const auto& witness:witnesses) if(!witness.private_owned) return Refusal::NonPrivateState;
    for(const auto& witness:witnesses) if(!witness.immutable) return Refusal::MutableState;
    if(draw.test_bytes.empty()) return Refusal::EmptyInput;
    if(draw.test_bytes.size()>max_test_bytes) return Refusal::OversizeInput;
    return Refusal::None;
}
}

Result replay(const Draw& draw) noexcept {
    Result result;
    result.refusal=validate(draw);
    if(result.refusal!=Refusal::None) return result;
    try {
        // Copying test-owned input is the entire fake replay. No mutable target
        // reference escapes and no state is changed before publication.
        result.private_bytes=draw.test_bytes;
    } catch(...) {
        result.private_bytes.clear();
        result.refusal=Refusal::OversizeInput;
    }
    return result;
}

const char* refusal_text(Refusal value) noexcept {
    switch(value) {
    case Refusal::None:return "";
    case Refusal::MissingSelection:return "selected draw missing";
    case Refusal::MissingSubmission:return "submission missing";
    case Refusal::MissingState:return "replay state missing";
    case Refusal::StaleState:return "replay state is stale";
    case Refusal::AliasedState:return "replay state aliases another role";
    case Refusal::NonPrivateState:return "replay state is not private";
    case Refusal::MutableState:return "replay state is mutable";
    case Refusal::EmptyInput:return "test bytes missing";
    case Refusal::OversizeInput:return "test bytes exceed private replay budget";
    }
    return "unknown replay refusal";
}
} // namespace acz::private_replay
