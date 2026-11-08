#include "../../src/acz-vr/buffer_binding_registry.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

// Test-only allocation seam: fail the Nth ordinary allocation made inside an
// event, including diagnostics, snapshot ownership, vectors and staged maps.
// Reset before inspecting results; production has no fault-injection interface.
static int allocation_budget=-1;
void* operator new(std::size_t bytes) {
    if(allocation_budget==0) throw std::bad_alloc();
    if(allocation_budget>0) --allocation_budget;
    if(auto p=std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace acz::binding;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) {std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1);} } while(0)
constexpr uint64_t top=std::numeric_limits<uint64_t>::max();
static_assert(D3D12_SRV_DIMENSION_BUFFER==1 && D3D12_RESOURCE_DIMENSION_BUFFER==1);
static_assert(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV==0 && D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE==1);
static_assert(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT==256);
static_assert(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT*16==65536);

struct Fixture {
    BufferBindingRegistry registry;
    Token cpu,gpu,other,resource;
    Fixture() {
        auto c=registry.add_heap({1,0x1000,0,8,32,0,0,0}); CHECK(c); cpu=c.token;
        auto g=registry.add_heap({2,0x2000,0x8000,8,64,0,1,0}); CHECK(g); gpu=g.token;
        auto o=registry.add_heap({3,0x3000,0x9000,8,16,0,1,0}); CHECK(o); other=o.token;
        auto r=registry.add_resource({10,0x10000,8192,1,false}); CHECK(r); resource=r.token;
    }
    StructuredSrv srv(uint64_t first=2,uint32_t count=3,uint32_t stride=416) const {
        return {resource,first,count,stride,1,0,0};
    }
};

// Malformed invocation shape must lose all current provenance, even on heaps
// outside the declared destination set. Historical snapshots remain immutable.
void malformed_copy_regression(unsigned scenario) {
    Fixture f; auto& r=f.registry;
    CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
    CHECK(r.write_srv(f.cpu,0x1020,f.srv(1,1,16)));
    CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
    CHECK(r.write_srv(f.other,0x3000,f.srv()));
    auto source=r.resolve_cpu(f.cpu,0x1000).snapshot;
    auto neighbor=r.resolve_cpu(f.cpu,0x1020).snapshot;
    auto destination=r.resolve_cpu(f.gpu,0x2000).snapshot;
    auto distant=r.resolve_cpu(f.other,0x3000).snapshot;
    std::vector<CopyRange> dst{{f.gpu,0x2000,2}},src{{f.cpu,0x1000,1}};
    Token ambiguity{};
    switch(scenario) {
    case 0: break; // unequal totals, different heaps
    case 1: src.clear(); break;
    case 2: dst={{f.cpu,0x1020,2}}; src={{f.cpu,0x1000,2}}; break;
    case 3: dst={{f.cpu,0x1000,1}}; src=dst; break;
    case 4: dst={{f.gpu,0x2000,1},{f.other,0x3000,1}}; break;
    case 5: src={{f.cpu,0x1000,1},{f.cpu,0x1020,0xffffffff}}; break;
    case 6: dst={{f.cpu,0x1040,1},{f.gpu,0x2000,1}};
            src={{f.cpu,0x1000,1},{f.cpu,0x1040,1}}; break;
    case 7: dst={{f.gpu,0x2000,1}}; src={{f.gpu,0x2000,1}}; break; // visible overlap
    case 8: src={{f.cpu,0x1000,0}}; break;
    case 9: { // an alias strictly between destination descriptor starts
        auto h=r.add_heap({4,0x2010,0,1,16}); CHECK(h); ambiguity=h.token;
        dst={{f.gpu,0x2000,1}}; src={{h.token,0x2010,1}};
        break;
    }
    }
    CHECK(!r.copy(dst,src));
    if(ambiguity.id) CHECK(r.remove_heap(ambiguity));
    CHECK(!r.current(*distant)); // discriminates the pre-J-025 local policy
    CHECK(!r.current(*source)); CHECK(!r.current(*neighbor)); CHECK(!r.current(*destination));
    CHECK(!r.resolve_cpu(f.cpu,0x1000)); CHECK(!r.resolve_cpu(f.cpu,0x1020));
    CHECK(!r.resolve_cpu(f.gpu,0x2000)); CHECK(!r.resolve_cpu(f.other,0x3000));
    CHECK(source->descriptor.first_element==2 && source->descriptor.view.stride==416);
    CHECK(neighbor->descriptor.view.stride==16 && distant->descriptor.elements==3);
    CHECK(destination->revision!=source->revision);
}

