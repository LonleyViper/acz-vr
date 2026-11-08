// FPS growth root cause (docs/world-render/fps-investigation.md section 7): offline checks of the HookState::pipelines claims index.
//  1. Randomised differential: after every production-style edit (try_emplace + displaced / move + erase, both followed by
//     pipelineIndexEdit_locked, exactly as d3d12_hook.cpp does), pipelineFor_locked and registeredOwnershipKey_locked return EXACTLY what
//     the original whole-map scans return, the index's "claimed" sets equal a reference walk, and the index is maintained incrementally
//     (one build, no rebuild per edit). Shared aliases (two records claiming one address), null ancestors, re-insertion over an existing
//     key and erase of absent keys are included.
//  2. Staleness: a fixture that edits the map directly (size change) forces a rebuild; a same-size in-place edit is caught by the
//     test-build cross-check (g_pipelineIndexMismatch) and the scan's answer is returned. Production edits records only via the 3 sites.
//  3. Timing (informational, printed): one Execute's ~15 negative lookups (hookCaptureCommandList metadataOwner) at N = 500/2000/4000.
//  4. Cost line: the new rel/cert/pscan categories and the pso/lists gauges are reported in part 2 and NOT summed into hooks-us-per-frame;
//     the worst-case line still fits the shared text capacity.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
using namespace acz;
static unsigned checks=0;

// ---- reference: the original loops, verbatim in effect
static auto refFor(IUnknown* object) {
    auto& records=state().pipelines;
    if(!object)return records.end();
    auto direct=records.find(reinterpret_cast<ID3D12PipelineState*>(object));
    if(direct!=records.end())return direct;
    return std::find_if(records.begin(),records.end(),[&](const auto& entry) {
        return entry.second.canonical_address==object ||
            std::find(entry.second.ancestor_addresses.begin(),entry.second.ancestor_addresses.end(),object)!=entry.second.ancestor_addresses.end();
    });
}
static bool refAlias(uintptr_t a) {
    for(const auto& e:state().pipelines) {
        if(reinterpret_cast<uintptr_t>(e.second.canonical_address)==a)return true;
        for(auto* p:e.second.ancestor_addresses)if(reinterpret_cast<uintptr_t>(p)==a)return true;
    }
    return false;
}
static bool refRetirement(uint64_t k) {
    for(const auto& e:state().pipelines) {
        if(reinterpret_cast<uintptr_t>(e.first)==k)return true;
        for(auto r:e.second.retirementKeys)if(r==k)return true;
    }
    return false;
}
// ---- production-shaped edits (d3d12_hook.cpp: PSO creation try_emplace, invalidate()/final-Release erase)
static void prodInsert(uintptr_t key,PipelineRecord record) {
    auto& s=state();PipelineRecord displaced;
    auto inserted=s.pipelines.try_emplace(reinterpret_cast<ID3D12PipelineState*>(key));
    displaced=std::move(inserted.first->second);
    inserted.first->second=std::move(record);
    pipelineIndexEdit_locked(key,inserted.second?nullptr:&displaced,&inserted.first->second);
}
static void prodErase(uintptr_t key) {
    auto& s=state();PipelineRecord old;
    auto found=s.pipelines.find(reinterpret_cast<ID3D12PipelineState*>(key));
    if(found!=s.pipelines.end()){old=std::move(found->second);s.pipelines.erase(found);pipelineIndexEdit_locked(key,&old,nullptr);}
}
static void resetState() {
    auto& s=state();s.pipelines.clear();s.pipelineIndex.clear();++s.pipelineEpoch;s.pipelineIndex.rebuilds=0;g_pipelineIndexMismatch=0;
}

