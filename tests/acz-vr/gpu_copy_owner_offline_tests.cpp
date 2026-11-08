// SYNTHETIC ONLY (GPU-copy step 3b part 1). Fake creation witnesses, fake
// fence/queue backend, fake recorder and fake READBACK map. No device, queue,
// fence, CopyBufferRegion, Signal, Map, game or XR exists in this test.
#include "gpu_copy_owner.h"
#include "gpu_copy_ledger_fixture.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

static std::atomic<unsigned> allocations{0};
void* operator new(size_t n) {++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new(size_t n,const std::nothrow_t&) noexcept {++allocations;return std::malloc(n?n:1);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}

using namespace acz::gpu_copy;
using namespace acz::gpu_copy::owner;
using F=FixtureAccess;
namespace acz::gpu_copy::owner {
struct OwnerFixtureAccess {
    static void enable(Registry& r) {std::lock_guard<std::mutex> l(r.mutex_);r.enabled_=true;}
    static void enable(Timeline& t) {std::lock_guard<std::mutex> l(t.mutex_);t.enabled_=true;}
    static bool arm(Timeline& t,const Key& k) {return t.arm(k);}
    static bool saturated(const Registry& r) {std::lock_guard<std::mutex> l(r.mutex_);return r.saturated_;}
};
}
using O=OwnerFixtureAccess;

// Storage statements (bytes are checked by the compiler, not estimated).
static_assert(sizeof(Eligibility)<=24,"Eligibility is a small Draw-path local");
static_assert(sizeof(CopyRecipe)<=40,"Recipe is a small Draw-path local");
static_assert(sizeof(BatchSummary)<=96,"Snapshot copy is small");
static_assert(sizeof(PollResult)<=16,"Poll result is small");
static_assert(sizeof(RestrictionGate)<=128,"Per-list gate, heap-resident with list state");
static_assert(sizeof(Timeline)<=1024,"Timeline is heap/static owner storage, never a frame");
static_assert(sizeof(Registry)<=64*1024,"Registry is heap/static owner storage, never a frame (part 2: 1024 records)");

static bool same_identity(const Identity& a,const Identity& b) {
    return a.device==b.device && a.queue==b.queue && a.fence==b.fence && a.list==b.list &&
        a.allocator==b.allocator && a.creation==b.creation && a.node==b.node && a.direct==b.direct;
}
constexpr Route D=0x100,D2=0x150,Q=0x200,Q2=0x250,A=0x300,A2=0x350,L=0x400,L2=0x450,L3=0x480,FENCE=0x900;

struct FakeFence final:FenceBackend {
    unsigned creates=0,signals=0,polls=0;
    bool create_ok=true,signal_ok=true;Route create_out=FENCE;
    Route signal_queue=0,signal_fence=0;uint64_t signal_value=0;
    uint64_t value=0;
    void (*on_signal)(void*)=nullptr;void* ctx=nullptr;
    bool create(Route device,Route& fence) noexcept override {CHECK(device==D);++creates;fence=create_out;return create_ok;}
    bool signal(Route q,Route f,uint64_t v) noexcept override {
        ++signals;signal_queue=q;signal_fence=f;signal_value=v;
        if(on_signal)on_signal(ctx);
        return signal_ok;
    }
    uint64_t completed(Route f) noexcept override {CHECK(f==FENCE);++polls;return value;}
    unsigned native() const {return creates+signals+polls;}
};

struct Rig {
    std::unique_ptr<Registry> registry{new Registry};
    std::unique_ptr<Timeline> timeline{new Timeline};
    FakeFence fence;
    Identity id{};
    Key key{};
    explicit Rig(bool arm=true,bool create=true) {
        O::enable(*registry);O::enable(*timeline);
        registry->device_created(true,D);
        registry->queue_created(true,Q,D,0,true);
        registry->allocator_created(true,A,D,true);
        registry->list_created(true,L,D,A,1,true);
        registry->list_created(true,L2,D,A,0,true);
        if(!create)return;
        CHECK(timeline->create(*registry,fence,Q));
        CHECK(registry->lookup_reset(L,A,id));
        key=Key{id.device,id.queue,id.fence,id.list,1};
        if(arm)CHECK(O::arm(*timeline,key));
    }
    BatchInput batch(const Route* lists,unsigned count,uint64_t current=1,uint64_t closed=1,Route queue=Q) const {
        return BatchInput{queue,count,lists,current,closed};
    }
};

static void off_cases() {
    // Default OFF: nothing recorded, nothing authenticated, no native call.
    Registry r;Timeline t;FakeFence f;
    r.device_created(true,D);r.queue_created(true,Q,D,0,true);
    r.allocator_created(true,A,D,true);r.list_created(true,L,D,A,0,true);
    Identity id{};CHECK(!r.lookup_reset(L,A,id));CHECK(!id.list);
    CHECK(!r.live_token(L,Kind::list) && !r.bind_timeline(Q,FENCE));
    CHECK(!t.create(r,f,Q));
    const Route lists[]={L};CHECK(!t.begin({Q,1,lists,1,1}));CHECK(!t.signal(1));
    CHECK(!t.poll().sampled && !t.complete(1) && !t.sample().valid);
    CHECK(f.native()==0);
    auto p=r.provider();Identity out{};CHECK(p.reset && !p.reset(p.context,&r,&r,out));
    // The 3a production provider stays all-null; this module does not change it.
    OwnershipProvider production{};CHECK(!production.reset && !production.execute);
    // An all-OFF ObservationFeed with this provider never constructs a Witness.
    ObservationFeed feed(p);feed.reset_result(reinterpret_cast<const void*>(L),reinterpret_cast<const void*>(A),true);
    CHECK(!feed.constructed() && feed.ownership_unavailable()==1);
}

static void registry_cases() {
    {Rig rig(false,false);auto& r=*rig.registry;
     const auto dt=r.live_token(D,Kind::device),qt=r.live_token(Q,Kind::queue);
     const auto at=r.live_token(A,Kind::allocator),lt=r.live_token(L,Kind::list);
     CHECK(dt && qt && at && lt);
     CHECK(dt!=D && qt!=Q && at!=A && lt!=L); // minted, never an address
     CHECK(dt!=qt && qt!=at && at!=lt);
     CHECK(!r.live_token(L,Kind::allocator)); // kind is part of the fact
     // Repeated device creation (ASSUMPTION: same live object) keeps its token.
     r.device_created(true,D);CHECK(r.live_token(D,Kind::device)==dt);
     // Repeated list creation without a witnessed Release: missed Release.
     r.list_created(true,L,D,A,0,true);CHECK(!r.live_token(L,Kind::list));CHECK(r.tombstones()==1);
     r.released(L);CHECK(!r.live_token(L,Kind::list)); // ambiguous stays ambiguous
     r.list_created(true,L,D,A,0,true);CHECK(!r.live_token(L,Kind::list));
     // Witnessed Release then reuse: a NEW token, the old one never resolves.
     const auto old2=r.live_token(L2,Kind::list);CHECK(old2);
     r.released(L2);CHECK(!r.live_token(L2,Kind::list));
     r.list_created(true,L2,D,A,0,true);const auto new2=r.live_token(L2,Kind::list);
     CHECK(new2 && new2!=old2);
     // D-171: failed creation with a non-null output tombstones the route.
     r.allocator_created(false,A2,D,true);CHECK(!r.live_token(A2,Kind::allocator));
     r.allocator_created(true,A2,D,true);CHECK(!r.live_token(A2,Kind::allocator));
     // Unwitnessed parents refuse: unknown device, unknown/mismatched allocator.
     r.queue_created(true,Q2,D2,0,true);CHECK(!r.live_token(Q2,Kind::queue));
     r.list_created(true,L3,D,0x777,0,true);CHECK(!r.live_token(L3,Kind::list));
     r.uncertain(A);CHECK(!r.live_token(A,Kind::allocator));
     CHECK(r.exhausted()==0 && !O::saturated(r));}
    {// Lookup refusals by fact.
     Rig rig(false);auto& r=*rig.registry;Identity id{};
     CHECK(r.lookup_reset(L,A,id) && id.direct && id.list && id.allocator && id.creation && id.node && id.fence);
     CHECK(id.list!=id.creation);
     Identity again{};CHECK(r.lookup_reset(L,A,again) && same_identity(again,id));
     Identity two{};CHECK(r.lookup_reset(L2,A,two) && two.list!=id.list); // mask 0 == 1
     Identity none{};
     CHECK(!r.lookup_reset(0x999,A,none) && !none.list);  // unwitnessed list
     CHECK(!r.lookup_reset(L,0x999,none));                // unwitnessed allocator
     r.allocator_created(true,A2,D,false);                // non-DIRECT allocator
     CHECK(!r.lookup_reset(L,A2,none));
     r.list_created(true,L3,D,A,2,true);                  // node mask 2 vs queue node 1
     CHECK(r.live_token(L3,Kind::list) && !r.lookup_reset(L3,A,none));
     r.list_created(true,0x4a0,D,A2,1,false);              // bundle/compute style list
     CHECK(r.live_token(0x4a0,Kind::list) && !r.lookup_reset(0x4a0,A2,none));
     Key out{};
     CHECK(r.lookup_execute(Q,rig.key,out) && std::memcmp(&out,&rig.key,sizeof out)==0);
     CHECK(!r.lookup_execute(Q2,rig.key,out));              // unwitnessed queue
     r.device_created(true,D);r.queue_created(true,Q2,D,0,true);
     CHECK(r.live_token(Q2,Kind::queue) && !r.lookup_execute(Q2,rig.key,out)); // not the timeline queue
     Key wrong=rig.key;wrong.fence+=1000;CHECK(!r.lookup_execute(Q,wrong,out));
     wrong=rig.key;wrong.list=12345;CHECK(!r.lookup_execute(Q,wrong,out));
     wrong=rig.key;wrong.generation=0;CHECK(!r.lookup_execute(Q,wrong,out));
     // Provider thunks are the same pure lookups and make no native call.
     const unsigned before=rig.fence.native();auto p=r.provider();Identity viaThunk{};
     CHECK(p.reset(p.context,reinterpret_cast<const void*>(L),reinterpret_cast<const void*>(A),viaThunk));
     CHECK(same_identity(viaThunk,id));
     CHECK(p.execute(p.context,reinterpret_cast<const void*>(Q),rig.key,out));
     CHECK(rig.fence.native()==before);
     // Released timeline queue: timeline lost, every lookup refuses.
     r.released(Q);CHECK(r.timeline().lost);
     CHECK(!r.lookup_reset(L,A,none) && !r.lookup_execute(Q,rig.key,out));}
    {// Capacity saturation refuses everything afterwards.
     Rig rig(false);auto& r=*rig.registry;
     for(Route n=0;n<Registry::capacity+4;++n)r.allocator_created(true,0x10000+n*16,D,true);
     CHECK(r.exhausted()>0 && O::saturated(r));
     Identity none{};CHECK(!r.lookup_reset(L,A,none) && !r.live_token(L,Kind::list));}
    {// Binding: one attempt per registry, fence on the same device.
     Rig rig(false,false);auto& r=*rig.registry;
     r.fence_created(true,FENCE,D);CHECK(r.bind_timeline(Q,FENCE));CHECK(!r.bind_timeline(Q,FENCE));
     Rig other(false,false);other.registry->device_created(true,D2);other.registry->fence_created(true,FENCE,D2);
     CHECK(!other.registry->bind_timeline(Q,FENCE) && other.registry->timeline().lost);}
}

static void feed_cases() {
    // Real 3a ObservationFeed with this provider: tokens reach the Witness only
    // from witnessed records. Fixture-attached ledger work is not exercised here.
    Rig rig(false);
    ObservationFeed feed(rig.registry->provider());
    feed.reset_result(reinterpret_cast<const void*>(0x999),reinterpret_cast<const void*>(A),true);
    CHECK(!feed.constructed() && feed.ownership_unavailable()==1);
    feed.reset_result(reinterpret_cast<const void*>(L),reinterpret_cast<const void*>(A),true);
    CHECK(feed.constructed());
    const auto snap=feed.snapshot();
    CHECK(snap.enabled && snap.generation==1 && !snap.ambiguous && snap.open);
    CHECK((snap.coverage&ownership_coverage)==ownership_coverage);
    CHECK(snap.coverage!=required_coverage); // hook bits are not supplied by ownership
    rig.registry->released(A);
    feed.reset_result(reinterpret_cast<const void*>(L),reinterpret_cast<const void*>(A),true);
    CHECK(feed.ownership_unavailable()==2 && feed.snapshot().coverage==0); // revoked
}

static void timeline_create_cases() {
    {Rig rig(false,false);rig.fence.create_ok=false;   // failed create, non-null output
     CHECK(!rig.timeline->create(*rig.registry,rig.fence,Q));
     CHECK(rig.timeline->lost() && rig.timeline->fence_route()==FENCE); // retained, not released
     CHECK(!rig.registry->live_token(FENCE,Kind::fence) && rig.registry->tombstones()==1);
     CHECK(!rig.timeline->create(*rig.registry,rig.fence,Q) && rig.fence.creates==1);} // no retry
    {Rig rig(false,false);CHECK(!rig.timeline->create(*rig.registry,rig.fence,Q2));CHECK(rig.fence.creates==0);}
    {Rig rig(false,false);rig.timeline->stop();CHECK(!rig.timeline->create(*rig.registry,rig.fence,Q));}
    {Rig rig(false);
     Key k=rig.key;k.fence+=1;CHECK(!O::arm(*rig.timeline,k));
     CHECK(O::arm(*rig.timeline,rig.key) && !O::arm(*rig.timeline,rig.key));}
}

static void positive_batch(Rig& rig,uint64_t& ticket) {
    // A repeated NON-candidate member (L2 twice) is not a destination writer.
    const Route lists[]={L2,L,L2};
    ticket=rig.timeline->begin(rig.batch(lists,3));
    CHECK(ticket==1);
    const auto b=rig.timeline->batch();
    CHECK(b.state==BatchState::registered && b.value==1 && b.count==3 && b.ordinal==1 && b.occurrences==1);
    CHECK(b.queue==rig.id.queue && b.list==rig.id.list && b.generation==1 && b.submission==1);
    CHECK(rig.timeline->member(1)==rig.id.list && rig.timeline->member(0)==rig.registry->live_token(L2,Kind::list));
    CHECK(rig.timeline->member(3)==0);
    CHECK(rig.fence.signals==0); // nothing native before forwarding
}

static void batch_cases() {
    {Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);
     CHECK(!rig.timeline->complete(ticket));
     CHECK(rig.timeline->signal(ticket));
     CHECK(rig.fence.signals==1 && rig.fence.signal_queue==Q && rig.fence.signal_fence==FENCE && rig.fence.signal_value==1);
     CHECK(!rig.timeline->signal(ticket) && rig.fence.signals==1); // exactly one Signal
     CHECK(!rig.timeline->complete(ticket));                        // Signal success != completion
     rig.fence.value=0;auto p=rig.timeline->poll();CHECK(p.sampled && !p.lost && !rig.timeline->complete(ticket));
     rig.fence.value=1;p=rig.timeline->poll();CHECK(p.value==1 && rig.timeline->complete(ticket));
     const unsigned polls=rig.fence.polls;CHECK(!rig.timeline->poll().sampled && rig.fence.polls==polls);
     // A later execution of the SAME recording is a second destination writer.
     const Route again[]={L};CHECK(!rig.timeline->begin(rig.batch(again,1)));
     CHECK(rig.timeline->quarantined() && rig.timeline->batch().re_executed && !rig.timeline->complete(ticket));
     CHECK(rig.timeline->member(1)==rig.id.list);} // first record preserved
    {// After a successful Reset (generation advanced) the old recording cannot run.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);CHECK(rig.timeline->signal(ticket));
     const Route again[]={L};CHECK(!rig.timeline->begin(rig.batch(again,1,2,1)));
     CHECK(!rig.timeline->quarantined());}
    struct Case {const char* name;Route lists[3];unsigned count;uint64_t current,closed;Route queue;bool(*flag)(const BatchSummary&);};
    const Case cases[]={
        {"duplicate",{L,L,0},2,1,1,Q,[](const BatchSummary& b){return b.duplicate && b.occurrences==2;}},
        {"unknown",{L,0x999,0},2,1,1,Q,[](const BatchSummary& b){return b.unknown;}},
        {"unclosed",{L,0,0},1,1,0,Q,[](const BatchSummary& b){return b.unclosed;}},
        {"cross-queue",{L,0,0},1,1,1,Q2,[](const BatchSummary& b){return b.cross_queue;}},
        {"unauthenticated-queue",{L,0,0},1,1,1,0x777,[](const BatchSummary& b){return b.unauthenticated;}},
    };
    for(const auto& c:cases) {
        Rig rig;rig.registry->device_created(true,D);rig.registry->queue_created(true,Q2,D,0,true);
        CHECK(!rig.timeline->begin(rig.batch(c.lists,c.count,c.current,c.closed,c.queue)));
        const auto b=rig.timeline->batch();
        if(!(b.state==BatchState::quarantined && c.flag(b))){std::fprintf(stderr,"case %s\n",c.name);CHECK(false);}
        CHECK(!rig.timeline->signal(1) && rig.fence.signals==0);
        rig.fence.value=1;rig.timeline->poll();CHECK(!rig.timeline->complete(1));
    }
    {// Unknown member without the candidate: possible alias of the recording.
     Rig rig;const Route lists[]={L2,0x999};CHECK(!rig.timeline->begin(rig.batch(lists,2)));
     CHECK(rig.timeline->quarantined() && rig.timeline->batch().unknown);}
    {// Known batch without the candidate is irrelevant and not retained.
     Rig rig;const Route lists[]={L2};CHECK(!rig.timeline->begin(rig.batch(lists,1,1,1,Q2)));
     CHECK(!rig.timeline->quarantined() && rig.timeline->batch().state==BatchState::none);}
    {// Oversized or malformed scans are incomplete: quarantine.
     Rig rig;CHECK(!rig.timeline->begin({Q,Timeline::batch_cap+1,nullptr,1,1}));CHECK(rig.timeline->batch().oversize);
     Rig rig2;CHECK(!rig2.timeline->begin({Q,2,nullptr,1,1}));CHECK(rig2.timeline->quarantined());}
    {// Concurrent/reentrant Execute between registration and Signal.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);
     const Route other[]={L2};CHECK(!rig.timeline->begin(rig.batch(other,1)));
     CHECK(rig.timeline->batch().reentrant && rig.timeline->quarantined());
     CHECK(!rig.timeline->signal(ticket) && rig.fence.signals==0);}
    {// Reentry from inside the native Signal itself.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);
     rig.fence.ctx=&rig;rig.fence.on_signal=[](void* c) {
         auto& r=*static_cast<Rig*>(c);const Route other[]={L2};
         CHECK(!r.timeline->begin(r.batch(other,1)));
     };
     CHECK(!rig.timeline->signal(ticket) && rig.fence.signals==1);
     CHECK(rig.timeline->batch().reentrant && rig.timeline->quarantined());
     rig.fence.value=1;rig.timeline->poll();CHECK(!rig.timeline->complete(ticket));}
    {// Failed Signal is ambiguous: retained forever, never complete.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);rig.fence.signal_ok=false;
     CHECK(!rig.timeline->signal(ticket) && rig.timeline->batch().state==BatchState::signal_failed);
     CHECK(rig.timeline->quarantined());rig.fence.value=1;rig.timeline->poll();CHECK(!rig.timeline->complete(ticket));}
    {// Wrong ticket while in flight quarantines.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);
     CHECK(!rig.timeline->signal(ticket+1) && rig.timeline->quarantined());}
    {// Stop between registration and Signal still issues the Signal.
     Rig rig;uint64_t ticket=0;positive_batch(rig,ticket);rig.timeline->stop();
     CHECK(rig.timeline->signal(ticket) && rig.fence.signals==1);
     rig.fence.value=1;rig.timeline->poll();CHECK(rig.timeline->complete(ticket));}
    {// Timeline queue retired before Execute: unauthenticated.
     Rig rig;rig.registry->released(Q);const Route lists[]={L};
     CHECK(!rig.timeline->begin(rig.batch(lists,1)) && rig.timeline->batch().unauthenticated);}
}