void mapping_and_metadata() {
    Fixture f; auto& r=f.registry;
    // Explicit synthetic SDK layouts with DIFFERENT increments on each heap.
    D3D12_DESCRIPTOR_HEAP_DESC sdk{}; sdk.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sdk.NumDescriptors=8; sdk.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for(uint32_t n=0;n<sdk.NumDescriptors;++n) {
        auto cpu=r.cpu_slot(f.gpu,0x2000+n*64),gpu=r.gpu_slot(f.gpu,0x8000+n*64);
        CHECK(cpu && gpu && cpu.index==n && gpu.index==n && cpu.heap==gpu.heap);
        CHECK(r.cpu_slot(f.cpu,0x1000+n*32).index==n);
        CHECK(r.gpu_slot(f.other,0x9000+n*16).index==n);
    }
    CHECK(!r.cpu_slot(f.cpu,0)); CHECK(!r.cpu_slot(f.cpu,0x1001));
    CHECK(!r.cpu_slot(f.cpu,0x1100)); CHECK(!r.cpu_slot(f.cpu,0xfff));
    CHECK(!r.cpu_slot(f.cpu,0x2000)); CHECK(!r.gpu_slot(f.cpu,0x1000));
    CHECK(!r.gpu_slot(f.gpu,0x8200)); CHECK(!r.gpu_slot(f.gpu,0x8001));
    CHECK(!r.resolve_cpu(f.cpu,0x1000)); CHECK(!r.cpu_slot({},0x1000));
    CHECK(r.write_srv(f.cpu,0x1060,f.srv()));
    auto source=r.resolve_cpu(f.cpu,0x1060); CHECK(source && source.snapshot);
    const auto immutable=source.snapshot;
    D3D12_BUFFER_SRV sdk_srv{2,3,416,D3D12_BUFFER_SRV_FLAG_NONE};
    const auto& d=immutable->descriptor;
    CHECK(d.first_element==sdk_srv.FirstElement && d.elements==sdk_srv.NumElements && d.view.stride==sdk_srv.StructureByteStride);
    CHECK(d.view.offset==0 && d.view.size==8192 && d.structured && d.buffer);
    CHECK(d.view.offset+d.first_element*d.view.stride==832);
    // Range flattening and increment arithmetic, not assumed contiguous tokens.
    CHECK(!r.copy({{f.gpu,0x2080,1},{f.other,0x3030,1}},{{f.cpu,0x1060,1},{f.cpu,0x1060+32,1}}));
    // Unknown source invalidates only the proven destinations, preserving source.
    CHECK(r.current(*immutable)); CHECK(immutable->descriptor.first_element==2);
    CHECK(r.write_srv(f.cpu,0x1060,f.srv()));
    CHECK(r.write_cbv(f.cpu,0x1080,{f.resource,0x10100,512}));
    CHECK(r.copy({{f.gpu,0x2080,1},{f.other,0x3030,1}},{{f.cpu,0x1060,2}}));
    auto a=r.resolve_gpu(f.gpu,0x8080),b=r.resolve_gpu(f.other,0x9030);
    CHECK(a && b); CHECK(a.snapshot->slot==2 && b.snapshot->slot==3);
    CHECK(a.snapshot->source_heap==f.cpu && a.snapshot->source_slot==3);
    CHECK(b.snapshot->descriptor.kind==acz::metadata::BindingKind::cbv);
    CHECK(b.snapshot->descriptor.view.offset==256 && b.snapshot->descriptor.view.size==512);
    auto copied=a.snapshot;
    CHECK(r.write_srv(f.cpu,0x1060,f.srv(4,2,512)));
    CHECK(r.current(*copied)); CHECK(copied->descriptor.first_element==2 && copied->descriptor.view.stride==416);
    CHECK(r.remove_heap(f.cpu)); CHECK(r.current(*copied));
    CHECK(r.resolve_gpu(f.gpu,0x8080));
    CHECK(r.write_srv(f.gpu,0x2080,f.srv(1,1,2048)));
    CHECK(!r.current(*copied)); CHECK(copied->descriptor.elements==3);
    auto root=r.root_srv(f.resource,0x1000c,3,416); CHECK(root);
    CHECK(root.snapshot->descriptor.view.offset==12 && root.snapshot->descriptor.view.size==1248);
    CHECK(root.snapshot->descriptor.first_element==0 && root.snapshot->descriptor.view.stride==416);
    CHECK(r.current(*root.snapshot));
    auto cb=r.root_cbv(f.resource,0x10100,256); CHECK(cb && cb.snapshot->descriptor.view.offset==256);
    CHECK(!r.root_srv(f.resource,0x10001,3,416)); CHECK(!r.root_srv(f.resource,0x10000,0,416));
    CHECK(!r.root_srv(f.resource,0x10000,1,3)); CHECK(!r.root_srv(f.resource,0x10000,1,2052));
    CHECK(!r.root_cbv(f.resource,0x10001,256)); CHECK(!r.root_cbv(f.resource,0x10000,0));
    CHECK(!r.root_cbv(f.resource,0x10000,257)); CHECK(!r.root_cbv(f.resource,0x10000,65792));
    CHECK(!r.root_cbv(f.resource,0x11f00,512));
}