static void differential(unsigned seed,unsigned n) {
    std::mt19937_64 rng(seed);auto rnd=[&](uint64_t lo,uint64_t hi){return std::uniform_int_distribution<uint64_t>(lo,hi)(rng);};
    auto& s=state();std::lock_guard lock(s.mutex);resetState();
    // Small address space so aliases are shared between records and collide with map keys / retirement keys.
    const uint64_t space=n*6;auto addr=[&]{return uintptr_t(0x10000+8*rnd(1,space));};
    std::vector<uintptr_t> keys;
    unsigned uniqueClaims=0,sharedClaims=0;
    auto make=[&] {
        PipelineRecord r;
        if(rnd(0,3))r.canonical_address=reinterpret_cast<IUnknown*>(addr());
        for(auto& a:r.ancestor_addresses)if(rnd(0,2)==0)a=reinterpret_cast<IUnknown*>(addr());   // many null slots
        if(rnd(0,4)==0)r.ancestor_addresses[1]=r.ancestor_addresses[0];                            // duplicate inside one record
        for(auto& k:r.retirementKeys)if(rnd(0,3)==0)k=addr();
        return r;
    };
    auto query=[&](uintptr_t a) {
        auto* o=reinterpret_cast<IUnknown*>(a);
        CHECK(pipelineFor_locked(o)==refFor(o));
        CHECK(registeredOwnershipKey_locked(a)==(a&&refRetirement(a)));   // 0 is never a key (zero retirement slots are unused)
        if(const auto* x=pipelineIndex_locked()) {
            if(a){CHECK((x->aliases.count(a)!=0)==refAlias(a));CHECK((x->retirement.count(a)!=0)==refRetirement(a));}
            // Section 8: count = number of claiming RECORDS, keys = XOR of their map keys (so count == 1 names the claimant).
            if(a) {
                unsigned na=0,nr=0;uintptr_t xa=0,xr=0;
                for(const auto& e:s.pipelines) {
                    const auto k=reinterpret_cast<uintptr_t>(e.first);
                    bool alias=reinterpret_cast<uintptr_t>(e.second.canonical_address)==a,ret=k==a;
                    for(auto* p:e.second.ancestor_addresses)alias|=reinterpret_cast<uintptr_t>(p)==a;
                    for(auto r:e.second.retirementKeys)ret|=r==a;
                    if(alias){++na;xa^=k;}if(ret){++nr;xr^=k;}
                }
                const auto ia=x->aliases.find(a),ir=x->retirement.find(a);
                CHECK(na?ia!=x->aliases.end()&&ia->second.count==na&&ia->second.keys==xa:ia==x->aliases.end());
                CHECK(nr?ir!=x->retirement.end()&&ir->second.count==nr&&ir->second.keys==xr:ir==x->retirement.end());
                if(na==1)++uniqueClaims;else if(na>1)++sharedClaims;
            }
        } else CHECK(false);
    };
    for(unsigned i=0;i<n;++i){const auto k=addr();prodInsert(k,make());keys.push_back(k);}
    query(0);
    for(unsigned q=0;q<6000;++q) {
        switch(rnd(0,9)) {
            case 0: {const auto k=addr();prodInsert(k,make());keys.push_back(k);break;}                       // insert (maybe over a key)
            case 1: if(!keys.empty()){prodInsert(keys[rnd(0,keys.size()-1)],make());}break;                      // replace an existing key
            case 2: if(!keys.empty()){prodErase(keys[rnd(0,keys.size()-1)]);}break;                              // erase (maybe absent)
            default: break;
        }
        uintptr_t a=addr();
        if(rnd(0,2)==0&&!s.pipelines.empty()) { // an address some record claims
            auto it=s.pipelines.begin();std::advance(it,rnd(0,s.pipelines.size()-1));
            const auto& r=it->second;
            switch(rnd(0,3)){case 0:a=reinterpret_cast<uintptr_t>(r.canonical_address);break;
                case 1:a=reinterpret_cast<uintptr_t>(r.ancestor_addresses[rnd(0,5)]);break;
                case 2:a=uintptr_t(r.retirementKeys[rnd(0,19)]);break;default:a=reinterpret_cast<uintptr_t>(it->first);break;}
        }
        query(a);
    }
    CHECK(g_pipelineIndexMismatch==0);
    CHECK(s.pipelineIndex.rebuilds==1);   // built once at the first lookup, maintained incrementally by every production edit
    CHECK(uniqueClaims>100&&sharedClaims>0);   // both the O(1) single-claimant path and the shared-address scan were exercised
    std::printf("differential seed=%u n=%u: %zu records, rebuilds=%llu, mismatches=%llu, single/shared claims %u/%u\n",seed,n,
        s.pipelines.size(),static_cast<unsigned long long>(s.pipelineIndex.rebuilds),static_cast<unsigned long long>(g_pipelineIndexMismatch),
        uniqueClaims,sharedClaims);
}

