#include "native_binding_observer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace acz::binding {

const char* overlap_reason(ObservationKind a,ObservationKind b,bool same) noexcept {
    static const char* const reasons[][2]={
        {"overlap:descriptor_write/descriptor_write:cross", "overlap:descriptor_write/descriptor_write:same"},
        {"overlap:descriptor_write/copy:cross", "overlap:descriptor_write/copy:same"},
        {"overlap:descriptor_write/committed_create:cross", "overlap:descriptor_write/committed_create:same"},
        {"overlap:descriptor_write/placed_create:cross", "overlap:descriptor_write/placed_create:same"},
        {"overlap:descriptor_write/reserved_create:cross", "overlap:descriptor_write/reserved_create:same"},
        {"overlap:descriptor_write/heap_create:cross", "overlap:descriptor_write/heap_create:same"},
        {"overlap:descriptor_write/resource_release:cross", "overlap:descriptor_write/resource_release:same"},
        {"overlap:descriptor_write/heap_release:cross", "overlap:descriptor_write/heap_release:same"},
        {"overlap:descriptor_write/root_create:cross", "overlap:descriptor_write/root_create:same"},
        {"overlap:descriptor_write/pso_create:cross", "overlap:descriptor_write/pso_create:same"},
        {"overlap:descriptor_write/device_release:cross", "overlap:descriptor_write/device_release:same"},
        {"overlap:descriptor_write/metadata_release:cross", "overlap:descriptor_write/metadata_release:same"},
        {"overlap:descriptor_write/other:cross", "overlap:descriptor_write/other:same"},
        {"overlap:copy/copy:cross", "overlap:copy/copy:same"},
        {"overlap:copy/committed_create:cross", "overlap:copy/committed_create:same"},
        {"overlap:copy/placed_create:cross", "overlap:copy/placed_create:same"},
        {"overlap:copy/reserved_create:cross", "overlap:copy/reserved_create:same"},
        {"overlap:copy/heap_create:cross", "overlap:copy/heap_create:same"},
        {"overlap:copy/resource_release:cross", "overlap:copy/resource_release:same"},
        {"overlap:copy/heap_release:cross", "overlap:copy/heap_release:same"},
        {"overlap:copy/root_create:cross", "overlap:copy/root_create:same"},
        {"overlap:copy/pso_create:cross", "overlap:copy/pso_create:same"},
        {"overlap:copy/device_release:cross", "overlap:copy/device_release:same"},
        {"overlap:copy/metadata_release:cross", "overlap:copy/metadata_release:same"},
        {"overlap:copy/other:cross", "overlap:copy/other:same"},
        {"overlap:committed_create/committed_create:cross", "overlap:committed_create/committed_create:same"},
        {"overlap:committed_create/placed_create:cross", "overlap:committed_create/placed_create:same"},
        {"overlap:committed_create/reserved_create:cross", "overlap:committed_create/reserved_create:same"},
        {"overlap:committed_create/heap_create:cross", "overlap:committed_create/heap_create:same"},
        {"overlap:committed_create/resource_release:cross", "overlap:committed_create/resource_release:same"},
        {"overlap:committed_create/heap_release:cross", "overlap:committed_create/heap_release:same"},
        {"overlap:committed_create/root_create:cross", "overlap:committed_create/root_create:same"},
        {"overlap:committed_create/pso_create:cross", "overlap:committed_create/pso_create:same"},
        {"overlap:committed_create/device_release:cross", "overlap:committed_create/device_release:same"},
        {"overlap:committed_create/metadata_release:cross", "overlap:committed_create/metadata_release:same"},
        {"overlap:committed_create/other:cross", "overlap:committed_create/other:same"},
        {"overlap:placed_create/placed_create:cross", "overlap:placed_create/placed_create:same"},
        {"overlap:placed_create/reserved_create:cross", "overlap:placed_create/reserved_create:same"},
        {"overlap:placed_create/heap_create:cross", "overlap:placed_create/heap_create:same"},
        {"overlap:placed_create/resource_release:cross", "overlap:placed_create/resource_release:same"},
        {"overlap:placed_create/heap_release:cross", "overlap:placed_create/heap_release:same"},
        {"overlap:placed_create/root_create:cross", "overlap:placed_create/root_create:same"},
        {"overlap:placed_create/pso_create:cross", "overlap:placed_create/pso_create:same"},
        {"overlap:placed_create/device_release:cross", "overlap:placed_create/device_release:same"},
        {"overlap:placed_create/metadata_release:cross", "overlap:placed_create/metadata_release:same"},
        {"overlap:placed_create/other:cross", "overlap:placed_create/other:same"},
        {"overlap:reserved_create/reserved_create:cross", "overlap:reserved_create/reserved_create:same"},
        {"overlap:reserved_create/heap_create:cross", "overlap:reserved_create/heap_create:same"},
        {"overlap:reserved_create/resource_release:cross", "overlap:reserved_create/resource_release:same"},
        {"overlap:reserved_create/heap_release:cross", "overlap:reserved_create/heap_release:same"},
        {"overlap:reserved_create/root_create:cross", "overlap:reserved_create/root_create:same"},
        {"overlap:reserved_create/pso_create:cross", "overlap:reserved_create/pso_create:same"},
        {"overlap:reserved_create/device_release:cross", "overlap:reserved_create/device_release:same"},
        {"overlap:reserved_create/metadata_release:cross", "overlap:reserved_create/metadata_release:same"},
        {"overlap:reserved_create/other:cross", "overlap:reserved_create/other:same"},
        {"overlap:heap_create/heap_create:cross", "overlap:heap_create/heap_create:same"},
        {"overlap:heap_create/resource_release:cross", "overlap:heap_create/resource_release:same"},
        {"overlap:heap_create/heap_release:cross", "overlap:heap_create/heap_release:same"},
        {"overlap:heap_create/root_create:cross", "overlap:heap_create/root_create:same"},
        {"overlap:heap_create/pso_create:cross", "overlap:heap_create/pso_create:same"},
        {"overlap:heap_create/device_release:cross", "overlap:heap_create/device_release:same"},
        {"overlap:heap_create/metadata_release:cross", "overlap:heap_create/metadata_release:same"},
        {"overlap:heap_create/other:cross", "overlap:heap_create/other:same"},
        {"overlap:resource_release/resource_release:cross", "overlap:resource_release/resource_release:same"},
        {"overlap:resource_release/heap_release:cross", "overlap:resource_release/heap_release:same"},
        {"overlap:resource_release/root_create:cross", "overlap:resource_release/root_create:same"},
        {"overlap:resource_release/pso_create:cross", "overlap:resource_release/pso_create:same"},
        {"overlap:resource_release/device_release:cross", "overlap:resource_release/device_release:same"},
        {"overlap:resource_release/metadata_release:cross", "overlap:resource_release/metadata_release:same"},
        {"overlap:resource_release/other:cross", "overlap:resource_release/other:same"},
        {"overlap:heap_release/heap_release:cross", "overlap:heap_release/heap_release:same"},
        {"overlap:heap_release/root_create:cross", "overlap:heap_release/root_create:same"},
        {"overlap:heap_release/pso_create:cross", "overlap:heap_release/pso_create:same"},
        {"overlap:heap_release/device_release:cross", "overlap:heap_release/device_release:same"},
        {"overlap:heap_release/metadata_release:cross", "overlap:heap_release/metadata_release:same"},
        {"overlap:heap_release/other:cross", "overlap:heap_release/other:same"},
        {"overlap:root_create/root_create:cross", "overlap:root_create/root_create:same"},
        {"overlap:root_create/pso_create:cross", "overlap:root_create/pso_create:same"},
        {"overlap:root_create/device_release:cross", "overlap:root_create/device_release:same"},
        {"overlap:root_create/metadata_release:cross", "overlap:root_create/metadata_release:same"},
        {"overlap:root_create/other:cross", "overlap:root_create/other:same"},
        {"overlap:pso_create/pso_create:cross", "overlap:pso_create/pso_create:same"},
        {"overlap:pso_create/device_release:cross", "overlap:pso_create/device_release:same"},
        {"overlap:pso_create/metadata_release:cross", "overlap:pso_create/metadata_release:same"},
        {"overlap:pso_create/other:cross", "overlap:pso_create/other:same"},
        {"overlap:device_release/device_release:cross", "overlap:device_release/device_release:same"},
        {"overlap:device_release/metadata_release:cross", "overlap:device_release/metadata_release:same"},
        {"overlap:device_release/other:cross", "overlap:device_release/other:same"},
        {"overlap:metadata_release/metadata_release:cross", "overlap:metadata_release/metadata_release:same"},
        {"overlap:metadata_release/other:cross", "overlap:metadata_release/other:same"},
        {"overlap:other/other:cross", "overlap:other/other:same"},
    };
    static_assert(std::size(reasons)*2==overlap_reason_count);
    constexpr unsigned n=static_cast<unsigned>(ObservationKind::count);
    auto x=static_cast<unsigned>(a),y=static_cast<unsigned>(b);
    if(x>=n)x=n-1;if(y>=n)y=n-1;
    if(x>y)std::swap(x,y);
    return reasons[x*n-x*(x-1)/2+y-x][same?1:0];
}

