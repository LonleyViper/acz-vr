#pragma once
// Inert, CPU-only contract for one caller-selected synthetic draw and proposed
// copy point. No native observation, byte acquisition, readiness or GPU action.
#include <cstddef>
#include <cstdint>
#include <limits>

namespace acz::draw_provenance {
constexpr std::size_t max_resources=64, max_entries=128, max_states=128;
constexpr std::size_t max_root_words=64, max_sampler_words=128;
enum class Phase : std::uint8_t { Empty, Collecting, Checked, Refused };
enum class Result : std::uint8_t { None, SyntheticConsistent };
enum class Reason : std::uint8_t { None, Attempt, Fault, Capacity, MissingIdentity,
    IdentityMismatch, DrawMismatch, ProfileMismatch, Origin, ProofForgery,
    MissingStage, RequiredInput, DuplicateInput, UnsupportedCategory,
    Descriptor, RootLifetime, RootWords, Texture, Sampler, Vertex, Index,
    ResourceIdentity, ResourceRange, ResourceVersion, History, Writer,
    Ordering, Queue, Alias, State, Restoration };
enum class Origin : std::uint8_t { Missing, CallerAssertion, FixtureObservation,
    NativeCertified, Invalid };
enum class Stage : std::uint8_t { VS, PS, GS, HS, DS, CS, IA, Count };
enum class Kind : std::uint8_t { Unknown, StructuredBuffer, RootConstants,
    RootDescriptor, Texture, Sampler, Vertex, Index };
enum class DescriptorKind : std::uint8_t { None, Dynamic, Static };
// Synthetic descriptor-table heap category. It is deliberately not a native
// D3D12 enum: callers must supply a retained fixture/native witness rather
// than relabeling an untyped heap handle as proof.
enum class HeapClass : std::uint8_t { None, CbvSrvUav, Sampler };
enum class IndexFormat : std::uint8_t { None, UInt16, UInt32 };
// Synthetic vocabulary only. These values are not native D3D12 state bits.
enum class State : std::uint32_t { Unknown, Common, CopySource, VertexBuffer,
    IndexBuffer, ShaderResource, RenderTarget, UnorderedAccess, DepthRead,
    DepthWrite, Present };
inline constexpr bool known(State s) noexcept {
    return s>=State::Common && s<=State::Present;
}
enum class Fault : std::uint8_t { None, Begin, Finish };

struct Limits { std::size_t resources=0, entries=0, states=0;
    std::size_t root_words=0, sampler_words=0; };
struct Draw {
    bool present=false, indexed=false;
    std::uint32_t count=0, instances=0, start=0, start_instance=0;
    std::int32_t base_vertex=0;
};
struct Identity {
    std::uint64_t capture=0, device=0, device_epoch=0, list_lifetime=0;
    std::uint64_t reset_generation=0, reset_revision=0, draw_ordinal=0;
    std::uint64_t submission_serial=0, queue=0, queue_epoch=0;
    std::uint64_t pso=0, pso_lifetime=0, root=0, root_lifetime=0;
    std::uint64_t input_layout=0, profile_revision=0, all_stage_profile=0;
    Draw draw{};
};
struct Key {
    Stage stage=Stage::VS; Kind kind=Kind::Unknown;
    std::uint32_t reg=0, space=0, root_parameter=0, ia_slot=0;
};
struct Required {
    Key key{};
    std::uint64_t resource_id=0, resource_lifetime=0, resource_generation=0;
    std::uint64_t resource_version=0, offset=0, bytes=0, stride=0;
    // Resource window is an absolute retained-resource coordinate. Input offset
    // is relative to this window, so a nonzero base cannot be silently shifted.
    std::uint64_t resource_window_offset=0, resource_window_bytes=0;
    DescriptorKind descriptor=DescriptorKind::None;
    std::uint64_t heap=0, heap_lifetime=0, heap_revision=0;
    std::uint64_t descriptor_slot=0, descriptor_revision=0, descriptor_source=0;
    HeapClass heap_class=HeapClass::None;
    std::uint64_t heap_capacity=0, table_base=0, table_extent=0;
    std::uint32_t root_words=0, sampler_words=0, subresources=0;
    std::uint64_t texture_lifetime=0, texture_version=0, texture_view=0;
    std::uint64_t texture_content=0, texture_layout=0;
    std::uint32_t texture_dimension=0, texture_format=0, first_subresource=0;
    std::uint64_t sampler_identity=0, sampler_lifetime=0;
    std::uint64_t sampler_revision=0, sampler_source=0;
    IndexFormat index_format=IndexFormat::None;
    std::uint64_t index_count=0, min_index=0, max_index=0;
    std::uint64_t vertex_count=0, instance_count=0;
    std::uint32_t words[max_root_words]{}, sampler_values[max_sampler_words]{};
};
struct Input {
    Key key{}; Origin origin=Origin::Missing;
    std::uint32_t resource=UINT32_MAX; // index into the separate resource inventory
    std::uint64_t root_id=0, root_lifetime=0;
    std::uint64_t heap=0, heap_lifetime=0, heap_revision=0;
    std::uint64_t descriptor_slot=0, descriptor_revision=0, descriptor_source=0;
    DescriptorKind descriptor=DescriptorKind::None;
    HeapClass heap_class=HeapClass::None;
    std::uint64_t heap_capacity=0, table_base=0, table_extent=0;
    std::uint64_t offset=0, bytes=0, stride=0;
    std::uint32_t word_count=0, sampler_word_count=0;
    std::uint32_t words[max_root_words]{};
    std::uint32_t sampler_words[max_sampler_words]{};
    std::uint64_t texture_lifetime=0, texture_version=0, texture_view=0;
    std::uint64_t texture_content=0, texture_layout=0;
    std::uint32_t dimension=0, format=0, first_subresource=0, subresources=0;
    std::uint64_t sampler_identity=0, sampler_lifetime=0, sampler_revision=0;
    std::uint64_t sampler_source=0;
    bool sampler_immutable=false;
    IndexFormat index_format=IndexFormat::None;
    std::uint64_t first_index=0, index_count=0, min_index=0, max_index=0;
    std::uint64_t first_vertex=0, vertex_count=0;
    std::uint64_t first_instance=0, instance_count=0;
    bool index_values_known=false, index_access_complete=false;
    // Duplicate inventory entry is allowed only as an explicit identical alias.
    std::uint32_t alias_of=UINT32_MAX;
};
struct Resource {
    Origin origin=Origin::Missing;
    std::uint64_t id=0, lifetime=0, generation=0, device=0, device_epoch=0;
    std::uint64_t offset=0, bytes=0, copy_offset=0, copy_bytes=0;
    std::uint64_t draw_version=0, copy_version=0;
    std::uint64_t draw_queue=0, draw_queue_epoch=0, draw_submission=0;
    std::uint64_t copy_queue=0, copy_queue_epoch=0, copy_after_submission=0;
    std::uint64_t history_serial=0, alias_group=0;
    bool complete_history=false, writer_excluded=false, ordered=false;
    bool current_at_draw=false, current_at_copy=false, released=false;
    bool version_wrapped=false, descriptor_overwritten=false, aliased=false;
};
struct SubresourceState {
    Origin origin=Origin::Missing;
    std::uint32_t resource=UINT32_MAX, subresource=0;
    State original=State::Unknown, copy_read=State::Unknown, restored=State::Unknown;
    bool covered=false, state_known=false, restoration_known=false;
    bool conflicting=false;
};
struct Profile {
    Identity identity{};
    std::uint64_t copy_after_submission=0, copy_serial=0;
    std::uint32_t used_stages=0, absent_stages=0;
    std::size_t count=0;
    Required required[max_entries]{};
    Origin origin=Origin::Missing;
};
struct Snapshot {
    Identity identity{};
    struct CopyPoint {
        Origin origin=Origin::Missing;
        std::uint64_t device=0, device_epoch=0, queue=0, queue_epoch=0;
        std::uint64_t after_submission=0, copy_serial=0;
        bool ordered=false;
    } copy{};
    std::uint32_t used_stages=0, absent_stages=0;
    std::size_t input_count=0, resource_count=0, state_count=0;
    Input inputs[max_entries]{};
    Resource resources[max_resources]{};
    SubresourceState states[max_states]{};
    Origin origin=Origin::Missing;
    // These must remain false. A caller cannot promote this policy result.
    bool copy_proof=false, replay_proof=false, readiness=false;
    bool requirements_proven=false, cpu_data_complete=false, gpu_replay_complete=false;
};
inline constexpr bool equal(Key a,Key b) noexcept {
    return a.stage==b.stage && a.kind==b.kind && a.reg==b.reg &&
        a.space==b.space && a.root_parameter==b.root_parameter && a.ia_slot==b.ia_slot;
}
// Shared exact-bit RootConstants subset. This does not grant an origin or a
// complete Contract result; native adapters must first validate their witness.
inline bool root_words_match(const Input& x,const Required& req,std::size_t limit) noexcept {
    if(!x.word_count || x.word_count!=req.root_words || x.word_count>limit ||
       x.word_count>max_root_words || x.bytes!=x.word_count*4ull ||
       req.bytes!=x.bytes || x.offset!=req.offset ||
       x.descriptor!=DescriptorKind::None || req.descriptor!=DescriptorKind::None)return false;
    for(std::size_t i=0;i<x.word_count;++i)if(x.words[i]!=req.words[i])return false;
    return true;
}
inline bool root_views_match(const Input& a,const Input& b) noexcept {
    if(a.word_count!=b.word_count||a.word_count>max_root_words||
       a.offset!=b.offset||a.bytes!=b.bytes)return false;
    for(std::size_t i=0;i<a.word_count;++i)if(a.words[i]!=b.words[i])return false;
    return true;
}
inline constexpr bool valid(Origin o) noexcept {
    return o==Origin::CallerAssertion || o==Origin::FixtureObservation;
}
inline constexpr bool checked_add(std::uint64_t a,std::uint64_t b,std::uint64_t& out) noexcept {
    if(b>UINT64_MAX-a) return false; out=a+b; return true;
}
inline constexpr bool span(std::uint64_t offset,std::uint64_t bytes,
                           std::uint64_t extent) noexcept {
    return bytes && offset<=extent && bytes<=extent-offset;
}
inline constexpr bool same_draw(const Draw& a,const Draw& b) noexcept {
    return a.present==b.present && a.indexed==b.indexed && a.count==b.count &&
        a.instances==b.instances && a.start==b.start &&
        a.start_instance==b.start_instance && a.base_vertex==b.base_vertex;
}
inline constexpr bool same_identity(const Identity& a,const Identity& b) noexcept {
    return a.capture==b.capture && a.device==b.device && a.device_epoch==b.device_epoch &&
        a.list_lifetime==b.list_lifetime && a.reset_generation==b.reset_generation &&
        a.reset_revision==b.reset_revision && a.draw_ordinal==b.draw_ordinal &&
        a.submission_serial==b.submission_serial && a.queue==b.queue &&
        a.queue_epoch==b.queue_epoch && a.pso==b.pso && a.pso_lifetime==b.pso_lifetime &&
        a.root==b.root && a.root_lifetime==b.root_lifetime &&
        a.input_layout==b.input_layout && a.profile_revision==b.profile_revision &&
        a.all_stage_profile==b.all_stage_profile;
}
inline constexpr bool complete(const Identity& i) noexcept {
    return i.capture && i.device && i.device_epoch && i.list_lifetime &&
        i.reset_generation && i.reset_revision && i.draw_ordinal &&
        i.submission_serial && i.queue && i.queue_epoch && i.pso &&
        i.pso_lifetime && i.root && i.root_lifetime && i.input_layout &&
        i.profile_revision && i.all_stage_profile && i.draw.present;
}
inline constexpr const char* diagnostic(Reason r) noexcept {
    switch(r) {
    case Reason::None:return "none"; case Reason::Attempt:return "attempt";
    case Reason::Fault:return "fault"; case Reason::Capacity:return "capacity";
    case Reason::MissingIdentity:return "missing identity";
    case Reason::IdentityMismatch:return "identity mismatch";
    case Reason::DrawMismatch:return "draw mismatch";
    case Reason::ProfileMismatch:return "profile mismatch";
    case Reason::Origin:return "invalid origin";
    case Reason::ProofForgery:return "proof forgery";
    case Reason::MissingStage:return "stage evidence";
    case Reason::RequiredInput:return "required input";
    case Reason::DuplicateInput:return "duplicate input";
    case Reason::UnsupportedCategory:return "unsupported category";
    case Reason::Descriptor:return "descriptor";
    case Reason::RootLifetime:return "root lifetime";
    case Reason::RootWords:return "root words";
    case Reason::Texture:return "texture"; case Reason::Sampler:return "sampler";
    case Reason::Vertex:return "vertex"; case Reason::Index:return "index";
    case Reason::ResourceIdentity:return "resource identity";
    case Reason::ResourceRange:return "resource range";
    case Reason::ResourceVersion:return "resource version";
    case Reason::History:return "history"; case Reason::Writer:return "writer";
    case Reason::Ordering:return "ordering"; case Reason::Queue:return "queue";
    case Reason::Alias:return "alias"; case Reason::State:return "state";
    case Reason::Restoration:return "restoration";
    }
    return "unknown refusal";
}
class Contract final {
public:
    Phase phase() const noexcept {return phase_;}
    Result result() const noexcept {return result_;}
    Reason reason() const noexcept {return reason_;}
    bool native_proof_available() const noexcept {return false;}
    const Profile& expected() const noexcept {return expected_;}
    const Snapshot& observed() const noexcept {return observed_;}
    bool begin(const Limits& limits,const Profile& expected,const Snapshot& observed,
               Fault fault=Fault::None) noexcept {
        if(phase_!=Phase::Empty) {
            if(phase_==Phase::Collecting)refuse(Reason::Attempt);
            return false;
        }
        phase_=Phase::Collecting;
        if(fault==Fault::Begin) {refuse(Reason::Fault);return false;}
        if(!limits.resources || limits.resources>max_resources || !limits.entries ||
           limits.entries>max_entries || !limits.states || limits.states>max_states ||
           !limits.root_words || limits.root_words>max_root_words ||
           !limits.sampler_words || limits.sampler_words>max_sampler_words ||
           expected.count>limits.entries || observed.input_count>limits.entries ||
           observed.resource_count>limits.resources || observed.state_count>limits.states)
            {refuse(Reason::Capacity);return false;}
        limits_=limits;expected_=expected;observed_=observed;return true;
    }
    // A missed event, fault or overlap is irreversible for this attempt.
    void missed_event() noexcept {refuse(Reason::History);}
    void overlap() noexcept {refuse(Reason::Ordering);}
    void mutation() noexcept {refuse(Reason::ResourceVersion);}
    bool finish(Fault fault=Fault::None) noexcept {
        if(phase_!=Phase::Collecting) {if(phase_==Phase::Empty)refuse(Reason::Attempt);return false;}
        if(fault==Fault::Finish){refuse(Reason::Fault);return false;}
        const auto& e=expected_;const auto& o=observed_;
        if(!valid(e.origin)||!valid(o.origin)){refuse(Reason::Origin);return false;}
        if(o.copy_proof||o.replay_proof||o.readiness||o.requirements_proven||
           o.cpu_data_complete||o.gpu_replay_complete){refuse(Reason::ProofForgery);return false;}
        if(!complete(e.identity)||!complete(o.identity)){refuse(Reason::MissingIdentity);return false;}
        if(!same_identity(e.identity,o.identity)){refuse(Reason::IdentityMismatch);return false;}
        if(!same_draw(e.identity.draw,o.identity.draw)){refuse(Reason::DrawMismatch);return false;}
        if(!valid(o.copy.origin) || !o.copy.copy_serial ||
           o.copy.device!=o.identity.device || o.copy.device_epoch!=o.identity.device_epoch)
            {refuse(Reason::IdentityMismatch);return false;}
        if(!e.copy_serial || !e.copy_after_submission ||
           o.copy.copy_serial!=e.copy_serial ||
           o.copy.after_submission!=e.copy_after_submission)
            {refuse(Reason::IdentityMismatch);return false;}
        if(o.copy.queue!=o.identity.queue || o.copy.queue_epoch!=o.identity.queue_epoch)
            {refuse(Reason::Queue);return false;}
        if(!o.copy.ordered || o.copy.after_submission<o.identity.submission_serial)
            {refuse(Reason::Ordering);return false;}
        constexpr std::uint32_t all=(1u<<static_cast<unsigned>(Stage::Count))-1u;
        if(!e.used_stages || (e.used_stages|e.absent_stages)!=all ||
           (e.used_stages&e.absent_stages) || e.used_stages!=o.used_stages ||
           e.absent_stages!=o.absent_stages){refuse(Reason::MissingStage);return false;}
        if(!e.count || !o.input_count){refuse(Reason::RequiredInput);return false;}
        // An indexed command has a mandatory IA fetch even if the supplied
        // selected profile was malformed to omit it.
        if(e.identity.draw.indexed && e.identity.draw.count) {
            bool required_index=false;
            for(std::size_t i=0;i<e.count;++i)
                required_index|=e.required[i].key.stage==Stage::IA &&
                    e.required[i].key.kind==Kind::Index;
            if(!required_index){refuse(Reason::Index);return false;}
        }
        for(std::size_t i=0;i<e.count;++i) {
            const auto& requirement=e.required[i];const auto& k=requirement.key;
            if(k.kind==Kind::Unknown || static_cast<unsigned>(k.stage)>=static_cast<unsigned>(Stage::Count) ||
               !(e.used_stages&(1u<<static_cast<unsigned>(k.stage))))
                {refuse(Reason::ProfileMismatch);return false;}
            for(std::size_t j=0;j<i;++j)if(equal(k,e.required[j].key))
                {refuse(Reason::ProfileMismatch);return false;}
            std::size_t matches=0, first=0;
            for(std::size_t j=0;j<o.input_count;++j) if(equal(k,o.inputs[j].key)) {
                if(++matches!=1 && !same_alias(o.inputs[j],o.inputs[first],j,first))
                    {refuse(Reason::DuplicateInput);return false;}
                if(matches==1) {first=j;if(!check_input(o.inputs[j],requirement))return false;}
            }
            if(!matches){refuse(Reason::RequiredInput);return false;}
        }
        for(std::size_t j=0;j<o.input_count;++j) {
            bool found=false;for(std::size_t i=0;i<e.count;++i)found|=equal(o.inputs[j].key,e.required[i].key);
            if(!found){refuse(Reason::ProfileMismatch);return false;}
        }
        for(std::size_t i=0;i<o.resource_count;++i)if(!check_resource(i))return false;
        for(std::size_t i=0;i<o.state_count;++i)if(!check_state(i))return false;
        for(std::size_t i=0;i<o.resource_count;++i) {
            bool covered=false;
            for(std::size_t j=0;j<o.state_count;++j)covered|=o.states[j].resource==i;
            if(!covered){refuse(Reason::State);return false;}
        }
        phase_=Phase::Checked;result_=Result::SyntheticConsistent;return true;
    }
private:
    void refuse(Reason r) noexcept {if(phase_!=Phase::Refused){reason_=r;phase_=Phase::Refused;result_=Result::None;}}
    static bool same_alias(const Input& a,const Input& prior,std::size_t at,std::size_t first) noexcept {
        if(!(valid(a.origin) && a.origin==prior.origin &&
            a.word_count<=max_root_words && a.sampler_word_count<=max_sampler_words &&
            a.alias_of==first && first<at && equal(a.key,prior.key) &&
            a.resource==prior.resource && a.root_id==prior.root_id &&
            a.root_lifetime==prior.root_lifetime && a.descriptor==prior.descriptor &&
            a.descriptor_revision==prior.descriptor_revision &&
            a.descriptor_source==prior.descriptor_source &&
            a.descriptor_slot==prior.descriptor_slot &&
            a.heap==prior.heap && a.heap_lifetime==prior.heap_lifetime &&
            a.heap_revision==prior.heap_revision &&
            a.heap_class==prior.heap_class && a.heap_capacity==prior.heap_capacity &&
            a.table_base==prior.table_base && a.table_extent==prior.table_extent &&
            a.offset==prior.offset && a.bytes==prior.bytes && a.stride==prior.stride &&
            a.word_count==prior.word_count && a.sampler_word_count==prior.sampler_word_count &&
            a.texture_lifetime==prior.texture_lifetime &&
            a.texture_version==prior.texture_version && a.texture_view==prior.texture_view &&
            a.texture_content==prior.texture_content && a.texture_layout==prior.texture_layout &&
            a.dimension==prior.dimension && a.format==prior.format &&
            a.first_subresource==prior.first_subresource && a.subresources==prior.subresources &&
            a.sampler_identity==prior.sampler_identity &&
            a.sampler_lifetime==prior.sampler_lifetime &&
            a.sampler_revision==prior.sampler_revision &&
            a.sampler_source==prior.sampler_source &&
            a.sampler_immutable==prior.sampler_immutable &&
            a.index_format==prior.index_format && a.first_index==prior.first_index &&
            a.index_count==prior.index_count && a.min_index==prior.min_index &&
            a.max_index==prior.max_index && a.first_vertex==prior.first_vertex &&
            a.vertex_count==prior.vertex_count && a.first_instance==prior.first_instance &&
            a.instance_count==prior.instance_count &&
            a.index_values_known==prior.index_values_known &&
            a.index_access_complete==prior.index_access_complete))return false;
        for(std::size_t i=0;i<a.word_count;++i)if(a.words[i]!=prior.words[i])return false;
        for(std::size_t i=0;i<a.sampler_word_count;++i)
            if(a.sampler_words[i]!=prior.sampler_words[i])return false;
        return true;
    }
    bool check_input(const Input& x,const Required& req) noexcept {
        if(!valid(x.origin)){refuse(Reason::Origin);return false;}
        if(x.key.kind!=Kind::StructuredBuffer && x.key.kind!=Kind::RootConstants &&
           x.key.kind!=Kind::RootDescriptor && x.key.kind!=Kind::Texture &&
           x.key.kind!=Kind::Sampler && x.key.kind!=Kind::Vertex &&
           x.key.kind!=Kind::Index)
            {refuse(Reason::UnsupportedCategory);return false;}
        if(x.root_id!=observed_.identity.root || x.root_lifetime!=observed_.identity.root_lifetime)
            {refuse(Reason::RootLifetime);return false;}
        if(x.key.kind!=Kind::Sampler && x.key.kind!=Kind::RootConstants &&
           x.resource>=observed_.resource_count){refuse(Reason::ResourceIdentity);return false;}
        if(x.offset!=req.offset || x.bytes!=req.bytes || x.stride!=req.stride)
            {refuse(Reason::ResourceRange);return false;}
        if(x.descriptor!=req.descriptor || x.heap!=req.heap ||
           x.heap_lifetime!=req.heap_lifetime || x.heap_revision!=req.heap_revision ||
           x.descriptor_slot!=req.descriptor_slot ||
           x.descriptor_revision!=req.descriptor_revision ||
           x.descriptor_source!=req.descriptor_source || x.heap_class!=req.heap_class ||
           x.heap_capacity!=req.heap_capacity || x.table_base!=req.table_base ||
           x.table_extent!=req.table_extent)
            {refuse(Reason::Descriptor);return false;}
        if(x.key.kind!=Kind::Sampler && x.key.kind!=Kind::RootConstants) {
            const auto& resource=observed_.resources[x.resource];
            if(resource.id!=req.resource_id || resource.lifetime!=req.resource_lifetime ||
               resource.generation!=req.resource_generation)
                {refuse(Reason::ResourceIdentity);return false;}
            if(resource.draw_version!=req.resource_version)
                {refuse(Reason::ResourceVersion);return false;}
            if(!req.resource_window_bytes ||
               resource.offset!=req.resource_window_offset ||
               resource.bytes!=req.resource_window_bytes)
                {refuse(Reason::ResourceRange);return false;}
        }
        if(x.descriptor==DescriptorKind::Dynamic) {
            if(!x.heap || !x.heap_lifetime || !x.heap_revision || !x.descriptor_revision ||
               !x.descriptor_source || x.heap_class==HeapClass::None || !x.heap_capacity ||
               !x.table_extent || x.table_base>=x.heap_capacity ||
               x.table_extent>x.heap_capacity-x.table_base || x.descriptor_slot<x.table_base ||
               x.descriptor_slot-x.table_base>=x.table_extent || x.descriptor_slot>=x.heap_capacity)
                {refuse(Reason::Descriptor);return false;}
            const HeapClass expected=x.key.kind==Kind::Sampler?HeapClass::Sampler:HeapClass::CbvSrvUav;
            if(x.heap_class!=expected){refuse(Reason::Descriptor);return false;}
            for(std::size_t j=0;j<observed_.input_count;++j) {
                const auto& other=observed_.inputs[j];
                if(&other!=&x && other.descriptor==DescriptorKind::Dynamic &&
                   other.heap==x.heap && other.heap_lifetime==x.heap_lifetime &&
                   // A revision updates one retained heap; it cannot change its class.
                   other.heap_class!=x.heap_class)
                    {refuse(Reason::Descriptor);return false;}
            }
        }
        if(x.key.kind==Kind::RootConstants) {
            if(!root_words_match(x,req,limits_.root_words)){refuse(Reason::RootWords);return false;}
            // Stage views of the same root parameter share one retained value.
            for(std::size_t j=0;j<observed_.input_count;++j) {
                const auto& other=observed_.inputs[j];
                if(&other==&x || other.key.kind!=Kind::RootConstants ||
                   other.root_id!=x.root_id || other.root_lifetime!=x.root_lifetime ||
                   other.key.root_parameter!=x.key.root_parameter)continue;
                if(!root_views_match(x,other)){refuse(Reason::RootWords);return false;}
            }
        } else if(x.key.kind==Kind::Sampler) {
            if(!x.sampler_identity || !x.sampler_lifetime || !x.sampler_revision ||
               !x.sampler_source || !x.sampler_immutable || !x.sampler_word_count ||
               x.sampler_word_count!=req.sampler_words ||
               x.sampler_word_count>limits_.sampler_words ||
               (x.descriptor!=DescriptorKind::Static && x.descriptor!=DescriptorKind::Dynamic))
                {refuse(Reason::Sampler);return false;}
            if(x.sampler_identity!=req.sampler_identity ||
               x.sampler_lifetime!=req.sampler_lifetime ||
               x.sampler_revision!=req.sampler_revision ||
               x.sampler_source!=req.sampler_source)
                {refuse(Reason::Sampler);return false;}
            if(x.descriptor==DescriptorKind::Static &&
               (x.heap || x.heap_lifetime || x.heap_revision || x.descriptor_revision))
                {refuse(Reason::Sampler);return false;}
            for(std::size_t i=0;i<x.sampler_word_count;++i)
                if(x.sampler_words[i]!=req.sampler_values[i])
                    {refuse(Reason::Sampler);return false;}
        } else if(x.key.kind==Kind::Texture) {
            if(x.descriptor!=DescriptorKind::Dynamic || !x.texture_lifetime ||
               !x.texture_version || !x.dimension || !x.format || !x.subresources ||
               !x.texture_view || !x.texture_content || !x.texture_layout ||
               x.subresources!=req.subresources)
                {refuse(Reason::Texture);return false;}
            if(x.texture_lifetime!=req.texture_lifetime ||
               x.texture_version!=req.texture_version ||
               x.texture_view!=req.texture_view ||
               x.texture_content!=req.texture_content ||
               x.texture_layout!=req.texture_layout ||
               x.dimension!=req.texture_dimension || x.format!=req.texture_format ||
               x.first_subresource!=req.first_subresource)
                {refuse(Reason::Texture);return false;}
            for(std::uint64_t sub=x.first_subresource;
                sub<std::uint64_t(x.first_subresource)+x.subresources;++sub) {
                bool covered=false;
                for(std::size_t i=0;i<observed_.state_count;++i)
                    covered|=observed_.states[i].resource==x.resource &&
                             observed_.states[i].subresource==sub;
                if(!covered){refuse(Reason::State);return false;}
            }
        } else if(x.key.kind==Kind::StructuredBuffer || x.key.kind==Kind::RootDescriptor ||
                  x.key.kind==Kind::Vertex || x.key.kind==Kind::Index) {
            const auto& r=observed_.resources[x.resource];
            if(!span(x.offset,x.bytes,r.bytes) || !x.stride ||
               (x.key.kind==Kind::StructuredBuffer && x.descriptor!=DescriptorKind::Dynamic) ||
               (x.key.kind==Kind::RootDescriptor && x.descriptor!=DescriptorKind::None))
                {refuse(Reason::ResourceRange);return false;}
            if(x.key.kind==Kind::Vertex) {
                if(x.vertex_count!=req.vertex_count || x.instance_count!=req.instance_count)
                    {refuse(Reason::Vertex);return false;}
                if(!check_vertex(x))return false;
            }
            if(x.key.kind==Kind::Index) {
                if(x.index_format!=req.index_format || x.index_count!=req.index_count ||
                   x.min_index!=req.min_index || x.max_index!=req.max_index ||
                   x.vertex_count!=req.vertex_count)
                    {refuse(Reason::Index);return false;}
                if(!check_index(x))return false;
            }
        } else {refuse(Reason::UnsupportedCategory);return false;}
        return true;
    }
    bool check_vertex(const Input& x) noexcept {
        const auto& d=observed_.identity.draw;
        std::uint64_t last=0, last_instance=0;
        if(!x.vertex_count || !x.instance_count ||
           !checked_add(d.start,d.count,last) || !checked_add(d.start_instance,d.instances,last_instance) ||
           x.first_instance>d.start_instance || x.first_instance>UINT64_MAX-x.instance_count ||
           last_instance>x.first_instance+x.instance_count ||
           x.first_vertex>UINT64_MAX-x.vertex_count ||
           (!d.indexed && (x.first_vertex>d.start || last>x.first_vertex+x.vertex_count)) ||
           x.vertex_count>x.bytes/x.stride)
            {refuse(Reason::Vertex);return false;}
        return true;
    }
    bool check_index(const Input& x) noexcept {
        const auto& d=observed_.identity.draw;
        std::uint64_t last=0;
        if(!d.indexed || !x.index_values_known || !x.index_access_complete ||
           (x.index_format!=IndexFormat::UInt16 && x.index_format!=IndexFormat::UInt32) ||
           x.stride!=(x.index_format==IndexFormat::UInt16?2u:4u) ||
           !checked_add(d.start,d.count,last) || x.first_index>d.start ||
           x.first_index>UINT64_MAX-x.index_count ||
           last>x.first_index+x.index_count || x.index_count>x.bytes/x.stride ||
           x.max_index<x.min_index || !x.vertex_count ||
           x.max_index>(x.index_format==IndexFormat::UInt16?UINT16_MAX:UINT32_MAX) ||
           static_cast<std::int64_t>(x.min_index)+d.base_vertex<0 ||
           static_cast<std::uint64_t>(static_cast<std::int64_t>(x.max_index)+d.base_vertex)>=x.vertex_count)
            {refuse(Reason::Index);return false;}
        // Every required IA vertex slot must cover the indexed fetch. One
        // valid slot cannot stand in for a second, narrower required slot.
        for(std::size_t r=0;r<expected_.count;++r) {
            const auto& key=expected_.required[r].key;
            if(key.kind!=Kind::Vertex || key.stage!=Stage::IA)continue;
            bool vertex_covered=false;
            for(std::size_t i=0;i<observed_.input_count;++i) {
                const auto& v=observed_.inputs[i];
                if(!equal(v.key,key))continue;
                const std::int64_t low=static_cast<std::int64_t>(x.min_index)+d.base_vertex;
                const std::int64_t high=static_cast<std::int64_t>(x.max_index)+d.base_vertex;
                vertex_covered|=low>=0 && static_cast<std::uint64_t>(low)>=v.first_vertex &&
                    v.first_vertex<=UINT64_MAX-v.vertex_count &&
                    static_cast<std::uint64_t>(high)<v.first_vertex+v.vertex_count;
            }
            if(!vertex_covered){refuse(Reason::Index);return false;}
        }
        return true;
    }
    bool check_resource(std::size_t i) noexcept {
        const auto& r=observed_.resources[i];const auto& id=observed_.identity;
        if(!valid(r.origin)){refuse(Reason::Origin);return false;}
        if(!r.id || !r.lifetime || !r.generation || r.device!=id.device ||
           r.device_epoch!=id.device_epoch || r.released)
            {refuse(Reason::ResourceIdentity);return false;}
        if(!r.bytes || !span(r.offset,r.bytes,UINT64_MAX) ||
           r.copy_offset!=r.offset || r.copy_bytes!=r.bytes)
            {refuse(Reason::ResourceRange);return false;}
        if(!r.draw_version || r.draw_version!=r.copy_version || r.version_wrapped)
            {refuse(Reason::ResourceVersion);return false;}
        if(!r.complete_history || !r.history_serial){refuse(Reason::History);return false;}
        if(!r.writer_excluded){refuse(Reason::Writer);return false;}
        if(!r.ordered || !r.current_at_draw || !r.current_at_copy ||
           r.draw_submission!=id.submission_serial ||
           r.copy_after_submission!=observed_.copy.after_submission ||
           r.copy_after_submission<r.draw_submission)
            {refuse(Reason::Ordering);return false;}
        if(r.draw_queue!=id.queue || r.copy_queue!=id.queue ||
           r.draw_queue_epoch!=id.queue_epoch || r.copy_queue_epoch!=id.queue_epoch)
            {refuse(Reason::Queue);return false;}
        if(r.aliased || r.alias_group){refuse(Reason::Alias);return false;}
        if(r.descriptor_overwritten){refuse(Reason::Descriptor);return false;}
        for(std::size_t j=0;j<i;++j)if(r.id==observed_.resources[j].id)
            {refuse(Reason::Alias);return false;}
        return true;
    }
    bool check_state(std::size_t i) noexcept {
        const auto& s=observed_.states[i];
        if(!valid(s.origin)){refuse(Reason::Origin);return false;}
        if(s.resource>=observed_.resource_count || !s.covered || !s.state_known ||
           s.conflicting || !known(s.original) || s.copy_read!=State::CopySource ||
           !known(s.restored))
            {refuse(Reason::State);return false;}
        if(!s.restoration_known || s.restored!=s.original)
            {refuse(Reason::Restoration);return false;}
        for(std::size_t j=0;j<i;++j)if(s.resource==observed_.states[j].resource &&
                                        s.subresource==observed_.states[j].subresource)
            {refuse(Reason::State);return false;}
        return true;
    }
    Phase phase_=Phase::Empty;Result result_=Result::None;Reason reason_=Reason::None;
    Limits limits_{}; Profile expected_{}; Snapshot observed_{};
};
} // namespace acz::draw_provenance