static void staleness() {
    auto& s=state();std::lock_guard lock(s.mutex);resetState();
    PipelineRecord r;r.canonical_address=reinterpret_cast<IUnknown*>(uintptr_t(0x5000));
    prodInsert(0x4000,r);
    auto* list=reinterpret_cast<IUnknown*>(uintptr_t(0x9000));
    CHECK(pipelineFor_locked(list)==s.pipelines.end()&&s.pipelineIndex.rebuilds==1);
    // Fixture edits the map directly (size change): the next lookup rebuilds and sees the new alias.
    PipelineRecord f;f.ancestor_addresses[2]=list;s.pipelines[reinterpret_cast<ID3D12PipelineState*>(uintptr_t(0x4100))]=f;
    CHECK(pipelineFor_locked(list)==refFor(list)&&pipelineFor_locked(list)!=s.pipelines.end()&&s.pipelineIndex.rebuilds==2);
    CHECK(g_pipelineIndexMismatch==0);
    // A production edit right after a direct (unsynchronised) erase sees the size mismatch and drops the index instead of patching it.
    s.pipelines.erase(reinterpret_cast<ID3D12PipelineState*>(uintptr_t(0x4100)));
    prodInsert(0x4200,r);CHECK(!s.pipelineIndex.valid);
    CHECK(pipelineFor_locked(list)==s.pipelines.end()&&s.pipelineIndex.rebuilds==3&&g_pipelineIndexMismatch==0);
    // Same-size in-place edit (never done by production code): the test-build cross-check returns the scan's answer and counts it.
    s.pipelines.at(reinterpret_cast<ID3D12PipelineState*>(uintptr_t(0x4000))).canonical_address=list;
    CHECK(pipelineFor_locked(list)==refFor(list)&&pipelineFor_locked(list)!=s.pipelines.end()&&g_pipelineIndexMismatch==2);
    // The shutdown drain swaps the map out and clears the index (d3d12_hook.cpp shutdownDiagnosticOwnership).
    decltype(s.pipelines) drained;s.pipelines.swap(drained);s.pipelineIndex.clear();++s.pipelineEpoch;
    CHECK(pipelineFor_locked(list)==s.pipelines.end()&&!registeredOwnershipKey_locked(0x4000));
    resetState();
}

static void timing() {
    auto& s=state();std::lock_guard lock(s.mutex);
    for(unsigned n:{500u,2000u,4000u}) {
        resetState();
        for(unsigned i=0;i<n;++i) {
            PipelineRecord r;const uintptr_t base=0x100000+uintptr_t(i)*0x1000;
            r.canonical_address=reinterpret_cast<IUnknown*>(base+8);
            for(unsigned j=0;j<6;++j)r.ancestor_addresses[j]=reinterpret_cast<IUnknown*>(base+16+8*j);
            for(unsigned j=0;j<20;++j)r.retirementKeys[j]=base+8*j;
            prodInsert(base,std::move(r));
        }
        const auto* x=pipelineIndex_locked();CHECK(x);
        constexpr unsigned executes=2000,lookups=15;   // one Execute: canonical + up to 11 variants + 3 bases (metadataOwner)
        uint64_t sink=0;
        auto t0=std::chrono::steady_clock::now();
        for(unsigned e=0;e<executes;++e)for(unsigned l=0;l<lookups;++l)
            sink+=pipelineScan_locked(reinterpret_cast<IUnknown*>(uintptr_t(0x7000000+64*l+e)))==s.pipelines.end();
        auto t1=std::chrono::steady_clock::now();
        for(unsigned e=0;e<executes;++e)for(unsigned l=0;l<lookups;++l) {
            const auto a=uintptr_t(0x7000000+64*l+e);
            sink+=s.pipelines.find(reinterpret_cast<ID3D12PipelineState*>(a))==s.pipelines.end()&&!x->aliases.count(a);
        }
        auto t2=std::chrono::steady_clock::now();
        auto us=[&](auto a,auto b){return std::chrono::duration<double,std::micro>(b-a).count()/executes;};
        std::printf("timing n=%u PSO records: one Execute's %u negative pipelineFor_locked lookups: scan %.1f us, index %.2f us (sink %llu)\n",
            n,lookups,us(t0,t1),us(t1,t2),static_cast<unsigned long long>(sink&1));
        // timing is informational only (host-load dependent; Astra review of D-350)
    }
    resetState();
}