Token NativeBindingObserver::heap_token(uint64_t id) const {
    auto i=heaps_.find(id);return i==heaps_.end()?Token{}:i->second;
}
Token NativeBindingObserver::resource_token(uint64_t id) const {
    auto i=resources_.find(id);return i==resources_.end()?Token{}:i->second;
}
void NativeBindingObserver::heap(const HeapEvent& e,uint64_t device,bool tolerate) {
    diagnose([&](auto& d){d.heap(e.id,device,e.cpu_base,e.gpu_base,e.count,e.increment,e.type,e.flags);});
    if(!enabled_) return;
    if(untracked_.count(e.id)) {invalidate_object(e.id);return;}
    // A concurrent final Release may still be retiring a displaced lifetime.
    auto old=heap_token(e.id);if(old.id) {registry_.remove_heap(old);heaps_.erase(e.id);}
    auto r=registry_.add_heap(e);
    if(!r) {
        std::snprintf(last_rejection_.data(),last_rejection_.size(),"%s",r.rejection.c_str());
        // XR call window: a counted exception. Origin stays UNKNOWN: tombstone the
        // address so later events and resolutions through it refuse (never proxy-owned).
        if(tolerate) {++tolerated_rejections_;mark_untracked(e.id);return;}
        disable("heap registration rejected");return;
    }
    heaps_[e.id]=r.token;heap_events_[e.id]=e;heap_devices_[e.id]=device;
    perf::gauge(perf::registry_heaps,heaps_.size());
}
void NativeBindingObserver::foreign_heap(uint64_t id) {
    ++foreign_heaps_;
    if(!enabled_) return;
    // Same address as an earlier (game) heap whose Release was missed: that
    // lifetime is over, so its registration must not survive the reuse.
    auto old=heap_token(id);if(old.id) {registry_.remove_heap(old);heaps_.erase(id);}
}
void NativeBindingObserver::resource(const ResourceEvent& e,uint64_t device) {
    if(!enabled_) return;
    if(untracked_.count(e.id)) {invalidate_object(e.id);return;}
    auto old=resource_token(e.id);if(old.id) {registry_.remove_resource(old);resources_.erase(e.id);}
    auto r=registry_.add_resource(e);if(!r) {disable("resource registration rejected");return;} resources_[e.id]=r.token;resource_devices_[e.id]=device;
    perf::gauge(perf::registry_resources,resources_.size());
}
void NativeBindingObserver::remove_heap(Token t) {
    if(t.id&&heap_token(t.id)==t)diagnose([&](auto& d){d.retire(t.id);});
    if(t.id && heap_token(t.id)==t) {registry_.remove_heap(t);heaps_.erase(t.id);heap_events_.erase(t.id);heap_devices_.erase(t.id);}
    perf::gauge(perf::registry_heaps,heaps_.size());
}
void NativeBindingObserver::remove_resource(Token t) {
    if(t.id&&textures_)textures_->retire(t.id);
    if(t.id && resource_token(t.id)==t) {registry_.remove_resource(t);resources_.erase(t.id);resource_devices_.erase(t.id);}
    perf::gauge(perf::registry_resources,resources_.size());
}
// FPS investigation: both lookups used to try every tracked heap/resource and, for each, re-scan the whole registry
// (O(N^2) per descriptor write / root-view resolution). cpu_slot/buffer_address can only succeed for the UNIQUE heap/resource
// whose interval contains/intersects the handle/span, so that candidate is computed once and then confirmed by the same
// registry call as before: identical results (registry_offline_tests compares the reference loops on random layouts).
Token NativeBindingObserver::cpu_heap(uint64_t cpu,uint64_t device) const {
    perf::Scope cost(perf::addr_lookup);
    const auto id=registry_.unique_heap(cpu,false);if(!id)return {};
    const auto h=heaps_.find(id);if(h==heaps_.end())return {};
    if(heap_devices_.at(h->first)!=device)return {};
    return registry_.cpu_slot(h->second,cpu)?h->second:Token{};
}
Token NativeBindingObserver::address_resource(uint64_t address,uint64_t bytes,uint64_t device) const {
    perf::Scope cost(perf::addr_lookup);
    const auto id=registry_.unique_overlap(address,bytes);if(!id)return {};
    const auto r=resources_.find(id);if(r==resources_.end())return {};
    if(resource_devices_.at(r->first)!=device)return {};
    return registry_.buffer_address(r->second,address,bytes)?r->second:Token{};
}
Footprint NativeBindingObserver::footprint(uint64_t cpu,uint32_t count,uint64_t device) const noexcept {
    for(const auto& h:heaps_) {
        auto owner=heap_devices_.find(h.first);
        if(owner!=heap_devices_.end() && owner->second==device) {
            auto f=registry_.footprint(h.second,cpu,count);if(f) return f;
        }
    }
    return {};
}
void NativeBindingObserver::invalidate_object(uint64_t id) {
    if(textures_)textures_->retire(id);
    remove_heap(heap_token(id));remove_resource(resource_token(id));
}
void NativeBindingObserver::mark_untracked(uint64_t id) {
    if(!id)return;
    invalidate_object(id);
    untracked_.insert(id);
}
void NativeBindingObserver::sampler(const D3D12_SAMPLER_DESC* d,uint64_t cpu,uint64_t device) {
    Sampler value;
    auto bits=[](const float& f) {uint32_t v;std::memcpy(&v,&f,sizeof(v));return v;};
    if(d)value.words={uint32_t(d->Filter),uint32_t(d->AddressU),uint32_t(d->AddressV),uint32_t(d->AddressW),
        bits(d->MipLODBias),d->MaxAnisotropy,uint32_t(d->ComparisonFunc),bits(d->BorderColor[0]),
        bits(d->BorderColor[1]),bits(d->BorderColor[2]),bits(d->BorderColor[3]),bits(d->MinLOD),bits(d->MaxLOD)};
    diagnose([&](auto& h){h.write(cpu,device,d?diagnostics::DescriptorTableDiagnostics::sampler:diagnostics::DescriptorTableDiagnostics::unknown,0,0,0,0,0,
        diagnostics::DescriptorTableDiagnostics::create_sampler,0,{},nullptr,d?&value.words:nullptr);});
    if(!enabled_) return;
    if(!d) {registry_.invalidate_write(cpu_heap(cpu,device),cpu);return;}
    registry_.write_sampler(cpu_heap(cpu,device),cpu,value);
}
void NativeBindingObserver::unsupported_write(uint64_t cpu,uint64_t device,unsigned type) {
    diagnose([&](auto& h){h.write(cpu,device,type,0,0,0,0,0,type==diagnostics::DescriptorTableDiagnostics::uav?
        diagnostics::DescriptorTableDiagnostics::create_uav:diagnostics::DescriptorTableDiagnostics::create_other);});
    if(enabled_) registry_.invalidate_write(cpu_heap(cpu,device),cpu);
}
void NativeBindingObserver::srv(uint64_t resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* d,uint64_t cpu,uint64_t device,const diagnostics::ViewWriteDescription* view_write) {
    diagnose([&](auto& h){
        const auto owner=resource_devices_.find(resource);
        const auto token=owner!=resource_devices_.end()&&owner->second==device?resource_token(resource):Token{};
        const bool supported=d&&d->ViewDimension==D3D12_SRV_DIMENSION_BUFFER&&
            d->Shader4ComponentMapping==D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING&&
            d->Format==DXGI_FORMAT_UNKNOWN&&!d->Buffer.Flags&&d->Buffer.NumElements&&
            d->Buffer.StructureByteStride&&d->Buffer.StructureByteStride<=2048&&!(d->Buffer.StructureByteStride%4);
        h.write(cpu,device,diagnostics::DescriptorTableDiagnostics::srv,d?unsigned(d->ViewDimension):0,
            d?unsigned(d->Format):0,resource,token.generation,supported?0u:diagnostics::DescriptorTableDiagnostics::unsupported_srv,
            diagnostics::DescriptorTableDiagnostics::create_srv,d?2u:1u,
            textures_&&!token.id?textures_->view(resource,device,!d):diagnostics::TextureDescriptionRegistry::Resolution{},view_write);
    });
    if(!enabled_) return;
    if(!d || !resource || d->ViewDimension!=D3D12_SRV_DIMENSION_BUFFER ||
       d->Shader4ComponentMapping!=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
        registry_.invalidate_write(cpu_heap(cpu,device),cpu);return;
    }
    auto owner=resource_devices_.find(resource);
    StructuredSrv v{owner!=resource_devices_.end() && owner->second==device?resource_token(resource):Token{},d->Buffer.FirstElement,d->Buffer.NumElements,
                    d->Buffer.StructureByteStride,static_cast<uint32_t>(d->ViewDimension),
                    static_cast<uint32_t>(d->Format),static_cast<uint32_t>(d->Buffer.Flags)};
    registry_.write_srv(cpu_heap(cpu,device),cpu,v);
}
void NativeBindingObserver::cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d,uint64_t cpu,uint64_t device) {
    diagnose([&](auto& h){h.write(cpu,device,d?diagnostics::DescriptorTableDiagnostics::cbv:diagnostics::DescriptorTableDiagnostics::unknown,0,0,0,0,0,
        diagnostics::DescriptorTableDiagnostics::create_cbv);});
    if(!enabled_) return;
    if(!d) {registry_.invalidate_write(cpu_heap(cpu,device),cpu);return;}
    registry_.write_cbv(cpu_heap(cpu,device),cpu,{address_resource(d->BufferLocation,d->SizeInBytes,device),d->BufferLocation,d->SizeInBytes});
}
void NativeBindingObserver::copy(UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dest,const UINT* ds,
                                 UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* source,const UINT* ss,
                                 D3D12_DESCRIPTOR_HEAP_TYPE type,uint64_t device,bool simple) {
    diagnostic_copy(dc,dest,ds,sc,source,ss,type,device,simple);
    if(!enabled_) return;
    if(dc>65536 || sc>65536 || !dest || !source) {unsupported_write(0,device);return;}
    std::vector<CopyRange> d,s;
    for(UINT i=0;i<dc;++i) d.push_back({cpu_heap(dest[i].ptr,device),dest[i].ptr,ds?ds[i]:1});
    for(UINT i=0;i<sc;++i) s.push_back({cpu_heap(source[i].ptr,device),source[i].ptr,ss?ss[i]:1});
    registry_.copy(d,s,static_cast<uint32_t>(type));
}
void NativeBindingObserver::diagnostic_copy(UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dest,const UINT* ds,
    UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* source,const UINT* ss,D3D12_DESCRIPTOR_HEAP_TYPE type,uint64_t device,bool simple) noexcept {
    diagnose([&](auto& h){
        using D=diagnostics::DescriptorTableDiagnostics;
        auto fail=[&](){h.invalidate();};
        if(!dc||!sc||dc>65536||sc>65536||!dest||!source||unsigned(type)>1){fail();return;}
        std::vector<D::Location> destinations;
        std::vector<D::Slot> values;
        auto flatten=[&](UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,const UINT* sizes,bool output) {
            uint64_t total=0;
            for(UINT i=0;i<n;++i) {
                const auto count=sizes?sizes[i]:1;
                if(!count||count>65536-total)return false;
                total+=count;
                auto l=h.locate(handles[i].ptr,device,false);
                auto heap=h.heaps.find(l.key);
                if(l.flags||heap==h.heaps.end()||heap->second.type!=unsigned(type)||
                   count>heap->second.count-l.slot)return false;
                for(unsigned j=0;j<count;++j) {
                    auto x=l;x.slot+=j;
                    if(output)destinations.push_back(x);
                    else {
                        auto v=h.slots.find({x.serial,x.slot});
                        D::Slot value;
                        if(v!=h.slots.end())value=v->second;
                        else value.flags|=D::unwritten|D::invalidated;
                        value.flags|=D::copied;value.writer=simple?D::copy_simple:D::copy_ranges;
                        value.source_heap=x.serial;value.source_class=x.type;
                        value.source_slot=x.slot;value.source_flags=heap->second.heap_flags;
                        values.push_back(value);
                    }
                }
            }
            return true;
        };
        if(!flatten(dc,dest,ds,true)||!flatten(sc,source,ss,false)||destinations.size()!=values.size()){fail();return;}
        for(size_t i=0;i<destinations.size();++i)h.store(destinations[i],values[i]);
    });
}
diagnostics::DescriptorTableDiagnostics::Snapshot NativeBindingObserver::diagnostic_snapshot(
    diagnostics::DescriptorTableDiagnostics::Location location,uint32_t offset,unsigned expected) const noexcept {
    using D=diagnostics::DescriptorTableDiagnostics;
    if(!diagnostic_active_||!diagnostic_) {D::Snapshot s;s.flags=D::incomplete;return s;}
    auto s=diagnostic_->snapshot(location,offset,expected);
    // Scalar epoch check only: never scan the registry at Draw. Historical copy
    // facts may survive, but a retirement/reuse/gap invalidates derived shape.
    if(textures_&&s.texture.epoch!=textures_->epoch) {s.texture.resolved=0;s.texture.status|=textures_->gaps|diagnostics::TextureDescriptionRegistry::history_gap;}
    if(!enabled_||texture_allocation_gap_) {s.texture.resolved=0;s.texture.status|=diagnostics::TextureDescriptionRegistry::history_gap;}
    if(!enabled_)s.flags|=D::incomplete;
    if(location.key&&!heap_token(location.key).id)s.flags|=D::incomplete|D::unknown_base;
    auto v=diagnostic_->slots.find({location.serial,s.slot});
    if(v!=diagnostic_->slots.end()&&v->second.type==D::srv) {
        const auto t=resource_token(v->second.resource);
        if(!t.id||t.generation!=v->second.resource_generation)s.flags|=D::unknown_resource;
    }
    return s;
}
NativeResolution NativeBindingObserver::rejected(const char* reason) {NativeResolution r;r.rejection=reason;return r;}
NativeResolution NativeBindingObserver::stamp(const capture::State& s,const Resolution& value,uint64_t device) const {
    if(!enabled_ || !s.tracking_complete || !s.list_identity) return rejected("disabled/incomplete native observation");
    NativeResolution r;r.list_identity=s.list_identity;r.generation=s.generation;r.revision=s.revision;
    r.snapshot=value.snapshot;r.rejection=value.rejection;r.device=device;return r;
}
NativeResolution NativeBindingObserver::table(const capture::State& s,uint32_t parameter,uint64_t heap_id,uint32_t offset,uint64_t device) const {
    if(!enabled_) return rejected("native metadata path disabled");
    if(std::find(s.heaps.begin(),s.heaps.end(),heap_id)==s.heaps.end()) return rejected("heap not bound at capture point");
    auto owner=heap_devices_.find(heap_id);if(owner==heap_devices_.end() || owner->second!=device) return rejected("heap device is not the list device");
    auto root=s.roots.find(parameter);
    if(root==s.roots.end() || root->second.kind!=metadata::ParameterKind::table) return rejected("missing table root binding");
    auto token=heap_token(heap_id);auto slot=registry_.gpu_slot(token,root->second.value);
    if(!slot) return rejected("unknown/stale/misaligned bound table");
    // Resolve each requested descriptor via registry slots; the increment comes
    // from the observed SDK heap event, never from an assumed game layout.
    auto h=heap_events_.find(heap_id);
    if(h==heap_events_.end() || offset>=h->second.count-slot.index) return rejected("table descriptor offset out of bounds");
    return stamp(s,registry_.resolve_gpu(token,root->second.value+uint64_t(offset)*h->second.increment),device);
}
NativeResolution NativeBindingObserver::root(const capture::State& s,uint32_t parameter,uint32_t bytes,uint32_t elements,uint32_t stride,uint64_t device) const {
    if(!enabled_) return rejected("native metadata path disabled");
    auto r=s.roots.find(parameter);if(r==s.roots.end()) return rejected("missing root binding");
    if(r->second.kind==metadata::ParameterKind::cbv && !elements && !stride)
        return stamp(s,registry_.root_cbv(address_resource(r->second.value,bytes,device),r->second.value,bytes),device);
    if(r->second.kind==metadata::ParameterKind::srv && !bytes)
        return stamp(s,registry_.root_srv(address_resource(r->second.value,uint64_t(elements)*stride,device),r->second.value,elements,stride),device);
    return rejected("unsupported root kind/provider layout");
}
Status NativeBindingObserver::current(const NativeResolution& r,const capture::State& s,uint64_t device) const {
    if(!enabled_ || !r || !s.tracking_complete || !s.list_identity || r.list_identity!=s.list_identity ||
       r.generation!=s.generation || r.revision!=s.revision || r.device!=device) return {"stale/mismatched capture-point metadata"};
    return registry_.current(*r.snapshot);
}
} // namespace acz::binding