static void fence_cases() {
    auto signalled=[](Rig& rig) {uint64_t t=0;positive_batch(rig,t);CHECK(rig.timeline->signal(t));return t;};
    {Rig rig;const auto t=signalled(rig);rig.fence.value=(std::numeric_limits<uint64_t>::max)();
     const auto p=rig.timeline->poll();CHECK(p.lost && !rig.timeline->complete(t)); // >= but loss
     CHECK(rig.registry->timeline().lost && !rig.timeline->sample().valid && rig.timeline->quarantined());
     rig.fence.value=1;CHECK(!rig.timeline->poll().sampled && !rig.timeline->complete(t));}
    {Rig rig;rig.fence.value=1;CHECK(rig.timeline->poll().lost);} // nonzero before any Signal
    {Rig rig;const auto t=signalled(rig);rig.fence.value=2;CHECK(rig.timeline->poll().lost && !rig.timeline->complete(t));} // other signaller
    {Rig rig;const auto t=signalled(rig);rig.fence.value=0;rig.timeline->poll();
     rig.fence.value=1;rig.timeline->poll();CHECK(rig.timeline->complete(t));}
    {Rig rig;signalled(rig);rig.fence.value=0;
     for(unsigned n=0;n<Timeline::poll_cap;++n)CHECK(rig.timeline->poll().sampled);
     const auto p=rig.timeline->poll();CHECK(p.timed_out && rig.timeline->quarantined());
     rig.fence.value=1;CHECK(!rig.timeline->poll().sampled && !rig.timeline->complete(1));
     CHECK(rig.fence.polls==Timeline::poll_cap);}
    {Rig rig;const auto t=signalled(rig);rig.registry->released(FENCE);
     CHECK(rig.timeline->poll().lost && !rig.timeline->complete(t));}
}