static void cost_line() {
    namespace P=acz::perf;
    CHECK(P::nested(P::release)&&P::nested(P::list_cert)&&P::nested(P::pso_scan)&&P::nested(P::flush)&&P::nested(P::addr_lookup)&&
          P::nested(P::draw_native)&&!P::nested(P::execute));
    CHECK(std::strcmp(P::cat_name[P::release],"rel")==0&&std::strcmp(P::cat_name[P::list_cert],"cert")==0&&std::strcmp(P::cat_name[P::pso_scan],"pscan")==0);
    P::Report r;r.window_us=5000000;r.tsc_per_us=3000;
    for(unsigned i=0;i<P::count;++i){r.us_total[i]=9.99e6;r.calls[i]=9.99e6;r.max_us[i]=9.99e6;}
    P::gauge(P::registry_resources,999999);P::gauge(P::registry_heaps,99999);P::gauge(P::pipelines,9999999);P::gauge(P::list_states,999999);
    char t[448];
    for(unsigned part=1;part<=2;++part){P::format(t,sizeof t,r,1,part);std::printf("worst-case part %u: %zu chars\n",part,std::strlen(t));CHECK(std::strlen(t)<440);}
    P::Report z;z.window_us=5000000;z.tsc_per_us=3000;for(unsigned i=0;i<P::count;++i){z.us_total[i]=300*1000.0*(i+1);z.calls[i]=300*100.0;z.max_us[i]=12;}
    P::gauge(P::pipelines,2100);P::gauge(P::list_states,40);
    P::format(t,sizeof t,z,300,1);std::printf("typical: %s\n",t);
    CHECK(std::strstr(t,";hooks-us-per-frame=58000;"));   // unchanged: the three new categories are nested, not summed
    P::format(t,sizeof t,z,300,2);std::printf("typical: %s\n",t);
    CHECK(std::strstr(t,";flush=12000/100/12;rel=13000/100/12;cert=14000/100/12;pscan=15000/100/12;objects=pso:2100,lists:40"));
}

