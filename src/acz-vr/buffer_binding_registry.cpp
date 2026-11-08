#include "buffer_binding_registry.h"
#include <atomic>
#include <limits>
#include <set>

namespace acz::binding {
namespace {
constexpr auto limit=std::numeric_limits<uint64_t>::max();
// Process-unique identities/revisions, permanently saturating on exhaustion.
uint64_t serial() {
    static std::atomic<uint64_t> next{1};
    auto n=next.load(std::memory_order_relaxed);
    while(n!=limit) if(next.compare_exchange_weak(n,n+1,std::memory_order_relaxed)) return n;
    return 0;
}
bool span(uint64_t base, uint64_t size) { return base && size && size<=limit-base; }
bool product(uint64_t a, uint64_t b, uint64_t& out) {
    if(b && a>limit/b) return false;
    out=a*b; return true;
}
bool stride_ok(uint32_t stride) { return stride && stride<=2048 && stride%4==0; }
Registration rejected(const char* why) { Registration r; r.rejection=why; return r; }
Resolution rejected_resolution(const std::string& why) { Resolution r; r.rejection=why; return r; }
}
Registration BufferBindingRegistry::add_heap(const HeapEvent& e) {
    uint64_t length=0;
    if(!e.id || heaps_.count(e.id)) return rejected("null/duplicate heap identity");
    if(e.type>1 || e.flags>1 || e.node_mask) return rejected("unsupported heap type/flags/node mask");
    if(!e.increment || !product(e.count,e.increment,length) || !span(e.cpu_base,length))
        return rejected("invalid/overflowing CPU heap interval");
    if((e.flags==1 && !span(e.gpu_base,length)) || (e.flags==0 && e.gpu_base))
        return rejected("invalid GPU heap interval/visibility");
    auto generation=serial(); if(!generation) return rejected("identity exhausted");
    Token token{e.id,generation}; heaps_.emplace(e.id,Heap{e,token,{}});
    Registration r; r.token=token; return r;
}
Registration BufferBindingRegistry::add_resource(const ResourceEvent& e) {
    if(!e.id || resources_.count(e.id)) return rejected("null/duplicate resource identity");
    if(e.dimension!=1 || e.aliased) return rejected("unsupported resource dimension/aliasing");
    if(!span(e.gpu_base,e.bytes)) return rejected("invalid/overflowing buffer interval");
    auto generation=serial(); if(!generation) return rejected("identity exhausted");
    Token token{e.id,generation}; auto inserted=resources_.emplace(e.id,Resource{e,token}).first;
    // Keep the index in step: an allocation failure rolls the registration back (the caller then sees the exception, as
    // for a failed resources_ insertion before).
    auto indexed=by_base_.end();
    try {indexed=by_base_.emplace(e.gpu_base,Span{e.id,e.bytes});sizes_.insert(e.bytes);}
    catch(...) {if(indexed!=by_base_.end())by_base_.erase(indexed);resources_.erase(inserted);throw;}
    Registration r; r.token=token; return r;
}
Status BufferBindingRegistry::remove_heap(Token t) {
    auto i=heaps_.find(t.id);
    if(i==heaps_.end() || i->second.token!=t) return {"unknown/stale heap token"};
    heaps_.erase(i); return {};
}
Status BufferBindingRegistry::remove_resource(Token t) {
    auto i=resources_.find(t.id);
    if(i==resources_.end() || i->second.token!=t) return {"unknown/stale resource token"};
    const auto& e=i->second.event;
    for(auto r=by_base_.equal_range(e.gpu_base);r.first!=r.second;++r.first)
        if(r.first->second.id==e.id){by_base_.erase(r.first);break;}
    if(auto s=sizes_.find(e.bytes);s!=sizes_.end())sizes_.erase(s);
    resources_.erase(i); // Descriptors retain historical tokens; current() rejects them.
    return {};
}
unsigned BufferBindingRegistry::overlaps(uint64_t address,uint64_t bytes,unsigned stop,uint64_t* only) const noexcept {
    // Intersection: address < base+len && base < address+bytes. A resource with base <= address-max_len ends at or before
    // `address`, so only bases in (address-max_len, address+bytes) can intersect.
    if(by_base_.empty())return 0;
    const uint64_t max_len=*sizes_.rbegin(),end=address+bytes;
    auto it=address>max_len?by_base_.upper_bound(address-max_len):by_base_.begin();
    unsigned matches=0;
    for(;it!=by_base_.end()&&it->first<end;++it) {
        if(address<it->first+it->second.bytes) {
            ++matches;if(only)*only=it->second.id;
            if(matches>=stop)break;
        }
    }
    return matches;
}
uint64_t BufferBindingRegistry::unique_overlap(uint64_t address,uint64_t bytes) const noexcept {
    if(!span(address,bytes))return 0;
    uint64_t id=0;
    return overlaps(address,bytes,2,&id)==1?id:0;
}
uint64_t BufferBindingRegistry::unique_heap(uint64_t handle,bool gpu) const noexcept {
    if(!handle)return 0;
    unsigned matches=0;uint64_t id=0;
    for(const auto& entry:heaps_) {
        const auto& h=entry.second.event;
        if(gpu && h.flags!=1) continue;
        auto base=gpu?h.gpu_base:h.cpu_base;
        auto bytes=uint64_t(h.count)*h.increment;
        if(handle>=base && handle-base<bytes) {if(++matches>1)return 0;id=entry.first;}
    }
    return matches==1?id:0;
}
Slot BufferBindingRegistry::slot(Token expected, uint64_t handle, bool gpu) const {
    Slot out;
    auto i=heaps_.find(expected.id);
    if(i==heaps_.end() || i->second.token!=expected) {out.rejection="unknown/stale heap token"; return out;}
    const auto& e=i->second.event;
    if(gpu && e.flags!=1) {out.rejection="heap is not shader-visible"; return out;}
    if(!handle) {out.rejection="null descriptor handle"; return out;}
    unsigned matches=0;
    for(const auto& entry:heaps_) {
        const auto& h=entry.second.event;
        if(gpu && h.flags!=1) continue;
        auto base=gpu?h.gpu_base:h.cpu_base;
        auto bytes=uint64_t(h.count)*h.increment;
        if(handle>=base && handle-base<bytes) ++matches;
    }
    if(matches!=1) {out.rejection=matches?"ambiguous descriptor interval":"unknown descriptor handle"; return out;}
    auto base=gpu?e.gpu_base:e.cpu_base;
    if(handle<base || handle-base>=uint64_t(e.count)*e.increment || (handle-base)%e.increment) {
        out.rejection="descriptor out of bounds/misaligned"; return out;
    }
    out.heap=expected; out.index=uint32_t((handle-base)/e.increment); return out;
}
Slot BufferBindingRegistry::cpu_slot(Token t,uint64_t h) const { return slot(t,h,false); }
Slot BufferBindingRegistry::gpu_slot(Token t,uint64_t h) const { return slot(t,h,true); }
uint64_t BufferBindingRegistry::observed_address(uint64_t address,uint64_t bytes) const noexcept {
    if(!address||!bytes||bytes>UINT64_MAX-address)return 0;
    // The diagnostic caller has a fixed work budget, unlike general proof
    // resolution. Above it, containment/uniqueness is explicitly unknown.
    if(resources_.size()>8192)return 0;
    // FPS investigation: the index replaces the whole-map scan (same predicate; only "exactly one intersecting" matters).
    uint64_t id=0;
    if(overlaps(address,bytes,2,&id)!=1)return 0;
    const auto& e=resources_.find(id)->second.event;
    return address>=e.gpu_base&&address-e.gpu_base<e.bytes&&bytes<=e.bytes-(address-e.gpu_base)?e.id:0;
}
Address BufferBindingRegistry::buffer_address(Token t,uint64_t address,uint64_t bytes) const {
    Address out;
    auto i=resources_.find(t.id);
    if(i==resources_.end() || i->second.token!=t) {out.rejection="unknown/stale resource token"; return out;}
    if(!span(address,bytes)) {out.rejection="null/empty/overflowing GPU address span"; return out;}
    // Any overlap with the requested byte span is ambiguous, even if only one
    // resource contains the whole span (e.g. overlapping placed resources).
    // FPS investigation: counted through the base index, stopping at 2 (only 0 / 1 / more matter).
    const unsigned matches=overlaps(address,bytes,2,nullptr);
    if(matches!=1) {out.rejection=matches?"ambiguous GPU address interval":"unknown GPU address"; return out;}
    const auto& e=i->second.event;
    if(address<e.gpu_base || address-e.gpu_base>=e.bytes || bytes>e.bytes-(address-e.gpu_base)) {
        out.rejection="buffer span out of bounds"; return out;
    }
    out.resource=t; out.offset=address-e.gpu_base; out.resource_bytes=e.bytes; return out;
}
bool BufferBindingRegistry::destination_range(const CopyRange& range) const noexcept {
    auto i=heaps_.find(range.heap.id);
    if(i==heaps_.end() || i->second.token!=range.heap || !range.count || range.count>65536)
        return false;
    const auto& h=i->second.event;
    if(range.cpu_start<h.cpu_base) return false;
    const auto offset=range.cpu_start-h.cpu_base;
    const auto bytes=uint64_t(h.count)*h.increment;
    if(offset>=bytes || offset%h.increment || range.count>h.count-offset/h.increment)
        return false;
    uint64_t length=0;
    if(!product(range.count,h.increment,length) || !span(range.cpu_start,length)) return false;
    // Checking only descriptor starts misses overlaps between two starts.
    for(const auto& entry:heaps_) {
        if(entry.first==range.heap.id) continue;
        const auto& e=entry.second.event;
        if(range.cpu_start<e.cpu_base+uint64_t(e.count)*e.increment &&
           e.cpu_base<range.cpu_start+length) return false;
    }
    return true;
}
bool BufferBindingRegistry::destination_set(const std::vector<CopyRange>& ranges) const noexcept {
    if(ranges.empty()) return false;
    uint64_t total=0;
    for(size_t n=0;n<ranges.size();++n) {
        const auto& r=ranges[n];
        if(!destination_range(r) || r.count>65536-total) return false;
        total+=r.count;
        const auto increment=heaps_.find(r.heap.id)->second.event.increment;
        const auto end=r.cpu_start+uint64_t(r.count)*increment;
        for(size_t k=0;k<n;++k) {
            const auto& previous=ranges[k];
            if(previous.heap!=r.heap) continue;
            const auto previous_end=previous.cpu_start+uint64_t(previous.count)*increment;
            if(r.cpu_start<previous_end && previous.cpu_start<end) return false;
        }
    }
    return true;
}
void BufferBindingRegistry::invalidate_all() noexcept {
    for(auto& h:heaps_) h.second.slots.clear();
}
void BufferBindingRegistry::invalidate_destinations(const std::vector<CopyRange>& ranges) noexcept {
    // Only called after the entire set is proven, with serialized ownership.
    // Erasing existing nodes needs no allocations, even under memory pressure.
    for(const auto& r:ranges) {
        auto& h=heaps_.find(r.heap.id)->second;
        auto first=uint32_t((r.cpu_start-h.event.cpu_base)/h.event.increment);
        h.slots.erase(h.slots.lower_bound(first),h.slots.lower_bound(first+r.count));
    }
}
Status BufferBindingRegistry::fail_write(const std::string& why,const Slot* proven) {
    if(proven) heaps_.find(proven->heap.id)->second.slots.erase(proven->index);
    else invalidate_all();
    return {why};
}
Status BufferBindingRegistry::store(const Slot& s,Snapshot value) {
    auto revision=serial(); if(!revision) return fail_write("revision exhausted",&s);
    value.heap=s.heap; value.slot=s.index; value.revision=revision;
    heaps_.at(s.heap.id).slots[s.index]=std::make_shared<const Snapshot>(std::move(value)); return {};
}
Status BufferBindingRegistry::invalidate_write(Token t,uint64_t handle) try {
    if(!destination_range({t,handle,1})) return fail_write("unproven write destination");
    auto s=cpu_slot(t,handle);
    if(!s) return fail_write(s.rejection);
    heaps_.at(t.id).slots.erase(s.index); return {};
} catch(...) {
    invalidate_all();
    throw;
}
Status BufferBindingRegistry::write_srv(Token t,uint64_t handle,const StructuredSrv& d) try {
    if(!destination_range({t,handle,1})) return fail_write("unproven write destination");
    auto s=cpu_slot(t,handle); if(!s) return fail_write(s.rejection);
    if(heaps_.at(t.id).event.type!=0) return fail_write("SRV heap type mismatch",&s);
    if(d.dimension!=1 || d.format || d.flags || !stride_ok(d.stride) || !d.elements)
        return fail_write("unsupported/null structured buffer SRV",&s);
    auto i=resources_.find(d.resource.id);
    if(i==resources_.end() || i->second.token!=d.resource) return fail_write("unknown/stale SRV resource",&s);
    uint64_t start=0,length=0;
    if(!product(d.first_element,d.stride,start) || !product(d.elements,d.stride,length) ||
       start>i->second.event.bytes || length>i->second.event.bytes-start)
        return fail_write("SRV element arithmetic/bounds",&s);
    Snapshot v; v.resource=d.resource; v.resource_bytes=i->second.event.bytes;
    // Preserve SDK FirstElement separately. The view origin is buffer byte 0.
    v.descriptor={metadata::BindingKind::srv,{d.resource.id,0,v.resource_bytes,d.stride},d.first_element,d.elements,true,true};
    return store(s,std::move(v));
} catch(...) {
    invalidate_all();
    throw;
}
Status BufferBindingRegistry::write_cbv(Token t,uint64_t handle,const Cbv& d) try {
    if(!destination_range({t,handle,1})) return fail_write("unproven write destination");
    auto s=cpu_slot(t,handle); if(!s) return fail_write(s.rejection);
    if(heaps_.at(t.id).event.type!=0) return fail_write("CBV heap type mismatch",&s);
    auto v=root_cbv(d.resource,d.gpu_address,d.bytes); if(!v) return fail_write(v.rejection,&s);
    return store(s,*v.snapshot);
} catch(...) {
    invalidate_all();
    throw;
}
Footprint BufferBindingRegistry::footprint(Token t,uint64_t cpu,uint32_t count) const noexcept {
    if(!destination_range({t,cpu,count})) return {};
    const auto& h=heaps_.find(t.id)->second.event;
    return {t,cpu,cpu+uint64_t(count)*h.increment,uint32_t((cpu-h.cpu_base)/h.increment),count,h.type,h.flags};
}
void BufferBindingRegistry::invalidate(const Footprint& f) noexcept {
    if(!f) {invalidate_all();return;}
    auto h=heaps_.find(f.heap.id);
    if(h==heaps_.end() || h->second.token!=f.heap) return;
    auto proven=footprint(f.heap,f.begin,f.count);
    if(!proven || proven.end!=f.end || proven.first!=f.first) {invalidate_all();return;}
    h->second.slots.erase(h->second.slots.lower_bound(f.first),h->second.slots.lower_bound(f.first+f.count));
}
Status BufferBindingRegistry::write_sampler(Token t,uint64_t handle,const Sampler& sampler) try {
    if(!destination_range({t,handle,1})) return fail_write("unproven sampler destination");
    auto s=cpu_slot(t,handle);if(!s) return fail_write(s.rejection);
    if(heaps_.at(t.id).event.type!=1) return fail_write("sampler heap type mismatch",&s);
    Snapshot v;v.descriptor.kind=metadata::BindingKind::sampler;v.descriptor.buffer=false;v.sampler=sampler;
    return store(s,std::move(v));
} catch(...) {invalidate_all();throw;}
Status BufferBindingRegistry::current(const Snapshot& v) const {
    auto r=resources_.find(v.resource.id);
    const bool sampler=v.descriptor.kind==metadata::BindingKind::sampler;
    if(sampler && (!v.heap.id || v.resource.id || v.resource.generation || v.descriptor.buffer))
        return {"invalid sampler provenance"};
    if(!sampler && (r==resources_.end() || r->second.token!=v.resource || r->second.event.bytes!=v.resource_bytes))
        return {"unknown/stale snapshot resource"};
    if(v.heap.id || v.heap.generation) {
        auto h=heaps_.find(v.heap.id);
        if(h==heaps_.end() || h->second.token!=v.heap) return {"unknown/stale snapshot heap"};
        auto s=h->second.slots.find(v.slot);
        if(s==h->second.slots.end() || s->second->revision!=v.revision) return {"invalidated/overwritten descriptor snapshot"};
        const auto& e=h->second.event;
        if(e.type!=(sampler?1u:0u)) return {"snapshot heap type mismatch"};
        auto cpu=cpu_slot(v.heap,e.cpu_base+uint64_t(v.slot)*e.increment);
        if(!cpu) return {cpu.rejection};
        if(e.flags) {
            auto gpu=gpu_slot(v.heap,e.gpu_base+uint64_t(v.slot)*e.increment);
            if(!gpu) return {gpu.rejection};
        }
    }
    // CBVs and root SRVs were resolved through GPU addresses. Recheck their
    // intervals if another live resource has since introduced an overlap.
    if(!sampler && (!v.heap.id || v.descriptor.kind==metadata::BindingKind::cbv)) {
        const auto& view=v.descriptor.view;
        if(view.offset>r->second.event.bytes || view.size>r->second.event.bytes-view.offset)
            return {"snapshot buffer span out of bounds"};
        auto a=buffer_address(v.resource,r->second.event.gpu_base+view.offset,view.size);
        if(!a) return {a.rejection};
    }
    return {};
}
Resolution BufferBindingRegistry::resolve(const Slot& s) const {
    if(!s) return rejected_resolution(s.rejection);
    const auto& slots=heaps_.at(s.heap.id).slots;
    auto i=slots.find(s.index);
    if(i==slots.end()) return rejected_resolution("unobserved/invalidated descriptor");
    auto valid=current(*i->second); if(!valid) return rejected_resolution(valid.rejection);
    Resolution r; r.snapshot=i->second; return r;
}
Resolution BufferBindingRegistry::resolve_cpu(Token t,uint64_t h) const {return resolve(cpu_slot(t,h));}
Resolution BufferBindingRegistry::resolve_gpu(Token t,uint64_t h) const {return resolve(gpu_slot(t,h));}
Resolution BufferBindingRegistry::root_srv(Token t,uint64_t a,uint32_t count,uint32_t stride) const {
    uint64_t length=0;
    if(!stride_ok(stride) || !count || a%4 || !product(count,stride,length))
        return rejected_resolution("invalid root structured SRV provider layout/alignment");
    auto address=buffer_address(t,a,length); if(!address) return rejected_resolution(address.rejection);
    Snapshot v; v.resource=t; v.resource_bytes=address.resource_bytes;
    v.descriptor={metadata::BindingKind::srv,{t.id,address.offset,length,stride},0,count,true,true};
    Resolution r; r.snapshot=std::make_shared<const Snapshot>(std::move(v)); return r;
}
Resolution BufferBindingRegistry::root_cbv(Token t,uint64_t a,uint32_t bytes) const {
    if(a%256 || !bytes || bytes%256 || bytes>65536) return rejected_resolution("invalid CBV size/alignment");
    auto address=buffer_address(t,a,bytes); if(!address) return rejected_resolution(address.rejection);
    Snapshot v; v.resource=t; v.resource_bytes=address.resource_bytes;
    v.descriptor={metadata::BindingKind::cbv,{t.id,address.offset,bytes,0},0,0,false,true};
    Resolution r; r.snapshot=std::make_shared<const Snapshot>(std::move(v)); return r;
}
Status BufferBindingRegistry::copy(const std::vector<CopyRange>& dest,const std::vector<CopyRange>& src,uint32_t type) try {
    // Bounded CPU adapter; larger native events are unsupported, not truncated.
    constexpr uint64_t max_slots=65536;
    // Prove the complete destination set BEFORE allocating/staging anything.
    if(type>1 || !destination_set(dest))
        return fail_write("unsupported/unproven copy destination set");
    // Shape checks precede source visibility/metadata resolution. The installed
    // SDK does not promise destination containment for malformed native calls.
    // Totals are bounded before addition; no allocation is needed for proof or
    // global invalidation (even diagnostic construction may subsequently throw).
    uint64_t destination_total=0,source_total=0;
    for(const auto& range:dest) destination_total+=range.count; // destination_set bounded it
    if(src.empty()) return fail_write("empty copy source set");
    for(const auto& range:src) {
        if(!range.count || range.count>max_slots-source_total)
            return fail_write("empty/oversized copy source range");
        source_total+=range.count;
    }
    if(destination_total!=source_total) return fail_write("descriptor copy count mismatch");
    for(const auto& range:src) {
        auto i=heaps_.find(range.heap.id);
        // Unknown/stale tokens cannot prove an interval from a bare handle.
        // Preserve the existing local rejection below for such source failures.
        if(i==heaps_.end() || i->second.token!=range.heap) continue;
        const auto& h=i->second.event;
        uint64_t length=0;
        if(range.cpu_start<h.cpu_base ||
           (range.cpu_start-h.cpu_base)%h.increment ||
           (range.cpu_start-h.cpu_base)/h.increment>=h.count ||
           range.count>h.count-(range.cpu_start-h.cpu_base)/h.increment ||
           !product(range.count,h.increment,length) || !span(range.cpu_start,length)) continue;
        const auto end=range.cpu_start+length;
        for(const auto& destination:dest) {
            const auto increment=heaps_.find(destination.heap.id)->second.event.increment;
            // destination_set already checked this whole interval's arithmetic.
            const auto destination_end=destination.cpu_start+uint64_t(destination.count)*increment;
            if(range.cpu_start<destination_end && destination.cpu_start<end)
                return fail_write("overlapping source/destination copy intervals");
        }
    }
    auto reject=[&](const std::string& why)->Status {
        invalidate_destinations(dest);
        return {why};
    };
    for(const auto& range:dest) if(heaps_.at(range.heap.id).event.type!=type)
        return reject("copy destination heap type mismatch");
    std::vector<Slot> destinations,sources;
    auto flatten=[&](const std::vector<CopyRange>& ranges,std::vector<Slot>& out,bool source)->Status {
        for(const auto& range:ranges) {
            if(!range.count || range.count>max_slots-out.size()) return {"empty/oversized descriptor copy range"};
            auto start=cpu_slot(range.heap,range.cpu_start); if(!start) return {start.rejection};
            const auto& h=heaps_.at(range.heap.id).event;
            if(h.type!=type) return {"copy source heap type mismatch"};
            if(source && h.flags) return {"copy source heap must be non-shader-visible"};
            if(range.count>h.count-start.index) return {"copy range exceeds heap"};
            for(uint32_t n=0;n<range.count;++n) {
                auto s=cpu_slot(range.heap,range.cpu_start+uint64_t(n)*h.increment);
                if(!s) return {s.rejection};
                out.push_back(s);
            }
        }
        return {};
    };
    auto d=flatten(dest,destinations,false); if(!d) return reject(d.rejection);
    auto s=flatten(src,sources,true); if(!s) return reject(s.rejection);
    if(destinations.size()!=sources.size()) return fail_write("descriptor copy count mismatch");
    std::set<std::pair<uint64_t,uint32_t>> seen;
    for(const auto& v:destinations) if(!seen.emplace(v.heap.generation,v.index).second) return reject("duplicate copy destination");
    std::set<std::pair<uint64_t,uint32_t>> source_seen;
    std::vector<Snapshot> staged;
    for(const auto& v:sources) {
        auto key=std::make_pair(v.heap.generation,v.index);
        if(seen.count(key)) return fail_write("overlapping source/destination copy slots");
        if(!source_seen.insert(key).second) return reject("duplicate descriptor copy sources");
        auto r=resolve(v); if(!r) return reject(r.rejection);
        auto value=*r.snapshot;
        value.source_heap=value.heap; value.source_slot=value.slot; value.source_revision=value.revision;
        staged.push_back(std::move(value));
    }
    // Reserve all revisions before changing slots; exhaustion fails closed.
    std::vector<std::shared_ptr<const Snapshot>> owned;
    for(size_t n=0;n<staged.size();++n) {
        auto revision=serial(); if(!revision) return reject("revision exhausted");
        auto& v=staged[n]; v.heap=destinations[n].heap; v.slot=destinations[n].index; v.revision=revision;
        owned.push_back(std::make_shared<const Snapshot>(std::move(v)));
    }
    // Allocate destination map nodes BEFORE modifying registry state. Swaps
    // below are noexcept. Any exception clears all slots before propagating.
    std::map<uint64_t,decltype(Heap::slots)> updated;
    for(size_t n=0;n<owned.size();++n) {
        const auto id=destinations[n].heap.id;
        auto i=updated.find(id);
        if(i==updated.end()) i=updated.emplace(id,heaps_.at(id).slots).first;
        i->second[destinations[n].index]=owned[n];
    }
    for(auto& entry:updated) heaps_.at(entry.first).slots.swap(entry.second);
    return {};
} catch(...) {
    invalidate_all();
    throw;
}
} // namespace acz::binding