static void gate_cases() {
    {RestrictionGate g;CHECK(!g.check().ok);g.reset(true);const auto e=g.check();CHECK(e.ok && g.still_valid(e));}
    {// Reentrant Draw inside native SetPredication: rejected while in flight.
     RestrictionGate g;g.reset(true);const auto before=g.check();CHECK(before.ok);
     g.begin(Restriction::predication);
     CHECK(!g.check().ok && !g.still_valid(before)); // intent + window, before native dispatch
     g.end(Restriction::predication);
     CHECK(!g.check().ok);                           // intent excludes the generation
     g.begin(Restriction::native_other);g.end(Restriction::native_other);g.reset(true);
     CHECK(g.check().ok);}
    {// Any native list call between check and recheck invalidates the epoch.
     RestrictionGate g;g.reset(true);const auto e=g.check();
     g.begin(Restriction::native_other);CHECK(!g.check().ok);g.end(Restriction::native_other);
     CHECK(g.check().ok && !g.still_valid(e));}
    for(auto r:{Restriction::begin_query,Restriction::end_query,Restriction::resolve_query,
               Restriction::bundle,Restriction::indirect}) {
        RestrictionGate g;g.reset(true);g.begin(r);g.end(r);CHECK(!g.check().ok);g.reset(true);CHECK(g.check().ok);
    }
    for(auto r:{Restriction::begin_pass,Restriction::end_pass,Restriction::protected_session}) {
        RestrictionGate g;g.reset(true);g.begin(r);g.end(r);g.reset(true);CHECK(!g.check().ok); // lifetime
    }
    {RestrictionGate g;g.reset(true);g.end(Restriction::native_other);g.reset(true);CHECK(!g.check().ok);} // unmatched
    {RestrictionGate g;g.reset(true);g.begin(Restriction::native_other);g.reset(true);
     g.end(Restriction::native_other);g.reset(true);CHECK(!g.check().ok);} // Reset with a call in flight
    {RestrictionGate g;g.reset(false);g.reset(true);CHECK(!g.check().ok);}
    {RestrictionGate g;g.reset(true);g.uncertain();g.reset(true);CHECK(!g.check().ok);}
}