// ---- Section 8 (3): CreationSightings with the O(1) index == the original 4096-entry walks (reference copied verbatim)
struct RefSightings {
    std::array<CreationSightings::Entry,CreationSightings::capacity> entries{};
    unsigned occupancy=0,highWater=0;bool overflow=false;
    void observe(uintptr_t address,unsigned seam) {
        if(!address)return;
        for(auto& e:entries)if(e.address==address){e.seams|=seam;return;}
        for(auto& e:entries)if(!e.address){e={address,seam};++occupancy;highWater=(std::max)(highWater,occupancy);return;}
        overflow=true;
    }
    unsigned classify(uintptr_t address) const {
        if(!address)return 2;
        for(const auto& e:entries)if(e.address==address)return 0;
        return overflow?2u:1u;
    }
};
static void sightings(unsigned seed) {
    std::mt19937_64 rng(seed);auto rnd=[&](uint64_t lo,uint64_t hi){return std::uniform_int_distribution<uint64_t>(lo,hi)(rng);};
    auto x=std::make_unique<CreationSightings>();auto ref=std::make_unique<RefSightings>();
    // Clustered heap-like addresses (same low bits, 64 KB strides) plus a small random set, so probes collide and repeat.
    auto addr=[&]{return rnd(0,3)?uintptr_t(0x1d0000000ull+0x10000*rnd(0,6000)):uintptr_t(rnd(0,40));};
    unsigned queries=0;
    for(unsigned step=0;step<60000;++step) {
        const auto a=addr();
        if(rnd(0,2)==0){const unsigned seam=1u<<rnd(0,8);x->observe(a,seam);ref->observe(a,seam);}
        CHECK(x->classify(a)==ref->classify(a));++queries;
        if(step%997==0) {
            CHECK(x->occupancy==ref->occupancy&&x->highWater==ref->highWater&&x->overflow==ref->overflow&&!x->evictions);
            for(size_t i=0;i<CreationSightings::capacity;++i)
                CHECK(x->entries[i].address==ref->entries[i].address&&x->entries[i].seams==ref->entries[i].seams);
            // Whole-struct copy (fixtures' save/restore) keeps the index usable.
            auto copy=std::make_unique<CreationSightings>(*x);CHECK(copy->classify(a)==ref->classify(a)&&copy->indexed());
        }
    }
    CHECK(x->overflow&&x->occupancy==CreationSightings::capacity);   // the run reached the capacity-loss branch
    CHECK(x->mismatches==0&&x->indexed());
    // A direct edit of the next free entry breaks the append-only shape: indexed() turns false and the original walks answer (edits
    // further away are not detected by the shape check; production never edits entries, and test builds compare every answer).
    auto y=std::make_unique<CreationSightings>();
    for(uintptr_t i=1;i<=10;++i)y->observe(i*0x1000,CreationSightings::pso);
    CHECK(y->indexed());
    y->entries[10].address=0x77000;CHECK(!y->indexed()&&y->classify(0x77000)==0&&y->classify(0x5000)==0&&y->classify(0x99000)==1);
    y->observe(0x99000,CreationSightings::root);CHECK(y->entries[11].address==0x99000&&y->occupancy==11);   // first empty entry, as before
    CHECK(y->mismatches==0);
    // Timing (informational): classify of an absent address on a full table, walk vs index.
    uint64_t sink=0;auto t0=std::chrono::steady_clock::now();
    for(unsigned i=0;i<20000;++i)sink+=ref->classify(0x123457+i);
    auto t1=std::chrono::steady_clock::now();
    for(unsigned i=0;i<20000;++i)sink+=x->lookup(0x123457+i)!=nullptr;
    auto t2=std::chrono::steady_clock::now();
    std::printf("sightings seed=%u: %u queries, occupancy %u, classify absent: walk %.3f us, index %.3f us (sink %llu)\n",seed,queries,
        x->occupancy,std::chrono::duration<double,std::micro>(t1-t0).count()/20000,std::chrono::duration<double,std::micro>(t2-t1).count()/20000,
        static_cast<unsigned long long>(sink&1));
}

