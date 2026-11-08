#include "draw_capture.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace acz::capture {
Id new_lifetime_identity() {
    static std::atomic<Id> next{1};
    Id value=next.load(std::memory_order_relaxed);
    while(value!=UINT64_MAX) {
        if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) return value;
    }
    return 0; // Saturate permanently: no identity may ever be recycled.
}
State::State():list_identity(new_lifetime_identity()) {}
void State::reset() {
    const auto next=generation+1;
    const auto identity=list_identity;
    *this=State{}; generation=next; list_identity=identity; // wrap rejected by freeze
}
bool State::compatible_root() const {
    return pipeline.identity && root_id && root_identity &&
           pipeline.explicit_root_identity==root_identity && root && root->valid();
}
void State::set_root(Id id, std::optional<metadata::RootBindingMetadata> layout, Id identity) {
    ++revision;
    if(root_id!=id || root_identity!=identity) roots.clear();
    root_id=id; root_identity=identity; root=std::move(layout);
}
void State::set_heaps(std::vector<Id> ids) {
    ++revision;
    if(heaps!=ids) {
        for(auto it=roots.begin();it!=roots.end();) {
            if(it->second.kind==metadata::ParameterKind::table) it=roots.erase(it); else ++it;
        }
    }
    heaps=std::move(ids);
}
void State::set_value(uint32_t parameter, metadata::ParameterKind kind, uint64_t value) {
    ++revision;
    if(parameter>=64) { tracking_complete=false; if(!tracking_gap)tracking_gap="rejected:root parameter out of range or invalid constants"; return; }
    roots[parameter]=RootValue{kind,value,{},{}};
}
void State::set_constants(uint32_t parameter, uint32_t offset, const uint32_t* data, uint32_t count) {
    ++revision;
    if(parameter>=64 || offset>64 || count>64-offset || (count && !data)) { tracking_complete=false; if(!tracking_gap)tracking_gap="rejected:root parameter out of range or invalid constants"; return; }
    auto& v=roots[parameter];
    if(v.kind!=metadata::ParameterKind::constants) v=RootValue{};
    v.kind=metadata::ParameterKind::constants;
    v.constants.resize(std::max<size_t>(v.constants.size(),offset+count));
    v.written.resize(v.constants.size());
    for(uint32_t i=0;i<count;++i) {v.constants[offset+i]=data[i]; v.written[offset+i]=true;}
}
namespace {
bool add(uint64_t a,uint64_t b,uint64_t& r) { if(b>UINT64_MAX-a)return false;r=a+b;return true; }
bool mul(uint64_t a,uint64_t b,uint64_t& r) { if(a && b>UINT64_MAX/a)return false;r=a*b;return true; }
void require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
}
Result freeze(const State& state,const Command& cmd,const Inputs& in) {
    try {
        require(state.list_identity && state.generation && state.tracking_complete,"incomplete command-list tracking");
        require(in.list_identity==state.list_identity,"wrong command-list lifetime");
        require(in.generation==state.generation && in.revision==state.revision,"stale capture point");
        require(state.pipeline.drawcpt,"unidentified cockpit pipeline");
        require(state.compatible_root(),"PSO and bound root compatibility unknown or unsupported");
        require(state.pipeline.requirements_proven,"all-stage resource requirements unproven");
        require(state.root_id && state.root && state.root->valid(),"unknown or unsupported root layout");
        for(const auto& p:state.root->layout().parameters) require(p.ranges.size()<=64,"root range metadata exceeds bound");
        require(cmd.count && cmd.instances && cmd.instances<=4096,"unsupported empty or oversized draw");
        require(in.indexing_proven && in.srt_indices.size()==cmd.instances,"shader element indexing unproven");
        require(in.resources.size()<=64 && in.tables.size()<=64 && state.pipeline.resources.size()<=64,
                "capture exceeds resource bound");
        require(state.roots.size()<=64 && state.vertices.size()<=32 && state.heaps.size()<=2 &&
                state.pipeline.vertex_slots.size()<=32 && in.vertex_access.size()<=32,"capture exceeds state bound");
        for(const auto& v:state.roots) require(v.first<64 && v.second.constants.size()<=64 && v.second.written.size()<=64,
                                              "root state exceeds bound");
        auto out=std::make_shared<Snapshot>(); out->state=state;out->command=cmd;
        size_t total=0;
        auto copy=[&](const BufferView& view)->const std::vector<uint8_t>& {
            require(view.resource!=0,"null resource");
            const ReadableCopy* found=nullptr;
            for(const auto& r:in.resources) if(r.resource==view.resource) {
                require(!found,"ambiguous resource copy"); found=&r;
            }
            require(found && found->cpu_readable && found->synchronized,"missing safe CPU-readable copy seam");
            require(found->list_identity==state.list_identity,"wrong resource-copy command-list lifetime");
            require(found->generation==state.generation && found->revision==state.revision,"stale resource copy");
            require(found->bytes && found->resource_size==found->byte_length,"partial or unmapped resource copy");
            uint64_t end;
            require(add(view.offset,view.size,end) && end<=found->resource_size,"resource view out of bounds");
            auto existing=out->resources.find(view.resource);
            if(existing!=out->resources.end()) return existing->second;
            require(found->byte_length<=max_copy_bytes-total,"capture exceeds byte bound");
            require(found->byte_length<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(found->bytes),"source address overflow");
            total+=found->byte_length;
            auto& bytes=out->resources[view.resource];bytes.resize(found->byte_length);
            std::memcpy(bytes.data(),found->bytes,bytes.size());return bytes;
        };
        auto resolve=[&](const Requirement& req)->std::optional<Descriptor> {
            const auto binding=state.root->resolve(req.kind,req.reg,req.space,req.stage);
            require(binding.valid,"required root binding unresolved");
            auto v=state.roots.find(binding.root_index);
            require(v!=state.roots.end() && v->second.kind==binding.parameter_kind,"required root value unset or wrong kind");
            if(binding.parameter_kind==metadata::ParameterKind::constants) return {}; // Owned/validated in state below.
            require(v->second.value!=0,"null root address or descriptor handle");
            if(binding.parameter_kind!=metadata::ParameterKind::table) {
                auto d=in.root_descriptors.find(v->second.value);
                require(d!=in.root_descriptors.end(),"root address metadata unavailable");
                require(d->second.kind==req.kind,"root descriptor kind mismatch");return d->second;
            }
            const TableCopy* table=nullptr;
            for(const auto& t:in.tables) if(t.base==v->second.value &&
                std::find(state.heaps.begin(),state.heaps.end(),t.heap)!=state.heaps.end()) {
                require(!table,"ambiguous descriptor table");table=&t;
            }
            require(table && binding.table_offset<table->descriptors.size(),"unknown heap or descriptor table bounds");
            require(table->list_identity==state.list_identity,"wrong descriptor command-list lifetime");
            require(table->generation==state.generation && table->revision==state.revision,"stale descriptor metadata");
            const auto& d=table->descriptors[binding.table_offset];
            require(d.kind==req.kind,"table descriptor kind mismatch");return d;
        };
        bool srt_required=false;
        Descriptor srt_desc;
        for(const auto& r:state.pipeline.resources) {
            require(r.kind==metadata::BindingKind::srv || r.kind==metadata::BindingKind::cbv,
                    "writable or sampler resource capture unsupported");
            const auto resolved=resolve(r);if(!resolved) continue;
            const auto d=*resolved;require(d.buffer,"texture resource capture unsupported");copy(d.view);
            out->bindings.push_back({r,d});
            if(r.kind==metadata::BindingKind::srv && r.reg==10 && r.space==0 && r.stage==metadata::ShaderStage::geometry) {
                require(!srt_required,"duplicate SrtData requirement");srt_required=true;srt_desc=d;
            }
        }
        require(srt_required,"SrtData absent from complete manifest");
        // Every root constant is required conservatively; no uninitialized bytes.
        const auto& layout=state.root->layout();
        for(size_t i=0;i<layout.parameters.size();++i) if(layout.parameters[i].kind==metadata::ParameterKind::constants) {
            auto v=state.roots.find(static_cast<uint32_t>(i));auto n=layout.parameters[i].constant_count;
            require(v!=state.roots.end() && v->second.kind==metadata::ParameterKind::constants &&
                    v->second.constants.size()>=n && v->second.written.size()>=n,
                    "root constants incomplete");
            for(uint32_t j=0;j<n;++j) require(v->second.written[j],"root constants unwritten");
        }
        require(state.pipeline.vertex_slots.size()<=32,"too many vertex slots");
        require(in.vertex_fetch_proven && in.vertex_access.size()==state.pipeline.vertex_slots.size(),
                "vertex and instance fetch bounds unproven");
        for(auto slot:state.pipeline.vertex_slots) {
            auto v=state.vertices.find(slot);require(v!=state.vertices.end() && v->second.stride,"vertex view missing");
            const Inputs::VertexAccess* access=nullptr;
            for(const auto& a:in.vertex_access) if(a.slot==slot) {require(!access,"duplicate vertex access");access=&a;}
            uint64_t end=0;
            require(access && access->length && add(access->offset,access->length,end) && end<=v->second.size,
                    "vertex or instance fetch out of bounds");
            copy(v->second); // Entire validated view/resource; do not guess vertex fetch semantics.
            out->vertex_access.push_back(*access);
        }
        if(cmd.indexed) {
            require(state.index && (state.index->stride==2 || state.index->stride==4),"index view missing or unsupported format");
            uint64_t end,bytes;
            require(add(cmd.start,cmd.count,end) && mul(end,state.index->stride,bytes) && bytes<=state.index->size,"index range out of bounds");
            copy(*state.index);
        }
        require(srt_desc.structured && srt_desc.view.stride>=srt::declared_size &&
                srt_desc.view.stride<=2048 && srt_desc.view.stride%4==0,"SrtData stride unknown or incomplete");
        uint64_t descriptor_end,descriptor_bytes;
        require(add(srt_desc.first_element,srt_desc.elements,descriptor_end) &&
                mul(descriptor_end,srt_desc.view.stride,descriptor_bytes) && descriptor_bytes<=srt_desc.view.size,
                "SrtData descriptor bounds invalid");
        const auto& source=out->resources.at(srt_desc.view.resource);
        for(auto index:in.srt_indices) {
            uint64_t element=0,offset=0,end=0;
            require(index<srt_desc.elements && add(srt_desc.first_element,index,element) &&
                mul(element,srt_desc.view.stride,offset) && add(offset,srt_desc.view.offset,offset) &&
                add(offset,srt_desc.view.stride,end) && end<=source.size(),"SrtData element out of bounds");
            require(srt_desc.view.stride<=max_copy_bytes-total,"capture exceeds element byte bound");total+=srt_desc.view.stride;
            out->srt_elements.emplace_back(source.begin()+static_cast<size_t>(offset),source.begin()+static_cast<size_t>(end));
        }
        out->cpu_data_complete=true;return {std::move(out),{}};
    } catch(const std::exception& e) {return {{},e.what()};}
      catch(...) {return {{},"capture allocation failed"};}
}
srt::Result eye_payload(const Snapshot& s,size_t instance,const srt::Matrix& first,const srt::Matrix& second) {
    if(!s.cpu_data_complete || instance>=s.srt_elements.size()) return {srt::Error::inconsistent_length,{}};
    const auto& bytes=s.srt_elements[instance];
    return srt::replace_matrices(bytes.data(),bytes.size(),bytes.size(),first,second);
}
} // namespace acz::capture