struct FakeRecorder final:CopyRecorder {
    unsigned calls=0;bool result=true;Session* session=nullptr;CopyRecipe last{};
    bool record(Route list,const CopyRecipe& r) noexcept override {
        CHECK(list==L);++calls;last=r;
        if(session)CHECK(session->snapshot().state==State::recorded_ambiguous); // marked first
        return result;
    }
};
struct FakeMap final:ReadbackMap {
    unsigned maps=0,unmaps=0;bool ok=true;uint8_t bytes[64]{};
    bool map(uint64_t dest,uint64_t offset,uint64_t n,const uint8_t*& data) noexcept override {
        CHECK(dest==77 && offset==0 && n<=sizeof bytes);++maps;data=ok?bytes:nullptr;return ok;
    }
    void unmap(uint64_t dest) noexcept override {CHECK(dest==77);++unmaps;}
};
struct LedgerBackend final:Backend {
    Timeline* timeline=nullptr;unsigned reads=0,drops=0,releases=0;
    FenceSample poll() noexcept override {return timeline->sample();}
    bool read() noexcept override {++reads;return true;}
    void drop_logical_source_lease(uint64_t) noexcept override {++drops;}
    void release_private(uint64_t) noexcept override {++releases;}
};
static bool mark_possible(void* s) noexcept {return F::record(*static_cast<Session*>(s));}
static bool mark_counted(void* c) noexcept {++*static_cast<unsigned*>(c);return true;}