void lifetimes_and_ambiguity() {
    Fixture f; auto& r=f.registry;
    CHECK(r.write_srv(f.gpu,0x2000,f.srv())); auto old=r.resolve_gpu(f.gpu,0x8000).snapshot;
    auto t=f.resource;
    CHECK(r.remove_resource(t)); CHECK(!r.resolve_gpu(f.gpu,0x8000)); CHECK(!r.current(*old));
    auto replacement=r.add_resource({10,0x10000,8192}); CHECK(replacement && replacement.token!=t);
    CHECK(!r.buffer_address(t,0x10000,4)); CHECK(!r.remove_resource(t));
    f.resource=replacement.token; CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
    auto old_heap=f.gpu; CHECK(r.remove_heap(old_heap));
    auto new_heap=r.add_heap({2,0x2000,0x8000,8,64,0,1}); CHECK(new_heap && new_heap.token!=old_heap);
    CHECK(!r.cpu_slot(old_heap,0x2000)); CHECK(!r.remove_heap(old_heap));
    CHECK(!r.resolve_gpu(new_heap.token,0x8000)); f.gpu=new_heap.token;
    auto overlap=r.add_heap({4,0x2020,0x8020,4,32,0,1}); CHECK(overlap);
    CHECK(!r.cpu_slot(f.gpu,0x2040)); CHECK(!r.gpu_slot(f.gpu,0x8040));
    CHECK(!r.cpu_slot(overlap.token,0x2040)); CHECK(r.remove_heap(overlap.token));
    CHECK(r.cpu_slot(f.gpu,0x2040));
    auto address=r.buffer_address(f.resource,0x1000c,416); CHECK(address && address.offset==12);
    CHECK(!r.buffer_address(f.resource,0x12000,1)); CHECK(!r.buffer_address(f.resource,0xffff,1));
    CHECK(!r.buffer_address(f.resource,0x11fff,2)); CHECK(!r.buffer_address(f.resource,0x10000,0));
    CHECK(!r.buffer_address(f.resource,top-3,8)); CHECK(!r.buffer_address({},0x10000,4));
    auto shared=r.add_resource({11,0x10100,512}); CHECK(shared);
    CHECK(!r.buffer_address(f.resource,0x10000,1024)); CHECK(!r.root_cbv(f.resource,0x10100,256));
    CHECK(!r.buffer_address(shared.token,0x10100,256)); CHECK(r.remove_resource(shared.token));
    CHECK(r.buffer_address(f.resource,0x10100,256));
    CHECK(r.write_cbv(f.gpu,0x2000,{f.resource,0x10100,256}));
    auto cb=r.resolve_gpu(f.gpu,0x8000).snapshot;
    auto root=r.root_srv(f.resource,0x10000,4,416); CHECK(root);
    auto later_overlap=r.add_resource({12,0x10100,256}); CHECK(later_overlap);
    CHECK(!r.current(*cb)); CHECK(!r.current(*root.snapshot));
    CHECK(!r.resolve_gpu(f.gpu,0x8000));
    CHECK(r.remove_resource(later_overlap.token)); CHECK(r.current(*cb));
    auto later_heap=r.add_heap({5,0x2000,0x8000,1,64,0,1}); CHECK(later_heap);
    CHECK(!r.current(*cb)); CHECK(!r.resolve_gpu(f.gpu,0x8000));
    CHECK(r.remove_heap(later_heap.token)); CHECK(r.current(*cb));
    // Same identity/VA in an independent registry still has a unique generation.
    Fixture another; CHECK(another.resource!=f.resource);
    CHECK(!another.registry.buffer_address(f.resource,0x10000,4));
    std::shared_ptr<const Snapshot> owned;
    {Fixture temporary; CHECK(temporary.registry.write_srv(temporary.cpu,0x1000,temporary.srv())); owned=temporary.registry.resolve_cpu(temporary.cpu,0x1000).snapshot;}
    CHECK(owned && owned->descriptor.elements==3 && owned->descriptor.view.stride==416);
}

void invalid_events() {
    BufferBindingRegistry r;
    CHECK(!r.add_heap({})); CHECK(!r.add_heap({1,1,0,0,32}));
    CHECK(!r.add_heap({1,1,0,8,0})); CHECK(!r.add_heap({1,top-8,0,2,8}));
    CHECK(!r.add_heap({1,1,top-8,2,8,0,1})); CHECK(!r.add_heap({1,1,0,2,8,0,1}));
    CHECK(!r.add_heap({1,1,100,2,8,0,0})); CHECK(!r.add_heap({1,1,0,2,8,2})); // RTV stays unsupported; sampler now supported.
    CHECK(!r.add_heap({1,1,0,2,8,0,2})); CHECK(!r.add_heap({1,1,0,2,8,0,0,1}));
    CHECK(!r.add_resource({})); CHECK(!r.add_resource({1,top-8,16}));
    CHECK(!r.add_resource({1,16,16,2})); CHECK(!r.add_resource({1,16,16,1,true}));
    Fixture f; auto& reg=f.registry;
    CHECK(!reg.add_heap({1,0x4000,0,1,32})); CHECK(!reg.add_resource({10,0x20000,100}));
    auto bad=[&](StructuredSrv d) {
        CHECK(reg.write_srv(f.cpu,0x1000,f.srv()));
        CHECK(!reg.write_srv(f.cpu,0x1000,d)); CHECK(!reg.resolve_cpu(f.cpu,0x1000));
    };
    auto d=f.srv(); d.flags=D3D12_BUFFER_SRV_FLAG_RAW; bad(d);
    d=f.srv(); d.format=DXGI_FORMAT_R32_UINT; bad(d);
    d=f.srv(); d.dimension=D3D12_SRV_DIMENSION_TEXTURE2D; bad(d);
    d=f.srv(); d.resource={}; bad(d);
    d=f.srv(); d.elements=0; bad(d);
    d=f.srv(); d.stride=0; bad(d);
    d=f.srv(); d.stride=3; bad(d);
    d=f.srv(); d.stride=2052; bad(d);
    d=f.srv(); d.first_element=top; bad(d);
    d=f.srv(); d.first_element=19; d.elements=1; bad(d);
    d=f.srv(); d.elements=0xffffffff; bad(d);
    CHECK(reg.write_srv(f.cpu,0x1000,f.srv(0,4,2048)));
    CHECK(reg.write_cbv(f.cpu,0x1020,{f.resource,0x11f00,256}));
    CHECK(!reg.write_cbv(f.cpu,0x1020,{f.resource,0x11f00,512}));
    CHECK(reg.resolve_cpu(f.cpu,0x1000));
    CHECK(!reg.resolve_cpu(f.cpu,0x1020));
    CHECK(reg.write_srv(f.cpu,0x1000,f.srv()));
    CHECK(reg.invalidate_write(f.cpu,0x1000)); CHECK(!reg.resolve_cpu(f.cpu,0x1000));
    CHECK(reg.write_srv(f.cpu,0x1000,f.srv()));
    CHECK(!reg.invalidate_write(f.cpu,0x1001)); CHECK(!reg.resolve_cpu(f.cpu,0x1000));
}