// ---- Section 8 (2), D-358: NO certification cache at ExecuteCommandLists. Every Execute-seam certification performs the full
// fresh owned query set (11 variants + 3 bases + identity/stable/round-trip QIs + GetDevice) and returns every reference it took.
// Each alias is an independently reference-counted object (a COM tear-off may be), so the rig also checks that every variant/base
// alias held an owned reference while certification used it and that each alias's own count is back to baseline afterwards.
namespace cert {
struct Rig;
struct Object {void** table=nullptr;Rig* rig=nullptr;ULONG refs=0,peak=0;};
using Table=std::array<void*,86>;
static unsigned qis=0,getDevices=0;
static void STDMETHODCALLTYPE unused() {std::fprintf(stderr,"unexpected native slot\n");std::exit(1);}
static HRESULT STDMETHODCALLTYPE query(ID3D12GraphicsCommandList*,REFIID,void**);
static ULONG STDMETHODCALLTYPE addRef(IUnknown*);
static ULONG STDMETHODCALLTYPE release(IUnknown*);
static HRESULT STDMETHODCALLTYPE device(ID3D12GraphicsCommandList*,REFIID,void** out){++getDevices;*out=nullptr;return E_NOINTERFACE;}
static D3D12_COMMAND_LIST_TYPE listType=D3D12_COMMAND_LIST_TYPE_DIRECT;
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE type(ID3D12GraphicsCommandList*){return listType;}
static BOOL WINAPI protect(LPVOID,SIZE_T,DWORD,DWORD* old){*old=PAGE_READWRITE;return TRUE;}
struct Rig {
    std::unique_ptr<std::array<Table,12>> tables=std::make_unique<std::array<Table,12>>(); // [11] = spare table for the swap scenario
    std::array<Object,11> aliases{};
    unsigned canonicalAlias=0; // which alias QI(IUnknown) returns
    ULONG refs=1;
    Rig() {
        acz::g_protect=protect;acz::g_pinResult=true;listType=D3D12_COMMAND_LIST_TYPE_DIRECT;
        for(unsigned i=0;i<12;++i) {
            auto& t=(*tables)[i];t.fill(reinterpret_cast<void*>(&unused));
            t[0]=reinterpret_cast<void*>(&query);t[1]=reinterpret_cast<void*>(&addRef);t[2]=reinterpret_cast<void*>(&release);
            t[7]=reinterpret_cast<void*>(&device);t[8]=reinterpret_cast<void*>(&type);
            if(i<11)aliases[i]={t.data(),this,1,1};   // each alias owned once by "another client"
        }
    }
    ID3D12GraphicsCommandList* list(unsigned i=0){return reinterpret_cast<ID3D12GraphicsCommandList*>(&aliases[i]);}
    void resetPeaks(){for(auto& a:aliases)a.peak=a.refs;}
};
static HRESULT STDMETHODCALLTYPE query(ID3D12GraphicsCommandList* self,REFIID iid,void** out) {
    ++qis;auto* r=reinterpret_cast<Object*>(self)->rig;*out=nullptr;
    if(iid==__uuidof(IUnknown))*out=r->list(r->canonicalAlias);
    if(iid==__uuidof(ID3D12CommandList)||iid==__uuidof(ID3D12DeviceChild)||iid==__uuidof(ID3D12Object))*out=r->list();
    for(unsigned i=0;i<11;++i)if(iid==acz::listIids[i])*out=r->list(i);
    if(!*out)return E_NOINTERFACE;
    auto* o=reinterpret_cast<Object*>(*out);++r->refs;o->peak=(std::max)(o->peak,++o->refs);return S_OK;
}
static ULONG STDMETHODCALLTYPE addRef(IUnknown* self){auto* o=reinterpret_cast<Object*>(self);o->peak=(std::max)(o->peak,++o->refs);return ++o->rig->refs;}
static ULONG STDMETHODCALLTYPE release(IUnknown* self){auto* o=reinterpret_cast<Object*>(self);CHECK(o->refs>0);--o->refs;return --o->rig->refs;}
using Step=std::function<void(Rig&)>;
// Runs `steps` (each followed by one Execute-seam certification) on a fresh rig; returns how many Execute certifications succeeded.
static unsigned run(const std::vector<Step>& steps) {
    Rig r;CHECK(acz::hookCaptureCommandList(r.list()));   // creation seam
    unsigned succeeded=0;
    for(const auto& step:steps) {
        if(step)step(r);
        const ULONG before=r.refs;std::array<ULONG,11> each{};for(unsigned i=0;i<11;++i)each[i]=r.aliases[i].refs;
        r.resetPeaks();const unsigned q0=qis,d0=getDevices;
        const bool ok=acz::hookCaptureCommandList(r.list(),acz::diagnostics::ListSeam::execute);
        CHECK(r.refs==before);   // every reference taken by certification is returned
        for(unsigned i=0;i<11;++i)CHECK(r.aliases[i].refs==each[i]);   // per alias (independently counted tear-off), too
        if(ok) {
            ++succeeded;
            CHECK(qis-q0>=29&&getDevices-d0==1);   // the full fresh query set, every time: nothing reused from an earlier call
            // Each variant alias the hook used was held by an owned reference of its own during certification.
            for(unsigned i=0;i<11;++i)CHECK(r.aliases[i].peak>each[i]);
        }
    }
    {   // No state outlives the rig's lifetime.
        auto& s=acz::state();std::lock_guard lock(s.mutex);
        s.states.erase(r.list());
        for(auto it=s.listKeys.begin();it!=s.listKeys.end();)if(it->second==r.list())it=s.listKeys.erase(it);else ++it;
        for(auto& t:*r.tables)s.lists.erase(t.data());
    }
    return succeeded;
}
} // namespace cert
static void certCacheAbsent() {
    using namespace cert;
    auto check=[](const std::vector<Step>& steps,unsigned expectOk,const char* name) {
        const auto ok=run(steps);
        if(ok!=expectOk){std::fprintf(stderr,"cert scenario %s: %u successful Executes, expected %u\n",name,ok,expectOk);CHECK(false);}
        std::printf("cert scenario %-16s %zu Executes, %u fully re-certified (no cache)\n",name,steps.size(),ok);
    };
    std::vector<Step> plain(12,Step{});
    check(plain,12,"repeat");
    auto released=plain;released[4]=[](Rig& r){auto& s=acz::state();std::lock_guard lock(s.mutex);s.states.erase(r.list());};
    check(released,12,"state-retired");
    auto refused=plain;refused[3]=[](Rig&){listType=D3D12_COMMAND_LIST_TYPE_BUNDLE;};refused[4]=[](Rig&){listType=D3D12_COMMAND_LIST_TYPE_DIRECT;};
    check(refused,11,"refusal");
    auto unrouted=plain;unrouted[6]=[](Rig& r){auto& s=acz::state();std::lock_guard lock(s.mutex);s.listKeys.erase(r.list(3));};
    check(unrouted,12,"alias-unrouted");
    auto swapped=plain;swapped[2]=[](Rig& r){r.aliases[4].table=(*r.tables)[11].data();};
    check(swapped,12,"table-swapped");
    // Tear-off released by its other owner between Executes: certification takes its own fresh reference each time, so the
    // alias's count never depends on a reference held from an earlier call (there is none to depend on).
    auto torn=plain;torn[5]=[](Rig& r){r.aliases[7].refs=0;};
    check(torn,12,"tearoff-released");
    std::printf("cert: no certification cache; every Execute re-queries all interfaces with owned references\n");
}