static void interface_cases() {
    const CopyRecipe good{5,0x1000,77,0,16};
    CHECK(recipe_valid(good,4096*2,4096));
    CHECK(!recipe_valid({5,0,77,0,0},4096,4096));          // zero length
    CHECK(!recipe_valid({5,0,77,0,4097},8192,8192));       // over cap
    CHECK(!recipe_valid({5,4090,77,0,16},4096,4096));      // source bound
    CHECK(!recipe_valid({5,(std::numeric_limits<uint64_t>::max)(),77,0,16},4096,4096)); // no overflow
    CHECK(!recipe_valid({5,0,77,4090,16},4096,4096));      // destination bound
    CHECK(!recipe_valid({5,0,5,0,16},4096,4096) && !recipe_valid({0,0,77,0,16},4096,4096));
    {RestrictionGate g;g.reset(true);FakeRecorder rec;unsigned marks=0;PossibleRecording mark{&marks,&mark_counted};
     auto e=g.check();
     CHECK(insert_before_draw(g,e,false,mark,rec,L,good,8192,4096)==Insert::refused);   // witness refuses
     CHECK(insert_before_draw(g,e,true,{},rec,L,good,8192,4096)==Insert::refused);      // no ledger mark
     CHECK(insert_before_draw(g,e,true,mark,rec,0,good,8192,4096)==Insert::refused);
     CHECK(insert_before_draw(g,e,true,mark,rec,L,{5,0,77,0,0},8192,4096)==Insert::refused);
     g.begin(Restriction::predication); // in-flight window
     CHECK(insert_before_draw(g,e,true,mark,rec,L,good,8192,4096)==Insert::refused);
     g.end(Restriction::predication);
     CHECK(insert_before_draw(g,g.check(),true,mark,rec,L,good,8192,4096)==Insert::refused); // intent
     CHECK(rec.calls==0 && marks==0);
     g.reset(true);e=g.check();rec.result=false;
     CHECK(insert_before_draw(g,e,true,mark,rec,L,good,8192,4096)==Insert::possibly_recorded);
     CHECK(rec.calls==1 && marks==1 && rec.last.bytes==16);} // false return still possibly recorded
}