void copy_failures() {
    Fixture f; auto& r=f.registry;
    auto fresh=[&]() {CHECK(r.write_srv(f.cpu,0x1000,f.srv())); CHECK(r.write_srv(f.cpu,0x1020,f.srv(1,1,16))); CHECK(r.write_srv(f.gpu,0x2000,f.srv()));};
    auto bad=[&](std::vector<CopyRange> dst,std::vector<CopyRange> src,uint32_t type=0,bool cpu_only=false) {
        fresh(); auto previous=r.resolve_gpu(f.gpu,0x8000).snapshot;
        CHECK(!r.copy(dst,src,type));
        if(cpu_only) {
            CHECK(r.resolve_gpu(f.gpu,0x8000)); CHECK(r.current(*previous));
            for(const auto& range:dst)
                for(uint32_t n=0;n<range.count;++n)
                    CHECK(!r.resolve_cpu(range.heap,range.cpu_start+uint64_t(n)*32));
        } else { CHECK(!r.resolve_gpu(f.gpu,0x8000)); CHECK(!r.current(*previous)); }
    };
    bad({{f.gpu,0x2000,1}},{{f.gpu,0x2040,1}}); // visible source forbidden
    bad({{f.gpu,0x2000,1}},{{f.cpu,0x1000,1}},1); // sampler type
    bad({},{{f.cpu,0x1000,1}}); bad({{f.gpu,0x2000,1}},{});
    bad({{f.gpu,0x2000,0}},{{f.cpu,0x1000,1}});
    bad({{f.gpu,0x2000,0xffffffff}},{{f.cpu,0x1000,1}});
    bad({{f.gpu,0x21c0,2}},{{f.cpu,0x1000,2}});
    bad({{f.gpu,0x2000,2}},{{f.cpu,0x1000,1}});
    bad({{f.gpu,0x2001,1}},{{f.cpu,0x1000,1}});
    bad({{f.gpu,top,1}},{{f.cpu,0x1000,1}});
    bad({{f.cpu,0x1000,1}},{{f.cpu,0x1000,1}}); // exact overlap
    bad({{f.cpu,0x1020,2}},{{f.cpu,0x1000,2}}); // partial overlap
    bad({{f.gpu,0x2000,1},{f.gpu,0x2000,1}},{{f.cpu,0x1000,2}});
    bad({{f.gpu,0x2000,2}},{{f.cpu,0x1000,1},{f.cpu,0x1000,1}});
    bad({{f.gpu,0x2000,1}},{{f.cpu,0x1040,1}}); // unknown descriptor
    fresh(); CHECK(r.copy({{f.cpu,0x1040,2}},{{f.cpu,0x1000,2}})); // disjoint same-heap
    CHECK(r.resolve_cpu(f.cpu,0x1040).snapshot->descriptor.first_element==2);
    CHECK(r.resolve_cpu(f.cpu,0x1060).snapshot->descriptor.view.stride==16);
    fresh(); CHECK(r.copy({{f.gpu,0x2000,2}},{{f.cpu,0x1000,2}}));
    CHECK(r.remove_resource(f.resource)); CHECK(!r.resolve_gpu(f.gpu,0x8000));
    CHECK(!r.copy({{f.other,0x9000,1}},{{f.cpu,0x1000,1}}));
}

// J-023 regression: on the pre-change registry this fails at the unaffected
// snapshot's current() check after a rejected known-destination SRV write.
void known_destination_regression() {
    Fixture f; auto& r=f.registry;
    CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
    CHECK(r.write_srv(f.cpu,0x1020,f.srv()));
    CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
    auto dst=r.resolve_cpu(f.cpu,0x1000).snapshot;
    auto neighbor=r.resolve_cpu(f.cpu,0x1020).snapshot;
    auto other=r.resolve_cpu(f.gpu,0x2000).snapshot;
    auto unsupported=f.srv(); unsupported.flags=1;
    CHECK(!r.write_srv(f.cpu,0x1000,unsupported));
    CHECK(!r.current(*dst)); CHECK(r.current(*neighbor)); CHECK(r.current(*other));
    CHECK(dst->descriptor.view.stride==416);
    CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
    dst=r.resolve_cpu(f.cpu,0x1000).snapshot;
    CHECK(!r.copy({{f.cpu,0x1000,1}},{{{},0xdead,1}}));
    CHECK(!r.current(*dst)); CHECK(r.current(*neighbor)); CHECK(r.current(*other));
}