// ---- Section 8 (4): one write session per flush == the per-call opens, byte for byte (real files in %TEMP%)
static void fileBatch() {
    wchar_t tmp[MAX_PATH]{};CHECK(GetTempPathW(MAX_PATH,tmp)>0);
    const auto dir=std::filesystem::path(tmp)/(L"acz-fps-batch-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    auto read=[](const std::filesystem::path& p) {
        std::string s;FILE* f=nullptr;
        if(!_wfopen_s(&f,p.c_str(),L"rb")&&f){char b[4096];size_t n;while((n=std::fread(b,1,sizeof b,f))>0)s.append(b,n);std::fclose(f);}
        return s;
    };
    std::mt19937_64 rng(7);auto rnd=[&](uint64_t lo,uint64_t hi){return std::uniform_int_distribution<uint64_t>(lo,hi)(rng);};
    using diagnostics::max_record_length;
    // The same operation sequence on two directories: per call (no session) and inside sessions of random length.
    struct Op {int kind;int file;uint64_t slot;char fill;};std::vector<Op> ops;
    for(unsigned i=0;i<400;++i)ops.push_back({int(rnd(0,9)),int(rnd(0,1)),rnd(0,7),char('a'+rnd(0,25))});
    uint64_t opensPlain=0,opensBatch=0,updatePlain=0,updateBatch=0;
    for(int mode=0;mode<2;++mode) {
        diagnostics::FileIO io;const auto base=dir/(mode?L"b":L"a");std::filesystem::create_directories(base);
        const std::filesystem::path files[2]={base/L"evidence.log",base/L"diagnostics.log"};
        for(auto& f:files)CHECK(io.reset(f));
        bool open=false;uint64_t evidenceBytes=0;   // tracked like Log's evidence_bytes_ (overwrites need no size call)
        for(const auto& op:ops) {
            if(mode&&!open){io.begin_batch();open=true;}
            std::string line(max_record_length,op.fill);line.back()='\n';
            uint64_t n=0;
            switch(op.kind) {
                case 0: case 1: case 2: CHECK(io.append(files[op.file],line.data(),line.size())==line.size());
                    if(!op.file)evidenceBytes+=line.size();break;
                case 3: case 4: case 5: {const auto slots=evidenceBytes/max_record_length;
                    const auto r=io.overwrite(files[0],(op.slot%(slots?slots:1))*max_record_length,line.data(),line.size());
                    CHECK(slots?r==line.size():r==0);break;}
                case 6: CHECK(io.size(files[op.file],n));if(!op.file)CHECK(n==evidenceBytes);break;
                case 7: CHECK(io.append(files[op.file],line.data(),7)==7);if(!op.file)evidenceBytes+=7;break;
                default: if(mode&&open){CHECK(io.end_batch());open=false;}break;   // flush boundary
            }
        }
        if(open)CHECK(io.end_batch());
        CHECK(io.opens==io.closes);
        (mode?opensBatch:opensPlain)=io.opens;(mode?updateBatch:updatePlain)=io.updateOpens;
    }
    for(const wchar_t* name:{L"evidence.log",L"diagnostics.log"})
        CHECK(read(dir/L"a"/name)==read(dir/L"b"/name)&&!read(dir/L"a"/name).empty());
    // Never more opens of either mode than the per-call path ("r+b" opens are the expensive ones: ~1.5 ms vs ~0.08 ms for "ab").
    CHECK(opensBatch<opensPlain&&updateBatch<=updatePlain&&opensBatch-updateBatch<=opensPlain-updatePlain);
    std::printf("file batch: identical bytes, opens %llu (r+b %llu) per call vs %llu (r+b %llu) in sessions\n",
        static_cast<unsigned long long>(opensPlain),static_cast<unsigned long long>(updatePlain),
        static_cast<unsigned long long>(opensBatch),static_cast<unsigned long long>(updateBatch));
    // Append-only traffic (the non-production Log: size + append per record) opens only "ab", exactly like the per-call path.
    {diagnostics::FileIO io;const auto p=dir/L"plain.log";CHECK(io.reset(p));io.begin_batch();uint64_t n=0;
     for(unsigned i=0;i<50;++i){CHECK(io.size(p,n)&&n==7u*i);CHECK(io.append(p,"record\n",7)==7);}
     CHECK(io.end_batch()&&io.updateOpens==0&&io.opens==50);}
    // Rotation inside a session closes the held file first (renaming an open file would fail on Windows).
    {diagnostics::FileIO io;const auto p=dir/L"rot.log";CHECK(io.reset(p));io.begin_batch();
     CHECK(io.append(p,"x\n",2)==2&&io.rotate(p,2)&&io.append(p,"y\n",2)==2);CHECK(io.end_batch());
     CHECK(read(p)=="y\n"&&read(dir/L"rot.log.1")=="x\n");CHECK(io.opens==io.closes);}
    // D-358 fault injection: an exception after a session open and before the handle is committed (where the old heldPath_ copy
    // could throw) must close that handle; the call then falls back to the per-call path. Opens and closes stay balanced.
    {static unsigned faults=0;faults=0;
     diagnostics::FileIO io;const auto p=dir/L"fault.log";CHECK(io.reset(p));
     io.fault_after_open=[]{++faults;throw std::bad_alloc();};
     io.begin_batch();
     for(unsigned i=0;i<20;++i){CHECK(io.append(p,"f\n",2)==2);CHECK(io.opens==io.closes);}   // nothing leaked after any call
     uint64_t n=0;CHECK(io.size(p,n)&&n==40);
     CHECK(io.end_batch()&&io.opens==io.closes&&faults==20&&io.opens==40);   // 20 aborted session opens + 20 per-call opens
     io.fault_after_open=nullptr;io.begin_batch();
     for(unsigned i=0;i<5;++i)CHECK(io.append(p,"g\n",2)==2);
     CHECK(io.opens==io.closes+1);   // the session handle is held until end_batch
     CHECK(io.end_batch()&&io.opens==io.closes);
     std::string expect;for(int i=0;i<20;++i)expect+="f\n";for(int i=0;i<5;++i)expect+="g\n";
     CHECK(read(p)==expect);}
    std::error_code ec;std::filesystem::remove_all(dir,ec);
}

int main() {
    differential(1,300);differential(2,800);differential(3,40);
    staleness();
    timing();
    sightings(11);sightings(12);
    certCacheAbsent();
    fileBatch();
    cost_line();
    std::printf("fps_growth_offline: %u checks passed\n",checks);
    return 0;
}