static void ledger_composition_cases(bool loss) {
    // End-to-end synthetic join with the EXISTING ledger, fixture-armed. This
    // shows the tokens/values compose; it is not native evidence of anything.
    Rig rig(false);RestrictionGate gate;gate.reset(true);
    std::unique_ptr<Session> session(new Session);LedgerBackend backend;backend.timeline=rig.timeline.get();
    Admission a=fixture_admission();a.key=rig.key;
    CHECK(F::arm(*session,true,backend,a) && F::reserve(*session,a) && F::created(*session));
    FakeRecorder rec;rec.session=session.get();
    const CopyRecipe recipe{5,0,77,0,16};
    CHECK(insert_before_draw(gate,gate.check(),true,{session.get(),&mark_possible},rec,L,recipe,4096,4096)
          ==Insert::possibly_recorded);
    CHECK(O::arm(*rig.timeline,rig.key));
    const Route lists[]={L};
    const auto ticket=rig.timeline->begin(rig.batch(lists,1));
    CHECK(ticket && F::execute(*session,rig.key,rig.timeline->batch().occurrences)==ticket);
    // (fake native ExecuteCommandLists forwarded exactly once here)
    CHECK(rig.timeline->signal(ticket) && F::signal(*session,ticket,true));
    CHECK(F::close(*session,rig.key)); // injected: stands for observed successful Reset
    FakeMap map;uint8_t out[16]{};
    for(unsigned i=0;i<16;++i)map.bytes[i]=static_cast<uint8_t>(0xa0+i);
    CHECK(!read_back(*rig.timeline,ticket,true,map,77,0,16,out,sizeof out) && map.maps==0); // not complete
    rig.fence.value=loss?(std::numeric_limits<uint64_t>::max)():1;
    rig.timeline->poll();
    if(loss) {
        CHECK(!session->poll() && session->snapshot().state==State::quarantined);
        CHECK(!read_back(*rig.timeline,ticket,true,map,77,0,16,out,sizeof out) && map.maps==0);
        CHECK(!session->read() && !session->retire() && backend.reads==0 && backend.releases==0);
        CHECK(session->snapshot().private_held && session->snapshot().source_lease);
        return;
    }
    CHECK(session->poll() && session->snapshot().gpu_complete);
    CHECK(!read_back(*rig.timeline,ticket,false,map,77,0,16,out,sizeof out) && map.maps==0); // no closure
    CHECK(!read_back(*rig.timeline,ticket,true,map,77,0,16,out,8) && map.maps==0);          // capacity
    CHECK(read_back(*rig.timeline,ticket,true,map,77,0,16,out,sizeof out));
    CHECK(map.maps==1 && map.unmaps==1 && out[0]==0xa0 && out[15]==0xaf);
    CHECK(session->read() && session->retire() && backend.drops==1 && backend.releases==1);
    map.ok=false;CHECK(!read_back(*rig.timeline,ticket,true,map,77,0,16,out,sizeof out) && map.unmaps==1);
}