void complete_destination_policy() {
    // Every failed multi-range event erases the full destination set, even when
    // the first source was valid. Neighbor/source revisions remain unchanged.
    for(unsigned scenario=0;scenario<8;++scenario) {
        Fixture f; auto& r=f.registry;
        CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
        CHECK(r.write_srv(f.cpu,0x1020,f.srv(1,1,16)));
        CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
        CHECK(r.write_srv(f.gpu,0x2040,f.srv()));
        CHECK(r.write_srv(f.gpu,0x2080,f.srv()));
        CHECK(r.write_srv(f.other,0x3000,f.srv()));
        CHECK(r.write_srv(f.other,0x3010,f.srv()));
        auto source=r.resolve_cpu(f.cpu,0x1000).snapshot;
        auto a=r.resolve_cpu(f.gpu,0x2000).snapshot;
        auto b=r.resolve_cpu(f.gpu,0x2040).snapshot;
        auto c=r.resolve_cpu(f.other,0x3000).snapshot;
        auto neighbor=r.resolve_cpu(f.gpu,0x2080).snapshot;
        auto distant=r.resolve_cpu(f.other,0x3010).snapshot;
        std::vector<CopyRange> dst{{f.gpu,0x2000,2},{f.other,0x3000,1}};
        std::vector<CopyRange> src{{f.cpu,0x1000,2},{f.cpu,0x1040,1}};
        if(scenario==1) src[1]={{999,1},0xdead,1}; // unknown heap
        if(scenario==2) src[1]={f.cpu,0x1041,1}; // misaligned source
        if(scenario==3) src={{f.cpu,0x1000,1}}; // count mismatch
        if(scenario==4) src={{f.cpu,0x1000,2},{f.cpu,0x1000,1}}; // duplicate source
        if(scenario==5) src={{f.cpu,0x1000,2},{f.gpu,0x2080,1}}; // visible source
        if(scenario==6) {
            auto dead=r.add_heap({4,0x4000,0,1,32}); CHECK(dead);
            CHECK(r.remove_heap(dead.token));
            auto replacement=r.add_heap({4,0x4000,0,1,32}); CHECK(replacement);
            src[1]={dead.token,0x4000,1}; // stale source lifetime at reused address
        }
        if(scenario==7) {
            auto extra=r.add_resource({20,0x20000,8192}); CHECK(extra);
            auto d=f.srv(); d.resource=extra.token;
            CHECK(r.write_srv(f.cpu,0x1040,d)); CHECK(r.remove_resource(extra.token));
        }
        CHECK(!r.copy(dst,src));
        CHECK(!r.current(*a)); CHECK(!r.current(*b)); CHECK(!r.current(*c));
        CHECK(!r.resolve_cpu(f.gpu,0x2000)); CHECK(!r.resolve_cpu(f.gpu,0x2040));
        CHECK(!r.resolve_cpu(f.other,0x3000)); // no partial successful copy
        CHECK(bool(r.current(*source))==(scenario!=3));
        CHECK(bool(r.current(*neighbor))==(scenario!=3)); CHECK(bool(r.current(*distant))==(scenario!=3));
        CHECK(a->descriptor.first_element==2 && a->revision!=source->revision);
        CHECK(r.write_srv(f.gpu,0x2000,f.srv(1,1,16)));
        auto replaced=r.resolve_cpu(f.gpu,0x2000).snapshot;
        CHECK(replaced->revision!=a->revision && !r.current(*a));
        CHECK(bool(r.current(*neighbor))==(scenario!=3) && bool(r.current(*distant))==(scenario!=3));
        auto heap=f.gpu; CHECK(r.remove_heap(heap)); CHECK(!r.current(*replaced));
        auto reuse=r.add_heap({2,0x2000,0x8000,8,64,0,1}); CHECK(reuse);
        CHECK(reuse.token!=heap && !r.current(*replaced));
        CHECK(a->descriptor.view.stride==416 && replaced->descriptor.view.stride==16);
    }
    // Destination proof is all-or-nothing, including a bad range AFTER a valid
    // range. A removed ambiguity must not resurrect erased provenance.
    for(unsigned scenario=0;scenario<13;++scenario) {
        Fixture f; auto& r=f.registry;
        CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
        CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
        CHECK(r.write_srv(f.other,0x3000,f.srv()));
        auto source=r.resolve_cpu(f.cpu,0x1000).snapshot;
        auto affected=r.resolve_cpu(f.gpu,0x2000).snapshot;
        auto unaffected=r.resolve_cpu(f.other,0x3000).snapshot;
        std::vector<CopyRange> dst{{f.gpu,0x2000,1},{f.gpu,0x2040,1}};
        Token ambiguity{};
        switch(scenario) {
        case 0: dst[1].heap={}; break;
        case 1: dst[1].cpu_start=0x2041; break;
        case 2: dst[1].cpu_start=0x2200; break;
        case 3: dst[1].cpu_start=top; break;
        case 4: dst[1].count=0; break;
        case 5: dst[1].count=65537; break;
        case 6: dst[1]={f.gpu,0x21c0,2}; break;
        case 7: dst[1]=dst[0]; break;
        case 8: dst={{f.gpu,0x2000,2},{f.gpu,0x2040,2}}; break;
        case 9: {
            auto dead=r.add_heap({4,0x4000,0,1,32}); CHECK(dead);
            CHECK(r.remove_heap(dead.token));
            CHECK(r.add_heap({4,0x4000,0,1,32}));
            dst[1]={dead.token,0x4000,1}; break;
        }
        case 10: { // overlap strictly BETWEEN the two descriptor starts
            auto h=r.add_heap({4,0x2020,0,1,16}); CHECK(h); ambiguity=h.token; break;
        }
        case 11: dst.clear(); break;
        case 12: dst[1]={{999,1},0xdead,1}; break;
        }
        CHECK(!r.copy(dst,{{f.cpu,0x1000,2}}));
        if(ambiguity.id) CHECK(r.remove_heap(ambiguity));
        CHECK(!r.current(*source)); CHECK(!r.current(*affected)); CHECK(!r.current(*unaffected));
        CHECK(!r.resolve_cpu(f.cpu,0x1000)); CHECK(!r.resolve_cpu(f.gpu,0x2000));
        CHECK(!r.resolve_cpu(f.other,0x3000));
    }
    // Invalid CBV and stale SRV resource writes preserve unrelated descriptors.
    Fixture f; auto& r=f.registry;
    CHECK(r.write_srv(f.cpu,0x1000,f.srv())); CHECK(r.write_srv(f.cpu,0x1020,f.srv()));
    auto neighbor=r.resolve_cpu(f.cpu,0x1020).snapshot;
    CHECK(!r.write_cbv(f.cpu,0x1000,{f.resource,0x10001,256}));
    CHECK(!r.resolve_cpu(f.cpu,0x1000)); CHECK(r.current(*neighbor));
    auto d=f.srv(); d.resource.generation++;
    CHECK(!r.write_srv(f.cpu,0x1000,d)); CHECK(r.current(*neighbor));
    CHECK(r.invalidate_write(f.cpu,0x1000)); CHECK(r.current(*neighbor));
    CHECK(!r.write_srv({},0x1000,d)); CHECK(!r.current(*neighbor));
}


