#include "private_replay_contract.h"
#include <cstdio>
#include <cstdlib>

using namespace acz::private_replay;
namespace {
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)

Witness witness(uint64_t id) { return {id,id+100,2,true,true}; }
Draw complete() {
    Draw draw;
    draw.selected_draw=1;draw.submission=2;
    draw.pso=witness(3);draw.root=witness(4);draw.ia=witness(5);
    draw.descriptors=witness(6);draw.textures=witness(7);draw.samplers=witness(8);
    draw.output=witness(9);draw.test_bytes={0x4c,0x33,0x45,0x2d};
    return draw;
}
void accepted_private_copy() {
    auto draw=complete();const auto original=draw.test_bytes;auto result=replay(draw);
    CHECK(result&&result.refusal==Refusal::None);CHECK(result.private_bytes==original);
    CHECK(result.private_bytes.data()!=draw.test_bytes.data());
    draw.test_bytes[0]=0;CHECK(result.private_bytes==original);
}
void refusal_matrix() {
    auto rejected=[](Draw draw,Refusal refusal) {
        const auto before=draw.test_bytes;auto result=replay(draw);
        CHECK(!result&&result.refusal==refusal);CHECK(result.private_bytes.empty());
        CHECK(draw.test_bytes==before);CHECK(refusal_text(refusal)[0]);
    };
    auto draw=complete();draw.selected_draw=0;rejected(draw,Refusal::MissingSelection);
    draw=complete();draw.submission=0;rejected(draw,Refusal::MissingSubmission);
    draw=complete();draw.pso.identity=0;rejected(draw,Refusal::MissingState);
    draw=complete();draw.samplers.submission=3;rejected(draw,Refusal::StaleState);
    draw=complete();draw.root.identity=draw.pso.identity;rejected(draw,Refusal::AliasedState);
    draw=complete();draw.output.private_owned=false;rejected(draw,Refusal::NonPrivateState);
    draw=complete();draw.textures.immutable=false;rejected(draw,Refusal::MutableState);
    draw=complete();draw.test_bytes.clear();rejected(draw,Refusal::EmptyInput);
    draw=complete();draw.test_bytes.resize(64u*1024u+1u);rejected(draw,Refusal::OversizeInput);
}
}
int main() {
    accepted_private_copy();refusal_matrix();
    std::printf("%u fake private replay checks passed; no native/GPU/runtime claim\n",checks);
}