static void allocation_cases() {
    // No heap allocation on the Draw-path or Execute-path entries.
    Rig rig;RestrictionGate gate;gate.reset(true);FakeRecorder rec;unsigned marks=0;
    FakeMap map;uint8_t out[16]{};const Route lists[]={L2,L};
    const unsigned before=allocations.load();
    const auto e=gate.check();CHECK(gate.still_valid(e));
    CHECK(insert_before_draw(gate,e,true,{&marks,&mark_counted},rec,L,{5,0,77,0,16},4096,4096)==Insert::possibly_recorded);
    gate.begin(Restriction::native_other);gate.end(Restriction::native_other);
    const auto t=rig.timeline->begin(rig.batch(lists,2));CHECK(t && rig.timeline->signal(t));
    rig.fence.value=1;rig.timeline->poll();CHECK(read_back(*rig.timeline,t,true,map,77,0,16,out,sizeof out));
    Identity id{};CHECK(rig.registry->lookup_reset(L,A,id));
    CHECK(allocations.load()==before);
}

static void concurrency_cases() {
    // Pure lookups race creation/release writers without deadlock or torn facts.
    Rig rig(false);std::atomic<bool> go{true};std::atomic<unsigned> ok{0};
    std::thread reader([&] {
        while(go.load() || !ok.load()){Identity id{};if(rig.registry->lookup_reset(L,A,id)){CHECK(id.list && id.allocator);++ok;}}
    });
    for(unsigned n=0;n<4000;++n) {
        const Route r=0x20000+(n%16)*16;
        rig.registry->allocator_created(true,r,D,true);rig.registry->released(r);
    }
    go=false;reader.join();
    CHECK(ok.load()>0 && !O::saturated(*rig.registry));
}