void write_destination_fallbacks() {
    for(unsigned event=0;event<3;++event) for(unsigned scenario=0;scenario<6;++scenario) {
        Fixture f; auto& r=f.registry;
        CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
        CHECK(r.write_srv(f.other,0x3000,f.srv()));
        auto a=r.resolve_cpu(f.cpu,0x1000).snapshot;
        auto b=r.resolve_cpu(f.other,0x3000).snapshot;
        Token heap=f.cpu,ambiguity{}; uint64_t handle=0x1000;
        switch(scenario) {
        case 0: heap={}; break;
        case 1: ++heap.generation; break;
        case 2: handle=0x1001; break;
        case 3: handle=top; break;
        case 4: handle=0xdead; break;
        case 5: {
            auto h=r.add_heap({4,0x1010,0,1,8}); CHECK(h); ambiguity=h.token; break;
        }
        }
        if(event==0) CHECK(!r.write_srv(heap,handle,f.srv()));
        if(event==1) CHECK(!r.write_cbv(heap,handle,{f.resource,0x10000,256}));
        if(event==2) CHECK(!r.invalidate_write(heap,handle));
        if(ambiguity.id) CHECK(r.remove_heap(ambiguity));
        CHECK(!r.current(*a)); CHECK(!r.current(*b));
        CHECK(!r.resolve_cpu(f.cpu,0x1000)); CHECK(!r.resolve_cpu(f.other,0x3000));
    }
    Fixture f; auto& r=f.registry;
    auto large=r.add_heap({4,0x4000,0,65537,32}); CHECK(large);
    CHECK(r.write_srv(large.token,0x4000,f.srv()));
    constexpr uint64_t last=0x4000+uint64_t(65536)*32;
    CHECK(r.write_srv(large.token,last,f.srv()));
    CHECK(r.write_srv(f.other,0x3000,f.srv()));
    auto tail=r.resolve_cpu(large.token,last).snapshot;
    auto neighbor=r.resolve_cpu(f.other,0x3000).snapshot;
    CHECK(!r.copy({{large.token,0x4000,65536}},{}));
    CHECK(!r.resolve_cpu(large.token,0x4000));
    CHECK(!r.current(*tail)); CHECK(!r.current(*neighbor));
    // Aggregate count exceeds adapter bound although each individual range fits.
    CHECK(!r.copy({{large.token,0x4000,65536},{large.token,last,1}},{}));
    CHECK(!r.current(*tail)); CHECK(!r.current(*neighbor));
}

void copy_shape_boundaries() {
    // Validate aggregate counts before source token/metadata resolution.
    for(unsigned n=0;n<3;++n) {
        Fixture f; auto& r=f.registry;
        CHECK(r.write_srv(f.other,0x3000,f.srv()));
        auto old=r.resolve_cpu(f.other,0x3000).snapshot;
        std::vector<CopyRange> src{{{},top,65536}};
        if(n==1) src.push_back({{},top,1}); // bound exceeded without overflow
        if(n==2) src={{ {},top,0xffffffff },{ {},top,0xffffffff }};
        CHECK(!r.copy({{f.gpu,0x2000,1}},src)); CHECK(!r.current(*old));
        CHECK(old->descriptor.view.stride==416);
    }
    Fixture f; auto& r=f.registry;
    auto highHeap=r.add_heap({4,top-128,0,4,32}); CHECK(highHeap);
    CHECK(r.write_srv(highHeap.token,top-128,f.srv()));
    CHECK(r.write_srv(f.other,0x3000,f.srv()));
    auto distant=r.resolve_cpu(f.other,0x3000).snapshot;
    // Touching interval endpoints do not overlap, even near uint64_t's limit.
    CHECK(r.copy({{highHeap.token,top-96,1}},{{highHeap.token,top-128,1}}));
    CHECK(r.current(*distant)); CHECK(r.resolve_cpu(highHeap.token,top-96));
    CHECK(!r.copy({{highHeap.token,top-96,2}},{{highHeap.token,top-128,2}}));
    CHECK(!r.current(*distant)); CHECK(!r.resolve_cpu(highHeap.token,top-96));
    // Maximum supported equal totals still use local unknown-source rejection.
    auto large=r.add_heap({5,0x4000,0,65536,32}); CHECK(large);
    CHECK(r.write_srv(f.other,0x3000,f.srv()));
    distant=r.resolve_cpu(f.other,0x3000).snapshot;
    CHECK(!r.copy({{large.token,0x4000,65536}},{{{},top,65536}}));
    CHECK(r.current(*distant));
}

