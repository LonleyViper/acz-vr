#include "capture_input_provider.h"
#include <algorithm>
#include <stdexcept>

namespace acz::capture_input {
namespace {
// Error reporting must not turn allocation exhaustion into an escaping error.
template<class T> T rejected(const char* why) noexcept {
    T result;
    try { result.rejection=why; } catch(...) {}
    return result;
}
void require(bool ok, const char* why) { if(!ok) throw std::runtime_error(why); }
bool same(const capture::Descriptor& a, const capture::Descriptor& b) {
    return a.kind==b.kind && a.view.resource==b.view.resource && a.view.offset==b.view.offset &&
        a.view.size==b.view.size && a.view.stride==b.view.stride && a.first_element==b.first_element &&
        a.elements==b.elements && a.structured==b.structured && a.buffer==b.buffer;
}
void validate(const binding::NativeBindingObserver& observer, const State& state, const Request& r) {
    require(r.serialized && r.synchronized,"caller serialization/synchronization assertion missing");
    require(r.list_identity && r.list_identity==state.list_identity && r.generation &&
        r.generation==state.generation && r.revision==state.revision,"stale list lifetime/recording point");
    require(r.buffers.size()<=64 && r.descriptors.size()<=4096 && r.srt_indices.size()<=4096 &&
        r.vertex_access.size()<=32,"provider metadata budget exceeded");
    size_t bytes=0;
    for(size_t i=0;i<r.buffers.size();++i) {
        const auto& b=r.buffers[i];
        require(bool(observer.current(b.witness,state,r.device)),"stale buffer witness");
        require(b.resource.id && b.resource.generation && b.resource==b.witness.snapshot->resource &&
            observer.resource_token(b.resource.id)==b.resource,"stale/mismatched full resource token");
        require(b.bytes && !b.bytes->empty() && b.bytes->size()==b.witness.snapshot->resource_bytes,
            "missing/partial owned CPU buffer");
        require(b.bytes->size()<=capture::max_copy_bytes-bytes,"provider byte budget exceeded");
        bytes+=b.bytes->size();
        for(size_t j=0;j<i;++j) require(r.buffers[j].resource.id!=b.resource.id,"duplicate owned resource");
    }
    for(const auto& d:r.descriptors) {
        require(d.parameter<64 && d.table_offset<64,"unsupported descriptor parameter/offset");
        require(bool(observer.current(d.metadata,state,r.device)),"stale descriptor provenance");
        const auto& m=*d.metadata.snapshot;
        auto root=state.roots.find(d.parameter);
        require(root!=state.roots.end(),"descriptor root parameter absent");
        binding::NativeResolution live;
        if(root->second.kind==metadata::ParameterKind::table) {
            live=observer.table(state,d.parameter,m.heap.id,d.table_offset,r.device);
            require(live && live.snapshot==d.metadata.snapshot,"descriptor is not the bound table slot");
        } else {
            require(!d.table_offset && !m.heap.id,"unsupported root descriptor provenance");
            const auto& v=m.descriptor;
            require(v.elements<=UINT32_MAX && v.view.size<=UINT32_MAX,"root provider layout exceeds SDK bounds");
            live=observer.root(state,d.parameter,v.kind==metadata::BindingKind::cbv?uint32_t(v.view.size):0,
                uint32_t(v.elements),v.view.stride,r.device);
            require(live && live.snapshot->resource==m.resource && live.snapshot->resource_bytes==m.resource_bytes &&
                same(live.snapshot->descriptor,v),"descriptor is not the bound root address/layout");
        }
        require(m.descriptor.view.resource==m.resource.id,"descriptor resource token mismatch");
        auto b=std::find_if(r.buffers.begin(),r.buffers.end(),[&](const Buffer& v){return v.resource==m.resource;});
        require(b!=r.buffers.end() && b->bytes->size()==m.resource_bytes,"descriptor lacks exact owned resource lifetime");
    }
}
}
Result assemble(const binding::NativeBindingObserver& observer, const State& state, const Request& request) {
    try {
        validate(observer,state,request);
        auto lease=std::make_shared<Lease>();
        lease->owners_=request; // Retain exact storage, snapshots and lifetime tokens.
        auto& in=lease->inputs_;
        in.list_identity=request.list_identity;in.generation=request.generation;in.revision=request.revision;
        in.topology=state.topology;in.strip_cut=state.pipeline.strip_cut;
        in.indexing_proven=request.indexing_proven;in.srt_indices=request.srt_indices;
        in.vertex_fetch_proven=request.vertex_fetch_proven;in.vertex_access=request.vertex_access;
        for(const auto& b:lease->owners_.buffers) in.resources.push_back({b.resource.id,in.generation,
            b.bytes->size(),b.bytes->data(),b.bytes->size(),true,true,in.revision,in.list_identity});
        for(const auto& d:lease->owners_.descriptors) {
            const auto& m=*d.metadata.snapshot;
            const auto& root=state.roots.at(d.parameter);
            if(root.kind!=metadata::ParameterKind::table) {
                require(in.root_descriptors.emplace(root.value,m.descriptor).second,"duplicate root descriptor address");
                continue;
            }
            auto t=std::find_if(in.tables.begin(),in.tables.end(),[&](const capture::TableCopy& v){return v.heap==m.heap.id && v.base==root.value;});
            if(t==in.tables.end()) {
                in.tables.push_back({m.heap.id,root.value,{},in.generation,in.revision,in.list_identity});
                t=std::prev(in.tables.end());
            }
            // A sparse hole is an unsupported descriptor, never a guessed SRV.
            capture::Descriptor absent;absent.buffer=false;
            if(t->descriptors.size()<=d.table_offset) t->descriptors.resize(d.table_offset+1,absent);
            require(!t->descriptors[d.table_offset].buffer,"duplicate table descriptor slot");
            t->descriptors[d.table_offset]=m.descriptor; // Keep pre-FirstElement origin verbatim.
        }
        return {std::move(lease),{}};
    } catch(const std::exception& e) {return rejected<Result>(e.what());}
      catch(...) {return rejected<Result>("provider allocation failed");}
}
capture::Result freeze_owned(const binding::NativeBindingObserver& observer, const State& state,
                             const Command& command, const Lease& lease) {
    try {
        validate(observer,state,lease.owners_);
        return capture::freeze(state,command,lease.inputs_);
    } catch(const std::exception& e) {return rejected<capture::Result>(e.what());}
      catch(...) {return rejected<capture::Result>("provider validation failed");}
}
} // namespace acz::capture_input