static void part2_cases() {
    // Production enable entries (part 2) behave like the fixture enable.
    {std::unique_ptr<Registry> r(new Registry);r->enable();r->device_created(true,D);
     CHECK(r->live_token(D,Kind::device) && r->live_records()==1);
     std::unique_ptr<Timeline> t(new Timeline);t->enable();FakeFence f;
     r->queue_created(true,Q,D,0,true);CHECK(t->create(*r,f,Q) && f.creates==1);}
    {// reset_refusal classifies each lookup_reset refusal without writing.
     Rig rig(false);auto& r=*rig.registry;
     CHECK(r.reset_refusal(L,A)==0);
     CHECK(r.reset_refusal(0x999,A)==16 && r.reset_refusal(L,0x999)==64);
     r.allocator_created(true,A2,D,false);CHECK(r.reset_refusal(L,A2)==512);
     r.list_created(true,L3,D,A,2,true);CHECK(r.reset_refusal(L3,A)==1024);
     r.uncertain(L2);CHECK(r.reset_refusal(L2,A)==32);
     Rig unbound(false,false);CHECK(unbound.registry->reset_refusal(L,A)==8);
     Registry off;CHECK(off.reset_refusal(L,A)&1);}
    {// Certified canonical routing: an unwitnessed member is a different object.
     Rig rig;const Route lists[]={L2,0x999};
     auto in=rig.batch(lists,2);in.aliases_certified=true;
     CHECK(!rig.timeline->begin(in) && !rig.timeline->quarantined());
     const Route with[]={0x999,L};auto in2=rig.batch(with,2);in2.aliases_certified=true;
     const auto t=rig.timeline->begin(in2);
     CHECK(t==1 && rig.timeline->batch().unknown && rig.timeline->batch().ordinal==1 && !rig.timeline->quarantined());
     CHECK(rig.timeline->signal(t));
     // Certification never excuses a duplicate or a re-execution.
     const Route again[]={L};auto in3=rig.batch(again,1);in3.aliases_certified=true;
     CHECK(!rig.timeline->begin(in3) && rig.timeline->batch().re_executed && rig.timeline->quarantined());}
    {Rig rig;const Route dup[]={L,L};auto in=rig.batch(dup,2);in.aliases_certified=true;
     CHECK(!rig.timeline->begin(in) && rig.timeline->batch().duplicate && rig.timeline->quarantined());}
}

// J-3b3: creation outcomes (diagnostic only) and the closed CreateCommandList1 witness.
static void creation_cases() {
    {Registry off;CHECK(off.list_created(true,L,D,A,0,true)==Created::off);
     CHECK(off.closed_list_created(true,L,D,0,true)==Created::off&&off.allocator_created(true,A,D,true)==Created::off);}
    Rig rig(false);auto& r=*rig.registry;Identity none{};
    CHECK(r.device_created(true,D)==Created::repeated);
    // Closed list: device is the only parent; first recording still needs a
    // witnessed allocator at Reset (lookup_reset), never one from creation.
    constexpr Route C=0x4c0,C2=0x4d0,C3=0x4e0,C4=0x4f0;
    CHECK(r.closed_list_created(true,C,D,0,true)==Created::recorded&&r.live_token(C,Kind::list));
    Identity closed{};CHECK(r.lookup_reset(C,A,closed)&&closed.list&&closed.allocator&&closed.direct);
    CHECK(r.reset_refusal(C,A)==0&&r.reset_refusal(C,0x999)==64);
    CHECK(r.closed_list_created(true,C2,D2,0,true)==Created::parent&&!r.live_token(C2,Kind::list)); // unwitnessed device
    CHECK(r.reset_refusal(C2,A)==32);
    CHECK(r.closed_list_created(false,C3,D,0,true)==Created::failed&&!r.live_token(C3,Kind::list)); // D-171
    CHECK(r.closed_list_created(true,C4,D,0,false)==Created::recorded&&!r.lookup_reset(C4,A,none)); // not DIRECT
    // Second creation at a live route without a witnessed Release: collision.
    CHECK(r.closed_list_created(true,C,D,0,true)==Created::collision&&!r.live_token(C,Kind::list));
    CHECK(r.list_created(true,C,D,A,0,true)==Created::collision); // ambiguous stays ambiguous
    // Witnessed Release then re-creation at the same address: fresh tokens.
    const auto before=r.live_token(L2,Kind::list);CHECK(before);r.released(L2);
    CHECK(r.closed_list_created(true,L2,D,0,true)==Created::recorded);
    const auto after=r.live_token(L2,Kind::list);CHECK(after&&after!=before);
    Identity reborn{};CHECK(r.lookup_reset(L2,A,reborn)&&reborn.list==after);
    // Open list on an unwitnessed (e.g. pre-hook) allocator: parent refusal.
    CHECK(r.list_created(true,0x5a0,D,0x999,0,true)==Created::parent&&r.reset_refusal(0x5a0,A)==32);
    CHECK(r.allocator_created(true,A2,D,true)==Created::recorded&&r.allocator_created(true,A2,D,true)==Created::collision);
}

int main() {
    off_cases();
    part2_cases();
    creation_cases();
    registry_cases();
    feed_cases();
    timeline_create_cases();
    batch_cases();
    fence_cases();
    gate_cases();
    interface_cases();
    ledger_composition_cases(false);
    ledger_composition_cases(true);
    allocation_cases();
    concurrency_cases();
    std::puts("gpu_copy_owner_offline: PASS (synthetic fakes only; no native evidence)");
    return 0;
}