void allocation_failure_policy() {
    // Walk every ordinary allocation in each event until one run succeeds.
    // Saturating serial exhaustion and non-allocation platform exceptions are
    // not practically injectable here; source audit must verify those branches.
    for(unsigned event=0;event<9;++event) {
        bool completed=false; unsigned failures=0;
        for(int budget=0;budget<256 && !completed;++budget) {
            Fixture f; auto& r=f.registry;
            CHECK(r.write_srv(f.cpu,0x1000,f.srv()));
            CHECK(r.write_srv(f.cpu,0x1020,f.srv(1,1,16)));
            CHECK(r.write_srv(f.gpu,0x2000,f.srv()));
            CHECK(r.write_srv(f.other,0x3000,f.srv()));
            auto source=r.resolve_cpu(f.cpu,0x1000).snapshot;
            auto dest=r.resolve_cpu(f.gpu,0x2000).snapshot;
            auto other=r.resolve_cpu(f.other,0x3000).snapshot;
            const std::vector<CopyRange> dst{{f.gpu,0x2000,1},{f.other,0x3000,1}};
            const std::vector<CopyRange> src{{f.cpu,0x1000,2}};
            const std::vector<CopyRange> mismatch{{f.cpu,0x1000,1}},empty;
            const std::vector<CopyRange> overlap{{f.cpu,0x1000,2}};
            auto unsupported=f.srv(); unsupported.flags=1;
            bool threw=false, accepted=false;
            allocation_budget=budget;
            try {
                switch(event) {
                case 0: accepted=bool(r.copy(dst,src)); break;
                case 1: accepted=bool(r.write_srv(f.gpu,0x2000,f.srv())); break;
                case 2: accepted=bool(r.write_cbv(f.gpu,0x2000,{f.resource,0x10000,256})); break;
                case 3: accepted=bool(r.write_srv(f.gpu,0x2000,unsupported)); break;
                case 4: accepted=bool(r.invalidate_write({},0xdead)); break;
                case 5: accepted=bool(r.invalidate_write(f.gpu,0x2000)); break;
                case 6: accepted=bool(r.copy(dst,mismatch)); break;
                case 7: accepted=bool(r.copy(dst,empty)); break;
                case 8: accepted=bool(r.copy(overlap,overlap)); break;
                }
            } catch(const std::bad_alloc&) { threw=true; }
            allocation_budget=-1;
            if(threw) {
                ++failures;
                CHECK(!r.current(*source)); CHECK(!r.current(*dest)); CHECK(!r.current(*other));
                CHECK(!r.resolve_cpu(f.gpu,0x2000)); CHECK(!r.resolve_cpu(f.other,0x3000));
                CHECK(dest->descriptor.view.stride==416 && source->descriptor.elements==3);
            } else {
                completed=true;
                CHECK(accepted==(event<3 || event==5));
                CHECK(!r.current(*dest));
                if(event==0) {
                    CHECK(!r.current(*other)); CHECK(r.current(*source));
                    auto a=r.resolve_cpu(f.gpu,0x2000),b=r.resolve_cpu(f.other,0x3000);
                    CHECK(a && b && a.snapshot->revision!=b.snapshot->revision);
                    CHECK(a.snapshot->source_revision==source->revision);
                    CHECK(b.snapshot->descriptor.view.stride==16);
                } else if(event<4 || event==5) { CHECK(r.current(*source)); CHECK(r.current(*other)); }
                else { CHECK(!r.current(*source)); CHECK(!r.current(*other)); }
            }
        }
        CHECK(completed); CHECK(event==5 ? failures==0 : failures>0);
    }
}

