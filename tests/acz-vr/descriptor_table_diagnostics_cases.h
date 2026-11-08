#pragma once
// Included by existing offline targets. Small maps/unique_ptrs only; no large
// automatic registry/contract arrays and no native runtime dependencies here.
#include "../../src/acz-vr/descriptor_table_diagnostics.h"
inline void descriptorTableDiagnosticCases() {
    using namespace acz::diagnostics;
    using D=DescriptorTableDiagnostics;using C=RootConstantWitness;
    D d;
    d.heap(90,7,1000,2000,8,32,0);
    auto location=d.bound(90,2000,7);
    CHECK(!location.flags&&location.slot==0&&location.serial==1);
    CHECK(d.snapshot(location,0).flags&D::unwritten);
    d.write(1000,7,D::srv,4,28); // Texture2D, R8G8B8A8_UNORM: observed, unsupported.
    auto s=d.snapshot(location,0);
    CHECK(s.type==D::srv&&s.dimension==4&&s.format==28);
    CHECK((s.flags&(D::unsupported_srv|D::unknown_resource))==(D::unsupported_srv|D::unknown_resource));
    CHECK(!(s.flags&D::unwritten));
    d.write(1000,7,D::cbv);s=d.snapshot(location,0);
    CHECK((s.flags&(D::overwritten|D::wrong_type))==(D::overwritten|D::wrong_type));
    d.write(1000,7,D::uav);CHECK(d.snapshot(location,0).type==D::uav);
    d.write(1000,7,D::sampler);CHECK(d.snapshot(location,0).type==D::sampler);
    D::Slot copied;copied.type=D::srv;copied.dimension=1;copied.resource_known=true;copied.flags=D::copied;
    d.store(location,copied);CHECK(d.snapshot(location,0).flags&D::copied);
    d.retire(90);CHECK(d.snapshot(location,0).flags&D::stale_heap);
    d.heap(90,7,1000,2000,8,32,0);
    CHECK(d.snapshot(location,0).flags&D::stale_heap);
    location=d.bound(90,2000,7);CHECK(d.snapshot(location,0).flags&D::unwritten);
    CHECK(d.bound(90,2001,7).flags&D::unknown_base);
    CHECK(d.bound(90,2000,8).flags&D::unknown_base);
    CHECK(d.snapshot(location,UINT32_MAX).flags&D::unknown_base);
    d.heap(91,7,3000,4000,2,32,1);
    CHECK(d.snapshot(d.bound(91,4000,7),0).flags&D::wrong_heap);
    d.overflow=true;CHECK(d.snapshot(location,0).flags&D::capacity);
    d.history_complete=false;CHECK(d.snapshot(location,0).flags&D::incomplete);
    d.invalidate();CHECK(d.snapshot(location,0).flags&D::invalidated);

    // Last-write ordering is diagnostic only, conservatively unknown when a
    // writer spans a boundary or the table/history is not fully covered.
    D history;history.heap(1,7,1000,2000,4,32,0,1);
    auto base=history.bound(1,2000,7);
    history.write(1000,7,D::srv,4,28,0,0,0,D::create_srv,2);
    history.write(1000,7,D::srv,4,28,0,0,0,D::create_srv,2);
    const auto bindOrdinal=history.write_ordinal;
    auto last=history.snapshot(base,0);
    last.classify(bindOrdinal,bindOrdinal,bindOrdinal,true);
    CHECK(last.writes==2&&last.ordinal==2&&last.writer==D::create_srv&&last.origin==D::create_srv);
    CHECK(last.desc==2&&last.timing==D::before_bind);
    history.write(1000,7,D::srv,0,0,0,0,D::unsupported_srv,D::create_srv,1);
    last=history.snapshot(base,0);last.classify(bindOrdinal,bindOrdinal,history.write_ordinal,true);
    CHECK(last.writes==3&&last.timing==D::after_bind&&last.desc==1);
    CHECK(last.dimension==0&&last.format==0&&(last.flags&D::unknown_resource));
    last.classify(bindOrdinal,bindOrdinal,bindOrdinal,true);CHECK(last.timing==D::during_draw);
    last.classify(2,3,3,true);CHECK(last.timing==D::unordered); // setter overlapped write
    last.classify(2,2,3,false);CHECK(last.timing==D::unordered);
    last.classify(UINT32_MAX,UINT32_MAX,3,true);CHECK(last.timing==D::unordered);
    last.classify(2,2,UINT32_MAX,true);CHECK(last.timing==D::unordered);
    last.flags|=D::invalidated;last.classify(2,2,3,true);CHECK(last.timing==D::unordered);
    auto value=history.slots.at({base.serial,0});value.writes=65535;
    history.store({1,base.serial,1,0,0},value);
    CHECK(history.snapshot(base,1).writes==1); // copied source count never becomes destination count
    history.slots.at({base.serial,0}).writes=65535;
    history.write(1000,7,D::srv);CHECK(history.snapshot(base,0).writes==65535);
    history.write_ordinal=UINT32_MAX;history.write(1000,7,D::srv);
    last=history.snapshot(base,0);last.classify(2,2,3,true);
    CHECK(last.ordinal==0&&(last.flags&D::capacity)&&last.timing==D::unordered);
    for(unsigned i=0;i<4;++i){history.creation(i,1);history.creation(i,3);}
    history.creation(0,0);history.creation(9,0);
    CHECK(history.creations[0]==3&&history.creations[1]==2&&history.textures[3]==1&&history.missing_desc==1);
    history.creations[0]=history.textures[0]=history.missing_desc=65535;
    history.creation(0,3);history.creation(0,0);
    CHECK(history.creations[0]==65535&&history.textures[0]==65535&&history.missing_desc==65535);

    C clock;clock.reset(2,3);RootDescriptorTableWitness w;w.bind(4,5,0,0,true);
    auto call=clock.begin(2,3,6);clock.returned(call);
    w.write(call,clock,0,2000,location,2,2);clock.end(call);
    CHECK(w.descriptor_before==2&&w.descriptor_after==2);
    auto draw=clock.begin(2,3,6);clock.returned(draw);
    CHECK(w.covered(clock,2,3,4,5,6,true));
    CHECK(!w.covered(clock,2,3,4,5,6,false)); // partial hook/history coverage
    CHECK(!w.covered(clock,2,4,4,5,6,true));
    CHECK(!w.covered(clock,2,3,4,5,7,true));
    CHECK(!w.covered(clock,2,3,4,8,6,true));
    w.written=false;CHECK(!w.covered(clock,2,3,4,5,6,true));w.written=true;
    clock.fail(C::bundle);CHECK(!w.covered(clock,2,3,4,5,6,true));
    clock.poison=C::indirect;CHECK(!w.covered(clock,2,3,4,5,6,true));
    clock.poison=C::covered;clock.end(draw);clock.reset(2,4);
    CHECK(!w.covered(clock,2,4,4,5,6,true));
    w.bind(4,8,0,0,true);CHECK(!w.written&&!w.descriptor_before&&!w.descriptor_after);
    DescriptorTableCounters counts;
    counts.record(0x3fff);counts.record(D::copied);
    CHECK(counts.sticky==0x3fff&&counts.reasons[6]==2&&counts.reasons[0]==1);
    counts.reasons[0]=65535;counts.record(D::no_table);CHECK(counts.reasons[0]==65535);
    char text[448]{};s.flags=0x3fff;s.parameter=s.offset=s.slot=s.heap_class=UINT32_MAX;
    s.heap=UINT64_MAX;s.type=s.dimension=s.format=UINT32_MAX;
    s.append(text,sizeof text);counts.append(text,sizeof text);
    CHECK(std::strlen(text)<220); // companion + maximum Draw prefix stays <437
    CHECK(std::strstr(text,";tf=3fff;tp=ffffffff;to=ffffffff;th=ffffffffffffffff")!=nullptr);
}