void sampler_metadata() {
    BufferBindingRegistry r;
    auto source=r.add_heap({11,0x11000,0,8,16,1});CHECK(source);
    auto destination=r.add_heap({12,0x12000,0x18000,8,32,1,1});CHECK(destination);
    auto neighbor=r.add_heap({13,0x13000,0,8,64,1});CHECK(neighbor);
    auto buffer=r.add_heap({14,0x14000,0,8,32});CHECK(buffer);
    Sampler sampler;for(unsigned n=0;n<sampler.words.size();++n)sampler.words[n]=0x3f000000+n;
    auto seed=[&] {
        for(unsigned n=0;n<8;++n) {
            CHECK(r.write_sampler(source.token,0x11000+n*16,sampler));
            CHECK(r.write_sampler(destination.token,0x12000+n*32,sampler));
            CHECK(r.write_sampler(neighbor.token,0x13000+n*64,sampler));
        }
    };
    seed();
    auto original=r.resolve_cpu(source.token,0x11000);CHECK(original);
    CHECK(original.snapshot->descriptor.kind==acz::metadata::BindingKind::sampler);
    CHECK(!original.snapshot->descriptor.buffer && !original.snapshot->resource.id && !original.snapshot->resource_bytes);
    CHECK(original.snapshot->sampler.words==sampler.words);
    CHECK(r.copy({{destination.token,0x12000,2}},{{source.token,0x11000,2}},1));
    auto copied=r.resolve_gpu(destination.token,0x18000);CHECK(copied);
    CHECK(copied.snapshot->source_heap==source.token && copied.snapshot->source_revision==original.snapshot->revision);
    sampler.words[0]=99;CHECK(r.write_sampler(source.token,0x11000,sampler));
    CHECK(!r.current(*original.snapshot) && r.current(*copied.snapshot));
    CHECK(original.snapshot->sampler.words[0]==0x3f000000 && copied.snapshot->sampler.words==original.snapshot->sampler.words);
    CHECK(r.remove_heap(source.token));CHECK(r.current(*copied.snapshot));
    auto old=source.token;source=r.add_heap({11,0x11000,0,8,16,1});CHECK(source && source.token!=old);
    CHECK(!r.resolve_cpu(old,0x11000));CHECK(!r.write_sampler(old,0x11000,sampler));
    CHECK(!r.current(*copied.snapshot)); // stale destination proof falls back globally
    seed();
    // Unknown source and mismatched types erase only the known destinations.
    for(unsigned scenario=0;scenario<4;++scenario) {
        seed();auto distant=r.resolve_cpu(neighbor.token,0x13000).snapshot;
        auto beside=r.resolve_cpu(destination.token,0x12040).snapshot;
        auto target=r.resolve_cpu(destination.token,0x12000).snapshot;
        if(scenario==0)CHECK(!r.copy({{destination.token,0x12000,1}},{{old,0x11000,1}},1));
        if(scenario==1)CHECK(!r.copy({{destination.token,0x12000,1}},{{buffer.token,0x14000,1}},1));
        if(scenario==2)CHECK(!r.copy({{destination.token,0x12000,1}},{{source.token,0x11000,1}},0));
        if(scenario==3)CHECK(!r.write_cbv(destination.token,0x12000,{}));
        CHECK(!r.current(*target) && r.current(*beside) && r.current(*distant));
        CHECK(target->sampler.words==sampler.words);
    }
    // Duplicate sources use local invalidation; duplicate/overlapping destinations,
    // native interval overlap and malformed counts retain the global fallback.
    for(unsigned scenario=0;scenario<5;++scenario) {
        seed();auto distant=r.resolve_cpu(neighbor.token,0x13000).snapshot;
        std::vector<CopyRange> dst{{destination.token,0x12000,2}},src{{source.token,0x11000,2}};
        if(scenario==0)src={{source.token,0x11000,1},{source.token,0x11000,1}};
        if(scenario==1)dst={{destination.token,0x12000,1},{destination.token,0x12000,1}};
        if(scenario==2)dst={{source.token,0x11010,2}};
        if(scenario==3)src[0].count=1;
        if(scenario==4)dst={{destination.token,0x12001,2}};
        CHECK(!r.copy(dst,src,1));
        CHECK(bool(r.current(*distant))==(scenario==0));
    }
    seed();auto retained=r.resolve_gpu(destination.token,0x18000).snapshot;
    auto token=destination.token;CHECK(r.remove_heap(token));
    CHECK(retained->sampler.words==sampler.words && !r.current(*retained));
    auto fresh=r.add_heap({12,0x12000,0x18000,8,32,1,1});CHECK(fresh && fresh.token!=token);
    CHECK(!r.resolve_gpu(fresh.token,0x18000));CHECK(!r.write_sampler(token,0x12000,sampler));
    destination=fresh;
    // The existing operator-new seam sweeps sampler publication and copy staging.
    for(unsigned event=0;event<2;++event) {
        bool completed=false;unsigned failures=0;
        for(int budget=0;budget<64 && !completed;++budget) {
            seed(); // destination now uses fresh lifetime below
            auto current=r.resolve_cpu(source.token,0x11000).snapshot;
            bool threw=false;bool accepted=false;
            std::vector<CopyRange> dst{{neighbor.token,0x13000,1}},src{{source.token,0x11000,1}};
            allocation_budget=budget;
            try {
                if(event==0)accepted=bool(r.write_sampler(source.token,0x11000,sampler));
                else accepted=bool(r.copy(dst,src,1));
            } catch(const std::bad_alloc&) {threw=true;}
            allocation_budget=-1;
            if(threw) {++failures;CHECK(!r.current(*current));}
            else {completed=true;CHECK(accepted);}
        }
        CHECK(completed && failures>0);
    }
}

int main(int argc,char** argv) {
    if(argc==2) {malformed_copy_regression(unsigned(std::atoi(argv[1]))); return 0;}
    for(unsigned n=0;n<10;++n) malformed_copy_regression(n);
    sampler_metadata();
    copy_shape_boundaries();
    known_destination_regression(); complete_destination_policy(); write_destination_fallbacks(); allocation_failure_policy();
    mapping_and_metadata(); lifetimes_and_ambiguity(); invalid_events(); copy_failures();
    std::cout << checks << " CPU-only registry checks passed; synthetic metadata only, no GPU bytes or runtime VR proof\n";
}
